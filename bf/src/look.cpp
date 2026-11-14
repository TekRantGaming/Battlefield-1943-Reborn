// The camera and look system (King Kong's mouse_look.cpp and input_remap.cpp, made for Battlefield's soldier).
//
// The game turns the soldier's aim in its 30 Hz tick: the aim update (sub_822B0B18(aim, input, flag), called from
// sub_822B2680 inside the soldier's movement tick sub_82210C10) adds YawSpeed * dt * curve(stick) to the yaw at
// aim+8 (wrapped to 0..2 pi) and the same for the pitch at aim+12 (clamped), with dt fixed at 1/30 s. The movement
// tick then copies them to soldier+12 and +16. Everything drawn each frame (the camera, the gun, the body) reads them
// through two getters, sub_82200FC8 (yaw) and sub_82201068 (pitch); a hardware watch on both values found no other
// reader (dev_watch.cpp). So at any frame rate the view turned only 30 times a second (camera log, run turn1: the
// rotation changed on every other frame at 50 FPS) and turning felt like 30 FPS.
//
// With bf_look on, the turn is worked out every frame instead:
//   - the mouse: the window's raw motion while the pointer is locked, a fixed angle per count (0.066 degrees at
//     sensitivity 1, King Kong's), at any speed and frame rate; this path takes over mouse motion from the runtime;
//   - the stick: the turn speed the game's own update gave on its last tick (its curve, acceleration and zoom
//     included), times the camera sensitivity, for the time since that tick.
// Each frame (at VdSwap) that turn is added up; the getters give the local soldier's yaw and pitch plus the turn not
// yet handed to the game; and on the next aim update the game's own turn is replaced by exactly that sum, so the
// value the getters give never jumps, the server gets the same aim, and the turn per second is the same at 30, 60,
// 144 FPS or unlocked. With bf_look off the game turns as on the Xbox 360, and camera sensitivity scales its turn.
//
// In a vehicle the soldier's aim does not update. A vehicle's guns and turrets are aimed by the mouse at a target
// angle (vehicle_mouse.cpp, its input given as the right stick in LookPad); in planes the mouse points where to fly
// and the plane is steered there (plane_mouse.cpp); elsewhere (a driver's seat) the mouse pushes the right stick, as
// the runtime did.

#include <algorithm>
#include <array>
#include <cstdio>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <native_x_renderer/native_x_renderer.h>
#include <rex/cvar.h>
#include "seat_info.h"
#include <rex/hook.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "comms.h"

REXCVAR_DEFINE_BOOL(bf_look, true, "BF1943/Controls",
                    "Camera turning: true every frame (our look system), false 30 times a second as on the Xbox 360");
REXCVAR_DEFINE_DOUBLE(bf_mouse_sensitivity, 1.0, "BF1943/Controls", "Mouse sensitivity, 0.1 to 5 (times)");
REXCVAR_DEFINE_DOUBLE(bf_mouse_vehicle_sensitivity, 1.0, "BF1943/Controls",
                      "Mouse speed in vehicles and their guns, 0.1 to 5 (times): the mouse's own, apart from on foot");
REXCVAR_DEFINE_DOUBLE(bf_mouse_zoom_sensitivity, 100.0, "BF1943/Controls",
                      "Mouse speed while aiming down sights or through a scope, 25 to 200 (%): at 100 a mouse movement "
                      "moves the view the same distance on screen as without zoom");
REXCVAR_DEFINE_BOOL(bf_vehicle_free_look, true, "BF1943/Controls",
                    "In seats without a gun or turret (drivers, passengers) the mouse turns the view like on foot, one "
                    "to one, and it stays where it is left (false: the mouse pushes the game's free look stick)");
REXCVAR_DEFINE_BOOL(bf_look_motion, true, "BF1943/Controls",
                    "Walking on every frame: the first person view and gun move on between the game's 30 Hz ticks "
                    "along the soldier's own movement (false: they step 30 times a second, as on the Xbox 360)");
REXCVAR_DECLARE(double, bf_camera_sensitivity);  // input_settings.cpp
namespace bf1943 {
double VehicleCameraGain();  // input_settings.cpp: the controller's camera gain in a vehicle's seat, 0 on foot
}
REXCVAR_DECLARE(bool, bf_mouse_invert_y);
REXCVAR_DECLARE(bool, bf_mouse_invert_y_vehicle);
REXCVAR_DECLARE(bool, mnk_mode);
REXCVAR_DECLARE(bool, mnk_mouse);
REXCVAR_DECLARE(bool, bf_native_input);       // native_input.cpp
REXCVAR_DECLARE(bool, bf_mouse_vehicle_aim);  // vehicle_mouse.cpp

#if defined(BF_DEV_TOOLS)
namespace bf1943 {
std::atomic<float> g_dev_camera[3];  // the local camera's position (dev_matscan.cpp)
std::atomic<float> g_dev_yaw{0};     // the local soldier's aim yaw after its last update (modern_mode.cpp's speed log)
std::atomic<bool> g_dev_camera_valid{false};
}  // namespace bf1943
#endif

namespace bf1943 {
bool GameMenuOpen();  // game_menu.cpp
void NoteMatchDevice(bool keyboard);  // game_menu.cpp: the device used in the match, for the HUD's prompts
uint64_t ClientTicks();  // vehicle_camera.cpp: the client ticks run so far
bool SpawnMouseActive();   // flash_mouse.cpp: the deploy screen's free pointer
void ResetVehicleMouse();  // vehicle_mouse.cpp
void ResetPlaneMouse();    // plane_mouse.cpp
bool PlaneMouse(uint8_t* gamepad, double dx, double dy);  // plane_mouse.cpp
void PulseButton(uint16_t button, int ms);  // input_settings.cpp: our press on the player's pad
bool LocalOnFoot();  // below
// A notch of the mouse wheel: on foot, BF's Toggle Weapon (the right bumper; BF's own Help & Options > Controls, run
// help3: Y is Draw Knife). In a vehicle the right bumper fires the second weapon or drops bombs (run helpveh1), so the
// wheel does nothing there.
// 13 Nov 2026 (the bindings' mouse columns): a notch up or down presses each button whose binding names it (WheelUp,
// WheelDown, among the keys of a keybind_* setting: the keyboard driver itself does not know them). The right
// bumper's only on foot, as before. Until the bindings page has put the wheel in the bindings (bf_wheel_bindable),
// the wheel is what it was: Toggle Weapon on foot.
void WheelNotch(int direction) {
  static const std::pair<const char*, uint16_t> kBinds[] = {
      {"keybind_a", 0x1000},          {"keybind_b", 0x2000},           {"keybind_x", 0x4000},
      {"keybind_y", 0x8000},          {"keybind_left_shoulder", 0x0100}, {"keybind_right_shoulder", 0x0200},
      {"keybind_lstick_press", 0x0040}, {"keybind_rstick_press", 0x0080}, {"keybind_back", 0x0020},
      {"keybind_start", 0x0010},      {"keybind_dpad_up", 0x0001},     {"keybind_dpad_down", 0x0002},
      {"keybind_dpad_left", 0x0004},  {"keybind_dpad_right", 0x0008}};
  if (rex::cvar::GetFlagByName("bf_wheel_bindable") != "true") {
    if (LocalOnFoot()) PulseButton(0x0200, 120);
    return;
  }
  const std::string token = direction > 0 ? "WheelUp" : "WheelDown";
  uint16_t press = 0;
  for (const auto& [bind, bit] : kBinds) {
    const std::string keys = "," + rex::cvar::GetFlagByName(bind) + ",";
    if (keys.find("," + token + ",") == std::string::npos) continue;
    if (bit == 0x0200 && !LocalOnFoot()) continue;  // in a vehicle the right bumper fires the second weapon
    press |= bit;
  }
  if (press) PulseButton(press, 120);
}
extern std::atomic<float> g_game_fov;       // fov.cpp: the game's vertical field of view this frame (radians)
extern std::atomic<float> g_fov_now_deg, g_fov_own_deg;  // fov.cpp: the field of view controller's now and own
#if defined(BF_DEV_TOOLS) && defined(_WIN32)
namespace dev {
void WatchHost(const void* address, double seconds, const char* label);  // dev_watch.cpp
}
#endif
}  // namespace bf1943

namespace bf1943 {
double TickSeconds();  // tick_rate.cpp
}  // namespace bf1943

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kAimFromMovement = 0x822B28FC;  // the aim update's return address in sub_822B2680
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
// Radians per count at sensitivity 1: 0.066 degrees (King Kong's; about 34 cm of a 1600 dpi mouse a full turn).
constexpr double kRadiansPerCount = 0.066 * kPi / 180.0;
// The longest gap between frames or aim updates still turned through (a hitch); longer gaps turn nothing.
constexpr double kMaxStep = 0.1;
// In a vehicle the mouse pushes the right stick by its movement over the last game tick (1/30 s), not by what came
// since the last pad read: the game reads the pad on every frame but its vehicles take the stick once a tick, so at
// 110 FPS most of the movement went to reads the vehicle never used, and the turn speed changed with the frame rate.
// One game tick (tick_rate.cpp).
double kPadWindow() { return bf1943::TickSeconds(); }
// The aim update counts as running for this long after its last call (on foot; not in a vehicle, dead or a menu).
constexpr double kAimAlive = 0.25;

