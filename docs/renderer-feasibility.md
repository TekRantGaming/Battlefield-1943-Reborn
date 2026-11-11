# Native renderer for Battlefield 1943: feasibility study

Status: study only, 4 November 2026. Nothing is built. TekRantGaming decides before any work starts (phase 4 of the plan).

The question: can Battlefield 1943 get a native renderer the way King Kong did, hooking the game's Direct3D calls
and drawing them ourselves on Vulkan and D3D12, instead of emulating the Xenos GPU?

Short answer: yes, it looks workable, with one new piece of work that King Kong did not need (shader constants
written straight into the GPU command buffer). Everything else measured matches King Kong's shape.

## What King Kong's renderer rests on

From `F:\KK-native-renderer\wt\stable-2.0.1\native-renderer\docs\design.md`:

1. The game draws only through the XDK Direct3D library (about 45 entry points used).
2. GPU draw packets are built only inside that library; no engine code builds packets.
3. The engine does not record and replay command buffers.
4. State setting goes through library calls, so hooking the calls gives the whole state.

## What Battlefield 1943 does (measured)

Tools: scans of the decrypted image (`input/image.bin`) and the generated code, plus one frame logged in game
with the SDK's `REX_DEV_FRAME_LOG` (run sh6, Wake Island, 60 FPS).

### 1. One Direct3D library, like King Kong

- Every call to the kernel's video functions (`Vd*`) comes from 16 functions between `0x82160850` and
  `0x821705C8`. The XDK Direct3D library sits at about `0x82160000` to `0x82181000`.
- The engine calls 137 of its functions directly, from 384 call sites in 174 engine functions. Most of those
  are Frostbite's render layer, a compact block at `0x82580000` to `0x825BFFFF` (about 80 functions).
- Draws are clean library calls. Frostbite's draw function `sub_82598298` takes the device from `this+672` and
  calls DrawIndexedVertices `sub_8217FC58`, DrawVertices `sub_8217F868` or a third draw `sub_8217F820`.

### 2. Packets are built only in the library

A scan for PM4 type 3 packet headers (`lis 0xC0nn` then the opcode) finds every site inside the library:
DRAW_INDX in 4 functions, DRAW_INDX_2 in 7, SET_CONSTANT in 3, EVENT_WRITE and WAIT_REG_MEM in the library's
own sync code. No engine function builds a packet.

### 3. No replayed command buffers found

The one INDIRECT_BUFFER site (`sub_82165058`) is reached only through the library's own flush and kick paths
(its callers are library functions, three levels up). The same as King Kong's segment kick.

### 4. The difference: shader constants

Frostbite uploads shader constants through `sub_82180220(device, 1, 252, 4)`, which looks like the 2009 XDK's
"begin shader constants" call: it hands back a pointer and the engine writes the values straight into the GPU
command buffer (vector stores), then ends the block. King Kong's 2005 XDK has no such path; its constants went
through setter calls into the device struct.

A native renderer can still catch them: hook the begin and end pair, give the engine a scratch buffer in guest
memory, and read the values at end. This is new work, and the first thing to prove.

Render states and textures: Frostbite's render layer makes 459 indirect calls. On the 360 XDK, render states are
set through a table of small library setter functions, so these should still be hookable functions, but the
table has to be mapped. To be confirmed with a call count at run time.

### Shaders: closer to XenosRecomp than King Kong

BF's shaders sit in per level databases (`Levels/<map>/Shaders/Single.res`, `Dx9ShaderDatabase`). Each one is a
standard XDK 2.0 container: magic `0x102A1100`, with an inline D3DX constant table (`FFFF0300` for ps_3_0) and
named constants (`outdoorLightShadowmapTexture`, `sampler_DepthBuffer`, `cameraPos`...). That is the container
XenosRecomp was written for. King Kong's 2005 containers (`0x102A0E00`) needed an adapter; BF should not. Our
shader list (`bf/data/shaders/bf1943-shaders.list`) already locates every shader in the player's own files, so
the shader pack can be built on the player's PC, as King Kong does.

