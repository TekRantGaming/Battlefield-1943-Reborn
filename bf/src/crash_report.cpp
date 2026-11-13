// Crash and hang reports (King Kong Recompiled's design, kk/src/crash_dump.cpp, MIT, TekRantGaming; handover pack 04
// section 4), kept small: <user>\crashes\crash-<time>.dmp and .txt for a crash, hang-<time>.dmp and .txt when no
// game frame came for 20 s. The text has the exception, the faulting module and offset, up to 16 callers as module +
// offset (decoded with the build's bf1943.map), then the stack's third party modules and the overlays hooked into the
// game (overlay_hooks.inc; also logged at the start). Module paths and user names are never written. Windows only.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/flags.h>
#include <rex/ppc/func.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(bf_null_page_guard, true, "BF1943",
                    "Reads of the game's null page give zeros (each logged) instead of a crash (Windows)");

namespace bf1943 {
namespace {

std::filesystem::path g_dir;
std::atomic<int64_t> g_last_frame_ms{0};
std::atomic<bool> g_writing{false};
std::atomic<bool> g_hang_written{false};
constexpr int64_t kHangMs = 20000;
int64_t g_start_ms = 0;             // InstallCrashReports
std::wstring g_exe_dir, g_windows;  // the game's own folder and Windows' (ThirdParty)

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

#include "overlay_hooks.inc"  // the known overlay and hook DLLs; HookedOverlays(), the ones in the process now
std::mutex g_overlays_mutex;
std::string g_overlays_seen;  // the set the last line logged

// "RivaTuner Statistics Server (RTSSHooks64.dll, RTSSVkLayer64.dll), OBS Studio (graphics-hook64.dll)"; "" with none.
std::string OverlaysText(const std::vector<HookedOverlay>& hooked) {
  std::string text;
  for (const HookedOverlay& h : hooked) text += (text.empty() ? "" : ", ") + h.product + " (" + h.files + ")";
  return text;
}

// The log's line on them, "BF overlays<when>: <products and their DLLs> is (are) hooked into the game" or "...: none":
// always, or (`changes`) only when the set differs from the last line's.
void LogOverlays(const std::string& when, bool changes) {
  const std::vector<HookedOverlay> hooked = HookedOverlays();
  const std::string text = OverlaysText(hooked);
  std::lock_guard<std::mutex> lock(g_overlays_mutex);
  if (changes && text == g_overlays_seen) return;
  g_overlays_seen = text;
  if (hooked.empty()) REXLOG_INFO("BF overlays{}: none", when);
  else REXLOG_INFO("BF overlays{}: {} {} hooked into the game", when, text, hooked.size() == 1 ? "is" : "are");
}

// The product a module file name is known as (the table above), "" for any other.
std::string OverlayProductOf(const std::wstring& file) {
  for (const OverlayHook& h : kOverlayHooks)
    if (_wcsicmp(file.c_str(), h.file) == 0) return h.product;
  return "";
}

// Whether a module (by its full path) is neither the game's own (bf1943.exe and the DLLs beside it) nor Windows' or a
// driver's (under the Windows folder: System32, the DriverStore): an overlay's hook, a capture's, an antivirus's,
// anything injected. "" (no module) is not.
bool ThirdParty(const std::wstring& path) {
  const auto under = [&path](const std::wstring& dir) {
    return !dir.empty() && path.size() > dir.size() && _wcsnicmp(path.c_str(), dir.c_str(), dir.size()) == 0 &&
           (path[dir.size()] == L'\\' || path[dir.size()] == L'/');
  };
  return !path.empty() && !under(g_exe_dir) && !under(g_windows);
}

std::string Stamp() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char b[32];
  std::strftime(b, sizeof(b), "%Y%m%d-%H%M%S", &tm);
  return b;
}

// "module + 0xoffset" for an address, the module by file name only; `path` (never written) gets its full path.
std::string Where(uint64_t address, std::wstring* path = nullptr) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &module) ||
      !module) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(address));
    return b;
  }
  wchar_t p[MAX_PATH] = {};
  GetModuleFileNameW(module, p, MAX_PATH);
  if (path) *path = p;
  std::string name = std::filesystem::path(p).filename().string();
  char b[64];
  std::snprintf(b, sizeof(b), " + 0x%llx", static_cast<unsigned long long>(address - reinterpret_cast<uint64_t>(module)));
  return name + b;
}

