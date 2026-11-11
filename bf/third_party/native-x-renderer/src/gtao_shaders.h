// NativeXRenderer - the GLSL 450 compute and pixel shaders of the ambient occlusion (gtao.h), compiled at startup
// by glslang (glsl.h, D16).
//
// A port of King Kong Recompiled's backend/shaders/gtao.hlsl (Copyright (c) 2026 TekRantGaming and contributors,
// MIT: thirdparty/king-kong-recompiled/LICENSE) and of the parts of XeGTAO it uses, from HLSL to GLSL:
//
//   XeGTAO, version 1.30. Copyright (C) 2016-2021, Intel Corporation. SPDX-License-Identifier: MIT.
//   Based on GTAO/GTSO, Jimenez et al., "Practical Real-Time Strategies for Accurate Indirect Occlusion".
//   Implementation: Filip Strugar and Steve Mccalla. https://github.com/GameTechDev/XeGTAO
//   The sample's normal decoding and noise (vaGTAO.hlsl) are under the same licence.
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//   documentation files (the "Software"), to deal in the Software without restriction, including without
//   limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
//   Software, and to permit persons to whom the Software is furnished to do so, subject to the following
//   conditions: The above copyright notice and this permission notice shall be included in all copies or
//   substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
//   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE
//   AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
//   OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
//   WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// XeGTAO's math, constants and passes are kept as they are, with KK's choices: 32-bit depths
// (XE_GTAO_FP32_DEPTHS), full precision, XE_GTAO_USE_DEFAULT_CONSTANTS 1 (XeGTAO.h's default: the radius
// multiplier, sample distribution power and the falloff range's share are XeGTAO's own; EffectRadius,
// EffectFalloffRange's start, FinalValuePower, DenoiseBlurBeta and DepthMIPSamplingOffset come from the
// constants), no bent normals. What changes is the language only:
//   - HLSL's GatherRed is GLSL's textureGather (the same four texels in the same order, Vulkan's and D3D's);
//     SampleLevel is textureLod; Load is texelFetch; RWTexture2D stores are imageStore.
//   - The edges image is R32_FLOAT and the AO terms and normals are R32_UINT: every Vulkan device stores to those
//     without shaderStorageImageExtendedFormats (XeGTAO's own R8 images would need it).
//   - The functions read the passes' textures by their global names instead of taking them as parameters.
//   - The camera mapping is NativeXRenderer's: the picture is a rectangle of host pixels, clip space is D3D's (x
//     right, y up, z 0 to 1) and the viewport's depth range is z_min to z_max (gtao.cpp, FillConstants).
//
// Each pass is a separate program: "#version 450", its defines, kCommon, then its own part. Each part is a raw
// string under MSVC's 16 KB limit for one literal.
//
//   kPrepare      the picture's stored depth into the working image (edges replicated; every second pixel at
//                 half resolution)
//   kPrefilter    XeGTAO_PrefilterDepths16x16: view-space depth and its four smaller mips
//   kNormals      XeGTAO_ComputeViewspaceNormal: normals rebuilt from the depth (no normal buffer)
//   kSmooth       KK's normal smoothing (native_ao_normal_smoothing), horizontal or vertical (GTAO_SMOOTH_DIR)
//   kMain         XeGTAO_MainPass, GTAO_SLICES slices of GTAO_STEPS steps (Low 2x2, Medium 3x3, High 9x3)
//   kDenoise      XeGTAO_Denoise, twice (GTAO_DENOISE_FINAL 1 on the last: the term scaled back)
//   kOutput       the occlusion per host pixel of the picture (R32F, 1 = none), upsampled by depth at half
//                 resolution, the sky (stored depth at the far plane) exactly 1
//   kTemporal     KK's local temporal accumulation (without a temporal upscaler)
//   kApplyVS/PS   one triangle over the picture: the scene colour times the occlusion (a multiplicative blend,
//                 gtao.cpp) or, with ao_mode 2, the occlusion itself

#pragma once

