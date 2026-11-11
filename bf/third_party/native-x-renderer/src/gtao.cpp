// NativeXRenderer - see gtao.h.

#include "gtao.h"

#include "glsl.h"
#include "gtao_shaders.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

// King Kong Recompiled's meanings (its plugin_main.cpp and docs/ao.md), its ao_mode and ao_strength under names of
// their own: the SDK's Xenos plugin defines ao_mode and ao_strength for its D3D12 AO (defaults 0 and 1), and whichever
// module registered first would have set the other's defaults.
REXCVAR_DEFINE_INT32(native_ao_mode, 1, "NativeXRenderer",
                     "Ambient occlusion (GTAO, read every frame): 0 off, 1 on, 2 the occlusion only")
    .range(0, 2);
REXCVAR_DEFINE_INT32(native_ao_strength, 3, "NativeXRenderer",
                     "Ambient occlusion strength, 1 to 3 (read every frame): XeGTAO's final power 1.6, 2.2 or 3.0 "
                     "and a radius of 6, 8 or 12 near-plane distances")
    .range(1, 3);
REXCVAR_DEFINE_INT32(native_ao_samples, 2, "NativeXRenderer",
                     "GTAO sample quality (read every frame): 1 low (2 slices x 2 steps), 2 medium (3 x 3), 3 high "
                     "(9 x 3), independently of native_ao_quality's resolution")
    .range(1, 3);
REXCVAR_DEFINE_STRING(native_ao_quality, "auto", "NativeXRenderer",
                      "GTAO's resolution (read every frame): full or half (of the scene's targets at the render "
                      "scale), or auto (full below a render scale of 1.5, half from 1080p up)")
    .allowed({"auto", "full", "half"});
REXCVAR_DEFINE_INT32(native_ao_normal_smoothing, 2, "NativeXRenderer",
                     "GTAO normal smoothing (read every frame): 0 off, 1 low, 2 high. The normals rebuilt from depth "
                     "are flat per triangle, so low-poly models get faceted occlusion; this blurs them across each "
                     "surface (silhouettes and right-angle edges stay sharp)")
    .range(0, 2);
REXCVAR_DEFINE_BOOL(native_ao_temporal, true, "NativeXRenderer",
                    "GTAO noise smoothing (read every frame): blend each pixel's occlusion with last frame's (or let "
                    "the temporal upscaler do it). Off shows the raw, grainier occlusion");
REXCVAR_DEFINE_BOOL(native_ao_gpu_time, false, "NativeXRenderer",
                    "Time the ambient occlusion's GPU work and log the average every 600 frames");
REXCVAR_DEFINE_DOUBLE(native_ao_radius, 0.0, "NativeXRenderer",
                      "GTAO's occlusion radius in the game's world units; 0: from the camera (the strength's radius "
                      "in near-plane distances)");
REXCVAR_DEFINE_DOUBLE(native_ao_radius_scale, 1.0, "NativeXRenderer",
                      "GTAO's radius from the camera times this (tuning)");
REXCVAR_DEFINE_DOUBLE(native_ao_falloff, 0.0, "NativeXRenderer",
                      "GTAO's falloff range, 0 to 1 of the radius (0: XeGTAO's 0.615)");
REXCVAR_DEFINE_DOUBLE(native_ao_power, 0.0, "NativeXRenderer", "GTAO's final power (0: from native_ao_strength)");
REXCVAR_DEFINE_DOUBLE(native_ao_fade, 0.0, "NativeXRenderer",
                      "GTAO's occlusion fades out linearly to this many near-plane distances (0: none)");

