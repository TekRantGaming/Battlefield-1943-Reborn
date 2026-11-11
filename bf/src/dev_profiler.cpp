// Developer aid (BF_DEV_TOOLS builds, Windows): BF_DEV_PROFILE=<start s>,<seconds> samples where every thread of the
// process is, about 1000 times a second for <seconds> from <start s> after launch, then logs the busiest threads
// (by name) and, for each, the functions it was in most (DbgHelp, from the PDBs beside the modules: bf1943.pdb and
// rexgpu-bfnative.pdb; functions in modules without one show as module+offset). For finding where frame time goes.
//
// BF_DEV_PROFILE=<start s>,<seconds>,<ms>: also the samples taken during frames longer than <ms> (from one of the
// game's swaps to the next: frame_stats.cpp), by function and by call chain (the five innermost frames), to see what a
// thread did in the long frames alone (a stall of the game's thread, or the renderer waiting in its present).

#if defined(BF_DEV_TOOLS) && defined(_WIN32)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <tlhelp32.h>

#include <rex/logging.h>

namespace {

using Clock = std::chrono::steady_clock;

// Long frames (BF_DEV_PROFILE's third value): from the swap before to the swap that ended them.
std::atomic<double> g_hitch_ms{0};
std::mutex g_windows_mutex;
std::vector<std::pair<Clock::time_point, Clock::time_point>> g_windows;
uint32_t g_long_frames = 0;
double g_long_frames_ms = 0;

std::string ThreadName(HANDLE h, DWORD id) {
  using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static auto get = reinterpret_cast<GetDescription>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  std::string name;
  PWSTR w = nullptr;
  if (get && SUCCEEDED(get(h, &w)) && w) {
    for (const wchar_t* p = w; *p; ++p) name += char(*p < 128 ? *p : '?');
    LocalFree(w);
  }
  return name.empty() ? "thread " + std::to_string(id) : name + " (" + std::to_string(id) + ")";
}

// The game, the runtime and the GPU plugins: where a wait's caller is worth naming.
bool IsOurs(DWORD64 image) {
  static const DWORD64 ours[] = {DWORD64(GetModuleHandleW(nullptr)), DWORD64(GetModuleHandleW(L"rexruntime.dll")),
                                 DWORD64(GetModuleHandleW(L"rexgpu-bfnative.dll")),
                                 DWORD64(GetModuleHandleW(L"rexgpu-xenos.dll"))};
  for (DWORD64 m : ours)
    if (m && m == image) return true;
  return false;
}

// A sample kept for the long frames: when, and the innermost frames' addresses.
struct Sample {
  Clock::time_point t;
  std::array<DWORD64, 5> chain{};
};

struct Thread {
  HANDLE handle;
  std::string name;
  std::unordered_map<DWORD64, uint32_t> pcs;
  std::unordered_map<DWORD64, uint32_t> callers;  // waits: the first frame up the stack in our own modules
  uint32_t samples = 0;
  std::deque<Sample> recent;  // the last two seconds' samples (long frames only)
  std::unordered_map<DWORD64, uint32_t> hitch_pcs;
  std::map<std::array<DWORD64, 5>, uint32_t> hitch_chains;
  uint32_t hitch_samples = 0;
  uint64_t cpu_start = 0;  // its CPU time when the window began (100 ns units)
  double cpu_share = 0;    // its CPU time over the first second, a share of one processor
};

uint64_t CpuTime(HANDLE h) {
  FILETIME c{}, e{}, k{}, u{};
  if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
  return (uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime);
}

void Run(double start, double seconds, double hitch_ms) {
  std::this_thread::sleep_for(std::chrono::duration<double>(start));
  g_hitch_ms = hitch_ms;
  const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  std::map<DWORD, Thread> threads;
  auto refresh = [&] {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || threads.count(te.th32ThreadID)) continue;
      HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                            te.th32ThreadID);
      if (h) threads[te.th32ThreadID] = {h, ThreadName(h, te.th32ThreadID)};
    }
    CloseHandle(snap);
  };
  refresh();
  // A second of CPU times first: only the threads that are busy get sampled (pausing every thread each round was too
  // slow to sample often).
  for (auto& [id, t] : threads) t.cpu_start = CpuTime(t.handle);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  std::string shares;
  for (auto& [id, t] : threads) {
    t.cpu_share = (CpuTime(t.handle) - t.cpu_start) / 1e7;
    if (t.cpu_share >= 0.05) shares += fmt::format("\n    {:5.1f}%  {}", t.cpu_share * 100, t.name);
  }
  for (auto it = threads.begin(); it != threads.end();) {
    if (it->second.cpu_share < 0.05) CloseHandle(it->second.handle), it = threads.erase(it);
    else ++it;
  }
  REXLOG_INFO("BF profile: threads using 5% of a processor or more over a second:{}", shares);
  REXLOG_INFO("BF profile: sampling {} threads for {:.0f} s", threads.size(), seconds);
  timeBeginPeriod(1);
  const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  uint32_t rounds = 0;
  while (std::chrono::steady_clock::now() < end) {
    ++rounds;
    for (auto& [id, t] : threads) {
      if (SuspendThread(t.handle) == DWORD(-1)) continue;
      CONTEXT c{};
      c.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(t.handle, &c)) {
        // A thread that is waiting sits in a kernel call: counted under that function like any other, and its
        // stack walked (RtlVirtualUnwind) to the first frame in the game, the runtime or a GPU plugin.
        ++t.pcs[c.Rip];
        ++t.samples;
        Sample sample{Clock::now()};
        sample.chain[0] = c.Rip;
        bool caller_found = false;
        CONTEXT u = c;
        for (int depth = 0; depth < 24; ++depth) {
          DWORD64 image = 0;
          PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(u.Rip, &image, nullptr);
          if (depth > 0 && depth < 5) sample.chain[depth] = u.Rip;
          if (depth > 0 && image && IsOurs(image) && !caller_found) {
            ++t.callers[u.Rip];
            caller_found = true;
          }
          if (caller_found && (hitch_ms <= 0 || depth >= 4)) break;
          if (!fn) {
            u.Rip = *reinterpret_cast<DWORD64*>(u.Rsp);
            u.Rsp += 8;
          } else {
            void* handler_data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, u.Rip, fn, &u, &handler_data, &frame, nullptr);
          }
          if (!u.Rip) break;
        }
        if (hitch_ms > 0) t.recent.push_back(sample);
      }
      ResumeThread(t.handle);
    }
    if (hitch_ms > 0) {
      // Long frames that have ended: their samples tallied; samples older than two seconds dropped.
      std::vector<std::pair<Clock::time_point, Clock::time_point>> ended;
      {
        std::lock_guard lock(g_windows_mutex);
        ended.swap(g_windows);
      }
      for (auto& [id, t] : threads) {
        for (const auto& [from, to] : ended) {
          for (const Sample& s : t.recent) {
            if (s.t < from || s.t > to) continue;
            ++t.hitch_samples;
            ++t.hitch_pcs[s.chain[0]];
            ++t.hitch_chains[s.chain];
          }
        }
        const auto old = Clock::now() - std::chrono::seconds(2);
        while (!t.recent.empty() && t.recent.front().t < old) t.recent.pop_front();
      }
    }
    Sleep(1);
  }
  timeEndPeriod(1);
  g_hitch_ms = 0;
  REXLOG_INFO("BF profile: {} rounds", rounds);

  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);
  auto name_of = [&](DWORD64 pc) -> std::string {
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 displacement = 0;
    if (SymFromAddr(process, pc, &displacement, sym)) return sym->Name;
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(pc), &module)) {
      char path[MAX_PATH] = {};
      GetModuleFileNameA(module, path, MAX_PATH);
      const char* file = std::strrchr(path, '\\');
      char text[64];
      std::snprintf(text, sizeof(text), "+%llx", static_cast<unsigned long long>(pc - DWORD64(module)));
      return std::string(file ? file + 1 : path) + text;
    }
    return "?";
  };
  std::vector<Thread*> order;
  for (auto& [id, t] : threads) order.push_back(&t);
  // The busiest first (every sampled thread has the same sample count): by their CPU share over the first second.
  std::sort(order.begin(), order.end(), [](Thread* a, Thread* b) { return a->cpu_share > b->cpu_share; });
  if (hitch_ms > 0) {
    REXLOG_INFO("BF profile: {} frames longer than {:.0f} ms ({:.0f} ms in all)", g_long_frames, hitch_ms,
                g_long_frames_ms);
    std::vector<Thread*> by_hitch = order;
    std::sort(by_hitch.begin(), by_hitch.end(),
              [](Thread* a, Thread* b) { return a->hitch_samples > b->hitch_samples; });
    for (size_t i = 0; i < by_hitch.size() && i < 4; ++i) {
      Thread& t = *by_hitch[i];
      if (!t.hitch_samples) break;
      std::map<std::string, uint32_t> by_function;
      for (const auto& [pc, n] : t.hitch_pcs) by_function[name_of(pc)] += n;
      std::vector<std::pair<uint32_t, std::string>> top;
      for (const auto& [name, n] : by_function) top.push_back({n, name});
      std::sort(top.rbegin(), top.rend());
      std::string text;
      for (size_t k = 0; k < top.size() && k < 14; ++k)
        text += fmt::format("\n    {:5.1f}%  {}", 100.0 * top[k].first / t.hitch_samples, top[k].second);
      std::map<std::string, uint32_t> by_chain;
      for (const auto& [chain, n] : t.hitch_chains) {
        std::string s;
        for (DWORD64 pc : chain) {
          if (!pc) break;
          s += (s.empty() ? "" : " < ") + name_of(pc);
        }
        by_chain[s] += n;
      }
      std::vector<std::pair<uint32_t, std::string>> chains;
      for (const auto& [name, n] : by_chain) chains.push_back({n, name});
      std::sort(chains.rbegin(), chains.rend());
      text += "\n    call chains (innermost first):";
      for (size_t k = 0; k < chains.size() && k < 12; ++k)
        text += fmt::format("\n    {:5.1f}%  {}", 100.0 * chains[k].first / t.hitch_samples, chains[k].second);
      REXLOG_INFO("BF profile, long frames: {} ({} samples):{}", t.name, t.hitch_samples, text);
    }
  }
  for (size_t i = 0; i < order.size() && i < 12; ++i) {
    Thread& t = *order[i];
    std::map<std::string, uint32_t> by_function;
    for (const auto& [pc, n] : t.pcs) by_function[name_of(pc)] += n;
    std::vector<std::pair<uint32_t, std::string>> top;
    for (const auto& [name, n] : by_function) top.push_back({n, name});
    std::sort(top.rbegin(), top.rend());
    std::string text;
    for (size_t k = 0; k < top.size() && k < 14; ++k)
      text += fmt::format("\n    {:5.1f}%  {}", 100.0 * top[k].first / std::max(1u, t.samples), top[k].second);
    std::map<std::string, uint32_t> by_caller;
    for (const auto& [pc, n] : t.callers) by_caller[name_of(pc)] += n;
    std::vector<std::pair<uint32_t, std::string>> callers;
    for (const auto& [name, n] : by_caller) callers.push_back({n, name});
    std::sort(callers.rbegin(), callers.rend());
    if (!callers.empty()) text += "\n    called from (first frame in our modules):";
    for (size_t k = 0; k < callers.size() && k < 8; ++k)
      text += fmt::format("\n    {:5.1f}%  {}", 100.0 * callers[k].first / std::max(1u, t.samples), callers[k].second);
    REXLOG_INFO("BF profile: {} ({:.0f}% busy, {} samples):{}", t.name, t.cpu_share * 100, t.samples, text);
  }
  for (auto& [id, t] : threads) CloseHandle(t.handle);
}