namespace native_x_renderer {
namespace gtao_glsl {

// XeGTAO.h's shared constants, the constant buffer (std140: the struct is 96 bytes, as XeGTAO's GTAOConstants;
// gtao.cpp's Constants mirrors the block) and the math every pass may use.
constexpr const char* kCommon = R"GLSL(
#define XE_GTAO_DEPTH_MIP_LEVELS 5
#define XE_GTAO_NUMTHREADS_X 8
#define XE_GTAO_NUMTHREADS_Y 8
#define XE_GTAO_DEFAULT_RADIUS_MULTIPLIER 1.457
#define XE_GTAO_DEFAULT_FALLOFF_RANGE 0.615
#define XE_GTAO_DEFAULT_SAMPLE_DISTRIBUTION_POWER 2.0
#define XE_GTAO_DEFAULT_THIN_OCCLUDER_COMPENSATION 0.0
#define XE_GTAO_OCCLUSION_TERM_SCALE 1.5
#define XE_GTAO_PI 3.1415926535897932384626433832795
#define XE_GTAO_PI_HALF 1.5707963267948966192313216916398
#define XE_HILBERT_LEVEL 6u
#define XE_HILBERT_WIDTH (1u << XE_HILBERT_LEVEL)

struct GTAOConstants {
  ivec2 ViewportSize;
  vec2 ViewportPixelSize;  // 1 / ViewportSize
  vec2 DepthUnpackConsts;
  vec2 CameraTanHalfFOV;
  vec2 NDCToViewMul;
  vec2 NDCToViewAdd;
  vec2 NDCToViewMul_x_PixelSize;
  float EffectRadius;  // world (view space) size of the occlusion
  float EffectFalloffRange;
  float RadiusMultiplier;
  float Padding0;
  float FinalValuePower;
  float DenoiseBlurBeta;
  float SampleDistributionPower;
  float ThinOccluderCompensation;
  float DepthMIPSamplingOffset;
  int NoiseIndex;  // frame % 64 with a temporal filter, else 0
};

layout(set = 0, binding = 0) uniform GtaoConstants {
  GTAOConstants g_GTAOConsts;
  ivec4 kk_Source;          // xy: the picture's origin in the targets (host pixels), zw: its size
  uvec4 kk_Working;         // xy: the working size covering the picture, zw: the padded size (multiples of 16)
  vec4 kk_Params;           // x: step (1 full, 2 half resolution), y, z: sky test (y * stored >= z), w: ao_mode
  vec4 kk_Fade;             // x: view depth at which the occlusion has faded out linearly, 0: no fade
  vec4 kk_Reprojection[4];  // rows: last frame's clip = R x this frame's clip (the camera vote, D118)
  vec4 kk_DepthRange;       // x: z_max - z_min, y: z_min (stored depth = x * clip z / clip w + y)
  vec4 kk_Temporal;         // x: history weight (0: none), y, z: last frame's depth unpack mul, add
  vec4 kk_Smooth;           // x: largest radius (working pixels), y: radius (view units), z, w: cos of the
                            // angles between normals smoothed fully and not at all
  vec4 kk_Smooth2;          // x, y: sin of the same for a tap's angle off this pixel's plane
};

// Stored depth at (or past) the far plane: the cleared sky. No occlusion there.
bool IsSky(float stored) {
  return kk_Params.y * stored >= kk_Params.z;
}

// R11G11B10_UNORM <-> vec3
vec3 XeGTAO_R11G11B10_UNORM_to_FLOAT3(uint packedInput) {
  vec3 unpackedOutput;
  unpackedOutput.x = float((packedInput) & 0x000007ffu) / 2047.0;
  unpackedOutput.y = float((packedInput >> 11) & 0x000007ffu) / 2047.0;
  unpackedOutput.z = float((packedInput >> 22) & 0x000003ffu) / 1023.0;
  return unpackedOutput;
}
uint XeGTAO_FLOAT3_to_R11G11B10_UNORM(vec3 unpackedInput) {
  return (uint(clamp(unpackedInput.x, 0.0, 1.0) * 2047.0 + 0.5)) |
         (uint(clamp(unpackedInput.y, 0.0, 1.0) * 2047.0 + 0.5) << 11) |
         (uint(clamp(unpackedInput.z, 0.0, 1.0) * 1023.0 + 0.5) << 22);
}

// Inputs are screen XY and view space depth, output is the view space position.
vec3 XeGTAO_ComputeViewspacePosition(vec2 screenPos, float viewspaceDepth, GTAOConstants consts) {
  vec3 ret;
  ret.xy = (consts.NDCToViewMul * screenPos.xy + consts.NDCToViewAdd) * viewspaceDepth;
  ret.z = viewspaceDepth;
  return ret;
}

float XeGTAO_ScreenSpaceToViewSpaceDepth(float screenDepth, GTAOConstants consts) {
  float depthLinearizeMul = consts.DepthUnpackConsts.x;
  float depthLinearizeAdd = consts.DepthUnpackConsts.y;
  return depthLinearizeMul / (depthLinearizeAdd - screenDepth);
}

vec4 XeGTAO_CalculateEdges(float centerZ, float leftZ, float rightZ, float topZ, float bottomZ) {
  vec4 edgesLRTB = vec4(leftZ, rightZ, topZ, bottomZ) - centerZ;
  float slopeLR = (edgesLRTB.y - edgesLRTB.x) * 0.5;
  float slopeTB = (edgesLRTB.w - edgesLRTB.z) * 0.5;
  vec4 edgesLRTBSlopeAdjusted = edgesLRTB + vec4(slopeLR, -slopeLR, slopeTB, -slopeTB);
  edgesLRTB = min(abs(edgesLRTB), abs(edgesLRTBSlopeAdjusted));
  return clamp(1.25 - edgesLRTB / (centerZ * 0.011), 0.0, 1.0);
}

// 2 bits per edge: 4 gradient values (0, 0.33, 0.66, 1) for smoother transitions.
float XeGTAO_PackEdges(vec4 edgesLRTB) {
  edgesLRTB = round(clamp(edgesLRTB, 0.0, 1.0) * 2.9);
  return dot(edgesLRTB, vec4(64.0 / 255.0, 16.0 / 255.0, 4.0 / 255.0, 1.0 / 255.0));
}

vec3 XeGTAO_CalculateNormal(vec4 edgesLRTB, vec3 pixCenterPos, vec3 pixLPos, vec3 pixRPos, vec3 pixTPos,
                            vec3 pixBPos) {
  vec4 acceptedNormals = clamp(vec4(edgesLRTB.x * edgesLRTB.z, edgesLRTB.z * edgesLRTB.y, edgesLRTB.y * edgesLRTB.w,
                                    edgesLRTB.w * edgesLRTB.x) + 0.01, 0.0, 1.0);
  pixLPos = normalize(pixLPos - pixCenterPos);
  pixRPos = normalize(pixRPos - pixCenterPos);
  pixTPos = normalize(pixTPos - pixCenterPos);
  pixBPos = normalize(pixBPos - pixCenterPos);
  vec3 pixelNormal = acceptedNormals.x * cross(pixLPos, pixTPos) + acceptedNormals.y * cross(pixTPos, pixRPos) +
                     acceptedNormals.z * cross(pixRPos, pixBPos) + acceptedNormals.w * cross(pixBPos, pixLPos);
  return normalize(pixelNormal);
}

// http://h14s.p5r.org/2012/09/0x5f3759df.html, [Drobot2014a] Low Level Optimizations for GCN
float XeGTAO_FastSqrt(float x) {
  return intBitsToFloat(0x1fbd1df5 + (floatBitsToInt(x) >> 1));
}
// Input -1 to 1, output 0 to PI (seblagarde.wordpress.com, inverse trigonometric functions on GCN).
float XeGTAO_FastACos(float inX) {
  const float PI = 3.141593;
  const float HALF_PI = 1.570796;
  float x = abs(inX);
  float res = -0.156583 * x + HALF_PI;
  res *= XeGTAO_FastSqrt(1.0 - x);
  return (inX >= 0.0) ? res : PI - res;
}

// From https://www.shadertoy.com/view/3tB3z3, with R2.
uint HilbertIndex(uint posX, uint posY) {
  uint index = 0u;
  for (uint curLevel = XE_HILBERT_WIDTH / 2u; curLevel > 0u; curLevel /= 2u) {
    uint regionX = (posX & curLevel) > 0u ? 1u : 0u;
    uint regionY = (posY & curLevel) > 0u ? 1u : 0u;
    index += curLevel * curLevel * ((3u * regionX) ^ regionY);
    if (regionY == 0u) {
      if (regionX == 1u) {
        posX = (XE_HILBERT_WIDTH - 1u) - posX;
        posY = (XE_HILBERT_WIDTH - 1u) - posY;
      }
      uint temp = posX;
      posX = posY;
      posY = temp;
    }
  }
  return index;
}

// The sample's noise: a Hilbert curve driving R2. It rotates over 64 patterns only when a temporal filter can
// smooth it (NoiseIndex), else it stays fixed.
vec2 SpatioTemporalNoise(uvec2 pixCoord, uint temporalIndex) {
  uint index = HilbertIndex(pixCoord.x, pixCoord.y);
  index += 288u * (temporalIndex % 64u);
  return fract(0.5 + float(index) * vec2(0.75487766624669276005, 0.5698402909980532659114));
}
)GLSL";

// The picture's stored depth into the working image. Past the picture the edge repeats: the occlusion there is
// the edge's surface continued.
constexpr const char* kPrepare = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 1) uniform sampler2D g_srcSceneDepth;
layout(set = 0, binding = 2, r32f) uniform writeonly image2D g_outRawDepth;
void main() {
  uvec2 id = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(id, kk_Working.zw))) return;
  uvec2 w = min(id, kk_Working.xy - 1u);
  ivec2 p = kk_Source.xy + min(ivec2(w * uint(kk_Params.x)), kk_Source.zw - 1);
  imageStore(g_outRawDepth, ivec2(id), vec4(texelFetch(g_srcSceneDepth, p, 0).x));
}
)GLSL";

