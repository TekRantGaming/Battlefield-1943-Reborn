// NativeXRenderer - see blit.h.

#include "blit.h"

#include "glsl.h"

#include "GraphicsAccessories.hpp"

#include <rex/logging.h>

#include <cmath>
#include <cstring>
#include <string>

namespace native_x_renderer {

using namespace Diligent;

namespace {

// One triangle covering the viewport; uv is 0..1 across it (top left 0, 0). DiligentCore's Vulkan backend
// flips Y (D19): NDC y = -1 is the bottom of the viewport.
constexpr const char* kVertexShader = R"(#version 450
layout(location = 0) out vec2 v_uv;
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  v_uv = vec2(p.x, 1.0 - p.y);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The constants every pixel shader reads.
constexpr const char* kConstants = R"(
layout(set = 0, binding = 1) uniform BlitConstants {
  ivec4 source_origin;   // xy: first source texel
  ivec4 target_origin;   // xy: first target texel (the viewport's origin)
  ivec4 options;         // x: swap red and blue, y: samples, z: sample (-1 = average), w: swizzle (scaled)
  vec4 scale;            // x: color factor; y: not 0 for a 2_10_10_10_FLOAT source; z: kPackedToDepth's tile scale;
                         // w: not 0 for a 16-bit fixed-point source (D139)
  ivec4 gamma;           // x: the gamma ramp to apply, 0: the 256-entry table, 1: piecewise linear
  ivec4 depth;           // x: kDepthStencilPacked, 1 for float24 (20e4) depth, 0 for unorm24
  vec4 fill;             // kFillColor: the color; kFillDepthStencil: x the depth
};
// A 2_10_10_10_FLOAT render target's value as the guest's format holds it: RGB in the 7e3 range, alpha in 0
// to 1 at 2 bits.
vec4 GuestRange(vec4 c) {
  // A 16-bit fixed-point target: -32 to 32 as the Xenos stores it, NaN as 0 (D139).
  if (scale.w != 0.0) return clamp(mix(c, vec4(0.0), isnan(c)), vec4(-32.0), vec4(32.0));
  if (scale.y == 0.0) return c;
  return vec4(clamp(c.rgb, 0.0, 31.875), round(clamp(c.a, 0.0, 1.0) * 3.0) / 3.0);
}
)";

constexpr const char* kColorBody = R"(
layout(set = 0, binding = 0) uniform sampler2D source;
layout(location = 0) out vec4 color;
void main() {
  vec4 c = GuestRange(texelFetch(source, ivec2(gl_FragCoord.xy) - target_origin.xy + source_origin.xy, 0));
  if (options.x != 0) c = c.bgra;
  color = c * scale.x;
}
)";

constexpr const char* kColorMsaaBody = R"(
layout(set = 0, binding = 0) uniform sampler2DMS source;
layout(location = 0) out vec4 color;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy) - target_origin.xy + source_origin.xy;
  vec4 c;
  if (options.z >= 0) {
    c = GuestRange(texelFetch(source, p, options.z));
  } else {
    c = vec4(0.0);
    for (int i = 0; i < options.y; ++i) c += GuestRange(texelFetch(source, p, i));
    c /= float(options.y);
  }
  if (options.x != 0) c = c.bgra;
  color = c * scale.x;
}
)";

constexpr const char* kDepthBody = R"(
layout(set = 0, binding = 0) uniform sampler2D source;
layout(location = 0) out vec4 color;
void main() {
  color = vec4(texelFetch(source, ivec2(gl_FragCoord.xy) - target_origin.xy + source_origin.xy, 0).r);
}
)";

constexpr const char* kDepthMsaaBody = R"(
layout(set = 0, binding = 0) uniform sampler2DMS source;
layout(location = 0) out vec4 color;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy) - target_origin.xy + source_origin.xy;
  color = vec4(texelFetch(source, p, max(options.z, 0)).r);
}
)";

