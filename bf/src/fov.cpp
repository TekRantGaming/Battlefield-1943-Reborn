// Field of view (Settings > Display > Camera > Field of view, bf_fov): the camera's horizontal field of view at 16:9.
//
// The game's camera manager (the singleton at 0x82AF3BA0) keeps the current vertical field of view in degrees and
// blends it toward each request every frame (a camera change, or a weapon's zoom: aim down sights narrows it); then
// sub_82294A58 turns it into radians and hands it to the view's setFovY, sub_82583248(view, f1 = radians), called from
// 0x82294CAC, which writes the view's +0x0C and +0x10 for the projection. A second view (sub_82623DB8, called from
// 0x822948E4 in sub_82294818) takes the same value. The game's default is 55 degrees vertical, 85.6 horizontal at
// 16:9 (the voted projection, NXR D132: 1/tan(v/2) = 1.9210). Static study: side-session note fov_study.md.
//
// The value handed over is mapped in tangent space, out = 2 atan(k tan(in / 2)) with k = tan(chosen / 2) /
// tan(27.5 degrees): unzoomed it becomes exactly the chosen field of view, and a zoom keeps its own ratio, so aiming
// down sights still narrows as much. The game's own state is never written, so nothing compounds, and the server and
// other players are not affected (the camera manager is the client's). The first person weapon has a projection of its
// own (RenderFov, 30 degrees), which this leaves alone.

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>

#include <native_x_renderer/native_x_renderer.h>

#include "comms.h"

REXCVAR_DEFINE_DOUBLE(bf_fov, 0.0, "BF1943",
                      "The camera's horizontal field of view at 16:9 in degrees (60 to 120); 0: the game's own, 85.6");
// Vehicles' own slider (TekRantGaming, 12 Nov 2026): the camera while the local soldier sits in a vehicle.
REXCVAR_DEFINE_DOUBLE(bf_fov_vehicle, 0.0, "BF1943",
                      "The camera's horizontal field of view in vehicles, at 16:9 in degrees (60 to 120); 0: the game's own");

namespace bf1943 {
bool LocalSeatedNow();  // hud_glyphs.cpp: the local soldier sits in a vehicle (from the pad hook)
// The game's own vertical field of view this frame (radians, before our mapping), zoom included: look.cpp scales the
// mouse by how far a zoom narrows it.
std::atomic<float> g_game_fov{0.0f};
// The camera manager's field of view controller this frame (degrees): +0x30 the view's now (a sight's zoom and the
// sprint's widening included), +0x28 the camera's own (its request C). Read in the frame update that sets the view
// (sub_82294A58(mgr), whose r31 holds the manager when it calls setFovY; the controller is *(mgr + 0xE0)): look.cpp's
// zoom is the view narrower than the camera's own.
std::atomic<float> g_fov_now_deg{0.0f}, g_fov_own_deg{0.0f};
}  // namespace bf1943

namespace {
uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
float LoadF(const uint8_t* base, uint32_t a) {
  const uint32_t u = Load32(base, a);
  float f;
  std::memcpy(&f, &u, sizeof(f));
  return f;
}
constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kCameraFovCaller = 0x82294CB0;  // sub_82294A58's call of setFovY
constexpr uint32_t kSecondViewCaller = 0x822948E4;  // sub_82294818's call of sub_82623DB8 (at 0x822948E0)

// The slider that applies now: the vehicle's while the soldier sits in one, else the camera's.
double Chosen() { return bf1943::LocalSeatedNow() ? REXCVAR_GET(bf_fov_vehicle) : REXCVAR_GET(bf_fov); }

double Mapped(double radians) {
  const double chosen = Chosen();
  if (chosen <= 0.0 || !(radians > 0.0 && radians < kPi)) return radians;
  const double h = std::clamp(chosen, 60.0, 120.0) * kPi / 180.0;
  const double tan_v = std::tan(h * 0.5) * 9.0 / 16.0;  // the vertical half angle's tangent at 16:9
  const double k = tan_v / std::tan(27.5 * kPi / 180.0);
  return 2.0 * std::atan(k * std::tan(radians * 0.5));
}
// The same mapping for a value in degrees (the first person weapon's own projection).
double MappedDegrees(double degrees) { return Mapped(degrees * kPi / 180.0) * 180.0 / kPi; }
}  // namespace

REX_EXTERN(__imp__sub_82583248);
#if defined(BF_DEV_TOOLS)
namespace bf1943 {
extern std::atomic<uint32_t> g_dev_view;  // dev_matscan.cpp: the rendered view, for BF_DEV_VIEWLOG
extern std::atomic<uint32_t> g_dev_view_calls;
void DevViewSample(uint8_t* base, uint32_t view);
}
#endif
REX_HOOK_RAW(sub_82583248) {
#if defined(BF_DEV_TOOLS)
  if (uint32_t(ctx.lr) == kCameraFovCaller) {
    bf1943::g_dev_view.store(ctx.r3.u32, std::memory_order_relaxed);
    bf1943::g_dev_view_calls.fetch_add(1, std::memory_order_relaxed);
    bf1943::DevViewSample(base, ctx.r3.u32);
  }
#endif
  if (uint32_t(ctx.lr) == kCameraFovCaller) {
    // The rendered camera (its world matrix at +0x40: right, up, back, position rows): voice chat's listener and the
    // position a voice comes from (comms.cpp), on foot, in a vehicle or watching after a death alike.
    if (const uint32_t view = ctx.r3.u32) {
      const float right[3] = {LoadF(base, view + 0x40), LoadF(base, view + 0x44), LoadF(base, view + 0x48)};
      const float back[3] = {LoadF(base, view + 0x60), LoadF(base, view + 0x64), LoadF(base, view + 0x68)};
      const float pos[3] = {LoadF(base, view + 0x70), LoadF(base, view + 0x74), LoadF(base, view + 0x78)};
      bf1943::comms::NoteCamera(pos, right, back);
    }
    bf1943::g_game_fov.store(float(ctx.f1.f64), std::memory_order_relaxed);
    const uint32_t manager = ctx.r31.u32;
    const uint32_t controller = manager ? Load32(base, manager + 0xE0) : 0;
    if (controller) {
      bf1943::g_fov_now_deg.store(LoadF(base, controller + 0x30), std::memory_order_relaxed);
      bf1943::g_fov_own_deg.store(LoadF(base, controller + 0x28), std::memory_order_relaxed);
    }
    ctx.f1.f64 = Mapped(ctx.f1.f64);
  }
  __imp__sub_82583248(ctx, base);
}

