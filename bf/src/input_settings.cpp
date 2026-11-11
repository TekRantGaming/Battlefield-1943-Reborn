// Settings > Controls, King Kong's (kk/src/input_remap.cpp and launcher.cpp ControlsItems, tag v2.1.0), on the pad
// state the game reads (XamInputGetState, auto_join.cpp) and the rumble it sets (XamInputSetState, below):
//   Camera sensitivity  25 to 300%: the turn itself scaled, a full push included (look.cpp, at the aim update)
//   Camera response     Original passes the stick as it is, to the game's own 15% dead zone on each axis; Modern
//                       takes the dead zone on the push's length and sets each axis so the game reads that vector,
//                       so diagonals and small pushes turn the same way in every direction (CameraStick below)
//   Stick deadzone      0 to 40%, added to the game's own: smaller pushes count as none, the rest is rescaled from
//                       the edge of the dead zone
//   Camera horizontal, Camera vertical, Left stick (Normal, Invert X, Invert Y, Both): the axes flipped
//   Aim                 Hold, as on the console, or Toggle: a press of the left trigger raises the gun and the next
//                       lowers it; opening our menus lowers it
//   Vibration           on or off, and its strength 10 to 200% (above 100% stronger than the console, up to the
//                       controller's full strength)
//   Button remapping    which game button each controller button presses (bf_remap_<button>)
// The mouse turns the soldier itself (look.cpp: bf_mouse_sensitivity, bf_mouse_invert_y). Keyboard & mouse and the
// key bindings are the SDK's own settings (mnk_mode, mnk_mouse, keybind_*), set by the menu.

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <cmath>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/ppc/context.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

REXCVAR_DEFINE_DOUBLE(bf_camera_sensitivity, 100.0, "BF1943/Controls", "Right stick sensitivity, 25 to 300 (%)");
// Original by default: the game's own feel (TekRantGaming, 5 Nov); Modern is a choice.
REXCVAR_DEFINE_BOOL(bf_camera_modern, false, "BF1943/Controls", "Camera response: true Modern, false Original");
REXCVAR_DEFINE_DOUBLE(bf_deadzone, 0.0, "BF1943/Controls", "Stick dead zone, 0 to 40 (%)");
REXCVAR_DEFINE_BOOL(bf_invert_rs_x, false, "BF1943/Controls", "Camera horizontal inverted");
REXCVAR_DEFINE_BOOL(bf_invert_rs_y, false, "BF1943/Controls", "Camera vertical inverted");
REXCVAR_DEFINE_BOOL(bf_invert_ls_x, false, "BF1943/Controls", "Left stick X inverted");
REXCVAR_DEFINE_BOOL(bf_invert_ls_y, false, "BF1943/Controls", "Left stick Y inverted");
REXCVAR_DEFINE_BOOL(bf_toggle_aim, false, "BF1943/Controls", "Aim: false Hold, true Toggle");
// The mouse's own (TekRantGaming, 8 Nov 2026: the mouse has settings of its own, apart from the controller's).
REXCVAR_DEFINE_BOOL(bf_mouse_toggle_aim, false, "BF1943/Controls", "Aim with the mouse or keys: false Hold, true Toggle");
REXCVAR_DEFINE_BOOL(bf_vibration, true, "BF1943/Controls", "Controller rumble");
REXCVAR_DEFINE_DOUBLE(bf_vibration_strength, 100.0, "BF1943/Controls", "Rumble strength, 10 to 200 (%)");
REXCVAR_DEFINE_BOOL(bf_mouse_invert_y, false, "BF1943/Controls", "Mouse vertical inverted (Keyboard & mouse)");
REXCVAR_DECLARE(bool, mnk_mode);
REXCVAR_DECLARE(bool, mnk_mouse);

namespace bf1943 {
// Presses of our own on the player's pad state (the mouse wheel's weapon switch, look.cpp): each button until its
// time, read by the pad hook (auto_join.cpp), which bumps the packet number when they change so the game sees them.
namespace {
std::mutex g_pulse_mutex;
std::array<std::chrono::steady_clock::time_point, 16> g_pulse_until{};
}  // namespace
void PulseButton(uint16_t button, int ms) {
  std::lock_guard lock(g_pulse_mutex);
  for (int i = 0; i < 16; ++i)
    if (button & (1u << i)) g_pulse_until[i] = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
}
uint16_t PulsedButtons() {
  std::lock_guard lock(g_pulse_mutex);
  const auto now = std::chrono::steady_clock::now();
  uint16_t out = 0;
  for (int i = 0; i < 16; ++i)
    if (now < g_pulse_until[i]) out |= uint16_t(1u << i);
  return out;
}
}  // namespace bf1943