float LoadF(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return std::bit_cast<float>(uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]);
}
uint32_t LoadBits(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void StoreF(uint8_t* base, uint32_t a, float v) {
  const uint32_t u = std::bit_cast<uint32_t>(v);
  uint8_t* p = base + a;
  p[0] = uint8_t(u >> 24), p[1] = uint8_t(u >> 16), p[2] = uint8_t(u >> 8), p[3] = uint8_t(u);
}

double Wrap(double yaw) {
  yaw = std::fmod(yaw, kTwoPi);
  return yaw < 0 ? yaw + kTwoPi : yaw;
}
double AngleDiff(double to, double from) {  // to - from, the short way round
  double d = std::fmod(to - from, kTwoPi);
  if (d > kPi) d -= kTwoPi;
  if (d < -kPi) d += kTwoPi;
  return d;
}

// The controller's turn scale: the soldier's on foot; in a vehicle's seat the vehicle's own above 100% (under it the
// stick itself is scaled, input_settings.cpp), so the soldier's setting does not reach vehicles.
double CameraGain() {
  if (const double vehicle = bf1943::VehicleCameraGain(); vehicle > 0.0) return std::max(vehicle, 1.0);
  return std::clamp(REXCVAR_GET(bf_camera_sensitivity), 25.0, 300.0) / 100.0;
}

#if defined(BF_DEV_TOOLS)
bool DevLog() {
  static const bool on = std::getenv("BF_DEV_LOOK") != nullptr;
  return on;
}
// BF_DEV_MOUSE="<counts/s across>,<counts/s down>": the mouse moves that fast, without Keyboard & mouse (King Kong's
// KK_DEV_MOUSE), from BF_DEV_MOUSE_AT seconds after the first aim update, for BF_DEV_MOUSE_FOR seconds.
struct DevMouse {
  double x = 0, y = 0, at = 0, seconds = 1e9;
  bool on = false;
};
const DevMouse& GetDevMouse() {
  static const DevMouse m = [] {
    DevMouse d;
    if (const char* v = std::getenv("BF_DEV_MOUSE"); v && *v) {
      d.on = true;
      d.x = std::atof(v);
      if (const char* comma = std::strchr(v, ',')) d.y = std::atof(comma + 1);
      if (const char* at = std::getenv("BF_DEV_MOUSE_AT")) d.at = std::atof(at);
      if (const char* f = std::getenv("BF_DEV_MOUSE_FOR")) d.seconds = std::atof(f);
    }
    return d;
  }();
  return m;
}
#endif

// Mouse look: Keyboard & mouse with the mouse camera, the pointer locked to the game, our menu closed.
bool MouseLook() {
#if defined(BF_DEV_TOOLS)
  if (GetDevMouse().on) return true;
#endif
  return REXCVAR_GET(mnk_mode) && REXCVAR_GET(mnk_mouse) && rex::input::mnk::IsMouseLookActive();
}

struct State {
  std::mutex mutex;
  rex::ui::Window* window = nullptr;
  double raw_dx = 0, raw_dy = 0;  // window counts (times the display density) not yet taken by a frame
  double vehicle_dx = 0, vehicle_dy = 0;  // window counts not yet taken by the vehicle mouse (vehicle_mouse.cpp)
  Clock::time_point vehicle_moved{};      // the mouse's last movement
  Clock::time_point vehicle_taken{};      // the vehicle mouse's last take (TakeVehicleMouse)
  Clock::time_point stick_moved{};        // the controller's right stick last pushed (not by the mouse)
  // The mouse's movements over the last kPadWindow, for the right stick push in a vehicle (window counts).
  struct PadMove {
    Clock::time_point at;
    double dx, dy;
  };
  std::deque<PadMove> pad_moves;
  uint64_t vehicle_pad_tick = ~0ull;
  Clock::time_point vehicle_pad_at{};
  double vehicle_pad_dx = 0, vehicle_pad_dy = 0;
  bool vehicle_pad_seated = false;
  // Aiming down sights or through a scope (the view narrower than the camera's own), and the camera's own field of
  // view (radians).
  bool zooming = false;
  double hip_fov = 0;
#if defined(BF_DEV_TOOLS)
  double dbg_blend = 0;  // BF_DEV_LOOK: the zoom fields as the last aim update saw them
  int dbg_flag54 = 0, dbg_zoom_flag = 0;
  double dbg_now_deg = 0, dbg_own_deg = 0;  // the field of view controller's +0x30 and +0x28
#endif
  // The local soldier: its aim and movement objects (guest addresses), learned from the aim update.
  uint32_t aim = 0, soldier = 0;
  uint32_t entity = 0;            // the soldier entity the getters are asked about (their this minus 288)
  uint64_t frame_id = 0, refreshed_frame = ~0ull;
  uint64_t foot_camera_frame = ~0ull >> 1;  // the last frame the on foot camera controller ran (kFootCameraCaller)
  uint64_t ticks_total = 0;  // BF_DEV_CAMFRAMES
  Clock::time_point last_aim{};   // last aim update
  Clock::time_point tick_at{};    // the frame time of the last aim update
  Clock::time_point frame_at{};   // this frame's time (latched at VdSwap)
  bool frame_valid = false;
  // Turn not yet handed to the game: the mouse part (radians) and the stick's speed (radians a second).
  double mouse_yaw = 0, mouse_pitch = 0;
  double rate_yaw = 0, rate_pitch = 0;
  // This frame's offsets for the getters.
  double off_yaw = 0, off_pitch = 0;
  // Pitch limits and the zoom's scale from the last aim update.
  double pitch_lo = -1.5, pitch_hi = 1.5, zoom = 1.0, pitch_now = 0;
  // The yaw and pitch the last update wrote (to spot the game setting them itself: a spawn, a vehicle).
  double wrote_yaw = 0, wrote_pitch = 0;
  bool wrote = false;
  // While the movement tick copies the new aim to the soldier: the old values' bits and the turn handed over.
  bool copying = false;
  uint32_t old_yaw_bits = 0, old_pitch_bits = 0;
  double copy_yaw = 0, copy_pitch = 0;
  bool menu_mouse_off = false;
#if defined(BF_DEV_TOOLS)
  // BF_DEV_LOOK: a second's numbers.
  Clock::time_point log_at = Clock::now();
  int frames = 0, ticks = 0, changed_frames = 0, refreshes = 0;
  double turned = 0, pitched = 0, counts = 0, count_y = 0, game_turn = 0, game_pitch = 0;
  uint32_t last_view_bits = 0;
  std::map<uint32_t, std::pair<int, int>> callers;
  Clock::time_point first_aim{};
#endif
};
State g;
std::atomic<uint32_t> g_dev_eye{0};

bool Alive(const State& s, Clock::time_point now) {
  return s.aim && std::chrono::duration<double>(now - s.last_aim).count() < kAimAlive;
}

double Density(const State& s) {
  if (!s.window || !s.window->GetMediumDpi()) return 1.0;
  const double d = double(s.window->GetDpi()) / s.window->GetMediumDpi();
  return d > 0.25 ? d : 1.0;
}

#if defined(BF_DEV_TOOLS)
// A scripted test run (BF_DEV_SCRIPTED=1, tools/run_game.ps1): the PC's own mouse is kept out of the match as it is
// out of the launcher (game_menu.cpp). A test window at the back still gets the pointer's moves over any part of it
// that shows, and with the developer mouse on (MouseLook) they turned a tank's and an AA gun's aim in the middle of
// a test (runs v41tank1 and v41aa, 13 Nov 2026: the prompts changed to the keyboard's at the same moment).
bool ScriptedRun() {
  static const bool on = [] {
    const char* v = std::getenv("BF_DEV_SCRIPTED");
    return v && *v == '1';
  }();
  return on;
}
#endif

class Listener : public rex::ui::WindowInputListener {
 public:
  void OnMouseMove(rex::ui::MouseEvent& e) override {
#if defined(BF_DEV_TOOLS)
    if (ScriptedRun()) return;
#endif
    if ((e.dx() == 0.0f && e.dy() == 0.0f) || !MouseLook() || bf1943::GameMenuOpen() || bf1943::SpawnMouseActive() ||
        bf1943::comms::Typing())
      return;
    // A move of two counts or more is the player's (a hand resting on the mouse jitters by one): the HUD's prompts
    // show keys (game_menu.cpp NoteMatchDevice).
    if (std::fabs(e.dx()) + std::fabs(e.dy()) >= 2.0f) bf1943::NoteMatchDevice(true);
    std::lock_guard lock(g.mutex);
    g.raw_dx += e.dx();
    g.raw_dy += e.dy();
    g.vehicle_dx += e.dx(), g.vehicle_dy += e.dy(), g.vehicle_moved = Clock::now();
    g.pad_moves.push_back({Clock::now(), e.dx(), e.dy()});
    if (g.pad_moves.size() > 4096) g.pad_moves.pop_front();  // no pad reads (a loading screen): keep it bounded
  }
  // Keyboard & mouse: the wheel toggles the weapon, a notch a press (WheelNotch).
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
#if defined(BF_DEV_TOOLS)
    if (ScriptedRun()) return;
#endif
    if (!e.scroll_y() || !MouseLook() || bf1943::GameMenuOpen() || bf1943::comms::Typing()) return;
    bf1943::WheelNotch(e.scroll_y() > 0 ? 1 : -1);
  }
};
Listener g_listener;

// The pitch with the turn added, inside the limits.
double ClampPitch(const State& s, double pitch) { return std::clamp(pitch, s.pitch_lo, s.pitch_hi); }

// The mouse's scale for the zoom: while aiming down sights or through a scope, how much the zoom narrows the view
// (the tangent of half the field of view against the unzoomed one), times Aiming sensitivity. A mouse movement then
// moves the view by the same distance on screen as without zoom, for every sight and scope, following the game's own
// zoom in and out as it blends. Before, the mouse took the game's zoomed turn multiplier, which the game sets for the
// stick and which is not the zoom's own ratio.
double MouseZoom(const State& s) {
  if (!s.zooming) return 1.0;
  const double cur = bf1943::g_game_fov.load(std::memory_order_relaxed), hip = s.hip_fov;
  const double user = std::clamp(REXCVAR_GET(bf_mouse_zoom_sensitivity), 25.0, 200.0) / 100.0;
  if (!(cur > 0.01 && hip > 0.01 && cur < 3.1 && hip < 3.1)) return s.zoom * user;  // no view yet: the game's own
  return std::clamp(std::tan(cur * 0.5) / std::tan(hip * 0.5), 0.02, 1.0) * user;
}

}  // namespace

