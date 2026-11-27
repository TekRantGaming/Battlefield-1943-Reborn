// The black box (diagnostics.h): the last 30 seconds of every frame, kept in memory and written to the log only when
// something went wrong or the player says so (the engine designer's wish list of 27 Nov 2026, item 1: "the last ~30 s
// of per-frame detail ... written out only on a trigger").
//
//   kept         each game frame (VdSwap to VdSwap): its time, the game's main thread's waits in it, the game's file reads,
//                packets in and out and the longest gap between a server's packets, audio underruns, sounds EA's mixer
//                stopped, the scene; each renderer frame (native_x_renderer.h FrameDiagnostics): the time between
//                presents and where it went (GPU wait, the renderer's own work), pipelines made, draws skipped while
//                compiling, texture loads (their size, time, the largest), HD copies made, uploads
//   written on   a game frame over 100 ms in play (at most one dump every 2 minutes, 10 a run), the player's mark key
//                (keybind_mark_problem, F10 by default; 30 a run), the hang watchdog, a crash
//   as           one header line, a line a second (the worst of each second), the slow frames one by one (the worst
//                40), the last 5 seconds' frame times; about 80 lines a dump
//
// Each frame costs a short uncontended lock and a copy of a few dozen bytes; nothing is formatted or written on the
// game's or the renderer's threads (a thread of its own writes the dumps, except a hang's and a crash's, which are
// written at once by the thread that saw them).
#include "diagnostics.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <native_x_renderer/native_x_renderer.h>
#include <rex/audio/diagnostics.h>
#include <rex/logging.h>

namespace bf1943::diagnostics {
namespace {

using Clock = std::chrono::steady_clock;
int64_t NowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); }

constexpr int64_t kKeepUs = 30'000'000;  // 30 s
constexpr double kSlowFrameMs = 33.4;    // one-by-one lines and the per-second worst

struct GameRec {
  int64_t t_us = 0;  // the frame's end
  float ms = 0;
  float waited_ms = 0;  // the game thread's waits (NoteWait)
  uint32_t read_kb = 0;
  uint16_t reads = 0, read_slowest_ms = 0;
  uint16_t packets_in = 0, packets_out = 0;
  uint16_t gap_ms = 0;  // the longest gap between a server's packets that ended in the frame
  uint16_t underruns = 0, culled = 0;
  int8_t scene = -1;
  float cpu_ms = -1;  // the game frame thread's CPU time in the frame (less: it waited or had no core)
  float ours[int(Ours::kCount)] = {};  // our own work in the frame, by name (OursTimer)
  // The audio threads in the frame (rex/audio/diagnostics.h Timed, EA's control step): time in all and the longest.
  float callback_ms = 0, callback_max = 0, xma_ms = 0, ea_ms = 0, ea_max = 0;
  uint16_t callbacks = 0, ea_steps = 0;
};
struct RenderRec {
  int64_t t_us = 0;  // the present
  native_x_renderer::FrameDiagnostics f;
};

