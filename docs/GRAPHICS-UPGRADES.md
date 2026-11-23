# Graphics upgrades

What changed in how the game looks from 12 to 19 Nov 2026 on `local/pc-improvements` (ThatZaliaSGuy's commits, with
Claude, and pull requests 11, 16, 32 and 62). The weather, sky and clouds have a page of their own:
[WEATHER-AND-SKY.md](WEATHER-AND-SKY.md).

The renderer is NativeXRenderer (`bf/third_party/native-x-renderer/src/`); the game's side is in `bf/src/`. The
reasons, measurements and test runs behind each step are in the decision log,
`bf/third_party/native-x-renderer/docs/decisions.md` (the D-numbers below).

## Contents

- [Quick reference: the settings](#quick-reference-the-settings)
- [Wider screens and the HUD](#wider-screens-and-the-hud)
- [Antialiasing and upscaling](#antialiasing-and-upscaling)
- [Light and shadows](#light-and-shadows)
- [Night](#night)
- [Reflections](#reflections)
- [Textures and detail](#textures-and-detail)
- [The first person view](#the-first-person-view)
- [Smooth at 60 ticks and at any frame rate](#smooth-at-60-ticks-and-at-any-frame-rate)
- [Memory and weaker PCs](#memory-and-weaker-pcs)
- [Developer switches](#developer-switches)
- [Known gaps and work in progress](#known-gaps-and-work-in-progress)

## Quick reference: the settings

Settings > Graphics, Enhancements or Camera. The defaults are the code's after upstream's changes of 20 Nov 2026
(cbf18714, de289206): Soft shadows, Reflections and ADS depth of field off and Detail distance 1x by default, as the
released game; and Day and night, the sky, rain on the lens, wetness, Lamp glow and Lens flare set by the server on
each join (off on a regular server, on for Night Ops, on in a Custom Match or on LAN).

| Section > row | Cvar | Default | Choices |
|---|---|---|---|
| Image > Antialiasing | `bf_post_aa` | the renderer's (`native_x_renderer_antialiasing`, smaa) | off, FXAA, SMAA low to ultra, CMAA 2, CMAA 2 Ultra |
| Image > MSAA | `native_x_renderer_msaa` | 1 (off) | off, 2x, 4x |
| Upscaling > Upscaler, Upscaler mode, Upscaler sharpness | `bf_temporal`, `bf_temporal_mode`, `native_x_renderer_upscaler_sharpness` | off, native, 0 | FSR 4.1, FSR 3.1, XeSS 3, DLSS, TAA; Native AA to Ultra Performance |
| Upscaling > Spatial upscaler, its mode | `native_x_renderer_scaling`, `bf_upscaler_mode` | AMD FSR 1 at Quality for new installs (NVIDIA NIS on NVIDIA cards) | bilinear, FSR 1, NIS |
| Image > Shadow quality | `native_x_renderer_shadow_scale` | Original | Original, High (2x), Ultra (4x) |
| Lighting > Soft shadows | `native_x_renderer_soft_shadows` | off | off, soft, softer |
| Lighting > Reflections | `native_x_renderer_reflections` | off | off, SSR |
| (the server's) Lamp glow | `native_x_renderer_lamp_bloom` | set on joining | |
| (the server's) Lens flare | `native_x_renderer_lens_flare` | set on joining | |
| HUD > HUD scale | `native_x_renderer_hud_scale` | 100 | 50 to 100% |
| HUD > Minimap size | `native_x_renderer_minimap_scale` | 100 | 75, 100, 125% |
| Textures > HUD scale | `bf_hud_scale` | original | Original (Xbox 360), High-Res |
| Textures > HD textures | `native_x_renderer_hd_textures` | off | needs an HD pack |
| Distance > Detail distance (Enhancements) | `bf_lod_distance` | 1 | 1x (original) to 8x |
| Enhancements > Sniper scope (experimental) | `bf_scope_pip` | off | |
| Camera > ADS depth of field, its strength, quality | `native_x_renderer_ads_dof`, `native_x_renderer_ads_dof_strength`, `native_x_renderer_dof_quality` | off, default, medium | Low, Default, Strong; low, medium, high |
| Field of view > Custom FOV, Weapon FOV, ADS FOV, Vehicle FOV | `bf_weapon_fov_custom`, `bf_weapon_fov`, `bf_ads_fov`, `bf_fov_vehicle` | off, 86, 86, game's | 86 to 120 degrees |
| Image > Animation smoothing | `bf_smooth_bones` | off (experimental) | |

## Wider screens and the HUD

- **Ultrawide** (12 Nov, pull request 11, D166): the 3D view fills a screen of any shape; the game's cameras are
  widened (`bf/src/fov.cpp`) and the renderer draws the view at a render scale per axis
  (`native_x_renderer_fill_screen`, on).
- **The HUD placed draw by draw** (D166): after the game's composite, every 2D draw is placed on its own from its quad
  and its vertex shader's constants (`draw.cpp` ClassifyHudDraw): what spans the canvas is stretched with the view; a
  scope's mask larger than the screen is drawn twice; what starts at a canvas edge reaches the screen's edge; the HUD's
  left and right thirds sit at the screen's edges (`native_x_renderer_hud_layout`, edges); the rest stays in a 16:9
  box in the middle.
- **World markers** (D166, D167, D206): flag icons, squad icons and the squad leader's order ring sit where the wider
  view shows their place. Only marker-sized draws follow a marker, so a plane's gunsight and a scope's crosshair are
  never pulled apart.
- **HUD scale and minimap size** (D167, 13 Nov): the HUD's size (50 to 100%) and the minimap's (75, 100 or 125%),
  each part scaled about its own corner. The flags' markers in the world keep the game's size.
- **Sharper HUD text and icons** (D200, Textures > HUD scale: High-Res): the game's text and icons were made for 720p;
  sharper copies of its 8-bit textures and its atlases are built at 2 to 8 times their size from the game's own.
- **Squad colours** (18 and 19 Nov): the squad and the player's own icons in a brighter green on the HUD, the minimap
  and the maps (`bf_hud_squad_bright_green`), and squad mates' icons in the world following them every frame
  (`bf_hud_squad_icons_smooth`).

## Antialiasing and upscaling

From pull request 16 (13 Nov, D167):

- **On the finished picture:** FXAA, SMAA at its four presets, and Intel CMAA 2 (two presets, compute passes ported to
  GLSL). It switches while the game runs.
- **MSAA 2x and 4x** (13 Nov): the renderer's own multisampling of the game's view, for what is thinner than a pixel
  far off (masts, power lines). Works with the antialiasing above; costs frame rate and video memory.
- **Temporal upscalers** on the 3D view before the game's post-processing: AMD FSR 4.1 and FSR 3.1 (from AMD's FSR SDK
  2.4.0), Intel XeSS 3, NVIDIA DLSS, and a plain TAA of the renderer's own. Their libraries are fetched when the build
  is configured, checked by their SHA256 and loaded only from the game's folder. With one chosen, the antialiasing row
  is greyed (the upscaler antialiases itself). At start the game asks AMD's and Intel's libraries whether this GPU runs
  them, for the menu.
- **Spatial upscaling and sharpening** when the game is drawn below the screen's size: bilinear with AMD CAS, FSR 1
  with RCAS, or NVIDIA NIS.

## Light and shadows

- **The day and night cycle's light everywhere** (D199): the time of day's sun, sky and ground light reach every one
  of the game's 832 lit pixel shaders, read from its own archives (105 had kept the day's full light at any hour,
  among them some soldiers' and arms'). The shadows of skinned meshes turn with the moved sun too.
- **The first person arms and gun** take the soft shadows and the day's light (D219: checked, nothing to change). At
  night they are lit evenly by lamps and effects, not in patches (D188).
- **Soft sun shadows** (D197, Lighting > Soft shadows): the game filters its shadow map itself, one texel wide whatever
  casts the shadow. Now its shadow compare gets a penumbra that widens with the distance to what casts it: sharp at a
  post's foot, soft under a treetop. Softer widens it more.
- **Shadow quality** (Image): the shadow maps drawn at 2x or 4x the game's size (D92). At 3440 x 1440 the render scale
  (2.69) is already above 2x, so High does nothing there; Ultra gives 4096 x 4096.
- **Shadows turned with the sky's sun** (D181, D205, D218): with day and night on, the game's shadows are turned to the
  sky's sun and moon. Three fixes keep them on their objects: the turned depth centred on the camera (D181, they slid
  with the camera on foot); the turn's pivot on the ground, not in the air at a plane's camera (D205); and from 60 to
  150 m over the ground the turn fades out to the game's own direction (D218).
- **Sky reflections dimmed at night** (D182): 71 of the game's shaders reflect a cube map of a day's sky (the sea, shiny
  props, glass). It now follows the hour: 1 at noon, about 0.09 at night, redder at dusk, greyer under an overcast.
- **Ambient occlusion and global illumination** (GTAO and SSGI, from before 12 Nov: D132, D133) are on in the Medium to
  Ultra presets (Lighting > Ambient occlusion, Global illumination).

## Night

The game had no night. Night first came as separate night maps (12 to 14 Nov: D162's moonlit grade over the finished
picture, D169's per-level night sky). Since 16 Nov the day and night cycle draws the night itself (with the
renderer's sky, [WEATHER-AND-SKY.md](WEATHER-AND-SKY.md#the-sky)), the separate night look is gone (D175, D176), and
the lights stay and follow the cycle: they come on from 17:47 and are full at 18:20, fade from 05:40 and are out at
06:13.

- **The effects' own light** (D163, `native_x_renderer_night_glow`, 1): muzzle flashes, tracers, fire, sparks and
  explosions glow and light what is round them, up to four of the brightest at once. Not daylit at night or in storms
  (D195).
- **The level's lamps** (D163, D170, D177, `native_x_renderer_night_lamps`, 1): street lamps and work lamps as sodium
  lamps (amber), lit buildings (some houses lived in), fires that waver, the control towers' cabs, red lights on masts
  and water towers. A lamp lights the ground with the surface's day colour. Lamps are seen and light the ground across
  the level, through the haze (D195).
- **Lamp glass, bloom and lens flare** (D183, D195): a lamp's own glass glows in its colour (no floating ball of
  light), a coloured bulb keeps its colour (D178), with a soft bloom (Lamp glow) and a subtle lens flare, halo and haze
  from the brightest lamps in view (Lens flare).
- **Vehicle headlights** (D177, D178): jeeps, the Sherman (two) and the Japanese tank (one), only while someone is in
  the vehicle (the player's own, or one that drives; kept 6 s after it stops).
- **Lighthouses and buoys** (D170, D195): Iwo Jima's lighthouse beam turns over the sea out to 1.6 km (since 20 Nov one
  beam, dimmer, slower and amber, cbf18714); the buoys that mark the map's edges stay lit.

## Reflections

- **The sea's reflection** (D190, D204, Lighting > Reflections: SSR): the sea's shaders' day sky cube map is left out
  and the renderer reflects the scene itself: the reflected ray marched through the view's depth, rippled by small
  waves (more with the wind), the renderer's sky and clouds where the ray finds no scene, as much as Fresnel gives. It
  keeps the game's foam, crests and the shoreline wash (shallows under 0.6 m and near-white water get none), and nothing
  see-through drawn over the sea after it (flags, smoke). At the horizon the far sea reflects as a flat sea, without
  mirroring its own pale water (D204).
- **Window glass** (D191, `native_x_renderer_glass_reflections`): started and experimental, off: not yet seen working.
  The panes keep D182's dimmed cube map.

## Textures and detail

- **Detail distance** (D202, Enhancements > Distance): how far models keep their full mesh, and small things stay drawn
  at all: 1x (the game's own, the default since 20 Nov) to 8x. It writes the game's own level of detail scale. Its help text says
  what 2x and 8x show and cost.
- **HD textures** (13 Nov): an HD pack for the HUD, the deploy screen and the icon atlases, with prompts and flag
  letters drawn at the pack's size. No HD art is in git; the pack is built locally.
- **Sharper 8-bit textures** (D200): see [the HUD](#wider-screens-and-the-hud).

## The first person view

- **Field of view** (13 Nov, `bf/src/fov.cpp`): the camera's field of view (86 to 120 degrees at 16:9), and with Custom
  FOV the weapon drawn at its own Weapon FOV and ADS FOV, and vehicles at Vehicle FOV. The weapon's mapping fades out
  through a scope.
- **ADS depth of field** (D196, Camera): while aiming down the sights the back of the gun at the eye blurs, graded
  along the gun so the front sight stays clear (the sights too with Iron Sight on), gathered at half size without
  grain; strength Low, Default or Strong; quality low, medium or high (24, 48 or 80 taps). No crosshair dot on the
  bazookas while aiming.
- **Picture in picture sniper scope** (D168, Enhancements, experimental, off): only the scope's lens zooms in and the
  world round it stays unzoomed.
- **SMG Iron Sight** (`bf_smg_sights`, HUD > SMG aiming): the gun's own sights brought to the eye instead of the game's
  zoom; tuned sight values are the defaults (the tuning tool is internal).
- **Animation smoothing** (D220, Image, experimental): each bone's movement blended between the game's ticks, the
  first person arms and gun too, with the aim's own per-frame movement kept (`bf_smooth_bones_aim`).
- **Recoil and motion blur:** the recoil kick eases back every frame (`bf_smooth_recoil`); a motion blur of the
  renderer's own (Camera > Motion blur, off by default), with soldiers and vehicles blurring by their own motion
  (Object motion blur), and no object motion blur while orbiting a vehicle with free look.

## Smooth at 60 ticks and at any frame rate

The game runs at 60 ticks a second (`bf_tick_rate`, 60; the Xbox's was 30). Things the game counts per tick are scaled
so they behave per second as at 30:

- the soldier's movement, jump and air steering (`bf_move_tick_scale`), and swimmers getting out of the sea
  (`bf_swim_exit_tick_scale`), 18 Nov;
- the first person gun's lag, sway and springs, and the camera's recoil spring (D203, `bf_gun_tick_scale`).

And at any frame rate:

- **30 FPS at 60 ticks** (D210): choosing 30 FPS no longer turns the game's one-tick-a-frame timing back on, which
  walked the soldier at half speed and pulled him back.
- **Motion between ticks** (`bf_smooth_motion`, on): vehicles, soldiers and everything animated move on every frame
  above 30 FPS; particles (`bf_effects_tick`), physics debris (`bf_physics_tick_time`) and the vehicle camera
  (`bf_vehicle_camera_tick`) keep the game's timing so nothing speeds up.

## Memory and weaker PCs

15 Nov (D171 to D174), measured in the decision entries:

- On fewer than 4 cores there is no recorder thread and no spinning waits (D171).
- The renderer's pipeline cache let go once the stored pipelines are compiled (about 80 MB, D173), and the guest
  memory mirror a sparse buffer (about 340 MB less graphics memory, D174). The dynamic heap went to 64 MB (D172) and
  back to 256 MB on 20 Nov (87538683: no forced waits for the GPU).
- Night Ops performance work (20 Nov, upstream): the clouds' march, the fog, the rain and the lamps' light made cheaper
  for the same picture; see `docs/NIGHT-OPS-PERFORMANCE.md`.

## Developer switches

| Switch | What it does |
|---|---|
| `native_x_renderer_debug_hud` | Logs the 2D draws after the composite every 10 s, and each kind of HUD draw once. |
| `NXR_HUD_MARKER_LOG=1` | Logs HUD draws that keep moving and are not taken for world markers. |
| `NXR_BOX_LOG=1` | (Uncommitted.) Logs each full-canvas draw kept in the 16:9 box. |
| `NXR_SEA_DEBUG=1` | Paints in red where the sea's reflection is left out. |
| `native_x_renderer_sky_debug` 7 to 12 | The sea's reflection's parts (the sky it mirrors, its strength, the water's own colour). |
| `NXR_GLASS_DEBUG=1..4` | The window glass reflection's panes and mask. |
| `NXR_SHADOW_LOG` | The shadows' turn, casters and receivers. |
| `NXR_WEAPON_DUMP=<s>` | The first person weapon's draws, shaders and light rows on two frames. |
| `NXR_DUMP_PS=<hashes>` | Disassembles those pixel shaders. |
| `BF_DEV_CAMERA_TOUR`, `BF_DEV_CAMERA_POINTS`, `BF_DEV_CAMERA_AT`, `BF_DEV_CAMERA_GLIDE` | Test cameras: random spots, fixed views, a camera held behind a place, a glide. |
| `BF_DEV_BONELOG_AFTER`, `BF_DUMP_VIRT`, `BF_WATCH_VIRT` | The bones log, and dumping or watching game memory (the gun's springs). |

## Known gaps and work in progress

As of 20 Nov 2026:

- **The loading screen's fade on wider screens:** the fade to black over the loading screen (and into the deploy
  screen) covers only the 16:9 middle while the loading picture fills the screen. Confirmed in frames captured at
  3440 x 1440; `NXR_BOX_LOG` was added to find that draw. Not fixed yet.
- **The shoreline wash under the sea's reflection:** the player reports the wash not showing over the reflection on all
  maps. Fixed-view runs with the reflection on and off were taken on Wake Island; not compared yet.
- **Sun rays** around objects, clouds and the first person view: asked for, not started.
- Not seen in a real flight: shadows from high up (D218).
- Window glass reflections (D191) stay off until they work.
