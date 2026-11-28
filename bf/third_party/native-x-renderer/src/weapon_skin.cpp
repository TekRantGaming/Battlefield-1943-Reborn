// NativeXRenderer - weapon skins on the player's own first person weapon (weapon_skin.h).

#include "weapon_skin.h"

#include <native_x_renderer/native_x_renderer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <initializer_list>
#include <mutex>
#include <utility>
#include <vector>

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RenderDevice.h"
#include "Texture.h"
#include "TextureView.h"
#include "RefCntAutoPtr.hpp"

#include <rex/logging.h>

#include "glsl.h"

using namespace Diligent;

namespace native_x_renderer {
float AdsBlendNow();  // dof.cpp: how far the player aims down the sights (0 to 1)
namespace {

std::atomic<int> g_skin{0};
std::atomic<int> g_magazine{0};  // SetWeaponSkinMagazine: 0 under the gun, 1 out to its left
std::atomic<int64_t> g_skin_at{0};  // steady clock ticks

constexpr const char* kVS = R"(#version 450
layout(location = 0) out vec2 v_uv;
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  v_uv = vec2(p.x, 1.0 - p.y);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The electric skin, our own (the user, 22 Nov 2026: "Let's get rid of the repaint entirely, and do our own ... The
// gold for the wood could even have a metallic shine to look more realistic"; of what came next, a black gun with
// blue arcs and effects drawn over the picture: "it looks like screenspace", "And it looks really low quality",
// "Can you make it a reactive skin on the model?"; "Please make it better. Find 'Thunderstorm' 'Lightning' 'Dark
// Smoke' 'Reactive Camo' 'Animated Weapon Skin' references to learn from to make this amazing").
//
// What those references have in common: an animated camo is on the surface itself, slow layers of a pattern drifting
// at different speeds (Call of Duty's Orion and its kind: panning textures, the slower the richer); cloud is a broad
// soft noise for its shape with a smaller one to break it up; a storm seen from above shows its lightning as a glow
// inside the cloud, hardly ever a bolt (NASA's photographs from the space station); a drawn bolt is a thin line where
// a noise crosses a value, a pixel wide by the screen's own derivatives, shaped anew at a set rate (the realtimevfx
// forum's "noise-based lightning"); a reactive skin answers what the player does.
//
// So the skin is a thunderstorm seen from above, on the gun: StormAt, worked out from a place on the gun's own
// texture, the clock and how stirred up it is.
// - Its metal is the storm: a dark deck of cloud drifting under a layer of billows drifting another way, shaded from
//   one side, the texture's own wear and edges kept over it.
// - Lightning inside it: in each stretch of the surface now and then a flash, a stroke and its return and a dying
//   glow, seen through the cloud (brightest where the billows are thin); in a stroke, a thin branching bolt.
// - Its wood gold as polished metal.
// - Reactive: each shot stirs it up (WeaponSkinShot: more flashes, sooner, brighter, easing off in a few seconds), as
//   does the strike down the magazine.
// The storm's dark body is painted into the gun's colour texture (kPS, a copy made 60 times a second); its light
// is drawn on the gun's own mesh after the game has shaded it (kOverPS, DrawPath::DrawSkinOver: the gun's draws again
// with their own vertex shader, the texture's place from interpolator 7, where the game's own pixel shaders
// 49750AD0B93EF9C5 and 234707FCE022853D read their colour from), so it is as bright by night as by day, sharp at the
// picture's own size, and on the gun and nothing else.
constexpr const char* kStorm = R"(
float h21(vec2 p) {
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}
float vnoise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(h21(i), h21(i + vec2(1.0, 0.0)), u.x), mix(h21(i + vec2(0.0, 1.0)), h21(i + vec2(1.0, 1.0)), u.x), u.y);
}
float fbm(vec2 p) {
  float s = 0.0, a = 0.5;
  for (int i = 0; i < 4; ++i) {
    s += a * vnoise(p);
    p = p * 2.03 + vec2(17.1, 9.2);
    a *= 0.5;
  }
  return s;
}
struct Storm {
  float cloud;  // how much cloud is here (0 a gap in it, 1 its thick)
  float shade;  // its top's light from one side (0 to 1)
  vec3 glow;    // the lightning's light here
};
// `uv` a place on the texture, `size` its texels, `t` the clock, `heat` how stirred up it is (0 to 1), `charge` how
// much of a new magazine's charge is here now (0 to 1: the surface's lightning all at once).
Storm StormAt(vec2 uv, vec2 size, float t, float heat, float charge) {
  Storm s;
  vec2 p = uv * size / 1024.0 * 3.2;
  // Two layers, each carried its own way and both bent by a slow third: the deck, and the billows over it.
  vec2 warp = vec2(fbm(p * 0.7 + vec2(t * 0.020, 3.1)), fbm(p * 0.7 + vec2(8.7, -t * 0.017))) - 0.5;
  vec2 q = p + warp * 1.6 + vec2(t * 0.012, -t * 0.008);
  vec2 q2 = p * 2.1 + warp * 2.4 + vec2(-t * 0.022, t * 0.015) + 11.0;
  float top = 1.0 - abs(2.0 * fbm(q2) - 1.0);
  float h = fbm(q) * 0.62 + top * 0.42;
  s.cloud = smoothstep(0.40, 0.80, h);
  // (Its height a little way toward the light: lit where it falls away that way.)
  float h_lit = fbm(q + vec2(0.07, 0.05)) * 0.62 + (1.0 - abs(2.0 * fbm(q2 + vec2(0.147, 0.105)) - 1.0)) * 0.42;
  s.shade = clamp(0.5 + (h - h_lit) * 5.0, 0.0, 1.0);
  // The flashes: each cell of the surface has its own clock; in some of its turns, a flash at a place of its own.
  float glow = 0.0, strokes = 0.0;
  vec2 cp = p * 0.85, cell = floor(cp);
  // (Fewer: the user, 22 Nov 2026, "reduce the frequency of the flashes". A cell's clock turns once in 8 s at rest,
  // and three turns in ten have a flash.)
  float rate = 0.12 + 0.8 * heat;
  for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
      vec2 cc = cell + vec2(dx, dy);
      float clock = t * rate + h21(cc + 3.3) * 9.0, turn = floor(clock), age = fract(clock) / rate;
      float pick = h21(cc + turn * 1.37);
      if (pick < 0.70 - 0.3 * heat) continue;
      vec2 mid = cc + 0.2 + 0.6 * vec2(h21(cc + turn * 2.11 + 1.0), h21(cc + turn * 3.7 + 5.0));
      float stroke = age < 0.06 ? 1.0 : (age > 0.10 && age < 0.17 ? 0.7 : 0.0);
      vec2 to = cp - mid;
      float fall = exp(-dot(to, to) / (0.07 + 0.10 * pick));
      glow += fall * max(stroke, 0.45 * exp(-age * 5.0));
      strokes += fall * fall * stroke;
    }
  // Seen through the cloud: most where the billows are thin, and on their lit sides.
  float through = 0.22 + 1.2 * (1.0 - smoothstep(0.25, 0.9, top)) + 0.7 * s.shade;
  // The bolt: a thin line where a noise crosses its middle, a pixel and a half wide whatever the size it is drawn
  // at, shaken anew 30 times a second; only in a stroke, near its flash.
  float tick = floor(t * 30.0);
  vec2 lq = q * 1.9 + 0.04 * (vec2(vnoise(p * 19.0 + tick * 7.13), vnoise(p * 19.0 + tick * 3.71 + 19.0)) - 0.5);
  float n = fbm(lq);
  float off = abs(n - 0.5) / max(fwidth(n), 1e-5);
  float near_stroke = clamp(strokes * 1.6 + charge, 0.0, 1.0);
  glow += charge * 0.9;
  s.glow = vec3(0.42, 0.66, 1.0) * glow * through * (0.55 + 0.6 * s.cloud) + vec3(0.25, 0.5, 1.0) * exp(-off / 6.0) * near_stroke * 0.5 +
           vec3(0.9, 0.96, 1.0) * (1.0 - smoothstep(0.0, 1.4, off)) * near_stroke * 1.6;
  return s;
}
)";
constexpr const char* kPSHead = R"(#version 450
layout(std140, set = 0, binding = 0) uniform SkinConstants {
  vec4 k;  // x the clock (seconds), y and z the texture's size, w the lightning's brightness in the texture
  vec4 k2; // x the most any channel may be, y how stirred up the storm is (0 to 1)
};
layout(set = 0, binding = 1) uniform sampler2D srcTex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
)";
constexpr const char* kPS = R"(
float Warmth(vec3 c) { return (c.r - c.b) / (max(c.r, max(c.g, c.b)) + 0.03); }
void main() {
  vec4 c = texture(srcTex, v_uv);
  float lum = dot(c.rgb, vec3(0.299, 0.587, 0.114));
  vec3 c1 = textureLod(srcTex, v_uv, 1.0).rgb, c4 = textureLod(srcTex, v_uv, 4.0).rgb;
  // The wood: told by its colour over a stretch of the texture (its coarse levels: a speck of rust is not wood).
  float wood = smoothstep(0.24, 0.38, 0.6 * Warmth(c4) + 0.4 * Warmth(c1));
  float lum4 = dot(c4, vec3(0.299, 0.587, 0.114));
  // The texture's own light and dark against its surroundings': its wear, edges and stamps (and the wood's grain).
  float detail = clamp(lum / (lum4 + 0.02), 0.0, 2.0);
  // The metal: the storm. Nearly black in its gaps, a blue grey on its lit tops.
  Storm s = StormAt(v_uv, k.yz, k.x, k2.y, 0.0);
  vec3 sky = mix(vec3(0.010, 0.013, 0.022), vec3(0.090, 0.100, 0.130), s.cloud * (0.25 + 0.75 * s.shade));
  vec3 metal = sky * (0.45 + 0.75 * detail) + vec3(0.09) * pow(lum, 3.0);
  // The gold: as polished metal, from a deep gold in the grain's hollows to pale highlights on its ridges, with
  // fine brushed streaks.
  float brushed = vnoise(vec2(v_uv.x * k.y * 0.035, v_uv.y * k.z * 0.9)) * 0.6 + vnoise(vec2(v_uv.x * k.y * 0.08, v_uv.y * k.z * 2.3)) * 0.4;
  float sheen = clamp(0.55 * detail + 0.55 * brushed - 0.25, 0.0, 1.0);
  sheen = sheen * sheen * (3.0 - 2.0 * sheen);
  vec3 gold = mix(vec3(0.33, 0.19, 0.035), vec3(1.25, 0.98, 0.50), sheen) * (0.55 + 0.75 * lum4);
  // (A little of the lightning in the colour too, where the game's light reaches it: its light proper is drawn on
  // the mesh after. Each channel at most k2.x: brighter overflows the game's colour target, see WeaponSkinMake. The
  // alpha is how much is wood, for that pass: the game's shaders read only the colour's three channels.)
  out_color = vec4(min(mix(metal, gold, wood) + s.glow * k.w * mix(1.0, 0.25, wood), vec3(k2.x)), wood);
}
)";
// The storm's light on the gun's own mesh (DrawPath::DrawSkinOver; its constants in the motion pass's buffer).
constexpr const char* kOverPSHead = R"(#version 450
layout(set = 0, binding = 0) uniform XeMotionConstants {
  vec4 ndc_to_pixels;  // (the motion pass's)
  vec4 sk;             // x the clock, y how stirred up the storm is, zw the texture's size
  vec4 sk2;            // x its brightness
  vec4 sk3;            // a new magazine's charge: xyz the magazine's well in the world, w the seconds since it went in (-1: none)
};
layout(set = 0, binding = 1) uniform sampler2D skinTex;  // the skinned copy: its alpha how much is wood
layout(location = 3) in vec4 in_world;  // the place in the world (the game's vertex shader's o3)
layout(location = 7) in vec4 in_uv;
layout(location = 0) out vec4 out_fx;
)";
constexpr const char* kOverPS = R"(
void main() {
  float wood = textureLod(skinTex, in_uv.xy, 0.0).a;
  // A new magazine's charge (the user, 22 Nov 2026: "Reloading should also make a cool lightning effect when the new
  // magazine is inserted that electrifies the weapon"): from the magazine's well it runs out over the gun both ways,
  // 2 m a second: a bright front, the surface's lightning all alight behind it, dying over a second.
  float charge = 0.0;
  if (sk3.w >= 0.0 && sk3.w < 1.6) {
    float from = distance(in_world.xyz, sk3.xyz), front = sk3.w * 2.0;
    float edge = exp(-(from - front) * (from - front) / (0.045 * 0.045));
    float behind = from < front ? exp(-(sk3.w - from / 2.0) * 3.5) : 0.0;
    charge = clamp(edge * 1.2 + behind, 0.0, 1.5) * (1.0 - smoothstep(1.1, 1.6, sk3.w));
  }
  Storm s = StormAt(in_uv.xy, sk.zw, sk.x, sk.y, min(charge, 1.0));
  // (Its alpha: the gun is here. The charge's front shows on the wood too.)
  out_fx = vec4((s.glow * mix(1.0, 0.25, wood) + vec3(0.55, 0.78, 1.0) * max(charge - 1.0, 0.0) * 2.0) * sk2.x, 1.0);
}
)";