// The recompiled game function (its Xbox 360 address) a host address is in: the one starting where the address's
// unwind data says its function starts, else (a function without unwind data) the closest start below it, marked
// approximate. 0: not the game's code (ours, the runtime's or a system module's).
uint32_t GuestFunctionOf(uint64_t host, bool* approximate) {
  DWORD64 image = 0;
  uint64_t begin = 0;
  if (PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(host, &image, nullptr)) begin = image + fn->BeginAddress;
  uint64_t best = 0;
  uint32_t guest = 0;
  for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m) {
    const uint64_t h = reinterpret_cast<uint64_t>(m->host);
    if (begin && h == begin) {
      *approximate = false;
      return uint32_t(m->guest);
    }
    if (!begin && h <= host && h > best) best = h, guest = uint32_t(m->guest);
  }
  *approximate = true;
  return begin ? 0 : guest;
}

// A guest word, or 0 from a page that is not committed and readable (this runs inside an exception handler).
uint32_t LoadGuest32(const uint8_t* membase, uint32_t address) {
  const uint8_t* p = membase + address;
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
      reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize < p + 4)
    return 0;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
bool PlausibleStack(uint32_t address) { return address >= 0x70000000 && address < 0x80000000 && (address & 3) == 0; }

std::string Hex(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof(b), "%08llX", static_cast<unsigned long long>(v));
  return b;
}

// The report's text: the exception, the host callers with the game function each is in, and the crashing thread's
// guest registers and guest call stack (PowerPC frames as the recompiled code builds them: [r1] holds the caller's r1,
// the caller's frame keeps the saved LR at back chain - 8). The same lines go into bf1943.log (12 Nov 2026: a
// player's log of a crash at a match's end had only "read of guest 0x00000000"; the crashes folder is not sent).
std::string ReportText(const char* kind, EXCEPTION_POINTERS* ep, const std::string& reason) {
  std::string out = std::string("Battlefield 1943 ") + kind + " report\nReason: " + reason + "\n";
  if (!ep || !ep->ExceptionRecord) return out;
  const EXCEPTION_RECORD* er = ep->ExceptionRecord;
  char b[160];
  std::snprintf(b, sizeof(b), "Exception: 0x%08lx at %s\nThread: %lu\n", er->ExceptionCode,
                Where(reinterpret_cast<uint64_t>(er->ExceptionAddress)).c_str(), GetCurrentThreadId());
  out += b;
  if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
    std::snprintf(b, sizeof(b), "Access: %s of 0x%llx\n", er->ExceptionInformation[0] == 1 ? "write" : "read",
                  static_cast<unsigned long long>(er->ExceptionInformation[1]));
    out += b;
  }
  // The callers, by the x64 unwind data from the exception's context (a function without unwind data ends the walk).
  CONTEXT context = *ep->ContextRecord;
  std::vector<std::wstring> others;  // the stack's modules neither the game's own nor Windows', each once
  for (int i = 0; i < 16 && context.Rip; ++i) {
    bool approximate = false;
    const uint32_t guest = GuestFunctionOf(context.Rip, &approximate);
    std::wstring path;
    std::string line = "  " + std::to_string(i) + " " + Where(context.Rip, &path);
    if (guest) line += std::string(" (game function ") + (approximate ? "near sub_" : "sub_") + Hex(guest) + ")";
    out += line + "\n";
    if (ThirdParty(path)) {
      const std::wstring file = std::filesystem::path(path).filename().wstring();
      bool seen = false;
      for (const std::wstring& o : others) seen = seen || _wcsicmp(o.c_str(), file.c_str()) == 0;
      if (!seen) others.push_back(file);
    }
    DWORD64 image = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
    if (!function) break;
    PVOID handler_data = nullptr;
    DWORD64 frame = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context, &handler_data, &frame, nullptr);
  }
  // What else is in the stack (ThirdParty), a known overlay with its name: an injected hook shows here (12 Nov 2026: a
  // player's v0.3.0 crashed at its first frame in five frames of RivaTuner Statistics Server's, and nothing named it).
  // Then the overlays hooked into the game at the time, in the stack or not.
  std::string third;
  for (const std::wstring& file : others) {
    const std::string product = OverlayProductOf(file);
    third += (third.empty() ? "" : ", ") + std::filesystem::path(file).string() +
             (product.empty() ? "" : " (" + product + ")");
  }
  out += "Third party in the stack: " + (third.empty() ? std::string("none") : third) + "\n";
  const std::string hooked = OverlaysText(HookedOverlays());
  out += "Overlays hooked in: " + (hooked.empty() ? std::string("none") : hooked) + "\n";
  auto* ts = rex::runtime::ThreadState::Get();
  PPCContext* ctx = ts ? ts->context() : nullptr;
  if (!ctx) return out + "No game thread\n";
  std::snprintf(b, sizeof(b), "Game thread %u: lr %08llX ctr %08llX r1 %08X\n", ts->thread_id(),
                static_cast<unsigned long long>(ctx->lr), static_cast<unsigned long long>(ctx->ctr.u64), ctx->r1.u32);
  out += b;
  std::snprintf(b, sizeof(b), "  r3 %08X r4 %08X r5 %08X r6 %08X r7 %08X r8 %08X r9 %08X r10 %08X r11 %08X\n",
                ctx->r3.u32, ctx->r4.u32, ctx->r5.u32, ctx->r6.u32, ctx->r7.u32, ctx->r8.u32, ctx->r9.u32,
                ctx->r10.u32, ctx->r11.u32);
  out += b;
  std::snprintf(b, sizeof(b), "  r26 %08X r27 %08X r28 %08X r29 %08X r30 %08X r31 %08X\n", ctx->r26.u32, ctx->r27.u32,
                ctx->r28.u32, ctx->r29.u32, ctx->r30.u32, ctx->r31.u32);
  out += b;
  const uint8_t* membase = ts->memory() ? ts->memory()->virtual_membase() : nullptr;
  if (!membase) return out;
  std::string calls = "  Game call stack (return addresses):";
  uint32_t sp = ctx->r1.u32;
  for (int i = 0; i < 24 && PlausibleStack(sp); ++i) {
    const uint32_t back = LoadGuest32(membase, sp);
    if (!PlausibleStack(back) || back <= sp) break;
    calls += " " + Hex(LoadGuest32(membase, back - 8));
    sp = back;
  }
  return out + calls + "\n";
}

