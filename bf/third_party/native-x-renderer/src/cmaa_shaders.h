// NativeXRenderer - Intel's Conservative Morphological Anti-Aliasing 2.3 as GLSL compute shaders (internal; aa.cpp).
//
// A port of CMAA2.hlsl (https://github.com/GameTechDev/CMAA2, Copyright (c) 2018 Intel Corporation, author Filip
// Strugar, Apache License 2.0: THIRD_PARTY_NOTICES.md), its single sample path with the luma computed in place
// (CMAA2_EDGE_DETECTION_LUMA_PATH 1), the edges packed two to a texel, the blend items collected and spread over the
// group (CMAA2_COLLECT_EXPAND_BLEND_ITEMS) and the deferred apply's thread group swap. What differs from the original:
//   - The image is the game's finished one, gamma encoded: its luma is taken as it is (the original takes the square
//     root of linear colour) and the blending is done on the encoded values, as this renderer's FXAA and SMAA do.
//   - The last pass does not write the image: it writes the changed pixels to a texture of its own (alpha 1), and a
//     full-screen pass lays them over the image. So the image needs no unordered access and keeps its alpha.
//   - Vulkan does not define a read or write outside an image or a buffer: every one is guarded here (Direct3D
//     reads zero and drops the write), and the colours read at the image's border are its border's.
//   - The edge threshold is a constant of the pass (the presets Low 0.15, Medium 0.10, High 0.07, Ultra 0.05).

#pragma once

namespace native_x_renderer {

// Before each shader: the constants and the working buffers' layouts.
constexpr const char* kCmaaCommon = R"(#version 450
layout(std140, set = 0, binding = 0) uniform CmaaConstants {
  ivec2 c_size;            // the image
  uint c_candidates_max;   // elements of CmaaCandidates
  uint c_items_max;        // of CmaaItems
  uint c_locations_max;    // of CmaaLocations
  float c_threshold;
};
// Counters and counts: [3] the items of the indirect pass about to run, [4] shape candidates, [8] blend locations,
// [12] blend items (the original's byte offsets over four).
layout(std430, set = 0, binding = 1) buffer CmaaControl { uint g_control[]; };
const float kLocalContrast = 0.10;
const float kSimpleShapeBlur = 0.10;
const uint kMaxLineLength = 86u;
)";

// EdgesColor2x2CS: each thread a 2x2 block; a 16x16 group reads a 32x32 pixel kernel and writes its inner 28x28.
constexpr const char* kCmaaEdgesCS = R"(
layout(local_size_x = 16, local_size_y = 16) in;
layout(set = 0, binding = 2) uniform sampler2D g_color;
layout(set = 0, binding = 3, r8ui) uniform uimage2D g_edges;
layout(set = 0, binding = 4, r32ui) uniform uimage2D g_heads;
layout(std430, set = 0, binding = 5) buffer CmaaCandidates { uint g_candidates[]; };

shared vec4 s_edges_h[256];
shared vec4 s_edges_v[256];

float Luma(ivec2 p) {
  p = clamp(p, ivec2(0), c_size - 1);
  return dot(texelFetch(g_color, p, 0).rgb, vec3(0.299, 0.587, 0.114));
}
void Quad(int addr, out vec2 e00, out vec2 e10, out vec2 e01, out vec2 e11) {
  vec4 h = s_edges_h[addr], v = s_edges_v[addr];
  e00 = vec2(v.x, h.x), e10 = vec2(v.y, h.y), e01 = vec2(v.z, h.z), e11 = vec2(v.w, h.w);
}
vec2 g_n[16];  // the 4x4 neighbourhood, [x * 4 + y]
float ContrastV(int x, int y) {
  return max(max(g_n[(x + 1) * 4 + y].y, g_n[(x + 1) * 4 + y + 1].y),
             max(g_n[(x + 2) * 4 + y].y, g_n[(x + 2) * 4 + y + 1].y)) * kLocalContrast;
}
float ContrastH(int x, int y) {
  return max(max(g_n[x * 4 + y + 1].x, g_n[(x + 1) * 4 + y + 1].x),
             max(g_n[x * 4 + y + 2].x, g_n[(x + 1) * 4 + y + 2].x)) * kLocalContrast;
}
float Edge(float value, float contrast) { return (value - contrast) > c_threshold ? 1.0 : 0.0; }

