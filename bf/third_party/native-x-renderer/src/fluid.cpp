// NativeXRenderer - a GPU particle fluid (fluid.h; D227). Ported from threejs-particle-fluids (MIT, Daniel Greenheck
// and Bryan Amrein): its air (position based fluids, XPBD density constraints, a hash grid with a counting sort and
// neighbour lists, vorticity confinement, XSPH viscosity) and its smoke tracers, as GLSL compute passes.

#include "fluid.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <rex/logging.h>

#include "glsl.h"

using namespace Diligent;

namespace native_x_renderer {
namespace {

constexpr uint32_t kMaxNeighbours = 64;

struct Constants {
  float domain_min[4];   // w: 3 in 3D, 2 in 2D
  float domain_max[4];   // w: the step's dt
  float kernel[4];       // h, h², poly6's coefficient, the spiky gradient's
  float solve[4];        // rest density, its inverse, the relaxation, a particle's mass
  float counts[4];       // particles, tracers, the hash table's size, the cell size
  float flow[4];         // vorticity, viscosity, the tracers' life, drag
  float force0[4];       // gravity, swirl
  float force1[4];       // the centre, pull
  float force2[4];       // reach, Coriolis, the splat weight, tracers emitted this step
  float force3[4];       // the updraft's radius (sink), where its air comes back (source), the step's number, the most speed
  float pattern0[4];     // D229: the pattern's two layers, their blend, its turn
  float pattern1[4];     // its zoom, the organisation, the disc's radius, 1 with a pattern
  float pattern2[4];     // the cells' time
  float splat_min[4];    // w: where the emitted tracers start (a ring)
  float splat_size[4];
  float cloud0[4];       // D229: the flow's speed, 1 to restart phase a, b
  float cloud1[4];       // the weather map: the domain's origin in the sky (x, z), the sky's units a domain unit, 1 / its size
  float cloud2[4];       // phase b's share, the convection, its rate, 1 to reset
  float cloud3[4];       // the sky's drift (x, z)
  uint32_t splat_dims[4];  // w: emitters
  uint32_t cloud_dims[4];  // D229: the cloud field's texels (x, z), the velocity grid's cells (x, z)
  float emit[FluidSim::kMaxEmitters][4];
};

constexpr const char* kCommon = R"GLSL(
layout(std140, set = 0, binding = 0) uniform FluidConstants {
  vec4 f_domain_min, f_domain_max, f_kernel, f_solve, f_counts, f_flow, f_force0, f_force1, f_force2, f_force3,
       f_pattern0, f_pattern1, f_pattern2, f_splat_min, f_splat_size, f_cloud0, f_cloud1, f_cloud2, f_cloud3;
  uvec4 f_splat_dims;
  uvec4 f_cloud_dims;
  vec4 f_emit[16];
};
layout(std430, set = 0, binding = 1) buffer FluidPos { vec4 g_pos[]; };
layout(std430, set = 0, binding = 2) buffer FluidPred { vec4 g_pred[]; };
layout(std430, set = 0, binding = 3) buffer FluidVel { vec4 g_vel[]; };
layout(std430, set = 0, binding = 4) buffer FluidLambda { float g_lambda[]; };
layout(std430, set = 0, binding = 5) buffer FluidDelta { vec4 g_delta[]; };
layout(std430, set = 0, binding = 6) buffer FluidOmega { vec4 g_omega[]; };
layout(std430, set = 0, binding = 7) buffer FluidEta { vec4 g_eta[]; };
layout(std430, set = 0, binding = 8) buffer FluidDv { vec4 g_dv[]; };
layout(std430, set = 0, binding = 9) buffer FluidKey { uint g_key[]; };
layout(std430, set = 0, binding = 10) buffer FluidCounts { uint g_counts[]; };
layout(std430, set = 0, binding = 11) buffer FluidStarts { uint g_starts[]; };
layout(std430, set = 0, binding = 12) buffer FluidCursor { uint g_cursor[]; };
layout(std430, set = 0, binding = 13) buffer FluidSorted { uint g_sorted[]; };
layout(std430, set = 0, binding = 14) buffer FluidNbr { uint g_nbr[]; };
layout(std430, set = 0, binding = 15) buffer FluidNbrCount { uint g_nbr_count[]; };
layout(std430, set = 0, binding = 16) buffer FluidTPos { vec4 g_tpos[]; };
layout(std430, set = 0, binding = 17) buffer FluidTVel { vec4 g_tvel[]; };
layout(std430, set = 0, binding = 18) buffer FluidAccum { uint g_accum[]; };
layout(std430, set = 0, binding = 21) buffer FluidGridSum { int g_gsum[]; };
layout(std430, set = 0, binding = 22) buffer FluidGridVel { vec2 g_gvel[]; };

const uint MAXN = 64u;
uint particle_count() { return uint(f_counts.x); }
bool is3d() { return f_domain_min.w > 2.5; }
float dt() { return f_domain_max.w; }
float poly6(float r2) {
  float d = max(f_kernel.y - r2, 0.0);
  return d * d * d * f_kernel.z;
}
vec3 spiky_grad(vec3 r) {
  float l = length(r);
  float t = max(f_kernel.x - l, 0.0);
  return (l > 1e-6 ? r / l : vec3(0.0)) * (-f_kernel.w * t * t);
}
ivec3 cell_of(vec3 p) { return ivec3(floor((p - f_domain_min.xyz) / f_counts.w)); }
uint key_of(ivec3 c) {
  uint h = (uint(c.x) * 73856093u) ^ (uint(c.y) * 19349663u) ^ (uint(c.z) * 83492791u);
  return h & (uint(f_counts.z) - 1u);
}
vec3 keep_in(vec3 p) {
  p = clamp(p, f_domain_min.xyz, f_domain_max.xyz);
  if (!is3d()) p.y = f_domain_min.y;
  return p;
}
)GLSL";

// 1. Forces and the prediction: x* = x + v dt (Macklin and Mueller 2013, Algorithm 1), with the storm's drivers.
constexpr const char* kPredictCS = R"GLSL(
layout(local_size_x = 64) in;
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= particle_count()) return;
  vec3 x = g_pos[i].xyz, v = g_vel[i].xyz;
  vec2 d = x.xz - f_force1.xz;
  float r = length(d);
  // The updraft: up and out of the layer, back at the source radius at a hashed bearing, at rest.
  if (f_force3.x > 0.0 && r < f_force3.x) {
    uint n = i * 747796405u + uint(f_force3.z) * 2891336453u;
    n = ((n >> ((n >> 28u) + 4u)) ^ n) * 277803737u;
    float a = float((n >> 22u) ^ n) / 4294967295.0 * 6.2831853;
    vec3 back = vec3(f_force1.x + cos(a) * f_force3.y, x.y, f_force1.z + sin(a) * f_force3.y);
    back = keep_in(back);
    g_pos[i] = vec4(back, 0.0);
    g_pred[i] = vec4(back, 0.0);
    g_vel[i] = vec4(0.0);
    return;
  }
  vec3 a = f_force0.xyz;
  vec2 dir = r > 1e-3 ? d / r : vec2(0.0);
  float near = 1.0 - smoothstep(f_force2.x * 0.8, f_force2.x * 1.25, r);
  a.xz += -dir * f_force1.w * near;                       // toward the low
  a.xz += vec2(-dir.y, dir.x) * f_force0.w * near;        // round it
  a.xz += f_force2.y * vec2(v.z, -v.x);                   // Coriolis: the moving air turned to its right
  v += a * dt();
  v *= max(1.0 - f_flow.w * dt(), 0.0);
  if (!is3d()) v.y = 0.0;
  g_vel[i] = vec4(v, 0.0);
  g_pred[i] = vec4(keep_in(x + v * dt()), 0.0);
}
)GLSL";

constexpr const char* kClearCS = R"GLSL(
layout(local_size_x = 64) in;
void main() {
  uint k = gl_GlobalInvocationID.x;
  if (k < uint(f_counts.z)) g_counts[k] = 0u;
}
)GLSL";

// 2. The grid: each particle's cell, counted per hash bucket, the buckets' starts (one workgroup's scan), the order.
constexpr const char* kHashCS = R"GLSL(
layout(local_size_x = 64) in;
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= particle_count()) return;
  uint key = key_of(cell_of(g_pred[i].xyz));
  g_key[i] = key;
  atomicAdd(g_counts[key], 1u);
}
)GLSL";

constexpr const char* kScanCS = R"GLSL(
layout(local_size_x = 256) in;
shared uint s_sum[256];
void main() {
  uint t = gl_LocalInvocationID.x;
  uint n = uint(f_counts.z), per = (n + 255u) / 256u, b = t * per;
  uint sum = 0u;
  for (uint k = 0u; k < per; ++k)
    if (b + k < n) sum += g_counts[b + k];
  s_sum[t] = sum;
  barrier();
  for (uint off = 1u; off < 256u; off <<= 1u) {
    uint add = t >= off ? s_sum[t - off] : 0u;
    barrier();
    s_sum[t] += add;
    barrier();
  }
  uint run = s_sum[t] - sum;
  for (uint k = 0u; k < per; ++k) {
    uint idx = b + k;
    if (idx < n) {
      g_starts[idx] = run;
      g_cursor[idx] = run;
      run += g_counts[idx];
    }
  }
}
)GLSL";

