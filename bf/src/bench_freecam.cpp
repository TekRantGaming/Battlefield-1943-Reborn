// Benchmark (developer builds, tools/bench): the game's own free camera, flown on a timed path over the level, so a
// benchmark sees the whole level the same way every run. BF_BENCH_FREECAM=<seconds after joining>[,<path>[,<value>]]:
//   orbit[,<s a lap>] (default, 60 s): a circle around the middle of the capture points, wide enough to take them all
//                    in, 60 m above the highest, looking at the middle and down onto the level
//   flags[,<m/s>]    (default 30 m/s): low, a smooth loop through the capture points 25 m above each (a closed
//                    centripetal Catmull-Rom spline, the points in their order around the middle), at a steady
//                    speed, looking 60 m ahead along it and a little down
//   props[,<m/s>]    (default 20 m/s): the same through the groups of explosive props the server listed
//                    ($BF_BENCH_DIR/props.txt, bench_explode.cpp), 20 m above them, looking down onto the group ahead
// The HUD is hidden while it flies (native_x_renderer_hide_hud).
//
// BF_BENCH_EXPLODE_VIEW=<metres>[,<half angle>] (with the server's BF_BENCH_EXPLODE=view, bench_explode.cpp): the
// explosive props the server listed ($BF_BENCH_DIR/props.txt) go off in view: each when it is in the camera's view cone
// (on the ground plane, default 25 degrees to each side, of any height) and within that distance; a prop the path never
// brings that near while in view, when it is as near as it will be in view. Before it flies, one lap of the path is
// gone through (a pose every 0.05 s) for each prop's nearest in view, so a group off the path gets its own distance on
// any level and path. The props due are added to $BF_BENCH_DIR/trigger.txt (their lines in props.txt), which the
// server reads 10 times a second.
//
// The client builds a FreeCamera at start (sub_8224FB50: client +0xA4 the camera, +0xAC its camera manager, the free
// camera there as camera 0). The client's camera mode switch sub_82254B28(client, mode) (a virtual, 0x8200BF00) makes
// it the view with mode 1 (0: the soldier's). Its matrix at camera +0x10 (rows right, up, forward, place, as the
// soldier's at +0x3A0) is the view from then on; the camera's own update (sub_82288500: pad input onto x, y, z, yaw,
// pitch at +0x98, then the matrix) is not run in this build, so its place stays 9999,9999,9999 until written. Here the
// matrix is written at every pad read (auto_join.cpp's XamInputGetState), on the game's thread.

#if defined(BF_DEV_TOOLS)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "capture_points.h"

namespace bf1943 {
bool InMatch();
}

