# Native renderer (Vulkan only)

TekRantGaming's decision (DECISIONS.md, 5 Nov 2026): a native renderer on Vulkan only, one path for Windows and Linux, no
D3D12, no DXIL, no DirectX of any kind. Built from King Kong's native renderer (tag v2.1.0,
`F:\KK-native-renderer\wt\v2.1.0\native-renderer`, read only) in stages M0 to M5, each with a gate
(PROMPT-VULKAN-RENDERER.md). The SDK's Xenos renderer stays the default; the native one is a setting
(`--gpu_plugin=bfnative`), off until every gate passes and TekRantGaming says otherwise.

## Layout

| Part | Where | What |
|---|---|---|
| Plugin | `bf/native-renderer/` | `rexgpu-bfnative.dll` (`librexgpu-bfnative.so` on Linux), built inside our SDK copy next to `rexgpu-xenos` (`sdk\src-bf\build_bf.bat` passes `BF_NATIVE_RENDERER_DIR`) |
| Hooks | `bf/src/native_hooks.cpp`, `bf/src/native_hook_list.inc` | one hook per Direct3D library entry point the engine calls, connected to the plugin through `BfNativeApi` (`plugin/native_api.h`) when `gpu_plugin` is `bfnative` |
| Entry point list | `tools/d3d_entry_points.py` -> `bf/native-renderer/d3d_entry_points.txt` | functions in the XDK library range 0x82160000 to 0x82181000 called from outside it: 149 entry points, 605 call sites, 206 calling functions |

The plugin compiles the Xenos GPU sources without D3D12 and DXBC (`REXGPU_XENOS_VULKAN_ONLY_SOURCES` from
`sdk\src-bf\src\graphics\CMakeLists.txt`) and refuses any backend but Vulkan. Until the native passes replace it,
the Xenos Vulkan renderer inside the plugin draws the picture (M0), so the game runs while the hooks grow.

## SDK patches from King Kong

Checked against our SDK copy (`sdk\src-bf`, its own git):

| Patch | What | Here |
|---|---|---|
| 0001 | timeline semaphores and synchronization2 on the provider's device | already in |
| 0002 | descriptor indexing features | already in |
| 0003 | libraries ask for instance and device extensions | already in |
| 0012 | `REX_DEV_FRAME_LOG` on Vulkan | already in |
| 0035 | present semaphores per swapchain image | already in |
| 0038 | presenter remembers the swapchain pipeline's format (stops a CPU wait every paint) | already in, adapted |
| 0050 | Linux: RenderDoc only when already attached | applied (SDK commit 10e62186) |

## Validation

The Khronos validation layer is copied (read only source: King Kong's unpacked Vulkan SDK) to `deps\vulkan-layers`
(not in git). Test runs enable it with `VK_ADD_LAYER_PATH=F:\BF-1942-MP-Decomp\deps\vulkan-layers` and
`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`; its messages go to the run's stdout.

## Stages

| Stage | Gate | State |
|---|---|---|
| M0 | the plugin loads on Vulkan and presents the Xenos picture, validation clean | **passed** (run m0a, 5 Nov: Wake Island in match with the HUD, Khronos layer loaded, 0 errors, 10 warnings all `Undefined-Value-ShaderOutputNotConsumed`, a shader output the next stage does not read, from the Xenos translator; present mode IMMEDIATE by default, then MAILBOX) |
| M1 | the hooks count every library call for one Wake Island frame, matching the frame log; the constants begin and end pair (`sub_82180220`) captured | counted (runs m1b, fc2unl): every draw accounted for, see below; the end of the constants pair not yet named |

### M1 so far (runs m1a, m1b)

- 158 hooks (149 entry points, the 11 draw packet builders, the segment kick and its callers) connect to the
  plugin; a Wake Island frame in match makes about 14,000 to 16,500 engine calls into 95 entry points and 444
  nested ones (`BF_NATIVE_COUNT`).
- Constants: `sub_82180220(device, kind, first, count)` is called about 1,540 times a frame, with (1, 252, 4),
  (0, 8, 12), (0, 0, 8), (1, 0, 4) and so on, and returns a pointer into the command buffer (for example
  `BF9A88B0`) that the engine fills. `sub_821802A8` (350 a frame) and `sub_82180200` (22 to 23 a frame) are the
  next candidates for its end and for the resolve.
- Draws: the frame log counts 1,440 draws. The hooked builders account for 862 of them (`8217FC58` 624 to 635
  engine calls, `8217F388` 222 nested from `8217F820`, `8217E0D0` 15 nested). Another 576 are single vertex draws
  (index count 1, no pixel shader, depth off, 16x16 scissor, the vertex shader built into the executable at
  `8210A728`), 24 at the end of each group of draws, 24 groups a frame: a vertex shader writing to memory, the
  library's own bookkeeping. No draw builder found by the packet scan makes them, and the segment kick runs only 8
  times a frame. Their source: every one of them sits in the same indirect buffer (guest physical `1FC9BD20`,
  REX_DEV_FRAME_LOG's `ib`), a command buffer recorded once and replayed 24 times a frame. 862 + 576 = 1,438 of
  the frame log's 1,440 (the two logs are a few frames apart; the count moves by a few draws a frame).
- The hooks connect only with `BF_NATIVE_COUNT`, until the native passes draw.

### Speed of the M0 plugin

Uncapped in a Wake Island match (runs perf_native, pf_vkxenos, perf_xenos): rexgpu-bfnative 45 to 49 FPS, the
Xenos plugin on Vulkan the same, the Xenos plugin on D3D12 134 to 152. The M0 plugin is the Xenos Vulkan backend,
so its speed is that backend's; present mode IMMEDIATE, no render target fallback logged.
| M2 | shadow, depth and scene passes drawn natively; golden frame comparison | |
| M3 | the whole frame native including the HUD, on three maps, within King Kong's testing.md thresholds | |
| M4 | 30 minute soak, no hitch over 50 ms | |
| M5 | uncapped on the native renderer | |