constexpr const char* kScatterCS = R"GLSL(
layout(local_size_x = 64) in;
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= particle_count()) return;
  g_sorted[atomicAdd(g_cursor[g_key[i]], 1u)] = i;
}
)GLSL";

// The neighbours within h of each particle, gathered once a step (each bucket visited once: two cells may share one).
constexpr const char* kNeighboursCS = R"GLSL(
layout(local_size_x = 64) in;
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= particle_count()) return;
  vec3 x = g_pred[i].xyz;
  ivec3 c = cell_of(x);
  uint seen[27];
  uint nseen = 0u, cnt = 0u;
  int yr = is3d() ? 1 : 0;
  for (int dx = -1; dx <= 1; ++dx)
    for (int dy = -yr; dy <= yr; ++dy)
      for (int dz = -1; dz <= 1; ++dz) {
        uint key = key_of(c + ivec3(dx, dy, dz));
        bool again = false;
        for (uint s = 0u; s < nseen; ++s) again = again || seen[s] == key;
        if (again) continue;
        seen[nseen++] = key;
        uint s0 = g_starts[key], s1 = s0 + g_counts[key];
        for (uint k = s0; k < s1 && cnt < MAXN; ++k) {
          uint j = g_sorted[k];
          vec3 r = x - g_pred[j].xyz;
          if (dot(r, r) < f_kernel.y) g_nbr[i * MAXN + cnt++] = j;
        }
      }
  g_nbr_count[i] = cnt;
}
)GLSL";

// 3. The density constraint, C = rho / rho0 - 1 (only against compression, as the library's gas), XPBD's lambda.
constexpr const char* kLambdaCS = R"GLSL(
layout(local_size_x = 64) in;
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= particle_count()) return;
  vec3 x = g_pred[i].xyz;
  float m = f_solve.w, rho = 0.0, gsq = 0.0;
  vec3 gsum = vec3(0.0);
  uint n = g_nbr_count[i];
  for (uint k = 0u; k < n; ++k) {
    uint j = g_nbr[i * MAXN + k];
    vec3 r = x - g_pred[j].xyz;
    rho += m * poly6(dot(r, r));
    vec3 g = spiky_grad(r) * m;
    gsum += g;
    gsq += dot(g, g);
  }
  float c = rho * f_solve.y - 1.0;
  if (c <= 0.0) {
    g_lambda[i] = 0.0;
    return;
  }
  float norm = (dot(gsum, gsum) + gsq) * f_solve.y * f_solve.y;
  g_lambda[i] = -c / max(norm + f_solve.z, 1e-20);
}
)GLSL";

constexpr const char* kDeltaCS = R"GLSL(
layout(local_size_x = 64) in;
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= particle_count()) return;
  vec3 x = g_pred[i].xyz;
  float li = g_lambda[i];
  vec3 sum = vec3(0.0);
  uint n = g_nbr_count[i];
  for (uint k = 0u; k < n; ++k) {
    uint j = g_nbr[i * MAXN + k];
    sum += spiky_grad(x - g_pred[j].xyz) * ((li + g_lambda[j]) * f_solve.w);
  }
  g_delta[i] = vec4(sum * f_solve.y, 0.0);
}
)GLSL";

constexpr const char* kApplyCS = R"GLSL(
layout(local_size_x = 64) in;
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= particle_count()) return;
  vec3 d = g_delta[i].xyz;
  float l = length(d), most = f_kernel.x * 0.25;  // (at most a quarter of h an iteration: run fluid4's first step threw
  if (l > most) d *= most / l;                     // particles at 650 m/s from a slightly packed start)
  g_pred[i] = vec4(keep_in(g_pred[i].xyz + d), 0.0);
}
)GLSL";

// 4. Velocities from the solved positions, then vorticity confinement and XSPH viscosity.
constexpr const char* kAdvectCS = R"GLSL(
layout(local_size_x = 64) in;
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= particle_count()) return;
  vec3 x = g_pos[i].xyz, xs = g_pred[i].xyz;
  vec3 v = (xs - x) / max(dt(), 1e-6);
  float s = length(v);
  if (s > f_force3.w) v *= f_force3.w / s;
  g_vel[i] = vec4(v, 0.0);
  g_pos[i] = vec4(xs, 0.0);
}
)GLSL";

constexpr const char* kCurlCS = R"GLSL(
layout(local_size_x = 64) in;
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= particle_count()) return;
  vec3 xi = g_pos[i].xyz, vi = g_vel[i].xyz;
  float volume = f_solve.w * f_solve.y;
  vec3 curl = vec3(0.0), smoothing = vec3(0.0);
  uint n = g_nbr_count[i];
  for (uint k = 0u; k < n; ++k) {
    uint j = g_nbr[i * MAXN + k];
    vec3 r = xi - g_pos[j].xyz, vj = g_vel[j].xyz;
    curl += cross(vi - vj, spiky_grad(r)) * volume;
    smoothing += (vj - vi) * poly6(dot(r, r)) * volume;
  }
  g_omega[i] = vec4(curl, length(curl));
  g_dv[i] = vec4(smoothing * f_flow.y, 0.0);
}
)GLSL";

constexpr const char* kGradientCS = R"GLSL(
layout(local_size_x = 64) in;
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= particle_count()) return;
  vec3 xi = g_pos[i].xyz;
  float oi = g_omega[i].w, volume = f_solve.w * f_solve.y;
  vec3 eta = vec3(0.0);
  uint n = g_nbr_count[i];
  for (uint k = 0u; k < n; ++k) {
    uint j = g_nbr[i * MAXN + k];
    eta += spiky_grad(xi - g_pos[j].xyz) * ((g_omega[j].w - oi) * volume);
  }
  g_eta[i] = vec4(eta, 0.0);
}
)GLSL";

constexpr const char* kVApplyCS = R"GLSL(
layout(local_size_x = 64) in;
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= particle_count()) return;
  vec3 change = g_dv[i].xyz;
  vec3 eta = g_eta[i].xyz;
  float el = length(eta);
  if (el >= 1e-12) change += cross(eta / el, g_omega[i].xyz) * (f_flow.x * dt());
  vec3 v = g_vel[i].xyz + change;
  if (!is3d()) v.y = 0.0;
  g_vel[i] = vec4(v, 0.0);
}
)GLSL";

