// NativeXRenderer - see aa.h.

#include "aa.h"

#include "glsl.h"

#include <rex/logging.h>

#include <cstring>
#include <vector>

#include "AreaTex.h"
#include "SearchTex.h"
#include "cmaa_shaders.h"
#include "smaa_source.h"  // kSmaaSource: thirdparty/smaa/SMAA.hlsl, embedded by CMake

namespace native_x_renderer {

using namespace Diligent;

namespace {

// One triangle over the viewport; v_texcoord is 0..1 across it, top left 0, 0 (DiligentCore's Vulkan
// backend flips Y, D19).
constexpr const char* kFullScreen = R"(
layout(location = 0) out vec2 v_texcoord;
void FullScreen() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  v_texcoord = vec2(p.x, 1.0 - p.y);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* kConstants = R"(
layout(set = 0, binding = 0) uniform AAConstants {
  vec4 rt_metrics;  // 1 / width, 1 / height, width, height
};
)";

// The reference implementation's preset (SMAA_PRESET_LOW, MEDIUM, HIGH or ULTRA: the edge threshold and how far
// the searches go), from the mode's name (ParseMode: smaa_low, smaa_medium, smaa or smaa_high, smaa_ultra).
const char* g_smaa_preset = "HIGH";
// CMAA 2's edge threshold (its presets: Low 0.15, Medium 0.10, High 0.07, Ultra 0.05), from the mode's name.
float g_cmaa_threshold = 0.07f;

std::string SmaaShader(bool vertex, const char* body) {
  std::string source = std::string("#version 450\n#define SMAA_GLSL_4\n#define SMAA_PRESET_") + g_smaa_preset +
                       "\n#define SMAA_RT_METRICS rt_metrics\n";
  source += vertex ? "#define SMAA_INCLUDE_PS 0\n" : "#define SMAA_INCLUDE_VS 0\n";
  source += kConstants;
  source += kSmaaSource;
  source += "\n";
  if (vertex) source += kFullScreen;
  source += body;
  return source;
}

constexpr const char* kSmaaEdgesVS = R"(
layout(location = 1) out vec4 v_offset[3];
void main() {
  FullScreen();
  SMAAEdgeDetectionVS(v_texcoord, v_offset);
}
)";
constexpr const char* kSmaaEdgesPS = R"(
layout(set = 0, binding = 1) uniform sampler2D colorTex;
layout(location = 0) in vec2 v_texcoord;
layout(location = 1) in vec4 v_offset[3];
layout(location = 0) out vec4 out_edges;
void main() {
  out_edges = vec4(SMAALumaEdgeDetectionPS(v_texcoord, v_offset, colorTex), 0.0, 0.0);
}
)";
constexpr const char* kSmaaWeightsVS = R"(
layout(location = 1) out vec4 v_offset[3];
layout(location = 4) out vec2 v_pixcoord;
void main() {
  FullScreen();
  SMAABlendingWeightCalculationVS(v_texcoord, v_pixcoord, v_offset);
}
)";
constexpr const char* kSmaaWeightsPS = R"(
layout(set = 0, binding = 1) uniform sampler2D edgesTex;
layout(set = 0, binding = 2) uniform sampler2D areaTex;
layout(set = 0, binding = 3) uniform sampler2D searchTex;
layout(location = 0) in vec2 v_texcoord;
layout(location = 1) in vec4 v_offset[3];
layout(location = 4) in vec2 v_pixcoord;
layout(location = 0) out vec4 out_weights;
void main() {
  out_weights = SMAABlendingWeightCalculationPS(v_texcoord, v_pixcoord, v_offset, edgesTex, areaTex, searchTex,
                                                vec4(0.0));
}
)";
constexpr const char* kSmaaBlendVS = R"(
layout(location = 1) out vec4 v_offset;
void main() {
  FullScreen();
  SMAANeighborhoodBlendingVS(v_texcoord, v_offset);
}
)";
constexpr const char* kSmaaBlendPS = R"(
layout(set = 0, binding = 1) uniform sampler2D colorTex;
layout(set = 0, binding = 2) uniform sampler2D blendTex;
layout(location = 0) in vec2 v_texcoord;
layout(location = 1) in vec4 v_offset;
layout(location = 0) out vec4 out_color;
void main() {
  out_color = SMAANeighborhoodBlendingPS(v_texcoord, v_offset, colorTex, blendTex);
}
)";

