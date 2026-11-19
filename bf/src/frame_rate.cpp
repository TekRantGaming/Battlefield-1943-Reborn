// Frame rate above 30 (TekRantGaming, 5 Nov 2026: unlimited, with no speed, animation or physics problems).
//
// The game's main loop (sub_82397278) reads the setting Game.VsyncEnable (true on Xenon, Game.cfg). With it on,
// the game timer runs in mode 2 (sub_822BBCF8): frames locked to two vertical blanks, 30 per second whatever the
// guest refresh rate (runs fps1 and fps2: 30.0 at 60 and at 120 Hz). With it off the timer runs in mode 0, a
// variable frame time, optionally limited by Game.MaxFps (sub_822C4830).
//
// The game logic runs in ticks: the timer (sub_822C7388, called twice per frame) sets the tick count at timer+48
// to elapsed time times a rate, and the game runs the ticks between +52 and +48 each frame. With Game.MaxFps 0 the
// rate is the tick rate at +44 (30 per second, correct at any frame rate: run tbunl, 30.0 ticks per second at 75 to
// 115 FPS). With a limit it is Game.MaxFps itself: at 60 the game ran 61 ticks per second, twice as fast, and the
// server pulled the soldier back (TekRantGaming's play test; run tb60). So the game's own limit is never used: Game.MaxFps
// stays 0 and the limit is ours, a wait before each present (LimitFrame, from frame_stats.cpp's VdSwap).
//
// A dedicated server keeps the game's own timing: with the unlock on the server too, clients were kicked a few
// seconds after joining (runs fps3, fps4).
//
// Settings are read through the game's registry: sub_821F3248(registry, name) returns a pointer to a bool, and
// sub_82616750(registry, name, default) a pointer to a float (big endian in guest memory).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
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
#include <windows.h>
#include <timeapi.h>
#else
#include <linux/futex.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>
#endif

REXCVAR_DEFINE_BOOL(bf_unlock_fps, true, "BF1943", "Run above the original 30 frames per second");
namespace bf1943 {
int TickRate();  // tick_rate.cpp
}
REXCVAR_DECLARE(bool, server);  // server_mode.cpp: a dedicated server keeps the game's own timing
REXCVAR_DEFINE_DOUBLE(bf_max_fps, 60.0, "BF1943", "Frame rate limit with bf_unlock_fps (0: none)");

namespace {

bool Unlocked() { return REXCVAR_GET(bf_unlock_fps) && !REXCVAR_GET(server); }

std::string GuestString(const uint8_t* base, uint32_t address) {
  std::string s;
  for (uint32_t i = 0; address && i < 96 && base[address + i]; ++i) s += char(base[address + i]);
  return s;
}

void StoreFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  base[address] = uint8_t(bits >> 24);
  base[address + 1] = uint8_t(bits >> 16);
  base[address + 2] = uint8_t(bits >> 8);
  base[address + 3] = uint8_t(bits);
}

uint32_t g_vsync = 0, g_max_fps = 0;  // guest addresses of the two settings, once the game has read them

#if defined(BF_DEV_TOOLS)
// Which settings the game reads: the shadow ones always, every name once with BF_CONFIG_PROBE=1.
bool LogSetting(const std::string& name) {
  if (name.find("hadow") != std::string::npos) return true;
  static const bool probe = [] {
    const char* v = std::getenv("BF_CONFIG_PROBE");
    return v && *v == '1';
  }();
  if (!probe) return false;
  static std::mutex mutex;
  static std::set<std::string> seen;
  std::lock_guard<std::mutex> lock(mutex);
  return seen.insert(name).second;
}
#endif

}  // namespace

// The game logic stays at 30 ticks per second. The main loop (sub_82396C48) creates its two timers
// (sub_822C7270(timer, rate, mode)) with a rate of 30; creating them at 60, on client and server alike, made the
// soldier move about twice as far per second (runs tt30, tt60): the step per tick is a fixed 1/30 s. Smooth motion
// above 30 needs the frames between ticks drawn from interpolated positions.

// Our menu's frame rate setting, live: 30 locks to the original timing, 0 is no limit.
//
// The original timing (Game.VsyncEnable on: the timer's mode 2) runs one game tick a frame. That is the Xbox's 30 ticks
// a second only while the tick rate is 30: at 60 (tick_rate.cpp) a 30 FPS player's game ran 30 ticks of a sixtieth a
// second, half time, and its own server (60 a second, in real time) pulled the soldier back every few frames (19 Nov
// 2026, the user: "a smooth and playable experience no matter the framerate"; runs fps30 and fr_30r: 29.8 ticks a
// second, 0.2 to 0.8 m a second walked against 4.5). So 30 locks only at 30 ticks a second; at any other rate it is our
// own limit at 30 with the game's timer free (mode 0, its ticks from the elapsed time: two a frame at 30 FPS).
namespace {
double g_requested_fps = -1;  // the menu's choice (SetFrameRate), for LimitFrame to apply again when the rate changes
}  // namespace
void SetFrameRate(double fps) {
  g_requested_fps = fps;
  const bool locked = fps == 30 && bf1943::TickRate() == 30;
  rex::cvar::SetFlagByName("bf_unlock_fps", locked ? "false" : "true");
  rex::cvar::SetFlagByName("bf_max_fps", std::to_string(fps));
  if (REXCVAR_GET(server)) return;
  if (g_vsync) *REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(g_vsync) = locked ? 1 : 0;
  if (g_max_fps) StoreFloat(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(g_max_fps), 0, 0.0f);
}