// 5. The tracers: new ones at the emitters (a ring of slots), the rest carried by the air's velocity where they are,
// v(x) = sum v_j W / sum W (Macklin et al. 2014, eq. 28), until their life is over.
constexpr const char* kPatternGLSL = R"GLSL(
layout(set = 0, binding = 20) uniform sampler2DArray g_pattern;
float hash11(uint n) {
  n = (n << 13u) ^ n;
  n = n * (n * n * 15731u + 789221u) + 1376312589u;
  return float(n & 0x7fffffffu) / float(0x7fffffff);
}
float hash21(vec2 p) {
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}
float vnoise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash21(i), hash21(i + vec2(1, 0)), f.x), mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), f.x), f.y);
}
// D229: the chance a new tracer is kept at q (relative to the centre): scattered convective cells blended by the
// organisation into the pattern, turned and zoomed; nothing past the disc's edge.
float emit_chance(vec2 q) {
  float radius = f_pattern1.z;
  vec2 u = q / radius;
  float len = length(u);
  float edge = 1.0 - smoothstep(0.85, 1.0, len);
  float c = cos(f_pattern0.w), sn = sin(f_pattern0.w);
  vec2 up = len > 1e-5 ? u / len * pow(len, f_pattern2.y) : vec2(0.0);  // (D229: the radius to the pattern's power)
  vec2 ur = vec2(c * up.x - sn * up.y, sn * up.x + c * up.y);
  vec2 tuv = vec2(0.5 + ur.x * 0.5 * f_pattern1.x, 0.5 - ur.y * 0.5 * f_pattern1.x);  // (row 0 the north, +z)
  float pat = mix(texture(g_pattern, vec3(tuv, f_pattern0.x)).r, texture(g_pattern, vec3(tuv, f_pattern0.y)).r,
                  f_pattern0.z);
  vec2 drift = vec2(f_pattern2.x * 0.013, -f_pattern2.x * 0.009);
  float cells = smoothstep(0.55, 0.85, vnoise(u * 5.0 + drift) * 0.65 + vnoise(u * 13.0 - drift * 1.7) * 0.35);
  cells *= 1.0 - smoothstep(0.55, 1.0, length(u));
  return mix(cells, pat, f_pattern1.y) * edge;
}
)GLSL";
constexpr const char* kTracersBody = R"GLSL(
layout(local_size_x = 64) in;
void main() {
  uint s = gl_GlobalInvocationID.x, cap = uint(f_counts.y);
  if (s >= cap) return;
  uint emit = uint(f_force2.w), base = uint(f_splat_min.w), emitters = max(f_splat_dims.w, 1u);
  uint rel = (s + cap - base) % cap;
  if (rel < emit && f_pattern1.w > 0.5) {
    // Up to eight tries at random in the disc, each kept with its chance there; none kept: no tracer this time.
    for (uint k = 0u; k < 8u; ++k) {
      uint seed = s * 9781u + base * 6271u + k * 1013u;
      float a = hash11(seed) * 6.2831853, rr = f_pattern1.z * sqrt(hash11(seed + 1u));
      vec2 q = vec2(cos(a), sin(a)) * rr;
      if (hash11(seed + 2u) < emit_chance(q)) {
        g_tpos[s] = vec4(keep_in(vec3(f_force1.x + q.x, f_domain_min.y, f_force1.z + q.y)), 0.0);
        g_tvel[s] = vec4(0.0);
        return;
      }
    }
    g_tpos[s] = vec4(0.0, 0.0, 0.0, -1.0);
    return;
  }
  if (rel < emit && f_splat_dims.w > 0u) {
    vec4 e = f_emit[rel % emitters];
    float a = hash11(s * 7u + base * 13u) * 6.2831853, rr = e.w * sqrt(hash11(s * 11u + base * 3u + 1u));
    vec3 p = e.xyz + vec3(cos(a) * rr, 0.0, sin(a) * rr);
    if (is3d()) p.y += (hash11(s * 5u + base + 2u) - 0.5) * e.w;
    g_tpos[s] = vec4(keep_in(p), 0.0);
    g_tvel[s] = vec4(0.0);
    return;
  }
  vec4 tp = g_tpos[s];
  if (tp.w < 0.0) return;
  float age = tp.w + dt();
  vec3 p = tp.xyz;
  if (age > f_flow.z) {
    g_tpos[s].w = -1.0;
    return;
  }
  ivec3 c = cell_of(p);
  int yr = is3d() ? 1 : 0;
  vec3 vsum = vec3(0.0);
  float wsum = 0.0;
  uint seen[27];
  uint nseen = 0u;
  for (int dx = -1; dx <= 1; ++dx)
    for (int dy = -yr; dy <= yr; ++dy)
      for (int dz = -1; dz <= 1; ++dz) {
        uint key = key_of(c + ivec3(dx, dy, dz));
        bool again = false;
        for (uint q = 0u; q < nseen; ++q) again = again || seen[q] == key;
        if (again) continue;
        seen[nseen++] = key;
        uint s0 = g_starts[key], s1 = s0 + g_counts[key];
        for (uint k = s0; k < s1; ++k) {
          uint j = g_sorted[k];
          vec3 r = p - g_pos[j].xyz;
          float w = poly6(dot(r, r));
          vsum += g_vel[j].xyz * w;
          wsum += w;
        }
      }
  vec3 v = wsum > 1e-20 ? vsum / wsum : vec3(0.0);
  if (!is3d()) v.y = 0.0;
  g_tpos[s] = vec4(keep_in(p + v * dt()), age);
  g_tvel[s] = vec4(v, 0.0);
}
)GLSL";

// 6. The tracers splatted into the density volume (trilinear, fixed point atomics), then resolved into it.
constexpr const char* kSplatCS = R"GLSL(
layout(local_size_x = 64) in;
void main() {
  uint s = gl_GlobalInvocationID.x;
  if (s >= uint(f_counts.y)) return;
  vec4 tp = g_tpos[s];
  if (tp.w < 0.0) return;
  float life = f_flow.z;
  float w0 = f_force2.z * smoothstep(0.0, 2.0, tp.w) * (1.0 - smoothstep(life * 0.7, life, tp.w));
  uvec3 dims = f_splat_dims.xyz;
  vec3 g = (tp.xyz - f_splat_min.xyz) / f_splat_size.xyz * vec3(dims) - 0.5;
  if (dims.y == 1u) g.y = 0.0;
  ivec3 b = ivec3(floor(g));
  vec3 f = g - vec3(b);
  for (int k = 0; k < 8; ++k) {
    ivec3 o = ivec3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
    if (dims.y == 1u && o.y == 1) continue;
    ivec3 q = b + o;
    if (any(lessThan(q, ivec3(0))) || any(greaterThanEqual(q, ivec3(dims)))) continue;
    vec3 t = mix(1.0 - f, f, vec3(o));
    float w = w0 * t.x * (dims.y == 1u ? 1.0 : t.y) * t.z;
    atomicAdd(g_accum[uint(q.x) + uint(q.y) * dims.x + uint(q.z) * dims.x * dims.y], uint(w * 256.0 + 0.5));
  }
}
)GLSL";

constexpr const char* kResolveCS = R"GLSL(
layout(local_size_x = 64) in;
layout(set = 0, binding = 19, r32f) uniform writeonly image3D g_density;
void main() {
  uvec3 dims = f_splat_dims.xyz;
  uint idx = gl_GlobalInvocationID.x;
  if (idx >= dims.x * dims.y * dims.z) return;
  uint x = idx % dims.x, y = (idx / dims.x) % dims.y, z = idx / (dims.x * dims.y);
  imageStore(g_density, ivec3(x, y, z), vec4(float(g_accum[idx]) / 256.0));
  g_accum[idx] = 0u;
}
)GLSL";

// D229: the cloud field. 7. The air's velocity splatted onto the grid (a tent of 1.5 cells, fixed point atomics).
constexpr const char* kGridSplatCS = R"GLSL(
layout(local_size_x = 64) in;
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= particle_count()) return;
  vec3 x = g_pos[i].xyz, v = g_vel[i].xyz;
  uvec2 dims = f_cloud_dims.zw;
  vec2 g = (x.xz - f_domain_min.xz) / (f_domain_max.xz - f_domain_min.xz) * vec2(dims) - 0.5;
  ivec2 b = ivec2(floor(g));
  vec2 fr = g - vec2(b);
  for (int dz = -1; dz <= 2; ++dz)
    for (int dx = -1; dx <= 2; ++dx) {
      ivec2 q = b + ivec2(dx, dz);
      if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, ivec2(dims)))) continue;
      vec2 t = max(1.0 - abs(vec2(dx, dz) - fr) / 1.5, 0.0);
      float w = t.x * t.y;
      if (w <= 0.0) continue;
      uint c = (uint(q.x) + uint(q.y) * dims.x) * 3u;
      atomicAdd(g_gsum[c], int(w * 1024.0 + 0.5));
      atomicAdd(g_gsum[c + 1u], int(round(w * v.x * 64.0)));
      atomicAdd(g_gsum[c + 2u], int(round(w * v.z * 64.0)));
    }
}
)GLSL";

// 8. The grid's velocities from its sums (none where the air is too thin to say), the sums cleared.
constexpr const char* kGridResolveCS = R"GLSL(
layout(local_size_x = 64) in;
void main() {
  uvec2 dims = f_cloud_dims.zw;
  uint c = gl_GlobalInvocationID.x;
  if (c >= dims.x * dims.y) return;
  float w = float(g_gsum[c * 3u]) / 1024.0;
  vec2 v = vec2(float(g_gsum[c * 3u + 1u]), float(g_gsum[c * 3u + 2u])) / 64.0;
  g_gvel[c] = w > 0.05 ? v / w * smoothstep(0.05, 0.3, w) : vec2(0.0);
  g_gsum[c * 3u] = 0, g_gsum[c * 3u + 1u] = 0, g_gsum[c * 3u + 2u] = 0;
}
)GLSL";