namespace bf1943 {

void InstallLook(rex::ui::Window* window) {
  // The mouse turns the soldier itself (above) and aims or steers in vehicles (LookPad): the runtime's mouse to right
  // stick part stays at its lowest, under the game's dead zone. (The pull request asked for 0, which needs a runtime
  // DLL with a wider range for this setting; 0.01 does the same with the DLL every player has, 13 Nov 2026.)
  rex::cvar::SetFlagByName("mnk_sensitivity", "0.010000");
  std::lock_guard lock(g.mutex);
  if (!window || g.window) return;
  g.window = window;
  window->AddInputListener(&g_listener, 1000);  // ahead of the runtime's, which still gets every event
}

// Once a frame (VdSwap, frame_stats.cpp): this frame's time, the mouse since the last frame, and the offsets the
// getters give until the next frame.
void LookFrame() {
  const auto now = Clock::now();
  std::lock_guard lock(g.mutex);
  const double since = g.frame_valid ? std::chrono::duration<double>(now - g.frame_at).count() : 0.0;
  g.frame_at = now;
  g.frame_valid = true;
  g.frame_id++;
  // Our menu takes the mouse back from the game (the pointer is free to pick rows), and gives it back on closing.
  // So does the deploy screen's pointer (flash_mouse.cpp).
  const bool menu = GameMenuOpen() || SpawnMouseActive();
  if (menu != g.menu_mouse_off) {
    g.menu_mouse_off = menu;
    g.pad_moves.clear();
    g.vehicle_dx = g.vehicle_dy = 0;
    g.vehicle_pad_tick = ~0ull;
    g.vehicle_pad_at = {};
    ResetVehicleMouse();
    ResetPlaneMouse();
    rex::input::mnk::SetMouseLookActive(!menu);
  }
#if defined(BF_DEV_TOOLS)
  const double dev_t = std::chrono::duration<double>(now - g.first_aim).count();
  if (GetDevMouse().on && Alive(g, now) && since < kMaxStep && !menu && dev_t > GetDevMouse().at &&
      dev_t < GetDevMouse().at + GetDevMouse().seconds) {
    const double d = Density(g);
    g.raw_dx += GetDevMouse().x * since * d;
    g.raw_dy += GetDevMouse().y * since * d;
  }
  // The developer mouse in a vehicle too (BF_DEV_MOUSE_VEHICLE=1): it moves while the soldier's aim is not running.
  // BF_DEV_MOUSE_VEHICLE=<moving s>,<still s>: it moves and stops in turns (to see a turret settle).
  static const bool dev_vehicle = std::getenv("BF_DEV_MOUSE_VEHICLE") != nullptr;
  static const double dev_on = [] {
    const char* v = std::getenv("BF_DEV_MOUSE_VEHICLE");
    return v && std::strchr(v, ',') ? std::atof(v) : 0.0;
  }();
  static const double dev_off = [] {
    const char* v = std::getenv("BF_DEV_MOUSE_VEHICLE");
    return v && std::strchr(v, ',') ? std::atof(std::strchr(v, ',') + 1) : 0.0;
  }();
  const bool dev_moving = dev_on <= 0 || std::fmod(dev_t, dev_on + dev_off) < dev_on;
  if (GetDevMouse().on && dev_vehicle && dev_moving && (g.vehicle_pad_seated || !Alive(g, now)) && since < kMaxStep && !menu)
  {
    g.pad_moves.push_back({now, GetDevMouse().x * since * Density(g), GetDevMouse().y * since * Density(g)});
    g.vehicle_dx += GetDevMouse().x * since * Density(g), g.vehicle_dy += GetDevMouse().y * since * Density(g);
    g.vehicle_moved = now;
  }
#endif
  const double dx = g.raw_dx, dy = g.raw_dy;
  g.raw_dx = g.raw_dy = 0;
  if (!REXCVAR_GET(bf_look) || g.vehicle_pad_seated || !Alive(g, now) || menu) {
    g.mouse_yaw = g.mouse_pitch = g.off_yaw = g.off_pitch = 0;
    g.rate_yaw = g.rate_pitch = 0;
    return;
  }
  g.pad_moves.clear();  // on foot the mouse turns the soldier, not the stick
  // The mouse: a fixed angle per count. Right is a larger yaw (as the stick pushed right, run look1).
  const double k =
      kRadiansPerCount * std::clamp(REXCVAR_GET(bf_mouse_sensitivity), 0.05, 10.0) / Density(g) * MouseZoom(g);
  g.mouse_yaw += dx * k;
  g.mouse_pitch += -dy * k * (REXCVAR_GET(bf_mouse_invert_y) ? -1.0 : 1.0);
  const double el = std::min(std::chrono::duration<double>(now - g.tick_at).count(), kMaxStep);
  g.off_yaw = g.mouse_yaw + g.rate_yaw * el;
  // The pitch stays inside the limits; mouse movement past a limit is dropped.
  const double want = g.pitch_now + g.mouse_pitch + g.rate_pitch * el;
  const double got = ClampPitch(g, want);
  g.mouse_pitch -= want - got;
  g.off_pitch = got - g.pitch_now;
#if defined(BF_DEV_TOOLS)
  g.frames++;
  g.counts += dx / Density(g);
  g.count_y += dy / Density(g);
#endif
}

// For vehicle_mouse.cpp: the mouse's counts since the last call (not times the display density), and the seconds since
// it last moved. False without mouse look.
bool MouseStickSeat();  // plane_mouse.cpp: a plane, or a seat whose gun the mouse's stick turns
bool VehicleMouseTurretSeat();                                                           // vehicle_mouse.cpp
bool VehicleMouseInverted();  // vehicle_mouse.cpp: Mouse vertical in vehicles (same as on foot, normal, inverted)
bool GameVerticalLookInverted();  // game_menu.cpp: the game's own Vertical look (it negates the stick's y in vehicles)
bool TickBlendVehicleView();                                                             // tick_blend.cpp
void NoteSeatLook(double dx, double dy);                                                 // native_input.cpp

// Vehicle free look (bf_vehicle_free_look): the view's turn from where the game points it, yaw right and pitch up in
// radians, set by the pad hook and read by tick_blend.cpp's view hook on the main thread (lock free: that hook holds
// tick_blend's lock, and this file's lock is held where tick_blend is called).
namespace {
std::atomic<double> g_free_yaw{0.0}, g_free_pitch{0.0};
std::atomic<int64_t> g_free_at{0};  // steady clock nanoseconds of the pad hook's last free look seat
constexpr double kFreePitch = 70.0 * 3.14159265358979323846 / 180.0;
}  // namespace
bool VehicleFreeLook(double& yaw, double& pitch) {
  const int64_t at = g_free_at.load(std::memory_order_acquire);
  if (!at || Clock::now().time_since_epoch().count() - at > std::chrono::nanoseconds(std::chrono::milliseconds(200)).count())
    return false;
  yaw = g_free_yaw.load(std::memory_order_relaxed), pitch = g_free_pitch.load(std::memory_order_relaxed);
  return true;
}
bool TakeVehicleMouse(double& dx, double& dy, double& idle) {
  std::lock_guard lock(g.mutex);
  const double density = Density(g);
  dx = g.vehicle_dx / density, dy = g.vehicle_dy / density;
  g.vehicle_dx = g.vehicle_dy = 0;
  // The mouse's movement while no seat's turning part took it (on foot, a driver's seat, a menu) is not the gun's:
  // taken at a seat's first step it swung the gun as far as the mouse had moved since the last seat (12 Nov 2026,
  // runs vmseat1 and vmboat1: a gun went to its stop as the seat was changed). Only what came since the last take,
  // when that was a moment ago (the parts take it twice a tick).
  const auto now = Clock::now();
  if (now - g.vehicle_taken > std::chrono::milliseconds(250)) dx = dy = 0;
  g.vehicle_taken = now;
  idle = g.vehicle_moved == Clock::time_point{} ? 1e9
                                                : std::chrono::duration<double>(Clock::now() - g.vehicle_moved).count();
  return MouseLook();
}

// Supplies vehicle aiming and the mouse-to-stick fallback when the soldier's aim is not running.
// `gamepad` is the game's X_INPUT_GAMEPAD (big endian).
void LookPad(uint8_t* gamepad, bool seated) {
  std::lock_guard lock(g.mutex);
  const auto now = Clock::now();
  // A delayed vehicle tick can consume pending motion, but an input pause drops the old backlog.
  const double max_age = seated ? 0.1 : kPadWindow();
  while (!g.pad_moves.empty() && std::chrono::duration<double>(now - g.pad_moves.front().at).count() > max_age)
    g.pad_moves.pop_front();
#if defined(BF_DEV_TOOLS)
  // BF_DEV_LOOK: how often the game reads the pad, a line every 5 s.
  if (DevLog()) {
    static Clock::time_point since = now;
    static int reads = 0;
    ++reads;
    if (now - since > std::chrono::seconds(5)) {
      REXLOG_INFO("BF look pad: {:.1f} pad reads a second", reads / std::chrono::duration<double>(now - since).count());
      since = now, reads = 0;
    }
  }
#endif
  // The right stick pushed by the player (not the runtime's mouse to stick, within a quarter second of a mouse
  // movement): the controller aims (aim assist, AimAssistOn).
  {
    auto axis = [&](int at) { return float(int16_t(uint16_t(gamepad[at]) << 8 | gamepad[at + 1])) / 32767.0f; };
    if (std::max(std::fabs(axis(8)), std::fabs(axis(10))) > 0.3f &&
        std::chrono::duration<double>(now - g.vehicle_moved).count() > 0.25)
      g.stick_moved = now;
  }
  // Not in the air raid's bombers (13 Nov 2026): the mouse neither steers them nor turns their view, in the bomb
  // sight or behind them; the keys and the controller steer.
  const bool mouse_active = MouseLook() && !comms::Typing() && !BomberSeat();
  // A seat where the soldier's own aim update runs (a landing boat's gunner: its gun follows the soldier's look): the
  // mouse turns that aim itself on every frame, as on foot (LookFrame and the aim hook), so it is no vehicle seat for
  // the mouse here. (The pull request took every seat: LookFrame then handed the aim hook no turn, and the hook, which
  // replaces the game's own turn with the frames', held the aim still for the mouse and a controller alike; found in
  // its review, 13 Nov 2026.)
  // The new way needs both switches on: bf_native_input (the game's concepts written) and bf_mouse_vehicle_aim (the
  // mouse turns guns). With either off the mouse in a vehicle is 0.3's: it pushes the pad's right stick (below).
  const bool native = REXCVAR_GET(bf_native_input) && REXCVAR_GET(bf_mouse_vehicle_aim);
  // A plane, or a seat whose gun has turning parts (0.4.1): the mouse's stick has it (plane_mouse.cpp), also where the
  // soldier's own aim update runs too (a gunner's seat is a soldier's controllable, seat_info.h).
  const bool stick_seat = seated && mouse_active && native && MouseStickSeat();
  const bool own_aim = Alive(g, now) && REXCVAR_GET(bf_look) && !stick_seat;
  const bool vehicle_mouse = seated && mouse_active && !own_aim && native;
  if (vehicle_mouse != g.vehicle_pad_seated || !mouse_active) {
    g.pad_moves.clear();
    g.vehicle_dx = g.vehicle_dy = 0;
    g.vehicle_pad_tick = ~0ull;
    g.vehicle_pad_at = {};
    g_free_yaw.store(0.0), g_free_pitch.store(0.0), g_free_at.store(0);
    if (g.vehicle_pad_seated) ResetVehicleMouse(), ResetPlaneMouse();
    g.vehicle_pad_seated = vehicle_mouse;
  }
  if (!mouse_active || own_aim) return;
  // In a vehicle (native mouse and keyboard): the mouse never pushes the pad. Its stick flies a plane and turns a
  // seat's gun (plane_mouse.cpp), written into the game's input concepts (native_input.cpp), or it turns the view in a
  // seat without a gun (free look, tick_blend.cpp). A controller's sticks are left as they are.
  if (vehicle_mouse) {
    double pdx = 0, pdy = 0;
    for (const auto& m : g.pad_moves) pdx += m.dx, pdy += m.dy;
    g.pad_moves.clear();
    g.vehicle_pad_tick = ~0ull;
    g.vehicle_pad_at = {};
    const double d = Density(g);
    if (PlaneMouse(gamepad, pdx / d, pdy / d)) return;
    if (VehicleMouseTurretSeat()) {  // a gun's seat between its parts' steps: nothing but the stick writes there
      g_free_at.store(0);
      return;
    }
    // No free look (it is off, or this view is not one the view hook turns): the seat's own look, natively.
    if (!REXCVAR_GET(bf_vehicle_free_look) || !TickBlendVehicleView()) {
      NoteSeatLook(pdx / d, pdy / d);
      g_free_at.store(0);
      return;
    }
    // Free look (bf_vehicle_free_look): the mouse turns the view itself, as on foot, with no stick between (12 Nov 2026:
    // driver look "acts like a controller joystick"); tick_blend.cpp turns the view it hands the renderer. Only the
    // player's own view changes: nothing goes to the server. A controller's stick takes the game's free look back.
    auto axis = [&](int at) { return float(int16_t(uint16_t(gamepad[at]) << 8 | gamepad[at + 1])) / 32767.0f; };
    if (std::max(std::fabs(axis(8)), std::fabs(axis(10))) > 0.3f &&
        std::chrono::duration<double>(now - g.vehicle_moved).count() > 0.25) {
      g_free_yaw.store(0.0), g_free_pitch.store(0.0), g_free_at.store(0);
      return;
    }
    const double k = kRadiansPerCount * std::clamp(REXCVAR_GET(bf_mouse_vehicle_sensitivity), 0.05, 10.0) / d;
    const double iy = VehicleMouseInverted() ? -1.0 : 1.0;  // Invert vehicle (vehicle_mouse.cpp)
    double yaw = g_free_yaw.load(), pitch = g_free_pitch.load();
    if (std::isfinite(pdx)) yaw = std::remainder(yaw + pdx * k, kTwoPi);
    if (std::isfinite(pdy)) pitch = std::clamp(pitch - pdy * k * iy, -kFreePitch, kFreePitch);
    g_free_yaw.store(yaw), g_free_pitch.store(pitch);
    g_free_at.store(now.time_since_epoch().count(), std::memory_order_release);
    return;
  }
  g_free_at.store(0);
  // Not seated (dead, watching), or a seat without a gun the 0.3 way: the mouse to the right stick, as before. Not on
  // the deploy screen with its pointer free (flash_mouse.cpp): there the mouse points and clicks.
  if (SpawnMouseActive()) {
    g.pad_moves.clear();
    return;
  }
  double dx = 0, dy = 0;
  for (const auto& m : g.pad_moves) dx += m.dx, dy += m.dy;
  if (dx == 0 && dy == 0) return;
  // The runtime's mouse to stick scale (200 per count at sensitivity 1, mnk_input_driver.cpp), a tick's movement.
  const double k = 200.0 / 32767.0 * std::clamp(REXCVAR_GET(bf_mouse_vehicle_sensitivity), 0.05, 10.0) / Density(g);
  // Past the game's own dead zone (0.15 on each axis, input_settings.cpp): what to send so that the game reads `a`,
  // or small movements did nothing.
  auto add = [](uint8_t* p, double a) {
    if (a == 0 || !std::isfinite(a)) return;
    a = std::clamp(a, -1.0, 1.0);
    const double send = std::copysign(0.15 + std::fabs(a) * 0.85, a);
    const int now = int16_t(uint16_t(p[0]) << 8 | p[1]);
    const int out = std::clamp(int(std::lround(now + send * 32767.0)), -32768, 32767);
    p[0] = uint8_t(uint16_t(out) >> 8), p[1] = uint8_t(out);
  };
  add(gamepad + 8, dx * k);
  add(gamepad + 10, -dy * k * (VehicleMouseInverted() ? -1.0 : 1.0));  // a vehicle's view: the vehicles' setting
}

}  // namespace bf1943

