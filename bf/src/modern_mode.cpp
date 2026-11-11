// Modern movement and aiming (TekRantGaming, 7 Nov 2026: "build a new modern movement and aiming system that takes from modern
// FPS games ... it must retain aim assist ... a toggle on/off setting ... servers that have the new system enabled and
// servers for the old system so we dont end up with anything unfair in the competitive space").
//
// Two settings:
//   bf_movement_pref  the player's choice (Settings > Gameplay > Movement and aiming): Quick match picks servers that
//                     play it (game_menu.cpp), and offline (the tutorial) the game plays it.
//   bf_modern_mode    what this game plays now. A dedicated server is started with it (the server package's
//                     "movement" setting, start_servers.ps1); a player's game takes the joined server's from the
//                     server list (SetJoinedServerMode), so the soldier moves the same on both sides and the server
//                     never pulls the player back.
//
// How: the game's own tuning values. Battlefield's data objects (Frostbite's reflection, tools/class_vtables.py) start
// with their class's vtable; after a level loads, a scan of guest memory finds every instance of the classes below
// (the soldier's movement per pose, its physics, sprint, the aimer, zoom levels), and in modern mode their values are
// changed (and put back in classic). The field offsets are the game's own field tables (BF_DEV_FIELDS=all).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(bf_movement_pref, "classic", "BF1943/Gameplay",
                      "Movement and aiming the player picks: classic (the original game) or modern");
REXCVAR_DEFINE_BOOL(bf_modern_mode, false, "BF1943/Gameplay",
                    "Modern movement and aiming in this game (a dedicated server: what it plays; a player's game: "
                    "what the joined server plays)");
REXCVAR_DECLARE(bool, server);  // server_mode.cpp