// Button remapping: what the game gets for each controller button (a button name, or "none").
#define BF_REMAP(name, label) REXCVAR_DEFINE_STRING(bf_remap_##name, #name, "BF1943/Controls/Remap", label);
BF_REMAP(a, "A") BF_REMAP(b, "B") BF_REMAP(x, "X") BF_REMAP(y, "Y")
BF_REMAP(lb, "Left bumper") BF_REMAP(rb, "Right bumper") BF_REMAP(lt, "Left trigger") BF_REMAP(rt, "Right trigger")
BF_REMAP(ls, "Left stick press") BF_REMAP(rs, "Right stick press") BF_REMAP(back, "Back") BF_REMAP(start, "Start")
BF_REMAP(up, "D-pad up") BF_REMAP(down, "D-pad down") BF_REMAP(left, "D-pad left") BF_REMAP(right, "D-pad right")
#undef BF_REMAP

namespace bf1943 {


// The buttons by name, with their XInput bits; the triggers are 0x10000 (left) and 0x20000 (right) here.
struct ButtonName {
  const char* name;
  uint32_t bit;
};
constexpr ButtonName kButtons[] = {
    {"a", 0x1000},     {"b", 0x2000},     {"x", 0x4000},    {"y", 0x8000},      {"lb", 0x0100},  {"rb", 0x0200},
    {"lt", 0x10000},   {"rt", 0x20000},   {"ls", 0x0040},   {"rs", 0x0080},     {"back", 0x0020}, {"start", 0x0010},
    {"up", 0x0001},    {"down", 0x0002},  {"left", 0x0004}, {"right", 0x0008},
};

namespace {

PPCFunc* RuntimeExport(const char* name) {
#if defined(_WIN32)
  static HMODULE runtime = GetModuleHandleA("rexruntime.dll");
  return reinterpret_cast<PPCFunc*>(GetProcAddress(runtime, name));
#else
  return reinterpret_cast<PPCFunc*>(dlsym(RTLD_NEXT, name));
#endif
}

uint32_t BitOf(const std::string& name) {
  for (const auto& b : kButtons)
    if (name == b.name) return b.bit;
  return 0;  // "none"
}

// What each button in kButtons presses, from the bf_remap_* settings: read twice a second, not on every pad read
// (the game reads the pad many times a frame; looking 16 settings up by name each time cost frames, run vkf_base).
const std::array<uint32_t, 16>& RemapTable() {
  using Clock = std::chrono::steady_clock;
  static std::array<uint32_t, 16> table{};
  static Clock::time_point read{};
  const auto now = Clock::now();
  if (read.time_since_epoch().count() && now - read < std::chrono::milliseconds(500)) return table;
  read = now;
  const std::string values[16] = {
      REXCVAR_GET(bf_remap_a),  REXCVAR_GET(bf_remap_b),     REXCVAR_GET(bf_remap_x),     REXCVAR_GET(bf_remap_y),
      REXCVAR_GET(bf_remap_lb), REXCVAR_GET(bf_remap_rb),    REXCVAR_GET(bf_remap_lt),    REXCVAR_GET(bf_remap_rt),
      REXCVAR_GET(bf_remap_ls), REXCVAR_GET(bf_remap_rs),    REXCVAR_GET(bf_remap_back),  REXCVAR_GET(bf_remap_start),
      REXCVAR_GET(bf_remap_up), REXCVAR_GET(bf_remap_down),  REXCVAR_GET(bf_remap_left),  REXCVAR_GET(bf_remap_right)};
  for (size_t i = 0; i < 16; ++i) table[i] = values[i].empty() ? kButtons[i].bit : BitOf(values[i]);
  return table;
}

int16_t Load16(const uint8_t* p) { return int16_t(uint16_t(p[0]) << 8 | p[1]); }
void Store16(uint8_t* p, int v) {
  const uint16_t u = uint16_t(int16_t(std::clamp(v, -32768, 32767)));
  p[0] = uint8_t(u >> 8), p[1] = uint8_t(u);
}

// One stick: the dead zone, then the push rescaled from its edge; Modern takes both radially, Original per axis.
void Stick(uint8_t* x, uint8_t* y, double scale, bool invert_x, bool invert_y) {
  double fx = Load16(x) / 32767.0, fy = Load16(y) / 32767.0;
  const double dz = std::clamp(REXCVAR_GET(bf_deadzone), 0.0, 40.0) / 100.0;
  if (REXCVAR_GET(bf_camera_modern)) {
    const double m = std::hypot(fx, fy);
    if (m <= dz || m == 0) {
      fx = fy = 0;
    } else {
      const double k = std::min(1.0, (m - dz) / (1.0 - dz) * scale) / m;
      fx *= k, fy *= k;
    }
  } else {
    auto axis = [&](double v) {
      const double a = std::fabs(v);
      return a <= dz ? 0.0 : std::copysign(std::min(1.0, (a - dz) / (1.0 - dz) * scale), v);
    };
    fx = axis(fx), fy = axis(fy);
  }
  if (invert_x) fx = -fx;
  if (invert_y) fy = -fy;
  Store16(x, int(std::lround(fx * 32767.0)));
  Store16(y, int(std::lround(fy * 32767.0)));
}

// The game's own dead zone, per axis, on the sticks it reads (sub_821C8430 on each axis of the pad: 0.15 at
// 0x82AB3500, then the rest rescaled from its edge).
constexpr double kGameDeadzone = 0.15;

// The right stick. Modern: one dead zone for the push's length (the game's 0.15 plus Stick deadzone), the rest
// rescaled, then each axis set so that after the game's own per axis dead zone the game reads exactly that vector;
// the game's aim curve already follows the push's length (sub_822B0B18), so every direction turns the same way.
// Original: the game's per axis dead zone as on the Xbox 360, with Stick deadzone added per axis.
void CameraStick(uint8_t* x, uint8_t* y, bool invert_x, bool invert_y) {
  double fx = Load16(x) / 32767.0, fy = Load16(y) / 32767.0;
  const double extra = std::clamp(REXCVAR_GET(bf_deadzone), 0.0, 40.0) / 100.0;
  if (REXCVAR_GET(bf_camera_modern)) {
    const double m = std::min(std::hypot(fx, fy), 1.0), dz = kGameDeadzone + extra;
    if (m <= dz) {
      fx = fy = 0;
    } else {
      const double len = std::min(1.0, (m - dz) / (1.0 - dz)), ux = fx / std::hypot(fx, fy), uy = fy / std::hypot(fx, fy);
      auto through = [](double a) {  // what to send so the game's per axis dead zone gives `a`
        return a == 0 ? 0.0 : std::copysign(kGameDeadzone + std::fabs(a) * (1.0 - kGameDeadzone), a);
      };
      fx = through(len * ux), fy = through(len * uy);
    }
  } else if (extra > 0) {
    auto axis = [&](double v) {
      const double a = std::fabs(v);
      return a <= extra ? 0.0 : std::copysign(std::min(1.0, (a - extra) / (1.0 - extra)), v);
    };
    fx = axis(fx), fy = axis(fy);
  }
  if (invert_x) fx = -fx;
  if (invert_y) fy = -fy;
  Store16(x, int(std::lround(fx * 32767.0)));
  Store16(y, int(std::lround(fy * 32767.0)));
}

bool g_aim_raised = false, g_aim_was_down = false;

// Keyboard & mouse in a vehicle (BF's own Help & Options > Controls, runs help3 and helpveh1: land and boat vehicles
// throttle with the left trigger, brake with the left bumper, turn with the left stick and fire their second weapon with
// the right bumper; planes throttle with the left trigger, yaw with the left stick and drop bombs with the right bumper).
// The keys that walk on foot (the left stick's up and down: W and S) are the throttle and the brake there, and the key
// that zooms on foot (the left trigger: the right mouse button) is the second weapon. Only what the keys gave is moved
// (SDK LastKey*); a controller's own sticks and triggers stay as they are.
void KeyboardInVehicle(uint8_t* gamepad) {
  const int key_ly = rex::input::mnk::LastKeyLeftStick(true);
  const bool key_lt = rex::input::mnk::LastKeyTrigger(false) != 0;
  if (!key_ly && !key_lt) return;
  uint16_t buttons = uint16_t(gamepad[0] << 8 | gamepad[1]);
  if (key_lt) buttons |= 0x0200, gamepad[2] = 0;   // zoom key: the right bumper
  if (key_ly > 0) gamepad[2] = 255;                // forward: the throttle
  if (key_ly < 0) buttons |= 0x0100;               // back: the brake (the left bumper)
  Store16(gamepad + 6, Load16(gamepad + 6) - key_ly);
  gamepad[0] = uint8_t(buttons >> 8), gamepad[1] = uint8_t(buttons);
}

}  // namespace

PPCFunc* RuntimeExportForRumble() { return RuntimeExport("__imp__XamInputSetState"); }

// The game's copy of the pad (X_INPUT_GAMEPAD at state + 4, big endian: buttons, triggers, sticks), after our menu
// took what it needed. `menu_open`: our menu has the pad (the game got an empty one); the aim toggle lowers.
// `seated`: the player sits in a vehicle (the keyboard's vehicle keys; no aim toggle, whose trigger is the throttle
// there).
void ApplyControllerSettings(uint8_t* gamepad, bool menu_open, bool seated) {
  if (menu_open) {
    g_aim_raised = g_aim_was_down = false;
    return;
  }
  if (seated) KeyboardInVehicle(gamepad);
  uint32_t in = uint32_t(gamepad[0]) << 8 | gamepad[1];
  if (gamepad[2] > 30) in |= 0x10000;
  if (gamepad[3] > 30) in |= 0x20000;
  // Remapping: each pressed button presses what it is mapped to.
  uint32_t out = 0;
  const auto& remap = RemapTable();
  for (size_t i = 0; i < std::size(kButtons); ++i)
    if (in & kButtons[i].bit) out |= remap[i];
  // Aim toggle: the left trigger as the game sees it, after remapping.
  if (seated) g_aim_raised = g_aim_was_down = false;
  // The aim key's own choice when the keyboard or mouse holds the trigger (the right mouse button), else the
  // controller's.
  const bool key_aim = rex::input::mnk::LastKeyTrigger(false) > 30;
  const bool toggle = key_aim ? REXCVAR_GET(bf_mouse_toggle_aim) : REXCVAR_GET(bf_toggle_aim);
  if (toggle && !seated) {
    const bool down = out & 0x10000;
    if (down && !g_aim_was_down) g_aim_raised = !g_aim_raised;
    g_aim_was_down = down;
    out = g_aim_raised ? out | 0x10000 : out & ~0x10000u;
  }
  gamepad[0] = uint8_t(out >> 8), gamepad[1] = uint8_t(out);
  // An analog trigger keeps its own depth when it presses itself; anything else presses fully.
  const uint8_t lt = gamepad[2], rt = gamepad[3];
  gamepad[2] = (out & 0x10000) ? ((in & 0x10000) && remap[6] == 0x10000 && (!toggle || seated) ? lt : 255) : 0;
  gamepad[3] = (out & 0x20000) ? ((in & 0x20000) && remap[7] == 0x20000 ? rt : 255) : 0;
  // Sticks: left (move), then right (camera). The camera sensitivity scales the turn itself (look.cpp); the mouse
  // has its own vertical setting there.
  Stick(gamepad + 4, gamepad + 6, 1.0, REXCVAR_GET(bf_invert_ls_x), REXCVAR_GET(bf_invert_ls_y));
  CameraStick(gamepad + 8, gamepad + 10, REXCVAR_GET(bf_invert_rs_x), REXCVAR_GET(bf_invert_rs_y));
}

}  // namespace bf1943

// XamInputSetState(user, flags, vibration): X_INPUT_VIBRATION, big endian u16 left and right motor speeds. Off: no
// rumble; the strength scales both motors (capped at the controller's full strength).
extern "C" REX_FUNC(__imp__XamInputSetState) {
  static PPCFunc* original = bf1943::RuntimeExportForRumble();
  const uint32_t vibration = ctx.r5.u32;
  if (vibration) {
    uint8_t* v = base + vibration;
    const double k = REXCVAR_GET(bf_vibration) ? std::clamp(REXCVAR_GET(bf_vibration_strength), 10.0, 200.0) / 100.0 : 0.0;
    for (int i = 0; i < 2; ++i) {
      const double speed = double(uint16_t(v[i * 2] << 8 | v[i * 2 + 1])) * k;
      const uint16_t s = uint16_t(std::clamp(std::lround(speed), 0L, 65535L));
      v[i * 2] = uint8_t(s >> 8), v[i * 2 + 1] = uint8_t(s);
    }
  }
  if (original) original(ctx, base);
}
