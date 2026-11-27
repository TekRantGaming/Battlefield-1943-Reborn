// NativeXRenderer - see dof.h.

#include "dof.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "glsl.h"

// The user, 18 Nov 2026: "an ADS Depth of Field strength toggle in options with Low, Default, Strong settings".
REXCVAR_DEFINE_STRING(native_x_renderer_ads_dof_strength, "default", "NativeXRenderer",
                      "ADS depth of field: its strength, low, default or strong (0.6, 1 or 1.6 times the blur); taken at once");

namespace native_x_renderer {

using namespace Diligent;

namespace {

std::atomic<float> g_ads_blend{0.0f};
std::atomic<int64_t> g_ads_until{0};  // steady clock ticks

constexpr const char* kVS = R"(#version 450
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Each pass's constants, the same for all.
constexpr const char* kConstants = R"(#version 450
layout(std140, set = 0, binding = 0) uniform DofConstants {
  vec4 size;   // the view in the colour: width, height; in the depth: width, height
  vec4 lens;   // the focus (metres: no blur from there on), the nearest (the most blur), the largest radius in colour pixels, the taps
  vec4 half_size;  // the half size textures: width, height, 1 / width, 1 / height
  vec4 sights;  // the sights' clear middle: its radius and where the blur is whole (shares of the height); the bands (NXR_DOF_SHOW)
};
float MetresAt(sampler2D depthTex, vec2 at) {
  const ivec2 q = ivec2(clamp(at, vec2(0.0), size.xy - 1.0) * (size.zw / size.xy));
  return 0.1 / max(texelFetch(depthTex, q, 0).r, 1e-6);
}
// A pixel's blur radius in colour pixels, from its distance (the stored depth is 0.1 / metres): none from the focus
// on, the most at the nearest and nearer, smoothly between (no edge where the blur begins); and none in the middle of
// the view, where the sights are lined up, more away from it.
float RadiusAt(sampler2D depthTex, vec2 at) {
  const float t = clamp((lens.x - MetresAt(depthTex, at)) / (lens.x - lens.y), 0.0, 1.0);
  const float away = smoothstep(sights.x, sights.y, length(at - size.xy * 0.5) / size.y);
  return t * t * (3.0 - 2.0 * t) * away * lens.z;
}
)";

// 1. The colour and the blur radius at half size: each texel of four of the view's pixels, the radius their largest,
// the colour leaning to the blurred ones (a soft gun's edge keeps the gun's colour, not the sky's beside it).
constexpr const char* kPrepPS = R"(
layout(set = 0, binding = 1) uniform sampler2D colorTex;
layout(set = 0, binding = 2) uniform sampler2D depthTex;
layout(location = 0) out vec4 out_color;
void main() {
  const ivec2 h = ivec2(gl_FragCoord.xy);
  vec3 sum = vec3(0.0);
  float weight = 0.0, largest = 0.0;
  for (int i = 0; i < 4; ++i) {
    const ivec2 p = min(h * 2 + ivec2(i & 1, i >> 1), ivec2(size.xy) - 1);
    const float r = RadiusAt(depthTex, vec2(p) + 0.5);
    const float w = 0.2 + r / max(lens.z, 1e-3);
    sum += max(texelFetch(colorTex, p, 0).rgb, vec3(0.0)) * w;
    weight += w;
    largest = max(largest, r);
  }
  out_color = vec4(sum / weight, largest);
}
)";

