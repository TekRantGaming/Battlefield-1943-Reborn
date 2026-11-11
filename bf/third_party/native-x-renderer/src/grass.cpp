// NativeXRenderer - see grass.h (a developer prototype).

#include "grass.h"

#include "glsl.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

REXCVAR_DEFINE_BOOL(native_grass, false, "NativeXRenderer",
                    "Experimental (read every frame): grass tufts on the terrain's flat, green ground");
REXCVAR_DEFINE_DOUBLE(native_grass_radius, 1440.0, "NativeXRenderer",
                      "Grass: the distance it reaches (m; 45, 90, 180, 360 and 1440 are the menu's Low to Full map). On "
                      "a level's grass map every tuft within it is drawn (D152); without one, past 45 m in rings each "
                      "twice as far, with tufts twice as far apart and larger");
REXCVAR_DEFINE_STRING(native_grass_textures, "", "NativeXRenderer",
                      "Grass: a file of plant pictures (the game's own vegetation textures, made by the game from the "
                      "player's files: \"BFVG\", version 1); empty or missing: blades drawn by the shader");
REXCVAR_DEFINE_BOOL(native_grass_water, true, "NativeXRenderer",
                    "Grass: none where the water covers the ground (found each frame from the finished scene), while "
                    "the level's water height is not known");
REXCVAR_DEFINE_DOUBLE(native_grass_water_level, -100000.0, "NativeXRenderer",
                      "Grass: the level's water height (its water mesh's, set by the game when a level loads); none "
                      "grows below it. Below -10000: not known");
REXCVAR_DEFINE_BOOL(native_grass_handoff, true, "NativeXRenderer",
                    "Grass: one decision for a tuft in every ring (the nearer ring's, or the further one's for a tuft new "
                    "to a ring); false: each ring decides apart, as before");
REXCVAR_DEFINE_DOUBLE(native_grass_fade_band, 0.2, "NativeXRenderer",
                      "Grass: how gradually a tuft fades in and out with distance (each side of its share; 0.06 before)");
REXCVAR_DEFINE_BOOL(native_grass_bounds, true, "NativeXRenderer",
                    "Grass: a tuft drawn while any of it may be on screen (false: its root or top, as before)");
REXCVAR_DEFINE_BOOL(native_grass_fade_pass, true, "NativeXRenderer",
                    "Grass: fading tufts drawn after the others without writing depth");
REXCVAR_DEFINE_DOUBLE(native_grass_settle, 0.15, "NativeXRenderer",
                      "Grass: the time constant (s) of a tuft's look of its ground (how fast a change of the ground's "
                      "colour moves its decision)");
REXCVAR_DEFINE_DOUBLE(native_grass_fade_time, 0.2, "NativeXRenderer",
                      "Grass: the time (s) a tuft takes at least to fade in or out when its decision changes");
REXCVAR_DEFINE_BOOL(native_grass_pops, false, "NativeXRenderer",
                    "Grass (developer): count the tufts whose opacity jumps in a frame while in view, logged with the "
                    "counts");
REXCVAR_DEFINE_BOOL(native_grass_steady, true, "NativeXRenderer",
                    "Grass: where it grows decided over frames, from several pixels, and kept while its spot is hidden "
                    "(false: from one pixel each frame, as before: tufts came and went as the camera moved)");
REXCVAR_DEFINE_DOUBLE(native_grass_cell, 0.45, "NativeXRenderer", "Grass prototype: one tuft per cell of this size (m)");
REXCVAR_DEFINE_DOUBLE(native_grass_height, 0.45, "NativeXRenderer", "Grass prototype: a tuft's height (m)");
REXCVAR_DEFINE_DOUBLE(native_grass_density, 0.7, "NativeXRenderer", "Grass prototype: the share of cells with a tuft");
REXCVAR_DEFINE_DOUBLE(native_grass_slope, 0.45, "NativeXRenderer",
                      "Grass prototype: the steepest ground with grass (rise over run; 0.45 is 24 degrees)");
REXCVAR_DEFINE_DOUBLE(native_grass_green, 0.45, "NativeXRenderer",
                      "Grass prototype: the lowest saturation of the ground's lit colour where grass grows");
REXCVAR_DEFINE_DOUBLE(native_grass_hue, 48.0, "NativeXRenderer",
                      "Grass prototype: the lowest hue (degrees, 60 is yellow, 120 green) of ground where grass grows");
REXCVAR_DEFINE_INT32(native_grass_debug, 0, "NativeXRenderer",
                     "Grass prototype: 1 tufts in one flat colour (where they grow), 2 also on ground that is not green, 3 in "
                     "every cell (no slope, view or hidden root test), 5 on any ground in their own colours, 6 on any "
                     "ground coloured by the level's map: green grass, amber within the edge, red not grass");
REXCVAR_DEFINE_BOOL(native_grass_gpu_time, false, "NativeXRenderer",
                    "Grass prototype: time its GPU work and log the average every 600 frames");
REXCVAR_DEFINE_BOOL(native_grass_map, true, "NativeXRenderer",
                    "Grass: where it grows from the level's own map of its ground (its grass, given by the game: "
                    "GrassMap), the same whatever the view; false: from the ground's colour on screen, as before");
REXCVAR_DEFINE_DOUBLE(native_grass_edge, 1.5, "NativeXRenderer",
                      "Grass: no tuft nearer than this (m) to ground that is not grass, on the level's map");
REXCVAR_DEFINE_DOUBLE(native_grass_edge_fade, 2.5, "NativeXRenderer",
                      "Grass: past that edge, tufts thin in and grow to their full size over this distance (m)");
REXCVAR_DEFINE_DOUBLE(native_grass_pixels, 1.5, "NativeXRenderer",
                      "Grass on a level's map (D152): a far tuft fades out as it shrinks on screen from 1.5 times this "
                      "size to half of it (pixels): a tuft of a pixel or two only shimmers");

