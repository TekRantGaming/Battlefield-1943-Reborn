// Planes' and tanks' views with the keyboard and mouse (the user, 18 Nov 2026: "free-look when holding Left Shift by
// default for planes first and third person views ... a bomber view to planes in that V also cycles through, we can
// just copy the bomb view from the bomber run ... tanks third person views only").
//
// Free look (hold): the Air and Ground vehicles' Free-Look keys (Shift by default; Controls > Bindings, their own row:
// the user, 18 Nov 2026, "Make it remapable in the controls as just Free-Look"; vehicle_binds.cpp, read as the Guide
// button) turn the view with the mouse while held, in a plane's cockpit and chase views (not the bomb view: the user,
// 18 Nov 2026, "The bomb view should not have free-look") and a tank's outside view, as look.cpp's
// free look does for drivers without a gun; let go, the view is back where the game points it and the mouse flies the
// plane or turns the turret again. The keys still fly and drive meanwhile. Only the player's own view turns
// (tick_blend.cpp): the server sees nothing.
//
// The bomb view: the planes' camera button (V, the D-pad's up) goes from the cockpit to the chase view, then to the
// bomb view, then the cockpit again (the game's own cycle is the first two: run vw_plane2; the look back camera in
// corsair.dbx is not on the button). It is the air raid bomber's bomb sight (tick_blend.cpp BombView: ahead and 45
// degrees down, its field of view 55, fov.cpp), on the plane the player flies, shown in place of the chase view (the
// game stays in that view: its sounds and HUD are that view's). The press that leaves the chase view for it is kept
// from the game; the next one goes through, and the game goes back to the cockpit.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>

#include "seat_info.h"

REXCVAR_DEFINE_BOOL(bf_vehicle_free_look_hold, true, "BF1943/Controls",
                    "Holding Free-Look (Shift; the Air and Ground vehicles' bindings) turns the view with the mouse in "
                    "a plane's first and third person views and a tank's third person view; let go, the view is back "
                    "ahead");
REXCVAR_DEFINE_BOOL(bf_plane_bomb_view, true, "BF1943/Controls",
                    "Planes: the camera button (V) also goes to a bomb view after the chase view (the air raid "
                    "bomber's bomb sight: ahead and 45 degrees down)");
REXCVAR_DECLARE(bool, bf_smooth_motion);        // tick_blend.cpp: the vehicle view goes through it
REXCVAR_DECLARE(bool, bf_debug_vehicle_mouse);  // vehicle_mouse.cpp

namespace bf1943 {
int FirstPersonView();        // fov.cpp: 1 first person, 0 an outside view, -1 not known
float VehicleViewReach();     // tick_blend.cpp: the view's distance from the vehicle's root (m), -1 not known
bool TickBlendVehicleView();  // tick_blend.cpp: the vehicle's view goes through our view hook

namespace {
using Clock = std::chrono::steady_clock;
constexpr uint16_t kDpadUp = 0x0001;
constexpr uint16_t kGuide = 0x0400;  // the keys' Guide button: Free-Look's keys in a plane or a ground vehicle
constexpr float kTankOutside = 4.0f;  // metres from the tank's root: further is its outside view

std::mutex g_mutex;
bool g_bomb = false;     // the bomb view is shown
bool g_up_held = false;  // the camera button was down at the last read
std::atomic<bool> g_tank_free{false};  // free look is held in a tank's outside view (as of the pad hook's last read)
bool g_keep_up = false;  // the camera button is kept from the game until it is let go
uint32_t g_seat_key = 0;

bool PlaneSeat(SeatInfo& seat) { return LocalSeat(seat) && seat.kind == SeatKind::kPlane; }
}  // namespace

bool PlaneSeatNow() {
  SeatInfo seat;
  return PlaneSeat(seat);
}

bool BombViewActive() {
  SeatInfo seat;
  if (!REXCVAR_GET(bf_plane_bomb_view) || !PlaneSeat(seat)) return false;
  std::lock_guard lock(g_mutex);
  return g_bomb && seat.key == g_seat_key;
}

// Free look held: the Free-Look keys, in a plane (its cockpit and chase views, not the bomb view) or at a tank's wheel (a driver's seat with a turret,
// not a boat's) in an outside view, with the vehicle's view going through our view hook.
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
  const bool plane = known && seat.kind == SeatKind::kPlane && !BombViewActive();
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

// From look.cpp's pad hook: the plane's camera button through its views and the bomb view. `gamepad` is the game's
// X_INPUT_GAMEPAD (big endian, the buttons first).
void VehicleViewsPad(uint8_t* gamepad) {
  SeatInfo seat;
  const bool plane = REXCVAR_GET(bf_plane_bomb_view) && PlaneSeat(seat);
  std::lock_guard lock(g_mutex);
  uint16_t buttons = uint16_t(gamepad[0] << 8 | gamepad[1]);
  const bool up = buttons & kDpadUp;
  if (!plane || seat.key != g_seat_key) {
    if (g_bomb) REXLOG_INFO("BF plane views: the bomb view is left (no plane, or another seat)");
    g_bomb = false, g_keep_up = false, g_up_held = up;
    g_seat_key = plane ? seat.key : 0;
    return;
  }
  // The game went to another view itself (a respawn, its own camera change): the bomb view is let go.
  if (g_bomb && FirstPersonView() == 1) {
    g_bomb = false;
    REXLOG_INFO("BF plane views: the bomb view is left (the game shows its cockpit)");
  }
  if (up && !g_up_held) {
    if (g_bomb) {
      g_bomb = false;  // this press goes to the game: from its look back view to the cockpit
      REXLOG_INFO("BF plane views: the cockpit again");
    } else if (FirstPersonView() == 0 && REXCVAR_GET(bf_smooth_motion) && TickBlendVehicleView()) {
      g_bomb = true, g_keep_up = true;
      REXLOG_INFO("BF plane views: the bomb view");
    }
  }
  g_up_held = up;
  if (g_keep_up) {
    if (up) {
      buttons &= ~kDpadUp;
      gamepad[0] = uint8_t(buttons >> 8), gamepad[1] = uint8_t(buttons);
    } else {
      g_keep_up = false;
    }
  }
}

// The keys' Guide button (Free-Look's keys) kept from the game in a vehicle (look.cpp's pad hook).
void TakeFreeLookPress(uint8_t* gamepad) {
  if (!(rex::input::mnk::LastKeyButtons() & kGuide)) return;
  uint16_t buttons = uint16_t(gamepad[0] << 8 | gamepad[1]);
  buttons &= ~kGuide;
  gamepad[0] = uint8_t(buttons >> 8), gamepad[1] = uint8_t(buttons);
}
}  // namespace bf1943
