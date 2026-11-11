# SSGI port notes (King Kong Recompiled's screen space global illumination in NativeXRenderer)

Status, 6 November 2026: written, not built, not run. The GLSL of every pass was checked with the LunarG SDK's
glslang 16.6 (`-V --target-env vulkan1.1`, each program as `#version 450` plus `kCommon` plus its part) and
`spirv-val --target-env vulkan1.1`: all 9 programs compile and validate. `src/ssgi.cpp` (with `src/gtao.cpp` as a
control) also passed `clang++ -fsyntax-only -std=c++20 -Wall -Wextra` against the DiligentCore and SDK headers, with no
error and no warning in the new files. Nothing else was checked: no build, no game, no tests. This file stands in for a
decision entry (the next free number is D133) until the module is integrated.

## What it is

One bounce of indirect light from the lit scene colour, added to the scene's colour at the end of the main view's
opaque draws, just after the ambient occlusion (D132). It is a port of King Kong Recompiled's screen space GI
(`F:\KK-native-renderer\wt\v2.1.0\native-renderer`: `docs/gi.md`, `backend/ssgi.*`, `backend/scene_gi.*`,
`backend/gi_config.*`, `backend/game_gi.cpp`, `backend/shaders/ssgi.hlsl`, and the GI sections of
`handover-bf/03-lighting-effects.md`), from NVRHI and HLSL compiled offline to DiligentCore and GLSL compiled at
startup by glslang (D16). The gather is a visibility bitmask over GTAO's slices (Therrien, Levesque and Gilet, "Screen
Space Indirect Lighting with Visibility Bitmask", 2023: a published method, no code of theirs). The depth passes are
XeGTAO's (Intel, MIT).

| File | What |
|---|---|
| `src/ssgi.h` | `SsgiSettings` (the cvars), `SsgiLevel` (Low, Medium, High), `Ssgi` (the passes) |
| `src/ssgi.cpp` | the cvars, the pipelines, the images, the constants, `Apply`, `ForgetTargets`, the GPU timer |
| `src/ssgi_shaders.h` | the GLSL 450 of every pass, with XeGTAO's MIT notice |

## What was ported

KK's passes, with its math and constants unchanged:

1. A copy of the scene colour (KK copies it for its composite; here every pass reads the copy).
2. XeGTAO's `prepare`, `prefilter` (view depth and 4 smaller mips) and `normals`, on images of the GI's own. KK's GI
   also owns a second `Gtao` and runs only its `Prepare`, unsmoothed, so turning the AO's normal smoothing on never
   changes the GI.
3. `radiance`: per working pixel the mean of its host texels with every sky texel left out (black), then linear light
   (power 2.2) and KK's sunlit source estimate (task 073): with `E` the linear mean, `a = luma(ambient) * limit^(1/2.2)`,
   `source = E + 0.65 * smoothstep(a^2.2, (a + max(0.15, a))^2.2, luma(E)) * E / maxChannel(E)`. Then 4 smaller mips
   (2x2 means, 5 levels).
4. `gather`: per working pixel `slices` directions with XeGTAO's Hilbert and R2 noise (start 17, so not the AO's
   pattern), `steps` samples per side within the radius (squared distribution), at most 0.45 of the picture's height
   on the screen. 32 sectors a slice, spaced evenly in GTAO's integrand. A sample covers the sectors between its front
   and its back (`thickness` deeper); only fresh sectors send light, only if both surfaces face each other (smooth
   grazing weight), not closer than `max(0.15, 2 * pixel size)`, faded over the last 30 % of the radius. Far samples
   read the radiance mip of about their spacing (trilinear). Output RGBA16F: rgb the light, a the visibility.
5. `temporal` (without a temporal upscaler): the gathered light blended into last frame's at 0.20, where the
   reprojected point's 4 bilinear history texels have a view depth within 5 % of its depth then (weight at least
   0.25), the history clamped to this frame's 3x3 range widened by half. The pre-blur result is the next history.
6. `denoise`: 2 (Low, Medium, 3x3 kernel) or 3 (High, 5x5) a-trous passes with steps 1, 2, 4, weighted by depth and
   normal, the sky weighing nothing.
7. The composite (a full screen triangle over the picture, alpha not written), in linear light:
   `scale = max(1, luma(lit) / (limit * luma(A)))`, `albedo = min(lit / (A * scale), limit)`,
   `bounces = 1 / max(1 - luma(albedo) * hidden, 0.4)`, `delta = albedo * (gi * 3 * intensity * bounces - A * hidden *
   occlusion)`, `delta >= -0.35 * lit`, the positive part scaled so that no channel adds more than 75 % of `lit`,
   faded with distance, `out = (lit + delta)^(1/2.2)`. Sky (stored depth at the far plane) and outside the picture
   unchanged. Debug views 1 (the bounce light), 2 (the change on grey), 3 (the visibility).

