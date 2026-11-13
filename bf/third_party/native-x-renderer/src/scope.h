// D168: the picture in picture sniper scope (Battlefield 1943 Reborn 0.4.1, Settings > Enhancements > Sniper scope,
// experimental, off by default; TekRantGaming, 13 Nov 2026: "add a modern sniper scope implementation" and "Picture in
// picture"). While the game's sniper scope is up only its lens is magnified, and the world around it stays in view:
//   - every frame the view is not zoomed, the game's finished scene (its composite, before the HUD) is remembered with
//     the camera it was seen from (Remember);
//   - while the scope is up the game composes the zoomed view black but for a round view (its scoped composite; while
//     the scope comes up, a black pass of its own). Right after that (Apply), the lens keeps the middle of its picture
//     in a smaller circle, and around it the remembered unzoomed view is drawn turned to where the camera looks now,
//     softened (a coarser mip) and darkened, with the lens's dark rim. The HUD is drawn over it as always; the game's
//     reticle, a full screen quad among the HUD's draws, is cut to the lens's square (DrawPath::SetScopeClip), so its
//     lines end at the lens (runs v41scope12 to v41scope16, 13 Nov 2026).
// The world around the lens is the one seen when the scope went up, turned with the camera: what moves there stays where
// it was until the scope comes down (one view is drawn a frame, not two).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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

#include <rex/logging.h>

