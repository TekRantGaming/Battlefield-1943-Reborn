// NativeXRenderer - the GLSL 450 compute and pixel shaders of the screen space global illumination (ssgi.h),
// compiled at startup by glslang (glsl.h, D16).
//
// A port of King Kong Recompiled's backend/shaders/ssgi.hlsl and of the three depth passes of its
// backend/shaders/gtao.hlsl that the GI runs on its own Gtao instance (Copyright (c) 2026 TekRantGaming and
// contributors, MIT: thirdparty/king-kong-recompiled/LICENSE), from HLSL to GLSL. The gather follows Therrien,
// Levesque and Gilet, "Screen Space Indirect Lighting with Visibility Bitmask" (2023), a published method; no code
// of theirs is used. The depth passes and the helpers below are XeGTAO's:
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
// KK's math and constants are kept as they are. What changes:
//   - The language: SampleLevel is textureLod, Load is texelFetch, RWTexture2D stores are imageStore, countbits is
//     bitCount, frac is fract, lerp is mix, saturate is clamp to 0 and 1.
//   - The camera mapping is NativeXRenderer's (as in gtao_shaders.h): the picture is a rectangle of host pixels,
//     clip space is D3D's and the viewport's depth range is z_min to z_max, so the sky test has a sign (reversed
//     depth, Battlefield 1943) and the temporal pass works from the picture's clip space, not the guest viewport.
//   - KK's cs_radiance and cs_radiance_down are one pass here (kRadiance): mip 0 and its four smaller mips through
//     group shared memory, as XeGTAO's depth prefilter does it. The values are the same (2x2 means of linear light),
//     and no pass binds one texture both to read and to write.
//   - The composite does not multiply in the AO (KK's kk_Extra.x is always 0 there).
//
// Each pass is a separate program: "#version 450", its defines, kCommon, then its own part. Each part is a raw
// string under MSVC's 16 KB limit for one literal.
//
//   kPrepare      the picture's stored depth into the working image (XeGTAO's, as gtao_shaders.h)
//   kPrefilter    XeGTAO_PrefilterDepths16x16: view space depth and its four smaller mips
//   kNormals      XeGTAO_ComputeViewspaceNormal: normals rebuilt from the depth (unsmoothed, as KK's GI)
//   kRadiance     the lit scene colour per working pixel (sky texels left out, then linear light and KK's sunlit
//                 source estimate), with its four smaller mips
//   kGather       the visibility bitmask gather: rgb the bounce light, a the share of the hemisphere nothing hides
//   kTemporal     this frame's light blended into last frame's, reprojected by the camera vote
//   kDenoise      an edge aware a-trous blur (3x3 for two slices, 5x5 for more), steps 1, 2, 4
//   kCompositeVS/PS   one triangle over the picture: the bounce light added to the scene colour in linear light

#pragma once

