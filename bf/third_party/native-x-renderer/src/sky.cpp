// NativeXRenderer - a sky of its own (sky.h).

#include "sky.h"
#include "storm.h"

#include <mutex>

#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

#include "glsl.h"
#include <native_x_renderer/native_x_renderer.h>  // LightningStrike

namespace native_x_renderer {

using namespace Diligent;

namespace {

// ---------------------------------------------------------------------------------------------------------------
// The noise (Beachhead's crates/sim/src/cloudnoise.rs, in C++): tiling volumes from fixed seeds.
//   shape, 128^3 RGBA8: R a Perlin fractal remapped by Worley cells ("Perlin-Worley"), G, B, A Worley fractals of
//     rising frequency; detail, 32^3 RGB8: three Worley fractals; the weather map, 512^2 RG8: two value-noise fractals.

uint64_t SplitMix(uint64_t& state) {
  state += 0x9e3779b97f4a7c15ull;
  uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}
float Uniform(uint64_t& state) { return (float(SplitMix(state) >> 40) + 1.0f) / 16777216.0f; }
uint64_t CellState(uint64_t seed, uint32_t ix, uint32_t iy, uint32_t iz) {
  const uint64_t key = (uint64_t(iz) << 42) ^ (uint64_t(iy) << 21) ^ uint64_t(ix);
  return seed ^ (key * 0x9e3779b97f4a7c15ull);
}
uint64_t Stream(uint64_t seed, uint64_t k) {
  uint64_t s = seed ^ (k * 0xd6e8feb86659fd93ull);
  return SplitMix(s);
}
int Wrap(int v, int n) {
  const int r = v % n;
  return r < 0 ? r + n : r;
}

struct WorleyGrid {
  uint32_t cells = 0;
  std::vector<std::array<float, 3>> points;
  WorleyGrid(uint32_t n, uint64_t seed) : cells(n), points(size_t(n) * n * n) {
    size_t i = 0;
    for (uint32_t z = 0; z < n; ++z)
      for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
          uint64_t s = CellState(seed, x, y, z);
          const float a = Uniform(s), b = Uniform(s), c = Uniform(s);
          points[i++] = {a, b, c};
        }
  }
  float Sample(const float p[3]) const {
    const int c = int(cells);
    const float q[3] = {p[0] * cells, p[1] * cells, p[2] * cells};
    const int base[3] = {int(std::floor(q[0])), int(std::floor(q[1])), int(std::floor(q[2]))};
    float best = 1e30f;
    for (int dz = -1; dz <= 1; ++dz) {
      const int cz = base[2] + dz, wz = Wrap(cz, c);
      for (int dy = -1; dy <= 1; ++dy) {
        const int cy = base[1] + dy, wy = Wrap(cy, c);
        for (int dx = -1; dx <= 1; ++dx) {
          const int cx = base[0] + dx;
          const auto& f = points[(size_t(wz) * cells + wy) * cells + Wrap(cx, c)];
          const float ex = cx + f[0] - q[0], ey = cy + f[1] - q[1], ez = cz + f[2] - q[2];
          best = std::min(best, ex * ex + ey * ey + ez * ez);
        }
      }
    }
    return 1.0f - std::min(std::sqrt(best), 1.0f);
  }
};
float WorleyFbm(const WorleyGrid& a, const WorleyGrid& b, const WorleyGrid& c, const float p[3]) {
  return 0.625f * a.Sample(p) + 0.25f * b.Sample(p) + 0.125f * c.Sample(p);
}

const float kGradients[12][3] = {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1},  {-1, 0, 1},
                                 {1, 0, -1}, {-1, 0, -1}, {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}};
float Fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
float Lerp(float a, float b, float t) { return a + (b - a) * t; }
float Perlin(const float p[3], uint32_t cells, uint64_t seed) {
  const int c = int(cells);
  const float q[3] = {p[0] * cells, p[1] * cells, p[2] * cells};
  const float base[3] = {std::floor(q[0]), std::floor(q[1]), std::floor(q[2])};
  const float t[3] = {q[0] - base[0], q[1] - base[1], q[2] - base[2]};
  const int i[3] = {int(base[0]), int(base[1]), int(base[2])};
  const float u[3] = {Fade(t[0]), Fade(t[1]), Fade(t[2])};
  auto corner = [&](int dx, int dy, int dz) {
    uint64_t s = CellState(seed, uint32_t(Wrap(i[0] + dx, c)), uint32_t(Wrap(i[1] + dy, c)), uint32_t(Wrap(i[2] + dz, c)));
    const float* g = kGradients[SplitMix(s) % 12];
    return g[0] * (t[0] - dx) + g[1] * (t[1] - dy) + g[2] * (t[2] - dz);
  };
  const float x00 = Lerp(corner(0, 0, 0), corner(1, 0, 0), u[0]), x10 = Lerp(corner(0, 1, 0), corner(1, 1, 0), u[0]);
  const float x01 = Lerp(corner(0, 0, 1), corner(1, 0, 1), u[0]), x11 = Lerp(corner(0, 1, 1), corner(1, 1, 1), u[0]);
  return std::clamp(Lerp(Lerp(x00, x10, u[1]), Lerp(x01, x11, u[1]), u[2]), -1.0f, 1.0f);
}
float PerlinFbm(const float p[3], uint32_t cells, uint64_t seed) {
  float sum = 0, amp = 1;
  for (uint32_t o = 0; o < 5; ++o) {
    sum += amp * Perlin(p, cells << o, seed + o);
    amp *= 0.5f;
  }
  return std::clamp(0.5f + 0.5f * sum, 0.0f, 1.0f);
}
uint8_t ToU8(float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }
float Remap01(float v, float lo, float hi) { return std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f); }

// A volume of `size`^3 texels of 4 bytes, each slice filled by `texel(x, y, z, out)`, on every processor.
std::vector<uint8_t> FillVolume(uint32_t size, const std::function<void(uint32_t, uint32_t, uint32_t, uint8_t*)>& texel) {
  std::vector<uint8_t> data(size_t(size) * size * size * 4);
  const uint32_t threads = std::clamp(std::thread::hardware_concurrency(), 1u, std::min(size, 16u));
  std::vector<std::thread> pool;
  std::atomic<uint32_t> next{0};
  for (uint32_t t = 0; t < threads; ++t)
    pool.emplace_back([&] {
      for (uint32_t z; (z = next.fetch_add(1)) < size;)
        for (uint32_t y = 0; y < size; ++y)
          for (uint32_t x = 0; x < size; ++x) texel(x, y, z, &data[((size_t(z) * size + y) * size + x) * 4]);
    });
  for (auto& t : pool) t.join();
  return data;
}
float Centre(uint32_t i, uint32_t size) { return (float(i) + 0.5f) / float(size); }

std::vector<uint8_t> ShapeVolume(uint32_t size, uint64_t seed) {
  const uint64_t ws = Stream(seed, 1), ps = Stream(seed, 2);
  std::vector<WorleyGrid> grids;
  for (uint32_t k = 0; k < 6; ++k) grids.emplace_back(4u << k, ws + k);
  return FillVolume(size, [&](uint32_t x, uint32_t y, uint32_t z, uint8_t* out) {
    const float p[3] = {Centre(x, size), Centre(y, size), Centre(z, size)};
    const float wfbm = WorleyFbm(grids[0], grids[1], grids[2], p);
    out[0] = ToU8(Remap01(PerlinFbm(p, 4, ps), wfbm - 1.0f, 1.0f));
    out[1] = ToU8(WorleyFbm(grids[1], grids[2], grids[3], p));
    out[2] = ToU8(WorleyFbm(grids[2], grids[3], grids[4], p));
    out[3] = ToU8(WorleyFbm(grids[3], grids[4], grids[5], p));
  });
}
std::vector<uint8_t> DetailVolume(uint32_t size, uint64_t seed) {
  const uint64_t ws = Stream(seed, 3);
  std::vector<WorleyGrid> grids;
  for (uint32_t k = 0; k < 5; ++k) grids.emplace_back(4u << k, ws + k);
  return FillVolume(size, [&](uint32_t x, uint32_t y, uint32_t z, uint8_t* out) {
    const float p[3] = {Centre(x, size), Centre(y, size), Centre(z, size)};
    for (uint32_t c = 0; c < 3; ++c) out[c] = ToU8(WorleyFbm(grids[c], grids[c + 1], grids[c + 2], p));
    out[3] = 255;
  });
}
float ValueNoise(float px, float py, uint32_t cells, uint64_t seed) {
  const int c = int(cells);
  const float qx = px * cells, qy = py * cells, fx = std::floor(qx), fy = std::floor(qy);
  const float tx = Fade(qx - fx), ty = Fade(qy - fy);
  auto at = [&](int dx, int dy) {
    uint64_t s = CellState(seed, uint32_t(Wrap(int(fx) + dx, c)), uint32_t(Wrap(int(fy) + dy, c)), 0);
    return Uniform(s);
  };
  return Lerp(Lerp(at(0, 0), at(1, 0), tx), Lerp(at(0, 1), at(1, 1), tx), ty);
}
float ValueFbm(float px, float py, uint32_t cells, uint64_t seed) {
  float sum = 0, amp = 1, norm = 0;
  for (uint32_t o = 0; o < 4; ++o) {
    sum += amp * ValueNoise(px, py, cells << o, seed + o);
    norm += amp;
    amp *= 0.5f;
  }
  return sum / norm;
}
std::vector<uint8_t> WeatherMap(uint32_t size, uint64_t seed) {
  const uint64_t rs = Stream(seed, 4), gs = Stream(seed, 5);
  std::vector<uint8_t> data(size_t(size) * size * 2);
  for (uint32_t y = 0; y < size; ++y)
    for (uint32_t x = 0; x < size; ++x) {
      data[(size_t(y) * size + x) * 2] = ToU8(ValueFbm(Centre(x, size), Centre(y, size), 4, rs));
      data[(size_t(y) * size + x) * 2 + 1] = ToU8(ValueFbm(Centre(x, size), Centre(y, size), 2, gs));
    }
  return data;
}
// The next mip level of a volume of 4 byte texels: each texel the mean of 2x2x2.
std::vector<uint8_t> HalveVolume(const std::vector<uint8_t>& src, uint32_t size) {
  const uint32_t n = size / 2;
  std::vector<uint8_t> dst(size_t(n) * n * n * 4);
  for (uint32_t z = 0; z < n; ++z)
    for (uint32_t y = 0; y < n; ++y)
      for (uint32_t x = 0; x < n; ++x)
        for (uint32_t c = 0; c < 4; ++c) {
          uint32_t sum = 0;
          for (uint32_t k = 0; k < 8; ++k)
            sum += src[((size_t(z * 2 + (k >> 2)) * size + y * 2 + ((k >> 1) & 1)) * size + x * 2 + (k & 1)) * 4 + c];
          dst[((size_t(z) * n + y) * n + x) * 4 + c] = uint8_t((sum + 4) / 8);
        }
  return dst;
}

constexpr uint32_t kShapeSize = 128, kDetailSize = 32, kWeatherSize = 512;

// ---------------------------------------------------------------------------------------------------------------
// The shaders.

constexpr const char* kVS = R"(#version 450
layout(location = 0) out vec2 v_uv;
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  v_uv = vec2(p.x, 1.0 - p.y);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// D193: the player is on foot (SetEyeOnFoot; true until told otherwise).
std::atomic<bool> g_eye_on_foot{true};

// What both pixel shaders share: the constants, the ray of a view coordinate, and the sky's colour (Beachhead's
// sky_color.gdshaderinc: the gradient, the sun, at night the stars and the moon, the cirrus).
constexpr const char* kCommon = R"(#version 450
layout(std140, set = 0, binding = 0) uniform SkyConstants {
  vec4 inv0, inv1, inv2;  // the world from clip x, y and w, rows
  vec4 eye;               // xyz the camera; w the world's time, seconds
  vec4 to_ndc;            // the view's 0 to 1 coordinate to NDC (uv * xy + zw)
  vec4 view;              // xy the view's size in pixels; z the exposure; w the debug mode
  vec4 sun_dir;           // xyz toward the sun; w its energy
  vec4 sun_color;         // rgb; w the night, 0 to 1
  vec4 moon_dir;          // xyz toward the moon; w the cirrus's cover
  vec4 moon_color;        // rgb
  vec4 moon_light;        // rgb: the moon's light on the clouds
  vec4 fog_color, zenith_color, horizon_color, mid_color;  // the sky's palette, horizon to zenith
  vec4 cloud;             // x the cover, y the type, z the base's altitude, w the layer's thickness
  vec4 drift;             // xy the clouds' drift, m; z their slow change; w their density
  vec4 last0, last1, last2;  // the last frame's clip x, y and w from a world direction, rows
  vec4 last_to_ndc;          // and its view coordinate to NDC
  vec4 misc;                 // x this frame's number (the march's start in each pixel); y 1 with a last frame to blend; z the cloud layer's scale in the world; w how much of this frame the blend takes
  vec4 eye_world;            // xyz the camera in the world; w the sea's height there
  vec4 fog_a;                // x the haze's density at the sea, 1/m; y its scale height; z the ground fog's density; w its top over the sea
  vec4 fog_b;                // x the stored depth's numerator; y the rain shafts, 0 to 1; z 1 with the sea's height known; w the rain here, 0 to 1
  vec4 flash;                // xyz the lightning's place (x, the height over the sea, z); w its light now
  vec4 cover_m[9];           // the rain's cover: three cascades' transforms from the world, three rows each
  vec4 rain;                 // x how far the rain leans (the tangent); y 1 with a cover map; the lens: z how many of the rain's drops, w of a splash's (view.z and view.w: how strongly each shows)
  vec4 ssr;                  // D190: x the sea's reflection of the renderer's own (0 off, 1 on); y the wind, 0 to 1
  vec4 bolt;                 // D192: x how many of the bolt's pieces (0: none); y its light now; z pixels a metre makes at 1 m; w 1: the clouds skip hidden rays
  vec4 bolt_box;             // the view's pixels it may show in (x0, y0, x1, y1)
  vec4 bolt_seg[144];        // each piece: its ends in the view's pixels (ax, ay, bx, by); their distances along the view, its half width (m), how bright
  vec4 gale;                 // D194: x the storm's mist, 0 to 1 (the ground fog filled in, low on the ground under what is seen)
  vec4 storm_a;              // D208: the hurricane (storm.h): its centre (x, z), its eye's radius and its rain shield's
  vec4 storm_b;              // its strength, its bands' turn, 1 while there is one, its volutus (D211)
  vec4 layers;               // D211: x the altocumulus's cover, y its brightness; z the clouds' shadows on (D214)
};
layout(set = 0, binding = 1) uniform sampler2D weatherTex;

vec3 Ray(vec2 uv) {
  vec2 ndc = uv * to_ndc.xy + to_ndc.zw;
  vec3 c = vec3(ndc, 1.0);
  return normalize(vec3(dot(inv0.xyz, c), dot(inv1.xyz, c), dot(inv2.xyz, c)));
}
float hash13(vec3 p) {
  p = fract(p * 0.1031);
  p += dot(p, p.zyx + 31.32);
  return fract((p.x + p.y) * p.z);
}
float h21(vec2 p) {
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}
float vnoise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(h21(i), h21(i + vec2(1.0, 0.0)), f.x), mix(h21(i + vec2(0.0, 1.0)), h21(i + vec2(1.0, 1.0)), f.x), f.y);
}
vec3 stars(vec3 rd) {
  vec3 q = rd * 95.0;
  vec3 id = floor(q);
  vec3 f = fract(q) - 0.5;
  float h = hash13(id);
  if (h < 0.975) return vec3(0.0);
  vec3 off = vec3(hash13(id + 7.1), hash13(id + 13.7), hash13(id + 29.3)) - 0.5;
  float d = length(f - off * 0.5);
  float mag = 0.35 + 0.65 * hash13(id + 3.3);
  float twinkle = 1.0 + 0.25 * sin(eye.w * (2.0 + 3.0 * h) + h * 400.0);
  vec3 tint = mix(vec3(0.75, 0.85, 1.0), vec3(1.0, 0.9, 0.75), hash13(id + 91.7));
  return tint * smoothstep(0.2, 0.02, d) * mag * mag * twinkle * 2.2;
}
// The moon's face without a picture of it: a lit sphere with a few dark seas of noise.
vec3 lunar_surface(vec2 uv) {
  float z = sqrt(max(0.0, 1.0 - dot(uv, uv)));
  float seas = smoothstep(0.45, 0.7, vnoise(uv * 2.3 + 4.0) * 0.6 + vnoise(uv * 5.1 + 9.0) * 0.4);
  float sphere_light = 0.40 + 0.60 * max(dot(vec3(uv, z), normalize(vec3(-0.25, 0.15, 1.0))), 0.0);
  return vec3(0.82, 0.83, 0.80) * mix(1.0, 0.62, seas) * sphere_light * 1.1;
}
vec3 sky_color(vec3 rd) {
  float up = clamp(rd.y, 0.0, 1.0);
  vec3 sky = mix(fog_color.rgb, horizon_color.rgb, smoothstep(0.0, 0.1, up));
  sky = mix(sky, mid_color.rgb, smoothstep(0.1, 0.5, up));
  sky = mix(sky, zenith_color.rgb, smoothstep(0.5, 1.0, up));
  float night_amount = sun_color.w;
  float sun_cos = dot(rd, normalize(sun_dir.xyz));
  sky += sun_color.rgb * sun_dir.w * (0.18 * pow(max(sun_cos, 0.0), 8.0) + 0.6 * pow(max(sun_cos, 0.0), 400.0));
  float sun_edge = max(fwidth(sun_cos), 0.000006);
  float sun_disc = smoothstep(cos(radians(2.0)) - sun_edge, cos(radians(2.0)) + sun_edge, sun_cos);
  sky += sun_color.rgb * sun_disc * 6.0 * sun_dir.w;
  if (night_amount > 0.0) {
    float vis = smoothstep(0.02, 0.3, rd.y) * night_amount * night_amount;
    sky += stars(rd) * vis;
    vec3 md = normalize(moon_dir.xyz);
    float moon_cos = dot(rd, md);
    float edge = max(fwidth(moon_cos), 0.000004);
    float disc = smoothstep(cos(radians(1.5)) - edge, cos(radians(1.5)) + edge, moon_cos);
    vec3 moon_tex = vec3(0.0);
    if (disc > 0.0) {
      vec3 right = normalize(cross(abs(md.y) > 0.98 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0), md));
      vec3 top = cross(md, right);
      vec2 muv = vec2(dot(rd, right), dot(rd, top)) / sin(radians(1.5));
      moon_tex = lunar_surface(muv);
    }
    sky = mix(sky, moon_tex * 1.35, disc * night_amount);
    sky += moon_color.rgb * (0.10 * pow(max(moon_cos, 0.0), 400.0) + 0.035 * pow(max(moon_cos, 0.0), 24.0)) * night_amount;
  }
  // D211: altocumulus, a mid-level layer 5 km up (the user, 19 Nov 2026: "Clouds should be able to form on multiple
  // altitude layers, including cirrus high above"): small rounded cells in patches (two of the weather map's scales),
  // drifting slower than the low clouds, their tops lit toward the sun, grey beneath.
  if (rd.y > 0.01 && layers.x > 0.0) {
    vec2 hit = rd.xz / rd.y * 5000.0 + drift.xy * 0.35;
    float clump = smoothstep(0.35, 0.75, texture(weatherTex, hit / 26000.0 + 0.13).g);
    float cells = texture(weatherTex, hit / 2400.0).r * 0.55 + texture(weatherTex, hit / 700.0 + 0.41).g * 0.45;
    float c = smoothstep(1.0 - layers.x, 1.0 - layers.x + 0.18, cells) * clump;
    vec2 to_sun = normalize(sun_dir.xz + vec2(1e-4));
    float lit = texture(weatherTex, (hit - to_sun * 60.0) / 700.0 + 0.41).g;
    float shade = clamp(0.55 + (cells - lit) * 3.0, 0.25, 1.0);
    c *= smoothstep(0.01, 0.12, rd.y);
    vec3 grey = mix(fog_color.rgb, mid_color.rgb, 0.5) * 0.9;
    vec3 top = vec3(1.0) * (0.75 + 0.35 * sun_dir.w) * (1.0 - 0.92 * night_amount);
    sky = mix(sky, mix(grey, top, shade) * layers.y, c * 0.85);
  }
  // D212: the hurricane's wall seen from outside it: the low layer's own clouds fade into the horizon's haze a few
  // degrees up, so a storm a few kilometres off did not show. Toward the storm, over the angle its dense part takes,
  // its towers at the eyewall (3 km tall as it forms, 10 km at full strength; the anvil's flat top): dark blue-grey
  // in the sky's own colours, the top lit, the foot in the haze. Only while the eye is outside the dense part.
  if (storm_b.z > 0.5 && rd.y > -0.02) {
    vec2 to = storm_a.xy - eye.xz;
    float dist = length(to);
    float dense = storm_a.w * 0.75;
    if (dist > dense * 0.9) {
      float edge = max(dist - 1.8 * storm_a.z, 400.0);  // to the eyewall, where the towers stand
      vec2 dir = normalize(rd.xz + vec2(1e-5));
      float off = acos(clamp(dot(dir, to / dist), -1.0, 1.0));
      float span = asin(clamp(dense / dist, 0.0, 1.0));
      float across = off / max(span, 1e-3);
      if (across < 1.0) {
        float az = atan(dir.y, dir.x);
        // Its towers 0.6 km tall as the storm forms, 2 km at full strength (D213: 3 and 10 km before, too large for the
        // game's levels); an anvil's broad flat top.
        float tall = 600.0 + 1400.0 * smoothstep(0.25, 1.0, storm_b.x * smoothstep(0.0, 1.0, 1.0 - dist / (4.0 * storm_a.w)));
        float top = atan(tall / edge) * sqrt(max(1.0 - pow(across, 4.0), 0.0));
        top *= 0.75 + 0.25 * texture(weatherTex, vec2(az * 3.0, 0.37)).r + 0.15 * texture(weatherTex, vec2(az * 17.0, 0.71)).g;
        float el = asin(clamp(rd.y, -1.0, 1.0));
        if (el < top) {
          float h = clamp(el / max(top, 1e-3), 0.0, 1.0);
          // Billows: the weather map at two scales, alike across and up (stretched up it streaked like a curtain).
          float body = texture(weatherTex, vec2(az, el) * 5.0 + 0.1).r * 0.6 + texture(weatherTex, vec2(az, el) * 15.0 + 0.37).g * 0.4;
          // In the sky's own colours: a dark blue-grey body, its top lit near the horizon's brightness.
          vec3 base = mix(fog_color.rgb, horizon_color.rgb, 0.5);
          vec3 dark = base * mix(0.32, 0.5, body) * vec3(0.85, 0.92, 1.05);
          vec3 lit = horizon_color.rgb * 1.05;
          vec3 wall = mix(dark, lit, smoothstep(0.75, 1.0, h) * (0.4 + 0.6 * body));
          wall *= 1.0 - 0.9 * night_amount;
          float amount = storm_b.x * smoothstep(1.0, 0.85, across) * smoothstep(top, top * 0.93, el);
          float haze = 1.0 - exp(-edge / 30000.0);
          sky = mix(sky, mix(wall, fog_color.rgb, haze * 0.8), amount * smoothstep(dense * 0.9, dense * 1.2, dist));
        }
      }
    }
  }
  // Cirrus: the ray to a plane 10 km up, the weather map stretched along the upper wind into streaks.
  float cirrus = moon_dir.w;
  if (rd.y > 0.01 && cirrus > 0.0) {
    vec2 hit = rd.xz / rd.y * 10000.0 + drift.xy * 0.6;
    vec2 along = normalize(vec2(-0.97, -0.24));
    vec2 cuv = vec2(dot(hit, along) / 60000.0, dot(hit, vec2(-along.y, along.x)) / 9000.0);
    float n = texture(weatherTex, cuv).r * 0.65 + texture(weatherTex, cuv * 3.7 + 0.31).r * 0.35;
    float c = smoothstep(1.0 - cirrus, 1.0 - cirrus + 0.35, n);
    c *= smoothstep(0.01, 0.15, rd.y);
    sky = mix(sky, vec3(1.0) * (0.8 + 0.3 * sun_dir.w) * (1.0 - 0.93 * night_amount), c * 0.55);
  }
  return sky;
}
)";

// The clouds (Beachhead's cloud_core.gdshaderinc, without its towers and its storms): a layer between two altitudes on
// a round planet, marched along the ray; premultiplied light in rgb and what shows through in a.
constexpr const char* kCloudBindings = R"(
layout(set = 0, binding = 2) uniform sampler3D shapeTex;
layout(set = 0, binding = 3) uniform sampler3D detailTex;
layout(set = 0, binding = 7) uniform sampler2D cloudDepthTex;  // the view's depth (full size), for the hidden rays
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_where;
)";
// The clouds' density where they are (D214: shared with the fog's pass, for the clouds' shadows on the ground).
constexpr const char* kCloudShared = R"(
const float PI = 3.14159265;
const float PLANET_R = 6360000.0;
const float shape_scale = 9000.0;
const float detail_scale = 1400.0;
const float weather_scale = 40000.0;
const float max_distance = 30000.0;
const float interior_extinction = 0.60;
const float outside_extinction = 0.16;
const int steps = 48;
const int light_steps = 5;

