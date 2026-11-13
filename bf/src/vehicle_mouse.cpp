// The mouse in vehicles aims, it does not push a stick (TekRantGaming, 8 Nov 2026: "Mouse: should not be joystick emulation").
//
// How the game turns a vehicle's guns and turrets (research in the session scratchpad vehicle_aim/notes.md): each
// turning part (a tank's turret and gun, a mounted machine gun, a jeep gunner's mount) is a ChildRotationBodyData part
// with a physics body. Twice a game tick sub_822ADE38(helper, input) reads the seat's input concept for the part
// (RotationInput at data+0x34; the right stick's x is concept 6, its y concept 5), squares it unless UseLinearInput
// (data+0x44), and pushes the body with an angular impulse: the stick sets an acceleration toward a damped top speed,
// and the turret coasts. The server runs the same step with the same input, so the angle itself must not be set.
//
// Tight aim (12 Nov 2026): the target leads the part by at most kMaxLead, and is put back on the part at a stop.
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
REXCVAR_DECLARE(bool, bf_mouse_invert_y_vehicle);    // input_settings.cpp
REXCVAR_DEFINE_BOOL(bf_mouse_vehicle_aim, true, "BF1943/Controls",
                    "The mouse aims vehicle guns and turrets at a target angle (false: it pushes the right stick)");
REXCVAR_DEFINE_BOOL(bf_debug_vehicle_mouse, false, "BF1943/Controls",
                    "Log vehicle mouse aim to bf1943.log (turret steps, dropped movement, stops, plane commands)");

namespace bf1943 {
bool TakeVehicleMouse(double& dx, double& dy, double& idle);  // look.cpp
bool PlaneMouseActive();                                     // plane_mouse.cpp
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
  uint32_t body = 0;
  bool engaged = false;  // the mouse has the part (else its target follows the turret)
  std::chrono::steady_clock::time_point seen{}, seen_before{};
  std::chrono::steady_clock::time_point stalled{};  // pushed without moving since (a stop, or blocked)
  bool measured = false;   // its angle has been seen to change: the angle and speed read are this part's
  bool open_loop = false;  // pushed hard and never seen to move: driven by the mouse's speed alone
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
std::chrono::steady_clock::time_point g_axis_seen[8] = {};
bool g_real_stick = false;  // the controller's own right stick is pushed

// The parts step once or twice a 30 Hz tick. A command older than this is stale. 50 ms (one and a half ticks)
// dropped the mouse and fell back to the stick push whenever a frame hitched (12 Nov 2026: "locking up").
constexpr auto kStale = std::chrono::milliseconds(120);
// How far the target may lead the part (radians, about 6 degrees). The part turns at most about 1.2 radians a second
// (run vmouse1), so this is under a tenth of a second of turning: the gun stops when the mouse stops. Mouse movement
// faster than the part can follow is dropped rather than queued. A 2.8 radian lead kept a turret turning for seconds
// after the mouse stopped, and reversing first had to unwind the queue, which felt like a stick, not a mouse.
constexpr double kMaxLead = 0.10;
// Pushed hard without moving for this long: the part is at a stop (or blocked), so the target is put back on it.
// Otherwise the target wound up past the stop and the mouse had to unwind it before the gun moved again.
constexpr auto kStallReset = std::chrono::milliseconds(300);
// The input worked out in a step reaches the part a tick later (the pad hook writes it, the next tick reads it), so the
// error is taken against where the part will be by then (run 12 Nov 16:39: the Sherman overshot back and forth).
constexpr double kDelay = 1.0 / 30.0;
constexpr double kTopSpeed = 1.2;  // radians a second at full input (run vmouse1; 1.24 measured 12 Nov 16:39)

// Diagnostics for test runs (bf_debug_vehicle_mouse): what was dropped by the lead limit and how often a stop was hit.
double g_dbg_dropped[8] = {};
int g_dbg_stalls[8] = {};

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
  return std::remainder(a, 2 * kPi);
}

}  // namespace
}  // namespace bf1943