// Over the picture, before the HUD, while the skin is worn (WeaponSkinBloom). The user, 22 Nov 2026: "low-cost little
// thunder clouds floating near the gun ... that strike the gun with mini-lightning which gives the gun a really cool
// electrical particle effect on it and sparking off around the gun"; then, of the first try (clouds, bolts and
// sparks drawn flat on the screen from the clock): "That looks really bad though. We can't make that more realistic
// looking and cool? Like some animated Call of Duty skins are? ... cool weapon effects used in Unity games with
// their Shuriken Particles"; "Maybe do particle smoke with lightning around the gun?" So, as those are made:
// particles in the gun's own space, born on its surface, hidden behind it where it is in front.
// - The gun on the screen: the storm's light as the mesh pass drew it (kOverPS: its alpha is the gun itself, not the
//   hands), a quarter the view's size with its mips (kBloomExtractPS).
// - Where the gun is (kAnchorPS, a texture of 7 texels, eased): its middle on the screen, its spread, how much of
//   the view it covers, and its middle and spread in the weapon's own view space (each of its pixels at its depth).
// - The storm's light laid over the picture, with a soft bloom of it (kBloomApplyPS).
// - The storm (the user, 22 Nov 2026, of bolts between places on the gun's surface, looping out through smoke born
//   on it: "this is looking bad with the 2D lightning bolts floating on the gun"; "It should look more like a 3D PBF
//   dark storm vortex rotating around the gun, with lightning in the vortex illuminating it from within and crawling
//   between plumes, and periodically a lightning bolt striking down the magazine"). The gun's place and long axis in
//   the weapon's own view space come from the arcs' places at their depths (kAnchorPS, kVortex GunFrame). Round that
//   axis: eight plumes go round, each giving off dark smoke that the vortex carries on and shears into bands (kFxCS,
//   60 times a second: particles kept by their place along the axis, their angle round it and their distance from
//   it, so the storm stays on the gun however it moves; carried by a set flow, not a fluid solved for its pressure).
//   Each a quad facing the view, hidden where the gun is in front of it (kParticleVS/PS), fainter between the eye
//   and the gun so the sights stay clear.
// - The lightning in it (kBoltVS/PS): bolts from one plume to another, as thick as their distance makes them, jagged
//   in two scales and shaped anew 30 times a second; a stroke and its return. Each lights the smoke near it from
//   within, and the gun (kBloomApplyPS, from each pixel's own depth).
// - The strike: every few seconds a larger bolt from the top of the storm onto the gun and down its magazine (under
//   the Thompson, out to the left of the Type 100: SetWeaponSkinMagazine), with sparks off its foot.
// - A blue bloom round the arcs (kBloomApplyPS).
// And then, the user, 22 Nov 2026: "Should just make it particle-based thick smoke wrapping around the gun from the
// back of the reciever to just before the muzzle. but color the smoke like thunder clouds". So the smoke is given
// off all along that stretch of the gun (bs[1].zw: from where to where along its axis, in parts of its half
// length), close round it and thick, turning slowly, in a thundercloud's colours (a dark slate underside, blue grey
// in its body, pale on its lit tops); the bolts, the strike and the sparks are left out (mg.w: 1 puts them back),
// only their soft light inside the smoke kept.
// And of that (a dark haze round the gun, run skin9_day): "Make it look less like smoke and more like particle clouds
// being pulled around the weapon, like hurricane spiral"; "rotate the vortex the opposite direction". So: the cloud
// is fed in at the back of the receiver from three places going round it, and drawn forward and inward toward the
// muzzle, turning the faster the nearer it is drawn in (as a hurricane's bands wind in to its eye): three spiral
// bands round the gun. Each puff a small cumulus (a few lobes with a clear edge, bright on its lit side, drawn out
// a little along its way), not a soft dark ball.
// Its size and look, the user, 22 Nov 2026, of the first storm (a haze 15 cm across, bolts as wide as a finger, a
// turn in three seconds, run skin6_day): "The smoke/cloud vortex needs to be closer to the gun, not filling the
// screen. and the lightning more subtle. Like viewing a mini-thunderstorm from above the clouds"; "Slow down the spin
// speed too". So: the plumes 3 to 4 cm from the gun's axis, a turn in ten seconds; a storm seen from above shows its
// lightning as a soft glow inside the cloud and hardly ever a bolt (NASA's photographs from the space station), so
// three bolts in five are only their light in the smoke; each puff is shaded as a ball, by the day's light from
// above and by the lightning from where it is (as smoke sprites are lit in games: Unity's six-way lighting).
constexpr const char* kBloomCommon = R"(#version 450
layout(std140, set = 0, binding = 0) uniform BloomConstants {
  vec4 a;  // xy the view's size (its part of the colour), zw the depth's part
  vec4 b;  // xy the quarter textures' size, z the bloom's strength, w a developer's view (1: the overlay alone)
  vec4 c;  // x the clock, y how much the storm shows (0 aiming down the sights), z the anchor's ease, w how dark it is (0 day, 1 night)
  vec4 d;  // x the lightning's flash now (0 to 1), y the step's seconds, z a number of this frame's, w the arcs' brightness the glow brings them to
  vec4 e;  // xy the weapon's view's projection (clip x = e.x * view x, clip w = view z), z the smoke's count, w all particles
  vec4 ba[2];  // the bolts' ages in seconds (-1: none): five in the storm, the sixth the strike
  vec4 bs[2];  // their seeds
  // The gun itself, from its own bones (DrawPath::KeepSkinDraw, WeaponSkinGunFrame), in the weapon's view space: the
  // back of its receiver (w 1: known this frame), its muzzle, the way its top faces, its magazine's top and end.
  // (gun_up.w: how far the player aims down the sights, 0 to 1: the cloud keeps out of the sight line.)
  vec4 gun_back, gun_muzzle, gun_up, gun_mag_top, gun_mag_end;
  vec4 mg;     // the magazine: x 0 under the gun, 1 out to its left; y its place along the gun from its middle (m), z its length (m); w 1: the bolts, the strike and the sparks drawn
};
float h11(float n) { return fract(sin(n * 127.1) * 43758.5453); }
float BoltAge(int k) { return k < 4 ? ba[0][k] : ba[1][k - 4]; }
float BoltSeed(int k) { return k < 4 ? bs[0][k] : bs[1][k - 4]; }
// A stroke, a pause, its return. The strike's are longer.
float BoltOn(float age) { return age < 0.0 ? 0.0 : (age < 0.05 ? 1.0 : (age > 0.085 && age < 0.15 ? 0.75 : 0.0)); }
// Its light in the cloud: the stroke, and after it a glow that flickers and dies away in a third of a second.
float GlowOn(float age) {
  return age < 0.0 ? 0.0 : max(BoltOn(age), 0.85 * exp(-age / 0.11) * (0.7 + 0.3 * sin(age * 90.0)));
}
float StrikeOn() { return mg.w > 0.5 ? BoltOn(BoltAge(5) * 0.55) : 0.0; }
float SegDist(vec3 p, vec3 s0, vec3 s1) {
  vec3 pa = p - s0, ba3 = s1 - s0;
  return length(pa - ba3 * clamp(dot(pa, ba3) / max(dot(ba3, ba3), 1e-9), 0.0, 1.0));
}
struct Particle {
  vec4 pa;  // smoke: its place along the gun's axis (m), its angle round it, its distance from it, w its age; a spark: its place in the view's space
  vec4 vl;  // smoke: x the distance from the axis it settles at; a spark: its velocity; w its life (0: none)
  vec4 ts;  // y its own number, z its size
};
)";
// (After the declaration of anchorTex.) The gun's middle in the weapon's view space.
constexpr const char* kVortex = R"(
const float kPlumes = 8.0;
struct Gun {
  vec3 mid, axis, up, side, mag;  // its middle and its long axis (away from the eye) in the weapon's view space; up and to a side of it; the way its magazine goes
  float half_len;
};
Gun GunFrame() {
  Gun g;
  if (gun_back.w > 0.5) {
    // From the gun's own model and bones: its barrel's line from the back of the receiver to the muzzle. The cloud is
    // fixed to it and cannot swing off it (by its outline in the picture it lagged and drifted in a reload).
    vec3 along = gun_muzzle.xyz - gun_back.xyz;
    g.mid = 0.5 * (gun_muzzle.xyz + gun_back.xyz);
    g.half_len = max(0.5 * length(along), 0.05);
    g.axis = normalize(along);
    g.up = normalize(gun_up.xyz - g.axis * dot(gun_up.xyz, g.axis));
    g.side = cross(g.axis, g.up);
    g.mag = normalize(gun_mag_end.xyz - gun_mag_top.xyz + vec3(1e-6));
    return g;
  }
  g.mid = texelFetch(anchorTex, ivec2(4, 0), 0).xyz;
  g.mid.z = max(g.mid.z, 0.12);
  // Its long axis: the way the arcs' places are spread the most (their covariance's first axis, by a few steps of
  // the power method).
  vec3 dg = texelFetch(anchorTex, ivec2(5, 0), 0).xyz, od = texelFetch(anchorTex, ivec2(6, 0), 0).xyz;
  mat3 m = mat3(dg.x, od.x, od.y, od.x, dg.y, od.z, od.y, od.z, dg.z);
  vec3 v = normalize(vec3(0.55, 0.25, 0.8));
  for (int i = 0; i < 8; ++i) v = normalize(m * v + v * 1e-8);
  if (v.z < 0.0) v = -v;
  g.axis = v;
  g.half_len = clamp(1.7 * sqrt(max(dot(v, m * v), 1e-7)), 0.10, 0.30);
  vec3 down = vec3(0.0, -1.0, 0.0);
  down = normalize(down - v * dot(down, v));
  g.up = -down;
  g.side = cross(v, g.up);
  vec3 way = mg.x > 0.5 ? vec3(-1.0, 0.0, 0.0) : vec3(0.0, -1.0, 0.0);
  g.mag = normalize(way - v * dot(way, v));
  return g;
}
// The gun's own shape along its barrel's line (the weapon export's models, runs\\export_out3: for each of 12
// stretches from the back of the receiver to the muzzle, how far the gun reaches from the line upward, downward,
// and to each side, metres; the grip, stock and magazine are cut off at 38 mm, so the cloud does not balloon round
// them). First the Thompson's, then the Type 100's (whose magazine goes out to the side).
const vec4 kShape[24] = vec4[24](
    vec4(0.036, 0.038, 0.021, 0.021), vec4(0.036, 0.038, 0.019, 0.019), vec4(0.031, 0.038, 0.018, 0.026), vec4(0.025, 0.038, 0.016, 0.021),
    vec4(0.018, 0.038, 0.020, 0.021), vec4(0.018, 0.038, 0.020, 0.021), vec4(0.018, 0.038, 0.026, 0.026), vec4(0.016, 0.026, 0.024, 0.023),
    vec4(0.013, 0.014, 0.012, 0.012), vec4(0.013, 0.020, 0.016, 0.016), vec4(0.022, 0.030, 0.017, 0.017), vec4(0.020, 0.015, 0.016, 0.016),
    vec4(0.025, 0.038, 0.018, 0.024), vec4(0.019, 0.038, 0.024, 0.024), vec4(0.014, 0.036, 0.017, 0.018), vec4(0.012, 0.034, 0.015, 0.016),
    vec4(0.012, 0.036, 0.016, 0.018), vec4(0.012, 0.038, 0.020, 0.030), vec4(0.020, 0.038, 0.025, 0.038), vec4(0.013, 0.038, 0.021, 0.038),
    vec4(0.012, 0.038, 0.014, 0.038), vec4(0.012, 0.038, 0.012, 0.030), vec4(0.020, 0.038, 0.013, 0.014), vec4(0.012, 0.036, 0.013, 0.014));