// The soldier's movement tick (sub_82210C10(soldier, entry input)): the aim update runs inside it, then the new yaw
// and pitch are copied to soldier+12 and +16.
// Only the tick that runs our aim update (the local soldier's) counts: the soldier is taken from it there.
thread_local uint32_t t_moving = 0;
REX_EXTERN(__imp__sub_82210C10);
REX_HOOK_RAW(sub_82210C10) {
  const uint32_t outer = t_moving;
  t_moving = ctx.r3.u32;
  __imp__sub_82210C10(ctx, base);
  const uint32_t soldier = t_moving;
  t_moving = outer;
  std::lock_guard lock(g.mutex);
  if (soldier != g.soldier) return;
  g.copying = false;
  // The aim as the tick left it (aim assist, sub_822B0F40, may have moved it after the update): the next update
  // checks against it, and the frames turn from it.
  if (g.wrote && g.aim) {
    g.wrote_yaw = LoadF(base, g.aim + 8);
    g.wrote_pitch = LoadF(base, g.aim + 0xC);
    g.pitch_now = g.wrote_pitch;
  }
}

// The aim update (sub_822B0B18(aim, input, flag)). input+0 the time step, +0xC/+0x10 the stick (concepts 4 and 5),
// +0x14/+0x18 the pitch limits in degrees, +0x54 zooming, +0x80 the zoom state; aim+0 its data (YawSpeed 0x74,
// PitchSpeed 0x68, zoomed multiplier 0x14), +8 yaw, +0xC pitch, +0x30 the zoom blend, +0x38 extra pitch limits.
namespace bf1943 {
std::atomic<uint32_t> g_aim_data{0};  // the local soldier's aimer data (HaloAimerData), for modern_mode.cpp
}
REX_EXTERN(__imp__sub_822B0B18);
REX_HOOK_RAW(sub_822B0B18) {
  const uint32_t aim = ctx.r3.u32, input = ctx.r4.u32;
  if (aim && uint32_t(ctx.lr) == kAimFromMovement) bf1943::g_aim_data.store(LoadBits(base, aim), std::memory_order_relaxed);
  const bool zoom_flag = (ctx.r5.u32 & 0xFF) != 0;
  if (uint32_t(ctx.lr) != kAimFromMovement || !aim || !input) return __imp__sub_822B0B18(ctx, base);
  const double yaw0 = LoadF(base, aim + 8), pitch0 = LoadF(base, aim + 0xC);
  const uint32_t yaw0_bits = LoadBits(base, aim + 8), pitch0_bits = LoadBits(base, aim + 0xC);
  __imp__sub_822B0B18(ctx, base);
  const double yaw1 = LoadF(base, aim + 8), pitch1 = LoadF(base, aim + 0xC);
  const double dt = LoadF(base, input);
  const auto now = Clock::now();

  std::lock_guard lock(g.mutex);
  const bool fresh = !Alive(g, now) || g.aim != aim || g.soldier != t_moving;
  g.aim = aim;
  g.soldier = t_moving;
  g.last_aim = now;
#if defined(BF_DEV_TOOLS)
  if (fresh) g.first_aim = now;
  g.ticks++;
  g.ticks_total++;
#endif
  // Pitch limits (the update's own clamp: input degrees, then the limit object) and the zoom's scale.
  double lo = LoadF(base, input + 0x14) * kPi / 180.0, hi = LoadF(base, input + 0x18) * kPi / 180.0;
  if (const uint32_t limits = LoadBits(base, aim + 0x38)) {
    lo = std::max(lo, double(LoadF(base, limits + 0xC)));
    hi = std::min(hi, double(LoadF(base, limits + 0x10)));
  }
  if (lo > hi) std::swap(lo, hi);
  g.pitch_lo = lo, g.pitch_hi = hi;
  double zoom = 1.0;
  const double zoom_blend = LoadF(base, aim + 0x30);
  if (base[input + 0x54] && zoom_flag) {
    const uint32_t data = LoadBits(base, aim);
    if (data) zoom = zoom_blend * LoadF(base, data + 0x14) + (1.0 - zoom_blend);
  }
  g.zoom = std::clamp(zoom, 0.05, 1.0);
  // The mouse's zoom (MouseZoom), from the camera manager's field of view controller (side note fov_study.md), read
  // where the game sets the view each frame (fov.cpp: *(manager + 0xE0), the manager in r31 there; the static
  // object's copy at 0x82AF3BA0 + 0xE0 read zeros): +0x30 the field of view now in degrees (the zoom's blend and the
  // sprint's widening included), +0x28 the camera's own (its request C). Zooming while the view is narrower than the
  // camera's own: from the moment a sight or scope narrows it until it is back. The aim update's own zoom fields
  // (input+0x54, aim+0x30) stay 0 in this game (run kbm2: the view went from 55 to 50 degrees with them at 0).
#if defined(BF_DEV_TOOLS)
  g.dbg_blend = zoom_blend, g.dbg_flag54 = base[input + 0x54], g.dbg_zoom_flag = zoom_flag;
#endif
  {
    const double now_deg = bf1943::g_fov_now_deg.load(std::memory_order_relaxed);
    const double own_deg = bf1943::g_fov_own_deg.load(std::memory_order_relaxed);
    if (now_deg > 1.0 && now_deg < 179.0 && own_deg > 1.0 && own_deg < 179.0) {
      g.hip_fov = own_deg * kPi / 180.0;
      g.zooming = now_deg < own_deg - 0.05;
    } else {
      g.zooming = false;
    }
#if defined(BF_DEV_TOOLS)
    g.dbg_now_deg = now_deg, g.dbg_own_deg = own_deg;
#endif
  }
#if defined(BF_DEV_TOOLS)
  // BF_DEV_LOOK: each zoom's numbers once it settles: the game's stick multiplier against the view's own ratio.
  {
    static bool logged = false;
    static double previous_deg = 0;
    const bool settled = std::fabs(g.dbg_now_deg - previous_deg) < 0.01;
    previous_deg = g.dbg_now_deg;
    if (!g.zooming) logged = false;
    if (DevLog() && g.zooming && !logged && settled && g.hip_fov > 0) {
      logged = true;
      const double cur = bf1943::g_game_fov.load(std::memory_order_relaxed);
      REXLOG_INFO("BF look zoom: field of view {:.1f} to {:.1f} degrees, view ratio {:.3f}, the game's stick multiplier "
                  "{:.3f}", g.hip_fov * 180.0 / kPi, cur * 180.0 / kPi, std::tan(cur * 0.5) / std::tan(g.hip_fov * 0.5),
                  g.zoom);
    }
  }
#endif

  // The game's own turn this update.
  const double turn_yaw = AngleDiff(yaw1, yaw0) * CameraGain(), turn_pitch = (pitch1 - pitch0) * CameraGain();
#if defined(BF_DEV_TOOLS)
  g.game_turn += turn_yaw;
  g.game_pitch += turn_pitch;
#endif
  if (!REXCVAR_GET(bf_look) || bf1943::GameMenuOpen()) {
    // The Xbox 360's turning, scaled by the camera sensitivity.
    if (CameraGain() != 1.0) {
      StoreF(base, aim + 8, float(Wrap(yaw0 + turn_yaw)));
      StoreF(base, aim + 0xC, float(std::clamp(pitch0 + turn_pitch, lo, hi)));
    }
    g.wrote = false;
    g.mouse_yaw = g.mouse_pitch = g.rate_yaw = g.rate_pitch = g.off_yaw = g.off_pitch = 0;
    return;
  }
  // The game set the aim itself since our last update (a spawn, leaving a vehicle): start again from its value.
  const bool moved = g.wrote && (std::fabs(AngleDiff(yaw0, g.wrote_yaw)) > 1e-4 || std::fabs(pitch0 - g.wrote_pitch) > 1e-4);
  if (fresh || moved || !g.frame_valid) {
    g.mouse_yaw = g.mouse_pitch = g.off_yaw = g.off_pitch = 0;
    g.rate_yaw = g.rate_pitch = 0;
  }
  // Hand the frames' turn since the last update to the game in place of its own, and turn at the game's new speed
  // from now on.
  const double el = std::min(std::chrono::duration<double>(g.frame_at - g.tick_at).count(), kMaxStep);
  const double give_yaw = g.mouse_yaw + g.rate_yaw * std::max(el, 0.0);
  const double give_pitch_want = pitch0 + g.mouse_pitch + g.rate_pitch * std::max(el, 0.0);
  const double new_yaw = Wrap(yaw0 + give_yaw), new_pitch = std::clamp(give_pitch_want, lo, hi);
  StoreF(base, aim + 8, float(new_yaw));
  StoreF(base, aim + 0xC, float(new_pitch));
  g.wrote = true;
  g.wrote_yaw = LoadF(base, aim + 8);
#if defined(BF_DEV_TOOLS)
  bf1943::g_dev_yaw.store(float(g.wrote_yaw), std::memory_order_relaxed);
#endif
  g.wrote_pitch = LoadF(base, aim + 0xC);
  g.copying = true;
  g.old_yaw_bits = yaw0_bits, g.old_pitch_bits = pitch0_bits;
  g.copy_yaw = AngleDiff(g.wrote_yaw, yaw0), g.copy_pitch = g.wrote_pitch - pitch0;
#if defined(BF_DEV_TOOLS)
  // BF_DEV_LOOK: the updates around each wrap of the yaw (the frames' turn handed over, the game's own).
  {
    static int after_wrap = 0;
    if (yaw0 > kTwoPi - 0.5 || yaw0 < 0.5) after_wrap = 8;
    if (DevLog() && after_wrap > 0) {
      --after_wrap;
      REXLOG_INFO("BF look wrap: yaw0 {:.4f} game {:.4f} gave {:.4f} wrote {:.4f} moved {} fresh {} el {:.4f} rate {:.3f}",
                  yaw0, yaw1, give_yaw, g.wrote_yaw, moved, fresh, el, g.rate_yaw);
    }
  }
  g.turned += g.copy_yaw;
  g.pitched += g.copy_pitch;
#endif
  g.mouse_yaw = g.mouse_pitch = 0;
  g.off_yaw = g.off_pitch = 0;
  g.tick_at = g.frame_at;
  g.pitch_now = g.wrote_pitch;
  g.rate_yaw = dt > 1e-4 ? turn_yaw / dt : 0.0;
  g.rate_pitch = dt > 1e-4 ? turn_pitch / dt : 0.0;
#if defined(BF_DEV_TOOLS) && defined(_WIN32)
  // BF_DEV_WATCH=<aim updates>[:<offset>[:char]]: a hardware watch on the aim's yaw (offset 8, or the offset given in
  // hex; with "char", in the soldier's movement object) for 3 seconds: every function that reads or writes it.
  static const std::string watch = [] {
    const char* v = std::getenv("BF_DEV_WATCH");
    return std::string(v ? v : "");
  }();
  static long updates = 0;
  if (!watch.empty() && ++updates == std::atol(watch.c_str())) {
    const size_t colon = watch.find(':');
    const uint32_t offset = colon == std::string::npos ? 8 : uint32_t(std::strtoul(watch.c_str() + colon + 1, nullptr, 16));
    const bool character = watch.find("char") != std::string::npos, eye = watch.find("eye") != std::string::npos;
    const uint32_t object = eye ? g_dev_eye.load() : character ? g.soldier : aim;
    REXLOG_INFO("BF watch on {:08X} + {:X}", object, offset);
    if (object) bf1943::dev::WatchHost(base + object + offset, 3.0, character ? "soldier" : "aim");
  }
#endif
}

