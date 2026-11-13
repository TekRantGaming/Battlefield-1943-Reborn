// NativeXRenderer - see taa.h.

#include "taa.h"

#include <rex/logging.h>

#include <cstring>
#include <vector>

#include "glsl.h"

namespace native_x_renderer {

using namespace Diligent;

namespace {

constexpr const char* kVS = R"(#version 450
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The resolve. The colour is this frame's (jittered by a fraction of a pixel, a different one each frame); the history
// is the last result, read where this pixel was a frame ago (the motion vectors: pixels, from this frame's place to the
// last's). The history is kept within what this frame shows around the pixel (its 3x3 neighbourhood's range, in a
// tone-mapped space so a bright pixel does not outweigh its neighbours), which is what stops ghosting; then a tenth
// of the new frame is blended in, so each pixel is the mean of about ten jittered samples.
constexpr const char* kPS = R"(#version 450
layout(std140, set = 0, binding = 0) uniform TaaConstants {
  vec4 size;    // width, height, 1 / width, 1 / height
  vec4 params;  // x: 1 when there is no history
};
layout(set = 0, binding = 1) uniform sampler2D colorTex;
layout(set = 0, binding = 2) uniform sampler2D historyTex;
layout(set = 0, binding = 3) uniform sampler2D motionTex;
layout(location = 0) out vec4 out_color;

vec3 Tone(vec3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }
vec3 Untone(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1.0 / 65536.0); }

void main() {
  const ivec2 p = ivec2(gl_FragCoord.xy);
  const ivec2 last = ivec2(size.xy) - 1;
  const vec4 center = texelFetch(colorTex, p, 0);
  const vec3 color = Tone(max(center.rgb, vec3(0.0)));
  vec3 lowest = color, highest = color;
  vec2 motion = texelFetch(motionTex, p, 0).rg;
  float longest = dot(motion, motion);
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      if (x == 0 && y == 0) continue;
      const ivec2 q = clamp(p + ivec2(x, y), ivec2(0), last);
      const vec3 c = Tone(max(texelFetch(colorTex, q, 0).rgb, vec3(0.0)));
      lowest = min(lowest, c);
      highest = max(highest, c);
      // The fastest motion around: a moving edge takes its own history, not the still background's.
      const vec2 m = texelFetch(motionTex, q, 0).rg;
      const float l = dot(m, m);
      if (l > longest) longest = l, motion = m;
    }
  }
  const vec2 uv = (gl_FragCoord.xy + motion) * size.zw;
  float weight = 0.1;
  if (params.x > 0.5 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) weight = 1.0;
  vec3 history = Tone(max(textureLod(historyTex, uv, 0.0).rgb, vec3(0.0)));
  history = clamp(history, lowest, highest);
  out_color = vec4(Untone(mix(history, color, weight)), center.a);
}
)";

RefCntAutoPtr<IShader> Shader(IRenderDevice* device, SHADER_TYPE type, const char* source, const char* name) {
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

}  // namespace

// The developer's shader check (aa.cpp's TestUtilityShaders): how many of the two failed to compile.
int TestTaaShaders() {
  return int(CompileGlsl(GlslStage::kVertex, kVS, "TAA VS").empty()) + int(CompileGlsl(GlslStage::kPixel, kPS, "TAA PS").empty());
}

bool Taa::Load(IRenderDevice* device) {
  device_ = device;
  vs_ = Shader(device, SHADER_TYPE_VERTEX, kVS, "NativeXRenderer TAA VS");
  ps_ = Shader(device, SHADER_TYPE_PIXEL, kPS, "NativeXRenderer TAA PS");
  BufferDesc constants;
  constants.Name = "NativeXRenderer TAA constants";
  constants.Size = 32;
  constants.Usage = USAGE_DYNAMIC;
  constants.BindFlags = BIND_UNIFORM_BUFFER;
  constants.CPUAccessFlags = CPU_ACCESS_WRITE;
  device->CreateBuffer(constants, nullptr, &constants_);
  if (!vs_ || !ps_ || !constants_) {
    REXLOG_ERROR("[native_x_renderer] TAA: its pass could not be set up");
    return false;
  }
  return true;
}

bool Taa::Dispatch(IDeviceContext* context, const Inputs& in) {
  if (!in.color || !in.motion || !in.output || !vs_ || !ps_) return false;
  const TextureDesc& out = in.output->GetDesc();
  if (!pso_ || format_ != out.Format) {
    pso_.Release(), srb_.Release();
    GraphicsPipelineStateCreateInfo info;
    info.PSODesc.Name = "NativeXRenderer TAA";
    info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
    info.GraphicsPipeline.NumRenderTargets = 1;
    info.GraphicsPipeline.RTVFormats[0] = out.Format;
    info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
    info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
    info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
    info.pVS = vs_;
    info.pPS = ps_;
    SamplerDesc point, linear;
    point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
    point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
    linear.MinFilter = linear.MagFilter = linear.MipFilter = FILTER_TYPE_LINEAR;
    linear.AddressU = linear.AddressV = linear.AddressW = TEXTURE_ADDRESS_CLAMP;
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "colorTex", point},
                                       {SHADER_TYPE_PIXEL, "historyTex", linear},
                                       {SHADER_TYPE_PIXEL, "motionTex", point}};
    info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
    info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    info.PSODesc.ResourceLayout.NumImmutableSamplers = 3;
    device_->CreateGraphicsPipelineState(info, &pso_);
    if (pso_) pso_->CreateShaderResourceBinding(&srb_, true);
    format_ = out.Format;
    if (!srb_) {
      REXLOG_ERROR("[native_x_renderer] TAA: no pipeline for the format {}", int(out.Format));
      return false;
    }
  }
  bool reset = in.reset;
  if (!history_ || history_->GetDesc().Width != out.Width || history_->GetDesc().Height != out.Height ||
      history_->GetDesc().Format != out.Format) {
    TextureDesc desc;
    desc.Name = "NativeXRenderer TAA history";
    desc.Type = RESOURCE_DIM_TEX_2D;
    desc.Width = out.Width;
    desc.Height = out.Height;
    desc.Format = out.Format;
    desc.BindFlags = BIND_SHADER_RESOURCE;
    history_.Release();
    device_->CreateTexture(desc, nullptr, &history_);
    if (!history_) return false;
    reset = true;
    REXLOG_INFO("[native_x_renderer] TAA: {}x{}, its history made", out.Width, out.Height);
  }
  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    const float k[8] = {float(out.Width), float(out.Height), 1.0f / float(out.Width), 1.0f / float(out.Height),
                        reset ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    std::memcpy(data, k, sizeof(k));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }
  auto set = [&](const char* name, IDeviceObject* object) {
    if (auto* v = srb_->GetVariableByName(SHADER_TYPE_PIXEL, name)) v->Set(object);
  };
  set("TaaConstants", constants_);
  set("colorTex", in.color->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  set("historyTex", history_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  set("motionTex", in.motion->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  ITextureView* target = in.output->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
  if (!target) return false;
  context->SetRenderTargets(1, &target, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  Viewport viewport;
  viewport.Width = float(out.Width);
  viewport.Height = float(out.Height);
  viewport.MaxDepth = 1.0f;
  context->SetViewports(1, &viewport, 0, 0);
  context->SetPipelineState(pso_);
  context->CommitShaderResources(srb_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DrawAttribs draw;
  draw.NumVertices = 3;
  context->Draw(draw);
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  CopyTextureAttribs copy{in.output, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, history_,
                          RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
  context->CopyTexture(copy);
  return true;
}

}  // namespace native_x_renderer
