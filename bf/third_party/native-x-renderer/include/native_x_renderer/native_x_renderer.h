// NativeXRenderer - native renderer for Xbox 360 games recompiled with the ReXGlue SDK.
//
// Replaces the drawing done through the game's XDK Direct3D library with host rendering on
// DiligentCore (Vulkan), keeping the SDK's Xenos emulation as the fallback. See native_x_renderer/README.md
// and native_x_renderer/docs/.
//
// Phase 1: the trace and the observer. The game reports its D3D entry points through hooks.h,
// describes its D3D device layout in a Profile, and calls Initialize() once before the guest starts.
// Nothing is drawn yet: the Xenos path draws.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rex::system {
class IGraphicsSystem;
}  // namespace rex::system
namespace rex::ui {
class Window;
}  // namespace rex::ui

namespace native_x_renderer {

// Bumped when the interface the game side uses changes.
inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 6;

// One line for the game's startup log: version and what the library does at this stage.
const char* Describe();

// The D3D entry points a game can report (calls.inc).
enum class Call : uint8_t {
#define NATIVE_X_RENDERER_CALL(id, arguments, flags) k##id,
#include <native_x_renderer/calls.inc>
#undef NATIVE_X_RENDERER_CALL
  kCount
};
inline constexpr size_t kCallCount = size_t(Call::kCount);

const char* CallName(Call call);

// Where the D3D device structure keeps what NativeXRenderer reads, as byte offsets from the device
// pointer (r3 of every device call). The values depend on the XDK version the game was built with:
// see docs/xdk-d3d-reference.md. kUnknown means "not known for this game".
inline constexpr uint32_t kUnknown = 0xFFFFFFFFu;

struct DeviceLayout {
  // The 64-bit dirty masks that a draw's flush uploads from the shadow. Their number and order change
  // between XDK versions (POTCAWE, 5426: six masks from +0, fetch slots in the fourth; GRAW, 2417: five from
  // +16, fetch slots in the third). NativeXRenderer reads three of them. The float constants: one bit per
  // group of 4 vec4, group 0 in the most significant bit. The fetch constants: one bit per slot, slot s in
  // bit fetch_dirty_slot0_bit - s (bit 0 the least significant).
  uint32_t vs_constants_dirty = kUnknown;     // u64
  uint32_t ps_constants_dirty = kUnknown;     // u64
  uint32_t fetch_constants_dirty = kUnknown;  // u64
  uint32_t fetch_dirty_slot0_bit = kUnknown;  // 31 to 63
  uint32_t command_buffer_write = kUnknown;  // u32: address of the last dword written (pre-increment)
  uint32_t fetch_constants = kUnknown;       // 32 slots x 6 dwords (register 0x4800)
  uint32_t vs_float_constants = kUnknown;    // 256 vec4 (0x4000)
  uint32_t ps_float_constants = kUnknown;    // 256 vec4 (0x4400)
  uint32_t bool_loop_constants = kUnknown;   // 0x4900

  // The register shadow, in contiguous groups of registers.
  struct RegisterGroup {
    uint32_t offset = 0;
    uint16_t first_register = 0;
    uint16_t count = 0;
  };
  std::array<RegisterGroup, 8> register_groups{};

  uint32_t fence_counter = kUnknown;     // u32: fence of the command buffer segment being written
  uint32_t write_back_block = kUnknown;  // u32 pointer: [+0] last fence the GPU reached
  uint32_t flags = kUnknown;             // u32: tiling and predication flag bytes