// Ultrawide (0.4.0, NativeXRenderer D43: Profile::widens_cameras): a 16:9 view's aspect ratio widened by
// AspectFactor() (the screen's shape over 16:9), so the 3D view fills the screen with more on each side (Hor+): the
// vertical field of view, bf_fov's mapping and a zoom's ratio stay as they were. Only views of the game's 16:9 (its main
// and second camera, aspect 1280/720 from sub_823CD748); square views (cube and shadow maps, 1.0) and other shapes are
// left alone. 1 on a 16:9 screen or with native_x_renderer_fill_screen off.
namespace {
double Widened(double aspect) {
  const double factor = native_x_renderer::AspectFactor();
  if (factor == 1.0 || !(std::fabs(aspect - 16.0 / 9.0) < 0.02)) return aspect;
  return aspect * factor;
}
}  // namespace

// setAspect, sub_825832B8(view, f1 = aspect): writes the view's +0x20 and its horizontal estimate +0x10 (+0x0C times
// the aspect), so the game's culling sees the wider view too.
REX_EXTERN(__imp__sub_825832B8);
REX_HOOK_RAW(sub_825832B8) {
  ctx.f1.f64 = Widened(ctx.f1.f64);
  __imp__sub_825832B8(ctx, base);
}

// The perspective projection, sub_82623DB8(out, f1 = vertical field of view, f2 = aspect, f3 near, f4 far): a view whose
// aspect was set before the renderer knew the screen (still 16:9) is widened here as well.
REX_EXTERN(__imp__sub_82623DB8);
REX_HOOK_RAW(sub_82623DB8) {
  if (uint32_t(ctx.lr) == kSecondViewCaller) ctx.f1.f64 = Mapped(ctx.f1.f64);
  ctx.f2.f64 = Widened(ctx.f2.f64);
  __imp__sub_82623DB8(ctx, base);
}

// HUD world markers on an ultrawide screen (0.4.0): the game projects capture flags, name tags and objective icons
// to its 1280x720 HUD in a few functions (research 12 Nov 2026, side note): sub_8241B770(r4 position, r5 world
// matrix, r6 view-projection, r7 out float2 pixels; r3 = in front), sub_8241D038 (the same with a margin test, out in
// r7) and sub_8241B598(r3 out, r5 position, r6 view, r7 projection). Each point is noted for NativeXRenderer, which then
// places the HUD draw on it where the widened 3D view shows that point (HudMode::kPoint), not in the 16:9 box.
namespace {
void NoteMarker(const uint8_t* base, uint32_t out) {
  if (!out || native_x_renderer::AspectFactor() == 1.0) return;
  native_x_renderer::NoteHudMarker(LoadF(base, out), LoadF(base, out + 4));
}
}  // namespace

REX_EXTERN(__imp__sub_8241B770);
REX_HOOK_RAW(sub_8241B770) {
  const uint32_t out = ctx.r7.u32;
  __imp__sub_8241B770(ctx, base);
  NoteMarker(base, out);
}

REX_EXTERN(__imp__sub_8241D038);
REX_HOOK_RAW(sub_8241D038) {
  const uint32_t out = ctx.r7.u32;
  __imp__sub_8241D038(ctx, base);
  NoteMarker(base, out);
}

REX_EXTERN(__imp__sub_8241B598);
REX_HOOK_RAW(sub_8241B598) {
  const uint32_t out = ctx.r3.u32;
  __imp__sub_8241B598(ctx, base);
  NoteMarker(base, out);
}

// The first person weapon's own projection (TekRantGaming, 12 Nov 2026: "the FOV slider doesn't take into
// consideration the first-person weapon ... those stay the same"): sub_821FAAF0(soldier) returns the weapon's render
// field of view in degrees (SoldierWeaponData.RenderFov, 30, blended toward ZoomRenderFov while aiming; the study's
// fov_study.md), or -1 when not in first person; its consumers write it into the player's render record and the
// vehicle seat's view. Mapped the same way as the world's camera after the original, so the hands and the weapon
// widen with it and keep their place in the picture.
REX_EXTERN(__imp__sub_821FAAF0);
REX_HOOK_RAW(sub_821FAAF0) {
  __imp__sub_821FAAF0(ctx, base);
  if (ctx.f1.f64 > 0.0 && ctx.f1.f64 < 180.0) ctx.f1.f64 = MappedDegrees(ctx.f1.f64);
}
