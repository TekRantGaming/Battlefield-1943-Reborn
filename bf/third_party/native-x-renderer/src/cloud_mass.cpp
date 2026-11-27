// NativeXRenderer - the clouds' mass as particles (cloud_mass.h; D232).

#include "cloud_mass.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include <rex/logging.h>

#include "glsl.h"

using namespace Diligent;

namespace native_x_renderer {
namespace {

struct Constants {
  float now[4];    // the grid's place: its first cell's corner (x, z), its size (m), the step's dt
  float last[4];   // its place at the last step (x, z), the step's number, how much of this step's count the mass takes
  float wind[4];   // the clouds' wind (x, z; m/s), how many tracers, the count a cell holds on average (times 256)
  float map[4];    // the weather map: its drift (x, y; the clouds' units), cloud units a metre, 1 / its size
  float storm[4];  // a storm: its centre (x, z), its rain shield's radius, its strength
  float grid[4];   // the cells each way, the match's clock (s), a square's size (m), squares each way
  float eye[4];    // the storm's eye's radius
};

constexpr const char* kCommon = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform CloudMassConstants {
  vec4 c_now, c_last, c_wind, c_map, c_storm, c_grid, c_eye;
};
layout(std430, set = 0, binding = 1) buffer CloudMassTracers { vec4 g_tr[]; };  // x, z, age (under 0: none), life
layout(std430, set = 0, binding = 2) buffer CloudMassCount { uint g_cnt[]; };
)GLSL";

constexpr const char* kClearCS = R"GLSL(
layout(local_size_x = 64) in;
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i < uint(c_grid.x * c_grid.x)) g_cnt[i] = 0u;
}
)GLSL";