// How far the gun's surface is from its barrel's line at a part of its stretch (0 the back of the receiver, 1 the
// muzzle) in the way of an angle round it (0 up, a quarter turn to the side that is the model's -x): the cloud is
// formed to it (the user, 22 Nov 2026: "Can we also have it tighter and more formed to the guns?").
float GunReach(float part, float angle) {
  float at = clamp(part, 0.0, 0.999) * 11.0;
  int first = int(at) + (mg.x > 0.5 ? 12 : 0);
  vec4 shape = mix(kShape[first], kShape[first + 1], fract(at));
  float c = cos(angle), s = sin(angle);
  float tall = c > 0.0 ? shape.x : shape.y, wide = s > 0.0 ? shape.z : shape.w;
  return 1.0 / sqrt(c * c / (tall * tall) + s * s / (wide * wide));
}
// While aiming (gun_up.w) the Thompson's storm stands a little off it. Seen down its sights the gun is end on, and the
// Thompson's wide receiver at the eye hid the whole cloud, where the Type 100's narrower one leaves wisps showing at its
// sides (the user, 28 Nov 2026: "the new skin is disappearing when a player ADSs with iron sights"; then, of runs aim_smg
// and aim_jp: "The Type 100 was perfect. The Thompson just needed to show it's smoke on the side like the Type-100").
// So the Thompson's (mg.x 0: its magazine under it) only, out to just past its receiver's outline as seen (8.5 cm a
// metre of the distance from the eye); the Type 100 stays as it is.
vec3 Around(Gun g, vec3 cyl) {
  float r = cyl.z;
  if (gun_up.w > 0.0 && mg.x < 0.5) {
    float z = max((g.mid + g.axis * cyl.x).z, 0.06);
    r = mix(r, max(r, 0.085 * z), gun_up.w);
  }
  return g.mid + g.axis * cyl.x + (g.up * cos(cyl.y) + g.side * sin(cyl.y)) * r;
}
// A plume's middle: along the axis, its angle round it (all go round together, each a little by itself), its distance.
vec3 PlumeCyl(Gun g, float k, float t) {
  float h1 = h11(k * 3.7 + 1.0), h2 = h11(k * 9.1 + 2.0);
  return vec3((h1 * 2.0 - 1.0) * 0.65 * g.half_len + 0.015 * sin(t * 0.3 + k * 1.3),
              k * 6.2832 / kPlumes - 0.6 * t + 0.25 * sin(t * 0.3 + k * 1.9), 0.030 + 0.012 * h2);
}
// A bolt of the storm: from one plume to another near it.
void BoltEnds(Gun g, int b, out vec3 A, out vec3 B) {
  float seed = BoltSeed(b);
  float k0 = floor(h11(seed * 1.7 + float(b)) * kPlumes);
  float k1 = mod(k0 + 1.0 + floor(h11(seed * 5.3 + 2.0) * 2.0), kPlumes);
  A = Around(g, PlumeCyl(g, k0, c.x));
  B = Around(g, PlumeCyl(g, k1, c.x));
}
// The strike: from the top of the storm (p0) onto the gun over its magazine (p1) and down the magazine to its end
// (p2), on the eye's side of it.
void StrikePoints(Gun g, out vec3 p0, out vec3 p1, out vec3 p2) {
  vec3 base = g.mid + g.axis * mg.y;
  vec3 eye = -normalize(base);
  p0 = base + g.up * 0.06 + g.axis * 0.02 * (h11(BoltSeed(5)) - 0.5);
  p1 = base + g.up * 0.03 + eye * 0.012;
  p2 = base + g.mag * mg.z + eye * 0.02;
}
// How much the lightning lights a place: the storm's bolts near it, and the strike.
float LightningAt(Gun g, vec3 p) {
  float light = 0.0;
  for (int b = 0; b < 5; ++b) {
    float on = BoltOn(BoltAge(b));
    if (on <= 0.0) continue;
    vec3 A, B;
    BoltEnds(g, b, A, B);
    float dist = SegDist(p, A, B);
    light += on / (1.0 + dist * dist / (0.028 * 0.028));
  }
  float on = StrikeOn();
  if (on > 0.0) {
    vec3 p0, p1, p2;
    StrikePoints(g, p0, p1, p2);
    float dist = min(SegDist(p, p0, p1), SegDist(p, p1, p2));
    light += 1.8 * on / (1.0 + dist * dist / (0.04 * 0.04));
  }
  // (A new magazine's charge: from the magazine's well, dying in half a second.)
  if (ba[1].z >= 0.0 && gun_back.w > 0.5) {
    vec3 to = gun_mag_top.xyz - p;
    light += 2.5 * exp(-ba[1].z * 4.0) / (1.0 + dot(to, to) / (0.07 * 0.07));
  }
  return light;
}
// The way to the lightning from a place (xyz, not of length one: the nearer and brighter, the longer) for a puff's
// shading.
vec3 LightningFrom(Gun g, vec3 p) {
  vec3 way = vec3(0.0);
  for (int b = 0; b < 5; ++b) {
    float on = GlowOn(BoltAge(b));
    if (on <= 0.0) continue;
    vec3 A, B;
    BoltEnds(g, b, A, B);
    vec3 to = 0.5 * (A + B) - p;
    way += normalize(to + vec3(1e-5)) * on / (1.0 + dot(to, to) / (0.060 * 0.060));
  }
  float on = StrikeOn();
  if (on > 0.0) {
    vec3 p0, p1, p2;
    StrikePoints(g, p0, p1, p2);
    vec3 to = p1 - p;
    way += normalize(to + vec3(1e-5)) * 1.8 * on / (1.0 + dot(to, to) / (0.04 * 0.04));
  }
  if (ba[1].z >= 0.0 && gun_back.w > 0.5) {
    vec3 to = gun_mag_top.xyz - p;
    way += normalize(to + vec3(1e-5)) * 2.5 * exp(-ba[1].z * 4.0) / (1.0 + dot(to, to) / (0.07 * 0.07));
  }
  return way;
}
)";
constexpr const char* kBloomExtractPS = R"(
layout(set = 0, binding = 1) uniform sampler2D fxTex;
layout(location = 0) out vec4 out_color;
void main() {
  vec2 at = floor(gl_FragCoord.xy) * 4.0;  // this quarter pixel's first view pixel
  vec4 sum = vec4(0.0);
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 2; ++x) {
      vec2 p = at + vec2(x, y) * 2.0 + 1.0;
      sum += texelFetch(fxTex, ivec2(min(p * a.zw / a.xy, a.zw - 1.0)), 0);
    }
  out_color = sum * 0.25;  // the storm's light, and in its alpha how much is the gun
}
)";
constexpr const char* kAnchorPS = R"(
layout(set = 0, binding = 1) uniform sampler2D bloomTex;
layout(set = 0, binding = 2) uniform sampler2D depthTex;
layout(location = 0) out vec4 out_color;
void main() {
  int i = int(gl_FragCoord.x);
  float aspect = a.x / a.y;
  vec2 used = a.xy * 0.25 / b.xy;  // the view's part of the quarter texture
  vec2 proj = max(e.xy, vec2(0.1));
  float W = 0.0, Wz = 0.0;
  vec2 M = vec2(0.0);
  vec3 S = vec3(0.0), M3 = vec3(0.0), D3 = vec3(0.0), O3 = vec3(0.0);
  // (24 by 10 places of the view. One pixel's shader goes through them one after another, which the GPU cannot
  // spread: 80 by 36 took 0.55 ms by themselves, run skin13_nofx. The cloud is placed by the gun's bones now, and
  // this is only for the overlay's part of the picture and for a gun whose bones are not known.)
  for (int y = 0; y < 10; ++y)
    for (int x = 0; x < 24; ++x) {
      vec2 uv = (vec2(x, y) + 0.5) / vec2(24.0, 10.0);
      vec4 t = textureLod(bloomTex, uv * used, 3.0);
      vec2 p = vec2(uv.x * aspect, uv.y);  // in screen heights
      W += t.a;
      M += t.a * p;
      S += t.a * vec3(p.x * p.x, p.y * p.y, p.x * p.y);
      // In the weapon's view space, where the picture there is near (the gun, the hands).
      float depth = texelFetch(depthTex, ivec2(min(uv * a.zw, a.zw - 1.0)), 0).r;
      if (depth > 0.0667) {
        float z = 0.1 / depth;
        vec3 q = vec3((uv.x * 2.0 - 1.0) * z / proj.x, (1.0 - uv.y * 2.0) * z / proj.y, z);
        Wz += t.a;
        M3 += t.a * q;
        D3 += t.a * q * q;
        O3 += t.a * vec3(q.x * q.y, q.x * q.z, q.y * q.z);
      }
    }
  float sum = max(W, 1e-6), sum3 = max(Wz, 1e-6);
  vec2 mid = M / sum;
  vec3 cov = S / sum - vec3(mid.x * mid.x, mid.y * mid.y, mid.x * mid.y);
  vec3 mid3 = M3 / sum3;
  float cover = W / 240.0;                  // how much of the view the gun covers
  float seen = step(2e-3, cover) * c.z;     // (unseen: where it was last)
  // (The gun's place and axis in its view space are taken as they are now, not eased: eased, the cloud lagged behind
  // the gun and swung off it in a reload, the user, 22 Nov 2026: "keep the vortex locked to the weapon model, It
  // sways off the gun when realoading".)
  float seen3 = step(2e-3, cover) * step(1e-6, Wz);
  if (i == 0) out_color = vec4(mid, 0.0, seen);
  else if (i == 1) out_color = vec4(cov, seen);
  else if (i == 2) out_color = vec4(0.0);
  else if (i == 3) out_color = vec4(cover, 0.0, 0.0, c.z);
  else if (i == 4) out_color = vec4(mid3, seen3);
  else if (i == 5) out_color = vec4(D3 / sum3 - mid3 * mid3, seen3);
  else out_color = vec4(O3 / sum3 - vec3(mid3.x * mid3.y, mid3.x * mid3.z, mid3.y * mid3.z), seen3);
}
)";
// The glow's and the bloom's triangle: over the gun's part of the picture only (the anchor's), none while no arc
// is seen. (Over the whole picture the two cost 0.25 ms at 3440 x 1440, run skin2_day.)
constexpr const char* kAreaVS = R"(
layout(set = 0, binding = 3) uniform sampler2D anchorVsTex;
void main() {
  vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));  // (0,0) (2,0) (0,2)
  vec2 C = texelFetch(anchorVsTex, ivec2(0, 0), 0).xy;
  vec3 cov = texelFetch(anchorVsTex, ivec2(1, 0), 0).xyz;
  float cover = texelFetch(anchorVsTex, ivec2(3, 0), 0).r;
  float tr = 0.5 * (cov.x + cov.y), df = 0.5 * (cov.x - cov.y);
  float s = clamp(sqrt(max(tr + sqrt(df * df + cov.z * cov.z), 1e-6)), 0.05, 0.20);
  float reach = 2.7 * s + 0.08;
  // (In screen heights from the top left; a triangle twice the box's size holds the box.)
  vec2 at = C - reach + corner * 2.0 * reach;
  vec2 ndc = vec2(at.x / (a.x / a.y) * 2.0 - 1.0, 1.0 - at.y * 2.0);
  gl_Position = cover > 1e-3 ? vec4(ndc, 0.0, 1.0) : vec4(2.0, 2.0, 0.0, 1.0);
}
)";
constexpr const char* kBloomApplyPS = R"(
layout(set = 0, binding = 1) uniform sampler2D bloomTex;
layout(set = 0, binding = 2) uniform sampler2D anchorTex;
layout(set = 0, binding = 4) uniform sampler2D depthTex;
layout(set = 0, binding = 5) uniform sampler2D fxTex;
layout(location = 0) out vec4 out_color;
)";
constexpr const char* kBloomApplyPSMain = R"(
void main() {
  // The storm's light as the mesh pass drew it, and a soft bloom of it.
  vec2 at = gl_FragCoord.xy * a.zw / a.xy;
  vec4 fx = textureLod(fxTex, at / vec2(textureSize(fxTex, 0)), 0.0);
  vec2 uv = gl_FragCoord.xy * 0.25 / b.xy;
  vec3 bloom = textureLod(bloomTex, uv, 1.0).rgb * 0.45 + textureLod(bloomTex, uv, 2.0).rgb * 0.35 + textureLod(bloomTex, uv, 3.0).rgb * 0.20;
  vec3 add = fx.rgb * d.w + bloom * b.z;
  // The storm's bolts' light on the gun: by each of its pixels' own place (its depth).
  float depth = texelFetch(depthTex, ivec2(min(at, a.zw - 1.0)), 0).r;
  if (fx.a > 0.5 && depth > 0.0667 && e.x > 0.01 && c.y > 0.0 && d.x > 0.0) {
    float z = 0.1 / depth;
    vec2 v = gl_FragCoord.xy / a.xy;
    vec3 p = vec3((v.x * 2.0 - 1.0) * z / e.x, (1.0 - v.y * 2.0) * z / e.y, z);
    add += vec3(0.30, 0.55, 1.0) * LightningAt(GunFrame(), p) * 0.13 * c.y * (c.w < 0.0 ? 1.0 : mix(1.0, 0.22, c.w));
  }
  out_color = vec4(add, b.w > 0.5 ? 1.0 : 0.0);
}
)";
constexpr const char* kFxCS = R"(
layout(local_size_x = 64) in;
layout(set = 0, binding = 2) uniform sampler2D anchorTex;
layout(std430, set = 0, binding = 4) buffer WeaponFxParticles { Particle g_p[]; };
)";
constexpr const char* kFxCSMain = R"(
uint mixed(uint x) {
  x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
  return x;
}
float rnd(inout uint s) {
  s = mixed(s + 0x9E3779B9u);
  return float(s & 0xFFFFFFu) / 16777216.0;
}
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= uint(e.w)) return;
  uint s = mixed(i * 747796405u + uint(d.z * 16777216.0));
  float dt = d.y, t = c.x;
  bool seen = texelFetch(anchorTex, ivec2(3, 0), 0).r > 2e-3 && c.y > 0.05;
  Gun g = GunFrame();
  Particle p = g_p[i];
  bool smoke = float(i) < e.z;
  if (p.vl.w > 0.0) {
    p.pa.w += dt;
    if (p.pa.w >= p.vl.w) {
      p.vl.w = 0.0;
    } else if (smoke) {
      // Carried round the gun: faster near it than far (so its smoke is drawn out into bands), drawn to the
      // distance it settles at (the gun's own section there), wandering a little in and out.
      float r = max(p.pa.z, 0.012);
      // How far along its stretch of the gun it is (0 the back of the receiver, 1 the muzzle).
      float part = clamp((p.pa.x / g.half_len - bs[1].z) / max(bs[1].w - bs[1].z, 0.05), 0.0, 1.2);
      // (The gun's section where the puff is on the gun itself, 0 the back of the receiver and 1 the muzzle: `part`
      // is its place in the cloud's stretch, which begins a third of the way along the gun, so with it the cloud
      // followed the receiver's section over the magazine and the fore end's over the barrel, on both guns.)
      float on_gun = clamp(p.pa.x / g.half_len * 0.5 + 0.5, 0.0, 1.0);
      float settle = GunReach(on_gun, p.pa.y) + 0.004 + (p.vl.x - 0.85) * 0.022;
      // One even spiral on both guns (the user, 22 Nov 2026: "more of a uniform spiral around both guns. It's a little
      // too dense on the Type 100, whereas it's more sparse on the Thompson"): the turn was by the puff's distance
      // from the barrel's line, 0.55 * 0.036 / r a second, so the smoke wound nearly twice as fast round the Type 100's
      // thin barrel as round the Thompson's body and bunched there. Now the turn is tied to the way along: a turn and a fifth
      // from the stretch's start to the muzzle, whatever the gun's girth or length, so the two bands lie
      // as the same spiral on both. (Still the way the user asked: "rotate the vortex the opposite direction".)
      p.pa.y -= 1.2 * 6.2832 * 2.0 * (0.087 + 0.053 * part) / max(bs[1].w - bs[1].z, 0.05) * dt;
      p.pa.z += ((settle - r) * 2.5 + 0.004 * sin(p.pa.y * 3.0 + p.pa.x * 40.0 + t * 0.6 + p.ts.y)) * dt;
      // (Along the whole stretch in about six seconds, whatever the gun's length; faster as it is drawn in. The user,
      // 22 Nov 2026: "slow down the speed of the clouds from stock to muzzle by about a third": 0.13 + 0.08 before.)
      p.pa.x += 2.0 * g.half_len * (0.087 + 0.053 * part) * dt;
      if (part > 1.15) p.vl.w = 0.0;
    } else {
      p.vl.xyz += vec3(0.0, -1.5, 0.0) * dt;
      p.vl.xyz *= exp(-dt * 1.6);
      p.pa.xyz += p.vl.xyz * dt;
    }
  }
  if (p.vl.w <= 0.0 && seen) {
    if (smoke) {
      // Given off by its plume (about three in four of them out at a time).
      // Given off all along the gun from the back of its receiver to just before its muzzle, anywhere round it.
      // Fed in at the back, from two places going round the gun (the other way from the cloud's own turn, so
      // each band leans back as it winds forward).
      if (rnd(s) < dt * 2.5) {
        float arm = floor(rnd(s) * 2.0);
        float wide = 0.85 + 0.35 * rnd(s);
        // (Wider arms, so the bands close up into a deck: "make the clouding thicker, like atmosphere clouds".)
        float turn = arm * 3.1416 + 0.3 * t + (rnd(s) - 0.5) * 0.6;
        float from = bs[1].z + 0.14 * rnd(s) * (bs[1].w - bs[1].z);
        p.pa = vec4(from * g.half_len, turn, GunReach(clamp(from * 0.5 + 0.5, 0.0, 1.0), turn) + 0.006 + (wide - 0.85) * 0.022, 0.0);
        p.vl = vec4(wide, 0.0, 0.0, 9.0);
        p.ts = vec4(0.0, rnd(s) * 100.0, 0.0060 + 0.0040 * rnd(s), 0.0);
      }
    } else {
      // Sparks: off the strike's foot and where it meets the gun while it is new; a few off the storm's bolts' ends.
      float age = BoltAge(5);
      vec3 at = vec3(0.0);
      float born = 0.0;
      if (ba[1].z >= 0.0 && ba[1].z < 0.14 && gun_back.w > 0.5 && rnd(s) < 0.7) {
        at = gun_mag_top.xyz + (vec3(rnd(s), rnd(s), rnd(s)) - 0.5) * 0.02;
        born = 1.0;
      } else if (mg.w < 0.5) {
      } else if (age >= 0.0 && age < 0.1 && rnd(s) < 0.3) {
        vec3 p0, p1, p2;
        StrikePoints(g, p0, p1, p2);
        at = rnd(s) < 0.65 ? p2 : p1;
        born = 1.0;
      } else {
        int k = int(i % 5u);
        float bolt_age = BoltAge(k);
        if (k < 2 && bolt_age >= 0.0 && bolt_age < 0.05 && rnd(s) < 0.04) {
          vec3 A, B;
          BoltEnds(g, k, A, B);
          at = rnd(s) < 0.5 ? A : B;
          born = 1.0;
        }
      }
      if (born > 0.5) {
        vec3 away = normalize(at - g.mid + vec3(0.0, 0.03, -0.03));
        vec3 dir = normalize(away + (vec3(rnd(s), rnd(s), rnd(s)) - 0.5) * 1.7 + vec3(0.0, 0.4, 0.0));
        p.pa = vec4(at, 0.0);
        p.vl = vec4(dir * (0.08 + 0.32 * rnd(s)), 0.18 + 0.36 * rnd(s));
        p.ts = vec4(1.0, rnd(s) * 100.0, 0.0, 0.0);
      }
    }
  }
  g_p[i] = p;
}
)";
constexpr const char* kParticleVS = R"(
layout(set = 0, binding = 2) uniform sampler2D anchorTex;
layout(std430, set = 0, binding = 4) readonly buffer WeaponFxParticles { Particle g_p[]; };
layout(location = 0) out vec2 v_q;
layout(location = 1) out vec4 v_m;  // x its life's part, y its number, z its distance into the view, w 1 a spark
layout(location = 2) out vec4 v_l;  // x the lightning's light on it, y how much it shows (less between the eye and the gun), z its radius (m)
layout(location = 3) out vec3 v_w;  // the way to the lightning from it (the view's space)
)";
constexpr const char* kParticleVSMain = R"(
void main() {
  const vec2 corners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(-1.0, 1.0), vec2(1.0, -1.0), vec2(1.0, 1.0));
  Particle p = g_p[gl_InstanceIndex];
  vec2 q = corners[gl_VertexIndex];
  v_q = q, v_m = vec4(0.0), v_l = vec4(0.0), v_w = vec3(0.0);
  gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
  if (p.vl.w <= 0.0 || c.y <= 0.0) return;
  float f = clamp(p.pa.w / p.vl.w, 0.0, 1.0);
  bool spark = float(gl_InstanceIndex) >= e.z;
  Gun g = GunFrame();
  vec3 pos = spark ? p.pa.xyz : Around(g, p.pa.xyz);
  if (pos.z < 0.06) return;
  vec2 ndc = e.xy * pos.xy / pos.z;
  if (!spark) {
    // (Drawn out a little along its way: where it will be a moment on.)
    vec3 ahead = Around(g, p.pa.xyz + vec3(0.004, -0.12, 0.0));
    vec2 way = e.xy * ahead.xy / max(ahead.z, 0.06) - ndc;
    way = length(way) > 1e-6 ? normalize(way * a.xy) : vec2(1.0, 0.0);
    vec2 size = p.ts.z * (1.0 + 0.5 * f) * e.xy / pos.z;
    ndc += (way * q.x * 1.15 + vec2(-way.y, way.x) * q.y) * size.y * vec2(a.y / a.x, 1.0);
    v_w = LightningFrom(g, pos);
    // Lit from within by the lightning; fainter between the eye and the gun (the sights and the gun stay seen).
    vec3 on_axis = g.mid + g.axis * p.pa.x;
    // (And fading out at the stretch's two ends.)
    float part = p.pa.x / g.half_len;
    float ends = smoothstep(bs[1].z - 0.02, bs[1].z + 0.10, part) * (1.0 - smoothstep(bs[1].w - 0.10, bs[1].w + 0.10, part));
    // (And while aiming, none over the top of the gun, where the eye looks along its sights.)
    float over_top = smoothstep(0.1, 0.65, cos(p.pa.y));
    v_l = vec4(LightningAt(g, pos), mix(0.6, 1.0, smoothstep(-0.02, 0.03, pos.z - on_axis.z)) * ends * (1.0 - gun_up.w * over_top),
               p.ts.z * (1.0 + 0.3 * f), 0.0);
  } else {
    // A streak from where it was a thirtieth of a second ago, a few pixels wide.
    vec3 tail = pos - p.vl.xyz * 0.03;
    vec2 ndc1 = e.xy * tail.xy / max(tail.z, 0.06);
    vec2 px = 2.0 / a.xy;
    vec2 dir = (ndc - ndc1) / px;
    float len = length(dir);
    dir = len > 1e-3 ? dir / len : vec2(1.0, 0.0);
    ndc = mix(ndc1, ndc, q.x * 0.5 + 0.5) + (dir * q.x + vec2(-dir.y, dir.x) * q.y) * 1.9 * px;
  }
  gl_Position = vec4(ndc, 0.5, 1.0);
  v_m = vec4(f, p.ts.y, pos.z, spark ? 1.0 : 0.0);
}
)";
// A puff as a volume (the user, 22 Nov 2026: "Make the clouds more realistic volumetric, but keep the performance
// impact minimal"): not a card with a shade painted on, but a cluster of five small balls (a middle and four lobes
// round it, its own by its number). Per pixel, without another pass or a march:
// - the ball that stands highest toward the eye there gives the surface: its normal, for the light;
// - how deep the cluster is there is its thickness: how much of the picture it hides (thin at its rim, as cloud is);
// - its surface's own distance from the eye (the puff's, less the ball's height) is what is tested against the
//   picture's depth, so a puff goes into the gun and into other puffs as a round thing, not as a flat card;
// - lit from above; its underside and what is tucked against the gun darker (the light does not reach in); a bright
//   rim where the light comes through its thin edge; and the lightning's light from where it is, through it.
constexpr const char* kParticlePS = R"(
layout(set = 0, binding = 3) uniform sampler2D depthTex;
layout(location = 0) in vec2 v_q;
layout(location = 1) in vec4 v_m;
layout(location = 2) in vec4 v_l;
layout(location = 3) in vec3 v_w;
layout(location = 0) out vec4 out_color;
float h21(vec2 p) {
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}
float vnoise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(h21(i), h21(i + vec2(1.0, 0.0)), u.x), mix(h21(i + vec2(0.0, 1.0)), h21(i + vec2(1.0, 1.0)), u.x), u.y);
}
void main() {
  vec2 dat = gl_FragCoord.xy * a.zw / a.xy - 0.5;
  ivec2 d0 = ivec2(floor(dat)), dmax = ivec2(a.zw) - 1;
  vec2 df = fract(dat);
  // (Left at once where it is outside its own shape or well behind what the picture has there: most of its pixels.)
  float lobes = 1.0 - length(v_q) / 0.72;
  if (v_m.w < 0.5) {
    for (int i = 0; i < 4; ++i) {
      float la = v_m.y * 6.283 + float(i) * 1.7;
      lobes = max(lobes, 1.0 - length(v_q - 0.40 * vec2(cos(la), sin(la))) / (0.50 + 0.10 * fract(sin(v_m.y * 12.9898 + float(i) * 78.233) * 43758.5453)));
    }
    if (lobes < -0.14) discard;
  }
  float z00 = texelFetch(depthTex, clamp(d0, ivec2(0), dmax), 0).r;
  if (v_m.w < 0.5 && 0.1 / max(z00, 1e-5) < v_m.z - 0.02) discard;
  float z10 = texelFetch(depthTex, clamp(d0 + ivec2(1, 0), ivec2(0), dmax), 0).r;
  float z01 = texelFetch(depthTex, clamp(d0 + ivec2(0, 1), ivec2(0), dmax), 0).r, z11 = texelFetch(depthTex, clamp(d0 + ivec2(1, 1), ivec2(0), dmax), 0).r;
  float scene = 0.1 / max(mix(mix(z00, z10, df.x), mix(z01, z11, df.x), df.y), 1e-5);
  float f = v_m.x;
  // While aiming, nothing of the cloud or its sparks at the middle of the view: the sight picture.
  float sight = length((gl_FragCoord.xy / a.xy - 0.5) * vec2(a.x / a.y, 1.0));
  // (Only the sight picture itself: with a wider ring cleared, 0.09 to 0.20 of the view's height, nothing of the cloud
  // was left to see on the gun while aiming. The top of the gun is kept clear along its whole length besides.)
  float clear = 1.0 - gun_up.w * (1.0 - smoothstep(0.035, 0.085, sight));
  if (clear <= 0.0) discard;
  if (v_m.w > 0.5) {
    // A spark: white hot, then orange; added to the picture.
    float soft = clamp((scene - v_m.z + 0.008) / 0.008, 0.0, 1.0) * clear;
    float core = (1.0 - smoothstep(0.15, 0.9, abs(v_q.y))) * (1.0 - smoothstep(0.6, 1.0, abs(v_q.x)));
    out_color = vec4(mix(vec3(1.0, 0.55, 0.2), vec3(0.8, 0.92, 1.0), 1.0 - f) * core * (1.0 - f) * 2.6 * soft * c.y, 0.0);
    return;
  }
  // A puff of the storm: a soft ball eaten into by noise (its own, turned by its number); dark, a little lighter
  // above by day, and lit blue-white from within where the lightning is near. Over the picture.
  float r2 = dot(v_q, v_q);
  if (r2 >= 1.0) discard;
  float soft = clamp((scene - v_m.z) / 0.012, 0.0, 1.0);
  if (soft <= 0.0) discard;
  float an = v_m.y * 6.283;
  vec2 rq = vec2(cos(an) * v_q.x - sin(an) * v_q.y, sin(an) * v_q.x + cos(an) * v_q.y);
  float n = 0.6 * vnoise(rq * 2.1 + v_m.y * 13.0) + 0.4 * vnoise(rq * 4.6 + v_m.y * 29.0 + c.x * 0.3);
  float body = 1.0 - r2;
  // A small cumulus: a middle and four lobes round it (its own, by its number), their edge broken a little by the
  // noise and clear, not a haze.
  float shape = lobes + (n - 0.5) * 0.30;
  // (Thick: nearly solid in its body, soft only at its rim, as a cumulus is against the sky.)
  float al = smoothstep(0.0, 0.26, shape);
  al *= smoothstep(0.0, 0.05, f) * soft * 0.96 * c.y * v_l.y * clear;
  // Shaded as a ball (its face toward the eye, roughened by its noise): the day's light from above; the lightning's
  // from where it is, soft, so the cloud glows inside rather than shows a bolt.
  vec3 nrm = normalize(vec3(v_q + (vec2(n, vnoise(rq * 3.3 + v_m.y * 7.0)) - 0.5) * 0.9, sqrt(max(1.0 - r2, 0.0)) + 0.25));
  body = clamp(shape * 1.6, 0.0, 1.0);
  float day = clamp(dot(nrm, normalize(vec3(0.25, 0.9, 0.35))) * 0.6 + 0.45, 0.0, 1.0);
  float way_len = length(v_w);
  float flash = way_len * (clamp(dot(nrm, vec3(v_w.xy, -v_w.z) / max(way_len, 1e-5)) * 0.55 + 0.6, 0.0, 1.0) + 0.45);
  // A thundercloud's colours: a dark slate underside, blue grey in its body, pale on its lit tops.
  day = clamp(day * (0.75 + 0.5 * body), 0.0, 1.0);  // (darker toward its edge's underside, as a cloud's base)
  vec3 cloud = day < 0.45 ? mix(vec3(0.085, 0.095, 0.130), vec3(0.340, 0.365, 0.430), day / 0.45)
                          : mix(vec3(0.340, 0.365, 0.430), vec3(0.880, 0.895, 0.920), (day - 0.45) / 0.55);
  // (By night the cycle draws the scene a twelfth as bright, D177, and the night pass after this lights what it
  // finds from the lamps as that much brighter a surface: at a sixteenth the cloud came out a pale tan under the
  // base's sodium lamps, lighter than the gun, run skin12_night. A thundercloud is dark: a fiftieth by night. And
  // its lightning's light is brought down with the scene.)
  // (c.w under 0: drawn after the night pass, over the picture as it is shown: a thundercloud in the dark, a
  // quarter as bright as by day, and its lightning at its full strength.)
  float shade_by = c.w < 0.0 ? mix(1.0, 0.36, -c.w) : mix(1.0, 0.02, smoothstep(0.0, 0.6, c.w));
  vec3 lightning = vec3(0.50, 0.70, 1.0) * min(flash * 1.5, 2.6) * 0.75 + vec3(0.95, 0.97, 1.0) * smoothstep(0.9, 2.2, flash) * 0.9;
  vec3 col = cloud * shade_by + lightning * (c.w < 0.0 ? 1.0 : mix(1.0, 0.22, c.w));
  out_color = vec4(col * al, al);
}
)";
constexpr const char* kBoltVS = R"(
layout(set = 0, binding = 2) uniform sampler2D anchorTex;
layout(location = 0) out vec2 v_q;  // x along it (0 to 1), y across it (-1 to 1)
layout(location = 1) out vec4 v_m;  // x how bright, y its distance into the view, z its core's part of its width
)";
constexpr const char* kBoltVSMain = R"(
const float kPieces = 16.0;
vec3 BoltAt(Gun g, int k, float j, vec3 A, vec3 B, vec3 p0, vec3 p1, vec3 p2, float shape) {
  float seed = BoltSeed(k);
  float big = h11(seed * 13.0 + floor(j * 0.5) * 5.7 + shape * 1.9) - 0.5, small = h11(seed * 31.0 + j * 3.3 + shape * 2.7) - 0.5,
        turn = h11(seed * 7.0 + j * 9.1 + shape * 4.1) - 0.5;
  if (k == 5) {
    // The strike: jagged from the storm's top to the gun, then close down the magazine.
    if (j <= 6.0) return mix(p0, p1, j / 6.0) + (g.side * small + g.axis * big) * 0.03 * sin(3.14159 * j / 6.0);
    float fm = (j - 6.0) / (kPieces - 6.0);
    return mix(p1, p2, fm) + (g.axis * small + g.side * turn) * 0.008 * sin(3.14159 * fm);
  }
  float f = j / kPieces, bow = sin(3.14159 * f), len = distance(A, B);
  vec3 here = mix(A, B, f);
  vec3 out_of = here - (g.mid + g.axis * dot(here - g.mid, g.axis));
  out_of = normalize(out_of + vec3(1e-5));
  return here + (out_of * (0.2 * big + 0.06) + g.axis * 0.22 * small + cross(g.axis, out_of) * 0.16 * turn) * len * bow;
}
void main() {
  const vec2 corners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(-1.0, 1.0), vec2(1.0, -1.0), vec2(1.0, 1.0));
  int k = gl_InstanceIndex;
  float piece = float(gl_VertexIndex / 6);
  vec2 q = corners[gl_VertexIndex % 6];
  v_q = q, v_m = vec4(0.0);
  gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
  float age = BoltAge(k);
  float on = (k == 5 ? StrikeOn() : BoltOn(age)) * c.y;
  // (Of the storm's five, three are only their light in the smoke; none drawn unless mg.w says so.)
  if (on <= 0.0 || mg.w < 0.5 || (k >= 2 && k < 5) || texelFetch(anchorTex, ivec2(3, 0), 0).r < 2e-3) return;
  Gun g = GunFrame();
  vec3 A = vec3(0.0), B = vec3(0.0), s0 = vec3(0.0), s1 = vec3(0.0), s2 = vec3(0.0);
  if (k == 5) StrikePoints(g, s0, s1, s2);
  else BoltEnds(g, k, A, B);
  float shape = floor(age * 30.0);
  vec3 p0 = BoltAt(g, k, piece, A, B, s0, s1, s2, shape), p1 = BoltAt(g, k, piece + 1.0, A, B, s0, s1, s2, shape);
  if (p0.z < 0.06 || p1.z < 0.06) return;
  vec2 n0 = e.xy * p0.xy / p0.z, n1 = e.xy * p1.xy / p1.z;
  vec2 px = 2.0 / a.xy;
  vec2 dir = (n1 - n0) / px;
  float len = length(dir);
  dir = len > 1e-3 ? dir / len : vec2(1.0, 0.0);
  float end = q.x * 0.5 + 0.5;
  vec3 here = mix(p0, p1, end);
  // As thick as its distance makes it: a glow 3 mm to a side (5 the strike's) round a core of 0.5 mm (0.9), thinner
  // toward its ends; never under a pixel of core.
  float along = (piece + end) / kPieces;
  float taper = k == 5 ? mix(1.0, 0.7, along) : 0.55 + 0.45 * sin(3.14159 * along);
  float to_px = e.x / here.z * a.x * 0.5;
  float half_w = max((k == 5 ? 0.005 : 0.003) * taper * to_px, 3.0);
  float core = max((k == 5 ? 0.0009 : 0.0005) * taper * to_px, 1.0) / half_w;
  vec2 ndc = mix(n0, n1, end) + (dir * q.x * min(half_w * 0.25, 3.0) + vec2(-dir.y, dir.x) * q.y * half_w) * px;
  gl_Position = vec4(ndc, 0.5, 1.0);
  v_q = vec2(along, q.y);
  v_m = vec4(on * (k == 5 ? 1.0 : 0.5), here.z, core, 0.0);
}
)";
constexpr const char* kBoltPS = R"(
layout(set = 0, binding = 3) uniform sampler2D depthTex;
layout(location = 0) in vec2 v_q;
layout(location = 1) in vec4 v_m;
layout(location = 0) out vec4 out_color;
void main() {
  float scene = 0.1 / max(texelFetch(depthTex, ivec2(min(gl_FragCoord.xy * a.zw / a.xy, a.zw - 1.0)), 0).r, 1e-5);
  // (Hidden where the gun is in front of it.)
  float soft = clamp((scene - v_m.y + 0.006) / 0.006, 0.0, 1.0);
  float across = abs(v_q.y);
  float core = 1.0 - smoothstep(v_m.z * 0.6, v_m.z * 1.4, across);
  float halo = exp(-across * 5.0) * (1.0 - across);
  vec3 col = vec3(0.88, 0.96, 1.0) * core * 2.8 + vec3(0.18, 0.5, 1.0) * halo * 0.85;
  out_color = vec4(col * v_m.x * soft, 0.0);
}
)";

