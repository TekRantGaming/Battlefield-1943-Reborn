// The menu's camera and sound rows that need the game's own code (static study: side-session note
// shake_blur_audio_study.md):
//   - Camera shake (bf_camera_shake, bf_camera_shake_strength): explosion and bullet shakes. Each camera shaker
//     (FPSCameraData's CameraImpulseShakeData: yaw, roll and pitch curves at shaker+0x50, +0x54, +0x58) is turned into
//     a rotation each frame by sub_82287B40 (its only caller returns to 0x8228BF7C) from each curve's current value at
//     curve+8. The values are scaled for that call and put back, so nothing compounds and the curves run as authored.
//     Recoil, the camera bob and the server are untouched.
//   - Motion blur (bf_motion_blur, the row "Motion blur"): every blur the game has. The screen blur from explosions,
//     bullets and low health (the first person camera's blur getter sub_82282A80, read for the post process from
//     0x8228C0C0 in sub_8228C070: its answer is 0), the camera manager's own blur (+0x100, 0 for that call) and the
//     PsMotionBlur pass (skipped). In a calm match none of them ran (run seat2: no pass, every blur value 0).
//   - Music and voices (bf_music_volume, bf_voices_volume): a playing sound's gain is its category's volume,
//     sub_826F2040(sound, category) from the per sound update (0x8270CCBC), times its own. Categories: 0 effects, 1
//     music, 2 dialogue (voices), 3 the car radio (with the music). Run opt_vol0: in the match, music and the ambient
//     sound went silent at 0; gunfire did not (it never runs that update), so bf_effects_volume scales only what does
//     and is not in the menu yet. A change sets the sound system's dirty byte (*(0x82CC2204) + 0x201) so playing sounds
//     follow at once.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(bf_camera_shake, true, "BF1943", "The camera shaking from explosions and gunfire");
REXCVAR_DEFINE_DOUBLE(bf_camera_shake_strength, 100.0, "BF1943", "How hard the camera shakes, 0 to 200 (%)");
REXCVAR_DEFINE_BOOL(bf_motion_blur, true, "BF1943",
                    "The game's screen blur: from explosions, bullets and low health, its camera's blur and its motion "
                    "blur pass");
REXCVAR_DEFINE_DOUBLE(bf_music_volume, 100.0, "BF1943/Audio", "How loud the game's music is, 0 to 100 (%)");
REXCVAR_DEFINE_DOUBLE(bf_effects_volume, 100.0, "BF1943/Audio", "How loud the game's sound effects are, 0 to 100 (%)");
REXCVAR_DEFINE_DOUBLE(bf_voices_volume, 100.0, "BF1943/Audio", "How loud the soldiers' voices are, 0 to 100 (%)");

