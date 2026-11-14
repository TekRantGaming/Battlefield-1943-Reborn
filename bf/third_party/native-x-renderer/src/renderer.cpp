// NativeXRenderer - the renderer.
//
// With --native_x_renderer=true (read once, at startup), NativeXRenderer draws the game instead of the SDK's
// Xenos emulation:
//   - the SDK's command processor keeps executing the game's command buffer for synchronization only
//     (the SDK's recomp change 27: fences, interrupts, swaps, queries with fake sample counts), and
//     draws nothing on the host;
//   - the SDK's presenter is detached from the game's window, and NativeXRenderer presents there through
//     its own DiligentCore (Vulkan) swap chain. The SDK's ImGui overlays (its dialogs) are drawn over the frame
//     there (overlay.h, D75).
//
// Every D3D call is captured on the game's render thread, after the original ran, in the game's order, from
// the call's arguments and the state tracker (state.h), into a command (commands.h); the renderer's own thread
// runs the commands (D71): render targets, clears, resolves and presenting here, draws in draw.cpp. With
// native_x_renderer_thread off, each command runs right away on the game's thread.

#include "renderer.h"

#include "aa.h"
#include "gtao.h"
#include "grass.h"
#include "night.h"
#include "scope.h"
#include "ssgi.h"
#include "motion_blur.h"
#include "upscaler.h"
#include "fsr.h"
#include "xess.h"
#include "framegen.h"
#include "overlay_motion.h"
#include "blit.h"
#include "commands.h"
#include "device.h"
#include "draw.h"
#include "guest_memory.h"
#include "gamma.h"
#include "glsl.h"
#include "overlay.h"
#include "queries.h"
#include "scaling.h"
#include "internal.h"
#include "state.h"
#include "targets.h"

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/window.h>
#if !defined(_WIN32)
#include <SDL3/SDL.h>
#include <rex/ui/window_sdl.h>
#endif

#include "Query.h"
#include "RenderDeviceVk.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <map>
#include <mutex>
#include <span>
#include <string>

#include <xxhash.h>

REXCVAR_DEFINE_BOOL(native_x_renderer_replay_recorded_targets, false, "NativeXRenderer",
                    "Draws replayed from recorded command buffers render into the targets the recording device had "
                    "bound (D129) instead of the running device's (D131); for diagnosis");
REXCVAR_DEFINE_STRING(native_x_renderer_motion_blur, "off", "NativeXRenderer",
                      "Motion blur of the renderer's own on the main 3D view (motion_blur.h): off, low, medium or high "
                      "(6, 10 or 16 taps); taken at once");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_motion_blur_strength, 50.0, "NativeXRenderer",
                      "Motion blur: its strength, 0 to 100 (%): the exposure, of a sixtieth of a second");
REXCVAR_DEFINE_BOOL(native_x_renderer_motion_blur_objects, true, "NativeXRenderer",
                    "Motion blur: the objects that move on their own (soldiers, vehicles) blur by their own motion too");
REXCVAR_DEFINE_BOOL(native_x_renderer, false, "NativeXRenderer",
                    "Draw the game with NativeXRenderer instead of the SDK's Xenos emulation (read at startup). Off "
                    "in the library; a game that is ready turns it on in its own configuration (POTCAWE: D56). "
                    "If it cannot start, the Xenos path draws");
REXCVAR_DEFINE_STRING(native_x_renderer_antialiasing, "smaa", "NativeXRenderer",
                      "Antialiasing (read at startup): smaa or fxaa on the finished scene, before the HUD (needs the "
                      "game's profile to name the scene's resolve); dlaa, NVIDIA DLSS at the render resolution before "
                      "the game's post-processing (needs the game's hooks to mark its views; SMAA where DLSS is not "
                      "available); or off. Experimental, not finished (D53): dlss_quality, dlss_balanced, "
                      "dlss_performance or dlss_ultra_performance, the 3D view drawn at a lower resolution and "
                      "upscaled by DLSS; fsr_native, AMD FSR 3.1 at the render resolution, and fsr_quality, "
                      "fsr_balanced, fsr_performance or fsr_ultra_performance, upscaled by FSR 3.1 (D116)");
REXCVAR_DEFINE_BOOL(native_x_renderer_object_motion, true, "NativeXRenderer",
                    "DLSS (DLAA and the DLSS modes): motion vectors of their own for the objects that move on "
                    "their own, characters and animated props (read at startup); false: the camera's motion only");
REXCVAR_DEFINE_BOOL(native_x_renderer_camera_vote, false, "NativeXRenderer",
                    "DLSS and FSR 3.1: the camera's motion found by a vote of the main view's draws (their "
                    "world-view-projection this frame and the last, D118) instead of the matrix the game's view hook "
                    "gives (read at startup); the log compares the two where the game has the hook");
REXCVAR_DEFINE_STRING(native_x_renderer_views, "", "NativeXRenderer",
                      "DLSS and FSR 3.1: where the main 3D view starts and ends (read at startup): hooks (the game's "
                      "BeginView / EndViewGeometry, D47), auto (found from the draws, up to the profile's "
                      "upscale_resolve_caller, with the camera vote: D119), or empty for the profile's choice");
REXCVAR_DEFINE_BOOL(native_x_renderer_frame_generation, false, "NativeXRenderer",
                    "AMD FSR 3.1 frame generation (read at startup, D123): a frame between each two of the game's, "
                    "presented before the game's. Needs DLSS or FSR 3.1 (their 3D view and motion vectors) and a game "
                    "running at half the display's refresh rate; with V-Sync the two are shown a refresh apart");
REXCVAR_DEFINE_BOOL(native_x_renderer_night, false, "NativeXRenderer",
                    "Night (D162): the finished scene drawn as a moonlit night, before the HUD (Battlefield 1943 "
                    "Reborn's Night Ops servers)");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_night_brightness, 1.0, "NativeXRenderer",
                      "Night: how bright the moonlight is (1 the default, 0.5 darker, 2 brighter)");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_night_haze, 700.0, "NativeXRenderer",
                      "Night: the distance in metres at which the night haze hides about two thirds of the view");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_night_glow, 1.0, "NativeXRenderer",
                      "Night (D163): how strongly the effects keep their own light (muzzle flashes, tracers, fire, "
                      "explosions; 0 none)");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_night_lamps, 1.0, "NativeXRenderer",
                      "Night (D163): how strongly the level's lamps light their surroundings and glow (0 none)");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_night_sky, 1.0, "NativeXRenderer",
                      "Night (D169): the night sky's stars, Milky Way and moon, made by the renderer (0 none, 1 the "
                      "default)");
REXCVAR_DEFINE_INT32(native_x_renderer_night_debug, 0, "NativeXRenderer",
                     "Night (D163), tests: 1 shows each pixel's place in the world (5 m checks), 2 the lights alone");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_hitch_ms, 0.0, "NativeXRenderer",
                      "A frame whose interval between presents is above this many ms is logged with what the "
                      "renderer did in it: shaders and pipelines created, draws skipped or waiting for them, texture "
                      "loads, uploads, the wait for the GPU (0: off)");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_pipeline_wait_budget_ms, 2.0, "NativeXRenderer",
                      "D128: draws into offscreen targets older than a few frames wait for their pipelines at most "
                      "this many ms a frame in all, then are skipped (the target keeps its last frame); a new "
                      "target's draws still wait up to 3 s (D101)");
REXCVAR_DEFINE_BOOL(native_x_renderer_pipeline_storage, true, "NativeXRenderer",
                    "Keep the shaders and pipelines used between runs, and prepare them at startup (read at startup)");
REXCVAR_DEFINE_BOOL(native_x_renderer_param_gen_texel_centre, true, "NativeXRenderer",
                    "With the game's profile asking for it (D164): pixel shaders that read their pixel position get the "
                    "host pixel's centre less one guest pixel (read at startup); false: the SDK's floor(position) / "
                    "scale");
REXCVAR_DEFINE_BOOL(native_x_renderer_wait_offscreen_pipelines, true, "NativeXRenderer",
                    "A draw into a target not of the game's output size (post-processing, masks, shadows) waits for "
                    "its shaders and pipeline instead of being skipped (D101); false: skipped, as the scene's draws");
REXCVAR_DEFINE_BOOL(native_x_renderer_fill_screen, true, "NativeXRenderer",
                    "Fill an output that is not 16:9, with a render scale per axis, when the game's hooks widen its "
                    "cameras (read at startup); false: the 16:9 image with bars");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_aspect, 0.0, "NativeXRenderer",
                      "With native_x_renderer_fill_screen, the image's aspect ratio (read at startup, D104): 0 = the "
                      "output's, which it fills; another (2.3889 for 21:9, 3.5556 for 32:9) renders the game at that "
                      "ratio, with bars where the output is narrower or wider");
REXCVAR_DEFINE_STRING(native_x_renderer_hud_layout, "edges", "NativeXRenderer",
                      "With Profile::box_2d_after_composite on a wider output: edges = the HUD's left and right thirds "
                      "at the screen's edges, the rest in the middle; centered = all of it in a 16:9 box in the middle");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_hud_scale, 100.0, "NativeXRenderer",
                      "With Profile::box_2d_after_composite: the HUD's size in percent of the game's (50 to 100), each "
                      "part scaled about the screen's corner, edge or centre it hangs on; read while the game runs");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_minimap_scale, 100.0, "NativeXRenderer",
                      "The HUD's bottom left group (Battlefield 1943's minimap) in percent of the game's size (75, 100 "
                      "or 125), apart from native_x_renderer_hud_scale; read while the game runs");
REXCVAR_DEFINE_BOOL(native_x_renderer_debug_hud, false, "NativeXRenderer",
                    "Log the 2D layer's draws after the composite every 10 s (Profile::box_2d_after_composite)");
REXCVAR_DEFINE_BOOL(native_x_renderer_raise_priority, false, "NativeXRenderer",
                    "The renderer's thread and the guest GPU's command processor thread one step above normal "
                    "priority (read at startup, D136): the game waits on both, and its own threads spin while they "
                    "wait. Off by default: no gain shown in Battlefield 1943's tests, which were too noisy to tell");
REXCVAR_DEFINE_BOOL(native_x_renderer_thread, true, "NativeXRenderer",
                    "Run the renderer on a thread of its own (read at startup, D71): the game's render thread only "
                    "captures what each D3D call needs, and the renderer's thread draws, through DiligentCore and the "
                    "driver. Off: all of it on the game's render thread, inside its D3D calls");
REXCVAR_DEFINE_BOOL(native_x_renderer_present_with_swaps, false, "NativeXRenderer",
                    "Show each frame when the command processor executes the game's swap for it, as the SDK's "
                    "presenters do, instead of when the game calls Present (read at startup, D86): the game's CPU "
                    "runs a frame ahead, and its delays (Kameo's Bink videos) then do not reach the screen. Needs "
                    "the renderer's own thread (native_x_renderer_thread)");
REXCVAR_DEFINE_STRING(native_x_renderer_scaling, "bilinear", "NativeXRenderer",
                      "How the image is scaled into the window when the internal resolution is lower (read at "
                      "startup, D115): bilinear; fsr, AMD FidelityFX Super Resolution 1; nis, NVIDIA Image Scaling");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_sharpness, -1.0, "NativeXRenderer",
                      "Sharpening when presenting, 0 to 1 (read at startup, D115): with bilinear AMD CAS, with fsr "
                      "FSR 1's RCAS, with nis NIS's own; 0 none; negative, the filter's default (bilinear none, fsr "
                      "RCAS at 0.2 stops, nis 0.5)");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_upscaler_sharpness, 0.0, "NativeXRenderer",
                      "With AMD FSR 4 or FSR 3.1 as the antialiasing (fsr4_*, fsr_*): AMD's own sharpening pass after "
                      "the upscale, 0 (off) to 1; read while the game runs");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_resolution_scale, 0.0, "NativeXRenderer",
                      "Render scale, host pixels per guest pixel (read at startup). 0: from the screen, the largest "
                      "at which the game's image fits the monitor or the window (at least 1)");
REXCVAR_DEFINE_BOOL(native_x_renderer_check_nis, false, "NativeXRenderer",
                    "Ask the GPU at startup whether NVIDIA Image Scaling runs on it when another scaling is chosen "
                    "(D158): the answer for the game's menus (NisSupport), which offer NIS only where it runs");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_upscale_ratio, 0.0, "NativeXRenderer",
                      "With the render scale from the screen (native_x_renderer_resolution_scale 0): that scale "
                      "divided by this, at least 1, for a spatial upscaler to bring up to the screen (read at startup, "
                      "D157): AMD's 1.5 Quality, 1.7 Balanced, 2 Performance. Under 1 draws larger than the screen "
                      "(0.667: one and a half times, supersampling). 0 or 1: the screen's own");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_shadow_scale, 0.0, "NativeXRenderer",
                      "The shadow maps' scale, host pixels per guest pixel on both axes (read at startup, D92): the "
                      "depth passes with no color target that are not of the game's output size are drawn at least "
                      "at this scale (2: twice the game's resolution, 4: four times). 0, or below the render scale: "
                      "the render scale, as everything else");
REXCVAR_DEFINE_INT32(native_x_renderer_resolved_memory_mb, 384, "NativeXRenderer",
                     "Resolved textures unused for a minute that still hold the game's resolve are kept while all the "
                     "resolved textures take at most this many MB, as the console's memory keeps a resolve until "
                     "something writes there; above it the least recently used are freed (D96). 0: freed after a "
                     "minute unused, as before D96");
REXCVAR_DEFINE_INT32(native_x_renderer_upload_ahead, 256, "NativeXRenderer",
                     "Uploads ahead (D98): the vertex and index data of up to this many draws queued behind a draw are "
                     "uploaded with its own, up to the frame's end, so that their copies end the render pass once; 0: "
                     "each draw uploads its own");