namespace native_x_renderer {
namespace ssgi_glsl {

// XeGTAO's shared constants and helpers (as gtao_shaders.h), then the GI's own constants and helpers. The constant
// buffer is std140; ssgi.cpp's Constants mirrors it.
constexpr const char* kCommon = R"GLSL(
#define XE_GTAO_DEPTH_MIP_LEVELS 5
#define XE_GTAO_NUMTHREADS_X 8
#define XE_GTAO_NUMTHREADS_Y 8
#define XE_GTAO_DEFAULT_RADIUS_MULTIPLIER 1.457
#define XE_GTAO_DEFAULT_FALLOFF_RANGE 0.615
#define XE_HILBERT_LEVEL 6u
#define XE_HILBERT_WIDTH (1u << XE_HILBERT_LEVEL)
#define SSGI_PI 3.14159265
#define SSGI_HALF_PI 1.5707963

struct GTAOConstants {
  ivec2 ViewportSize;
  vec2 ViewportPixelSize;  // 1 / ViewportSize
  vec2 DepthUnpackConsts;
  vec2 CameraTanHalfFOV;
  vec2 NDCToViewMul;
  vec2 NDCToViewAdd;
  vec2 NDCToViewMul_x_PixelSize;
  float EffectRadius;  // the depth prefilter's weights only (KK's GI Gtao: 6 near planes)
  float EffectFalloffRange;
  float RadiusMultiplier;
  float Padding0;
  float FinalValuePower;
  float DenoiseBlurBeta;
  float SampleDistributionPower;
  float ThinOccluderCompensation;
  float DepthMIPSamplingOffset;
  int NoiseIndex;
};

layout(set = 0, binding = 0) uniform SsgiConstants {
  GTAOConstants g_GTAOConsts;  // XeGTAO's, as the depth was prepared (ViewportSize is the padded size)
  ivec4 kk_Source;             // xy: the picture's origin in the targets (host pixels), zw: its size
  uvec4 kk_Working;            // xy: working size covering the picture, zw: padded size (multiples of 16)
  vec4 kk_Params;              // x: step (1 or 2), y: unused, z: radius, w: thickness (world units)
  vec4 kk_Gather;              // x: slices, y: steps per side, z: largest radius on the screen (working pixels),
                               // w: intensity (with KK's x3 calibration)
  vec4 kk_Ambient;             // rgb: the frame's flat ambient light (picture values), w: the largest albedo (linear)
  vec4 kk_Mode;                // x: occlusion of the ambient (0 to 1), y: debug view, z: radiance mips, w: denoise step
  vec4 kk_Extra;               // x: unused, y: noise index, z: 1 with a history, w: the new light's weight
  vec4 kk_Reprojection[4];     // rows: last frame's clip = R x this frame's clip (the camera vote, D118)
  vec4 kk_DepthRange;          // x: z_max - z_min, y: z_min, z, w: last frame's depth unpack mul, add
  vec4 kk_Shape;               // x: multi-bounce strength, y: largest darkening share, z: gamma, w: sunlit restoration
  vec4 kk_Distance;            // x: max camera distance (0 unlimited), y: fade start
  vec4 kk_Sky;                 // x, y: sky test (x * stored >= y)
};

// Stored depth at (or past) the far plane: the cleared sky.
bool IsSky(float stored) {
  return kk_Sky.x * stored >= kk_Sky.y;
}

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

float XeGTAO_FastSqrt(float x) {
  return intBitsToFloat(0x1fbd1df5 + (floatBitsToInt(x) >> 1));
}
float XeGTAO_FastACos(float inX) {
  const float PI = 3.141593;
  const float HALF_PI = 1.570796;
  float x = abs(inX);
  float res = -0.156583 * x + HALF_PI;
  res *= XeGTAO_FastSqrt(1.0 - x);
  return (inX >= 0.0) ? res : PI - res;
}

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

// XeGTAO's Hilbert curve driving R2 with KK's different start (17), so the GI's pattern is not the AO's. The same
// every frame without a temporal filter (nothing crawls); with one, another per frame (kk_Extra.y).
vec2 Noise(uvec2 pixCoord) {
  uint index = HilbertIndex(pixCoord.x, pixCoord.y) + 17u + 288u * (uint(kk_Extra.y) % 64u);
  return fract(0.5 + float(index) * vec2(0.75487766624669276005, 0.5698402909980532659114));
}

float Luma(vec3 c) {
  return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// The game's targets hold gamma encoded values (about 2.2 on Battlefield 1943's 8-bit scene colour): the light is
// gathered and added in linear light.
vec3 ToLinear(vec3 c) {
  return pow(max(c, vec3(0.0)), vec3(kk_Shape.z));
}
vec3 ToGamma(vec3 c) {
  return pow(max(c, vec3(0.0)), vec3(1.0 / kk_Shape.z));
}

float CameraDistance(vec2 workingPixel, float z) {
  vec2 uv = (workingPixel + 0.5) * g_GTAOConsts.ViewportPixelSize;
  return length(XeGTAO_ComputeViewspacePosition(uv, z, g_GTAOConsts));
}
bool BeyondDistance(vec2 pixel, float z) {
  return kk_Distance.x > 0.0 && CameraDistance(pixel, z) >= kk_Distance.x;
}
)GLSL";

// The picture's stored depth into the working image (gtao_shaders.h's kPrepare). Past the picture the edge repeats.
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

// XeGTAO_PrefilterDepths16x16 (gtao_shaders.h's kPrefilter).
constexpr const char* kPrefilter = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 1) uniform sampler2D g_srcRawDepth;
layout(set = 0, binding = 2, r32f) uniform writeonly image2D g_outWorkingDepthMIP0;
layout(set = 0, binding = 3, r32f) uniform writeonly image2D g_outWorkingDepthMIP1;
layout(set = 0, binding = 4, r32f) uniform writeonly image2D g_outWorkingDepthMIP2;
layout(set = 0, binding = 5, r32f) uniform writeonly image2D g_outWorkingDepthMIP3;
layout(set = 0, binding = 6, r32f) uniform writeonly image2D g_outWorkingDepthMIP4;

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

// As KK and gtao_shaders.h: view depths clamped at 65504 world units.
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