// XeGTAO_PrefilterDepths16x16: each thread of an 8x8 group writes 2x2 pixels of mip 0, then the mips below by a
// weighted average through group shared memory.
constexpr const char* kPrefilter = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 1) uniform sampler2D g_srcRawDepth;
layout(set = 0, binding = 2, r32f) uniform writeonly image2D g_outWorkingDepthMIP0;
layout(set = 0, binding = 3, r32f) uniform writeonly image2D g_outWorkingDepthMIP1;
layout(set = 0, binding = 4, r32f) uniform writeonly image2D g_outWorkingDepthMIP2;
layout(set = 0, binding = 5, r32f) uniform writeonly image2D g_outWorkingDepthMIP3;
layout(set = 0, binding = 6, r32f) uniform writeonly image2D g_outWorkingDepthMIP4;

// Weighted average depth filter.
float XeGTAO_DepthMIPFilter(float depth0, float depth1, float depth2, float depth3, GTAOConstants consts) {
  float maxDepth = max(max(depth0, depth1), max(depth2, depth3));
  const float depthRangeScaleFactor = 0.75;  // found empirically (XeGTAO)
  float effectRadius = depthRangeScaleFactor * consts.EffectRadius * XE_GTAO_DEFAULT_RADIUS_MULTIPLIER;
  float falloffRange = XE_GTAO_DEFAULT_FALLOFF_RANGE * effectRadius;
  float falloffFrom = effectRadius * (1.0 - consts.EffectFalloffRange);
  float falloffMul = -1.0 / falloffRange;
  float falloffAdd = falloffFrom / falloffRange + 1.0;
  float weight0 = clamp((maxDepth - depth0) * falloffMul + falloffAdd, 0.0, 1.0);
  float weight1 = clamp((maxDepth - depth1) * falloffMul + falloffAdd, 0.0, 1.0);
  float weight2 = clamp((maxDepth - depth2) * falloffMul + falloffAdd, 0.0, 1.0);
  float weight3 = clamp((maxDepth - depth3) * falloffMul + falloffAdd, 0.0, 1.0);
  float weightSum = weight0 + weight1 + weight2 + weight3;
  return (weight0 * depth0 + weight1 * depth1 + weight2 * depth2 + weight3 * depth3) / weightSum;
}

// XeGTAO tests "#ifdef XE_GTAO_USE_HALF_FLOAT_PRECISION", which KK defines as 0, so its clamp is the half float
// one. Kept: view depths are clamped at 65504 world units.
float XeGTAO_ClampDepth(float depth) {
  return clamp(depth, 0.0, 65504.0);
}

shared float g_scratchDepths[8][8];

void main() {
  GTAOConstants consts = g_GTAOConsts;
  uvec2 baseCoord = gl_GlobalInvocationID.xy;
  uvec2 groupThreadID = gl_LocalInvocationID.xy;
  uvec2 pixCoord = baseCoord * 2u;
  vec4 depths4 = textureGatherOffset(g_srcRawDepth, vec2(pixCoord) * consts.ViewportPixelSize, ivec2(1, 1));
  float depth0 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(depths4.w, consts));
  float depth1 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(depths4.z, consts));
  float depth2 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(depths4.x, consts));
  float depth3 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(depths4.y, consts));
  imageStore(g_outWorkingDepthMIP0, ivec2(pixCoord + uvec2(0u, 0u)), vec4(depth0));
  imageStore(g_outWorkingDepthMIP0, ivec2(pixCoord + uvec2(1u, 0u)), vec4(depth1));
  imageStore(g_outWorkingDepthMIP0, ivec2(pixCoord + uvec2(0u, 1u)), vec4(depth2));
  imageStore(g_outWorkingDepthMIP0, ivec2(pixCoord + uvec2(1u, 1u)), vec4(depth3));

  // MIP 1
  float dm1 = XeGTAO_DepthMIPFilter(depth0, depth1, depth2, depth3, consts);
  imageStore(g_outWorkingDepthMIP1, ivec2(baseCoord), vec4(dm1));
  g_scratchDepths[groupThreadID.x][groupThreadID.y] = dm1;
  memoryBarrierShared();
  barrier();

  // MIP 2
  if (all(equal(groupThreadID % 2u, uvec2(0u)))) {
    float inTL = g_scratchDepths[groupThreadID.x + 0u][groupThreadID.y + 0u];
    float inTR = g_scratchDepths[groupThreadID.x + 1u][groupThreadID.y + 0u];
    float inBL = g_scratchDepths[groupThreadID.x + 0u][groupThreadID.y + 1u];
    float inBR = g_scratchDepths[groupThreadID.x + 1u][groupThreadID.y + 1u];
    float dm2 = XeGTAO_DepthMIPFilter(inTL, inTR, inBL, inBR, consts);
    imageStore(g_outWorkingDepthMIP2, ivec2(baseCoord / 2u), vec4(dm2));
    g_scratchDepths[groupThreadID.x][groupThreadID.y] = dm2;
  }
  memoryBarrierShared();
  barrier();

  // MIP 3
  if (all(equal(groupThreadID % 4u, uvec2(0u)))) {
    float inTL = g_scratchDepths[groupThreadID.x + 0u][groupThreadID.y + 0u];
    float inTR = g_scratchDepths[groupThreadID.x + 2u][groupThreadID.y + 0u];
    float inBL = g_scratchDepths[groupThreadID.x + 0u][groupThreadID.y + 2u];
    float inBR = g_scratchDepths[groupThreadID.x + 2u][groupThreadID.y + 2u];
    float dm3 = XeGTAO_DepthMIPFilter(inTL, inTR, inBL, inBR, consts);
    imageStore(g_outWorkingDepthMIP3, ivec2(baseCoord / 4u), vec4(dm3));
    g_scratchDepths[groupThreadID.x][groupThreadID.y] = dm3;
  }
  memoryBarrierShared();
  barrier();

  // MIP 4
  if (all(equal(groupThreadID % 8u, uvec2(0u)))) {
    float inTL = g_scratchDepths[groupThreadID.x + 0u][groupThreadID.y + 0u];
    float inTR = g_scratchDepths[groupThreadID.x + 4u][groupThreadID.y + 0u];
    float inBL = g_scratchDepths[groupThreadID.x + 0u][groupThreadID.y + 4u];
    float inBR = g_scratchDepths[groupThreadID.x + 4u][groupThreadID.y + 4u];
    float dm4 = XeGTAO_DepthMIPFilter(inTL, inTR, inBL, inBR, consts);
    imageStore(g_outWorkingDepthMIP4, ivec2(baseCoord / 8u), vec4(dm4));
  }
}
)GLSL";