RefCntAutoPtr<IShader> Compile(IRenderDevice* device, SHADER_TYPE type, const char* source, const char* name) {
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

struct Copy {
  RefCntAutoPtr<ITextureView> original, copy;
  RefCntAutoPtr<ITextureView> plain;  // the copy as a plain 2D texture (the mesh pass's)
  uint64_t used = 0;     // the frame it was last drawn with
  double made = -1.0;    // the clock when the copy was last made
  int skin = 0;          // the skin it was made with
};

struct State {
  std::vector<Copy> copies;
  RefCntAutoPtr<IPipelineState> pso;
  RefCntAutoPtr<IShaderResourceBinding> srb;
  RefCntAutoPtr<IBuffer> constants;
  bool failed = false;
  uint64_t made_frame = ~0ull;
};
State& S() {
  static State s;
  return s;
}

float ChargeAge(double now);
// While aiming (the user, 22 Nov 2026: "Let's also make sure that if players have the SMG iron sight toggled on, the
// sights stay unobstructed when zooming in"; then, of the cloud leaving and the storm's light dimmed while aiming:
// "Keep the effects when zooming in, let's just make sure the clouds never obstruct the weapon's ironsights"). So
// everything stays; only the cloud keeps out of the sight line: the puffs over the top of the gun (where the eye
// looks along it) and whatever of the cloud would be at the middle of the view (the sight picture) are let go as the
// aim comes in (AimNow, the overlay's gun_up.w).
float AimDim() { return 1.0f; }
float AimNow() {
  const float aim = std::clamp((AdsBlendNow() - 0.05f) / 0.45f, 0.0f, 1.0f);
  return aim * aim * (3.0f - 2.0f * aim);
}
double Clock() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

bool Ready(IRenderDevice* device) {
  State& s = S();
  if (s.srb) return true;
  if (s.failed) return false;
  s.failed = true;
  RefCntAutoPtr<IShader> vs = Compile(device, SHADER_TYPE_VERTEX, kVS, "NativeXRenderer weapon skin VS");
  RefCntAutoPtr<IShader> ps = Compile(device, SHADER_TYPE_PIXEL, (std::string(kPSHead) + kStorm + kPS).c_str(),
                                      "NativeXRenderer weapon skin PS");
  if (!vs || !ps) {
    REXLOG_ERROR("[native_x_renderer] weapon skin: its shaders did not compile; the skin is not drawn");
    return false;
  }
  BufferDesc b;
  b.Name = "NativeXRenderer weapon skin constants";
  b.Size = 32;
  b.Usage = USAGE_DYNAMIC;
  b.BindFlags = BIND_UNIFORM_BUFFER;
  b.CPUAccessFlags = CPU_ACCESS_WRITE;
  device->CreateBuffer(b, nullptr, &s.constants);
  SamplerDesc linear;
  linear.MinFilter = linear.MagFilter = linear.MipFilter = FILTER_TYPE_LINEAR;
  linear.AddressU = linear.AddressV = linear.AddressW = TEXTURE_ADDRESS_CLAMP;
  ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "srcTex", linear}};
  GraphicsPipelineStateCreateInfo info;
  info.PSODesc.Name = "NativeXRenderer weapon skin";
  info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
  info.GraphicsPipeline.NumRenderTargets = 1;
  info.GraphicsPipeline.RTVFormats[0] = TEX_FORMAT_RGBA16_FLOAT;
  info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
  info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
  info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
  info.pVS = vs;
  info.pPS = ps;
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
  info.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
  device->CreateGraphicsPipelineState(info, &s.pso);
  if (s.pso) s.pso->CreateShaderResourceBinding(&s.srb, true);
  if (!s.srb || !s.constants) {
    REXLOG_ERROR("[native_x_renderer] weapon skin: its pipeline could not be made; the skin is not drawn");
    s.srb.Release();
    return false;
  }
  s.failed = false;
  return true;
}

