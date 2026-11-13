// Vehicle guns and turrets with the mouse (0.4.1, pull request 16 of the players' repository by ThatZaliaSGuy). Until
// 0.4.0 the mouse aimed and did not push a stick (TekRantGaming, 8 Nov 2026: "Mouse: should not be joystick
// emulation"); the stick below came in with TekRantGaming's "review and merge" of pull request 16 (13 Nov 2026).
//
// How the game turns them: each turning part (a tank's turret and gun, a mounted machine gun, an AA gun's mount) is a
// ChildRotationBodyData part with a physics body. Twice a game tick sub_822ADE38(helper, input) reads the seat's input
// concept for the part (RotationInput at data+0x34: 6 for yaw, 5 for pitch), squares it unless UseLinearInput
// (data+0x44), and pushes the body with an angular impulse. The server runs the same step with the same input.
//
// The mouse drives them as it flies a plane (plane_mouse.cpp: a stick the mouse moves, which springs back to the
// centre), written into the game's input concepts (native_input.cpp): the mouse's speed is the gun's speed, the view
// stays on the gun's sight, and a still mouse leaves the gun still. 13 Nov 2026: the boat's gun was driven so by
// accident (taken for a plane) and was "exactly how we want all turrets and the AA gun to work"; the aim controller
// before it (a target angle for each part, the view leading the gun) shook on seats with several parts on one concept
// (the AA gun has four on pitch, turning opposite ways), lost the sight's zoom and left the view off the sight.
//
// So this file only watches the parts: which of the seat's two axes have a part (the mouse drives only those), and
// whether a driver's parts are a gun (a tank) or a view rig (a boat: its driver gets free look). It also scales the
// parts' drive at 60 Hz.

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <set>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "seat_info.h"

REXCVAR_DEFINE_BOOL(bf_mouse_vehicle_aim, true, "BF1943/Controls",
                    "The mouse turns vehicle guns and turrets (false: only the keys and a controller do)");
REXCVAR_DEFINE_BOOL(bf_debug_vehicle_mouse, false, "BF1943/Controls",
                    "Log vehicle mouse aim to bf1943.log (the seat's turning parts, the mouse's stick)");
REXCVAR_DECLARE(bool, bf_mouse_invert_y_vehicle);  // input_settings.cpp

namespace bf1943 {
int TickRate();       // tick_rate.cpp
bool GameMenuOpen();  // game_menu.cpp

namespace {
using Clock = std::chrono::steady_clock;

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

// A part stepped within this long is the seat's (they step once or twice a tick).
constexpr auto kFresh = std::chrono::milliseconds(150);

std::mutex g_mutex;
uint32_t g_seat_key = 0;
Clock::time_point g_axis_seen[8] = {};  // when a part on the concept last stepped in this seat
bool g_yaw_free = false;                // the seat has a yaw part that turns all the way round (a turret, a mount)
std::set<uint32_t> g_logged;            // the seat's parts already logged (once each)
}  // namespace
}  // namespace bf1943