constexpr const char* kScaledBody = R"(
layout(set = 0, binding = 0) uniform sampler2D source;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 color;
void main() {
  vec2 size = vec2(textureSize(source, 0));
  vec2 uv = (vec2(source_origin.xy) + v_uv * vec2(source_origin.zw)) / size;
  vec4 c = textureLod(source, uv, 0.0);
  if (options.x != 0) c = c.bgra;
  c = GuestRange(c);
  vec4 swizzled;
  for (int i = 0; i < 4; ++i) {
    int component = (options.w >> (3 * i)) & 7;
    swizzled[i] = component < 4 ? c[component] : float(component & 1);
  }
  color = swizzled * scale.x;
}
)";

constexpr const char* kDepthScaledBody = R"(
layout(set = 0, binding = 0) uniform sampler2D source;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 color;
void main() {
  color = vec4(texelFetch(source, ivec2(vec2(source_origin.xy) + v_uv * vec2(source_origin.zw)), 0).r);
}
)";

constexpr const char* kDepthToDepthBody = R"(
layout(set = 0, binding = 0) uniform sampler2D source;
layout(location = 0) in vec2 v_uv;
void main() {
  gl_FragDepth = texelFetch(source, ivec2(vec2(source_origin.xy) + v_uv * vec2(source_origin.zw)), 0).r;
}
)";

// The 32-bit words a resolve of a depth-stencil surface into a k_24_8 or k_24_8_FLOAT texture writes, the
// depth in bits 8:31 and the stencil in 0:7, as 4 bytes, low first: what a fetch of that memory as
// k_8_8_8_8 reads (D64). Float24 depth is encoded as the SDK's Float32To20e4 does, rounding to nearest even.
constexpr const char* kDepthStencilPackedBody = R"(
layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 2) uniform usampler2D stencil;
layout(location = 0) out vec4 color;
uint Float32To20e4(float f) {
  if (!(f > 0.0)) return 0u;
  uint u = floatBitsToUint(f);
  if (u >= 0x3FFFFFF8u) return 0xFFFFFFu;
  if (u < 0x38800000u) {
    u = (0x800000u | (u & 0x7FFFFFu)) >> min(113u - (u >> 23u), 24u);
  } else {
    u += 0xC8000000u;
  }
  u += 3u + ((u >> 3u) & 1u);
  return (u >> 3u) & 0xFFFFFFu;
}
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy) - target_origin.xy + source_origin.xy;
  float d = texelFetch(source, p, 0).r;
  uint s = texelFetch(stencil, p, 0).r & 0xFFu;
  uint d24 = depth.x != 0 ? Float32To20e4(d) : uint(clamp(d, 0.0, 1.0) * 16777215.0 + 0.5);
  color = vec4(uvec4(s, d24 & 0xFFu, (d24 >> 8u) & 0xFFu, d24 >> 16u)) / 255.0;
}
)";

// The other way (D113): such words, as a color target holds them after a draw wrote them (R the low byte), their
// depth into a depth target, each pixel from the other 40-pixel half of its 80-pixel EDRAM tile (a depth surface's
// tiles have their halves swapped from a color surface's; at 1x MSAA a sample is a pixel). Float24 depth is decoded
// as the SDK's Float20e4To32 does.
constexpr const char* kPackedToDepthBody = R"(
layout(set = 0, binding = 0) uniform sampler2D source;
layout(location = 0) in vec2 v_uv;
float Float20e4To32(uint f) {
  if (f == 0u) return 0.0;
  int exponent = int(f >> 20u);
  uint mantissa = f & 0xFFFFFu;
  if (exponent == 0) {
    int shift = 20 - findMSB(mantissa);
    exponent = 1 - shift;
    mantissa = (mantissa << uint(shift)) & 0xFFFFFu;
  }
  return uintBitsToFloat(uint(exponent + 112) << 23u | mantissa << 3u);
}
void main() {
  vec2 p = vec2(source_origin.xy) + v_uv * vec2(source_origin.zw);
  float guest_x = p.x / scale.z, tile = floor(guest_x / 80.0) * 80.0;
  p.x = (tile + mod(guest_x - tile + 40.0, 80.0)) * scale.z;
  vec4 c = texelFetch(source, ivec2(p), 0);
  uvec4 b = uvec4(c * 255.0 + 0.5);
  uint d24 = b.g | b.b << 8u | b.a << 16u;
  gl_FragDepth = depth.x != 0 ? Float20e4To32(d24) : float(d24) / 16777215.0;
}
)";