void main() {
  const ivec2 gt = ivec2(gl_LocalInvocationID.xy);
  const ivec2 pixel = (ivec2(gl_WorkGroupID.xy) * 14 + gt - 1) * 2;
  const int stride = 16;
  const int center = gt.x + gt.y * stride;
  const bool in_output = gt.x > 0 && gt.x < 15 && gt.y > 0 && gt.y < 15;

  float l[8];
  for (int i = 0; i < 8; ++i) l[i] = Luma(pixel + ivec2(i % 3, i / 3));
  // .x: the edge to the right, .y: the edge below
  vec2 qe0 = vec2(abs(l[0] - l[1]), abs(l[0] - l[3]));
  vec2 qe1 = vec2(abs(l[1] - l[2]), abs(l[1] - l[4]));
  vec2 qe2 = vec2(abs(l[3] - l[4]), abs(l[3] - l[6]));
  vec2 qe3 = vec2(abs(l[4] - l[5]), abs(l[4] - l[7]));
  s_edges_v[center] = vec4(qe0.x, qe1.x, qe2.x, qe3.x);
  s_edges_h[center] = vec4(qe0.y, qe1.y, qe2.y, qe3.y);

  memoryBarrierShared();
  barrier();

  uint out_edges[4] = uint[4](0u, 0u, 0u, 0u);
  if (in_output) {
    vec2 top_row = s_edges_h[center - stride].zw;   // the row above's bottom edges
    vec2 left_column = s_edges_v[center - 1].yw;    // the column to the left's right edges
    const bool some = any(notEqual(vec4(qe0, qe1) + vec4(qe2, qe3) + vec4(top_row, left_column), vec4(0.0)));
    if (some) {
      const ivec2 quad = pixel / 2;
      if (all(lessThan(quad, imageSize(g_heads)))) imageStore(g_heads, quad, uvec4(0xFFFFFFFFu));

      for (int i = 0; i < 16; ++i) g_n[i] = vec2(0.0);
      vec2 d0, d1, d2;
      Quad(center - stride - 1, d0, d1, d2, g_n[0 * 4 + 0]);
      Quad(center - stride, d0, d1, g_n[1 * 4 + 0], g_n[2 * 4 + 0]);
      Quad(center - stride + 1, d0, d1, g_n[3 * 4 + 0], d2);
      Quad(center - 1, d0, g_n[0 * 4 + 1], d1, g_n[0 * 4 + 2]);
      Quad(center + 1, g_n[3 * 4 + 1], d0, g_n[3 * 4 + 2], d1);
      Quad(center - 1 + stride, d0, g_n[0 * 4 + 3], d1, d2);
      Quad(center + stride, g_n[1 * 4 + 3], g_n[2 * 4 + 3], d0, d1);
      g_n[1 * 4 + 0].y = top_row.x;
      g_n[2 * 4 + 0].y = top_row.y;
      g_n[0 * 4 + 1].x = left_column.x;
      g_n[0 * 4 + 2].x = left_column.y;
      g_n[1 * 4 + 1] = qe0;
      g_n[2 * 4 + 1] = qe1;
      g_n[1 * 4 + 2] = qe2;
      g_n[2 * 4 + 2] = qe3;

      top_row.x = Edge(top_row.x, ContrastH(0, -1));
      top_row.y = Edge(top_row.y, ContrastH(1, -1));
      left_column.x = Edge(left_column.x, ContrastV(-1, 0));
      left_column.y = Edge(left_column.y, ContrastV(-1, 1));

      vec4 ce[4];
      ce[0].x = Edge(qe0.x, ContrastV(0, 0));
      ce[0].y = Edge(qe0.y, ContrastH(0, 0));
      ce[1].x = Edge(qe1.x, ContrastV(1, 0));
      ce[1].y = Edge(qe1.y, ContrastH(1, 0));
      ce[2].x = Edge(qe2.x, ContrastV(0, 1));
      ce[2].y = Edge(qe2.y, ContrastH(0, 1));
      ce[3].x = Edge(qe3.x, ContrastV(1, 1));
      ce[3].y = Edge(qe3.y, ContrastH(1, 1));
      // left
      ce[0].z = left_column.x;
      ce[1].z = ce[0].x;
      ce[2].z = left_column.y;
      ce[3].z = ce[2].x;
      // top
      ce[0].w = top_row.x;
      ce[1].w = top_row.y;
      ce[2].w = ce[0].y;
      ce[3].w = ce[1].y;

      const ivec2 offsets[4] = ivec2[4](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 1));
      for (int i = 0; i < 4; ++i) {
        const ivec2 at = pixel + offsets[i];
        const vec4 e = ce[i];
        // A corner of two edges: a candidate for the simple or the complex shapes.
        const bool candidate = (e.x * e.y + e.y * e.z + e.z * e.w + e.w * e.x) != 0.0;
        if (candidate && all(lessThan(at, c_size))) {
          const uint index = atomicAdd(g_control[4], 1u);
          if (index < c_candidates_max) g_candidates[index] = (uint(at.x) << 18) | uint(at.y);
        }
        out_edges[i] = uint(dot(e, vec4(1.0, 2.0, 4.0, 8.0)));
      }
    }
    // All of them are written, the empty ones too, so no edge of an earlier frame is traced.
    const ivec2 size = imageSize(g_edges);
    const ivec2 a = ivec2(pixel.x / 2, pixel.y), b = ivec2(pixel.x / 2, pixel.y + 1);
    if (all(lessThan(a, size))) imageStore(g_edges, a, uvec4((out_edges[1] << 4) | out_edges[0]));
    if (all(lessThan(b, size))) imageStore(g_edges, b, uvec4((out_edges[3] << 4) | out_edges[2]));
  }
}
)";

