// NativeXRenderer - night (internal): the night's lights on the finished scene, before the HUD.
//
// Battlefield 1943 Reborn: the day and night cycle (sky.h) darkens the game's own light at night; this pass adds the
// lights that come on with the dark (D163, D170, D175), on the game's composite (the first draw of the output's size
// after the profile's scene copy, Profile::scene_resolve_caller: the picture with the game's bloom and exposure), so
// the HUD drawn after it keeps its colours:
//   - the effects' own light (the game's half size layer of emissive particles) and what they light;
//   - the level's lamps and the models' lamps, from the camera kept from the terrain's draws.
// The scene itself stays as the cycle drew it. (The separate night look this pass began as, D162 and D169: a moonlit
// grade, a haze and a night sky over the game's day, went on 16 Nov 2026 with the night maps.)
// Header only: renderer.cpp alone includes it. Runs while the cycle is dark enough for the lights.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
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
  float glow = 1.0f;        // D163: the effects' own light (muzzle flashes, tracers, fire, explosions); 0 none
  float lamps = 1.0f;       // D163: the lamps' light and glow; 0 none
  // D177: how much darker than the game's day the cycle has made the scene (1 by day): a surface's own colour for the
  // lamps' light is the picture times this, or a lamp at midnight would light a nearly black ground.
  float scene_gain = 1.0f;
  // D183: a lamp's bloom round its lit glass, and the camera's lens flare from the brightest lamps in view (0 off, 1 on;
  // Settings > Enhancements).
  float bloom = 1.0f;
  float flare = 1.0f;
  bool seen_pass = true;  // each lamp's visibility found once a frame (false: per pixel, for comparisons)
  bool compute_lights = true;  // the effects' lights found by the compute search (false: the pixel pass, for comparisons)
  bool tiles = true;  // each pixel loops over its screen tile's lights (false: over all of them, for comparisons)
  bool timing = false;  // the night pass's GPU time (native_x_renderer_gpu_sections)
  int debug = 0;            // D163, tests: 1 each pixel's place in the world, 2 the lights alone
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

// D163: a lamp's bulb on screen. D183: its glass lit where the lamp's own mesh is (no glow in the air), a bloom round it
// and, for the brightest few, the lens flare, while nothing nearer hides its middle.
struct NightSprite {
  float uv[2] = {};      // the target's texture coordinates
  float radius[2] = {};  // in those coordinates, across and down: the bloom's reach
  float depth = 0.0f;    // the bulb's stored depth (0.1 / distance): nearer pixels hide it
  float color[3] = {};   // linear, times its strength
  float world[3] = {};   // D183: the bulb in the world
  float bulb = 0.1f;     // D183: its radius, metres: the surface within it glows
  float flare = 0.0f;    // D183: its lens flare's strength, 0 to 1 (the first sprites; 0 none)
};

// What one night pass reads besides the picture.
struct NightFrame {
  // The main view's depth at its end and the world's from before the first person weapon, each with the share
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
  const NightLight* lights = nullptr;
  uint32_t light_count = 0;
  const NightSprite* sprites = nullptr;
  uint32_t sprite_count = 0;
};

class Night {
 public:
  static constexpr uint32_t kMaxLights = 96;  // D195: the lamps in view across the map (was 24)
  static constexpr uint32_t kMaxSprites = 64;  // D195: across the map (was 24)
  static constexpr uint32_t kTilesX = 16, kTilesY = 9;  // the lights' screen tiles (TileLights)