// A clear of a rectangle (D80): the constant color, or the depth (the stencil is the pipeline's reference).
constexpr const char* kFillColorBody = R"(
layout(location = 0) out vec4 color;
void main() {
  color = fill;
}
)";

constexpr const char* kFillDepthStencilBody = R"(
void main() {
  gl_FragDepth = fill.x;
}
)";

// The guest's display gamma ramp (gamma.h), as the SDK's apply_gamma shaders apply it: the table maps each
// 8-bit channel to 10 bits (red in bits 20:29, green 10:19, blue 0:9); the piecewise linear ramp takes the
// 10-bit value, its 7 high bits choose an entry per channel (base in bits 0:15, delta in 16:31) and the low
// 3 bits interpolate within it.
constexpr const char* kGammaBody = R"(
layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 2) uniform usampler2D ramp;
layout(location = 0) out vec4 color;
float ApplyRamp(float value, int channel) {
  value = clamp(value, 0.0, 1.0);
  if (gamma.x == 0) {
    uint entry = texelFetch(ramp, ivec2(int(value * 255.0 + 0.5), 0), 0).r;
    return float((entry >> uint(20 - 10 * channel)) & 1023u) / 1023.0;
  }
  uint v = uint(value * 1023.0 + 0.5);
  uint entry = texelFetch(ramp, ivec2(int(v >> 3u), 1 + channel), 0).r;
  float result = float(entry & 0xFFFFu) + float((v & 7u) * (entry >> 16u)) * 0.125;
  return clamp(result * 1.52737048e-05, 0.0, 1.0);
}
void main() {
  vec4 c = texelFetch(source, ivec2(gl_FragCoord.xy) - target_origin.xy + source_origin.xy, 0);
  vec4 swizzled;
  for (int i = 0; i < 4; ++i) {
    int component = (options.w >> (3 * i)) & 7;
    swizzled[i] = component < 4 ? c[component] : float(component & 1);
  }
  color = vec4(ApplyRamp(swizzled.r, 0), ApplyRamp(swizzled.g, 1), ApplyRamp(swizzled.b, 2), 1.0);
}
)";

struct Constants {
  int32_t source_origin[4];
  int32_t target_origin[4];
  int32_t options[4];
  float scale[4];
  int32_t gamma[4];
  int32_t depth[4];
  float fill[4];
};

RefCntAutoPtr<IShader> CreateShader(IRenderDevice* device, SHADER_TYPE type, const std::vector<uint32_t>& spirv,
                                    const char* name) {
  RefCntAutoPtr<IShader> shader;
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

// D140: one guest texel a thread, from the middle of its host texels, into the guest memory mirror (a raw buffer of
// 32-bit words) at the guest's address for it: linear rows, or the Xenos 2D tiling (the SDK's GetTiledOffset2D),
// each word in the guest's endianness.
const char kMirrorWriteShader[] = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2DArray XeMirrorSource;
layout(std430, set = 0, binding = 1) buffer XeMirrorDest { uint xe_mirror[]; };
layout(std140, set = 0, binding = 2) uniform XeMirrorConstants {
  uvec4 a;  // address in words, width, height, pitch (texels)
  uvec4 b;  // tiled, endian, words per texel, log2 of the bytes per texel
  vec4 s;   // host texels per guest texel across and down, the slice
};
uint Swap(uint v, uint endian) {
  if (endian == 1u) return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
  if (endian == 2u) return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
  if (endian == 3u) return (v << 16) | (v >> 16);
  return v;
}
int TiledOffset2D(int x, int y, uint pitch, int bpb_log2) {
  int p = int((pitch + 31u) & ~31u);
  int macro_ = ((x >> 5) + (y >> 5) * (p >> 5)) << (bpb_log2 + 7);
  int micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2;
  int offset = macro_ + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}
void main() {
  uvec2 p = gl_GlobalInvocationID.xy;
  if (p.x >= a.y || p.y >= a.z) return;
  ivec3 h = ivec3(int((float(p.x) + 0.5) * s.x), int((float(p.y) + 0.5) * s.y), int(s.z));
  vec4 v = texelFetch(XeMirrorSource, h, 0);
  uint words = b.z;
  uint offset = b.x != 0u ? uint(TiledOffset2D(int(p.x), int(p.y), a.w, int(b.w))) : (p.y * a.w + p.x) * words * 4u;
  uint w = a.x + offset / 4u;
  xe_mirror[w] = Swap(floatBitsToUint(v.x), b.y);
  if (words > 1u) xe_mirror[w + 1u] = Swap(floatBitsToUint(v.y), b.y);
  if (words > 2u) {
    xe_mirror[w + 2u] = Swap(floatBitsToUint(v.z), b.y);
    xe_mirror[w + 3u] = Swap(floatBitsToUint(v.w), b.y);
  }
}
)";

