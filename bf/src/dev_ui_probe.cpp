// Developer aid (BF_DEV_TOOLS builds only): which of the UI manager's methods (vtable 0x82020030, built by
// sub_82464AE0) run every frame, and what the screen looks like without one of them. BF_UI_PROBE=1 logs call counts
// every 5 s; BF_UI_SKIP=<hex address> returns from that method at once.

#if defined(BF_DEV_TOOLS)

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

namespace {

bool ProbeOn() {
  static const bool on = [] {
    const char* v = std::getenv("BF_UI_PROBE");
    return v && *v == '1';
  }();
  return on;
}

uint32_t SkipAddress() {
  static const uint32_t a = [] {
    const char* v = std::getenv("BF_UI_SKIP");
    return v && *v ? uint32_t(std::stoul(v, nullptr, 16)) : 0u;
  }();
  return a;
}

constexpr uint32_t kMethods[] = {0x826B77E0, 0x8245F6E0, 0x826B6698, 0x824177D8, 0x8269F078, 0x82417900,
                                 0x826A1B30, 0x8269CAD0, 0x826A1BA8, 0x82434AB0, 0x82475858};
std::atomic<uint32_t> g_counts[std::size(kMethods)];

void Note(int i) {
  if (!ProbeOn()) return;
  g_counts[i]++;
  static std::atomic<int64_t> last{0};
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t was = last.load();
  if (now - was >= 5 && last.compare_exchange_strong(was, now)) {
    std::string text;
    for (size_t k = 0; k < std::size(kMethods); ++k)
      text += fmt::format(" {:08X}:{}", kMethods[k], g_counts[k].exchange(0));
    REXLOG_INFO("BF UI probe (5 s):{}", text);
  }
}

}  // namespace

#define BF_UI_METHOD(index, addr)                    \
  REX_EXTERN(__imp__sub_##addr);                     \
  REX_HOOK_RAW(sub_##addr) {                         \
    Note(index);                                     \
    if (SkipAddress() == 0x##addr) return;           \
    __imp__sub_##addr(ctx, base);                    \
  }

BF_UI_METHOD(0, 826B77E0)
BF_UI_METHOD(1, 8245F6E0)
BF_UI_METHOD(2, 826B6698)
BF_UI_METHOD(3, 824177D8)
BF_UI_METHOD(4, 8269F078)
BF_UI_METHOD(5, 82417900)
BF_UI_METHOD(6, 826A1B30)
BF_UI_METHOD(7, 8269CAD0)
BF_UI_METHOD(8, 826A1BA8)
BF_UI_METHOD(9, 82434AB0)
BF_UI_METHOD(10, 82475858)

#endif  // BF_DEV_TOOLS

#if defined(BF_DEV_TOOLS)
// The Flash player's trace() output (sub_82976D38 prints "AptTrace: %s"), r3 or r4 the text: BF_APT_TRACE=1 logs it.
REX_EXTERN(__imp__sub_82976D38);
REX_HOOK_RAW(sub_82976D38) {
  static const bool on = [] {
    const char* v = std::getenv("BF_APT_TRACE");
    return v && *v == '1';
  }();
  if (on) {
    static std::atomic<int> lines{0};
    if (lines.fetch_add(1) < 400) {
      auto text = [&](uint32_t p) {
        std::string s;
        for (uint32_t i = 0; p >= 0x10000 && i < 160 && base[p + i]; ++i) s += char(base[p + i]);
        return s;
      };
      REXLOG_INFO("BF APT trace r3 '{}' r4 '{}'", text(ctx.r3.u32), text(ctx.r4.u32));
    }
  }
  __imp__sub_82976D38(ctx, base);
}
#endif  // BF_DEV_TOOLS

#if defined(BF_DEV_TOOLS)
// The Flash player's calls by path: sub_82954490(player, path, ...) without arguments, sub_829544F8 with them.
// BF_APT_TRACE=1 logs the paths.
#define BF_APT_CALL(addr)                                                                                       \
  REX_EXTERN(__imp__sub_##addr);                                                                              \
  REX_HOOK_RAW(sub_##addr) {                                                                                  \
    static const bool on = [] {                                                                               \
      const char* v = std::getenv("BF_APT_TRACE");                                                            \
      return v && *v == '1';                                                                                  \
    }();                                                                                                      \
    if (on) {                                                                                                 \
      static std::atomic<int> lines{0};                                                                       \
      std::string s;                                                                                          \
      for (uint32_t i = 0, p = ctx.r4.u32; p >= 0x10000 && i < 96 && base[p + i]; ++i) s += char(base[p + i]); \
      const uint32_t player = ctx.r3.u32;                                                                     \
      __imp__sub_##addr(ctx, base);                                                                           \
      if (lines.fetch_add(1) < 200)                                                                           \
        REXLOG_INFO("BF APT call " #addr " player {:08X} '{}' -> {:08X}", player, s, ctx.r3.u32);              \
      return;                                                                                                 \
    }                                                                                                         \
    __imp__sub_##addr(ctx, base);                                                                             \
  }
BF_APT_CALL(82954490)
BF_APT_CALL(829544F8)
#endif  // BF_DEV_TOOLS
