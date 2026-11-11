// NativeXRenderer - see ssgi.h.

#include "ssgi.h"

#include "glsl.h"
#include "ssgi_shaders.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

// King Kong Recompiled's settings (its plugin_main.cpp, docs/gi.md "Settings"), all read every frame here. KK's
// native_gi (off, low, medium, high) is native_gi_mode plus native_gi_quality, and KK's native_gi_quality (the
// working resolution) is native_gi_resolution.
REXCVAR_DEFINE_INT32(native_gi_mode, 1, "NativeXRenderer",
                     "Global illumination (screen space, one bounce of light from the lit scene; read every frame): "
                     "0 off, 1 on, 2 the bounce light only")
    .range(0, 2);
REXCVAR_DEFINE_INT32(native_gi_quality, 2, "NativeXRenderer",
                     "Global illumination quality (read every frame): 1 low (1 slice x 5 steps, 3 m), 2 medium "
                     "(2 x 6, 5 m), 3 high (4 x 10, 8 m, full resolution up to a render scale of 1.5)")
    .range(1, 3);
REXCVAR_DEFINE_INT32(native_gi_distance, 2, "NativeXRenderer",
                     "Global illumination distance (read every frame): 1 near (16 m), 2 medium (32 m), 3 far (64 m), "
                     "4 full (no limit); the last quarter fades out")
    .range(1, 4);
REXCVAR_DEFINE_DOUBLE(native_gi_intensity, 1.0, "NativeXRenderer",
                      "Global illumination strength, 0 to 8 (read every frame): 1 normal, 4 strong");
REXCVAR_DEFINE_STRING(native_gi_resolution, "auto", "NativeXRenderer",
                      "Global illumination's working resolution (read every frame): full or half (of the scene's "
                      "targets at the render scale), or auto (the quality's: half for low and medium)")
    .allowed({"auto", "full", "half"});
REXCVAR_DEFINE_BOOL(native_gi_temporal, true, "NativeXRenderer",
                    "Global illumination noise smoothing (read every frame): blend each pixel's light with last "
                    "frame's where the camera saw the same surface (or let the temporal upscaler do it)");
REXCVAR_DEFINE_BOOL(native_gi_gpu_time, false, "NativeXRenderer",
                    "Time the global illumination's GPU work and log the average every 600 frames");
REXCVAR_DEFINE_DOUBLE(native_gi_radius, 0.0, "NativeXRenderer",
                      "Global illumination's gather radius in world units (metres); 0: the quality's 3, 5 or 8");
REXCVAR_DEFINE_DOUBLE(native_gi_thickness, 0.0, "NativeXRenderer",
                      "How deep a surface seen on the screen is taken to be, world units; 0: a fifth of the radius, "
                      "0.25 to 1");
REXCVAR_DEFINE_DOUBLE(native_gi_albedo, 0.75, "NativeXRenderer",
                      "The brightest surface colour the global illumination assumes, 0.05 to 1 (a picture value)");
REXCVAR_DEFINE_DOUBLE(native_gi_occlusion, 0.0, "NativeXRenderer",
                      "With the ambient occlusion off: how much of the flat ambient light the global illumination "
                      "takes away where the surroundings hide the sky, 0 to 1 (0: it only adds light)");
REXCVAR_DEFINE_INT32(native_gi_debug, 0, "NativeXRenderer",
                     "Global illumination debug view: 0 the picture, 1 the bounce light, 2 the change on grey, 3 the "
                     "visibility")
    .range(0, 3);
REXCVAR_DEFINE_DOUBLE(native_gi_ambient, 0.25, "NativeXRenderer",
                      "The scene's flat ambient light as a grey picture value, 0.01 to 1, for the global "
                      "illumination's albedo and sunlight estimates (King Kong read it from its shaders; 0.25 is its "
                      "default)");
REXCVAR_DEFINE_DOUBLE(native_gi_gamma, 2.2, "NativeXRenderer",
                      "The scene colour's gamma for the global illumination, 1 to 3 (it works in linear light)");
REXCVAR_DEFINE_DOUBLE(native_gi_sunlit, 0.65, "NativeXRenderer",
                      "Extra light sent by surfaces brighter than the ambient lit ceiling (taken as sunlit), 0 to 2; "
                      "0: the plain lit colour");

