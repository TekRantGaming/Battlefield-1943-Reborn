// Settings > Diagnostics > System logging (diagnostics.h): the PC and the game's health as a standing baseline, so a
// report's log already says what ran short (the user, 27 Nov 2026: "log things that will help catch issues ... they
// should already help us narrow things down or clearly indicate what's causing the issue").
//
//   at the game's start   the system: OS, CPU, RAM, GPU and driver, Wine or Proton (report.cpp SystemLine)
//   every minute          memory (the process, its peak, the PC's free), the guest's memory (its physical and
//                         virtual heaps, how full), the process's CPU, the game's file reads (count, MB, the slowest,
//                         how many over 100 ms: NtReadFile, dev_io_stats.cpp), power (mains or battery) and the CPU's
//                         clock against its top
//   as it happens         the power source changed; the CPU held well under its top clock while playing (power saving
//                         or heat); the guest's physical memory or the PC's nearly full; the window's focus and
//                         minimising (NoteWindow, bf1943_app.h)
//
// All of it on a thread of its own (no reads of /proc or the system on the game's threads), every 10 seconds.
#include "diagnostics.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <map>
#include <mutex>

#include <native_x_renderer/native_x_renderer.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "report.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <powerbase.h>
#include <psapi.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#endif

namespace bf1943::diagnostics {
namespace {

namespace fs = std::filesystem;
constexpr double kMB = 1024.0 * 1024.0;

// ---- the game's file reads (NoteFileRead, from NtReadFile)

std::atomic<int64_t> g_reads{0}, g_read_bytes{0}, g_read_us{0}, g_slow_reads{0};

// The window's focus (NoteFocus): since when it is away (steady ms; 0 focused), and whether that was said.
std::atomic<int64_t> g_away_since_ms{0};
std::atomic<bool> g_away_said{false};
int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
std::atomic<int32_t> g_slowest_read_ms{0};

// ---- the process and the PC

struct Proc {
  double rss_mb = -1, peak_mb = -1, private_mb = -1, cpu_s = -1;
  int64_t faults = -1;  // Linux: major page faults (read from the disk); Windows: all page faults (soft ones too)
  double read_mb = -1, write_mb = -1;  // everything the process read and wrote (the disk and the rest)
};
struct Pc {
  double free_mb = -1, total_mb = -1;
  double cpu_busy_s = -1, cpu_total_s = -1;  // all the PC's CPUs: time busy and in all (summed over the cores)
  int cores = 0;
  int mains = -1, battery = -1;  // 1 on mains, 0 on battery; battery %; -1 not known (a desktop has none)
  double mhz = -1, max_mhz = -1;  // the CPU's clock now (cores' average) and its top
};

#if defined(_WIN32)
Proc ReadProc() {
  Proc p;
  PROCESS_MEMORY_COUNTERS_EX m{};
  m.cb = sizeof(m);
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof(m))) {
    p.rss_mb = m.WorkingSetSize / kMB, p.peak_mb = m.PeakWorkingSetSize / kMB, p.private_mb = m.PrivateUsage / kMB;
    p.faults = m.PageFaultCount;
  }
  FILETIME created, exited, kernel, user;
  if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
    auto secs = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32 | f.dwLowDateTime) / 1e7; };
    p.cpu_s = secs(kernel) + secs(user);
  }
  IO_COUNTERS io{};
  if (GetProcessIoCounters(GetCurrentProcess(), &io)) p.read_mb = io.ReadTransferCount / kMB, p.write_mb = io.WriteTransferCount / kMB;
  return p;
}
Pc ReadPc() {
  Pc pc;
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof(ms);
  if (GlobalMemoryStatusEx(&ms)) pc.free_mb = ms.ullAvailPhys / kMB, pc.total_mb = ms.ullTotalPhys / kMB;
  FILETIME idle, kernel, user;  // kernel includes idle
  if (GetSystemTimes(&idle, &kernel, &user)) {
    auto secs = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32 | f.dwLowDateTime) / 1e7; };
    pc.cpu_total_s = secs(kernel) + secs(user), pc.cpu_busy_s = pc.cpu_total_s - secs(idle);
  }
  SYSTEM_POWER_STATUS ps{};
  if (GetSystemPowerStatus(&ps)) {
    if (ps.ACLineStatus != 255) pc.mains = ps.ACLineStatus == 1;
    if (ps.BatteryFlag != 128 && ps.BatteryFlag != 255 && ps.BatteryLifePercent != 255) pc.battery = ps.BatteryLifePercent;
    if (pc.battery < 0) pc.mains = -1;  // no battery: a desktop, nothing to say
  }
  // PROCESSOR_POWER_INFORMATION as Microsoft documents it (no header declares it).
  struct ProcessorPower {
    ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
  };
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  pc.cores = int(si.dwNumberOfProcessors);
  std::vector<ProcessorPower> cpus(si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1);
  if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, cpus.data(), ULONG(cpus.size() * sizeof(ProcessorPower))) == 0) {
    double now = 0, top = 0;
    for (const auto& c : cpus) now += c.CurrentMhz, top = std::max(top, double(c.MaxMhz));
    pc.mhz = now / cpus.size(), pc.max_mhz = top;
  }
  return pc;
}
#else
std::string ReadAll(const char* path) {
  std::ifstream f(path);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}
