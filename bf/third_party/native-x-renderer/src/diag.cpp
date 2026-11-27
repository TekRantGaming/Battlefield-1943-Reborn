// NativeXRenderer - see diag.h.

#include "diag.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <mutex>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif
#include <unordered_set>

REXCVAR_DEFINE_INT32(native_x_renderer_diagnostics_level, 2, "NativeXRenderer",
                     "How much the diagnostics lines say when native_x_renderer_diagnostics is on: 1 quiet (none of the "
                     "per-event lines), 2 normal, 3 detailed (lower thresholds, 4x the lines), 4 verbose (every event)");
REXCVAR_DEFINE_BOOL(native_x_renderer_diagnostics, false, "NativeXRenderer",
                    "Diagnostics lines in the log: pipelines that took over 50 ms to compile (with their shaders), the "
                    "HD textures remade after a change of the game's pictures (one line each time, with why), swap "
                    "chain resizes");

namespace native_x_renderer {
namespace diag {
namespace {

Totals g_totals;
FrameTextures g_frame_textures;
std::atomic<FrameDiagnosticsFn> g_frame_fn{nullptr};
std::atomic<const char*> g_repatch_reason{nullptr};
std::atomic<uint64_t> g_frame_now{0};
// The renderer's thread's slices since the last frame, and the frame waiting for its frame end (that thread only).
double g_slices[kSliceCount] = {};
FrameDiagnostics g_pending_frame;
bool g_have_pending = false;
std::chrono::steady_clock::time_point g_last_flush{};
std::atomic<uint64_t> g_same_again{0}, g_same_again_us{0};
std::atomic<uint64_t> g_hd_reused{0}, g_hd_reused_us{0};
std::atomic<uint64_t> g_stored[3] = {}, g_stored_bytes{0};
std::atomic<bool> g_storage_failed{false};
// GPU section times by frame (a ring of the last 512 frames).
constexpr int kMaxSections = 32;
constexpr uint64_t kGpuFrames = 512;
struct GpuFrame {
  uint64_t frame = UINT64_MAX;
  float ms[kMaxSections] = {};
};
std::mutex g_gpu_mutex;
GpuFrame g_gpu_frames[kGpuFrames];
const char* g_gpu_names[kMaxSections] = {};

void MaxOf(std::atomic<uint64_t>& a, uint64_t v) {
  uint64_t old = a.load(std::memory_order_relaxed);
  while (v > old && !a.compare_exchange_weak(old, v, std::memory_order_relaxed)) {
  }
}

// At most `per_minute` lines a minute from one place (the renderer's thread and the compile workers both log).
// At Normal `normal` lines a minute, scaled by the level as it is now (diag.h Scaled: it can change in the game).
class LineBudget {
 public:
  explicit LineBudget(int normal) : per_minute_(normal) {}
  bool Take() {
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (now - minute_ >= std::chrono::minutes(1)) minute_ = now, used_ = 0;
    return used_++ < Scaled(per_minute_);
  }

