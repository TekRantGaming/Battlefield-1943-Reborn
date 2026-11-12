// A dedicated server's map rotation (bf_map_rotation; static study: side-session note map_rotation_study.md).
//
// The game itself loads no next level when a round ends on a dedicated server (EA's rotation lived in its Linux
// servers' Lua scripts, not in this executable): RoundOver (sub_82413900, once per player result) is followed by
// nothing. But it can change level inside a running server, with the players kept: sub_823F7168(eastl string* level,
// 1) posts the change level request it uses for its own restarts; the server's player connections go to the exit
// level state and back, and each client goes from the match back to loading the new level.
//
// So a round's end starts a timer (bf_round_end_delay, the end of round screen), and the server's own tick
// (sub_824123F0, on the server thread, the only safe place for the request) posts the next level of the list. The
// level now running is written to <user data>\current_map.txt, which gmhost.py reports to the backend's server list.
// With one map in the list the same level is loaded again.

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#if defined(BF_DEV_TOOLS)
#include "dev_guest_memory.h"  // windows.h: VirtualQuery
#endif

REXCVAR_DECLARE(bool, server);  // server_mode.cpp
REXCVAR_DECLARE(std::string, user_data_root);
REXCVAR_DEFINE_STRING(bf_map_rotation, "", "BF1943",
                      "A dedicated server's maps in turn, separated by commas: wake, guadalcanal, iwo, coral (or level "
                      "paths). Empty: the -level of the command line only");
REXCVAR_DEFINE_DOUBLE(bf_round_end_delay, 45.0, "BF1943",
                      "Seconds between a round's end and the next map of bf_map_rotation (the end of round screen)");