// The bolts (the overlay's, and the skin's surge), from the clock alone: each bolt's time runs in slots of its own
// length, in three of four of them a bolt at a moment of its own. `age`: the seconds since its last began (-1: none in
// this slot or the last), `seed`: that bolt's own number. Eight of each (six used).
void Strikes(double now, float age[8], float seed[8]) {
  auto hash = [](uint32_t x) {
    x ^= x >> 16, x *= 0x7FEB352Du, x ^= x >> 15, x *= 0x846CA68Bu, x ^= x >> 16;
    return float(x & 0xFFFFFF) / float(0x1000000);
  };
  // (Five in the storm; the sixth the strike down the magazine, about every five seconds.)
  static constexpr double kSlot[6] = {6.5, 8.3, 3.7, 4.9, 5.6, 6.0};
  for (int i = 0; i < 8; ++i) age[i] = -1.0f, seed[i] = 0.0f;
  // NXR_SKIN_STRIKE_AGE=<seconds> (a developer's, for screenshots): every bolt held at that age, new ones each second.
  static const float held = [] {
    const char* v = std::getenv("NXR_SKIN_STRIKE_AGE");
    return v && *v ? float(std::atof(v)) : -1.0f;
  }();
  if (held >= 0.0f) {
    // (Begun anew for one frame each second, so their ends are taken; then held.)
    const double in_second = now - std::floor(now);
    for (int i = 0; i < 6; ++i)
      age[i] = in_second < 0.02 ? 0.0f : held, seed[i] = float(std::fmod(std::floor(now), 97.0)) + 10.0f * float(i);
    return;
  }
  for (int i = 0; i < 6; ++i) {
    const int64_t slot = int64_t(std::floor(now / kSlot[i] + 0.37 * i));
    for (int64_t s = slot - 1; s <= slot; ++s) {
      const uint32_t key = uint32_t(s) * 2654435761u + uint32_t(i) * 40503u;
      if (hash(key) >= (i == 5 ? 0.9f : 0.75f)) continue;
      const double start = (double(s) - 0.37 * i + 0.1 + 0.6 * hash(key + 1)) * kSlot[i];
      if (now >= start) age[i] = float(now - start), seed[i] = hash(key + 2) * 100.0f;
    }
  }
}
// How stirred up the storm is (0 to 1): by the player's shots (WeaponSkinShot: each adds, easing off over a few
// seconds) and by the strike down the magazine.
std::atomic<float> g_heat{0.0f};
std::atomic<int64_t> g_heat_at{0};
float ShotHeat(double now_ticks_seconds) {
  const double since = now_ticks_seconds - double(g_heat_at.load(std::memory_order_relaxed)) * 1e-6;
  return g_heat.load(std::memory_order_relaxed) * float(std::exp(-std::max(since, 0.0) / 2.5));
}
double Micros() {
  return double(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
float StormHeat(double now) {
  float age[8], seed[8];
  Strikes(now, age, seed);
  const float strike = age[5] >= 0.0f ? std::exp(-age[5] * 2.0f) : 0.0f;
  const float charge_age = ChargeAge(now);
  const float charge = charge_age >= 0.0f ? std::exp(-charge_age * 1.5f) : 0.0f;
  return std::clamp(ShotHeat(Micros() * 1e-6) + 0.7f * strike + charge, 0.0f, 1.0f);
}

bool MakeOne(IRenderDevice* device, IDeviceContext* context, Copy& c, double now, float bright, float most) {
  State& s = S();
  if (!c.copy) {
    TextureDesc d = c.original->GetTexture()->GetDesc();
    d.Name = "NativeXRenderer weapon skin copy";
    d.Format = TEX_FORMAT_RGBA16_FLOAT;
    d.MipLevels = 0;  // all of them
    d.Usage = USAGE_DEFAULT;
    d.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
    d.MiscFlags = MISC_TEXTURE_FLAG_GENERATE_MIPS;
    d.CPUAccessFlags = CPU_ACCESS_NONE;
    RefCntAutoPtr<ITexture> texture;
    device->CreateTexture(d, nullptr, &texture);
    if (!texture) return false;
    c.copy = texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    TextureViewDesc as_2d;
    as_2d.ViewType = TEXTURE_VIEW_SHADER_RESOURCE;
    as_2d.TextureDim = RESOURCE_DIM_TEX_2D;
    texture->CreateView(as_2d, &c.plain);
    REXLOG_INFO("[native_x_renderer] weapon skin: a skinned copy of the weapon's {}x{} colour texture", d.Width, d.Height);
  }
  ITexture* to = c.copy->GetTexture();
  const TextureDesc& desc = to->GetDesc();
  // Read as a plain 2D texture (the game's are arrays of one layer); written as the copy is.
  TextureViewDesc plain;
  plain.ViewType = TEXTURE_VIEW_SHADER_RESOURCE;
  plain.TextureDim = RESOURCE_DIM_TEX_2D;
  RefCntAutoPtr<ITextureView> from;
  c.original->GetTexture()->CreateView(plain, &from);
  TextureViewDesc first;
  first.ViewType = TEXTURE_VIEW_RENDER_TARGET;
  first.TextureDim = desc.Type;
  first.MostDetailedMip = 0, first.NumMipLevels = 1;
  first.FirstArraySlice = 0, first.NumArraySlices = 1;
  RefCntAutoPtr<ITextureView> target;
  to->CreateView(first, &target);
  if (!from || !target) return false;
  {
    const float k[8] = {float(std::fmod(now, 3600.0)), float(desc.Width), float(desc.Height), bright * AimDim(), most,
                        StormHeat(now), 0, 0};
    void* data = nullptr;
    context->MapBuffer(s.constants, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, k, sizeof(k));
    context->UnmapBuffer(s.constants, MAP_WRITE);
  }
  if (auto* v = s.srb->GetVariableByName(SHADER_TYPE_PIXEL, "SkinConstants")) v->Set(s.constants);
  if (auto* v = s.srb->GetVariableByName(SHADER_TYPE_PIXEL, "srcTex")) v->Set(from);
  ITextureView* rtv = target;
  context->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  Viewport viewport;
  viewport.Width = float(desc.Width);
  viewport.Height = float(desc.Height);
  viewport.MaxDepth = 1.0f;
  context->SetViewports(1, &viewport, 0, 0);
  context->SetPipelineState(s.pso);
  context->CommitShaderResources(s.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DrawAttribs attribs;
  attribs.NumVertices = 3;
  context->Draw(attribs);
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  context->GenerateMips(c.copy);
  c.made = now;
  return true;
}

}  // namespace

namespace {
// The skinned guns as their models are (the weapon export's weapon.obj and info.txt, runs\export_out3): the bone the
// gun's body is on, and in that bone's space (metres) the back of the receiver and the muzzle on the barrel's line,
// and the magazine's top and end.
struct GunModel {
  int body_bone, mag_bone;
  float back[3], muzzle[3], mag_top[3], mag_end[3];
  float mag_rest[3];  // the magazine's bone's place against the body's when it is in the gun (bones.txt)
};
constexpr GunModel kGuns[2] = {
    {0, 4, {0.0f, 1.226f, 0.146f}, {0.0f, 1.229f, 0.866f}, {0.0f, 1.200f, 0.445f}, {0.0f, 1.025f, 0.445f},
     {0.00003f, 0.03700f, 0.54099f}},  // the Thompson
    {3, 2, {0.005f, 1.212f, 0.194f}, {0.0f, 1.212f, 0.829f}, {0.026f, 1.201f, 0.597f}, {0.298f, 1.201f, 0.597f},
     {0.08267f, 0.10403f, 0.63573f}},  // the Type 100
};
// A new magazine went in: the clock (Clock()) when, -1 never. Told by the magazine's bone: off its rest by 4 cm or
// more (taken out), then back within 8 mm of it.
std::atomic<double> g_charge_at{-1.0};
float ChargeAge(double now) {
  const double at = g_charge_at.load(std::memory_order_relaxed);
  return at >= 0.0 && now - at < 2.0 ? float(now - at) : -1.0f;
}
// A place in the body bone's space is `at + x * col[0] + y * col[1] + z * col[2]` in the weapon's view space.
struct GunFrameNow {
  std::mutex mutex;
  float at[3] = {}, col[3][3] = {};
  float well_world[3] = {};  // the magazine's well in the world (the mesh pass's charge runs out from it)
  int64_t when = 0;  // steady clock ticks
  bool mag_out = false;
  int gun = -1;
};
GunFrameNow& Frame() {
  static GunFrameNow f;
  return f;
}
}  // namespace

int WeaponSkinBodyBone() { return kGuns[std::clamp(g_magazine.load(std::memory_order_relaxed), 0, 1)].body_bone; }
int WeaponSkinMagazineBone() { return kGuns[std::clamp(g_magazine.load(std::memory_order_relaxed), 0, 1)].mag_bone; }

void WeaponSkinGunFrame(const float bone[12], const float magazine[12], const float view_projection[16], float px, float py,
                        const double* view_move) {
  if (px < 0.01f || py < 0.01f) return;
  {
    // The magazine's bone against the body's (the body's rows are a turn and a place: its inverse is its transpose).
    const int which = std::clamp(g_magazine.load(std::memory_order_relaxed), 0, 1);
    const GunModel& gun = kGuns[which];
    const float d[3] = {magazine[3] - bone[3], magazine[7] - bone[7], magazine[11] - bone[11]};
    float off = 0.0f;
    for (int c = 0; c < 3; ++c) {
      const float rel = bone[c] * d[0] + bone[4 + c] * d[1] + bone[8 + c] * d[2];
      off += (rel - gun.mag_rest[c]) * (rel - gun.mag_rest[c]);
    }
    off = std::sqrt(off);
    GunFrameNow& f = Frame();
    std::lock_guard<std::mutex> lock(f.mutex);
    if (f.gun != which) f.gun = which, f.mag_out = false;  // (another gun: nothing carried over)
    if (off > 0.04f && off < 2.0f) {
      if (!f.mag_out) REXLOG_INFO("[native_x_renderer] weapon skin: the magazine is out ({:.3f} m off its place)", off);
      f.mag_out = true;
    } else if (f.mag_out && off < 0.008f) {
      f.mag_out = false;
      g_charge_at.store(Clock(), std::memory_order_relaxed);
      REXLOG_INFO("[native_x_renderer] weapon skin: a new magazine is in: its charge runs over the gun");
    }
    for (int c = 0; c < 3; ++c)
      f.well_world[c] = bone[c * 4] * gun.mag_top[0] + bone[c * 4 + 1] * gun.mag_top[1] + bone[c * 4 + 2] * gun.mag_top[2] + bone[c * 4 + 3];
  }
  auto view_of = [&](float x, float y, float z, float out[3]) {
    const float world[3] = {bone[0] * x + bone[1] * y + bone[2] * z + bone[3], bone[4] * x + bone[5] * y + bone[6] * z + bone[7],
                            bone[8] * x + bone[9] * y + bone[10] * z + bone[11]};
    float clip[4];
    for (int i = 0; i < 4; ++i)
      clip[i] = view_projection[i * 4] * world[0] + view_projection[i * 4 + 1] * world[1] + view_projection[i * 4 + 2] * world[2] +
                view_projection[i * 4 + 3];
    out[0] = clip[0] / px, out[1] = clip[1] / py, out[2] = clip[3];
    // (The weapon's picture moved in the view, the Iron Sight option's SetWeaponLayerMove: the gun is drawn moved, so
    // its cloud is too. The user, 22 Nov 2026: "The smoke shouldn't be off the side of the gun when aiming down
    // iron sights": it stayed where the gun would have been.)
    if (view_move) {
      const double v[4] = {out[0], out[1], out[2], 1.0};
      for (int i = 0; i < 3; ++i)
        out[i] = float(view_move[i * 4] * v[0] + view_move[i * 4 + 1] * v[1] + view_move[i * 4 + 2] * v[2] + view_move[i * 4 + 3] * v[3]);
    }
  };
  GunFrameNow& f = Frame();
  std::lock_guard<std::mutex> lock(f.mutex);
  float ex[3], ey[3], ez[3];
  view_of(0, 0, 0, f.at), view_of(1, 0, 0, ex), view_of(0, 1, 0, ey), view_of(0, 0, 1, ez);
  for (int c = 0; c < 3; ++c) f.col[0][c] = ex[c] - f.at[c], f.col[1][c] = ey[c] - f.at[c], f.col[2][c] = ez[c] - f.at[c];
  f.when = std::chrono::steady_clock::now().time_since_epoch().count();
}

namespace {
// Whether the gun was drawn (its frame told) within the last tenth of a second: in a vehicle it is not.
bool GunDrawnLately() {
  GunFrameNow& f = Frame();
  std::lock_guard<std::mutex> lock(f.mutex);
  const int64_t tenth = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(100)).count();
  return f.when && std::chrono::steady_clock::now().time_since_epoch().count() - f.when <= tenth;
}
// The gun's five vectors for the overlay (20 floats), if its frame was told within the last tenth of a second.
bool GunPoints(float out[20]) {
  GunFrameNow& f = Frame();
  std::lock_guard<std::mutex> lock(f.mutex);
  const int64_t tenth = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(100)).count();
  if (!f.when || std::chrono::steady_clock::now().time_since_epoch().count() - f.when > tenth) return false;
  const GunModel& gun = kGuns[std::clamp(g_magazine.load(std::memory_order_relaxed), 0, 1)];
  auto place = [&](const float q[3], float* to) {
    for (int c = 0; c < 3; ++c) to[c] = f.at[c] + q[0] * f.col[0][c] + q[1] * f.col[1][c] + q[2] * f.col[2][c];
  };
  place(gun.back, out), place(gun.muzzle, out + 4), place(gun.mag_top, out + 12), place(gun.mag_end, out + 16);
  out[3] = 1.0f;
  for (int c = 0; c < 3; ++c) out[8 + c] = f.col[1][c];  // the model's up
  // Only the muzzle must be in front of the eye. Aiming down the sights (the Iron Sight option most of all) brings the
  // back of the receiver to the eye or behind it; turned down then, the overlay got no gun and every puff fell behind the
  // eye: the whole storm went while aiming (the user, 28 Nov 2026: "the new skin is disappearing when a player ADSs
  // with iron sights ... it should only keep the iron sight view clear with the rest of the animation showing around
  // the weapon"; run skinaim_us2). The gun's line is the same with its back there.
  if (out[6] < 0.03f) return false;  // (the muzzle behind the eye: not a frame to go by)
  return true;
}
}  // namespace