namespace native_x_renderer::renderer {
namespace {

using namespace internal;
using namespace Diligent;

constexpr uint32_t kRbCopyDestInfo = 0x231B;
constexpr uint32_t kRbDepthClear = 0x231D;

const Profile* g_profile = nullptr;
std::unique_ptr<Device> g_device;
RenderTargets g_targets;
// The order of the writes to host targets: each draw or clear stamps the targets it writes (D95).
uint64_t g_target_writes = 0;
ResolvedTextures g_resolved;
Blitter g_blit;
// The SDK's dialogs over the presented frame (D75). Never destroyed: the runtime's ImGuiDrawer keeps a pointer.
Overlay& g_overlay = *new Overlay;
rex::ui::Window* g_window = nullptr;
DisplayGamma g_gamma;  // the guest's display gamma ramp, applied when presenting (D36)
Scaling g_scaling;     // the image scaled into the window by FSR 1 or NIS, and sharpened (D115)
// D158: whether NIS runs on this GPU (-1 not asked, 0 no, 1 yes), and the GPU's name, for the game's menus.
std::atomic<int> g_nis_support{-1};
// 14 Nov 2026 (TekRantGaming: only the upscalers this GPU's vendor supports are offered): which temporal upscalers run
// here, asked at the start (bits: 1 FSR 4, 2 FSR 3.1, 4 XeSS, 8 DLSS; -1 until asked).
std::atomic<int> g_temporal_support{-1};
std::string g_gpu_name;
Antialiasing g_aa;     // on the finished scene (D41)
// 13 Nov 2026: the setting read again once a frame, so Off, FXAA, SMAA and CMAA 2 change while the game runs (not to
// or from DLSS or FSR: those draw the 3D view differently from the start).
std::string g_aa_name;
bool g_aa_live = false;
Night g_night;         // the finished scene as a moonlit night (D162)
Scope g_scope;         // the picture in picture sniper scope (D168)
bool g_scope_pending = false;  // the scene's copy was made: the scope follows the game's composite (after the night)
bool g_scope_mask_seen = false;  // the scope's pass was drawn in this frame (the research log's)
bool g_scope_watch = false;  // the game reports its view (the picture in picture on, in a match): as of the last present
uint64_t g_scope_unzoomed = 0;  // the game's scoped picture seen without a sniper scope's zoom (the periodic log)
// A sniper scope's zoom (the half angles' tangents, now over the camera's own): the Springfield's 0.22 (run v41scope5);
// a sight's is about 0.6 to 0.8.
constexpr float kScopeZoom = 0.6f;
// NXR_SCOPE_RESEARCH=1: the draws after the scene of one frame with the scope up (its 40th composite; =N: the Nth, from
// 2) and of one without (the 90th after the scope first came down, so one in the match), logged (the research of which
// draws are the game's mask: DrawPath::SetScopeFrame). The 40th is still in the scope's raise, which the game draws
// black (run v41scope12).
int ScopeResearch(bool scoped) {
  static const char* env = std::getenv("NXR_SCOPE_RESEARCH");
  if (!env) return 0;
  static const int at = std::atoi(env) >= 2 ? std::atoi(env) : 40;
  static int scoped_frames = 0, plain_frames = 0;
  if (scoped) return ++scoped_frames == at || scoped_frames == at + 1 ? 2 : 0;
  return scoped_frames > 0 && (++plain_frames == 90 || plain_frames == 91) ? 1 : 0;
}
bool g_scope_research_frame = false;  // this frame's draws are logged: its present is too
bool g_scope_research_present = false;  // the logged frame's present: what it shows
int g_scope_draw_probes = 0;            // the logged frame's draws into the output read back after each
// NXR_SCOPE_RESEARCH: four pixels of a texture read back (ScopeProbe), to see where the scope's pass went.
ITexture* g_scope_probe_texture = nullptr;
void ScopeProbe(const char* when, ITexture* texture);
// D162: the main view's depth (its shader view) kept at the view's end for the night's haze, the view's share of it,
// and the frame it is from.
Diligent::RefCntAutoPtr<Diligent::ITextureView> g_night_depth;
float g_night_depth_uv[2] = {1.0f, 1.0f};
uint64_t g_night_depth_frame = ~0ull;
bool g_night_pending = false;  // the scene's copy was made: the night follows the game's composite
// D162: the world's depth from before the first person weapon (the profile's AO depth copy), and its frame.
Diligent::RefCntAutoPtr<Diligent::ITexture> g_night_world;
uint64_t g_night_world_frame = ~0ull;
float g_night_world_uv[2] = {1.0f, 1.0f};  // the view's share of it
// D163: the effects' layer (the game's half size pass of its emissive particles, copied with its mips), its frame.
Diligent::RefCntAutoPtr<Diligent::ITexture> g_night_glow;
uint64_t g_night_glow_frame = ~0ull;
uint64_t g_night_glow_frames = 0;  // frames with one since the last log line
// D163: the camera from the terrain's draws (world to clip, the terrain's viewport and NDC transform), its frame, and
// the main view's size on the host (its depth target's pixels).
struct NightCamera {
  float wvp[4][4] = {};
  Diligent::Viewport viewport;
  float ndc_scale[3] = {}, ndc_offset[3] = {};
  float view_width = 0, view_height = 0;
  uint64_t frame = ~0ull;
} g_night_camera;
// D163: the level's lamps the game gave (NightLamps), and the frame's choice of them.
std::mutex g_night_lamps_mutex;
std::vector<NightLamp> g_night_lamps;
std::atomic<bool> g_night_lamps_present{false};
// D163: the models with lamps (NightModels, under the lamps' lock, and its version), the render thread's copy and their
// index counts for the draw path, and this frame's draws of them (index count, world-view-projection).
std::vector<NightModelLamps> g_night_models;
uint64_t g_night_models_version = 0;
std::vector<NightModelLamps> g_night_models_render;
std::vector<uint32_t> g_night_model_counts;
uint64_t g_night_models_render_version = 0;
struct NightModelDraw {
  uint32_t index_count;
  Mat4d m;
};
std::vector<NightModelDraw> g_night_model_draws;
uint64_t g_night_models_placed = 0;  // models placed with their lamps since the last log line
void ReportNightModelDraw(uint32_t index_count, const Mat4d& m) {
  if (g_night_model_draws.size() < 48) g_night_model_draws.push_back({index_count, m});
}
std::vector<NightLight> g_night_frame_lights;
std::vector<NightSprite> g_night_frame_sprites;
bool NightLightsWanted();
bool BuildNightCamera(NightFrame& f, float to_u, float to_v, const NightSettings& s);
Gtao g_gtao;           // ambient occlusion at the end of the main view's opaque draws (D132)
bool g_gtao_on = false;
bool g_gtao_done = false;  // the opaque effects ran in this frame's view
Ssgi g_ssgi;           // global illumination at the same moment, after the AO (D133)
bool g_ssgi_on = false;
Grass g_grass;  // the grass prototype (grass.h), at the same moment, before the AO
bool g_grass_allowed = false;  // the AO's moment exists (the main view found from the draws)
bool g_grass_tried = false, g_grass_ready = false;
// native_grass on, the prototype set up the first time it is (nothing is created while it stays off).
bool GrassOn() {
  if (!g_grass_allowed || !Grass::On()) return false;
  if (!g_grass_tried && g_device) {
    g_grass_tried = true;
    g_grass_ready = g_grass.Initialize(g_device->device());
  }
  return g_grass_ready;
}
TerrainFrame g_terrain;  // this frame's terrain draws, for it
bool g_scene_antialiased = false;  // this frame's scene copy antialiased already (Profile::scene_resolve_once, D94)
Upscaler g_upscaler;   // NVIDIA DLSS (Phase 5, D46)
// Motion blur (motion_blur.h, 0.4.2): made at its first use; the view's camera vote kept for it (the ambient
// occlusion or the upscaler takes it first), and the time of its last frame.
MotionBlur g_motion_blur;
int g_motion_blur_state = 0;  // 0 not made, 1 made, -1 could not be
Mat4d g_blur_vote;
bool g_blur_voted = false;
std::chrono::steady_clock::time_point g_blur_last{};
int MotionBlurTaps() {
  const std::string mode = REXCVAR_GET(native_x_renderer_motion_blur);
  return mode == "low" ? 6 : mode == "medium" ? 10 : mode == "high" ? 16 : 0;
}
// Frame generation (D123): AMD's, and whether it is on; this frame's view inputs, taken at its end and prepared at
// Present, after the 2D layer's motion (D124) is drawn over them; whether the view ended this frame; the 2D layer's
// motion and its vertices' positions (a draw's).
FrameGeneration g_framegen;
bool g_frame_generation = false;
Upscaler::FrameInputs g_frame_inputs;
bool g_frame_inputs_valid = false;
bool g_view_ended = false;
OverlayMotion g_overlay_motion;
std::vector<float> g_overlay_positions;
OcclusionQueries g_queries;  // the guest's occlusion queries (D32)
bool g_queries_on = false;
RenderScale g_scale;  // host pixels per guest pixel (D37), per axis when filling the output (D43)
double g_aspect_factor = 1.0;  // g_scale.x / g_scale.y: how much wider the image is than the game's
bool g_aspect_box = false;     // the draws are a 2D layer kept at the game's aspect ratio (D43)
// Profile::box_2d_after_composite: 0 before the scene's copy, 1 after it (the composite next), 2 after the composite.
int g_hud_phase = 0;
bool g_scene_copied_this_frame = false, g_scene_copied_last_frame = true;
// DLSS or FSR 3.1 (D47, D49, D116): on when native_x_renderer_antialiasing is dlaa, a DLSS mode, fsr_native or an FSR
// mode, and the provider is available. Between BeginView with a matrix and EndViewGeometry the draws into targets of
// the game's output size are the main 3D view's: jittered, and their color and depth targets kept for the upscaler.
// With an upscaling mode those targets are the reduced ones (targets.h), at g_reduced_scale; EndViewGeometry brings
// them to the render scale.
bool g_temporal = false;
bool g_object_motion = false;  // D48, with DLSS or FSR 3.1
bool g_camera_vote = false;    // D118: the camera's reprojection from a vote of the main view's draws
// D119: the main 3D view found from the draws (no view hooks): from its first depth-writing draw of the output size
// with a perspective matrix to the profile's upscale_resolve_caller; then none again until the next Present.
bool g_auto_views = false;
bool g_auto_view_done = false;
// Whether the last frame's view reached the upscale point: only then is this frame's jittered (as King Kong's
// renderer does), so that a 3D scene that never gets there (GH Metallica's loading screen) is not shaken.
bool g_auto_upscaled_last_frame = false;
// The vote's results since the last stats line: applications with a vote, their offers and the top candidate's votes,
// and the difference from the view hook's reprojection where both were there (output pixels).
struct {
  uint32_t applications = 0, voted = 0, compared = 0;
  double offers = 0.0, votes = 0.0, difference = 0.0, max_difference = 0.0;
} g_vote_stats;
bool g_reduced = false;        // D49: the main 3D view at a lower scale
RenderScale g_reduced_scale;
// D121: with the views found from the draws, the view's first draw is known only once drawn, after its targets were
// chosen and cleared: the targets of the output size are the reduced ones from the frame's start when the last
// frame's view reached the upscale point (King Kong Recompiled's way), up to that point; a frame that does not reach
// it has them brought back at Present.
bool g_auto_reduced = false;
// D92: the shadow maps' scale, higher than the render scale when native_x_renderer_shadow_scale asks for it.
RenderScale g_shadow_scale;
bool g_shadow_scaled = false;
bool g_scene_view = false;
std::array<float, 16> g_scene_view_projection{};
HostTarget* g_scene_color = nullptr;
HostTarget* g_scene_depth = nullptr;
GuestSurface g_scene_color_surface;
// The reduced targets drawn or cleared in this view (D49), with their surfaces.
std::vector<std::pair<GuestSurface, HostTarget*>> g_reduced_used;
std::filesystem::path g_cache_root;  // the runtime's cache directory (D42)
std::unique_ptr<DrawPath> g_draw;  // created on the first draw (it needs the tracker's register file)

void ScopeProbe(const char* when, ITexture* texture) {
  if (!texture || !g_device) return;
  const TextureDesc& desc = texture->GetDesc();
  TextureDesc staging;
  staging.Name = "NativeXRenderer scope probe";
  staging.Type = RESOURCE_DIM_TEX_2D;
  staging.Width = 1, staging.Height = 1;
  staging.Format = desc.Format;
  staging.Usage = USAGE_STAGING;
  staging.CPUAccessFlags = CPU_ACCESS_READ;
  std::string text;
  // A corner (the game's black ring); on the reticle's level line inside the lens; inside the game's round view but
  // outside the lens, off the lines; inside the lens, off the lines. Each within the texture: a copy from outside it
  // lost the Vulkan device (run v41scope13, the luminance chain's 3 x 3 texture).
  const uint32_t w = desc.Width, h = desc.Height;
  if (!w || !h) return;
  for (auto at : {std::pair<uint32_t, uint32_t>{8, 8}, {w / 2 + h / 5, h / 2}, {w / 2 + h * 38 / 100, h / 2 + h / 10},
                  {w / 2 + h / 10, h / 2 + h / 10}}) {
    at.first = std::min(at.first, w - 1), at.second = std::min(at.second, h - 1);
    RefCntAutoPtr<ITexture> probe;
    g_device->device()->CreateTexture(staging, nullptr, &probe);
    if (!probe) return;
    Box box(at.first, at.first + 1, at.second, at.second + 1);
    CopyTextureAttribs copy(texture, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, probe, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    copy.pSrcBox = &box;
    g_device->context()->CopyTexture(copy);
    g_device->context()->WaitForIdle();
    MappedTextureSubresource mapped;
    g_device->context()->MapTextureSubresource(probe, 0, 0, MAP_READ, MAP_FLAG_NONE, nullptr, mapped);
    if (mapped.pData) {
      const uint8_t* px = static_cast<const uint8_t*>(mapped.pData);
      text += fmt::format(" ({}, {}): {:02X} {:02X} {:02X} {:02X}", at.first, at.second, px[0], px[1], px[2], px[3]);
    }
    g_device->context()->UnmapTextureSubresource(probe, 0, 0);
  }
  REXLOG_INFO("[native_x_renderer] scope research, {}: {} ({}x{} format {}):{}", when, fmt::ptr(texture), desc.Width,
              desc.Height, int(desc.Format), text);
}
bool g_draw_failed = false;
bool g_tiling = false;  // between BeginTiling and EndTiling
// The size of a tiled pass's image, from BeginTiling's tile rectangles: the GPU draws the pass once per tile
// into a surface of one tile's size, NativeXRenderer once into targets of every tile's (D60).
uint32_t g_tiled_width = 0, g_tiled_height = 0;
uint32_t g_frames_presented = 0;
// Frames the game has ended (OnFrameEnd, its thread): with g_frames_presented, how far the renderer's thread is behind.
std::atomic<uint32_t> g_frames_captured{0};

// The renderer's thread (D71): the commands the game's thread captures, run in order.
CommandQueue g_queue;
// Presenting with the guest's swaps (D86): on; the swaps the command processor has executed (its observer, on
// its thread); the oldest swap a composed frame waits for (0: none); the last tag given at capture (the game's
// thread). The game's CPU runs a frame ahead of the command processor: a frame's swap executes after the next
// frame's Present has run, so up to two composed frames wait, each in an image of its own (three, one being
// composed), copied into the swap chain's back buffer when shown.
rex::system::IGraphicsSystem* g_graphics = nullptr;
bool g_present_with_swaps = false;
// Uploads ahead (D98): the draws to look ahead (native_x_renderer_upload_ahead, read once a frame), the registers
// the ranges come from as they will be at each queued command (the fetch constants and the index buffer's; the
// queued records applied to a copy, the rest of which is not read), the ranges gathered, and the command up to which
// the data has been requested.
int32_t g_upload_ahead = 0;
std::unique_ptr<rex::graphics::RegisterFile> g_ahead_registers;
std::vector<std::pair<uint32_t, uint32_t>> g_ahead_ranges;
uint64_t g_ahead_until = 0;
std::atomic<uint64_t> g_swaps_executed{0}, g_pending_swap{0};
uint64_t g_last_swap_tag = 0;
struct PendingFrame {
  uint64_t tag;
  uint32_t image;
  std::chrono::steady_clock::time_point since;
};
constexpr uint32_t kMaxPendingFrames = 2;
std::array<PendingFrame, kMaxPendingFrames> g_pending_frames{};
uint32_t g_pending_count = 0;
struct FrameImage {
  Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
  Diligent::ITextureView *rtv = nullptr, *srv = nullptr;
};
std::array<FrameImage, kMaxPendingFrames + 1> g_frame_images;
uint32_t g_next_frame_image = 0;
// Frame generation (D123): the game's frame, composed; where the picture is in the window (PresentFrontBuffer's
// letterbox); the interval between the game's frames (for the log).
FrameImage g_frame_generation_image;
// The scene before the HUD (the profile's scene resolve, copied when it is resolved this frame) and its picture,
// composed as the frame is: FidelityFX takes the HUD from where the frame differs from it, and does not move it.
Diligent::RefCntAutoPtr<Diligent::ITexture> g_hudless_copy;
bool g_hudless_this_frame = false;
FrameImage g_hudless_image;
int32_t g_present_area[4] = {};
double g_game_frame_ms = 16.7;
std::chrono::steady_clock::time_point g_last_game_frame{};
// A composed frame not shown within this long (the command processor stopped) is shown.
constexpr auto kSwapTimeout = std::chrono::milliseconds(100);
// Capturing a command: the game's render thread, or a game's hook (BeginView...) if it ran on another thread.
std::mutex g_capture_mutex;
// The renderer's copy of the GPU registers, on its thread: what the draw path reads. The tracker's records
// are applied to it (D73).
std::unique_ptr<rex::graphics::RegisterFile> g_registers;
uint32_t g_captured = 0;  // commands captured since the last frame end, on the game's thread

// The arguments of the outermost call in progress (the renderer acts when it returns).
struct Arguments {
  Call call{};
  std::array<uint32_t, 8> r{};  // r3..r10
  double f1 = 0;
  uint32_t lr = 0;  // the return address in the caller
  uint32_t outer_lr = 0;  // its caller's (D65), for a Resolve when the profile checks it
};
Arguments g_call;
uint32_t g_front_buffer = 0;  // the texture object Present was given this frame

// Per frame, for the log.
struct FrameStats {
  uint32_t clears = 0, resolves = 0, skipped_resolves = 0;
  uint32_t mirror_writes = 0;  // D140: resolves also written to the guest memory mirror
} g_stats;

// GPU time per frame: a duration query around each frame's commands (from one Present to the next), read
// when the query is reused, kQueries frames later. A query not finished by then is counted as late: the
// GPU is more than kQueries frames behind the CPU.
class FrameTimer {
 public:
  void Initialize(IRenderDevice* device) {
    if (device->GetDeviceInfo().Features.DurationQueries != DEVICE_FEATURE_STATE_ENABLED) return;
    for (auto& query : queries_) {
      QueryDesc desc;
      desc.Name = "NativeXRenderer frame time";
      desc.Type = QUERY_TYPE_DURATION;
      device->CreateQuery(desc, &query);
      if (!query) return;
    }
    enabled_ = true;
  }
  void Begin(IDeviceContext* context) {
    if (!enabled_) return;
    if (pending_[index_]) {
      QueryDataDuration data;
      if (queries_[index_]->GetData(&data, sizeof(data), true) && data.Frequency) {
        double ms = double(data.Duration) * 1000.0 / double(data.Frequency);
        total_ms_ += ms;
        max_ms_ = std::max(max_ms_, ms);
        ++frames_;
        // native_x_renderer_hitch_ms: a frame whose GPU work alone took that long, by the guest frame it followed
        // (to tell a GPU bound hitch from a wait elsewhere in the present).
        if (long_ms_ > 0 && ms > long_ms_)
          REXLOG_INFO("[native_x_renderer] GPU long frame: after frame {}: {:.1f} ms of GPU work", frame_of_[index_], ms);
      } else {
        ++late_;
      }
      pending_[index_] = false;
    }
    context->BeginQuery(queries_[index_]);
    active_ = true;
  }
  void End(IDeviceContext* context) {
    if (!active_) return;
    context->EndQuery(queries_[index_]);
    frame_of_[index_] = last_frame_;
    pending_[index_] = true;
    index_ = (index_ + 1) % kQueries;
    active_ = false;
  }
  // The guest frame last ended (for the long frame log) and the length worth logging (0: none).
  void SetFrame(uint32_t frame, double long_ms) { last_frame_ = frame, long_ms_ = long_ms; }
  struct Result {
    double average_ms = 0, max_ms = 0;  // 0 if no frame was measured
    uint32_t frames = 0, late = 0;
  };
  // Since the last call.
  Result Take() {
    Result result{frames_ ? total_ms_ / frames_ : 0.0, max_ms_, frames_, late_};
    total_ms_ = max_ms_ = 0;
    frames_ = late_ = 0;
    return result;
  }

 private:
  static constexpr uint32_t kQueries = 32;
  std::array<RefCntAutoPtr<IQuery>, kQueries> queries_;
  std::array<bool, kQueries> pending_{};
  std::array<uint32_t, kQueries> frame_of_{};
  uint32_t index_ = 0, last_frame_ = 0;
  double long_ms_ = 0;
  bool enabled_ = false, active_ = false;
  double total_ms_ = 0, max_ms_ = 0;
  uint32_t frames_ = 0, late_ = 0;
} g_timer;

// Frame pacing on the CPU side, for the log: time spent in the renderer (on the render thread, inside the
// game's D3D calls), time waiting for the GPU (Device::Present), interval between presents.
struct Pacing {
  using Clock = std::chrono::steady_clock;
  double renderer_ms = 0, wait_ms = 0, max_wait_ms = 0, interval_ms = 0, max_interval_ms = 0;
  uint32_t frames = 0;
  // The last present's interval and wait for the GPU (native_x_renderer_hitch_ms).
  double last_interval_ms = 0, last_wait_ms = 0;
  // Since the last hitch check: the swap chain's Present (submitting the frame, presenting, acquiring the next image;
  // the GPU wait not included) and, of it, the submit alone (NXR_PRESENT_SPLIT=1 only: the frame submitted first).
  double swap_ms = 0, submit_ms = 0;
  Clock::time_point last_present{};
  // With native_x_renderer_present_with_swaps (D86): frames shown at their swap; shown earlier because two
  // newer frames were waiting, or because the swap did not come in time; not shown (a newer one's swap had
  // executed too).
  uint32_t at_swap = 0, before_swap = 0, timed_out = 0, skipped = 0;

  static double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
} g_pacing;

// Host objects created since the last log line: shader analyses and pipelines on the render thread,
// translations on worker threads (D34).
struct Creation {
  CreationTime analyses, translations, pipelines;
  uint64_t translating_draws = 0;  // skipped while a shader was translated (D34)
  uint64_t compiling_draws = 0;    // skipped while their pipeline compiled (D28)
  // Draws into offscreen targets that waited for their shaders and pipeline instead (D101), and how long.
  uint64_t waited_draws = 0;
  double waited_ms = 0.0, max_waited_ms = 0.0;
} g_creation;

// Buffer copies (each ends the render pass) and guest memory uploads per frame, since the last log line.
struct Uploads {
  uint64_t frames = 0, copies = 0, max_copies = 0, uploaded_bytes = 0, unchanged_bytes = 0;
  uint64_t unwatched_hashed_bytes = 0, unwatched_pages = 0;  // D97
  uint64_t unwatched_copy_bytes = 0;                         // D110
  uint64_t ahead_batches = 0, ahead_draws = 0;               // D98
} g_uploads;

// Object motion (D48) since the last log line: main view draws matched with the last frame's, those that
// moved, and their motion draws.
struct MotionCounts {
  uint64_t matched = 0, moved = 0, drawn = 0, max_drawn = 0, camera = 0;
  uint64_t candidates = 0, summarized = 0, match_ns = 0, draw_ns = 0, longest_key = 0;
} g_motion;

// The renderer's time by phase since the last frame's hitch check (native_x_renderer_hitch_ms): which part of a
// long frame was long.
struct PhaseTimes {
  double draws = 0, clears = 0, effects = 0, resolves = 0, present = 0;
} g_phase;

class RendererTime {
 public:
  explicit RendererTime(double* phase = nullptr) : start_(Pacing::Clock::now()), phase_(phase) {}
  ~RendererTime() {
    const double ms = Pacing::Ms(Pacing::Clock::now() - start_);
    g_pacing.renderer_ms += ms;
    if (phase_) *phase_ += ms;
  }

 private:
  Pacing::Clock::time_point start_;
  double* phase_;
};

// A surface the device has bound (render targets 0-3, 4 the depth), as its object says: read on the game's
// thread when a call is captured.
GuestSurface ReadBoundSurface(const uint8_t* base, uint32_t device, uint32_t index) {
  const DeviceLayout& d = g_profile->device;
  const ObjectLayout& o = g_profile->objects;
  return index < 4 ? ReadSurface(base, Load32(base, device + d.render_targets + 4 * index), false, o)
                   : ReadSurface(base, Load32(base, device + d.depth_stencil), true, o);
}

// A bound surface as NativeXRenderer draws it: in a tiled pass, at the size of every tile (D60).
GuestSurface Tiled(GuestSurface s) {
  if (g_tiling && s) {
    s.width = std::max(s.width, g_tiled_width);
    s.height = std::max(s.height, g_tiled_height);
  }
  return s;
}

float GuestFloat(const uint8_t* base, uint32_t address) { return LoadFloat(base, address); }

// <cache root>/native_x_renderer/<title id>: the stored shaders and pipelines of this game (D42).
std::filesystem::path StoragePath(uint32_t title_id) {
  if (g_cache_root.empty() || !REXCVAR_GET(native_x_renderer_pipeline_storage)) return {};
  return g_cache_root / "native_x_renderer" / fmt::format("{:08X}", title_id);
}

// The host target a draw, a clear or a resolve uses for a guest surface: in the main 3D view, while DLSS
// draws it at a lower scale, the reduced one for the targets of the game's output size (D49); with a shadow scale,
// the shadow map's for a depth surface bound with no color surface (a draw's or a clear's binding) that is not of
// the game's output size, and from then on for that surface, except in a draw with a color surface, which gets the
// target at the render scale, of the same scale as the color's (D92). A clear of a shadow map gets its target
// whatever is bound: the XDK's ClearF clears depth through an aliased color surface (POTCAWE's).
enum class Binding { kUnknown, kDepthOnly, kWithColor };

Binding BindingOf(const Command& c) {
  bool color = c.surfaces[0] || c.surfaces[1] || c.surfaces[2] || c.surfaces[3];
  return color ? Binding::kWithColor : Binding::kDepthOnly;
}

HostTarget* TargetFor(const GuestSurface& surface, Binding binding = Binding::kUnknown) {
  bool output_size =
      surface && surface.width == g_profile->output_width && surface.height == g_profile->output_height;
  bool reduced = g_reduced && (g_scene_view || g_auto_reduced) && output_size;
  bool shadow = false;
  if (g_shadow_scaled && surface && surface.depth && !output_size && binding != Binding::kWithColor) {
    shadow = g_targets.IsShadow(surface);
    if (!shadow && binding == Binding::kDepthOnly) {
      g_targets.MarkShadow(surface);
      shadow = true;
      REXLOG_INFO("[native_x_renderer] shadow map: the {}x{} depth surface at EDRAM base {} (pitch {}), drawn with no "
                  "color target, at the shadow scale {:.4f} (D92)",
                  surface.width, surface.height, surface.edram_base(), surface.pitch(), g_shadow_scale.x);
    }
  }
  using Kind = RenderTargets::Kind;
  HostTarget* target = g_targets.Get(surface, reduced ? Kind::kReduced : shadow ? Kind::kShadow : Kind::kRender);
  auto same = [&](const auto& used) { return used.second == target; };
  if (reduced && target && std::none_of(g_reduced_used.begin(), g_reduced_used.end(), same)) {
    g_reduced_used.emplace_back(surface, target);
  }
  return target;
}

// The reduced targets of the view that ended, at the render scale for what follows (D49): the color DLSS
// upscaled (`upscaled`) is there already; the others are scaled, color filtered and depth nearest.
void PromoteReduced(const HostTarget* upscaled) {
  if (g_reduced_used.empty()) return;
  IDeviceContext* context = g_device->context();
  for (const auto& [surface, reduced] : g_reduced_used) {
    if (reduced == upscaled) continue;
    HostTarget* full = g_targets.Get(surface);
    if (!full || reduced->samples > 1) continue;
    Blitter::Rect from{0, 0, reduced->width, reduced->height}, to{0, 0, full->width, full->height};
    g_blit.Copy(context, reduced->srv, reduced->depth ? Blitter::Source::kDepthToDepth : Blitter::Source::kColorScaled,
                from, full->view, to);
    full->written = ++g_target_writes;
  }
  g_reduced_used.clear();
  if (g_draw) g_draw->TargetsChanged();
}

// Called by a draw whose own requests copied, ending the render pass (D98): the draws queued behind it, up to the
// frame's end, have their vertex and index ranges requested now, at once, their copies recorded with the draw's. The
// game's thread has written their data before queueing them, and does not reuse it before the command processor's
// next signal, which waits for this thread; each draw still requests its own ranges when it runs, finding them
// uploaded. A resolve in between needs no stop: it writes the mirror on the GPU after these copies, and the pages a
// resolve made valid are not uploaded again. A draw that copies nothing costs nothing here: in games that upload
// little (GRAW, POTCAWE) a window at every draw, of the 2 or 3 draws queued behind it, cost more than it saved.
void UploadAhead() {
  if (g_upload_ahead <= 0 || !g_queue.threaded() || !g_draw) return;
  uint64_t index = g_queue.running_index();
  if (index + 1 < g_ahead_until) return;  // the next draws are in this window already
  const Command* next = g_queue.Peek(1);
  if (!next) return;
  if (!g_ahead_registers) g_ahead_registers = std::make_unique<rex::graphics::RegisterFile>();
  uint32_t* ahead_values = g_ahead_registers->values;
  const uint32_t* values = g_registers->values;
  constexpr uint32_t kFetch = rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, kFetchCount = 32 * 6;
  std::memcpy(ahead_values + kFetch, values + kFetch, kFetchCount * sizeof(uint32_t));
  ahead_values[rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR] = values[rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR];
  ahead_values[rex::graphics::XE_GPU_REG_VGT_DMA_BASE] = values[rex::graphics::XE_GPU_REG_VGT_DMA_BASE];
  g_ahead_ranges.clear();
  uint32_t draws = 0, ahead = 1;
  for (; next && draws < uint32_t(g_upload_ahead); next = g_queue.Peek(++ahead)) {
    if (next->type == Command::Type::kPresent || next->type == Command::Type::kFrameEnd) break;
    ApplyRecord(next->registers.data(), next->registers.size(), ahead_values);
    if (next->type == Command::Type::kDraw && next->programs[0]) {
      g_draw->AddDrawRanges(*next->programs[0], *g_ahead_registers, g_ahead_ranges);
      ++draws;
    }
  }
  g_ahead_until = index + ahead;
  g_draw->UploadAhead(g_ahead_ranges, draws);
}

Blitter::Rect HostRect(const RenderScale& scale, const Blitter::Rect& r);

// D113: a draw into a k_8_8_8_8 color surface at a depth surface's EDRAM base wrote 32-bit words where the depth
// surface's are, which on the console it then holds as its depth: Guitar Hero 5 restores the last frame's depth so,
// its depth resolve read as k_8_8_8_8 (D64) and drawn into such a color surface, each pixel from the other half of
// its EDRAM tile (a depth surface's tiles have their 40-sample halves swapped). Before the depth target is drawn
// with or resolved, the depth of a color target drawn there since its last write is copied into it: when that
// draw read a depth resolve's words, the resolve's own depth, pixel for pixel (the game's offset of 40 pixels is
// a fraction of a host pixel at most scales, and taking the words back would shift half the columns by a pixel);
// else the words, with the halves of each tile swapped back.
void CarryColorIntoDepth(const GuestSurface& surface, HostTarget* depth) {
  using Kind = RenderTargets::Kind;
  Kind kind = depth->reduced ? Kind::kReduced : depth->shadow ? Kind::kShadow : Kind::kRender;
  HostTarget* color = g_targets.FindColorOverDepth(surface, kind, depth->written);
  if (!color) return;
  Blitter::Rect r{0, 0, std::min(color->guest_width, depth->guest_width),
                  std::min(color->guest_height, depth->guest_height)};
  ResolvedTexture* words = color->words_from ? g_resolved.FindNewest(color->words_from) : nullptr;
  bool resolved = words && words->last_resolve == color->words_from_resolve && words->srv && !words->shadow;
  if (resolved) {
    r.width = std::min(r.width, words->guest_width);
    r.height = std::min(r.height, words->guest_height);
    g_blit.Copy(g_device->context(), words->srv, Blitter::Source::kDepthToDepth, HostRect(words->scale, r),
                depth->view, HostRect(depth->scale, r));
  } else {
    Blitter::Options options;
    options.depth_float24 = depth->depth_float24;
    options.tile_scale_x = color->scale.x;
    g_blit.Copy(g_device->context(), color->srv, Blitter::Source::kPackedToDepth, HostRect(color->scale, r),
                depth->view, HostRect(depth->scale, r), options);
  }
  depth->written = ++g_target_writes;
  if (g_draw) g_draw->TargetsChanged();
  static bool logged[2] = {};
  if (!logged[resolved]) {
    logged[resolved] = true;
    REXLOG_INFO("[native_x_renderer] the {}x{} depth at EDRAM base {} takes the words a {}x{} k_8_8_8_8 color target "
                "drawn at the same base wrote: {} (D113)",
                depth->guest_width, depth->guest_height, surface.edram_base(), color->guest_width,
                color->guest_height,
                resolved ? "the depth of the resolve those words were read from"
                         : "the words' depth, tile halves swapped");
  }
}

void LogHudStats() {
  static auto at = std::chrono::steady_clock::now();
  if (!g_draw || !REXCVAR_GET(native_x_renderer_debug_hud) ||
      std::chrono::steady_clock::now() - at < std::chrono::seconds(10))
    return;
  at = std::chrono::steady_clock::now();
  REXLOG_INFO("[native_x_renderer] HUD draws after the composite: {}", g_draw->TakeHudLog());
}

void ExecuteDraw(const Command& c) {
  RendererTime time(&g_phase.draws);
  if (!g_draw) {
    if (g_draw_failed) return;
    auto* kernel = rex::system::kernel_state();
    auto draw = std::make_unique<DrawPath>();
    // D164, before the stored pipelines are read (Initialize), which take it too.
    draw->SetParamGenTexelCentre(g_profile->param_gen_texel_centre &&
                                 REXCVAR_GET(native_x_renderer_param_gen_texel_centre));
    if (!kernel || !kernel->memory() ||
        !draw->Initialize(*g_device, *g_registers, *kernel->memory(), g_resolved, g_scale,
                          StoragePath(kernel->title_id()))) {
      REXLOG_ERROR("[native_x_renderer] could not set up the draw path; draws are skipped");
      g_draw_failed = true;
      SetStoredPipelinesFinished();
      return;
    }
    g_draw = std::move(draw);
    g_draw->SetBlitter(&g_blit);  // D155
    g_draw->SetUploadAhead(UploadAhead);
    g_draw->SetTextureSizeConstants(g_profile->texture_size_constants, g_profile->texture_size_constant_count);
  }
  std::array<HostTarget*, 5> targets;
  const HostTarget* first_target = nullptr;
  for (uint32_t i = 0; i < 5; ++i) targets[i] = TargetFor(Tiled(c.surfaces[i]), BindingOf(c));
  if (targets[4]) CarryColorIntoDepth(Tiled(c.surfaces[4]), targets[4]);
  for (uint32_t i = 0; i < 5; ++i) {
    if (targets[i]) targets[i]->written = ++g_target_writes;
    if (targets[i] && !first_target) first_target = targets[i];
  }
  // D101: a draw into a target of another size than the game's output (the first color target, else depth) waits for
  // its pipeline. Such passes may be drawn once and resolved: GH Metallica's post-processing mask, drawn at the
  // career's first song load, showed garbage for the session when its draw was skipped on a first run.
  // D128: only while the target is new (its first frames) without a limit; after that within a small budget a frame:
  // a pass the game draws every frame keeps its last frame's image when a draw is skipped, and Battlefield 1943's
  // offscreen passes waited up to 1.1 s for one pipeline, 23 s in a 9-minute session (its shader stutter).
  {
    const bool offscreen = REXCVAR_GET(native_x_renderer_wait_offscreen_pipelines) && first_target &&
                           (first_target->guest_width != g_profile->output_width ||
                            first_target->guest_height != g_profile->output_height);
    const bool new_target = first_target && g_targets.frame() - first_target->created_frame <= 2;
    g_draw->SetWaitForPipelines(offscreen,
                                new_target ? 0.0 : std::max(0.0, REXCVAR_GET(native_x_renderer_pipeline_wait_budget_ms)));
  }
  // Profile::box_2d_after_composite: the HUD's draws (and a frame without a 3D view) into the aspect box.
  bool box = g_aspect_box;
  float align = 0.5f;
  bool composite = false, hud = false;
  // (13 Nov 2026: with a HUD scale the HUD's draws are told apart on a 16:9 output too, without the box.)
  const float hud_scale = float(std::clamp(REXCVAR_GET(native_x_renderer_hud_scale), 50.0, 100.0) / 100.0);
  const float minimap_scale = float(std::clamp(REXCVAR_GET(native_x_renderer_minimap_scale), 50.0, 150.0) / 100.0);
  const bool hud_scaled = hud_scale != 1.0f || minimap_scale != 1.0f;
  if (g_profile->box_2d_after_composite && (g_aspect_factor != 1.0 || hud_scaled) && targets[0] &&
      targets[0]->guest_width == g_profile->output_width && targets[0]->guest_height == g_profile->output_height) {
    const bool wide = g_aspect_factor != 1.0;
    if (g_hud_phase == 1) composite = true;
    else if (g_hud_phase == 2) box = box || wide, hud = true;
    else if (!g_scene_copied_this_frame && !g_scene_copied_last_frame) box = box || wide, hud = true;
  }
  g_draw->SetHudScale(hud_scale, minimap_scale);
  g_draw->SetAspectBox(box, align);
  // The HUD's draws are placed one by one from their vertex shader's constants (DrawPath::SetHudClassify).
  g_draw->SetHudClassify(hud, REXCVAR_GET(native_x_renderer_hud_layout) == "edges",
                         REXCVAR_GET(native_x_renderer_debug_hud));
  // The main view's draws: their targets kept for the upscaler, and jittered (a view found from the draws only after a
  // frame whose view reached the upscale point, D119).
  bool scene = g_scene_view && (g_temporal || g_gtao_on || g_ssgi_on);
  bool jitter = g_temporal && scene && (!g_auto_views || g_auto_upscaled_last_frame);
  g_draw->SetJitter(jitter, g_upscaler.jitter_x(), g_upscaler.jitter_y(), g_profile->output_width,
                    g_profile->output_height);
  g_draw->SetSceneStart(g_auto_views && !g_scene_view && !g_auto_view_done, g_auto_upscaled_last_frame);
  // The objects' own motion: the upscalers', or the motion blur's (taken at once from its settings).
  g_draw->SetObjectMotion(g_object_motion || (MotionBlurTaps() > 0 && REXCVAR_GET(native_x_renderer_motion_blur_objects)));
  g_draw->SetCameraVote(g_camera_vote);
  g_draw->SetProjectionVote((g_gtao_on || g_ssgi_on) && g_scene_view && !g_gtao_done);
  // The terrain's draws: the grass's, and the night's lights' camera (D163).
  g_draw->SetTerrainCapture(g_scene_view && !g_gtao_done && (GrassOn() || NightLightsWanted()) ? &g_terrain : nullptr);
  // D163: the models with lamps (a jeep's headlights), at night.
  g_draw->SetModelLamps(g_scene_view && !g_night_model_counts.empty() && REXCVAR_GET(native_x_renderer_night) &&
                                REXCVAR_GET(native_x_renderer_night_lamps) > 0.0
                            ? &g_night_model_counts
                            : nullptr,
                        &ReportNightModelDraw);
  std::array<DrawPath::Copy, 2> copies;
  for (uint32_t i = 0; i < c.copy_count; ++i) {
    copies[i] = {c.copies[i].address, c.data.data() + c.copies[i].offset, c.copies[i].size};
  }
  g_draw->TakePackedRead();
  g_draw->Draw(c.programs[0] ? *c.programs[0] : StateTracker::kNoProgram,
               c.programs[1] ? *c.programs[1] : StateTracker::kNoProgram, std::span(copies.data(), c.copy_count),
               targets, g_tiling);
  if (composite) g_hud_phase = 2;
  // D162: the night on the game's composite, the first draw into a target of the output size after the scene's copy
  // (Battlefield 1943: its bloom chain draws only smaller targets in between): the picture with the game's bloom and
  // exposure, before the HUD drawn over it.
  if (g_night_pending && targets[0] && targets[0]->guest_width == g_profile->output_width &&
      targets[0]->guest_height == g_profile->output_height) {
    g_night_pending = false;
    HostTarget* t = targets[0];
    if (t->texture && t->view && t->samples == 1) {
      NightSettings night;
      night.brightness = float(std::clamp(REXCVAR_GET(native_x_renderer_night_brightness), 0.1, 4.0));
      night.haze_m = float(std::clamp(REXCVAR_GET(native_x_renderer_night_haze), 50.0, 20000.0));
      const bool depth = g_night_depth && g_night_depth_frame == g_device->frames_presented();
      // The depth's coordinates from the target's: its texture may be larger than the output (the EDRAM's pitch).
      const TextureDesc& desc = t->texture->GetDesc();
      const float view_w = float(t->scale.HostWidth(g_profile->output_width));
      const float view_h = float(t->scale.HostHeight(g_profile->output_height));
      const bool world = g_night_world && g_night_world_frame == g_device->frames_presented();
      // Each depth with its own share of its texture (with an upscaler the textures can differ from the target's).
      const float to_u = view_w > 0 ? float(desc.Width) / view_w : 1.0f;
      const float to_v = view_h > 0 ? float(desc.Height) / view_h : 1.0f;
      night.glow = float(std::clamp(REXCVAR_GET(native_x_renderer_night_glow), 0.0, 8.0));
      night.lamps = float(std::clamp(REXCVAR_GET(native_x_renderer_night_lamps), 0.0, 8.0));
      night.debug = REXCVAR_GET(native_x_renderer_night_debug);
      night.sky = float(std::clamp(REXCVAR_GET(native_x_renderer_night_sky), 0.0, 4.0));
      NightFrame f;
      f.depth = depth ? g_night_depth.RawPtr() : nullptr;
      f.depth_uv[0] = to_u * g_night_depth_uv[0], f.depth_uv[1] = to_v * g_night_depth_uv[1];
      f.world = world ? g_night_world->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
      f.world_uv[0] = to_u * g_night_world_uv[0], f.world_uv[1] = to_v * g_night_world_uv[1];
      // D163: the effects' layer covers the view (half its size).
      if (g_night_glow && g_night_glow_frame == g_device->frames_presented()) {
        f.glow = g_night_glow->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        f.glow_uv[0] = to_u, f.glow_uv[1] = to_v;
      }
      // D163: the camera and the lamps it shows.
      if (BuildNightCamera(f, to_u, to_v, night)) {
        f.lights = g_night_frame_lights.data(), f.light_count = uint32_t(g_night_frame_lights.size());
        f.sprites = g_night_frame_sprites.data(), f.sprite_count = uint32_t(g_night_frame_sprites.size());
      }
      g_night.Apply(g_device->device(), g_device->context(), t->texture, t->view, f, night);
      t->written = ++g_target_writes;
      g_draw->TargetsChanged();
    }
  }
  // D168: the picture in picture scope. On the game's composite (after the night), while the view is not zoomed, the
  // finished scene is remembered, and from there the game's own scope mask pass is watched for (DrawPath::IsScopeMask:
  // a sniper scope's only; a sight's zoom, a vehicle's or a menu's has none). Right after that pass, the lens (its
  // middle, the game's reticle in it) and the world around it are drawn over the pass's picture, before the HUD.
  if (g_scope_pending && targets[0] && targets[0]->guest_width == g_profile->output_width &&
      targets[0]->guest_height == g_profile->output_height) {
    g_scope_pending = false;
    HostTarget* t = targets[0];
    ScopeCamera camera;
    float zoom = 1.0f;
    if (t->texture && t->view && t->samples == 1 &&
        ScopeView(camera.right, camera.up, camera.back, camera.tan_half_v, zoom)) {
      if (zoom > 0.97f) {
        // With the view's depth at its end (taken for the scope too): the first person weapon left out.
        // Its coordinates from the target's, as the night's: the target's texture may be larger than the view.
        const bool depth = g_night_depth && g_night_depth_frame == g_device->frames_presented();
        const TextureDesc& desc = t->texture->GetDesc();
        const float view_w = float(t->scale.HostWidth(g_profile->output_width));
        const float view_h = float(t->scale.HostHeight(g_profile->output_height));
        const float uv[2] = {(view_w > 0 ? float(desc.Width) / view_w : 1.0f) * g_night_depth_uv[0],
                             (view_h > 0 ? float(desc.Height) / view_h : 1.0f) * g_night_depth_uv[1]};
        g_scope.Remember(g_device->device(), g_device->context(), t->texture, camera, depth ? g_night_depth.RawPtr() : nullptr,
                         uv);
      }
      const int research = ScopeResearch(zoom < kScopeZoom);
      g_scope_research_frame = g_scope_research_frame || research != 0;
      g_draw->SetScopeFrame(true, research);
      if (research) {
        REXLOG_INFO("[native_x_renderer] scope research: the composite on {}", fmt::ptr(t));
        ScopeProbe("the composite", t->texture);
      }
    }
  }
  if (g_draw->TakeScopeMaskDrawn() && targets[0] && targets[0]->texture && targets[0]->view &&
      targets[0]->samples == 1) {
    HostTarget* t = targets[0];
    ScopeCamera camera;
    float zoom = 1.0f;
    // Zoomed as a sniper scope zooms, too: the same pass blurs the view as the soldier spawns (run v41scope7).
    const bool noted = ScopeView(camera.right, camera.up, camera.back, camera.tan_half_v, zoom);
    if (noted && zoom >= kScopeZoom) ++g_scope_unzoomed;  // the periodic log: whether its shaders are a scope's alone
    if (noted && zoom < kScopeZoom) {
      if (g_scope_research_frame) ScopeProbe("before the scope's pass", t->texture);
      g_scope.Apply(g_device->device(), g_device->context(), t->texture, t->view, camera);
      t->written = ++g_target_writes;
      g_draw->TargetsChanged();
      {  // the game's reticle, drawn later with the HUD, cut to the lens (until the present)
        const TextureDesc& d = t->texture->GetDesc();
        int l, tp, r, b;
        Scope::LensSquare(d.Width, d.Height, l, tp, r, b);
        g_draw->SetScopeClip(l, tp, r, b);
      }
      g_scope_mask_seen = true;
      if (g_scope_research_frame) {
        REXLOG_INFO("[native_x_renderer] scope research: the scope's pass on {}", fmt::ptr(t));
        ScopeProbe("after the scope's pass", t->texture);
        g_scope_probe_texture = t->texture;
      }
    }
  }
  // D168 research: in a logged frame, the output's pixels after each of its draws (which of them paints the game's black
  // ring and its reticle in a steady scoped frame: there the game's composite and HUD, no mask pass, run v41scope13).
  if (g_scope_research_frame && targets[0] && targets[0]->texture && targets[0]->samples == 1 &&
      targets[0]->guest_width == g_profile->output_width && targets[0]->guest_height == g_profile->output_height &&
      g_scope_draw_probes < 220) {
    ++g_scope_draw_probes;
    ScopeProbe(fmt::format("after draw {}", g_scope_draw_probes).c_str(), targets[0]->texture);
  }
  // D113: which depth resolve's words the draw read, for a depth target at a color target's EDRAM.
  uint32_t packed = g_draw->TakePackedRead();
  const ResolvedTexture* words = packed ? g_resolved.FindNewest(packed) : nullptr;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!targets[i]) continue;
    targets[i]->words_from = words ? packed : 0;
    targets[i]->words_from_resolve = words ? words->last_resolve : 0;
  }
  // D124: a draw of the 2D layer after the view, its inline vertices' positions (big-endian floats, x and y first) for
  // frame generation's motion; its kind: its shaders, vertex count, stride and primitive type.
  if (g_frame_generation && !g_present_with_swaps && g_profile->overlay_motion && g_view_ended && c.copy_count == 1 &&
      c.programs[0]) {
    uint32_t initiator = g_registers->values[rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR];
    uint32_t primitive = initiator & 0x3F, count = initiator >> 16;
    const Command::Copy& copy = c.copies[0];
    uint32_t stride = count ? copy.size / count : 0;
    if (stride >= 8) {
      g_overlay_positions.resize(size_t(count) * 2);
      const uint8_t* data = c.data.data() + copy.offset;
      for (uint32_t i = 0; i < count * 2; ++i) {
        uint32_t word;
        std::memcpy(&word, data + size_t(i / 2) * stride + (i & 1) * 4, 4);
        word = rex::byte_swap(word);
        std::memcpy(&g_overlay_positions[i], &word, 4);
      }
      struct {
        uint64_t vertex_shader, pixel_shader;
        uint32_t count, stride, primitive, unused;
      } kind{c.programs[0]->hash, c.programs[1] ? c.programs[1]->hash : 0, count, stride, primitive, 0};
      g_overlay_motion.Add(XXH3_64bits(&kind, sizeof(kind)), c.programs[0]->hash, primitive, g_overlay_positions.data(),
                           count);
    }
  }
  if (g_auto_views && g_draw->TakeSceneStarted()) {  // D119: this draw started the main 3D view
    g_scene_view = scene = true;
    g_scene_color = g_scene_depth = nullptr;
  }
  if (scene && targets[0] && targets[0]->guest_width == g_profile->output_width &&
      targets[0]->guest_height == g_profile->output_height) {
    g_scene_color = targets[0];
    g_scene_color_surface = Tiled(c.surfaces[0]);
    if (targets[4]) g_scene_depth = targets[4];
  }
}

// ClearF(dev, flags, rect, color*, Z (f1), stencil): bits 0-3 clear RT0-3, 0x10 depth, 0x20 stencil. The
// targets are the ones bound when ClearF ran, and the color what it pointed to then.
// A rectangle of guest pixels on a host target at its scale (the render scale, D37; the reduced one in an upscaled
// view, D49).
Blitter::Rect HostRect(const RenderScale& scale, const Blitter::Rect& r) {
  int32_t x0 = scale.HostX(r.x), y0 = scale.HostY(r.y);
  return Blitter::Rect{x0, y0, uint32_t(scale.HostX(int64_t(r.x) + r.width) - x0),
                       uint32_t(scale.HostY(int64_t(r.y) + r.height) - y0)};
}

// `r` (guest pixels) within the target, and whether it covers it whole.
bool ClipToTarget(const HostTarget& target, Blitter::Rect& r) {
  int32_t x1 = std::clamp(r.x, 0, int32_t(target.guest_width));
  int32_t y1 = std::clamp(r.y, 0, int32_t(target.guest_height));
  int32_t x2 = std::clamp(int32_t(int64_t(r.x) + r.width), x1, int32_t(target.guest_width));
  int32_t y2 = std::clamp(int32_t(int64_t(r.y) + r.height), y1, int32_t(target.guest_height));
  r = {x1, y1, uint32_t(x2 - x1), uint32_t(y2 - y1)};
  return x1 == 0 && y1 == 0 && r.width == target.guest_width && r.height == target.guest_height;
}

// The blitter's fills are single-sampled (D80): a multisampled target's part is not cleared, logged once.
bool FillableTarget(const HostTarget& target) {
  if (target.samples <= 1) return true;
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_WARN("[native_x_renderer] a clear of part of a {}x{} target with {} samples was skipped: not supported",
                target.guest_width, target.guest_height, target.samples);
  }
  return false;
}