vec2 sphere_hits(vec3 ro, vec3 rd, float r) {
  vec3 c = vec3(ro.x, -PLANET_R, ro.z);
  vec3 oc = ro - c;
  float b = dot(oc, rd);
  float q = dot(oc, oc) - r * r;
  float h = b * b - q;
  if (h < 0.0) return vec2(-1.0);
  h = sqrt(h);
  return vec2(-b - h, -b + h);
}
float altitude(vec3 p, vec3 cam) { return length(p - vec3(cam.x, -PLANET_R, cam.z)) - PLANET_R; }
float remap(float v, float lo, float hi, float a, float b) { return a + (clamp(v, lo, hi) - lo) / max(hi - lo, 1e-5) * (b - a); }
float height_gradient(float h, float ctype) {
  float stratus = smoothstep(0.0, 0.08, h) * (1.0 - smoothstep(0.15, 0.3, h));
  float cumulus = smoothstep(0.0, 0.04, h) * (1.0 - smoothstep(0.35, 0.9, h));
  float cb = smoothstep(0.0, 0.1, h) * (1.0 - smoothstep(0.85, 1.0, h));
  return ctype < 0.5 ? mix(stratus, cumulus, ctype * 2.0) : mix(cumulus, cb, ctype * 2.0 - 1.0);
}
// The fair-weather deck in three altitude bands, each with its own pattern: clouds above and below each other.
float layer_amount(float cov, float thick) {
  return smoothstep(900.0, 1200.0, thick) * (1.0 - smoothstep(1600.0, 2200.0, thick)) * (1.0 - smoothstep(0.7, 0.95, cov));
}
float cloud_density(vec3 p, vec3 cam, bool cheap) {
  float coverage = cloud.x, cloud_type = cloud.y, base_altitude = cloud.z, thickness = cloud.w;
  float evolve = drift.z;
  vec2 wp = p.xz + drift.xy;
  float alt = altitude(p, cam);
  if (alt - base_altitude > thickness + 400.0) return 0.0;
  float lay = layer_amount(coverage, thickness) * (1.0 - smoothstep(0.15, 0.4, cloud_type));
  float band_h = mix(thickness, thickness / 3.0, lay);
  float band = lay > 0.01 ? clamp(floor((alt - base_altitude) / band_h), 0.0, 2.0) : 0.0;
  wp += band * vec2(7300.0, 4100.0);
  vec2 wm = texture(weatherTex, wp / weather_scale).rg;
  float cover = clamp(coverage * mix(0.6 + wm.r * 0.8, (0.2 + wm.r * 1.6) * 0.66, lay), 0.0, 1.0);
  // D208: the hurricane's cloud where it is: a whole deck under its strength, its layers merged into one.
  float storm = storm_at(p.xz * max(misc.z, 1e-3), storm_a, storm_b);
  cover = mix(cover, 1.0, 0.92 * storm);
  lay *= 1.0 - storm;
  // D211: in the storm's bands the low clouds tower (cumulonimbus: the tallest kind, the whole layer's depth).
  float storm_type = smoothstep(0.2, 0.7, storm);
  float local_type = clamp(cloud_type + (wm.g - 0.5) * 0.8 + (wm.r - 0.5) * 0.6, 0.0, 1.0);
  local_type = mix(local_type, 1.0, storm_type);
  float local_base = base_altitude + local_type * local_type * 400.0;
  local_base += (vnoise(wp / 3200.0 + 17.0) - 0.5) * mix(300.0, 0.0, lay);
  local_base += lay * (band * band_h - local_type * local_type * 400.0 + vnoise(wp / 2600.0 + 31.0) * 90.0);
  float local_thick = thickness * mix(0.3, 1.0, smoothstep(0.3, 0.9, local_type));
  local_thick = mix(local_thick, band_h * 0.66 * mix(0.45, 1.0, vnoise(wp / 4200.0 + 41.0)), lay);
  // How deep inside a cloud mass this column is, from the shape noise at the base: small puffs stay shallow.
  vec4 fs = textureLod(shapeTex, vec3(wp.x, local_base + evolve * 40.0, wp.y) / shape_scale, 2.0);
  float foot = clamp((fs.r - (1.0 - cover)) / max(cover, 1e-3), 0.0, 1.0);
  local_thick *= mix(0.32, 1.0, pow(foot, mix(0.6, 0.95, lay)));
  float h = (alt - local_base) / local_thick;
  float low = 0.0;
  if (h >= 0.0 && h <= 1.0) {
    vec3 sp = vec3(wp.x, p.y + evolve * 40.0, wp.y) / shape_scale;
    vec4 s = textureLod(shapeTex, sp + vec3(0.0, 0.0, evolve * 0.002), 0.0);
    float wfbm = s.g * 0.625 + s.b * 0.25 + s.a * 0.125;
    float base = remap(s.r, wfbm - 1.0, 1.0, 0.0, 1.0);
    float dome = smoothstep(0.0, 0.05, h) * sqrt(max(1.0 - pow(h, 1.7), 0.0)) * mix(1.0, 0.8, smoothstep(0.55, 1.0, h));
    base *= mix(height_gradient(h, local_type), dome, lay);
    float c = remap(base, 1.0 - cover, 1.0, 0.0, 1.0);
    c = clamp(c * (1.0 + 0.3 * lay), 0.0, 1.0);
    if (c > 0.0 && !cheap) {
      vec3 d = textureLod(detailTex, vec3(wp.x, p.y, wp.y) / detail_scale + evolve * 0.01, 0.0).rgb;
      float dfbm = d.r * 0.625 + d.g * 0.25 + d.b * 0.125;
      float erode = mix(dfbm, 1.0 - dfbm, clamp(h * 4.0, 0.0, 1.0));
      c = remap(c, erode * 0.7 * (1.0 - 0.25 * lay), 1.0, 0.0, 1.0);
    }
    if (!cheap) c = clamp(c * mix(2.2, 1.0, lay), 0.0, 1.0);  // crisp silhouettes in fair weather
    // D217: under the storm a whole deck, its noise only its underside's texture (at 75% between the bands the cover
    // alone left broken cumulus and blue sky, run stormsky1): filled in from 30% of its strength, whole by 70%.
    c = mix(c, max(c, 0.55 + 0.45 * base), smoothstep(0.3, 0.7, storm) * smoothstep(0.0, 0.08, h) * (1.0 - smoothstep(0.8, 1.0, h)));
    low = max(c, 0.0);
  }
  // D211: the volutus, a roll cloud along the storm's gust front (the user: "Cloud generation should match photoreal
  // topical clouds too, including volutus clouds"): a long horizontal tube low in the layer just outside the rain
  // shield, its cross-section 450 of the layer's units round, broken here and there along its length and eroded at its
  // edges, while the storm comes in (storm_b.w).
  if (storm_b.w > 0.0 && storm_b.z > 0.5) {
    float ws = max(misc.z, 1e-3);
    vec2 rel = p.xz * ws - storm_a.xy;
    float ring = (storm_a.w * 1.05) / ws;
    vec2 q = vec2(length(rel) / ws - ring, (alt - base_altitude - 380.0) * 1.3) / 450.0;
    float tube = 1.0 - smoothstep(0.45, 1.0, length(q));
    if (tube > 0.0) {
      float along = atan(rel.y, rel.x) * ring;
      float breaks = smoothstep(0.25, 0.55, vnoise(vec2(along / 2600.0, 3.7)));
      vec4 s = textureLod(shapeTex, vec3(p.x, p.y * 2.0, p.z) / 3000.0, 0.0);
      float roll = tube * breaks * (0.55 + 0.45 * s.r);
      if (!cheap) {
        vec3 d = textureLod(detailTex, p / detail_scale, 0.0).rgb;
        roll = remap(roll, (d.r * 0.625 + d.g * 0.25 + d.b * 0.125) * 0.5, 1.0, 0.0, 1.0);
      }
      low = max(low, clamp(roll * 1.6, 0.0, 1.0) * storm_b.w);
    }
  }
  return low * drift.w * (1.0 + 0.9 * storm);
}
)";
constexpr const char* kCloudPS = R"(
float hg(float cos_t, float g) {
  float g2 = g * g;
  return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * cos_t, 1.5));
}
float ign(vec2 px) { return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))); }
// Where the ray crosses the cloud layer; false when it does not.
bool cloud_shell(vec3 cam, vec3 rd, out float t0, out float t1) {
  float base_altitude = cloud.z, top = cloud.w + 400.0;
  float cam_alt = altitude(cam, cam);
  vec2 lo = sphere_hits(cam, rd, PLANET_R + base_altitude);
  vec2 hi = sphere_hits(cam, rd, PLANET_R + base_altitude + top);
  if (cam_alt < base_altitude) {
    t0 = lo.y;
    t1 = hi.y;
  } else if (cam_alt > base_altitude + top) {
    t0 = hi.x;
    t1 = lo.x > 0.0 ? lo.x : hi.y;
  } else {
    t0 = 0.0;
    t1 = lo.x > 0.0 ? lo.x : hi.y;
  }
  t0 = max(t0, 0.0);
  return t1 > t0;
}
void main() {
  // The layer is marched at its own scale (Beachhead's), the eye and the distances taken into it: in the world it is
  // smaller by misc.z, and looks the same from the ground.
  float world_scale = max(misc.z, 1e-3);
  vec3 rd = Ray(v_uv);
  vec3 cam = eye.xyz / world_scale;
  out_color = vec4(0.0, 0.0, 0.0, 1.0);
  out_where = vec4(0.0);
  float t0, t1;
  bool under = cam.y < cloud.z;
  if ((under && rd.y < -0.02) || !cloud_shell(cam, rd, t0, t1)) return;
  t1 = min(t1, 60000.0);
  float thickness = cloud.w, base_altitude = cloud.z;
  float len_low = t1 - t0;
  if (len_low < 1.0) return;
  // A hidden ray is not marched (the Night Ops performance pass, 20 Nov 2026: the march was most of the sky's 3 ms a
  // frame in a storm, and its long low rays, up to 144 steps, are the ones the island, the ship and the buildings
  // hide). Hidden: every full size pixel this one covers, and one more round them (the reach of the passes' bilinear
  // reads of this texture), shows the scene nearer than where the ray enters the layer. Those pixels show no sky and
  // none of this ray's cloud (the over pass takes only cloud that lies before the scene), so what is written here is
  // never seen.
  if (bolt.w > 0.5) {
    ivec2 full = ivec2(view.xy);
    ivec2 b = ivec2(gl_FragCoord.xy) * 2 - 1;
    float far_depth = 1e9;  // the smallest stored depth among them: the farthest (0: the sky)
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 4; ++x)
        far_depth = min(far_depth, texelFetch(cloudDepthTex, clamp(b + ivec2(x, y), ivec2(0), full - 1), 0).r);
    }
    if (far_depth > 1e-6) {
      vec2 ndc = v_uv * to_ndc.xy + to_ndc.zw;
      vec3 c = vec3(ndc, 1.0);
      vec3 r = vec3(dot(inv0.xyz, c), dot(inv1.xyz, c), dot(inv2.xyz, c));
      if (fog_b.x / far_depth * length(r) < t0 * world_scale * 0.9) return;
    }
  }
  // Long grazing paths get more steps.
  int n = clamp(int(len_low / 150.0), steps, steps * 3);
  float dt = len_low / float(n);
  float near_d = max(cloud_density(cam, cam, true), cloud_density(cam + rd * 40.0, cam, true));
  float inside = smoothstep(0.0, 0.08, near_d);
  float extinction = mix(outside_extinction, interior_extinction, inside);
  // Each pixel's march starts elsewhere within a step, another place each frame (the golden ratio spreads a pixel's
  // starts evenly over the step): the frames blended (the resolve) smooth the grain away.
  float t = t0 + (inside > 0.01 ? min(dt, 12.0) * 0.5 : dt * fract(ign(gl_FragCoord.xy) + 0.618034 * misc.x));
  float night_amount = sun_color.w;
  bool moon_key = night_amount > 0.5;
  vec3 ls = normalize(moon_key ? moon_dir.xyz : sun_dir.xyz);
  vec3 key_col = moon_key ? moon_light.rgb * 4.5 : sun_color.rgb * sun_dir.w;
  float cos_t = dot(rd, ls);
  vec3 ambient_top = mix(fog_color.rgb, mid_color.rgb, 0.6);
  vec3 scatter = vec3(0.0);
  float trans = 1.0, w_all = 0.0, w_t = 0.0, w_tt = 0.0, t_first = -1.0;
  for (int i = 0; i < n * 4 + 16; i++) {
    if (t > t1) break;
    vec3 p = cam + rd * t;
    float ray_step = inside > 0.01 && t < 192.0 ? min(dt, 12.0) : dt;
    float d = cloud_density(p, cam, false);
    if (d > 0.002) {
      float rel = altitude(p, cam) - base_altitude;
      // The light's way toward the sun or the moon: five steps of 80 m through the uneroded density.
      float od = 0.0, prev = 0.0;
      for (int j = 1; j <= light_steps; j++) {
        float dj = 80.0 * float(j);
        od += cloud_density(p + ls * dj, cam, true) * (dj - prev);
        prev = dj;
      }
      // The eroded density a short way toward the light: where it drops, this billow faces the light.
      float d_sun = cloud_density(p + ls * 50.0, cam, false);
      float facing = clamp(0.5 + (d - d_sun) / max(d, 0.12) * 0.5, 0.0, 1.0);
      float od_l = od * 0.09 * 0.5;
      float powder = 1.0 - exp(-od_l * 2.0);
      float h = clamp(rel / thickness, 0.0, 1.0);
      vec3 amb = mix(fog_color.rgb * 0.45, ambient_top, h * h) * 1.1;  // (Beachhead: 0.16 and 0.75; seen from the ground the bases were heavy)
      // Multiple scattering in three octaves: each sees less extinction and a flatter phase.
      float ms = 0.0, oa = 1.0, ob = 1.0, oc = 1.0;
      for (int o = 0; o < 3; o++) {
        ms += ob * exp(-od_l * oa) * (mix(hg(cos_t, 0.6 * oc), hg(cos_t, -0.25 * oc), 0.3) * 10.0 + 0.5);
        oa *= 0.4;
        ob *= 0.5;
        oc *= 0.5;
      }
      float surface = mix(0.5, 1.35, facing);
      vec3 light = key_col * ms * mix(1.0, powder, 0.45) * 0.8 * surface + amb + moon_light.rgb * (0.4 + 0.6 * h);
      // The lightning: a light inside the layer, falling off around it.
      if (flash.w > 0.0) {
        vec3 dp = p - flash.xyz / world_scale;
        light += vec3(0.72, 0.8, 1.0) * flash.w * 14.0 / (1.0 + dot(dp, dp) / (800.0 * 800.0));
      }
      // Thin wisps warm white, dense cores blue-grey.
      float dn = smoothstep(0.15, 1.0, d / max(drift.w, 1e-3));
      light *= mix(vec3(1.0), mix(vec3(1.0, 0.97, 0.91), vec3(0.6, 0.66, 0.76), dn), 0.8);
      float a = exp(-d * extinction * ray_step);
      float w = trans * (1.0 - a);
      scatter += light * w;
      w_all += w;
      w_t += w * t;
      w_tt += w * t * t * 1e-6;
      if (t_first < 0.0) t_first = max(t - ray_step, t0);
      trans *= a;
      if (trans < 0.01) break;
    }
    t += ray_step;
  }
  // Far clouds fade into the haze.
  float t_eff = w_t / max(w_all, 1e-4);
  float haze = 1.0 - exp(-t_eff / (max_distance * 0.45));
  scatter = mix(scatter, fog_color.rgb * (1.0 - trans), haze);
  // Toward the horizon the far clouds thin out into the haze (they stood on it as a dark band: run sky10).
  // (Only from under the layer: in it and over it the clouds are all around and below.)
  float low = mix(smoothstep(0.0, 0.07, rd.y), 1.0, smoothstep(cloud.z - 150.0, cloud.z, cam.y));
  out_color = vec4(scatter * low, mix(1.0, trans, low));
  // Where the cloud lies along the ray, in the world's kilometres: its mean distance, half its extent (a slab's, from
  // its spread) and its beginning; each times how much cloud there is, so that blending them stays right.
  float mean_km = t_eff * 1e-3;
  float half_km = sqrt(max(w_tt / max(w_all, 1e-4) - mean_km * mean_km, 0.0)) * 1.73 + dt * 0.5e-3;
  out_where = vec4(mean_km * world_scale, half_km * world_scale, 1.0, max(t_first, 0.0) * 1e-3 * world_scale) * w_all;
}
)";

// This frame's clouds blended with the last frames', taken where they are seen now.
constexpr const char* kResolvePS = R"(
layout(set = 0, binding = 2) uniform sampler2D cloudTex;
layout(set = 0, binding = 3) uniform sampler2D historyTex;
layout(set = 0, binding = 4) uniform sampler2D whereTex;
layout(set = 0, binding = 5) uniform sampler2D whereHistoryTex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_where;
void main() {
  vec4 now = textureLod(cloudTex, v_uv, 0.0);
  vec4 now_w = textureLod(whereTex, v_uv, 0.0);
  out_color = now;
  out_where = now_w;
  if (misc.y < 0.5) return;
  vec3 rd = Ray(v_uv);
  vec3 c = vec3(dot(last0.xyz, rd), dot(last1.xyz, rd), dot(last2.xyz, rd));
  if (c.z <= 1e-4) return;
  vec2 uv = (c.xy / c.z - last_to_ndc.zw) / last_to_ndc.xy;
  if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return;
  vec4 then = textureLod(historyTex, uv, 0.0);
  vec4 then_w = textureLod(whereHistoryTex, uv, 0.0);
  // Kept within what this frame shows around the pixel: clouds that moved or changed do not leave a trail.
  vec2 px = 1.0 / vec2(textureSize(cloudTex, 0));
  vec4 lo = now, hi = now, lo_w = now_w, hi_w = now_w;
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      vec2 at = v_uv + vec2(float(x), float(y)) * px;
      vec4 s = textureLod(cloudTex, at, 0.0), sw = textureLod(whereTex, at, 0.0);
      lo = min(lo, s), hi = max(hi, s);
      lo_w = min(lo_w, sw), hi_w = max(hi_w, sw);
    }
  then = clamp(then, lo, hi);
  then_w = clamp(then_w, lo_w, hi_w);
  // (The faster the eye moves through the layer, the more of this frame: the frames before were seen from elsewhere.)
  float take = misc.w > 0.0 ? misc.w : 0.1;
  out_color = mix(then, now, take);
  out_where = mix(then_w, now_w, take);
}
)";

// The sky drawn where the view's depth holds nothing, the clouds over it.
constexpr const char* kSkyPS = R"(
layout(set = 0, binding = 2) uniform sampler2D depthTex;
layout(set = 0, binding = 3) uniform sampler2D cloudTex;
layout(set = 0, binding = 4) uniform sampler3D shapeDebugTex;
layout(set = 0, binding = 5) uniform sampler3D detailDebugTex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
void main() {
  float depth = texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r;
  int debug = int(view.w + 0.5);
  if (debug == 2) {  // everything painted: green where the depth holds nothing, red elsewhere
    out_color = vec4((depth > 1e-6 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0)) * view.z, 1.0);
    return;
  }
  if (depth > 1e-6) discard;
  if (debug == 1) {  // the sky's pixels in one flat colour
    out_color = vec4(vec3(1.0, 0.0, 1.0) * view.z, 1.0);
    return;
  }
  vec2 uv = gl_FragCoord.xy / view.xy;
  if (debug == 3) {  // the noise volumes' slices: the shape's four channels, then the detail's
    vec3 q = vec3(uv.x * 5.0, uv.y * 2.0, 0.37);
    int band = int(uv.x * 5.0);
    vec4 s = textureLod(shapeDebugTex, q, 0.0);
    float v = band == 0 ? s.r : band == 1 ? s.g : band == 2 ? s.b : band == 3 ? s.a : textureLod(detailDebugTex, q, 0.0).r;
    out_color = vec4(vec3(v) * view.z, 1.0);
    return;
  }
  vec3 rd = Ray(uv);
  vec3 sky = sky_color(rd);
  vec4 clouds = textureLod(cloudTex, uv, 0.0);
  vec3 seen = sky * clouds.a + clouds.rgb;
  // Under the weather's whole cover the deck is one flat grey seen from below ("Sky box does this when the camera
  // looks up to the sky", 14 Nov 2026: nothing but a grey field): lighter and darker patches are laid over it, as an
  // overcast's base has, drifting with the clouds.
  float heavy = clamp((drift.w - 1.0) / 0.8, 0.0, 1.0);
  if (heavy > 0.0 && rd.y > 0.02) {
    vec2 hit = rd.xz / rd.y * 1500.0 + drift.xy;
    float patches = texture(weatherTex, hit / 14000.0).r * 0.6 + texture(weatherTex, hit / 3700.0 + 0.37).g * 0.4;
    seen *= mix(1.0, 0.66 + 0.68 * patches, heavy * smoothstep(0.02, 0.3, rd.y));
  }
  out_color = vec4(seen * view.z, 1.0);
}
)";

// The clouds over what the view's depth holds: as much of each ray's cloud as lies before it. Premultiplied, as the
// fog.
constexpr const char* kOverPS = R"(
layout(set = 0, binding = 2) uniform sampler2D depthTex;
layout(set = 0, binding = 3) uniform sampler2D cloudTex;
layout(set = 0, binding = 4) uniform sampler2D whereTex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
void main() {
  float depth = texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r;
  if (depth <= 1e-6) discard;  // the sky: the clouds are in it already
  vec2 uv = gl_FragCoord.xy / view.xy;
  vec4 clouds = textureLod(cloudTex, uv, 0.0);
  vec4 where = textureLod(whereTex, uv, 0.0);
  float opacity = 1.0 - clouds.a;
  if (opacity < 0.004 || where.z < 1e-3) discard;
  vec2 ndc = uv * to_ndc.xy + to_ndc.zw;
  vec3 c = vec3(ndc, 1.0);
  vec3 r = vec3(dot(inv0.xyz, c), dot(inv1.xyz, c), dot(inv2.xyz, c));
  float km = fog_b.x / depth * length(r) * 1e-3;
  float mean = where.x / where.z, half_km = where.y / where.z, first = where.w / where.z;
  float before = km <= first ? 0.0 : smoothstep(max(mean - half_km, first), mean + half_km, km);
  if (before <= 0.0) discard;
  out_color = vec4(clouds.rgb * before * view.z, 1.0 - opacity * before);
}
)";

