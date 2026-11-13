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
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <native_x_renderer/native_x_renderer.h>

#include "comms.h"
#include "scope_pip.h"
#include "seat_info.h"

REXCVAR_DEFINE_BOOL(bf_scope_pip, false, "BF1943",
                    "Sniper scope (Settings > Enhancements, experimental): picture in picture, only the scope's lens "
                    "zooms in and the world around it stays in view (NativeXRenderer D168)");
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
// The game's last answer to "the first person weapon's field of view" (sub_821FAAF0: -1 when the view is not a first
// person one) and when: 1 first person, 0 not, -1 not asked within a quarter second.
std::atomic<int> g_first_person{0};
std::atomic<int64_t> g_first_person_at{0};
int FirstPersonView() {
  const int64_t at = g_first_person_at.load(std::memory_order_relaxed);
  const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
  if (!at || now - at > std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(250)).count())
    return -1;
  return g_first_person.load(std::memory_order_relaxed);
}
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

// `weight`: how much of the mapping applies (1 all of it, 0 none: the value as the game has it).
double Mapped(double radians, double weight = 1.0) {
  const double chosen = Chosen();
  if (chosen <= 0.0 || !(radians > 0.0 && radians < kPi)) return radians;
  const double h = std::clamp(chosen, 60.0, 120.0) * kPi / 180.0;
  const double tan_v = std::tan(h * 0.5) * 9.0 / 16.0;  // the vertical half angle's tangent at 16:9
  const double k = 1.0 + (tan_v / std::tan(27.5 * kPi / 180.0) - 1.0) * std::clamp(weight, 0.0, 1.0);
  return 2.0 * std::atan(k * std::tan(radians * 0.5));
}
// The same mapping for a value in degrees (the first person weapon's own projection).
double MappedDegrees(double degrees, double weight) { return Mapped(degrees * kPi / 180.0, weight) * 180.0 / kPi; }
// How much of the mapping the first person weapon's projection takes: all of it unzoomed, none through a scope. A
// scope's black mask is part of the weapon's picture, and widened with the camera it shrank on the screen (13 Nov
// 2026: "player FOV should not affect the sniper's scope mesh. Right now at high FOV, it shrinks the scope mesh").
// The zoom is the view's field of view now against the camera's own, in tangents (1 unzoomed): the mapping fades out
// from 0.9 (the start of aiming) to 0.5 (a scope is well under that), so iron sights keep part of it.
double WeaponMapWeight() {
  const double now = bf1943::g_fov_now_deg.load(std::memory_order_relaxed);
  const double own = bf1943::g_fov_own_deg.load(std::memory_order_relaxed);
  if (!(now > 1.0 && own > 1.0 && now < 179.0 && own < 179.0)) return 1.0;
  const double zoom = std::tan(now * kPi / 360.0) / std::tan(own * kPi / 360.0);
  return std::clamp((zoom - 0.5) / (0.9 - 0.5), 0.0, 1.0);
}
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
    float right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, back[3] = {0, 0, 1};
    if (const uint32_t view = ctx.r3.u32) {
      for (int i = 0; i < 3; ++i)
        right[i] = LoadF(base, view + 0x40 + 4 * i), up[i] = LoadF(base, view + 0x50 + 4 * i),
        back[i] = LoadF(base, view + 0x60 + 4 * i);
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
    // The picture in picture scope (Enhancements, NativeXRenderer D168): how far the view is zoomed (the half angles'
    // tangents, now over the camera's own: a sight's about 0.6 to 0.8, a sniper scope's much less; the renderer also
    // waits for the game's scope mask). The world around the lens is drawn with the camera's own field of view as
    // rendered (bf_fov's mapping included).
    {
      constexpr double kPi = 3.14159265358979323846;
      const double now_deg = bf1943::g_fov_now_deg.load(std::memory_order_relaxed);
      const double own_deg = bf1943::g_fov_own_deg.load(std::memory_order_relaxed);
      const bool valid = now_deg > 1.0 && now_deg < 179.0 && own_deg > 1.0 && own_deg < 179.0;
      const double zoom = valid ? std::tan(now_deg * kPi / 360.0) / std::tan(own_deg * kPi / 360.0) : 1.0;
      const double own_rendered = valid ? Mapped(own_deg * kPi / 180.0) : 0.9;
      // A step of the zoom logged (a tenth of the way), while the scope is on: the research of a sight's and a scope's.
      static int logged_step = 10;
      const int step = int(std::lround(std::clamp(zoom, 0.0, 1.0) * 10));
      // Only in a build with the scope (scope_pip.h: not in 0.4.1), with the switch on.
      const bool pip = kScopePip && REXCVAR_GET(bf_scope_pip);
      if (step != logged_step && pip) {
        logged_step = step;
        REXLOG_INFO("BF scope: zoom {:.2f} (field of view {:.1f} of the camera's {:.1f} degrees)", zoom, now_deg, own_deg);
      }
      // On foot only (the soldier's seat, from the game's own input mapping): the game's composite, which the renderer
      // follows, is the same with and without a scope (run v41scope26), so a vehicle's or the air raid bomber's zoomed
      // view must never count as a scope's.
      bf1943::SeatInfo seat;
      const bool on_foot = pip && bf1943::LocalSeat(seat) && seat.kind == bf1943::SeatKind::kSoldier;
      native_x_renderer::NoteScopeView(on_foot, right, up, back,
                                       float(std::tan(std::clamp(own_rendered, 0.1, 3.0) * 0.5)), float(zoom));
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
// widen with it and keep their place in the picture; not through a scope (WeaponMapWeight).
REX_EXTERN(__imp__sub_821FAAF0);
REX_HOOK_RAW(sub_821FAAF0) {
  __imp__sub_821FAAF0(ctx, base);
  // Its answer also says whether the view is a first person one (tick_blend.cpp's free look: a first person view
  // turns in place, an outside one goes round the vehicle).
  bf1943::g_first_person.store(ctx.f1.f64 > 0.0 ? 1 : 0, std::memory_order_relaxed);
  bf1943::g_first_person_at.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
  if (ctx.f1.f64 > 0.0 && ctx.f1.f64 < 180.0) ctx.f1.f64 = MappedDegrees(ctx.f1.f64, WeaponMapWeight());
}