void WeaponSkinShot() {
  const double now = Micros();
  const float heat = std::min(ShotHeat(now * 1e-6) + 0.06f, 1.0f);
  g_heat.store(heat, std::memory_order_relaxed);
  g_heat_at.store(int64_t(now), std::memory_order_relaxed);
}

RefCntAutoPtr<IShader> CreateWeaponSkinOverShader(IRenderDevice* device) {
  return Compile(device, SHADER_TYPE_PIXEL, (std::string(kOverPSHead) + kStorm + kOverPS).c_str(), "NativeXRenderer weapon skin mesh PS");
}

ITextureView* WeaponSkinPlainView(IDeviceObject* copy) {
  for (const Copy& c : S().copies)
    if (c.copy && static_cast<IDeviceObject*>(c.copy.RawPtr()) == copy) return c.plain;
  return nullptr;
}

void WeaponSkinOverConstants(float out[12], float width, float height) {
  const double now = Clock();
  // NXR_SKIN_LIGHT=<brightness> (1; a developer's check).
  static const float light = [] {
    const char* v = std::getenv("NXR_SKIN_LIGHT");
    return v && *v ? float(std::atof(v)) : 1.0f;
  }();
  out[0] = float(std::fmod(now, 3600.0)), out[1] = StormHeat(now), out[2] = width, out[3] = height;
  out[4] = light * AimDim(), out[5] = out[6] = out[7] = 0.0f;
  GunFrameNow& f = Frame();
  std::lock_guard<std::mutex> lock(f.mutex);
  out[8] = f.well_world[0], out[9] = f.well_world[1], out[10] = f.well_world[2], out[11] = ChargeAge(now);
}

void SetWeaponSkinMagazine(int where) { g_magazine.store(where, std::memory_order_relaxed); }

void SetWeaponSkin(int skin) {
  g_skin.store(skin, std::memory_order_relaxed);
  g_skin_at.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
}

int WeaponSkinNow() {
  const int skin = g_skin.load(std::memory_order_relaxed);
  if (!skin) return 0;
  const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
  const int64_t quarter = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(250)).count();
  return now - g_skin_at.load(std::memory_order_relaxed) <= quarter ? skin : 0;
}

IDeviceObject* WeaponSkinView(IDeviceObject* view, bool hands, uint64_t frame) {
  if (!view || hands) return view;
  const int skin = WeaponSkinNow();
  if (!skin) return view;
  State& s = S();
  if (s.failed) return view;
  auto* original = static_cast<ITextureView*>(view);
  const TextureDesc& d = original->GetTexture()->GetDesc();
  if ((d.Type != RESOURCE_DIM_TEX_2D && !(d.Type == RESOURCE_DIM_TEX_2D_ARRAY && d.ArraySize == 1)) || d.Width < 256 ||
      d.Height < 256 || (d.Format != TEX_FORMAT_BC1_UNORM && d.Format != TEX_FORMAT_BC3_UNORM)) {
    return view;
  }
  for (Copy& c : s.copies) {
    if (c.original != original) continue;
    c.used = frame;
    return c.copy && c.skin == skin ? static_cast<IDeviceObject*>(c.copy.RawPtr()) : view;
  }
  if (s.copies.size() >= 8) return view;  // (a gun's few textures; a kit change lets the last ones go)
  Copy c;
  c.original = original;
  c.used = frame;
  s.copies.push_back(std::move(c));
  return view;
}

bool IsWeaponSkinCopy(IDeviceObject* view) {
  if (!view) return false;
  for (const Copy& c : S().copies)
    if (c.copy && static_cast<IDeviceObject*>(c.copy.RawPtr()) == view) return true;
  return false;
}

bool WeaponSkinMake(IRenderDevice* device, IDeviceContext* context, uint64_t frame, float wet) {
  State& s = S();
  if (s.copies.empty() || s.made_frame == frame) return false;
  s.made_frame = frame;
  std::erase_if(s.copies, [frame](const Copy& c) { return frame > c.used + 300; });
  const int skin = WeaponSkinNow();
  if (!skin || !device || !context || !Ready(device)) return false;
  const double now = Clock();
  // The arcs' brightness: twice white, the same by day and by night. More overflows what the game draws into: at
  // night from 4 the arcs' brightest places turned orange and red, from 8 in rainbow fringes (run skinN1, the
  // developer's check below stepping it 1 to 32); at 36, the first try at matching the night's darkness, the Type 100
  // was red all over (skinB2).
  float bright = 0.35f;  // (the lightning in the colour itself: a little; its light proper is the mesh pass's)
  // NXR_SKIN_BRIGHT=<a>,<b>,...: a developer's check of the arcs' brightness, each value in turn for 8 s.
  static const std::vector<float> steps = [] {
    std::vector<float> v;
    if (const char* e = std::getenv("NXR_SKIN_BRIGHT"))
      for (const char* c = e; *c;) {
        v.push_back(float(std::atof(c)));
        while (*c && *c != ',') ++c;
        if (*c) ++c;
      }
    return v;
  }();
  if (!steps.empty()) {
    static const double from = now;
    static int said = -1;
    const int i = int((now - from) / 8.0) % int(steps.size());
    bright = steps[size_t(i)];
    if (i != said) {
      said = i;
      REXLOG_INFO("[native_x_renderer] weapon skin: the arcs' brightness {:.1f} (NXR_SKIN_BRIGHT)", bright);
    }
  }
  // The most any channel may be: 2.5 dry, white (1) as soon as the gun is wet. Wet, the game's shader gives the gun
  // more shine, and the arcs' brightest places overflowed its colour target at night in orange and red outlines (run
  // skinH_nightUS capped at 2.5; skinI_nightUS at 2.5 to 1.3 by the wetness, red while it was still a little wet);
  // dry, peaks of 4.4 were clean (run skinN1). The bloom keeps their glow.
  const float w = std::clamp(wet / 0.08f, 0.0f, 1.0f);
  const float most = 2.5f - 1.5f * w * w * (3.0f - 2.0f * w);
  bool drew = false;
  for (Copy& c : s.copies) {
    if (frame > c.used + 2) continue;  // not drawn with now: made anew when it is
    // (60 times a second: the user, 21 Nov 2026, "We need to make everything 60 Hz in this game"; 30 before.)
    if (c.copy && c.skin == skin && now - c.made < 1.0 / 60.0 - 0.001) continue;
    c.skin = skin;
    drew |= MakeOne(device, context, c, now, bright, most);
  }
  return drew;
}


