// Native mouse and keyboard input (0.4.0): the game's input concepts written directly, not through a fake pad.
//
// Once a game tick the local player's input update sub_82264278 maps the pad to its EntryInputState (player+324) with
// the mapper's virtual sub_822BDB90(mapper, state), called at 0x822644B0; then the game clears disabled concepts,
// applies FlipY to concept 5, runs its local prediction, keeps the state and sends it to the server (sub_82378138).
// Research and the state's layout: docs/input-concepts.md. Writing the state right after the mapping is what the
// server gets: a turret's step (vehicle_mouse.cpp) works on a copy, so writes there were lost (run vmouse2).
//
// The state: 8 analog floats at +8 (concept c at +8+4c; two-sided 0, 1, 4, 5, 6, one-sided 2, 3, 7), the digital
// concepts in the big-endian u64 at +120 (bit i in byte +120+7-i/8), the seat's entry at +156.
//
// Now: the turret aim (vehicle_mouse.cpp's wanted input for concepts 5 and 6) is written here, before FlipY like the
// pad's value, so it reaches the server exactly as worked out (no dead zone, no pad rounding); the pad push stays as
// it was underneath. bf_debug_native_input logs the state after each mapping where it changed (the concept map per
// seat, docs/input-concepts.md).

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "seat_info.h"

REXCVAR_DEFINE_BOOL(bf_native_input, true, "BF1943/Controls",
                    "Mouse aim in vehicles is written into the game's own input for the tick (what the server gets), "
                    "not given as a controller's stick (false: the stick, as in 0.3)");
REXCVAR_DEFINE_BOOL(bf_debug_native_input, false, "BF1943/Controls",
                    "Log the local player's input concepts after each mapping where they changed (bf1943.log)");

namespace bf1943 {
// plane_mouse.cpp: the mouse's stick for concepts 6 (a plane's roll, a gun's turn) and 5 (pitch), as the game has them
// without its own invert; axes bit 0 for 6, bit 1 for 5.
bool PlaneMouseConcepts(float& roll6, float& pitch5, unsigned& axes);
// vehicle_mouse.cpp: the axes the seat's gun has (the same bits), 0 in a seat without one.
unsigned VehicleMouseAxes();
double TickSeconds();  // tick_rate.cpp
}  // namespace bf1943

REXCVAR_DECLARE(double, bf_mouse_vehicle_sensitivity);  // look.cpp
REXCVAR_DECLARE(bool, bf_mouse_invert_y_vehicle);       // input_settings.cpp

namespace {
// The mouse's counts for a seat's own look (look.cpp NoteSeatLook), taken once a tick by the mapping hook.
std::mutex g_seat_mutex;
double g_seat_dx = 0, g_seat_dy = 0;
std::chrono::steady_clock::time_point g_seat_at{};
bool TakeSeatLook(double& dx, double& dy) {
  std::lock_guard lock(g_seat_mutex);
  const bool fresh = std::chrono::steady_clock::now() - g_seat_at < std::chrono::milliseconds(200);
  dx = g_seat_dx, dy = g_seat_dy;
  g_seat_dx = g_seat_dy = 0;
  return fresh;
}
}  // namespace

namespace {
// The local seat as of the last mapping (seat_info.h). The mapper sets bit c of state+120 for each analog concept the
// seat binds, and bits 0..7 stay those flags (docs/input-concepts.md).
std::mutex g_info_mutex;
bf1943::SeatInfo g_info;
std::chrono::steady_clock::time_point g_info_at{};
}  // namespace