// Each tracer where it is now, and counted into its cell. The same in every player's game (the user, 21 Nov 2026: "Can
// the cloud and fog data be [streamed] to the client, so the client's clouds and fog are in sync?": not the data, what
// it is made from): a tracer keeps no state. It belongs to a square of the world (1.5 km; 512 of them a square) and
// to a turn of its own life (4 to 8 minutes, begun at its own moment of the match's clock); where and whether it was
// born in this turn is a hash of its square, its number and its turn, and the sky's weather map there at that time;
// from there it has gone with the clouds' wind for its age, and round and into the storm by its age (the storm as it
// is now). So two games with the same clock, wind and storm work out the same tracers, whenever they joined.
constexpr const char* kMoveCS = R"GLSL(
layout(local_size_x = 64) in;
layout(set = 0, binding = 3) uniform sampler2D g_weather;
uint mixed(uint s) {
  s = s * 747796405u + 2891336453u;
  uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
  return (w >> 22u) ^ w;
}
float rnd(inout uint s) {
  s = mixed(s);
  return float(s) / 4294967296.0;
}
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= uint(c_wind.z)) return;
  float tile = c_grid.z, now = c_grid.y;
  uint across = uint(c_grid.w), tiles = across * across;
  uint slot = i % tiles, k = i / tiles;
  ivec2 square = ivec2(floor(c_now.xy / tile + 0.5)) + ivec2(int(slot % across), int(slot / across));
  // Its own life and the moment it begins at, the same in every turn.
  uint own = mixed(uint(square.x) * 73856093u ^ uint(square.y) * 19349663u ^ k * 83492791u);
  uint s = own;
  float life = 240.0 + 240.0 * rnd(s);
  float at = now + rnd(s) * life;
  float turn = floor(at / life), age = at - turn * life;
  // This turn's birth.
  // (Its own numbers, apart from the life's and the moment's above: with the same ones, where it was born went by
  // when, and each square's tracers showed on one side of it: stripes, run cm_dbg.)
  s = mixed(mixed(own ^ 0xA511E9B3u) + (uint(int(turn)) + 1u) * 2654435761u);
  vec2 born = (vec2(square) + vec2(rnd(s), rnd(s))) * tile;
  // (Born anywhere, evenly: at rest the mass is even everywhere, and the sky is the sky as it was; only what carries
  // them, a storm, gathers and thins it. Born by the weather map's cloud, as at first, the grid had to stand in for
  // the map and could not: thin flat scraps and cones, the user, 21 Nov 2026: "The clouds don't look anything like
  // clouds yet".)
  // (A little more where the sky's weather map has cloud, at the time: its banks are heavier, and a storm that draws
  // them in is made of them, sheared into its bands. On average 55 in a hundred are born.)
  float cover = textureLod(g_weather, (born * c_map.z + c_map.xy + c_wind.xy * c_map.z * age) * c_map.w, 0.0).r;
  if (rnd(s) >= clamp(0.55 + 0.9 * (cover - 0.5), 0.1, 1.0)) return;
  float unused = 0.0;
  // Carried: the wind for its age; round and into a storm's low (fastest at its rain shield's radius).
  vec2 x = born + c_wind.xy * age + unused;
  if (c_storm.w > 0.01) {
    // The storm made of the clouds (the user, 21 Nov 2026: "Can we have the storm just build FROM the clouds, instead
    // of it being it's own thing?"): its low draws the air in from kilometres round, the faster the nearer (its speed
    // inward and round both as the rain shield's radius over the distance: a vortex), as far in as the eye's wall,
    // where it piles up (the wall) and none goes further (the eye, clear). Worked out for the tracer's age at once:
    // with that speed the distance squared falls evenly, and the turn is a logarithm of how far it has come in; at
    // the wall it goes on round.
    vec2 r = x - c_storm.xy;
    float d0 = max(length(r), 1.0), R = max(c_storm.z, 1.0);
    float wall = max(1.25 * c_eye.x, 0.12 * R);
    float inward = 7.0 * c_storm.w, round_speed = 26.0 * c_storm.w;  // m/s at the rain shield's radius
    // (Each comes in as far as a distance of its own between the wall and the rain shield's edge, more of them far
    // in: the storm's cloud fills its rain shield, thickest at the wall. All of them to the wall made one dense dot
    // 460 m across: run cm_storm3.)
    float rest = mix(wall, 0.95 * R, pow(rnd(s), 1.4));
    float d2 = d0 * d0 - 2.0 * inward * R * age;
    float d = sqrt(max(d2, rest * rest));
    float angle = 0.0;
    if (d0 > rest) {
      angle = round_speed / (2.0 * max(inward, 1e-3)) * log(d0 * d0 / (d * d));
      // (There since: on round it.)
      if (d2 < rest * rest) angle += round_speed * R / (rest * rest) * (age - (d0 * d0 - rest * rest) / (2.0 * inward * R));
    } else if (d0 > wall) {
      d = d0;  // (born nearer than where it would rest: it stays, going round)
      angle = round_speed * R / (d0 * d0) * age;
    } else {
      d = d0;  // (born inside the wall: it stays, thinly: the eye is not swept clean at once)
      angle = round_speed * R / (d0 * d0) * age;
    }
    float cs = cos(angle), sn = sin(angle);
    r = vec2(r.x * cs - r.y * sn, r.x * sn + r.y * cs) * (d / d0);
    x = c_storm.xy + r;
    if (d0 <= wall && rnd(s) < 0.85 * c_storm.w) return;  // (few are born in the eye)
  }
  vec2 rel = (x - c_now.xy) / c_now.z;
  if (any(lessThan(rel, vec2(0.0))) || any(greaterThanEqual(rel, vec2(1.0)))) return;
  float shows = smoothstep(0.0, 25.0, age) * (1.0 - smoothstep(life - 40.0, life, age));
  ivec2 cell = ivec2(rel * c_grid.x);
  atomicAdd(g_cnt[uint(cell.x) + uint(cell.y) * uint(c_grid.x)], uint(shows * 256.0 + 0.5));
}
)GLSL";

