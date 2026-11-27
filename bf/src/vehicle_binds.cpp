// The keyboard and mouse's binding sets for each kind of vehicle (vehicle_binds.h). The keyboard driver (the SDK's
// mnk_input_driver.cpp) asks for each bind's keys at each read (SetBindSource); in a vehicle's driver or pilot seat
// the answer is that vehicle kind's setting, else the infantry's own.

#include "vehicle_binds.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
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
    default: return kBindInfantry;  // on foot, a gunner or a passenger, a stationary gun, not known
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

bool BindInSet(int set, std::string_view infantry_cvar) {
  if (infantry_cvar == kFreeLookBind) return set == kBindGround || set == kBindAir;
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
    if (a == "left_trigger" || a == "right_shoulder") return "Second weapon";
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

bool SeedVehicleBinds(std::map<std::string, std::string>& values) {
  const bool pad = SeedVehiclePad(values);
  if (values.count("vehicle_binds_seeded")) return pad;
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