// ComputeDispatchArgsCS: the indirect arguments of the two passes that follow, from the counters.
constexpr const char* kCmaaArgsCS = R"(
layout(local_size_x = 1) in;
layout(std430, set = 0, binding = 2) buffer CmaaArgs { uint g_args[]; };
void main() {
  if (gl_WorkGroupID.x == 1u) {  // Dispatch(2, 1, 1): the candidates' pass
    const uint count = min(g_control[4], c_candidates_max);
    g_args[0] = (count + 127u) / 128u;
    g_args[1] = 1u;
    g_args[2] = 1u;
    g_control[3] = count;
  } else if (gl_WorkGroupID.y == 1u) {  // Dispatch(1, 2, 1): the deferred apply
    const uint count = min(g_control[8], c_locations_max);
    g_args[0] = 1u;
    g_args[1] = (count + 31u) / 32u;
    g_args[2] = 1u;
    g_control[3] = count;
    g_control[4] = 0u;  // for the next image
    g_control[8] = 0u;
    g_control[12] = 0u;
  }
}
)";

// ProcessCandidatesCS: the simple shapes' blends, the Z shapes found and their lines' blends, all as items in each
// 2x2 quad's list.
constexpr const char* kCmaaProcessCS = R"(
layout(local_size_x = 128) in;
layout(set = 0, binding = 2) uniform sampler2D g_color;
layout(set = 0, binding = 3, r8ui) uniform uimage2D g_edges;
layout(set = 0, binding = 4, r32ui) uniform uimage2D g_heads;
layout(std430, set = 0, binding = 5) buffer CmaaCandidates { uint g_candidates[]; };
layout(std430, set = 0, binding = 6) buffer CmaaItems { uvec2 g_items[]; };
layout(std430, set = 0, binding = 7) buffer CmaaLocations { uint g_locations[]; };

