// The mouse in vehicles aims, it does not push a stick (TekRantGaming, 8 Nov 2026: "Mouse: should not be joystick emulation").
//
// How the game turns a vehicle's guns and turrets (research in the session scratchpad vehicle_aim/notes.md): each
// turning part (a tank's turret and gun, a mounted machine gun, a jeep gunner's mount) is a ChildRotationBodyData part
// with a physics body. Twice a game tick sub_822ADE38(helper, input) reads the seat's input concept for the part
// (RotationInput at data+0x34; the right stick's x is concept 6, its y concept 5), squares it unless UseLinearInput
// (data+0x44), and pushes the body with an angular impulse: the stick sets an acceleration toward a damped top speed,
// and the turret coasts. The server runs the same step with the same input, so the angle itself must not be set.
//
// So the mouse moves a target angle for each of the local seat's parts (the counts times 0.066 degrees, the on foot
// scale, times Vehicle sensitivity), and in each call the input the part reads is replaced by the one that drives the
// body to that target as fast as the vehicle allows, and stops it there (a proportional and damping controller on the
// body's angle and angular velocity, the game's square undone). The server sees ordinary input values. A controller's
// stick takes over at once when it is used (the target follows the turret). Planes keep the stick push: their pitch
// and roll are forces of the flight model with no angle to aim (look.cpp LookPad).
//
// Only the local player's seat runs these steps in the player's game (vaim2: the helpers ran for the seat sat in).

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DECLARE(double, bf_mouse_vehicle_sensitivity);  // look.cpp
REXCVAR_DECLARE(bool, bf_mouse_invert_y);
REXCVAR_DEFINE_BOOL(bf_mouse_vehicle_aim, true, "BF1943/Controls",
                    "The mouse aims vehicle guns and turrets at a target angle (false: it pushes the right stick)");

namespace bf1943 {
bool TakeVehicleMouse(double& dx, double& dy, double& idle);  // look.cpp
bool GameMenuOpen();                                         // game_menu.cpp

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRadiansPerCount = 0.066 * kPi / 180.0;  // look.cpp's on foot scale

uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
float LoadF(const uint8_t* base, uint32_t a) { return std::bit_cast<float>(Load32(base, a)); }
void StoreF(uint8_t* base, uint32_t a, float v) {
  const uint32_t b = std::bit_cast<uint32_t>(v);
  uint8_t* p = base + a;
  p[0] = uint8_t(b >> 24), p[1] = uint8_t(b >> 16), p[2] = uint8_t(b >> 8), p[3] = uint8_t(b);
}
bool Guest(uint32_t a) { return a >= 0x10000 && a < 0xF0000000; }

struct Part {
  double target = 0;
  bool engaged = false;  // the mouse has the part (else its target follows the turret)
  std::chrono::steady_clock::time_point seen{}, seen_before{};
  double last_angle = 0, last_input = 0;
  double rate = 0;  // the target's speed (radians a second, smoothed): fed forward
};
std::mutex g_mutex;
std::map<uint32_t, Part> g_parts;  // by helper
double g_dx = 0, g_dy = 0;         // mouse radians not yet given to the parts (yaw, pitch)
std::chrono::steady_clock::time_point g_taken{};
// The input each concept should get (5 pitch, 6 yaw) and when it was worked out; the pad hook turns it into the right
// stick, before the game reads it, so the server gets the same input as the player's game (writing the input object
// in the step moved only the player's own copy: the server's corrections pulled the turret back, run vmouse2).
float g_want[8] = {};
std::chrono::steady_clock::time_point g_want_at[8] = {};
bool g_real_stick = false;  // the controller's own right stick is pushed

// The body's signed angle about its axis (the game's getter sub_82533BB0: 2 acos(w), signed by q.xyz and the axis).
double Angle(const uint8_t* base, uint32_t body) {
  const double qx = LoadF(base, body + 0x10), qy = LoadF(base, body + 0x14), qz = LoadF(base, body + 0x18);
  const double qw = std::clamp<double>(LoadF(base, body + 0x1C), -1.0, 1.0);
  const double ax = LoadF(base, body + 0x90), ay = LoadF(base, body + 0x94), az = LoadF(base, body + 0x98);
  double a = 2.0 * std::acos(qw);
  if (qx * ax + qy * ay + qz * az < 0) a = -a;
  while (a > kPi) a -= 2 * kPi;
  while (a <= -kPi) a += 2 * kPi;
  return a;
}
double AngularSpeed(const uint8_t* base, uint32_t body) {
  return LoadF(base, body + 0x70) * LoadF(base, body + 0x90) + LoadF(base, body + 0x74) * LoadF(base, body + 0x94) +
         LoadF(base, body + 0x78) * LoadF(base, body + 0x98);
}
double Wrap(double a) {
  while (a > kPi) a -= 2 * kPi;
  while (a <= -kPi) a += 2 * kPi;
  return a;
}

}  // namespace
}  // namespace bf1943