// sub_822ADE38(helper, input): one turning part's input step (see the top).
REX_EXTERN(__imp__sub_822ADE38);
REX_HOOK_RAW(sub_822ADE38) {
  using namespace bf1943;
  const uint32_t helper = ctx.r3.u32, input = ctx.r4.u32;
  const uint32_t data = Guest(helper) ? Load32(base, helper + 0x18) : 0;
  if (Guest(data) && Guest(input)) {
    const uint32_t c = Load32(base, data + 0x34);
    SeatInfo seat;
    if ((c == 5 || c == 6) && LocalSeat(seat)) {
      // Limits in degrees; a part that is not constrained, or whose limits are equal, turns all the way round.
      const bool constrained = (Load32(base, data + 0x2C) >> 24) != 0;
      const float lo = LoadF(base, data + 0x14), hi = LoadF(base, data + 0x18);
      std::lock_guard lock(g_mutex);
      if (seat.key != g_seat_key) {
        g_seat_key = seat.key;
        g_axis_seen[5] = g_axis_seen[6] = {};
        g_yaw_free = false;
        g_logged.clear();
      }
      g_axis_seen[c] = Clock::now();
      if (c == 6 && (!constrained || !(lo < hi) || hi - lo >= 359.0f)) g_yaw_free = true;
      // Once for each part of a seat (13 Nov: with several parts on a concept the last-part test logged every step,
      // 5 MB of log in two minutes).
      if (REXCVAR_GET(bf_debug_vehicle_mouse) && g_logged.size() < 64 && g_logged.insert(helper).second) {
        REXLOG_INFO("BF vehicle mouse: seat #{} ({}): part {:08X} on concept {} (data {:08X}, {} {:.0f} to {:.0f} "
                    "degrees, {} input)",
                    seat.key, SeatKindName(seat.kind), helper, c, data, constrained ? "limits" : "free, limits",
                    lo, hi, Load32(base, data + 0x44) >> 24 ? "linear" : "squared");
      }
    }
  }
  // 60 Hz (tick_rate.cpp): the part's push is worked out per call from its data, not from the tick's time
  // (data+32 x input x data+60 / data+16, bf1943_recomp.10.cpp sub_822ADE38), and twice the calls a second turned guns
  // at about half the speed (13 Nov 00:08: 0.42 and 0.68 rad/s at full input, against 1.05 to 1.24 at 30 Hz). The
  // part's drive (data+32) is scaled by rate / 30 for the call and put back after. The server runs the same step, so it
  // turns its guns the same way.
  const int rate = bf1943::TickRate();
  // Not in the game's image (0x82000000 up: read-only, a write there faults, tick_rate.cpp): data loaded with a level.
  if (rate != 30 && Guest(data) && !(data >= 0x82000000 && data < 0x84000000)) {
    // One at a time (the review of pull request 16, as of 12 before it): the drive belongs to the part's type, shared
    // by every vehicle of that type, and a server steps many vehicles: two steps at once could leave it scaled for
    // good. Recursive: a step that steps another part on this thread waits for nothing.
    static std::recursive_mutex drive_mutex;
    std::lock_guard drive_lock(drive_mutex);
    const float drive = LoadF(base, data + 32);
    StoreF(base, data + 32, drive * float(rate) / 30.0f);
    __imp__sub_822ADE38(ctx, base);
    StoreF(base, data + 32, drive);
    return;
  }
  __imp__sub_822ADE38(ctx, base);
}

namespace bf1943 {
// For look.cpp, when the mouse leaves the seat.
void ResetVehicleMouse() {
  std::lock_guard lock(g_mutex);
  g_axis_seen[5] = g_axis_seen[6] = {};
  g_yaw_free = false;
}

// The axes of the local seat the mouse turns a gun on: bit 0 concept 6 (yaw), bit 1 concept 5 (pitch); 0 in a seat
// without a gun. A gunner's seat: any turning part. A driver's: only with a yaw part that turns all the way round (a
// tank's turret); a boat driver's parts turn the view within limits and spring back, so that seat gets free look (13
// Nov: "boat driver camera still snaps forward"). Never on foot or in a plane (a plane's control surfaces are on the
// same concepts and step for a moment after the pilot has left).
unsigned VehicleMouseAxes() {
  if (!REXCVAR_GET(bf_mouse_vehicle_aim) || GameMenuOpen()) return 0;
  SeatInfo seat;
  if (!LocalSeat(seat)) return 0;
  if (seat.kind != SeatKind::kDriver && seat.kind != SeatKind::kGunner) return 0;
  std::lock_guard lock(g_mutex);
  if (seat.key != g_seat_key) return 0;
  if (seat.kind == SeatKind::kDriver && !g_yaw_free) return 0;
  const auto now = Clock::now();
  return (now - g_axis_seen[6] < kFresh ? 1u : 0u) | (now - g_axis_seen[5] < kFresh ? 2u : 0u);
}

// Invert vehicle (Settings, Controls): the mouse's up and down for guns, turrets and a vehicle's view. Its own switch
// (upstream 0.4.0's "same as on foot" choice, bf_mouse_vehicle_vertical, is not followed: 13 Nov, one plain invert for
// each of camera, vehicle and flight).
bool VehicleMouseInverted() { return REXCVAR_GET(bf_mouse_invert_y_vehicle); }

// For look.cpp: the seat has a gun the mouse turns.
bool VehicleMouseTurretSeat() { return VehicleMouseAxes() != 0; }

// The view is never turned off the gun's sight now (the camera lead of 13 Nov is gone); kept for tick_blend.cpp and
// the gun marker (comms_ui.cpp), which then do nothing.
bool TurretViewOffset(double& yaw, double& pitch) {
  yaw = pitch = 0;
  return false;
}
}  // namespace bf1943