const uint kSharedItems = 768u;
shared uint s_item_count;
shared uvec2 s_items[768];

vec3 Color(ivec2 p) { return texelFetch(g_color, clamp(p, ivec2(0), c_size - 1), 0).rgb; }

uint LoadEdge(ivec2 p) {
  if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, c_size))) return 0u;
  const uint e = imageLoad(g_edges, ivec2(p.x >> 1, p.y)).r;
  return (e >> uint((p.x & 1) * 4)) & 0xFu;
}
vec4 Unpack(uint e) {
  return vec4((e & 1u) != 0u ? 1.0 : 0.0, (e & 2u) != 0u ? 1.0 : 0.0, (e & 4u) != 0u ? 1.0 : 0.0,
              (e & 8u) != 0u ? 1.0 : 0.0);
}

// R11G11B10 with four exponent bits (the original's, from MiniEngine's PixelPacking_R11G11B10.hlsli, MIT): [0, 2).
uint Half(float v) { return packHalf2x16(vec2(v, 0.0)) & 0xFFFFu; }
uint PackColor(vec3 rgb) {
  rgb = clamp(rgb, vec3(0.0), vec3(uintBitsToFloat(0x3FFFFFFFu)));
  const uint r = ((Half(rgb.r) + 4u) >> 3) & 0x000007FFu;
  const uint g = ((Half(rgb.g) + 4u) << 8) & 0x003FF800u;
  const uint b = ((Half(rgb.b) + 8u) << 18) & 0xFFC00000u;
  return r | g | b;
}

void StoreColor(ivec2 p, vec3 color, bool complex_shape) {
  if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, c_size))) return;
  const uint index = atomicAdd(g_control[12], 1u);
  if (index >= c_items_max) return;
  const ivec2 quad = p / 2;
  const uint offset_xy = uint((p.y & 1) * 2 + (p.x & 1));
  // 2 bits the place in the quad, 3 unused (the MSAA sample), 1 the complex shape, 26 the item's index
  const uint header = (offset_xy << 30) | ((complex_shape ? 1u : 0u) << 26);
  const uint before = imageAtomicExchange(g_heads, quad, index | header);
  g_items[index] = uvec2(before, PackColor(color));
  if (before == 0xFFFFFFFFu) {  // the quad's first: one of the places the last pass visits
    const uint location = atomicAdd(g_control[8], 1u);
    if (location < c_locations_max) g_locations[location] = (uint(quad.x) << 16) | uint(quad.y);
  }
}

vec4 SimpleShapeBlend(vec4 edges, vec4 left, vec4 right, vec4 top, vec4 bottom) {
  float from_right = edges.r, from_below = edges.g, from_left = edges.b, from_above = edges.a;
  float blur = kSimpleShapeBlur;
  const float count = dot(edges, vec4(1.0));
  const float around = dot(left.bga + right.rga + top.rba + bottom.rgb, vec3(1.0));
  if (count == 2.0) {  // an L
    blur *= 0.75;
    const float k = 0.9;
    from_right += k * (edges.g * top.r * (1.0 - left.g) + edges.a * bottom.r * (1.0 - left.a));
    from_below += k * (edges.b * right.g * (1.0 - top.b) + edges.r * left.g * (1.0 - top.r));
    from_left += k * (edges.a * bottom.b * (1.0 - right.a) + edges.g * top.b * (1.0 - right.g));
    from_above += k * (edges.r * left.a * (1.0 - bottom.r) + edges.b * right.a * (1.0 - bottom.b));
  }
  blur *= clamp(1.30 - around / 10.0, 0.0, 1.0);  // less where many edges meet: text and texture detail
  return vec4(from_left, from_above, from_right, from_below) * blur;
}

