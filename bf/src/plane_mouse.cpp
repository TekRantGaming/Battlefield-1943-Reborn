// Mouse flying in planes (0.4.0): the Beachhead project's Modern mouse flight (E:\Projects\Beachhead, vehicle.gd: the
// pilot's mouse "flies the aircraft"), written into the game's input concepts (native_input.cpp), never the pad.
//
// Each mouse movement adds counts x sensitivity x 0.02 to a virtual stick; the stick springs back to the centre at 10
// a second (Beachhead's flight_mouse.lerp(0, min(10 dt, 1)) each step). Its x rolls (concept 6), its y pitches
// (concept 5): mouse back is stick back, nose up, as a flight stick; the game's own Vertical fly setting (FlipY) applies
// after, as to a controller. The rudder (concept 4) stays on A and D. So a flick gives a short, firm deflection, a
// steady pull holds it, and a still mouse lets the plane fly straight: direct control of the surfaces, no aim point.
// (12 Nov 2026: the War Thunder style aim point before it felt "like a joystick, or incremental movement", and its
// full deflections back and forth could not be flown.)
//
// The plane is the local player's own vehicle (tick_blend.cpp OwnVehicle), known by its part count (bf_plane_parts:
// the Corsair's tree has 57, the Zero's 52). A controller's own sticks take over while they are used.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(bf_plane_mouse_aim, true, "BF1943/Controls",
                    "In planes the mouse flies the plane (pitch and roll, springing back to the centre)");
REXCVAR_DEFINE_STRING(bf_plane_parts, "57,52", "BF1943/Controls",
                      "Part counts of the vehicles flown with the mouse, comma separated (the Corsair's is 57, the "
                      "Zero's 52; bf_debug_vehicle_mouse logs each vehicle's)");
REXCVAR_DEFINE_DOUBLE(bf_plane_mouse_sensitivity, 1.0, "BF1943/Controls",
                      "Mouse sensitivity in planes, 0.1 to 5 (times): 1 is full stick for about 50 counts");
REXCVAR_DECLARE(bool, bf_mouse_invert_y_vehicle);  // input_settings.cpp
REXCVAR_DECLARE(bool, bf_debug_vehicle_mouse);     // vehicle_mouse.cpp

