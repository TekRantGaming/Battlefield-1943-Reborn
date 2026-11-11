// Crash and hang reports (King Kong Recompiled's design, kk/src/crash_dump.cpp, MIT, TekRantGaming; handover pack 04
// section 4), kept small: <user>\crashes\crash-<time>.dmp and .txt for a crash, hang-<time>.dmp and .txt when no
// game frame came for 20 s. The text has the exception, the faulting module and offset, and up to 16 callers as
// module + offset (decoded with the build's bf1943.map). Module paths and user names are never written. Windows only.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>

#include <rex/logging.h>

namespace bf1943 {
namespace {

std::filesystem::path g_dir;
std::atomic<int64_t> g_last_frame_ms{0};
std::atomic<bool> g_writing{false};
std::atomic<bool> g_hang_written{false};
constexpr int64_t kHangMs = 20000;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string Stamp() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char b[32];
  std::strftime(b, sizeof(b), "%Y%m%d-%H%M%S", &tm);
  return b;
}

// "module + 0xoffset" for an address, the module by file name only.
std::string Where(uint64_t address) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &module) ||
      !module) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(address));
    return b;
  }
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(module, path, MAX_PATH);
  std::string name = std::filesystem::path(path).filename().string();
  char b[64];
  std::snprintf(b, sizeof(b), " + 0x%llx", static_cast<unsigned long long>(address - reinterpret_cast<uint64_t>(module)));
  return name + b;
}

void WriteReport(const char* kind, EXCEPTION_POINTERS* ep, const std::string& reason) {
  if (g_dir.empty() || g_writing.exchange(true)) return;
  std::error_code ec;
  std::filesystem::create_directories(g_dir, ec);
  const std::string stem = std::string(kind) + "-" + Stamp();
  const std::filesystem::path dmp = g_dir / (stem + ".dmp"), txt = g_dir / (stem + ".txt");
  HANDLE file = CreateFileW(dmp.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), ep, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                      ep ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
  }
  if (FILE* f = _wfopen(txt.wstring().c_str(), L"w")) {
    std::fprintf(f, "Battlefield 1943 %s report\nReason: %s\n", kind, reason.c_str());
    if (ep && ep->ExceptionRecord) {
      std::fprintf(f, "Exception: 0x%08lx at %s\nThread: %lu\n", ep->ExceptionRecord->ExceptionCode,
                   Where(reinterpret_cast<uint64_t>(ep->ExceptionRecord->ExceptionAddress)).c_str(),
                   GetCurrentThreadId());
      // The callers, by the x64 unwind data from the exception's context (recompiled code without unwind data ends
      // the walk).
      CONTEXT context = *ep->ContextRecord;
      for (int i = 0; i < 16 && context.Rip; ++i) {
        std::fprintf(f, "  %2d %s\n", i, Where(context.Rip).c_str());
        DWORD64 image = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
        if (!function) break;
        PVOID handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context, &handler_data, &frame, nullptr);
      }
    }
    std::fprintf(f, "Minidump: %s\n", dmp.filename().string().c_str());
    std::fclose(f);
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
  SetUnhandledExceptionFilter(OnUnhandled);
  AddVectoredExceptionHandler(0, OnVectored);
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(2));
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
}

// Each game frame (VdSwap): the hang watchdog's clock.
void NoteGuestFrame() {
  g_last_frame_ms = NowMs();
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
}  // namespace bf1943

#endif