void DetectZs(vec4 edges, vec4 m1, vec4 p1, vec4 p2, out float inverted, out float normal) {
  inverted = edges.r * edges.g * p1.a;
  inverted *= 2.0 + (m1.g + p2.a) - (edges.a + p1.g) - 0.7 * (p2.g + m1.a + edges.b + p1.r);
  normal = edges.r * edges.a * p1.g;
  normal *= 2.0 + (m1.a + p2.g) - (edges.g + p1.a) - 0.7 * (p2.a + m1.g + edges.b + p1.r);
}

void LineLengths(out float left, out float right, ivec2 at, bool horizontal, bool inverted, ivec2 step_right) {
  uint trace_left = horizontal ? 0x08u : 0x04u;   // the top edge, or the left one
  uint trace_right = horizontal ? 0x02u : 0x01u;  // the bottom edge, or the right one
  if (inverted) {
    const uint t = trace_left;
    trace_left = trace_right;
    trace_right = t;
  }
  bool go_left = true, go_right = true;
  left = 1.0;
  right = 1.0;
  for (uint i = 0u; i < 2u * kMaxLineLength; ++i) {
    const uint edge_left = LoadEdge(at - step_right * int(left));
    const uint edge_right = LoadEdge(at + step_right * (int(right) + 1));
    go_left = go_left && (edge_left & trace_left) == trace_left;
    go_right = go_right && (edge_right & trace_right) == trace_right;
    left += go_left ? 1.0 : 0.0;
    right += go_right ? 1.0 : 0.0;
    float longest = max(right, left);
    if (!go_left && !go_right) longest = float(kMaxLineLength);
    if (longest >= min(float(kMaxLineLength), 1.25 * min(right, left) - 0.25)) break;
  }
}

const float kSymmetryCorrection = 0.22;
const float kDampening = 0.15;

// A Z shape's blends as items in the group's memory; false when it is full (the blends are then done in place).
bool CollectBlendZs(ivec2 at, bool horizontal, bool inverted, float quality, float left, float right) {
  const float left_odd = kSymmetryCorrection * mod(left, 2.0);
  const float right_odd = kSymmetryCorrection * mod(right, 2.0);
  const float dampen = clamp((left + right - quality) * kDampening, 0.0, 1.0);
  const float from = -floor((left + 1.0) / 2.0) + 1.0;
  const float to = floor((right + 1.0) / 2.0);
  const uint count = uint(to - from + 1.0);
  uint index = atomicAdd(s_item_count, count);
  if (index + count > kSharedItems) return false;
  const float total = (to - from) + 1.0 - left_odd - right_odd;
  const float lerp_step = 1.0 / total;
  const float lerp_from = (0.5 - left_odd - from) * lerp_step;
  const uint item_header = (uint(at.x) << 18) | uint(at.y);
  const uint item_static = ((horizontal ? 1u : 0u) << 31) | ((inverted ? 1u : 0u) << 30);
  for (float i = from; i <= to; i += 1.0) {
    const float second = i > 0.0 ? 1.0 : 0.0;
    const float src_offset = 1.0 - second * 2.0;
    const float k = ((lerp_step * i + lerp_from) * src_offset + second) * dampen;
    s_items[index++] = uvec2(item_header, item_static | (uint(i + 256.0) << 20) | (uint(src_offset + 256.0) << 10) |
                                              uint(clamp(k, 0.0, 1.0) * 1023.0 + 0.5));
  }
  return true;
}