namespace native_x_renderer {

using namespace Diligent;

namespace {

// XeGTAO.h's GTAOConstants (std140 in gtao_shaders.h: 6 x 16 bytes).
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

// gtao_shaders.h's GtaoConstants block.
struct Constants {
  XeGtaoConstants g;
  int32_t source[4];       // the picture's origin xy, size zw (host pixels)
  uint32_t working[4];     // working size xy, padded size zw
  float params[4];         // step, sky test sign and threshold, ao_mode
  float fade[4];           // x: view depth where the occlusion has faded out, 0: no fade
  float reprojection[16];  // rows: last clip = R x this clip
  float depth_range[4];    // z_max - z_min, z_min
  float temporal[4];       // history weight, last frame's depth unpack mul and add
  float smooth[4];         // normal smoothing: largest radius (working pixels), radius (view), cos full / max angle
  float smooth2[4];        // sin of the plane's full / max angle
};
static_assert(sizeof(Constants) == 288, "the GtaoConstants block");

// XeGTAO.h's defaults.
constexpr float kDefaultRadiusMultiplier = 1.457f;
constexpr float kDefaultFalloffRange = 0.615f;
constexpr float kDefaultSampleDistributionPower = 2.0f;
constexpr float kDefaultThinOccluderCompensation = 0.0f;
constexpr float kDefaultDepthMipSamplingOffset = 3.30f;
constexpr uint32_t kDepthMipLevels = 5;
// XeGTAO's value with denoising on.
constexpr float kDenoiseBlurBeta = 1.2f;
// KK's local history weight.
constexpr float kHistoryWeight = 0.8f;

uint32_t RoundUp16(uint32_t v) { return (std::max(v, 1u) + 15) / 16 * 16; }

// Where clip z / w = a + b / w is 0 and 1: w = -b / a and w = b / (1 - a); the nearer positive one is the near plane.
void Planes(float a, float b, float& near_plane, float& far_plane) {
  constexpr float kInfinity = std::numeric_limits<float>::infinity();
  float w0 = a != 0.0f ? -b / a : kInfinity;
  float w1 = a != 1.0f ? b / (1.0f - a) : kInfinity;
  if (!(w0 > 0.0f)) w0 = kInfinity;
  if (!(w1 > 0.0f)) w1 = kInfinity;
  near_plane = std::min(w0, w1);
  far_plane = std::max(w0, w1);
  if (!std::isfinite(near_plane)) near_plane = 0.0f;
}

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

// Every texture is only fetched or gathered at its texels' centres, or sampled with a point filter (XeGTAO's
// depth mips: a linear filter would interpolate between neighbouring depths).
SamplerDesc PointClamp() {
  SamplerDesc point;
  point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
  point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
  return point;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Settings and camera.

GtaoSettings::Quality GtaoSettings::ParseQuality(const std::string& name) {
  if (name == "full") return Quality::kFull;
  if (name == "half") return Quality::kHalf;
  return Quality::kAuto;
}

const char* GtaoSettings::QualityName(Quality quality) {
  switch (quality) {
    case Quality::kFull:
      return "full";
    case Quality::kHalf:
      return "half";
    default:
      return "auto";
  }
}

GtaoSettings GtaoSettings::FromCvars() {
  GtaoSettings s;
  // The registry's lock guards the string against the settings UI setting it at the same time.
  s.mode = std::clamp(int(REXCVAR_GET(native_ao_mode)), 0, 2);
  s.strength = std::clamp(int(REXCVAR_GET(native_ao_strength)), 1, 3);
  s.quality = ParseQuality(REXCVAR_QUERY(std::string, native_ao_quality));
  s.samples = std::clamp(int(REXCVAR_GET(native_ao_samples)), 1, 3);
  s.normal_smoothing = std::clamp(int(REXCVAR_GET(native_ao_normal_smoothing)), 0, 2);
  s.temporal = REXCVAR_GET(native_ao_temporal);
  s.gpu_time = REXCVAR_GET(native_ao_gpu_time);
  s.radius = float(std::max(REXCVAR_GET(native_ao_radius), 0.0));
  s.radius_scale = float(REXCVAR_GET(native_ao_radius_scale));
  s.falloff = float(std::max(REXCVAR_GET(native_ao_falloff), 0.0));
  s.power = float(std::max(REXCVAR_GET(native_ao_power), 0.0));
  s.fade = float(std::max(REXCVAR_GET(native_ao_fade), 0.0));
  return s;
}

float GtaoProjection::near_plane() const {
  float n, f;
  Planes(a, b, n, f);
  return n;
}

float GtaoProjection::far_plane() const {
  float n, f;
  Planes(a, b, n, f);
  return std::isfinite(f) ? f : 0.0f;
}

bool GtaoProjection::valid() const {
  return px > 0.0f && py > 0.0f && std::isfinite(px) && std::isfinite(py) && std::isfinite(a) && std::isfinite(b) &&
         b != 0.0f && near_plane() > 0.0f;
}

bool GtaoProjection::FromMatrix(const Mat4d& m, GtaoProjection& out) {
  auto dot3 = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  const double* x = m.data();
  const double* y = m.data() + 4;
  const double* z = m.data() + 8;
  const double* w = m.data() + 12;
  const double ww = dot3(w, w);
  if (!(ww > 1e-8) || !(dot3(x, x) > 1e-8) || !(dot3(y, y) > 1e-8)) return false;
  // A perspective: clip z and w rows almost parallel (camera_vote.cpp's test, KK's LooksLikePerspectiveRows).
  const double k = dot3(z, w) / ww;
  if (!(k > 0.8 && k < 1.25)) return false;
  double residual = 0.0;
  for (int i = 0; i < 3; ++i) residual += (z[i] - k * w[i]) * (z[i] - k * w[i]);
  if (residual > 0.0025 * ww) return false;
  const double cx = dot3(x, w) / ww, cy = dot3(y, w) / ww;
  const double px2 = dot3(x, x) / ww - cx * cx, py2 = dot3(y, y) / ww - cy * cy;
  if (!(px2 > 0.0) || !(py2 > 0.0)) return false;
  GtaoProjection p;
  p.px = float(std::sqrt(px2));
  p.py = float(std::sqrt(py2));
  p.cx = float(cx);
  p.cy = float(cy);
  p.a = float(k);
  p.b = float(z[3] - k * w[3]);
  if (!p.valid()) return false;
  out = p;
  return true;
}

bool GtaoProjection::FromViewProjectionRows(const std::array<float, 16>& m, GtaoProjection& out) {
  Mat4d column;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) column[c * 4 + r] = m[r * 4 + c];
  }
  return FromMatrix(column, out);
}

float Gtao::RadiusInNearPlanes(int strength) {
  // KK's legacy AO's radius for the strength (AmbientOcclusion::TuningFor), in near-plane distances.
  switch (std::clamp(strength, 1, 3)) {
    case 1:
      return 6.0f;
    case 2:
      return 8.0f;
    default:
      return 12.0f;
  }
}

float Gtao::FinalPower(int strength) {
  switch (std::clamp(strength, 1, 3)) {
    case 1:
      return 1.6f;
    case 2:
      return 2.2f;
    default:
      return 3.0f;
  }
}

Gtao::Smoothing Gtao::SmoothingFor(int level) {
  switch (level) {
    case 1:
      return {8.0f, 0.25f, 20.0f, 35.0f};
    case 2:
      return {16.0f, 0.5f, 35.0f, 60.0f};
    default:
      return {};
  }
}

bool Gtao::HistoryKey::operator==(const HistoryKey& o) const {
  return std::tie(mode, strength, samples, normal_smoothing, step, radius, radius_scale, falloff, power, fade, x, y,
                  width, height, z_min, z_max) ==
         std::tie(o.mode, o.strength, o.samples, o.normal_smoothing, o.step, o.radius, o.radius_scale, o.falloff,
                  o.power, o.fade, o.x, o.y, o.width, o.height, o.z_min, o.z_max);
}

// ---------------------------------------------------------------------------------------------------------------
// Pipelines and images.

bool Gtao::Initialize(IRenderDevice* device) {
  device_ = device;
  ready_ = false;
  BufferDesc constants;
  constants.Name = "NativeXRenderer GTAO constants";
  constants.Size = sizeof(Constants);
  constants.Usage = USAGE_DYNAMIC;
  constants.BindFlags = BIND_UNIFORM_BUFFER;
  constants.CPUAccessFlags = CPU_ACCESS_WRITE;
  device_->CreateBuffer(constants, nullptr, &constants_);
  using namespace gtao_glsl;
  bool ok =
      constants_ &&
      CreateComputePass(prepare_, "NativeXRenderer GTAO prepare", "", kPrepare, {"g_srcSceneDepth"}, {"g_outRawDepth"}) &&
      CreateComputePass(prefilter_, "NativeXRenderer GTAO prefilter", "", kPrefilter, {"g_srcRawDepth"},
                        {"g_outWorkingDepthMIP0", "g_outWorkingDepthMIP1", "g_outWorkingDepthMIP2",
                         "g_outWorkingDepthMIP3", "g_outWorkingDepthMIP4"}) &&
      CreateComputePass(normals_, "NativeXRenderer GTAO normals", "", kNormals, {"g_srcRawDepth"}, {"g_outNormalmap"}) &&
      CreateComputePass(smooth_h_, "NativeXRenderer GTAO smooth normals h", "#define GTAO_SMOOTH_DIR ivec2(1, 0)\n",
                        kSmooth, {"g_srcSmoothNormals", "g_srcSmoothDepth"}, {"g_outSmoothNormals"}) &&
      CreateComputePass(smooth_v_, "NativeXRenderer GTAO smooth normals v", "#define GTAO_SMOOTH_DIR ivec2(0, 1)\n",
                        kSmooth, {"g_srcSmoothNormals", "g_srcSmoothDepth"}, {"g_outSmoothNormals"}) &&
      CreateComputePass(main_low_, "NativeXRenderer GTAO main low", "#define GTAO_SLICES 2\n#define GTAO_STEPS 2\n",
                        kMain, {"g_srcWorkingDepth", "g_srcNormalmap"}, {"g_outWorkingAOTerm", "g_outWorkingEdges"}) &&
      CreateComputePass(main_medium_, "NativeXRenderer GTAO main medium",
                        "#define GTAO_SLICES 3\n#define GTAO_STEPS 3\n", kMain,
                        {"g_srcWorkingDepth", "g_srcNormalmap"}, {"g_outWorkingAOTerm", "g_outWorkingEdges"}) &&
      CreateComputePass(main_high_, "NativeXRenderer GTAO main high", "#define GTAO_SLICES 9\n#define GTAO_STEPS 3\n",
                        kMain, {"g_srcWorkingDepth", "g_srcNormalmap"}, {"g_outWorkingAOTerm", "g_outWorkingEdges"}) &&
      CreateComputePass(denoise_, "NativeXRenderer GTAO denoise", "#define GTAO_DENOISE_FINAL 0\n", kDenoise,
                        {"g_srcWorkingAOTerm", "g_srcWorkingEdges"}, {"g_outFinalAOTerm"}) &&
      CreateComputePass(denoise_last_, "NativeXRenderer GTAO denoise last", "#define GTAO_DENOISE_FINAL 1\n", kDenoise,
                        {"g_srcWorkingAOTerm", "g_srcWorkingEdges"}, {"g_outFinalAOTerm"}) &&
      CreateComputePass(output_pass_, "NativeXRenderer GTAO output", "", kOutput,
                        {"g_srcFinalAOTerm", "g_srcOutputDepth", "g_srcViewDepth"}, {"g_outOcclusion"}) &&
      CreateComputePass(temporal_, "NativeXRenderer GTAO temporal", "", kTemporal,
                        {"g_srcSpatialAO", "g_srcTemporalSceneDepth", "g_srcHistoryAO", "g_srcHistoryDepth"},
                        {"g_outTemporalAO", "g_outTemporalDepth"});
  if (ok) {
    apply_vs_ = CreateShader(device_, SHADER_TYPE_VERTEX, kApplyVS, "NativeXRenderer GTAO apply VS");
    apply_ps_ = CreateShader(device_, SHADER_TYPE_PIXEL, std::string("#version 450\n") + kCommon + kApplyPS,
                             "NativeXRenderer GTAO apply PS");
    ok = apply_vs_ && apply_ps_;
  }
  if (!ok) {
    REXLOG_ERROR("[native_x_renderer] ambient occlusion (GTAO) could not be set up: no ambient occlusion");
    return false;
  }
  ready_ = true;
  REXLOG_INFO("[native_x_renderer] ambient occlusion: XeGTAO ready");
  return true;
}

bool Gtao::CreateComputePass(Pass& pass, const char* name, const std::string& defines, const char* body,
                             std::initializer_list<const char*> textures, std::initializer_list<const char*> images) {
  std::string source = std::string("#version 450\n") + defines + gtao_glsl::kCommon + body;
  RefCntAutoPtr<IShader> shader = CreateShader(device_, SHADER_TYPE_COMPUTE, source, name);
  if (!shader) return false;

  ComputePipelineStateCreateInfo info;
  info.PSODesc.Name = name;
  info.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
  info.pCS = shader;
  std::vector<ShaderResourceVariableDesc> variables;
  std::vector<ImmutableSamplerDesc> samplers;
  const SamplerDesc point = PointClamp();
  for (const char* texture : textures) {
    variables.push_back({SHADER_TYPE_COMPUTE, texture, SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC});
    samplers.push_back({SHADER_TYPE_COMPUTE, texture, point});
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
  if (auto* variable = pass.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "GtaoConstants")) {
    variable->Set(constants_);
  }
  pass.pso->CreateShaderResourceBinding(&pass.srb, true);
  return pass.srb != nullptr;
}

Gtao::Pass* Gtao::ApplyPass(TEXTURE_FORMAT format, bool show) {
  auto key = std::make_pair(format, show);
  auto it = apply_passes_.find(key);
  if (it != apply_passes_.end()) {
    Pass& pass = it->second;
    if (!pass.pso) return nullptr;
    if (!pass.srb) pass.pso->CreateShaderResourceBinding(&pass.srb, true);
    return pass.srb ? &pass : nullptr;
  }
  Pass& pass = apply_passes_[key];
  GraphicsPipelineStateCreateInfo info;
  info.PSODesc.Name = show ? "NativeXRenderer GTAO show" : "NativeXRenderer GTAO apply";
  info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
  info.GraphicsPipeline.NumRenderTargets = 1;
  info.GraphicsPipeline.RTVFormats[0] = format;
  info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
  info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
  info.GraphicsPipeline.RasterizerDesc.ScissorEnable = True;
  info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
  info.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable = False;
  // The alpha channel is the game's (later passes may read it).
  auto& blend = info.GraphicsPipeline.BlendDesc.RenderTargets[0];
  blend.RenderTargetWriteMask = COLOR_MASK_RGB;
  if (!show) {
    // Destination times source: the scene times the occlusion, without reading the target.
    blend.BlendEnable = True;
    blend.SrcBlend = BLEND_FACTOR_ZERO;
    blend.DestBlend = BLEND_FACTOR_SRC_COLOR;
    blend.BlendOp = BLEND_OPERATION_ADD;
    blend.SrcBlendAlpha = BLEND_FACTOR_ZERO;
    blend.DestBlendAlpha = BLEND_FACTOR_ONE;
    blend.BlendOpAlpha = BLEND_OPERATION_ADD;
  }
  info.pVS = apply_vs_;
  info.pPS = apply_ps_;
  ShaderResourceVariableDesc variables[] = {
      {SHADER_TYPE_PIXEL, "g_srcOcclusion", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC}};
  ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "g_srcOcclusion", PointClamp()}};
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
  info.PSODesc.ResourceLayout.Variables = variables;
  info.PSODesc.ResourceLayout.NumVariables = 1;
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
  info.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
  device_->CreateGraphicsPipelineState(info, &pass.pso);
  if (!pass.pso) {
    REXLOG_ERROR("[native_x_renderer] GTAO apply: no pipeline for target format {}", int(format));
    return nullptr;
  }
  if (auto* variable = pass.pso->GetStaticVariableByName(SHADER_TYPE_PIXEL, "GtaoConstants")) {
    variable->Set(constants_);
  }
  pass.pso->CreateShaderResourceBinding(&pass.srb, true);
  return pass.srb ? &pass : nullptr;
}

