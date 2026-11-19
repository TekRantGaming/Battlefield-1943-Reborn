// Developer aid (BF_DEV_TOOLS builds only): writes ranges of guest physical memory to files at a set time, for
// looking at what the GPU wrote there (resolve targets, with --readback_resolve=full).
//
//   BF_DUMP_PHYS=<seconds>:<hex address>:<hex size>[,<hex address>:<hex size>...]   BF_DUMP_OUT=<folder>
//
// Seconds count from the first frame of the game's UI update; each range goes to <folder>\phys_<address>.bin.

#if defined(BF_DEV_TOOLS)

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <mutex>
#include <vector>

#include <atomic>
#include <cmath>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // BF_DUMP_VIRT: VirtualQuery
#endif

#include <native_x_renderer/native_x_renderer.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

namespace {

struct Range {
  uint32_t address, size;
};

struct Plan {
  double at = -1;
  std::vector<Range> ranges;
  std::string out;
  bool done = false;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

Plan& GetPlan() {
  static Plan plan = [] {
    Plan p;
    const char* v = std::getenv("BF_DUMP_PHYS");
    const char* o = std::getenv("BF_DUMP_OUT");
    if (!v || !*v || !o || !*o) return p;
    p.out = o;
    std::string s = v;
    size_t colon = s.find(':');
    if (colon == std::string::npos) return p;
    p.at = std::atof(s.substr(0, colon).c_str());
    s = s.substr(colon + 1);
    for (size_t at = 0; at < s.size();) {
      size_t comma = s.find(',', at);
      if (comma == std::string::npos) comma = s.size();
      const std::string item = s.substr(at, comma - at);
      const size_t c = item.find(':');
      if (c != std::string::npos)
        p.ranges.push_back({uint32_t(std::stoul(item.substr(0, c), nullptr, 16)),
                            uint32_t(std::stoul(item.substr(c + 1), nullptr, 16))});
      at = comma + 1;
    }
    return p;
  }();
  return plan;
}

}  // namespace

namespace bf1943 {
extern std::atomic<float> g_sea_level;  // shader_prep.cpp: the level's water height (NaN: not known yet)
extern std::atomic<float> g_dev_camera[3];  // look.cpp: the local camera's position
uint32_t LocalSoldierEntity();              // look.cpp
namespace dev {
void WatchHostWrites(std::vector<const void*> addresses, double seconds, const char* label);  // dev_watch.cpp
}
}  // namespace bf1943

namespace {
// BF_DEV_SHORE_SCAN=<scan>[;<scan>...] (tests: where a swimmer gets out of the sea), each scan
//   <x>,<z>[,<radius>[,<step>[,<lines>]]]: the ground along <lines> straight lines out from x, z (16, from +x toward
//     +z), every <step> m (2) to <radius> m (60);
//   grid,<x0>,<z0>,<x1>,<z1>,<step>: a grid of the ground, one line per row of z;
// from the grass's height probe (grass must be on), each line logged as heights over the water.
struct ShoreLine {
  std::string label;
  std::vector<std::pair<float, float>> points;
};
struct ShoreScan {
  bool parsed = false, on = false, done = false, asked = false;
  std::vector<ShoreLine> lines;
  size_t line = 0, sample = 0;
  uint32_t probe = 5000000;  // the probe numbers (a float in the shader: under 2^24; the tour counts from 1)
  std::string text;
} g_shore;


// BF_DEV_ENTITY_TRACE=<file>[,<seconds>[,<hex size>]]: every <seconds> (0.25) the local soldier entity's first <size>
// bytes (0x1000) appended to <file> with the time and the camera's place: records of "ENTR", float seconds, the
// camera's x, y, z, the entity's address and size (little endian), then the bytes as the guest has them.
void EntityTraceFrame() {
  static FILE* f = nullptr;
  static double every = 0.25;
  static uint32_t size = 0x1000;
  static const bool on = [] {
    const char* v = std::getenv("BF_DEV_ENTITY_TRACE");
    if (!v || !*v) return false;
    std::string s = v;
    const size_t c = s.find(',');
    if (c != std::string::npos) {
      unsigned sz = 0;
      if (std::sscanf(s.c_str() + c + 1, "%lf,%x", &every, &sz) == 2 && sz) size = sz;
      s.resize(c);
    }
    f = std::fopen(s.c_str(), "wb");
    return f != nullptr;
  }();
  if (!on) return;
  static const auto start = std::chrono::steady_clock::now();
  static double next = 0;
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (t < next) return;
  next = t + every;
  const uint32_t entity = bf1943::LocalSoldierEntity();
  if (!entity) return;
  const float head[5] = {float(t), bf1943::g_dev_camera[0].load(), bf1943::g_dev_camera[1].load(),
                         bf1943::g_dev_camera[2].load(), 0.0f};
  std::fwrite("ENTR", 1, 4, f);
  std::fwrite(head, 4, 4, f);
  std::fwrite(&entity, 4, 1, f);
  std::fwrite(&size, 4, 1, f);
  std::fwrite(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(entity), 1, size, f);
  std::fflush(f);
}

// BF_DEV_STATE_LOG=1: the local soldier's character state (entity+0x3E4: 1 on the ground, 3 in the air, 6 swimming;
// +0x3E8 the one before, run swim_a1) logged at each change with the camera's place. BF_DEV_STATE_WATCH=1 also
// watches the writes to both for 20 s from the first swim: the game's code that changes them.
void StateLogFrame() {
  static const bool on = std::getenv("BF_DEV_STATE_LOG") != nullptr, watch = std::getenv("BF_DEV_STATE_WATCH") != nullptr;
  if (!on && !watch) return;
  static uint32_t last_entity = 0, last_state = ~0u;
  static bool watched = false;
  const uint32_t entity = bf1943::LocalSoldierEntity();
  if (!entity) return;
  uint8_t* p = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(entity);
  const uint32_t state = uint32_t(p[0x3E4]) << 24 | uint32_t(p[0x3E5]) << 16 | uint32_t(p[0x3E6]) << 8 | p[0x3E7];
  if (entity != last_entity || state != last_state) {
    REXLOG_INFO("BF character state: {} (was {}) at {:.2f} {:.2f} {:.2f}, entity {:08X}", state, last_state,
                bf1943::g_dev_camera[0].load(), bf1943::g_dev_camera[1].load(), bf1943::g_dev_camera[2].load(), entity);
    last_entity = entity, last_state = state;
    if (watch && !watched && state == 6) {
      watched = true;
      bf1943::dev::WatchHostWrites({p + 0x3E4, p + 0x3E8}, 20.0, "state");
    }
  }
}

void ShoreScanFrame() {
  ShoreScan& s = g_shore;
  if (!s.parsed) {
    s.parsed = true;
    const char* v = std::getenv("BF_DEV_SHORE_SCAN");
    for (const char* c = v; c && *c;) {
      float x0 = 0, z0 = 0, x1 = 0, z1 = 0, r = 60, st = 2;
      int n = 16;
      if (std::sscanf(c, "grid,%f,%f,%f,%f,%f", &x0, &z0, &x1, &z1, &st) == 5 && st > 0.1f) {
        for (float z = z0; z <= z1 + 1e-3f; z += st) {
          ShoreLine l;
          l.label = fmt::format("row z {:.1f} from x {:.1f} every {:.1f} m", z, x0, st);
          for (float x = x0; x <= x1 + 1e-3f; x += st) l.points.push_back({x, z});
          s.lines.push_back(std::move(l));
        }
      } else if (std::sscanf(c, "%f,%f,%f,%f,%d", &x0, &z0, &r, &st, &n) >= 2) {
        st = std::max(0.25f, st), n = std::max(1, n);
        for (int k = 0; k < n; ++k) {
          const double a = 6.283185307179586 * k / n;
          ShoreLine l;
          l.label = fmt::format("from {:.1f} {:.1f} toward {:.2f} {:.2f} (x, z) every {:.1f} m", x0, z0, std::cos(a),
                                std::sin(a), st);
          for (int i = 0; i * st <= r + 1e-3f; ++i)
            l.points.push_back({float(x0 + std::cos(a) * i * st), float(z0 + std::sin(a) * i * st)});
          s.lines.push_back(std::move(l));
        }
      }
      c = std::strchr(c, ';');
      if (c) ++c;
    }
    s.on = !s.lines.empty();
  }
  if (!s.on || s.done) return;
  const float sea = bf1943::g_sea_level.load();
  if (std::isnan(sea)) return;
  if (s.asked) {
    uint32_t id = 0;
    float ground = 0;
    if (!native_x_renderer::GrassProbeResult(&id, &ground) || id != s.probe) return;
    s.asked = false;
    s.text += fmt::format(" {:+.1f}", ground - sea);
    if (++s.sample >= s.lines[s.line].points.size()) {
      REXLOG_INFO("BF shore scan: {}, over the water {:.1f}:{}", s.lines[s.line].label, sea, s.text);
      s.text.clear(), s.sample = 0;
      if (++s.line >= s.lines.size()) {
        s.done = true;
        REXLOG_INFO("BF shore scan: done");
        return;
      }
    }
  }
  const auto& p = s.lines[s.line].points[s.sample];
  native_x_renderer::GrassProbe(p.first, p.second, ++s.probe);
  s.asked = true;
}
}  // namespace

namespace bf1943 {
namespace dev {
void WatchHostWrites(std::vector<const void*> addresses, double seconds, const char* label);  // dev_watch.cpp
void WatchHostAccess(std::vector<const void*> addresses, double seconds, const char* label);  // reads and writes
}

// Once a frame, from VdSwap (frame_stats.cpp): the UI's update does not run in every mode (NativeXRenderer).
void DevMemDumpFrame() {
  ShoreScanFrame();
  EntityTraceFrame();
  StateLogFrame();
  // BF_WATCH_PHYS=<hex physical address>:<start seconds>:<seconds>: a hardware watch on writes to that address through
  // its physical view and its 0xA0000000, 0xC0000000 and 0xE0000000 guest views (who writes texture data unseen).
  static const auto start = std::chrono::steady_clock::now();
  static bool watched = false;
  struct Watch {
    uint32_t address = 0;
    double at = -1, seconds = 0;
  };
  static const Watch watch = [] {
    Watch w;
    if (const char* v = std::getenv("BF_WATCH_PHYS"); v && *v) {
      unsigned address = 0;
      double at = 0, seconds = 0;
      if (std::sscanf(v, "%x:%lf:%lf", &address, &at, &seconds) == 3) w = {uint32_t(address), at, seconds};
    }
    return w;
  }();
  if (!watched && watch.at >= 0 &&
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= watch.at) {
    watched = true;
    auto* memory = REX_KERNEL_MEMORY();
    const uint32_t a = watch.address & 0x1FFFFFFF;
    std::vector<const void*> addresses = {memory->TranslatePhysical<const void*>(a),
                                          memory->TranslateVirtual<const void*>(0xA0000000u | a),
                                          memory->TranslateVirtual<const void*>(0xC0000000u | a),
                                          memory->TranslateVirtual<const void*>(0xE0000000u | a)};
    REXLOG_INFO("BF watch phys {:08X}: host {} {} {} {}", a, addresses[0], addresses[1], addresses[2], addresses[3]);
    dev::WatchHostWrites(addresses, watch.seconds, "phys");
  }
  // BF_WATCH_VIRT=<hex guest address>[+<hex address>...]:<start seconds>:<seconds>: a hardware watch on reads and
  // writes of up to four guest virtual addresses (who reads the gun's spring data, 19 Nov 2026).
  {
    static bool virt_watched = false;
    static const char* vw = std::getenv("BF_WATCH_VIRT");
    if (!virt_watched && vw && *vw) {
      std::string s = vw;
      const size_t c1 = s.find(':'), c2 = c1 == std::string::npos ? c1 : s.find(':', c1 + 1);
      if (c2 != std::string::npos &&
          std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >=
              std::atof(s.substr(c1 + 1, c2 - c1 - 1).c_str())) {
        virt_watched = true;
        std::vector<const void*> addresses;
        std::string list = s.substr(0, c1);
        for (size_t at = 0; at < list.size() && addresses.size() < 4;) {
          size_t plus = list.find('+', at);
          if (plus == std::string::npos) plus = list.size();
          const uint32_t a = uint32_t(std::stoul(list.substr(at, plus - at), nullptr, 16));
          addresses.push_back(REX_KERNEL_MEMORY()->TranslateVirtual<const void*>(a));
          REXLOG_INFO("BF watch virt {:08X}", a);
          at = plus + 1;
        }
        dev::WatchHostAccess(addresses, std::atof(s.substr(c2 + 1).c_str()), "virt");
      }
    }
  }
  // BF_DUMP_IMAGE=<file>: the game's image (guest 82000000 to 82DB0000) written once, at the first frame: the
  // decrypted image tools/guest_at.py and string_refs.py read (input/image.bin).
  static bool image_done = false;
  if (!image_done) {
    image_done = true;
    if (const char* v = std::getenv("BF_DUMP_IMAGE"); v && *v) {
      if (FILE* f = std::fopen(v, "wb")) {
        std::fwrite(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(0x82000000u), 1, 0xDB0000, f);
        std::fclose(f);
        REXLOG_INFO("BF dump: the image -> {}", v);
      }
    }
  }
  // BF_DUMP_VIRT=<seconds>:<hex from>:<hex to> (with BF_DUMP_OUT): the guest's virtual pages in that range that are
  // in use, once, to <folder>\virt.bin as records of a big endian page address and its 4096 bytes (the data objects
  // and the game's heaps live there, not in the physical dump; the gun's springs, 19 Nov 2026).
#if defined(_WIN32)
  {
    static bool virt_done = false;
    static const auto virt_start = std::chrono::steady_clock::now();
    const char* v = std::getenv("BF_DUMP_VIRT");
    const char* o = std::getenv("BF_DUMP_OUT");
    if (!virt_done && v && *v && o && *o) {
      unsigned long from = 0, to = 0;
      double at = 0;
      if (std::sscanf(v, "%lf:%lx:%lx", &at, &from, &to) == 3 &&
          std::chrono::duration<double>(std::chrono::steady_clock::now() - virt_start).count() >= at) {
        virt_done = true;
        const std::string path = std::string(o) + "\\virt.bin";
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
          uint32_t pages = 0;
          for (uint64_t a = from & ~0xFFFull; a < to; a += 0x1000) {
            const uint8_t* host = REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(uint32_t(a));
            MEMORY_BASIC_INFORMATION mi{};
            if (!VirtualQuery(host, &mi, sizeof(mi)) || mi.State != MEM_COMMIT ||
                (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                                                                              PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
              continue;
            const uint8_t be[4] = {uint8_t(a >> 24), uint8_t(a >> 16), uint8_t(a >> 8), uint8_t(a)};
            std::fwrite(be, 1, 4, f);
            std::fwrite(host, 1, 0x1000, f);
            ++pages;
          }
          std::fclose(f);
          REXLOG_INFO("BF dump: virtual {:08X} to {:08X}, {} pages in use -> {}", from, to, pages, path);
        }
      }
    }
  }
#endif
  Plan& plan = GetPlan();
  if (plan.at < 0 || plan.done) return;
  if (std::chrono::duration<double>(std::chrono::steady_clock::now() - plan.start).count() < plan.at) return;
  plan.done = true;
  for (const Range& r : plan.ranges) {
    const std::string path = plan.out + "\\phys_" + fmt::format("{:08X}", r.address) + ".bin";
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(REX_KERNEL_MEMORY()->TranslatePhysical<uint8_t*>(r.address), 1, r.size, f);
      std::fclose(f);
      REXLOG_INFO("BF dump: {:08X} size {:X} -> {}", r.address, r.size, path);
    }
  }
}
}  // namespace bf1943

namespace {
float GuestF(const uint8_t* base, uint32_t a) {
  const uint32_t u = uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
}  // namespace

// BF_DEV_SWIM_LOG=<n>: the swimming state's update (sub_82518F68(state r3, physics r4, step r5, velocity out r6)): every
// n-th call, the step's time (+104), the water's height (+180), the step's velocity in (+0) and the velocity it gives.
REX_EXTERN(__imp__sub_82518F68);
REX_HOOK_RAW(sub_82518F68) {
  static const long every = [] {
    const char* v = std::getenv("BF_DEV_SWIM_LOG");
    return v && *v ? std::max(1L, std::atol(v)) : 0L;
  }();
  if (!every) return __imp__sub_82518F68(ctx, base);
  const uint32_t step = ctx.r5.u32, out = ctx.r6.u32;
  const float dt = GuestF(base, step + 104), water = GuestF(base, step + 180);
  const float in[3] = {GuestF(base, step), GuestF(base, step + 4), GuestF(base, step + 8)};
  __imp__sub_82518F68(ctx, base);
  static std::atomic<long> calls{0};
  if (calls.fetch_add(1) % every) return;
  REXLOG_INFO("BF swim step: dt {:.5f}, water {:.2f}, in {:.3f} {:.3f} {:.3f}, out {:.3f} {:.3f} {:.3f}", dt, water, in[0], in[1],
              in[2], GuestF(base, out), GuestF(base, out + 4), GuestF(base, out + 8));
}

#endif  // BF_DEV_TOOLS