namespace bf1943 {
bool LocalSeat(SeatInfo& out) {
  std::lock_guard lock(g_info_mutex);
  if (std::chrono::steady_clock::now() - g_info_at > std::chrono::milliseconds(200)) return false;
  out = g_info;
  return true;
}
const char* SeatKindName(SeatKind kind) {
  switch (kind) {
    case SeatKind::kSoldier: return "on foot";
    case SeatKind::kPlane: return "plane pilot";
    case SeatKind::kDriver: return "driver";
    case SeatKind::kGunner: return "gunner or passenger";
    default: return "unknown";
  }
}

// For look.cpp's pad hook, in a vehicle seat with no plane, turret or free look: the mouse's counts (display density
// taken out) for the seat's own look concepts.
void NoteSeatLook(double dx, double dy) {
  std::lock_guard lock(g_seat_mutex);
  if (std::isfinite(dx)) g_seat_dx += dx;
  if (std::isfinite(dy)) g_seat_dy += dy;
  g_seat_at = std::chrono::steady_clock::now();
}
}  // namespace bf1943

namespace {
constexpr uint32_t kMapCaller = 0x822644B0;  // sub_82264278's call of the mapper

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
uint64_t Load64(const uint8_t* base, uint32_t a) { return uint64_t(Load32(base, a)) << 32 | Load32(base, a + 4); }
bool Guest(uint32_t a) { return a >= 0x10000 && a < 0xF0000000; }

// The last logged state, to log only changes.
struct Logged {
  float analog[8] = {};
  uint64_t digital = 0;
  uint32_t entry = ~0u;
  std::chrono::steady_clock::time_point at{};
} g_logged;

// The seat's bindings (bf_debug_native_input), logged when its mapping or the seat changes: for each concept, the
// inputs that feed it. mapper+12 is the mapping; its +4 vector holds a 16-byte vector per concept of 8-byte entries
// {name, id}: the name a C string, the id the input manager's own number (13 Nov: not the pad's input numbers).
const char* PadName(uint32_t id) {
  switch (id) {
    case 2: return "Start";
    case 3: return "Back";
    case 4: return "Y";
    case 5: return "A";
    case 6: return "X";
    case 7: return "B";
    case 8: case 9: case 10: case 11: return "Dpad";
    case 12: return "LT";
    case 13: return "LB";
    case 14: return "RT";
    case 15: return "RB";
    case 16: return "LS";
    case 17: return "RS";
    case 18: case 28: case 29: return "LX";
    case 19: case 30: case 31: return "LY";
    case 22: case 36: case 37: return "RX";
    case 23: case 38: case 39: return "RY";
    default: return "?";
  }
}
// A short printable C string at a guest address, or nothing.
std::string GuestName(const uint8_t* base, uint32_t a) {
  std::string s;
  if (!Guest(a)) return s;
  for (uint32_t i = 0; i < 28; ++i) {
    const char ch = char(base[a + i]);
    if (!ch) return s;
    if (ch < 32 || ch > 126) return {};
    s += ch;
  }
  return s;
}
uint32_t g_logged_mapping = 0;
void LogBindings(const uint8_t* base, uint32_t mapper) {
  if (!Guest(mapper)) return;
  const uint32_t mapping = Load32(base, mapper + 12);
  if (!Guest(mapping) || mapping == g_logged_mapping) return;
  g_logged_mapping = mapping;
  const uint32_t begin = Load32(base, mapping + 4), end = Load32(base, mapping + 8);
  if (!Guest(begin) || end < begin || end - begin > 64 * 16) return;
  std::string text;
  for (uint32_t c = 0; c < (end - begin) / 16; ++c) {
    const uint32_t eb = Load32(base, begin + 16 * c), ee = Load32(base, begin + 16 * c + 4);
    if (!Guest(eb) || ee <= eb || ee - eb > 64 * 8) continue;
    std::string pads;
    for (uint32_t e = eb; e < ee; e += 8) {
      const uint32_t id = Load32(base, e + 4);
      const std::string name = GuestName(base, Load32(base, e));
      pads += fmt::format("{}{}:{}{}{}", pads.empty() ? "" : "/", PadName(id), id, name.empty() ? "" : ":", name);
    }
    if (!pads.empty()) text += fmt::format(" c{}={}", c, pads);
  }
  REXLOG_INFO("BF native input: the seat's bindings (mapping {:08X}):{}", mapping, text.empty() ? " none" : text);
}

void LogState(const uint8_t* base, uint32_t state, const char* note) {
  Logged now;
  for (int c = 0; c < 8; ++c) now.analog[c] = LoadF(base, state + 8 + 4 * c);
  now.digital = Load64(base, state + 120) & ((uint64_t(1) << 40) - 1);
  now.entry = Load32(base, state + 156);
  bool changed = now.entry != g_logged.entry || now.digital != g_logged.digital;
  for (int c = 0; c < 8 && !changed; ++c) changed = std::fabs(now.analog[c] - g_logged.analog[c]) > 0.02f;
  const auto t = std::chrono::steady_clock::now();
  if (!changed || t - g_logged.at < std::chrono::milliseconds(100)) return;
  now.at = t;
  std::string analog, digital;
  for (int c = 0; c < 8; ++c)
    if (std::fabs(now.analog[c]) > 0.001f) analog += fmt::format(" c{}={:.2f}", c, now.analog[c]);
  for (int c = 8; c < 40; ++c)
    if (now.digital >> c & 1) digital += fmt::format(" {}", c);
  REXLOG_INFO("BF native input: entry {:08X} analog{}; down{}{}", now.entry, analog.empty() ? " none" : analog,
              digital.empty() ? " none" : digital, note);
  g_logged = now;
}
}  // namespace

