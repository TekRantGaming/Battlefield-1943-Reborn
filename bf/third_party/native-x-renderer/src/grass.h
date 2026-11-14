// NativeXRenderer - grass on the terrain (a developer prototype, off unless native_grass is set; internal).
//
// Battlefield 1943's terrain (Frostbite 1) is drawn in patches by one vertex shader that reads a 16-bit integer grid
// position per vertex and the height from a 2048 x 2048 k_16_FLOAT heightmap (its only vertex texture):
//   uv = grid * c22.xy + c22.zw;  world = (grid.x, height(uv), grid.y) * c20.xyz + c21.xyz;  clip = c12..c15 . world.
// DrawPath::CaptureTerrain keeps, from the main view's first terrain draw of the frame, those constants, the heightmap's
// host view, the draw's host viewport, scissor, depth test and NDC transform. At the end of the opaque draws (the
// ambient occlusion's moment, D132), Grass::Apply places tufts on a world-aligned grid of cells around the camera
// (one stable hash per cell: position, size, kind), a compute pass reading the heightmap (height and slope) and the
// scene's colour and depth at each tuft's root (the lit, shadowed and fogged ground the game drew there: the tuft's
// colour, and whether the root is hidden); then an instanced draw of three crossed cards per tuft (the game's own plant
// pictures, D145, D146), drawn into the scene's colour and depth with the terrain draw's viewport and depth test,
// dissolved where they thin out with distance. Each ring's tuft is the same plant as the first ring's it stands for
// (D146). No grass under water: the finished scene's water against a snapshot of the scene (MarkWater, D146).
// Where grass grows (D151): flat enough (heightmap slope), and on the level's ground marked grass in the game's own
// map of its ground (GrassMap, from the level's files), away from its edges: decided from the world alone, the same
// whatever the view, so a tuft never comes, goes or moves as the camera does (only with distance). Without that map,
// the ground's colour on screen green enough (D144 to D148).

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

#include "Buffer.h"
#include "DeviceContext.h"
#include "GraphicsTypes.h"
#include "PipelineState.h"
#include "Query.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"
#include "TextureView.h"

namespace native_x_renderer {

// The frame's terrain as its draws had it (DrawPath::CaptureTerrain).
struct TerrainFrame {
  bool valid = false;
  float wvp[4][4] = {};  // c12..c15: clip = row . (world, 1)
  float scale[4] = {}, offset[4] = {}, uv[4] = {};  // c20, c21, c22
  float ndc_scale[3] = {}, ndc_offset[3] = {};
  Diligent::Viewport viewport;
  Diligent::Rect scissor;
  uint8_t depth_func = 0;  // Diligent's COMPARISON_FUNCTION
  bool depth_clip = true;  // false: depth clamped (the world's near plane is 21 m; nearer ground is clamped)
  Diligent::RefCntAutoPtr<Diligent::ITextureView> height;
  uint32_t height_size = 0;
  bool height_unorm = false;  // guest k_16: the host texture holds its bits as R16_FLOAT (grass.cpp)
  uint64_t vertex_shader = 0;
  uint32_t draws = 0;  // terrain draws this frame
  // The depth partitions (Battlefield 1943 draws the terrain with one projection and viewport depth range for
  // 0.1 to 7.5 m, [1, 0.0133], and others further away, the stored depth being 0.1 / distance in all): each its own
  // world-view-projection and viewport. The grass is drawn once in each, clipped to its depth range.
  struct Partition {
    float wvp[4][4] = {};
    Diligent::Viewport viewport;
    Diligent::Rect scissor;
    float ndc_scale[3] = {}, ndc_offset[3] = {};
    uint32_t draws = 0;
  };
  std::array<Partition, 4> partitions;
  uint32_t partition_count = 0;
  void Reset() {
    valid = false;
    height.Release();
    draws = 0;
    partition_count = 0;
  }
};

// D169: the sky dome's draw (Battlefield 1943's, its vertex shader 00AB67BFF5297087): its camera at c0 to c3, rows
// (clip = row . (direction, 1), no translation: the dome goes with the eye), drawn whichever way the view looks (the
// terrain's camera is missing when it looks up at the sky), for the night sky's directions.
struct SkyCamera {
  bool valid = false;
  float m[4][4] = {};
  Diligent::Viewport viewport;
  float ndc_scale[3] = {}, ndc_offset[3] = {};
  // Toward the game's sun: its dome's pixel shader constant 9 is the light's direction (from the sun, unit), the one its
  // shadows and the sea's glint follow (research run niwo3: a moon placed there rose over the glint).
  bool sun_valid = false;
  float sun[3] = {};
};

class Grass {
 public:
  bool Initialize(Diligent::IRenderDevice* device);
  // native_grass now.
  static bool On();
  // Into `color` and `depth` (the scene's targets, at the AO's moment). True if drawn.
  bool Apply(Diligent::IDeviceContext* context, const TerrainFrame& terrain, Diligent::ITexture* color,
             Diligent::ITextureView* color_rtv, Diligent::ITexture* depth, Diligent::ITextureView* depth_dsv,
             uint64_t frame);
  // At the finished scene's copy (after the water and the other transparent draws): where the water covered the
  // ground this frame, for the next frame's placement (no grass under water). `finished` is that copy, `depth` the
  // scene's depth target (null: the one the snapshot was made from). True if made.
  bool MarkWater(Diligent::IDeviceContext* context, Diligent::ITexture* finished, Diligent::ITexture* depth);

