# GTAO port notes (King Kong Recompiled's ambient occlusion in NativeXRenderer)

Status, 6 November 2026: written, not compiled, not run. The GLSL of every pass was checked with the LunarG SDK's
glslang 16.6 (`-V --target-env vulkan1.1`) and `spirv-val`: all 14 programs compile and validate. Nothing else was
checked: no build, no game, no tests. This file stands in for a decision entry (the next free number is D132) until
the module is integrated.

## What it is

XeGTAO (Intel, MIT) from the scene's depth, multiplied into the scene's colour. It is a port of King Kong
Recompiled's GTAO (`F:\KK-native-renderer\wt\v2.1.0\native-renderer`: `docs/ao.md`, `backend/gtao.*`,
`backend/shaders/gtao.hlsl`, the settings in `plugin/plugin_main.cpp`), from NVRHI and HLSL compiled offline to
DiligentCore and GLSL compiled at startup by glslang (D16).

| File | What |
|---|---|
| `src/gtao.h` | `GtaoSettings` (the cvars), `GtaoProjection` (the camera from a matrix), `Gtao` (the passes) |
| `src/gtao.cpp` | the cvars, the pipelines, the images, the constants, `Apply`, `ForgetTargets`, the GPU timer |
| `src/gtao_shaders.h` | the GLSL 450 of every pass, with XeGTAO's MIT notice |

## What was ported

All of KK's compute passes, with XeGTAO's math and constants unchanged:

1. `prepare`: the picture's stored depth into a working image padded to a multiple of 16 (edges repeated; every
   second pixel at half resolution).
2. `prefilter`: `XeGTAO_PrefilterDepths16x16`, view-space depth and its 4 smaller mips (5 levels, R32F).
3. `normals`: `XeGTAO_ComputeViewspaceNormal`, packed R11G11B10 in R32_UINT.
4. `smooth h` and `smooth v`: KK's normal smoothing (native_ao_normal_smoothing 1 low, 2 high), in place through
   term b, the same radii, angles and plane tests.
5. `main low / medium / high`: `XeGTAO_MainPass` with 2x2, 3x3 and 9x3 slices and steps, separate programs as in KK.
6. `denoise` and `denoise last`: two of XeGTAO's edge-aware passes.
7. `output`: one R32F image over the picture, 1 for the sky (stored depth within 1e-6 of the far plane), depth-aware
   upsampling at half resolution, KK's distance fade.
8. `temporal`: KK's local accumulation (80 % history, 1 % depth rejection, 3x3 clamp, a quarter-picture jump limit),
   only without a temporal upscaler, only with a matching camera (at least 3 votes and half of the offers).
9. The opaque moment's apply: a full-screen triangle over the picture with a multiplicative blend (`dst * src`, alpha
   not written, nothing read back). ao_mode 2 writes the occlusion itself (no blend).

Kept from KK as it is: the strength mapping (power 1.6, 2.2, 3.0; radius 6, 8, 12 near-plane distances), the radius
multiplier 1.457, falloff 0.615, sample distribution power 2, mip sampling offset 3.30, blur beta 1.2, the noise
(Hilbert curve and R2, rotating over 64 frames only when a temporal filter smooths it), the half resolution rule of
`auto` (render scale above 1.5 on either axis), R32_FLOAT and R32_UINT images only. Also kept, though it looks like an
XeGTAO slip: `XeGTAO_ClampDepth` clamps view depth at 65504 because KK defines `XE_GTAO_USE_HALF_FLOAT_PRECISION` as
0 and XeGTAO tests it with `#ifdef`. With `XE_GTAO_USE_DEFAULT_CONSTANTS` 1 (XeGTAO's default, as in KK),
`native_ao_falloff` moves only where the falloff starts, not its length.

The KK fix 4fdb472d (effects forget freed render targets): `Gtao` never keeps a target by pointer. The depth and
colour are bound again at every `Apply` (dynamic shader variables). `ForgetTargets()` releases the bindings that
still hold the last depth target (prepare, output, temporal) and drops the history made from it. No pointer is
compared across frames.

## What was left out

- KK's draw classifier, its rule table and frame tracker (`ao_classifier.*`), `SceneAo`'s moments (opaque, ambient,
  resolve, the fallbacks) and its statistics. NativeXRenderer has no equivalent yet: the caller picks the moment.
- The ambient-only mode (the translator hook, `KKAmbientOcclusion` at b4, the shader list).
- The resolve moment's copy under the game's fog (`ApplyToCopy`, `AoFog`) and the depth copy (`CopyDepth`).
- The first-person hands redraw (`AoRedrawHands`, `ao_hands.hlsl`).
- `Prepare` and `SmoothNormals` as separate entry points for KK's GI and cel shading.
- KK's camera from the viewport registers (`ComputeAoViewMapping` with `PA_CL_VPORT_*`, the window offset and D3D9's
  half-pixel offset). NativeXRenderer's caller gives the picture in host pixels and the depth range instead.
- The AO timer's trace file (`KK_DEV_DLSS_LOG`) and the graphics panel's readout; `native_ao_gpu_time` logs only.
- `native_ao_method`, `native_ao_apply`, `native_ao_rules`, `native_ao_shaders`, `native_ao_dump_every`.