// The mapper, sub_822BDB90(mapper, state): the pad to concepts (docs/input-concepts.md).
REX_EXTERN(__imp__sub_822BDB90);
REX_HOOK_RAW(sub_822BDB90) {
  const uint32_t state = ctx.r4.u32, lr = uint32_t(ctx.lr), mapper_at_call = ctx.r3.u32;
  // The caller's own registers at its call (sub_82264278, bf1943_recomp.21.cpp): r31 the player, r29 the controllable,
  // r23 the InputType, r27 FlipY.
  const uint32_t player = ctx.r31.u32, ctrl = ctx.r29.u32, input_type = ctx.r23.u32, flip_y = ctx.r27.u32;
  __imp__sub_822BDB90(ctx, base);
  if (lr != kMapCaller || !Guest(state)) return;
  static bool checked = false;
  if (!checked) {
    checked = true;
    REXLOG_INFO("BF native input: the local player's mapping found (mapper vtable slot +12 = {:08X})",
                Load32(base, 0x8200E4A0 + 12));
  }
  const uint32_t mapper = mapper_at_call;
  // The seat (seat_info.h): the scheme number at controllable+256 (1 on foot, 2 land and sea, 3 planes), the vehicle and
  // the seat's index at player+304 and +308.
  {
    bf1943::SeatInfo s;
    s.controllable = ctrl;
    if (Guest(ctrl)) s.scheme = Load32(base, ctrl + 256), s.cls = Load32(base, ctrl);
    if (Guest(player)) s.vehicle = Load32(base, player + 304), s.seat = Load32(base, player + 308);
    s.bound = uint32_t(Load64(base, state + 120) & 0xFF);
    s.throttle = (s.bound & 1) != 0, s.fire = (s.bound & 0x80) != 0;
    // 13 Nov calibration run: planes are scheme 3, drivers scheme 2 (the boat's too), and a gunner's or passenger's
    // seat is scheme 1 like a soldier on foot, but with a vehicle (an AA gun is a vehicle of one seat). Bits 4 to 7 of
    // the bound concepts are always set, so they say nothing of a seat's weapon.
    s.kind = s.scheme == 3                   ? bf1943::SeatKind::kPlane
             : !s.vehicle                    ? bf1943::SeatKind::kSoldier
             : s.scheme == 2 || s.throttle   ? bf1943::SeatKind::kDriver
                                             : bf1943::SeatKind::kGunner;
    std::lock_guard lock(g_info_mutex);
    const bool changed = s.controllable != g_info.controllable || s.vehicle != g_info.vehicle ||
                         s.seat != g_info.seat || s.scheme != g_info.scheme || s.cls != g_info.cls;
    s.key = g_info.key + (changed || !g_info.key ? 1 : 0);
    if (s.key != g_info.key || s.bound != g_info.bound) {
      REXLOG_INFO("BF seat: #{} {}: scheme {}, class {:08X}, controllable {:08X}, vehicle {:08X}, seat {}, binds "
                  "analog concepts {:02X} (throttle {}, fire {}), input type {}, flip y {}",
                  s.key, bf1943::SeatKindName(s.kind), s.scheme, s.cls, s.controllable, s.vehicle, s.seat, s.bound,
                  s.throttle ? "yes" : "no", s.fire ? "yes" : "no", input_type, flip_y);
      g_logged_mapping = 0;
    }
    g_info = s;
    g_info_at = std::chrono::steady_clock::now();
  }
  if (REXCVAR_GET(bf_debug_native_input)) LogBindings(base, mapper);
  const char* note = "";
  if (REXCVAR_GET(bf_native_input)) {
    // The game inverts concept 5 after this when its own Vertical look or Vertical fly says so (FlipY, the caller's
    // r27: 13 Nov, "pitch controls are inverted when set to normal"). Those settings are the controller's: what the
    // mouse writes is inverted here first, so it arrives as written, and the mouse has its own invert settings.
    const float flip = (flip_y & 0xFF) ? -1.0f : 1.0f;
    auto clamp1 = [](float v) { return std::isfinite(v) ? std::fmax(-1.0f, std::fmin(1.0f, v)) : 0.0f; };
    // The mouse's stick (plane_mouse.cpp): a plane's roll and pitch, or a gun's turn and pitch.
    float roll6 = 0, pitch5 = 0;
    unsigned stick_axes = 0;
    if (bf1943::PlaneMouseConcepts(roll6, pitch5, stick_axes)) {
      if (stick_axes & 1) StoreF(base, state + 8 + 4 * 6, clamp1(roll6));
      if (stick_axes & 2) StoreF(base, state + 8 + 4 * 5, clamp1(pitch5) * flip);
      note = " (mouse stick written)";
    }
    // A seat's own look (no plane, no gun's part, no free look: a seat whose view the game turns): the mouse's rate
    // into the look concepts 4 (x) and 5 (y), as Beachhead's Classic does (counts x 0.001 x scale / step; the scale is
    // the land and sea sensitivity x 5 + 0.1, 1.35 at its 0.25), the stronger of it and the keys' kept. Taken each
    // tick, so a still mouse reads 0. Not in a gun's seat: there the stick alone writes.
    {
      double dx = 0, dy = 0;
      if (TakeSeatLook(dx, dy) && !stick_axes && !bf1943::VehicleMouseAxes()) {
        float dt = LoadF(base, state + 128);
        if (!(dt > 0.001f && dt < 0.5f)) dt = float(bf1943::TickSeconds());
        const double sens = REXCVAR_GET(bf_mouse_vehicle_sensitivity);
        const double scale = 1.35 * (std::isfinite(sens) ? std::clamp(sens, 0.05, 10.0) : 1.0);
        const double iy = REXCVAR_GET(bf_mouse_invert_y_vehicle) ? -1.0 : 1.0;
        auto stronger = [&](int c, double v) {
          v = std::clamp(v, -1.0, 1.0);
          const float now = LoadF(base, state + 8 + 4 * c);
          if (std::fabs(v) > std::fabs(now)) StoreF(base, state + 8 + 4 * c, float(v));
        };
        stronger(4, dx * 0.001 * scale / dt);
        stronger(5, -dy * 0.001 * scale * iy / dt * flip);
        if (dx != 0 || dy != 0) note = " (seat look written)";
      }
    }
  }
  if (REXCVAR_GET(bf_debug_native_input)) LogState(base, state, note);
}
