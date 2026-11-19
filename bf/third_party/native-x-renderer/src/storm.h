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

struct Storm {
  bool on = false;      // a hurricane (the match's weather a quarter or more)
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

// The hurricane of a match's weather (0 to 1) on its world clock, over a level whose middle is (mx, mz) and whose reach
// (metres from the middle) is `reach`.
inline Storm Of(float weather, double seconds, float mx, float mz, float reach) {
  Storm s;
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
  s.eye = 0.09f * s.radius;
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
  const float wall = 1.8f * s.eye;
  if (r < wall) return s.gain * (0.15f + 0.85f * Smooth(s.eye * 0.8f, wall, r));
  const float shield = 1.0f - Smooth(0.6f * s.radius, s.radius, r);
  const float angle = std::atan2(dz, dx);
  const float band = 0.5f + 0.5f * std::cos(2.0f * angle - 3.0f * std::log(r / wall) - s.spin);
  const float near = 1.0f - Smooth(wall, wall * 2.5f, r);  // close to the wall the bands merge into it
  return s.gain * shield * std::max(near, 0.75f + 0.25f * Smooth(0.35f, 0.8f, band));
}

// The weather at (x, z): the storm's there, or the weather outside it.
inline float WeatherAt(const Storm& s, float x, float z) { return std::max(s.outside, At(s, x, z)); }

// The same in GLSL (the clouds), with the storm in two vectors: a = (cx, cz, eye, radius), b = (gain, spin, on, -).
constexpr const char* kGlsl = R"(
float storm_at(vec2 p, vec4 a, vec4 b) {
  if (b.z < 0.5) return 0.0;
  vec2 d = p - a.xy;
  float r = length(d);
  if (r >= a.w) return 0.0;
  float wall = 1.8 * a.z;
  if (r < wall) return b.x * (0.15 + 0.85 * smoothstep(a.z * 0.8, wall, r));
  float shield = 1.0 - smoothstep(0.6 * a.w, a.w, r);
  float band = 0.5 + 0.5 * cos(2.0 * atan(d.y, d.x) - 3.0 * log(r / wall) - b.y);
  float near = 1.0 - smoothstep(wall, wall * 2.5, r);
  return b.x * shield * max(near, 0.75 + 0.25 * smoothstep(0.35, 0.8, band));
}
)";

}  // namespace storm
}  // namespace native_x_renderer