// "VmRSS:   123456 kB" and the like: the number after `key`, in kB.
double Field(const std::string& text, const char* key) {
  const size_t at = text.find(key);
  return at == std::string::npos ? -1 : std::atof(text.c_str() + at + std::strlen(key));
}
Proc ReadProc() {
  Proc p;
  const std::string status = ReadAll("/proc/self/status");
  p.rss_mb = Field(status, "VmRSS:") / 1024, p.peak_mb = Field(status, "VmHWM:") / 1024;
  p.private_mb = Field(status, "RssAnon:") / 1024;
  // /proc/self/stat: utime and stime are fields 14 and 15, after the name in parentheses.
  const std::string stat = ReadAll("/proc/self/stat");
  if (const size_t close = stat.rfind(')'); close != std::string::npos) {
    std::istringstream in(stat.substr(close + 2));
    std::string field;
    double ut = 0, st = 0;
    for (int i = 3; i <= 15 && in >> field; ++i) {
      if (i == 12) p.faults = std::atoll(field.c_str());
      if (i == 14) ut = std::atof(field.c_str());
      if (i == 15) st = std::atof(field.c_str());
    }
    p.cpu_s = (ut + st) / double(sysconf(_SC_CLK_TCK));
  }
  const std::string io = ReadAll("/proc/self/io");
  if (!io.empty()) p.read_mb = Field(io, "rchar:") / kMB, p.write_mb = Field(io, "wchar:") / kMB;
  return p;
}
Pc ReadPc() {
  Pc pc;
  const std::string mem = ReadAll("/proc/meminfo");
  pc.free_mb = Field(mem, "MemAvailable:") / 1024, pc.total_mb = Field(mem, "MemTotal:") / 1024;
  {
    // /proc/stat's first line: "cpu user nice system idle iowait irq softirq steal ..." (ticks, all cores).
    std::istringstream in(ReadAll("/proc/stat"));
    std::string line;
    if (std::getline(in, line) && line.rfind("cpu ", 0) == 0) {
      std::istringstream f(line.substr(4));
      double v[8] = {}, total = 0;
      for (int i = 0; i < 8 && f >> v[i]; ++i) total += v[i];
      const double tick = double(sysconf(_SC_CLK_TCK));
      pc.cpu_total_s = total / tick, pc.cpu_busy_s = (total - v[3] - v[4]) / tick;
    }
    while (std::getline(in, line))
      if (line.rfind("cpu", 0) == 0 && line.size() > 3 && std::isdigit(static_cast<unsigned char>(line[3]))) ++pc.cores;
  }
  std::error_code ec;
  for (const auto& e : fs::directory_iterator("/sys/class/power_supply", ec)) {
    const std::string type = ReadAll((e.path() / "type").c_str());
    if (type.rfind("Mains", 0) == 0) pc.mains = std::atoi(ReadAll((e.path() / "online").c_str()).c_str());
    if (type.rfind("Battery", 0) == 0) pc.battery = std::atoi(ReadAll((e.path() / "capacity").c_str()).c_str());
  }
  if (pc.battery < 0) pc.mains = -1;  // no battery: a desktop, nothing to say
  double now = 0, top = 0;
  int n = 0;
  for (const auto& e : fs::directory_iterator("/sys/devices/system/cpu", ec)) {
    const std::string name = e.path().filename().string();
    if (name.rfind("cpu", 0) != 0 || name.size() < 4 || !std::isdigit(static_cast<unsigned char>(name[3]))) continue;
    const std::string cur = ReadAll((e.path() / "cpufreq" / "scaling_cur_freq").c_str());
    if (cur.empty()) continue;
    now += std::atof(cur.c_str()) / 1000, ++n;
    top = std::max(top, std::atof(ReadAll((e.path() / "cpufreq" / "cpuinfo_max_freq").c_str()).c_str()) / 1000);
  }
  if (n) pc.mhz = now / n, pc.max_mhz = top;
  return pc;
}
#endif