void WriteReport(const char* kind, EXCEPTION_POINTERS* ep, const std::string& reason) {
  if (g_dir.empty() || g_writing.exchange(true)) return;
  std::error_code ec;
  std::filesystem::create_directories(g_dir, ec);
  const std::string stem = std::string(kind) + "-" + Stamp();
  const std::filesystem::path dmp = g_dir / (stem + ".dmp"), txt = g_dir / (stem + ".txt");
  // The text first (the minidump can take seconds, and the log is what players send).
  const std::string text = ReportText(kind, ep, reason) + "Minidump: " + dmp.filename().string() + "\n";
  if (FILE* f = _wfopen(txt.wstring().c_str(), L"w")) {
    std::fputs(text.c_str(), f);
    std::fclose(f);
  }
  for (size_t at = 0; at < text.size();) {
    const size_t end = std::min(text.find('\n', at), text.size());
    if (end > at) REXLOG_ERROR("BF {} report: {}", kind, text.substr(at, end - at));
    at = end + 1;
  }
  HANDLE file = CreateFileW(dmp.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), ep, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                      ep ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
  }
  g_writing = false;
}

LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* ep) {
  WriteReport("crash", ep, "unhandled exception");
  return EXCEPTION_CONTINUE_SEARCH;
}

// Fatal codes no normal handler sees (fail fast, heap corruption), noted before anything can swallow them.
LONG WINAPI OnVectored(EXCEPTION_POINTERS* ep) {
  const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
  if (code == 0xC0000409 || code == 0xC0000374 || code == EXCEPTION_STACK_OVERFLOW) {
    WriteReport("crash", ep, code == EXCEPTION_STACK_OVERFLOW ? "stack overflow" : "fail fast or heap corruption");
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

// The crashes folder and the handlers; the hang watchdog starts with the first game frame.
void InstallCrashReports(const std::filesystem::path& user_root) {
  g_dir = user_root / "crashes";
  g_start_ms = NowMs();
  {
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    g_exe_dir = std::filesystem::path(p).parent_path().wstring();
    wchar_t w[MAX_PATH] = {};
    GetWindowsDirectoryW(w, MAX_PATH);
    g_windows = w;
  }
  SetUnhandledExceptionFilter(OnUnhandled);
  AddVectoredExceptionHandler(0, OnVectored);
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      // A hook that came after the start (RivaTuner's with the first Vulkan device, OBS's when its capture starts): a
      // line when the set changes.
      LogOverlays(", " + std::to_string((NowMs() - g_start_ms) / 1000) + " s after the start", true);
      const int64_t last = g_last_frame_ms.load();
      if (!last) continue;
      const int64_t stalled = NowMs() - last;
      if (stalled > kHangMs && !g_hang_written.exchange(true)) {
        REXLOG_ERROR("BF hang: no game frame for {} s, writing a hang report", stalled / 1000);
        WriteReport("hang", nullptr, "no game frame for " + std::to_string(stalled / 1000) + " s");
      } else if (stalled < kHangMs) {
        g_hang_written = false;
      }
    }
  }).detach();
  REXLOG_INFO("BF crash reports: on (crashes folder in the user data)");
  LogOverlays("", false);  // the hooks in already; later ones: the poll above and the first game frame
}