// Windows' timer at 1 ms for the whole process (7 Nov 2026). The runtime turns the game's Sleep(n) into ::Sleep(n)
// (xthread.cpp, Delay), and at the default 15.6 ms resolution a Sleep(1) in a job wait, or our limiter's sleep below,
// took up to 15.6 ms: run hitch1, at the players' default 60 FPS limit, had 422 frames over 20 ms in three minutes,
// most with the renderer idle. Windows 11 stops honouring a process's resolution while its window is hidden or
// covered unless it opts out of that power throttling, so it does (a background window here, Alt-Tab for a player).
namespace bf1943 {
void RaiseTimerResolution() {
#if defined(_WIN32)
#if defined(BF_DEV_TOOLS)
  if (const char* v = std::getenv("BF_TIMER_DEFAULT"); v && *v == '1') return;  // A/B tests: Windows' own timer
#endif
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
  PROCESS_POWER_THROTTLING_STATE state{};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  state.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  state.StateMask = 0;  // the control bit on, the state off: timer requests are always honoured
  // A guest Sleep(1)'s length before and after, as the log's evidence (three each, the longest).
  auto sleep_ms = [] {
    double longest = 0;
    for (int i = 0; i < 3; ++i) {
      const auto start = std::chrono::steady_clock::now();
      ::Sleep(1);
      longest = std::max(longest, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    return longest;
  };
  const double before = sleep_ms();
  const bool kept = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
  const bool raised = timeBeginPeriod(1) == TIMERR_NOERROR;
  const double after = sleep_ms();
  REXLOG_INFO("BF timer: 1 ms resolution {}{}; Sleep(1) took up to {:.1f} ms before, {:.1f} ms after",
              raised ? "set" : "refused", kept ? ", kept while the window is hidden" : "", before, after);
#endif
}
}  // namespace bf1943

namespace bf1943 {
// The time our limit waited (microseconds) since the last call, for the FPS log line (frame_stats.cpp): the frame
// time less this is what the frames took, so a capped run still shows how much room there was.
std::atomic<uint64_t> g_limit_waited_us{0};
uint64_t TakeLimitWaitedUs() { return g_limit_waited_us.exchange(0); }
}

// Our frame limit, before each present: wait until 1/bf_max_fps after the last frame (a high resolution timer, then
// a spin through the last half millisecond for even pacing). A slow frame starts a new schedule instead of rushing
// to catch up.
// When the game last presented a frame (LastGuestPresentMs): before the game starts the launcher's pages are drawn by
// the window's own loop and pace themselves (game_menu.cpp, PaceLauncher), while the game's frames pace them after.
std::atomic<int64_t> g_last_present_ms{-1000000};

// While the game's window is minimised nothing is shown, so nothing holds the frame rate (VSync waits only for frames
// it shows): measured 15 Nov 2026, Wake Island minimised, Unlimited: 251 FPS, 3.3 cores and the graphics card 99.8%
// busy at about 290 W, all for frames nobody sees. bf_minimized_fps holds it there instead. On Windows the window's
// own state is read each frame (IsIconic: any thread, about a microsecond), so a restore by any route (the taskbar,
// Alt+Tab, back to maximised) ends it at once; elsewhere the window's minimise and restore events, and a focused
// window counts as not minimised.
REXCVAR_DEFINE_DOUBLE(bf_minimized_fps, 30.0, "BF1943",
                      "Frame rate limit while the game's window is minimised (nothing shown; 0: no limit of its own)");
std::atomic<void*> g_pacing_window{nullptr};
std::atomic<bool> g_window_minimized{false};
bool WindowMinimized() {
#if defined(_WIN32)
  HWND window = static_cast<HWND>(g_pacing_window.load(std::memory_order_relaxed));
  return window && IsIconic(window);
#else
  return g_window_minimized.load(std::memory_order_relaxed);
#endif
}
namespace bf1943 {
int64_t LastGuestPresentMs() { return g_last_present_ms.load(std::memory_order_relaxed); }
void SetPacingWindow(void* native_window) { g_pacing_window.store(native_window, std::memory_order_relaxed); }
void NoteWindowMinimized(bool minimized) { g_window_minimized.store(minimized, std::memory_order_relaxed); }
}  // namespace bf1943
void LimitFrame() {
  g_last_present_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch()).count(),
                          std::memory_order_relaxed);
#if defined(BF_DEV_TOOLS) && defined(_WIN32)
  // BF_TIMER_PROBE=1: every 600 frames, how long a Sleep(1) takes now (the window in the background in tests).
  static const bool probe = [] {
    const char* v = std::getenv("BF_TIMER_PROBE");
    return v && *v == '1';
  }();
  static uint64_t probe_frames = 0;
  if (probe && ++probe_frames % 600 == 0) {
    double longest = 0;
    for (int i = 0; i < 3; ++i) {
      const auto start = std::chrono::steady_clock::now();
      ::Sleep(1);
      longest = std::max(longest, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    REXLOG_INFO("BF timer probe: frame {}: Sleep(1) took up to {:.1f} ms", probe_frames, longest);
  }
#endif
  // 30 FPS chosen and the tick rate settled after the choice (a server joined at another rate): locked or not again.
  if (g_requested_fps == 30 && !REXCVAR_GET(server) &&
      REXCVAR_GET(bf_unlock_fps) == (bf1943::TickRate() == 30)) {
    REXLOG_INFO("BF frame rate: 30 at {} ticks a second: {}", bf1943::TickRate(),
                bf1943::TickRate() == 30 ? "the original timing" : "our limit, the game's timer free");
    SetFrameRate(30);
  }
  if (!Unlocked()) return;
#if defined(BF_DEV_TOOLS)
  // BF_DEV_NO_LIMIT=1 (tests): our limit never waits (does a load go faster without it?).
  static const bool no_limit = [] {
    const char* v = std::getenv("BF_DEV_NO_LIMIT");
    return v && *v == '1';
  }();
  if (no_limit) return;
#endif
  double fps = REXCVAR_GET(bf_max_fps);
  {
    const double hidden = REXCVAR_GET(bf_minimized_fps);
    const bool minimized = hidden > 0 && WindowMinimized();
    static bool was_minimized = false;
    if (minimized != was_minimized) {
      was_minimized = minimized;
      if (minimized) {
        REXLOG_INFO("BF window minimised: {:.0f} frames a second until it is shown again", hidden);
      } else {
        REXLOG_INFO("BF window shown again: the chosen frame rate");
      }
    }
    if (minimized && (fps <= 0 || fps > hidden)) fps = hidden;
  }
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next{};
#if defined(BF_DEV_TOOLS)
  // BF_DEV_SLOW_FRAMES=<min ms>,<max ms> (tests: a slow computer): each frame at least a random time in that range after
  // the last, whatever the chosen frame rate (our own limit is then left out).
  static const std::pair<double, double> slow = [] {
    const char* v = std::getenv("BF_DEV_SLOW_FRAMES");
    if (!v || !*v) return std::pair<double, double>{0.0, 0.0};
    const double lo = std::atof(v);
    const char* comma = std::strchr(v, ',');
    return std::pair<double, double>{lo, comma ? std::max(lo, std::atof(comma + 1)) : lo};
  }();
  if (slow.first > 0) {
    static Clock::time_point slow_last{};
    static uint32_t seed = 12345;
    seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
    const double ms = slow.first + (slow.second - slow.first) * double(seed & 0xFFFF) / 65535.0;
    const auto until = slow_last + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms));
    while (Clock::now() < until) std::this_thread::sleep_for(std::chrono::microseconds(200));
    slow_last = Clock::now();
    return;
  }
#endif
  const auto now = Clock::now();
  if (fps <= 0) {
    next = now;
    return;
  }
  const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
  if (next.time_since_epoch().count() == 0 || now > next + period) next = now;
  if (now < next) {
    const auto sleep = next - now - std::chrono::microseconds(500);
    if (sleep > std::chrono::microseconds(200)) {
#if defined(_WIN32)
      // A high resolution waitable timer (Windows 10 1803 and later) wakes within about 0.1 ms whatever the timer
      // resolution; else the 1 ms resolution above.
      static thread_local HANDLE timer =
          CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
      LARGE_INTEGER due;
      due.QuadPart = -std::max<long long>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(sleep).count() / 100);
      if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
      } else {
        std::this_thread::sleep_for(sleep);
      }
#else
      std::this_thread::sleep_for(sleep);
#endif
    }
    while (Clock::now() < next) std::this_thread::yield();
    bf1943::g_limit_waited_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - now).count());
  }
  next += period;
}

