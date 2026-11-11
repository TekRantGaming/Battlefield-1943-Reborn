// NativeXRenderer - screen space global illumination: one bounce of light from the lit scene colour, added to the
// scene's colour at the end of the opaque draws (internal; the design and the integration: docs/ssgi_port_notes.md).
//
// A port of King Kong Recompiled's screen space GI (its backend/ssgi.*, scene_gi.*, gi_config.*, game_gi.cpp,
// shaders/ssgi.hlsl and docs/gi.md; Copyright (c) 2026 TekRantGaming and contributors, MIT:
// thirdparty/king-kong-recompiled/LICENSE) from NVRHI and HLSL to DiligentCore and GLSL. The gather is a visibility
// bitmask over GTAO's slices (Therrien, Levesque and Gilet, "Screen Space Indirect Lighting with Visibility
// Bitmask", 2023); the depth passes are XeGTAO's (Intel, MIT, ssgi_shaders.h). All compute, in host pixels of the
// scene's targets, so it follows the render scale (D37):
//   1. the scene colour copied (the composite writes over it; every pass reads the copy);
//   2. XeGTAO's three depth passes on images of its own (KK's GI owns a second Gtao and runs only its Prepare,
//      unsmoothed): the stored depth into a working image padded to a multiple of 16, view depth with four smaller
//      mips, normals rebuilt from the depth;
//   3. the source light: per working pixel the mean of its host texels without the sky, in linear light (gamma
//      2.2), with KK's sunlit estimate (colour above the frame's ambient lit ceiling gets a bounded boost), and four
//      smaller mips;
//   4. the gather: per working pixel 2 slices x 6 steps per side (Low, Medium) or 4 x 10 (High) within 3, 5 or 8
//      world units, 32 sectors a slice: rgb the light from what hides the sky, a the share nothing hides;
//   5. without a temporal upscaler: this frame blended into last frame's (new light 0.20), reprojected by the camera
//      vote (at least 2 votes and half of the offers, KK's rule);
//   6. 2 (Low, Medium) or 3 (High) edge aware a-trous blur passes, steps 1, 2, 4;
//   7. a full screen triangle over the picture: colour + albedo x bounce x 3 x intensity in linear light, the
//      positive part at most 75 % of the receiver's own light, faded out over the last quarter of the distance limit
//      (16, 32, 64 m or none), alpha untouched, the sky untouched.
// Half resolution for Low and Medium; High is full resolution up to a render scale of 1.5 and half above.
//
// Targets are never remembered by pointer across frames (KK's commit 4fdb472d): the depth and colour are bound again
// at every Apply, and ForgetTargets drops the bindings that still hold the last ones and the history.

#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "Query.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"
#include "TextureView.h"

#include "camera_vote.h"
#include "gtao.h"
#include "scale.h"

namespace native_x_renderer {

// The settings (ssgi.cpp defines them as cvars; FromCvars reads them, every frame).
struct SsgiSettings {
  int mode = 1;      // native_gi_mode: 0 off, 1 on, 2 the GI only view (the bounce light alone, KK's debug view 1)
  int quality = 2;   // native_gi_quality: 1 Low, 2 Medium, 3 High (KK's levels)
  int distance = 2;  // native_gi_distance: 1 Near 16 m, 2 Medium 32 m, 3 Far 64 m, 4 Full (no limit)
  float intensity = 1.0f;  // native_gi_intensity: 1 normal, 4 strong (KK's calibrated x3 is applied on top)
  enum class Resolution { kAuto, kFull, kHalf };
  Resolution resolution = Resolution::kAuto;  // native_gi_resolution (KK's native_gi_quality): the working size
  bool temporal = true;  // native_gi_temporal: the local temporal accumulation
  // Not a cvar: the caller sets it when DLSS or FSR 3.1 runs on the view. Their history then smooths the noise, which
  // rotates every frame, and there is no local history (the camera vote is theirs, later in the view).
  bool temporal_upscaler = false;
  bool gpu_time = false;  // native_gi_gpu_time: duration queries around the GI's work, logged every 600 frames
  // KK's tuning: 0 keeps the level's values.
  float radius = 0.0f;      // native_gi_radius: world units (metres); 0, the level's 3, 5 or 8
  float thickness = 0.0f;   // native_gi_thickness: world units; 0, a fifth of the radius, 0.25 to 1
  float albedo = 0.75f;     // native_gi_albedo: the brightest albedo the composite assumes (a picture value)
  float occlusion = 0.0f;   // native_gi_occlusion: with the AO off, the hidden flat ambient taken away (0 to 1)
  int debug = 0;            // native_gi_debug: 1 the bounce light, 2 the change on grey, 3 the visibility
  // This port's, for a game without a known ambient constant or sun (Battlefield 1943 today):
  float ambient = 0.25f;    // native_gi_ambient: the flat ambient light as a grey picture value (KK's default)
  float gamma = 2.2f;       // native_gi_gamma: the scene colour's encoding
  float sunlit = 0.65f;     // native_gi_sunlit: KK's sunlit source restoration (0: the plain lit colour)