// The counts blurred (five cells each way, as the plume's grid is) into how much cloud is where, eased over time.
constexpr const char* kResolveCS = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 3) uniform sampler2D g_prev;
layout(set = 0, binding = 4, r16f) uniform writeonly image2D g_out;
void main() {
  int n = int(c_grid.x);
  ivec2 t = ivec2(gl_GlobalInvocationID.xy);
  if (t.x >= n || t.y >= n) return;
  // (Nine cells each way: about a hundred tracers a place, so their own scatter is a twentieth of the mass.)
  const float k[9] = float[9](1.0, 8.0, 28.0, 56.0, 70.0, 56.0, 28.0, 8.0, 1.0);
  float sum = 0.0;
  for (int dy = -4; dy <= 4; ++dy)
    for (int dx = -4; dx <= 4; ++dx) {
      ivec2 q = clamp(t + ivec2(dx, dy), ivec2(0), ivec2(n - 1));
      sum += float(g_cnt[uint(q.x) + uint(q.y) * uint(n)]) * k[dx + 4] * k[dy + 4];
    }
  sum /= 65536.0;
  float mass = clamp(sum / max(c_wind.w, 1.0) * 0.5, 0.0, 1.0);
  vec2 x = c_now.xy + (vec2(t) + 0.5) * c_now.z / float(n);
  vec2 uv = (x - c_last.xy) / c_now.z;
  bool known = c_last.z > 1.5 && all(greaterThan(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)));
  float was = known ? textureLod(g_prev, uv, 0.0).r : mass;
  if (isnan(was)) was = mass;
  imageStore(g_out, t, vec4(mix(was, mass, c_last.w)));
}
)GLSL";

}  // namespace

int TestCloudMassShaders() {
  return int(CompileGlsl(GlslStage::kCompute, std::string(kCommon) + kClearCS, "cloud mass clear CS").empty()) +
         int(CompileGlsl(GlslStage::kCompute, std::string(kCommon) + kMoveCS, "cloud mass move CS").empty()) +
         int(CompileGlsl(GlslStage::kCompute, std::string(kCommon) + kResolveCS, "cloud mass resolve CS").empty());
}