// The game's job scheduler, sub_82609000(scheduler, flags): runs the oldest job it can claim and returns 1, or 0 when
// there is none. Threads waiting for jobs call it in a loop (the main thread: about 78% of its time in this morning's
// profile, sub_823C2430), so with no work it spins a core. bf_job_wait_yield: after 64 empty calls in a row on a
// thread, it gives up the rest of its time slice (SwitchToThread), so the game's workers get the core on PCs with
// few cores. Measured (runs jobyield0, jobyield1, Wake Island at 90 FPS): 2.37 cores without, 2.27 with, the same
// frame times (11.1 ms average, 11.2 ms 99%): the wait costs little, so it stayed off then.
// On by default since 14 Nov 2026 (the performance brief): on a PC with few cores the waiting threads took the cores
// from the threads with the work. The client held to two cores of this PC (Wake Island's carrier, no frame limit but
// the 90 FPS the menu offers here): without it 78 FPS, 1% low 19 to 23 FPS, 204 to 210 frames over twice the average
// (runs pb_base_wake2c2, 2c3); with it 88 to 90 FPS, 1% low 77 FPS, no frame over 50 ms (pb_yield_wake2c, 2c2),
// on less CPU (1.20 cores against 1.37). On four cores the longest frame 11.5 ms instead of 14.2 (pb_yield_wake4c,
// pb_base_wake4c); on all sixteen nothing changes (pb_yield_wake16: 90.0 FPS, 0.1% low 86). A thread with no other
// thread ready on its core gets the core straight back, so a PC with cores to spare loses nothing.
REXCVAR_DEFINE_BOOL(bf_job_wait_yield, true, "BF1943",
                    "A thread waiting for the game's jobs yields its core after 64 empty polls (fewer cores busy)");
// bf_job_wait_sleep: the main thread's own wait only (its call from sub_823C2430's loop, return address 823C24C8;
// the workers already block on events, run loadstacks1): after 64 empty polls it sleeps a quarter millisecond on a
// high resolution timer instead of spinning, so a capped frame rate leaves that core idle (at the 90 FPS cap the
// frame's work is about 3.6 ms of 11.1, the rest of the main thread's time spent polling). Measured (Wake Island,
// the carrier): at the 90 FPS cap 1.49 cores instead of 2.21 with the same frame times (11.1 ms, 99% 11.2 ms; runs
// jobsleep1, jobsleep0); without our limit 110 FPS either way, 2.46 cores instead of 3.01 (jobsleep1u, 0u). On.
REXCVAR_DEFINE_BOOL(bf_job_wait_sleep, true, "BF1943",
                    "The game's main thread sleeps 0.25 ms at a time while it waits for its jobs, not spinning");
#if defined(BF_DEV_TOOLS)
// BF_DEV_JOBWAITS=1 (dev builds): the game's waits for its jobs by caller, logged every 10 s: how many waits (runs of
// empty polls from one call site on one thread), their total and longest time, and the empty polls. A wait ends at
// a poll that ran a job, or when the thread next polls from another call site.
namespace jobwake {
std::string Stats();
}
namespace jobwaits {
using Clock = std::chrono::steady_clock;
struct Site {
  uint64_t waits = 0, polls = 0, total_us = 0, max_us = 0;
};
std::mutex g_mutex;
std::vector<std::pair<uint32_t, Site>> g_sites;
Clock::time_point g_logged{};
bool On() {
  static const bool on = [] {
    const char* v = std::getenv("BF_DEV_JOBWAITS");
    return v && *v == '1';
  }();
  return on;
}
struct Run {
  uint32_t caller = 0, polls = 0;
  Clock::time_point first{}, last{};
};
void End(Run& run) {
  if (!run.polls) return;
  const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(run.last - run.first).count());
  std::lock_guard lock(g_mutex);
  auto it = std::find_if(g_sites.begin(), g_sites.end(), [&](const auto& s) { return s.first == run.caller; });
  if (it == g_sites.end()) it = g_sites.insert(g_sites.end(), {run.caller, Site{}});
  ++it->second.waits, it->second.polls += run.polls, it->second.total_us += us;
  it->second.max_us = std::max(it->second.max_us, us);
  run.polls = 0;
  const auto now = Clock::now();
  if (g_logged == Clock::time_point{}) g_logged = now;
  const double secs = std::chrono::duration<double>(now - g_logged).count();
  if (secs < 10) return;
  std::sort(g_sites.begin(), g_sites.end(), [](const auto& a, const auto& b) { return a.second.total_us > b.second.total_us; });
  std::string line;
  for (size_t i = 0; i < g_sites.size() && i < 10; ++i) {
    const Site& s = g_sites[i].second;
    line += fmt::format("; {:08X}: {:.0f} waits/s, {:.1f} ms/s, longest {:.2f} ms, {:.0f} polls/s", g_sites[i].first,
                        s.waits / secs, s.total_us / 1000.0 / secs, s.max_us / 1000.0, s.polls / secs);
  }
  REXLOG_INFO("BF job waits over {:.0f} s{}; {}", secs, line, jobwake::Stats());
  g_sites.clear();
  g_logged = now;
}
void Poll(uint32_t caller, bool ran) {
  thread_local Run run;
  if (ran || run.caller != caller) End(run);
  if (ran) return;
  const auto now = Clock::now();
  if (!run.polls) run.caller = caller, run.first = now;
  run.last = now;
  ++run.polls;
}
}  // namespace jobwaits
#endif

// bf_job_wait_wake (with bf_job_wait_sleep): the main thread's frame wait (sub_823C2430: while the frame's job group
// at client+3248 is not done, poll the scheduler) sleeps until another thread's poll has run a job, or a quarter
// millisecond, instead of a quarter millisecond after every 64 polls. The wait ends when the frame's jobs are done,
// and the game's jobs run in these polls (its JobManager threads), so a job run is when it can have ended. Measured
// before (BF_DEV_JOBWAITS, run pr_jw1, Wake Island at the 90 FPS cap): that wait took 817 ms a second of the main
// thread's time in 102,000 polls a second, each through the scheduler's lock: the main thread's 39% of a core was
// nearly all those polls. Every wait still starts with 64 polls, so a short one never sleeps.
REXCVAR_DEFINE_BOOL(bf_job_wait_wake, true, "BF1943",
                    "The game's main thread sleeps in its frame wait until another thread has run a job");
REXCVAR_DEFINE_INT32(bf_job_wait_wake_processors, 4, "BF1943",
                     "bf_job_wait_wake only with at least this many processors (fewer: the frame wait polls)");
// bf_job_wait_sleep_unlimited (15 Nov 2026): whether the frame wait sleeps (bf_job_wait_sleep, bf_job_wait_wake) with
// no frame rate limit too. Pull request 28 made it poll then, for the most frames a second; but its polls hold the
// main thread at 95% of a core (57% of it in the scheduler's polls, run o15_prof_low) and on a processor with few
// cores they take the core the game's jobs need. Measured with no limit (Wake Island, 50 s a run): all 16 threads
// 226.5 and 224.6 FPS sleeping against 226.7 and 226.2 polling, on 3.15 cores instead of 3.76; four cores 1% low 127.8
// FPS and 12 frames over twice the average sleeping, 97.8 and 75 polling; two cores 132.9 FPS sleeping, 50.3 polling
// (runs o15_sleep_*, o15_poll_*). 1 (the default): sleep; 0: poll (pull request 28); -1: sleep only with fewer than
// bf_job_wait_sleep_unlimited_processors processors.
REXCVAR_DEFINE_INT32(bf_job_wait_sleep_unlimited, 1, "BF1943",
                     "With no frame rate limit, the main thread's frame wait sleeps (1), polls (0), or sleeps only on "
                     "processors with fewer than bf_job_wait_sleep_unlimited_processors processors (-1)");