namespace {

constexpr float kPi = 3.14159265f;
uint32_t g_client = 0;

uint32_t Load32(const uint8_t* base, uint32_t at) {
  uint32_t v;
  std::memcpy(&v, base + at, 4);
  return __builtin_bswap32(v);
}
float LoadF(const uint8_t* base, uint32_t at) {
  const uint32_t v = Load32(base, at);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void StoreF(uint8_t* base, uint32_t at, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  std::memcpy(base + at, &v, 4);
}

struct V3 {
  float x = 0, y = 0, z = 0;
};
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Norm(V3 a) {
  const float l = std::sqrt(Dot(a, a));
  return l > 1e-6f ? a * (1.f / l) : V3{0, 0, 1};
}
V3 LoadV(const uint8_t* base, uint32_t at) { return {LoadF(base, at), LoadF(base, at + 4), LoadF(base, at + 8)}; }
void StoreRow(uint8_t* base, uint32_t at, V3 v, float w) {
  StoreF(base, at, v.x), StoreF(base, at + 4, v.y), StoreF(base, at + 8, v.z), StoreF(base, at + 12, w);
}

struct Config {
  double at = -1;
  std::string path = "orbit";
  double value = 0;  // orbit: seconds a lap; flags: metres a second (0: the default)
};
Config g_cfg;

// A closed centripetal Catmull-Rom spline through the points, sampled into a polyline with the distance along it.
struct Spline {
  std::vector<V3> at;
  std::vector<float> dist;  // from the start, at each sample; the last is the whole length (back at the start)
  void Build(const std::vector<V3>& p) {
    const size_t n = p.size();
    constexpr int kSteps = 200;
    for (size_t i = 0; i < n; ++i) {
      const V3 p0 = p[(i + n - 1) % n], p1 = p[i], p2 = p[(i + 1) % n], p3 = p[(i + 2) % n];
      auto knot = [](float t, V3 a, V3 b) { return t + std::sqrt(std::sqrt(Dot(b - a, b - a))) + 1e-3f; };
      const float t0 = 0, t1 = knot(t0, p0, p1), t2 = knot(t1, p1, p2), t3 = knot(t2, p2, p3);
      for (int s = 0; s < kSteps; ++s) {
        const float t = t1 + (t2 - t1) * s / kSteps;
        const V3 a1 = p0 * ((t1 - t) / (t1 - t0)) + p1 * ((t - t0) / (t1 - t0));
        const V3 a2 = p1 * ((t2 - t) / (t2 - t1)) + p2 * ((t - t1) / (t2 - t1));
        const V3 a3 = p2 * ((t3 - t) / (t3 - t2)) + p3 * ((t - t2) / (t3 - t2));
        const V3 b1 = a1 * ((t2 - t) / (t2 - t0)) + a2 * ((t - t0) / (t2 - t0));
        const V3 b2 = a2 * ((t3 - t) / (t3 - t1)) + a3 * ((t - t1) / (t3 - t1));
        at.push_back(b1 * ((t2 - t) / (t2 - t1)) + b2 * ((t - t1) / (t2 - t1)));
      }
    }
    at.push_back(at.front());
    dist.assign(at.size(), 0);
    for (size_t i = 1; i < at.size(); ++i) dist[i] = dist[i - 1] + std::sqrt(Dot(at[i] - at[i - 1], at[i] - at[i - 1]));
  }
  float Length() const { return dist.empty() ? 0 : dist.back(); }
  V3 At(float s) const {
    const float len = Length();
    if (len <= 0) return {};
    s = std::fmod(s, len);
    if (s < 0) s += len;
    const size_t i = std::upper_bound(dist.begin(), dist.end(), s) - dist.begin();
    const size_t a = i == 0 ? 0 : i - 1, b = std::min(i, at.size() - 1);
    const float span = dist[b] - dist[a];
    return span > 0 ? at[a] + (at[b] - at[a]) * ((s - dist[a]) / span) : at[a];
  }
};

// The path's state, set when the camera takes the view.
Spline g_spline;
V3 g_middle;
float g_radius = 0, g_top = 0;

double Speed() { return g_cfg.value > 0 ? g_cfg.value : g_cfg.path == "props" ? 20.0 : 30.0; }
double LapSeconds() {
  if ((g_cfg.path == "flags" || g_cfg.path == "props") && g_spline.Length() > 0) return g_spline.Length() / Speed();
  return g_cfg.value >= 5 ? g_cfg.value : 60.0;
}

// The camera's place and the point it looks at, `t` seconds into its flight.
void Pose(double t, V3* place, V3* look) {
  if ((g_cfg.path == "flags" || g_cfg.path == "props") && g_spline.Length() > 0) {
    const float s = float(std::fmod(t * Speed(), double(g_spline.Length())));
    *place = g_spline.At(s);
    *look = g_spline.At(s + 60.f);
    look->y -= g_cfg.path == "props" ? 18 : 10;  // the props: down onto the group ahead
    return;
  }
  const float angle = float(std::fmod(t / LapSeconds(), 1.0)) * 2 * kPi;
  const float r = g_radius + 60;
  *place = g_middle + V3{std::cos(angle) * r, 0, std::sin(angle) * r};
  place->y = g_top + 60;
  *look = g_middle;
}

// BF_BENCH_EXPLODE_VIEW: the props, the distance each goes off at, and those already sent.
struct ViewProp {
  V3 at;
  float trigger = 0;  // 0: never in view on this path
  bool sent = false;
};
std::vector<ViewProp> g_view_props;
double g_view_cos = 0;
std::string g_trigger_path;

// Horizontal: whether `p` is in the cone from `place` toward `look`, and how far it is.
bool InView(V3 place, V3 look, V3 p, float* dist) {
  const float fx = look.x - place.x, fz = look.z - place.z, fl = std::hypot(fx, fz);
  const float dx = p.x - place.x, dz = p.z - place.z, d = std::hypot(dx, dz);
  *dist = d;
  if (fl < 1e-3f || d < 1e-3f) return d < 1e-3f;
  return (dx * fx + dz * fz) / (d * fl) >= g_view_cos;
}

// From take off until the server's list is there (it writes it after its scan of the level, and deletes the last
// level's at a level change): the props, and for each the distance it goes off at (one lap gone through). True when
// done, or when there is nothing to do.
bool PrepareView() {
  const char* v = std::getenv("BF_BENCH_EXPLODE_VIEW");
  const char* dir = std::getenv("BF_BENCH_DIR");
  if (!v || !*v || !dir || !*dir) return true;
  double distance = 80, half_angle = 25;
  std::sscanf(v, "%lf,%lf", &distance, &half_angle);
  g_view_cos = std::cos(half_angle * kPi / 180.0);
  g_trigger_path = std::string(dir) + "/trigger.txt";
  FILE* f = std::fopen((std::string(dir) + "/props.txt").c_str(), "r");
  if (!f) return false;  // not yet: asked again
  char line[256];
  V3 p;
  while (std::fgets(line, sizeof(line), f))
    if (std::sscanf(line, "%f %f %f", &p.x, &p.y, &p.z) == 3) g_view_props.push_back({p});
  std::fclose(f);
  std::vector<float> nearest(g_view_props.size(), 1e9f);
  const double lap = LapSeconds();
  for (double t = 0; t < lap; t += 0.05) {
    V3 place, look;
    Pose(t, &place, &look);
    for (size_t i = 0; i < g_view_props.size(); ++i)
      if (float d; InView(place, look, g_view_props[i].at, &d)) nearest[i] = std::min(nearest[i], d);
  }
  int own = 0, never = 0;
  for (size_t i = 0; i < g_view_props.size(); ++i) {
    if (nearest[i] >= 1e9f) {
      ++never;
      continue;
    }
    g_view_props[i].trigger = std::max(float(distance), nearest[i] + 2.f);
    own += nearest[i] + 2.f > float(distance);
  }
  REXLOG_INFO("BF bench: {} props go off in view within {:.0f} m ({} farther, as near as the path brings them; {} never "
              "in view on this path)", g_view_props.size() - never, distance, own, never);
  return true;
}

// Each frame: the props now due, added to the server's trigger file.
void ExplodeInView(V3 place, V3 look) {
  if (g_view_props.empty()) return;
  std::string due;
  for (size_t i = 0; i < g_view_props.size(); ++i) {
    ViewProp& p = g_view_props[i];
    if (p.sent || p.trigger <= 0) continue;
    if (float d; InView(place, look, p.at, &d) && d <= p.trigger) p.sent = true, due += std::to_string(i) + "\n";
  }
  if (due.empty()) return;
  if (FILE* f = std::fopen(g_trigger_path.c_str(), "a")) {
    std::fputs(due.c_str(), f);
    std::fclose(f);
  }
}

}  // namespace

// The client's start (sub_8224FB50, r3 the client): its free camera made; the client kept for the mode switch.
REX_EXTERN(__imp__sub_8224FB50);
REX_HOOK_RAW(sub_8224FB50) {
  const uint32_t client = ctx.r3.u32;
  __imp__sub_8224FB50(ctx, base);
  g_client = client;
}

REX_EXTERN(sub_82254B28);  // the client's camera mode switch

namespace bf1943 {
// From the pad read (auto_join.cpp), on the game's thread. `hidden`: our menu or the menu driver has the pad.
void BenchFreeCam(PPCContext& ctx, uint8_t* base, bool hidden) {
  static const bool configured = [] {
    if (const char* v = std::getenv("BF_BENCH_FREECAM"); v && *v) {
      char path[32] = {};
      std::sscanf(v, "%lf,%31[a-z],%lf", &g_cfg.at, path, &g_cfg.value);
      if (*path) g_cfg.path = path;
    }
    return true;
  }();
  (void)configured;
  const Config& cfg = g_cfg;
  if (cfg.at < 0 || !g_client) return;
  using Clock = std::chrono::steady_clock;
  static Clock::time_point joined{}, flying{};
  static bool on = false;
  static float handed = 1;  // +1: right = up x forward, as the soldier's matrix has it; -1 the other way
  static std::vector<V3> points;
  V3& middle = g_middle;
  float &radius = g_radius, &top = g_top;
  Spline& spline = g_spline;
  const auto now = Clock::now();
  // The level: its capture points (none between levels: the match goes on in auto_join's sense through a server's
  // level change, so InMatch alone does not tell). Looked at 4 times a second.
  static bool level = false;
  static bool view_ready = false;  // the props' list read (PrepareView), or none wanted
  static Clock::time_point looked{};
  if (now - looked >= std::chrono::milliseconds(250)) looked = now, level = bf1943::InMatch() && !CapturePoints().empty();
  if (!level) {
    // Between levels: everything of this level's flight starts again, so the next level takes off on its own
    // capture points and props, its --freecam delay counted from its arrival.
    if (on || joined.time_since_epoch().count()) {
      on = false, joined = {}, flying = {}, points.clear();
      g_middle = {}, g_radius = 0, g_top = 0, g_spline = Spline{};
      g_view_props.clear();
      rex::cvar::SetFlagByName("native_x_renderer_hide_hud", "false");
      REXLOG_INFO("BF bench: a new level: the free camera takes off again there");
    }
    return;
  }
  if (hidden) return;
  if (!joined.time_since_epoch().count()) joined = now;
  const uint32_t camera = Load32(base, g_client + 0xA4);
  if (!camera) return;
  if (!on) {
    if (std::chrono::duration<double>(now - joined).count() < cfg.at) return;
    const std::vector<CapturePoint> cps = CapturePoints();
    if (cps.empty()) return;  // not read yet
    for (const CapturePoint& c : cps) points.push_back({c.x, c.y, c.z}), middle = middle + V3{c.x, c.y, c.z};
    middle = middle * (1.f / points.size());
    top = middle.y;
    for (const V3& p : points) {
      radius = std::max(radius, std::hypot(p.x - middle.x, p.z - middle.z));
      top = std::max(top, p.y);
    }
    if (cfg.path == "props") {
      // The explosive props the server listed ($BF_BENCH_DIR/props.txt, bench_explode.cpp), in groups (each within
      // 40 m of its group's first), the groups in their order around the middle, 20 m above each.
      const char* dir = std::getenv("BF_BENCH_DIR");
      FILE* f = dir && *dir ? std::fopen((std::string(dir) + "/props.txt").c_str(), "r") : nullptr;
      if (!f) return;  // not written yet
      std::vector<std::pair<V3, int>> groups;  // (sum of places, count)
      V3 p;
      char line[256];
      while (std::fgets(line, sizeof(line), f)) {  // x y z, then the prop's name and class (words of their own)
        if (std::sscanf(line, "%f %f %f", &p.x, &p.y, &p.z) != 3) continue;
        auto it = std::find_if(groups.begin(), groups.end(), [&](const std::pair<V3, int>& g) {
          const V3 c = g.first * (1.f / g.second);
          return std::hypot(c.x - p.x, c.z - p.z) < 40.f;
        });
        if (it == groups.end()) groups.push_back({p, 1});
        else it->first = it->first + p, ++it->second;
      }
      std::fclose(f);
      std::vector<V3> loop;
      for (const auto& [sum, n] : groups) loop.push_back(sum * (1.f / n));
      std::sort(loop.begin(), loop.end(), [&](V3 a, V3 b) {
        return std::atan2(a.z - middle.z, a.x - middle.x) < std::atan2(b.z - middle.z, b.x - middle.x);
      });
      for (V3& q : loop) q.y += 20;
      if (loop.size() >= 3) spline.Build(loop);
      REXLOG_INFO("BF bench: the props path through {} groups of props", loop.size());
    } else {  // the low loop: the points in their order around the middle, 25 m above each
      std::vector<V3> loop = points;
      std::sort(loop.begin(), loop.end(), [&](V3 a, V3 b) {
        return std::atan2(a.z - middle.z, a.x - middle.x) < std::atan2(b.z - middle.z, b.x - middle.x);
      });
      for (V3& p : loop) p.y += 25;
      if (loop.size() >= 3) spline.Build(loop);
    }
    // The handedness of the game's matrices, from the soldier's (right +0x3A0, up +0x3B0, forward +0x3C0).
    const uint32_t manager = Load32(base, 0x82AF3AE4);  // as map_players.cpp
    const uint32_t local = manager ? Load32(base, manager + 0xB8) : 0;
    const uint32_t soldier = local ? Load32(base, local + 0x138) : 0;
    if (soldier && Load32(base, soldier) == 0x82007518) {
      const V3 r = LoadV(base, soldier + 0x3A0), u = LoadV(base, soldier + 0x3B0), f = LoadV(base, soldier + 0x3C0);
      handed = Dot(Cross(u, f), r) >= 0 ? 1.f : -1.f;
    }
    PPCContext call = ctx;
    call.r3.u64 = g_client;
    call.r4.u64 = 1;
    sub_82254B28(call, base);
    view_ready = PrepareView();
    // No HUD over the free camera's view (the soldier's, left on the carrier, is not what is shown).
    rex::cvar::SetFlagByName("native_x_renderer_hide_hud", "true");
    on = true;
    flying = now;
    // "a lap of N s": tools/bench measures one lap from this line (--duration lap).
    REXLOG_INFO("BF bench: free camera on, path {}: {} capture points, middle {:.0f} {:.0f} {:.0f}, radius {:.0f} m, "
                "highest {:.0f}, the low loop {:.0f} m, a lap of {:.1f} s; matrices {}-handed",
                cfg.path, points.size(), middle.x, middle.y, middle.z, radius, top, spline.Length(), LapSeconds(),
                handed > 0 ? "right = up x forward" : "right = forward x up");
  }
  static Clock::time_point view_tried{};
  if (!view_ready && now - view_tried >= std::chrono::milliseconds(500)) view_tried = now, view_ready = PrepareView();
  const double t = std::chrono::duration<double>(now - flying).count();
  V3 place, look;
  Pose(t, &place, &look);
  ExplodeInView(place, look);
  // The camera looks along minus its matrix's forward row (seen in the orbit's first run: the row toward the island
  // showed the sea).
  const V3 forward = Norm(place - look);
  V3 right = Norm(Cross(V3{0, 1, 0}, forward) * handed);
  const V3 up = Norm(Cross(forward, right) * handed);
  StoreRow(base, camera + 0x10, right, 0);
  StoreRow(base, camera + 0x20, up, 0);
  StoreRow(base, camera + 0x30, forward, 0);
  StoreRow(base, camera + 0x40, place, 1);
  static Clock::time_point logged{};
  if (std::chrono::duration<double>(now - logged).count() >= 5.0) {
    logged = now;
    REXLOG_INFO("BF bench: free camera at {:.0f} {:.0f} {:.0f} looking {:.2f} {:.2f} {:.2f} ({:.0f} s)", place.x, place.y,
                place.z, -forward.x, -forward.y, -forward.z, t);
  }
}
}  // namespace bf1943

#endif  // BF_DEV_TOOLS