struct MirrorConstants {
  uint32_t a[4];
  uint32_t b[4];
  float s[4];
};

}  // namespace

bool Blitter::WriteToMirror(IDeviceContext* context, ITextureView* source_array, IBuffer* mirror,
                            IBufferView* mirror_uav, const MirrorWrite& write) {
  uint32_t words = 0, bytes_log2 = 0;
  switch (write.guest_format) {
    case 36: words = 1, bytes_log2 = 2; break;  // k_32_FLOAT
    case 37: words = 2, bytes_log2 = 3; break;  // k_32_32_FLOAT
    case 38: words = 4, bytes_log2 = 4; break;  // k_32_32_32_32_FLOAT
    default: return false;
  }
  if (!context || !source_array || !mirror || !mirror_uav || !write.width || !write.height) return false;
  if (!mirror_tried_) {
    mirror_tried_ = true;
    RefCntAutoPtr<IShader> shader =
        CreateShader(device_, SHADER_TYPE_COMPUTE, CompileGlsl(GlslStage::kCompute, kMirrorWriteShader, "mirror write"),
                     "NativeXRenderer mirror write CS");
    BufferDesc constants;
    constants.Name = "NativeXRenderer mirror write constants";
    constants.Size = sizeof(MirrorConstants);
    constants.Usage = USAGE_DYNAMIC;
    constants.BindFlags = BIND_UNIFORM_BUFFER;
    constants.CPUAccessFlags = CPU_ACCESS_WRITE;
    device_->CreateBuffer(constants, nullptr, &mirror_constants_);
    if (shader && mirror_constants_) {
      ComputePipelineStateCreateInfo info;
      info.PSODesc.Name = "NativeXRenderer mirror write";
      info.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
      info.pCS = shader;
      ShaderResourceVariableDesc variables[] = {
          {SHADER_TYPE_COMPUTE, "XeMirrorSource", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
          {SHADER_TYPE_COMPUTE, "XeMirrorDest", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC}};
      SamplerDesc point;
      point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
      point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
      ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_COMPUTE, "XeMirrorSource", point}};
      info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
      info.PSODesc.ResourceLayout.Variables = variables;
      info.PSODesc.ResourceLayout.NumVariables = 2;
      info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
      info.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
      device_->CreateComputePipelineState(info, &mirror_.pso);
      if (mirror_.pso) {
        if (auto* variable = mirror_.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "XeMirrorConstants"))
          variable->Set(mirror_constants_);
        mirror_.pso->CreateShaderResourceBinding(&mirror_.srb, true);
      }
    }
    if (!mirror_.srb) REXLOG_ERROR("[native_x_renderer] mirror write (D140): the compute pass could not be made");
  }
  if (!mirror_.srb) return false;
  {
    void* data = nullptr;
    context->MapBuffer(mirror_constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    if (!data) return false;
    MirrorConstants c{};
    c.a[0] = write.address >> 2, c.a[1] = write.width, c.a[2] = write.height, c.a[3] = write.pitch;
    c.b[0] = write.tiled ? 1u : 0u, c.b[1] = write.endian, c.b[2] = words, c.b[3] = bytes_log2;
    c.s[0] = write.scale_x, c.s[1] = write.scale_y, c.s[2] = float(write.slice);
    std::memcpy(data, &c, sizeof(c));
    context->UnmapBuffer(mirror_constants_, MAP_WRITE);
  }
  IShaderResourceVariable* source_var = mirror_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "XeMirrorSource");
  IShaderResourceVariable* dest_var = mirror_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "XeMirrorDest");
  if (!source_var || !dest_var) return false;
  source_var->Set(source_array);
  dest_var->Set(mirror_uav);
  context->SetPipelineState(mirror_.pso);
  context->CommitShaderResources(mirror_.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DispatchComputeAttribs dispatch((write.width + 7) / 8, (write.height + 7) / 8, 1);
  context->DispatchCompute(dispatch);
  // Back to the state the draws read the mirror in (GuestMemory::PrepareForRead), so its bookkeeping stays true.
  StateTransitionDesc barrier{mirror, RESOURCE_STATE_UNKNOWN,
                              RESOURCE_STATE_SHADER_RESOURCE | RESOURCE_STATE_INDEX_BUFFER,
                              STATE_TRANSITION_FLAG_UPDATE_STATE};
  context->TransitionResourceStates(1, &barrier);
  return true;
}