REXCVAR_DEFINE_INT32(bf_job_wait_sleep_unlimited_processors, 8, "BF1943",
                     "bf_job_wait_sleep_unlimited -1: the frame wait sleeps with no limit on fewer processors than this");
namespace jobwake {
std::atomic<uint32_t> g_runs{0};       // jobs run by any thread's poll (the futex word on Linux)
std::atomic<bool> g_sleeping{false};  // the main thread sleeps in its frame wait
#if defined(_WIN32)
HANDLE Event() {
  static const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  return event;
}
#endif
#if defined(BF_DEV_TOOLS)
std::atomic<uint64_t> g_woken{0}, g_timed_out{0}, g_skipped{0};
#endif
void Ran() {
  g_runs.fetch_add(1);
  if (!g_sleeping.load()) return;
#if defined(_WIN32)
  SetEvent(Event());
#else
  syscall(SYS_futex, reinterpret_cast<uint32_t*>(&g_runs), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
#endif
}
// With fewer processors (bf_job_wait_wake_processors) the main thread's polls in this wait are the ones that pick up
// the frame's jobs first, while the JobManager threads wait for a core: there, the wait polls as before. Waking it
// when a job is posted as well (the job manager's wake of its workers, sub_826070B8, about 19,000 a second) did not
// help: on two cores still 13 to 22 frames over twice the average against 1 to 6 polling (runs pr_post2c_*).
// The processors this process may run on (its affinity), read again at most once a second.
uint32_t Processors() {
  using Clock = std::chrono::steady_clock;
  static std::atomic<uint32_t> count{0};
  static std::atomic<int64_t> read_at{0};
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
  if (count.load(std::memory_order_relaxed) && now - read_at.load(std::memory_order_relaxed) < 1000)
    return count.load(std::memory_order_relaxed);
  read_at.store(now, std::memory_order_relaxed);
  uint32_t n = 0;
#if defined(_WIN32)
  DWORD_PTR process = 0, system = 0;
  if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system))
    for (; process; process &= process - 1) ++n;
#else
  cpu_set_t set;
  if (sched_getaffinity(0, sizeof(set), &set) == 0) n = uint32_t(CPU_COUNT(&set));
#endif
  if (!n) n = std::max(1u, std::thread::hardware_concurrency());
  count.store(n, std::memory_order_relaxed);
  return n;
}
// Sleeps unless a job was run since `before` (read before this thread's poll); false when the wait could not be made.
bool Sleep(uint32_t before) {
  g_sleeping.store(true);
  if (g_runs.load() != before) {
    g_sleeping.store(false);
#if defined(BF_DEV_TOOLS)
    ++g_skipped;
#endif
    return true;
  }
  bool woken = false, ok = false;
#if defined(_WIN32)
  static thread_local HANDLE timer =
      CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  LARGE_INTEGER due;
  due.QuadPart = -2500;  // 0.25 ms, in 100 ns units
  if (timer && Event() && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
    const HANDLE handles[2] = {Event(), timer};
    const DWORD r = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    woken = r == WAIT_OBJECT_0, ok = r == WAIT_OBJECT_0 || r == WAIT_OBJECT_0 + 1;
  }
#else
  const timespec timeout{0, 250000};
  const long r = syscall(SYS_futex, reinterpret_cast<uint32_t*>(&g_runs), FUTEX_WAIT_PRIVATE, before, &timeout,
                         nullptr, 0);
  woken = r == 0 || errno == EAGAIN, ok = r == 0 || errno == EAGAIN || errno == ETIMEDOUT || errno == EINTR;
#endif
  g_sleeping.store(false);
#if defined(BF_DEV_TOOLS)
  if (ok) ++(woken ? g_woken : g_timed_out);
#endif
  return ok;
}
#if defined(BF_DEV_TOOLS)
std::string Stats() {
  return fmt::format("the frame wait's sleeps: {} woken by a job, {} timed out, {} not made (a job had run)",
                     g_woken.exchange(0), g_timed_out.exchange(0), g_skipped.exchange(0));
}
#endif
}  // namespace jobwake

REX_EXTERN(__imp__sub_82609000);
REX_HOOK_RAW(sub_82609000) {
  const uint32_t caller = uint32_t(ctx.lr);
  const bool frame_wait = caller == 0x823C24C8u;
  const bool wake = REXCVAR_GET(bf_job_wait_wake) && REXCVAR_GET(bf_job_wait_sleep);
  const uint32_t runs_before = wake && frame_wait ? jobwake::g_runs.load() : 0;
  __imp__sub_82609000(ctx, base);
  const bool ran = ctx.r3.u32 & 0xFF;
  if (wake && ran) jobwake::Ran();
#if defined(BF_DEV_TOOLS)
  if (jobwaits::On()) jobwaits::Poll(caller, ran);
#endif
  // With a frame rate limit; with none, as bf_job_wait_sleep_unlimited says (DiXN, pull request 28: it polled then).
  const int unlimited = REXCVAR_GET(bf_job_wait_sleep_unlimited);
  const bool limited = !Unlocked() || REXCVAR_GET(bf_max_fps) > 0;
  const bool sleep = REXCVAR_GET(bf_job_wait_sleep) && frame_wait &&
                     (limited || unlimited > 0 ||
                      (unlimited < 0 && jobwake::Processors() <
                                            uint32_t(std::max(1, REXCVAR_GET(bf_job_wait_sleep_unlimited_processors)))));
  if (!sleep && !REXCVAR_GET(bf_job_wait_yield)) return;
  // `empty` counts a thread's empty polls from any call site, as it always has: counting them per call site instead
  // (reset when the site changed) made the waiting threads yield less, and on two cores the 1% low fell from 83 to
  // 40 to 74 FPS (runs pr_nowake2c_*, 14 Nov 2026). The frame wait's own count is kept apart.
  thread_local uint32_t empty = 0, frame_wait_polls = 0;
  if (ran) {
    empty = 0, frame_wait_polls = 0;
    return;
  }
  if (!frame_wait) frame_wait_polls = 0;
  if (sleep && wake && jobwake::Processors() >= uint32_t(std::max(1, REXCVAR_GET(bf_job_wait_wake_processors)))) {
    // After the wait's first 64 polls: one poll per sleep, each until a job was run or 0.25 ms.
    if (frame_wait_polls < 64) {
      ++frame_wait_polls;
      return;
    }
    if (jobwake::Sleep(runs_before)) return;
  }
  if (++empty < 64) return;
  empty = 0;
  if (sleep) {
#if defined(_WIN32)
    static thread_local HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -2500;  // 0.25 ms, in 100 ns units
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
      return;
    }
#endif
    std::this_thread::sleep_for(std::chrono::microseconds(250));
    return;
  }
  std::this_thread::yield();
}