// 2. The gather, at half size: `taps` points evenly over a disc of the largest radius (a Vogel spiral, the golden
// angle between taps, the same in every pixel and frame: no grain, no shimmer). A tap counts where its own blur
// reaches this texel (a soft thing spreads over what is beside it), or where this texel and the tap are both soft
// (inside the soft thing: the sharp world behind its edge does not leak in). Out: the colour, and how much of it
// shows (all of it in a soft texel; by the share of taps that reach a sharp one).
constexpr const char* kGatherPS = R"(
layout(set = 0, binding = 1) uniform sampler2D halfTex;
layout(location = 0) out vec4 out_color;
void main() {
  const vec2 p = gl_FragCoord.xy;
  const vec4 center = textureLod(halfTex, p * half_size.zw, 0.0);
  const float largest = lens.z * 0.5;  // in half size texels
  const float own = center.a * 0.5;
  const int taps = int(lens.w);
  // The texel itself only where it is soft (beside a soft thing, a sharp texel's own colour is the view's, under it).
  const float self = 0.5 * clamp(own * 2.0, 0.0, 1.0);
  vec3 sum = center.rgb * self;
  float weight = self + 1e-4, covered = 0.0;
  for (int i = 0; i < taps; ++i) {
    const float r = largest * sqrt((float(i) + 0.5) / float(taps));
    const float angle = float(i) * 2.39996323;
    const vec2 at = p + r * vec2(cos(angle), sin(angle));
    const vec4 s = textureLod(halfTex, at * half_size.zw, 0.0);
    const float theirs = s.a * 0.5;
    const float reach = clamp(theirs - r + 1.0, 0.0, 1.0);
    const float inside = clamp(own - r + 1.0, 0.0, 1.0) * clamp(theirs * 2.0, 0.0, 1.0);
    const float w = max(reach, inside);
    sum += s.rgb * w;
    weight += w;
    covered += reach * step(0.5, theirs);
  }
  const float show = max(clamp(own * 2.0, 0.0, 1.0), clamp(covered / float(taps) * 2.0, 0.0, 1.0));
  out_color = vec4(sum / weight, show);
}
)";

// 3. A small tent over the gathered texels (premultiplied by how much shows), to smooth what is left of the taps.
constexpr const char* kFillPS = R"(
layout(set = 0, binding = 1) uniform sampler2D blurTex;
layout(location = 0) out vec4 out_color;
void main() {
  const vec2 uv = gl_FragCoord.xy * half_size.zw;
  vec4 sum = vec4(0.0);
  for (int i = 0; i < 4; ++i) {
    const vec2 o = vec2((i & 1) != 0 ? 0.75 : -0.75, (i & 2) != 0 ? 0.75 : -0.75);
    const vec4 s = textureLod(blurTex, uv + o * half_size.zw, 0.0);
    sum += vec4(s.rgb * s.a, s.a);
  }
  out_color = sum * 0.25;
}
)";

// 4. Over the view, at full size: the smoothed blur where it shows, by this pixel's own radius from the full size
// depth (the sharp edges stay sharp) or by the blur spreading over it; blended over the colour as it is.
constexpr const char* kCompositePS = R"(
layout(set = 0, binding = 1) uniform sampler2D fillTex;
layout(set = 0, binding = 2) uniform sampler2D depthTex;
layout(location = 0) out vec4 out_color;
void main() {
  const vec2 p = gl_FragCoord.xy;
  if (sights.z > 0.5 && (sights.z < 1.5 || p.x < size.x * 0.5)) {  // NXR_DOF_SHOW: the distance in bands, under 3 m (2: the left half)
    const float m = MetresAt(depthTex, p);
    if (m >= 3.0) discard;
    const vec3 bands[8] = vec3[8](vec3(1, 0, 0), vec3(1, 0.5, 0), vec3(1, 1, 0), vec3(0, 1, 0), vec3(0, 1, 1),
                                  vec3(0, 0, 1), vec3(1, 0, 1), vec3(1, 1, 1));
    const float edges[7] = float[7](0.15, 0.25, 0.35, 0.5, 0.7, 1.0, 1.4);
    int i = 0;
    while (i < 7 && m >= edges[i]) ++i;
    out_color = vec4(bands[i], 1.0);
    return;
  }
  const float own = RadiusAt(depthTex, p);
  const vec4 b = textureLod(fillTex, p * 0.5 * half_size.zw, 0.0);
  const float show = max(clamp(own - 0.25, 0.0, 1.0), b.a);
  if (show < 1.0 / 255.0 || b.a < 1e-4) discard;
  out_color = vec4(b.rgb / max(b.a, 1e-4), show);
}
)";

