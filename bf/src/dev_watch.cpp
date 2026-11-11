// Developer aid (BF_DEV_TOOLS builds only, Windows): a hardware data watch on 4 bytes of guest memory. Every thread
// gets the address in debug register 0 (read or write); each hit is counted by the host instruction after the access,
// and when the watch ends the counts are logged by function (the recompiled sub_XXXXXXXX names from bf1943.pdb, which
// tools\run_game.ps1 copies when BF_DEV_WATCH is set). For finding which code reads a value, such as the aim's yaw.

#if defined(BF_DEV_TOOLS) && defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>

namespace bf1943::dev {
namespace {

constexpr int kSlots = 4096;
struct Slot {
  std::atomic<uint64_t> pc{0};
  std::atomic<uint32_t> hits{0};
  std::atomic<uint32_t> thread{0};
  std::atomic<uint32_t> regs{0};  // which debug registers (watched addresses) this place hit, a bit each
};
Slot g_slots[kSlots];
std::atomic<bool> g_armed{false};
std::atomic<bool> g_started{false};
std::atomic<uint32_t> g_register_hits[4];

void Count(uint64_t pc, uint32_t regs) {
  uint32_t i = uint32_t((pc * 0x9E3779B97F4A7C15ull) >> 52) & (kSlots - 1);
  for (int n = 0; n < kSlots; ++n, i = (i + 1) & (kSlots - 1)) {
    uint64_t expected = 0;
    const uint64_t have = g_slots[i].pc.load(std::memory_order_relaxed);
    if (have == pc || (have == 0 && g_slots[i].pc.compare_exchange_strong(expected, pc))) {
      g_slots[i].hits.fetch_add(1, std::memory_order_relaxed);
      g_slots[i].thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
      g_slots[i].regs.fetch_or(regs, std::memory_order_relaxed);
      return;
    }
    if (expected == pc) {
      g_slots[i].hits.fetch_add(1, std::memory_order_relaxed);
      g_slots[i].regs.fetch_or(regs, std::memory_order_relaxed);
      return;
    }
  }
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT* c = info->ContextRecord;
  if (!(c->Dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
  for (int i = 0; i < 4; ++i)
    if (c->Dr6 & (1ull << i)) g_register_hits[i].fetch_add(1, std::memory_order_relaxed);
  const uint32_t regs = uint32_t(c->Dr6 & 0xF);
  c->Dr6 = 0;
  if (g_armed.load(std::memory_order_relaxed)) Count(c->Rip, regs);
  return EXCEPTION_CONTINUE_EXECUTION;
}

void SetAll(uint64_t address) {
  const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  int set = 0;
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
    HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
    if (!h) continue;
    if (SuspendThread(h) != DWORD(-1)) {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(h, &c)) {
        c.Dr0 = address;
        // L0 on, RW0 = 11 (read or write), LEN0 = 11 (4 bytes); or all off.
        c.Dr7 = address ? (c.Dr7 & ~0xF0003ull) | 1ull | (3ull << 16) | (3ull << 18) : (c.Dr7 & ~0xF0003ull);
        c.Dr6 = 0;
        if (SetThreadContext(h, &c)) ++set;
      }
      ResumeThread(h);
    }
    CloseHandle(h);
  }
  CloseHandle(snap);
  REXLOG_INFO("BF watch: {} threads {}", set, address ? "watching" : "cleared");
}

void Report(const char* label) {
  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);
  struct Row {
    uint64_t pc;
    uint32_t hits, thread, regs;
  };
  std::vector<Row> rows;
  for (auto& s : g_slots)
    if (const uint64_t pc = s.pc.load()) rows.push_back({pc, s.hits.load(), s.thread.load(), s.regs.load()});
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.hits > b.hits; });
  REXLOG_INFO("BF watch {}: {} places; hits by debug register {} {} {} {}", label, rows.size(),
              g_register_hits[0].load(), g_register_hits[1].load(), g_register_hits[2].load(),
              g_register_hits[3].load());
  for (size_t i = 0; i < rows.size() && i < 60; ++i) {
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 displacement = 0;
    std::string name = "?";
    if (SymFromAddr(process, rows[i].pc, &displacement, sym)) {
      name = sym->Name;
    } else {
      HMODULE module = nullptr;
      if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(rows[i].pc), &module)) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* file = std::strrchr(path, '\\');
        name = std::string(file ? file + 1 : path);
        displacement = rows[i].pc - DWORD64(module);
      }
    }
    REXLOG_INFO("BF watch {}: {} +{:X}: {} hits (thread {}, addresses {}{}{}{})", label, name, uint64_t(displacement),
                rows[i].hits, rows[i].thread, rows[i].regs & 1 ? "1" : "", rows[i].regs & 2 ? "2" : "",
                rows[i].regs & 4 ? "3" : "", rows[i].regs & 8 ? "4" : "");
  }
}

