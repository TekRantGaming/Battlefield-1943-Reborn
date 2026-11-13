// A sampling CPU profiler for Linux test runs (dev builds only, BF_DEV_PROFILE), 13 Nov 2026: perf could not open
// any event on the ROG Ally from a user's container (perf_event_paranoid), and eu-stack froze the game for 10 s a
// snapshot. This one needs no permission and changes nothing on the machine.
//
//   BF_DEV_PROFILE=<start s>,<seconds>[,<hz>]   from that many seconds after the start, for that long (default 499 Hz)
//   BF_DEV_PROFILE_OUT=<file>                   where the samples go (default: bf1943-profile.txt in the working dir)
//
// Every thread that exists when it starts gets a timer on its own CPU time clock (timer_create on the thread's CPU
// clock, SIGEV_THREAD_ID), so a thread is sampled only while it runs: a blocked thread costs nothing and is never
// woken by the signal. The handler stores the interrupted instruction pointer and the thread; at the end the samples
// are counted by (thread, address) and written with the process's mappings, so they can be named offline (each
// address as module + offset; the module's own symbol table, eu-addr2line or llvm-symbolizer).
#if defined(__linux__) && defined(BF_DEV_TOOLS)

#include <dirent.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unwind.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/logging.h>

namespace {
constexpr size_t kMaxSamples = 1u << 19;  // about 520,000: 30 s at 499 Hz on 35 busy threads
constexpr int kDepth = 8;                 // the interrupted address and up to 7 callers (BF_DEV_PROFILE_STACKS=1)
struct Sample {
  uint64_t ip[kDepth];
  int32_t tid;
};
bool g_stacks = false;
struct Walk {
  uint64_t* out;
  int n, cap;
};
_Unwind_Reason_Code Step(_Unwind_Context* c, void* arg) {
  auto* w = static_cast<Walk*>(arg);
  const uint64_t ip = _Unwind_GetIP(c);
  if (!ip) return _URC_END_OF_STACK;
  w->out[w->n++] = ip;
  return w->n < w->cap ? _URC_NO_REASON : _URC_END_OF_STACK;
}
Sample* g_samples = nullptr;
std::atomic<size_t> g_count{0};
std::atomic<bool> g_on{false};

void OnProf(int, siginfo_t* si, void* uc_void) {
  if (!g_on.load(std::memory_order_relaxed)) return;
  const size_t i = g_count.fetch_add(1, std::memory_order_relaxed);
  if (i >= kMaxSamples) return;
  auto* uc = static_cast<ucontext_t*>(uc_void);
  Sample& s = g_samples[i];
  s.ip[0] = uint64_t(uc->uc_mcontext.gregs[REG_RIP]);
  s.tid = si->si_value.sival_int;
  if (g_stacks) {
    // Callers from the unwind tables (no frame pointers in this build): the walk starts in this handler, passes the
    // kernel's signal frame (glibc's __restore_rt has unwind info) and goes on into the interrupted code, whose first
    // frame is the interrupted address itself (kept already): the callers are the frames after it.
    uint64_t frames[kDepth + 6] = {};
    Walk w{frames, 0, kDepth + 6};
    _Unwind_Backtrace(Step, &w);
    int at = -1;
    for (int k = 0; k < w.n; ++k)
      if (frames[k] == s.ip[0]) {
        at = k;
        break;
      }
    for (int k = 1; k < kDepth && at >= 0 && at + k < w.n; ++k) s.ip[k] = frames[at + k];
  }
}

std::string ReadSmall(const std::string& path) {
  std::ifstream f(path);
  std::string s;
  std::getline(f, s);
  return s;
}

void Run(double start_s, double seconds, int hz, std::string out) {
  std::this_thread::sleep_for(std::chrono::duration<double>(start_s));
  g_samples = static_cast<Sample*>(std::calloc(kMaxSamples, sizeof(Sample)));
  if (!g_samples) return;
  struct sigaction sa {};
  sa.sa_sigaction = OnProf;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPROF, &sa, nullptr);
  const pid_t self = pid_t(syscall(SYS_gettid));
  std::vector<std::pair<timer_t, pid_t>> timers;
  std::map<pid_t, std::string> names;
  if (DIR* d = opendir("/proc/self/task")) {
    while (dirent* e = readdir(d)) {
      const pid_t tid = pid_t(std::atoi(e->d_name));
      if (tid <= 0 || tid == self) continue;
      names[tid] = ReadSmall("/proc/self/task/" + std::to_string(tid) + "/comm");
      const clockid_t clock = clockid_t((~tid << 3) | 6);  // the thread's CPU time clock (CPUCLOCK_SCHED, per thread)
      sigevent sev {};
      sev.sigev_notify = SIGEV_THREAD_ID;
      sev.sigev_signo = SIGPROF;
      sev._sigev_un._tid = tid;
      sev.sigev_value.sival_int = tid;
      timer_t t {};
      if (timer_create(clock, &sev, &t) != 0) continue;
      itimerspec its {};
      its.it_interval.tv_nsec = long(1e9 / hz);
      its.it_value = its.it_interval;
      if (timer_settime(t, 0, &its, nullptr) != 0) {
        timer_delete(t);
        continue;
      }
      timers.emplace_back(t, tid);
    }
    closedir(d);
  }
  g_on.store(true);
  REXLOG_INFO("BF profile: sampling {} threads at {} Hz of their CPU time for {:.0f} s", timers.size(), hz, seconds);
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
  g_on.store(false);
  for (auto& [t, tid] : timers) timer_delete(t);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // handlers still running finish
  const size_t n = std::min(g_count.load(), kMaxSamples);
  std::map<std::pair<int32_t, uint64_t>, uint32_t> counts;
  std::map<std::pair<int32_t, std::string>, uint32_t> stacks;
  for (size_t i = 0; i < n; ++i) {
    ++counts[{g_samples[i].tid, g_samples[i].ip[0]}];
    if (g_stacks) {
      char buf[kDepth * 18] = {};
      int len = 0;
      for (int k = 0; k < kDepth && g_samples[i].ip[k]; ++k)
        len += std::snprintf(buf + len, sizeof(buf) - len, "%s%llx", k ? " " : "", (unsigned long long)g_samples[i].ip[k]);
      ++stacks[{g_samples[i].tid, std::string(buf, len)}];
    }
  }
  if (FILE* f = std::fopen(out.c_str(), "w")) {
    std::fprintf(f, "# BF profile: %zu samples, %d Hz, %.0f s\n# maps\n", n, hz, seconds);
    std::ifstream maps("/proc/self/maps");
    for (std::string line; std::getline(maps, line);)
      if (line.find(" r-xp ") != std::string::npos || line.find(" r--p 00000000 ") != std::string::npos)
        std::fprintf(f, "M %s\n", line.c_str());
    for (auto& [tid, name] : names) std::fprintf(f, "T %d %s\n", tid, name.c_str());
    for (auto& [key, c] : counts) std::fprintf(f, "S %d %llx %u\n", key.first, (unsigned long long)key.second, c);
    for (auto& [key, c] : stacks) std::fprintf(f, "K %d %u %s\n", key.first, c, key.second.c_str());
    std::fclose(f);
  }
  REXLOG_INFO("BF profile: {} samples written to {}", n, out);
}

struct Starter {
  Starter() {
    const char* v = std::getenv("BF_DEV_PROFILE");
    if (!v || !*v) return;
    double start = 60, seconds = 30;
    int hz = 499;
    std::sscanf(v, "%lf,%lf,%d", &start, &seconds, &hz);
    const char* o = std::getenv("BF_DEV_PROFILE_OUT");
    const char* k = std::getenv("BF_DEV_PROFILE_STACKS");
    g_stacks = k && *k == '1';
    std::thread(Run, start, seconds, hz < 10 ? 499 : hz, std::string(o && *o ? o : "bf1943-profile.txt")).detach();
  }
} g_starter;
}  // namespace

#endif
