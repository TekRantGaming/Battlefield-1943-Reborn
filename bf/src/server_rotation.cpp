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
#include <atomic>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
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
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // GetThreadTimes (the server's stall notes)
#include <psapi.h>    // K32GetProcessMemoryInfo (the server's memory, each 30 s)
#else
#include <unistd.h>  // sysconf (the server's memory, each 30 s)
#include <sys/resource.h>
#include <time.h>
#endif
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
REXCVAR_DEFINE_DOUBLE(bf_server_hang_seconds, 120.0, "BF1943",
                      "A dedicated server that has not ticked for this long ends its process, so its manager starts "
                      "it again (0: never)");

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
void TeamBalanceRoundOver();    // team_balance.cpp: nothing moves between a round's end and the next level
void TeamBalanceLevelChange();  // team_balance.cpp: a new level, a new round
void TeamBalanceTick(PPCContext& ctx, uint8_t* base);  // team_balance.cpp: once a second
void BotsLevelChange();                               // bots.cpp: the old level's spawn points and bots gone
void BotsTick(PPCContext& ctx, uint8_t* base);        // bots.cpp: every tick
void ServerStatsTick();  // server_stats.cpp: each player's counts to the host's file, every 5 s
void MatchRulesRoundOver();                         // match_rules.cpp: the round's clock stops
void MatchRulesTick(PPCContext& ctx, uint8_t* base);  // match_rules.cpp: the time limit
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
  bf1943::TeamBalanceRoundOver();
  bf1943::MatchRulesRoundOver();
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
    bf1943::TeamBalanceLevelChange();
    bf1943::BotsLevelChange();
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
  bf1943::TeamBalanceLevelChange();
  bf1943::BotsLevelChange();
  WriteCurrent(next);
}

