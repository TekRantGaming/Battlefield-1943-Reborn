// Benchmark (developer builds, tools/bench): the level's explosive props (oil drums, propane tanks, gas cylinders)
// found on the server and blown up as a hit would, so a benchmark gets explosions where and when it wants them.
//
// BF_BENCH_EXPLODE on a server (the props found first, by a scan of 3 to 11 s on a thread of its own, 2 s into the
// first level and 10 s into each after a level change; tools/bench starts the game after "BF explode: scan done"):
//   view[,<damage>]           each prop the game's free camera asks for: the lines of props.txt it adds to
//                             $BF_BENCH_DIR/trigger.txt as they come into view (bench_freecam.cpp,
//                             BF_BENCH_EXPLODE_VIEW), read 10 times a second
//   <s>,<every s>[,<damage>]  one by one on a timer, from that many seconds after the scan
// The props and their places go to $BF_BENCH_DIR/props.txt, in the order the camera's trigger lines count them.
//
// How the game does it (Wake Island, a player shooting the props, 19 Nov 2026):
//   - The props: entities of classes 0x82015A68 (oil drums, propane tanks) and 0x82015978 / 0x8201591C (gas
//     cylinders), their data at +0x14, the data's name at +0x0C (Objects/Street_Objects/OildrumExplosive_t01v01_Br_Dy/
//     ..., Objects/Objects/PropaneTank/PropaneTank, Objects/Objects/Gascylinder_large/...). They live in physical
//     memory; the game's pointers to them are their 0xC0000000 view (a hit's this; another view's address damages
//     nothing).
//   - A hit: sub_822EB198(health, damage record, giver) on a health component (vtable 0x8200FD20; +72 its state, +92
//     its state thresholds, +96 the prop's interface it reports to, +100 the health) takes the record's damage (+56)
//     off the health, picks the state the health is in, tells the prop (its interface's vtable +4, sub_82330D90, which
//     sends the damage event), and when the state changes runs the prop's destruction: its explosion, fire, debris.
//   - The damage record: 76 bytes as sub_82330BB8 (the physics' collision damage) builds one: +0 the place, +16 the
//     direction, +56 the damage, +73 1. The giver as a collision's: sub_82388918(giver, 0, 0, 0, 0, -1, &"Death", 0, 0)
//     (no attacker; one made by hand crashed the server in the destruction).

#if defined(BF_DEV_TOOLS)

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(bool, server);

REX_EXTERN(sub_821C6328);  // eastl string = C text
REX_EXTERN(sub_82388918);  // a damage giver record
REX_EXTERN(sub_822EB198);  // a hit on a health component: (health, damage record, giver)
REX_EXTERN(sub_822A7BB0);  // an entity's transform (+48 its place)

namespace {

constexpr uint32_t kGameContext = 0x82AF56F0;  // as bots.cpp: +40 the server's player manager once a level runs
constexpr uint32_t kHealthVtable = 0x8200FD20;
constexpr uint32_t kPropVtables[] = {0x82015A68u, 0x82015978u, 0x8201591Cu};
constexpr uint32_t kDeathText = 0x82010E48;  // "Death"

uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
float LoadF(const uint8_t* base, uint32_t a) {
  const uint32_t v = Load32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void StoreF(uint8_t* base, uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}
bool InImage(uint32_t v) { return v >= 0x82000000u && v < 0x82100000u; }  // the image's vtables
uint32_t View(uint32_t a) { return (a & 0x1FFFFFFFu) | 0xC0000000u; }    // the physical view the game uses

// Every committed, readable guest range below 4 GB, less the image (0x80000000 to 0xA0000000).
template <typename Fn>
void ForEachGuestRange(const uint8_t* base, Fn&& fn) {
  auto give = [&](uint64_t s, uint64_t e) {
    s = std::max<uint64_t>(s, 0x10000);
    if (s < 0x80000000ull && e > s) fn(s, std::min<uint64_t>(e, 0x80000000ull));
    if (e > 0xA0000000ull) fn(std::max<uint64_t>(s, 0xA0000000ull), e);
  };
#if defined(_WIN32)
  for (uint64_t a = 0x10000; a < 0x100000000ull;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) break;
    const uint64_t start = std::max<uint64_t>(uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base), a);
    const uint64_t end = std::min<uint64_t>(uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base) + mbi.RegionSize,
                                            0x100000000ull);
    if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && end > start) give(start, end);
    if (end <= a) break;
    a = end;
  }
