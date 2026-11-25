// Planes' and tanks' free look with the keyboard and mouse (the user, 18 Nov 2026: "free-look when holding Left Shift by
// default for planes first and third person views ... tanks third person views only"). A plane's views are the game's
// own two, the cockpit and the chase view (the user, 24 Nov 2026: "revert back to the game's default two views; 1p and
// 3p, no bomb view").
//
// Free look (hold): the Air and Ground vehicles' Free-Look keys (Shift by default; Controls > Bindings, their own row:
// the user, 18 Nov 2026, "Make it remapable in the controls as just Free-Look"; vehicle_binds.cpp, read as the Guide
// button) turn the view with the mouse while held, in a plane's cockpit and chase views and a tank's outside view, as
// look.cpp's free look does for drivers without a gun; let go, the view is back where the game points it and the mouse
// flies the plane or turns the turret again. The keys still fly and drive meanwhile. Only the player's own view turns
// (tick_blend.cpp): the server sees nothing.

#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>

#include "seat_info.h"

REXCVAR_DEFINE_BOOL(bf_vehicle_free_look_hold, true, "BF1943/Controls",
                    "Holding Free-Look (Shift; the Air and Ground vehicles' bindings) turns the view with the mouse in "
                    "a plane's first and third person views and a tank's third person view; let go, the view is back "
                    "ahead");
REXCVAR_DECLARE(bool, bf_smooth_motion);        // tick_blend.cpp: the vehicle view goes through it
REXCVAR_DECLARE(bool, bf_debug_vehicle_mouse);  // vehicle_mouse.cpp

namespace bf1943 {
float VehicleViewReach();     // tick_blend.cpp: the view's distance from the vehicle's root (m), -1 not known
bool TickBlendVehicleView();  // tick_blend.cpp: the vehicle's view goes through our view hook

namespace {
using Clock = std::chrono::steady_clock;
constexpr uint16_t kGuide = 0x0400;  // the keys' Guide button: Free-Look's keys in a plane or a ground vehicle
constexpr float kTankOutside = 4.0f;  // metres from the tank's root: further is its outside view

std::atomic<bool> g_tank_free{false};  // free look is held in a tank's outside view (as of the pad hook's last read)
}  // namespace

// Free look held: the Free-Look keys, in a plane (its cockpit and chase views) or at a tank's wheel (a driver's seat
// with a turret, not a boat's) in an outside view, with the vehicle's view going through our view hook.
bool FreeLookHeld(bool turret_seat) {
  if (!REXCVAR_GET(bf_vehicle_free_look_hold) || !(rex::input::mnk::LastKeyButtons() & kGuide)) {
    g_tank_free.store(false, std::memory_order_relaxed);
    return false;
  }
  SeatInfo seat;
  const bool smooth = REXCVAR_GET(bf_smooth_motion) && TickBlendVehicleView();
  const bool known = LocalSeat(seat);
  const bool tank = known && seat.kind == SeatKind::kDriver && !seat.boat && turret_seat;
  // The tank's outside view: its camera well away from the tank (the game's own first person answer is yes there too,
  // run vw_tank2); the gun's sight is within a few metres of the tank's root.
  const float reach = VehicleViewReach();
  const bool outside = reach > kTankOutside;
  const bool plane = known && seat.kind == SeatKind::kPlane;
  const bool held = smooth && known && (plane || (tank && outside));
  g_tank_free.store(held && tank, std::memory_order_relaxed);
  // bf_debug_vehicle_mouse: why it is not, once a second while the key is held (a seat it does not apply to, or a
  // tank's first person view).
  static Clock::time_point logged{};
  if (!held && REXCVAR_GET(bf_debug_vehicle_mouse) && Clock::now() - logged > std::chrono::seconds(1)) {
    logged = Clock::now();
    REXLOG_INFO("BF free look: the key is held but not free look here (vehicle view {}, seat {}, tank {}, the view "
                "{:.1f} m from the vehicle)",
                smooth, known ? SeatKindName(seat.kind) : "not known", tank, reach);
  }
  return held;
}

// For tick_blend.cpp's free look turn: a tank's outside view goes round the tank (the game calls it first person).
bool TankFreeLook() { return g_tank_free.load(std::memory_order_relaxed); }

// The keys' Guide button (Free-Look's keys) kept from the game in a vehicle (look.cpp's pad hook).
void TakeFreeLookPress(uint8_t* gamepad) {
  if (!(rex::input::mnk::LastKeyButtons() & kGuide)) return;
  uint16_t buttons = uint16_t(gamepad[0] << 8 | gamepad[1]);
  buttons &= ~kGuide;
  gamepad[0] = uint8_t(buttons >> 8), gamepad[1] = uint8_t(buttons);
}
}  // namespace bf1943
