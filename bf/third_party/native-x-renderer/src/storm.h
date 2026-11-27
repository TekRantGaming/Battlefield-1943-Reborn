// D208: the weather as a tropical hurricane over the level (19 Nov 2026, the user: "the weather in general is should be
// based on a tropical hurricane, so the higher percentage, the more the storm is building up, and at 100% weather,
// the hurricane completely engulfs the map. The clouds and sky should replicate the tropical hurricane, in a
// localized manner. Weather should also feel localized too, so areas of the map that aren't directly covered by the
// storm, just experience the wind. Thunder and lighting only happen directly under the storm itself too.").
//
// The match's weather (WorldState::storm) is the hurricane's stage. Its size is the level's own (D213, the user:
// "3km and 10km might be too large for the scale of this game ... It should never be bigger than the map itself"),
// from the level's reach (its capture points' farthest from their middle, and 300 m, SetMapArea): from a quarter of
// the weather the storm is out at sea, 4 reaches from the middle, its rain shield 0.45 of a reach across; it comes in
// as the weather rises, and at 100% its centre is 0.3 of a reach off the middle and its rain shield 1.5 reaches (about
// the level's own size): the level covered, the calm eye (0.09 of the shield) passing over part of it. Full strength
// out to 0.6 of the shield, fading to its edge; between the bands at least 0.75 (run scaleb100: 0.55 left the US base at 55%; run scale100: the shield's falloff
// and 0.4 between the bands left the US base at 15% under a 100% storm). (D208's: 14 km out, a shield of 3.5 km to
// 9 km, its centre 1.8 km off.) Its bearing turns slowly with the world's clock (a full turn in
// about 87 hours: over a match, a few degrees), so every player sees it in the same place.
//
// The storm's strength at a place (0 to 1): calm in the eye (0.15), rising to full at the eyewall (1.8 eyes out),
// falling to nothing at the rain shield's edge, and over that, two rain bands spiralling in (a logarithmic spiral,
// turning anticlockwise as the northern hemisphere's do, slowly with the clock): in a band the strength stays, between
// two it drops to under half. Under a quarter of the weather there is no hurricane: the weather is the same everywhere,
// as before.
//
// The weather where the camera is (WorldState::weather, renderer.cpp's World) is the larger of the storm's strength
// there and the weather outside it (the match's weather, at most 0.15: fair to broken cloud). The rain, the mist, the
// light, the wetness and the rain's sound follow it; the wind is the match's own, everywhere.
#pragma once

#include <algorithm>
#include <cmath>