constexpr const char* kFxaaVS = R"(
void main() {
  FullScreen();
}
)";
// The SDK's FXAA (its Vulkan presenter's GetSwapFxaaComputeSource, the normal quality), as a pixel shader
// that computes the luma itself (the SDK's gamma pass puts it in alpha) and keeps the source's alpha.
constexpr const char* kFxaaPS = R"(
layout(set = 0, binding = 1) uniform sampler2D colorTex;
layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 out_color;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);
const float kEdgeThreshold = 0.166;
const float kEdgeThresholdMin = 0.0833;
const float kSpanMax = 8.0;
const float kDirReduceMul = 0.125;
const float kDirReduceMin = 1.0 / 128.0;

float SampleLuma(vec2 uv) {
  return dot(textureLod(colorTex, uv, 0.0).rgb, kLumaWeights);
}

void main() {
  vec2 uv = v_texcoord;
  vec2 texel = rt_metrics.xy;
  vec4 rgbm = textureLod(colorTex, uv, 0.0);

  float lumaM = dot(rgbm.rgb, kLumaWeights);
  float lumaNW = SampleLuma(uv + vec2(-texel.x, -texel.y));
  float lumaNE = SampleLuma(uv + vec2( texel.x, -texel.y));
  float lumaSW = SampleLuma(uv + vec2(-texel.x,  texel.y));
  float lumaSE = SampleLuma(uv + vec2( texel.x,  texel.y));

  float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
  float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
  float lumaRange = lumaMax - lumaMin;

  vec3 result = rgbm.rgb;
  if (lumaRange >= max(kEdgeThresholdMin, lumaMax * kEdgeThreshold)) {
    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * kDirReduceMul), kDirReduceMin);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-kSpanMax), vec2(kSpanMax)) * texel;

    vec3 rgbA = 0.5 * (textureLod(colorTex, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
                       textureLod(colorTex, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (textureLod(colorTex, uv + dir * -0.5, 0.0).rgb +
                                     textureLod(colorTex, uv + dir * 0.5, 0.0).rgb);
    float lumaB = dot(rgbB, kLumaWeights);
    result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
  }
  out_color = vec4(result, rgbm.a);
}
)";

std::string FxaaShader(bool vertex) {
  std::string source = "#version 450\n";
  source += kConstants;
  if (vertex) {
    source += kFullScreen;
    source += kFxaaVS;
  } else {
    source += kFxaaPS;
  }
  return source;
}

RefCntAutoPtr<IShader> CreateShader(IRenderDevice* device, SHADER_TYPE type, const std::string& source,
                                    const char* name) {
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

RefCntAutoPtr<ITexture> CreateLookupTexture(IRenderDevice* device, const char* name, uint32_t width, uint32_t height,
                                            TEXTURE_FORMAT format, const void* data, uint32_t pitch) {
  TextureDesc desc;
  desc.Name = name;
  desc.Type = RESOURCE_DIM_TEX_2D;
  desc.Width = width;
  desc.Height = height;
  desc.Format = format;
  desc.Usage = USAGE_IMMUTABLE;
  desc.BindFlags = BIND_SHADER_RESOURCE;
  TextureSubResData subresource{data, pitch};
  TextureData initial{&subresource, 1};
  RefCntAutoPtr<ITexture> texture;
  device->CreateTexture(desc, &initial, &texture);
  return texture;
}

}  // namespace

