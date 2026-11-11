// Developer aid (BF_DEV_TOOLS builds only, Windows): finds the transforms near the local camera in guest memory and
// follows them frame by frame, to find which objects (a vehicle's body, the seated soldier, the camera's own) move
// only in the game's 30 Hz tick, and where they are kept (for smoothing them between ticks).
//
//   BF_DEV_MATSCAN=<seconds>[,<seconds>...]   scan at that many seconds after the first in match frame
//   BF_DEV_MATSCAN_R=<metres>                 how far from the camera (default 8)
//   BF_DEV_MATSCAN_FRAMES=<n>                 frames to follow the finds (default 90)
//
// A transform is Frostbite's LinearTransform: four rows of three floats, each row 16 bytes (right, up, forward, then
// the position), big endian. Taken as one: the first three rows of unit length and at right angles, the position
// within the radius. Each find is logged with its address, then once the frames are followed: how many frames its
// position changed in, how far it went, and its position on the first frames (the stepping shows there).
#if defined(BF_DEV_TOOLS) && defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

namespace bf1943 {
namespace dev {
void WatchHostWrites(std::vector<const void*> addresses, double seconds, const char* label);  // dev_watch.cpp
void WatchHostAccess(std::vector<const void*> addresses, double seconds, const char* label);  // reads and writes
}
extern std::atomic<float> g_dev_camera[3];  // look.cpp
extern std::atomic<bool> g_dev_camera_valid;
extern std::atomic<float> g_dev_view_pos[3];  // below: the rendered camera's position
bool InMatch();  // auto_join.cpp
bool DevInBoneArray(uint32_t address);  // tick_blend.cpp: in a skeleton's bone array
}  // namespace bf1943

namespace {

float BeFloat(const uint8_t* p) {
  const uint32_t v = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

struct Find {
  uint32_t address;
  float dist;
  float first[3], last[3];
  uint32_t changes = 0;
  double travelled = 0;
  std::string trail;
};

struct Plan {
  std::vector<double> at;
  float radius = 8.0f;
  uint32_t frames = 90;
  size_t next = 0;
  bool active = false;
  uint32_t followed = 0;
  std::vector<Find> finds;
  std::chrono::steady_clock::time_point start{};
};

Plan& GetPlan() {
  static Plan plan = [] {
    Plan p;
    if (const char* v = std::getenv("BF_DEV_MATSCAN"); v && *v) {
      std::string s = v;
      for (size_t at = 0; at < s.size();) {
        size_t comma = std::min(s.find(',', at), s.size());
        p.at.push_back(std::atof(s.substr(at, comma - at).c_str()));
        at = comma + 1;
      }
    }
    if (const char* v = std::getenv("BF_DEV_MATSCAN_R"); v && *v) p.radius = float(std::atof(v));
    if (const char* v = std::getenv("BF_DEV_MATSCAN_FRAMES"); v && *v) p.frames = uint32_t(std::atoi(v));
    return p;
  }();
  return plan;
}

bool IsTransform(const uint8_t* m) {
  float r[3][3];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r[i][j] = BeFloat(m + 16 * i + 4 * j);
  for (int i = 0; i < 3; ++i) {
    const float len = r[i][0] * r[i][0] + r[i][1] * r[i][1] + r[i][2] * r[i][2];
    if (!(len > 0.96f && len < 1.04f)) return false;
  }
  auto dot = [&](int a, int b) { return r[a][0] * r[b][0] + r[a][1] * r[b][1] + r[a][2] * r[b][2]; };
  return std::fabs(dot(0, 1)) < 0.03f && std::fabs(dot(0, 2)) < 0.03f && std::fabs(dot(1, 2)) < 0.03f;
}

void Scan(Plan& p, const uint8_t* base, const float c[3]) {
  const auto t0 = std::chrono::steady_clock::now();
  p.finds.clear();
  uint64_t bytes = 0;
  for (uint64_t a = 0; a < 0x100000000ull;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) break;
    const uint64_t region_start = uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base);
    const uint64_t region_end = region_start + mbi.RegionSize;
    const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                          (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                          PAGE_EXECUTE_READWRITE));
    if (readable && region_end > region_start + 64) {
      bytes += region_end - region_start;
      for (uint64_t at = (region_start + 15) & ~15ull; at + 64 <= region_end && at + 64 <= 0x100000000ull; at += 16) {
        const uint8_t* m = base + at;
        const float x = BeFloat(m + 48), y = BeFloat(m + 52), z = BeFloat(m + 56);
        const float dx = x - c[0], dy = y - c[1], dz = z - c[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (!(d2 < p.radius * p.radius)) continue;
        if (!IsTransform(m)) continue;
        if (bf1943::DevInBoneArray(uint32_t(at))) continue;  // a skeleton's bone: those are looked after (tick_blend.cpp)
        Find f{};
        f.address = uint32_t(at);
        f.dist = std::sqrt(d2);
        f.first[0] = f.last[0] = x, f.first[1] = f.last[1] = y, f.first[2] = f.last[2] = z;
        p.finds.push_back(f);
      }
    }
    a = std::max(region_end, a + 4096);
  }
  std::sort(p.finds.begin(), p.finds.end(), [](const Find& a, const Find& b) { return a.dist < b.dist; });
  if (p.finds.size() > 4096) p.finds.resize(4096);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  REXLOG_INFO("BF matscan: {} transforms within {} m of the camera ({:.2f} {:.2f} {:.2f}) in {} MB of guest memory, "
              "{:.0f} ms",
              p.finds.size(), p.radius, c[0], c[1], c[2], bytes >> 20, ms);
}

}  // namespace