bool Gtao::EnsureImages(uint32_t pw, uint32_t ph, uint32_t ow, uint32_t oh, bool temporal) {
  auto create = [&](RefCntAutoPtr<ITexture>& texture, const char* name, uint32_t width, uint32_t height,
                    TEXTURE_FORMAT format, uint32_t mips) {
    texture.Release();
    TextureDesc desc;
    desc.Name = name;
    desc.Type = RESOURCE_DIM_TEX_2D;
    desc.Width = std::max(width, 1u);
    desc.Height = std::max(height, 1u);
    desc.MipLevels = mips;
    desc.Format = format;
    desc.Usage = USAGE_DEFAULT;
    desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    device_->CreateTexture(desc, nullptr, &texture);
    return texture != nullptr;
  };
  // The bindings that hold images about to be replaced: made again by the next dispatch.
  auto release_bindings = [&] {
    for (Pass* pass : {&prepare_, &prefilter_, &normals_, &smooth_h_, &smooth_v_, &main_low_, &main_medium_,
                       &main_high_, &denoise_, &denoise_last_, &output_pass_, &temporal_}) {
      pass->srb.Release();
    }
    for (auto& [key, pass] : apply_passes_) pass.srb.Release();
  };
  const bool same = output_ && pw == padded_w_ && ph == padded_h_ && ow == out_w_ && oh == out_h_;
  if (!same) {
    release_bindings();
    padded_w_ = padded_h_ = out_w_ = out_h_ = 0;
    spatial_.Release();
    temporal_depth_.Release();
    history_ao_.Release();
    history_depth_.Release();
    history_frame_ = UINT64_MAX;
    bool ok = create(raw_, "NativeXRenderer GTAO stored depth", pw, ph, TEX_FORMAT_R32_FLOAT, 1) &&
              create(depth_mips_, "NativeXRenderer GTAO view depth", pw, ph, TEX_FORMAT_R32_FLOAT, kDepthMipLevels) &&
              create(normal_map_, "NativeXRenderer GTAO normals", pw, ph, TEX_FORMAT_R32_UINT, 1) &&
              create(term_a_, "NativeXRenderer GTAO term a", pw, ph, TEX_FORMAT_R32_UINT, 1) &&
              create(term_b_, "NativeXRenderer GTAO term b", pw, ph, TEX_FORMAT_R32_UINT, 1) &&
              create(edges_, "NativeXRenderer GTAO edges", pw, ph, TEX_FORMAT_R32_FLOAT, 1) &&
              create(output_, "NativeXRenderer GTAO occlusion", ow, oh, TEX_FORMAT_R32_FLOAT, 1);
    for (uint32_t mip = 0; ok && mip < kDepthMipLevels; ++mip) {
      TextureViewDesc view;
      view.ViewType = TEXTURE_VIEW_UNORDERED_ACCESS;
      view.MostDetailedMip = mip;
      view.NumMipLevels = 1;
      depth_mip_uav_[mip].Release();
      depth_mips_->CreateView(view, &depth_mip_uav_[mip]);
      ok = depth_mip_uav_[mip] != nullptr;
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
      output_.Release();
      return false;
    }
    padded_w_ = pw;
    padded_h_ = ph;
    out_w_ = ow;
    out_h_ = oh;
  }
  if (temporal && !spatial_) {
    history_frame_ = UINT64_MAX;
    bool ok = create(spatial_, "NativeXRenderer GTAO spatial result", ow, oh, TEX_FORMAT_R32_FLOAT, 1) &&
              create(temporal_depth_, "NativeXRenderer GTAO temporal depth", ow, oh, TEX_FORMAT_R32_FLOAT, 1) &&
              create(history_ao_, "NativeXRenderer GTAO history", ow, oh, TEX_FORMAT_R32_FLOAT, 1) &&
              create(history_depth_, "NativeXRenderer GTAO history depth", ow, oh, TEX_FORMAT_R32_FLOAT, 1);
    if (!ok) {
      spatial_.Release();
      return false;
    }
  } else if (!temporal && spatial_) {
    // A live switch to a temporal upscaler (or native_ao_temporal off): the history is its, or none. The local
    // images go, with the bindings that hold them; a switch back starts a fresh history.
    output_pass_.srb.Release();
    temporal_.srb.Release();
    spatial_.Release();
    temporal_depth_.Release();
    history_ao_.Release();
    history_depth_.Release();
    history_frame_ = UINT64_MAX;
  }
  return true;
}