// 9. The flow map and the convection carried a step (semi-Lagrangian, a midpoint trace back along the grid's velocity):
// the flow map's phases where the air came from, plus how far; the convection drawn toward the pattern.
constexpr const char* kCloudBody = R"GLSL(
layout(local_size_x = 64) in;
layout(set = 0, binding = 23) uniform sampler2D g_cloud_in;
layout(set = 0, binding = 24, rgba32f) uniform writeonly image2D g_cloud_out;
layout(set = 0, binding = 25) uniform sampler2D g_conv_in;
layout(set = 0, binding = 26, rgba16f) uniform writeonly image2D g_conv_out;
layout(set = 0, binding = 27) uniform sampler2D g_weather;
layout(set = 0, binding = 28) uniform sampler3D g_shape;  // D234: the clouds' shape volume (r: their columns)
vec2 grid_velocity(vec2 x) {
  uvec2 dims = f_cloud_dims.zw;
  vec2 g = (x - f_domain_min.xz) / (f_domain_max.xz - f_domain_min.xz) * vec2(dims) - 0.5;
  g = clamp(g, vec2(0.0), vec2(dims) - 1.001);
  ivec2 b = ivec2(floor(g));
  vec2 f = g - vec2(b);
  uint i = uint(b.x) + uint(b.y) * dims.x;
  return mix(mix(g_gvel[i], g_gvel[i + 1u], f.x), mix(g_gvel[i + dims.x], g_gvel[i + dims.x + 1u], f.x), f.y);
}
void main() {
  uvec2 dims = f_cloud_dims.xy;
  uint idx = gl_GlobalInvocationID.x;
  if (idx >= dims.x * dims.y) return;
  uvec2 t = uvec2(idx % dims.x, idx / dims.x);
  vec2 size = f_domain_max.xz - f_domain_min.xz;
  vec2 x = f_domain_min.xz + (vec2(t) + 0.5) / vec2(dims) * size;
  float speed = f_cloud0.x, step_dt = dt() * speed;
  vec2 back = x - grid_velocity(x - grid_velocity(x) * step_dt * 0.5) * step_dt;
  vec2 buv = (back - f_domain_min.xz) / size;
  bool reset = f_cloud2.w > 0.5;
  vec4 d = texture(g_cloud_in, buv) + vec4(back - x, back - x);
  if (reset || f_cloud0.y > 0.5 || any(isnan(d.xy))) d.xy = vec2(0.0);
  if (reset || f_cloud0.z > 0.5 || any(isnan(d.zw))) d.zw = vec2(0.0);
  imageStore(g_cloud_out, ivec2(t), clamp(d, vec4(-1e5), vec4(1e5)));
  vec2 rel = x - f_force1.xz;
  // The weather map where the air has carried it from, the two phases blended.
  vec2 sky = f_cloud1.xy + x * f_cloud1.z + f_cloud3.xy;
  vec2 pa = sky + d.xy * f_cloud1.z, pb = sky + d.zw * f_cloud1.z;
  vec2 wm = mix(textureLod(g_weather, pa * f_cloud1.w, 0.0).rg, textureLod(g_weather, pb * f_cloud1.w, 0.0).rg, f_cloud2.x);
  // D234: the storm's cloud is the sky's own clouds, gathered and carried (the user, 23 Nov 2026: "be like PBF so the
  // hurricane can be built from those clouds actually swirling and forming together"). Its r is no longer drawn toward
  // the radar's pattern: it is a cloud amount carried by the air (with no phases: it lasts), that grows out of the
  // clouds the sky has where the air brought them from (their columns as the march sees them: the shape noise at the
  // layer's foot, cut by the cover), keeps a while once grown and is carried on round, so its trails are wound by the
  // storm's turning into bands (the spiral is the flow's, not a picture); gathered where the air converges, cleared
  // in the eye as it opens. The radar's pattern (and the scattered cells before the storm organises) only says how
  // readily the clouds grow where they are.
  float m = texture(g_conv_in, buv).r;
  if (isnan(m) || reset) m = 0.0;
  {
    float hx = size.x / float(f_cloud_dims.z);
    float div = (grid_velocity(x + vec2(hx, 0.0)).x - grid_velocity(x - vec2(hx, 0.0)).x +
                 grid_velocity(x + vec2(0.0, hx)).y - grid_velocity(x - vec2(0.0, hx)).y) / (2.0 * hx);
    m *= clamp(exp(-div * step_dt), 0.9, 1.1);
  }
  float strength = f_cloud2.y;
  float r = length(rel) / max(f_pattern1.z, 1.0);   // in rain shields
  float eye = f_pattern2.z / max(f_pattern1.z, 1.0), eye_open = f_pattern2.w;
  // (The shape as the march reads it a third of the way up the layer, the forming clock's drift with it.)
  vec3 sy = vec3(0.0, 2000.0 + f_cloud3.w * 40.0, f_cloud3.w * 18.0);
  vec4 s = mix(textureLod(g_shape, (vec3(pa.x, 0.0, pa.y) + sy) / 9000.0, 0.0),
               textureLod(g_shape, (vec3(pb.x, 0.0, pb.y) + sy) / 9000.0, 0.0), f_cloud2.x);
  float wfbm = s.g * 0.625 + s.b * 0.25 + s.a * 0.125;
  float fs = clamp((s.r - (wfbm - 1.0)) / max(2.0 - wfbm, 1e-3), 0.0, 1.0);
  float cover = clamp(f_cloud3.z * (0.6 + wm.r * 0.8), 0.05, 1.0);
  float seed = clamp((fs - (1.0 - cover)) / cover, 0.0, 1.0);
  float wall = exp(-pow((r - eye * 1.45) / 0.14, 2.0));   // the eye's wall, where the inflow piles up and rises
  float reach = 1.0 - smoothstep(0.55, 1.0, r);           // the rain shield
  float ready = f_pattern1.w > 0.5 ? mix(1.0, 0.45 + 1.1 * emit_chance(rel), 0.5) : 1.0;
  float target = strength * reach * clamp(smoothstep(mix(0.08, 0.0, wall), mix(0.55, 0.2, wall), seed) * ready, 0.0, 1.0);
  float rate = f_cloud2.z * (1.0 + 2.0 * wall);
  // Grows quickly toward what the clouds here make of it; once grown it is let go more slowly (half a minute), so it
  // is carried on round as a trail.
  m += (target - m) * (1.0 - exp(-(target > m ? rate : 1.0 / 30.0) * dt()));
  m *= 1.0 - (1.0 - exp(-0.6 * dt())) * eye_open * (1.0 - smoothstep(eye * 0.75, eye * 1.05, r));
  imageStore(g_conv_out, ivec2(t), vec4(clamp(m, 0.0, 1.0), wm, 1.0));
}
)GLSL";

// A developer's view of the density, over a corner of the picture.
constexpr const char* kDebugVS = R"GLSL(
layout(location = 0) out vec2 v_uv;
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  v_uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";
constexpr const char* kDebugPS3D = R"GLSL(
layout(set = 0, binding = 0) uniform sampler3D g_view;
layout(std140, set = 0, binding = 1) uniform DebugConstants { vec4 g_params; };
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
  float v = texture(g_view, vec3(v_uv.x, 0.5, v_uv.y)).r;
  float t = v / (v + 4.0);
  o_color = vec4(mix(vec3(0.04, 0.05, 0.08), vec3(0.95, 0.97, 1.0), t), 0.85);
}
)GLSL";
constexpr const char* kDebugPS2D = R"GLSL(
layout(set = 0, binding = 0) uniform sampler2D g_view;
layout(std140, set = 0, binding = 1) uniform DebugConstants { vec4 g_params; };
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
  vec4 q = texture(g_view, v_uv);
  vec3 c = mix(vec3(0.04, 0.05, 0.08), vec3(1.0, 0.35, 0.2), clamp(q.r, 0.0, 1.0));
  o_color = vec4(c, 0.9);
}
)GLSL";
constexpr const char* kDebugPSFlow = R"GLSL(
layout(set = 0, binding = 0) uniform sampler2D g_view;
layout(std140, set = 0, binding = 1) uniform DebugConstants { vec4 g_params; };
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
  vec4 d = texture(g_view, v_uv);
  vec2 p = (v_uv - 0.5) * g_params.x + d.xy;
  float ch = mod(floor(p.x / 100.0) + floor(p.y / 100.0), 2.0);
  o_color = vec4(mix(vec3(0.12, 0.14, 0.2), vec3(0.85, 0.88, 0.95), ch), 0.9);
}
)GLSL";
constexpr const char* kDebugPSArray = R"GLSL(
layout(set = 0, binding = 0) uniform sampler2DArray g_view;
layout(std140, set = 0, binding = 1) uniform DebugConstants { vec4 g_params; };
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
  float v = texture(g_view, vec3(v_uv, g_params.x)).r;
  o_color = vec4(mix(vec3(0.04, 0.05, 0.08), vec3(0.95, 0.97, 1.0), v), 0.9);
}
)GLSL";

RefCntAutoPtr<IShader> MakeShader(IRenderDevice* device, SHADER_TYPE type, const std::string& body, const char* name,
                                  bool common) {
  RefCntAutoPtr<IShader> shader;
  const GlslStage stage = type == SHADER_TYPE_VERTEX    ? GlslStage::kVertex
                          : type == SHADER_TYPE_COMPUTE ? GlslStage::kCompute
                                                        : GlslStage::kPixel;
  const std::string source = std::string("#version 450\n") + (common ? kCommon : "") + body;
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

// MakePass's flags: what a pass has besides the buffers.
constexpr uint32_t kPassDensity = 1;  // the density volume (written)
constexpr uint32_t kPassPattern = 2;  // the storm's pattern (read)
constexpr uint32_t kPassClouds = 4;   // D229: the cloud field (read one, write the other) and the weather map

uint32_t NextPow2(uint32_t n) {
  uint32_t p = 1;
  while (p < n) p <<= 1;
  return p;
}

}  // namespace

FluidSim::~FluidSim() = default;