// sub_822ADE38(helper, input): one turning part's input step (see the top).
REX_EXTERN(__imp__sub_822ADE38);
REX_HOOK_RAW(sub_822ADE38) {
  using namespace bf1943;
  const uint32_t helper = ctx.r3.u32, input = ctx.r4.u32;
  // Not in a plane: its turning parts are its control surfaces on the same concepts (5, 6), and turret aim on them
  // fought the plane's mouse aim (run 12 Nov 22:53: "turret aim written" in the Corsair, which could not be flown).
  if (REXCVAR_GET(bf_mouse_vehicle_aim) && Guest(helper) && Guest(input) && !GameMenuOpen() && !PlaneMouseActive()) {
    const uint32_t data = Load32(base, helper + 0x18), body = Load32(base, helper + 4);
    const uint32_t values = Load32(base, input + 0x160);
    const uint32_t input_concept = Guest(data) ? Load32(base, data + 0x34) : 99;
    if (Guest(body) && Guest(values) && (input_concept == 5 || input_concept == 6)) {
      // The mouse's movement since the last call, as yaw and pitch radians (shared by the seat's parts: taken once
      // a tick's worth, then each part takes its axis). Take it before locking: LookPad holds the look mutex when
      // it calls VehicleMouseStick, which takes this mutex.
      double dx = 0, dy = 0, idle = 1e9;
      const bool mouse = TakeVehicleMouse(dx, dy, idle);
      std::lock_guard lock(g_mutex);
      const auto now = std::chrono::steady_clock::now();
      const bool debug = REXCVAR_GET(bf_debug_vehicle_mouse);
      if (now - g_taken > std::chrono::milliseconds(500)) g_dx = g_dy = 0;  // out of the seat meanwhile
      const double sensitivity = REXCVAR_GET(bf_mouse_vehicle_sensitivity);
      const double k = kRadiansPerCount * (std::isfinite(sensitivity) ? std::clamp(sensitivity, 0.05, 10.0) : 1.0);
      if (now - g_axis_seen[6] >= kStale) g_dx = 0;
      if (now - g_axis_seen[5] >= kStale) g_dy = 0;
      g_axis_seen[input_concept] = now;
      // Right as the stick's right; up as the stick's up (issues #3 and #5). Up: positive, as the right stick's y
      // (concept 5 +1 when pushed up: the Sherman's gun to its -25 degree stop, up).
      if (mouse && std::isfinite(dx) && std::isfinite(dy)) {
        if (now - g_axis_seen[6] < kStale) g_dx = std::clamp(g_dx + dx * k, -2.8, 2.8);
        if (now - g_axis_seen[5] < kStale)
          g_dy = std::clamp(g_dy - dy * k * (REXCVAR_GET(bf_mouse_invert_y_vehicle) ? -1.0 : 1.0), -2.8, 2.8);
      }
      Part& p = g_parts[helper];
      const double angle = Angle(base, body), speed = AngularSpeed(base, body);
      if (p.body != body || now - p.seen > std::chrono::milliseconds(250)) p = Part{}, p.body = body;
      p.seen_before = p.seen;
      p.seen = now;
      // A controller's stick in use (a value the mouse did not cause): the stick has it, the target follows.
      const bool stick = g_real_stick && idle > 0.25;
      const bool valid = std::isfinite(angle) && std::isfinite(speed);
      if (p.engaged && (!mouse || stick || !valid) && debug)
        REXLOG_INFO("BF vehicle mouse: part {:08X} let go ({})", helper,
                    !mouse ? "no mouse look" : stick ? "the controller's stick" : "no angle");
      if (!mouse || stick || !valid) {
        p.engaged = false;
        p.rate = 0;
        p.stalled = {};
      }
      if (!p.engaged) p.target = angle;
      // The mouse's share for this part: right is the stick's right (concept 6 positive), which turns the angle
      // down; up is the stick's up (concept 5 positive), which turns it down too (a gun up).
      double& share = input_concept == 6 ? g_dx : g_dy;
      const double dt = std::chrono::duration<double>(now - p.seen_before).count();
      double moved = 0;
      if (mouse && !stick && valid && share != 0) {
        p.engaged = true;
        moved = -share;
        p.target = Wrap(p.target - share);
      }
      share = 0;
      // The target's speed, fed forward. It follows quickly (about 40 ms) so it is gone soon after the mouse stops.
      if (dt > 0 && dt < 0.2) p.rate += (moved / dt - p.rate) * std::min(1.0, dt * 25.0);
      if (p.engaged) {
        // A constrained part's target stays within its limits (degrees in the data).
        if (Load32(base, data + 0x2C) >> 24) {
          const double lo = LoadF(base, data + 0x14) * kPi / 180.0, hi = LoadF(base, data + 0x18) * kPi / 180.0;
          if (lo < hi) p.target = std::clamp(p.target, lo, hi);
        }
        // Tight: the target leads the part by at most kMaxLead; what the part cannot follow is dropped.
        const double lead = Wrap(p.target - angle);
        if (std::fabs(lead) > kMaxLead) {
          g_dbg_dropped[input_concept] += std::fabs(lead) - kMaxLead;
          p.target = Wrap(angle + std::copysign(kMaxLead, lead));
        }
        if (std::fabs(speed) > 0.05) p.measured = true;
        double s = 0;
        const double err = Wrap(p.target - (angle + speed * kDelay));
        if (p.open_loop) {
          // The part's angle cannot be read (an AA gun's yaw and pitch read 0 with no speed at full input, 12 Nov
          // 16:57): the mouse's own speed drives it, at most the part's top speed, and it stops when the mouse stops.
          // 0.4.0: written into the game's concepts (native_input.cpp); there is no stick push to fall back to.
          p.target = angle;
          s = std::clamp(-p.rate / kTopSpeed, -1.0, 1.0);
          if (!(Load32(base, data + 0x44) >> 24)) s = std::copysign(std::sqrt(std::fabs(s)), s);
        } else {
          // Toward the target, damped by the speed, with the target's own speed fed forward: positive input turns the
          // angle down.
          s = std::clamp(-9.0 * err + 0.6 * speed - p.rate / kTopSpeed, -1.0, 1.0);
          if (std::fabs(err) < 0.002 && std::fabs(speed) < 0.02 && std::fabs(p.rate) < 0.01) s = 0;
          if (!(Load32(base, data + 0x44) >> 24)) s = std::copysign(std::sqrt(std::fabs(s)), s);  // the game squares it
          if (std::fabs(err) > 0.006 && std::fabs(speed) < 0.01) {
            if (p.stalled == std::chrono::steady_clock::time_point{}) p.stalled = now;
            if (now - p.stalled > kStallReset) {
              p.stalled = {};
              ++g_dbg_stalls[input_concept];
              if (p.measured) {
                // At a stop or blocked: the target rests on the part, so the next mouse movement turns it at once.
                p.target = angle, p.rate = 0, s = 0;
              } else {
                p.open_loop = true;
                p.target = angle, s = 0;
                if (debug)
                  REXLOG_INFO("BF vehicle mouse: part {:08X} concept {} never moved at full input: driven by the "
                              "mouse's speed", helper, input_concept);
              }
            } else {
              // Short of the target and still: inputs this small are below the part's threshold (1 degree was left
              // at an input of 0.16, run vmouse6), so it gets the least push that moves it.
              s = std::copysign(std::max(std::fabs(s), 0.42), s == 0 ? -err : s);
            }
          } else {
            p.stalled = {};
          }
        }
        g_want[input_concept] = float(s), g_want_at[input_concept] = now;
        static std::chrono::steady_clock::time_point logged[8] = {};
        if (debug && now - logged[input_concept] > std::chrono::milliseconds(250)) {
          logged[input_concept] = now;
          REXLOG_INFO("BF vehicle mouse: part {:08X} concept {} angle {:.4f} target {:.4f} speed {:.3f} input {:.3f} "
                      "dropped {:.1f} deg, stops {}{}",
                      helper, input_concept, angle, p.target, speed, s, g_dbg_dropped[input_concept] * 180.0 / kPi,
                      g_dbg_stalls[input_concept], p.open_loop ? ", open loop" : "");
          g_dbg_dropped[input_concept] = 0, g_dbg_stalls[input_concept] = 0;
        }
      } else {
        g_want_at[input_concept] = {};
      }
      if (g_parts.size() > 64) g_parts.clear();
      g_taken = now;
    }
  }
  __imp__sub_822ADE38(ctx, base);
}

