# Night Ops performance pass of 20 November 2026

TekRantGaming's brief (20 Nov 2026, "Night Ops performance optimisation and code audit", run unattended): measure
first, find what Night Ops costs, fix it without changing what is seen, prove each change, and keep looking.

Status labels as in [PERFORMANCE.md](PERFORMANCE.md): **confirmed** (measured before and after on this PC),
**estimated**, **unverified**.

## How it was measured

- **The PC:** Ryzen 7 7800X3D, RTX 4070 Ti, Windows 11. Output 3840 x 2160.
- **The scene:** Wake Island on a local Night Ops server (`run_dedicated_test.ps1 -Night`), the storm at full strength
  from the start (`BF_DEV_NIGHT_RAMP=1`), the test player on the US carrier turning left and right every 6 s, his own
  settings file (Ultra: GTAO, SSGI, grass radius 1440, shadow scale 4, upscaler quality, FXAA). Night Ops turns the
  sky, lamp glow and lens flare on. With `-Bots 12` and `-Fire` (the test player firing) there are effects on screen.
  Scratchpad `perf_night.ps1`.
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

Night Ops at full storm, his settings: 68 to 69 FPS, GPU bound (11 to 12 ms of GPU work a frame, about 12 ms of each
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

The night pass at full storm, his settings: 1.0 to 1.9 ms before without effects (2.5 to 2.9 with), 0.52 ms on
average after (0.82 at the most, run `perf_tilesL_ab`, no effects). With effects on screen the search alone saves a
further 1.2 ms a frame. The frame stays GPU bound: the sky (2.9 ms) and his SSGI and GTAO (2.6 ms) are now the largest
passes.

## Looked at, not changed

- **The sky's cloud march** (about 2.9 ms in a full storm, half size, temporal): the remaining cost is the march over
  visible cloud (48 to 144 steps, five light samples a step). Fewer steps or a quarter size march would change the
  look; left for TekRantGaming to choose.
- **SSGI and GTAO** (1.65 and 0.92 ms): his Ultra settings, the same by day.
- **The game's side** (bf/src): the per frame memory walks found were one time scans or test tools; the team icons'
  per frame cost was fixed earlier the same day (squad_colors.cpp). The renderer's own CPU time is about 2 ms a frame
  and the frame is GPU bound.
- **The night pass's scene copy** (a full size copy each frame): the pass reads the picture it writes, in linear light,
  so it is not a simple blend; kept.