#else
  FILE* maps = std::fopen("/proc/self/maps", "r");
  if (!maps) return;
  const uint64_t lo = uint64_t(reinterpret_cast<uintptr_t>(base)), hi = lo + 0x100000000ull;
  std::vector<std::pair<uint64_t, uint64_t>> ranges;
  char line[512];
  while (std::fgets(line, sizeof(line), maps)) {
    unsigned long long s = 0, e = 0;
    char perms[8] = {};
    if (std::sscanf(line, "%llx-%llx %7s", &s, &e, perms) != 3 || perms[0] != 'r' || e <= lo || s >= hi) continue;
    ranges.push_back({std::max<uint64_t>(s, lo) - lo, std::min<uint64_t>(e, hi) - lo});
  }
  std::fclose(maps);
  for (const auto& [s, e] : ranges) give(s, e);
#endif
}

struct Target {
  uint32_t entity;  // its 0xC0000000 view
  std::string name;
  float at[3] = {};
  std::vector<uint32_t> healths;
  bool done = false;
};
std::vector<Target> g_targets;  // the server thread's only

// The level's: when it was found and scanned, the props blown up, and how far into trigger.txt the camera's requests
// were read (a new level's scan starts past the old level's). BenchExplodeLevelChange starts them again.
struct Level {
  std::chrono::steady_clock::time_point loaded{}, scanned{};
  bool after_change = false;  // a level change brought it: its data still streaming in for a while
  bool scan_started = false, scan_done = false;
  int exploded = 0;
  long trigger_read = 0;
};
Level g_level;

std::string BenchFile(const char* name) {
  const char* dir = std::getenv("BF_BENCH_DIR");
  return dir && *dir ? std::string(dir) + "/" + name : std::string();
}

