// The local player's seat as the game itself has it (0.4.1), read at the input mapping once a game tick
// (native_input.cpp): which control scheme the seat uses, which vehicle and seat it is. It replaces telling seats apart
// by a vehicle's part count (planes lost mid flight, a boat's view rig taken for a gun).
#pragma once

#include <cstdint>

namespace bf1943 {
enum class SeatKind {
  kNone,     // not known (no mapping yet, or a scheme not seen before)
  kSoldier,  // on foot (scheme 1)
  kPlane,    // a plane's pilot (scheme 3)
  kDriver,   // a land vehicle's or a boat's driver (the seat binds the throttle)
  kGunner,   // any other seat: a gunner or a passenger
  // The air raid's bombers, flown from the bunker (13 Nov 2026, the bomber run's log: scheme 3 like a plane, class
  // 820036D0, no throttle; it binds only the left stick's X to steer, fire, the camera and the menu). The mouse has
  // no part in it: "Bomber run should only have left/right movement on keyboard or controller".
  kBomber,
};

struct SeatInfo {
  uint32_t key = 0;           // changes exactly when the seat does (a count of seat changes)
  uint32_t scheme = 0;        // the controllable's scheme number
  uint32_t controllable = 0;  // guest addresses, to tell seats apart only
  uint32_t cls = 0;           // the controllable's class (its vtable)
  uint32_t vehicle = 0;
  uint32_t seat = 0;          // the seat's index in the vehicle
  uint32_t bound = 0;         // bit c: the seat binds analog concept c
  bool throttle = false;      // concept 0 bound
  bool fire = false;          // concept 7 bound
  SeatKind kind = SeatKind::kNone;
};

// The seat as of a mapping within 200 ms; false when there was none (a menu, loading).
bool LocalSeat(SeatInfo& out);
const char* SeatKindName(SeatKind kind);
// The local player is flying the air raid's bombers now.
bool BomberSeat();
}  // namespace bf1943