// The guest's memory: its physical heaps (0xA0000000 64 KB pages, 0xC0000000 16 MB, 0xE0000000 4 KB; one physical
// memory under them), how much is taken. (Its virtual heap 0x40000000 is barely used by this game: left out.)
struct Guest {
  double physical_used_mb = -1, physical_mb = -1;
  double largest_free_mb = -1;  // the longest free run in the physical heaps' 4 KB view (large allocations need one)
};
Guest ReadGuest() {
  Guest g;
  auto* state = rex::system::KernelState::shared();
  if (!state || !state->memory()) return g;
  auto* memory = state->memory();
  auto used = [&](uint32_t address, double& used_mb, double& total_mb) {
    auto* heap = memory->LookupHeap(address);
    if (!heap) return;
    const double page = heap->page_size();
    const double total = heap->GetTotalPageCount() * page, free = heap->GetUnreservedPageCount() * page;
    used_mb = (used_mb < 0 ? 0 : used_mb) + (total - free) / kMB;
    total_mb = (total_mb < 0 ? 0 : total_mb) + total / kMB;
  };
  for (uint32_t a : {0xA0000000u, 0xC0000000u, 0xE0000000u}) used(a, g.physical_used_mb, g.physical_mb);
  if (auto* heap = memory->LookupHeap(0xE0000000u)) g.largest_free_mb = heap->GetLargestUnreservedRun() * double(heap->page_size()) / kMB;
  // The physical heaps are three views of one memory: its size is the largest view's, not their sum.
  if (g.physical_mb > 512) g.physical_mb = 512;
  return g;
}

std::string Mb(double mb) { return mb < 0 ? "?" : mb >= 1024 ? fmt::format("{:.1f} GB", mb / 1024) : fmt::format("{:.0f} MB", mb); }
std::string Percent(double part, double whole) { return whole > 0 ? fmt::format("{:.0f}%", 100.0 * part / whole) : "?"; }

// ---- the process's threads: CPU time each, and (Linux) the time each was ready to run but waited for a CPU (the OS
// gave it to someone else: another program, or too many of the game's own threads for the cores)

struct ThreadTimes {
  std::string name;
  double cpu_s = 0, delay_s = -1;
};
using ThreadMap = std::map<uint64_t, ThreadTimes>;  // by the OS's thread id

