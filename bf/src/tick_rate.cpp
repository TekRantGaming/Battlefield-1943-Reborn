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
// Every player and server in a match must run the same rate: a game takes its server's when it joins. Default 30.

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>

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

// A player's game takes the rate of the server it joins: SetTickRate, before the game's timers are made (a join
// from the launcher); once they are made the rate is the run's, and a server of another rate is joined by starting
// the game again at it (game_menu.cpp). 60 is the default since the update after 0.4.5 (TekRantGaming, 14 Nov 2026:
// "all servers are going to be 60 Hertz. So make sure that the game is loading up in the appropriate mode. We will no
// longer be offering 30 Hertz server"), so a player's game starts at the official servers' rate and joins them without
// a restart; 30 is the Xbox 360's, 75 and 100 experimental (the player is warned before joining one).
// 0.4.6.1 made 30 the default once more; 0.4.6.2 (TekRantGaming, 14 Nov 2026: "update all clients to default back to
// 60 Hz") 60 again. A server of another rate is still joined by starting the game again at it.
REXCVAR_DEFINE_DOUBLE(bf_tick_rate, 60.0, "BF1943",
                      "The game's simulation rate, ticks a second: 60 (the default), 30 (the Xbox 360's), or 75 or 100 "
                      "(experimental). A server runs at it; a player's game starts at it and takes the rate of the "
                      "server it joins");