// The game's Direct3D frame throttle (sub_821659B0: wait until the GPU has finished an earlier frame) polls
// sub_8216B640 in a loop (from 82165A6C), and that function is itself a delay loop (bdnz, then a time check): the
// game's render thread (JobManager3) spun 12.5% of its time there while it waited for our renderer (profile
// o15_prof_low, 15 Nov 2026), and on a processor with few cores that spin holds a core the renderer needs to catch
// up. bf_d3d_throttle_wait, from the 32nd poll of one wait on: 0 the game's own spin, 1 give the core to another
// thread that is ready (SwitchToThread; none ready: back at once), 2 sleep (asked for 0.1 ms; on Windows a whole
// millisecond of the 1 ms timer), -1 (the default) sleep with fewer than 4 processors, yield with more.
// Measured with no frame limit (Wake Island, 50 s a run, runs o15_thr*): two cores 131 FPS spinning (1% low 28, 252
// frames over twice the average), 191 yielding (61, 81), 198 to 204 sleeping (90 to 104, 11 to 42); four cores 228
// spinning or yielding (1% low 135), 219 sleeping (1% low 78, a 135 ms frame); all 16 threads 228 spinning (1% low
// 155), 226 to 230 yielding (154 to 172), 223 to 230 sleeping (142 to 145). Two cores at 90 FPS: 1% low 73.5 spinning,
// 84 yielding or sleeping.
REXCVAR_DEFINE_INT32(bf_d3d_throttle_wait, -1, "BF1943",
                     "Direct3D's frame throttle while it waits for the GPU: 0 spins (the game's own), 1 yields its core "
                     "after 32 polls, 2 sleeps between polls, -1 sleeps with fewer than 4 processors and yields with "
                     "more");