bool PropName(const std::string& text) {
  std::string low = text;
  for (char& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (low.rfind("objects/", 0) != 0) return false;
  for (const char* skip : {"_havok", "_mesh", "_mask", "/textures/", "/weapons/", "_ladder"})
    if (low.find(skip) != std::string::npos) return false;
  for (const char* word : {"explosive", "propane", "gascylinder"})
    if (low.find(word) != std::string::npos) return true;
  return false;
}

// The level's props: their names, the data objects that hold the names, the entities that hold the data, their
// health components. Four passes over the guest's memory, reads only: on a thread of its own (StartScan), so the
// server's thread never waits for it (a scan of 3 to 11 s on it dropped players joining).
std::vector<Target> ScanMemory(const uint8_t* base, size_t* name_count, size_t* data_count, size_t* health_count) {
  std::vector<Target> targets;
  // 1: the names (a NUL ended run of printable characters on a word boundary).
  std::unordered_map<uint32_t, std::string> names;
  ForEachGuestRange(base, [&](uint64_t s, uint64_t e) {
    for (uint64_t a = s; a < e;) {
      if (base[a] != 'O') {  // every name wanted starts with "Objects/"
        ++a;
        continue;
      }
      uint64_t b = a;
      while (b < e && b - a < 200 && std::isprint(base[b])) ++b;
      if ((a & 3) == 0 && b < e && base[b] == 0 && b - a >= 12) {
        const std::string text(reinterpret_cast<const char*>(base + a), b - a);
        if (PropName(text)) names[uint32_t(a)] = text;
      }
      a = std::max(b, a + 1);
    }
  });
  // 2: the objects holding a name: the nearest word at or above the pointer to it that is a vtable in the image.
  std::unordered_map<uint32_t, std::string> data;  // data object -> its name
  ForEachGuestRange(base, [&](uint64_t s, uint64_t e) {
    for (uint64_t a = (s + 3) & ~3ull; a + 4 <= e; a += 4) {
      const auto it = names.find(Load32(base, uint32_t(a)));
      if (it == names.end()) continue;
      for (uint32_t back = 0; back <= 0x80 && a >= s + back; back += 4)
        if (InImage(Load32(base, uint32_t(a) - back))) {
          data[uint32_t(a) - back] = it->second.substr(it->second.rfind('/') + 1);
          break;
        }
    }
  });
  // 3: the entities: a prop class's vtable with one of those objects at +0x14, in the game's physical view.
  ForEachGuestRange(base, [&](uint64_t s, uint64_t e) {
    if (s >= 0xE0000000ull || e <= 0xC0000000ull) return;
    for (uint64_t a = (std::max<uint64_t>(s, 0xC0000000ull) + 3) & ~3ull; a + 0x18 <= std::min<uint64_t>(e, 0xE0000000ull);
         a += 4) {
      const uint32_t vt = Load32(base, uint32_t(a));
      if (std::find(std::begin(kPropVtables), std::end(kPropVtables), vt) == std::end(kPropVtables)) continue;
      const auto it = data.find(Load32(base, uint32_t(a) + 0x14));
      if (it == data.end()) continue;
      targets.push_back(Target{uint32_t(a), it->second});
    }
  });
  std::sort(targets.begin(), targets.end(), [](const Target& a, const Target& b) { return a.entity < b.entity; });
  // 4: the health components (in the heaps below the physical views), each to the prop that starts nearest at or below
  // the interface it reports to (props lie 0x160 bytes apart).
  size_t healths = 0;
  ForEachGuestRange(base, [&](uint64_t s, uint64_t e) {
    if (s >= 0xA0000000ull) return;
    for (uint64_t a = (s + 3) & ~3ull; a + 104 <= e; a += 4) {
      if (Load32(base, uint32_t(a)) != kHealthVtable) continue;
      const uint32_t owner = View(Load32(base, uint32_t(a) + 96));
      auto it = std::upper_bound(targets.begin(), targets.end(), owner,
                                 [](uint32_t v, const Target& t) { return v < t.entity; });
      if (it == targets.begin()) continue;
      --it;
      if (owner - it->entity < 0x400) it->healths.push_back(uint32_t(a)), ++healths;
    }
  });
  *name_count = names.size(), *data_count = data.size(), *health_count = healths;
  return targets;
}

// The scan's thread hands its props to the server's thread here; a level change meanwhile (the generation) drops them.
struct Pending {
  std::mutex mutex;
  bool ready = false;
  int generation = 0;
  std::vector<Target> targets;
  size_t names = 0, data = 0, healths = 0;
  double ms = 0;
};
Pending g_pending;
std::atomic<int> g_generation{0};

void StartScan(const uint8_t* base) {
  const int generation = g_generation.load();
  std::thread([base, generation] {
    const auto t0 = std::chrono::steady_clock::now();
    size_t names = 0, data = 0, healths = 0;
    std::vector<Target> targets = ScanMemory(base, &names, &data, &healths);
    std::lock_guard lock(g_pending.mutex);
    if (generation != g_generation.load()) return;  // the level changed while it ran
    g_pending.targets = std::move(targets), g_pending.names = names, g_pending.data = data, g_pending.healths = healths;
    g_pending.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g_pending.generation = generation, g_pending.ready = true;
  }).detach();
}

// On the server's thread, once the scan is done: each prop's place (sub_822A7BB0, the game's own call), those not placed
// in the level (no transform: 0,0,0 or garbage) left out, the list written for the camera. False until then.
bool FinishScan(PPCContext& ctx, uint8_t* base) {
  std::vector<Target> targets;
  size_t names, data, healths;
  double ms;
  {
    std::lock_guard lock(g_pending.mutex);
    if (!g_pending.ready || g_pending.generation != g_generation.load()) return false;
    targets.swap(g_pending.targets), names = g_pending.names, data = g_pending.data, healths = g_pending.healths;
    ms = g_pending.ms, g_pending.ready = false;
  }
  g_targets.clear();
  for (Target& t : targets) {
    PPCContext call = ctx;
    call.r3.u64 = t.entity;
    sub_822A7BB0(call, base);
    for (int i = 0; i < 3 && call.r3.u32; ++i) t.at[i] = LoadF(base, call.r3.u32 + 48 + 4 * i);
    if (std::isfinite(t.at[0]) && std::isfinite(t.at[2]) && std::fabs(t.at[0]) < 5000.f && std::fabs(t.at[2]) < 5000.f &&
        (t.at[0] != 0.f || t.at[1] != 0.f || t.at[2] != 0.f))
      g_targets.push_back(std::move(t));
  }
  if (const std::string path = BenchFile("props.txt"); !path.empty()) {
    if (FILE* f = std::fopen((path + ".tmp").c_str(), "w")) {
      for (const Target& t : g_targets) std::fprintf(f, "%.2f %.2f %.2f %s\n", t.at[0], t.at[1], t.at[2], t.name.c_str());
      std::fclose(f);
      std::error_code e;
      std::filesystem::rename(path + ".tmp", path, e);
    }
  }
  REXLOG_INFO("BF explode: scan done in {:.0f} ms (its own thread): {} names, {} data objects, {} props placed, {} health "
              "components", ms, names, data, g_targets.size(), healths);
  return true;
}

// A hit of `amount` on each of the prop's health components, as a bullet's.
void Explode(PPCContext& ctx, uint8_t* base, const Target& t, float amount) {
  static uint32_t giver = 0, damage = 0;
  if (!damage) damage = REX_KERNEL_MEMORY()->SystemHeapAlloc(0x80, 16);
  if (!giver) {  // made once, kept
    const uint32_t g = REX_KERNEL_MEMORY()->SystemHeapAlloc(0x40, 16), text = REX_KERNEL_MEMORY()->SystemHeapAlloc(0x20, 16);
    if (!g || !text) return;
    std::memset(base + g, 0, 0x40), std::memset(base + text, 0, 0x20);
    PPCContext c = ctx;
    c.r1.u32 = ctx.r1.u32 - 0x100;  // a frame of our own: the ninth argument goes on the stack
    c.r3.u64 = text, c.r4.u64 = kDeathText;
    sub_821C6328(c, base);
    c = ctx;
    c.r1.u32 = ctx.r1.u32 - 0x100;
    std::memset(base + c.r1.u32 + 0x50, 0, 8);  // the ninth argument (+0x54): 0
    c.r3.u64 = g, c.r4.u64 = 0, c.r5.u64 = 0, c.r6.u64 = 0, c.r7.u64 = 0, c.r8.u64 = 0xFFFFFFFFu, c.r9.u64 = text,
    c.r10.u64 = 0;
    sub_82388918(c, base);
    giver = g;
  }
  if (!damage) return;
  std::memset(base + damage, 0, 0x80);
  StoreF(base, damage + 0, t.at[0]), StoreF(base, damage + 4, t.at[1] + 0.5f), StoreF(base, damage + 8, t.at[2]);
  StoreF(base, damage + 12, 1.f), StoreF(base, damage + 20, -1.f), StoreF(base, damage + 56, amount);
  base[damage + 73] = 1;
  for (const uint32_t health : t.healths) {
    PPCContext call = ctx;
    call.r3.u64 = health, call.r4.u64 = damage, call.r5.u64 = giver;
    sub_822EB198(call, base);
  }
}

struct Config {
  enum class Mode { kOff, kTimer, kView } mode = Mode::kOff;
  double start = 0, every = 10, damage = 1000;
};
Config ReadConfig() {
  Config c;
  const char* v = std::getenv("BF_BENCH_EXPLODE");
  if (!v || !*v) return c;
  if (std::strncmp(v, "view", 4) == 0) {
    c.mode = Config::Mode::kView;
    std::sscanf(v, "view,%lf", &c.damage);
  } else if (std::sscanf(v, "%lf,%lf,%lf", &c.start, &c.every, &c.damage) >= 1) {
    c.mode = Config::Mode::kTimer;
  }
  return c;
}

void Tick(PPCContext& ctx, uint8_t* base, const Config& cfg, double since_scan) {
  int& exploded = g_level.exploded;
  auto boom = [&](Target& t, const char* why) {
    t.done = true;
    Explode(ctx, base, t, float(cfg.damage));
    REXLOG_INFO("BF explode: {} of {}: {} at {:.1f} {:.1f} {:.1f} ({}, {} health components)", ++exploded,
                g_targets.size(), t.name, t.at[0], t.at[1], t.at[2], why, t.healths.size());
  };
  if (cfg.mode == Config::Mode::kTimer) {
    if (since_scan < cfg.start) return;
    const size_t due = size_t((since_scan - cfg.start) / std::max(1.0, cfg.every)) + 1;
    for (Target& t : g_targets)
      if (!t.done && size_t(exploded) < due) boom(t, "on the timer");
    return;
  }
  // The camera's requests: the lines of props.txt it added to $BF_BENCH_DIR/trigger.txt, the new whole lines read 10
  // times a second.
  static std::chrono::steady_clock::time_point read{};
  const auto now = std::chrono::steady_clock::now();
  if (now - read < std::chrono::milliseconds(100)) return;
  read = now;
  static const std::string path = BenchFile("trigger.txt");
  FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
  if (!f) return;
  std::string text;
  if (std::fseek(f, g_level.trigger_read, SEEK_SET) == 0) {
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
  }
  std::fclose(f);
  const size_t whole = text.rfind('\n');
  if (whole == std::string::npos) return;
  g_level.trigger_read += long(whole + 1);
  for (size_t at = 0; at <= whole;) {
    const size_t end = text.find('\n', at);
    const unsigned long i = std::strtoul(text.c_str() + at, nullptr, 10);
    if (i < g_targets.size() && !g_targets[i].done) boom(g_targets[i], "in the camera's view");
    at = end + 1;
  }
}

}  // namespace