namespace {
uint32_t Load32(const uint8_t* base, uint32_t a) {
  return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
}
float LoadF(const uint8_t* base, uint32_t a) {
  const uint32_t u = Load32(base, a);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
void StoreF(uint8_t* base, uint32_t a, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  base[a] = uint8_t(u >> 24), base[a + 1] = uint8_t(u >> 16), base[a + 2] = uint8_t(u >> 8), base[a + 3] = uint8_t(u);
}
double Percent(double v) { return std::clamp(v, 0.0, 200.0) / 100.0; }
double ShakeScale() {
  return REXCVAR_GET(bf_camera_shake) ? std::clamp(REXCVAR_GET(bf_camera_shake_strength), 0.0, 200.0) / 100.0 : 0.0;
}
constexpr uint32_t kShakeCaller = 0x8228BF7C;
constexpr uint32_t kBlurCaller = 0x8228C0C0;
constexpr uint32_t kSoundUpdateCaller = 0x8270CCBC;
constexpr uint32_t kSoundSystem = 0x82CC2204;
#if defined(BF_DEV_TOOLS)
// BF_DEV_OPTIONS=1: how often each hook runs from its caller, every 5 s (are they live?).
std::atomic<uint32_t> g_shakes{0}, g_blurs{0}, g_sounds{0}, g_motion_passes{0};
std::atomic<float> g_blur_max{0}, g_manager_blur_max{0}, g_motion_max{0};
void Max(std::atomic<float>& a, float v) {
  float o = a.load();
  while (v > o && !a.compare_exchange_weak(o, v)) {}
}
#endif
}  // namespace

REX_EXTERN(__imp__sub_82287B40);
REX_HOOK_RAW(sub_82287B40) {
  const double k = ShakeScale();
#if defined(BF_DEV_TOOLS)
  if (uint32_t(ctx.lr) == kShakeCaller) ++g_shakes;
#endif
  if (uint32_t(ctx.lr) != kShakeCaller || k == 1.0) return __imp__sub_82287B40(ctx, base);
  const uint32_t shaker = ctx.r3.u32;
  uint32_t curve[3] = {};
  float saved[3] = {};
  for (int i = 0; i < 3; ++i) {
    curve[i] = shaker ? Load32(base, shaker + 0x50 + 4 * i) : 0;
    if (curve[i]) saved[i] = LoadF(base, curve[i] + 8), StoreF(base, curve[i] + 8, float(saved[i] * k));
  }
  __imp__sub_82287B40(ctx, base);
  for (int i = 0; i < 3; ++i) {
    if (curve[i]) StoreF(base, curve[i] + 8, saved[i]);
  }
}

REX_EXTERN(__imp__sub_82282A80);
REX_HOOK_RAW(sub_82282A80) {
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_82282A80(ctx, base);
#if defined(BF_DEV_TOOLS)
  if (lr == kBlurCaller) ++g_blurs, Max(g_blur_max, float(ctx.f1.f64));
#endif
  if (lr == kBlurCaller && !REXCVAR_GET(bf_motion_blur)) ctx.f1.f64 = 0.0;
}

// The game's PsMotionBlur pass (sub_825A38D8(this, f1 = amount, ...)): the study found no caller, and none ran in a
// match (run seat2); with the setting off it is skipped all the same.
REX_EXTERN(__imp__sub_825A38D8);
REX_HOOK_RAW(sub_825A38D8) {
#if defined(BF_DEV_TOOLS)
  ++g_motion_passes, Max(g_motion_max, float(ctx.f1.f64));
#endif
  // 0.4.2: never run (the renderer's motion blur takes its place, native_x_renderer_motion_blur).
  (void)base;
}
// The view effects for the renderer (sub_8228C070(manager, camera)): the manager's own blur (+0x100) wins over the
// camera's when above 0; with the setting off it is 0 for that call and put back after.
REX_EXTERN(__imp__sub_8228C070);
REX_HOOK_RAW(sub_8228C070) {
  const uint32_t manager = ctx.r3.u32;
#if defined(BF_DEV_TOOLS)
  if (manager) Max(g_manager_blur_max, LoadF(base, manager + 0x100));
#endif
  // 0.4.2 ("remove 1943's current implementation of motion blur"): the camera manager's own blur is always 0 for
  // this call; the renderer's motion blur takes its place. bf_motion_blur is now only the screen blur from
  // explosions, bullets and low health (sub_82282A80 above).
  const bool off = manager != 0;
  const float saved = off ? LoadF(base, manager + 0x100) : 0.0f;
  if (off) StoreF(base, manager + 0x100, 0.0f);
  __imp__sub_8228C070(ctx, base);
  if (off) StoreF(base, manager + 0x100, saved);
}

REX_EXTERN(__imp__sub_826F2040);
REX_HOOK_RAW(sub_826F2040) {
  const uint32_t lr = uint32_t(ctx.lr), category = ctx.r4.u32;
  __imp__sub_826F2040(ctx, base);
#if defined(BF_DEV_TOOLS)
  {
    static std::atomic<int> logged{0};
    if (std::getenv("BF_DEV_OPTIONS") && logged < 40 && logged.fetch_add(1) < 40)
      REXLOG_INFO("BF options: category volume {} -> {:.3f} for lr {:08X}", category, ctx.f1.f64, lr);
  }
#endif
  // At the per sound update, which sets each voice's gain (category volume times the sound's own). One shots such as
  // gunfire take another path: scaled at play start (0x82708624) nothing changed (run opt_vol0b), so the effects
  // category is not offered yet.
  if (lr != kSoundUpdateCaller) return;
#if defined(BF_DEV_TOOLS)
  ++g_sounds;
#endif
  // The game's own music (the launcher's menu music: BF's, playing behind our pages, docs/LAUNCHER-PLAN.md 2.4):
  // logged when it starts, after 10 s without any.
  if (category == 1) {
    static std::atomic<int64_t> last{0};
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - last.exchange(now) > 10) REXLOG_INFO("BF audio: the game's music is playing (gain {:.2f})", ctx.f1.f64);
  }
  const double scale = category == 0                   ? Percent(REXCVAR_GET(bf_effects_volume))
                       : category == 2                   ? Percent(REXCVAR_GET(bf_voices_volume))
                       : category == 1 || category == 3 ? Percent(REXCVAR_GET(bf_music_volume))
                                                         : 1.0;
  ctx.f1.f64 *= std::min(scale, 1.0);
}

namespace bf1943 {
// Once a frame (VdSwap): a changed volume marks the sound system dirty, so sounds already playing take it at once.
void GameOptionsFrame() {
#if defined(BF_DEV_TOOLS)
  static const bool dev = std::getenv("BF_DEV_OPTIONS") != nullptr;
  static auto at = std::chrono::steady_clock::now();
  if (dev && std::chrono::steady_clock::now() - at > std::chrono::seconds(5)) {
    at = std::chrono::steady_clock::now();
    REXLOG_INFO("BF options: in 5 s {} shake rotations, {} blur reads (largest {:.3f}, the manager's {:.3f}), {} sound "
                "volume updates, {} motion blur passes (largest {:.3f})",
                g_shakes.exchange(0), g_blurs.exchange(0), g_blur_max.exchange(0), g_manager_blur_max.exchange(0),
                g_sounds.exchange(0), g_motion_passes.exchange(0), g_motion_max.exchange(0));
  }
#endif
  static double last[3] = {-1, -1, -1};
  const double now[3] = {REXCVAR_GET(bf_music_volume), REXCVAR_GET(bf_effects_volume), REXCVAR_GET(bf_voices_volume)};
  if (now[0] == last[0] && now[1] == last[1] && now[2] == last[2]) return;
  std::copy(now, now + 3, last);
  auto* memory = REX_KERNEL_MEMORY();
  const uint32_t system = Load32(memory->TranslateVirtual<uint8_t*>(kSoundSystem), 0);
  if (system) *memory->TranslateVirtual<uint8_t*>(system + 0x201) = 1;
}
}  // namespace bf1943