// Aim assist on or off (GitHub issue 4: "Aim assist toggle in Settings > Controls"), one setting for a controller and
// one for the mouse. The game's aim assist is sub_822B0F40(aim, input, flag), called by sub_822B2680 in the soldier's
// movement tick right after the aim update: it pulls the aim only with input+0x54 set (a byte of the entry input) and
// the flag set; the flag is the game's own AimAssist option (GstGameplay AimAssist, read by name there each tick, 1
// in every profile; its English pages never showed it). For the local soldier the flag is cleared when the setting of
// the device that aimed last (the mouse moved after the right stick was pushed: the mouse) is off; the aim is then
// the player's alone. Other soldiers (a server's) keep the game's.
REXCVAR_DEFINE_BOOL(bf_aim_assist, true, "BF1943/Controls", "Aim assist with a controller (the game's own)");
// 13 Nov 2026 ("disable aim-assist from mouse and keyboard controls, and slightly reduce the strength of aim-assist
// for controllers"): the mouse never has it (this setting is no longer read; it stays so an old settings file loads),
// and a controller's pull is scaled: the aim's turn by the game's assist in a tick, times bf_aim_assist_strength.
REXCVAR_DEFINE_BOOL(bf_aim_assist_mouse, false, "BF1943/Controls",
                    "Not used: the mouse has no aim assist (it was the game's own, made for controllers)");
REXCVAR_DEFINE_DOUBLE(bf_aim_assist_strength, 75.0, "BF1943/Controls",
                      "A controller's aim assist, 0 to 100 (% of the console's pull on the aim)");
