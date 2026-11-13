// Native mouse and keyboard input (0.4.0, pull request 11 of the players' repository by ThatZaliaSGuy): the game's
// input concepts written directly, not through a fake pad.
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

REXCVAR_DEFINE_BOOL(bf_native_input, true, "BF1943/Controls",
                    "Mouse aim in vehicles is written into the game's own input for the tick (what the server gets), "
                    "not given as a controller's stick (false: the stick, as in 0.3)");
REXCVAR_DEFINE_BOOL(bf_debug_native_input, false, "BF1943/Controls",
                    "Log the local player's input concepts after each mapping where they changed (bf1943.log)");

namespace bf1943 {
// vehicle_mouse.cpp: the input the mouse wants for concepts 6 (x) and 5 (y) this tick, as the game reads its stick
// (-1 to 1, after the dead zone); axes bit 0 for 6, bit 1 for 5. False when no turning part has the mouse.
bool VehicleMouseConcepts(unsigned& axes, float& yaw6, float& pitch5);
// plane_mouse.cpp: a plane's steering for concepts 6 (roll), 5 (pitch, before FlipY) and 4 (rudder).
bool PlaneMouseConcepts(float& roll6, float& pitch5, float& yaw4);
bool VehicleMouseInverted();      // vehicle_mouse.cpp: Mouse vertical in vehicles (same as on foot, normal, inverted)
bool GameVerticalLookInverted();  // game_menu.cpp: the game's own Vertical look (it negates concept 5 in vehicles)
}  // namespace bf1943

REXCVAR_DECLARE(double, bf_mouse_vehicle_sensitivity);  // look.cpp

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

namespace bf1943 {
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

// The seat's bindings (bf_debug_native_input), logged when its mapping changes: for each concept, the pad inputs that
// feed it. mapper+12 is the mapping; its +4 vector holds a 16-byte vector per concept of 8-byte entries {button data,
// source}, the button data's +12 the pad input (sub_822BAF90, sub_821CB358; docs/input-concepts.md).
const char* PadName(uint32_t id) {
  static const char* const kNames[] = {"LX+", "LX-", "LY+", "LY-", "RX+", "RX-", "RY+", "RY-", "Start", "Back",
                                       "LS",  "RS",  "A",   "B",   "X",   "Y",   "16",  "17",  "LB",    "RB",
                                       "LT",  "RT",  "Up",  "Down", "Left", "Right"};
  return id < std::size(kNames) ? kNames[id] : "?";
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
      const uint32_t data = Load32(base, e);
      if (!Guest(data)) continue;
      // Not a pad input number (12 Nov 23:10: most read "?"): the raw words, to find the field.
      const uint32_t id = Load32(base, data + 12);
      pads += id < 26 ? fmt::format("{}{}", pads.empty() ? "" : "/", PadName(id))
                      : fmt::format("{}[{:X} {:X} {:X} {:X}]", pads.empty() ? "" : "/", Load32(base, data),
                                    Load32(base, data + 4), Load32(base, data + 8), id);
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
  __imp__sub_822BDB90(ctx, base);
  if (lr != kMapCaller || !Guest(state)) return;
  static bool checked = false;
  if (!checked) {
    checked = true;
    REXLOG_INFO("BF native input: the local player's mapping found (mapper vtable slot +12 = {:08X})",
                Load32(base, 0x8200E4A0 + 12));
  }
  const uint32_t mapper = mapper_at_call;
  if (REXCVAR_GET(bf_debug_native_input)) LogBindings(base, mapper);
  const char* note = "";
  if (REXCVAR_GET(bf_native_input)) {
    float roll6 = 0, pitch5p = 0, yaw4 = 0;
    if (bf1943::PlaneMouseConcepts(roll6, pitch5p, yaw4)) {
      auto clamp1 = [](float v) { return std::isfinite(v) ? std::fmax(-1.0f, std::fmin(1.0f, v)) : 0.0f; };
      (void)yaw4;  // the rudder stays the keys' (A, D)
      StoreF(base, state + 8 + 4 * 6, clamp1(roll6));
      StoreF(base, state + 8 + 4 * 5, clamp1(pitch5p));
      note = " (plane steering written)";
    }
    unsigned axes = 0;
    float yaw6 = 0, pitch5 = 0;
    const bool turret = bf1943::VehicleMouseConcepts(axes, yaw6, pitch5);
    // A seat's own look (no plane, no turret part, no free look: a boat's or jeep's gun that follows the soldier's
    // look, a driver's view the game turns): the mouse's rate into the look concepts 4 (x) and 5 (y), as Beachhead's
    // Classic does (counts x 0.001 x scale / step; the scale is the land and sea sensitivity x 5 + 0.1, 1.35 at its
    // 0.25), the stronger of it and the keys' kept. Taken each tick, so a still mouse reads 0.
    {
      double dx = 0, dy = 0;
      if (TakeSeatLook(dx, dy) && !turret) {
        float dt = LoadF(base, state + 128);
        if (!(dt > 0.001f && dt < 0.5f)) dt = 1.0f / 30.0f;
        const double sens = REXCVAR_GET(bf_mouse_vehicle_sensitivity);
        const double scale = 1.35 * (std::isfinite(sens) ? std::clamp(sens, 0.05, 10.0) : 1.0);
        const double iy = bf1943::VehicleMouseInverted() ? -1.0 : 1.0;  // Mouse vertical in vehicles
        auto stronger = [&](int c, double v) {
          v = std::clamp(v, -1.0, 1.0);
          const float now = LoadF(base, state + 8 + 4 * c);
          if (std::fabs(v) > std::fabs(now)) StoreF(base, state + 8 + 4 * c, float(v));
        };
        stronger(4, dx * 0.001 * scale / dt);
        stronger(5, -dy * 0.001 * scale * iy / dt);
        if (dx != 0 || dy != 0) note = " (seat look written)";
      }
    }
    if (turret) {
      if (axes & 1) StoreF(base, state + 8 + 4 * 6, std::isfinite(yaw6) ? std::fmax(-1.0f, std::fmin(1.0f, yaw6)) : 0.0f);
      if (axes & 2) {
        // The game's own Vertical look (Inverted) negates concept 5 after this mapping (FlipY) and before the gun's
        // part reads it, which turned the aim loop round (the gun ran away from the mouse, v0.4.0's fix of 12 Nov
        // 2026 for the pad's stick): written negated, it comes out as the loop wants it. Which way the mouse aims is
        // the vehicles' own setting (vehicle_mouse.cpp VehicleMouseInverted).
        const float v = std::isfinite(pitch5) ? std::fmax(-1.0f, std::fmin(1.0f, pitch5)) : 0.0f;
        StoreF(base, state + 8 + 4 * 5, bf1943::GameVerticalLookInverted() ? -v : v);
      }
      if (axes) note = " (turret aim written)";
    }
  }
  if (REXCVAR_GET(bf_debug_native_input)) LogState(base, state, note);
}