namespace bf1943 {
// From the server's tick (server_rotation.cpp), on the server's thread.
void BenchExplodeTick(PPCContext& ctx, uint8_t* base) {
  static const Config cfg = ReadConfig();
  if (cfg.mode == Config::Mode::kOff || !REXCVAR_GET(server)) return;
  using Clock = std::chrono::steady_clock;
  // A level: the server's game context made and its player manager there (as bots.cpp's Manager).
  if (!(Load32(base, 0x82AF574C) & 1) || !Load32(base, kGameContext + 40)) {
    g_level.loaded = {};
    return;
  }
  const auto now = Clock::now();
  if (g_level.loaded == Clock::time_point{}) g_level.loaded = now;
  if (!g_level.scan_started) {
    // The first level: 2 s in (the runner starts the game after the scan); after a level change, 10 s in, the level's
    // data streamed in by then (at 5 s Iwo Jima showed 20 of its 31 props).
    if (now - g_level.loaded < std::chrono::seconds(g_level.after_change ? 10 : 2)) return;
    g_level.scan_started = true;
    StartScan(base);
    return;
  }
  if (!g_level.scan_done) {
    if (!FinishScan(ctx, base)) return;
    g_level.scan_done = true;
    g_level.scanned = Clock::now();
    // The camera's requests from here on (an earlier level's in the file are not this level's props).
    if (const std::string path = BenchFile("trigger.txt"); !path.empty())
      if (FILE* f = std::fopen(path.c_str(), "rb")) {
        if (std::fseek(f, 0, SEEK_END) == 0) g_level.trigger_read = std::ftell(f);
        std::fclose(f);
      }
    return;
  }
  Tick(ctx, base, cfg, std::chrono::duration<double>(now - g_level.scanned).count());
}

// The server's level changes (server_rotation.cpp, beside BotsLevelChange): the old level's props are gone with it
// (their entities freed), so the next level is scanned again.
void BenchExplodeLevelChange() {
  if (!g_targets.empty()) REXLOG_INFO("BF explode: a new level: its props scanned again");
  g_targets.clear();
  g_level = {};
  g_level.after_change = true;
  ++g_generation;  // a scan still running is of the old level
  // The old level's list gone, so the camera waits for the new one (bench_freecam.cpp PrepareView).
  if (const std::string path = BenchFile("props.txt"); !path.empty()) std::remove(path.c_str());
}
}  // namespace bf1943

#endif  // BF_DEV_TOOLS