namespace native_x_renderer {

using namespace Diligent;

namespace {

constexpr uint32_t kMaxSide = 256;   // cells across a ring
constexpr uint32_t kMaxRings = 6;    // 45 m, then each twice as far: 1440 m
constexpr uint32_t kInstanceBytes = 48;  // root and size; colour, kind and seed; fade
constexpr uint32_t kAllMax = kMaxSide * kMaxSide * kMaxRings / 2;  // D152: tufts on a map at most (a draw list's room)

// std140, as the shaders' GrassConstants.
struct Constants {
  float wvp[4][4];
  float scale[4], offset[4], uv[4];
  float ndc_scale[4], ndc_offset[4];
  float camera[4];    // xyz, radius
  float grid[4];      // cell size, cells across, first cell x, first cell z
  float viewport[4];  // host x, y, width, height
  float depth[4];     // viewport min depth, max depth, 1 if nearer is greater, time (s)
  float params[4];    // density, height, steepest slope, green
  float target[4];    // colour width, height, heightmap size, debug
  float decode[4];    // how the heightmap's view holds the guest's 16 bits: 0 as they are, 1 half float, 2 snorm, 3 unorm
  float level[6][4];  // each ring: cell size, first cell x, first cell z, outer radius
  float lod[4];       // rings, the state's stamp, steady (1), plant pictures (their count, 0: drawn blades)
  float kinds[16][4]; // each plant picture's average colour and width over height
  float water[4];     // the water's picture: valid (1), its width, height (half the scene's)
  float finish[4];    // the finished scene's size over the scene target's (x, y)
  float probe[4];     // a ground height asked for (GrassProbe): x, z, asked (1), its number
  float fix[4];       // one decision across rings (1), fade band, bounds (1), fading tufts in their own pass (1)
  float pops[4];      // counting pops (1), this frame's number
  float lists[4];     // where the fading tufts' list starts in the instances
  float steps[4];     // the score's time constant (s), a whole tuft's fade (s), this frame's time (s)
  float map[4];       // D151, the level's grass map: 1 used, -1 being read (no grass), 0 none; first corner x, z; cell (m)
  float map2[4];      // its cells across, down; the edge kept clear (m); the edge's fade (m)
  float map3[4];      // this frame's parity (each ring's average colour: written to this half, read from the other)
  float all[4];       // D152, every tuft on the map: the first cell x, z (first ring cells), cells across, down
  float all2[4];      // their capacity; the screen size (pixels) where a far tuft is gone, where it is whole; pixels a
                      // metre seen a metre away
};

constexpr const char* kCommon = R"GLSL(
layout(std140, set = 0, binding = 0) uniform GrassConstants {
  vec4 k_wvp[4];
  vec4 k_scale, k_offset, k_uv;
  vec4 k_ndc_scale, k_ndc_offset;
  vec4 k_camera;
  vec4 k_grid;
  vec4 k_viewport;
  vec4 k_depth;
  vec4 k_params;
  vec4 k_target;
  vec4 k_decode;
  vec4 k_level[6];
  vec4 k_lod;
  vec4 k_kinds[16];
  vec4 k_water;
  vec4 k_finish;
  vec4 k_probe;
  vec4 k_fix;
  vec4 k_pops;
  vec4 k_lists;
  vec4 k_steps;
  vec4 k_map;
  vec4 k_map2;
  vec4 k_map3;
  vec4 k_all;
  vec4 k_all2;
};
vec4 Clip(vec3 w) {
  vec4 p = vec4(w, 1.0);
  vec4 c = vec4(dot(k_wvp[0], p), dot(k_wvp[1], p), dot(k_wvp[2], p), dot(k_wvp[3], p));
  c.xyz = c.xyz * k_ndc_scale.xyz + k_ndc_offset.xyz * c.w;
  return c;
}
)GLSL";

// One thread a cell of a ring: whether a tuft grows there (kept in the state from frame to frame), and the tufts that
// do and are in view appended to the instances, their count in the indirect draw's arguments.
constexpr const char* kPlaceCS = R"GLSL(
layout(local_size_x = 64) in;
layout(set = 0, binding = 1) uniform sampler2DArray g_height;
layout(set = 0, binding = 2) uniform sampler2D g_sceneColor;
layout(set = 0, binding = 3) uniform sampler2D g_sceneDepth;
layout(std430, set = 0, binding = 4) writeonly buffer GrassInstancesOut { vec4 g_out[]; };
layout(std430, set = 0, binding = 5) buffer GrassState { vec4 g_state[]; };
layout(std430, set = 0, binding = 6) buffer GrassArgs { uint g_args[]; };
layout(std430, set = 0, binding = 7) buffer GrassStats { uint g_stats[]; };
layout(set = 0, binding = 8) uniform sampler2D g_water;
layout(std430, set = 0, binding = 9) buffer GrassSeen { uvec4 g_seen[]; };
layout(set = 0, binding = 10) uniform sampler2D g_map;
layout(std430, set = 0, binding = 11) buffer GrassAverage { uint g_avg[]; };

uint Hash(uint x) {
  x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
  return x;
}
float Rand(inout uint s) { s = Hash(s); return float(s & 0xFFFFFFu) / 16777216.0; }
// The child (of four) a cell of ring `level` keeps for the ring past it: chosen by hash.
ivec2 Child(ivec2 cell, uint level) {
  uint h = Hash(uint(cell.x) * 2654435761u ^ uint(cell.y) * 40503u ^ level * 9737333u);
  return cell * 2 + ivec2(int(h & 1u), int((h >> 1) & 1u));
}
// The first ring's cell standing for `cell` of ring `level`: the kept child of the kept child, down to the first
// ring. A tuft is the same plant (place, kind, size) in every ring that draws it.
ivec2 Representative(ivec2 cell, uint level) {
  for (uint l = level; l > 0u; --l) cell = Child(cell, l);
  return cell;
}
// Whether `cell` of ring `level` is the one its parent in the next ring keeps (drawn on there), or thins out.
bool Kept(ivec2 cell, uint level) {
  ivec2 parent = cell >> 1;
  return Child(parent, level + 1u) == cell;
}
// The heightmap is the guest's k_16 (unsigned normalized) whose bits the host texture holds as R16_FLOAT (RenderDoc:
// texel 0.1018 where the terrain's vertex is at 181.70 = 0.18170 x 1000, and 0.1018 is the half float 0x2E84 =
// 11908 / 65535): the bits are read back, and filtered here (bilinear) as the guest's values.
float Texel(ivec2 p) {
  int n = int(k_target.z);
  p = clamp(p, ivec2(0), ivec2(n - 1));
  float h = texelFetch(g_height, ivec3(p, 0), 0).r;
  if (k_decode.x == 1.0) return float(packHalf2x16(vec2(h, 0.0)) & 0xFFFFu) / 65535.0;
  if (k_decode.x == 2.0) {
    float b = round(h * 32767.0);
    return (b < 0.0 ? b + 65536.0 : b) / 65535.0;
  }
  return h;
}
float Height(vec2 xz) {
  vec2 g = (xz - k_offset.xz) / k_scale.xz;
  vec2 t = (g * k_uv.xy + k_uv.zw) * k_target.z - 0.5;
  ivec2 i = ivec2(floor(t));
  vec2 f = t - vec2(i);
  float a = mix(Texel(i), Texel(i + ivec2(1, 0)), f.x);
  float b = mix(Texel(i + ivec2(0, 1)), Texel(i + ivec2(1, 1)), f.x);
  return mix(a, b, f.y) * k_scale.y + k_offset.y;
}
// Green ground: the lit colour's hue from yellow-green to green and its saturation (the scene's colour before the tone
// mapping, RenderDoc: grass hue 52 to 66 degrees, saturation 0.6; sand and rock below 0.05).
float Green(vec3 ground) {
  float hi = max(ground.r, max(ground.g, ground.b)), lo = min(ground.r, min(ground.g, ground.b));
  float saturation = hi > 0.0 ? (hi - lo) / hi : 0.0;
  float hue = 0.0;
  if (hi > lo) {
    if (hi == ground.r) hue = 60.0 * mod((ground.g - ground.b) / (hi - lo), 6.0);
    else if (hi == ground.g) hue = 60.0 * ((ground.b - ground.r) / (hi - lo) + 2.0);
    else hue = 60.0 * ((ground.r - ground.g) / (hi - lo) + 4.0);
  }
  return (saturation < k_params.w || hue < k_decode.z || hue > 170.0) ? 0.0 : 1.0;
}
bool OnScreen(vec4 c, float margin_x, float margin_y) {
  return c.w > 0.05 && abs(c.x / c.w) <= margin_x && abs(c.y / c.w) <= margin_y;
}
// D151: the level's grass map (GrassMap): the distance (m) from this point to ground that is not grass (bilinear
// between the cells' centres); 0 off the map.
float MapDistance(vec2 xz) {
  vec2 uv = (xz - k_map.yz) / (k_map.w * k_map2.xy);
  if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 0.0;
  return textureLod(g_map, uv, 0.0).r * (255.0 / 16.0);
}
// D151: the ground's colour for a tuft whose root has not been seen yet: the average of the roots seen last frame in
// its ring, else in the nearest ring that saw any, else a plain dark green.
vec3 RingAverage(uint level, uint rings) {
  uint read = (uint(k_map3.x) & 1u) ^ 1u;
  for (uint d = 0u; d < rings; ++d) {
    for (int side = 0; side < 2; ++side) {
      if (side == 1 && d == 0u) continue;
      int l = side == 0 ? int(level + d) : int(level) - int(d);
      if (l < 0 || l >= int(rings)) continue;
      uint b = (read * 6u + uint(l)) * 4u;
      uint count = g_avg[b + 3u];
      if (count > 0u) return vec3(float(g_avg[b]), float(g_avg[b + 1u]), float(g_avg[b + 2u])) / (256.0 * float(count));
    }
  }
  return vec3(0.16, 0.2, 0.09);
}
const ivec2 kAround[8] = ivec2[](ivec2(-3, -3), ivec2(0, -3), ivec2(3, -3), ivec2(-3, 0), ivec2(3, 0), ivec2(-3, 3),
                                 ivec2(0, 3), ivec2(3, 3));
void main() {
  uint i = gl_GlobalInvocationID.x;
  // A ground height asked for (GrassProbe, the developer camera tour's).
  if (i == 0u && k_probe.z > 0.5) {
    g_stats[4] = uint(k_probe.w);
    g_stats[5] = floatBitsToUint(Height(k_probe.xy));
  }
  uint n = uint(k_grid.y), per = n * n, rings = uint(k_lod.x);
  if (i >= per * rings) return;
  // D151: the level's map is being read (a level loading): no grass until it comes.
  if (k_map.x < -0.5) return;
  bool mapped = k_map.x > 0.5;
  int debug = int(k_target.w + 0.5);
  uint level = i / per, j = i % per;
  vec4 L = k_level[level];
  ivec2 cell = ivec2(int(j % n), int(j / n)) + ivec2(L.yz);
  // The cell's own slot (wrapped: the grid moves with the camera a cell at a time, a cell keeps its slot).
  ivec2 wrapped = ((cell % int(n)) + int(n)) % int(n);
  uint slot = level * per + uint(wrapped.y) * n + uint(wrapped.x);
  // The tuft: the first ring's cell this one stands for, and that cell's own random values (the same in every ring).
  ivec2 fine = Representative(cell, level);
  uint s = Hash(uint(fine.x) * 73856093u ^ uint(fine.y) * 19349663u);
  float r0 = Rand(s), r1 = Rand(s), r2 = Rand(s), r3 = Rand(s), r4 = Rand(s), r5 = Rand(s), r6 = Rand(s), r7 = Rand(s);
  float r8 = Rand(s);
  vec2 xz = (vec2(fine) + vec2(r1, r2)) * k_level[0].x;
  // D151: on the level's map a tuft grows on its grass only, none within the edge (k_map2.z) of other ground, and
  // thins in and grows to its size over the edge's fade (k_map2.w) past it: the same tuft in every ring and whatever
  // the view (its place and values are its first ring cell's). Debug 6 shows the map: tufts on any ground, coloured.
  float dm = mapped ? MapDistance(xz) : 0.0;
  float edge_taper = 1.0;
  if (mapped && debug != 6) {
    if (dm < k_map2.z) return;
    edge_taper = smoothstep(k_map2.z, k_map2.z + max(k_map2.w, 0.01), dm);
  }
  if (r0 > k_params.x * edge_taper) return;
  float dist = length(xz - k_camera.xz);
  // The ring draws its tufts from where the last ring hands them on (0.85 of its radius) to where it hands them on to
  // the next; it keeps their state from a little nearer (0.65), so a tuft has its history when it is handed on.
  bool last = level + 1u == rings;
  float hand_in = level == 0u ? 0.0 : k_level[level - 1u].w * 0.85;
  float hand_out = last ? L.w : L.w * 0.85;
  float lowest = level == 0u ? 0.0 : k_level[level - 1u].w * 0.65;
  if (dist > L.w || dist < lowest) return;
  bool drawn_here = dist >= hand_in && (last || dist < hand_out);
  float y = Height(xz);
  // Under the sea (the level's water height, its water mesh's: TekRantGaming, 9 Nov, grass under the shallows and the deep
  // sea): never grass, and no state kept.
  if (y < k_water.w + 0.3) return;
  float e = 1.0;
  vec2 d = vec2(Height(xz + vec2(e, 0.0)) - Height(xz - vec2(e, 0.0)), Height(xz + vec2(0.0, e)) - Height(xz - vec2(0.0, e)));
  bool all_cells = k_target.w > 2.5 && k_target.w < 4.5;
  if (!all_cells && length(d) / (2.0 * e) > k_params.z) return;
  vec3 root = vec3(xz.x, y, xz.y);
  vec4 c = Clip(root);
  bool in_view = c.w > 0.05;
  vec3 ndc = in_view ? c.xyz / c.w : vec3(9.0);
  if (!all_cells) in_view = in_view && abs(ndc.x) <= 1.25 && abs(ndc.y) <= 1.8;
  // DiligentCore's Vulkan viewport is flipped (y + height, -height).
  vec2 px = k_viewport.xy + vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * k_viewport.zw;
  bool inside = in_view && all(greaterThanEqual(px, k_viewport.xy)) && all(lessThan(px, k_viewport.xy + k_viewport.zw));
  // What the game drew at the root, if it is the ground: not hidden by something clearly nearer (the game stores
  // 0.1 / distance in every depth partition, D132).
  bool seen = false, wet = false;
  vec3 ground = vec3(0.0);
  bool steady = k_lod.z > 0.5;
  if (inside) {
    ivec2 p = clamp(ivec2(px), ivec2(0), ivec2(k_target.xy) - 1);
    float drawn = 0.1 / max(texelFetch(g_sceneDepth, p, 0).r, 1e-7);
    if (all_cells || drawn >= c.w * 0.92 - 0.25) {
      seen = true;
      ground = texelFetch(g_sceneColor, p, 0).rgb;
      if (mapped) {
        // D151: the ground's light round the root from eight more pixels 3 apart at its depth (a nearer thing's left
        // out): only the tuft's colour now, steadier than a few texels of the ground's texture.
        float taken = 1.0;
        for (int k = 0; k < 8; ++k) {
          ivec2 q = clamp(p + kAround[k], ivec2(0), ivec2(k_target.xy) - 1);
          if (0.1 / max(texelFetch(g_sceneDepth, q, 0).r, 1e-7) >= c.w * 0.92 - 0.25) {
            ground += texelFetch(g_sceneColor, q, 0).rgb;
            taken += 1.0;
          }
        }
        ground /= taken;
      } else if (steady) {  // a few pixels round it: one texel of the ground's texture does not decide
        for (int k = 0; k < 4; ++k) {
          ivec2 o = ivec2(k == 0 ? 2 : k == 1 ? -2 : 0, k == 2 ? 2 : k == 3 ? -2 : 0);
          ground += texelFetch(g_sceneColor, clamp(p + o, ivec2(0), ivec2(k_target.xy) - 1), 0).rgb;
        }
        ground *= 0.2;
      }
      // Under water (the water's picture, Grass::MarkWater, the last frame's): never grass (TekRantGaming, 9 Nov: the sea
      // had grass under it; the sea floor is drawn green before the water over it).
      if (k_water.x > 0.5 && !all_cells) {
        ivec2 w = clamp(p / 2, ivec2(0), ivec2(k_water.yz) - 1);
        wet = texelFetch(g_water, w, 0).r > 0.5;
      }
    }
  }
  // Its state (the same cell last frame): the share of frames its ground looked green, its colour, and how much of
  // the tuft shows (its presence, 0 to 1).
  vec4 st0 = g_state[slot * 2u], st1 = g_state[slot * 2u + 1u];
  bool known = st0.w == k_lod.y && int(st0.x) == cell.x && int(st0.y) == cell.y;
  float score = known ? st0.z : 0.0;
  vec3 colour = known ? st1.rgb : ground;
  float presence = known ? st1.w : 0.0;
  // One decision for a tuft in every ring (TekRantGaming, 9 Nov: tufts still came and went as he walked; the rings decided
  // apart): while the nearer ring keeps the tuft's state (within its radius), this ring takes that ring's; a tuft new
  // to this ring takes the further ring's, if that one has it.
  if (k_fix.x > 0.5) {
    if (level > 0u && dist < k_level[level - 1u].w) {
      ivec2 child = Child(cell, level);
      ivec2 cw = ((child % int(n)) + int(n)) % int(n);
      uint cslot = (level - 1u) * per + uint(cw.y) * n + uint(cw.x);
      vec4 c0 = g_state[cslot * 2u], c1 = g_state[cslot * 2u + 1u];
      if (c0.w == k_lod.y && int(c0.x) == child.x && int(c0.y) == child.y) {
        score = c0.z, colour = c1.rgb, presence = c1.w, known = true;
      }
    } else if (!known && level + 1u < rings && Kept(cell, level)) {
      ivec2 parent = cell >> 1;
      ivec2 pw = ((parent % int(n)) + int(n)) % int(n);
      uint pslot = (level + 1u) * per + uint(pw.y) * n + uint(pw.x);
      vec4 p0 = g_state[pslot * 2u], p1 = g_state[pslot * 2u + 1u];
      if (p0.w == k_lod.y && int(p0.x) == parent.x && int(p0.y) == parent.y) {
        score = p0.z, colour = p1.rgb, presence = p1.w, known = true;
      }
    }
  }
  bool known_before = known;
  float presence_before = presence;
  float ease = 1.0 - exp(-k_steps.z / k_steps.x);
  if (mapped) {
    // D151: a tuft on the map always grows (only distance fades it). Its colour eases to its root's look while the
    // root is seen (that look itself the first time), and starts from its ring's average while it has not been seen.
    if (seen) {
      colour = known ? mix(colour, ground, ease) : ground;
      if (r5 < 0.125) {  // an eighth of the roots seen: each ring's average, for the next frame
        uint b = ((uint(k_map3.x) & 1u) * 6u + level) * 4u;
        vec3 g = clamp(ground, vec3(0.0), vec3(16.0)) * 256.0;
        atomicAdd(g_avg[b], uint(g.r));
        atomicAdd(g_avg[b + 1u], uint(g.g));
        atomicAdd(g_avg[b + 2u], uint(g.b));
        atomicAdd(g_avg[b + 3u], 1u);
      }
    } else if (!known) {
      colour = RingAverage(level, rings);
    }
    if (debug == 6) colour = dm <= 0.0 ? vec3(1.0, 0.05, 0.05) : dm < k_map2.z ? vec3(1.0, 0.75, 0.0) : vec3(0.1, 1.0, 0.2);
    score = 1.0;
    presence = 1.0;
  } else {
    // The score eases to this frame's look of the ground (a time constant of k_steps.x s); a root seen for the first
    // time starts from a quarter, so its tuft comes in by fading.
    if (seen) {
      float g = k_target.w >= 1.5 ? 1.0 : Green(ground);
      if (wet) score = 0.0, presence = 0.0;  // under water: none, at once
      else score = !steady ? g : mix(known ? score : 0.25, g, ease);
      colour = known && steady ? mix(colour, ground, 0.15) : ground;
      if (wet) atomicAdd(g_stats[2], 1u);
    }
    // Its presence follows the score with a margin (none below 0.35, all of it above 0.65), by at most a whole tuft
    // in k_steps.y s (TekRantGaming, 9 Nov: tufts still flickered and popped as he walked): a decision that changes fades the
    // tuft in or out, never at once. Without steady: this frame's pixel alone, as before.
    float wanted_presence = steady ? smoothstep(0.35, 0.65, score) : (seen && score > 0.5 ? 1.0 : 0.0);
    if (!steady) presence = wanted_presence;
    else if (!wet) presence += clamp(wanted_presence - presence, -k_steps.z / k_steps.y, k_steps.z / k_steps.y);
    presence = clamp(presence, 0.0, 1.0);
  }
  bool grows = presence > 0.004;
  if (seen || known || mapped) {
    g_state[slot * 2u] = vec4(float(cell.x), float(cell.y), score, k_lod.y);
    g_state[slot * 2u + 1u] = vec4(colour, presence);
  }
  if (known_before && (presence > 0.5) != (presence_before > 0.5) && drawn_here) atomicAdd(g_stats[1], 1u);
  if (!drawn_here) return;
  float kind, size, kind_size = 1.0;
  if (k_lod.w > 0.5) {
    // The game's plants (grass.h): mostly its grass (two pictures); the others in patches of 7 m, each patch with a
    // plant of its own and its own share of them, now and then any of the others.
    uint ph = Hash(uint(int(floor(xz.x / 7.0))) * 1640531513u ^ uint(int(floor(xz.y / 7.0))) * 2246822519u);
    float patch_kind = float(ph & 0xFFFFu) / 65536.0;
    float patch_share = float((ph >> 16) & 0xFFu) / 256.0;
    if (r4 < 0.08 + 0.3 * patch_share) {
      float pick = r6 < 0.7 ? patch_kind : r7;
      // A long leaf, a fern, a leafy weed, a jungle leaf, a leafy branch, a tall weed, two leafy bushes, a young palm,
      // a dried palm frond.
      const float kShare[10] = float[](0.13, 0.30, 0.43, 0.46, 0.51, 0.65, 0.78, 0.91, 0.96, 1.0);
      kind = 11.0;
      for (int k = 0; k < 10; ++k)
        if (pick < kShare[k]) {
          kind = float(k + 2);
          break;
        }
    } else {
      kind = r6 < 0.5 ? 0.0 : 1.0;
    }
    if (kind > k_lod.w - 1.0) kind = 0.0;  // an older file with fewer pictures
    const float kSizes[12] = float[](1.0, 1.0, 1.1, 1.4, 1.5, 1.8, 1.6, 2.2, 1.5, 1.3, 1.5, 1.3);
    size = (0.6 + 0.6 * r3) * k_params.y * kSizes[int(kind)];
    kind_size = kSizes[int(kind)];
  } else {
    kind = r4 < 0.05 ? 2.0 : r4 < 0.15 ? 1.0 : 0.0;
    size = (0.6 + 0.6 * r3) * k_params.y * (kind == 1.0 ? 1.7 : 1.0);
  }
  // D151: near the map's edge, smaller as well as fewer.
  size *= mix(0.7, 1.0, edge_taper);
  // Past near_d (half the first ring's hand on) the tufts thin out as 1 / d^2 and grow as d^0.9, so the ground stays
  // nearly as covered (TekRantGaming, 9 Nov: the far ground looked bare until he came closer; grown as d itself, far tufts
  // stood out tall on the skyline, run g3_gc_tour3), to 8 times their size; with the distance itself, never by ring.
  // The tall kinds' extra size goes as they grow (a far tall weed is not 12 m tall).
  float near_d = k_level[0].w * 0.425;
  float grow = clamp(pow(max(dist, near_d) / near_d, 0.9), 1.0, 8.0);
  size *= grow * mix(1.0, 1.2 / kind_size, clamp((grow - 1.0) / 4.0, 0.0, 1.0) * step(1.2, kind_size));
  // The share of the ring's tufts that show here: of the density wanted (1 / d^2 past near_d, relative to the first
  // ring's), the ring's cells (4^-level of it) give all; the quarter the next ring keeps always show, the other three
  // quarters thin out to none by the hand on (the last ring's all alike), and all fade at the last ring's edge.
  float wanted = min(1.0, (near_d / max(dist, 0.01)) * (near_d / max(dist, 0.01))) * pow(4.0, float(level));
  float share = last ? min(wanted, 1.0) : Kept(cell, level) ? 1.0 : clamp((wanted - 0.25) / 0.75, 0.0, 1.0);
  if (last) share *= 1.0 - smoothstep(L.w * 0.8, L.w, dist);
  // Each tuft at its own share (r8): whole tufts come and go, each over a stretch of distance (k_fix.y each side of
  // its share), blended (never a dissolve's sparkle, never a smaller tuft).
  float band = k_fix.y;
  float t = band + (1.0 - 2.0 * band) * r8;
  float fade = smoothstep(t - band, t + band, share) * presence;
  // On screen: any of it (a ball round the tuft, as wide as its widest card: a near tuft at the screen's edge whose
  // root is out of view, TekRantGaming's tufts popping as he turned and walked; the old test, root or top, with k_fix.z 0).
  float aspect = k_lod.w > 0.5 ? clamp(k_kinds[int(kind)].w, 0.25, 2.5) : 1.5;
  bool on_screen;
  if (k_fix.z > 0.5) {
    float r = size * max(0.6, aspect * 0.6);
    vec4 cc = Clip(root + vec3(0.0, size * 0.5, 0.0));
    on_screen = cc.w > -r && abs(cc.x) <= cc.w * 1.02 + r * 2.0 && abs(cc.y) <= cc.w * 1.02 + r * 2.6;
  } else {
    on_screen = in_view || OnScreen(Clip(root + vec3(0.0, size, 0.0)), 1.5, 1.3);
  }
  if (all_cells) on_screen = true;
  bool shown = grows && fade >= 0.01 && on_screen;
  // The pops (developer measure, k_pops.x 1): a tuft on screen last frame and now (any of it: its ball), whose
  // opacity jumped by more than a quarter in the frame: by its decision, by its root seen for the first time (it was
  // not known: behind a crest, a rock, a bush), or by its fade; keyed by its first ring cell.
  if (k_pops.x > 0.5) {
    bool here = on_screen && c.w > 0.05;
    uint e = Hash(uint(fine.x) * 2246822519u ^ uint(fine.y) * 3266489917u) & 0x7FFFFu;
    uint key = Hash(uint(fine.x) * 83492791u ^ uint(fine.y) * 2654435761u ^ 0x9E3779B9u) | 1u;
    uvec4 was_seen = g_seen[e];
    uint now = uint(k_pops.y), prev = now - 1u;
    float opacity = shown ? fade : 0.0;
    uint opacity_bits = uint(opacity * 255.0 + 0.5);
    float opacity_prev = float(was_seen.w & 0xFFu) / 255.0;
    bool grows_prev = (was_seen.w & 0x100u) != 0u;
    if (here && was_seen.y == key && was_seen.x == prev && abs(opacity - opacity_prev) > 0.25) {
      if (grows && !grows_prev && !known_before) atomicAdd(g_stats[7], 1u);
      else if (grows != grows_prev) atomicAdd(g_stats[3], 1u);
      else atomicAdd(g_stats[6], 1u);
    }
    g_seen[e] = uvec4(here ? now : 0u, key, 0u, opacity_bits | (grows ? 0x100u : 0u));
  }
  if (!shown) return;
  // Two lists: whole tufts (depth written), and tufts fading (drawn after, depth not written: a half faded tuft does
  // not hide the tufts behind it).
  uint list = fade < 0.999 && k_fix.w > 0.5 ? 1u : 0u;
  uint k = atomicAdd(g_args[1u + list * 4u], 1u) + list * uint(k_lists.x);
  atomicAdd(g_stats[0], 1u);
  g_out[k * 3u] = vec4(root, size);
  g_out[k * 3u + 1u] = vec4(colour, kind + r5 * 0.999);
  g_out[k * 3u + 2u] = vec4(fade, 0.0, 0.0, 0.0);
}
)GLSL";