namespace {
using Clock = std::chrono::steady_clock;

struct Map {
  const char* key;
  const char* level;
  const char* title;
};
constexpr Map kMaps[] = {{"wake", "Levels/Wake_island_s", "Wake Island"},
                         {"guadalcanal", "Levels/Guadal_Canal", "Guadalcanal"},
                         {"iwo", "Levels/Iwo_Jima_s", "Iwo Jima"},
                         {"coral", "Levels/Coral_sea", "Coral Sea"}};

struct State {
  std::mutex mutex;
  std::vector<std::string> levels;
  size_t index = 0;
  bool parsed = false, pending = false, started_logged = false;
  Clock::time_point due{}, first_tick{};
  Clock::time_point changed_at{};  // our last level change, and the level it left (a restart of it just after: dropped)
  std::string changed_from;
} g;

std::string LevelOf(std::string key) {
  key.erase(0, key.find_first_not_of(" \t"));
  key.erase(key.find_last_not_of(" \t") + 1);
  for (const Map& m : kMaps) {
    if (key == m.key) return m.level;
  }
  return key;
}
std::string TitleOf(const std::string& level) {
  for (const Map& m : kMaps) {
    if (level == m.level) return m.title;
  }
  return level;
}

void Parse() {
  if (g.parsed) return;
  g.parsed = true;
  std::stringstream in(REXCVAR_GET(bf_map_rotation));
  std::string item;
  while (std::getline(in, item, ',')) {
    if (const std::string level = LevelOf(item); !level.empty()) g.levels.push_back(level);
  }
}

void WriteCurrent(const std::string& level) {
  const std::filesystem::path dir = REXCVAR_GET(user_data_root);
  if (dir.empty()) return;
  std::ofstream(dir / "current_map.txt", std::ios::trunc) << TitleOf(level) << "\n";
  REXLOG_INFO("BF rotation: now {}", TitleOf(level));
}

// A level's name as an eastl string in guest memory {begin, end, capacity end} with its text after it, for the change
// level request (sub_823F7168 copies it into its message at once). 0 when no memory could be had.
uint32_t LevelNameString(uint8_t* base, const std::string& level) {
  static uint32_t scratch = 0;
  if (!scratch) scratch = REX_KERNEL_MEMORY()->SystemHeapAlloc(128);
  if (!scratch) return 0;
  uint8_t* p = base + scratch;
  std::memset(p, 0, 128);
  const uint32_t text = scratch + 16, size = uint32_t(std::min<size_t>(level.size(), 100));
  std::memcpy(p + 16, level.data(), size);
  auto store32 = [&](uint32_t at, uint32_t v) {
    p[at] = uint8_t(v >> 24), p[at + 1] = uint8_t(v >> 16), p[at + 2] = uint8_t(v >> 8), p[at + 3] = uint8_t(v);
  };
  store32(0, text), store32(4, text + size), store32(8, text + size + 1);
  return scratch;
}

// The same level, whatever the case ("Levels/Wake_island_s" and "levels/wake_island_s").
bool SameLevel(const std::string& a, const std::string& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

// Where the Mission entity (sub_8232EEE8) asks for its next level after a round's end: the return address of its call.
constexpr uint32_t kRoundEndRestart = 0x8232F1BC;
}  // namespace

namespace bf1943 {
void NoteLevelChange();  // modern_mode.cpp: the new level's data gets the server's movement and aiming values again
}  // namespace bf1943

namespace bf1943 {
// For our GameManager host (gm_host.cpp): the rotation's maps by title, and the one running now ("" before the
// server's first tick, or with no rotation).
std::vector<std::string> RotationTitles() {
  std::lock_guard lock(g.mutex);
  Parse();
  std::vector<std::string> out;
  for (const std::string& level : g.levels) out.push_back(TitleOf(level));
  return out;
}
std::string CurrentMapTitle() {
  std::lock_guard lock(g.mutex);
  if (!g.started_logged || g.levels.empty()) return {};
  return TitleOf(g.levels[g.index % g.levels.size()]);
}
}  // namespace bf1943

// RoundOver's per player result: the round has ended. The game itself then restarts the same level, 20 s later (its
// Mission entity's next level, empty: run roundend4), and on TekRantGaming's playtest of 10 Nov 2026 Wake Island loaded
// again and was played until our timer, 45 s after the round's end, loaded the next map. So that restart is held
// (sub_823F7168 below) and the next map loads once the pause between rounds (bf_round_end_delay) is over: one load,
// from the end of round screen to the next map.
REX_EXTERN(__imp__sub_82413900);
REX_HOOK_RAW(sub_82413900) {
  __imp__sub_82413900(ctx, base);
  if (!REXCVAR_GET(server)) return;
  std::lock_guard lock(g.mutex);
  Parse();
  // One map: the same one again (else the server stayed on the end of round screen).
  if (g.levels.empty() || g.pending) return;
  g.pending = true;
  const double pause = std::max(0.0, REXCVAR_GET(bf_round_end_delay));
  g.due = Clock::now() + std::chrono::milliseconds(int64_t(pause * 1000));
  REXLOG_INFO("BF rotation: the round is over; {} in {:.0f} s", TitleOf(g.levels[(g.index + 1) % g.levels.size()]), pause);
}

// Every level change the server asks for (the game's own; ours call the original directly): sub_823F7168(eastl
// string* level, flag), called from four places: the Mission entity's next level after a round's end (lr 0x8232F1BC,
// an empty name: this level again), the PreRound entity's restart when there are not enough players (0x82326580), the
// backend's reload of the current level (0x823C2DC8) and Game::loadLevel (0x82405800). After a round's end every
// request is held until the pause between rounds is over (the server's tick then loads the next map), or, once it is
// over, becomes the next map at once. In the 30 s after one of our changes the round end's restart, or a request for
// the map we left, is dropped (a pause shorter than the game's 20 s); anything else goes as the game asks.
REX_EXTERN(__imp__sub_823F7168);
REX_HOOK_RAW(sub_823F7168) {
  if (!REXCVAR_GET(server)) return __imp__sub_823F7168(ctx, base);
  auto load = [&](uint32_t a) {
    return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
  };
  std::string asked;
  if (const uint32_t s = ctx.r3.u32) {
    const uint32_t b = load(s), e = load(s + 4);
    if (b && e > b && e - b < 200) asked.assign(reinterpret_cast<const char*>(base + b), e - b);
  }
  std::string next;
  {
    std::lock_guard lock(g.mutex);
    Parse();
    const auto now = Clock::now();
    if (g.pending && !g.levels.empty()) {
      next = g.levels[(g.index + 1) % g.levels.size()];
      if (now < g.due) {
        REXLOG_INFO("BF rotation: the game restarts {} after the round's end (lr {:08X}): held, {} in {:.0f} s",
                    asked.empty() ? "its level" : TitleOf(asked), uint32_t(ctx.lr), TitleOf(next),
                    std::chrono::duration<double>(g.due - now).count());
        return;
      }
    } else if (g.changed_at != Clock::time_point{} && now - g.changed_at < std::chrono::seconds(30) &&
               (uint32_t(ctx.lr) == kRoundEndRestart || (!asked.empty() && SameLevel(asked, g.changed_from)))) {
      REXLOG_INFO("BF rotation: the game restarts {} just after our change to {} (lr {:08X}): dropped",
                  asked.empty() ? "its level" : TitleOf(asked), TitleOf(g.levels[g.index % g.levels.size()]),
                  uint32_t(ctx.lr));
      return;
    }
  }
  const uint32_t name = next.empty() ? 0 : LevelNameString(base, next);
  if (!name) {
    REXLOG_INFO("BF rotation: the game changes the level to {} (lr {:08X})", asked.empty() ? "its own" : asked,
                uint32_t(ctx.lr));
    return __imp__sub_823F7168(ctx, base);
  }
  {
    std::lock_guard lock(g.mutex);
    g.pending = false;
    g.changed_from = g.levels[g.index % g.levels.size()], g.changed_at = Clock::now();
    g.index = (g.index + 1) % g.levels.size();
  }
  REXLOG_INFO("BF rotation: the game restarts {} after the round's end (lr {:08X}): {} ({}) instead",
              asked.empty() ? "its level" : TitleOf(asked), uint32_t(ctx.lr), TitleOf(next), next);
  PPCContext call = ctx;
  call.r3.u64 = name;
  __imp__sub_823F7168(call, base);
  ctx.r3 = call.r3;
  bf1943::NoteLevelChange();  // the new level's data: the modern mode's values again (modern_mode.cpp)
  WriteCurrent(next);
}

// The server's tick: the next level when the round end's delay is over and the game did not restart the level.
REX_EXTERN(__imp__sub_824123F0);
namespace bf1943 {
void ModernModeTick(uint8_t* base);  // modern_mode.cpp: the movement and aiming values for this server's mode
void GmHostStart();  // gm_host.cpp: our GameManager host, once (bf_gm_backend)
#if defined(BF_DEV_TOOLS)
void RunGameFixTest(PPCContext& ctx, uint8_t* base);  // game_fixes.cpp: the empty slot guard, tested once
#endif
}  // namespace bf1943
// The server's own tick rate, logged every 30 s (a dedicated server only): how many ticks a second, the longest gap
// between two and the tick's own time, so a server on a slow machine shows in its log (TekRantGaming's 7 Nov 2026 play test:
// the frame rate fell on the larger maps of the official server, a Hetzner machine, and not on Wake Island).
static void NoteServerTick(double work_ms) {
  static Clock::time_point window{}, last{};
  static uint32_t ticks = 0;
  static double longest_gap = 0, work_total = 0, work_longest = 0;
  const auto now = Clock::now();
  if (last != Clock::time_point{}) {
    longest_gap = std::max(longest_gap, std::chrono::duration<double, std::milli>(now - last).count());
  } else {
    window = now;
  }
  last = now;
  ++ticks;
  work_total += work_ms;
  work_longest = std::max(work_longest, work_ms);
  const double seconds = std::chrono::duration<double>(now - window).count();
  if (seconds < 30.0) return;
  REXLOG_INFO("BF server ticks: {:.1f} a second over {:.0f} s, longest gap {:.1f} ms; the tick's own work {:.2f} ms average, "
              "{:.1f} ms longest",
              ticks / seconds, seconds, longest_gap, work_total / ticks, work_longest);
  window = now, ticks = 0, longest_gap = work_total = work_longest = 0;
}

#if defined(BF_DEV_TOOLS)
namespace {
// BF_DEV_MOVE_SPAWNS=<x,y,z>[;<x,y,z>...]><x,y,z> (tests, a dedicated server): every copy in memory of a position
// given first (spawn points' positions, the level file's exact values: three floats together) becomes the last
// position, so the test's soldier spawns there (Wake Island: the carrier's spawn points moved to the airfield's flag,
// to take it and reach the air raid bunker). Looked for every 3 s in the server's first 5 minutes.
void DevMoveSpawns(uint8_t* base) {
  struct Config {
    std::vector<std::array<uint32_t, 3>> from;
    std::array<uint32_t, 3> to{};
    bool on = false;
  };
  static const Config cfg = [] {
    Config c;
    const char* v = std::getenv("BF_DEV_MOVE_SPAWNS");
    if (!v || !*v) return c;
    const std::string text = v;
    const size_t arrow = text.find('>');
    if (arrow == std::string::npos) return c;
    auto parse = [](const std::string& item, std::array<uint32_t, 3>& out) {
      float f[3] = {};
      if (std::sscanf(item.c_str(), "%f,%f,%f", &f[0], &f[1], &f[2]) != 3) return false;
      for (int i = 0; i < 3; ++i) std::memcpy(&out[i], &f[i], 4);
      return true;
    };
    for (size_t pos = 0; pos < arrow;) {
      const size_t semi = std::min(text.find(';', pos), arrow);
      std::array<uint32_t, 3> a{};
      if (parse(text.substr(pos, semi - pos), a)) c.from.push_back(a);
      pos = semi + 1;
    }
    c.on = parse(text.substr(arrow + 1), c.to) && !c.from.empty();
    return c;
  }();
  if (!cfg.on) return;
  static Clock::time_point first{}, next{};
  const auto now = Clock::now();
  if (first == Clock::time_point{}) first = now;
  if (now < next || now - first > std::chrono::minutes(5)) return;
  next = now + std::chrono::seconds(3);
  auto be = [](const uint8_t* m) { return uint32_t(m[0]) << 24 | uint32_t(m[1]) << 16 | uint32_t(m[2]) << 8 | m[3]; };
  int moved = 0;
  std::string where;
#if !defined(_WIN32)
  (void)be;
  static bool told = false;
  if (!told) {
    told = true;
    REXLOG_INFO("BF dev move spawns: the memory scan is Windows only");
  }
  return;
#else
  for (uint64_t a = 0x00100000; a < 0x100000000ull;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) break;
    const uint64_t start = uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base);
    const uint64_t end = start + mbi.RegionSize;
    const bool writable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                          (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
    // The physical memory's other views (0xA0000000 and up) alias the same bytes: one pass is enough.
    if (writable && start >= 0x00100000ull && start < 0xA0000000ull)
      for (uint64_t p = (std::max<uint64_t>(start, 0x00100000ull) + 3) & ~3ull; p + 12 <= end; p += 4) {
        uint8_t* m = base + p;
        const uint32_t x = be(m);
        for (const auto& f : cfg.from) {
          if (x != f[0] || be(m + 4) != f[1] || be(m + 8) != f[2]) continue;
          for (int i = 0; i < 3; ++i) {
            const uint32_t w = cfg.to[i];
            m[i * 4] = uint8_t(w >> 24), m[i * 4 + 1] = uint8_t(w >> 16), m[i * 4 + 2] = uint8_t(w >> 8),
                    m[i * 4 + 3] = uint8_t(w);
          }
          ++moved;
          if (where.size() < 600) where += fmt::format(" {:08X}", uint32_t(p));
          break;
        }
      }
    a = end;
  }
#endif
  if (moved)
    REXLOG_INFO("BF dev spawns: {} copies of the spawn points moved ({:.0f} s after the first tick):{}", moved,
                std::chrono::duration<double>(now - first).count(), where);
}

// BF_DEV_TICKETS=<after s>[,<offset hex>,<value>] (tests, a dedicated server: a round's end on demand): <after s>
// after the first tick, and 4 s and 10 s after it, each of the server's ticket counters (ServerTicketCounterEntity:
// vtable 0x820150A0, found from its type record 0x82D568B0, one a team) logged, its first 0x94 bytes as words; with an
// offset and a value, that float of every counter is set to the value once (+84 is the team's tickets: set to 1, the
// next death, a suicide from the pause, ends the round; set to 0 nothing happens until a ticket is lost).
void DevTickets(uint8_t* base) {
  struct Config {
    double after = -1;
    uint32_t offset = 0;
    float value = 0;
    bool set = false;
  };
  static const Config cfg = [] {
    Config c;
    const char* v = std::getenv("BF_DEV_TICKETS");
    if (!v || !*v) return c;
    c.after = std::atof(v);
    if (const char* comma = std::strchr(v, ',')) {
      c.offset = uint32_t(std::strtoul(comma + 1, nullptr, 16));
      if (const char* second = std::strchr(comma + 1, ',')) c.value = float(std::atof(second + 1)), c.set = true;
    }
    return c;
  }();
  if (cfg.after < 0) return;
  static Clock::time_point first{};
  static int step = 0;
  const auto now = Clock::now();
  if (first == Clock::time_point{}) first = now;
  static constexpr double kAt[] = {0, 4, 10};
  if (step >= 3 || now - first < std::chrono::duration<double>(cfg.after + kAt[step])) return;
  const int this_step = step++;
  auto be = [](const uint8_t* m) { return uint32_t(m[0]) << 24 | uint32_t(m[1]) << 16 | uint32_t(m[2]) << 8 | m[3]; };
  std::vector<uint32_t> found;
#if defined(_WIN32)
  for (uint64_t a = 0x00100000; a < 0xA0000000ull && found.size() < 8;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) break;
    const uint64_t start = uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base);
    const uint64_t end = std::min<uint64_t>(start + mbi.RegionSize, 0xA0000000ull);
    const bool writable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                          (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
    if (writable && start >= 0x00100000ull)
      for (uint64_t p = (start + 15) & ~15ull; p + 0x94 <= end && found.size() < 8; p += 16)
        if (be(base + p) == 0x820150A0u) found.push_back(uint32_t(p));
    a = start + mbi.RegionSize;
  }
#else
  (void)be;  // the memory scan is Windows only
#endif
  for (uint32_t obj : found) {
    std::string words;
    for (uint32_t off = 0; off < 0x94; off += 4) words += fmt::format("{}{:08X}", off % 32 ? " " : fmt::format(" | +{:02X}:", off), be(base + obj + off));
    REXLOG_INFO("BF dev tickets ({:.0f} s): counter {:08X}{}", cfg.after + kAt[this_step], obj, words);
  }
  if (cfg.set && this_step == 0 && cfg.offset + 4 <= 0x94)
    for (uint32_t obj : found) {
      uint32_t bits;
      std::memcpy(&bits, &cfg.value, 4);
      uint8_t* m = base + obj + cfg.offset;
      m[0] = uint8_t(bits >> 24), m[1] = uint8_t(bits >> 16), m[2] = uint8_t(bits >> 8), m[3] = uint8_t(bits);
      REXLOG_INFO("BF dev tickets: counter {:08X} +{:X} set to {}", obj, cfg.offset, cfg.value);
    }
  if (found.empty()) REXLOG_INFO("BF dev tickets ({:.0f} s): no ticket counter found", cfg.after + kAt[this_step]);
}
}  // namespace
#endif