  float dm1 = XeGTAO_DepthMIPFilter(depth0, depth1, depth2, depth3, consts);
  imageStore(g_outWorkingDepthMIP1, ivec2(baseCoord), vec4(dm1));
  g_scratchDepths[groupThreadID.x][groupThreadID.y] = dm1;
  memoryBarrierShared();
  barrier();

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

// XeGTAO_ComputeViewspaceNormal (gtao_shaders.h's kNormals): packed as R11G11B10 in an R32_UINT image.
constexpr const char* kNormals = R"GLSL(
layout(local_size_x = XE_GTAO_NUMTHREADS_X, local_size_y = XE_GTAO_NUMTHREADS_Y) in;
layout(set = 0, binding = 1) uniform sampler2D g_srcRawDepth;
layout(set = 0, binding = 2, r32ui) uniform writeonly uimage2D g_outNormalmap;

vec3 XeGTAO_ComputeViewspaceNormal(uvec2 pixCoord, GTAOConstants consts) {
  vec2 normalizedScreenPos = (vec2(pixCoord) + 0.5) * consts.ViewportPixelSize;
  vec4 valuesUL = textureGather(g_srcRawDepth, vec2(pixCoord) * consts.ViewportPixelSize);
  vec4 valuesBR = textureGatherOffset(g_srcRawDepth, vec2(pixCoord) * consts.ViewportPixelSize, ivec2(1, 1));
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

// KK's cs_radiance and cs_radiance_down in one pass. Each thread of an 8x8 group makes 2x2 working pixels of mip 0
// (the encoded mean of each one's host texels, sky texels black, then linear light and the sunlit estimate), then
// the four smaller mips as 2x2 means through group shared memory. The padded size is a multiple of 16, so every mip
// is exactly half the one above, as KK's separate passes make them.
constexpr const char* kRadiance = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 1) uniform sampler2D g_srcSceneColor;   // a copy of the scene colour (host pixels)
layout(set = 0, binding = 2) uniform sampler2D g_srcSourceDepth;  // the scene's stored depth (host pixels)
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D g_outRadianceMIP0;
layout(set = 0, binding = 4, rgba16f) uniform writeonly image2D g_outRadianceMIP1;
layout(set = 0, binding = 5, rgba16f) uniform writeonly image2D g_outRadianceMIP2;
layout(set = 0, binding = 6, rgba16f) uniform writeonly image2D g_outRadianceMIP3;
layout(set = 0, binding = 7, rgba16f) uniform writeonly image2D g_outRadianceMIP4;

// KK's SourceRadiance (task 073): the final lit picture is not diffuse reflectance. A bounded amount of energy is
// restored only where the brightness is above the frame's ambient lit albedo ceiling, a sunlit-ness heuristic (no
// sun direction, no material). The smooth ramp avoids a moving hard threshold; one peak keeps the source's hue.
vec3 SourceRadiance(vec3 energy) {
  float ambient = Luma(max(kk_Ambient.rgb, vec3(0.0)));
  float ceiling = ambient * pow(kk_Ambient.w, 1.0 / kk_Shape.z);
  float lower = pow(ceiling, kk_Shape.z);
  float upper = pow(ceiling + max(0.15, ceiling), kk_Shape.z);
  float sunlit = smoothstep(lower, upper, Luma(energy));
  float peak = max(max(energy.r, energy.g), energy.b);
  return energy + (energy / max(peak, 1e-4)) * (kk_Shape.w * sunlit);
}

vec3 RadianceAt(uvec2 id) {
  uvec2 w = min(id, kk_Working.xy - 1u);
  int stepSize = int(kk_Params.x);
  ivec2 base = kk_Source.xy + ivec2(w) * stepSize;
  ivec2 last = kk_Source.xy + kk_Source.zw - 1;
  vec3 sum = vec3(0.0);
  for (int j = 0; j < stepSize; ++j) {
    for (int i = 0; i < stepSize; ++i) {
      ivec2 p = min(base + ivec2(i, j), last);
      // Every host texel is tested before the mean: a half resolution block may hold both rock and sky, and the
      // sky must never send light.
      if (!IsSky(texelFetch(g_srcSourceDepth, p, 0).x)) sum += texelFetch(g_srcSceneColor, p, 0).rgb;
    }
  }
  return SourceRadiance(ToLinear(sum / float(stepSize * stepSize)));
}

shared vec3 g_scratch[8][8];

void main() {
  uvec2 baseCoord = gl_GlobalInvocationID.xy;
  uvec2 t = gl_LocalInvocationID.xy;
  uvec2 pixCoord = baseCoord * 2u;
  vec3 r0 = RadianceAt(pixCoord);
  vec3 r1 = RadianceAt(pixCoord + uvec2(1u, 0u));
  vec3 r2 = RadianceAt(pixCoord + uvec2(0u, 1u));
  vec3 r3 = RadianceAt(pixCoord + uvec2(1u, 1u));
  imageStore(g_outRadianceMIP0, ivec2(pixCoord), vec4(r0, 1.0));
  imageStore(g_outRadianceMIP0, ivec2(pixCoord + uvec2(1u, 0u)), vec4(r1, 1.0));
  imageStore(g_outRadianceMIP0, ivec2(pixCoord + uvec2(0u, 1u)), vec4(r2, 1.0));
  imageStore(g_outRadianceMIP0, ivec2(pixCoord + uvec2(1u, 1u)), vec4(r3, 1.0));

  vec3 m1 = (r0 + r1 + r2 + r3) * 0.25;
  imageStore(g_outRadianceMIP1, ivec2(baseCoord), vec4(m1, 1.0));
  g_scratch[t.x][t.y] = m1;
  memoryBarrierShared();
  barrier();

  if (all(equal(t % 2u, uvec2(0u)))) {
    vec3 m2 = (g_scratch[t.x][t.y] + g_scratch[t.x + 1u][t.y] + g_scratch[t.x][t.y + 1u] +
               g_scratch[t.x + 1u][t.y + 1u]) * 0.25;
    imageStore(g_outRadianceMIP2, ivec2(baseCoord / 2u), vec4(m2, 1.0));
    g_scratch[t.x][t.y] = m2;
  }
  memoryBarrierShared();
  barrier();

  if (all(equal(t % 4u, uvec2(0u)))) {
    vec3 m3 = (g_scratch[t.x][t.y] + g_scratch[t.x + 2u][t.y] + g_scratch[t.x][t.y + 2u] +
               g_scratch[t.x + 2u][t.y + 2u]) * 0.25;
    imageStore(g_outRadianceMIP3, ivec2(baseCoord / 4u), vec4(m3, 1.0));
    g_scratch[t.x][t.y] = m3;
  }
  memoryBarrierShared();
  barrier();

  if (all(equal(t % 8u, uvec2(0u)))) {
    vec3 m4 = (g_scratch[t.x][t.y] + g_scratch[t.x + 4u][t.y] + g_scratch[t.x][t.y + 4u] +
               g_scratch[t.x + 4u][t.y + 4u]) * 0.25;
    imageStore(g_outRadianceMIP4, ivec2(baseCoord / 8u), vec4(m4, 1.0));
  }
}
)GLSL";

