// NativeXRenderer - the ground fog pushed about (ground_fog.h; D231).

#include "ground_fog.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <rex/logging.h>

#include "glsl.h"

using namespace Diligent;

namespace native_x_renderer {
namespace {

struct Constants {
  float now[4];    // the field's place now: its first texel's corner (x, z), its size (m), the step's dt
  float last[4];   // its place at the last step (x, z), the world's time, how long the fog takes to come back (s)
  float wind[4];   // the wind (x, z; m/s), the gusts' strength (m/s), the rain (0 to 1)
  float counts[4]; // how many pushers
  float push[GroundFog::kMaxPushers * 2][4];  // each: (x, z, radius, strength), (its motion or its end x, z; -, kind); a shot: (x, z, its height, strength), (its end x, z, its end's height, kind)
  float tscale[4], toffset[4], tuv[4];  // the terrain (grass.h's TerrainFrame)
  float terrain[4];                     // its heightmap's size, how its texels are read, 1 with one
};

constexpr const char* kStepCS = R"GLSL(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(std140, set = 0, binding = 0) uniform GroundFogConstants {
  vec4 g_now, g_last, g_wind, g_counts;
  vec4 g_push[64];
  vec4 g_tscale, g_toffset, g_tuv, g_terrain;
};
layout(set = 0, binding = 1) uniform sampler2D g_in;
layout(set = 0, binding = 2, rgba16f) uniform writeonly image2D g_out;
layout(set = 0, binding = 3) uniform sampler2D g_solid;  // what stands in the fog, 0 to 1 (the field's place now)
layout(set = 0, binding = 4) uniform sampler2DArray g_height;  // the terrain's heightmap (as grass.cpp reads it)
float TerrainTexel(ivec2 p) {
  int n = int(g_terrain.x);
  p = clamp(p, ivec2(0), ivec2(n - 1));
  float h = texelFetch(g_height, ivec3(p, 0), 0).r;
  if (g_terrain.y == 1.0) return float(packHalf2x16(vec2(h, 0.0)) & 0xFFFFu) / 65535.0;
  if (g_terrain.y == 2.0) {
    float b = round(h * 32767.0);
    return (b < 0.0 ? b + 65536.0 : b) / 65535.0;
  }
  return h;
}
float TerrainHeight(vec2 xz) {
  vec2 g = (xz - g_toffset.xz) / g_tscale.xz;
  vec2 t = (g * g_tuv.xy + g_tuv.zw) * g_terrain.x - 0.5;
  ivec2 i = ivec2(floor(t));
  vec2 f = t - vec2(i);
  float a = mix(TerrainTexel(i), TerrainTexel(i + ivec2(1, 0)), f.x);
  float b = mix(TerrainTexel(i + ivec2(0, 1)), TerrainTexel(i + ivec2(1, 1)), f.x);
  return mix(a, b, f.y) * g_tscale.y + g_toffset.y;
}
float solid_at(vec2 world) { return textureLod(g_solid, (world - g_now.xy) / g_now.z, 0.0).r; }
// A motion with its part into what stands there taken off: the air goes round it, not through.
vec2 around(vec2 v, vec2 x, float cell) {
  vec2 g = vec2(solid_at(x + vec2(cell, 0.0)) - solid_at(x - vec2(cell, 0.0)),
                solid_at(x + vec2(0.0, cell)) - solid_at(x - vec2(0.0, cell)));
  float gl = length(g);
  if (gl < 0.04) return v;
  vec2 n = g / gl;
  return v - n * max(dot(v, n), 0.0) * min(gl * 2.5, 1.0);
}
float h21(vec2 p) {
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}
float vnoise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(h21(i), h21(i + vec2(1.0, 0.0)), f.x), mix(h21(i + vec2(0.0, 1.0)), h21(i + vec2(1.0, 1.0)), f.x), f.y);
}
// The field at a place in the world as the last step left it: at rest outside it.
vec3 last_at(vec2 world) {
  vec2 uv = (world - g_last.xy) / g_now.z;
  if (any(lessThan(uv, vec2(0.004))) || any(greaterThan(uv, vec2(0.996)))) return vec3(0.0, 0.0, 1.0);
  vec3 s = textureLod(g_in, uv, 0.0).rgb;
  return any(isnan(s)) || any(isinf(s)) ? vec3(0.0, 0.0, 1.0) : s;
}
void main() {
  ivec2 dims = imageSize(g_out);
  ivec2 t = ivec2(gl_GlobalInvocationID.xy);
  if (t.x >= dims.x || t.y >= dims.y) return;
  float cell = g_now.z / float(dims.x), dt = g_now.w, time = g_last.z;
  vec2 x = g_now.xy + (vec2(t) + 0.5) * cell;
  // Carried: what is here now was a step back along the air's motion and the wind.
  vec2 back = x - around(last_at(x).xy + g_wind.xy, x, cell) * dt;
  vec3 s = last_at(back);
  vec2 v = s.xy;
  float d = s.z;
  // Thinner where the air spreads, thicker where it gathers.
  float spread = (last_at(back + vec2(cell, 0.0)).x - last_at(back - vec2(cell, 0.0)).x +
                  last_at(back + vec2(0.0, cell)).y - last_at(back - vec2(0.0, cell)).y) / (2.0 * cell);
  d *= exp(-clamp(spread, -4.0, 4.0) * dt);
  // The wind's gusts: slow eddies that drift with it (a noise's curl, which turns the fog, and some of its slope,
  // which gathers and thins it: the wind is seen in the fog).
  if (g_wind.z > 0.0) {
    vec2 gp = (x - g_wind.xy * time * 0.6) * 0.035 + vec2(0.0, time * 0.05);
    const float e = 0.25;
    float n0 = vnoise(gp);
    vec2 slope = vec2(vnoise(gp + vec2(e, 0.0)) - n0, vnoise(gp + vec2(0.0, e)) - n0) / e;
    v += (vec2(slope.y, -slope.x) + 0.6 * slope) * g_wind.z * dt;
  }
  // What pushes it.
  float ground_here = g_terrain.z > 0.5 ? TerrainHeight(x) : -1e9;
  int count = int(g_counts.x + 0.5);
  for (int i = 0; i < count; ++i) {
    vec4 a = g_push[2 * i], b = g_push[2 * i + 1];
    int kind = int(b.w + 0.5);
    float radius = max(a.z, 0.1);
    if (kind == 2) {
      // A shot: a hole where the bullet goes through the fog, not along its whole flight (the user, 21 Nov 2026:
      // "like Moses parting the sea, not localized holes in the fog from the bullet spread"): where its line, at
      // this place, is within about half a metre of the ground under it (where it strikes), the fog is thrown out
      // from it, half a metre round. Without the terrain's heights: nothing.
      vec2 ab = b.xy - a.xy;
      float along = clamp(dot(x - a.xy, ab) / max(dot(ab, ab), 1e-4), 0.0, 1.0);
      vec2 off = x - (a.xy + ab * along);
      float l = length(off);
      float over_ground = mix(a.z, b.z, along) - ground_here;
      float in_fog = exp(-(over_ground - 0.2) * (over_ground - 0.2) / 0.5);
      float w = exp(-l * l / 0.55) * in_fog;
      v += (l > 1e-3 ? off / l : vec2(0.0)) * a.w * w;
      d *= 1.0 - 0.9 * w;
      continue;
    }
    vec2 r = x - a.xy;
    float l = length(r);
    vec2 out_way = l > 1e-3 ? r / l : vec2(0.0);
    float w = exp(-l * l / (radius * radius));
    if (kind == 1) {  // a blast: the air thrown outward, the fog at its middle blown away
      v += out_way * a.w * w;
      d *= 1.0 - 0.9 * exp(-l * l / (radius * radius * 0.2));
    } else {  // something moving through: the air there goes with it and is pushed aside
      float speed = length(b.xy);
      v = mix(v, b.xy * 0.7 + out_way * speed * 0.5, min(w * a.w, 1.0));
      d *= 1.0 - min(0.25 * speed, 0.8) * w * dt * 30.0 * 0.4;  // what goes through it parts it
    }
  }
  // The motion dies down; the fog comes back to its rest (the rain dimples it).
  v *= exp(-dt / 2.5);
  float fast = length(v);
  if (fast > 40.0) v *= 40.0 / fast;
  float rest = 1.0 - 0.3 * g_wind.w * vnoise(x * 0.9 + time * vec2(2.3, 1.7));
  d += (rest - d) * (1.0 - exp(-dt / max(g_last.w, 0.1)));
  // Round what stands there, and none inside it.
  float solid = clamp(solid_at(x), 0.0, 1.0);
  v = around(v, x, cell) * (1.0 - solid);
  d *= 1.0 - solid;
  imageStore(g_out, t, vec4(v, clamp(d, 0.0, 3.0), 1.0));
}
)GLSL";