## Settings (cvars, defined in gtao.cpp)

| Cvar | Default | Meaning |
|---|---|---|
| `ao_mode` | 1 | 0 off, 1 on, 2 the occlusion only |
| `ao_strength` | 3 | 1 to 3: power 1.6 / 2.2 / 3.0, radius 6 / 8 / 12 near-plane distances |
| `native_ao_samples` | 3 | 1 low (2x2), 2 medium (3x3), 3 high (9x3) |
| `native_ao_quality` | auto | auto, full, half |
| `native_ao_normal_smoothing` | 2 | 0 off, 1 low, 2 high |
| `native_ao_temporal` | true | the local temporal filter (or the upscaler's history) |
| `native_ao_gpu_time` | false | duration queries, the average logged every 600 frames |
| `native_ao_radius` | 0 | world units; 0 from the camera |
| `native_ao_radius_scale` | 1 | times the radius from the camera |
| `native_ao_falloff` | 0 | XeGTAO's falloff range (0: 0.615) |
| `native_ao_power` | 0 | XeGTAO's final power (0: from the strength) |
| `native_ao_fade` | 0 | fades out over this many near-plane distances (0: none) |

All are read again each time `GtaoSettings::FromCvars()` runs (call it once a frame). The defaults are the ones
asked for. They are not KK v2.1.0's cvar defaults: there ao_mode is 0, native_ao_samples 2 and
native_ao_normal_smoothing 0 (its launcher presets set the rest).

`ao_mode` and `ao_strength` are also defined by the SDK's Xenos plugin (`sdk/src-bf/src/graphics/d3d12/
render_target_cache.cpp`, defaults 0 and 1, in the `rexgpu-xenos` DLL when `REXGLUE_USE_D3D12` is on). When that DLL
registers them first, rex::cvar ignores the second registration ("duplicate registration", logged), and
`FromCvars` reads them by name (`REXCVAR_QUERY`), so it still sees the live value, with that plugin's defaults. If a
link ever sees both definitions in one image (`FLAGS_ao_mode_storage_` twice), delete the two definitions here.

## Public API

```cpp
GtaoSettings GtaoSettings::FromCvars();
bool GtaoProjection::FromMatrix(const Mat4d& clip_from_position, GtaoProjection& out);   // column form
bool GtaoProjection::FromViewProjectionRows(const std::array<float, 16>& m, GtaoProjection& out);  // BeginView rows

class Gtao {
  bool Initialize(Diligent::IRenderDevice* device);
  void Configure(const GtaoSettings& settings);
  struct Inputs {
    Diligent::ITexture* depth;      // HostTarget::texture of the scene depth (D24S8 or D32S8X24)
    Diligent::ITextureView* color;  // HostTarget::view of the scene colour (its RTV)
    int32_t x, y; uint32_t width, height;  // the 3D view's picture, host pixels
    GtaoProjection projection;
    float z_min, z_max;             // the viewport's depth range
    RenderScale scale;              // HostTarget::scale
    uint64_t frame;
    const Mat4d* reprojection; uint32_t votes, offers;  // the camera vote, for the local history
  };
  bool Apply(Diligent::IDeviceContext* context, const Inputs& in);
  void ForgetTargets();
  Diligent::ITexture* occlusion() const;  // the last R32F result (and origin_x/y, width/height, step)
  uint32_t TakeApplied();
};
```

`Apply` ends the render pass, unbinds all targets first (the depth is read as a texture), runs the compute passes,
draws the apply triangle into `color`, and leaves no target bound. The depth target ends in the SHADER_RESOURCE state;
the next draw rebinds it with a transition once `DrawPath::TargetsChanged()` is called.

## Integration (what I expect you to add)

1. `CMakeLists.txt`: `src/gtao.cpp` in `add_library(native_x_renderer ...)`. `gtao_shaders.h` is a plain header.
2. `THIRD_PARTY_NOTICES.md`: a row for XeGTAO (Intel, MIT, the notice in `src/gtao_shaders.h`), and the King Kong row
   extended with the GTAO port. `docs/decisions.md`: D132 from this file.
3. `renderer.cpp`, globals: `Gtao g_gtao;` and `bool g_gtao_on = false;`.
4. `renderer.cpp`, startup, after `g_aa.Initialize(...)`: `g_gtao_on = g_gtao.Initialize(device->device());`.
5. The scene's targets and camera without an upscaler. Today `g_scene_color` and `g_scene_depth` are kept only with
   DLSS or FSR (`bool scene = g_scene_view && g_temporal;` in the draw path), and the views are found from the draws
   (D119) only when `g_temporal`. For AO without an upscaler:
   - `g_auto_views = (g_temporal || g_gtao_on) && !hooks && profile.upscale_resolve_caller;` (Battlefield 1943 has
     no view hooks; its upscale point is the scene resolve 0x82586834);
   - `bool scene = g_scene_view && (g_temporal || g_gtao_on);` while `jitter` stays tied to `g_temporal`
     (`bool jitter = g_temporal && scene && ...`), and `SetSceneStart(..., g_temporal && g_auto_upscaled_last_frame)`;
   - the camera vote (`g_camera_vote`) on when AO's local history should work, and in `draw.cpp` the offer at line
     1427 for the view's draws when not jittered too (`if ((jittered || scene_view_) && ...)`): today only jittered
     draws vote. Without that, AO runs without local history (fixed noise, the two spatial passes), which is KK's own
     fallback;
   - the depth range: `DrawPath::jittered_z_min/max()` are recorded only for jittered draws (defaults 0 and 1). Either
     record them for the view's draws too, or accept 0 to 1.
6. The moment: in `ExecuteResolve`, at the upscale point, before `UpscaleScene()` (so DLSS or FSR get the occluded
   image, as in KK, and SMAA after it), something like:

   ```cpp
   if (g_gtao_on && g_scene_color && g_scene_depth && g_draw) {
     g_gtao.Configure([] {
       GtaoSettings s = GtaoSettings::FromCvars();
       s.temporal_upscaler = g_temporal;
       return s;
     }());
     Gtao::Inputs in;
     if (GtaoProjection::FromMatrix(g_draw->scene_start_matrix(), in.projection)) {
       in.depth = g_scene_depth->texture;
       in.color = g_scene_color->view;
       in.width = g_scene_color->scale.HostWidth(g_profile->output_width);
       in.height = g_scene_color->scale.HostHeight(g_profile->output_height);
       in.scale = g_scene_color->scale;
       in.z_min = g_draw->jittered_z_min();
       in.z_max = g_draw->jittered_z_max();
       in.frame = g_device->frames_presented();
       Mat4d vote;
       uint32_t votes = 0, offers = 0;
       // Only without an upscaler: UpscaleScene takes the vote itself.
       if (!g_temporal && g_camera_vote && g_draw->TakeCameraVote(vote, votes, offers)) {
         in.reprojection = &vote;
         in.votes = votes;
         in.offers = offers;
       }
       g_gtao.Apply(g_device->context(), in);
       g_draw->TargetsChanged();
     }
   }
   ```

   With view hooks instead, the same in `UpscaleScene()`'s place with `GtaoProjection::FromViewProjectionRows(
   g_scene_view_projection, ...)`.
7. `ForgetTargets`: call `g_gtao.ForgetTargets()` where targets are freed (the `if (targets.count || resolved.count)`
   block of the frame's end, next to `g_draw->TargetsChanged()`), and wherever a later change gives a target a new
   texture (a live render scale or upscaler switch, if one is added).
8. The log line with the others: `g_gtao.TakeApplied()`.

## Doubts and open risks

- Not compiled. Every DiligentCore call mirrors one already in `aa.cpp`, `scaling.cpp`, `blit.cpp`, `targets.cpp`
  or `renderer.cpp`, and each type and member was looked up in the DiligentCore headers under
  `thirdparty/diligent-core` (`DispatchComputeAttribs`, `TextureViewDesc::MostDetailedMip/NumMipLevels`,
  `RenderTargetBlendDesc`, `COLOR_MASK_RGB`, `QueryDataDuration`, `RefCntAutoPtr::RawPtr`). New for NativeXRenderer:
  per-mip UAV views, R32_UINT textures read through `usampler2D` with an immutable point sampler, storage images in
  `r32f` and `r32ui`. These are plain DiligentCore features, but no other NXR pass uses them yet.
- The GLSL was validated with glslang 16.6. The SDK builds glslang 11.0 (2020). Nothing used is newer than GLSL 4.50
  core (textureGather, textureGatherOffset, shared memory, barrier, struct parameters from a uniform block).
- The camera comes from one draw (`scene_start_matrix`, the view's first depth-writing draw). KK voted over the first
  16 scene draws, because a world matrix that stretches an axis gives a wrong projection. If that first draw is
  scaled (a skybox, a stretched prop), the occlusion is wrong for the whole frame. A small vote like KK's
  `AoCameraFinder` is the fix.
- Reversed depth: `FromMatrix` uses the camera vote's perspective test (clip z close to clip w), which rejects a
  reversed-depth projection. The rest (sky test, depth unpack, near plane) handles reversed depth.
- The moment: at the scene resolve, everything the game drew before it is darkened with the scene: water, smoke,
  particles, glass and the first-person weapon (KK's "resolve moment" problem, which its classifier solved). The
  weapon is likely drawn with another field of view and near plane, so its occlusion uses the wrong camera.
- The picture is assumed to be the whole output-size target with clip space spread over it (no viewport offset, no
  half-pixel offset). A game whose 3D view is a sub-rectangle needs its viewport passed as x, y, width, height.
- `ForgetTargets` resets the local history; calling it every frame would disable the history.
- `native_ao_gpu_time` opens a duration query inside the frame timer's; DiligentCore implements both as timestamp
  pairs, which should nest, but this is not tried.
- Memory at 1280x720, full resolution: about 27 MB, plus about 15 MB for the local history; at render scale 2 the
  `auto` quality works at half resolution (about the same).