// Clears a color target's rectangle (guest pixels, within it): the whole target through DiligentCore, a part of it
// filled by the blitter (D80).
void ClearColorRect(HostTarget* target, Blitter::Rect r, const float color[4]) {
  auto* context = g_device->context();
  if (ClipToTarget(*target, r)) {
    context->SetRenderTargets(1, &target->view, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->ClearRenderTarget(target->view, color, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  } else if (r.width && r.height && FillableTarget(*target)) {
    g_blit.FillColor(context, target->view, HostRect(target->scale, r), color);
  }
  target->written = ++g_target_writes;
  if (g_draw) g_draw->TargetsChanged();
}

// The same for a depth-stencil target's depth and/or stencil.
void ClearDepthStencilRect(HostTarget* target, Blitter::Rect r, bool depth, float z, bool stencil, uint8_t s) {
  auto* context = g_device->context();
  if (ClipToTarget(*target, r)) {
    CLEAR_DEPTH_STENCIL_FLAGS clear_flags = CLEAR_DEPTH_FLAG_NONE;
    if (depth) clear_flags |= CLEAR_DEPTH_FLAG;
    if (stencil) clear_flags |= CLEAR_STENCIL_FLAG;
    context->SetRenderTargets(0, nullptr, target->view, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->ClearDepthStencil(target->view, clear_flags, z, s, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  } else if (r.width && r.height && FillableTarget(*target)) {
    g_blit.FillDepthStencil(context, target->view, HostRect(target->scale, r), depth, z, stencil, s);
  }
  target->written = ++g_target_writes;
  if (g_draw) g_draw->TargetsChanged();
}

void ExecuteClear(const Command& c) {
  RendererTime time(&g_phase.clears);
  uint32_t flags = c.r[1], stencil = c.r[5];
  // A rectangle (x1, y1, x2, y2): a target it covers is cleared whole (D78; Kameo clears its scene with the
  // target's own rectangle), a part of one is filled (D80).
  auto rect = [&](const HostTarget* target) {
    if (!c.has_rect) return Blitter::Rect{0, 0, target->guest_width, target->guest_height};
    return Blitter::Rect{c.rect[0], c.rect[1], uint32_t(std::max(0, c.rect[2] - c.rect[0])),
                         uint32_t(std::max(0, c.rect[3] - c.rect[1]))};
  };
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(flags & (1u << i))) continue;
    HostTarget* target = TargetFor(Tiled(c.surfaces[i]));
    if (!target || !c.has_color) continue;
    ClearColorRect(target, rect(target), c.color.data());
  }
  if (flags & 0x30) {
    Binding binding = BindingOf(c) == Binding::kDepthOnly ? Binding::kDepthOnly : Binding::kUnknown;
    HostTarget* target = TargetFor(Tiled(c.surfaces[4]), binding);
    if (target) {
      ClearDepthStencilRect(target, rect(target), (flags & 0x10) != 0, float(c.f1), (flags & 0x20) != 0,
                            uint8_t(stencil));
    }
  }
  ++g_stats.clears;
}

// The bytes a resolve destination covers in guest memory from its address: the base level, every slice or face,
// laid out as its fetch constant says (dword 0 the pitch and tiling, dword 5 the packed mips). Resolves write
// level 0 only (below).
uint32_t ResolveGuestBytes(uint32_t fetch0, uint32_t fetch5, uint32_t format, uint32_t width, uint32_t height,
                           uint32_t slices, bool cube) {
  namespace rg = rex::graphics;
  rg::texture_util::TextureGuestLayout layout = rg::texture_util::GetGuestTextureLayout(
      cube ? rg::xenos::DataDimension::kCube : rg::xenos::DataDimension::k2DOrStacked, (fetch0 >> 22) & 0x1FF, width,
      height, cube ? 6 : slices, (fetch0 >> 31) != 0, rg::xenos::TextureFormat(format), ((fetch5 >> 11) & 1) != 0,
      true, 0);
  return layout.base.level_data_extent_bytes;
}

// The clears a resolve does after its copy (D80), over the rectangle it copied (guest pixels of the source):
// 0x100 its color source with the call's color, 0x200 the depth-stencil surface with Z (f1) and RB_DEPTH_CLEAR's
// stencil, as the EDRAM copy does them (the SDK's resolve: the color one only when the source is a color target).
void ResolveClears(const Command& c, HostTarget* source, const Blitter::Rect& copied) {
  uint32_t flags = c.r[1];
  if ((flags & 0x100) && c.has_color && source && !source->depth && (flags & 7) < 4) {
    ClearColorRect(source, copied, c.color.data());
  }
  if (flags & 0x200) {
    HostTarget* depth = (flags & 7) == 4 ? source : TargetFor(Tiled(c.surfaces[4]));
    if (depth && depth->depth) {
      ClearDepthStencilRect(depth, copied, true, float(c.f1), true, uint8_t(c.clear_stencil));
    }
  }
}

// Resolve(dev, flags, srcRect*, destTexture, destPoint*, level, slice, clearColor*, Z (f1), stencil, ...).
// flags: 0-3 = RT0-3, 4 = depth; 0x10-0x80 = one sample (fragment 0-3) instead of the average; 0x100, 0x200:
// clear the color, the depth and stencil afterwards (ResolveClears).
void UpscaleScene();

// D163: the night's lights need the camera: night on and lamps wanted (asked on every draw: no lock).
bool NightLightsWanted() {
  return (g_night_lamps_present.load(std::memory_order_relaxed) || !g_night_model_counts.empty()) &&
         REXCVAR_GET(native_x_renderer_night) && REXCVAR_GET(native_x_renderer_night_lamps) > 0.0;
}

// D163: the terrain's camera kept for the night's lights (at the AO's moment, before the terrain's frame is reset): its
// world-view-projection (the world in the level's own units), viewport and NDC transform, and the view's host size.
void NoteNightCamera() {
  if (!g_device || !g_terrain.valid || g_night_camera.frame == g_device->frames_presented()) return;
  if (!REXCVAR_GET(native_x_renderer_night)) return;
  std::memcpy(g_night_camera.wvp, g_terrain.wvp, sizeof(g_night_camera.wvp));
  g_night_camera.viewport = g_terrain.viewport;
  for (int i = 0; i < 3; ++i) {
    g_night_camera.ndc_scale[i] = g_terrain.ndc_scale[i];
    g_night_camera.ndc_offset[i] = g_terrain.ndc_offset[i];
  }
  g_night_camera.view_width = g_scene_depth ? float(g_scene_depth->scale.HostWidth(g_profile->output_width))
                                            : g_terrain.viewport.Width;
  g_night_camera.view_height = g_scene_depth ? float(g_scene_depth->scale.HostHeight(g_profile->output_height))
                                             : g_terrain.viewport.Height;
  g_night_camera.frame = g_device->frames_presented();
}

// D163: this frame's camera for the night pass (`to_u`, `to_v`: the night target's texture coordinates to the view's
// 0 to 1), and the lamps it lights and shows: the nearest whose light reaches into the view, and the nearest bulbs in
// it. False without this frame's camera.
bool BuildNightCamera(NightFrame& f, float to_u, float to_v, const NightSettings& s) {
  const NightCamera& cam = g_night_camera;
  if (!g_device || cam.frame != g_device->frames_presented()) return false;
  const float(*m)[4] = cam.wvp;
  // World from clip x, y and w: the inverse of those three rows' 3x3 part, in double precision.
  const double a[3][3] = {{m[0][0], m[0][1], m[0][2]}, {m[1][0], m[1][1], m[1][2]}, {m[3][0], m[3][1], m[3][2]}};
  const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                     a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  if (!(std::abs(det) > 1e-12)) return false;
  double inv[3][3];
  inv[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
  inv[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
  inv[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
  inv[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
  inv[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
  inv[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
  inv[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
  inv[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
  inv[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
  const double t[3] = {m[0][3], m[1][3], m[3][3]};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) f.inverse[r][c] = float(inv[r][c]);
    f.inverse_t[r] = float(t[r]);
    f.eye[r] = float(-(inv[r][0] * t[0] + inv[r][1] * t[1] + inv[r][2] * t[2]));  // where clip x, y and w are 0
  }
  // The night target's texture coordinates to NDC: to the view's 0 to 1, its depth target's pixels, the terrain's
  // viewport (DiligentCore's, Direct3D's way round: NDC y +1 at its top), then the translated shaders' NDC transform
  // undone.
  const Diligent::Viewport& vp = cam.viewport;
  if (!(vp.Width > 0.0f && vp.Height > 0.0f && cam.ndc_scale[0] != 0.0f && cam.ndc_scale[1] != 0.0f)) return false;
  f.to_ndc[0] = to_u * cam.view_width * 2.0f / (vp.Width * cam.ndc_scale[0]);
  f.to_ndc[1] = -to_v * cam.view_height * 2.0f / (vp.Height * cam.ndc_scale[1]);
  f.to_ndc[2] = (-vp.TopLeftX * 2.0f / vp.Width - 1.0f - cam.ndc_offset[0]) / cam.ndc_scale[0];
  f.to_ndc[3] = (vp.TopLeftY * 2.0f / vp.Height + 1.0f - cam.ndc_offset[1]) / cam.ndc_scale[1];
  f.camera = true;
  // The lamps: their light where its sphere meets the view (clip -w <= x, y <= w, w >= 0), nearest first; their bulbs
  // where they are in the view, nearest first.
  g_night_frame_lights.clear();
  g_night_frame_sprites.clear();
  if (!(s.lamps > 0.0f)) return true;
  struct Near {
    float distance;
    uint32_t index;
  };
  static std::vector<Near> lit, shown;
  lit.clear(), shown.clear();
  // The level's lamps, and the models' placed from this frame's draws of them (their world matrices).
  static std::vector<NightLamp> lamps;
  {
    std::lock_guard lock(g_night_lamps_mutex);
    lamps = g_night_lamps;
  }
  uint32_t placed_models = 0;
  for (size_t d = 0; d < g_night_model_draws.size(); ++d) {
    const NightModelDraw& draw = g_night_model_draws[d];
    const NightModelLamps* model = nullptr;
    for (const NightModelLamps& mm : g_night_models_render)
      if (mm.index_count == draw.index_count) model = &mm;
    if (!model) continue;
    // The draw's world matrix (rows: world = R x local + T).
    const Mat4d& mm = draw.m;
    double R[3][3], T[3];
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) R[r][c] = mm[r * 4 + c];
      T[r] = mm[r * 4 + 3];
    }
    // The same model drawn twice in the frame: once.
    bool seen = false;
    for (size_t e = 0; e < d && !seen; ++e) {
      const Mat4d& o = g_night_model_draws[e].m;
      seen = g_night_model_draws[e].index_count == draw.index_count && std::abs(o[3] - mm[3]) < 0.05 &&
             std::abs(o[7] - mm[7]) < 0.05 && std::abs(o[11] - mm[11]) < 0.05;
    }
    if (seen) continue;
    ++placed_models;
    {
      static uint64_t logged_at = 0;
      if (g_device->frames_presented() >= logged_at + 600) {
        logged_at = g_device->frames_presented();
        REXLOG_INFO("[native_x_renderer] night: a model with lamps ({} indices) at {:.2f} {:.2f} {:.2f}, its axes x {:.2f} "
                    "{:.2f} {:.2f} y {:.2f} {:.2f} {:.2f} z {:.2f} {:.2f} {:.2f}; eye {:.1f} {:.1f} {:.1f} (D163)",
                    draw.index_count, T[0], T[1], T[2], R[0][0], R[1][0], R[2][0], R[0][1], R[1][1], R[2][1], R[0][2],
                    R[1][2], R[2][2], f.eye[0], f.eye[1], f.eye[2]);
      }
    }
    for (uint32_t i = 0; i < model->lamp_count && i < 4; ++i) {
      const NightLamp& local = model->lamps[i];
      NightLamp w = local;
      for (int r = 0; r < 3; ++r) {
        w.position[r] = float(R[r][0] * local.position[0] + R[r][1] * local.position[1] + R[r][2] * local.position[2] + T[r]);
        w.direction[r] = float(R[r][0] * local.direction[0] + R[r][1] * local.direction[1] + R[r][2] * local.direction[2]);
      }
      const float n = std::sqrt(w.direction[0] * w.direction[0] + w.direction[1] * w.direction[1] +
                                w.direction[2] * w.direction[2]);
      for (float& v : w.direction) v = n > 1e-4f ? v / n : 0.0f;
      lamps.push_back(w);
    }
  }
  g_night_models_placed += placed_models;
  const float planes[5][4] = {
      {m[3][0] + m[0][0], m[3][1] + m[0][1], m[3][2] + m[0][2], m[3][3] + m[0][3]},
      {m[3][0] - m[0][0], m[3][1] - m[0][1], m[3][2] - m[0][2], m[3][3] - m[0][3]},
      {m[3][0] + m[1][0], m[3][1] + m[1][1], m[3][2] + m[1][2], m[3][3] + m[1][3]},
      {m[3][0] - m[1][0], m[3][1] - m[1][1], m[3][2] - m[1][2], m[3][3] - m[1][3]},
      {m[3][0], m[3][1], m[3][2], m[3][3]}};
  for (uint32_t i = 0; i < lamps.size(); ++i) {
    const NightLamp& l = lamps[i];
    const float dx = l.position[0] - f.eye[0], dy = l.position[1] - f.eye[1], dz = l.position[2] - f.eye[2];
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    bool in_view = distance < l.range + 250.0f;
    for (int p = 0; p < 5 && in_view; ++p) {
      const float n = std::sqrt(planes[p][0] * planes[p][0] + planes[p][1] * planes[p][1] + planes[p][2] * planes[p][2]);
      const float side = planes[p][0] * l.position[0] + planes[p][1] * l.position[1] + planes[p][2] * l.position[2] + planes[p][3];
      if (n > 0.0f && side < -l.range * n) in_view = false;
    }
    if (in_view) lit.push_back({distance, i});
    if (l.glow > 0.0f && distance < 400.0f) shown.push_back({distance, i});
  }
  auto nearest = [](std::vector<Near>& v, size_t keep) {
    std::sort(v.begin(), v.end(), [](const Near& x, const Near& y) { return x.distance < y.distance; });
    if (v.size() > keep) v.resize(keep);
  };
  nearest(lit, Night::kMaxLights);
  nearest(shown, Night::kMaxSprites * 2);
  {
    static uint64_t logged_at = 0;
    const uint64_t frame = g_device->frames_presented();
    if (frame >= logged_at + 600) {
      logged_at = frame;
      const NightLamp* l = lit.empty() ? nullptr : &lamps[lit[0].index];
      REXLOG_INFO("[native_x_renderer] night: eye {:.1f} {:.1f} {:.1f}; {} lamps lit, the nearest at {:.1f} {:.1f} {:.1f} "
                  "({:.1f} m); to NDC {:.4f} {:.4f} {:.4f} {:.4f}; viewport {:.0f} {:.0f} {:.0f}x{:.0f}, view {:.0f}x{:.0f} "
                  "(D163)",
                  f.eye[0], f.eye[1], f.eye[2], lit.size(), l ? l->position[0] : 0.0f, l ? l->position[1] : 0.0f,
                  l ? l->position[2] : 0.0f, lit.empty() ? 0.0f : lit[0].distance, f.to_ndc[0], f.to_ndc[1],
                  f.to_ndc[2], f.to_ndc[3], vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, cam.view_width,
                  cam.view_height);
    }
  }
  const float rad = 3.14159265f / 180.0f;
  for (const Near& n : lit) {
    const NightLamp& l = lamps[n.index];
    NightLight o;
    std::memcpy(o.position, l.position, sizeof(o.position));
    o.range = l.range;
    std::memcpy(o.color, l.color, sizeof(o.color));
    std::memcpy(o.direction, l.direction, sizeof(o.direction));
    o.cos_outer = std::cos(l.cone_outer * rad);
    o.cos_inner = std::cos(l.cone_inner * rad);
    g_night_frame_lights.push_back(o);
  }
  const float row_x = std::sqrt(m[0][0] * m[0][0] + m[0][1] * m[0][1] + m[0][2] * m[0][2]);
  const float row_y = std::sqrt(m[1][0] * m[1][0] + m[1][1] * m[1][1] + m[1][2] * m[1][2]);
  for (const Near& n : shown) {
    if (g_night_frame_sprites.size() >= Night::kMaxSprites) break;
    const NightLamp& l = lamps[n.index];
    const float* p = l.position;
    const float cx = m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3];
    const float cy = m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3];
    const float cw = m[3][0] * p[0] + m[3][1] * p[1] + m[3][2] * p[2] + m[3][3];
    if (!(cw > 0.2f)) continue;
    const float nx = cx / cw, ny = cy / cw;
    if (std::abs(nx) > 1.1f || std::abs(ny) > 1.1f) continue;
    NightSprite o;
    o.uv[0] = (nx - f.to_ndc[2]) / f.to_ndc[0];
    o.uv[1] = (ny - f.to_ndc[3]) / f.to_ndc[1];
    // A halo eight times the bulb's radius, and never under four pixels of the target across.
    const float halo = l.glow * 8.0f;
    const float min_uv = 4.0f / std::max(1.0f, cam.view_width / std::max(to_u, 1e-3f));
    o.radius[0] = std::max(std::abs(halo * row_x / cw / f.to_ndc[0]), min_uv);
    o.radius[1] = std::max(std::abs(halo * row_y / cw / f.to_ndc[1]), min_uv * std::abs(f.to_ndc[0] / f.to_ndc[1]));
    o.depth = 0.1f / cw;
    const float through = std::exp(-n.distance / std::max(s.haze_m, 1.0f));  // the haze between
    for (int c = 0; c < 3; ++c) o.color[c] = l.color[c] * 0.35f * through;
    g_night_frame_sprites.push_back(o);
  }
  return true;
}

// D163: the effects' layer (`from`: the source's host rectangle) copied for the night pass, with its mips for the glow.
void CopyNightGlow(HostTarget* source, const Blitter::Rect& from) {
  using namespace Diligent;
  if (!g_device || !source->srv || !from.width || !from.height) return;
  if (!g_night_glow || g_night_glow->GetDesc().Width != from.width || g_night_glow->GetDesc().Height != from.height) {
    g_night_glow.Release();
    TextureDesc d;
    d.Name = "NativeXRenderer night effects";
    d.Type = RESOURCE_DIM_TEX_2D;
    d.Width = from.width;
    d.Height = from.height;
    d.MipLevels = 0;  // all of them
    d.Format = TEX_FORMAT_RGBA16_FLOAT;
    d.Usage = USAGE_DEFAULT;
    d.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
    d.MiscFlags = MISC_TEXTURE_FLAG_GENERATE_MIPS;
    g_device->device()->CreateTexture(d, nullptr, &g_night_glow);
    if (!g_night_glow) return;
  }
  const Blitter::Rect to{0, 0, from.width, from.height};
  g_blit.Copy(g_device->context(), source->srv, Blitter::Source::kColor, from,
              g_night_glow->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET), to, Blitter::Options{});
  g_device->context()->GenerateMips(g_night_glow->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  g_night_glow_frame = g_device->frames_presented();
  ++g_night_glow_frames;
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("[native_x_renderer] night: the effects' layer found, {}x{} on the host ({}x{} in the game), for their "
                "own light (D163)", from.width, from.height, source->guest_width, source->guest_height);
  }
  if (g_draw) g_draw->TargetsChanged();
}

// D132, D133: at the end of the main view's opaque draws, the ambient occlusion (GTAO) multiplied into the scene's
// colour, then the global illumination (SSGI) added to it, both King Kong Recompiled's, with one camera.
void ApplyOpaqueEffects() {
  if (!g_scene_color || !g_scene_depth || !g_draw || !g_scene_color->view) return;
  // The grass prototype (grass.h), before the AO darkens the ground under it.
  if (GrassOn() && g_scene_depth->view) {
    RendererTime time(&g_phase.effects);
    if (g_grass.Apply(g_device->context(), g_terrain, g_scene_color->texture, g_scene_color->view,
                      g_scene_depth->texture, g_scene_depth->view, g_device->frames_presented())) {
      g_draw->TargetsChanged();
    }
  }
  NoteNightCamera();  // D163
  g_terrain.Reset();
  GtaoSettings ao = GtaoSettings::FromCvars();
  ao.temporal_upscaler = g_temporal;
  SsgiSettings gi = SsgiSettings::FromCvars();
  gi.temporal_upscaler = g_temporal;
  const bool ao_on = g_gtao_on && ao.mode > 0;
  const bool gi_on = g_ssgi_on && gi.mode > 0;
  g_gtao.Configure(ao);
  g_ssgi.Configure(gi);
  if (!ao_on && !gi_on) return;
  // The projection most of the view's draws agree on (the first draw is the weapon's own camera in Battlefield
  // 1943).
  GtaoProjection projection;
  float z_min = 0.0f, z_max = 1.0f;
  if (!g_draw->TakeProjection(projection, z_min, z_max)) return;
  RendererTime time(&g_phase.effects);
  const uint32_t width = g_scene_color->scale.HostWidth(g_profile->output_width);
  const uint32_t height = g_scene_color->scale.HostHeight(g_profile->output_height);
  const uint64_t frame = g_device->frames_presented();
  // Without an upscaler the camera vote is the effects' own (their local histories' reprojection); with one,
  // UpscaleScene takes it later in the view.
  Mat4d vote;
  uint32_t votes = 0, offers = 0;
  const bool voted = !g_temporal && g_camera_vote && g_draw->TakeCameraVote(vote, votes, offers);
  if (voted) g_blur_vote = vote, g_blur_voted = true;  // the motion blur's too, at the view's end
  bool applied = false;
  if (ao_on) {
    Gtao::Inputs in;
    in.projection = projection;
    in.depth = g_scene_depth->texture;
    in.color = g_scene_color->view;
    in.width = width;
    in.height = height;
    in.scale = g_scene_color->scale;
    in.z_min = z_min;
    in.z_max = z_max;
    // The radius in near plane distances (KK's 6, 8 or 12) from the depth curve's own near plane, where the stored
    // depth reaches 1: the view's projection alone gives the world partition's (Battlefield 1943: 21.34, its
    // weapon and the world together store 0.1 / distance), and a radius of 256 units darkened everything alike.
    if (ao.radius <= 0.0f) {
      const GtaoProjection& p = in.projection;
      const float zs = z_max - z_min;
      const float d = zs != 0.0f ? (1.0f - z_min) / zs - p.a : 0.0f;
      const float near_plane = d != 0.0f ? p.b / d : 0.0f;
      if (near_plane > 0.0f && std::isfinite(near_plane) && near_plane < p.near_plane()) {
        GtaoSettings s = ao;
        s.radius = Gtao::RadiusInNearPlanes(s.strength) * near_plane * std::max(s.radius_scale, 0.01f);
        g_gtao.Configure(s);
      }
    }
    in.frame = frame;
    if (voted) {
      in.reprojection = &vote;
      in.votes = votes;
      in.offers = offers;
    }
    applied = g_gtao.Apply(g_device->context(), in) || applied;
  }
  if (gi_on) {
    Ssgi::Inputs in;
    in.projection = projection;
    in.depth = g_scene_depth->texture;
    in.color = g_scene_color->view;
    in.color_texture = g_scene_color->texture;
    in.width = width;
    in.height = height;
    in.scale = g_scene_color->scale;
    in.z_min = z_min;
    in.z_max = z_max;
    in.frame = frame;
    if (voted) {
      in.reprojection = &vote;
      in.votes = votes;
      in.offers = offers;
    }
    in.ao_on = ao_on;  // KK's rule: with the AO on, the GI only adds light
    applied = g_ssgi.Apply(g_device->context(), in) || applied;
  }
  if (applied) g_draw->TargetsChanged();
}

void ExecuteResolve(const Command& c) {
  RendererTime time(&g_phase.resolves);
  uint32_t flags = c.r[1], level = c.r[5], slice = c.r[6];
  GuestSurface surface = Tiled(c.surfaces[0]);
  // D119: the profile's upscale point, the copy of the finished 3D scene of the output size: DLSS or FSR 3.1 on the
  // view first, so that the copy takes their image. No view again until the next Present. Before the copy's source is
  // chosen: with reduced targets (D121) it is then the upscaled image's, at the render scale.
  if (g_auto_views && g_scene_view && c.caller == g_profile->upscale_resolve_caller && surface && !surface.depth &&
      surface.width == g_profile->output_width && surface.height == g_profile->output_height) {
    UpscaleScene();
    g_auto_view_done = true;
    g_auto_reduced = false;
  }
  // D132: the ambient occlusion, at the profile's depth copy that ends the view's opaque draws (once a frame).
  if ((g_gtao_on || g_ssgi_on) && g_scene_view && !g_gtao_done && c.caller == g_profile->ao_resolve_caller && surface.depth &&
      surface.width == g_profile->output_width && surface.height == g_profile->output_height) {
    g_gtao_done = true;
    ApplyOpaqueEffects();
  }
  HostTarget* source = TargetFor(surface);
  // D168 research (NXR_SCOPE_RESEARCH): in a logged frame, each copy of an output size color target: from which target,
  // and four of its pixels (where the screen's picture comes from in a scoped frame).
  if (g_scope_research_frame && source && source->texture && !surface.depth &&
      surface.width == g_profile->output_width && surface.height == g_profile->output_height) {
    REXLOG_INFO("[native_x_renderer] scope research: a resolve from {} (caller {:08X}{})", fmt::ptr(source), c.caller,
                c.scene_caller ? ", the scene's copy" : "");
    ScopeProbe("the resolve's source", source->texture);
  }
  // D163: the night's camera from the terrain, if the opaque effects did not take it (they reset the terrain's frame).
  if (c.caller == g_profile->ao_resolve_caller && surface.depth) NoteNightCamera();
  // D162: the world's depth for the night's haze, at the same depth copy (once a frame): the first person weapon's
  // draws clear the depth after it, so on foot the view's depth at its end held only the weapon and the night took
  // everything else for the far sky (run nightzip1). Copied into a float texture's red channel.
  if (REXCVAR_GET(native_x_renderer_night) && c.caller == g_profile->ao_resolve_caller && surface.depth && source &&
      source->srv && source->texture && surface.width == g_profile->output_width &&
      surface.height == g_profile->output_height && g_night_world_frame != g_device->frames_presented()) {
    const TextureDesc& d = source->texture->GetDesc();
    if (!g_night_world || g_night_world->GetDesc().Width != d.Width || g_night_world->GetDesc().Height != d.Height) {
      g_night_world.Release();
      TextureDesc w;
      w.Name = "NativeXRenderer night world depth";
      w.Type = RESOURCE_DIM_TEX_2D;
      w.Width = d.Width;
      w.Height = d.Height;
      w.Format = TEX_FORMAT_R32_FLOAT;
      w.Usage = USAGE_DEFAULT;
      w.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
      g_device->device()->CreateTexture(w, nullptr, &g_night_world);
    }
    if (g_night_world) {
      Blitter::Options options;
      options.samples = source->samples;
      const Blitter::Rect all{0, 0, d.Width, d.Height};
      g_blit.Copy(g_device->context(), source->srv, source->samples > 1 ? Blitter::Source::kDepthMsaa : Blitter::Source::kDepth,
                  all, g_night_world->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET), all, options);
      g_night_world_frame = g_device->frames_presented();
      g_night_world_uv[0] = float(source->scale.HostWidth(g_profile->output_width)) / float(d.Width);
      g_night_world_uv[1] = float(source->scale.HostHeight(g_profile->output_height)) / float(d.Height);
      if (g_draw) g_draw->TargetsChanged();
    }
  }
  // The rectangle the copy reads, then narrowed to what it copied: the clears' rectangle, which they get however
  // the copy goes.
  Blitter::Rect copied{0, 0, surface.width, surface.height};
  if (c.has_source_rect) {
    copied = {c.source_rect[0], c.source_rect[1], uint32_t(std::max(0, c.source_rect[2] - c.source_rect[0])),
              uint32_t(std::max(0, c.source_rect[3] - c.source_rect[1]))};
  }
  struct Clears {
    const Command& c;
    HostTarget*& source;  // the target read, which a multisampled surface's alias can replace (D95)
    const Blitter::Rect& copied;
    ~Clears() {
      if (c.r[1] & 0x300) ResolveClears(c, source, copied);
    }
  } clears{c, source, copied};
  if (!source || !c.has_texture || level) {
    ++g_stats.skipped_resolves;
    return;
  }
  if (surface.depth) CarryColorIntoDepth(surface, source);
  // The destination texture's fetch constant: dword 1 base and format, dword 2 size, dword 5 the dimension. The
  // header holds the CPU address; draws sample the GPU address the device puts in the fetch constant.
  uint32_t fetch1 = c.fetch[1], fetch2 = c.fetch[2];
  uint32_t dimension = (c.fetch[5] >> 9) & 3;
  // A cube's faces are the slices of a host cube texture, `slice` the face (D51); its size is encoded as a
  // 2D texture's, with a stack depth of 6.
  bool cube = dimension == uint32_t(rex::graphics::xenos::DataDimension::kCube);
  if (dimension != uint32_t(rex::graphics::xenos::DataDimension::k2DOrStacked) && !cube) {
    // 1D and 3D destinations: not yet (a 3D texture's size is encoded differently, and it would need a host
    // texture of its dimension).
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_WARN("[native_x_renderer] resolve to a texture of dimension {}: not supported yet", dimension);
    }
    ++g_stats.skipped_resolves;
    return;
  }
  uint32_t address = GpuAddress(fetch1 & 0xFFFFF000u), format = fetch1 & 0x3F;
  uint32_t width = (fetch2 & 0x1FFF) + 1, height = ((fetch2 >> 13) & 0x1FFF) + 1, slices = (fetch2 >> 26) + 1;
  // From a shadow map, into a texture at its scale (D92).
  ResolvedTexture* target = g_resolved.GetOrCreate(address, format, width, height, slices, cube, source->shadow);
  ITextureView* target_view = target ? target->SliceView(slice) : nullptr;
  if (!target_view) {
    ++g_stats.skipped_resolves;
    return;
  }

  Blitter::Rect from{0, 0, surface.width, surface.height};
  if (c.has_source_rect) {
    int32_t x1 = c.source_rect[0], y1 = c.source_rect[1], x2 = c.source_rect[2], y2 = c.source_rect[3];
    from = {x1, y1, uint32_t(std::max(0, x2 - x1)), uint32_t(std::max(0, y2 - y1))};
  }
  Blitter::Rect to{0, 0, from.width, from.height};
  if (c.has_dest_point) {
    to.x = c.dest_point[0];
    to.y = c.dest_point[1];
  }
  // Clip to both textures, in guest pixels.
  from.width = std::min({from.width, source->guest_width - std::min(source->guest_width, uint32_t(from.x)),
                         target->guest_width - std::min(target->guest_width, uint32_t(to.x))});
  from.height = std::min({from.height, source->guest_height - std::min(source->guest_height, uint32_t(from.y)),
                          target->guest_height - std::min(target->guest_height, uint32_t(to.y))});
  to.width = from.width;
  to.height = from.height;
  copied = from;
  // A multisampled color surface whose EDRAM was last written through a single-sampled one (D95): the copy reads
  // that target, its pixels averaged by the filtered copy at half the size (2x2 for 4x MSAA, 1x2 for 2x) as the
  // resolve averages the samples, and the resolve's clears go there too. A depth surface likewise (D112), with one
  // pixel of each block taken (the nearest copy), not the average, whichever sample the resolve asked for.
  uint32_t samples_x = 1, samples_y = 1;
  HostTarget* alias = g_targets.FindMsaaAlias(surface, samples_x, samples_y);
  bool aliased = alias && alias != source && alias->written > source->written;
  if (aliased) {
    Blitter::Rect a{from.x * int32_t(samples_x), from.y * int32_t(samples_y), from.width * samples_x,
                    from.height * samples_y};
    ClipToTarget(*alias, a);
    static bool logged[2] = {};
    if (!logged[surface.depth]) {
      logged[surface.depth] = true;
      REXLOG_INFO("[native_x_renderer] a {} resolve through a {}x{} surface with {}x MSAA reads the {}x{} target "
                  "drawn at the same EDRAM (base {}): {} ({})",
                  surface.depth ? "depth" : "color", surface.width, surface.height, surface.samples(),
                  alias->guest_width, alias->guest_height, surface.edram_base(),
                  surface.depth ? "one sample of each pixel" : "its samples averaged", surface.depth ? "D112" : "D95");
    }
    if (!surface.depth && ((flags >> 4) & 0xF) != 0) {
      static bool logged_fragment = false;
      if (!logged_fragment) {
        logged_fragment = true;
        REXLOG_WARN("[native_x_renderer] a resolve of one sample through an aliased MSAA surface: averaged instead");
      }
    }
    source = alias;
    copied = from = a;
  }
  // Then on the host: the source at its scale (the render scale, D37; the reduced one in an upscaled view,
  // D49; a shadow map's, D92), the destination at its own (the render scale, or the shadow maps'). At one scale,
  // with a fractional scale the two rectangles' rounded sizes can differ by a pixel: the copy takes the smaller.
  // From a reduced target the copy scales.
  from = HostRect(source->scale, from);
  to = HostRect(target->scale, to);
  bool scaled = source->reduced || aliased;
  // Into a cube's face, which is square on the host whatever the render scale's axes (targets.cpp): the copy scales.
  if (cube && (source->scale.x != target->scale.x || source->scale.y != target->scale.y)) scaled = true;
  if (scaled && source->samples > 1) {
    ++g_stats.skipped_resolves;  // reduced targets have no MSAA (NativeXRenderer draws without it)
    return;
  }
  from.width = std::min(from.width, source->width - std::min(source->width, uint32_t(from.x)));
  from.height = std::min(from.height, source->height - std::min(source->height, uint32_t(from.y)));
  to.width = std::min(to.width, target->width - std::min(target->width, uint32_t(to.x)));
  to.height = std::min(to.height, target->height - std::min(target->height, uint32_t(to.y)));
  if (!scaled) {
    from.width = to.width = std::min(from.width, to.width);
    from.height = to.height = std::min(from.height, to.height);
  }

  // What the copy itself does, from the RB_COPY_DEST_INFO the call wrote.
  uint32_t dest_info = c.dest_info;
  Blitter::Options options;
  options.samples = source->samples;
  if (uint32_t fragments = (flags >> 4) & 0xF) {
    options.sample = int32_t(std::countr_zero(fragments));
  }
  options.swap_red_blue = (dest_info >> 24) & 1;
  // A 2_10_10_10_FLOAT target is RGBA16F on the host, which keeps what its format would not: an alpha above
  // 1 that a draw wrote, for one (POTCAWE's journal, D50). The copy takes the guest's range, as the EDRAM's
  // packing does on the Xenos path.
  namespace xenos = rex::graphics::xenos;
  options.float_7e3 =
      !source->depth && xenos::GetStorageColorFormat(xenos::ColorRenderTargetFormat(surface.format())) ==
                            xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT;
  int32_t exp_bias = int32_t(dest_info << 10) >> 26;  // bits 16-21, signed
  // A 16-bit fixed-point source: its values within the guest's range before the copy (D139).
  {
    const auto storage = xenos::GetStorageColorFormat(xenos::ColorRenderTargetFormat(surface.format()));
    options.fixed16 = Fixed16Range() && !source->depth && (storage == xenos::ColorRenderTargetFormat::k_16_16 ||
                                                           storage == xenos::ColorRenderTargetFormat::k_16_16_16_16);
  }
  // A 16-bit fixed-point source holds the guest's -32 to 32 values (D130). Into a 16-bit fixed-point texture, whose
  // fetches read -1 to 1 for the same bits, a 32nd of them.
  const uint32_t dest_format = (dest_info >> 7) & 0x3F;
  const auto source_format = xenos::GetStorageColorFormat(xenos::ColorRenderTargetFormat(surface.format()));
  if (!source->depth &&
      (source_format == xenos::ColorRenderTargetFormat::k_16_16 ||
       source_format == xenos::ColorRenderTargetFormat::k_16_16_16_16) &&
      (dest_format == uint32_t(xenos::ColorFormat::k_16_16) || dest_format == uint32_t(xenos::ColorFormat::k_16_16_16_16)))
    exp_bias -= 5;
  options.scale = std::ldexp(1.0f, exp_bias);

  Blitter::Source kind = source->depth ? (source->samples > 1 ? Blitter::Source::kDepthMsaa : Blitter::Source::kDepth)
                                       : (source->samples > 1 ? Blitter::Source::kColorMsaa : Blitter::Source::kColor);
  if (scaled) kind = source->depth ? Blitter::Source::kDepthScaled : Blitter::Source::kColorScaled;
  g_blit.Copy(g_device->context(), source->srv, kind, from, target_view, to, options);
  // D168 research (NXR_SCOPE_RESEARCH): in a logged frame, where each color resolve went, and four of its pixels there.
  if (g_scope_research_frame && !source->depth && target->texture) {
    REXLOG_INFO("[native_x_renderer] scope research: resolved {}x{} of {} (caller {:08X}) into {:08X}, {}x{} format {}{}",
                copied.width, copied.height, fmt::ptr(source), c.caller, address, width, height, format,
                aliased ? ", read through its MSAA alias" : "");
    ScopeProbe("the resolve's texture", target->texture);
  }
  // D163: the effects' layer for the night: a colour copy of half the output's size after the world's opaque draws (the
  // night's world depth taken this frame) whose last draw blended: Battlefield 1943's half size pass of its emissive
  // particles (muzzle flashes, tracers, fire, explosions: run nightrd2, its premultiplied draws then one draw over the
  // scene); its other half size pass, the sky's, draws without blending.
  // (Before the scene's copy: every copy goes through the scene copy's caller, the bloom chain's half size ones after it.)
  if (REXCVAR_GET(native_x_renderer_night) && REXCVAR_GET(native_x_renderer_night_glow) > 0.0 && !source->depth &&
      !source->shadow && !scaled && source->samples == 1 && !g_night_pending && slice == 0 && !cube &&
      g_night_world_frame == g_device->frames_presented() && source->last_draw_blended &&
      copied.width * 2 == g_profile->output_width && copied.height * 2 == g_profile->output_height) {
    CopyNightGlow(source, from);
  }
  if (REXCVAR_GET(native_x_renderer_night_debug) == 3 && !source->depth && !source->shadow) {
    static uint32_t logged = 0;
    if (logged < 400 && copied.width * 4 >= g_profile->output_width && copied.width < g_profile->output_width) {
      ++logged;
      REXLOG_INFO("[native_x_renderer] night test: resolve {}x{} (surface {}x{}, target {}x{}) blended {} scene {} "
                  "after the world {} samples {} scaled {} caller {:08X} frame {}",
                  copied.width, copied.height, surface.width, surface.height, source->guest_width,
                  source->guest_height, source->last_draw_blended, c.scene_caller,
                  g_night_world_frame == g_device->frames_presented(), source->samples, scaled, c.caller,
                  g_device->frames_presented());
    }
  }
  // A depth resolve that a fetch reads as k_8_8_8_8 (for the stencil): the words too (D64).
  if (source->depth && target->read_packed && !scaled && !cube && source->stencil_srv) {
    if (ITextureView* packed = target->PackedSliceView(g_device->device(), slice)) {
      Blitter::Options packed_options;
      packed_options.stencil = source->stencil_srv;
      packed_options.depth_float24 = source->depth_float24;
      g_blit.Copy(g_device->context(), source->srv, Blitter::Source::kDepthStencilPacked, from, packed, to,
                  packed_options);
      target->packed_written = true;
    }
  }
  // Frame generation (D123): the finished scene, before the HUD, copied for the frame's HUD-less picture.
  if (g_frame_generation && !g_profile->overlay_motion && c.scene_caller && slice == 0 &&
      target->guest_width == g_profile->output_width &&
      target->guest_height == g_profile->output_height && target->texture) {
    const TextureDesc& resolved = target->texture->GetDesc();
    // D141: one copy per size and format, two kept (a frame can resolve two scenes of different formats; one copy made
    // again for each freed and allocated device memory twice a frame).
    static RefCntAutoPtr<ITexture> copies[2];
    static uint32_t next = 0;
    ITexture* found = nullptr;
    for (auto& t : copies) {
      if (t && t->GetDesc().Width == resolved.Width && t->GetDesc().Height == resolved.Height &&
          t->GetDesc().Format == resolved.Format) {
        found = t;
      }
    }
    if (!found) {
      RefCntAutoPtr<ITexture>& slot = copies[next];  // the one made longer ago
      next ^= 1;
      slot.Release();
      TextureDesc copy = resolved;
      copy.Name = "NativeXRenderer HUD-less scene";
      copy.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
      copy.MipLevels = 1;
      copy.ArraySize = 1;
      copy.Type = RESOURCE_DIM_TEX_2D;
      copy.MiscFlags = MISC_TEXTURE_FLAG_NONE;
      g_device->device()->CreateTexture(copy, nullptr, &slot);
      found = slot;
    }
    g_hudless_copy = found;
    // Drawn, as the renderer's other copies are: with a transfer (CopyTexture) here, POTCAWE's frames came 60 ms apart
    // every few seconds, for a reason not found.
    if (g_hudless_copy && target->srv) {
      Blitter::Rect all{0, 0, resolved.Width, resolved.Height};
      g_blit.Copy(g_device->context(), target->srv, Blitter::Source::kColor, all,
                  g_hudless_copy->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET), all);
      g_hudless_this_frame = true;
    }
  }
  // The copy of the finished scene (the profile's scene_resolve_caller): antialiased in place (D41).
  // It is of the game's output size (D65).
  if (g_temporal && c.scene_caller && slice == 0)
    Upscaler::sharpness = float(REXCVAR_GET(native_x_renderer_upscaler_sharpness));
  if (g_aa_live && c.scene_caller && slice == 0) {  // the method changed in the settings: taken here, at once
    std::string name = REXCVAR_GET(native_x_renderer_antialiasing);
    if (name != g_aa_name) {
      g_aa_name = name;
      const bool temporal = name == "dlaa" || name.rfind("dlss", 0) == 0 || name.rfind("fsr", 0) == 0 ||
                            name.rfind("xess", 0) == 0 || name.rfind("taa", 0) == 0;
      if (!temporal) g_aa.Initialize(g_device->device(), Antialiasing::ParseMode(name));
    }
  }
  if (g_aa.mode() != Antialiasing::Mode::kOff && c.scene_caller && slice == 0 &&
      target->guest_width == g_profile->output_width &&
      target->guest_height == g_profile->output_height &&
      !(g_profile->scene_resolve_once && g_scene_antialiased)) {
    g_aa.Apply(g_device->context(), target->texture, target_view);
    g_scene_antialiased = true;
  }
  // Profile::box_2d_after_composite: the scene's copy; the composite comes next, then the HUD.
  if (g_profile->box_2d_after_composite && c.scene_caller && slice == 0 &&
      target->guest_width == g_profile->output_width && target->guest_height == g_profile->output_height) {
    g_hud_phase = 1;
    g_scene_copied_this_frame = true;
  }
  // Night (D162): the scene's copy arms it; it runs after the game's composite (ExecuteDraw). Graded here, before the
  // bloom chain, the game's automatic exposure (its 1x1 luminance) brightened it back (runs night_wake2, night_gc2).
  if (REXCVAR_GET(native_x_renderer_night) && c.scene_caller && slice == 0 &&
      target->guest_width == g_profile->output_width && target->guest_height == g_profile->output_height) {
    g_night_pending = true;
  }
  // D168: the scope's pass is armed the same way, while the game says it is on.
  if (c.scene_caller && slice == 0 && target->guest_width == g_profile->output_width &&
      target->guest_height == g_profile->output_height) {
    float r[3], u[3], b[3], t, zoom;
    g_scope_pending = ScopeView(r, u, b, t, zoom);
  }
  // The grass's water picture (grass.h): the finished scene, the water in it, against the grass's snapshot.
  if (c.scene_caller && slice == 0 && g_grass_ready && Grass::On() &&
      target->guest_width == g_profile->output_width && target->guest_height == g_profile->output_height) {
    g_grass.MarkWater(g_device->context(), target->texture, nullptr);
  }
  if (g_draw) g_draw->TargetsChanged();
  g_resolved.Touch(target, ResolveGuestBytes(c.fetch[0], c.fetch[5], format, width, height, slices, cube));
  // D140: memory a draw reads as vertices gets the resolve's texels too, as the guest's resolve writes them. The first
  // slice only (a stacked texture's others would need their slice's place in memory).
  if (!cube && slice == 0) {  // D155: kept for a first draw that reads it as vertices
    ResolvedTexture::GuestLayout& l = target->last_layout;
    l.address = address, l.width = width, l.height = height;
    l.pitch = ((c.fetch[0] >> 22) & 0x1FF) << 5;
    if (!l.pitch) l.pitch = width;
    l.guest_format = format, l.tiled = (c.fetch[0] >> 31) != 0, l.endian = (fetch1 >> 6) & 3;
    l.scale_x = target->scale.x, l.scale_y = target->scale.y, l.slice = slice, l.valid = true;
  }
  if (target->vertex_data && !cube && slice == 0 && g_resolved.memory()) {
    Blitter::MirrorWrite write;
    write.address = address;
    write.width = width, write.height = height;
    write.pitch = ((c.fetch[0] >> 22) & 0x1FF) << 5;
    if (!write.pitch) write.pitch = width;
    write.guest_format = format;
    write.tiled = (c.fetch[0] >> 31) != 0;
    write.endian = (fetch1 >> 6) & 3;
    write.scale_x = target->scale.x, write.scale_y = target->scale.y;
    write.slice = slice;
    GuestMemory* memory = g_resolved.memory();
    if (g_blit.WriteToMirror(g_device->context(), target->ArrayView(), memory->buffer(), memory->uav(), write)) {
      ++g_stats.mirror_writes;
      static const bool log = std::getenv("NXR_D140_LOG") != nullptr;  // a diagnosis aid: each one
      if (log) REXLOG_INFO("[native_x_renderer] D140: resolve at {:#x} written to the mirror", address);
    } else {
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXLOG_WARN("[native_x_renderer] D140: a resolve read as vertices, guest format {}, is not written to the guest "
                    "memory mirror (only 32-bit float formats are)", format);
      }
    }
  }
  ++g_stats.resolves;
}