// What stands in the fog, carried to the field's new place: marked where this frame's view saw something stand,
// cleared where it saw the bare ground and nothing on it, else faded (a mark lasts about a minute unseen). The
// frame's marks are cleared for the next.
constexpr const char* kCarryCS = R"GLSL(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(std140, set = 0, binding = 0) uniform GroundFogConstants {
  vec4 g_now, g_last, g_wind, g_counts;
  vec4 g_push[64];
  vec4 g_tscale, g_toffset, g_tuv, g_terrain;
};
layout(set = 0, binding = 1) uniform sampler2D g_solid_in;
layout(set = 0, binding = 2, r16f) uniform writeonly image2D g_solid_out;
layout(set = 0, binding = 3, r32ui) uniform uimage2D g_seen;
void main() {
  ivec2 dims = imageSize(g_solid_out);
  ivec2 t = ivec2(gl_GlobalInvocationID.xy);
  if (t.x >= dims.x || t.y >= dims.y) return;
  vec2 x = g_now.xy + (vec2(t) + 0.5) * g_now.z / float(dims.x);
  vec2 uv = (x - g_last.xy) / g_now.z;
  float was = any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) ? 0.0 : textureLod(g_solid_in, uv, 0.0).r;
  if (isnan(was)) was = 0.0;
  uint seen = imageLoad(g_seen, t).r;
  float now = (seen & 1u) != 0u ? 1.0 : (seen & 2u) != 0u ? was * 0.6 : was * exp(-g_now.w / 45.0);
  imageStore(g_solid_out, t, vec4(now));
  imageStore(g_seen, t, uvec4(0u));
}
)GLSL";