// The fog over the view: what the haze, the low layer and the far rain put between the eye and what the depth holds.
// Premultiplied: rgb the fog's own light, a what still shows through.
constexpr const char* kFogShared = R"(
layout(set = 0, binding = 2) uniform sampler2D depthTex;
layout(set = 0, binding = 3) uniform sampler2DArray coverTex;
layout(set = 0, binding = 4) uniform sampler2D sceneTex;  // D190: the scene as drawn before the fog (the sea's reflection)
layout(set = 0, binding = 5) uniform sampler2D seaMaskTex;  // D190: where the sea is drawn (its depth; 0 elsewhere)
layout(set = 0, binding = 6) uniform sampler2D seaAfterTex;  // D190: the picture just after the sea was drawn
layout(set = 0, binding = 7) uniform sampler2D seaFloorTex;  // D190: the world's depth without the sea (0: not known)
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
// 1 where the world's point p is under cover: in the first cascade of the game's shadow map that holds it, something
// stands between it and the light (40 cm nearer at the least).
float Covered(vec3 p) {
  if (rain.y < 0.5) return 0.0;
  for (int i = 0; i < 3; ++i) {
    vec4 rx = cover_m[i * 3], ry = cover_m[i * 3 + 1], rz = cover_m[i * 3 + 2];
    vec2 at = vec2(dot(rx.xyz, p) + rx.w, dot(ry.xyz, p) + ry.w);
    if (abs(at.x) < 0.48 && abs(at.y) < 0.48) {
      float stored = textureLod(coverTex, vec3(at + 0.5, float(i)), 0.0).r;
      return dot(rz.xyz, p) + rz.w > stored + 0.4 * length(rz.xyz) ? 1.0 : 0.0;
    }
  }
  return 0.0;
}
// The unnormalised ray of a pixel: the world's point at the view distance w along it is the eye + w * ray.
vec3 ViewRay(vec2 px) {
  vec2 ndc = px / view.xy * to_ndc.xy + to_ndc.zw;
  vec3 c = vec3(ndc, 1.0);
  return vec3(dot(inv0.xyz, c), dot(inv1.xyz, c), dot(inv2.xyz, c));
}
)";
// D214: the clouds' textures in the fog's pass (the clouds' shadows on the ground).
constexpr const char* kFogClouds = R"(
layout(set = 0, binding = 8) uniform sampler3D shapeTex;
layout(set = 0, binding = 9) uniform sampler3D detailTex;
)";
constexpr const char* kFogPS = R"(
float ign(vec2 px) { return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))); }
// The haze's optical depth: a density a at the sea thinning by exp(-h / H), from the height h0 along dy for L metres.
float haze_depth(float a, float H, float h0, float dy, float L) {
  float x = dy / H * L;
  float f = abs(x) < 1e-3 ? 1.0 : (1.0 - exp(-x)) / x;
  return a * exp(-max(h0, 0.0) / H) * L * f;
}
// The drop that lands in the 32 cm square of the ground around xz now: xy where (metres), z how far through its
// second or so it is (0 to 1), w whether there is one (the more rain, the more squares have one).
vec4 Drop(vec2 xz, float amount, float tsec) {
  vec2 id = floor(xz / 0.32);
  float h = h21(id), hh = h21(id + 19.7);
  float clock = tsec * (1.1 + 0.8 * hh) + h * 7.0;
  float turn = floor(clock);
  vec2 centre = id + 0.3 + 0.4 * vec2(h21(id + turn), h21(id - turn));
  return vec4(centre * 0.32, fract(clock), step(1.0 - amount * 0.6, h21(id + turn * 3.1)));
}
void main() {
  float depth = texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r;
  vec3 r = ViewRay(gl_FragCoord.xy);
  vec3 r_down = dFdy(r);
  float rl = length(r);
  vec3 rd = r / rl;
  bool nothing = depth <= 1e-6;  // the sky
  float L = nothing ? 60000.0 : fog_b.x / depth * rl;
  // What is seen there, and which way it faces (from its neighbours in the picture).
  vec3 P = eye_world.xyz + r * (fog_b.x / max(depth, 1e-7));
  vec3 facing = normalize(cross(dFdx(P), dFdy(P)));
  if (dot(facing, r) > 0.0) facing = -facing;  // toward the eye
  float seen = L;
  float sea = eye_world.w;
  float eye_h = eye_world.y - sea;
  int debug = int(view.w + 0.5);
  if (debug == 4) {  // the distance: red every 10 m, green every 100 m, blue every 1000 m
    out_color = vec4(nothing ? vec3(0.0) : vec3(fract(L / 10.0), fract(L / 100.0), fract(L / 1000.0)) * view.z, 0.0);
    return;
  }
  if (debug == 5) {  // the height over the sea: red every 1 m, green every 10 m; blue under it
    float h = eye_h + rd.y * L;
    out_color = vec4(nothing ? vec3(0.0) : vec3(fract(h), fract(h / 10.0), h < -0.5 ? 1.0 : 0.0) * view.z, 0.0);
    return;
  }
  if (debug == 6) {  // the rain's cover: red where what is seen is under something, green where it is open
    float under = Covered(P + vec3(0.0, 0.3, 0.0));
    out_color = vec4(nothing ? vec3(0.0) : vec3(under, 1.0 - under, 0.0) * 0.3 * view.z, nothing ? 1.0 : 0.5);
    return;
  }
  // Not under the sea (a ray that finds nothing below the horizon ends at it).
  if (rd.y < -1e-5 && eye_h > 0.0) L = min(L, eye_h / -rd.y);
  float night_amount = sun_color.w;
  float dither = ign(gl_FragCoord.xy);

  // The distance fog.
  float tau = fog_a.x > 0.0 ? haze_depth(fog_a.x, fog_a.y, eye_h, rd.y, L) : 0.0;

  // The ground fog: a layer from the sea up to its top, in patches that drift; marched for 2.5 km, its mean beyond.
  // D194: in a full storm (gale.x) the mist the storm kicks up: the patches filled in, drifting faster, and the layer
  // from the ground under what is seen, not the sea (2 m under it: so over the island too, not only the shore).
  float tau_low = 0.0;
  float top = fog_a.w;
  if (fog_a.z > 0.0 && fog_b.z > 0.5) {
    float floor_h = nothing ? 0.0 : max(eye_h + rd.y * L - 2.0, 0.0) * gale.x;
    float eh = eye_h - floor_h;  // the eye over the layer's floor
    float ta = 0.0, tb = L;
    if (abs(rd.y) > 1e-5) {
      float t0 = -eh / rd.y, t1 = (top - eh) / rd.y;
      ta = max(min(t0, t1), 0.0);
      tb = min(max(t0, t1), L);
    } else if (eh < 0.0 || eh > top) {
      tb = -1.0;
    }
    const float reach = 2500.0;
    const int n = 16;
    float tm = min(tb, reach);
    if (tm > ta) {
      float dt = (tm - ta) / float(n);
      float t = ta + dt * dither;
      vec2 wind = drift.xy * (0.15 + 0.45 * gale.x);
      for (int i = 0; i < n; ++i) {
        float h = eh + rd.y * t;
        vec2 xz = eye.xz + rd.xz * t + wind;
        float patches = smoothstep(0.32, 0.68, texture(weatherTex, xz / 900.0).r);
        patches = mix(patches, 0.55 + 0.45 * patches, gale.x);
        float up = exp(-max(h, 0.0) / (top * 0.3)) * (1.0 - smoothstep(top * 0.6, top, h));
        // Thinner right at the eye, so that what is held in the hand stays clear.
        tau_low += fog_a.z * patches * up * dt * smoothstep(0.0, 6.0, t);
        t += dt;
      }
    }
    if (tb > reach && tb > ta) {
      float from = max(ta, reach);
      float h_mid = clamp(eh + rd.y * 0.5 * (from + tb), 0.0, top);
      tau_low += fog_a.z * 0.5 * exp(-h_mid / (top * 0.3)) * (tb - from);
    }
  }

  // The rain shafts: under the thicker cloud (the clouds' own map), from the cloud base down, slanted by the wind.
  float tau_rain = 0.0;
  if (fog_b.y > 0.0) {
    float base = cloud.z;
    float t_end = min(L, 16000.0);
    if (rd.y > 1e-4) t_end = min(t_end, (base + 200.0 - eye_h) / rd.y);
    const float t_start = 500.0;
    if (t_end > t_start && eye_h < base) {
      const int n = 20;
      float dt = (t_end - t_start) / float(n);
      float t = t_start + dt * dither;
      float edge = mix(0.74, 0.44, fog_b.y);
      vec2 along = normalize(vec2(6.0, 3.0));
      for (int i = 0; i < n; ++i) {
        float a = eye_h + rd.y * t;
        vec2 wp = eye.xz + rd.xz * t + drift.xy - along * (base - a) * (0.1 + rain.x);
        float cell = smoothstep(edge, edge + 0.1, texture(weatherTex, wp / 40000.0).r);
        float streak = 0.5 + 0.5 * texture(weatherTex, wp / 1700.0).r;
        float under = 1.0 - smoothstep(base - 350.0, base + 150.0, a);
        tau_rain += 0.0007 * fog_b.y * cell * streak * under * dt * smoothstep(t_start, t_start + 700.0, t);
        t += dt;
      }
    }
  }

  // The fog's own light: the horizon's haze, the sun's glow through it, and the lightning's.
  float sun_cos = dot(rd, normalize(sun_dir.xyz));
  vec3 haze = fog_color.rgb + sun_color.rgb * sun_dir.w * (1.0 - night_amount) * 0.3 * pow(max(sun_cos, 0.0), 8.0) +
              moon_light.rgb * 2.0;
  vec3 low = mix(haze, vec3(dot(haze, vec3(0.333))) * 1.08, 0.5);
  vec3 rain_far = fog_color.rgb * 0.3;  // (named rain until 17 Nov 2026, it hid the constants' rain below: the streaks leant by a tenth or so of the fog's colour, whatever the wind)
  // (The rain's streaks take the haze's light as it is without the lightning: lit by a flash's quick strokes the
  // falling rain was seen in jumps, as under a strobe, "Rainfall still resets/pulls-up when lightning flashes",
  // 14 Nov 2026.)
  vec3 calm = haze;
  if (flash.w > 0.0) {
    vec3 fd = normalize(flash.xyz - vec3(eye.x, eye_h, eye.z));
    vec3 lit = vec3(0.72, 0.8, 1.0) * flash.w * (0.05 + 0.4 * pow(max(dot(rd, fd), 0.0), 8.0));
    // (Not the far rain shafts: dark faint curtains from the cloud base to the ground, lit they stood out all at
    // once as bright columns reaching up into the cloud, which may be what read as the rain "pulling-up when
    // lightning flashes", 14 Nov 2026: neither the rain's clock nor the antialiasing explains it.)
    haze += lit, low += lit;
  }
  // The rain here ("Then rain", 14 Nov 2026). Streaks on five upright cylinders around the eye, 3 m to 31 m out,
  // each hidden where the view's depth is nearer: columns around the cylinder, in each a drop every metre or two
  // falling at 8 to 11 m/s, leaning by the wind's setting. (At first from 1.6 m, with drops 45 cm long every few
  // metres: few, long, bright lines that crossed each other, "The rain streaks ... look bad".) None under cover. What is rained on is a little darker (wet). Where a drop lands on what lies
  // flat (the ground, a deck, the sea; not the sides of things: "they look a bit weird on the sides of objects",
  // 14 Nov 2026), within 45 m a ring spreads, and within 18 m the drop kicks up a small splash ("we should also had
  // rain kick up"): a few centimetres high for the first third of the drop's second, found from the pixels under this
  // one in the picture (the ground there, and whether a drop has just landed on it).
  vec3 rain_light = vec3(0.0);
  float wet = 1.0, streaks = 0.0;
  if (fog_b.w > 0.0) {
    float amount = fog_b.w, tsec = eye.w;
    // (With most of the lightning's light again, now that a flash is one smooth swell: unlit, the streaks went dark
    // against the lit scene for the flash's half second and came back after it, as if the rain stopped and began
    // again: runs fl0 and fl1.)
    vec3 streak = mix(calm, haze, 0.7) * 0.6 + vec3(0.015);
    // The cylinders' axis is the way the rain falls (straight down, leant by the wind), so every streak runs along
    // it: they lean together and draw toward one point, as rain does. (Upright cylinders with a lean on each made
    // streaks that crossed: run cov2.)
    vec2 wind_way = normalize(vec2(6.0, 3.0)) * rain.x;
    vec3 axis = normalize(vec3(-wind_way.x, 1.0, -wind_way.y));
    vec3 e1 = normalize(cross(axis, vec3(0.0, 0.0, 1.0)));
    vec3 e2 = cross(axis, e1);
    float along_ray = dot(rd, axis);
    vec3 off = rd - along_ray * axis;
    float flat_len = length(off);
    float ang = atan(dot(off, e1), dot(off, e2));
    float eye_along = dot(eye_world.xyz, axis);
    float sum = 0.0;
    // A downpour (the user, 17 Nov 2026: "make the rain itself more visible/dense at 100% weather"): from 75% of the
    // weather up, more drops in each column, longer and brighter, and a sixth cylinder further out.
    float heavy = smoothstep(0.75, 1.0, amount);
    for (int i = 0; i < 6; ++i) {
      float fi = float(i);
      if (i == 5 && heavy < 0.01) break;
      float R = 3.0 * pow(1.8, fi);
      float t = R / max(flat_len, 0.05);
      float y = eye_along + along_ray * t;
      float per_m = 14.0 / (1.0 + 0.7 * fi) * (1.0 + 0.8 * heavy);  // (more columns in a downpour)
      float u = ang * R * per_m + fi * 17.3;
      float wpx = max(fwidth(u), 1e-4);
      if (t > seen || flat_len < 0.05) continue;
      if (Covered(eye_world.xyz + rd * t) > 0.5) continue;
      float col = floor(u);
      float h1 = h21(vec2(col, fi * 7.0 + 1.0)), h2 = h21(vec2(col, fi * 7.0 + 2.0)), h3 = h21(vec2(col, fi * 7.0 + 3.0));
      float period = (1.2 + 1.3 * h2) * (1.0 - 0.35 * heavy);
      float v = (y + tsec * (8.0 + 3.0 * h1)) / period + h3 * 10.0;
      float on = step(1.0 - amount * 0.85 - 0.13 * heavy, h21(vec2(col + floor(v) * 13.0, fi + 40.0)));
      if (i == 5) on *= heavy;
      float f = fract(v);
      float len = 0.3 / period * (1.0 + 0.15 * fi) * (1.0 + 0.45 * heavy);
      float along = smoothstep(0.0, len * 0.2, f) * (1.0 - smoothstep(len * 0.6, len, f));
      float half_w = 0.002 * per_m;  // half as thick as at first ("make the world rain itself about 50% thinner")
      float across = 1.0 - smoothstep(0.0, max(half_w, wpx), abs(fract(u) - 0.5 - (h1 - 0.5) * 0.6));
      across *= clamp(half_w / wpx, 0.15, 1.0);
      sum += on * along * across * 0.55 / (1.0 + 0.3 * fi);
    }
    // A streak brightens what is behind it by a share (the pass's alpha, over 1) more than it adds a light of its
    // own, so it shows as well against a bright sky as against a dark one. (Only adding, the streaks were lost
    // against the lightning's glow, which is strongest high up: as a flash faded they came back from the bottom of
    // the view upward, "Rainfall still looks like it's being pulled-up when lightning flashes", 14 Nov 2026.)
    // (Shown nearly up to where the eye looks along the rain: looking up, the streaks fan out from that point as
    // rain coming at the eye does. Hidden from further off, a look up into a storm showed no rain at all.)
    // D195: thinner still within about 30 degrees of that point (the user, 18 Nov 2026: looking "toward the direction
    // rain is coming from ... looks like the player is in a circular tunnel": the cylinders' streaks lined up in rings).
    streaks = min(sum, 1.0) * smoothstep(0.05, 0.16, flat_len) * mix(0.25, 1.0, smoothstep(0.16, 0.5, flat_len));
    rain_light = streak * streaks * (0.45 + 0.45 * heavy);
    // (Not what faces down, a roof's or a porch's underside: 30 cm over a thin roof's underside is out in the rain
    // above it, and rings, splashes and the wet showed on the inside of the roofs: the user, 17 Nov 2026.)
    float open = facing.y < -0.5 ? 0.0 : 1.0 - Covered(P + vec3(0.0, 0.3, 0.0));
    if (!nothing && seen > 1.2) {
      wet = 1.0 - 0.12 * amount * open;
      if (seen < 45.0 && facing.y > 0.96 && open > 0.5 && P.y < eye_world.y - 0.05) {
        vec4 drop = Drop(P.xz, amount, tsec);
        float rr = length(P.xz - drop.xy);
        float ring = (1.0 - smoothstep(0.0, 0.012, abs(rr - drop.z * 0.13))) * (1.0 - drop.z) * (1.0 - drop.z);
        rain_light += streak * ring * drop.w * 0.6 * (1.0 - smoothstep(25.0, 45.0, seen));
      }
    }
    // The kick-up. Toward the ground in the picture: the way the rays go down.
    if (seen > 1.2) {
      float dir = r_down.y < 0.0 ? 1.0 : -1.0;
      float ray_px = length(r_down);
      // How far off the ground under this pixel is (8 pixels down, of a 1440 pixel view), for the splash's size here.
      float scale = view.y / 1440.0;
      float d0 = texelFetch(depthTex, ivec2(gl_FragCoord.xy + vec2(0.0, dir * 8.0 * scale)), 0).r;
      float w0 = fog_b.x / max(d0, 1e-7);
      if (d0 > 1e-6 && w0 < 18.0) {
        float step_px = clamp(0.08 / (ray_px * w0) / 8.0, 1.0, 10.0);
        float kick = 0.0;
        vec3 before = vec3(0.0);
        for (int k = 1; k <= 9; ++k) {
          vec2 q = gl_FragCoord.xy + vec2(0.0, dir * float(k) * step_px);
          if (q.y < 0.0 || q.y >= view.y) break;
          float dq = texelFetch(depthTex, ivec2(q), 0).r;
          if (dq <= 1e-6) continue;
          float wq = fog_b.x / dq;
          vec3 rq = ViewRay(q);
          vec3 G = eye_world.xyz + rq * wq;
          vec3 was = before;
          before = G;
          if (k == 1) continue;
          float away = wq * length(rq);
          // Flat ground only (it hardly rises from one place to the next), not behind what this pixel shows, and
          // under the eye: what lies flat and is seen from beneath (a ceiling, a roof's underside) faces down.
          if (away > 18.0 || away > seen + 0.05 || abs(G.y - was.y) > 0.5 * length(G.xz - was.xz) + 0.003 ||
              G.y > eye_world.y - 0.05)
            continue;
          vec4 drop = Drop(G.xz, amount, tsec);
          if (drop.w < 0.5 || drop.z > 0.3 || Covered(G + vec3(0.0, 0.3, 0.0)) > 0.5) continue;
          float off = length(G.xz - drop.xy);
          float high = float(k) * step_px * ray_px * wq;
          float top = 0.075 * sin(3.14159 * drop.z / 0.3);
          if (off < 0.03 && high < top) kick = max(kick, (1.0 - off / 0.03) * (1.0 - 0.6 * high / max(top, 1e-4)));
        }
        rain_light += streak * kick * 0.8;
      }
    }
  }
  // D190: the sea's reflection of the renderer's own (the user, 17 Nov 2026: "turn those reflections off and use our own
  // SSR on them and the water"; the game's own is a day sky's cube map, which the draw path leaves out of the sea's
  // shaders while this is on). The sea is the plane at its height (Sky::ReadSea): a pixel whose ray meets it before
  // what the depth holds (or at it) shows the sea. Its facing: up, rippled by a few small waves (more with the wind,
  // calmer far off). The reflected ray is marched through the view's depth (40 steps growing from about half a metre);
  // where it passes behind what is drawn, that is what the sea shows. Where it leaves the view or meets only the sky:
  // the sky drawn in its direction if that is in the view, else the sky's own palette that way. As much as Fresnel
  // gives (2% straight down to all of it at a grazing look), taken from the game's own colour of the water.
  vec3 sea_refl = vec3(0.0);
  float sea_f = 0.0;
  vec3 dbg_sky = vec3(0.0), dbg_own = vec3(0.0);  // D204 debug 7 to 9: the sky mirrored, where from, the water's own
  float dbg_src = 0.0;
  vec3 dbg_add = vec3(0.0);
  float dbg_self = 0.0, dbg_f0 = 0.0, dbg_got = 0.0;
  if (ssr.x > 0.0 && fog_b.z > 0.5 && rd.y < -1e-4 && eye_h > 0.05) {
    float ts = eye_h / -rd.y;
    // (Where the sea's draws are, when they could be drawn again (ssr.z); flat ground at the sea's height is not it.
    // With them, the sea is wherever they are, its waves too: by the plane alone the crests near the eye, above it,
    // were left out (the user, 17 Nov 2026: "isn't being used at near-field").)
    bool masked = mod(ssr.z, 10.0) > 0.5;
    bool sea_here = masked ? !nothing && textureLod(seaMaskTex, gl_FragCoord.xy / view.xy * ssr.w, 0.0).r > 0.0
                           : nothing || ts <= L + 0.2 + 0.002 * L;
    if (sea_here) {
      if (masked) ts = fog_b.x / depth * rl;  // the surface drawn, its waves
      vec3 S = eye_world.xyz + rd * ts;
      float tsec = eye.w;
      float amp = (0.05 + 0.14 * ssr.y) / (1.0 + ts / 150.0);
      // (Waves alone, five of them, lay in even bands across the middle distance: run ssr_g1. Their phases are now
      // shifted by a drifting noise, and two octaves of the noise's own slope are added.)
      vec2 g = vec2(0.0);
      vec2 warp = vec2(vnoise(S.xz * 0.045 + tsec * 0.02), vnoise(S.xz * 0.045 + 7.3 - tsec * 0.017)) * 6.0;
      for (int k = 0; k < 5; ++k) {
        float fk = float(k);
        float a = fk * 2.399 + 0.6;
        vec2 dir = vec2(cos(a), sin(a));
        float freq = 0.55 * pow(1.7, fk);
        float ph = dot(dir, S.xz) * freq - tsec * sqrt(9.81 * freq) * 0.6 + fk * 1.7 + warp.x * (0.5 + 0.3 * fk) + warp.y;
        g += dir * (cos(ph) / (1.0 + fk * 0.6));
      }
      for (int k = 0; k < 2; ++k) {
        float sc = k == 0 ? 0.35 : 1.1;
        vec2 q = S.xz * sc + vec2(tsec * 0.13, -tsec * 0.09) * (1.0 + float(k));
        const float e = 0.15;
        float n0 = vnoise(q);
        g += vec2(vnoise(q + vec2(e, 0.0)) - n0, vnoise(q + vec2(0.0, e)) - n0) / e * (k == 0 ? 0.9 : 0.5);
      }
      // (Ripples tilted the reflection so far that near the eye it broke up into bright smears of the beach and the
      // hills, run sea_fix_a: their slope is held to 0.08 near, more freedom far off where a pixel covers more water.)
      vec2 slope = g * amp;
      float most = 0.08 + 0.1 * smoothstep(20.0, 200.0, ts);
      slope *= min(1.0, most / max(length(slope), 1e-5));
      vec3 N = normalize(vec3(-slope.x, 1.0, -slope.y));
      vec3 R = reflect(rd, N);
      R.y = max(R.y, 0.01);
      R = normalize(R);
      float cosv = clamp(dot(-rd, N), 0.0, 1.0);
      // D204: far off, how much is reflected from the sea's mean surface, flat (a pixel there covers many ripples;
      // they still turn the reflected ray above). Taken from the rippled facing alone, it stayed about half at a
      // grazing look to the horizon, and the other half was the game's own water, which with our sky's small cube in
      // it is a clipped pale cyan there (the user, 19 Nov 2026: "with SSR on the water, the horizon looks awful and
      // blindingly bright blue at the horizon line"; run sea_dbg7: the sky mirrored 166 167 182, the strength 0.47,
      // the game's own water 201 254 254).
      cosv = mix(cosv, clamp(-rd.y, 0.0, 1.0), smoothstep(120.0, 600.0, ts));
      sea_f = 0.02 + 0.98 * pow(1.0 - cosv, 5.0);
      dbg_f0 = sea_f;
      // The view's forward: from the world's offset from the eye to clip x, y and w (the inverse of inv0 to inv2).
      mat3 to_clip = inverse(transpose(mat3(inv0.xyz, inv1.xyz, inv2.xyz)));
      vec3 found = vec3(0.0);
      float got = 0.0;
      vec3 X = S + R * 0.05;
      float step_len = 0.4 + ts * 0.004;
      for (int i = 0; i < 40; ++i) {
        X += R * step_len;
        vec3 c = to_clip * (X - eye_world.xyz);
        if (c.z <= 0.05) break;
        vec2 uv = (c.xy / c.z - to_ndc.zw) / to_ndc.xy;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;
        float dq = texelFetch(depthTex, ivec2(uv * view.xy), 0).r;
        float wq = dq > 1e-6 ? fog_b.x / dq : 1e9;
        if (c.z > wq && c.z - wq < max(1.0, step_len * 1.5) && wq > c.z * 0.85) {  // (not far nearer: the tower's near legs mirrored in the far sea, run glass_dbg)
          // Where between the last step and this one it went behind (halved five times): the place itself, not up to
          // a step past it (a step's error far out drew the shore as streaks).
          vec3 lo = X - R * step_len, hi = X;
          for (int b = 0; b < 5; ++b) {
            vec3 m = 0.5 * (lo + hi);
            vec3 cm = to_clip * (m - eye_world.xyz);
            vec2 um = (cm.xy / cm.z - to_ndc.zw) / to_ndc.xy;
            float dm = texelFetch(depthTex, ivec2(clamp(um, vec2(0.0), vec2(1.0)) * (view.xy - 1.0)), 0).r;
            if (dm > 1e-6 && cm.z > fog_b.x / dm) hi = m; else lo = m;
          }
          vec3 ch = to_clip * (hi - eye_world.xyz);
          uv = clamp((ch.xy / ch.z - to_ndc.zw) / to_ndc.xy, vec2(0.0), vec2(1.0));
          // D204: not the sea itself. At a grazing look the ray climbs a centimetre a metre and went behind the game's
          // own waves further out: the far water mirrored its own pale cyan at the horizon (run sea_dbg8: what was
          // added there 208 254 254, the crests a quarter of it). What it meets within a metre of the sea, or on the
          // sea's own draws, is passed and the march goes on.
          if (hi.y - sea < 1.0 || (masked && textureLod(seaMaskTex, uv * ssr.w, 0.0).r > 0.0)) {
            dbg_self = 1.0;
            step_len *= 1.13;
            continue;
          }
          vec2 edge = min(uv, 1.0 - uv);
          got = smoothstep(0.0, 0.08, min(edge.x, edge.y));
          found = textureLod(sceneTex, uv, 0.0).rgb;
          break;
        }
        step_len *= 1.13;
      }
      // The sky that way: as drawn in the view, that way or the nearest sky above it in the picture (the sky the
      // renderer draws, its clouds and its overcast); only where the view holds no sky there, its palette, greyed by
      // the clouds' cover. (The palette alone is a clear sky's: under an overcast the sea shone blue as if the game's
      // own sky were in it, the user, 17 Nov 2026: "isn't taking into account our sky".)
      vec3 sky_way = mix(horizon_color.rgb, zenith_color.rgb, sqrt(clamp(R.y, 0.0, 1.0)));
      sky_way = mix(sky_way, vec3(dot(sky_way, vec3(0.3, 0.55, 0.15))) * 0.9, smoothstep(0.2, 0.8, cloud.x)) * view.z;
      vec3 cd = to_clip * R;
      if (cd.z > 1e-4) {
        vec2 uv = (cd.xy / cd.z - to_ndc.zw) / to_ndc.xy;
        vec3 cu = to_clip * normalize(R + vec3(0.0, 0.25, 0.0));
        vec2 up = cu.z > 1e-4 ? (cu.xy / cu.z - to_ndc.zw) / to_ndc.xy - uv : vec2(0.0, -1.0);
        up = length(up) > 1e-5 ? normalize(up) : vec2(0.0, -1.0);
        vec2 q = clamp(uv, vec2(0.0), vec2(1.0));
        for (int k = 0; k < 24; ++k) {
          // (Above the horizon only: the far sea past the game's draw distance holds no depth either, and taken for
          // the sky it mirrored its own pale far water at the horizon, the user, 18 Nov 2026: "Water near the horizon
          // is really pale blue during the day".)
          if (texelFetch(depthTex, ivec2(q * (view.xy - 1.0)), 0).r <= 1e-6 && normalize(ViewRay(q * view.xy)).y > 0.002) {
            sky_way = textureLod(sceneTex, q, 0.0).rgb;
            dbg_src = 1.0;
            break;
          }
          q += up * 0.025;
          if (any(lessThan(q, vec2(0.0))) || any(greaterThan(q, vec2(1.0)))) break;
        }
      }
      sea_refl = mix(sky_way, found, got);
      dbg_sky = sky_way;
      dbg_got = got;
      dbg_own = texelFetch(sceneTex, ivec2(gl_FragCoord.xy), 0).rgb;
      sea_f *= ssr.x;
      // Not over the foam the sea's own shader draws where it washes up on the shore (the user, 17 Nov 2026: "New
      // water reflections are now hiding the shoreline wash animations"): in the shallows (under 0.6 m of water, from
      // the world's depth without the sea), and where the water's colour is near white.
      {
        float fz = texelFetch(seaFloorTex, ivec2(gl_FragCoord.xy), 0).r;
        if (fz > 1e-6) {
          float under = (fog_b.x / fz * rl - ts) * -rd.y;  // the water's depth straight down
          sea_f *= smoothstep(0.1, 0.6, under);  // (to 1.2 m a sandbar out at sea showed as a dark ring: run shore_on5)
        }
        vec3 own = texelFetch(sceneTex, ivec2(gl_FragCoord.xy), 0).rgb;
        float most = max(max(own.r, own.g), own.b), least = min(min(own.r, own.g), own.b);
        float foam = smoothstep(0.7, 0.88, least / max(most, 1e-4)) * smoothstep(0.25, 0.45, most);
        // D204: near only (the far water's pale cyan was taken for foam and halved the reflection at the horizon; far
        // shores are left to the shallows above).
        foam *= 1.0 - smoothstep(120.0, 600.0, ts);
        sea_f *= 1.0 - 0.9 * foam;
        // The foam and the crests the game draws on its waves out at sea kept through the reflection (the user, 18
        // Nov 2026: "the SSR hides the seafoam animation that's on the original water waves"): what of the water is
        // brighter than the water round it (four places 6 pixels off, of a 1440 view) is added back over it.
        float o = 6.0 * view.y / 1440.0;
        vec2 at0 = gl_FragCoord.xy, last_px = view.xy - 1.0;
        // D204: a place above the horizon is sky, not water round it (its darker sky made the horizon's row of water
        // "brighter than round it"): the pixel's own colour stands in for it.
        vec3 round_it = vec3(0.0);
        for (int k = 0; k < 4; ++k) {
          vec2 d = k == 0 ? vec2(o, 0.0) : k == 1 ? vec2(-o, 0.0) : k == 2 ? vec2(0.0, o) : vec2(0.0, -o);
          vec2 p = clamp(at0 + d, vec2(0.0), last_px);
          round_it += normalize(ViewRay(p)).y < -1e-4 ? texelFetch(sceneTex, ivec2(p), 0).rgb : own;
        }
        round_it *= 0.25;
        sea_refl += max(own - round_it, vec3(0.0)) * 1.3;
        dbg_add = max(own - round_it, vec3(0.0)) * 1.3;
      }
      // Not over what see-through was drawn over the sea after it (a flag, smoke, spray: the user, 17 Nov 2026: "Flags
      // also now have a water shader on them"): where the picture is no longer what the sea left.
      if (mod(ssr.z, 10.0) > 1.5) {
        vec3 now = texelFetch(sceneTex, ivec2(gl_FragCoord.xy), 0).rgb;
        vec3 then = texelFetch(seaAfterTex, ivec2(gl_FragCoord.xy), 0).rgb;
        float differs = length(now - then) / (0.03 + max(max(then.r, then.g), then.b));
        float kept = 1.0 - smoothstep(0.03, 0.12, differs);
        // NXR_SEA_DEBUG=1: where it is left out (something drawn over the sea after it) in red.
        if (ssr.z > 9.5 && kept < 0.5) { sea_refl = vec3(1.0, 0.0, 0.0) / max(view.z, 1e-3); sea_f = 1.0; kept = 1.0; }
        sea_f *= kept;
      }
    }
  }
  float t_fog = exp(-tau), t_low = exp(-tau_low), t_rain = exp(-tau_rain);
  // The low layer nearest, the haze behind it, the far rain behind both.
  vec3 light = low * (1.0 - t_low) + t_low * (haze * (1.0 - t_fog) + t_fog * rain_far * (1.0 - t_rain));
  float shows = t_low * t_fog * t_rain;
  // D192: the bolt (Sky::FillBolt lays its pieces out in the view's pixels): a white-hot core in a blue glow (Beachhead's
  // bolt shader), hidden where the view's depth is nearer, seen through the haze between.
  vec3 bolt_light = vec3(0.0);
  if (bolt.x > 0.5 && all(greaterThanEqual(gl_FragCoord.xy, bolt_box.xy)) && all(lessThanEqual(gl_FragCoord.xy, bolt_box.zw))) {
    float scene_w = nothing ? 1e9 : fog_b.x / depth;
    float core = 0.0, glow = 0.0, at_w = 0.0;
    int bn = int(bolt.x + 0.5);
    for (int i = 0; i < bn; ++i) {
      vec4 bs = bolt_seg[2 * i], bm = bolt_seg[2 * i + 1];
      vec2 ab = bs.zw - bs.xy, ap = gl_FragCoord.xy - bs.xy;
      float bt = clamp(dot(ap, ab) / max(dot(ab, ab), 1e-4), 0.0, 1.0);
      float bw = mix(bm.x, bm.y, bt);
      if (bw > scene_w) continue;
      float bd = length(ap - ab * bt);
      float hw = bm.z * bolt.z / bw;
      // At least about a pixel and a half wide, dimmer for it (far off a bolt is thinner than a pixel).
      float hc = max(hw, 0.75);
      float c = (1.0 - smoothstep(hc * 0.35, hc, bd)) * bm.w * max(hw / hc, 0.4);
      float hg = max(hw * 2.6, 2.0);
      float g = exp(-bd * bd / (hg * hg)) * 0.3 * bm.w;
      if (c + g > core + glow) at_w = bw;
      core = max(core, c);
      glow = max(glow, g);
    }
    if (core + glow > 0.0) {
      float tb = fog_a.x > 0.0 ? exp(-haze_depth(fog_a.x, fog_a.y, eye_h, rd.y, at_w * rl)) : 1.0;
      vec3 tint = vec3(0.62, 0.68, 1.0);
      bolt_light = (mix(tint, vec3(1.0), smoothstep(0.35, 0.9, core)) * core * 6.0 + tint * glow * 1.5) * bolt.y * tb * t_low;
    }
  }
  // D204 (native_x_renderer_sky_debug): 7 the sky the sea mirrors, 8 its strength (red: the sky's palette, green: the
  // picture's sky; blue: what the march found), 9 the water's own colour under the reflection, 10 the reflection as
  // added (sea_refl * sea_f * shows), 11 the crests added back, 12 the strength: red by Fresnel alone, green as
  // drawn (after the foam and what was drawn over the sea), blue where the march passed the sea itself.
  if (debug >= 7 && debug <= 12) {
    vec3 c = debug == 7 ? dbg_sky : debug == 8 ? vec3(sea_f * (1.0 - dbg_src), sea_f * dbg_src, dbg_got)
           : debug == 9 ? dbg_own : debug == 10 ? sea_refl * sea_f * shows : debug == 11 ? dbg_add
           : vec3(dbg_f0, sea_f, dbg_self);
    out_color = vec4(c, 0.0);
    return;
  }
  // D214: the clouds' shadows on the ground (the user, 19 Nov 2026: "Can we get clouds to cast shadows onto the map
  // from the direction of the sun?"): from the point seen toward the sun, the clouds' own density (their shared code,
  // the cheap kind) at a quarter and three quarters of the layer's depth; what light gets through takes the direct
  // sun's share off what shows of the scene (three quarters at the most: the sky's light stays). By day, the sun over
  // 3 degrees; nothing for the sky.
  float cloud_shade = 1.0;
  if (!nothing && layers.z > 0.5 && sun_dir.w > 0.0 && sun_color.w < 0.5) {
    vec3 ls = normalize(sun_dir.xyz);
    if (ls.y > 0.05) {
      float ws = max(misc.z, 1e-3);
      vec3 q = (eye.xyz + rd * L) / ws;
      float od = 0.0;
      for (int k = 0; k < 2; k++) {
        float h = cloud.z + cloud.w * (k == 0 ? 0.25 : 0.75);
        float t = (h - q.y) / ls.y;
        if (t > 0.0) od += cloud_density(q + ls * t, q + ls * t, true);
      }
      // (D215: darker, the user: "Let's make cloud shadow darker than that please", "Darker still", then "That's a
      // little too dark": 6.0 and 0.91, between 4.5 and 0.88 (the ground 21% darker) and 7.5 and 0.93 (34%);
      // D214's 2.2 and 0.75 (9%). Then "A bit darker": 6.75 and 0.92, about 31%.)
      float through = exp(-od * 6.75);
      // Under the weather's overcast there is little direct sun left to take (the sun's light 75% down at full
      // weather, Fill): the shadows' most falls to 15% of it as the weather here closes in (D217: under a 100% storm
      // the tank and the ground were left nearly black, runs vwet1 and vwet2).
      float direct = 1.0 - 0.85 * smoothstep(0.3, 0.9, layers.w);
      cloud_shade = mix(1.0, through, 0.92 * direct * smoothstep(0.05, 0.12, ls.y));
    }
  }
  out_color = vec4((light + rain_light + bolt_light) * view.z + sea_refl * sea_f * shows,
                   shows * wet * (1.0 + (0.45 + 0.35 * smoothstep(0.75, 1.0, fog_b.w)) * streaks) * (1.0 - sea_f) * cloud_shade);
}
)";
// The view's lowest heights, for the sea's (Sky::ReadSea): each pixel of the small target the lowest of 4 x 4 places of
// its part of the view, 6 m to 150 m away (not what is held in the hand; further away the heights found drift by
// metres: run fog5).
constexpr const char* kSeaPS = R"(
void main() {
  vec2 cell = view.xy / vec2(32.0, 18.0);
  float lowest = 1e9;
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) {
      vec2 px = (floor(gl_FragCoord.xy) + (vec2(float(x), float(y)) + 0.5) / 4.0) * cell;
      float depth = texelFetch(depthTex, ivec2(px), 0).r;
      if (depth <= 1e-6) continue;
      vec3 r = ViewRay(px);
      float w = fog_b.x / depth;
      float dist = w * length(r);
      if (dist < 6.0 || dist > 150.0) continue;
      lowest = min(lowest, eye_world.y + r.y * w);
    }
  // The last pixel tells whether the eye is under cover (3e9) or not (2e9), for the rain on the lens and the wetness.
  // (Five places, the eye's and four 1.2 m around it: under cover when most are. One place alone took a davit's
  // arm over a carrier's deck for a roof, and nothing got wet there: run kitJ3.)
  if (ivec2(gl_FragCoord.xy) == ivec2(31, 17)) {
    vec3 at = eye_world.xyz + vec3(0.0, 0.5, 0.0);
    float under = Covered(at) + Covered(at + vec3(1.2, 0.0, 0.0)) + Covered(at - vec3(1.2, 0.0, 0.0)) +
                  Covered(at + vec3(0.0, 0.0, 1.2)) + Covered(at - vec3(0.0, 0.0, 1.2));
    lowest = under > 3.5 ? 3e9 : 2e9;
  }
  out_color = vec4(lowest, 0.0, 0.0, 1.0);
}
)";

