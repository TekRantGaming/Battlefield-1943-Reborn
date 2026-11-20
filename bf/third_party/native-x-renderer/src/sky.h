// NativeXRenderer - a sky of its own: time of day, sun, moon, stars and volumetric clouds (internal; 14 Nov 2026).
//
// Battlefield 1943 Reborn, stage 1 of "day/night with ... sky, volumetric clouds, and new lighting system ... similar or
// better than how it's implemented in Beachhead": the sky and the clouds of the user's own Beachhead project
// (its game/world: sky_color.gdshaderinc, cloud_core.gdshaderinc, atmosphere_view.gd's
// time of day; crates/sim/src/cloudnoise.rs's noise), brought over as passes on the view's colour:
//   - where the view's depth holds nothing (the game's own sky), the sky is drawn anew: a gradient from the haze at the
//     horizon to the zenith, the sun's glow and disc, at night the stars and the moon, and thin cirrus;
//   - the clouds are a layer between two altitudes, marched along each pixel's ray through a density made of tiling
//     noise volumes, lit toward the sun or the moon. They are marched at half the view's size and laid over the sky;
//   - the time of day turns the sun and the moon and blends the sky's colours from day through dusk to night.
// The pass runs at the end of the view's opaque draws (the ambient occlusion's moment), so the game's water, smoke and
// the first person weapon are drawn over it. What the game lights itself (the ground, the buildings) is not changed
// here.
//
// The fog (14 Nov 2026: "let's do the ground fog, distance fog, rain shafts, storm mist, and lightning in clouds"), a
// second pass (Fog) laid over the view once the game's water is drawn, by the view's depth:
//   - the distance fog: a haze that thins with the height above the sea, from the eye to what is seen;
//   - the ground fog: a low layer over the sea and the shore in drifting patches, marched;
//   - the storm's mist: under heavy weather the haze thickens and a taller low layer comes in;
//   - the rain shafts: far curtains of rain from the cloud base down, under the thicker cloud, slanted by the wind;
//   - the rain: under heavy weather, streaks falling around the eye (hidden by what is nearer), what is rained on a
//     little darker, and rings and small splashes where drops land on what lies flat. Not under cover: what the
//     game's shadow map shadows (Inputs::cover) gets none;
//   - the lightning: under a storm, flashes inside the cloud layer (the clouds' march takes their light; the fog
//     glows toward them). When and where is told by the world's clock alone, so every player sees the same ones.
// The sea's height is found from the view itself (the lowest flat thing seen: a small pass read back now and then).
//
// Clouds in the world ("our volumetric clouds are outside of the world, so you can't fly through them in the map",
// 15 Nov 2026): the layer is a model of itself at the game's scale, its base at Inputs::cloud_height over the sea
// (everything about it, its depth and its patterns, smaller by the same measure, so from the ground it looks as it
// did), low enough for the planes. The march also tells where along each ray its cloud lies, and a third pass
// (CloudsOver, with the fog's) lays the cloud over what the view's depth holds as far as that is inside or behind it:
// a plane in a cloud fades into it, the ground under a cloud seen from above is hidden by it.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <thread>
#include <vector>

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Shader.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"

#include "storm.h"