bool Blitter::Initialize(IRenderDevice* device) {
  device_ = device;
  vertex_shader_ = CreateShader(device, SHADER_TYPE_VERTEX, CompileGlsl(GlslStage::kVertex, kVertexShader, "blit vs"),
                                "NativeXRenderer blit VS");
  struct {
    Source kind;
    const char* body;
    const char* name;
  } pixel[] = {
      {Source::kColor, kColorBody, "NativeXRenderer blit color PS"},
      {Source::kColorMsaa, kColorMsaaBody, "NativeXRenderer blit color MSAA PS"},
      {Source::kDepth, kDepthBody, "NativeXRenderer blit depth PS"},
      {Source::kDepthMsaa, kDepthMsaaBody, "NativeXRenderer blit depth MSAA PS"},
      {Source::kColorScaled, kScaledBody, "NativeXRenderer blit scaled PS"},
      {Source::kColorGamma, kGammaBody, "NativeXRenderer blit gamma PS"},
      {Source::kDepthScaled, kDepthScaledBody, "NativeXRenderer blit depth scaled PS"},
      {Source::kDepthToDepth, kDepthToDepthBody, "NativeXRenderer blit depth to depth PS"},
      {Source::kDepthStencilPacked, kDepthStencilPackedBody, "NativeXRenderer blit depth stencil packed PS"},
      {Source::kPackedToDepth, kPackedToDepthBody, "NativeXRenderer blit packed to depth PS"},
      {Source::kFillColor, kFillColorBody, "NativeXRenderer fill color PS"},
      {Source::kFillDepthStencil, kFillDepthStencilBody, "NativeXRenderer fill depth stencil PS"},
  };
  for (const auto& p : pixel) {
    std::string source = std::string("#version 450\n") + kConstants + p.body;
    pixel_shaders_[p.kind] = CreateShader(device, SHADER_TYPE_PIXEL, CompileGlsl(GlslStage::kPixel, source, p.name), p.name);
    if (!pixel_shaders_[p.kind]) return false;
  }
  BufferDesc constants;
  constants.Name = "NativeXRenderer blit constants";
  constants.Size = sizeof(Constants);
  constants.Usage = USAGE_DYNAMIC;
  constants.BindFlags = BIND_UNIFORM_BUFFER;
  constants.CPUAccessFlags = CPU_ACCESS_WRITE;
  device->CreateBuffer(constants, nullptr, &constants_);
  return vertex_shader_ && constants_;
}