// Rain on the lens (Sky::Lens). Two sizes of beads, each in a square of its own: it swells, sits for 7 to 20 seconds
// and goes; and drops that run down in columns, wavering, with a thin wet trail above them. Each bends the picture
// behind it as a small lens does (what shows in a drop is the picture around it, turned over).
constexpr const char* kLensPS = R"(
layout(set = 0, binding = 2) uniform sampler2D sceneTex;
layout(set = 0, binding = 3) uniform sampler2D glassTex;       // D191: the panes' depth (0: no glass)
layout(set = 0, binding = 4) uniform sampler2D worldDepthTex;  // the world's depth before the first person weapon
layout(set = 0, binding = 5) uniform sampler2D viewDepthTex;   // the view's depth at its end
layout(set = 0, binding = 6) uniform sampler2D reflectTex;     // the scene again, filtered
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
vec3 Beads(vec2 q, float scale, float t, float amount) {
  vec2 g = q * scale;
  vec2 id = floor(g), f = fract(g) - 0.5;
  vec2 rnd = vec2(h21(id), h21(id + 37.3));
  float life = fract(t * (0.05 + 0.09 * rnd.x) + rnd.y * 10.0);
  float pick = h21(id + 5.1);
  float show = step(1.0 - amount * 0.07, pick) * view.z;  // few: "it's a bit too busy" (14 Nov 2026)
  float size = (0.1 + 0.22 * h21(id + 9.2)) * smoothstep(0.0, 0.08, life) * (1.0 - smoothstep(0.8, 1.0, life));
  vec2 d = f - (rnd - 0.5) * 0.5;
  // Not a perfect circle ("break up the perfect circles, making them more blob-like", 14 Nov 2026): each a little
  // longer one way, at its own angle, with an uneven edge.
  float turn = rnd.x * 6.2832, longer = 0.12 + 0.22 * rnd.y;
  vec2 across = vec2(cos(turn), sin(turn));
  vec2 e = d + across * dot(d, across) * longer * -1.0;
  float edge = length(e) + size * 0.3 * (vnoise(g * 4.5 + rnd * 9.0) - 0.5);
  float m = smoothstep(size, size * 0.55, edge) * show;
  return vec3(-d / max(size, 1e-3) * m, m);
}
vec3 Runners(vec2 q, float t, float amount) {
  float cx = q.x * 16.0;
  float id = floor(cx), fx = fract(cx) - 0.5;
  float r1 = h21(vec2(id, 3.0)), r2 = h21(vec2(id, 7.0));
  float pick = h21(vec2(id, 11.0));
  // (D217: more of them the harder it rains, 0.2 to 0.45 of the columns.)
  float show = max(step(1.0 - amount * (0.2 + 0.25 * amount), pick) * view.z, step(1.0 - amount * 0.2 - rain.w * 0.3, pick) * view.w);
  float clock = t * (0.06 + 0.1 * r1) * (1.0 + 2.5 * rain.w) + r2;  // splashed water runs off fast
  // It runs in fits: quick, then nearly still.
  float y = fract(clock) + 0.015 * sin(clock * 40.0);
  // A streak of water, not a tadpole ("The running droplets look like tadpoles, not wet droplets streaking",
  // 14 Nov 2026: a round head on a thin wavy tail): the head small and longer than wide, the wet trail above it as
  // wide as the head and narrowing upward as it thins, nearly straight, with a few small beads left behind in it.
  float dx = fx - 0.045 * sin(q.y * 8.0 + r2 * 20.0) * (0.5 + 0.5 * r1);
  float dy = (q.y - y * 1.2 + 0.1) * 16.0;
  vec2 d = vec2(dx, dy * 0.42);
  float head = smoothstep(0.13, 0.06, length(d));
  float up = clamp(-dy / 7.0, 0.0, 1.0);
  float width = mix(0.115, 0.035, up);  // ("make the streaks a bit wider and less dark")
  float above = step(dy, 0.0);
  float trail = smoothstep(width, width * 0.4, abs(dx)) * above * (1.0 - up) * (1.0 - up) * 0.6;
  float left = smoothstep(0.05, 0.02, length(vec2(dx, (fract(dy * 0.8 + r1) - 0.5) * 0.7))) * step(dy, -0.6) * (1.0 - up) * 0.5;
  return vec3(-d / 0.13 * head + vec2(-dx / width, 0.0) * trail, max(head, max(trail, left))) * show;
}
void main() {
  vec2 px = gl_FragCoord.xy;
  vec2 q = px / view.y;  // squares stay square
  float t = eye.w, amount = rain.z;
  vec3 a = Beads(q, 13.0, t, amount), b = Beads(q + 3.7, 27.0, t * 1.3, amount), r = Runners(q, t, amount);
  // Few of them. The beads, which sit, keep out of the middle of the view; the drops that run down may cross it, but
  // nothing covers the sights themselves ("drops can occasionally streak down the screen ... we just don't want
  // droplets obscuring the crosshair", 14 Nov 2026).
  vec2 mid = (px - 0.5 * view.xy) / view.y;
  float clear = smoothstep(0.24, 0.46, length(mid * vec2(0.7, 1.0)));
  float sights = smoothstep(0.045, 0.1, length(mid));
  // D195: in a vehicle (ssr.y) the drops are on its glass (the user, 18 Nov 2026: "For the planes, the camera droplet
  // effect should be on the first-person cockpit glass"): where what is seen is more than about 2 m off, not on the
  // cockpit's frame and instruments nearer than that (its glass writes no depth).
  if (ssr.y > 0.5) {
    vec2 c_uv = px / view.xy;
    float z = max(textureLod(worldDepthTex, c_uv * cloud.xy, 0.0).r, textureLod(viewDepthTex, c_uv * cloud.zw, 0.0).r);
    float on_glass = z > 1e-6 ? smoothstep(1.6, 2.6, 0.1 / z) : 1.0;
    clear *= on_glass, sights *= on_glass;
  }
  vec2 bend = (a.xy + b.xy * 0.5) * clear + r.xy * sights;
  float m_beads = clamp((a.z + b.z) * clear, 0.0, 1.0), m_runs = r.z * sights;
  float m = clamp(m_beads + m_runs, 0.0, 1.0);
  // A splash is not the rain's beads ("maybe we use a different shader for the splash effect?", then "A little more
  // subtle than that, with sparse blurred splotching maybe", 14 Nov 2026): a few soft patches of water on the lens,
  // the picture blurred and wavering a little inside each, sliding slowly down; they fade as it dries (view.w).
  float blot = 0.0;
  if (view.w > 0.0) {
    vec2 bq = q * 2.6 + vec2(3.1, -t * 0.04);
    blot = smoothstep(0.6, 0.82, vnoise(bq) * 0.7 + vnoise(bq * 2.3 + 5.7) * 0.3) * view.w * mix(0.3, 1.0, sights);
    bend += (vec2(vnoise(q * 14.0 + 1.7), vnoise(q * 14.0 + 9.2)) - 0.5) * 0.25 * blot;
  }
  // D189, D193: a strong storm's shake: the picture moved by drift.zw (of the view's height) and turned a little about
  // its middle by drift.y (the crosshair's aim stays where it is; the drops on the lens do not move), a touch larger
  // (by the most the shake can take, so it does not breathe) so its corners stay filled.
  vec2 from = px;
  if (drift.x > 0.0) {
    vec2 c = 0.5 * view.xy, d = px - c;
    float ca = cos(drift.y), sa = sin(drift.y);
    d = vec2(ca * d.x - sa * d.y, sa * d.x + ca * d.y) / (1.0 + drift.x * (0.0016 * view.x / view.y + 0.004));
    from = c + d + drift.zw * view.y;
  }
  vec2 at = clamp(from + bend * 0.035 * view.y, vec2(0.5), view.xy - 0.5);
  vec3 seen = texelFetch(sceneTex, ivec2(at), 0).rgb;
  if (blot > 0.01) {  // the patch's blur: eight places around, further out the more water
    float reach = 0.007 * view.y * blot;
    vec3 sum = seen;
    for (int n = 0; n < 8; ++n) {
      float ang = 0.785398 * float(n);
      vec2 p = clamp(at + vec2(cos(ang), sin(ang)) * reach * (n % 2 == 0 ? 1.0 : 0.55), vec2(0.5), view.xy - 0.5);
      sum += texelFetch(sceneTex, ivec2(p), 0).rgb;
    }
    seen = mix(seen, sum / 9.0, min(blot * 1.5, 1.0)) * (1.0 + 0.03 * blot);
  }
  // A drop on the lens ("more transparent, blurred, and reflective too", 14 Nov 2026): clear water, so the picture
  // shows through it only a little darker at the rim; out of focus, so what shows is blurred; and it catches the
  // light: a glint on its upper side and a soft brightening there from what is around it.
  float glint = clamp(bend.y, 0.0, 1.0) * m;
  float rim = 4.0 * m_beads * (1.0 - m_beads) + 0.25 * 4.0 * m_runs * (1.0 - m_runs);  // the runs' wet trail hardly darkens
  if (m > 0.01) {
    float reach = 0.0045 * view.y * m;
    vec3 sum = seen;
    for (int n = 0; n < 8; ++n) {
      float ang = 0.785398 * float(n);
      vec2 p = clamp(at + vec2(cos(ang), sin(ang)) * reach * (n % 2 == 0 ? 1.0 : 0.55), vec2(0.5), view.xy - 0.5);
      sum += texelFetch(sceneTex, ivec2(p), 0).rgb;
    }
    vec3 soft = sum / 9.0;
    seen = mix(seen, soft, min(m * 1.4, 1.0));
    seen += soft * 0.22 * glint + vec3(dot(soft, vec3(0.333))) * 0.3 * glint * glint;
  }
  // D191: the window glass's reflection of the renderer's own (the user, 17 Nov 2026: "use our own SSR on them"): where
  // a pane is nearer than the world's opaque depth and nothing nearer covers it (the weapon), its place from its depth
  // and its facing from its neighbours'; the reflected ray marched through the world's depth as the sea's (D190), else
  // the sky drawn that way, else the sky's palette. Fresnel from 4%, the most it takes 85%. (cloud: the depths'
  // shares of their textures here; ssr.z the exposure.)
  {
    vec2 g_uv = from / view.xy;
    float gz = ssr.x > 0.5 ? textureLod(glassTex, g_uv, 0.0).r : 0.0;
    vec2 gndc = g_uv * to_ndc.xy + to_ndc.zw;
    vec3 gray = vec3(dot(inv0.xyz, vec3(gndc, 1.0)), dot(inv1.xyz, vec3(gndc, 1.0)), dot(inv2.xyz, vec3(gndc, 1.0)));
    vec3 GP = eye_world.xyz + gray * (fog_b.x / max(gz, 1e-7));
    vec3 GN = cross(dFdx(GP), dFdy(GP));
    if (ssr.w > 0.5 && gz >= 0.999) seen = vec3(1.0, 1.0, 0.0);  // NXR_GLASS_DEBUG=2: the panes' coverage in yellow
    if (ssr.w > 2.5 && all(lessThan(px, vec2(24.0)))) seen = vec3(1.0, 0.0, 1.0);  // =3: a corner mark, the pass ran
    if (gz > 0.0) {
      float wz = textureLod(worldDepthTex, g_uv * cloud.xy, 0.0).r;
      float vz = textureLod(viewDepthTex, g_uv * cloud.zw, 0.0).r;
      float nl = length(GN);
      if (ssr.w > 2.5) {  // NXR_GLASS_DEBUG=3: why a pane fails (red: behind the world's depth, blue: covered at the
                          // view's end, green: no facing; white: passes), brighter the further off
        vec3 why = vec3(gz < wz * 0.9995 ? 0.3 + 0.7 * clamp((wz - gz) / max(wz, 1e-9) * 20.0, 0.0, 1.0) : 0.0,
                        nl <= 1e-8 ? 1.0 : 0.0,
                        vz > gz * 1.002 ? 0.3 + 0.7 * clamp((vz - gz) / max(gz, 1e-9) * 20.0, 0.0, 1.0) : 0.0);
        seen = dot(why, vec3(1.0)) > 0.0 ? why : vec3(1.0);
      } else if (gz >= wz * 0.9995 && vz <= gz * 1.002 && nl > 1e-8) {
        vec3 rd = normalize(gray);
        vec3 N = GN / nl;
        if (dot(N, rd) > 0.0) N = -N;
        vec3 R = normalize(reflect(rd, N));
        float cosv = clamp(dot(-rd, N), 0.0, 1.0);
        float F = min(0.04 + 0.96 * pow(1.0 - cosv, 5.0), 0.85);
        mat3 to_clip = inverse(transpose(mat3(inv0.xyz, inv1.xyz, inv2.xyz)));
        vec3 found = vec3(0.0);
        float got = 0.0;
        float wdist = fog_b.x / gz;
        vec3 X = GP + R * 0.05;
        float step_len = 0.25 + wdist * 0.004;
        for (int i = 0; i < 36; ++i) {
          X += R * step_len;
          step_len *= 1.14;
          vec3 c = to_clip * (X - eye_world.xyz);
          if (c.z <= 0.05) break;
          vec2 uv = (c.xy / c.z - to_ndc.zw) / to_ndc.xy;
          if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;
          float dq = textureLod(worldDepthTex, uv * cloud.xy, 0.0).r;
          if (dq <= 1e-6) continue;
          float wq = fog_b.x / dq;
          if (c.z > wq && c.z - wq < max(0.8, step_len * 1.5) && wq > c.z * 0.85) {
            vec2 edge = min(uv, 1.0 - uv);
            got = smoothstep(0.0, 0.08, min(edge.x, edge.y));
            found = textureLod(reflectTex, uv, 0.0).rgb;
            break;
          }
        }
        vec3 sky_way = mix(horizon_color.rgb, zenith_color.rgb, sqrt(clamp(R.y, 0.0, 1.0))) * ssr.z;
        vec3 cd = to_clip * R;
        if (cd.z > 1e-4) {
          vec2 uv = (cd.xy / cd.z - to_ndc.zw) / to_ndc.xy;
          if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0))) &&
              textureLod(worldDepthTex, uv * cloud.xy, 0.0).r <= 1e-6) {
            sky_way = textureLod(reflectTex, uv, 0.0).rgb;
          }
        }
        seen = mix(seen, mix(sky_way, found, got), F);
        if (ssr.w > 0.5) seen = mix(seen, vec3(1.0, 0.0, 1.0), 0.6);  // NXR_GLASS_DEBUG=1: the panes in magenta
      } else if (ssr.w > 0.5) {
        seen = mix(seen, vec3(0.0, 1.0, 1.0), 0.4);  // hidden panes in cyan
      }
    }
  }
  // D189: the storm's edges: blurred more toward the corners, and a little darker. D194 (the user, 18 Nov 2026: "I
  // didn't see the edge vignette blur"): its own strength (misc.w: in a vehicle too), from nearer the middle, out to
  // 1.6% of the view's height, two rings of eight places around, and 15% darker at the most.
  if (misc.w > 0.0) {
    float e = smoothstep(0.25, 0.75, length(mid * vec2(0.62, 1.0))) * misc.w;
    if (e > 0.01) {
      float reach = 0.016 * view.y * e;
      vec3 sum = seen;
      for (int n = 0; n < 16; ++n) {
        float ang = 0.392699 * float(n) + 0.3;
        vec2 p = clamp(at + vec2(cos(ang), sin(ang)) * reach * (n % 2 == 0 ? 1.0 : 0.45), vec2(0.5), view.xy - 0.5);
        sum += texelFetch(sceneTex, ivec2(p), 0).rgb;
      }
      seen = mix(seen, sum / 17.0, min(e * 1.4, 1.0)) * (1.0 - 0.15 * e);
    }
  }
  out_color = vec4(seen * (1.0 + 0.03 * m - 0.12 * rim), 1.0);
}
)";