namespace {
constexpr uint32_t kTimerCallers[] = {0x8239709C, 0x823970C8, 0x82370E78};
constexpr uint32_t kOneThirtieth = 0x820071D4;  // the shared float 1/30
std::atomic<int> g_rate{0};
std::atomic<bool> g_timers_made{false};  // the game's timers exist: the rate is fixed for this run

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
// The rates there are: 30 (the Xbox 360's, the default), and 60, 75 and 100, experimental (the game's timers and time
// step take any rate; only 30 and 60 have been played). Anything else is `otherwise`.
int ValidTickRate(long rate, int otherwise) {
  return rate == 30 || rate == 60 || rate == 75 || rate == 100 ? int(rate) : otherwise;
}
// The rate in use, fixed once the game's timers are made.
int TickRate() {
  int r = g_rate.load(std::memory_order_relaxed);
  if (r) return r;
  const double v = REXCVAR_GET(bf_tick_rate);
  r = std::isfinite(v) ? ValidTickRate(std::lround(v), 30) : 30;
  g_rate.store(r, std::memory_order_relaxed);
  return r;
}
// The rate for this run, chosen before the game's timers are made (the server about to be joined). False once they
// are made and the rate asked for is another: this run cannot change it.
bool SetTickRate(int rate) {
  rate = ValidTickRate(rate, 30);
  if (g_timers_made.load(std::memory_order_acquire)) return TickRate() == rate;
  if (TickRate() != rate) REXLOG_INFO("BF tick rate: {} ticks a second for this run (the server's)", rate);
  g_rate.store(rate, std::memory_order_relaxed);
  return true;
}
bool TickRateFixed() { return g_timers_made.load(std::memory_order_acquire); }
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
  if (ours) g_timers_made.store(true, std::memory_order_release);
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

// Swimmers getting out of the sea above 30 ticks a second (18 Nov 2026, the user: "sometimes players can't exit the
// water back to land when swimming"; runs swim_e1 to swim_st60a, Wake Island's east shore by the airfield, a seabed of
// 20 to 25 degrees). The character physics' deep water test, sub_8252FB10(physics r3, position r4): deep when the
// position is more than WaterDepthLimit under the water's height (physics+304; the limit from the physics data,
// physics+148 -> +0x44, 1.4 m for the soldier, 0.05 less while swimming) and a ray down from the surface over that depth
// finds no ground. A swimmer is held with its feet BodyUnderWater (1.4 m) under the surface: the swimming state's
// update (sub_82518F68) sets its vertical speed to (hold - y) / dt each tick. Pushing into a sloping seabed, the
// capsule slides up the slope and is pulled back, and settles above its hold by about speed * dt / tan(slope): it gets
// out once that reaches the 0.05 m, so the lift is half as much at 60 ticks as at 30, and swimmers stayed in the water
// on slopes the Xbox's game let them climb (at 30 a second the same shore let them out: swim_e30, swim_t30f240).
// Here a position held above the limit counts as lifted by that times rate / 30, as a 30 a second tick would have
// lifted it: at 30 nothing changes, and the test can only answer "not deep" where it answered "deep", never the other
// way. The server runs it too (its soldier is the authority), so player and server agree.
REXCVAR_DEFINE_BOOL(bf_swim_exit_tick_scale, true, "BF1943",
                    "Swimmers get out of the water on the same shores at any tick rate as at the Xbox's 30");
namespace {
float GuestFloat(const uint8_t* base, uint32_t a) {
  const uint32_t u = uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
  return std::bit_cast<float>(u);
}
void StoreGuestFloat(uint8_t* base, uint32_t a, float f) {
  const uint32_t u = std::bit_cast<uint32_t>(f);
  base[a] = uint8_t(u >> 24), base[a + 1] = uint8_t(u >> 16), base[a + 2] = uint8_t(u >> 8), base[a + 3] = uint8_t(u);
}
}  // namespace

#if defined(BF_DEV_TOOLS)
namespace {
// BF_DEV_WATER_TEST=1 (developer builds): each change of the test's answer logged, with the depth it saw.
void WaterTestLog(uint32_t phys, bool deep, float x, float y, float z, float water, float scaled) {
  static const bool on = std::getenv("BF_DEV_WATER_TEST") != nullptr;
  if (!on) return;
  static std::atomic<uint32_t> last_phys{0};
  static std::atomic<int> last{-1};
  if (last_phys.exchange(phys) == phys && last.exchange(deep) == int(deep)) return;
  last = deep;
  REXLOG_INFO("BF water test: physics {:08X} {} at {:.2f} {:.2f} {:.2f}, {:.3f} under the water {:.2f} (counted {:.3f})", phys,
              deep ? "deep" : "not deep", x, y, z, water - y, water, scaled);
}
}  // namespace
#endif

REX_EXTERN(__imp__sub_8252FB10);
REX_HOOK_RAW(sub_8252FB10) {
  const int rate = bf1943::TickRate();
  const uint32_t phys = ctx.r3.u32, pos = ctx.r4.u32;
#if defined(BF_DEV_TOOLS)
  if (phys && pos && std::getenv("BF_DEV_WATER_TEST") && (rate <= 30 || !REXCVAR_GET(bf_swim_exit_tick_scale))) {
    const float x = GuestFloat(base, pos), y = GuestFloat(base, pos + 4), z = GuestFloat(base, pos + 8);
    const float water = GuestFloat(base, phys + 304);
    __imp__sub_8252FB10(ctx, base);
    WaterTestLog(phys, (ctx.r3.u32 & 0xFF) != 0, x, y, z, water, water - y);
    return;
  }
#endif
  if (rate <= 30 || !phys || !pos || !REXCVAR_GET(bf_swim_exit_tick_scale)) return __imp__sub_8252FB10(ctx, base);
  const uint32_t data = uint32_t(base[phys + 148]) << 24 | uint32_t(base[phys + 149]) << 16 |
                        uint32_t(base[phys + 150]) << 8 | base[phys + 151];
  const float y = GuestFloat(base, pos + 4), water = GuestFloat(base, phys + 304);
  const float limit = data ? GuestFloat(base, data + 0x44) : 0.0f;
  const float depth = water - y;
  if (!(limit > 0.0f && depth > 0.0f && depth < limit)) return __imp__sub_8252FB10(ctx, base);
  // Lifted (limit - depth) above the hold: counted as rate / 30 times that.
  const float scaled = limit - (limit - depth) * float(rate / 30.0);
  StoreGuestFloat(base, pos + 4, water - scaled);
  __imp__sub_8252FB10(ctx, base);
  StoreGuestFloat(base, pos + 4, y);  // the caller's copy as it was
#if defined(BF_DEV_TOOLS)
  WaterTestLog(phys, (ctx.r3.u32 & 0xFF) != 0, GuestFloat(base, pos), y, GuestFloat(base, pos + 8), water, scaled);
#endif
}

// The soldier's movement at more than 30 ticks a second (18 Nov 2026, after the swimmers: "check every event where this
// could happen"). Two more places where the character physics counts in ticks, not seconds, so that at 60 Frostbite's
// numbers for the Xbox's 30 came out twice as strong or half as long (levels-common
// objects/soldiers/common/defaultsoldierphysics.dbx):
//
// - Every state's move toward the wanted velocity, sub_8291A390(request r3, velocity r4), called by the seven states
//   that walk, run, swim, fall, slide, climb or steer a parachute: velocity += gain * (wanted - velocity) across the
//   ground (the upward speed is the states' own), the gain from the pose's AccelerationGain, DecelerationGain or
//   SprintGain (the request's +0: on the ground 0.1 to speed up, 0.8 to stop, 0.2 in the air, 0.05 falling or under a
//   parachute, 0.1 swimming, 1 elsewhere), with no time step in it (a cap at +96, 100 m/s, is never reached). At 60 the
//   soldier reached its speed, stopped, and steered in the air in half the time (runs mv_30, mv_60off: half speed in
//   0.21 s at 30, 0.12 at 60; a parachute's slowing after it opens, 13.5 to 11.7 m/s, in 1 s at 30, 0.5 at 60). Here
//   the gain is 1 - (1 - gain)^(30 / rate): the same share in a second as 30 ticks of it (mv_60: 0.21 s; pc_60).
// - The jump, sub_82513B60(state, physics r4, step, velocity): it holds the upward speed at sqrt(2 g JumpHeight)
//   (6.1 m/s) for two ticks, a count at physics+68, then hands over to the in-air state: a fifteenth of a second at 30,
//   a thirtieth at 60. The soldier rose 0.38 to 0.41 m in it at 30 (jl_30, mv_30) and 0.18 to 0.22 at 60 (jl_60off,
//   mv_60off): jumps were about 0.2 m lower. Here the count is to 2 * rate / 30 ticks (mv_60: 0.40 to 0.41 m), kept in
//   the same field: it is saved and put back with the rest of the soldier's state (sub_825302D0) when the game replays
//   ticks, so a replayed jump counts the same.
//
// Looked at and left: the parachute's own VerticalGain (sub_82513F40 takes 0.11 of the fall above 6 m/s each tick):
// it falls at 11.3 m/s at 30 and 11.45 at 60 (pc_30, pc_60off), and the gain scaled the same way changed nothing (11.46).
// In the air gravity is taken in finer steps at 60 (about 5 cm less rise after the jump state, v dt / 2): not a count.
//
// None of this changes anything at 30. Servers run it too, so player and server agree.
REXCVAR_DEFINE_BOOL(bf_move_tick_scale, true, "BF1943",
                    "The soldier speeds up, stops, swims, steers in the air and jumps as at the Xbox's 30 ticks a "
                    "second at any tick rate");
namespace {
uint32_t GuestU32(const uint8_t* base, uint32_t a) {
  return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
}
void StoreGuestU32(uint8_t* base, uint32_t a, uint32_t u) {
  base[a] = uint8_t(u >> 24), base[a + 1] = uint8_t(u >> 16), base[a + 2] = uint8_t(u >> 8), base[a + 3] = uint8_t(u);
}
// A per tick fraction at 30 ticks a second as the fraction at `rate` that takes the same share in the same time.
float TickGain(float gain, int rate) {
  if (!(gain > 0.0f && gain < 1.0f)) return gain;
  return float(1.0 - std::pow(1.0 - double(gain), 30.0 / rate));
}
bool MoveScaled(int rate) { return rate > 30 && REXCVAR_GET(bf_move_tick_scale); }
}  // namespace

REX_EXTERN(__imp__sub_8291A390);
REX_HOOK_RAW(sub_8291A390) {
  const int rate = bf1943::TickRate();
  const uint32_t req = ctx.r3.u32;
#if defined(BF_DEV_TOOLS)
  static const bool log = std::getenv("BF_DEV_MOVE_LOG") != nullptr;
#else
  constexpr bool log = false;
#endif
  if (!req || (!MoveScaled(rate) && !log)) return __imp__sub_8291A390(ctx, base);
  const float gain = GuestFloat(base, req);
  const float scaled = MoveScaled(rate) ? TickGain(gain, rate) : gain;
  StoreGuestFloat(base, req, scaled);
#if defined(BF_DEV_TOOLS)
  // BF_DEV_MOVE_LOG=1 (developer builds): each gain seen for the first time, and what it became.
  if (log) {
    static std::atomic<int> seen{0};
    static std::atomic<uint32_t> gains[16];
    const uint32_t bits = std::bit_cast<uint32_t>(gain);
    bool known = false;
    for (int i = 0, n = std::min(seen.load(), 16); i < n && !known; ++i) known = gains[i].load() == bits;
    if (!known && seen.load() < 16) {
      gains[seen.fetch_add(1) & 15] = bits;
      REXLOG_INFO("BF move gain: {:.4f} -> {:.4f} a tick at {}", gain, scaled, rate);
    }
  }
  // BF_DEV_MOVE_LOG=2: every call's velocity before and after (the first 3000).
  static const bool each = log && std::atoi(std::getenv("BF_DEV_MOVE_LOG")) >= 2;
  const uint32_t vel = ctx.r4.u32, lr = uint32_t(ctx.lr);
  float before[3] = {};
  if (each && vel) for (int i = 0; i < 3; ++i) before[i] = GuestFloat(base, vel + 4 * i);
#endif
  __imp__sub_8291A390(ctx, base);
  StoreGuestFloat(base, req, gain);  // the caller's request as it was
#if defined(BF_DEV_TOOLS)
  static std::atomic<int> calls{0};
  if (each && vel && calls.fetch_add(1) < 3000)
    REXLOG_INFO("BF move: lr {:08X} vel {:08X} gain {:.4f}->{:.4f} v {:.3f} {:.3f} {:.3f} -> {:.3f} {:.3f} {:.3f}", lr, vel,
                gain, scaled, before[0], before[1], before[2], GuestFloat(base, vel), GuestFloat(base, vel + 4),
                GuestFloat(base, vel + 8));
#endif
}

REX_EXTERN(__imp__sub_82513B60);
REX_HOOK_RAW(sub_82513B60) {
  const int rate = bf1943::TickRate();
  const uint32_t phys = ctx.r4.u32;
  if (!phys || !MoveScaled(rate)) return __imp__sub_82513B60(ctx, base);
  const uint32_t ticks = uint32_t(std::lround(2.0 * rate / 30.0));
  const uint32_t so_far = GuestU32(base, phys + 68);  // this jump's ticks at this rate
  // The game adds one and ends the jump at 2: 1 for the last tick, 0 before it.
  StoreGuestU32(base, phys + 68, so_far + 1 >= ticks ? 1 : 0);
  __imp__sub_82513B60(ctx, base);
  if (GuestU32(base, phys + 68) == 1) StoreGuestU32(base, phys + 68, so_far + 1);  // still jumping
#if defined(BF_DEV_TOOLS)
  static const bool log = std::getenv("BF_DEV_MOVE_LOG") != nullptr;
  if (log) REXLOG_INFO("BF jump tick: physics {:08X} tick {} of {}", phys, so_far + 1, ticks);
#endif
}

// The first person gun and hands at more than 30 ticks a second (19 Nov 2026, the user: "Player hand model and weapons
// also banded around like the tickrate bug ... Both [turning and walking]. It's like a choppy bouncing around").
//
// The gun's lag and bounce are the weapon camera's (levels-common objects/weapons/handheld/common/*weaponcamera.dbx:
// FirstPersonCameraData with a WeaponSpringEffect and a CameraSpringEffect, WeaponLagSpringEffectData). Once a tick
// sub_821E68E8(effect r3, input r4, zoomed r5) runs them (found with a hardware watch on the spring data, run
// gs_watch1: about 60 reads a second, all from sub_821E6678):
// - Six lag values (the move and turn sticks times the camera data's Move/Rotate modifiers, zoomed or not) each go
//   toward their target by the camera data's ReleaseModifier (data+60, 0.2 to 0.3) every tick: s += (t - s) * R.
// - Then sub_821E6678 steps both spring effects' six springs (an axis: its data {Damping, Constant}, position,
//   velocity): v += -K x dt; v -= D v; x += v dt. The spring itself goes with the tick's time, but its damping is a
//   share taken every tick.
// At 60 both shares were taken twice as often: the lag let go of the stick twice as fast and the springs lost their
// swing twice as fast, so the gun followed the input stiffly and in small steps instead of swaying and settling as
// on the Xbox. Here, for the call, R and each D are 1 - (1 - share)^(30 / rate) (TickGain), then put back: the shares
// taken in a second are 30 ticks' worth again. Nothing changes at 30. Only the local player's view uses it.
REXCVAR_DEFINE_BOOL(bf_gun_tick_scale, true, "BF1943",
                    "The first person gun and hands lag, sway and settle as at the Xbox's 30 ticks a second at any "
                    "tick rate");
namespace {
bool GuestData(uint32_t a) { return a >= 0x10000 && a < 0x80000000u; }  // the heaps (never the image)
}  // namespace

REX_EXTERN(__imp__sub_821E68E8);
REX_HOOK_RAW(sub_821E68E8) {
  const int rate = bf1943::TickRate();
  const uint32_t effect = ctx.r3.u32;
  const bool scale = GuestData(effect) && rate > 30 && REXCVAR_GET(bf_gun_tick_scale);
#if defined(BF_DEV_TOOLS)
  // BF_DEV_GUN_LOG=1: each call's lag and springs (the gun's offset and turn, the camera's), the first 6000 calls.
  static const bool log = std::getenv("BF_DEV_GUN_LOG") != nullptr;
#else
  constexpr bool log = false;
#endif
  if (!scale && !log) return __imp__sub_821E68E8(ctx, base);
  // The shares this call takes: the camera data's ReleaseModifier and every spring axis's Damping (the data is shared
  // by every weapon with that camera: put back right after the call, one call at a time).
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  uint32_t saved_at[13];
  float saved[13];
  int n = 0;
  auto take = [&](uint32_t a) {
    if (!GuestData(a) || n == 13) return;
    for (int i = 0; i < n; ++i)
      if (saved_at[i] == a) return;
    saved_at[n] = a, saved[n] = GuestFloat(base, a);
    StoreGuestFloat(base, a, TickGain(saved[n], rate));
    ++n;
  };
  if (scale) {
    const uint32_t data = GuestU32(base, effect);
    if (GuestData(data)) take(data + 60);
    for (uint32_t at : {effect + 112, effect + 116}) {
      const uint32_t springs = GuestU32(base, at);
      if (!GuestData(springs)) continue;
      for (int i = 0; i < 6; ++i) {
        const uint32_t axis = GuestU32(base, springs + 48 + 4 * i);
        if (GuestData(axis)) take(GuestU32(base, axis));  // its data's Damping (+0; the Constant is +4)
      }
    }
  }
  __imp__sub_821E68E8(ctx, base);
  while (n > 0) --n, StoreGuestFloat(base, saved_at[n], saved[n]);
#if defined(BF_DEV_TOOLS)
  static int calls = 0;
  if (log && calls < 6000) {
    ++calls;
    static const auto t0 = std::chrono::steady_clock::now();
    auto v = [&](uint32_t a) { return GuestFloat(base, effect + a); };
    REXLOG_INFO("BF gun: t {:.4f} effect {:08X} lag {:.5f} {:.5f} {:.5f} | {:.5f} {:.5f} {:.5f} cam {:.5f} {:.5f} {:.5f} | "
                "{:.5f} {:.5f} {:.5f} scaled {}",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), effect, v(16), v(20),
                v(24), v(32), v(36), v(40), v(80), v(84), v(88), v(96), v(100), v(104), scale);
  }
#endif
}

