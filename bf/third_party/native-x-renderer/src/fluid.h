// NativeXRenderer - a GPU particle fluid (internal; D227, 20 Nov 2026).
//
// The user, 20 Nov 2026: "controlled particle-based fluid dynamics into the clouds ... planes to fly through clouds,
// pushing the particles of the cloud, and leaving a wispy trail", the hurricane forming from spinning clouds, and the
// ground fog particle based too; "I have a system we could incorporate ... threejs-particle-fluids. We just need to
// make sure it stays performant." (The plan: source/.handoff/FLUID-WEATHER-PLAN.md.)
//
// The core of that library's air, ported from its Three.js TSL (WebGPU) to GLSL compute passes here (its MIT
// notice: THIRD_PARTY_NOTICES.md): position based fluids (Macklin and Mueller 2013) solved as XPBD density
// constraints, a spatial hash grid with a counting sort and neighbour lists, vorticity confinement and XSPH viscosity,
// and massless tracers carried by the air's interpolated velocity (Macklin et al. 2014, eq. 28) and splatted into a
// density volume with atomics. In 2D (the storm's layer: x and z, y held) or 3D (the planes' wakes, the ground fog).
//
// Each Step() is one simulation step, recorded on the context: predict (forces), hash and sort, neighbour lists,
// the density iterations, velocities, vorticity and viscosity, the tracers, the splat. Its GPU time is measured.
// What drives it (the forces' centre and strengths, the tracers' emitters) is set before each step.

#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "Query.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Shader.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"

namespace native_x_renderer {

// A developer's view of a texture over a corner of the picture, `size` pixels square, `left` pixels from the left, by
// its kind: 0 a 3D volume's middle slice (its y), 1 one layer of a 2D array (D228: a storm's radar pattern), 2 a
// storm's convection (D229: r in red), 3 a flow map (D229: a checkerboard 100 units a square, as the flow has carried
// it; `layer` the domain's width).
class DebugTextureView {
 public:
  void Draw(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, Diligent::ITextureView* view, int kind,
            float layer, Diligent::ITexture* target, uint32_t width, uint32_t height, uint32_t size, uint32_t left);