// KK's cs_gather: a visibility bitmask over GTAO's slices. Each slice keeps 32 sectors of the pixel's hemisphere,
// spaced evenly in the slice's own measure of light (GTAO's integrand: the cosine to the normal times the sine of the
// angle from the view vector). A sample covers the sectors between its front and its back (pushed `thickness`
// away); only sectors no nearer sample has covered let its light through, and only if both surfaces face each other.
// rgb: the light from what hides the sky (linear), a: the share of the hemisphere nothing hides.
constexpr const char* kGather = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 1) uniform sampler2D g_srcViewDepth;   // view space depth, all five mips (point)
layout(set = 0, binding = 2) uniform usampler2D g_srcNormalmap;
layout(set = 0, binding = 3) uniform sampler2D g_srcRadiance;    // the source light, working size, five mips (linear)
layout(set = 0, binding = 4) uniform sampler2D g_srcRawDepth;    // the working stored depth (sky test)
layout(set = 0, binding = 5, rgba16f) uniform writeonly image2D g_outGi;

vec3 LoadNormalAt(ivec2 pos) {
  uint packedInput = texelFetch(g_srcNormalmap, pos, 0).x;
  vec3 n = XeGTAO_R11G11B10_UNORM_to_FLOAT3(packedInput) * 2.0 - 1.0;
  float l = length(n);
  return l > 1e-4 ? n / l : vec3(0.0, 0.0, -1.0);
}

// The slice's measure of light from its lower edge up to angle t (from the view vector, signed): the integral of
// cos(t - n) |sin t|. sinN, cosN: sin(n), cos(n).
float SliceMeasure(float t, float n, float sinN, float cosN) {
  float c = cos(2.0 * t - n) * 0.25, k = t * sinN * 0.5, z = cosN * 0.25;
  return t >= 0.0 ? (-c + k + z) : (c - k - z);
}

// The sectors between two heights u0 <= u1 (0 to 1), each edge rounded to the nearest boundary: a sliver covers
// nothing, so a flat surface does not hide its own sky.
uint SectorBits(float u0, float u1) {
  uint a = uint(clamp(round(u0 * 32.0), 0.0, 32.0));
  uint b = uint(clamp(round(u1 * 32.0), 0.0, 32.0));
  if (b <= a) return 0u;
  uint count = b - a;
  uint run = count >= 32u ? 0xFFFFFFFFu : ((1u << count) - 1u);
  return a >= 32u ? 0u : (run << a);
}

