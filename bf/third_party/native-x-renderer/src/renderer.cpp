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

#include "GraphicsAccessories.hpp"
#include "renderer.h"

#include "aa.h"
#include "cpu_cores.h"
#include "gtao.h"
#include "grass.h"
#include "night.h"
#include "scope.h"
#include "ssgi.h"
#include "dof.h"
#include "fluid.h"
#include "ground_fog.h"
#include "cloud_mass.h"
#include "sky.h"
#include "weapon_export.h"
#include "weapon_skin.h"
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
#include <rex/thread.h>
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <string>

#include <xxhash.h>

#if defined(_M_X64)
#include <intrin.h>
#elif defined(__x86_64__)
#include <x86intrin.h>
#endif

REXCVAR_DEFINE_INT32(native_x_renderer_replay_threads, 3, "NativeXRenderer",
                     "Threads that read recorded command buffer runs in parallel; 0 reads them on the game's thread");
REXCVAR_DEFINE_BOOL(native_x_renderer_replay_recorded_targets, false, "NativeXRenderer",
                    "Draws replayed from recorded command buffers render into the targets the recording device had "
                    "bound (D129) instead of the running device's (D131); for diagnosis");
REXCVAR_DEFINE_BOOL(native_x_renderer_ads_dof, false, "NativeXRenderer",
                    "Depth of field of the renderer's own while the player aims down the sights (dof.h): the back "
                    "of the gun at the eye is blurred, less toward its front, the sights in the middle not; taken at once");
REXCVAR_DEFINE_STRING(native_x_renderer_dof_quality, "medium", "NativeXRenderer",
                      "Depth of field: low, medium or high (24, 48 or 80 taps, gathered at half size); taken at once");
REXCVAR_DEFINE_STRING(native_x_renderer_motion_blur, "off", "NativeXRenderer",
                      "Motion blur of the renderer's own on the main 3D view (motion_blur.h): off, low, medium or high "
                      "(6, 10 or 16 taps); taken at once");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_motion_blur_strength, 50.0, "NativeXRenderer",
                      "Motion blur: its strength, 0 to 100 (%): the exposure, of a sixtieth of a second");
REXCVAR_DEFINE_BOOL(native_x_renderer_motion_blur_objects, true, "NativeXRenderer",
                    "Motion blur: the objects that move on their own (soldiers, vehicles) blur by their own motion too");
// The sky of its own (sky.h): stage 1 of the day and night work, 14 Nov 2026.
// "an option in the graphics settings for players to choose if they want the volumetric clouds on, or if they want
// to use the original sky ... can we have the original sky be affected by the lighting changes for day and night"
// (14 Nov 2026): the day and night is one setting (the light, the shadows and whichever sky follow the time of day),
// the sky another (native_x_renderer_sky: the renderer's own with volumetric clouds, or the game's painted one,
// which is then tinted by the hour).
REXCVAR_DEFINE_BOOL(native_x_renderer_day_night, false, "NativeXRenderer",
                    "Day and night: the game's light, its shadows and the sky follow the time of day");
REXCVAR_DEFINE_BOOL(native_x_renderer_sky, false, "NativeXRenderer",
                    "The renderer's own sky in place of the game's: time of day, sun, moon, stars and volumetric clouds");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_time_of_day, 16.0, "NativeXRenderer",
                      "The sky's time of day, 0 to 24 hours (12 noon, 0 midnight)");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_day_length, 0.0, "NativeXRenderer",
                      "Minutes a whole day takes, the time of day running on from its setting; 0: it stands still");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_cloud_cover, 0.35, "NativeXRenderer", "How much of the sky the clouds take, 0 to 1");
// "We just don't want the clouds accumulating and dissipating at an unrealistic speed" (14 Nov 2026): the rate is a
// setting, against Beachhead's full cloud generation speed.
REXCVAR_DEFINE_DOUBLE(native_x_renderer_cloud_rate, 0.33, "NativeXRenderer",
                      "How fast the volumetric clouds form and change, 0 to 1 (1: Beachhead's full generation speed)");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_weather, 0.0, "NativeXRenderer",
                      "The weather, 0 fair to 1 heavy overcast: more and denser cloud, a greyer sky, less sun");
// "let's do the ground fog, distance fog, rain shafts, storm mist, and lightning in clouds" (14 Nov 2026): sky.h's Fog.
// The rain shafts, the storm's mist and the lightning come with the weather.
REXCVAR_DEFINE_DOUBLE(native_x_renderer_fog, 0.0, "NativeXRenderer",
                      "The distance fog, 0 to 1: a haze between the eye and what is far, thinner higher up");
// "Rain should also tilt/angle when wind is higher % (which should also be part of the weather sliders)" (14 Nov 2026).
REXCVAR_DEFINE_DOUBLE(native_x_renderer_wind, 0.3, "NativeXRenderer",
                      "The wind, 0 calm to 1 a gale: how far the rain and the far rain shafts lean");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_cloud_height, 450.0, "NativeXRenderer",
                      "The clouds' base over the sea, metres (100 to 3000): low enough, the planes fly through them "
                      "(with native_x_renderer_world_own; a match's server sets its own)");
REXCVAR_DEFINE_INT32(native_x_renderer_storm_type, 0, "NativeXRenderer",
                     "Developer: the storm's type with native_x_renderer_world_own (D224): 0 a thunderstorm, 1 a "
                     "hurricane (a match's server sets its own)");
REXCVAR_DEFINE_BOOL(native_x_renderer_fluid_storm, true, "NativeXRenderer",
                    "The storm made of the sky's own clouds by a particle fluid (D229): its air pulls them in and turns "
                    "them round as the weather rises, convection from scattered cells organising into bands (a "
                    "hurricane's on Hurricane Michael's radar); off, the storm's own drawn shape");
REXCVAR_DEFINE_BOOL(native_x_renderer_cloud_shadows, true, "NativeXRenderer",
                    "The clouds cast their shadows on the ground by day (D214)");
REXCVAR_DEFINE_BOOL(native_x_renderer_lightning, true, "NativeXRenderer",
                    "Lightning: a storm's flashes in the clouds, on the fog and on the ground");
REXCVAR_DEFINE_BOOL(native_x_renderer_reflections, false, "NativeXRenderer",
                    "Reflections of the renderer's own on the sea (screen space; the game's day sky cube map left out "
                    "of its shaders, D190)");
REXCVAR_DEFINE_BOOL(native_x_renderer_glass_reflections, false, "NativeXRenderer",
                    "Experimental (D191): the window glass's reflections of the renderer's own in place of the game's "
                    "day sky cube map; with native_x_renderer_reflections");
REXCVAR_DEFINE_BOOL(native_x_renderer_storm_sway, true, "NativeXRenderer",
                    "A strong storm's shake: on foot in the open the view shakes lightly in the gusts, its edges blurred (D189, D193)");
REXCVAR_DEFINE_BOOL(native_x_renderer_rain_lens, true, "NativeXRenderer",
                    "Rain on the lens: while you stand in the rain, drops gather on the view and run down it");
REXCVAR_DEFINE_BOOL(native_x_renderer_wetness, true, "NativeXRenderer",
                    "Wetness: what you hold gets wet in the rain and in the sea, with beads of water, and dries again");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_ground_fog, 0.0, "NativeXRenderer",
                      "The ground fog, 0 to 1: a low layer over the sea and the shore in drifting patches");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_ground_fog_height, 0.5, "NativeXRenderer",
                      "How deep the ground fog is, metres (0.2 to 5; 0.5 is shin high)");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_ground_fog_sparse, 0.7, "NativeXRenderer",
                      "How sparse the ground fog is, 0 everywhere to 1 a few small patches");
REXCVAR_DEFINE_BOOL(native_x_renderer_cloud_particles, false, "NativeXRenderer",
                    "Developer: the clouds' mass as particles (their tracers carried by the wind and drawn into a storm; "
                    "the clouds' march reads their count in place of its weather map)");
REXCVAR_DEFINE_BOOL(native_x_renderer_fog_push, true, "NativeXRenderer",
                    "The ground fog is pushed about by what moves through it, by blasts, shots, the wind and the rain, "
                    "and comes back");
REXCVAR_DEFINE_BOOL(native_x_renderer_world_own, false, "NativeXRenderer",
                    "Developer: the player's own time, weather and fog settings even in a match whose server keeps them");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_sun_azimuth, -1.0, "NativeXRenderer",
                      "Where the sky's sun stands at noon, degrees around the world's up axis; under 0: from the game's "
                      "own sun, so that the sky's sun stands where the game's shadows say at their hour");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_sky_exposure, 3.0, "NativeXRenderer",
                      "The sky's brightness in the view's own light units");
REXCVAR_DEFINE_BOOL(native_x_renderer_sky_light, true, "NativeXRenderer",
                    "With the renderer's own sky, the game's daylight on the ground follows its time of day");
REXCVAR_DEFINE_BOOL(native_x_renderer_sky_sun_moves, true, "NativeXRenderer",
                    "With the sky's light on the ground, the light's direction and the game's shadows turn with the "
                    "sky's sun and moon");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_night_rooms, 1.0, "NativeXRenderer",
                      "D222: how much of the night's light in the buildings' rooms (NightRooms) is drawn, 0 none to 2 "
                      "twice the tuned lift");
REXCVAR_DEFINE_BOOL(native_x_renderer_night_rooms_debug, false, "NativeXRenderer",
                    "Developer: D222's rooms tinted green where their light is drawn, red where a wall's outside face "
                    "is left out");
REXCVAR_DEFINE_INT32(native_x_renderer_sky_debug, 0, "NativeXRenderer",
                     "Developer: 1 paints the pixels the sky takes in one flat colour; 4 the fog's distances in "
                     "bands, 5 its heights over the sea; 7 to 10 the sea's reflection (D204)");
REXCVAR_DEFINE_INT32(native_x_renderer_msaa, 1, "NativeXRenderer",
                     "Multisampling of the game's view (its targets of the output's size): 1 (none), 2 or 4 samples");
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
REXCVAR_DEFINE_DOUBLE(native_x_renderer_night_glow, 1.0, "NativeXRenderer",
                      "Night's lights (D163): how strongly the effects keep their own light (muzzle flashes, tracers, fire, "
                      "explosions; 0 none)");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_night_lamps, 1.0, "NativeXRenderer",
                      "Night's lights (D163): how strongly the level's lamps light their surroundings and glow (0 none)");
REXCVAR_DEFINE_BOOL(native_x_renderer_lamp_bloom, true, "NativeXRenderer",
                    "Night's lights (D183): a soft bloom round a lit lamp's glass");
REXCVAR_DEFINE_BOOL(native_x_renderer_lens_flare, true, "NativeXRenderer",
                    "Night's lights (D183): a subtle lens flare, halo and haze from the brightest lamps in view");
REXCVAR_DEFINE_BOOL(native_x_renderer_cloud_checkerboard, false, "NativeXRenderer",
                    "The sky's clouds: half their texels marched each frame, the rest filled in from their neighbours "
                    "and the last frames (about half the clouds' cost; a little softer in motion)");
REXCVAR_DEFINE_BOOL(native_x_renderer_gpu_sections, false, "NativeXRenderer",
                    "The GPU time of the renderer's own passes (sky, fog, grass, AO, the night pass, ...), logged every "
                    "600 frames (tests: a few duration queries a frame)");
REXCVAR_DEFINE_BOOL(native_x_renderer_night_lamp_visibility_pass, true, "NativeXRenderer",
                    "Night's lights: each lamp's visibility (for its bloom and lens flare) found once a frame in a small "
                    "pass, not again for every pixel (false: the old per pixel way, for comparisons)");
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
REXCVAR_DEFINE_BOOL(native_x_renderer_hide_hud, false, "NativeXRenderer",
                    "No HUD over a frame with a 3D view: the draws after the composite are skipped "
                    "(Profile::box_2d_after_composite; menus and frames without a 3D view are drawn as ever). For "
                    "screenshots and the benchmark's free camera (bench_freecam.cpp turns it on)");
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
REXCVAR_DEFINE_BOOL(native_x_renderer_record_thread, true, "NativeXRenderer",
                    "With native_x_renderer_thread: the draws' DiligentCore calls made on a second thread, the "
                    "recorder's, while the renderer's thread prepares the next draws (read at startup)");