namespace native_x_renderer {

class Sky {
 public:
  struct Inputs {
    Diligent::ITexture* color = nullptr;      // the view's colour: its sky pixels are drawn anew
    Diligent::ITextureView* depth_srv = nullptr;
    bool old_cloud_bands = false;    // BF_CLOUD_BANDS_AB (tests): the bands' noises worked out for every sample
    bool old_cloud_reach = false;    // BF_CLOUD_REACH_AB (tests): the column's depth looked up for every sample
    bool old_cloud_density = false;  // BF_CLOUD_DENSITY_AB (tests): the cloud's shape looked up for every sample
    int fog_parts_off = 0;  // BF_FOG_PARTS (tests): parts of the fog pass off (8, 16, 32, 64: sky.cpp's kFogPS)
    bool old_rain_order = false;  // BF_FOG_AB (tests): the rain's cover looked up for every pixel, as before
    bool checker_clouds = false;  // half the cloud texels marched a frame (native_x_renderer_cloud_checkerboard)
    bool skip_hidden_clouds = true;  // the clouds' march passes over the rays the scene hides (false: all, for comparisons)
    uint32_t width = 0, height = 0;           // the view's part of the colour, in its pixels
    // The camera, as night.h's: the view's 0 to 1 coordinate to NDC (uv * xy + zw), the world from clip x, y and w
    // (world = inverse x ((x, y, w) - t), rows), and the eye.
    float to_ndc[4] = {};
    float inverse[3][3] = {};
    float inverse_t[3] = {};
    float eye[3] = {};
    // Clip x, y and w from a world direction (the camera's matrix, its rows for x, y and w): where this frame's
    // clouds are in the next frame's view.
    float forward[3][3] = {};
    double hour = 13.0;         // the time of day, 0 to 24
    float sun_azimuth = 135.0f; // where the sun stands at noon, degrees from the world's +z toward +x
    float coverage = 0.45f;     // how much of the sky the clouds take, 0 to 1
    float exposure = 1.0f;      // the sky's light in the view's own units
    double seconds = 0.0;       // the world's time: the clouds drift with it
    double evolve = 0.0;        // the clouds' forming clock: Beachhead's, 1 a second at its full generation speed
    float weather = 0.0f;       // 0 fair to 1 heavy overcast: more and denser cloud, a greyer sky, less sun
    // D208: the hurricane (storm.h): the weather outside it (the clouds' cover away from it), and its two vectors for
    // the clouds (storm::kGlsl's a and b). `weather` is the weather where the eye is.
    float weather_outside = 0.0f;
    float match_weather = 0.0f;  // the match's own weather (the hurricane's stage): the lightning
    uint32_t world_snaps = 0;    // D221: counts the world's jumps to a new round (SnapWorld): each dries the last one's rain
    float storm_a[4] = {}, storm_b[4] = {};
    float fog = 0.0f;           // the distance fog, 0 to 1
    float ground_fog = 0.0f;    // the ground fog, 0 to 1
    float wind = 0.3f;          // 0 calm to 1 a gale: how far the rain leans
    float depth_near = 0.1f;    // the view's stored depth is this over the distance along the view
    float cloud_height = 450.0f;  // the cloud base over the sea, metres (the layer as Beachhead has it: 1500)
    float cloud_lift = 0.0f;      // a developer's check: the clouds seen as from this much higher
    // The rain's cover ("Make it so rain is blocked when indoors or under cover", 14 Nov 2026): the game's shadow map
    // (DrawPath::ShadowMap: a layer for each of three cascades, each with its transform from the world). What it
    // shadows is under something along the light, and gets no rain. Null: no cover known.
    Diligent::ITextureView* cover = nullptr;
    float cover_m[3][3][4] = {};
    int debug = 0;              // 1: the sky's pixels in one flat colour (which pixels are taken)
  };
  ~Sky();
  // Starts making the noise volumes (on a thread of its own: a few seconds).
  bool Load(Diligent::IRenderDevice* device);
  // False until the noise is made, and when the pass could not be set up.
  bool Apply(Diligent::IDeviceContext* context, const Inputs& in);
  // The fog over the view's colour by its depth (debug 4: the distance in bands, 5: the height over the sea).
  bool Fog(Diligent::IDeviceContext* context, const Inputs& in);
  // This frame's clouds (Apply's) over what the view's depth holds, where that is inside or behind them.
  bool CloudsOver(Diligent::IDeviceContext* context, const Inputs& in);
  // Rain on the lens ("a quality camera or glass droplet effect", 14 Nov 2026), over the finished view: beads that
  // come and go and drops that run down, each bending the picture behind it. How wet the lens is builds up while the
  // eye stands in the rain and dries under cover (Fog keeps it). Written for this project: the well-known Shadertoy
  // ones are CC BY-NC-SA, not ours to take.
  // D191: the window glass's reflection, drawn with the lens: the panes' depth (DrawPath::DrawGlassMask), the world's
  // depth from before the first person weapon and the view's depth at its end, each with its share of its texture.
  struct GlassInputs {
    Diligent::ITextureView* mask = nullptr;
    Diligent::ITextureView* world_depth = nullptr;
    Diligent::ITextureView* view_depth = nullptr;
    float world_uv[2] = {1.0f, 1.0f}, view_uv[2] = {1.0f, 1.0f};
    bool cockpit = false;  // D195: the player sits in a vehicle: the drops on its glass only (the depths, no mask)
  };
  bool Lens(Diligent::IDeviceContext* context, Diligent::ITexture* color, uint32_t width, uint32_t height, bool drops = true,
            const GlassInputs* glass = nullptr);
  // Water on the lens without rain too ("when running across or jumping into the water", 14 Nov 2026): running with
  // the eye under 1.1 m over the sea (wading) splashes it, drying in 3 seconds; the eye at the water (jumped in, gone
  // under) soaks it, drying in 5.
  float lens_wet() const { return std::max(lens_wet_, splash_wet_); }
  float sway() const { return sway_; }
  // D190: the sky's zenith and horizon as the view shows them (exposure in), greyed by the clouds' cover, from the
  // last frame's constants; false before there are any.
  bool SeaSkyColours(float zenith[3], float horizon[3]) const;  // D189: a strong storm's sway now (0 to 1)
  // D205: the sea's height as read from the view (ReadSea); false before it is known.
  bool SeaLevel(float* sea) const {
    if (sea_known_) *sea = sea_;
    return sea_known_;
  }
  // A wet copy of a texture of the first person weapon's (DrawPath::WetRequest), into `to` (RGBA8, the same size):
  // darker, with beads of water, `count` of them showing by `show`; tint: drawn red instead (a developer's check).
  bool WetCopy(Diligent::IDeviceContext* context, Diligent::ITextureView* from, Diligent::ITexture* to, float count,
               float show, bool tint, double seconds, bool skin, bool vehicle = false, bool relief_only = false);
  // How wet what is held in the hand is: it builds up in twelve seconds of rain on the eye (at once when the eye
  // goes under the sea) and dries in thirty. How many beads (the most there were), and how strongly they show.
  float weapon_wet_count() const { return weapon_peak_; }
  float weapon_wet_show() const { return weapon_peak_ > 0.0f ? std::clamp(weapon_wet_ / weapon_peak_, 0.0f, 1.0f) : 0.0f; }
  // Whether these inputs give the fog anything to draw.
  static bool FogWanted(const Inputs& in);