// XeGTAO_ComputeViewspaceNormal: the normals from the stored depth, packed as R11G11B10 in an R32_UINT image.
constexpr const char* kNormals = R"GLSL(
layout(local_size_x = XE_GTAO_NUMTHREADS_X, local_size_y = XE_GTAO_NUMTHREADS_Y) in;
layout(set = 0, binding = 1) uniform sampler2D g_srcRawDepth;
layout(set = 0, binding = 2, r32ui) uniform writeonly uimage2D g_outNormalmap;

vec3 XeGTAO_ComputeViewspaceNormal(uvec2 pixCoord, GTAOConstants consts) {
  vec2 normalizedScreenPos = (vec2(pixCoord) + 0.5) * consts.ViewportPixelSize;
  vec4 valuesUL = textureGather(g_srcRawDepth, vec2(pixCoord) * consts.ViewportPixelSize);
  vec4 valuesBR = textureGatherOffset(g_srcRawDepth, vec2(pixCoord) * consts.ViewportPixelSize, ivec2(1, 1));
  // View space Z at the centre, then left, top, right, bottom.
  float viewspaceZ = XeGTAO_ScreenSpaceToViewSpaceDepth(valuesUL.y, consts);
  float pixLZ = XeGTAO_ScreenSpaceToViewSpaceDepth(valuesUL.x, consts);
  float pixTZ = XeGTAO_ScreenSpaceToViewSpaceDepth(valuesUL.z, consts);
  float pixRZ = XeGTAO_ScreenSpaceToViewSpaceDepth(valuesBR.z, consts);
  float pixBZ = XeGTAO_ScreenSpaceToViewSpaceDepth(valuesBR.x, consts);
  vec4 edgesLRTB = XeGTAO_CalculateEdges(viewspaceZ, pixLZ, pixRZ, pixTZ, pixBZ);
  vec3 CENTER = XeGTAO_ComputeViewspacePosition(normalizedScreenPos, viewspaceZ, consts);
  vec3 LEFT = XeGTAO_ComputeViewspacePosition(normalizedScreenPos + vec2(-1.0, 0.0) * consts.ViewportPixelSize, pixLZ, consts);
  vec3 RIGHT = XeGTAO_ComputeViewspacePosition(normalizedScreenPos + vec2(1.0, 0.0) * consts.ViewportPixelSize, pixRZ, consts);
  vec3 TOP = XeGTAO_ComputeViewspacePosition(normalizedScreenPos + vec2(0.0, -1.0) * consts.ViewportPixelSize, pixTZ, consts);
  vec3 BOTTOM = XeGTAO_ComputeViewspacePosition(normalizedScreenPos + vec2(0.0, 1.0) * consts.ViewportPixelSize, pixBZ, consts);
  return XeGTAO_CalculateNormal(edgesLRTB, CENTER, LEFT, RIGHT, TOP, BOTTOM);
}

void main() {
  uvec2 pixCoord = gl_GlobalInvocationID.xy;
  vec3 viewspaceNormal = XeGTAO_ComputeViewspaceNormal(pixCoord, g_GTAOConsts);
  imageStore(g_outNormalmap, ivec2(pixCoord),
             uvec4(XeGTAO_FLOAT3_to_R11G11B10_UNORM(clamp(viewspaceNormal * 0.5 + 0.5, 0.0, 1.0))));
}
)GLSL";

// KK's normal smoothing (its docs/ao.md, "Normal smoothing"): the normals rebuilt from depth are flat per triangle,
// so low-poly models get faceted occlusion with hard creases. A separable cross-bilateral blur of the normals over
// the surface: 4 taps each side, spaced over a radius in view units (kk_Smooth.y) capped in working pixels
// (kk_Smooth.x). A tap counts fully when its normal is within the smoothed angle and not at all past the largest
// one (creases of low-poly models are smoothed, the right angles of boxes and walls never), and only when its
// position lies near this pixel's plane, so silhouettes and objects in front of others keep their own normals.
// GTAO_SMOOTH_DIR is ivec2(1, 0) for the horizontal pass, ivec2(0, 1) for the vertical one.
constexpr const char* kSmooth = R"GLSL(
layout(local_size_x = XE_GTAO_NUMTHREADS_X, local_size_y = XE_GTAO_NUMTHREADS_Y) in;
layout(set = 0, binding = 1) uniform usampler2D g_srcSmoothNormals;
layout(set = 0, binding = 2) uniform sampler2D g_srcSmoothDepth;  // the working view-space depth (mip 0)
layout(set = 0, binding = 3, r32ui) uniform writeonly uimage2D g_outSmoothNormals;

vec3 UnpackNormal(uint packedNormal) {
  return normalize(XeGTAO_R11G11B10_UNORM_to_FLOAT3(packedNormal) * 2.0 - 1.0);
}

void main() {
  ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
  ivec2 dir = GTAO_SMOOTH_DIR;
  if (any(greaterThanEqual(pix, ivec2(kk_Working.zw)))) return;
  uint packedNormal = texelFetch(g_srcSmoothNormals, pix, 0).x;
  float z0 = texelFetch(g_srcSmoothDepth, pix, 0).x;
  // View units per working pixel at this depth; the radius in pixels shrinks with distance.
  float pixelSize = z0 * g_GTAOConsts.NDCToViewMul_x_PixelSize.x;
  float radius = min(kk_Smooth.x, kk_Smooth.y / max(pixelSize, 1e-9));
  if (radius < 1.0) {
    imageStore(g_outSmoothNormals, pix, uvec4(packedNormal));
    return;
  }
  vec3 n0 = UnpackNormal(packedNormal);
  vec3 p0 = XeGTAO_ComputeViewspacePosition((vec2(pix) + 0.5) * g_GTAOConsts.ViewportPixelSize, z0, g_GTAOConsts);
  vec3 viewDir = normalize(p0);
  float stride = radius * 0.25;
  ivec2 last = ivec2(kk_Working.xy) - 1;
  vec3 sum = n0;
  for (int i = 1; i <= 4; ++i) {
    float gauss = exp(-0.0625 * float(i * i));  // exp(-(i/4)^2)
    for (int side = -1; side <= 1; side += 2) {
      // The outermost pixels' rebuilt normals see the repeated edge: no taps there or beyond.
      ivec2 q = pix + dir * int(round(float(side * i) * stride));
      if (any(lessThan(q, ivec2(1))) || any(greaterThanEqual(q, last))) continue;
      float zq = texelFetch(g_srcSmoothDepth, q, 0).x;
      vec3 nq = UnpackNormal(texelFetch(g_srcSmoothNormals, q, 0).x);
      vec3 pq = XeGTAO_ComputeViewspacePosition((vec2(q) + 0.5) * g_GTAOConsts.ViewportPixelSize, zq, g_GTAOConsts);
      vec3 d = pq - p0;
      // Across a crease of angle a, a point past it lies at most sin(a) of its distance off this plane. A depth
      // step (a silhouette, an arm in front of the body) is mostly off-plane even with parallel normals: the
      // larger of the two pixels' views decides.
      float len = max(length(d), 1e-6 * z0);
      float offPlane = max(abs(dot(n0, d)), abs(dot(nq, d))) / len;
      // Seen at a grazing angle a surface behind is not far off that plane, but the step to it runs along the
      // view ray: within 15 degrees of it is a depth step.
      float alongView = abs(dot(d, viewDir)) / len;
      float wPlane = clamp((kk_Smooth2.y - offPlane) / (kk_Smooth2.y - kk_Smooth2.x), 0.0, 1.0) *
                     clamp((0.966 - alongView) / (0.966 - 0.906), 0.0, 1.0);
      float wNormal = clamp((dot(n0, nq) - kk_Smooth.w) / (kk_Smooth.z - kk_Smooth.w), 0.0, 1.0);
      sum += nq * (gauss * wPlane * wNormal);
    }
  }
  imageStore(g_outSmoothNormals, pix,
             uvec4(XeGTAO_FLOAT3_to_R11G11B10_UNORM(clamp(normalize(sum) * 0.5 + 0.5, 0.0, 1.0))));
}
)GLSL";