void BlendZs(ivec2 at, bool horizontal, bool inverted, float quality, float left, float right, ivec2 step_right) {
  ivec2 blend_dir = horizontal ? ivec2(0, -1) : ivec2(-1, 0);
  if (inverted) blend_dir = -blend_dir;
  const float left_odd = kSymmetryCorrection * mod(left, 2.0);
  const float right_odd = kSymmetryCorrection * mod(right, 2.0);
  const float dampen = clamp((left + right - quality) * kDampening, 0.0, 1.0);
  const float from = -floor((left + 1.0) / 2.0) + 1.0;
  const float to = floor((right + 1.0) / 2.0);
  const float total = (to - from) + 1.0 - left_odd - right_odd;
  const float lerp_step = 1.0 / total;
  const float lerp_from = (0.5 - left_odd - from) * lerp_step;
  for (float i = from; i <= to; i += 1.0) {
    const float second = i > 0.0 ? 1.0 : 0.0;
    const float src_offset = 1.0 - second * 2.0;
    const float k = ((lerp_step * i + lerp_from) * src_offset + second) * dampen;
    const ivec2 p = at + step_right * int(i);
    StoreColor(p, mix(Color(p), Color(p + blend_dir * int(src_offset)), k), true);
  }
}

void main() {
  if (gl_LocalInvocationID.x == 0u) s_item_count = 0u;
  memoryBarrierShared();
  barrier();

  if (gl_GlobalInvocationID.x < g_control[3]) {
    const uint id = g_candidates[gl_GlobalInvocationID.x];
    const ivec2 pixel = ivec2(int(id >> 18), int(id & 0x3FFFu));

    const vec4 edges = Unpack(LoadEdge(pixel));
    const vec4 edges_left = Unpack(LoadEdge(pixel + ivec2(-1, 0)));
    const vec4 edges_right = Unpack(LoadEdge(pixel + ivec2(1, 0)));
    const vec4 edges_bottom = Unpack(LoadEdge(pixel + ivec2(0, 1)));
    const vec4 edges_top = Unpack(LoadEdge(pixel + ivec2(0, -1)));

    {  // the simple shapes
      const vec4 blend = SimpleShapeBlend(edges, edges_left, edges_right, edges_top, edges_bottom);
      vec3 color = Color(pixel) * (1.0 - dot(blend, vec4(1.0)));
      if (blend.x > 0.0) color += blend.x * Color(pixel + ivec2(-1, 0));
      if (blend.y > 0.0) color += blend.y * Color(pixel + ivec2(0, -1));
      if (blend.z > 0.0) color += blend.z * Color(pixel + ivec2(1, 0));
      if (blend.w > 0.0) color += blend.w * Color(pixel + ivec2(0, 1));
      StoreColor(pixel, color, false);
    }
    {  // the complex ones: a Z, lying or standing
      float inverted_score, normal_score, best;
      bool horizontal = true, inverted = false;
      DetectZs(edges, edges_left, edges_right, Unpack(LoadEdge(pixel + ivec2(2, 0))), inverted_score, normal_score);
      best = max(inverted_score, normal_score);
      if (best > 0.0) inverted = inverted_score > normal_score;
      // The same turned a quarter turn counter-clockwise: left is bottom, top is left, right is top, bottom right.
      DetectZs(edges.argb, edges_bottom.argb, edges_top.argb, Unpack(LoadEdge(pixel + ivec2(0, -2))).argb,
               inverted_score, normal_score);
      const float vertical = max(inverted_score, normal_score);
      if (vertical > best) {
        best = vertical;
        horizontal = false;
        inverted = inverted_score > normal_score;
      }
      if (best > 0.0) {
        const float quality = floor(clamp(4.0 - best, 0.0, 3.0));  // 0 the best, 2 and 3 dubious
        const ivec2 step_right = horizontal ? ivec2(1, 0) : ivec2(0, -1);
        float left, right;
        LineLengths(left, right, pixel, horizontal, inverted, step_right);
        left -= quality;
        right -= quality;
        if (left + right >= 5.0) {
          if (!CollectBlendZs(pixel, horizontal, inverted, quality, left, right))
            BlendZs(pixel, horizontal, inverted, quality, left, right, step_right);
        }
      }
    }
  }

  memoryBarrierShared();
  barrier();

  // The collected items, spread over the group's threads.
  const uint total = min(kSharedItems, s_item_count);
  const uint loops = (total + 127u - gl_LocalInvocationID.x) / 128u;
  for (uint loop = 0u; loop < loops; ++loop) {
    const uvec2 item = s_items[loop * 128u + gl_LocalInvocationID.x];
    const ivec2 start = ivec2(int(item.x >> 18), int(item.x & 0x3FFFu));
    const bool horizontal = ((item.y >> 31) & 1u) != 0u;
    const bool inverted = ((item.y >> 30) & 1u) != 0u;
    const int step_index = int((item.y >> 20) & 0x3FFu) - 256;
    const int src_offset = int((item.y >> 10) & 0x3FFu) - 256;
    const float k = float(item.y & 0x3FFu) / 1023.0;
    const ivec2 step_right = horizontal ? ivec2(1, 0) : ivec2(0, -1);
    ivec2 blend_dir = horizontal ? ivec2(0, -1) : ivec2(-1, 0);
    if (inverted) blend_dir = -blend_dir;
    const ivec2 p = start + step_right * step_index;
    StoreColor(p, mix(Color(p), Color(p + blend_dir * src_offset), k), true);
  }
}
)";