 private:
  bool MakeTextures();
  bool Ready();
  void Fill(const Inputs& in, struct SkyConstantBlock& k);
  void FillBolt(const Inputs& in, struct SkyConstantBlock& k, float weather, float sea, float scale);  // D192
  uint64_t said_bolt_ = 0;
  void ReadSea(Diligent::IDeviceContext* context);
  Diligent::IRenderDevice* device_ = nullptr;
  Diligent::RefCntAutoPtr<Diligent::IShader> vs_, cloud_ps_, resolve_ps_, sky_ps_, fog_ps_, sea_ps_, lens_ps_;
  Diligent::RefCntAutoPtr<Diligent::IPipelineState> lens_pso_;
  Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> lens_srb_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> lens_copy_;
  Diligent::TEXTURE_FORMAT lens_format_ = Diligent::TEX_FORMAT_UNKNOWN;
  float lens_wet_ = 0.0f, lens_rain_ = 0.0f, splash_wet_ = 0.0f, splash_dry_ = 3.0f;
  float sway_ = 0.0f;
  float edge_ = 0.0f;  // D194: the storm's blurred edges, apart from the shake
  Diligent::RefCntAutoPtr<Diligent::ITexture> fog_scene_, no_scene_;  // D190: the scene before the fog
  // BF_FOG_CHECK (tests): the picture before the fog, the two ways' results read back, and the passes counted.
  Diligent::RefCntAutoPtr<Diligent::ITexture> fog_before_, fog_staging_[2];
  // BF_CLOUD_CHECK (tests): the march's two outputs read back, this way and the old.
  Diligent::RefCntAutoPtr<Diligent::ITexture> cloud_staging_[4];
  uint64_t cloud_check_count_ = 0;
  uint64_t fog_check_count_ = 0;
  std::vector<uint8_t> last_fill_;
 public:
  // D190: the sea's draws drawn again (DrawPath::DrawGlassMask, sea), for the fog pass's reflection: where the sea
  // is (its depth; 0 elsewhere), with its share of its texture. Null: the sea's plane alone.
  Diligent::ITextureView* sea_mask = nullptr;
  float sea_mask_uv[2] = {1.0f, 1.0f};
  // D190: the picture just after the sea was drawn (DrawPath::sea_after, of the fog's own size; null: none): the
  // reflection only where the picture is still that, not over what see-through was drawn over the sea since.
  Diligent::ITextureView* sea_after = nullptr;
  // D190: the world's depth before the sea was drawn (the night's copy, pixel for pixel with the fog's picture; null:
  // none): how deep the water is, the reflection kept off the shallows where the sea's own wash runs up the shore.
  Diligent::ITextureView* sea_floor = nullptr;
 private:  // D191: the last constants Fill made (the view's), for the glass's reflection
  // Drying fades every drop away together ("The drying effect looks like it's reversing the droplets. The screen
  // effect should just fade away entirely", 14 Nov 2026): how many drops there are stays at the most there were
  // (the peaks) while what is left of the wetness against it is how strongly they show.
  float lens_peak_ = 0.0f, splash_peak_ = 0.0f;
  float weapon_wet_ = 0.0f, weapon_peak_ = 0.0f;
  uint32_t world_snaps_ = 0;  // D221: the last Inputs::world_snaps seen (DryForNewRound)
  void DryForNewRound(const Inputs& in);
 public:
  bool whole_colour_ = false;  // the developer's check 98 (WetCopy)
 private:
  Diligent::RefCntAutoPtr<Diligent::IShader> wet_ps_;
  Diligent::RefCntAutoPtr<Diligent::IPipelineState> wet_pso_;
  Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> wet_srb_;
  float last_eye_[3] = {};
  bool last_eye_valid_ = false;
  double lens_seconds_ = 0.0;
  bool eye_open_ = true;  // the eye is not under cover (read with the sea's heights)
  std::chrono::steady_clock::time_point lens_at_{};
  Diligent::RefCntAutoPtr<Diligent::IPipelineState> cloud_pso_, resolve_pso_, sky_pso_, fog_pso_, sea_pso_;
  Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> cloud_srb_, resolve_srb_, sky_srb_, fog_srb_, sea_srb_;
  Diligent::TEXTURE_FORMAT fog_format_ = Diligent::TEX_FORMAT_UNKNOWN;
  // The sea's height: the view's lowest heights in a small target, copied out and read a few frames later.
  Diligent::RefCntAutoPtr<Diligent::ITexture> sea_target_, sea_staging_, no_cover_;
  uint32_t fog_frames_ = 0;
  int sea_wait_ = 0;
  std::deque<float> sea_seen_;
  float sea_ = 0.0f;
  bool sea_known_ = false;
  // The clouds over frames: each frame's march starts at another depth in each pixel, and the frames are blended
  // where the last one's clouds are seen now (by direction: they are far), which smooths the march's grain away.
  Diligent::RefCntAutoPtr<Diligent::ITexture> history_[2];
  // Where along each ray its cloud lies (the mean distance, half its extent and its beginning, each times how much
  // cloud there is, which is the third), marched and kept over frames with the clouds.
  Diligent::RefCntAutoPtr<Diligent::ITexture> where_, where_history_[2];
  Diligent::RefCntAutoPtr<Diligent::IShader> over_ps_;
  Diligent::RefCntAutoPtr<Diligent::IPipelineState> over_pso_;
  Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> over_srb_;
  Diligent::TEXTURE_FORMAT over_format_ = Diligent::TEX_FORMAT_UNKNOWN;
  bool clouds_fresh_ = false;  // marched in this frame's view, not yet laid over its depth
  float cloud_eye_[3] = {};
  bool cloud_eye_valid_ = false;
  int history_now_ = 0;
  bool history_valid_ = false;
  float last_forward_[3][3] = {};
  float last_to_ndc_[4] = {};
  uint32_t frames_ = 0;
  float lowest_eye_ = 1e9f;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> shape_, detail_, weather_, clouds_;
  Diligent::TEXTURE_FORMAT format_ = Diligent::TEX_FORMAT_UNKNOWN;
  // The noise, made on the thread: the shape volume's and the detail volume's mip levels, and the weather map.
  std::thread noise_thread_;
  std::atomic<bool> noise_ready_{false};
  std::vector<std::vector<uint8_t>> shape_mips_, detail_mips_;
  std::vector<uint8_t> weather_data_;
  bool failed_ = false, said_ = false, textures_made_ = false;
};

// What the sky's time of day does to the light on the ground: what the game's own daylight (its sun's colour, its
// sky's and its ground's light) is multiplied by. All 1 by day; at dusk the sun warms and sinks, at night the moon's
// dim blue takes its place.
struct SkyLight {
  float key[3], sky[3], ground[3];
  float painted[3];     // what the game's own painted sky is multiplied by (dimmer and warmer at dusk than `sky`)
  float sun_elevation;  // degrees; under 0 the sun is down
  float sun_turn;       // degrees the sun has gone around since noon
};
SkyLight SkyLightAt(double hour, float weather = 0.0f);

// The storm's lightning at this moment of the world's clock (the clouds' drift seconds): its light now (0: none), and
// where it is from the middle of the 4 km square the eye is in, metres.
struct SkyFlash {
  float light = 0.0f;
  float x = 0.0f, z = 0.0f;
};
SkyFlash SkyFlashAt(double seconds, float weather);
// D208: where strikes may fall now: the hurricane (storm.h), the middle of the eye's 4 km square, and where players are
// gathered (x, z pairs, at most 8).
void SetStrikeWorld(const storm::Storm& storm, float square_x, float square_z);
void SetStrikeGroups(const float* xz, uint32_t count);
// The player's own choice: no lightning at all (the storm's other weather stays).
void SetSkyLightning(bool on);
void SetCloudShadows(bool on);  // D214: the clouds' shadows on the ground
// D189: a strong storm's sway and blurred edges on the view (native_x_renderer_storm_sway).
void SetStormSway(bool on);
// D190: the sea's reflection of the renderer's own, in the fog pass (native_x_renderer_reflections).
void SetSeaReflection(bool on);
bool SeaReflection();

// The developer's shader check (aa.cpp's TestUtilityShaders): how many of its shaders failed to compile.
int TestSkyShaders();

}  // namespace native_x_renderer
