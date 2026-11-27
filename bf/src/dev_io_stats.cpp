// Developer aid (BF_DEV_TOOLS builds): BF_DEV_IOSTATS=1 logs, every 2 s, the game's file reads (calls, bytes, time in
// the call), its waits (time in each kind, and the callers that waited longest) and its sleeps: where a load that
// keeps no thread busy spends its time (runs bootprof1 and loadprof1: the boot and a level load were both idle).
//
// In every build (27 Nov 2026): each NtReadFile also goes to Settings > Diagnostics > System logging's counts (the
// game's file reads a minute, the slowest: diagnostics_health.cpp).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "diagnostics.h"  // NoteFileRead
#if defined(BF_DEV_TOOLS)
#include "dev_guest_memory.h"  // SafeLoad32
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;

PPCFunc* RuntimeExport(const char* name) {
#if defined(_WIN32)
  static HMODULE runtime = GetModuleHandleA("rexruntime.dll");
  return reinterpret_cast<PPCFunc*>(GetProcAddress(runtime, name));
#else
  return reinterpret_cast<PPCFunc*>(dlsym(RTLD_NEXT, name));
#endif
}
}  // namespace

#if defined(BF_DEV_TOOLS)
namespace {
bool On() {
  static const bool on = [] {
    const char* v = std::getenv("BF_DEV_IOSTATS");
    return v && *v == '1';
  }();
  return on;
}

struct Kind {
  uint64_t calls = 0, bytes = 0;
  double ms = 0;
  std::map<uint32_t, double> by_caller;  // the guest LR, ms
};

std::mutex g_mutex;
std::map<std::string, Kind> g_kinds;
Clock::time_point g_window = Clock::now();

void Note(const char* kind, uint32_t lr, double ms, uint64_t bytes) {
  std::lock_guard lock(g_mutex);
  Kind& k = g_kinds[kind];
  ++k.calls, k.bytes += bytes, k.ms += ms;
  k.by_caller[lr] += ms;
  const auto now = Clock::now();
  if (now - g_window < std::chrono::seconds(2)) return;
  std::string line;
  for (auto& [name, v] : g_kinds) {
    std::vector<std::pair<double, uint32_t>> top;
    for (auto& [c, t] : v.by_caller) top.push_back({t, c});
    std::sort(top.rbegin(), top.rend());
    std::string callers;
    for (size_t i = 0; i < top.size() && i < 3; ++i) callers += fmt::format(" {:08X} {:.0f} ms", top[i].second, top[i].first);
    line += fmt::format("; {} {} calls{} {:.0f} ms ({})", name, v.calls,
                        v.bytes ? fmt::format(" {} KB", v.bytes >> 10) : std::string(), v.ms, callers);
  }
  REXLOG_INFO("BF dev io in {:.1f} s{}", std::chrono::duration<double>(now - g_window).count(), line);
  g_kinds.clear();
  g_window = now;
}

template <typename F>
void Timed(const char* kind, PPCContext& ctx, uint8_t* base, PPCFunc* original, uint64_t bytes, F) {
  if (!On()) {
    original(ctx, base);
    return;
  }
  const uint32_t lr = uint32_t(ctx.lr);
  const auto t0 = Clock::now();
  original(ctx, base);
  Note(kind, lr, std::chrono::duration<double, std::milli>(Clock::now() - t0).count(), bytes);
}
}  // namespace