// DeferredColorApply2x2CS: each listed quad's four pixels, each the weighted mean of its items.
constexpr const char* kCmaaApplyCS = R"(
layout(local_size_x = 4, local_size_y = 32) in;
layout(set = 0, binding = 2, r32ui) uniform uimage2D g_heads;
layout(std430, set = 0, binding = 3) buffer CmaaItems { uvec2 g_items[]; };
layout(std430, set = 0, binding = 4) buffer CmaaLocations { uint g_locations[]; };
layout(set = 0, binding = 5, rgba16f) uniform image2D g_resolved;

vec3 UnpackColor(uint rgb) {
  return vec3(unpackHalf2x16((rgb << 3) & 0x3FF8u).x, unpackHalf2x16((rgb >> 8) & 0x3FF8u).x,
              unpackHalf2x16((rgb >> 18) & 0x3FF0u).x);
}

void main() {
  const uint candidate = gl_GlobalInvocationID.y;
  const uint offset_xy = gl_LocalInvocationID.x;
  if (candidate >= g_control[3]) return;
  const uint id = g_locations[candidate];
  const ivec2 quad = ivec2(int(id >> 16), int(id & 0xFFFFu));
  const ivec2 pixel = quad * 2 + ivec2(int(offset_xy & 1u), int(offset_xy >> 1));
  if (any(greaterThanEqual(quad, imageSize(g_heads))) || any(greaterThanEqual(pixel, c_size))) return;
  uint next = imageLoad(g_heads, quad).r;
  vec4 sum = vec4(0.0);
  for (uint i = 0u; next != 0xFFFFFFFFu && i < 32u; ++i) {
    const uint item_offset = (next >> 30) & 3u;
    const bool complex_shape = ((next >> 26) & 1u) != 0u;
    const uint index = next & ((1u << 26) - 1u);
    if (index >= c_items_max) break;
    const uvec2 item = g_items[index];
    next = item.x;
    if (item_offset == offset_xy) {
      const float weight = 0.8 + (complex_shape ? 1.0 : 0.0);
      sum += vec4(UnpackColor(item.y) * weight, weight);
    }
  }
  if (sum.a == 0.0) return;
  imageStore(g_resolved, pixel, vec4(sum.rgb / sum.a, 1.0));
}
)";

// The full-screen pass that lays the changed pixels over the image (it keeps the image's alpha).
constexpr const char* kCmaaOutputPS = R"(
layout(set = 0, binding = 1) uniform sampler2D colorTex;
layout(set = 0, binding = 2) uniform sampler2D resolvedTex;
layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 out_color;
void main() {
  const ivec2 p = ivec2(gl_FragCoord.xy);
  const vec4 color = texelFetch(colorTex, p, 0);
  const vec4 resolved = texelFetch(resolvedTex, p, 0);
  out_color = vec4(resolved.a > 0.5 ? resolved.rgb : color.rgb, color.a);
}
)";

}  // namespace native_x_renderer