#if defined(_WIN32)
ThreadMap ReadThreads() {
  ThreadMap out;
  using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const auto get_description = reinterpret_cast<GetDescription>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
  const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return out;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  const DWORD pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    const HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
    if (!t) continue;
    ThreadTimes tt;
    FILETIME created, exited, kernel, user;
    if (GetThreadTimes(t, &created, &exited, &kernel, &user)) {
      auto secs = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32 | f.dwLowDateTime) / 1e7; };
      tt.cpu_s = secs(kernel) + secs(user);
    }
    PWSTR desc = nullptr;
    if (get_description && SUCCEEDED(get_description(t, &desc)) && desc) {
      char utf8[128] = {};
      WideCharToMultiByte(CP_UTF8, 0, desc, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
      tt.name = utf8;
      LocalFree(desc);
    }
    CloseHandle(t);
    if (tt.name.empty()) tt.name = "unnamed";
    out[te.th32ThreadID] = std::move(tt);
  }
  CloseHandle(snap);
  return out;
}
#else
ThreadMap ReadThreads() {
  ThreadMap out;
  std::error_code ec;
  const double tick = double(sysconf(_SC_CLK_TCK));
  for (const auto& e : fs::directory_iterator("/proc/self/task", ec)) {
    const uint64_t tid = std::strtoull(e.path().filename().c_str(), nullptr, 10);
    if (!tid) continue;
    ThreadTimes tt;
    tt.name = ReadAll((e.path() / "comm").c_str());
    while (!tt.name.empty() && (tt.name.back() == '\n' || tt.name.back() == ' ')) tt.name.pop_back();
    if (tt.name.empty()) tt.name = "unnamed";
    const std::string stat = ReadAll((e.path() / "stat").c_str());
    if (const size_t close = stat.rfind(')'); close != std::string::npos) {
      std::istringstream in(stat.substr(close + 2));
      std::string field;
      double ut = 0, st = 0;
      for (int i = 3; i <= 15 && in >> field; ++i) {
        if (i == 14) ut = std::atof(field.c_str());
        if (i == 15) st = std::atof(field.c_str());
      }
      tt.cpu_s = (ut + st) / tick;
    }
    // schedstat: time on a CPU, time waiting on a run queue (ns), slices.
    std::istringstream sched(ReadAll((e.path() / "schedstat").c_str()));
    double on_cpu = 0, waited = 0;
    if (sched >> on_cpu >> waited) tt.delay_s = waited / 1e9;
    out[tid] = std::move(tt);
  }
  return out;
}
#endif

// The minute's threads: the busiest by name (threads of one name, the compile workers, summed), with their waits for a
// CPU; the process's whole wait for a CPU.
std::string ThreadsPart(const ThreadMap& before, const ThreadMap& now, double secs, double* delay_total) {
  struct Sum {
    double cpu = 0, delay = 0;
    int count = 0;
  };
  std::map<std::string, Sum> by_name;
  *delay_total = 0;
  bool have_delay = false;
  for (const auto& [tid, t] : now) {
    const auto it = before.find(tid);
    const double cpu = t.cpu_s - (it != before.end() ? it->second.cpu_s : 0);
    double delay = 0;
    if (t.delay_s >= 0) delay = t.delay_s - (it != before.end() && it->second.delay_s >= 0 ? it->second.delay_s : 0), have_delay = true;
    Sum& s = by_name[t.name];
    s.cpu += std::max(0.0, cpu), s.delay += std::max(0.0, delay), ++s.count;
    *delay_total += std::max(0.0, delay);
  }
  if (!have_delay) *delay_total = -1;
  std::vector<std::pair<double, std::string>> top;
  for (const auto& [name, s] : by_name) top.push_back({s.cpu, name});
  std::sort(top.rbegin(), top.rend());
  std::string out;
  for (size_t i = 0; i < top.size() && i < 6 && top[i].first > 0.01 * secs; ++i) {
    const Sum& s = by_name[top[i].second];
    out += fmt::format("{}{}{} {:.0f}%", out.empty() ? "" : ", ", top[i].second,
                       s.count > 1 ? fmt::format(" (x{})", s.count) : std::string(), 100.0 * s.cpu / secs);
    if (have_delay && s.delay >= 0.05) out += fmt::format(" (waited {:.1f} s for a CPU)", s.delay);
  }
  return out;
}

// ---- the game thread's waits (NoteWait), by the guest caller

struct WaitStat {
  double ms = 0, longest = 0;
  int64_t count = 0;
};
std::mutex g_wait_mutex;
std::map<uint32_t, WaitStat> g_waits;

std::string TakeWaits(double secs) {
  std::map<uint32_t, WaitStat> waits;
  {
    std::lock_guard lock(g_wait_mutex);
    waits.swap(g_waits);
  }
  if (waits.empty()) return {};
  double total = 0;
  int64_t count = 0;
  std::vector<std::pair<double, uint32_t>> top;
  for (const auto& [caller, w] : waits) total += w.ms, count += w.count, top.push_back({w.ms, caller});
  std::sort(top.rbegin(), top.rend());
  std::string out = fmt::format("; the game's main thread waited {:.1f} s of {:.0f} in {} waits from 1 ms (by guest caller:", total / 1000,
                                secs, count);
  for (size_t i = 0; i < top.size() && i < 3; ++i) {
    const WaitStat& w = waits[top[i].second];
    out += fmt::format("{} {:08X} {:.1f} s in {}, the longest {:.0f} ms", i ? ";" : "", top[i].second, w.ms / 1000,
                       w.count, w.longest);
  }
  return out + ")";
}