namespace native_x_renderer {

using namespace Diligent;

namespace {

// XeGTAO.h's GTAOConstants (std140 in ssgi_shaders.h: 6 x 16 bytes), as gtao.cpp has it.
struct XeGtaoConstants {
  int32_t viewport_size[2];
  float viewport_pixel_size[2];
  float depth_unpack[2];
  float camera_tan_half_fov[2];
  float ndc_to_view_mul[2];
  float ndc_to_view_add[2];
  float ndc_to_view_mul_x_pixel_size[2];
  float effect_radius;
  float effect_falloff_range;
  float radius_multiplier;
  float padding0;
  float final_value_power;
  float denoise_blur_beta;
  float sample_distribution_power;
  float thin_occluder_compensation;
  float depth_mip_sampling_offset;
  int32_t noise_index;
};
static_assert(sizeof(XeGtaoConstants) == 96, "GTAOConstants is 6 vec4");

// ssgi_shaders.h's SsgiConstants block.
struct Constants {
  XeGtaoConstants g;
  int32_t source[4];       // the picture's origin xy, size zw (host pixels)
  uint32_t working[4];     // working size xy, padded size zw
  float params[4];         // step, unused, radius, thickness
  float gather[4];         // slices, steps per side, largest radius on the screen (working pixels), intensity x 3
  float ambient[4];        // rgb the flat ambient light (picture values), w the albedo limit (linear)
  float mode[4];           // occlusion, debug view, radiance mips, denoise step
  float extra[4];          // unused, noise index, history (1 or 0), the new light's weight
  float reprojection[16];  // rows: last clip = R x this clip
  float depth_range[4];    // z_max - z_min, z_min, last frame's depth unpack mul and add
  float shape[4];          // multi-bounce strength, largest darkening share, gamma, sunlit restoration
  float distance[4];       // max camera distance (0: none), fade start
  float sky[4];            // sky test: sign, threshold
};
static_assert(sizeof(Constants) == 336, "the SsgiConstants block");

// XeGTAO.h's defaults (as gtao.cpp).
constexpr float kDefaultRadiusMultiplier = 1.457f;
constexpr float kDefaultFalloffRange = 0.615f;
constexpr float kDefaultSampleDistributionPower = 2.0f;
constexpr float kDefaultDepthMipSamplingOffset = 3.30f;
constexpr uint32_t kDepthMipLevels = 5;

uint32_t RoundUp16(uint32_t v) { return (std::max(v, 1u) + 15) / 16 * 16; }

RefCntAutoPtr<IShader> CreateShader(IRenderDevice* device, SHADER_TYPE type, const std::string& source,
                                    const char* name) {
  RefCntAutoPtr<IShader> shader;
  GlslStage stage = type == SHADER_TYPE_VERTEX    ? GlslStage::kVertex
                    : type == SHADER_TYPE_COMPUTE ? GlslStage::kCompute
                                                  : GlslStage::kPixel;
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

// Depths, normals and the gathered light are fetched at texel centres or sampled with a point filter (XeGTAO's depth
// mips). Only the source light's mips are sampled with a linear filter (KK's gather: a far sample reads the mean of
// the surfaces around it, between two mips).
SamplerDesc PointClamp() {
  SamplerDesc point;
  point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
  point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
  return point;
}

SamplerDesc LinearClamp() {
  SamplerDesc linear;
  linear.MinFilter = linear.MagFilter = linear.MipFilter = FILTER_TYPE_LINEAR;
  linear.AddressU = linear.AddressV = linear.AddressW = TEXTURE_ADDRESS_CLAMP;
  return linear;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Settings.

SsgiSettings::Resolution SsgiSettings::ParseResolution(const std::string& name) {
  if (name == "full") return Resolution::kFull;
  if (name == "half") return Resolution::kHalf;
  return Resolution::kAuto;
}

const char* SsgiSettings::ResolutionName(Resolution resolution) {
  switch (resolution) {
    case Resolution::kFull:
      return "full";
    case Resolution::kHalf:
      return "half";
    default:
      return "auto";
  }
}

SsgiSettings SsgiSettings::FromCvars() {
  SsgiSettings s;
  s.mode = std::clamp(int(REXCVAR_GET(native_gi_mode)), 0, 2);
  s.quality = std::clamp(int(REXCVAR_GET(native_gi_quality)), 1, 3);
  s.distance = std::clamp(int(REXCVAR_GET(native_gi_distance)), 1, 4);
  s.intensity = float(std::clamp(REXCVAR_GET(native_gi_intensity), 0.0, 8.0));
  // The registry's lock guards the string against the settings UI setting it at the same time.
  s.resolution = ParseResolution(REXCVAR_QUERY(std::string, native_gi_resolution));
  s.temporal = REXCVAR_GET(native_gi_temporal);
  s.gpu_time = REXCVAR_GET(native_gi_gpu_time);
  s.radius = float(std::max(REXCVAR_GET(native_gi_radius), 0.0));
  s.thickness = float(std::max(REXCVAR_GET(native_gi_thickness), 0.0));
  s.albedo = float(std::clamp(REXCVAR_GET(native_gi_albedo), 0.05, 1.0));
  s.occlusion = float(std::clamp(REXCVAR_GET(native_gi_occlusion), 0.0, 1.0));
  s.debug = std::clamp(int(REXCVAR_GET(native_gi_debug)), 0, 3);
  s.ambient = float(std::clamp(REXCVAR_GET(native_gi_ambient), 0.01, 1.0));
  s.gamma = float(std::clamp(REXCVAR_GET(native_gi_gamma), 1.0, 3.0));
  s.sunlit = float(std::clamp(REXCVAR_GET(native_gi_sunlit), 0.0, 2.0));
  return s;
}

SsgiLevel Ssgi::LevelFor(int quality) {
  // KK's GiParamsFor: scene-scale radii (metres), so that a sunlit wall lights the shade a few metres away.
  switch (std::clamp(quality, 1, 3)) {
    case 1:
      // Low its own cost too (D150: KK's low was medium's 2 x 6 with a shorter reach, the same 0.47 ms a frame at
      // 1080p on an RTX 4070 Ti): one slice of five steps.
      return {1, 5, 2, true, 3.0f};
    case 2:
      return {2, 6, 2, true, 5.0f};
    default:
      return {4, 10, 3, false, 8.0f};
  }
}

float Ssgi::DistanceMetres(int distance) {
  switch (distance) {
    case 1:
      return 16.0f;
    case 2:
      return 32.0f;
    case 3:
      return 64.0f;
    default:
      return 0.0f;
  }
}

bool Ssgi::HistoryKey::operator==(const HistoryKey& o) const {
  return std::tie(quality, distance, step, radius, thickness, gamma, sunlit, x, y, width, height, z_min, z_max) ==
         std::tie(o.quality, o.distance, o.step, o.radius, o.thickness, o.gamma, o.sunlit, o.x, o.y, o.width,
                  o.height, o.z_min, o.z_max);
}

// ---------------------------------------------------------------------------------------------------------------
// Pipelines and images.

bool Ssgi::Initialize(IRenderDevice* device) {
  device_ = device;
  ready_ = false;
  BufferDesc constants;
  constants.Name = "NativeXRenderer GI constants";
  constants.Size = sizeof(Constants);
  constants.Usage = USAGE_DYNAMIC;
  constants.BindFlags = BIND_UNIFORM_BUFFER;
  constants.CPUAccessFlags = CPU_ACCESS_WRITE;
  device_->CreateBuffer(constants, nullptr, &constants_);
  using namespace ssgi_glsl;
  bool ok =
      constants_ &&
      CreateComputePass(prepare_, "NativeXRenderer GI prepare", kPrepare, {"g_srcSceneDepth"}, {}, {"g_outRawDepth"}) &&
      CreateComputePass(prefilter_, "NativeXRenderer GI prefilter", kPrefilter, {"g_srcRawDepth"}, {},
                        {"g_outWorkingDepthMIP0", "g_outWorkingDepthMIP1", "g_outWorkingDepthMIP2",
                         "g_outWorkingDepthMIP3", "g_outWorkingDepthMIP4"}) &&
      CreateComputePass(normals_, "NativeXRenderer GI normals", kNormals, {"g_srcRawDepth"}, {}, {"g_outNormalmap"}) &&
      CreateComputePass(radiance_pass_, "NativeXRenderer GI radiance", kRadiance,
                        {"g_srcSceneColor", "g_srcSourceDepth"}, {},
                        {"g_outRadianceMIP0", "g_outRadianceMIP1", "g_outRadianceMIP2", "g_outRadianceMIP3",
                         "g_outRadianceMIP4"}) &&
      CreateComputePass(gather_, "NativeXRenderer GI gather", kGather,
                        {"g_srcViewDepth", "g_srcNormalmap", "g_srcRawDepth"}, {"g_srcRadiance"}, {"g_outGi"}) &&
      CreateComputePass(temporal_, "NativeXRenderer GI temporal", kTemporal,
                        {"g_srcGi", "g_srcHistory", "g_srcHistoryDepth", "g_srcRawDepth"}, {}, {"g_outGi"}) &&
      CreateComputePass(denoise_, "NativeXRenderer GI denoise", kDenoise,
                        {"g_srcGi", "g_srcDenoiseDepth", "g_srcDenoiseNormals", "g_srcRawDepth"}, {}, {"g_outGi"});
  if (ok) {
    composite_vs_ = CreateShader(device_, SHADER_TYPE_VERTEX, kCompositeVS, "NativeXRenderer GI composite VS");
    composite_ps_ = CreateShader(device_, SHADER_TYPE_PIXEL, std::string("#version 450\n") + kCommon + kCompositePS,
                                 "NativeXRenderer GI composite PS");
    ok = composite_vs_ && composite_ps_;
  }
  if (!ok) {
    REXLOG_ERROR("[native_x_renderer] global illumination (SSGI) could not be set up: no global illumination");
    return false;
  }
  ready_ = true;
  REXLOG_INFO("[native_x_renderer] global illumination: screen space, visibility bitmask, ready");
  return true;
}

bool Ssgi::CreateComputePass(Pass& pass, const char* name, const char* body,
                             std::initializer_list<const char*> textures,
                             std::initializer_list<const char*> linear_textures,
                             std::initializer_list<const char*> images) {
  std::string source = std::string("#version 450\n") + ssgi_glsl::kCommon + body;
  RefCntAutoPtr<IShader> shader = CreateShader(device_, SHADER_TYPE_COMPUTE, source, name);
  if (!shader) return false;

  ComputePipelineStateCreateInfo info;
  info.PSODesc.Name = name;
  info.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
  info.pCS = shader;
  std::vector<ShaderResourceVariableDesc> variables;
  std::vector<ImmutableSamplerDesc> samplers;
  const SamplerDesc point = PointClamp(), linear = LinearClamp();
  for (const char* texture : textures) {
    variables.push_back({SHADER_TYPE_COMPUTE, texture, SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC});
    samplers.push_back({SHADER_TYPE_COMPUTE, texture, point});
  }
  for (const char* texture : linear_textures) {
    variables.push_back({SHADER_TYPE_COMPUTE, texture, SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC});
    samplers.push_back({SHADER_TYPE_COMPUTE, texture, linear});
  }
  for (const char* image : images) {
    variables.push_back({SHADER_TYPE_COMPUTE, image, SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC});
  }
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
  info.PSODesc.ResourceLayout.Variables = variables.data();
  info.PSODesc.ResourceLayout.NumVariables = uint32_t(variables.size());
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers.data();
  info.PSODesc.ResourceLayout.NumImmutableSamplers = uint32_t(samplers.size());
  device_->CreateComputePipelineState(info, &pass.pso);
  if (!pass.pso) {
    REXLOG_ERROR("[native_x_renderer] {}: no pipeline", name);
    return false;
  }
  if (auto* variable = pass.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "SsgiConstants")) {
    variable->Set(constants_);
  }
  pass.pso->CreateShaderResourceBinding(&pass.srb, true);
  return pass.srb != nullptr;
}

Ssgi::Pass* Ssgi::CompositePass(TEXTURE_FORMAT format) {
  auto it = composite_passes_.find(format);
  if (it != composite_passes_.end()) {
    Pass& pass = it->second;
    if (!pass.pso) return nullptr;
    if (!pass.srb) pass.pso->CreateShaderResourceBinding(&pass.srb, true);
    return pass.srb ? &pass : nullptr;
  }
  Pass& pass = composite_passes_[format];
  GraphicsPipelineStateCreateInfo info;
  info.PSODesc.Name = "NativeXRenderer GI composite";
  info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
  info.GraphicsPipeline.NumRenderTargets = 1;
  info.GraphicsPipeline.RTVFormats[0] = format;
  info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
  info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
  info.GraphicsPipeline.RasterizerDesc.ScissorEnable = True;
  info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
  info.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable = False;
  // No blend: the pixel shader writes the whole new colour, from a copy. The alpha channel is the game's.
  info.GraphicsPipeline.BlendDesc.RenderTargets[0].RenderTargetWriteMask = COLOR_MASK_RGB;
  info.pVS = composite_vs_;
  info.pPS = composite_ps_;
  const char* names[] = {"g_srcSceneCopy", "g_srcGiResult", "g_srcWorkDepth", "g_srcSceneDepth"};
  std::vector<ShaderResourceVariableDesc> variables;
  std::vector<ImmutableSamplerDesc> samplers;
  for (const char* name : names) {
    variables.push_back({SHADER_TYPE_PIXEL, name, SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC});
    samplers.push_back({SHADER_TYPE_PIXEL, name, PointClamp()});
  }
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
  info.PSODesc.ResourceLayout.Variables = variables.data();
  info.PSODesc.ResourceLayout.NumVariables = uint32_t(variables.size());
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers.data();
  info.PSODesc.ResourceLayout.NumImmutableSamplers = uint32_t(samplers.size());
  device_->CreateGraphicsPipelineState(info, &pass.pso);
  if (!pass.pso) {
    REXLOG_ERROR("[native_x_renderer] GI composite: no pipeline for target format {}", int(format));
    return nullptr;
  }
  if (auto* variable = pass.pso->GetStaticVariableByName(SHADER_TYPE_PIXEL, "SsgiConstants")) {
    variable->Set(constants_);
  }
  pass.pso->CreateShaderResourceBinding(&pass.srb, true);
  return pass.srb ? &pass : nullptr;
}

void Ssgi::ReleaseBindings() {
  for (Pass* pass : {&prepare_, &prefilter_, &normals_, &radiance_pass_, &gather_, &temporal_, &denoise_}) {
    pass->srb.Release();
  }
  for (auto& [format, pass] : composite_passes_) pass.srb.Release();
}

bool Ssgi::EnsureImages(uint32_t pw, uint32_t ph) {
  if (raw_ && pw == padded_w_ && ph == padded_h_) return true;
  auto create = [&](RefCntAutoPtr<ITexture>& texture, const char* name, TEXTURE_FORMAT format, uint32_t mips) {
    texture.Release();
    TextureDesc desc;
    desc.Name = name;
    desc.Type = RESOURCE_DIM_TEX_2D;
    desc.Width = std::max(pw, 1u);
    desc.Height = std::max(ph, 1u);
    desc.MipLevels = mips;
    desc.Format = format;
    desc.Usage = USAGE_DEFAULT;
    desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    device_->CreateTexture(desc, nullptr, &texture);
    return texture != nullptr;
  };
  // The bindings hold images about to be replaced: made again by the next dispatch.
  ReleaseBindings();
  padded_w_ = padded_h_ = 0;
  result_ = nullptr;
  history_frame_ = UINT64_MAX;
  // RGBA16F, R32F and R32UI are all storage formats every Vulkan device has (no shaderStorageImageExtendedFormats).
  bool ok = create(raw_, "NativeXRenderer GI stored depth", TEX_FORMAT_R32_FLOAT, 1) &&
            create(depth_mips_, "NativeXRenderer GI view depth", TEX_FORMAT_R32_FLOAT, kDepthMipLevels) &&
            create(normal_map_, "NativeXRenderer GI normals", TEX_FORMAT_R32_UINT, 1) &&
            create(radiance_, "NativeXRenderer GI radiance", TEX_FORMAT_RGBA16_FLOAT, kRadianceMips) &&
            create(gi_a_, "NativeXRenderer GI term a", TEX_FORMAT_RGBA16_FLOAT, 1) &&
            create(gi_b_, "NativeXRenderer GI term b", TEX_FORMAT_RGBA16_FLOAT, 1) &&
            create(history_, "NativeXRenderer GI history", TEX_FORMAT_RGBA16_FLOAT, 1) &&
            create(history_depth_, "NativeXRenderer GI history depth", TEX_FORMAT_R32_FLOAT, 1);
  for (uint32_t mip = 0; ok && mip < kDepthMipLevels; ++mip) {
    TextureViewDesc view;
    view.ViewType = TEXTURE_VIEW_UNORDERED_ACCESS;
    view.MostDetailedMip = mip;
    view.NumMipLevels = 1;
    depth_mip_uav_[mip].Release();
    depth_mips_->CreateView(view, &depth_mip_uav_[mip]);
    ok = depth_mip_uav_[mip] != nullptr;
  }
  for (uint32_t mip = 0; ok && mip < kRadianceMips; ++mip) {
    TextureViewDesc view;
    view.ViewType = TEXTURE_VIEW_UNORDERED_ACCESS;
    view.MostDetailedMip = mip;
    view.NumMipLevels = 1;
    radiance_mip_uav_[mip].Release();
    radiance_->CreateView(view, &radiance_mip_uav_[mip]);
    ok = radiance_mip_uav_[mip] != nullptr;
  }
  if (ok) {
    TextureViewDesc view;
    view.ViewType = TEXTURE_VIEW_SHADER_RESOURCE;
    view.MostDetailedMip = 0;
    view.NumMipLevels = 1;
    depth_mip0_srv_.Release();
    depth_mips_->CreateView(view, &depth_mip0_srv_);
    ok = depth_mip0_srv_ != nullptr;
  }
  if (!ok) {
    raw_.Release();
    return false;
  }
  padded_w_ = pw;
  padded_h_ = ph;
  return true;
}

bool Ssgi::EnsureCopy(const TextureDesc& color) {
  if (copy_) {
    const TextureDesc& d = copy_->GetDesc();
    if (d.Width == color.Width && d.Height == color.Height && d.Format == color.Format) return true;
  }
  // The passes that bind the copy.
  radiance_pass_.srb.Release();
  for (auto& [format, pass] : composite_passes_) pass.srb.Release();
  copy_.Release();
  TextureDesc desc;
  desc.Name = "NativeXRenderer GI scene colour";
  desc.Type = RESOURCE_DIM_TEX_2D;
  desc.Width = color.Width;
  desc.Height = color.Height;
  desc.MipLevels = 1;
  desc.Format = color.Format;
  desc.Usage = USAGE_DEFAULT;
  desc.BindFlags = BIND_SHADER_RESOURCE;
  device_->CreateTexture(desc, nullptr, &copy_);
  return copy_ != nullptr;
}

bool Ssgi::Dispatch(IDeviceContext* context, Pass& pass, std::initializer_list<Binding> bindings, uint32_t groups_x,
                    uint32_t groups_y) {
  if (!pass.srb) pass.pso->CreateShaderResourceBinding(&pass.srb, true);
  if (!pass.srb) return false;
  for (const Binding& binding : bindings) {
    if (!binding.object) return false;
    if (auto* variable = pass.srb->GetVariableByName(SHADER_TYPE_COMPUTE, binding.name)) variable->Set(binding.object);
  }
  context->SetPipelineState(pass.pso);
  context->CommitShaderResources(pass.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DispatchComputeAttribs dispatch(std::max(groups_x, 1u), std::max(groups_y, 1u), 1);
  context->DispatchCompute(dispatch);
  return true;
}

void Ssgi::WriteConstants(IDeviceContext* context, const void* data, size_t size) {
  // A dynamic buffer: each map is a new region, so the dispatches already recorded keep the values they had.
  void* mapped = nullptr;
  context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
  std::memcpy(mapped, data, size);
  context->UnmapBuffer(constants_, MAP_WRITE);
}

void Ssgi::Configure(const SsgiSettings& settings) { settings_ = settings; }

void Ssgi::ForgetTargets() {
  // The passes that bind the host's depth target (the colour is only copied and drawn into, never kept).
  prepare_.srb.Release();
  radiance_pass_.srb.Release();
  for (auto& [format, pass] : composite_passes_) pass.srb.Release();
  // The history was worked out from those targets: a new texture (a new render size) starts again.
  history_frame_ = UINT64_MAX;
}

// ---------------------------------------------------------------------------------------------------------------
// native_gi_gpu_time (as gtao.cpp's).

void Ssgi::TimerBegin(IDeviceContext* context) {
  if (!settings_.gpu_time || queries_failed_ || query_active_) return;
  if (!queries_[0]) {
    if (device_->GetDeviceInfo().Features.DurationQueries != DEVICE_FEATURE_STATE_ENABLED) {
      queries_failed_ = true;
      REXLOG_WARN("[native_x_renderer] native_gi_gpu_time: the device has no duration queries");
      return;
    }
    for (auto& query : queries_) {
      QueryDesc desc;
      desc.Name = "NativeXRenderer GI time";
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
    } else {
      ++timer_late_;
    }
    query_pending_[query_index_] = false;
  }
  context->BeginQuery(queries_[query_index_]);
  query_active_ = true;
}

void Ssgi::TimerEnd(IDeviceContext* context) {
  if (!query_active_) return;
  context->EndQuery(queries_[query_index_]);
  query_pending_[query_index_] = true;
  query_index_ = (query_index_ + 1) % kQueries;
  query_active_ = false;
  if (timer_frames_ >= 600) {
    REXLOG_INFO("[native_x_renderer] global illumination GPU time: {:.3f} ms a frame on average over {} frames (the "
                "longest {:.3f} ms; {} not read in time)",
                timer_total_ms_ / double(timer_frames_), timer_frames_, timer_max_ms_, timer_late_);
    timer_total_ms_ = timer_max_ms_ = 0.0;
    timer_frames_ = timer_late_ = 0;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// The passes.

bool Ssgi::Apply(IDeviceContext* context, const Inputs& in) {
  const SsgiSettings& s = settings_;
  if (!ready_ || !context || s.mode <= 0) return false;
  auto refuse = [&](const char* why) {
    if (!warned_inputs_) {
      warned_inputs_ = true;
      REXLOG_WARN("[native_x_renderer] global illumination: {}: none this frame (logged once)", why);
    }
    return false;
  };
  if (!in.depth || !in.color) return refuse("no depth or colour target");
  ITexture* color_texture = in.color_texture ? in.color_texture : in.color->GetTexture();
  if (!color_texture) return refuse("no colour texture");
  ITextureView* depth_srv = in.depth->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  if (!depth_srv) return refuse("the depth target has no shader resource view");
  const TextureDesc& dd = in.depth->GetDesc();
  const TextureDesc& cd = color_texture->GetDesc();
  if (cd.SampleCount > 1) return refuse("the colour target is multisampled");
  if (in.width < 16 || in.height < 16 || in.x < 0 || in.y < 0 || uint64_t(in.x) + in.width > dd.Width ||
      uint64_t(in.y) + in.height > dd.Height || uint64_t(in.x) + in.width > cd.Width ||
      uint64_t(in.y) + in.height > cd.Height) {
    return refuse("the picture is not inside the depth and colour targets");
  }
  const GtaoProjection& p = in.projection;
  if (!p.valid()) return refuse("no camera");
  const float zs = in.z_max - in.z_min, zo = in.z_min;
  if (zs == 0.0f || !std::isfinite(zs)) return refuse("the viewport's depth range is empty");

  // The level, and the working resolution: Low and Medium half, High full up to a render scale of 1.5 (KK's auto).
  const SsgiLevel level = LevelFor(s.quality);
  bool half = level.half;
  if (s.resolution == SsgiSettings::Resolution::kHalf) {
    half = true;
  } else if (s.resolution == SsgiSettings::Resolution::kFull) {
    half = false;
  } else if (!level.half) {
    half = in.scale.x > 1.5f || in.scale.y > 1.5f;
  }
  const uint32_t step = half ? 2 : 1;
  const uint32_t work_w = (in.width + step - 1) / step, work_h = (in.height + step - 1) / step;
  const uint32_t pw = RoundUp16(work_w), ph = RoundUp16(work_h);
  if (!EnsureImages(pw, ph) || !EnsureCopy(cd)) return refuse("its images could not be created");
  step_ = step;

  const float radius = s.radius > 0.0f ? s.radius : level.radius;
  const float thickness = s.thickness > 0.0f ? s.thickness : std::clamp(0.2f * radius, 0.25f, 1.0f);
  const float distance = DistanceMetres(s.distance);
  // The depth curve's own near plane, where the stored depth reaches 1 (renderer.cpp's rule for the AO): Battlefield
  // 1943's world projection says 21.34 while its depth is 0.1 / distance. Only the depth prefilter's weights use it.
  float near_plane = p.near_plane();
  {
    const float d = (1.0f - zo) / zs - p.a;
    const float curve = d != 0.0f ? p.b / d : 0.0f;
    if (curve > 0.0f && std::isfinite(curve) && curve < near_plane) near_plane = curve;
  }

  // Temporal accumulation: the local history without a temporal upscaler, with a matching camera (KK's GI rule: at
  // least 2 votes and half of the offers) and a contiguous frame with the same settings and picture.
  const bool local = s.temporal && !s.temporal_upscaler;
  const bool reprojection_valid = in.reprojection && in.votes >= 2 && in.votes * 2 >= in.offers;
  HistoryKey key;
  key.quality = s.quality;
  key.distance = s.distance;
  key.step = step;
  key.radius = radius;
  key.thickness = thickness;
  key.gamma = s.gamma;
  key.sunlit = s.sunlit;
  key.x = in.x;
  key.y = in.y;
  key.width = in.width;
  key.height = in.height;
  key.z_min = in.z_min;
  key.z_max = in.z_max;
  const bool history = local && reprojection_valid && history_frame_ != UINT64_MAX && in.frame != UINT64_MAX &&
                       in.frame == history_frame_ + 1 && key == history_key_;
  // The noise rotates only where a temporal filter smooths it (as gtao.cpp), else it stays fixed.
  const int32_t noise =
      (s.temporal_upscaler || (local && reprojection_valid)) && in.frame != UINT64_MAX ? int32_t(in.frame % 64) : 0;

  // The constants. The camera mapping is gtao.cpp's: working pixel i is host pixel origin + step * i; XeGTAO's uv is
  // (i + 0.5) / padded size; view x / view z = (clip x - cx) / px, view y alike (y up); stored depth = zs * (a + b /
  // view z) + zo.
  Constants k{};
  XeGtaoConstants& g = k.g;
  const float fw = float(in.width), fh = float(in.height), fstep = float(step);
  const float mul_x = 2.0f * fstep * float(pw) / (fw * p.px);
  const float add_x = ((1.0f - fstep) / fw - 1.0f - p.cx) / p.px;
  const float mul_y = -2.0f * fstep * float(ph) / (fh * p.py);
  const float add_y = (1.0f - (1.0f - fstep) / fh - p.cy) / p.py;
  g.viewport_size[0] = int32_t(pw);
  g.viewport_size[1] = int32_t(ph);
  g.viewport_pixel_size[0] = 1.0f / float(pw);
  g.viewport_pixel_size[1] = 1.0f / float(ph);
  g.depth_unpack[0] = -p.b * zs;
  g.depth_unpack[1] = zs * p.a + zo;
  g.camera_tan_half_fov[0] = 1.0f / p.px;
  g.camera_tan_half_fov[1] = 1.0f / p.py;
  g.ndc_to_view_mul[0] = mul_x;
  g.ndc_to_view_mul[1] = mul_y;
  g.ndc_to_view_add[0] = add_x;
  g.ndc_to_view_add[1] = add_y;
  g.ndc_to_view_mul_x_pixel_size[0] = mul_x / float(pw);
  g.ndc_to_view_mul_x_pixel_size[1] = mul_y / float(ph);
  g.effect_radius = kPrefilterRadiusInNearPlanes * near_plane;
  g.effect_falloff_range = kDefaultFalloffRange;
  g.radius_multiplier = kDefaultRadiusMultiplier;
  g.final_value_power = 2.2f;
  g.denoise_blur_beta = 1.2f;
  g.sample_distribution_power = kDefaultSampleDistributionPower;
  g.thin_occluder_compensation = 0.0f;
  g.depth_mip_sampling_offset = kDefaultDepthMipSamplingOffset;
  g.noise_index = noise;
  k.source[0] = in.x;
  k.source[1] = in.y;
  k.source[2] = int32_t(in.width);
  k.source[3] = int32_t(in.height);
  k.working[0] = work_w;
  k.working[1] = work_h;
  k.working[2] = pw;
  k.working[3] = ph;
  k.params[0] = fstep;
  k.params[2] = std::max(radius, 1e-3f);
  k.params[3] = std::max(thickness, 1e-4f);
  k.gather[0] = float(std::max(level.slices, 1u));
  k.gather[1] = float(std::max(level.steps, 1u));
  // At most this share of the picture's height (working pixels): the radiance's mips and the temporal accumulation
  // keep a scene-scale gather smooth.
  k.gather[2] = std::max(8.0f, kMaxScreenRadius * float(work_h));
  // KK's task 066 calibration: intensity 1 is the normal presentation strength over an already lit picture.
  k.gather[3] = kCalibration * s.intensity;
  for (int i = 0; i < 3; ++i) {
    k.ambient[i] = in.ambient ? std::clamp(in.ambient[i], 0.0f, 4.0f) : s.ambient;
    if (!std::isfinite(k.ambient[i])) k.ambient[i] = s.ambient;
  }
  // The albedo limit is given as a picture value; the composite works in linear light.
  k.ambient[3] = std::pow(std::clamp(s.albedo, 0.05f, 1.0f), s.gamma);
  // KK's rule: with the AO on (either method), the GI only adds light.
  k.mode[0] = in.ao_on ? 0.0f : std::clamp(s.occlusion, 0.0f, 1.0f);
  k.mode[1] = float(s.mode >= 2 && s.debug == 0 ? 1 : s.debug);
  k.mode[2] = float(kRadianceMips);
  k.mode[3] = 1.0f;
  k.extra[1] = float(noise);
  k.extra[2] = history ? 1.0f : 0.0f;
  k.extra[3] = kNewWeight;
  if (history) {
    for (int i = 0; i < 16; ++i) k.reprojection[i] = float((*in.reprojection)[i]);
  }
  k.depth_range[0] = zs;
  k.depth_range[1] = zo;
  // Last frame's stored depth to its view depth (the same depth range: the key matched).
  const GtaoProjection& hp = history ? history_projection_ : p;
  k.depth_range[2] = -hp.b * zs;
  k.depth_range[3] = zs * hp.a + zo;
  k.shape[0] = 1.0f;  // KK's multi-bounce strength
  k.shape[1] = kDarkest;
  k.shape[2] = s.gamma;
  k.shape[3] = s.sunlit;
  k.distance[0] = distance;
  k.distance[1] = distance * 0.75f;
  // Stored depth at the far plane or beyond (within 1e-6 of the range) is sky; reversed depth tests the other way.
  const float sky_sign = zs > 0.0f ? 1.0f : -1.0f;
  k.sky[0] = sky_sign;
  k.sky[1] = sky_sign * (zo + zs * (1.0f - 1e-6f));
  WriteConstants(context, &k, sizeof(k));

  TimerBegin(context);
  // The depth is read as a texture and the colour copied: neither left bound as a target.
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  {
    CopyTextureAttribs copy{color_texture, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, copy_,
                            RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
    context->CopyTexture(copy);
  }
  ITextureView* copy_srv = copy_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* raw_srv = raw_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* raw_uav = raw_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
  ITextureView* depth_mips_srv = depth_mips_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* depth_mip0_srv = depth_mip0_srv_.RawPtr();
  ITextureView* normals_srv = normal_map_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* normals_uav = normal_map_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
  ITextureView* radiance_srv = radiance_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* a_srv = gi_a_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* a_uav = gi_a_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
  ITextureView* b_srv = gi_b_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* b_uav = gi_b_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);

  bool ok =
      Dispatch(context, prepare_, {{"g_srcSceneDepth", depth_srv}, {"g_outRawDepth", raw_uav}}, pw / 8, ph / 8) &&
      Dispatch(context, prefilter_,
               {{"g_srcRawDepth", raw_srv},
                {"g_outWorkingDepthMIP0", depth_mip_uav_[0].RawPtr()},
                {"g_outWorkingDepthMIP1", depth_mip_uav_[1].RawPtr()},
                {"g_outWorkingDepthMIP2", depth_mip_uav_[2].RawPtr()},
                {"g_outWorkingDepthMIP3", depth_mip_uav_[3].RawPtr()},
                {"g_outWorkingDepthMIP4", depth_mip_uav_[4].RawPtr()}},
               pw / 16, ph / 16) &&
      Dispatch(context, normals_, {{"g_srcRawDepth", raw_srv}, {"g_outNormalmap", normals_uav}}, pw / 8, ph / 8) &&
      Dispatch(context, radiance_pass_,
               {{"g_srcSceneColor", copy_srv},
                {"g_srcSourceDepth", depth_srv},
                {"g_outRadianceMIP0", radiance_mip_uav_[0].RawPtr()},
                {"g_outRadianceMIP1", radiance_mip_uav_[1].RawPtr()},
                {"g_outRadianceMIP2", radiance_mip_uav_[2].RawPtr()},
                {"g_outRadianceMIP3", radiance_mip_uav_[3].RawPtr()},
                {"g_outRadianceMIP4", radiance_mip_uav_[4].RawPtr()}},
               pw / 16, ph / 16) &&
      Dispatch(context, gather_,
               {{"g_srcViewDepth", depth_mips_srv},
                {"g_srcNormalmap", normals_srv},
                {"g_srcRadiance", radiance_srv},
                {"g_srcRawDepth", raw_srv},
                {"g_outGi", a_uav}},
               pw / 8, ph / 8);
  // Temporal accumulation: a into b with the history (or a alone without one), b kept as the next frame's history
  // with this frame's view depth; the blur then starts from b.
  bool from_b = false;
  if (ok && local) {
    ok = Dispatch(context, temporal_,
                  {{"g_srcGi", a_srv},
                   {"g_srcHistory", history_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)},
                   {"g_srcHistoryDepth", history_depth_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)},
                   {"g_srcRawDepth", raw_srv},
                   {"g_outGi", b_uav}},
                  pw / 8, ph / 8);
    if (ok) {
      CopyTextureAttribs light{gi_b_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, history_,
                               RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
      context->CopyTexture(light);
      CopyTextureAttribs depth{depth_mips_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, history_depth_,
                               RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
      depth.SrcMipLevel = 0;
      context->CopyTexture(depth);
      history_frame_ = in.frame;
      history_key_ = key;
      history_projection_ = p;
      if (history) ++history_frames_;
      from_b = true;
    }
  }
  if (!ok || !local) history_frame_ = UINT64_MAX;
  // The blur: a to b, b to a, ... with steps 1, 2, 4 (the result is where the last pass wrote).
  ITexture* last = from_b ? gi_b_.RawPtr() : gi_a_.RawPtr();
  for (uint32_t i = 0; ok && i < level.denoise; ++i) {
    if (i > 0) {
      k.mode[3] = float(1u << i);
      WriteConstants(context, &k, sizeof(k));
    }
    const bool a_to_b = ((i & 1) == 0) != from_b;
    ok = Dispatch(context, denoise_,
                  {{"g_srcGi", a_to_b ? a_srv : b_srv},
                   {"g_srcDenoiseDepth", depth_mip0_srv},
                   {"g_srcDenoiseNormals", normals_srv},
                   {"g_srcRawDepth", raw_srv},
                   {"g_outGi", a_to_b ? b_uav : a_uav}},
                  pw / 8, ph / 8);
    last = a_to_b ? gi_b_.RawPtr() : gi_a_.RawPtr();
  }
  result_ = ok ? last : nullptr;

  // Into the scene's colour: one triangle over the picture, reading the copy.
  Pass* composite = ok ? CompositePass(in.color->GetDesc().Format) : nullptr;
  bool bound = false;
  if (composite) {
    struct {
      const char* name;
      ITextureView* view;
    } inputs[] = {{"g_srcSceneCopy", copy_srv},
                  {"g_srcGiResult", result_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)},
                  {"g_srcWorkDepth", depth_mip0_srv},
                  {"g_srcSceneDepth", depth_srv}};
    bound = true;
    for (const auto& input : inputs) {
      IShaderResourceVariable* variable = composite->srb->GetVariableByName(SHADER_TYPE_PIXEL, input.name);
      if (!variable || !input.view) {
        bound = false;
        break;
      }
      variable->Set(input.view);
    }
  }
  if (bound) {
    ITextureView* color = in.color;
    context->SetRenderTargets(1, &color, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Viewport viewport;
    viewport.TopLeftX = float(in.x);
    viewport.TopLeftY = float(in.y);
    viewport.Width = fw;
    viewport.Height = fh;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->SetViewports(1, &viewport, 0, 0);
    Diligent::Rect scissor{in.x, in.y, in.x + int32_t(in.width), in.y + int32_t(in.height)};
    context->SetScissorRects(1, &scissor, 0, 0);
    context->SetPipelineState(composite->pso);
    context->CommitShaderResources(composite->srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    DrawAttribs draw;
    draw.NumVertices = 3;
    context->Draw(draw);
    // The renderer binds its own targets again (DrawPath::TargetsChanged).
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  }
  TimerEnd(context);
  if (!bound) return refuse("a pass could not be bound or the colour format has no pipeline");

  if (computed_ % 1800 == 0) {
    REXLOG_INFO("[native_x_renderer] global illumination: {}x{} at {},{} ({} resolution, quality {}: {} slices x {} "
                "steps, {} blur passes); radius {:.2f}, thickness {:.2f} world units, distance {}; intensity {:.2f}, "
                "ambient {:.3f} {:.3f} {:.3f}{}; {}; {} frames used history",
                in.width, in.height, in.x, in.y, half ? "half" : "full", s.quality, level.slices, level.steps,
                level.denoise, radius, thickness, distance > 0.0f ? std::to_string(int(distance)) + " m" : "full",
                s.intensity, k.ambient[0], k.ambient[1], k.ambient[2], in.ambient ? "" : " (native_gi_ambient)",
                s.temporal_upscaler ? "the upscaler's history" : local ? "local history" : "no history",
                history_frames_);
  }
  ++computed_;
  ++applied_;
  return true;
}

}  // namespace native_x_renderer
