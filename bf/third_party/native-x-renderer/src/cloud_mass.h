// NativeXRenderer - the clouds' mass as particles (internal; D232, 21 Nov 2026; a developer's first slice, off unless
// native_x_renderer_cloud_particles is set).
//
// The user, 21 Nov 2026: "You positive we couldn't make our clouds part of the particle system with the storm?";
// "That's a lot like what I did with my Three.js Particle Based Fluids ... the volume plume" (threejs-particle-fluids'
// Vortex Plume: massless tracers carried by the air are the smoke, splatted into a density grid, blurred, and ray
// marched; MIT, THIRD_PARTY_NOTICES.md); "we could do a form of LOD for the particles ... Lower more static at
// distance, more particles at close".
//
// The far, coarse level of that: tracers over a square of the sky round the eye are the clouds' mass. Each is born
// where the sky's own weather map has cloud (the likelier the more it has: so the sky at rest is the sky as it was),
// drifts with the clouds' wind, is drawn round and into a storm (its low: the storm is made of the clouds it pulls
// in), fades in and out over its life and is born again elsewhere (the clouds renew). Their count in each cell of a
// grid, blurred and eased over time, is how much cloud is there: the clouds' march reads it in place of the weather
// map's cover (sky.cpp cloud_density) and adds its billows from the noise as before. Stepped at 20 Hz (the far
// level: slow), three small compute passes. The near, fine level (the air as a particle fluid at 60 Hz, a plane's
// wake) is the plan's next step (.handoff/FLUID-WEATHER-PLAN.md).

#pragma once

#include <cstdint>

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"

namespace native_x_renderer {

class CloudMass {
 public:
  static constexpr uint32_t kCells = 256;       // the grid, each way
  static constexpr uint32_t kTracers = 262144;
  static constexpr float kSize = 24000.0f;      // metres across (the world's)
  struct Step {
    float eye[2] = {};          // the world's x and z
    float dt = 0.05f;
    double seconds = 0.0;       // the match's clouds' clock (the same in every player's game)
    float wind[2] = {};         // the clouds' wind, the world's m/s
    float drift[2] = {};        // the weather map's drift (the clouds' units)
    float scale = 1.0f;         // the world's metres a cloud unit
    float storm[4] = {};        // a storm: its centre (the world's x, z), its rain shield's radius, its strength (0: none)
    float storm_eye = 0.0f;     // its eye's radius (0: no eye)
    Diligent::ITextureView* weather = nullptr;  // the sky's weather map
  };
  bool Initialize(Diligent::IRenderDevice* device);
  bool Ready() const { return ready_; }
  void Advance(Diligent::IDeviceContext* context, const Step& step);
  // How much cloud is where (R16F, as the weather map's cover: 0 to 1, a half on average; null before the first
  // step), and its place: the world's x and z of its first cell's corner.
  Diligent::ITextureView* View() const { return view_; }
  const float* origin() const { return origin_; }

 private:
  struct Pass {
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
  };
  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_, tracers_, count_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> mass_[2];
  Pass clear_, move_, resolve_;
  Diligent::ITextureView* view_ = nullptr;
  int index_ = 0;
  float origin_[2] = {};
  uint32_t steps_ = 0;
  bool placed_ = false, ready_ = false;
};

int TestCloudMassShaders();

}  // namespace native_x_renderer