// The server's tick: the next level when the round end's delay is over and the game did not restart the level.
REX_EXTERN(__imp__sub_824123F0);
namespace bf1943 {
void ModernModeTick(uint8_t* base);  // modern_mode.cpp: the movement and aiming values for this server's mode
void GmHostStart();  // gm_host.cpp: our GameManager host, once (bf_gm_backend)
void InstallNullPageGuardOnce();  // crash_report.cpp: a server draws no frame, so the guard comes with its first tick
void TickProbeBegin();  // server_tick_probe.cpp: a long tick's stack (Linux, BF_TICK_PROBE)
void TickProbeEnd();
#if defined(BF_DEV_TOOLS)
void RunGameFixTest(PPCContext& ctx, uint8_t* base);  // game_fixes.cpp: the empty slot guard, tested once
#endif
}  // namespace bf1943
// What the calling thread has cost so far (14 Nov 2026, the 60 Hz servers' "desyncing, rubber banding": the US
// server's log had single ticks of 100 to 300 ms among ticks of half a millisecond, with nothing to say what they
// waited for): its time on the processor, and how often it stopped running, by its own choice (a wait) and because
// the system took the processor away. A tick that took long with little processor time was waiting, not working.
struct ThreadCost {
  double cpu_ms = 0;
  long waits = 0, taken = 0;  // voluntary and involuntary context switches (Linux; 0 on Windows)
};
static ThreadCost ThreadCostNow() {
  ThreadCost c;
#if defined(_WIN32)
  FILETIME made, ended, kernel, user;
  if (GetThreadTimes(GetCurrentThread(), &made, &ended, &kernel, &user)) {
    const auto ticks = [](const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    c.cpu_ms = double(ticks(kernel) + ticks(user)) / 10000.0;
  }
#else
  timespec ts{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) c.cpu_ms = double(ts.tv_sec) * 1000.0 + double(ts.tv_nsec) / 1e6;
#if defined(RUSAGE_THREAD)
  rusage u{};
  if (getrusage(RUSAGE_THREAD, &u) == 0) c.waits = u.ru_nvcsw, c.taken = u.ru_nivcsw;
#endif
#endif
  return c;
}
// The machine's processors as Linux counts them (/proc/stat's first line): all their time, and the part the host of
// a virtual machine gave to others ("steal"). False elsewhere.
static bool MachineTimes(double& total, double& stolen) {
#if defined(_WIN32)
  (void)total, (void)stolen;
  return false;
#else
  std::ifstream stat("/proc/stat");
  std::string name;
  double v[8] = {};
  if (!(stat >> name) || name != "cpu") return false;
  for (double& x : v)
    if (!(stat >> x)) return false;
  total = 0;
  for (double x : v) total += x;
  stolen = v[7];
  return true;
#endif
}

// The process's memory in MB (17 Nov 2026: servers run for days and nothing showed whether they grow): Windows' private
// bytes (what the process has committed, the guest's memory included), Linux's resident set. 0 when not known.
static double ProcessMemoryMb() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
    return double(pmc.PrivateUsage) / (1024.0 * 1024.0);
  return 0;
#else
  std::ifstream statm("/proc/self/statm");
  double size = 0, resident = 0;
  if (!(statm >> size >> resident)) return 0;
  return resident * double(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
#endif
}

// The server's own tick rate, logged every 30 s (a dedicated server only): how many ticks a second, the longest gap
// between two and the tick's own time, so a server on a slow machine shows in its log (TekRantGaming's 7 Nov 2026 play test:
// the frame rate fell on the larger maps of the official server, a Hetzner machine, and not on Wake Island).
// 14 Nov 2026: each stall is said as it happens too (a tick of 50 ms or more, or as long between two ticks), with
// what the thread cost meanwhile, and each 30 s how many there were, the machine's processors, its load and what its
// host took away.
// The server's tick watchdog (17 Nov 2026): a server that stops ticking with its process alive (a deadlock, a loop
// with no fault) keeps its players and its place in the list and nothing restarts it, since the manager and systemd
// only see a process end. A thread started with the first tick checks every 5 s; after bf_server_hang_seconds with no
// tick it logs and ends the process (a crash report with a minidump on Windows, a core dump on Linux), and the
// manager starts the server again. A level load ticks too (its longest tick on the official servers is under a second).
std::atomic<int64_t> g_last_tick_ms{0};
int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
void StartTickWatchdogOnce() {
  static const bool started = [] {
    const double limit = REXCVAR_GET(bf_server_hang_seconds);
    if (limit <= 0) return true;
    std::thread([limit] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        const int64_t seen = g_last_tick_ms.load(std::memory_order_relaxed);
        double quiet = double(SteadyMs() - seen) / 1000.0;
        if (quiet < limit) continue;
        // Still no tick 3 s later: a machine that slept or a paused virtual machine wakes this thread and the tick's
        // at once, and the tick then runs within milliseconds.
        std::this_thread::sleep_for(std::chrono::seconds(3));
        if (g_last_tick_ms.load(std::memory_order_relaxed) != seen) continue;
        quiet = double(SteadyMs() - seen) / 1000.0;
        {
          REXLOG_ERROR("BF server hang: no tick for {:.0f} s (bf_server_hang_seconds {:.0f}): ending the process so "
                       "its manager starts it again",
                       quiet, limit);
          rex::FlushLogging();
#if defined(_WIN32)
          // An exception of our own, so the crash report and its minidump (every thread's stack, the stuck tick's
          // included) are written; abort() is a fail fast that ends the process before any report.
          RaiseException(0xE0000BF1, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#endif
          std::abort();  // Linux: SIGABRT, the abort's stack in game.out and a core dump
        }
      }
    }).detach();
    REXLOG_INFO("BF server: tick watchdog on ({:.0f} s without a tick ends the process)", limit);
    return true;
  }();
  (void)started;
}

// The machine's witness (20 Nov 2026, the US Central test server, a Contabo KVM VPS): its stalls came in the first six
// seconds of every minute and said "it was working", yet with the game server stopped a plain Python loop on the same
// machine stood still at the same seconds, every processor at once, idle ones too. The host stops running the whole
// machine and tells it nothing (it gives no steal time), so the paused time counts as the tick thread's own processor
// time. A thread that only sleeps, waking every 20 ms, is held as well when the machine stands still: a stall it was
// silent through is the machine's, not the tick's. It costs 50 wakes a second.
constexpr int64_t kWitnessPeriodUs = 20000;
std::atomic<int64_t> g_witness_seen_us{0};                         // its latest wake
std::atomic<int64_t> g_witness_from_us{0}, g_witness_to_us{0};     // its latest late wake: silent from, to
std::atomic<uint32_t> g_witness_stands{0};                         // late wakes of 50 ms or more
std::atomic<int64_t> g_witness_still_us{0};                        // their time held, all of them
static int64_t SteadyUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}
static void StartMachineWitnessOnce() {
  static const bool started = [] {
    g_witness_seen_us.store(SteadyUs(), std::memory_order_relaxed);
    std::thread([] {
      auto next = Clock::now();
      for (;;) {
        next += std::chrono::microseconds(kWitnessPeriodUs);
        std::this_thread::sleep_until(next);
        const int64_t now = SteadyUs(), seen = g_witness_seen_us.load(std::memory_order_relaxed);
        const int64_t held = now - seen - kWitnessPeriodUs;
        if (held >= 30000) {
          g_witness_from_us.store(seen + kWitnessPeriodUs, std::memory_order_relaxed);
          g_witness_to_us.store(now, std::memory_order_release);  // after "from": a reader that sees it sees both
          if (held >= 50000) {
            g_witness_stands.fetch_add(1, std::memory_order_relaxed);
            g_witness_still_us.fetch_add(held, std::memory_order_relaxed);
          }
        }
        g_witness_seen_us.store(now, std::memory_order_relaxed);
        if (const auto t = Clock::now(); next < t) next = t;  // no wakes to catch up after a stand
      }
    }).detach();
    return true;
  }();
  (void)started;
}
// How much of [from, to] the witness was held through (beyond its own 20 ms): the machine stood still that long.
static double MachineStillMs(int64_t from_us, int64_t to_us) {
  const auto overlap = [&](int64_t a, int64_t b) { return std::max<int64_t>(0, std::min(b, to_us) - std::max(a, from_us)); };
  const int64_t to = g_witness_to_us.load(std::memory_order_acquire), from = g_witness_from_us.load(std::memory_order_relaxed);
  const int64_t seen = g_witness_seen_us.load(std::memory_order_relaxed);
  // Its latest late wake, and a stand it is still in (not woken yet as the tick goes on).
  return double(std::max(overlap(from, to), overlap(seen + kWitnessPeriodUs, to_us))) / 1000.0;
}