bool FluidSim::MakePass(const std::string& body, const char* name, uint32_t, Pass& out, uint32_t flags) {
  RefCntAutoPtr<IShader> cs = MakeShader(device_, SHADER_TYPE_COMPUTE, body, name, true);
  if (!cs) return false;
  ComputePipelineStateCreateInfo info;
  info.PSODesc.Name = name;
  info.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
  info.pCS = cs;
  // Only the resolve pass writes the density volume and only the tracers' pass reads the pattern (dynamic variables;
  // the rest are bound once, static). The 18
  // storage buffers as plain ones: as Vulkan's dynamic storage buffers (Diligent's default) they passed the AMD
  // driver's limit of 8 a pipeline (run fluid1).
  const bool image = flags & kPassDensity;
  static constexpr const char* kBuffers[] = {"FluidPos",   "FluidPred",  "FluidVel",    "FluidLambda",   "FluidDelta",
                                             "FluidOmega", "FluidEta",   "FluidDv",     "FluidKey",      "FluidCounts",
                                             "FluidStarts", "FluidCursor", "FluidSorted", "FluidNbr", "FluidNbrCount",
                                             "FluidTPos",  "FluidTVel",  "FluidAccum", "FluidGridSum", "FluidGridVel"};
  std::vector<ShaderResourceVariableDesc> variables;
  for (const char* b : kBuffers)
    variables.push_back({SHADER_TYPE_COMPUTE, b, SHADER_RESOURCE_VARIABLE_TYPE_STATIC, SHADER_VARIABLE_FLAG_NO_DYNAMIC_BUFFERS});
  if (image) variables.push_back({SHADER_TYPE_COMPUTE, "g_density", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC});
  const bool pattern = flags & kPassPattern, clouds = flags & kPassClouds;
  if (pattern) variables.push_back({SHADER_TYPE_COMPUTE, "g_pattern", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC});
  if (clouds)
    for (const char* v : {"g_cloud_in", "g_cloud_out", "g_conv_in", "g_conv_out", "g_weather", "g_shape"})
      variables.push_back({SHADER_TYPE_COMPUTE, v, SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC});
  SamplerDesc lin, wrap;
  lin.MinFilter = lin.MagFilter = lin.MipFilter = FILTER_TYPE_LINEAR;
  lin.AddressU = lin.AddressV = lin.AddressW = TEXTURE_ADDRESS_CLAMP;
  wrap.MinFilter = wrap.MagFilter = wrap.MipFilter = FILTER_TYPE_LINEAR;
  wrap.AddressU = wrap.AddressV = wrap.AddressW = TEXTURE_ADDRESS_WRAP;
  ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_COMPUTE, "g_pattern", lin},
                                     {SHADER_TYPE_COMPUTE, "g_cloud_in", lin},
                                     {SHADER_TYPE_COMPUTE, "g_conv_in", lin},
                                     {SHADER_TYPE_COMPUTE, "g_weather", wrap},
                                     {SHADER_TYPE_COMPUTE, "g_shape", wrap}};
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
  info.PSODesc.ResourceLayout.Variables = variables.data();
  info.PSODesc.ResourceLayout.NumVariables = uint32_t(variables.size());
  info.PSODesc.ResourceLayout.ImmutableSamplers = pattern ? samplers : nullptr;
  info.PSODesc.ResourceLayout.NumImmutableSamplers = clouds ? 5 : pattern ? 1 : 0;
  device_->CreateComputePipelineState(info, &out.pso);
  if (!out.pso) return false;
  auto set = [&](const char* var, IBuffer* buffer) {
    if (auto* v = out.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, var))
      v->Set(buffer->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
  };
  if (auto* v = out.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "FluidConstants")) v->Set(constants_);
  set("FluidPos", pos_), set("FluidPred", pred_), set("FluidVel", vel_), set("FluidLambda", lambda_);
  set("FluidDelta", delta_), set("FluidOmega", omega_), set("FluidEta", eta_), set("FluidDv", dv_);
  set("FluidKey", key_), set("FluidCounts", counts_), set("FluidStarts", starts_), set("FluidCursor", cursor_);
  set("FluidSorted", sorted_), set("FluidNbr", nbr_), set("FluidNbrCount", nbr_count_), set("FluidTPos", tpos_);
  set("FluidTVel", tvel_), set("FluidAccum", accum_), set("FluidGridSum", grid_sum_), set("FluidGridVel", grid_vel_);
  out.pso->CreateShaderResourceBinding(&out.srb, true);
  if (!out.srb) return false;
  if (image)
    if (auto* v = out.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_density"))
      v->Set(density_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
  return true;
}

bool FluidSim::Initialize(IRenderDevice* device, const Config& config, const char* name) {
  device_ = device;
  config_ = config;
  name_ = name ? name : "fluid";
  const uint32_t n = std::max(config.particles, 64u), t = std::max(config.tracers, 64u);
  config_.particles = n, config_.tracers = t;
  if (!config.three_d) config_.splat_dims[1] = 1;
  table_ = std::min(NextPow2(n * 2), 1u << 20);
  h_ = config.spacing * config.h_factor;
  // The rest density: the kernel summed over a lattice at the rest spacing (square in 2D, cubic in 3D), a particle's
  // mass 1; the relaxation, a share of the constraint's gradient norm there (PBF's epsilon).
  {
    const double kPi = 3.14159265358979;
    const double h = h_, d = config.spacing;
    const double poly6 = config.three_d ? 315.0 / (64.0 * kPi * std::pow(h, 9)) : 4.0 / (kPi * std::pow(h, 8));
    const double spiky = config.three_d ? 45.0 / (kPi * std::pow(h, 6)) : 30.0 / (kPi * std::pow(h, 5));
    const int reach = int(std::ceil(h / d));
    double rho = 0.0, gsq = 0.0;
    for (int x = -reach; x <= reach; ++x)
      for (int y = config.three_d ? -reach : 0; y <= (config.three_d ? reach : 0); ++y)
        for (int z = -reach; z <= reach; ++z) {
          const double r2 = (x * x + y * y + z * z) * d * d;
          if (r2 >= h * h) continue;
          rho += poly6 * std::pow(h * h - r2, 3);
          const double r = std::sqrt(r2), tt = h - r;
          if (r > 0.0) gsq += std::pow(spiky * tt * tt, 2);
        }
    rest_density_ = float(rho);
    relaxation_ = float(config.relaxation * gsq / (rho * rho));
  }
  BufferDesc cb;
  cb.Name = "NativeXRenderer fluid constants";
  cb.Size = sizeof(Constants);
  cb.Usage = USAGE_DYNAMIC;
  cb.BindFlags = BIND_UNIFORM_BUFFER;
  cb.CPUAccessFlags = CPU_ACCESS_WRITE;
  device->CreateBuffer(cb, nullptr, &constants_);
  auto buffer = [&](const char* label, uint64_t count, uint32_t stride, const void* init, RefCntAutoPtr<IBuffer>& out) {
    BufferDesc d;
    const std::string full = "NativeXRenderer fluid " + name_ + " " + label;
    d.Name = full.c_str();
    d.Size = count * stride;
    d.Usage = USAGE_DEFAULT;
    d.BindFlags = BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE;
    d.Mode = BUFFER_MODE_STRUCTURED;
    d.ElementByteStride = stride;
    BufferData data(init, init ? d.Size : 0);
    device->CreateBuffer(d, init ? &data : nullptr, &out);
  };
  const std::vector<float> zeros4(size_t(std::max(n, t)) * 4, 0.0f);
  std::vector<float> dead(size_t(t) * 4, 0.0f);
  for (uint32_t i = 0; i < t; ++i) dead[size_t(i) * 4 + 3] = -1.0f;
  const uint32_t voxels = config_.splat_dims[0] * config_.splat_dims[1] * config_.splat_dims[2];
  const std::vector<uint32_t> zero_u(std::max<size_t>({size_t(table_), size_t(voxels), size_t(n) * kMaxNeighbours}), 0u);
  buffer("positions", n, 16, zeros4.data(), pos_);
  buffer("predicted", n, 16, zeros4.data(), pred_);
  buffer("velocities", n, 16, zeros4.data(), vel_);
  buffer("lambda", n, 4, zero_u.data(), lambda_);
  buffer("delta", n, 16, zeros4.data(), delta_);
  buffer("omega", n, 16, zeros4.data(), omega_);
  buffer("eta", n, 16, zeros4.data(), eta_);
  buffer("dv", n, 16, zeros4.data(), dv_);
  buffer("keys", n, 4, zero_u.data(), key_);
  buffer("counts", table_, 4, zero_u.data(), counts_);
  buffer("starts", table_, 4, zero_u.data(), starts_);
  buffer("cursor", table_, 4, zero_u.data(), cursor_);
  buffer("sorted", n, 4, zero_u.data(), sorted_);
  buffer("neighbours", uint64_t(n) * kMaxNeighbours, 4, zero_u.data(), nbr_);
  buffer("neighbour counts", n, 4, zero_u.data(), nbr_count_);
  buffer("tracer positions", t, 16, dead.data(), tpos_);
  buffer("tracer velocities", t, 16, zeros4.data(), tvel_);
  buffer("density sums", voxels, 4, zero_u.data(), accum_);
  // D229: the cloud field (2D only), its grid's buffers a cell's worth without one (every pass declares them).
  if (config_.three_d) config_.cloud_dims[0] = config_.cloud_dims[1] = 0;
  const bool clouds = config_.cloud_dims[0] > 0 && config_.cloud_dims[1] > 0;
  for (auto& g : config_.grid_dims) g = std::clamp(g, 2u, 1024u);
  const uint32_t cells = clouds ? config_.grid_dims[0] * config_.grid_dims[1] : 1;
  {
    const std::vector<int32_t> zero_i(size_t(cells) * 3, 0);
    buffer("grid sums", uint64_t(cells) * 3, 4, zero_i.data(), grid_sum_);
    buffer("grid velocities", cells, 8, zero_i.data(), grid_vel_);
  }
  if (clouds) {
    const std::vector<float> zero_f(size_t(config_.cloud_dims[0]) * config_.cloud_dims[1] * 4, 0.0f);
    auto field = [&](const char* what, int k, TEXTURE_FORMAT format, uint32_t texel, RefCntAutoPtr<ITexture>& out) {
      TextureDesc cd;
      const std::string cname = "NativeXRenderer fluid " + name_ + " " + what + " " + std::to_string(k);
      cd.Name = cname.c_str();
      cd.Type = RESOURCE_DIM_TEX_2D;
      cd.Width = config_.cloud_dims[0], cd.Height = config_.cloud_dims[1];
      cd.Format = format;
      cd.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
      TextureSubResData sub{zero_f.data(), uint64_t(config_.cloud_dims[0]) * texel};
      TextureData data(&sub, 1);
      device->CreateTexture(cd, &data, &out);
    };
    for (int k = 0; k < 2; ++k) {
      field("flow", k, TEX_FORMAT_RGBA32_FLOAT, 16, cloud_[k]);
      field("convection", k, TEX_FORMAT_RGBA16_FLOAT, 8, conv_[k]);
    }
    TextureDesc wd;
    wd.Name = "NativeXRenderer fluid no weather";
    wd.Type = RESOURCE_DIM_TEX_2D;
    wd.Width = wd.Height = 1;
    wd.Format = TEX_FORMAT_RG8_UNORM;
    wd.Usage = USAGE_IMMUTABLE;
    wd.BindFlags = BIND_SHADER_RESOURCE;
    const uint8_t zero2[2] = {};
    TextureSubResData wsub{zero2, 2};
    TextureData wdata(&wsub, 1);
    device->CreateTexture(wd, &wdata, &no_weather_);
    TextureDesc sd;  // D234: bound while the sky's shape volume is not known
    sd.Name = "NativeXRenderer fluid no shape";
    sd.Type = RESOURCE_DIM_TEX_3D;
    sd.Width = sd.Height = sd.Depth = 1;
    sd.Format = TEX_FORMAT_RGBA8_UNORM;
    sd.Usage = USAGE_IMMUTABLE;
    sd.BindFlags = BIND_SHADER_RESOURCE;
    const uint8_t zero4[4] = {};
    TextureSubResData ssub{zero4, 4, 4};
    TextureData sdata(&ssub, 1);
    device->CreateTexture(sd, &sdata, &no_shape_);
    if (!cloud_[0] || !cloud_[1] || !conv_[0] || !conv_[1] || !no_weather_ || !no_shape_) {
      REXLOG_ERROR("[native_x_renderer] fluid {}: its cloud field could not be made", name_);
      return false;
    }
  }
  TextureDesc td;
  const std::string tname = "NativeXRenderer fluid " + name_ + " density";
  td.Name = tname.c_str();
  td.Type = RESOURCE_DIM_TEX_3D;
  td.Width = config_.splat_dims[0], td.Height = config_.splat_dims[1], td.Depth = config_.splat_dims[2];
  td.Format = TEX_FORMAT_R32_FLOAT;
  td.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
  device->CreateTexture(td, nullptr, &density_);
  {  // D229: a layer of nothing for the pattern's place while there is none
    TextureDesc nd;
    nd.Name = "NativeXRenderer fluid no pattern";
    nd.Type = RESOURCE_DIM_TEX_2D_ARRAY;
    nd.Width = nd.Height = 1, nd.ArraySize = 1;
    nd.Format = TEX_FORMAT_R8_UNORM;
    nd.Usage = USAGE_IMMUTABLE;
    nd.BindFlags = BIND_SHADER_RESOURCE;
    const uint8_t zero = 0;
    TextureSubResData sub{&zero, 1};
    TextureData data(&sub, 1);
    device->CreateTexture(nd, &data, &no_pattern_);
  }
  if (!constants_ || !no_pattern_ || !pos_ || !pred_ || !vel_ || !lambda_ || !delta_ || !omega_ || !eta_ || !dv_ || !key_ || !counts_ ||
      !starts_ || !cursor_ || !sorted_ || !nbr_ || !nbr_count_ || !tpos_ || !tvel_ || !accum_ || !density_ ||
      !grid_sum_ || !grid_vel_) {
    REXLOG_ERROR("[native_x_renderer] fluid {}: its buffers could not be made", name_);
    return false;
  }
  const bool ok = MakePass(kPredictCS, "NativeXRenderer fluid predict", 64, predict_) &&
                  MakePass(kClearCS, "NativeXRenderer fluid clear", 64, clear_) &&
                  MakePass(kHashCS, "NativeXRenderer fluid hash", 64, hash_) &&
                  MakePass(kScanCS, "NativeXRenderer fluid scan", 256, scan_) &&
                  MakePass(kScatterCS, "NativeXRenderer fluid scatter", 64, scatter_) &&
                  MakePass(kNeighboursCS, "NativeXRenderer fluid neighbours", 64, neighbours_) &&
                  MakePass(kLambdaCS, "NativeXRenderer fluid lambda", 64, lambda_pass_) &&
                  MakePass(kDeltaCS, "NativeXRenderer fluid delta", 64, delta_pass_) &&
                  MakePass(kApplyCS, "NativeXRenderer fluid apply", 64, apply_) &&
                  MakePass(kAdvectCS, "NativeXRenderer fluid advect", 64, advect_) &&
                  MakePass(kCurlCS, "NativeXRenderer fluid curl", 64, curl_) &&
                  MakePass(kGradientCS, "NativeXRenderer fluid vorticity gradient", 64, gradient_) &&
                  MakePass(kVApplyCS, "NativeXRenderer fluid velocity apply", 64, vapply_) &&
                  MakePass(std::string(kPatternGLSL) + kTracersBody, "NativeXRenderer fluid tracers", 64, tracers_,
                           kPassPattern) &&
                  MakePass(kSplatCS, "NativeXRenderer fluid splat", 64, splat_) &&
                  MakePass(kResolveCS, "NativeXRenderer fluid resolve", 64, resolve_, kPassDensity) &&
                  (!clouds || (MakePass(kGridSplatCS, "NativeXRenderer fluid grid splat", 64, grid_splat_) &&
                               MakePass(kGridResolveCS, "NativeXRenderer fluid grid resolve", 64, grid_resolve_) &&
                               MakePass(std::string(kPatternGLSL) + kCloudBody, "NativeXRenderer fluid clouds", 64,
                                        cloud_pass_, kPassPattern | kPassClouds)));
  if (!ok) {
    REXLOG_ERROR("[native_x_renderer] fluid {}: its passes could not be made", name_);
    return false;
  }
  if (clouds) {
    cloud_index_ = 0;
    cloud_view_ = cloud_[0]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    conv_view_ = conv_[0]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    REXLOG_INFO("[native_x_renderer] fluid {}: a flow map of {}x{} carried on a velocity grid of {}x{} (D229)", name_,
                config_.cloud_dims[0], config_.cloud_dims[1], config_.grid_dims[0], config_.grid_dims[1]);
  }
  ready_ = true;
  REXLOG_INFO("[native_x_renderer] fluid {} (D227): {} particles ({}D), {} tracers, spacing {:.1f} m, h {:.1f} m, rest "
              "density {:.4g}, relaxation {:.3g}, hash table {}, density volume {}x{}x{}",
              name_, n, config.three_d ? 3 : 2, t, config.spacing, h_, rest_density_, relaxation_, table_,
              config_.splat_dims[0], config_.splat_dims[1], config_.splat_dims[2]);
  return true;
}

void FluidSim::Seed(IDeviceContext* context, const float centre[3], float) {
  if (!ready_) return;
  const uint32_t n = config_.particles;
  std::vector<float> p(size_t(n) * 4, 0.0f);
  const double golden = 2.399963229728653;
  if (!config_.three_d) {
    // A sunflower: even spacing in a disc of the rest spacing's area per particle.
    const double radius = config_.spacing * std::sqrt(double(n) / 3.14159265358979);
    for (uint32_t k = 0; k < n; ++k) {
      const double r = radius * std::sqrt((k + 0.5) / n), a = k * golden;
      p[size_t(k) * 4 + 0] = centre[0] + float(r * std::cos(a));
      p[size_t(k) * 4 + 1] = config_.domain_min[1];
      p[size_t(k) * 4 + 2] = centre[2] + float(r * std::sin(a));
    }
  } else {
    // A cubic lattice cut to a ball, nearest first.
    const int side = int(std::ceil(std::cbrt(double(n) * 6.0 / 3.14159265358979))) + 2;
    std::vector<std::array<float, 3>> pts;
    for (int x = -side / 2; x <= side / 2; ++x)
      for (int y = -side / 2; y <= side / 2; ++y)
        for (int z = -side / 2; z <= side / 2; ++z) pts.push_back({float(x), float(y), float(z)});
    std::sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) {
      return a[0] * a[0] + a[1] * a[1] + a[2] * a[2] < b[0] * b[0] + b[1] * b[1] + b[2] * b[2];
    });
    for (uint32_t k = 0; k < n && k < pts.size(); ++k)
      for (int c = 0; c < 3; ++c) p[size_t(k) * 4 + c] = centre[c] + pts[k][c] * config_.spacing;
  }
  const std::vector<float> zeros(p.size(), 0.0f);
  context->UpdateBuffer(pos_, 0, p.size() * 4, p.data(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->UpdateBuffer(pred_, 0, p.size() * 4, p.data(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->UpdateBuffer(vel_, 0, zeros.size() * 4, zeros.data(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void FluidSim::Dispatch(IDeviceContext* context, Pass& pass, uint32_t threads, uint32_t local_size) {
  context->SetPipelineState(pass.pso);
  context->CommitShaderResources(pass.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  const uint32_t groups = (threads + local_size - 1) / local_size;
  context->DispatchCompute(DispatchComputeAttribs(std::min(groups, 65535u), 1, 1));  // (to 4 million threads)
  // What one pass writes the next reads: a barrier on every buffer (UAV to UAV).
  IBuffer* all[] = {pos_, pred_, vel_, lambda_, delta_, omega_, eta_, dv_, key_, counts_,
                    starts_, cursor_, sorted_, nbr_, nbr_count_, tpos_, tvel_, accum_, grid_sum_, grid_vel_};
  StateTransitionDesc barriers[std::size(all)];
  for (size_t k = 0; k < std::size(all); ++k)
    barriers[k] = StateTransitionDesc(all[k], RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS,
                                      STATE_TRANSITION_FLAG_UPDATE_STATE);
  context->TransitionResourceStates(uint32_t(std::size(barriers)), barriers);
}

void FluidSim::Step(IDeviceContext* context, float dt, const Forces& f, const Emitter* emitters, int emitter_count,
                    uint32_t emit) {
  if (!ready_ || dt <= 0.0f) return;
  TimerBegin(context);
  Constants k{};
  const double kPi = 3.14159265358979;
  for (int c = 0; c < 3; ++c) {
    k.domain_min[c] = config_.domain_min[c], k.domain_max[c] = config_.domain_max[c];
    k.splat_min[c] = config_.splat_min[c], k.splat_size[c] = config_.splat_size[c];
    k.splat_dims[c] = config_.splat_dims[c];
    k.force0[c] = f.gravity[c], k.force1[c] = f.centre[c];
  }
  if (!config_.three_d) k.domain_max[1] = k.domain_min[1];
  k.domain_min[3] = config_.three_d ? 3.0f : 2.0f;
  k.domain_max[3] = dt;
  k.kernel[0] = h_, k.kernel[1] = h_ * h_;
  k.kernel[2] = float(config_.three_d ? 315.0 / (64.0 * kPi * std::pow(double(h_), 9)) : 4.0 / (kPi * std::pow(double(h_), 8)));
  k.kernel[3] = float(config_.three_d ? 45.0 / (kPi * std::pow(double(h_), 6)) : 30.0 / (kPi * std::pow(double(h_), 5)));
  k.solve[0] = rest_density_, k.solve[1] = 1.0f / rest_density_, k.solve[2] = relaxation_, k.solve[3] = 1.0f;
  k.counts[0] = float(config_.particles), k.counts[1] = float(config_.tracers), k.counts[2] = float(table_);
  k.counts[3] = h_;
  k.flow[0] = f.vorticity, k.flow[1] = f.viscosity, k.flow[2] = f.tracer_life, k.flow[3] = f.drag;
  k.force0[3] = f.swirl, k.force1[3] = f.pull;
  k.force2[0] = f.reach, k.force2[1] = f.coriolis, k.force2[2] = f.splat_weight;
  k.force3[0] = f.sink, k.force3[1] = f.source, k.force3[2] = float(steps_ % 1000000), k.force3[3] = config_.max_speed;
  k.pattern0[0] = f.pattern_layer_a, k.pattern0[1] = f.pattern_layer_b, k.pattern0[2] = f.pattern_blend;
  k.pattern0[3] = f.pattern_turn;
  k.pattern1[0] = f.pattern_zoom, k.pattern1[1] = f.organisation, k.pattern1[2] = f.pattern_radius;
  k.pattern1[3] = f.pattern_radius > 0.0f ? 1.0f : 0.0f;
  k.pattern2[0] = f.cells_time;
  k.pattern2[1] = f.pattern_power;
  k.cloud0[0] = f.flow_speed, k.cloud0[1] = f.restart_a ? 1.0f : 0.0f, k.cloud0[2] = f.restart_b ? 1.0f : 0.0f;
  k.cloud2[0] = f.share_b, k.cloud2[1] = f.convection, k.cloud2[2] = f.convection_rate;
  k.cloud1[0] = f.cloud_origin[0], k.cloud1[1] = f.cloud_origin[1], k.cloud1[2] = f.cloud_scale;
  k.cloud1[3] = 1.0f / std::max(f.weather_scale, 1.0f);
  k.cloud3[0] = f.cloud_drift[0], k.cloud3[1] = f.cloud_drift[1];
  k.cloud3[2] = f.cloud_coverage, k.cloud3[3] = f.shape_y;  // D234
  k.pattern2[2] = f.eye, k.pattern2[3] = f.eye_open;
  k.cloud2[3] = f.cloud_reset ? 1.0f : 0.0f;
  k.cloud_dims[0] = config_.cloud_dims[0], k.cloud_dims[1] = config_.cloud_dims[1];
  k.cloud_dims[2] = config_.grid_dims[0], k.cloud_dims[3] = config_.grid_dims[1];
  const int ec = std::clamp(emitter_count, 0, kMaxEmitters);
  emit = ec > 0 || f.pattern_radius > 0.0f ? std::min(emit, config_.tracers) : 0;
  k.force2[3] = float(emit);
  k.splat_min[3] = float(emit_base_);
  k.splat_dims[3] = uint32_t(ec);
  for (int e = 0; e < ec; ++e) {
    for (int c = 0; c < 3; ++c) k.emit[e][c] = emitters[e].position[c];
    k.emit[e][3] = emitters[e].radius;
  }
  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, &k, sizeof(k));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }
  const uint32_t n = config_.particles, t = config_.tracers;
  const uint32_t voxels = config_.splat_dims[0] * config_.splat_dims[1] * config_.splat_dims[2];
  Dispatch(context, predict_, n);
  Dispatch(context, clear_, table_);
  Dispatch(context, hash_, n);
  Dispatch(context, scan_, 256, 256);
  Dispatch(context, scatter_, n);
  Dispatch(context, neighbours_, n);
  for (uint32_t it = 0; it < config_.iterations; ++it) {
    Dispatch(context, lambda_pass_, n);
    Dispatch(context, delta_pass_, n);
    Dispatch(context, apply_, n);
  }
  Dispatch(context, advect_, n);
  Dispatch(context, curl_, n);
  if (f.vorticity > 0.0f) Dispatch(context, gradient_, n);
  Dispatch(context, vapply_, n);
  // D229: the cloud field carried by the air's velocity on the grid.
  if (HasClouds()) {
    Dispatch(context, grid_splat_, n);
    Dispatch(context, grid_resolve_, config_.grid_dims[0] * config_.grid_dims[1]);
    const int from = cloud_index_, to = cloud_index_ ^ 1;
    auto bind = [&](const char* var, ITexture* tex, TEXTURE_VIEW_TYPE view) {
      if (auto* v = cloud_pass_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, var)) v->Set(tex->GetDefaultView(view));
    };
    bind("g_cloud_in", cloud_[from], TEXTURE_VIEW_SHADER_RESOURCE);
    bind("g_cloud_out", cloud_[to], TEXTURE_VIEW_UNORDERED_ACCESS);
    bind("g_conv_in", conv_[from], TEXTURE_VIEW_SHADER_RESOURCE);
    bind("g_conv_out", conv_[to], TEXTURE_VIEW_UNORDERED_ACCESS);
    if (auto* v = cloud_pass_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_weather"))
      v->Set(weather_ ? weather_ : no_weather_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    if (auto* v = cloud_pass_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_shape"))
      v->Set(shape_ ? shape_ : no_shape_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    if (auto* v = cloud_pass_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_pattern"))
      v->Set(pattern_ ? pattern_ : no_pattern_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    Dispatch(context, cloud_pass_, config_.cloud_dims[0] * config_.cloud_dims[1]);
    cloud_index_ = to;
    cloud_view_ = cloud_[to]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    conv_view_ = conv_[to]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  }
  if (auto* v = tracers_.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "g_pattern"))
    v->Set(pattern_ ? pattern_ : no_pattern_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  Dispatch(context, tracers_, t);
  Dispatch(context, splat_, t);
  Dispatch(context, resolve_, voxels);
  emit_base_ = (emit_base_ + emit) % t;
  ++steps_;
  TimerEnd(context);
}

void FluidSim::LogState(IDeviceContext* context) {
  if (!ready_) return;
  const uint32_t n = config_.particles, t = config_.tracers;
  const uint64_t bytes = uint64_t(n) * 16 * 2 + uint64_t(t) * 16;
  if (!readback_) {
    BufferDesc d;
    d.Name = "NativeXRenderer fluid readback";
    d.Size = bytes;
    d.Usage = USAGE_STAGING;
    d.CPUAccessFlags = CPU_ACCESS_READ;
    device_->CreateBuffer(d, nullptr, &readback_);
    if (!readback_) return;
  }
  context->CopyBuffer(pos_, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, readback_, 0, uint64_t(n) * 16,
                      RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->CopyBuffer(vel_, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, readback_, uint64_t(n) * 16, uint64_t(n) * 16,
                      RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->CopyBuffer(tpos_, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, readback_, uint64_t(n) * 32, uint64_t(t) * 16,
                      RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->WaitForIdle();
  void* data = nullptr;
  context->MapBuffer(readback_, MAP_READ, MAP_FLAG_NONE, data);
  if (!data) return;
  const float* f = static_cast<const float*>(data);
  double speed = 0.0, fastest = 0.0, r_sum = 0.0;
  uint32_t nan = 0, alive = 0, tnan = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const float* p = f + size_t(i) * 4;
    const float* v = f + size_t(n) * 4 + size_t(i) * 4;
    if (!std::isfinite(p[0]) || !std::isfinite(v[0])) {
      ++nan;
      continue;
    }
    const double s = std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
    speed += s, fastest = std::max(fastest, s), r_sum += std::sqrt(double(p[0]) * p[0] + double(p[2]) * p[2]);
  }
  double t_age = 0.0;
  for (uint32_t s = 0; s < t; ++s) {
    const float* p = f + size_t(n) * 8 + size_t(s) * 4;
    if (!std::isfinite(p[0])) ++tnan;
    else if (p[3] >= 0.0f) ++alive, t_age += p[3];
  }
  context->UnmapBuffer(readback_, MAP_READ);
  REXLOG_INFO("[native_x_renderer] fluid {} state (D227), step {}: particles' speed {:.2f} m/s on average, {:.2f} the "
              "fastest, {:.0f} m from the centre on average, {} not finite; tracers {} alive ({:.1f} s old on average), {} not finite; particle 0 "
              "at {:.1f} {:.1f} {:.1f} moving {:.3f} {:.3f} {:.3f}",
              name_, steps_, n > nan ? speed / (n - nan) : 0.0, fastest, n > nan ? r_sum / (n - nan) : 0.0, nan, alive,
              alive ? t_age / alive : 0.0, tnan, f[0], f[1], f[2], f[size_t(n) * 4], f[size_t(n) * 4 + 1], f[size_t(n) * 4 + 2]);
}

void FluidSim::SetPattern(ITextureView* pattern) { pattern_ = pattern; }

ITextureView* FluidSim::Clouds() const { return cloud_view_; }

ITextureView* FluidSim::Convection() const { return conv_view_; }

void FluidSim::SetCloudSource(ITextureView* weather) { weather_ = weather; }
void FluidSim::SetCloudShape(ITextureView* shape) { shape_ = shape; }

ITextureView* FluidSim::Density() const {
  return density_ ? density_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

void DebugTextureView::Draw(IRenderDevice* device, IDeviceContext* context, ITextureView* view, int kind, float layer,
                            ITexture* target, uint32_t width, uint32_t height, uint32_t size, uint32_t left) {
  if (!device || !view || !target) return;
  const int k = std::clamp(kind, 0, 3);
  const TextureDesc& desc = target->GetDesc();
  if (!constants_) {
    BufferDesc cb;
    cb.Name = "NativeXRenderer debug texture view constants";
    cb.Size = 16;
    cb.Usage = USAGE_DYNAMIC;
    cb.BindFlags = BIND_UNIFORM_BUFFER;
    cb.CPUAccessFlags = CPU_ACCESS_WRITE;
    device->CreateBuffer(cb, nullptr, &constants_);
    if (!constants_) return;
  }
  if (!pso_[k] || format_[k] != desc.Format) {
    pso_[k].Release(), srb_[k].Release();
    RefCntAutoPtr<IShader> vs = MakeShader(device, SHADER_TYPE_VERTEX, kDebugVS, "NativeXRenderer debug view VS", false);
    RefCntAutoPtr<IShader> ps =
        MakeShader(device, SHADER_TYPE_PIXEL, k == 3 ? kDebugPSFlow : k == 2 ? kDebugPS2D : k == 1 ? kDebugPSArray : kDebugPS3D,
                   "NativeXRenderer debug view PS", false);
    if (!vs || !ps) return;
    GraphicsPipelineStateCreateInfo info;
    info.PSODesc.Name = "NativeXRenderer debug texture view";
    info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
    info.GraphicsPipeline.NumRenderTargets = 1;
    info.GraphicsPipeline.RTVFormats[0] = desc.Format;
    info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
    info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
    info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
    auto& rt = info.GraphicsPipeline.BlendDesc.RenderTargets[0];
    rt.BlendEnable = True;
    rt.SrcBlend = BLEND_FACTOR_SRC_ALPHA, rt.DestBlend = BLEND_FACTOR_INV_SRC_ALPHA, rt.BlendOp = BLEND_OPERATION_ADD;
    info.pVS = vs;
    info.pPS = ps;
    SamplerDesc lin;
    lin.MinFilter = lin.MagFilter = lin.MipFilter = FILTER_TYPE_LINEAR;
    lin.AddressU = lin.AddressV = lin.AddressW = TEXTURE_ADDRESS_CLAMP;
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "g_view", lin}};
    info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
    info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    info.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
    device->CreateGraphicsPipelineState(info, &pso_[k]);
    if (!pso_[k]) return;
    pso_[k]->CreateShaderResourceBinding(&srb_[k], true);
    format_[k] = desc.Format;
  }
  if (!srb_[k]) return;
  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    const float params[4] = {layer, 0.0f, 0.0f, 0.0f};
    std::memcpy(data, params, sizeof(params));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }
  if (auto* v = srb_[k]->GetVariableByName(SHADER_TYPE_PIXEL, "g_view")) v->Set(view);
  if (auto* v = srb_[k]->GetVariableByName(SHADER_TYPE_PIXEL, "DebugConstants")) v->Set(constants_);
  ITextureView* rtv = target->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
  context->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  const float side = float(std::min({size, width, height}));
  Viewport vp;
  vp.TopLeftX = float(left), vp.TopLeftY = 16.0f, vp.Width = side, vp.Height = side, vp.MaxDepth = 1.0f;
  context->SetViewports(1, &vp, 0, 0);
  context->SetPipelineState(pso_[k]);
  context->CommitShaderResources(srb_[k], RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DrawAttribs attribs;
  attribs.NumVertices = 3;
  context->Draw(attribs);
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
}

void FluidSim::DrawDebug(IDeviceContext* context, ITexture* target, uint32_t width, uint32_t height, uint32_t size) {
  if (!ready_) return;
  if (HasClouds()) {
    debug_view_.Draw(device_, context, Convection(), 2, 0.0f, target, width, height, size, 16);
    debug_flow_.Draw(device_, context, Clouds(), 3, config_.domain_max[0] - config_.domain_min[0], target, width, height,
                     size, 32 + size);
  }
  else debug_view_.Draw(device_, context, Density(), 0, 0.0f, target, width, height, size, 16);
}

void FluidSim::TimerBegin(IDeviceContext* context) {
  if (queries_failed_ || query_active_) return;
  if (!queries_[0]) {
    if (device_->GetDeviceInfo().Features.DurationQueries != DEVICE_FEATURE_STATE_ENABLED) {
      queries_failed_ = true;
      return;
    }
    for (auto& query : queries_) {
      QueryDesc desc;
      desc.Name = "NativeXRenderer fluid time";
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
      const double ms = double(data.Duration) * 1000.0 / double(data.Frequency);
      timer_total_ms_ += ms;
      timer_max_ms_ = std::max(timer_max_ms_, ms);
      ++timer_steps_;
    }
    query_pending_[query_index_] = false;
  }
  context->BeginQuery(queries_[query_index_]);
  query_active_ = true;
}

void FluidSim::TimerEnd(IDeviceContext* context) {
  if (!query_active_) return;
  context->EndQuery(queries_[query_index_]);
  query_pending_[query_index_] = true;
  query_index_ = (query_index_ + 1) % kQueries;
  query_active_ = false;
  if (timer_steps_ >= 300) {
    REXLOG_INFO("[native_x_renderer] fluid {} GPU time (D227): {:.3f} ms a step on average over {} steps (the longest "
                "{:.3f} ms); {} particles, {} tracers",
                name_, timer_total_ms_ / double(timer_steps_), timer_steps_, timer_max_ms_, config_.particles,
                config_.tracers);
    timer_total_ms_ = timer_max_ms_ = 0.0;
    timer_steps_ = 0;
  }
}

}  // namespace native_x_renderer
