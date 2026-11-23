// The mouse's stick: planes (0.4.0, pull request 11 of the players' repository by ThatZaliaSGuy), and vehicle guns and
// turrets (0.4.1, pull request 16 by the same).
//
// The Beachhead project's Modern mouse flight (its vehicle.gd: the pilot's mouse "flies the
// aircraft"), written into the game's input concepts (native_input.cpp), never the pad. Each mouse movement adds
// counts x sensitivity x 0.02 to a virtual stick; the stick springs back to the centre at 10 a second (Beachhead's
// flight_mouse.lerp(0, min(10 dt, 1)) each step). So a flick gives a short, firm deflection, a steady pull holds it,
// and a still mouse lets go: the mouse's speed is the plane's or the gun's turning speed.
//
// In a plane its x rolls (concept 6) and its y pitches (concept 5): mouse back is stick back, nose up. The rudder
// (concept 4) stays on A and D. 18 Nov 2026 (the user: "For planes, we want to be able to rebind the mouse's axes too,
// so players who prefer to control the planes Pitch, Roll and Yaw with either mouse or keyboard inputs can freely
// remap those"): each of the mouse's axes flies what bf_plane_mouse_x and bf_plane_mouse_y say (roll, pitch, yaw or
// nothing), and the keys do the rest (the Air vehicles keys, vehicle_binds.cpp). In a plane what the mouse writes is
// kept only where it is stronger than what the keys or a controller gave that axis (native_input.cpp), so they fly
// together. In a gun's or turret's seat (vehicle_mouse.cpp says which axes the seat has) its x
// turns the gun (concept 6) and its y raises it (concept 5): mouse up is gun up. 13 Nov 2026: the boat's gun was flown
// like this by accident and was "exactly how we want all turrets and the AA gun to work".
//
// Which seat it is comes from the game (seat_info.h), not from a vehicle's part count. A controller's own sticks take
// over while they are used. The game's own invert settings are the controller's: native_input.cpp undoes them for
// what is written here, and the mouse has its own (Invert flight, Invert vehicle).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>

#include "seat_info.h"

REXCVAR_DEFINE_BOOL(bf_plane_mouse_aim, true, "BF1943/Controls",
                    "In planes the mouse flies the plane (pitch and roll, springing back to the centre)");
REXCVAR_DEFINE_DOUBLE(bf_plane_mouse_sensitivity, 1.0, "BF1943/Controls",
                      "Mouse sensitivity in planes, 0.1 to 5 (times): 1 is full stick for about 50 counts");
REXCVAR_DEFINE_BOOL(bf_mouse_invert_y_flight, false, "BF1943/Controls",
                    "Mouse vertical inverted in planes (false: mouse back climbs, as a flight stick)");
REXCVAR_DEFINE_STRING(bf_plane_mouse_x, "roll", "BF1943/Controls",
                      "What the mouse's left and right flies in planes: roll, pitch, yaw or none");
REXCVAR_DEFINE_STRING(bf_plane_mouse_y, "pitch", "BF1943/Controls",
                      "What the mouse's forward and back flies in planes: pitch, roll, yaw or none");
REXCVAR_DEFINE_BOOL(bf_mouse_invert_x_flight, false, "BF1943/Controls",
                    "Mouse horizontal inverted in planes (false: mouse right rolls, yaws or pitches the positive way)");
// 23 Nov 2026 (TekRantGaming: "Don't forget Invert Air. That should never be tied to Invert Vehicle"): Free-Look in a
// plane (Shift held, the mouse turns the view, look.cpp) has its own up and down, no longer Invert vehicle's.
REXCVAR_DEFINE_BOOL(bf_mouse_invert_y_air_look, false, "BF1943/Controls",
                    "Mouse vertical inverted for Free-Look in planes (false: mouse up looks up)");
