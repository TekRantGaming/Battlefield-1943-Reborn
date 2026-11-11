# Shadows: what is known (5 November 2026)

TekRantGaming: the shadows look wrong, like the shadow problems in Trials. Status: not fixed yet. What is proven so far, and
what to try next.

## How Battlefield 1943 draws sun shadows

From one frame logged with the SDK's frame log (`REX_DEV_FRAME_LOG`, Wake Island, on the carrier):

- Three cascades, each a 1024x1024 D24S8 depth surface in EDRAM. Each one: a clear at 1x (pitch 1040), a fast clear
  at 4x MSAA (pitch 520, compare ALWAYS), 7 to 12 caster draws (LESS_EQUAL, slope scaled depth bias, no pixel
  shader), then a resolve of the depth into a 32 bit colour texture (format 6) at `099AC000`, `09DAC000` and
  `0A1AC000`, 4 MB apart.
- The lit shaders (397 of them in a run) read the three as one stacked texture: `k_24_8`, 1024x1024, 3 layers,
  point sampled. Manual 2x2 filtering: four `tfetch2D` taps half a texel apart, `getWeights2D`, then
  `sgt receiver, tap` and one minus the weighted result scales the sun light.
- The vertex shader computes the three cascade positions from vertex constants c0 to c2, c4 to c6, c8 to c10.
  The pixel shader picks the first cascade the point is inside and passes the layer's centre (1/6, 1/2 or 5/6) as
  the third source component of the `tfetch2D` (`r9.xyz`; ordinary 2D fetches pad with x, `r7.xyx`).

## Proven

- The shadow map is drawn correctly. With `--readback_resolve=full` (and a fix in our SDK copy, below) the
  resolved cascade lands in guest memory; detiled, it is a clean light view of the carrier deck, the boat davits and
  a landing craft, depths 0.94 to 1.0. Its depth gradient matches the cascade 0 matrix exactly.
- The texture is not stale: the SDK reloads it every frame (3,247 loads in run sh15).
- The game draws everything fully lit. Forcing every `k_24_8` fetch to one gives the same picture as stock;
  forcing it to zero, or halving the fetched depth (`REX_DEV_DEPTH_EXP=-1`), makes everything dark. So the
  receiver depths are in the right range (between half the stored depth and the stored depth), the comparison
  works, and the taps never land on a texel where a caster is closer than the receiver.
- Not the render target path (ROV is the same), not float24 depth options, not the depth transfer options, and
  not D3D12 only (Vulkan shows no shadows either).

## Changes in our SDK copy (`sdk/src-bf`, its own local git; the game still uses the pinned SDK)

- Testing aids: the frame log also lists each draw's texture fetches, depth state, viewport, and for draws that
  read a depth texture the cascade matrices; `REX_DEV_DEPTH_FETCH=one|zero`, `REX_DEV_DEPTH_EXP=<n>`,
  `REX_DEV_LOG_DEPTH_LOADS=1`, `REX_DEV_STACK_LAYER=<n>`.
- `readback_resolve=full` read the readback buffer before the GPU had written it (all zeros); it now submits and
  waits for the copy. Needed for the dumps, not for the game.
- A 2D fetch of a stacked texture now takes the layer from the third source component, floor(z times depth). The
  shader code matches the console's intent (see above), but in the carrier view it changed nothing visible: most
  crane shadows there fall in cascades 1 and 2, so this view cannot confirm it.

## Next

1. A test scene with known shadows close to the player (on the island, next to a building), to confirm or drop
   the layer fix. TekRantGaming's screenshot of what looks wrong, and where, would help most.
2. If shadows are still missing there: dump the receivers' cascade coordinates (a dev pass that writes them to the
   colour target) and compare with the map at those texels.
3. The Trials patches (0011 for 16 bit fixed point targets, 0009) do not apply cleanly to our SDK and do not
   touch this path; BF does use `k_16_16_16_16` for its half resolution particles, so 0011 is still worth porting
   separately.

Tools: `tools/run_game.ps1 -GpuPlugin` runs a test with our SDK copy's GPU plugin; `BF_DUMP_PHYS` (dev builds,
`bf/src/dev_mem_dump.cpp`) dumps guest memory; `tools/detile_depth.py` turns a dumped cascade into an image and
reads depths at map coordinates; `tools/frame_groups.py` groups a frame log into passes; `tools/tfetch_swizzle.py`
decodes the fetch swizzles of a dumped shader; `tools/rt_formats.py` counts render target formats in the pipeline
storage; `tools/pm4_scan.py` finds where the code builds GPU packets.