bool Gtao::Dispatch(IDeviceContext* context, Pass& pass, std::initializer_list<Binding> bindings, uint32_t groups_x,
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

void Gtao::Configure(const GtaoSettings& settings) { settings_ = settings; }

void Gtao::ForgetTargets() {
  // The passes that bind the host's depth target (the colour is only a render target, never kept).
  prepare_.srb.Release();
  output_pass_.srb.Release();
  temporal_.srb.Release();
  // The history was worked out from those targets: a new texture (a new render size) starts again.
  history_frame_ = UINT64_MAX;
}

// ---------------------------------------------------------------------------------------------------------------
// native_ao_gpu_time.

void Gtao::TimerBegin(IDeviceContext* context) {
  if (!settings_.gpu_time || queries_failed_ || query_active_) return;
  if (!queries_[0]) {
    if (device_->GetDeviceInfo().Features.DurationQueries != DEVICE_FEATURE_STATE_ENABLED) {
      queries_failed_ = true;
      REXLOG_WARN("[native_x_renderer] native_ao_gpu_time: the device has no duration queries");
      return;
    }
    for (auto& query : queries_) {
      QueryDesc desc;
      desc.Name = "NativeXRenderer GTAO time";
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

void Gtao::TimerEnd(IDeviceContext* context) {
  if (!query_active_) return;
  context->EndQuery(queries_[query_index_]);
  query_pending_[query_index_] = true;
  query_index_ = (query_index_ + 1) % kQueries;
  query_active_ = false;
  if (timer_frames_ >= 600) {
    REXLOG_INFO("[native_x_renderer] ambient occlusion GPU time: {:.3f} ms a frame on average over {} frames (the "
                "longest {:.3f} ms; {} not read in time)",
                timer_total_ms_ / double(timer_frames_), timer_frames_, timer_max_ms_, timer_late_);
    timer_total_ms_ = timer_max_ms_ = 0.0;
    timer_frames_ = timer_late_ = 0;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// The passes.

bool Gtao::Apply(IDeviceContext* context, const Inputs& in) {
  const GtaoSettings& s = settings_;
  if (!ready_ || !context || s.mode <= 0) return false;
  auto refuse = [&](const char* why) {
    if (!warned_inputs_) {
      warned_inputs_ = true;
      REXLOG_WARN("[native_x_renderer] ambient occlusion: {}: none this frame (logged once)", why);
    }
    return false;
  };
  if (!in.depth || !in.color) return refuse("no depth or colour target");
  ITextureView* depth_srv = in.depth->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  if (!depth_srv) return refuse("the depth target has no shader resource view");
  const TextureDesc& dd = in.depth->GetDesc();
  const TextureDesc& cd = in.color->GetTexture()->GetDesc();
  if (in.width < 2 || in.height < 2 || in.x < 0 || in.y < 0 || uint64_t(in.x) + in.width > dd.Width ||
      uint64_t(in.y) + in.height > dd.Height || uint64_t(in.x) + in.width > cd.Width ||
      uint64_t(in.y) + in.height > cd.Height) {
    return refuse("the picture is not inside the depth and colour targets");
  }
  const GtaoProjection& p = in.projection;
  if (!p.valid()) return refuse("no camera");
  const float zs = in.z_max - in.z_min, zo = in.z_min;
  if (zs == 0.0f || !std::isfinite(zs)) return refuse("the viewport's depth range is empty");

  // Half resolution from a render scale of 1.5 (1080p) up (D150: at 1080p full resolution cost 0.73 ms a frame on an
  // RTX 4070 Ti, about 4 on a GTX 1060, half 0.35, and the two looked alike; KK's "auto" had full up to 1.5).
  const bool half = s.quality == GtaoSettings::Quality::kHalf ||
                    (s.quality == GtaoSettings::Quality::kAuto && (in.scale.x >= 1.49f || in.scale.y >= 1.49f));
  const uint32_t step = half ? 2 : 1;
  const uint32_t work_w = (in.width + step - 1) / step, work_h = (in.height + step - 1) / step;
  const uint32_t pw = RoundUp16(work_w), ph = RoundUp16(work_h);
  const bool temporal = s.temporal && !s.temporal_upscaler;
  if (!EnsureImages(pw, ph, in.width, in.height, temporal)) return refuse("its images could not be created");
  origin_x_ = in.x;
  origin_y_ = in.y;
  step_ = step;

  // The constants. Working pixel i is host pixel origin + step * i; XeGTAO's uv is (i + 0.5) / padded size, so the
  // host pixel centre is origin + 0.5 - step / 2 + step * padded * uv. Clip x is -1 at the picture's left edge and
  // +1 at its right, clip y +1 at its top and -1 at its bottom; view x / view z = (clip x - cx) / px, view y alike
  // (y up, as XeGTAO has it). Stored depth = zs * (a + b / view z) + zo.
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
  const float near_plane = p.near_plane();
  const float radius =
      s.radius > 0.0f ? s.radius : RadiusInNearPlanes(s.strength) * near_plane * std::max(s.radius_scale, 0.01f);
  g.effect_radius = radius;
  g.effect_falloff_range = s.falloff > 0.0f ? std::min(s.falloff, 1.0f) : kDefaultFalloffRange;
  g.radius_multiplier = kDefaultRadiusMultiplier;
  g.padding0 = 0.0f;
  g.final_value_power = s.power > 0.0f ? s.power : FinalPower(s.strength);
  g.denoise_blur_beta = kDenoiseBlurBeta;
  g.sample_distribution_power = kDefaultSampleDistributionPower;
  g.thin_occluder_compensation = kDefaultThinOccluderCompensation;
  g.depth_mip_sampling_offset = kDefaultDepthMipSamplingOffset;
  // The noise rotates only where a temporal filter smooths it: the upscaler's history, or a matching camera.
  const bool reprojection_valid = in.reprojection && in.votes >= 3 && in.votes * 2 >= in.offers;
  g.noise_index = (s.temporal_upscaler || (temporal && reprojection_valid)) && in.frame != UINT64_MAX
                      ? int32_t(in.frame % 64)
                      : 0;
  k.source[0] = in.x;
  k.source[1] = in.y;
  k.source[2] = int32_t(in.width);
  k.source[3] = int32_t(in.height);
  k.working[0] = work_w;
  k.working[1] = work_h;
  k.working[2] = pw;
  k.working[3] = ph;
  k.params[0] = fstep;
  // Stored depth at the far plane or beyond (within 1e-6 of the range) is sky; reversed depth tests the other way.
  const float sky_sign = zs > 0.0f ? 1.0f : -1.0f;
  k.params[1] = sky_sign;
  k.params[2] = sky_sign * (zo + zs * (1.0f - 1e-6f));
  k.params[3] = float(s.mode);
  k.fade[0] = s.fade > 0.0f ? s.fade * near_plane : 0.0f;
  k.depth_range[0] = zs;
  k.depth_range[1] = zo;
  const Smoothing smoothing = SmoothingFor(s.normal_smoothing);
  if (smoothing.max_pixels_1080 > 0.0f) {
    const float rad = 3.14159265f / 180.0f;
    k.smooth[0] = std::max(smoothing.max_pixels_1080 * fh / 1080.0f / fstep, 1.0f);
    k.smooth[1] = smoothing.radius_fraction * radius;
    k.smooth[2] = std::cos(smoothing.full_angle_degrees * rad);
    k.smooth[3] = std::cos(smoothing.max_angle_degrees * rad);
    k.smooth2[0] = std::sin(std::min(smoothing.full_angle_degrees, kPlaneFullDegrees) * rad);
    k.smooth2[1] = std::sin(std::min(smoothing.max_angle_degrees, kPlaneMaxDegrees) * rad);
  }
  // A contiguous frame with the same settings and picture reuses the history; cuts and unmatched cameras keep this
  // frame's spatial result, without animated noise.
  HistoryKey key;
  key.mode = s.mode;
  key.strength = s.strength;
  key.samples = s.samples;
  key.normal_smoothing = s.normal_smoothing;
  key.step = step;
  key.radius = s.radius;
  key.radius_scale = s.radius_scale;
  key.falloff = s.falloff;
  key.power = s.power;
  key.fade = s.fade;
  key.x = in.x;
  key.y = in.y;
  key.width = in.width;
  key.height = in.height;
  key.z_min = in.z_min;
  key.z_max = in.z_max;
  if (temporal && reprojection_valid && history_frame_ != UINT64_MAX && in.frame != UINT64_MAX &&
      in.frame == history_frame_ + 1 && key == history_key_) {
    k.temporal[0] = kHistoryWeight;
    for (int i = 0; i < 16; ++i) k.reprojection[i] = float((*in.reprojection)[i]);
    // Last frame's stored depth to its view depth (the same depth range: the key matched).
    k.temporal[1] = -history_projection_.b * zs;
    k.temporal[2] = zs * history_projection_.a + zo;
    ++history_frames_;
  }
  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, &k, sizeof(k));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }

  TimerBegin(context);
  // The depth is read as a texture: not left bound as the depth target (DiligentCore would unbind it with a message).
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  ITextureView* raw_srv = raw_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* raw_uav = raw_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
  ITextureView* depth_mips_srv = depth_mips_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* depth_mip0_srv = depth_mip0_srv_.RawPtr();
  ITextureView* normals_srv = normal_map_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* normals_uav = normal_map_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
  ITextureView* term_a_srv = term_a_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* term_a_uav = term_a_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
  ITextureView* term_b_srv = term_b_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* term_b_uav = term_b_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
  ITextureView* edges_srv = edges_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* edges_uav = edges_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
  ITextureView* output_srv = output_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* output_uav = output_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
  ITextureView* spatial_uav = temporal ? spatial_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS) : output_uav;

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
      Dispatch(context, normals_, {{"g_srcRawDepth", raw_srv}, {"g_outNormalmap", normals_uav}}, pw / 8, ph / 8);
  if (ok && smoothing.max_pixels_1080 > 0.0f) {
    // In place through term b, free until the first denoise writes it.
    ok = Dispatch(context, smooth_h_,
                  {{"g_srcSmoothNormals", normals_srv},
                   {"g_srcSmoothDepth", depth_mip0_srv},
                   {"g_outSmoothNormals", term_b_uav}},
                  pw / 8, ph / 8) &&
         Dispatch(context, smooth_v_,
                  {{"g_srcSmoothNormals", term_b_srv},
                   {"g_srcSmoothDepth", depth_mip0_srv},
                   {"g_outSmoothNormals", normals_uav}},
                  pw / 8, ph / 8);
  }
  Pass& main_pass = s.samples <= 1 ? main_low_ : s.samples == 2 ? main_medium_ : main_high_;
  ok = ok &&
       Dispatch(context, main_pass,
                {{"g_srcWorkingDepth", depth_mips_srv},
                 {"g_srcNormalmap", normals_srv},
                 {"g_outWorkingAOTerm", term_a_uav},
                 {"g_outWorkingEdges", edges_uav}},
                pw / 8, ph / 8) &&
       Dispatch(context, denoise_,
                {{"g_srcWorkingAOTerm", term_a_srv}, {"g_srcWorkingEdges", edges_srv}, {"g_outFinalAOTerm", term_b_uav}},
                pw / 16, ph / 8) &&
       Dispatch(context, denoise_last_,
                {{"g_srcWorkingAOTerm", term_b_srv}, {"g_srcWorkingEdges", edges_srv}, {"g_outFinalAOTerm", term_a_uav}},
                pw / 16, ph / 8) &&
       Dispatch(context, output_pass_,
                {{"g_srcFinalAOTerm", term_a_srv},
                 {"g_srcOutputDepth", depth_srv},
                 {"g_srcViewDepth", depth_mip0_srv},
                 {"g_outOcclusion", spatial_uav}},
                (in.width + 7) / 8, (in.height + 7) / 8);
  if (ok && temporal) {
    ok = Dispatch(context, temporal_,
                  {{"g_srcSpatialAO", spatial_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)},
                   {"g_srcTemporalSceneDepth", depth_srv},
                   {"g_srcHistoryAO", history_ao_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)},
                   {"g_srcHistoryDepth", history_depth_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)},
                   {"g_outTemporalAO", output_uav},
                   {"g_outTemporalDepth", temporal_depth_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS)}},
                  (in.width + 7) / 8, (in.height + 7) / 8);
    if (ok) {
      CopyTextureAttribs ao{output_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, history_ao_,
                            RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
      context->CopyTexture(ao);
      CopyTextureAttribs depth{temporal_depth_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, history_depth_,
                               RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
      context->CopyTexture(depth);
      history_frame_ = in.frame;
      history_key_ = key;
      history_projection_ = p;
    }
  }
  if (!ok || !temporal) history_frame_ = UINT64_MAX;

  // Into the scene's colour: one triangle over the picture.
  Pass* apply = ok ? ApplyPass(in.color->GetDesc().Format, s.mode >= 2) : nullptr;
  if (apply) {
    ITextureView* color = in.color;
    context->SetRenderTargets(1, &color, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    apply->srb->GetVariableByName(SHADER_TYPE_PIXEL, "g_srcOcclusion")->Set(output_srv);
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
    context->SetPipelineState(apply->pso);
    context->CommitShaderResources(apply->srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    DrawAttribs draw;
    draw.NumVertices = 3;
    context->Draw(draw);
    // The renderer binds its own targets again (DrawPath::TargetsChanged).
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  }
  TimerEnd(context);
  if (!apply) return refuse("a pass could not be bound or the colour format has no pipeline");

  if (computed_ % 1800 == 0) {
    // The camera and the radius it gives (KK's docs/ao.md, "Strength and radius").
    REXLOG_INFO("[native_x_renderer] ambient occlusion: {}x{} at {},{} ({} resolution, {} samples, smoothing {}, "
                "{}); near {:.4f}, far {:.1f}, px {:.3f}, py {:.3f}; radius {:.4f} world units (strength {}); {} "
                "frames used history",
                in.width, in.height, in.x, in.y, half ? "half" : "full",
                s.samples <= 1 ? "low" : s.samples == 2 ? "medium" : "high", s.normal_smoothing,
                s.temporal_upscaler ? "the upscaler's history" : temporal ? "local history" : "no history",
                near_plane, p.far_plane(), p.px, p.py, radius, s.strength, history_frames_);
  }
  ++computed_;
  ++applied_;
  return true;
}

}  // namespace native_x_renderer