bool CloudMass::Initialize(IRenderDevice* device) {
  if (ready_) return true;
  BufferDesc cb;
  cb.Name = "NativeXRenderer cloud mass constants";
  cb.Size = sizeof(Constants);
  cb.Usage = USAGE_DYNAMIC;
  cb.BindFlags = BIND_UNIFORM_BUFFER;
  cb.CPUAccessFlags = CPU_ACCESS_WRITE;
  device->CreateBuffer(cb, nullptr, &constants_);
  auto buffer = [&](const char* name, uint64_t count, uint32_t stride, const void* init, RefCntAutoPtr<IBuffer>& out) {
    BufferDesc d;
    d.Name = name;
    d.Size = count * stride;
    d.Usage = USAGE_DEFAULT;
    d.BindFlags = BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE;
    d.Mode = BUFFER_MODE_STRUCTURED;
    d.ElementByteStride = stride;
    BufferData data(init, d.Size);
    device->CreateBuffer(d, &data, &out);
  };
  {
    std::vector<float> none(size_t(kTracers) * 4, 0.0f);
    for (uint32_t i = 0; i < kTracers; ++i) none[size_t(i) * 4 + 2] = -1.0f, none[size_t(i) * 4 + 3] = 300.0f;
    buffer("NativeXRenderer cloud mass tracers", kTracers, 16, none.data(), tracers_);
    const std::vector<uint32_t> zero(size_t(kCells) * kCells, 0u);
    buffer("NativeXRenderer cloud mass counts", uint64_t(kCells) * kCells, 4, zero.data(), count_);
  }
  {
    const std::vector<uint16_t> zero(size_t(kCells) * kCells, 0);
    for (int k = 0; k < 2; ++k) {
      TextureDesc desc;
      const std::string name = "NativeXRenderer cloud mass " + std::to_string(k);
      desc.Name = name.c_str();
      desc.Type = RESOURCE_DIM_TEX_2D;
      desc.Width = desc.Height = kCells;
      desc.Format = TEX_FORMAT_R16_FLOAT;
      desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
      TextureSubResData sub{zero.data(), uint64_t(kCells) * 2};
      TextureData data(&sub, 1);
      device->CreateTexture(desc, &data, &mass_[k]);
    }
  }
  if (!constants_ || !tracers_ || !count_ || !mass_[0] || !mass_[1]) return false;
  SamplerDesc repeat, clamp;
  repeat.MinFilter = repeat.MagFilter = repeat.MipFilter = FILTER_TYPE_LINEAR;
  repeat.AddressU = repeat.AddressV = repeat.AddressW = TEXTURE_ADDRESS_WRAP;
  clamp.MinFilter = clamp.MagFilter = clamp.MipFilter = FILTER_TYPE_LINEAR;
  clamp.AddressU = clamp.AddressV = clamp.AddressW = TEXTURE_ADDRESS_CLAMP;
  auto pass = [&](const char* body, const char* name, std::initializer_list<const char*> dynamic,
                  std::initializer_list<ImmutableSamplerDesc> sampled, Pass& out) {
    const std::vector<uint32_t> code = CompileGlsl(GlslStage::kCompute, std::string(kCommon) + body, name);
    if (code.empty()) return false;
    RefCntAutoPtr<IShader> cs;
    ShaderCreateInfo sci;
    sci.Desc.Name = name;
    sci.Desc.ShaderType = SHADER_TYPE_COMPUTE;
    sci.Desc.UseCombinedTextureSamplers = true;
    sci.ByteCode = code.data();
    sci.ByteCodeSize = code.size() * sizeof(uint32_t);
    device->CreateShader(sci, &cs);
    if (!cs) return false;
    ComputePipelineStateCreateInfo ci;
    ci.PSODesc.Name = name;
    ci.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
    ci.pCS = cs;
    // (The storage buffers as plain ones, not Vulkan's dynamic ones: fluid.cpp's note on the AMD driver's limit.)
    std::vector<ShaderResourceVariableDesc> vars = {
        {SHADER_TYPE_COMPUTE, "CloudMassTracers", SHADER_RESOURCE_VARIABLE_TYPE_STATIC, SHADER_VARIABLE_FLAG_NO_DYNAMIC_BUFFERS},
        {SHADER_TYPE_COMPUTE, "CloudMassCount", SHADER_RESOURCE_VARIABLE_TYPE_STATIC, SHADER_VARIABLE_FLAG_NO_DYNAMIC_BUFFERS}};
    for (const char* v : dynamic) vars.push_back({SHADER_TYPE_COMPUTE, v, SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC});
    const std::vector<ImmutableSamplerDesc> smp(sampled);
    ci.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    ci.PSODesc.ResourceLayout.Variables = vars.data();
    ci.PSODesc.ResourceLayout.NumVariables = uint32_t(vars.size());
    ci.PSODesc.ResourceLayout.ImmutableSamplers = smp.empty() ? nullptr : smp.data();
    ci.PSODesc.ResourceLayout.NumImmutableSamplers = uint32_t(smp.size());
    device->CreateComputePipelineState(ci, &out.pso);
    if (!out.pso) return false;
    if (auto* v = out.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "CloudMassConstants")) v->Set(constants_);
    if (auto* v = out.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "CloudMassTracers"))
      v->Set(tracers_->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    if (auto* v = out.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "CloudMassCount"))
      v->Set(count_->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    out.pso->CreateShaderResourceBinding(&out.srb, true);
    return out.srb != nullptr;
  };
  if (!pass(kClearCS, "NativeXRenderer cloud mass clear", {}, {}, clear_) ||
      !pass(kMoveCS, "NativeXRenderer cloud mass move", {"g_weather"}, {{SHADER_TYPE_COMPUTE, "g_weather", repeat}}, move_) ||
      !pass(kResolveCS, "NativeXRenderer cloud mass resolve", {"g_prev", "g_out"}, {{SHADER_TYPE_COMPUTE, "g_prev", clamp}},
            resolve_))
    return false;
  ready_ = true;
  REXLOG_INFO("[native_x_renderer] cloud mass: {} tracers over {:.0f} km, a grid of {} x {} cells (D232)", kTracers,
              kSize / 1000.0f, kCells, kCells);
  return true;
}