### The frame (Wake Island, one frame)

1,401 draws and 22 resolves (King Kong's V-Rex: about 900 draws and 25 resolves). Native resolution 1280x720.

| Pass | Targets | Notes |
|---|---|---|
| Sun shadows | three 1024x1024 cascades, D24S8 | 7 to 12 caster draws each; depth resolved to a 32 bit colour texture (format 6) |
| Exposure | 256x32, 16_16_16_16_FLOAT | resolved to a float texture |
| Depth prepass and scene | 1280x720, float depth D24FS8, colour 2_10_10_10_FLOAT (7e3 HDR) | about 500 draws |
| Scene depth copy | resolve of the 1280x720 depth | for soft particles and water |
| Half resolution effects | 640x360, 16_16_16_16 (fixed point) | particles that read the depth buffer |
| Bloom and luminance | 320x180 down to 1x1, float | chain of resolves |
| Final | 1280x720, 8_8_8_8 | then the Flash HUD, then present |

Every pass draws in groups followed by 24 small draws (16x16 scissor, no depth, no pixel shader). Their purpose
is not known yet (occlusion queries or memory export). They must be understood before a native renderer can skip
or replace them.

## What can be copied from King Kong almost as it is

From `native-renderer` (released at `b9ebbe93` in `F:\KK-native-renderer\kk-recomp`):

- `backend/` on NVRHI: Vulkan and D3D12 devices, resources, pipelines, barriers, GPU loss handling.
- Upscalers: DLSS, FSR 3, FSR 4, XeSS; SMAA; ambient occlusion; HDR output.
- The shader translator on XenosRecomp and DXC, the pipeline cache, prewarm and pipeline lists.
- `plugin/`: the rexgpu-native plugin interface (`native_graphics_system`, settings, shader pack service,
  launcher prewarm, upscaler checks).
- The frame log and testing tools.

## What has to be new for Battlefield

- Front end hooks: the BF library's entry points (137 used), including the constant begin and end pair and the
  render state setter table. King Kong's `hooks/` is game specific (about 850 lines) and is rewritten.
- Pass classification: the passes above, so AO, upscaling and HDR know which draw is which.
- Resource identity: textures and targets by guest address and fetch constant, Frostbite's reuse of EDRAM.
- Depth as texture: the shadow cascades are read from depth resolved into a colour texture.
- HDR formats: the scene's 7e3 (2_10_10_10_FLOAT) target and the fixed point 16_16_16_16 half resolution target.
- Motion vectors: Frostbite's object transforms, for DLSS and FSR 3.
- The 24 small draws per pass.
- The Flash HUD (EA's APT player) draws through the same library and needs no special path.

## Risks

1. Constants in the command buffer: if Frostbite also writes other state straight into the command buffer
   (not only constants), the hook surface grows. First thing to check with a run time trace.
2. Unknown small draws (24 per pass).
3. Frostbite may keep GPU resources the CPU reads back (occlusion results, the luminance 1x1).
4. Multiplayer: the renderer only runs on clients; the dedicated server stays on its current path.

## Rough size

King Kong's renderer took about three weeks with several sessions working at once. For Battlefield, with the
backend, upscalers and effects copied:

- Phase 4.1, picture identical to Xenos: about two to three weeks (new front end, constant path, passes).
- Phase 4.2 and 4.3, resolution, upscalers, AA, AO, HDR: about one to two weeks more, most of it reused.

## If hooking turns out not to work

Fall back to improving the Xenos path, as Trials did: SDK patches for the formats BF uses (the 16 bit fixed
point target and the float depth), resolution scaling through the SDK's `draw_resolution_scale`, FSR and NIS
through the presenter, and our own post effects on the final image.

## First steps if TekRantGaming agrees

1. A run time trace of every library entry point the engine calls in one frame, with counts.
2. Prove the constant path: hook the begin and end pair and log the constants of one draw.
3. Identify the 24 small draws.
4. Then the plugin skeleton from King Kong's `plugin/` and `backend/`, and the first triangle.