 private:
  Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso_[4];
  Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb_[4];
  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
  Diligent::TEXTURE_FORMAT format_[4] = {Diligent::TEX_FORMAT_UNKNOWN, Diligent::TEX_FORMAT_UNKNOWN,
                                         Diligent::TEX_FORMAT_UNKNOWN, Diligent::TEX_FORMAT_UNKNOWN};
};

class FluidSim {
 public:
  struct Config {
    bool three_d = false;           // 3D, else 2D in x and z
    uint32_t particles = 16384;     // the air's particles
    uint32_t tracers = 32768;       // the most tracers alive
    float spacing = 30.0f;          // the particles' rest spacing, metres
    float h_factor = 2.5f;          // the kernel's radius in spacings
    uint32_t iterations = 3;        // density iterations a step
    float relaxation = 0.2f;        // added to the constraint's gradient norm (PBF's epsilon), a share of it at rest
    float max_speed = 120.0f;       // m/s: no particle faster (a safety rail against a solve that blows up)
    float domain_min[3] = {-2000.0f, 0.0f, -2000.0f};
    float domain_max[3] = {2000.0f, 0.0f, 2000.0f};
    uint32_t splat_dims[3] = {256, 1, 256};  // the density volume (y 1 in 2D)
    float splat_min[3] = {-2000.0f, 0.0f, -2000.0f};
    float splat_size[3] = {4000.0f, 1.0f, 4000.0f};
    // D229: a flow map and a convection field carried by the air (2D only): `cloud_dims` texels over the domain (0:
    // none), the air's velocity splatted onto a grid of `grid_dims` cells to carry them.
    uint32_t cloud_dims[2] = {0, 0};
    uint32_t grid_dims[2] = {128, 128};
  };
  // What drives a step.
  struct Forces {
    float gravity[3] = {};
    float centre[3] = {};         // the low the air is drawn to
    float pull = 0.0f;            // m/s² toward the centre (fading out past `reach`)
    float reach = 1500.0f;        // metres
    float swirl = 0.0f;           // m/s² round the centre (anticlockwise seen from above, the northern hemisphere's)
    float coriolis = 0.0f;        // the Coriolis parameter, 1/s: turns the moving air to its right
    float drag = 0.05f;           // 1/s
    float vorticity = 0.0f;       // vorticity confinement's strength
    float viscosity = 0.01f;      // XSPH's share
    float tracer_life = 60.0f;    // seconds
    float splat_weight = 1.0f;    // each tracer's share of the density volume
    // The storm's overturning in 2D: air that reaches the updraft (within `sink` of the centre) rises out of the layer
    // and comes back at `source` (its outflow returning, at a hashed bearing, at rest). The inflow this makes is what
    // the Coriolis term turns into the storm's spin. 0: none.
    float sink = 0.0f, source = 0.0f;
    // D229: tracers placed by a pattern (SetPattern) instead of the emitters: at random in a disc of `pattern_radius`
    // round the centre, each kept with its chance there, a blend by `organisation` (0 to 1) of scattered convective
    // cells (a noise of cells drifting with `cells_time`) and the pattern (two of its layers blended, turned by
    // `pattern_turn` radians, its middle `pattern_zoom` of it across the disc). 0 emitted: none.
    float pattern_radius = 0.0f, pattern_turn = 0.0f, pattern_zoom = 1.0f, organisation = 1.0f;
    float pattern_layer_a = 0.0f, pattern_layer_b = 0.0f, pattern_blend = 0.0f, cells_time = 0.0f;
    // D229: the flow map (Config::cloud_dims; RGBA32F: two phases' (x, z) each): where what is at a point now was when
    // its phase began, less the point (the domain's units), carried by the air's velocity times `flow_speed` every
    // step; `restart_a` / `restart_b` start that phase again (nothing moved). Its user blends the two, each started
    // again where its share is nothing, so what it carries (the sky's clouds) moves with the air without ever being
    // stretched past a phase's span. The convection (RGBA16F, r), carried too and drawn toward the pattern's chance
    // there (as the tracers', above) times `convection`, at `convection_rate` a second. `pattern_power`: the pattern
    // read at the disc's radius to that power (1: as it is; more: its middle held small, its outside brought in).
    // cloud_reset: both phases started and the convection set to the pattern outright (a new storm).
    // The weather map (SetCloudSource) read where the flow map says, the phases blended by `share_b` (b's share), into
    // the convection's g and b (its cover and kind): the domain's point x is the sky's (cloud_origin + x * cloud_scale +
    // cloud_drift), the map `weather_scale` of the sky's units across. (Baked here so the clouds' march reads it once.)
    float cloud_origin[2] = {}, cloud_scale = 1.0f, cloud_drift[2] = {}, weather_scale = 40000.0f, share_b = 0.0f;
    float flow_speed = 1.0f;
    bool restart_a = false, restart_b = false;
    float convection = 0.0f, convection_rate = 0.05f, pattern_power = 1.0f;
    bool cloud_reset = false;
    // D234: the storm's cloud grown from the sky's own (SetCloudShape): their cover (the sky's), the shape volume's
    // forming clock (sky.cpp's drift.z: the shape's drift), the eye's radius (the domain's units) and how open it is.
    float cloud_coverage = 0.5f, shape_y = 0.0f, eye = 0.0f, eye_open = 0.0f;
  };
  struct Emitter {
    float position[3] = {};
    float radius = 0.0f;
  };
  static constexpr int kMaxEmitters = 16;

