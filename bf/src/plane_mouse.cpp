// Mouse flying in planes (0.4.0, pull request 11 of the players' repository by ThatZaliaSGuy): the Beachhead
// project's Modern mouse flight (its vehicle.gd: the pilot's mouse "flies the aircraft"), written into the game's
// input concepts (native_input.cpp), never the pad.
//
// Each mouse movement adds counts x sensitivity x 0.02 to a virtual stick; the stick springs back to the centre at 10
// a second (Beachhead's flight_mouse.lerp(0, min(10 dt, 1)) each step). Its x rolls (concept 6), its y pitches
// (concept 5): mouse back is stick back, nose up, as a flight stick; the game's own Vertical fly setting (FlipY) applies
// after, as to a controller. The rudder (concept 4) stays on A and D. So a flick gives a short, firm deflection, a
// steady pull holds it, and a still mouse lets the plane fly straight: direct control of the surfaces, no aim point.
// (12 Nov 2026: the War Thunder style aim point before it felt "like a joystick, or incremental movement", and its
// full deflections back and forth could not be flown.)
//
// Which vehicle is a plane is the game's own data: the vehicle the local player drives has wings (the hook below).
// The pull request went by the part count of the tree nearest the seat (57 the Corsair, "52 the Zero"), and 52 parts
// is a landing boat: in its driver's seat the mouse wrote roll and pitch and the view could not be turned (run v52a
// of 13 Nov 2026, Wake Island's carrier). A controller's own sticks take over while they are used.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(bf_plane_mouse_aim, true, "BF1943/Controls",
                    "In planes the mouse flies the plane (pitch and roll, springing back to the centre); false: it "
                    "looks around, as in a driver's seat");
REXCVAR_DEFINE_DOUBLE(bf_plane_mouse_sensitivity, 1.0, "BF1943/Controls",
                      "Mouse sensitivity in planes, 0.1 to 5 (times): 1 is full stick for about 50 counts");
REXCVAR_DECLARE(bool, bf_debug_vehicle_mouse);     // vehicle_mouse.cpp

namespace bf1943 {
bool VehicleMouseInverted();  // vehicle_mouse.cpp: Mouse vertical in vehicles (same as on foot, normal, inverted)
bool OwnVehicleRows(float rows[4][3]);  // tick_blend.cpp: the own vehicle's base rows (left, up, forward, position)

namespace {
using Clock = std::chrono::steady_clock;
constexpr double kPerCount = 0.02;  // Beachhead: flight_mouse += relative x sensitivity x 0.02
constexpr double kReturn = 10.0;    // Beachhead: back to the centre at 10 a second

// Every vehicle's physics object has the same class (vtable 820344D0: the Corsair, a jeep, a Sherman and a landing
// boat, runs planem1, vehvt1 and pchk1 of 11 Nov 2026), and only a plane's has wings: its vector at +0x60 holds its
// aerodynamic surfaces (six in the Corsair, class vtable 820341C0), empty in the jeep (runs planeyaw2, jeepdump1).
// The word at +4 is not a kind to go by: the Corsair had 10 there in most runs and 4 in one (planeyaw1); it is logged.
constexpr uint32_t kVehiclePhysicsVtable = 0x820344D0, kWingVtable = 0x820341C0;
// The step runs about 45 times a second while the player drives; one older than this is a vehicle left.
constexpr auto kStepFresh = std::chrono::milliseconds(300);

struct State {
  std::mutex mutex;
  bool active = false;
  double x = 0, y = 0;  // the virtual stick, before clamping (Beachhead keeps pitch's excess)
  Clock::time_point updated{};
  Clock::time_point stick_until{};  // a controller's sticks fly until then
  float roll6 = 0, pitch5 = 0;
  Clock::time_point steered_at{};
  Clock::time_point logged{};
  // The vehicle the local player drives, from its physics input step: whether it has wings, when it last stepped
  // with a driver's input, and its component (for the log).
  bool wings = false;
  uint32_t component = 0;
  Clock::time_point step_at{};
  uint32_t logged_component = 0;  // bf_debug_vehicle_mouse: the last step logged
  bool logged_idle = false;
  Clock::time_point step_logged_at{};
} g;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
bool Guest(uint32_t a) { return a >= 0x10000 && a < 0xF0000000; }

float Axis(const uint8_t* p) { return float(int16_t(uint16_t(p[0]) << 8 | p[1])) / 32767.0f; }

// A physics object's wings (0: no plane).
int Wings(const uint8_t* base, uint32_t phys) {
  const uint32_t begin = Load32(base, phys + 0x60), end = Load32(base, phys + 0x64);
  if (!Guest(begin) || end <= begin || end - begin > 0x100) return 0;
  const uint32_t wing = Load32(base, begin);
  return Guest(wing) && Load32(base, wing) == kWingVtable ? int((end - begin) / 4) : 0;
}

// Under g.mutex: the local player drives a plane now.
bool PlaneNow(Clock::time_point now) { return g.wings && now - g.step_at < kStepFresh; }
}  // namespace
}  // namespace bf1943