  // `texture`, the finished scene, lit in place (drawn into `target`, its render target view). `f.depth`: the main
  // view's depth (its shader view) and the share of it the view covers. `f.world`: the world's depth taken before the
  // first person weapon's draws clear it (an R32 float copy, its red channel; null without one). Per pixel the nearer
  // of the two is taken: on foot the view's depth at its end holds only the weapon, and the world's copy has no
  // weapon yet.
  // D163: the lights (lamps and what the effects light), the lamps' glows and the effects' own light, from `f`.
  void Apply(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, Diligent::ITexture* texture,
             Diligent::ITextureView* target, const NightFrame& f, const NightSettings& s) {
    using namespace Diligent;
    if (!device || !context || !texture || !target) return;
    const TextureDesc& desc = texture->GetDesc();
    if (!EnsureCopy(device, desc)) return;
    const uint32_t lights = f.camera && s.lamps > 0.0f ? std::min(f.light_count, kMaxLights) : 0;
    const uint32_t sprites = s.lamps > 0.0f ? std::min(f.sprite_count, kMaxSprites) : 0;
    const bool glow = f.glow && s.glow > 0.0f;
    // The lamps' visibility found once each (SeenPass) when a glow or a flare wants it.
    const bool seen_pass = s.seen_pass && sprites > 0 && (s.bloom > 0.0f || s.flare > 0.0f) && SeenPass(device);
    bool check_tiles = false;
    Pass* pass = PassFor(device, desc.Format, seen_pass);
    if (!pass) return;
    if (s.timing) timer_.Begin(device, context);
    {
      Constants k = {};
      k.night[0] = std::clamp(s.scene_gain, 1.0f, 64.0f);
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
      k.night[1] = std::clamp(s.bloom, 0.0f, 2.0f);  // D183
      k.extra[0] = seen_pass ? 1.0f : 0.0f;
      k.glow2[3] = std::clamp(s.flare, 0.0f, 2.0f);   // D183
      // The effects' lights: found in the effects' layer's mip about 48 texels wide.
      const bool effect_lights = glow && f.camera;
      if (effect_lights && device && LightsPass(device)) {
        const float width = float(f.glow->GetTexture()->GetDesc().Width);
        k.glow2[0] = std::max(0.0f, std::round(std::log2(std::max(width, 1.0f) / 48.0f)));
        k.glow2[1] = 1.1f * s.glow;
        k.glow2[2] = 1.0f;
      }
      TileLights(f, lights, s.tiles, k);
      for (uint32_t i = 0; i < lights; ++i) {
        const NightLight& l = f.lights[i];
        float* a = k.lights[i * 3];
        a[0] = l.position[0], a[1] = l.position[1], a[2] = l.position[2], a[3] = l.range;
        a[4] = l.color[0], a[5] = l.color[1], a[6] = l.color[2], a[7] = l.cos_outer;
        a[8] = l.direction[0], a[9] = l.direction[1], a[10] = l.direction[2], a[11] = l.cos_inner;
      }
      for (uint32_t i = 0; i < sprites; ++i) {
        const NightSprite& p = f.sprites[i];
        float* a = k.sprites[i * 3];
        a[0] = p.uv[0], a[1] = p.uv[1], a[2] = p.radius[0], a[3] = p.radius[1];
        a[4] = p.color[0], a[5] = p.color[1], a[6] = p.color[2], a[7] = p.depth;
        a[8] = p.world[0], a[9] = p.world[1], a[10] = p.world[2], a[11] = 2.0f * std::round(255.0f * std::clamp(p.flare, 0.0f, 1.0f)) + std::clamp(p.bulb, 0.0f, 1.9f);
      }
      void* data = nullptr;
      context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
      std::memcpy(data, &k, sizeof(k));
      context->UnmapBuffer(constants_, MAP_WRITE);
      // BF_NIGHT_TILES_CHECK=1 (tests): every 60th pass with lights is drawn a second time with every light in every
      // tile, and the two pictures compared (TilesCheck).
      static const bool tiles_check = [] {
        const char* v = std::getenv("BF_NIGHT_TILES_CHECK");
        return v && *v == '1';
      }();
      check_tiles = tiles_check && s.tiles && lights > 0 && f.camera && (tiles_check_count_++ % 60) == 0;
      if (check_tiles) {
        untiled_ = std::make_unique<Constants>(k);
        std::memset(untiled_->tiles, 0, sizeof(untiled_->tiles));
        for (uint32_t t = 0; t < kTilesX * kTilesY; ++t)
          for (uint32_t i = 0; i < lights; ++i) untiled_->tiles[t][i / 32] |= 1u << (i % 32);
      }
    }
    // D163: the effects' lights into their 4 x 2 texture, first: by the compute search, or the pixel pass without it.
    const bool effect_lights = glow && f.camera && LightsPass(device);
    const bool compute_lights = effect_lights && s.compute_lights && ComputeLights(device);
    // BF_NIGHT_FX_CHECK=1 (tests): every 10th search, the pixel pass's result first, kept, then the compute search's,
    // both read back and compared (the largest difference logged): the two must pick the same lights.
    static const bool fx_check = [] {
      const char* v = std::getenv("BF_NIGHT_FX_CHECK");
      return v && *v == '1';
    }();
    const bool check = compute_lights && fx_check && (check_count_++ % 10) == 0 && Staging(device);
    if (check) {
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
      DrawAttribs d;
      d.NumVertices = 3;
      context->Draw(d);
      context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
      CopyTextureAttribs keep{effect_lights_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, staging_[0],
                              RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
      context->CopyTexture(keep);
    }
    if (compute_lights) {
      context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
      auto set = [&](const char* name, IDeviceObject* object) {
        if (auto* v = compute_lights_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, name)) v->Set(object);
      };
      ITextureView* any = f.glow;
      set("glowTex", f.glow);
      set("depthTex", f.depth ? f.depth : any);
      set("worldDepthTex", f.world ? f.world : any);
      set("colorTex", any);
      set("effectLightsTex", any);
      set("effectLightsOut", effect_lights_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
      context->SetPipelineState(compute_lights_.pso);
      context->CommitShaderResources(compute_lights_.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      if (s.timing) lights_timer_.Begin(device, context);
      context->DispatchCompute(DispatchComputeAttribs(1, 1, 1));
      lights_timer_.End(context);
      ++applied_effect_lights_, ++applied_compute_lights_;
      if (check) {
        CopyTextureAttribs keep{effect_lights_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, staging_[1],
                                RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
        context->CopyTexture(keep);
        context->WaitForIdle();
        float v[2][8][4] = {};
        for (int i = 0; i < 2; ++i) {
          MappedTextureSubresource m;
          context->MapTextureSubresource(staging_[i], 0, 0, MAP_READ, MAP_FLAG_NONE, nullptr, m);
          if (!m.pData) continue;
          for (int y = 0; y < 2; ++y)
            std::memcpy(v[i][y * 4], static_cast<const uint8_t*>(m.pData) + size_t(y) * m.Stride, 4 * 16);
          context->UnmapTextureSubresource(staging_[i], 0, 0);
        }
        float most = 0.0f;
        int lit = 0;
        for (int t = 0; t < 8; ++t) {
          for (int c = 0; c < 4; ++c) most = std::max(most, std::fabs(v[0][t][c] - v[1][t][c]));
          if (t < 4 && v[1][t][3] > 0.0f) ++lit;
        }
        REXLOG_INFO("[native_x_renderer] night: effects' lights check: {} lights; the compute search and the pixel pass "
                    "differ by {:.6f} at most (light 0 at {:.2f} {:.2f} {:.2f} range {:.2f} / {:.2f} {:.2f} {:.2f} range {:.2f})",
                    lit, most, v[1][0][0], v[1][0][1], v[1][0][2], v[1][0][3], v[0][0][0], v[0][0][1], v[0][0][2],
                    v[0][0][3]);
      }
    } else if (effect_lights) {
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
      if (s.timing) lights_timer_.Begin(device, context);
      context->Draw(lights_draw);
      lights_timer_.End(context);
      context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
      ++applied_effect_lights_;
    }
    // The lamps' visibility, into its kMaxSprites x 1 texture (only the first `sprites` texels are read).
    if (seen_pass) {
      context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
      auto set = [&](const char* name, ITextureView* view) {
        if (auto* v = seen_pass_.srb->GetVariableByName(SHADER_TYPE_PIXEL, name)) v->Set(view);
      };
      ITextureView* any = f.depth ? f.depth : f.world ? f.world : copy_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
      set("depthTex", f.depth ? f.depth : any);
      set("worldDepthTex", f.world ? f.world : any);
      set("colorTex", any);
      set("glowTex", any);
      set("effectLightsTex", any);
      ITextureView* rtv = sprite_seen_->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
      context->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      Viewport row;
      row.Width = float(sprites);
      row.Height = 1.0f;
      row.MaxDepth = 1.0f;
      context->SetViewports(1, &row, 0, 0);
      context->SetPipelineState(seen_pass_.pso);
      context->CommitShaderResources(seen_pass_.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      DrawAttribs seen_draw;
      seen_draw.NumVertices = 3;
      context->Draw(seen_draw);
      context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
      ++applied_seen_;
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
    if (auto* v = pass->srb->GetVariableByName(SHADER_TYPE_PIXEL, "spriteSeenTex"))
      v->Set(seen_pass ? sprite_seen_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : color);
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
    timer_.End(context);
    if (check_tiles) TilesCheck(device, context, texture, target, pass, viewport);
    ++applied_;
    if (f.world) ++applied_world_;
    if (glow) ++applied_glow_;
    lights_ += lights, sprites_ += sprites;
    if (!logged_) {
      logged_ = true;
      REXLOG_INFO("[native_x_renderer] night: its lights on the finished scene, {}x{} format {}, {}{}", desc.Width,
                  desc.Height, int(desc.Format), f.depth ? "with the view's depth" : "without the view's depth",
                  f.world ? ", and the world's from before the weapon" : "");
    }
    if (f.world && !logged_world_) {
      logged_world_ = true;
      REXLOG_INFO("[native_x_renderer] night: the world's depth from before the first person weapon in use");
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
  // The night pass's GPU time since the last call (its own passes, the scene's copy and the lights' and lamps'
  // visibility included): the average and the most of one frame, over `frames` timed; and the visibility passes drawn.
  void TakeGpu(double& average_ms, double& max_ms, uint64_t& frames, uint64_t& seen, double& lights_ms,
              uint64_t& compute_lights) {
    timer_.Take(average_ms, max_ms, frames);
    double lights_max = 0.0;
    uint64_t lights_frames = 0;
    lights_timer_.Take(lights_ms, lights_max, lights_frames);
    seen = applied_seen_, applied_seen_ = 0;
    compute_lights = applied_compute_lights_, applied_compute_lights_ = 0;
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
  vec4 night;   // x: the scene's gain back to its day brightness (D177), y: the lamps' bloom (D183), z, w: the view's
                // share of the depth texture
  vec4 flags;   // x: 1 with the view's depth, y: 1 with the world's (from before the first person weapon), z, w: its share
  vec4 glowp;   // D163: x: 1 with the effects' layer, y: its strength, z, w: its share
  vec4 to_ndc;  // D163: the night target's texture coordinate to NDC (uv * xy + zw)
  vec4 inv0;    // D163: the world from clip x, y and w (rows)
  vec4 inv1;
  vec4 inv2;
  vec4 inv_t;   // xyz: clip x, y and w at the world's origin; w: 1 with the camera
  vec4 eye;     // xyz: the camera; w: the lamps' strength
  vec4 counts;  // x: lights, y: glows, z: the target's aspect, w: tests (1 the world's place, 2 the lights alone)
  vec4 glow2;   // x: the coarse mip the effects' lights are found in, y: their strength, z: 1 with them, w: the lens
                // flare (D183)
  vec4 extra;   // x: 1 with each lamp's visibility found once a frame (spriteSeenTex: LampSeen, GlassAt)
  uvec4 beams;  // the lights that are turning beams: bits 0 to 95 in x, y, z (TileLights)
  uvec4 tiles[144];  // 16 x 9 screen tiles: each its lights, bits 0 to 95 in x, y, z (TileLights)
  vec4 lights[288];  // 96 x (position, range), (colour, cos outer), (direction, cos inner)
  vec4 sprites[192];  // 64 x (uv, radius), (colour, the bulb's depth), (the bulb in the world, its radius + 2 x 255 x flare)
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
// D183: how much of a lamp's middle is in view (0 to 1): the scene's distance at three points across it against the
// bulb's, with the lamp's own housing (within `tol` metres in front of the bulb) not hiding it.
float LampSeen(vec2 c, vec2 r, float bulb_depth, float tol) {
  float lamp = 0.1 / max(bulb_depth, 1e-7);
  float seen = 0.0;
  for (int k = -1; k <= 1; ++k) {
    float d = DepthAt(clamp(c + vec2(float(k) * 0.12 * r.x, 0.0), vec2(0.0), vec2(1.0)));
    float dist = d > 1e-7 ? 0.1 / d : 1e9;
    seen += smoothstep(lamp - tol - 0.6, lamp - tol, dist);
  }
  return seen / 3.0;
}
// D195: how much of a lamp's own glass is at its middle (0 to 1): the surface seen at five points across the bulb against
// its place in the world, as the glass is lit. Its bloom and its flare only where that is, so nothing glows in the air
// where a lamp's point is off its mesh (the user, 18 Nov 2026: "I thought we got rid of these floating orbs"). 1 with
// no camera to place it by.
float GlassAt(vec2 c, vec2 r, vec3 bulb_at, float bulb) {
  if (inv_t.w < 0.5) return 1.0;
  float most = 0.0;
  for (int k = 0; k < 5; ++k) {
    vec2 o = k == 0 ? vec2(0.0) : vec2(k == 1 ? 1.0 : k == 2 ? -1.0 : 0.0, k == 3 ? 1.0 : k == 4 ? -1.0 : 0.0) * 0.3 * r;
    vec2 p = clamp(c + o, vec2(0.0), vec2(1.0));
    float d = DepthAt(p);
    if (d <= 2e-5) continue;
    most = max(most, 1.0 - smoothstep(bulb * 0.45, bulb * 1.5, length(WorldAt(p, d) - bulb_at)));
  }
  return most;
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

  // D163's search of the effects' layer as a compute shader (the Night Ops performance pass, 20 Nov 2026: the pixel
  // pass above ran on 8 threads, each scanning the whole coarse mip up to four times one texel after another, 1.24 ms
  // of the GPU's frame whenever the effects were on screen). Here 256 threads share each of the four scans and keep
  // the best of their texels; ties go to the first in the pixel pass's order (rows, then columns), so the same four
  // places are picked. Then threads 0 to 3 place their light as the pixel pass did, into the same 4 x 2 texture.
  static constexpr const char* kComputeLights = R"(
layout(local_size_x = 256) in;
layout(set = 0, binding = 8, rgba32f) uniform writeonly image2D effectLightsOut;
shared float s_best[256];
shared int s_at[256];
shared ivec2 s_picked[4];
shared float s_energy[4];
void main() {
  int me = int(gl_LocalInvocationIndex);
  if (glowp.x < 0.5 || inv_t.w < 0.5) {
    if (me < 8) imageStore(effectLightsOut, ivec2(me % 4, me / 4), vec4(0.0));
    return;
  }
  int lod = int(glow2.x + 0.5);
  ivec2 size = textureSize(glowTex, lod);
  int count = size.x * size.y;
  for (int k = 0; k < 4; ++k) {
    float best = 0.0;
    int at = 0x7fffffff;
    for (int i = me; i < count; i += 256) {
      int x = i % size.x, y = i / size.x;
      bool taken = false;
      for (int j = 0; j < k; ++j) {
        if (max(abs(x - s_picked[j].x), abs(y - s_picked[j].y)) <= 4) taken = true;
      }
      if (taken) continue;
      vec3 e = max(texelFetch(glowTex, ivec2(x, y), lod).rgb, vec3(0.0));
      float w = dot(e, vec3(0.2126, 0.7152, 0.0722)) * Hot(e);
      if (w > best) best = w, at = i;  // within a thread its texels come in order: the first of a tie stays
    }
    s_best[me] = best;
    s_at[me] = at;
    barrier();
    for (int stride = 128; stride > 0; stride >>= 1) {
      if (me < stride) {
        float b = s_best[me + stride];
        int a = s_at[me + stride];
        if (b > s_best[me] || (b == s_best[me] && a < s_at[me])) s_best[me] = b, s_at[me] = a;
      }
      barrier();
    }
    if (me == 0) {
      // As the pixel pass: no texel above 0 leaves the pick at (-1, -1), which still keeps the next picks away.
      s_picked[k] = s_best[0] > 0.0 ? ivec2(s_at[0] % size.x, s_at[0] / size.x) : ivec2(-1);
      s_energy[k] = s_best[0];
    }
    barrier();
  }
  if (me >= 4) return;
  ivec2 at = s_picked[me];
  float energy = s_energy[me];
  vec4 row0 = vec4(0.0), row1 = vec4(0.0);
  if (at.x >= 0 && energy >= 0.004) {
    vec2 euv = (vec2(at) + 0.5) / vec2(size);
    vec2 cell = 1.0 / vec2(size);
    float depth = 0.0;
    for (int j = -2; j <= 2; ++j) {
      for (int i = -2; i <= 2; ++i) depth = max(depth, DepthAt((euv + vec2(i, j) * cell * 0.75) / glowp.zw));
    }
    if (depth >= 2e-5) {
      vec3 P = WorldAt(euv / glowp.zw, depth);
      vec3 toward = eye.xyz - P;
      P += toward * min(0.35, length(toward) * 0.3) / max(length(toward), 1e-3);
      float power = min(energy * exp2(2.0 * float(lod)) / (float(textureSize(glowTex, 0).x) * 0.25), 2.5);
      vec3 colour = max(texelFetch(glowTex, at, lod).rgb, vec3(0.0));
      colour /= max(max(colour.r, max(colour.g, colour.b)), 1e-4);
      float range = clamp(3.5 + sqrt(power) * 5.0, 3.5, 16.0);
      row0 = vec4(P, range);
      row1 = vec4(colour * power * glow2.y, 0.0);
    }
  }
  imageStore(effectLightsOut, ivec2(me, 0), row0);
  imageStore(effectLightsOut, ivec2(me, 1), row1);
}
)";

  // The lamps' visibility, once a frame: texel i of a kMaxSprites x 1 target is lamp i's LampSeen (x) and GlassAt
  // (y). The main pass used to find both for every pixel in a lamp's bloom, and for the lens flare's four lamps for
  // every pixel of the picture (16 depth reads a lamp each), though neither depends on the pixel.
  static constexpr const char* kPixelSeen = R"(
layout(location = 0) out vec4 out_seen;
void main() {
  int i = int(gl_FragCoord.x);
  out_seen = vec4(0.0);
  if (i >= int(counts.y + 0.5)) return;
  vec4 s0 = sprites[i * 3], s1 = sprites[i * 3 + 1], s2 = sprites[i * 3 + 2];
  float bulb = s2.w - 2.0 * floor(s2.w * 0.5);
  out_seen = vec4(LampSeen(s0.xy, s0.zw, s1.w, max(3.0 * bulb, 0.3)), GlassAt(s0.xy, s0.zw, s2.xyz, bulb), 0.0, 0.0);
}
)";

  static constexpr const char* kPixel = R"(
layout(set = 0, binding = 6) uniform sampler2D spriteSeenTex;
layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 out_color;

// A lamp's LampSeen (x) and GlassAt (y): found once a frame by the visibility pass (they depend on the lamp alone),
// else here.
// (Two shaders, one for each way, rather than a branch: each compiles only the code it runs.)
vec2 SpriteSeen(int i, vec4 s0, vec4 s1, vec4 s2, float bulb) {
#ifdef NIGHT_SEEN_PASS
  return texelFetch(spriteSeenTex, ivec2(i, 0), 0).xy;
#else
  return vec2(LampSeen(s0.xy, s0.zw, s1.w, max(3.0 * bulb, 0.3)), GlassAt(s0.xy, s0.zw, s2.xyz, bulb));
#endif
}

void main() {
  vec4 src = textureLod(colorTex, v_texcoord, 0.0);
  vec3 lin = pow(clamp(src.rgb, 0.0, 1.0), vec3(2.2));
  float luma = dot(lin, vec3(0.2126, 0.7152, 0.0722));
  // The nearer of the view's depth at its end (on foot: the weapon alone, the rest cleared to far) and the world's from
  // before the weapon.
  float depth = 0.0, view_depth = 0.0, world_depth = 0.0;
  if (flags.x > 0.5 || flags.y > 0.5) {
    view_depth = flags.x > 0.5 ? textureLod(depthTex, v_texcoord * night.zw, 0.0).r : 0.0;
    world_depth = flags.y > 0.5 ? textureLod(worldDepthTex, v_texcoord * flags.zw, 0.0).r : 0.0;
    depth = max(view_depth, world_depth);
  }
  // The scene stays as the day and night cycle drew it: only the lights below are added.
  vec3 result = lin;
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
    // The first person arms and weapon (nearer than 3 m and than the world's depth): lit evenly, not by the facing
    // from the depth's neighbours, which is each flat triangle's own on their few large ones: near a lamp the arms
    // showed in patches of light and dark ("What are those patchy shadows on the player arms?", 17 Nov 2026).
    float held = flags.x > 0.5 && flags.y > 0.5 && view_depth > 0.0333 && view_depth > world_depth * 1.02 ? 1.0 : 0.0;
    // D177: the surface's own colour from the picture as the day would show it (the cycle's dark undone), the day's
    // bright and shaded brought closer.
    vec3 day = lin * night.x;
    vec3 albedo = min(day * (0.5 / (luma * night.x + 0.2)), vec3(1.0));
    vec3 lit = vec3(0.0);
    // Only the lights whose reach can touch this pixel's screen tile (TileLights), in the same order as all of them.
    ivec2 tile = clamp(ivec2(v_texcoord * vec2(16.0, 9.0)), ivec2(0), ivec2(15, 8));
    uvec4 mine = tiles[tile.y * 16 + tile.x];
    for (int word = 0; word < 3; ++word) {
     uint bits = mine[word];
     while (bits != 0u) {
      int i = word * 32 + findLSB(bits);
      bits &= bits - 1u;
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
      float facing = mix(clamp(dot(N, l), 0.0, 1.0) * 0.8 + 0.2, 0.6, held);
      lit += l1.rgb * (att * spot * facing);
     }
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
        float facing = mix(clamp(dot(N, L / max(d, 1e-3)), 0.0, 1.0) * 0.75 + 0.25, 0.6, held);
        flashes += l1.rgb * (att * facing);
      }
    }
    result += albedo * (lamps + flashes);
    if (counts.w > 0.5 && counts.w < 1.5) result = fract(P / 5.0) * 0.6;
    if (counts.w > 1.5) result = (lamps + flashes) * 0.25;
  }
  // D195: a lighthouse's beams seen in the air (the user, 18 Nov 2026: "The lighthouses should also have a realistic
  // rotating long light"): each narrow, far reaching light (a cone under 10 degrees, 150 m or more: only the
  // lighthouse's turning beams) is a shaft of light 1.6 km long, widening with its cone, fading along it, seen where
  // the view's ray passes through it before what the pixel shows. From the ray's nearest approach to the beam's axis.
  if (nl > 0 && inv_t.w > 0.5) {
    vec3 E = eye.xyz;
    bool sky_px = depth <= 2e-5;
    vec3 Pv = WorldAt(v_texcoord, sky_px ? 1e-5 : depth);
    vec3 rv = Pv - E;
    float Lv = sky_px ? 1e6 : length(rv);
    vec3 rdv = rv / max(length(rv), 1e-3);
    vec3 shaft = vec3(0.0);
    for (int word = 0; word < 3; ++word) {
     uint bits = beams[word];
     while (bits != 0u) {
      int i = word * 32 + findLSB(bits);
      bits &= bits - 1u;
      vec4 l0 = lights[i * 3], l1 = lights[i * 3 + 1], l2 = lights[i * 3 + 2];
      if (l1.w < 0.985 || l0.w < 150.0 || dot(l2.xyz, l2.xyz) < 0.25) continue;
      vec3 u = normalize(l2.xyz);
      const float R = 1600.0;
      vec3 w0 = E - l0.xyz;
      float b = dot(rdv, u), dd = dot(rdv, w0), ee = dot(u, w0);
      float den = max(1.0 - b * b, 1e-4);
      float sa = clamp((ee - b * dd) / den, 0.0, R);
      float ta = clamp(dot(l0.xyz + u * sa - E, rdv), 0.0, Lv);
      sa = clamp(dot(E + rdv * ta - l0.xyz, u), 0.0, R);
      float tan_outer = sqrt(max(1.0 - l1.w * l1.w, 0.0)) / l1.w;
      float rad = max(0.6, sa * tan_outer);
      float dist = length(E + rdv * ta - (l0.xyz + u * sa));
      float core = exp(-2.0 * (dist / rad) * (dist / rad));
      float chord = min(2.0 * rad / sqrt(den), 300.0);
      float fall = exp(-sa / 650.0) * smoothstep(0.0, 4.0, sa);
      shaft += l1.rgb * core * chord * fall * 0.0005;
     }
    }
    result += shaft * min(eye.w, 1.0);
  }
  // D163, D183 (the user, 17 Nov 2026: "illuminate without that floating light ball ... color the mesh the light is
  // coming from when lit and give it a bloom with a clean and subtle camera lens flare"): each lamp lights its own
  // glass, the surface within the bulb's radius of it (where no mesh is, nothing glows), with a soft bloom round it
  // while its middle is in view.
  int ns = int(counts.y + 0.5);
  bool placed = inv_t.w > 0.5 && depth > 2e-5;
  vec3 Pw = placed ? WorldAt(v_texcoord, depth) : vec3(0.0);
  float aspect = counts.z;
  for (int i = 0; i < ns; ++i) {
    vec4 s0 = sprites[i * 3], s1 = sprites[i * 3 + 1], s2 = sprites[i * 3 + 2];
    vec2 q = (v_texcoord - s0.xy) / s0.zw;
    float r2 = dot(q, q);
    if (r2 >= 1.0) continue;
    float bulb = s2.w - 2.0 * floor(s2.w * 0.5);
    // The lamp's own colour, a touch whiter, its brightest channel held near 1.3 so the glass keeps its colour
    // instead of burning white.
    float peak = max(s1.r, max(s1.g, s1.b));
    vec3 hue = mix(s1.rgb, vec3(peak), 0.12) * (min(peak * 0.9, 1.3) / max(peak, 1e-4));
    if (placed) {
      float d = length(Pw - s2.xyz);
      float lit = 1.0 - smoothstep(bulb * 0.45, bulb * 1.5, d);
      result += hue * lit * min(eye.w, 1.0);
    }
    if (night.y > 0.0) {
      // (Far off its glass is under a pixel and may not be drawn at all: from 60 m to 150 m away its glow needs none.)
      float far = smoothstep(60.0, 150.0, 0.1 / max(s1.w, 1e-7));
      vec2 lg = SpriteSeen(i, s0, s1, s2, bulb);
      float seen = lg.x * max(lg.y, far);
      float r = sqrt(r2), edge = 1.0 - r;
      float glare = exp(-r2 * 160.0) * 1.2 + 0.010 / (r2 + 0.010) * edge * edge * edge * 0.45;
      result += s1.rgb * glare * seen * night.y * eye.w;
    }
  }
  // D183: the camera's lens flare from the brightest few lamps in view (the first sprites, their strength in s2.w's
  // whole pairs, 255ths), stronger the more directly the camera looks at one: faint ghosts along the line through the picture's middle,
  // a soft halo ring and a milky veil round the light (the lens's own haze), in the lamp's colour.
  if (glow2.w > 0.0) {
    vec2 A = vec2(aspect, 1.0);
    for (int i = 0; i < min(ns, 4); ++i) {
      vec4 s0 = sprites[i * 3], s1 = sprites[i * 3 + 1], s2 = sprites[i * 3 + 2];
      float pairs = floor(s2.w * 0.5), bulb = s2.w - 2.0 * pairs, strength = pairs / 255.0;
      if (strength <= 0.0) continue;
      vec2 c = s0.xy;
      if (c.x < 0.0 || c.x > 1.0 || c.y < 0.0 || c.y > 1.0) continue;
      vec2 lg = SpriteSeen(i, s0, s1, s2, bulb);
      float seen = lg.x;
      if (seen <= 0.0) continue;
      seen *= lg.y;
      if (seen <= 0.0) continue;
      vec2 v = c - 0.5;
      float look = 1.0 - smoothstep(0.0, 1.0, length(v * A * 2.0 / aspect));
      float F = strength * seen * (0.3 + 0.7 * look) * glow2.w * eye.w;
      vec3 col = s1.rgb / max(max(s1.r, max(s1.g, s1.b)), 1e-3);
      float dl = length((v_texcoord - c) * A);
      vec3 add = col * (exp(-dl * dl * 30.0) * 0.022 + exp(-pow((dl - 0.12) / 0.010, 2.0)) * 0.006 +
                        exp(-pow((dl - 0.24) / 0.020, 2.0)) * 0.0025);
      const float ts[4] = float[4](-0.42, -0.86, -1.30, 0.55);
      const float rs[4] = float[4](0.030, 0.055, 0.085, 0.020);
      const vec3 tints[4] = vec3[4](vec3(1.0, 0.72, 0.42), vec3(0.42, 0.80, 1.0), vec3(0.78, 0.55, 1.0), vec3(1.0));
      for (int g = 0; g < 4; ++g) {
        vec2 at = 0.5 + v * ts[g];
        float dg = length((v_texcoord - at) * A);
        float disc = smoothstep(rs[g], rs[g] * 0.6, dg) * (0.7 + 0.3 * smoothstep(rs[g] * 0.5, rs[g] * 0.95, dg));
        add += col * tints[g] * disc * 0.018;
      }
      result += add * F;
    }
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
    float inverse_t[4], eye[4], counts[4], glow2[4];
    float extra[4];  // x: 1 with the lamps' visibility found once each (spriteSeenTex)
    uint32_t beams[4];  // the lights that are turning beams (bits 0 to 95 in x, y, z)
    uint32_t tiles[kTilesX * kTilesY][4];  // each screen tile's lights (bits 0 to 95 in x, y, z)
    float lights[kMaxLights * 3][4];
    float sprites[kMaxSprites * 3][4];
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

  Pass* PassFor(Diligent::IRenderDevice* device, Diligent::TEXTURE_FORMAT format, bool seen_pass) {
    using namespace Diligent;
    const uint32_t key = uint32_t(format) * 2u + (seen_pass ? 1u : 0u);
    auto it = passes_.find(key);
    if (it != passes_.end()) return it->second.pso ? &it->second : nullptr;
    Pass& pass = passes_[key];
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
    // kCommon starts with its #version line: the variant's define goes right after it.
    std::string source = std::string(kCommon) + kPixel;
    if (seen_pass) source.insert(source.find('\n') + 1, "#define NIGHT_SEEN_PASS 1\n");
    RefCntAutoPtr<IShader> ps = Shader(device, SHADER_TYPE_PIXEL, source.c_str(),
                                       seen_pass ? "NativeXRenderer night PS (lamps seen once)" : "NativeXRenderer night PS");
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
        {SHADER_TYPE_PIXEL, "effectLightsTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_PIXEL, "spriteSeenTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC}};
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "colorTex", point},
                                       {SHADER_TYPE_PIXEL, "depthTex", point},
                                       {SHADER_TYPE_PIXEL, "worldDepthTex", point},
                                       {SHADER_TYPE_PIXEL, "glowTex", linear},
                                       {SHADER_TYPE_PIXEL, "effectLightsTex", point},
                                       {SHADER_TYPE_PIXEL, "spriteSeenTex", point}};
    info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    info.PSODesc.ResourceLayout.Variables = variables;
    info.PSODesc.ResourceLayout.NumVariables = 6;
    info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    info.PSODesc.ResourceLayout.NumImmutableSamplers = 6;
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
    t.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET | BIND_UNORDERED_ACCESS;
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

  // BF_NIGHT_TILES_CHECK: the picture just drawn (tiled) kept, the pass drawn again from the same input with every light
  // in every tile, both read back and compared: the pixels that differ and by how much (0 and 0: the tiles change
  // nothing). The second picture stays (the same, or the check says so).
  void TilesCheck(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, Diligent::ITexture* texture,
                  Diligent::ITextureView* target, Pass* pass, const Diligent::Viewport& viewport) {
    using namespace Diligent;
    const TextureDesc& desc = texture->GetDesc();
    for (auto& st : tiles_staging_) {
      if (st && st->GetDesc().Width == desc.Width && st->GetDesc().Height == desc.Height && st->GetDesc().Format == desc.Format)
        continue;
      st.Release();
      TextureDesc t = desc;
      t.Name = "NativeXRenderer night tiles check";
      t.Usage = USAGE_STAGING;
      t.BindFlags = BIND_NONE;
      t.CPUAccessFlags = CPU_ACCESS_READ;
      t.MiscFlags = MISC_TEXTURE_FLAG_NONE;
      t.MipLevels = 1;
      device->CreateTexture(t, nullptr, &st);
      if (!st) return;
    }
    CopyTextureAttribs first{texture, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, tiles_staging_[0],
                             RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
    context->CopyTexture(first);
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, untiled_.get(), sizeof(Constants));
    context->UnmapBuffer(constants_, MAP_WRITE);
    context->SetRenderTargets(1, &target, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->SetViewports(1, &viewport, 0, 0);
    context->SetPipelineState(pass->pso);
    context->CommitShaderResources(pass->srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    DrawAttribs draw;
    draw.NumVertices = 3;
    context->Draw(draw);
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
    CopyTextureAttribs second{texture, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, tiles_staging_[1],
                              RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
    context->CopyTexture(second);
    context->WaitForIdle();
    MappedTextureSubresource a, b;
    context->MapTextureSubresource(tiles_staging_[0], 0, 0, MAP_READ, MAP_FLAG_NONE, nullptr, a);
    context->MapTextureSubresource(tiles_staging_[1], 0, 0, MAP_READ, MAP_FLAG_NONE, nullptr, b);
    uint64_t differ = 0;
    int most = 0;
    if (a.pData && b.pData) {
      const uint32_t row_bytes = uint32_t(std::min<uint64_t>(a.Stride, b.Stride));
      const uint32_t bytes = std::min<uint32_t>(row_bytes, desc.Width * 4);
      for (uint32_t y = 0; y < desc.Height; ++y) {
        const uint8_t* pa = static_cast<const uint8_t*>(a.pData) + size_t(y) * a.Stride;
        const uint8_t* pb = static_cast<const uint8_t*>(b.pData) + size_t(y) * b.Stride;
        if (std::memcmp(pa, pb, bytes) == 0) continue;
        for (uint32_t x = 0; x + 4 <= bytes; x += 4) {
          int d = 0;
          for (int c = 0; c < 4; ++c) d = std::max(d, std::abs(int(pa[x + c]) - int(pb[x + c])));
          if (d) ++differ, most = std::max(most, d);
        }
      }
    }
    if (a.pData) context->UnmapTextureSubresource(tiles_staging_[0], 0, 0);
    if (b.pData) context->UnmapTextureSubresource(tiles_staging_[1], 0, 0);
    REXLOG_INFO("[native_x_renderer] night: tiles check: {}x{} format {}: {} pixels differ, by {} at most (a byte)",
                desc.Width, desc.Height, int(desc.Format), differ, most);
  }

  // Which lights can reach each of the picture's 16 x 9 tiles (the Night Ops performance pass, 20 Nov 2026: every
  // pixel looped over every light in view, up to 96, though a light lights only what is within its range; with the
  // lamps of Wake Island in view that loop was most of the night pass, about 1.3 ms of its 1.85). A light reaches
  // only points within its range of it, and a point is drawn where it projects: inside the projection of the cube
  // round the light's sphere. So a tile outside that box has none of the light's pixels, and leaving the light out of
  // its list changes nothing; the lists keep the lights' order, so each pixel adds the same lights in the same order.
  // A light whose cube reaches behind the camera, or holds it, is in every tile; so is every light without a camera
  // or with `tiled` false (comparisons). The turning beams (seen in the air across the picture) go in their own list.
  static void TileLights(const NightFrame& f, uint32_t lights, bool tiled, Constants& k) {
    auto set = [](uint32_t* mask, uint32_t i) { mask[i / 32] |= 1u << (i % 32); };
    for (uint32_t i = 0; i < lights; ++i) {
      const NightLight& l = f.lights[i];
      const bool beam = l.cos_outer >= 0.985f && l.range >= 150.0f &&
                        l.direction[0] * l.direction[0] + l.direction[1] * l.direction[1] + l.direction[2] * l.direction[2] >= 0.25f;
      if (beam) set(k.beams, i);
    }
    // The world to clip x, y and w: the inverse of f.inverse (clip = M world + t).
    const float(*a)[3] = f.inverse;
    const float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                      a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    const bool camera = tiled && f.camera && std::fabs(det) > 1e-20f && f.to_ndc[0] != 0.0f && f.to_ndc[1] != 0.0f;
    float m[3][3] = {};
    if (camera) {
      const float inv = 1.0f / det;
      m[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * inv;
      m[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * inv;
      m[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * inv;
      m[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) * inv;
      m[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * inv;
      m[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * inv;
      m[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * inv;
      m[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * inv;
      m[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * inv;
    }
    for (uint32_t i = 0; i < lights; ++i) {
      const NightLight& l = f.lights[i];
      // The light's box on the picture, in texture coordinates (0 to 1); all of it when it cannot be found.
      float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
      bool boxed = false;
      if (camera && l.range > 0.0f) {
        const float r = l.range;
        const float dx = l.position[0] - f.eye[0], dy = l.position[1] - f.eye[1], dz = l.position[2] - f.eye[2];
        if (dx * dx + dy * dy + dz * dz > 3.0f * r * r * 1.0201f) {  // the eye outside the cube (its corners' reach)
          boxed = true;
          u0 = v0 = 1e30f, u1 = v1 = -1e30f;
          for (int c = 0; c < 8 && boxed; ++c) {
            const float w[3] = {l.position[0] + ((c & 1) ? r : -r), l.position[1] + ((c & 2) ? r : -r),
                                l.position[2] + ((c & 4) ? r : -r)};
            float clip[3];
            for (int row = 0; row < 3; ++row)
              clip[row] = m[row][0] * w[0] + m[row][1] * w[1] + m[row][2] * w[2] + f.inverse_t[row];
            if (clip[2] <= 1e-3f) {  // a corner at or behind the eye: the box would wrap
              boxed = false;
              break;
            }
            const float u = (clip[0] / clip[2] - f.to_ndc[2]) / f.to_ndc[0];
            const float v = (clip[1] / clip[2] - f.to_ndc[3]) / f.to_ndc[1];
            u0 = std::min(u0, u), u1 = std::max(u1, u), v0 = std::min(v0, v), v1 = std::max(v1, v);
          }
          if (!boxed) u0 = v0 = 0.0f, u1 = v1 = 1.0f;
        }
      }
      if (boxed) {
        // A margin of a hundredth of the picture for the rounding of the pixels' own places.
        u0 -= 0.01f, v0 -= 0.01f, u1 += 0.01f, v1 += 0.01f;
        if (u1 < 0.0f || v1 < 0.0f || u0 > 1.0f || v0 > 1.0f) continue;  // off the picture: in no tile
      }
      const int tx0 = std::clamp(int(std::floor(u0 * kTilesX)), 0, int(kTilesX) - 1);
      const int tx1 = std::clamp(int(std::floor(u1 * kTilesX)), 0, int(kTilesX) - 1);
      const int ty0 = std::clamp(int(std::floor(v0 * kTilesY)), 0, int(kTilesY) - 1);
      const int ty1 = std::clamp(int(std::floor(v1 * kTilesY)), 0, int(kTilesY) - 1);
      for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx) set(k.tiles[ty * kTilesX + tx], i);
    }
  }

  // The effects' lights' compute search (kComputeLights), made once after LightsPass (its texture); false if that
  // failed (the pixel pass then searches as before).
  bool ComputeLights(Diligent::IRenderDevice* device) {
    using namespace Diligent;
    if (compute_tried_) return compute_lights_.srb != nullptr;
    compute_tried_ = true;
    if (!constants_ || !effect_lights_) return false;
    std::vector<uint32_t> spirv =
        CompileGlsl(GlslStage::kCompute, std::string(kCommon) + kComputeLights, "NativeXRenderer night lights CS");
    if (spirv.empty()) {
      REXLOG_ERROR("[native_x_renderer] night: the effects' lights' compute search did not compile: the pixel pass searches");
      return false;
    }
    RefCntAutoPtr<IShader> cs;
    ShaderCreateInfo sci;
    sci.Desc.Name = "NativeXRenderer night lights CS";
    sci.Desc.ShaderType = SHADER_TYPE_COMPUTE;
    sci.Desc.UseCombinedTextureSamplers = true;
    sci.ByteCode = spirv.data();
    sci.ByteCodeSize = spirv.size() * sizeof(uint32_t);
    device->CreateShader(sci, &cs);
    if (!cs) return false;
    ComputePipelineStateCreateInfo info;
    info.PSODesc.Name = "NativeXRenderer night effects' lights (compute)";
    info.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
    info.pCS = cs;
    SamplerDesc point;
    point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
    point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
    ShaderResourceVariableDesc variables[] = {
        {SHADER_TYPE_COMPUTE, "colorTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_COMPUTE, "depthTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_COMPUTE, "worldDepthTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_COMPUTE, "glowTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_COMPUTE, "effectLightsTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_COMPUTE, "effectLightsOut", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC}};
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_COMPUTE, "colorTex", point},
                                       {SHADER_TYPE_COMPUTE, "depthTex", point},
                                       {SHADER_TYPE_COMPUTE, "worldDepthTex", point},
                                       {SHADER_TYPE_COMPUTE, "glowTex", point},
                                       {SHADER_TYPE_COMPUTE, "effectLightsTex", point}};
    info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    info.PSODesc.ResourceLayout.Variables = variables;
    info.PSODesc.ResourceLayout.NumVariables = 6;
    info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    info.PSODesc.ResourceLayout.NumImmutableSamplers = 5;
    device->CreateComputePipelineState(info, &compute_lights_.pso);
    if (!compute_lights_.pso) {
      REXLOG_ERROR("[native_x_renderer] night: no pipeline for the effects' lights' compute search: the pixel pass searches");
      return false;
    }
    if (auto* variable = compute_lights_.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "NightConstants"))
      variable->Set(constants_);
    compute_lights_.pso->CreateShaderResourceBinding(&compute_lights_.srb, true);
    if (compute_lights_.srb) REXLOG_INFO("[native_x_renderer] night: the effects' lights found by a compute search");
    return compute_lights_.srb != nullptr;
  }

  // The lamps' visibility pass and its kMaxSprites x 1 texture (RGBA32F), made once; false if that failed (the main
  // pass then finds it per pixel as before).
  bool SeenPass(Diligent::IRenderDevice* device) {
    using namespace Diligent;
    if (seen_tried_) return seen_pass_.srb != nullptr;
    seen_tried_ = true;
    if (!constants_) return false;
    TextureDesc t;
    t.Name = "NativeXRenderer night lamps' visibility";
    t.Type = RESOURCE_DIM_TEX_2D;
    t.Width = kMaxSprites;
    t.Height = 1;
    t.Format = TEX_FORMAT_RGBA32_FLOAT;
    t.Usage = USAGE_DEFAULT;
    t.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
    device->CreateTexture(t, nullptr, &sprite_seen_);
    RefCntAutoPtr<IShader> vs = Shader(device, SHADER_TYPE_VERTEX, kVertex, "NativeXRenderer night lamps seen VS");
    RefCntAutoPtr<IShader> ps = Shader(device, SHADER_TYPE_PIXEL, (std::string(kCommon) + kPixelSeen).c_str(),
                                       "NativeXRenderer night lamps seen PS");
    if (!sprite_seen_ || !vs || !ps) {
      REXLOG_ERROR("[native_x_renderer] night: the lamps' visibility pass could not be made: found per pixel instead");
      return false;
    }
    GraphicsPipelineStateCreateInfo info;
    info.PSODesc.Name = "NativeXRenderer night lamps' visibility";
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
    device->CreateGraphicsPipelineState(info, &seen_pass_.pso);
    if (!seen_pass_.pso) {
      REXLOG_ERROR("[native_x_renderer] night: no pipeline for the lamps' visibility: found per pixel instead");
      return false;
    }
    if (auto* variable = seen_pass_.pso->GetStaticVariableByName(SHADER_TYPE_PIXEL, "NightConstants"))
      variable->Set(constants_);
    seen_pass_.pso->CreateShaderResourceBinding(&seen_pass_.srb, true);
    return seen_pass_.srb != nullptr;
  }

  // D179: the effects' layer without what was in it before the effects were drawn (the game's sky dome, or the
  // picture it began as): `layer` minus `before`, never under nothing, into `target` (the three of one size).
 public:
  bool Subtract(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, Diligent::ITextureView* layer,
                Diligent::ITextureView* before, Diligent::ITextureView* target) {
    using namespace Diligent;
    if (!device || !context || !layer || !before || !target) return false;
    if (!subtract_tried_) {
      subtract_tried_ = true;
      static constexpr const char* kPixelSubtract = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D layerTex;
layout(set = 0, binding = 1) uniform sampler2D beforeTex;
layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 out_color;
void main() {
  vec4 a = textureLod(layerTex, v_texcoord, 0.0), b = textureLod(beforeTex, v_texcoord, 0.0);
  out_color = vec4(max(a.rgb - b.rgb, vec3(0.0)), a.a);
}
)";
      RefCntAutoPtr<IShader> vs = Shader(device, SHADER_TYPE_VERTEX, kVertex, "NativeXRenderer night subtract VS");
      RefCntAutoPtr<IShader> ps = Shader(device, SHADER_TYPE_PIXEL, kPixelSubtract, "NativeXRenderer night subtract PS");
      if (vs && ps) {
        GraphicsPipelineStateCreateInfo info;
        info.PSODesc.Name = "NativeXRenderer night effects' layer alone";
        info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
        info.GraphicsPipeline.NumRenderTargets = 1;
        info.GraphicsPipeline.RTVFormats[0] = target->GetTexture()->GetDesc().Format;
        info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
        info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
        info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
        info.pVS = vs;
        info.pPS = ps;
        SamplerDesc point;
        point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
        point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
        ShaderResourceVariableDesc variables[] = {{SHADER_TYPE_PIXEL, "layerTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
                                                  {SHADER_TYPE_PIXEL, "beforeTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC}};
        ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "layerTex", point}, {SHADER_TYPE_PIXEL, "beforeTex", point}};
        info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
        info.PSODesc.ResourceLayout.Variables = variables;
        info.PSODesc.ResourceLayout.NumVariables = 2;
        info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
        info.PSODesc.ResourceLayout.NumImmutableSamplers = 2;
        device->CreateGraphicsPipelineState(info, &subtract_pass_.pso);
        if (subtract_pass_.pso) subtract_pass_.pso->CreateShaderResourceBinding(&subtract_pass_.srb, true);
      }
      if (!subtract_pass_.srb) REXLOG_ERROR("[native_x_renderer] night: no pass for the effects' layer alone");
    }
    if (!subtract_pass_.srb) return false;
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
    subtract_pass_.srb->GetVariableByName(SHADER_TYPE_PIXEL, "layerTex")->Set(layer);
    subtract_pass_.srb->GetVariableByName(SHADER_TYPE_PIXEL, "beforeTex")->Set(before);
    context->SetRenderTargets(1, &target, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    const TextureDesc& out = target->GetTexture()->GetDesc();
    Viewport viewport;
    viewport.Width = float(out.Width);
    viewport.Height = float(out.Height);
    viewport.MaxDepth = 1.0f;
    context->SetViewports(1, &viewport, 0, 0);
    context->SetPipelineState(subtract_pass_.pso);
    context->CommitShaderResources(subtract_pass_.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    DrawAttribs draw;
    draw.NumVertices = 3;
    context->Draw(draw);
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
    return true;
  }

 private:
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
  std::map<uint32_t, Pass> passes_;  // by format x 2 + 1 with the lamps' visibility pass
  Diligent::RefCntAutoPtr<Diligent::ITexture> copy_;
  Pass lights_pass_;
  Pass seen_pass_;  // the lamps' visibility, once a frame
  Pass compute_lights_;  // the effects' lights' compute search
  std::unique_ptr<Constants> untiled_;  // BF_NIGHT_TILES_CHECK's constants with every light in every tile
  Diligent::RefCntAutoPtr<Diligent::ITexture> tiles_staging_[2];
  uint64_t tiles_check_count_ = 0;
  // BF_NIGHT_FX_CHECK's two read back copies of the 4 x 2 texture (the pixel pass's, the compute search's).
  Diligent::RefCntAutoPtr<Diligent::ITexture> staging_[2];
  uint64_t check_count_ = 0;
  bool Staging(Diligent::IRenderDevice* device) {
    using namespace Diligent;
    if (staging_[0] && staging_[1]) return true;
    for (auto& st : staging_) {
      TextureDesc t;
      t.Name = "NativeXRenderer night effects' lights check";
      t.Type = RESOURCE_DIM_TEX_2D;
      t.Width = 4;
      t.Height = 2;
      t.Format = TEX_FORMAT_RGBA32_FLOAT;
      t.Usage = USAGE_STAGING;
      t.BindFlags = BIND_NONE;
      t.CPUAccessFlags = CPU_ACCESS_READ;
      device->CreateTexture(t, nullptr, &st);
      if (!st) return false;
    }
    return true;
  }
  bool compute_tried_ = false;
  uint64_t applied_compute_lights_ = 0;
  Diligent::RefCntAutoPtr<Diligent::ITexture> sprite_seen_;
  bool seen_tried_ = false;
  uint64_t applied_seen_ = 0;
  // The night pass's GPU time: a duration query around each Apply, read when the query comes round again (8 frames
  // on), so nothing waits for the GPU.
  class Timer {
   public:
    void Begin(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context) {
      using namespace Diligent;
      if (!tried_) {
        tried_ = true;
        if (device->GetDeviceInfo().Features.DurationQueries == DEVICE_FEATURE_STATE_ENABLED) {
          for (auto& q : queries_) {
            QueryDesc d;
            d.Name = "NativeXRenderer night pass time";
            d.Type = QUERY_TYPE_DURATION;
            device->CreateQuery(d, &q);
            if (!q) return;
          }
          on_ = true;
        }
      }
      if (!on_) return;
      if (pending_[index_]) {
        QueryDataDuration data;
        if (queries_[index_]->GetData(&data, sizeof(data), true) && data.Frequency) {
          const double ms = double(data.Duration) * 1000.0 / double(data.Frequency);
          total_ += ms, most_ = std::max(most_, ms), ++frames_;
        }
        pending_[index_] = false;
      }
      context->BeginQuery(queries_[index_]);
      active_ = true;
    }
    void End(Diligent::IDeviceContext* context) {
      if (!active_) return;
      context->EndQuery(queries_[index_]);
      pending_[index_] = true;
      index_ = (index_ + 1) % kCount;
      active_ = false;
    }
    void Take(double& average, double& most, uint64_t& frames) {
      average = frames_ ? total_ / double(frames_) : 0.0, most = most_, frames = frames_;
      total_ = most_ = 0.0, frames_ = 0;
    }

   private:
    static constexpr uint32_t kCount = 8;
    Diligent::RefCntAutoPtr<Diligent::IQuery> queries_[kCount];
    bool pending_[kCount] = {};
    uint32_t index_ = 0;
    bool tried_ = false, on_ = false, active_ = false;
    double total_ = 0.0, most_ = 0.0;
    uint64_t frames_ = 0;
  } timer_, lights_timer_;  // the whole night pass; the effects' lights' search alone
  Pass subtract_pass_;  // D179
  bool subtract_tried_ = false;
  Diligent::RefCntAutoPtr<Diligent::ITexture> effect_lights_;
  bool lights_tried_ = false;
  uint64_t applied_effect_lights_ = 0;
  uint64_t applied_ = 0, applied_world_ = 0, applied_glow_ = 0, lights_ = 0, sprites_ = 0;
  bool logged_ = false, logged_world_ = false, logged_glow_ = false, logged_lights_ = false;
};

}  // namespace native_x_renderer