// A wet copy of a colour texture (Sky::WetCopy): the surface darker, as wet things are, and beads of water in two
// sizes, each in a square of the texture: a darker rim, a little of the surface's own colour brightened inside, and a
// small white glint toward one side. Which squares have a bead goes by `count` (rain.z); all of it shows by view.z.
constexpr const char* kWetPS = R"(
layout(set = 0, binding = 2) uniform sampler2D srcTex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
// As the lens's drops ("Let's make it look similar to the lens droplets", then "make them look more wet reflective
// and a bit 3D", 14 Nov 2026): xy the place within the drop (-1 to 1 across it), z how much of a drop is here, w the
// shadow it casts beside itself.
vec4 Bead(vec2 px, float cell, float count, float key) {
  // (D217: the drops seeded by `key`, the cell's size at 1024 texels, not by its size here: a vehicle's colour and its
  // normal map, copied at other sizes, get their drops in the same places.)
  vec2 g = px / cell;
  vec2 id = floor(g), f = fract(g) - 0.5;
  vec2 rnd = vec2(h21(id + key), h21(id + 37.3 + key));
  // (Half as many as at first: "it's a bit too much there too. Probably about 50% less droplets", 14 Nov 2026. D217:
  // plenty again, "rain-drop sized, and plenty", 19 Nov 2026: 0.45 of the cells on the weapon, 0.6 on a vehicle.)
  float on = step(1.0 - count * (rain.x > 0.5 ? 0.6 : 0.45), h21(id + 5.1 + key));
  float size = (0.12 + 0.22 * h21(id + 9.2)) * (0.6 + 0.4 * count);
  vec2 d = f - (rnd - 0.5) * 0.5;
  // A blob, not a circle (as the lens's): a little longer one way, with an uneven edge.
  float turn = rnd.x * 6.2832;
  vec2 across = vec2(cos(turn), sin(turn));
  vec2 e = d - across * dot(d, across) * (0.12 + 0.22 * rnd.y);
  float edge = length(e) + size * 0.28 * (vnoise(g * 4.5 + rnd * 9.0) - 0.5);
  float m = smoothstep(size, size * 0.62, edge) * on;
  float shadow = smoothstep(size * 1.2, size * 0.7, length(d - vec2(0.22, 0.22) * size)) * on * (1.0 - m);
  return vec4(d / max(size, 1e-3), m, shadow);
}
// Water running down ("We can have streaks on the weapon and hands too", 14 Nov 2026): in columns of the texture, a
// small drop that creeps along it with a thin wet trail behind. xy the place within its head, z the head, w the trail.
// (Along the texture, which is not down on every part of the model: the texture does not say which way is down.)
vec4 Run(vec2 px, float cell, float count, float t) {
  float cx = px.x / cell;
  float id = floor(cx), fx = fract(cx) - 0.5;
  float r1 = h21(vec2(id, 3.0)), r2 = h21(vec2(id, 7.0));
  // (as many runs as at first, asked back on 14 Nov 2026; D217: "more frequent droplet runs", in 0.6 of the columns)
  // (the wetter and the harder it rains, the more: to 85% of the columns)
  float on = step(1.0 - min(count * 0.85 * (0.5 + 0.5 * rain.y), 0.85), h21(vec2(id, 11.0)));
  float clock = t * (0.012 + 0.02 * r1) + r2;
  float y = fract(clock);
  float dx = fx - 0.06 * sin(px.y / cell * 0.9 + r2 * 20.0);
  float dy = (px.y / view.y - y) * view.y / cell;
  vec2 d = vec2(dx, dy * 0.5);
  float head = smoothstep(0.16, 0.08, length(d)) * on;
  float up = clamp(-dy / 6.0, 0.0, 1.0);
  float width = mix(0.11, 0.03, up);
  float trail = smoothstep(width, width * 0.4, abs(dx)) * step(dy, 0.0) * (1.0 - up) * (1.0 - up) * on;
  return vec4(d / 0.16, head, trail * (1.0 - head));
}
void main() {
  vec2 px = gl_FragCoord.xy;
  vec4 c = textureLod(srcTex, px / view.xy, 0.0);
  if (view.w > 1.5) {  // the developer's check 98: the texture's colour on the whole (its smallest level)
    out_color = vec4(textureLod(srcTex, vec2(0.5), 16.0).rgb, c.a);
    return;
  }
  if (view.w > 0.5) {
    float check = mod(floor(px.x / (view.x / 16.0)) + floor(px.y / (view.y / 16.0)), 2.0);
    out_color = vec4(mix(vec3(1.0, 0.0, 1.0), vec3(0.0, 1.0, 0.0), check), c.a);
    return;
  }
  // Not a texture that is no colour at all: the hands' large texture is their surface's relief (its colour on the
  // whole is grey or bluish, not a surface's colour; drawn flat it left the hands as they were, and drops in it showed as dark
  // dents: runs mrg5 and avg1). It is left as it is; the hands still shine when wet.
  vec3 relief = textureLod(srcTex, vec2(0.5), 16.0).rgb;
  // A normal map (its colour on the whole is the blue of one: the weapon's own, found so and not by its number,
  // which differs between the game's ways of drawing a weapon; run slot1) gets the drops as relief ("Can we make the
  // water droplets on the gun a bump map too so they protrude in 3D?", 14 Nov 2026): under each drop, in the same
  // place as in the colour texture, the surface's normal leans outward from the drop's middle, as a dome's does.
  if (abs(relief.r - 0.5) < 0.12 && abs(relief.g - 0.5) < 0.12 && relief.b > 0.7) {
    if (rain.w > 0.5) {  // the hands': no drops ("just keep them looking wet with the shine", 14 Nov 2026)
      out_color = c;
      return;
    }
    float n_count = rain.z, n_scale = view.x / 1024.0 * (rain.x > 0.5 ? 0.23 : clamp(view.y / view.x, 0.4, 1.0));  // (D217)
    vec4 na = Bead(px, 30.0 * n_scale, n_count, 30.0), nb = Bead(px + 7.0 * n_scale, 15.0 * n_scale, n_count, 15.0);
    vec4 nrun = Run(px, 30.0 * n_scale, n_count, eye.w);
    vec4 at = na.z >= nb.z ? na : nb;
    if (nrun.z > at.z) at = vec4(nrun.xy, nrun.z, 0.0);
    vec3 normal = c.rgb * 2.0 - 1.0;
    normal.xy += at.xy * at.z * 1.5 * view.z;
    out_color = vec4(normalize(normal) * 0.5 + 0.5, c.a);
    return;
  }
  // The colour's drops only in the weapon's large colour texture (the smaller ones are other things); a vehicle's
  // texture that is not its colour and no normal map is left as it is (D217).
  if ((view.x < 1000.0 && rain.x < 0.5) || rain.x > 1.5) {
    out_color = c;
    return;
  }
  // (Nor bluish only: grey on the whole too, run mrg7. A weapon's own colours are warm: its red over its blue.)
  // (Only asked of the hands' textures now, which are told by their material: asked of a weapon's too, a weapon of
  // cool grey steel, the Type 100, got no drops at all: run kitJ.)
  if (rain.w > 0.5 && relief.r - relief.b < 0.035) {
    out_color = c;
    return;
  }
  float count = rain.z, show = view.z;
  // D217: on a vehicle's copy (twice its texture's size, its texture over several metres) the cells a quarter as
  // large: drops of about 5 to 14 mm.
  float scale = view.x / 1024.0 * (rain.x > 0.5 ? 0.23 : 1.0);
  // A long weapon's texture (twice as wide as tall: the bazooka's, 1024 x 512) is spread over a longer model: its
  // cells smaller by the texture's height over its width, so its drops come as small and as many as the Thompson's
  // (the user, 19 Nov 2026: "The droplets on the thompson are dens enough, but not on things like the Bazooka").
  if (rain.x < 0.5) scale *= clamp(view.y / view.x, 0.4, 1.0);
  vec4 a = Bead(px, 30.0 * scale, count, 30.0), b = Bead(px + 7.0 * scale, 15.0 * scale, count, 15.0);
  vec4 run = Run(px, 30.0 * scale, count, eye.w);  // (D217: columns 30 wide, 44 before: more runs)
  vec4 drop_at = a.z >= b.z ? a : b;
  if (run.z > drop_at.z) drop_at = vec4(run.xy, run.z, 0.0);
  vec2 n = drop_at.xy;
  float m = drop_at.z, shadow = max(a.w, b.w), trail = run.w;
  // No drops on skin ("Should we remove droplets on the hands? they still look like black dots instead of transparent
  // water beads that streak towards the ground", 14 Nov 2026): the hands' texture (rain.w: told by the shaders it is
  // drawn with; by its colour on the whole the weapon's wood and worn metal passed for skin, run mrg1) only gets wet,
  // darker and shining.
  if (rain.w > 0.5) m = 0.0, shadow = 0.0, trail = 0.0;
  // And none where the surface itself is warm in colour (red well over green, green over blue: skin, and wood too):
  // in a hosted match the hands are drawn in other ways and are not told from the weapon (runs mrg3 and mrg4).
  float warm = smoothstep(1.1, 1.28, c.r / max(c.g, 1e-3)) * smoothstep(1.02, 1.14, c.g / max(c.b, 1e-3));
  m *= 1.0 - warm, shadow *= 1.0 - warm, trail *= 1.0 - warm;
  float darker = 1.0 - 0.22 * min(count * 2.0, 1.0);
  // A drop as glass ("Make the droplets and the wetness almost glass-like", 14 Nov 2026): clear, so what is under it
  // shows drawn in and turned over, hardly darkened; the light gathers in a bright crescent on the far side; a sharp
  // glint and a soft reflection of the sky on the near side; a thin bright line along the near edge and only a
  // faint dark one along the far; a little shadow beside it.
  vec3 through = textureLod(srcTex, (px - n * m * 8.0 * scale) / view.xy, 0.0).rgb * mix(darker, 1.0, 0.5);
  float toward = clamp(-(n.x + n.y) * 0.7, 0.0, 1.0);
  float glint = smoothstep(0.34, 0.05, length(n + vec2(0.4, 0.4))) * m;
  float sky = smoothstep(0.25, -0.7, n.y + n.x * 0.5) * m;
  float gathered = smoothstep(0.8, 0.2, length(n - vec2(0.32, 0.32))) * m;
  float rim = 4.0 * m * (1.0 - m);
  vec3 drop = through * (1.0 + 0.45 * gathered - 0.14 * rim * (1.0 - toward)) + vec3(0.55, 0.62, 0.72) * 0.25 * sky +
              vec3(0.8) * rim * toward * 0.35 + vec3(1.0) * glint;
  // The wet surface: darker, and where water has run a slightly brighter, clearer line.
  vec3 surface = c.rgb * darker * (1.0 - 0.18 * shadow);
  surface = mix(surface, c.rgb * mix(darker, 1.0, 0.35) + vec3(0.06), trail * 0.7);
  vec3 wet = mix(surface, drop, m);
  out_color = vec4(mix(c.rgb, wet, show), c.a);
}
)";

RefCntAutoPtr<IShader> MakeShader(IRenderDevice* device, SHADER_TYPE type, const std::string& source, const char* name) {
  RefCntAutoPtr<IShader> shader;
  std::vector<uint32_t> spirv = CompileGlsl(type == SHADER_TYPE_VERTEX ? GlslStage::kVertex : GlslStage::kPixel, source, name);
  if (spirv.empty()) return shader;
  ShaderCreateInfo info;
  info.Desc.Name = name;
  info.Desc.ShaderType = type;
  info.Desc.UseCombinedTextureSamplers = true;
  info.ByteCode = spirv.data();
  info.ByteCodeSize = spirv.size() * sizeof(uint32_t);
  device->CreateShader(info, &shader);
  return shader;
}

// ---------------------------------------------------------------------------------------------------------------
// The time of day (Beachhead's atmosphere_view.gd, _apply_time_weather): colours are sRGB there, linear here.

struct Rgb {
  float r, g, b;
};
Rgb Mix(const Rgb& a, const Rgb& b, float t) { return {Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t)}; }
float ToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
void StoreLinear(float* out, const Rgb& c, float w) { out[0] = ToLinear(c.r), out[1] = ToLinear(c.g), out[2] = ToLinear(c.b), out[3] = w; }
float Smooth(float lo, float hi, float v) {
  const float t = std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

}  // namespace

struct SkyConstantBlock {
  float inv[3][4];
  float eye[4], to_ndc[4], view[4];
  float sun_dir[4], sun_color[4], moon_dir[4], moon_color[4], moon_light[4];
  float fog[4], zenith[4], horizon[4], mid[4];
  float cloud[4], drift[4];
  float last[3][4], last_to_ndc[4], misc[4];
  float eye_world[4], fog_a[4], fog_b[4], flash[4];
  float cover_m[9][4], rain[4];
  float ssr[4];  // D190
  float bolt[4], bolt_box[4], bolt_seg[144][4];  // D192
  float gale[4];  // D194
  float storm_a[4], storm_b[4];  // D208
  float layers[4];               // D211
};

bool Sky::SeaSkyColours(float zenith[3], float horizon[3]) const {
  if (last_fill_.size() != sizeof(SkyConstantBlock)) return false;
  SkyConstantBlock k;
  std::memcpy(&k, last_fill_.data(), sizeof(k));
  const float grey = std::clamp((k.cloud[0] - 0.2f) / 0.6f, 0.0f, 1.0f);
  const float g = grey * grey * (3.0f - 2.0f * grey);
  for (float* out : {zenith, horizon}) {
    const float* in = out == zenith ? k.zenith : k.horizon;
    const float lum = (0.3f * in[0] + 0.55f * in[1] + 0.15f * in[2]) * 0.9f;
    // (Times 0.62: the palette with the exposure came to about 1.0 at the horizon where the sky drawn there is about
    // 0.64, and the sea's far band shone white, run shore_on4.)
    for (int c = 0; c < 3; ++c) out[c] = (in[c] + (lum - in[c]) * g) * k.view[2] * 0.62f;
  }
  return true;
}