namespace bf1943 {

std::atomic<uint32_t> g_dev_view{0};  // fov.cpp: the view the camera manager renders (its setFovY's view)
std::atomic<uint32_t> g_dev_view_calls{0};  // its setFovY calls since the last frame

// BF_DEV_VIEWLOG=1: each frame, the transforms in the rendered view (the camera manager's RenderViewDesc, the first
// 0x200 bytes): their offsets once, then the first one's position and heading every frame. The camera as drawn, for
// the vehicle judder (the seated soldier's own camera transform moves every frame in a tank: run seek4).
bool ViewLogOn() {
  static const bool on = [] {
    const char* v = std::getenv("BF_DEV_VIEWLOG");
    return v && *v == '1';
  }();
  return on;
}

// From fov.cpp's setFovY hook (the camera manager's copy of the renderer's view, sub_82294A58): the view's camera
// transform (+0x40, run view1) at the moment it is handed back to the renderer.
std::atomic<float> g_dev_view_pos[3];  // the rendered camera's position, every frame (the vehicle seeker)

void DevViewSample(uint8_t* base, uint32_t view) {
  {
    const uint8_t* a = base + view + 0x40;
    if (IsTransform(a)) {
      g_dev_view_pos[0].store(BeFloat(a + 48), std::memory_order_relaxed);
      g_dev_view_pos[1].store(BeFloat(a + 52), std::memory_order_relaxed);
      g_dev_view_pos[2].store(BeFloat(a + 56), std::memory_order_relaxed);
    }
  }
  // BF_DEV_VIEWFIND=<sample>: at that sample in a match, every place in guest memory that holds the view's camera
  // transform (+0x40, 48 bytes, exactly) outside the copy's own stack page, then a write watch on the first two (who
  // sets the camera each frame).
  static const long find_at = [] {
    const char* v = std::getenv("BF_DEV_VIEWFIND");
    return v && *v ? std::atol(v) : -1L;
  }();
  static long found_samples = 0;
  if (find_at >= 0 && InMatch() && ++found_samples == find_at) {
    const uint8_t* want = base + view + 0x40;
    std::vector<uint32_t> hits;
    for (uint64_t a = 0; a < 0x100000000ull && hits.size() < 16;) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) break;
      const uint64_t start = uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base);
      const uint64_t end = start + mbi.RegionSize;
      const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                            PAGE_EXECUTE_READWRITE));
      if (readable) {
        for (uint64_t at = (start + 15) & ~15ull; at + 48 <= end && hits.size() < 16; at += 16) {
          if ((uint32_t(at) >> 12) == ((view + 0x40) >> 12)) continue;  // the copy's own page
          if (!std::memcmp(base + at, want, 48)) hits.push_back(uint32_t(at));
        }
      }
      a = std::max(end, a + 4096);
    }
    std::string list;
    for (uint32_t h : hits) list += fmt::format(" {:08X}", h);
    REXLOG_INFO("BF view find: the camera transform of view copy {:08X} is also at{}", view, list.empty() ? " none" : list);
    std::vector<const void*> watch;
    for (size_t i = 0; i < hits.size() && watch.size() < 4; ++i) watch.push_back(base + hits[i] + 48);
    if (!watch.empty()) dev::WatchHostWrites(watch, 3.0, "view");
  }
  if (!ViewLogOn() || !InMatch()) return;
  static uint64_t samples = 0;
  ++samples;
  const uint8_t* a = base + view + 0x40;
  const uint8_t* b = base + view + 0x80;
  REXLOG_INFO("BF view: sample {} at {} A {} pos {:.4f} {:.4f} {:.4f} fwd {:.4f} {:.4f} B {} pos {:.4f} {:.4f} {:.4f}",
              samples,
              std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now().time_since_epoch()).count(),
              IsTransform(a), BeFloat(a + 48), BeFloat(a + 52), BeFloat(a + 56), BeFloat(a + 32), BeFloat(a + 40),
              IsTransform(b), BeFloat(b + 48), BeFloat(b + 52), BeFloat(b + 56));
}

