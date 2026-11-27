// Settings > Diagnostics, the second round (27 Nov 2026; the engine designer's list after the first match with the
// black box): what a report needs that the first round did not say.
//
//   build         the version servers are told, the commit and whether it had local changes, the SDK's version
//   ghosts        the game's network objects ("ghosts", sub_8275D230 and its table lookup sub_8275D0D8): an id the
//                 server named that this game does not have, or has without its object, with the id and the guest
//                 caller; the most objects known against the game's 8192 (its Network.MaxGhostCount)
//   movement      the view jumping on foot further in one frame than the soldier can move (a server's position
//                 correction: rubber banding)
//   input         from a key or mouse press to the game's next input read and to the next frame; presses that began
//                 and ended between two reads (the game may not have seen them)
//   devices       audio devices added and removed, controllers connected and disconnected in a match (SDL's events)
//   display       the refresh, the cap, V-Sync, the video driver and the desktop; presents later than 1.5 frames
//   renderer      the game's waits for room in the renderer's queue, the frames the renderer is behind
//   streams       reads of the sound and music files over 50 ms (a stream arriving late)
//   warnings      every warning and error line counted by kind (our fix-ups firing: the null page, skipped calls, the
//                 kernel's stand-ins), per scene and for the run
//   settings      a setting changed while the game runs
//   summary       at exit: the time in each kind of scene, the worst moments the black box saw and their causes, the
//                 joins and deploys, the totals of the above
//
// Everything is counted where it happens (relaxed atomics or a short lock) and said at a scene's end, a minute's line
// or the exit.
#include "diagnostics.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <SDL3/SDL.h>
#include <fmt/format.h>

#include <native_x_renderer/native_x_renderer.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/version.h>

namespace bf1943 {
bool LocalOnFoot();      // look.cpp
const char* BfVersion();  // pipeline_upload.cpp
}  // namespace bf1943

#ifndef BF_GIT_COMMIT
#define BF_GIT_COMMIT "unknown"
#endif
#ifndef BF_GIT_DIRTY
#define BF_GIT_DIRTY 0
#endif

namespace bf1943::diagnostics {
namespace {

using Clock = std::chrono::steady_clock;
int64_t NowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); }

void MaxOf(std::atomic<int64_t>& a, int64_t v) {
  int64_t old = a.load(std::memory_order_relaxed);
  while (v > old && !a.compare_exchange_weak(old, v, std::memory_order_relaxed)) {
  }
}

// At most Scaled(area, normal) lines a minute from one place: the area's level as it is now (it can change in the
// game).
class Budget {
 public:
  Budget(Area area, int normal) : area_(area), normal_(normal) {}
  bool Take() {
    std::lock_guard lock(mutex_);
    const auto now = Clock::now();
    if (now - minute_ >= std::chrono::minutes(1)) minute_ = now, used_ = 0;
    return used_++ < Scaled(area_, normal_);
  }

 private:
  std::mutex mutex_;
  Clock::time_point minute_ = Clock::now();
  int used_ = 0;
  const Area area_;
  const int normal_;
};

// A millisecond histogram (1 ms steps to 250, the last longer) for medians of short times.
struct MsHistogram {
  std::array<std::atomic<uint32_t>, 251> counts{};
  std::atomic<int64_t> n{0}, longest_us{0};
  void Add(double ms) {
    counts[size_t(std::clamp(int(ms), 0, 250))].fetch_add(1, std::memory_order_relaxed);
    n.fetch_add(1, std::memory_order_relaxed);
    MaxOf(longest_us, int64_t(ms * 1000));
  }
  // "median 12 ms, 95% 30 ms, longest 41 ms" and cleared; "" when empty.
  std::string Take() {
    std::array<uint32_t, 251> c{};
    int64_t total = 0;
    for (size_t i = 0; i < c.size(); ++i) total += c[i] = counts[i].exchange(0, std::memory_order_relaxed);
    n.store(0, std::memory_order_relaxed);
    const int64_t longest = longest_us.exchange(0, std::memory_order_relaxed);
    if (!total) return {};
    auto at = [&](double share) {
      int64_t seen = 0;
      for (size_t i = 0; i < c.size(); ++i)
        if ((seen += c[i]) >= int64_t(std::ceil(total * share))) return int(i);
      return 250;
    };
    return fmt::format("median {} ms, 95% {} ms, longest {:.0f} ms", at(0.5), at(0.95), longest / 1000.0);
  }
};