// ---- the threads sampled 4 times a second (Normal and up): the busiest around a black box trigger

struct ThreadSample {
  int64_t t_us = 0;
  std::vector<std::pair<std::string, std::pair<float, float>>> busy;  // name, (CPU ms, waited for a CPU ms) since the last
};
std::mutex g_samples_mutex;
std::deque<ThreadSample> g_samples;  // 30 s

int64_t SteadyUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void ThreadSamplerThread() {
  ThreadMap last = ReadThreads();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    ThreadMap now = ReadThreads();
    std::map<std::string, std::pair<float, float>> by_name;
    for (const auto& [tid, t] : now) {
      const auto it = last.find(tid);
      if (it == last.end()) continue;
      auto& v = by_name[t.name];
      v.first += float((t.cpu_s - it->second.cpu_s) * 1000);
      if (t.delay_s >= 0 && it->second.delay_s >= 0) v.second += float((t.delay_s - it->second.delay_s) * 1000);
    }
    ThreadSample s;
    s.t_us = SteadyUs();
    for (auto& [name, v] : by_name)
      if (v.first >= 5 || v.second >= 5) s.busy.push_back({name, v});
    std::sort(s.busy.begin(), s.busy.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    if (s.busy.size() > 10) s.busy.resize(10);
    {
      std::lock_guard lock(g_samples_mutex);
      g_samples.push_back(std::move(s));
      while (!g_samples.empty() && g_samples.front().t_us < SteadyUs() - 30'000'000) g_samples.pop_front();
    }
    last = std::move(now);
  }
}