REXCVAR_DECLARE(bool, bf_mouse_invert_y_vehicle);       // input_settings.cpp
REXCVAR_DECLARE(double, bf_mouse_vehicle_sensitivity);  // look.cpp
REXCVAR_DECLARE(bool, bf_debug_vehicle_mouse);          // vehicle_mouse.cpp

namespace bf1943 {
unsigned VehicleMouseAxes();  // vehicle_mouse.cpp

namespace {
using Clock = std::chrono::steady_clock;
constexpr double kPerCount = 0.02;  // Beachhead: flight_mouse += relative x sensitivity x 0.02
constexpr double kReturn = 10.0;    // Beachhead: back to the centre at 10 a second

struct State {
  std::mutex mutex;
  bool active = false;
  bool plane = false;    // flying (else a gun)
  unsigned axes = 0;     // bit 0 concept 6, bit 1 concept 5, bit 2 concept 4 (a plane's yaw)
  uint32_t seat_key = 0;
  double x = 0, y = 0;   // the virtual stick, before clamping (Beachhead keeps pitch's excess)
  Clock::time_point updated{};
  Clock::time_point stick_until{};  // a controller's sticks have it until then
  Clock::time_point gun_until{};    // a gun's axes hold this long after its parts were last seen
  Clock::time_point plane_until{};  // a plane is held this long after its seat was last known (a hitch)
  float roll6 = 0, pitch5 = 0, yaw4 = 0;
  Clock::time_point steered_at{};
  Clock::time_point logged{};
} g;

float Axis(const uint8_t* p) { return float(int16_t(uint16_t(p[0]) << 8 | p[1])) / 32767.0f; }

// What a mouse axis flies in a plane (bf_plane_mouse_x, _y): the axes bit of its concept (0 for nothing).
unsigned FlownBy(const std::string& v) {
  if (v == "roll") return 1;
  if (v == "pitch") return 2;
  if (v == "yaw") return 4;
  return 0;
}

void Let_go() {
  g.active = false;
  g.axes = 0;
  g.x = g.y = 0;
  g.updated = {}, g.steered_at = {}, g.gun_until = {}, g.plane_until = {};
}
}  // namespace

// For the HUD's prompts (hud_glyphs.cpp): what the mouse flies in a plane now (1 roll, 2 pitch, 4 yaw), none while
// it flies nothing.
unsigned PlaneMouseFlies() {
  if (!REXCVAR_GET(bf_plane_mouse_aim)) return 0;
  return FlownBy(REXCVAR_GET(bf_plane_mouse_x)) | FlownBy(REXCVAR_GET(bf_plane_mouse_y));
}

// Air's own inverts, never Invert vehicle's: Free-Look's up and down in a plane (look.cpp), and the mouse's up and
// down where it flies a plane (native_input.cpp's seat look, with plane mouse flying switched off).
bool AirLookInverted() { return REXCVAR_GET(bf_mouse_invert_y_air_look); }
bool FlightMouseInverted() { return REXCVAR_GET(bf_mouse_invert_y_flight); }

void ResetPlaneMouse() {
  std::lock_guard lock(g.mutex);
  Let_go();
}

// The mouse's stick has the seat (a plane, or a gun).
bool PlaneMouseActive() {
  std::lock_guard lock(g.mutex);
  return g.active;
}
// For look.cpp: the mouse's stick has this seat (a plane, or a seat whose gun has turning parts), as of now.
bool MouseStickSeat() {
  SeatInfo seat;
  if (LocalSeat(seat) && seat.kind == SeatKind::kPlane && REXCVAR_GET(bf_plane_mouse_aim)) return true;
  return VehicleMouseAxes() != 0;
}
// For native_input.cpp: the stick for concepts 6 (roll, or a gun's turn), 5 (pitch, positive up for a gun and nose
// down for a plane, as the game has them without its own invert) and 4 (a plane's yaw), worked out within 100 ms; axes
// says which to write, plane whether it flies a plane (there the stronger of it and the game's own is kept).
bool PlaneMouseConcepts(float& roll6, float& pitch5, float& yaw4, unsigned& axes, bool& plane) {
  std::lock_guard lock(g.mutex);
  if (!g.active || Clock::now() - g.steered_at > std::chrono::milliseconds(100)) return false;
  roll6 = g.roll6, pitch5 = g.pitch5, yaw4 = g.yaw4, axes = g.axes, plane = g.plane;
  return axes != 0;
}

// From look.cpp's pad hook in a vehicle: the mouse's counts since the last call (display density taken out) and the
// game's X_INPUT_GAMEPAD (big endian, sticks at +4 left x, +6 left y, +8 right x, +10 right y), read only to see a
// controller. True in a plane or a gun's seat (the counts are taken); false in any other seat.
bool PlaneMouse(uint8_t* gamepad, double dx, double dy) {
  SeatInfo seat;
  const bool have_seat = LocalSeat(seat);
  bool plane = have_seat && seat.kind == SeatKind::kPlane && REXCVAR_GET(bf_plane_mouse_aim);
  std::lock_guard lock(g.mutex);
  const auto now = Clock::now();
  // A hitch in flight (the seat's mapping older than 200 ms: a frame of 200 ms or more) is not leaving the plane: it
  // is held for a second, as 0.4.0 held it (the review of pull request 16: the next read let go of the stick, and that
  // frame's mouse went to the rudder and the pitch as free look).
  if (plane) g.plane_until = now + std::chrono::seconds(1);
  else if (!have_seat && g.active && g.plane && now < g.plane_until && REXCVAR_GET(bf_plane_mouse_aim)) plane = true;
  const unsigned gun = plane ? 0 : VehicleMouseAxes();
  // Another seat: the stick starts centred, and nothing is carried over.
  if (g.active && have_seat && seat.key != g.seat_key) Let_go();
  // A plane's: the concepts the mouse's axes fly. With none the keys and a controller fly it: the mouse's counts are
  // taken and nothing is written (not the seat's own look either, whose concepts are the rudder and the pitch here).
  const unsigned plane_axes = FlownBy(REXCVAR_GET(bf_plane_mouse_x)) | FlownBy(REXCVAR_GET(bf_plane_mouse_y));
  if (plane && !plane_axes) {
    if (g.active) Let_go();
    return true;
  }
  unsigned axes = plane ? plane_axes : gun;
  // A gun's parts step with the game's ticks: its axes are held through a hitch.
  if (gun) g.gun_until = now + std::chrono::milliseconds(300);
  else if (!plane && g.active && !g.plane && have_seat && now < g.gun_until) axes = g.axes;
  if (!axes) {
    if (g.active) Let_go();
    return false;
  }
  if (!g.active || g.plane != plane) {
    g.active = true, g.plane = plane;
    g.x = g.y = 0;
    g.updated = now;
    g.stick_until = {};
    REXLOG_INFO("BF mouse stick: seat #{} ({}): {}", have_seat ? seat.key : 0,
                have_seat ? SeatKindName(seat.kind) : "not known",
                plane ? "flying the plane" : axes == 3 ? "turning the gun (yaw and pitch)"
                                             : axes == 1 ? "turning the gun (yaw only)" : "turning the gun (pitch only)");
  }
  if (have_seat) g.seat_key = seat.key;
  g.axes = axes;
  // A controller's own sticks have it (until let go for half a second), and the mouse's stick is centred. In a plane
  // any stick: but the keys give the left stick too (A and D the rudder, W and S the throttle), and what they gave is
  // not a controller's (12 Nov 2026: A or D held centred the mouse's stick). At a gun the right stick only (the left
  // drives a tank).
  // The keys' right stick is no controller's either (18 Nov 2026: roll or pitch on keys while the mouse yaws).
  const float lx = !plane || rex::input::mnk::LastKeyLeftStick(false) ? 0.0f : Axis(gamepad + 4);
  const float ly = !plane || rex::input::mnk::LastKeyLeftStick(true) ? 0.0f : Axis(gamepad + 6);
  const float rx = rex::input::mnk::LastKeyRightStick(false) ? 0.0f : Axis(gamepad + 8);
  const float ry = rex::input::mnk::LastKeyRightStick(true) ? 0.0f : Axis(gamepad + 10);
  if (std::max({std::fabs(lx), std::fabs(ly), std::fabs(rx), std::fabs(ry)}) > 0.3f)
    g.stick_until = now + std::chrono::milliseconds(500);
  if (now < g.stick_until) {
    g.x = g.y = 0;
    g.steered_at = {};
    g.updated = now;
    return true;
  }
  // Back toward the centre for the time since the last read, then this read's movement added.
  const double dt = std::clamp(std::chrono::duration<double>(now - g.updated).count(), 0.0, 0.1);
  g.updated = now;
  const double keep = 1.0 - std::min(kReturn * dt, 1.0);
  g.x *= keep, g.y *= keep;
  const double set = plane ? REXCVAR_GET(bf_plane_mouse_sensitivity) : REXCVAR_GET(bf_mouse_vehicle_sensitivity);
  const double sens = std::isfinite(set) ? std::clamp(set, 0.1, 5.0) : 1.0;
  const bool invert = plane ? REXCVAR_GET(bf_mouse_invert_y_flight) : REXCVAR_GET(bf_mouse_invert_y_vehicle);
  const double invert_x = plane && REXCVAR_GET(bf_mouse_invert_x_flight) ? -1.0 : 1.0;
  if (std::isfinite(dx)) g.x += dx * sens * kPerCount * invert_x;
  if (std::isfinite(dy)) g.y += dy * sens * kPerCount * (invert ? -1.0 : 1.0);
  g.x = std::clamp(g.x, -2.0, 2.0), g.y = std::clamp(g.y, -2.0, 2.0);
  // The mouse right: roll right, or the gun to the right. The mouse back (down): the stick back, a plane's nose up and
  // a gun down (concept 5 negative).
  // In a plane each axis flies what it is set to: the mouse right is the positive way (roll right, yaw right, nose
  // down), the mouse back the negative (nose up, as before; roll left, yaw left).
  if (plane) {
    double c[3] = {0, 0, 0};  // roll6, pitch5, yaw4
    auto add = [&](unsigned bit, double v) {
      if (bit) c[bit == 1 ? 0 : bit == 2 ? 1 : 2] += v;
    };
    add(FlownBy(REXCVAR_GET(bf_plane_mouse_x)), g.x);
    add(FlownBy(REXCVAR_GET(bf_plane_mouse_y)), -g.y);
    g.roll6 = float(std::clamp(c[0], -1.0, 1.0));
    g.pitch5 = float(std::clamp(c[1], -1.0, 1.0));
    g.yaw4 = float(std::clamp(c[2], -1.0, 1.0));
  } else {
    g.roll6 = float(std::clamp(g.x, -1.0, 1.0));
    g.pitch5 = float(std::clamp(-g.y, -1.0, 1.0));
    g.yaw4 = 0;
  }
  // Only a stick off its centre steers (pull request 11's fix, dropped again by pull request 16 and put back in its
  // review): a centred mouse stick writes nothing, so a controller's gentle stick (under the 0.3 that marks a
  // controller in use) is the game's, in a plane and at a gun alike.
  if (std::fabs(g.roll6) > 0.02f || std::fabs(g.pitch5) > 0.02f || std::fabs(g.yaw4) > 0.02f) g.steered_at = now;
  if (REXCVAR_GET(bf_debug_vehicle_mouse) && now - g.logged > std::chrono::milliseconds(250) &&
      (g.roll6 != 0 || g.pitch5 != 0 || g.yaw4 != 0)) {
    g.logged = now;
    REXLOG_INFO("BF mouse stick: {} roll {:.2f} pitch {:.2f} yaw {:.2f} (axes {})", plane ? "plane" : "gun", g.roll6,
                g.pitch5, g.yaw4, axes);
  }
  return true;
}
}  // namespace bf1943
