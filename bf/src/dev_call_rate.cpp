// Developer aid (BF_DEV_TOOLS builds only): how often some guest functions run, logged every 5 s with
// BF_CALL_RATE=1. The list: the functions that load a fixed 1/30 second (0.0333333) constant, to find any that step
// the game by a fixed 30 Hz frame while it runs faster (frame_rate.cpp).

#if defined(BF_DEV_TOOLS)

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

namespace {

bool RateOn() {
  static const bool on = [] {
    const char* v = std::getenv("BF_CALL_RATE");
    return v && *v == '1';
  }();
  return on;
}

constexpr uint32_t kFunctions[] = {0x822147A8, 0x82290960, 0x822E8A50, 0x823C78C8, 0x82512580, 0x82522430,
                                   0x8253EDF8, 0x82739138, 0x827899A8, 0x828B57F0};
std::atomic<uint32_t> g_calls[std::size(kFunctions)];

void Count(int i) {
  if (!RateOn()) return;
  g_calls[i]++;
  static std::atomic<int64_t> last{0};
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t was = last.load();
  if (now - was >= 5 && last.compare_exchange_strong(was, now)) {
    std::string text;
    for (size_t k = 0; k < std::size(kFunctions); ++k)
      text += fmt::format(" {:08X}:{}", kFunctions[k], g_calls[k].exchange(0));
    REXLOG_INFO("BF call rate (5 s):{}", text);
  }
}

}  // namespace

#define BF_COUNTED(index, addr)    \
  REX_EXTERN(__imp__sub_##addr);   \
  REX_HOOK_RAW(sub_##addr) {       \
    Count(index);                  \
    __imp__sub_##addr(ctx, base);  \
  }

BF_COUNTED(0, 822147A8)
BF_COUNTED(1, 82290960)
BF_COUNTED(2, 822E8A50)
// Physics speed check (TekRantGaming: falling trees and rubble in slow motion at a high frame rate): every 5 s, the float
// arguments the next call got.
#define BF_COUNTED_ARGS(index, addr)                                                                               REX_EXTERN(__imp__sub_##addr);                                                                                  REX_HOOK_RAW(sub_##addr) {                                                                                        Count(index);                                                                                                   if (RateOn()) {                                                                                                   static std::atomic<int64_t> last{0};                                                                            const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(                                                              std::chrono::steady_clock::now().time_since_epoch()).count();                           int64_t was = last.load();                                                                                      if (now - was >= 5000 && last.compare_exchange_strong(was, now))                                                  REXLOG_INFO("BF call args " #addr ": f1 {:.6f} f2 {:.6f} f3 {:.6f} r3 {:08X} r4 {:08X}", ctx.f1.f64,                          ctx.f2.f64, ctx.f3.f64, ctx.r3.u32, ctx.r4.u32);                                                }                                                                                                               __imp__sub_##addr(ctx, base);                                                                                 }
BF_COUNTED_ARGS(3, 823C78C8)
BF_COUNTED(4, 82512580)
BF_COUNTED(5, 82522430)
// 8253EDF8 is hooked in frame_rate.cpp (bf_physics_tick_time): not counted here.
// sub_82739138 (the particle manager's frame) is hooked in effects.cpp: not counted here.
// The game timer's per frame step (sub_822C7388(timer)): mode at +40, frame time at +72 (double), time scale at
// +88, the tick rate at +96 (doubles), ticks at +48. Logged every 5 s with BF_CALL_RATE=1: how far the ticks and
// the frame times moved in that time.
REX_EXTERN(__imp__sub_822C7388);
REX_HOOK_RAW(sub_822C7388) {
  const uint32_t timer = ctx.r3.u32;
  __imp__sub_822C7388(ctx, base);
  if (!RateOn()) return;
  auto u32 = [&](uint32_t a) {
    return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
  };
  auto f64 = [&](uint32_t a) {
    const uint64_t bits = uint64_t(u32(a)) << 32 | u32(a + 4);
    double value;
    std::memcpy(&value, &bits, 8);
    return value;
  };
  static uint32_t first_ticks = 0, frames = 0;
  static double time_sum = 0;
  static std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
  if (!frames) first_ticks = u32(timer + 48);
  ++frames;
  time_sum += f64(timer + 72);
  const auto now = std::chrono::steady_clock::now();
  const double real = std::chrono::duration<double>(now - since).count();
  if (real >= 5) {
    REXLOG_INFO("BF timer {:08X}: mode {} frames {} in {:.2f} s, frame times sum {:.3f} s, ticks +{} (rate {} scale {} "
                "last step {:.5f})",
                timer, u32(timer + 40), frames, real, time_sum, u32(timer + 48) - first_ticks, f64(timer + 96),
                f64(timer + 88), f64(timer + 72));
    frames = 0;
    time_sum = 0;
    since = now;
  }
}

// The client's update (sub_823961E8(client, frame time, ...)): with [client+192] == 0 it steps the client once a
// frame with the frame time, otherwise once per 30 Hz tick. Logged every 5 s with BF_CALL_RATE=1; BF_VARSTEP=1 runs
// the update as if the field were 0 (experiment: the variable step path for the online client). Called from
// tick_blend.cpp's hook of the update (all builds); true when it ran the update itself.
REX_EXTERN(__imp__sub_823961E8);
namespace bf1943 {
bool DevClientUpdate(PPCContext& ctx, uint8_t* base) {
  const uint32_t client = ctx.r3.u32;
  const uint32_t mode = uint32_t(base[client + 192]) << 24 | base[client + 193] << 16 | base[client + 194] << 8 |
                        base[client + 195];
  if (RateOn()) {
    static std::atomic<int64_t> last{0};
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t was = last.load();
    if (now - was >= 5 && last.compare_exchange_strong(was, now))
      REXLOG_INFO("BF client update {:08X}: mode +192 = {}, frame time {:.4f}, ticks {}", client, mode, ctx.f1.f64,
                  ctx.r5.u32);
  }
  static const bool varstep = [] {
    const char* v = std::getenv("BF_VARSTEP");
    return v && *v == '1';
  }();
  // Only the online client's value 2: before the client is set up the field holds stale heap bytes (run vs60v), and
  // it is put back only if the update left it at 0.
  if (varstep && mode == 2) {
    std::memset(base + client + 192, 0, 4);
    __imp__sub_823961E8(ctx, base);
    if (!base[client + 192] && !base[client + 193] && !base[client + 194] && !base[client + 195]) base[client + 195] = 2;
    return true;
  }
  return false;
}
}  // namespace bf1943

BF_COUNTED(8, 827899A8)
BF_COUNTED(9, 828B57F0)

#endif  // BF_DEV_TOOLS