  static SsgiSettings FromCvars();
  static Resolution ParseResolution(const std::string& name);
  static const char* ResolutionName(Resolution resolution);
};

// The work of a level (KK's GiParamsFor, docs/gi.md "Levels").
struct SsgiLevel {
  uint32_t slices = 0;   // directions per pixel
  uint32_t steps = 0;    // samples per direction and side
  uint32_t denoise = 0;  // edge aware blur passes (each twice as wide)
  bool half = true;      // working resolution with native_gi_resolution=auto (High: full up to a scale of 1.5)
  float radius = 0.0f;   // world units
};

class Ssgi {
 public:
  // Compiles the passes' shaders and creates their pipelines (the composite's per colour format, on first use).
  // False if that failed (logged): Apply then does nothing.
  bool Initialize(Diligent::IRenderDevice* device);
  bool ready() const { return ready_; }

  // The settings for the next Apply calls (live: once a frame is fine).
  void Configure(const SsgiSettings& settings);
  const SsgiSettings& settings() const { return settings_; }

  struct Inputs {
    // The scene's depth target, as targets.cpp creates it (D24_UNORM_S8_UINT or D32_FLOAT_S8X24_UINT, with
    // BIND_SHADER_RESOURCE): its default shader resource view reads the depth.
    Diligent::ITexture* depth = nullptr;
    // A render target view of the scene's colour target (the same host size as the depth), written in place, and
    // that target's texture, copied first and sampled for the lit scene (nullptr: color->GetTexture()).
    Diligent::ITextureView* color = nullptr;
    Diligent::ITexture* color_texture = nullptr;
    // The 3D view's picture in host pixels of those targets (its viewport); clip x -1 at its left edge, clip y +1 at
    // its top (D3D's clip space, as the game's matrices give it).
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
    GtaoProjection projection;
    // The viewport's depth range (DrawPath::TakeProjection): stored depth = z_min + (z_max - z_min) * clip z / w.
    float z_min = 0.0f, z_max = 1.0f;
    RenderScale scale;  // the targets' render scale (native_gi_resolution=auto)
    uint64_t frame = UINT64_MAX;  // the frame's number: the noise's rotation, the history's continuity
    // The camera vote's result for this view (CameraVote::Result, column form: last clip = r x this clip), for the
    // local temporal accumulation; used when at least 2 draws and half of the offers agreed (KK's GI rule).
    const Mat4d* reprojection = nullptr;
    uint32_t votes = 0, offers = 0;
    // The ambient occlusion ran at this moment (either method): the GI then only adds light (occlusion 0), KK's rule.
    bool ao_on = false;
    // The frame's flat ambient light (rgb, picture values: a lighting constant such as KK's g_vAmbientColor), or
    // nullptr for native_gi_ambient's grey.
    const float* ambient = nullptr;
  };
  // Adds the bounce light into in.color (or shows it, native_gi_mode 2). Ends the render pass and leaves no target
  // bound: the renderer binds its targets again afterwards (DrawPath::TargetsChanged). False when it did nothing (off,
  // not ready, or inputs it cannot use; logged once).
  bool Apply(Diligent::IDeviceContext* context, const Inputs& in);

  // Drops the bindings that still hold the last depth target, and the history. Call it wherever targets are retired
  // or get a new texture, between frames. The working images are kept; they are resized at the next Apply if needed.
  void ForgetTargets();

  // The last result before the composite: RGBA16F at the working size (rgb the bounce light in linear light, a the
  // visibility), and its working step (1 full, 2 half resolution).
  Diligent::ITexture* result() const { return result_; }
  uint32_t step() const { return step_; }

  // Applications since the last call, for the log.
  uint32_t TakeApplied() {
    uint32_t applied = applied_;
    applied_ = 0;
    return applied;
  }

  static SsgiLevel LevelFor(int quality);
  // native_gi_distance in metres: 16, 32, 64, or 0 (no limit).
  static float DistanceMetres(int distance);