Kept from KK as it is: the levels (Low 2 x 6, 2 blur passes, half resolution, 3 m; Medium 2 x 6, 2, half, 5 m; High
4 x 10, 3, full up to a render scale of 1.5 and half above, 8 m), the thickness rule (a fifth of the radius, 0.25 to 1),
the albedo limit 0.75 (a picture value), the x3 calibration, the 75 % receiver bound, the 0.35 darkening bound,
occlusion 0 by default and always 0 with the AO on, the distance limit (Near 16, Medium 32, Far 64 m, Full none; the
last quarter fades by smoothstep, after the receiver bound; gather, temporal and denoise skip receivers beyond it), the
history weight 0.20, the vote rule (at least 2 votes and half of the offers), the radiance mip count 5, KK's
fallback ambient of 0.25 grey.

The KK fix 4fdb472d (effects forget freed render targets): `Ssgi` never keeps a target by pointer. The depth and
colour are bound again at every `Apply` (dynamic shader variables). `ForgetTargets()` releases the bindings that
hold the depth target (prepare, radiance, composite) and drops the history.

## What changed in the port, and why

- KK's `cs_radiance` and `cs_radiance_down` are one pass that writes all 5 mips through group shared memory (as
  XeGTAO's depth prefilter). The values are the same: the padded size is a multiple of 16, so every mip is exactly
  half the one above and its texels are the same 2x2 means. One pass also means no texture is bound to be read and
  written at once, which DiligentCore's state tracking does not allow.
- The camera mapping is NativeXRenderer's (`gtao.cpp`): the picture in host pixels, D3D's clip space, the viewport's
  depth range. So the sky test has a sign (Battlefield 1943's reversed depth, 0.0047 down to 0) and the temporal pass
  goes through the picture's clip space (as NXR's AO temporal pass) instead of KK's guest viewport registers.
- The temporal pass turns last frame's clip z into view depth with last frame's depth unpack (KK used this frame's;
  the same while the projection does not change).
- The noise rotates only where a temporal filter smooths it: the upscaler's history, or the local history with a
  camera vote that passed (NXR's AO rule). KK's GI rotated whenever temporal accumulation was on, even in a frame
  without history.
- The depth prefilter's weights use 6 near planes of the depth curve's own near plane (KK's GI `Gtao` runs at
  `ao_strength` 1, 6 near planes; the curve's near plane is renderer.cpp's rule for the AO, since Battlefield 1943's
  world projection says 21.34 while the depth is 0.1 / distance). The gather's radius is in world units (metres), as
  KK's.
- KK's composite could multiply the AO in (`kk_Extra.x`); it never did (always 0), so it is gone. The GI does not
  read `Gtao::occlusion()`.
- With DLSS or FSR 3.1 there is no local history: the camera vote is the upscaler's (`UpscaleScene` takes it later in
  the view), so the noise rotates and the upscaler's history smooths it, as NXR's AO does it. KK kept its own history
  with a vote of the jittered transforms.
- All settings are read every frame (KK read most at start).
- Three port settings for a game whose ambient constant and sun are not known: `native_gi_ambient` (KK's 0.25 grey
  default as a cvar), `native_gi_gamma` (KK's 2.2 as a cvar) and `native_gi_sunlit` (KK's 0.65 restoration as a cvar;
  0 gives the plain lit colour as the source). `Inputs::ambient` takes a real ambient once one is found.

## What was left out

- `SceneGi`: KK's draw classifier, rule table and frame tracker, its moments (the opaque end, the Present fallback),
  its camera finder (NXR has `DrawPath::TakeProjection`) and its own camera reprojection vote over keyed draws (NXR
  has `CameraVote`). The caller picks the moment, as for the AO.
- The frame's ambient light read from the lighting shaders' constant tables (`g_vAmbientColor` by name,
  `GameShader::ambient_constant`, `NoteAmbient`). Battlefield 1943 needs a census of its shaders' constant tables first
  (handover section 4.7); until then `native_gi_ambient` stands in.
- Any sun. There is no per map sun table yet (handover section 5 and 20, item 3), so this is KK's fallback: the source
  is the lit scene colour with the brightness heuristic. KK's suggested sun path (a pixel is sunlit when N.L > 0 and
  the sun's shadow map says lit; albedo = colour / (ambient + sun x N.L x visibility)) is not built; KK has no
  measurements of it either.