namespace bf1943 {

// Joining a server from our menu (game_menu.cpp Join): the game plays what that server plays.
void SetJoinedServerMode(bool modern) {
  if (REXCVAR_GET(bf_modern_mode) != modern)
    REXLOG_INFO("BF modern mode: the server plays {} movement and aiming", modern ? "modern" : "classic");
  rex::cvar::SetFlagByName("bf_modern_mode", modern ? "true" : "false");
}

// Whether this game plays the modern system now.
bool ModernMode() { return REXCVAR_GET(bf_modern_mode); }
bool InMatch();      // auto_join.cpp
bool LevelLoaded();  // auto_join.cpp: the level's loading screen is over

extern std::atomic<uint32_t> g_aim_data;  // look.cpp: the local soldier's aimer data (HaloAimerData)

namespace {

uint32_t Load32(const uint8_t* base, uint32_t a) {
  return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
}
float LoadFloat(const uint8_t* base, uint32_t a) {
  const uint32_t bits = Load32(base, a);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// A class whose instances the scan finds: its vtable (tools/class_vtables.py) and the fields shown or changed.
struct Field {
  const char* name;
  uint32_t offset;
  char type;  // 'f' float, 'b' bool (byte), 'i' a 32 bit integer
};
struct Class {
  const char* name;
  uint32_t vtable, size;
  std::vector<Field> fields;
};
const std::vector<Class>& Classes() {
  static const std::vector<Class> classes = {
      {"CharacterStatePoseInfo", 0x82034C24, 0x24,
       {{"DecelerationGain", 0x0C, 'f'}, {"SprintGain", 0x10, 'f'}, {"SprintMultiplier", 0x14, 'f'},
        {"PoseType", 0x18, 'i'}, {"Velocity", 0x1C, 'f'}, {"AccelerationGain", 0x20, 'f'}}},
      {"CharacterPhysicsData", 0x82034D2C, 0x90,
       {{"LadderAcceptAnglePitch", 0x18, 'f'}, {"JumpPenaltyTime", 0x1C, 'f'}, {"InputAcceleration", 0x20, 'f'},
        {"LadderAcceptAngle", 0x24, 'f'}, {"SpeedPenaltyOnDamage", 0x28, 'f'}, {"SpeedPenaltyOnDamageTime", 0x2C, 'f'},
        {"JumpPenaltyFactor", 0x30, 'f'}, {"WaterDepthLimit", 0x44, 'f'}, {"MaxAscendAngle", 0x80, 'f'},
        {"PhysicalRadius", 0x84, 'f'}, {"Mass", 0x8C, 'f'}}},
      {"CharacterSprintData", 0x82034C38, 0x18,
       {{"SprintMinimumPower", 0x0C, 'f'}, {"SprintPowerIncreasePerSecond", 0x10, 'f'},
        {"SprintPowerDecreasePerSecond", 0x14, 'f'}}},
      {"CharacterPoseData", 0x82034D18, 0x50,
       {{"TransitionTime", 0x20, 'f'}, {"PoseType", 0x34, 'i'}, {"StepHeight", 0x48, 'f'}, {"Height", 0x4C, 'f'}}},
      {"SoldierSprintSettingsData", 0x8205ABEC, 0x38,
       {{"ThrottleInputRequired", 0x0C, 'f'}, {"FovOutTime", 0x10, 'f'}, {"RecoverTime", 0x24, 'f'},
        {"FovInDelay", 0x28, 'f'}, {"Fov", 0x2C, 'f'}, {"FovOutDelay", 0x30, 'f'}, {"FovInTime", 0x34, 'f'}}},
      {"HaloAimerData", 0x8205F188, 0x7C,
       {{"SuckTargetSpeedInfluence", 0x0C, 'f'}, {"SuckUserInputMultiplier", 0x14, 'f'},
        {"AccelerationInputThreshold", 0x40, 'f'}, {"AccelerationMultiplier", 0x64, 'f'},
        {"PitchSpeed", 0x68, 'f'}, {"SuckSoftZone", 0x6C, 'f'}, {"AccelerationDamping", 0x70, 'f'},
        {"YawSpeed", 0x74, 'f'}, {"UsePitchAcceleration", 0x78, 'b'}, {"UseYawAcceleration", 0x79, 'b'}}},
      {"ZoomLevelData", 0x8205F110, 0x70,
       {{"TransitionTime", 0x0C, 'f'}, {"FovDelayTime", 0x10, 'f'}, {"FovTransitionTime", 0x14, 'f'},
        {"MoveSpeedMultiplier", 0x48, 'f'}, {"LookSpeedMultiplier", 0x4C, 'f'}, {"SprintLookSpeedMultiplier", 0x50, 'f'},
        {"FieldOfView", 0x54, 'f'}}},
  };
  return classes;
}

// Every committed range of guest memory outside the image, in the host's view (base + guest address), as 64 bit
// [start, end): the last region reaches 4 GB (32 bit sums wrapped to guest 0 there: run jeepT's crash). A level's
// data objects sit in the low virtual heap (the aimer at 0x1FD3B080 in run mod1).
template <typename Fn>
void ForEachGuestRange(const uint8_t* base, Fn&& fn) {
#if defined(_WIN32)
  for (uint64_t a = 0x00010000; a < 0x100000000ull;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) break;
    const uint64_t start = std::max<uint64_t>(uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base), a);
    const uint64_t end = std::min<uint64_t>(uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base) + mbi.RegionSize,
                                            0x100000000ull);
    const bool image = start >= 0x80000000ull && start < 0xA0000000ull;
    if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && !image && end > start)
      fn(start, end);
    if (end <= a) break;
    a = end;
  }
#endif
}