// What stands in the fog, marked from the view: every third pixel each way, what is seen there by its height over
// the terrain (its heightmap, read as grass.cpp reads it; the sea's height where the ground is under the sea).
constexpr const char* kMarkCS = R"GLSL(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(std140, set = 0, binding = 0) uniform GroundFogView {
  vec4 m_inv0, m_inv1, m_inv2;  // the world from clip x, y and w, rows
  vec4 m_eye;                   // xyz the camera; w the stored depth's numerator
  vec4 m_to_ndc;                // the view's 0 to 1 coordinate to NDC
  vec4 m_view;                  // xy the view's size in pixels; z pixels between the places looked at
  vec4 m_field;                 // xy the field's corner in the world (x, z); z its size
  vec4 m_scale, m_offset, m_uv; // the terrain: world = (grid.x, height, grid.y) * scale + offset; uv = grid * uv.xy + uv.zw
  vec4 m_terrain;               // x the heightmap's size, y how its texels are read (grass.cpp), z the sea's height
};
layout(set = 0, binding = 1) uniform sampler2D g_depth;
layout(set = 0, binding = 2) uniform sampler2DArray g_height;
layout(set = 0, binding = 3, r32ui) uniform uimage2D g_seen;
float Texel(ivec2 p) {
  int n = int(m_terrain.x);
  p = clamp(p, ivec2(0), ivec2(n - 1));
  float h = texelFetch(g_height, ivec3(p, 0), 0).r;
  if (m_terrain.y == 1.0) return float(packHalf2x16(vec2(h, 0.0)) & 0xFFFFu) / 65535.0;
  if (m_terrain.y == 2.0) {
    float b = round(h * 32767.0);
    return (b < 0.0 ? b + 65536.0 : b) / 65535.0;
  }
  return h;
}
float Height(vec2 xz) {
  vec2 g = (xz - m_offset.xz) / m_scale.xz;
  vec2 t = (g * m_uv.xy + m_uv.zw) * m_terrain.x - 0.5;
  ivec2 i = ivec2(floor(t));
  vec2 f = t - vec2(i);
  float a = mix(Texel(i), Texel(i + ivec2(1, 0)), f.x);
  float b = mix(Texel(i + ivec2(0, 1)), Texel(i + ivec2(1, 1)), f.x);
  return max(mix(a, b, f.y) * m_scale.y + m_offset.y, m_terrain.z);
}
void main() {
  vec2 px = (vec2(gl_GlobalInvocationID.xy) + 0.5) * m_view.z;
  if (px.x >= m_view.x || px.y >= m_view.y) return;
  ivec2 q = clamp(ivec2(px), ivec2(0), ivec2(m_view.xy) - 1);
  float d = texelFetch(g_depth, q, 0).r;
  if (d <= 1e-6) return;
  vec2 ndc = px / m_view.xy * m_to_ndc.xy + m_to_ndc.zw;
  vec3 c = vec3(ndc, 1.0);
  vec3 ray = vec3(dot(m_inv0.xyz, c), dot(m_inv1.xyz, c), dot(m_inv2.xyz, c));
  float w = m_eye.w / d;
  if (w * length(ray) < 2.0) return;  // (not what is held in the hand)
  vec3 P = m_eye.xyz + ray * w;
  vec2 rel = (P.xz - m_field.xy) / m_field.z;
  if (any(lessThan(rel, vec2(0.0))) || any(greaterThanEqual(rel, vec2(1.0)))) return;
  float over = P.y - Height(P.xz);
  uint bit = over > 0.25 && over < 1.8 ? 1u : abs(over) <= 0.12 ? 2u : 0u;
  if (bit == 0u) return;
  imageAtomicOr(g_seen, ivec2(rel * vec2(imageSize(g_seen))), bit);
}
)GLSL";