// ---- the scene's counts (taken at its end) and the run's

struct Counts {
  std::atomic<int64_t> ghost_unknown{0}, ghost_empty{0}, ghost_none{0};
  std::atomic<int64_t> jumps{0}, jump_cm_max{0};
  std::atomic<int64_t> presses{0}, taps_missed{0};
  std::atomic<int64_t> late_presents{0}, game_waited_us{0}, behind_max{0};
  std::atomic<int64_t> slow_streams{0}, stream_slowest_ms{0};
  std::atomic<int64_t> culled_near_slow{0};
  std::atomic<int64_t> ea_max_us{0}, callback_max_us{0}, xma_us{0};
};
Counts g_scene, g_run;
MsHistogram g_to_read, g_to_frame;  // a press to the game's read, to the next frame (scene)
MsHistogram g_run_to_frame;
std::atomic<int64_t> g_ghosts_peak{0}, g_ghosts_now{0};
std::atomic<int64_t> g_last_slow_us{0};  // the last game frame over 33 ms (NoteMovementFrame)
constexpr int64_t kGhostLimit = 8192;  // Network.MaxGhostCount (13 bit ids)

void Bump(std::atomic<int64_t> Counts::*field, int64_t by = 1) {
  (g_scene.*field).fetch_add(by, std::memory_order_relaxed);
  (g_run.*field).fetch_add(by, std::memory_order_relaxed);
}

// ---- ghosts (sub_8275D230: an object id read from a packet, its object or 0; sub_8275D0D8: the id's table entry)

struct Lookup {
  bool called = false, found = false;
  uint32_t id = 0;
};
thread_local Lookup t_lookup;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
}

void NoteGhost(const uint8_t* base, uint32_t self, uint32_t caller, uint32_t result) {
  const int64_t known = Load32(base, self + 472);  // the table's entries (its bucket count is at +468)
  g_ghosts_now.store(known, std::memory_order_relaxed);
  MaxOf(g_ghosts_peak, known);
  if (result) return;
  const Lookup l = t_lookup;
  if (!l.called) {  // id 0: "no object" (a soldier in no vehicle, ...): normal
    g_scene.ghost_none.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Bump(l.found ? &Counts::ghost_empty : &Counts::ghost_unknown);
  static Budget budget(Area::kNetwork, 30);
  if (Want(Area::kNetwork, Level::kNormal) && budget.Take())
    REXLOG_INFO("BF ghosts{}: the server named object {} {} (guest caller {:08X}; {} objects known)", SceneTag(), l.id,
                l.found ? "known to this game but without its object (gone, or not made yet)"
                        : "this game does not have (its creation never arrived, or it was removed here)",
                caller, known);
}

// ---- movement: the view's jump on foot

struct Movement {
  float eye[3] = {};
  bool have = false;
  double usual_m = 0;       // the usual move a frame (an average that follows slowly)
  int64_t on_foot_since = 0;
  // A jump waiting to be confirmed: a death (the deploy screen follows) or a respawn moves the view too, so a jump
  // counts only when the soldier is still on foot and playing a second later.
  int64_t pending_at = 0;
  double pending_m = 0, pending_usual = 0;
} g_move;  // the game frame thread's only

// ---- input timing (SDL's event watch: the UI thread; the read: the game's input thread; frames: VdSwap's)

std::atomic<uint64_t> g_press_ns{0};        // the oldest press not read yet (SDL's ticks, ns), 0 none
std::atomic<uint64_t> g_read_press_ns{0};   // a press the game read, waiting for its frame
std::atomic<uint64_t> g_last_read_ns{0};    // the game's last input read
std::mutex g_keys_mutex;
std::unordered_map<uint32_t, uint64_t> g_down_ns;  // a key or button held: when it went down

// ---- display

std::atomic<double> g_target_ms{0};  // the frame time the game aims at (the cap or the refresh)
std::atomic<bool> g_display_said{false};

// ---- sound streams' handles

std::mutex g_handles_mutex;
std::unordered_set<uint32_t> g_sound_handles;

// ---- warnings by kind

std::mutex g_warn_mutex;
std::map<std::string, int64_t> g_warn_scene, g_warn_run;

// A message's kind: its numbers and addresses left out, at most 90 characters.
std::string Kind(const std::string& m) {
  std::string out;
  bool in_number = false;
  for (size_t i = 0; i < m.size() && out.size() < 90; ++i) {
    const unsigned char c = static_cast<unsigned char>(m[i]);
    const bool digitish = std::isdigit(c) || (in_number && std::isxdigit(c));
    if (digitish) {
      if (!in_number) out += '#';
      in_number = true;
      continue;
    }
    in_number = false;
    out += char(c);
  }
  return out;
}

std::string TopKinds(const std::map<std::string, int64_t>& kinds, size_t n) {
  std::vector<std::pair<int64_t, std::string>> v;
  for (const auto& [k, c] : kinds) v.push_back({c, k});
  std::sort(v.rbegin(), v.rend());
  std::string out;
  for (size_t i = 0; i < v.size() && i < n; ++i) out += fmt::format("{}{}x \"{}\"", i ? ", " : "", v[i].first, v[i].second);
  return out;
}

// ---- the run's summary

std::mutex g_summary_mutex;
std::array<double, size_t(Scene::kCount)> g_scene_secs{};
struct TriggerSeen {
  std::string why, cause;
  float ms;
};
std::vector<TriggerSeen> g_triggers;
std::vector<double> g_deploys;  // the deploy screen to alive, seconds
int g_joins = 0;

const char* kSceneNames[] = {"starting", "menus", "loading", "deploy screen", "playing", "paused", "tutorial",
                             "end of round"};

}  // namespace