// BF_DEV_MODERN_DUMP=<seconds>[,<file>] (developer builds): that long after the start, every instance of the classes
// above with its values, to the file (or the log).
void DevDump(const uint8_t* base) {
#if defined(BF_DEV_TOOLS)
  static const std::string spec = [] {
    const char* v = std::getenv("BF_DEV_MODERN_DUMP");
    return std::string(v ? v : "");
  }();
  if (spec.empty()) return;
  static const double at = std::atof(spec.c_str());
  static const std::string file = spec.find(',') != std::string::npos ? spec.substr(spec.find(',') + 1) : "";
  static const auto start = std::chrono::steady_clock::now();
  static bool done = false;
  if (done || std::chrono::steady_clock::now() - start < std::chrono::duration<double>(at)) return;
  done = true;
  FILE* f = file.empty() ? nullptr : std::fopen(file.c_str(), "w");
  // The local soldier's aimer data (look.cpp): an instance's own first words, and what sits before it.
  if (const uint32_t aim = g_aim_data.load(); aim && f) {
    std::fprintf(f, "aimer data at %08X:", aim);
    for (int k = -8; k < 32; ++k) std::fprintf(f, " %08X", Load32(base, aim + 4 * k));
    std::fprintf(f, "\n");
  }
  const auto t0 = std::chrono::steady_clock::now();
  std::map<uint32_t, const Class*> by_vtable;
  for (const Class& c : Classes()) by_vtable[c.vtable] = &c;
  uint64_t scanned = 0;
  int found = 0;
  ForEachGuestRange(base, [&](uint64_t start, uint64_t end) {
    scanned += end - start;
    for (uint64_t a64 = start; a64 + 4 <= end; a64 += 4) {
      const uint32_t a = uint32_t(a64);
      const uint32_t w = Load32(base, a);
      const auto it = by_vtable.find(w);
      if (it == by_vtable.end()) continue;
      const Class& c = *it->second;
      if (a64 + c.size > end) continue;
      ++found;
      char buf[96];
      std::snprintf(buf, sizeof(buf), "%s @%08X:", c.name, a);
      std::string line = buf;
      for (const Field& fd : c.fields) {
        if (fd.type == 'f') std::snprintf(buf, sizeof(buf), " %s=%g", fd.name, LoadFloat(base, a + fd.offset));
        else if (fd.type == 'b') std::snprintf(buf, sizeof(buf), " %s=%u", fd.name, base[a + fd.offset]);
        else std::snprintf(buf, sizeof(buf), " %s=%u", fd.name, Load32(base, a + fd.offset));
        line += buf;
      }
      // An aimer's response curves (InputPolynomial at +0x44, ZoomedInputPolynomial at +0x54): arrays of 8 floats,
      // their begin and end at +8 and +12 of each.
      if (c.vtable == 0x8205F188) {
        for (uint32_t arr : {0x44u, 0x54u}) {
          const uint32_t b = Load32(base, a + arr + 8), e = Load32(base, a + arr + 12);
          line += arr == 0x44 ? " InputPolynomial=" : " ZoomedInputPolynomial=";
          for (uint32_t q = b; q && q < e && q < b + 64; q += 4) {
            std::snprintf(buf, sizeof(buf), "%g,", LoadFloat(base, q));
            line += buf;
          }
        }
      }
      if (f) std::fprintf(f, "%s\n", line.c_str());
      else REXLOG_INFO("BF modern dump: {}", line);
    }
  });
  if (f) std::fclose(f);
  REXLOG_INFO("BF modern dump: {} instances in {} MB of guest memory, {:.0f} ms", found, scanned >> 20,
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
#else
  (void)base;
#endif
}

void StoreFloat(uint8_t* base, uint32_t a, float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  base[a] = uint8_t(bits >> 24), base[a + 1] = uint8_t(bits >> 16), base[a + 2] = uint8_t(bits >> 8);
  base[a + 3] = uint8_t(bits);
}

// One value the modern mode changes: where, and the game's own value there (put back in classic).
struct Change {
  uint32_t address, vtable, object;  // the value, and the data object it is in (its vtable checked before writes)
  uint32_t original, modern;         // as raw big endian words (floats)
};

// The modern values, worked out from the game's own (runs mod2: the values on Guadalcanal and Wake Island):
//   aim       the soldier's aimer (YawSpeed 3.5, PitchSpeed 1.5, no turn acceleration, the curve 0.3x + 0.4x^8 +
//             0.3x^128: at 90% of the stick 44% of the turn): the curve 0.45x + 0.55x^2 (90%: 85%), turning 4.2 and
//             pitching 2.3 rad/s. Its aim assist (the Suck values) is the game's own.
//   zoom      zoomed in, the look speed follows the zoom (65% of the field of view's ratio, never below the game's:
//             0.2 at 50 degrees became 0.58; it was 85%, 0.76, and TekRantGaming, 10 Nov 2026: aiming down the sights felt as
//             fast as from the hip, bring it in a little for tighter control), moving zoomed 55% (was 30%), zooming
//             in and out 30% quicker
//   movement  on the ground (standing and crouched) the speed builds 3.5 times quicker (0.1 a tick became 0.35) and
//             stops sooner (deceleration 0.8 became 1), the sprint builds 2.5 times quicker; the stick reaches its
//             full push in an eighth of a second (InputAcceleration 3 became 8); the slow down after a jump is
//             shorter and smaller (30% for 2 s became 75% for 0.6 s); the top speeds stay the game's (4.5 m/s
//             walking, 6.1 sprinting), so every server plays at the same speeds
//   sprint    shooting 0.12 s after a sprint (was 0.3), a sprint from 70% of the stick (was 80%)
//   crouch    standing up and crouching in 0.15 s (was 0.4)
// In the air nothing changes (no extra air control, no bunny hops).
std::vector<Change> ChangesFor(const uint8_t* base, uint32_t vtable, uint32_t a) {
  std::vector<Change> out;
  auto f = [&](uint32_t off) { return LoadFloat(base, a + off); };
  auto set = [&](uint32_t address, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    const uint32_t was = Load32(base, address);
    if (was != bits) out.push_back({address, vtable, a, was, bits});
  };
  switch (vtable) {
    case 0x8205F188: {  // HaloAimerData: the soldier's (on foot) aimers only
      if (std::fabs(f(0x74) - 3.5f) > 1e-3f || std::fabs(f(0x68) - 1.5f) > 1e-3f || base[a + 0x79] || base[a + 0x78]) break;
      const uint32_t curve = Load32(base, a + 0x44 + 8), curve_end = Load32(base, a + 0x44 + 12);
      if (!curve || curve_end != curve + 32) break;
      if (std::fabs(LoadFloat(base, curve) - 0.3f) > 1e-3f || std::fabs(LoadFloat(base, curve + 12) - 0.4f) > 1e-3f) break;
      set(a + 0x74, 4.2f);
      set(a + 0x68, 2.3f);
      static const float kCurve[8] = {0.45f, 0.55f, 0, 0, 0, 0, 0, 0};
      for (int i = 0; i < 8; ++i) set(curve + 4 * i, kCurve[i]);
      break;
    }
    case 0x8205F110: {  // ZoomLevelData
      const float fov = f(0x54);
      if (fov > 1 && fov < 54) {
        const float ratio = std::tan(fov * 0.5f * 3.14159265f / 180.0f) / std::tan(27.5f * 3.14159265f / 180.0f);
        set(a + 0x4C, std::max(f(0x4C), 0.65f * ratio));
        if (f(0x48) < 0.5f) set(a + 0x48, 0.55f);
      }
      set(a + 0x0C, f(0x0C) * 0.7f);
      set(a + 0x14, f(0x14) * 0.7f);
      break;
    }
    case 0x82034D2C: {  // CharacterPhysicsData: the soldier's (radius 0.35, mass 100)
      if (std::fabs(f(0x84) - 0.35f) > 1e-3f) break;
      set(a + 0x20, std::max(f(0x20), 8.0f));
      set(a + 0x1C, std::min(f(0x1C), 0.6f));
      set(a + 0x30, std::max(f(0x30), 0.75f));
      break;
    }
    case 0x82034C24: {  // CharacterStatePoseInfo: on the ground (not in the air: top speed 8, no deceleration)
      if (f(0x1C) >= 7.9f || f(0x0C) <= 0.0f) break;
      if (f(0x20) < 0.5f) set(a + 0x20, std::min(0.6f, f(0x20) * 3.5f));
      if (f(0x0C) < 1.0f) set(a + 0x0C, 1.0f);
      if (f(0x10) < 0.5f) set(a + 0x10, std::min(1.0f, f(0x10) * 2.5f));
      break;
    }
    case 0x82034D18: {  // CharacterPoseData: standing and crouched (height 1.8 and 1.15), quicker changes
      if (f(0x4C) > 1.0f && f(0x4C) < 2.0f) set(a + 0x20, std::min(f(0x20), 0.15f));
      break;
    }
    case 0x8205ABEC: {  // SoldierSprintSettingsData
      set(a + 0x24, std::min(f(0x24), 0.12f));
      set(a + 0x0C, std::min(f(0x0C), 0.7f));
      break;
    }
  }
  return out;
}

struct Applied {
  std::mutex mutex;
  std::vector<Change> changes;  // found by the last scan
  bool modern = false;          // what they hold now
  uint32_t level = 0;           // the level serial they were found for
};
Applied& GetApplied() {
  static Applied a;
  return a;
}
std::atomic<uint32_t> g_level_serial{1};  // a new level: scan again (NoteLevelChange)

// Every instance of the classes the modern mode changes, and their changes (from the game's values as they are: a
// scan is made before any change, or right after a level loaded).
std::vector<Change> Scan(const uint8_t* base) {
  std::vector<Change> all;
  const auto t0 = std::chrono::steady_clock::now();
  int objects = 0;
  ForEachGuestRange(base, [&](uint64_t start, uint64_t end) {
    for (uint64_t a64 = start; a64 + 0x90 <= end; a64 += 4) {
      const uint32_t w = Load32(base, uint32_t(a64));
      if (w != 0x8205F188 && w != 0x8205F110 && w != 0x82034D2C && w != 0x82034C24 && w != 0x8205ABEC &&
          w != 0x82034D18)
        continue;
      ++objects;
      for (Change& c : ChangesFor(base, w, uint32_t(a64))) all.push_back(c);
    }
  });
  REXLOG_INFO("BF modern mode: {} data objects, {} values to change ({:.0f} ms)", objects, all.size(),
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  return all;
}

#if defined(BF_DEV_TOOLS)
}  // namespace
namespace dev {
void WatchHostAccess(std::vector<const void*> addresses, double seconds, const char* label);  // dev_watch.cpp
}
namespace {
// BF_DEV_WATCH_POSE=1: who reads the standing pose's top speed and gains on the ground (CharacterStatePoseInfo with
// 4.5 m/s), for 20 s after the scan: the soldier's movement code.
void DevWatchPose(uint8_t* base, const std::vector<Change>& changes) {
  if (!std::getenv("BF_DEV_WATCH_POSE")) return;
  for (const Change& c : changes) {
    if (c.vtable != 0x82034C24 || std::fabs(LoadFloat(base, c.object + 0x1C) - 4.5f) > 1e-3f) continue;
    dev::WatchHostAccess({base + c.object + 0x1C, base + c.object + 0x20, base + c.object + 0x0C}, 20.0, "pose");
    REXLOG_INFO("BF modern mode: watching the pose at {:08X}", c.object);
    return;
  }
}
#endif

// Writes each change's modern or original value, where its object is still the one scanned.
void Write(uint8_t* base, std::vector<Change>& changes, bool modern) {
  int written = 0;
  for (const Change& c : changes) {
    if (Load32(base, c.object) != c.vtable) continue;
    const uint32_t v = modern ? c.modern : c.original;
    base[c.address] = uint8_t(v >> 24), base[c.address + 1] = uint8_t(v >> 16), base[c.address + 2] = uint8_t(v >> 8);
    base[c.address + 3] = uint8_t(v);
    ++written;
  }
  REXLOG_INFO("BF modern mode: {} movement and aiming ({} values)", modern ? "modern" : "classic", written);
}

}  // namespace

// A level is loading or loaded (server_rotation.cpp's level change; the client's match start, below): the next
// check scans again.
void NoteLevelChange() { g_level_serial.fetch_add(1); }

namespace {
// The scanner's thread: once a second, a scan when a level (re)loaded, then the values the mode wants. A scan reads
// hundreds of MB of guest memory (about 3 s here), so it never runs on the game's thread.
void Scanner(uint8_t* base) {
  bool was_in_match = false;
  auto settled_at = std::chrono::steady_clock::now();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    DevDump(base);
    // A player's game: a match's level loaded (at the menu driver's stage 5 the loading screen is still up: run
    // modern1 found no data then). A dedicated server: its levels' changes, and its first level after the start.
    const bool in_match = REXCVAR_GET(server) ? true : InMatch() && LevelLoaded();
    if (in_match && !was_in_match) NoteLevelChange();
    was_in_match = in_match;
    static uint32_t seen = 0;
    if (const uint32_t serial = g_level_serial.load(); serial != seen) seen = serial, settled_at = std::chrono::steady_clock::now();
    if (!in_match) continue;
    // A few seconds after the level is up, so its data is all in.
    if (std::chrono::steady_clock::now() - settled_at < std::chrono::seconds(REXCVAR_GET(server) ? 20 : 3)) continue;
    Applied& ap = GetApplied();
    std::lock_guard lock(ap.mutex);
    const uint32_t level = g_level_serial.load();
    const bool modern = ModernMode();
    // A scan that found none of the soldier's data (the level still loading) tries again every 5 s.
    static auto retry_at = std::chrono::steady_clock::now();
    if (ap.level != level || (ap.changes.empty() && std::chrono::steady_clock::now() > retry_at)) {
      ap.level = level;
      ap.changes.clear();
      // The game's own values: scanned while nothing of ours is in them (a new level loads them fresh).
      ap.changes = Scan(base);
      ap.modern = false;
#if defined(BF_DEV_TOOLS)
      DevWatchPose(base, ap.changes);
#endif
      retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }
    if (ap.modern != modern) {
      Write(base, ap.changes, modern);
      ap.modern = modern;
    }
  }
}
}  // namespace