namespace {
struct AssistStats {
  uint64_t calls = 0, armed = 0, cleared = 0;  // the local soldier's calls, with input+0x54 set, and the flag cleared
  Clock::time_point since = Clock::now();
} g_assist;
}  // namespace
REX_EXTERN(__imp__sub_822B0F40);
REX_HOOK_RAW(sub_822B0F40) {
  const uint32_t aim = ctx.r3.u32, input = ctx.r4.u32;
  bool mine = false, mouse = false;
  {
    std::lock_guard lock(g.mutex);
    mine = aim && aim == g.aim && uint32_t(ctx.lr) == 0x822B290C;  // sub_822B2680's call, the local soldier's aim
    mouse = mine && MouseLook() && g.vehicle_moved != Clock::time_point{} && g.vehicle_moved > g.stick_moved;
    if (mine) {
      ++g_assist.calls;
      if (input && base[input + 0x54]) ++g_assist.armed;
      const bool on = !mouse && REXCVAR_GET(bf_aim_assist);
      if (!on && (ctx.r5.u32 & 0xFF)) ctx.r5.u64 = 0, ++g_assist.cleared;
      // A line a minute while it acts or is turned off: how often the game would have pulled the aim.
      const auto now = Clock::now();
      if (now - g_assist.since > std::chrono::seconds(60)) {
        if (g_assist.armed || g_assist.cleared)
          REXLOG_INFO("BF aim assist: {} ticks in the last minute, {} with the game's assist armed, {} turned off by "
                      "the settings (aiming with the {}; controller {}, mouse {})",
                      g_assist.calls, g_assist.armed, g_assist.cleared, mouse ? "mouse" : "controller",
                      REXCVAR_GET(bf_aim_assist) ? "on" : "off", "never");
        g_assist = AssistStats{};
      }
    }
  }
  // A controller's assist at less than the console's strength: what the call turned the aim by, scaled.
  const double strength = std::clamp(REXCVAR_GET(bf_aim_assist_strength), 0.0, 100.0) / 100.0;
  const bool scale = mine && !mouse && strength < 0.999 && (ctx.r5.u32 & 0xFF);
  const float yaw0 = scale ? LoadF(base, aim + 8) : 0.0f, pitch0 = scale ? LoadF(base, aim + 0xC) : 0.0f;
  __imp__sub_822B0F40(ctx, base);
  if (scale) {
    const float yaw1 = LoadF(base, aim + 8), pitch1 = LoadF(base, aim + 0xC);
    const double dyaw = std::remainder(double(yaw1) - yaw0, 6.283185307179586), dpitch = double(pitch1) - pitch0;
    if (dyaw != 0.0 || dpitch != 0.0) {
      StoreF(base, aim + 8, float(Wrap(yaw0 + dyaw * strength)));
      StoreF(base, aim + 0xC, float(pitch0 + dpitch * strength));
    }
  }
}

namespace {
// A getter's answer for the local soldier: its value plus the turn not yet handed over. `field` is the soldier's own
// copy (+12 yaw, +16 pitch); `bits` the getter's answer.
double Offset(const uint8_t* base, uint32_t field, uint32_t bits, bool yaw) {
  std::lock_guard lock(g.mutex);
  if (!g.soldier || !REXCVAR_GET(bf_look)) return 0;
  // Between the aim update and the movement tick's copy the soldier still holds the old values: add what was handed.
  if (g.copying && bits == (yaw ? g.old_yaw_bits : g.old_pitch_bits))
    return (yaw ? g.copy_yaw + g.off_yaw : g.copy_pitch + g.off_pitch);
  if (bits != LoadBits(base, g.soldier + field)) return 0;  // another soldier
  return yaw ? g.off_yaw : g.off_pitch;
}
}  // namespace

// The soldier entity's aim transform (sub_822047F8(entity)): from the two getters it builds the aim matrix at
// entity+688 (and a vector at +736). The game builds it once a tick (from sub_8220C310), and the first person
// camera copies it every frame (sub_82201188, below; sub_821F99B0 reads it too). A hardware watch found no other
// writer (run watch4).
REX_EXTERN(__imp__sub_822047F8);
REX_HOOK_RAW(sub_822047F8) {
#if defined(BF_DEV_TOOLS)
  if (std::getenv("BF_DEV_WATCH")) g_dev_eye = ctx.r3.u32;
#endif
  __imp__sub_822047F8(ctx, base);
}

namespace {
// Walking on every frame (bf_look_motion). The soldier moves only in the game's 30 Hz tick, so its aim matrix's
// position (entity+688, row 3: +48, +52, +56), which the first person camera and the gun are placed from, stepped 30
// times a second while the view turned on every frame: walking while turning, the gun and the world in front shook
// (TekRantGaming: "as if the camera was vibrating", the door frame juddering; run judder3: the camera moved 0.12 units every
// 3 to 4 frames at 110 FPS, 0.002 on the frames between). Each frame the position goes on along the last tick's
// movement for the time since that tick (at most one tick); the next tick writes the game's own position again.
// Nothing reaches the server (its soldier never runs this), and the aim and the gun keep the game's own placement.
struct Motion {
  uint32_t entity = 0;
  float tick[3] = {}, prev[3] = {}, written[3] = {};
  bool have_tick = false, have_prev = false, wrote = false;
  // The game's time since its last tick, as its own accumulator would hold it: each frame adds the frame's time, each
  // tick takes one tick off (the remainder carries over), so a frame between ticks sits where the game's time does.
  double phase = 0;
  Clock::time_point last_now{};
  uint64_t last_frame = ~0ull;
  uint64_t ticks_seen = 0;  // the client's tick count at the last frame (bf1943::ClientTicks)
} g_motion;

double kTick() { return bf1943::TickSeconds(); }
// A step longer than this in one tick is not walking (a respawn, a teleport, a vehicle): no motion carried.
constexpr double kMaxTickStep = 0.6;
// Blend, not extrapolation (TekRantGaming's ruling, 6 Nov: King Kong's rule allows no guessing ahead): each frame the position
// is the last two ticks' positions blended by the game's time since the last tick, so the view is one tick (33 ms)
// behind the game's soldier and never overshoots. The extrapolation it replaces carried the walk only (a step over
// 0.04 a tick horizontally): the idle bob overshot and came back, so standing still was left at 30 Hz, and jumping in
// place too (TekRantGaming, sixth test: "When jumping whilst not walking the hands and gun judder"); landing, it dipped into
// the ground for a frame. The turn is not blended: it stays this frame's (below).

void CarryMotion(uint8_t* base, uint32_t entity, Clock::time_point now, uint64_t frame) {
  Motion& m = g_motion;
  const uint32_t pos = entity + 688 + 48;
  float cur[3] = {LoadF(base, pos), LoadF(base, pos + 4), LoadF(base, pos + 8)};
  if (m.entity != entity) m = Motion{}, m.entity = entity, m.ticks_seen = bf1943::ClientTicks();
  if (frame != m.last_frame) {  // once a frame (the camera transform is asked for twice)
    if (m.last_now != Clock::time_point{}) m.phase += std::chrono::duration<double>(now - m.last_now).count();
    m.last_now = now;
    m.last_frame = frame;
    // The game's ticks since the last frame, counted (below 30 FPS a frame holds two at times: taking one tick off for
    // each new position seen let the phase run up to two ticks and start again, a jump back every fifth frame at 24
    // FPS, run walkslow2). The game's own remainder after them is under a tick, so ours is never more.
    const uint64_t ticks = bf1943::ClientTicks();
    const uint64_t n = std::min<uint64_t>(ticks - m.ticks_seen, 8);
    m.ticks_seen = ticks;
    if (n > 0) {
      m.phase = std::max(0.0, m.phase - double(n) * kTick());
      const bool ours = m.wrote && cur[0] == m.written[0] && cur[1] == m.written[1] && cur[2] == m.written[2];
      if (ours) {
        // The ticks left the position as it was: no movement to carry.
        std::memcpy(cur, m.have_tick ? m.tick : cur, sizeof(cur));
        if (m.have_tick) std::memcpy(m.prev, m.tick, sizeof(m.prev)), m.have_prev = true;
      } else if (m.have_tick) {
        // The last tick's own step: the movement since the last frame's tick over the ticks in it.
        for (int k = 0; k < 3; ++k) m.prev[k] = float(cur[k] - (cur[k] - m.tick[k]) / double(n));
        m.have_prev = true;
      }
      std::memcpy(m.tick, cur, sizeof(m.tick));
      m.have_tick = true;
    }
    if (m.phase > kTick() * 0.98) m.phase = kTick() * 0.98;
  }
  m.wrote = false;
  if (!m.have_prev || !REXCVAR_GET(bf_look_motion)) return;
  const double dx = m.tick[0] - m.prev[0], dy = m.tick[1] - m.prev[1], dz = m.tick[2] - m.prev[2];
  const double step = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (step > kMaxTickStep || step == 0) return;
  const double f = std::clamp(m.phase / kTick(), 0.0, 1.0);
  // From the previous tick's position toward the last one's (f 0 just after a tick, near 1 before the next).
  m.written[0] = float(m.prev[0] + dx * f);
  m.written[1] = float(m.prev[1] + dy * f);
  m.written[2] = float(m.prev[2] + dz * f);
  StoreF(base, pos, m.written[0]);
  StoreF(base, pos + 4, m.written[1]);
  StoreF(base, pos + 8, m.written[2]);
  m.wrote = true;
}
}  // namespace

// The first person camera's transform (sub_82201188(entity, out matrix)): it starts from the aim matrix at
// entity+688. For the local soldier, while there is a turn not yet handed to the game, the game first rebuilds
// that matrix with its own sub_822047F8, whose getters now give this frame's yaw and pitch, so the camera turns on
// every frame. Once a frame at most.
REX_EXTERN(__imp__sub_82201188);
namespace bf1943 {
// Whether a soldier entity sits in a vehicle, in any seat: sub_8220C038(soldier, on) turns its character physics on
// (on foot) and off (in a vehicle) and keeps that in bit 0x04 of entity+1016 (run seat3: 0x9E on foot, 0x92 in a
// landing boat's driver and gunner seats, 0x9E again after getting out).
bool SeatedIn(const uint8_t* base, uint32_t entity) { return entity && !(base[entity + 1016] & 0x04); }
}  // namespace bf1943

