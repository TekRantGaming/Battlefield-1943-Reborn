// Vehicle cameras above 30 FPS (TekRantGaming, 7 Nov 2026: the car, boat, tank and plane cameras "shake like crazy" above
// 30 FPS and feel 30 FPS).
//
// Measured in a Sherman on Guadalcanal (runs view4, tick4): the camera the game renders through comes from its camera
// manager (the singleton at 0x82AF3BA0), whose per frame update sub_82294A58 writes the camera transform (+0x50, +0x90)
// and hands the renderer its view. In a vehicle that transform takes big steps in the game's 30 Hz tick (the vehicle
// moves only there) and a small change on every frame in between (0.0015 against steps of 0.02 to 0.15 in the view
// matrix): a camera smoothing that runs with the frame's time and chases a target that moves 30 times a second, so
// above 30 FPS it catches up and stops between the steps. At 30 FPS the two run together, as on the Xbox.
//
// bf_vehicle_camera_tick: in a vehicle, the camera manager's camera moves as at 30 FPS, with the vehicle in the tick.
// Without bf_smooth_motion the manager runs only on the frames a tick ran (the view stays put between). With it, the
// manager runs on every frame but its camera is given time only on tick frames, and tick_blend.cpp hands the renderer
// that camera blended between its last two ticks, together with the vehicle. On foot nothing changes (the view turns
// and walks every frame there: look.cpp).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(bf_vehicle_camera_tick, true, "BF1943",
                    "In a vehicle, the camera follows the game's 30 Hz tick, as at 30 FPS (the renderer draws the frames "
                    "between): no camera shake above 30 FPS");
REXCVAR_DECLARE(bool, bf_unlock_fps);  // frame_rate.cpp
REXCVAR_DECLARE(bool, bf_smooth_motion);  // tick_blend.cpp

namespace bf1943 {
bool InMatch();       // auto_join.cpp
bool LocalOnFoot();   // look.cpp
uint32_t LocalSoldierEntity();
bool LocalSeated(const uint8_t* base);  // look.cpp: in a vehicle, any seat
void TickBlendTick();                   // tick_blend.cpp
void TickBlendCameraRestore(uint8_t* base);
void TickBlendCameraPublish(uint8_t* base, uint32_t manager);
void TickBlendBeforeTick(uint8_t* base);
void TickBlendAfterTick(uint8_t* base);
void TickBlendPublishTrees(uint8_t* base);
void TickBlendTreesForManager(uint8_t* base);
bool TickBlendViewWithoutManager(PPCContext& ctx, uint8_t* base);
void TickBlendViewForManager(PPCContext& ctx, uint8_t* base);
void TickBlendCameraForManager(uint8_t* base, uint32_t manager);
}  // namespace bf1943

namespace {
std::atomic<uint64_t> g_ticks{0};  // client ticks run
uint64_t g_camera_ticks = ~0ull;   // the tick count when the camera manager last ran
uint64_t g_skipped = 0, g_ran = 0;
}  // namespace

namespace bf1943 {
// The client ticks run so far (effects.cpp: the particles step with the tick).
uint64_t ClientTicks() { return g_ticks.load(std::memory_order_relaxed); }
std::atomic<bool> g_in_client_tick{false};  // the client's tick is running (tick_blend.cpp's BF_DEV_TREE)
}  // namespace bf1943

// The client's tick (sub_82380600(client, f1 = the tick's time, ...)), once per game tick (sub_823961E8's loop).
REX_EXTERN(__imp__sub_82380600);
REX_HOOK_RAW(sub_82380600) {
  bf1943::TickBlendBeforeTick(base);
  bf1943::g_in_client_tick.store(true, std::memory_order_relaxed);
  __imp__sub_82380600(ctx, base);
  bf1943::g_in_client_tick.store(false, std::memory_order_relaxed);
  g_ticks.fetch_add(1, std::memory_order_relaxed);
  {  // The client's ticks a second, every 30 s (tick_rate.cpp: 30 or 60), to compare with the server's own line.
    static auto since = std::chrono::steady_clock::now();
    static uint64_t from = 0;
    const auto now = std::chrono::steady_clock::now();
    const double s = std::chrono::duration<double>(now - since).count();
    if (s >= 30.0) {
      const uint64_t t = g_ticks.load(std::memory_order_relaxed);
      REXLOG_INFO("BF client ticks: {:.1f} a second over {:.0f} s (tick {:.4f} s)", double(t - from) / s, s,
                  ctx.f1.f64);
      since = now, from = t;
    }
  }
  bf1943::TickBlendTick();
  bf1943::TickBlendAfterTick(base);
}

namespace {
// While the camera manager runs for a vehicle with bf_smooth_motion (tick_blend.cpp blends the view it hands over).
std::atomic<bool> g_vehicle_camera_frame{false};
double g_camera_dt = 0;  // the frame times given since the camera's last tick frame
}  // namespace

namespace bf1943 {
bool VehicleCameraFrame() { return g_vehicle_camera_frame.load(std::memory_order_relaxed); }
}  // namespace bf1943

