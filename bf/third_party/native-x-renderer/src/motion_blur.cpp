// NativeXRenderer - see motion_blur.h.

#include "motion_blur.h"

#include <rex/logging.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "glsl.h"
#include "motion.h"

namespace native_x_renderer {

using namespace Diligent;

namespace {

constexpr const char* kVS = R"(#version 450
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* kConstants = R"(
layout(std140, set = 0, binding = 0) uniform BlurConstants {
  mat4 reprojection;  // clip, this frame -> clip, the last frame
  vec4 size;          // the view in the colour: width, height; in the depth: width, height
  vec4 depth;         // the depth at the near plane, at the far plane; 1 with a camera reprojection; unused
  vec4 blur;          // the motion's scale (exposure), the longest blur in colour pixels, the taps, unused
};
)";

// The camera's motion: where the last frame's camera put this pixel's point. In depth pixels, from this frame's place
// to the last frame's (the upscaler's convention, so DrawObjectMotion's draws go over it as they are). What is nearer
// than the view's near plane or beyond its far plane (Battlefield 1943's first person weapon, drawn with another
// camera into the same depth) has none.
constexpr const char* kVelocityPS1 = R"(#version 450
)";
constexpr const char* kVelocityPS2 = R"(
layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(location = 0) out vec2 out_motion;
void main() {
  const ivec2 pixel = ivec2(gl_FragCoord.xy);
  out_motion = vec2(0.0);
  if (depth.z < 0.5 || any(greaterThanEqual(vec2(pixel), size.zw))) return;
  const float z = (texelFetch(depthTex, pixel, 0).r - depth.x) / (depth.y - depth.x);
  if (z < 0.0 || z > 1.0) return;
  const vec2 uv = (vec2(pixel) + 0.5) / size.zw;
  const vec4 last = reprojection * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, z, 1.0);
  if (last.w <= 0.0) return;
  const vec2 last_uv = vec2(last.x / last.w * 0.5 + 0.5, 0.5 - last.y / last.w * 0.5);
  out_motion = (last_uv - uv) * size.zw;
}
)";

// The blur: the colour gathered along the pixel's motion, half before and half after it.
constexpr const char* kBlurPS1 = R"(#version 450
)";
constexpr const char* kBlurPS2 = R"(
layout(set = 0, binding = 1) uniform sampler2D colorTex;
layout(set = 0, binding = 2) uniform sampler2D motionTex;
layout(location = 0) out vec4 out_color;

// A pixel's blur, in colour pixels: its motion at the exposure, no longer than the longest allowed.
vec2 BlurOf(vec2 at) {
  const ivec2 q = ivec2(clamp(at, vec2(0.0), size.xy - 1.0) * (size.zw / size.xy));
  vec2 m = texelFetch(motionTex, q, 0).rg * (size.xy / size.zw) * blur.x;
  const float len = length(m);
  if (len > blur.y) m *= blur.y / len;
  return m;
}

void main() {
  const vec2 p = gl_FragCoord.xy;
  const vec4 center = texelFetch(colorTex, ivec2(p), 0);
  out_color = center;
  if (any(greaterThanEqual(p, size.xy))) return;
  const vec2 own = BlurOf(p);
  const float own_len = length(own);
  // The longest blur around (a moving thing's edge spreads over the still pixels beside it).
  vec2 widest = own;
  float widest_len = own_len;
  for (int i = 0; i < 4; ++i) {
    const vec2 o = vec2(i == 0 ? -1.0 : i == 1 ? 1.0 : 0.0, i == 2 ? -1.0 : i == 3 ? 1.0 : 0.0) * max(blur.y * 0.25, 2.0);
    const vec2 m = BlurOf(p + o);
    const float l = length(m);
    if (l > widest_len) widest = m, widest_len = l;
  }
  if (widest_len < 0.75) return;
  const int taps = int(blur.z);
  // Interleaved gradient noise: each pixel's taps start elsewhere along the line.
  const float noise = fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
  vec3 sum = max(center.rgb, vec3(0.0));
  float weight = 1.0;
  for (int i = 0; i < taps; ++i) {
    const float t = (float(i) + noise) / float(taps) - 0.5;
    const vec2 at = p + widest * t;
    if (any(lessThan(at, vec2(0.0))) || any(greaterThanEqual(at, size.xy))) continue;
    const float reach = length(widest * t);
    // A tap counts as far as its own blur reaches back to this pixel, or this pixel's own reaches it.
    const float theirs = length(BlurOf(at));
    const float w = clamp(max(theirs, own_len) * 0.5 - reach + 1.0, 0.0, 1.0);
    sum += max(textureLod(colorTex, at / vec2(textureSize(colorTex, 0)), 0.0).rgb, vec3(0.0)) * w;
    weight += w;
  }
  out_color = vec4(sum / weight, center.a);
}
)";

