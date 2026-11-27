// Settings > Diagnostics, Detailed and up (27 Nov 2026: "the game's own code" named by function): the game frame
// thread's code sampled about 1000 times a second, so a slow frame's "the game's own code" says which of the game's
// functions it was in ("sub_822A9978 40%"), and how much was in the SDK, the system or the graphics driver.
//
//   Linux     a real-time signal (SIGRTMAX - 2; the SDK's are SIGRTMIN and up, the hang report's SIGRTMAX - 1) to the
//             thread; its handler keeps the interrupted instruction's address (async signal safe: a store into a ring)
//   Windows   the thread suspended for a moment, its instruction pointer read, resumed (nothing allocated meanwhile)
//
// An address is named through the game's function table: bf1943_RegisterFunctions (generated) run into a registrar
// of our own gives every recompiled function's host address and guest address; a sample is the nearest function at or
// below it. Our own code inside the game's binary can be named as its neighbouring game function ("nearest").
// Addresses outside the game's binary are named by their module (dladdr / GetModuleHandleEx).
// "from the game's sub_X" is the game's link register then: the game function the sample's code was called under. Our
// own code called from there (the frame limit's sleep under VdSwap's caller) shows the same way.
#include "diagnostics.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/thread_state.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cxxabi.h>
#include <dlfcn.h>
#include <elf.h>
#include <fstream>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#endif

void bf1943_RegisterFunctions(rex::runtime::IModuleRegistrar* registrar);  // generated (bf1943_register.cpp)

namespace bf1943::diagnostics {
namespace {

// ---- the game's functions by host address

class Collector final : public rex::runtime::IModuleRegistrar {
 public:
  bool SetFunction(uint32_t guest_address, ::PPCFunc* func) override {
    if (func) table.push_back({reinterpret_cast<uintptr_t>(func), guest_address});
    return true;
  }
  std::vector<std::pair<uintptr_t, uint32_t>> table;  // host address, guest address
};
std::vector<std::pair<uintptr_t, uint32_t>> g_table;
uintptr_t g_table_low = 0, g_table_high = 0;

// Linux: the binary's own symbol table (dev builds keep it): every function with its size, so a sample is named
// exactly (our code too), not as the nearest game function. Empty when the binary is stripped.
struct Symbol {
  uintptr_t start, end;
  std::string name;
};
std::vector<Symbol> g_symbols;

#if !defined(_WIN32)
void ReadOwnSymbols() {
  std::ifstream f("/proc/self/exe", std::ios::binary);
  if (!f) return;
  Elf64_Ehdr eh{};
  if (!f.read(reinterpret_cast<char*>(&eh), sizeof(eh)) || std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
      eh.e_ident[EI_CLASS] != ELFCLASS64)
    return;
  std::vector<Elf64_Shdr> sh(eh.e_shnum);
  f.seekg(std::streamoff(eh.e_shoff));
  if (!f.read(reinterpret_cast<char*>(sh.data()), std::streamsize(sh.size() * sizeof(Elf64_Shdr)))) return;
  // The load bias: a function's address now against its address in the file (the binary is position independent).
  Dl_info self{};
  if (!dladdr(reinterpret_cast<void*>(&ReadOwnSymbols), &self) || !self.dli_fbase) return;
  const uintptr_t bias = reinterpret_cast<uintptr_t>(self.dli_fbase);
  for (const Elf64_Shdr& s : sh) {
    if (s.sh_type != SHT_SYMTAB || s.sh_link >= sh.size()) continue;
    const Elf64_Shdr& strs = sh[s.sh_link];
    std::vector<char> names(strs.sh_size);
    f.seekg(std::streamoff(strs.sh_offset));
    if (!f.read(names.data(), std::streamsize(names.size()))) return;
    std::vector<Elf64_Sym> syms(s.sh_size / sizeof(Elf64_Sym));
    f.seekg(std::streamoff(s.sh_offset));
    if (!f.read(reinterpret_cast<char*>(syms.data()), std::streamsize(syms.size() * sizeof(Elf64_Sym)))) return;
    for (const Elf64_Sym& y : syms) {
      if (ELF64_ST_TYPE(y.st_info) != STT_FUNC || !y.st_value || !y.st_size || y.st_name >= names.size()) continue;
      g_symbols.push_back({bias + y.st_value, bias + y.st_value + y.st_size, std::string(&names[y.st_name])});
    }
  }
  std::sort(g_symbols.begin(), g_symbols.end(), [](const Symbol& a, const Symbol& b) { return a.start < b.start; });
}

std::string Demangled(const std::string& name) {
  if (name.rfind("_Z", 0) != 0) return name;
  int status = 0;
  char* d = abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status);
  std::string out = status == 0 && d ? d : name;
  std::free(d);
  // "bf1943::(anonymous namespace)::Foo(int, ...)" is enough as "bf1943::Foo"; the arguments left out.
  if (const size_t paren = out.find('('); paren != std::string::npos) out.resize(paren);
  for (const char* drop : {"(anonymous namespace)::", "std::__cxx11::"})
    for (size_t at; (at = out.find(drop)) != std::string::npos;) out.erase(at, std::strlen(drop));
  return out.size() > 80 ? out.substr(0, 80) + "..." : out;
}
#endif

// ---- the samples (a ring; the handler writes, a dump reads)

struct Sample {
  std::atomic<int64_t> t_us{0};
  std::atomic<uintptr_t> pc{0};
  std::atomic<uint32_t> lr{0};  // the game's link register then: its caller, for a sample outside the game's code
};
std::atomic<const PPCContext*> g_context{nullptr};  // the frame thread's guest context (NoteFrameThread)
constexpr size_t kSamples = 1 << 15;  // 32 s at 1000 a second
Sample g_samples[kSamples];
std::atomic<uint64_t> g_next{0};
std::atomic<bool> g_running{false};

int64_t MonotonicUs() {
#if defined(_WIN32)
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#else
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);  // steady_clock's clock on Linux (the black box's times)
  return int64_t(ts.tv_sec) * 1'000'000 + ts.tv_nsec / 1000;
#endif
}