// ---- build

std::string BuildLine() {
  return fmt::format("version {} (what servers are told), commit {}{} (as of the build's configure), {} build, "
                     "ReXGlue SDK {}",
                     BfVersion(), BF_GIT_COMMIT, BF_GIT_DIRTY ? " with local changes" : "",
#if defined(BF_DEV_TOOLS)
                     "dev",
#else
                     "release",
#endif
                     REXGLUE_VERSION_STRING);
}

// ---- SDL's events

namespace {
bool EventWatch(void*, SDL_Event* e) {
  switch (e->type) {
    case SDL_EVENT_AUDIO_DEVICE_ADDED:
    case SDL_EVENT_AUDIO_DEVICE_REMOVED: {
      if (e->adevice.recording) break;
      const char* name = SDL_GetAudioDeviceName(e->adevice.which);
      if (Want(Area::kAudio, Level::kNormal))
        REXLOG_INFO("BF audio{}: an output device {}: {}", SceneTag(),
                    e->type == SDL_EVENT_AUDIO_DEVICE_ADDED ? "came" : "went away", name ? name : "(no name)");
      break;
    }
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED: {
      const int s = SceneIndex();
      if (s >= int(Scene::kLoading) && Want(Area::kInput, Level::kNormal)) {  // the launcher logs its own; in a match it is news
        const char* name = e->type == SDL_EVENT_GAMEPAD_ADDED ? SDL_GetGamepadNameForID(e->gdevice.which) : nullptr;
        REXLOG_INFO("BF input{}: a controller {}{}", SceneTag(),
                    e->type == SDL_EVENT_GAMEPAD_ADDED ? "connected" : "disconnected",
                    name ? fmt::format(" ({})", name) : std::string());
      }
      break;
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
      if (e->type == SDL_EVENT_KEY_DOWN && e->key.repeat) break;
      if (std::string(SceneName()) != "playing") break;
      const uint64_t ns = e->common.timestamp ? e->common.timestamp : SDL_GetTicksNS();
      const uint32_t key = e->type == SDL_EVENT_KEY_DOWN ? uint32_t(e->key.scancode) : 0x10000u + e->button.button;
      {
        std::lock_guard lock(g_keys_mutex);
        g_down_ns[key] = ns;
      }
      uint64_t zero = 0;
      g_press_ns.compare_exchange_strong(zero, ns);
      Bump(&Counts::presses);
      break;
    }
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      const uint32_t key = e->type == SDL_EVENT_KEY_UP ? uint32_t(e->key.scancode) : 0x10000u + e->button.button;
      uint64_t down = 0;
      {
        std::lock_guard lock(g_keys_mutex);
        auto it = g_down_ns.find(key);
        if (it == g_down_ns.end()) break;
        down = it->second;
        g_down_ns.erase(it);
      }
      // Down and up with no input read of the game between them: it may never have seen the press.
      if (g_last_read_ns.load(std::memory_order_relaxed) < down && g_last_read_ns.load() != 0) Bump(&Counts::taps_missed);
      break;
    }
    default:
      break;
  }
  return true;
}
}  // namespace