static void NoteServerTick(double work_ms, const ThreadCost& before, const ThreadCost& after) {
  bf1943::InstallNullPageGuardOnce();
  g_last_tick_ms.store(SteadyMs(), std::memory_order_relaxed);
  StartTickWatchdogOnce();
  StartMachineWitnessOnce();
  static Clock::time_point window{}, last{};
  static uint32_t ticks = 0, slow25 = 0, slow100 = 0, gaps50 = 0, said = 0, machine_stalls = 0;
  static double longest_gap = 0, work_total = 0, work_longest = 0;
  static ThreadCost last_after;
  static double machine_total = 0, machine_stolen = 0;
  static bool machine = false;
  constexpr double kStallMs = 50.0;
  constexpr uint32_t kSaidPerWindow = 20;
  const auto now = Clock::now();
  const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
  const int64_t work_us = int64_t(work_ms * 1000.0);
  // A stall the witness was held through for half its time or more: the whole machine stood still, not the thread.
  const auto still_words = [&](double stall_ms, int64_t from_us, int64_t to_us) -> std::string {
    const double still = MachineStillMs(from_us, to_us);
    if (still < stall_ms * 0.5) return {};
    ++machine_stalls;
    return fmt::format("the whole machine stood still for {:.0f} ms of it (a thread that only sleeps was held as long: "
                       "the machine's host did not run it)",
                       still);
  };
  if (last != Clock::time_point{}) {
    // From the last tick's end to this one's start: the game's own wait for the next tick, and whatever held it.
    const double gap = std::chrono::duration<double, std::milli>(now - last).count();
    longest_gap = std::max(longest_gap, gap);
    const double between = gap - work_ms;
    if (between >= kStallMs) {
      ++gaps50;
      const int64_t last_us = std::chrono::duration_cast<std::chrono::microseconds>(last.time_since_epoch()).count();
      const std::string still = still_words(between, last_us, now_us - work_us);
      if (said++ < kSaidPerWindow)
        REXLOG_WARN("BF server stall: {:.0f} ms between two ticks ({:.1f} ms of it on the processor; the thread waited {} "
                    "times and had the processor taken {} times){}{}",
                    between, before.cpu_ms - last_after.cpu_ms, before.waits - last_after.waits,
                    before.taken - last_after.taken, still.empty() ? "" : ": ", still);
    }
  } else {
    window = now;
    machine = MachineTimes(machine_total, machine_stolen);
    REXLOG_INFO("BF server: {} processors on this machine", std::thread::hardware_concurrency());
  }
  if (work_ms >= kStallMs) {
    std::string why = still_words(work_ms, now_us - work_us, now_us);
    if (why.empty()) why = after.cpu_ms - before.cpu_ms >= work_ms * 0.7 ? "it was working" : "it was waiting";
    if (said++ < kSaidPerWindow)
      REXLOG_WARN("BF server stall: a tick took {:.0f} ms, {:.1f} ms of it on the processor (the thread waited {} times "
                  "and had the processor taken {} times): {}",
                  work_ms, after.cpu_ms - before.cpu_ms, after.waits - before.waits, after.taken - before.taken, why);
  }
  last = now;
  last_after = after;
  ++ticks;
  slow25 += work_ms >= 25.0, slow100 += work_ms >= 100.0;
  work_total += work_ms;
  work_longest = std::max(work_longest, work_ms);
  const double seconds = std::chrono::duration<double>(now - window).count();
  if (seconds < 30.0) return;
  std::string host;
  double total = 0, stolen = 0;
  if (machine && MachineTimes(total, stolen) && total > machine_total) {
    host = fmt::format("; the machine's host took {:.1f}% of its processor time", 100.0 * (stolen - machine_stolen) / (total - machine_total));
    machine_total = total, machine_stolen = stolen;
  }
#if !defined(_WIN32)
  double load[3] = {};
  if (getloadavg(load, 3) >= 1) host += fmt::format(", load {:.2f}", load[0]);
#endif
  if (const double mb = ProcessMemoryMb(); mb > 0) host += fmt::format("; memory {:.0f} MB", mb);
  // The witness's stands since the last report: how often the whole machine stood still, ticks or not.
  const uint32_t stands = g_witness_stands.exchange(0, std::memory_order_relaxed);
  const double still_ms = double(g_witness_still_us.exchange(0, std::memory_order_relaxed)) / 1000.0;
  if (stands > 0 || machine_stalls > 0)
    host += fmt::format("; the whole machine stood still {} times (50 ms or more, {:.0f} ms in all), {} of the stalls in "
                        "those",
                        stands, still_ms, machine_stalls);
  REXLOG_INFO("BF server ticks: {:.1f} a second over {:.0f} s, longest gap {:.1f} ms; the tick's own work {:.2f} ms average, "
              "{:.1f} ms longest; {} ticks of 25 ms or more, {} of 100 ms or more, {} waits of 50 ms or more between ticks{}",
              ticks / seconds, seconds, longest_gap, work_total / ticks, work_longest, slow25, slow100, gaps50, host);
  window = now, ticks = slow25 = slow100 = gaps50 = said = machine_stalls = 0, longest_gap = work_total = work_longest = 0;
}