bool Sky::WetCopy(IDeviceContext* context, ITextureView* from, ITexture* to, float count, float show, bool tint,
                  double seconds, bool skin, bool vehicle, bool relief_only) {
  if (!from || !to || !Ready()) return false;
  const TextureDesc& desc = to->GetDesc();
  if (!wet_pso_) {
    SamplerDesc linear;
    linear.MinFilter = linear.MagFilter = linear.MipFilter = FILTER_TYPE_LINEAR;
    linear.AddressU = linear.AddressV = linear.AddressW = TEXTURE_ADDRESS_CLAMP;
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "srcTex", linear}};
    GraphicsPipelineStateCreateInfo info;
    info.PSODesc.Name = "NativeXRenderer sky wet copy";
    info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
    info.GraphicsPipeline.NumRenderTargets = 1;
    info.GraphicsPipeline.RTVFormats[0] = TEX_FORMAT_RGBA8_UNORM;
    info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
    info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
    info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
    info.pVS = vs_;
    info.pPS = wet_ps_;
    info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
    info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    info.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
    device_->CreateGraphicsPipelineState(info, &wet_pso_);
    if (wet_pso_) wet_pso_->CreateShaderResourceBinding(&wet_srb_, true);
    if (!wet_srb_) return false;
  }
  if (!wet_srb_ || desc.Format != TEX_FORMAT_RGBA8_UNORM) return false;
  TextureViewDesc first;
  first.ViewType = TEXTURE_VIEW_RENDER_TARGET;
  first.TextureDim = desc.Type;  // (the game's textures are arrays of one layer: the copy is as they are)
  first.MostDetailedMip = 0, first.NumMipLevels = 1;
  first.FirstArraySlice = 0, first.NumArraySlices = 1;
  RefCntAutoPtr<ITextureView> target;
  to->CreateView(first, &target);
  if (!target) return false;
  SkyConstantBlock k{};
  k.view[0] = float(desc.Width), k.view[1] = float(desc.Height), k.view[2] = show, k.view[3] = tint ? (whole_colour_ ? 2.0f : 1.0f) : 0.0f;
  k.rain[2] = count;
  k.rain[3] = skin ? 1.0f : 0.0f;
  k.rain[0] = relief_only ? 2.0f : vehicle ? 1.0f : 0.0f;  // D217 (2: a vehicle's texture that is not its colour)
  k.rain[1] = std::clamp(lens_rain_, 0.0f, 1.0f);  // D217: how hard it rains here (the runs' number)
  // D217: the runs' own clock (the user, 19 Nov 2026: "The droplet runs should be a lot more frequent and faster when
  // directly in the rain, but when under cover, the drying begins and the become less frequent and slower"): it runs at
  // 0.3 to 2.5 times the world's by how wet the surface is (its share of drops: building in the rain, drying under
  // cover), summed as the copies are made, so a change of pace never jumps a run along.
  {
    static double last = -1.0, clock = 0.0;
    // (And by how hard it rains where the eye is, lens_rain_: "Speed and frequency of all droplet runs (including the
    // player camera) should be tied to weather percentage too", 19 Nov 2026: 0.6 to 1.4 times.)
    if (last >= 0.0 && seconds > last && seconds - last < 1.0)
      clock += (seconds - last) * (0.3 + 2.2 * std::clamp(count, 0.0f, 1.0f)) * (0.6 + 0.8 * std::clamp(lens_rain_, 0.0f, 1.0f));
    if (seconds != last) last = seconds;
    k.eye[3] = float(std::fmod(clock, 3600.0));
  }
  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, &k, sizeof(k));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }
  if (auto* v = wet_srb_->GetVariableByName(SHADER_TYPE_PIXEL, "SkyConstants")) v->Set(constants_);
  if (auto* v = wet_srb_->GetVariableByName(SHADER_TYPE_PIXEL, "srcTex")) v->Set(from);
  ITextureView* rtv = target;
  context->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  Viewport viewport;
  viewport.Width = float(desc.Width);
  viewport.Height = float(desc.Height);
  viewport.MaxDepth = 1.0f;
  context->SetViewports(1, &viewport, 0, 0);
  context->SetPipelineState(wet_pso_);
  context->CommitShaderResources(wet_srb_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DrawAttribs attribs;
  attribs.NumVertices = 3;
  context->Draw(attribs);
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  context->GenerateMips(to->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  return true;
}

bool Sky::Lens(IDeviceContext* context, ITexture* color, uint32_t width, uint32_t height, bool drops,
               const GlassInputs* glass) {
  if (!color || !width || !height || !Ready()) return false;
  const bool glassy = glass && glass->mask && glass->world_depth && glass->view_depth &&
                      last_fill_.size() == sizeof(SkyConstantBlock);
  if ((!drops || lens_wet() < 0.01f) && sway_ < 0.005f && edge_ < 0.005f && !glassy) return false;
  const bool depthy = glass && glass->world_depth && glass->view_depth;  // D195
  const TextureDesc& desc = color->GetDesc();
  if (!lens_pso_ || lens_format_ != desc.Format) {
    lens_pso_.Release(), lens_srb_.Release();
    SamplerDesc point;
    point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
    point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
    SamplerDesc lin;
    lin.MinFilter = lin.MagFilter = lin.MipFilter = FILTER_TYPE_LINEAR;
    lin.AddressU = lin.AddressV = lin.AddressW = TEXTURE_ADDRESS_CLAMP;
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "sceneTex", point},
                                       {SHADER_TYPE_PIXEL, "glassTex", point},
                                       {SHADER_TYPE_PIXEL, "worldDepthTex", point},
                                       {SHADER_TYPE_PIXEL, "viewDepthTex", point},
                                       {SHADER_TYPE_PIXEL, "reflectTex", lin}};
    GraphicsPipelineStateCreateInfo info;
    info.PSODesc.Name = "NativeXRenderer sky lens";
    info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
    info.GraphicsPipeline.NumRenderTargets = 1;
    info.GraphicsPipeline.RTVFormats[0] = desc.Format;
    info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
    info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
    info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
    info.pVS = vs_;
    info.pPS = lens_ps_;
    info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
    info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    info.PSODesc.ResourceLayout.NumImmutableSamplers = 5;
    device_->CreateGraphicsPipelineState(info, &lens_pso_);
    if (lens_pso_) lens_pso_->CreateShaderResourceBinding(&lens_srb_, true);
    lens_format_ = desc.Format;
    if (!lens_srb_) return false;
  }
  if (!lens_copy_ || lens_copy_->GetDesc().Width != desc.Width || lens_copy_->GetDesc().Height != desc.Height ||
      lens_copy_->GetDesc().Format != desc.Format) {
    lens_copy_.Release();
    TextureDesc copy;
    copy.Name = "NativeXRenderer sky lens, the view behind";
    copy.Type = RESOURCE_DIM_TEX_2D;
    copy.Width = desc.Width, copy.Height = desc.Height;
    copy.Format = desc.Format;
    copy.BindFlags = BIND_SHADER_RESOURCE;
    device_->CreateTexture(copy, nullptr, &lens_copy_);
    if (!lens_copy_) return false;
  }
  ITextureView* target = color->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
  if (!target) return false;
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  CopyTextureAttribs copy(color, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, lens_copy_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->CopyTexture(copy);
  SkyConstantBlock k{};
  // D191: the view's own constants (its rays, the sea's height, the sky's palette) under the lens's.
  if (glassy) {
    std::memcpy(&k, last_fill_.data(), sizeof(k));
    k.ssr[0] = 1.0f;
    k.ssr[2] = k.view[2];  // the exposure, before the lens takes view.z
    static const float debug = [] {
      const char* v = std::getenv("NXR_GLASS_DEBUG");
      return v && *v >= '1' && *v <= '4' ? float(*v - '0') : 0.0f;
    }();
    k.ssr[3] = debug;
    k.cloud[0] = glass->world_uv[0], k.cloud[1] = glass->world_uv[1];
    k.cloud[2] = glass->view_uv[0], k.cloud[3] = glass->view_uv[1];
    k.drift[0] = k.drift[1] = 0.0f;
  } else {
    k.ssr[0] = 0.0f;
    if (depthy) {  // D195: the depths alone, for the drops on a vehicle's glass
      k.cloud[0] = glass->world_uv[0], k.cloud[1] = glass->world_uv[1];
      k.cloud[2] = glass->view_uv[0], k.cloud[3] = glass->view_uv[1];
    }
  }
  k.ssr[1] = depthy && glass->cockpit ? 1.0f : 0.0f;  // D195
  k.view[0] = float(width), k.view[1] = float(height);
  // D217: the lens's drops on a clock paced by how hard it rains here (0.4 to 2.4 times the world's), summed so a change
  // of pace never jumps them along.
  {
    static double last = -1.0, clock = 0.0;
    if (last >= 0.0 && lens_seconds_ > last && lens_seconds_ - last < 1.0)
      clock += (lens_seconds_ - last) * (0.4 + 2.0 * std::clamp(lens_rain_, 0.0f, 1.0f));
    if (lens_seconds_ != last) last = lens_seconds_;
    k.eye[3] = float(std::fmod(clock, 3600.0));
  }
  k.rain[2] = lens_peak_;
  k.rain[3] = splash_peak_;
  k.view[2] = lens_peak_ > 0.0f ? std::clamp(lens_wet_ / lens_peak_, 0.0f, 1.0f) : 0.0f;
  k.view[3] = splash_peak_ > 0.0f ? std::clamp(splash_wet_ / splash_peak_, 0.0f, 1.0f) : 0.0f;
  if (!drops) k.view[2] = k.view[3] = 0.0f;
  // D193: the storm's shake (the user, 18 Nov 2026: "The screen wobble is incorrect. It should be lightly shaking the
  // player camera when outside and not in a vehicle"; D189's was a slow lean of up to 0.6 degrees): how strong
  // (drift.x), the picture's roll now (drift.y, radians, to about 0.09 degrees) and its offset (drift.zw, to about 0.16%
  // of the view's height: 2 pixels at 1440): a quick jitter of 6 to 18 Hz, in buffets that come and go over a few
  // seconds. On the wall clock, smooth whatever the world's clock does.
  {
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const float buffet = float(0.65 + 0.35 * (0.6 * std::sin(t * 0.43) + 0.4 * std::sin(t * 1.07 + 1.3)));
    const float jx = float(0.5 * std::sin(t * 41.0) + 0.3 * std::sin(t * 67.0 + 1.1) + 0.2 * std::sin(t * 109.0 + 2.3));
    const float jy = float(0.5 * std::sin(t * 37.0 + 0.7) + 0.3 * std::sin(t * 73.0 + 2.9) + 0.2 * std::sin(t * 97.0 + 0.4));
    const float jr = float(0.6 * std::sin(t * 47.0 + 1.9) + 0.4 * std::sin(t * 83.0 + 0.2));
    const float s = sway_ * buffet;
    k.drift[0] = sway_;
    k.misc[3] = edge_;  // D194
    k.drift[1] = s * 0.0016f * jr;
    k.drift[2] = s * 0.0016f * jx;
    k.drift[3] = s * 0.0016f * jy;
  }
  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, &k, sizeof(k));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }
  if (auto* v = lens_srb_->GetVariableByName(SHADER_TYPE_PIXEL, "SkyConstants")) v->Set(constants_);
  if (auto* v = lens_srb_->GetVariableByName(SHADER_TYPE_PIXEL, "sceneTex"))
    v->Set(lens_copy_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  if (auto* v = lens_srb_->GetVariableByName(SHADER_TYPE_PIXEL, "reflectTex"))
    v->Set(lens_copy_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  {
    ITextureView* none = no_scene_ ? no_scene_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
    auto set = [&](const char* name, ITextureView* view) {
      if (auto* v = lens_srb_->GetVariableByName(SHADER_TYPE_PIXEL, name)) {
        if (view) v->Set(view);
      }
    };
    set("glassTex", glassy ? glass->mask : none);
    set("worldDepthTex", depthy ? glass->world_depth : none);
    set("viewDepthTex", depthy ? glass->view_depth : none);
  }
  context->SetRenderTargets(1, &target, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  Viewport viewport;
  viewport.Width = float(width);
  viewport.Height = float(height);
  viewport.MaxDepth = 1.0f;
  context->SetViewports(1, &viewport, 0, 0);
  context->SetPipelineState(lens_pso_);
  context->CommitShaderResources(lens_srb_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DrawAttribs attribs;
  attribs.NumVertices = 3;
  context->Draw(attribs);
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  return true;
}

// The lightning (sky.h): the world's clock in slots (16 s in a storm's first stage down to 5 s in its worst), most
// of them with one flash of one to four strokes, each a sharp light that dies away in a tenth of a second.
namespace {
std::atomic<bool> g_lightning{true};
}
void SetSkyLightning(bool on) { g_lightning.store(on, std::memory_order_relaxed); }
namespace {
std::atomic<bool> g_cloud_shadows{true};  // D214
}  // namespace
void SetCloudShadows(bool on) { g_cloud_shadows.store(on, std::memory_order_relaxed); }
namespace {
std::atomic<bool> g_storm_sway{true};
}
void SetStormSway(bool on) { g_storm_sway.store(on, std::memory_order_relaxed); }
void SetEyeOnFoot(bool on_foot) { g_eye_on_foot.store(on_foot, std::memory_order_relaxed); }
namespace {
std::atomic<bool> g_sea_reflection{true};
}
void SetSeaReflection(bool on) { g_sea_reflection.store(on, std::memory_order_relaxed); }
bool SeaReflection() { return g_sea_reflection.load(std::memory_order_relaxed); }

namespace {
// One slot's flash, if it has one: when it starts on the world's clock, its strokes' strengths, where it is.
struct SlotFlash {
  double at = 0.0;
  int strokes = 0;
  float amp[4] = {};
  float angle = 0.0f, far = 0.0f;
  bool bolt = false;   // D192: a bolt to the ground
  uint64_t shape = 0;  // its shape's seed
};
// D208: where the strikes may fall: the hurricane, the middle of the eye's 4 km square (the strikes' offsets are from
// there), and where the players are gathered (the game's, SetPlayerGroups). Set each frame (renderer.cpp).
struct StrikeWorld {
  storm::Storm storm;
  float square[2] = {};
  float groups[16] = {};
  int count = 0;
};
std::mutex g_strike_mutex;
StrikeWorld g_strike;
StrikeWorld StrikeContext() {
  std::lock_guard lock(g_strike_mutex);
  return g_strike;
}

bool FlashInSlot(int64_t s, double period, float storm, SlotFlash& f) {
  uint64_t state = 0x1943ull ^ (uint64_t(s) * 0x9e3779b97f4a7c15ull);
  if (Uniform(state) > 0.65f + 0.35f * storm) return false;
  f.at = double(s) * period + Uniform(state) * (period - 2.0);
  f.strokes = 1 + int(SplitMix(state) % 4);
  f.angle = Uniform(state) * 6.2831853f;
  // Most strike 1.5 to 10 km off; about one in six within 300 m to 1.5 km (17 Nov 2026, with the storm's sounds: a
  // close strike's crack), the same draw split so the rest of each slot's numbers stay as they were.
  const float u = Uniform(state);
  f.far = u < 0.18f ? 300.0f + u / 0.18f * 1200.0f : 1500.0f + (u - 0.18f) / 0.82f * 8500.0f;
  for (int k = 0; k < f.strokes; ++k) f.amp[k] = 0.45f + 0.55f * Uniform(state);
  // D192: a bolt to the ground (Beachhead's lightning.gd, its ground and close strikes): always within 1.5 km (where
  // the thunder is a close strike's crack), further out to 7 km now and then, more often the worse the storm; the
  // rest stay flashes inside the cloud. Drawn after the slot's other numbers, which stay as they were.
  const float b = Uniform(state);
  f.bolt = f.far < 1500.0f || (f.far < 7000.0f && b < 0.35f + 0.4f * storm);
  f.shape = SplitMix(state);
  // D208: under the hurricane only, and near the players now and then (the user, 19 Nov 2026: "Thunder and lighting
  // only happen directly under the storm itself too", "Lightning bolts should also have a higher chance to strike near
  // a group of players, resulting in the close lightning audio being loud enough to startle the player"). Three in
  // ten go to one of the players' groups, 60 to 400 m from it, as a bolt; the others stay where they were drawn. A
  // strike falls only where the storm is strong: kept with the chance smoothstep(0.45, 0.85, its strength there).
  // Drawn after the slot's other numbers, which stay as they were.
  const StrikeWorld w = StrikeContext();
  if (w.storm.on) {
    float ox = std::sin(f.angle) * f.far, oz = std::cos(f.angle) * f.far;
    const float near_group = Uniform(state);
    const uint64_t pick = SplitMix(state);
    const float ja = Uniform(state) * 6.2831853f, jr = 60.0f + 340.0f * Uniform(state);
    const float keep = Uniform(state);
    if (w.count > 0 && near_group < 0.3f) {
      const int g = int(pick % uint64_t(w.count));
      ox = w.groups[g * 2] + std::sin(ja) * jr - w.square[0];
      oz = w.groups[g * 2 + 1] + std::cos(ja) * jr - w.square[1];
      f.bolt = true;
    }
    else {
      // D213: in the storm itself (a level-sized storm: placed in the 4 km square and kept only under the storm, most
      // strikes were lost): within its rain shield, the same draws.
      const float rr = w.storm.radius * std::sqrt(Uniform(state)), ra = Uniform(state) * 6.2831853f;
      ox = w.storm.cx + std::sin(ra) * rr - w.square[0];
      oz = w.storm.cz + std::cos(ra) * rr - w.square[1];
    }
    const float here = storm::At(w.storm, w.square[0] + ox, w.square[1] + oz);
    if (keep > storm::Smooth(0.3f, 0.7f, here)) return false;
    f.angle = std::atan2(ox, oz);
    f.far = std::max(std::sqrt(ox * ox + oz * oz), 1.0f);
  }
  return true;
}

// D192: a bolt's channel (Beachhead's _build_bolt, _jag): from the cloud base straight down to the sea, jagged by
// halving each piece with a sideways offset (5 halvings: 32 pieces), and a few branches off it that run down and
// out, some splitting once more. Each piece: its two ends (world), its half width (metres), how bright it is.
struct BoltPiece {
  float a[3], b[3];
  float half = 0.0f, bright = 1.0f;
};
constexpr int kBoltPieces = 72;
void Jag(const float a[3], const float b[3], int levels, uint64_t& state, std::vector<std::array<float, 3>>& out) {
  out.assign({{a[0], a[1], a[2]}, {b[0], b[1], b[2]}});
  const float len = std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2]));
  float off = len * 0.16f;
  for (int l = 0; l < levels; ++l) {
    std::vector<std::array<float, 3>> next;
    next.reserve(out.size() * 2);
    for (size_t i = 0; i + 1 < out.size(); ++i) {
      const auto& p = out[i];
      const auto& q = out[i + 1];
      float dir[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]};
      const float dl = std::max(std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]), 1e-4f);
      for (float& c : dir) c /= dl;
      float rnd[3] = {Uniform(state) * 2.0f - 1.0f, Uniform(state) * 2.0f - 1.0f, Uniform(state) * 2.0f - 1.0f};
      const float along = rnd[0] * dir[0] + rnd[1] * dir[1] + rnd[2] * dir[2];
      float perp[3] = {rnd[0] - dir[0] * along, rnd[1] - dir[1] * along, rnd[2] - dir[2] * along};
      const float pl = std::max(std::sqrt(perp[0] * perp[0] + perp[1] * perp[1] + perp[2] * perp[2]), 1e-4f);
      const float k = off * (Uniform(state) * 2.0f - 1.0f) / pl;
      next.push_back(p);
      next.push_back({(p[0] + q[0]) * 0.5f + perp[0] * k, (p[1] + q[1]) * 0.5f + perp[1] * k, (p[2] + q[2]) * 0.5f + perp[2] * k});
    }
    next.push_back(out.back());
    out.swap(next);
    off *= 0.55f;
  }
}
// The bolt of a strike `dist` metres off: its foot at `foot` (x, the sea's height, z), its top at the cloud base.
std::vector<BoltPiece> BoltShape(uint64_t seed, const float foot[3], float base, float dist) {
  uint64_t state = seed;
  std::vector<BoltPiece> pieces;
  const float width = 0.45f + dist * 0.0013f;  // (Beachhead's: wider far off, so it stays seen)
  const float ground[3] = {foot[0], foot[1], foot[2]};
  const float top[3] = {foot[0] + (Uniform(state) - 0.5f) * 0.1f * dist, base + 30.0f, foot[2] + (Uniform(state) - 0.5f) * 0.1f * dist};
  auto add = [&](const std::vector<std::array<float, 3>>& pts, float half, float bright, bool taper) {
    const size_t n = pts.size();
    for (size_t i = 0; i + 1 < n && pieces.size() < size_t(kBoltPieces); ++i) {
      BoltPiece p;
      for (int c = 0; c < 3; ++c) p.a[c] = pts[i][c], p.b[c] = pts[i + 1][c];
      const float t = taper ? 1.0f - 0.7f * float(i) / float(n) : 1.0f;
      p.half = half * t, p.bright = bright * t;
      pieces.push_back(p);
    }
  };
  std::vector<std::array<float, 3>> main, branch, twig;
  Jag(top, ground, 5, state, main);
  add(main, width * 0.5f, 1.0f, false);
  const int branches = 3 + int(SplitMix(state) % 3);
  for (int b = 0; b < branches; ++b) {
    const size_t at = size_t(float(main.size()) * (0.08f + 0.67f * Uniform(state)));
    const auto& p = main[std::min(at, main.size() - 1)];
    float rem[3] = {ground[0] - p[0], ground[1] - p[1], ground[2] - p[2]};
    const float rl = std::max(std::sqrt(rem[0] * rem[0] + rem[1] * rem[1] + rem[2] * rem[2]), 1e-3f);
    const float sa = Uniform(state) * 6.2831853f;
    float d[3] = {rem[0] / rl * 0.6f + std::sin(sa) * 0.8f, rem[1] / rl * 0.6f + (Uniform(state) * 0.3f - 0.2f),
                  rem[2] / rl * 0.6f + std::cos(sa) * 0.8f};
    const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const float len = rl * (0.15f + 0.25f * Uniform(state));
    const float from[3] = {p[0], p[1], p[2]};
    const float to[3] = {p[0] + d[0] / dl * len, p[1] + d[1] / dl * len, p[2] + d[2] / dl * len};
    Jag(from, to, 3, state, branch);
    add(branch, width * 0.5f * 0.45f, 0.6f, true);
    if (Uniform(state) < 0.4f) {
      const auto& q = branch[branch.size() / 2];
      const float qf[3] = {q[0], q[1], q[2]};
      const float qt[3] = {q[0] + (d[0] / dl + std::sin(sa) * 0.5f) * len * 0.5f, q[1] - len * 0.2f,
                           q[2] + (d[2] / dl + std::cos(sa) * 0.5f) * len * 0.5f};
      Jag(qf, qt, 2, state, twig);
      add(twig, width * 0.5f * 0.28f, 0.45f, true);
    }
  }
  return pieces;
}
// D211: lightning inside the clouds (the user's reference of a tropical storm, 19 Nov 2026: bolts branching through
// the clouds): an in-cloud strike's channel crawls along under the cloud base, 1.5 to 4 km, dipping a little, with
// four to six branches forking off sideways and down, some forking once more.
std::vector<BoltPiece> CrawlerShape(uint64_t seed, const float start[3], float dist) {
  uint64_t state = seed ^ 0xc4a71e5ull;
  std::vector<BoltPiece> pieces;
  const float width = (0.4f + dist * 0.0011f) * 1.6f;  // D212: 1.6 times (thin and faint seen across a storm)
  const float a = Uniform(state) * 6.2831853f, len = 1500.0f + 2500.0f * Uniform(state);
  const float end[3] = {start[0] + std::sin(a) * len, start[1] - 60.0f - 140.0f * Uniform(state), start[2] + std::cos(a) * len};
  auto add = [&](const std::vector<std::array<float, 3>>& pts, float half, float bright) {
    const size_t n = pts.size();
    for (size_t i = 0; i + 1 < n && pieces.size() < size_t(kBoltPieces); ++i) {
      BoltPiece q;
      for (int c = 0; c < 3; ++c) q.a[c] = pts[i][c], q.b[c] = pts[i + 1][c];
      const float t = 1.0f - 0.6f * float(i) / float(n);
      q.half = half * t, q.bright = bright * t;
      pieces.push_back(q);
    }
  };
  std::vector<std::array<float, 3>> main, branch, twig;
  Jag(start, end, 5, state, main);
  add(main, width * 0.5f, 0.85f);
  const int branches = 4 + int(SplitMix(state) % 3);
  for (int b = 0; b < branches; ++b) {
    const auto& q = main[std::min(size_t(float(main.size()) * (0.05f + 0.85f * Uniform(state))), main.size() - 1)];
    const float side = (Uniform(state) < 0.5f ? -1.0f : 1.0f) * (0.6f + 0.9f * Uniform(state));
    const float da = a + side, bl = len * (0.15f + 0.3f * Uniform(state));
    const float from[3] = {q[0], q[1], q[2]};
    const float to[3] = {q[0] + std::sin(da) * bl, q[1] - bl * (0.05f + 0.25f * Uniform(state)), q[2] + std::cos(da) * bl};
    Jag(from, to, 3, state, branch);
    add(branch, width * 0.5f * 0.45f, 0.55f);
    if (Uniform(state) < 0.45f) {
      const auto& r = branch[branch.size() / 2];
      const float tf[3] = {r[0], r[1], r[2]};
      const float tt[3] = {r[0] + std::sin(da + side * 0.5f) * bl * 0.4f, r[1] - bl * 0.15f, r[2] + std::cos(da + side * 0.5f) * bl * 0.4f};
      Jag(tf, tt, 2, state, twig);
      add(twig, width * 0.5f * 0.28f, 0.4f);
    }
  }
  return pieces;
}

// How bright the bolt is dt seconds into its flash: each stroke a sharp light (it flickers, as Beachhead's return
// strokes do), and faint between them until the flash is over. 0: no bolt now.
float BoltLight(const SlotFlash& f, double dt) {
  const double end = (f.strokes - 1) * 0.12 + 0.5;
  if (dt < 0.0 || dt > end) return 0.0f;
  float v = 0.0f;
  for (int k = 0; k < f.strokes; ++k) {
    const double since = dt - k * 0.12;
    if (since >= 0.0) v = std::max(v, f.amp[k] * float((1.0 - std::exp(-since * 150.0)) * std::exp(-since * 14.0)));
  }
  return std::max(v, 0.1f * float(1.0 - dt / end));
}
}  // namespace

void SetStrikeWorld(const storm::Storm& storm, float square_x, float square_z) {
  std::lock_guard lock(g_strike_mutex);
  g_strike.storm = storm, g_strike.square[0] = square_x, g_strike.square[1] = square_z;
}
void SetStrikeGroups(const float* xz, uint32_t count) {
  std::lock_guard lock(g_strike_mutex);
  g_strike.count = int(std::min<uint32_t>(xz ? count : 0, 8));
  for (int i = 0; i < g_strike.count * 2; ++i) g_strike.groups[i] = xz[i];
}

uint32_t LightningStrikes(double from, double to, float weather, LightningStrike* out, uint32_t max) {
  if (!g_lightning.load(std::memory_order_relaxed) || !out || max == 0 || to <= from) return 0;
  const float storm = Smooth(0.6f, 0.95f, weather);
  if (storm <= 0.0f || to < 0.0) return 0;
  const double period = Lerp(16.0f, 5.0f, storm);
  uint32_t n = 0;
  for (int64_t s = std::max<int64_t>(0, int64_t(from / period) - 1); s <= int64_t(to / period) && n < max; ++s) {
    SlotFlash f;
    if (!FlashInSlot(s, period, storm, f) || f.at <= from || f.at > to) continue;
    LightningStrike& o = out[n++];
    o.at = f.at;
    o.x = std::sin(f.angle) * f.far, o.z = std::cos(f.angle) * f.far;
    o.strokes = f.strokes;
    float sum = 0.0f;
    for (int k = 0; k < f.strokes; ++k) sum += f.amp[k];
    o.strength = std::min(1.0f, sum / 2.0f);
  }
  return n;
}

SkyFlash SkyFlashAt(double seconds, float weather) {
  SkyFlash out;
  if (!g_lightning.load(std::memory_order_relaxed)) return out;
  // NXR_FLASH=1 (a developer's check): one flash held on, 5 km off.
  static const bool held = [] {
    const char* v = std::getenv("NXR_FLASH");
    return v && *v == '1';
  }();
  if (held) return {1.0f, 3500.0f, 3500.0f};
  const float storm = Smooth(0.6f, 0.95f, weather);
  if (storm <= 0.0f || seconds < 0.0) return out;
  const double period = Lerp(16.0f, 5.0f, storm);
  const int64_t slot = int64_t(seconds / period);
  for (int64_t s = slot; s >= slot - 1 && s >= 0; --s) {
    SlotFlash f;
    if (!FlashInSlot(s, period, storm, f)) continue;
    const double dt = seconds - f.at;
    const int strokes = f.strokes;
    const float angle = f.angle, far = f.far;
    float light = 0.0f;
    for (int k = 0; k < strokes; ++k) {
      const float amp = f.amp[k];
      // Each stroke swells in a few hundredths of a second and dies away over half a second, so a flash is one
      // light that swells and fades with a slight flicker. (Strokes that each died within a tenth of a second
      // lit the sky and the fog in sharp separate flashes, and the rain seen against them between the flashes
      // looked held or pulled up, as a wheel's spokes do under a strobe: "Rainfall still looks like it's being
      // pulled-up when lightning flashes", 14 Nov 2026.)
      const double since = dt - k * 0.12;
      if (since >= 0.0 && since < 1.5)
        light += amp * 0.6f * float((1.0 - std::exp(-since * 45.0)) * std::exp(-since * 6.0));
    }
    // D208: a strike far off lights the scene less (a quarter at 9 km), now that strikes are where the storm is.
    light *= 1.0f - 0.75f * Smooth(1500.0f, 9000.0f, far);
    if (light > 0.004f) {
      out.light = light;
      out.x = std::sin(angle) * far, out.z = std::cos(angle) * far;
      return out;
    }
  }
  return out;
}

// The same time of day as Apply's (the sun's elevation and what follows from it), for the game's own light.
SkyLight SkyLightAt(double hour_in, float weather) {
  const float hour = float(std::fmod(std::fmod(hour_in, 24.0) + 24.0, 24.0));
  const float el = 70.0f * std::sin(3.14159265f * (hour - 6.0f) / 12.0f);
  const float day = Smooth(-14.0f, 6.0f, el), direct = Smooth(-3.0f, 6.0f, el);
  const float ext = Lerp(0.5f, 1.0f, Smooth(0.0f, 15.0f, el));
  const float warm = (1.0f - Smooth(4.0f, 28.0f, el)) * Smooth(-9.0f, 0.0f, el);
  const float night = 1.0f - day;
  const Rgb noon{1.0f, 0.96f, 0.9f}, moon{0.62f, 0.75f, 1.0f};
  const Rgb sun = Mix(noon, {1.0f, 0.42f, 0.14f}, warm * 0.9f);
  const Rgb mid_day{0.4f, 0.54f, 0.8f};
  const Rgb mid = Mix(Mix(mid_day, {0.55f, 0.40f, 0.55f}, warm * 0.3f * day), {0.03f, 0.05f, 0.12f}, night);
  const float moon_up = Smooth(0.0f, 0.2f, std::sin(-el * 3.14159265f / 180.0f));
  // The moon's light on the ground against the sun's at noon: far more than the real 1 in 400,000, as a film's night.
  const float moonlight = 0.07f * night * moon_up;
  SkyLight out;
  out.sun_elevation = el;
  out.sun_turn = 15.0f * (hour - 12.0f);
  const float key[3] = {sun.r / noon.r, sun.g / noon.g, sun.b / noon.b}, m[3] = {moon.r, moon.g, moon.b};
  const float sky[3] = {mid.r / mid_day.r, mid.g / mid_day.g, mid.b / mid_day.b};
  for (int i = 0; i < 3; ++i) {
    out.key[i] = key[i] * direct * ext + m[i] * moonlight;
    out.sky[i] = sky[i];
    out.ground[i] = 0.5f * (out.key[i] + out.sky[i]);
  }
  // The painted sky is a picture of a blue afternoon: at dusk it is dimmed and pushed toward the sunset's colours
  // (run dn1: with the sky's light alone it stayed a day's sky over a dusk's ground).
  const float dusk[3] = {1.0f, 0.6f, 0.45f};
  for (int i = 0; i < 3; ++i) out.painted[i] = Lerp(out.sky[i], dusk[i], warm * 0.6f) * Lerp(1.0f, 0.6f, warm);
  // The weather: under an overcast the sun's light goes (a quarter is left), and the sky's turns grey and dimmer.
  const float w = std::clamp(weather, 0.0f, 1.0f);
  if (w > 0.0f) {
    const float sky_grey = (out.sky[0] + out.sky[1] + out.sky[2]) / 3.0f;
    const float painted_grey = (out.painted[0] + out.painted[1] + out.painted[2]) / 3.0f;
    for (int i = 0; i < 3; ++i) {
      out.key[i] *= 1.0f - 0.75f * w;
      out.sky[i] = Lerp(out.sky[i], sky_grey, 0.6f * w) * (1.0f - 0.3f * w);
      out.painted[i] = Lerp(out.painted[i], painted_grey * 0.9f, 0.7f * w) * (1.0f - 0.35f * w);
      out.ground[i] = 0.5f * (out.key[i] + out.sky[i]);
    }
  }
  return out;
}

int TestSkyShaders() {
  return int(CompileGlsl(GlslStage::kVertex, kVS, "sky VS").empty()) +
         int(CompileGlsl(GlslStage::kPixel, std::string(kCommon) + storm::kGlsl + kCloudBindings + kCloudShared + kCloudPS, "sky clouds PS").empty()) +
         int(CompileGlsl(GlslStage::kPixel, std::string(kCommon) + kResolvePS, "sky clouds resolve PS").empty()) +
         int(CompileGlsl(GlslStage::kPixel, std::string(kCommon) + kSkyPS, "sky PS").empty()) +
         int(CompileGlsl(GlslStage::kPixel, std::string(kCommon) + kOverPS, "sky clouds over PS").empty()) +
         int(CompileGlsl(GlslStage::kPixel, std::string(kCommon) + kFogShared + kFogClouds + storm::kGlsl + kCloudShared + kFogPS, "sky fog PS").empty()) +
         int(CompileGlsl(GlslStage::kPixel, std::string(kCommon) + kFogShared + kSeaPS, "sky sea PS").empty()) +
         int(CompileGlsl(GlslStage::kPixel, std::string(kCommon) + kLensPS, "sky lens PS").empty()) +
         int(CompileGlsl(GlslStage::kPixel, std::string(kCommon) + kWetPS, "sky wet PS").empty());
}

Sky::~Sky() {
  if (noise_thread_.joinable()) noise_thread_.join();
}

bool Sky::Load(IRenderDevice* device) {
  device_ = device;
  vs_ = MakeShader(device, SHADER_TYPE_VERTEX, kVS, "NativeXRenderer sky VS");
  cloud_ps_ = MakeShader(device, SHADER_TYPE_PIXEL, std::string(kCommon) + storm::kGlsl + kCloudBindings + kCloudShared + kCloudPS,
                         "NativeXRenderer sky clouds PS");
  resolve_ps_ = MakeShader(device, SHADER_TYPE_PIXEL, std::string(kCommon) + kResolvePS, "NativeXRenderer sky clouds resolve PS");
  sky_ps_ = MakeShader(device, SHADER_TYPE_PIXEL, std::string(kCommon) + kSkyPS, "NativeXRenderer sky PS");
  over_ps_ = MakeShader(device, SHADER_TYPE_PIXEL, std::string(kCommon) + kOverPS, "NativeXRenderer sky clouds over PS");
  fog_ps_ = MakeShader(device, SHADER_TYPE_PIXEL, std::string(kCommon) + kFogShared + kFogClouds + storm::kGlsl + kCloudShared + kFogPS,
                       "NativeXRenderer sky fog PS");
  sea_ps_ = MakeShader(device, SHADER_TYPE_PIXEL, std::string(kCommon) + kFogShared + kSeaPS, "NativeXRenderer sky sea PS");
  lens_ps_ = MakeShader(device, SHADER_TYPE_PIXEL, std::string(kCommon) + kLensPS, "NativeXRenderer sky lens PS");
  wet_ps_ = MakeShader(device, SHADER_TYPE_PIXEL, std::string(kCommon) + kWetPS, "NativeXRenderer sky wet PS");
  BufferDesc constants;
  constants.Name = "NativeXRenderer sky constants";
  constants.Size = sizeof(SkyConstantBlock);
  constants.Usage = USAGE_DYNAMIC;
  constants.BindFlags = BIND_UNIFORM_BUFFER;
  constants.CPUAccessFlags = CPU_ACCESS_WRITE;
  device->CreateBuffer(constants, nullptr, &constants_);
  if (!vs_ || !cloud_ps_ || !resolve_ps_ || !sky_ps_ || !over_ps_ || !fog_ps_ || !sea_ps_ || !lens_ps_ || !wet_ps_ || !constants_) {
    REXLOG_ERROR("[native_x_renderer] sky: its passes could not be set up");
    failed_ = true;
    return false;
  }
  noise_thread_ = std::thread([this] {
    const auto t0 = std::chrono::steady_clock::now();
    shape_mips_.push_back(ShapeVolume(kShapeSize, 1942));
    for (uint32_t size = kShapeSize; size > 4; size /= 2) shape_mips_.push_back(HalveVolume(shape_mips_.back(), size));
    detail_mips_.push_back(DetailVolume(kDetailSize, 1943));
    for (uint32_t size = kDetailSize; size > 4; size /= 2) detail_mips_.push_back(HalveVolume(detail_mips_.back(), size));
    weather_data_ = WeatherMap(kWeatherSize, 1944);
    uint64_t sum[4] = {};
    uint8_t lo[4] = {255, 255, 255, 255}, hi[4] = {};
    const auto& s0 = shape_mips_[0];
    for (size_t i = 0; i < s0.size(); ++i) sum[i & 3] += s0[i], lo[i & 3] = std::min(lo[i & 3], s0[i]), hi[i & 3] = std::max(hi[i & 3], s0[i]);
    const double n = double(s0.size() / 4);
    REXLOG_INFO("[native_x_renderer] sky: the cloud noise made in {:.1f} s; the shape's channels mean {:.0f} {:.0f} {:.0f} "
                "{:.0f}, from {} {} {} {} to {} {} {} {}",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), sum[0] / n, sum[1] / n,
                sum[2] / n, sum[3] / n, lo[0], lo[1], lo[2], lo[3], hi[0], hi[1], hi[2], hi[3]);
    noise_ready_.store(true, std::memory_order_release);
  });
  return true;
}