void main() {
  uvec2 pix = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pix, kk_Working.zw))) return;
  if (any(greaterThanEqual(pix, kk_Working.xy)) || IsSky(texelFetch(g_srcRawDepth, ivec2(pix), 0).x)) {
    imageStore(g_outGi, ivec2(pix), vec4(0.0, 0.0, 0.0, 1.0));  // the sky, or the padding
    return;
  }
  GTAOConstants consts = g_GTAOConsts;
  vec2 uv = (vec2(pix) + 0.5) * consts.ViewportPixelSize;
  float z = texelFetch(g_srcViewDepth, ivec2(pix), 0).x * 0.99999;
  vec3 P = XeGTAO_ComputeViewspacePosition(uv, z, consts);
  if (kk_Distance.x > 0.0 && length(P) >= kk_Distance.x) {
    imageStore(g_outGi, ivec2(pix), vec4(0.0, 0.0, 0.0, 1.0));
    return;  // no gather samples for distant receivers
  }
  vec3 V = normalize(-P);
  vec3 N = LoadNormalAt(ivec2(pix));

  float radius = kk_Params.z;
  float thickness = kk_Params.w;
  // View space size of a working pixel at this depth, and the radius on the screen.
  float pixelSize = z * consts.NDCToViewMul_x_PixelSize.x;
  float radiusPx = min(radius / max(pixelSize, 1e-6), kk_Gather.z);
  if (radiusPx < 2.0) {
    imageStore(g_outGi, ivec2(pix), vec4(0.0, 0.0, 0.0, 1.0));
    return;
  }
  uint sliceCount = uint(kk_Gather.x);
  uint stepCount = uint(kk_Gather.y);
  float minS = 1.3 / radiusPx;  // not the pixel itself
  float maxMip = kk_Mode.z - 1.0;
  vec2 noise = Noise(pix);

  vec3 light = vec3(0.0);
  float hidden = 0.0;
  float weightSum = 0.0;
  for (uint slice = 0u; slice < sliceCount; ++slice) {
    float phi = (float(slice) + noise.x) / float(sliceCount) * SSGI_PI;
    float cosPhi = cos(phi), sinPhi = sin(phi);
    vec2 omega = vec2(cosPhi, -sinPhi) * radiusPx;  // working pixels (y down)
    vec3 dirV = vec3(cosPhi, sinPhi, 0.0);           // view space (y up)
    vec3 ortho = dirV - dot(dirV, V) * V;
    vec3 axis = normalize(cross(ortho, V));
    vec3 projN = N - axis * dot(N, axis);
    float projLen = length(projN);
    float cosN = clamp(dot(projN, V) / max(projLen, 1e-4), 0.0, 1.0);
    float n = (dot(ortho, projN) >= 0.0 ? 1.0 : -1.0) * XeGTAO_FastACos(cosN);
    // The slice's measure from the normal's one horizon to the other.
    float sinN = sin(n), cosNs = cos(n);
    float low = SliceMeasure(n - SSGI_HALF_PI, n, sinN, cosNs);
    float total = max(SliceMeasure(n + SSGI_HALF_PI, n, sinN, cosNs) - low, 1e-4);
    float invTotal = 1.0 / total;

    uint mask = 0u;
    vec3 sliceLight = vec3(0.0);
    for (uint side = 0u; side < 2u; ++side) {
      float sideSign = side == 0u ? 1.0 : -1.0;
      for (uint s = 0u; s < stepCount; ++s) {
        float stepNoise = fract(noise.y + float(slice + s * sliceCount + side * 7u) * 0.6180339887);
        float t = (float(s) + stepNoise) / float(stepCount);
        t = t * t + minS;  // denser near the pixel
        vec2 offset = round(t * omega) * sideSign;
        vec2 sp = vec2(pix) + 0.5 + offset;
        if (any(lessThan(sp, vec2(0.0))) || any(greaterThanEqual(sp, vec2(kk_Working.xy)))) break;
        float offsetLength = length(offset);
        vec2 suv = sp * consts.ViewportPixelSize;
        float mip = clamp(log2(offsetLength) - consts.DepthMIPSamplingOffset, 0.0, float(XE_GTAO_DEPTH_MIP_LEVELS - 1));
        float sz = textureLod(g_srcViewDepth, suv, mip).x;
        vec3 S = XeGTAO_ComputeViewspacePosition(suv, sz, consts);
        vec3 d = S - P;
        float dist = length(d);
        // No transport of sub-surface or local reconstruction noise (KK task 073).
        if (dist > radius || dist < max(0.15, 2.0 * abs(pixelSize))) continue;
        vec3 back = d - V * thickness;
        float af = XeGTAO_FastACos(dot(d / dist, V));
        float ab = XeGTAO_FastACos(dot(normalize(back), V));
        // Angles in the slice from the view vector, positive towards +omega, within the normal's hemisphere, then
        // into the sector space.
        vec2 h = side == 0u ? vec2(af, ab) : vec2(-ab, -af);
        h = clamp(h, vec2(n - SSGI_HALF_PI), vec2(n + SSGI_HALF_PI));
        vec2 u = clamp((vec2(SliceMeasure(h.x, n, sinN, cosNs), SliceMeasure(h.y, n, sinN, cosNs)) - low) * invTotal,
                       0.0, 1.0);
        uint bits = SectorBits(u.x, u.y);
        uint fresh = bits & ~mask;
        mask |= bits;
        if (fresh == 0u) continue;
        // The surface there must face this pixel to light it, and this pixel must face it.
        vec3 Ns = LoadNormalAt(ivec2(sp));
        vec3 dir = d / dist;
        float sourceFacing = dot(Ns, -dir);
        float receiverFacing = dot(N, dir);
        if (sourceFacing <= 0.0 || receiverFacing <= 0.0) continue;
        // A smooth grazing band: a nearly tangent sample on the same surface must not become a bright emitter.
        float facing = clamp(sourceFacing * 2.0, 0.0, 1.0) * clamp(receiverFacing * 2.0, 0.0, 1.0);
        float fade = clamp((radius - dist) / (0.3 * radius), 0.0, 1.0);
        // The radiance's mip of about the samples' spacing there, so a far sample stands for the surfaces around it.
        float spacing = 2.0 * sqrt(offsetLength * radiusPx) / float(stepCount);
        float rmip = clamp(log2(max(spacing * 0.5, 1.0)), 0.0, maxMip);
        vec3 L = textureLod(g_srcRadiance, suv, rmip).rgb;
        sliceLight += L * (float(bitCount(fresh)) / 32.0) * facing * fade;
      }
    }
    float sliceWeight = projLen * total;
    light += sliceLight * sliceWeight;
    hidden += float(bitCount(mask)) / 32.0 * sliceWeight;
    weightSum += sliceWeight;
  }
  float inv = weightSum > 1e-4 ? 1.0 / weightSum : 0.0;
  // Fade in over small radii on the screen (a few pixels give no reliable slices).
  float fadeIn = clamp((radiusPx - 2.0) / 6.0, 0.0, 1.0);
  imageStore(g_outGi, ivec2(pix), vec4(light * inv * fadeIn, 1.0 - clamp(hidden * inv, 0.0, 1.0) * fadeIn));
}
)GLSL";