#if defined(BF_DEV_TOOLS)
namespace {
// BF_DEV_MOVE_SPAWNS=<x,y,z>[;<x,y,z>...]><x,y,z> (tests, a dedicated server): every copy in memory of a position
// given first (spawn points' positions, the level file's exact values: three floats together) becomes the last
// position, so the test's soldier spawns there (Wake Island: the carrier's spawn points moved to the airfield's flag,
// to take it and reach the air raid bunker). Looked for every 3 s in the server's first 5 minutes.
// A test aid only: it writes over every match in memory, not only the spawn points. On 16 Nov 2026 (the US carrier's 7
// moved onto the Japanese carrier) two servers crashed 24 and 26 s after a player's game was killed ("Unhandled guest
// access violation: write", sub_826230C8 in the free sub_82624D90, from the server tick); the same steps without it,
// on the same build and on 0.4.6.7, never crashed (runs killtest1 to 3, oldsrv1, drop1). Do not read a crash in a run
// that used it as the game's.
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
  static const bool near_on = [] {
    const char* v = std::getenv("BF_DEV_MOVE_SPAWNS_NEAR");
    return v && *v == '1';
  }();
  static int near_said = 0;
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
          if (x != f[0] || be(m + 4) != f[1] || be(m + 8) != f[2]) {
            // BF_DEV_MOVE_SPAWNS_NEAR=1: the scans also name every three floats within 15 m of a given position (and its height to 5 cm) (the copies
            // the game spawns from, when they are not the level file's exact values).
            if (near_on && near_said < 80) {
              float q[3];
              for (int i = 0; i < 3; ++i) {
                const uint32_t u = be(m + i * 4), t = f[i];
                float a, b;
                std::memcpy(&a, &u, 4), std::memcpy(&b, &t, 4);
                q[i] = std::abs(a - b) < (i == 1 ? 0.05f : 15.0f) ? a : NAN;
              }
              if (!std::isnan(q[0]) && !std::isnan(q[1]) && !std::isnan(q[2])) {
                ++near_said;
                REXLOG_INFO("BF dev spawns: near a given position at {:08X}: {:.4f} {:.4f} {:.4f} ({:08X} {:08X} {:08X})", uint32_t(p), q[0], q[1], q[2], be(m), be(m + 4), be(m + 8));
              }
            }
            continue;
          }
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

// BF_DEV_FIND_SPAWNS=<x>,<z>,<r> (tests, a dedicated server): once, 10 s after the first tick, every three floats in
// memory whose x and z lie within r of (x, z) and whose y lies between 150 and 300, logged with their exact values (up
// to 80): the spawn points of a base, for BF_DEV_MOVE_SPAWNS (Japan's carrier on Wake Island). Only reads.
void DevFindSpawns(uint8_t* base) {
  static float want[3] = {};
  static const bool on = [] {
    const char* v = std::getenv("BF_DEV_FIND_SPAWNS");
    return v && std::sscanf(v, "%f,%f,%f", &want[0], &want[1], &want[2]) == 3;
  }();
  static Clock::time_point first{};
  static bool done = false;
  if (!on || done) return;
  const auto now = Clock::now();
  if (first == Clock::time_point{}) first = now;
  if (now - first < std::chrono::seconds(10)) return;
  done = true;
#if defined(_WIN32)
  auto fl = [](const uint8_t* m) {
    const uint32_t w = uint32_t(m[0]) << 24 | uint32_t(m[1]) << 16 | uint32_t(m[2]) << 8 | m[3];
    float f;
    std::memcpy(&f, &w, 4);
    return f;
  };
  int found = 0;
  for (uint64_t a = 0x00100000; a < 0x100000000ull && found < 80;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) break;
    const uint64_t start = uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base);
    const uint64_t end = start + mbi.RegionSize;
    const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    if (readable && start >= 0x00100000ull && start < 0xA0000000ull)
      for (uint64_t p = (std::max<uint64_t>(start, 0x00100000ull) + 3) & ~3ull; p + 12 <= end && found < 80; p += 4) {
        const float x = fl(base + p), y = fl(base + p + 4), z = fl(base + p + 8);
        if (!(std::fabs(x - want[0]) < want[2] && std::fabs(z - want[1]) < want[2] && y > 150.0f && y < 300.0f)) continue;
        REXLOG_INFO("BF dev find spawns: {:08X}: {:.17g},{:.17g},{:.17g}", uint32_t(p), x, y, z);
        ++found;
      }
    a = end;
  }
  REXLOG_INFO("BF dev find spawns: {} near {} {} (within {} m)", found, want[0], want[1], want[2]);