  uint32_t vertex_declaration = kUnknown;
  uint32_t index_buffer = kUnknown;
  uint32_t render_targets = kUnknown;  // 4 surface pointers
  uint32_t depth_stencil = kUnknown;
  uint32_t vertex_buffers = kUnknown;  // one pointer per stream
  uint32_t textures = kUnknown;        // one pointer per sampler
  uint32_t viewport = kUnknown;        // x, y, width, height, min z, max z (floats)
  uint32_t pixel_shader = kUnknown;
  uint32_t vertex_shader = kUnknown;
  uint32_t predication_mask = kUnknown;
  uint32_t tile_count = kUnknown;
  uint32_t tile_rects = kUnknown;  // x1, y1, x2, y2 per tile
  // Only for a game that commits inline draws itself (an EndVertices inlined into the engine, D77): where
  // BeginVertices and BeginIndexedVertices save the address of the last dword of their packets, which EndVertices
  // copies into the write pointer (Kameo, XDK 2099: +13620). The next call that finds the write pointer there
  // draws the inline draw first. kUnknown when the game calls EndVertices (hooked) or never begins one itself.
  uint32_t inline_draw_end = kUnknown;
  // A device that records a command buffer instead of drawing (the XDK's command buffer devices, created by
  // Direct3D_CreateDevice for recording; Battlefield 1943, XDK 7978: the flag byte +10940 has 0x80): the byte at
  // recording_flags with any bit of recording_mask set. Calls on such a device, on whatever thread records, are only
  // traced: they do not reach the state tracker, the observer or the renderer, which follow the main device. kUnknown
  // when the game records no command buffers.
  uint32_t recording_flags = kUnknown;
  uint8_t recording_mask = 0;
};

// A bit field of a u32: (value >> shift) & ((1 << bits) - 1).
struct BitField {
  uint8_t shift = 0;
  uint8_t bits = 0;
  uint32_t Get(uint32_t value) const { return (value >> shift) & ((1u << bits) - 1u); }
};

// Where the XDK's resource objects keep what NativeXRenderer reads, as byte offsets in the object. Like the
// device layout, they depend on the XDK version (docs/xdk-d3d-reference.md).
struct ObjectLayout {
  // A texture's fetch constant: 6 dwords, as in the device's fetch constants (the front buffer, resolve
  // destinations).
  uint32_t texture_fetch = kUnknown;
  // A surface (render target, depth stencil): its RB_SURFACE_INFO (pitch, MSAA), its RB_COLOR_INFO or
  // RB_DEPTH_INFO, and a u32 with its width and height minus one.
  uint32_t surface_info = kUnknown;
  uint32_t surface_color_depth_info = kUnknown;
  uint32_t surface_size = kUnknown;
  BitField surface_width_minus_one;
  BitField surface_height_minus_one;
  // A vertex or index buffer's base address and size (the trace prints them).
  uint32_t buffer_address = kUnknown;
  uint32_t buffer_size = kUnknown;
};

// Everything NativeXRenderer needs to know about one game's D3D library.
// A draw of the 2D layer after the main 3D view (Profile::overlay_motion, D124), as the profile's overlay_motion_adjust
// sees it: its vertex shader (the microcode's XXH3 hash), its vertex count, its vertices' positions this frame and the
// last (x then y, pixels of the canvas), and whether the last ones are a matched draw's (else they are this frame's:
// no movement). The adjustment may write `previous`.
struct OverlayDraw {
  uint64_t vertex_shader;
  uint32_t vertex_count;
  const float* positions;
  float* previous;
  bool matched;
};

struct Profile {
  const char* game = "";
  const char* xdk_version = "";  // as read from the XEX (for example "2.0.5426")
  DeviceLayout device;
  ObjectLayout objects;
  // The game's output resolution (its front buffer): the image the render scale fits to the screen.
  uint32_t output_width = 1280, output_height = 720;
  // The return address of the Resolve call that copies the finished scene: after the game's
  // post-processing, before its HUD is composited. NativeXRenderer's antialiasing (FXAA or SMAA) is applied to
  // that copy, a texture of the game's output size (resolves from the same caller to other sizes are not
  // the scene). 0: no antialiasing.
  uint32_t scene_resolve_caller = 0;
  // When the game resolves through a function of its own that every resolve goes through (GRAW), the return
  // address one level up as well, in the function that called that one (D65; the trace prints both, as
  // "from <caller> < <its caller>"). It is read from the frame's LR slot, which __savegprlr fills: a game
  // built with the codegen's non_volatile_as_local needs the shared SDK patch that keeps that store. 0: not
  // checked.
  uint32_t scene_resolve_outer_caller = 0;
  // When such copies come more than once a frame (Kameo: the scene with its fog, then the bloom composite, into the
  // same texture), only the first one of each frame is antialiased (D94): the post-processing after it adds no edges.
  // False: every one.
  bool scene_resolve_once = false;
  // The game's hooks mark its views (BeginView with the scene's matrix, EndViewGeometry): DLSS and FSR 3.1 jitter the
  // main 3D view between them and run at its end (D47). False: the view is found from the draws, with
  // upscale_resolve_caller (D119).
  bool view_hooks = false;
  // The return address of the Resolve call that copies the finished 3D scene before the game's post-processing reads
  // it, for a game whose views are found from the draws (D119): the main 3D view starts at its first draw of the
  // output size that writes depth with a perspective matrix in its vertex constants (the camera vote's, D118), and
  // ends here, where DLSS or FSR 3.1 runs on it before the copy. It may be scene_resolve_caller (GH Metallica: its
  // copy comes before its HUD and note highway, which stay out of the view). 0: no DLSS or FSR 3.1 without hooks.
  uint32_t upscale_resolve_caller = 0;
  // The return address of the Resolve call of the output-size depth that comes right after the main view's opaque
  // draws (before its water, particles and first person weapon): the ambient occlusion (gtao.cpp, D132) is worked
  // out from the depth and multiplied into the scene's colour just before it. Needs the view found from the draws
  // (upscale_resolve_caller). 0: no ambient occlusion.
  uint32_t ao_resolve_caller = 0;
  // Resolves whose destination the game then reads on the CPU (D165). A resolve stays on the host (D70): its texels
  // reach guest memory only where a draw reads them as vertices (D140). A CPU reader would get what that memory held
  // before: zeros in a fresh process, another level's leftovers once the heap has been used again. For a resolve
  // that matches an entry here (the return address of the Resolve call; its caller's too when the game resolves
  // through one function of its own, 0: any; the destination's size, 0: any; its guest format, -1: any) the
  // destination's guest bytes are set to zero as the game makes the call, on its own thread, so the reader finds an
  // empty picture every time (Battlefield 1943's undergrowth cell masks: its jobs place the game's own undergrowth
  // from them, and placed plants on roads from a second level on). caller 0: an unused entry.
  struct ZeroedResolve {
    uint32_t caller = 0, outer_caller = 0, width = 0, height = 0;
    int32_t format = -1;
  };
  ZeroedResolve zeroed_resolves[4];
  // The game draws its 2D layer after the main 3D view (its HUD; GH Metallica's note highway and notes) with inline
  // vertices whose first two floats are their position in pixels of the output canvas (output_width x output_height).
  // With frame generation those draws' motion comes from their vertices, each draw matched with the last frame's
  // (D124), and the HUD-less picture is not used: the 2D layer moves as it moved, the scene's motion behind it.
  bool overlay_motion = false;
  // With overlay_motion: called on the renderer's thread at the frame's end with its 2D layer's draws in order, before
  // their motion is drawn, where the vertices do not show the movement (GH Metallica's board stands still while its
  // texture scrolls): the game sets their last positions. nullptr: none.
  void (*overlay_motion_adjust)(OverlayDraw* draws, size_t count) = nullptr;
  // The game's hooks widen its 16:9 cameras by AspectFactor() (D43): NativeXRenderer then fills an output of any
  // aspect ratio, with a separate render scale per axis, instead of showing the 16:9 image with bars.
  bool widens_cameras = false;
  // With widens_cameras, a game without hooks round its 2D views whose frame is: the scene's copy
  // (scene_resolve_caller), then only smaller targets (a bloom chain), then one draw of the output's size (the
  // composite), then the HUD into the output: every draw of the output's size after that first one goes into the
  // aspect box (D43), and so does every one in a frame without a scene's copy (a loading screen, a video, a menu)
  // when the frame before had none either. Battlefield 1943 (D162's order).
  bool box_2d_after_composite = false;
  // Float constants in which the game gives its shaders the size of a texture they sample, as (width, height,
  // 1 / width, 1 / height), to compute texel positions themselves (POTCAWE's shadow maps: a bilinear filter of four
  // point samples on the map's texels). A resolve result is larger on the host (the render scale, the shadow scale,
  // D92): when the texture bound to `fetch_constant` is one, and the constant holds exactly its guest size, the draw
  // gets its host size instead, so the game's filter works on the host's texels (D93).
  struct TextureSizeConstant {
    bool pixel_shader = true;     // in the pixel shader's float constants, else the vertex shader's
    uint32_t constant = 0;        // the float constant (0-255)
    uint32_t fetch_constant = 0;  // the texture's fetch constant (0-31)
  };
  const TextureSizeConstant* texture_size_constants = nullptr;
  uint32_t texture_size_constant_count = 0;
  // The game's pixel shaders that read their pixel position (PsParamGen) sample at it plus one texel (Battlefield
  // 1943's water and the composites of its half resolution layers): with a render scale the position given them is
  // the host pixel's centre less one guest pixel, so they read the texel under the host pixel (D164; the SDK
  // translator's param_gen_texel_centre). False: the SDK's floor(position) / scale.
  bool param_gen_texel_centre = false;
};

// Call once, before the guest starts (for example in ReXApp::OnPostSetup). Reads the cvars, logs the
// configuration and turns the hooks on if a feature needs them. The profile must outlive the run.
// graphics: the runtime's graphics system (the observer registers a draw observer there; the renderer
// turns its host rendering off). window: the game's window (the renderer presents there). Call it from
// the UI thread: the renderer detaches the SDK's presenter from the window.
// cache_root: the runtime's cache directory (ReXApp: runtime()->cache_root()), where the renderer keeps the
// shaders and pipelines between runs; empty for none.
void Initialize(const Profile& profile, rex::system::IGraphicsSystem* graphics, rex::ui::Window* window,
                const std::filesystem::path& cache_root = {});

// Whether NativeXRenderer draws the game (the native_x_renderer cvar, and the renderer started), as of Initialize.
// For game settings that NativeXRenderer does not support yet (for example POTCAWE's 2x MSAA, rendered in two
// EDRAM tiles).
bool Renders();

// The pipelines stored by earlier runs (or a game's setup, D134), made before play: how many, how many are compiled,
// whether the storage was read (at the first draw), and whether all are done (also when there is nothing to make or
// NativeXRenderer does not draw). For a setup screen over the game's first frames; callable from any thread.
void StorageProgress(uint32_t* queued, uint32_t* done, uint32_t* loading, uint32_t* finished);

// D142: a game's own pictures over a guest texture's (Battlefield 1943's controller button prompts in its UI atlases,
// in the player's chosen style). After each load of the base level of a guest k_8_8_8_8 2D texture with a single mip,
// `fn` gets its size and its guest data (as the game holds it: tiled, big endian, width * height * 4 bytes) and may
// fill `rgba` (width * height * 4, RGBA8, top row first), which is then written over level 0. No scan: only textures
// the game draws are seen, once per load. Null: none. From any thread; the call is on the renderer's thread.
// D160: `fn` returning true takes the texture as one of its own, filled or not (an empty `rgba` leaves the texture as
// it is): after RepatchTextures, each texture it took is offered again with `again` true, its guest data unchanged
// since it was last offered (a texture whose data changed is offered at its load, `again` false), before the next draw
// that follows.
using TexturePatchFn = bool (*)(uint32_t width, uint32_t height, const uint8_t* guest_data, std::vector<uint8_t>& rgba,
                                bool again);
void SetTexturePatcher(TexturePatchFn fn);
// D160: the textures the patcher took are offered to it again before the next draw, for pictures that follow the
// game's state (Battlefield 1943's keyboard prompts: the keys of a vehicle's seat differ from a soldier's on foot).
// From any thread.
void RepatchTextures();

// How much wider the rendered image is than the game's (output aspect ratio over the game's), when
// NativeXRenderer fills an output that is not 16:9 (Profile::widens_cameras, D43): above 1 for a wider output,
// below 1 for a taller one. The game's camera hooks widen its 16:9 views by it. 1 otherwise. Fixed at
// Initialize.
double AspectFactor();

// Whether the draws from now on are a 2D layer laid out for the game's aspect ratio (a HUD, menus, a video
// behind them). While NativeXRenderer fills an output of another aspect ratio (AspectFactor() != 1, D43), those
// draws go into a box of the game's aspect ratio in the middle of their render target instead of being
// stretched with the 3D view. Set by the game's hooks around its 2D views; off again at every Present.
void SetGameAspectBox(bool on);

// A world marker the game projected to its HUD this moment (a capture flag's icon, a name tag), in pixels of the
// game's output (1280x720): with Profile::box_2d_after_composite on a wider output, a HUD draw on it is placed where
// the widened 3D view shows that point, not in the 16:9 box. Any thread; kept 0.2 s.
void NoteHudMarker(float x, float y);
// Whether a HUD marker was noted within `radius` pixels of (x, y) in the last 0.2 s.
// marker_x, when given, gets the nearest marker's x (canvas pixels).
bool NearHudMarker(double x, double y, double radius, double* marker_x = nullptr);
// One of the game's own menu screens is up now (its UI says so about once a second): the 2D layer is a menu, kept
// together in the middle, not anchored to the edges. Any thread.
void NoteGameMenu();
// Whether NoteGameMenu came within the last 1.5 s.
bool GameMenuRecently();

// D150: a fixed GPU workload timed once, for a first start's auto detect: started now, run at a present soon after;
// its time (ms) once known (false until then; -1 if it could not run). About 0.6 ms on an RTX 4070 Ti.
void GpuBenchmarkStart();
bool GpuBenchmarkResult(double* ms);

// D158: whether NVIDIA Image Scaling runs on this GPU (asked at startup when native_x_renderer_scaling is nis, or with
// native_x_renderer_check_nis), and the GPU's name; false until the renderer has started and asked.
bool NisSupport(bool* runs, std::string* gpu);

// Developer tests (a camera tour): the terrain's ground height at world x, z, as the grass reads it, asked for with a
// number and answered a few frames later (GrassProbeResult true once it is, with that number). Grass must be on.
void GrassProbe(float x, float z, uint32_t id);
bool GrassProbeResult(uint32_t* id, float* ground);

// D151: where grass may grow on the level, from the game's own map of its ground (TekRantGaming, 9 Nov 2026: grass decided
// from the screen flickered at the edges of the ground's grass, moved about, and faded in as he turned): each cell's
// distance to ground that is not grass (m x 16, at most 255; 0 not grass), `width` x `height` cells of `cell` m, the
// first cell's corner at world (x0, z0), its columns along x and rows along z; copied. Width 0 with cell 0: the level's
// map is being read (no grass until it comes); width 0 with cell < 0: none known (grass decides from the screen).
void GrassMap(const uint8_t* distance, uint32_t width, uint32_t height, float x0, float z0, float cell);

// D163: a lamp of the level, for the night (native_x_renderer_night; Battlefield 1943 Reborn's Night Ops servers: the
// level's lampposts, from its own files): where its light comes from, where it points, how far it reaches, its colour.
struct NightLamp {
  float position[3] = {};                // the bulb in the world (the game's units, metres, y up)
  float direction[3] = {};               // a spot's axis (unit); 0, 0, 0: a point light
  float color[3] = {1.0f, 0.8f, 0.55f};  // linear, times its strength
  float range = 15.0f;                   // metres: no light beyond
  float cone_inner = 35.0f, cone_outer = 60.0f;  // a spot's half angles in degrees: full light within, none beyond
  float glow = 0.25f;                    // the bulb's glow, its radius in metres (0: none)
};
// The level's lamps, copied (count 0: none). Lit only at night.
void NightLamps(const NightLamp* lamps, uint32_t count);

// D163: lamps that move with a model (a jeep's headlights): a draw of the main view with this many indices (one of the
// model's levels of detail) is that model; its lamps are in its own space (x right, y up, z forward), placed each frame
// by its world matrix, three rigid rows (the position in their fourth floats) in its vertex shader's float constants
// from `matrix_register` (Battlefield 1943: the camera in c16, then a matrix per part of the model from c17 in the
// parts' order). Copied (count 0: none). Lit only at night.
struct NightModelLamps {
  uint32_t index_count = 0;
  uint32_t matrix_register = 17;
  NightLamp lamps[4];
  uint32_t lamp_count = 0;
};
void NightModels(const NightModelLamps* models, uint32_t count);

// The game starts drawing a view: a camera's image (its 3D scene, a reflection, a shadow map, a 2D layer).
// `scene_view_projection`: for a view of the 3D world, its view-projection matrix (16 floats, rows, as the
// game gives them to its shaders, for a row vector); null for any other view. With DLAA
// (native_x_renderer_antialiasing = dlaa, D47), the 3D view drawn into targets of the game's output size (the
// main one: reflections and shadow maps are smaller) is jittered from frame to frame.
void BeginView(const float* scene_view_projection);

// A frame's camera and how to move models the game placed from another camera onto it (D154: Battlefield 1943 places
// a tank's vision slit at each 30 Hz tick from that tick's camera, and draws it so on the frames between, where the
// view is blended on every frame). `position`: the frame's camera position as its view gets it (x, y, z); `move`: 16
// floats, column form (rows: world = move x world), taking a model placed from the tick's camera to the frame's;
// `placed`: the position of the camera the models were placed from (x, y, z). A draw in a near projection (near plane
// under half a metre) with a world matrix in three consecutive registers that puts the eye at a recorded position, and
// whose object holds that record's placed camera within half a metre, is drawn moved. (Until D156 the object had to
// hold the frame's eye within half a metre: the air raid's bomb sight, up to a tick of a bomber's flight from the
// frame's eye, was left out on the frames a whole tick behind.)
void SetAttachedCameraMove(const float* position, const float* move, const float* placed);

// The view's 3D drawing is done and its post-processing starts. For the main 3D view, DLAA runs here on its
// color and depth, and the game's post-processing then reads the antialiased image.
void EndViewGeometry();

}  // namespace native_x_renderer
