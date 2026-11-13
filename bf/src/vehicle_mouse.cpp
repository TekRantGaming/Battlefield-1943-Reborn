// The mouse in vehicles aims, it does not push a stick (TekRantGaming, 8 Nov 2026: "Mouse: should not be joystick emulation").
//
// How the game turns a vehicle's guns and turrets (research in the session scratchpad vehicle_aim/notes.md): each
// turning part (a tank's turret and gun, a mounted machine gun, a jeep gunner's mount) is a ChildRotationBodyData part
// with a physics body. Twice a game tick sub_822ADE38(helper, input) reads the seat's input concept for the part
// (RotationInput at data+0x34; the right stick's x is concept 6, its y concept 5), squares it unless UseLinearInput
// (data+0x44), and pushes the body with an angular impulse: the stick sets an acceleration toward a damped top speed,
// and the turret coasts. The server runs the same step with the same input, so the angle itself must not be set.
//
// Tight aim (12 Nov 2026, pull request 11): the target leads the part by at most kMaxLead, and is put back on the part
// at a stop.
//
// So the mouse moves a target angle for each of the local seat's parts (the counts times 0.066 degrees, the on foot
// scale, times Vehicle sensitivity), and in each call the input the part reads is replaced by the one that drives the
// body to that target as fast as the vehicle allows, and stops it there (a proportional and damping controller on the
// body's angle and angular velocity, the game's square undone). The server sees ordinary input values. A controller's
// stick takes over at once when it is used (the target follows the turret). Since 0.4.0 the wanted input is written
// into the game's input concepts (native_input.cpp), not given as the pad's right stick, and a plane's turning parts,
// its control surfaces, are left to plane_mouse.cpp.
//
// Only the local player's seat runs these steps in the player's game (vaim2: the helpers ran for the seat sat in).

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DECLARE(double, bf_mouse_vehicle_sensitivity);  // look.cpp
REXCVAR_DECLARE(bool, bf_mouse_invert_y_vehicle);    // input_settings.cpp
REXCVAR_DECLARE(bool, bf_mouse_invert_y);            // input_settings.cpp: the soldier's own
REXCVAR_DECLARE(std::string, bf_mouse_vehicle_vertical);  // input_settings.cpp: same, normal or inverted
REXCVAR_DEFINE_BOOL(bf_mouse_vehicle_aim, true, "BF1943/Controls",
                    "The mouse aims vehicle guns and turrets at a target angle (false: it pushes the right stick)");
REXCVAR_DEFINE_BOOL(bf_debug_vehicle_mouse, false, "BF1943/Controls",
                    "Log vehicle mouse aim to bf1943.log (turret steps, dropped movement, stops, plane commands)");