// XeGTAO_MainPass ("Algorithm 1" of the paper), with GTAO_SLICES slices of GTAO_STEPS steps: compile-time
// constants, so each quality is its own program (KK: Low and Medium do not inherit High's footprint).
constexpr const char* kMain = R"GLSL(
layout(local_size_x = XE_GTAO_NUMTHREADS_X, local_size_y = XE_GTAO_NUMTHREADS_Y) in;
layout(set = 0, binding = 1) uniform sampler2D g_srcWorkingDepth;  // view-space depth, all five mips
layout(set = 0, binding = 2) uniform usampler2D g_srcNormalmap;
layout(set = 0, binding = 3, r32ui) uniform writeonly uimage2D g_outWorkingAOTerm;
layout(set = 0, binding = 4, r32f) uniform writeonly image2D g_outWorkingEdges;

vec3 LoadNormal(ivec2 pos) {
  uint packedInput = texelFetch(g_srcNormalmap, pos, 0).x;
  vec3 unpackedOutput = XeGTAO_R11G11B10_UNORM_to_FLOAT3(packedInput);
  return normalize(unpackedOutput * 2.0 - 1.0);
}

void XeGTAO_OutputWorkingTerm(uvec2 pixCoord, float visibility, vec3 bentNormal) {
  visibility = clamp(visibility / XE_GTAO_OCCLUSION_TERM_SCALE, 0.0, 1.0);
  imageStore(g_outWorkingAOTerm, ivec2(pixCoord), uvec4(uint(visibility * 255.0 + 0.5)));
}

void XeGTAO_MainPass(uvec2 pixCoord, float sliceCount, float stepsPerSlice, vec2 localNoise, vec3 viewspaceNormal,
                     GTAOConstants consts) {
  vec2 normalizedScreenPos = (vec2(pixCoord) + 0.5) * consts.ViewportPixelSize;
  vec4 valuesUL = textureGather(g_srcWorkingDepth, vec2(pixCoord) * consts.ViewportPixelSize);
  vec4 valuesBR = textureGatherOffset(g_srcWorkingDepth, vec2(pixCoord) * consts.ViewportPixelSize, ivec2(1, 1));

  // View space Z at the centre, then left, top, right, bottom.
  float viewspaceZ = valuesUL.y;
  float pixLZ = valuesUL.x;
  float pixTZ = valuesUL.z;
  float pixRZ = valuesBR.z;
  float pixBZ = valuesBR.x;

  vec4 edgesLRTB = XeGTAO_CalculateEdges(viewspaceZ, pixLZ, pixRZ, pixTZ, pixBZ);
  imageStore(g_outWorkingEdges, ivec2(pixCoord), vec4(XeGTAO_PackEdges(edgesLRTB)));

  // The centre moved slightly towards the camera against depth imprecision (XE_GTAO_FP32_DEPTHS).
  viewspaceZ *= 0.99999;

  vec3 pixCenterPos = XeGTAO_ComputeViewspacePosition(normalizedScreenPos, viewspaceZ, consts);
  vec3 viewVec = normalize(-pixCenterPos);

  // XE_GTAO_USE_DEFAULT_CONSTANTS 1.
  float effectRadius = consts.EffectRadius * XE_GTAO_DEFAULT_RADIUS_MULTIPLIER;
  float sampleDistributionPower = XE_GTAO_DEFAULT_SAMPLE_DISTRIBUTION_POWER;
  float falloffRange = XE_GTAO_DEFAULT_FALLOFF_RANGE * effectRadius;
  float falloffFrom = effectRadius * (1.0 - consts.EffectFalloffRange);

  // Fadeout precompute optimisation.
  float falloffMul = -1.0 / falloffRange;
  float falloffAdd = falloffFrom / falloffRange + 1.0;

  float visibility = 0.0;
  vec3 bentNormal = viewspaceNormal;

  {
    float noiseSlice = localNoise.x;
    float noiseSample = localNoise.y;

    // If the offset is under approximately a pixel, push it out to the minimum distance.
    const float pixelTooCloseThreshold = 1.3;

    // Approximate view space pixel size at pixCoord.
    vec2 pixelDirRBViewspaceSizeAtCenterZ = vec2(viewspaceZ) * consts.NDCToViewMul_x_PixelSize;

    float screenspaceRadius = effectRadius / pixelDirRBViewspaceSizeAtCenterZ.x;

    // Fade out for small screen radii.
    visibility += clamp((10.0 - screenspaceRadius) / 100.0, 0.0, 1.0) * 0.5;

    // The minimum distance to start sampling from, to avoid sampling the centre pixel.
    float minS = pixelTooCloseThreshold / screenspaceRadius;

    for (float slice = 0.0; slice < sliceCount; slice++) {
      float sliceK = (slice + noiseSlice) / sliceCount;
      // Lines 5, 6 of the paper.
      float phi = sliceK * XE_GTAO_PI;
      float cosPhi = cos(phi);
      float sinPhi = sin(phi);
      vec2 omega = vec2(cosPhi, -sinPhi);

      // In screen units (pixels).
      omega *= screenspaceRadius;

      // Line 8.
      vec3 directionVec = vec3(cosPhi, sinPhi, 0.0);

      // Line 9.
      vec3 orthoDirectionVec = directionVec - (dot(directionVec, viewVec) * viewVec);

      // Line 10: axisVec is orthogonal to directionVec and viewVec, used to define projectedNormal.
      vec3 axisVec = normalize(cross(orthoDirectionVec, viewVec));

      // Line 11.
      vec3 projectedNormalVec = viewspaceNormal - axisVec * dot(viewspaceNormal, axisVec);

      // Line 13.
      float signNorm = sign(dot(orthoDirectionVec, projectedNormalVec));

      // Line 14.
      float projectedNormalVecLength = length(projectedNormalVec);
      float cosNorm = clamp(dot(projectedNormalVec, viewVec) / projectedNormalVecLength, 0.0, 1.0);

      // Line 15.
      float n = signNorm * XeGTAO_FastACos(cosNorm);

      // A lower weight target; not -1 as in the paper because it is under the horizon.
      float lowHorizonCos0 = cos(n + XE_GTAO_PI_HALF);
      float lowHorizonCos1 = cos(n - XE_GTAO_PI_HALF);

      // Lines 17, 18, the side loop unrolled.
      float horizonCos0 = lowHorizonCos0;
      float horizonCos1 = lowHorizonCos1;

      for (float stepIndex = 0.0; stepIndex < stepsPerSlice; stepIndex++) {
        // R1 sequence (extremelearning.com.au, unreasonable effectiveness of quasirandom sequences).
        float stepBaseNoise = float(slice + stepIndex * stepsPerSlice) * 0.6180339887498948482;
        float stepNoise = fract(noiseSample + stepBaseNoise);

        // Approximately line 20, with noise.
        float s = (stepIndex + stepNoise) / stepsPerSlice;

        // Additional distribution modifier.
        s = pow(s, sampleDistributionPower);

        // Avoid sampling the centre pixel.
        s += minS;

        // Approximately lines 21, 22.
        vec2 sampleOffset = s * omega;

        float sampleOffsetLength = length(sampleOffset);

        // Point sampling: a linear filter would interpolate between neighbouring depths of the same mip.
        float mipLevel = clamp(log2(sampleOffsetLength) - consts.DepthMIPSamplingOffset, 0.0,
                               float(XE_GTAO_DEPTH_MIP_LEVELS));

        // Snap to the pixel centre.
        sampleOffset = round(sampleOffset) * consts.ViewportPixelSize;

        vec2 sampleScreenPos0 = normalizedScreenPos + sampleOffset;
        float SZ0 = textureLod(g_srcWorkingDepth, sampleScreenPos0, mipLevel).x;
        vec3 samplePos0 = XeGTAO_ComputeViewspacePosition(sampleScreenPos0, SZ0, consts);

        vec2 sampleScreenPos1 = normalizedScreenPos - sampleOffset;
        float SZ1 = textureLod(g_srcWorkingDepth, sampleScreenPos1, mipLevel).x;
        vec3 samplePos1 = XeGTAO_ComputeViewspacePosition(sampleScreenPos1, SZ1, consts);

        vec3 sampleDelta0 = samplePos0 - pixCenterPos;
        vec3 sampleDelta1 = samplePos1 - pixCenterPos;
        float sampleDist0 = length(sampleDelta0);
        float sampleDist1 = length(sampleDelta1);

        // Approximately lines 23, 24.
        vec3 sampleHorizonVec0 = sampleDelta0 / sampleDist0;
        vec3 sampleHorizonVec1 = sampleDelta1 / sampleDist1;

        // Samples out of the radius are discarded, with the falloff range for smooth transitions (default
        // constants without the thin object heuristic).
        float weight0 = clamp(sampleDist0 * falloffMul + falloffAdd, 0.0, 1.0);
        float weight1 = clamp(sampleDist1 * falloffMul + falloffAdd, 0.0, 1.0);

        // Sample horizon cos.
        float shc0 = dot(sampleHorizonVec0, viewVec);
        float shc1 = dot(sampleHorizonVec1, viewVec);

        // Discard unwanted samples.
        shc0 = mix(lowHorizonCos0, shc0, weight0);
        shc1 = mix(lowHorizonCos1, shc1, weight1);

        // The thickness heuristic disabled (XeGTAO's default).
        horizonCos0 = max(horizonCos0, shc0);
        horizonCos1 = max(horizonCos1, shc1);
      }

      // XeGTAO's fudge against slight overdarkening on high slopes.
      projectedNormalVecLength = mix(projectedNormalVecLength, 1.0, 0.05);

      // Line 27, unrolled.
      float h0 = -XeGTAO_FastACos(horizonCos1);
      float h1 = XeGTAO_FastACos(horizonCos0);
      float iarc0 = (cosNorm + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) / 4.0;
      float iarc1 = (cosNorm + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n)) / 4.0;
      float localVisibility = projectedNormalVecLength * (iarc0 + iarc1);
      visibility += localVisibility;
    }
    visibility /= sliceCount;
    visibility = pow(visibility, consts.FinalValuePower);
    // Total occlusion is not allowed (the pixel is visible).
    visibility = max(0.03, visibility);
  }

  XeGTAO_OutputWorkingTerm(pixCoord, visibility, bentNormal);
}