REXCVAR_DEFINE_INT32(native_x_renderer_record_thread_cores, 4, "NativeXRenderer",
                     "The recorder's thread (native_x_renderer_record_thread) only with at least this many processor "
                     "cores (read at startup): on fewer it competes with the game's own threads (Battlefield 1943 "
                     "Reborn, 15 Nov 2026, two cores at 90 FPS: 1% low 34 FPS with it, 78 without)");
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
REXCVAR_DEFINE_BOOL(native_x_renderer_check_temporal, true, "NativeXRenderer",
                    "At the start, ask AMD's FSR and Intel's XeSS libraries whether this GPU runs them, for a menu's "
                    "temporal upscaler choices (TemporalSupport). Off: neither library is loaded, and none is "
                    "reported (a game that offers no temporal upscaler: D171)");
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
// The weapon skin's overlay of this frame, to go over the picture after the night pass (weapon_skin.h).
WeaponSkinBloomInputs g_skin_overlay;
uint64_t g_skin_overlay_frame = ~0ull;
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
Diligent::RefCntAutoPtr<Diligent::ITexture> g_glass_mask;  // D191: the window glass's depth, for its reflection
Diligent::RefCntAutoPtr<Diligent::ITexture> g_sea_mask;  // D190: where the sea is drawn, for its reflection
uint64_t g_night_world_frame = ~0ull;
float g_night_world_uv[2] = {1.0f, 1.0f};  // the view's share of it
// D163: the effects' layer (the game's half size pass of its emissive particles, copied with its mips), its frame.
Diligent::RefCntAutoPtr<Diligent::ITexture> g_night_glow;
// D179: the effects' layer as it was before the effects were drawn into it (a copy of its whole texture, taken at
// the first draw into it after the game's sky dome's), whose layer it is, and the two views of them at the
// layer's size the difference is made from.
Diligent::RefCntAutoPtr<Diligent::ITexture> g_night_glow_before, g_night_glow_a, g_night_glow_b;
uint64_t g_night_glow_before_frame = ~0ull;
const HostTarget* g_night_glow_before_of = nullptr;
const HostTarget* g_night_glow_dome_into = nullptr;  // the sky dome's draws went into this target, this frame
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
// D222: the rooms of the level's buildings (NightRooms, under the lamps' lock, and its version), and the fog pass's
// copy of them. The render thread's: the rooms as last copied, the index counts of their buildings' standing parts
// (sorted, for the draw path), and the frame each room's standing part was last drawn at its building.
std::vector<NightRoom> g_night_rooms;
uint64_t g_night_rooms_version = 0;
std::vector<NightRoom> g_night_rooms_frame;
std::vector<NightRoom> g_night_rooms_render;
uint64_t g_night_rooms_render_version = 0;
std::vector<uint32_t> g_room_intact_counts;
std::vector<uint64_t> g_room_seen;
uint64_t g_room_frame = 0;
uint32_t g_room_matches = 0;  // since the last log line
void NoteIntactPart(uint32_t index_count, const float* places, uint32_t n) {
  for (size_t i = 0; i < g_night_rooms_render.size(); ++i) {
    const NightRoom& r = g_night_rooms_render[i];
    if (r.intact[0] != index_count && r.intact[1] != index_count) continue;
    for (uint32_t k = 0; k < n; ++k) {
      const float dx = places[k * 3] - r.building[0], dy = places[k * 3 + 1] - r.building[1], dz = places[k * 3 + 2] - r.building[2];
      if (dx * dx + dy * dy + dz * dz < 0.75f * 0.75f) {
        g_room_seen[i] = g_room_frame;
        ++g_room_matches;
        break;
      }
    }
  }
}
std::vector<NightModelLamps> g_night_models_render;
std::vector<uint32_t> g_night_model_counts;
uint64_t g_night_models_render_version = 0;
struct NightModelDraw {
  uint32_t index_count;
  Mat4d m;
};
std::vector<NightModelDraw> g_night_model_draws;
std::atomic<bool> g_night_vehicle_seated{false};  // D178: the player sits in a vehicle (NightVehicleSeated)
// D208: the level's middle (SetMapArea), where the hurricane comes to; (0, 0) until a level's points are known.
std::atomic<float> g_map_x{0.0f}, g_map_z{0.0f}, g_map_reach{1200.0f};  // (D213: its reach, 1200 m until known)
std::atomic<bool> g_map_known{false};
// D228: the storm's radar pattern (SetStormPattern, tools/storm_bake.py), kept until the render thread makes its
// texture (a 2D array, a layer a frame, R8).
std::mutex g_storm_pattern_mutex;
std::vector<uint8_t> g_storm_pattern_bytes;
StormPattern g_storm_pattern_meta;  // (its `frames` not kept: the bytes above)
bool g_storm_pattern_new = false;
RefCntAutoPtr<ITexture> g_storm_pattern_texture;
// The pattern's texture, made on the render thread when a new pattern came (null: none yet).
ITexture* StormPatternTexture() {
  std::lock_guard lock(g_storm_pattern_mutex);
  if (g_storm_pattern_new && g_device) {
    g_storm_pattern_new = false;
    const StormPattern& m = g_storm_pattern_meta;
    TextureDesc d;
    d.Name = "NativeXRenderer storm pattern";
    d.Type = RESOURCE_DIM_TEX_2D_ARRAY;
    d.Width = m.width, d.Height = m.height, d.ArraySize = m.count;
    d.Format = TEX_FORMAT_R8_UNORM;
    d.Usage = USAGE_IMMUTABLE;
    d.BindFlags = BIND_SHADER_RESOURCE;
    std::vector<TextureSubResData> layers(m.count);
    for (uint32_t k = 0; k < m.count; ++k)
      layers[k].pData = g_storm_pattern_bytes.data() + size_t(k) * m.width * m.height, layers[k].Stride = m.width;
    TextureData data(layers.data(), m.count);
    g_storm_pattern_texture.Release();
    g_device->device()->CreateTexture(d, &data, &g_storm_pattern_texture);
    REXLOG_INFO("[native_x_renderer] storm pattern (D228): {} frames of {}x{}, the eye {:.1f} px in radius, {:.3f} km a "
                "pixel, {:.1f} minutes apart: {}",
                m.count, m.width, m.height, m.eye_px, m.km_per_px, m.minutes,
                g_storm_pattern_texture ? "made" : "not made");
  }
  return g_storm_pattern_texture;
}
uint64_t g_night_models_placed = 0;  // models placed with their lamps since the last log line
void ReportNightModelDraw(uint32_t index_count, const Mat4d& m) {
  if (g_night_model_draws.size() < 48) g_night_model_draws.push_back({index_count, m});
}
std::vector<NightLight> g_night_frame_lights;
std::vector<NightSprite> g_night_frame_sprites;
// D175: the night's lights at the sky's day and night (16 Nov 2026, TekRantGaming: the night maps gone, the day and night
// cycle of pull request 32 in their place, the lights kept). How dark the cycle's hour is for them: 0 by day, rising as
// the sun nears the horizon, 1 once it is 6 degrees down (set with the outdoor light, read on every draw: no lock).
std::atomic<float> g_cycle_dark{0.0f};
// The night's lights' strength: the cycle's darkness.
float NightLightsAmount() { return g_cycle_dark.load(std::memory_order_relaxed); }
// The night pass runs (its depths, the effects' layer, the lamps' camera): while the cycle is dark enough for them.
bool NightPassOn() { return NightLightsAmount() > 0.01f; }
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
TerrainFrame g_fog_terrain;  // D231: the last terrain seen (its heightmap kept), for the ground fog's ground
bool g_scene_antialiased = false;  // this frame's scene copy antialiased already (Profile::scene_resolve_once, D94)
Upscaler g_upscaler;   // NVIDIA DLSS (Phase 5, D46)
// Motion blur (motion_blur.h, 0.4.2): made at its first use; the view's camera vote kept for it (the ambient
// occlusion or the upscaler takes it first), and the time of its last frame.
DepthOfField g_dof;
int g_dof_state = 0;  // 0 not made, 1 made, -1 could not be
// The sky of its own (sky.h): made at its first use; the moment its time began to run.
Sky g_own_sky;
int g_sky_state = 0;  // 0 not made, 1 made, -1 could not be
std::chrono::steady_clock::time_point g_sky_start{};
bool SkyLoaded() {
  if (g_sky_state < 0 || !g_device) return false;
  if (g_sky_state == 0) {
    g_sky_state = g_own_sky.Load(g_device->device()) ? 1 : -1;
    g_sky_start = std::chrono::steady_clock::now();
  }
  return g_sky_state == 1;
}
// NXR_SKY_OFF=1 (a developer's, D229): the renderer's sky off whatever the setting (a server's join turns it on).
bool SkyForcedOff() {
  static const bool off = [] {
    const char* v = std::getenv("NXR_SKY_OFF");
    return v && *v == '1';
  }();
  return off;
}
bool SkyOn() { return REXCVAR_GET(native_x_renderer_sky) && !SkyForcedOff() && SkyLoaded(); }
// The clouds' march passes over the rays the scene hides (sky.cpp's kCloudPS). BF_CLOUD_AB=1 (tests): on and off by
// turns, 600 frames each (a stats line's span), so both ways are timed in one run.
bool AbEveryFrame() {
  static const bool on = [] {
    const char* v = std::getenv("BF_AB_EVERY_FRAME");
    return v && *v == '1';
  }();
  return on;
}
uint64_t AbPhase() {
  if (!g_device) return 0;
  return AbEveryFrame() ? g_device->frames_presented() % 2 : (g_device->frames_presented() / 600) % 2;
}
// native_x_renderer_cloud_checkerboard: half the cloud texels marched each frame (about half the march's GPU time; in
// motion the clouds a little softer for a frame or two). BF_CLOUD_CB_AB=1 (tests): on and off by turns, 600 frames each.
// NXR_CLOUD_OLD=1 (a developer's, D229): the clouds' march held on the old way, before the Night Ops performance
// pass's changes (the shape, the column's depth and the bands' noises for every sample; hidden rays marched).
bool CloudOld() {
  static const bool old = [] {
    const char* v = std::getenv("NXR_CLOUD_OLD");
    return v && *v == '1';
  }();
  return old;
}
bool CloudCheckerNow() {
  static const bool ab = [] {
    const char* v = std::getenv("BF_CLOUD_CB_AB");
    return v && *v == '1';
  }();
  if (ab) return g_device && AbPhase() == 0;
  return REXCVAR_GET(native_x_renderer_cloud_checkerboard);
}
// BF_CLOUD_DENSITY_AB=1 (tests): the clouds' shape looked up for every sample (the old way) and only where it may
// show (the new) by turns, 600 frames each.
bool CloudDensityOldNow() {
  if (CloudOld()) return true;
  static const bool ab = [] {
    const char* v = std::getenv("BF_CLOUD_DENSITY_AB");
    return v && *v == '1';
  }();
  return ab && g_device && AbPhase() == 1;
}
// BF_CLOUD_REACH_AB=1 (tests): the clouds' column depth looked up for every sample (the old way) and only where the
// sample can be in the column (the new) by turns (with BF_AB_EVERY_FRAME every frame).
bool CloudReachOldNow() {
  if (CloudOld()) return true;
  static const bool ab = [] {
    const char* v = std::getenv("BF_CLOUD_REACH_AB");
    return v && *v == '1';
  }();
  return ab && g_device && AbPhase() == 1;
}
// BF_CLOUD_BANDS_AB=1 (tests): the clouds' bands' noises worked out for every sample (the old way) or only where there
// are bands (the new) by turns.
bool CloudBandsOldNow() {
  if (CloudOld()) return true;
  static const bool ab = [] {
    const char* v = std::getenv("BF_CLOUD_BANDS_AB");
    return v && *v == '1';
  }();
  return ab && g_device && AbPhase() == 1;
}
bool CloudSkipNow() {
  static const bool ab = [] {
    const char* v = std::getenv("BF_CLOUD_AB");
    return v && *v == '1';
  }();
  if (CloudOld()) return false;
  return !ab || !g_device || AbPhase() == 0;
}
// The fog (sky.h's Fog), with either sky: whether the world now has any (ExecuteDraw keeps it), the camera and the
// settings of this frame's view kept from the end of its opaque draws, and the game's sky dome seen being drawn
// (the fog is laid right after it: the game's water is drawn by then, its smoke and fire are not yet).
bool g_fog_wanted = false;
Sky::Inputs g_fog_in;
bool g_fog_ready = false, g_fog_dome = false;
bool g_lens_due = false;
// A view that shows no terrain (the camera looks up: "Sky box does this when the camera looks up to the sky ... it
// glitches out", 14 Nov 2026: the game's own sky is not drawn with the renderer's on, and without the terrain's
// camera the renderer's was not either, so the sky was the bare background). The sky is then drawn late, once the
// game's sky dome's draws have come by: the view's direction from the dome's matrix, the rest (the eye's place, the
// view's size) as the last view with terrain had it.
Sky::Inputs g_last_sky_in;
bool g_last_sky_valid = false, g_sky_late = false;
void ApplyLateSky();  // the fog ran in this frame's view: the rain on the lens may follow once
void TrackSkyEye(float eye[3], bool fresh);
// The view's camera for ListenerView (the weather's sounds): its eye, and its matrix's rows to the picture's x and to
// its depth (`a` rows 0 and 2: the right and the forward in the world, scaled).
void NoteListener(const float eye[3], const double a[3][3]) {
  const float right[3] = {float(a[0][0]), float(a[0][1]), float(a[0][2])};
  const float forward[3] = {float(a[2][0]), float(a[2][1]), float(a[2][2])};
  NoteListenerView(eye, right, forward);
}
bool FogOn() { return g_fog_wanted && SkyLoaded(); }
void ApplyFog();
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
bool g_msaa_trace = false;  // one frame's copies and draws logged (now and then)
uint32_t g_msaa_trace_ms = 0, g_msaa_trace_single = 0;  // draws' views since the last logged copy
uint64_t g_msaa_downs = 0, g_msaa_ups = 0;  // host MSAA: the copies between a target's twin and its one-sample texture
HostTarget* g_scene_color = nullptr;
HostTarget* g_scene_depth = nullptr;
GuestSurface g_scene_color_surface;
// The reduced targets drawn or cleared in this view (D49), with their surfaces.
std::vector<std::pair<GuestSurface, HostTarget*>> g_reduced_used;
std::filesystem::path g_cache_root;  // the runtime's cache directory (D42)
std::unique_ptr<DrawPath> g_draw;  // created on the first draw (it needs the tracker's register file)
// Where the sky's sun stands at noon, degrees around the world's up axis: the setting, or under 0 from the game's own
// sun (the direction its shaders get, DrawPath::GameLightDir): the afternoon hour at which the sky's sun is as high
// as the game's (Wake Island's is 35 degrees up: 16:00), and the azimuth that puts it there at that hour, so that
// at that hour the sky, the light and the game's own shadows agree.
float SkyNoonAzimuth() {
  const float set = float(REXCVAR_GET(native_x_renderer_sun_azimuth));
  if (set >= 0.0f) return set;
  float dir[3];
  if (g_draw && g_draw->GameLightDir(dir)) {
    const double len = std::sqrt(double(dir[0]) * dir[0] + double(dir[1]) * dir[1] + double(dir[2]) * dir[2]);
    if (len > 1e-3 && dir[1] > 0.0f) {
      const double pi = 3.14159265358979;
      const double up = std::min(std::asin(dir[1] / len) * 180.0 / pi, 70.0);
      const double hour = 6.0 + 12.0 * (1.0 - std::asin(up / 70.0) / pi);
      const double az = std::atan2(double(dir[0]), double(dir[2])) * 180.0 / pi + 15.0 * (hour - 12.0);
      static bool said = false;
      if (!said) {
        said = true;
        REXLOG_INFO("[native_x_renderer] sky: the game's sun is {:.1f} degrees up: the sky's hour for it {:.2f}, its "
                    "noon azimuth {:.1f}", up, hour, std::fmod(az + 720.0, 360.0));
      }
      return float(std::fmod(az + 720.0, 360.0));
    }
  }
  return 135.0f;
}
// The game's own hour: where its sun stands (16:00 until that is known).
double SkyGameHour() {
  float dir[3];
  if (g_draw && g_draw->GameLightDir(dir)) {
    const double len = std::sqrt(double(dir[0]) * dir[0] + double(dir[1]) * dir[1] + double(dir[2]) * dir[2]);
    if (len > 1e-3 && dir[1] > 0.0f) {
      const double pi = 3.14159265358979;
      return 6.0 + 12.0 * (1.0 - std::asin(std::min(std::asin(dir[1] / len) * 180.0 / pi, 70.0) / 70.0) / pi);
    }
  }
  return 16.0;
}
// The world now (the public WorldState with its clocks run on to this moment): a match's server's (SetWorldState),
// or the player's own settings. Without the day and night the hour is the game's own, so that a sky of the
// renderer's own agrees with the game's light.
std::mutex g_world_mutex;
WorldState g_world_set;
std::chrono::steady_clock::time_point g_world_set_at{};
// D221: a new round or level (SnapWorld): until this moment (the steady clock's ticks) the world is taken as set, not
// eased; and a count of the jumps so taken, by which the sky dries what the last round's rain left (Sky::Inputs).
std::atomic<int64_t> g_world_snap_until{0};
std::atomic<uint32_t> g_world_snaps{0};
WorldState World() {
  static const auto start = std::chrono::steady_clock::now();
  static auto last = start;
  static double own_evolve = 0.0, own_hours = 0.0;
  const auto now = std::chrono::steady_clock::now();
  const double dt = std::chrono::duration<double>(now - last).count();
  last = now;
  WorldState w;
  std::chrono::steady_clock::time_point at;
  {
    std::lock_guard lock(g_world_mutex);
    w = g_world_set, at = g_world_set_at;
  }
  if (w.valid && REXCVAR_GET(native_x_renderer_world_own)) w = WorldState{};
  if (w.valid) {
    const double since = std::chrono::duration<double>(now - at).count();
    if (w.day_night && w.day_minutes > 0.0) w.hour += since / (w.day_minutes * 60.0) * 24.0;
    w.cloud_seconds += since;
    w.cloud_evolve += since * w.cloud_rate;
    // The clocks drawn with only ever run forward ("When the lightning strikes, it looks like it reverses the rain and
    // then restarts it", 14 Nov 2026): each answer of the server's sets its clocks anew, a little before or after
    // where this game had run them on to, and a step back played the rain backward and a flash again. They run at
    // up to a fifth faster or slower toward the server's; only when more than five seconds off are they set.
    static double shown_seconds = 0.0, shown_evolve = 0.0;
    static bool shown = false;
    if (!shown || std::abs(w.cloud_seconds - shown_seconds) > 5.0 || std::abs(w.cloud_evolve - shown_evolve) > 5.0) {
      shown = true, shown_seconds = w.cloud_seconds, shown_evolve = w.cloud_evolve;
    } else {
      shown_seconds += dt * (1.0 + std::clamp((w.cloud_seconds - shown_seconds) * 0.5, -0.2, 0.2));
      shown_evolve += dt * w.cloud_rate * storm::RateFactor(w.weather) *  // (D213: the host's rate under the storm)
                      (1.0 + std::clamp((w.cloud_evolve - shown_evolve) * 0.5, -0.2, 0.2));
    }
    w.cloud_seconds = shown_seconds, w.cloud_evolve = shown_evolve;
  } else {
    // No server's world: the player's own choice of day and night, and the world as it is by itself (16:00, the
    // time standing still, fair weather). The weather and the clock are the server's to set ("take them out of the
    // graphics settings. Players use what the server sets", 15 Nov 2026): their settings are read only with
    // native_x_renderer_world_own (the tests), so a value saved while they were in the menu does nothing now.
    w = WorldState{};
    w.day_night = REXCVAR_GET(native_x_renderer_day_night);
    const bool own = REXCVAR_GET(native_x_renderer_world_own);
    if (own) {
      w.day_minutes = REXCVAR_GET(native_x_renderer_day_length);
      w.cloud_cover = float(REXCVAR_GET(native_x_renderer_cloud_cover));
      w.cloud_rate = float(std::clamp(REXCVAR_GET(native_x_renderer_cloud_rate), 0.0, 4.0));
      w.weather = float(REXCVAR_GET(native_x_renderer_weather));
      w.fog = float(REXCVAR_GET(native_x_renderer_fog));
      w.ground_fog = float(REXCVAR_GET(native_x_renderer_ground_fog));
      w.ground_fog_height = float(REXCVAR_GET(native_x_renderer_ground_fog_height));  // D231
      w.ground_fog_sparse = float(REXCVAR_GET(native_x_renderer_ground_fog_sparse));
      w.wind = float(REXCVAR_GET(native_x_renderer_wind));
      w.cloud_height = float(REXCVAR_GET(native_x_renderer_cloud_height));
      w.storm_type = REXCVAR_GET(native_x_renderer_storm_type);  // D224
    }
    own_evolve += dt * w.cloud_rate * storm::RateFactor(w.weather);  // D213
    if (w.day_night && w.day_minutes > 0.0) own_hours += dt / (w.day_minutes * 60.0) * 24.0;
    w.hour = (own ? REXCVAR_GET(native_x_renderer_time_of_day) : 16.0) + own_hours;
    w.cloud_seconds = std::chrono::duration<double>(now - start).count();
    w.cloud_evolve = own_evolve;
  }
  if (!w.day_night) w.hour = SkyGameHour();
  w.hour = std::fmod(std::fmod(w.hour, 24.0) + 24.0, 24.0);
  // D211: the weather eased to what is set (the user, 19 Nov 2026: "The cloud weather change should happen smoothly,
  // not immediate and jumpy like it currently does"): each of the weather, the cloud cover, the fogs, the wind and the
  // cloud base moves toward its set value with a time constant of 8 s, the shares at most 4% a second (5% the wind,
  // 25 m a second the base): a whole change of the weather takes about 25 s. Taken at once on the first look, when the
  // world's source changes (joining, leaving) and after five seconds without a look. D221: and while a new round or
  // level is coming in (SnapWorld): the last round's storm is not eased away on the next one.
  {
    static std::mutex ease_mutex;
    static bool have = false, was_valid = false;
    static WorldState shown;
    static auto eased_at = now;
    std::lock_guard lock(ease_mutex);
    const double edt = std::chrono::duration<double>(now - eased_at).count();
    eased_at = now;
    const bool snap = now.time_since_epoch().count() < g_world_snap_until.load(std::memory_order_relaxed);
    if (snap && have && w.valid == was_valid &&
        (std::abs(shown.weather - w.weather) > 0.005f || std::abs(shown.cloud_cover - w.cloud_cover) > 0.005f ||
         std::abs(shown.fog - w.fog) > 0.005f || std::abs(shown.ground_fog - w.ground_fog) > 0.005f)) {
      g_world_snaps.fetch_add(1, std::memory_order_relaxed);
      REXLOG_INFO("[native_x_renderer] world (D221): a new round's taken at once: weather {:.0f}% to {:.0f}%, clouds "
                  "{:.0f}% to {:.0f}%, fog {:.0f}% to {:.0f}%",
                  shown.weather * 100.0f, w.weather * 100.0f, shown.cloud_cover * 100.0f, w.cloud_cover * 100.0f,
                  shown.fog * 100.0f, w.fog * 100.0f);
    }
    if (!have || w.valid != was_valid || edt > 5.0 || snap) {
      shown = w, have = true, was_valid = w.valid;
    } else {
      const float k = float(1.0 - std::exp(-std::max(edt, 0.0) / 8.0));
      auto ease = [&](float& s, float target, float most) {
        const float lim = most * float(edt);
        s += std::clamp((target - s) * k, -lim, lim);
      };
      ease(shown.weather, w.weather, 0.04f);
      ease(shown.cloud_cover, w.cloud_cover, 0.04f);
      ease(shown.fog, w.fog, 0.04f);
      ease(shown.ground_fog, w.ground_fog, 0.04f);
      ease(shown.ground_fog_height, w.ground_fog_height, 0.5f);  // D231
      ease(shown.ground_fog_sparse, w.ground_fog_sparse, 0.1f);
      ease(shown.wind, w.wind, 0.05f);
      ease(shown.cloud_height, w.cloud_height, 25.0f);
    }
    w.weather = shown.weather, w.cloud_cover = shown.cloud_cover, w.fog = shown.fog, w.ground_fog = shown.ground_fog;
    w.wind = shown.wind, w.cloud_height = shown.cloud_height, w.ground_fog_height = shown.ground_fog_height;
    w.ground_fog_sparse = shown.ground_fog_sparse;
  }
  // D208: the match's weather is the hurricane's stage (storm.h); the weather here is the storm's where the camera is
  // (the weather outside it while no camera is known yet). The lightning's strikes are told where they may fall.
  w.storm = w.weather;
  {
    const storm::Storm s = storm::Of(w.storm, w.cloud_seconds, g_map_x.load(std::memory_order_relaxed),
                                     g_map_z.load(std::memory_order_relaxed), g_map_reach.load(std::memory_order_relaxed),
                                     w.storm_type);
    float eye[3], right[3], forward[3];
    double age = 1e9;
    if (ListenerView(eye, right, forward, &age) && age < 5.0) {
      w.weather = storm::WeatherAt(s, eye[0], eye[2]);
      SetStrikeWorld(s, std::round(eye[0] / 4000.0f) * 4000.0f, std::round(eye[2] / 4000.0f) * 4000.0f);
      static std::atomic<int64_t> said{0};
      const int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
      if (s.on && now_s - said.load(std::memory_order_relaxed) >= 10) {
        said.store(now_s, std::memory_order_relaxed);
        REXLOG_INFO("[native_x_renderer] storm (D208, D224): the match's weather {:.0f}%, a {} centred at {:.0f}, {:.0f} "
                    "({:.0f} m from the camera; the level's middle {:.0f}, {:.0f}{}), its eye {:.0f} m, its rain shield {:.0f} m, "
                    "strength {:.2f}; the weather here {:.0f}%",
                    w.storm * 100.0f, storm::StormTypeName(s.type), s.cx, s.cz, std::hypot(eye[0] - s.cx, eye[2] - s.cz),
                    g_map_x.load(), g_map_z.load(),
                    g_map_known.load() ? "" : ", not known", s.eye, s.radius, s.gain, w.weather * 100.0f);
      }
    } else if (s.on) {
      w.weather = s.outside;
    }
  }
  return w;
}

// D229: the storm made of the sky's own clouds (the fluid weather plan's step 3; the user, 20 Nov 2026: "when a storm
// forms, it forms FROM the clouds, pulling them in"). A 2D FluidSim in the storm's own frame (its rain shield 1,000
// units, whatever its size in the world: it grows and moves without the sim starting again), stepped 30 times a second
// while there is a storm and the renderer's sky: the air pulled to the low, driven round it (anticlockwise seen from
// above: +x east, +z north, the game's noon sun on Guadalcanal at +z) and turned by Coriolis, with an updraft at the eye
// once it opens. Its flow map (FluidSim::Clouds) says where that air has carried the sky's clouds from, in two phases
// that start again by turns every `kFlowPeriod` seconds, each faded out before it does, so the clouds coming in are
// pulled round into the storm, their own shapes and cover, never stretched past a phase's span (sky.cpp reads the
// weather map and the shape noise there). Over that, the storm's convection: scattered cells blended by the
// organisation (rising with the stage) into the storm's pattern (Hurricane Michael's radar, D228, turned with its spin;
// its radius to a power so the radar's eye is the storm's and its outer bands fall inside the rain shield), carried by
// the air too. (Sky's storm_c, storm_cloud, storm_conv.) NXR_STORM_FLUID_LOG=1: the particles' state every 10 s.
// D229: with NXR_DEVICE_FAULT=1, the renderer's passes as labelled groups in the command buffers (VK_EXT_debug_utils),
// for AMD's Radeon GPU Detective's crash reports: which pass was in flight when the GPU faulted.
bool FaultLabels() {
  static const bool on = [] {
    const char* v = std::getenv("NXR_DEVICE_FAULT");
    return v && *v == '1';
  }();
  return on;
}
struct StormClouds {
  FluidSim sim;
  bool tried = false, seeded = false, reset = false;
  double owed = 0.0, turn = 0.0, cells = 0.0, pattern_clock = 0.0, flow_clock = 0.0;
  uint64_t steps = 0;
  std::chrono::steady_clock::time_point last{};
  float c[4] = {};
  bool on = false;
};
StormClouds g_storm_clouds;
// D232: the clouds' mass as particles (cloud_mass.h), a developer's first slice.
CloudMass g_cloud_mass;
void CloudMassInputs(Sky::Inputs& in) {
  in.cloud_mass = nullptr, in.cloud_mass_size = 0.0f;
  if (!REXCVAR_GET(native_x_renderer_cloud_particles) || !g_cloud_mass.Ready() || !g_cloud_mass.View()) return;
  in.cloud_mass = g_cloud_mass.View();
  in.cloud_mass_origin[0] = g_cloud_mass.origin()[0], in.cloud_mass_origin[1] = g_cloud_mass.origin()[1];
  in.cloud_mass_size = CloudMass::kSize;
}
void StepCloudMass(IDeviceContext* context);
void WakeInputs(Sky::Inputs& in);  // D234
ITexture* StormPatternTexture();
void StepStormClouds(IDeviceContext* context) {
  StormClouds& sc = g_storm_clouds;
  const WorldState w = World();
  const storm::Storm s = storm::Of(w.storm, w.cloud_seconds, g_map_x.load(std::memory_order_relaxed),
                                   g_map_z.load(std::memory_order_relaxed), g_map_reach.load(std::memory_order_relaxed),
                                   w.storm_type);
  sc.on = s.on && SkyOn() && REXCVAR_GET(native_x_renderer_fluid_storm);
  if (!sc.on) {
    sc.c[0] = 0.0f;
    sc.seeded = false;  // the next storm starts afresh
    return;
  }
  if (!sc.tried) {
    sc.tried = true;
    FluidSim::Config config;
    config.particles = 16384;
    config.tracers = 64;  // (none emitted: the cloud field is the storm's clouds)
    config.spacing = 18.0f;
    for (int k : {0, 2}) {
      config.domain_min[k] = config.splat_min[k] = -1400.0f;
      config.domain_max[k] = 1400.0f;
      config.splat_size[k] = 2800.0f;
    }
    config.splat_dims[0] = config.splat_dims[2] = 4;
    config.cloud_dims[0] = config.cloud_dims[1] = 256;
    config.grid_dims[0] = config.grid_dims[1] = 128;
    sc.sim.Initialize(g_device->device(), config, "storm");
  }
  if (!sc.sim.Ready()) {
    sc.c[0] = 0.0f;
    return;
  }
  // The sky's weather map and where it is (as the clouds were last drawn): none yet, nothing to carry.
  ITextureView* weather = nullptr;
  float drift[2] = {}, scale = 1.0f;
  if (!g_own_sky.CloudMap(&weather, drift, &scale)) {
    sc.c[0] = 0.0f;
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!sc.seeded) {
    const float centre[3] = {};
    sc.sim.Seed(context, centre, 0.0f);
    sc.seeded = true, sc.reset = true, sc.owed = 1.0 / 30.0, sc.last = now, sc.flow_clock = 0.0;
  }
  // At most three steps owed (as the ground fog's): after a hitch the storm drops the time rather than catching up
  // with up to 15 steps in one frame and making the hitch longer.
  sc.owed = std::min(sc.owed + std::chrono::duration<double>(now - sc.last).count(), 3.0 / 60.0);
  sc.last = now;
  const bool hurricane = s.type == storm::kHurricane;
  const float stage = storm::Smooth(0.25f, 1.0f, w.storm);
  const float eye_open = hurricane ? storm::Smooth(0.65f, 0.9f, stage) : 0.0f;
  FluidSim::Forces f;
  f.pull = 0.25f, f.reach = 1500.0f, f.drag = 0.1f;
  f.swirl = hurricane ? 0.9f * std::pow(stage, 1.5f) : 0.15f;  // (a terminal speed of swirl / drag at the edge, more within)
  f.coriolis = hurricane ? 0.002f * stage : 0.0005f;
  f.vorticity = 1.5f, f.viscosity = 0.03f;
  f.sink = hurricane ? 1000.0f * (s.eye / std::max(s.radius, 1.0f)) * 0.8f * eye_open : 0.0f, f.source = 1350.0f;
  f.tracer_life = 120.0f, f.splat_weight = 0.35f;
  f.pattern_radius = 1000.0f;
  static const float flow_speed = [] {  // (NXR_STORM_FLOW_SPEED: a developer's, for tuning)
    const char* v = std::getenv("NXR_STORM_FLOW_SPEED");
    return v && *v ? float(std::atof(v)) : 1.0f;
  }();
  f.flow_speed = flow_speed;
  // The weather map it carries: the domain's unit the shield's 1/1000 in the world, the sky's units the world's over
  // its scale (sky.cpp kCloudShared's weather_scale 40,000).
  sc.sim.SetCloudSource(weather);
  {  // D234: the storm's cloud grows out of the sky's own (their shape volume, their cover)
    ITextureView* shape = nullptr;
    float coverage = 0.5f, evolve = 0.0f;
    if (g_own_sky.CloudShape(&shape, &coverage, &evolve)) {
      sc.sim.SetCloudShape(shape);
      f.cloud_coverage = coverage, f.shape_y = evolve;
    } else {
      sc.sim.SetCloudShape(nullptr);
    }
    f.eye = 1000.0f * s.eye / std::max(s.radius, 1.0f), f.eye_open = eye_open;
  }
  const float sky_scale = std::max(scale, 1e-3f);
  f.cloud_origin[0] = s.cx / sky_scale, f.cloud_origin[1] = s.cz / sky_scale;
  f.cloud_scale = s.radius / 1000.0f / sky_scale;
  f.cloud_drift[0] = drift[0], f.cloud_drift[1] = drift[1];
  f.weather_scale = 40000.0f;
  f.convection = hurricane ? 0.4f + 0.6f * stage : 0.5f + 0.4f * stage;
  f.convection_rate = 1.0f / 12.0f;  // (D234: how fast the clouds grow into the storm's)
  f.organisation = hurricane ? storm::Smooth(0.3f, 0.85f, stage) : 0.12f;
  // The pattern's eye (its radius in its texture, 8.4 of 128 pixels) where the storm's eye is, its edge at the rain
  // shield's: the radius to the power that takes one to the other.
  f.pattern_zoom = hurricane ? 1.0f : 0.5f;
  {
    const float eye = std::clamp(s.eye / std::max(s.radius, 1.0f), 0.05f, 0.5f);
    f.pattern_power = hurricane ? std::clamp(std::log(8.4f / 128.0f) / std::log(eye), 1.0f, 3.0f) : 1.0f;
  }
  constexpr float kStep = 1.0f / 60.0f;  // (60 Hz: the user, 21 Nov 2026: "The simulation should be stepped at 60 Hz")
  ITexture* pattern = StormPatternTexture();
  sc.sim.SetPattern(pattern ? pattern->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr);
  const uint32_t frames = pattern ? pattern->GetDesc().ArraySize : 1;
  if (FaultLabels()) context->BeginDebugGroup("NativeXRenderer storm fluid"), g_device->FaultMarker(context, 60, true, "storm fluid");
  while (sc.owed >= kStep) {
    sc.owed -= kStep;
    // The pattern turns with the storm (a turn in about 4 minutes at full strength) and its frames play there and back,
    // ten times as fast as the radar's (its 55 minutes in 5.5).
    sc.turn += kStep * (hurricane ? 0.026 * stage : 0.004);
    sc.cells += kStep;
    sc.pattern_clock += kStep * 10.0 / (5.0 * 60.0);
    const double span = std::max(double(frames) - 1.0, 1.0);
    const double pp = std::fmod(sc.pattern_clock, 2.0 * span);
    const double at = pp <= span ? pp : 2.0 * span - pp;
    f.pattern_layer_a = float(std::floor(at));
    f.pattern_layer_b = float(std::min(std::floor(at) + 1.0, span));
    f.pattern_blend = float(at - std::floor(at));
    f.pattern_turn = -float(std::fmod(sc.turn, 6.283185307));
    f.cells_time = float(std::fmod(sc.cells, 10000.0));
    f.cloud_reset = sc.reset;
    sc.reset = false;
    // The flow map's phases: a starts again as its clock turns over, b half a period on.
    constexpr double kFlowPeriod = 90.0;
    const double before = sc.flow_clock / kFlowPeriod;
    sc.flow_clock += kStep;
    const double after = sc.flow_clock / kFlowPeriod;
    f.restart_a = std::floor(after) != std::floor(before);
    f.restart_b = std::floor(after + 0.5) != std::floor(before + 0.5);
    f.share_b = float(std::abs(2.0 * std::fmod(after, 1.0) - 1.0));
    sc.sim.Step(context, kStep, f, nullptr, 0, 0);
    static const bool log_state = [] {
      const char* v = std::getenv("NXR_STORM_FLUID_LOG");
      return v && *v == '1';
    }();
    if (log_state && ++sc.steps % 300 == 0) sc.sim.LogState(context);
  }
  if (FaultLabels()) g_device->FaultMarker(context, 60, false, "storm fluid"), context->EndDebugGroup();
  // Phase a's share: none as it starts again, all half a period on.
  const double phase = std::fmod(sc.flow_clock / 90.0, 1.0);
  const float share_a = float(1.0 - std::abs(2.0 * phase - 1.0));
  sc.c[0] = 1.0f, sc.c[1] = share_a, sc.c[2] = 2.8f, sc.c[3] = eye_open;
}