REX_EXTERN(__imp__sub_8216B640);
REX_HOOK_RAW(sub_8216B640) {
  if (uint32_t(ctx.lr) == 0x82165A70u) {
    using Clock = std::chrono::steady_clock;
    thread_local uint32_t polls = 0;
    thread_local Clock::time_point last{};
    const auto now = Clock::now();
    if (now - last > std::chrono::milliseconds(2)) polls = 0;  // a new wait
    last = now;
    int mode = REXCVAR_GET(bf_d3d_throttle_wait);
    if (mode < 0) mode = jobwake::Processors() < 4 ? 2 : 1;
    if (mode > 0 && ++polls > 32) {
      if (mode == 1) {
#if defined(_WIN32)
        SwitchToThread();
#else
        sched_yield();
#endif
      } else {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }
  }
  __imp__sub_8216B640(ctx, base);
}

REX_EXTERN(__imp__sub_821F3248);
REX_HOOK_RAW(sub_821F3248) {
  const std::string name = GuestString(base, ctx.r4.u32);
  __imp__sub_821F3248(ctx, base);
#if defined(BF_DEV_TOOLS)
  if (ctx.r3.u32 && LogSetting(name)) REXLOG_INFO("BF setting {} = {}", name, base[ctx.r3.u32]);
#endif
  if (ctx.r3.u32 && name == "Game.VsyncEnable") g_vsync = ctx.r3.u32;
  if (Unlocked() && ctx.r3.u32 && name == "Game.VsyncEnable") {
    base[ctx.r3.u32] = 0;
    REXLOG_INFO("BF frame rate: Game.VsyncEnable off (frame rate unlocked)");
  }
}

// The level of detail's distance ("double the LOD distance, and provide a graphics slider from 1.00x to 2.00x with
// 0.25 steps"; "I meant for all objects, terrain, and vegetation at distance"). The game's mesh settings object (class
// 0x82040568, found by its layout: run names6) has GlobalLodScale at +0x10, 1 as the game runs: every mesh's distance
// to the camera is multiplied by it before its level of detail is chosen, so 0.5 keeps each model's detailed mesh,
// and the model itself, twice as far (run glod1: at 30 the island's trees and buildings are gone from the carrier, at
// 0.03 more of them show). It covers everything drawn as a mesh: objects, buildings, vehicles, trees, bushes, rocks.
// A worker finds the object once a level is loaded and writes 1 / bf_lod_distance; the setting can change at any
// time. Not on a dedicated server (it draws nothing).
// (Tried before: Render.EdgeModelLodScale is the PlayStation 3's and does nothing here, runs lod1 to lod5; each
// model's own Render.MeshLodSettingsAsset distances worked too, run lod9, but took a search of the heap every few
// seconds for what this does in one place.)
// "continue with 8x instead of 10x": 1x to 8x in whole steps (the 1.5 steps asked for with 10x do not land on 8).
// The game's own cut-offs are 1000 m for soldiers, vehicles and the carriers, 500 to 643 m for larger objects, 250 to
// 350 m for buildings, palm trees and boulders (their lodsettings files); the map is 2048 m across, so 2x carries the
// first across it, 4x the second, and 8x the buildings and trees. 2 by default (the doubling first asked for).
REXCVAR_DEFINE_DOUBLE(bf_lod_distance, 2.0, "BF1943", "How far models keep their detail, 1.0 (the game's own) to 8.0 times as far");
REXCVAR_DEFINE_DOUBLE(bf_dev_lod_scale, 0.0, "BF1943", "Developer: the level of detail's distance times this instead (0: not)");
// (19 Nov 2026, D202: as the game runs, MaxBillboardCount 8192, BillboardScale 1, ShadowMaxLod 0, LodBlendOverlap 1.
// BillboardScale is the size of the flat pictures far trees are drawn as, not where they switch: at 4 the far forests
// stood four times as tall, at 0.25 a quarter, runs lod_bb4, lod_bb025. Only GlobalLodScale moves the switch: at 8x the
// far forests of Guadalcanal are drawn as models, at 4x and less still as pictures, runs lod_g*.)
namespace bf1943 {
bool LevelLoaded();  // auto_join.cpp
}
namespace {
std::atomic<uint8_t*> g_guest_base{nullptr};
#if defined(_WIN32)
constexpr uint32_t kMeshSettingsClass = 0x82040568;
uint32_t Load32Host(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
float LoadFloatHost(const uint8_t* p) {
  const uint32_t bits = Load32Host(p);
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}
void StoreFloatHost(uint8_t* p, float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  p[0] = uint8_t(bits >> 24), p[1] = uint8_t(bits >> 16), p[2] = uint8_t(bits >> 8), p[3] = uint8_t(bits);
}
// The mesh settings object: its class at +0, and its members within reason (GlobalLodScale +0x10, MaxBillboardCount
// +0x18, BillboardScale +0x1C, ShadowMaxLod +0x4C, LodBlendOverlap +0x5C, eight flags from +0x69).
bool IsMeshSettings(const uint8_t* p) {
  if (Load32Host(p) != kMeshSettingsClass) return false;
  const float lod = LoadFloatHost(p + 0x10), billboard = LoadFloatHost(p + 0x1C), overlap = LoadFloatHost(p + 0x5C);
  if (!(lod >= 0.01f && lod <= 100.0f) || !(billboard >= 0.01f && billboard <= 100.0f) || !(overlap >= 0.0f && overlap <= 1000.0f)) return false;
  const uint32_t billboards = Load32Host(p + 0x18);
  if (billboards == 0 || billboards > 100000 || Load32Host(p + 0x4C) > 8) return false;
  for (int i = 0x69; i <= 0x70; ++i)
    if (p[i] > 1) return false;
  return true;
}
uint8_t* FindMeshSettings(uint8_t* base) {
  for (uint64_t at = 0x10000; at < 0xFFFF0000ull;) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(base + at, &info, sizeof(info))) break;
    const uint64_t region = uint64_t(static_cast<uint8_t*>(info.BaseAddress) - base);
    const uint64_t end = std::min<uint64_t>(region + info.RegionSize, 0xFFFF0000ull);
    if (info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE)
      for (uint64_t a = (std::max(at, region) + 3) & ~3ull; a + 0x80 <= end; a += 4)
        if (base[a] == 0x82 && IsMeshSettings(base + a)) return base + a;
    if (end <= at) break;
    at = end;
  }
  return nullptr;
}
void LodWorker() {
  uint8_t* settings = nullptr;
  double applied = -1;
  int misses = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
    if (!base || !bf1943::LevelLoaded()) continue;
    if (settings && !IsMeshSettings(settings)) settings = nullptr, applied = -1;
    if (!settings) {
      if (misses > 0 && misses++ % 10) continue;  // not found before: looked for again every 5 s
      const auto t0 = std::chrono::steady_clock::now();
      settings = FindMeshSettings(base);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (!settings) {
        if (!misses++) REXLOG_WARN("BF level of detail: the game's mesh settings were not found ({:.0f} ms); looked for again", ms);
        continue;
      }
      misses = 0;
      REXLOG_INFO("BF level of detail: the game's mesh settings at {:08X} (GlobalLodScale {}, MaxBillboardCount {}, "
                  "BillboardScale {}, ShadowMaxLod {}, LodBlendOverlap {}; found in {:.0f} ms)",
                  uint32_t(settings - base), LoadFloatHost(settings + 0x10), Load32Host(settings + 0x18),
                  LoadFloatHost(settings + 0x1C), Load32Host(settings + 0x4C), LoadFloatHost(settings + 0x5C), ms);
    }
    const double dev = REXCVAR_GET(bf_dev_lod_scale);
    const double factor = dev > 0 ? dev : std::clamp(REXCVAR_GET(bf_lod_distance), 1.0, 8.0);
    StoreFloatHost(settings + 0x10, float(1.0 / factor));  // each half second: the game may put its own back
    if (factor != applied) {
      applied = factor;
      REXLOG_INFO("BF level of detail: distance {:.2f} times the game's (GlobalLodScale {:.3f})", factor, 1.0 / factor);
    }
  }
}
#endif
}  // namespace
// The terrain's own level of detail and the game's undergrowth are not covered yet. Their settings are known by name
// and layout from the program's member tables (the terrain settings: GeometryLodScale a float at +0x80,
// UndergrowthDensityScale +0x3C, UndergrowthPoplessDistanceRatio +0x18, its flags from +0x90 to +0xCA; the view
// distances: NormalViewDistance +0x20, TerrainViewDistance +0x24), but the live terrain settings object was not found
// by that layout in three searches of the game's memory (runs names6, glod1, glod2).
#if defined(BF_DEV_TOOLS) && defined(_WIN32)
namespace {
// BF_DEV_LIGHTS (research, 14 Nov 2026: "find the lights"): the game's weather system holds its lighting as data
// (Weather.DynamicOutdoorLightData: KeyColor, TopColor, BottomColor and their intensities, Direction;
// Weather.DynamicSunData; Weather.DynamicFogData: the level's weather state files). 25 s into a level the game's
// memory is searched for a run of big endian words, and each place found is logged with the words around it.
//   BF_DEV_LIGHTS=<hex words>            the words looked for, e.g. Wake Island's KeyColor 3F800000,3F789375,3F69FBE7
//   BF_DEV_LIGHTS_SET=<offset>:<hex words>  then written at every place found, <offset> bytes from it, four times a
//                                        second (which copy the game draws with shows on the screen)
}  // namespace
namespace bf1943::dev {
void WatchHostAccess(std::vector<const void*> addresses, double seconds, const char* label);  // dev_watch.cpp
}
namespace {
std::vector<uint32_t> HexWords(const char* v) {
  std::vector<uint32_t> out;
  for (const char* q = v; q && *q;) {
    char* e = nullptr;
    const unsigned long w = std::strtoul(q, &e, 16);
    if (e == q) break;
    out.push_back(uint32_t(w));
    q = *e == ',' ? e + 1 : e;
  }
  return out;
}
void DevLightsWorker() {
  const char* v = std::getenv("BF_DEV_LIGHTS");
  if (!v || !*v) return;
  const std::vector<uint32_t> want = HexWords(v);
  if (want.empty()) return;
  int set_offset = 0;
  std::vector<uint32_t> set;
  if (const char* s = std::getenv("BF_DEV_LIGHTS_SET"); s && *s) {
    char* e = nullptr;
    set_offset = int(std::strtol(s, &e, 0));
    if (e && *e == ':') set = HexWords(e + 1);
  }
  // BF_DEV_LIGHTS_EARLY=<seconds>: from the start, the game's writable memory is searched over and over for the words;
  // the moment they are there (the level's data just loaded) the place is watched for that long: who takes the data.
  if (const char* early = std::getenv("BF_DEV_LIGHTS_EARLY"); early && *early) {
    const double seconds = std::atof(early);
    for (;;) {
      uint8_t* b = g_guest_base.load(std::memory_order_relaxed);
      if (!b) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      for (uint64_t at = 0x10000; at < 0x40000000ull;) {  // the game's heaps are low (the data was at 1FEE7800)
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(b + at, &info, sizeof(info))) break;
        const uint64_t region = uint64_t(static_cast<uint8_t*>(info.BaseAddress) - b);
        const uint64_t end = std::min<uint64_t>(region + info.RegionSize, 0x40000000ull);
        if (info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE) {
          for (uint64_t a = (std::max(at, region) + 3) & ~3ull; a + want.size() * 4 <= end; a += 4) {
            if (b[a] != uint8_t(want[0] >> 24) || Load32Host(b + a) != want[0]) continue;
            bool same = true;
            for (size_t i = 1; same && i < want.size(); ++i) same = Load32Host(b + a + i * 4) == want[i];
            if (!same) continue;
            REXLOG_INFO("BF dev lights: early: the words at {:08X}; watched for {:.0f} s", uint32_t(a), seconds);
            bf1943::dev::WatchHostAccess({b + a, b + a - 16, b + a - 32, b + a + 16}, seconds, "the outdoor light's data, from its load");
            return;
          }
        }
        if (end <= at) break;
        at = end;
      }
    }
  }
  uint8_t* base = nullptr;
  for (int waited = 0;; ++waited) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    base = g_guest_base.load(std::memory_order_relaxed);
    if (!base || !bf1943::LevelLoaded()) {
      waited = 0;
      continue;
    }
    static const int wait_ticks = [] {
      const char* s = std::getenv("BF_DEV_LIGHTS_WAIT");
      return s && *s ? std::max(1, int(std::atof(s) * 2.0)) : 50;
    }();
    if (waited >= wait_ticks) break;
  }
  // BF_DEV_LIGHTS_RATIO=<g/r>,<b/r>[;<g/r>,<b/r>...]: every three floats together in writable memory whose
  // proportions are one of those within 0.4% (a colour the game keeps scaled or converted), logged, 300 at the most.
  if (const char* r = std::getenv("BF_DEV_LIGHTS_RATIO"); r && *r) {
    std::vector<std::pair<float, float>> ratios;
    for (const char* q = r; *q;) {
      char* e = nullptr;
      const float a = std::strtof(q, &e);
      if (e == q || *e != ',') break;
      const float b = std::strtof(e + 1, &e);
      ratios.emplace_back(a, b);
      q = *e == ';' ? e + 1 : e;
      if (*e != ';') break;
    }
    int logged = 0;
    for (uint64_t at = 0x10000; at < 0xFFFF0000ull && logged < 300;) {
      MEMORY_BASIC_INFORMATION info{};
      if (!VirtualQuery(base + at, &info, sizeof(info))) break;
      const uint64_t region = uint64_t(static_cast<uint8_t*>(info.BaseAddress) - base);
      const uint64_t end = std::min<uint64_t>(region + info.RegionSize, 0xFFFF0000ull);
      if (info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE) {
        for (uint64_t a = (std::max(at, region) + 3) & ~3ull; a + 12 <= end && logged < 300; a += 4) {
          const uint8_t top = base[a];
          if (top < 0x3A || top > 0x44) continue;  // a float from about 0.0005 to 1000
          const float x = LoadFloatHost(base + a), y = LoadFloatHost(base + a + 4), z = LoadFloatHost(base + a + 8);
          if (!(x > 0.0f) || !(y > 0.0f) || !(z > 0.0f)) continue;
          for (size_t i = 0; i < ratios.size(); ++i) {
            if (std::abs(y / x - ratios[i].first) > ratios[i].first * 0.004f ||
                std::abs(z / x - ratios[i].second) > ratios[i].second * 0.004f) {
              continue;
            }
            ++logged;
            std::string words;
            for (int o = -0x10; o < 0x30; o += 4)
              if (a + o >= region && a + o + 4 <= end) words += fmt::format("{}{:08X}", o == 0 ? " | " : " ", Load32Host(base + a + o));
            REXLOG_INFO("BF dev lights: proportion {} at {:08X}: {:.5f} {:.5f} {:.5f}; from -10:{}", i, uint32_t(a), x, y, z, words);
          }
        }
      }
      if (end <= at) break;
      at = end;
    }
    REXLOG_INFO("BF dev lights: {} places with those proportions", logged);
  }
  std::vector<uint32_t> found;
  for (uint64_t at = 0x10000; at < 0xFFFF0000ull;) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(base + at, &info, sizeof(info))) break;
    const uint64_t region = uint64_t(static_cast<uint8_t*>(info.BaseAddress) - base);
    const uint64_t end = std::min<uint64_t>(region + info.RegionSize, 0xFFFF0000ull);
    if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && info.Protect != 0) {
      for (uint64_t a = (std::max(at, region) + 3) & ~3ull; a + want.size() * 4 <= end; a += 4) {
        bool same = true;
        for (size_t i = 0; same && i < want.size(); ++i) same = Load32Host(base + a + i * 4) == want[i];
        if (!same) continue;
        if (found.size() < 64) found.push_back(uint32_t(a));
        std::string words;
        for (int o = -0x20; o < 0x80; o += 4)
          if (a + o >= region && a + o + 4 <= end) words += fmt::format("{}{:08X}", o == 0 ? " | " : " ", Load32Host(base + a + o));
        REXLOG_INFO("BF dev lights: found at {:08X} ({}): from -20:{}", uint32_t(a),
                    info.Protect == PAGE_READWRITE ? "writable" : "read only", words);
      }
    }
    if (end <= at) break;
    at = end;
  }
  REXLOG_INFO("BF dev lights: {} places hold the words", found.size());
  // BF_DEV_LIGHTS_WATCH=<seconds>[:<offset>,<offset>...]: who reads or writes the first place found (and up to three
  // offsets from it) for that long: the code that takes the light's data, by function (dev_watch.cpp).
  if (const char* w = std::getenv("BF_DEV_LIGHTS_WATCH"); w && *w && !found.empty()) {
    char* e = nullptr;
    const double seconds = std::strtod(w, &e);
    std::vector<const void*> at{base + found[0]};
    while (e && (*e == ':' || *e == ',') && at.size() < 4) at.push_back(base + found[0] + std::strtol(e + 1, &e, 0));
    REXLOG_INFO("BF dev lights: watching {} addresses from {:08X} for {:.0f} s", at.size(), found[0], seconds);
    bf1943::dev::WatchHostAccess(at, seconds, "the outdoor light's data");
  }
  if (set.empty() || found.empty()) return;
  REXLOG_INFO("BF dev lights: writing {} words at +{} of each, four times a second", set.size(), set_offset);
  for (;;) {
    for (uint32_t a : found) {
      MEMORY_BASIC_INFORMATION info{};
      uint8_t* p = base + a + set_offset;
      if (!VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE) continue;
      for (size_t i = 0; i < set.size(); ++i) {
        const uint32_t w = set[i];
        p[i * 4] = uint8_t(w >> 24), p[i * 4 + 1] = uint8_t(w >> 16), p[i * 4 + 2] = uint8_t(w >> 8), p[i * 4 + 3] = uint8_t(w);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
}
}  // namespace
#endif

namespace bf1943 {
// From the menu's frame: the worker started once the game's memory is known.
void ApplyLodDistance() {
#if defined(_WIN32)
  static bool started = false;
  if (!started && g_guest_base.load(std::memory_order_relaxed) && !REXCVAR_GET(server)) {
    started = true;
    std::thread(LodWorker).detach();
#if defined(BF_DEV_TOOLS)
    std::thread(DevLightsWorker).detach();
#endif
  }
#endif
}
}  // namespace bf1943
REX_EXTERN(__imp__sub_82616750);
REX_HOOK_RAW(sub_82616750) {
  const std::string name = GuestString(base, ctx.r4.u32);
  __imp__sub_82616750(ctx, base);
  g_guest_base.store(base, std::memory_order_relaxed);
  // Every setting the game asks for by name, once each with its value (13 Nov 2026: which of them are the level of
  // detail's and the view distance's is not known yet; the log's list says). Dev builds only (600 lines).
#if defined(BF_DEV_TOOLS)
  if (ctx.r3.u32 && !name.empty()) {
    static std::mutex names_mutex;
    static std::set<std::string> names;
    std::lock_guard lock(names_mutex);
    if (names.size() < 600 && names.insert(name).second) {
      const uint32_t bits = uint32_t(base[ctx.r3.u32]) << 24 | base[ctx.r3.u32 + 1] << 16 | base[ctx.r3.u32 + 2] << 8 |
                            base[ctx.r3.u32 + 3];
      float value;
      std::memcpy(&value, &bits, 4);
      REXLOG_INFO("BF game setting: {} = {} (bits {:08X})", name, value, bits);
    }
  }
#endif
#if defined(BF_DEV_TOOLS)
  if (ctx.r3.u32 && LogSetting(name)) {
    uint32_t bits = uint32_t(base[ctx.r3.u32]) << 24 | base[ctx.r3.u32 + 1] << 16 | base[ctx.r3.u32 + 2] << 8 |
                    base[ctx.r3.u32 + 3];
    float value;
    std::memcpy(&value, &bits, 4);
    REXLOG_INFO("BF setting {} = {}", name, value);
  }
#endif
#if defined(BF_DEV_TOOLS)
  // BF_DEV_MAX_PLAYERS=<n> (tests, 15 Nov 2026): a dedicated server's Game.MaxPlayerCount (64 by default; the official
  // servers' lists say 24) set to n, to see what the game does with a join over its cap.
  if (REXCVAR_GET(server) && ctx.r3.u32 && name == "Game.MaxPlayerCount") {
    if (const char* v = std::getenv("BF_DEV_MAX_PLAYERS"); v && *v) {
      const uint32_t n = uint32_t(std::atoi(v));
      base[ctx.r3.u32] = uint8_t(n >> 24), base[ctx.r3.u32 + 1] = uint8_t(n >> 16), base[ctx.r3.u32 + 2] = uint8_t(n >> 8),
      base[ctx.r3.u32 + 3] = uint8_t(n);
      REXLOG_INFO("BF test: the server's Game.MaxPlayerCount set to {}", n);
    }
  }
#endif
  if (ctx.r3.u32 && name == "Game.MaxFps") g_max_fps = ctx.r3.u32;
  // A dedicated server's main timer ticks once a frame, paced by Game.MaxFps (60 Hz research, tick_rate.cpp): at
  // another rate than 30 it is paced at that rate, so its ticks a second match its players'.
  if (REXCVAR_GET(server) && ctx.r3.u32 && name == "Game.MaxFps" && bf1943::TickRate() != 30) {
    uint32_t bits = uint32_t(base[ctx.r3.u32]) << 24 | base[ctx.r3.u32 + 1] << 16 | base[ctx.r3.u32 + 2] << 8 |
                    base[ctx.r3.u32 + 3];
    float was;
    std::memcpy(&was, &bits, 4);
    StoreFloat(base, ctx.r3.u32, float(bf1943::TickRate()));
    REXLOG_INFO("BF tick rate: the server's Game.MaxFps {} -> {}", was, bf1943::TickRate());
  }
  if (Unlocked() && ctx.r3.u32 && name == "Game.MaxFps") {
    // 0: the game's timer keeps its 30 ticks per second; the limit is LimitFrame's.
    StoreFloat(base, ctx.r3.u32, 0.0f);
    REXLOG_INFO("BF frame rate: Game.MaxFps 0, our limit {}", REXCVAR_GET(bf_max_fps));
  }
}

// The Flash UI (HUD, menus) above 30. The UI's update (sub_826B6698(ui, frame time)) advances the Flash player one
// movie frame per game frame: sub_82954388(player, 33, 1) into sub_82991B20, which counts one frame. The movies are
// made for 30 frames per second, so at 60 the HUD's animations ran twice as fast. Here the player advances on a
// 30 Hz clock instead: some frames skip the call, a slow frame makes up one step at most. The UI still draws every
// frame. With the frame rate locked to 30 every call goes through.
REXCVAR_DEFINE_BOOL(bf_ui_30hz, true, "BF1943", "Advance the Flash UI at 30 frames per second at any frame rate");

REX_EXTERN(__imp__sub_82954388);
REX_HOOK_RAW(sub_82954388) {
  if (!Unlocked() || !REXCVAR_GET(bf_ui_30hz) || ctx.r4.u32 != 33) return __imp__sub_82954388(ctx, base);
  using Clock = std::chrono::steady_clock;
  constexpr double kStep = 1.0 / 30;
  static Clock::time_point last = Clock::now();
  static double owed = kStep;  // seconds of UI time not advanced yet
  const Clock::time_point now = Clock::now();
  owed += std::min(std::chrono::duration<double>(now - last).count(), 0.25);
  last = now;
#if defined(BF_DEV_TOOLS)
  static int frames = 0, advances = 0;
  static Clock::time_point counted = now;
  ++frames;
  if (now - counted >= std::chrono::seconds(5)) {
    REXLOG_INFO("BF UI clock: {} game frames, {} Flash advances in 5 s", frames, advances);
    frames = advances = 0;
    counted = now;
  }
#endif
  if (owed < kStep) return;
  PPCContext saved = ctx;
  for (int steps = 0; owed >= kStep && steps < 2; ++steps) {
    if (steps) ctx = saved;
    __imp__sub_82954388(ctx, base);
    owed -= kStep;
#if defined(BF_DEV_TOOLS)
    ++advances;
#endif
  }
  owed = std::min(owed, kStep);
}

// Physics in time with the game's tick (TekRantGaming: "physics is in slow motion. When I broke a tree, it came down very
// slowly. When I blew up a building, the rubble moved slowly"). sub_8253EDF8(world, tick time f1, frame time f2) runs
// once a game tick (30 a second, more when it catches up): its own steps take the tick's 1/30 s, but a second system
// in it (sub_82537AC8, its time smoothed toward f2) took the frame's time, so above 30 FPS it advanced 9 ms a tick at
// 110 FPS instead of 33 (runs args110, args30). On the Xbox the two are equal: the tick's time goes in both.
REXCVAR_DEFINE_BOOL(bf_physics_tick_time, true, "BF1943",
                    "Physics effects (debris, falling trees) at the right speed above 30 FPS: the game tick's time for "
                    "the tick's second physics step, as at 30 FPS");
REX_EXTERN(__imp__sub_8253EDF8);
REX_HOOK_RAW(sub_8253EDF8) {
  if (REXCVAR_GET(bf_physics_tick_time) && ctx.f1.f64 > 0.0) ctx.f2.f64 = ctx.f1.f64;
  const uint32_t world = ctx.r3.u32;
  __imp__sub_8253EDF8(ctx, base);
#if defined(BF_DEV_TOOLS)
  // BF_PHYS_LOG=1: every 5 s, the smoothed time the second system stepped with (world + 0xA32C).
  static const bool log = std::getenv("BF_PHYS_LOG") != nullptr;
  if (log) {
    static auto last = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - last > std::chrono::seconds(5)) {
      last = std::chrono::steady_clock::now();
      const uint8_t* p = base + world + 0xA32C;
      const uint32_t u = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
      float v;
      std::memcpy(&v, &u, 4);
      REXLOG_INFO("BF physics: second step time {:.6f}", v);
    }
  }
#endif
}
