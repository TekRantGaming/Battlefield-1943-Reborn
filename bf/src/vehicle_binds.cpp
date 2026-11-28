// The keyboard and mouse's binding sets for each kind of vehicle (vehicle_binds.h). The keyboard driver (the SDK's
// mnk_input_driver.cpp) asks for each bind's keys at each read (SetBindSource); in any seat of a vehicle the answer is
// that vehicle kind's setting, else the infantry's own.

#include "vehicle_binds.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>

#include "seat_info.h"

// Each vehicle's set, its defaults the infantry's (the SDK's keybind_* in mnk_input_driver.cpp: keep them the same).
#define BF_VEHICLE_BIND(action, keys, what)                                                                        \
  REXCVAR_DEFINE_STRING(keybind_ground_##action, keys, "Input/Keybinds/Ground vehicles", "Ground vehicles: " what); \
  REXCVAR_DEFINE_STRING(keybind_air_##action, keys, "Input/Keybinds/Air vehicles", "Air vehicles: " what);          \
  REXCVAR_DEFINE_STRING(keybind_boat_##action, keys, "Input/Keybinds/Boats", "Boats: " what)
BF_VEHICLE_BIND(a, "Space", "A button (change seat)");
BF_VEHICLE_BIND(b, "E", "B button (exit)");
BF_VEHICLE_BIND(x, "R", "X button");
BF_VEHICLE_BIND(y, "F", "Y button");
BF_VEHICLE_BIND(left_trigger, "RMB", "Left trigger (the keyboard's: the second weapon)");
BF_VEHICLE_BIND(right_trigger, "LMB", "Right trigger: fire");
BF_VEHICLE_BIND(left_shoulder, "G", "Left shoulder (brake)");
BF_VEHICLE_BIND(right_shoulder, "Q", "Right shoulder (second weapon)");
BF_VEHICLE_BIND(lstick_up, "W", "Left stick up (the keyboard's: throttle)");
BF_VEHICLE_BIND(lstick_down, "S", "Left stick down (the keyboard's: brake, reverse)");
BF_VEHICLE_BIND(lstick_left, "A", "Left stick left");
BF_VEHICLE_BIND(lstick_right, "D", "Left stick right");
// The left stick's press does nothing in a vehicle (the game's Controls page); Shift is the planes' and the ground
// vehicles' Free-Look below, so their sets start with no key here (18 Nov 2026).
REXCVAR_DEFINE_STRING(keybind_ground_lstick_press, "", "Input/Keybinds/Ground vehicles",
                      "Ground vehicles: Left stick press");
REXCVAR_DEFINE_STRING(keybind_air_lstick_press, "", "Input/Keybinds/Air vehicles", "Air vehicles: Left stick press");
REXCVAR_DEFINE_STRING(keybind_boat_lstick_press, "Shift", "Input/Keybinds/Boats", "Boats: Left stick press");
BF_VEHICLE_BIND(rstick_up, "I", "Right stick up");
BF_VEHICLE_BIND(rstick_down, "K", "Right stick down");
BF_VEHICLE_BIND(rstick_left, "J", "Right stick left");
BF_VEHICLE_BIND(rstick_right, "L", "Right stick right");
BF_VEHICLE_BIND(rstick_press, "C,Control", "Right stick press");
BF_VEHICLE_BIND(dpad_up, "Up,V", "D-pad up (camera)");
BF_VEHICLE_BIND(dpad_down, "Down", "D-pad down");
BF_VEHICLE_BIND(dpad_left, "Left", "D-pad left");
BF_VEHICLE_BIND(dpad_right, "Right", "D-pad right");
BF_VEHICLE_BIND(back, "Tab", "Back button: command (tap), score (hold)");
#undef BF_VEHICLE_BIND
// Free-Look (vehicle_views.cpp; the user, 18 Nov 2026: "Shift as default. Make it remapable in the controls as just
// Free-Look"): held, the mouse turns the view in a plane and in a tank's third person view. The keyboard driver reads
// it as the Guide button (DriverBind below), which the game never gets.
REXCVAR_DEFINE_STRING(keybind_ground_free_look, "Shift", "Input/Keybinds/Ground vehicles",
                      "Ground vehicles: Free-Look (hold: the mouse turns a tank's third person view)");
REXCVAR_DEFINE_STRING(keybind_air_free_look, "Shift", "Input/Keybinds/Air vehicles",
                      "Air vehicles: Free-Look (hold: the mouse turns the view)");

// Seat keys (issue 80; the user, 27 Nov 2026: "default seat keys be 1-9, not F1-F9"): in any seat of a vehicle, a
// seat's key moves there (SeatKeysPad presses the game's own Change seat until the seat is reached). One setting each
// for every vehicle; their rows are in the ground vehicles' and the boats' tabs of the bindings.
#define BF_SEAT_KEY(n)                                                                                   \
  REXCVAR_DEFINE_STRING(keybind_seat_##n, #n, "Input/Keybinds/Vehicle seats",                            \
                        "Vehicles: move to seat " #n " (1 the driver or pilot) if it is free")
BF_SEAT_KEY(1); BF_SEAT_KEY(2); BF_SEAT_KEY(3); BF_SEAT_KEY(4); BF_SEAT_KEY(5);
BF_SEAT_KEY(6); BF_SEAT_KEY(7); BF_SEAT_KEY(8); BF_SEAT_KEY(9);
#undef BF_SEAT_KEY

// The controller's button remapping for each vehicle kind (the infantry's is bf_remap_*, input_settings.cpp): what each
// button presses, its own by default.
#define BF_VEHICLE_REMAP(button)                                                                                     \
  REXCVAR_DEFINE_STRING(bf_remap_ground_##button, #button, "BF1943/Controls/Remap/Ground vehicles",                  \
                        "Ground vehicles: what the controller's " #button " presses");                              \
  REXCVAR_DEFINE_STRING(bf_remap_air_##button, #button, "BF1943/Controls/Remap/Air vehicles",                        \
                        "Air vehicles: what the controller's " #button " presses");                                 \
  REXCVAR_DEFINE_STRING(bf_remap_boat_##button, #button, "BF1943/Controls/Remap/Boats",                              \
                        "Boats: what the controller's " #button " presses")
BF_VEHICLE_REMAP(a); BF_VEHICLE_REMAP(b); BF_VEHICLE_REMAP(x); BF_VEHICLE_REMAP(y);
BF_VEHICLE_REMAP(lb); BF_VEHICLE_REMAP(rb); BF_VEHICLE_REMAP(lt); BF_VEHICLE_REMAP(rt);
BF_VEHICLE_REMAP(ls); BF_VEHICLE_REMAP(rs); BF_VEHICLE_REMAP(back); BF_VEHICLE_REMAP(start);
BF_VEHICLE_REMAP(up); BF_VEHICLE_REMAP(down); BF_VEHICLE_REMAP(left); BF_VEHICLE_REMAP(right);
#undef BF_VEHICLE_REMAP

// The controller's stick remapping, each set's: which of the controller's stick axes the game gets on each of its own
// (input_settings.cpp AxisRemap), "-" before it inverted, "none" nothing; each its own by default.
#define BF_AXIS(axis, what)                                                                                          \
  REXCVAR_DEFINE_STRING(bf_axis_##axis, #axis, "BF1943/Controls/Sticks", "Infantry: what the game gets on " what);  \
  REXCVAR_DEFINE_STRING(bf_axis_ground_##axis, #axis, "BF1943/Controls/Sticks/Ground vehicles",                     \
                        "Ground vehicles: what the game gets on " what);                                           \
  REXCVAR_DEFINE_STRING(bf_axis_air_##axis, #axis, "BF1943/Controls/Sticks/Air vehicles",                           \
                        "Air vehicles: what the game gets on " what);                                              \
  REXCVAR_DEFINE_STRING(bf_axis_boat_##axis, #axis, "BF1943/Controls/Sticks/Boats", "Boats: what the game gets on " what)
BF_AXIS(lx, "the left stick's x (lx, -lx, ly, -ly, rx, -rx, ry, -ry or none)");
BF_AXIS(ly, "the left stick's y (lx, -lx, ly, -ly, rx, -rx, ry, -ry or none)");
BF_AXIS(rx, "the right stick's x (lx, -lx, ly, -ly, rx, -rx, ry, -ry or none)");
BF_AXIS(ry, "the right stick's y (lx, -lx, ly, -ly, rx, -rx, ry, -ry or none)");
#undef BF_AXIS

namespace bf1943 {
namespace {
using Clock = std::chrono::steady_clock;

// The actions each vehicle's set has, by the infantry's names without "keybind_".
constexpr std::array<std::string_view, 23> kVehicleActions = {
    "a",           "b",           "x",          "y",           "left_trigger", "right_trigger", "left_shoulder",
    "right_shoulder", "lstick_up", "lstick_down", "lstick_left", "lstick_right", "lstick_press", "rstick_up",
    "rstick_down", "rstick_left", "rstick_right", "rstick_press", "dpad_up",     "dpad_down",     "dpad_left",
    "dpad_right",  "back"};
constexpr const char* kSetPrefix[kBindSetCount] = {"", "ground_", "air_", "boat_"};
constexpr std::string_view kPrefix = "keybind_";
// The controller's buttons by their remapping names (input_settings.cpp kButtons).
constexpr std::array<std::string_view, 16> kPadButtons = {"a",  "b",  "x",    "y",     "lb", "rb",   "lt",   "rt",
                                                          "ls", "rs", "back", "start", "up", "down", "left", "right"};

// The action's name without "keybind_", empty when it is none of the vehicles' actions.
std::string_view ActionOf(std::string_view infantry_cvar) {
  if (infantry_cvar.substr(0, kPrefix.size()) != kPrefix) return {};
  const std::string_view action = infantry_cvar.substr(kPrefix.size());
  for (std::string_view a : kVehicleActions)
    if (a == action) return a;
  return {};
}

int SetOfSeat() {
  SeatInfo seat;
  if (!LocalSeat(seat)) return kBindInfantry;
  switch (seat.kind) {
    case SeatKind::kPlane:
    case SeatKind::kBomber: return kBindAir;
    case SeatKind::kDriver: return seat.boat ? kBindBoat : kBindGround;  // its names (native_input.cpp)
    // A gunner's or a passenger's seat, or a stationary gun: its vehicle's set (issue 78: the turrets and passenger
    // seats took the infantry's keys, so Change seat there was the infantry's Jump). The planes have no such seats.
    case SeatKind::kGunner: return !seat.vehicle ? kBindInfantry : seat.boat ? kBindBoat : kBindGround;
    default: return kBindInfantry;  // on foot, not known
  }
}

std::atomic<int> g_logged_set{-1};

// The keyboard driver's source: the active set's keys for the actions it has. In a plane or a ground vehicle the
// Guide button (no key of its own, and nothing the game reads) is Free-Look's keys (vehicle_views.cpp reads it).
std::string DriverBind(std::string_view name, const std::string& own) {
  const int set = ActiveBindSet();
  if (name == "keybind_guide" && (set == kBindGround || set == kBindAir))
    return rex::cvar::GetFlagByName(BindCvar(set, kFreeLookBind));
  if (set == kBindInfantry || ActionOf(name).empty()) return own;
  return rex::cvar::GetFlagByName(BindCvar(set, name));
}
}  // namespace

const char* BindSetName(int set) {
  switch (set) {
    case kBindGround: return "Ground vehicles";
    case kBindAir: return "Air vehicles";
    case kBindBoat: return "Boats";
    default: return "Infantry";
  }
}

bool VehicleBindAction(std::string_view infantry_cvar) { return !ActionOf(infantry_cvar).empty(); }

bool SeatKeyBind(std::string_view cvar) {
  return cvar.size() == 14 && cvar.substr(0, 13) == "keybind_seat_" && cvar[13] >= '1' && cvar[13] <= '9';
}

bool BindInSet(int set, std::string_view infantry_cvar) {
  if (infantry_cvar == kFreeLookBind) return set == kBindGround || set == kBindAir;
  if (SeatKeyBind(infantry_cvar)) return set == kBindGround || set == kBindBoat;  // a plane has one seat
  return set == kBindInfantry || VehicleBindAction(infantry_cvar);
}

std::string BindCvar(int set, std::string_view infantry_cvar) {
  if (infantry_cvar == kFreeLookBind)
    return set == kBindGround ? "keybind_ground_free_look" : set == kBindAir ? "keybind_air_free_look" : "";
  const std::string_view action = ActionOf(infantry_cvar);
  if (set <= kBindInfantry || set >= kBindSetCount || action.empty()) return std::string(infantry_cvar);
  return std::string(kPrefix) + kSetPrefix[set] + std::string(action);
}

// Worked out at most every 10 ms on each thread (the driver asks for 25 binds at each read).
int ActiveBindSet() {
  thread_local Clock::time_point at{};
  thread_local int set = kBindInfantry;
  const auto now = Clock::now();
  if (now - at < std::chrono::milliseconds(10)) return set;
  at = now, set = SetOfSeat();
  if (g_logged_set.exchange(set, std::memory_order_relaxed) != set) {
    SeatInfo seat;
    const bool known = LocalSeat(seat);
    REXLOG_INFO("BF bindings: the {} keys now ({})", BindSetName(set), known ? SeatKindName(seat.kind) : "no seat");
  }
  return set;
}

std::string ActiveBind(std::string_view infantry_cvar) {
  return rex::cvar::GetFlagByName(BindCvar(ActiveBindSet(), infantry_cvar));
}

const char* BindLabel(int set, std::string_view infantry_cvar, const char* label) {
  if (infantry_cvar == kFreeLookBind) return "Free-Look";
  const std::string_view a = ActionOf(infantry_cvar);
  if (set == kBindAir) {  // the game's plane layout (its Controls page, runs help3 and helpveh1)
    if (a == "lstick_up") return "Throttle";
    if (a == "lstick_down") return "Free look (hold; the left bumper)";
    if (a == "lstick_left") return "Yaw left (rudder)";
    if (a == "lstick_right") return "Yaw right (rudder)";
    if (a == "rstick_up") return "Pitch: stick up (climbs, Invert flight Normal)";
    if (a == "rstick_down") return "Pitch: stick down (dives, Invert flight Normal)";
    if (a == "rstick_left") return "Roll left";
    if (a == "rstick_right") return "Roll right";
    if (a == "left_trigger" || a == "right_shoulder") return "Drop bombs";
    // The game's own (its sticks turn the view while held); Free-Look, the mouse's, has its own row (18 Nov 2026).
    if (a == "left_shoulder") return "Game free look (hold)";
    if (a == "right_trigger") return "Fire";
  } else if (set == kBindGround || set == kBindBoat) {  // land and sea (the same page)
    if (a == "lstick_up") return "Throttle";
    if (a == "lstick_down") return "Brake, reverse";
    if (a == "lstick_left") return "Turn left";
    if (a == "lstick_right") return "Turn right";
    if (a == "left_trigger") return "Second weapon (gun seats: zoom)";  // input_settings.cpp ThrottleSeat
    if (a == "right_shoulder") return "Second weapon";
    if (a == "left_shoulder") return "Brake";
    if (a == "right_trigger") return "Fire";
    if (a == "rstick_up") return "Look, turret up (mouse camera off)";
    if (a == "rstick_down") return "Look, turret down (mouse camera off)";
    if (a == "rstick_left") return "Look, turret left (mouse camera off)";
    if (a == "rstick_right") return "Look, turret right (mouse camera off)";
  }
  if (set != kBindInfantry) {
    if (a == "a") return "Change seat";
    if (a == "b") return "Exit";
    if (a == "x" || a == "y") return a == "x" ? "X button" : "Y button";
    if (a == "lstick_press") return "Left stick press";
    if (a == "rstick_press") return "Right stick press";
    if (a == "dpad_up") return "Camera (D-pad up)";
  }
  return label;
}

const char* PadLabel(int set, std::string_view b, const char* label) {
  if (set == kBindInfantry || set < 0 || set >= kBindSetCount) return label;
  // The game's Controls page (runs help3 and helpveh1): vehicles and boats, and planes.
  if (b == "lt") return "Throttle";
  if (b == "rt") return "Fire";
  if (b == "lb") return set == kBindAir ? "Free look (hold)" : "Brake";
  if (b == "rb") return set == kBindAir ? "Drop bombs" : "Secondary fire";
  if (b == "a") return "Change seat";
  if (b == "b") return "Exit";
  if (b == "x") return "X button";
  if (b == "y") return "Y button";
  if (b == "ls") return "Left stick press";
  if (b == "rs") return "Right stick press";
  if (b == "up") return "Toggle camera (D-pad up)";
  return label;
}

std::string RemapCvar(int set, std::string_view button) {
  const char* prefix = set > kBindInfantry && set < kBindSetCount ? kSetPrefix[set] : "";
  return std::string("bf_remap_") + prefix + std::string(button);
}

std::string AxisCvar(int set, std::string_view axis) {
  const char* prefix = set > kBindInfantry && set < kBindSetCount ? kSetPrefix[set] : "";
  return std::string("bf_axis_") + prefix + std::string(axis);
}

const char* AxisLabel(int set, int axis) {
  static constexpr const char* kLabels[kBindSetCount][4] = {
      {"Move left and right", "Move forward and back", "Look left and right", "Look up and down"},
      {"Turn left and right", "Left stick up and down", "Look, turret left and right", "Look, turret up and down"},
      {"Yaw (rudder)", "Left stick up and down", "Roll", "Pitch"},
      {"Turn left and right", "Left stick up and down", "Look, turret left and right", "Look, turret up and down"}};
  if (set < 0 || set >= kBindSetCount || axis < 0 || axis > 3) return "";
  return kLabels[set][axis];
}

void InstallVehicleBinds() { rex::input::mnk::SetBindSource(&DriverBind); }

// ---- Seat keys (issue 80). The UI thread notes a press (SeatKeysFrame); the pad hook then presses the game's Change
// seat (A) for 80 ms, waits for the seat to change, and again, until the seat is the one asked for. The game moves to
// the next free seat at each press, so a seat that is taken or not in this vehicle is never reached: the presses stop
// when the player is back in the seat they started from, after nine, or after 3 s.
namespace {
std::atomic<int> g_seat_request{-1};  // the seat index asked for, -1 none
struct SeatWalk {
  int target = -1;
  uint32_t start_seat = 0, key_before = 0;
  int presses = 0;
  enum { kPress, kWait, kGap } phase = kPress;
  Clock::time_point until{}, deadline{};
};
SeatWalk g_walk;
std::mutex g_walk_mutex;
// A seat there are others to move to (a plane has only its pilot's).
bool InVehicle(const SeatInfo& seat) {
  return seat.vehicle && (seat.kind == SeatKind::kDriver || seat.kind == SeatKind::kGunner);
}
}  // namespace

bool KeyBindPressed(const std::string& bind);  // comms_ui.cpp

void SeatKeysFrame() {
  if (ActiveBindSet() == kBindInfantry) return;
  static const char* kKeys[9] = {"keybind_seat_1", "keybind_seat_2", "keybind_seat_3", "keybind_seat_4", "keybind_seat_5",
                                 "keybind_seat_6", "keybind_seat_7", "keybind_seat_8", "keybind_seat_9"};
  for (int i = 0; i < 9; ++i) {
    const std::string keys = rex::cvar::GetFlagByName(kKeys[i]);
    if (!keys.empty() && KeyBindPressed(keys)) {
      g_seat_request.store(i, std::memory_order_relaxed);
      return;
    }
  }
}

void SeatKeysPad(uint8_t* gamepad) {
  std::unique_lock lock(g_walk_mutex, std::try_to_lock);  // the game reads the pad from more than one thread
  if (!lock.owns_lock()) return;
  const auto now = Clock::now();
  SeatInfo seat;
  const bool known = LocalSeat(seat) && InVehicle(seat);
#if defined(BF_DEV_TOOLS)
  // BF_DEV_SEAT_KEYS="20:2,32:1" (developer builds, tests): seat 2's key 20 s after the player first sits in a vehicle,
  // seat 1's at 32 s.
  if (static const char* dev = std::getenv("BF_DEV_SEAT_KEYS"); dev && known) {
    static Clock::time_point first{};
    static size_t next = 0;
    static std::vector<std::pair<double, int>> keys = [] {
      std::vector<std::pair<double, int>> out;
      double at = 0;
      int n = 0;
      for (const char* s = std::getenv("BF_DEV_SEAT_KEYS"); s && std::sscanf(s, "%lf:%d", &at, &n) == 2;) {
        out.push_back({at, n - 1});
        s = std::strchr(s, ',');
        if (s) ++s;
      }
      return out;
    }();
    if (first == Clock::time_point{}) first = now;
    if (next < keys.size() && now - first >= std::chrono::duration<double>(keys[next].first))
      g_seat_request.store(keys[next++].second, std::memory_order_relaxed);
  }
#endif
  const int asked = g_seat_request.exchange(-1, std::memory_order_relaxed);
  if (asked >= 0) {
    if (!known || seat.seat == uint32_t(asked)) {
      if (known) REXLOG_INFO("BF seats: already in seat {}", asked + 1);
      g_walk.target = -1;
      return;
    }
    g_walk = SeatWalk{};
    g_walk.target = asked, g_walk.start_seat = seat.seat, g_walk.key_before = seat.key;
    g_walk.until = now + std::chrono::milliseconds(80), g_walk.deadline = now + std::chrono::seconds(3);
    REXLOG_INFO("BF seats: to seat {} from seat {} ({})", asked + 1, seat.seat + 1, seat.vehicle_name);
  }
  SeatWalk& w = g_walk;
  if (w.target < 0) return;
  if (!known || now > w.deadline) {
    REXLOG_INFO("BF seats: seat {} not reached ({})", w.target + 1, known ? "the game did not move the player" : "out of the vehicle");
    w.target = -1;
    return;
  }
  if (seat.seat == uint32_t(w.target)) {
    REXLOG_INFO("BF seats: in seat {} after {} change(s)", w.target + 1, w.presses + 1);
    w.target = -1;
    return;
  }
  switch (w.phase) {
    case SeatWalk::kPress:
      if (now < w.until) {
        gamepad[0] |= 0x10;  // A (0x1000, the buttons big endian): the game's Change seat
        return;
      }
      w.phase = SeatWalk::kWait, w.until = now + std::chrono::milliseconds(700);
      return;
    case SeatWalk::kWait:
      if (seat.key != w.key_before) {
        ++w.presses;
        if (seat.seat == w.start_seat || w.presses >= 9) {
          REXLOG_INFO("BF seats: seat {} is taken or not in this vehicle", w.target + 1);
          w.target = -1;
          return;
        }
        w.phase = SeatWalk::kGap, w.until = now + std::chrono::milliseconds(120);
      } else if (now > w.until) {
        REXLOG_INFO("BF seats: seat {} not reached (the game did not change seat)", w.target + 1);
        w.target = -1;
      }
      return;
    case SeatWalk::kGap:
      if (now >= w.until) w.phase = SeatWalk::kPress, w.until = now + std::chrono::milliseconds(80), w.key_before = seat.key;
      return;
  }
}

// The controller's button remapping as it was (one for everywhere) into each vehicle's set, once.
static bool SeedVehiclePad(std::map<std::string, std::string>& values) {
  if (values.count("vehicle_pad_seeded")) return false;
  int copied = 0;
  for (std::string_view button : kPadButtons) {
    const std::string presses = rex::cvar::GetFlagByName(RemapCvar(kBindInfantry, button));
    for (int set = kBindGround; set < kBindSetCount; ++set) {
      const std::string name = RemapCvar(set, button);
      if (values.count("cvar." + name)) continue;
      const auto* info = rex::cvar::GetFlagInfo(name);
      if (info && info->default_value == presses) continue;
      values["cvar." + name] = presses;
      rex::cvar::SetFlagByName(name, presses);
      ++copied;
    }
  }
  values["vehicle_pad_seeded"] = "1";
  REXLOG_INFO("BF bindings: each vehicle's controller buttons start as the infantry's ({} settings copied)", copied);
  return true;
}

// Issue 90 (0.4.8 to 0.4.8.2, SDK v0.11): the infantry's keys read by name gave nothing (the settings bridge, fixed
// in compat/cvar_bridge.cpp), so the first start seeded every vehicle set with no keys and saved them: in a vehicle
// the keyboard did nothing. Once: a vehicle set whose saved keys are all empty (what that start wrote; no player
// clears every key of a tab) is set back to its defaults and seeded again from the infantry's keys. A set with any key
// of its own is left as the player has it.
static bool RepairEmptyVehicleBinds(std::map<std::string, std::string>& values) {
  if (values.count("vehicle_binds_repaired_v011")) return false;
  values["vehicle_binds_repaired_v011"] = "1";
  int sets = 0;
  for (int set = kBindGround; set < kBindSetCount; ++set) {
    int listed = 0;
    bool all_empty = true;
    for (std::string_view action : kVehicleActions) {
      const auto it = values.find("cvar." + BindCvar(set, std::string(kPrefix) + std::string(action)));
      if (it == values.end()) continue;
      ++listed;
      if (!it->second.empty()) all_empty = false;
    }
    if (listed < 10 || !all_empty) continue;
    for (std::string_view action : kVehicleActions) {
      const std::string name = BindCvar(set, std::string(kPrefix) + std::string(action));
      values.erase("cvar." + name);
      if (const auto* info = rex::cvar::GetFlagInfo(name)) rex::cvar::SetFlagByName(name, info->default_value);
    }
    ++sets;
  }
  if (sets) {
    values.erase("vehicle_binds_seeded");  // seeded again below, from the infantry's keys as they are
    REXLOG_INFO("BF bindings: {} vehicle key set(s) saved with no keys (issue 90) set back and seeded again", sets);
  }
  return true;
}

bool SeedVehicleBinds(std::map<std::string, std::string>& values) {
  const bool repaired = RepairEmptyVehicleBinds(values);
  const bool pad = SeedVehiclePad(values) || repaired;
  if (values.count("vehicle_binds_seeded")) return pad;
  // Never from keys that could not be read (issue 90): the sets keep their defaults, and the next start tries again.
  bool any_keys = false;
  for (std::string_view action : kVehicleActions)
    any_keys = any_keys || !rex::cvar::GetFlagByName(std::string(kPrefix) + std::string(action)).empty();
  if (!any_keys) {
    REXLOG_WARN("BF bindings: the infantry's keys read as none: the vehicles' keys not seeded from them this start");
    return pad;
  }
  int copied = 0;
  for (std::string_view action : kVehicleActions) {
    const std::string infantry = std::string(kPrefix) + std::string(action);
    std::string keys = rex::cvar::GetFlagByName(infantry);
    // The wheel never pressed the right bumper in a vehicle (look.cpp WheelNotch: it fires the second weapon there).
    if (action == "right_shoulder") {
      std::string kept;
      for (size_t at = 0; at <= keys.size();) {
        size_t comma = keys.find(',', at);
        if (comma == std::string::npos) comma = keys.size();
        std::string k = keys.substr(at, comma - at);
        at = comma + 1;
        while (!k.empty() && k.front() == ' ') k.erase(k.begin());
        while (!k.empty() && k.back() == ' ') k.pop_back();
        if (k.empty() || k == "WheelUp" || k == "WheelDown") continue;
        kept += (kept.empty() ? "" : ",") + k;
      }
      keys = kept;
    }
    for (int set = kBindGround; set < kBindSetCount; ++set) {
      // The infantry's sprint key is the planes' and the ground vehicles' Free-Look (Shift), not their left stick.
      if (action == "lstick_press" && (set == kBindGround || set == kBindAir)) continue;
      const std::string name = BindCvar(set, infantry);
      if (values.count("cvar." + name)) continue;
      const auto* info = rex::cvar::GetFlagInfo(name);
      if (info && info->default_value == keys) continue;
      values["cvar." + name] = keys;
      rex::cvar::SetFlagByName(name, keys);
      ++copied;
    }
  }
  values["vehicle_binds_seeded"] = "1";
  REXLOG_INFO("BF bindings: each vehicle's keys start as the infantry's ({} settings copied that were not the defaults)",
              copied);
  return true;
}

}  // namespace bf1943