// sub_822A6B18(component, input): a vehicle's physics input step. In the player's game it runs for the vehicle the
// local player drives (run planem1 of 11 Nov 2026: none on foot, none for parked vehicles; run v52b of 13 Nov: it
// stopped when the driver changed to a gunner's seat), about 45 times a second. component+0x14 is the physics object;
// component+0x1C set: no input (the step of a vehicle without its driver). Not seen yet: whether another player's
// vehicle steps here too (never with one player on the server; the log below would say "another component").
REX_EXTERN(__imp__sub_822A6B18);
REX_HOOK_RAW(sub_822A6B18) {
  using namespace bf1943;
  const uint32_t comp = ctx.r3.u32;
  if (Guest(comp)) {
    const uint32_t phys = Load32(base, comp + 0x14);
    if (Guest(phys) && Load32(base, phys) == kVehiclePhysicsVtable) {
      const int wings = Wings(base, phys);
      const bool idle = base[comp + 0x1C] != 0;
      std::lock_guard lock(g.mutex);
      const auto now = Clock::now();
      if (!idle) g.wings = wings > 0, g.component = comp, g.step_at = now;
      // bf_debug_vehicle_mouse: each vehicle that steps, once (and again every 5 s: two components in turns would be
      // two vehicles stepping, which the detection does not expect).
      if (REXCVAR_GET(bf_debug_vehicle_mouse) &&
          (comp != g.logged_component || idle != g.logged_idle || now - g.step_logged_at > std::chrono::seconds(5))) {
        REXLOG_INFO("BF plane mouse: vehicle step, component {:08X}, {} wings{}, word at +4 {}{}{}", comp, wings,
                    wings ? " (a plane)" : "", Load32(base, phys + 4), idle ? ", no input" : "",
                    g.logged_component && comp != g.logged_component && now - g.step_logged_at < kStepFresh
                        ? " (another component stepped a moment ago)"
                        : "");
        g.logged_component = comp, g.logged_idle = idle, g.step_logged_at = now;
      }
    }
  }
  __imp__sub_822A6B18(ctx, base);
}

namespace bf1943 {
void ResetPlaneMouse() {
  std::lock_guard lock(g.mutex);
  g.active = false;
  g.x = g.y = 0;
  g.updated = {}, g.steered_at = {};
}

// For native_input.cpp and vehicle_mouse.cpp: the local player flies a plane with the mouse.
bool PlaneMouseActive() {
  std::lock_guard lock(g.mutex);
  return g.active;
}
// For vehicle_mouse.cpp: the local player's vehicle is a plane, whoever flies it (the mouse, a controller, the keys):
// its turning parts are its control surfaces, not turrets.
bool PlaneSeat() {
  std::lock_guard lock(g.mutex);
  return g.active || PlaneNow(Clock::now());
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
  std::lock_guard lock(g.mutex);
  const auto now = Clock::now();
  // Once flying, a plane stays one until the seat is left (look.cpp calls ResetPlaneMouse): a step missed for a
  // moment (a hitch) must not hand pitch and roll to the seat's look in mid flight.
  if (!g.active && !PlaneNow(now)) return false;
  if (!g.active) {
    g.active = true;
    g.x = g.y = 0;
    g.updated = now;
    g.stick_until = {};
    REXLOG_INFO("BF plane mouse: in a plane (component {:08X}), mouse flying on", g.component);
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
  if (std::isfinite(dy)) g.y += dy * sens * kPerCount * (VehicleMouseInverted() ? -1.0 : 1.0);
  g.x = std::clamp(g.x, -2.0, 2.0), g.y = std::clamp(g.y, -2.0, 2.0);
  // Roll right for the mouse right; the mouse back pulls the stick back (concept 5 negative), nose up.
  g.roll6 = float(std::clamp(g.x, -1.0, 1.0));
  g.pitch5 = float(std::clamp(-g.y, -1.0, 1.0));
  // Only a stick that is off its centre steers. At the centre the pad's own values stay: written always, a zero
  // replaced a controller's gentle input (under the 0.3 that marks its sticks as in use, over the game's 0.15 dead
  // zone) half a second after its last larger one (found in the review of the pull request, 13 Nov 2026).
  const bool off_centre = std::fabs(g.x) > 0.01 || std::fabs(g.y) > 0.01;
  g.steered_at = off_centre ? now : Clock::time_point{};
  if (REXCVAR_GET(bf_debug_vehicle_mouse) && now - g.logged > std::chrono::milliseconds(250)) {
    g.logged = now;
    // With what the plane does, from its body's rows (left, up, forward): its bank to the right (the left wing's
    // rise), its nose above the horizon, its heading and its height.
    float rows[4][3];
    if (OwnVehicleRows(rows)) {
      const double deg = 180.0 / 3.14159265358979323846;
      REXLOG_INFO("BF plane mouse: stick roll {:.2f} pitch {:.2f}; the plane banks {:.1f} right, nose {:.1f} up, "
                  "heading {:.0f}, height {:.1f}",
                  g.roll6, g.pitch5, std::asin(std::clamp(double(rows[0][1]), -1.0, 1.0)) * deg,
                  std::asin(std::clamp(double(rows[2][1]), -1.0, 1.0)) * deg,
                  std::atan2(double(rows[2][0]), double(rows[2][2])) * deg, rows[3][1]);
    } else {
      REXLOG_INFO("BF plane mouse: stick roll {:.2f} pitch {:.2f}", g.roll6, g.pitch5);
    }
  }
  return true;
}
}  // namespace bf1943