void main() {
  uvec2 pixCoord = gl_GlobalInvocationID.xy;
  vec2 noise = SpatioTemporalNoise(pixCoord, uint(g_GTAOConsts.NoiseIndex));
  vec3 normal = LoadNormal(ivec2(pixCoord));
  XeGTAO_MainPass(pixCoord, float(GTAO_SLICES), float(GTAO_STEPS), noise, normal, g_GTAOConsts);
}
)GLSL";

// XeGTAO_Denoise: each thread filters two horizontally adjacent pixels (dispatched at half width). The last pass
// (GTAO_DENOISE_FINAL 1) uses the full blur beta and scales the term back by XE_GTAO_OCCLUSION_TERM_SCALE.
constexpr const char* kDenoise = R"GLSL(
layout(local_size_x = XE_GTAO_NUMTHREADS_X, local_size_y = XE_GTAO_NUMTHREADS_Y) in;
layout(set = 0, binding = 1) uniform usampler2D g_srcWorkingAOTerm;
layout(set = 0, binding = 2) uniform sampler2D g_srcWorkingEdges;
layout(set = 0, binding = 3, r32ui) uniform writeonly uimage2D g_outFinalAOTerm;

vec4 XeGTAO_UnpackEdges(float _packedVal) {
  uint packedVal = uint(_packedVal * 255.5);
  vec4 edgesLRTB;
  edgesLRTB.x = float((packedVal >> 6) & 0x03u) / 3.0;
  edgesLRTB.y = float((packedVal >> 4) & 0x03u) / 3.0;
  edgesLRTB.z = float((packedVal >> 2) & 0x03u) / 3.0;
  edgesLRTB.w = float((packedVal >> 0) & 0x03u) / 3.0;
  return clamp(edgesLRTB, 0.0, 1.0);
}

void XeGTAO_AddSample(float ssaoValue, float edgeValue, inout float sum, inout float sumWeight) {
  float weight = edgeValue;
  sum += (weight * ssaoValue);
  sumWeight += weight;
}

void XeGTAO_Output(uvec2 pixCoord, float outputValue, bool finalApply) {
  outputValue *= finalApply ? XE_GTAO_OCCLUSION_TERM_SCALE : 1.0;
  imageStore(g_outFinalAOTerm, ivec2(pixCoord), uvec4(uint(outputValue * 255.0 + 0.5)));
}

void XeGTAO_DecodeGatherPartial(uvec4 packedValue, out float outDecoded[4]) {
  for (int i = 0; i < 4; i++) outDecoded[i] = float(packedValue[i]) / 255.0;
}