 private:
  std::mutex mutex_;
  std::chrono::steady_clock::time_point minute_ = std::chrono::steady_clock::now();
  int used_ = 0;
  const int per_minute_;
};

}  // namespace

bool LinesOn() { return REXCVAR_GET(native_x_renderer_diagnostics) && Level() >= 2; }
int Level() { return REXCVAR_GET(native_x_renderer_diagnostics) ? REXCVAR_GET(native_x_renderer_diagnostics_level) : 0; }
int Scaled(int normal) {
  const int l = Level();
  return l <= 1 ? 0 : l == 2 ? normal : l == 3 ? normal * 4 : normal * 20;
}
Totals& totals() { return g_totals; }
FrameTextures& frame_textures() { return g_frame_textures; }

void NoteTextureLoad(uint64_t bytes, double ms, uint32_t width, uint32_t height) {
  g_totals.texture_loads.fetch_add(1, std::memory_order_relaxed);
  g_totals.texture_load_bytes.fetch_add(bytes, std::memory_order_relaxed);
  g_totals.texture_load_us.fetch_add(uint64_t(ms * 1000), std::memory_order_relaxed);
  FrameTextures& f = g_frame_textures;
  ++f.loads, f.bytes += bytes, f.ms += ms;
  if (bytes > f.largest) f.largest = bytes, f.largest_width = width, f.largest_height = height;
}

void NoteHdMade(double ms, bool again) {
  (again ? g_totals.hd_remade : g_totals.hd_made).fetch_add(1, std::memory_order_relaxed);
  ++g_frame_textures.hd_made, g_frame_textures.hd_ms += ms;
}

void NotePipelineReady(double ms, bool failed, uint64_t vs, uint64_t ps) {
  g_totals.pipelines.fetch_add(1, std::memory_order_relaxed);
  if (failed) {
    g_totals.pipelines_failed.fetch_add(1, std::memory_order_relaxed);
    return;  // PipelineCache::Ready logs it
  }
  if (ms < kQuickPipelineMs) {
    g_totals.pipelines_quick.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_totals.pipelines_slow.fetch_add(1, std::memory_order_relaxed);
  g_totals.pipelines_slow_us.fetch_add(uint64_t(ms * 1000), std::memory_order_relaxed);
  MaxOf(g_totals.pipeline_slowest_us, uint64_t(ms * 1000));
  static LineBudget budget(30);
  const double over = Level() >= 4 ? kQuickPipelineMs : Level() == 3 ? 25.0 : 50.0;
  if (ms > over && LinesOn() && budget.Take())
    REXLOG_INFO("[native_x_renderer] diagnostics: a pipeline was ready {:.0f} ms after it was asked for (VS {:016X}, "
                "PS {:016X}): compiled here, not from the driver's cache; the time includes its wait behind others being "
                "compiled",
                ms, vs, ps);
}

void NoteShaderFailure() { g_totals.shader_failures.fetch_add(1, std::memory_order_relaxed); }

void NoteSwapChainResize(uint32_t width, uint32_t height) {
  g_totals.swap_chain_resizes.fetch_add(1, std::memory_order_relaxed);
  static LineBudget budget(10);
  if (LinesOn() && budget.Take())
    REXLOG_INFO("[native_x_renderer] diagnostics: the swap chain resized to {}x{}", width, height);
}

void NoteSwapChainRebuild() { g_totals.swap_chain_rebuilds.fetch_add(1, std::memory_order_relaxed); }

void DeliverFrame(FrameDiagnostics& frame) {
  g_totals.frames.fetch_add(1, std::memory_order_relaxed);
  FrameTextures& t = g_frame_textures;
  frame.texture_loads = t.loads;
  frame.texture_load_kb = uint32_t(t.bytes >> 10);
  frame.texture_load_ms = float(t.ms);
  frame.largest_load_kb = uint32_t(t.largest >> 10);
  frame.largest_load_width = uint16_t(std::min<uint32_t>(t.largest_width, 65535));
  frame.largest_load_height = uint16_t(std::min<uint32_t>(t.largest_height, 65535));
  frame.hd_made = t.hd_made;
  frame.hd_ms = float(t.hd_ms);
  t = {};
  g_frame_now.store(frame.frame + 1, std::memory_order_relaxed);
  g_pending_frame = frame, g_have_pending = true;
}

void AddSlice(int slice, double ms) {
  if (slice >= 0 && slice < kSliceCount) g_slices[slice] += ms;
}

void FlushFrame() {
  const auto now = std::chrono::steady_clock::now();
  const double wall = g_last_flush.time_since_epoch().count()
                          ? std::chrono::duration<double, std::milli>(now - g_last_flush).count()
                          : 0.0;
  g_last_flush = now;
  if (!g_have_pending) {
    std::fill(std::begin(g_slices), std::end(g_slices), 0.0);
    return;
  }
  FrameDiagnostics f = g_pending_frame;
  g_have_pending = false;
  double busy = 0;
  for (int i = 0; i < kSliceCount; ++i) {
    f.slice_ms[i] = float(g_slices[i]);
    if (i < kSliceTopCount) busy += g_slices[i];
    g_slices[i] = 0;
  }
  f.wall_ms = float(wall);
  f.idle_ms = float(std::max(0.0, wall - busy));
  if (FrameDiagnosticsFn fn = g_frame_fn.load(std::memory_order_acquire)) fn(f);
}

void NoteHdAgainPicture(uint64_t hash, double ms) {
  static std::mutex mutex;
  static std::unordered_set<uint64_t> seen;
  std::lock_guard lock(mutex);
  if (seen.insert(hash).second) return;
  g_same_again.fetch_add(1, std::memory_order_relaxed);
  g_same_again_us.fetch_add(uint64_t(ms * 1000), std::memory_order_relaxed);
}

void NoteHdReused(double ms) {
  g_hd_reused.fetch_add(1, std::memory_order_relaxed);
  g_hd_reused_us.fetch_add(uint64_t(ms * 1000), std::memory_order_relaxed);
}

void NoteStored(int kind, uint64_t bytes) {
  if (kind >= 0 && kind < 3) g_stored[kind].fetch_add(1, std::memory_order_relaxed);
  g_stored_bytes.fetch_add(bytes, std::memory_order_relaxed);
}
void NoteStorageFailed() { g_storage_failed = true; }

uint64_t FrameNow() { return g_frame_now.load(std::memory_order_relaxed); }

double ThreadCpuMs() {
#if defined(_WIN32)
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return -1;
  auto ms = [](const FILETIME& f) { return double(uint64_t(f.dwHighDateTime) << 32 | f.dwLowDateTime) / 1e4; };
  return ms(kernel) + ms(user);
#else
  timespec ts{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return -1;
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
#endif
}

void NoteGpuSection(uint64_t frame, int id, const char* name, double ms) {
  if (id < 0 || id >= kMaxSections) return;
  std::lock_guard lock(g_gpu_mutex);
  GpuFrame& f = g_gpu_frames[frame % kGpuFrames];
  if (f.frame != frame) f = GpuFrame{}, f.frame = frame;
  f.ms[id] += float(ms);
  g_gpu_names[id] = name;
}

const char* RepatchReason() { return g_repatch_reason.load(std::memory_order_acquire); }

}  // namespace diag

double ThreadCpuMs() { return diag::ThreadCpuMs(); }

const char* FrameSliceName(int slice) {
  static constexpr const char* kNames[kSliceCount] = {
      "draws", "clears", "resolves", "tiling", "occlusion queries", "presents", "frame end", "view begins",
      "view geometry ends (DLAA, the view's effects)", "2D boxes",
      "frame end: the device check", "frame end: the SDK's frame", "frame end: unused targets freed",
      "frame end: occlusion results", "frame end: the frame's counts", "frame end: the 600-frame log lines",
      "frame end: the draw path's frame end"};
  return slice >= 0 && slice < kSliceCount ? kNames[slice] : "?";
}
void SetFrameDiagnostics(FrameDiagnosticsFn fn) { diag::g_frame_fn.store(fn, std::memory_order_release); }

DiagnosticsTotals GetDiagnosticsTotals() {
  const diag::Totals& t = diag::g_totals;
  auto get = [](const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); };
  DiagnosticsTotals out;
  out.frames = get(t.frames);
  out.pipelines = get(t.pipelines);
  out.pipelines_quick = get(t.pipelines_quick);
  out.pipelines_slow = get(t.pipelines_slow);
  out.pipelines_failed = get(t.pipelines_failed);
  out.pipelines_slow_ms = get(t.pipelines_slow_us) / 1000.0;
  out.pipeline_slowest_ms = get(t.pipeline_slowest_us) / 1000.0;
  out.shader_failures = get(t.shader_failures);
  out.texture_loads = get(t.texture_loads);
  out.texture_load_bytes = get(t.texture_load_bytes);
  out.texture_load_ms = get(t.texture_load_us) / 1000.0;
  out.hd_made = get(t.hd_made);
  out.hd_remade = get(t.hd_remade);
  out.swap_chain_resizes = get(t.swap_chain_resizes);
  out.swap_chain_rebuilds = get(t.swap_chain_rebuilds);
  out.hd_same_again = diag::g_same_again.load(std::memory_order_relaxed);
  out.hd_same_again_ms = diag::g_same_again_us.load(std::memory_order_relaxed) / 1000.0;
  out.hd_reused = diag::g_hd_reused.load(std::memory_order_relaxed);
  out.hd_reused_ms = diag::g_hd_reused_us.load(std::memory_order_relaxed) / 1000.0;
  out.stored_shaders = diag::g_stored[0].load(std::memory_order_relaxed);
  out.stored_translations = diag::g_stored[1].load(std::memory_order_relaxed);
  out.stored_pipelines = diag::g_stored[2].load(std::memory_order_relaxed);
  out.stored_bytes = diag::g_stored_bytes.load(std::memory_order_relaxed);
  out.storage_failed = diag::g_storage_failed.load();
  return out;
}

uint32_t GpuSectionsOfFrame(uint64_t frame, GpuSection* out, uint32_t max) {
  std::lock_guard lock(diag::g_gpu_mutex);
  const diag::GpuFrame& f = diag::g_gpu_frames[frame % diag::kGpuFrames];
  if (f.frame != frame) return 0;
  uint32_t n = 0;
  for (int i = 0; i < diag::kMaxSections; ++i)
    if (f.ms[i] > 0 && diag::g_gpu_names[i]) {
      GpuSection s{diag::g_gpu_names[i], f.ms[i]};
      if (n < max) {
        out[n++] = s;
      } else {
        // Keep the largest: replace the smallest held if this one is larger.
        uint32_t small = 0;
        for (uint32_t k = 1; k < n; ++k)
          if (out[k].ms < out[small].ms) small = k;
        if (s.ms > out[small].ms) out[small] = s;
      }
    }
  std::sort(out, out + n, [](const GpuSection& a, const GpuSection& b) { return a.ms > b.ms; });
  return n;
}

void RepatchTextures(const char* why) {
  diag::g_repatch_reason.store(why, std::memory_order_release);
  RepatchTextures();
}

}  // namespace native_x_renderer