void StartEventWatch() {
  static std::atomic<bool> started{false};
  if (!AnyOn() || started.exchange(true)) return;
  SDL_AddEventWatch(EventWatch, nullptr);
  // The desktop the game runs on (said once: the compositor and the video driver decide a lot of the pacing).
  const char* driver = SDL_GetCurrentVideoDriver();
#if defined(_WIN32)
  const std::string desktop = "Windows (the desktop window manager composes)";
#else
  auto env = [](const char* k) { const char* v = std::getenv(k); return std::string(v && *v ? v : "?"); };
  std::string desktop = fmt::format("{} session, desktop {}", env("XDG_SESSION_TYPE"), env("XDG_CURRENT_DESKTOP"));
  if (std::getenv("GAMESCOPE_WAYLAND_DISPLAY")) desktop += ", under gamescope";
  if (std::getenv("WINEPREFIX") || std::getenv("STEAM_COMPAT_DATA_PATH")) desktop += ", Wine or Proton";
#endif
  if (On(Area::kRender))
    REXLOG_INFO("BF display: video driver {}; {}; variable refresh (VRR) is not reported to the game", driver ? driver : "?",
                desktop);
}

// ---- input

void NoteInputRead() {
  const uint64_t now = SDL_GetTicksNS();
  g_last_read_ns.store(now, std::memory_order_relaxed);
  if (const uint64_t press = g_press_ns.exchange(0, std::memory_order_relaxed); press && now > press) {
    g_to_read.Add((now - press) / 1e6);
    static Budget budget(Area::kInput, 3);  // Verbose only, in effect: 60 a minute
    if (Want(Area::kInput, Level::kVerbose) && budget.Take())
      REXLOG_INFO("BF input{}: a press reached the game's input read after {:.1f} ms", SceneTag(), (now - press) / 1e6);
    uint64_t zero = 0;
    g_read_press_ns.compare_exchange_strong(zero, press);
  }
}

// ---- display and the renderer's frames

void NoteDisplay(double refresh_hz, double cap_fps, bool vsync) {
  const double refresh_ms = refresh_hz > 0 ? 1000.0 / refresh_hz : 0, cap_ms = cap_fps > 0 ? 1000.0 / cap_fps : 0;
  const double target = std::max(refresh_ms, cap_ms);
  const double old = g_target_ms.exchange(target);
  if (Want(Area::kRender, Level::kNormal) && (std::abs(old - target) > 0.01 || !g_display_said.exchange(true)))
    REXLOG_INFO("BF display{}: the screen at {:.2f} Hz; the frame rate cap {}; V-Sync {}; the frame time aimed at {:.2f} ms",
                SceneTag(), refresh_hz, cap_fps > 0 ? fmt::format("{:.0f}", cap_fps) : std::string("none"),
                vsync ? "on" : "off", target);
}

void NoteRenderFrame(float interval_ms, float game_waited_ms, uint16_t frames_behind) {
  const double target = g_target_ms.load(std::memory_order_relaxed);
  if (target > 0 && interval_ms > 1.5 * target && SceneIsPlay()) Bump(&Counts::late_presents);
  if (game_waited_ms > 0) Bump(&Counts::game_waited_us, int64_t(game_waited_ms * 1000));
  MaxOf(g_scene.behind_max, frames_behind);
  MaxOf(g_run.behind_max, frames_behind);
}

// ---- movement (each game frame, VdSwap's thread)