namespace bf1943 {
// For look.cpp's pad hook: the right stick the vehicle mouse wants (x for concept 6, y for concept 5, -1 to 1 as the
// game reads them after its dead zone), given the controller's own axes. The return value identifies a mounted
// rotation seat, and the mask identifies which axes have a fresh mouse command.
void ResetVehicleMouse() {
  std::lock_guard lock(g_mutex);
  g_parts.clear();
  g_dx = g_dy = 0;
  g_taken = {};
  g_want_at[5] = g_want_at[6] = {};
  g_axis_seen[5] = g_axis_seen[6] = {};
  g_real_stick = false;
}

// For look.cpp: the seat has a turning part (a turret, a mounted gun): its stick must not be held off centre, or the
// part keeps turning.
bool VehicleMouseTurretSeat() {
  std::lock_guard lock(g_mutex);
  return std::chrono::steady_clock::now() - g_taken < std::chrono::milliseconds(500);
}

// For native_input.cpp (0.4.0): the same wanted input, written into the game's input state right after its mapping
// (what the server gets), concept 6 for x and 5 for y. Nothing while a controller's stick is in use.
bool VehicleMouseConcepts(unsigned& axes, float& yaw6, float& pitch5) {
  if (PlaneMouseActive()) return false;
  std::lock_guard lock(g_mutex);
  if (g_real_stick) return false;
  const auto now = std::chrono::steady_clock::now();
  axes = (now - g_want_at[6] < kStale ? 1u : 0u) | (now - g_want_at[5] < kStale ? 2u : 0u);
  yaw6 = g_want[6], pitch5 = g_want[5];
  return axes != 0;
}

bool VehicleMouseStick(float real_x, float real_y, unsigned& axes, float& x, float& y) {
  std::lock_guard lock(g_mutex);
  g_real_stick = std::fabs(real_x) > 0.3f || std::fabs(real_y) > 0.3f;
  const auto now = std::chrono::steady_clock::now();
  const bool yaw = now - g_want_at[6] < kStale;
  const bool pitch = now - g_want_at[5] < kStale;
  axes = (yaw ? 1u : 0u) | (pitch ? 2u : 0u);
  x = g_want[6];
  y = g_want[5];
  // Only with an axis to aim: a seat whose parts are all left to the stick push gets it (an AA gun, 12 Nov 16:57: with
  // no axis fresh the mouse was dropped altogether).
  return axes != 0 && now - g_taken < kStale;
}
}  // namespace bf1943