void Store(uintptr_t pc) {
  const uint64_t i = g_next.fetch_add(1, std::memory_order_relaxed) % kSamples;
  g_samples[i].pc.store(pc, std::memory_order_relaxed);
  const PPCContext* ctx = g_context.load(std::memory_order_relaxed);
  g_samples[i].lr.store(ctx ? uint32_t(ctx->lr) : 0, std::memory_order_relaxed);  // a plain read: racy, harmless
  g_samples[i].t_us.store(MonotonicUs(), std::memory_order_release);
}

#if defined(_WIN32)
std::atomic<DWORD> g_thread_id{0};
#else
std::atomic<pid_t> g_tid{0};
const int kSampleSignal = SIGRTMAX - 2;
void OnSample(int, siginfo_t*, void* uc) {
  const auto* u = static_cast<const ucontext_t*>(uc);
#if defined(__x86_64__)
  Store(uintptr_t(u->uc_mcontext.gregs[REG_RIP]));
#elif defined(__aarch64__)
  Store(uintptr_t(u->uc_mcontext.pc));
#endif
}
#endif

void SamplerThread() {
#if defined(_WIN32)
  HANDLE thread = nullptr;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!thread) {
      const DWORD id = g_thread_id.load();
      if (!id) continue;
      thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
      if (!thread) return;
    }
    if (!Want(Area::kBlackBox, Level::kDetailed)) continue;  // the verbosity lowered in the game: no samples
    if (SuspendThread(thread) == DWORD(-1)) continue;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_CONTROL;
    const bool ok = GetThreadContext(thread, &c);
    ResumeThread(thread);
    if (ok) Store(uintptr_t(c.Rip));
  }
#else
  const pid_t pid = getpid();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const pid_t tid = g_tid.load();
    if (tid && Want(Area::kBlackBox, Level::kDetailed)) syscall(SYS_tgkill, pid, tid, kSampleSignal);
  }
#endif
}

// The game function holding a guest address (the table's guest starts, sorted once).
std::vector<uint32_t> g_guest_starts;
std::string GuestFunctionOf(uint32_t guest) {
  if (g_guest_starts.empty() || guest < g_guest_starts.front()) return {};
  auto it = std::upper_bound(g_guest_starts.begin(), g_guest_starts.end(), guest);
  return fmt::format("sub_{:08X}", *std::prev(it));
}

// A sample's name: the game's function (by the table), or the module it is in.
std::string NameOf(uintptr_t pc) {
  // Outside the game's own binary: the module it is in (libc: a system call, the SDK, the graphics driver).
  {
#if defined(_WIN32)
    HMODULE module = nullptr, self = GetModuleHandleA(nullptr);
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(pc), &module) && module && module != self) {
      char path[MAX_PATH] = {};
      GetModuleFileNameA(module, path, MAX_PATH);
      const char* name = std::strrchr(path, '\\');
      return name ? name + 1 : path;
    }
#else
    static Dl_info self{};
    static const bool have_self = dladdr(reinterpret_cast<void*>(&ReadOwnSymbols), &self) != 0;
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname && have_self && info.dli_fbase != self.dli_fbase) {
      const char* name = std::strrchr(info.dli_fname, '/');
      std::string out = name ? name + 1 : info.dli_fname;
      if (info.dli_sname) out += std::string(" ") + Demangled(info.dli_sname);  // an exported symbol: its name too
      return out;
    }