void DevViewLogFrame(uint8_t*) {}

// Once a frame (VdSwap, frame_stats.cpp).
void DevMatScanFrame() {
  {
    uint8_t* b = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(0);
    DevViewLogFrame(b);
  }
  Plan& p = GetPlan();
  if (p.at.empty()) return;
  uint8_t* base = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(0);
  const auto now = std::chrono::steady_clock::now();
  if (!p.start.time_since_epoch().count()) {
    if (!InMatch()) return;
    p.start = now;
  }
  if (p.active) {
    ++p.followed;
    for (Find& f : p.finds) {
      const uint8_t* m = base + f.address;
      const float x = BeFloat(m + 48), y = BeFloat(m + 52), z = BeFloat(m + 56);
      const double step = std::sqrt(double(x - f.last[0]) * (x - f.last[0]) + double(y - f.last[1]) * (y - f.last[1]) +
                                    double(z - f.last[2]) * (z - f.last[2]));
      if (step > 1e-5) ++f.changes;
      f.travelled += step;
      if (p.followed <= 12) f.trail += fmt::format(" {:.3f}", step);
      f.last[0] = x, f.last[1] = y, f.last[2] = z;
    }
    if (p.followed >= p.frames) {
      const float c[3] = {g_dev_view_pos[0].load(), g_dev_view_pos[1].load(), g_dev_view_pos[2].load()};
      REXLOG_INFO("BF matscan: followed {} frames; the camera is now at {:.2f} {:.2f} {:.2f}", p.followed, c[0], c[1],
                  c[2]);
      for (const Find& f : p.finds) {
        if (!f.changes) continue;
        REXLOG_INFO("BF matscan: {:08X} at {:.2f} m: moved in {} of {} frames, {:.3f} m; first steps{}", f.address,
                    f.dist, f.changes, p.followed, f.travelled, f.trail);
      }
      size_t still = 0;
      for (const Find& f : p.finds) still += f.changes == 0;
      REXLOG_INFO("BF matscan: {} of {} did not move", still, p.finds.size());
      // BF_DEV_MATSCAN_WATCH=1: a hardware write watch (dev_watch.cpp) on the positions of up to four transforms that
      // moved in the tick's rhythm (in 35 to 60% of the frames, over half a metre), one per physical page alias: who
      // writes them, for 3 s.
      if (const char* w = std::getenv("BF_DEV_MATSCAN_WATCH"); w && *w == '1') {
        std::vector<const void*> watch;
        std::vector<uint32_t> seen;
        // BF_DEV_MATSCAN_WATCH_FRAME=1: those that change on every frame instead (a camera smoothed each frame).
        const char* every = std::getenv("BF_DEV_MATSCAN_WATCH_FRAME");
        const bool every_frame = every && *every == '1';
        for (const Find& f : p.finds) {
          const double share = double(f.changes) / p.followed;
          if (every_frame ? share < 0.95 : (share < 0.25 || share > 0.6)) continue;
          if (f.travelled < 0.5 || f.travelled > 100) continue;
          const uint32_t phys = f.address & 0x1FFFFFFF;
          if (std::find(seen.begin(), seen.end(), phys) != seen.end()) continue;
          seen.push_back(phys);
          watch.push_back(base + f.address + 48);
          REXLOG_INFO("BF matscan: watching writes to {:08X} (+48)", f.address);
          if (watch.size() == 4) break;
        }
        // BF_DEV_MATSCAN_WATCH_READS=1: reads and writes instead, of four that are not at the camera (over 0.12 m away:
        // the vehicle's parts, not the seated soldier's own matrices).
        const char* r = std::getenv("BF_DEV_MATSCAN_WATCH_READS");
        if (r && *r == '1') {
          std::vector<const void*> away;
          std::vector<uint32_t> away_seen;
          // BF_DEV_MATSCAN_WATCH_MIN=<metres> (default 0.12): only those at least that far from the camera. The watch is
          // on the 16 MB page view (0xC0000000 up) of a physical find, the one the game's pointers use; a debug register
          // on another view of the same page sees nothing (run vscan1).
          static const double min_dist = [] {
            const char* v = std::getenv("BF_DEV_MATSCAN_WATCH_MIN");
            return v && *v ? std::atof(v) : 0.12;
          }();
          for (const Find& f : p.finds) {
            const double share = double(f.changes) / p.followed;
            if (f.dist < min_dist || share < 0.25 || share > 0.6 || f.travelled < 0.5 || f.travelled > 100) continue;
            const uint32_t phys = f.address & 0x1FFFFFFF;
            if (std::find(away_seen.begin(), away_seen.end(), phys) != away_seen.end()) continue;
            away_seen.push_back(phys);
            const uint32_t at = f.address >= 0xA0000000u ? 0xC0000000u | phys : f.address;
            away.push_back(base + at + 48);
            REXLOG_INFO("BF matscan: watching reads and writes of {:08X} (+48) at {:.2f} m", at, f.dist);
            if (away.size() == 4) break;
          }
          if (!away.empty()) dev::WatchHostAccess(away, 3.0, "matscan reads");
        }
        else if (!watch.empty()) dev::WatchHostWrites(watch, 3.0, "matscan");
      }
      p.active = false;
    }
    return;
  }
  if (p.next >= p.at.size()) return;
  if (std::chrono::duration<double>(now - p.start).count() < p.at[p.next]) return;
  ++p.next;
  const float c[3] = {g_dev_view_pos[0].load(), g_dev_view_pos[1].load(), g_dev_view_pos[2].load()};
  Scan(p, base, c);
  p.active = !p.finds.empty();
  p.followed = 0;
}

}  // namespace bf1943

#else
namespace bf1943 {
void DevMatScanFrame() {}
}  // namespace bf1943
#if defined(BF_DEV_TOOLS)
// Developer builds off Windows: only the rendered camera's position (fov.cpp's view sample, for the vehicle seeker);
// the view logs, finds and watches above are Windows only.
#include <atomic>
#include <cstdint>
#include <cstring>
namespace bf1943 {
std::atomic<uint32_t> g_dev_view{0};
std::atomic<uint32_t> g_dev_view_calls{0};
std::atomic<float> g_dev_view_pos[3];
void DevViewSample(uint8_t* base, uint32_t view) {
  auto be_float = [base](uint32_t at) {
    const uint32_t u = uint32_t(base[at]) << 24 | uint32_t(base[at + 1]) << 16 | uint32_t(base[at + 2]) << 8 | base[at + 3];
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  };
  for (uint32_t i = 0; i < 3; ++i) g_dev_view_pos[i].store(be_float(view + 0x40 + 48 + 4 * i), std::memory_order_relaxed);
}
}  // namespace bf1943
#endif
#endif