// BeginTiling(dev, flags, count, tileRects*, clearColor*, Z (f1), stencil). The tile rectangles are
// D3DRECTs (x1, y1, x2, y2) in the coordinates of the whole image. The GPU replays the pass once per tile
// into surfaces of one tile's size, and the game resolves each tile's rectangle (POTCAWE, GRAW). NativeXRenderer
// draws the pass once: the surfaces bound in it get host targets of the size the tiles cover together, and
// each tile's resolve copies its own rectangle from there (D60).
void ExecuteBeginTiling(const Command& c) {
  RendererTime time;
  g_tiling = true;
  g_tiled_width = c.width;
  g_tiled_height = c.height;
  static uint32_t logged_width = 0, logged_height = 0;
  if (g_tiled_width != logged_width || g_tiled_height != logged_height) {
    logged_width = g_tiled_width;
    logged_height = g_tiled_height;
    REXLOG_INFO("[native_x_renderer] a tiled pass of {} tiles: drawn once, into targets of {}x{}", c.count,
                g_tiled_width, g_tiled_height);
  }
}

// EndTiling(dev, resolveFlags, resolveRects*, destTexture, clearColor*, Z (f1), stencil, params): a game can
// let it resolve the tiles. POTCAWE and GRAW resolve each tile themselves and pass no destination.
void ExecuteEndTiling(const Command& c) {
  RendererTime time;
  g_tiling = false;
  static bool logged = false;
  if (c.on && !logged) {
    logged = true;
    REXLOG_WARN("[native_x_renderer] EndTiling with a resolve destination: not supported yet");
  }
}