bool Sky::MakeTextures() {
  if (noise_thread_.joinable()) noise_thread_.join();
  auto volume = [&](const char* name, uint32_t size, const std::vector<std::vector<uint8_t>>& mips,
                    RefCntAutoPtr<ITexture>& out) {
    TextureDesc desc;
    desc.Name = name;
    desc.Type = RESOURCE_DIM_TEX_3D;
    desc.Width = desc.Height = desc.Depth = size;
    desc.Format = TEX_FORMAT_RGBA8_UNORM;
    desc.MipLevels = uint32_t(mips.size());
    desc.Usage = USAGE_IMMUTABLE;
    desc.BindFlags = BIND_SHADER_RESOURCE;
    std::vector<TextureSubResData> levels(mips.size());
    for (size_t i = 0; i < mips.size(); ++i) {
      const uint32_t n = size >> i;
      levels[i].pData = mips[i].data();
      levels[i].Stride = uint64_t(n) * 4;
      levels[i].DepthStride = uint64_t(n) * n * 4;
    }
    TextureData data(levels.data(), uint32_t(levels.size()));
    device_->CreateTexture(desc, &data, &out);
  };
  volume("NativeXRenderer sky shape noise", kShapeSize, shape_mips_, shape_);
  volume("NativeXRenderer sky detail noise", kDetailSize, detail_mips_, detail_);
  {
    TextureDesc desc;
    desc.Name = "NativeXRenderer sky weather map";
    desc.Type = RESOURCE_DIM_TEX_2D;
    desc.Width = desc.Height = kWeatherSize;
    desc.Format = TEX_FORMAT_RG8_UNORM;
    desc.MipLevels = 1;
    desc.Usage = USAGE_IMMUTABLE;
    desc.BindFlags = BIND_SHADER_RESOURCE;
    TextureSubResData level;
    level.pData = weather_data_.data();
    level.Stride = uint64_t(kWeatherSize) * 2;
    TextureData data(&level, 1);
    device_->CreateTexture(desc, &data, &weather_);
  }
  shape_mips_.clear(), detail_mips_.clear(), weather_data_.clear();
  return shape_ && detail_ && weather_;
}

bool Sky::Ready() {
  if (failed_ || !device_) return false;
  if (!noise_ready_.load(std::memory_order_acquire)) return false;
  if (!textures_made_) {
    textures_made_ = true;
    if (!MakeTextures()) {
      REXLOG_ERROR("[native_x_renderer] sky: the noise textures could not be made");
      failed_ = true;
      return false;
    }
  }
  return true;
}

// The constants every pass shares: the camera, the time of day's sun, moon and colours, the clouds, the fog.
static_assert(kBoltPieces * 2 == 144, "the fog shader's bolt_seg");

// D192: the bolt in its flash now, if a strike has one, laid out in the view's pixels for the fog's pass. NXR_BOLT=<m>
// (a developer's check): a bolt held on that far straight ahead of the camera.
void Sky::FillBolt(const Inputs& in, SkyConstantBlock& k, float weather, float sea, float scale) {
  k.bolt[0] = 0.0f;
  static const float held = [] {
    const char* v = std::getenv("NXR_BOLT");
    return v ? float(std::atof(v)) : 0.0f;
  }();
  float light = 0.0f, x = 0.0f, z = 0.0f;
  uint64_t seed = 0;
  bool crawler = false;  // D211: an in-cloud strike's channel
  // NXR_CRAWLER=<m> (a developer's check, D211): an in-cloud strike's channel held on that far straight ahead.
  static const float held_crawler = [] {
    const char* v = std::getenv("NXR_CRAWLER");
    return v ? float(std::atof(v)) : 0.0f;
  }();
  if (held > 0.0f) {
    const float ax = in.inverse[0][2], az = in.inverse[2][2];  // the view's middle, flat
    const float al = std::max(std::sqrt(ax * ax + az * az), 1e-4f);
    x = in.eye[0] + ax / al * held, z = in.eye[2] + az / al * held;
    light = 1.0f, seed = 0x1943b0170ull;
  } else if (held_crawler > 0.0f) {
    const float ax = in.inverse[0][2], az = in.inverse[2][2];
    const float al = std::max(std::sqrt(ax * ax + az * az), 1e-4f);
    x = in.eye[0] + ax / al * held_crawler, z = in.eye[2] + az / al * held_crawler;
    light = 0.7f, seed = 0x1943c4a7ull, crawler = true;
  } else if (g_lightning.load(std::memory_order_relaxed)) {
    const float storm = Smooth(0.6f, 0.95f, weather);
    if (storm <= 0.0f || in.seconds < 0.0) return;
    const double period = Lerp(16.0f, 5.0f, storm);
    const int64_t slot = int64_t(in.seconds / period);
    for (int64_t s = slot; s >= slot - 1 && s >= 0; --s) {
      SlotFlash f;
      if (!FlashInSlot(s, period, storm, f)) continue;
      // D211: a flash inside the clouds within 9 km shows its channel crawling along the cloud base.
      if (!f.bolt && f.far > 9000.0f) continue;
      light = BoltLight(f, in.seconds - f.at) * (f.bolt ? 1.0f : 0.9f);  // (D212: 0.9, 0.7 before)
      if (light <= 0.0f) continue;
      crawler = !f.bolt;
      // Where the sky lights it (Fill's flash, LightningStrikes): the strike's offset from the middle of the eye's
      // 4 km square.
      x = std::round(in.eye[0] / 4000.0f) * 4000.0f + std::sin(f.angle) * f.far;
      z = std::round(in.eye[2] / 4000.0f) * 4000.0f + std::cos(f.angle) * f.far;
      seed = f.shape;
      break;
    }
  }
  if (light <= 0.0f) return;
  // None seen from in or over the clouds (the layer would hide it).
  const float base = k.cloud[2] * scale;
  light *= 1.0f - Smooth(base * 0.85f, base, in.eye[1] - sea);
  if (light <= 0.0f) return;
  const float dist = std::sqrt((x - in.eye[0]) * (x - in.eye[0]) + (z - in.eye[2]) * (z - in.eye[2]));
  const float foot[3] = {x, sea, z};
  const float start[3] = {x, sea + base - 40.0f, z};
  const std::vector<BoltPiece> pieces = crawler ? CrawlerShape(seed, start, dist) : BoltShape(seed, foot, sea + base, dist);
  const float w = float(in.width), h = float(in.height);
  if (w < 1.0f || h < 1.0f || std::abs(in.to_ndc[0]) < 1e-6f || std::abs(in.to_ndc[1]) < 1e-6f) return;
  // The view's pixels a metre across makes 1 m along the view.
  const float* fy = in.forward[1];
  const float per_m = std::sqrt(fy[0] * fy[0] + fy[1] * fy[1] + fy[2] * fy[2]) / std::abs(in.to_ndc[1]) * h;
  auto clip = [&](const float p[3], float c[3]) {
    const float d[3] = {p[0] - in.eye[0], p[1] - in.eye[1], p[2] - in.eye[2]};
    for (int r = 0; r < 3; ++r) c[r] = in.forward[r][0] * d[0] + in.forward[r][1] * d[1] + in.forward[r][2] * d[2];
  };
  float box[4] = {1e9f, 1e9f, -1e9f, -1e9f};
  int n = 0;
  for (const BoltPiece& p : pieces) {
    float ca[3], cb[3];
    clip(p.a, ca), clip(p.b, cb);
    constexpr float kNear = 1.0f;
    if (ca[2] < kNear && cb[2] < kNear) continue;
    if (ca[2] < kNear || cb[2] < kNear) {  // cut at 1 m in front of the eye
      float* behind = ca[2] < kNear ? ca : cb;
      const float* front = ca[2] < kNear ? cb : ca;
      const float t = (kNear - behind[2]) / (front[2] - behind[2]);
      for (int c = 0; c < 3; ++c) behind[c] += (front[c] - behind[c]) * t;
    }
    const float pa[2] = {(ca[0] / ca[2] - in.to_ndc[2]) / in.to_ndc[0] * w, (ca[1] / ca[2] - in.to_ndc[3]) / in.to_ndc[1] * h};
    const float pb[2] = {(cb[0] / cb[2] - in.to_ndc[2]) / in.to_ndc[0] * w, (cb[1] / cb[2] - in.to_ndc[3]) / in.to_ndc[1] * h};
    const float reach = std::max(p.half * 2.6f * per_m / std::min(ca[2], cb[2]), 2.0f) * 3.0f + 2.0f;
    const float lo[2] = {std::min(pa[0], pb[0]) - reach, std::min(pa[1], pb[1]) - reach};
    const float hi[2] = {std::max(pa[0], pb[0]) + reach, std::max(pa[1], pb[1]) + reach};
    if (hi[0] < 0.0f || hi[1] < 0.0f || lo[0] > w || lo[1] > h) continue;  // off the view
    float* seg = k.bolt_seg[2 * n];
    seg[0] = pa[0], seg[1] = pa[1], seg[2] = pb[0], seg[3] = pb[1];
    float* more = k.bolt_seg[2 * n + 1];
    more[0] = ca[2], more[1] = cb[2], more[2] = p.half, more[3] = p.bright;
    box[0] = std::min(box[0], lo[0]), box[1] = std::min(box[1], lo[1]);
    box[2] = std::max(box[2], hi[0]), box[3] = std::max(box[3], hi[1]);
    ++n;
  }
  if (n == 0) return;
  k.bolt[0] = float(n), k.bolt[1] = light, k.bolt[2] = per_m;
  k.bolt_box[0] = std::max(box[0], 0.0f), k.bolt_box[1] = std::max(box[1], 0.0f);
  k.bolt_box[2] = std::min(box[2], w), k.bolt_box[3] = std::min(box[3], h);
  if (seed != said_bolt_) {
    said_bolt_ = seed;
    REXLOG_INFO("[native_x_renderer] sky: a lightning bolt {:.0f} m off, {} of its {} pieces in view", dist, n,
                pieces.size());
  }
}

void Sky::Fill(const Inputs& in, SkyConstantBlock& k) {
  const float hour = float(std::fmod(std::fmod(in.hour, 24.0) + 24.0, 24.0));
  const float kPi = 3.14159265f;
  const float el = 70.0f * std::sin(kPi * (hour - 6.0f) / 12.0f);  // the sun's elevation, degrees
  const float az = (in.sun_azimuth - 15.0f * (hour - 12.0f)) * kPi / 180.0f;
  const float ce = std::cos(el * kPi / 180.0f);
  const float to_sun[3] = {ce * std::sin(az), std::sin(el * kPi / 180.0f), ce * std::cos(az)};
  const float day = Smooth(-14.0f, 6.0f, el), direct = Smooth(-3.0f, 6.0f, el);
  const float ext = Lerp(0.5f, 1.0f, Smooth(0.0f, 15.0f, el));
  const float warm = (1.0f - Smooth(4.0f, 28.0f, el)) * Smooth(-9.0f, 0.0f, el);
  const float night = 1.0f - day;
  const Rgb kDuskSun{1.0f, 0.42f, 0.14f}, kDuskHaze{0.88f, 0.52f, 0.32f}, kDuskMid{0.55f, 0.40f, 0.55f},
      kDuskZenith{0.12f, 0.16f, 0.35f};
  const Rgb kNightFog{0.04f, 0.06f, 0.125f}, kNightHorizon{0.065f, 0.095f, 0.18f}, kNightMid{0.03f, 0.05f, 0.12f},
      kNightZenith{0.01f, 0.022f, 0.08f};
  const Rgb kFog{0.71f, 0.74f, 0.79f}, kHorizon{0.6f, 0.66f, 0.76f}, kMid{0.4f, 0.54f, 0.8f}, kZenith{0.24f, 0.45f, 0.82f};
  const Rgb kMoon{0.62f, 0.75f, 1.0f};
  const Rgb sun_color = Mix({1.0f, 0.96f, 0.9f}, kDuskSun, warm * 0.9f);
  const float moon_up = Smooth(0.0f, 0.2f, -to_sun[1]);
  const float moon_energy = 0.25f * night * moon_up;

  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) k.inv[r][c] = in.inverse[r][c];
  // The eye above the sea: the sea's height as the fog's pass found it (ReadSea), and until then taken as 30 m under
  // the lowest eye seen (a soldier's on a beach).
  if (in.eye[1] != 0.0f) lowest_eye_ = std::min(lowest_eye_, in.eye[1]);
  const float sea = sea_known_ ? sea_ : lowest_eye_ < 1e8f ? lowest_eye_ - 30.0f : in.eye[1] - 30.0f;
  k.eye[0] = in.eye[0], k.eye[2] = in.eye[2];
  k.eye[1] = std::max(in.eye[1] - sea, 2.0f) + in.cloud_lift;
  k.eye[3] = float(std::fmod(in.seconds, 3600.0));
  std::memcpy(k.to_ndc, in.to_ndc, sizeof(k.to_ndc));
  k.view[0] = float(in.width), k.view[1] = float(in.height), k.view[2] = in.exposure, k.view[3] = float(in.debug);
  for (int i = 0; i < 3; ++i) k.sun_dir[i] = to_sun[i], k.moon_dir[i] = -to_sun[i];
  const float weather = std::clamp(in.weather, 0.0f, 1.0f);
  k.sun_dir[3] = direct * ext * (1.0f - 0.75f * weather);
  StoreLinear(k.sun_color, sun_color, night);
  k.moon_dir[3] = 0.12f;  // the cirrus's cover (Beachhead's 0.25 streaked the whole sky: run sky10)
  // D211: the hurricane's outflow: the cirrus thickens as it comes (to 0.4 of the sky); altocumulus in patches,
  // more under a greyer sky, dimmer as the weather closes in.
  k.moon_dir[3] += 0.28f * Smooth(0.25f, 0.7f, in.match_weather);
  k.layers[0] = 0.22f + 0.3f * Smooth(0.1f, 0.6f, in.weather_outside), k.layers[1] = 1.0f - 0.35f * weather;
  k.layers[2] = g_cloud_shadows.load(std::memory_order_relaxed) ? 1.0f : 0.0f;  // D214: the clouds' shadows
  k.layers[3] = weather;  // D217: the weather here, for the clouds' shadows under an overcast
  StoreLinear(k.moon_color, kMoon, 0.0f);
  StoreLinear(k.moon_light, kMoon, 0.0f);
  for (int i = 0; i < 3; ++i) k.moon_light[i] *= moon_energy * 0.3f;
  // Under the weather the sky's colours go grey (their own brightness, a little less).
  auto grey = [weather](const Rgb& c) {
    const float l = (c.r + c.g + c.b) / 3.0f * 0.85f;
    return Mix(c, Rgb{l, l, l * 1.04f}, 0.75f * weather);
  };
  StoreLinear(k.fog, grey(Mix(Mix(kFog, kDuskHaze, warm * 0.5f * day), kNightFog, night)), 0.0f);
  StoreLinear(k.horizon, grey(Mix(Mix(kHorizon, kDuskHaze, warm * 0.55f * day), kNightHorizon, night)), 0.0f);
  StoreLinear(k.mid, grey(Mix(Mix(kMid, kDuskMid, warm * 0.3f * day), kNightMid, night)), 0.0f);
  StoreLinear(k.zenith, grey(Mix(Mix(kZenith, kDuskZenith, warm * 0.3f * day), kNightZenith, night)), 0.0f);
  // Fair weather: cumulus from 1500 m, 1800 m deep at the most; a light wind carries them.
  // The weather brings more cloud (toward a whole cover) and denser.
  // D208: the weather outside the hurricane; the clouds' shader adds the hurricane's own where it is.
  const float outside = std::clamp(in.storm_b[2] > 0.5f ? in.weather_outside : weather, 0.0f, 1.0f);
  k.cloud[0] = std::clamp(Lerp(in.coverage, 1.0f, 0.85f * outside), 0.0f, 1.0f);
  for (int i = 0; i < 4; ++i) k.storm_a[i] = in.storm_a[i], k.storm_b[i] = in.storm_b[i];
  {  // (D217, a check: what the sky's passes get of the storm, every 10 s)
    static auto said = std::chrono::steady_clock::time_point{};
    if (std::chrono::steady_clock::now() - said > std::chrono::seconds(10)) {
      said = std::chrono::steady_clock::now();
      REXLOG_INFO("[native_x_renderer] sky: the storm as the sky gets it: a {:.0f} {:.0f} {:.0f} {:.0f}, b {:.2f} {:.2f} {:.0f} "
                  "{:.2f}; the weather here {:.2f}, outside {:.2f}, the cover {:.2f}, the eye {:.0f} {:.0f} {:.0f}, scale {:.2f}",
                  in.storm_a[0], in.storm_a[1], in.storm_a[2], in.storm_a[3], in.storm_b[0], in.storm_b[1], in.storm_b[2],
                  in.storm_b[3], in.weather, in.weather_outside, k.cloud[0], in.eye[0], in.eye[1], in.eye[2], k.misc[2]);
    }
  }
  // Taller and fuller (the user, 16 Nov 2026: "more vertically dense. Right now they look thin and whispy"): the
  // layer 2800 m deep in its own scale (was 1800) and its clouds of a taller kind (0.62, was 0.5).
  k.cloud[1] = 0.62f, k.cloud[2] = 1500.0f, k.cloud[3] = 2800.0f;
  // In the world the layer is smaller by this (sky.h: the planes reach it), its base at the height set.
  const float scale = std::clamp(in.cloud_height, 100.0f, 3000.0f) / k.cloud[2];
  k.misc[2] = scale;
  k.drift[0] = float(std::fmod(in.seconds * 6.0, 50400.0)), k.drift[1] = float(std::fmod(in.seconds * 3.0, 50400.0));
  // How the clouds form and change: Beachhead's forming clock (atmosphere_view.gd's _evolve: 1 a second at its
  // 100% cloud generation speed), kept by the renderer at the rate set (renderer.cpp, World).
  k.drift[2] = float(std::fmod(in.evolve, 1000.0)), k.drift[3] = 1.0f + 0.8f * outside;

  // The fog. The distance fog's setting squared, to a sight of about 1 km at its most; the storm's mist adds to it
  // and brings a low layer of its own. The ground fog: to 28 m over the sea, a sight of about 60 m in its patches
  // at its most.
  const float mist = Smooth(0.45f, 1.0f, weather);
  const float fog = std::clamp(in.fog, 0.0f, 1.0f), ground = std::clamp(in.ground_fog, 0.0f, 1.0f);
  k.eye_world[0] = in.eye[0], k.eye_world[1] = in.eye[1], k.eye_world[2] = in.eye[2], k.eye_world[3] = sea;
  // (The mist stays low, 300 m: thicker or higher it hid the storm's clouds whole, run fog6.)
  k.fog_a[0] = fog * fog * 0.0035f + mist * 0.0006f;
  k.fog_a[1] = Lerp(500.0f, 300.0f, mist * (1.0f - fog));
  // D194: the storm's mist (the user, 18 Nov 2026: "low but thick Ground fog should be all over the map being kicked
  // up by that 100% storm weather"): from 75% of the weather, the ground fog thicker (to a sight of about 60 m in it
  // at the eye's height), lower (to 12 m) and everywhere (the shader fills its patches in and lays it on the ground).
  const float gale = Smooth(0.75f, 1.0f, weather);
  k.gale[0] = gale;
  k.fog_a[2] = ground * ground * 0.06f + mist * 0.0035f + gale * 0.022f;
  k.fog_a[3] = Lerp(Lerp(28.0f, 60.0f, mist * (1.0f - ground)), 12.0f, gale);
  k.fog_b[0] = in.depth_near;
  k.fog_b[1] = Smooth(0.45f, 0.9f, weather);
  k.fog_b[2] = sea_known_ ? 1.0f : 0.0f;
  // The rain here: from a little over half the weather; none over the clouds it falls from.
  // (D212: the eye's height over the sea, as the cloud base is set: the user, 19 Nov 2026: "rain also shouldn't show
  // when the camera is above the clouds". Taken over the world's 0 before, which on Guadalcanal, its sea at 173 m,
  // put the fade 173 m low.)
  k.fog_b[3] = Smooth(0.55f, 0.95f, weather) *
               (1.0f - Smooth(k.cloud[2] * scale, (k.cloud[2] + 0.3f * k.cloud[3]) * scale, k.eye[1] - sea));
  // The rain's lean: the wind's setting, to 35 degrees at its most. Its cover.
  // A strong storm drives it far harder (the user, 17 Nov 2026: "Wind setting should also drastically affect rain
  // slant at 100% storm strength"): from 80% of the weather the lean grows to 2.1 (65 degrees) at full wind.
  k.rain[0] = std::clamp(in.wind, 0.0f, 1.0f) * (0.7f + 1.4f * Smooth(0.8f, 1.0f, weather));
  k.rain[1] = in.cover ? 1.0f : 0.0f;
  k.ssr[0] = SeaReflection() ? 1.0f : 0.0f;
  k.ssr[1] = std::clamp(in.wind, 0.0f, 1.0f);
  k.ssr[2] = sea_mask ? (sea_after ? 2.0f : 1.0f) : 0.0f;
  static const bool sea_debug = [] {
    const char* v = std::getenv("NXR_SEA_DEBUG");
    return v && *v == '1';
  }();
  if (sea_debug) k.ssr[2] += 10.0f;
  k.ssr[3] = sea_mask_uv[0];
  std::memcpy(k.cover_m, in.cover_m, sizeof(k.cover_m));
  // The lightning, in the middle of the cloud layer.
  const SkyFlash flash = SkyFlashAt(in.seconds, std::max(in.match_weather, weather));  // D208
  k.flash[0] = std::round(in.eye[0] / 4000.0f) * 4000.0f + flash.x;
  k.flash[1] = (k.cloud[2] + 0.45f * k.cloud[3]) * scale;
  k.flash[2] = std::round(in.eye[2] / 4000.0f) * 4000.0f + flash.z;
  k.flash[3] = flash.light;
  FillBolt(in, k, std::max(in.match_weather, weather), sea, scale);  // D208
  last_fill_.assign(reinterpret_cast<const uint8_t*>(&k), reinterpret_cast<const uint8_t*>(&k) + sizeof(k));
  if (!said_) {
    said_ = true;
    REXLOG_INFO("[native_x_renderer] sky: the view {}x{}; {:.1f} h: the sun {:.0f} degrees up, night {:.2f}; the eye at "
                "{:.0f}, {:.0f}, {:.0f}",
                in.width, in.height, hour, el, night, in.eye[0], in.eye[1], in.eye[2]);
  }
}

bool Sky::FogWanted(const Inputs& in) {
  return in.fog > 0.001f || in.ground_fog > 0.001f || in.weather > 0.45f || in.debug == 4 || in.debug == 5 || in.debug == 6 ||
         SeaReflection();
}

// The sea's height from the small target's copy (made a few frames ago): the view's lowest heights; where the lowest
// of them lie flat (the 5th and the 20th of a hundred within a metre), that is the sea, or a deck or an airfield
// with no sea in sight. The lowest such seen in the last minutes is taken.
void Sky::ReadSea(IDeviceContext* context) {
  MappedTextureSubresource mapped;
  context->MapTextureSubresource(sea_staging_, 0, 0, MAP_READ, MAP_FLAG_DO_NOT_WAIT, nullptr, mapped);
  static int said = 0;
  if (!mapped.pData) {
    if (said++ < 3) REXLOG_INFO("[native_x_renderer] sky: the sea's heights could not be read");
    return;
  }
  std::vector<float> seen;
  for (uint32_t y = 0; y < 18; ++y) {
    const float* row = reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped.pData) + y * mapped.Stride);
    if (y == 17 && row[31] > 1.5e9f) {
      eye_open_ = row[31] < 2.5e9f;
      NoteEyeCovered(!eye_open_);  // (the weather's sounds: rain muffled under cover)
    }
    for (uint32_t x = 0; x < 32; ++x)
      if (std::isfinite(row[x]) && row[x] < 1e8f) seen.push_back(row[x]);
  }
  context->UnmapTextureSubresource(sea_staging_, 0, 0);
  std::sort(seen.begin(), seen.end());
  if (seen.size() < 40) return;
  const float low = seen[seen.size() / 20], more = seen[seen.size() / 5];
  if (more - low > 1.0f) return;
  sea_seen_.push_back(low);
  if (sea_seen_.size() > 240) sea_seen_.pop_front();
  const float sea = *std::min_element(sea_seen_.begin(), sea_seen_.end());
  if (!sea_known_ || std::abs(sea - sea_) > 0.5f)
    REXLOG_INFO("[native_x_renderer] sky: the sea's height taken as {:.1f} (the lowest flat ground in sight now {:.1f})", sea, low);
  sea_ = sea, sea_known_ = true;
}