namespace {
// The camera controller sub_8228C6F0's call of the transform: every frame on foot, never in a vehicle (runs jump1 and
// ms_boat2: 6,797 calls on foot, none in 9,239 boat frames, where only the call from 0x8220C820 runs).
constexpr uint32_t kFootCameraCaller = 0x8228C770;
// This frame's aim matrix for the local soldier, before the first reader of the frame: the camera (sub_82201188) or
// the first person model (sub_821F99B0, which reads it too). Rebuilt once a frame with this frame's turn, then carried
// along the walk. Done only in the camera before, the gun read the matrix a frame behind, or the game's own tick
// values on tick frames, and shook against the view (TekRantGaming, third watched test: "the first person hands with the gun
// still significantly shakes"). Returns whether it rebuilt.
bool PrepareAim(PPCContext& ctx, uint8_t* base, uint32_t entity, const char* reader) {
  bool refresh = false;
  {
    std::lock_guard lock(g.mutex);
    if (entity && entity == g.entity && REXCVAR_GET(bf_look) && g.refreshed_frame != g.frame_id &&
        (g.off_yaw != 0 || g.off_pitch != 0)) {
      g.refreshed_frame = g.frame_id;
      refresh = true;
#if defined(BF_DEV_TOOLS)
      g.refreshes++;
#endif
    }
#if defined(BF_DEV_TOOLS)
    static const bool camframes = std::getenv("BF_DEV_CAMFRAMES") != nullptr;
    static uint64_t logged_frame = ~0ull;
    if (camframes && entity && entity == g.entity && logged_frame != g.frame_id) {
      logged_frame = g.frame_id;
      REXLOG_INFO("BF aim first: frame {} reader {}", g.frame_id, reader);
    }
#endif
  }
  if (refresh) {
    const PPCContext saved = ctx;
    ctx.r3.u64 = entity;
    __imp__sub_822047F8(ctx, base);
    ctx = saved;
  }
  {
    // Walking on every frame, on foot only (while the aim update runs).
    const auto now = Clock::now();
    std::lock_guard lock(g.mutex);
    // The phase advances by the frames' own times (VdSwap's, after the frame limit: evenly spaced). The time this call
    // came at jittered by a millisecond or two, and the per frame step with it (run jump2: 0.045 and 0.065 by turns).
    // On foot only: the soldier's camera controller (its call from kFootCameraCaller) ran in the last frames. In a
    // vehicle it does not run, and the camera stays on the game's tick: the vehicle's body steps 30 times a second, and a
    // view moved between ticks moved against it (TekRantGaming: the jeep and the boat "shake"; a marked clip showed the view a
    // tick ahead of the jeep's dashboard).
    // Not in a vehicle's gunner or passenger seat either: the soldier's own camera runs there (run seat3, a landing
    // boat's gunner), but the seat moves with the vehicle in the tick (TekRantGaming, 7 Nov: those seats still shook).
    if (entity && entity == g.entity && Alive(g, now) && g.frame_id - g.foot_camera_frame <= 2 &&
        !bf1943::SeatedIn(base, entity))
      CarryMotion(base, entity, g.frame_valid ? g.frame_at : now, g.frame_id);
  }
  return refresh;
}
}  // namespace

// The first person model's placement (sub_821F99B0(entity)) reads the aim matrix too (+688): this frame's first.
REX_EXTERN(__imp__sub_821F99B0);
REX_HOOK_RAW(sub_821F99B0) {
  PrepareAim(ctx, base, ctx.r3.u32, "model");
  __imp__sub_821F99B0(ctx, base);
}

#if defined(BF_DEV_TOOLS)
namespace bf1943 {
extern std::atomic<float> g_sea_level;  // shader_prep.cpp: the level's water height (NaN: not known yet)
}  // namespace bf1943
namespace {
// BF_DEV_CAMERA_TOUR=<spots>[:<seconds at each>[:<seed>]] (tests; TekRantGaming, 9 Nov: test all over each map, not only at
// the spawn): the local camera (only: the soldier stays where it is, nothing reaches the server) moved to random spots
// of the level by turns, two inland (ground 2 m or more above the water) for each at the coast (ground from 4 m below
// the water to 1.5 m above it), at eye height over the ground or 2.5 m over the water. The ground from the grass's
// height probe (NativeXRenderer GrassProbe), so grass must be on. It starts 8 s after the first camera in the level;
// each spot is logged ("BF dev camera: spot") for the run's screenshots to be matched by time. Turning is the match
// script's (BF_MATCH_SCRIPT, e.g. rsr held).
struct Tour {
  bool parsed = false, on = false, done = false;
  int spots = 0, index = 0, tries = 0;
  double dwell = 10;
  uint32_t seed = 1, probe = 0;
  bool asked = false, at_spot = false;
  float x = 0, y = 0, z = 0, ground = 0, probe_x = 0, probe_z = 0;
  // BF_DEV_CAMERA_POINTS: fixed stops instead, each a place and a point looked at (x, y, z, tx, ty, tz).
  std::vector<std::array<float, 6>> points;
  bool look = false;
  float look_at[3] = {};
  Clock::time_point first{}, since{};
  uint64_t frame = ~0ull;
} g_tour;

float TourRand() {
  uint32_t& s = g_tour.seed;
  s ^= s << 13, s ^= s >> 17, s ^= s << 5;
  return float(s & 0xFFFFFF) / 16777216.0f;
}

void DevCameraTour(uint8_t* base, uint32_t out, uint64_t frame) {
  Tour& t = g_tour;
  if (!t.parsed) {
    t.parsed = true;
    // BF_DEV_CAMERA_POINTS=x,y,z,tx,ty,tz;... (tests: the night maps' lights, 14 Nov 2026): the camera at each place in
    // turn, looking at its point, for the tour's dwell (BF_DEV_CAMERA_TOUR's second number, 10 s without it).
    if (const char* pts = std::getenv("BF_DEV_CAMERA_POINTS"); pts && *pts) {
      std::string all = pts;
      for (size_t pos = 0; pos < all.size();) {
        const size_t semi = std::min(all.find(';', pos), all.size());
        std::array<float, 6> q{};
        if (std::sscanf(all.substr(pos, semi - pos).c_str(), "%f,%f,%f,%f,%f,%f", &q[0], &q[1], &q[2], &q[3], &q[4], &q[5]) == 6)
          t.points.push_back(q);
        pos = semi + 1;
      }
    }
    const char* v = std::getenv("BF_DEV_CAMERA_TOUR");
    if (!t.points.empty()) {
      t.on = true;
      t.spots = int(t.points.size());
      if (v && *v)
        if (const char* c = std::strchr(v, ':')) t.dwell = std::max(1.0, std::atof(c + 1));
    } else if (v && *v) {
      t.on = true;
      t.spots = std::max(1, std::atoi(v));
      if (const char* c = std::strchr(v, ':')) {
        t.dwell = std::max(1.0, std::atof(c + 1));
        if (const char* c2 = std::strchr(c + 1, ':')) t.seed = std::max(1u, uint32_t(std::strtoul(c2 + 1, nullptr, 10)));
      }
    }
  }
  if (!t.on || t.done) return;
  const auto now = Clock::now();
  if (frame != t.frame) {  // once a frame (the camera is asked for twice)
    t.frame = frame;
    if (t.first == Clock::time_point{}) t.first = now;
    const float sea = bf1943::g_sea_level.load();
    if (now - t.first > std::chrono::seconds(8) && !std::isnan(sea)) {
      if (t.at_spot && now - t.since > std::chrono::duration<double>(t.dwell)) {
        t.at_spot = false;
        if (++t.index >= t.spots) {
          t.done = true;
          REXLOG_INFO("BF dev camera: tour done ({} spots)", t.spots);
          return;
        }
      }
      if (!t.points.empty() && !t.at_spot) {
        const auto& q = t.points[size_t(t.index)];
        t.x = q[0], t.y = q[1], t.z = q[2];
        t.look = true, t.look_at[0] = q[3], t.look_at[1] = q[4], t.look_at[2] = q[5];
        t.at_spot = true, t.since = now;
        REXLOG_INFO("BF dev camera: point {} of {} at {:.1f} {:.1f} {:.1f} looking at {:.1f} {:.1f} {:.1f}", t.index + 1, t.spots,
                    t.x, t.y, t.z, q[3], q[4], q[5]);
      }
      // The next spot, found while the camera stays at the last one: a random place, its ground asked for, kept if it
      // is the kind wanted (every third at the coast; after 400 tries any).
      if (t.points.empty() && (!t.at_spot || t.asked)) {
        uint32_t id = 0;
        float ground = 0;
        if (t.asked && native_x_renderer::GrassProbeResult(&id, &ground) && id == t.probe) {
          t.asked = false;
          ++t.tries;
          const bool coast_wanted = t.index % 3 == 2 && t.tries < 400;
          const bool inland = ground > sea + 2.0f, coast = ground > sea - 4.0f && ground < sea + 1.5f;
          if (!t.at_spot && (coast_wanted ? coast : inland || t.tries >= 400)) {
            t.x = t.probe_x, t.z = t.probe_z, t.ground = ground;
            t.y = std::max(ground + 1.7f, sea + 2.5f);
            t.at_spot = true, t.since = now, t.tries = 0;
            REXLOG_INFO("BF dev camera: spot {} of {} ({}) at {:.1f} {:.1f} {:.1f}, ground {:.1f}, water {:.1f}", t.index + 1,
                        t.spots, coast ? "coast" : "inland", t.x, t.y, t.z, ground, sea);
          }
        }
        if (!t.at_spot && !t.asked) {
          t.probe_x = -900.0f + 1800.0f * TourRand();
          t.probe_z = -900.0f + 1800.0f * TourRand();
          native_x_renderer::GrassProbe(t.probe_x, t.probe_z, ++t.probe);
          t.asked = true;
        }
      }
    }
  }
  if (t.at_spot) {
    StoreF(base, out + 48, t.x);
    StoreF(base, out + 52, t.y);
    StoreF(base, out + 56, t.z);
    if (t.look) {  // the point kept ahead, its distance and height kept, whichever way the soldier faces (the view's turn is
                   // the soldier's: its rows here are rebuilt from the aim)
      float fx = LoadF(base, out + 32), fz = LoadF(base, out + 40);
      const float n = std::sqrt(fx * fx + fz * fz);
      if (n > 1e-3f) {
        fx /= n, fz /= n;
        const float d = std::hypot(t.look_at[0] - t.x, t.look_at[2] - t.z);
        StoreF(base, out + 48, t.look_at[0] + fx * d);  // row 2 is the view's back (run nlight2)
        StoreF(base, out + 56, t.look_at[2] + fz * d);
      }
    }
  }
}
}  // namespace
#endif

