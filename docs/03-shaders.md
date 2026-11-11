# Shader preparation (4 Nov 2026)

TekRantGaming's rule: shaders are built on the player's PC from their own game files; we ship no shader code, only our list
(shader hashes, where each shader is in the game files, and pipeline descriptions: shader hashes and render state).

## Why
The runtime translates a Xenos shader and makes a pipeline the first time a draw needs it, on a worker thread; until
it is ready the draw is skipped (objects missing for a moment). Run sh1, fresh cache: 207 pipelines made during play
and 2,191 draws skipped, most of them in the first seconds after spawning. The runtime also keeps what it made in
storage files (`<cache>\shaders\shareable\5841097E.xsh` and `5841097E.rtv.d3d12.xpso`) and makes all of them when
the game starts: run sh2 (seeded storage) translated 290 shaders in 9 ms and made 246 pipelines in 40 ms at start,
and made none during play.

## How
- `tools/shader_list.py` (ours, run by us): reads recorded storage from test runs and the game files, and writes
  `bf/data/shaders/bf1943-shaders.list` plus the merged pipeline descriptions. Pixel shaders are byte for byte in
  the level shader databases (`Levels/*/Shaders/Single.res`, type Dx9ShaderDatabase) and the separate Xenon shader
  resources; vertex shaders differ in 2 to 22 dwords (the vertex fetch fields the game's D3D fills in when a shader
  is bound), kept as XOR masks; two shaders are built into default.xex (read from the loaded image).
- `bf/src/shader_prep.cpp` (in the game): after the xex is loaded and before the runtime opens its storage, reads the
  listed shaders from the player's archives (`.fbrb`, inflated with miniz), applies the masks, checks every shader
  against its hash, and adds them and the pipeline descriptions to the storage. Once per list version (marker
  `bf1943-prepared.txt`). Run sh3, fresh cache: 290 shaders in 2.0 s, then 5 pipelines made during play (new ones).
- The list grows from every recorded run: `python -I tools/shader_list.py --game input/pkg --out bf/data/shaders
  <storage folders>`. 4 Nov: 662 shaders, 669 pipelines (Direct3D 12) from all runs so far.

## NativeXRenderer (6 Nov 2026, what players run)
Players draw through NativeXRenderer (Vulkan), which keeps its own storage, `<cache>
ative_x_renderer841097E.pipelines`
(shader microcode, translations, pipelines; D42) and makes everything in it at its first draw. Before this, nothing
seeded it, so a first session made every pipeline during play (TekRantGaming's first session: 1,368 pipelines, about 31,000
draws skipped, objects flickering in).
- `tools/shader_list.py` reads NativeXRenderer's storage files too (`--scan runs` finds every storage folder) and
  writes `bf/data/shaders/5841097E.nxr`: that file's translations and pipelines (shader hashes, modifications and render
  state), with no shader records. Its shaders join the list (all 1,079 recorded so far are found in the game files).
- `shader_prep.cpp` writes the player's storage on a first start (and when the list changes): the existing records
  stay; the listed shaders are added from the player's archives, then our translations and pipelines.
- NativeXRenderer reports the progress (D134, `native_x_renderer::StorageProgress`) and the shader screen in our menu
  waits on it. Run shnxr3 (fresh cache, the driver's cache cold for most of them): 1,688 pipelines compiled 17 s after
  the first draw; shnxr5 (fresh cache, the driver's warm): 1.7 s, then 0 pipelines created and 0 draws skipped in a
  Wake Island match.
- First start on a PC whose driver has never compiled them (a fresh NVIDIA cache folder, run first1): the archives
  read in 0.9 s on threads (3.4 s before), then 1,688 pipelines in 24 s behind the shader screen (NXR D135: fourteen
  compile threads at normal priority until then, below normal in play); 2 new pipelines at the spawn and none after in
  a Guadalcanal match. Later starts: 1.4 s. Coverage runs on NativeXRenderer (cov_iwo1, cov_coral1, cov_gc1, shnxr5):
  2 to 7 new pipelines a match on each of the four maps.
- To grow the list: play or script runs on NativeXRenderer (each map, vehicles, weapons, effects), then
  `python -I tools/shader_list.py --game input/pkg --out bf/data/shaders --scan runs`.

## Next
- More coverage: every map, vehicles, weapons, effects, menus; a Vulkan recording (Linux uses Vulkan).
- The setup page in our menu: one progress bar for preparation plus the runtime's start time creation.
- Third party: `bf/third_party/miniz` (miniz 3.0.2, MIT, from the official GitHub release).