Antialiasing::Mode Antialiasing::ParseMode(std::string_view name) {
  if (name == "off") return Mode::kOff;
  if (name == "fxaa") return Mode::kFxaa;
  if (name.substr(0, 4) == "cmaa") {
    g_cmaa_threshold = name == "cmaa_low" ? 0.15f : name == "cmaa_medium" ? 0.10f : name == "cmaa_ultra" ? 0.05f : 0.07f;
    return Mode::kCmaa;
  }
  if (name == "smaa_low") g_smaa_preset = "LOW";
  else if (name == "smaa_medium") g_smaa_preset = "MEDIUM";
  else if (name == "smaa_ultra") g_smaa_preset = "ULTRA";
  else g_smaa_preset = "HIGH";
  if (name != "smaa" && name != "smaa_low" && name != "smaa_medium" && name != "smaa_high" && name != "smaa_ultra")
    REXLOG_WARN("[native_x_renderer] antialiasing \"{}\" unknown: SMAA", name);
  return Mode::kSmaa;
}

const char* Antialiasing::ModeName(Mode mode) {
  switch (mode) {
    case Mode::kFxaa:
      return "FXAA";
    case Mode::kSmaa:
      return "SMAA";
    case Mode::kCmaa:
      return "CMAA 2";
    default:
      return "off";
  }
}