// Query Issue(query, flags): D3DISSUE_BEGIN (2) or D3DISSUE_END (1). An occlusion query's Issue points an
// EVENT_WRITE_ZPD at its sample counts (RB_SAMPLE_COUNT_ADDR: a normal packet with one tile, one
// bin-predicated packet per tile with several; the last is the last tile's block, which the end marker
// is in); other query types write none.
void ExecuteQuery(const Command& c) {
  RendererTime time;
  uint32_t flags = c.r[1];
  if (flags & 2) {
    // Samples are reported at the game's scale: from the scale of the targets the query's draws go to, the
    // reduced one in an upscaled view (D49).
    const RenderScale& scale = g_reduced && (g_scene_view || g_auto_reduced) ? g_reduced_scale : g_scale;
    g_queries.SetSampleScale(double(scale.x) * double(scale.y));
    g_queries.Begin(g_device->context());
  } else if (flags & 1) {
    g_queries.End(g_device->context(), c.address);
  }
}

// The game's aspect box (D43) and its views (D47), in the order of its draws.
void ExecuteAspectBox(const Command& c) { g_aspect_box = c.on && g_aspect_factor != 1.0; }

// With the views found from the draws (D119) the hooks' are not followed; the matrix is kept for the log's comparison
// with the camera vote (D118).
void ExecuteBeginView(const Command& c) {
  if (g_auto_views) {
    if (c.has_matrix) g_scene_view_projection = c.matrix;
    return;
  }
  g_scene_view = c.has_matrix;
  if (g_scene_view) g_scene_view_projection = c.matrix;
  g_scene_color = g_scene_depth = nullptr;
}

// The main 3D view's end: DLSS or FSR 3.1 on its color and depth, before the game's post-processing reads them.
void UpscaleScene() {
  if (!g_scene_view) return;
  g_scene_view = false;
  g_view_ended = true;
  RendererTime time(&g_phase.effects);
  // DLSS submits the command buffer (upscaler.cpp), which an open occlusion query forbids.
  bool upscaled = false;
  // With an upscaling mode, a view drawn at the render scale (the first after a frame without one, D121) is left as
  // drawn: the upscaler takes the reduced size.
  bool sized = !g_reduced || (g_scene_color && g_scene_color->reduced);
  if (!sized && g_camera_vote && g_draw) {
    Mat4d unused;
    uint32_t votes = 0, offers = 0;
    g_draw->TakeCameraVote(unused, votes, offers);  // its offers are not the next view's
  }
  if (g_temporal && sized && g_scene_color && g_scene_depth && g_scene_depth->srv &&
      !(g_queries_on && g_queries.open())) {
    // DLSS's result goes to the scene color at the render scale: the target itself, or for a reduced one the
    // surface's target at the render scale (D49).
    HostTarget* output = g_scene_color->reduced ? g_targets.Get(g_scene_color_surface) : g_scene_color;
    // The camera vote of this view's draws (D118).
    Mat4d vote;
    bool voted = false;
    if (g_camera_vote && g_draw) {
      uint32_t votes = 0, offers = 0;
      voted = g_draw->TakeCameraVote(vote, votes, offers);
      if (voted) g_blur_vote = vote, g_blur_voted = true;
      ++g_vote_stats.applications;
      if (voted) {
        ++g_vote_stats.voted;
        g_vote_stats.offers += offers;
        g_vote_stats.votes += votes;
      }
    }
    upscaled = output && g_upscaler.Apply(g_device->context(), g_scene_color->texture, g_scene_depth->texture,
                                          g_scene_depth->srv, output->texture, g_scene_view_projection,
                                          g_draw ? g_draw->jittered_z_min() : 0.0f,
                                          g_draw ? g_draw->jittered_z_max() : 1.0f, [](ITextureView* motion) {
                                            if (g_draw) g_draw->DrawObjectMotion(motion, g_scene_depth->view);
                                          }, voted ? &vote : nullptr);
    // Frame generation's inputs (D123), prepared at Present.
    if (upscaled && g_frame_generation && g_upscaler.frame_inputs().depth) {
      g_frame_inputs = g_upscaler.frame_inputs();
      g_frame_inputs_valid = true;
      // A view found from the draws has no view matrix: the camera from its first draw's (D124).
      if (g_auto_views && g_draw) {
        const Mat4d& m = g_draw->scene_start_matrix();
        std::array<float, 16> rows;
        for (int r = 0; r < 4; ++r) {
          for (int c = 0; c < 4; ++c) rows[r * 4 + c] = float(m[c * 4 + r]);
        }
        CameraFromViewProjection(rows, g_frame_inputs.camera_near, g_frame_inputs.camera_far, g_frame_inputs.fov_y);
        static bool logged = false;
        if (!logged) {
          logged = true;
          REXLOG_INFO("[native_x_renderer] frame generation: the camera from the view's first draw: near {:.3f}, far "
                      "{:.1f}, vertical field of view {:.1f} degrees (D124)",
                      g_frame_inputs.camera_near, g_frame_inputs.camera_far, g_frame_inputs.fov_y * 57.29578f);
        }
      }
    }
    if (upscaled && g_upscaler.last_vote_difference_px() >= 0.0) {
      ++g_vote_stats.compared;
      g_vote_stats.difference += g_upscaler.last_vote_difference_px();
      g_vote_stats.max_difference = std::max(g_vote_stats.max_difference, g_upscaler.last_vote_difference_px());
    }
    if (g_draw) g_draw->TargetsChanged();
  }
  // Motion blur (motion_blur.h): on the view's colour as the game's post-processing will read it (the upscaled image
  // with an upscaler), from the view's depth, the camera vote and the objects' own motion.
  if (const int taps = MotionBlurTaps(); taps > 0 && g_scene_color && g_scene_depth && g_scene_depth->srv && g_draw &&
                                         g_motion_blur_state >= 0 && !(g_queries_on && g_queries.open())) {
    if (g_motion_blur_state == 0) g_motion_blur_state = g_motion_blur.Load(g_device->device()) ? 1 : -1;
    // Without an upscaler or ambient occlusion nobody took the camera vote yet.
    if (!g_blur_voted && g_camera_vote && !g_temporal) {
      uint32_t votes = 0, offers = 0;
      g_blur_voted = g_draw->TakeCameraVote(g_blur_vote, votes, offers);
    }
    HostTarget* blurred = upscaled && g_scene_color->reduced ? g_targets.Get(g_scene_color_surface) : g_scene_color;
    const auto now = std::chrono::steady_clock::now();
    const float seconds = g_blur_last.time_since_epoch().count() ? std::chrono::duration<float>(now - g_blur_last).count() : 1.0f / 60.0f;
    g_blur_last = now;
    if (g_motion_blur_state == 1 && blurred && blurred->texture && seconds < 0.25f) {
      MotionBlur::Inputs in;
      in.color = blurred->texture;
      in.depth = g_scene_depth->texture;
      in.depth_srv = g_scene_depth->srv;
      in.width = blurred->scale.HostWidth(g_profile->output_width);
      in.height = blurred->scale.HostHeight(g_profile->output_height);
      in.depth_width = g_scene_depth->scale.HostWidth(g_profile->output_width);
      in.depth_height = g_scene_depth->scale.HostHeight(g_profile->output_height);
      in.reprojection = g_blur_voted ? &g_blur_vote : nullptr;
      in.z_min = g_draw->vote_z_min();
      in.z_max = g_draw->vote_z_max();
      {
        static auto at = std::chrono::steady_clock::now() - std::chrono::seconds(60);
        if (now - at > std::chrono::seconds(10)) {
          at = now;
          REXLOG_INFO("[native_x_renderer] motion blur: camera vote {}, the world's depth range {:.5f} to {:.5f} (the last draw's {:.5f} to {:.5f}), frame {:.1f} ms",
                      g_blur_voted ? "taken" : "none", in.z_min, in.z_max, g_draw->jittered_z_min(), g_draw->jittered_z_max(), seconds * 1000.0f);
        }
      }
      if (REXCVAR_GET(native_x_renderer_motion_blur_objects))
        in.object_motion = [](ITextureView* motion) {
          if (g_draw) g_draw->DrawObjectMotion(motion, g_scene_depth->view);
        };
      in.strength = float(std::clamp(REXCVAR_GET(native_x_renderer_motion_blur_strength), 0.0, 100.0) / 100.0);
      in.samples = taps;
      in.frame_seconds = seconds;
      if (g_motion_blur.Apply(g_device->context(), in)) g_draw->TargetsChanged();
    }
  }
  g_blur_voted = false;
  PromoteReduced(upscaled ? g_scene_color : nullptr);
  // D162: the view's depth for the night at the scene's copy, which follows in this frame.
  // D168: the picture in picture scope's too, to leave the first person weapon out of the view it remembers.
  if ((REXCVAR_GET(native_x_renderer_night) || g_scope_watch) && g_scene_depth && g_scene_depth->srv &&
      g_scene_depth->texture) {
    const TextureDesc& depth = g_scene_depth->texture->GetDesc();
    g_night_depth = g_scene_depth->srv;
    g_night_depth_uv[0] = float(g_scene_depth->scale.HostWidth(g_profile->output_width)) / float(depth.Width);
    g_night_depth_uv[1] = float(g_scene_depth->scale.HostHeight(g_profile->output_height)) / float(depth.Height);
    g_night_depth_frame = g_device->frames_presented();
  }
  g_scene_color = g_scene_depth = nullptr;
}

void ExecuteEndViewGeometry() {
  if (!g_auto_views) UpscaleScene();
}

// Composes the frame into `back_buffer` (the swap chain's, or a frame image of its size, D86).
bool EnsureFrameImage(FrameImage& image);