namespace {
constexpr uint32_t kSmoke = 512, kSparks = 96, kParticles = kSmoke + kSparks, kBolts = 6, kBoltPieces = 16;

struct Bloom {
  RefCntAutoPtr<IShader> vs, area_vs;
  RefCntAutoPtr<IPipelineState> extract, anchor_pso, apply, particles_pso, bolts_pso, fx_pso;
  RefCntAutoPtr<IShaderResourceBinding> extract_srb, anchor_srb, apply_srb, particles_srb, bolts_srb, fx_srb;
  RefCntAutoPtr<IBuffer> constants, particles;
  RefCntAutoPtr<ITexture> quarter, anchor, over;  // over: the mesh pass's target, the depth's size
  TEXTURE_FORMAT apply_format = TEX_FORMAT_UNKNOWN;
  double last = -1.0;
  uint32_t frame = 0;
  bool failed = false, fx = false;
};
Bloom& B() {
  static Bloom b;
  return b;
}
enum class Blend { kNone, kOver, kMultiply, kEase };
struct Sampled {
  SHADER_TYPE stage;
  const char* name;
  bool linear;
};
struct Stored {
  SHADER_TYPE stage;
  const char* name;
};
RefCntAutoPtr<IPipelineState> BloomPipeline(IRenderDevice* device, const char* name, IShader* vs, IShader* ps,
                                            TEXTURE_FORMAT format, Blend blend, std::initializer_list<Sampled> textures,
                                            std::initializer_list<Stored> buffers = {}) {
  GraphicsPipelineStateCreateInfo info;
  info.PSODesc.Name = name;
  info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
  info.GraphicsPipeline.NumRenderTargets = 1;
  info.GraphicsPipeline.RTVFormats[0] = format;
  info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
  info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
  info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
  if (blend != Blend::kNone) {  // the target's alpha kept in each
    RenderTargetBlendDesc& rt = info.GraphicsPipeline.BlendDesc.RenderTargets[0];
    rt.BlendEnable = True;
    // kOver: the shader's colour added, the picture kept by one less its alpha (smoke over it, lights added).
    // kMultiply: the picture times one plus the shader's colour. kEase: toward the shader's colour by its alpha.
    rt.SrcBlend = blend == Blend::kOver ? BLEND_FACTOR_ONE : blend == Blend::kMultiply ? BLEND_FACTOR_DEST_COLOR : BLEND_FACTOR_SRC_ALPHA;
    rt.DestBlend = blend == Blend::kMultiply ? BLEND_FACTOR_ONE : BLEND_FACTOR_INV_SRC_ALPHA;
    rt.BlendOp = BLEND_OPERATION_ADD;
    rt.SrcBlendAlpha = BLEND_FACTOR_ZERO;
    rt.DestBlendAlpha = BLEND_FACTOR_ONE;
    rt.BlendOpAlpha = BLEND_OPERATION_ADD;
    rt.RenderTargetWriteMask = COLOR_MASK_RGB;
  }
  info.pVS = vs;
  info.pPS = ps;
  SamplerDesc point, linear;
  point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
  point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
  linear.MinFilter = linear.MagFilter = linear.MipFilter = FILTER_TYPE_LINEAR;
  linear.AddressU = linear.AddressV = linear.AddressW = TEXTURE_ADDRESS_CLAMP;
  std::vector<ImmutableSamplerDesc> samplers;
  for (const Sampled& t : textures) samplers.push_back({t.stage, t.name, t.linear ? linear : point});
  // (The storage buffers as plain ones, not Vulkan's dynamic ones: fluid.cpp's note on the AMD driver's limit.)
  std::vector<ShaderResourceVariableDesc> variables;
  for (const Stored& s : buffers)
    variables.push_back({s.stage, s.name, SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE, SHADER_VARIABLE_FLAG_NO_DYNAMIC_BUFFERS});
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
  info.PSODesc.ResourceLayout.Variables = variables.empty() ? nullptr : variables.data();
  info.PSODesc.ResourceLayout.NumVariables = uint32_t(variables.size());
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers.data();
  info.PSODesc.ResourceLayout.NumImmutableSamplers = uint32_t(samplers.size());
  RefCntAutoPtr<IPipelineState> pso;
  device->CreateGraphicsPipelineState(info, &pso);
  return pso;
}
float EnvFloat(const char* name, float otherwise) {
  const char* v = std::getenv(name);
  return v && *v ? float(std::atof(v)) : otherwise;
}
std::string Source(std::initializer_list<const char*> parts) {
  std::string s;
  for (const char* p : parts) s += p;
  return s;
}
const std::string& FxSource() {
  static const std::string s = Source({kBloomCommon, kFxCS, kVortex, kFxCSMain});
  return s;
}
const std::string& BoltVsSource() {
  static const std::string s = Source({kBloomCommon, kBoltVS, kVortex, kBoltVSMain});
  return s;
}
const std::string& ParticleVsSource() {
  static const std::string s = Source({kBloomCommon, kParticleVS, kVortex, kParticleVSMain});
  return s;
}
const std::string& ApplyPsSource() {
  static const std::string s = Source({kBloomCommon, kBloomApplyPS, kVortex, kBloomApplyPSMain});
  return s;
}

// The particles and the bolts: their buffers, the pass that moves them and the two that draw them.
bool MakeFx(IRenderDevice* device, TEXTURE_FORMAT format) {
  Bloom& b = B();
  auto buffer = [&](const char* name, uint32_t count, uint32_t stride, RefCntAutoPtr<IBuffer>& out) {
    if (out) return;
    BufferDesc d;
    d.Name = name;
    d.Size = uint64_t(count) * stride;
    d.Usage = USAGE_DEFAULT;
    d.BindFlags = BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE;
    d.Mode = BUFFER_MODE_STRUCTURED;
    d.ElementByteStride = stride;
    const std::vector<float> none(size_t(count) * stride / 4, 0.0f);
    BufferData data(none.data(), d.Size);
    device->CreateBuffer(d, &data, &out);
  };
  buffer("NativeXRenderer weapon skin particles", kParticles, 48, b.particles);
  if (!b.particles) return false;
  if (!b.fx_pso) {
    const std::vector<uint32_t> code = CompileGlsl(GlslStage::kCompute, FxSource(), "NativeXRenderer weapon skin particles CS");
    if (code.empty()) return false;
    RefCntAutoPtr<IShader> cs;
    ShaderCreateInfo sci;
    sci.Desc.Name = "NativeXRenderer weapon skin particles CS";
    sci.Desc.ShaderType = SHADER_TYPE_COMPUTE;
    sci.Desc.UseCombinedTextureSamplers = true;
    sci.ByteCode = code.data();
    sci.ByteCodeSize = code.size() * sizeof(uint32_t);
    device->CreateShader(sci, &cs);
    if (!cs) return false;
    SamplerDesc point, linear;
    point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
    point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
    linear.MinFilter = linear.MagFilter = linear.MipFilter = FILTER_TYPE_LINEAR;
    linear.AddressU = linear.AddressV = linear.AddressW = TEXTURE_ADDRESS_CLAMP;
    (void)linear;
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_COMPUTE, "anchorTex", point}};
    ShaderResourceVariableDesc variables[] = {
        {SHADER_TYPE_COMPUTE, "WeaponFxParticles", SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE, SHADER_VARIABLE_FLAG_NO_DYNAMIC_BUFFERS}};
    ComputePipelineStateCreateInfo ci;
    ci.PSODesc.Name = "NativeXRenderer weapon skin particles";
    ci.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
    ci.pCS = cs;
    ci.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
    ci.PSODesc.ResourceLayout.Variables = variables;
    ci.PSODesc.ResourceLayout.NumVariables = uint32_t(std::size(variables));
    ci.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    ci.PSODesc.ResourceLayout.NumImmutableSamplers = uint32_t(std::size(samplers));
    device->CreateComputePipelineState(ci, &b.fx_pso);
    if (b.fx_pso) b.fx_pso->CreateShaderResourceBinding(&b.fx_srb, true);
    if (!b.fx_srb) return false;
    if (auto* v = b.fx_srb->GetVariableByName(SHADER_TYPE_COMPUTE, "WeaponFxParticles"))
      v->Set(b.particles->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
  }
  b.particles_pso.Release(), b.particles_srb.Release(), b.bolts_pso.Release(), b.bolts_srb.Release();
  RefCntAutoPtr<IShader> pvs = Compile(device, SHADER_TYPE_VERTEX, ParticleVsSource().c_str(),
                                       "NativeXRenderer weapon skin particles VS");
  RefCntAutoPtr<IShader> pps = Compile(device, SHADER_TYPE_PIXEL, Source({kBloomCommon, kParticlePS}).c_str(),
                                       "NativeXRenderer weapon skin particles PS");
  RefCntAutoPtr<IShader> bvs = Compile(device, SHADER_TYPE_VERTEX, BoltVsSource().c_str(), "NativeXRenderer weapon skin bolts VS");
  RefCntAutoPtr<IShader> bps = Compile(device, SHADER_TYPE_PIXEL, Source({kBloomCommon, kBoltPS}).c_str(),
                                       "NativeXRenderer weapon skin bolts PS");
  if (!pvs || !pps || !bvs || !bps) return false;
  b.particles_pso = BloomPipeline(device, "NativeXRenderer weapon skin particles", pvs, pps, format, Blend::kOver,
                                  {{SHADER_TYPE_PIXEL, "depthTex", false}, {SHADER_TYPE_VERTEX, "anchorTex", false}},
                                  {{SHADER_TYPE_VERTEX, "WeaponFxParticles"}});
  b.bolts_pso = BloomPipeline(device, "NativeXRenderer weapon skin bolts", bvs, bps, format, Blend::kOver,
                              {{SHADER_TYPE_PIXEL, "depthTex", false}, {SHADER_TYPE_VERTEX, "anchorTex", false}});
  if (b.particles_pso) b.particles_pso->CreateShaderResourceBinding(&b.particles_srb, true);
  if (b.bolts_pso) b.bolts_pso->CreateShaderResourceBinding(&b.bolts_srb, true);
  if (!b.particles_srb || !b.bolts_srb) return false;
  if (auto* v = b.particles_srb->GetVariableByName(SHADER_TYPE_VERTEX, "WeaponFxParticles"))
    v->Set(b.particles->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
  return true;
}
}  // namespace

int TestWeaponSkinShaders() {
  int failed = int(CompileGlsl(GlslStage::kVertex, kVS, "weapon skin VS").empty()) +
               int(CompileGlsl(GlslStage::kPixel, std::string(kPSHead) + kStorm + kPS, "weapon skin PS").empty()) +
               int(CompileGlsl(GlslStage::kPixel, std::string(kOverPSHead) + kStorm + kOverPS, "weapon skin mesh PS").empty());
  for (const char* body : {kBloomExtractPS, kAnchorPS, kParticlePS, kBoltPS})
    failed += int(CompileGlsl(GlslStage::kPixel, std::string(kBloomCommon) + body, "weapon skin overlay PS").empty());
  failed += int(CompileGlsl(GlslStage::kPixel, ApplyPsSource(), "weapon skin bloom PS").empty());
  failed += int(CompileGlsl(GlslStage::kVertex, std::string(kBloomCommon) + kAreaVS, "weapon skin area VS").empty());
  failed += int(CompileGlsl(GlslStage::kVertex, ParticleVsSource(), "weapon skin particles VS").empty());
  failed += int(CompileGlsl(GlslStage::kVertex, BoltVsSource(), "weapon skin bolts VS").empty());
  failed += int(CompileGlsl(GlslStage::kCompute, FxSource(), "weapon skin particles CS").empty());
  return failed;
}

ITextureView* WeaponSkinOverTarget(IRenderDevice* device, uint32_t width, uint32_t height) {
  Bloom& b = B();
  if (!device || !width || !height) return nullptr;
  if (!b.over || b.over->GetDesc().Width != width || b.over->GetDesc().Height != height) {
    b.over.Release();
    TextureDesc d;
    d.Name = "NativeXRenderer weapon skin mesh pass";
    d.Type = RESOURCE_DIM_TEX_2D;
    d.Width = width, d.Height = height;
    d.MipLevels = 1;
    d.Format = TEX_FORMAT_RGBA16_FLOAT;
    d.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
    device->CreateTexture(d, nullptr, &b.over);
    if (!b.over) return nullptr;
  }
  return b.over->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
}

