// NativeXRenderer - night (internal, D162): the finished scene graded to a moonlit night, before the HUD.
//
// Battlefield 1943 Reborn's Night Ops servers (TekRantGaming, 11 Nov 2026: "cant be pitch black due to it being
// unplayable"): the game's own day, drawn as night on its composite (the first draw of the output's size after the
// profile's scene copy, Profile::scene_resolve_caller: the picture with the game's bloom and exposure), so the HUD drawn
// after it keeps its colours. At the scene copy itself the game's automatic exposure brightened it back. Per pixel, in
// linear light:
//   - moonlight: the colour mostly taken out (the eye sees little colour at night), a cool blue, much less light;
//   - a night haze with distance from the main view's depth (Battlefield 1943's world stores 0.1 / distance), which
//     also turns the day sky, far away, into a dark night sky with its clouds faintly lit.
// Muzzle flashes and explosions are darkened like the rest (still the brightest things on screen): keeping the
// brightest pixels as they were kept sunlit sand, rock and grass glowing too (run night_wake3).
// D163: the lights too: the effects' own light (the game's half size layer of emissive particles), what they light,
// the level's lamps and models' lamps, from the camera kept from the terrain's draws.
// Header only: renderer.cpp alone includes it. Off unless native_x_renderer_night is on.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"
#include "TextureView.h"
#include "glsl.h"
#include <native_x_renderer/native_x_renderer.h>

#include <rex/logging.h>

namespace native_x_renderer {

struct NightSettings {
  float brightness = 1.0f;  // the moonlight: 1 the default, 0.5 darker, 2 brighter
  float haze_m = 700.0f;    // the distance at which the night haze hides about two thirds of what is there
  float glow = 1.0f;        // D163: the effects' own light (muzzle flashes, tracers, fire, explosions); 0 none
  float lamps = 1.0f;       // D163: the lamps' light and glow; 0 none
  int debug = 0;            // D163, tests: 1 each pixel's place in the world, 2 the lights alone
  float sky = 1.0f;         // D169: the night sky's stars, Milky Way and moon; 0 none
};

// D163: a light chosen for this frame (Night::Apply takes kMaxLights): a point light, or a spot when `direction` is set.
struct NightLight {
  float position[3] = {};
  float range = 0.0f;       // metres: no light beyond
  float color[3] = {};      // linear, times the light's strength
  float cos_outer = -1.0f;  // a spot: no light beyond this angle from its axis
  float direction[3] = {};  // a spot's axis (unit); 0, 0, 0: a point light
  float cos_inner = 1.0f;   // a spot: its full light within this angle
};

// D163: a lamp's bulb on screen: a glow where nothing nearer hides it.
struct NightSprite {
  float uv[2] = {};      // the target's texture coordinates
  float radius[2] = {};  // in those coordinates, across and down
  float depth = 0.0f;    // the bulb's stored depth (0.1 / distance): nearer pixels hide it
  float color[3] = {};   // linear, times its strength
};

// What one night pass reads besides the picture.
struct NightFrame {
  // The main view's depth at its end and the world's from before the first person weapon (D162), each with the share
  // of its texture the view covers.
  Diligent::ITextureView* depth = nullptr;
  float depth_uv[2] = {1.0f, 1.0f};
  Diligent::ITextureView* world = nullptr;
  float world_uv[2] = {1.0f, 1.0f};
  // D163: the effects' layer (a texture with its mips: Battlefield 1943's half size pass of its emissive particles).
  Diligent::ITextureView* glow = nullptr;
  float glow_uv[2] = {1.0f, 1.0f};
  // D163: the camera, for the lights: a depth texture coordinate to NDC (ndc = uv * xy + zw), the world from clip x,
  // y and w (world = inverse x ((x, y, w) - t), rows), and the eye.
  bool camera = false;
  float to_ndc[4] = {};
  float inverse[3][3] = {};
  float inverse_t[3] = {};
  float eye[3] = {};
  // D169: the sky's directions (the sky dome's camera, or the terrain's), as `to_ndc` and `inverse`; the level's style.
  bool sky_camera = false;
  float sky_to_ndc[4] = {};
  float sky_inverse[3][3] = {};
  const NightSkyStyle* sky_style = nullptr;
  bool sun_valid = false;  // the game's sun (toward it), from the sky dome's draw: the moon goes there
  float sun[3] = {};
  const NightLight* lights = nullptr;
  uint32_t light_count = 0;
  const NightSprite* sprites = nullptr;
  uint32_t sprite_count = 0;
};

class Night {
 public:
  static constexpr uint32_t kMaxLights = 24;
  static constexpr uint32_t kMaxSprites = 24;

