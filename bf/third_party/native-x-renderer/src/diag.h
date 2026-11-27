// NativeXRenderer - the diagnostics' counts (internal; the interface is native_x_renderer.h's Diagnostics section).
//
// Totals bumped where the renderer makes or loads something (relaxed atomics: read now and then by a game's health or
// scene lines on other threads), and the per-frame record handed to the game's FrameDiagnosticsFn after each present.

#pragma once

#include <native_x_renderer/native_x_renderer.h>

#include <atomic>
#include <chrono>
#include <cstdint>

namespace native_x_renderer::diag {

inline double MsSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// native_x_renderer_diagnostics: the diagnostics' own log lines (the counts are kept either way).
bool LinesOn();
// native_x_renderer_diagnostics_level while the lines are on (0 when off): 1 quiet, 2 normal, 3 detailed, 4 verbose; a
// per-minute line limit at that level (0 quiet, 4x detailed, 20x verbose).
int Level();
int Scaled(int normal);

struct Totals {
  std::atomic<uint64_t> frames{0};
  std::atomic<uint64_t> pipelines{0}, pipelines_quick{0}, pipelines_slow{0}, pipelines_failed{0};
  std::atomic<uint64_t> pipelines_slow_us{0}, pipeline_slowest_us{0};
  std::atomic<uint64_t> shader_failures{0};
  std::atomic<uint64_t> texture_loads{0}, texture_load_bytes{0}, texture_load_us{0};
  std::atomic<uint64_t> hd_made{0}, hd_remade{0};
  std::atomic<uint64_t> swap_chain_resizes{0}, swap_chain_rebuilds{0};
};
Totals& totals();

// This frame's texture work (the renderer's thread only; taken with the frame's record).
struct FrameTextures {
  uint32_t loads = 0;
  uint64_t bytes = 0;
  double ms = 0;
  uint64_t largest = 0;
  uint32_t largest_width = 0, largest_height = 0;
  uint32_t hd_made = 0;
  double hd_ms = 0;
};
FrameTextures& frame_textures();

// A texture load's time and size (GuestTextures::LoadFromMemory), an HD copy made (`again`: after RepatchTextures).
void NoteTextureLoad(uint64_t bytes, double ms, uint32_t width, uint32_t height);
void NoteHdMade(double ms, bool again);
// A pipeline found compiled or failed (PipelineCache::Ready): its time since its creation, its shaders' hashes.
void NotePipelineReady(double ms, bool failed, uint64_t vs, uint64_t ps);
void NoteShaderFailure();
void NoteSwapChainResize(uint32_t width, uint32_t height);
void NoteSwapChainRebuild();
// An HD copy made again (after RepatchTextures): its picture's hash, so one the same as before is counted (diag.h).
void NoteHdAgainPicture(uint64_t hash, double ms);
// The pipeline storage's additions (storage.cpp) and a failed write.
void NoteStored(int kind, uint64_t bytes);  // 0 shader, 1 translation, 2 pipeline
void NoteStorageFailed();
// A GPU section's time for a frame (renderer.cpp SectionTimers), and the frame now being recorded.
void NoteGpuSection(uint64_t frame, int id, const char* name, double ms);
uint64_t FrameNow();
// The calling thread's CPU time so far (ms), -1 when not known: a clock read (Linux CLOCK_THREAD_CPUTIME_ID, Windows
// GetThreadTimes, which moves in scheduler ticks).
double ThreadCpuMs();

// The record's delivery (renderer.cpp after each present): kept until the frame end command has run (FlushFrame),
// so the frame end's own time is in it.
void DeliverFrame(FrameDiagnostics& frame);
void FlushFrame();
// A part of the renderer's thread's time (the renderer's thread only).
void AddSlice(int slice, double ms);
class ScopedSlice {
 public:
  explicit ScopedSlice(int slice) : slice_(slice), start_(std::chrono::steady_clock::now()) {}
  ~ScopedSlice() { AddSlice(slice_, MsSince(start_)); }

 private:
  int slice_;
  std::chrono::steady_clock::time_point start_;
};

// RepatchTextures(why): the reason of the last call (a literal), read where the textures are offered again.
const char* RepatchReason();


}  // namespace native_x_renderer::diag