REX_HOOK_RAW(sub_82201188) {
  const uint32_t entity = ctx.r3.u32;
  const bool refresh = PrepareAim(ctx, base, entity, "camera");
  if (uint32_t(ctx.lr) == kFootCameraCaller && entity) {
    std::lock_guard lock(g.mutex);
    if (entity == g.entity) g.foot_camera_frame = g.frame_id;
  }
  const uint32_t out = ctx.r4.u32;
  __imp__sub_82201188(ctx, base);
#if defined(BF_DEV_TOOLS)
  // The local camera's position, for the developer memory scan (dev_matscan.cpp).
  if (entity && out && entity == g.entity) {
    bf1943::g_dev_camera[0].store(LoadF(base, out + 48), std::memory_order_relaxed);
    bf1943::g_dev_camera[1].store(LoadF(base, out + 52), std::memory_order_relaxed);
    bf1943::g_dev_camera[2].store(LoadF(base, out + 56), std::memory_order_relaxed);
    bf1943::g_dev_camera_valid.store(true, std::memory_order_release);
  }
  // BF_DEV_CAMFRAMES=1: every local camera transform: the frame, the game tick count, refreshed or not, the
  // position (row 3) and the heading (row 2), and the aim matrix's position, for the walk and turn judder.
  {
    static const bool camframes = std::getenv("BF_DEV_CAMFRAMES") != nullptr;
    if (camframes && entity != g.entity && out && entity) {
      // Other entities' camera transforms (a vehicle's seat, for the vehicle judder): entity, caller, position.
      std::lock_guard lock(g.mutex);
      REXLOG_INFO("BF cam other: frame {} entity {:08X} lr {:08X} pos {:.4f} {:.4f} {:.4f} head {:.5f}", g.frame_id, entity,
                  uint32_t(ctx.lr), LoadF(base, out + 48), LoadF(base, out + 52), LoadF(base, out + 56),
                  std::atan2(LoadF(base, out + 40), LoadF(base, out + 32)));
    }
    if (camframes && entity == g.entity && out) {
      std::lock_guard lock(g.mutex);
      REXLOG_INFO("BF cam: frame {} at {} ticks {} refresh {} pos {:.4f} {:.4f} {:.4f} head {:.5f} aimpos {:.4f} {:.4f} "
                  "{:.4f} off {:.5f}",
                  g.frame_id,
                  std::chrono::duration_cast<std::chrono::microseconds>(g.frame_at.time_since_epoch()).count(),
                  g.ticks_total, refresh, LoadF(base, out + 48), LoadF(base, out + 52), LoadF(base, out + 56),
                  std::atan2(LoadF(base, out + 40), LoadF(base, out + 32)), LoadF(base, entity + 688 + 48),
                  LoadF(base, entity + 688 + 52), LoadF(base, entity + 688 + 56), g.off_yaw);
    }
  }
  // BF_DEV_LOOK: each camera transform near the yaw's wrap: the soldier's yaw, the offset, the aim matrix's heading
  // and the camera's (row 2's x and z).
  if (DevLog() && entity == g.entity && out) {
    std::lock_guard lock(g.mutex);
    const double v = g.soldier ? LoadF(base, g.soldier + 12) : 0.0;
    if (v > kTwoPi - 0.6 || v < 0.6)
      REXLOG_INFO("BF look cam: frame {} refresh {} yaw {:.4f} off {:.4f} aim {:.4f} cam {:.4f}", g.frame_id, refresh, v,
                  g.off_yaw, std::atan2(LoadF(base, entity + 688 + 40), LoadF(base, entity + 688 + 32)),
                  std::atan2(LoadF(base, out + 40), LoadF(base, out + 32)));
  }
  if (entity && out && entity == g.entity) {
    uint64_t frame;
    {
      std::lock_guard lock(g.mutex);
      frame = g.frame_id;
    }
    DevCameraTour(base, out, frame);
    // BF_DEV_CAMERA_AT=<x>,<y>,<z>[,<back m>[,<up m>]] (tests, D163's lamps): the local camera kept that far back from a
    // point of the level along the way it looks, that far up (default 12 and 1.7): the point, a lamppost's foot say,
    // ahead whatever the soldier faces. Turning is the match script's (rsr held). The soldier stays where it is.
    static const std::vector<float> at = [] {
      std::vector<float> v;
      if (const char* s = std::getenv("BF_DEV_CAMERA_AT"); s && *s) {
        for (const char* p = s; *p;) {
          char* end = nullptr;
          const float f = std::strtof(p, &end);
          if (end == p) break;
          v.push_back(f);
          p = *end == ',' ? end + 1 : end;
        }
      }
      return v;
    }();
    if (at.size() >= 3) {
      const float back = at.size() > 3 ? at[3] : 12.0f, up = at.size() > 4 ? at[4] : 1.7f;
      float fx = LoadF(base, out + 32), fz = LoadF(base, out + 40);
      const float n = std::sqrt(fx * fx + fz * fz);
      if (n > 1e-3f) fx /= n, fz /= n;
      // Row 2 is the view's back, not its forward (run nlight2, 14 Nov 2026: with "- back" the point was behind).
      StoreF(base, out + 48, at[0] + fx * back);
      StoreF(base, out + 52, at[1] + up);
      StoreF(base, out + 56, at[2] + fz * back);
    }
  }
#endif
}

// The soldier's yaw getter, read each frame by the camera, the gun and the body.
REX_EXTERN(__imp__sub_82200FC8);
REX_HOOK_RAW(sub_82200FC8) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_82200FC8(ctx, base);
  const float v = float(ctx.f1.f64);
  const double off = Offset(base, 12, std::bit_cast<uint32_t>(v), true);
  {
    // The local soldier's entity: a getter call with a turn added, or one that returns the local soldier's own yaw
    // (a controller, no mouse turn: without it the walk and jump blend never started; after a respawn too).
    std::lock_guard lock(g.mutex);
    if (off != 0 || (g.soldier && std::bit_cast<uint32_t>(v) == LoadBits(base, g.soldier + 12) && v != 0.0f))
      g.entity = self - 288;
  }
  // Not wrapped: the yaw wraps only in the game's own update, as before (a getter value wrapped ahead of the tick made
  // the camera step back once a turn, run lrcam3).
  if (off != 0) ctx.f1.f64 = double(float(v + off));
#if defined(BF_DEV_TOOLS)
  if (DevLog()) {
    std::lock_guard lock(g.mutex);
    const uint32_t bits = std::bit_cast<uint32_t>(float(ctx.f1.f64));
    if (off != 0 || (g.soldier && std::bit_cast<uint32_t>(v) == LoadBits(base, g.soldier + 12))) {
      if (bits != g.last_view_bits) g.changed_frames++, g.last_view_bits = bits;
    }
    // Per caller: calls, and calls that got a turn added.
    auto& c = g.callers[uint32_t(ctx.lr)];
    c.first++;
    if (off != 0) c.second++;
  }
#endif
}

// The soldier's pitch getter.
REX_EXTERN(__imp__sub_82201068);
REX_HOOK_RAW(sub_82201068) {
  __imp__sub_82201068(ctx, base);
  const float v = float(ctx.f1.f64);
  const double off = Offset(base, 16, std::bit_cast<uint32_t>(v), false);
  if (off != 0) ctx.f1.f64 = double(float(v + off));
}

namespace bf1943 {
// Whether the local soldier is on foot now: its on foot camera controller ran in the last frames (in a vehicle, dead
// or in a menu it does not). For the developer vehicle seeker (auto_join.cpp).
bool LocalOnFoot() {
  std::lock_guard lock(g.mutex);
  return g.entity && g.frame_id - g.foot_camera_frame <= 3;
}

// The local soldier sits in a vehicle (any seat).
bool LocalSeated(const uint8_t* base) {
  std::lock_guard lock(g.mutex);
  return SeatedIn(base, g.entity);
}

// The local soldier's entity (the yaw getter's self - 288), 0 before the first.
uint32_t LocalSoldierEntity() {
  std::lock_guard lock(g.mutex);
  return g.entity;
}

#if defined(BF_DEV_TOOLS)
// BF_DEV_LOOK: once a second, the turn rate and its parts (King Kong's KK_DEV_SPOTS style), from VdSwap.
void LookDevLog(const uint8_t* base) {
  if (!DevLog()) return;
  std::lock_guard lock(g.mutex);
  const auto now = Clock::now();
  const double secs = std::chrono::duration<double>(now - g.log_at).count();
  if (secs < 1.0) return;
  (void)base;
  REXLOG_INFO("BF look: {:.1f} FPS, {} aim updates, {} frames with a new view yaw; yaw {:.4f} rad/s (game's own "
              "{:.4f}), pitch {:.4f} rad/s (game's own {:.4f}), mouse {:.0f} {:.0f} counts, {:.7f} rad per count "
              "across, {:.7f} down, pitch now {:.3f}, look {}",
              g.frames / secs, g.ticks, g.changed_frames, g.turned / secs, g.game_turn / secs, g.pitched / secs,
              g.game_pitch / secs, g.counts, g.count_y, g.counts != 0 ? g.turned / g.counts : 0.0,
              g.count_y != 0 ? g.pitched / g.count_y : 0.0, g.pitch_now, REXCVAR_GET(bf_look));
  for (const auto& [lr, c] : g.callers) REXLOG_INFO("BF look: yaw getter from {:08X}: {} calls, {} turned", lr, c.first, c.second);
  g.callers.clear();
  g.log_at = now;
  REXLOG_INFO("BF look: {} camera matrix refreshes; zoom: zooming {}, field of view controller now {:.2f} own {:.2f} "
              "degrees, the view's {:.2f}; mouse scale {:.3f}, the game's stick multiplier {:.3f} (input+0x54 {} blend "
              "{:.3f})",
              g.refreshes, g.zooming, g.dbg_now_deg, g.dbg_own_deg,
              bf1943::g_game_fov.load(std::memory_order_relaxed) * 180.0 / kPi, MouseZoom(g), g.zoom, g.dbg_flag54,
              g.dbg_blend);
  g.refreshes = 0;
  g.frames = g.ticks = g.changed_frames = 0;
  g.counts = g.count_y = g.game_turn = g.game_pitch = g.turned = g.pitched = 0;
}
#endif
}  // namespace bf1943