REX_HOOK_RAW(sub_824123F0) {
  const auto tick_start = Clock::now();
  __imp__sub_824123F0(ctx, base);
  if (!REXCVAR_GET(server)) return;
#if defined(BF_DEV_TOOLS)
  // BF_DEV_SERVER_SLOW=<ms> (tests): each server tick takes that much longer, as on a slow machine.
  static const double slow_ms = [] {
    const char* v = std::getenv("BF_DEV_SERVER_SLOW");
    return v && *v ? std::atof(v) : 0.0;
  }();
  if (slow_ms > 0) std::this_thread::sleep_for(std::chrono::microseconds(int64_t(slow_ms * 1000)));
  DevMoveSpawns(base);
  DevTickets(base);
  bf1943::RunGameFixTest(ctx, base);  // game_fixes.cpp: BF_DEV_FIX_TEST=1, once
#endif
  NoteServerTick(std::chrono::duration<double, std::milli>(Clock::now() - tick_start).count());
  bf1943::ModernModeTick(base);
  bf1943::GmHostStart();
  std::string next;
  {
    std::lock_guard lock(g.mutex);
    Parse();
    if (g.levels.empty()) return;
    const auto now = Clock::now();
    if (!g.started_logged) {
      g.started_logged = true, g.first_tick = now;
      WriteCurrent(g.levels[0]);
    }
#if defined(BF_DEV_TOOLS)
    // BF_DEV_ROTATE_AFTER=<s> (tests): the next level that long after the server's first tick, as if a round ended.
    static const double dev_after = [] {
      const char* v = std::getenv("BF_DEV_ROTATE_AFTER");
      return v && *v ? std::atof(v) : -1.0;
    }();
    static bool dev_done = false;
    if (dev_after >= 0 && !dev_done && !g.levels.empty() &&
        now - g.first_tick > std::chrono::milliseconds(int64_t(dev_after * 1000))) {
      dev_done = true, g.pending = true, g.due = now;
    }
#endif
    if (!g.pending || now < g.due) return;
    next = g.levels[(g.index + 1) % g.levels.size()];
  }
  // The name first: the game's own restart was held, so without this change the server would stay on the end of
  // round screen (no guest memory: tried again on the next tick).
  const uint32_t name = LevelNameString(base, next);
  if (!name) return;
  {
    std::lock_guard lock(g.mutex);
    if (!g.pending) return;
    g.pending = false;
    g.changed_from = g.levels[g.index % g.levels.size()], g.changed_at = Clock::now();
    g.index = (g.index + 1) % g.levels.size();
  }
  PPCContext call = ctx;
  call.r3.u64 = name;
  call.r4.u64 = 1;
  __imp__sub_823F7168(call, base);
  bf1943::NoteLevelChange();  // the new level's data: the modern mode's values again (modern_mode.cpp)
  REXLOG_INFO("BF rotation: changing level to {} ({}), the pause between rounds over", TitleOf(next), next);
  WriteCurrent(next);
}