// The guest's picture `srv` (`width` x `height`, of guest texture format `format`, read with `swizzle`) into `target`
// (the window's size): through the guest's gamma ramp at the picture's size (D36), then scaled into the window,
// letterboxed to its aspect ratio; the area it covers into g_present_area. Black alone without a picture.
void ComposePicture(ITextureView* srv, uint32_t width, uint32_t height, uint32_t format, uint32_t swizzle,
                    ITextureView* target) {
  auto* context = g_device->context();
  Blitter::Options options;
  ITextureView* source = nullptr;
  bool gamma_applied = false;
  if (srv) {
    options.swizzle = swizzle;
    source = srv;
    if (ITextureView* gamma = g_gamma.Apply(context, g_blit, srv, width, height, format, options.swizzle)) {
      source = gamma;
      options.swizzle = Blitter::Options{}.swizzle;
      gamma_applied = true;
    }
  }
  const float black[4] = {0, 0, 0, 1};
  context->SetRenderTargets(1, &target, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->ClearRenderTarget(target, black, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  if (!srv) return;
  // Letterbox: keep the picture's aspect ratio in the window.
  const auto& desc = g_device->swap_chain()->GetDesc();
  float scale = std::min(float(desc.Width) / float(width), float(desc.Height) / float(height));
  uint32_t scaled_width = uint32_t(float(width) * scale), scaled_height = uint32_t(float(height) * scale);
  Blitter::Rect from{0, 0, width, height};
  Blitter::Rect to{int32_t((desc.Width - scaled_width) / 2), int32_t((desc.Height - scaled_height) / 2), scaled_width,
                   scaled_height};
  g_present_area[0] = to.x;
  g_present_area[1] = to.y;
  g_present_area[2] = int32_t(to.width);
  g_present_area[3] = int32_t(to.height);
  // FSR 1, NIS or CAS (D115) take the image after the gamma ramp, its swizzle applied; the stretch otherwise.
  if (gamma_applied && g_scaling.Apply(context, g_blit, source, width, height, target, to)) return;
  g_blit.Copy(context, source, Blitter::Source::kColorScaled, from, target, to, options);
}

void PresentFrontBuffer(const Command& c, ITextureView* back_buffer) {
  if (g_draw) g_draw->TargetsChanged();
  ResolvedTexture* front = nullptr;
  uint32_t fetch1 = 0;
  if (c.has_texture) {
    fetch1 = c.fetch[1];
    front = g_resolved.FindNewest(GpuAddress(fetch1 & 0xFFFFF000u));
    if (front) g_resolved.MarkUsed(front);
  }
  if (g_scope_research_present) {  // D168 research: the picture this present shows
    g_scope_research_present = false;
    REXLOG_INFO("[native_x_renderer] scope research: the present shows {:08X}{}", GpuAddress(fetch1 & 0xFFFFF000u),
                front ? fmt::format(", {}x{} format {}, its last resolve {} frames ago", front->width, front->height,
                                    front->guest_format, g_resolved.frame() - front->last_resolve_frame)
                      : ", nothing resolved there");
    if (front && front->texture) ScopeProbe("the front buffer", front->texture);
  }
  const auto& desc = g_device->swap_chain()->GetDesc();
  g_present_area[0] = g_present_area[1] = 0;
  g_present_area[2] = int32_t(desc.Width);
  g_present_area[3] = int32_t(desc.Height);
  // The front buffer is read through its fetch constant, swizzle included: a resolve with the red/blue swap
  // (RB_COPY_DEST_INFO) writes BGRA, and the front buffer's swizzle swaps it back.
  ComposePicture(front ? front->srv : nullptr, front ? front->width : 0, front ? front->height : 0, fetch1 & 0x3F,
                 (c.fetch[3] >> 1) & 0xFFF, back_buffer);
  // Frame generation (D123): the scene before the HUD, composed the same way.
  if (g_frame_generation && g_hudless_this_frame && front && g_hudless_copy && EnsureFrameImage(g_hudless_image)) {
    const TextureDesc& hudless = g_hudless_copy->GetDesc();
    ComposePicture(g_hudless_copy->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE), hudless.Width, hudless.Height,
                   fetch1 & 0x3F, (c.fetch[3] >> 1) & 0xFFF, g_hudless_image.rtv);
  } else {
    g_hudless_this_frame = false;
  }
}

// D150: a fixed GPU workload, timed on request (GpuBenchmarkStart), for the game's first start auto detect: a compute
// pass of 1920 x 1088 threads, each 256 dependent multiply adds and 32 texel reads, written to an image, four times
// between a duration query, six more before it untimed; sixteen times, a frame apart, the shortest kept (the GPU idles
// in the launcher, and its clock rises with the work: the first run was the slowest). A slower GPU takes longer in
// proportion.
class GpuBenchmark {
 public:
  void Request() { requested_.store(true); }
  bool Result(double* ms) const {
    if (!done_.load()) return false;
    *ms = result_ms_;
    return true;
  }
  void Run(IRenderDevice* device, IDeviceContext* context) {
    if (!requested_.load() || done_.load()) return;
    if (!started_) {
      started_ = true;
      began_ = std::chrono::steady_clock::now();
      if (!Start(device, context)) Finish(-1.0);
      return;
    }
    QueryDataDuration data;
    if (query_->GetData(&data, sizeof(data), true) && data.Frequency) {
      const double ms = double(data.Duration) * 1000.0 / double(data.Frequency);
      best_ms_ = best_ms_ < 0 ? ms : std::min(best_ms_, ms);
      // The next run's work about 40 ms of this GPU's (four dispatches timed, the rest before them untimed): enough to
      // keep any GPU busy between frames, so its clock rises to its full (partly busy, an RTX 4070 Ti stayed at 1.3 to
      // 1.6 GHz of its 2.8, run ad3, and a slower GPU, busy throughout, would have looked faster than it is); never a
      // long wait in one frame on a slow one.
      burst_ = std::clamp(int(40.0 / std::max(ms / 4.0, 0.05)), 4, 64);
      // Runs for 3 s at least, sixteen at least: a GPU idling at the game's start takes a few seconds to reach its full
      // clock even fully busy (an RTX 4070 Ti, run ad4: 585 MHz, then 1,545, 1,800 and 2,100 MHz over 1.2 s, 2,805 in
      // play), and the shortest run is the one at its full clock.
      const double ran = std::chrono::duration<double>(std::chrono::steady_clock::now() - began_).count();
      if ((++runs_ >= 16 && ran >= 3.0) || runs_ >= 900) return Finish(best_ms_);
      Dispatch(context);
      frames_ = 0;
    } else if (++frames_ > 300) {
      Finish(best_ms_);
    }
  }

 private:
  bool Start(IRenderDevice* device, IDeviceContext* context) {
    if (device->GetDeviceInfo().Features.DurationQueries != DEVICE_FEATURE_STATE_ENABLED) return false;
    static const char* kSource = R"GLSL(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D g_src;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D g_dst;
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  vec4 v = vec4(p.xyxy) * 0.001 + vec4(0.1, 0.2, 0.3, 0.4);
  vec4 acc = vec4(0.0);
  for (int i = 0; i < 256; ++i) {
    v = fract(fma(v, vec4(1.0001, 0.9997, 1.0003, 0.9999), vec4(0.37, 0.11, 0.53, 0.29)));
    if ((i & 7) == 0) acc += texelFetch(g_src, (p * 7 + ivec2(i * 13, i * 29)) & 511, 0);
  }
  imageStore(g_dst, p, v * 0.5 + acc * 0.002);
}
)GLSL";
    std::vector<uint32_t> spirv = CompileGlsl(GlslStage::kCompute, kSource, "NativeXRenderer GPU benchmark");
    if (spirv.empty()) return false;
    ShaderCreateInfo si;
    si.Desc.Name = "NativeXRenderer GPU benchmark";
    si.Desc.ShaderType = SHADER_TYPE_COMPUTE;
    si.Desc.UseCombinedTextureSamplers = true;
    si.ByteCode = spirv.data();
    si.ByteCodeSize = spirv.size() * sizeof(uint32_t);
    RefCntAutoPtr<IShader> cs;
    device->CreateShader(si, &cs);
    if (!cs) return false;
    ComputePipelineStateCreateInfo pi;
    pi.PSODesc.Name = "NativeXRenderer GPU benchmark";
    pi.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
    pi.pCS = cs;
    pi.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    SamplerDesc point;
    point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_COMPUTE, "g_src", point}};
    pi.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    pi.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
    device->CreateComputePipelineState(pi, &pso_);
    if (!pso_) return false;
    std::vector<uint8_t> noise(512 * 512 * 4);
    uint32_t seed = 12345;
    for (uint8_t& b : noise) seed = seed * 1664525u + 1013904223u, b = uint8_t(seed >> 24);
    TextureDesc src;
    src.Name = "NativeXRenderer GPU benchmark source";
    src.Type = RESOURCE_DIM_TEX_2D;
    src.Width = src.Height = 512;
    src.Format = TEX_FORMAT_RGBA8_UNORM;
    src.Usage = USAGE_IMMUTABLE;
    src.BindFlags = BIND_SHADER_RESOURCE;
    TextureSubResData sub{noise.data(), 512 * 4};
    TextureData init{&sub, 1};
    device->CreateTexture(src, &init, &src_);
    TextureDesc dst;
    dst.Name = "NativeXRenderer GPU benchmark output";
    dst.Type = RESOURCE_DIM_TEX_2D;
    dst.Width = 1920, dst.Height = 1088;
    dst.Format = TEX_FORMAT_RGBA8_UNORM;
    dst.Usage = USAGE_DEFAULT;
    dst.BindFlags = BIND_UNORDERED_ACCESS;
    device->CreateTexture(dst, nullptr, &dst_);
    QueryDesc qd;
    qd.Name = "NativeXRenderer GPU benchmark";
    qd.Type = QUERY_TYPE_DURATION;
    device->CreateQuery(qd, &query_);
    if (!src_ || !dst_ || !query_) return false;
    pso_->CreateShaderResourceBinding(&srb_, true);
    if (!srb_) return false;
    if (auto* v = srb_->GetVariableByName(SHADER_TYPE_COMPUTE, "g_src")) v->Set(src_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    if (auto* v = srb_->GetVariableByName(SHADER_TYPE_COMPUTE, "g_dst")) v->Set(dst_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
    Dispatch(context);
    return true;
  }
  void Dispatch(IDeviceContext* context) {
    context->SetPipelineState(pso_);
    context->CommitShaderResources(srb_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    DispatchComputeAttribs groups(1920 / 8, 1088 / 8, 1);
    // Outside the timing first, the rest of the run's burst (the caches, and the clock rising: with one, on a GPU idling
    // in the game's start, a run read 5.85 ms on one start and 11.45 on the next, run smoke123).
    for (int i = 4; i < burst_; ++i) context->DispatchCompute(groups);
    context->BeginQuery(query_);
    for (int i = 0; i < 4; ++i) context->DispatchCompute(groups);
    context->EndQuery(query_);
  }
  void Finish(double ms) {
    result_ms_ = ms;
    done_.store(true);
    REXLOG_INFO("[native_x_renderer] GPU benchmark (D150): {:.3f} ms (an RTX 4070 Ti takes about {:.3f})", ms,
                kReferenceMs);
    pso_.Release(), srb_.Release(), src_.Release(), dst_.Release(), query_.Release();
  }

 public:
  static constexpr double kReferenceMs = 5.58;  // runs ad5, ad6: 5.576 and 5.575 ms at its full clock (D153)

 private:
  std::atomic<bool> requested_{false}, done_{false};
  bool started_ = false;
  std::chrono::steady_clock::time_point began_{};
  int frames_ = 0, runs_ = 0;
  double result_ms_ = -1.0, best_ms_ = -1.0;
  int burst_ = 10;  // dispatches a run: four timed, the rest before them
  RefCntAutoPtr<IPipelineState> pso_;
  RefCntAutoPtr<IShaderResourceBinding> srb_;
  RefCntAutoPtr<ITexture> src_, dst_;
  RefCntAutoPtr<IQuery> query_;
} g_gpu_benchmark;

// Shows the composed frame: the swap chain's Present, the frame fence, the pacing. Present ends DiligentCore's
// command buffer: the draw path binds its state again (D86: this may come between the next frame's draws).
void FinishPresent() {
  g_gpu_benchmark.Run(g_device->device(), g_device->context());
  g_timer.End(g_device->context());
  const auto swap_start = Pacing::Clock::now();
  double submit_ms = 0;
  double wait_ms = g_device->Present(&submit_ms);
  g_pacing.swap_ms += Pacing::Ms(Pacing::Clock::now() - swap_start) - wait_ms;
  g_pacing.submit_ms += submit_ms;
  g_timer.Begin(g_device->context());
  if (g_draw) g_draw->TargetsChanged();
  g_pacing.wait_ms += wait_ms;
  g_pacing.renderer_ms -= wait_ms;
  g_pacing.last_wait_ms = wait_ms;
  g_pacing.max_wait_ms = std::max(g_pacing.max_wait_ms, wait_ms);
  auto now = Pacing::Clock::now();
  if (g_pacing.last_present != Pacing::Clock::time_point{}) {
    double interval = Pacing::Ms(now - g_pacing.last_present);
    g_pacing.last_interval_ms = interval;
    g_pacing.interval_ms += interval;
    g_pacing.max_interval_ms = std::max(g_pacing.max_interval_ms, interval);
    ++g_pacing.frames;
  }
  g_pacing.last_present = now;
}

// A frame image at the swap chain's size and format (created again when it changes).
bool EnsureFrameImage(FrameImage& image) {
  const auto& desc = g_device->swap_chain()->GetDesc();
  if (!image.texture || image.texture->GetDesc().Width != desc.Width ||
      image.texture->GetDesc().Height != desc.Height) {
    TextureDesc texture;
    texture.Name = "NativeXRenderer frame image";
    texture.Type = RESOURCE_DIM_TEX_2D;
    texture.Width = desc.Width;
    texture.Height = desc.Height;
    texture.Format = desc.ColorBufferFormat;
    texture.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
    image = {};
    g_device->device()->CreateTexture(texture, nullptr, &image.texture);
    if (!image.texture) return false;
    image.rtv = image.texture->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    image.srv = image.texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  }
  return true;
}

// The frame image to compose into (D86).
FrameImage* NextFrameImage() {
  FrameImage& image = g_frame_images[g_next_frame_image];
  g_next_frame_image = (g_next_frame_image + 1) % g_frame_images.size();
  return EnsureFrameImage(image) ? &image : nullptr;
}

// Frame generation (D123): the frame composed into g_frame_generation_image, a frame generated between the last
// one and it, both copied into the swap chain's back buffer and presented, the generated one first: with V-Sync,
// which frame generation turns on, the swap chain shows them a refresh apart. False when there is no image to
// compose into.
bool PresentWithGeneratedFrame(const Command& c) {
  if (!EnsureFrameImage(g_frame_generation_image)) return false;
  auto* context = g_device->context();
  const auto& swap_desc = g_device->swap_chain()->GetDesc();
  if (g_frame_inputs_valid) {
    // The 2D layer's motion over the view's (D124), then FidelityFX's prepare pass on them.
    if (g_profile->overlay_motion) {
      g_overlay_motion.Draw(g_device->device(), context, g_frame_inputs.motion, g_frame_inputs.depth,
                            g_profile->output_width, g_profile->output_height, g_profile->overlay_motion_adjust);
    }
    FrameGeneration::View view;
    view.depth = g_frame_inputs.depth;
    view.motion = g_frame_inputs.motion;
    view.jitter_x = g_frame_inputs.jitter_x;
    view.jitter_y = g_frame_inputs.jitter_y;
    view.camera_near = g_frame_inputs.camera_near;
    view.camera_far = g_frame_inputs.camera_far;
    view.fov_y = g_frame_inputs.fov_y;
    view.reset = g_frame_inputs.reset;
    g_framegen.Prepare(context, view, swap_desc.Width, swap_desc.Height);
  } else {
    g_overlay_motion.Clear();
  }
  g_frame_inputs_valid = false;
  PresentFrontBuffer(c, g_frame_generation_image.rtv);
  auto now = std::chrono::steady_clock::now();
  if (g_last_game_frame != std::chrono::steady_clock::time_point{}) {
    double ms = std::chrono::duration<double, std::milli>(now - g_last_game_frame).count();
    if (ms < 100.0) g_game_frame_ms += (ms - g_game_frame_ms) * 0.1;
  }
  g_last_game_frame = now;
  bool generated = g_framegen.Generate(context, g_frame_generation_image.texture,
                                       g_hudless_this_frame ? g_hudless_image.texture.RawPtr() : nullptr,
                                       g_present_area);
  g_hudless_this_frame = false;
  auto show = [&](ITextureView* srv) {
    const auto& desc = g_device->swap_chain()->GetDesc();
    ITextureView* back_buffer = g_device->swap_chain()->GetCurrentBackBufferRTV();
    Blitter::Rect all{0, 0, desc.Width, desc.Height};
    g_blit.Copy(context, srv, Blitter::Source::kColor, all, back_buffer, all);
    g_overlay.Present(context, back_buffer, desc.Width, desc.Height, g_window);
    FinishPresent();
  };
  if (generated) show(g_framegen.output_srv());
  show(g_frame_generation_image.srv);
  g_framegen.EndFrame();
  return true;
}

// Shows the waiting frame `index` (D86): its image copied into the back buffer, then FinishPresent. The frames
// before it are dropped.
void ShowPendingFrame(uint32_t index) {
  RendererTime time(&g_phase.present);
  FrameImage& image = g_frame_images[g_pending_frames[index].image];
  if (image.srv) {
    auto* context = g_device->context();
    auto* back_buffer = g_device->swap_chain()->GetCurrentBackBufferRTV();
    const auto& desc = g_device->swap_chain()->GetDesc();
    const auto& from_desc = image.texture->GetDesc();
    Blitter::Rect from{0, 0, from_desc.Width, from_desc.Height}, to{0, 0, desc.Width, desc.Height};
    g_blit.Copy(context, image.srv,
                from_desc.Width == desc.Width && from_desc.Height == desc.Height ? Blitter::Source::kColor
                                                                                 : Blitter::Source::kColorScaled,
                from, back_buffer, to);
  }
  FinishPresent();
  for (uint32_t i = index + 1; i < g_pending_count; ++i) g_pending_frames[i - index - 1] = g_pending_frames[i];
  g_pending_count -= index + 1;
  g_pending_swap.store(g_pending_count ? g_pending_frames[0].tag : 0, std::memory_order_relaxed);
}

// The waiting frames (D86): the newest whose swap has executed is shown (older ones are skipped); with `room`,
// the oldest is shown anyway if the queue is full; one that waited kSwapTimeout is shown. On the renderer's
// thread: before every command, at Present, and when the queue wakes it (the swap observer, or every few
// milliseconds while it waits).
void PresentPending(bool room = false) {
  if (!g_pending_count) return;
  uint64_t executed = g_swaps_executed.load(std::memory_order_acquire);
  int ready = -1;
  for (uint32_t i = 0; i < g_pending_count; ++i) {
    if (executed >= g_pending_frames[i].tag) ready = int(i);
  }
  if (ready >= 0) {
    ++g_pacing.at_swap;
    g_pacing.skipped += uint32_t(ready);
    ShowPendingFrame(uint32_t(ready));
  } else if (room && g_pending_count == kMaxPendingFrames) {
    ++g_pacing.before_swap;
    ShowPendingFrame(0);
  } else if (std::chrono::steady_clock::now() - g_pending_frames[0].since > kSwapTimeout) {
    ++g_pacing.timed_out;
    ShowPendingFrame(0);
  }
}

void ExecutePresent(const Command& c) {
  bool with_swap = g_present_with_swaps && c.swap_tag;
  if (with_swap) PresentPending(true);  // room for this frame
  RendererTime time(&g_phase.present);
  g_aspect_box = false;
  g_hud_phase = 0;
  LogHudStats();
  if (g_draw) g_draw->EndHudFrame();
  g_scene_copied_last_frame = g_scene_copied_this_frame;
  g_scene_copied_this_frame = false;
  PromoteReduced(nullptr);  // a view without EndViewGeometry (D49)
  g_scene_view = false;
  g_auto_upscaled_last_frame = g_auto_view_done;
  g_auto_reduced = g_reduced && g_auto_view_done;
  g_view_ended = false;
  g_auto_view_done = false;
  g_gtao_done = false;
  g_terrain.Reset();
  g_scene_antialiased = false;
  g_night_pending = false;  // a frame whose composite never came
  g_scope_pending = false;
  g_scope_research_present = g_scope_research_frame;  // PresentFrontBuffer logs what it shows
  g_scope_draw_probes = 0;
  if (g_scope_research_frame && g_scope_probe_texture) ScopeProbe("at the present", g_scope_probe_texture);
  g_scope_probe_texture = nullptr;
  // A research frame's present: the draws after it until the next composite are logged too.
  // D168: the next frame's draws watched for the game's scope mask pass while the game reports its view (the picture in
  // picture on, in a match), whatever the zoom: the pass is drawn only at a sniper scope's (Apply's test). Armed at the
  // composite only, the pass was missed in most scoped frames (run v41scope12: drawn in 9 of about 470; there the
  // composite came before the mask pass only while the scope was being raised).
  {
    float right[3], up[3], back[3], tan_half_v, zoom;
    const bool watch = ScopeView(right, up, back, tan_half_v, zoom);
    g_scope_watch = watch;
    if (g_draw) {
      g_draw->SetScopeFrame(watch, g_scope_research_frame ? 3 : 0);
      g_draw->SetScopeClip(0, 0, 0, 0);
    }
  }
  if (g_scope_research_frame)
    REXLOG_INFO("[native_x_renderer] scope research: present (the scope's pass {})", g_scope_mask_seen ? "drawn" : "not drawn");
  g_scope_research_frame = false;
  g_scope_mask_seen = false;
  g_scope.EndFrame();
  // D163: this frame's model draws are used; the models the game gave, taken for the next frames.
  g_night_model_draws.clear();
  {
    std::lock_guard lock(g_night_lamps_mutex);
    if (g_night_models_render_version != g_night_models_version) {
      g_night_models_render_version = g_night_models_version;
      g_night_models_render = g_night_models;
      g_night_model_counts.clear();
      for (const NightModelLamps& m : g_night_models_render) {
        g_night_model_counts.push_back(m.index_count);
        g_night_model_counts.push_back(m.matrix_register);
      }
    }
  }
  if (g_temporal) g_upscaler.NextFrame();
  if (!g_device->UpdateSize()) {
    // Minimized: the frame drawn but not shown is still ended (D125). Returning without it left every frame's work
    // unsubmitted and its uploads held, and after Alt+Tab from exclusive fullscreen DiligentCore's dynamic heap ran
    // out within seconds in a song (16 MB host-visible pages created every 30-80 ms, ~1 GB; then a crash in memcpy).
    g_timer.End(g_device->context());
    g_device->FinishHiddenFrame();
    g_timer.Begin(g_device->context());
    if (g_draw) g_draw->TargetsChanged();
    return;
  }
  const auto& desc = g_device->swap_chain()->GetDesc();
  // With the guest's swaps (D86): composed into a frame image now, shown when the command processor executes the
  // frame's swap, which comes after this command has run (the swap passes the signal gate).
  // Frame generation (D123); not with the guest's swaps (D86) for now.
  if (g_frame_generation && !with_swap && PresentWithGeneratedFrame(c)) return;
  FrameImage* image = with_swap ? NextFrameImage() : nullptr;
  ITextureView* target = image ? image->rtv : g_device->swap_chain()->GetCurrentBackBufferRTV();
  PresentFrontBuffer(c, target);
  g_overlay.Present(g_device->context(), target, desc.Width, desc.Height, g_window);
  if (!image) {
    FinishPresent();
    return;
  }
  g_pending_frames[g_pending_count++] = {c.swap_tag, uint32_t(image - g_frame_images.data()),
                                         std::chrono::steady_clock::now()};
  g_pending_swap.store(g_pending_frames[0].tag, std::memory_order_relaxed);
  PresentPending();
}

// Runs a command on the renderer's thread (or right away without it).
void Execute(Command& c);

// ---------------------------------------------------------------------------------------------------
// Capturing, on the game's render thread (D71): what each call needs from guest memory and the tracker, read
// before the call returns to the game.

// One command: filled, then published when it goes out of scope.
class Capture {
 public:
  explicit Capture(Command::Type type) : lock_(g_capture_mutex), command(g_queue.Next()) {
    command.type = type;
    ++g_captured;
  }
  ~Capture() { g_queue.Publish(); }
  Capture(const Capture&) = delete;
  Capture& operator=(const Capture&) = delete;

 private:
  std::lock_guard<std::mutex> lock_;

 public:
  Command& command;
};


void CaptureClear(PPCContext& ctx, const uint8_t* base) {
  uint32_t device = ctx.r3.u32, flags = ctx.r4.u32, rect = ctx.r5.u32, color = ctx.r6.u32;
  Capture capture(Command::Type::kClear);
  Command& c = capture.command;
  c.r = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  c.f1 = ctx.f1.f64;
  c.has_rect = rect != 0;
  if (c.has_rect) {
    for (uint32_t i = 0; i < 4; ++i) c.rect[i] = int32_t(Load32(base, rect + 4 * i));
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (flags & (1u << i)) c.surfaces[i] = ReadBoundSurface(base, device, i);
  }
  if (flags & 0x30) c.surfaces[4] = ReadBoundSurface(base, device, 4);
  if (color) {
    c.has_color = true;
    for (uint32_t i = 0; i < 4; ++i) c.color[i] = GuestFloat(base, color + 4 * i);
  }
}

// An inline draw's vertices and indices are in the command buffer, which the library reuses on its own
// schedule: they are copied into the command. Its stream is vertex fetch constant 95 (fetch slot 31, rule 3 of
// xdk-d3d-reference.md); DMA indices are at VGT_DMA_BASE. The draw's own packets write both, and the draw
// initiator (rule 11): they are read from its scan, the register file being on the renderer's thread (D73).
void CaptureInlineData(const StateTracker& tracker, const uint8_t* base, Command& c) {
  namespace rg = rex::graphics;
  auto copy = [&](uint32_t address, uint32_t size) {
    constexpr uint32_t kMaxSize = 1u << 20;
    if (!size || size > kMaxSize || c.copy_count >= c.copies.size()) return;
    const uint8_t* source = base + (0xA0000000u | (address & 0x1FFFFFFFu));
    c.copies[c.copy_count++] = {address, uint32_t(c.data.size()), size};
    c.data.insert(c.data.end(), source, source + size);
  };
  uint32_t fetch0, fetch1;
  if (tracker.LastWrite(rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 31 * 6 + 4, fetch0) &&
      tracker.LastWrite(rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 31 * 6 + 5, fetch1) &&
      (fetch0 & 3) == uint32_t(rg::xenos::FetchConstantType::kVertex)) {
    copy(fetch0 & ~3u, ((fetch1 >> 2) & 0xFFFFFF) * 4);
  }
  uint32_t initiator, dma_base;
  if (tracker.LastWrite(rg::XE_GPU_REG_VGT_DRAW_INITIATOR, initiator) && ((initiator >> 6) & 3) == 0 &&
      tracker.LastWrite(rg::XE_GPU_REG_VGT_DMA_BASE, dma_base)) {  // DMA indices: 16 or 32 bits each
    copy(dma_base, (initiator >> 16) * ((initiator >> 11) & 1 ? 4 : 2));
  }
}

void CaptureDraw(const TrackedCall& tracked, StateTracker& tracker, const uint8_t* base) {
  Capture capture(Command::Type::kDraw);
  Command& c = capture.command;
  c.call = tracked.call;
  tracker.TakeRecord(c.registers);
  // A draw replayed from a recorded command buffer renders into the surfaces bound on the device that runs the
  // buffer, as on the console, where the buffer holds draws and their state but not the render targets (D131). Only
  // when that device has none bound, the ones the recording device had (D129's snapshot).
  bool bound = false;
  if (tracked.has_surfaces && !REXCVAR_GET(native_x_renderer_replay_recorded_targets)) {
    for (uint32_t i = 0; i < 5; ++i) {
      c.surfaces[i] = ReadBoundSurface(base, tracked.device, i);
      bound |= c.surfaces[i].object != 0;
    }
  }
  if (tracked.has_surfaces && !bound) {
    // The surfaces the recording device had bound for it.
    for (uint32_t i = 0; i < 5; ++i) {
      const SurfaceSnapshot& r = tracked.surfaces[i];
      GuestSurface& s = c.surfaces[i];
      s = {};
      if (!r.object) continue;
      s.object = r.object;
      s.surface_info = r.surface_info;
      s.info = r.info;
      s.width = r.width;
      s.height = r.height;
      s.depth = i == 4;
    }
  } else if (!tracked.has_surfaces) {
    for (uint32_t i = 0; i < 5; ++i) c.surfaces[i] = ReadBoundSurface(base, tracked.device, i);
  }
  c.programs[0] = tracker.vertex_program_ptr();
  c.programs[1] = tracker.pixel_program_ptr();
  if ((Info(tracked.call).flags & kInlineEnd) || tracked.call == Call::kDrawVerticesUP) {
    CaptureInlineData(tracker, base, c);
  }
}

void CaptureResolve(const uint8_t* base, const Arguments& a, const PacketScan& scan) {
  uint32_t device = a.r[0], flags = a.r[1], source_rect = a.r[2], texture = a.r[3], dest_point = a.r[4];
  Capture capture(Command::Type::kResolve);
  Command& c = capture.command;
  c.r = a.r;
  c.f1 = a.f1;
  c.surfaces[0] = ReadBoundSurface(base, device, std::min(flags & 7u, 4u));
  if (texture) {
    // The destination texture's fetch constant (ObjectLayout::texture_fetch).
    c.has_texture = true;
    uint32_t fetch = texture + g_profile->objects.texture_fetch;
    for (uint32_t i = 0; i < 6; ++i) c.fetch[i] = Load32(base, fetch + 4 * i);
  }
  if (source_rect) {
    c.has_source_rect = true;
    for (uint32_t i = 0; i < 4; ++i) c.source_rect[i] = int32_t(Load32(base, source_rect + 4 * i));
  }
  if (dest_point) {
    c.has_dest_point = true;
    c.dest_point = {int32_t(Load32(base, dest_point)), int32_t(Load32(base, dest_point + 4))};
  }
  // The RB_COPY_DEST_INFO the call wrote: its last non-zero value (the library resets the copy registers
  // afterwards); the stencil a depth clear writes, from RB_DEPTH_CLEAR (D80).
  for (const auto& [reg, value] : scan.writes) {
    if (reg == kRbCopyDestInfo && value) c.dest_info = value;
    if (reg == kRbDepthClear) c.clear_stencil = value & 0xFF;
  }
  // The clears after the copy (D80): the color (a D3DVECTOR4 at r10) and the depth surface.
  if ((flags & 0x100) && a.r[7]) {
    c.has_color = true;
    for (uint32_t i = 0; i < 4; ++i) c.color[i] = GuestFloat(base, a.r[7] + 4 * i);
  }
  if (flags & 0x200) c.surfaces[4] = ReadBoundSurface(base, device, 4);
  c.scene_caller = a.lr == g_profile->scene_resolve_caller &&
                   (!g_profile->scene_resolve_outer_caller || a.outer_lr == g_profile->scene_resolve_outer_caller);
  c.caller = a.lr;
  // D165: a destination the game reads on the CPU gets defined guest bytes (zeros) now, on the game's thread, before
  // the game can read them. NXR_RESOLVE_LOG (a diagnosis aid): each caller's resolves by destination size and format,
  // once, and for the small ones how many found guest bytes that were not zero (what a CPU reader would have got).
  if (texture) {
    const uint32_t fetch1 = c.fetch[1], fetch2 = c.fetch[2];
    const uint32_t format = fetch1 & 0x3F, width = (fetch2 & 0x1FFF) + 1, height = ((fetch2 >> 13) & 0x1FFF) + 1;
    bool zero = false;
    for (const Profile::ZeroedResolve& z : g_profile->zeroed_resolves) {
      zero |= z.caller && z.caller == a.lr && (!z.outer_caller || z.outer_caller == a.outer_lr) &&
              (!z.width || z.width == width) && (!z.height || z.height == height) &&
              (z.format < 0 || uint32_t(z.format) == format);
    }
    static const bool log = std::getenv("NXR_RESOLVE_LOG") != nullptr;
    static const bool off = std::getenv("NXR_D165_OFF") != nullptr;  // a diagnosis aid: the game reads what is there
    if (off) zero = false;
    if (zero || log) {
      const uint32_t dimension = (c.fetch[5] >> 9) & 3;
      const bool cube = dimension == uint32_t(rex::graphics::xenos::DataDimension::kCube);
      const uint32_t slices = (fetch2 >> 26) + 1, address = GpuAddress(fetch1 & 0xFFFFF000u);
      const bool flat = dimension == uint32_t(rex::graphics::xenos::DataDimension::k2DOrStacked) || cube;
      const uint32_t bytes = flat ? ResolveGuestBytes(c.fetch[0], c.fetch[5], format, width, height, slices, cube) : 0;
      GuestMemory* memory = g_resolved.memory();
      uint8_t* data = memory && bytes && bytes <= (1u << 20) ? memory->memory().TranslatePhysical<uint8_t*>(address) : nullptr;
      size_t set = 0;
      if (data)
        for (uint32_t i = 0; i < bytes; ++i) set += data[i] != 0;
      if (log) {
        struct Seen {
          uint64_t calls = 0, dirty = 0;
        };
        static std::mutex mutex;
        static std::map<uint64_t, Seen> seen;
        std::lock_guard lock(mutex);
        Seen& s = seen[uint64_t(a.lr) << 32 | uint64_t(width & 0xFFF) << 20 | uint64_t(height & 0xFFF) << 8 | format];
        ++s.calls;
        s.dirty += set != 0;
        // Each kind once, then the small ones (what a game would read on the CPU) every 200th call and whenever one
        // is the first of its kind to hold something.
        if (s.calls == 1 || (data && width <= 256 && height <= 256 && (s.calls % 200 == 0 || (set && s.dirty == 1))))
          REXLOG_INFO("[native_x_renderer] resolve from {:#x} < {:#x} to {}x{} guest format {} at {:#x}: call {} of this "
                      "kind, {} of them found guest bytes not zero (this one {} of {}){}",
                      a.lr, a.outer_lr, width, height, format, address, s.calls, s.dirty, set, bytes,
                      zero ? ", zeroed (D165)" : "");
      }
      if (zero && data && set) {
        std::memset(data, 0, bytes);
        static std::atomic<uint32_t> zeroed{0};
        if (zeroed.fetch_add(1, std::memory_order_relaxed) == 0)
          REXLOG_INFO("[native_x_renderer] D165: a resolve destination the game reads on the CPU ({}x{} guest format {} "
                      "from {:#x}) held {} bytes of another use: set to zero before the game reads it",
                      width, height, format, a.lr, set);
      }
    }
  }
}

// BeginTiling(dev, flags, count, tileRects*, clearColor*, Z (f1), stencil): the size of the image the tiles
// cover together.
void CaptureBeginTiling(const uint8_t* base, const Arguments& a) {
  constexpr uint32_t kMaxTiles = 32;  // EDRAM holds 2048 of its own tiles; games use two or three
  constexpr int32_t kMaxSize = 8192;  // the largest guest surface
  uint32_t count = a.r[2], rects = a.r[3];
  Capture capture(Command::Type::kBeginTiling);
  Command& c = capture.command;
  c.count = count;
  for (uint32_t i = 0; rects && i < std::min(count, kMaxTiles); ++i) {
    int32_t x2 = int32_t(Load32(base, rects + 16 * i + 8)), y2 = int32_t(Load32(base, rects + 16 * i + 12));
    c.width = std::max(c.width, uint32_t(std::clamp(x2, 0, kMaxSize)));
    c.height = std::max(c.height, uint32_t(std::clamp(y2, 0, kMaxSize)));
  }
}

// Query Issue(query, flags): the address its EVENT_WRITE_ZPD points at (see ExecuteQuery).
void CaptureQuery(const Arguments& a, const PacketScan& scan) {
  constexpr uint32_t kRbSampleCountAddr = 0x2325;
  uint32_t address = 0;
  for (const auto* writes : {&scan.writes, &scan.predicated_writes}) {
    for (const auto& [reg, value] : *writes) {
      if (reg == kRbSampleCountAddr) address = value;
    }
  }
  if (!address) return;
  Capture capture(Command::Type::kQuery);
  capture.command.r = a.r;
  capture.command.address = address;
}

// Present: the front buffer's fetch constant. The tracker's record goes too, so that it stays short in
// frames without draws.
void CapturePresent(StateTracker& tracker, const uint8_t* base) {
  Capture capture(Command::Type::kPresent);
  Command& c = capture.command;
  // The swap that shows this frame (D86): the game's Present has called VdSwap by now; one that calls it later
  // gets the next number all the same.
  if (g_present_with_swaps) {
    uint64_t issued = 0, executed = 0;
    g_graphics->GetSwapCounts(issued, executed);
    c.swap_tag = std::max(issued, g_last_swap_tag + 1);
    g_last_swap_tag = c.swap_tag;
  }
  tracker.TakeRecord(c.registers);
  if (g_front_buffer) {
    c.has_texture = true;
    uint32_t fetch = g_front_buffer + g_profile->objects.texture_fetch;
    for (uint32_t i = 0; i < 6; ++i) c.fetch[i] = Load32(base, fetch + 4 * i);
  }
}

}  // namespace