void HealthThread() {
  const auto start = std::chrono::steady_clock::now();
  Proc last = ReadProc(), last10 = last;
  Pc last_pc = ReadPc(), last_pc10 = last_pc;
  ThreadMap last_threads = ReadThreads();
  auto last_at = start, last10_at = start;
  int last_mains = last_pc.mains, slow_clock = 0, ticks = 0;
  auto physical_warned = start - std::chrono::hours(1), pc_warned = physical_warned, others_warned = physical_warned,
       vram_warned = physical_warned;
  bool clock_said = false;
  double physical_peak = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    ++ticks;
    const auto now = std::chrono::steady_clock::now();
    const Pc pc = ReadPc();
    const Proc p10 = ReadProc();
    const Guest guest = ReadGuest();
    physical_peak = std::max(physical_peak, guest.physical_used_mb);
    // As it happens: the power source, a held low clock while playing, memory nearly full.
    if (pc.mains >= 0 && last_mains >= 0 && pc.mains != last_mains)
      REXLOG_INFO("BF health{}: now on {} (battery {}%)", SceneTag(), pc.mains ? "mains power" : "battery", pc.battery);
    last_mains = pc.mains;
    // A low clock alone is normal (the frame limit leaves the CPU idle: 27 Nov 2026, 41% at a steady 60 FPS): said
    // only when the game also struggled, 3 frames or more over 33 ms in each of three 10 s checks.
    const bool playing = std::string(SceneName()) == "playing";
    const int64_t slow_frames = TakeRecentSlowFrames();
    if (playing && pc.mhz > 0 && pc.max_mhz > 0 && pc.mhz < 0.6 * pc.max_mhz && slow_frames >= 3) ++slow_clock;
    else slow_clock = 0;
    if (!playing) clock_said = false;
    if (slow_clock == 3 && !clock_said) {  // 30 s
      clock_said = true;
      REXLOG_WARN("BF health{}: slow frames while the CPU ran at {:.0f}% of its top clock for 30 s ({:.0f} of {:.0f} "
                  "MHz): power saving or heat may be holding it back",
                  SceneTag(), 100 * pc.mhz / pc.max_mhz, pc.mhz, pc.max_mhz);
    }
    // Other programs busy while the game struggled (27 Nov 2026: a build of ours with 8 compilers during a match
    // gave 12 FPS and audio underruns; the game's own CPU fell): the cores the rest of the PC kept busy.
    const double secs10 = std::chrono::duration<double>(now - last10_at).count();
    if (pc.cpu_busy_s >= 0 && last_pc10.cpu_busy_s >= 0 && p10.cpu_s >= 0 && last10.cpu_s >= 0 && secs10 > 0 && pc.cores > 0) {
      const double pc_cores = (pc.cpu_busy_s - last_pc10.cpu_busy_s) / secs10, game_cores = (p10.cpu_s - last10.cpu_s) / secs10;
      const double others = std::max(0.0, pc_cores - game_cores);
      if (SceneIsPlay() && slow_frames >= 3 && others > std::max(1.5, 0.25 * pc.cores) && now - others_warned > std::chrono::minutes(5)) {
        others_warned = now;
        REXLOG_WARN("BF health{}: {} slow frames in 10 s while other programs kept {:.1f} of the {} cores busy (the game "
                    "{:.1f}): another program is taking the CPU",
                    SceneTag(), slow_frames, others, pc.cores, game_cores);
      }
    }
    last10 = p10, last_pc10 = pc, last10_at = now;
    if (const int64_t away = g_away_since_ms.load(); away && SteadyMs() - away > 10000 && !g_away_said.exchange(true))
      REXLOG_INFO("BF window{}: focus lost {:.0f} s ago, still away", SceneTag(), (SteadyMs() - away) / 1000.0);
    if (guest.physical_mb > 0 && guest.physical_used_mb > 0.95 * guest.physical_mb && now - physical_warned > std::chrono::minutes(5)) {
      physical_warned = now;
      REXLOG_WARN("BF health{}: the game's physical memory is {} full ({} of {})", SceneTag(),
                  Percent(guest.physical_used_mb, guest.physical_mb), Mb(guest.physical_used_mb), Mb(guest.physical_mb));
    }
    if (pc.total_mb > 0 && pc.free_mb >= 0 && pc.free_mb < 0.05 * pc.total_mb && now - pc_warned > std::chrono::minutes(5)) {
      pc_warned = now;
      REXLOG_WARN("BF health{}: the PC's memory is nearly full ({} free of {})", SceneTag(), Mb(pc.free_mb), Mb(pc.total_mb));
    }
    native_x_renderer::VideoMemory vram;
    const bool have_vram = native_x_renderer::QueryVideoMemory(&vram);
    if (have_vram && vram.budget && vram.used > vram.budget * 9 / 10 && now - vram_warned > std::chrono::minutes(5)) {
      vram_warned = now;
      REXLOG_WARN("BF health{}: video memory nearly at the driver's budget ({} used of {}): the driver may move textures "
                  "out to system memory (stutter, slow frames)",
                  SceneTag(), Mb(vram.used / kMB), Mb(vram.budget / kMB));
    }
    if (ticks % 6) continue;
    // Every minute: the lines.
    const Proc p = ReadProc();
    const ThreadMap threads = ReadThreads();
    const double secs = std::chrono::duration<double>(now - last_at).count();
    const double cpu = p.cpu_s >= 0 && last.cpu_s >= 0 && secs > 0 ? 100.0 * (p.cpu_s - last.cpu_s) / secs : -1;
    const int64_t reads = g_reads.exchange(0), bytes = g_read_bytes.exchange(0), us = g_read_us.exchange(0),
                  slow = g_slow_reads.exchange(0);
    const int32_t slowest = g_slowest_read_ms.exchange(0);
    std::string power;
    if (pc.mains >= 0) power = fmt::format("; power: {} ({}%)", pc.mains ? "mains" : "battery", pc.battery);
    std::string clock;
    if (pc.mhz > 0 && pc.max_mhz > 0) clock = fmt::format("; CPU clock {:.0f} of {:.0f} MHz", pc.mhz, pc.max_mhz);
    std::string pc_cpu;
    if (pc.cpu_total_s > 0 && last_pc.cpu_total_s > 0 && pc.cpu_total_s > last_pc.cpu_total_s) {
      const double busy = (pc.cpu_busy_s - last_pc.cpu_busy_s) / (pc.cpu_total_s - last_pc.cpu_total_s);
      const double game = cpu >= 0 && pc.cores ? cpu / 100.0 : 0;
      pc_cpu = fmt::format("; the PC's CPUs {:.0f}% busy ({} cores: the game {:.1f}, other programs {:.1f})", 100 * busy,
                           pc.cores, game, std::max(0.0, busy * pc.cores - game));
    }
    std::string faults;
    if (p.faults >= 0 && last.faults >= 0)
#if defined(_WIN32)
      faults = fmt::format("; page faults {} (all kinds)", p.faults - last.faults);
#else
      faults = fmt::format("; page faults from the disk {}", p.faults - last.faults);
#endif
    std::string video;
    if (have_vram)
      video = vram.budget ? fmt::format("; video memory {} of the driver's budget {} ({}; the GPU has {})", Mb(vram.used / kMB),
                                        Mb(vram.budget / kMB), Percent(double(vram.used), double(vram.budget)),
                                        Mb(vram.heap_size / kMB))
                          : fmt::format("; video memory: the GPU has {} (no budget reported)", Mb(vram.heap_size / kMB));
    REXLOG_INFO(
        "BF health{}: memory {} (peak {}, private {}), the PC {} free of {}; the game's memory {} full ({} of {}, the "
        "most so far {}, the largest free block {}){}; CPU {} of one core over {:.0f} s{}{}; file reads {} ({:.1f} MB, {:.0f} ms in all, the "
        "slowest {} ms, {} over 100 ms); the process read {:.1f} MB, wrote {:.1f} MB{}{}",
        SceneTag(), Mb(p.rss_mb), Mb(p.peak_mb), Mb(p.private_mb), Mb(pc.free_mb), Mb(pc.total_mb),
        Percent(guest.physical_used_mb, guest.physical_mb), Mb(guest.physical_used_mb), Mb(guest.physical_mb),
        Mb(physical_peak), Mb(guest.largest_free_mb), video, cpu < 0 ? std::string("?") : fmt::format("{:.0f}%", cpu), secs, pc_cpu, faults, reads,
        bytes / kMB, us / 1000.0, slowest, slow, p.read_mb >= 0 && last.read_mb >= 0 ? p.read_mb - last.read_mb : 0.0,
        p.write_mb >= 0 && last.write_mb >= 0 ? p.write_mb - last.write_mb : 0.0, power, clock);
    double delay_total = -1;
    const std::string busiest = ThreadsPart(last_threads, threads, secs, &delay_total);
    // More work than cores: the game's threads kept waiting for a CPU while the PC was busy.
    if (delay_total > 0.15 * secs && pc.cpu_total_s > last_pc.cpu_total_s &&
        (pc.cpu_busy_s - last_pc.cpu_busy_s) / (pc.cpu_total_s - last_pc.cpu_total_s) > 0.85)
      REXLOG_WARN("BF health{}: the game's threads waited {:.1f} s for a CPU in {:.0f} s while the PC's {} cores were over "
                  "85% busy: more work than cores (other programs, or the game's own threads)",
                  SceneTag(), delay_total, secs, pc.cores);
    if (!busiest.empty() || delay_total >= 0)
      REXLOG_INFO("BF health{}: threads by CPU over {:.0f} s (100% = one core): {}{}{}", SceneTag(), secs,
                  busiest.empty() ? "none busy" : busiest,
                  delay_total >= 0 ? fmt::format("; the game's threads waited {:.1f} s in all for a CPU (the OS ran "
                                                 "something else)",
                                                 delay_total)
                                   : std::string(),
                  TakeWaits(secs));
    // Detailed and up: the game frame thread's code over the last 30 s (the profiler's ring), every minute even with no
    // black box dump to show it.
    if (Want(Area::kBlackBox, Level::kDetailed))
      if (const std::string top = TopFunctions(SteadyUs() - 30'000'000, SteadyUs(), 10); !top.empty())
        REXLOG_INFO("BF profile{}: the game frame thread's code over the last 30 s: {}", SceneTag(), top);
    last = p, last_pc = pc, last_threads = threads, last_at = now;
  }
}

}  // namespace