#define BF_TIMED_IMPORT(name, kind, bytes_expr)                              \
  extern "C" REX_FUNC(__imp__##name) {                                       \
    static PPCFunc* original = RuntimeExport("__imp__" #name);               \
    const uint64_t bytes = (bytes_expr);                                     \
    Timed(kind, ctx, base, original, bytes, 0);                              \
  }

namespace {
// The files too: each open logged with its path, and each read counted under its file's name.
std::mutex g_files_mutex;
std::map<uint32_t, std::string> g_files;  // guest handle -> its file's name
uint32_t Load32(const uint8_t* base, uint32_t a) {
  return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
}
// X_OBJECT_ATTRIBUTES {root, name, attributes}; the name an X_ANSI_STRING {u16 length, u16 maximum, u32 buffer}.
std::string ObjectPath(const uint8_t* base, uint32_t attributes) {
  if (!attributes) return {};
  const uint32_t name = Load32(base, attributes + 4);
  if (!name) return {};
  const uint32_t length = uint32_t(base[name]) << 8 | base[name + 1], buffer = Load32(base, name + 4);
  if (!buffer || length > 512) return {};
  return std::string(reinterpret_cast<const char*>(base + buffer), length);
}
const char* ReadKind(uint32_t handle) {
  static std::mutex mutex;
  static std::map<std::string, std::string> kinds;  // kept: Note keeps the kind's text by pointer
  std::string kind = "read";
  {
    std::lock_guard lock(g_files_mutex);
    const auto it = g_files.find(handle);
    if (it != g_files.end()) kind += " " + it->second;
  }
  std::lock_guard lock(mutex);
  return kinds.emplace(kind, kind).first->second.c_str();
}
}  // namespace

namespace bf1943 {
// From shader_prep.cpp's NtCreateFile and NtOpenFile (the game's every open): the handle's file, for the read counts.
void DevNoteOpenedHandle(const uint8_t* base, uint32_t handle_out, uint32_t attributes, uint32_t status, uint32_t lr,
                         uint32_t sp) {
  if (!On() || !handle_out || status != 0) return;
  const std::string path = ObjectPath(base, attributes);
  const uint32_t handle = Load32(base, handle_out);
  const size_t slash = path.find_last_of("\\/");
  {
    std::lock_guard lock(g_files_mutex);
    g_files[handle] = slash == std::string::npos ? path : path.substr(slash + 1);
  }
  // The guest call chain (each frame's back chain at [r1], its saved LR at back - 8), for finding who opens what.
  std::string chain = fmt::format(" {:08X}", lr);
  for (int i = 0; i < 18 && sp; ++i) {
    const uint32_t back = dev::SafeLoad32(base, sp);  // a guest read that never faults (the stack's top ends it)
    if (back <= sp || back - sp > 0x100000) break;
    chain += fmt::format(" {:08X}", dev::SafeLoad32(base, back - 8));
    sp = back;
  }
  REXLOG_INFO("BF dev io open {} -> {:08X} from{}", path, handle, chain);
}
}  // namespace bf1943

BF_TIMED_IMPORT(KeDelayExecutionThread, "sleep", 0)

#endif  // BF_DEV_TOOLS

// Every build (27 Nov 2026): the guest's waits, timed for Settings > Diagnostics (diagnostics.h NoteWait: the game
// thread's waits in the black box's frames and the minute's top waits by caller); dev builds' stats too.
#if defined(BF_DEV_TOOLS)
#define BF_WAIT_CALL(kind) Timed(kind, ctx, base, original, 0, 0)
#else
#define BF_WAIT_CALL(kind) original(ctx, base)
#endif
#define BF_WAIT_IMPORT(name, kind)                                                                           \
  extern "C" REX_FUNC(__imp__##name) {                                                                       \
    static PPCFunc* original = RuntimeExport("__imp__" #name);                                               \
    const uint32_t lr = uint32_t(ctx.lr);                                                                    \
    const auto t0 = Clock::now();                                                                            \
    BF_WAIT_CALL(kind);                                                                                      \
    bf1943::diagnostics::NoteWait(lr, std::chrono::duration<double, std::milli>(Clock::now() - t0).count()); \
  }
BF_WAIT_IMPORT(KeWaitForSingleObject, "wait1")
BF_WAIT_IMPORT(NtWaitForSingleObjectEx, "ntwait1")
BF_WAIT_IMPORT(NtWaitForMultipleObjectsEx, "ntwaitN")
BF_WAIT_IMPORT(KeWaitForMultipleObjects, "waitN")

// Every build: the read's time and length (r9, the length asked for) to the diagnostics' counts; dev builds' stats too.
extern "C" REX_FUNC(__imp__NtReadFile) {
  static PPCFunc* original = RuntimeExport("__imp__NtReadFile");
  const uint64_t bytes = ctx.r9.u32;
  const uint32_t handle = ctx.r3.u32;
  const auto t0 = Clock::now();
#if defined(BF_DEV_TOOLS)
  Timed(On() ? ReadKind(ctx.r3.u32) : "read", ctx, base, original, bytes, 0);
#else
  original(ctx, base);
#endif
  bf1943::diagnostics::NoteFileRead(bytes, std::chrono::duration<double, std::milli>(Clock::now() - t0).count(), handle);
}