// The camera manager's frame (sub_82294A58(manager, f1)).
REX_EXTERN(__imp__sub_82294A58);
#if defined(BF_DEV_TOOLS)
namespace bf1943 {
void DevMarkCamera();  // tick_blend.cpp: BF_DEV_TIMELINE
}
#endif
namespace {
void CameraManagerFrame(PPCContext& ctx, uint8_t* base);
}  // namespace

// The camera manager builds the camera from the vehicle's parts: the game's own values in the trees while it runs, ours
// again after it (tick_blend.cpp).
REX_HOOK_RAW(sub_82294A58) {
#if defined(BF_DEV_TOOLS)
  bf1943::DevMarkCamera();
#endif
  CameraManagerFrame(ctx, base);
  bf1943::TickBlendPublishTrees(base);
}

namespace {
void CameraManagerFrame(PPCContext& ctx, uint8_t* base) {
  const uint64_t ticks = g_ticks.load(std::memory_order_relaxed);
  bf1943::TickBlendCameraForManager(base, ctx.r3.u32);  // which manager it is (tick_blend.cpp's dev searches)
  // In a vehicle: the soldier's on foot camera stopped (a driver's seat, a tank's gun), or it runs in a seat (a boat's
  // or a jeep's gunner and passenger seats use the soldier's own camera: run seat3).
  const bool vehicle = REXCVAR_GET(bf_vehicle_camera_tick) && REXCVAR_GET(bf_unlock_fps) && bf1943::InMatch() &&
                       (!bf1943::LocalOnFoot() || bf1943::LocalSeated(base));
  // With smooth motion the manager runs on every frame (the view is handed over every frame), but the camera's own
  // update and smoothing move only on the frames a tick ran, by all the time since the last such frame, and by none
  // between: the camera the game makes is the one it makes at 30 FPS, and tick_blend.cpp hands the renderer that camera
  // blended between its last two ticks, as it does the vehicle (runs mot1, mot2).
  if (vehicle && REXCVAR_GET(bf_smooth_motion)) {
    g_camera_dt += ctx.f1.f64;
    // No tick since the manager's last frame: it does not run (as at 30 FPS); the view gets this frame's camera.
    // The manager runs on every frame (it places the cockpit around the view on each: the Corsair's canopy, run
    // vplaneevery; held to the ticks, the canopy came and went, vplanefinal). BF_DEV_MGR_TICKONLY=1: only on tick
    // frames, the view handed our camera between (to compare).
#if defined(BF_DEV_TOOLS)
    static const bool tick_only = std::getenv("BF_DEV_MGR_TICKONLY") != nullptr;
#else
    constexpr bool tick_only = false;
#endif
    if (ticks == g_camera_ticks && tick_only && bf1943::TickBlendViewWithoutManager(ctx, base)) {
      ++g_skipped;
      return;
    }
    if (ticks != g_camera_ticks) {
      ctx.f1.f64 = std::min(g_camera_dt, 0.1);
      g_camera_dt = 0;
    } else {
      ctx.f1.f64 = 0.0;
    }
    g_camera_ticks = ticks;
    ++g_ran;
    const uint32_t manager = ctx.r3.u32;
    bf1943::TickBlendTreesForManager(base);
    bf1943::TickBlendCameraRestore(base);
    bf1943::TickBlendViewForManager(ctx, base);
    g_vehicle_camera_frame.store(true, std::memory_order_relaxed);
    __imp__sub_82294A58(ctx, base);
    g_vehicle_camera_frame.store(false, std::memory_order_relaxed);
    bf1943::TickBlendCameraPublish(base, manager);
    return;
  }
  bf1943::TickBlendCameraRestore(base);
  g_camera_dt = 0;
  if (vehicle && ticks == g_camera_ticks) {
    ++g_skipped;
    return;
  }
  g_camera_ticks = ticks;
  ++g_ran;
#if defined(BF_DEV_TOOLS)
  // BF_DEV_VCAM=1: once a second, on foot or not, and the camera frames run and held (which seats are held).
  {
    static const bool dev = std::getenv("BF_DEV_VCAM") != nullptr;
    static auto at = std::chrono::steady_clock::now();
    static uint64_t ran0 = 0, skipped0 = 0;
    if (dev && std::chrono::steady_clock::now() - at > std::chrono::seconds(1)) {
      at = std::chrono::steady_clock::now();
      const uint32_t e = bf1943::LocalSoldierEntity();
      REXLOG_INFO("BF vcam: on foot {} seated {} in match {}: {} run, {} held; entity {:08X} +1016 {:02X}",
                  bf1943::LocalOnFoot(), bf1943::LocalSeated(base), bf1943::InMatch(), g_ran - ran0,
                  g_skipped - skipped0, e, e ? base[e + 1016] : 0);
      ran0 = g_ran, skipped0 = g_skipped;
    }
  }
#endif
  if ((g_ran + g_skipped) % 3000 == 0)
    REXLOG_INFO("BF vehicle camera: {} camera frames run, {} held to the tick", g_ran, g_skipped);
  bf1943::TickBlendTreesForManager(base);
  __imp__sub_82294A58(ctx, base);
}
}  // namespace