void StartHealth() {
  static std::atomic<bool> started{false}, sampler_started{false};
  if (On(Area::kSystem) && !started.exchange(true)) std::thread(HealthThread).detach();
  // The black box's threads around a trigger (its own switch: it may be on with System off).
  if (On(Area::kBlackBox) && !sampler_started.exchange(true)) std::thread(ThreadSamplerThread).detach();
}

std::string BusyThreadsAround(int64_t at_us, int64_t span_us) {
  std::map<std::string, std::pair<float, float>> sum;
  double secs = 0;
  {
    std::lock_guard lock(g_samples_mutex);
    int64_t prev = 0;
    for (const ThreadSample& s : g_samples) {
      if (s.t_us >= at_us - span_us && s.t_us <= at_us + span_us + 250'000) {
        for (const auto& [name, v] : s.busy) sum[name].first += v.first, sum[name].second += v.second;
        secs += prev ? (s.t_us - prev) / 1e6 : 0.25;
      }
      prev = s.t_us;
    }
  }
  if (sum.empty() || secs <= 0) return {};
  std::vector<std::pair<float, std::string>> top;
  for (const auto& [name, v] : sum) top.push_back({v.first, name});
  std::sort(top.rbegin(), top.rend());
  std::string out;
  for (size_t i = 0; i < top.size() && i < 8; ++i) {
    const auto& v = sum[top[i].second];
    out += fmt::format("{}{} {:.0f}%", i ? ", " : "", top[i].second, v.first / 10.0 / secs);
    if (v.second >= 20) out += fmt::format(" (waited {:.0f} ms for a CPU)", v.second);
  }
  return fmt::format("over {:.1f} s: {}", secs, out);
}