bool WeaponSkinBloom(IRenderDevice* device, IDeviceContext* context, const WeaponSkinBloomInputs& in) {
  Bloom& b = B();
  if (b.failed || !device || !context || !in.color || !in.depth_srv || !b.over || in.width < 64 || in.height < 64) return false;
  if (!b.extract) {
    b.failed = true;
    b.vs = Compile(device, SHADER_TYPE_VERTEX, kVS, "NativeXRenderer weapon skin bloom VS");
    b.area_vs = Compile(device, SHADER_TYPE_VERTEX, (std::string(kBloomCommon) + kAreaVS).c_str(),
                        "NativeXRenderer weapon skin area VS");
    auto ps = [&](const char* body, const char* name) {
      return Compile(device, SHADER_TYPE_PIXEL, (std::string(kBloomCommon) + body).c_str(), name);
    };
    RefCntAutoPtr<IShader> extract_ps = ps(kBloomExtractPS, "NativeXRenderer weapon skin gun PS");
    RefCntAutoPtr<IShader> anchor_ps = ps(kAnchorPS, "NativeXRenderer weapon skin anchor PS");
    BufferDesc d;
    d.Name = "NativeXRenderer weapon skin overlay constants";
    d.Size = 240;
    d.Usage = USAGE_DYNAMIC;
    d.BindFlags = BIND_UNIFORM_BUFFER;
    d.CPUAccessFlags = CPU_ACCESS_WRITE;
    device->CreateBuffer(d, nullptr, &b.constants);
    if (!b.vs || !b.area_vs || !extract_ps || !anchor_ps || !b.constants) {
      REXLOG_ERROR("[native_x_renderer] weapon skin: the overlay's shaders did not compile; no storm's light or smoke");
      return false;
    }
    b.extract = BloomPipeline(device, "NativeXRenderer weapon skin gun", b.vs, extract_ps, TEX_FORMAT_RGBA16_FLOAT, Blend::kNone,
                              {{SHADER_TYPE_PIXEL, "fxTex", false}});
    b.anchor_pso = BloomPipeline(device, "NativeXRenderer weapon skin anchor", b.vs, anchor_ps, TEX_FORMAT_RGBA16_FLOAT, Blend::kEase,
                                 {{SHADER_TYPE_PIXEL, "bloomTex", true}, {SHADER_TYPE_PIXEL, "depthTex", false}});
    if (b.extract) b.extract->CreateShaderResourceBinding(&b.extract_srb, true);
    if (b.anchor_pso) b.anchor_pso->CreateShaderResourceBinding(&b.anchor_srb, true);
    TextureDesc t;
    t.Name = "NativeXRenderer weapon skin anchor";
    t.Type = RESOURCE_DIM_TEX_2D;
    t.Width = 8, t.Height = 1;
    t.MipLevels = 1;
    t.Format = TEX_FORMAT_RGBA16_FLOAT;
    t.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
    device->CreateTexture(t, nullptr, &b.anchor);
    if (!b.extract_srb || !b.anchor_srb || !b.anchor) {
      b.extract.Release();
      REXLOG_ERROR("[native_x_renderer] weapon skin: no overlay pipeline");
      return false;
    }
    ITextureView* anchor_rtv = b.anchor->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    const float none[4] = {0.5f, 0.5f, 0.0f, 0.0f};
    context->SetRenderTargets(1, &anchor_rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->ClearRenderTarget(anchor_rtv, none, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
    b.failed = false;
  }
  const TEXTURE_FORMAT format = in.color->GetDesc().Format;
  if (!b.apply || b.apply_format != format) {
    b.apply.Release(), b.apply_srb.Release();
    RefCntAutoPtr<IShader> apply_ps = Compile(device, SHADER_TYPE_PIXEL, ApplyPsSource().c_str(),
                                              "NativeXRenderer weapon skin bloom PS");
    if (apply_ps)
      b.apply = BloomPipeline(device, "NativeXRenderer weapon skin bloom", b.area_vs, apply_ps, format, Blend::kOver,
                              {{SHADER_TYPE_PIXEL, "bloomTex", true},
                               {SHADER_TYPE_PIXEL, "anchorTex", false},
                               {SHADER_TYPE_PIXEL, "depthTex", false},
                               {SHADER_TYPE_PIXEL, "fxTex", true},
                               {SHADER_TYPE_VERTEX, "anchorVsTex", false}});
    if (b.apply) b.apply->CreateShaderResourceBinding(&b.apply_srb, true);
    b.apply_format = format;
    if (!b.apply_srb) {
      b.apply.Release();
      REXLOG_ERROR("[native_x_renderer] weapon skin: no overlay pipeline for the format {}", int(format));
      return false;
    }
    b.fx = MakeFx(device, format);
    if (!b.fx) REXLOG_ERROR("[native_x_renderer] weapon skin: its particles could not be made; no smoke");
    REXLOG_INFO("[native_x_renderer] weapon skin: the storm's light on the gun's mesh{} over a picture of format {}",
                b.fx ? ", its smoke, sparks and bolts" : "", int(format));
  }
  const uint32_t qw = (in.width + 3) / 4, qh = (in.height + 3) / 4;
  if (!b.quarter || b.quarter->GetDesc().Width != qw || b.quarter->GetDesc().Height != qh) {
    b.quarter.Release();
    TextureDesc d;
    d.Name = "NativeXRenderer weapon skin gun";
    d.Type = RESOURCE_DIM_TEX_2D;
    d.Width = qw, d.Height = qh;
    d.MipLevels = 4;
    d.Format = TEX_FORMAT_RGBA16_FLOAT;
    d.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
    d.MiscFlags = MISC_TEXTURE_FLAG_GENERATE_MIPS;
    device->CreateTexture(d, nullptr, &b.quarter);
    if (!b.quarter) return false;
  }
  // Developer's checks: NXR_SKIN_BLOOM=<strength> (0.5), NXR_SKIN_BLOOM_SHOW=1 (the overlay alone),
  // NXR_SKIN_GLOW=<the storm's light's strength over the picture> (1; 0: none), NXR_SKIN_FX=0 (no particles).
  static const float show = EnvFloat("NXR_SKIN_BLOOM_SHOW", 0.0f), strength = EnvFloat("NXR_SKIN_BLOOM", 0.5f),
                     glow_to = EnvFloat("NXR_SKIN_GLOW", 1.0f), fx_on = EnvFloat("NXR_SKIN_FX", 1.0f);
  // The particles only while the gun itself is drawn (its frame told within the last tenth of a second): with no gun in
  // the picture they were still drawn where it had last been (the user, 24 Nov 2026: "Our new SMG shader skin's smoke
  // stays on screen when entering vehicles").
  // (Only whether it was drawn: GunPoints also turns down a frame whose back is at the eye, as it is while aiming down
  // the sights, and the smoke went with it: the user, 24 Nov 2026, "when aiming down sights on SMG, the skin and cloud
  // smoke disappear entirely".)
  const bool fx = b.fx && fx_on > 0.0f && in.px > 0.01f && in.py > 0.01f && GunDrawnLately();
  {
    const double now = Clock();
    const float dt = b.last < 0.0 ? 1.0f / 60.0f : float(std::clamp(now - b.last, 0.001, 0.05));
    b.last = now;
    float k[60] = {float(in.width), float(in.height), float(in.depth_width), float(in.depth_height),
                   float(qw), float(qh), strength * AimDim(), show};
    // (The storm stays while aiming; its cloud keeps out of the sight line: k[47], the shaders' gun_up.w.)
    k[8] = float(std::fmod(now, 3600.0)), k[9] = 1.0f;
    k[10] = 1.0f - std::exp(-dt * 14.0f), k[11] = std::clamp(in.dark, 0.0f, 1.0f) * (in.after_night ? -1.0f : 1.0f);
    Strikes(now, &k[20], &k[28]);
    k[26] = ChargeAge(now);  // (the bolts' seventh age: a new magazine's charge)
    float flash = k[26] >= 0.0f ? std::exp(-k[26] * 3.0f) : 0.0f;
    for (uint32_t i = 0; i < kBolts; ++i) {
      const float age = k[20 + i];
      if (age >= 0.0f) flash = std::max(flash, age < 0.3f ? 1.0f : std::exp(-(age - 0.3f) * 9.0f));
    }
    // (A little less of the storm's light by night, when the picture round it is dark.)
    k[12] = flash, k[13] = dt, k[14] = float((++b.frame * 2654435761u) & 0xFFFFFFu) / 16777216.0f;
    // (By night the scene is a twelfth as bright, D177: the storm's light with it, though less, so it still stands out.)
    k[15] = glow_to * (in.after_night ? 0.8f : 1.0f - 0.8f * k[11]);
    k[16] = in.px, k[17] = in.py, k[18] = float(kSmoke), k[19] = float(kParticles);
    // The magazine: under the gun or out to its left, a little ahead of the gun's middle as seen, 13 cm long.
    // NXR_SKIN_MAG_ALONG and NXR_SKIN_MAG_LENGTH (metres) are developer's checks.
    static const float mag_along = EnvFloat("NXR_SKIN_MAG_ALONG", 0.03f), mag_length = EnvFloat("NXR_SKIN_MAG_LENGTH", 0.13f);
    k[36] = float(g_magazine.load(std::memory_order_relaxed)), k[37] = mag_along, k[38] = mag_length;
    // The smoke's stretch of the gun, in parts of its half length from its middle (behind it negative): from the
    // back of the receiver to just before the muzzle. NXR_SKIN_SMOKE_FROM / _TO, and NXR_SKIN_BOLTS=1 (the bolts,
    // the strike and the sparks drawn) are developer's checks.
    static const float smoke_from = EnvFloat("NXR_SKIN_SMOKE_FROM", -0.35f), smoke_to = EnvFloat("NXR_SKIN_SMOKE_TO", 1.02f)  /* the muzzle: the user, 22 Nov 2026, "All the way down to the muzzle" */,
                       bolts = EnvFloat("NXR_SKIN_BOLTS", 0.0f);
    k[34] = smoke_from, k[35] = smoke_to;
    // (mg follows the gun's five vectors in the buffer.)
    for (int i = 0; i < 4; ++i) k[56 + i] = k[36 + i];
    k[59] = bolts;
    for (int i = 36; i < 56; ++i) k[i] = 0.0f;
    const bool points = GunPoints(&k[36]);
    if (points) k[34] = -1.0f, k[35] = 1.0f;  // the stretch is the barrel's line itself, end to end
    k[47] = AimNow();
    // NXR_SKIN_AIM_LOG=1 (a diagnosis aid, 28 Nov 2026): twice a second while aiming, what the overlay goes by.
    if (static const bool aim_log = std::getenv("NXR_SKIN_AIM_LOG") != nullptr; aim_log && k[47] > 0.3f) {
      static double said_at = -1.0;
      if (now - said_at > 0.5) {
        said_at = now;
        REXLOG_INFO("[native_x_renderer] skin aim overlay: aim {:.2f}, fx {}, points {}, back {:.3f} {:.3f} {:.3f}, muzzle {:.3f} "
                    "{:.3f} {:.3f}, px {:.3f} py {:.3f}",
                    k[47], fx, points, k[36], k[37], k[38], k[40], k[41], k[42], in.px, in.py);
      }
    }
    void* data = nullptr;
    context->MapBuffer(b.constants, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, k, sizeof(k));
    context->UnmapBuffer(b.constants, MAP_WRITE);
  }
  ITextureView* color_rtv = in.color->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
  ITextureView* over_srv = b.over->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  if (!color_rtv || !over_srv) return false;
  struct Input {
    SHADER_TYPE stage;
    const char* name;
    IDeviceObject* object;
  };
  auto run = [&](IPipelineState* pso, IShaderResourceBinding* srb, ITextureView* rtv, uint32_t w, uint32_t h,
                 std::initializer_list<Input> inputs, uint32_t vertices = 3, uint32_t instances = 1) {
    context->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Viewport viewport;
    viewport.Width = float(w);
    viewport.Height = float(h);
    viewport.MaxDepth = 1.0f;
    context->SetViewports(1, &viewport, 0, 0);
    context->SetPipelineState(pso);
    for (SHADER_TYPE stage : {SHADER_TYPE_VERTEX, SHADER_TYPE_PIXEL})
      if (auto* v = srb->GetVariableByName(stage, "BloomConstants")) v->Set(b.constants);
    for (const Input& i : inputs)
      if (auto* v = srb->GetVariableByName(i.stage, i.name)) v->Set(i.object);
    context->CommitShaderResources(srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    DrawAttribs draw;
    draw.NumVertices = vertices;
    draw.NumInstances = instances;
    context->Draw(draw);
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  };
  const SHADER_TYPE P = SHADER_TYPE_PIXEL, V = SHADER_TYPE_VERTEX;
  // The mesh pass's picture a quarter the view's size, with its mips.
  TextureViewDesc first;
  first.ViewType = TEXTURE_VIEW_RENDER_TARGET;
  first.TextureDim = RESOURCE_DIM_TEX_2D;
  first.MostDetailedMip = 0, first.NumMipLevels = 1;
  RefCntAutoPtr<ITextureView> quarter_rtv;
  b.quarter->CreateView(first, &quarter_rtv);
  if (!quarter_rtv) return false;
  run(b.extract, b.extract_srb, quarter_rtv, qw, qh, {{P, "fxTex", over_srv}});
  ITextureView* quarter_srv = b.quarter->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  context->GenerateMips(quarter_srv);
  ITextureView* anchor_srv = b.anchor->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  run(b.anchor_pso, b.anchor_srb, b.anchor->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET), 8, 1,
      {{P, "bloomTex", quarter_srv}, {P, "depthTex", in.depth_srv}});
  if (fx) {  // the particles' step, and they and the bolts drawn
    for (const Input& i : {Input{SHADER_TYPE_COMPUTE, "BloomConstants", b.constants}, Input{SHADER_TYPE_COMPUTE, "anchorTex", anchor_srv}})
      if (auto* v = b.fx_srb->GetVariableByName(i.stage, i.name)) v->Set(i.object);
    context->SetPipelineState(b.fx_pso);
    context->CommitShaderResources(b.fx_srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->DispatchCompute(DispatchComputeAttribs((kParticles + 63) / 64, 1, 1));
    run(b.particles_pso, b.particles_srb, color_rtv, in.width, in.height, {{P, "depthTex", in.depth_srv}, {V, "anchorTex", anchor_srv}},
        6, kParticles);
    run(b.bolts_pso, b.bolts_srb, color_rtv, in.width, in.height, {{P, "depthTex", in.depth_srv}, {V, "anchorTex", anchor_srv}},
        6 * kBoltPieces, kBolts);
  }
  run(b.apply, b.apply_srb, color_rtv, in.width, in.height,
      {{P, "bloomTex", quarter_srv}, {P, "anchorTex", anchor_srv}, {P, "depthTex", in.depth_srv}, {P, "fxTex", over_srv},
       {V, "anchorVsTex", anchor_srv}});
  return true;
}

}  // namespace native_x_renderer