#if !defined(_WIN32)
namespace {
// Linux: the game window's Wayland display and surface, or its X11 display and window, from its SDL window (the
// SDK's only window on Linux), for DiligentCore's swap chain; its size in pixels as the SDK tracks it.
rex::ui::Window* g_host_window = nullptr;
uint32_t HostPixelSize(bool height) {
  if (!g_host_window) return 0;
  return height ? g_host_window->GetActualPhysicalHeight() : g_host_window->GetActualPhysicalWidth();
}
// D159: the window's real size (its X11 surface's) when the SDK's is out of date: an SDL pixel size event of our
// own, which the SDK takes on its UI thread as it takes SDL's (WindowSDL::HandleWindowEvent), so its size, ImGui's
// and ours agree again. SDL_PushEvent is safe from the renderer's thread.
void HostRealPixelSize(uint32_t width, uint32_t height) {
  if (!g_host_window || !width || !height) return;
  SDL_Window* sdl = static_cast<rex::ui::WindowSDL*>(g_host_window)->sdl_window();
  if (!sdl) return;
  SDL_Event event{};
  event.type = SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
  event.window.windowID = SDL_GetWindowID(sdl);
  event.window.data1 = int32_t(width);
  event.window.data2 = int32_t(height);
  if (!SDL_PushEvent(&event)) REXLOG_WARN("[native_x_renderer] the window's real size not sent: {}", SDL_GetError());
}
void* LinuxHostWindow(rex::ui::Window* window) {
  static HostWindow host;
  SDL_Window* sdl = window ? static_cast<rex::ui::WindowSDL*>(window)->sdl_window() : nullptr;
  if (!sdl) return nullptr;
  const SDL_PropertiesID props = SDL_GetWindowProperties(sdl);
  host = HostWindow{};
  host.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
  host.wayland_surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
  if (!host.display || !host.wayland_surface) {
    host.wayland_surface = nullptr;
    host.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    host.x11_window = uint32_t(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    if (!host.display || !host.x11_window) return nullptr;
  }
  g_host_window = window;
  host.pixel_size = HostPixelSize;
  host.real_pixel_size = HostRealPixelSize;
  REXLOG_INFO("[native_x_renderer] the game window is on {}", host.wayland_surface ? "Wayland" : "X11");
  return &host;
}
}  // namespace
#endif

bool Initialize(const Profile& profile, rex::system::IGraphicsSystem* graphics, rex::ui::Window* window,
                const std::filesystem::path& cache_root) {
  g_profile = &profile;
  g_cache_root = cache_root;
  if (!REXCVAR_GET(native_x_renderer)) {
    return false;
  }
  // Before taking the window's swap chain: if the profile is incomplete, the Xenos path draws.
  if (!DeviceLayoutComplete(profile.device, "renderer")) {
    return false;
  }
  const ObjectLayout& o = profile.objects;
  if (o.texture_fetch == kUnknown || o.surface_info == kUnknown || o.surface_color_depth_info == kUnknown ||
      o.surface_size == kUnknown || !o.surface_width_minus_one.bits || !o.surface_height_minus_one.bits) {
    REXLOG_WARN("[native_x_renderer] renderer: the profile's object layout lacks the texture or surface fields; off");
    return false;
  }
#if defined(_WIN32)
  void* native_window = window ? window->GetNativeWindowHandle() : nullptr;
#else
  void* native_window = LinuxHostWindow(window);
#endif
  if (!graphics || !native_window) {
    REXLOG_ERROR("[native_x_renderer] renderer: needs the graphics system and the game's window; off");
    return false;
  }

  // The window can hold one swap chain: take it from the SDK's presenter. If NativeXRenderer cannot start, the
  // presenter gets it back and the Xenos path draws (D56: NativeXRenderer can be a game's default).
  window->SetPresenter(nullptr);
  auto device = std::make_unique<Device>();
  auto fall_back = [&](const char* reason) {
    g_blit = Blitter();
    device.reset();
    window->SetPresenter(graphics->presenter());
    REXLOG_ERROR("[native_x_renderer] renderer: {}; the Xenos path draws", reason);
    return false;
  };
  // FSR 3.1 picks 16-bit shaders where the GPU has them: the device enables them then (D116).
  bool fsr = REXCVAR_GET(native_x_renderer_antialiasing).rfind("fsr", 0) == 0;
  if (!device->Create(native_window, fsr) || !g_blit.Initialize(device->device())) {
    return fall_back("no host device");
  }
  if (!graphics->SetHostRendering(false)) {
    return fall_back("the graphics system cannot turn host rendering off (SDK patch missing?)");
  }
  // The SDK's backend draws nothing now. Its shader storage holds the Xenos path's pipelines, which the
  // runtime creates before the game starts: GRAW waited more than 14 s for 400 of them (D60).
  if (rex::cvar::Query<bool>("store_shaders") && rex::cvar::SetFlagByName("store_shaders", "false")) {
    REXLOG_INFO("[native_x_renderer] store_shaders set to false: the SDK's shader storage is for the Xenos path");
  }
  // The render scale (D37): the largest at which the game's image fits the output, which is the swap chain
  // (the monitor in fullscreen, the window's client area otherwise), at least 1, at most what the largest
  // texture allows; or the native_x_renderer_resolution_scale override.
  // Filling the output (D43): when the game widens its cameras, the scale is per axis, so that the image
  // has the output's aspect ratio; the vertical scale is the one computed or forced, and the horizontal one
  // follows from the output's aspect ratio. native_x_renderer_aspect (D104) asks for another ratio than the
  // output's: the image has that one, as large as fits the output, and the presentation centers it with bars.
  {
    const auto& output = device->swap_chain()->GetDesc();
    uint32_t guest_width = std::max(profile.output_width, 1u), guest_height = std::max(profile.output_height, 1u);
    double guest_aspect = double(guest_width) / guest_height;
    double output_aspect = output.Height ? double(output.Width) / output.Height : guest_aspect;
    double asked_aspect = REXCVAR_GET(native_x_renderer_aspect);
    double image_aspect = asked_aspect > 0.0 ? asked_aspect : output_aspect;
    bool fill = profile.widens_cameras && REXCVAR_GET(native_x_renderer_fill_screen) &&
                std::abs(image_aspect / guest_aspect - 1.0) > 0.01;
    double fit = fill ? std::min(double(output.Height) / guest_height,
                                 double(output.Width) / (guest_width * image_aspect / guest_aspect))
                      : std::min(double(output.Width) / guest_width, double(output.Height) / guest_height);
    double forced = REXCVAR_GET(native_x_renderer_resolution_scale);
    // D157: a spatial upscaler's mode, the scale from the screen divided by its ratio (never below 1, below).
    double ratio = REXCVAR_GET(native_x_renderer_upscale_ratio);
    // Under 1 too (13 Nov 2026, Battlefield 1943's Render resolution above 100%): the image larger than the output,
    // brought down to it when presenting (supersampling).
    bool divided = forced <= 0.0 && ratio > 0.0 && std::abs(ratio - 1.0) > 1e-3;
    double from_output = divided ? fit / ratio : fit;
    uint32_t max_dimension = device->device()->GetAdapterInfo().Texture.MaxTexture2DDimension;
    double factor = fill ? image_aspect / guest_aspect : 1.0;
    double max_scale =
        max_dimension ? double(max_dimension) / std::max(guest_width * std::max(factor, 1.0), double(guest_height))
                      : 1.0;
    double highest = std::max(max_scale, 1.0);
    double scale =
        std::clamp(forced > 0.0 ? forced : from_output, std::min(1.0 / std::min(factor, 1.0), highest), highest);
    // A taller output keeps the horizontal scale at least 1 (the vertical one grows instead).
    g_scale.y = float(scale);
    g_scale.x = float(scale * factor);
    g_aspect_factor = double(g_scale.x) / double(g_scale.y);
    REXLOG_INFO("[native_x_renderer] render scale {:.4f} x {:.4f} ({}{}): the game's {}x{} rendered at {}x{}, for a {}x{} "
                "output",
                g_scale.x, g_scale.y,
                forced > 0.0 ? std::string("native_x_renderer_resolution_scale")
                : divided    ? fmt::format("from the output divided by {:.2f}, native_x_renderer_upscale_ratio", ratio)
                             : std::string("from the output"),
                fill ? fmt::format(", filling it: the game's cameras widened by {:.4f}", g_aspect_factor) : "",
                guest_width, guest_height, g_scale.HostWidth(guest_width), g_scale.HostHeight(guest_height),
                output.Width, output.Height);
  }
  g_targets.Initialize(device->device(), g_scale);
  g_resolved.Initialize(device->device(), g_scale);
  // The shadow maps' scale (D92): at least native_x_renderer_shadow_scale on both axes, when that is above the render
  // scale; at most what the largest texture allows for a 2048x2048 map.
  {
    double asked = REXCVAR_GET(native_x_renderer_shadow_scale);
    uint32_t max_dimension = device->device()->GetAdapterInfo().Texture.MaxTexture2DDimension;
    double highest = max_dimension ? double(max_dimension) / 2048.0 : 1.0;
    double shadow = std::min(asked, highest);
    g_shadow_scaled = shadow > double(std::max(g_scale.x, g_scale.y));
    if (g_shadow_scaled) {
      g_shadow_scale.x = g_shadow_scale.y = float(shadow);
      g_targets.SetShadowScale(g_shadow_scale);
      g_resolved.SetShadowScale(g_shadow_scale);
      REXLOG_INFO("[native_x_renderer] shadow maps at scale {:.4f} (native_x_renderer_shadow_scale; the render scale is "
                  "{:.4f} x {:.4f}): a 1024x1024 map is {}x{} on the host",
                  shadow, g_scale.x, g_scale.y, g_shadow_scale.HostWidth(1024), g_shadow_scale.HostHeight(1024));
    }
  }
  // Occlusion queries measured here and answered to the command processor (D32); without them it
  // reports its fake sample count. The SDK's occlusion_query_enable (on by default) decides, as on the Xenos
  // path: a game that turns it off keeps the fake count (D67). Unregistered (no GPU plugin): on.
  bool measure_queries = rex::cvar::GetFlagByName("occlusion_query_enable").empty() ||
                         rex::cvar::Query<bool>("occlusion_query_enable");
  g_queries_on = measure_queries && g_queries.Initialize(device->device()) &&
                 graphics->SetOcclusionQueryResolver(&OcclusionQueries::Resolve, &g_queries);
  // Samples are counted at the render scale and reported at the game's (D32, D37).
  g_queries.SetSampleScale(double(g_scale.x) * double(g_scale.y));
  if (!measure_queries) {
    REXLOG_INFO("[native_x_renderer] occlusion queries not measured (occlusion_query_enable = false): the command "
                "processor's fake sample count, as on the Xenos path");
  } else if (!g_queries_on) {
    REXLOG_WARN("[native_x_renderer] occlusion queries not measured (SDK patch 0003 missing, or no device support)");
  }
  // The guest's display gamma ramp (D36); without it the front buffer is presented as it is.
  g_gamma.Initialize(device->device(), graphics);
  // The filter that scales the image into the window, and sharpens it (D115). D158: NIS that cannot run on this GPU
  // gives AMD FSR 1 (as King Kong's presenters without NIS do), and with native_x_renderer_check_nis the GPU is asked
  // whether NIS runs when another filter is chosen (NisSupport), so a game's menu offers NIS only where it does.
  {
    const Scaling::Filter filter = Scaling::ParseFilter(REXCVAR_GET(native_x_renderer_scaling));
    const float sharpness = float(REXCVAR_GET(native_x_renderer_sharpness));
    const bool ok = g_scaling.Initialize(device->device(), filter, sharpness);
    g_gpu_name = device->device()->GetAdapterInfo().Description;
    if (filter == Scaling::Filter::kNis) {
      if (!ok) {
        REXLOG_WARN("[native_x_renderer] NVIDIA Image Scaling cannot run on this GPU: AMD FSR 1 instead (D158)");
        g_scaling.Initialize(device->device(), Scaling::Filter::kFsr, sharpness);
      }
      g_nis_support = ok ? 1 : 0;
    } else if (REXCVAR_GET(native_x_renderer_check_nis)) {
      g_nis_support = Scaling::Supported(device->device(), Scaling::Filter::kNis) ? 1 : 0;
    }
  }
  // Which temporal upscalers this GPU runs, for the game's menus (TemporalSupport): FSR as AMD's library lists it for
  // this GPU (FSR 4 on a Radeon RX 9000 only), XeSS as Intel's library answers for it, DLSS on an NVIDIA GeForce RTX
  // (NGX runs on RTX cards only).
  {
    bool fsr4 = false, fsr3 = false;
    Fsr probe;
    probe.Generations(device->device(), &fsr4, &fsr3);
    const bool xess = Xess::Runs(device->device());
    const GraphicsAdapterInfo& adapter = device->device()->GetAdapterInfo();
    const std::string name = adapter.Description;
    const bool dlss = adapter.Vendor == ADAPTER_VENDOR_NVIDIA && name.find("RTX") != std::string::npos;
    g_temporal_support = (fsr4 ? 1 : 0) | (fsr3 ? 2 : 0) | (xess ? 4 : 0) | (dlss ? 8 : 0);
    REXLOG_INFO("[native_x_renderer] temporal upscalers on {}: FSR 4 {}, FSR 3.1 {}, XeSS {}, DLSS {}", name,
                fsr4 ? "yes" : "no", fsr3 ? "yes" : "no", xess ? "yes" : "no", dlss ? "yes" : "no");
  }
  // The SDK's dialogs (D75): without them a game that asks for text (GRAW's new game) waits on a black screen.
  g_window = window;
  if (!g_overlay.Initialize(device->device(), device->swap_chain()->GetDesc().ColorBufferFormat)) {
    REXLOG_WARN("[native_x_renderer] the SDK's dialogs cannot be drawn (no overlay pipeline)");
  }
  // Antialiasing (D41), where the profile names the finished scene's resolve.
  {
    std::string name = REXCVAR_GET(native_x_renderer_antialiasing);
    // DLSS or FSR 3.1 on the main 3D view, before the game's post-processing, in place of the antialiasing of the
    // finished scene: DLAA or FSR's native AA at the render resolution (D47, D116), or an upscaling mode from a lower
    // one (D49). SMAA where the provider is not available.
    using Provider = Upscaler::Provider;
    static constexpr struct {
      const char* name;
      Upscaler::Mode mode;
      Provider provider;
    } kTemporalModes[] = {{"dlaa", Upscaler::Mode::kDlaa, Provider::kDlss},
                          {"dlss_quality", Upscaler::Mode::kQuality, Provider::kDlss},
                          {"dlss_balanced", Upscaler::Mode::kBalanced, Provider::kDlss},
                          {"dlss_performance", Upscaler::Mode::kPerformance, Provider::kDlss},
                          {"dlss_ultra_performance", Upscaler::Mode::kUltraPerformance, Provider::kDlss},
                          // 13 Nov 2026: AMD FSR 4.1 (FSR SDK 2.4.0, the first with Vulkan); fsr_* stays FSR 3.1,
                          // from the same SDK.
                          {"fsr4_native", Upscaler::Mode::kDlaa, Provider::kFsr},
                          {"fsr4_quality", Upscaler::Mode::kQuality, Provider::kFsr},
                          {"fsr4_balanced", Upscaler::Mode::kBalanced, Provider::kFsr},
                          {"fsr4_performance", Upscaler::Mode::kPerformance, Provider::kFsr},
                          {"fsr4_ultra_performance", Upscaler::Mode::kUltraPerformance, Provider::kFsr},
                          {"fsr_native", Upscaler::Mode::kDlaa, Provider::kFsr},
                          {"fsr_quality", Upscaler::Mode::kQuality, Provider::kFsr},
                          {"fsr_balanced", Upscaler::Mode::kBalanced, Provider::kFsr},
                          {"fsr_performance", Upscaler::Mode::kPerformance, Provider::kFsr},
                          {"fsr_ultra_performance", Upscaler::Mode::kUltraPerformance, Provider::kFsr},
                          // 13 Nov 2026: Intel XeSS 3 (xess.h), on FSR's path.
                          {"taa", Upscaler::Mode::kDlaa, Provider::kTaa},  // our own (taa.h)
                          {"xess_native", Upscaler::Mode::kDlaa, Provider::kXess},
                          {"xess_quality", Upscaler::Mode::kQuality, Provider::kXess},
                          {"xess_balanced", Upscaler::Mode::kBalanced, Provider::kXess},
                          {"xess_performance", Upscaler::Mode::kPerformance, Provider::kXess},
                          {"xess_ultra_performance", Upscaler::Mode::kUltraPerformance, Provider::kXess}};
    for (const auto& entry : kTemporalModes) {
      if (name != entry.name) continue;
      uint32_t guest_width = std::max(profile.output_width, 1u), guest_height = std::max(profile.output_height, 1u);
      uint32_t width = g_scale.HostWidth(guest_width), height = g_scale.HostHeight(guest_height);
      g_upscaler.SetFrameGenerationInputs(REXCVAR_GET(native_x_renderer_frame_generation));
      Upscaler::fsr_generation = name.rfind("fsr4", 0) == 0 ? 4 : 3;
      Upscaler::sharpness = float(REXCVAR_GET(native_x_renderer_upscaler_sharpness));
      g_temporal = g_upscaler.Initialize(device->device(), width, height, entry.mode, entry.provider);
      const char* provider = entry.provider == Provider::kTaa    ? "TAA"
                             : entry.provider == Provider::kXess ? "XeSS"
                             : entry.provider != Provider::kFsr  ? "DLSS"
                             : Upscaler::fsr_generation >= 4     ? "FSR 4"
                                                                 : "FSR 3.1";
      name = g_temporal ? "off" : "smaa";
      g_object_motion = g_temporal && REXCVAR_GET(native_x_renderer_object_motion);
      g_camera_vote = g_temporal && REXCVAR_GET(native_x_renderer_camera_vote);
      // D119: the main 3D view from the game's hooks, or found from the draws up to the profile's upscale point (with
      // the camera vote; an upscaling mode's reduced targets from the frame's start, D121). Neither: SMAA, as DLSS
      // would never run.
      std::string views = REXCVAR_GET(native_x_renderer_views);
      bool hooks = views == "hooks" || (views != "auto" && profile.view_hooks);
      g_auto_views = g_temporal && !hooks && profile.upscale_resolve_caller;
      if (g_auto_views) {
        g_camera_vote = true;
      } else if (g_temporal && !hooks) {
        REXLOG_WARN("[native_x_renderer] {}: the game marks no views and its profile names no upscale point (D119): "
                    "SMAA instead", g_upscaler.Name());
        g_temporal = false;
      }
      g_object_motion = g_object_motion && g_temporal;
      g_camera_vote = g_camera_vote && g_temporal;
      name = g_temporal ? "off" : "smaa";
      if (g_temporal && g_upscaler.upscales()) {
        g_reduced = true;
        g_reduced_scale.x = float(g_upscaler.input_width()) / float(guest_width);
        g_reduced_scale.y = float(g_upscaler.input_height()) / float(guest_height);
        g_targets.SetReducedScale(g_reduced_scale);
      }
      std::string what = !g_temporal ? fmt::format("{} not available, SMAA instead", provider)
                         : g_reduced ? fmt::format("{} (experimental, D53): the 3D view drawn at {}x{} (scale "
                                                   "{:.3f}x{:.3f}), upscaled to {}x{} by {}",
                                                   g_upscaler.Name(), g_upscaler.input_width(),
                                                   g_upscaler.input_height(), g_reduced_scale.x, g_reduced_scale.y,
                                                   width, height, provider)
                         : entry.provider != Provider::kDlss
                             ? fmt::format("{0} at the render resolution", provider)
                             : std::string("DLAA (NVIDIA DLSS at the render resolution)");
      REXLOG_INFO("[native_x_renderer] antialiasing: {}{}{}", what,
                  g_temporal ? (g_object_motion ? ", with the motion of objects that move on their own"
                                            : ", with the camera's motion only")
                         : "",
                  g_auto_views    ? "; the main 3D view and the camera's motion found from the draws (D118, D119)"
                  : g_camera_vote ? "; the camera's motion from a vote of the main view's draws (D118)"
                                  : "");
    }
    // Frame generation (D123): the temporal upscaler's view and motion vectors are its inputs.
    if (REXCVAR_GET(native_x_renderer_frame_generation)) {
      if (!g_temporal) {
        REXLOG_WARN("[native_x_renderer] frame generation needs DLSS or FSR 3.1 on (their 3D view and motion vectors): "
                    "off");
      } else if (g_framegen.Load(device->device())) {
        g_frame_generation = true;
        device->ForceVsync(true);
        REXLOG_INFO("[native_x_renderer] frame generation: AMD FSR 3.1's, a frame between each two of the game's, with "
                    "V-Sync (D123)");
      }
    }
    Antialiasing::Mode mode = Antialiasing::ParseMode(name);
    if (mode != Antialiasing::Mode::kOff && !profile.scene_resolve_caller) {
      REXLOG_INFO("[native_x_renderer] antialiasing: off, the game's profile names no scene resolve");
      mode = Antialiasing::Mode::kOff;
    }
    g_aa.Initialize(device->device(), mode);
    g_aa_name = name;
    g_aa_live = !g_temporal && profile.scene_resolve_caller != 0;
  }
  // D132: ambient occlusion where the profile names its moment and the main view is found from the draws.
  // Started whatever native_ao_mode says, so that the menu can turn it on and off at once.
  if (profile.ao_resolve_caller && profile.upscale_resolve_caller && !profile.view_hooks) {
    g_gtao_on = g_gtao.Initialize(device->device());
    g_ssgi_on = g_ssgi.Initialize(device->device());
    g_grass_allowed = g_gtao_on || g_ssgi_on;
    // The view is found from the draws (D119) with the camera vote's registers, as with an upscaler.
    if (g_gtao_on || g_ssgi_on) g_auto_views = true, g_camera_vote = true;
    REXLOG_INFO("[native_x_renderer] ambient occlusion (GTAO, D132): {}", g_gtao_on ? "on" : "could not start");
    REXLOG_INFO("[native_x_renderer] global illumination (SSGI, D133): {}", g_ssgi_on ? "on" : "could not start");
  }
  g_timer.Initialize(device->device());
  g_timer.Begin(device->context());
  g_device = std::move(device);
  g_registers = std::make_unique<rex::graphics::RegisterFile>();
  // The renderer's thread (D71), only where the command processor can wait for it before it signals the game
  // (SetSignalGate(nullptr) answers whether it can, and sets nothing).
  bool threaded = REXCVAR_GET(native_x_renderer_thread);
  if (threaded && !graphics->SetSignalGate(nullptr, nullptr)) {
    REXLOG_WARN("[native_x_renderer] the command processor has no signal gate (SDK patch 0006 missing): the renderer "
                "runs on the game's render thread");
    threaded = false;
  }
  // Presenting with the guest's swaps (D86): the renderer's thread, and the command processor's swap counts
  // (SDK patch 0008).
  g_graphics = graphics;
  if (REXCVAR_GET(native_x_renderer_present_with_swaps)) {
    uint64_t issued = 0, executed = 0;
    if (!threaded) {
      REXLOG_WARN("[native_x_renderer] native_x_renderer_present_with_swaps needs the renderer's own thread: frames "
                  "are shown when the game calls Present");
    } else if (!graphics->GetSwapCounts(issued, executed)) {
      REXLOG_WARN("[native_x_renderer] the command processor does not count the guest's swaps (SDK patch 0008 "
                  "missing): frames are shown when the game calls Present");
    } else {
      g_present_with_swaps = true;
      g_last_swap_tag = issued;
      g_swaps_executed.store(executed, std::memory_order_relaxed);
    }
  }
  CommandQueue::Poller poller = nullptr;
  if (g_present_with_swaps) poller = [] { PresentPending(); };
  const bool raise = REXCVAR_GET(native_x_renderer_raise_priority);
  g_queue.SetRaisePriority(raise);
  g_queue.Start(threaded, &Execute, poller);
  if (threaded) {
    // The gate is called on the command processor's thread: its priority raised there, the first time (D136).
    static bool raise_gate = false;
    raise_gate = raise;
    graphics->SetSignalGate(
        [](void*) {
          static thread_local bool raised = false;
          if (raise_gate && !raised) raised = true, RaiseCurrentThreadPriority();
          g_queue.WaitIdle();
        },
        nullptr);
    std::atexit([] { g_queue.Stop(); });
  }
  if (g_present_with_swaps) {
    graphics->SetSwapObserver(
        [](void*, uint64_t executed) {
          g_swaps_executed.store(executed, std::memory_order_release);
          uint64_t pending = g_pending_swap.load(std::memory_order_relaxed);
          if (pending && executed >= pending) g_queue.Wake();
        },
        nullptr);
  }
  REXLOG_INFO("[native_x_renderer] renderer on: the SDK runs the command buffer for synchronization only; the "
              "renderer runs {}; frames are shown {}",
              threaded ? "on a thread of its own (D71)" : "on the game's render thread",
              g_present_with_swaps ? "when the command processor executes their swaps (D86)"
                                   : "when the game calls Present");
  return true;
}

void OnEnter(Call call, PPCContext& ctx, uint8_t* base, bool nested) {
  if (call == Call::kClearF) {
    // Captured right away (ClearF is also called from inside Clear): the bound targets are the ones to clear.
    CaptureClear(ctx, base);
    return;
  }
  if (nested) return;
  // The arguments of the calls the renderer acts on; the setters' are not needed (D72).
  switch (call) {
    case Call::kResolve:
    case Call::kPresent:
    case Call::kBeginTiling:
    case Call::kEndTiling:
    case Call::kQueryIssue:
      break;
    default:
      // A recorded command buffer's run (kReplay) hands on the draws it replays (StateTracker::LeaveReplay).
      if (!(Info(call).flags & (kDraw | kReplay))) return;
      break;
  }
  g_call.call = call;
  g_call.r = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  g_call.f1 = ctx.f1.f64;
  g_call.lr = uint32_t(ctx.lr);
  static const bool resolve_log = std::getenv("NXR_RESOLVE_LOG") != nullptr;
  bool outer = g_profile->scene_resolve_outer_caller != 0 || resolve_log;
  for (const Profile::ZeroedResolve& z : g_profile->zeroed_resolves) outer |= z.caller && z.outer_caller;  // D165
  g_call.outer_lr = call == Call::kResolve && outer ? OuterReturnAddress(base, ctx.r1.u32) : 0;
  if (call == Call::kPresent) g_front_buffer = ctx.r4.u32;
}

void OnLeave(const TrackedCall& tracked, StateTracker& tracker, PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  if (tracked.call != g_call.call) return;
  if (tracked.draw) {
    CaptureDraw(tracked, tracker, base);
    return;
  }
  switch (tracked.call) {
    case Call::kBeginTiling:
      CaptureBeginTiling(base, g_call);
      break;
    case Call::kEndTiling: {
      Capture capture(Command::Type::kEndTiling);
      capture.command.on = g_call.r[3] != 0;
      break;
    }
    case Call::kResolve:
      CaptureResolve(base, g_call, *tracked.scan);
      break;
    case Call::kQueryIssue:
      if (g_queries_on) CaptureQuery(g_call, *tracked.scan);
      break;
    case Call::kPresent:
      CapturePresent(tracker, base);
      break;
    default:
      break;  // the call's state is in the tracker; nothing to do here
  }
}

void OnFrameEnd(uint32_t frame) {
  if (!g_device) return;
  Capture capture(Command::Type::kFrameEnd);
  capture.command.frame = frame;
  capture.command.captured = g_captured;
  g_captured = 0;
  g_frames_captured.fetch_add(1, std::memory_order_relaxed);
}

double AspectFactor() { return g_aspect_factor; }

void GpuBenchmarkStart() { g_gpu_benchmark.Request(); }
bool GpuBenchmarkResult(double* ms) { return g_gpu_benchmark.Result(ms); }
bool TemporalSupport(bool* fsr4, bool* fsr3, bool* xess, bool* dlss) {
  const int support = g_temporal_support.load();
  if (support < 0) return false;
  *fsr4 = (support & 1) != 0, *fsr3 = (support & 2) != 0, *xess = (support & 4) != 0, *dlss = (support & 8) != 0;
  return true;
}
bool NisSupport(bool* runs, std::string* gpu) {
  const int support = g_nis_support.load();
  if (support < 0) return false;
  if (runs) *runs = support == 1;
  if (gpu) *gpu = g_gpu_name;
  return true;
}

void NightModels(const NightModelLamps* models, uint32_t count) {
  std::lock_guard lock(g_night_lamps_mutex);
  if (models && count) g_night_models.assign(models, models + count);
  else g_night_models.clear();
  ++g_night_models_version;
  REXLOG_INFO("[native_x_renderer] night: {} models with lamps (D163)", g_night_models.size());
}

void NightLamps(const NightLamp* lamps, uint32_t count) {
  std::lock_guard lock(g_night_lamps_mutex);
  if (lamps && count) g_night_lamps.assign(lamps, lamps + count);
  else g_night_lamps.clear();
  g_night_lamps_present.store(!g_night_lamps.empty());
  REXLOG_INFO("[native_x_renderer] night: {} lamps of the level (D163)", g_night_lamps.size());
}

void StorageProgress(uint32_t* queued, uint32_t* done, uint32_t* loading, uint32_t* finished) {
  StoredPipelineProgress(queued, done, loading, finished);
}

void SetGameAspectBox(bool on) {
  Capture capture(Command::Type::kAspectBox);
  capture.command.on = on;
}

void BeginView(const float* scene_view_projection) {
  Capture capture(Command::Type::kBeginView);
  capture.command.has_matrix = scene_view_projection != nullptr;
  if (scene_view_projection) std::memcpy(capture.command.matrix.data(), scene_view_projection, 16 * sizeof(float));
}

void EndViewGeometry() { Capture capture(Command::Type::kEndViewGeometry); }

namespace {

void ExecuteFrameEnd(const Command& c) {
  uint32_t frame = c.frame;
  ++g_frames_presented;
  g_timer.SetFrame(frame, REXCVAR_GET(native_x_renderer_hitch_ms));
  {
    // NXR_TEX_LOADS=1: how many frames the game has ended that this thread has not (each 600 frames: average, most).
    static const bool on = [] {
      const char* v = std::getenv("NXR_TEX_LOADS");
      return v && *v == '1';
    }();
    static uint64_t sum = 0;
    static uint32_t most = 0, n = 0;
    if (on) {
      const uint32_t behind = g_frames_captured.load(std::memory_order_relaxed) - g_frames_presented;
      sum += behind;
      most = std::max(most, behind);
      if (++n == 600) {
        REXLOG_INFO("[native_x_renderer] frames the game has ended ahead of the renderer: {:.2f} average, {} most",
                    double(sum) / n, most);
        sum = 0, most = 0, n = 0;
      }
    }
  }
  g_upload_ahead = REXCVAR_GET(native_x_renderer_upload_ahead);
  sdk::NextFrame();
  // Render targets and resolved textures unused for a minute are freed (D52): 3600 frames at 60 FPS, and as many
  // more as the frame rate since the last log is above that (D74; at 120 FPS 3600 frames were half a minute). The
  // resolved textures that still hold their resolve, only above native_x_renderer_resolved_memory_mb (D96).
  {
    uint64_t unused_frames = 3600;
    if (g_pacing.frames && g_pacing.interval_ms > 0) {
      unused_frames = std::max<uint64_t>(unused_frames, uint64_t(60000.0 * g_pacing.frames / g_pacing.interval_ms));
    }
    uint64_t keep_bytes = uint64_t(std::max(0, REXCVAR_GET(native_x_renderer_resolved_memory_mb))) << 20;
    Freed targets = g_targets.EndFrame(unused_frames), resolved = g_resolved.EndFrame(unused_frames, keep_bytes);
    if (targets.count || resolved.count) {
      if (g_draw) g_draw->TargetsChanged();
      if (targets.count) g_gtao.ForgetTargets(), g_ssgi.ForgetTargets();
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: freed after a minute unused: {} render targets ({} MB), {} "
                  "resolved textures ({} MB); {} and {} left",
                  frame, targets.count, targets.bytes >> 20, resolved.count, resolved.bytes >> 20, g_targets.count(),
                  g_resolved.count());
    }
  }
  // The frame was submitted at Present: collect the occlusion query results that arrived.
  if (g_queries_on) g_queries.Poll();
  DrawPath::Stats draws = g_draw ? g_draw->TakeStats() : DrawPath::Stats{};
  {
    // NXR_DRAW_DROPS=1 (a bring-up aid): a frame with under 80% of the last frame's draws is logged with the recorded
    // command buffer runs since the last frame (objects flickering out).
    static const int drops = [] {
      const char* v = std::getenv("NXR_DRAW_DROPS");
      return v && *v ? std::atoi(v) : 0;
    }();
    static uint32_t last_draws = 0;
    static ReplayStats last_replay{};
    // NXR_DRAW_DROPS=2: also every frame that skipped any draw (still translating, its pipeline compiling, failed, or
    // no targets), the first 400: what shows as a flash of a missing object or effect.
    if (drops >= 2 && (draws.translating || draws.compiling || draws.failed || draws.no_targets)) {
      static uint32_t logged = 0;
      if (logged < 400) {
        ++logged;
        REXLOG_INFO("[native_x_renderer] skipped draws: frame {}: {} drawn; skipped {} translating, {} compiling, {} "
                    "failed, {} without targets",
                    frame, draws.draws, draws.translating, draws.compiling, draws.failed, draws.no_targets);
      }
    }
    if (drops) {
      const ReplayStats r = GetReplayStats();
      if (last_draws > 200 && draws.draws * 5 < last_draws * 4) {
        REXLOG_INFO("[native_x_renderer] draw drop: frame {}: {} draws after {}; replays this frame {} ({} unread), their "
                    "draws {} ({} without recorded surfaces); skipped {} translating, {} compiling, {} failed, {} without "
                    "targets",
                    frame, draws.draws, last_draws, r.calls - last_replay.calls, r.unread - last_replay.unread,
                    r.draws - last_replay.draws, r.unmatched - last_replay.unmatched, draws.translating, draws.compiling,
                    draws.failed, draws.no_targets);
      }
      last_replay = r;
    }
    last_draws = draws.draws;
  }
  {
    // native_x_renderer_hitch_ms: a long frame, with what this frame did (for play tests: shader stutter, texture
    // streaming, pacing).
    static uint64_t last_loads = 0, last_loaded_bytes = 0;
    static double last_renderer_ms = 0;
    const uint64_t loads = g_draw ? g_draw->texture_loads() : 0;
    const uint64_t loaded_bytes = g_draw ? g_draw->texture_loaded_bytes() : 0;
    double renderer_ms = g_pacing.renderer_ms - last_renderer_ms;
    if (renderer_ms < 0) renderer_ms = g_pacing.renderer_ms;  // the log's reset, every 600 frames
    const double hitch_ms = REXCVAR_GET(native_x_renderer_hitch_ms);
    if (hitch_ms > 0 && g_pacing.last_interval_ms > hitch_ms) {
      REXLOG_INFO("[native_x_renderer] hitch: frame {}: {:.1f} ms since the last present (GPU wait {:.1f} ms, renderer "
                  "{:.1f} ms); {} draws ({} drawn); created: {} pipelines ({:.1f} ms, max {:.1f}), {} shader analyses "
                  "({:.1f} ms), {} translations done ({:.1f} ms on workers); draws skipped: {} translating, {} "
                  "compiling; {} waited ({:.1f} ms); texture loads {} ({} KB); uploaded {} KB in {} copies; renderer by "
                  "phase: draws {:.1f} ms, clears {:.1f}, resolves {:.1f}, effects {:.1f}, present {:.1f} (swap chain "
                  "{:.1f}, submit {:.1f})",
                  frame, g_pacing.last_interval_ms, g_pacing.last_wait_ms, renderer_ms, draws.draws, draws.drawn,
                  draws.pipelines.count, draws.pipelines.ms, draws.pipelines.max_ms, draws.analyses.count,
                  draws.analyses.ms, draws.translations.count, draws.translations.ms, draws.translating,
                  draws.compiling, draws.waited, draws.waited_ms, loads - last_loads,
                  (loaded_bytes - last_loaded_bytes) >> 10, draws.uploaded_bytes >> 10, draws.copies, g_phase.draws,
                  g_phase.clears, g_phase.resolves, g_phase.effects, g_phase.present, g_pacing.swap_ms, g_pacing.submit_ms);
    }
    g_phase = {};
    g_pacing.swap_ms = g_pacing.submit_ms = 0;
    last_loads = loads;
    last_loaded_bytes = loaded_bytes;
    last_renderer_ms = g_pacing.renderer_ms;
  }
  g_creation.analyses.Merge(draws.analyses);
  g_creation.translations.Merge(draws.translations);
  g_creation.pipelines.Merge(draws.pipelines);
  g_creation.translating_draws += draws.translating;
  g_creation.compiling_draws += draws.compiling;
  g_creation.waited_draws += draws.waited;
  g_creation.waited_ms += draws.waited_ms;
  g_creation.max_waited_ms = std::max(g_creation.max_waited_ms, draws.max_waited_ms);
  ++g_uploads.frames;
  g_uploads.copies += draws.copies;
  g_uploads.max_copies = std::max<uint64_t>(g_uploads.max_copies, draws.copies);
  g_uploads.uploaded_bytes += draws.uploaded_bytes;
  g_uploads.unchanged_bytes += draws.unchanged_bytes;
  g_uploads.unwatched_hashed_bytes += draws.unwatched_hashed_bytes;
  g_uploads.unwatched_pages = draws.unwatched_pages;
  g_uploads.unwatched_copy_bytes = draws.unwatched_copy_bytes;
  g_uploads.ahead_batches += draws.ahead_batches;
  g_uploads.ahead_draws += draws.ahead_draws;
  g_motion.matched += draws.motion_matched;
  g_motion.moved += draws.motion_moved;
  g_motion.drawn += draws.motion_drawn;
  g_motion.camera += draws.motion_camera;
  g_motion.max_drawn = std::max<uint64_t>(g_motion.max_drawn, draws.motion_drawn);
  g_motion.candidates += draws.motion_candidates;
  g_motion.summarized += draws.motion_summarized;
  g_motion.match_ns += draws.motion_match_ns;
  g_motion.draw_ns += draws.motion_draw_ns;
  g_motion.longest_key = std::max<uint64_t>(g_motion.longest_key, draws.motion_longest_key);
  if (g_frames_presented == 1 || g_frames_presented % 600 == 0) {
    REXLOG_INFO("[native_x_renderer] renderer: frame {}: {} clears, {} resolves ({} skipped, {} also written to the guest "
                "memory mirror for vertex reads, D140); {} render targets, {} "
                "resolve targets ({} invalidated by guest memory changes, {} kept with their memory unchanged; {} fetches in "
                "600 frames read guest memory a resolve had written)",
                frame, g_stats.clears, g_stats.resolves, g_stats.skipped_resolves, g_stats.mirror_writes, g_targets.count(),
                g_resolved.count(), g_resolved.TakeInvalidated(), g_resolved.TakeKept(), g_resolved.TakeGoneReads());
    FrameTimer::Result gpu = g_timer.Take();
    REXLOG_INFO("[native_x_renderer] renderer: frame {}: {} draw calls, {} drawn, {} without targets, {} failed, {} waiting for "
                "their pipeline; "
                "{} KB uploaded in {} copies; textures: {} resolved, {} guest, {} placeholders (and {} for the "
                "other dimension); {} texture and sampler bindings set, shader resources committed in {} draws "
                "(binding sets: {} created, {} kept, D111); {} "
                "shaders, {} pipelines; GPU {:.2f} ms average, {:.2f} ms max over {} frames ({} more than 32 frames "
                "late)",
                frame, draws.draws, draws.drawn, draws.no_targets, draws.failed, draws.compiling,
                draws.uploaded_bytes >> 10,
                draws.copies, draws.textures_resolved, draws.textures_guest, draws.textures_placeholder,
                draws.textures_other_dimension, draws.bindings_set, draws.committed, draws.binding_sets_created,
                draws.binding_sets_kept,
                g_draw ? g_draw->shader_count() : 0, g_draw ? g_draw->pipeline_count() : 0, gpu.average_ms,
                gpu.max_ms, gpu.frames, gpu.late);
    if (g_aa.mode() != Antialiasing::Mode::kOff) {
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: antialiasing ({}) applied {} times in {} frames ({} sets of "
                  "its image sized textures made, D141)",
                  frame, Antialiasing::ModeName(g_aa.mode()), g_aa.TakeApplied(), g_uploads.frames, g_aa.TakeMade());
    }
    if (const uint64_t scoped = g_scope.TakeApplied(); scoped || g_scope_unzoomed) {
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: the picture in picture scope drawn in {} of {} frames, the "
                  "game's scoped picture {} times without a scope's zoom (D168)",
                  frame, scoped, g_uploads.frames, g_scope_unzoomed);
      g_scope_unzoomed = 0;
    }
    if (REXCVAR_GET(native_x_renderer_night)) {
      uint64_t world = 0, glow = 0, lights = 0, sprites = 0;
      const uint64_t applied = g_night.TakeApplied();
      g_night.TakeCounts(world, glow, lights, sprites);
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: night applied {} times in {} frames, {} with the world's "
                  "depth from before the weapon (D162); the effects' light in {} ({} copies of their layer), {} lights "
                  "and {} lamp glows drawn in all, {} models' lamps placed (D163)", frame, applied, g_uploads.frames,
                  world, glow, g_night_glow_frames, lights, sprites, g_night_models_placed);
      g_night_glow_frames = 0;
      g_night_models_placed = 0;
    }
    if (g_gtao_on) {
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: ambient occlusion (GTAO) applied {} times in {} frames",
                  frame, g_gtao.TakeApplied(), g_uploads.frames);
    }
    if (g_ssgi_on) {
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: global illumination (SSGI) applied {} times in {} frames",
                  frame, g_ssgi.TakeApplied(), g_uploads.frames);
    }
    if (g_scaling.active()) {
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: scaling into the window applied {} times in {} frames",
                  frame, g_scaling.TakeApplied(), g_uploads.frames);
    }
    if (g_temporal) {
      uint32_t applied = g_upscaler.TakeApplied();
      double n = std::max(double(applied), 1.0);
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: {} applied {} times in {} frames; object motion (D48), "
                  "per application: {:.0f} draws matched with the last frame's, {:.0f} moved, {:.0f} motion draws "
                  "({} max), {:.0f} with the camera's motion (D120)",
                  frame, g_upscaler.Name(), applied, g_uploads.frames, g_motion.matched / n,
                  g_motion.moved / n, g_motion.drawn / n, g_motion.max_drawn, g_motion.camera / n);
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: object motion's cost per application: matching {:.2f} ms "
                  "({:.0f} of the last frame's draws compared, {:.0f} of them ruled out by their summary (D143), at most {} of "
                  "one key), the motion draws {:.2f} ms",
                  frame, g_motion.match_ns / n / 1e6, g_motion.candidates / n, g_motion.summarized / n, g_motion.longest_key,
                  g_motion.draw_ns / n / 1e6);
      g_motion = {};
      if (g_frame_generation) {
        FrameGeneration::Stats generation = g_framegen.TakeStats();
        OverlayMotion::Stats overlay = g_overlay_motion.TakeStats();
        REXLOG_INFO("[native_x_renderer] renderer: frame {}: frame generation (D123): {} frames generated, {} game "
                    "frames without one; the game's frames {:.2f} ms apart; the 2D layer's motion (D124): {} draws, "
                    "{} matched with the last frame's",
                    frame, generation.generated, generation.skipped, g_game_frame_ms, overlay.draws, overlay.matched);
      }
      if (g_camera_vote) {
        double voted = std::max(double(g_vote_stats.voted), 1.0);
        REXLOG_INFO("[native_x_renderer] renderer: frame {}: camera vote (D118): {} of {} views voted, {:.0f} offers "
                    "and {:.0f} votes for the winner per view; its difference from the view hook's: {:.3f} px average, "
                    "{:.3f} px max ({} views compared)",
                    frame, g_vote_stats.voted, g_vote_stats.applications, g_vote_stats.offers / voted,
                    g_vote_stats.votes / voted,
                    g_vote_stats.compared ? g_vote_stats.difference / g_vote_stats.compared : 0.0,
                    g_vote_stats.max_difference, g_vote_stats.compared);
        g_vote_stats = {};
      }
    }
    if (g_uploads.frames) {
      double n = double(g_uploads.frames);
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: per frame, {:.1f} buffer copies average, {} max; guest memory "
                  "{:.0f} KB copied, {:.0f} KB requested again but unchanged (not copied); {} pages rewritten every "
                  "frame left unwatched (D97), {:.0f} KB of them compared per frame (their copy on the CPU {} MB, "
                  "D110); uploads ahead (D98): {:.1f} batches per frame, {:.1f} draws each",
                  frame, g_uploads.copies / n, g_uploads.max_copies, g_uploads.uploaded_bytes / n / 1024,
                  g_uploads.unchanged_bytes / n / 1024, g_uploads.unwatched_pages,
                  g_uploads.unwatched_hashed_bytes / n / 1024, g_uploads.unwatched_copy_bytes >> 20,
                  g_uploads.ahead_batches / n,
                  g_uploads.ahead_batches ? double(g_uploads.ahead_draws) / g_uploads.ahead_batches : 0.0);
      g_uploads = {};
    }
    if (g_draw) {
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: since the start, {} guest texture loads ({} MB), {} skipped "
                  "(data unchanged), {} textures not created (unsupported format); {} guest textures on the host "
                  "({} MB)",
                  frame, g_draw->texture_loads(), g_draw->texture_loaded_bytes() >> 20, g_draw->texture_loads_unchanged(),
                  g_draw->textures_unsupported(), g_draw->texture_count(), g_draw->texture_host_bytes() >> 20);
    }
    // Device memory (D55): what DiligentCore's memory manager has in use and in its pages, next to what
    // NativeXRenderer knows it holds. Used below allocated is memory kept in partly empty pages; used above
    // the parts is memory held by something else (released resources still referenced, among others).
    if (RefCntAutoPtr<IRenderDeviceVk> vulkan{g_device->device(), IID_RenderDeviceVk}) {
      Uint64 used = 0, allocated = 0, host_used = 0, host_allocated = 0;
      Uint32 pages = 0;
      vulkan->GetMemoryUsage(&used, &allocated, &host_used, &host_allocated, &pages);
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: device memory {} MB used in {} MB of pages ({} pages), "
                  "host-visible {} MB in {} MB; of it: guest textures {} MB, render targets {} MB ({}), resolved "
                  "textures {} MB ({})",
                  frame, used >> 20, allocated >> 20, pages, host_used >> 20, host_allocated >> 20,
                  g_draw ? g_draw->texture_host_bytes() >> 20 : 0, g_targets.bytes() >> 20, g_targets.count(),
                  g_resolved.bytes() >> 20, g_resolved.count());
    }
    if (g_pacing.frames) {
      double n = g_pacing.frames;
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: frame interval {:.2f} ms average, {:.2f} ms max; renderer "
                  "CPU {:.2f} ms per frame; waiting for the GPU {:.2f} ms per frame, {:.2f} ms max",
                  frame, g_pacing.interval_ms / n, g_pacing.max_interval_ms, g_pacing.renderer_ms / n,
                  g_pacing.wait_ms / n, g_pacing.max_wait_ms);
      if (g_present_with_swaps) {
        REXLOG_INFO("[native_x_renderer] renderer: frame {}: shown with the guest's swaps (D86): {} at their swap, {} "
                    "before it (two newer frames waiting), {} after waiting {} ms for it; {} skipped (a newer one's "
                    "swap had executed too)",
                    frame, g_pacing.at_swap, g_pacing.before_swap, g_pacing.timed_out, kSwapTimeout.count(),
                    g_pacing.skipped);
      }
      if (g_queue.threaded()) {
        CommandQueue::Waits producer = g_queue.TakeProducerWaits(), gate = g_queue.TakeGateWaits();
        REXLOG_INFO("[native_x_renderer] renderer: frame {}: on its own thread (D71): {} commands in the last frame; "
                    "per frame, the game's thread waited {:.2f} ms for room in the queue ({:.1f} times), the command "
                    "processor {:.2f} ms at its signals ({:.1f} times); {} inline draws' data restored",
                    frame, c.captured, producer.ms / n, producer.count / n, gate.ms / n, gate.count / n,
                    draws.inline_restored);
      }
    }
    REXLOG_INFO("[native_x_renderer] renderer: frame {}: created on the render thread: {} shader analyses ({:.1f} ms, "
                "max {:.1f}), {} pipelines ({:.1f} ms, max {:.1f}); on worker threads: {} translations ({:.1f} ms, "
                "max {:.1f}); draws skipped: {} while a shader was translated, {} while their pipeline compiled; "
                "offscreen draws that waited for theirs (D101): {} ({:.1f} ms, max {:.1f})",
                frame, g_creation.analyses.count, g_creation.analyses.ms, g_creation.analyses.max_ms,
                g_creation.pipelines.count, g_creation.pipelines.ms, g_creation.pipelines.max_ms,
                g_creation.translations.count, g_creation.translations.ms, g_creation.translations.max_ms,
                g_creation.translating_draws, g_creation.compiling_draws, g_creation.waited_draws,
                g_creation.waited_ms, g_creation.max_waited_ms);
    g_creation = {};
    if (g_queries_on) {
      OcclusionQueries::Stats q = g_queries.TakeStats();
      // The first few addresses (GRAW has hundreds).
      std::string max;
      size_t listed = 0;
      for (const auto& [address, samples] : q.max_samples) {
        if (listed++ == 8) break;
        max += fmt::format(" {:#x}: {}", address, samples);
      }
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: occlusion queries: {} begun ({} most in a frame, {} over the "
                  "frame's budget), {} ended, {} results arrived for {} addresses (most samples per query:{}{}); {} "
                  "answered to the command processor, {} before any result",
                  frame, q.begun, q.most_in_a_frame, q.over_budget, q.ended, q.arrived, q.max_samples.size(),
                  max.empty() ? " none" : max, q.max_samples.size() > 8 ? " ..." : "", q.answered, q.unanswered);
    }
    {
      // Recorded command buffer runs (Battlefield 1943's job threads record most of its scene): their draws, and those
      // whose recording was not found (D129, D131: drawn into the device's bound surfaces, which may not be the ones
      // they were recorded for: an object or shadow caster in the wrong target for that frame).
      static ReplayStats last{};
      static uint64_t last_recorded = 0;
      const ReplayStats r = GetReplayStats();
      const RecordedDrawsSize kept = GetRecordedDrawsSize();
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: recorded command buffers in the last 600 frames: {} runs ({} "
                  "unread), {} draws, {} of them without recorded surfaces; {} draws recorded, {} kept (at most {})",
                  frame, r.calls - last.calls, r.unread - last.unread, r.draws - last.draws, r.unmatched - last.unmatched,
                  kept.recorded - last_recorded, kept.kept, kept.most);
      last = r;
      last_recorded = kept.recorded;
    }
    Device::FenceState fences = g_device->GetFenceState();
    REXLOG_INFO("[native_x_renderer] renderer: frame {}: frames presented {}, completed {}; DiligentCore queue fence "
                "next {}, completed {}",
                frame, fences.frames_presented, fences.frames_completed, fences.queue_next, fences.queue_completed);
    // DiligentCore ignores Vulkan errors in release builds: after VK_ERROR_DEVICE_LOST, waits return at once
    // (frames look complete) while the queue fence stops advancing (it only reads fence status).
    static bool reported_loss = false;
    if (!reported_loss && fences.queue_next - fences.queue_completed > 64) {
      reported_loss = true;
      REXLOG_ERROR("[native_x_renderer] DiligentCore's command queue is {} submissions behind while frames complete: "
                   "the Vulkan device is probably lost (DiligentCore does not report it in release builds)",
                   fences.queue_next - fences.queue_completed);
    }
    auto last_present = g_pacing.last_present;
    g_pacing = {};
    g_pacing.last_present = last_present;
  }
  if (g_draw) g_draw->EndFrame();
  g_stats = {};
}