void LogSystem(const std::filesystem::path& user_dir) {
  if (!On(Area::kSystem)) return;
  REXLOG_INFO("BF health: the system: {}", report::SystemLine(user_dir));
}

void NoteFileRead(uint64_t bytes, double ms, uint32_t handle) {
  CountRead(bytes, ms);
  if (ms > 50 && handle && IsSoundHandle(handle)) NoteSlowStream(ms);
  g_reads.fetch_add(1, std::memory_order_relaxed);
  g_read_bytes.fetch_add(int64_t(bytes), std::memory_order_relaxed);
  g_read_us.fetch_add(int64_t(ms * 1000), std::memory_order_relaxed);
  if (ms > 100) g_slow_reads.fetch_add(1, std::memory_order_relaxed);
  int32_t old = g_slowest_read_ms.load(std::memory_order_relaxed);
  const int32_t v = int32_t(ms + 0.5);
  while (v > old && !g_slowest_read_ms.compare_exchange_weak(old, v, std::memory_order_relaxed)) {
  }
}

void NoteFocus(bool focused) {
  if (!focused) {
    int64_t zero = 0;
    if (g_away_since_ms.compare_exchange_strong(zero, SteadyMs())) g_away_said = false;
    return;
  }
  const int64_t since = g_away_since_ms.exchange(0);
  if (!since || !On(Area::kSystem)) return;
  const double secs = (SteadyMs() - since) / 1000.0;
  if (secs >= 0.5) REXLOG_INFO("BF window{}: focus back after {:.1f} s away", SceneTag(), secs);
}

// The memory at a round's end, against the first round's (a leak shows as a climb from round to round).
void LogRoundMemory(int round) {
  if (!On(Area::kSystem)) return;
  static double first_rss = -1, first_guest = -1;
  const Proc p = ReadProc();
  const Guest g = ReadGuest();
  if (first_rss < 0) first_rss = p.rss_mb, first_guest = g.physical_used_mb;
  REXLOG_INFO("BF health{}: round {} ended: memory {} ({:+.0f} MB since the first round's end), the game's memory {} "
              "({:+.0f} MB), the largest free block {}",
              SceneTag(), round, Mb(p.rss_mb), p.rss_mb - first_rss, Mb(g.physical_used_mb), g.physical_used_mb - first_guest,
              Mb(g.largest_free_mb));
}

void NoteWait(uint32_t caller, double ms) {
  if (!OnGameThread()) return;
  CountGameThreadWait(ms);
  if (ms < 1.0 || !On(Area::kSystem)) return;
  std::lock_guard lock(g_wait_mutex);
  if (g_waits.size() >= 256 && !g_waits.count(caller)) return;
  WaitStat& w = g_waits[caller];
  w.ms += ms, w.longest = std::max(w.longest, ms), ++w.count;
}

void NoteWindow(const char* what) {
  if (Anonymized() || On(Area::kSystem)) REXLOG_INFO("BF window{}: {}", SceneTag(), what);
}

}  // namespace bf1943::diagnostics
