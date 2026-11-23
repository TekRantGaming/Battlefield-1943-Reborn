# Weather, sky and clouds

Built from 14 to 19 Nov 2026 on `local/pc-improvements` (ThatZaliaSGuy's commits, with Claude). This page says what
each part does, how to set it, and where it lives in the code. The reasons, measurements and test runs behind each
step are in the decision log, `bf/third_party/native-x-renderer/docs/decisions.md` (D-numbers below).

Most of it is the renderer's own (NativeXRenderer, `bf/third_party/native-x-renderer/src/`): `sky.cpp` and `sky.h`
(the sky, clouds, fog, rain, lightning, lens and wetness passes), `storm.h` (the hurricane) and `renderer.cpp` (the
settings, and how the match's world reaches the passes). The game's side is in `bf/src/`: `weather_audio.cpp` (the
weather's sound), `world_sync.cpp` (the match's world in each player's game, the F9 panel) and `gm_host.cpp` (the match
host that keeps the world).

The sky and clouds started from the user's own Beachhead project (its sky, cloud and lightning shaders and its cloud
noise), brought over and rebuilt for this game.

## Contents

- [Quick reference: the settings](#quick-reference-the-settings)
- [Who decides the weather](#who-decides-the-weather)
- [The sky](#the-sky)
- [The clouds](#the-clouds)
- [The hurricane](#the-hurricane)
- [Fog and the storm's mist](#fog-and-the-storms-mist)
- [Rain](#rain)
- [Lightning](#lightning)
- [The weather's sound](#the-weathers-sound)
- [The storm's shake and blurred edges](#the-storms-shake-and-blurred-edges)
- [Wetness: weapons, hands, vehicles and the lens](#wetness-weapons-hands-vehicles-and-the-lens)
- [Developer switches](#developer-switches)
- [Known gaps](#known-gaps)
- [In progress (not yet built or tested)](#in-progress-not-yet-built-or-tested)

## Quick reference: the settings

Since upstream's 20 Nov change (de289206, "the server decides its look"), most of the look is the server's: each join
sets it (ApplyServerLook). On a regular server it is off, as in the released game; on a Night Ops server it is on; in
a Custom Match or on LAN it is on, and the host's world decides what shows.

| Setting | Cvar | Who sets it | What it does |
|---|---|---|---|
| Day and night | `native_x_renderer_day_night` | the server | The time of day changes the game's own light, the sun's direction and the shadows. |
| Sky (Volumetric clouds) | `native_x_renderer_sky` | the server | The renderer's own sky in place of the game's painted one. |
| Rain on the lens | `native_x_renderer_rain_lens` | the server | Drops gather on the view in the rain and run down it. |
| Wet weapon and hands | `native_x_renderer_wetness` | the server | What you hold (and vehicles) gets wet in the rain and the sea. |
| Graphics > Weather > Lightning | `native_x_renderer_lightning` | the player (on) | A storm's flashes, bolts and their light. Off, the storm keeps its rain, mist and wind. |
| Graphics > Weather > Storm shake | `native_x_renderer_storm_sway` | the player (on) | A strong storm shakes the view lightly and blurs its edges. |
| Audio > Weather volume | `bf_weather_volume` | the player (100) | The rain, wind and thunder, 0 to 100%. |
| (no row) | `native_x_renderer_cloud_shadows` | on | The clouds' shadows on the ground by day (D214). |

Most of the weather shows only with the Volumetric clouds sky.

## Who decides the weather

The time of day and the weather are the match's, not the player's (14 and 15 Nov 2026: "Players use what the server
sets"). They left the graphics settings.

- **The match's host keeps the world:** its clock, the weather, cloud cover, how fast the clouds change, distance fog,
  ground fog, wind and the cloud height (`bf/src/gm_host.cpp`, the `bf_world_*` cvars below). Every player's game asks
  for it and draws the same sky (`bf/src/world_sync.cpp`). Lightning is timed by the world's clock alone, so every
  player sees the same flashes.
- **Custom Match** sets it on its **Weather and time** page: Time passes, Time of day, A day lasts, Cloud cover,
  Cloud change rate, Weather, Dynamic storm, Distance fog, Ground fog, Wind and Cloud height.
- **The F9 panel** lets the match's admin change it while the match runs (the host's game in a Custom Match; on a
  server, the players in its admin list). Its rows: Time passes, Time of day, A day lasts, Cloud cover, Cloud change
  rate, Weather, Distance fog, Ground fog, Wind, Cloud height and Dynamic storm. The mouse works on every slider, a
  value can be typed in and the arrows step it.
- **Changes ease in** (D211): the weather, cloud cover, fogs, wind and cloud base move toward what is set over about
  25 seconds for a whole change of the weather, instead of jumping.
- **The wind follows the weather** (D207) until its own row is set apart from it; set back to the weather's value, it
  follows again.
- **Dynamic storm** (D209): the host takes the weather up to a full storm over each round and back. See
  [BOTS-AND-CUSTOM-MATCH.md](BOTS-AND-CUSTOM-MATCH.md#dynamic-storm).
- **Night Ops servers** (upstream, 20 Nov): the world held at 19:00, and the storm built by the round's tickets: a clear
  starry night for the first 40% of the tickets lost, the clouds gathering to 55%, the storm rolling in to 100% weather
  by 80%, then the full storm; a new round starts clear. On a Night Ops server the F9 panel no longer sets the clouds,
  weather or fog. `BF_DEV_NIGHT_RAMP=<seconds>` (tests) runs the build by time.

| Host cvar | Default | What it sets |
|---|---|---|
| `bf_world_day_night` | false | The light follows the match's clock. |
| `bf_world_time` | 16.0 | The time of day at the start, 0 to 24. |
| `bf_world_day_minutes` | 0 | Minutes a whole day and night take; 0: the time stands still. |
| `bf_world_cloud_cover` | 0.35 | Cloud cover, 0 to 1. |
| `bf_world_cloud_rate` | 0.33 | How fast clouds form and change, 0 to 1. |
| `bf_world_weather` | 0.0 | The weather, 0 fair to 1 a storm. |
| `bf_world_fog` | 0.0 | Distance fog, 0 to 1. |
| `bf_world_ground_fog` | 0.0 | Ground fog, 0 to 1. |
| `bf_world_wind` | 0.3 | Wind, 0 calm to 1 a gale. |
| `bf_world_cloud_height` | 450 | The cloud base over the sea, metres (100 to 1500). |
| `bf_world_dynamic_storm` | false | The storm builds and eases over each round. |
| `bf_world_admins` | "" | Player IDs (comma separated) allowed to change the world while it runs. |

Developers: `native_x_renderer_world_own` (off) makes a game use its own `native_x_renderer_*` world settings even in a
match whose server keeps them.

## The sky

With the **Volumetric clouds** sky the renderer draws a sky of its own where the view shows no world (where the
game's sky was): a gradient from the haze at the horizon to the zenith, the sun's glow and disc, and at night the stars
and the moon. The time of day turns the sun and the moon and blends the colours from day through dusk to night.

- **On every map** (D188): the game's own sky draws (its tint, painted clouds and haze) are left out on Wake Island,
  Guadalcanal, Iwo Jima and Coral Sea.
- **No flicker on effects** (D179, D201): on frames with a muzzle flash or an explosion the game draws its sky with
  other shaders, and its daylit sky showed through for those frames. Those variants are passed over too, on every
  map, found by the sky's own shape (its vertex shader and vertex count) so ones never seen are caught as well.
- **Looking straight up** (D180): with no ground in view the sky still knows where the eye is (from the world's lit
  draws), so climbing in a plane the clouds come nearer as they should.
- **The game's light follows the sky** (`native_x_renderer_sky_light`, on; `native_x_renderer_sky_sun_moves`, on): the
  daylight on the ground and the shadows' direction turn with the sky's sun and moon. With weather the sun's light
  falls (to a quarter at a full overcast) and the sky's turns grey.
- `native_x_renderer_sky_exposure` (3.0) is the sky's brightness; `native_x_renderer_sun_azimuth` (-1: from the game's
  own sun) where the sun stands at noon.

The night sky first had a style of each level's own (D169: stars, the Milky Way, a moon, per map). That was taken out
on 16 Nov with the old night look (D176); the renderer's sky now gives the night its stars and moon, and the night's
lamps stay (see [GRAPHICS-UPGRADES.md](GRAPHICS-UPGRADES.md)).

## The clouds

The clouds are volumes, marched along each pixel's ray through tiling noise (shape and detail volumes made at start-up)
and lit toward the sun or the moon, at half the view's size and blended over the last frames to smooth them.

- **In the world, low enough to fly through** (15 Nov): the layer is a model of itself at the game's scale, its base at
  the match's cloud height over the sea (`native_x_renderer_cloud_height`, 450 m; 100 m at the lowest since D180). A
  plane in a cloud fades into it, and the ground under a cloud seen from above is hidden by it.
- **Taller and fuller** (D180): the low layer is 2800 m deep in its own scale, of a taller kind.
- **Layers at several heights** (D211): the low cumulus layer; an altocumulus layer about 5 km up (small rounded
  cells in patches, more of them as the weather outside the storm rises); and cirrus about 10 km up, thicker as the
  hurricane comes.
- **Storm clouds** (D208, D211): in the storm's rain bands the low layer takes the cumulonimbus kind (the layer's whole
  depth), and over the storm the cover closes toward a whole deck, its density up to 1.9 times.
- **A volutus** (D211): a roll cloud low in the layer along the hurricane's gust front, just outside its rain shield,
  broken along its length, from about a third of the match's weather to about 95%.
- **The storm's wall from outside** (D212, D213): while the eye is outside the hurricane, the sky draws its towers
  toward it, 0.6 km tall as it forms and 2 km at full strength, with an anvil's flat top, dark blue-grey with the top
  lit and its foot in the haze.
- **How fast they change:** the clouds' forming clock runs at the Cloud change rate (`bf_world_cloud_rate`, 0.33,
  where 1 is Beachhead's full speed), times 1 to 3 as the match's weather goes from 25% to 100% (D213, `storm.h`
  RateFactor).
- **Cloud shadows** (D214, D215): by day (the sun over 3 degrees) each point seen looks toward the sun through the same
  clouds that are drawn, the hurricane's included, and loses that share of the direct sun, so their shadows move with
  them. Darkened in steps at the user's word to about 31% darker ground than in the sun. Under the weather's own
  overcast, where there is little direct sun left, they are kept lighter (D217). `native_x_renderer_cloud_shadows`.

## The hurricane

Since D208 the weather is a tropical hurricane over the level, local to where it is (`storm.h`):

- **The match's weather is the storm's stage.** Under 25% there is no hurricane: the weather is the same everywhere.
  From 25% it forms out at sea and comes in as the weather rises; at 100% its rain shield covers the level and its calm
  eye passes over part of it.
- **Sized to the level** (D213): from the level's reach (its capture points' farthest from their middle, at least
  300 m; Guadalcanal about 710 m). At 25% it is 4 reaches out with a shield 0.45 reaches across; at 100% its centre is
  0.3 of a reach off the middle and its shield 1.5 reaches (about the level's own size). It is never bigger than the
  map.
- **Its strength at a place:** 0.15 in the eye, full at the eyewall, full out to 0.6 of the shield and fading to its
  edge, with two rain bands spiralling in (anticlockwise) and at least 0.75 between them. Its bearing turns slowly with
  the world's clock, so every player sees it in the same place.
- **The weather where you are** is the larger of the storm's strength there and the weather outside it (at most 0.15).
  The rain, the mist, the light, the wetness and the rain's sound follow it. The wind is the match's own everywhere:
  outside the storm there is only the wind and a far rumble.
- **Lightning falls only under it** (D208, D213): see [Lightning](#lightning).

## Fog and the storm's mist

Drawn by the fog pass (`sky.cpp` Fog) over the view by its depth, after the game's water.

- **Distance fog** (`bf_world_fog`): a haze between the eye and what is far, thinner higher up.
- **Ground fog** (`bf_world_ground_fog`): a low layer over the sea and the shore in drifting patches.
- **The storm's mist** (D194): from 75% of the weather here the ground fog is thicker (a sight of about 60 m in it at a
  soldier's eye), lower (its top at 12 m), and all over the map, over the island's high ground too, its patches filled
  in and drifting four times as fast.
- **Far rain shafts:** curtains of rain from the cloud base down under the thicker cloud, slanted by the wind.

## Rain

Falls under heavy weather where you are (14 Nov, then D186, D189, D212).

- **Streaks** around the eye, hidden by what is nearer, slanted by the wind: the wind times 0.7, and from 80% of the
  weather up to 65 degrees from upright at a full gale (D189). A streak brightens what is behind it a little, so it
  holds against a flash.
- **What is rained on** is a little darker, with rings and small kick-up splashes where drops land on what lies flat.
- **Not under cover:** what the game's shadow map shows as covered gets no rain. Nothing on the underside of roofs or
  platforms (D186).
- **Not above the clouds** (D212): no rain is drawn with the eye over the cloud base, and the rain's sound falls away.
- The rain is not lit by the lightning (it looked held still under a strobe).

## Lightning

From about 60% of the weather (strongest by 95%), only under the hurricane (D184, D192, D208, D211, D212, D213):

- **When and where** come from the world's clock alone (`sky.cpp` FlashInSlot), so every player sees the same strikes
  and the thunder (below) plays the same ones. A flash every 16 seconds or so at the storm's start, every 5 at its
  worst.
- **A flash** lights the clouds from inside, glows through the fog and lights the ground; one light that swells and
  fades with a flicker, not sharp separate strokes. A far strike lights the scene less (a quarter at 9 km).
- **Bolts to the ground** (D192, from Beachhead's lightning): always within 1.5 km, and out to 7 km for a share that
  grows with the storm. Each bolt is a jagged channel from 30 m into the cloud base down to the ground, with three to
  five branches, a white-hot core in a blue glow, flickering stroke by stroke. None is drawn from in or over the clouds.
- **In-cloud lightning** (D211, D212): a strike inside the clouds within 9 km shows its channel crawling along under
  the cloud base, 1.5 to 4 km long, with four to six branches.
- **Near the players** (D208): three in ten strikes go to one of the players' groups, 60 to 400 m from it, as a bolt.
  The rest fall within the storm's rain shield, kept by the storm's strength there.
- `native_x_renderer_lightning` turns it all off.

## The weather's sound

`bf/src/weather_audio.cpp`, from the project's own recordings in `bf/data/sounds/weather/` (CC0, see its
`LICENSE.txt`): rain in three strengths, wind, a distant rumble, and three kinds of thunder.

- **Mixed into the game's own 5.1 output** (D187), at Master x Effects x Weather volume, silent outside a match.
- **Rain** in three strengths cross-faded with the weather where you are; **wind** with the match's wind; a low
  **rumble** under a storm.
- **Thunder for each strike** (D184, D185): the same strikes the sky draws, after the time the sound takes to come at
  343 m/s. A close crack within 1.5 km, a distant strike to 5 km, a rumble in the clouds beyond. Within 400 m it is a
  startling crack (D208). It comes from where the sky flashed: panned round the speakers by its bearing, quieter and
  duller behind you, following the camera's turn.
- **Under the game's sounds** (D207): the rain and the wind 18 dB down and the thunder 6 dB down, so gunfire and
  explosions stand over the rain and a close strike over the gunfire. In the game's loud moments the rain and the wind
  are turned down further (until the game stands 12 dB over them, 18 dB at most); the thunder at most 3 dB.
- **Under cover** (D187): the rain quieter and muffled, the wind and rumble less so, the thunder a little duller.
- **Above the clouds** (D212): the rain falls to 15% from near the cloud base up; the thunder and the rumble stay.

## The storm's shake and blurred edges

`native_x_renderer_storm_sway` (Storm shake), D189, D193, D194:

- **The shake:** from 80% of the weather and a fresh wind, while you are on foot and in the open: a quick jitter of the
  picture (up to about 2 pixels at 1440 and 0.09 degrees, 6 to 18 Hz) in buffets that swell and ease. Two seconds to
  come, half a second to go; it ends at once in a vehicle or under a roof. It moves the picture, not the game's camera:
  the aim and the crosshair stay.
- **Blurred edges:** in the open, on foot or in a vehicle, the view's edges blur and darken a little toward the
  corners (up to 1.6% of the view's height, 15% darker).

## Wetness: weapons, hands, vehicles and the lens

`native_x_renderer_wetness` and `native_x_renderer_rain_lens` (14 Nov, then D195, D217).

- **What you hold** gets wet in about 12 seconds of rain, at once under the sea, and dries in about 30 seconds under
  cover: darker, with glass-like beads that stand up in the weapon's normal map, water running down it, and a wet
  shine. The hands get wet without drops. All six kits.
- **Vehicles** (D217): jeeps, the Sherman and the Japanese tank get wet in the rain the same way, with raindrop-sized
  drops (about 5 to 14 mm) in plenty.
- **Drip runs** (D217): more frequent and faster the harder it rains and the wetter it is, slowing and thinning as it
  dries under cover; all of it paced by the weather where you are, updated 60 times a second.
- **Rain on the lens:** while you stand in the rain, sparse drops gather on the view and bend it, clearer and catching
  the light; runners streak down it. The middle and the sights stay clear. Drops on a vehicle's glass too (D195).
- **Water from the sea:** running through shallow water splashes the lens (dries in about 3 s); going under soaks it
  (about 5 s).

## Developer switches

Environment variables read by the code (most in developer builds or for test runs):

| Switch | What it does |
|---|---|
| `NXR_FLASH=1` | One lightning flash held on, 5 km off. |
| `NXR_BOLT=<m>` | A bolt held on that far straight ahead of the camera. |
| `NXR_CRAWLER=<m>` | An in-cloud strike's channel held that far ahead. |
| `NXR_LENS_SPLASH=1` | The lens soaked again every six seconds. |
| `NXR_SKY_PHASE_LOG=1` | Logs the pixel shaders drawn around the sky dome once (finding the game's sky variants). |
| `NXR_SKY_DRAWS=<frame>:<count>` | Dumps that many draws of a frame. |
| `NXR_WET_TINT`, `NXR_DRAW_DROPS` | Wetness checks: the wet copies drawn tinted, drops drawn alone. |
| `BF_DEV_WORLD_PANEL=<s>` | Opens the F9 panel by itself in a test run. |
| `BF_DEV_WEATHER_SOUND_OUT` | Writes the weather's mix out for measuring. |

`native_x_renderer_sky_debug` (0) paints parts of the sky's work: 1 the sky's pixels, 4 the fog's distances in bands,
5 its heights over the sea (7 to 12: the sea's reflection, see [GRAPHICS-UPGRADES.md](GRAPHICS-UPGRADES.md)).

## Known gaps

From the decision entries' "not tested" notes, as of 19 Nov 2026:

- Not seen in a real flight: cloud shadows from the air, rain above the clouds, the clouds coming nearer in a climb.
- Not tested in a run: the roll cloud and the altocumulus layer up close; the rain bands' strength floor on screen.
- A dedicated server run by the server package (`gmhost.py`) keeps its own world without the dynamic storm (see
  [BOTS-AND-CUSTOM-MATCH.md](BOTS-AND-CUSTOM-MATCH.md#dedicated-servers)).
- Reported by the player on 19 Nov 2026: the cloud shadows showed on top of the clouds from above them, and were
  still too dark (being fixed, below).

## In progress (not yet built or tested)

Uncommitted changes in the working tree on 20 Nov 2026 (`sky.cpp`, `sky.h`, `renderer.cpp`), carried over upstream's
Night Ops performance work on the same passes (its fog and cloud march changes; `docs/NIGHT-OPS-PERFORMANCE.md`).
They have no decision entries written yet.

- **D221, cloud shadows off the clouds:** the clouds are laid over the ground before the fog pass darkens it, so the
  shadow darkened the clouds too, seen from above. The fog pass now reads the clouds as laid over and gives back what
  the shadow took from them, so only the ground through them is darkened. Lighter too: about 24% (built; checked from
  above only through thick cover, not yet through gaps).
- **D222, the shove of a close strike:** a bolt within 600 m (full within 120 m) shoves the view when its thunder
  arrives: a push away from the strike of up to 1.1% of the view's height that swings back at 2 Hz and settles in about
  a second, a little down and turned, with the thunder's rumble on it. With Storm shake on. Not built.
- **D223, storm clouds dark and lit by lightning:** clouds where the storm is strong go nearly black blue-grey (the
  denser and lower, the darker), and lightning lights them from inside in its own colour, with a wide glow and a bright
  core. While a flash lasts the clouds' frame blend takes most of the new frame, so the flash shows in them. Not built.

Also still to do: thunder for every flash and bolt seen (the sound and the sky already use the same strikes; to be
compared in a storm run), and sun rays around objects, clouds and the first person view.