  static constexpr uint32_t kRadianceMips = 5;
  // The largest gather radius on the screen, as a share of the picture's height (working pixels).
  static constexpr float kMaxScreenRadius = 0.45f;
  // KK's task 066 calibration of intensity 1 (in the composite constant only).
  static constexpr float kCalibration = 3.0f;
  // The new light's weight in the history (KK's task 071).
  static constexpr float kNewWeight = 0.20f;
  // The largest share of a pixel's light the composite may take away (only with native_gi_occlusion above 0).
  static constexpr float kDarkest = 0.35f;
  // KK's GI Gtao runs with ao_strength 1: its depth prefilter's weights use 6 near-plane distances.
  static constexpr float kPrefilterRadiusInNearPlanes = 6.0f;

 private:
  struct Pass {
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
  };
  struct Binding {
    const char* name;
    Diligent::IDeviceObject* object;
  };
  // A compute pass: its GLSL (after "#version 450" and the common part), the textures it samples with a point clamp
  // sampler, those with a linear one, and the storage images it writes, all dynamic variables.
  bool CreateComputePass(Pass& pass, const char* name, const char* body, std::initializer_list<const char*> textures,
                         std::initializer_list<const char*> linear_textures, std::initializer_list<const char*> images);
  // The composite for a colour format.
  Pass* CompositePass(Diligent::TEXTURE_FORMAT format);
  bool EnsureImages(uint32_t padded_w, uint32_t padded_h);
  bool EnsureCopy(const Diligent::TextureDesc& color);
  void ReleaseBindings();
  // Binds `bindings` to the pass's SRB (made again after ForgetTargets) and dispatches groups_x x groups_y groups.
  bool Dispatch(Diligent::IDeviceContext* context, Pass& pass, std::initializer_list<Binding> bindings,
                uint32_t groups_x, uint32_t groups_y);
  void WriteConstants(Diligent::IDeviceContext* context, const void* data, size_t size);
  void TimerBegin(Diligent::IDeviceContext* context);
  void TimerEnd(Diligent::IDeviceContext* context);

  Diligent::IRenderDevice* device_ = nullptr;
  bool ready_ = false;
  SsgiSettings settings_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
  Pass prepare_, prefilter_, normals_, radiance_pass_, gather_, temporal_, denoise_;
  Diligent::RefCntAutoPtr<Diligent::IShader> composite_vs_, composite_ps_;
  std::map<Diligent::TEXTURE_FORMAT, Pass> composite_passes_;

  // The working images (padded_w_ x padded_h_), with the views the passes bind, and the copy of the colour.
  uint32_t padded_w_ = 0, padded_h_ = 0, step_ = 1;
  Diligent::RefCntAutoPtr<Diligent::ITexture> raw_, depth_mips_, normal_map_, radiance_, gi_a_, gi_b_, history_,
      history_depth_, copy_;
  std::array<Diligent::RefCntAutoPtr<Diligent::ITextureView>, 5> depth_mip_uav_, radiance_mip_uav_;
  Diligent::RefCntAutoPtr<Diligent::ITextureView> depth_mip0_srv_;
  Diligent::ITexture* result_ = nullptr;

  // What the history was made with: it is used only by the next frame with the same of all of these.
  struct HistoryKey {
    int quality = 0, distance = 0;
    uint32_t step = 0;
    float radius = 0, thickness = 0, gamma = 0, sunlit = 0;
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
    float z_min = 0, z_max = 0;
    bool operator==(const HistoryKey& o) const;
  };
  uint64_t history_frame_ = UINT64_MAX;
  HistoryKey history_key_;
  GtaoProjection history_projection_;
  uint64_t history_frames_ = 0;  // frames that used history, for the log

  // native_gi_gpu_time: a duration query around each Apply, read when it is reused kQueries applications later.
  static constexpr uint32_t kQueries = 4;
  std::array<Diligent::RefCntAutoPtr<Diligent::IQuery>, kQueries> queries_;
  std::array<bool, kQueries> query_pending_{};
  uint32_t query_index_ = 0;
  bool query_active_ = false, queries_failed_ = false;
  double timer_total_ms_ = 0.0, timer_max_ms_ = 0.0;
  uint32_t timer_frames_ = 0, timer_late_ = 0;

  uint32_t applied_ = 0;
  uint64_t computed_ = 0;
  bool warned_inputs_ = false;
};

}  // namespace native_x_renderer