void Execute(Command& c) {
  if (g_pending_count) PresentPending();
  // A command past the last uploads-ahead window (D98): its data is no longer known to be as the window found it.
  if (g_draw && g_queue.running_index() >= g_ahead_until) g_draw->EndAheadWindow();
  ApplyRecord(c.registers.data(), c.registers.size(), g_registers->values);
  switch (c.type) {
    case Command::Type::kDraw:
      ExecuteDraw(c);
      break;
    case Command::Type::kClear:
      ExecuteClear(c);
      break;
    case Command::Type::kResolve:
      ExecuteResolve(c);
      break;
    case Command::Type::kBeginTiling:
      ExecuteBeginTiling(c);
      break;
    case Command::Type::kEndTiling:
      ExecuteEndTiling(c);
      break;
    case Command::Type::kQuery:
      ExecuteQuery(c);
      break;
    case Command::Type::kPresent:
      ExecutePresent(c);
      break;
    case Command::Type::kFrameEnd:
      ExecuteFrameEnd(c);
      break;
    case Command::Type::kBeginView:
      ExecuteBeginView(c);
      break;
    case Command::Type::kEndViewGeometry:
      ExecuteEndViewGeometry();
      break;
    case Command::Type::kAspectBox:
      ExecuteAspectBox(c);
      break;
    case Command::Type::kNone:
      break;
  }
}

}  // namespace

}  // namespace native_x_renderer::renderer
