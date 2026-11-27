/**
 * @file        audio/diagnostics.cpp
 * @brief       Audio diagnostics for players' logs (audio_diagnostics)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/audio/diagnostics.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <string>

#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(audio_diagnostics, false, "Audio",
                    "Diagnostics for players' logs: every 5 s when sound went wrong (output underruns, starved or "
                    "unreadable XMA voices, the game's mixer late) a summary line, and an all-clear line every minute");

namespace rex::audio::diag {
namespace {

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void Max(std::atomic<int64_t>& a, int64_t v) {
  int64_t old = a.load(std::memory_order_relaxed);
  while (v > old && !a.compare_exchange_weak(old, v, std::memory_order_relaxed)) {
  }
}
void Min(std::atomic<int64_t>& a, int64_t v) {
  int64_t old = a.load(std::memory_order_relaxed);
  while (v < old && !a.compare_exchange_weak(old, v, std::memory_order_relaxed)) {
  }
}

// One window's counts (5 s), taken and cleared by the summary.
struct Window {
  std::atomic<int64_t> frames_in{0}, frames_out{0}, silent_chunks{0}, underruns{0}, underrun_us_max{0};
  std::atomic<int64_t> queue_min{INT64_MAX}, queue_max{0}, in_gap_us_max{0};
  std::atomic<int64_t> starved{0}, starved_ms_max{0}, bad_frames{0}, no_bits{0}, input_waits{0};
  std::atomic<int64_t> mixer_waits{0}, mixer_wait_us_max{0}, mixer_late{0};
};
Window g_window;

// The run's totals (RunTotals), bumped beside the window's.
struct RunCounts {
  std::atomic<int64_t> underruns{0}, silent_chunks{0}, starved{0}, bad_frames{0}, mixer_late{0};
};
RunCounts g_run;
std::atomic<const char*> g_scene{nullptr};
std::string SceneTag() {
  const char* s = g_scene.load(std::memory_order_relaxed);
  return s ? std::string(" [") + s + "]" : std::string();
}

// The minute's totals (the summary's thread only).
struct MinuteTotals {
  int64_t frames_in = 0, frames_out = 0, underruns = 0, silent_chunks = 0, starved = 0, bad_frames = 0, no_bits = 0,
          input_waits = 0, mixer_waits = 0, mixer_late = 0;
};
MinuteTotals g_minute;
int64_t g_window_start_us = 0, g_minute_start_us = 0, g_last_in_us = 0;  // NoteFrameIn's thread only

// A run of silent device chunks (the device's callback only): an underrun when a frame ends it.
bool g_started = false, g_in_gap = false;
int64_t g_gap_chunks = 0;

int64_t Take(std::atomic<int64_t>& a, int64_t reset = 0) { return a.exchange(reset, std::memory_order_relaxed); }

void Summarize(int64_t now) {
  const double secs = (now - g_window_start_us) / 1e6;
  g_window_start_us = now;
  Window& w = g_window;
  const int64_t in = Take(w.frames_in), out = Take(w.frames_out), silent = Take(w.silent_chunks),
                runs = Take(w.underruns), run_us = Take(w.underrun_us_max), qmin = Take(w.queue_min, INT64_MAX),
                qmax = Take(w.queue_max), gap_us = Take(w.in_gap_us_max), starved = Take(w.starved),
                starved_ms = Take(w.starved_ms_max), bad = Take(w.bad_frames), no_bits = Take(w.no_bits),
                waits_in = Take(w.input_waits), mixer = Take(w.mixer_waits), mixer_us = Take(w.mixer_wait_us_max),
                late = Take(w.mixer_late);
  MinuteTotals& m = g_minute;
  m.frames_in += in, m.frames_out += out, m.underruns += runs, m.silent_chunks += silent, m.starved += starved;
  m.bad_frames += bad, m.no_bits += no_bits, m.input_waits += waits_in, m.mixer_waits += mixer, m.mixer_late += late;
  // A gap between the game's frames over 50 ms is worth a line even when the device's queue rode it out.
  if (runs || starved || bad || no_bits || late || gap_us > 50000) {
    REXAPU_INFO(
        "audio diagnostics{}: {:.1f} s: {} underrun(s) (longest {:.0f} ms of silence, {} empty device chunks); queue "
        "{}..{} frames; longest gap between the game's frames {:.1f} ms; XMA: {} voice(s) starved of input (longest "
        "{} ms), {} unreadable frame(s), {} frame(s) without bits, {} wait(s) for input; game mixer: {} wait(s) "
        "(longest {:.1f} ms), {} late",
        SceneTag(), secs, runs, run_us / 1000.0, silent, qmin == INT64_MAX ? 0 : qmin, qmax, gap_us / 1000.0, starved,
        starved_ms, bad, no_bits, waits_in, mixer, mixer_us / 1000.0, late);
  }
  if (now - g_minute_start_us >= 60000000) {
    const bool clear = !m.underruns && !m.starved && !m.bad_frames && !m.no_bits && !m.mixer_late;
    REXAPU_INFO(
        "audio diagnostics{}: the last {:.0f} s {}: {} frames in, {} out, {} underrun(s) ({} empty chunks), {} starved "
        "voice(s), {} unreadable frame(s), {} without bits, {} input wait(s), {} mixer wait(s) ({} late)",
        SceneTag(), (now - g_minute_start_us) / 1e6, clear ? "were clean" : "had faults", m.frames_in, m.frames_out, m.underruns,
        m.silent_chunks, m.starved, m.bad_frames, m.no_bits, m.input_waits, m.mixer_waits, m.mixer_late);
    g_minute = {};
    g_minute_start_us = now;
  }
}

}  // namespace

void NoteDevice(const char* name, int channels, int rate, int buffer_frames, uint32_t game_frame_samples) {
  if (!On()) return;
  REXAPU_INFO(
      "audio diagnostics: on; device '{}', {} channel(s) at {} Hz, its buffer {} sample frames ({:.1f} ms); the game's "
      "frames are {} samples ({:.2f} ms)",
      name ? name : "?", channels, rate, buffer_frames, rate ? 1000.0 * buffer_frames / rate : 0.0,
      game_frame_samples, rate ? 1000.0 * game_frame_samples / rate : 0.0);
}

void NoteFrameIn(size_t queued) {
  if (!On()) return;
  const int64_t now = NowUs();
  Window& w = g_window;
  w.frames_in.fetch_add(1, std::memory_order_relaxed);
  Max(w.queue_max, int64_t(queued));
  if (g_last_in_us) Max(w.in_gap_us_max, now - g_last_in_us);
  g_last_in_us = now;
  if (!g_window_start_us) g_window_start_us = g_minute_start_us = now;
  if (now - g_window_start_us >= 5000000) Summarize(now);
}

void NoteFrameOut(size_t queued_after, int64_t chunk_us) {
  if (!On()) return;
  Window& w = g_window;
  g_started = true;
  w.frames_out.fetch_add(1, std::memory_order_relaxed);
  Min(w.queue_min, int64_t(queued_after));
  if (g_in_gap) {
    g_in_gap = false;
    w.underruns.fetch_add(1, std::memory_order_relaxed);
    g_run.underruns.fetch_add(1, std::memory_order_relaxed);
    Max(w.underrun_us_max, g_gap_chunks * chunk_us);
  }
}

void NoteSilentChunk() {
  if (!On() || !g_started) return;  // the silence before the game's first frame is no underrun
  if (!g_in_gap) g_in_gap = true, g_gap_chunks = 0;
  ++g_gap_chunks;
  g_window.silent_chunks.fetch_add(1, std::memory_order_relaxed);
  g_run.silent_chunks.fetch_add(1, std::memory_order_relaxed);
}

void NoteXmaStarved(int64_t ms) {
  if (!On()) return;
  g_window.starved.fetch_add(1, std::memory_order_relaxed);
  g_run.starved.fetch_add(1, std::memory_order_relaxed);
  Max(g_window.starved_ms_max, ms);
}
void NoteXmaBadFrame() {
  if (On()) g_window.bad_frames.fetch_add(1, std::memory_order_relaxed), g_run.bad_frames.fetch_add(1, std::memory_order_relaxed);
}
void NoteXmaNoBits() {
  if (On()) g_window.no_bits.fetch_add(1, std::memory_order_relaxed);
}
void NoteXmaInputWait() {
  if (On()) g_window.input_waits.fetch_add(1, std::memory_order_relaxed);
}

void NoteMixerWait(bool ready, int64_t waited_us) {
  if (!On()) return;
  g_window.mixer_waits.fetch_add(1, std::memory_order_relaxed);
  Max(g_window.mixer_wait_us_max, waited_us);
  if (!ready) g_window.mixer_late.fetch_add(1, std::memory_order_relaxed), g_run.mixer_late.fetch_add(1, std::memory_order_relaxed);
}

void SetScene(const char* scene) { g_scene.store(scene, std::memory_order_relaxed); }

Totals RunTotals() {
  Totals t;
  t.underruns = g_run.underruns.load(std::memory_order_relaxed);
  t.silent_chunks = g_run.silent_chunks.load(std::memory_order_relaxed);
  t.starved = g_run.starved.load(std::memory_order_relaxed);
  t.bad_frames = g_run.bad_frames.load(std::memory_order_relaxed);
  t.mixer_late = g_run.mixer_late.load(std::memory_order_relaxed);
  return t;
}

namespace {
std::atomic<int64_t> g_time_count[int(Timed::kCount)] = {}, g_time_ns[int(Timed::kCount)] = {}, g_time_max[int(Timed::kCount)] = {};
}  // namespace

void NoteTime(Timed what, int64_t ns) {
  const int i = int(what);
  g_time_count[i].fetch_add(1, std::memory_order_relaxed);
  g_time_ns[i].fetch_add(ns, std::memory_order_relaxed);
  int64_t old = g_time_max[i].load(std::memory_order_relaxed);
  while (ns > old && !g_time_max[i].compare_exchange_weak(old, ns, std::memory_order_relaxed)) {
  }
}

void TakeTimes(TimeTotals out[int(Timed::kCount)]) {
  for (int i = 0; i < int(Timed::kCount); ++i) {
    out[i].count = g_time_count[i].exchange(0, std::memory_order_relaxed);
    out[i].total_ns = g_time_ns[i].exchange(0, std::memory_order_relaxed);
    out[i].max_ns = g_time_max[i].exchange(0, std::memory_order_relaxed);
  }
}

}  // namespace rex::audio::diag