void NoteMovementFrame(int64_t now_us, float frame_ms) {
  if (frame_ms > 33.4f) g_last_slow_us.store(now_us, std::memory_order_relaxed);
  // A press the game read is shown in this frame.
  if (const uint64_t press = g_read_press_ns.exchange(0, std::memory_order_relaxed)) {
    const uint64_t ns = SDL_GetTicksNS();
    if (ns > press) g_to_frame.Add((ns - press) / 1e6), g_run_to_frame.Add((ns - press) / 1e6);
  }
  float eye[3], right[3], forward[3];
  double age = 1;
  const bool playing = std::string(SceneName()) == "playing";
  if (!playing || !LocalOnFoot() || !native_x_renderer::ListenerView(eye, right, forward, &age) || age > 0.2) {
    g_move.have = false, g_move.on_foot_since = 0, g_move.pending_at = 0;  // a death or a seat: not a correction
    return;
  }
  if (g_move.pending_at && now_us - g_move.pending_at > 1'000'000) {
    const double d = g_move.pending_m;
    g_move.pending_at = 0;
    Bump(&Counts::jumps);
    MaxOf(g_scene.jump_cm_max, int64_t(d * 100));
    MaxOf(g_run.jump_cm_max, int64_t(d * 100));
    static Budget budget(Area::kNetwork, 20);
    if (Want(Area::kNetwork, Level::kNormal) && budget.Take())
      REXLOG_INFO("BF movement{}: the view jumped {:.1f} m in one frame on foot a second ago (usually {:.2f} m a frame), "
                  "and the soldier is still on foot: a position correction from the server (rubber banding), most likely",
                  SceneTag(), d, g_move.pending_usual);
  }
  if (!g_move.on_foot_since) g_move.on_foot_since = now_us;
  if (g_move.have) {
    const double dx = eye[0] - g_move.eye[0], dy = eye[1] - g_move.eye[1], dz = eye[2] - g_move.eye[2];
    const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
    // On foot for 2 s (a spawn's move left out); over 2 m and 8 times the usual move in one frame.
    // Over 2 m and 8 times the usual move in one frame, under 100 m (further is a teleport: a spawn, a death camera).
    if (now_us - g_move.on_foot_since > 2'000'000 && d > 2.0 && d < 100.0 && d > 8 * std::max(g_move.usual_m, 0.05)) {
      if (!g_move.pending_at) g_move.pending_at = now_us, g_move.pending_m = d, g_move.pending_usual = g_move.usual_m;
    } else {
      g_move.usual_m = g_move.usual_m * 0.95 + d * 0.05;
    }
  }
  std::copy(eye, eye + 3, g_move.eye);
  g_move.have = true;
}

int64_t LastSlowFrameUs() { return g_last_slow_us.load(std::memory_order_relaxed); }

// ---- files