// The camera's recoil when firing (19 Nov 2026, the same audit): CameraRecoilData (each weapon's firing data:
// SpringConstant +0x14, 150 to 200; SpringDamping +0x10, 0.2 to 0.4; SpringMinThresholdAngle +0x0C), stepped once a
// tick by sub_822D5BB0(recoil r3, dt f1) from the soldier weapon's update (sub_822DB740): two springs (pitch and
// yaw), each {data*, position, target, velocity}: v += -K (x - t) dt; v -= D v; x += v dt. The damping is a share
// taken every tick again (found by the same per-tick shape as the gun's springs), so at 60 the view's kick died out
// twice as fast. Here D is 1 - (1 - D)^(30 / rate) for the call, then put back; with the gun's switch.
REX_EXTERN(__imp__sub_822D5BB0);
REX_HOOK_RAW(sub_822D5BB0) {
  const int rate = bf1943::TickRate();
  const uint32_t recoil = ctx.r3.u32;
  const bool scale = GuestData(recoil) && rate > 30 && REXCVAR_GET(bf_gun_tick_scale);
#if defined(BF_DEV_TOOLS)
  // BF_DEV_GUN_LOG=1: each step's springs (pitch and yaw: position, target, velocity), the first 6000.
  static const bool log = std::getenv("BF_DEV_GUN_LOG") != nullptr;
#else
  constexpr bool log = false;
#endif
  if (!scale && !log) return __imp__sub_822D5BB0(ctx, base);
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  uint32_t at[2] = {};
  float was[2] = {};
  int n = 0;
  if (scale) {
    for (uint32_t field : {recoil + 4, recoil + 20}) {
      const uint32_t data = GuestU32(base, field);
      if (!GuestData(data) || (n == 1 && at[0] == data + 16)) continue;
      at[n] = data + 16, was[n] = GuestFloat(base, at[n]);
      StoreGuestFloat(base, at[n], TickGain(was[n], rate));
      ++n;
    }
  }
  __imp__sub_822D5BB0(ctx, base);
  while (n > 0) --n, StoreGuestFloat(base, at[n], was[n]);
#if defined(BF_DEV_TOOLS)
  static int calls = 0;
  if (log && calls < 6000) {
    ++calls;
    static const auto t0 = std::chrono::steady_clock::now();
    auto v = [&](uint32_t a) { return GuestFloat(base, recoil + a); };
    REXLOG_INFO("BF recoil: t {:.4f} recoil {:08X} pitch {:.5f} {:.5f} {:.5f} yaw {:.5f} {:.5f} {:.5f} scaled {}",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), recoil, v(8), v(12),
                v(16), v(24), v(28), v(32), scale);
  }
#endif
}