namespace native_x_renderer {

// The camera a view was seen from: its right, up and back in the world, and the tangent of half its vertical field
// of view.
struct ScopeCamera {
  float right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, back[3] = {0, 0, 1};
  float tan_half_v = 0.5f;
};

class Scope {
 public:
  // Not scoped: the finished scene (`texture`) kept, with the camera now. `weapon_depth` (null for none): the view's depth
  // at its end, which on foot holds only the first person weapon (the rest cleared to far, D162), and the share of it
  // the view covers: the weapon's pixels are left out (alpha 0; the mips then average the world's alone), so the world
  // around the lens has no frozen rifle in it (run v41scope18: the rifle held low as the scope came up).
  void Remember(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, Diligent::ITexture* texture,
                const ScopeCamera& camera, Diligent::ITextureView* weapon_depth, const float depth_uv[2]) {
    using namespace Diligent;
    if (!device || !context || !texture) return;
    const TextureDesc& desc = texture->GetDesc();
    if (!EnsureTexture(device, desc, around_, true)) return;
    Pass* pass = RememberPassFor(device, around_->GetDesc().Format);
    ITextureView* color = texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    ITextureView* out = around_->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    if (!pass || !color || !out) return;
    if (!remember_logged_) {
      remember_logged_ = true;
      REXLOG_INFO("[native_x_renderer] scope: the view before the scope remembered from the composite, {}x{} format {}, "
                  "{} (D168)", desc.Width, desc.Height, int(desc.Format),
                  weapon_depth ? "the first person weapon left out by the view's depth" : "without the view's depth");
    }
    {
      const float k[4] = {depth_uv[0], depth_uv[1], weapon_depth ? 1.0f : 0.0f, 0.0f};
      void* data = nullptr;
      context->MapBuffer(remember_constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
      std::memcpy(data, k, sizeof(k));
      context->UnmapBuffer(remember_constants_, MAP_WRITE);
    }
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
    pass->srb->GetVariableByName(SHADER_TYPE_PIXEL, "colorTex")->Set(color);
    pass->srb->GetVariableByName(SHADER_TYPE_PIXEL, "depthTex")->Set(weapon_depth ? weapon_depth : color);
    context->SetRenderTargets(1, &out, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Viewport viewport;
    viewport.Width = float(desc.Width);
    viewport.Height = float(desc.Height);
    viewport.MaxDepth = 1.0f;
    context->SetViewports(1, &viewport, 0, 0);
    context->SetPipelineState(pass->pso);
    context->CommitShaderResources(pass->srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    DrawAttribs draw;
    draw.NumVertices = 3;
    context->Draw(draw);
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
    // Its mips when the scope comes up (Apply): not every frame without it (a full chain of the output's size).
    mips_stale_ = true;
    remembered_ = camera;
    have_around_ = true;
    remembered_at_ = frame_;
  }

  // Scoped: the picture in picture drawn over `texture` (its render target view `target`). `camera` is the camera now,
  // with the unzoomed field of view (the one the world around the lens is shown with).
  void Apply(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, Diligent::ITexture* texture,
             Diligent::ITextureView* target, const ScopeCamera& camera) {
    using namespace Diligent;
    if (!device || !context || !texture || !target) return;
    const TextureDesc& desc = texture->GetDesc();
    if (!EnsureTexture(device, desc, zoomed_, false)) return;
    if (mips_stale_ && have_around_ && around_) {
      context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
      context->GenerateMips(around_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
      mips_stale_ = false;
    }
    Pass* pass = PassFor(device, desc.Format);
    if (!pass) return;
    const float aspect = float(desc.Width) / float(std::max(1u, desc.Height));
    {
      Constants k = {};
      k.lens[0] = kLensRadius, k.lens[1] = aspect, k.lens[2] = kRimWidth, k.lens[3] = have_around_ ? 1.0f : 0.0f;
      for (int i = 0; i < 3; ++i) {
        k.now_right[i] = camera.right[i], k.now_up[i] = camera.up[i], k.now_back[i] = camera.back[i];
        k.then_right[i] = remembered_.right[i], k.then_up[i] = remembered_.up[i], k.then_back[i] = remembered_.back[i];
      }
      // The softening: a mip about 4 x 4 texels to one at 1080 lines.
      const float mip = std::max(0.0f, std::log2(float(desc.Height) / 1080.0f) + 2.0f);
      k.fov[0] = std::max(camera.tan_half_v, 0.01f), k.fov[1] = std::max(remembered_.tan_half_v, 0.01f);
      k.fov[2] = mip, k.fov[3] = kAround;
      // NXR_SCOPE_DEBUG=1 (research): the world around the lens red, so the pass is seen wherever it is drawn.
      static const bool debug = std::getenv("NXR_SCOPE_DEBUG") != nullptr;
      if (debug) k.lens[3] = 2.0f;
      void* data = nullptr;
      context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
      std::memcpy(data, &k, sizeof(k));
      context->UnmapBuffer(constants_, MAP_WRITE);
    }
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
    CopyTextureAttribs copy{texture, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, zoomed_,
                            RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
    context->CopyTexture(copy);
    ITextureView* zoomed = zoomed_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    pass->srb->GetVariableByName(SHADER_TYPE_PIXEL, "zoomTex")->Set(zoomed);
    // Without a remembered view yet, the zoomed view stands in (lens.w says it is not used).
    pass->srb->GetVariableByName(SHADER_TYPE_PIXEL, "aroundTex")
        ->Set((have_around_ && around_ ? around_ : zoomed_)->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
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
    if (!logged_) {
      logged_ = true;
      REXLOG_INFO("[native_x_renderer] scope: picture in picture after the game's scope mask, {}x{} format {}, {} (D168)",
                  desc.Width, desc.Height, int(desc.Format),
                  have_around_ ? "the world around from the view before the scope" : "nothing remembered yet");
    }
  }

  // The lens's square in the pixels of a `width` x `height` picture (the game's reticle is cut to it: DrawPath's
  // SetScopeClip).
  static void LensSquare(uint32_t width, uint32_t height, int& left, int& top, int& right, int& bottom) {
    const float r = kLensRadius * float(height), cx = float(width) * 0.5f, cy = float(height) * 0.5f;
    left = int(std::floor(cx - r)), right = int(std::ceil(cx + r));
    top = int(std::floor(cy - r)), bottom = int(std::ceil(cy + r));
  }
  void EndFrame() { ++frame_; }
  uint64_t TakeApplied() {
    const uint64_t n = applied_;
    applied_ = 0;
    return n;
  }
  void Forget() { have_around_ = false; }

 private:
  static constexpr float kLensRadius = 0.30f;  // the lens's radius, a share of the screen's height
  static constexpr float kRimWidth = 0.018f;   // its dark rim
  static constexpr float kAround = 0.45f;      // how much darker the world around the lens is

  struct Pass {
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
  };
  struct Constants {
    float lens[4];  // radius (share of the height), aspect, rim width, 1 with a remembered view
    float now_right[4], now_up[4], now_back[4];
    float then_right[4], then_up[4], then_back[4];
    float fov[4];  // tan(half vertical fov) now (unzoomed) and remembered, the softening's mip, the darkening
  };

  static constexpr const char* kVertex = R"(#version 450
layout(location = 0) out vec2 v_texcoord;
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  v_texcoord = vec2(p.x, 1.0 - p.y);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
  static constexpr const char* kPixel = R"(#version 450
layout(set = 0, binding = 0) uniform ScopeConstants {
  vec4 lens;
  vec4 now_right;
  vec4 now_up;
  vec4 now_back;
  vec4 then_right;
  vec4 then_up;
  vec4 then_back;
  vec4 fov;
};
layout(set = 0, binding = 1) uniform sampler2D zoomTex;
layout(set = 0, binding = 2) uniform sampler2D aroundTex;
layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 o_color;

void main() {
  vec2 uv = v_texcoord;
  // From the middle of the screen, in heights (so the lens is round on any screen).
  vec2 p = (uv - 0.5) * vec2(lens.y, 1.0);
  float r = length(p);
  float R = lens.x;
  if (r < R) {
    // The lens: the middle of the game's scope view as it is (its reticle in it), a little darker toward its edge.
    vec3 c = texture(zoomTex, uv).rgb;
    c *= 1.0 - 0.35 * smoothstep(R * 0.80, R, r);
    o_color = vec4(c, 1.0);
    return;
  }
  vec3 c = vec3(0.0);
  if (lens.w > 1.5) {
    o_color = vec4(0.9, 0.05, 0.05, 1.0);
    return;
  }
  if (lens.w > 0.5) {
    // Around the lens: the remembered unzoomed view, as the camera now would see it.
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    vec3 world = ndc.x * fov.x * lens.y * now_right.xyz + ndc.y * fov.x * now_up.xyz - now_back.xyz;
    vec3 d = vec3(dot(world, then_right.xyz), dot(world, then_up.xyz), dot(world, then_back.xyz));
    if (d.z < -1e-3) {
      vec2 t = d.xy / -d.z;
      vec2 n = vec2(t.x / (fov.y * lens.y), t.y / fov.y);
      vec2 uv2 = vec2(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5);
      // Past the remembered view's edge: its edge, fading.
      float outside = max(max(-uv2.x, uv2.x - 1.0), max(-uv2.y, uv2.y - 1.0));
      // The remembered view holds the world's colour times its share (alpha), the weapon's pixels none. Push and pull:
      // from a coarse level, where every footprint has some world, each finer level's world colour is blended in by its
      // share, so the weapon's gap takes its neighbours' colours, finer toward its edge (run v41scope21: the first
      // fill, one coarse level, was a flat grey for the whole rifle).
      vec2 at = clamp(uv2, 0.0, 1.0);
      vec4 top = textureLod(aroundTex, at, fov.z + 7.0);
      vec3 seen = top.a > 1e-4 ? top.rgb / top.a : vec3(0.0);
      for (int i = 6; i >= 0; --i) {
        vec4 level = textureLod(aroundTex, at, fov.z + float(i));
        if (level.a > 1e-4) seen = mix(seen, level.rgb / level.a, clamp(level.a, 0.0, 1.0));
      }
      c = seen * (1.0 - clamp(outside * 4.0, 0.0, 0.8));
    }
    c *= 1.0 - fov.w;
  }
  // The rim: dark just outside the lens.
  float rim = 1.0 - smoothstep(R, R + lens.z, r);
  c = mix(c, vec3(0.015), rim);
  o_color = vec4(c, 1.0);
}
)";

  // The remembered view: the composite's colour times the world's share, which is 0 on the first person weapon (the
  // view's depth at its end nearer than 3 m: on foot the rest is cleared to far, a reversed depth's 0; D162's test).
  static constexpr const char* kRememberPixel = R"(#version 450
layout(set = 0, binding = 0) uniform RememberConstants {
  vec4 mask;  // xy: the share of the depth the view covers, z: 1 with the depth
};
layout(set = 0, binding = 1) uniform sampler2D colorTex;
layout(set = 0, binding = 2) uniform sampler2D depthTex;
layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 o_color;
void main() {
  vec3 c = textureLod(colorTex, v_texcoord, 0.0).rgb;
  float w = 1.0;
  if (mask.z > 0.5 && textureLod(depthTex, v_texcoord * mask.xy, 0.0).r > 0.0333) w = 0.0;
  o_color = vec4(c * w, w);
}
)";

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
      buffer.Name = "NativeXRenderer scope constants";
      buffer.Size = sizeof(Constants);
      buffer.Usage = USAGE_DYNAMIC;
      buffer.BindFlags = BIND_UNIFORM_BUFFER;
      buffer.CPUAccessFlags = CPU_ACCESS_WRITE;
      device->CreateBuffer(buffer, nullptr, &constants_);
      if (!constants_) return nullptr;
    }
    RefCntAutoPtr<IShader> vs = Shader(device, SHADER_TYPE_VERTEX, kVertex, "NativeXRenderer scope VS");
    RefCntAutoPtr<IShader> ps = Shader(device, SHADER_TYPE_PIXEL, kPixel, "NativeXRenderer scope PS");
    if (!vs || !ps) {
      REXLOG_ERROR("[native_x_renderer] scope: its shaders did not compile: the game's own scope is drawn");
      return nullptr;
    }
    GraphicsPipelineStateCreateInfo info;
    info.PSODesc.Name = "NativeXRenderer scope";
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
    SamplerDesc linear;  // the world around: its mips, softened
    linear.MinFilter = linear.MagFilter = linear.MipFilter = FILTER_TYPE_LINEAR;
    linear.AddressU = linear.AddressV = linear.AddressW = TEXTURE_ADDRESS_CLAMP;
    ShaderResourceVariableDesc variables[] = {{SHADER_TYPE_PIXEL, "zoomTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
                                              {SHADER_TYPE_PIXEL, "aroundTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC}};
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "zoomTex", point}, {SHADER_TYPE_PIXEL, "aroundTex", linear}};
    info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    info.PSODesc.ResourceLayout.Variables = variables;
    info.PSODesc.ResourceLayout.NumVariables = 2;
    info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    info.PSODesc.ResourceLayout.NumImmutableSamplers = 2;
    device->CreateGraphicsPipelineState(info, &pass.pso);
    if (!pass.pso) {
      REXLOG_ERROR("[native_x_renderer] scope: no pipeline for format {}", int(format));
      return nullptr;
    }
    if (auto* variable = pass.pso->GetStaticVariableByName(SHADER_TYPE_PIXEL, "ScopeConstants")) variable->Set(constants_);
    pass.pso->CreateShaderResourceBinding(&pass.srb, true);
    return pass.srb ? &pass : nullptr;
  }

  Pass* RememberPassFor(Diligent::IRenderDevice* device, Diligent::TEXTURE_FORMAT format) {
    using namespace Diligent;
    auto it = remember_passes_.find(format);
    if (it != remember_passes_.end()) return it->second.pso ? &it->second : nullptr;
    Pass& pass = remember_passes_[format];
    if (!remember_constants_) {
      BufferDesc buffer;
      buffer.Name = "NativeXRenderer scope remember constants";
      buffer.Size = 16;
      buffer.Usage = USAGE_DYNAMIC;
      buffer.BindFlags = BIND_UNIFORM_BUFFER;
      buffer.CPUAccessFlags = CPU_ACCESS_WRITE;
      device->CreateBuffer(buffer, nullptr, &remember_constants_);
      if (!remember_constants_) return nullptr;
    }
    RefCntAutoPtr<IShader> vs = Shader(device, SHADER_TYPE_VERTEX, kVertex, "NativeXRenderer scope remember VS");
    RefCntAutoPtr<IShader> ps = Shader(device, SHADER_TYPE_PIXEL, kRememberPixel, "NativeXRenderer scope remember PS");
    if (!vs || !ps) {
      REXLOG_ERROR("[native_x_renderer] scope: the remembered view's shaders did not compile");
      return nullptr;
    }
    GraphicsPipelineStateCreateInfo info;
    info.PSODesc.Name = "NativeXRenderer scope remember";
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
    ShaderResourceVariableDesc variables[] = {{SHADER_TYPE_PIXEL, "colorTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
                                              {SHADER_TYPE_PIXEL, "depthTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC}};
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "colorTex", point}, {SHADER_TYPE_PIXEL, "depthTex", point}};
    info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    info.PSODesc.ResourceLayout.Variables = variables;
    info.PSODesc.ResourceLayout.NumVariables = 2;
    info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    info.PSODesc.ResourceLayout.NumImmutableSamplers = 2;
    device->CreateGraphicsPipelineState(info, &pass.pso);
    if (!pass.pso) {
      REXLOG_ERROR("[native_x_renderer] scope: no remembered view pipeline for format {}", int(format));
      return nullptr;
    }
    if (auto* variable = pass.pso->GetStaticVariableByName(SHADER_TYPE_PIXEL, "RememberConstants"))
      variable->Set(remember_constants_);
    pass.pso->CreateShaderResourceBinding(&pass.srb, true);
    return pass.srb ? &pass : nullptr;
  }

  // The zoomed view's copy (the pass's input) or the remembered view (`mips`: with its mips), each of the size and
  // format of the picture it copies (the composite's and the mask pass's may differ).
  bool EnsureTexture(Diligent::IRenderDevice* device, const Diligent::TextureDesc& desc,
                     Diligent::RefCntAutoPtr<Diligent::ITexture>& texture, bool mips) {
    using namespace Diligent;
    // The remembered view in half floats: its colour times the world's share, divided again where the weapon's gap is
    // filled (8 bits a channel lose too much there).
    const TEXTURE_FORMAT format = mips ? TEX_FORMAT_RGBA16_FLOAT : desc.Format;
    if (texture && texture->GetDesc().Width == desc.Width && texture->GetDesc().Height == desc.Height &&
        texture->GetDesc().Format == format)
      return true;
    texture.Release();
    if (mips) have_around_ = false;
    TextureDesc c;
    c.Name = mips ? "NativeXRenderer scope world around" : "NativeXRenderer scope input";
    c.Type = RESOURCE_DIM_TEX_2D;
    c.Width = desc.Width;
    c.Height = desc.Height;
    c.Format = format;
    c.Usage = USAGE_DEFAULT;
    c.BindFlags = BIND_SHADER_RESOURCE;
    if (mips) {
      c.MipLevels = 0;  // the whole chain
      c.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
      c.MiscFlags = MISC_TEXTURE_FLAG_GENERATE_MIPS;
    }
    device->CreateTexture(c, nullptr, &texture);
    if (!texture) {
      REXLOG_ERROR("[native_x_renderer] scope: its {} texture could not be made ({}x{} format {})",
                   mips ? "remembered view's" : "input", desc.Width, desc.Height, int(format));
      return false;
    }
    return true;
  }

  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_, remember_constants_;
  std::map<Diligent::TEXTURE_FORMAT, Pass> passes_, remember_passes_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> zoomed_, around_;
  ScopeCamera remembered_;
  bool have_around_ = false, mips_stale_ = false;
  uint64_t frame_ = 0, remembered_at_ = 0, applied_ = 0;
  bool logged_ = false, remember_logged_ = false;
};

}  // namespace native_x_renderer