namespace bf1943 {
bool TakeVehicleMouse(double& dx, double& dy, double& idle);  // look.cpp
bool PlaneSeat();                                            // plane_mouse.cpp: the local player's vehicle is a plane
bool GameMenuOpen();                                         // game_menu.cpp

// Which way is up for the mouse in a vehicle (the guns and turrets here, a seat's view and a plane's stick in
// look.cpp, native_input.cpp and plane_mouse.cpp): Settings > Controls > Keyboard & mouse > Mouse vertical in
// vehicles. "same" (the default) follows the soldier's own Mouse vertical, so a player who plays inverted on foot is
// inverted in a tank too (TekRantGaming, 12 Nov 2026 evening: 0.3.0 players called vehicle aiming inverted; 0.3.0 had
// given the vehicles a Normal of their own); a 0.3.0 user folder's Inverted for vehicles still counts under "same".
bool VehicleMouseInverted() {
  const std::string vertical = REXCVAR_GET(bf_mouse_vehicle_vertical);
  if (vertical == "inverted") return true;
  if (vertical == "normal") return false;
  return REXCVAR_GET(bf_mouse_invert_y) || REXCVAR_GET(bf_mouse_invert_y_vehicle);
}

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRadiansPerCount = 0.066 * kPi / 180.0;  // look.cpp's on foot scale

uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
float LoadF(const uint8_t* base, uint32_t a) { return std::bit_cast<float>(Load32(base, a)); }
bool Guest(uint32_t a) { return a >= 0x10000 && a < 0xF0000000; }

struct Part {
  double target = 0;
  uint32_t body = 0;
  bool engaged = false;  // the mouse has the part (else its target follows the turret)
  std::chrono::steady_clock::time_point seen{}, seen_before{};
  std::chrono::steady_clock::time_point stalled{};  // pushed without moving since (a stop, or blocked)
  bool measured = false;   // its angle has been seen to change: the angle and speed read are this part's
  bool open_loop = false;  // pushed hard and never seen to move: driven by the mouse's speed alone
  double last_angle = 0, last_input = 0;
  double rate = 0;  // the target's speed (radians a second, smoothed): fed forward
  // Which way the part turns for a positive input. +1: the angle goes down (a tank's gun: the game's stick up, concept
  // 5 at +1, takes it to its -25 degree stop, up; so the Sherman's gun and turret, run vt2 of 13 Nov 2026, a landing
  // boat's guns, run v52b, and a jeep's gun, run vmjeepup2 of 12 Nov). -1: the part's axis runs the other way, so
  // its angle grows with a positive input, and its target moves and its input is given the other way round. Learnt
  // from the part's own response and remembered by its data (its type) for the session (g_dir_by_data).
  // The one part seen to run the other way was a Corsair's elevator (runs vmaaup1 to 7 of 12 Nov 2026: the loop,
  // built on +1, drove it to its +30 degree stop against the mouse and held it there). Those runs were taken for
  // the carrier's anti aircraft gun; the test's soldier had sat down in the plane beside it (found 13 Nov 2026). A
  // plane's parts are no longer aimed here at all, so this is kept as a guard for the guns not tried yet (the Type
  // 97's, the Kurogane's, the stationary guns).
  int dir = 1;
  int against = 0;  // steps of motion against the direction modelled, in a row
  bool learnt = false;  // a step of motion as modelled was seen (then a stop it pushes into is a stop, not a mistake)
  double applied = 0;   // the input given last call, after dir
  double prev_angle = 0;  // the angle at the last call: the part's own motion since (the body's speed can read oddly:
                          // run vmaaup5, a Corsair's elevator, its sign swinging while the angle rose steadily)
  std::chrono::steady_clock::time_point stuck{};  // pushed hard with the part still since
  // The direction is read only where the part starts to move from rest under one push (below): the calls in a row it
  // was still, the push that found it at rest, and until when that start counts.
  int rest_steps = 0, onset_sign = 0;
  std::chrono::steady_clock::time_point onset_until{};
  std::chrono::steady_clock::time_point flipped{};  // the last change of dir: the part coasts the old way a moment
};
std::mutex g_mutex;
std::map<uint32_t, Part> g_parts;  // by helper
std::map<uint32_t, int> g_dir_by_data;  // the direction learnt, by the part's data (its type)
void Flip(Part& p, uint32_t data, uint32_t helper, double angle, const char* why) {
  p.dir = -p.dir, p.against = 0, p.stuck = {}, p.stalled = {}, p.target = angle, p.rate = 0;
  p.flipped = std::chrono::steady_clock::now();
  g_dir_by_data[data] = p.dir;
  REXLOG_INFO("BF vehicle mouse: part {:08X} (data {:08X}) turns the other way for an input ({}): direction {:+d}", helper,
              data, why, p.dir);
}
double g_dx = 0, g_dy = 0;         // mouse radians not yet given to the parts (yaw, pitch)
std::chrono::steady_clock::time_point g_taken{};
// The input each concept should get (5 pitch, 6 yaw) and when it was worked out; native_input.cpp writes it into the
// game's input state right after the pad's mapping, so the server gets the same input as the player's game (writing
// the input object in the step moved only the player's own copy: the server's corrections pulled the turret back, run
// vmouse2).
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
// The input worked out in a step reaches the part a tick later (the mapping hook writes it, the next tick reads it),
// so the error is taken against where the part will be by then (run 12 Nov 16:39: the Sherman overshot back and forth).
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
  // Nor with Mouse flying off (0.3 aimed the elevator like a gun and held the stick over: "near impossible").
  if (REXCVAR_GET(bf_mouse_vehicle_aim) && Guest(helper) && Guest(input) && !GameMenuOpen() && !PlaneSeat()) {
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
      // Right as the stick's right; up as the stick's up (issues #3 and #5: the mouse's down was taken as the stick's
      // up, so a tank's gun and a jeep's machine gun aimed up as the mouse went down, the opposite of on foot). Up:
      // positive, as the right stick's y (concept 5 +1 when pushed up: the Sherman's gun to its -25 degree stop, up).
      // Which way is up for the mouse is the vehicles' own setting (VehicleMouseInverted).
      if (mouse && std::isfinite(dx) && std::isfinite(dy)) {
        if (now - g_axis_seen[6] < kStale) g_dx = std::clamp(g_dx + dx * k, -2.8, 2.8);
        if (now - g_axis_seen[5] < kStale)
          g_dy = std::clamp(g_dy - dy * k * (VehicleMouseInverted() ? -1.0 : 1.0), -2.8, 2.8);
      }
      Part& p = g_parts[helper];
      const double angle = Angle(base, body), speed = AngularSpeed(base, body);
      if (p.body != body || now - p.seen > std::chrono::milliseconds(250)) {  // a new seat, a respawn, a new part
        p = Part{}, p.body = body, p.prev_angle = angle;
        // The direction learnt for its type, earlier in the session.
        if (const auto it = g_dir_by_data.find(data); it != g_dir_by_data.end()) p.dir = it->second, p.learnt = true;
      }
      // A call that sees the body at rest at zero while the part is elsewhere (a tick replayed from the server's
      // state, every few calls in run vmouse1) is not the part's state: nothing is learnt from it and the target is
      // left alone (the lead limit would pull it to within kMaxLead of zero, up to 6 degrees a call; the review of the
      // pull request, 13 Nov 2026), the last input stands, and the mouse's movement waits for the next real call.
      const bool replay = p.engaged && angle == 0 && speed == 0 && std::fabs(p.last_angle) > 0.02;
      if (replay) {
        p.seen = now;
        g_want_at[input_concept] = now;
      } else {
      p.seen_before = p.seen;
      p.seen = now;
      // The part's own response to the input given last call, against the direction modelled (its angle's step
      // against the input's sign: a tank's gun moves against the sign of its input). Two steps against in a row, or
      // half a second pushed hard with the part still before it ever moved as modelled (driven into a stop), and the
      // direction is the other one. Not within 0.4 s of a change: the part coasts the old way for a few steps after
      // its input turns round (run vmaaup4: the part's direction flipped back and forth five times in half a
      // second on its own momentum).
      const double step = Wrap(angle - p.prev_angle);  // the part's own motion since the last call
      p.prev_angle = angle;
      // At a stop the part bounces back a little when pushed into it (run vmaaup6: the part at its 30 degree stop
      // read as moving against the input twice and its direction turned round): no samples within 0.05 radians of a
      // limit, and once a direction has been seen right, four steps against in a row to change it.
      bool at_limit = false;
      if (Load32(base, data + 0x2C) >> 24) {
        const double lo = LoadF(base, data + 0x14) * kPi / 180.0, hi = LoadF(base, data + 0x18) * kPi / 180.0;
        at_limit = lo < hi && (angle < lo + 0.05 || angle > hi - 0.05);
      }
      // Samples are taken only where the part starts to move from rest under one push (for half a second from the
      // last call that found it at rest under that push, while the push keeps its sign). While it brakes, the tight
      // loop pushes hard against a part that still coasts the old way for several steps, which read as motion against
      // the input and turned the direction round in the middle of a turn (run vt1 of 13 Nov 2026: the Sherman's turret
      // ran 7 degrees back, then the direction turned round again).
      const int push = std::fabs(p.applied) > 0.3 ? (p.applied > 0 ? 1 : -1) : 0;
      if (std::fabs(step) < 0.0006) ++p.rest_steps;
      else p.rest_steps = 0;
      if (push && p.rest_steps >= 2) p.onset_sign = push, p.onset_until = now + std::chrono::milliseconds(500);
      if (push != p.onset_sign) p.onset_sign = 0;
      const bool sampling = push && push == p.onset_sign && now < p.onset_until;
      if (p.engaged && std::fabs(p.applied) > 0.3 && now - p.flipped > std::chrono::milliseconds(400) && !at_limit) {
        if (std::fabs(step) > 0.004) {
          if (sampling) {
            const bool modelled = (step * p.applied < 0) == (p.dir > 0);
            p.against = modelled ? 0 : p.against + 1;
            if (modelled) p.learnt = true;
            if (p.against >= (p.learnt ? 4 : 2)) Flip(p, data, helper, angle, "its angle moved against the input");
          }
          p.stuck = {};
        } else if (!p.learnt && std::fabs(Wrap(p.target - angle)) > 0.05) {
          if (p.stuck == std::chrono::steady_clock::time_point{}) p.stuck = now;
          else if (now - p.stuck > std::chrono::milliseconds(500))
            Flip(p, data, helper, angle, "pushed hard for half a second without moving");
        } else {
          p.stuck = {};
        }
      } else {
        p.stuck = {};
      }
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
      // down; up is the stick's up (concept 5 positive), which turns it down too (a gun up). A part that runs the
      // other way (dir) has its target moved the other way.
      double& share = input_concept == 6 ? g_dx : g_dy;
      const double dt = std::chrono::duration<double>(now - p.seen_before).count();
      double moved = 0;
      const double target_before = p.target;
      if (mouse && !stick && valid && share != 0) {
        p.engaged = true;
        p.target = Wrap(p.target - share * p.dir);
      }
      share = 0;
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
        // The target's own movement, after its limits and its lead: at a stop it is none, so the feed forward below
        // stops pushing the part into the stop (run vmaaup6: the part pushed into its 30 degree stop bounced, which
        // read as moving against the mouse).
        moved = Wrap(p.target - target_before);
      }
      // The target's speed, fed forward. It follows quickly (about 40 ms) so it is gone soon after the mouse stops.
      if (dt > 0 && dt < 0.2) p.rate += (moved / dt - p.rate) * std::min(1.0, dt * 25.0);
      if (p.engaged) {
        if (std::fabs(speed) > 0.05 || std::fabs(step) > 0.004) p.measured = true;
        if (p.measured) p.open_loop = false;  // its angle does change: aimed at the target again
        double s = 0;
        const double err = Wrap(p.target - (angle + speed * kDelay));
        if (p.open_loop) {
          // The part's angle cannot be read (pull request 11: an AA gun's yaw and pitch read 0 with no speed at full
          // input, 12 Nov 16:57): the mouse's own speed drives it, at most the part's top speed, and it stops when
          // the mouse stops.
          p.target = angle;
          s = std::clamp(-p.rate / kTopSpeed, -1.0, 1.0);
          if (!(Load32(base, data + 0x44) >> 24)) s = std::copysign(std::sqrt(std::fabs(s)), s);
          p.last_angle = angle, p.last_input = s;
        } else {
          // Toward the target, damped by the speed, with the target's own speed fed forward: positive input turns the
          // angle down (dir +1; the other way round is put right below).
          s = std::clamp(-9.0 * err + 0.6 * speed - p.rate / kTopSpeed, -1.0, 1.0);
          if (std::fabs(err) < 0.002 && std::fabs(speed) < 0.02 && std::fabs(p.rate) < 0.01) s = 0;
          if (!(Load32(base, data + 0x44) >> 24)) s = std::copysign(std::sqrt(std::fabs(s)), s);  // the game squares it
          if (std::fabs(err) > 0.006 && std::fabs(speed) < 0.01 && std::fabs(step) < 0.0004) {
            if (p.stalled == std::chrono::steady_clock::time_point{}) p.stalled = now;
            if (now - p.stalled > kStallReset) {
              p.stalled = {};
              ++g_dbg_stalls[input_concept];
              if (p.measured) {
                // At a stop or blocked: the target rests on the part, so the next mouse movement turns it at once.
                p.target = angle, p.rate = 0, s = 0;
              } else {
                // Never seen to move: by the mouse's speed from here on (until its angle is seen to change).
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
          p.last_angle = angle, p.last_input = s;
        }
        p.applied = s * p.dir;  // the part's own way round
        g_want[input_concept] = float(p.applied), g_want_at[input_concept] = now;
        static std::chrono::steady_clock::time_point logged[8] = {};
        if (debug && now - logged[input_concept] > std::chrono::milliseconds(250)) {
          logged[input_concept] = now;
          REXLOG_INFO("BF vehicle mouse: part {:08X} concept {} angle {:.4f} target {:.4f} speed {:.3f} input {:.3f} "
                      "dir {:+d} dropped {:.1f} deg, stops {}{}",
                      helper, input_concept, angle, p.target, speed, p.applied, p.dir,
                      g_dbg_dropped[input_concept] * 180.0 / kPi, g_dbg_stalls[input_concept],
                      p.open_loop ? ", open loop" : "");
          g_dbg_dropped[input_concept] = 0, g_dbg_stalls[input_concept] = 0;
        }
      } else {
        p.applied = 0;
        g_want_at[input_concept] = {};
      }
      }  // not a replayed call
      if (g_parts.size() > 64) g_parts.clear();
      g_taken = now;
    }
  }
  __imp__sub_822ADE38(ctx, base);
}

namespace bf1943 {
// Out of a seat, or the menu opened: every part's state forgotten (the directions learnt stay for the session).
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

// For native_input.cpp (0.4.0): the wanted input, written into the game's input state right after its mapping (what
// the server gets), concept 6 for x and 5 for y; axes bit 0 for 6, bit 1 for 5. Nothing while a controller's stick is
// in use.
bool VehicleMouseConcepts(unsigned& axes, float& yaw6, float& pitch5) {
  if (PlaneSeat()) return false;
  std::lock_guard lock(g_mutex);
  if (g_real_stick) return false;
  const auto now = std::chrono::steady_clock::now();
  axes = (now - g_want_at[6] < kStale ? 1u : 0u) | (now - g_want_at[5] < kStale ? 2u : 0u);
  yaw6 = g_want[6], pitch5 = g_want[5];
  return axes != 0;
}

// For look.cpp's pad hook: told the controller's own right stick (`real_x`, `real_y`: pushed, the controller aims),
// and gives the input the vehicle mouse wants (x for concept 6, y for concept 5, -1 to 1 as the game reads them after
// its dead zone) with the mask of the axes that have a fresh mouse command. True with an axis to aim.
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