#endif
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
  const bool is_server = REXCVAR_GET(server);
  const ThreadCost cost_before = is_server ? ThreadCostNow() : ThreadCost{};
  if (is_server) bf1943::TickProbeBegin();
  __imp__sub_824123F0(ctx, base);
  if (!is_server) return;
  bf1943::TickProbeEnd();
#if defined(BF_DEV_TOOLS)
  // BF_DEV_SERVER_SLOW=<ms> (tests): each server tick takes that much longer, as on a slow machine.
  static const double slow_ms = [] {
    const char* v = std::getenv("BF_DEV_SERVER_SLOW");
    return v && *v ? std::atof(v) : 0.0;
  }();
  if (slow_ms > 0) std::this_thread::sleep_for(std::chrono::microseconds(int64_t(slow_ms * 1000)));
  // BF_DEV_SERVER_HANG_AT=<s> (tests of the tick watchdog): that long after the first tick, the tick stops for good.
  static const double hang_at = [] {
    const char* v = std::getenv("BF_DEV_SERVER_HANG_AT");
    return v && *v ? std::atof(v) : -1.0;
  }();
  static const auto first_tick = Clock::now();
  if (hang_at >= 0 && std::chrono::duration<double>(Clock::now() - first_tick).count() > hang_at) {
    REXLOG_WARN("BF dev: the server's tick stops now (BF_DEV_SERVER_HANG_AT)");
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
  }
  DevMoveSpawns(base);
  DevFindSpawns(base);
  DevTickets(base);
  // The null page guard first, as in a real tick: on Linux a read of reserved guest memory gives zeros, and the
  // self test's walks then read the null page (without the guard that read faults again and again).
  bf1943::InstallNullPageGuardOnce();
  bf1943::RunGameFixTest(ctx, base);  // game_fixes.cpp: BF_DEV_FIX_TEST=1, once
#endif
  NoteServerTick(std::chrono::duration<double, std::milli>(Clock::now() - tick_start).count(), cost_before,
                 ThreadCostNow());
  bf1943::ModernModeTick(base);
  bf1943::GmHostStart();
  bf1943::ServerStatsTick();
  bf1943::TeamBalanceTick(ctx, base);  // team_balance.cpp: an uneven match evened, a player who has died at a time
  bf1943::BotsTick(ctx, base);  // bots.cpp: the bots made, spawned and driven
  bf1943::MatchRulesTick(ctx, base);  // match_rules.cpp: a round's time limit
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
  bf1943::TeamBalanceLevelChange();
  bf1943::BotsLevelChange();
  REXLOG_INFO("BF rotation: changing level to {} ({}), the pause between rounds over", TitleOf(next), next);
  WriteCurrent(next);
}