void XeGTAO_Denoise(uvec2 pixCoordBase, GTAOConstants consts) {
  const bool finalApply = GTAO_DENOISE_FINAL != 0;
  float blurAmount = finalApply ? consts.DenoiseBlurBeta : consts.DenoiseBlurBeta / 5.0;
  const float diagWeight = 0.85 * 0.5;

  float aoTerm[2];  // pixel pixCoordBase and pixel pixCoordBase + (1, 0)
  vec4 edgesC_LRTB[2];
  float weightTL[2];
  float weightTR[2];
  float weightBL[2];
  float weightBR[2];

  // Gather the edge and visibility quads used later.
  vec2 gatherCenter = vec2(pixCoordBase) * consts.ViewportPixelSize;
  vec4 edgesQ0 = textureGatherOffset(g_srcWorkingEdges, gatherCenter, ivec2(0, 0));
  vec4 edgesQ1 = textureGatherOffset(g_srcWorkingEdges, gatherCenter, ivec2(2, 0));
  vec4 edgesQ2 = textureGatherOffset(g_srcWorkingEdges, gatherCenter, ivec2(1, 2));

  float visQ0[4];
  XeGTAO_DecodeGatherPartial(textureGatherOffset(g_srcWorkingAOTerm, gatherCenter, ivec2(0, 0)), visQ0);
  float visQ1[4];
  XeGTAO_DecodeGatherPartial(textureGatherOffset(g_srcWorkingAOTerm, gatherCenter, ivec2(2, 0)), visQ1);
  float visQ2[4];
  XeGTAO_DecodeGatherPartial(textureGatherOffset(g_srcWorkingAOTerm, gatherCenter, ivec2(0, 2)), visQ2);
  float visQ3[4];
  XeGTAO_DecodeGatherPartial(textureGatherOffset(g_srcWorkingAOTerm, gatherCenter, ivec2(2, 2)), visQ3);

  for (int side = 0; side < 2; side++) {
    uvec2 pixCoord = uvec2(pixCoordBase.x + uint(side), pixCoordBase.y);

    vec4 edgesL_LRTB = XeGTAO_UnpackEdges((side == 0) ? (edgesQ0.x) : (edgesQ0.y));
    vec4 edgesT_LRTB = XeGTAO_UnpackEdges((side == 0) ? (edgesQ0.z) : (edgesQ1.w));
    vec4 edgesR_LRTB = XeGTAO_UnpackEdges((side == 0) ? (edgesQ1.x) : (edgesQ1.y));
    vec4 edgesB_LRTB = XeGTAO_UnpackEdges((side == 0) ? (edgesQ2.w) : (edgesQ2.z));

    edgesC_LRTB[side] = XeGTAO_UnpackEdges((side == 0) ? (edgesQ0.y) : (edgesQ1.x));

    // Edges are not perfectly symmetrical: this enforces it, a slightly sharper blur.
    edgesC_LRTB[side] *= vec4(edgesL_LRTB.y, edgesR_LRTB.x, edgesT_LRTB.w, edgesB_LRTB.z);

    // A small amount of AO leaks from neighbours with 3 or 4 edges: less spatial and temporal aliasing.
    const float leak_threshold = 2.5;
    const float leak_strength = 0.5;
    float edginess = (clamp(4.0 - leak_threshold - dot(edgesC_LRTB[side], vec4(1.0)), 0.0, 1.0) /
                      (4.0 - leak_threshold)) * leak_strength;
    edgesC_LRTB[side] = clamp(edgesC_LRTB[side] + edginess, 0.0, 1.0);

    // For diagonals; used by the first and second pass.
    weightTL[side] = diagWeight * (edgesC_LRTB[side].x * edgesL_LRTB.z + edgesC_LRTB[side].z * edgesT_LRTB.x);
    weightTR[side] = diagWeight * (edgesC_LRTB[side].z * edgesT_LRTB.y + edgesC_LRTB[side].y * edgesR_LRTB.z);
    weightBL[side] = diagWeight * (edgesC_LRTB[side].w * edgesB_LRTB.x + edgesC_LRTB[side].x * edgesL_LRTB.w);
    weightBR[side] = diagWeight * (edgesC_LRTB[side].y * edgesR_LRTB.w + edgesC_LRTB[side].w * edgesB_LRTB.y);

    float ssaoValue = (side == 0) ? (visQ0[1]) : (visQ1[0]);
    float ssaoValueL = (side == 0) ? (visQ0[0]) : (visQ0[1]);
    float ssaoValueT = (side == 0) ? (visQ0[2]) : (visQ1[3]);
    float ssaoValueR = (side == 0) ? (visQ1[0]) : (visQ1[1]);
    float ssaoValueB = (side == 0) ? (visQ2[2]) : (visQ3[3]);
    float ssaoValueTL = (side == 0) ? (visQ0[3]) : (visQ0[2]);
    float ssaoValueBR = (side == 0) ? (visQ3[3]) : (visQ3[2]);
    float ssaoValueTR = (side == 0) ? (visQ1[3]) : (visQ1[2]);
    float ssaoValueBL = (side == 0) ? (visQ2[3]) : (visQ2[2]);

    float sumWeight = blurAmount;
    float sum = ssaoValue * sumWeight;

    XeGTAO_AddSample(ssaoValueL, edgesC_LRTB[side].x, sum, sumWeight);
    XeGTAO_AddSample(ssaoValueR, edgesC_LRTB[side].y, sum, sumWeight);
    XeGTAO_AddSample(ssaoValueT, edgesC_LRTB[side].z, sum, sumWeight);
    XeGTAO_AddSample(ssaoValueB, edgesC_LRTB[side].w, sum, sumWeight);

    XeGTAO_AddSample(ssaoValueTL, weightTL[side], sum, sumWeight);
    XeGTAO_AddSample(ssaoValueTR, weightTR[side], sum, sumWeight);
    XeGTAO_AddSample(ssaoValueBL, weightBL[side], sum, sumWeight);
    XeGTAO_AddSample(ssaoValueBR, weightBR[side], sum, sumWeight);

    aoTerm[side] = sum / sumWeight;

    XeGTAO_Output(pixCoord, aoTerm[side], finalApply);
  }
}

void main() {
  XeGTAO_Denoise(gl_GlobalInvocationID.xy * uvec2(2u, 1u), g_GTAOConsts);
}
)GLSL";

// The occlusion per host pixel of the picture (R32F, 1 = none). At half resolution working pixel i is host pixel
// 2i, so host pixel p sits at p / 2 in working pixels: the four nearest, bilinear and weighted by how close their
// depth is to this pixel's. The sky gets exactly 1; kk_Fade fades it out with distance.
constexpr const char* kOutput = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 1) uniform usampler2D g_srcFinalAOTerm;
layout(set = 0, binding = 2) uniform sampler2D g_srcOutputDepth;  // the scene's stored depth
layout(set = 0, binding = 3) uniform sampler2D g_srcViewDepth;    // the working view-space depth (mip 0)
layout(set = 0, binding = 4, r32f) uniform writeonly image2D g_outOcclusion;