std::string Source(const char* body) { return std::string(kConstants) + body; }

RefCntAutoPtr<IShader> Shader(IRenderDevice* device, SHADER_TYPE type, const std::string& source, const char* name) {
  RefCntAutoPtr<IShader> shader;
  std::vector<uint32_t> spirv =
      CompileGlsl(type == SHADER_TYPE_VERTEX ? GlslStage::kVertex : GlslStage::kPixel, source.c_str(), name);
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

RefCntAutoPtr<IPipelineState> Pipeline(IRenderDevice* device, const char* name, IShader* vs, IShader* ps, TEXTURE_FORMAT format,
                                       const char* const* textures, const bool* linear, int count, bool blend) {
  GraphicsPipelineStateCreateInfo info;
  info.PSODesc.Name = name;
  info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
  info.GraphicsPipeline.NumRenderTargets = 1;
  info.GraphicsPipeline.RTVFormats[0] = format;
  info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
  info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
  info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
  if (blend) {  // over the colour by how much shows; the colour's alpha kept
    RenderTargetBlendDesc& rt = info.GraphicsPipeline.BlendDesc.RenderTargets[0];
    rt.BlendEnable = True;
    rt.SrcBlend = BLEND_FACTOR_SRC_ALPHA;
    rt.DestBlend = BLEND_FACTOR_INV_SRC_ALPHA;
    rt.BlendOp = BLEND_OPERATION_ADD;
    rt.SrcBlendAlpha = BLEND_FACTOR_ZERO;
    rt.DestBlendAlpha = BLEND_FACTOR_ONE;
    rt.BlendOpAlpha = BLEND_OPERATION_ADD;
    rt.RenderTargetWriteMask = COLOR_MASK_RGB;
  }
  info.pVS = vs;
  info.pPS = ps;
  SamplerDesc point_desc, linear_desc;
  point_desc.MinFilter = point_desc.MagFilter = point_desc.MipFilter = FILTER_TYPE_POINT;
  point_desc.AddressU = point_desc.AddressV = point_desc.AddressW = TEXTURE_ADDRESS_CLAMP;
  linear_desc.MinFilter = linear_desc.MagFilter = linear_desc.MipFilter = FILTER_TYPE_LINEAR;
  linear_desc.AddressU = linear_desc.AddressV = linear_desc.AddressW = TEXTURE_ADDRESS_CLAMP;
  ImmutableSamplerDesc samplers[2];
  for (int i = 0; i < count; ++i) samplers[i] = {SHADER_TYPE_PIXEL, textures[i], linear[i] ? linear_desc : point_desc};
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
  info.PSODesc.ResourceLayout.NumImmutableSamplers = uint32_t(count);
  RefCntAutoPtr<IPipelineState> pso;
  device->CreateGraphicsPipelineState(info, &pso);
  return pso;
}

constexpr TEXTURE_FORMAT kHalfFormat = TEX_FORMAT_RGBA16_FLOAT;

}  // namespace

void RecordAdsBlend(float blend) {
  g_ads_blend.store(std::clamp(blend, 0.0f, 1.0f), std::memory_order_relaxed);
  g_ads_until.store((std::chrono::steady_clock::now() + std::chrono::milliseconds(250)).time_since_epoch().count(),
                    std::memory_order_relaxed);
}

float AdsBlendNow() {
  if (g_ads_until.load(std::memory_order_relaxed) < std::chrono::steady_clock::now().time_since_epoch().count()) return 0.0f;
  return g_ads_blend.load(std::memory_order_relaxed);
}

int TestDofShaders() {
  return int(CompileGlsl(GlslStage::kVertex, kVS, "depth of field VS").empty()) +
         int(CompileGlsl(GlslStage::kPixel, Source(kPrepPS), "depth of field half size PS").empty()) +
         int(CompileGlsl(GlslStage::kPixel, Source(kGatherPS), "depth of field gather PS").empty()) +
         int(CompileGlsl(GlslStage::kPixel, Source(kFillPS), "depth of field fill PS").empty()) +
         int(CompileGlsl(GlslStage::kPixel, Source(kCompositePS), "depth of field composite PS").empty());
}

