// Frame statistics: how many frames the game presented (VdSwap, once per frame) and the frame times (average, 99th
// percentile, longest, and how many took over 50 ms), logged every 5 seconds with BF_FRAME_STATS=1 (the frame rate
// and optimisation work) and otherwise every 30 seconds, so a player's log shows when and how the frame rate fell
// (TekRantGaming's 7 Nov 2026 play test: 14 FPS on a map, nothing in the log to say why).
// Every frame's time also goes into a ring for the Performance metrics overlay (perf_overlay.cpp).

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace bf1943 {
uint64_t TakeLimitWaitedUs();  // frame_rate.cpp: our limit's waits since the last call
}

namespace {

PPCFunc* RuntimeExport(const char* name) {
#if defined(_WIN32)
  static HMODULE runtime = GetModuleHandleA("rexruntime.dll");
  return reinterpret_cast<PPCFunc*>(GetProcAddress(runtime, name));
#else
  return reinterpret_cast<PPCFunc*>(dlsym(RTLD_NEXT, name));
#endif
}

bool StatsOn() {
  static const bool on = [] {
    const char* v = std::getenv("BF_FRAME_STATS");
    return v && *v && *v != '0';
  }();
  return on;
}

using Clock = std::chrono::steady_clock;

// BF_HITCH_MS=<ms>: a game frame (VdSwap to VdSwap) longer than that is logged, for play tests (stutter on the
// game's own thread, next to NativeXRenderer's hitch lines from its render thread).
void NoteHitch() {
  static const double threshold = [] {
    const char* v = std::getenv("BF_HITCH_MS");
    return v && *v ? std::atof(v) : 0.0;
  }();
  if (threshold <= 0) return;
  static Clock::time_point last{};
  static uint64_t frame = 0;
  const auto now = Clock::now();
  ++frame;
  if (last.time_since_epoch().count()) {
    const double ms = std::chrono::duration<double, std::milli>(now - last).count();
    if (ms > threshold) REXLOG_INFO("BF hitch: game frame {} took {:.1f} ms", frame, ms);
  }
  last = now;
}

// The last frames' times (ms) for the overlay, and the frame rate over the last second.
struct Recent {
  std::mutex mutex;
  std::array<float, 256> times{};
  size_t count = 0, next = 0;
  Clock::time_point last{}, second{};
  int second_frames = 0;
  double fps = 0, frame_ms = 0;
  bool have = false;
} g_recent;

void NoteRecent() {
  std::lock_guard<std::mutex> lock(g_recent.mutex);
  const auto now = Clock::now();
  if (g_recent.last.time_since_epoch().count()) {
    g_recent.times[g_recent.next] = std::chrono::duration<float, std::milli>(now - g_recent.last).count();
    g_recent.next = (g_recent.next + 1) % g_recent.times.size();
    g_recent.count = std::min(g_recent.count + 1, g_recent.times.size());
  } else {
    g_recent.second = now;
  }
  g_recent.last = now;
  ++g_recent.second_frames;
  const double elapsed = std::chrono::duration<double>(now - g_recent.second).count();
  if (elapsed >= 1.0) {  // FPS and the average frame time over the last second, as the overlay shows them
    g_recent.fps = g_recent.second_frames / elapsed;
    g_recent.frame_ms = elapsed * 1000.0 / g_recent.second_frames;
    g_recent.second_frames = 0;
    g_recent.second = now;
    g_recent.have = true;
  }
}

void NoteFrame() {
  static std::mutex mutex;
  static Clock::time_point last{}, window{};
  static std::vector<float> times;
  std::lock_guard<std::mutex> lock(mutex);
  const auto now = Clock::now();
  if (last.time_since_epoch().count()) times.push_back(std::chrono::duration<float, std::milli>(now - last).count());
  else window = now;
  last = now;
  if (now - window < std::chrono::seconds(StatsOn() ? 5 : 30) || times.empty()) return;
  std::vector<float> sorted = times;
  std::sort(sorted.begin(), sorted.end());
  float sum = 0;
  size_t long_frames = 0;
  for (float t : times) sum += t, long_frames += t > 50.0f;
  const float seconds = std::chrono::duration<float>(now - window).count();
  // Our limit's wait, a frame on average: the average less this is the time the frames took (the room under the cap).
  const double waited = double(bf1943::TakeLimitWaitedUs()) / 1000.0 / double(times.size());
  REXLOG_INFO("BF FPS {:.1f}: {} frames, average {:.1f} ms, 99% {:.1f} ms, longest {:.1f} ms, {} over 50 ms; our limit "
              "waited {:.1f} ms a frame",
              times.size() / seconds, times.size(), sum / times.size(),
              sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)], sorted.back(), long_frames, waited);
  times.clear();
  window = now;
}

}  // namespace

void LimitFrame();  // frame_rate.cpp: our frame limit above 30
namespace bf1943 {
void LookFrame();       // look.cpp: this frame's turn
void TickBlendFrame();  // tick_blend.cpp: the time since the game's last tick
void NoteGuestFrame();  // crash_report.cpp: the hang watchdog's clock
void GameOptionsFrame();  // game_options.cpp: live sound volumes
#if defined(BF_DEV_TOOLS)
void LookDevLog(const uint8_t* base);  // look.cpp: BF_DEV_LOOK
#if defined(_WIN32)
void RenderDocFrame();  // dev_renderdoc.cpp: BF_RENDERDOC_AT
#endif
void DevMemDumpFrame();  // dev_mem_dump.cpp: BF_DUMP_PHYS
void DevMatScanFrame();  // dev_matscan.cpp: BF_DEV_MATSCAN
#if defined(_WIN32)
void DevProfileFrame();  // dev_profiler.cpp: BF_DEV_PROFILE's long frames
#endif
#endif
}

extern "C" REX_FUNC(__imp__VdSwap) {
  static PPCFunc* original = RuntimeExport("__imp__VdSwap");
  LimitFrame();
  if (original) original(ctx, base);
  NoteRecent();
  NoteHitch();
  bf1943::LookFrame();  // look.cpp
  bf1943::TickBlendFrame();
  bf1943::NoteGuestFrame();
  bf1943::GameOptionsFrame();
#if defined(BF_DEV_TOOLS)
  bf1943::LookDevLog(base);
#if defined(_WIN32)
  bf1943::RenderDocFrame();
#endif
  bf1943::DevMemDumpFrame();
  bf1943::DevMatScanFrame();
#if defined(_WIN32)
  bf1943::DevProfileFrame();
#endif
#endif
  NoteFrame();
}

namespace bf1943 {
// For perf_overlay.cpp: the last frames' times, oldest first.
size_t RecentFrameTimes(float* out, size_t max) {
  std::lock_guard<std::mutex> lock(g_recent.mutex);
  const size_t n = std::min(max, g_recent.count);
  for (size_t i = 0; i < n; ++i)
    out[i] = g_recent.times[(g_recent.next + g_recent.times.size() - n + i) % g_recent.times.size()];
  return n;
}
bool FrameRateNow(double* fps, double* frame_time_ms) {
  std::lock_guard<std::mutex> lock(g_recent.mutex);
  *fps = g_recent.fps, *frame_time_ms = g_recent.frame_ms;
  return g_recent.have;
}
}  // namespace bf1943