#endif
  }
#if !defined(_WIN32)
  if (!g_symbols.empty()) {
    auto it = std::upper_bound(g_symbols.begin(), g_symbols.end(), pc,
                               [](uintptr_t v, const Symbol& s) { return v < s.start; });
    if (it != g_symbols.begin() && pc < std::prev(it)->end) {
      std::string name = std::prev(it)->name;
      // A hooked game function's own body is __imp__sub_...: the game's code all the same.
      if (name.rfind("__imp__sub_", 0) == 0) name = name.substr(7);
      return Demangled(name);
    }
  }
#endif
  if (pc >= g_table_low && pc <= g_table_high && !g_table.empty()) {
    auto it = std::upper_bound(g_table.begin(), g_table.end(), std::make_pair(pc, UINT32_MAX));
    if (it != g_table.begin()) return fmt::format("sub_{:08X} (nearest)", std::prev(it)->second);
  }
#if defined(_WIN32)
  HMODULE module = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(pc), &module) && module) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* name = std::strrchr(path, '\\');
    return name ? name + 1 : path;
  }
#else
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
    const char* name = std::strrchr(info.dli_fname, '/');
    return name ? name + 1 : info.dli_fname;
  }
#endif
  return "?";
}

}  // namespace

void StartProfiler() {
  static std::atomic<bool> started{false};
  if (!Want(Area::kBlackBox, Level::kDetailed) || started.exchange(true)) return;
  Collector c;
  bf1943_RegisterFunctions(&c);
  std::sort(c.table.begin(), c.table.end());
  g_table = std::move(c.table);
  if (!g_table.empty()) g_table_low = g_table.front().first, g_table_high = g_table.back().first + 0x10000;
  for (const auto& [host, guest] : g_table) g_guest_starts.push_back(guest);
  std::sort(g_guest_starts.begin(), g_guest_starts.end());
#if !defined(_WIN32)
  ReadOwnSymbols();
#endif
#if !defined(_WIN32)
  struct sigaction sa{};
  sa.sa_sigaction = OnSample;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  if (sigaction(kSampleSignal, &sa, nullptr) != 0) return;
#endif
  g_running = true;
  std::thread(SamplerThread).detach();
  REXLOG_INFO("BF profiler: the game frame thread's code sampled about 1000 times a second while the verbosity is "
              "Detailed or Verbose ({} game functions known, {} "
              "symbols {})",
              g_table.size(), g_symbols.size(), g_symbols.empty() ? "(a stripped binary: game functions only, the nearest)" : "with their sizes");
}

void NoteFrameThread() {
  if (!g_running.load(std::memory_order_relaxed)) return;
  if (!g_context.load(std::memory_order_relaxed))
    if (auto* ts = rex::runtime::ThreadState::Get()) g_context.store(ts->context(), std::memory_order_relaxed);
#if defined(_WIN32)
  DWORD zero = 0;
  g_thread_id.compare_exchange_strong(zero, GetCurrentThreadId());
#else
  pid_t zero = 0;
  g_tid.compare_exchange_strong(zero, pid_t(syscall(SYS_gettid)));
#endif
}

std::string TopFunctions(int64_t from_us, int64_t to_us, int n) {
  if (!g_running.load()) return {};
  std::map<std::string, int> counts;
  int total = 0;
  for (const Sample& s : g_samples) {
    const int64_t t = s.t_us.load(std::memory_order_acquire);
    if (t < from_us || t > to_us) continue;
    std::string name = NameOf(s.pc.load(std::memory_order_relaxed));
    // Outside the game's code (libc, the SDK, the driver): who in the game called it.
    if (name.rfind("sub_", 0) != 0)
      if (const std::string caller = GuestFunctionOf(s.lr.load(std::memory_order_relaxed)); !caller.empty())
        name += " from the game's " + caller;
    ++counts[name];
    ++total;
  }
  if (!total) return {};
  std::vector<std::pair<int, std::string>> top;
  for (const auto& [name, c] : counts) top.push_back({c, name});
  std::sort(top.rbegin(), top.rend());
  std::string out;
  // Under 1% left out (after the first three): noise in a line meant to say where the time went.
  for (size_t i = 0; i < top.size() && int(i) < n && (i < 3 || top[i].first * 100 >= total); ++i)
    out += fmt::format("{}{} {:.0f}%", i ? ", " : "", top[i].second, 100.0 * top[i].first / total);
  return fmt::format("{} ({} samples)", out, total);
}

}  // namespace bf1943::diagnostics