// Up to four addresses, writes only (RW = 01, LEN = 11), in DR0..DR3; all off for an empty list.
void SetAllWrites(const std::vector<uint64_t>& addresses, bool reads = false) {
  const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
    HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
    if (!h) continue;
    if (SuspendThread(h) != DWORD(-1)) {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(h, &c)) {
        DWORD64* dr[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
        c.Dr7 &= ~0xFFFF00FFull;
        for (size_t i = 0; i < 4; ++i) {
          *dr[i] = i < addresses.size() ? addresses[i] : 0;
          if (i < addresses.size())
            c.Dr7 |= (1ull << (i * 2)) | ((reads ? 3ull : 1ull) << (16 + i * 4)) | (3ull << (18 + i * 4));
        }
        c.Dr6 = 0;
        SetThreadContext(h, &c);
      }
      ResumeThread(h);
    }
    CloseHandle(h);
  }
  CloseHandle(snap);
}

}  // namespace

// Watches writes to up to four host addresses (4 bytes each) for `seconds`, setting the threads again every 250 ms so
// that new ones are watched too, then logs who wrote them. Once per process.
void WatchHostWrites(std::vector<const void*> addresses, double seconds, const char* label) {
  if (g_started.exchange(true)) return;
  std::vector<uint64_t> a;
  for (const void* p : addresses) a.push_back(reinterpret_cast<uint64_t>(p));
  if (a.size() > 4) a.resize(4);
  std::string name = label;
  std::thread([a, seconds, name] {
    AddVectoredExceptionHandler(1, Handler);
    g_armed = true;
    REXLOG_INFO("BF watch {}: watching writes to {} addresses", name, a.size());
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
      SetAllWrites(a);
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    SetAllWrites({});
    g_armed = false;
    Report(name.c_str());
  }).detach();
}

// As WatchHostWrites, reads and writes (RW = 11): up to four addresses, for one value seen through its guest aliases.
void WatchHostAccess(std::vector<const void*> addresses, double seconds, const char* label) {
  if (g_started.exchange(true)) return;
  std::vector<uint64_t> a;
  for (const void* p : addresses) a.push_back(reinterpret_cast<uint64_t>(p));
  if (a.size() > 4) a.resize(4);
  std::string name = label;
  std::thread([a, seconds, name] {
    AddVectoredExceptionHandler(1, Handler);
    g_armed = true;
    REXLOG_INFO("BF watch {}: watching reads and writes of {} addresses", name, a.size());
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
      SetAllWrites(a, true);
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    SetAllWrites({});
    g_armed = false;
    Report(name.c_str());
  }).detach();
}

// Watches the 4 bytes at host address `address` for `seconds`, then logs who touched them. Once per process.
void WatchHost(const void* address, double seconds, const char* label) {
  if (g_started.exchange(true)) return;
  const uint64_t a = reinterpret_cast<uint64_t>(address);
  std::string name = label;
  std::thread([a, seconds, name] {
    AddVectoredExceptionHandler(1, Handler);
    g_armed = true;
    SetAll(a);
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    SetAll(0);
    g_armed = false;
    Report(name.c_str());
  }).detach();
}

}  // namespace bf1943::dev

#elif defined(BF_DEV_TOOLS)
// Developer builds off Windows: the hardware watches use Windows' debug registers and exception handlers.
#include <vector>

#include <rex/logging.h>

namespace bf1943::dev {
void WatchHostWrites(std::vector<const void*>, double, const char* label) {
  REXLOG_INFO("BF watch {}: hardware watches are Windows only", label);
}
void WatchHostAccess(std::vector<const void*>, double, const char* label) {
  REXLOG_INFO("BF watch {}: hardware watches are Windows only", label);
}
}  // namespace bf1943::dev
#endif  // BF_DEV_TOOLS && _WIN32
