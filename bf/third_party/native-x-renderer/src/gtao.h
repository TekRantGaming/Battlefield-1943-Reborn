// NativeXRenderer - ambient occlusion: XeGTAO from the scene's depth, multiplied into the scene's colour (internal;
// the design and the integration: docs/gtao_port_notes.md).
//
// A port of King Kong Recompiled's GTAO (its backend/gtao.*, shaders/gtao.hlsl and docs/ao.md; Copyright (c) 2026
// TekRantGaming and contributors, MIT: thirdparty/king-kong-recompiled/LICENSE) from NVRHI and HLSL to DiligentCore
// and GLSL. XeGTAO itself is Intel's (MIT, gtao_shaders.h). All compute, in host pixels of the scene's targets, so it
// follows the render scale (D37):
//   1. the picture's stored depth into a working image padded to a multiple of 16 (no pass writes outside its
//      images; the padding repeats the picture's edge), every second pixel at half resolution;
//   2. XeGTAO's depth prefilter: view-space depth and its four smaller mips;
//   3. normals rebuilt from the depth (the game has no normal buffer), then optionally smoothed across each surface
//      (native_ao_normal_smoothing: two separable passes, in place);
//   4. XeGTAO's main pass: native_ao_samples 1 Low (2 slices of 2 steps), 2 Medium (3 x 3), 3 High (9 x 3);
//   5. two of XeGTAO's edge-aware denoise passes;
//   6. one R32F image over the picture (1 = no occlusion), upsampled with weights from the depth at half
//      resolution; stored depth at the far plane (the cleared sky) gets exactly 1;
//   7. without a temporal upscaler (native_ao_temporal), a local temporal filter: last frame's occlusion
//      reprojected by the camera vote (D118), rejected by depth, clamped to this frame's neighbourhood;
//   8. a full-screen triangle with a multiplicative blend (destination times source, alpha untouched, nothing
//      read back) into the scene's colour target; ao_mode 2 writes the occlusion itself.
// Half resolution with native_ao_quality=auto when the render scale is above 1.5 on either axis. Images are R32_FLOAT
// and R32_UINT only: every Vulkan device stores to those without shaderStorageImageExtendedFormats.
//
// Targets are never remembered by pointer across frames (KK's fix for the DLSS switch crash, its commit 4fdb472d):
// the depth and colour are bound again at every Apply, and ForgetTargets drops the bindings that still hold the last
// ones and the history that came from them, for whoever retires targets (a new render size).

#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>

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
#include "scale.h"

namespace native_x_renderer {

// The settings, as KK names them (gtao.cpp defines them as cvars; FromCvars reads them).
struct GtaoSettings {
  int mode = 1;              // ao_mode: 0 off, 1 on, 2 the occlusion only
  int strength = 3;          // ao_strength 1 to 3: XeGTAO's final power 1.6, 2.2, 3.0; radius 6, 8, 12 near planes
  int samples = 3;           // native_ao_samples: 1 Low (2 x 2), 2 Medium (3 x 3), 3 High (9 x 3)
  enum class Quality { kAuto, kFull, kHalf };
  Quality quality = Quality::kAuto;  // native_ao_quality: auto is half above a render scale of 1.5
  int normal_smoothing = 2;  // native_ao_normal_smoothing: 0 off, 1 low, 2 high
  bool temporal = true;      // native_ao_temporal: the local temporal filter (or the upscaler's history)
  // Not a cvar: the caller sets it when DLSS or FSR 3.1 runs on the view. Its history then smooths the noise, which
  // rotates every frame, and there is no local history.
  bool temporal_upscaler = false;
  bool gpu_time = false;     // native_ao_gpu_time: duration queries around the AO work, logged every 600 frames
  // KK's tuning: 0 (1 for radius_scale) keeps the values above.
  float radius = 0.0f;        // native_ao_radius: world units; 0, the strength's radius times the near plane
  float radius_scale = 1.0f;  // native_ao_radius_scale: times the radius from the camera
  float falloff = 0.0f;       // native_ao_falloff: XeGTAO's falloff range (0: its 0.615)
  float power = 0.0f;         // native_ao_power: XeGTAO's final power (0: from the strength)
  float fade = 0.0f;          // native_ao_fade: near-plane distances where the occlusion has faded out (0: none)