// D208: the sky's view of the hurricane: the weather outside it and its two vectors for the clouds (storm::kGlsl).
void StormInputs(const WorldState& world, Sky::Inputs& in) {
  const storm::Storm s = storm::Of(world.storm, world.cloud_seconds, g_map_x.load(std::memory_order_relaxed),
                                   g_map_z.load(std::memory_order_relaxed), g_map_reach.load(std::memory_order_relaxed),
                                   world.storm_type);
  in.match_weather = world.storm;
  in.world_snaps = g_world_snaps.load(std::memory_order_relaxed);  // D221
  in.weather_outside = s.outside;
  in.storm_a[0] = s.cx, in.storm_a[1] = s.cz, in.storm_a[2] = s.eye, in.storm_a[3] = s.radius;
  in.storm_b[0] = s.gain, in.storm_b[1] = s.spin, in.storm_b[2] = s.on ? 1.0f + float(s.type) : 0.0f;  // (D224: its type)
  // D229: the storm's clouds from the fluid.
  for (int k = 0; k < 4; ++k) in.storm_c[k] = g_storm_clouds.on ? g_storm_clouds.c[k] : 0.0f;
  const bool fluid = g_storm_clouds.on && g_storm_clouds.c[0] > 0.5f && g_storm_clouds.sim.Ready();
  in.storm_cloud = fluid ? g_storm_clouds.sim.Clouds() : nullptr;
  in.storm_conv = fluid ? g_storm_clouds.sim.Convection() : nullptr;
  // D211: the volutus (the roll cloud along the storm's gust front) while the storm comes in, gone once it is over the
  // level.
  in.storm_b[3] = storm::Smooth(0.33f, 0.5f, world.storm) * (1.0f - storm::Smooth(0.85f, 0.97f, world.storm));
}

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

// The A/B test switches' phase (0 or 1): by turns of 600 frames, or with BF_AB_EVERY_FRAME=1 every other frame, so
// that both ways meet the same view, the same moment of the storm and the same load (600 frame windows lined up with
// the camera tour's spots and put the two ways in different places: run perf_fair_*, 20 Nov 2026). The GPU sections
// then keep each phase's time apart.

// The GPU time of the renderer's own passes, each by name (native_x_renderer_gpu_sections, off by default: a few
// duration queries a frame): a ring of 4 queries a section, read when a query comes round again (4 uses on), so
// nothing waits for the GPU. (16 a section, with the sky's and the AO's parts, took the Vulkan pool's 256 duration
// queries and the next allocation failed: a crash, run perf_cbU_ab.) Logged every 600 frames as milliseconds a frame, to tell what a frame's GPU time is
// spent on (the Night Ops performance pass, 20 Nov 2026).
class SectionTimers {
 public:
  enum Id { kSky, kCloudsOver, kFog, kLens, kWet, kGrass, kGtao, kSsgi, kAa, kUpscale, kMotionBlur, kDof,
            kSkyMarch, kSkyResolve, kSkyDraw, kGiDepth, kAoDepth, kGiGather, kGiBlur, kAoMain, kGiCopy, kGiComposite, kAoRest, kSkin, kSkinBloom, kCount };
  static constexpr const char* kNames[kCount] = {"sky",  "clouds over", "fog and rain", "lens", "wet copies", "grass",
                                                 "GTAO", "SSGI",        "AA",           "upscaler", "motion blur",
                                                 "DOF",  "(sky: the clouds' march", "the clouds' blend", "the sky drawn)",
                                                 "(SSGI's depth and normals)", "(GTAO's depth and normals)",
                                                 "(SSGI's light and gather)", "(SSGI's history and blur)",
                                                 "(GTAO's occlusion and blur)", "(SSGI's colour copy)",
                                                 "(SSGI's composite)", "(GTAO's history and apply)",
                                                 "weapon skin", "weapon skin bloom"};
  void Begin(IRenderDevice* device, IDeviceContext* context, Id id) {
    if (FaultLabels() && context && !labelled_[id]) {
      context->BeginDebugGroup(kNames[id]);
      if (g_device) g_device->FaultMarker(context, int(id), true, kNames[id]);
      labelled_[id] = true;
    }
    if (!REXCVAR_GET(native_x_renderer_gpu_sections) || !device || !context) return;
    Section& sec = sections_[id];
    if (!sec.tried) {
      sec.tried = true;
      if (device->GetDeviceInfo().Features.DurationQueries != DEVICE_FEATURE_STATE_ENABLED) return;
      for (auto& q : sec.queries) {
        QueryDesc d;
        d.Name = "NativeXRenderer section time";
        d.Type = QUERY_TYPE_DURATION;
        device->CreateQuery(d, &q);
        if (!q) return;
      }
      sec.on = true;
    }
    if (!sec.on || sec.active) return;
    if (sec.pending[sec.index]) {
      QueryDataDuration data;
      if (sec.queries[sec.index]->GetData(&data, sizeof(data), true) && data.Frequency) {
        const int ph = sec.phase[sec.index];
        sec.total_ms += double(data.Duration) * 1000.0 / double(data.Frequency), ++sec.uses;
        sec.phase_ms[ph] += double(data.Duration) * 1000.0 / double(data.Frequency), ++sec.phase_uses[ph];
      }
      sec.pending[sec.index] = false;
    }
    sec.phase[sec.index] = int(AbPhase() & 1);
    context->BeginQuery(sec.queries[sec.index]);
    sec.active = true;
  }
  void End(IDeviceContext* context, Id id) {
    if (labelled_[id] && context) {
      if (g_device) g_device->FaultMarker(context, int(id), false, kNames[id]);
      context->EndDebugGroup();
      labelled_[id] = false;
    }
    Section& sec = sections_[id];
    if (!sec.active) return;
    context->EndQuery(sec.queries[sec.index]);
    sec.pending[sec.index] = true;
    sec.index = (sec.index + 1) % kQueries;
    sec.active = false;
  }
  // "sky 1.20 (600), fog and rain 3.40 (600)": each timed section's milliseconds a frame over `frames`, and its uses.
  std::string Take(uint64_t frames) {
    std::string out;
    for (int i = 0; i < kCount; ++i) {
      Section& sec = sections_[i];
      if (!sec.uses) continue;
      out += fmt::format("{}{} {:.3f} ({})", out.empty() ? "" : ", ", kNames[i],
                         frames ? sec.total_ms / double(frames) : 0.0, sec.uses);
      // With BF_AB_EVERY_FRAME: each phase's milliseconds a use (A: phase 0, B: phase 1).
      if (AbEveryFrame())
        out += fmt::format(" [A {:.3f} B {:.3f}]", sec.phase_uses[0] ? sec.phase_ms[0] / double(sec.phase_uses[0]) : 0.0,
                           sec.phase_uses[1] ? sec.phase_ms[1] / double(sec.phase_uses[1]) : 0.0);
      sec.total_ms = 0.0, sec.uses = 0;
      sec.phase_ms[0] = sec.phase_ms[1] = 0.0, sec.phase_uses[0] = sec.phase_uses[1] = 0;
    }
    return out;
  }

 private:
  static constexpr uint32_t kQueries = 4;
  struct Section {
    RefCntAutoPtr<IQuery> queries[kQueries];
    bool pending[kQueries] = {};
    uint32_t index = 0;
    bool tried = false, on = false, active = false;
    double total_ms = 0.0;
    uint64_t uses = 0;
    int phase[kQueries] = {};  // each query's A/B phase (AbPhase)
    double phase_ms[2] = {};
    uint64_t phase_uses[2] = {};
  };
  Section sections_[kCount];
  bool labelled_[kCount] = {};
} g_sections;
// Parts of passes timed from their own files (SkySectionBegin / End: 0 the clouds' march, 1 their blend, 2 the sky
// drawn; 3 SSGI's depth and normals, 4 GTAO's; 5 SSGI's light and gather, 6 its history and blur; 7 GTAO's occlusion
// and blur; 8 SSGI's colour copy, 9 its composite; 10 GTAO's history and apply).
}  // namespace
// BF_AO_DISCARD_AB=1 (tests): GTAO's apply and SSGI's composite write every pixel (the old way) or leave the ones they
// do not change (the new), by turns.
bool AoDiscardOldNow() {
  static const bool ab = [] {
    const char* v = std::getenv("BF_AO_DISCARD_AB");
    return v && *v == '1';
  }();
  return ab && g_device && AbPhase() == 1;
}
void SkySectionBegin(int part) {
  if (g_device && part >= 0 && part < 11)
    g_sections.Begin(g_device->device(), g_device->context(), SectionTimers::Id(SectionTimers::kSkyMarch + part));
}
void SkySectionEnd(int part) {
  if (g_device && part >= 0 && part < 11) g_sections.End(g_device->context(), SectionTimers::Id(SectionTimers::kSkyMarch + part));
}
namespace {
// A GPU section around `work` (a call returning what it returns).
template <typename F>
auto Timed(SectionTimers::Id id, F&& work) {
  g_sections.Begin(g_device ? g_device->device() : nullptr, g_device ? g_device->context() : nullptr, id);
  auto result = work();
  if (g_device) g_sections.End(g_device->context(), id);
  return result;
}

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

#if defined(__x86_64__) || defined(_M_X64)
uint64_t Ticks() { return __rdtsc(); }
double TicksMs(uint64_t ticks) {
  static const double ms_per_tick = [] {
    const auto start = Pacing::Clock::now();
    const uint64_t first = __rdtsc();
    while (Pacing::Clock::now() - start < std::chrono::milliseconds(5)) {
    }
    return Pacing::Ms(Pacing::Clock::now() - start) / double(__rdtsc() - first);
  }();
  return double(ticks) * ms_per_tick;
}
#else
uint64_t Ticks() { return uint64_t(Pacing::Clock::now().time_since_epoch().count()); }
double TicksMs(uint64_t ticks) { return Pacing::Ms(Pacing::Clock::duration(ticks)); }
#endif

class RendererTime {
 public:
  explicit RendererTime(double* phase = nullptr) : start_(Ticks()), phase_(phase) {}
  ~RendererTime() {
    const double ms = TicksMs(Ticks() - start_);
    g_pacing.renderer_ms += ms;
    if (phase_) *phase_ += ms;
  }

 private:
  uint64_t start_;
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

}  // namespace
}  // namespace native_x_renderer::renderer
namespace native_x_renderer {
void DumpNextDraws(int count);  // draw.cpp
bool ObjectMotionBlurPaused();  // native_x_renderer.cpp: SetObjectMotionBlurPaused within a quarter second
}
namespace native_x_renderer::renderer {
namespace {
// Host MSAA (13 Nov 2026, "Let's build MSAA 2x and 4x"; targets.h's HostTarget). The game draws without multisampling
// and D31/D60 left the console's out; this is the renderer's own, for geometry thinner than a pixel (a distant mast,
// power lines), which no antialiasing of the finished picture can put back. The targets of the output's size get a
// multisampled twin; the draws go into the twins (DrawPath, through HostMsaaDrawView), and whatever else reads or
// writes a target takes its one-sample texture, after MsaaRead or MsaaWrite here. Not with a reduced view (D49).
uint32_t MsaaSetting() {
  const int32_t v = REXCVAR_GET(native_x_renderer_msaa);
  return g_reduced ? 1 : v >= 4 ? 4 : v >= 2 ? 2 : 1;
}

// The one-sample texture brought up to date from the twin: the colour's samples averaged, the depth's first taken.
void MsaaDown(HostTarget* t) {
  if (!t || !t->msaa_view || !t->single_stale) return;
  t->single_stale = false;
  const Blitter::Rect all{0, 0, t->width, t->height};
  Blitter::Options options;
  options.samples = t->msaa_samples;
  g_blit.Copy(g_device->context(), t->msaa_srv,
              t->depth ? Blitter::Source::kDepthMsaaToDepth : Blitter::Source::kColorMsaa, all, t->view, all, options);
  ++g_msaa_downs;
  if (g_msaa_trace) {
    REXLOG_INFO("[native_x_renderer] MSAA trace: down, {} format {} (after {} multisampled views, {} one-sample)",
                t->depth ? "depth" : "color", int(t->format), g_msaa_trace_ms, g_msaa_trace_single);
    g_msaa_trace_ms = g_msaa_trace_single = 0;
  }
  if (g_draw) g_draw->TargetsChanged();
}

// The twin brought up to date from the one-sample texture: every sample of a pixel alike.
void MsaaUp(HostTarget* t) {
  if (!t || !t->msaa_view || !t->msaa_stale) return;
  t->msaa_stale = false;
  const Blitter::Rect all{0, 0, t->width, t->height};
  if (t->msaa_delta && t->msaa_before) {
    // The change made to the one-sample texture, added to each sample (HostTarget::msaa_before).
    Blitter::Options options;
    options.before = t->msaa_before->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    g_blit.Copy(g_device->context(), t->srv, Blitter::Source::kColorDelta, all, t->msaa_view, all, options);
  } else {
    g_blit.Copy(g_device->context(), t->srv, t->depth ? Blitter::Source::kDepthToDepth : Blitter::Source::kColor, all,
                t->msaa_view, all);
  }
  t->msaa_delta = false;
  ++g_msaa_ups;
  if (g_msaa_trace) {
    REXLOG_INFO("[native_x_renderer] MSAA trace: up, {} format {} (after {} multisampled views, {} one-sample)",
                t->depth ? "depth" : "color", int(t->format), g_msaa_trace_ms, g_msaa_trace_single);
    g_msaa_trace_ms = g_msaa_trace_single = 0;
  }
  if (t->depth) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("[native_x_renderer] MSAA: a depth target's multisampled twin was filled from its one-sample "
                  "texture (the stencil is not carried)");
    }
  }
  if (g_draw) g_draw->TargetsChanged();
}