bool Sky::Fog(IDeviceContext* context, const Inputs& in) {
  if (!in.color || !in.depth_srv || !in.width || !in.height || !Ready()) return false;
  const TextureDesc& color = in.color->GetDesc();
  SamplerDesc repeat, point;
  repeat.MinFilter = repeat.MagFilter = repeat.MipFilter = FILTER_TYPE_LINEAR;
  repeat.AddressU = repeat.AddressV = repeat.AddressW = TEXTURE_ADDRESS_WRAP;
  point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
  point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
  auto pipeline = [&](const char* name, IShader* ps, TEXTURE_FORMAT format, bool blend, RefCntAutoPtr<IPipelineState>& pso,
                      RefCntAutoPtr<IShaderResourceBinding>& srb) {
    SamplerDesc clamp_linear;
    clamp_linear.MinFilter = clamp_linear.MagFilter = clamp_linear.MipFilter = FILTER_TYPE_LINEAR;
    clamp_linear.AddressU = clamp_linear.AddressV = clamp_linear.AddressW = TEXTURE_ADDRESS_CLAMP;
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "weatherTex", repeat},
                                       {SHADER_TYPE_PIXEL, "depthTex", point},
                                       {SHADER_TYPE_PIXEL, "coverTex", point},
                                       {SHADER_TYPE_PIXEL, "sceneTex", clamp_linear},
                                       {SHADER_TYPE_PIXEL, "seaMaskTex", point},
                                       {SHADER_TYPE_PIXEL, "seaAfterTex", point},
                                       {SHADER_TYPE_PIXEL, "seaFloorTex", point},
                                       {SHADER_TYPE_PIXEL, "shapeTex", repeat},   // D214
                                       {SHADER_TYPE_PIXEL, "detailTex", repeat}};
    GraphicsPipelineStateCreateInfo info;
    info.PSODesc.Name = name;
    info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
    info.GraphicsPipeline.NumRenderTargets = 1;
    info.GraphicsPipeline.RTVFormats[0] = format;
    info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
    info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
    info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
    if (blend) {  // the fog's light added, what is there kept by what shows through; its alpha left alone
      auto& rt = info.GraphicsPipeline.BlendDesc.RenderTargets[0];
      rt.BlendEnable = True;
      rt.SrcBlend = BLEND_FACTOR_ONE, rt.DestBlend = BLEND_FACTOR_SRC_ALPHA, rt.BlendOp = BLEND_OPERATION_ADD;
      rt.SrcBlendAlpha = BLEND_FACTOR_ZERO, rt.DestBlendAlpha = BLEND_FACTOR_ONE, rt.BlendOpAlpha = BLEND_OPERATION_ADD;
    }
    info.pVS = vs_;
    info.pPS = ps;
    info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
    info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    info.PSODesc.ResourceLayout.NumImmutableSamplers = 9;
    device_->CreateGraphicsPipelineState(info, &pso);
    if (pso) pso->CreateShaderResourceBinding(&srb, true);
  };
  if (!no_cover_) {  // what the cover's place holds without a shadow map: three layers of nothing
    TextureDesc desc;
    desc.Name = "NativeXRenderer sky no cover";
    desc.Type = RESOURCE_DIM_TEX_2D_ARRAY;
    desc.Width = desc.Height = 1;
    desc.ArraySize = 3;
    desc.Format = TEX_FORMAT_R32_FLOAT;
    desc.Usage = USAGE_IMMUTABLE;
    desc.BindFlags = BIND_SHADER_RESOURCE;
    const float far_away = 1e9f;
    TextureSubResData layers[3];
    for (auto& l : layers) l.pData = &far_away, l.Stride = 4;
    TextureData data(layers, 3);
    device_->CreateTexture(desc, &data, &no_cover_);
    if (!no_cover_) return false;
  }
  if (!no_scene_) {  // D190: the scene's place before it is copied: one black texel
    TextureDesc desc;
    desc.Name = "NativeXRenderer sky no scene";
    desc.Type = RESOURCE_DIM_TEX_2D;
    desc.Width = desc.Height = 1;
    desc.Format = TEX_FORMAT_RGBA8_UNORM;
    desc.Usage = USAGE_IMMUTABLE;
    desc.BindFlags = BIND_SHADER_RESOURCE;
    const uint32_t black = 0;
    TextureSubResData texel{&black, 4};
    TextureData data(&texel, 1);
    device_->CreateTexture(desc, &data, &no_scene_);
  }
  if (!fog_pso_ || fog_format_ != color.Format) {
    fog_pso_.Release(), fog_srb_.Release();
    pipeline("NativeXRenderer sky fog", fog_ps_, color.Format, true, fog_pso_, fog_srb_);
    fog_format_ = color.Format;
  }
  if (!sea_pso_) {
    pipeline("NativeXRenderer sky sea", sea_ps_, TEX_FORMAT_R32_FLOAT, false, sea_pso_, sea_srb_);
    TextureDesc desc;
    desc.Name = "NativeXRenderer sky sea heights";
    desc.Type = RESOURCE_DIM_TEX_2D;
    desc.Width = 32, desc.Height = 18;
    desc.Format = TEX_FORMAT_R32_FLOAT;
    desc.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
    device_->CreateTexture(desc, nullptr, &sea_target_);
    desc.Name = "NativeXRenderer sky sea heights, read";
    desc.BindFlags = BIND_NONE;
    desc.Usage = USAGE_STAGING;
    desc.CPUAccessFlags = CPU_ACCESS_READ;
    device_->CreateTexture(desc, nullptr, &sea_staging_);
  }
  if (!fog_srb_ || !sea_srb_ || !sea_target_ || !sea_staging_) {
    REXLOG_ERROR("[native_x_renderer] sky: no fog pipeline (the view's format {})", int(color.Format));
    failed_ = true;
    return false;
  }
  SkyConstantBlock k{};
  Fill(in, k);
  {
    // How wet the lens is: it builds up in two seconds of rain on the eye, and dries in four under cover or once
    // the rain stops.
    const auto now = std::chrono::steady_clock::now();
    const float dt = lens_at_.time_since_epoch().count() ? std::min(std::chrono::duration<float>(now - lens_at_).count(), 0.25f) : 0.0f;
    lens_at_ = now;
    lens_rain_ = k.fog_b[3];
    lens_seconds_ = in.seconds;
    const float aim = eye_open_ ? lens_rain_ : 0.0f;
    // D189: a strong storm's sway (the user, 17 Nov 2026: "a minor screen wobble/lean and slight vignette blur on edges
    // of player camera that sells the effect of a strong storm"): from 80% of the weather and a fresh wind up. D193:
    // only on foot (SetEyeOnFoot) and in the open, none under cover or in a vehicle; two seconds to come, half a
    // second to go.
    {
      const float storm = Smooth(0.8f, 1.0f, in.weather) * Smooth(0.35f, 1.0f, in.wind);
      const bool out_on_foot = eye_open_ && g_eye_on_foot.load(std::memory_order_relaxed);
      const float sway_aim = g_storm_sway.load(std::memory_order_relaxed) && out_on_foot ? storm : 0.0f;
      sway_ += std::clamp(sway_aim - sway_, -dt / 0.5f, dt / 2.0f);
      // D194: the blurred edges in the open whether on foot or in a vehicle; none under a roof.
      const float edge_aim = g_storm_sway.load(std::memory_order_relaxed) && eye_open_ ? storm : 0.0f;
      edge_ += std::clamp(edge_aim - edge_, -dt / 0.5f, dt / 2.0f);
      static bool shaking = false;
      if ((sway_ > 0.5f) != shaking) {
        shaking = !shaking;
        REXLOG_INFO("[native_x_renderer] sky: the storm's shake {} (on foot {}, in the open {})", shaking ? "on" : "off",
                    g_eye_on_foot.load(std::memory_order_relaxed), eye_open_);
      }
    }
    lens_wet_ = aim > lens_wet_ ? std::min(aim, lens_wet_ + dt / 2.0f) : std::max(aim, lens_wet_ - dt / 4.0f);
    // (While it gets wetter the drops come one by one; drying, all there are fade together.)
    if (lens_wet_ >= lens_peak_ || lens_wet_ <= 0.0f || (aim > 0.0f && lens_wet_ == aim)) lens_peak_ = lens_wet_;
    // Water splashed on it (sky.h): by the eye's height over the sea and how fast it moves along the ground.
    splash_wet_ = std::max(0.0f, splash_wet_ - dt / splash_dry_);
    if (splash_wet_ <= 0.0f) splash_peak_ = 0.0f;
    if (sea_known_ && last_eye_valid_ && dt > 0.0f) {
      const float over = in.eye[1] - sea_;
      const float dx = in.eye[0] - last_eye_[0], dz = in.eye[2] - last_eye_[2];
      const float speed = std::sqrt(dx * dx + dz * dz) / dt;
      if (over < 0.3f && over > -30.0f) {
        splash_wet_ = splash_peak_ = 1.0f, splash_dry_ = 5.0f;
      } else if (over < 1.1f && speed > 1.5f && speed < 40.0f && splash_wet_ < 0.6f) {
        splash_wet_ = splash_peak_ = 0.6f, splash_dry_ = 3.0f / 0.6f;
      }
    }
    std::memcpy(last_eye_, in.eye, sizeof(last_eye_));
    last_eye_valid_ = true;
    // What is held in the hand (sky.h): wet in twelve seconds of rain, at once under the sea; dry in thirty.
    if (splash_wet_ >= 1.0f) weapon_wet_ = 1.0f;
    weapon_wet_ = aim > weapon_wet_ ? std::min(aim, weapon_wet_ + dt / 12.0f) : std::max(aim, weapon_wet_ - dt / 30.0f);
    if (weapon_wet_ >= weapon_peak_ || weapon_wet_ <= 0.0f) weapon_peak_ = weapon_wet_;
    // NXR_LENS_SPLASH=1 (a developer's check): soaked again every six seconds.
    static const bool test = [] {
      const char* v = std::getenv("NXR_LENS_SPLASH");
      return v && *v == '1';
    }();
    if (test && std::fmod(in.seconds, 6.0) < 0.1) splash_wet_ = splash_peak_ = 1.0f, splash_dry_ = 5.0f;
  }
  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, &k, sizeof(k));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }
  auto draw = [&](ITextureView* target, uint32_t width, uint32_t height, IPipelineState* pso, IShaderResourceBinding* srb) {
    if (auto* v = srb->GetVariableByName(SHADER_TYPE_PIXEL, "SkyConstants")) v->Set(constants_);
    if (auto* v = srb->GetVariableByName(SHADER_TYPE_PIXEL, "weatherTex")) v->Set(weather_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    if (auto* v = srb->GetVariableByName(SHADER_TYPE_PIXEL, "depthTex")) v->Set(in.depth_srv);
    if (auto* v = srb->GetVariableByName(SHADER_TYPE_PIXEL, "coverTex"))
      v->Set(in.cover ? in.cover : no_cover_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    context->SetRenderTargets(1, &target, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Viewport viewport;
    viewport.Width = float(width);
    viewport.Height = float(height);
    viewport.MaxDepth = 1.0f;
    context->SetViewports(1, &viewport, 0, 0);
    context->SetPipelineState(pso);
    context->CommitShaderResources(srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    DrawAttribs attribs;
    attribs.NumVertices = 3;
    context->Draw(attribs);
  };
  // The sea's height: the lowest heights drawn and copied out every half second or so, read four frames later.
  if (sea_wait_ > 0) {
    if (--sea_wait_ == 0) ReadSea(context);
  } else if (fog_frames_ % 30 == 0) {
    draw(sea_target_->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET), 32, 18, sea_pso_, sea_srb_);
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
    CopyTextureAttribs copy(sea_target_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, sea_staging_,
                            RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->CopyTexture(copy);
    sea_wait_ = 4;
  }
  const bool probed = sea_wait_ == 4;
  ++fog_frames_;
  // Without fog, weather or rain the pass only keeps the sea's height and the lens.
  if (!FogWanted(in)) return probed;
  ITextureView* target = in.color->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
  if (!target) return probed;
  // D190: the scene as it is, for the sea's reflection (only while that is on).
  if (SeaReflection()) {
    const TextureDesc& cd = in.color->GetDesc();
    if (!fog_scene_ || fog_scene_->GetDesc().Width != cd.Width || fog_scene_->GetDesc().Height != cd.Height ||
        fog_scene_->GetDesc().Format != cd.Format) {
      fog_scene_.Release();
      TextureDesc copy;
      copy.Name = "NativeXRenderer sky fog, the scene for the sea's reflection";
      copy.Type = RESOURCE_DIM_TEX_2D;
      copy.Width = cd.Width, copy.Height = cd.Height;
      copy.Format = cd.Format;
      copy.BindFlags = BIND_SHADER_RESOURCE;
      device_->CreateTexture(copy, nullptr, &fog_scene_);
    }
    if (fog_scene_) {
      context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
      CopyTextureAttribs copy(in.color, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, fog_scene_,
                              RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      context->CopyTexture(copy);
    }
  }
  if (auto* v = fog_srb_->GetVariableByName(SHADER_TYPE_PIXEL, "seaFloorTex")) {
    ITextureView* none = no_scene_ ? no_scene_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
    if (sea_floor) v->Set(sea_floor);
    else if (none) v->Set(none);
  }
  if (auto* v = fog_srb_->GetVariableByName(SHADER_TYPE_PIXEL, "seaAfterTex")) {
    ITextureView* none = no_scene_ ? no_scene_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
    if (sea_after) v->Set(sea_after);
    else if (none) v->Set(none);
  }
  if (auto* v = fog_srb_->GetVariableByName(SHADER_TYPE_PIXEL, "seaMaskTex")) {
    ITextureView* none = no_scene_ ? no_scene_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
    if (sea_mask) v->Set(sea_mask);
    else if (none) v->Set(none);
  }
  if (auto* v = fog_srb_->GetVariableByName(SHADER_TYPE_PIXEL, "sceneTex")) {
    ITexture* scene = fog_scene_ ? fog_scene_.RawPtr() : no_scene_.RawPtr();
    if (scene) v->Set(scene->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  }
  if (auto* v = fog_srb_->GetVariableByName(SHADER_TYPE_PIXEL, "shapeTex")) v->Set(shape_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));  // D214
  if (auto* v = fog_srb_->GetVariableByName(SHADER_TYPE_PIXEL, "detailTex")) v->Set(detail_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  draw(target, in.width, in.height, fog_pso_, fog_srb_);
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  return true;
}

bool Sky::CloudsOver(IDeviceContext* context, const Inputs& in) {
  if (!clouds_fresh_) return false;
  clouds_fresh_ = false;
  if (!in.color || !in.depth_srv || !in.width || !in.height || !Ready() || !history_[history_now_] ||
      !where_history_[history_now_]) {
    return false;
  }
  const TextureDesc& color = in.color->GetDesc();
  if (!over_pso_ || over_format_ != color.Format) {
    over_pso_.Release(), over_srb_.Release();
    SamplerDesc clamp, point;
    clamp.MinFilter = clamp.MagFilter = clamp.MipFilter = FILTER_TYPE_LINEAR;
    clamp.AddressU = clamp.AddressV = clamp.AddressW = TEXTURE_ADDRESS_CLAMP;
    point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
    point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "weatherTex", clamp},
                                       {SHADER_TYPE_PIXEL, "depthTex", point},
                                       {SHADER_TYPE_PIXEL, "cloudTex", clamp},
                                       {SHADER_TYPE_PIXEL, "whereTex", clamp}};
    GraphicsPipelineStateCreateInfo info;
    info.PSODesc.Name = "NativeXRenderer sky clouds over";
    info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
    info.GraphicsPipeline.NumRenderTargets = 1;
    info.GraphicsPipeline.RTVFormats[0] = color.Format;
    info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
    info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
    info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
    auto& rt = info.GraphicsPipeline.BlendDesc.RenderTargets[0];  // the cloud's light added, the rest kept by what shows through
    rt.BlendEnable = True;
    rt.SrcBlend = BLEND_FACTOR_ONE, rt.DestBlend = BLEND_FACTOR_SRC_ALPHA, rt.BlendOp = BLEND_OPERATION_ADD;
    rt.SrcBlendAlpha = BLEND_FACTOR_ZERO, rt.DestBlendAlpha = BLEND_FACTOR_ONE, rt.BlendOpAlpha = BLEND_OPERATION_ADD;
    info.pVS = vs_;
    info.pPS = over_ps_;
    info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
    info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    info.PSODesc.ResourceLayout.NumImmutableSamplers = 4;
    device_->CreateGraphicsPipelineState(info, &over_pso_);
    if (over_pso_) over_pso_->CreateShaderResourceBinding(&over_srb_, true);
    over_format_ = color.Format;
    if (!over_srb_) {
      REXLOG_ERROR("[native_x_renderer] sky: no pipeline for the clouds over the view (its format {})", int(color.Format));
      return false;
    }
  }
  if (!over_srb_) return false;
  ITextureView* target = in.color->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
  if (!target) return false;
  SkyConstantBlock k{};
  Fill(in, k);
  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, &k, sizeof(k));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }
  auto set = [&](const char* name, IDeviceObject* object) {
    if (auto* v = over_srb_->GetVariableByName(SHADER_TYPE_PIXEL, name)) v->Set(object);
  };
  set("SkyConstants", constants_);
  set("weatherTex", weather_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  set("depthTex", in.depth_srv);
  set("cloudTex", history_[history_now_]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  set("whereTex", where_history_[history_now_]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  context->SetRenderTargets(1, &target, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  Viewport viewport;
  viewport.Width = float(in.width);
  viewport.Height = float(in.height);
  viewport.MaxDepth = 1.0f;
  context->SetViewports(1, &viewport, 0, 0);
  context->SetPipelineState(over_pso_);
  context->CommitShaderResources(over_srb_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DrawAttribs attribs;
  attribs.NumVertices = 3;
  context->Draw(attribs);
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  return true;
}

bool Sky::Apply(IDeviceContext* context, const Inputs& in) {
  if (!in.color || !in.depth_srv || !in.width || !in.height || !Ready()) return false;
  const TextureDesc& color = in.color->GetDesc();
  SamplerDesc repeat, clamp, point;
  repeat.MinFilter = repeat.MagFilter = repeat.MipFilter = FILTER_TYPE_LINEAR;
  repeat.AddressU = repeat.AddressV = repeat.AddressW = TEXTURE_ADDRESS_WRAP;
  clamp.MinFilter = clamp.MagFilter = clamp.MipFilter = FILTER_TYPE_LINEAR;
  clamp.AddressU = clamp.AddressV = clamp.AddressW = TEXTURE_ADDRESS_CLAMP;
  point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
  point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
  uint32_t targets = 1;  // the clouds' passes draw two: the clouds, and where they lie along the ray
  auto pipeline = [&](const char* name, IShader* ps, TEXTURE_FORMAT format, const ImmutableSamplerDesc* samplers,
                      uint32_t count, RefCntAutoPtr<IPipelineState>& pso, RefCntAutoPtr<IShaderResourceBinding>& srb) {
    GraphicsPipelineStateCreateInfo info;
    info.PSODesc.Name = name;
    info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
    info.GraphicsPipeline.NumRenderTargets = uint8_t(targets);
    for (uint32_t i = 0; i < targets; ++i) info.GraphicsPipeline.RTVFormats[i] = format;
    info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
    info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
    info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
    info.pVS = vs_;
    info.pPS = ps;
    info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
    info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    info.PSODesc.ResourceLayout.NumImmutableSamplers = count;
    device_->CreateGraphicsPipelineState(info, &pso);
    if (pso) pso->CreateShaderResourceBinding(&srb, true);
  };
  if (!cloud_pso_) {
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "shapeTex", repeat},
                                       {SHADER_TYPE_PIXEL, "detailTex", repeat},
                                       {SHADER_TYPE_PIXEL, "weatherTex", repeat},
                                       {SHADER_TYPE_PIXEL, "cloudDepthTex", point}};
    targets = 2;
    pipeline("NativeXRenderer sky clouds", cloud_ps_, TEX_FORMAT_RGBA16_FLOAT, samplers, 4, cloud_pso_, cloud_srb_);
  }
  if (!resolve_pso_) {
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "weatherTex", repeat},
                                       {SHADER_TYPE_PIXEL, "cloudTex", clamp},
                                       {SHADER_TYPE_PIXEL, "historyTex", clamp},
                                       {SHADER_TYPE_PIXEL, "whereTex", clamp},
                                       {SHADER_TYPE_PIXEL, "whereHistoryTex", clamp}};
    targets = 2;
    pipeline("NativeXRenderer sky clouds resolve", resolve_ps_, TEX_FORMAT_RGBA16_FLOAT, samplers, 5, resolve_pso_, resolve_srb_);
  }
  targets = 1;
  if (!sky_pso_ || format_ != color.Format) {
    sky_pso_.Release(), sky_srb_.Release();
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "weatherTex", repeat},
                                       {SHADER_TYPE_PIXEL, "depthTex", point},
                                       {SHADER_TYPE_PIXEL, "cloudTex", clamp},
                                       {SHADER_TYPE_PIXEL, "shapeDebugTex", repeat},
                                       {SHADER_TYPE_PIXEL, "detailDebugTex", repeat}};
    pipeline("NativeXRenderer sky", sky_ps_, color.Format, samplers, 5, sky_pso_, sky_srb_);
    format_ = color.Format;
  }
  if (!cloud_srb_ || !resolve_srb_ || !sky_srb_) {
    REXLOG_ERROR("[native_x_renderer] sky: no pipeline (the view's format {})", int(color.Format));
    failed_ = true;
    return false;
  }
  // The clouds at half the view's size.
  const uint32_t cw = std::max(in.width / 2, 1u), ch = std::max(in.height / 2, 1u);
  if (!clouds_ || clouds_->GetDesc().Width != cw || clouds_->GetDesc().Height != ch) {
    clouds_.Release();
    TextureDesc desc;
    desc.Name = "NativeXRenderer sky clouds";
    desc.Type = RESOURCE_DIM_TEX_2D;
    desc.Width = cw;
    desc.Height = ch;
    desc.Format = TEX_FORMAT_RGBA16_FLOAT;
    desc.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
    device_->CreateTexture(desc, nullptr, &clouds_);
    for (auto& h : history_) {
      h.Release();
      desc.Name = "NativeXRenderer sky clouds over frames";
      device_->CreateTexture(desc, nullptr, &h);
    }
    where_.Release();
    desc.Name = "NativeXRenderer sky clouds, where";
    device_->CreateTexture(desc, nullptr, &where_);
    for (auto& h : where_history_) {
      h.Release();
      desc.Name = "NativeXRenderer sky clouds, where, over frames";
      device_->CreateTexture(desc, nullptr, &h);
    }
    history_valid_ = false;
    if (!clouds_ || !history_[0] || !history_[1] || !where_ || !where_history_[0] || !where_history_[1]) {
      failed_ = true;
      return false;
    }
  }

  SkyConstantBlock k{};
  Fill(in, k);
  k.bolt[3] = in.skip_hidden_clouds ? 1.0f : 0.0f;  // the march passes over the rays the scene hides
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) k.last[r][c] = last_forward_[r][c];
  std::memcpy(k.last_to_ndc, last_to_ndc_, sizeof(k.last_to_ndc));
  k.misc[0] = float(frames_++ % 1024), k.misc[1] = history_valid_ ? 1.0f : 0.0f;
  {
    // How far the eye went through the layer since the last frame (at the layer's own scale): the blend's share of
    // this frame, a tenth standing still, more the faster it flies.
    float moved = 0.0f;
    if (cloud_eye_valid_) {
      const float dx = in.eye[0] - cloud_eye_[0], dy = in.eye[1] - cloud_eye_[1], dz = in.eye[2] - cloud_eye_[2];
      moved = std::sqrt(dx * dx + dy * dy + dz * dz) / std::max(k.misc[2], 1e-3f);
    }
    std::memcpy(cloud_eye_, in.eye, sizeof(cloud_eye_));
    cloud_eye_valid_ = true;
    k.misc[3] = std::clamp(0.1f + moved / 20.0f, 0.1f, 0.6f);
  }
  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, &k, sizeof(k));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }
  auto draw = [&](ITextureView* target, uint32_t width, uint32_t height, IPipelineState* pso, IShaderResourceBinding* srb) {
    context->SetRenderTargets(1, &target, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Viewport viewport;
    viewport.Width = float(width);
    viewport.Height = float(height);
    viewport.MaxDepth = 1.0f;
    context->SetViewports(1, &viewport, 0, 0);
    context->SetPipelineState(pso);
    context->CommitShaderResources(srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    DrawAttribs attribs;
    attribs.NumVertices = 3;
    context->Draw(attribs);
  };
  auto draw2 = [&](ITextureView* first, ITextureView* second, uint32_t width, uint32_t height, IPipelineState* pso,
                   IShaderResourceBinding* srb) {
    ITextureView* both[2] = {first, second};
    context->SetRenderTargets(2, both, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Viewport viewport;
    viewport.Width = float(width);
    viewport.Height = float(height);
    viewport.MaxDepth = 1.0f;
    context->SetViewports(1, &viewport, 0, 0);
    context->SetPipelineState(pso);
    context->CommitShaderResources(srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    DrawAttribs attribs;
    attribs.NumVertices = 3;
    context->Draw(attribs);
  };
  auto set = [](IShaderResourceBinding* srb, const char* name, IDeviceObject* object) {
    if (auto* v = srb->GetVariableByName(SHADER_TYPE_PIXEL, name)) v->Set(object);
  };
  if (in.debug != 1) {
    set(cloud_srb_, "SkyConstants", constants_);
    set(cloud_srb_, "shapeTex", shape_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    set(cloud_srb_, "detailTex", detail_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    set(cloud_srb_, "weatherTex", weather_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    set(cloud_srb_, "cloudDepthTex", in.depth_srv);
    draw2(clouds_->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET), where_->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET), cw, ch,
          cloud_pso_, cloud_srb_);
    // Blended with the frames before, into the other of the two kept textures.
    history_now_ ^= 1;
    set(resolve_srb_, "SkyConstants", constants_);
    set(resolve_srb_, "cloudTex", clouds_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    set(resolve_srb_, "historyTex", history_[history_now_ ^ 1]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    set(resolve_srb_, "whereTex", where_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    set(resolve_srb_, "whereHistoryTex", where_history_[history_now_ ^ 1]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    draw2(history_[history_now_]->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET),
          where_history_[history_now_]->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET), cw, ch, resolve_pso_, resolve_srb_);
    history_valid_ = true;
    clouds_fresh_ = in.debug == 0;
    std::memcpy(last_forward_, in.forward, sizeof(last_forward_));
    std::memcpy(last_to_ndc_, in.to_ndc, sizeof(last_to_ndc_));
  }
  ITextureView* target = in.color->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
  if (!target) return false;
  set(sky_srb_, "SkyConstants", constants_);
  set(sky_srb_, "weatherTex", weather_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  set(sky_srb_, "depthTex", in.depth_srv);
  set(sky_srb_, "cloudTex", history_[history_now_]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  set(sky_srb_, "shapeDebugTex", shape_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  set(sky_srb_, "detailDebugTex", detail_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  // The view's part of the colour only (its texture may be larger).
  draw(target, in.width, in.height, sky_pso_, sky_srb_);
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  return true;
}

}  // namespace native_x_renderer