  // The cvars' values now (ao_mode and ao_strength by name, so that they work whichever module registered them).
  static GtaoSettings FromCvars();
  static Quality ParseQuality(const std::string& name);
  static const char* QualityName(Quality quality);
};

// The camera's projection, from a world-view-projection with a rigid world and view and one uniform scale (KK's
// ExtractAoProjection): with clip = M x position, the w row is view z, the z row a times it plus b, the x and y rows
// px and py times view x and y plus the off-centre terms times view z. The ratios of the rows' lengths give them.
struct GtaoProjection {
  float px = 0.0f, py = 0.0f;  // clip x = px * view x + cx * view z (1 / tan of half the field of view)
  float cx = 0.0f, cy = 0.0f;
  float a = 0.0f, b = 0.0f;    // clip z = a * view z + b, clip w = view z
  // Where clip z / w is 0 and 1, the nearer and the farther (reversed depth swaps them). 0 when there is none.
  float near_plane() const;
  float far_plane() const;
  bool valid() const;

  // From a matrix in column form (clip = m x position, rows m[r * 4 + c]): a draw's world-view-projection from the
  // camera vote's registers (CameraVote::MatrixAt, DrawPath::scene_start_matrix). False if it is not a perspective.
  static bool FromMatrix(const Mat4d& clip_from_position, GtaoProjection& out);
  // From the rows a game's view hook gives (BeginView: rows for a row vector, column c is m[c], m[4 + c], m[8 + c],
  // m[12 + c]), as the upscaler's CameraFromViewProjection takes them.
  static bool FromViewProjectionRows(const std::array<float, 16>& m, GtaoProjection& out);
};

class Gtao {
 public:
  // Compiles the passes' shaders and creates their pipelines (the apply pass's per colour format, on first use).
  // False if that failed (logged): Apply then does nothing.
  bool Initialize(Diligent::IRenderDevice* device);
  bool ready() const { return ready_; }

  // The settings for the next Apply calls (live: once a frame is fine).
  void Configure(const GtaoSettings& settings);
  const GtaoSettings& settings() const { return settings_; }

  struct Inputs {
    // The scene's depth target, as targets.cpp creates it (D24_UNORM_S8_UINT or D32_FLOAT_S8X24_UINT, with
    // BIND_SHADER_RESOURCE): its default shader resource view reads the depth.
    Diligent::ITexture* depth = nullptr;
    // A render target view of the scene's colour target (the same host size as the depth), multiplied in place.
    Diligent::ITextureView* color = nullptr;
    // The 3D view's picture in host pixels of those targets (its viewport); clip x -1 at its left edge, clip y +1 at
    // its top (D3D's clip space, as the game's matrices give it).
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
    GtaoProjection projection;
    // The viewport's depth range (DrawPath::jittered_z_min / max): stored depth = z_min + (z_max - z_min) * clip z / w.
    float z_min = 0.0f, z_max = 1.0f;
    RenderScale scale;  // the targets' render scale (native_ao_quality=auto)
    uint64_t frame = UINT64_MAX;  // the frame's number: the noise's rotation, the history's continuity
    // The camera vote's result for this view (CameraVote::Result, column form: last clip = r x this clip), for the
    // local temporal filter; used when at least 3 draws and half of the offers agreed (KK's rule).
    const Mat4d* reprojection = nullptr;
    uint32_t votes = 0, offers = 0;
  };
  // Works the occlusion out from in.depth and multiplies it into in.color (or shows it, ao_mode 2). Ends the render
  // pass and leaves no target bound: the renderer binds its targets again afterwards (DrawPath::TargetsChanged). False
  // when it did nothing (off, not ready, or inputs it cannot use; logged once).
  bool Apply(Diligent::IDeviceContext* context, const Inputs& in);

  // Drops the bindings that still hold the last depth target, and the history worked out from it. Call it wherever
  // targets are retired or get a new texture (a new render size, the reduced targets of an upscaling mode), between
  // frames. The working images are kept; they are resized at the next Apply if needed.
  void ForgetTargets();

  // The last result: R32F over the picture (1 = none), where it lies in its target, and its working step.
  Diligent::ITexture* occlusion() const { return output_; }
  int32_t origin_x() const { return origin_x_; }
  int32_t origin_y() const { return origin_y_; }
  uint32_t width() const { return out_w_; }
  uint32_t height() const { return out_h_; }
  uint32_t step() const { return step_; }

  // Applications since the last call, for the log.
  uint32_t TakeApplied() {
    uint32_t applied = applied_;
    applied_ = 0;
    return applied;
  }