constexpr const char* kDrawVS = R"GLSL(
layout(std430, set = 0, binding = 1) readonly buffer GrassInstances { vec4 g_inst[]; };
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_color;
layout(location = 2) flat out float v_kind;
layout(location = 3) flat out float v_seed;
layout(location = 4) flat out float v_fade;
const vec2 kCorners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));
void main() {
  uint inst = uint(gl_InstanceIndex);
  vec4 a = g_inst[inst * 3u];
  vec4 b = g_inst[inst * 3u + 1u];
  v_fade = g_inst[inst * 3u + 2u].x;
  v_uv = vec2(0.0);
  v_color = vec3(0.0);
  v_kind = 0.0;
  v_seed = 0.0;
  if (a.w <= 0.0) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    return;
  }
  // D152: drawn only in the depth partitions it reaches (the grass is drawn once per partition, each clipped to its
  // depths: the game stores 0.1 / distance, D132), not three times over.
  {
    float hi = max(k_depth.x, k_depth.y), lo = min(k_depth.x, k_depth.y);
    float near_d = 0.1 / max(hi, 1e-6), far_d = lo > 1e-7 ? 0.1 / lo : 1e9;
    float wc = Clip(a.xyz + vec3(0.0, a.w * 0.5, 0.0)).w, reach = a.w * 1.6 + 0.3;
    if (wc + reach < near_d || wc - reach > far_d) {
      gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
      return;
    }
  }
  uint v = uint(gl_VertexIndex);
  uint card = v / 6u;
  vec2 q = kCorners[v % 6u];
  float kind = floor(b.w);
  float seed = fract(b.w);
  float angle = seed * 6.2832 + float(card) * 2.0944;
  vec2 dir = vec2(cos(angle), sin(angle));
  float h = a.w;
  float w = k_lod.w > 0.5 ? h * clamp(k_kinds[int(kind)].w, 0.25, 2.5) : h * (kind == 1.0 ? 1.5 : 1.2);
  float t = k_depth.w;
  vec2 sway = vec2(sin(t * 1.7 + a.x * 0.37 + a.z * 0.21), sin(t * 1.3 + a.z * 0.33)) * 0.07 * h;
  vec3 p = a.xyz + vec3(dir.x, 0.0, dir.y) * (q.x - 0.5) * w + vec3(0.0, q.y * h - 0.04, 0.0) +
           vec3(sway.x, 0.0, sway.y) * q.y * q.y;
  gl_Position = Clip(p);
  v_uv = q;
  v_color = b.rgb;
  v_kind = kind;
  v_seed = seed * 7.0 + float(card) * 0.37;
}
)GLSL";

constexpr const char* kDrawPS = R"GLSL(
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_color;
layout(location = 2) flat in float v_kind;
layout(location = 3) flat in float v_seed;
layout(location = 4) flat in float v_fade;
layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 2) uniform sampler2DArray g_plants;
float H1(float x) { return fract(sin(x * 127.1) * 43758.5453); }
void main() {
  float u = v_uv.x, v = v_uv.y;
  if (k_lod.w > 0.5) {
    // The plant's picture (its top at the card's top), lit as the ground at its root: the picture's own colour over
    // its average, times the ground's lit colour (light, shadow and fog from the game's own drawing), its foot darker.
    int kind = int(v_kind);
    vec4 t = texture(g_plants, vec3(u, 1.0 - v, float(kind)));
    // The edge blended over a pixel (the alpha's own rate of change across the screen): leaves do not shimmer as the
    // view moves the way a hard alpha test does.
    // A tuft coming or going (its fade, the placement's) blended as a whole.
    float a = clamp((t.a - 0.5) / max(fwidth(t.a), 0.0001) + 0.5, 0.0, 1.0) * v_fade;
    if (a < 0.02) discard;
    vec3 avg = max(k_kinds[kind].rgb, vec3(0.04));
    float lum = dot(v_color, vec3(0.3, 0.59, 0.11)) / max(dot(avg, vec3(0.3, 0.59, 0.11)), 0.04);
    vec3 c = mix(t.rgb * lum, t.rgb / avg * v_color, 0.55) * mix(0.6, 1.05, v);
    if (k_target.w > 0.5 && k_target.w < 4.5) c = vec3(1.0, 0.0, 1.0) * max(dot(v_color, vec3(0.3, 0.59, 0.11)), 0.2);
    if (k_target.w > 5.5 && k_target.w < 6.5) c = v_color * 0.6;  // D151's map, coloured by the placement
    out_color = vec4(c, a);
    return;
  }
  int blades = v_kind == 1.0 ? 10 : 6;
  bool hit = false;
  float tip = 0.0;
  for (int k = 0; k < 10; ++k) {
    if (k >= blades) break;
    float s = v_seed * 13.7 + float(k) * 1.618;
    float centre = 0.12 + 0.76 * H1(s);
    float top = 0.5 + 0.5 * H1(s + 0.3);
    float lean = (H1(s + 0.6) - 0.5) * 0.6;
    float x = centre + lean * v * v;
    float width = max((v_kind == 1.0 ? 0.075 : 0.055) * (1.0 - v / top), fwidth(u) * 0.6 * step(v, top));
    if (v < top && abs(u - x) < width) {
      hit = true;
      tip = max(tip, v / top);
    }
  }
  if (!hit) discard;
  vec3 c = v_color * vec3(0.92, 1.06, 0.82) * mix(0.5, 1.12, v);
  if (v_kind == 1.0) c *= 0.8;
  if (v_kind == 2.0 && tip > 0.82) {
    float pick = H1(v_seed + 4.0);
    vec3 petal = pick < 0.4 ? vec3(1.0, 0.85, 0.2) : pick < 0.75 ? vec3(1.0, 1.0, 0.95) : vec3(0.9, 0.25, 0.3);
    c = petal * dot(v_color, vec3(0.3, 0.59, 0.11)) * 1.6;
  }
  if (k_target.w > 5.5 && k_target.w < 6.5) c = v_color * 0.6;  // D151's map, coloured by the placement
  else if (k_target.w > 0.5) c = vec3(1.0, 0.0, 1.0) * max(dot(v_color, vec3(0.3, 0.59, 0.11)), 0.2);
  out_color = vec4(c, v_fade);
}
)GLSL";

// D152: functions shared by the map's two passes (each declares its own resources first).
constexpr const char* kFnHash = R"GLSL(
uint Hash(uint x) {
  x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
  return x;
}
float Rand(inout uint s) { s = Hash(s); return float(s & 0xFFFFFFu) / 16777216.0; }
)GLSL";
constexpr const char* kFnHeight = R"GLSL(
float Texel(ivec2 p) {
  int n = int(k_target.z);
  p = clamp(p, ivec2(0), ivec2(n - 1));
  float h = texelFetch(g_height, ivec3(p, 0), 0).r;
  if (k_decode.x == 1.0) return float(packHalf2x16(vec2(h, 0.0)) & 0xFFFFu) / 65535.0;
  if (k_decode.x == 2.0) {
    float b = round(h * 32767.0);
    return (b < 0.0 ? b + 65536.0 : b) / 65535.0;
  }
  return h;
}
float Height(vec2 xz) {
  vec2 g = (xz - k_offset.xz) / k_scale.xz;
  vec2 t = (g * k_uv.xy + k_uv.zw) * k_target.z - 0.5;
  ivec2 i = ivec2(floor(t));
  vec2 f = t - vec2(i);
  float a = mix(Texel(i), Texel(i + ivec2(1, 0)), f.x);
  float b = mix(Texel(i + ivec2(0, 1)), Texel(i + ivec2(1, 1)), f.x);
  return mix(a, b, f.y) * k_scale.y + k_offset.y;
}
)GLSL";

