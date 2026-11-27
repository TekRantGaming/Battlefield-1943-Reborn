// The keyboard and mouse's binding sets (the user, 18 Nov 2026: "We also need to make separate keymapping controls for
// Ground, air, and boat vehicles (turrets stay on Infantry control layer)"): the SDK's keybind_* settings are the
// infantry's, and each kind of vehicle has its own copy (keybind_ground_*, keybind_air_*, keybind_boat_*), used while
// the local player sits in one. On foot the infantry's. (Gunners, passengers and the stationary guns had the
// infantry's until issue 78, 27 Nov 2026: the user, "Vehicle's keys entirely".)
// The user again: "And then we need to do a version of all of this the same for controller/gamepad remapping too": the
// controller's button remapping (bf_remap_*, input_settings.cpp) has a copy for each kind too (bf_remap_ground_* ...),
// and so has its stick remapping (bf_axis_*: which of the controller's stick axes the game gets on each of its own),
// so a plane's pitch, roll and yaw can each be on any stick.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace bf1943 {
enum BindSet : int { kBindInfantry = 0, kBindGround, kBindAir, kBindBoat, kBindSetCount };

// "Infantry", "Ground vehicles", "Air vehicles", "Boats".
const char* BindSetName(int set);
// The actions a vehicle's set has its own keys for (the infantry's settings, "keybind_a" ...): every controller button
// and stick but Start (the game menu) and Guide; chat and voice keep one set too.
bool VehicleBindAction(std::string_view infantry_cvar);
// Free-Look (vehicle_views.cpp): the planes' and the ground vehicles' own action, no button of the controller's; its
// rows use this name ("keybind_free_look"), its settings are keybind_air_free_look and keybind_ground_free_look.
constexpr const char* kFreeLookBind = "keybind_free_look";
// The bindings' rows of a set: the infantry's actions in its tab, a vehicle set's own actions in theirs (Free-Look
// only in the planes' and the ground vehicles').
bool BindInSet(int set, std::string_view infantry_cvar);
// The setting that holds an action's keys in a set: "keybind_a" in the ground set is "keybind_ground_a"; the
// infantry's own name for the infantry and for an action the set does not have.
std::string BindCvar(int set, std::string_view infantry_cvar);
// The set the local player's seat uses now: any seat of a land vehicle (a stationary gun too) or a boat, a plane's
// pilot (the air raid's bombers too); on foot or no seat known the infantry's.
int ActiveBindSet();
// An action's keys as they are now (the active set's), for the wheel (look.cpp) and the HUD's key prompts
// (hud_glyphs.cpp).
std::string ActiveBind(std::string_view infantry_cvar);
// What an action is called in a set's tab of the bindings (in a plane the right stick's keys pitch and roll, ...);
// `label` (the infantry's) when the set calls it nothing else.
const char* BindLabel(int set, std::string_view infantry_cvar, const char* label);
// The same for the controller's page, by the game button the action is ("lt", "rb", ...): the game's own controller
// layouts (in a plane the left trigger is the throttle, where the keyboard's zoom key drops bombs).
const char* PadLabel(int set, std::string_view pad_action, const char* label);

// The controller's button remapping of a set: "bf_remap_a" for the infantry's A, "bf_remap_air_a" for the air
// vehicles' (`button` a kButtons name of input_settings.cpp: "a", "lb", "lt", "up", ...).
std::string RemapCvar(int set, std::string_view button);
// The controller's stick remapping: the game's stick axes ("lx", "ly", "rx", "ry"), and for each the setting that says
// which of the controller's axes it gets ("lx", "-lx" inverted, ..., "none"); "bf_axis_rx", "bf_axis_air_rx", ...
constexpr const char* kStickAxes[4] = {"lx", "ly", "rx", "ry"};
std::string AxisCvar(int set, std::string_view axis);
// What the game's stick axis (0 to 3, kStickAxes) does in a set, with the game's own Normal layouts.
const char* AxisLabel(int set, int axis);
// The keyboard driver reads its binds through the active set (bf1943_app.h, at the start).
void InstallVehicleBinds();
// The seat keys (issue 80): keybind_seat_1 to keybind_seat_9, one setting each for every vehicle (keys 1 to 9 by
// default), their rows in the ground vehicles' and the boats' tabs. SeatKeysFrame: the UI thread, each frame in a match (comms_ui.cpp): a
// press asks for that seat. SeatKeysPad: the pad hook in a vehicle seat (auto_join.cpp), after the controller settings:
// presses the game's Change seat until the seat asked for is reached, or found taken.
bool SeatKeyBind(std::string_view cvar);
void SeatKeysFrame();
void SeatKeysPad(uint8_t* gamepad);
// At the start, once the saved settings are applied (game_menu.cpp): the first run with the sets copies the infantry's
// keys as they are into each vehicle's set, so every set starts as the keys were; the wheel stays off the right
// bumper in a vehicle, as before (it fires the second weapon there). The controller's button remapping the same, once
// too. True when `values` (the menu's saved settings) changed.
bool SeedVehicleBinds(std::map<std::string, std::string>& values);
}  // namespace bf1943