#if defined(BF_DEV_TOOLS)
extern std::atomic<float> g_dev_camera[3];  // look.cpp: the local camera's position
extern std::atomic<float> g_dev_yaw;        // look.cpp: the local soldier's yaw
// BF_DEV_SPEED=1 (developer builds): every 0.1 s the local soldier's speed over the ground and its turn rate, from
// the camera's position and the aim's yaw (modern and classic movement and aiming compared: runs modern1, classic1).
void DevSpeed() {
  static const bool on = std::getenv("BF_DEV_SPEED") != nullptr;
  if (!on) return;
  using Clock = std::chrono::steady_clock;
  static Clock::time_point last{};
  static float lx = 0, lz = 0, lyaw = 0;
  const auto now = Clock::now();
  if (last.time_since_epoch().count() && now - last < std::chrono::milliseconds(100)) return;
  const float x = g_dev_camera[0].load(), z = g_dev_camera[2].load(), yaw = g_dev_yaw.load();
  if (last.time_since_epoch().count()) {
    const double dt = std::chrono::duration<double>(now - last).count();
    float dyaw = yaw - lyaw;
    while (dyaw > 3.14159265f) dyaw -= 6.2831853f;
    while (dyaw < -3.14159265f) dyaw += 6.2831853f;
    REXLOG_INFO("BF speed: {:.2f} m/s, turning {:.2f} rad/s", std::sqrt((x - lx) * (x - lx) + (z - lz) * (z - lz)) / dt,
                dyaw / dt);
  }
  last = now, lx = x, lz = z, lyaw = yaw;
}
#endif

// Called often from the game's frame (auto_join.cpp's pad read; server_rotation.cpp's server tick): the first call
// starts the scanner's own thread.
void ModernModeTick(uint8_t* base) {
#if defined(BF_DEV_TOOLS)
  DevSpeed();
#endif
  static std::once_flag once;
  std::call_once(once, [base] { std::thread([base] { Scanner(base); }).detach(); });
}

}  // namespace bf1943