struct Starter {
  Starter() {
    const char* v = std::getenv("BF_DEV_PROFILE");
    if (!v || !*v) return;
    const double start = std::atof(v);
    const char* comma = std::strchr(v, ',');
    const double seconds = comma ? std::atof(comma + 1) : 10.0;
    const char* comma2 = comma ? std::strchr(comma + 1, ',') : nullptr;
    const double hitch_ms = comma2 ? std::atof(comma2 + 1) : 0.0;
    std::thread([start, seconds, hitch_ms] { Run(start, seconds, hitch_ms); }).detach();
  }
} g_starter;

}  // namespace

namespace bf1943 {
// Each of the game's swaps (frame_stats.cpp): a frame longer than BF_DEV_PROFILE's third value is a window whose
// samples the profiler tallies.
void DevProfileFrame() {
  static Clock::time_point last{};
  const double hitch_ms = g_hitch_ms.load(std::memory_order_relaxed);
  const auto now = Clock::now();
  if (hitch_ms > 0 && last != Clock::time_point{}) {
    const double ms = std::chrono::duration<double, std::milli>(now - last).count();
    if (ms > hitch_ms) {
      std::lock_guard lock(g_windows_mutex);
      g_windows.push_back({last, now});
      ++g_long_frames;
      g_long_frames_ms += ms;
    }
  }
  last = now;
}
}  // namespace bf1943

#endif  // BF_DEV_TOOLS && _WIN32