namespace native_x_renderer {
namespace storm {

// D224: the storm's type (the user, 20 Nov 2026: "make the Hurricane and storm-type that can be selected, but by
// default we just run the regular storm in the same localized fashion. I want to add more storm types later too"). A
// match chooses one (gm_host.cpp bf_world_storm_type, sent with its world); each type is its own shape in At() and in
// the GLSL's storm_at, placed and sized by the same weather. A new type: a number here, its name, its case in both.
//   kStorm      a thunderstorm: one storm cell, strongest over its middle, with heavier cells in it that drift round;
//               no eye and no spiral bands. The default.
//   kHurricane  D208's tropical hurricane: a calm eye, the eyewall, two spiral rain bands.
enum StormType : int { kStorm = 0, kHurricane = 1, kStormTypes = 2 };
inline const char* StormTypeName(int type) { return type == kHurricane ? "hurricane" : "storm"; }

struct Storm {
  int type = kStorm;    // D224
  bool on = false;      // a storm (the match's weather a quarter or more)
  float cx = 0, cz = 0;  // its centre (world metres)
  float eye = 600.0f;    // the eye's radius
  float radius = 9000.0f;  // the rain shield's
  float gain = 0.0f;     // how strong it is (its stage)
  float spin = 0.0f;     // the bands' turn now (radians)
  float outside = 0.0f;  // the weather outside it
};

inline float Smooth(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// The storm of a match's weather (0 to 1) on its world clock, over a level whose middle is (mx, mz) and whose reach
// (metres from the middle) is `reach`, of the match's type (D224; every type placed and sized the same).
inline Storm Of(float weather, double seconds, float mx, float mz, float reach, int type = kStorm) {
  Storm s;
  s.type = type >= 0 && type < kStormTypes ? type : kStorm;
  const float w = std::clamp(weather, 0.0f, 1.0f);
  s.outside = std::min(w, 0.15f);  // D211: 0.3 before (a flat grey sheet over the sky outside the storm)
  if (w < 0.25f) {
    s.outside = w;
    return s;
  }
  s.on = true;
  const float stage = Smooth(0.25f, 1.0f, w);
  s.gain = Smooth(0.25f, 0.6f, w);
  const float r = std::clamp(reach, 500.0f, 4000.0f);
  const float distance = r * (4.0f + (0.3f - 4.0f) * stage);
  s.radius = r * (0.45f + (1.5f - 0.45f) * stage);
  // D226: a hurricane's eye 0.18 of its rain shield (0.09 before: about 80 m at the game's scale, smaller than one
  // cloud, it drew as a small puckered dimple). A storm has no eye: its towers stand at its middle.
  s.eye = s.type == kHurricane ? 0.18f * s.radius : 0.0f;
  const double bearing = 0.7 + std::fmod(seconds, 312000.0) / 312000.0 * 6.283185307;
  s.cx = mx + float(std::sin(bearing)) * distance;
  s.cz = mz + float(std::cos(bearing)) * distance;
  s.spin = float(std::fmod(seconds * 0.004, 6.283185307));
  return s;
}

// D213: how fast the clouds form and change under the match's weather (the user, 19 Nov 2026: "The cloud change rate
// should also update with the % of the storm to match realistic movement"): the cloud change rate set times this, 1
// in fair weather to 3 at 100%. The host's clock runs it so too (gm_host.cpp, the same formula), so the players' games
// keep with it.
inline float RateFactor(float weather) { return 1.0f + 2.0f * Smooth(0.25f, 1.0f, weather); }

// The storm's strength at (x, z), 0 to 1 (before the weather outside it).
inline float At(const Storm& s, float x, float z) {
  if (!s.on) return 0.0f;
  const float dx = x - s.cx, dz = z - s.cz;
  const float r = std::sqrt(dx * dx + dz * dz);
  if (r >= s.radius) return 0.0f;
  if (s.type == kStorm) {
    // D224, a thunderstorm: full strength out to 0.55 of its rain shield, fading to its edge, and heavier cells in it
    // (three lobes that drift round with the clock): never under 0.8 of full inside the core.
    const float core = 1.0f - Smooth(0.55f * s.radius, s.radius, r);
    const float cells = 0.5f + 0.5f * std::cos(3.0f * std::atan2(dz, dx) + 5.0f * r / s.radius - s.spin);
    return s.gain * core * (0.8f + 0.2f * cells);
  }
  // D226 (the user, 20 Nov 2026: "The Hurricane shape also looks like a nipple ... Can we make that more realistic?"):
  // as a hurricane is seen from above, a calm eye with a steep wall (calm inside 0.85 of the eye, full by 1.25 of
  // it; D208's rose over 1.8 eyes, a cone), the central dense overcast full out to 0.4 of the rain shield, then two
  // open spiral bands (pitched about 20 degrees, anticlockwise). Where it rains: between the bands still at least 0.75
  // (D213), so at 100% the level is covered; the clouds' own look is storm_look's (GLSL), with clearer gaps.
  const float calm = 0.85f * s.eye, wall = 1.25f * s.eye;
  if (r < wall) return s.gain * Smooth(calm, wall, r);  // (D234: calm and clear inside the eye, no rain: was 0.12)
  const float shield = 1.0f - Smooth(0.6f * s.radius, s.radius, r);
  const float core = 1.0f - Smooth(0.4f * s.radius, 0.55f * s.radius, r);
  const float band = 0.5f + 0.5f * std::cos(2.0f * std::atan2(dz, dx) - 5.5f * std::log(r / wall) - s.spin);
  return s.gain * shield * std::max(core, 0.75f + 0.25f * Smooth(0.35f, 0.8f, band));
}

// The weather at (x, z): the storm's there, or the weather outside it.
inline float WeatherAt(const Storm& s, float x, float z) { return std::max(s.outside, At(s, x, z)); }

// The same in GLSL (the clouds), with the storm in two vectors: a = (cx, cz, eye, radius), b = (gain, spin, its type
// plus 1 while there is one (0: none; D224), the volutus).
constexpr const char* kGlsl = R"(
float storm_at(vec2 p, vec4 a, vec4 b) {
  if (b.z < 0.5) return 0.0;
  vec2 d = p - a.xy;
  float r = length(d);
  if (r >= a.w) return 0.0;
  if (b.z < 1.5) {  // D224: a thunderstorm (storm.h kStorm)
    float core = 1.0 - smoothstep(0.55 * a.w, a.w, r);
    float cells = 0.5 + 0.5 * cos(3.0 * atan(d.y, d.x) + 5.0 * r / a.w - b.y);
    return b.x * core * (0.8 + 0.2 * cells);
  }
  float wall = 1.25 * a.z;  // D226: as At()
  if (r < wall) return b.x * smoothstep(0.85 * a.z, wall, r);
  float shield = 1.0 - smoothstep(0.6 * a.w, a.w, r);
  float core = 1.0 - smoothstep(0.4 * a.w, 0.55 * a.w, r);
  float band = 0.5 + 0.5 * cos(2.0 * atan(d.y, d.x) - 5.5 * log(r / wall) - b.y);
  return b.x * shield * max(core, 0.75 + 0.25 * smoothstep(0.35, 0.8, band));
}
// D226: how much of the storm's cloud there is at p, for the clouds (0 to 1). A hurricane's own: no cloud in the eye
// but for its rim, the eyewall rising steeply (0.85 to 1.05 of the eye), the central dense overcast whole out to 0.38
// of the rain shield, then the two spiral bands with clearer gaps between them (a quarter of the cloud). Any other
// type: its strength.
float storm_look(vec2 p, vec4 a, vec4 b) {
  if (b.z < 1.5 || b.z > 2.5) return storm_at(p, a, b);
  vec2 d = p - a.xy;
  float r = length(d);
  if (r >= a.w) return 0.0;
  float wall = 1.25 * a.z;
  float rise = smoothstep(0.85 * a.z, 1.05 * a.z, r);
  float shield = 1.0 - smoothstep(0.55 * a.w, a.w, r);
  float core = 1.0 - smoothstep(0.38 * a.w, 0.52 * a.w, r);
  float band = 0.5 + 0.5 * cos(2.0 * atan(d.y, d.x + 1e-4) - 5.5 * log(max(r, wall) / wall) - b.y);
  float bands = mix(0.25, 1.0, smoothstep(0.45, 0.85, band));
  return b.x * rise * shield * max(core, bands);
}
// D226: 1 in a hurricane's eye, where the sky is clear (the cloud's cover taken away there); 0 elsewhere.
float storm_eye(vec2 p, vec4 a, vec4 b) {
  if (b.z < 1.5 || b.z > 2.5) return 0.0;
  return b.x * (1.0 - smoothstep(0.7 * a.z, 0.95 * a.z, length(p - a.xy)));
}
)";

}  // namespace storm
}  // namespace native_x_renderer