// The guest's null page (guest 0x0 to 0xFFFF; the runtime keeps it without access, protect_zero): a read there gives
// zeros instead of a crash, each one logged (the first 16 with the game function, then a count). 12 Nov 2026: a
// player's game crashed as a match ended, "read of guest 0x00000000" on one of the game's job threads; the game's
// server code had a read at guest 0x8 on 11 Nov (game_fixes.cpp). The read is let through by making the page
// readable for that one instruction (the trap flag's single step puts the protection back), so the next is seen too;
// writes there still crash. After 100000 reads the page stays readable (no more steps). bf_null_page_guard false
// keeps the runtime's crash.
namespace {
uint8_t* g_null_page = nullptr;  // the guest's 0x0 on the host
std::mutex g_null_mutex;
int g_null_steppers = 0;          // threads between the read and its step (the page readable meanwhile)
std::atomic<uint64_t> g_null_reads{0};
bool g_null_open = false;         // readable for good
thread_local bool t_null_stepping = false;
constexpr uint32_t kNullPageSize = 0x10000;

LONG CALLBACK NullPageGuard(EXCEPTION_POINTERS* ep) {
  const EXCEPTION_RECORD* er = ep->ExceptionRecord;
  if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && t_null_stepping) {
    t_null_stepping = false;
    std::lock_guard<std::mutex> lock(g_null_mutex);
    DWORD old = 0;
    if (--g_null_steppers == 0 && !g_null_open) VirtualProtect(g_null_page, kNullPageSize, PAGE_NOACCESS, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || er->ExceptionInformation[0] != 0)
    return EXCEPTION_CONTINUE_SEARCH;
  const uint64_t at = er->ExceptionInformation[1];
  const uint64_t page = reinterpret_cast<uint64_t>(g_null_page);
  if (at < page || at >= page + kNullPageSize || t_null_stepping) return EXCEPTION_CONTINUE_SEARCH;
  const uint64_t n = g_null_reads.fetch_add(1) + 1;
  if (n <= 16 || (n & (n - 1)) == 0) {
    bool approximate = false;
    const uint32_t guest = GuestFunctionOf(ep->ContextRecord->Rip, &approximate);
    auto* ts = rex::runtime::ThreadState::Get();
    PPCContext* ctx = ts ? ts->context() : nullptr;
    const std::string where = guest ? std::string(approximate ? "near sub_" : "in sub_") + Hex(guest)
                                    : std::string("outside the game's code: ") + Where(ep->ContextRecord->Rip);
    REXLOG_WARN("BF null page: the game read guest 0x{:08X} (null), read {} times so far; zeros given, play goes on "
                "({}, game thread {}, lr {:08X})",
                uint32_t(at - page), n, where, ts ? ts->thread_id() : 0, ctx ? uint32_t(ctx->lr) : 0);
  }
  std::lock_guard<std::mutex> lock(g_null_mutex);
  DWORD old = 0;
  if (n >= 100000 && !g_null_open) {
    g_null_open = true;
    REXLOG_WARN("BF null page: {} reads: it stays readable from now on", n);
  }
  if (g_null_steppers++ == 0) VirtualProtect(g_null_page, kNullPageSize, PAGE_READONLY, &old);
  t_null_stepping = true;
  ep->ContextRecord->EFlags |= 0x100;  // the trap flag: a single step after the read
  return EXCEPTION_CONTINUE_EXECUTION;
}

void InstallNullPageGuard() {
  if (!REXCVAR_GET(bf_null_page_guard) || REXCVAR_GET(protect_zero) == false) return;
  auto* ts = rex::runtime::ThreadState::Get();
  if (!ts || !ts->memory()) return;
  g_null_page = ts->memory()->virtual_membase();
  // First in the chain, ahead of the runtime's own handler (installed with the guest memory, before the first frame),
  // which would log the read as unhandled and pass it on to the crash.
  AddVectoredExceptionHandler(1, NullPageGuard);
  REXLOG_INFO("BF null page: reads of the game's null page give zeros (logged) instead of a crash");
}
}  // namespace