float ViewDepth(float stored) {
  return XeGTAO_ScreenSpaceToViewSpaceDepth(stored, g_GTAOConsts);
}

void main() {
  ivec2 id = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(id, kk_Source.zw))) return;
  float stored = texelFetch(g_srcOutputDepth, kk_Source.xy + id, 0).x;
  float ao = 1.0;
  if (!IsSky(stored)) {
    if (kk_Params.x < 1.5) {
      ao = float(texelFetch(g_srcFinalAOTerm, id, 0).x) / 255.0;
    } else {
      float z = ViewDepth(stored);
      vec2 f = vec2(id) * 0.5;
      ivec2 base = ivec2(floor(f));
      vec2 t = f - vec2(base);
      ivec2 last = ivec2(kk_Working.xy) - 1;
      float total = 0.0, weight = 0.0;
      for (int j = 0; j < 4; ++j) {
        ivec2 o = ivec2(j & 1, j >> 1);
        ivec2 q = clamp(base + o, ivec2(0), last);
        float bilinear = (o.x != 0 ? t.x : 1.0 - t.x) * (o.y != 0 ? t.y : 1.0 - t.y);
        float dz = abs(texelFetch(g_srcViewDepth, q, 0).x - z) / max(z, 1e-6);
        float w = (bilinear + 1e-3) / (1e-3 + dz * 50.0);
        total += float(texelFetch(g_srcFinalAOTerm, q, 0).x) / 255.0 * w;
        weight += w;
      }
      ao = weight > 0.0 ? total / weight : 1.0;
    }
    // KK's distance fade (native_ao_fade).
    if (kk_Fade.x > 0.0) ao = mix(1.0, ao, clamp(1.0 - ViewDepth(stored) / kk_Fade.x, 0.0, 1.0));
  }
  imageStore(g_outOcclusion, id, vec4(clamp(ao, 0.0, 1.0)));
}
)GLSL";

// KK's local temporal denoise without a temporal upscaler: this frame's depth into the last frame's picture by the
// camera's reprojection (the camera vote, D118). It rejects history whose depth differs by more than 1 % (at least
// 0.002 world units), samples outside the last picture and jumps over a quarter of the picture; accepted history is
// clamped to this frame's 3x3 neighbourhood, then blended at kk_Temporal.x. Camera motion only: the depth test and
// the clamp bound trails on objects that move on their own.
constexpr const char* kTemporal = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 1) uniform sampler2D g_srcSpatialAO;
layout(set = 0, binding = 2) uniform sampler2D g_srcTemporalSceneDepth;
layout(set = 0, binding = 3) uniform sampler2D g_srcHistoryAO;
layout(set = 0, binding = 4) uniform sampler2D g_srcHistoryDepth;
layout(set = 0, binding = 5, r32f) uniform writeonly image2D g_outTemporalAO;
layout(set = 0, binding = 6, r32f) uniform writeonly image2D g_outTemporalDepth;

void main() {
  ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(pixel, kk_Source.zw))) return;
  float stored = texelFetch(g_srcTemporalSceneDepth, kk_Source.xy + pixel, 0).x;
  float current = texelFetch(g_srcSpatialAO, pixel, 0).x;
  imageStore(g_outTemporalDepth, pixel, vec4(stored));
  float result = current;
  if (kk_Temporal.x > 0.0 && !IsSky(stored)) {
    // The pixel's centre in clip space (D3D's: y up), then in the last frame's picture.
    vec2 size = vec2(kk_Source.zw);
    vec2 uv = (vec2(pixel) + 0.5) / size;
    vec4 clip = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, (stored - kk_DepthRange.y) / kk_DepthRange.x, 1.0);
    vec4 prev = vec4(dot(kk_Reprojection[0], clip), dot(kk_Reprojection[1], clip), dot(kk_Reprojection[2], clip),
                     dot(kk_Reprojection[3], clip));
    vec2 prevNdc = prev.xy / max(prev.w, 1e-6);
    vec2 prevPixel = vec2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) * size - 0.5;
    // Large camera jumps are cuts. There is no history outside the last picture.
    if (prev.w > 1e-6 && all(greaterThanEqual(prevPixel, vec2(0.0))) && all(lessThanEqual(prevPixel, size - 1.0)) &&
        all(lessThan(abs(prevPixel - vec2(pixel)), size * 0.25))) {
      float expected = prev.z / prev.w * kk_DepthRange.x + kk_DepthRange.y;
      float expectedZ = kk_Temporal.y / (kk_Temporal.z - expected);
      float sum = 0.0, weight = 0.0;
      ivec2 base = ivec2(floor(prevPixel));
      vec2 f = fract(prevPixel);
      for (int j = 0; j < 4; ++j) {
        ivec2 o = ivec2(j & 1, j >> 1);
        ivec2 q = min(base + o, kk_Source.zw - 1);
        float depth = texelFetch(g_srcHistoryDepth, q, 0).x;
        float z = kk_Temporal.y / (kk_Temporal.z - depth);
        float w = (o.x != 0 ? f.x : 1.0 - f.x) * (o.y != 0 ? f.y : 1.0 - f.y);
        if (expectedZ > 0.0 && z > 0.0 && abs(z - expectedZ) < max(0.002, expectedZ * 0.01)) {
          sum += texelFetch(g_srcHistoryAO, q, 0).x * w;
          weight += w;
        }
      }
      if (weight > 0.5) {
        float lo = current, hi = current;
        for (int y = -1; y <= 1; ++y) {
          for (int x = -1; x <= 1; ++x) {
            ivec2 q = clamp(pixel + ivec2(x, y), ivec2(0), kk_Source.zw - 1);
            float value = texelFetch(g_srcSpatialAO, q, 0).x;
            lo = min(lo, value);
            hi = max(hi, value);
          }
        }
        result = mix(current, clamp(sum / weight, lo, hi), kk_Temporal.x);
      }
    }
  }
  imageStore(g_outTemporalAO, pixel, vec4(clamp(result, 0.0, 1.0)));
}
)GLSL";

// One triangle covering the viewport (the picture); the pixel shader works from gl_FragCoord.
constexpr const char* kApplyVS = R"GLSL(#version 450
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// The occlusion at this pixel of the target (1 outside the picture). Blended as destination times source
// (ao_mode 1), or written as it is (ao_mode 2); the pipeline leaves alpha alone either way.
constexpr const char* kApplyPS = R"GLSL(
layout(set = 0, binding = 1) uniform sampler2D g_srcOcclusion;
layout(location = 0) out vec4 out_color;
void main() {
  ivec2 q = ivec2(gl_FragCoord.xy) - kk_Source.xy;
  float ao = 1.0;
  if (all(greaterThanEqual(q, ivec2(0))) && all(lessThan(q, kk_Source.zw))) ao = texelFetch(g_srcOcclusion, q, 0).x;
  out_color = vec4(ao, ao, ao, 1.0);
}
)GLSL";

}  // namespace gtao_glsl
}  // namespace native_x_renderer