 private:
  struct Pass {
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
    bool plants = false;  // its binding has the plants' pictures
  };
  bool LoadPlants(Diligent::IRenderDevice* device);
  Pass* DrawPass(Diligent::TEXTURE_FORMAT color, Diligent::TEXTURE_FORMAT depth, uint8_t depth_func, bool depth_clip,
                 bool depth_write);
  void WriteConstants(Diligent::IDeviceContext* context, const void* constants, size_t size);
  void TimerBegin(Diligent::IDeviceContext* context);
  void TimerEnd(Diligent::IDeviceContext* context);

  Diligent::IRenderDevice* device_ = nullptr;
  bool ready_ = false;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> instances_;  // this frame's tufts in view (appended)
  // Each cell's state from frame to frame (where grass grows, its colour), the indirect draw's arguments, the counts.
  Diligent::RefCntAutoPtr<Diligent::IBuffer> state_, args_, stats_, stats_read_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> seen_;  // each tuft's last frame, for counting pops (native_grass_pops)
  // D151: the level's grass map (GrassMap) as a texture (each cell's distance to ground that is not grass), a 1 x 1
  // stand in without one, and each ring's average ground colour of the tufts seen (the last frame's and this one's):
  // the colour of a tuft whose root has not been seen yet.
  Diligent::RefCntAutoPtr<Diligent::ITexture> map_, map_none_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> average_;
  uint64_t map_version_ = 0;
  int map_state_ = 0;  // -1 being read, 0 none known, 1 the map
  float map_info_[6] = {};  // x0, z0, cell, width, height, grass cells
  // D152: every tuft on the map, laid out once (place, size, plant) and each one's colour from frame to frame; how many
  // (counted on the GPU, read back for the log); the two passes; what the layout was made from.
  Diligent::RefCntAutoPtr<Diligent::IBuffer> all_, all_state_, all_count_, all_count_read_;
  Pass all_gen_, all_cull_;
  std::tuple<uint64_t, float, float, float, float, float> all_key_{};
  bool all_made_ = false, all_count_pending_ = false;
  uint64_t all_count_read_at_ = 0;
  uint32_t all_tufts_ = 0;

  std::tuple<uint32_t, uint32_t, float, float, void*> state_layout_{};
  uint32_t state_stamp_ = 0, state_resets_ = 0, state_resets_logged_ = 0;
  // The plants' pictures (native_grass_textures): kinds of plant, each its average colour and width over height.
  Diligent::RefCntAutoPtr<Diligent::ITexture> plants_, placeholder_;
  uint32_t plant_kinds_ = 0, plant_tries_ = 0;
  float kind_info_[16][4] = {};
  uint64_t stats_read_at_ = 0;
  uint32_t stats_frames_ = 0;
  Diligent::RefCntAutoPtr<Diligent::IShader> place_cs_, draw_vs_, draw_ps_;
  Pass place_;
  // The water's picture (MarkWater): the scene after the grass at half size, then where the water covered it.
  Pass snap_, water_pass_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> snap_color_, snap_depth_, water_;
  uint32_t water_w_ = 0, water_h_ = 0, snap_scene_w_ = 0, snap_scene_h_ = 0;
  bool snapped_ = false, water_logged_ = false;
  Diligent::RefCntAutoPtr<Diligent::ITexture> snap_depth_texture_;  // the scene's depth target at the snapshot
  uint64_t snap_frame_ = 0, water_frame_ = 0;
  std::vector<uint8_t> snap_constants_;
  // A ground height asked for (GrassProbe): read back from the counts' buffer.
  Diligent::RefCntAutoPtr<Diligent::IBuffer> probe_read_;
  bool probe_copied_ = false;
  uint64_t probe_read_at_ = 0;
  std::map<std::tuple<int, int, int, int>, Pass> draw_passes_;
  uint64_t applied_ = 0;
  std::chrono::steady_clock::time_point last_apply_{};  // the last frame's placement (the decisions' pace in time)
  // GPU time (native_grass_gpu_time), as the AO's.
  static constexpr int kQueries = 4;
  std::array<Diligent::RefCntAutoPtr<Diligent::IQuery>, kQueries> queries_;
  std::array<bool, kQueries> query_pending_{};
  int query_index_ = 0;
  bool query_active_ = false, queries_failed_ = false;
  double timer_total_ms_ = 0.0, timer_max_ms_ = 0.0;
  uint32_t timer_frames_ = 0;
};

}  // namespace native_x_renderer