bool DepthOfField::Load(IRenderDevice* device) {
  device_ = device;
  vs_ = Shader(device, SHADER_TYPE_VERTEX, kVS, "NativeXRenderer depth of field VS");
  RefCntAutoPtr<IShader> prep = Shader(device, SHADER_TYPE_PIXEL, Source(kPrepPS), "NativeXRenderer depth of field half size PS");
  RefCntAutoPtr<IShader> gather = Shader(device, SHADER_TYPE_PIXEL, Source(kGatherPS), "NativeXRenderer depth of field gather PS");
  RefCntAutoPtr<IShader> fill = Shader(device, SHADER_TYPE_PIXEL, Source(kFillPS), "NativeXRenderer depth of field fill PS");
  composite_ps_ = Shader(device, SHADER_TYPE_PIXEL, Source(kCompositePS), "NativeXRenderer depth of field composite PS");
  BufferDesc constants;
  constants.Name = "NativeXRenderer depth of field constants";
  constants.Size = 64;
  constants.Usage = USAGE_DYNAMIC;
  constants.BindFlags = BIND_UNIFORM_BUFFER;
  constants.CPUAccessFlags = CPU_ACCESS_WRITE;
  device->CreateBuffer(constants, nullptr, &constants_);
  if (vs_ && prep && gather && fill) {
    static const char* const kPrep[] = {"colorTex", "depthTex"};
    static const bool kPrepLinear[] = {false, false};
    static const char* const kGather[] = {"halfTex"};
    static const char* const kFill[] = {"blurTex"};
    static const bool kLinear[] = {true};
    passes_[0].pso = Pipeline(device, "NativeXRenderer depth of field half size", vs_, prep, kHalfFormat, kPrep, kPrepLinear, 2, false);
    passes_[1].pso = Pipeline(device, "NativeXRenderer depth of field gather", vs_, gather, kHalfFormat, kGather, kLinear, 1, false);
    passes_[2].pso = Pipeline(device, "NativeXRenderer depth of field fill", vs_, fill, kHalfFormat, kFill, kLinear, 1, false);
    for (Pass& pass : passes_)
      if (pass.pso) pass.pso->CreateShaderResourceBinding(&pass.srb, true);
  }
  if (!vs_ || !composite_ps_ || !constants_ || !passes_[0].srb || !passes_[1].srb || !passes_[2].srb) {
    REXLOG_ERROR("[native_x_renderer] depth of field: its passes could not be set up");
    failed_ = true;
    return false;
  }
  return true;
}