// KK's cs_temporal: this frame's gathered light (g_srcGi) blended into last frame's where the camera saw this pixel's
// surface then (its depth there within 5 % of this point's depth then) and on the picture; the history clamped to the
// light around the pixel now, widened by half its range. The point goes through NativeXRenderer's clip space (the
// picture, D3D's y up, the viewport's depth range), as gtao_shaders.h's kTemporal does it.
constexpr const char* kTemporal = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 1) uniform sampler2D g_srcGi;
layout(set = 0, binding = 2) uniform sampler2D g_srcHistory;       // last frame's accumulated light (working size)
layout(set = 0, binding = 3) uniform sampler2D g_srcHistoryDepth;  // last frame's view space depth (working size)
layout(set = 0, binding = 4) uniform sampler2D g_srcRawDepth;      // this frame's working stored depth
layout(set = 0, binding = 5, rgba16f) uniform writeonly image2D g_outGi;

void main() {
  uvec2 pix = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pix, kk_Working.zw))) return;
  vec4 current = texelFetch(g_srcGi, ivec2(pix), 0);
  float stored = texelFetch(g_srcRawDepth, ivec2(pix), 0).x;
  if (kk_Extra.z < 0.5 || any(greaterThanEqual(pix, kk_Working.xy)) || IsSky(stored) ||
      BeyondDistance(vec2(pix), XeGTAO_ScreenSpaceToViewSpaceDepth(stored, g_GTAOConsts))) {
    imageStore(g_outGi, ivec2(pix), current);
    return;
  }
  // Working pixel i is picture pixel step * i: its centre in this frame's clip space, then in last frame's.
  float stepSize = kk_Params.x;
  vec2 size = vec2(kk_Source.zw);
  vec2 uv = (vec2(pix) * stepSize + 0.5) / size;
  vec4 clip = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, (stored - kk_DepthRange.y) / kk_DepthRange.x, 1.0);
  vec4 prev = vec4(dot(kk_Reprojection[0], clip), dot(kk_Reprojection[1], clip), dot(kk_Reprojection[2], clip),
                   dot(kk_Reprojection[3], clip));
  if (abs(prev.w) <= 1e-9) {
    imageStore(g_outGi, ivec2(pix), current);
    return;
  }
  vec3 pn = prev.xyz / prev.w;
  vec2 prevPicture = vec2(pn.x * 0.5 + 0.5, 0.5 - pn.y * 0.5) * size - 0.5;  // picture pixels (centres at .0)
  vec2 pw = prevPicture / stepSize;                                          // working pixels (centres at .0)
  if (any(lessThan(pw, vec2(-0.5))) || any(greaterThan(pw, vec2(kk_Working.xy) - 0.5))) {
    imageStore(g_outGi, ivec2(pix), current);
    return;
  }
  // The point's view depth then, from its clip z through last frame's depth unpack.
  float expected = pn.z * kk_DepthRange.x + kk_DepthRange.y;
  float prevZ = kk_DepthRange.z / (kk_DepthRange.w - expected);
  ivec2 base = ivec2(floor(pw));
  vec2 t = pw - vec2(base);
  ivec2 last = ivec2(kk_Working.xy) - 1;
  vec4 history = vec4(0.0);
  float weight = 0.0;
  for (int j = 0; j < 4; ++j) {
    ivec2 o = ivec2(j & 1, j >> 1);
    ivec2 c = clamp(base + o, ivec2(0), last);
    float bilinear = (o.x != 0 ? t.x : 1.0 - t.x) * (o.y != 0 ? t.y : 1.0 - t.y);
    float hz = texelFetch(g_srcHistoryDepth, c, 0).x;
    float same = (prevZ > 0.0 && abs(hz - prevZ) <= 0.05 * prevZ) ? 1.0 : 0.0;
    history += texelFetch(g_srcHistory, c, 0) * bilinear * same;
    weight += bilinear * same;
  }
  if (weight < 0.25) {
    imageStore(g_outGi, ivec2(pix), current);  // a surface the camera did not see last frame
    return;
  }
  history /= weight;
  vec4 lo = current, hi = current;
  for (int k = 0; k < 9; ++k) {
    ivec2 q = clamp(ivec2(pix) + ivec2(k % 3 - 1, k / 3 - 1), ivec2(0), last);
    vec4 v = texelFetch(g_srcGi, q, 0);
    lo = min(lo, v);
    hi = max(hi, v);
  }
  vec4 pad = (hi - lo) * 0.5;
  history = clamp(history, lo - pad, hi + pad);
  imageStore(g_outGi, ivec2(pix), mix(history, current, kk_Extra.w));
}
)GLSL";

