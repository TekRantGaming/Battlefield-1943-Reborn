// The game's simulation rate (0.4.x, bf_tick_rate): 30 ticks a second as on the Xbox 360, or 60.
//
// Research (12 Nov 2026, side notes; docs/60hz.md): the game's time step per tick is not a fixed constant. A GameTime
// object takes its rate once, when the client or server is made, from the main timer (timer+44), and gives each tick
// dt = 1 / rate (sub_822BB9F8); the client's EntryInputState dt (+128), the soldier, aim, weapons and physics all take
// that dt. The timers are made by sub_822C7270(timer, rate, mode) with a literal 30 at three places (the main loop's
// two: return addresses 0x8239709C and 0x823970C8; the listen server's thread job: 0x82370E78). Making them at the
// chosen rate makes every tick that long. A dedicated server's main timer ticks once a frame instead, paced by
// Game.MaxFps (frame_rate.cpp sets it to the rate on a server).
//
// One shared float 1/30 (guest 0x820071D4) is read by 12 functions: a clock delta's fallback, the physics' second step
// bound, a projectile's one-tick advance, a physics object's default step, start-up settle loops and a frame-count
// timer. It is set to 1 / rate once, at the first timer: the settle loops then settle in half the time at 60.
//
// Every player and server in a match must run the same rate (the 0.4 version gate keeps 30 and 60 apart). Default 30.

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_DOUBLE(bf_tick_rate, 30.0, "BF1943",
                      "The game's simulation rate, ticks a second: 30 (the original) or 60. Read at the start; every "
                      "player and server in a match must use the same");

namespace {
constexpr uint32_t kTimerCallers[] = {0x8239709C, 0x823970C8, 0x82370E78};
constexpr uint32_t kOneThirtieth = 0x820071D4;  // the shared float 1/30
std::atomic<int> g_rate{0};

// Four bytes of the game's read-only data written (its page made writable for the write, then put back: a plain write
// faulted, 13 Nov 00:00). True when written.
bool WriteReadOnly(uint8_t* host, uint32_t bits) {
#if defined(_WIN32)
  DWORD old = 0;
  if (!VirtualProtect(host, 4, PAGE_READWRITE, &old)) return false;
  host[0] = uint8_t(bits >> 24), host[1] = uint8_t(bits >> 16), host[2] = uint8_t(bits >> 8), host[3] = uint8_t(bits);
  DWORD unused = 0;
  VirtualProtect(host, 4, old, &unused);
  return true;
#else
  const long page = sysconf(_SC_PAGESIZE);
  uint8_t* start = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(host) & ~uintptr_t(page - 1));
  if (mprotect(start, size_t(page), PROT_READ | PROT_WRITE) != 0) return false;
  host[0] = uint8_t(bits >> 24), host[1] = uint8_t(bits >> 16), host[2] = uint8_t(bits >> 8), host[3] = uint8_t(bits);
  mprotect(start, size_t(page), PROT_READ);
  return true;
#endif
}
}  // namespace

namespace bf1943 {
// The rate in use: 30 or 60, fixed at the first read.
int TickRate() {
  int r = g_rate.load(std::memory_order_relaxed);
  if (r) return r;
  const double v = REXCVAR_GET(bf_tick_rate);
  r = std::isfinite(v) && std::lround(v) == 60 ? 60 : 30;
  g_rate.store(r, std::memory_order_relaxed);
  return r;
}
// One tick's time in seconds.
double TickSeconds() { return 1.0 / TickRate(); }
}  // namespace bf1943

// sub_822C7270(timer, rate, mode): a timer made at the chosen rate (the three game timers only).
REX_EXTERN(__imp__sub_822C7270);
REX_HOOK_RAW(sub_822C7270) {
  const uint32_t lr = uint32_t(ctx.lr);
  const int rate = bf1943::TickRate();
  bool ours = false;
  for (uint32_t c : kTimerCallers) ours = ours || lr == c;
  if (ours && rate != 30 && ctx.r4.u32 == 30) {
    ctx.r4.u64 = uint64_t(rate);
    // The shared 1/30, once.
    static std::atomic<bool> written{false};
    if (!written.exchange(true)) {
      const uint32_t bits = std::bit_cast<uint32_t>(float(1.0 / rate));
      uint8_t* p = base + kOneThirtieth;
      const uint32_t was = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
      const bool ok = WriteReadOnly(p, bits);
      REXLOG_INFO("BF tick rate: {} ticks a second; the shared step at {:08X} {:.6f} -> {:.6f}{}", rate, kOneThirtieth,
                  std::bit_cast<float>(was), 1.0 / rate, ok ? "" : " (could not be written: left at 1/30)");
    }
    REXLOG_INFO("BF tick rate: timer made at {} (caller {:08X}, mode {})", rate, lr, ctx.r5.u32);
  }
  __imp__sub_822C7270(ctx, base);
}