Blitter::Pipeline* Blitter::GetPipeline(Source kind, TEXTURE_FORMAT target_format, uint8_t variant) {
  auto key = std::make_tuple(kind, target_format, variant);
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) return it->second.pso ? &it->second : nullptr;
  Pipeline& pipeline = pipelines_[key];

  GraphicsPipelineStateCreateInfo info;
  std::string name = "NativeXRenderer blit " + std::to_string(int(kind)) + "/" + std::to_string(int(target_format)) +
                     "/" + std::to_string(int(variant));
  info.PSODesc.Name = name.c_str();
  info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
  // kDepthToDepth and kPackedToDepth write depth only, always; kFillDepthStencil the depth and/or the stencil, always.
  bool fill = kind == Source::kFillColor || kind == Source::kFillDepthStencil;
  bool depth_target =
      kind == Source::kDepthToDepth || kind == Source::kPackedToDepth || kind == Source::kFillDepthStencil;
  info.GraphicsPipeline.NumRenderTargets = depth_target ? 0 : 1;
  info.GraphicsPipeline.RTVFormats[0] = depth_target ? TEX_FORMAT_UNKNOWN : target_format;
  info.GraphicsPipeline.DSVFormat = depth_target ? target_format : TEX_FORMAT_UNKNOWN;
  info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
  info.GraphicsPipeline.RasterizerDesc.ScissorEnable = True;
  info.GraphicsPipeline.DepthStencilDesc.DepthEnable = depth_target;
  info.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable =
      kind == Source::kFillDepthStencil ? (variant & 1) != 0 : depth_target;
  info.GraphicsPipeline.DepthStencilDesc.DepthFunc = COMPARISON_FUNC_ALWAYS;
  if (kind == Source::kFillDepthStencil && (variant & 2)) {
    auto& ds = info.GraphicsPipeline.DepthStencilDesc;
    ds.StencilEnable = True;
    ds.StencilReadMask = ds.StencilWriteMask = 0xFF;
    for (StencilOpDesc* face : {&ds.FrontFace, &ds.BackFace}) {
      face->StencilFunc = COMPARISON_FUNC_ALWAYS;
      face->StencilPassOp = face->StencilDepthFailOp = face->StencilFailOp = STENCIL_OP_REPLACE;
    }
  }
  info.pVS = vertex_shader_;
  info.pPS = pixel_shaders_[kind];

  // kColorGamma also reads the ramp, kDepthStencilPacked the stencil: integer textures, point sampling (they
  // are only fetched).
  const char* second = kind == Source::kColorGamma ? "ramp" : kind == Source::kDepthStencilPacked ? "stencil" : nullptr;
  ShaderResourceVariableDesc variables[] = {
      {SHADER_TYPE_PIXEL, "source", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
      {SHADER_TYPE_PIXEL, second, SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
  };
  SamplerDesc linear;
  linear.MinFilter = linear.MagFilter = linear.MipFilter = FILTER_TYPE_LINEAR;
  linear.AddressU = linear.AddressV = linear.AddressW = TEXTURE_ADDRESS_CLAMP;
  SamplerDesc point;
  point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
  point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
  ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "source", linear}, {SHADER_TYPE_PIXEL, second, point}};
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
  info.PSODesc.ResourceLayout.Variables = variables;
  info.PSODesc.ResourceLayout.NumVariables = fill ? 0 : second ? 2 : 1;
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
  info.PSODesc.ResourceLayout.NumImmutableSamplers = fill ? 0 : second ? 2 : 1;
  device_->CreateGraphicsPipelineState(info, &pipeline.pso);
  if (!pipeline.pso) {
    REXLOG_ERROR("[native_x_renderer] blit: no pipeline for source {} and target format {}", int(kind),
                 int(target_format));
    return nullptr;
  }
  if (auto* variable = pipeline.pso->GetStaticVariableByName(SHADER_TYPE_PIXEL, "BlitConstants")) {
    variable->Set(constants_);
  }
  pipeline.pso->CreateShaderResourceBinding(&pipeline.srb, true);
  return &pipeline;
}