bool Antialiasing::Initialize(IRenderDevice* device, Mode mode) {
  // Called again when the method is changed while the game runs: the last method's passes and textures go (the GPU
  // keeps what a frame in flight still uses).
  smaa_edges_ = smaa_weights_ = cmaa_edges_ = cmaa_args_ = cmaa_process_ = cmaa_apply_ = Pass{};
  output_passes_.clear();
  for (Textures& t : textures_) t = Textures{};
  area_.Release(), search_.Release(), constants_.Release();
  cmaa_constants_.Release(), cmaa_control_.Release(), cmaa_indirect_.Release();
  device_ = device;
  mode_ = mode;
  if (mode_ == Mode::kOff) return true;
  BufferDesc constants;
  constants.Name = "NativeXRenderer antialiasing constants";
  constants.Size = 16;
  constants.Usage = USAGE_DYNAMIC;
  constants.BindFlags = BIND_UNIFORM_BUFFER;
  constants.CPUAccessFlags = CPU_ACCESS_WRITE;
  device->CreateBuffer(constants, nullptr, &constants_);
  bool ok = constants_ != nullptr;
  if (ok && mode_ == Mode::kSmaa) {
    area_ = CreateLookupTexture(device, "NativeXRenderer SMAA area", AREATEX_WIDTH, AREATEX_HEIGHT, TEX_FORMAT_RG8_UNORM,
                                areaTexBytes, AREATEX_PITCH);
    search_ = CreateLookupTexture(device, "NativeXRenderer SMAA search", SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT,
                                  TEX_FORMAT_R8_UNORM, searchTexBytes, SEARCHTEX_PITCH);
    ok = area_ && search_ &&
         CreatePass(smaa_edges_, "NativeXRenderer SMAA edges", SmaaShader(true, kSmaaEdgesVS),
                    SmaaShader(false, kSmaaEdgesPS), {{"colorTex", true}}, TEX_FORMAT_RG8_UNORM) &&
         CreatePass(smaa_weights_, "NativeXRenderer SMAA weights", SmaaShader(true, kSmaaWeightsVS),
                    SmaaShader(false, kSmaaWeightsPS), {{"edgesTex", true}, {"areaTex", true}, {"searchTex", false}},
                    TEX_FORMAT_RGBA8_UNORM);
    if (ok) {
      smaa_weights_.srb->GetVariableByName(SHADER_TYPE_PIXEL, "areaTex")
          ->Set(area_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
      smaa_weights_.srb->GetVariableByName(SHADER_TYPE_PIXEL, "searchTex")
          ->Set(search_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    }
  }
  if (ok && mode_ == Mode::kCmaa) {
    BufferDesc k;
    k.Name = "NativeXRenderer CMAA constants";
    k.Size = 32;
    k.Usage = USAGE_DYNAMIC;
    k.BindFlags = BIND_UNIFORM_BUFFER;
    k.CPUAccessFlags = CPU_ACCESS_WRITE;
    device->CreateBuffer(k, nullptr, &cmaa_constants_);
    BufferDesc control;
    control.Name = "NativeXRenderer CMAA counters";
    control.Size = 64;
    control.Usage = USAGE_DEFAULT;
    control.BindFlags = BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE;
    control.Mode = BUFFER_MODE_STRUCTURED;
    control.ElementByteStride = 4;
    const uint32_t zero[16] = {};
    BufferData initial(zero, sizeof(zero));
    device->CreateBuffer(control, &initial, &cmaa_control_);
    BufferDesc indirect = control;
    indirect.Name = "NativeXRenderer CMAA indirect arguments";
    indirect.Size = 16;
    indirect.BindFlags = BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE | BIND_INDIRECT_DRAW_ARGS;
    BufferData initial_args(zero, 16);
    device->CreateBuffer(indirect, &initial_args, &cmaa_indirect_);
    ok = cmaa_constants_ && cmaa_control_ && cmaa_indirect_ &&
         CreateCompute(cmaa_edges_, "NativeXRenderer CMAA edges", kCmaaEdgesCS) &&
         CreateCompute(cmaa_args_, "NativeXRenderer CMAA arguments", kCmaaArgsCS) &&
         CreateCompute(cmaa_process_, "NativeXRenderer CMAA candidates", kCmaaProcessCS) &&
         CreateCompute(cmaa_apply_, "NativeXRenderer CMAA apply", kCmaaApplyCS);
  }
  if (!ok) {
    REXLOG_ERROR("[native_x_renderer] antialiasing ({}) could not be set up: the image is not antialiased",
                 ModeName(mode_));
    mode_ = Mode::kOff;
    return false;
  }
  REXLOG_INFO("[native_x_renderer] antialiasing: {}", ModeName(mode_));
  return true;
}

bool Antialiasing::CreatePass(Pass& pass, const char* name, const std::string& vertex, const std::string& pixel,
                              std::initializer_list<Input> inputs, TEXTURE_FORMAT target_format) {
  std::string vs_name = std::string(name) + " VS", ps_name = std::string(name) + " PS";
  RefCntAutoPtr<IShader> vs = CreateShader(device_, SHADER_TYPE_VERTEX, vertex, vs_name.c_str());
  RefCntAutoPtr<IShader> ps = CreateShader(device_, SHADER_TYPE_PIXEL, pixel, ps_name.c_str());
  if (!vs || !ps) return false;

  GraphicsPipelineStateCreateInfo info;
  info.PSODesc.Name = name;
  info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
  info.GraphicsPipeline.NumRenderTargets = 1;
  info.GraphicsPipeline.RTVFormats[0] = target_format;
  info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
  info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
  info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
  info.pVS = vs;
  info.pPS = ps;

  std::vector<ShaderResourceVariableDesc> variables;
  std::vector<ImmutableSamplerDesc> samplers;
  SamplerDesc linear, point;
  linear.MinFilter = linear.MagFilter = linear.MipFilter = FILTER_TYPE_LINEAR;
  linear.AddressU = linear.AddressV = linear.AddressW = TEXTURE_ADDRESS_CLAMP;
  point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
  point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
  for (const Input& input : inputs) {
    variables.push_back({SHADER_TYPE_PIXEL, input.name, SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC});
    samplers.push_back({SHADER_TYPE_PIXEL, input.name, input.linear ? linear : point});
  }
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
  info.PSODesc.ResourceLayout.Variables = variables.data();
  info.PSODesc.ResourceLayout.NumVariables = uint32_t(variables.size());
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers.data();
  info.PSODesc.ResourceLayout.NumImmutableSamplers = uint32_t(samplers.size());
  device_->CreateGraphicsPipelineState(info, &pass.pso);
  if (!pass.pso) {
    REXLOG_ERROR("[native_x_renderer] {}: no pipeline", name);
    return false;
  }
  for (SHADER_TYPE stage : {SHADER_TYPE_VERTEX, SHADER_TYPE_PIXEL}) {
    if (auto* variable = pass.pso->GetStaticVariableByName(stage, "AAConstants")) variable->Set(constants_);
  }
  pass.pso->CreateShaderResourceBinding(&pass.srb, true);
  return pass.srb != nullptr;
}

// A CMAA 2 compute pass: every resource is set at each application (the working set changes with the image's size).
bool Antialiasing::CreateCompute(Pass& pass, const char* name, const char* body) {
  const std::string source = std::string(kCmaaCommon) + body;
  std::vector<uint32_t> spirv = CompileGlsl(GlslStage::kCompute, source, name);
  if (spirv.empty()) return false;
  ShaderCreateInfo shader_info;
  shader_info.Desc.Name = name;
  shader_info.Desc.ShaderType = SHADER_TYPE_COMPUTE;
  shader_info.Desc.UseCombinedTextureSamplers = true;
  shader_info.ByteCode = spirv.data();
  shader_info.ByteCodeSize = spirv.size() * sizeof(uint32_t);
  RefCntAutoPtr<IShader> shader;
  device_->CreateShader(shader_info, &shader);
  if (!shader) return false;
  ComputePipelineStateCreateInfo info;
  info.PSODesc.Name = name;
  info.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
  info.pCS = shader;
  SamplerDesc point;
  point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
  point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
  ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_COMPUTE, "g_color", point}};
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
  info.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
  device_->CreateComputePipelineState(info, &pass.pso);
  if (!pass.pso) {
    REXLOG_ERROR("[native_x_renderer] {}: no pipeline", name);
    return false;
  }
  pass.pso->CreateShaderResourceBinding(&pass.srb, true);
  return pass.srb != nullptr;
}

