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

REXCVAR_DEFINE_DOUBLE(bf_fov, 0.0, "BF1943",
                      "The camera's horizontal field of view at 16:9 in degrees (60 to 120); 0: the game's own, 85.6");

namespace bf1943 {
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

double Mapped(double radians) {
  const double chosen = REXCVAR_GET(bf_fov);
  if (chosen <= 0.0 || !(radians > 0.0 && radians < kPi)) return radians;
  const double h = std::clamp(chosen, 60.0, 120.0) * kPi / 180.0;
  const double tan_v = std::tan(h * 0.5) * 9.0 / 16.0;  // the vertical half angle's tangent at 16:9
  const double k = tan_v / std::tan(27.5 * kPi / 180.0);
  return 2.0 * std::atan(k * std::tan(radians * 0.5));
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

REX_EXTERN(__imp__sub_82623DB8);
REX_HOOK_RAW(sub_82623DB8) {
  if (uint32_t(ctx.lr) == kSecondViewCaller) ctx.f1.f64 = Mapped(ctx.f1.f64);
  __imp__sub_82623DB8(ctx, base);
}