// KK's cs_denoise: a 3x3 (two slices) or 5x5 a-trous blur with step kk_Mode.w, weighted by depth and normal.
constexpr const char* kDenoise = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 1) uniform sampler2D g_srcGi;
layout(set = 0, binding = 2) uniform sampler2D g_srcDenoiseDepth;     // working view space depth (mip 0)
layout(set = 0, binding = 3) uniform usampler2D g_srcDenoiseNormals;
layout(set = 0, binding = 4) uniform sampler2D g_srcRawDepth;         // working stored depth (sky test)
layout(set = 0, binding = 5, rgba16f) uniform writeonly image2D g_outGi;

vec3 LoadNormalAt(ivec2 pos) {
  uint packedInput = texelFetch(g_srcDenoiseNormals, pos, 0).x;
  vec3 n = XeGTAO_R11G11B10_UNORM_to_FLOAT3(packedInput) * 2.0 - 1.0;
  float l = length(n);
  return l > 1e-4 ? n / l : vec3(0.0, 0.0, -1.0);
}

void main() {
  uvec2 pix = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pix, kk_Working.zw))) return;
  vec4 centre = texelFetch(g_srcGi, ivec2(pix), 0);
  if (any(greaterThanEqual(pix, kk_Working.xy)) || IsSky(texelFetch(g_srcRawDepth, ivec2(pix), 0).x)) {
    imageStore(g_outGi, ivec2(pix), centre);
    return;
  }
  float z = texelFetch(g_srcDenoiseDepth, ivec2(pix), 0).x;
  if (BeyondDistance(vec2(pix), z)) {
    imageStore(g_outGi, ivec2(pix), vec4(0.0, 0.0, 0.0, 1.0));
    return;
  }
  vec3 N = LoadNormalAt(ivec2(pix));
  int stepPx = int(kk_Mode.w);
  ivec2 last = ivec2(kk_Working.xy) - 1;
  const float kKernel[3] = float[3](0.375, 0.25, 0.0625);
  vec4 sum = vec4(0.0);
  float weight = 0.0;
  for (int j = -2; j <= 2; ++j) {
    for (int i = -2; i <= 2; ++i) {
      // Two-slice levels use a compact 3x3 kernel; High keeps the 5x5 one.
      if (kk_Gather.x <= 2.0 && (abs(i) == 2 || abs(j) == 2)) continue;
      ivec2 q = ivec2(pix) + ivec2(i, j) * stepPx;
      if (any(lessThan(q, ivec2(0))) || any(greaterThan(q, last))) continue;
      float zq = texelFetch(g_srcDenoiseDepth, q, 0).x;
      vec3 Nq = LoadNormalAt(q);
      float w = kKernel[abs(i)] * kKernel[abs(j)];
      w *= exp(-abs(zq - z) / max(z * 0.03 * float(stepPx), 1e-5));
      float nd = clamp(dot(N, Nq), 0.0, 1.0);
      nd *= nd;
      nd *= nd;
      w *= nd * nd;
      if (IsSky(texelFetch(g_srcRawDepth, q, 0).x)) w = 0.0;  // the sky has no light to share
      sum += texelFetch(g_srcGi, q, 0) * w;
      weight += w;
    }
  }
  imageStore(g_outGi, ivec2(pix), weight > 1e-6 ? sum / weight : centre);
}
)GLSL";