// The null page guard, once: at the first game frame, or at a dedicated server's first tick (a server draws no
// frame, so until 13 Nov 2026 it never had the guard: official server 8 crashed on a read of guest 0x8C in
// sub_822A90D8, a vehicle's physics, 30 s after a player joined).
void InstallNullPageGuardOnce() {
  static const bool guard = (InstallNullPageGuard(), true);
  (void)guard;
}

// Each game frame (VdSwap): the hang watchdog's clock.
void NoteGuestFrame() {
  g_last_frame_ms = NowMs();
  InstallNullPageGuardOnce();
  // The overlays once more at the first game frame, beside the renderer's first present (where a hook's crash comes).
  static const bool overlays = (LogOverlays(" at the first game frame", false), true);
  (void)overlays;
#if defined(BF_DEV_TOOLS)
  // BF_DEV_NULL_READ_AT=<s> (tests of the null page guard): reads of the guest's 0x8 that long after the first frame,
  // then each 5 s.
  {
    static const double null_at = [] {
      const char* v = std::getenv("BF_DEV_NULL_READ_AT");
      return v && *v ? std::atof(v) : -1.0;
    }();
    static const int64_t start = NowMs();
    static double next = null_at;
    const double t = double(NowMs() - start) / 1000.0;
    if (null_at >= 0 && t > next && g_null_page) {
      next = t + 5;
      const uint32_t v = *reinterpret_cast<volatile const uint32_t*>(g_null_page + 8);
      REXLOG_INFO("BF dev null read: guest 0x8 read as {:08X}", v);
    }
  }
#endif
#if defined(BF_DEV_TOOLS)
  // BF_DEV_CRASH_AT=<s> / BF_DEV_HANG_AT=<s> (tests of the reports): a crash, or a 25 s stall, that long after the
  // first frame.
  static const int64_t first = NowMs();
  static const double crash_at = [] {
    const char* v = std::getenv("BF_DEV_CRASH_AT");
    return v && *v ? std::atof(v) : -1.0;
  }();
  static const double hang_at = [] {
    const char* v = std::getenv("BF_DEV_HANG_AT");
    return v && *v ? std::atof(v) : -1.0;
  }();
  static bool hung = false;
  const double t = double(NowMs() - first) / 1000.0;
  if (hang_at >= 0 && t > hang_at && !hung) hung = true, std::this_thread::sleep_for(std::chrono::seconds(25));
  if (crash_at >= 0 && t > crash_at) *static_cast<volatile int*>(nullptr) = 1;
#endif
}

}  // namespace bf1943

#else

#include <filesystem>
namespace bf1943 {
void InstallCrashReports(const std::filesystem::path&) {}
void NoteGuestFrame() {}
void InstallNullPageGuardOnce() {}
}  // namespace bf1943

#endif
