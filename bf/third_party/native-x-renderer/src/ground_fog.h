// NativeXRenderer - the ground fog pushed about (internal; D231, 21 Nov 2026).
//
// The user, 21 Nov 2026: "low-cost, close to the ground fog that can be pushed around by any dynamic object, bullet
// shots, explosions, vehicles, characters, wind and rain, but regenerates itself."
//
// A field round the eye (256 x 256 texels over 192 m, RGBA16F: the air's own motion along x and z in m/s, and how
// much of the ground fog is there, 1 at rest), stepped at 30 Hz in one compute pass: carried along its own motion and
// the wind (semi-Lagrangian), thinned where the air spreads and piled up where it gathers, stirred by the wind's
// gusts and dimpled by the rain; what moves through it (SetMovers), a blast (Blast) and a shot (Shot) set the air
// there moving. The motion dies down in a few seconds and the fog comes back to its rest in about twelve. The fog
// pass (sky.cpp) reads it over the ground fog's patches near the eye. The fluid weather plan's step 4: no particles
// (the plan's first idea): the field alone costs a few hundredths of a millisecond and the fog pass's march cannot
// show finer than its texels anyway.

#pragma once

#include <cstdint>
#include <vector>

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"

namespace native_x_renderer {

class GroundFog {
 public:
  static constexpr uint32_t kTexels = 256;
  static constexpr float kSize = 192.0f;  // metres across
  static constexpr int kMaxPushers = 32;
  // What pushes the fog in a step: 0 something moving through it (its place, its motion, its radius), 1 a blast (its
  // place, its radius, `strength` the air's speed at its middle, m/s), 2 a shot (from its place to `to`, `strength`
  // the air's speed along it).
  struct Pusher {
    int kind = 0;
    float position[3] = {};
    float velocity[3] = {};  // kind 2: where the shot ends
    float radius = 1.0f;
    float strength = 1.0f;
  };
  struct Step {
    float eye[3] = {};
    float dt = 1.0f / 30.0f;
    double seconds = 0.0;      // the world's time
    float wind[2] = {};        // m/s along x and z
    float gust = 0.0f;         // the gusts' strength, m/s
    float rain = 0.0f;         // 0 to 1
    float sea = 0.0f, top = 28.0f;  // the layer: from the sea's height to its top over it
    bool layer_known = false;  // false: what pushes is not left out by its height
    // What stands in the fog (the user, 21 Nov 2026: "it shouldn't flow through objects, but around them"; "use the
    // heightmap of the terrain, and anything not part of that terrain is considered by the fog to be an object the fog
    // has to collide with"): the view (its depth, and the camera as sky.h's Inputs has it: the view's 0 to 1
    // coordinate to NDC, the world from clip x, y and w, the stored depth's numerator, the view's size in pixels) and
    // the level's terrain (grass.h's TerrainFrame: its heightmap and how the world lies on it). Null: nothing new is
    // marked this step. What is seen 25 cm to 1.8 m over the terrain (a wall's foot, a hull, a crate, sandbags) is
    // marked solid where it stands; where the terrain itself is seen and nothing over it, the mark is taken off (a
    // tank that drove off); unseen, a mark fades in about a minute.
    Diligent::ITextureView* depth = nullptr;
    float to_ndc[4] = {};
    float inverse[3][3] = {};
    float depth_near = 0.1f;
    Diligent::ITextureView* terrain = nullptr;
    float terrain_scale[4] = {}, terrain_offset[4] = {}, terrain_uv[4] = {};
    float terrain_size = 0.0f, terrain_decode = 0.0f, sea_floor = -1e9f;  // sea_floor: the sea's height (the ground under it counts as it)
    uint32_t width = 0, height = 0;
  };

  // The shader's source (for the offline compile check).
  static const char* ShaderSource();
  bool Initialize(Diligent::IRenderDevice* device);
  bool Ready() const { return ready_; }
  // One step, recorded on the context.
  void Advance(Diligent::IDeviceContext* context, const Step& step, const Pusher* pushers, int count);
  // The field now (null before the first step), and its place: the world's x and z of its first texel's corner.
  Diligent::ITextureView* View() const { return view_; }
  const float* origin() const { return origin_; }
  // What stands in the fog (R16F, 0 to 1; the field's place), for a developer's view.
  Diligent::ITextureView* Solid() const {
    return solid_[solid_index_] ? solid_[solid_index_]->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
  }

 private:
  Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso_;
  Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> field_[2];
  // What stands in the fog: carried to the field's new place and faded, then marked from the view; read by the step.
  Diligent::RefCntAutoPtr<Diligent::IPipelineState> carry_pso_, mark_pso_;
  Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> carry_srb_, mark_srb_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> mark_constants_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> solid_[2];
  Diligent::RefCntAutoPtr<Diligent::ITexture> no_height_;
  Diligent::RefCntAutoPtr<Diligent::ITextureView> terrain_;  // the terrain's heightmap as last told, and how it lies
  float terrain_k_[4][4] = {};
  Diligent::RefCntAutoPtr<Diligent::ITexture> seen_;  // this frame's marks (bit 0 something stands there, bit 1 bare ground seen)
  int solid_index_ = 0;
  Diligent::ITextureView* view_ = nullptr;
  int index_ = 0;
  float origin_[2] = {};
  bool placed_ = false, ready_ = false;
};

}  // namespace native_x_renderer
