# Night Ops performance pass of 20 November 2026

TekRantGaming's brief (20 Nov 2026, "Night Ops performance optimisation and code audit", run unattended): measure
first, find what Night Ops costs, fix it without changing what is seen, prove each change, and keep looking.

Status labels as in [PERFORMANCE.md](PERFORMANCE.md): **confirmed** (measured before and after on this PC),
**estimated**, **unverified**.

## How it was measured

- **The PC:** Ryzen 7 7800X3D, RTX 4070 Ti, Windows 11. Output 3840 x 2160.
- **The scene:** Wake Island on a local Night Ops server (`run_dedicated_test.ps1 -Night`), the test player on the US
  carrier turning left and right every 6 s, his own
  settings file (Ultra: GTAO, SSGI, grass radius 1440, shadow scale 4, upscaler quality, FXAA). Night Ops turns the
  sky, lamp glow and lens flare on. With `-Bots 12` and `-Fire` (the test player firing) there are effects on screen.
  Scratchpad `perf_night.ps1`.
- **Correction (found later the same day, TekRantGaming's question):** the runs above were all at the **clear night**
  stage (clouds 15%, no rain), not the full storm they were meant to be: `BF_DEV_NIGHT_RAMP` builds the storm only
  through the server's match file, and `run_dedicated_test.ps1` did not pass `--match-file` (the manager does), so the
  server stayed at "Night Ops at 0%". Every result below headed "full storm" is the clear night's. The storm's stages and
  other places on the map are measured in [the storm and the map](#the-storm-and-the-map) below.
- **GPU time by pass (new, on by default):** `native_x_renderer_gpu_sections` puts a duration query round each of the
  renderer's own passes and logs every 600 frames "GPU ms a frame by pass: sky ..., fog and rain ..., GTAO ...". The
  night pass logs its own time and the effects' lights' search's: "night pass GPU ... (the effects' lights' search ...)".
- **A/B in one run:** the view changes as the camera turns, so two runs are not comparable (one run had effects on
  screen, the other none). Each change has a test switch that turns it on and off by turns, 600 frames each, and the
  windows are compared at the same number of lamps in view: `BF_NIGHT_AB` (lamp visibility), `BF_NIGHT_FX_AB` (effects'
  lights' search), `BF_NIGHT_TILES_AB` (light tiles), `BF_CLOUD_AB` (cloud skip), `BF_NIGHT_PARTS` (flare, bloom, lamp
  light off by turns).
- **Same picture, checked:** `BF_NIGHT_FX_CHECK` and `BF_NIGHT_TILES_CHECK` draw the old way and the new way in the
  same frame and compare the results read back.

## The baseline

Night Ops (clear night stage, see the correction), his settings: 68 to 69 FPS, GPU bound (11 to 12 ms of GPU work a frame, about 12 ms of each
frame spent waiting for the GPU). By pass: sky 3.1 ms, SSGI 1.65, night pass 1.0 to 1.9 without effects and 2.5 to 2.9
with them, GTAO 0.92, fog and rain 0.6, FXAA 0.45, grass 0.05. SSGI and GTAO are his Ultra choices and are the same by
day; the sky and the night pass are what Night Ops adds.

## Changes

### 1. The effects' lights' search: 1.22 ms to 0.03 ms (confirmed)

- **Problem:** whenever the game's effects (muzzle flashes, fire, explosions) were on screen, the night pass took
  about 1.2 ms more.
- **Cause:** D163's search for the four brightest hot places in the effects' layer ran as a pixel pass of 8 pixels
  (a 4 x 2 target): each pixel scanned the whole coarse mip (about 48 x 27 texels) up to four times, one texel after
  another. Eight GPU threads did 1.24 ms of serial work for eight texels of output.
- **Fix:** the same search as a compute shader (night.h `kComputeLights`): 256 threads share each of the four scans
  and keep the best of their texels, ties going to the first in the old scan order, so the same places are picked;
  then four threads place their light exactly as before. The old pass stays as the fallback.
- **Measured** (`BF_NIGHT_FX_AB`, 12 bots, the player firing): the search 1.217 to 1.235 ms (old) against 0.030 to
  0.032 ms (compute) in alternate windows; the night pass 2.0 to 2.9 ms against 0.85 to 1.75.
- **Same result:** `BF_NIGHT_FX_CHECK` compared both searches' 4 x 2 results in 1042 frames, 204 of them with 1 to 4
  lights: identical every time (largest difference 0.000000).

### 2. The lamps' light by screen tiles: about a third of the night pass (confirmed)

- **Problem:** the night pass's cost grew with the lamps in view; turning the lamps' light off by turns
  (`BF_NIGHT_PARTS`) showed it was most of the pass (0.45 to 0.65 ms without it, up to 1.85 with it), while the lens
  flare and the bloom cost little.
- **Cause:** every pixel of the picture looped over every light in view (up to 96), though a light only lights what is
  within its range. A second loop over every light found the lighthouse beams.
- **Fix:** the CPU projects each light's range (the cube round its sphere) onto the picture and lists, for each of
  16 x 9 tiles, the lights whose box touches it (a bit mask a tile, in the night constants); a pixel loops over its
  tile's lights only, in the same order. A light whose box reaches behind the camera or holds it is in every tile. The
  beams have their own list. A light left out of a tile cannot reach any of its pixels, so nothing changes.
- **Measured** (`BF_NIGHT_TILES_AB`): at about 83 lights a frame, 0.82 ms tiled against 1.29 ms; the part that grows
  with the lights halved (fitted over 31 windows).
- **Same picture:** `BF_NIGHT_TILES_CHECK` drew the pass tiled and untiled from the same input in 174 frames at
  3840 x 2160 and compared them: 0 pixels differed.

### 3. The lamps' visibility found once a frame: about 13% of the night pass (confirmed)

- **Cause:** for a lamp's bloom, every pixel in the bloom tested whether the lamp's middle is in view and on its glass
  (`LampSeen` and `GlassAt`, 16 depth reads), and for the lens flare every pixel of the picture did it for the four
  brightest lamps, though the answers depend on the lamp alone.
- **Fix:** a pass of one pixel a lamp (a 64 x 1 target) finds both once a frame; the main pass reads them. Two builds of
  the main shader, one for each way. Same math, so the same picture.
- **Measured** (`BF_NIGHT_AB`): at about 42 lamp glows a frame the night pass 1.38 ms against 1.59.

### 4. The clouds' march skips the rays the scene hides: about 2 to 3% of the sky (confirmed, small here)

- **Cause:** the cloud march (half size, up to 144 steps a ray for the long low ones) ran for every pixel, including
  those where the island, the ship or a building stands nearer than the cloud layer; their result was never shown
  (the sky is drawn where nothing is, and the over pass takes only cloud that lies before the scene).
- **Fix:** a cloud texel whose full size pixels, and one more round them (the reach of the bilinear reads), all show
  the scene nearer than where the ray enters the layer is not marched (sky.cpp, kCloudPS).
- **Measured** (`BF_CLOUD_AB`, from the carrier): sky 2.94 ms against 3.02. From the carrier most of the picture is
  sky and sea, so little is hidden; among the island's buildings and hills more is (**estimated** larger, not measured).
- **Picture:** unchanged where seen; a texel uncovered as the camera turns is marched that frame, and the clouds'
  blend keeps its history within this frame's values.

## Overall (confirmed)

The night pass (clear night stage), his settings: 1.0 to 1.9 ms before without effects (2.5 to 2.9 with), 0.52 ms on
average after (0.82 at the most, run `perf_tilesL_ab`, no effects). With effects on screen the search alone saves a
further 1.2 ms a frame. The frame stays GPU bound: the sky (2.9 ms) and his SSGI and GTAO (2.6 ms) are now the largest
passes.

## The whole frame after (confirmed)

The same scene (no bots, clear night stage, his settings with VSync on): the frame interval 14.6 ms before (68 FPS; runs
perf_nightA_old) and 11.3 to 11.8 ms after (85 to 89 FPS; runs perf_utilP, perf_skyQ), GPU 9.8 to 10.3 ms a frame.
nvidia-smi during the run (new in perf_night.ps1): the GPU 83 to 90% busy at 2.3 to 2.7 GHz. The rest is VSync: his
settings have it on, and a frame that misses a refresh waits for the next, so frames of about 10 ms of GPU work come
out at 11 to 12 ms. Under about 8.3 ms of GPU work (120 Hz) the frame would hold the refresh rate.

The sky's 2.9 ms (clear night stage) by part (new sub-timers, sky.cpp): the clouds' march 2.43 ms, their blend over frames 0.14, the sky
drawn 0.34. The march's work is the cloud that is seen (up to 144 steps a ray, each with five samples toward the
moon and one more for the billows' facing); its density already skips the detail noise where the coarse shape is
empty.

## A crash found and fixed on the way (confirmed)

The GPU timers added in this pass kept 16 duration queries a section; with the sky's and the AO's and GI's parts timed
too they passed the Vulkan pool's 256 duration queries, the next allocation failed and the game crashed (a null read,
run perf_cbU_ab, 36 s into a match). Now 4 a section, the timers off by default (`native_x_renderer_gpu_sections`,
for tests; the night pass's own timers with them), and the pool 512 (device.cpp). The same run again: no crash.
Builds pushed before this fix (v0.4.8-testing 4c77eb82 and 34f15612) had the timers on and could crash this way.

## The storm and the map

Run with the test launcher passing the server's match file (`perf_night.ps1` now does), the storm built through every
stage over 5 minutes (`BF_DEV_NIGHT_RAMP=300`), the dev camera touring 24 random inland and coast spots (14 s each),
8 bots, on all four maps (scratchpad `night_suite.ps1`, `stage_report.py`; runs suite_a_*). Each 600 frame reading
put with the stage the server reported then. GPU ms a frame, averages by stage:

| Map | Stage | Frame | GPU | Sky | Fog and rain | Night pass | SSGI | GTAO |
|---|---|---|---|---|---|---|---|---|
| Wake Island | clear night | 14.42 | 10.73 | 2.46 | 0.59 | 0.45 | 1.67 | 0.88 |
| | clouds gather | 13.87 | 11.32 | 1.93 | 0.71 | 0.50 | 2.22 | 1.06 |
| | storm rolls in | 11.27 | 10.84 | 1.35 | 1.04 | 0.48 | 2.09 | 1.00 |
| | full storm | 12.15 | 11.74 | 1.21 | 1.45 (max 2.59) | 0.50 | 2.27 | 1.05 |
| Guadalcanal | clear night | 11.97 | 11.04 | 1.74 | 0.32 | 0.64 | 1.90 | 0.97 |
| | full storm | 12.78 | 12.35 | 0.94 | 1.27 | 0.73 | 2.13 | 1.03 |
| Iwo Jima | clear night | 11.19 | 10.31 | 2.69 | 0.59 | 0.34 | 1.49 | 0.86 |
| | full storm | 12.07 | 11.65 | 1.47 | 1.71 | 0.36 | 1.77 | 0.94 |
| Coral Sea | clear night | 12.76 | 9.50 | 2.26 | 0.62 | 0.32 | 1.42 | 0.88 |
| | full storm | 10.83 | 10.41 | 1.48 | 1.94 | 0.27 | 1.51 | 0.90 |

At the clear night the sky's clouds are the most (their march finds thin cloud far along every ray); in the storm
the deck stops the rays early and the sky costs less, while the fog and rain pass grows to 1.3 to 1.9 ms. The night
pass stays at 0.3 to 0.7 ms everywhere. (Guadalcanal, Iwo Jima and Coral Sea ran with the rain cover change below
already in; Wake without it.)

The fog and rain pass at the full storm by part (`BF_FOG_PARTS`, Wake, touring, run perf_fogparts1; the pass 1.92 ms
with all of it): the rain shafts 0.64 ms, the rain here (streaks, rings, splashes) 0.38, the ground fog 0.26, the
sea's reflection 0.20.

### 5. The fog and rain pass, and the clouds' density: look ups only where they count (confirmed, small)

- **The rain's cover:** each of the six rain cylinders looked up the game's shadow map (`Covered`: under a roof?)
  for every pixel before finding whether a streak is drawn there at all; now the streak first (across the column
  first, before the rest of a drop's hashes), the look up only where one is drawn. The wet surfaces' cover look up
  only where it is used (not the sky's pixels).
- **The rain on the lens:** its beads only outside the clear middle, its runners only outside the sights (both are
  multiplied by those masks).
- **The clouds' density:** inside the layer the fine shape texture only where the height profile lets the cloud
  show (exactly 0 elsewhere while the storm under 0.3 does not fill the deck in).
- **Same picture:** `BF_FOG_CHECK` drew the fog pass both ways from the same picture in 456 frames (Wake, Guadalcanal;
  rain at 0, 0.5 and 1): 0 pixels differed. `BF_CLOUD_CHECK` ran the march both ways in 410 frames on Iwo Jima from
  15% to 91% cover: 0 texels differed.
- **Measured fairly** (`BF_AB_EVERY_FRAME`: the old way and the new by turns every frame, each pass's time kept
  apart by phase, so both meet the same view and the same moment of the storm; Iwo Jima, the storm built over 5
  minutes, touring 40 spots, run perf_everyframe1), ms a frame:

  | Stage | Fog and rain new | old | Clouds' march new | old |
  |---|---|---|---|---|
  | clear night | 0.518 | 0.521 | 2.468 | 2.524 |
  | clouds gather | 0.460 | 0.459 | 2.018 | 2.052 |
  | storm rolls in | 0.736 | 0.735 | 1.011 | 1.016 |
  | full storm | 1.687 | 1.768 | 0.983 | 0.985 |

- **Two changes taken back:** skipping the rain shafts' streak read and the ground fog's patches read where a step
  adds nothing needed `textureGrad` with the gradients (`dFdx`, `dFdy`) at every step to keep the same texture level,
  and that cost more than the reads saved; and working out the clouds' height profile for every sample (not only
  inside the layer) cost more than the shape read saved. The first in-run A/Bs (by 600 frame windows) showed gains
  because their "old" side carried the same extra work, and the windows lined up with the camera tour's spots; the
  before and after suite (suite_a against suite_b, the same tour seeds) showed the fog pass 0.25 to 0.45 ms slower in
  the storm stages on three maps. Both taken back; the table above is after that.

The fog and rain pass at the full storm by part (`BF_FOG_PARTS`, Wake, touring, run perf_fogparts1; 1.92 ms with all
of it): the rain shafts 0.64 ms, the rain here (streaks, rings, splashes) 0.38, the ground fog 0.26, the sea's
reflection 0.20. The shafts' cost is their weather cells' own read and arithmetic at each of 20 steps; no exact
change takes it away.

### 6. The clouds' density: no work for what is multiplied by nothing (confirmed, the largest of the sky's)

- **The bands:** the fair weather deck can be split into three altitude bands, each with its own heights and depths
  from two value noises (four hashes each). With the layer as it is now (2800 m deep, a cloud kind of 0.62: the user's
  "more vertically dense" clouds of 16 Nov 2026) the bands' amount is exactly 0 everywhere, and the noises were still
  worked out for every sample of the march and of each sample's five steps toward the moon, then multiplied by that 0.
  Now only where there are bands (the amount over 0).
- **The column's depth:** each sample read the shape texture at the cloud's foot to learn how deep the column is
  (0.32 to 1 of its full depth). A sample under the column's base or over its full depth is outside whatever the
  read says, so it is not read there (most of the layer's shell lies above the low clouds' tops).
- **Same picture:** `BF_CLOUD_CHECK` (every 60th march made again the old way from the same constants, both read back):
  535 checks on Iwo Jima and 562 on Wake, the storm built from 15% to 91% cover: 0 texels differed.
- **Measured** (`BF_AB_EVERY_FRAME`, the storm built over 5 minutes, touring 40 spots), the clouds' march in ms a frame:

  | Stage | Bands: new | old | Column's depth: new | old |
  |---|---|---|---|---|
  | clear night | 1.814 | 2.518 | 2.465 | 2.510 |
  | clouds gather | 1.511 | 2.037 | 2.009 | 2.044 |
  | storm rolls in | 0.821 | 1.053 | 1.036 | 1.045 |
  | full storm | 0.792 | 1.017 | 0.992 | 1.001 |

  (Bands: Iwo Jima, run perf_bands1, with the column's depth change in both sides. Column's depth: Iwo Jima, run
  perf_reach1.) The fog and rain pass in the same runs: unchanged (within 0.006 ms).

### 7. SSGI and GTAO: the pixels they do not change left alone; the gather's repeated samples (confirmed)

Both are the user's Ultra settings (SSGI quality 3 at half of 4K: 4 slices of 10 steps each way; GTAO high), about
3 ms a frame together, by day too. Timed by part (new GPU sections, Wake at night, run perf_aoparts2), ms a frame:
SSGI 1.8 to 2.7 = its depth and normals 0.09, its light and gather 0.7 to 1.36, its history and blur 0.43 to 0.65, its
4K colour copy 0.21, its composite 0.31; GTAO 0.95 to 1.3 = its depth and normals 0.08, its occlusion and blur 0.55,
its history and apply 0.24 to 0.5.

- **The sky and the far pixels:** SSGI's composite wrote every pixel, the sky's and those past its 64 m reach with the
  colour they already had (read back from its copy); GTAO's apply blended the sky and the pixels past its fade with
  1.0 (times 1). Now neither touches them (`discard`).
  - **Same picture:** `BF_AO_DISCARD_CHECK` (every 60th apply and composite drawn both ways on the same picture, both
    read back): 245 checks of each on Guadalcanal through the storm, 0 pixels differed.
  - **Measured** (`BF_AO_DISCARD_AB`, every frame, Wake through the storm, run perf_aodiscard1): SSGI's composite 0.31
    to 0.17 ms, GTAO's history and apply 0.25 to 0.21; the two passes 0.18 ms a frame lighter at every stage.
- **Tried and taken back: the gather's repeats.** Each slice's steps go outward from the pixel, rounded to whole
  pixels; a step landing on the last step's pixel reads the same sample and adds nothing, so it was passed over. Exact
  (`BF_GI_GATHER_CHECK`: 362 checks on Guadalcanal, 0 texels differed) but no faster (`BF_GI_REPEAT_AB`, Wake through
  the storm, run perf_girepeat1: the gather 0.782 / 0.781, 0.844 / 0.842, 0.832 / 0.831, 0.838 / 0.840 ms new / old by
  stage). It was in the 0.4.8-test8 build (bd92a91f); taken out after.

## Looked at, not changed

- **The sky's cloud march** (about 2.9 ms at the clear night stage, half size, temporal): the remaining cost is the march over
  visible cloud (48 to 144 steps, five light samples a step). Fewer steps or a quarter size march would change the
  look; left for TekRantGaming to choose.
- **The clouds' march, amortised over frames: built as an option, off** (`native_x_renderer_cloud_checkerboard`):
  each frame marches half the cloud texels (a checkerboard turning every frame), packed into a half width pass so
  every GPU thread works (a whole size checkerboard with half the threads idle saved only 0.08 ms), and the blend fills
  the rest from their marched neighbours and the last frames. Measured (`BF_CLOUD_CB_AB`, run perf_cbV_ab): the march
  2.45 ms to 1.30. But the screenshots taken just after the camera turns show the clouds' edges combed in rows
  (shot_60 against shot_64): a visible loss in motion. Off by default; **his call** whether it is worth it, or worth
  a better fill.
- **SSGI and GTAO** (1.8 and 0.95 ms): his Ultra settings (GI quality 3: full resolution, 4 slices of 10 steps), the
  same by day. Both build the view's raw depth and normals from the same depth with the same code (gtao_shaders.h and
  ssgi_shaders.h differ only in comments there); the GI could take the AO's, about 0.1 to 0.2 ms (**estimated**). Their
  depth mips are weighted by each one's own radius, so those differ and cannot be shared. Measured (new sub-timers): 0.08 ms each, and
  the AO smooths its normals in place, so only the raw depth could be shared (under 0.05 ms). Not done.
- **The game's side** (bf/src): the per frame memory walks found were one time scans or test tools; the team icons'
  per frame cost was fixed earlier the same day (squad_colors.cpp). The renderer's own CPU time is about 2 ms a frame
  and the frame is GPU bound.
- **The night pass's scene copy** (a full size copy each frame): the pass reads the picture it writes, in linear light,
  so it is not a simple blend; kept.