void NoteOpenedHandle(uint32_t handle, const std::string& path) {
  std::string low = path;
  for (char& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
  const bool sound = low.find("sound") != std::string::npos || low.find("music") != std::string::npos ||
                     low.find("\\vo-") != std::string::npos || low.find("/vo-") != std::string::npos;
  std::lock_guard lock(g_handles_mutex);
  if (sound) g_sound_handles.insert(handle);
  else g_sound_handles.erase(handle);  // a handle reused for another file
}

bool IsSoundHandle(uint32_t handle) {
  std::lock_guard lock(g_handles_mutex);
  return g_sound_handles.count(handle) != 0;
}

void NoteSlowStream(double ms) {
  Bump(&Counts::slow_streams);
  MaxOf(g_scene.stream_slowest_ms, int64_t(ms));
  MaxOf(g_run.stream_slowest_ms, int64_t(ms));
}

// ---- warnings, settings

void CountWarning(const std::string& message) {
  const std::string kind = Kind(message);
  std::lock_guard lock(g_warn_mutex);
  if (g_warn_run.size() < 400 || g_warn_run.count(kind)) ++g_warn_run[kind];
  if (g_warn_scene.size() < 200 || g_warn_scene.count(kind)) ++g_warn_scene[kind];
}

void NoteSettingChanged(const std::string& name, const std::string& value) {
  if (SceneIndex() < 0 || !Want(Area::kSystem, Level::kNormal)) return;
  REXLOG_INFO("BF settings{}: {} changed to {}", SceneTag(), name, value);
}

// ---- the scene's part, the summary

void NoteTriggerCause(const char* why, float ms, const std::string& cause) {
  std::lock_guard lock(g_summary_mutex);
  if (g_triggers.size() < 200) g_triggers.push_back({why, cause, ms});
}

std::string TakeExtraRollup(int scene, double secs) {
  {
    std::lock_guard lock(g_summary_mutex);
    if (scene >= 0 && scene < int(Scene::kCount)) g_scene_secs[size_t(scene)] += secs;
  }
  auto take = [](std::atomic<int64_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
  std::string out;
  const int64_t unknown = take(g_scene.ghost_unknown), empty = take(g_scene.ghost_empty), none = take(g_scene.ghost_none);
  if (On(Area::kNetwork) && (unknown || empty || g_ghosts_now.load()))
    out += fmt::format("; ghosts: {} unknown and {} without their object ({} \"no object\" references), {} objects known "
                       "now of {}",
                       unknown, empty, none, g_ghosts_now.load(), kGhostLimit);
  const int64_t jumps = take(g_scene.jumps), jump_cm = take(g_scene.jump_cm_max);
  if (jumps) out += fmt::format("; rubber banding: {} jump(s) on foot, the longest {:.1f} m", jumps, jump_cm / 100.0);
  const int64_t presses = take(g_scene.presses), missed = take(g_scene.taps_missed);
  const std::string to_read = g_to_read.Take(), to_frame = g_to_frame.Take();
  if (On(Area::kInput) && presses)
    out += fmt::format("; input: {} presses, to the game's read {}, to the frame {}{}", presses,
                       to_read.empty() ? "?" : to_read, to_frame.empty() ? "?" : to_frame,
                       missed ? fmt::format(", {} too short for the game to see", missed) : std::string());
  const int64_t late = take(g_scene.late_presents), waited = take(g_scene.game_waited_us), behind = take(g_scene.behind_max);
  if (On(Area::kRender) && (late || waited >= 1000 || behind > 2))
    out += fmt::format("; pacing: {} presents over 1.5 frames late, the game waited {:.0f} ms for the renderer, the "
                       "renderer at most {} frame(s) behind",
                       late, waited / 1000.0, behind);
  const int64_t streams = take(g_scene.slow_streams), stream_ms = take(g_scene.stream_slowest_ms);
  if (On(Area::kAudio) && streams) out += fmt::format("; sound stream reads over 50 ms: {} (the slowest {} ms)", streams, stream_ms);
  const int64_t ea_max = take(g_scene.ea_max_us), cb_max = take(g_scene.callback_max_us), xma = take(g_scene.xma_us);
  if (On(Area::kAudio) && (ea_max || cb_max))
    out += fmt::format("; the audio threads: EA's control step the longest {:.1f} ms, the render callback the longest "
                       "{:.1f} ms, the XMA decoder {:.1f} ms a second",
                       ea_max / 1000.0, cb_max / 1000.0, secs > 0 ? xma / 1000.0 / secs : 0.0);
  const int64_t near = take(g_scene.culled_near_slow);
  if (On(Area::kAudio) && near) out += fmt::format("; EA's mixer looked overloaded {} time(s) within 0.2 s of a slow frame", near);
  std::map<std::string, int64_t> warns;
  {
    std::lock_guard lock(g_warn_mutex);
    warns.swap(g_warn_scene);
  }
  if (!warns.empty()) {
    int64_t total = 0;
    for (const auto& [k, c] : warns) total += c;
    out += fmt::format("; warnings and errors: {} of {} kind(s): {}", total, warns.size(), TopKinds(warns, 3));
  }
  return out;
}

void NoteCulledNearSlow() { Bump(&Counts::culled_near_slow); }

void NoteAudioTimes(float ea_max_ms, float callback_max_ms, float xma_ms) {
  MaxOf(g_scene.ea_max_us, int64_t(ea_max_ms * 1000));
  MaxOf(g_scene.callback_max_us, int64_t(callback_max_ms * 1000));
  g_scene.xma_us.fetch_add(int64_t(xma_ms * 1000), std::memory_order_relaxed);
  MaxOf(g_run.ea_max_us, int64_t(ea_max_ms * 1000));
  MaxOf(g_run.callback_max_us, int64_t(callback_max_ms * 1000));
}

void NoteDeployToAlive(double secs) {
  std::lock_guard lock(g_summary_mutex);
  g_deploys.push_back(secs);
}
void NoteJoin() {
  std::lock_guard lock(g_summary_mutex);
  ++g_joins;
}

void LogRunSummary() {
  std::lock_guard lock(g_summary_mutex);
  std::string scenes;
  double total = 0;
  for (size_t i = 0; i < g_scene_secs.size(); ++i) total += g_scene_secs[i];
  for (size_t i = 0; i < g_scene_secs.size(); ++i)
    if (g_scene_secs[i] >= 1)
      scenes += fmt::format("{}{} {:.0f}% ({:.0f} s)", scenes.empty() ? "" : ", ", kSceneNames[i],
                            total > 0 ? 100 * g_scene_secs[i] / total : 0.0, g_scene_secs[i]);
  REXLOG_INFO("BF summary: {:.0f} minutes: {}", total / 60, scenes.empty() ? "no scenes" : scenes);
  // The black box's triggers: the worst five, and how many had each cause.
  if (!g_triggers.empty()) {
    std::vector<TriggerSeen> t = g_triggers;
    std::sort(t.begin(), t.end(), [](const TriggerSeen& a, const TriggerSeen& b) { return a.ms > b.ms; });
    std::map<std::string, int> causes;
    for (const TriggerSeen& x : g_triggers) ++causes[Kind(x.cause)];  // the numbers left out: like causes count together
    std::string by;
    for (const auto& [c, n] : causes) by += fmt::format("{}{}x {}", by.empty() ? "" : ", ", n, c);
    std::string worst;
    for (size_t i = 0; i < t.size() && i < 5; ++i)
      worst += fmt::format("{}{} {:.0f} ms: {}", i ? "; " : "", t[i].why, t[i].ms, t[i].cause);
    REXLOG_INFO("BF summary: the black box wrote {} time(s); causes: {}; the worst: {}", g_triggers.size(), by, worst);
  }
  if (!g_deploys.empty()) {
    std::vector<double> d = g_deploys;
    std::sort(d.begin(), d.end());
    REXLOG_INFO("BF summary: {} join(s); the deploy screen to alive {} time(s), median {:.1f} s, longest {:.1f} s", g_joins,
                d.size(), d[d.size() / 2], d.back());
  }
  const auto r = native_x_renderer::GetDiagnosticsTotals();
  REXLOG_INFO("BF summary: ghosts: {} unknown, {} without their object, the most known {} of {}; rubber banding: {} "
              "jump(s) (the longest {:.1f} m); input: {} presses, to the frame {}, {} too short to see; pacing: {} late "
              "presents, the game waited {:.1f} s for the renderer; sound stream reads over 50 ms: {}",
              g_run.ghost_unknown.load(), g_run.ghost_empty.load(), g_ghosts_peak.load(), kGhostLimit, g_run.jumps.load(),
              g_run.jump_cm_max.load() / 100.0, g_run.presses.load(),
              [&] { std::string s = g_run_to_frame.Take(); return s.empty() ? std::string("?") : s; }(),
              g_run.taps_missed.load(), g_run.late_presents.load(), g_run.game_waited_us.load() / 1e6,
              g_run.slow_streams.load());
  REXLOG_INFO("BF summary: renderer: {} pipelines ({} compiled here, {} failed), {} shader failures; HD copies {} made, "
              "{} made again ({} the same picture as before: {:.0f} ms a cache would have saved); pipeline storage: {} "
              "pipelines, {} shaders added ({:.1f} MB){}",
              r.pipelines, r.pipelines_slow, r.pipelines_failed, r.shader_failures, r.hd_made, r.hd_remade,
              r.hd_same_again, r.hd_same_again_ms, r.stored_pipelines, r.stored_shaders, r.stored_bytes / 1048576.0,
              r.storage_failed ? ", a WRITE FAILED: the next start compiles them again" : "");
  std::map<std::string, int64_t> warns;
  {
    std::lock_guard wlock(g_warn_mutex);
    warns = g_warn_run;
  }
  if (!warns.empty()) {
    int64_t total_w = 0;
    for (const auto& [k, c] : warns) total_w += c;
    REXLOG_INFO("BF summary: warnings and errors: {} of {} kind(s); the most: {}", total_w, warns.size(), TopKinds(warns, 8));
  }
}

}  // namespace bf1943::diagnostics

// ---- the ghost hooks (every build; the dev build's line in game_fixes.cpp became these)

REX_EXTERN(__imp__sub_8275D0D8);
REX_HOOK_RAW(sub_8275D0D8) {
  const uint32_t id = ctx.r4.u32 & 0xFFFF;
  __imp__sub_8275D0D8(ctx, base);
  bf1943::diagnostics::t_lookup = {true, ctx.r3.u32 != 0, id};
}

REX_EXTERN(__imp__sub_8275D230);
REX_HOOK_RAW(sub_8275D230) {
  const uint32_t self = ctx.r3.u32, caller = uint32_t(ctx.lr);
  bf1943::diagnostics::t_lookup.called = false;
  __imp__sub_8275D230(ctx, base);
  bf1943::diagnostics::NoteGhost(base, self, caller, ctx.r3.u32);
}