// CMAA 2 on `color` (the image's copy): the edges and the shape candidates, the candidates' blends as lists per 2x2
// quad, and the lists resolved into `set.resolved` (alpha 1 where a pixel changed; cleared first). What a pass wrote
// is put in the shader resource state before the next pass reads it: DiligentCore's Vulkan backend places no barrier
// between two passes that both use a resource for unordered access.
void Antialiasing::ApplyCmaa(IDeviceContext* context, Textures& set, ITextureView* color) {
  {
    struct Constants {
      int32_t size[2];
      uint32_t candidates_max, items_max, locations_max;
      float threshold;
      uint32_t pad[2];
    } k{{int32_t(set.width), int32_t(set.height)}, set.candidates_max, set.items_max, set.locations_max,
        g_cmaa_threshold, {0, 0}};
    void* data = nullptr;
    context->MapBuffer(cmaa_constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, &k, sizeof(k));
    context->UnmapBuffer(cmaa_constants_, MAP_WRITE);
  }
  ITextureView* resolved_rt = set.resolved->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
  context->SetRenderTargets(1, &resolved_rt, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  const float zero[4] = {};
  context->ClearRenderTarget(resolved_rt, zero, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);

  auto set_var = [](Pass& pass, const char* name, IDeviceObject* object) {
    if (auto* v = pass.srb->GetVariableByName(SHADER_TYPE_COMPUTE, name)) v->Set(object);
  };
  auto uav = [](IBuffer* buffer) { return buffer->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS); };
  auto image = [](ITexture* texture) { return texture->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS); };
  auto written = [&](std::initializer_list<IBuffer*> buffers, std::initializer_list<ITexture*> textures) {
    std::vector<StateTransitionDesc> barriers;
    for (IBuffer* r : buffers)
      barriers.emplace_back(r, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_SHADER_RESOURCE, STATE_TRANSITION_FLAG_UPDATE_STATE);
    for (ITexture* r : textures)
      barriers.emplace_back(r, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_SHADER_RESOURCE, STATE_TRANSITION_FLAG_UPDATE_STATE);
    context->TransitionResourceStates(uint32_t(barriers.size()), barriers.data());
  };
  auto common = [&](Pass& pass) {
    set_var(pass, "CmaaConstants", cmaa_constants_);
    set_var(pass, "CmaaControl", uav(cmaa_control_));
  };
  auto run = [&](Pass& pass) {
    context->SetPipelineState(pass.pso);
    context->CommitShaderResources(pass.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  };

  common(cmaa_edges_);
  set_var(cmaa_edges_, "g_color", color);
  set_var(cmaa_edges_, "g_edges", image(set.edges));
  set_var(cmaa_edges_, "g_heads", image(set.heads));
  set_var(cmaa_edges_, "CmaaCandidates", uav(set.candidates));
  run(cmaa_edges_);
  context->DispatchCompute(DispatchComputeAttribs((set.width + 27) / 28, (set.height + 27) / 28, 1));
  written({cmaa_control_, set.candidates}, {set.edges, set.heads});

  common(cmaa_args_);
  set_var(cmaa_args_, "CmaaArgs", uav(cmaa_indirect_));
  run(cmaa_args_);
  context->DispatchCompute(DispatchComputeAttribs(2, 1, 1));
  written({cmaa_control_}, {});

  common(cmaa_process_);
  set_var(cmaa_process_, "g_color", color);
  set_var(cmaa_process_, "g_edges", image(set.edges));
  set_var(cmaa_process_, "g_heads", image(set.heads));
  set_var(cmaa_process_, "CmaaCandidates", uav(set.candidates));
  set_var(cmaa_process_, "CmaaItems", uav(set.items));
  set_var(cmaa_process_, "CmaaLocations", uav(set.locations));
  run(cmaa_process_);
  {
    DispatchComputeIndirectAttribs indirect;
    indirect.pAttribsBuffer = cmaa_indirect_;
    indirect.AttribsBufferStateTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context->DispatchComputeIndirect(indirect);
  }
  written({cmaa_control_, cmaa_indirect_, set.items, set.locations}, {set.heads});

  common(cmaa_args_);
  set_var(cmaa_args_, "CmaaArgs", uav(cmaa_indirect_));
  run(cmaa_args_);
  context->DispatchCompute(DispatchComputeAttribs(1, 2, 1));
  written({cmaa_control_}, {});

  common(cmaa_apply_);
  set_var(cmaa_apply_, "g_heads", image(set.heads));
  set_var(cmaa_apply_, "CmaaItems", uav(set.items));
  set_var(cmaa_apply_, "CmaaLocations", uav(set.locations));
  set_var(cmaa_apply_, "g_resolved", image(set.resolved));
  run(cmaa_apply_);
  {
    DispatchComputeIndirectAttribs indirect;
    indirect.pAttribsBuffer = cmaa_indirect_;
    indirect.AttribsBufferStateTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context->DispatchComputeIndirect(indirect);
  }
  written({cmaa_control_, cmaa_indirect_}, {});
}