struct Constants {
  float reprojection[16];
  float size[4];
  float depth[4];
  float blur[4];
};

std::string Source(const char* head, const char* body) { return std::string(head) + kConstants + body; }

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

RefCntAutoPtr<IPipelineState> Pipeline(IRenderDevice* device, const char* name, IShader* vs, IShader* ps,
                                       TEXTURE_FORMAT format, const char* const* textures, const bool* linear, int count) {
  GraphicsPipelineStateCreateInfo info;
  info.PSODesc.Name = name;
  info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
  info.GraphicsPipeline.NumRenderTargets = 1;
  info.GraphicsPipeline.RTVFormats[0] = format;
  info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
  info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
  info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
  info.pVS = vs;
  info.pPS = ps;
  SamplerDesc point_desc, linear_desc;
  point_desc.MinFilter = point_desc.MagFilter = point_desc.MipFilter = FILTER_TYPE_POINT;
  point_desc.AddressU = point_desc.AddressV = point_desc.AddressW = TEXTURE_ADDRESS_CLAMP;
  linear_desc.MinFilter = linear_desc.MagFilter = linear_desc.MipFilter = FILTER_TYPE_LINEAR;
  linear_desc.AddressU = linear_desc.AddressV = linear_desc.AddressW = TEXTURE_ADDRESS_CLAMP;
  ImmutableSamplerDesc samplers[4];
  for (int i = 0; i < count; ++i) samplers[i] = {SHADER_TYPE_PIXEL, textures[i], linear[i] ? linear_desc : point_desc};
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
  info.PSODesc.ResourceLayout.NumImmutableSamplers = uint32_t(count);
  RefCntAutoPtr<IPipelineState> pso;
  device->CreateGraphicsPipelineState(info, &pso);
  return pso;
}

}  // namespace

int TestMotionBlurShaders() {
  return int(CompileGlsl(GlslStage::kVertex, kVS, "motion blur VS").empty()) +
         int(CompileGlsl(GlslStage::kPixel, Source(kVelocityPS1, kVelocityPS2).c_str(), "motion blur velocity PS").empty()) +
         int(CompileGlsl(GlslStage::kPixel, Source(kBlurPS1, kBlurPS2).c_str(), "motion blur PS").empty());
}

bool MotionBlur::Load(IRenderDevice* device) {
  device_ = device;
  vs_ = Shader(device, SHADER_TYPE_VERTEX, kVS, "NativeXRenderer motion blur VS");
  velocity_ps_ = Shader(device, SHADER_TYPE_PIXEL, Source(kVelocityPS1, kVelocityPS2), "NativeXRenderer motion blur velocity PS");
  blur_ps_ = Shader(device, SHADER_TYPE_PIXEL, Source(kBlurPS1, kBlurPS2), "NativeXRenderer motion blur PS");
  BufferDesc constants;
  constants.Name = "NativeXRenderer motion blur constants";
  constants.Size = sizeof(Constants);
  constants.Usage = USAGE_DYNAMIC;
  constants.BindFlags = BIND_UNIFORM_BUFFER;
  constants.CPUAccessFlags = CPU_ACCESS_WRITE;
  device->CreateBuffer(constants, nullptr, &constants_);
  if (vs_ && velocity_ps_) {
    static const char* const kTextures[] = {"depthTex"};
    static const bool kLinear[] = {false};
    velocity_pso_ = Pipeline(device, "NativeXRenderer motion blur velocity", vs_, velocity_ps_, kMotionVectorFormat, kTextures, kLinear, 1);
    if (velocity_pso_) velocity_pso_->CreateShaderResourceBinding(&velocity_srb_, true);
  }
  if (!vs_ || !velocity_ps_ || !blur_ps_ || !constants_ || !velocity_srb_) {
    REXLOG_ERROR("[native_x_renderer] motion blur: its passes could not be set up");
    failed_ = true;
    return false;
  }
  return true;
}