  ~FluidSim();
  bool Initialize(Diligent::IRenderDevice* device, const Config& config, const char* name);
  bool Ready() const { return ready_; }
  // Lays the particles out on a lattice in a disc (2D) or ball (3D) round `centre` of `radius`, at rest.
  void Seed(Diligent::IDeviceContext* context, const float centre[3], float radius);
  // One step of `dt` seconds; `emit` tracers spread over the emitters.
  void Step(Diligent::IDeviceContext* context, float dt, const Forces& forces, const Emitter* emitters, int emitter_count,
            uint32_t emit);
  // D229: the pattern new tracers are placed by (a 2D array, R8; null: the emitters).
  void SetPattern(Diligent::ITextureView* pattern);
  // D229: the sky's weather map the flow map carries (RG; null: none).
  void SetCloudSource(Diligent::ITextureView* weather);
  // D234: the sky's cloud shape volume the storm's cloud grows from (3D, r; null: none).
  void SetCloudShape(Diligent::ITextureView* shape);
  // D229: the flow map (RGBA32F, cloud_dims; null without one) and the convection (RGBA16F: r; g and b the weather
  // map as carried).
  Diligent::ITextureView* Clouds() const;
  Diligent::ITextureView* Convection() const;
  // The tracers' density (R32F, splat_dims; 0 until the first step).
  Diligent::ITextureView* Density() const;
  // A developer's view: the density drawn over a corner of `target` (its size width x height), `size` pixels square.
  void DrawDebug(Diligent::IDeviceContext* context, Diligent::ITexture* target, uint32_t width, uint32_t height, uint32_t size);
  const Config& config() const { return config_; }
  // A developer's look (it waits for the GPU): the particles' and tracers' state now, in the log.
  void LogState(Diligent::IDeviceContext* context);

 private:
  struct Pass {
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
  };
  bool MakePass(const std::string& body, const char* name, uint32_t local_size, Pass& out, uint32_t flags = 0);
  bool HasClouds() const { return cloud_view_ != nullptr; }
  void Dispatch(Diligent::IDeviceContext* context, Pass& pass, uint32_t threads, uint32_t local_size = 64);
  void TimerBegin(Diligent::IDeviceContext* context);
  void TimerEnd(Diligent::IDeviceContext* context);

  Diligent::IRenderDevice* device_ = nullptr;
  Config config_;
  std::string name_ = "fluid";
  bool ready_ = false;
  uint32_t table_ = 0;      // the hash table's size, a power of two
  float h_ = 0.0f, rest_density_ = 1.0f, relaxation_ = 0.0f;
  uint32_t emit_base_ = 0;  // where the next tracer goes (a ring)
  uint64_t steps_ = 0;

  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
  // The air: positions (w: unused), predicted positions, velocities, the density solve's lambdas and corrections,
  // vorticity (curl, its length, its gradient), XSPH's change; the grid: each particle's cell key, the table's
  // counts, starts and cursors, the sorted order, the neighbour lists and their counts. The tracers: positions (w:
  // age, below 0 dead), velocities. The density volume's fixed point sums, and the volume.
  Diligent::RefCntAutoPtr<Diligent::IBuffer> pos_, pred_, vel_, lambda_, delta_, omega_, eta_, dv_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> key_, counts_, starts_, cursor_, sorted_, nbr_, nbr_count_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> tpos_, tvel_, accum_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> density_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> readback_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> no_pattern_;  // a 1x1 layer of nothing, bound while there is none
  Diligent::ITextureView* pattern_ = nullptr;
  // D229: the flow map and the convection: the grid's fixed point sums (weight, velocity) and velocities, the two
  // fields' textures twice over (read one, write the other; `cloud_index_` the latest).
  Diligent::RefCntAutoPtr<Diligent::IBuffer> grid_sum_, grid_vel_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> cloud_[2], conv_[2];
  Diligent::ITextureView* cloud_view_ = nullptr;
  Diligent::ITextureView* conv_view_ = nullptr;
  int cloud_index_ = 0;
  DebugTextureView debug_flow_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> no_weather_;
  Diligent::ITextureView* weather_ = nullptr;
  Diligent::RefCntAutoPtr<Diligent::ITexture> no_shape_;  // D234
  Diligent::ITextureView* shape_ = nullptr;

  Pass predict_, clear_, hash_, scan_, scatter_, neighbours_, lambda_pass_, delta_pass_, apply_, advect_, curl_,
      gradient_, vapply_, tracers_, splat_, resolve_, grid_splat_, grid_resolve_, cloud_pass_;
  DebugTextureView debug_view_;

  static constexpr int kQueries = 4;
  Diligent::RefCntAutoPtr<Diligent::IQuery> queries_[kQueries];
  bool query_pending_[kQueries] = {};
  int query_index_ = 0;
  bool query_active_ = false, queries_failed_ = false;
  double timer_total_ms_ = 0.0, timer_max_ms_ = 0.0;
  int timer_steps_ = 0;
};

}  // namespace native_x_renderer