Antialiasing::Pass* Antialiasing::OutputPass(TEXTURE_FORMAT format) {
  auto it = output_passes_.find(format);
  if (it != output_passes_.end()) return it->second.pso ? &it->second : nullptr;
  Pass& pass = output_passes_[format];
  bool ok = mode_ == Mode::kCmaa
                ? CreatePass(pass, "NativeXRenderer CMAA output", FxaaShader(true),
                             std::string("#version 450\n") + kCmaaOutputPS, {{"colorTex", false}, {"resolvedTex", false}},
                             format)
                : mode_ == Mode::kSmaa
                ? CreatePass(pass, "NativeXRenderer SMAA blending", SmaaShader(true, kSmaaBlendVS),
                             SmaaShader(false, kSmaaBlendPS), {{"colorTex", true}, {"blendTex", true}}, format)
                : CreatePass(pass, "NativeXRenderer FXAA", FxaaShader(true), FxaaShader(false), {{"colorTex", true}}, format);
  return ok ? &pass : nullptr;
}

Antialiasing::Textures* Antialiasing::EnsureTextures(uint32_t width, uint32_t height, TEXTURE_FORMAT format) {
  Textures* set = nullptr;
  for (Textures& t : textures_) {
    if (t.color && t.width == width && t.height == height && t.format == format) set = &t;
  }
  if (!set) {
    // D141: the set used longest ago (or never) is made again for this size; the other one stays for the other image.
    set = &textures_[0];
    for (Textures& t : textures_) {
      if (t.used < set->used) set = &t;
    }
    *set = Textures{};
    auto create = [&](RefCntAutoPtr<ITexture>& texture, const char* name, TEXTURE_FORMAT texture_format,
                      BIND_FLAGS bind) {
      TextureDesc desc;
      desc.Name = name;
      desc.Type = RESOURCE_DIM_TEX_2D;
      desc.Width = width;
      desc.Height = height;
      desc.Format = texture_format;
      desc.Usage = USAGE_DEFAULT;
      desc.BindFlags = bind;
      device_->CreateTexture(desc, nullptr, &texture);
      return texture != nullptr;
    };
    const bool ok =
        create(set->color, "NativeXRenderer antialiasing input", format, BIND_SHADER_RESOURCE) &&
        (mode_ != Mode::kSmaa ||
         (create(set->edges, "NativeXRenderer SMAA edges", TEX_FORMAT_RG8_UNORM,
                 BIND_SHADER_RESOURCE | BIND_RENDER_TARGET) &&
          create(set->weights, "NativeXRenderer SMAA weights", TEX_FORMAT_RGBA8_UNORM,
                 BIND_SHADER_RESOURCE | BIND_RENDER_TARGET))) &&
        (mode_ != Mode::kCmaa || [&] {
          // Intel's sizes (vaCMAA2DX11.cpp): enough for any image but noise, which the shaders' own limits stop.
          set->candidates_max = std::max(1u, width * height / 4);
          set->items_max = std::max(1u, width * height / 2);
          set->locations_max = (width * height + 3) / 6;
          auto list = [&](RefCntAutoPtr<IBuffer>& buffer, const char* name, uint32_t count, uint32_t stride) {
            BufferDesc desc;
            desc.Name = name;
            desc.Size = uint64_t(count) * stride;
            desc.Usage = USAGE_DEFAULT;
            desc.BindFlags = BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE;
            desc.Mode = BUFFER_MODE_STRUCTURED;
            desc.ElementByteStride = stride;
            device_->CreateBuffer(desc, nullptr, &buffer);
            return buffer != nullptr;
          };
          auto work = [&](RefCntAutoPtr<ITexture>& texture, const char* name, uint32_t w, uint32_t h, TEXTURE_FORMAT f,
                          BIND_FLAGS bind) {
            TextureDesc desc;
            desc.Name = name;
            desc.Type = RESOURCE_DIM_TEX_2D;
            desc.Width = w;
            desc.Height = h;
            desc.Format = f;
            desc.Usage = USAGE_DEFAULT;
            desc.BindFlags = bind;
            device_->CreateTexture(desc, nullptr, &texture);
            return texture != nullptr;
          };
          return work(set->edges, "NativeXRenderer CMAA edges", (width + 1) / 2, height, TEX_FORMAT_R8_UINT,
                      BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE) &&
                 work(set->heads, "NativeXRenderer CMAA list heads", (width + 1) / 2, (height + 1) / 2,
                      TEX_FORMAT_R32_UINT, BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE) &&
                 work(set->resolved, "NativeXRenderer CMAA changed pixels", width, height, TEX_FORMAT_RGBA16_FLOAT,
                      BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE | BIND_RENDER_TARGET) &&
                 list(set->candidates, "NativeXRenderer CMAA candidates", set->candidates_max, 4) &&
                 list(set->items, "NativeXRenderer CMAA blend items", set->items_max * 3, 4) &&
                 list(set->locations, "NativeXRenderer CMAA blend places", set->locations_max, 4);
        }());
    if (!ok) {
      *set = Textures{};
      return nullptr;
    }
    set->width = width, set->height = height, set->format = format;
    ++made_;
    static uint32_t logged = 0;
    if (logged < 16) {
      ++logged;
      REXLOG_INFO("[native_x_renderer] antialiasing: textures made for {}x{} images, format {} (D141: two sizes kept)",
                  width, height, int(format));
    }
  }
  set->used = ++uses_;
  // A set not used in the last 1,200 applications (a loading screen's second image, long gone) is freed.
  for (Textures& t : textures_) {
    if (&t != set && t.color && uses_ - t.used > 1200) t = Textures{};
  }
  return set;
}