template <typename T, size_t N>
class Ring {
 public:
  void Push(const T& v) {
    std::lock_guard lock(mutex_);
    items_[next_] = v;
    next_ = (next_ + 1) % N;
    count_ = std::min(count_ + 1, N);
  }
  // The records from `from_us` on, oldest first. `wait`: false gives up after a few ms (a crash's thread may have
  // stopped while holding the lock).
  std::vector<T> Since(int64_t from_us, bool wait = true) {
    std::unique_lock lock(mutex_, std::defer_lock);
    if (wait) {
      lock.lock();
    } else {
      for (int i = 0; i < 50 && !lock.try_lock(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      if (!lock.owns_lock()) return {};
    }
    std::vector<T> out;
    out.reserve(count_);
    for (size_t i = 0; i < count_; ++i) {
      const T& v = items_[(next_ + N - count_ + i) % N];
      if (v.t_us >= from_us) out.push_back(v);
    }
    return out;
  }

 private:
  std::mutex mutex_;
  std::array<T, N> items_{};
  size_t next_ = 0, count_ = 0;
};

// 8192 frames: 30 s at 270 frames a second.
Ring<GameRec, 8192> g_game;
Ring<RenderRec, 8192> g_render;

// Counts since the last game frame (bumped on any thread; the game frame takes them).
std::atomic<int64_t> g_reads{0}, g_read_bytes{0}, g_packets_in{0}, g_packets_out{0}, g_wait_us{0}, g_culled{0};
std::atomic<int32_t> g_read_slowest_ms{0}, g_gap_ms{0};
std::atomic<int64_t> g_culled_total{0};
std::array<std::atomic<int64_t>, size_t(Ours::kCount)> g_ours_ns{};  // since the last game frame
std::atomic<int64_t> g_ea_steps{0}, g_ea_ns{0}, g_ea_max_ns{0};    // since the last game frame

std::atomic<bool> g_started{false};  // its thread and callback set up (StartBlackBox); on while the area is
std::atomic<int> g_dumps{0}, g_auto_dumps{0}, g_marks{0};
std::atomic<int64_t> g_last_auto_us{0}, g_last_mark_us{0}, g_last_mark_wall{0};
// The game's main thread (the SDK names it "Main XThread ..."): its waits are the ones worth a frame's line (the
// thread calling VdSwap is another, and does none of the kernel's waits). Found by name once per thread.
thread_local int t_main_thread = -1;

void MaxOf(std::atomic<int32_t>& a, int32_t v) {
  int32_t old = a.load(std::memory_order_relaxed);
  while (v > old && !a.compare_exchange_weak(old, v, std::memory_order_relaxed)) {
  }
}

const char* WhyText(Trigger why) {
  switch (why) {
    case Trigger::kSlowFrame: return "a slow game frame";
    case Trigger::kMark: return "the player marked a problem";
    case Trigger::kHang: return "the game stopped making frames (hang)";
    case Trigger::kCrash: return "a crash";
  }
  return "?";
}

// ---- the dump

struct Second {
  int frames = 0, render_frames = 0;
  float worst = 0, render_worst = 0, gpu_wait = 0, waited = 0, game_waited = 0;
  uint32_t pipelines = 0, compiling = 0, loads = 0, load_kb = 0, hd = 0, reads = 0, read_kb = 0, read_slowest = 0;
  float pipelines_ms = 0, load_ms = 0, hd_ms = 0;
  uint32_t in = 0, out = 0, gap = 0, underruns = 0, culled = 0;
  float ea_max = 0, callback_max = 0;
  bool any() const { return frames || render_frames; }
};

// ---- a frame's parts, every one named (27 Nov 2026: "have the logs name everything directly"): the renderer's thread
// by slice (native_x_renderer.h FrameSlice: its commands by kind, the frame end's steps), the draws' known parts, the
// GPU; the game's frame thread by our hooks (Ours), its waits, and the game's own code as the rest. The cause is the
// largest part; Detailed and Verbose print the whole account.

struct Part {
  float ms;
  std::string what;
};

// The renderer records of a window, summed.
struct RenderSum {
  uint32_t n = 0;
  float interval = 0, wait = 0, own = 0, pipe_ms = 0, load_ms = 0, hd_ms = 0, game_waited = 0, shader_ms = 0;
  float swap_ms = 0, cpu = 0, idle = 0, wall = 0;
  bool cpu_known = false;
  float slices[native_x_renderer::kSliceCount] = {};
  uint32_t pipes = 0, compiling = 0, translating = 0, loads = 0, load_kb = 0, hd = 0, largest = 0, lw = 0, lh = 0;
  uint32_t shaders = 0;
  std::map<std::string, float> gpu;
};

RenderSum SumRender(const std::vector<RenderRec>& render, int64_t from_us, int64_t to_us) {
  RenderSum s;
  for (const RenderRec& r : render) {
    if (r.t_us <= from_us || r.t_us > to_us) continue;
    const auto& f = r.f;
    ++s.n;
    s.interval = std::max(s.interval, f.interval_ms), s.wait += f.gpu_wait_ms, s.own += f.renderer_ms;
    s.game_waited += f.game_waited_ms, s.swap_ms += f.swap_ms;
    if (f.thread_cpu_ms >= 0) s.cpu += f.thread_cpu_ms, s.cpu_known = true;
    // On the renderer's thread: shader analyses, and draws that waited for their shaders or pipeline (translations
    // run on workers: only their waits cost the frame).
    s.shader_ms += f.analyses_ms + f.waited_ms, s.shaders += f.analyses + f.waited_draws;
    s.pipes += f.pipelines, s.pipe_ms += f.pipelines_ms, s.compiling += f.skipped_compiling;
    s.translating += f.skipped_translating;
    s.loads += f.texture_loads, s.load_kb += f.texture_load_kb, s.load_ms += f.texture_load_ms;
    s.hd += f.hd_made, s.hd_ms += f.hd_ms;
    if (f.largest_load_kb > s.largest) s.largest = f.largest_load_kb, s.lw = f.largest_load_width, s.lh = f.largest_load_height;
    for (int i = 0; i < native_x_renderer::kSliceCount; ++i) s.slices[i] += f.slice_ms[i];
    s.idle += f.idle_ms, s.wall += f.wall_ms;
    native_x_renderer::GpuSection sections[8];
    const uint32_t ns = native_x_renderer::GpuSectionsOfFrame(f.frame, sections, 8);
    for (uint32_t i = 0; i < ns; ++i) s.gpu[sections[i].name] += sections[i].ms;
  }
  return s;
}

std::string TopGpu(const RenderSum& r, size_t n, float* top_ms = nullptr) {
  std::vector<std::pair<float, std::string>> top;
  for (const auto& [name, ms] : r.gpu) top.push_back({ms, name});
  std::sort(top.rbegin(), top.rend());
  if (top_ms) *top_ms = top.empty() ? 0 : top[0].first;
  std::string out;
  for (size_t i = 0; i < top.size() && i < n; ++i) out += fmt::format("{}{} {:.1f} ms", i ? ", " : "", top[i].second, top[i].first);
  return out;
}

float OursSum(const GameRec& g) {
  float sum = 0;
  for (float v : g.ours) sum += v;
  return sum;
}

// Every named part of a frame (both threads: they run side by side, so parts overlap the frame's time, not add to it).
std::vector<Part> PartsOf(const GameRec& g, const RenderSum& r) {
  using native_x_renderer::FrameSliceName;
  std::vector<Part> p;
  if (r.n) {
    // The draws' known parts, then the rest of the draws.
    const float draws = r.slices[native_x_renderer::kSliceDraws];
    p.push_back({r.hd_ms, "HD copies made on the renderer's thread"});
    p.push_back({r.load_ms, "texture loads"});
    p.push_back({r.pipe_ms, "pipelines created"});
    p.push_back({r.shader_ms, "new shaders (analysed, or draws waiting for them: their first draws)"});
    p.push_back({draws - r.hd_ms - r.load_ms - r.pipe_ms - r.shader_ms, "the renderer's draws (the rest of them)"});
    for (int i = native_x_renderer::kSliceClears; i < native_x_renderer::kSliceTopCount; ++i) {
      if (i == native_x_renderer::kSliceFrameEnd || i == native_x_renderer::kSlicePresents) continue;
      p.push_back({r.slices[i], std::string("the renderer's ") + FrameSliceName(i)});
    }
    p.push_back({r.swap_ms, "the swap chain (the compositor or the driver held the present)"});
    p.push_back({r.slices[native_x_renderer::kSlicePresents] - r.swap_ms,
                 "presenting the frame (composing, scaling, the copies to the screen)"});
    float steps = 0;
    for (int i = native_x_renderer::kSliceTopCount; i < native_x_renderer::kSliceCount; ++i) {
      p.push_back({r.slices[i], std::string("the renderer's ") + FrameSliceName(i)});
      steps += r.slices[i];
    }
    p.push_back({r.slices[native_x_renderer::kSliceFrameEnd] - steps, "the renderer's frame end (the rest of it)"});
    std::string gpu_top = TopGpu(r, 1);
    p.push_back({r.wait, gpu_top.empty() ? std::string("the GPU") : "the GPU (most: " + gpu_top + ")"});
    p.push_back({r.game_waited, "the game waiting for room in the renderer's queue"});
  }
  for (int i = 0; i < int(Ours::kCount); ++i) p.push_back({g.ours[i], OursName(Ours(i))});
  p.push_back({g.waited_ms, "the game's main thread waiting (kernel waits)"});
  p.push_back({float(g.read_slowest_ms), "a slow file read"});
  p.push_back({std::max(0.0f, g.ms - OursSum(g) - g.waited_ms), "the game's own code"});
  return p;
}

std::string CauseOf(const GameRec& g, const RenderSum& r) {
  const std::vector<Part> parts = PartsOf(g, r);
  const Part* best = nullptr;
  for (const Part& p : parts)
    if (!best || p.ms > best->ms) best = &p;
  std::string what = best && best->ms >= 0.3f * g.ms ? best->what : "spread over many parts (no part over 30%)";
  if (best && best->what == "the game's own code")
    if (const std::string top = TopFunctions(g.t_us - int64_t(g.ms * 1000), g.t_us, 2); !top.empty())
      what += " (" + top + ")";
  // A thread that ran far less than the frame took was not working: it waited, or the OS gave its core away.
  const bool renderer_side = best && best->what.rfind("the renderer", 0) == 0;
  const float cpu = renderer_side ? (r.cpu_known ? r.cpu : -1.0f) : g.cpu_ms;
  if (cpu >= 0 && cpu < 0.4f * g.ms && g.ms > 40)
    what += fmt::format(" (its thread ran only {:.0f} ms of the {:.0f}: it waited, or had no CPU)", cpu, g.ms);
  return what;
}

// The whole account of a frame, both threads (Detailed and Verbose).
std::string Account(const GameRec& g, const RenderSum& r) {
  using native_x_renderer::FrameSliceName;
  std::string out;
  if (r.n) {
    out = fmt::format("the renderer's thread over {:.1f} ms:", r.wall);
    for (int i = 0; i < native_x_renderer::kSliceTopCount; ++i) {
      const float ms = r.slices[i];
      if (ms < 0.05f) continue;
      out += fmt::format(" {} {:.1f}", FrameSliceName(i), ms);
      if (i == native_x_renderer::kSliceDraws && ms >= 0.5f)
        out += fmt::format(" (HD {:.1f}, textures {:.1f}, pipelines {:.1f}, new shaders {:.1f})", r.hd_ms, r.load_ms,
                           r.pipe_ms, r.shader_ms);
      if (i == native_x_renderer::kSlicePresents) out += fmt::format(" (swap chain {:.1f})", r.swap_ms);
      if (i == native_x_renderer::kSliceFrameEnd) {
        std::string steps;
        for (int k = native_x_renderer::kSliceTopCount; k < native_x_renderer::kSliceCount; ++k)
          if (r.slices[k] >= 0.05f)
            steps += fmt::format("{}{} {:.1f}", steps.empty() ? "" : ", ",
                                 std::string(FrameSliceName(k)).substr(sizeof("frame end: ") - 1), r.slices[k]);
        if (!steps.empty()) out += " (" + steps + ")";
      }
      out += ",";
    }
    out += fmt::format(" waiting for the game's commands {:.1f}", r.idle);
    if (r.cpu_known) out += fmt::format("; its CPU {:.1f}", r.cpu);
    if (const std::string gpu = TopGpu(r, 6); !gpu.empty()) out += "; GPU passes: " + gpu;
    out += " | ";
  }
  out += fmt::format("the game's frame thread over {:.1f} ms:", g.ms);
  for (int i = 0; i < int(Ours::kCount); ++i)
    if (g.ours[i] >= 0.05f) out += fmt::format(" {} {:.1f},", OursName(Ours(i)), g.ours[i]);
  out += fmt::format(" kernel waits {:.1f}, the game's own code {:.1f}", g.waited_ms,
                     std::max(0.0f, g.ms - OursSum(g) - g.waited_ms));
  if (const std::string top = TopFunctions(g.t_us - int64_t(g.ms * 1000), g.t_us, 6); !top.empty())
    out += " (the thread's code by function: " + top + ")";
  if (g.cpu_ms >= 0) out += fmt::format("; its CPU {:.1f}", g.cpu_ms);
  if (g.ea_steps || g.callbacks)
    out += fmt::format(" | the audio threads: EA's control step {}x {:.1f} ms (the longest {:.1f}), the render callback {}x "
                       "{:.1f} ms (the longest {:.1f}), the XMA decoder {:.1f} ms",
                       g.ea_steps, g.ea_ms, g.ea_max, g.callbacks, g.callback_ms, g.callback_max, g.xma_ms);
  return out;
}

// The renderer's side of a slow frame's line (Normal and up).
std::string RenderLine(const RenderSum& r) {
  if (!r.n) return "renderer: no present";
  std::string s = fmt::format("renderer: {} present(s), the longest {:.1f} ms (GPU wait {:.1f}, its own work {:.1f})", r.n,
                              r.interval, r.wait, r.own);
  if (r.pipes || r.compiling || r.translating)
    s += fmt::format("; pipelines {} ({:.1f} ms), draws skipped {} compiling {} translating", r.pipes, r.pipe_ms,
                     r.compiling, r.translating);
  if (r.loads)
    s += fmt::format("; texture loads {} ({} KB, {:.1f} ms; the largest {} KB {}x{})", r.loads, r.load_kb, r.load_ms,
                     r.largest, r.lw, r.lh);
  if (r.hd) s += fmt::format("; HD copies {} ({:.1f} ms)", r.hd, r.hd_ms);
  if (r.shaders) s += fmt::format("; shader analyses and draws waiting for their shaders {} ({:.1f} ms)", r.shaders, r.shader_ms);
  if (r.game_waited >= 1) s += fmt::format("; the game waited {:.1f} ms for the renderer", r.game_waited);
  if (const std::string gpu = TopGpu(r, 4); !gpu.empty()) s += "; GPU passes: " + gpu;
  return s;
}

std::string GameLine(const GameRec& g) {
  std::string s = fmt::format("game {:.1f} ms", g.ms);
  if (g.waited_ms >= 1) s += fmt::format(", the main thread waited {:.1f} ms", g.waited_ms);
  if (g.reads) s += fmt::format(", file reads {} ({} KB, slowest {} ms)", g.reads, g.read_kb, g.read_slowest_ms);
  if (g.gap_ms) s += fmt::format(", a server's packets {} ms apart", g.gap_ms);
  if (g.cpu_ms >= 0) s += fmt::format(", its thread ran {:.1f} ms", g.cpu_ms);
  if (g.underruns) s += fmt::format(", audio underruns {}", g.underruns);
  return s;
}

RenderSum RenderOf(const std::vector<RenderRec>& render, const GameRec& g) {
  return SumRender(render, g.t_us - int64_t(g.ms * 1000), g.t_us + 2000);
}

void WriteDump(Trigger why, const std::string& detail, int64_t at_us, bool wait_for_locks) {
  const Level level = LevelOf(Area::kBlackBox);
  const int64_t now = NowUs();
  const std::vector<GameRec> game = g_game.Since(now - kKeepUs, wait_for_locks);
  const std::vector<RenderRec> render = g_render.Since(now - kKeepUs, wait_for_locks);
  const int n = ++g_dumps;
  const double span = game.empty() ? 0 : (now - game.front().t_us) / 1e6;
  REXLOG_INFO("BF black box{}: {}{}; the last {:.1f} s ({} game frames, {} presents); dump {} of this run ({}); times "
              "are seconds before the trigger (negative) or after it",
              SceneTag(), WhyText(why), detail.empty() ? "" : " (" + detail + ")", span, game.size(), render.size(), n,
              LevelName(level));
  if (game.empty() && render.empty()) {
    REXLOG_INFO("BF black box: nothing kept (no frames yet, or the frames' lock was held by the thread that stopped)");
    return;
  }
  // A line a second (Normal and up).
  if (level >= Level::kNormal) {
    const int64_t first = std::min(game.empty() ? now : game.front().t_us, render.empty() ? now : render.front().t_us);
    const int base_s = int((first - at_us) / 1'000'000) - 1;
    const int seconds = int((now - at_us) / 1'000'000) - base_s + 1;
    std::vector<Second> per(size_t(std::max(seconds, 1)));
    auto slot = [&](int64_t t) -> Second& {
      int64_t d = t - at_us;
      int s = int(d >= 0 ? d / 1'000'000 : -((-d + 999'999) / 1'000'000));
      return per[size_t(std::clamp(s - base_s, 0, int(per.size()) - 1))];
    };
    for (const GameRec& g : game) {
      Second& s = slot(g.t_us);
      ++s.frames, s.worst = std::max(s.worst, g.ms), s.waited += g.waited_ms;
      s.reads += g.reads, s.read_kb += g.read_kb, s.read_slowest = std::max<uint32_t>(s.read_slowest, g.read_slowest_ms);
      s.in += g.packets_in, s.out += g.packets_out, s.gap = std::max<uint32_t>(s.gap, g.gap_ms);
      s.underruns += g.underruns, s.culled += g.culled;
      s.ea_max = std::max(s.ea_max, g.ea_max), s.callback_max = std::max(s.callback_max, g.callback_max);
    }
    for (const RenderRec& r : render) {
      Second& s = slot(r.t_us);
      ++s.render_frames, s.render_worst = std::max(s.render_worst, r.f.interval_ms);
      s.gpu_wait = std::max(s.gpu_wait, r.f.gpu_wait_ms);
      s.game_waited += r.f.game_waited_ms;
      s.pipelines += r.f.pipelines, s.pipelines_ms += r.f.pipelines_ms, s.compiling += r.f.skipped_compiling;
      s.loads += r.f.texture_loads, s.load_kb += r.f.texture_load_kb, s.load_ms += r.f.texture_load_ms;
      s.hd += r.f.hd_made, s.hd_ms += r.f.hd_ms;
    }
    REXLOG_INFO("BF black box: a line a second: s | game frames, the longest ms, the main thread's waits ms | presents, "
                "the longest ms, GPU wait max ms, the game's wait for the renderer ms | pipelines (ms), draws skipped "
                "compiling | texture loads (KB, ms), HD copies (ms) | file reads (KB, slowest ms) | packets in/out, "
                "longest gap ms | audio underruns, sounds stopped, EA's control step and the render callback the longest ms");
    for (size_t i = 0; i < per.size(); ++i) {
      const Second& s = per[i];
      if (!s.any()) continue;
      REXLOG_INFO("BF black box: {:+4d} | {} {:.1f} {:.0f} | {} {:.1f} {:.1f} {:.0f} | {} ({:.0f}) {} | {} ({} {:.0f}) {} "
                  "({:.0f}) | {} ({} {}) | {}/{} {} | {} {} {:.1f} {:.1f}",
                  int(i) + base_s, s.frames, s.worst, s.waited, s.render_frames, s.render_worst, s.gpu_wait,
                  s.game_waited, s.pipelines, s.pipelines_ms, s.compiling, s.loads, s.load_kb, s.load_ms, s.hd, s.hd_ms,
                  s.reads, s.read_kb, s.read_slowest, s.in, s.out, s.gap, s.underruns, s.culled, s.ea_max, s.callback_max);
    }
  }
  // The slow frames one by one: the worst 10 (Quiet) or 40, in time order; Detailed and up with their whole account.
  const size_t keep = level == Level::kQuiet ? 10 : 40;
  std::vector<const GameRec*> slow;
  for (const GameRec& g : game)
    if (g.ms > kSlowFrameMs) slow.push_back(&g);
  if (slow.size() > keep) {
    std::nth_element(slow.begin(), slow.begin() + long(keep), slow.end(), [](auto* a, auto* b) { return a->ms > b->ms; });
    slow.resize(keep);
    std::sort(slow.begin(), slow.end(), [](auto* a, auto* b) { return a->t_us < b->t_us; });
  }
  if (!slow.empty())
    REXLOG_INFO("BF black box: {} game frame(s) over {:.0f} ms{}", slow.size(), kSlowFrameMs,
                slow.size() == keep ? fmt::format(" (the worst {})", keep) : std::string());
  for (const GameRec* g : slow) {
    const RenderSum r = RenderOf(render, *g);
    const std::string cause = CauseOf(*g, r);
    if (level == Level::kQuiet) {
      REXLOG_INFO("BF black box: {:+.2f} s: game {:.1f} ms; most likely: {}", (g->t_us - at_us) / 1e6, g->ms, cause);
      continue;
    }
    REXLOG_INFO("BF black box: {:+.2f} s: {}; {}; most likely: {}", (g->t_us - at_us) / 1e6, GameLine(*g), RenderLine(r),
                cause);
    if (level >= Level::kDetailed) REXLOG_INFO("BF black box:   account: {}", Account(*g, r));
  }
  if (level >= Level::kNormal)
    if (const std::string busy = BusyThreadsAround(at_us, 1'000'000); !busy.empty())
      REXLOG_INFO("BF black box: the busiest threads around the trigger (100% = one core) {}", busy);
  // The trigger's own cause for the run's summary: the slowest frame near the trigger (a mark's problem is the worst
  // moment just before it).
  {
    const GameRec* worst = nullptr;
    for (const GameRec& g : game)
      if (std::llabs(g.t_us - at_us) < (why == Trigger::kMark ? 3'000'000 : 500'000) && (!worst || g.ms > worst->ms)) worst = &g;
    std::string cause = "no slow frame near it";
    if (worst && worst->ms > kSlowFrameMs) cause = CauseOf(*worst, RenderOf(render, *worst));
    NoteTriggerCause(why == Trigger::kMark ? "a mark" : why == Trigger::kSlowFrame ? "a slow frame" : why == Trigger::kHang ? "a hang" : "a crash",
                     worst ? worst->ms : 0, cause);
  }
  // The last 5 s of frame times, 60 a line (Normal and up).
  if (level >= Level::kNormal) {
    const int64_t last_from = now - 5'000'000;
    for (int pass = 0; pass < 2; ++pass) {
      std::vector<float> times;
      if (pass == 0) {
        for (const GameRec& g : game)
          if (g.t_us >= last_from) times.push_back(g.ms);
      } else {
        for (const RenderRec& r : render)
          if (r.t_us >= last_from) times.push_back(r.f.interval_ms);
      }
      for (size_t at = 0; at < times.size(); at += 60) {
        std::string line;
        for (size_t i = at; i < std::min(times.size(), at + 60); ++i) line += fmt::format(" {:.1f}", times[i]);
        REXLOG_INFO("BF black box: the last 5 s, {} ms ({}-{} of {}):{}", pass ? "presents' intervals" : "game frames",
                    at + 1, std::min(times.size(), at + 60), times.size(), line);
      }
    }
  }
  // Verbose: the 2 s around the trigger frame by frame, each with its whole account.
  if (level >= Level::kVerbose) {
    for (const GameRec& g : game) {
      if (g.t_us < at_us - 1'500'000 || g.t_us > at_us + 500'000) continue;
      REXLOG_INFO("BF black box: frame {:+.3f} s: {}", (g.t_us - at_us) / 1e6, Account(g, RenderOf(render, g)));
    }
  }
  REXLOG_INFO("BF black box: end of dump {}", n);
}

// ---- the writer thread (dumps that can wait a moment, to include what followed the trigger)

struct Job {
  Trigger why;
  std::string detail;
  int64_t at_us, write_at_us;
};
std::mutex g_jobs_mutex;
std::condition_variable g_jobs_cv;
std::deque<Job> g_jobs;

void WriterThread() {
  for (;;) {
    Job job;
    {
      std::unique_lock lock(g_jobs_mutex);
      g_jobs_cv.wait(lock, [] { return !g_jobs.empty(); });
      job = g_jobs.front();
      g_jobs.pop_front();
    }
    const int64_t wait = job.write_at_us - NowUs();
    if (wait > 0) std::this_thread::sleep_for(std::chrono::microseconds(wait));
    WriteDump(job.why, job.detail, job.at_us, true);
  }
}

}  // namespace

void StartBlackBox() {
  static std::atomic<bool> started{false};
  if (!On(Area::kBlackBox) || started.exchange(true)) return;
  g_started = true;
  StartProfiler();  // Detailed and up: the game frame thread's code by function
  native_x_renderer::SetFrameDiagnostics([](const native_x_renderer::FrameDiagnostics& f) {
    g_render.Push(RenderRec{NowUs(), f});
    NoteRenderFrame(f.interval_ms, f.game_waited_ms, f.frames_behind);
  });
  std::thread(WriterThread).detach();
#if defined(BF_DEV_TOOLS)
  // BF_DEV_MARK_AT=<s>[,<s>...] (tests): the mark key pressed that long after the log opened.
  if (const char* v = std::getenv("BF_DEV_MARK_AT"); v && *v) {
    std::vector<double> at;
    for (const char* p = v; *p;) {
      at.push_back(std::atof(p));
      while (*p && *p != ',') ++p;
      if (*p == ',') ++p;
    }
    std::thread([at] {
      const auto start = Clock::now();
      for (double t : at) {
        std::this_thread::sleep_until(start + std::chrono::milliseconds(int64_t(t * 1000)));
        MarkProblem();
      }
    }).detach();
  }
#endif
  REXLOG_INFO("BF black box: on (the last 30 s of frames kept; written on a slow game frame in play, a mark (the "
              "{} key), a hang or a crash)",
              MarkKeyName());
}

bool BlackBoxOn() { return (g_started.load(std::memory_order_relaxed) && On(Area::kBlackBox)); }

void BlackBoxGameFrame(float ms) {
  NoteFrameThread();  // the profiler's thread to sample (once)
  if (!(g_started.load(std::memory_order_relaxed) && On(Area::kBlackBox))) return;
  static int64_t last_underruns = -1;
  const int64_t underruns = rex::audio::diag::On() ? rex::audio::diag::RunTotals().underruns : 0;
  GameRec g;
  g.t_us = NowUs();
  g.ms = ms;
  g.waited_ms = float(g_wait_us.exchange(0, std::memory_order_relaxed) / 1000.0);
  g.reads = uint16_t(std::min<int64_t>(g_reads.exchange(0, std::memory_order_relaxed), 65535));
  g.read_kb = uint32_t(g_read_bytes.exchange(0, std::memory_order_relaxed) >> 10);
  g.read_slowest_ms = uint16_t(std::min(g_read_slowest_ms.exchange(0, std::memory_order_relaxed), 65535));
  g.packets_in = uint16_t(std::min<int64_t>(g_packets_in.exchange(0, std::memory_order_relaxed), 65535));
  g.packets_out = uint16_t(std::min<int64_t>(g_packets_out.exchange(0, std::memory_order_relaxed), 65535));
  g.gap_ms = uint16_t(std::min(g_gap_ms.exchange(0, std::memory_order_relaxed), 65535));
  g.underruns = uint16_t(last_underruns < 0 ? 0 : std::min<int64_t>(underruns - last_underruns, 65535));
  last_underruns = underruns;
  g.culled = uint16_t(std::min<int64_t>(g_culled.exchange(0, std::memory_order_relaxed), 65535));
  g.scene = int8_t(SceneIndex());
  for (int i = 0; i < int(Ours::kCount); ++i) g.ours[i] = float(g_ours_ns[size_t(i)].exchange(0, std::memory_order_relaxed) / 1e6);
  {
    rex::audio::diag::TimeTotals t[int(rex::audio::diag::Timed::kCount)];
    rex::audio::diag::TakeTimes(t);
    const auto& cb = t[int(rex::audio::diag::Timed::kRenderCallback)];
    const auto& xma = t[int(rex::audio::diag::Timed::kXmaWork)];
    g.callbacks = uint16_t(std::min<int64_t>(cb.count, 65535));
    g.callback_ms = float(cb.total_ns / 1e6), g.callback_max = float(cb.max_ns / 1e6), g.xma_ms = float(xma.total_ns / 1e6);
    g.ea_steps = uint16_t(std::min<int64_t>(g_ea_steps.exchange(0, std::memory_order_relaxed), 65535));
    g.ea_ms = float(g_ea_ns.exchange(0, std::memory_order_relaxed) / 1e6);
    g.ea_max = float(g_ea_max_ns.exchange(0, std::memory_order_relaxed) / 1e6);
    NoteAudioTimes(g.ea_max, g.callback_max, g.xma_ms);
  }
  {
    static double last_cpu = -1;
    const double cpu = native_x_renderer::ThreadCpuMs();
    if (cpu >= 0 && last_cpu >= 0) g.cpu_ms = float(cpu - last_cpu);
    last_cpu = cpu;
  }
  g_game.Push(g);
  NoteMovementFrame(g.t_us, ms);  // diagnostics_extra.cpp: rubber banding, input reaching the screen
  // A slow frame in play: a dump 2 s later (what followed it too), at most one every 2 minutes and 10 a run.
  if (ms > 100.0f && SceneIsPlay()) {
    const int64_t last = g_last_auto_us.load(std::memory_order_relaxed);
    if ((!last || g.t_us - last > 120'000'000) && g_auto_dumps.load(std::memory_order_relaxed) < 10) {
      g_last_auto_us = g.t_us;
      ++g_auto_dumps;
      BlackBoxDump(Trigger::kSlowFrame, fmt::format("a game frame took {:.0f} ms", ms), false);
    }
  }
}

void BlackBoxDump(Trigger why, const std::string& detail, bool now) {
  if (!(g_started.load(std::memory_order_relaxed) && On(Area::kBlackBox))) return;
  const int64_t at = NowUs();
  if (now) {
    WriteDump(why, detail, at, why != Trigger::kCrash);
    return;
  }
  {
    std::lock_guard lock(g_jobs_mutex);
    if (g_jobs.size() >= 4) return;  // a burst of marks: the ones queued cover it
    g_jobs.push_back(Job{why, detail, at, at + (why == Trigger::kMark ? 1'000'000 : 2'000'000)});
  }
  g_jobs_cv.notify_one();
}

const char* OursName(Ours what) {
  switch (what) {
    case Ours::kFrameLimit: return "our frame limit's wait";
    case Ours::kSdkSwap: return "the SDK's VdSwap";
    case Ours::kLook: return "our look (the mouse's turn)";
    case Ours::kTickBlend: return "our tick blend";
    case Ours::kWatchdog: return "our hang watchdog's clock";
    case Ours::kGameOptions: return "our game options (volumes)";
    case Ours::kDevTools: return "our dev tools";
    case Ours::kFrameStats: return "our frame statistics and the black box";
    case Ours::kPadHook: return "our pad hook (prompts, controller settings, the team, the map)";
    case Ours::kCount: break;
  }
  return "?";
}
void AddOurs(Ours what, double ms) {
  g_ours_ns[size_t(what)].fetch_add(int64_t(ms * 1e6), std::memory_order_relaxed);
}
OursTimer::OursTimer(Ours what)
    : what_(what), start_ns_(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()) {}
OursTimer::~OursTimer() {
  const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
  g_ours_ns[size_t(what_)].fetch_add(now - start_ns_, std::memory_order_relaxed);
}

void MarkProblem() {
  if (!(g_started.load(std::memory_order_relaxed) && On(Area::kBlackBox))) return;
  const int64_t now = NowUs(), last = g_last_mark_us.load();
  if (last && now - last < 3'000'000) return;  // a held or repeated key
  if (g_marks.load() >= 30) return;
  g_last_mark_us = now;
  g_last_mark_wall = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();
  const int n = ++g_marks;
  REXLOG_WARN("BF mark{}: the player marked a problem (mark {} of this run)", SceneTag(), n);
  BlackBoxDump(Trigger::kMark, fmt::format("mark {}", n), false);
}

int Marks() { return g_marks.load(); }
int64_t LastMarkTime() { return g_last_mark_wall.load(); }
int64_t LastMarkUs() { return g_last_mark_us.load(); }
int BlackBoxDumps() { return g_dumps.load(); }

// ---- counts from the game's hooks

void CountRead(uint64_t bytes, double ms) {
  g_reads.fetch_add(1, std::memory_order_relaxed);
  g_read_bytes.fetch_add(int64_t(bytes), std::memory_order_relaxed);
  MaxOf(g_read_slowest_ms, int32_t(ms + 0.5));
}
void CountPacketIn(int64_t gap_ms) {
  g_packets_in.fetch_add(1, std::memory_order_relaxed);
  if (gap_ms > 0) MaxOf(g_gap_ms, int32_t(std::min<int64_t>(gap_ms, INT32_MAX)));
}
void CountPacketOut() { g_packets_out.fetch_add(1, std::memory_order_relaxed); }

void NoteEaStep(int64_t ns) {
  g_ea_steps.fetch_add(1, std::memory_order_relaxed);
  g_ea_ns.fetch_add(ns, std::memory_order_relaxed);
  int64_t old = g_ea_max_ns.load(std::memory_order_relaxed);
  while (ns > old && !g_ea_max_ns.compare_exchange_weak(old, ns, std::memory_order_relaxed)) {
  }
}

void NoteVoiceCulled() {
  g_culled.fetch_add(1, std::memory_order_relaxed);
  g_culled_total.fetch_add(1, std::memory_order_relaxed);
}
int64_t VoicesCulled() { return g_culled_total.load(std::memory_order_relaxed); }

bool OnGameThread() {
  if (t_main_thread < 0) {
    char name[64] = {};
#if defined(_WIN32)
    using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto get_description = reinterpret_cast<GetDescription>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    PWSTR desc = nullptr;
    if (get_description && SUCCEEDED(get_description(GetCurrentThread(), &desc)) && desc) {
      WideCharToMultiByte(CP_UTF8, 0, desc, -1, name, sizeof(name) - 1, nullptr, nullptr);
      LocalFree(desc);
    }
#else
    pthread_getname_np(pthread_self(), name, sizeof(name));
#endif
    t_main_thread = std::strncmp(name, "Main XThread", 12) == 0 ? 1 : 0;
  }
  return t_main_thread == 1;
}
void CountGameThreadWait(double ms) { g_wait_us.fetch_add(int64_t(ms * 1000), std::memory_order_relaxed); }

}  // namespace bf1943::diagnostics