// Before the one-sample texture is read, and before it is written.
void MsaaRead(HostTarget* t, int line = __builtin_LINE()) {
  if (g_msaa_trace && t && t->msaa_view && t->single_stale) REXLOG_INFO("[native_x_renderer] MSAA trace: read of {} {} from line {}", t->depth ? "depth" : "color", fmt::ptr(t), line);
  MsaaDown(t);
}
void MsaaWrite(HostTarget* t, int line = __builtin_LINE()) {
  if (!t || !t->msaa_view) return;
  if (g_msaa_trace)
    REXLOG_INFO("[native_x_renderer] MSAA trace: write of {} {} from line {}", t->depth ? "depth" : "color", fmt::ptr(t), line);
  MsaaDown(t);
  // The first write since the twin and the texture were alike, of a float colour target: the texture kept as it is,
  // for the twin to take the change alone (HostTarget::msaa_before).
  if (!t->msaa_stale && !t->depth &&
      GetTextureFormatAttribs(t->format).ComponentType == COMPONENT_TYPE_FLOAT) {
    if (!t->msaa_before) {
      TextureDesc desc = t->texture->GetDesc();
      desc.Name = "NativeXRenderer target before a write (MSAA)";
      desc.BindFlags = BIND_SHADER_RESOURCE;
      g_device->device()->CreateTexture(desc, nullptr, &t->msaa_before);
    }
    if (t->msaa_before) {
      CopyTextureAttribs copy(t->texture, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, t->msaa_before,
                              RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      g_device->context()->CopyTexture(copy);
      t->msaa_delta = true;
      if (g_draw) g_draw->TargetsChanged();
    }
  }
  t->msaa_stale = true;
}

// A target's twin made, remade or dropped as the setting asks.
void EnsureMsaa(HostTarget* t) {
  if (!t || !t->texture) return;
  const bool eligible = !t->reduced && !t->shadow && t->samples == 1 && t->guest_width == g_profile->output_width &&
                        t->guest_height == g_profile->output_height;
  const uint32_t want = eligible ? MsaaSetting() : 1;
  if (want == t->msaa_samples || (want == 1 && !t->msaa_samples)) return;
  if (t->msaa_view) {
    MsaaDown(t);
    t->msaa_texture.Release();
    t->msaa_before.Release();
    t->msaa_view = t->msaa_srv = nullptr;
    if (g_draw) g_draw->TargetsChanged();
  }
  t->msaa_samples = want;
  t->msaa_stale = t->single_stale = t->msaa_delta = false;
  if (want <= 1) return;
  IRenderDevice* device = g_device->device();
  if (!(device->GetTextureFormatInfoExt(t->format).SampleCounts & SAMPLE_COUNT(want))) {
    REXLOG_WARN("[native_x_renderer] MSAA: {} samples are not supported for a {} target's format ({}); drawn without",
                want, t->depth ? "depth" : "color", int(t->format));
    return;
  }
  TextureDesc desc = t->texture->GetDesc();
  const std::string name = std::string(desc.Name ? desc.Name : "NativeXRenderer target") + " MSAA";
  desc.Name = name.c_str();
  desc.SampleCount = want;
  desc.MipLevels = 1;
  device->CreateTexture(desc, nullptr, &t->msaa_texture);
  if (!t->msaa_texture) {
    REXLOG_ERROR("[native_x_renderer] MSAA: could not create {}", name);
    return;
  }
  t->msaa_view = t->msaa_texture->GetDefaultView(t->depth ? TEXTURE_VIEW_DEPTH_STENCIL : TEXTURE_VIEW_RENDER_TARGET);
  t->msaa_srv = t->msaa_texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  if (!t->msaa_view || !t->msaa_srv) {
    t->msaa_texture.Release();
    t->msaa_view = t->msaa_srv = nullptr;
    return;
  }
  t->msaa_stale = true;  // what the target holds is in its one-sample texture
  REXLOG_INFO("[native_x_renderer] MSAA: {} ({}x{}, {} samples)", name, desc.Width, desc.Height, want);
}

ITextureView* MsaaDrawView(HostTarget* t, bool multisampled, bool writes) {
  if (!t->msaa_view) return t->view;
  ++(multisampled ? g_msaa_trace_ms : g_msaa_trace_single);
  if (multisampled) {
    MsaaUp(t);
    if (writes) t->single_stale = true;
    return t->msaa_view;
  }
  MsaaDown(t);
  if (writes) t->msaa_stale = true;
  if (g_msaa_trace) REXLOG_INFO("[native_x_renderer] MSAA trace: a one-sample draw into {} {}", t->depth ? "depth" : "color", fmt::ptr(t));
  return t->view;
}

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
  EnsureMsaa(target);
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
    MsaaWrite(full);
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
  MsaaRead(color);
  MsaaWrite(depth);
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
    draw->SetSoftShadowFetchConstant(g_profile->soft_shadow_fetch_constant);  // D197
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
  // The first person weapon's wetness (DrawPath::WetRequest): how wet it is now, and the wet copies of its textures
  // that the last draws asked for, made here between draws.
  {
    static const int tint = [] {  // NXR_WET_TINT=<the pixel shader's texture number>: a developer's check
      const char* v = std::getenv("NXR_WET_TINT");
      return v && *v ? std::atoi(v) : -1;
    }();
    const bool on = (REXCVAR_GET(native_x_renderer_wetness) || tint >= 0) && g_sky_state == 1;
    g_own_sky.whole_colour_ = tint == 98;
    static const auto wet_start = std::chrono::steady_clock::now();
    g_draw->SetWeaponWet(on ? g_own_sky.weapon_wet_count() : 0.0f, on ? g_own_sky.weapon_wet_show() : 0.0f, on ? tint : -1,
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - wet_start).count());
    static std::vector<DrawPath::WetRequest> wanted;
    if (on && g_draw->TakeWetRequests(wanted)) {
      RendererTime time(&g_phase.effects);
      for (DrawPath::WetRequest& w : wanted) {
        RefCntAutoPtr<ITextureView> copy = w.copy;
        if (!copy) {
          TextureDesc d = w.original->GetTexture()->GetDesc();
          d.Name = "NativeXRenderer wet copy";
          // (Over the player's weapon skin, a float copy as the skin's is: its arcs keep their glow under the drops.)
          d.Format = d.Format == TEX_FORMAT_RGBA16_FLOAT ? TEX_FORMAT_RGBA16_FLOAT : TEX_FORMAT_RGBA8_UNORM;
          if (w.vehicle) d.Width *= 2, d.Height *= 2;  // D217: raindrop-sized drops on a vehicle's texture
          d.MipLevels = 0;  // all of them
          d.Usage = USAGE_DEFAULT;
          d.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
          d.MiscFlags = MISC_TEXTURE_FLAG_GENERATE_MIPS;
          d.CPUAccessFlags = CPU_ACCESS_NONE;
          RefCntAutoPtr<ITexture> texture;
          g_device->device()->CreateTexture(d, nullptr, &texture);
          if (!texture) continue;
          copy = texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
          static bool said = false;
          if (!said) {
            said = true;
            REXLOG_INFO("[native_x_renderer] wetness: the first wet copy, {}x{} (wet {:.2f}, showing {:.2f})", d.Width,
                        d.Height, g_draw->wet_count(), g_draw->wet_show());
          }
        }
        // Read as a plain 2D texture (the game's are arrays of one layer).
        TextureViewDesc plain;
        plain.ViewType = TEXTURE_VIEW_SHADER_RESOURCE;
        plain.TextureDim = RESOURCE_DIM_TEX_2D;
        RefCntAutoPtr<ITextureView> from;
        w.original->GetTexture()->CreateView(plain, &from);
        if (from && Timed(SectionTimers::kWet, [&] {
              return g_own_sky.WetCopy(g_device->context(), from, copy->GetTexture(), g_draw->wet_count(),
                                       g_draw->wet_show(), g_draw->wet_tint() >= 0, g_draw->wet_seconds(), w.skin,
                                       w.vehicle, w.relief_only);
            })) {
          g_draw->SetWetCopy(w.original, copy);
        } else {
          static bool said_failed = false;
          if (!said_failed) {
            said_failed = true;
            REXLOG_INFO("[native_x_renderer] wetness: a wet copy could not be made ({})", from ? "the pass" : "no view");
          }
        }
      }
      g_draw->TargetsChanged();
    }
  }
  // The player's own weapon skin (weapon_skin.h): its copies made between draws, as the wet copies are.
  // (Once a frame, at its first draw: the gun's draws that frame use the copies made then.)
  static uint64_t skin_frame = ~0ull;
  if (skin_frame != g_device->frames_presented() && WeaponSkinNow()) {
    skin_frame = g_device->frames_presented();
    bool drew = false;
    Timed(SectionTimers::kSkin, [&] {
      drew = WeaponSkinMake(g_device->device(), g_device->context(), g_device->frames_presented(),
                            g_draw->wet_count() * g_draw->wet_show());
      return drew;
    });
    if (drew) g_draw->TargetsChanged();
  }
  // The fog, before the first draw that follows the game's sky dome's (Battlefield 1943: 8ABF56776060E64C, six draws
  // after the water's; with a sky of the renderer's own they are not drawn, but they still come by here).
  if ((g_fog_ready || g_sky_late) && c.programs[1]) {
    if (g_draw->PixelShaderHash(*c.programs[1]) == 0x8ABF56776060E64Cull) {
      g_fog_dome = true;
    } else if (g_fog_dome) {
      if (g_sky_late) ApplyLateSky();
      else ApplyFog();
    }
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
  const bool hide_hud = REXCVAR_GET(native_x_renderer_hide_hud);  // the phases are told apart for it too
  if (g_profile->box_2d_after_composite && (g_aspect_factor != 1.0 || hud_scaled || hide_hud) && targets[0] &&
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
  // (D204: the sea's debug views, 7 and up, keep the frame as played.)
  g_draw->SetSkipGameSky(SkyOn() && (REXCVAR_GET(native_x_renderer_sky_debug) == 0 || REXCVAR_GET(native_x_renderer_sky_debug) >= 7));
  {
    // The game's own daylight by the sky's hour (sky.h's SkyLightAt, DrawPath::SetOutdoorLight).
    DrawPath::OutdoorLight light;
    const WorldState world = World();
    const int sky_debug = REXCVAR_GET(native_x_renderer_sky_debug);
    SetSkyLightning(REXCVAR_GET(native_x_renderer_lightning));
    SetCloudShadows(REXCVAR_GET(native_x_renderer_cloud_shadows));  // D214
    SetStormSway(REXCVAR_GET(native_x_renderer_storm_sway));
    SetSeaReflection(REXCVAR_GET(native_x_renderer_reflections) && SkyOn());
    g_draw->SetOwnSeaReflection(SeaReflection());
    if (float z[3], h[3]; SeaReflection() && g_own_sky.SeaSkyColours(z, h)) g_draw->SetSeaSky(z, h);
    if (float sea; g_own_sky.SeaLevel(&sea)) g_draw->SetSeaLevel(sea);  // D205
    // D191 (experimental, off: not yet seen working in a match): the window glass's own reflection.
    g_draw->SetOwnGlassReflection(SeaReflection() && REXCVAR_GET(native_x_renderer_glass_reflections));
    g_fog_wanted = world.fog > 0.001f || world.ground_fog > 0.001f || world.weather > 0.45f || sky_debug == 4 || sky_debug == 5 ||
                   SeaReflection() ||
                   sky_debug == 6 || REXCVAR_GET(native_x_renderer_rain_lens) || REXCVAR_GET(native_x_renderer_wetness) ||
                   (REXCVAR_GET(native_x_renderer_sky) && !SkyForcedOff());  // (the clouds over the view are laid with it)  // (the lens's water needs the sea's height)
    // D175: the night's lights come on as the cycle's sun sets (from 4 degrees up) and are full 6 degrees under.
    {
      float dark = 0.0f;
      if (world.day_night) {
        const float t = std::clamp((SkyLightAt(world.hour, world.weather).sun_elevation - 4.0f) / -10.0f, 0.0f, 1.0f);
        dark = t * t * (3.0f - 2.0f * t);
      }
      g_cycle_dark.store(dark, std::memory_order_relaxed);
    }
    if (world.day_night && REXCVAR_GET(native_x_renderer_sky_light)) {
      const SkyLight at = SkyLightAt(world.hour, world.weather);
      light.on = true;
      light.game_sky = !SkyOn();  // the game's own sky is drawn: tinted by the hour
      for (int i = 0; i < 3; ++i) light.key[i] = at.key[i], light.sky[i] = at.sky[i], light.ground[i] = at.ground[i];
      for (int i = 0; i < 3; ++i) light.painted[i] = at.painted[i];
      // The storm's lightning: for its moment the sky's light on everything, and the painted sky, flash blue-white.
      if (const float flash = SkyFlashAt(world.cloud_seconds, world.storm).light; flash > 0.0f) {  // (D208: the match's)
        const float tint[3] = {0.72f, 0.8f, 1.0f};
        for (int i = 0; i < 3; ++i) {
          light.sky[i] += flash * 0.9f * tint[i];
          light.ground[i] += flash * 0.6f * tint[i];
          light.painted[i] += flash * 0.5f * tint[i];
        }
      }
      // Where the light comes from: the sun, or once it is down the moon (opposite it), never under 5 degrees.
      if (REXCVAR_GET(native_x_renderer_sky_sun_moves)) {
        const bool moon = at.sun_elevation < -1.5f;
        const float rad = 3.14159265f / 180.0f;
        light.turn = true;
        light.azimuth = (SkyNoonAzimuth() - at.sun_turn + (moon ? 180.0f : 0.0f)) * rad;
        light.elevation = std::max(moon ? -at.sun_elevation : at.sun_elevation, 5.0f) * rad;
        // While it rains (sky.cpp's rain: from 55% of the weather) the shadows go steep.
        light.steep = std::clamp((world.weather - 0.55f) / 0.4f, 0.0f, 1.0f);
      }
    }
    // NXR_LIGHT_TEST=<nine numbers> (a developer's check of the mapping): the multipliers themselves, the sun's
    // r, g, b, the sky's and the ground's.
    static const std::vector<float> test = [] {
      std::vector<float> out;
      for (const char* q = std::getenv("NXR_LIGHT_TEST"); q && *q;) {
        char* e = nullptr;
        const float v = std::strtof(q, &e);
        if (e == q) break;
        out.push_back(v);
        q = *e == ',' ? e + 1 : e;
      }
      return out;
    }();
    if (test.size() == 9) {
      light.on = true;
      for (int i = 0; i < 3; ++i) light.key[i] = test[i], light.sky[i] = test[3 + i], light.ground[i] = test[6 + i];
    }
    g_draw->SetOutdoorLight(light);
    static bool said_dir = false;
    float dir[3];
    if (!said_dir && g_draw->GameLightDir(dir)) {
      said_dir = true;
      REXLOG_INFO("[native_x_renderer] the game's outdoor light direction: {:.4f} {:.4f} {:.4f}", dir[0], dir[1], dir[2]);
    }
  }
  g_draw->SetProjectionVote((g_gtao_on || g_ssgi_on) && g_scene_view && !g_gtao_done);
  // The terrain's draws: the grass's, and the night's lights' camera (D163).
  g_draw->SetTerrainCapture(g_scene_view && !g_gtao_done && (GrassOn() || NightLightsWanted() || SkyOn() || FogOn()) ? &g_terrain : nullptr);
  // D217: the same models' bodies wet in the rain, by day or night.
  g_draw->SetWetModels(g_scene_view && !g_night_model_counts.empty() ? &g_night_model_counts : nullptr);
  // D163: the models with lamps (a jeep's headlights), at night.
  g_draw->SetModelLamps(g_scene_view && !g_night_model_counts.empty() && NightPassOn() &&
                                REXCVAR_GET(native_x_renderer_night_lamps) > 0.0
                            ? &g_night_model_counts
                            : nullptr,
                        &ReportNightModelDraw);
  // D222: the buildings' standing parts, while the rooms may be lit.
  g_draw->SetIntactParts(g_scene_view && !g_room_intact_counts.empty() && g_cycle_dark.load(std::memory_order_relaxed) > 0.0f
                             ? &g_room_intact_counts
                             : nullptr,
                         &NoteIntactPart);
  std::array<DrawPath::Copy, 2> copies;
  for (uint32_t i = 0; i < c.copy_count; ++i) {
    copies[i] = {c.copies[i].address, c.data.data() + c.copies[i].offset, c.copies[i].size};
  }
  // D179: the effects' layer before the effects: the game draws its sky dome into the half size layer first
  // (E059F66662ECF0A4; not drawn with a sky of the renderer's own, but its draws still come by here), then the
  // effects. At the first other draw into that target, its texture is kept.
  if (NightPassOn() && c.programs[1] && targets[0] && !targets[0]->depth && targets[0]->samples == 1 &&
      targets[0]->texture && REXCVAR_GET(native_x_renderer_night_glow) > 0.0) {
    const bool dome = g_draw->PixelShaderHash(*c.programs[1]) == 0xE059F66662ECF0A4ull;
    if (dome && targets[0]->guest_width != g_profile->output_width) {
      g_night_glow_dome_into = targets[0];
    } else if (!dome && g_night_glow_dome_into == targets[0] &&
               g_night_glow_before_frame != g_device->frames_presented()) {
      using namespace Diligent;
      const TextureDesc& have = targets[0]->texture->GetDesc();
      if (!g_night_glow_before || g_night_glow_before->GetDesc().Width != have.Width ||
          g_night_glow_before->GetDesc().Height != have.Height || g_night_glow_before->GetDesc().Format != have.Format) {
        g_night_glow_before.Release();
        TextureDesc d;
        d.Name = "NativeXRenderer night effects' layer before them";
        d.Type = RESOURCE_DIM_TEX_2D;
        d.Width = have.Width, d.Height = have.Height;
        d.Format = have.Format;
        d.Usage = USAGE_DEFAULT;
        d.BindFlags = BIND_SHADER_RESOURCE;
        g_device->device()->CreateTexture(d, nullptr, &g_night_glow_before);
      }
      if (g_night_glow_before) {
        MsaaRead(targets[0]);
        g_device->context()->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
        CopyTextureAttribs copy{targets[0]->texture, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, g_night_glow_before,
                                RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
        g_device->context()->CopyTexture(copy);
        g_night_glow_before_frame = g_device->frames_presented();
        g_night_glow_before_of = targets[0];
        g_draw->TargetsChanged();
      }
    }
  }
  // D195: which pixel shaders draw the effects (into the effects' layer after its sky dome), named once each.
  if (NightPassOn() && c.programs[1] && targets[0] && targets[0] == g_night_glow_dome_into) {
    static std::mutex said_mutex;
    static std::set<uint64_t> said;
    const uint64_t h = g_draw->PixelShaderHash(*c.programs[1]);
    std::lock_guard lock(said_mutex);
    if (h != 0xE059F66662ECF0A4ull && said.size() < 64 && said.insert(h).second)
      REXLOG_INFO("[native_x_renderer] night: an effect drawn by pixel shader {:016X}", h);
  }
  g_draw->TakePackedRead();
  // native_x_renderer_hide_hud: the HUD's draws (after the composite of a frame with a 3D view) not drawn.
  if (!(hide_hud && g_hud_phase == 2))
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
      const bool depth = g_night_depth && g_night_depth_frame == g_device->frames_presented();
      // The depth's coordinates from the target's: its texture may be larger than the output (the EDRAM's pitch).
      const TextureDesc& desc = t->texture->GetDesc();
      const float view_w = float(t->scale.HostWidth(g_profile->output_width));
      const float view_h = float(t->scale.HostHeight(g_profile->output_height));
      const bool world = g_night_world && g_night_world_frame == g_device->frames_presented();
      // Each depth with its own share of its texture (with an upscaler the textures can differ from the target's).
      const float to_u = view_w > 0 ? float(desc.Width) / view_w : 1.0f;
      const float to_v = view_h > 0 ? float(desc.Height) / view_h : 1.0f;
      // D175: the scene as the cycle drew it, with the lights only, as strong as the hour is dark.
      const float amount = NightLightsAmount();
      night.glow = float(std::clamp(REXCVAR_GET(native_x_renderer_night_glow), 0.0, 8.0)) * amount;
      night.lamps = float(std::clamp(REXCVAR_GET(native_x_renderer_night_lamps), 0.0, 8.0)) * amount;
      night.debug = REXCVAR_GET(native_x_renderer_night_debug);
      night.bloom = REXCVAR_GET(native_x_renderer_lamp_bloom) ? 1.0f : 0.0f;
      night.flare = REXCVAR_GET(native_x_renderer_lens_flare) ? 1.0f : 0.0f;
      night.seen_pass = REXCVAR_GET(native_x_renderer_night_lamp_visibility_pass);
      night.timing = REXCVAR_GET(native_x_renderer_gpu_sections);
      // BF_NIGHT_AB=1 (tests): the lamps' visibility pass on and off by turns, 600 frames each (a stats line's span),
      // so both ways are timed in one run over the same scene.
      static const bool night_ab = [] {
        const char* v = std::getenv("BF_NIGHT_AB");
        return v && *v == '1';
      }();
      if (night_ab) night.seen_pass = AbPhase() == 0;
      // BF_NIGHT_FX_AB=1 (tests): the effects' lights' compute search and the old pixel pass by turns, 600 frames each.
      static const bool fx_ab = [] {
        const char* v = std::getenv("BF_NIGHT_FX_AB");
        return v && *v == '1';
      }();
      if (fx_ab) night.compute_lights = AbPhase() == 0;
      // BF_NIGHT_TILES_AB=1 (tests): the lights' screen tiles and all lights for every pixel by turns, 600 frames each.
      static const bool tiles_ab = [] {
        const char* v = std::getenv("BF_NIGHT_TILES_AB");
        return v && *v == '1';
      }();
      if (tiles_ab) night.tiles = AbPhase() == 0;
      // D177: the cycle's midnight draws the scene about a twelfth as bright as the game's day.
      night.scene_gain = 1.0f + 11.0f * g_cycle_dark.load(std::memory_order_relaxed);
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
      // BF_NIGHT_PARTS=1 (tests): the night pass's parts off by turns, 600 frames each, to time each: all on, the lens
      // flare off, the lamps' bloom off, the lamps' light on the scene off (their glass and bloom kept).
      static const bool parts = [] {
        const char* v = std::getenv("BF_NIGHT_PARTS");
        return v && *v == '1';
      }();
      if (parts) {
        const uint64_t part = (g_device->frames_presented() / 600) % 4;
        if (part == 1) night.flare = 0.0f;
        if (part == 2) night.bloom = 0.0f;
        if (part == 3) f.light_count = 0;
      }
      MsaaWrite(t);
      g_night.Apply(g_device->device(), g_device->context(), t->texture, t->view, f, night);
      // The player's weapon skin's overlay (its storm's light, its cloud), kept from the scene's end for here.
      if (g_skin_overlay_frame == g_device->frames_presented() && g_skin_overlay.depth_srv && view_w > 0 && view_h > 0) {
        g_skin_overlay_frame = ~0ull;
        g_skin_overlay.color = t->texture;
        g_skin_overlay.width = uint32_t(view_w), g_skin_overlay.height = uint32_t(view_h);
        WeaponSkinBloom(g_device->device(), g_device->context(), g_skin_overlay);
      }
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
        MsaaRead(t);
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
      MsaaWrite(t);
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
  if (ClipToTarget(*target, r)) {
    g_device->recorder().Do([view = target->view, msaa = target->msaa_view,
                             c = std::array<float, 4>{color[0], color[1], color[2], color[3]}](
                                IDeviceContext* context) mutable {
      context->SetRenderTargets(1, &view, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      context->ClearRenderTarget(view, c.data(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      if (msaa) {  // host MSAA: the twin too, and both are then alike
        context->SetRenderTargets(1, &msaa, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->ClearRenderTarget(msaa, c.data(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      }
    });
    if (target->msaa_view) target->msaa_stale = target->single_stale = target->msaa_delta = false;
  } else if (r.width && r.height && FillableTarget(*target)) {
    g_blit.FillColor(g_device->context(), target->view, HostRect(target->scale, r), color);
    if (target->msaa_view) g_blit.FillColor(g_device->context(), target->msaa_view, HostRect(target->scale, r), color);
  }
  target->written = ++g_target_writes;
  if (g_draw) g_draw->TargetsChanged();
}

// The same for a depth-stencil target's depth and/or stencil.
void ClearDepthStencilRect(HostTarget* target, Blitter::Rect r, bool depth, float z, bool stencil, uint8_t s) {
  if (ClipToTarget(*target, r)) {
    CLEAR_DEPTH_STENCIL_FLAGS clear_flags = CLEAR_DEPTH_FLAG_NONE;
    if (depth) clear_flags |= CLEAR_DEPTH_FLAG;
    if (stencil) clear_flags |= CLEAR_STENCIL_FLAG;
    g_device->recorder().Do([view = target->view, msaa = target->msaa_view, clear_flags, z, s](IDeviceContext* context) {
      context->SetRenderTargets(0, nullptr, view, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      context->ClearDepthStencil(view, clear_flags, z, s, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      if (msaa) {  // host MSAA: the twin too; alike once the depth is cleared (the stencil is not carried)
        context->SetRenderTargets(0, nullptr, msaa, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->ClearDepthStencil(msaa, clear_flags, z, s, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      }
    });
    if (target->msaa_view && depth) target->msaa_stale = target->single_stale = false;
  } else if (r.width && r.height && FillableTarget(*target)) {
    g_blit.FillDepthStencil(g_device->context(), target->view, HostRect(target->scale, r), depth, z, stencil, s);
    if (target->msaa_view)
      g_blit.FillDepthStencil(g_device->context(), target->msaa_view, HostRect(target->scale, r), depth, z, stencil, s);
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
         NightPassOn() && REXCVAR_GET(native_x_renderer_night_lamps) > 0.0;
}

// D163: the terrain's camera kept for the night's lights (at the AO's moment, before the terrain's frame is reset): its
// world-view-projection (the world in the level's own units), viewport and NDC transform, and the view's host size.
void NoteNightCamera() {
  if (!g_device || !g_terrain.valid || g_night_camera.frame == g_device->frames_presented()) return;
  if (!NightPassOn()) return;
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

// The world (a direction, or a place less the eye) from clip x, y and w: the inverse of rows 0, 1 and 3's 3x3 part of
// `m`, in double precision; false when it has none.
bool InverseXYW(const float (*m)[4], double inv[3][3]) {
  const double a[3][3] = {{m[0][0], m[0][1], m[0][2]}, {m[1][0], m[1][1], m[1][2]}, {m[3][0], m[3][1], m[3][2]}};
  const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                     a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  if (!(std::abs(det) > 1e-12)) return false;
  inv[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
  inv[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
  inv[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
  inv[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
  inv[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
  inv[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
  inv[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
  inv[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
  inv[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
  return true;
}
// The night target's texture coordinates to NDC: to the view's 0 to 1, its depth target's pixels, the draw's viewport
// (DiligentCore's, Direct3D's way round: NDC y +1 at its top), then the translated shaders' NDC transform undone.
bool NightToNdc(float out[4], float to_u, float to_v, float view_w, float view_h, const Diligent::Viewport& vp,
                const float ndc_scale[3], const float ndc_offset[3]) {
  if (!(vp.Width > 0.0f && vp.Height > 0.0f && ndc_scale[0] != 0.0f && ndc_scale[1] != 0.0f)) return false;
  out[0] = to_u * view_w * 2.0f / (vp.Width * ndc_scale[0]);
  out[1] = -to_v * view_h * 2.0f / (vp.Height * ndc_scale[1]);
  out[2] = (-vp.TopLeftX * 2.0f / vp.Width - 1.0f - ndc_offset[0]) / ndc_scale[0];
  out[3] = (vp.TopLeftY * 2.0f / vp.Height + 1.0f - ndc_offset[1]) / ndc_scale[1];
  return true;
}
// D163: this frame's camera for the night pass (`to_u`, `to_v`: the night target's texture coordinates to the view's
// 0 to 1), and the lamps it lights and shows: the nearest whose light reaches into the view, and the nearest bulbs in
// it. False without this frame's camera.
bool BuildNightCamera(NightFrame& f, float to_u, float to_v, const NightSettings& s) {
  const NightCamera& cam = g_night_camera;
  if (!g_device || cam.frame != g_device->frames_presented()) return false;
  const float(*m)[4] = cam.wvp;
  // World from clip x, y and w: the inverse of those three rows' 3x3 part.
  double inv[3][3];
  if (!InverseXYW(m, inv)) return false;
  const double t[3] = {m[0][3], m[1][3], m[3][3]};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) f.inverse[r][c] = float(inv[r][c]);
    f.inverse_t[r] = float(t[r]);
    f.eye[r] = float(-(inv[r][0] * t[0] + inv[r][1] * t[1] + inv[r][2] * t[2]));  // where clip x, y and w are 0
  }
  // The night target's texture coordinates to NDC: to the view's 0 to 1, its depth target's pixels, the terrain's
  // viewport (DiligentCore's, Direct3D's way round: NDC y +1 at its top), then the translated shaders' NDC transform
  // undone.
  if (!NightToNdc(f.to_ndc, to_u, to_v, cam.view_width, cam.view_height, cam.viewport, cam.ndc_scale, cam.ndc_offset))
    return false;
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
    // D178: a vehicle's lamps are on only while someone is in it (the user, 16 Nov 2026). The player's own: the game
    // says when the player sits in a vehicle (NightVehicleSeated), and it is the model nearest the camera then (within
    // 14 m: the third person camera stands behind it). Any other: one that drives (over 0.5 m/s, its lamps kept on 6 s
    // after it stops); an empty one never moves by itself.
    {
      struct Track {
        double p[3];
        double seen_at, moved_at;
      };
      static std::vector<Track> tracks;
      static const auto t0 = std::chrono::steady_clock::now();
      const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      static uint64_t swept = ~0ull;
      if (swept != g_device->frames_presented()) {
        swept = g_device->frames_presented();
        std::erase_if(tracks, [now](const Track& k) { return now - k.seen_at > 3.0; });
      }
      Track* track = nullptr;
      double best = 9.0;  // the same vehicle as last frame: within 3 m of where one was
      for (Track& k : tracks) {
        const double q = (k.p[0] - T[0]) * (k.p[0] - T[0]) + (k.p[1] - T[1]) * (k.p[1] - T[1]) + (k.p[2] - T[2]) * (k.p[2] - T[2]);
        if (q < best && k.seen_at < now) best = q, track = &k;
      }
      if (!track) {
        tracks.push_back({{T[0], T[1], T[2]}, now, -1e9});
        track = &tracks.back();
      } else {
        const double dt = now - track->seen_at;
        if (dt > 1e-4 && dt < 0.5 && std::sqrt(best) / dt > 0.5) track->moved_at = now;
        track->p[0] = T[0], track->p[1] = T[1], track->p[2] = T[2], track->seen_at = now;
      }
      // NXR_VEHICLE_LAMPS_ON=1 (tests): every vehicle's lamps on, empty or not, to see them from outside (the
      // developer camera moves only the soldier's view, not a vehicle's).
      static const bool all_on = [] {
        const char* v = std::getenv("NXR_VEHICLE_LAMPS_ON");
        return v && *v == '1';
      }();
      bool on = all_on || now - track->moved_at < 6.0;
      if (!on && g_night_vehicle_seated.load(std::memory_order_relaxed)) {
        // The nearest model with lamps to the camera, within 14 m: the player's own.
        auto far2 = [&](const Mat4d& o) {
          const double x = o[3] - f.eye[0], y = o[7] - f.eye[1], z = o[11] - f.eye[2];
          return x * x + y * y + z * z;
        };
        const double mine = far2(mm);
        on = mine < 14.0 * 14.0;
        for (size_t e = 0; e < g_night_model_draws.size() && on; ++e) {
          bool known = false;
          for (const NightModelLamps& k : g_night_models_render) known = known || k.index_count == g_night_model_draws[e].index_count;
          if (known && far2(g_night_model_draws[e].m) < mine - 0.01) on = false;
        }
      }
      if (!on) continue;
    }
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
  // D170: this frame's firelight and turning beams (seconds since the renderer started, each lamp its own phase from
  // where it is, so no two fires waver together).
  {
    static const auto t0 = std::chrono::steady_clock::now();
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    for (NightLamp& l : lamps) {
      if (l.flicker <= 0.0f && l.spin == 0.0f) continue;
      const float phase = std::fmod(std::abs(l.position[0] * 0.73f + l.position[2] * 1.37f + l.position[1] * 0.31f), 6.2831853f);
      if (l.flicker > 0.0f) {
        // Three unrelated waves and a slow breath: a fire's light, never off, never a strobe.
        const float w = 0.5f * std::sin(t * 7.3f + phase) + 0.3f * std::sin(t * 13.1f + phase * 2.1f) +
                        0.2f * std::sin(t * 23.7f + phase * 3.7f);
        const float breath = 0.5f + 0.5f * std::sin(t * 0.9f + phase * 0.5f);
        const float k = 1.0f + l.flicker * (0.28f * w - 0.12f * breath);
        for (float& c : l.color) c *= std::max(0.3f, k);
      }
      if (l.spin != 0.0f) {
        const float a = t * l.spin + phase, ca = std::cos(a), sa = std::sin(a);
        const float x = l.direction[0], z = l.direction[2];
        l.direction[0] = x * ca - z * sa, l.direction[2] = x * sa + z * ca;
      }
    }
  }
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
    // D195 (the user, 18 Nov 2026: the light "projected by vehicle headlights and lamps/light sources" seen "from
    // across the entire map"): every lamp whose light reaches into the view, however far (was its range + 250 m).
    bool in_view = true;
    for (int p = 0; p < 5 && in_view; ++p) {
      const float n = std::sqrt(planes[p][0] * planes[p][0] + planes[p][1] * planes[p][1] + planes[p][2] * planes[p][2]);
      const float side = planes[p][0] * l.position[0] + planes[p][1] * l.position[1] + planes[p][2] * l.position[2] + planes[p][3];
      if (n > 0.0f && side < -l.range * n) in_view = false;
    }
    if (in_view) lit.push_back({distance, i});
    // D195 (the user, 18 Nov 2026: "The lamp lights should also be able to be seen from across the entire map"): every
    // bulb, however far (was 400 m); the nearest in view are taken below.
    if (l.glow > 0.0f) shown.push_back({distance, i});
  }
  auto nearest = [](std::vector<Near>& v, size_t keep) {
    std::sort(v.begin(), v.end(), [](const Near& x, const Near& y) { return x.distance < y.distance; });
    if (v.size() > keep) v.resize(keep);
  };
  nearest(lit, Night::kMaxLights);
  nearest(shown, shown.size());  // (all of them, nearest first: those out of view are passed over below)
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
                  f.to_ndc[2], f.to_ndc[3], cam.viewport.TopLeftX, cam.viewport.TopLeftY, cam.viewport.Width, cam.viewport.Height, cam.view_width,
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
    // A halo six times the bulb's radius, and never under four pixels of the target across.
    const float halo = l.glow * 6.0f;
    const float min_uv = 4.0f / std::max(1.0f, cam.view_width / std::max(to_u, 1e-3f));
    o.radius[0] = std::max(std::abs(halo * row_x / cw / f.to_ndc[0]), min_uv);
    o.radius[1] = std::max(std::abs(halo * row_y / cw / f.to_ndc[1]), min_uv * std::abs(f.to_ndc[0] / f.to_ndc[1]));
    o.depth = 0.1f / cw;
    // The air between: a bulb 2 km away keeps about a third (D195; was 700 m, which left the far side of a map dark).
    const float through = std::exp(-n.distance / 1800.0f);
    // D177: a hooded lamp or a headlight shows its bulb to those it shines toward: from the side less, from behind
    // only its spill.
    float seen = 1.0f;
    if (l.direction[0] * l.direction[0] + l.direction[1] * l.direction[1] + l.direction[2] * l.direction[2] > 0.25f) {
      const float d = std::max(n.distance, 1e-3f);
      const float toward = (l.direction[0] * (f.eye[0] - p[0]) + l.direction[1] * (f.eye[1] - p[1]) +
                            l.direction[2] * (f.eye[2] - p[2])) / d;
      const float t = std::clamp((toward + 0.25f) / 0.75f, 0.0f, 1.0f);
      seen = 0.08f + 0.92f * t * t * (3.0f - 2.0f * t);
    }
    for (int c = 0; c < 3; ++c) o.color[c] = l.color[c] * 0.35f * through * seen;
    // D183: the bulb in the world, for its glass to glow; the lens flare's strength from how bright it shows and how
    // near it is (a lamp across the map flares little).
    std::memcpy(o.world, p, sizeof(o.world));
    o.bulb = std::clamp(l.glow, 0.04f, 1.0f);
    const float lum = 0.2126f * o.color[0] + 0.7152f * o.color[1] + 0.0722f * o.color[2];
    const float closeness = n.distance / 35.0f;
    o.flare = std::clamp(lum * 0.25f / (1.0f + closeness * closeness), 0.0f, 1.0f);
    g_night_frame_sprites.push_back(o);
  }
  // D183: the four strongest flares first (the shader flares only the first four); the rest flare none.
  {
    std::vector<NightSprite>& v = g_night_frame_sprites;
    std::stable_sort(v.begin(), v.end(), [](const NightSprite& a, const NightSprite& b) { return a.flare > b.flare; });
    for (size_t i = 4; i < v.size(); ++i) v[i].flare = 0.0f;
  }
  return true;
}

// D163: the effects' layer (`from`: the source's host rectangle) copied for the night pass, with its mips for the glow.
void CopyNightGlow(HostTarget* source, const Blitter::Rect& from) {
  using namespace Diligent;
  if (!g_device || !source->srv || !from.width || !from.height) return;
  MsaaRead(source);
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
  // D179: the layer began with something in it (the game's sky dome drawn into it, or with a sky of the renderer's own
  // whatever it held before): only what the effects added is their light. Without it the whole layer was added to
  // the picture whenever an effect was drawn: the sky flashed with every shot (the user, 16 Nov 2026).
  bool alone = false;
  if (g_night_glow_before && g_night_glow_before_frame == g_device->frames_presented() &&
      g_night_glow_before_of == source) {
    auto make = [&](RefCntAutoPtr<ITexture>& t, const char* name) {
      if (t && t->GetDesc().Width == from.width && t->GetDesc().Height == from.height) return;
      t.Release();
      TextureDesc d;
      d.Name = name;
      d.Type = RESOURCE_DIM_TEX_2D;
      d.Width = from.width, d.Height = from.height;
      d.Format = TEX_FORMAT_RGBA16_FLOAT;
      d.Usage = USAGE_DEFAULT;
      d.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
      g_device->device()->CreateTexture(d, nullptr, &t);
    };
    make(g_night_glow_a, "NativeXRenderer night effects with their ground");
    make(g_night_glow_b, "NativeXRenderer night effects' ground");
    if (g_night_glow_a && g_night_glow_b) {
      g_blit.Copy(g_device->context(), source->srv, Blitter::Source::kColor, from,
                  g_night_glow_a->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET), to, Blitter::Options{});
      g_blit.Copy(g_device->context(), g_night_glow_before->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE),
                  Blitter::Source::kColor, from, g_night_glow_b->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET), to,
                  Blitter::Options{});
      alone = g_night.Subtract(g_device->device(), g_device->context(),
                               g_night_glow_a->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE),
                               g_night_glow_b->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE),
                               g_night_glow->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET));
    }
  }
  if (!alone)
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
    // Host MSAA: it reads the one-sample textures and draws its tufts into the twins, as the game's draws do (a
    // one-sample depth written here would flatten the twin's, and what the game draws next, the sky, would then
    // cover every edge's samples: run msaa4u).
    const bool twins = g_scene_color->msaa_view && g_scene_depth->msaa_view &&
                       g_scene_color->msaa_samples == g_scene_depth->msaa_samples;
    MsaaRead(g_scene_color);
    MsaaRead(g_scene_depth);
    if (twins) {
      MsaaUp(g_scene_color);
      MsaaUp(g_scene_depth);
    } else {
      MsaaWrite(g_scene_color);
      MsaaWrite(g_scene_depth);
    }
    if (Timed(SectionTimers::kGrass, [&] {
          return g_grass.Apply(g_device->context(), g_terrain, g_scene_color->texture,
                               twins ? g_scene_color->msaa_view : g_scene_color->view, g_scene_depth->texture,
                               twins ? g_scene_depth->msaa_view : g_scene_depth->view, g_device->frames_presented());
        })) {
      if (twins) g_scene_color->single_stale = g_scene_depth->single_stale = true;
      g_draw->TargetsChanged();
    }
  }
  // The sky of its own (sky.h), where the view's depth holds nothing: before the game's water, effects and the first
  // person weapon are drawn over it. Its camera from the terrain's draws, as the night's (BuildNightCamera).
  {  // NXR_SKY_DRAWS=<frame>: the 60 draws after this moment in that frame of the view, logged (which is the game's sky)
    static const int at = [] {
      const char* v = std::getenv("NXR_SKY_DRAWS");
      return v && *v ? std::atoi(v) : 0;
    }();
    static const int count = [] {  // NXR_SKY_DRAWS=<frame>:<count>: that many instead of 60
      const char* v = std::getenv("NXR_SKY_DRAWS");
      const char* c = v ? std::strchr(v, ':') : nullptr;
      return c ? std::max(1, std::atoi(c + 1)) : 60;
    }();
    static int frames = 0;
    if (at > 0 && ++frames == at) DumpNextDraws(count);
  }
  if ((SkyOn() || FogOn()) && g_terrain.valid && g_scene_depth->srv) {
    RendererTime time(&g_phase.effects);
    Sky::Inputs in;
    const float(*m)[4] = g_terrain.wvp;
    const double a[3][3] = {{m[0][0], m[0][1], m[0][2]}, {m[1][0], m[1][1], m[1][2]}, {m[3][0], m[3][1], m[3][2]}};
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                       a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    const Diligent::Viewport& vp = g_terrain.viewport;
    if (std::abs(det) > 1e-12 && vp.Width > 0.0f && vp.Height > 0.0f && g_terrain.ndc_scale[0] != 0.0f &&
        g_terrain.ndc_scale[1] != 0.0f) {
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
        for (int c = 0; c < 3; ++c) in.inverse[r][c] = float(inv[r][c]);
        in.inverse_t[r] = float(t[r]);
        in.eye[r] = float(-(inv[r][0] * t[0] + inv[r][1] * t[1] + inv[r][2] * t[2]));
        for (int c = 0; c < 3; ++c) in.forward[r][c] = float(a[r][c]);
      }
      TrackSkyEye(in.eye, true);
      NoteListener(in.eye, a);
      in.width = g_scene_color->scale.HostWidth(g_profile->output_width);
      in.height = g_scene_color->scale.HostHeight(g_profile->output_height);
      in.to_ndc[0] = float(in.width) * 2.0f / (vp.Width * g_terrain.ndc_scale[0]);
      in.to_ndc[1] = -float(in.height) * 2.0f / (vp.Height * g_terrain.ndc_scale[1]);
      in.to_ndc[2] = (-vp.TopLeftX * 2.0f / vp.Width - 1.0f - g_terrain.ndc_offset[0]) / g_terrain.ndc_scale[0];
      in.to_ndc[3] = (vp.TopLeftY * 2.0f / vp.Height + 1.0f - g_terrain.ndc_offset[1]) / g_terrain.ndc_scale[1];
      const WorldState world = World();
      in.seconds = world.cloud_seconds;
      in.evolve = world.cloud_evolve;
      in.weather = world.weather;
      StormInputs(world, in);  // D208
  CloudMassInputs(in);  // D232
      in.fog = world.fog;
      in.ground_fog = world.ground_fog;
      in.ground_fog_height = world.ground_fog_height, in.ground_fog_sparse = world.ground_fog_sparse;
      in.wind = world.wind;
      in.hour = world.hour;
      in.sun_azimuth = SkyNoonAzimuth();
      in.coverage = world.cloud_cover;
      in.exposure = float(REXCVAR_GET(native_x_renderer_sky_exposure));
      in.debug = REXCVAR_GET(native_x_renderer_sky_debug);
      in.cloud_height = world.cloud_height;
      WakeInputs(in);  // D234
      {  // NXR_CLOUD_LIFT=<metres> (a developer's check): the clouds seen as from that much higher, in them or over them
        static const float lift = [] {
          const char* v = std::getenv("NXR_CLOUD_LIFT");
          return v && *v ? float(std::atof(v)) : 0.0f;
        }();
        in.cloud_lift = lift;
      }
      if (SkyOn()) {
        MsaaRead(g_scene_depth);
        MsaaWrite(g_scene_color);
        in.color = g_scene_color->texture;
        in.depth_srv = g_scene_depth->srv;
        if (Timed(SectionTimers::kSky, [&] { in.skip_hidden_clouds = CloudSkipNow(); in.checker_clouds = CloudCheckerNow(); in.old_cloud_density = CloudDensityOldNow(); in.old_cloud_reach = CloudReachOldNow(); in.old_cloud_bands = CloudBandsOldNow(); return g_own_sky.Apply(g_device->context(), in); })) g_draw->TargetsChanged();
      }
      g_last_sky_in = in, g_last_sky_valid = true;
      // The fog waits for the game's water (ApplyFog).
      if (FogOn()) {
        g_fog_in = in;
        g_fog_ready = true;
        g_fog_dome = false;
      }
    }
  } else if ((SkyOn() || FogOn()) && !g_terrain.valid && g_last_sky_valid) {
    g_sky_late = true;  // no terrain in this view: after the sky dome's draws (ApplyLateSky)
    g_fog_dome = false;
  }
  NoteNightCamera();  // D163
  if (g_terrain.valid && g_terrain.height && g_terrain.height_size) g_fog_terrain = g_terrain;  // D231
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
  // Host MSAA: the effects read the one-sample depth. The ambient occlusion alone multiplies its factor into the
  // colour's twin, every sample of a pixel alike, so the edges' samples stay apart for what is drawn after it; the
  // global illumination reads the colour too, and takes the one-sample texture.
  MsaaRead(g_scene_depth);
  const bool ao_twin = ao_on && !gi_on && g_scene_color->msaa_view;
  if (ao_twin) MsaaUp(g_scene_color);
  else MsaaWrite(g_scene_color);
  if (ao_on) {
    Gtao::Inputs in;
    in.projection = projection;
    in.depth = g_scene_depth->texture;
    in.color = ao_twin ? g_scene_color->msaa_view : g_scene_color->view;
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
    const bool ao_applied = Timed(SectionTimers::kGtao, [&] { return g_gtao.Apply(g_device->context(), in); });
    if (ao_applied && ao_twin) g_scene_color->single_stale = true;
    applied = ao_applied || applied;
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
    applied = Timed(SectionTimers::kSsgi, [&] { return g_ssgi.Apply(g_device->context(), in); }) || applied;
  }
  if (applied) g_draw->TargetsChanged();
}

// The renderer's sky in a view without terrain (g_sky_late), then its fog.
// The eye's place for the sky, followed from frame to frame: `fresh`, it is kept with its speed; else it is set to
// where that speed has carried it since (for twenty seconds at the most).
void TrackSkyEye(float eye[3], bool fresh) {
  static float last[3] = {}, speed[3] = {};
  static std::chrono::steady_clock::time_point at{};
  static bool known = false;
  const auto now = std::chrono::steady_clock::now();
  const float dt = known ? std::chrono::duration<float>(now - at).count() : 0.0f;
  if (fresh) {
    if (known && dt > 1e-4f && dt < 0.5f) {
      for (int i = 0; i < 3; ++i) {
        const float v = (eye[i] - last[i]) / dt;
        speed[i] = std::abs(v) < 400.0f ? speed[i] + (v - speed[i]) * std::min(1.0f, dt * 8.0f) : 0.0f;  // (a jump: a respawn)
      }
    } else {
      speed[0] = speed[1] = speed[2] = 0.0f;
    }
    for (int i = 0; i < 3; ++i) last[i] = eye[i];
    at = now, known = true;
  } else if (known && dt < 20.0f) {
    for (int i = 0; i < 3; ++i) eye[i] = last[i] + speed[i] * dt;
  }
}

void ApplyLateSky() {
  if (!g_sky_late) return;
  g_sky_late = g_fog_dome = false;
  float m[4][4];
  if (!g_scene_color || !g_scene_depth || !g_draw || !g_scene_color->texture || !g_scene_depth->srv ||
      !g_draw->DomeCamera(m)) {
    return;
  }
  const double a[3][3] = {{m[0][0], m[0][1], m[0][2]}, {m[1][0], m[1][1], m[1][2]}, {m[3][0], m[3][1], m[3][2]}};
  const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                     a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  if (std::abs(det) < 1e-12) return;
  Sky::Inputs in = g_last_sky_in;  // the view's size as it last was
  // The eye's place now, from the game's lit draws (a plane climbing with its nose up shows no terrain for as long
  // as it climbs: with the eye kept where the terrain last gave it, the clouds never came nearer, and jumped down
  // when the nose came down again; the user, 16 Nov 2026).
  {
    float eye[3];
    if (g_draw->WorldCamera(eye)) {
      static bool said_eye = false;
      if (!said_eye) {
        said_eye = true;
        REXLOG_INFO("[native_x_renderer] sky: a view without terrain: the eye at {:.1f} {:.1f} {:.1f} from the game's lit draws "
                    "(the terrain's last gave {:.1f} {:.1f} {:.1f})", eye[0], eye[1], eye[2], in.eye[0], in.eye[1], in.eye[2]);
      }
    }
    // Fresh when a lit draw of the world's gave it within the last two frames; else (a cockpit's view of nothing but
    // sky) the eye goes on from where it last was at the speed it last had.
    uint64_t at = 0;
    const bool fresh = g_draw->WorldCamera(eye, &at) && at + 2 >= g_device->frames_presented();
    if (fresh)
      for (int i = 0; i < 3; ++i) in.eye[i] = eye[i];
    TrackSkyEye(in.eye, fresh);
  }
  const double inv[3][3] = {
      {(a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det, (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det,
       (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det},
      {(a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det, (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det,
       (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det},
      {(a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det, (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det,
       (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det}};
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col) in.inverse[row][col] = float(inv[row][col]), in.forward[row][col] = float(a[row][col]);
  NoteListener(in.eye, a);
  const WorldState world = World();
  in.seconds = world.cloud_seconds;
  in.evolve = world.cloud_evolve;
  in.weather = world.weather;
  StormInputs(world, in);  // D208
  CloudMassInputs(in);  // D232
  in.fog = world.fog;
  in.ground_fog = world.ground_fog;
  in.ground_fog_height = world.ground_fog_height, in.ground_fog_sparse = world.ground_fog_sparse;
  in.wind = world.wind;
  in.cloud_height = world.cloud_height;
  WakeInputs(in);  // D234
  in.hour = world.hour;
  in.sun_azimuth = SkyNoonAzimuth();
  in.coverage = world.cloud_cover;
  in.debug = REXCVAR_GET(native_x_renderer_sky_debug);
  RendererTime time(&g_phase.effects);
  if (SkyOn()) {
    MsaaRead(g_scene_depth);
    MsaaWrite(g_scene_color);
    in.color = g_scene_color->texture;
    in.depth_srv = g_scene_depth->srv;
    if (Timed(SectionTimers::kSky, [&] { in.skip_hidden_clouds = CloudSkipNow(); in.checker_clouds = CloudCheckerNow(); in.old_cloud_density = CloudDensityOldNow(); in.old_cloud_reach = CloudReachOldNow(); in.old_cloud_bands = CloudBandsOldNow(); return g_own_sky.Apply(g_device->context(), in); })) g_draw->TargetsChanged();
  }
  static bool said = false;
  if (!said) {
    said = true;
    REXLOG_INFO("[native_x_renderer] sky: a view without terrain: the sky drawn from the sky dome's camera");
  }
  if (FogOn()) {
    g_fog_in = in;
    g_fog_ready = true;
    ApplyFog();
  }
}

// The fog over this frame's view (sky.h's Fog), once: after the game's sky dome's draws (ExecuteDraw), or, in a view
// that drew none, before the finished scene is copied (ExecuteResolve).
// D232: the clouds' mass as particles, stepped at 20 Hz (the far level: slow): the eye, the sky's weather map and its
// drift (the clouds' wind is how fast that goes), the storm.
void StepCloudMass(IDeviceContext* context) {
  SetStrikeByMass(REXCVAR_GET(native_x_renderer_cloud_particles) && g_cloud_mass.Ready() && SkyOn());
  if (!REXCVAR_GET(native_x_renderer_cloud_particles) || !g_device || !context || !SkyOn()) return;
  static bool tried = false, made = false;
  if (!tried) tried = true, made = g_cloud_mass.Initialize(g_device->device());
  if (!made) return;
  ITextureView* weather = nullptr;
  float drift[2] = {}, scale = 1.0f;
  if (!g_own_sky.CloudMap(&weather, drift, &scale) || !weather) return;
  static std::chrono::steady_clock::time_point last{};
  static double owed = 0.0;
  static float drift_was[2] = {};
  static bool have_drift = false;
  const auto now = std::chrono::steady_clock::now();
  const double frame = last.time_since_epoch().count() ? std::chrono::duration<double>(now - last).count() : 0.0;
  last = now;
  owed += frame < 0.5 ? frame : 0.0;
  constexpr double kStep = 1.0 / 20.0;
  if (owed < kStep) return;
  const double dt = std::min(owed, 0.2);
  owed = 0.0;
  CloudMass::Step step;
  step.eye[0] = g_fog_in.eye[0], step.eye[1] = g_fog_in.eye[2];
  step.dt = float(dt), step.scale = scale, step.weather = weather;
  step.drift[0] = drift[0], step.drift[1] = drift[1];
  // (The clouds' wind: the weather map is read at a place plus its drift, 6 and 3 of the clouds' units a second of the
  // clouds' clock (sky.cpp), so what it shows goes the other way. The clock: the match's, every player's the same.)
  step.wind[0] = -6.0f * scale, step.wind[1] = -3.0f * scale;
  step.seconds = g_fog_in.seconds;
  (void)drift_was, (void)have_drift;
  if (g_fog_in.storm_b[2] > 0.5f) {
    step.storm[0] = g_fog_in.storm_a[0], step.storm[1] = g_fog_in.storm_a[1];
    step.storm[2] = g_fog_in.storm_a[3], step.storm[3] = g_fog_in.storm_b[0];
    step.storm_eye = g_fog_in.storm_a[2];
  }
  g_cloud_mass.Advance(context, step);
}

// D231: the ground fog pushed about (ground_fog.h): what the game says moves through it, its blasts and shots, kept
// here until the field's next step; the field stepped at 30 Hz before the fog pass, which reads it.
struct FogPush {
  std::mutex mutex;
  std::vector<GroundFog::Pusher> movers, once;  // once: blasts and shots, for one step
  std::chrono::steady_clock::time_point movers_at{};
};
FogPush g_fog_push;
// D234: the planes' wakes through the clouds (sky.cpp wake_density). What flies fast enough to be a plane (35 to 400
// m/s): the camera itself (a plane's pilot or gunner: the local player's own plane) and the ground fog's movers (D231:
// the vehicles within 110 m of the camera; a plane's body is 2.8 m round). Each one's path is cut into pieces of 150 m
// while it is at the clouds' heights; a piece is kept 45 s, the newest 24 in all (the one still being flown among them,
// so the hole follows the plane at once).
struct WakeSegment {
  float a[3], b[3];
  double at_a, at_b;
};
struct WakeSource {
  float at[3], from[3];
  double from_t = 0.0, seen = 0.0;
  bool camera = false;
};
struct Wakes {
  std::vector<WakeSegment> done;
  std::vector<WakeSource> sources;
  float eye[3] = {};
  double eye_t = -1.0;
  bool eye_moving = false;
} g_wakes;
void WakeInputs(Sky::Inputs& in) {
  Wakes& w = g_wakes;
  in.wake_count = 0;
  const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  constexpr double kKeep = 45.0;
  constexpr float kPiece = 150.0f;
  // The clouds' heights in the world (sky.cpp: over the sea, the layer's base at the height set, 3,200 of its
  // 1,500-unit base over it).
  float sea = 0.0f;
  if (!g_own_sky.SeaLevel(&sea)) sea = in.eye[1] - 30.0f;
  const float base = sea + std::clamp(in.cloud_height, 100.0f, 3000.0f);
  const float lo = base - 150.0f, hi = sea + (base - sea) * (1.0f + 3200.0f / 1500.0f) + 150.0f;
  auto in_band = [&](const float* p) { return p[1] > lo && p[1] < hi; };
  auto close = [&](WakeSource& s) {
    const float d = std::hypot(s.at[0] - s.from[0], s.at[1] - s.from[1], s.at[2] - s.from[2]);
    if (d > 5.0f && (in_band(s.at) || in_band(s.from))) {
      WakeSegment g;
      std::memcpy(g.a, s.from, sizeof(g.a)), std::memcpy(g.b, s.at, sizeof(g.b));
      g.at_a = s.from_t, g.at_b = s.seen;
      w.done.push_back(g);
    }
    std::memcpy(s.from, s.at, sizeof(s.from));
    s.from_t = s.seen;
  };
  struct Seen {
    float p[3];
    bool camera;
  };
  std::vector<Seen> seen;
  // (The camera's speed over a tenth of a second or more: it moves with the game's ticks, so from one frame to the next
  // it often stands still and then jumps two frames' way, run st_wake10.)
  if (w.eye_t < 0.0 || now - w.eye_t > 1.0) {
    std::memcpy(w.eye, in.eye, sizeof(w.eye)), w.eye_t = now, w.eye_moving = false;
  } else if (now - w.eye_t >= 0.1) {
    const float dist = std::hypot(in.eye[0] - w.eye[0], in.eye[1] - w.eye[1], in.eye[2] - w.eye[2]);
    const float speed = dist / float(now - w.eye_t);
    w.eye_moving = speed > 35.0f && speed < 400.0f;
    std::memcpy(w.eye, in.eye, sizeof(w.eye)), w.eye_t = now;
  }
  if (w.eye_moving) seen.push_back({{in.eye[0], in.eye[1], in.eye[2]}, true});
  // NXR_WAKE_TEST=x,y,z,dx,dy,dz,speed,after,seconds (a developer's check): a plane that is not drawn, flown in a
  // straight line from (x, y, z) along (dx, dy, dz) at speed m/s, from `after` seconds after the first wakes were
  // asked for, for `seconds`; taken as a mover is (not the camera), so its wake is seen from wherever the camera is.
  {
    static const std::array<float, 9> test = [] {
      std::array<float, 9> q{};
      const char* v = std::getenv("NXR_WAKE_TEST");
      if (!v || std::sscanf(v, "%f,%f,%f,%f,%f,%f,%f,%f,%f", &q[0], &q[1], &q[2], &q[3], &q[4], &q[5], &q[6], &q[7], &q[8]) != 9)
        q[6] = 0.0f;
      return q;
    }();
    static const double test_from = now;
    const double t = now - test_from - test[7];
    if (test[6] > 0.0f && t >= 0.0 && t <= test[8]) {
      const float len = std::max(std::hypot(test[3], test[4], test[5]), 1e-3f);
      const float d = test[6] * float(t) / len;
      seen.push_back({{test[0] + test[3] * d, test[1] + test[4] * d, test[2] + test[5] * d}, false});
    }
  }
  {
    std::lock_guard lock(g_fog_push.mutex);
    if (std::chrono::steady_clock::now() - g_fog_push.movers_at < std::chrono::milliseconds(500))
      for (const GroundFog::Pusher& p : g_fog_push.movers) {
        const float v = std::hypot(p.velocity[0], p.velocity[1], p.velocity[2]);
        if (p.kind != 0 || p.radius < 2.0f || v < 35.0f || v > 400.0f) continue;
        // (Not the camera's own plane again.)
        if (std::hypot(p.position[0] - in.eye[0], p.position[1] - in.eye[1], p.position[2] - in.eye[2]) < 25.0f) continue;
        seen.push_back({{p.position[0], p.position[1], p.position[2]}, false});
      }
  }
  for (const Seen& s : seen) {
    WakeSource* best = nullptr;
    float best_d = 0.0f;
    for (WakeSource& src : w.sources) {
      if (src.seen >= now) continue;  // (one source a thing a frame)
      const float d = std::hypot(s.p[0] - src.at[0], s.p[1] - src.at[1], s.p[2] - src.at[2]);
      const float most = 60.0f + 400.0f * float(now - src.seen);
      if ((s.camera == src.camera) && d < most && (!best || d < best_d)) best = &src, best_d = d;
    }
    if (!best) {
      WakeSource src;
      std::memcpy(src.at, s.p, sizeof(src.at)), std::memcpy(src.from, s.p, sizeof(src.from));
      src.from_t = src.seen = now, src.camera = s.camera;
      w.sources.push_back(src);
      continue;
    }
    std::memcpy(best->at, s.p, sizeof(best->at));
    best->seen = now;
    if (std::hypot(best->at[0] - best->from[0], best->at[1] - best->from[1], best->at[2] - best->from[2]) > kPiece) close(*best);
  }
  for (size_t i = 0; i < w.sources.size();) {  // gone a second: its last piece kept, the source let go
    if (now - w.sources[i].seen > 1.0) {
      close(w.sources[i]);
      w.sources.erase(w.sources.begin() + i);
    } else {
      ++i;
    }
  }
  w.done.erase(std::remove_if(w.done.begin(), w.done.end(), [&](const WakeSegment& g) { return now - g.at_b > kKeep; }),
               w.done.end());
  // The pieces still being flown first, then the newest done ones.
  int n = 0;
  auto put = [&](const float* a, double at_a, const float* b, double at_b) {
    if (n >= Sky::Inputs::kWakeSegments) return;
    float* o = in.wake[n++];
    o[0] = a[0], o[1] = a[1], o[2] = a[2], o[3] = float(now - at_a);
    o[4] = b[0], o[5] = b[1], o[6] = b[2], o[7] = float(now - at_b);
  };
  for (const WakeSource& s : w.sources)
    if ((in_band(s.at) || in_band(s.from)) && std::hypot(s.at[0] - s.from[0], s.at[1] - s.from[1], s.at[2] - s.from[2]) > 1.0f)
      put(s.from, s.from_t, s.at, s.seen);
  for (auto it = w.done.rbegin(); it != w.done.rend(); ++it) put(it->a, it->at_a, it->b, it->at_b);
  in.wake_count = n;
  static double said = 0.0;
  if (n > 0 && now - said > 10.0) {
    said = now;
    REXLOG_INFO("[native_x_renderer] wakes (D234): {} pieces in the clouds from {} planes, the newest at {:.0f} {:.0f} {:.0f}",
                n, w.sources.size(), in.wake[0][4], in.wake[0][5], in.wake[0][6]);
  }
}
GroundFog g_ground_fog;
void StepGroundFog(IDeviceContext* context) {
  g_fog_in.ground = nullptr, g_fog_in.ground_size = 0.0f;
  g_fog_in.terrain = nullptr, g_fog_in.terrain_size = 0.0f;
  float terrain_decode = 0.0f;
  if (g_fog_terrain.valid && g_fog_terrain.height) {
    // (How the heightmap's texels are read: grass.cpp's rule.)
    const TEXTURE_FORMAT f = g_fog_terrain.height->GetDesc().Format;
    terrain_decode = !g_fog_terrain.height_unorm ? 0.0f : f == TEX_FORMAT_R16_FLOAT ? 1.0f : f == TEX_FORMAT_R16_SNORM ? 2.0f : 0.0f;
    g_fog_in.terrain = g_fog_terrain.height;
    std::memcpy(g_fog_in.terrain_scale, g_fog_terrain.scale, sizeof(g_fog_in.terrain_scale));
    std::memcpy(g_fog_in.terrain_offset, g_fog_terrain.offset, sizeof(g_fog_in.terrain_offset));
    std::memcpy(g_fog_in.terrain_uv, g_fog_terrain.uv, sizeof(g_fog_in.terrain_uv));
    g_fog_in.terrain_size = float(g_fog_terrain.height_size), g_fog_in.terrain_decode = terrain_decode;
  }
  if (!REXCVAR_GET(native_x_renderer_fog_push) || !g_device || !context) return;
  // (Only while there is a ground fog: the fog pass's sheet reads it.)
  if (g_fog_in.ground_fog <= 0.001f) return;
  static bool tried = false, made = false;
  if (!tried) tried = true, made = g_ground_fog.Initialize(g_device->device());
  static bool said_terrain = false;
  if (!said_terrain && g_fog_in.terrain) {
    said_terrain = true;
    REXLOG_INFO("[native_x_renderer] ground fog: the terrain's heightmap {} texels (read as {}), scale {:.3f} {:.3f} {:.3f}, "
                "offset {:.1f} {:.1f} {:.1f}, uv {:.6f} {:.6f} {:.4f} {:.4f}", g_fog_in.terrain_size, terrain_decode,
                g_fog_in.terrain_scale[0], g_fog_in.terrain_scale[1], g_fog_in.terrain_scale[2], g_fog_in.terrain_offset[0],
                g_fog_in.terrain_offset[1], g_fog_in.terrain_offset[2], g_fog_in.terrain_uv[0], g_fog_in.terrain_uv[1],
                g_fog_in.terrain_uv[2], g_fog_in.terrain_uv[3]);
  }
  if (!made) return;
  static const bool test = [] {  // NXR_GROUND_FOG_TEST=1 (a developer's): something circling the eye, a blast, a shot
    const char* v = std::getenv("NXR_GROUND_FOG_TEST");
    return v && *v == '1';
  }();
  static std::chrono::steady_clock::time_point last{};
  static double owed = 0.0, clock = 0.0;
  const auto now = std::chrono::steady_clock::now();
  const double frame = last.time_since_epoch().count() ? std::chrono::duration<double>(now - last).count() : 0.0;
  last = now;
  owed += frame < 0.5 ? frame : 0.0;
  constexpr double kStep = 1.0 / 60.0;  // (60 Hz: the user, 21 Nov 2026)
  auto smooth = [](float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  };
  for (int steps = 0; owed >= kStep && steps < 3; ++steps) {
    owed -= kStep, clock += kStep;
    GroundFog::Step step;
    std::memcpy(step.eye, g_fog_in.eye, sizeof(step.eye));
    // (The gusts and the rain's dimples by the match's clock, every player's the same.)
    step.dt = float(kStep), step.seconds = g_fog_in.seconds;
    if (steps == 0) {  // what stands in the fog, from this frame's view (once a frame)
      step.depth = g_fog_in.depth_srv;
      std::memcpy(step.to_ndc, g_fog_in.to_ndc, sizeof(step.to_ndc));
      std::memcpy(step.inverse, g_fog_in.inverse, sizeof(step.inverse));
      step.depth_near = g_fog_in.depth_near, step.width = g_fog_in.width, step.height = g_fog_in.height;
      step.terrain = g_fog_in.terrain;
      std::memcpy(step.terrain_scale, g_fog_in.terrain_scale, sizeof(step.terrain_scale));
      std::memcpy(step.terrain_offset, g_fog_in.terrain_offset, sizeof(step.terrain_offset));
      std::memcpy(step.terrain_uv, g_fog_in.terrain_uv, sizeof(step.terrain_uv));
      step.terrain_size = g_fog_in.terrain_size, step.terrain_decode = terrain_decode;
      float sea = 0.0f;
      if (g_own_sky.SeaLevel(&sea)) step.sea_floor = sea;
    }
    const float mist = smooth(0.45f, 1.0f, g_fog_in.weather), gale = smooth(0.75f, 1.0f, g_fog_in.weather);
    // The wind: the way the fog's patches drift (sky.cpp: its drift is 6 and 3 m a second of the clouds' clock,
    // 0.15 of it for the patches and 0.6 in a gale; they are read that far on, so they move the other way).
    const float carried = 0.15f + 0.45f * gale;
    step.wind[0] = -6.0f * carried, step.wind[1] = -3.0f * carried;
    step.gust = 0.2f + 1.2f * std::clamp(g_fog_in.wind, 0.0f, 1.0f);
    step.rain = mist;
    // (The sheet lies on the ground wherever that is, which is not known here: nothing is left out by its height.)
    step.layer_known = false;
    std::vector<GroundFog::Pusher> pushers;
    {
      std::lock_guard lock(g_fog_push.mutex);
      if (now - g_fog_push.movers_at < std::chrono::milliseconds(500)) {
        // (Each carried on from where the game's last tick left it, along its motion: the game says where they are
        // once a tick, the fog steps more often.)
        pushers = g_fog_push.movers;
        const float since = std::min(std::chrono::duration<float>(now - g_fog_push.movers_at).count(), 0.1f);
        for (GroundFog::Pusher& p : pushers)
          for (int c = 0; c < 3; ++c) p.position[c] += p.velocity[c] * since;
      }
      pushers.insert(pushers.end(), g_fog_push.once.begin(), g_fog_push.once.end());
      g_fog_push.once.clear();
    }
    if (test) {
      const float a = float(clock * 0.7);
      GroundFog::Pusher mover;
      mover.position[0] = step.eye[0] + 12.0f * std::cos(a), mover.position[1] = step.eye[1];
      mover.position[2] = step.eye[2] + 12.0f * std::sin(a);
      mover.velocity[0] = -8.4f * std::sin(a), mover.velocity[2] = 8.4f * std::cos(a);
      mover.radius = 1.8f;
      pushers.push_back(mover);
      const int tick = int(clock * 30.0 + 0.5);
      if (tick % 150 == 0) {
        GroundFog::Pusher blast;
        blast.kind = 1, blast.radius = 7.0f, blast.strength = 14.0f;
        blast.position[0] = step.eye[0] + 18.0f, blast.position[1] = step.eye[1], blast.position[2] = step.eye[2] + 6.0f;
        pushers.push_back(blast);
      }
      if (tick % 60 == 30) {
        GroundFog::Pusher shot;
        shot.kind = 2, shot.radius = 0.8f, shot.strength = 5.0f;
        shot.position[0] = step.eye[0] - 2.0f, shot.position[1] = step.eye[1], shot.position[2] = step.eye[2] + 3.0f;
        shot.velocity[0] = step.eye[0] - 30.0f, shot.velocity[1] = step.eye[1], shot.velocity[2] = step.eye[2] + 40.0f;
        pushers.push_back(shot);
      }
    }
    // (The nearest first when there are too many.)
    if (pushers.size() > size_t(GroundFog::kMaxPushers)) {
      auto away = [&](const GroundFog::Pusher& p) {
        const float dx = p.position[0] - step.eye[0], dz = p.position[2] - step.eye[2];
        return dx * dx + dz * dz;
      };
      std::sort(pushers.begin(), pushers.end(), [&](const auto& l, const auto& r) { return away(l) < away(r); });
      pushers.resize(GroundFog::kMaxPushers);
    }
    g_ground_fog.Advance(context, step, pushers.data(), int(pushers.size()));
  }
  if (owed > kStep) owed = 0.0;  // (a slow frame: not made up for)
  if (g_ground_fog.View()) {
    g_fog_in.ground = g_ground_fog.View();
    g_fog_in.ground_origin[0] = g_ground_fog.origin()[0], g_fog_in.ground_origin[1] = g_ground_fog.origin()[1];
    g_fog_in.ground_size = GroundFog::kSize;
  }
}
void ApplyFog() {
  if (!g_fog_ready) return;
  g_fog_ready = g_fog_dome = false;
  g_lens_due = true;
  if (!g_scene_color || !g_scene_depth || !g_draw || !g_scene_color->texture || !g_scene_depth->srv) return;
  RendererTime time(&g_phase.effects);
  MsaaRead(g_scene_depth);
  MsaaWrite(g_scene_color);
  g_fog_in.color = g_scene_color->texture;
  g_fog_in.depth_srv = g_scene_depth->srv;
  // The rain's cover: this frame's shadow map, if the game drew with one.
  const DrawPath::ShadowMap& shadows = g_draw->shadow_map();
  g_fog_in.cover = shadows.view && shadows.frame == g_device->frames_presented() ? shadows.view.RawPtr() : nullptr;
  std::memcpy(g_fog_in.cover_m, shadows.cascades, sizeof(g_fog_in.cover_m));
  // D190: the picture after the sea, if the sea was the last drawn (before the clouds go over it).
  if (g_draw->sea_after_due()) g_draw->TakeSeaAfter();
  // The clouds over what the view holds (planes in them, the ground under them), then the fog over all.
  const bool over = SkyOn() && Timed(SectionTimers::kCloudsOver, [&] { return g_own_sky.CloudsOver(g_device->context(), g_fog_in); });
  // D190: where the sea is drawn, from its draws drawn again (flat ground at its height is not it).
  g_own_sky.sea_mask = nullptr;
  g_own_sky.sea_after = nullptr;
  g_own_sky.sea_floor = nullptr;
  if (SeaReflection() && g_draw->sea_draw_count() > 0) {
    const TextureDesc& cd = g_scene_color->texture->GetDesc();
    if (!g_sea_mask || g_sea_mask->GetDesc().Width != cd.Width || g_sea_mask->GetDesc().Height != cd.Height) {
      g_sea_mask.Release();
      TextureDesc m;
      m.Name = "NativeXRenderer sea mask";
      m.Type = RESOURCE_DIM_TEX_2D;
      m.Width = cd.Width, m.Height = cd.Height;
      m.Format = TEX_FORMAT_R32_FLOAT;
      m.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
      g_device->device()->CreateTexture(m, nullptr, &g_sea_mask);
    }
    if (g_sea_mask) {
      ITextureView* rtv = g_sea_mask->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
      const float zero[4] = {};
      g_device->context()->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      g_device->context()->ClearRenderTarget(rtv, zero, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      if (g_scene_depth->view && g_draw->DrawGlassMask(rtv, true, g_scene_depth->view) > 0) {
        g_own_sky.sea_mask = g_sea_mask->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        g_own_sky.sea_mask_uv[0] = float(g_fog_in.width) / float(cd.Width);
        g_own_sky.sea_mask_uv[1] = float(g_fog_in.height) / float(cd.Height);
        // (Pixel for pixel with the fog's own picture, or not used.)
        ITexture* after = g_draw->sea_after();
        if (after && after->GetDesc().Width == cd.Width && after->GetDesc().Height == cd.Height)
          g_own_sky.sea_after = after->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        if (g_night_world && g_night_world_frame == g_device->frames_presented() &&
            g_night_world->GetDesc().Width == cd.Width && g_night_world->GetDesc().Height == cd.Height)
          g_own_sky.sea_floor = g_night_world->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        static bool said_after = false;
        if (!said_after) {
          said_after = true;
          REXLOG_INFO("[native_x_renderer] sea reflection: the picture after the sea {} ({}, the scene's {}x{}); the "
                      "world's depth without it {}", g_own_sky.sea_after ? "used" : "not used", after ? "taken" : "not taken",
                      cd.Width, cd.Height, g_own_sky.sea_floor ? "used" : "not used");
        }
      }
      g_draw->TargetsChanged();
      g_device->context();  // (what was recorded is done before the fog reads it)
    }
  }
  {
    // BF_FOG_AB=1 (tests): the rain's cover looked up the old way and the new by turns, 600 frames each.
    static const bool fog_ab = [] {
      const char* v = std::getenv("BF_FOG_AB");
      return v && *v == '1';
    }();
    g_fog_in.old_rain_order = fog_ab && AbPhase() == 1;
    // BF_FOG_PARTS=1 (tests): by turns of 600 frames, all of the fog pass, then without its ground fog, its rain
    // shafts, the rain here (streaks, rings, splashes), the sea's reflection: what each costs.
    static const bool fog_parts = [] {
      const char* v = std::getenv("BF_FOG_PARTS");
      return v && *v == '1';
    }();
    static const int kOff[5] = {0, 8, 16, 32, 64};
    g_fog_in.fog_parts_off = fog_parts ? kOff[(g_device->frames_presented() / 600) % 5] : 0;
    // NXR_FOG_OFF=<bits> (a developer's, D229): those parts off all the time (8 the ground fog, 16 the shafts, 32 the
    // rain here, 64 the sea's reflection).
    static const int fog_off = [] {
      const char* v = std::getenv("NXR_FOG_OFF");
      return v && *v ? std::atoi(v) : 0;
    }();
    g_fog_in.fog_parts_off |= fog_off;
  }
  {  // NXR_GROUND_FOG_HEIGHT=<metres> (a developer's, D231): the ground fog's top, whatever the match says
    static const float height = [] {
      const char* v = std::getenv("NXR_GROUND_FOG_HEIGHT");
      return v && *v ? float(std::atof(v)) : 0.0f;
    }();
    if (height > 0.0f) g_fog_in.ground_fog_height = height;
  }
  {  // NXR_GROUND_FOG_SPARSE=<0 to 1> (a developer's): how sparse it is, whatever the match says
    static const float sparse = [] {
      const char* v = std::getenv("NXR_GROUND_FOG_SPARSE");
      return v && *v ? float(std::atof(v)) : -1.0f;
    }();
    if (sparse >= 0.0f) g_fog_in.ground_fog_sparse = sparse;
  }
  StepGroundFog(g_device->context());  // D231
  {
    // D222: the buildings' rooms, as bright as the cycle is dark (none by day) and as the player's setting says.
    // BF_NIGHT_ROOMS_AB=1 (tests): with them and without by turns, 600 frames each.
    static const bool rooms_ab = [] {
      const char* v = std::getenv("BF_NIGHT_ROOMS_AB");
      return v && *v == '1';
    }();
    const float amount = float(std::clamp(REXCVAR_GET(native_x_renderer_night_rooms), 0.0, 2.0)) *
                         g_cycle_dark.load(std::memory_order_relaxed);
    g_fog_in.room_amount = rooms_ab && AbPhase() == 1 ? 0.0f : amount;
    g_fog_in.room_debug = REXCVAR_GET(native_x_renderer_night_rooms_debug);
    g_night_rooms_frame.clear();
    uint32_t down = 0;
    if (g_fog_in.room_amount > 0.001f) {
      // A room whose building has a standing part is lit while that was drawn at its place in the last 600 frames
      // (the game culls a part out of view; a destroyed house's roof is never drawn again), or while the eye is in it.
      for (size_t i = 0; i < g_night_rooms_render.size(); ++i) {
        const NightRoom& r = g_night_rooms_render[i];
        bool live = !r.intact[0] && !r.intact[1];
        if (!live && i < g_room_seen.size()) live = g_room_seen[i] && g_room_frame - g_room_seen[i] <= 600;
        if (!live) {
          bool in = true;
          for (int a = 0; a < 3 && in; ++a) {
            const float q = r.to_local[a][0] * g_fog_in.eye[0] + r.to_local[a][1] * g_fog_in.eye[1] +
                            r.to_local[a][2] * g_fog_in.eye[2] + r.to_local[a][3];
            in = std::abs(q) <= r.half[a] + 0.5f;
          }
          live = in;
        }
        if (live) g_night_rooms_frame.push_back(r);
        else ++down;
      }
    }
    static uint64_t said_at = 0;
    if (g_room_frame >= said_at + 3600 && !g_night_rooms_render.empty()) {
      said_at = g_room_frame;
      REXLOG_INFO("[native_x_renderer] night rooms (D222): {} of {} live ({} not seen standing), {} standing parts matched "
                  "since the last line", g_night_rooms_frame.size(), g_night_rooms_render.size(), down, g_room_matches);
      g_room_matches = 0;
    }
    g_fog_in.rooms = g_night_rooms_frame.data();
    g_fog_in.room_count = uint32_t(g_night_rooms_frame.size());
  }
  const bool drawn = Timed(SectionTimers::kFog, [&] { return g_own_sky.Fog(g_device->context(), g_fog_in); });
  {  // NXR_GROUND_FOG_DEBUG=1 (a developer's, D231): what stands in the fog, seen from above, over the top left corner
    static const bool fog_debug = [] {
      const char* v = std::getenv("NXR_GROUND_FOG_DEBUG");
      return v && *v == '1';
    }();
    static DebugTextureView solid_view;
    if (fog_debug && g_ground_fog.Ready() && g_ground_fog.Solid())
      solid_view.Draw(g_device->device(), g_device->context(), g_ground_fog.Solid(), 2, 0.0f, g_scene_color->texture,
                      g_scene_color->scale.HostWidth(g_profile->output_width),
                      g_scene_color->scale.HostHeight(g_profile->output_height), 420, 16);
  }
  if (drawn || over) g_draw->TargetsChanged();
  static bool said = false;
  if (!said) {
    said = true;
    REXLOG_INFO("[native_x_renderer] fog: the first pass {}: distance fog {:.2f}, ground fog {:.2f}, weather {:.2f}; the "
                "eye at {:.0f}, {:.0f}, {:.0f}", drawn ? "drawn" : "not drawn", g_fog_in.fog, g_fog_in.ground_fog,
                g_fog_in.weather, g_fog_in.eye[0], g_fog_in.eye[1], g_fog_in.eye[2]);
  }
}

void ExecuteResolve(const Command& c) {
  RendererTime time(&g_phase.resolves);
  uint32_t flags = c.r[1], level = c.r[5], slice = c.r[6];
  GuestSurface surface = Tiled(c.surfaces[0]);
  if (c.caller == g_profile->upscale_resolve_caller && surface && !surface.depth &&
      surface.width == g_profile->output_width && surface.height == g_profile->output_height) {
    if (g_sky_late) ApplyLateSky();
    if (g_fog_ready) ApplyFog();
    // Rain on the lens, over the finished view (once a frame: the fog's pass of this frame keeps how wet it is).
    const bool drops = REXCVAR_GET(native_x_renderer_rain_lens);
    // D191: the window glass's mask, drawn from this frame's kept glass draws, for its reflection with the lens.
    Sky::GlassInputs glass;
    bool glassy = false;
    if (g_lens_due && g_scene_color && g_scene_color->texture && g_draw && g_scene_depth && g_scene_depth->srv &&
        SeaReflection() && REXCVAR_GET(native_x_renderer_glass_reflections) && g_draw->glass_draw_count() > 0 && g_night_world &&
        g_night_world_frame == g_device->frames_presented()) {
      const TextureDesc& cd = g_scene_color->texture->GetDesc();
      if (!g_glass_mask || g_glass_mask->GetDesc().Width != cd.Width || g_glass_mask->GetDesc().Height != cd.Height) {
        g_glass_mask.Release();
        TextureDesc m;
        m.Name = "NativeXRenderer glass mask";
        m.Type = RESOURCE_DIM_TEX_2D;
        m.Width = cd.Width, m.Height = cd.Height;
        m.Format = TEX_FORMAT_R32_FLOAT;
        m.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
        g_device->device()->CreateTexture(m, nullptr, &g_glass_mask);
      }
      if (g_glass_mask) {
        ITextureView* rtv = g_glass_mask->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
        const float zero[4] = {};
        g_device->context()->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        g_device->context()->ClearRenderTarget(rtv, zero, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        if (g_draw->DrawGlassMask(rtv) > 0) {
          const float view_w = float(g_scene_color->scale.HostWidth(g_profile->output_width));
          const float view_h = float(g_scene_color->scale.HostHeight(g_profile->output_height));
          const TextureDesc& dd = g_scene_depth->texture->GetDesc();
          glass.mask = g_glass_mask->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
          glass.world_depth = g_night_world->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
          glass.view_depth = g_scene_depth->srv;
          glass.world_uv[0] = view_w / float(g_night_world->GetDesc().Width);
          glass.world_uv[1] = view_h / float(g_night_world->GetDesc().Height);
          glass.view_uv[0] = view_w / float(dd.Width), glass.view_uv[1] = view_h / float(dd.Height);
          glassy = true;
        }
        g_draw->TargetsChanged();
      }
    }
    // D195: in a vehicle, the depths for the drops to keep to its glass.
    if (!glassy && g_night_vehicle_seated.load(std::memory_order_relaxed) && g_scene_color && g_scene_depth &&
        g_scene_depth->srv && g_night_world && g_night_world_frame == g_device->frames_presented()) {
      const float view_w = float(g_scene_color->scale.HostWidth(g_profile->output_width));
      const float view_h = float(g_scene_color->scale.HostHeight(g_profile->output_height));
      const TextureDesc& dd = g_scene_depth->texture->GetDesc();
      glass.world_depth = g_night_world->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
      glass.view_depth = g_scene_depth->srv;
      glass.world_uv[0] = view_w / float(g_night_world->GetDesc().Width);
      glass.world_uv[1] = view_h / float(g_night_world->GetDesc().Height);
      glass.view_uv[0] = view_w / float(dd.Width), glass.view_uv[1] = view_h / float(dd.Height);
    }
    glass.cockpit = g_night_vehicle_seated.load(std::memory_order_relaxed);
    if (g_lens_due && g_scene_color && g_scene_color->texture && g_draw &&
        ((drops && g_own_sky.lens_wet() >= 0.01f) || g_own_sky.sway() >= 0.005f || glassy || g_own_sky.shoving())) {
      g_lens_due = false;
      RendererTime time(&g_phase.effects);
      MsaaWrite(g_scene_color);
      if (Timed(SectionTimers::kLens, [&] {
            return g_own_sky.Lens(g_device->context(), g_scene_color->texture,
                                  g_scene_color->scale.HostWidth(g_profile->output_width),
                                  g_scene_color->scale.HostHeight(g_profile->output_height), drops,
                                  glassy || glass.world_depth ? &glass : nullptr);
          })) {
        g_draw->TargetsChanged();
      }
    }
  }
  // D229: the storm's clouds stepped once a frame (at the game's upscale point, as the test below).
  if (g_device && c.caller == g_profile->upscale_resolve_caller && surface && !surface.depth) {
    StepStormClouds(g_device->context());
    StepCloudMass(g_device->context());  // D232
    {  // NXR_CLOUD_MASS_DEBUG=1 (a developer's): the clouds' mass seen from above, over the picture's top left corner
      static const bool mass_debug = [] {
        const char* v = std::getenv("NXR_CLOUD_MASS_DEBUG");
        return v && *v == '1';
      }();
      static DebugTextureView mass_view;
      if (mass_debug && g_cloud_mass.Ready() && g_cloud_mass.View() && g_scene_color && g_scene_color->texture && g_draw) {
        MsaaWrite(g_scene_color);
        mass_view.Draw(g_device->device(), g_device->context(), g_cloud_mass.View(), 2, 0.0f, g_scene_color->texture,
                       g_scene_color->scale.HostWidth(g_profile->output_width),
                       g_scene_color->scale.HostHeight(g_profile->output_height), 520, 16);
        g_draw->TargetsChanged();
      }
    }
    // NXR_STORM_FLUID_DEBUG=1 (a developer's): the storm's fluid density over the picture's top left corner.
    static const bool storm_debug = [] {
      const char* v = std::getenv("NXR_STORM_FLUID_DEBUG");
      return v && *v == '1';
    }();
    if (storm_debug && g_storm_clouds.on && g_storm_clouds.sim.Ready() && g_scene_color && g_scene_color->texture && g_draw) {
      MsaaWrite(g_scene_color);
      g_storm_clouds.sim.DrawDebug(g_device->context(), g_scene_color->texture,
                                   g_scene_color->scale.HostWidth(g_profile->output_width),
                                   g_scene_color->scale.HostHeight(g_profile->output_height), 640);
      g_draw->TargetsChanged();
    }
  }
  // D227: the fluid core's test (NXR_FLUID_TEST=1, a developer's): a 2D storm of 16,384 particles drawn toward its low
  // and turned by the Coriolis term, tracers fed on a ring round it, 30 steps a second, its density over the
  // picture's top left corner and its GPU time in the log.
  {
    static const bool fluid_test = [] {
      const char* v = std::getenv("NXR_FLUID_TEST");
      return v && *v == '1';
    }();
    if (fluid_test && g_scene_color && g_scene_color->texture && g_device && g_draw &&
        c.caller == g_profile->upscale_resolve_caller && surface && !surface.depth) {
      static FluidSim sim;
      static bool tried = false, seeded = false;
      static auto last = std::chrono::steady_clock::now();
      static double owed = 0.0;
      if (!tried) {
        tried = true;
        FluidSim::Config config;
        config.spacing = 24.0f;
        sim.Initialize(g_device->device(), config, "test");
      }
      if (sim.Ready()) {
        IDeviceContext* context = g_device->context();
        const float centre[3] = {0.0f, 0.0f, 0.0f};
        if (!seeded) sim.Seed(context, centre, 0.0f), seeded = true;
        const auto now = std::chrono::steady_clock::now();
        owed = std::min(owed + std::chrono::duration<double>(now - last).count(), 0.25);
        last = now;
        constexpr float kStep = 1.0f / 30.0f;
        FluidSim::Forces forces;
        forces.pull = 0.5f, forces.reach = 2200.0f, forces.coriolis = 0.03f, forces.drag = 0.02f;
        forces.sink = 120.0f, forces.source = 1700.0f;
        forces.vorticity = 2.0f, forces.viscosity = 0.02f, forces.tracer_life = 60.0f, forces.splat_weight = 1.0f;
        FluidSim::Emitter ring[8];
        for (int e = 0; e < 8; ++e) {
          const float a = float(e) * 0.785398f;
          ring[e].position[0] = std::cos(a) * 1200.0f, ring[e].position[2] = std::sin(a) * 1200.0f;
          ring[e].radius = 80.0f;
        }
        while (owed >= kStep) {
          owed -= kStep;
          sim.Step(context, kStep, forces, ring, 8, 16);  // (32,768 slots: each tracer lives its 60 s)
          static uint32_t stepped = 0;
          if (++stepped % 300 == 1) sim.LogState(context);
        }
        MsaaWrite(g_scene_color);
        sim.DrawDebug(context, g_scene_color->texture, g_scene_color->scale.HostWidth(g_profile->output_width),
                      g_scene_color->scale.HostHeight(g_profile->output_height), 640);
        g_draw->TargetsChanged();
      }
    }
  }
  // D228: the storm's radar pattern over the picture's top left corner, its frames in turn a second each
  // (NXR_STORM_PATTERN_DEBUG=1, a developer's).
  {
    static const bool pattern_debug = [] {
      const char* v = std::getenv("NXR_STORM_PATTERN_DEBUG");
      return v && *v == '1';
    }();
    if (pattern_debug && g_scene_color && g_scene_color->texture && g_device && g_draw &&
        c.caller == g_profile->upscale_resolve_caller && surface && !surface.depth) {
      if (ITexture* pattern = StormPatternTexture()) {
        static DebugTextureView view;
        static const auto start = std::chrono::steady_clock::now();
        const uint32_t frames = pattern->GetDesc().ArraySize;
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        MsaaWrite(g_scene_color);
        view.Draw(g_device->device(), g_device->context(), pattern->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE), true,
                  float(uint64_t(t) % std::max(frames, 1u)), g_scene_color->texture,
                  g_scene_color->scale.HostWidth(g_profile->output_width),
                  g_scene_color->scale.HostHeight(g_profile->output_height), 640, 680);
        g_draw->TargetsChanged();
      }
    }
  }
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
  MsaaRead(source);  // host MSAA: the copy reads the one-sample texture
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
  if ((NightPassOn() || SeaReflection()) && c.caller == g_profile->ao_resolve_caller && surface.depth && source &&
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
    MsaaRead(source);
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
  g_device->recorder().Do([srv = source->srv, kind, from, target_view, to, options](IDeviceContext* context) {
    g_blit.Copy(context, srv, kind, from, target_view, to, options);
  });
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
  if (NightPassOn() && REXCVAR_GET(native_x_renderer_night_glow) > 0.0 && !source->depth &&
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
    g_sections.Begin(g_device->device(), g_device->context(), SectionTimers::kAa);
    g_aa.Apply(g_device->context(), target->texture, target_view);
    g_sections.End(g_device->context(), SectionTimers::kAa);
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
  if (NightPassOn() && c.scene_caller && slice == 0 &&
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
    memory->EnsureMirror(write.address, uint32_t(std::min<uint64_t>(uint64_t((write.pitch + 31) & ~31u) * ((write.height + 31) & ~31u) * 16, 0x20000000u)));  // D174
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
    // With host MSAA the view's draws count every sample of a pixel.
    g_queries.SetSampleScale(double(scale.x) * double(scale.y) * double(MsaaSetting()));
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
    MsaaWrite(output);
    MsaaWrite(g_scene_color);
    MsaaRead(g_scene_depth);
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
    upscaled = output && Timed(SectionTimers::kUpscale, [&] { return g_upscaler.Apply(g_device->context(), g_scene_color->texture, g_scene_depth->texture,
                                          g_scene_depth->srv, output->texture, g_scene_view_projection,
                                          g_draw ? g_draw->jittered_z_min() : 0.0f,
                                          g_draw ? g_draw->jittered_z_max() : 1.0f, [](ITextureView* motion) {
                                            if (g_draw) g_draw->DrawObjectMotion(motion, g_scene_depth->view);
                                          }, voted ? &vote : nullptr); });
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
      MsaaWrite(blurred);
      MsaaRead(g_scene_depth);
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
      // Not while the game says so (SetObjectMotionBlurPaused: a vehicle's outside view turned round it by free look).
      if (REXCVAR_GET(native_x_renderer_motion_blur_objects) && !ObjectMotionBlurPaused())
        in.object_motion = [](ITextureView* motion) {
          if (g_draw) g_draw->DrawObjectMotion(motion, g_scene_depth->view);
        };
      in.strength = float(std::clamp(REXCVAR_GET(native_x_renderer_motion_blur_strength), 0.0, 100.0) / 100.0);
      in.samples = taps;
      in.frame_seconds = seconds;
      if (Timed(SectionTimers::kMotionBlur, [&] { return g_motion_blur.Apply(g_device->context(), in); })) g_draw->TargetsChanged();
    }
  }
  g_blur_voted = false;
  // Depth of field (dof.h): while the player aims down the sights, after the motion blur.
  if (const float ads = REXCVAR_GET(native_x_renderer_ads_dof) ? AdsBlendNow() : 0.0f;
      ads > 0.01f && g_scene_color && g_scene_depth && g_scene_depth->srv && g_draw && g_dof_state >= 0 &&
      !(g_queries_on && g_queries.open())) {
    if (g_dof_state == 0) g_dof_state = g_dof.Load(g_device->device()) ? 1 : -1;
    HostTarget* soft = upscaled && g_scene_color->reduced ? g_targets.Get(g_scene_color_surface) : g_scene_color;
    if (g_dof_state == 1 && soft && soft->texture) {
      MsaaWrite(soft);
      MsaaRead(g_scene_depth);
      const std::string quality = REXCVAR_GET(native_x_renderer_dof_quality);
      DepthOfField::Inputs in;
      in.color = soft->texture;
      in.depth = g_scene_depth->texture;
      in.depth_srv = g_scene_depth->srv;
      in.width = soft->scale.HostWidth(g_profile->output_width);
      in.height = soft->scale.HostHeight(g_profile->output_height);
      in.depth_width = g_scene_depth->scale.HostWidth(g_profile->output_width);
      in.depth_height = g_scene_depth->scale.HostHeight(g_profile->output_height);
      in.blend = ads;
      in.taps = quality == "low" ? 24 : quality == "high" ? 80 : 48;
      if (Timed(SectionTimers::kDof, [&] { return g_dof.Apply(g_device->context(), in); })) g_draw->TargetsChanged();
    }
  }
  // A developer's weapon export (weapon_export.h): its textures read back here, where the context is used directly.
  if (WeaponExportOn()) WeaponExportFlush(g_device->device(), g_device->context());
  // The player's weapon skin's subtle blue bloom round its arcs (weapon_skin.h), after the depth of field.
  if (WeaponSkinNow() && g_scene_color && g_scene_depth && g_scene_depth->srv && !(g_queries_on && g_queries.open())) {
    HostTarget* soft = upscaled && g_scene_color->reduced ? g_targets.Get(g_scene_color_surface) : g_scene_color;
    if (soft && soft->texture) {
      MsaaWrite(soft);
      MsaaRead(g_scene_depth);
      WeaponSkinBloomInputs in;
      in.color = soft->texture;
      in.depth_srv = g_scene_depth->srv;
      in.width = soft->scale.HostWidth(g_profile->output_width);
      in.height = soft->scale.HostHeight(g_profile->output_height);
      in.depth_width = g_scene_depth->scale.HostWidth(g_profile->output_width);
      in.depth_height = g_scene_depth->scale.HostHeight(g_profile->output_height);
      in.ads = AdsBlendNow();
      in.dark = NightLightsAmount();
      if (g_draw) in.px = g_draw->weapon_px(), in.py = g_draw->weapon_py();
      // The storm's light on the gun's own mesh: the gun's draws again, into a target of the depth's size.
      if (g_draw && g_scene_depth->view && g_scene_depth->texture) {
        const TextureDesc& depth_size = g_scene_depth->texture->GetDesc();
        if (ITextureView* over = WeaponSkinOverTarget(g_device->device(), depth_size.Width, depth_size.Height))
          g_draw->DrawSkinOver(over, g_scene_depth->view);
      }
      // By night the overlay goes over the picture after the night pass instead (at the game's composite, below):
      // before it, the night's lamps lit the cloud as a surface, a tan haze brighter than the gun (run skin15_night).
      if (NightPassOn()) {
        in.color = nullptr;
        in.after_night = true;
        g_skin_overlay = in;
        g_skin_overlay_frame = g_device->frames_presented();
      } else if (Timed(SectionTimers::kSkinBloom, [&] { return WeaponSkinBloom(g_device->device(), g_device->context(), in); })) {
        g_draw->TargetsChanged();
      }
    }
  }
  PromoteReduced(upscaled ? g_scene_color : nullptr);
  // D162: the view's depth for the night at the scene's copy, which follows in this frame.
  // D168: the picture in picture scope's too, to leave the first person weapon out of the view it remembers.
  if ((NightPassOn() || g_scope_watch) && g_scene_depth && g_scene_depth->srv &&
      g_scene_depth->texture) {
    const TextureDesc& depth = g_scene_depth->texture->GetDesc();
    MsaaRead(g_scene_depth);
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
    ITextureView* back_buffer = g_device->BackBuffer();  // first: a new swap chain replaces the description (issue 26)
    const auto& desc = g_device->swap_chain()->GetDesc();
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
    auto* back_buffer = g_device->BackBuffer();  // first: a new swap chain replaces the description (issue 26)
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
  // Host MSAA: what it copies in a frame, said now and then.
  if (MsaaSetting() > 1) {
    static uint64_t frames = 0, downs = 0, ups = 0;
    if (++frames % 1800 == 0) {
      REXLOG_INFO("[native_x_renderer] MSAA {}x: {:.2f} copies down and {:.2f} up per frame over the last 1800",
                  MsaaSetting(), double(g_msaa_downs - downs) / 1800.0, double(g_msaa_ups - ups) / 1800.0);
      downs = g_msaa_downs, ups = g_msaa_ups;
    }
    if (g_msaa_trace)
      REXLOG_INFO("[native_x_renderer] MSAA trace: present (after {} multisampled views, {} one-sample)",
                  g_msaa_trace_ms, g_msaa_trace_single);
    static const bool trace = std::getenv("NXR_MSAA_TRACE") != nullptr;  // a developer's: one frame in 1800 logged
    g_msaa_trace = trace && frames % 1800 == 900;
    g_msaa_trace_ms = g_msaa_trace_single = 0;
  }
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
  g_fog_ready = g_fog_dome = g_lens_due = g_sky_late = false;
  g_terrain.Reset();
  g_scene_antialiased = false;
  g_night_pending = false;  // a frame whose composite never came
  g_night_glow_dome_into = nullptr;
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
    // D222: the rooms, and their buildings' standing parts for the draw path.
    if (g_night_rooms_render_version != g_night_rooms_version) {
      g_night_rooms_render_version = g_night_rooms_version;
      g_night_rooms_render = g_night_rooms;
      g_room_seen.assign(g_night_rooms_render.size(), 0);
      g_room_intact_counts.clear();
      for (const NightRoom& r : g_night_rooms_render)
        for (uint32_t c : r.intact)
          if (c) g_room_intact_counts.push_back(c);
      std::sort(g_room_intact_counts.begin(), g_room_intact_counts.end());
      g_room_intact_counts.erase(std::unique(g_room_intact_counts.begin(), g_room_intact_counts.end()), g_room_intact_counts.end());
    }
  }
  g_room_frame = g_device->frames_presented() + 1;  // (the next frame's draws come with this number)
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
  // With the guest's swaps (D86): composed into a frame image now, shown when the command processor executes the
  // frame's swap, which comes after this command has run (the swap passes the signal gate).
  // Frame generation (D123); not with the guest's swaps (D86) for now.
  if (g_frame_generation && !with_swap && PresentWithGeneratedFrame(c)) return;
  FrameImage* image = with_swap ? NextFrameImage() : nullptr;
  // The back buffer before the swap chain's description: a new swap chain replaces it (issue 26).
  ITextureView* target = image ? image->rtv : g_device->BackBuffer();
  const auto& desc = g_device->swap_chain()->GetDesc();
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

enum ReplayState : uint32_t { kReplayIdle, kReplayQueued, kReplayRunning };
struct ReplaySlot {
  explicit ReplaySlot(const Profile& profile) : tracker(profile) {}
  StateTracker tracker;
  StateTracker::ReplayJob job;
  std::array<GuestSurface, 5> bound{};
  std::atomic<uint64_t> seq{0};
  std::atomic<uint32_t> state{kReplayIdle}, pending{0};
  std::atomic<bool> credited{false};
};
std::vector<ReplaySlot*> g_replay_slots;
std::atomic<uint64_t> g_replays_dispatched{0}, g_replays_enqueued{0};
std::atomic<uint32_t> g_replay_burst{0}, g_replay_sleepers{0}, g_replay_wake{0};
std::mutex g_replay_mutex;
std::condition_variable g_replay_cv;
std::vector<uint32_t> g_replay_carry;
const StateTracker::Program* g_replay_programs[2] = {};
const uint8_t* g_replay_base = nullptr;
thread_local bool t_replay_enqueuing = false;

void EnqueueReplay(ReplaySlot& s);

bool CreditReplay(ReplaySlot& s) {
  return !s.credited.exchange(true) && s.pending.fetch_sub(1) == 1;
}

void FinishReplay(uint32_t i) {
  if (g_replay_slots[i]->pending.fetch_sub(1) != 1) return;
  t_replay_enqueuing = true;
  for (;;) {
    ReplaySlot& s = *g_replay_slots[i];
    const uint64_t seq = s.seq.load();
    EnqueueReplay(s);
    s.state.store(kReplayIdle);
    g_replays_enqueued.store(seq);
    if (++i >= g_replay_burst.load() || g_replay_slots[i]->seq.load() != seq + 1 || !CreditReplay(*g_replay_slots[i])) break;
  }
  t_replay_enqueuing = false;
}

bool RunOneReplay() {
  const uint32_t burst = g_replay_burst.load();
  for (uint32_t i = 0; i < burst; ++i) {
    ReplaySlot& s = *g_replay_slots[i];
    uint32_t queued = kReplayQueued;
    if (!s.state.compare_exchange_strong(queued, kReplayRunning)) continue;
    s.tracker.RunReplay(s.job, g_replay_base);
    FinishReplay(i);
    return true;
  }
  return false;
}

std::mutex& WaitReplays() {
  if (!t_replay_enqueuing) {
    const uint64_t target = g_replays_dispatched.load();
    const bool spin = SpinWaitsAllowed();
    while (g_replays_enqueued.load() < target) {
      if (RunOneReplay()) continue;
      if (spin) {
        Pause();
      } else {
        std::this_thread::yield();
      }
    }
  }
  return g_capture_mutex;
}

// One command: filled, then published when it goes out of scope.
class Capture {
 public:
  explicit Capture(Command::Type type) : lock_(WaitReplays()), command(g_queue.Next()) {
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

// A draw replayed from a recorded command buffer renders into the surfaces bound on the device that runs the
// buffer, as on the console, where the buffer holds draws and their state but not the render targets (D131). Only
// when that device has none bound, the ones the recording device had (D129's snapshot).
void UseRecordedSurfaces(const TrackedCall& tracked, std::array<GuestSurface, 5>& surfaces) {
  if (!tracked.replayed) return;
  const bool recorded_targets = REXCVAR_GET(native_x_renderer_replay_recorded_targets);
  for (const GuestSurface& s : surfaces) {
    if (s.object && !recorded_targets) return;
  }
  std::array<SurfaceSnapshot, 5> recorded;
  if (!FindRecordedDraw(tracked.begin, recorded)) return;
  // The surfaces the recording device had bound for it.
  for (uint32_t i = 0; i < 5; ++i) {
    const SurfaceSnapshot& r = recorded[i];
    GuestSurface& s = surfaces[i];
    s = {};
    if (!r.object) continue;
    s.object = r.object;
    s.surface_info = r.surface_info;
    s.info = r.info;
    s.width = r.width;
    s.height = r.height;
    s.depth = i == 4;
  }
}

void CaptureDraw(const TrackedCall& tracked, StateTracker& tracker, const uint8_t* base) {
  Capture capture(Command::Type::kDraw);
  Command& c = capture.command;
  c.call = tracked.call;
  tracker.TakeRecord(c.registers);
  for (uint32_t i = 0; i < 5; ++i) c.surfaces[i] = ReadBoundSurface(base, tracked.device, i);
  UseRecordedSurfaces(tracked, c.surfaces);
  c.programs[0] = tracker.vertex_program_ptr();
  c.programs[1] = tracker.pixel_program_ptr();
  if ((Info(tracked.call).flags & kInlineEnd) || tracked.call == Call::kDrawVerticesUP) {
    CaptureInlineData(tracker, base, c);
  }
}

void EnqueueReplay(ReplaySlot& w) {
  StateTracker::ReplayJob& job = w.job;
  TrackedCall draw{.call = job.call, .draw = true, .device = job.device, .replayed = true};
  for (size_t i = 0; i < job.draw_count; ++i) {
    StateTracker::ReplayDraw& d = job.draws[i];
    d.record.insert(d.record.begin(), g_replay_carry.begin(), g_replay_carry.end());
    g_replay_carry.clear();
    for (int t = 0; t < 2; ++t) g_replay_programs[t] = d.programs[t] ? d.programs[t] : g_replay_programs[t];
    draw.begin = d.begin;
    Capture capture(Command::Type::kDraw);
    Command& c = capture.command;
    c.call = job.call;
    std::swap(c.registers, d.record);
    c.surfaces = w.bound;
    UseRecordedSurfaces(draw, c.surfaces);
    c.programs[0] = g_replay_programs[0];
    c.programs[1] = g_replay_programs[1];
  }
  g_replay_carry.insert(g_replay_carry.end(), job.record.begin(), job.record.end());
  if (w.tracker.vertex_program_ptr()) g_replay_programs[0] = w.tracker.vertex_program_ptr();
  if (w.tracker.pixel_program_ptr()) g_replay_programs[1] = w.tracker.pixel_program_ptr();
}

bool ReplayQueued() {
  const uint32_t burst = g_replay_burst.load();
  for (uint32_t i = 0; i < burst; ++i) {
    if (g_replay_slots[i]->state.load() == kReplayQueued) return true;
  }
  return false;
}

void RunReplayWorker() {
  rex::thread::set_current_thread_name("NXRReplay");
  for (;;) {
    const uint32_t wake = g_replay_wake.load();
    for (uint32_t i = 0; SpinWaitsAllowed() && i < 4096 && !ReplayQueued(); ++i) Pause();
    if (RunOneReplay()) continue;
    std::unique_lock lock(g_replay_mutex);
    g_replay_sleepers.fetch_add(1);
    g_replay_cv.wait_for(lock, std::chrono::milliseconds(5), [&] { return ReplayQueued() || g_replay_wake.load() != wake; });
    g_replay_sleepers.fetch_sub(1);
  }
}

void WakeReplayWorkers(bool all) {
  if (!g_replay_sleepers.load()) return;
  std::lock_guard lock(g_replay_mutex);
  if (all) {
    g_replay_wake.fetch_add(1);
    g_replay_cv.notify_all();
  } else {
    g_replay_cv.notify_one();
  }
}

void SyncReplays(StateTracker& tracker) {
  const uint32_t burst = g_replay_burst.load();
  if (!burst) return;
  WaitReplays();
  for (uint32_t i = 0; i < burst; ++i) tracker.MergeReplay(g_replay_slots[i]->tracker);
  tracker.GiveRecord(g_replay_carry);
  g_replay_burst.store(0);
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
void WaitForFullscreenSize(rex::ui::Window* window, Device& device) {
  SDL_Window* sdl = static_cast<rex::ui::WindowSDL*>(window)->sdl_window();
  if (!sdl || !window->IsFullscreen()) return;
  const bool synced = SDL_SyncWindow(sdl);
  int width = 0, height = 0;
  if (!SDL_GetWindowSizeInPixels(sdl, &width, &height) || width <= 0 || height <= 0) return;
  REXLOG_INFO("[native_x_renderer] the window at {}x{} for the render scale ({})", width, height,
              synced ? "synced" : "SDL_SyncWindow timed out");
  device.swap_chain()->Resize(uint32_t(width), uint32_t(height));
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
#if !defined(_WIN32)
  if (static_cast<const HostWindow*>(native_window)->wayland_surface) WaitForFullscreenSize(window, *device);
#endif
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
  // (NGX runs on RTX cards only). Not asked when the game offers none (native_x_renderer_check_temporal, D171).
  if (!REXCVAR_GET(native_x_renderer_check_temporal)) {
    g_temporal_support = 0;
    REXLOG_INFO("[native_x_renderer] temporal upscalers: not checked (native_x_renderer_check_temporal off)");
  } else {
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
  if (g_present_with_swaps) poller = [] {
    g_device->recorder().BeginInline();
    PresentPending();
    g_device->recorder().EndInline();
  };
  const bool raise = REXCVAR_GET(native_x_renderer_raise_priority);
  g_queue.SetRaisePriority(raise);
  if (threaded && REXCVAR_GET(native_x_renderer_record_thread)) {
    const uint32_t cores = AvailableCores(), need = uint32_t(std::max(0, REXCVAR_GET(native_x_renderer_record_thread_cores)));
    if (cores >= need) g_device->recorder().Start();
    // D229: the race check's self-test (NXR_RACE_SELFTEST=1, with NXR_RACE_CHECK=1).
    if (const char* t = std::getenv("NXR_RACE_SELFTEST"); t && *t == '1') g_device->RaceSelfTest();
    REXLOG_INFO("[native_x_renderer] recorder thread {} ({} cores, at least {} for it); waits {}",
                cores >= need ? "on" : "off", cores, need, SpinWaitsAllowed() ? "spin, then sleep" : "sleep at once");
  }
  g_queue.Start(threaded, &Execute, poller);
  if (threaded) {
    // The gate is called on the command processor's thread: its priority raised there, the first time (D136).
    static bool raise_gate = false;
    raise_gate = raise;
    graphics->SetSignalGate(
        [](void*, uint64_t tag) {
          static thread_local bool raised = false;
          if (raise_gate && !raised) raised = true, RaiseCurrentThreadPriority();
          g_queue.WaitIdle(tag);
        },
        nullptr, [](void*) {
          WaitReplays();
          return g_queue.Produced();
        });
    const int32_t replay_threads = REXCVAR_GET(native_x_renderer_replay_threads);
    for (int32_t i = 0; replay_threads > 0 && i <= replay_threads; ++i) g_replay_slots.push_back(new ReplaySlot(*g_profile));
    for (int32_t i = 0; i < replay_threads; ++i) std::thread(RunReplayWorker).detach();
    std::atexit([] {
      g_queue.Stop();
      if (g_device) g_device->recorder().Stop();
    });
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
  if ((Info(call).flags & kReplay) && !g_replay_burst.load()) WakeReplayWorkers(true);
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

bool DispatchReplay(Call call, StateTracker& tracker, const uint8_t* base) {
  if (g_replay_slots.empty()) return false;
  tracker.set_sync(&SyncReplays);
  if (g_replay_burst.load() == g_replay_slots.size()) SyncReplays(tracker);
  const uint32_t i = g_replay_burst.load();
  ReplaySlot& s = *g_replay_slots[i];
  if (!tracker.TakeReplay(call, base, s.job)) return false;
  if (!i) {
    g_replay_programs[0] = tracker.vertex_program_ptr();
    g_replay_programs[1] = tracker.pixel_program_ptr();
  }
  g_replay_base = base;
  for (uint32_t k = 0; k < 5; ++k) s.bound[k] = ReadBoundSurface(base, s.job.device, k);
  const uint64_t seq = g_replays_dispatched.load() + 1;
  s.seq.store(seq);
  s.pending.store(i ? 2 : 1);
  s.credited.store(i == 0);
  s.state.store(kReplayQueued);
  g_replay_burst.store(i + 1);
  g_replays_dispatched.store(seq);
  if (i && g_replays_enqueued.load() >= seq - 1) CreditReplay(s);
  WakeReplayWorkers(false);
  return true;
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

void NightVehicleSeated(bool seated) { g_night_vehicle_seated.store(seated, std::memory_order_relaxed); }
void SetStormPattern(const StormPattern& pattern) {
  if (!pattern.frames || !pattern.width || !pattern.height || !pattern.count || pattern.count > 256) return;
  std::lock_guard lock(g_storm_pattern_mutex);
  const size_t bytes = size_t(pattern.width) * pattern.height * pattern.count;
  g_storm_pattern_bytes.assign(pattern.frames, pattern.frames + bytes);
  g_storm_pattern_meta = pattern;
  g_storm_pattern_meta.frames = nullptr;
  g_storm_pattern_new = true;
}
void SetMapArea(float x, float z, float reach) {
  g_map_x.store(x, std::memory_order_relaxed), g_map_z.store(z, std::memory_order_relaxed);
  if (reach > 0.0f) g_map_reach.store(reach, std::memory_order_relaxed);
  g_map_known.store(true, std::memory_order_relaxed);
}
void SetPlayerGroups(const float* xz, uint32_t count) { SetStrikeGroups(xz, count); }
void SetFogMovers(const FogMover* movers, uint32_t count) {  // D231
  std::lock_guard lock(g_fog_push.mutex);
  g_fog_push.movers.clear();
  for (uint32_t i = 0; movers && i < count && i < uint32_t(GroundFog::kMaxPushers); ++i) {
    GroundFog::Pusher p;
    std::memcpy(p.position, movers[i].position, sizeof(p.position));
    std::memcpy(p.velocity, movers[i].velocity, sizeof(p.velocity));
    p.radius = movers[i].radius;
    g_fog_push.movers.push_back(p);
  }
  g_fog_push.movers_at = std::chrono::steady_clock::now();
}
void FogBlast(const float position[3], float radius, float strength) {
  GroundFog::Pusher p;
  p.kind = 1, p.radius = radius, p.strength = strength;
  std::memcpy(p.position, position, sizeof(p.position));
  std::lock_guard lock(g_fog_push.mutex);
  if (g_fog_push.once.size() < 16) g_fog_push.once.push_back(p);
}
void FogShot(const float from[3], const float to[3], float strength) {
  GroundFog::Pusher p;
  p.kind = 2, p.radius = 0.8f, p.strength = strength;
  std::memcpy(p.position, from, sizeof(p.position));
  std::memcpy(p.velocity, to, sizeof(p.velocity));
  std::lock_guard lock(g_fog_push.mutex);
  if (g_fog_push.once.size() < 16) g_fog_push.once.push_back(p);
}
void NightModels(const NightModelLamps* models, uint32_t count) {
  std::lock_guard lock(g_night_lamps_mutex);
  if (models && count) g_night_models.assign(models, models + count);
  else g_night_models.clear();
  ++g_night_models_version;
  REXLOG_INFO("[native_x_renderer] night: {} models with lamps (D163)", g_night_models.size());
}
void NightRooms(const NightRoom* rooms, uint32_t count) {
  std::lock_guard lock(g_night_lamps_mutex);
  if (rooms && count) g_night_rooms.assign(rooms, rooms + count);
  else g_night_rooms.clear();
  ++g_night_rooms_version;
  REXLOG_INFO("[native_x_renderer] night: {} rooms of the level's buildings (D222)", g_night_rooms.size());
}

void SetWorldState(const WorldState& state) {
  std::lock_guard lock(g_world_mutex);
  g_world_set = state, g_world_set_at = std::chrono::steady_clock::now();
}
void SnapWorld(double seconds) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                             std::chrono::duration<double>(std::max(seconds, 0.0)));
  g_world_snap_until.store(until.time_since_epoch().count(), std::memory_order_relaxed);
  g_world_snaps.fetch_add(1, std::memory_order_relaxed);
}
WorldState CurrentWorldState() { return World(); }

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

// A lost device (TekRantGaming, 21 Nov 2026: "it froze my game when i loaded into the server"; NVIDIA's driver reset
// the GPU, nvlddmkm event 153). DiligentCore ignores Vulkan's VK_ERROR_DEVICE_LOST in release builds: its waits return
// at once, so frames look complete and the game runs on, while nothing new is drawn (a frozen picture) and the command
// queue's fence stops. Checked every 30 frames: the queue's completed fence has not moved for 3 s while 60 or more
// frames were presented and more than 64 submissions wait. The game reads DeviceLost() and tells the player.
// NXR_TEST_DEVICE_LOST=<s>: taken as lost that many seconds after the renderer's first frame (tests).
std::atomic<bool> g_device_lost{false};

void WatchDeviceLoss() {
  using Clock = std::chrono::steady_clock;
  static const Clock::time_point start = Clock::now();
  static const double test_after = [] {
    const char* v = std::getenv("NXR_TEST_DEVICE_LOST");
    return v && *v ? std::atof(v) : -1.0;
  }();
  if (g_device_lost.load(std::memory_order_relaxed) || !g_device) return;
  const Clock::time_point now = Clock::now();
  if (test_after >= 0 && std::chrono::duration<double>(now - start).count() >= test_after) {
    REXLOG_ERROR("[native_x_renderer] the Vulkan device taken as lost (NXR_TEST_DEVICE_LOST={:.0f})", test_after);
    g_device_lost.store(true, std::memory_order_relaxed);
    return;
  }
  static uint64_t last_completed = 0, presented_then = 0;
  static Clock::time_point moved_at = now;
  const Device::FenceState fences = g_device->GetFenceState();
  if (fences.queue_completed != last_completed) {
    last_completed = fences.queue_completed, presented_then = fences.frames_presented, moved_at = now;
    return;
  }
  const double still = std::chrono::duration<double>(now - moved_at).count();
  if (still >= 3.0 && fences.frames_presented >= presented_then + 60 && fences.queue_next > fences.queue_completed + 64) {
    REXLOG_ERROR("[native_x_renderer] the Vulkan device is lost: the command queue's fence has stood at {} for {:.1f} s "
                 "({} submissions waiting) while {} frames were presented (the graphics driver reset the GPU?)",
                 fences.queue_completed, still, fences.queue_next - fences.queue_completed,
                 fences.frames_presented - presented_then);
    g_device_lost.store(true, std::memory_order_relaxed);
  }
}

void ExecuteFrameEnd(const Command& c) {
  uint32_t frame = c.frame;
  ++g_frames_presented;
  if (frame % 30 == 0) WatchDeviceLoss();
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
    if (const std::string sections = g_sections.Take(g_uploads.frames); !sections.empty())
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: GPU ms a frame by pass: {}", frame, sections);
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
    if (NightPassOn()) {
      uint64_t world = 0, glow = 0, lights = 0, sprites = 0;
      const uint64_t applied = g_night.TakeApplied();
      g_night.TakeCounts(world, glow, lights, sprites);
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: night applied {} times in {} frames, {} with the world's "
                  "depth from before the weapon (D162); the effects' light in {} ({} copies of their layer), {} lights "
                  "and {} lamp glows drawn in all, {} models' lamps placed (D163)", frame, applied, g_uploads.frames,
                  world, glow, g_night_glow_frames, lights, sprites, g_night_models_placed);
      g_night_glow_frames = 0;
      g_night_models_placed = 0;
      double night_ms = 0.0, night_max_ms = 0.0;
      uint64_t night_timed = 0, night_seen = 0;
      double night_lights_ms = 0.0;
      uint64_t night_compute = 0;
      g_night.TakeGpu(night_ms, night_max_ms, night_timed, night_seen, night_lights_ms, night_compute);
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: night pass GPU {:.3f} ms average, {:.3f} ms max over {} "
                  "frames (the effects' lights' search {:.3f} ms of it, by compute in {}); the lamps' visibility "
                  "found once a frame in {}",
                  frame, night_ms, night_max_ms, night_timed, night_lights_ms, night_compute, night_seen);
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

// A draw's context calls are recorded for the recorder's thread; any other command, and a draw that presents
// first, makes its own at once, after the recorded ones. A command counts as run once this thread is past it: a
// recorded call holds copies of what it uploads, and a draw changes nothing the game reads.
void ExecuteCommand(Command& c);

void Execute(Command& c) {
  Recorder* recorder = g_device && g_device->recorder().threaded() ? &g_device->recorder() : nullptr;
  if (!recorder) return ExecuteCommand(c);
  const bool record = c.type == Command::Type::kDraw && !g_pending_count;
  if (!record) recorder->BeginInline();
  ExecuteCommand(c);
  if (record) recorder->Publish();
  else recorder->EndInline();
}

void ExecuteCommand(Command& c) {
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

namespace native_x_renderer {
Diligent::ITextureView* HostMsaaDrawView(HostTarget* target, bool multisampled, bool writes) {
  return renderer::MsaaDrawView(target, multisampled, writes);
}
bool DeviceLost() { return renderer::g_device_lost.load(std::memory_order_relaxed); }
}  // namespace native_x_renderer