void Antialiasing::Draw(IDeviceContext* context, Pass& pass, ITextureView* target, bool clear) {
  context->SetRenderTargets(1, &target, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  if (clear) {
    const float zero[4] = {};
    context->ClearRenderTarget(target, zero, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  }
  const auto& desc = target->GetTexture()->GetDesc();
  Viewport viewport;
  viewport.Width = float(desc.Width);
  viewport.Height = float(desc.Height);
  viewport.MaxDepth = 1.0f;
  context->SetViewports(1, &viewport, 0, 0);
  context->SetPipelineState(pass.pso);
  context->CommitShaderResources(pass.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DrawAttribs draw;
  draw.NumVertices = 3;
  context->Draw(draw);
}

void Antialiasing::Apply(IDeviceContext* context, ITexture* texture, ITextureView* target) {
  if (mode_ == Mode::kOff || !texture || !target) return;
  const TextureDesc& desc = texture->GetDesc();
  Pass* output = OutputPass(desc.Format);
  Textures* set = output ? EnsureTextures(desc.Width, desc.Height, desc.Format) : nullptr;
  if (!set) return;

  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    const float metrics[4] = {1.0f / float(desc.Width), 1.0f / float(desc.Height), float(desc.Width),
                              float(desc.Height)};
    std::memcpy(data, metrics, sizeof(metrics));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }
  // The input: a copy of the image, since the last pass writes over it. The resolve that made the image left
  // it bound as the render target: unbound first, or DiligentCore unbinds it with a message every frame.
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  CopyTextureAttribs copy{texture, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, set->color,
                          RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
  context->CopyTexture(copy);
  ITextureView* color = set->color->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);

  if (mode_ == Mode::kSmaa) {
    smaa_edges_.srb->GetVariableByName(SHADER_TYPE_PIXEL, "colorTex")->Set(color);
    Draw(context, smaa_edges_, set->edges->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET), true);
    smaa_weights_.srb->GetVariableByName(SHADER_TYPE_PIXEL, "edgesTex")
        ->Set(set->edges->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    Draw(context, smaa_weights_, set->weights->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET), true);
    output->srb->GetVariableByName(SHADER_TYPE_PIXEL, "colorTex")->Set(color);
    output->srb->GetVariableByName(SHADER_TYPE_PIXEL, "blendTex")
        ->Set(set->weights->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  } else if (mode_ == Mode::kCmaa) {
    ApplyCmaa(context, *set, color);
    output->srb->GetVariableByName(SHADER_TYPE_PIXEL, "colorTex")->Set(color);
    output->srb->GetVariableByName(SHADER_TYPE_PIXEL, "resolvedTex")
        ->Set(set->resolved->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  } else {
    output->srb->GetVariableByName(SHADER_TYPE_PIXEL, "colorTex")->Set(color);
  }
  Draw(context, *output, target, false);
  // The image is sampled next (the composite): not left bound as the render target either.
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  ++applied_;
}

int TestTaaShaders();  // taa.cpp
int TestMotionBlurShaders();  // motion_blur.cpp
int TestUtilityShaders() {
  int failed = TestTaaShaders() + TestMotionBlurShaders();
  const std::pair<const char*, const char*> shaders[] = {{"CMAA edges", kCmaaEdgesCS},
                                                         {"CMAA arguments", kCmaaArgsCS},
                                                         {"CMAA candidates", kCmaaProcessCS},
                                                         {"CMAA apply", kCmaaApplyCS}};
  for (const auto& [name, body] : shaders)
    if (CompileGlsl(GlslStage::kCompute, std::string(kCmaaCommon) + body, name).empty()) ++failed;
  if (CompileGlsl(GlslStage::kPixel, std::string("#version 450\n") + kCmaaOutputPS, "CMAA output").empty()) ++failed;
  REXLOG_INFO("[native_x_renderer] utility shader test: {} of 7 failed", failed);
  return failed;
}

}  // namespace native_x_renderer