namespace bf1943 {
bool OwnVehicle(float rows[4][3], int& parts, bool& scored, uint64_t& tick);  // tick_blend.cpp

namespace {
using Clock = std::chrono::steady_clock;
constexpr double kPerCount = 0.02;  // Beachhead: flight_mouse += relative x sensitivity x 0.02
constexpr double kReturn = 10.0;    // Beachhead: back to the centre at 10 a second

struct State {
  std::mutex mutex;
  bool active = false;
  double x = 0, y = 0;  // the virtual stick, before clamping (Beachhead keeps pitch's excess)
  Clock::time_point updated{};
  Clock::time_point stick_until{};  // a controller's sticks fly until then
  int last_parts = -1;              // for the log: the vehicle last seen
  float roll6 = 0, pitch5 = 0;
  Clock::time_point steered_at{};
  Clock::time_point logged{};
} g;

bool IsPlane(int parts) {
  std::stringstream list(REXCVAR_GET(bf_plane_parts));
  std::string item;
  while (std::getline(list, item, ','))
    if (!item.empty() && std::atoi(item.c_str()) == parts) return true;
  return false;
}

float Axis(const uint8_t* p) { return float(int16_t(uint16_t(p[0]) << 8 | p[1])) / 32767.0f; }
}  // namespace

void ResetPlaneMouse() {
  std::lock_guard lock(g.mutex);
  g.active = false;
  g.last_parts = -1;
  g.x = g.y = 0;
  g.updated = {}, g.steered_at = {};
}

// For native_input.cpp and vehicle_mouse.cpp: the local player flies a plane (its turning parts, the control surfaces,
// are not turrets).
bool PlaneMouseActive() {
  std::lock_guard lock(g.mutex);
  return g.active;
}
// For native_input.cpp: the stick for concepts 6 (roll) and 5 (pitch, before the game's FlipY), worked out within
// 100 ms. The rudder (4) is the keys'.
bool PlaneMouseConcepts(float& roll6, float& pitch5, float& yaw4) {
  std::lock_guard lock(g.mutex);
  if (!g.active || Clock::now() - g.steered_at > std::chrono::milliseconds(100)) return false;
  roll6 = g.roll6, pitch5 = g.pitch5, yaw4 = 0.0f;
  return true;
}

// From look.cpp's pad hook in a vehicle: the mouse's counts since the last call (display density taken out) and the
// game's X_INPUT_GAMEPAD (big endian, sticks at +4 left x, +6 left y, +8 right x, +10 right y), read only to see a
// controller. True in a plane (the counts are taken); false in any other seat.
bool PlaneMouse(uint8_t* gamepad, double dx, double dy) {
  if (!REXCVAR_GET(bf_plane_mouse_aim)) return false;
  float rows[4][3];
  int parts = 0;
  bool scored = false;
  uint64_t tick = 0;
  const bool have = OwnVehicle(rows, parts, scored, tick);
  std::lock_guard lock(g.mutex);
  const auto now = Clock::now();
  const int seen = have ? parts : 0;
  if (seen != g.last_parts) {
    g.last_parts = seen;
    REXLOG_INFO("BF plane mouse: seated in {} ({}){}", have ? "a vehicle of " + std::to_string(parts) + " parts"
                                                            : "a vehicle not found yet",
                have ? (scored ? "moving with the seat" : "nearest the seat") : "-",
                have && IsPlane(parts) ? ": a plane, mouse flying on" : "");
  }
  // Once flying, a plane stays one until the seat is left (look.cpp calls ResetPlaneMouse): the vehicle found from the
  // chase camera, 20 m behind, was lost now and then or was something nearer (12 Nov 23:18: "not found yet", then a
  // 13 part vehicle, mid flight), and each time turret aim and seat look wrote pitch and roll, crossing the inputs.
  if (!g.active && (!have || !IsPlane(parts))) return false;
  if (!g.active) {
    g.active = true;
    g.x = g.y = 0;
    g.updated = now;
    g.stick_until = {};
  }
  // A controller's own sticks fly (until let go for half a second), and the mouse's stick is centred. The keys give the
  // left stick too (A and D the rudder, W and S the throttle): what they gave is not a controller's (12 Nov 2026: A or
  // D held centred the mouse's stick, so the rudder and the mouse could not be used together).
  const float lx = rex::input::mnk::LastKeyLeftStick(false) ? 0.0f : Axis(gamepad + 4);
  const float ly = rex::input::mnk::LastKeyLeftStick(true) ? 0.0f : Axis(gamepad + 6);
  if (std::max({std::fabs(lx), std::fabs(ly), std::fabs(Axis(gamepad + 8)), std::fabs(Axis(gamepad + 10))}) > 0.3f)
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
  const double sens = std::clamp(REXCVAR_GET(bf_plane_mouse_sensitivity), 0.1, 5.0);
  if (std::isfinite(dx)) g.x += dx * sens * kPerCount;
  if (std::isfinite(dy)) g.y += dy * sens * kPerCount * (REXCVAR_GET(bf_mouse_invert_y_vehicle) ? -1.0 : 1.0);
  g.x = std::clamp(g.x, -2.0, 2.0), g.y = std::clamp(g.y, -2.0, 2.0);
  // Roll right for the mouse right; the mouse back pulls the stick back (concept 5 negative), nose up.
  g.roll6 = float(std::clamp(g.x, -1.0, 1.0));
  g.pitch5 = float(std::clamp(-g.y, -1.0, 1.0));
  g.steered_at = now;
  if (REXCVAR_GET(bf_debug_vehicle_mouse) && now - g.logged > std::chrono::milliseconds(250)) {
    g.logged = now;
    REXLOG_INFO("BF plane mouse: stick roll {:.2f} pitch {:.2f}, tick {}", g.roll6, g.pitch5, tick);
  }
  return true;
}
}  // namespace bf1943