  // The values a strength gives: the effect radius in near-plane distances (KK's legacy AO's 6, 8, 12) and XeGTAO's
  // final power (1.6, 2.2, 3.0; 2.2 is XeGTAO's own tuning).
  static float RadiusInNearPlanes(int strength);
  static float FinalPower(int strength);
  // Normal smoothing (1 low, 2 high): the largest radius in host pixels of a 1080-line picture (scaled with the
  // picture), the radius in view units as a share of the effect radius, and the angles between normals smoothed
  // fully and not at all. A tap's angle off the pixel's plane uses the same ramp, capped at kPlane*Degrees.
  struct Smoothing {
    float max_pixels_1080 = 0.0f;
    float radius_fraction = 0.0f;
    float full_angle_degrees = 0.0f;
    float max_angle_degrees = 0.0f;
  };
  static constexpr float kPlaneFullDegrees = 20.0f, kPlaneMaxDegrees = 40.0f;
  static Smoothing SmoothingFor(int level);

 private:
  struct Pass {
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
  };
  struct Binding {
    const char* name;
    Diligent::IDeviceObject* object;
  };
  // A compute pass: its GLSL (after "#version 450", `defines` and the common part), the textures it samples (each
  // with a point clamp sampler) and the storage images it writes, all dynamic variables.
  bool CreateComputePass(Pass& pass, const char* name, const std::string& defines, const char* body,
                         std::initializer_list<const char*> textures, std::initializer_list<const char*> images);
  // The apply pass for a colour format (mode 2: no blend).
  Pass* ApplyPass(Diligent::TEXTURE_FORMAT format, bool show);
  bool EnsureImages(uint32_t padded_w, uint32_t padded_h, uint32_t out_w, uint32_t out_h, bool temporal);
  // Binds `bindings` to the pass's SRB (made again after ForgetTargets) and dispatches groups_x x groups_y groups.
  bool Dispatch(Diligent::IDeviceContext* context, Pass& pass, std::initializer_list<Binding> bindings,
                uint32_t groups_x, uint32_t groups_y);
  void TimerBegin(Diligent::IDeviceContext* context);
  void TimerEnd(Diligent::IDeviceContext* context);

  Diligent::IRenderDevice* device_ = nullptr;
  bool ready_ = false;
  GtaoSettings settings_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
  Pass prepare_, prefilter_, normals_, smooth_h_, smooth_v_, main_low_, main_medium_, main_high_, denoise_,
      denoise_last_, output_pass_, temporal_;
  Diligent::RefCntAutoPtr<Diligent::IShader> apply_vs_, apply_ps_;
  std::map<std::pair<Diligent::TEXTURE_FORMAT, bool>, Pass> apply_passes_;

  // The working images (padded_w_ x padded_h_) and the result (out_w_ x out_h_), with the views the passes bind.
  uint32_t padded_w_ = 0, padded_h_ = 0, out_w_ = 0, out_h_ = 0, step_ = 1;
  int32_t origin_x_ = 0, origin_y_ = 0;
  Diligent::RefCntAutoPtr<Diligent::ITexture> raw_, depth_mips_, normal_map_, term_a_, term_b_, edges_, output_;
  std::array<Diligent::RefCntAutoPtr<Diligent::ITextureView>, 5> depth_mip_uav_;
  Diligent::RefCntAutoPtr<Diligent::ITextureView> depth_mip0_srv_;
  // The local temporal filter's images (only while it runs).
  Diligent::RefCntAutoPtr<Diligent::ITexture> spatial_, temporal_depth_, history_ao_, history_depth_;

  // What the history was made with: it is used only by the next frame with the same of all of these.
  struct HistoryKey {
    int mode = 0, strength = 0, samples = 0, normal_smoothing = 0;
    uint32_t step = 0;
    float radius = 0, radius_scale = 0, falloff = 0, power = 0, fade = 0;
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
    float z_min = 0, z_max = 0;
    bool operator==(const HistoryKey& o) const;
  };
  uint64_t history_frame_ = UINT64_MAX;
  HistoryKey history_key_;
  GtaoProjection history_projection_;
  uint64_t history_frames_ = 0;  // frames that used history, for the log

  // native_ao_gpu_time: a duration query around each Apply, read when it is reused kQueries applications later.
  static constexpr uint32_t kQueries = 4;
  std::array<Diligent::RefCntAutoPtr<Diligent::IQuery>, kQueries> queries_;
  std::array<bool, kQueries> query_pending_{};
  uint32_t query_index_ = 0;
  bool query_active_ = false, queries_failed_ = false;
  double timer_total_ms_ = 0.0, timer_max_ms_ = 0.0;
  uint32_t timer_frames_ = 0, timer_late_ = 0;

  uint32_t applied_ = 0;
  uint64_t computed_ = 0;  // all Apply calls that worked the occlusion out
  bool warned_inputs_ = false;
};

}  // namespace native_x_renderer
