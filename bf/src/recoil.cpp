// The recoil kick on every frame (bf_smooth_recoil; static study: side-session note recoil_gunfire_study.md). TekRantGaming,
// sixth test: "Firing gun knockback looks lower fps".
//
// At each shot sub_822DC330 stores a random kick on the firing object (+0x6C pitch, +0x70 yaw, +0x74 roll, +0x78 field of
// view) and sets its cooldown +0x68 to the shot interval (60 / rate of fire). Each 30 Hz tick, sub_822E3CE8 lowers the
// cooldown by the tick's time. The camera reads the kick each frame through four getters (sub_822D6250, 62A8, 6300,
// 6358), each the kick times cooldown / interval: a linear fall from the full kick to 0 over one interval, but sampled
// at the tick, so at 110 FPS it came down as a staircase (the bone trace of run adsfire1: a jump each shot, then steps).
//
// Here each getter's answer is the same line sampled at this frame: times (cooldown - time since the tick) / cooldown.
// A new shot can only come at a tick, so between ticks the cooldown falls exactly so; nothing is guessed. The kick still
// lands on its frame as the game authors it. The camera only: the aim, the input and the server never see the kick,
// and the game's own values are never written.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(bf_smooth_recoil, true, "BF1943",
                    "The weapon's recoil kick eases back on every frame instead of in the game's 30 Hz steps");

namespace bf1943 {
double TickSeconds();  // tick_rate.cpp
}  // namespace bf1943

namespace {
using Clock = std::chrono::steady_clock;
std::atomic<int64_t> g_tick_us{0};  // the last firing tick (sub_822E3CE8), all firing objects tick together

int64_t NowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); }

float LoadF(const uint8_t* base, uint32_t a) {
  const uint32_t u = uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// The getter's answer resampled at this frame.
void Resample(PPCContext& ctx, const uint8_t* base, uint32_t firing) {
  if (!REXCVAR_GET(bf_smooth_recoil) || !firing || ctx.f1.f64 == 0.0) return;
  const double cooldown = LoadF(base, firing + 0x68);
  const int64_t tick = g_tick_us.load(std::memory_order_relaxed);
  if (!(cooldown > 0.0) || !tick) return;
  const double since = std::clamp(double(NowUs() - tick) / 1e6, 0.0, bf1943::TickSeconds());
  const double game = ctx.f1.f64;
  ctx.f1.f64 *= std::max(0.0, cooldown - since) / cooldown;
#if defined(BF_DEV_TOOLS)
  // BF_DEV_RECOIL=1: the first 400 non zero answers: the game's (a staircase) and ours, with the time since the tick.
  static const bool dev = std::getenv("BF_DEV_RECOIL") != nullptr;
  static std::atomic<int> logged{0};
  if (dev && logged.fetch_add(1) < 400)
    REXLOG_INFO("BF recoil: game {:.5f} ours {:.5f} cooldown {:.4f} since tick {:.4f} (at {} us)", game, ctx.f1.f64,
                cooldown, since, NowUs());
#endif
}
}  // namespace

REX_EXTERN(__imp__sub_822E3CE8);
REX_HOOK_RAW(sub_822E3CE8) {
  __imp__sub_822E3CE8(ctx, base);
  g_tick_us.store(NowUs(), std::memory_order_relaxed);
}

#define BF_RECOIL_GETTER(name)          \
  REX_EXTERN(__imp__##name);            \
  REX_HOOK_RAW(name) {                  \
    const uint32_t firing = ctx.r3.u32; \
    __imp__##name(ctx, base);           \
    Resample(ctx, base, firing);        \
  }

BF_RECOIL_GETTER(sub_822D6250)
BF_RECOIL_GETTER(sub_822D62A8)
BF_RECOIL_GETTER(sub_822D6300)
BF_RECOIL_GETTER(sub_822D6358)