// D152: every tuft of the level's grass map laid out once (a thread per first ring cell over the map's box; the same
// rules, plants and random values as the rings, without their thinning or growing): its place, size and plant.
constexpr const char* kAllGenCS = R"GLSL(
layout(local_size_x = 64) in;
layout(set = 0, binding = 1) uniform sampler2D g_map;
layout(std430, set = 0, binding = 2) writeonly buffer GrassAllOut { vec4 g_all_out[]; };
layout(std430, set = 0, binding = 3) buffer GrassAllCount { uint g_all_count[]; };
)GLSL";
constexpr const char* kAllGenMain = R"GLSL(
float MapDistance(vec2 xz) {
  vec2 uv = (xz - k_map.yz) / (k_map.w * k_map2.xy);
  if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 0.0;
  return textureLod(g_map, uv, 0.0).r * (255.0 / 16.0);
}
void main() {
  uint i = (gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * 64u + gl_LocalInvocationID.x;
  uint across = uint(k_all.z), down = uint(k_all.w);
  if (i >= across * down) return;
  ivec2 fine = ivec2(int(i % across), int(i / across)) + ivec2(k_all.xy);
  uint s = Hash(uint(fine.x) * 73856093u ^ uint(fine.y) * 19349663u);
  float r0 = Rand(s), r1 = Rand(s), r2 = Rand(s), r3 = Rand(s), r4 = Rand(s), r5 = Rand(s), r6 = Rand(s), r7 = Rand(s);
  vec2 xz = (vec2(fine) + vec2(r1, r2)) * k_level[0].x;
  float dm = MapDistance(xz);
  if (dm < k_map2.z) return;
  float taper = smoothstep(k_map2.z, k_map2.z + max(k_map2.w, 0.01), dm);
  if (r0 > k_params.x * taper) return;
  // The plant (the rings' own choice, D145, D146): mostly the game's grass, the others in patches of 7 m.
  float kind, size;
  if (k_lod.w > 0.5) {
    uint ph = Hash(uint(int(floor(xz.x / 7.0))) * 1640531513u ^ uint(int(floor(xz.y / 7.0))) * 2246822519u);
    float patch_kind = float(ph & 0xFFFFu) / 65536.0;
    float patch_share = float((ph >> 16) & 0xFFu) / 256.0;
    if (r4 < 0.08 + 0.3 * patch_share) {
      float pick = r6 < 0.7 ? patch_kind : r7;
      const float kShare[10] = float[](0.13, 0.30, 0.43, 0.46, 0.51, 0.65, 0.78, 0.91, 0.96, 1.0);
      kind = 11.0;
      for (int k = 0; k < 10; ++k)
        if (pick < kShare[k]) {
          kind = float(k + 2);
          break;
        }
    } else {
      kind = r6 < 0.5 ? 0.0 : 1.0;
    }
    if (kind > k_lod.w - 1.0) kind = 0.0;
    const float kSizes[12] = float[](1.0, 1.0, 1.1, 1.4, 1.5, 1.8, 1.6, 2.2, 1.5, 1.3, 1.5, 1.3);
    size = (0.6 + 0.6 * r3) * k_params.y * kSizes[int(kind)];
  } else {
    kind = r4 < 0.05 ? 2.0 : r4 < 0.15 ? 1.0 : 0.0;
    size = (0.6 + 0.6 * r3) * k_params.y * (kind == 1.0 ? 1.7 : 1.0);
  }
  size *= mix(0.7, 1.0, taper);  // near the map's edge, smaller as well as fewer (D151)
  uint k = atomicAdd(g_all_count[0], 1u);
  if (k < uint(k_all2.x)) g_all_out[k] = vec4(xz, size, kind + r5 * 0.999);
}
)GLSL";

// D152: each frame, every tuft laid out (a thread each): within the grass distance, in view, flat enough and above
// the water; its colour as the rings' (the ground's light at its root, eased; its distance band's average until its
// root is seen); faded only at the distance's edge and as it shrinks under a couple of pixels on screen.
constexpr const char* kAllCullCS = R"GLSL(
layout(local_size_x = 64) in;
layout(set = 0, binding = 1) uniform sampler2DArray g_height;
layout(set = 0, binding = 2) uniform sampler2D g_sceneColor;
layout(set = 0, binding = 3) uniform sampler2D g_sceneDepth;
layout(std430, set = 0, binding = 4) writeonly buffer GrassInstancesOut { vec4 g_out[]; };
layout(std430, set = 0, binding = 5) readonly buffer GrassAll { vec4 g_all[]; };
layout(std430, set = 0, binding = 6) buffer GrassAllState { vec4 g_all_state[]; };
layout(std430, set = 0, binding = 7) buffer GrassArgs { uint g_args[]; };
layout(std430, set = 0, binding = 8) buffer GrassStats { uint g_stats[]; };
layout(std430, set = 0, binding = 9) buffer GrassSeen { uvec4 g_seen[]; };
layout(std430, set = 0, binding = 10) buffer GrassAverage { uint g_avg[]; };
layout(std430, set = 0, binding = 11) readonly buffer GrassAllCountIn { uint g_all_count[]; };
)GLSL";
constexpr const char* kAllCullMain = R"GLSL(
const ivec2 kAround[8] = ivec2[](ivec2(-3, -3), ivec2(0, -3), ivec2(3, -3), ivec2(-3, 0), ivec2(3, 0), ivec2(-3, 3),
                                 ivec2(0, 3), ivec2(3, 3));
uint Band(float d) { return d < 20.0 ? 0u : d < 45.0 ? 1u : d < 90.0 ? 2u : d < 180.0 ? 3u : d < 360.0 ? 4u : 5u; }
vec3 BandAverage(uint band) {
  uint read = (uint(k_map3.x) & 1u) ^ 1u;
  for (uint d = 0u; d < 6u; ++d) {
    for (int side = 0; side < 2; ++side) {
      if (side == 1 && d == 0u) continue;
      int l = side == 0 ? int(band + d) : int(band) - int(d);
      if (l < 0 || l >= 6) continue;
      uint b = (read * 6u + uint(l)) * 4u;
      uint count = g_avg[b + 3u];
      if (count > 0u) return vec3(float(g_avg[b]), float(g_avg[b + 1u]), float(g_avg[b + 2u])) / (256.0 * float(count));
    }
  }
  return vec3(0.16, 0.2, 0.09);
}
void main() {
  uint i = (gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * 64u + gl_LocalInvocationID.x;
  // A ground height asked for (GrassProbe, the developer camera tour's).
  if (i == 0u && k_probe.z > 0.5) {
    g_stats[4] = uint(k_probe.w);
    g_stats[5] = floatBitsToUint(Height(k_probe.xy));
  }
  if (i >= min(g_all_count[0], uint(k_all2.x))) return;
  vec4 t = g_all[i];
  vec2 xz = t.xy;
  float size = t.z, kind = floor(t.w);
  float dist = length(xz - k_camera.xz);
  if (dist > k_camera.w) return;
  float y = Height(xz);
  if (y < k_water.w + 0.3) return;  // under the level's water (D147)
  vec3 root = vec3(xz.x, y, xz.y);
  // In view: a ball round it as wide as its widest card (D148).
  float aspect = k_lod.w > 0.5 ? clamp(k_kinds[int(kind)].w, 0.25, 2.5) : 1.5;
  float r = size * max(0.6, aspect * 0.6);
  vec4 cc = Clip(root + vec3(0.0, size * 0.5, 0.0));
  if (!(cc.w > -r && abs(cc.x) <= cc.w * 1.02 + r * 2.0 && abs(cc.y) <= cc.w * 1.02 + r * 2.6)) return;
  vec2 d = vec2(Height(xz + vec2(1.0, 0.0)) - Height(xz - vec2(1.0, 0.0)), Height(xz + vec2(0.0, 1.0)) - Height(xz - vec2(0.0, 1.0)));
  if (length(d) * 0.5 > k_params.z) return;  // too steep
  // Its size on screen (pixels): a far tuft fades out as it shrinks to a pixel or two, and at the distance's edge.
  float px = cc.w > 0.05 ? size * k_all2.w / cc.w : 1.0e6;
  float fade = smoothstep(k_all2.y, k_all2.z, px) * (1.0 - smoothstep(k_camera.w * 0.85, k_camera.w, dist));
  if (fade < 0.01) return;
  // Its colour: the ground's light round its root at its depth (nine pixels 3 apart), eased while seen; until then its
  // distance band's average of the roots seen last frame.
  vec4 c = Clip(root);
  bool seen = false;
  vec3 ground = vec3(0.0);
  if (c.w > 0.05) {
    vec3 ndc = c.xyz / c.w;
    vec2 at = k_viewport.xy + vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * k_viewport.zw;
    if (all(greaterThanEqual(at, k_viewport.xy)) && all(lessThan(at, k_viewport.xy + k_viewport.zw))) {
      ivec2 p = clamp(ivec2(at), ivec2(0), ivec2(k_target.xy) - 1);
      if (0.1 / max(texelFetch(g_sceneDepth, p, 0).r, 1e-7) >= c.w * 0.92 - 0.25) {
        seen = true;
        ground = texelFetch(g_sceneColor, p, 0).rgb;
        float taken = 1.0;
        for (int k = 0; k < 8; ++k) {
          ivec2 q = clamp(p + kAround[k], ivec2(0), ivec2(k_target.xy) - 1);
          if (0.1 / max(texelFetch(g_sceneDepth, q, 0).r, 1e-7) >= c.w * 0.92 - 0.25) {
            ground += texelFetch(g_sceneColor, q, 0).rgb;
            taken += 1.0;
          }
        }
        ground /= taken;
      }
    }
  }
  vec4 st = g_all_state[i];
  bool known = st.w > 0.5;
  vec3 colour = st.rgb;
  uint band = Band(dist);
  if (seen) {
    colour = known ? mix(colour, ground, 1.0 - exp(-k_steps.z / k_steps.x)) : ground;
    if (fract(t.w) < 0.125) {  // an eighth of the roots seen: each band's average, for the next frame
      uint b = ((uint(k_map3.x) & 1u) * 6u + band) * 4u;
      vec3 g = clamp(ground, vec3(0.0), vec3(16.0)) * 256.0;
      atomicAdd(g_avg[b], uint(g.r));
      atomicAdd(g_avg[b + 1u], uint(g.g));
      atomicAdd(g_avg[b + 2u], uint(g.b));
      atomicAdd(g_avg[b + 3u], 1u);
    }
  } else if (!known) {
    colour = BandAverage(band);
  }
  g_all_state[i] = vec4(colour, 1.0);
  // The pops (developer measure): its opacity jumped by more than a quarter since last frame, on screen in both.
  if (k_pops.x > 0.5 && i < (1u << 19)) {
    uvec4 was = g_seen[i];
    uint now = uint(k_pops.y);
    if (was.y == i + 1u && was.x == now - 1u && abs(fade - float(was.w & 0xFFu) / 255.0) > 0.25) atomicAdd(g_stats[6], 1u);
    g_seen[i] = uvec4(now, i + 1u, 0u, uint(fade * 255.0 + 0.5));
  }
  // Two lists, as the rings': whole tufts (depth written), and tufts fading (drawn after, depth not written).
  uint list = fade < 0.999 && k_fix.w > 0.5 ? 1u : 0u;
  uint k = atomicAdd(g_args[1u + list * 4u], 1u) + list * uint(k_lists.x);
  atomicAdd(g_stats[0], 1u);
  g_out[k * 3u] = vec4(root, size);
  g_out[k * 3u + 1u] = vec4(colour, t.w);
  g_out[k * 3u + 2u] = vec4(fade, 0.0, 0.0, 0.0);
}
)GLSL";

// The water's picture (TekRantGaming, 9 Nov: no grass under the sea). The scene at the end of the opaque draws, the grass in
// it, kept at half size (colour and depth); then, in the finished scene (after the water and the other transparent
// draws), the pixels the water covered: nothing nearer in the depth (the water does not write it; the game clears it
// before the first person weapon), the colour changed and turned blue. The next frame's placement grows no grass there.
constexpr const char* kSnapCS = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 1) uniform sampler2D g_sceneColor;
layout(set = 0, binding = 2) uniform sampler2D g_sceneDepth;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D g_snapColor;
layout(set = 0, binding = 4, r32f) uniform writeonly image2D g_snapDepth;
void main() {
  ivec2 q = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(q, ivec2(k_water.yz)))) return;
  ivec2 p = min(q * 2, ivec2(k_target.xy) - 1);
  imageStore(g_snapColor, q, vec4(texelFetch(g_sceneColor, p, 0).rgb, 0.0));
  imageStore(g_snapDepth, q, vec4(texelFetch(g_sceneDepth, p, 0).r));
}
)GLSL";

constexpr const char* kWaterCS = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 1) uniform sampler2D g_snapColor;
layout(set = 0, binding = 2) uniform sampler2D g_snapDepth;
layout(set = 0, binding = 3) uniform sampler2D g_finished;
layout(set = 0, binding = 4) uniform sampler2D g_sceneDepth;
layout(set = 0, binding = 5, r8) uniform writeonly image2D g_waterOut;
void main() {
  ivec2 q = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(q, ivec2(k_water.yz)))) return;
  ivec2 p = min(q * 2, ivec2(k_target.xy) - 1);
  vec3 before = texelFetch(g_snapColor, q, 0).rgb;
  float depth_before = texelFetch(g_snapDepth, q, 0).r;
  float depth_after = texelFetch(g_sceneDepth, p, 0).r;
  ivec2 f = clamp(ivec2((vec2(p) + 0.5) * k_finish.xy), ivec2(0), textureSize(g_finished, 0) - 1);
  vec3 after = texelFetch(g_finished, f, 0).rgb;
  // Nothing nearer drawn since (the game clears the depth before its first person weapon, so it is the weapon's or
  // cleared; 0.1 / distance: nearer is greater).
  bool same_depth = depth_after <= depth_before * 1.0001 + 1e-6;
  float lum = dot(before, vec3(0.3, 0.59, 0.11));
  bool changed = length(after - before) > 0.02 + 0.15 * lum;
  bool blue = after.b + 0.01 > (after.r + 0.01) * 1.15 &&
              (after.b + 0.02) / (after.r + 0.02) > (before.b + 0.02) / (before.r + 0.02) * 1.3;
  imageStore(g_waterOut, q, vec4(same_depth && changed && blue ? 1.0 : 0.0));
}
)GLSL";