void Blitter::Copy(IDeviceContext* context, ITextureView* source, Source kind, const Rect& from,
                   ITextureView* target, const Rect& to, const Options& options) {
  if (!source || !target || !to.width || !to.height) return;
  if (kind == Source::kColorGamma && !options.ramp) return;
  if (kind == Source::kDepthStencilPacked && !options.stencil) return;
  Pipeline* pipeline = GetPipeline(kind, target->GetDesc().Format);
  if (!pipeline) return;

  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    Constants c{};
    c.source_origin[0] = from.x;
    c.source_origin[1] = from.y;
    c.source_origin[2] = int32_t(from.width);
    c.source_origin[3] = int32_t(from.height);
    c.target_origin[0] = to.x;
    c.target_origin[1] = to.y;
    c.options[0] = options.swap_red_blue ? 1 : 0;
    c.options[1] = int32_t(options.samples);
    c.options[2] = options.sample;
    c.options[3] = int32_t(options.swizzle);
    c.scale[0] = options.scale;
    c.scale[1] = options.float_7e3 ? 1.0f : 0.0f;
    c.scale[2] = options.tile_scale_x;
    c.scale[3] = options.fixed16 ? 1.0f : 0.0f;
    c.gamma[0] = options.gamma_pwl ? 1 : 0;
    c.depth[0] = options.depth_float24 ? 1 : 0;
    std::memcpy(data, &c, sizeof(c));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }

  pipeline->srb->GetVariableByName(SHADER_TYPE_PIXEL, "source")->Set(source);
  if (kind == Source::kColorGamma) pipeline->srb->GetVariableByName(SHADER_TYPE_PIXEL, "ramp")->Set(options.ramp);
  if (kind == Source::kDepthStencilPacked) {
    pipeline->srb->GetVariableByName(SHADER_TYPE_PIXEL, "stencil")->Set(options.stencil);
  }
  Draw(context, pipeline, target, kind == Source::kDepthToDepth || kind == Source::kPackedToDepth, to);
}

void Blitter::FillColor(IDeviceContext* context, ITextureView* target, const Rect& to, const float color[4]) {
  if (!target || !to.width || !to.height) return;
  Pipeline* pipeline = GetPipeline(Source::kFillColor, target->GetDesc().Format);
  if (!pipeline) return;
  void* data = nullptr;
  context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
  Constants c{};
  std::memcpy(c.fill, color, sizeof(c.fill));
  std::memcpy(data, &c, sizeof(c));
  context->UnmapBuffer(constants_, MAP_WRITE);
  Draw(context, pipeline, target, false, to);
}

void Blitter::FillDepthStencil(IDeviceContext* context, ITextureView* target, const Rect& to, bool write_depth,
                               float depth, bool write_stencil, uint8_t stencil) {
  if (!target || !to.width || !to.height) return;
  write_stencil = write_stencil &&
                  GetTextureFormatAttribs(target->GetDesc().Format).ComponentType == COMPONENT_TYPE_DEPTH_STENCIL;
  if (!write_depth && !write_stencil) return;
  Pipeline* pipeline = GetPipeline(Source::kFillDepthStencil, target->GetDesc().Format,
                                   uint8_t((write_depth ? 1 : 0) | (write_stencil ? 2 : 0)));
  if (!pipeline) return;
  void* data = nullptr;
  context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
  Constants c{};
  c.fill[0] = depth;
  std::memcpy(data, &c, sizeof(c));
  context->UnmapBuffer(constants_, MAP_WRITE);
  if (write_stencil) context->SetStencilRef(stencil);
  Draw(context, pipeline, target, true, to);
}

void Blitter::Draw(IDeviceContext* context, Pipeline* pipeline, ITextureView* target, bool depth_target,
                   const Rect& to) {
  if (depth_target) {
    context->SetRenderTargets(0, nullptr, target, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  } else {
    context->SetRenderTargets(1, &target, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  }
  Viewport viewport;
  viewport.TopLeftX = float(to.x);
  viewport.TopLeftY = float(to.y);
  viewport.Width = float(to.width);
  viewport.Height = float(to.height);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  context->SetViewports(1, &viewport, 0, 0);
  Diligent::Rect scissor{to.x, to.y, to.x + int32_t(to.width), to.y + int32_t(to.height)};
  context->SetScissorRects(1, &scissor, 0, 0);
  context->SetPipelineState(pipeline->pso);
  context->CommitShaderResources(pipeline->srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DrawAttribs draw;
  draw.NumVertices = 3;
  context->Draw(draw);
}

}  // namespace native_x_renderer
