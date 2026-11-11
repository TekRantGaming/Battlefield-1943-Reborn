// Developer aid (BF_DEV_TOOLS builds only), BF_CONFIG_PROBE=1: where the game looks for its configuration archives.
// sub_82383738 mounts /data/dist/<platform>/config.zz and, when sub_8266B8A8(fs, &base, &path, 0) says it exists,
// /scratch/config.zip over it (a developer override). Logged: both strings (the string objects hold a pointer to
// their characters first) and the answer.

#if defined(BF_DEV_TOOLS)

#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

namespace {

bool ProbeOn() {
  static const bool on = [] {
    const char* v = std::getenv("BF_CONFIG_PROBE");
    return v && *v == '1';
  }();
  return on;
}

uint32_t Load32(const uint8_t* base, uint32_t a) {
  return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
}

std::string StringObject(const uint8_t* base, uint32_t object) {
  std::string s;
  const uint32_t chars = object ? Load32(base, object) : 0;
  for (uint32_t i = 0; chars && i < 200 && base[chars + i]; ++i) s += char(base[chars + i]);
  return s;
}

}  // namespace

// The settings registry's string getter sub_82616E38(&out, registry, &name) (from the client's state change,
// "Client.SecondaryServerIp"): every name read, once, and the value.
REX_EXTERN(__imp__sub_82616E38);
REX_HOOK_RAW(sub_82616E38) {
  if (!ProbeOn()) return __imp__sub_82616E38(ctx, base);
  const uint32_t out = ctx.r3.u32;
  const std::string name = StringObject(base, ctx.r5.u32);
  __imp__sub_82616E38(ctx, base);
  static std::mutex mutex;
  static std::set<std::string> seen;
  std::lock_guard<std::mutex> lock(mutex);
  if (seen.insert(name).second)
    REXLOG_INFO("BF config probe: string setting {} = '{}'", name, StringObject(base, out));
}

REX_EXTERN(__imp__sub_8266B8A8);
REX_HOOK_RAW(sub_8266B8A8) {
  if (!ProbeOn()) return __imp__sub_8266B8A8(ctx, base);
  const std::string a = StringObject(base, ctx.r4.u32), b = StringObject(base, ctx.r5.u32);
  __imp__sub_8266B8A8(ctx, base);
  REXLOG_INFO("BF config probe: exists('{}', '{}') = {}", a, b, ctx.r3.u32 & 0xFF);
}

#endif  // BF_DEV_TOOLS