RefCntAutoPtr<IShader> MakeShader(IRenderDevice* device, SHADER_TYPE type, const std::string& body, const char* name) {
  RefCntAutoPtr<IShader> shader;
  GlslStage stage = type == SHADER_TYPE_VERTEX    ? GlslStage::kVertex
                    : type == SHADER_TYPE_COMPUTE ? GlslStage::kCompute
                                                  : GlslStage::kPixel;
  std::string source = std::string("#version 450\n") + kCommon + body;
  std::vector<uint32_t> spirv = CompileGlsl(stage, source, name);
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

SamplerDesc Sampler(FILTER_TYPE filter) {
  SamplerDesc s;
  s.MinFilter = s.MagFilter = s.MipFilter = filter;
  s.AddressU = s.AddressV = s.AddressW = TEXTURE_ADDRESS_CLAMP;
  return s;
}

}  // namespace

bool Grass::On() { return REXCVAR_GET(native_grass); }

namespace {
// A ground height asked for (GrassProbe), answered by the placement a few frames later (GrassProbeResult).
struct Probe {
  std::mutex mutex;
  float x = 0, z = 0, ground = 0;
  uint32_t id = 0;
  bool asked = false, answered = false;
} g_probe;
}  // namespace

void GrassProbe(float x, float z, uint32_t id) {
  std::lock_guard lock(g_probe.mutex);
  g_probe.x = x, g_probe.z = z, g_probe.id = id;
  g_probe.asked = true, g_probe.answered = false;
}

bool GrassProbeResult(uint32_t* id, float* ground) {
  std::lock_guard lock(g_probe.mutex);
  if (!g_probe.answered) return false;
  *id = g_probe.id, *ground = g_probe.ground;
  return true;
}

namespace {
// D151: the level's grass map as the game gave it (GrassMap), taken by the next frame's placement.
struct GrassMapData {
  std::mutex mutex;
  std::vector<uint8_t> distance;
  uint32_t width = 0, height = 0;
  float x0 = 0, z0 = 0, cell = 0;
  int state = 0;  // -1 being read, 0 none known, 1 the map
  uint64_t version = 0;
} g_map;
}  // namespace

void GrassMap(const uint8_t* distance, uint32_t width, uint32_t height, float x0, float z0, float cell) {
  std::lock_guard lock(g_map.mutex);
  const bool given = distance && width > 0 && height > 0 && cell > 0;
  if (given) g_map.distance.assign(distance, distance + size_t(width) * height);
  else g_map.distance.clear();
  g_map.width = given ? width : 0, g_map.height = given ? height : 0;
  g_map.x0 = x0, g_map.z0 = z0, g_map.cell = cell;
  g_map.state = given ? 1 : cell == 0 ? -1 : 0;
  ++g_map.version;
}