  // `texture`, the finished scene, graded in place (drawn into `target`, its render target view). `f.depth`: the main
  // view's depth (its shader view) and the share of it the view covers; null for no haze and no sky. `f.world`: the
  // world's depth taken before the first person weapon's draws clear it (an R32 float copy, its red channel; null
  // without one). Per pixel the nearer of the two is taken: on foot the view's depth at its end holds only the weapon
  // (run nightzip1: everything else read as far away, all haze), and the world's copy has no weapon yet.
  // D163: the lights (lamps and what the effects light), the lamps' glows and the effects' own light, from `f`.
  void Apply(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, Diligent::ITexture* texture,
             Diligent::ITextureView* target, const NightFrame& f, const NightSettings& s) {
    using namespace Diligent;
    if (!device || !context || !texture || !target) return;
    const TextureDesc& desc = texture->GetDesc();
    Pass* pass = PassFor(device, desc.Format);
    if (!pass || !EnsureCopy(device, desc)) return;
    const uint32_t lights = f.camera && s.lamps > 0.0f ? std::min(f.light_count, kMaxLights) : 0;
    const uint32_t sprites = s.lamps > 0.0f ? std::min(f.sprite_count, kMaxSprites) : 0;
    const bool glow = f.glow && s.glow > 0.0f;
    {
      Constants k = {};
      k.night[0] = s.brightness, k.night[1] = std::max(s.haze_m, 1.0f);
      k.night[2] = f.depth_uv[0], k.night[3] = f.depth_uv[1];
      k.flags[0] = f.depth ? 1.0f : 0.0f, k.flags[1] = f.world ? 1.0f : 0.0f;
      k.flags[2] = f.world_uv[0], k.flags[3] = f.world_uv[1];
      k.glow[0] = glow ? 1.0f : 0.0f, k.glow[1] = s.glow, k.glow[2] = f.glow_uv[0], k.glow[3] = f.glow_uv[1];
      std::memcpy(k.to_ndc, f.to_ndc, sizeof(k.to_ndc));
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) k.inverse[r][c] = f.inverse[r][c];
      for (int i = 0; i < 3; ++i) k.inverse_t[i] = f.inverse_t[i], k.eye[i] = f.eye[i];
      k.inverse_t[3] = f.camera ? 1.0f : 0.0f;
      k.eye[3] = s.lamps;
      k.counts[0] = float(lights), k.counts[1] = float(sprites);
      k.counts[2] = float(desc.Width) / float(std::max(1u, desc.Height));  // the target's aspect, for round glows
      k.counts[3] = float(s.debug);
      k.sky[0] = s.sky;
      k.sky[1] = f.sky_camera ? 1.0f : 0.0f;
      std::memcpy(k.sky_to_ndc, f.sky_to_ndc, sizeof(k.sky_to_ndc));
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) k.sky_inverse[r][c] = f.sky_inverse[r][c];
      {
        const NightSkyStyle st = f.sky_style ? *f.sky_style : NightSkyStyle{};
        const float* md = f.sun_valid ? f.sun : st.moon_direction;  // the game's sun when its sky gave it
        const float ml = std::sqrt(md[0] * md[0] + md[1] * md[1] + md[2] * md[2]);
        for (int i = 0; i < 3; ++i) {
          k.moon[i] = ml > 1e-6f ? md[i] / ml : (i == 1 ? 1.0f : 0.0f);
          k.moon_color[i] = st.moon_color[i];
          k.horizon[i] = st.horizon_color[i];
        }
        k.moon[3] = std::clamp(st.moon_radius, 0.0f, 0.1f);
        k.moon_color[3] = std::clamp(st.cloud_dim, 0.0f, 1.0f);
        k.horizon[3] = std::clamp(st.horizon_height, 0.01f, 0.9f);
        k.stars[0] = std::max(st.stars, 0.0f), k.stars[1] = std::max(st.milky_way, 0.0f), k.stars[2] = st.seed;
      }
      // The effects' lights: found in the effects' layer's mip about 48 texels wide.
      const bool effect_lights = glow && f.camera;
      if (effect_lights && device && LightsPass(device)) {
        const float width = float(f.glow->GetTexture()->GetDesc().Width);
        k.glow2[0] = std::max(0.0f, std::round(std::log2(std::max(width, 1.0f) / 48.0f)));
        k.glow2[1] = 1.1f * s.glow;
        k.glow2[2] = 1.0f;
      }
      for (uint32_t i = 0; i < lights; ++i) {
        const NightLight& l = f.lights[i];
        float* a = k.lights[i * 3];
        a[0] = l.position[0], a[1] = l.position[1], a[2] = l.position[2], a[3] = l.range;
        a[4] = l.color[0], a[5] = l.color[1], a[6] = l.color[2], a[7] = l.cos_outer;
        a[8] = l.direction[0], a[9] = l.direction[1], a[10] = l.direction[2], a[11] = l.cos_inner;
      }
      for (uint32_t i = 0; i < sprites; ++i) {
        const NightSprite& p = f.sprites[i];
        float* a = k.sprites[i * 2];
        a[0] = p.uv[0], a[1] = p.uv[1], a[2] = p.radius[0], a[3] = p.radius[1];
        a[4] = p.color[0], a[5] = p.color[1], a[6] = p.color[2], a[7] = p.depth;
      }
      void* data = nullptr;
      context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
      std::memcpy(data, &k, sizeof(k));
      context->UnmapBuffer(constants_, MAP_WRITE);
    }
    // D163: the effects' lights into their 4 x 2 texture, first.
    const bool effect_lights = glow && f.camera && LightsPass(device);
    if (effect_lights) {
      context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
      auto set = [&](const char* name, ITextureView* view) {
        if (auto* v = lights_pass_.srb->GetVariableByName(SHADER_TYPE_PIXEL, name)) v->Set(view);
      };
      ITextureView* any = f.glow;
      set("glowTex", f.glow);
      set("depthTex", f.depth ? f.depth : any);
      set("worldDepthTex", f.world ? f.world : any);
      set("colorTex", any);
      set("effectLightsTex", any);
      ITextureView* rtv = effect_lights_->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
      context->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      Viewport small;
      small.Width = 4.0f;
      small.Height = 2.0f;
      small.MaxDepth = 1.0f;
      context->SetViewports(1, &small, 0, 0);
      context->SetPipelineState(lights_pass_.pso);
      context->CommitShaderResources(lights_pass_.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      DrawAttribs lights_draw;
      lights_draw.NumVertices = 3;
      context->Draw(lights_draw);
      context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
      ++applied_effect_lights_;
    }
    // The input: a copy of the scene, which the pass writes over (the antialiasing's way: unbound first).
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
    CopyTextureAttribs copy{texture, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, copy_,
                            RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
    context->CopyTexture(copy);
    ITextureView* color = copy_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    pass->srb->GetVariableByName(SHADER_TYPE_PIXEL, "colorTex")->Set(color);
    pass->srb->GetVariableByName(SHADER_TYPE_PIXEL, "depthTex")->Set(f.depth ? f.depth : color);
    pass->srb->GetVariableByName(SHADER_TYPE_PIXEL, "worldDepthTex")->Set(f.world ? f.world : color);
    pass->srb->GetVariableByName(SHADER_TYPE_PIXEL, "glowTex")->Set(glow ? f.glow : color);
    if (auto* v = pass->srb->GetVariableByName(SHADER_TYPE_PIXEL, "effectLightsTex"))
      v->Set(effect_lights ? effect_lights_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : color);
    context->SetRenderTargets(1, &target, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    const TextureDesc& out = target->GetTexture()->GetDesc();
    Viewport viewport;
    viewport.Width = float(out.Width);
    viewport.Height = float(out.Height);
    viewport.MaxDepth = 1.0f;
    context->SetViewports(1, &viewport, 0, 0);
    context->SetPipelineState(pass->pso);
    context->CommitShaderResources(pass->srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    DrawAttribs draw;
    draw.NumVertices = 3;
    context->Draw(draw);
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
    ++applied_;
    if (f.world) ++applied_world_;
    if (glow) ++applied_glow_;
    lights_ += lights, sprites_ += sprites;
    if (!logged_) {
      logged_ = true;
      REXLOG_INFO("[native_x_renderer] night: on the finished scene, {}x{} format {}, {}{} (D162)", desc.Width,
                  desc.Height, int(desc.Format), f.depth ? "with the view's depth (haze and sky)" : "without the view's depth",
                  f.world ? ", and the world's from before the weapon" : "");
    }
    if (f.world && !logged_world_) {
      logged_world_ = true;
      REXLOG_INFO("[native_x_renderer] night: the world's depth from before the first person weapon in use (D162)");
    }
    if (glow && !logged_glow_) {
      logged_glow_ = true;
      REXLOG_INFO("[native_x_renderer] night: the effects' own light in use (D163)");
    }
    if (lights && !logged_lights_) {
      logged_lights_ = true;
      REXLOG_INFO("[native_x_renderer] night: lights in use, {} in this frame, {} glows (D163)", lights, sprites);
    }
  }
  // Since the last call: the passes with the world's depth, those with the effects' light, and the lights and glows
  // drawn in them together.
  void TakeCounts(uint64_t& world, uint64_t& glow, uint64_t& lights, uint64_t& sprites) {
    world = applied_world_, glow = applied_glow_, lights = lights_, sprites = sprites_;
    applied_world_ = applied_glow_ = lights_ = sprites_ = 0;
  }
  uint64_t TakeApplied() {
    const uint64_t n = applied_;
    applied_ = 0;
    return n;
  }

 private:
  struct Pass {
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
  };

  static constexpr const char* kVertex = R"(#version 450
layout(location = 0) out vec2 v_texcoord;
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  v_texcoord = vec2(p.x, 1.0 - p.y);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
  // What both passes read: the constants, the textures, and the helpers.
  static constexpr const char* kCommon = R"(#version 450
layout(set = 0, binding = 0) uniform NightConstants {
  vec4 night;   // x: brightness, y: haze distance (m), z, w: the view's share of the depth texture
  vec4 flags;   // x: 1 with the view's depth, y: 1 with the world's (from before the first person weapon), z, w: its share
  vec4 glowp;   // D163: x: 1 with the effects' layer, y: its strength, z, w: its share
  vec4 to_ndc;  // D163: the night target's texture coordinate to NDC (uv * xy + zw)
  vec4 inv0;    // D163: the world from clip x, y and w (rows)
  vec4 inv1;
  vec4 inv2;
  vec4 inv_t;   // xyz: clip x, y and w at the world's origin; w: 1 with the camera
  vec4 eye;     // xyz: the camera; w: the lamps' strength
  vec4 counts;  // x: lights, y: glows, z: the target's aspect, w: tests (1 the world's place, 2 the lights alone)
  vec4 glow2;   // x: the coarse mip the effects' lights are found in, y: their strength, z: 1 with them
  vec4 skyp;    // D169: x: the night sky's strength (stars, Milky Way, moon); 0 none; y: 1 with its directions
  vec4 sky0;    // D169: the sky's directions from clip x, y and w (rows), and its NDC from a texture coordinate
  vec4 sky1;
  vec4 sky2;
  vec4 skyndc;
  vec4 moonp;   // D169, the level's sky: xyz toward the moon, w its radius
  vec4 moonc;   //   rgb the moon's colour, w the sky's share behind clouds
  vec4 horizon; //   rgb the haze low on the sky, w how high it reaches
  vec4 starp;   //   x the stars, y the Milky Way, z the stars' seed
  vec4 lights[72];   // 24 x (position, range), (colour, cos outer), (direction, cos inner)
  vec4 sprites[48];  // 24 x (uv, radius), (colour, the bulb's depth)
};
layout(set = 0, binding = 1) uniform sampler2D colorTex;
layout(set = 0, binding = 2) uniform sampler2D depthTex;
layout(set = 0, binding = 3) uniform sampler2D worldDepthTex;
layout(set = 0, binding = 4) uniform sampler2D glowTex;
layout(set = 0, binding = 5) uniform sampler2D effectLightsTex;

// The stored depth at a night target coordinate: the nearer of the view's at its end and the world's from before the
// first person weapon (the game's depth, 0.1 / distance, grows toward the camera).
float DepthAt(vec2 uv) {
  float v = flags.x > 0.5 ? textureLod(depthTex, uv * night.zw, 0.0).r : 0.0;
  float w = flags.y > 0.5 ? textureLod(worldDepthTex, uv * flags.zw, 0.0).r : 0.0;
  return max(v, w);
}
// The world point a night target coordinate and its stored depth show.
vec3 WorldAt(vec2 uv, float depth) {
  float w = 0.1 / max(depth, 1e-7);
  vec2 ndc = uv * to_ndc.xy + to_ndc.zw;
  vec3 c = vec3(ndc * w, w) - inv_t.xyz;
  return vec3(dot(inv0.xyz, c), dot(inv1.xyz, c), dot(inv2.xyz, c));
}
// How much of the effects' layer is light of its own: fire, flashes and tracers are coloured or very bright; the grey
// smoke and dust drawn with them is neither (it would glow white at night).
float Hot(vec3 e) {
  float hi = max(e.r, max(e.g, e.b)), lo = min(e.r, min(e.g, e.b));
  float saturation = hi > 1e-4 ? (hi - lo) / hi : 0.0;
  float lum = dot(e, vec3(0.2126, 0.7152, 0.0722));
  return max(smoothstep(0.12, 0.45, saturation), smoothstep(1.5, 6.0, lum));
}
)";

  // D163: the effects' lights: up to four of the brightest hot places of the effects' layer (in its coarse mip), each
  // where the nearest surface round it is, a little toward the camera. Column i is light i: row 0 its world position
  // and range, row 1 its colour.
  static constexpr const char* kPixelLights = R"(
layout(location = 0) out vec4 out_light;
void main() {
  ivec2 me = ivec2(gl_FragCoord.xy);
  out_light = vec4(0.0);
  if (glowp.x < 0.5 || inv_t.w < 0.5) return;
  int lod = int(glow2.x + 0.5);
  ivec2 size = textureSize(glowTex, lod);
  ivec2 picked[4];
  float energy = 0.0;
  ivec2 at = ivec2(-1);
  for (int k = 0; k <= me.x && k < 4; ++k) {
    float best = 0.0;
    ivec2 b = ivec2(-1);
    for (int y = 0; y < size.y; ++y) {
      for (int x = 0; x < size.x; ++x) {
        bool taken = false;
        for (int j = 0; j < k; ++j) {
          if (max(abs(x - picked[j].x), abs(y - picked[j].y)) <= 4) taken = true;
        }
        if (taken) continue;
        vec3 e = max(texelFetch(glowTex, ivec2(x, y), lod).rgb, vec3(0.0));
        float w = dot(e, vec3(0.2126, 0.7152, 0.0722)) * Hot(e);
        if (w > best) best = w, b = ivec2(x, y);
      }
    }
    picked[k] = b;
    energy = best;
    at = b;
  }
  if (at.x < 0 || energy < 0.004) return;
  vec2 euv = (vec2(at) + 0.5) / vec2(size);  // the view's 0 to 1
  vec2 cell = 1.0 / vec2(size);
  float depth = 0.0;
  for (int j = -2; j <= 2; ++j) {
    for (int i = -2; i <= 2; ++i) depth = max(depth, DepthAt((euv + vec2(i, j) * cell * 0.75) / glowp.zw));
  }
  if (depth < 2e-5) return;  // only the sky round it: nothing near to light
  vec3 P = WorldAt(euv / glowp.zw, depth);
  vec3 toward = eye.xyz - P;
  P += toward * min(0.35, length(toward) * 0.3) / max(length(toward), 1e-3);
  // Its strength: the coarse texel's light times the texels of the layer in it.
  float power = min(energy * exp2(2.0 * float(lod)) / (float(textureSize(glowTex, 0).x) * 0.25), 2.5);
  vec3 colour = max(texelFetch(glowTex, at, lod).rgb, vec3(0.0));
  colour /= max(max(colour.r, max(colour.g, colour.b)), 1e-4);
  float range = clamp(3.5 + sqrt(power) * 5.0, 3.5, 16.0);
  out_light = me.y == 0 ? vec4(P, range) : vec4(colour * power * glow2.y, 0.0);
}
)";

  static constexpr const char* kPixel = R"(
layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 out_color;

// D169: the night sky, made here (nothing drawn or licensed: TekRantGaming, 14 Nov 2026, "a nighttime skybox ... open
// for us to distribute ... the stars look good. Not over the top"). `d` is a direction in the world (y up), `px` the
// angle one pixel covers there. Stars on a grid of each cube face, one at most a cell, kept within its cell's middle
// so a pixel looks in its own cell only; a faint Milky Way band; a moon with a soft halo.
float Hash13(vec3 p) {
  p = fract(p * 0.1031);
  p += dot(p, p.zyx + 31.32);
  return fract((p.x + p.y) * p.z);
}
vec3 Hash33(vec3 p) {
  p = fract(p * vec3(0.1031, 0.1030, 0.0973));
  p += dot(p, p.yxz + 33.33);
  return fract((p.xxy + p.yxx) * p.zyx);
}
float Noise3(vec3 p) {
  vec3 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(mix(Hash13(i), Hash13(i + vec3(1, 0, 0)), f.x), mix(Hash13(i + vec3(0, 1, 0)), Hash13(i + vec3(1, 1, 0)), f.x), f.y),
             mix(mix(Hash13(i + vec3(0, 0, 1)), Hash13(i + vec3(1, 0, 1)), f.x), mix(Hash13(i + vec3(0, 1, 1)), Hash13(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
float Fbm(vec3 p) {
  float v = 0.0, a = 0.5;
  for (int i = 0; i < 4; ++i) {
    v += a * Noise3(p);
    p = p * 2.03 + 17.1;
    a *= 0.5;
  }
  return v;
}
// One layer of stars: `cells` across a cube face, `chance` of a star in a cell, `bright` the brightest.
vec3 StarLayer(vec3 d, float px, float cells, float chance, float bright, float seed) {
  vec3 a = abs(d);
  float face, m;
  vec2 uv;
  if (a.x >= a.y && a.x >= a.z) {
    face = d.x > 0.0 ? 0.0 : 1.0; m = a.x; uv = d.yz;
  } else if (a.y >= a.z) {
    face = d.y > 0.0 ? 2.0 : 3.0; m = a.y; uv = d.xz;
  } else {
    face = d.z > 0.0 ? 4.0 : 5.0; m = a.z; uv = d.xy;
  }
  uv = uv / m * cells;
  vec2 cell = floor(uv);
  vec3 h = Hash33(vec3(cell, face * 7.0 + seed));
  if (h.z > chance) return vec3(0.0);
  vec2 at = cell + 0.25 + 0.5 * h.xy;  // the star, in the cell's middle half
  float dist = length(uv - at) / cells * m;  // about the angle, on that face
  float b = Hash13(vec3(cell * 1.7, face + seed * 3.1));
  float strength = bright * pow(b, 6.0) + bright * 0.06;  // a few bright, many faint
  float sigma = max(px * 0.55, 0.00035);
  float shape = exp(-dist * dist / (2.0 * sigma * sigma));
  // A star smaller than a pixel keeps its light: spread over the pixel, not lost.
  strength *= min(1.0, 0.00045 / sigma) + 0.15;
  float t = Hash13(vec3(cell * 3.3, face + seed * 5.7));
  vec3 tint = t < 0.15 ? vec3(1.0, 0.80, 0.62) : t < 0.35 ? vec3(1.0, 0.93, 0.84) : t < 0.8 ? vec3(0.95, 0.97, 1.0) : vec3(0.78, 0.86, 1.0);
  return tint * strength * shape;
}
// `veil`: the stars' share left (the day sky's clouds and the haze low on the sky); the moon shows through more.
vec3 NightSky(vec3 d, float px, float veil, float clear) {
  vec3 c = vec3(0.0);
  // The Milky Way: a soft band across the sky, patchy, with darker lanes; very faint.
  const vec3 band_n = normalize(vec3(0.35, 0.42, 0.84));
  float across = dot(d, band_n);
  float band = exp(-across * across / (2.0 * 0.13 * 0.13));
  float patches = Fbm(d * 5.0 + 3.0);
  float lanes = smoothstep(0.42, 0.62, Fbm(d * 11.0 + 41.0)) * exp(-across * across / (2.0 * 0.05 * 0.05));
  c += vec3(0.85, 0.88, 1.0) * band * (0.35 + 0.9 * patches) * (1.0 - 0.75 * lanes) * 0.0045 * starp.y;
  // Stars: a sparse bright layer and denser faint ones, more of the faint ones in the band; fewer and dimmer on a hazy
  // night (the faint ones go first).
  float st = starp.x, seed = starp.z;
  c += StarLayer(d, px, 34.0, 0.33 * min(st, 1.0), 0.85 * st, 1.0 + seed);
  c += StarLayer(d, px, 90.0, (0.22 + 0.25 * band * starp.y) * clamp(st * st, 0.0, 1.0), 0.16 * st, 2.0 + seed);
  c += StarLayer(d, px, 170.0, (0.10 + 0.35 * band * starp.y) * clamp(st * st * st, 0.0, 1.0), 0.06 * st, 3.0 + seed);
  c *= veil;
  // The moon: a small disc, not full white, a little texture, its edge darker, a soft halo round it.
  vec3 moon_d = moonp.xyz;
  float moon_r = max(moonp.w, 1e-4);
  float ang = acos(clamp(dot(d, moon_d), -1.0, 1.0));
  vec3 rel = d - moon_d * dot(d, moon_d);
  float rim = clamp(ang / moon_r, 0.0, 1.0);
  float disc = 1.0 - smoothstep(moon_r - px * 1.2, moon_r + px * 0.6, ang);
  float maria = Fbm(rel / moon_r * 1.6 + 7.0);
  float surface = (0.78 + 0.22 * smoothstep(0.35, 0.65, maria)) * (1.0 - 0.28 * rim * rim);
  // The moon down to the horizon (it rises out of the sea), dimmed a little where the day sky was cloud or the sun's
  // white glow (the sun is where the moon is: D169).
  float shown = smoothstep(-0.015, 0.03, d.y) * mix(0.75, 1.0, clear);
  c = mix(c, moonc.rgb * surface * shown, moonp.w > 0.0 ? disc : 0.0);
  c += mix(vec3(0.30, 0.38, 0.55), moonc.rgb, 0.5) * (exp(-ang / 0.035) * 0.03 + exp(-ang / 0.18) * 0.008) * shown;
  return c;
}

void main() {
  vec4 src = textureLod(colorTex, v_texcoord, 0.0);
  vec3 lin = pow(clamp(src.rgb, 0.0, 1.0), vec3(2.2));
  float luma = dot(lin, vec3(0.2126, 0.7152, 0.0722));
  // Moonlight: little colour, a cool blue, a fifth of the light.
  vec3 moon = mix(vec3(luma), lin, 0.35) * vec3(0.55, 0.72, 1.0) * (0.20 * night.x);
  // The night haze with distance (0.1 / depth), the day sky far away becoming the night sky.
  // The nearer of the view's depth at its end (on foot: the weapon alone, the rest cleared to far) and the world's from
  // before the weapon.
  float haze = 0.0, depth = 0.0, view_depth = 0.0, world_depth = 0.0;
  if (flags.x > 0.5 || flags.y > 0.5) {
    view_depth = flags.x > 0.5 ? textureLod(depthTex, v_texcoord * night.zw, 0.0).r : 0.0;
    world_depth = flags.y > 0.5 ? textureLod(worldDepthTex, v_texcoord * flags.zw, 0.0).r : 0.0;
    depth = max(view_depth, world_depth);
    float dist = 0.1 / max(depth, 1e-7);
    haze = 1.0 - exp(-dist / night.y);
  }
  vec3 sky = (vec3(0.004, 0.009, 0.024) + luma * 0.03 * vec3(0.6, 0.75, 1.0)) * night.x;
  vec3 result = mix(moon, sky, haze);
  // D169: the night sky where the view shows the sky (nothing within 5 km), where the day sky was clear blue (its
  // clouds, grey or white, keep covering it), fading toward the horizon. The direction is taken for every pixel (the
  // pixel's angle needs its neighbours').
  if (skyp.x > 0.0 && skyp.y > 0.5) {
    vec2 ndc = v_texcoord * skyndc.xy + skyndc.zw;
    vec3 ray = vec3(ndc, 1.0);
    vec3 dir = normalize(vec3(dot(sky0.xyz, ray), dot(sky1.xyz, ray), dot(sky2.xyz, ray)));
    float px = max(length(fwidth(dir)), 1e-5);
    float open = 1.0 - smoothstep(2e-6, 2e-5, depth);
    if (open > 0.0) {
      float hi = max(lin.r, max(lin.g, lin.b)), lo = min(lin.r, min(lin.g, lin.b));
      float clear = smoothstep(0.30, 0.60, hi > 1e-4 ? (hi - lo) / hi : 0.0);
      float above = smoothstep(-0.02, horizon.w, dir.y);
      float low = 1.0 - smoothstep(-0.04, horizon.w * 1.6, dir.y);
      // The level's haze: strongest low on the sky, a third of it overhead (damp air, dust).
      result += (NightSky(dir, px, mix(moonc.w, 1.0, clear) * above, clear) + horizon.rgb * mix(0.35, 1.0, low) * night.x) * open * skyp.x;
    }
    // Tests (night debug 3): red the sky test, green the clear sky test, blue the direction's height.
    if (counts.w > 2.5 && counts.w < 3.5) {
      float hi = max(lin.r, max(lin.g, lin.b)), lo = min(lin.r, min(lin.g, lin.b));
      result = vec3(open, smoothstep(0.30, 0.60, hi > 1e-4 ? (hi - lo) / hi : 0.0), dir.y * 0.5 + 0.5) * 0.5;
    }
  } else if (counts.w > 2.5 && counts.w < 3.5) {
    result = vec3(0.0, 0.0, 0.0);  // no camera: black
  }
  // D163: the lights (the lamps, and what the effects light), on the surface the pixel shows (its place in the world
  // from its depth, its facing from its neighbours'), lighting its colour without most of the day's sun (the bright
  // and the shaded brought closer).
  int nl = int(counts.x + 0.5);
  bool effect_lights = glow2.z > 0.5;
  if ((nl > 0 || effect_lights) && inv_t.w > 0.5 && depth > 2e-5) {
    vec3 P = WorldAt(v_texcoord, depth);
    vec3 N = cross(dFdx(P), dFdy(P));
    float nlen = length(N);
    N = nlen > 1e-8 ? N / nlen : vec3(0.0, 1.0, 0.0);
    if (dot(N, eye.xyz - P) < 0.0) N = -N;
    vec3 albedo = lin * (0.5 / (luma + 0.2));
    vec3 lit = vec3(0.0);
    for (int i = 0; i < nl; ++i) {
      vec4 l0 = lights[i * 3], l1 = lights[i * 3 + 1], l2 = lights[i * 3 + 2];
      vec3 L = l0.xyz - P;
      float d2 = dot(L, L), r2 = l0.w * l0.w;
      if (d2 >= r2) continue;
      float d = sqrt(d2);
      vec3 l = L / max(d, 1e-3);
      float fall = 1.0 - d2 / r2;
      // D170: a long light (a lighthouse's beam, over 60 m) falls off with its range, not as a lamp's 2 m.
      float k = l0.w > 60.0 ? 100.0 / r2 : 0.25;
      float att = fall * fall / (1.0 + d2 * k);
      float spot = dot(l2.xyz, l2.xyz) > 0.5 ? smoothstep(l1.w, l2.w, dot(-l, l2.xyz)) : 1.0;
      float facing = clamp(dot(N, l), 0.0, 1.0) * 0.8 + 0.2;
      lit += l1.rgb * (att * spot * facing);
    }
    // Up close a lamp's light rolls off (12 Nov 2026: right under a lamppost the ground looked like the day, its sun
    // shadows and all: run nadenight2); a pool of light seen from afar keeps its strength.
    vec3 lamps = lit * eye.w;
    lamps /= 1.0 + 1.5 * max(lamps.r, max(lamps.g, lamps.b));
    vec3 flashes = vec3(0.0);
    if (effect_lights) {
      for (int i = 0; i < 4; ++i) {
        vec4 l0 = texelFetch(effectLightsTex, ivec2(i, 0), 0), l1 = texelFetch(effectLightsTex, ivec2(i, 1), 0);
        if (l0.w <= 0.0) continue;
        vec3 L = l0.xyz - P;
        float d2 = dot(L, L), r2 = l0.w * l0.w;
        if (d2 >= r2) continue;
        float d = sqrt(d2);
        float fall = 1.0 - d2 / r2;
        float att = fall * fall / (1.0 + d2 * 0.6);
        float facing = clamp(dot(N, L / max(d, 1e-3)), 0.0, 1.0) * 0.75 + 0.25;
        flashes += l1.rgb * (att * facing);
      }
    }
    result += albedo * (lamps + flashes) * (1.0 - haze);
    if (counts.w > 0.5 && counts.w < 1.5) result = fract(P / 5.0) * 0.6;
    if (counts.w > 1.5) result = (lamps + flashes) * 0.25;
  }
  // D163: the lamps' bulbs, where nothing nearer hides them.
  int ns = int(counts.y + 0.5);
  for (int i = 0; i < ns; ++i) {
    vec4 s0 = sprites[i * 2], s1 = sprites[i * 2 + 1];
    vec2 q = (v_texcoord - s0.xy) / s0.zw;
    float r2 = dot(q, q);
    if (r2 >= 1.0 || depth > s1.w * 1.05) continue;
    float g = exp(-r2 * 40.0) * 3.0 + exp(-r2 * 8.0) * 0.35 + (1.0 - r2) * 0.04;
    result += s1.rgb * (g * eye.w);
  }
  // D163: the effects' own light (the game's half size layer of its emissive particles: muzzle flashes, tracers,
  // fire, explosions), as drawn and with a soft glow round it; its grey smoke and dust not. The first person weapon,
  // drawn after the effects and over them, keeps covering them (its pixels nearer than 3 m and than the world's depth).
  if (glowp.x > 0.5) {
    vec2 guv = v_texcoord * glowp.zw;
    vec3 e = max(textureLod(glowTex, guv, 0.0).rgb, vec3(0.0));
    vec3 halo = max(textureLod(glowTex, guv, 2.0).rgb * 0.3 + textureLod(glowTex, guv, 4.0).rgb * 0.2, vec3(0.0));
    float weapon = flags.x > 0.5 && flags.y > 0.5 && view_depth > 0.0333 && view_depth > world_depth * 1.02 ? 1.0 : 0.0;
    vec3 add = (e * Hot(e) * (1.0 - weapon) + halo * Hot(halo) * (1.0 - 0.7 * weapon)) * glowp.y;
    result += vec3(1.0) - exp(-add);
  }
  out_color = vec4(pow(max(result, vec3(0.0)), vec3(1.0 / 2.2)), src.a);
}
)";

  struct Constants {
    float night[4], flags[4], glow[4], to_ndc[4];
    float inverse[3][4];
    float inverse_t[4], eye[4], counts[4], glow2[4], sky[4];
    float sky_inverse[3][4], sky_to_ndc[4];  // D169
    float moon[4], moon_color[4], horizon[4], stars[4];
    float lights[kMaxLights * 3][4];
    float sprites[kMaxSprites * 2][4];
  };

  static Diligent::RefCntAutoPtr<Diligent::IShader> Shader(Diligent::IRenderDevice* device,
                                                           Diligent::SHADER_TYPE type, const char* source,
                                                           const char* name) {
    using namespace Diligent;
    RefCntAutoPtr<IShader> shader;
    std::vector<uint32_t> spirv =
        CompileGlsl(type == SHADER_TYPE_VERTEX ? GlslStage::kVertex : GlslStage::kPixel, source, name);
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

  Pass* PassFor(Diligent::IRenderDevice* device, Diligent::TEXTURE_FORMAT format) {
    using namespace Diligent;
    auto it = passes_.find(format);
    if (it != passes_.end()) return it->second.pso ? &it->second : nullptr;
    Pass& pass = passes_[format];
    if (!constants_) {
      BufferDesc buffer;
      buffer.Name = "NativeXRenderer night constants";
      buffer.Size = sizeof(Constants);
      buffer.Usage = USAGE_DYNAMIC;
      buffer.BindFlags = BIND_UNIFORM_BUFFER;
      buffer.CPUAccessFlags = CPU_ACCESS_WRITE;
      device->CreateBuffer(buffer, nullptr, &constants_);
      if (!constants_) return nullptr;
    }
    RefCntAutoPtr<IShader> vs = Shader(device, SHADER_TYPE_VERTEX, kVertex, "NativeXRenderer night VS");
    RefCntAutoPtr<IShader> ps =
        Shader(device, SHADER_TYPE_PIXEL, (std::string(kCommon) + kPixel).c_str(), "NativeXRenderer night PS");
    if (!vs || !ps) {
      REXLOG_ERROR("[native_x_renderer] night: its shaders did not compile: the scene is drawn as day");
      return nullptr;
    }
    GraphicsPipelineStateCreateInfo info;
    info.PSODesc.Name = "NativeXRenderer night";
    info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
    info.GraphicsPipeline.NumRenderTargets = 1;
    info.GraphicsPipeline.RTVFormats[0] = format;
    info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
    info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
    info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
    info.pVS = vs;
    info.pPS = ps;
    SamplerDesc point;
    point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
    point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
    SamplerDesc linear;  // the effects' layer: half size, and its mips for the glow
    linear.MinFilter = linear.MagFilter = linear.MipFilter = FILTER_TYPE_LINEAR;
    linear.AddressU = linear.AddressV = linear.AddressW = TEXTURE_ADDRESS_CLAMP;
    ShaderResourceVariableDesc variables[] = {
        {SHADER_TYPE_PIXEL, "colorTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_PIXEL, "depthTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_PIXEL, "worldDepthTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_PIXEL, "glowTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_PIXEL, "effectLightsTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC}};
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "colorTex", point},
                                       {SHADER_TYPE_PIXEL, "depthTex", point},
                                       {SHADER_TYPE_PIXEL, "worldDepthTex", point},
                                       {SHADER_TYPE_PIXEL, "glowTex", linear},
                                       {SHADER_TYPE_PIXEL, "effectLightsTex", point}};
    info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    info.PSODesc.ResourceLayout.Variables = variables;
    info.PSODesc.ResourceLayout.NumVariables = 5;
    info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    info.PSODesc.ResourceLayout.NumImmutableSamplers = 5;
    device->CreateGraphicsPipelineState(info, &pass.pso);
    if (!pass.pso) {
      REXLOG_ERROR("[native_x_renderer] night: no pipeline for format {}", int(format));
      return nullptr;
    }
    if (auto* variable = pass.pso->GetStaticVariableByName(SHADER_TYPE_PIXEL, "NightConstants")) variable->Set(constants_);
    pass.pso->CreateShaderResourceBinding(&pass.srb, true);
    return pass.srb ? &pass : nullptr;
  }

  // D163: the effects' lights' pass and its 4 x 2 texture (RGBA32F), made once; false if that failed.
  bool LightsPass(Diligent::IRenderDevice* device) {
    using namespace Diligent;
    if (lights_tried_) return lights_pass_.srb != nullptr;
    lights_tried_ = true;
    if (!constants_) return false;
    TextureDesc t;
    t.Name = "NativeXRenderer night effects' lights";
    t.Type = RESOURCE_DIM_TEX_2D;
    t.Width = 4;
    t.Height = 2;
    t.Format = TEX_FORMAT_RGBA32_FLOAT;
    t.Usage = USAGE_DEFAULT;
    t.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
    device->CreateTexture(t, nullptr, &effect_lights_);
    RefCntAutoPtr<IShader> vs = Shader(device, SHADER_TYPE_VERTEX, kVertex, "NativeXRenderer night lights VS");
    RefCntAutoPtr<IShader> ps = Shader(device, SHADER_TYPE_PIXEL, (std::string(kCommon) + kPixelLights).c_str(),
                                       "NativeXRenderer night lights PS");
    if (!effect_lights_ || !vs || !ps) {
      REXLOG_ERROR("[native_x_renderer] night: the effects' lights' pass could not be made: no light from the effects");
      return false;
    }
    GraphicsPipelineStateCreateInfo info;
    info.PSODesc.Name = "NativeXRenderer night effects' lights";
    info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
    info.GraphicsPipeline.NumRenderTargets = 1;
    info.GraphicsPipeline.RTVFormats[0] = TEX_FORMAT_RGBA32_FLOAT;
    info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
    info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
    info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
    info.pVS = vs;
    info.pPS = ps;
    SamplerDesc point;
    point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
    point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
    ShaderResourceVariableDesc variables[] = {
        {SHADER_TYPE_PIXEL, "colorTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_PIXEL, "depthTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_PIXEL, "worldDepthTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_PIXEL, "glowTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_PIXEL, "effectLightsTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC}};
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "colorTex", point},
                                       {SHADER_TYPE_PIXEL, "depthTex", point},
                                       {SHADER_TYPE_PIXEL, "worldDepthTex", point},
                                       {SHADER_TYPE_PIXEL, "glowTex", point},
                                       {SHADER_TYPE_PIXEL, "effectLightsTex", point}};
    info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    info.PSODesc.ResourceLayout.Variables = variables;
    info.PSODesc.ResourceLayout.NumVariables = 5;
    info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    info.PSODesc.ResourceLayout.NumImmutableSamplers = 5;
    device->CreateGraphicsPipelineState(info, &lights_pass_.pso);
    if (!lights_pass_.pso) {
      REXLOG_ERROR("[native_x_renderer] night: no pipeline for the effects' lights");
      return false;
    }
    if (auto* variable = lights_pass_.pso->GetStaticVariableByName(SHADER_TYPE_PIXEL, "NightConstants"))
      variable->Set(constants_);
    lights_pass_.pso->CreateShaderResourceBinding(&lights_pass_.srb, true);
    return lights_pass_.srb != nullptr;
  }

  bool EnsureCopy(Diligent::IRenderDevice* device, const Diligent::TextureDesc& desc) {
    using namespace Diligent;
    if (copy_ && copy_->GetDesc().Width == desc.Width && copy_->GetDesc().Height == desc.Height &&
        copy_->GetDesc().Format == desc.Format) {
      return true;
    }
    copy_.Release();
    TextureDesc c;
    c.Name = "NativeXRenderer night input";
    c.Type = RESOURCE_DIM_TEX_2D;
    c.Width = desc.Width;
    c.Height = desc.Height;
    c.Format = desc.Format;
    c.Usage = USAGE_DEFAULT;
    c.BindFlags = BIND_SHADER_RESOURCE;
    device->CreateTexture(c, nullptr, &copy_);
    return copy_ != nullptr;
  }

  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
  std::map<Diligent::TEXTURE_FORMAT, Pass> passes_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> copy_;
  Pass lights_pass_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> effect_lights_;
  bool lights_tried_ = false;
  uint64_t applied_effect_lights_ = 0;
  uint64_t applied_ = 0, applied_world_ = 0, applied_glow_ = 0, lights_ = 0, sprites_ = 0;
  bool logged_ = false, logged_world_ = false, logged_glow_ = false, logged_lights_ = false;
};

}  // namespace native_x_renderer