void CloudMass::Advance(IDeviceContext* context, const Step& step) {
  if (!ready_ || !context || !step.weather) return;
  const float cell = kSize / float(kCells);
  float origin[2];
  // (By whole squares of the tracers' own: a multiple of the grid's cells.)
  constexpr float kSquare = 1500.0f;
  constexpr uint32_t kSquares = uint32_t(kSize / kSquare);
  for (int c = 0; c < 2; ++c) origin[c] = std::floor((step.eye[c] - kSize * 0.5f) / kSquare + 0.5f) * kSquare;
  (void)cell;
  if (!placed_) origin_[0] = origin[0], origin_[1] = origin[1], placed_ = true;
  ++steps_;
  Constants k{};
  k.now[0] = origin[0], k.now[1] = origin[1], k.now[2] = kSize, k.now[3] = std::clamp(step.dt, 0.001f, 0.25f);
  k.last[0] = origin_[0], k.last[1] = origin_[1], k.last[2] = float(steps_), k.last[3] = 0.2f;
  k.wind[0] = step.wind[0], k.wind[1] = step.wind[1], k.wind[2] = float(kTracers);
  // (Nine in ten of them show at any time, the rest fading in or out: the count a cell holds at rest, which is a
  // half as the mass is read.)
  k.wind[3] = 256.0f * 0.91f * 0.55f * float(kTracers) / float(kCells * kCells);  // (55 in a hundred are born)
  k.map[0] = step.drift[0], k.map[1] = step.drift[1], k.map[2] = 1.0f / std::max(step.scale, 1e-3f), k.map[3] = 1.0f / 40000.0f;
  std::memcpy(k.storm, step.storm, sizeof(k.storm));
  k.eye[0] = step.storm_eye;
  k.grid[0] = float(kCells), k.grid[1] = float(std::fmod(step.seconds, 86400.0)), k.grid[2] = kSquare, k.grid[3] = float(kSquares);
  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, &k, sizeof(k));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }
  auto barrier = [&] {
    StateTransitionDesc b[] = {
        StateTransitionDesc(tracers_, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS, STATE_TRANSITION_FLAG_UPDATE_STATE),
        StateTransitionDesc(count_, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS, STATE_TRANSITION_FLAG_UPDATE_STATE)};
    context->TransitionResourceStates(2, b);
  };
  context->SetPipelineState(clear_.pso);
  context->CommitShaderResources(clear_.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->DispatchCompute(DispatchComputeAttribs((kCells * kCells + 63) / 64, 1, 1));
  barrier();
  if (auto* v = move_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_weather")) v->Set(step.weather);
  context->SetPipelineState(move_.pso);
  context->CommitShaderResources(move_.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->DispatchCompute(DispatchComputeAttribs((kTracers + 63) / 64, 1, 1));
  barrier();
  const int from = index_, to = index_ ^ 1;
  if (auto* v = resolve_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_prev"))
    v->Set(mass_[from]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  if (auto* v = resolve_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_out"))
    v->Set(mass_[to]->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
  context->SetPipelineState(resolve_.pso);
  context->CommitShaderResources(resolve_.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->DispatchCompute(DispatchComputeAttribs((kCells + 7) / 8, (kCells + 7) / 8, 1));
  index_ = to;
  origin_[0] = origin[0], origin_[1] = origin[1];
  view_ = mass_[to]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
}

}  // namespace native_x_renderer