- KK's jitter handling for the vote (`ApplyGiJitter`), the dump lines (`--native_dump_frame`), the 600-frame statistics
  line, the launcher row, the F8 panel, the Original look, `nr_game_gi_tests` and `nr_ssgi_bench`, the DXIL shaders.
- KK's GI timer's panel readout; `native_gi_gpu_time` logs only.

## Settings (cvars, defined in ssgi.cpp, all read every frame)

| Cvar | Default | Meaning |
|---|---|---|
| `native_gi_mode` | 1 | 0 off, 1 on, 2 the bounce light only (KK's debug view 1) |
| `native_gi_quality` | 2 | 1 Low, 2 Medium, 3 High (KK's levels) |
| `native_gi_distance` | 2 | 1 Near 16 m, 2 Medium 32 m, 3 Far 64 m, 4 Full |
| `native_gi_intensity` | 1.0 | 0 to 8: 1 normal, 4 strong (the x3 calibration on top) |
| `native_gi_resolution` | auto | auto, full, half (KK's `native_gi_quality`) |
| `native_gi_temporal` | true | the local temporal accumulation |
| `native_gi_gpu_time` | false | duration queries, the average logged every 600 frames |
| `native_gi_radius` | 0 | world units; 0 the level's 3, 5 or 8 |
| `native_gi_thickness` | 0 | world units; 0 a fifth of the radius, 0.25 to 1 |
| `native_gi_albedo` | 0.75 | the brightest albedo assumed, 0.05 to 1 (a picture value) |
| `native_gi_occlusion` | 0 | with the AO off: hidden ambient taken away, 0 to 1 |
| `native_gi_debug` | 0 | 1 the bounce light, 2 the change on grey, 3 the visibility |
| `native_gi_ambient` | 0.25 | the flat ambient light as a grey picture value (port) |
| `native_gi_gamma` | 2.2 | the scene colour's gamma (port) |
| `native_gi_sunlit` | 0.65 | KK's sunlit source restoration; 0 the plain lit colour (port) |

KK's cvar defaults differ: there `native_gi` is off in the plugin (Remastered turns on Medium) and the struct's
distance is Full while the cvar says Medium. The defaults above are the ones asked for (on, Medium, Medium).

## Public API

```cpp
SsgiSettings SsgiSettings::FromCvars();

class Ssgi {
  bool Initialize(Diligent::IRenderDevice* device);
  void Configure(const SsgiSettings& settings);
  struct Inputs {
    Diligent::ITexture* depth;              // HostTarget::texture of the scene depth (D24S8 or D32S8X24)
    Diligent::ITextureView* color;          // HostTarget::view of the scene colour (its RTV)
    Diligent::ITexture* color_texture;      // HostTarget::texture of the scene colour (nullptr: color->GetTexture())
    int32_t x, y; uint32_t width, height;   // the 3D view's picture, host pixels
    GtaoProjection projection;              // DrawPath::TakeProjection
    float z_min, z_max;                     // that camera's viewport depth range
    RenderScale scale;                      // HostTarget::scale
    uint64_t frame;
    const Mat4d* reprojection; uint32_t votes, offers;  // the camera vote, for the local history
    bool ao_on;                             // the AO ran at this moment: the GI only adds light
    const float* ambient;                   // rgb picture values, or nullptr for native_gi_ambient
  };
  bool Apply(Diligent::IDeviceContext* context, const Inputs& in);
  void ForgetTargets();
  Diligent::ITexture* result() const;       // RGBA16F working size: rgb light, a visibility (and step())
  uint32_t TakeApplied();
};
```

`Apply` ends the render pass, unbinds all targets, copies the colour, runs the compute passes, draws the composite
into `color` and leaves no target bound. The depth target ends in the SHADER_RESOURCE state; the next draw rebinds it
once `DrawPath::TargetsChanged()` is called.

## Integration (the exact code for renderer.cpp)

1. `CMakeLists.txt`: `src/ssgi.cpp` after `src/gtao.cpp` in `add_library(native_x_renderer ...)`. `ssgi_shaders.h` is
   a plain header.
2. `THIRD_PARTY_NOTICES.md`: extend the King Kong row with "the global illumination's passes and settings
   (`src/ssgi.cpp`, D133)", and the XeGTAO row with "and its depth passes in `src/ssgi_shaders.h`". The gather's method
   is the paper above (no code). `docs/decisions.md`: D133 from this file.
3. `renderer.cpp`, includes: `#include "ssgi.h"` next to `#include "gtao.h"`.
4. `renderer.cpp`, globals, after `bool g_gtao_done = false;`:

   ```cpp
   Ssgi g_ssgi;           // global illumination at the same moment, after the AO (D133)
   bool g_ssgi_on = false;
   ```

5. `renderer.cpp`, startup, the D132 block becomes:

   ```cpp
   // D132, D133: ambient occlusion and global illumination where the profile names their moment and the main view is
   // found from the draws. Started whatever native_ao_mode and native_gi_mode say, so that the menu can turn them on
   // and off at once.
   if (profile.ao_resolve_caller && profile.upscale_resolve_caller && !profile.view_hooks) {
     g_gtao_on = g_gtao.Initialize(device->device());
     g_ssgi_on = g_ssgi.Initialize(device->device());
     // The view is found from the draws (D119) with the camera vote's registers, as with an upscaler.
     if (g_gtao_on || g_ssgi_on) g_auto_views = true, g_camera_vote = true;
     REXLOG_INFO("[native_x_renderer] ambient occlusion (GTAO, D132): {}", g_gtao_on ? "on" : "could not start");
     REXLOG_INFO("[native_x_renderer] global illumination (SSGI, D133): {}", g_ssgi_on ? "on" : "could not start");
   }
   ```

6. `renderer.cpp`, the draw path (today lines 620 and 627):

   ```cpp
   bool scene = g_scene_view && (g_temporal || g_gtao_on || g_ssgi_on);
   ...
   g_draw->SetProjectionVote((g_gtao_on || g_ssgi_on) && g_scene_view && !g_gtao_done);
   ```

7. `renderer.cpp`: `ApplyAmbientOcclusion()` becomes `ApplyOpaqueEffects()`. `TakeProjection` and `TakeCameraVote`
   both start the next vote, so they are taken once for the two effects:

   ```cpp
   // D132, D133: at the end of the main view's opaque draws, the ambient occlusion (GTAO) multiplied into the scene's
   // colour, then the global illumination (SSGI) added to it, both King Kong Recompiled's, with one camera.
   void ApplyOpaqueEffects() {
     if (!g_scene_color || !g_scene_depth || !g_draw || !g_scene_color->view) return;
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
     RendererTime time;
     const uint32_t width = g_scene_color->scale.HostWidth(g_profile->output_width);
     const uint32_t height = g_scene_color->scale.HostHeight(g_profile->output_height);
     const uint64_t frame = g_device->frames_presented();
     // Without an upscaler the camera vote is the effects' own (their local histories' reprojection); with one,
     // UpscaleScene takes it later in the view.
     Mat4d vote;
     uint32_t votes = 0, offers = 0;
     const bool voted = !g_temporal && g_camera_vote && g_draw->TakeCameraVote(vote, votes, offers);
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
   ```

   And in `ExecuteResolve`, the moment:

   ```cpp
   // D132, D133: the ambient occlusion and the global illumination, at the profile's depth copy that ends the view's
   // opaque draws (once a frame).
   if ((g_gtao_on || g_ssgi_on) && g_scene_view && !g_gtao_done && c.caller == g_profile->ao_resolve_caller &&
       surface.depth && surface.width == g_profile->output_width && surface.height == g_profile->output_height) {
     g_gtao_done = true;
     ApplyOpaqueEffects();
   }
   ```

   The forward declaration and every other call of `ApplyAmbientOcclusion` (there is only the one above today) are
   renamed with it. `g_gtao_done` now means "the opaque effects ran in this frame's view"; it is still reset at
   Present.
8. `ForgetTargets`: next to `if (targets.count) g_gtao.ForgetTargets();` add `if (targets.count) g_ssgi.ForgetTargets();`,
   and the same wherever a later change gives a target a new texture (a live render scale or upscaler switch).
9. The log line with the others:

   ```cpp
   if (g_ssgi_on) {
     REXLOG_INFO("[native_x_renderer] renderer: frame {}: global illumination (SSGI) applied {} times in {} frames",
                 frame, g_ssgi.TakeApplied(), g_uploads.frames);
   }
   ```

## Costs KK measured (RTX 4070 Ti, Vulkan)

- Synthetic bench before the Beta 4 rework, `Gtao::Prepare` plus all GI passes: 720p Low 0.16, Medium 0.22, High
  half 0.28, High full 0.99 ms; 1440p Low 0.64, Medium 0.87, High half 1.09, High full 4.0 ms.
- After the Beta 4 rework (desktop shared): 4K half Low 1.85, Medium 2.45, High 2.97 ms; 1440p Medium 0.97 ms.
- After task 073 (Medium 2 x 6, 2 blur passes): 1440p Medium 0.651 ms, High half 1.273 ms; repeats up to 0.795 / 1.376
  and 1.942 / 3.598 ms under desktop load.
- Distance limit (task 123), outdoors with a long view: 20 to 33 % less (4K High Full 1.93 ms against Far 1.41 ms;
  1440p Medium 0.41 against 0.32 ms); a close up scene saves nothing.
- In game, 4K DLSS Quality (the GI on the 2560 x 1440 render): 0.96 to 1.01 ms of a 9.0 to 9.4 ms GPU frame (AO 0.39 to
  0.40 ms). Frame rate at 1440p DLSS Quality: off 173 FPS, Medium 159, High 153.
- Sharing one prepare between AO and GI would save about 0.02 ms each (REPORT-347a); KK never did it.

This port adds a full copy of the scene colour per frame (about 23 MB of traffic at 3200 x 1800 RGBA8). Battlefield
1943's AO at 3200 x 1800 half resolution measured 1.08 ms Medium (D132); expect the GI at Medium about the same as
KK's 1440p to 4K numbers, roughly 0.7 to 2 ms.

Memory at 3200 x 1800, half resolution (1600 x 912 padded): about 25 MB of R32 images (stored depth, view depth mips,
normals, history depth), 35 MB of RGBA16F light images (a, b, history), 16 MB of radiance mips, and the 23 MB colour
copy: about 100 MB. At full resolution the working images are four times that.

## What could be shared with Gtao later (needs edits to gtao.h)

- The stored depth, the view depth mips and the normals: Gtao would expose `raw_depth()`, `view_depth()` and
  `normals()` plus its XeGTAO constants. Two catches: Gtao smooths its normals in place (the GI wants them
  unsmoothed; KK keeps two instances for that reason), and Gtao's prefilter weights use its own radius (12 near planes
  at strength 3, the GI's 6). Saving: about 0.02 ms (KK's figure) and 3 working images.
- The XeGTAO GLSL helpers and the three depth passes are copied from `gtao_shaders.h` into `ssgi_shaders.h`; a shared
  header would keep them in one place.
- The GPU timer and `CreateShader` / `PointClamp` are copies of gtao.cpp's.

## Doubts and open risks

- Not built or run. Every DiligentCore call mirrors one in `gtao.cpp`, `aa.cpp` or `scaling.cpp`. New for
  NativeXRenderer: RGBA16F storage images (`rgba16f`, a format every Vulkan device stores to), a linear immutable
  sampler with mips in a compute pass, a copy of one mip (`CopyTextureAttribs::SrcMipLevel`), and the dynamic constant
  buffer mapped more than once a frame (before the second and third blur passes). DiligentCore gives a dynamic buffer
  a new region at each discard map and binds it at each dispatch, but no NXR pass does this yet.
- The ambient light is a guess (0.25 grey). The composite's albedo estimate (colour / ambient in the shade) and the
  sunlit classification both depend on it: too low an ambient caps every albedo at the limit and lights shade too
  much; too high makes the bounce weak. A census of Battlefield 1943's lighting constants should replace it
  (`Inputs::ambient`).
- No sun. Bright textures (sand, concrete, sunlit roads) are taken as sunlit and send extra light; KK saw faces glow
  before task 073's rejection. `native_gi_sunlit=0` is the safe fallback.
- Gamma 2.2 is assumed for the scene colour. A colour target in a float format (RGBA16F, R11G11B10F) would hold
  something else; the module does not check the format.
- The moment: everything in the colour target at the depth resolve gets bounce light (and sends it). The first person
  weapon is drawn with its own camera (near 0.1, far 7.5) but read with the world's projection: its view x and y are
  off, so it may get light from itself or the world wrongly. Water, smoke and particles drawn before that resolve
  would be lit too.
- World units are taken as metres (radii 3, 5, 8 m and distances 16, 32, 64 m). That holds if Frostbite's view space
  is metres and the camera vote's projection has no uniform scale.
- With DLSS or FSR 3.1 the GI's noise rotates every frame with no local history; if the upscaler does not smooth it
  well, the light may shimmer. KK kept its own history there.
- The camera vote is shared with the AO; the GI's history needs it to pass KK's GI rule (2 votes, half the offers).
  Without it the GI runs spatially only (fixed noise), which is KK's fallback.
- The history key holds the level, distance, step, radius, thickness, gamma, sunlit, picture and depth range;
  intensity, albedo, occlusion and ambient change only the composite and keep the history.
- `ForgetTargets` resets the history; calling it every frame would disable it.
- KK's open problems carry over: no per object motion in the history (moving vehicles and soldiers may trail light);
  KK's Beta 5 note that a face could look odd with GI on (soldiers' faces are a test case).