bool Grass::Initialize(IRenderDevice* device) {
  device_ = device;
  BufferDesc constants;
  constants.Name = "NativeXRenderer grass constants";
  constants.Size = sizeof(Constants);
  constants.Usage = USAGE_DYNAMIC;
  constants.BindFlags = BIND_UNIFORM_BUFFER;
  constants.CPUAccessFlags = CPU_ACCESS_WRITE;
  device->CreateBuffer(constants, nullptr, &constants_);
  BufferDesc instances;
  instances.Name = "NativeXRenderer grass instances";
  instances.Size = uint64_t(kMaxSide) * kMaxSide * kMaxRings * kInstanceBytes;
  instances.Usage = USAGE_DEFAULT;
  instances.BindFlags = BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE;
  instances.Mode = BUFFER_MODE_STRUCTURED;
  instances.ElementByteStride = 16;
  device->CreateBuffer(instances, nullptr, &instances_);
  BufferDesc state = instances;
  state.Name = "NativeXRenderer grass state";
  state.Size = uint64_t(kMaxSide) * kMaxSide * kMaxRings * 32;
  device->CreateBuffer(state, nullptr, &state_);
  BufferDesc seen = instances;
  seen.Name = "NativeXRenderer grass pops";
  seen.Size = uint64_t(1) << 23;  // 2^19 tufts' last frame (native_grass_pops)
  seen.BindFlags = BIND_UNORDERED_ACCESS;
  device->CreateBuffer(seen, nullptr, &seen_);
  BufferDesc args;
  args.Name = "NativeXRenderer grass draw arguments";
  args.Size = 32;  // two indirect draws: whole tufts, fading tufts
  args.Usage = USAGE_DEFAULT;
  args.BindFlags = BIND_UNORDERED_ACCESS | BIND_INDIRECT_DRAW_ARGS;
  args.Mode = BUFFER_MODE_STRUCTURED;
  args.ElementByteStride = 4;
  device->CreateBuffer(args, nullptr, &args_);
  BufferDesc stats = args;
  stats.Name = "NativeXRenderer grass counts";
  stats.Size = 32;  // counts 0 to 3, a probe's number and ground height, counts 6 and 7
  stats.BindFlags = BIND_UNORDERED_ACCESS;
  {
    const std::vector<uint8_t> zero(stats.Size, 0);
    BufferData initial(zero.data(), stats.Size);
    device->CreateBuffer(stats, &initial, &stats_);
  }
  {
    // D151: each ring's average ground colour of the roots seen (sums x 256 and a count, six rings), two halves: this
    // frame's and the last one's.
    BufferDesc average = args;
    average.Name = "NativeXRenderer grass ring colours";
    average.Size = 2 * kMaxRings * 4 * 4;
    average.BindFlags = BIND_UNORDERED_ACCESS;
    const std::vector<uint8_t> zero(average.Size, 0);
    BufferData initial(zero.data(), average.Size);
    device->CreateBuffer(average, &initial, &average_);
  }
  {
    TextureDesc desc;
    desc.Name = "NativeXRenderer grass map (none)";
    desc.Type = RESOURCE_DIM_TEX_2D;
    desc.Width = desc.Height = 1;
    desc.Format = TEX_FORMAT_R8_UNORM;
    desc.Usage = USAGE_IMMUTABLE;
    desc.BindFlags = BIND_SHADER_RESOURCE;
    const uint32_t texel = 0;
    TextureSubResData one{&texel, 4};
    TextureData data{&one, 1};
    device->CreateTexture(desc, &data, &map_none_);
  }
  {
    // D152: every tuft on the map (place, size, plant), each one's colour, how many were laid out.
    BufferDesc all = instances;
    all.Name = "NativeXRenderer grass map tufts";
    all.Size = uint64_t(kAllMax) * 16;
    device->CreateBuffer(all, nullptr, &all_);
    all.Name = "NativeXRenderer grass map tuft colours";
    device->CreateBuffer(all, nullptr, &all_state_);
    BufferDesc count = args;
    count.Name = "NativeXRenderer grass map tuft count";
    count.Size = 16;
    count.BindFlags = BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE;
    const std::vector<uint8_t> zero(count.Size, 0);
    BufferData initial(zero.data(), count.Size);
    device->CreateBuffer(count, &initial, &all_count_);
  }
  BufferDesc readback;
  readback.Name = "NativeXRenderer grass counts read back";
  readback.Size = 32;
  readback.Usage = USAGE_STAGING;
  readback.CPUAccessFlags = CPU_ACCESS_READ;
  device->CreateBuffer(readback, nullptr, &stats_read_);
  readback.Name = "NativeXRenderer grass probe read back";
  readback.Size = 8;
  device->CreateBuffer(readback, nullptr, &probe_read_);
  readback.Name = "NativeXRenderer grass map tuft count read back";
  readback.Size = 16;
  device->CreateBuffer(readback, nullptr, &all_count_read_);
  {
    TextureDesc desc;
    desc.Name = "NativeXRenderer grass plants (none yet)";
    desc.Type = RESOURCE_DIM_TEX_2D_ARRAY;
    desc.Width = desc.Height = 1;
    desc.ArraySize = 1;
    desc.Format = TEX_FORMAT_RGBA8_UNORM;
    desc.Usage = USAGE_IMMUTABLE;
    desc.BindFlags = BIND_SHADER_RESOURCE;
    const uint32_t texel = 0;
    TextureSubResData one{&texel, 4};
    TextureData data{&one, 1};
    device->CreateTexture(desc, &data, &placeholder_);
  }
  place_cs_ = MakeShader(device, SHADER_TYPE_COMPUTE, kPlaceCS, "NativeXRenderer grass place");
  draw_vs_ = MakeShader(device, SHADER_TYPE_VERTEX, kDrawVS, "NativeXRenderer grass VS");
  draw_ps_ = MakeShader(device, SHADER_TYPE_PIXEL, kDrawPS, "NativeXRenderer grass PS");
  if (!constants_ || !instances_ || !state_ || !args_ || !stats_ || !average_ || !map_none_ || !place_cs_ || !draw_vs_ ||
      !draw_ps_) {
    REXLOG_ERROR("[native_x_renderer] grass prototype: could not be set up");
    return false;
  }
  ComputePipelineStateCreateInfo info;
  info.PSODesc.Name = "NativeXRenderer grass place";
  info.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
  info.pCS = place_cs_;
  ShaderResourceVariableDesc variables[] = {
      {SHADER_TYPE_COMPUTE, "g_height", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
      {SHADER_TYPE_COMPUTE, "g_sceneColor", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
      {SHADER_TYPE_COMPUTE, "g_sceneDepth", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
      {SHADER_TYPE_COMPUTE, "GrassInstancesOut", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
      {SHADER_TYPE_COMPUTE, "GrassState", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
      {SHADER_TYPE_COMPUTE, "GrassArgs", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
      {SHADER_TYPE_COMPUTE, "GrassStats", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
      {SHADER_TYPE_COMPUTE, "GrassSeen", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
      {SHADER_TYPE_COMPUTE, "GrassAverage", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
      {SHADER_TYPE_COMPUTE, "g_map", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
      {SHADER_TYPE_COMPUTE, "g_water", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC}};
  ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_COMPUTE, "g_height", Sampler(FILTER_TYPE_POINT)},
                                     {SHADER_TYPE_COMPUTE, "g_sceneColor", Sampler(FILTER_TYPE_POINT)},
                                     {SHADER_TYPE_COMPUTE, "g_sceneDepth", Sampler(FILTER_TYPE_POINT)},
                                     {SHADER_TYPE_COMPUTE, "g_water", Sampler(FILTER_TYPE_POINT)},
                                     {SHADER_TYPE_COMPUTE, "g_map", Sampler(FILTER_TYPE_LINEAR)}};
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
  info.PSODesc.ResourceLayout.Variables = variables;
  info.PSODesc.ResourceLayout.NumVariables = uint32_t(std::size(variables));
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
  info.PSODesc.ResourceLayout.NumImmutableSamplers = uint32_t(std::size(samplers));
  device->CreateComputePipelineState(info, &place_.pso);
  if (!place_.pso) {
    REXLOG_ERROR("[native_x_renderer] grass prototype: no placement pipeline");
    return false;
  }
  if (auto* v = place_.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "GrassConstants")) v->Set(constants_);
  if (auto* v = place_.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "GrassState"))
    v->Set(state_->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
  if (auto* v = place_.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "GrassArgs"))
    v->Set(args_->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
  if (auto* v = place_.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "GrassStats"))
    v->Set(stats_->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
  if (auto* v = place_.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "GrassSeen"))
    v->Set(seen_->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
  if (auto* v = place_.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "GrassAverage"))
    v->Set(average_->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));

  place_.pso->CreateShaderResourceBinding(&place_.srb, true);
  ready_ = place_.srb != nullptr;
  // The water's picture: two small passes (kSnapCS, kWaterCS). Without them the grass still works (under water too).
  auto simple_compute = [&](const char* body, const char* name, std::initializer_list<const char*> textures,
                            std::initializer_list<const char*> images, Pass& out) {
    RefCntAutoPtr<IShader> cs = MakeShader(device, SHADER_TYPE_COMPUTE, body, name);
    if (!cs) return;
    ComputePipelineStateCreateInfo ci;
    ci.PSODesc.Name = name;
    ci.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
    ci.pCS = cs;
    std::vector<ShaderResourceVariableDesc> vars;
    std::vector<ImmutableSamplerDesc> samps;
    for (const char* t : textures) {
      vars.push_back({SHADER_TYPE_COMPUTE, t, SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC});
      samps.push_back({SHADER_TYPE_COMPUTE, t, Sampler(FILTER_TYPE_POINT)});
    }
    for (const char* t : images) vars.push_back({SHADER_TYPE_COMPUTE, t, SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC});
    ci.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    ci.PSODesc.ResourceLayout.Variables = vars.data();
    ci.PSODesc.ResourceLayout.NumVariables = uint32_t(vars.size());
    ci.PSODesc.ResourceLayout.ImmutableSamplers = samps.data();
    ci.PSODesc.ResourceLayout.NumImmutableSamplers = uint32_t(samps.size());
    device->CreateComputePipelineState(ci, &out.pso);
    if (!out.pso) return;
    if (auto* v = out.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "GrassConstants")) v->Set(constants_);
    out.pso->CreateShaderResourceBinding(&out.srb, true);
  };
  // D152: the map's two passes (without them, the rings draw the map's grass as D151 did).
  auto map_compute = [&](const std::string& body, const char* name, std::initializer_list<const char*> dynamic,
                         std::initializer_list<std::pair<const char*, FILTER_TYPE>> textures, Pass& out) {
    RefCntAutoPtr<IShader> cs = MakeShader(device, SHADER_TYPE_COMPUTE, body, name);
    if (!cs) return;
    ComputePipelineStateCreateInfo ci;
    ci.PSODesc.Name = name;
    ci.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
    ci.pCS = cs;
    std::vector<ShaderResourceVariableDesc> vars;
    std::vector<ImmutableSamplerDesc> samps;
    for (const char* v : dynamic) vars.push_back({SHADER_TYPE_COMPUTE, v, SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC});
    for (const auto& [t, filter] : textures) samps.push_back({SHADER_TYPE_COMPUTE, t, Sampler(filter)});
    ci.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    ci.PSODesc.ResourceLayout.Variables = vars.data();
    ci.PSODesc.ResourceLayout.NumVariables = uint32_t(vars.size());
    ci.PSODesc.ResourceLayout.ImmutableSamplers = samps.data();
    ci.PSODesc.ResourceLayout.NumImmutableSamplers = uint32_t(samps.size());
    device->CreateComputePipelineState(ci, &out.pso);
    if (!out.pso) return;
    auto set = [&](const char* v, IDeviceObject* o) {
      if (auto* var = out.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, v)) var->Set(o);
    };
    set("GrassConstants", constants_);
    set("GrassAllOut", all_->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    set("GrassAllCount", all_count_->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    set("GrassAll", all_->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
    set("GrassAllState", all_state_->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    set("GrassArgs", args_->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    set("GrassStats", stats_->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    set("GrassSeen", seen_->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    set("GrassAverage", average_->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    set("GrassAllCountIn", all_count_->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
    out.pso->CreateShaderResourceBinding(&out.srb, true);
  };
  if (all_ && all_state_ && all_count_ && all_count_read_) {
    map_compute(std::string(kAllGenCS) + kFnHash + kAllGenMain, "NativeXRenderer grass map tufts laid out", {"g_map"},
                {{"g_map", FILTER_TYPE_LINEAR}}, all_gen_);
    map_compute(std::string(kAllCullCS) + kFnHash + kFnHeight + kAllCullMain, "NativeXRenderer grass map tufts in view",
                {"g_height", "g_sceneColor", "g_sceneDepth", "GrassInstancesOut"},
                {{"g_height", FILTER_TYPE_POINT}, {"g_sceneColor", FILTER_TYPE_POINT}, {"g_sceneDepth", FILTER_TYPE_POINT}},
                all_cull_);
  }
  if (!all_gen_.srb || !all_cull_.srb) REXLOG_WARN("[native_x_renderer] grass: no map passes (D152): the rings draw it");
  simple_compute(kSnapCS, "NativeXRenderer grass scene snapshot", {"g_sceneColor", "g_sceneDepth"},
                 {"g_snapColor", "g_snapDepth"}, snap_);
  simple_compute(kWaterCS, "NativeXRenderer grass water picture", {"g_snapColor", "g_snapDepth", "g_finished", "g_sceneDepth"},
                 {"g_waterOut"}, water_pass_);
  if (!snap_.srb || !water_pass_.srb) REXLOG_WARN("[native_x_renderer] grass: no water picture (grass may grow under water)");
  REXLOG_INFO("[native_x_renderer] grass prototype: {}", ready_ ? "ready (native_grass turns it on)" : "failed");
  return ready_;
}

Grass::Pass* Grass::DrawPass(TEXTURE_FORMAT color, TEXTURE_FORMAT depth, uint8_t depth_func, bool depth_clip,
                             bool depth_write) {
  auto key = std::make_tuple(int(color), int(depth), int(depth_func), int(depth_clip) | (depth_write ? 2 : 0));
  auto it = draw_passes_.find(key);
  if (it != draw_passes_.end()) return it->second.srb ? &it->second : nullptr;
  Pass& pass = draw_passes_[key];
  GraphicsPipelineStateCreateInfo info;
  info.PSODesc.Name = "NativeXRenderer grass draw";
  info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
  info.GraphicsPipeline.NumRenderTargets = 1;
  info.GraphicsPipeline.RTVFormats[0] = color;
  info.GraphicsPipeline.DSVFormat = depth;
  info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
  info.GraphicsPipeline.RasterizerDesc.ScissorEnable = True;
  info.GraphicsPipeline.RasterizerDesc.DepthClipEnable = depth_clip;
  info.GraphicsPipeline.DepthStencilDesc.DepthEnable = True;
  info.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable = depth_write;
  info.GraphicsPipeline.DepthStencilDesc.DepthFunc = COMPARISON_FUNCTION(depth_func);
  // Blended (the leaves' edges, a pixel wide: the pixel shader's alpha); the target's own alpha left as it is.
  auto& blend = info.GraphicsPipeline.BlendDesc.RenderTargets[0];
  blend.BlendEnable = True;
  blend.SrcBlend = BLEND_FACTOR_SRC_ALPHA;
  blend.DestBlend = BLEND_FACTOR_INV_SRC_ALPHA;
  blend.BlendOp = BLEND_OPERATION_ADD;
  blend.SrcBlendAlpha = BLEND_FACTOR_ZERO;
  blend.DestBlendAlpha = BLEND_FACTOR_ONE;
  blend.BlendOpAlpha = BLEND_OPERATION_ADD;
  blend.RenderTargetWriteMask = COLOR_MASK_RGB;
  info.pVS = draw_vs_;
  info.pPS = draw_ps_;
  ShaderResourceVariableDesc variables[] = {
      {SHADER_TYPE_VERTEX, "GrassInstances", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
      {SHADER_TYPE_PIXEL, "g_plants", SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE}};
  SamplerDesc plants_sampler = Sampler(FILTER_TYPE_ANISOTROPIC);
  plants_sampler.MaxAnisotropy = 4;
  ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "g_plants", plants_sampler}};
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
  info.PSODesc.ResourceLayout.Variables = variables;
  info.PSODesc.ResourceLayout.NumVariables = uint32_t(std::size(variables));
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
  info.PSODesc.ResourceLayout.NumImmutableSamplers = uint32_t(std::size(samplers));
  device_->CreateGraphicsPipelineState(info, &pass.pso);
  if (!pass.pso) {
    REXLOG_ERROR("[native_x_renderer] grass prototype: no draw pipeline for formats {} {}", int(color), int(depth));
    return nullptr;
  }
  for (SHADER_TYPE stage : {SHADER_TYPE_VERTEX, SHADER_TYPE_PIXEL}) {
    if (auto* v = pass.pso->GetStaticVariableByName(stage, "GrassConstants")) v->Set(constants_);
  }
  if (auto* v = pass.pso->GetStaticVariableByName(SHADER_TYPE_VERTEX, "GrassInstances")) {
    v->Set(instances_->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
  }
  pass.pso->CreateShaderResourceBinding(&pass.srb, true);
  if (pass.srb) {
    if (auto* v = pass.srb->GetVariableByName(SHADER_TYPE_PIXEL, "g_plants"))
      v->Set(plants_ ? plants_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : placeholder_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    pass.plants = plants_ != nullptr;
  }
  return pass.srb ? &pass : nullptr;
}

void Grass::TimerBegin(IDeviceContext* context) {
  if (!REXCVAR_GET(native_grass_gpu_time) || queries_failed_ || query_active_) return;
  if (!queries_[0]) {
    if (device_->GetDeviceInfo().Features.DurationQueries != DEVICE_FEATURE_STATE_ENABLED) {
      queries_failed_ = true;
      return;
    }
    for (auto& query : queries_) {
      QueryDesc desc;
      desc.Name = "NativeXRenderer grass time";
      desc.Type = QUERY_TYPE_DURATION;
      device_->CreateQuery(desc, &query);
      if (!query) {
        queries_failed_ = true;
        return;
      }
    }
  }
  if (query_pending_[query_index_]) {
    QueryDataDuration data;
    if (queries_[query_index_]->GetData(&data, sizeof(data), true) && data.Frequency) {
      double ms = double(data.Duration) * 1000.0 / double(data.Frequency);
      timer_total_ms_ += ms;
      timer_max_ms_ = std::max(timer_max_ms_, ms);
      ++timer_frames_;
    }
    query_pending_[query_index_] = false;
  }
  context->BeginQuery(queries_[query_index_]);
  query_active_ = true;
}

void Grass::TimerEnd(IDeviceContext* context) {
  if (!query_active_) return;
  context->EndQuery(queries_[query_index_]);
  query_pending_[query_index_] = true;
  query_index_ = (query_index_ + 1) % kQueries;
  query_active_ = false;
  if (timer_frames_ >= 600) {
    REXLOG_INFO("[native_x_renderer] grass prototype GPU time: {:.3f} ms a frame on average over {} frames (the "
                "longest {:.3f} ms)",
                timer_total_ms_ / double(timer_frames_), timer_frames_, timer_max_ms_);
    timer_total_ms_ = timer_max_ms_ = 0.0;
    timer_frames_ = 0;
  }
}

void Grass::WriteConstants(IDeviceContext* context, const void* constants, size_t size) {
  void* data = nullptr;
  context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
  std::memcpy(data, constants, size);
  context->UnmapBuffer(constants_, MAP_WRITE);
}

// The plants' pictures (native_grass_textures, made by the game from the player's own files): a texture array with
// mips made here, each mip's alpha scaled so as much of it stays opaque at the alpha test as at full size (else far
// plants thin out and vanish), colour averaged over the opaque texels only (no dark fringes).
bool Grass::LoadPlants(IRenderDevice* device) {
  const std::string path = REXCVAR_GET(native_grass_textures);
  if (path.empty()) return false;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return false;
  std::ifstream f(std::filesystem::path(path), std::ios::binary);
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto u32 = [&](size_t at) {
    uint32_t v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, &d[at], 4);
    return v;
  };
  if (d.size() < 16 || std::memcmp(d.data(), "BFVG", 4) || u32(4) != 1) return false;
  const uint32_t kinds = std::min<uint32_t>(u32(8), 16), size = u32(12);
  if (kinds == 0 || size < 16 || size > 2048 || d.size() < 16 + size_t(kinds) * 16 + size_t(kinds) * size * size * 4)
    return false;
  for (uint32_t k = 0; k < kinds; ++k) std::memcpy(kind_info_[k], &d[16 + k * 16], 16);
  uint32_t mips = 1;
  while ((size >> mips) >= 4) ++mips;
  std::vector<std::vector<uint8_t>> levels(size_t(kinds) * mips);
  std::vector<TextureSubResData> sub(size_t(kinds) * mips);
  for (uint32_t k = 0; k < kinds; ++k) {
    const uint8_t* src = &d[16 + size_t(kinds) * 16 + size_t(k) * size * size * 4];
    std::vector<uint8_t>& top = levels[size_t(k) * mips];
    top.assign(src, src + size_t(size) * size * 4);
    auto coverage = [](const std::vector<uint8_t>& p, float scale) {
      size_t n = 0;
      for (size_t i = 3; i < p.size(); i += 4) n += p[i] * scale >= 127.5f;
      return double(n) / double(p.size() / 4);
    };
    const double want = coverage(top, 1.0f);
    for (uint32_t m = 1; m < mips; ++m) {
      const uint32_t s0 = size >> (m - 1), s1 = size >> m;
      const std::vector<uint8_t>& up = levels[size_t(k) * mips + m - 1];
      std::vector<uint8_t>& lv = levels[size_t(k) * mips + m];
      lv.assign(size_t(s1) * s1 * 4, 0);
      for (uint32_t y = 0; y < s1; ++y)
        for (uint32_t x = 0; x < s1; ++x) {
          float rgb[3] = {}, a = 0, wsum = 0;
          for (uint32_t j = 0; j < 2; ++j)
            for (uint32_t i = 0; i < 2; ++i) {
              const uint8_t* q = &up[(size_t(y * 2 + j) * s0 + x * 2 + i) * 4];
              const float w = q[3] / 255.0f;
              for (int c = 0; c < 3; ++c) rgb[c] += q[c] * w;
              a += q[3], wsum += w;
            }
          uint8_t* o = &lv[(size_t(y) * s1 + x) * 4];
          for (int c = 0; c < 3; ++c) o[c] = uint8_t(wsum > 0 ? rgb[c] / wsum : 0);
          o[3] = uint8_t(a / 4.0f + 0.5f);
        }
      // The alpha scale that keeps the top level's share of opaque texels (a search between 1 and 4).
      float lo = 1.0f, hi = 4.0f;
      if (coverage(lv, 1.0f) < want) {
        for (int it = 0; it < 12; ++it) {
          const float mid = 0.5f * (lo + hi);
          (coverage(lv, mid) < want ? lo : hi) = mid;
        }
        for (size_t i = 3; i < lv.size(); i += 4) lv[i] = uint8_t(std::min(255.0f, lv[i] * hi));
      }
    }
    for (uint32_t m = 0; m < mips; ++m) {
      TextureSubResData& r = sub[size_t(k) * mips + m];
      r.pData = levels[size_t(k) * mips + m].data();
      r.Stride = uint64_t(size >> m) * 4;
    }
  }
  TextureDesc desc;
  desc.Name = "NativeXRenderer grass plants";
  desc.Type = RESOURCE_DIM_TEX_2D_ARRAY;
  desc.Width = desc.Height = size;
  desc.ArraySize = kinds;
  desc.MipLevels = mips;
  desc.Format = TEX_FORMAT_RGBA8_UNORM;
  desc.Usage = USAGE_IMMUTABLE;
  desc.BindFlags = BIND_SHADER_RESOURCE;
  TextureData data{sub.data(), uint32_t(sub.size())};
  device->CreateTexture(desc, &data, &plants_);
  if (!plants_) return false;
  plant_kinds_ = kinds;
  for (auto& [key, pass] : draw_passes_)
    if (pass.srb) {
      if (auto* v = pass.srb->GetVariableByName(SHADER_TYPE_PIXEL, "g_plants"))
        v->Set(plants_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
      pass.plants = true;
    }
  REXLOG_INFO("[native_x_renderer] grass: {} plant pictures from the game ({} x {}, {} mips)", kinds, size, size, mips);
  return true;
}

bool Grass::Apply(IDeviceContext* context, const TerrainFrame& t, ITexture* color, ITextureView* color_rtv,
                  ITexture* depth, ITextureView* depth_dsv, uint64_t frame) {
  static uint32_t refused = 0;
  auto refuse = [&](const char* why) {
    if (refused++ < 5) REXLOG_WARN("[native_x_renderer] grass prototype: {}", why);
    return false;
  };
  if (!ready_ || !t.valid || !t.height) return refuse("no terrain this frame");
  if (!color || !color_rtv || !depth || !depth_dsv) return refuse("no scene targets");
  ITextureView* color_srv = color->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* depth_srv = depth->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  if (!color_srv || !depth_srv) return refuse("the scene targets have no shader resource views");
  // The plants' pictures, once the game has made them (looked for again every 300 frames).
  if (!plants_ && plant_tries_++ % 300 == 0) LoadPlants(device_);
  // D151: the level's grass map, when the game gives another (a level loading): a texture of each cell's distance to
  // ground that is not grass, made once.
  {
    std::lock_guard lock(g_map.mutex);
    if (g_map.version != map_version_) {
      map_version_ = g_map.version;
      map_.Release();
      map_state_ = g_map.state;
      if (g_map.state == 1) {
        TextureDesc desc;
        desc.Name = "NativeXRenderer grass map";
        desc.Type = RESOURCE_DIM_TEX_2D;
        desc.Width = g_map.width, desc.Height = g_map.height;
        desc.Format = TEX_FORMAT_R8_UNORM;
        desc.Usage = USAGE_IMMUTABLE;
        desc.BindFlags = BIND_SHADER_RESOURCE;
        TextureSubResData sub{g_map.distance.data(), g_map.width};
        TextureData init{&sub, 1};
        device_->CreateTexture(desc, &init, &map_);
        size_t grass = 0;
        for (uint8_t d : g_map.distance) grass += d > 0;
        map_info_[0] = g_map.x0, map_info_[1] = g_map.z0, map_info_[2] = g_map.cell;
        map_info_[3] = float(g_map.width), map_info_[4] = float(g_map.height), map_info_[5] = float(grass);
        if (map_) {
          REXLOG_INFO("[native_x_renderer] grass: the level's map, {} x {} cells of {:.2f} m from {:.0f}, {:.0f} ({:.0f} "
                      "m2 of grass): tufts only there, decided from the world alone (D151)",
                      g_map.width, g_map.height, g_map.cell, g_map.x0, g_map.z0,
                      double(grass) * g_map.cell * g_map.cell);
        } else {
          map_state_ = 0;
          REXLOG_WARN("[native_x_renderer] grass: the level's map ({} x {}) could not be made a texture", g_map.width,
                      g_map.height);
        }
      } else {
        REXLOG_INFO("[native_x_renderer] grass: {}", g_map.state < 0 ? "the level's map is being read (no grass yet)"
                                                                    : "no level map (grass from the screen's colour)");
      }
    }
  }
  const bool use_map = REXCVAR_GET(native_grass_map) && map_state_ != 0;
  // Clipped to each partition's depth range (the game's own draws clamp; the grass is drawn once per partition).
  Pass* draw = DrawPass(color_rtv->GetDesc().Format, depth_dsv->GetDesc().Format, t.depth_func, true, true);
  Pass* draw_fading = DrawPass(color_rtv->GetDesc().Format, depth_dsv->GetDesc().Format, t.depth_func, true, false);
  if (!draw || !draw_fading) return refuse("no draw pipeline");

  // The camera: the point the world-view-projection maps to clip x = y = w = 0 (solved with Cramer's rule on the
  // x, y and w rows).
  const float(*m)[4] = t.wvp;
  auto det3 = [](double a, double b, double c, double d, double e, double f, double g, double h, double i) {
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  };
  const int rows[3] = {0, 1, 3};
  double A[3][3], B[3];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) A[r][c] = m[rows[r]][c];
    B[r] = -double(m[rows[r]][3]);
  }
  double D = det3(A[0][0], A[0][1], A[0][2], A[1][0], A[1][1], A[1][2], A[2][0], A[2][1], A[2][2]);
  if (std::abs(D) < 1e-12) return refuse("the camera matrix is singular");
  double cam[3];
  for (int k = 0; k < 3; ++k) {
    double M[3][3];
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) M[r][c] = c == k ? B[r] : A[r][c];
    cam[k] = det3(M[0][0], M[0][1], M[0][2], M[1][0], M[1][1], M[1][2], M[2][0], M[2][1], M[2][2]) / D;
  }

  // The rings: the first to 45 m (or the whole radius below it), then each twice as far with cells twice the size,
  // so every ring has as many cells (a full map's 1440 m is six).
  const float radius = float(std::clamp(REXCVAR_GET(native_grass_radius), 5.0, 1440.0));
  const float first = std::min(radius, 45.0f);
  uint32_t rings = 1;
  while (rings < kMaxRings && first * float(1u << (rings - 1)) < radius - 0.5f) ++rings;
  float cell = float(std::clamp(REXCVAR_GET(native_grass_cell), 0.2, 4.0));
  uint32_t side = uint32_t(std::ceil(2.0f * first / cell)) + 1;
  if (side > kMaxSide) {
    cell = 2.0f * first / float(kMaxSide - 1);
    side = kMaxSide;
  }
  Constants k{};
  std::memcpy(k.wvp, t.wvp, sizeof(k.wvp));
  std::memcpy(k.scale, t.scale, 16);
  std::memcpy(k.offset, t.offset, 16);
  std::memcpy(k.uv, t.uv, 16);
  for (int i = 0; i < 3; ++i) k.ndc_scale[i] = t.ndc_scale[i], k.ndc_offset[i] = t.ndc_offset[i];
  k.camera[0] = float(cam[0]), k.camera[1] = float(cam[1]), k.camera[2] = float(cam[2]), k.camera[3] = radius;
  k.grid[0] = cell;
  k.grid[1] = float(side);
  k.grid[2] = std::floor(float(cam[0]) / cell) - float(side / 2);
  k.grid[3] = std::floor(float(cam[2]) / cell) - float(side / 2);
  for (uint32_t r = 0; r < rings; ++r) {
    const float c = cell * float(1u << r);
    k.level[r][0] = c;
    k.level[r][1] = std::floor(float(cam[0]) / c) - float(side / 2);
    k.level[r][2] = std::floor(float(cam[2]) / c) - float(side / 2);
    k.level[r][3] = r + 1 == rings ? radius : first * float(1u << r);
  }
  // The state is the grid's and the level's: made again (a new stamp) when either changes.
  const auto layout = std::make_tuple(side, rings, cell, first, t.height.RawPtr());
  if (layout != state_layout_) {
    if (state_stamp_ > 0 && state_resets_logged_ < 12) {
      ++state_resets_logged_;
      REXLOG_INFO("[native_x_renderer] grass: every tuft's state made again (frame {}): {}{}{}{}{}", frame,
                  std::get<0>(layout) != std::get<0>(state_layout_) ? "cells across, " : "",
                  std::get<1>(layout) != std::get<1>(state_layout_) ? "rings, " : "",
                  std::get<2>(layout) != std::get<2>(state_layout_) ? "cell size, " : "",
                  std::get<3>(layout) != std::get<3>(state_layout_) ? "first ring, " : "",
                  std::get<4>(layout) != std::get<4>(state_layout_) ? "the heightmap's view" : "");
    }
    state_layout_ = layout, ++state_stamp_, ++state_resets_;
  }
  k.lod[0] = float(rings);
  k.lod[1] = float(state_stamp_);
  k.lod[2] = REXCVAR_GET(native_grass_steady) ? 1.0f : 0.0f;
  k.lod[3] = draw->plants ? float(plant_kinds_) : 0.0f;
  std::memcpy(k.kinds, kind_info_, sizeof(k.kinds));
  k.viewport[0] = t.viewport.TopLeftX, k.viewport[1] = t.viewport.TopLeftY;
  k.viewport[2] = t.viewport.Width, k.viewport[3] = t.viewport.Height;
  k.depth[0] = t.viewport.MinDepth, k.depth[1] = t.viewport.MaxDepth;
  const uint8_t f = t.depth_func;
  k.depth[2] = (f == COMPARISON_FUNC_GREATER || f == COMPARISON_FUNC_GREATER_EQUAL) ? 1.0f : 0.0f;
  static const auto start = std::chrono::steady_clock::now();
  k.depth[3] = float(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
  k.params[0] = float(REXCVAR_GET(native_grass_density));
  k.params[1] = float(REXCVAR_GET(native_grass_height));
  k.params[2] = float(REXCVAR_GET(native_grass_slope));
  k.params[3] = float(REXCVAR_GET(native_grass_green));
  k.target[0] = float(color->GetDesc().Width), k.target[1] = float(color->GetDesc().Height);
  k.target[2] = float(t.height_size);
  {
    // A guest k_16 (unsigned normalized) heightmap: its 16 bits from whatever the host view reads them as (RenderDoc:
    // the R16_FLOAT view reads 0.0731 and the R16_SNORM one 0.349 where the guest value is 11438 / 65535).
    const TEXTURE_FORMAT f = t.height->GetDesc().Format;
    k.decode[0] = !t.height_unorm ? 0.0f
                  : f == TEX_FORMAT_R16_FLOAT ? 1.0f
                  : f == TEX_FORMAT_R16_SNORM ? 2.0f
                                              : 0.0f;
    k.decode[1] = float(int(f));
    k.decode[2] = float(REXCVAR_GET(native_grass_hue));
  }
  k.target[3] = float(REXCVAR_GET(native_grass_debug));
  // The water's picture, half the scene's size (made again when the scene's size changes): the last frame's, if it was
  // made in the last few frames.
  const uint32_t water_w = (color->GetDesc().Width + 1) / 2, water_h = (color->GetDesc().Height + 1) / 2;
  // The level's water height, once the game has it (its water mesh's): the water's picture is then not needed.
  const double water_level = REXCVAR_GET(native_grass_water_level);
  const bool level_known = water_level > -10000.0;
  k.water[3] = level_known ? float(water_level) : -1.0e9f;
  const bool water_passes = snap_.srb && water_pass_.srb && REXCVAR_GET(native_grass_water) && !level_known;
  if (water_passes && (!water_ || water_w != water_w_ || water_h != water_h_)) {
    auto make = [&](const char* name, TEXTURE_FORMAT format, RefCntAutoPtr<ITexture>& out) {
      out.Release();
      TextureDesc desc;
      desc.Name = name;
      desc.Type = RESOURCE_DIM_TEX_2D;
      desc.Width = water_w, desc.Height = water_h;
      desc.Format = format;
      desc.Usage = USAGE_DEFAULT;
      desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
      device_->CreateTexture(desc, nullptr, &out);
    };
    make("NativeXRenderer grass scene snapshot colour", TEX_FORMAT_RGBA16_FLOAT, snap_color_);
    make("NativeXRenderer grass scene snapshot depth", TEX_FORMAT_R32_FLOAT, snap_depth_);
    make("NativeXRenderer grass water picture", TEX_FORMAT_R8_UNORM, water_);
    water_w_ = water_w, water_h_ = water_h;
    water_frame_ = 0;
    if (!snap_color_ || !snap_depth_ || !water_) {
      REXLOG_WARN("[native_x_renderer] grass: the water picture's textures could not be made");
      snap_color_.Release(), snap_depth_.Release(), water_.Release();
    }
  }
  const bool water_ok = water_passes && water_ && snap_color_ && snap_depth_;
  k.water[0] = water_ok && water_frame_ && frame >= water_frame_ && frame - water_frame_ <= 3 ? 1.0f : 0.0f;
  k.water[1] = float(water_w), k.water[2] = float(water_h);
  k.fix[0] = REXCVAR_GET(native_grass_handoff) ? 1.0f : 0.0f;
  k.fix[1] = float(std::clamp(REXCVAR_GET(native_grass_fade_band), 0.02, 0.45));
  k.fix[2] = REXCVAR_GET(native_grass_bounds) ? 1.0f : 0.0f;
  k.fix[3] = REXCVAR_GET(native_grass_fade_pass) ? 1.0f : 0.0f;
  k.pops[0] = REXCVAR_GET(native_grass_pops) ? 1.0f : 0.0f;
  k.pops[1] = float(applied_ + 2);
  const uint32_t list_start = kMaxSide * kMaxSide * kMaxRings / 2;
  k.lists[0] = float(list_start);
  // The decisions' pace in time (frame rates differ): the score's time constant, a whole tuft's fade.
  {
    const auto now = std::chrono::steady_clock::now();
    double dt = last_apply_ == std::chrono::steady_clock::time_point{} ? 1.0 / 60.0
                                                                         : std::chrono::duration<double>(now - last_apply_).count();
    last_apply_ = now;
    k.steps[0] = float(std::clamp(REXCVAR_GET(native_grass_settle), 0.02, 5.0));
    k.steps[1] = float(std::clamp(REXCVAR_GET(native_grass_fade_time), 0.02, 5.0));
    k.steps[2] = float(std::clamp(dt, 0.0, 0.25));
  }
  // D151: the level's grass map (1 used, -1 being read: no grass), its corner, cell and size, its edge rule.
  const uint32_t parity = uint32_t(applied_) & 1u;
  k.map[0] = !use_map ? 0.0f : map_state_ < 0 ? -1.0f : 1.0f;
  k.map[1] = map_info_[0], k.map[2] = map_info_[1], k.map[3] = std::max(map_info_[2], 0.01f);
  k.map2[0] = std::max(map_info_[3], 1.0f), k.map2[1] = std::max(map_info_[4], 1.0f);
  k.map2[2] = float(std::clamp(REXCVAR_GET(native_grass_edge), 0.0, 15.0));
  k.map2[3] = float(std::clamp(REXCVAR_GET(native_grass_edge_fade), 0.0, 15.0));
  k.map3[0] = float(parity);
  // D152: every tuft on the map, laid out once, then those within the distance and in view (needs the map passes).
  const bool all_mode = use_map && map_state_ == 1 && map_ && all_gen_.srb && all_cull_.srb;
  if (all_mode) {
    const float c0 = cell, x0 = map_info_[0], z0 = map_info_[1], mc = map_info_[2];
    const float fx0 = std::floor(x0 / c0), fz0 = std::floor(z0 / c0);
    k.all[0] = fx0, k.all[1] = fz0;
    k.all[2] = std::ceil((x0 + map_info_[3] * mc) / c0) - fx0, k.all[3] = std::ceil((z0 + map_info_[4] * mc) / c0) - fz0;
    k.all2[0] = float(kAllMax);
    const double pixels = std::clamp(REXCVAR_GET(native_grass_pixels), 0.1, 20.0);
    k.all2[1] = float(pixels * 0.5), k.all2[2] = float(pixels * 1.5);
    // The viewport's pixels for a metre seen a metre away: the y row's length, the NDC scale, half its height.
    const float fy = std::sqrt(t.wvp[1][0] * t.wvp[1][0] + t.wvp[1][1] * t.wvp[1][1] + t.wvp[1][2] * t.wvp[1][2]);
    k.all2[3] = fy * std::abs(t.ndc_scale[1]) * t.viewport.Height * 0.5f;
  }
  // A ground height asked for (GrassProbe): written by the placement, read back a few frames later.
  uint32_t probe_id = 0;
  {
    std::lock_guard lock(g_probe.mutex);
    if (g_probe.asked && !probe_copied_) {
      k.probe[0] = g_probe.x, k.probe[1] = g_probe.z, k.probe[2] = 1.0f, k.probe[3] = float(g_probe.id);
      probe_id = g_probe.id;
    }
  }
  if (probe_copied_ && frame >= probe_read_at_) {
    void* data = nullptr;
    context->MapBuffer(probe_read_, MAP_READ, MAP_FLAG_DO_NOT_WAIT, data);
    if (data) {
      uint32_t v[2];
      std::memcpy(v, data, sizeof(v));
      context->UnmapBuffer(probe_read_, MAP_READ);
      float ground;
      std::memcpy(&ground, &v[1], 4);
      std::lock_guard lock(g_probe.mutex);
      if (v[0] == g_probe.id) g_probe.asked = false, g_probe.answered = true, g_probe.ground = ground;
      probe_copied_ = false;
    }
  }
  WriteConstants(context, &k, sizeof(k));
  if (applied_ % 1800 == 0) {
    for (uint32_t p = 0; p < t.partition_count; ++p) {
      const auto& part = t.partitions[p];
      REXLOG_INFO("[native_x_renderer] grass prototype: depth partition {}: {} draws, viewport depth {:.5f}..{:.5f}, "
                  "wvp w row {:.4f} {:.4f} {:.4f} {:.4f}, z row {:.4f} {:.4f} {:.4f} {:.4f}",
                  p, part.draws, part.viewport.MinDepth, part.viewport.MaxDepth, part.wvp[3][0], part.wvp[3][1],
                  part.wvp[3][2], part.wvp[3][3], part.wvp[2][0], part.wvp[2][1], part.wvp[2][2], part.wvp[2][3]);
    }
    REXLOG_INFO("[native_x_renderer] grass prototype: camera {:.1f} {:.1f} {:.1f}; {} terrain draws (VS {:016X}); "
                "c20 {:.4f} {:.4f} {:.4f} {:.4f} c21 {:.2f} {:.2f} {:.2f} {:.2f} c22 {:.6f} {:.6f} {:.4f} {:.4f}; "
                "heightmap {}; viewport {:.0f},{:.0f} {:.0f}x{:.0f} depth {:.5f}..{:.5f} func {}; ndc {:.3f} {:.3f} "
                "{:.3f} + {:.3f} {:.3f} {:.3f}; {}x{} cells of {:.2f} m; heightmap view format {} (decode {})",
                cam[0], cam[1], cam[2], t.draws, t.vertex_shader, t.scale[0], t.scale[1], t.scale[2], t.scale[3],
                t.offset[0], t.offset[1], t.offset[2], t.offset[3], t.uv[0], t.uv[1], t.uv[2], t.uv[3],
                t.height_size, t.viewport.TopLeftX, t.viewport.TopLeftY, t.viewport.Width, t.viewport.Height,
                t.viewport.MinDepth, t.viewport.MaxDepth, int(t.depth_func), t.ndc_scale[0], t.ndc_scale[1],
                t.ndc_scale[2], t.ndc_offset[0], t.ndc_offset[1], t.ndc_offset[2], side, side, cell, k.decode[1], k.decode[0]);
  }
  ++applied_;

  const uint32_t count = side * side * rings;
  TimerBegin(context);
  // The draw's arguments: 18 vertices (three cards), the tufts the placement appends.
  const uint32_t draw_args[8] = {18, 0, 0, 0, 18, 0, 0, list_start};
  context->UpdateBuffer(args_, 0, sizeof(draw_args), draw_args, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  // D151: this frame's half of the rings' average colours, emptied (the other half is the last frame's).
  {
    const uint32_t zero[kMaxRings * 4] = {};
    context->UpdateBuffer(average_, parity * sizeof(zero), sizeof(zero), zero, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  }
  // Every 600 frames, the counts (tufts drawn, tufts that came or went) read back and logged.
  if (stats_read_at_ && frame >= stats_read_at_) {
    void* data = nullptr;
    context->MapBuffer(stats_read_, MAP_READ, MAP_FLAG_DO_NOT_WAIT, data);
    if (data) {
      uint32_t v[8];
      std::memcpy(v, data, sizeof(v));
      context->UnmapBuffer(stats_read_, MAP_READ);
      if (REXCVAR_GET(native_grass_pops))
        REXLOG_INFO("[native_x_renderer] grass pops: per frame {:.2f} by their decision, {:.2f} by their root seen at "
                    "last, {:.2f} by their fade (tufts on screen whose opacity jumped by a quarter or more)",
                    double(v[3]) / stats_frames_, double(v[7]) / stats_frames_, double(v[6]) / stats_frames_);
      REXLOG_INFO("[native_x_renderer] grass: {} rings to {:.0f} m ({} cells each, {:.2f} m in the first); per frame "
                  "{:.0f} tufts drawn, {:.1f} came or went ({}), {:.0f} places seen under water; the state made "
                  "again {} times in all",
                  rings, radius, side * side, cell, double(v[0]) / stats_frames_, double(v[1]) / stats_frames_,
                  all_mode ? "every tuft on the level's map"
                  : use_map ? "the level's map"
                  : REXCVAR_GET(native_grass_steady) ? "steady"
                                                     : "from each frame's pixel",
                  double(v[2]) / stats_frames_, state_resets_);
      const uint32_t zero[8] = {};
      context->UpdateBuffer(stats_, 0, sizeof(zero), zero, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      stats_frames_ = 0;
    }
    stats_read_at_ = 0;
  }
  ++stats_frames_;
  // D152: how many tufts the map's layout made, read back once for the log.
  if (all_count_pending_ && frame >= all_count_read_at_) {
    void* data = nullptr;
    context->MapBuffer(all_count_read_, MAP_READ, MAP_FLAG_DO_NOT_WAIT, data);
    if (data) {
      uint32_t n = 0;
      std::memcpy(&n, data, 4);
      context->UnmapBuffer(all_count_read_, MAP_READ);
      all_tufts_ = n;
      REXLOG_INFO("[native_x_renderer] grass: {} tufts laid out on the level's map{} (D152)", std::min(n, kAllMax),
                  n > kAllMax ? " (of " + std::to_string(n) + ": no room for more)" : std::string());
      all_count_pending_ = false;
    }
  }
  // Placement: the scene's colour and depth read as textures, so neither stays bound as a target.
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  if (all_mode) {
    // The layout, when the map or its rules change (a level loading; the density, edge or plants changed).
    const auto key = std::make_tuple(map_version_, k.params[0], k.map2[2], k.map2[3], cell, k.lod[3]);
    if (!all_made_ || key != all_key_) {
      all_key_ = key, all_made_ = true;
      const uint32_t zero[4] = {};
      context->UpdateBuffer(all_count_, 0, sizeof(zero), zero, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      static const std::vector<uint8_t> no_colours(size_t(kAllMax) * 16, 0);
      context->UpdateBuffer(all_state_, 0, no_colours.size(), no_colours.data(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      if (auto* v = all_gen_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_map"))
        v->Set(map_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
      context->SetPipelineState(all_gen_.pso);
      context->CommitShaderResources(all_gen_.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      const uint32_t cells = uint32_t(k.all[2]) * uint32_t(k.all[3]), groups = (cells + 63) / 64;
      context->DispatchCompute(DispatchComputeAttribs(std::min(groups, 32768u), (groups + 32767) / 32768, 1));
      context->CopyBuffer(all_count_, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, all_count_read_, 0, 16,
                          RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      all_count_pending_ = true, all_count_read_at_ = frame + 4;
    }
    all_cull_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_height")->Set(t.height);
    all_cull_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_sceneColor")->Set(color_srv);
    all_cull_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_sceneDepth")->Set(depth_srv);
    all_cull_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "GrassInstancesOut")
        ->Set(instances_->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    context->SetPipelineState(all_cull_.pso);
    context->CommitShaderResources(all_cull_.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    const uint32_t groups = (kAllMax + 63) / 64;
    context->DispatchCompute(DispatchComputeAttribs(std::min(groups, 32768u), (groups + 32767) / 32768, 1));
  } else {
  place_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_height")->Set(t.height);
  place_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_sceneColor")->Set(color_srv);
  place_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_sceneDepth")->Set(depth_srv);
  place_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "GrassInstancesOut")
      ->Set(instances_->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
  if (auto* v = place_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_water"))
    v->Set(water_ok ? water_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : depth_srv);
  if (auto* v = place_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_map"))
    v->Set((map_ ? map_ : map_none_)->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  context->SetPipelineState(place_.pso);
  context->CommitShaderResources(place_.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->DispatchCompute(DispatchComputeAttribs((count + 63) / 64, 1, 1));
  }
  if (!stats_read_at_ && stats_frames_ >= 600) {
    context->CopyBuffer(stats_, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, stats_read_, 0, 32,
                        RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    stats_read_at_ = frame + 4;
  }
  if (probe_id && probe_read_) {
    context->CopyBuffer(stats_, 16, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, probe_read_, 0, 8,
                        RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    probe_copied_ = true;
    probe_read_at_ = frame + 3;
  }

  // The tufts: three cards of two triangles each.
  ITextureView* rtv = color_rtv;
  context->SetRenderTargets(1, &rtv, depth_dsv, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  for (uint32_t p = 0; p < t.partition_count; ++p) {
    const TerrainFrame::Partition& part = t.partitions[p];
    std::memcpy(k.wvp, part.wvp, sizeof(k.wvp));
    for (int i = 0; i < 3; ++i) k.ndc_scale[i] = part.ndc_scale[i], k.ndc_offset[i] = part.ndc_offset[i];
    k.viewport[0] = part.viewport.TopLeftX, k.viewport[1] = part.viewport.TopLeftY;
    k.viewport[2] = part.viewport.Width, k.viewport[3] = part.viewport.Height;
    k.depth[0] = part.viewport.MinDepth, k.depth[1] = part.viewport.MaxDepth;
    WriteConstants(context, &k, sizeof(k));
    context->SetViewports(1, &part.viewport, 0, 0);
    context->SetScissorRects(1, &part.scissor, 0, 0);
    // The whole tufts, then the fading ones over them (their own list, depth not written).
    for (int list = 0; list < 2; ++list) {
      Pass* pass = list == 0 ? draw : draw_fading;
      context->SetPipelineState(pass->pso);
      context->CommitShaderResources(pass->srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      DrawIndirectAttribs attribs;
      attribs.pAttribsBuffer = args_;
      attribs.DrawArgsOffset = list * 16;
      attribs.AttribsBufferStateTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
      context->DrawIndirect(attribs);
    }
  }
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  // The scene as it is now (the grass in it), for the water's picture after the water is drawn (MarkWater).
  snapped_ = false;
  if (water_ok) {
    WriteConstants(context, &k, sizeof(k));
    snap_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_sceneColor")->Set(color_srv);
    snap_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_sceneDepth")->Set(depth_srv);
    snap_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_snapColor")->Set(snap_color_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
    snap_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_snapDepth")->Set(snap_depth_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
    context->SetPipelineState(snap_.pso);
    context->CommitShaderResources(snap_.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->DispatchCompute(DispatchComputeAttribs((water_w + 7) / 8, (water_h + 7) / 8, 1));
    snap_constants_.assign(reinterpret_cast<const uint8_t*>(&k), reinterpret_cast<const uint8_t*>(&k) + sizeof(k));
    snap_depth_texture_ = depth;
    snap_scene_w_ = color->GetDesc().Width, snap_scene_h_ = color->GetDesc().Height;
    snap_frame_ = frame;
    snapped_ = true;
  }
  TimerEnd(context);
  return true;
}

bool Grass::MarkWater(IDeviceContext* context, ITexture* finished, ITexture* depth) {
  static uint64_t calls = 0, refused[4] = {};
  if (++calls % 600 == 0)
    REXLOG_INFO("[native_x_renderer] grass: water picture: {} calls; refused: {} no snapshot, {} no finished scene or "
                "depth, {} no views; the last made at frame {}",
                calls, refused[0], refused[1], refused[3], water_frame_);
  auto refuse = [&](int why) {
    ++refused[why];
    return false;
  };
  if (!snapped_) return refuse(0);
  snapped_ = false;
  // The depth target the snapshot was made from (by the finished scene's copy the renderer no longer has it as the
  // scene's: the game draws its first person weapon after a depth clear).
  if (!depth) depth = snap_depth_texture_;
  snap_depth_texture_.Release();
  if (!finished || !depth || snap_constants_.size() != sizeof(Constants)) return refuse(1);
  ITextureView* finished_srv = finished->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* depth_srv = depth->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  if (!finished_srv || !depth_srv) return refuse(3);
  Constants k;
  std::memcpy(&k, snap_constants_.data(), sizeof(k));
  k.finish[0] = float(finished->GetDesc().Width) / float(std::max(1u, snap_scene_w_));
  k.finish[1] = float(finished->GetDesc().Height) / float(std::max(1u, snap_scene_h_));
  WriteConstants(context, &k, sizeof(k));
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  water_pass_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_snapColor")->Set(snap_color_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  water_pass_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_snapDepth")->Set(snap_depth_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  water_pass_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_finished")->Set(finished_srv);
  water_pass_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_sceneDepth")->Set(depth_srv);
  water_pass_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_waterOut")->Set(water_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
  context->SetPipelineState(water_pass_.pso);
  context->CommitShaderResources(water_pass_.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->DispatchCompute(DispatchComputeAttribs((water_w_ + 7) / 8, (water_h_ + 7) / 8, 1));
  water_frame_ = snap_frame_;
  if (!water_logged_) {
    water_logged_ = true;
    REXLOG_INFO("[native_x_renderer] grass: the water's picture made ({} x {}; the finished scene {} x {})", water_w_,
                water_h_, finished->GetDesc().Width, finished->GetDesc().Height);
  }
  return true;
}

}  // namespace native_x_renderer