bool MotionBlur::Apply(IDeviceContext* context, const Inputs& in) {
  if (failed_ || !device_ || !in.color || !in.depth || !in.depth_srv || !in.width || !in.height || !in.depth_width ||
      !in.depth_height || in.strength <= 0.0f) {
    return false;
  }
  const TextureDesc& color = in.color->GetDesc();
  const TextureDesc& depth = in.depth->GetDesc();
  if (!blur_pso_ || format_ != color.Format) {
    blur_pso_.Release(), blur_srb_.Release();
    static const char* const kTextures[] = {"colorTex", "motionTex"};
    static const bool kLinear[] = {true, false};
    blur_pso_ = Pipeline(device_, "NativeXRenderer motion blur", vs_, blur_ps_, color.Format, kTextures, kLinear, 2);
    if (blur_pso_) blur_pso_->CreateShaderResourceBinding(&blur_srb_, true);
    format_ = color.Format;
    if (!blur_srb_) {
      REXLOG_ERROR("[native_x_renderer] motion blur: no pipeline for the format {}", int(color.Format));
      failed_ = true;
      return false;
    }
  }
  auto ensure = [&](RefCntAutoPtr<ITexture>& texture, const char* name, uint32_t width, uint32_t height, TEXTURE_FORMAT format,
                    BIND_FLAGS bind) {
    if (texture && texture->GetDesc().Width == width && texture->GetDesc().Height == height && texture->GetDesc().Format == format)
      return true;
    TextureDesc desc;
    desc.Name = name;
    desc.Type = RESOURCE_DIM_TEX_2D;
    desc.Width = width;
    desc.Height = height;
    desc.Format = format;
    desc.BindFlags = bind;
    texture.Release();
    device_->CreateTexture(desc, nullptr, &texture);
    return bool(texture);
  };
  // The motion at the depth's size (the objects' own motion is drawn there, against that depth).
  const bool made = !velocity_ || !source_;
  if (!ensure(velocity_, "NativeXRenderer motion blur motion", depth.Width, depth.Height, kMotionVectorFormat,
              BIND_SHADER_RESOURCE | BIND_RENDER_TARGET) ||
      !ensure(source_, "NativeXRenderer motion blur source", color.Width, color.Height, color.Format, BIND_SHADER_RESOURCE)) {
    REXLOG_ERROR("[native_x_renderer] motion blur: its textures could not be made");
    failed_ = true;
    return false;
  }
  if (made)
    REXLOG_INFO("[native_x_renderer] motion blur: the view {}x{} (depth {}x{}), {} taps, strength {:.0f}%, objects' own motion {}",
                in.width, in.height, in.depth_width, in.depth_height, in.samples, in.strength * 100.0f,
                in.object_motion ? "on" : "off");
  Constants k{};
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) k.reprojection[r * 4 + c] = in.reprojection ? float((*in.reprojection)[c * 4 + r]) : (r == c ? 1.0f : 0.0f);
  k.size[0] = float(in.width), k.size[1] = float(in.height), k.size[2] = float(in.depth_width), k.size[3] = float(in.depth_height);
  k.depth[0] = in.z_min, k.depth[1] = in.z_max != in.z_min ? in.z_max : in.z_min + 1.0f, k.depth[2] = in.reprojection ? 1.0f : 0.0f;
  // A frame's motion brought to the exposure: a sixtieth of a second at full strength.
  const float frame = std::clamp(in.frame_seconds, 1.0f / 1000.0f, 1.0f / 15.0f);
  k.blur[0] = in.strength * (1.0f / 60.0f) / frame;
  k.blur[1] = float(in.height) * 0.06f;
  k.blur[2] = float(std::clamp(in.samples, 4, 32));
  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, &k, sizeof(k));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }
  // ---- the motion
  ITextureView* motion_rtv = velocity_->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
  context->SetRenderTargets(1, &motion_rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  Viewport viewport;
  viewport.Width = float(depth.Width);
  viewport.Height = float(depth.Height);
  viewport.MaxDepth = 1.0f;
  context->SetViewports(1, &viewport, 0, 0);
  context->SetPipelineState(velocity_pso_);
  if (auto* v = velocity_srb_->GetVariableByName(SHADER_TYPE_PIXEL, "BlurConstants")) v->Set(constants_);
  if (auto* v = velocity_srb_->GetVariableByName(SHADER_TYPE_PIXEL, "depthTex")) v->Set(in.depth_srv);
  context->CommitShaderResources(velocity_srb_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DrawAttribs draw;
  draw.NumVertices = 3;
  context->Draw(draw);
  if (in.object_motion) in.object_motion(motion_rtv);
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  // ---- the blur, from a copy of the colour into the colour
  CopyTextureAttribs copy{in.color, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, source_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
  context->CopyTexture(copy);
  ITextureView* target = in.color->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
  if (!target) return false;
  context->SetRenderTargets(1, &target, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  viewport.Width = float(color.Width);
  viewport.Height = float(color.Height);
  context->SetViewports(1, &viewport, 0, 0);
  context->SetPipelineState(blur_pso_);
  auto set = [&](const char* name, IDeviceObject* object) {
    if (auto* v = blur_srb_->GetVariableByName(SHADER_TYPE_PIXEL, name)) v->Set(object);
  };
  set("BlurConstants", constants_);
  set("colorTex", source_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  set("motionTex", velocity_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  context->CommitShaderResources(blur_srb_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->Draw(draw);
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  ++applied_;
  return true;
}

}  // namespace native_x_renderer