struct ViewConstants {
  float inv[3][4];
  float eye[4];
  float to_ndc[4];
  float view[4];
  float field[4];
  float scale[4], offset[4], uv[4];
  float terrain[4];
};

}  // namespace

const char* GroundFog::ShaderSource() { return kStepCS; }

// NXR_SHADER_TEST (aa.cpp): the passes compiled offline.
int TestGroundFogShader() {
  return int(CompileGlsl(GlslStage::kCompute, kStepCS, "ground fog CS").empty()) +
         int(CompileGlsl(GlslStage::kCompute, kCarryCS, "ground fog solid carry CS").empty()) +
         int(CompileGlsl(GlslStage::kCompute, kMarkCS, "ground fog solid mark CS").empty());
}

bool GroundFog::Initialize(IRenderDevice* device) {
  if (ready_) return true;
  const std::vector<uint32_t> spirv = CompileGlsl(GlslStage::kCompute, kStepCS, "NativeXRenderer ground fog CS");
  if (spirv.empty()) return false;
  RefCntAutoPtr<IShader> cs;
  ShaderCreateInfo shader;
  shader.Desc.Name = "NativeXRenderer ground fog CS";
  shader.Desc.ShaderType = SHADER_TYPE_COMPUTE;
  shader.Desc.UseCombinedTextureSamplers = true;
  shader.ByteCode = spirv.data();
  shader.ByteCodeSize = spirv.size() * sizeof(uint32_t);
  device->CreateShader(shader, &cs);
  if (!cs) return false;
  BufferDesc cb;
  cb.Name = "NativeXRenderer ground fog constants";
  cb.Size = sizeof(Constants);
  cb.Usage = USAGE_DYNAMIC;
  cb.BindFlags = BIND_UNIFORM_BUFFER;
  cb.CPUAccessFlags = CPU_ACCESS_WRITE;
  device->CreateBuffer(cb, nullptr, &constants_);
  // At rest: no motion, all of the fog.
  std::vector<uint16_t> rest(size_t(kTexels) * kTexels * 4, 0);
  for (size_t i = 0; i < size_t(kTexels) * kTexels; ++i) rest[i * 4 + 2] = rest[i * 4 + 3] = 0x3C00;  // 1.0
  for (int k = 0; k < 2; ++k) {
    TextureDesc desc;
    const std::string name = "NativeXRenderer ground fog field " + std::to_string(k);
    desc.Name = name.c_str();
    desc.Type = RESOURCE_DIM_TEX_2D;
    desc.Width = desc.Height = kTexels;
    desc.Format = TEX_FORMAT_RGBA16_FLOAT;
    desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    TextureSubResData sub{rest.data(), uint64_t(kTexels) * 8};
    TextureData data(&sub, 1);
    device->CreateTexture(desc, &data, &field_[k]);
  }
  {  // (a heightmap of nothing, bound while the terrain is not known)
    TextureDesc desc;
    desc.Name = "NativeXRenderer ground fog no terrain";
    desc.Type = RESOURCE_DIM_TEX_2D_ARRAY;
    desc.Width = desc.Height = 1;
    desc.ArraySize = 1;
    desc.Format = TEX_FORMAT_R16_FLOAT;
    desc.Usage = USAGE_IMMUTABLE;
    desc.BindFlags = BIND_SHADER_RESOURCE;
    const uint16_t zero = 0;
    TextureSubResData sub{&zero, 2};
    TextureData data(&sub, 1);
    device->CreateTexture(desc, &data, &no_height_);
  }
  if (!constants_ || !field_[0] || !field_[1] || !no_height_) return false;
  ComputePipelineStateCreateInfo info;
  info.PSODesc.Name = "NativeXRenderer ground fog";
  info.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
  info.pCS = cs;
  ShaderResourceVariableDesc variables[] = {{SHADER_TYPE_COMPUTE, "g_in", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
                                            {SHADER_TYPE_COMPUTE, "g_out", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
                                            {SHADER_TYPE_COMPUTE, "g_solid", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
                                            {SHADER_TYPE_COMPUTE, "g_height", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC}};
  SamplerDesc linear;
  linear.MinFilter = linear.MagFilter = linear.MipFilter = FILTER_TYPE_LINEAR;
  linear.AddressU = linear.AddressV = linear.AddressW = TEXTURE_ADDRESS_CLAMP;
  SamplerDesc nearest = linear;
  nearest.MinFilter = nearest.MagFilter = nearest.MipFilter = FILTER_TYPE_POINT;
  ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_COMPUTE, "g_in", linear}, {SHADER_TYPE_COMPUTE, "g_solid", linear},
                                     {SHADER_TYPE_COMPUTE, "g_height", nearest}};
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
  info.PSODesc.ResourceLayout.Variables = variables;
  info.PSODesc.ResourceLayout.NumVariables = 4;
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
  info.PSODesc.ResourceLayout.NumImmutableSamplers = 3;
  device->CreateComputePipelineState(info, &pso_);
  if (!pso_) return false;
  if (auto* v = pso_->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "GroundFogConstants")) v->Set(constants_);
  pso_->CreateShaderResourceBinding(&srb_, true);
  if (!srb_) return false;
  // What stands in the fog: its map twice over (read one, write the other), and its two passes.
  {
    const std::vector<uint16_t> none(size_t(kTexels) * kTexels, 0);
    for (int k = 0; k < 2; ++k) {
      TextureDesc desc;
      const std::string name = "NativeXRenderer ground fog solid " + std::to_string(k);
      desc.Name = name.c_str();
      desc.Type = RESOURCE_DIM_TEX_2D;
      desc.Width = desc.Height = kTexels;
      desc.Format = TEX_FORMAT_R16_FLOAT;
      desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
      TextureSubResData sub{none.data(), uint64_t(kTexels) * 2};
      TextureData data(&sub, 1);
      device->CreateTexture(desc, &data, &solid_[k]);
    }
    BufferDesc vb;
    vb.Name = "NativeXRenderer ground fog view constants";
    vb.Size = sizeof(ViewConstants);
    vb.Usage = USAGE_DYNAMIC;
    vb.BindFlags = BIND_UNIFORM_BUFFER;
    vb.CPUAccessFlags = CPU_ACCESS_WRITE;
    device->CreateBuffer(vb, nullptr, &mark_constants_);
    if (!solid_[0] || !solid_[1] || !mark_constants_) return false;
    {
      TextureDesc desc;
      desc.Name = "NativeXRenderer ground fog solid seen";
      desc.Type = RESOURCE_DIM_TEX_2D;
      desc.Width = desc.Height = kTexels;
      desc.Format = TEX_FORMAT_R32_UINT;
      desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
      const std::vector<uint32_t> zero(size_t(kTexels) * kTexels, 0);
      TextureSubResData sub{zero.data(), uint64_t(kTexels) * 4};
      TextureData data(&sub, 1);
      device->CreateTexture(desc, &data, &seen_);
      if (!seen_) return false;
    }
    SamplerDesc point = linear;
    point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
    auto pass = [&](const char* source, const char* name, std::initializer_list<const char*> dynamic,
                    std::initializer_list<ImmutableSamplerDesc> sampled, const char* block, IBuffer* buffer,
                    RefCntAutoPtr<IPipelineState>& pso, RefCntAutoPtr<IShaderResourceBinding>& srb) {
      const std::vector<uint32_t> code = CompileGlsl(GlslStage::kCompute, source, name);
      if (code.empty()) return false;
      RefCntAutoPtr<IShader> shader_cs;
      ShaderCreateInfo sci;
      sci.Desc.Name = name;
      sci.Desc.ShaderType = SHADER_TYPE_COMPUTE;
      sci.Desc.UseCombinedTextureSamplers = true;
      sci.ByteCode = code.data();
      sci.ByteCodeSize = code.size() * sizeof(uint32_t);
      device->CreateShader(sci, &shader_cs);
      if (!shader_cs) return false;
      ComputePipelineStateCreateInfo ci;
      ci.PSODesc.Name = name;
      ci.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
      ci.pCS = shader_cs;
      std::vector<ShaderResourceVariableDesc> vars;
      for (const char* v : dynamic) vars.push_back({SHADER_TYPE_COMPUTE, v, SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC});
      const std::vector<ImmutableSamplerDesc> smp(sampled);
      ci.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
      ci.PSODesc.ResourceLayout.Variables = vars.data();
      ci.PSODesc.ResourceLayout.NumVariables = uint32_t(vars.size());
      ci.PSODesc.ResourceLayout.ImmutableSamplers = smp.data();
      ci.PSODesc.ResourceLayout.NumImmutableSamplers = uint32_t(smp.size());
      device->CreateComputePipelineState(ci, &pso);
      if (!pso) return false;
      if (auto* v = pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, block)) v->Set(buffer);
      pso->CreateShaderResourceBinding(&srb, true);
      return srb != nullptr;
    };
    if (!pass(kCarryCS, "NativeXRenderer ground fog solid carry", {"g_solid_in", "g_solid_out", "g_seen"},
              {{SHADER_TYPE_COMPUTE, "g_solid_in", linear}}, "GroundFogConstants", constants_, carry_pso_, carry_srb_) ||
        !pass(kMarkCS, "NativeXRenderer ground fog solid mark", {"g_depth", "g_height", "g_seen"},
              {{SHADER_TYPE_COMPUTE, "g_depth", point}, {SHADER_TYPE_COMPUTE, "g_height", point}}, "GroundFogView",
              mark_constants_, mark_pso_, mark_srb_))
      return false;
  }
  ready_ = true;
  REXLOG_INFO("[native_x_renderer] ground fog: the field made ({} x {} texels over {:.0f} m; D231)", kTexels, kTexels, kSize);
  return true;
}