bool DepthOfField::Apply(IDeviceContext* context, const Inputs& in) {
  if (failed_ || !device_ || !in.color || !in.depth || !in.depth_srv || !in.width || !in.height || !in.depth_width ||
      !in.depth_height || in.blend <= 0.01f) {
    return false;
  }
  const TextureDesc& color = in.color->GetDesc();
  if (!composite_.pso || format_ != color.Format) {
    composite_.pso.Release(), composite_.srb.Release();
    static const char* const kTextures[] = {"fillTex", "depthTex"};
    static const bool kLinear[] = {true, false};
    composite_.pso = Pipeline(device_, "NativeXRenderer depth of field composite", vs_, composite_ps_, color.Format, kTextures,
                              kLinear, 2, true);
    if (composite_.pso) composite_.pso->CreateShaderResourceBinding(&composite_.srb, true);
    format_ = color.Format;
    if (!composite_.srb) {
      REXLOG_ERROR("[native_x_renderer] depth of field: no pipeline for the format {}", int(color.Format));
      failed_ = true;
      return false;
    }
  }
  const uint32_t half_width = (in.width + 1) / 2, half_height = (in.height + 1) / 2;
  auto ensure = [&](RefCntAutoPtr<ITexture>& texture, const char* name) {
    if (texture && texture->GetDesc().Width == half_width && texture->GetDesc().Height == half_height) return true;
    TextureDesc desc;
    desc.Name = name;
    desc.Type = RESOURCE_DIM_TEX_2D;
    desc.Width = half_width;
    desc.Height = half_height;
    desc.Format = kHalfFormat;
    desc.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
    texture.Release();
    device_->CreateTexture(desc, nullptr, &texture);
    return bool(texture);
  };
  if (!ensure(half_, "NativeXRenderer depth of field half size") || !ensure(blur_, "NativeXRenderer depth of field blur")) {
    REXLOG_ERROR("[native_x_renderer] depth of field: its {}x{} textures could not be made", half_width, half_height);
    failed_ = true;
    return false;
  }
  // The lens: sharp from 0.7 m on (a front sight), the most blur at 0.15 m and nearer (the back of the gun at the
  // eye), up to 1.1% of the view's height times the strength (Low 0.6, Strong 1.6); none within 26% of the height of
  // the middle (the sights lined up there: the Garand's ring reaches about 25%), whole from 40%.
  const std::string strength = REXCVAR_GET(native_x_renderer_ads_dof_strength);
  const float scale = strength == "low" ? 0.6f : strength == "strong" ? 1.6f : 1.0f;
  const float radius = float(in.height) * 0.011f * scale * in.blend;
  const int taps = std::clamp(in.taps, 8, 128);
  if (!said_) {
    said_ = true;
    REXLOG_INFO("[native_x_renderer] depth of field: the view {}x{} (depth {}x{}), gathered at {}x{}, {} taps, up to {:.1f} pixels",
                in.width, in.height, in.depth_width, in.depth_height, half_width, half_height, taps, float(in.height) * 0.011f);
  }
  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    static const int show = [] {
      const char* v = std::getenv("NXR_DOF_SHOW");
      return v && *v ? std::atoi(v) : 0;
    }();
    const float k[16] = {float(in.width), float(in.height), float(in.depth_width), float(in.depth_height),
                         0.7f, 0.15f, radius, float(taps),
                         float(half_width), float(half_height), 1.0f / float(half_width), 1.0f / float(half_height),
                         0.26f, 0.40f, float(show), 0.0f};
    std::memcpy(data, k, sizeof(k));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }
  DrawAttribs draw;
  draw.NumVertices = 3;
  auto run = [&](Pass& pass, ITexture* target, uint32_t width, uint32_t height,
                 std::initializer_list<std::pair<const char*, IDeviceObject*>> inputs) {
    ITextureView* rtv = target->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    if (!rtv) return false;
    context->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Viewport viewport;
    viewport.Width = float(width);
    viewport.Height = float(height);
    viewport.MaxDepth = 1.0f;
    context->SetViewports(1, &viewport, 0, 0);
    context->SetPipelineState(pass.pso);
    if (auto* v = pass.srb->GetVariableByName(SHADER_TYPE_PIXEL, "DofConstants")) v->Set(constants_);
    for (const auto& [name, object] : inputs)
      if (auto* v = pass.srb->GetVariableByName(SHADER_TYPE_PIXEL, name)) v->Set(object);
    context->CommitShaderResources(pass.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->Draw(draw);
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
    return true;
  };
  ITextureView* color_srv = in.color->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  if (!color_srv) return false;
  ITextureView* half_srv = half_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* blur_srv = blur_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  // The half size colour and radius; the gather into the blur; the fill back into the half size texture (its colour
  // no longer needed); the result over the colour.
  return run(passes_[0], half_, half_width, half_height, {{"colorTex", color_srv}, {"depthTex", in.depth_srv}}) &&
         run(passes_[1], blur_, half_width, half_height, {{"halfTex", half_srv}}) &&
         run(passes_[2], half_, half_width, half_height, {{"blurTex", blur_srv}}) &&
         run(composite_, in.color, in.width, in.height, {{"fillTex", half_srv}, {"depthTex", in.depth_srv}});
}

}  // namespace native_x_renderer
