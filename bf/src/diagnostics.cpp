// Diagnostics for players' logs (diagnostics.h): the player's choices, the log's anonymizer, the network monitor and
// the start's snapshots.
#include "diagnostics.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <native_x_renderer/native_x_renderer.h>
#include <rex/audio/diagnostics.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "report.h"
#include "world_sync.h"

namespace bf1943::diagnostics {
namespace {

namespace fs = std::filesystem;

struct AreaInfo {
  const char* key;
  const char* name;
};
constexpr AreaInfo kAreas[] = {
    {"diag_audio", "audio"}, {"diag_render", "rendering"}, {"diag_network", "network"}, {"diag_input", "input"},
    {"diag_system", "system"}, {"diag_blackbox", "black box"}};
static_assert(std::size(kAreas) == size_t(Area::kCount));

// Each area's level (Level as an int; 0 off). Changed in the game too (ApplyNow), read by every thread: atomics.
std::array<std::atomic<int>, size_t(Area::kCount)> g_level{};
std::atomic<bool> g_anonymized{false};
bool g_applied = false;  // ApplyAtStart ran (a player's game, not a dedicated server)
int g_log_mb = 5, g_log_files = 20;
fs::path g_user_dir;  // ApplyAtStart's

// Online requests this minute (NoteRequest).
std::mutex g_req_mutex;
int g_req_count = 0, g_req_failed = 0, g_req_lines = 0;
double g_req_slowest = 0;
std::chrono::steady_clock::time_point g_req_minute = std::chrono::steady_clock::now();

// bf1943-settings.txt as the menu writes it ("key = value"), read before the menu is made.
std::map<std::string, std::string> ReadSettings(const fs::path& file) {
  std::map<std::string, std::string> out;
  std::ifstream f(file);
  std::string line;
  auto trim = [](std::string s) {
    s.erase(0, s.find_first_not_of(" \t"));
    s.erase(s.find_last_not_of(" \t\r") + 1);
    return s;
  };
  while (std::getline(f, line)) {
    const size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    out[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
  }
  return out;
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// ---- the anonymizer (the log file's filter: runs on the SDK's log writer thread)

std::shared_ptr<const report::LogScrubRules> g_rules;  // set before the filter, never changed after
bool g_rules_failed = false;
std::mutex g_anon_mutex;  // the names (game threads add them; the writer reads them)
std::string g_own_name;                                 // lower case
std::vector<std::pair<std::string, int>> g_players;     // other players' names (lower case), their numbers
std::mutex g_ip_mutex;    // the addresses' numbers (the writer, and the self test)
std::map<std::string, int> g_ips;                       // addresses as written, their numbers

bool WordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Every whole-word, any-case `name` in `text` (lower case copy `low`, kept in step) replaced by `with`.
void ReplaceName(std::string& text, std::string& low, const std::string& name, const std::string& with) {
  if (name.size() < 3) return;
  for (size_t at = low.find(name); at != std::string::npos; at = low.find(name, at)) {
    const size_t end = at + name.size();
    if ((at > 0 && WordChar(low[at - 1]) && WordChar(name.front())) ||
        (end < low.size() && WordChar(low[end]) && WordChar(name.back()))) {
      at = end;
      continue;
    }
    text.replace(at, name.size(), with);
    low.replace(at, name.size(), Lower(with));
    at += with.size();
  }
}

// Where the message starts after the file pattern's "[date time] [level] [category] [tN] ".
size_t MessageStart(const std::string& line) {
  if (line.empty() || line[0] != '[') return 0;
  size_t at = 0;
  for (int i = 0; i < 4; ++i) {
    const size_t close = line.find("] ", at);
    if (close == std::string::npos) return 0;
    at = close + 2;
  }
  return at;
}

int g_next_player = 1;

void AddName(const std::string& name, bool own) {
  const std::string key = Lower(name);
  // "[TAG]Name": the name alone too (lines give either), with the same number.
  std::string base = key;
  if (base.size() > 2 && base[0] == '[')
    if (const size_t close = base.find(']'); close != std::string::npos && close + 1 < base.size()) base = base.substr(close + 1);
  if (base.size() < 3) return;
  // Not a name that is an everyday word of the log's own lines (27 Nov 2026: the host's own entry, "server", made
  // every "server" in the log [player 13]), nor digits alone.
  static const std::set<std::string> kWords = {
      "server", "servers", "host", "hosts", "player", "players", "admin", "admins", "bot", "bots", "team", "teams",
      "squad", "game", "games", "match", "round", "map", "level", "online", "voice", "chat", "network", "menu",
      "menus", "pause", "none", "unknown", "test", "guest", "user", "name", "names", "dedicated", "console", "the",
      "and", "you", "all", "ping", "relay", "join", "joined", "playing", "loading", "frame", "frames", "audio",
      "sound", "render", "renderer", "window", "system", "health", "diagnostics", "report", "log", "error", "warning",
      "info", "debug", "core", "gpu", "apu", "cpu", "kernel", "client", "local", "remote", "default", "empty", "null",
      "true", "false", "yes", "no", "on", "off", "us", "japan", "marine", "marines", "soldier", "vehicle", "plane",
      "tank", "jeep", "boat", "flag", "base", "spawn", "deploy", "kit", "rifle", "infantry", "pilot", "gunner"};
  if (kWords.count(base) || base.find_first_not_of("0123456789") == std::string::npos) return;
  std::lock_guard lock(g_anon_mutex);
  if (own) {
    g_own_name = base;
    return;
  }
  if (base == g_own_name) return;
  int number = 0;
  bool have_key = false, have_base = false;
  for (const auto& p : g_players) {
    if (p.first == base || p.first == key) number = p.second;
    have_key = have_key || p.first == key, have_base = have_base || p.first == base;
  }
  if (have_key && have_base) return;  // known (the host's list comes again and again)
  if (!number) number = g_next_player++;
  if (!have_base) g_players.emplace_back(base, number);
  if (!have_key && key != base) g_players.emplace_back(key, number);
  // Longest first, so "[TAG]Name" is replaced before "Name" would split it.
  std::stable_sort(g_players.begin(), g_players.end(),
                   [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
}

// ---- the network monitor

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct Sender {
  uint32_t ip = 0;
  uint16_t port = 0;
  int64_t last_us = 0;
  int64_t packets = 0, bytes = 0, gap_max_us = 0, gaps = 0;  // this window
  int64_t steady = 0;  // packets in the window before: a gap counts only in a steady stream (a match's server)
  // The window's spacing between packets (1 ms steps to 100, the last longer), its largest packet, packets over 1400
  // bytes (near a common path's MTU: fragmented on some routers and VPNs).
  std::array<uint32_t, 101> spacing{};
  int64_t largest = 0, over_mtu = 0;
};
// "spacing median 33 ms, 95% 34 ms; the largest 812 bytes" from a sender's window (and cleared).
std::string SpacingText(Sender& s) {
  int64_t total = 0;
  for (uint32_t c : s.spacing) total += c;
  std::string out;
  if (total >= 10) {
    auto at = [&](double share) {
      int64_t seen = 0;
      for (size_t i = 0; i < s.spacing.size(); ++i)
        if ((seen += s.spacing[i]) >= int64_t(total * share)) return int(i);
      return 100;
    };
    out = fmt::format(", spacing median {} ms, 95% {}{} ms", at(0.5), at(0.95) >= 100 ? "over " : "", std::min(at(0.95), 100));
  }
  out += fmt::format(", the largest {} bytes{}", s.largest, s.over_mtu ? fmt::format(" ({} over 1400)", s.over_mtu) : std::string());
  s.spacing.fill(0), s.largest = 0, s.over_mtu = 0;
  return out;
}
std::mutex g_net_mutex;
std::vector<Sender> g_senders;
std::vector<Sender> g_dests;  // where the game sends to (NoteSend): our own gaps
int g_gap_lines = 0;  // gap lines this minute (Scaled(network, 30))

std::string Address(uint32_t ip, uint16_t port) {
  const auto* a = reinterpret_cast<const uint8_t*>(&ip);
  const auto* p = reinterpret_cast<const uint8_t*>(&port);
  return fmt::format("{}.{}.{}.{}:{}", a[0], a[1], a[2], a[3], p[0] << 8 | p[1]);
}

void NetworkThread() {
  int64_t window_start = NowUs(), minute_start = window_start;
  // The ping each second (the game's own number for the joined server): its spread over the window.
  int ping_n = 0, ping_min = 0, ping_max = 0;
  int64_t ping_sum = 0, ping_jitter = 0;
  int ping_last = -1;
  for (int tick = 1;; ++tick) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (const int p = world::OwnPing(); p >= 0) {
      ping_min = ping_n ? std::min(ping_min, p) : p, ping_max = ping_n ? std::max(ping_max, p) : p;
      ping_sum += p, ++ping_n;
      if (ping_last >= 0) ping_jitter += std::abs(p - ping_last);
      ping_last = p;
    }
    if (tick % 10) continue;
    const int64_t now = NowUs();
    const double secs = (now - window_start) / 1e6;
    window_start = now;
    std::vector<std::string> lines;
    {
      std::lock_guard lock(g_net_mutex);
      if (now - minute_start >= 60000000) minute_start = now, g_gap_lines = 0;
      for (Sender& s : g_senders) {
        // A match's server sends many a second; a lookup or a ping answer now and then is not worth a line.
        if (s.packets >= 30)
          lines.push_back(fmt::format("network diagnostics{}: {:.1f} s from {}: {} packets ({:.1f}/s), {:.1f} KB, "
                                      "longest gap {}, {} gap(s) over 150 ms{}",
                                      SceneTag(), secs, Address(s.ip, s.port), s.packets, s.packets / secs,
                                      s.bytes / 1024.0,
                                      s.steady >= 30 ? fmt::format("{:.0f} ms", s.gap_max_us / 1000.0)
                                                     : std::string("not measured yet (the stream just began)"),
                                      s.gaps, SpacingText(s)));
        s.steady = s.packets;
        s.packets = s.bytes = s.gap_max_us = s.gaps = 0;
      }
      std::erase_if(g_senders, [&](const Sender& s) { return now - s.last_us > 60000000; });
      // What we sent: the match's server gets packets many times a second; a pause of ours shows here.
      for (Sender& d : g_dests) {
        if (d.packets >= 30)
          lines.push_back(fmt::format("network diagnostics{}: {:.1f} s to {}: {} packets sent ({:.1f}/s), {:.1f} KB, our "
                                      "longest gap {}, {} gap(s) over 150 ms{}",
                                      SceneTag(), secs, Address(d.ip, d.port), d.packets, d.packets / secs,
                                      d.bytes / 1024.0,
                                      d.steady >= 30 ? fmt::format("{:.0f} ms", d.gap_max_us / 1000.0)
                                                     : std::string("not measured yet"),
                                      d.gaps, SpacingText(d)));
        d.steady = d.packets;
        d.packets = d.bytes = d.gap_max_us = d.gaps = 0;
      }
      std::erase_if(g_dests, [&](const Sender& d) { return now - d.last_us > 60000000; });
    }
    if (!lines.empty()) {
      for (const std::string& l : lines) REXLOG_INFO("{}", l);
      if (ping_n)
        REXLOG_INFO("network diagnostics{}: ping to the joined server {:.0f} ms on average ({} to {} ms, it changed by "
                    "{:.0f} ms a second on average)",
                    SceneTag(), double(ping_sum) / ping_n, ping_min, ping_max,
                    ping_n > 1 ? double(ping_jitter) / (ping_n - 1) : 0.0);
    }
    ping_n = 0, ping_sum = ping_jitter = 0, ping_last = -1;
  }
}

// ---- the scene

constexpr const char* kSceneNames[] = {"starting", "menus",    "loading",     "deploy screen",
                                       "playing",  "paused",   "tutorial",    "end of round"};
std::atomic<bool> g_round_over{false};
static_assert(std::size(kSceneNames) == size_t(Scene::kCount));

// This scene's counts (game threads bump them; the scene's change takes them).
struct SceneCounts {
  std::atomic<int64_t> frames{0}, slow_frames{0}, gaps{0}, long_gaps{0};
  std::atomic<int64_t> recent_slow{0};  // frames over 33 ms since the health thread last took them (RecentSlowFrames)
  std::atomic<int32_t> longest_frame_ms{0}, longest_gap_ms{0};
  // The frame times in 0.5 ms steps up to 100 ms (the last: longer), for the 1% and 0.1% lows; frames whose time
  // differs from the last one's by over half (and 4 ms): uneven pacing (stutter that the average hides).
  std::array<std::atomic<uint32_t>, 201> histogram{};
  std::atomic<int64_t> uneven{0};
};
SceneCounts g_scene_counts;
void MaxOf(std::atomic<int32_t>& a, int32_t v) {
  int32_t old = a.load(std::memory_order_relaxed);
  while (v > old && !a.compare_exchange_weak(old, v, std::memory_order_relaxed)) {
  }
}

std::mutex g_scene_mutex;
std::atomic<int> g_scene{-1};
std::string g_scene_map;
std::chrono::steady_clock::time_point g_scene_since;
rex::audio::diag::Totals g_scene_audio;
native_x_renderer::DiagnosticsTotals g_scene_render;
uint64_t g_scene_render_same = 0;
double g_scene_render_same_ms = 0;
// A join (the menus to loading) until the first spawn: its steps; rounds seen (their end's memory line).
std::chrono::steady_clock::time_point g_join_at{};
bool g_join_deploy_said = false;
int g_rounds = 0;
int64_t g_scene_culled = 0;
std::chrono::steady_clock::time_point g_period_since;  // the scene's last line (its start, or a "still" line)
int g_pending = -1, g_pending_seen = 0;
std::string g_pending_map;

std::string Duration(double s) {
  const int t = int(s);
  return t >= 3600 ? fmt::format("{}h{:02}m{:02}s", t / 3600, t / 60 % 60, t % 60)
         : t >= 60 ? fmt::format("{}m{:02}s", t / 60, t % 60)
                   : fmt::format("{:.1f}s", s);
}

// The frame time at a share of the slowest frames (0.01: the 1% low) from the scene's histogram (ms; -1 none).
double LowFrameMs(const std::array<uint32_t, 201>& h, int64_t frames, double share) {
  const int64_t want = std::max<int64_t>(1, int64_t(frames * share));
  int64_t seen = 0;
  for (int i = 200; i >= 0; --i)
    if ((seen += h[size_t(i)]) >= want) return i == 200 ? 100.0 : (i + 1) * 0.5;
  return -1;
}

// What happened in the scene since its last line (g_scene_mutex held): taken, so the next line starts from zero.
std::string TakeRollup(double secs, int scene) {
  const auto audio = rex::audio::diag::RunTotals();
  const auto render = native_x_renderer::GetDiagnosticsTotals();
  const int64_t culled = VoicesCulled();
  const int64_t frames = g_scene_counts.frames.exchange(0), slow = g_scene_counts.slow_frames.exchange(0),
                gaps = g_scene_counts.gaps.exchange(0), long_gaps = g_scene_counts.long_gaps.exchange(0),
                uneven = g_scene_counts.uneven.exchange(0);
  const int32_t longest = g_scene_counts.longest_frame_ms.exchange(0), gap_ms = g_scene_counts.longest_gap_ms.exchange(0);
  std::array<uint32_t, 201> h{};
  for (size_t i = 0; i < h.size(); ++i) h[i] = g_scene_counts.histogram[i].exchange(0);
  // No frames: the game draws none of its own there (our menus before it runs).
  std::string what = frames ? fmt::format("{} frames ({:.1f}/s), {} over 50 ms (longest {} ms)", frames,
                                          secs > 0 ? frames / secs : 0.0, slow, longest)
                            : std::string("no game frames");
  if (frames >= 100) {
    const double low1 = LowFrameMs(h, frames, 0.01), low01 = LowFrameMs(h, frames, 0.001);
    what += fmt::format("; 1% low {:.0f} FPS ({:.1f} ms), 0.1% low {:.0f} FPS ({:.1f} ms); uneven pacing {:.1f}% of frames",
                        1000.0 / low1, low1, 1000.0 / low01, low01, 100.0 * uneven / frames);
  }
  if (On(Area::kRender)) {
    const uint64_t made = render.pipelines - g_scene_render.pipelines;
    if (made || render.shader_failures != g_scene_render.shader_failures)
      what += fmt::format("; pipelines {} ({} ready at once, {} compiled{}, {} failed), shader failures {}", made,
                          render.pipelines_quick - g_scene_render.pipelines_quick,
                          render.pipelines_slow - g_scene_render.pipelines_slow,
                          render.pipelines_slow > g_scene_render.pipelines_slow
                              ? fmt::format(", {:.0f} ms each on average",
                                            (render.pipelines_slow_ms - g_scene_render.pipelines_slow_ms) /
                                                double(render.pipelines_slow - g_scene_render.pipelines_slow))
                              : std::string(),
                          render.pipelines_failed - g_scene_render.pipelines_failed,
                          render.shader_failures - g_scene_render.shader_failures);
    if (render.texture_loads != g_scene_render.texture_loads)
      what += fmt::format("; texture loads {} ({:.1f} MB, {:.0f} ms on the renderer's thread)",
                          render.texture_loads - g_scene_render.texture_loads,
                          (render.texture_load_bytes - g_scene_render.texture_load_bytes) / 1048576.0,
                          render.texture_load_ms - g_scene_render.texture_load_ms);
    if (render.hd_made != g_scene_render.hd_made || render.hd_remade != g_scene_render.hd_remade)
      what += fmt::format("; HD copies {} made, {} made again", render.hd_made - g_scene_render.hd_made,
                          render.hd_remade - g_scene_render.hd_remade);
    if (render.swap_chain_resizes != g_scene_render.swap_chain_resizes ||
        render.swap_chain_rebuilds != g_scene_render.swap_chain_rebuilds)
      what += fmt::format("; swap chain resized {}, rebuilt {}", render.swap_chain_resizes - g_scene_render.swap_chain_resizes,
                          render.swap_chain_rebuilds - g_scene_render.swap_chain_rebuilds);
  }
  if (On(Area::kAudio)) {
    what += fmt::format("; audio: {} underrun(s), {} starved voice(s), {} unreadable frame(s), mixer {} late",
                        audio.underruns - g_scene_audio.underruns, audio.starved - g_scene_audio.starved,
                        audio.bad_frames - g_scene_audio.bad_frames, audio.mixer_late - g_scene_audio.mixer_late);
    if (culled != g_scene_culled) what += fmt::format(", EA's mixer looked overloaded {} time(s)", culled - g_scene_culled);
  }
  if (On(Area::kNetwork) && (gaps || gap_ms))
    what += fmt::format("; network: {} gap(s) over 150 ms, {} over 400 ms (longest {} ms)", gaps, long_gaps, gap_ms);
  g_scene_audio = audio, g_scene_render = render, g_scene_culled = culled;
  if (render.hd_same_again != g_scene_render_same) {
    what += fmt::format("; HD copies made again the same as before: {} ({:.0f} ms a cache would have saved)",
                        render.hd_same_again - g_scene_render_same, render.hd_same_again_ms - g_scene_render_same_ms);
    g_scene_render_same = render.hd_same_again, g_scene_render_same_ms = render.hd_same_again_ms;
  }
  return what + TakeExtraRollup(scene, secs);
}

std::string SceneWithMap(int scene) {
  return std::string(kSceneNames[scene]) + (g_scene_map.empty() ? "" : " (" + g_scene_map + ")");
}

// ---- snapshots

void LogSettings(const char* what, const std::vector<std::string>& categories, const char* prefix = nullptr) {
  std::vector<std::string> names;
  for (const std::string& c : categories) {
    auto list = rex::cvar::ListFlagsByCategory(c);
    names.insert(names.end(), list.begin(), list.end());
  }
  if (prefix)
    for (const std::string& n : rex::cvar::ListFlags())
      if (const auto* info = rex::cvar::GetFlagInfo(n); info && info->category.rfind(prefix, 0) == 0) names.push_back(n);
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  std::string line;
  int changed = 0;
  for (const std::string& n : names) {
    const auto* info = rex::cvar::GetFlagInfo(n);
    const std::string value = rex::cvar::GetFlagByName(n);
    if (!info || value == info->default_value) continue;
    ++changed;
    line += (line.empty() ? "" : ", ") + n + "=" + value;
    if (line.size() > 600) {
      REXLOG_INFO("BF diagnostics: {} settings not at their defaults: {}", what, line);
      line.clear();
    }
  }
  if (!line.empty() || !changed)
    REXLOG_INFO("BF diagnostics: {} settings not at their defaults: {}", what, changed ? line : "none");
}

}  // namespace

const char* Key(Area a) { return kAreas[size_t(a)].key; }
const char* Name(Area a) { return kAreas[size_t(a)].name; }
bool On(Area a) { return g_level[size_t(a)].load(std::memory_order_relaxed) != 0; }
Level LevelOf(Area a) { return Level(g_level[size_t(a)].load(std::memory_order_relaxed)); }
const char* LevelName(Level l) {
  switch (l) {
    case Level::kOff: return "off";
    case Level::kQuiet: return "quiet";
    case Level::kNormal: return "normal";
    case Level::kDetailed: return "detailed";
    case Level::kVerbose: return "verbose";
  }
  return "?";
}
bool Want(Area a, Level at_least) { return g_applied && g_level[size_t(a)].load(std::memory_order_relaxed) >= int(at_least); }
int Scaled(Area a, int normal) {
  switch (LevelOf(a)) {
    case Level::kOff:
    case Level::kQuiet: return 0;
    case Level::kNormal: return normal;
    case Level::kDetailed: return normal * 4;
    case Level::kVerbose: return normal * 20;
  }
  return normal;
}
bool AnyOn() {
  for (const auto& l : g_level)
    if (l.load(std::memory_order_relaxed)) return true;
  return false;
}
bool Anonymized() { return g_anonymized.load(); }

// The areas' levels from the settings (the menu's rows): an area off, or at Logging verbosity.
void ReadLevels(const std::map<std::string, std::string>& settings) {
  auto get = [&](const std::string& k, const std::string& def) {
    const auto it = settings.find(k);
    return it == settings.end() || it->second.empty() ? def : it->second;
  };
  const std::string verbosity = get(kVerbosityKey, "normal");
  const Level overall = verbosity == "verbose" ? Level::kVerbose : verbosity == "detailed" ? Level::kDetailed : Level::kNormal;
  for (size_t i = 0; i < g_level.size(); ++i) {
    const std::string v = get(kAreas[i].key, "1");
    Level l = v == "0" || v == "off" ? Level::kOff
              : v == "quiet"         ? Level::kQuiet
              : v == "detailed"      ? Level::kDetailed
              : v == "verbose"       ? Level::kVerbose
                                     : overall;  // "1" (On), "normal": Logging verbosity
#if defined(BF_DEV_TOOLS)
    // BF_DEV_DIAG_LEVEL=<level> (tests): every area at that level.
    if (const char* d = std::getenv("BF_DEV_DIAG_LEVEL"); d && *d && l != Level::kOff)
      l = std::string(d) == "quiet" ? Level::kQuiet : std::string(d) == "detailed" ? Level::kDetailed
        : std::string(d) == "verbose" ? Level::kVerbose : Level::kNormal;
#endif
    g_level[i].store(int(l));
  }
}

// What the levels switch outside this file: the SDK's audio diagnostics, the renderer's lines and thresholds, its GPU
// timers. At the start, and again whenever a Diagnostics row changes in the game (ApplyNow).
void ApplyLevelSwitches() {
  rex::cvar::SetFlagByName("audio_diagnostics", On(Area::kAudio) ? "true" : "false");
  const Level lr = LevelOf(Area::kRender);
  rex::cvar::SetFlagByName("native_x_renderer_diagnostics", On(Area::kRender) ? "true" : "false");
  rex::cvar::SetFlagByName("native_x_renderer_gpu_sections", On(Area::kRender) ? "true" : "false");
  rex::cvar::SetFlagByName("native_x_renderer_diagnostics_level", std::to_string(int(lr)));
  // The renderer's slow frames: from 50 ms at Normal, 33 Detailed, 20 Verbose; 100 (a player's game) with Rendering off.
  const double want = lr == Level::kOff ? 100 : lr == Level::kQuiet ? 100 : lr == Level::kDetailed ? 33.4
                    : lr == Level::kVerbose ? 20 : 50;
  rex::cvar::SetFlagByName("native_x_renderer_hitch_ms", std::to_string(want));
}

void ApplyAtStart(const fs::path& user_dir) {
  const auto settings = ReadSettings(user_dir / "bf1943-settings.txt");
  auto get = [&](const std::string& k, const std::string& def) {
    const auto it = settings.find(k);
    return it == settings.end() || it->second.empty() ? def : it->second;
  };
  ReadLevels(settings);
  g_applied = true, g_user_dir = user_dir;

  // The log's size (each file; rotated ones kept beside it as bf1943.1.log ...).
  const std::string size = get(kLogSizeKey, "auto");
  g_log_mb = size == "large" ? 200 : size == "small" ? 5 : AnyOn() ? 25 : 5;
  g_log_files = g_log_mb > 5 ? 2 : 20;
  rex::cvar::SetFlagByName("log_max_file_size_mb", std::to_string(g_log_mb));
  rex::cvar::SetFlagByName("log_max_files", std::to_string(g_log_files));

  ApplyLevelSwitches();

  // The anonymizer: the report's privacy rules for this PC (its user and PC names, the recovery code, the install
  // id), with the soldier name. Lines go through it on the SDK's log writer thread.
  report::Context c;
  c.user_dir = user_dir;
  c.account_code = get("account_code", "");
  try {
    g_rules = report::MakeLogScrub(report::MakeScrubber(c));
  } catch (...) {
    g_rules_failed = true;
  }
  NoteOwnName(get("player_name", ""));
#if defined(BF_DEV_TOOLS)
  // BF_DEV_LOG_RAW=1 (dev builds): the log as it is, for our own runs (the file is still written on its own thread).
  if (const char* v = std::getenv("BF_DEV_LOG_RAW"); v && *v == '1') {
    rex::cvar::SetFlagByName("log_file_writer_thread", "true");
    return;
  }
#endif
  g_anonymized = true;
  rex::SetLogFileFilter(AnonymizeLine);
}

// BF_DEV_LOG_SELFTEST=1 (dev builds): the report's scrub cases and the log anonymizer's, in the log at the start.
void SelfTest() {
  std::string failures;
  const bool scrub = report::ScrubSelfTest(failures);
  // The anonymizer's state aside for the test, and back after it.
  decltype(g_players) players;
  decltype(g_ips) ips;
  std::string own;
  int next = 1;
  {
    std::scoped_lock lock(g_anon_mutex, g_ip_mutex);
    players.swap(g_players), ips.swap(g_ips), own.swap(g_own_name), std::swap(next, g_next_player);
  }
  AddName("SoldierOne", true);
  AddName("[ABC]Enemy_7", false);
  AddName("Medic", false);
  AddName("Server", false);  // the host's own entry: an everyday word, never a name to replace
  const std::string p = "[2026-11-26 10:00:00.000] [info] [core] [t12] ";
  const std::pair<std::string, std::string> cases[] = {
      {p + "BF LIVE: playing as SoldierOne (id 0001E240)", p + "BF LIVE: playing as [you] (id 0001E240)"},
      {p + "BF comms: chat all [ABC]Enemy_7: (5 characters)", p + "BF comms: chat all [player 1]: (5 characters)"},
      {p + "enemy_7 and MEDIC joined", p + "[player 1] and [player 2] joined"},
      {p + "Medical kit picked up", p + "Medical kit picked up"},
      {p + "ping to the joined server 33 ms", p + "ping to the joined server 33 ms"},
      {p + "joined 203.0.113.24:3658, then 198.51.100.9", p + "joined [ip 1], then [ip 2]"},
      {p + "again 203.0.113.24:3658", p + "again [ip 1]"},
      {p + "bind to 127.0.0.1:0", p + "bind to 127.0.0.1:0"},
      {"no prefix SoldierOne", "no prefix [you]"},
  };
  for (const auto& [in, want] : cases) {
    const std::string got = AnonymizeLine(in);
    if (got != want) failures += "  \"" + in + "\" gave \"" + got + "\", wanted \"" + want + "\"\n";
  }
  {
    std::scoped_lock lock(g_anon_mutex, g_ip_mutex);
    players.swap(g_players), ips.swap(g_ips), own.swap(g_own_name), std::swap(next, g_next_player);
  }
  if (scrub && failures.empty()) REXLOG_INFO("BF diagnostics: log self test passed");
  else REXLOG_ERROR("BF diagnostics: log self test FAILED:\n{}", failures);
}

void AfterLogOpened() {
  if (!g_applied) return;
#if defined(BF_DEV_TOOLS)
  if (const char* v = std::getenv("BF_DEV_LOG_SELFTEST"); v && *v == '1' && Anonymized()) SelfTest();
#endif
  REXLOG_INFO("BF build: {}", BuildLine());
  std::string on, off;
  for (size_t i = 0; i < g_level.size(); ++i) {
    const bool is_on = g_level[i].load() != 0;
    std::string& to = is_on ? on : off;
    to += (to.empty() ? "" : ", ") + std::string(kAreas[i].name) +
          (is_on ? std::string(" (") + LevelName(Level(g_level[i].load())) + ")" : std::string());
  }
  REXLOG_INFO("BF diagnostics: on: {}; off: {}; the log file up to {} MB ({} older kept); {}{}",
              on.empty() ? "none" : on, off.empty() ? "none" : off, g_log_mb, g_log_files,
              Anonymized() ? "anonymized as it is written (names, addresses, keys and chat replaced)"
                           : "NOT anonymized (BF_DEV_LOG_RAW, a dev build)",
              g_rules_failed ? "; the privacy rules could not start: names only" : "");
  StartParts();
}

// The parts with threads of their own, started once when their area is on (at the log's start, or when a row turns it
// on in the game).
void StartParts() {
  static std::atomic<bool> network_started{false};
  if (On(Area::kNetwork) && !network_started.exchange(true)) std::thread(NetworkThread).detach();
  StartHealth();
  StartBlackBox();
  StartProfiler();
}

void ApplyNow() {
  if (!g_applied) return;
  std::array<int, size_t(Area::kCount)> before{};
  for (size_t i = 0; i < before.size(); ++i) before[i] = g_level[i].load();
  ReadLevels(ReadSettings(g_user_dir / "bf1943-settings.txt"));
  std::string changes;
  for (size_t i = 0; i < before.size(); ++i)
    if (before[i] != g_level[i].load())
      changes += fmt::format("{}{} {} -> {}", changes.empty() ? "" : ", ", kAreas[i].name, LevelName(Level(before[i])),
                             LevelName(Level(g_level[i].load())));
  if (changes.empty()) return;
  ApplyLevelSwitches();
  StartParts();
  StartEventWatch();
  REXLOG_INFO("BF diagnostics{}: changed in the game, at once: {}", SceneTag(), changes);
}

void AtGameStart() {
  if (!g_applied) return;
#if defined(BF_DEV_TOOLS)
  // BF_DEV_DIAG_CHANGE_AT=<s>:<key>=<value> (tests): that long after the game's start, the setting written to the settings
  // file and applied as the Diagnostics rows do (ApplyNow).
  if (const char* v = std::getenv("BF_DEV_DIAG_CHANGE_AT"); v && *v) {
    const std::string spec = v;
    const size_t colon = spec.find(':'), eq = spec.find('=');
    if (colon != std::string::npos && eq != std::string::npos && eq > colon) {
      const double at = std::atof(spec.c_str());
      const std::string key = spec.substr(colon + 1, eq - colon - 1), value = spec.substr(eq + 1);
      std::thread([at, key, value] {
        std::this_thread::sleep_for(std::chrono::milliseconds(int64_t(at * 1000)));
        std::ofstream(g_user_dir / "bf1943-settings.txt", std::ios::app) << key << " = " << value << "\n";
        ApplyNow();
      }).detach();
    }
  }
#endif
  LogSystem(g_user_dir);
  StartEventWatch();
  if (On(Area::kRender)) LogSettings("rendering", {"NativeXRenderer", "BF1943/Renderer", "BF1943/Performance", "GPU"});
  if (On(Area::kAudio)) LogSettings("audio", {"Audio", "BF1943/Audio", "BF1943/Voice"});
  if (On(Area::kInput)) LogSettings("input", {"BF1943/Controls", "Input"}, "Input/");
}

void NoteOwnName(const std::string& name) { AddName(name, true); }
void NotePlayerName(const std::string& name) {
  if (Anonymized()) AddName(name, false);
}

std::string AnonymizeLine(const std::string& line) {
  const size_t from = MessageStart(line);
  // Warnings and errors counted by kind (the scene's line and the run's summary): our fix-ups firing among them.
  if (from && (line.find("] [warning] ") < from || line.find("] [error] ") < from || line.find("] [critical] ") < from))
    CountWarning(line.substr(from));
  std::string msg = line.substr(from), low = Lower(msg);
  {
    std::lock_guard lock(g_anon_mutex);
    ReplaceName(msg, low, g_own_name, "[you]");
    for (const auto& [name, n] : g_players) ReplaceName(msg, low, name, "[player " + std::to_string(n) + "]");
  }
  if (g_rules) {
    std::lock_guard lock(g_ip_mutex);
    msg = report::ScrubLogLine(msg, *g_rules, [](const std::string& addr) {
      auto [it, added] = g_ips.try_emplace(addr, int(g_ips.size()) + 1);
      return "[ip " + std::to_string(it->second) + "]";
    });
  }
  return line.substr(0, from) + msg;
}

std::string Summary() {
  std::string out;
  for (size_t i = 0; i < g_level.size(); ++i)
    if (g_level[i].load()) out += (out.empty() ? "" : ", ") + std::string(kAreas[i].name);
  return out.empty() ? "off" : out;
}

void NoteScene(Scene scene, const std::string& map) {
  if (!g_applied) return;
  const int s = int(scene);
  std::string ended, began, join_line;
  int round_end = 0;
  {
    std::lock_guard lock(g_scene_mutex);
    const int cur = g_scene.load();
    const auto now = std::chrono::steady_clock::now();
    if (s == cur && map == g_scene_map) {
      g_pending = -1;
      // A long scene (a whole match) also gets a line every 10 minutes: a log cut short (a crash, the process
      // killed) still has the scene's numbers.
      if (now - g_period_since < std::chrono::minutes(10)) return;
      const double part = std::chrono::duration<double>(now - g_period_since).count();
      ended = fmt::format("BF scene: still {} after {}; the last {}: {}", SceneWithMap(cur),
                          Duration(std::chrono::duration<double>(now - g_scene_since).count()), Duration(part),
                          TakeRollup(part, cur));
      g_period_since = now;
    } else {
      if (s != g_pending || map != g_pending_map) g_pending = s, g_pending_map = map, g_pending_seen = 0;
      // Half a second (two notes); the deploy screen a second; loading and leaving it at once (a short load matters).
      const int need = scene == Scene::kDeploy ? 4 : scene == Scene::kLoading || cur == int(Scene::kLoading) ? 1 : 2;
      if (++g_pending_seen < need) return;
      if (cur >= 0) {
        const double secs = std::chrono::duration<double>(now - g_scene_since).count();
        const double part = std::chrono::duration<double>(now - g_period_since).count();
        // After "still" lines, this one covers the time since the last of them.
        const std::string since = part + 1 < secs ? fmt::format("; the last {}: ", Duration(part)) : std::string(": ");
        ended = fmt::format("BF scene: {} ended after {}{}{}", SceneWithMap(cur), Duration(secs), since, TakeRollup(part, cur));
      } else {
        TakeRollup(0, -1);  // the counts so far: the first scene starts from zero
      }
      // A join's steps (the menus to loading, the map loaded, alive), the deploy screen's time, a round's end.
      const double cur_secs = cur >= 0 ? std::chrono::duration<double>(now - g_scene_since).count() : 0;
      if (s == int(Scene::kLoading) && (cur == int(Scene::kMenus) || cur == int(Scene::kStarting))) {
        g_join_at = now, g_join_deploy_said = false;
        NoteJoin();
      }
      const bool joining = g_join_at != std::chrono::steady_clock::time_point{};
      const double since_join = joining ? std::chrono::duration<double>(now - g_join_at).count() : 0;
      if (joining && s == int(Scene::kDeploy) && !g_join_deploy_said) {
        g_join_deploy_said = true;
        join_line = fmt::format("BF join: the map loaded {:.1f} s after the join began ({})", since_join,
                                map.empty() ? std::string("map not known") : map);
      }
      if (cur == int(Scene::kDeploy) && s == int(Scene::kPlaying)) {
        NoteDeployToAlive(cur_secs);
        if (joining) {
          join_line = fmt::format("BF join: alive {:.1f} s after the join began ({:.1f} s on the deploy screen)",
                                  since_join, cur_secs);
          g_join_at = {};
        }
      }
      if (s == int(Scene::kRoundEnd)) round_end = ++g_rounds;
      g_scene = s, g_scene_map = map, g_scene_since = now, g_period_since = now, g_pending = -1;
      rex::audio::diag::SetScene(kSceneNames[s]);
      began = fmt::format("BF scene: now {}{}", kSceneNames[s], map.empty() ? "" : " (" + map + ")");
    }
  }
  if (!ended.empty()) REXLOG_INFO("{}", ended);
  if (!began.empty()) REXLOG_INFO("{}", began);
  if (!join_line.empty()) REXLOG_INFO("{}", join_line);
  if (round_end) LogRoundMemory(round_end);
}

void AtExit() {
  if (!g_applied) return;
  std::string line;
  {
    std::lock_guard lock(g_scene_mutex);
    const int cur = g_scene.load();
    if (cur >= 0) {
      const auto now = std::chrono::steady_clock::now();
      const double secs = std::chrono::duration<double>(now - g_scene_since).count();
      const double part = std::chrono::duration<double>(now - g_period_since).count();
      const std::string since = part + 1 < secs ? fmt::format("; the last {}: ", Duration(part)) : std::string(": ");
      line = fmt::format("BF scene: {} ended after {} (the game closed){}{}", SceneWithMap(cur), Duration(secs), since,
                         TakeRollup(part, cur));
      g_scene = -1;
    }
  }
  if (!line.empty()) REXLOG_INFO("{}", line);
  REXLOG_INFO("BF diagnostics: the game closed; marks {}, black box dumps {}", Marks(), BlackBoxDumps());
  LogRunSummary();
}

int SceneIndex() { return g_scene.load(std::memory_order_relaxed); }
bool SceneIsPlay() {
  const int s = g_scene.load(std::memory_order_relaxed);
  return s == int(Scene::kPlaying) || s == int(Scene::kDeploy) || s == int(Scene::kTutorial);
}

const char* MarkKeyName() {
  thread_local std::string key;
  key = rex::cvar::GetFlagByName("keybind_mark_problem");
  if (const size_t comma = key.find(','); comma != std::string::npos) key.resize(comma);
  if (key.empty()) key = "no key bound";
  return key.c_str();
}

void NoteRoundOver(bool over) { g_round_over = over; }
int64_t TakeRecentSlowFrames() { return g_scene_counts.recent_slow.exchange(0); }
bool RoundOver() { return g_round_over.load(); }

const char* SceneName() {
  const int s = g_scene.load(std::memory_order_relaxed);
  return s >= 0 ? kSceneNames[s] : "";
}
std::string SceneTag() {
  const int s = g_scene.load(std::memory_order_relaxed);
  return s >= 0 ? std::string(" [") + kSceneNames[s] + "]" : std::string();
}

void NoteFrameTime(float ms) {
  BlackBoxGameFrame(ms);
  // Each game frame on the game's thread: the last one's time for the pacing.
  static float last = 0;
  if (last > 0 && std::abs(ms - last) > std::max(4.0f, 0.5f * std::min(ms, last)))
    g_scene_counts.uneven.fetch_add(1, std::memory_order_relaxed);
  last = ms;
  g_scene_counts.histogram[size_t(std::clamp(int(ms * 2.0f), 0, 200))].fetch_add(1, std::memory_order_relaxed);
  g_scene_counts.frames.fetch_add(1, std::memory_order_relaxed);
  if (ms > 50.0f) g_scene_counts.slow_frames.fetch_add(1, std::memory_order_relaxed);
  if (ms > 33.4f) g_scene_counts.recent_slow.fetch_add(1, std::memory_order_relaxed);
  MaxOf(g_scene_counts.longest_frame_ms, int32_t(ms + 0.5f));
}

void NoteRequest(const char* kind, const std::string& what, bool ok, double ms, const char* failure) {
  if (!g_applied || !On(Area::kNetwork)) return;
  std::string line, minute;
  {
    std::lock_guard lock(g_req_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now - g_req_minute >= std::chrono::minutes(1)) {
      if (g_req_count)
        minute = fmt::format("network diagnostics{}: online requests in the last minute: {}, {} failed, the slowest "
                             "{:.0f} ms",
                             SceneTag(), g_req_count, g_req_failed, g_req_slowest);
      g_req_count = g_req_failed = g_req_lines = 0, g_req_slowest = 0, g_req_minute = now;
    }
    ++g_req_count, g_req_failed += !ok, g_req_slowest = std::max(g_req_slowest, ms);
    if ((!ok || ms > (Want(Area::kNetwork, Level::kVerbose) ? 500 : 2000)) && g_req_lines < Scaled(Area::kNetwork, 20)) {
      ++g_req_lines;
      line = fmt::format("network diagnostics{}: {} {} {} after {:.0f} ms{}", SceneTag(), kind, what,
                         ok ? "answered (slow)" : "FAILED", ms, failure && *failure ? fmt::format(" ({})", failure) : "");
    }
  }
  if (!minute.empty()) REXLOG_INFO("{}", minute);
  if (!line.empty()) REXLOG_INFO("{}", line);
}

void NoteReceive(uint32_t ip, uint16_t port, int bytes) {
  if (!On(Area::kNetwork)) {
    CountPacketIn(0);
    return;
  }
  const int64_t now = NowUs();
  std::string gap_line;
  {
    std::lock_guard lock(g_net_mutex);
    Sender* s = nullptr;
    for (Sender& x : g_senders)
      if (x.ip == ip && x.port == port) s = &x;
    if (!s) {
      if (g_senders.size() >= 16) return;  // never more than a few in a match
      s = &g_senders.emplace_back();
      s->ip = ip, s->port = port;
    }
    int64_t gap_ms = 0;
    if (s->last_us && s->steady >= 30) {
      const int64_t gap = now - s->last_us;
      if (gap > 150000) gap_ms = gap / 1000;
      s->gap_max_us = std::max(s->gap_max_us, gap);
      if (gap > 150000) ++s->gaps, g_scene_counts.gaps.fetch_add(1, std::memory_order_relaxed);
      if (gap > 400000) g_scene_counts.long_gaps.fetch_add(1, std::memory_order_relaxed);
      if (gap > 150000) MaxOf(g_scene_counts.longest_gap_ms, int32_t(gap / 1000));
      if (gap > 400000 && g_gap_lines < Scaled(Area::kNetwork, 30)) {
        ++g_gap_lines;
        gap_line = fmt::format("network diagnostics{}: no packet from {} for {:.0f} ms", SceneTag(), Address(ip, port),
                               gap / 1000.0);
      }
    }
    if (s->last_us) ++s->spacing[size_t(std::min<int64_t>((now - s->last_us) / 1000, 100))];
    s->largest = std::max<int64_t>(s->largest, bytes), s->over_mtu += bytes > 1400;
    s->last_us = now;
    ++s->packets;
    s->bytes += bytes;
    CountPacketIn(gap_ms);
  }
  if (!gap_line.empty()) REXLOG_INFO("{}", gap_line);
}

void NoteSend(uint32_t ip, uint16_t port, int bytes) {
  CountPacketOut();
  if (!On(Area::kNetwork)) return;
  const int64_t now = NowUs();
  std::string gap_line;
  {
    std::lock_guard lock(g_net_mutex);
    Sender* d = nullptr;
    for (Sender& x : g_dests)
      if (x.ip == ip && x.port == port) d = &x;
    if (!d) {
      if (g_dests.size() >= 16) return;
      d = &g_dests.emplace_back();
      d->ip = ip, d->port = port;
    }
    if (d->last_us && d->steady >= 30) {
      const int64_t gap = now - d->last_us;
      d->gap_max_us = std::max(d->gap_max_us, gap);
      if (gap > 150000) ++d->gaps;
      if (gap > 400000 && g_gap_lines < Scaled(Area::kNetwork, 30)) {
        ++g_gap_lines;
        gap_line = fmt::format("network diagnostics{}: we sent nothing to {} for {:.0f} ms", SceneTag(),
                               Address(ip, port), gap / 1000.0);
      }
    }
    if (d->last_us) ++d->spacing[size_t(std::min<int64_t>((now - d->last_us) / 1000, 100))];
    d->largest = std::max<int64_t>(d->largest, bytes), d->over_mtu += bytes > 1400;
    d->last_us = now;
    ++d->packets;
    d->bytes += bytes;
  }
  if (!gap_line.empty()) REXLOG_INFO("{}", gap_line);
}

}  // namespace bf1943::diagnostics