// sub_822ADE38(helper, input): one turning part's input step (see the top).
REX_EXTERN(__imp__sub_822ADE38);
REX_HOOK_RAW(sub_822ADE38) {
  using namespace bf1943;
  const uint32_t helper = ctx.r3.u32, input = ctx.r4.u32;
  if (REXCVAR_GET(bf_mouse_vehicle_aim) && Guest(helper) && Guest(input) && !GameMenuOpen()) {
    const uint32_t data = Load32(base, helper + 0x18), body = Load32(base, helper + 4);
    const uint32_t values = Load32(base, input + 0x160);
    const uint32_t input_concept = Guest(data) ? Load32(base, data + 0x34) : 99;
    if (Guest(body) && Guest(values) && (input_concept == 5 || input_concept == 6)) {
      std::lock_guard lock(g_mutex);
      const auto now = std::chrono::steady_clock::now();
      // The mouse's movement since the last call, as yaw and pitch radians (shared by the seat's parts: taken once
      // a tick's worth, then each part takes its axis).
      double dx = 0, dy = 0, idle = 1e9;
      const bool mouse = TakeVehicleMouse(dx, dy, idle);
      if (now - g_taken > std::chrono::milliseconds(500)) g_dx = g_dy = 0;  // out of the seat meanwhile
      const double k = kRadiansPerCount * std::clamp(REXCVAR_GET(bf_mouse_vehicle_sensitivity), 0.05, 10.0);
      g_dx += dx * k, g_dy += dy * k * (REXCVAR_GET(bf_mouse_invert_y) ? -1.0 : 1.0);
      Part& p = g_parts[helper];
      const double angle = Angle(base, body), speed = AngularSpeed(base, body);
      if (now - p.seen > std::chrono::seconds(1)) p.engaged = false, p.rate = 0;  // a new seat or a respawn
      p.seen_before = p.seen;
      p.seen = now;
      // A controller's stick in use (a value the mouse did not cause): the stick has it, the target follows.
      const bool stick = g_real_stick && idle > 0.25;
#if defined(BF_DEV_TOOLS)
      if (p.engaged && (!mouse || stick) && std::getenv("BF_DEV_VMOUSE"))
        REXLOG_INFO("BF vehicle mouse: part {:08X} let go ({})", helper, !mouse ? "no mouse look" : "the stick");
#endif
      if (!mouse || stick) p.engaged = false;
      if (!p.engaged) p.target = angle;
      // The mouse's share for this part: right is the stick's right (concept 6 positive), which turns the angle
      // down; down is the stick's down (concept 5 negative), which turns it up.
      double& share = input_concept == 6 ? g_dx : g_dy;
      const double dt = std::chrono::duration<double>(now - p.seen_before).count();
      double moved = 0;
      if (mouse && share != 0) {
        p.engaged = true;
        moved = -share;
        p.target = Wrap(p.target - share);
        share = 0;
      }
      if (dt > 0 && dt < 0.2) p.rate += (moved / dt - p.rate) * std::min(1.0, dt * 12.0);
      if (p.engaged) {
        // A constrained part's target stays within its limits (degrees in the data).
        if (Load32(base, data + 0x2C) >> 24) {
          const double lo = LoadF(base, data + 0x14) * kPi / 180.0, hi = LoadF(base, data + 0x18) * kPi / 180.0;
          if (lo < hi) p.target = std::clamp(p.target, lo, hi);
        }
        // The target never runs more than half a radian ahead of the part (a gun at its stop does not wind up).
        p.target = angle + std::clamp(Wrap(p.target - angle), -0.5, 0.5);
        // Toward the target, damped by the speed, with the target's own speed fed forward (the part's top speed is
        // about 1.2 radians a second at full input: run vmouse1): positive input turns the angle down.
        const double err = Wrap(p.target - angle);
        double s = std::clamp(-9.0 * err + 0.6 * speed - p.rate / 1.2, -1.0, 1.0);
        if (std::fabs(err) < 0.002 && std::fabs(speed) < 0.02 && std::fabs(p.rate) < 0.01) s = 0;
        if (!(Load32(base, data + 0x44) >> 24)) s = std::copysign(std::sqrt(std::fabs(s)), s);  // the game squares it
        // Stalled short of the target: inputs this small are below the part's threshold (1 degree was left at an input
        // of 0.16, run vmouse6), so it gets the least push that moves it.
        if (std::fabs(err) > 0.006 && std::fabs(speed) < 0.01) s = std::copysign(std::max(std::fabs(s), 0.42), s == 0 ? -err : s);
        // A call that sees the body at rest at zero (a tick replayed from the server's state, every few calls in
        // run vmouse1) keeps the last input rather than answering a state that is not the part's.
        if (angle == 0 && speed == 0 && std::fabs(p.last_angle) > 0.02) s = p.last_input;
        else p.last_angle = angle, p.last_input = s;
        g_want[input_concept] = float(s), g_want_at[input_concept] = now;
#if defined(BF_DEV_TOOLS)
        static const bool dev = std::getenv("BF_DEV_VMOUSE") != nullptr;
        static auto logged = now;
        if (dev && now - logged > std::chrono::milliseconds(250)) {
          logged = now;
          REXLOG_INFO("BF vehicle mouse: part {:08X} concept {} angle {:.4f} target {:.4f} speed {:.3f} input {:.3f}",
                      helper, input_concept, angle, p.target, speed, s);
        }
#endif
      }
      if (g_parts.size() > 64) g_parts.clear();
      g_taken = now;
    }
  }
  __imp__sub_822ADE38(ctx, base);
}

namespace bf1943 {
// For look.cpp's pad hook: the right stick the vehicle mouse wants (x for concept 6, y for concept 5, -1 to 1 as the
// game reads them after its dead zone), given the controller's own (`real_x`, `real_y`). False when the mouse does
// not have the seat's parts (then the stick stays the player's).
bool VehicleMouseStick(float real_x, float real_y, float* x, float* y) {
  std::lock_guard lock(g_mutex);
  g_real_stick = std::fabs(real_x) > 0.3f || std::fabs(real_y) > 0.3f;
  const auto now = std::chrono::steady_clock::now();
  const bool yaw = now - g_want_at[6] < std::chrono::milliseconds(150);
  const bool pitch = now - g_want_at[5] < std::chrono::milliseconds(150);
  if (!yaw && !pitch) return false;
  *x = yaw ? g_want[6] : 0.0f;
  *y = pitch ? g_want[5] : 0.0f;
  return true;
}
}  // namespace bf1943