void GroundFog::Advance(IDeviceContext* context, const Step& step, const Pusher* pushers, int count) {
  if (!ready_ || !context) return;
  // The field's place: round the eye, moved by whole texels (what it holds stays where it is in the world).
  const float cell = kSize / float(kTexels);
  float origin[2];
  for (int c = 0; c < 2; ++c) origin[c] = std::floor((step.eye[c * 2] - kSize * 0.5f) / cell) * cell;
  if (!placed_) origin_[0] = origin[0], origin_[1] = origin[1], placed_ = true;
  Constants k{};
  k.now[0] = origin[0], k.now[1] = origin[1], k.now[2] = kSize, k.now[3] = std::clamp(step.dt, 0.001f, 0.1f);
  k.last[0] = origin_[0], k.last[1] = origin_[1];
  k.last[2] = float(std::fmod(step.seconds, 3600.0)), k.last[3] = 12.0f;
  k.wind[0] = step.wind[0], k.wind[1] = step.wind[1], k.wind[2] = step.gust, k.wind[3] = std::clamp(step.rain, 0.0f, 1.0f);
  int n = 0;
  for (int i = 0; i < count && n < kMaxPushers; ++i) {
    const Pusher& p = pushers[i];
    // Not what is over the layer (a plane high up): fading out over 4 m above its top.
    float weight = 1.0f;
    if (step.layer_known) weight = 1.0f - std::clamp((p.position[1] - step.sea - step.top) / 4.0f, 0.0f, 1.0f);
    if (weight <= 0.0f) continue;
    float* a = k.push[2 * n];
    float* b = k.push[2 * n + 1];
    a[0] = p.position[0], a[1] = p.position[2], a[2] = p.radius, a[3] = p.strength * weight;
    b[0] = p.velocity[0], b[1] = p.velocity[2], b[3] = float(p.kind);
    if (p.kind == 2) a[2] = p.position[1], b[2] = p.velocity[1];  // a shot: its heights
    ++n;
  }
  k.counts[0] = float(n);
  // The terrain: as last told (a step without the view keeps it).
  if (step.terrain && step.terrain_size > 0.0f) {
    terrain_ = step.terrain;
    std::memcpy(terrain_k_[0], step.terrain_scale, 16), std::memcpy(terrain_k_[1], step.terrain_offset, 16);
    std::memcpy(terrain_k_[2], step.terrain_uv, 16);
    terrain_k_[3][0] = step.terrain_size, terrain_k_[3][1] = step.terrain_decode, terrain_k_[3][2] = 1.0f;
  }
  std::memcpy(k.tscale, terrain_k_[0], 16), std::memcpy(k.toffset, terrain_k_[1], 16);
  std::memcpy(k.tuv, terrain_k_[2], 16), std::memcpy(k.terrain, terrain_k_[3], 16);
  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, &k, sizeof(k));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }
  // What stands in the fog: marked from the view against the terrain, then carried to the field's place now.
  if (step.depth && step.terrain && step.terrain_size > 0.0f && step.width && step.height) {
    ViewConstants vc{};
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) vc.inv[r][c] = step.inverse[r][c];
    vc.eye[0] = step.eye[0], vc.eye[1] = step.eye[1], vc.eye[2] = step.eye[2], vc.eye[3] = step.depth_near;
    std::memcpy(vc.to_ndc, step.to_ndc, sizeof(vc.to_ndc));
    constexpr float kStride = 3.0f;
    vc.view[0] = float(step.width), vc.view[1] = float(step.height), vc.view[2] = kStride;
    vc.field[0] = origin[0], vc.field[1] = origin[1], vc.field[2] = kSize;
    std::memcpy(vc.scale, step.terrain_scale, sizeof(vc.scale));
    std::memcpy(vc.offset, step.terrain_offset, sizeof(vc.offset));
    std::memcpy(vc.uv, step.terrain_uv, sizeof(vc.uv));
    vc.terrain[0] = step.terrain_size, vc.terrain[1] = step.terrain_decode, vc.terrain[2] = step.sea_floor;
    {
      void* data = nullptr;
      context->MapBuffer(mark_constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
      std::memcpy(data, &vc, sizeof(vc));
      context->UnmapBuffer(mark_constants_, MAP_WRITE);
    }
    if (auto* v = mark_srb_->GetVariableByName(SHADER_TYPE_COMPUTE, "g_depth")) v->Set(step.depth);
    if (auto* v = mark_srb_->GetVariableByName(SHADER_TYPE_COMPUTE, "g_height")) v->Set(step.terrain);
    if (auto* v = mark_srb_->GetVariableByName(SHADER_TYPE_COMPUTE, "g_seen"))
      v->Set(seen_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
    context->SetPipelineState(mark_pso_);
    context->CommitShaderResources(mark_srb_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    const uint32_t gx = (uint32_t(float(step.width) / kStride) + 7) / 8, gy = (uint32_t(float(step.height) / kStride) + 7) / 8;
    context->DispatchCompute(DispatchComputeAttribs(std::max(gx, 1u), std::max(gy, 1u), 1));
    const int sfrom = solid_index_, sto = solid_index_ ^ 1;
    if (auto* v = carry_srb_->GetVariableByName(SHADER_TYPE_COMPUTE, "g_solid_in"))
      v->Set(solid_[sfrom]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    if (auto* v = carry_srb_->GetVariableByName(SHADER_TYPE_COMPUTE, "g_solid_out"))
      v->Set(solid_[sto]->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
    if (auto* v = carry_srb_->GetVariableByName(SHADER_TYPE_COMPUTE, "g_seen"))
      v->Set(seen_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
    context->SetPipelineState(carry_pso_);
    context->CommitShaderResources(carry_srb_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->DispatchCompute(DispatchComputeAttribs((kTexels + 7) / 8, (kTexels + 7) / 8, 1));
    solid_index_ = sto;
  }
  if (auto* v = srb_->GetVariableByName(SHADER_TYPE_COMPUTE, "g_height"))
    v->Set(terrain_ ? terrain_.RawPtr() : no_height_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  if (auto* v = srb_->GetVariableByName(SHADER_TYPE_COMPUTE, "g_solid"))
    v->Set(solid_[solid_index_]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  const int from = index_, to = index_ ^ 1;
  if (auto* v = srb_->GetVariableByName(SHADER_TYPE_COMPUTE, "g_in"))
    v->Set(field_[from]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  if (auto* v = srb_->GetVariableByName(SHADER_TYPE_COMPUTE, "g_out"))
    v->Set(field_[to]->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
  context->SetPipelineState(pso_);
  context->CommitShaderResources(srb_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->DispatchCompute(DispatchComputeAttribs((kTexels + 7) / 8, (kTexels + 7) / 8, 1));
  index_ = to;
  origin_[0] = origin[0], origin_[1] = origin[1];
  view_ = field_[to]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
}

}  // namespace native_x_renderer