// One triangle covering the viewport (the picture); the pixel shader works from gl_FragCoord.
constexpr const char* kCompositeVS = R"GLSL(#version 450
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// KK's ps_composite, into the scene colour target (alpha not written). The lighting gives colour = albedo x
// (ambient + direct) in gamma space; a product stays a product in linear light, so the albedo is estimated there:
// colour / ambient in the shade, the brightness over albedo-limit x ambient where lit, never above the limit. Then
// colour + albedo x (bounce x intensity x bounces - ambient x hidden share x occlusion), never darker than
// kk_Shape.y of the pixel's own light, the positive part bounded to 75 % of the receiver's linear light, faded with
// distance, encoded back to the picture's gamma.
constexpr const char* kCompositePS = R"GLSL(
layout(set = 0, binding = 1) uniform sampler2D g_srcSceneCopy;   // the scene colour as it was (host pixels)
layout(set = 0, binding = 2) uniform sampler2D g_srcGiResult;    // the gathered light (working size)
layout(set = 0, binding = 3) uniform sampler2D g_srcWorkDepth;   // working view space depth (mip 0)
layout(set = 0, binding = 4) uniform sampler2D g_srcSceneDepth;  // the scene's stored depth (host pixels)
layout(location = 0) out vec4 out_color;

// The gathered light at a host pixel of the picture: the working pixels around it, bilinear and weighted by how
// close their depth is to this pixel's (as gtao_shaders.h's kOutput).
vec4 GiAt(ivec2 q, float z) {
  float stepSize = kk_Params.x;
  if (stepSize < 1.5) return texelFetch(g_srcGiResult, q, 0);
  vec2 f = vec2(q) / stepSize;
  ivec2 base = ivec2(floor(f));
  vec2 t = f - vec2(base);
  ivec2 last = ivec2(kk_Working.xy) - 1;
  vec4 total = vec4(0.0);
  float weight = 0.0;
  for (int j = 0; j < 4; ++j) {
    ivec2 o = ivec2(j & 1, j >> 1);
    ivec2 c = clamp(base + o, ivec2(0), last);
    float bilinear = (o.x != 0 ? t.x : 1.0 - t.x) * (o.y != 0 ? t.y : 1.0 - t.y);
    float dz = abs(texelFetch(g_srcWorkDepth, c, 0).x - z) / max(z, 1e-6);
    float w = (bilinear + 1e-3) / (1e-3 + dz * 50.0);
    total += texelFetch(g_srcGiResult, c, 0) * w;
    weight += w;
  }
  return weight > 0.0 ? total / weight : vec4(0.0, 0.0, 0.0, 1.0);
}

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 colour = texelFetch(g_srcSceneCopy, p, 0);
  ivec2 q = p - kk_Source.xy;
  float stored = texelFetch(g_srcSceneDepth, p, 0).x;
  if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, kk_Source.zw)) || IsSky(stored)) {
    out_color = colour;  // outside the picture, or the sky
    return;
  }
  float z = XeGTAO_ScreenSpaceToViewSpaceDepth(stored, g_GTAOConsts);
  float distanceFade = 1.0;
  if (kk_Distance.x > 0.0) {
    float cameraDistance = CameraDistance(vec2(q) / kk_Params.x, z);
    if (cameraDistance >= kk_Distance.x) {
      out_color = colour;
      return;
    }
    distanceFade = 1.0 - smoothstep(kk_Distance.y, kk_Distance.x, cameraDistance);
  }
  vec4 gi = GiAt(q, z);
  float hiddenShare = clamp(1.0 - gi.a, 0.0, 1.0);
  vec3 lit = ToLinear(colour.rgb);
  vec3 A = max(ToLinear(kk_Ambient.rgb), vec3(1e-5));
  float limit = kk_Ambient.w;  // linear
  float scale = max(1.0, Luma(lit) / max(limit * Luma(A), 1e-7));
  vec3 albedo = min(lit / (A * scale), vec3(limit));
  float bounces = 1.0 / max(1.0 - Luma(albedo) * hiddenShare * kk_Shape.x, 0.4);
  vec3 bounce = gi.rgb * kk_Gather.w * bounces;
  vec3 delta = albedo * (bounce - A * hiddenShare * kk_Mode.x);
  delta = max(delta, -kk_Shape.y * lit);
  // The final addition bounded relative to this receiver's own linear colour, all channels together (the bounce's
  // hue kept): at most 75 % extra linear light (about 29 % display lift), even at high intensity.
  vec3 relative = max(delta, vec3(0.0)) / max(lit, vec3(1e-6));
  float largest = max(max(relative.r, relative.g), relative.b);
  delta = min(delta, vec3(0.0)) + max(delta, vec3(0.0)) * min(1.0, 0.75 / max(largest, 1e-6));
  // The fade after the receiver bound, so a saturated bounce cannot cancel it.
  delta *= distanceFade;
  bounce *= distanceFade;
  gi.a = mix(1.0, gi.a, distanceFade);
  vec3 result = ToGamma(lit + delta);
  int debugView = int(kk_Mode.y + 0.5);
  if (debugView == 1) {
    out_color = vec4(ToGamma(bounce), colour.a);
  } else if (debugView == 2) {
    out_color = vec4(clamp(0.5 + (result - colour.rgb) * 2.0, 0.0, 1.0), colour.a);
  } else if (debugView == 3) {
    out_color = vec4(gi.aaa, colour.a);
  } else {
    out_color = vec4(result, colour.a);
  }
}
)GLSL";

}  // namespace ssgi_glsl
}  // namespace native_x_renderer
