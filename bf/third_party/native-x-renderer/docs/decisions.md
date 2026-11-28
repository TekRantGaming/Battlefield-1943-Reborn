# NativeXRenderer — decision log

One entry per decision: date, what was decided, why, and what would make us revisit it. Add entries at the
end, and do not rewrite old ones. If a decision changes, add a new entry that supersedes the old one.

The project was called **XenosNative** until D58. Entries D1–D57 keep that name: read `XenosNative` as
NativeXRenderer, and `xenos_native` (folders, cvars, namespace) as `native_x_renderer`.

---

## D1 — Replace the D3D layer, not the GPU (2026-10-13)

**Decision:** XenosNative hooks the game's XDK Direct3D library and draws on the host. It does not change
the SDK's Xenos emulation.

**Why:** the remaining GPU cost of POTCAWE on the Xenos path is largely emulation:
- EDRAM ownership transfers;
- resolves through the EDRAM buffer and guest memory;
- texture reloads of resolve results;
- the scene drawn twice because of predicated tiling.

SDK patches 0017–0019 showed how far optimizing the emulator goes: 0019 (lazy resolves) measured no gain.
A renderer that sees D3D calls has none of these costs.

**Revisit if:** Phase 1 shows that the engine bypasses the D3D library for a large part of its rendering.

## D2 — DiligentCore with its Vulkan backend (2026-10-13, Gustavo)

**Decision:** the host API layer is DiligentCore (Apache 2.0), Vulkan backend.

**Why:**
- Hell's Gate proved the combination with the same SDK version (v0.10.0) and the SDK's SPIR-V translator.
- Less of our own code for devices, swap chains, pipelines and resource binding than raw Vulkan.
- A D3D12 backend remains possible later.

**Cost accepted:** a large dependency, and its build integration (known issues, see the plan, Phase 2
step 1).

**Revisit if:** the integration costs more than writing the needed Vulkan code directly on the SDK's
Vulkan device.

## D3 — One repository, the library in its own folder (2026-10-13, Gustavo)

**Decision:** `POTCAWERecompVulkan` is a clone of POTCAWERecomp. XenosNative lives in `xenos_native/`, with
its own CMake target and documents, and with no game-specific code. Once a second game uses it, it is
extracted with `git subtree split --prefix=xenos_native` and used as a submodule.

**Why:** developing a library with one user in two repositories doubles every change. Keeping the folder
clean keeps the extraction a mechanical step.

## D4 — Reuse the SDK's translator and texture code (2026-10-13)

**Decision:** shaders are translated with the SDK's `SpirvShaderTranslator`, and textures are loaded with
the SDK's texture loaders. XenosNative does not write its own.

**Why:** they are proven on these games, through the SDK's Vulkan backend and through Hell's Gate.

**Revisit if:** the translator's `loop { switch }` control flow corrupts shaders on Vulkan (see the plan's
risks). The fix would then be an option in the translator, not a new translator.

## D5 — Keep the Xenos path as fallback and reference; sync-only command processor (2026-10-13)

**Decision:** the original D3D bodies keep running, and the SDK command processor keeps executing the ring
in a sync-only mode. A cvar selects the renderer at startup.

**Why:**
- Synchronization stays correct without reimplementing it: fences, swap and vblank callbacks, waits,
  queries.
- Every comparison has a reference.
- Users keep a working game while parity is incomplete.

**Revisit if:** frame pacing above 60 FPS needs the ring removed (as Hell's Gate found).

## D6 — Starting point: POTCAWERecomp `rt-to-texture` at `c6be8a3` (2026-10-13)

**Decision:** the clone starts from `rt-to-texture`, not `main`.

**Why:** SDK patch 0020 (occlusion queries, the sun flare) exists only there, and it is applied on top of
patch 0019. Patch 0019 (lazy resolves) is an option that is off and harmless.

## D7 — Hell's Gate as a design reference only (2026-10-13)

**Decision:** no code is copied from Hell's Gate. Its documents inform the design, and it is credited.

**Why:** its repository has no license (checked 2026-10-13 through the GitHub API: `license: null`), which
means all rights are reserved. Its documents are kept for personal reference in `local_docs/reference/`,
which is gitignored.

**Revisit if:** the author adds a license or gives permission. Then record which code was adopted, under
which terms.

## D8 — Name: XenosNative (2026-10-13, Gustavo)

**Decision:** folder `xenos_native/`, namespace `xenos_native`, CMake target `xenos_native`, cvars prefixed
`xenos_native`.

## D9 — The game files are copied (2026-10-13, Gustavo)

**Decision:** `assets/` holds a full copy of the game files (6.4 GB), so the project does not depend on
POTCAWERecomp's folder.

## D10 — XenosNative's SDK patches travel with the library (2026-10-13, proposed)

**Decision (to confirm with the first such patch):**
- SDK changes that only XenosNative needs go into `xenos_native/sdk_patches/` and are applied after the
  shared series. Examples: the sync-only command processor mode, the draw callback for the observer, and
  exports of translator and texture code if that route is chosen.
- `cmake/apply_sdk_patches.cmake` applies both folders.
- The shared `patches/` series stays identical to GRAWRecomp's and KameoRecomp's.

**Why:** when `xenos_native/` is extracted, its SDK requirements go with it. Games that do not use
XenosNative do not carry its patches.

**Note:** the observer and the trace mode are part of the library, not debug instrumentation. Every new
game goes through them, so project rule 2 (no debug instrumentation in the repository) does not apply to
them. They are off by default and cost nothing when off.

## D11 — D10 confirmed: the draw observer is XenosNative's first SDK patch (2026-10-13)

**Decision:**
- D10 holds. `xenos_native/sdk_patches/0001` adds `IGraphicsSystem::SetDrawObserver`: an optional
  callback the command processor calls for every draw packet it executes.
- `cmake/apply_sdk_patches.cmake` applies `xenos_native/sdk_patches/` after `patches/`, in one series: one
  stamp, and the same "start over" message if anything changed.
- The patch bumps `kGpuPluginAbiVersion` to 2, because the plugin interface gained a virtual function.

**Why:**
- The observer needs the command processor's register file at each draw, and the command processor lives
  in the GPU plugin DLL.
- A plain C callback with a plain-data structure crosses the DLL boundary safely.
- With no observer set, the patch costs one relaxed atomic load per type-3 packet.
- The shared series stays identical to GRAWRecomp's and KameoRecomp's.

**Revisit if:** the SDK gains its own draw hook upstream; then drop the patch.

## D12 — A draw's state: the device shadow, plus the packets the draw call itself wrote (2026-10-13)

**Decision:** XenosNative rebuilds a draw's GPU state from the device structure (register shadow,
constants, fetch constants, current objects) **and** from the packets the D3D call wrote. It reads those
packets right after the call returns, to get what the library writes only into the command buffer:
- the program control registers;
- the shader microcode actually loaded (`IM_LOAD`);
- the shaders' literal constants (`LOAD_ALU_CONSTANT`), kept in a small persistent model;
- `VGT_INDX_OFFSET`;
- fetch constants written outside the flush.

The rules are in [`xdk-d3d-reference.md`](xdk-d3d-reference.md#rebuilding-a-draws-gpu-state).

**Why:**
- The shadow alone matched 92 % of POTCAWE's draws in the observer. The shadow plus the call's own packets
  match 100 % in every verified scene.
- The alternative for the program registers and the literals was to reimplement the XDK's program flush
  from the shader objects. That is more code, more version-specific, and needs its own verification.
- Hell's Gate reached the same conclusion for the vertex shader variant: it reads the last `IM_LOAD`.
- The packet reader is small and follows only register writes, constant and shader loads, and indirect
  buffers. It is not a second command processor: no draws, predication, events or EDRAM.

**Revisit if:** a game writes the same state through a path the reader does not follow (for example an
engine constant buffer outside the call's range). The observer would show it as a mismatch.

## D13 — Bring-up features share one hook per entry point (2026-10-13)

**Decision:**
- The game declares each entry point once (`XENOS_NATIVE_HOOK(address, Call)`, in its own executable).
- The library dispatches every hooked call to its features (trace, observer, later the renderer) with
  whether it is nested in another hooked call, and ends the frame at Present.
- The list of calls is one table (`include/xenos_native/calls.inc`): name, argument names for the trace,
  flags.
- Features are off by default:
  - cvars `xenos_native_trace` and `xenos_native_verify`;
  - keys F9 and F10, through the SDK's rebindable keybinds.

  With all features off a hook is one branch around the original.

**Why:**
- A new game writes one line per function and gets the trace and the observer for free.
- The nesting flag tells the engine's calls from the library's internal ones (Clear → ClearF,
  DrawVerticesUP → BeginVertices, EndTiling → SetPredication).
- The renderer of Phase 2 will plug into the same dispatch.

## D14 — Go for Phase 2 (2026-10-13, Gustavo)

**Decision:** Phase 1 met its exit criterion, so XenosNative goes on to the core renderer (Phase 2).

**Why:** every draw of POTCAWE is visible at the D3D layer, and its GPU state is rebuilt exactly (100 % in
every verified scene). The costs D1 targets are all there and reachable:
- the aliased clears are library calls;
- the tiled scene is issued once by the CPU;
- the resolves are explicit calls with a destination texture.

**Also decided:** the analysis tools of Phase 1 move into the library (`xenos_native/tools/`), generic and
tracked, so the next game uses them as they are.

## D15 — The SDK's graphics code compiled into XenosNative, the cvars left to the plugin (2026-10-13)

**Decision (Phase 2, step 2):**
- XenosNative compiles the part of the SDK's graphics code it needs into its own static target
  `xenos_native_sdk`:
  - the SPIR-V shader translator and `SpirvShader`;
  - the register file;
  - the ucode, texture and draw utilities.
- The GPU plugin keeps its own copy, as it is.
- The cvars stay the plugin's: three cvars the code reads get accessors that forward to the plugin's
  registered value (`src/sdk_cvars.cpp`), and `util/draw.cpp`, which defines two cvars, is compiled
  through a wrapper (`src/sdk_draw.cpp`) that turns those definitions into forwarders.
- `util/draw_extent_estimator.cpp` (it defines `execute_unclipped_draw_vs_on_cpu`) and `flags.cpp` are
  not compiled.

**Why:**
- The plugin is a DLL with hidden symbols; exporting C++ classes from it would need a large SDK patch.
  Hell's Gate also compiles the SDK sources into its executable.
- The cvar registry ignores a second registration of a name. The executable's static initializers run
  before the plugin is loaded, so a second copy of the definitions would win, and the settings
  (`potcawe.toml`, the command line, `src/startup.cpp`'s defaults such as
  `execute_unclipped_draw_vs_on_cpu = true`) would stop reaching the plugin, which still runs the Xenos
  path. Forwarders keep one registration, the plugin's.

**Revisit if:** the SDK exports a shader translation interface from the plugin, or moves the cvar
definitions out of the sources XenosNative needs.

## D16 — XenosNative's utility shaders: GLSL, compiled at startup with the SDK's glslang (2026-10-13)

**Decision:**
- XenosNative's own shaders are written in GLSL: the copies of resolves, presenting, later clears of
  rectangles.
- They are compiled to SPIR-V when the renderer starts, with the glslang library the SDK already builds
  (`src/glsl.cpp`, with glslang's default resource limits).
- DiligentCore receives SPIR-V only, as for the game's shaders.

**Why:**
- DiligentCore is built without its compilers (`DILIGENT_NO_GLSLANG`, D15). Turning them on would bring
  a second glslang and SPIRV-Tools next to the SDK's.
- Writing the utility shaders with the SPIR-V builder, as the SDK does for its own, is much more code
  for a handful of small shaders.
- Precompiled SPIR-V would need a build step and a glslang executable the SDK does not build.

**Cost:** a few milliseconds at startup.

## D17 — Guest memory and primitives: the SDK's SharedMemory and PrimitiveProcessor on DiligentCore (2026-10-13)

**Decision:**
- Vertex and index data reach the GPU the way the SDK's backends do it: one 512 MB storage buffer mirrors
  guest physical memory, the translated vertex shaders fetch vertices from it, and indexed draws use it
  as their index buffer.
- XenosNative does not reimplement this. It subclasses the SDK's `SharedMemory` (`src/guest_memory.cpp`)
  and `PrimitiveProcessor` (`src/primitives.cpp`), which are backend-independent, and gives them
  DiligentCore buffers. Both base classes are compiled into `xenos_native_sdk` (D15).
- Each draw requests the ranges its vertex fetch constants and its index buffer cover. Pages already
  valid cost nothing. A page is uploaded again after the CPU writes it, which the base class learns
  through the memory's write watches.

**Why:**
- The page watches cover every CPU write, with or without Unlock. The planned alternative, hooking the
  Unlock cache flush, misses the Bink planes (written without Unlock) and the dynamic data written into
  the command buffer.
- The primitive processor already knows every conversion the host needs: triangle fans, quad lists and
  line loops to lists and strips, points and rectangles expanded in the vertex shader, primitive reset
  values. Index endianness is swapped by the vertex shader, so guest index buffers are used in place.
- The SDK's own `SharedMemory` stays idle on this path (host rendering is off, D5). The memory's
  invalidation callbacks support several listeners, so the two coexist.

**Cost:** about 170 small copies per frame at the dock (1.5 MB, mostly vertex data the game writes every
frame). Each copy ends the render pass in progress. To revisit in Phase 3.

**Rejected:** the Unlock hook as the only invalidation source (incomplete, above). It stays a possible
optimization: it names exact ranges where page watches need an access violation.

## D18 — Resources bound by name through DiligentCore's SPIR-V reflection (2026-10-13)

**Decision:**
- The translator's SPIR-V is given to DiligentCore unchanged except for one instruction (below).
  DiligentCore reflects it and remaps its descriptor sets to its own layout.
- XenosNative binds by the names DiligentCore reports:
  - storage and uniform buffers by their block type: `XeSharedMemory`, `XeSystemConstants`,
    `XeFloatConstants`, `XeBoolLoopConstants`, `XeFetchConstants`;
  - textures and samplers by their variable names, `xe_texture{N}_{2d|3d|cube}_{u|s}` and
    `xe_sampler{N}_...`, in the order of the shader's bindings.
- The buffers are static variables, one per stage; textures and samplers are dynamic variables, set per
  draw.
- The translator's `Features` describe the DiligentCore device: SPIR-V 1.5, one shared memory binding,
  full 32-bit indices, no clip or cull distances, swizzles applied in the shader.
- `OpSource Unknown 0` is added to every module.

**Why:**
- DiligentCore names a buffer by its block type unless the module says it is HLSL.
- `XeFloatConstants` exists in both stages with different contents, so it must not become one resource.
- DiligentCore refuses a module without `OpSource`, and glslang's builder writes none for an unknown
  source language.
- DiligentCore does not enable shader clip and cull distances. Image views with a swizzle would need one
  view per swizzle.

**Rejected:** declaring an explicit pipeline resource signature with the translator's set and binding
numbers. It is more code, and DiligentCore would remap the bindings anyway.

## D19 — Viewport: the translated shaders produce Direct3D-style Y (2026-10-13)

**Decision:** draws use `draw_util::GetHostViewportInfo` with `origin_bottom_left = true`, as the SDK's
D3D12 backend does, and pass the viewport to DiligentCore unchanged.

**Why:** DiligentCore's Vulkan backend flips Y with a negative viewport height, to behave like Direct3D.
The SDK's Vulkan backend expects an unflipped viewport. With the flag, the two flips cancel, and the pixels
(and so the winding rule) are those of the SDK's Vulkan path.

## D20 — D12 completed: the draw packet's registers, and microcode copied when loaded (2026-10-13)

**Decision:** two additions to the rebuild of D12, found when draws first ran:
- `VGT_DRAW_INITIATOR`, `VGT_DMA_BASE` and `VGT_DMA_SIZE` come from the draw packet itself, which the
  command processor decodes into those registers. The tracker reads every draw packet, including
  bin-predicated ones: the XDK predicates all draw packets, tiled or not.
- The tracker copies the vertex and pixel microcode when the call that loads it (`IM_LOAD`) returns, and
  keeps the copy.

**Why:**
- The observer compared the register shadow's groups only. These three registers are outside them, so
  Phase 1's 100 % did not cover them. Without them, every draw used the initiator of the last internal
  rectangle (a clear or a resolve).
- The GPU copies the microcode when it executes the `IM_LOAD`; later draws run the copy. While a level
  loads, the game frees and reuses that memory: reading it at draw time gave microcode the SDK's
  disassembler threw on, and the game crashed while the dock loaded. With the copy, no shader has failed
  analysis since.

## D21 — Packets read when each command buffer segment ends; D20's explanation corrected (2026-10-13)

**Decision:**
- The tracker reads a call's packets as soon as each command buffer segment ends (the nested
  `KickSegment`), and the rest when the call returns. Before, it read everything when the call returned.
- Shader microcode (`IM_LOAD`, `IM_LOAD_IMMEDIATE`) and `LOAD_ALU_CONSTANT` data are copied at the moment
  the packet is read.
- The shader cache protects the SDK's analysis against access violations as well as exceptions (some
  invalid microcode makes it dereference null), and logs where a rejected program came from.

**Why:** D20 said the vertex shader memory was reused while a level loads. The diagnostics showed
something more specific:
- the failing programs are vertex shaders loaded inline (`IM_LOAD_IMMEDIATE`, 105–192 dwords) by
  `DrawIndexedVertices`;
- our copy matches the shader the SDK dumps (`--dump_shaders`) for its first 16–22 dwords, then holds a
  coherency packet sequence (`COHER_STATUS_HOST`, `COHER_SIZE_HOST`, `COHER_BASE_HOST`, `WAIT_REG_MEM`, as
  `sub_822B0010` writes it) where the microcode should be.

Reading each finished segment early removed most of these, but not all: one load in three still shows a
few (3 in `potcawe_046`). Something writes that sequence into the command buffer over the inline
microcode. The Xenos path, which reads the microcode later (when the command processor executes the
packet), has never failed on these shaders, so the final contents are probably right, but that is not
verified packet by packet. The writer is not identified yet (open point in `local_docs/05-phase2.md`). Meanwhile the draws that get such a program are skipped and logged,
not crashed on.

## D22 — DiligentCore patched for robust buffer access; frames in flight bounded (2026-10-14)

**Decision:**
- XenosNative's changes to DiligentCore are a `git format-patch` series in `xenos_native/diligent_patches/`,
  applied to the submodule at configure time (`xenos_native/cmake/apply_diligent_patches.cmake`), as the
  SDK's patches are (D10). It is made on the submodule's branch `xenos-native`, in a worktree.
- Patch 0001 enables Vulkan's `robustBufferAccess` when the device supports it.
- `Device::Present` waits for the GPU to finish the frame two frames back (a DiligentCore fence).
- The renderer logs an error when DiligentCore's command queue fence falls more than 64 submissions behind
  while frames complete.

**Why:**
- Without the feature the device was lost (`VK_ERROR_DEVICE_LOST`) on the first frames of the Port Royal
  dock; with it, it is not. So some buffer access goes out of range. Which one is not identified: the
  translated shaders read guest memory (vertices, indices) at addresses taken from the game's fetch
  constants, and the bound views and buffers have fixed sizes. The SDK's Vulkan backend enables
  `robustBufferAccess` too; DiligentCore does not, and has no option for it.
- DiligentCore ignores Vulkan errors in release builds. After the loss, its waits returned at once and the
  game kept running at 60 FPS, but its command queue fence stopped advancing (it reads fence status, which
  reported the loss), so its dynamic heap was only reclaimed when it filled up and DiligentCore forced a
  GPU idle (every 1.4 s), the duration queries never completed, and the GPU sat at 8.6 %. That looked like
  a GPU that could not keep up; it was a lost device. The check above makes the next one visible.
- Without vertical sync DiligentCore's swap chain uses mailbox presentation, which never blocks: nothing
  bounded how far the CPU could run ahead of a slow GPU.

**Evidence:** `local_docs/05-phase2.md`, runs 20–26: `vkGetFenceStatus` returned -4 from the first dock
frames on; with the feature enabled, no loss, no heap exhaustion, the queue fence 3 submissions behind, GPU
utilization 69 % for 10.7 ms of GPU time per frame.

**Rejected:** creating the Vulkan device in XenosNative and attaching DiligentCore to it
(`AttachToVulkanDevice`): more code to keep in step with DiligentCore's own device setup, for one feature.

## D23 — Guest textures: the SDK's TextureCache on DiligentCore, unchanged data not loaded again (2026-10-14)

**Decision:**
- `GuestTextures` (`src/guest_textures.cpp`) is a subclass of the SDK's `TextureCache`, as `GuestMemory` is
  of `SharedMemory` (D17). The base class keeps doing what does not depend on the backend: texture keys from
  the fetch constants, guest layouts (tiling, packed mips, pitch), the memory a load needs, invalidation
  through the guest memory mirror's page watches, the LRU.
- The host side is ported from the SDK's Vulkan backend:
  - the host formats are the D3D12 backend's table (DiligentCore's formats are DXGI's), changed where
    DiligentCore's Vulkan mapping does not match the DXGI layout: 2_10_10_10 swaps red and blue in the
    host swizzle; 5_6_5 and 4_4_4_4 are expanded to RGBA8; DXT1 is decompressed to RGBA8, because
    DiligentCore's `BC1_UNORM` is Vulkan's BC1 without alpha; 1_5_5_5 and DXT3A_AS_1_1_1_1 are not
    supported yet;
  - the loads are the SDK's precompiled SPIR-V load shaders (untiling, endian swap, format conversion),
    run as DiligentCore compute pipelines. They read the guest memory mirror and write a scratch buffer,
    which `UpdateTexture` copies into the texture, level by level and slice by slice.
- A texture binding samples, in this order: the host copy of a resolve to its address (D11), the guest
  texture, or a placeholder.
- Each draw requests its textures before its vertex data. The tracker does not report fetch constant
  writes one by one, so the 32 bindings are checked again at each draw (cheap when nothing changed).
- When a texture is outdated, its guest data is hashed first (XXH3, base and mips separately). A part that
  hashes as at its last load is not loaded again.

**Why:**
- The SDK's texture code is what the Xenos path runs, so formats, layouts and invalidation behave the same.
  Only the host objects and the dispatches are XenosNative's.
- The hash: `SharedMemory` invalidates the whole 256 KB block around a CPU write, which costs fewer access
  violations. POTCAWE's engine writes reference counts in pages of those blocks every frame, so 31
  textures (16 MB of host data) were reloaded at the dock every frame with their data unchanged. Hashing
  their 5 MB of guest data per frame on the CPU replaces all of those loads. Textures whose data does
  change (the Bink planes) are still loaded.

**Evidence:** `local_docs/05-phase2.md`, runs 27–35:
- every reload at the dock had unchanged data (run 34);
- the writer is game code, not the D3D library (run 32);
- loads stopped at the dock once the hash was in place (run 35).

**Rejected:**
- A CPU path with `texture_util`: slower, and the load shaders already exist.
- Narrower invalidation in `SharedMemory`: the memory callbacks are shared with the Xenos path. It stays
  an option.
- Hooking the engine's writes: game-specific, and the hash covers every game.

## D24 — Float24 depth keeps the guest's values on the host (2026-10-14)

**Decision:** the draw path calls `draw_util::GetHostViewportInfo` with `full_float24_in_0_to_1` false, so
24-bit float depth (`kD24FS8`) keeps its guest value on the host.

**Why:**
- The SDK's Vulkan backend halves float24 depth, mapping its [0, 2) range to [0, 1). This lets EDRAM data
  move through depth attachments, and the backend doubles the values back when it resolves.
- XenosNative has no EDRAM, and its resolves copy host depth as it is. A halved shadow map was therefore
  compared with the full-range depth that the shader computes, and POTCAWE's character shadows disappeared
  (run 42).
- Guest depth above 1, which float24 can hold, is clipped or clamped on the host. It has not been seen.

**Evidence:** `local_docs/05-phase2.md`, run 42: the shadows match the Xenos path.

## D25 — The launcher offers XenosNative before parity; game settings it lacks are turned off (2026-10-14, Gustavo)

**Decision:**
- POTCAWE Launcher gets its "Renderer" row now, in Phase 2: "Xenos (emulated)" or "XenosNative
  (experimental)", saved as `xenos_native` in `potcawe_options.toml`. It is offered only when Vulkan is
  installed.
- With XenosNative, the render quality, antialiasing and graphics API rows show what XenosNative uses
  (1280×720, no MSAA, Vulkan) and are disabled; the choices for Xenos are kept. `draw_resolution_scale` is
  saved as 1, since the SDK's backend only keeps the GPU in sync then.
- The library gets `xenos_native::Renders()`. POTCAWE's MSAA hooks (`src/msaa.cpp`) apply when it is true,
  whatever `potcawe_no_msaa` says, so the command line gets the same result as the launcher.

**Why:**
- Gustavo wants to play with XenosNative to find what still differs from the Xenos path, which was planned
  for Phase 3. Playing through the launcher is the normal way to start the game.
- With 2× MSAA the engine renders the scene in two EDRAM tiles of a 1280×416 surface. XenosNative draws
  each pass once at the surface's size and does not join the tiles yet, so only the top 416 rows showed
  (run 46). Joining them is Phase 3.
- `Renders()` is a general query, not a POTCAWE feature: any game can turn off what XenosNative does not
  support yet.

**Evidence:** `local_docs/05-phase2.md`, runs 46–48.

## D26 — Every segment change read, NewSegment hooked; D21's cause corrected (2026-10-14)

**Decision:**
- A new hooked call, `NewSegment`: the library function that moves the write pointer to a newly
  allocated command buffer segment (POTCAWE `sub_822AE950`). The tracker treats it like `KickSegment`: it
  reads the packets written so far, then continues from the new segment.
- Whichever segment change comes first reads up to the write pointer; the next one reads only what was
  written since, so nothing is read twice when `KickSegment` calls `NewSegment`.
- The observer compares the programs too: the hash of the microcode the tracker copied against the
  command processor's (`ucode_data_hash`, the same XXH3 over the guest's bytes).

**Why:** `KickSegment` is not the only way to a new segment. The library's reservation (POTCAWE
`sub_822AEE50`) calls `KickSegment`, and if what it reserves still does not fit, calls `NewSegment` again
directly. Before this decision, such a call's packets continued in a segment the tracker did not know
about, and one of two things happened:
- The new segment was below the old one in memory: the call was "not fully read" and its packets were lost
  (one or two calls per frame). When those were a draw's program flush, that draw and the following ones
  kept the previous programs (up to 24 draws in a row). A vertex shader that does not match its vertex
  data draws stretched triangles. That is what Gustavo saw at the dock, for a few seconds at a time.
- The new segment was above it: the tracker read the memory in between as packets. That memory holds old
  packets from earlier frames, including stale `IM_LOAD_IMMEDIATE` headers whose microcode had since been
  partly overwritten by newer packets, such as `sub_822B0010`'s coherency sequence. That explains D21's
  "overwritten" inline vertex shaders: no writer overwrote them; the tracker read memory that was no longer
  part of the command buffer.

**Evidence:** `local_docs/05-phase2.md`, runs 49–52. At the dock, 30 observer verifications of 2 frames:
- before: 2–6 calls "not fully read" and up to 33 draws with the wrong vertex program per verification,
  each run of wrong draws starting at a call that was not fully read;
- after: every draw matches (about 68,000), 0 calls not fully read, 0 program mismatches.

With the renderer, 2.5 minutes at the dock after the load: no shader analysis or translation failures (they
appeared in one load in three, and during play), and the shader and pipeline counts stay flat.

## D27 — With XenosNative, the SDK runs its Vulkan backend (2026-10-14)

**Decision:** when `xenos_native` is on, POTCAWE's startup sets `gpu_backend` to `vulkan` before the SDK
loads its GPU plugin (`src/options.cpp`), whatever the launcher or `potcawe.toml` say. The launcher
already shows "Vulkan" in its API row with XenosNative (D25).

**Why:**
- With XenosNative the SDK's backend only keeps the GPU in sync (sync-only command processor), which both
  of its backends support. With D3D12 the process held an idle D3D12 device beside DiligentCore's Vulkan
  device.
- Overlays such as RTSS can attach to that D3D12 device and report "D3D12". Gustavo read that as
  XenosNative not being on, although his logs show it drawing every frame (`potcawe_085`–`087`).
- With one graphics API in the process the overlay reports what presents, and nothing is created that is
  not used.

**Evidence:** `local_docs/05-phase2.md`, run 54: the SDK's Vulkan backend in sync-only mode, XenosNative
drawing, the overlay reports Vulkan, 60 FPS, the same renderer CPU time as with D3D12 underneath.

## D28 — Pipelines compiled asynchronously; draws wait by being skipped (2026-10-14)

**Decision:**
- DiligentCore compiles XenosNative's pipelines in its thread pool (the `AsyncShaderCompilation` feature,
  4 threads, `PSO_CREATE_FLAG_ASYNCHRONOUS`).
- A draw whose pipeline is still compiling is skipped. The pipeline's resources (static variables, the
  resource binding) are set up on the first draw after it is ready.
- Shader analysis and translation (the SDK's) stay on the render thread.
- The renderer's log reports what was created on the render thread (count, time, the longest one) and how
  many draws were skipped while their pipeline compiled.

**Why:**
- A pipeline the driver has not compiled before took up to 205 ms on the render thread (run 56): a
  245 ms frame when the camera turned at the dock. Pipelines the driver has cached take under 1.5 ms.
- Skipping a draw for a frame or two is what the Xenos path does with asynchronous pipelines, which
  POTCAWE already uses (`gpu_async_pipelines_no_frame_wait`, `src/startup.cpp`). A new object appears a
  frame late instead of the whole image stopping.
- Translation costs at most 2.7 ms per shader and happens about a hundred times while a level loads, so
  moving it off the thread can wait.

**Evidence:** `local_docs/05-phase2.md`, runs 56–58. Walking and turning at the dock, the longest frame
went from 245 ms to 18.5 ms, and pipeline creation on the render thread from 300 ms to 0.3 ms per
600 frames. In 12 minutes at the dock, 409 draws were skipped during the load and a few during play, and no
frame took over 20 ms after the load.

## D29 — Phase 2 closed; go for Phase 3, performance first (2026-10-14, Gustavo)

**Decision:** Phase 2 is closed on its exit check (`plan.md`), and Phase 3 starts. Its order, agreed with
Gustavo: GPU time first, then the occlusion queries (the sun glare), then resolution, MSAA and the rest of
the game.

**Why:**
- The exit criteria hold: the three scenes match the Xenos path, the fallback is unchanged, and 12 minutes
  at the dock passed with no crash and no frame over 20 ms after the load.
- XenosNative exists to be faster than the emulation, and at the dock it is not yet: 12 ms of GPU time
  facing the town against about 5 ms for the Xenos path at the same settings. That has to be understood
  before more features are built on the current draw path.

## D30 — Render targets bound only when they change (2026-10-14)

**Decision:** the draw path remembers the render targets it bound last and calls `SetRenderTargets` only
when a draw's targets differ. The renderer tells it (`DrawPath::TargetsChanged`) when a clear, a resolve or
Present binds other targets.

**Why:** DiligentCore's Vulkan backend adds a barrier after every transition out of a writable state, even
to the same state (`TransitionTextureState`: "Always add barrier after writes"). Binding the same render
targets again therefore placed a barrier, and a barrier ends the render pass (dynamic rendering). Every one
of the dock's ~1,100 draws ran in its own render pass and stored and reloaded the RGBA16F scene target and
its depth buffer: about 10 µs per draw on an RTX 4070 Ti, whatever the draw. That was the "80 % of the GPU
facing the town" Gustavo saw.

**Evidence:** `local_docs/06-phase3-performance.md`. At the dock, 1280×720, AA Off:
- GPU time 12.1 → 3.3 ms per frame facing the town, 2.8 → 1.8 ms facing the sea;
- renderer CPU time 5.35 → 4.3 ms;
- render passes per frame 1,145 → 173, barriers 1,325 → 320.

The image is unchanged.

**Rejected:** a DiligentCore patch that skips same-state barriers for render targets. Not binding what is
already bound is enough, and keeps the patch series at one entry.

## D31 — Phase 3 holds the renderer's own work; POTCAWE's polish moves to Phase 4, the second game to Phase 5 (2026-10-14, Gustavo)

**Decision:**
- **Phase 3** is the work that changes how XenosNative renders, in this order:
  1. the occlusion queries (the sun glare);
  2. DXT1 textures kept compressed on the host (BC1);
  3. shader translation off the render thread;
  4. the guest memory uploads, step A (one batch at the start of the frame); step B (the draws of a pass
     recorded after all of their uploads) only if step A's measurements call for it;
  5. the render resolution chosen from the window, at any scale, not only whole multiples of 1280×720.
- **The game's MSAA is not brought to XenosNative.** The antialiasing XenosNative offers will be its own,
  a modern post-process AA (FXAA or SMAA) or host MSAA, applied to the scene before the HUD is composited.
  Until then the Antialiasing option stays forced off with XenosNative (D25).
- **Phase 4** is POTCAWE's polish: that antialiasing, every level compared with the Xenos path, the
  known limits from Phase 2 as scenes need them, a persistent pipeline cache, long sessions, ultrawide
  output. It ends with the decision to make XenosNative POTCAWE's default.
- **Phase 5** is the old Phase 4: a second game (GRAW) and the library's extraction.

**Why:**
- The game's 2× MSAA exists to fit EDRAM: it is what splits the scene into two tiles, and POTCAWERecomp
  needs hooks to make it work (`src/msaa.cpp`). XenosNative draws the scene once into a plain texture, so
  its antialiasing can be any host technique. Gustavo prefers a modern one of XenosNative's own.
- Whole multiples are a limit of the Xenos path's EDRAM emulation. XenosNative's render targets and
  resolve results are ordinary textures, so the scale can follow the window (a 1920×1080 window wants
  1.5×). The SDK's translator bakes the scale into shaders as an integer in three places, which a patch
  will have to turn into a constant.
- Resolution comes last because it touches almost every part of the renderer: the earlier items are
  validated at 1× first, and the occlusion queries already scale their counts (D32).
- The old Phase 3 mixed renderer work with walking the whole game; the old Phase 4 was the second game.
  With polish as its own phase, Phase 3 has a measurable end and Phase 5 starts from a finished POTCAWE.

Gustavo agreed to the list, the order and the split after a viability analysis (2026-10-14: "Me parece
bien ese orden y la división en Fase 4 y 5").

## D32 — Occlusion queries measured on the host and answered when the command processor ends them (2026-10-14)

**Decision:**
- **The tracker reads Query Issue's packets.** Issue's first argument is the query, not the device, but
  the library writes its packets into the device's command buffer. A new call flag, `kPackets`, makes the
  state tracker read such a call with the device of the last device call. The tracker also records the
  register writes of bin-predicated `SET_CONSTANT` packets (`PacketScan::predicated_writes`), because the
  library writes the query's `RB_SAMPLE_COUNT_ADDR` (0x2325) in them, one per tile. They are not applied
  to the register file.
- **Host queries bracket the guest's.** `Issue(BEGIN)` (flags & 2) begins a DiligentCore occlusion query.
  `Issue(END)` (flags & 1) ends it and keys it by the end event's sample count address: the last 0x2325
  write of the call.
- **Outside any render pass.** A Vulkan query begun outside a render pass must end outside it, and the
  draws in a guest query can span several render passes. DiligentCore's API has no call that only ends
  the render pass, so a state change on a 16-byte buffer forces a barrier, and a barrier ends it (D30).
- **No waiting.** Once per frame, after the submit, the results that arrived are collected in order
  without waiting. The latest count per sample count address is kept.
- **The command processor writes the answer.** A new SDK patch, `xenos_native/sdk_patches/0003`, adds
  `IGraphicsSystem::SetOcclusionQueryResolver`:
  - at an `EVENT_WRITE_ZPD` whose block holds the end marker (`0xFFFFFEED`), the command processor asks
    the resolver;
  - the answer becomes `Total_A` and `ZPass_A`, with every other count zeroed;
  - `ZPass_A` and `ZPass_B` are written last, in one 8-byte store after a release fence, so the D3D
    library never sees half a result: it takes a query as finished as soon as either word loses the
    marker.
  - Without a resolver, or if it declines, the fake count stays as before. The Xenos path never sets one.
- **The answer is the latest count that arrived** for that address, one or two frames old. Before the
  first result it is 0 samples (not visible).
- **Counts are reported at the guest's resolution:** divided by the render scale's area
  (`OcclusionQueries::SetSampleScale`). It is 1 until the resolution work (D31).

**Why:**
- POTCAWE's sun flare draws a quad at the sun inside a query (`sub_8244F4A8`), reads the previous frame's
  result (`sub_824521F8`), and sets the glare's strength from the visible fraction. In sync-only mode the
  command processor reported its fake 1000 samples, more than the quad can produce, so the glare was at
  full strength and drawn over the geometry in front of the sun. Gustavo saw this at the dock after D26.
- **The SDK has to write the answer.** The command processor executes the end event later than the
  game's Issue call, on its own thread, and overwrites the block with the fake count. If XenosNative
  wrote guest memory itself, the two writers would race. With the resolver, the command processor is the
  only writer, as on the Xenos path.
- **Waiting for the exact result would stall the game.** At the end event the host GPU has not run the
  query yet: XenosNative records on the game's thread and submits at the end of the frame, with up to two
  frames in flight. Waiting would hold the command processor, and with it the game, for up to two frames
  per query. The Xenos path's own host queries do a full GPU wait per query end (patch 0020); that is
  what XenosNative avoids. A result one or two frames late cannot be seen in a flare's strength, and
  POTCAWE reads last frame's result anyway.
- **Keyed by address.** The game reuses its query objects every frame, and the end event's address is
  what the command processor sees. At the dock the same two blocks come back every frame.
- **Several tiles.** XenosNative draws a tiled pass once, so the whole count belongs in one block: the
  last tile's, which is the one that holds the end marker. The other tiles' blocks get 0 from the
  command processor's unchanged path, and GetData adds the blocks. This is not exercised yet: with
  XenosNative the scene has one tile (Antialiasing forced off, D25).

**Evidence:** `local_docs/06-phase3-performance.md`, runs 101–108. At the dock, 1280×720, AA Off:
- every frame: 2 queries begun, ended, measured and answered; 1,200 per 600 frames, none unanswered once
  the first results arrived;
- samples: 0 with the sun hidden or off screen, up to 104 per query facing it;
- three views of the sun, compared with the Xenos path at the same spot: the glare matches, and the
  white-out over the geometry is gone;
- 10 observer verifications of 2 frames on the Xenos path: every draw matches, 0 calls not fully read;
  reading Issue's packets did not disturb the tracker.

**Rejected:**
- Writing the counts into guest memory from XenosNative: two writers, as above.
- Exact, same-frame results by waiting on the host GPU: the stall above.
- Ending the render pass through a DiligentCore patch: the barrier needs no patch, and keeps the patch
  series at one entry.

## D33 — DXT1 kept compressed as BC1 with 1-bit alpha; textures counted at their host size (2026-10-14)

**Decision:**
- **A second DiligentCore patch.** `xenos_native/diligent_patches/0002` maps `TEX_FORMAT_BC1_*` to
  `VK_FORMAT_BC1_RGBA_*`, and those back to `TEX_FORMAT_BC1_*`. DiligentCore's D3D11 and D3D12 backends
  get the same decoding from DXGI's BC1.
- **DXT1 is loaded as BC1.** `k_DXT1` and `k_DXT1_AS_16_16_16_16` load into `BC1_UNORM` through the
  SDK's 64bpb load shader, which only untiles and byte-swaps the 8-byte blocks. This is what both of the
  SDK's backends do.
- **Fallbacks.** A texture whose size is not a multiple of 4 is decompressed to RGBA8, as before and as
  for the other block formats. Every block format is decompressed when the device has no BC formats (the
  `TextureCompressionBC` feature is optional), as the SDK's Vulkan backend does.
- **Host sizes.** Host textures report their real size to the SDK's texture cache: every mip and slice,
  from DiligentCore's `GetMipLevelProperties`. Before, they reported the base level at the guest's bits
  per pixel. The log shows how many guest textures are on the host and their size.

**Why:**
- **Memory.** DXT1 is 4 bits per texel. Decompressed to RGBA8 it takes 8 times the memory, and its load
  writes 8 times the data.
- **Alpha.** Vulkan's BC1_RGB formats decode the fourth color of a three-color block (first color not
  greater than the second) as opaque black. DXT1, like Direct3D's BC1, decodes it as transparent:
  alpha-tested DXT1 textures would get black borders with BC1_RGB. That is why D23 decompressed DXT1.
- **The memory limits.** The SDK's texture cache destroys textures unused for 30 s above its soft limit
  (384 MB), and any unused texture above its hard limit (768 MB). Counting decompressed textures at
  4 bits per texel and without their mips hid most of their real size from those limits.

**Evidence:** `local_docs/06-phase3-performance.md`, runs 109–113. At the dock, 1280×720, AA Off:

| | DXT1 decompressed (run 109) | DXT1 as BC1 (runs 110, 111, 113) |
|---|---|---|
| Guest textures on the host | 412–460, **276–318 MB** | 412–458, **128–139 MB** |
| Video memory in use (RTSS) | 3,968 MB | 3,763 MB |
| Data written by texture loads, from the start to the dock | 1,821 MB | 1,673 MB |
| GPU time facing the town | 3.14–3.30 ms (135–145 copies) | 3.14–3.17 ms (118–135 copies), 3.5–3.6 ms (159–170 copies) |

- The GPU time follows the number of guest memory copies of the run (the render pass breaks, D30), not
  the texture format.
- The data written by loads hardly changes: most of it is the menus' video planes, which are not DXT1.
- The image is the same: at the start view, 57.6 % of the pixels are identical before and after, and
  41.9 % differ by less than 16 levels. Two runs of the same build give 83.1 % and 16.6 %. The extra
  small differences are the hardware's BC1 interpolation against the SDK's decompression shader. The
  alpha-tested foliage has no black borders.

**Rejected:** keeping DXT1 decompressed until a DiligentCore release decodes BC1 with alpha. The patch
changes a few lines and decodes BC1 the way DiligentCore's D3D backends already do.

## D34 — Shaders translated on worker threads; draws wait by being skipped (2026-10-14)

**Decision:**
- **Translation leaves the render thread.** Two worker threads translate guest shaders to SPIR-V and
  create their DiligentCore shaders. Each worker has its own `SpirvShaderTranslator`, which keeps
  per-translation state. On a machine with fewer than four hardware threads there is one worker.
- **Analysis stays on the render thread.** The SDK's analysis runs when a draw first uses a microcode. It
  costs about 0.02 ms, and the draw needs its results right away to choose the shader modifications.
- **The handoff.** The render thread creates the `Translation` object (`GetOrCreateTranslation`) and
  queues it. The worker translates it and stores the shader, then publishes the state (pending, ready or
  failed) with a release store. The render thread reads that state with an acquire load.
- **Draws wait by being skipped.** Until both of its shaders are ready, a draw is skipped, as when its
  pipeline is still compiling (D28). The log counts the two cases apart. Analyses and pipelines are timed
  on the render thread, translations on the workers.
- **Shutdown.** The destructor sets an atomic flag, wakes the workers and joins them, without taking the
  queue's lock. Workers also wake every 50 ms on their own, so a missed notification cannot hang the exit.

**Why:**
- **The cost at a level load.** D28 measured translation at up to 2.7 ms per shader, about a hundred
  times per level load. At the dock that is 93 translations and 75 ms of the render thread, in the frames
  where the game is also loading.
- **Same treatment as pipelines.** Skipping draws for a few frames is already how new pipelines are
  handled (D28) and how the Xenos path handles asynchronous pipelines. An object appearing a frame or two
  late during a load cannot be seen.
- **Thread safety.** The translator only reads the analysis of the translation's shader, which no longer
  changes, and writes into the `Translation` it was given. The render thread may add translations to the
  same shader meanwhile, but the worker never touches that map. DiligentCore creates device objects from
  any thread.
- **Two workers.** The load's 93 translations take about 90 ms of CPU, so two workers finish within a
  few frames. More would compete with the game's threads and DiligentCore's four compilation threads,
  which work at the same moment.
- **Precedent in the SDK.** It gives each thread its own translator when it loads its shader storage
  (`d3d12/pipeline_cache.cpp`), and both of its backends compile pipelines on creation threads.

**Evidence:** `local_docs/06-phase3-performance.md`, runs 109–116. At the dock, 1280×720:

| | Translation on the render thread (runs 109, 110, 113) | On worker threads (runs 114, 116) |
|---|---|---|
| Translations at the dock's load | 93, 75.3 ms on the render thread (max 2.6 ms) | 93, 84.7–87.6 ms on the workers (max 3.1 ms), none on the render thread |
| Longest frame in the 600 frames with the load | 79.6–83.2 ms | 51.9–53.9 ms |
| Draws skipped in those frames | 382 while their pipeline compiled | 1,368–1,496 while a shader was translated, 595–631 while their pipeline compiled |
| Longest frame during play at the dock | 18.4–19.0 ms | 18.4–20.0 ms |

- The image at the start view, 25 s after the load, differs from earlier runs no more than two runs of
  one build differ from each other.
- A normal window close exits in 0.5 s with the workers running (run 115).
- No translation failures.

**Rejected:**
- **Analysis on the workers too.** It would add a second wait for no gain: it is 1.7 ms for the whole
  load.
- **One translator behind a lock.** That serializes the translations, and the render thread would block
  on the lock whenever it needs the translator's default modifications.

## D35 — Guest memory pages are copied only when their data changed; this replaces upload step A (2026-10-14)

**Decision:**
- **A hash per page.** The guest memory mirror keeps a 64-bit hash (XXH3) of the data last copied into
  each 4 KB page (1 MB for the 512 MB mirror).
- **Unchanged pages are not copied.** When the SDK's `SharedMemory` uploads invalid pages, a page whose
  current data has the same hash is marked valid without a copy. The others are copied in contiguous
  runs.
- **Copies come from a snapshot.** The pages are copied into a CPU buffer first, hashed there, and
  uploaded from it. The CPU may write a page at any moment, so the recorded hash must describe exactly
  the bytes the mirror received.
- **The log**, every 600 frames: buffer copies per frame (average and maximum), the guest memory copied,
  and the guest memory requested again but unchanged.
- **This replaces step A** as D31 planned it (one batch of uploads at the start of the frame). Step B
  (the draws of a pass recorded after all of their uploads) is not needed for now.

**Why: the measurement that changed the plan.** A probe, since removed, logged every upload of frames
4000–4005 at the dock, with the draw and what requested it, and hashed every uploaded page (runs
117–118):
- **The uploads.** About 133–178 per frame, about 3 MB: 105 for index data, 33–47 for vertex data,
  25–26 for texture data.
- **The same ranges every frame.** 131 of about 150 ranges were uploaded again in the next frame, at the
  same draws.
- **Few real changes.** Only 6–7 uploads per frame (13–16 pages) held data that had changed. They are
  vertex data the CPU writes just before draws 41, 607 and 1062–1122, and one index range. Over 600
  frames at the dock, 2.5 % of the uploaded pages had changed.
- **The cause.** The pages are invalidated without being changed: `SharedMemory` invalidates more than
  the CPU writes. POTCAWE writes reference counts next to its data every frame (`sub_8242D218`), which
  D23 had already found for textures.
- **Step A would only have moved the copies.** It would have moved about 130 copies of unchanged data to
  the start of the frame, still 3 MB per frame. It could not have moved the few that change, because the
  CPU writes them during the frame, right before their draws.
- **Skipping them removes the copies**, and it needs no knowledge of when the game writes.

**Correctness:**
- **The mirror holds the last snapshot.** Nothing on the GPU writes the mirror (no memory export;
  resolves write host textures), so each page holds exactly the last snapshot copied into it.
- **Writes during the check.** Pages are made valid (write-protected) before they are hashed, as they
  were before being copied. A write during or after the hash invalidates the page again, and its next
  use checks it again.
- **First use.** A hash of 0 means unknown, so a page is always copied the first time.

**Evidence:** `local_docs/06-phase3-performance.md`, runs 117–121. At the dock, 1280×720, AA Off:

| | Before (runs 109–116) | After (runs 119, 120) |
|---|---|---|
| Buffer copies per frame, start view | 118–170 | 7–9 (9.4–12.2 on average over 600 frames) |
| Guest memory copied per frame | 2,664–3,192 KB | 60–80 KB, and about 2,500 KB found unchanged |
| GPU time facing the town | 3.14–3.60 ms | 1.55–1.66 ms |
| Renderer CPU time per frame | 4.34–4.48 ms | 3.99–4.11 ms |
| Longest frame during play | 18.4–20.0 ms | 18.3–19.0 ms |

- **The image is unchanged.** The start view is within run-to-run variation.
- **A three-minute walk** around the dock (run 120): NPCs, subtitles, HUD and shadows are correct, no
  errors, 1.0–1.7 ms of GPU time.
- **The main menu's video** still plays (run 121).
- **Over the session**, XenosNative's GPU time at the dock went from 12.1 ms (Phase 2's close) to 3.3 ms
  (D30) to 1.65 ms. RTSS shows 14–19 % of the GPU, against about 30 % for the Xenos path at the same
  settings.

**Rejected:**
- **Step A as planned**, for the reasons above.
- **Finer invalidation in `SharedMemory`.** That is an SDK change to a design the Xenos path shares;
  hashing solves it in XenosNative without a patch.
- **Hashing the guest memory and copying it directly**, without a snapshot. It saves one memcpy, but a
  CPU write between the hash and the copy could leave the mirror and its recorded hash disagreeing.

## D36 — XenosNative applies the guest's display gamma ramp when presenting (2026-10-14, Gustavo)

**Decision:**
- **SDK patch 0004.** `xenos_native/sdk_patches/0004` adds `IGraphicsSystem::GetGammaRamp`. It copies
  the ramps the command processor tracks from the DC_LUT register writes: the 256-entry table and the
  piecewise linear (PWL) ramp, with a version that changes on every write. A mutex taken only where those
  writes change an entry makes the copy safe from the render thread.
- **A gamma pass before the scaling.** Present maps the front buffer through the ramp, texel by texel and
  at the front buffer's size, into an RGBA16F texture. That texture is then scaled into the window as
  before. This is the order of the SDK's presenters.
- **The SDK's arithmetic and choice.** The pass computes what the SDK's `apply_gamma` shaders compute:
  - the table maps each 8-bit channel to 10 bits;
  - the PWL takes the 10-bit value, picks an entry with its 7 high bits and interpolates with the low 3.
  As the SDK does, the table is used for 8_8_8_8 front buffers and the PWL for 2_10_10_10 ones.
- **Updates.** The ramp texture (R32_UINT, 256×4: the table, then the PWL's red, green and blue) is
  rewritten only when the version and the content changed. The log shows both ramps at a few levels
  whenever they change, and which ramp the front buffer's format selects.
- Done in Phase 3 rather than Phase 4, where the plan had put it, at Gustavo's request: he found the image
  flat compared with the Xenos path.

**Why:**
- **The image was too light.** Without the ramp the image was lighter in the darks than on the Xenos
  path. A level of 48 came out as 32 there, and 128 as 116 (the D33 checks). Gustavo also saw less
  contrast while playing.
- **POTCAWE's ramp.** It presents a `2_10_10_10_AS_16_16_16_16` front buffer (format 54), and writes a PWL
  ramp that darkens the low levels. Red at 32, 64, 128 and 192 of 255 gives 67, 196, 467 and 748 of 1023;
  a straight line gives 129, 259, 518 and 776. The 256-entry table stays at its default (linear).
- **The GPU applies it after the frame.** The display controller applies the ramp to the front buffer
  after the frame is finished. So it belongs to presenting, not to the draws, and only the command
  processor sees the register writes.
- **Thread safety.** The command processor writes the ramp on its own thread, and the SDK's presenters
  read it there. XenosNative presents on the game's render thread, hence the copy under a lock.

**Evidence:** `local_docs/06-phase3-performance.md`, runs 123–125. The start view at the dock, compared
with the Xenos path:
- **Tone curve, XenosNative to the Xenos path** (green, average per level):
  - before: 24 → 10, 48 → 32, 128 → 116, 192 → 186;
  - after: 24 → 23.8, 48 → 47.1, 128 → 127.1, 192 → 191.8, the diagonal.
- **Pixels differing by 16 levels or more:** 30–31 % before, 1.9–2.4 % after.
- **Pixels within 6 levels:** 2.8 % before, 87 % after. The remaining small differences are the two
  renderers'; the same comparison between two runs of one build gives 97–98 %.
- **The Xenos path is unchanged** with the patch (run 125): no errors, 87.7 % of the pixels identical to
  its earlier screenshot.

**Rejected:**
- **Applying the ramp in the scaled copy**, after the bilinear filter. The ramp would act on filtered
  values, and the result would differ from the SDK's whenever the window is not 1:1.
- **Hooking the D3D library's gamma functions.** That needs addresses per game. The register writes reach
  the command processor in every game.

## D37 — The render resolution follows the screen, at any scale, with no selector (2026-10-14, Gustavo)

**Decision:**
- **The scale comes from the output.** XenosNative renders at the largest scale at which the game's 16:9
  image fits the output: the monitor in fullscreen, the window's client area otherwise. The scale is
  `min(output width / guest width, output height / guest height)`, and the guest size comes from the
  game's profile (POTCAWE: 1280×720). It need not be a whole number:

  | Output | Scale | Rendered |
  |---|---|---|
  | 1366×768 | 1.067 | 1365×768 |
  | 1920×1080, 1920×1200, 2560×1080 | 1.5 | 1920×1080 |
  | 2560×1440, 2560×1600, 3440×1440 | 2 | 2560×1440 |
  | 3840×2160 | 3 | 3840×2160 |

- **No scale selector with XenosNative.** The launcher shows the resulting resolution instead of the
  scale row, which stays for the Xenos path. A setting outside the launcher (`potcawe.toml`) can force a
  scale, for a weak GPU behind a large monitor.
- **Bounds.** The scale is at least 1: in a window smaller than the game's resolution the game still
  renders at 1280×720, and Present reduces the image. The scale is at most what the device's largest
  texture allows.
- **Chosen at startup.** A window resized during play scales the presented image; the render resolution
  changes at the next start.
- **Everything the game renders is scaled, shadow maps included,** as the Xenos path does at 2×2 and 3×3.
  Textures loaded from guest memory keep their size, except those that resolves produce.
- **Separate scales per axis** throughout the renderer, equal for now, so that a later change of aspect
  ratio only has to give them different values.
- **Other aspect ratios move to Phase 4.** Filling a screen that is not 16:9 (no bars on 16:10, ultrawide
  or the Steam Deck) needs the game's camera projection, its HUD and menus, and any effect that assumes
  16:9 to change. That is game-side work, done together for every aspect ratio. Until then the image is
  16:9 with bars.
- **The translator's scale.** SDK patch `xenos_native/sdk_patches/0005` lets the SPIR-V translator revert
  a fractional scale (`SetFractionalResolutionScale`). The translated shaders revert the scale in two
  places, both of which XenosNative needs:
  - the pixel position of parameter generation;
  - the texel offsets and weights of resolution-scaled textures.
  The whole-number scale keeps its uses in EDRAM emulation and memory export, which XenosNative does not
  use. The SDK's own backends do not call it, so the Xenos path is unchanged.

**Why:**
- **Gustavo's requests (2026-10-14).**
  - No fixed scales: "que todo se calcule en base a resoluciones". A 1366×768 or 1920×1200 monitor
    should get exactly its resolution, not the next multiple.
  - Shadows scaled like the rest, as on the Xenos path.
  - No bars, but together with the ultrawide work.
- **Integer scales waste GPU time.** They are a limit of the Xenos path's EDRAM emulation. For 1600×900
  or 1920×1200, its automatic scale renders 2560×1440: 2.6 and 1.8 times the pixels needed. For 1366×768
  it stays at 1× and stretches the image.
- **XenosNative can do any scale.** It has no EDRAM: its render targets and resolve results are plain
  textures of any size. Vulkan viewports take fractional positions and sizes, so the SDK's viewport
  helper can run at scale 1 and its result be multiplied.
- **A minimum of 1** keeps the game's smallest targets (its luminance and bloom chain) at least their
  guest size, for almost no saving below it.

**Evidence:** to come with the implementation (`local_docs/06-phase3-performance.md`). The first check is
at exactly 2× against the Xenos path at 2×2, then fractional scales (1.067, 1.25, 1.5).

## D38 — How the renderer applies the render scale (2026-10-14)

**Decision:**
- **One rounding rule.** `RenderScale` (`src/scale.h`) holds a float per axis, fixed at startup. A guest
  pixel coordinate becomes a host one as `floor(guest × scale + 0.5)`. The rule is the same for both edges
  of every rectangle and for sizes counted from 0.
- **Host sizes.** Render targets and resolve textures are created at that size, at least 1 pixel. They
  stay keyed by the guest's surface and texture.
- **Viewports.** The SDK's viewport helper runs at scale 1, in guest pixels. Its NDC transform is
  relative to the viewport, so XenosNative multiplies the viewport's offset and extent by the scale as
  floats. Vulkan takes fractional viewports.
- **Point sizes** keep `1 / guest extent`, which equals the SDK's `scale / host extent`.
- **Scissors** are converted edge by edge with the rule.
- **Resolves** are clipped in guest pixels, as before, and both rectangles are then converted. With a
  fractional scale, their rounded sizes can differ by one pixel when their offsets differ; the copy takes
  the smaller.
- **Textures from resolves.** The system constant `textures_resolution_scaled` has a bit set for every
  fetch constant bound to a resolve result, so that the translated shaders revert the scale in texel
  offsets and weights, as the SDK does.
- **The translators**, on the render thread and on the workers, get the scale through
  `SetFractionalResolutionScale` (`sdk_patches/0005`).
- **Occlusion queries** divide their counts by `scale x × scale y` (D32's sample scale).
- **The output and the guest size.** The output is the swap chain at startup: the monitor in
  fullscreen, the client area in a window. The guest size is the profile's `output_width/height`
  (POTCAWE: 1280×720).
- **Bounds and override.** The scale is clamped to 1 and to `MaxTexture2DDimension / max(guest width,
  height)`. The cvar `xenos_native_resolution_scale` (a double, 0 = from the output) forces a scale.
- **Present** is unchanged. The front buffer comes out at the render size, the gamma pass runs at that
  size (D36), and the copy to the window is 1:1 when the output's 16:9 rectangle is the render size.

**Why:**
- **No gaps or overlaps.** A single rounding rule for edges means two rectangles that touch in guest
  pixels touch on the host. That covers per-tile resolves of one texture, and viewports and scissors
  that tile a target.
- **Viewports.** Scaling them as floats keeps the SDK's half-pixel offset and its handling of the
  guest's viewport in guest units: one tested code path, at every scale.
- **Texel offsets in resolved textures are host texels in the SDK** (it divides them by the scale), so
  manual filtering in game shaders stays consistent with the larger textures. Doing the same keeps
  XenosNative equal to the Xenos path at 2×2.
- **Everything scales uniformly**, the smallest targets included (the 1×1 luminance targets become 2×2
  at 2×), as on the Xenos path. That is what the 2× comparison checks.

**Evidence:** `local_docs/06-phase3-performance.md`, runs 127–139:
- **Scales.** Automatic from windows of 1366×768 (1.067 → 1365×768), 1600×900 (1.25) and 1920×1080
  (1.5), and fullscreen on a 2560×1440 monitor (2.0). Forced 1, 1.4, 1.6 and 2 in other windows.
- **No errors in any run.** The render targets and resolve textures are logged at their host sizes.
- **Parity at exactly 2×.** XenosNative fullscreen at 2560×1440 against the Xenos path at 2×2 on the
  same monitor, 1:1, start view below the overlay:
  - the tone curve is the diagonal;
  - 70.0 % of the pixels are within 2 levels, 22.5 % within 3–6, and 7.5 % differ by 7 or more;
  - at 1× the same comparison gave 55.5 %, 31.3 % and 13.3 %.
- **GPU time facing the town:**

  | Scale | 1 | 1.067 | 1.25 | 1.4 | 1.5 | 1.6 | 2 |
  |---|---|---|---|---|---|---|---|
  | ms | 1.55–1.65 | 1.68–1.74 | 1.95–1.99 | 2.19–2.29 | 2.35–2.51 | 2.53–2.74 | 3.32–3.84 |

- **Occlusion queries:** at most 92–98 samples per query facing the sun at 1.067, 1.25 and 2, in the
  range of 1× (104, a different framing). The counts are at the guest's scale.
- **Not the renderer:** captures of a 1920×1080 window come out posterized on both paths, the Xenos path
  at 1×1 too. Other window sizes and fullscreen do not. It is in this machine's display or capture,
  and the tests avoid that window size.

**Rejected:**
- **Rounding sizes up and edges to nearest.** Two rules would disagree on where a rectangle that starts
  at 0 ends.
- **Keeping the smallest targets (1×1, 4×4) at 1×.** The Xenos path scales them too, and the 2×
  comparison matches.

## D39 — Phase 3 closed; Phase 4 (POTCAWE polish) next (2026-10-14, Gustavo)

**Decision:** Phase 3 is closed on its exit check (`plan.md`), and Phase 4 starts. Gustavo chooses the
order of its items.

**What Phase 3 changed**, at the dock:
- GPU time 12.1 → 1.65 ms per frame at 1×: render targets bound once (D30), guest memory copied only
  when it changed (D35);
- the sun's occlusion queries measured on the host (D32);
- DXT1 kept compressed (D33);
- shader translation off the render thread (D34);
- the display gamma ramp applied (D36);
- the render resolution following the screen at any scale (D37, D38).

**Why:**
- **Every item is done.** The last was the A/B against the Xenos path, with both paths in one session.
- **XenosNative is faster at the same resolution.** It uses 38–56 % less of the GPU without
  antialiasing, and holds 60 FPS at 3× where the Xenos path with 2× MSAA drops to 33.
- **No frame over 20 ms at the dock after the load:** 18.4–19.0 ms at most in every run.
- **What remains is polish, not renderer work:** antialiasing of its own, the rest of the game, other
  aspect ratios, a persistent pipeline cache and long sessions.

Gustavo: "Dala por cerrada!" (2026-10-14).

## D40 — Phase 4 starts with antialiasing, aspect ratios and a pipeline cache; temporal upscaling and frame generation become Phase 5 (2026-10-14, Gustavo)

**Decision:**
- **Phase 4 (POTCAWE polish) starts with three items:**
  - XenosNative's own antialiasing, FXAA and SMAA, on the scene before the HUD;
  - any aspect ratio without bars (ultrawide, 16:10, the Steam Deck);
  - a persistent pipeline cache.
  Its other items follow: every level compared with the Xenos path, long sessions, and the known limits
  from Phase 2.
- **The order of work:**
  1. antialiasing, which needs the frame's HUD composite located (the aspect ratio work needs it too);
  2. the pipeline cache, which is self-contained;
  3. aspect ratios, the largest and least certain item: it needs the game's camera, its HUD and its
     menus.
- **A new Phase 5: temporal upscaling and frame generation.** It covers:
  - the motion vectors these need (camera motion first, then moving objects);
  - DLSS / DLAA and FSR 3 as quality options (DiligentCore's SuperResolution module already has DLSS on
    Vulkan);
  - frame generation, to show more than 60 FPS without touching the game's timing.
- **The old Phase 5** (a second game and the library's extraction) **becomes Phase 6.**

**Why:**
- **Gustavo's request** (2026-10-14): "deja lo del DLSS y FrameGen para una Fase 5. De momento
  enfoquémonos en el Antialiasing propio (FXAA, SMAA), la proporción de pantalla sin bandas (ultrawide,
  16:10, Steam Deck) y la caché persistente de pipelines".
- **Temporal techniques need motion vectors,** which a 2007 game does not produce. Making them is
  research of its own, while FXAA and SMAA need only the final image.
- **Upscaling buys little speed here:** XenosNative already takes 3.3 ms at 1440p and 5.85 ms at 4K on
  an RTX 4070 Ti. Their value is image quality (DLAA) and frame rates above 60 (frame generation), both
  after the polish.

## D41 — XenosNative's antialiasing: SMAA or FXAA on the copy of the finished scene, named by the profile (2026-10-14)

**Decision:**
- **Two methods**, chosen at startup by the cvar `xenos_native_antialiasing` (`smaa` by default, `fxaa`,
  `off`), and by POTCAWE's launcher in its Antialiasing row when XenosNative is the renderer:
  - **SMAA 1x**, preset High, from its authors' reference implementation, vendored unchanged in
    `xenos_native/thirdparty/smaa` (iryoku/smaa `71c806a`, MIT). CMake embeds `SMAA.hlsl` as a string
    and it is compiled as GLSL (`SMAA_GLSL_4`) at startup. Three passes: luma edges, blending weights
    with the area and search lookup textures, neighborhood blending.
  - **FXAA**, the SDK's (its Vulkan presenter's normal quality), as one pixel shader that computes the luma
    itself.
- **Where: on the copy of the finished scene.** The game's profile names the Resolve that copies the scene
  after the post-processing and before the HUD (`Profile::scene_resolve_caller`, the call's return
  address); POTCAWE's is `0x824583EC`. Right after that copy, its texture is antialiased in place: a copy
  of it is the input, and the last pass writes the texture. Without a named resolve there is no
  antialiasing.
- **The trace now prints each call's caller** (" from 0x…", the return address), to find such a call in a
  new game.

**Why:**
- **The HUD must stay sharp.** The game's MSAA is not brought over (D31), so the antialiasing works on the
  image. Applied to the front buffer it would blur the HUD and the menus' text. POTCAWE copies the
  finished scene to `0x09B2E000`, then draws the HUD in the same EDRAM, then composites that copy and the
  HUD into the back buffer (traced frame below). The copy is the last point before the HUD.
- **A return address names the point without a game hook.** It identifies the engine's call, it lives in
  the game's profile with its other addresses (rule 1), and the library stays general: a callback would
  need a hook in the game's code for the same information.
- **SMAA is the default.** It smooths edges as well as FXAA with less blur on textures, at a cost of about
  0.15 ms at 2560×1440. FXAA stays as the cheaper choice.
- **The images are gamma-encoded,** which both methods expect.

**Evidence:** `local_docs/07-phase4.md`, runs 156–163:
- **The trace of a frame at the dock** (run 156): the copy to `0x09B2E000` from `0x824583EC` (the scene
  blended with the last frame's copy), then the HUD in its surface, then two full-screen draws: that copy,
  then the HUD texture.
- **Applied once per frame at the dock** (600 times in 600 frames). In the menus it is applied somewhat
  more often (717 in 600 frames), and the menus' text is as sharp as without antialiasing.
- **Edges:** the dock's ropes and railings lose their stair-steps with both methods. FXAA also softens
  the textures; SMAA keeps them.
- **GPU time facing the town:** at 1×, 1.56–1.65 ms off, 1.61–1.72 SMAA, 1.59–1.69 FXAA. At 2×, 3.30 ms
  off (the A/B), 3.46 SMAA, 3.39 FXAA.

**Rejected:**
- **Host MSAA,** rendering the scene's targets with samples. It costs much more GPU (the Xenos path's 2×
  MSAA added 60 % at 2×2), it does nothing for the alpha-tested foliage, and every target, resolve and
  shader with MSAA would have to be handled.
- **Antialiasing the front buffer,** which blurs the HUD.
- **A temporal method (TAA, DLAA):** it needs motion vectors, which is Phase 5 (D40).

## D42 — The shaders, translations and pipelines are kept between runs and prepared at startup (2026-10-14)

**Decision:**
- **A record of what the game used.** XenosNative appends every new guest shader (its microcode), every new
  translation (shader and modification) and every new pipeline (its shaders, their modifications, and the
  fixed-function state without the host shader pointers) to a file per title:
  `<runtime cache directory>/xenos_native/<title id>.pipelines` (`src/storage.cpp`).
  - The file is a versioned header followed by records.
  - New records are written at the end of the frame they appeared in.
  - A record cut short (the process killed while writing) ends the file, and another version starts it
    again.
- **Prepared at the next start.** At the first draw the stored shaders are analyzed (render thread, about
  1.3 ms) and the stored translations queued to the workers. Each stored pipeline is created, up to 64 per
  frame, as soon as its shaders are translated. The draws then find everything ready.
- **DiligentCore's pipeline cache** (Vulkan's `VkPipelineCache`, the driver's binaries) is kept next to it
  (`<title id>.vkcache`). It is given to every pipeline creation, and saved once no new pipeline appeared
  for 600 frames, so not in the middle of a load. Vulkan ignores data from another device or driver.
- **On by default:** the cvar `xenos_native_pipeline_storage`. The game passes the runtime's cache
  directory to `xenos_native::Initialize`.

**Why:**
- **First use still showed.** Since D28 and D34 nothing stalls the frame, but every shader and pipeline
  seen for the first time in a run made its draws wait a few frames. At the dock's load that was about
  1,350–1,600 draws skipped while translating and 570–1,700 while compiling: objects appearing late in
  every session, not only the first.
- **Not depending on the driver's cache.** The plan asked that a new run not depend on the driver's cache,
  which a driver update clears.
- **Translations are recorded on their own, not only through pipelines.** With pipelines only, the second
  and third runs still translated 13 and 6 shaders at the dock's load. Those were draws whose translation
  never led to a pipeline (no target bound, for one), and they were translated again every run.

**Evidence:** `local_docs/07-phase4.md`, runs 164–169. At the dock's load (the 600 frames that contain it):

| Run | Stored before | Translations at the load | Pipelines at the load | Draws skipped (translating + compiling) |
|---|---|---|---|---|
| 164 (pipelines only, empty) | nothing | 93 | 91 | 1,611 + 1,678 |
| 165 | 97 shaders, 112 pipelines | 13 | 4 | 638 + 128 |
| 166 | 117 pipelines | 6 | 8 | 347 + 231 |
| 167 (with translations, empty) | nothing | 93 | 104 | 1,356 + 569 |
| 168 | 97 shaders, 108 translations, 125 pipelines | 0 | 3 | 0 + 6 |
| 169 | 128 pipelines | 0 | 0 | **0 + 0** |

- **The cost of preparing them:** at startup, 47 ms after the first draw to create the 128 pipelines, of
  which 1.3 ms of analyses and 1.5–1.9 ms of creations are on the render thread. The 108 translations
  (about 88 ms) run on the workers while the menus show.
- **The files:** 77 KB of records, and 5.3 MB of pipeline cache.
- **The longest frame of the load is unchanged** (44–53 ms): that is the game's own loading, not
  XenosNative's.

**Rejected:**
- **The SDK's shader storage.** It belongs to the Xenos path's backends: it stores their pipeline
  descriptions and creates their pipelines, which XenosNative does not use.
- **Saving the pipeline cache at exit.** The game may be killed, and a save during play happens only
  after things are quiet.

## D43 — Screens of any aspect ratio are filled: wider 3D views, 2D layers in a 16:9 box (2026-10-14)

**Decision:**
- **The renderer fills the output.** When the game's profile says its hooks widen its cameras
  (`Profile::widens_cameras`) and the output is not the game's aspect ratio, the render scale is per axis
  (D37's `RenderScale` had an x and a y for this). The vertical scale is the one D37 computes from the
  output's height (or the override), the horizontal one follows from the output's aspect ratio, and the
  game's 1280×720 image becomes, for example, 3440×1440 or 1280×800. Neither scale goes below 1.
  - `xenos_native::AspectFactor()` is the result: the output's aspect ratio over the game's, above 1 for a
    wider output and below 1 for a taller one. It is 1 otherwise, and always on the Xenos path.
  - The cvar `xenos_native_fill_screen` (on by default, read at startup) turns it off: the 16:9 image with
    bars, as before.
- **The game widens its 3D views.** A game hook multiplies the tangent of the half angle of every 16:9
  perspective camera by the factor: the width on a wider output (Hor+), the height on a taller one (Vert+).
  It does so before the game builds both the projection and the culling frustum, so its culling follows.
- **2D layers go into a box.** `xenos_native::SetGameAspectBox(bool)` marks the draws that follow as a 2D
  layer laid out for the game's aspect ratio (a HUD, menus, a video). In a box, the draw path maps guest
  pixels to host pixels with the smaller of the two scales on both axes, plus an offset that centers the
  render target's guest area in its host size. That applies to viewports and scissors; clears and resolves
  are not boxed. The game's hooks set the box around its 2D views, and Present clears it.
- **Fill-rate rectangles are opened.** A 3D effect the game clips to a screen rectangle computed for 16:9
  gets the whole viewport on the wider axis (a game hook).
- **POTCAWE** (`src/aspect.cpp`): four midasm hooks.
  - The camera builder's perspective branch (`sub_823288C8`).
  - The view setup, which reads the view's matrix: a square parallel projection is a 2D view.
  - The start of a view's post-processing (box off, since the composite covers the screen).
  - The scissor of a draw's screen rectangle (opened on the wider axis outside 2D views).

**Why:**
- **Gustavo asked for no bars** on ultrawide, 16:10 and the Steam Deck (2026-10-14). His primary monitor is
  3440×1440.
- **The camera is the only reliable place for the 3D views.** The game's own culling then agrees with what
  is drawn. Changing the matrices in shader constants would leave the culling at 16:9 and pop objects at
  the edges. It also could not tell the 3D views from the 2D ones and the shadow maps.
- **The box is the renderer's job, not the camera's.** Narrowing the 2D views' parallel camera
  (`P00 / factor`) was tried first (run 177). The HUD and the menus were right, but the game's text layout
  measures in screen pixels through that camera. It kept the glyphs at their stretched width and dropped
  what no longer fit: the pause menu showed "Reiniciar desde punto de" without "control". In the box, the
  game lays everything out exactly as at 16:9.
- **A centered 16:9 box is the usual treatment** for HUDs, menus and videos on wide screens. Anchoring
  each HUD element to the screen's edges needs per-element knowledge of the game's layout. It remains
  possible later.
- **The rectangle is only an optimization in POTCAWE.** The sun's haze at Port Royal is a large mesh
  clipped to a screen rectangle the game computes for fill rate. At 21:9 the rectangle fell short of the
  mesh, and a vertical seam showed in the sky (RenderDoc pixel history, event 1433: a scissor 2340 pixels
  wide). At 16:9, removing the clipping changes nothing (runs 182, 183).

**Evidence:** `local_docs/07-phase4.md`, runs 170–188.

| Output | Render size | GPU at the dock | Draws per frame | Result |
|---|---|---|---|---|
| 1600×900 (16:9, reference) | 1600×900 | 2.02–2.05 ms | 1,128–1,143 | no change: no fill, no hook active |
| 2560×1080 (21:9) | 2560×1080 | 2.85–2.91 ms | 1,178–1,199 | 3D Hor+ (camera ×1.3333), HUD and menus in a 16:9 box, no seam |
| 1280×800 (16:10) | 1280×800 | 1.75 ms | 1,124–1,147 | 3D Vert+ (×0.9), HUD and menus in a 16:9 box |

- **The wider view draws about 5 % more objects** (the sides). Nothing pops at the edges: the game's
  culling uses the widened frustum.
- **The Xenos path is unchanged** (run 188). The hooks do nothing when `AspectFactor()` is 1.
- **The launcher's render quality row** shows the filled size, for example 3440×1440 on the ultrawide.

**Rejected:**
- **Widening the matrices in the shader constants** (in the renderer): culling stays at 16:9, and the
  views cannot be told apart.
- **Narrowing the 2D views' camera:** the game's text layout drops words (above).
- **Hooking `sub_822FC610` and `sub_82303D60`**, the camera class with the "Projection Angle
  Width/Height" properties. Neither runs in the menus or at the dock (run 171).

## D44 — XenosNative applies the SDK's anisotropic filtering override; POTCAWE defaults to 16x (2026-10-14)

**Decision:**
- **XenosNative honors `anisotropic_override`**, the SDK's cvar, as the SDK's texture caches do. It forces
  the level on textures with mipmaps, linear filtering and no base map, and keeps what the game asks
  elsewhere. The device's maximum caps the result (`textures.cpp`; the sampler cache key already holds
  the level, so a change at run time makes new samplers).
- **POTCAWE defaults to 16x on both paths.** The value is `anisotropic_override = 5` in `potcawe.toml`
  (`src/startup.cpp`), and the launcher has an "Anisotropic filtering" row (16×, 8×, 4× (original), 2×,
  off). That part is a game setting, so it was made in POTCAWERecomp first (`f3b67db`, branch
  `rt-to-texture`, not pushed) and merged here.

**Why:**
- **A parity gap.** XenosNative used only what the game asks for. POTCAWE asks for 4x on few textures (one
  sampler: a texture with 11 mip levels, only in the run that turns toward the sea). At the dock its textures (5 and 8 mip levels)
  are plain trilinear. The SDK forces 4x on those by default (`anisotropic_override = 3`), so on the Xenos
  path the dock's planks kept their grain at an angle, and on XenosNative they blurred to a gray smear.
- **16x costs nothing measurable**, on either path (below). Gustavo asked for 16x if it was cheap.

**Evidence:** `local_docs/07-phase4.md`, runs 189–198 (`tools/gpu_ab.ps1`, the dock, 60 FPS, RTX 4070 Ti).

| Configuration | XenosNative GPU per frame | GPU % | Xenos path GPU % |
|---|---|---|---|
| 2×, what the game asks | 3.46 ms | 28.7 | — |
| 2× (2×2), 4x | 3.50 ms | 28.0 | 51.9 |
| 2× (2×2), 16x | 3.46 ms | 28.1 | 50.4 |
| 3× (3×3), 4x | 6.22 ms | 44.1 | 96.5 |
| 3× (3×3), 16x | 6.22 ms | 44.7 | 96.6 |

- **The planks** (`local_docs/evidence/aniso_compare.png`, the default view enlarged): blurred with what the
  game asks, grain with 4x, slightly sharper far away with 16x. XenosNative at 4x matches the Xenos path at
  4x.

**Rejected:**
- **A XenosNative-only cvar.** It would duplicate the SDK's, and the two paths would need two settings
  for one option.
- **Keeping 4x as the default.** It matches the Xenos path as it was, but 16x is sharper at no measured
  cost. The other launcher options keep the console's behavior by default, and this one is the exception
  (POTCAWERecomp's README says why).

## D45 — The Vulkan pipeline cache is saved on a thread of its own (2026-10-14)

**Decision:** D42's save of DiligentCore's (Vulkan's) pipeline cache runs on a thread of its own:
- the driver's serialization (`GetData`) and the file write leave the render thread;
- the file is written next to the cache and renamed over it, so a process killed while writing leaves the
  last complete cache;
- a save that fails (a pipeline added between `GetData`'s two calls makes it return nothing) is tried again
  600 frames later;
- the log line gives the save's duration.

When it is saved does not change: 600 frames after the last new pipeline.

**Why:**
- **Gustavo noticed short stutters** every few minutes in a session of about 10 minutes (`potcawe_200`,
  3840×2160, 60 FPS).
- **Each one was a save.** The six saves in the session each fall in a 600-frame window whose longest
  frame is 32–83 ms, against 18.4–19 ms in the others. The three longest (78, 81 and 83 ms) are 2–3 minutes
  apart.
- **The cache had grown to 24–25 MB.** D42 measured 5.3 MB. It probably grew with the render scales tried
  since: the translator writes the scale into the shaders (D38), so each scale adds its own binaries.
- **A save follows every few new pipelines** (2–9 when an area shows something new). So in a new area a
  save came about 10 s later, in the middle of play.

**Evidence:** `local_docs/07-phase4.md`, runs 200–202.
- Run 201 started without the records, to provoke saves while playing:
  - five saves, 28–73 ms each on their own thread;
  - the windows after them at 18.4–18.9 ms.
- Run 202 is a normal start with the records:
  - the menus' window at 30.5 ms, as before;
  - the dock at 18.4–19.0 ms, its load at 52 ms.

**Open:** in run 201 (no records, so everything new at the title screen) one frame took 460 ms and the
window averaged 21 ms, against 43–149 ms in D42's runs without records. It came a second after the save had
ended, and the save thread does not touch the render thread. Not investigated yet. It only concerns the
first start, before the records exist.

**Rejected:**
- **Saving less often only.** It would make the stutter rarer, not shorter.
- **Saving at exit.** As in D42: the game may be killed.

## D46 — Phase 5 starts with DLSS before Phase 4 closes; DLSS through DiligentCore's SuperResolution module (2026-10-14)

**Decision:**
- **Phase 5 starts now, with DLSS,** at Gustavo's request. Phase 4's remaining items stay open: every level
  compared with the Xenos path, the hour-long session, and the known limits when a scene needs them.
- **DLSS comes through DiligentCore's SuperResolution module.** That is its NGX provider for Vulkan.
  - **NVIDIA's NGX SDK:** the headers, the loader library and the DLSS runtime `nvngx_dlss.dll` (310.5.3)
    come from DiligentGraphics/DLSS-Headers, which DiligentCore fetches when configuring.
  - **The runtime** is copied next to the game's executable (`xenos_native_copy_runtime`).
  - **Not built in:** DirectSR (D3D12) and FSR 1 (its shaders need the HLSL compiler, which is not built).
- **The build passes the Windows SDK version.** The module enables DLSS only with a Windows SDK of
  10.0.26100 or later, read from `WindowsSDKVersion`, which only Visual Studio's prompts set. XenosNative's
  CMake passes the newest installed SDK that way.
- **The order of work** (`docs/plan.md`, Phase 5):
  1. find DLSS and report it (this entry);
  2. jitter the 3D views;
  3. motion vectors from depth and the camera's matrices;
  4. DLAA (DLSS at the render resolution) as an antialiasing mode;
  5. the upscaling modes;
  6. then FSR and frame generation.
- **Where DLSS goes:** before the game's post-processing, at the start of the view's post-processing (the
  point POTCAWE's hook from D43 already marks). The game's bloom and its blend with the last frame then work
  on a clean image. The 3D views are known from their matrices, and only they are jittered.
- **The cvar:** `xenos_native_antialiasing = dlaa` looks for DLSS and reports it. Until DLAA draws, SMAA is
  used meanwhile.

**Why:**
- **DiligentCore already supports it.** Its module has a DLSS provider on Vulkan, the device extensions
  NGX needs (DiligentCore enables them whenever the GPU has them), and an interface for the inputs:
  color, depth, motion vectors, jitter, exposure and reset.
- **DLAA first:** it needs no second render scale. The upscaling modes need the 3D views rendered smaller
  than the HUD and the output, which XenosNative's single scale (D37) does not do yet.
- **Before post-processing:** that is where DLSS is meant to run, and the game's post-processing starts at
  one known point.

**Evidence:** `local_docs/08-phase5.md`, run 203 (1600×900, RTX 4070 Ti): "NVIDIA DLSS is available: NGX:
DLSS", native resolution supported. The input sizes for that output:
- quality 1067×600;
- balanced 928×522;
- performance 800×450;
- ultra performance 533×300.

"Max quality" (NGX's ultra quality) is not offered and falls back to quality.

**The license:** NVIDIA's RTX SDK license allows distributing the runtime in object form as part of an
application. This repository is private. The runtime is fetched, not committed, so a published repository
would not carry it.

## D47 — DLAA: the main 3D view jittered through its viewport, camera motion vectors, DLSS before the post-processing (2026-10-14)

**Decision:**
- **The game marks its views.** `xenos_native::BeginView(matrix)` starts a view, with its view-projection
  matrix for a 3D view and null otherwise. `xenos_native::EndViewGeometry()` is called where the view's
  post-processing starts. POTCAWE calls both from `src/views.cpp`: the view setup that uploads the matrix,
  and the start of the post-processing, the same points as D43's box.
- **The jitter.** Between the two calls, the draws into targets of the game's output size (the main view's;
  reflections and shadow maps are smaller) are shifted by a sub-pixel offset through their host viewport,
  in host pixels. The offset is a Halton (2, 3) sequence of 64, one per frame. The viewport also gives the
  view's depth range.
- **The motion vectors.** At `EndViewGeometry` a pixel pass computes them from the view's depth, its matrix
  this frame and the last, its depth range (reversed when the near plane is above the far one) and the
  jitter. They follow the camera's motion only.
- **DLSS at the native resolution.** Its preset (K) is chosen by NGX for DLAA. DLSS takes the color, the
  depth (with NGX's inverted-depth flag when reversed), the vectors and the jitter. Its result is copied
  over the color, and the game's post-processing then reads the antialiased image.
  - After NGX records its commands, the command buffer is submitted and DiligentCore's state cache cleared
    (NGX binds its own compute pipelines).
  - DLAA is skipped while an occlusion query is open.
  - History starts again after a frame without the view.
- **`xenos_native_antialiasing = dlaa`** replaces the antialiasing of the finished scene. SMAA is used
  where DLSS is not available. The launcher's antialiasing row with XenosNative offers SMAA, DLAA (NVIDIA
  RTX), FXAA and off; SMAA stays the default.
- **Two DiligentCore patches:**
  - `0003`: DLSS at the output resolution is created with NGX's DLAA quality value;
  - `0004`: NGX's messages are routed to DiligentCore's log when NGX logging is on (NVIDIA's registry
    settings). The minimum level stays off, because at ON NGX writes log files next to the executable.

**Why:**
- **The viewport, not the matrix.** The game builds its matrices on the CPU and the main view shares them
  with its reflection. A sub-pixel shift of the viewport moves the rasterized image exactly, for every draw
  of the view, without knowing which constant is the projection. It also leaves the matrix unjittered for
  the motion vectors.
- **Before the post-processing.** That is where DLSS is meant to run. POTCAWE's post-processing (bloom, a
  blend with the last frame) then works on a stable image.
- **The depth range is the key.** POTCAWE clears depth to 0 and tests `GreaterEqual`: reversed depth, which
  its viewport gives as z from 1 to 0.
  - **What it caused:** the first version read depth as 0 near, 1 far. The motion vectors were noise,
    DLSS took the depth upside down, and the result was blurred (dumps of DLSS's input, vectors and output,
    run 225).
  - **Ruled out on the way:** the jitter's sign (four combinations), a one-frame offset of the jitter, the
    model (preset M or K), the HDR and exposure flags, and jittered draws that re-composite a copy of the
    scene. None changed it (runs 206–224).
  - **With the depth range:** the image is sharp. With the vectors at zero or negated it is softer again
    (runs 226–229): the vectors are right and needed, because POTCAWE's camera moves a little every frame.
- **Why the submission:** DiligentCore does not know what NGX binds. Without clearing its cache the scene
  was black (run 204). Clearing it with commands pending makes DiligentCore warn and loses track of them,
  hence the submission first.

**Evidence:** `local_docs/08-phase5.md`, runs 204–233, and the image dumps.

| Run | 1600×900, the dock | Result |
|---|---|---|
| 204 | first version, no submission | the 3D scene black |
| 205 | submission and cache cleared | correct image, but blurred |
| 206–224 | the experiments above | all blurred, except without jitter |
| 225 | dumps | DLSS's input sharp, its output blurred; the motion vectors noise |
| 226 | the depth range (reversed) | sharp; softer than SMAA only where SMAA keeps the aliasing |
| 228, 229 | vectors at zero, negated | softer: the computed vectors are needed |

| Cost (`gpu_ab`, 60 FPS) | SMAA | DLAA |
|---|---|---|
| 2× (2560×1440) | 28.1 % of the GPU, 70 W | 35.2 %, 85 W (about +1.2 ms per frame) |
| 3× (3840×2160) | 43.9 %, 122 W | 58.8 %, 161 W (about +2.5 ms per frame) |

**The log while DLAA runs:**
- **NGX:** DLSS 310.9.16, from the driver's override (`C:\ProgramData\NVIDIA\NGX\models`), not the
  bundled 310.5.3. Preset K for DLAA.
- **XenosNative's GPU timer** is not reliable with DLAA: the mid-frame submission puts the GPU's idle time
  between the two halves of the frame into the measurement. GPU utilization is the measure.

**Limits:**
- **Objects moving on their own** (characters, animated objects, water) have only the camera's motion
  vectors, so they may ghost or soften while they move. Their vectors need the last frame's transforms per
  draw: a later step.
- **Draws that sample a copy of the scene** at screen positions (POTCAWE's haze, the water) are jittered
  with the rest. That shifts their copied content twice; it showed nothing in the tests.
- **The upscaling modes** are not built: they need the 3D view at a second, lower render scale.

**Rejected:**
- **Jitter in the projection matrix** (a game hook): it needs to know the projection constant, and the
  reflection and the main view share the matrix.
- **DLSS on the finished scene,** where SMAA runs: after the game's post-processing, with a blend of
  the last frame already in it.

## D48 — DLAA: motion vectors of their own for the objects that move on their own (2026-10-14)

**Decision:**
- **Draws matched across frames.** With DLAA, each draw of the main 3D view that writes depth is matched
  with the last frame's. The key is its vertex shader, its first vertex fetch address, its index address
  and type, and its vertex count. Several draws with one key (a mesh drawn in several places) are told
  apart by their constants: the closest of the last frame's is the same object.
- **Moved or not.** A draw moved if its vertex shader's float constants changed, apart from the registers
  the whole view shares. Those are the registers that every draw using them sees with one value, and that
  half of the view's draws or more use. They are found in each frame for the next. In POTCAWE they are
  c0–c3 (the view-projection), c8, c9 and c27.
- **A motion vertex shader per translation.** The translated SPIR-V is patched
  (`spirv::MakeMotionVertexShader`):
  - a new entry point calls the shader's `main` twice;
  - in the first call every load of a float constant reads a second uniform block, the last frame's
    constants (`XeFloatConstantsPrevious`); in the second, this frame's. A select on a private flag
    chooses between the two blocks;
  - the clip-space position of each call goes to two extra outputs, locations 30 and 31.
  It is made on the shader workers, once the draw's own vertex shader is ready.
- **The motion draws.** A draw that moved is kept: its constants of both frames, the system, bool/loop
  and fetch constants, the viewport, the scissor and the index buffer.
  - **When.** It is drawn again at `EndViewGeometry`, after the camera's motion pass and before DLSS.
  - **How.** Its own state, with the depth tested equal to the scene's and not written, no stencil and no
    blending, into the motion vectors. A pixel shader turns the two positions into host pixels.
  - **Result.** Where the draw is what the scene shows, its motion replaces the camera's.
- **Left to the camera's motion:**
  - vertex shaders that sample textures (the motion pipeline binds none);
  - shaders that use the float constants other than through loads;
  - SPIR-V before 1.4.
- **`xenos_native_object_motion`** (default true) turns it off.

**Why:**
- **The draw's own shader, not the game's matrices.** POTCAWE skins its characters on the GPU. Their
  vertex shaders take all 256 constant registers: the bones in c31–c219, the world matrix in c4–c7. Running
  the draw's own vertex shader with the last frame's constants gives each vertex's exact motion, bones
  included, without knowing the game's constant layout. So the pass is a library feature, with nothing
  game-specific.
- **One function, called twice.** Cloning the function would mean telling every operand id from a literal,
  opcode by opcode. The select only rewrites loads. The driver inlines both calls and folds the flag, so
  each call reads one block.
- **Drawn at the end, depth equal.** The translator marks the position invariant and its arithmetic
  NoContraction (for multipass rendering). So the second call rebuilds, bit for bit, the depth the draw
  wrote. Testing equal against the final depth keeps only what the scene shows: what a later draw covered,
  and what alpha test discarded, keep the camera's motion. This needs no second depth target, and the
  targets change once per frame, not twice per moving draw.
- **The shared registers.** With the camera moving, every draw's constants change in c0–c3 and c9 (probe,
  run 236). Without the shared registers, every draw would be a motion draw. With them, about 150 of ~960
  draws are: Jack, the NPCs and animated props, whether the camera moves or not.

**Evidence:** `local_docs/08-phase5.md`, runs 234–245.
- **Constants probe (run 236).**
  - Camera still: 811 of ~1,019 draws per frame unchanged. About 150 changed: the skinned draws (189–191
    registers each, the bones and the world) and about 30 props (c4–c7).
  - Camera moving: every draw differs in c0–c3 and c9.
- **Reprojection test (run 239).** Frame B's color was compared with frame A's, fetched where the motion
  vectors point. Over the 81,006 pixels the objects' motion changed (5.6 %):

  | Vectors | Mean error | Median |
  |---|---|---|
  | zero | 0.093 | 0.045 |
  | camera only | 0.064 | 0.022 |
  | camera and objects | 0.032 | 0.007 |

- **Screenshots (runs 244, 245).** Jack runs toward the camera. With the camera's motion only, his hair
  leaves a trail over the pier and the dreadlocks smear; with the objects' motion they do not
  (`local_docs/evidence/object_motion_dlaa.png`).

| Cost (`gpu_ab`, 60 FPS, the dock) | Camera's motion only | With the objects' motion |
|---|---|---|
| 2× (2560×1440) | 35.5 % of the GPU | 34.9 % |
| 3× (3840×2160) | 59.3 % | 59.3 % |
| Render thread (CPU) | 4.8 ms per frame | 5.5 ms per frame (~150 motion draws) |

**Limits:**
- **Vertex data the CPU rewrites** (CPU skinning, particles in a dynamic buffer) is not followed. With the
  same constants it looks still; at a new address it is a new draw.
- **Not followed:**
  - transparent draws (no depth write), the water among them;
  - objects new this frame;
  - the frame after a history reset.
  All of them keep the camera's motion.
- **The motion pipelines are not in the stored records (D42).** They compile when a moving object first
  shows, so it has the camera's motion for a few frames. DiligentCore's pipeline cache keeps them for the
  next run.
- **A view with many moving objects** costs render thread time in proportion: about 4 µs per motion draw.

**Rejected:**
- **A motion output on every draw** (a second render target on every pipeline): it changes every pipeline
  and every guest pixel shader, and most draws stand still.
- **Comparing world matrices:** the library does not know which constants they are. The shared registers
  come from the data.
- **Cloning the vertex shader's function:** see above.

## D49 — The DLSS modes: the main 3D view at a reduced scale, upscaled before the post-processing (2026-10-14)

**Decision:**
- **The modes.** `xenos_native_antialiasing` takes `dlss_quality`, `dlss_balanced`, `dlss_performance` and
  `dlss_ultra_performance` besides `dlaa`. The launcher's antialiasing row with XenosNative offers them as
  DLSS Quality, Balanced, Performance and Ultra Performance. The input resolution is NGX's recommendation
  for the mode and the output: at 1600×900, 1067×600, 928×522, 800×450 and 533×300; at 3840×2160,
  2560×1440, 2227×1253, 1920×1080 and 1280×720.
- **The reduced targets.** While a 3D view is open (`BeginView` with a matrix, until `EndViewGeometry`), the
  targets of the game's output size get a second host texture at the reduced scale: the input size over
  the guest size, per axis. That view's draws, clears and resolves use it. Every other target (shadow
  maps, the reflection, the post-processing's) keeps the render scale. The same surfaces are used at the
  render scale after the view.
- **Resolves from a reduced target scale** into resolved textures, which stay at the render scale: color
  filtered, depth nearest.
- **At `EndViewGeometry`:**
  - DLSS takes the reduced color, depth and motion vectors, and writes its result into the surface's
    target at the render scale;
  - the view's other reduced targets are copied to theirs: the depth nearest, through a depth-writing pass;
  - if DLSS cannot run (an occlusion query open), the color is copied, filtered.
- **The view's draws:**
  - their viewports and scissors use their target's scale;
  - their samplers take the mip bias `log2(reduced / render scale)` (NVIDIA's guide);
  - the jitter keeps its Halton sequence, with `max(64, 8 × output pixels / input pixels)` phases.
- **The occlusion queries** report their samples at the game's scale from the scale of the phase they began
  in (each query keeps its own).
- **DiligentCore patch `0005`:** `SuperResolutionDesc::OptimizationType`, passed to NGX as the quality value
  when the input is smaller than the output. NGX picks the model (render preset) by it.

**Why:**
- **Resolved textures at one scale.** The translator bakes the render scale into the shaders where they
  revert it (D37, SDK patch 0005): in resolved textures' texel offsets and weights, and in the pixel position
  of parameter generation.
  - With every resolved texture at the render scale, the first stays right for every draw. The resolves
    inside the view (the scene copy and its depth, for the haze and the water) are upscaled on the way.
  - The second (VPOS) would be off by the ratio in a reduced view. No draw at the dock reads it (probe,
    run 248); a one-time warning names it if a game does.
  - So one translation, one pipeline and one stored record still serve every scale.
- **By phase, not by surface.** The scene's surfaces (`0x40243E70`, `0x40243EB0`) are drawn in the view,
  and resolved and drawn again by the post-processing after it (run 248). A second host texture per surface,
  chosen by the phase, keeps both right.
- **The depth is brought back.** The post-processing resolves the scene's depth again after the view (from
  `0x82457ED8`, for its blend with the last frame), so the target at the render scale needs it.
- **The quality value matters.** Without it, NGX created every mode as MaxPerf. With it, NGX picks its
  default model for each mode (DLSS 310: K for Quality and Balanced, M for Performance, L for Ultra
  Performance).

**Evidence:** `local_docs/08-phase5.md`, runs 248–266.
- **The frame (run 248)**, logged by a probe:
  - the reflection view (128²);
  - in the main view, the shadow maps (1024²), then the scene's clear (inside the view), 994 draws, the color
    and depth copies, and 25 more draws;
  - after `EndViewGeometry`, the post-processing's resolves of the scene's depth and color;
  - then the HUD view, which clears its own depth;
  - no draw with parameter generation.
- **Images (runs 249–253, 1600×900):** every mode gives the dock's whole image, haze, water, sun glare and
  HUD as DLAA does. The detail drops gradually down to Ultra Performance.
- **The sun's occlusion query:** its sample counts stay comparable in every mode (92–114 at the game's
  scale). Without the per-query scale, Ultra Performance would report about a tenth of them.

| Cost (`gpu_ab`, 60 FPS, the dock) | 2× (2560×1440) | 3× (3840×2160) |
|---|---|---|
| SMAA | 28.0 %, 70 W | 45.9 %, 122 W |
| DLAA | 36.8 %, 86 W | 60.5 %, 161 W |
| DLSS Quality | 32.2 %, 78 W | 51.4 %, 140 W |
| DLSS Balanced | 31.1 %, 76 W | 49.4 %, 136 W |
| DLSS Performance | 31.3 %, 81 W | 52.2 %, 146 W |
| DLSS Ultra Performance | 29.4 %, 75 W | 46.3 %, 130 W |

- **The saving is moderate.** POTCAWE's own rendering is light (about 6 ms at 3×, D38). Part of it does not
  shrink: the shadow maps, the reflection, the post-processing and the HUD at the output resolution. DLSS
  itself costs more at a larger output.
- **Performance costs more than Balanced:** NGX's default for it is the heavier model M.

**Limits:**
- **Only the main 3D view's output-sized targets are reduced.** The shadow maps stay at the render scale,
  3072² at 3×.
- **The pixel position of parameter generation** is not reverted for the reduced scale (warned once).
- **The scene copy inside the view is upscaled**, so the haze and the water sample it at the render scale
  from data at the reduced one: softer, as everything in the view.
- **The stencil of the reduced depth is not copied** to the target at the render scale. POTCAWE's HUD view
  clears it.

**Rejected:**
- **Resolved textures at the source's scale:** the baked scale would be wrong for the reduced ones.
- **A per-draw scale in the system constants** (a change to SDK patch 0005): not needed while no draw in
  the view reads its pixel position.
- **A shader per scale:** twice the translations and pipelines for the view's shaders, for the same reason
  as above.

## D50 — Resolves of 2_10_10_10_FLOAT targets take the guest format's range; SMAA unbinds its image (2026-10-15)

**Decision:**
- **The range at the resolve.** When a resolve copies a `2_10_10_10_FLOAT` render target, which is
  RGBA16F on the host, the copy first brings each value to what the guest format holds:
  - RGB clamped to the 7e3 range, 0 to 31.875;
  - alpha clamped to 0 to 1 and rounded to its 2 bits.

  The blitter's color sources (texel, MSAA and filtered) apply it when the resolve asks
  (`BlitOptions::float_7e3`).
- **SMAA unbinds the image it antialiases** before copying it and after writing it. The resolve's copy
  leaves the image bound as the render target, and SMAA's `CopyTexture` made DiligentCore unbind it with an
  info message every frame.

**Why:**
- **POTCAWE's journal** (Back), found by Gustavo in his long session: the pages showed the paused scene
  through them, in negative.
- **How the journal is drawn.** The journal is a 3D view drawn into the scene's surface (`2_10_10_10_FLOAT`),
  cleared to alpha 0.627. It is copied to a texture, and the frame's last composite blends it over the
  scene and HUD by that alpha.
- **What went wrong.** The pages' draws leave alpha above 1, which the guest's 2-bit alpha cannot hold. On
  the Xenos path the resolve packs the target into its EDRAM format, which clamps it. XenosNative copied
  the RGBA16F as it was, and the composite's `(1 - alpha)` subtracted the scene from the pages.
- **The SDK's backends do the same.** They draw `2_10_10_10_FLOAT` targets as RGBA16F with unclamped
  outputs (its "host render target path"), and their resolves read through the EDRAM format. Clamping at
  XenosNative's resolve matches what the Xenos path shows. RGB keeps its 16-bit precision (no 7e3 rounding),
  and only its range is the guest's.
- **The message flood** filled 5 MB of log in five minutes of play with SMAA and made the log rotate. It
  is older than D49: the runs that showed none had SMAA off.

**Evidence:** `local_docs/07-phase4.md`, runs 267–277.
- **The page textures are right:** dumps of the page's composition and of its 1024×512 target, before
  their resolve, show the parchment, the map and the text, nothing else.
- **The trace of the journal frame:**
  - the two pages composed and resolved;
  - the reflection, the shadows, the scene, the post-processing and the HUD;
  - the journal view (clear alpha 0.627, 46 draws, resolved as `k_16_16_16_16_EXPAND`);
  - the final composite.
- **After the change:** the journal matches the Xenos path. The dock with SMAA and with DLSS Quality
  differs from before only by run-to-run noise. No DiligentCore message with SMAA.

**Limits:**
- **Draws still blend with the unclamped values inside the target,** as on the SDK's host render target
  path. A draw whose result depends on alpha clamped before blending would still differ from the console,
  on both paths.
- **Other formats whose range the host format exceeds** (`16_16` and `16_16_16_16`, fixed point from -32 to
  32) are not brought to their range at the resolve. None has shown a problem.

## D51 — Resolves into cube textures (2026-10-15)

**Decision:**
- **The resolve.** A resolve whose destination is a cube texture writes one face: the call's `slice`
  argument. The face goes to that slice of a host cube texture, a six-slice 2D array created as a cube.
  The destination's size is decoded as a 2D texture's: width and height, and a stack depth of 6.
- **The registry.** Resolved textures are keyed by cube or not as well, and a lookup asks for one kind:
  - a cube fetch is given the newest cube texture resolved to its address, through its cube view;
  - a flat fetch is given the newest texture of the other kind, through its 2D array view.

  Presenting and the guest texture cache's "already resolved" check still take the newest of either kind.
- **Still skipped:** resolves into 3D textures. A 3D texture's size is encoded differently.

**Why:**
- **Gustavo's long session** (run 278) met a cube in an area of level 3 (01:54–02:00). There the game
  renders a 256×256 target (`k_32_FLOAT`) and resolves it into the six faces of a cube texture every frame,
  likely a point light's shadow map. XenosNative skipped all six resolves, so whatever sampled the cube got
  no data.
- **The face order** is the same in D3D and Vulkan (+X, −X, +Y, −Y, +Z, −Z). Cubes loaded from guest memory
  already reach the translated shaders as host cubes in that order.

**Evidence:**
- At the dock (run 279) nothing changed. The comparison against the last run differs only by an NPC that
  walked through the view, and there is no warning.
- The cube's area has not been checked yet: Gustavo is to compare it with the Xenos path.

**Limits:**
- **Resolves into 3D textures** are still skipped (logged once).
- **A cube texture is not presented.** A front buffer is never one.

## D52 — Unused render targets and resolved textures are freed after a minute; the pipeline cache is saved at most every 5 minutes (2026-10-15)

**Decision:**
- **Freeing.** Every host render target and resolved texture records the last frame it was used:
  - a render target when it is drawn to, cleared or resolved from;
  - a resolved texture when it is resolved to, bound to a draw or presented.

  Every 600 frames, those unused for 3600 frames (a minute at 60 FPS) are freed, and the log says how many
  and how much. A later use creates them again. Entries without a texture (an unsupported format) stay, so
  their warning is not repeated.
- **The pipeline cache.** It is saved 10 s after new pipelines, as in D45, and then at most every
  5 minutes. A failed save is tried again 10 s later. What is not saved yet is in the pipeline records
  (D42), compiled again at the next start.

**Why:**
- **Gustavo's long session** (run 278, 54 minutes, levels 1 to 3, 3840×2160).
  - **Nothing freed these.** Resolved textures grew from 44 to 101 (about 470 MB at 3×) and render targets
    from 12 to 19 (about 200 MB). New areas bring surfaces and resolve destinations of their own: the
    journal's page textures are at a new address in each level, 18 MB each at 3×.
  - **About 50 pipeline cache saves** of 32–43 MB were written while new areas brought pipelines every few
    seconds: some 2 GB. They were harmless to the frame rate (D45).
- **A minute is long enough.** What the game samples or presents stays used every frame. What it resolves
  only now and then (the journal's pages, a cube map) is resolved again before it is sampled.

**Evidence:** `local_docs/07-phase4.md`, run 280.
- The journal was opened, closed for 80 s and opened again. A minute after closing, 2 render targets
  (9 MB) and 8 resolved textures (6 MB) were freed.
- On reopening they were created again and the journal was right; the dock after it was right too.
- The pipeline cache was saved right after the first new pipelines.

**Limits:**
- **Device memory still grew in the session:** 1.42 GB after the first load, 2.16 GB at the end, 65 pages
  of DiligentCore's memory manager created and 6 freed.
  - The targets explain only part of it. The guest textures the SDK's cache keeps stay at about 383 MB,
    while 21 GB of them were loaded over the session.
  - Two possible causes, not measured yet:
    - fragmentation of the memory manager's pages;
    - textures the SDK's cache freed but that the pipelines' resource bindings still reference until those
      pipelines draw again.
- **A process killed between saves** loses the unsaved pipelines' binaries, not their records.

## D53 — The DLSS modes leave the launcher: experimental, from potcawe_options.toml only (2026-10-15)

**Decision:**
- **The launcher's antialiasing row** with XenosNative offers SMAA, DLAA (NVIDIA RTX), FXAA and off. DLSS
  Quality, Balanced, Performance and Ultra Performance (D49) are no longer in it.
- **`xenos_native_antialiasing` still takes the four DLSS values**, written by hand in
  `potcawe_options.toml`:
  - the file says so, in a comment the launcher writes above the key: experimental, for testing only, not
    finished;
  - when the file names one, the row shows it last, as "DLSS Quality (experimental)" (the word in the
    launcher's five languages), so that saving keeps it;
  - the cvar's description and the renderer's log line call them experimental.
- **Merged from POTCAWERecomp** (`b782265`): the launcher's Save writes the options at once and keeps the
  window open, its button reading "Saved" for a moment.

**Why:**
- **Gustavo's call:** the modes are not finished.
  - The image is right, but the saving against DLAA is moderate (D49) and Performance costs more than
    Balanced.
  - The resolution choices are being redesigned ("internal resolution").
- **Keeping them reachable** lets them be tested without a launcher that silently replaces them: before
  this change, an unknown value in the file became SMAA at the next save.

**Evidence:** a test copy of the launcher, in a folder of its own with a test options file (Gustavo's
launcher was open):
- with `dlss_quality`, the row shows five entries, "DLSS Quality (experimental)" selected, and Save keeps
  `dlss_quality`, the window staying open;
- with `dlaa`, four entries, and Save keeps `dlaa`.

## D54 — The launcher chooses XenosNative's image format and internal resolution (2026-10-15)

**Decision:**
- **The internal resolution row.** "Render quality" is now "Internal resolution" (made in POTCAWERecomp,
  `594be5e`, and merged). With XenosNative it is no longer disabled:
  - "Automatic", the screen's or the window's size as before;
  - then the usual heights below it (2160, 1800, 1440, 1080, 900, 720), down to the lowest the image
    allows: 720 lines, or 800 for an output narrower than 16:9, where the width keeps at least 1280;
  - nothing above the output (Gustavo: no supersampling);
  - a height the options file names is offered as well when it is in range. A height no longer offered
    (another monitor, another window size) becomes automatic.
- **A new "Image format" row**, enabled with XenosNative when the screen or the window is not 16:9: "Fill
  the screen" (D43) or "16:9 with bars". Otherwise it shows "16:9", disabled; for Xenos, "16:9 with bars".
- **The internal resolution follows the format.** Its widths are computed as the renderer computes them, by
  `NativeRenderSize(output, fill, height)`:
  - on a 3440×1440 screen, filling: 3440×1440, 2580×1080, 2150×900 and 1720×720;
  - with bars: 2560×1440, 1920×1080, 1600×900 and 1280×720.
- **The same in a window**, relative to the window's size.
- **The options file:** the launcher writes `xenos_native_fill_screen` and `xenos_native_resolution_scale`
  (the height over 720; 0 for automatic), with a comment saying so. The renderer already took both (D38,
  D43), and needs no change.

**Why:**
- **Gustavo's request.** A 4K screen should be able to render at 1440p. His ultrawide should be able to show
  the game at 2560×1440, which is a 16:9 image with bars: the image format and the internal resolution are
  two choices, hence two rows (his choice among three proposals).
- **Heights, not percentages.** They name the resolutions people know (1440p, 1080p), and give the exact
  1:1 case: 2560×1440 with bars on a 3440×1440 screen is not scaled at all.

**Evidence:** `local_docs/07-phase4.md`, runs 281 and 282, and a test copy of the launcher (Gustavo's was
open):
- **The rows** for Gustavo's 3440×1440 monitor, filling and with bars; for a 1600×900 window (format
  disabled); and for Xenos. Selecting 2150×900 saves `xenos_native_resolution_scale = 1.25`; with bars and
  1.5, 1920×1080 is selected.
- **The renderer in a 1600×1000 window:**
  - with bars and 1.25, it renders 1600×900 with bars above and below;
  - filling and 1.111111, 1280×800, the lowest for 16:10, as the launcher offers it.

**Limits:**
- **A lower internal resolution is scaled to the screen with a bilinear filter**, the HUD included (the game
  composes it into the same image): softer text. A sharpening filter could come later.
- **Outputs that are not 16:9 need `Profile::widens_cameras`** to fill (D43). Without it, the format row is
  only a label.

## D55 — Pipelines not drawn for 10 s let go of their textures; device memory in the log (2026-10-15)

**Decision:**
- **Idle pipelines let go of their textures.**
  - Every pipeline records the frame it last drew in.
  - Every 600 frames, the pipelines not drawn in those 600 frames set their texture variables to null.
    This happens at the end of the frame, after the object motion draws, which commit the frame's bindings
    again (D48).
  - Nothing needs those bindings again: every draw sets all of its pipeline's textures before it commits
    them.
  - The log says how many pipelines let go, and how many textures only they held were freed, with their
    size (watched with weak pointers through the reset).
- **Device memory in the log.**
  - DiligentCore patch `0006` adds `IRenderDeviceVk::GetMemoryUsage`. It reports what the device's global
    memory manager has in use and in its pages, device-local and host-visible, and the page count.
  - Every 600 frames the renderer logs it next to what XenosNative knows it holds: guest textures, render
    targets and resolved textures.
- **Texture sizes count mips and compressed blocks** (`TextureBytes`, from DiligentCore's mip level
  properties). Render targets and resolved textures have one mip, so D52's figures do not change.

**Why:**
- **The long session's growth** (run 278, D52's limits): 1.42 → 2.16 GB.
  - That figure was the memory manager's pages, not what is in them. A page is freed only when it is empty,
    so the pages follow the highest use.
  - **The parts at the end, at 3840×2160:**
    - guest textures at the SDK cache's soft limit, about 383 MB (`texture_cache_memory_limit_soft`, 384 MB,
      the same budget as the Xenos path);
    - resolved textures never freed, about 470 MB (D52 frees them now);
    - render targets, about 200 MB;
    - the fixed part: the 512 MB guest memory mirror, the 32 MB converted index buffer, and the output-size
      textures of the gamma ramp and the antialiasing.
  - **The rest could not be told apart in that log:** partly empty pages, or textures still held.
- **A resource binding holds its views, and a view holds its texture.**
  - In DiligentCore, a texture's default view shares the texture's reference counters, and any other view
    holds the texture.
  - So a pipeline that is not drawn again keeps the textures of its last draw alive. The SDK's texture
    cache may have evicted them, or D52 freed them: their memory stays taken while the game is elsewhere,
    and in a new level that is for good.

**Evidence:** `local_docs/07-phase4.md`, runs 283–287. All at the dock, in a 1600×900 window, turning the
camera for 3–8 minutes:
- **Default limits (283):** flat.
  - 851 MB in use in 880 MB of pages (28 pages);
  - of it: guest textures 142 MB, render targets 41 MB, resolved textures 64 MB, and about 604 MB fixed.
- **The SDK's cache evicting hard** (soft limit 64 MB, 2 s; runs 284, 285 and 287):
  - the guest textures swing between 74 and 128 MB, and the memory in use follows them;
  - the pages stay at 864–928 MB: no growth, and little fragmentation.
- **The journal (286):** opened, closed for 80 s, opened again.
  - Its pipelines let go of their textures once it closed.
  - A minute later D52 freed 2 render targets (9 MB) and 8 resolved textures (6 MB), and the memory in
    use dropped by those 15 MB (865 → 850 MB).
  - Reopened, it was right.
- **What the release frees by itself (287, the cache evicting hard):**
  - 16 textures (2 MB) of the menus' pipelines when the dock loaded;
  - then 0–10 textures (up to 1 MB) every 10 s at the dock.

  At the default limits (286) none: the SDK's cache still held them.

**Limits:**
- **At the dock few textures were held.** The release matters where the game leaves an area for good: a
  new level, which the automated drivers cannot reach. Gustavo's next long session will show it in the new
  log lines.
- **Partly empty pages are not compacted.** DiligentCore frees a page only when it is empty; at the dock
  the pages exceed the use by 30–100 MB.
- **The guest textures may still take up to the SDK cache's limits**: 384 MB soft, 768 MB hard, as on the
  Xenos path.

## D56 — XenosNative is POTCAWE's default renderer; the Xenos path stays as a choice and as the fallback (2026-10-15)

**Decision:**
- **POTCAWE draws with XenosNative by default.**
  - `src/startup.cpp` writes `xenos_native = true` into `potcawe.toml`. An existing file that lacks the key
    gets it appended, with its comment, as with every default there.
  - The launcher selects XenosNative when no file names a renderer.
  - The launcher no longer marks it "(experimental)". Its hint now offers Xenos for when something looks
    wrong, in the five languages.
  - Without `vulkan-1.dll` the launcher offers only Xenos, as before.
  - The library keeps `xenos_native` off: a game turns it on in its own configuration once it is ready.
- **The Xenos path stays**, through the launcher's Renderer row or `xenos_native = false`.
- **A fallback when XenosNative cannot start.**
  - XenosNative takes the window from the SDK's presenter before it creates its device.
  - If the device, the blitter or the switch that turns host rendering off fails, it now releases what it
    created and gives the window back to the SDK's presenter (`IGraphicsSystem::presenter()`). The Xenos
    path then draws.
  - Before, the window was left without a presenter.
- **Anchoring the HUD to the screen's edges moves to a later phase.** It means anchoring HUD elements to
  the screen's edges at wide aspect ratios, where the 2D layers are now drawn in a centered 16:9 box (D43).
  Gustavo's call; `plan.md`, "Later".

**Why:**
- **Gustavo's call:** Phase 4's exit question, "make XenosNative the default for POTCAWE, or keep it
  optional".
- **Performance:** at the same resolution without antialiasing, XenosNative uses 38–56 % less of the GPU
  (D39). At 3× it holds 60 FPS where the Xenos path with 2× MSAA drops to 33.
- **Features the Xenos path does not have:**
  - any internal resolution (D37, D54) and any aspect ratio (D43);
  - SMAA or FXAA (D41), and NVIDIA DLAA (D47);
  - a persistent pipeline cache (D42).
- **Four long sessions** through the first levels (runs 278, 288 and 291), the last one 64 minutes without
  any problem:
  - the memory in use stays bounded (D52, D55);
  - 60 FPS;
  - no message from the renderer.

  The only crash, in run 288, was the game's: the setjmp/longjmp fix.

**Evidence:** `local_docs/07-phase4.md`, run 292 and a clean copy.
- **The defaults, from an empty folder:** the exe, the launcher and their DLLs, with no toml file.
  - The launcher shows "Renderizador: XenosNative", SMAA and Vulkan, and the new hint.
  - The game logs `xenos_native=true` and `gpu_backend=vulkan`, renders with SMAA, and its new
    `potcawe.toml` ends with `xenos_native = true`.
- **An existing `potcawe.toml`** (the build's) got the key appended with its comment.
- **The fallback (run 292):** a temporary probe made the start fail after XenosNative had created its
  device. The log says "no host device; the Xenos path draws", the SDK's presenter creates its swap chain
  again, and the menus and the dock are drawn by the Xenos path at 60 FPS. The probe was removed.

**Limits:**
- **Not every level has been compared side by side with the Xenos path** (Phase 4's remaining item).
  Gustavo has played the first levels on XenosNative without finding a difference.
- **The fallback covers XenosNative's own start, not a machine without Vulkan.** With XenosNative on, the
  game runs the SDK's Vulkan backend (D27), which then has to work too. The launcher offers XenosNative
  only where `vulkan-1.dll` exists.
- **The known limits from Phase 2 stand** (`plan.md`).

## D57 — Phase 4 closed; its open checks move to "Later" (2026-10-15, Gustavo)

**Decision:** Phase 4 (POTCAWE polish) is closed. Phase 5 (temporal techniques) continues. Four checks were
not done as written, and move to the plan's "Later" section, to be done when a scene needs them or a
difference is reported:
- **Every level compared side by side with the Xenos path.** Gustavo played the first levels on
  XenosNative in four long sessions without seeing a difference. What those sessions found was fixed: the
  journal (D50), a cube map (D51), and the memory (D52, D55). The script error was the game's: the
  setjmp/longjmp fix.
- **The known limits from Phase 2:** partial clears, signed formats with separate data, 1_5_5_5 and
  DXT3A_AS_1_1_1_1, and resolves into 3D textures. Only the cube resolves were met (D51).
- **Other levels and the cutscenes at 21:9.** D43 was checked in the menus, the dock and the pause menu.
  The long sessions ran at 3840×2160 (16:9).
- **HUD elements anchored to the screen's edges**, already moved there by D56.

**What Phase 4 changed:**
- **Antialiasing:**
  - its own: SMAA (the default) or FXAA on the finished scene (D41);
  - NVIDIA DLAA (D47, from Phase 5).
- **A persistent pipeline cache** (D42): saved on a thread of its own (D45), and at most every 5 minutes
  (D52).
- **Any aspect ratio** without bars (D43).
- **Anisotropic filtering** at 16x (D44).
- **The launcher:**
  - the image format and the internal resolution (D54);
  - the DLSS modes made experimental (D53).
- **Long sessions:**
  - resolves of 2_10_10_10_FLOAT targets (D50) and into cube textures (D51);
  - unused targets freed (D52);
  - idle pipelines' textures released, and device memory in the log (D55).
- **XenosNative is POTCAWE's default renderer**, with a fallback to the Xenos path (D56).

**Why:**
- **The hour-long session is done:** 64 minutes with no problem, the memory in use bounded, 60 FPS, and no
  message from the renderer (run 291).
- **XenosNative is the default** (D56).
- **What remains are checks,** not known problems.

Gustavo: "Demos por cerrada la Fase 4." (2026-10-15).

## D58 — XenosNative becomes NativeXRenderer, a public project in a repository of its own (2026-10-15, Gustavo)

**Decision:**
- **The name.** XenosNative is now **NativeXRenderer**.
  - The identifiers are the name in snake_case, as D8 did: the folder, CMake target, namespace and include
    path `native_x_renderer`, the cvars `native_x_renderer` and `native_x_renderer_*`, and the log prefix
    `[native_x_renderer]`.
  - The submodules' branches are `native-x-renderer`.
  - POTCAWE's side follows: `src/native_x_renderer_potcawe.cpp` and `docs/native-x-renderer-potcawe.md`.
  - The trace reader is `tools/frame_trace.py`, and `ppc_index.py`'s variable is `PPC_INDEX`.
- **The decision log keeps the old name** in D1–D57, as its rule says (old entries are not rewritten). Read
  `XenosNative` there as NativeXRenderer, and `xenos_native` as `native_x_renderer`.
- **Not renamed:**
  - "the Xenos path", which is the SDK's emulation of the Xbox 360's GPU, Xenos;
  - the pipeline storage's internal magic (`XNPS`).
- **The project is public,** in a repository of its own (`ElGuslml/NativeXRenderer`), with DiligentCore as
  its own submodule. POTCAWE uses it as a submodule. Its history is kept: 49 commits, split out of this
  repository with `git subtree split` before the rename. This brings forward the extraction that Phase 6
  planned.
- **POTCAWE's first release** is published on the public POTCAWERecomp repository, built from public
  sources.

**Why:**
- **Gustavo's call: no "Xenos" in the name,** to avoid confusion with the SDK's Xenos emulation and with
  other projects. XenosRecomp, the Xbox 360 shader recompiler of Unleashed Recompiled, is one.
- **Public, so that the release can be built from the published sources.**

**What changes for a user:**
- The old keys in `potcawe_options.toml` (`xenos_native*`) are no longer read. The launcher writes the new
  ones at its next Save, and until then the defaults apply: NativeXRenderer, SMAA, filling the screen,
  automatic resolution.
- The pipeline storage moves to `userdata/cache/native_x_renderer/`. It is prepared again once, at the first
  run.

## D59 — GRAW is the second game; the profile describes what changes between XDK versions (2026-10-15, Gustavo)

**Decision:**
- **GRAW goes second, then Kameo,** as Phase 6 planned. Gustavo first asked for Kameo (XDK 2.0.2099), then
  chose to follow the plan.
- **The profile describes what NativeXRenderer had taken from POTCAWE's XDK (5426).** GRAW's XDK (2417)
  differs in the device and in the resource objects:
  - **The dirty masks** (`DeviceLayout`): `dirty_masks`, six masks from one offset, becomes the three masks
    the state tracker reads, each with its offset (VS constants, PS constants, fetch slots), and
    `fetch_dirty_slot0_bit`, the bit of fetch slot 0. POTCAWE: `+0`, `+8`, `+24`, bit 31. GRAW: `+16`,
    `+24`, `+32`, bit 63.
  - **The resource objects** (`ObjectLayout`, new in `Profile`): a texture's fetch constant, a surface's
    RB_SURFACE_INFO, its color or depth info and its size (a u32 with two bit fields), a buffer's address
    and size. They were constants in `targets.cpp`, `renderer.cpp` and `trace.cpp`.
  - **An incomplete profile turns a feature off.** The observer and the renderer check that the profile has
    what they read; if not, they log what is missing, and the Xenos path draws.
- **Inline draws the game fills itself.** New flags in `calls.inc`: `kInlineBegin` for BeginVertices and
  the new `BeginIndexedVertices`, `kInlineEnd` for EndVertices, which is now the draw (`kDraw`). The begin is
  tracked as an ordinary call (its flush commits the state and the programs) and keeps its dirty masks for
  the end, which reads the fetch constant and the draw packet the begin left after the write pointer.
- **Two state tracker rules, generalized:**
  - a fetch dword that a draw's own packets write with the shadow's value comes from the shadow: the
    override from an earlier packet ends there;
  - a segment change inside another one (the kick calling the new-segment function) reads nothing.
- Interface version 0.5 (`kVersionMinor`). POTCAWE's profile names the masks and its object layout, which
  were the library's constants.

**Why:**
- **The masks.** In XDK 2417 the fetch slots are the high half of the third mask, slot s in bit 63 − s;
  the library read the low half of the fourth (POTCAWE). The first observer run on GRAW gave 78.5 % of the
  draws matching.
- **The objects.** A surface's info is at `+48`/`+52` in GRAW, `+24`/`+28` in POTCAWE, and its size is
  encoded differently; a texture's fetch constant is at `+16`, not `+28`. The renderer would have read wrong
  surfaces and front buffers.
- **Inline draws.** GRAW's engine calls BeginVertices, BeginIndexedVertices and EndVertices itself (7 per
  frame in training: the present's quads and particles). POTCAWE only reaches BeginVertices from
  DrawVerticesUP, so a draw at BeginVertices' return never mattered there. In GRAW it saw no draw packet,
  and taking only the end missed the programs the begin's flush loaded.
- **The fetch rule.** GRAW's SetStreamSource records the stream only; the draw's flush writes the vertex
  fetch constant into the shadow and sets its dirty bit after the tracker read the masks. The override an
  inline draw's packets left on slot 31 survived every later draw: 1346 mismatches on vertex fetch 95.
- **The segment rule.** GRAW's kick writes the segment's fence and leaves a submission record past the
  submitted range before it calls the new-segment function. Reading those as packets made the scan stop
  (or misparse) in the middle of draws, Present and BeginTiling.

**Results:**
- **GRAW, training mission** (`tools/nav_training.ps1`, frames 2100–2102): 3888 of 3888 draw calls match
  the rebuilt state (100 %), no call read incompletely, no program, register, constant or fetch mismatch.
  The 95 draw packets per frame without a call are the library's own (clears, resolves,
  SetShaderGPRAllocation's one-point draws).
- **POTCAWE, Port Royal dock**: 1132 of 1132 (100 %), as before; with NativeXRenderer drawing, the same
  image at 60 FPS.

**Revisit if:** a third XDK needs another mask (the bool and loop constants, the register groups): the
tracker reads them from the shadow at every draw today, so it has not needed their masks.

## D60 — A tiled pass is drawn once, at the size of all its tiles, without the game's MSAA (2026-10-16)

**Decision:**
- **A tiled pass** (BeginTiling…EndTiling) is drawn once into host targets of the size its tiles cover
  together. BeginTiling's rectangles (D3DRECTs in the whole image's coordinates) give that size; every
  surface bound inside the bracket gets a host target at least that large. Each tile's resolve copies its
  own rectangle from there, so the per-tile resolves need no merging.
- **Host render targets have one sample**, whatever MSAA the guest surface has (D31: the game's MSAA is not
  brought over). Each draw shows the translated shaders a surface without MSAA (RB_SURFACE_INFO's MSAA field
  cleared for the draw): alpha to coverage dithers as it does at 1× on the console, and no interpolator is
  centroid.
- An EndTiling that resolves into a texture is logged once and not supported (no game has one yet).
- **The SDK's shader storage is turned off** (`store_shaders = false`) when NativeXRenderer draws: it holds
  the Xenos path's pipelines, and the runtime creates them all before the game's first instruction.

**Why:**
- GRAW renders its scene in three tiles of 1280×256 with 4× MSAA (`2_10_10_10_FLOAT`). The library drew the
  pass once at the surface's size, so only the top 256 rows would show (POTCAWE's D25, with its 2× MSAA).
  POTCAWE avoided it with its own hooks, which turn its MSAA off and leave one tile; the tiles' size makes that
  unnecessary for any game.
- GRAW resolves each tile with its rectangle and a destination point at the tile's corner (the engine's
  `sub_821959F8`), as POTCAWE does: copying each rectangle from one full-size target rebuilds the image.
- With one sample per pixel the shaders must not compute coverage for four.
- GRAW waited more than 14 s at startup while the runtime created 400 Vulkan pipelines of the Xenos path's
  `fsi` mode, which NativeXRenderer never uses.

**Results:** GRAW's training mission (`tools/nav_training.ps1`, 1280×720): the three bands form one image with
no seam; 1272–1346 draws per frame, all drawn; 60 FPS, 1.7–1.8 ms of GPU time per frame. The game starts in
about a second. Evidence: GRAWRecomp `local_docs/03-native-x-renderer-first-frames.md`.

**Revisit if:** a game reads MSAA samples itself, or host MSAA becomes one of NativeXRenderer's antialiasing
choices.

## D61 — Only device calls, and what they call, reach the state tracker and the renderer (2026-10-16)

**Decision:** whether a call goes to the state tracker and the renderer is decided per thread, at its
outermost call: a device call (first argument the device) or a call that writes into the device's command
buffer (`kPackets`) does, with every call nested in it. Other outermost calls (the resource Lock and Unlock,
the cache flush) only reach the trace.

**Why:**
- GRAW locks and unlocks vertex and index buffers from another thread (the engine's `sub_821A1000`), while
  the render thread is inside a D3D call. The tracker pushed an inactive entry for every call onto its call
  stack, one `std::vector` for all threads: two threads pushing and popping at once corrupted the heap. The
  renderer's record of the outermost call's arguments was overwritten too.
- The crashes had many faces, all within seconds of the first frame: heap corruption found by SPIRV-Cross and
  the SPIR-V builder on the shader workers, an access violation in the runtime's `WaitMultiple`, a packet scan
  of unmapped memory, and the tracker's own stack. 6 runs in 7 crashed, with and without D60's changes.
- On one thread nothing changes: neither the tracker nor the renderer used those calls.

**Results:** 3 runs of 45 s with no crash, then the training mission (D60's results).

**Revisit if:** a game makes device calls from two threads; the tracker assumes one thread at a time inside the
device, which the XDK requires anyway.

## D62 — The occlusion query pool holds GRAW's queries; a frame has a budget (2026-10-16)

**Decision:**
- DiligentCore's occlusion query pool has 4096 slots (its default is 128).
- A frame begins at most 2048 host queries. Past that, a guest query gets none, and the command processor
  answers it with its address's latest count.
- At most 1024 queries wait for their results (32 before). A query dropped from the wait, or abandoned, is
  kept for reuse instead of destroyed.
- The log reports the most queries begun in a frame and the ones over the budget, and lists at most 8
  addresses.

**Why:**
- GRAW begins up to 367 occlusion queries in a frame, for 300–370 addresses (POTCAWE: two).
- DiligentCore takes a pool slot at each begin and gets the slots back only when the command buffer is
  submitted, at Present. With 128 the pool ran out in the middle of a frame; the query had no slot, and
  reading its result crashed (`QueryVkImpl::GetData`).
- With 32 waiting, the results of all but the last 32 queries of each frame were dropped, and those addresses
  would have answered 0 samples: not visible.
- The queries alive (at most 1025) and the slots a frame takes (at most 2048) fit in the pool together.

**Results:** GRAW's training mission: 31095 queries in 600 frames, 367 at most in a frame, none over the budget,
results for 367 addresses, no errors.

**Revisit if:** a game begins more than 2048 queries in a frame.

## D63 — GRAW's false colors: cube maps sampled with gradients on NVIDIA; fixed in the SDK, not with "if blocks" (2026-10-16, Gustavo)

**Decision:**
- **The SDK's SPIR-V translator samples cube maps at a LOD it computes** (shared SDK patch 0021, in the games'
  `patches/`, since the SDK's own Vulkan path needs it too): `log2(max(|ddx|, |ddy|) * width / 2)` from the
  gradients of the direction, which include the LOD bias as a scale, sampled with an explicit LOD instead of
  the gradients. The bias scale's LOD is clamped to ±32.
- **The "if blocks" mode** that the plan's risk proposed for the translator's `loop { switch (pc) }` was
  written and tried at Gustavo's request, and dropped: it changed the false colors without removing them,
  and the switch turned out not to be their cause. Its diff is kept in GRAWRecomp's
  `local_docs/evidence/03_first_frames/`.

**Why:**
- In a RenderDoc capture of GRAW, the shader debugger computes the right color for a green pixel from the
  same SPIR-V and inputs. Replacing the draw's pixel shader with patched SPIR-V and reading the GPU's
  intermediate values (GRAWRecomp `local_docs/03`) showed every value right but one: the cube map sample of
  the ambient light, garbage in the first pixel of each 2×2 quad (negative values from an RGBA16 UNORM
  texture). It stays garbage with the gradients scaled by 1, and becomes right with an explicit LOD.
- The guest asks for LOD 150 (the cube's smallest mip) through `setTexLOD`; the translator applies it as a
  gradient scale, 2^150, which is infinite: zero derivatives became NaN gradients.
- The Xenos path's Vulkan `fbo` mode showed the same false colors in GRAW; with the patch it draws the
  training mission with the D3D12 path's colors. NativeXRenderer translates with the same code.

**Results:** GRAW's training mission with NativeXRenderer has the Xenos path's colors (no green surfaces, no
dot pattern but the fence's alpha-to-coverage dither); POTCAWE's dock is unchanged.

**Revisit if:** a driver gets cube map gradients right (anisotropic filtering then comes back to cube maps),
or a game's cube coordinates are unnormalized (the LOD assumes the face spans 2 units).

## D64 — A depth resolve read as k_8_8_8_8 binds its depth and stencil words (2026-10-16)

**Decision:**
- A resolve of a depth-stencil surface into a `k_24_8` or `k_24_8_FLOAT` texture writes, in the guest's
  memory, 32-bit words: the depth in bits 8–31 (float24 for D24FS8, unorm24 for D24S8) and the stencil in
  0–7. When a fetch reads that texture's address as `k_8_8_8_8`, NativeXRenderer binds those words as an RGBA8
  texture (R the stencil, G, B, A the depth's bytes, low first) instead of the resolve's depth (R32_FLOAT).
- The words are written on demand: the first such fetch marks the resolved texture, and from then on every
  depth resolve into it also packs the host depth and stencil into the words (a blit,
  `Blitter::Source::kDepthStencilPacked`, through a stencil view of the depth target). Until then the fetch
  gets zero words (no stencil). Depth resolves nobody reads that way (shadow maps) cost nothing more.
- Other reinterpretations of a resolve (another format than the one resolved to) are not handled yet.

**Why:** GRAW marks its objective in the stencil, resolves the depth-stencil surface to memory as
`k_24_8_FLOAT` and reads that memory as `k_8_8_8_8` to get the stencil, whose edges a full-screen pass
draws in yellow: the objective outline, and through the same mask, whether the reticle shows the objective's
arrow. The resolve registry found the texture by address only and bound the depth, so the pass read the
depth the last frame's HUD had left as "stencil": large yellow outlines around the HUD, and no arrow. The
Xenos path's capture showed the pass reading an RGBA8 texture with the stencil in R.

**Results:** GRAW's training mission: no yellow outlines, the objective's arrow and line in the reticle, as
on the Xenos path.

**Revisit if:** a game reads a resolve with another format than these two (the words would have to be
written for that format too).

## D65 — The scene resolve can be named by two return addresses; it is of the game's output size (2026-10-16)

**Decision:**
- The profile's `scene_resolve_outer_caller` (interface 0.6): the return address one level above
  `scene_resolve_caller`, for games whose resolves all go through a function of their own. It is read from
  the guest stack: the back chain gives the caller's stack pointer, 8 bytes below which the Xbox 360
  compiler saves the return address (`__savegprlr`, `internal.h`'s `OuterReturnAddress`). 0: not checked.
- The trace prints both addresses for every call: `from <caller> < <its caller>`.
- The scene resolve must also be of the game's output size (`output_width` × `output_height`).

**Why:** GRAW resolves through one engine function, `sub_82114588`, reached through function pointers:
its return address into Resolve is the same for 25 of the frame's 31 resolves. One level up, the post-processing
passes share one (`sub_82194CC0`): among them only the scene copy is 1280×720, and the other 1280×720 resolve
through `sub_82114588`, the front buffer's, comes from the Present. The size rule only states what the
finished scene is; POTCAWE's scene resolve already is of its output size.

**Results:** GRAW: SMAA applied once per frame in the mission, before the HUD (its text stays sharp), 0.1 ms of
GPU time at 1280×720.

**Revisit if:** a game's finished scene is not of its output size (a game that renders at a lower size and
upscales itself), or the compiler saved the return address elsewhere.

## D66 — GRAW's launcher offers NativeXRenderer as experimental; GRAW fills ultrawide screens with its own hooks (2026-10-16, Gustavo)

**Decision:**
- GRAW Launcher gets POTCAWE's rows (D25, D54): "Renderer" (Xenos, or NativeXRenderer marked experimental,
  offered when Vulkan is installed), NativeXRenderer's internal resolution, its antialiasing (SMAA, FXAA or
  off) in place of the game's 4× MSAA, anisotropic filtering (D44, 16× by default), and Save keeps the window
  open. Xenos stays the default.
- DLAA is not offered: GRAW does not mark its views yet (`BeginView` / `EndViewGeometry`). Chosen without
  them, DLAA leaves the image without antialiasing: the scene resolve's SMAA is off while DLSS is on, and
  DLSS runs at `EndViewGeometry`.
- GRAW sets `Profile::widens_cameras`. Its ultrawide hooks for the Xenos path widen the 16:9 projections and
  narrow the HUD around its center, for an image the presenter stretches. When NativeXRenderer draws they
  take `AspectFactor()`, and the renderer stretches instead of the presenter. `SetGameAspectBox` is not used.
- GRAW's defaults keep `native_x_renderer_fill_screen` off. The launcher turns it on with "Ultrawide" on an
  output wider than 16:9 only: the HUD hook narrows on x only, so a taller output would distort the HUD
  (GRAW warns once).

**Why:**
- Gustavo asked for POTCAWE's launcher in GRAW: the renderer, the resolution, the antialiasing and 16×
  anisotropic filtering. He plays on a 3440×1440 monitor, where the Xenos path already offers ultrawide.
- GRAW's hooks already do what D43 asks of a game for wider outputs: the camera is widened before the
  culling, and the HUD stays at 16:9. Reusing them gives NativeXRenderer the Xenos path's ultrawide image,
  menus included (the hook narrows them too, with black at the sides).

**Results:** the training mission and the campaign menu in a 2580×1080 window: NativeXRenderer filling it
(rendered at 2580×1080, cameras ×1.3438) against the Xenos path's ultrawide on D3D12 at 2×: the same framing,
HUD and menus, 60 FPS in both. GRAWRecomp `local_docs/06-launcher.md`.

**Revisit if:** GRAW's HUD should anchor to the screen's edges, or a taller output (16:10, the Steam Deck)
should fill: the HUD hook would then need the vertical axis, or the box.

## D67 — Occlusion queries are measured only with the SDK's occlusion_query_enable on; GRAW keeps the fake count (2026-10-16, Gustavo)

**Decision:** the renderer measures the guest's occlusion queries (D32) only when the SDK's
`occlusion_query_enable` is on, its default. With it off, the resolver is not set, and the command processor
answers its fake sample count, as on the Xenos path. GRAW's defaults turn it off (`yeti.toml`); POTCAWE keeps
it on, for its sun's glare.

**Why:**
- Gustavo saw elements vanish for a few frames in GRAW's training mission, with NativeXRenderer only, the
  camera still: the objective's arrow and its line in the reticle, a barred window across the street. A
  burst of 80 screenshots caught the line missing in 4 of them, the arrow in 3.
- D32 answers a query with the latest count that arrived for its sample count address, one or two frames
  old. That holds in POTCAWE, whose two query objects measure the same thing every frame. GRAW keeps a pool
  of about 400 query objects (300–430 addresses in 600 frames, 150–430 begun per frame) and gives them to
  different objects from frame to frame: an object can get the count of another one that was hidden, or 0
  before its address has a result, and the game skips it until its query says it is visible again.
- The Xenos path never measured GRAW's queries: its host queries make the game wait (44–50 FPS, GRAWRecomp's
  step 0), so GRAW's defaults already turned them off there. One cvar now means the same for both paths.

**Results:** GRAW's training mission, 1280×720: a burst of 120 screenshots with the arrow and the line in
every one (before: missing in 4 of 80). GPU time 1.83 → 1.50 ms per frame at 1×, with the same draws: the
queries no longer end the render pass (D32's barrier). GRAWRecomp `local_docs/07-flicker-occlusion-queries.md`.

**Revisit if:** a game needs measured counts and reuses its query objects for different things. Each answer
would then have to come from the same use of the query, by waiting for it (the cost D32 avoided) or by the
game reading its results later than it does.

## D68 — The render thread's per-call costs cut: clock reads, cvar lookups, shader hashes, the shadow copy (2026-10-17, Gustavo)

**Decision:** five changes on the render thread, none of them visible in the image.
- **Only the calls that do work are timed.** `RendererTime` (the log's "renderer CPU") read the clock on entry
  and exit of every hooked call. It now covers ClearF, the draws, BeginTiling, EndTiling, Resolve, Query Issue
  and Present. The other calls only feed the state tracker.
- **The SDK cvars are looked up once per frame.** The forwarding accessors (D15, `sdk_cvars.h`) looked the
  plugin's value up by name on every read: the registry's lock, a `std::string` key, a conversion from text.
  `anisotropic_override` was read per sampler and `half_pixel_offset` per draw. Each accessor now keeps its own
  value per thread and looks it up again when the frame changes (`sdk::NextFrame`, at the renderer's frame end).
  A cvar changed at run time reaches the renderer from the next frame on.
- **A program's hash is computed once per load.** The state tracker hashes the microcode (XXH3) when it applies
  an `IM_LOAD`, and the draw path passes it to `ShaderCache::Get`, which hashed both programs on every draw.
- **The shadow copy is precomputed.** A draw copies the device's register groups, fetch constants and
  bool/loop constants from the shadow. The groups become runs without the packet registers when the tracker
  is created, and every run is one byte-swapping loop the compiler vectorizes; before, each register was
  checked against the packet register list with `std::find`. The fetch constants are copied whole and the
  overridden ones patched after.
- **The shader loads' microcode goes into one buffer** of the packet scan, which keeps its capacity, instead
  of a `std::vector` per load.

**Why:** a sampling profile of GRAW's render thread in the training mission (GRAWRecomp
`local_docs/05-performance.md`): NativeXRenderer took 62 % of the thread's busy time. The clock reads were
6.2 %, the cvar lookups 3.5 % with their allocations, `Apply` 8.9 %, the shader hashes 2.6 %. The game's
D3D calls are mostly state calls that do nothing in the renderer, and every one of them paid for two clock
reads.

**Results:** GRAW's training mission, the same spot, two runs each, library D67 against D68: the render
thread 55.8 / 57.6 % of a core → 46.4 / 44.4 % (about 1.9 ms less per frame at 60 FPS); the process 0.91 /
0.92 → 0.81 / 0.79 cores; the same 1327 draws, the same GPU time (5.8 ms at 3×) and the same image. The
observer on the Xenos path: 3750 of 3750 draws fully match, 1,961,208 float constants compared, the programs
equal. POTCAWE at the dock: the same image.

**Left:** the guest memory hashing of D35 (about 6 % of the render thread: every range a draw reads is
hashed to find what changed), DiligentCore's and the driver's per-draw work (descriptor sets, pipeline
binds, about 15 %), and the packet scan's per-dword record of the flush's constant writes.

## D69 — Alpha to coverage without MSAA: blended by the alpha, depth where the alpha is at least 0.5 (2026-10-17, Gustavo)

**Decision:** a draw with alpha to coverage (`RB_COLORCONTROL.alpha_to_mask_enable`) on NativeXRenderer's host
targets, which have one sample (D60), no longer uses the translated shader's 2×2 dither:
- a color target that does not blend blends by the shader's alpha (`SRC_ALPHA`, `INV_SRC_ALPHA`, for color and
  alpha); a target that blends already keeps its blend;
- a draw that writes depth tests the alpha against 0.5 (`>=`), or keeps the game's test where it is a
  `>`/`>=` test with a higher reference;
- the cvar `native_x_renderer_alpha_to_coverage` chooses: `blend` (the default) or `dither` (as before).

**Why:**
- With the game's MSAA, alpha to coverage covers a fraction of each pixel's samples equal to the alpha, and the
  resolve blends them with what is behind. With one sample the shader dithers it in 2×2 pixels: GRAW's
  chain-link fences turned into scattered dark dots, the trees' edges and the cables dotted (Gustavo, the
  open item of D60). Blending by the alpha is what the resolve gives, without the pattern.
- **Depth.** GRAW draws these objects twice (about 125 draws a frame in the training mission): a depth-only
  pass with alpha to coverage and an alpha test (`>` 0.235, 0.3, 0.8...), then the color pass with the depth
  test `>=` against it (reversed depth: GRAW clears depth to 0) and no depth write. Written where the alpha
  is at least 0.5, the depth keeps the color pass to the front layer where the object is mostly covered, and
  later geometry behind it hidden there. Where the alpha is lower, the color pass still passes against what
  is behind and blends over it; geometry drawn behind it later is drawn over it, as through the uncovered
  samples of the MSAA.
- **Draws that blend already.** GRAW fades some objects in with alpha blending and alpha to coverage together.
  With MSAA each covered sample is blended too, so the result is closer to alpha × alpha; a fixed-function
  blend cannot square the alpha, so they keep their blend, and come out somewhat more opaque.

**Results:** GRAW's training mission at 1× (SMAA off for the comparison), against the Xenos path at 1× with
the game's 4× MSAA (D3D12 and Vulkan): the fence a faint mesh as on the Xenos path instead of dots, the tree's
and the cables' edges without the pattern; the same GPU time (1.43–1.50 ms). POTCAWE at the dock: the same
image (its palms are alpha tested, without coverage). GRAWRecomp `local_docs/08-alpha-to-coverage.md`.

**Not solved by it:** thin geometry and thin alpha-tested lines narrower than a pixel (GRAW's barred windows)
stay aliased and dotted at 1×: the Xenos path's MSAA smooths their edges. A higher internal resolution,
host MSAA or DLAA would.

## D70 — Resolved textures are dropped when their guest memory is allocated again or written (2026-10-17, Gustavo)

**Decision:** a resolved texture is invalidated when the guest memory it stands for changes, and fetches read
the guest's data there again, as on the Xenos path:
- each resolve records the bytes its destination covers in guest memory (the base level and every slice or
  face, laid out as its fetch constant says), and marks them in the guest memory mirror as valid and written
  by the GPU (`GuestMemory::ResolveWritten`, the SDK's `MakeRangeValid`, as `RangeWrittenByGpu` does for the
  Xenos path without firing the watches): the pages become write-watched;
- `ResolvedTextures` registers a global watch on the mirror. The mirror fires it on guest threads for a CPU
  write to a watched page, a new physical allocation (shared SDK patch, `invalidate_physical_on_alloc`) and a
  protection change; the callback queues the range, and the render thread applies the queue before each
  lookup and resolve. A texture whose range it overlaps is invalidated; the next resolve into it makes it
  valid again;
- `FindNewest` returns nothing when the newest texture at the address is invalidated, so the fetch binds the
  guest texture and the texture cache loads it (its own watch has marked it outdated);
- a fetch of a block-compressed format (DXT, DXN, CTX1: 4×4 blocks) never binds a resolved texture: a resolve
  does not write them.

**Why:**
- Resolved textures were found by exact address forever. Games free a resolve destination's memory and use it
  for textures they fill with the CPU. Seen in GRAW: a DXT3 128×32 texture at `0x1E5D4000` was bound to a
  stale 64×64 `k_24_8_FLOAT` depth resolve every run; the Cross-Com video (Bink planes `k_8` written by the
  CPU, drawn into a 160×120 target and resolved) gets a block of its own for each video, freed when it ends
  (the resolve at `0xE102000`, invalidated with the block `0xE102000`–`0xE125FFF`). A plane fetched at the
  address of the last video's resolve read that RGBA texture instead of the plane: the likely cause of the
  green-tinted video Gustavo saw at 120 FPS (not reproduced in four loads of his save before or after).
- **The mirror's watches.** They are what the Xenos path's caches use, and they see every CPU write (Unlock or
  not) and every allocation. Marking the pages GPU-written is what keeps a resolve from being invalidated by
  writes next to it: the mirror widens the invalidation around a CPU write up to 256 KB, but stops at
  GPU-written pages. The mirror's data in those pages stays what was last uploaded; a draw reading them as
  vertices read the guest's memory before, which a resolve did not write either.

**Results:** GRAW, the save Gustavo loaded (`yeti_158`) and the training mission (`yeti_159`): three
invalidations each, all at reallocations: the menus' 1280×720 resolve while the mission loads, the 64×64 depth
resolve, and the Cross-Com's 160×120 resolve when its video ends. The Cross-Com video, the HUD's objective
outline (D64) and the scene as before; the frame time and the renderer's CPU time unchanged (12.9 ms and
5.6 ms per frame in the mission at 120 Hz, as before). GRAWRecomp `local_docs/10-crosscom-stale-resolve.md`.

**Rejected:** watching the resolve destinations with a memory invalidation callback of NativeXRenderer's
own: it would get the exact pages of a CPU write, but its unwatch range would either narrow the mirror's
256 KB unprotection (many more access violations, which the SDK's comment measures) or let the mirror's
widening unprotect a resolve's pages without notice.

## D71 — The renderer on a thread of its own; the command processor waits for it before it signals the game (2026-10-17, Gustavo)

**Decision:**
- Every hooked call is captured on the game's render thread into a command (`src/commands.h`): the register
  changes since the last command, the programs (shared copies, kept by the hash of their microcode), the
  surfaces bound as their objects say, the call's arguments and what they point to (a clear's color, a
  resolve's rectangles, its texture's fetch constant and RB_COPY_DEST_INFO, the size of a tiled pass, a query's
  sample count address, the front buffer's fetch constant), and an inline draw's vertices and indices.
- A thread of its own (`NativeXRenderer`) runs the commands in order with the code that ran inside the calls
  before: render targets, clears, resolves, the draw path, tiling, queries, the views (BeginView,
  EndViewGeometry, the aspect box), Present and the frame's end. It owns its copy of the register file,
  DiligentCore's context and the driver. The queue holds 4096 commands, which keep their memory from one use
  to the next; the game's thread waits when it is full.
- SDK patch 0006 (`IGraphicsSystem::SetSignalGate`): the command processor calls the renderer before every
  signal to the guest: the packets that write guest memory (EVENT_WRITE_SHD fences, MEM_WRITE, COND_WRITE,
  REG_TO_MEM, EVENT_WRITE_EXT and _ZPD), the scratch register and read pointer write-backs, and the interrupts
  (INTERRUPT, XE_SWAP). The renderer returns once every command queued so far has run.
- When an inline draw is drawn, its copy is compared with guest memory; if the game reused that memory, the
  copy goes into the mirror and those pages are invalidated.
- The tracker notes the registers whose value changes, comparing as it copies (in blocks of 16, which one
  vectorized pass finds unchanged); a command takes exactly those.
- `native_x_renderer_thread` (on by default, read at startup). Off, every command runs as soon as it is
  captured, on the game's thread. Without patch 0006 the renderer stays on the game's thread.

**Why:**
- GRAW, a mission from Gustavo's save at 120 Hz: 79 FPS with the GPU mostly idle. The game's render thread was
  93 % busy, 11.6 ms a frame: 4.2 ms of the game's own code, about 7 ms of NativeXRenderer inside its calls,
  spread over 1800-2000 draws with no single hot spot. The Xenos path, whose work runs on the command
  processor's thread, ran the same spot at 118 FPS. Per-draw cuts on the game's thread were estimated at
  90-95 FPS.
- **Reading guest memory later is safe because of the gate.** The game reuses vertex buffers, index buffers,
  textures and command buffer memory only after the GPU has signalled that it is done: a resource carries the
  fence of its last use (stamped when it is unbound), segments end with fences, swaps and callbacks are
  interrupts. With the gate the game never sees the GPU further along than the renderer's thread. A resource is
  stamped by a later call than the draws that used it, so its fence comes after them. The one gap: a segment
  kicked from inside a call, whose packets the command processor can reach before that call is queued; only
  command buffer memory is at risk there, which is why inline draws' data is copied.
- **Deadlocks.** The renderer's thread waits only for the queue and the GPU, never for a game thread or the
  command processor; the command processor holds no lock while it waits; the CPU's own register writes (MMIO)
  do not reach the gate.

**Results:**
- GRAW, the same mission (RTSS's frame rate, 1280×720 window): 120 Hz: 79 → 117 FPS. 144 Hz: 119 FPS with the
  first version, 128 once programs were shared and the registers taken in one pass, 131 with the vectorized
  comparison. The training mission at 120 Hz: 120 FPS (8.33 ms), 112 with the thread off.
- At 144 Hz the game's thread is the limit again (92 % busy, the tracker 18 % of it); the renderer's thread is
  about 60 % busy (the rest is its spin while the queue is empty). The command processor waits about 1.2 ms a
  frame at its signals in that mission; in POTCAWE's dock at 60 FPS, never.
- The images as before: GRAW's training, the mission, the Cross-Com video, the menus, the team screen;
  POTCAWE's dock with SMAA and with DLAA (applied every frame, object motion as before). No inline draw's data
  needed restoring in these runs. The game exits cleanly.
- Not a hang: loads that stopped on a black screen at 120-144 Hz were the SDK's keyboard dialog (a new game's
  save name, `XamShowKeyboardUI`), which the test's keys opened at those frame rates and which NativeXRenderer
  does not show (the SDK's ImGui overlays, plan "Later"). The same happened before this decision.
- GRAWRecomp `local_docs/11-cpu-120fps.md`.

**Rejected:**
- Copying everything a draw reads from guest memory on the game's thread: the texture cache and the primitive
  processor read guest memory inside the SDK's code, and the gate makes it unnecessary.
- Holding the command processor until the game's call in progress returns, to close the kicked-segment gap: that
  call can itself be waiting for the command processor (ring space).

**Revisit if:** a game reads GPU results back with the CPU without a signal in between, or polls GPU progress
other than through the command processor's signals.

## D72 — Less of the renderer's work on the game's render thread: the tracker skips what it would not use (2026-10-17, Gustavo)

**Decision:** on the game's render thread, after D71:
- A call that is not a draw and wrote nothing into the command buffer leaves the tracker at once: no scan, no
  Apply, no tracked call (`StateTracker::set_skip_empty_calls`, on unless the observer verifies). The dirty
  masks are read for draws and inline begins only; the renderer keeps the arguments of the calls it acts on only.
- A draw's scan leaves out its float, bool and loop constant writes: the tracker takes those from the shadow
  anyway (float constants by dirty group, bool and loop all forty).
- Fetch constants are copied from the shadow for the slots the draw's flush uploads (dirty when the call
  started), not all 32; the others keep what the GPU has. A slot the flush dirties as it goes is in the draw's
  packets.
- The tracker notes its changes as runs of registers (a changed block of the shadow is one run), and a command
  carries them as runs, copied in bulk on both sides.
- Programs: kept by the tracker for its lifetime (one replaced by the same hash loaded otherwise too) and passed
  to the renderer's thread as plain pointers; a direct-mapped cache of 256 finds most loads before the map; a
  load read when its call returns is hashed where it is in guest memory instead of being copied (a load read
  when a segment ends is still copied, D21).
- The queue's two positions sit on cache lines of their own; the producer reads the consumer's position only
  when its last reading says the queue is full.

**Why:**
- At 240 Hz GRAW's mission ran at 133.5 FPS: the game's render thread was the limit again, 2.1 ms of its 7.5 ms
  frame in the tracker and the capture, and 1.3 ms waiting for the game's main thread.
- A trace of one frame: 1,800 draws and about 17,500 setters (SetVertexShaderConstantF 5,500, SetTexture 3,700,
  the shader, declaration, stream and index setters); a count over 600 frames: those setters never wrote a
  packet (0 of millions), only the shadow.
- Counted per draw: 203 packet writes, 147 of them float constants and 18 bool and loop constants, which Apply
  skipped (float) or overwrote with the shadow (bool, loop); 9.4 dirty constant groups, 1.6 shader loads of
  245 dwords; 45 registers changed. Timed with the TSC: Apply's constants 37 %, shader loads 28 %, the shadow's
  render state and fetch constants 17 %, packet writes 15 %.

**Results:**
- GRAW's mission at 240 Hz (RTSS): 133.5 -> 135.3 (setters) -> 143.2 (constant writes skipped; the fetch slots
  alone changed nothing measurable) -> 146 (runs, plain program pointers, the queue) -> 147.7 FPS (program cache,
  no microcode copies). The training mission: 214 FPS.
- The observer: 100 % of GRAW's draws in the mission (5,178 over three frames), fetch slots and programs
  included, after each step; POTCAWE's dock 100 % (3,384), and its image as before with the fast path on.
- The game's render thread at 147.7 FPS: its own code 3.75 ms, NativeXRenderer about 1.4 ms, 1.4 ms waiting for
  the main thread (whose own code is about 4.2 ms). Further gains are in the game's code.

**Revisit if:** a game's setters write packets (they then take the normal path: the fast path only skips calls
that wrote nothing), or the observer finds fetch constants that differ from the shadow in a slot that was not
dirty.

## D73 — The register file on the renderer's thread: the tracker records what changed (2026-10-17, Gustavo)

**Decision:**
- The tracker no longer keeps a register file of its own for the renderer. For each call it records the
  registers that change (`RegisterRecord`): runs of the shadow's dwords, big-endian as the guest stores them,
  and runs of packet values. It finds them by comparing the shadow with a big-endian image of the file the
  records build, in blocks of 16 dwords, and skips packet writes that change nothing.
- It resolves the fetch constant overrides (D59's rule 3) itself, so a record holds only those two kinds of
  runs.
- A draw or a Present takes the record into its command. The renderer's thread writes it into its own file,
  byte-swapping the shadow's runs.
- The tracker keeps a file of its own only for the observer (`set_apply_locally`). Without a renderer it drops
  the records (`set_keep_record`).
- An inline draw's stream (fetch 95) and its initiator and DMA base come from the draw's own scan, where rule 11
  puts them, not from a register file.

**Why:**
- After D72 and the SDK's counters (151.4 FPS at 240 Hz in GRAW's mission), the game's render thread spent
  about 1.15 ms a frame in the tracker's Apply and the capture of the changed registers: two passes over the
  same data, one on each side of the command.

**Results:**
- The observer: 100 % of GRAW's draws in the mission (5,311 over three frames) and in training (4,080); POTCAWE's
  dock 100 % (3,357, with 66 DrawVerticesUP); POTCAWE's image as with D72.
- A temporary comparison over 276,480 of GRAW's inline draws: the values from the scan and from the register
  file never differed.
- GRAW at 240 Hz: training 216 -> 220.7 FPS; the mission 151.4 -> 151.4-153.0 FPS.
- The game's render thread does about 0.25 ms less: the capture is lighter. Comparing the shadow still costs
  what Apply cost (0.76 ms), because the shadow can only be read on the game's thread. A first version that
  copied the shadow into the record without comparing cost the same: it wrote about 2 KB a draw into memory
  that was not in the cache.
- The mission gains little because its frame is no longer set by the render thread alone. The main thread waits
  for five events of its worker threads, the render thread among them (`sub_82244EB8`, 2.4 ms a frame), while
  the render thread waits for the main thread's frame (1.4 ms). Only the render thread's work while the main
  thread waits shortens the frame.

**Revisit if:** a consumer needs the register file on the game's thread (it can use `set_apply_locally`, as the
observer does), or a renderer wants to drop a record (the image assumes every record reaches the file in
order).

## D74 — A resolved texture outlives an allocation or release of its memory; unused textures are freed by time (2026-10-18, Gustavo)

**Decision:**
- The resolved textures watch guest memory through the memory's physical invalidation callbacks instead of the
  mirror's global watch. The callback says whether the range is a CPU write (the page written, widened) or an exact
  range (an allocation, a release, a protection change, a file read). A CPU write invalidates the texture as before
  (D70). For an exact range the guest bytes the texture covers are hashed and compared with the hash taken at the
  resolve: unchanged, the texture is kept and its memory watched again; changed, invalidated. The log counts both.
- A fetch that finds no valid resolved texture where a resolve once wrote is counted ("fetches in 600 frames read
  guest memory a resolve had written") and the first such addresses are logged with when and why the texture went.
- Unused render targets and resolved textures are freed after a minute of wall time, not after 3600 frames.

**Why:**
- GRAW's checkpoint reload releases and re-allocates the memory of its Cross-Com pictures (five 128x128 textures)
  without writing it; D70 invalidated them at the allocation, so later draws read guest memory that holds the data
  of a resolve the host never wrote (the console keeps the resolve in memory across an allocation). The 3600-frame
  limit was half a minute at 120 FPS, so a texture resolved once and sampled for a minute was freed as well.
- Investigated for the scene fading to black at the Angel (GRAW, local_docs/13); not its cause, the bug reproduced
  with this in place. Right on its own.

**Results:** GRAW: at a reload the five Cross-Com textures are kept ("kept: ... allocated, released or reprotected with
its data unchanged"); the observer 100 % (5,311 draws in the mission; 2,259 in the Angel run). Cost: a hash of the
texture's guest bytes at each resolve that starts watching it, and at each exact-range event.

**Revisit if:** a game allocates a resolve destination and expects it cleared without writing it (the hash then
matches a stale resolve: the game would see the old image where the console would see the allocator's contents).

## D75 — The SDK's dialogs are drawn over NativeXRenderer's frames (2026-10-18, Gustavo)

**Decision:**
- NativeXRenderer hands the runtime's ImGuiDrawer a drawer of its own (`Overlay`, `src/overlay.cpp`), with no
  presenter: the SDK's "bring your own renderer" contract (`ui::ImmediateDrawer`, `ui::AppUIDrawContext`).
- Dear ImGui stays on the UI thread, where the window's input reaches it, as in the SDK's own mode: once per
  presented frame the renderer's thread asks the UI thread (fire and forget, one request in flight) to run
  `ImGuiDrawer::Draw` if a dialog is open. The drawer only records what Dear ImGui draws: vertices, indices, draws,
  scissors and textures (the font atlas, kept as data until the renderer's thread makes its texture).
- The renderer's thread draws the latest record over the back buffer before Present: one pipeline (position,
  texture coordinates, RGBA8 color; alpha blending; scissor), dynamic vertex and index buffers, a texture per
  ImmediateTexture. While no dialog is open nothing is drawn.

**Why:**
- The SDK draws its dialogs (XamShowKeyboardUI's keyboard, message boxes, its F3 overlay) with ImGui from its
  presenter, which NativeXRenderer detaches from the window (plan, Phase 2 step 4). A game that asks for text
  waited for an answer on a black screen: GRAW's new game asks for the save's name (D71's results).
- Calling ImGuiDrawer::Draw on the renderer's thread would race the UI thread's input events on Dear ImGui's
  state; recording on the UI thread and drawing the record keeps every ImGui call where the SDK makes them.

**Results:** GRAW, new game: the save-name dialog shows at the same size and place as on the Xenos path; the
name typed in it is taken (Enter) and the game goes on to the difficulty menu. No cost without a dialog: one
deferred call on the UI thread per frame.

**Revisit if:** a game stops presenting while a dialog is open (the record would not be drawn: the renderer would
have to present by itself), or a dialog draws with textures of its own in formats other than RGBA8.

## D76 — Guest memory read by its virtual address goes through the SDK's translation (2026-10-18, Gustavo)

**Decision:** every read of guest memory by a virtual address (`Load32`, `Load64`, `LoadFloat`, a shader load's
microcode, the device's shadow) goes through `GuestPointer` (`src/internal.h`), the SDK's `rex::memory::GuestPtr`:
above 0xE0000000 it adds the 4 KB the SDK's mapping takes off on hosts whose allocation granularity is coarser
than 4 KB (Windows). Physical addresses read through the 0xA0000000 view or `TranslatePhysical` are unchanged.

**Why:**
- The SDK maps the 0xE0000000 view (physical memory in 4 KB pages) 4 KB lower on Windows, and the recompiled
  code adds the 4 KB back on every access (`PhysicalHostOffset`). `base + address` read 4 KB away from what the
  game wrote there.
- Kameo (XDK 2099) writes its command buffer at 0xEF0Axxxx. The first observer runs found 0 % of the draws
  matching: the tracker read other packets than the calls wrote (SetViewport's 16 bytes held the gamma table
  that Present writes further on), so the shader loads, the SQ registers and the fetch constants were wrong, and
  3,199 of a level frame's 22,905 calls were read incompletely. POTCAWE and GRAW keep their command buffers and
  devices below 0xE0000000, which is why it never showed.

**Results:** Kameo, the same four spots: 100 % of the draws a hooked call writes match (the title video 174, the
menu 329, a video 1, the first level 3,035); no call read incompletely in the level. Addresses below 0xE0000000
read as before: with this decision and D77, GRAW's training mission (3,843 draws over three frames at 120 Hz) and
POTCAWE's dock (1,137 draws) stay at 100 %, their images unchanged.

**Revisit if:** the SDK changes how it maps the 0xE0000000 view.

## D77 — A game that inlines EndVertices: the begin is the draw, handed on when the game commits it (2026-10-18, Gustavo)

**Decision:**
- `DeviceLayout::inline_draw_end`: where BeginVertices and BeginIndexedVertices save the last dword of their packets
  (Kameo: +13620), for a game whose engine commits inline draws itself. kUnknown otherwise (POTCAWE, GRAW).
- With it, a top-level BeginVertices is tracked as the draw when it returns: its scan reaches the fetch constant
  and draw packets it left after the write pointer, and the state is the one its flush uploaded. The tracked call
  is kept, not handed on.
- Before the next device call, if the write pointer is at that saved dword, the game has committed the draw: the
  kept call goes to the observer and the renderer as an EndVertices (the renderer copies the vertices then, filled
  by the game). If the write pointer is elsewhere, the game dropped it: it is discarded (logged once).

**Why:**
- Kameo's XDK library was linked with link-time code generation: EndVertices does not exist as a function, the
  engine does `[dev+0] = [dev+13620]` itself at 43 sites. Its particles and sprites are BeginVertices quads: about
  860 a frame in the first level. Without this, the observer counted them as draw packets written outside any
  hooked call, and the renderer would not draw them.
- A first version tracked the EndVertices at the next device call: 8 of the level's inline draws then took render
  states the engine had already set for the next draw (RB_DEPTHCONTROL, RB_BLENDCONTROL, PA_CL_CLIP_CNTL), through
  render-state handlers that only write the shadow. The begin's own state is the one the GPU draws with.
- One rule in the core instead of 43 `[[midasm_hook]]`s in the game: any game with the same inlining needs only the
  offset. The engine fills and commits an inline draw before its next D3D call (Kameo: in the same function).

**Results:** Kameo's observer at 100 % with the inline draws: the title video 177 of 177 (3 inline), the menu 330 of
330 (1), a video 3 of 3 (2), the first level 3,851 of 3,851 (818 inline); none dropped. GRAW and POTCAWE leave the
field unset: unchanged (D76's results).

**Revisit if:** a game makes another D3D call between a BeginVertices and its inlined end (the draw is then
dropped and the log says so), or inlines EndVertices without the begin saving its end in the device.

## D78 — A clear given a rectangle that covers its target clears it whole (2026-10-18, Gustavo)

**Decision:** a Clear/ClearF with a rectangle is no longer skipped as a whole: the rectangle is captured with the
call, and each target it covers (x1 ≤ 0, y1 ≤ 0, x2 ≥ the guest width, y2 ≥ the guest height) is cleared as before.
A target the rectangle covers only in part is still skipped, logged once with the rectangle and the size.

**Why:** Kameo clears its scene with `Clear(1, &rect, 0x3F, ...)`, the rectangle being the target's own 1280×720. The
skipped clear left the scene's depth as the previous frame left it: in the first level the geometry failed the depth
test and only a sky gradient drawn without it showed, and every occlusion query got 0 samples. POTCAWE and GRAW pass
no rectangle, which is why it had not shown.

**Results:** Kameo's first level draws as on the Xenos path (no more "partial clear" in the log). None of GRAW's 415
and POTCAWE's 328 logs had a partial clear; with this decision and D79 GRAW's training mission (3,843 draws over three
frames) and POTCAWE's dock (1,128) stay at 100 %, their images unchanged.

**Revisit if:** a game clears part of a target (a viewport, a split screen): that needs a scissored clear or a quad.

## D79 — Inside an occlusion query, a depth test that always passes is kept (2026-10-18, Gustavo)

**Decision:** while an occlusion query is open, a draw whose depth test always passes without writing keeps that test
(`DrawPath::SetCountingSamples`), instead of the SDK's normalization that turns it off: the draw then rasterizes into
the depth target, writing nothing, and the host query counts its samples.

**Why:** the SDK's `GetNormalizedDepthControl` turns off a depth test that is ALWAYS without writes (to skip depth
target work), and a draw that then writes no color has no target and is skipped. The guest GPU rasterizes it and
counts its samples. Kameo's light glows measure how much of each glow is on screen with such a quad (no color writes,
depth always, alpha test off, inside a query) and draw the glow by the count: skipped, every count was 0 and the
glows never showed (the Xenos path shows the same: its backends skip the draw the same way).

**Results:** Kameo's first level: the queries count 25-49 samples per glow at the traced spot and the torches glow;
without the queries' answers (`occlusion_query_enable = false`, every glow "visible") the same glows show, plus a few
that the measured counts hide. POTCAWE's sun flare queries count what they counted before at its dock; GRAW measures
none (`occlusion_query_enable = false`).

**Revisit if:** a game counts samples of draws with depth disabled altogether (the guest GPU may count those too; here
only a test that is enabled and always passes is kept).

## D80 — A resolve's clears are done after its copy, over the rectangle it copied; partial clears are filled (2026-10-20, Gustavo)

**Decision:**
- **Resolve.** Flags `0x100` (clear the render target) and `0x200` (clear the depth and stencil) are done after the
  copy, as the EDRAM copy does them (the SDK's resolve, `GetResolveInfo`): the color one on the resolved color
  target with the call's color (a `D3DVECTOR4` at `r10`); the depth one on the depth-stencil surface bound (the
  source itself for a depth resolve) with Z (`f1`) and the stencil in `RB_DEPTH_CLEAR` bits 0-7, which the call
  writes (its own stencil argument is on the stack). Both cover the rectangle the copy read, narrowed to what it
  copied, and run even when the copy itself is skipped.
- **Clears of a part of a target** are filled by the blitter (`Blitter::FillColor`, `FillDepthStencil`: a
  triangle over the rectangle, a constant color or `gl_FragDepth` and the stencil reference), since DiligentCore's
  clears take no rectangle. A rectangle that covers the target is still a DiligentCore clear (D78). This closes
  "partial clears", a known limit since Phase 2.

**Why:** Kameo's flying dragons in the distance are billboards of a 128×128 picture the game renders every frame:
twelve dragons at different moments of their wing beat drawn one after the other into the same 512×512 target,
each resolved into its own texture with `0x300`, which clears the target for the next one. The clears were
ignored, each picture held every dragon drawn before it, and the billboards showed the wings in two or three
positions at once (Gustavo). GRAW's tiled pass resolves each tile with `0x300` too; since D60 draws the pass once,
those clears had not mattered there. No game had logged a partial clear, apart from Kameo before D78.

**Results:** Kameo's first cinematic and the spot after it: one dragon per billboard, as on the Xenos path (RenderDoc:
the impostor target held three wing poses before). GRAW's training mission: the observer 3,843/3,843, the same image
(its tiles' clears now run, on rectangles the copies have already read). POTCAWE's dock: 1,128/1,128, the same image.

**Revisit if:** a game clears part of a multisampled target (the fills are single-sampled: such a clear is skipped and
logged once), or depends on the region outside a resolve's copy being cleared as well.

## D81 — The draws' repeated look-ups and state cut: shaders cached by draw, viewport and scissor when they change (2026-10-20, Gustavo)

**Decision:** step 1 of the renderer thread's CPU work (plan, "The renderer thread's CPU time"):
- **Shaders.** The draw path keeps two direct-mapped caches of what it found: the analyzed shader by its
  microcode's hash and type (256 entries, before `ShaderCache::Get`'s map) and the host shader of a translation by
  the shader and its modification (1,024 entries, before the shader's translations and `GetHostShader`'s map).
  Only shaders found are kept, and host shaders only once ready, which they stay.
- **Viewport and scissor** are set when they differ from the last draw's: DiligentCore records both into the
  command buffer on every call. The cache is dropped where the render targets are (D30's `TargetsChanged`, and
  the draw path's own binds: DiligentCore resets the viewport with the targets).
- **The bool/loop constants** are uploaded when they differ from the last upload of the frame (a dynamic buffer
  keeps its data until the frame ends; object motion's draws upload their own and drop the cache). The fetch
  constants are uploaded every draw: they change with nearly every draw, and comparing them cost more (1.3 % of
  the thread) than the upload it saved.
- Not done: stencil reference, blend factors and an unchanged pipeline, which DiligentCore already filters; the index
  buffer, which shares the guest memory buffer with the vertex fetches (its state transitions go with step 2).

**Why:** the profile of Kameo's first level (plan, the table "Group / Share / What"): the shader look-ups were about
4.5 % of the renderer thread's busy time, `SetViewports` 1.7 %, the constants' maps 1.9 %.

**Results:** Kameo's first level, the same spot and protocol as the profile: the renderer's CPU 9.6–9.9 → 9.2–9.4 ms a
frame (about −5 %); in the profile the shader look-ups 4.5 → 0.7 % and `SetViewports` gone. GRAW's training mission
at 120 FPS: 3.05 → 2.90 ms, the observer 3,843/3,843, the same image. POTCAWE's dock: 4.5 → 4.4 ms, 1,128/1,128, the
same image.

## D82 — Textures and samplers set when they change, shader resources committed when something changed (2026-10-20, Gustavo)

**Decision:** step 2 of the renderer thread's CPU work (plan, "The renderer thread's CPU time"):
- **Bindings.** Each pipeline keeps, next to its texture and sampler variables, the object each one holds; a draw
  sets a variable only when its view or sampler is another object. The variable holds a reference to its object, so
  the pointer cannot belong to another object while it is kept. D55's release forgets them with the variables.
- **The commit.** The draw path keeps the pipeline whose shader resources the last draw committed, and a draw with
  that pipeline draws with them when none of its variables changed and every texture they hold is readable in the
  state it is in (DiligentCore's own test: shader resource, depth read for depth textures; a texture loaded, copied or
  drawn into since is not). The kept pipeline is dropped where the render targets change (D30's `TargetsChanged`,
  and the draw path's own binds, after which a commit transitions what was a target): all the other work on the
  context (clears, resolves, the blitter, antialiasing, DLSS and its object motion draws, Present and its submission)
  goes through there. The guest texture loads inside a draw's view look-ups are compute work, on the other bind point,
  and leave their textures' states for the test above.
- The log's draw line counts both: texture and sampler bindings set, and the draws that committed.

**Why:** after D81 the binding was the largest part of the renderer thread in Kameo's first level: `ShaderVariable::Set`
11.3 % (AddRef, Release and DiligentCore's cache for every texture and sampler of every draw) and
`CommitShaderResources` 12.9 % (a descriptor set allocated and written per draw). Measured with a temporary probe at
the same spot: of about 57,000 variables a frame 1.7 % change, and three draws in four have the last draw's pipeline
and resources.

**Results:** Kameo's first level: the renderer's CPU 9.2–9.4 → 7.2–7.5 ms a frame (about −20 %; 9.6–9.9 before D81),
about 950 bindings set and 1,080 commits for 4,100 draws a frame; in the profile `ShaderVariable::Set` 11.3 → 0.2 %,
`CommitShaderResources` 12.9 → 4.3 %, the readable test 1.0 %. The image, the title and level videos, the menu and the
cinematic's dragons unchanged. GRAW's training mission at 120 FPS: 2.78–2.98 → 2.82–2.89 ms (865 of 1,300 draws
commit), the observer 3,843/3,843, the same image. POTCAWE's dock: 4.35–4.40 → 4.28–4.33 ms, 1,128/1,128, the same
image.

**Revisit if:** something binds a pipeline or commits shader resources on the graphics bind point, or submits the
command buffer, without going through `TargetsChanged`; or a texture is written without a state change DiligentCore
tracks (a state set with `STATE_TRANSITION_FLAG_UPDATE_STATE` off).

## D83 — A draw's texture look-ups kept per fetch constant; only changed fetch constants recompute their binding (2026-10-20, Gustavo)

**Decision:** step 3 of the renderer thread's CPU work (plan, "The renderer thread's CPU time"):
- **The SDK's bindings.** `GuestTextures::RequestTextures` marked all 32 fetch constants written before every draw,
  so the SDK's texture cache computed every used binding again (`BindingInfoFromFetchConstant`, the host format's
  swizzle) in every draw. It now keeps the fetch constants as it last saw them and marks written only the used
  ones whose contents differ, as the SDK's command processors mark the ones the guest writes. A binding of
  unchanged contents is the one computed from them; the SDK still resets all bindings at every frame and when a
  texture became outdated or was destroyed.
- **The look-ups.** `Textures` keeps, per fetch constant, what `View` (per binding dimension and sign),
  `HostSwizzle` and `IsResolved` found for its six words, while a generation is unchanged. The generation changes
  when the resolved textures may answer differently (`ResolvedTextures::generation`: a texture created, resolved to,
  invalidated or freed), when the guest texture bindings change (`GuestTextures::generation`: the SDK's
  `UpdateTextureBindingsImpl`, called for every binding computed again or reset, and every frame), and at every
  frame, so that the textures found are marked used again in the new frame (the SDK's LRU, D52). The resolved
  textures' queued invalidations are applied before each look-up as before (`ApplyInvalidations`, now public).
  The look-ups' side effects stay per call: the stats counters, and `NoteGuestRead` for a fetch that reads guest
  memory a resolve had written.
- **Samplers.** A direct-mapped cache of 128 entries before the derivation and the map: keyed by everything the
  derivation reads (the fetch constant's words, the binding's slot and filters, the bias, the anisotropic
  override). Samplers are never freed.

**Why:** after D82, in Kameo's first level: the SDK's `RequestTextures` 7.6 % of the renderer thread's busy time,
`Textures::View` 3.5 %, `Sampler` 3.5 %, `HostSwizzle` 2.4 %, with `ResolvedTextures::FindNewest` (a walk of a map)
twice or three times per texture. About 38,000 views and as many samplers a frame, nearly all the same as the
draw before.

**Results:** Kameo's first level: the renderer's CPU 7.2–7.5 → 6.6–6.7 ms a frame; the texture look-ups about 17 →
9 % of the busy time. The log's texture counts unchanged; the level, the videos (the same frames as the Xenos path),
the menu, the cinematic's dragons, and at a render scale of 2 (where `IsResolved` is used) the level and the menu as
on the Xenos path at 2x. GRAW and POTCAWE: with D84 below.

**Revisit if:** a guest texture's view changes without a binding change (a texture recreated in place), or a
resolved texture changes without `Touch`, `GetOrCreate`, an invalidation or `EndFrame`.

## D84 — The SDK's shared memory keeps its merged ranges between calls (SDK patch 0007, 2026-10-20, Gustavo)

**Decision:** step 4. `SharedMemory::RequestRanges` sorted and merged the requested ranges in a local `std::vector`
reserved on entry: a heap allocation and a free in every call, also for `RequestRange`'s single range, which the
draw path and the SDK's primitive processor make for every vertex and index buffer of every draw. SDK patch
`sdk_patches/0007` makes it a member, cleared on entry and kept with its capacity, as `upload_ranges_` already was
(`RequestRanges` was not reentrant before either). It changes the SDK's own backends the same way.

**Why:** in Kameo's first level the allocation, the free and the vector's growth were about 5 % of the renderer
thread's busy time (`RtlAllocateHeap`, `RtlFreeHeap`, `vector::_Reallocate` under `RequestRanges`), the largest part
of its 13 %.

**Results:** Kameo's first level: 6.6–6.7 → 6.4–6.5 ms a frame; `RequestRanges` 13.2 → 6.9 % of the busy time (the
rest: the lock, and `VirtualProtect` for the pages uploaded, which the watches need). The four steps together:
9.6–9.9 → 6.4–6.5 ms (about −33 %). With D83 and D84: GRAW's training mission at 120 FPS 2.8–2.9 → 2.75–2.85 ms,
the observer 3,843/3,843, the same image; POTCAWE's dock 4.3 → 3.9–4.0 ms, 1,128/1,128, the same image.

## D85 — Signed texture bindings read the bits as signed (2026-10-20, Gustavo)

**Decision:** the translated shaders declare two bindings for a texture whose sign is decided at run time,
`xe_textureN_..._u` and `..._s`, sample both, and pick per component from the fetch constant's signs (the system
constant `texture_swizzled_signs`). The signed binding expects the texture's bits read as signed normalized values.
NativeXRenderer bound the unsigned view to both (`GuestTextures::GetView` ignored `is_signed`). Now:
- the host formats whose bits the SDK's Vulkan backend reads as signed through a view of another format (8- and
  16-bit normalized: `k_8`, `k_8_A`, `k_8_8`, `k_8_8_8_8` and its `_AS_16_16_16_16`, `k_16`, `k_16_16`,
  `k_16_16_16_16`) name their signed format in the format table;
- such textures are created with the typeless format (DiligentCore creates the image with a mutable format and its
  default view of the unsigned format) and get a second view of the signed format;
- a signed binding gets that view; formats without one (block compressed, float, the 10_11_11 family the SDK loads
  separately as signed) keep the unsigned view, as before.

**Why:** Kameo's menu background and loading screen are purple smoke and flames distorted by a 128×128 `k_8_8`
signed map (Gustavo: "las flamas se ven raras, como cortadas"). Read unsigned, a small negative value (0xFF, −0.01)
became +1.0: the distortion jumped at every zero crossing of the map, which cut the smoke into hard-edged shapes
and kept it from fading at the top of the loading screen. Found in a RenderDoc capture of the menu: the draws sample
the same image through `xe_texture0_2d_u` and `xe_texture0_2d_s`.

**Results:** Kameo's menu and loading screen draw the smoke and flames as on the Xenos path (KameoRecomp
`local_docs/15`). GRAW's training mission: the observer 3,847/3,847, the same image; POTCAWE's dock: 1,119/1,119, the
same image (no signed texture at their spots changed visibly).

**Revisit if:** a signed fetch of a format without a signed view shows wrong values (the 10_11_11 family needs the
SDK's separate signed load), or a resolved texture is fetched as signed (resolved textures have one view).

## D86 — Frames shown with the guest's swaps (an option; SDK patch 0008, 2026-10-20, Gustavo)

**Decision:** `native_x_renderer_present_with_swaps` (off by default for now, read at startup, needs the renderer's
own thread): a frame is shown when the command processor executes the game's swap for it, as the SDK's presenters
show it, instead of when the game calls Present.
- SDK patch `sdk_patches/0008`: `IGraphicsSystem::NoteSwapIssued` (VdSwap, after it wrote the `PM4_XE_SWAP`),
  `GetSwapCounts(issued, executed)` and `SetSwapObserver` (called on the GPU thread after every executed swap).
- The Present command carries the number of its swap: the swaps issued when the game's Present returned (it has
  called VdSwap by then; a game that calls it later gets the next number all the same).
- The renderer composes the frame at the Present command, as before, but into a frame image of the back buffer's
  size and format (three, round robin), and queues it. A queued frame is shown (copied into the back buffer, then
  the swap chain's Present) once its swap has executed: checked before every command, and when the queue wakes the
  renderer's thread (the swap observer wakes it; while it waits for commands it also looks every 5 ms). When several
  are ready the newest is shown; with two queued, the next Present shows the oldest first; one queued 100 ms is shown.
- Nothing waits on the renderer's thread: the command processor executes a swap only after the renderer has run
  the commands captured so far (the swap passes D71's signal gate), and those may include the next frame's Present.
- Presenting now ends DiligentCore's command buffer between the next frame's commands: the draw path's state caches
  are dropped then (`TargetsChanged`).

**Why:** Kameo's Bink videos showed isolated 30-50 ms frames on NativeXRenderer and none on the Xenos path. The game's
Bink code locks a video texture and now and then waits a vertical blank for the GPU (~16 ms, 30 times in 30 s at the
title screen, on both renderers: a probe in the game's GPU wait hook with its host stack). Its CPU runs a frame ahead of
the command processor, whose swaps kept an exact 60 Hz during those seconds (a probe in `PM4_XE_SWAP`). The SDK shows
frames at the swaps, so the delay never reaches the screen; NativeXRenderer showed them at the CPU's Present, 30 ms
apart. A first version with one composed frame waiting in the back buffer did not help: every frame's swap executed
after the next frame's Present had run (the CPU a full frame ahead), so every frame was shown early.

**Results:** Kameo's title screen video, 30 s of RTSS frame times: off 9-11 frames over 20 ms (max 33-35 ms), on none
(max 18.4 ms, as the Xenos path: 18.9); every frame shown at its swap, none early, late or skipped. The first level at
60 FPS: max 18.4 ms; at 120 FPS (`video_mode_refresh_rate=120`): max 9.8 ms; Gustavo's options in fullscreen 3440×1440
(ultrawide, fill, SMAA): every frame at its swap. The images as the Xenos path's, the videos' frames now at the same
moments. The cost: a frame is shown up to one frame later than at the Present call, as on the Xenos path. GRAW and
POTCAWE: with the option off, the observer at 3,843/3,843 and 1,128/1,128 and the same images; on (GRAW's training
mission at 120 FPS, POTCAWE's dock at 60), every frame at its swap, the same images.

**Revisit if:** a game presents without VdSwap, or calls VdSwap more than once per Present (the numbering would drift:
frames shown late or by the timeout); the option would then need the numbering per game.

## D87 — D79 withdrawn: a query's draws that write nothing are not counted (2026-10-20, Gustavo)

**Decision:** D79 is undone. Inside an occlusion query, a draw whose depth test always passes without writing is
normalized like any other (the SDK turns the test off); if it writes no color it has no target, is skipped, and
the query counts none of its samples. `DrawPath::SetCountingSamples` is gone. The Xenos path's equivalent, shared
SDK patch 0026, is removed from the series (25 shared patches again).

**Why:** Kameo has two features measured that way, each with two queries over one quad: a "total" query whose quad
writes no color and no depth and is never occluded, and a "visible" one with the scene's depth test. The light glows
(`sub_823550D8`) take their opacity as visible / total × the light's own (depth ALWAYS for the total); the sun
(`sub_8213C240`) takes visible / (0.75 × total) for a lens flare and a dimming of the sunlight (depth disabled
altogether for the total). With D79 and patch 0026 the glows' totals were counted, and in the Enchanted Kingdom's
forest the glows around the sun washed the image out and flickered on both renderers, at any scale and frame rate
(Gustavo). In one session at that spot on the Xenos path, with a temporary switch for 0026's behavior: counted, the
image was overexposed and flickering; not counted, both gone. Counting the sun's total (depth disabled, a temporary
switch too) changed nothing visible there. Xenia Canary's notes for its occlusion queries name Kameo (4D5307D2) among
the titles that look more correct when unresolved results stay zero, and Gustavo recalls the original's torches
without a glow around the flame. The guest GPU evidently counts no samples for
these quads (or the game expects none): both features stay off, as they were before D79.

**Results:** Kameo's first level: the torches without the glow quads (as before D79), the image otherwise unchanged
on both renderers, no new warnings. Gustavo at the forest spot with NativeXRenderer without D79: right, no haze and no
flicker. The sun's total stays 0 (the sun's dimming and flare are not computed, as before);
measured in the first level with a temporary probe of the game's variables: total 0, visible about 4,200 samples.

**Revisit if:** a game's effect measured by a quad that writes nothing is known to show on the console (a capture or
footage of the hardware); then count such draws for that game only.

## D88 — An inline draw followed by the game's own packets is still drawn (2026-10-21, Gustavo)

**Decision:** for a game that commits its inline draws itself (D77), the draw is handed on when the device's write
pointer is at the draw's end, as before, or past it within the same segment (less than 64 KB further): the game
committed the draw and then wrote packets of its own before the next device call. Those packets are not read. A
write pointer short of the end, or in another segment, still drops the draw (the warning of D77).

**Why:** in Kameo, D77's warning showed in two of Gustavo's sessions, once each (it logs once). A temporary probe
logged every case in a session of his: 20 in about 13 minutes, all from the same engine function (`sub_82351F88`, a
quad-list effect begun at `0x82351FE0`), the write pointer always 88 bytes past the end, the next call a SetTexture.
The 88 bytes are a cache coherency sequence the game's inlined D3D code writes right after committing the draw for a
buffer the CPU has just written (`COHER_STATUS_HOST` 0x03000000, `COHER_SIZE_HOST` / `COHER_BASE_HOST` with a 16-20 KB
range such as `0x1F987000`, then a `WAIT_REG_MEM` on the status). The draw was committed; NativeXRenderer dropped it,
and the effect was missing for that frame.

**Results:** Kameo, Gustavo playing the same places and missions again: the case logged once ("followed by 88 bytes
... drawn (D88)"), no draw dropped, no new warning. GRAW and POTCAWE have no such sequence at their spots.

**Revisit if:** a game writes packets that change the state of the next draw (register writes, constants) between
committing an inline draw and its next device call: they would have to be read then.

## D89 — The library's SDK changes are commits of the SDK's `recomp` branch (2026-10-21, Gustavo)

**Decision:** NativeXRenderer's eight changes to the ReXGlue SDK are no longer patch files in `sdk_patches/`: they are
commits 26-33 of the `recomp` branch of [ElGuslml/rexglue-sdk](https://github.com/ElGuslml/rexglue-sdk/tree/recomp),
after the 25 fixes the games share, on top of the official v0.10.0. The games use that branch as their
`thirdparty/rexglue-sdk` submodule; nothing is applied at configure time. The branch's `RECOMP.md` lists every
change. A library change that needs the SDK changed is a commit on `recomp` too, and the games move both submodules.

**Why:** with three games every SDK change was made by hand in several places: the shared series copied byte for
byte into each game's `patches/`, the library's series applied after it, the submodule left modified, and a
`checkout -- .` and a new configure whenever a patch changed. Removing patch 0026 (D87) took a commit, a configure and
a test in each game. One branch is one place: a change is one commit, a new upstream release is one rebase, and the
submodule is a plain checkout. The library gives up working with the official SDK plus its own patches; no project
uses it that way (`git format-patch c94f5eb..recomp` gives patch files back if one does).

**Results:** the branch's tree is the one the games built and tested with (the 54 files the 33 patches touch are
identical). KameoRecomp, GRAWRecomp and POTCAWERecompVulkan on the branch: built from a clean submodule, the same
images, the observers at 100 % (KameoRecomp `local_docs/18`).

**Revisit if:** another project wants NativeXRenderer with a different SDK.

## D90 — The SDK's `recomp` branch on upstream's nightly of 2026-10-02 (2026-10-21, Gustavo)

**Decision:** the `recomp` branch of ElGuslml/rexglue-sdk is rebased from v0.10.0 onto upstream's `development` at
`bd833a2` (the nightly `nightly-20261002-bd833a2a`, v0.10.0 and 24 later commits). The library's eight changes
(26-33) apply without conflicts and unchanged, and the library's code needs no change. The branch as it was on
v0.10.0 is kept as the tag `recomp-v0.10.0`, so the games' older commits still build.

Two shared changes are different on the nightly:
- **Change 5 is dropped** (`fullscreen_borderless_window`). Upstream is borderless at the desktop mode by default, and
  its `fullscreen_exclusive` switches the display mode.
- **Change 34 (the quit prompt) uses upstream's UI input blockers.** Before, it blocked the controller with its own
  code.

**Why:** the next work builds on upstream's new input code: the launchers on SDL3, then remapping the controller. That
code has vibration, dead zones, UI input blockers, hotplug notices and the real device subtype. The nightly also brings:
- **Display:** a fullscreen that switches the display mode; the monitor, window size and fullscreen applied without a
  restart.
- **Codegen:** `stwcx.` as an atomic compare-and-swap, and sync fences.
- **Logging:** the log is configured through `LogConfig`.

Every upstream commit left out makes the next rebase larger.

**Results** (KameoRecomp `local_docs/21`):
- **Codegen:** in KameoRecomp, GRAWRecomp and POTCAWERecompVulkan the same function lists and switch tables, and the
  gap finder finds nothing new. Kameo's reads of uninitialized registers are the same 93 benign cases.
- **The test of every game:** all six runs OK, the observers at 100 %, the CPU and the images within the runs before
  it.
- **The quit prompt and fullscreen:** the quit prompt behaves as before in Kameo and POTCAWE. Borderless and
  exclusive fullscreen work with this library's swap chain and with the Xenos path.

**Revisit if:** the next upstream release or nightly conflicts with one of the library's changes: rebase again, the
same way.

## D91 — The SDK's dialogs make their textures with the drawer they are drawn with (2026-10-22, Gustavo)

**Decision:** no change in the library for the achievement icons. The SDK's `recomp` branch (changes 37 and 38) makes
every texture of its ImGui dialogs with the `ImGuiDrawer`'s current immediate drawer, which is this library's
`Overlay` while NativeXRenderer draws (D75):
- the font atlas, now made on Dear ImGui's request (`ImGuiBackendFlags_RendererHasTextures`): `Overlay::CreateTexture`
  is called again whenever the atlas changes (new glyphs or sizes), and the records keep the old texture alive until
  they are drawn;
- the achievement icons (`AchievementIconCache`), which were made with the SDK presenter's drawer: `Overlay::Draw`
  skipped them (it draws only the textures it made), so the toast and the F7 list had no icons with NativeXRenderer
  (seen in Kameo, 2026-10-22).

**Why:** the plan for the achievements (POTCAWERecompVulkan `local_docs/10`) expected the library to draw
`ImmediateTexture` images of the SDK's drawers on its swap chain. The textures it could not draw belonged to the
presenter's drawer, which NativeXRenderer detaches; making them with the drawer the dialogs are drawn with is the
SDK's contract for "bring your own renderer", and keeps the library free of the SDK's D3D12 and Vulkan objects.

**Results** (POTCAWERecompVulkan `local_docs/14`): the achievement toast and the F7 list with the game's icons and
Barlow text on NativeXRenderer and on the Xenos path; the quit prompt in the same style; the test of every game
(recomp-tools, 2026-10-22): all six runs OK, the observers at 100 % (Kameo 4,129/4,129, GRAW 1,292/1,292, POTCAWE
1,128/1,128), the images within their limits.

**Revisit if:** a dialog draws a texture made outside the `ImGuiDrawer` (for example a game's own `ImmediateDrawer`
calls): it would still be skipped.

## D92 — Shadow maps at a scale of their own: the launcher's shadow quality (2026-10-22, Gustavo)

**Decision:**
- **`native_x_renderer_shadow_scale`** (read at startup; 0 by default): when it is above the render scale, the shadow
  maps are drawn at that scale on both axes. The launchers' "Shadow quality" row writes 0 (Original: the render scale,
  as everything else), 2 (High) or 4 (Ultra): at least twice or four times the game's resolution, never below the
  render scale (Gustavo's choice of fixed sizes over a multiplier: at 1440p POTCAWE's 1024² maps are 2048² in Original
  and High and 4096² in Ultra; the memory stays bounded). It is capped where a 2048² map would exceed the largest
  texture.
- **Which surfaces are shadow maps, with no game code:** a depth surface drawn or cleared with no color surface bound,
  which is not of the game's output size. The first such use marks the surface (`RenderTargets::MarkShadow`, keyed as
  targets are); from then on:
  - a draw with no color surface, a clear (whatever is bound) and a resolve from it use its target at the shadow
    scale;
  - a draw that binds a color surface with it uses its target at the render scale, so that both attachments are at one
    scale.
- **Resolves from a shadow map** go to a resolved texture at the shadow scale (`ResolvedTexture::shadow`, part of its
  key), and the destination rectangle is scaled by that texture's scale. Fetches sample it with normalized
  coordinates, so its size does not matter to them.
- **The slope-scaled depth bias** is multiplied by the target's scale (the larger axis), as the SDK's Xenos path
  multiplies it by its resolution scale: the slope is per host pixel, so at a higher scale the same factor would give
  less bias. At the render scale this changes nothing at 1×; POTCAWE sets no polygon offset at the dock (probe, below).

**Why:**
- **Gustavo's request** (the launcher's row, shown as "coming soon" since Launcher 2's phase 1; his choice of fixed
  sizes).
- **No game hooks needed.** Shadow maps are the passes that draw depth only: POTCAWE's two cascades (a 1024² float24
  surface, pitch 1040, bound with no color target: trace `potcawe_013`, `rt0=0x0`) and Kameo's 768² and 1024² maps. The
  scene, a depth prepass or the HUD are of the output size or bind a color target.
- **The clear goes to the shadow target whatever is bound.** POTCAWE clears its shadow maps with the XDK's ClearF,
  which binds an aliased color surface for its rectangle (pitch 520, 4× MSAA). Taken by the binding, that clear went
  to the target at the render scale; the shadow target was never cleared, the second cascade was drawn over the
  first, and characters got large dark shapes instead of their shadows (runs 398 and 399, 4× and 2×; run 400, a forced
  4× render scale, right; run 401, the clear fixed, right).
- **The translator's baked scale** (D37) reverts the render scale, not the shadow scale, in the texel offsets of
  resolved textures: a fetch offset into a shadow map spans the render scale's host texels. POTCAWE's shadow fetches
  are point samples (`tfetch3D`, four per pixel), whose footprint stays within a guest texel either way; the image at
  Ultra matches a forced 4× render scale (below).

**Results** (POTCAWERecompVulkan `local_docs/19-shadow-quality.md`):
- **Image:** at the dock in a 1280×720 window, Ultra (4096²) gives Jack's and the sailors' shadows smoother edges than
  Original (1024²), and the same shadows as a forced 4× render scale.
- **Cost** (`gpu_ab`, the dock, 60 FPS): at 1×, 1.73 → 1.91 ms of GPU time per frame (17.8 → 19.0 %); at 2×, 3.50 →
  3.64 ms (28.5 → 29.0 %). Memory: +128 MB for the 4096² depth target and +128 MB for its resolved texture (two
  slices).
- **Polygon offset:** a temporary probe saw none at the dock (no draw with a slope or constant bias).
- **The test of every game** (recomp-tools, defaults): all six runs OK, observers at 100 % (Kameo 4,114/4,114, GRAW
  1,381/1,381, POTCAWE 1,128/1,128), the images within their limits. With the shadow scale at 4: Kameo's two maps found
  and at 3072² and 4096², its image within its limit; GRAW's none.

**Limits:**
- **GRAW's shadow pass binds a color target** (a 1024² `8_8_8_8` surface with its 1024² depth): it is not found, and
  GRAW's launcher has no row (`game.h`'s `kShadowQuality`).
- **A shadow map drawn into a color surface** (POTCAWE's point light cube in level 3, `k_32_FLOAT`, D51) keeps the
  render scale.
- **The first frame's clear** of a shadow map, before its first draw marks it, goes to the render scale's target, which
  is then unused and freed after a minute (D52).

**Revisit if:** a game draws depth only into a surface that is not a shadow map and that something else of the render
scale then reads by pixel position, or a game's shadow fetches use offsets of more than a texel.

## D93 — A texture's size in a game's constant: the host's, for a resolve result (2026-10-22, Gustavo)

**Decision:**
- **`Profile::texture_size_constants`:** a game lists the float constants in which it gives its shaders the size of a
  texture they sample, `(width, height, 1 / width, 1 / height)`, with the stage and the texture's fetch constant.
- **At each draw** whose shader of that stage reads the constant and samples that fetch constant: when the texture
  bound there is a resolve result whose host size differs from its guest size (`Textures::Resolved`), and the constant
  holds exactly the guest size, the uploaded constant gets the host size instead (the register file is unchanged). The
  first time, the log says so.
- **POTCAWE's profile:** the pixel shaders' c38, fetch constant 8 (its shadow maps).

**Why:**
- **D92's shadow scale changed nothing to see** (Gustavo, 2026-10-22, at 3440×1440: Original 2752×2048 and Ultra
  4096² looked the same, with the same jagged edges on Jack's shadow). POTCAWE's shadow-receiving pixel shaders (the nine
  dumped at the dock, `6B954BAB187F64D2` and the others with `tfetch3D … tf8`) filter the map themselves:
  `floor(uv × c38.xy) × c38.zw`, four point samples at that texel's corners, blended by `frac(uv × c38.xy)`, with
  c38 = (1024, 1024, 1/1024, 1/1024). On a larger map those samples land on the 1024 grid: one host texel of each block
  is read, and the shadow is the 1024² one whatever the map's size, at the render scale (D37) as with the shadow scale.
- **With the host size in c38** the same filter runs on the host's texels: Original gets the render scale's map (2048²
  at 1440p, which was drawn and wasted until now), High and Ultra theirs.
- **Matched by value and by use:** three other pixel shaders read c38 without sampling fetch constant 8, with another
  meaning; they are left alone, and so would be a constant that does not hold the guest size.
- **The game's knowledge stays in its profile** (rule 1): which constant holds which texture's size is POTCAWE's; the
  library only swaps a size it can check.

**Results** (POTCAWERecompVulkan `local_docs/19-shadow-quality.md`, the dock):
- **At 1×:** Ultra's shadows sharp (Jack's hair, sword hilt and hat drawn), Original's as before (1024², nothing to
  change).
- **At a forced 2×:** Original sharper than before (2048² used), Ultra sharper still.
- **The test of every game** (recomp-tools, 1280×720 windows, where the render scale is 1 and nothing is swapped): all
  six runs OK, observers at 100 % (Kameo 4,160/4,160, GRAW 1,285/1,285, POTCAWE 1,128/1,128), the images within
  their limits. Kameo and GRAW set no entries: D93 does nothing there.

**Limits:** the first frame at a level may sample a map resolved at the render scale before the shadow pass is marked
(D92), with the size it has: right for that frame.

**Revisit if:** a game passes a texture's size in another layout (only the width, texel size first): a field for the
layout.

## D94 — The scene copied twice a frame: antialiased once (2026-10-23, Gustavo)

**Decision:**
- **`Profile::scene_resolve_once`:** when true, only the first resolve of each frame that D41 / D65 take for the
  finished scene (the profile's callers, the game's output size) is antialiased; the flag is reset at each Present.
  False (the default): every one, as before.
- **Kameo's profile sets it.**

**Why:**
- **Kameo copies its scene twice a frame through the same calls:** the first post-processing pass (the scene with its
  fog) and the bloom composite, both from `0x820DC4B0 < 0x8227AE6C`, both 1280×720 into the same texture
  (`0x82758880`; the level trace of KameoRecomp `local_docs/08`). The callers and the destination cannot tell them
  apart, so FXAA or SMAA ran twice in every frame of play.
- **Measured** (KameoRecomp `local_docs/23-gpu-cost-4k.md`, Gustavo's garden at 3840×2160 and 120 FPS, RenderDoc's GPU
  duration per event): the frame 6.31 ms, SMAA 0.91 ms of it in two runs of a copy, the edges, the weights and the
  blending. With the option: one run (~0.32 ms in the first level's frame), about 0.45 ms a frame less at 4K.
- **The first copy rather than the last:** the last one cannot be known when it comes, and between the two Kameo draws
  only the bloom's downsampled passes and one full-screen composite, with no geometry: the edges are the first copy's.
- **Off by default:** GRAW and POTCAWE copy their scene once a frame.

**Results:** the test of every game (recomp-tools), all six runs OK: Kameo with its profile setting it, the observer at
100 % (4,154/4,154) and the image within its limit (the same difference to the baseline as before, 10.35); GRAW
(1,285/1,285) and POTCAWE (1,137/1,137) unchanged. A RenderDoc capture of Kameo's first level at 3840×2160 shows one
SMAA run.

**Revisit if:** a game whose first copy of the frame is not the one with the edges (geometry drawn between two
copies): the copy's index in the frame as the profile's choice.

## D95 — A resolve through an MSAA surface reads the single-sampled target drawn at its EDRAM (2026-10-24)

**Decision:**
- **Each host target stamps its last write** (`HostTarget::written`): every draw stamps the targets bound to it,
  every clear and fill the one it clears, the promotion of a reduced target (D49) the full one, in one increasing
  order (`g_target_writes`).
- **At a resolve from a multisampled color surface**, `RenderTargets::FindMsaaAlias` looks for a single-sampled color
  target at the render scale with the same EDRAM base and storage format and the pitch the samples take: twice the
  surface's for 4x MSAA, the same for 2x. A 4x pixel is 2×2 samples and a 2x one 1×2 (one above the other), stored as
  that many pixels of the 1x surface, as the SDK's EDRAM lays them out. When that target was written after the
  surface's own, the resolve copies from it: the source rectangle times the samples, filtered to the destination's
  size (`kColorScaled`, a factor of 2 per axis that a linear sample at each destination pixel's center averages
  exactly), and the resolve's clears (0x100, 0x200, D80) go to that target too. The first time, the log says so.
- **A resolve of one sample** (fragment bits) through such an alias is averaged, with a warning once.

**Why:**
- **Guitar Hero: Metallica's songs were covered in noise and stripes** with NativeXRenderer (2026-10-24): the engine
  draws its 1280×720 scene into a 1x surface at EDRAM base 0 (pitch 1280), binds a 640×360 4x MSAA surface at base 0
  (pitch 640) and resolves it with no draw in between, a 2×2 average of the scene for its post-processing (the
  RenderDoc capture: that resolve's host target, never drawn, held garbage, which the post-processing spread over the
  scene). On the console both surfaces are the same EDRAM; NativeXRenderer keys host targets by the surface's shape
  (D60), so the 4x surface had a target of its own that nothing drew into.
- **Ordered by writes, not by frames**: the same EDRAM can be drawn through either surface; the target written last
  holds what the EDRAM holds.
- **Color only**: depth through an aliased MSAA surface has not been seen; a resolve averages depth samples, which
  no game reads as a downsample yet.

**Results** (GHMetalRecomp, 1280×720 window): the log names the alias (640×360 4x reads the 1280×720 target, base 0);
the career's opening cinematic and its first song in the arena are clean, at 60 FPS. The observer is unaffected (it
compares the guest's draws, 654 of 654 in a song).

**Limits:** a 1x target at a reduced scale (D49) is found only after its promotion; a surface partly covered by the
alias reads what the alias has there.

**Revisit if:** a game resolves depth through an aliased MSAA surface, or draws through a multisampled surface and
resolves through a single-sampled one (the opposite direction: a copy of each sample).

## D96 — A resolved texture that still holds its resolve is freed by time only above a memory budget (2026-10-24, Gustavo)

**Decision:**
- **D52's check stays** (every 600 frames, the resolved textures unused for a minute), with two outcomes:
  - **an invalidated texture is freed** as before: its guest memory holds newer data (D70, D74), so no fetch finds
    it again;
  - **one that still holds its resolve is kept** while all the resolved textures take at most
    `native_x_renderer_resolved_memory_mb` (384 MB by default, the soft limit of the SDK's cache for guest
    textures). Above it, the least recently used are freed first, until the total is under it.
- **0 restores D52's behavior**: freed after a minute unused.
- **Render targets are unchanged.**

**Why:**
- **Guitar Hero: Metallica's stage under a blocky pattern.** In Gustavo's play with NativeXRenderer (SMAA,
  3440×1440) the pattern appeared now and then, in songs and in the pause menu.
  - The RenderDoc capture: the post-processing's full-screen composite blends the sharp and the blurred scene
    through a 128×64 k_8 mask at 0x76ED000, and read that mask as a guest texture, which held garbage.
  - The engine resolves the mask once, when the career's first song loads. It samples it only in the pause menu,
    on the failed-song screen and in some moments of a song.
  - After a minute without those, D52 freed the mask, which the console's memory would still hold. The log has
    "a draw reads guest memory at 0x76ed000 ... freed unused", and the reads ("fetches ... read guest memory a
    resolve had written") at each of the five moments he dumped a frame.
- **D52's premise does not hold for every game.** D52 assumed that what a game resolves only now and then it
  resolves again before sampling it. That holds for Kameo's journal pages; GRAW's Cross-Com pictures (D74) and
  this mask are resolved once and sampled for minutes.
- **A budget keeps D52's bound.** Kameo's 64-minute session at 3840×2160 freed 90 resolved textures (551 MB).
  GH Metallica's 22 take 117 MB at 3440×1440.

**Results** (GHMetalRecomp, 1280×720 window, the career's first song): reproduced with D52's minute shortened to
120 frames, a local change for the test only.
- **With the budget at 0:**
  - the stage under the blocky pattern in the song and in the pause menu, as in Gustavo's screenshot;
  - the mask and two 512×64 textures read from guest memory: 500 to 3,600 such reads every 600 frames.
- **With 384 MB:** clean, no such read, no resolved texture freed (19 kept), 60 FPS.
- **Gustavo's play** (3440×1440, SMAA): no blocky stage.
- **The test of every game** (recomp-tools): all six runs OK, the observer at 100 % (Kameo 4,114/4,114, GRAW
  1,381/1,381, POTCAWE 1,128/1,128), Kameo's image within its limit (10.01, as before). GRAW and POTCAWE also took
  D94 and D95 with it.

**Limits:** above the budget the least recently used texture that still holds its resolve may be one the game
samples later; the log's line names it when a draw reads there.

**Revisit if:** a game's resolved textures pass the budget in normal play (the log's device memory line), or a
game resolves into ever new addresses without its memory being written in between (they would stay up to the
budget).

## D97 — Guest memory pages rewritten every frame are compared by hash, not watched (2026-10-25)

**Decision:**
- **A page found written again 8 times, each within 16 frames of the last, is left unwatched.** It stays invalid:
  every request hashes it and copies it if it changed (D35's hash), and nothing protects it, so the game's writes
  there no longer fault. After 120 frames unchanged it is watched again. `native_x_renderer_unwatch_rewritten`
  (on) turns it off.
- **Texture data and resolve destinations stay watched**: the texture cache and the resolved textures (D70) learn
  of the game's writes through the watches. Pages the texture cache requests, or a resolve wrote, are always
  watched; one that was unwatched fires the invalidation for its page when it becomes so.
- **Those pages are made valid as written by the GPU.** The SDK widens the invalidation around a CPU write up to
  256 KB, but not across GPU-written pages; the flag has no other use there. Texture pages then stop the widening:
  a write to one texture page no longer invalidates the textures around it.

**Why:**
- **The renderer thread in GH Metallica's songs** (`native_x_renderer_thread`): 4.8 ms of CPU per frame, 56 % of a
  core at 120 FPS (the frame rate GHMetalRecomp now offers), and more than one core at 240. Sampled by function,
  `SharedMemory::RequestRanges` took 59 % of its busy time:
  - VirtualProtect, protecting the requested pages again (`MakeRangeValid`), 26 %;
  - XXH3, hashing pages requested again unchanged, 19 %;
  - the shared memory's lock, held by the game's thread while it handled its faults, 10 %.
- **What was rewritten.** Counted per frame:
  - a ring buffer of about 18 MB, rewritten about every 10 frames (450 pages a frame);
  - 20 single pages of small textures, rewritten every frame (a few of them three times).
  - 35 faults a frame, each invalidating up to 64 pages around it, so about 1,000 pages (450 of them texture pages)
    were protected again and hashed again every frame.
- **Hashing costs less than watching** for such pages: a 4 KB page hashes in a fraction of a microsecond; a
  protection is a system call, and a fault an exception, a system call and the lock.
- **Exact invalidation everywhere was worse:** with every page marked as GPU-written, the ring buffer faulted
  once per page (470 faults a frame) until its pages were unwatched. The widening stays for the other pages.

**Results** (GHMetalRecomp, 1280×720 window, the career's first song, the bot):
- **The renderer thread**: 4.8 → 2.9 ms of CPU per frame; at 120 FPS 56 → 36 % of a core, at 60 FPS 29 → 18 %.
  VirtualProtect 26 → 1.5 % of its busy time, the lock 10 → 3 %.
- **Per frame:** about 6,100 pages unwatched (the ring buffer), 8.5 MB of them hashed; 27 faults (25 on the small
  textures' pages), 33 pages protected again.
- **The whole process:** 2.06 → 1.65 cores at 120 FPS, 1.17 → 0.90 at 60. GHMetalRecomp's own change to its
  D3D library's waits for the GPU is in that count too.
- **The images:** the same (the stage, the band, the highway, the failed-song screen).

**Limits:**
- An unwatched page is hashed at every request, several times a frame when several draws read it.
- A page that is both a vertex buffer and a texture is unwatched until the texture cache requests it: a texture
  there can miss one change (the invalidation is fired when the page becomes watched again).

**Revisit if:** a game rewrites texture data every frame in large amounts (it stays watched: the faults remain), or
its uploads per frame (each ending the render pass) become the larger cost: GH Metallica has about 450 a frame.

## D98 — Uploads ahead: the queued draws' vertex and index data uploaded in one batch (2026-10-25)

**Decision:**
- **A draw whose own requests copied** (its vertices, indices or textures; the render pass is ended for them anyway)
  **looks at the draws queued behind it before it draws** (`CommandQueue::Peek`, the renderer's thread only): their
  register records applied to a copy of the registers the ranges come from (the fetch constants, `VGT_DRAW_INITIATOR`,
  `VGT_DMA_BASE`), the vertex fetch ranges their vertex shaders use and their index ranges gathered
  (`DrawPath::AddDrawRanges`), up to `native_x_renderer_upload_ahead` draws (256) or the frame's end, and requested at
  once (`DrawPath::UploadAhead`): their copies are recorded with the draw's own.
- **Each draw still requests its own ranges when it runs**, finding them uploaded; a draw whose vertex shader was
  not analyzed yet is not looked at (it is analyzed at its own draw).
- **Within a window, an unwatched page (D97) is hashed once**: the draws' requests skip a page the window already
  hashed (`GuestMemory::BeginWindow` / `EndWindow`); an inline draw's restore clears that for its pages.
- **Resolves do not end a window**: a resolve writes the mirror on the GPU after the window's copies, and the pages it
  made valid are not uploaded again. Only the frame's end does.

**Why:**
- **Every upload ended the render pass.** GH Metallica's songs at 3440×1440, 160 FPS: about 440 copies a frame, and
  the draws interrupted by copies 193–223 times a frame (RenderDoc). Each interruption stores and loads the render
  targets again (3440×1440 color and depth), and waits behind barriers.
- **What the copies were** (a temporary count): per 100 draws, 46–51 copies of guest memory read as vertices and
  indices, about 1 of converted indices, about 1.4 of texture data, none of inline data restored.
- **The queue is deep enough:** at a draw, on average 200–250 commands were queued behind it, more than 64 for over
  90 % of the draws (a temporary count).
- **It is safe by the queue's design (D71):** the game's thread writes a draw's data before queueing it and reuses
  it only after the command processor's next signal, which waits until this thread has run every queued command. So
  the data of every queued draw is as it will be when the draw runs. Inline draws (whose memory the D3D library
  reuses on its own schedule) still restore their copies at their draw, as before.

**Results** (GHMetalRecomp, the career's first song, the bot):
- **The GPU, at 3440×1440, 160 FPS, ultrawide, SMAA:** NativeXRenderer's frame timer 4.5–5.6 → 2.7–2.9 ms a frame.
  Two RenderDoc captures: draws interrupted by copies 223 / 193 → 9 / 6 a frame; every event's GPU time 4.41 / 4.23
  → 2.94 / 3.12 ms; the scene's draws at the output's resolution 1.75 / 1.62 → 0.78 / 1.00 ms; the copies 0.84 / 0.72
  → 0.52 / 0.43 ms.
- **The renderer thread, at 1280×720, 120 FPS** (the same session with `native_x_renderer_upload_ahead = 0`): 2.86 →
  2.59 ms of CPU a frame; unwatched pages hashed 10.4 → 5.3 MB a frame; 4.8 windows a frame, about 100 draws each.
- **Earlier versions:**
  - windows ended at resolves (31 a frame in GH Metallica): 37 windows of 13.5 draws, 2.65 ms; without the window's
    hashing rule the unwatched pages were hashed twice (20 MB a frame);
  - a window opened by every draw, copying the whole register file (80 KB): in the test of every game GRAW's renderer
    thread went 2.56 → 3.09 ms and POTCAWE's 3.05 → 3.77 ms, with 607 and 397 windows a frame of 2–3 draws (their
    queues are short, and they upload little: 81 and 66 KB a frame). Opened only by a draw that copied, with the few
    registers it reads: GH Metallica 4.8 windows a frame of about 100 draws, 2.86 → 2.59 ms.
- **The images:** the same (the stage, the band, the highway, the HUD).
- **The CPU at 3440×1440:** at 120 FPS the process 1.54 → 1.45 cores, the renderer thread 34 → 30 % of a core
  (60 s of the song). At 160 FPS the renderer thread rose (57–66 → 86–87 % of a core): without D98 the GPU was nearly
  saturated (4.9–6.0 ms of the 6.25), and the thread was blocked behind it part of the time; per frame it took
  4.4–5.3 ms of CPU without D98 and 5.0–5.6 ms with it. Of that, 2.3–3.4 ms a frame is spent in DiligentCore's
  `Present` at this size, with D98 or without (a temporary timer; not at 1280×720): [performance.md](performance.md).
- **The test of every game** (recomp-tools, run 2026-10-25_133912): all OK, observers 4146/4146 (Kameo), 1285/1285
  (GRAW), 1124/1124 (POTCAWE); renderer CPU Kameo 5.94 → 5.80 ms, GRAW 2.56 → 2.41, POTCAWE 3.05 → 3.14 (within its
  runs' 3.05–3.65); 5.1, 6.6 and 5.4 windows a frame.

**Limits:** a draw whose vertex shader is new is not looked at; texture loads still end the render pass where they
happen (about 1 % of the draws in GH Metallica).

**Revisit if:** a game is found to rewrite a queued draw's data before the command processor's next signal (the draw
would show the data as the window found it), or its queue is shallow (fewer draws per window).

## D99 — The swap chain acquires the next image when it is needed, and waits for it asleep (2026-10-25)

**Decision:**
- **DiligentCore patch `0007`** changes the Vulkan swap chain's image acquisition:
  - **`Present` no longer acquires the next image at its end.** The first `GetCurrentBackBufferRTV()` after it does,
    or the next `Present` (or `Resize`, when the swap chain is minimized). NativeXRenderer draws a frame into its own
    render targets and asks for the back buffer only to compose the frame at its `Present` command, about a frame
    later.
  - **`AcquireNextImage` asks with a timeout of 0.** While no image is free it sleeps 200 µs at a time (a
    high-resolution waitable timer, the way the games' waits sleep) and asks again.
- NativeXRenderer itself is unchanged: `Present(0)`, 3 images, at most 2 frames in flight (D22).

**Why:**
- **Where the time went.** GH Metallica's songs at 3440×1440 in a borderless window, 160 FPS (the display at 160 Hz):
  the renderer thread spent 2.1–3.5 ms of every frame in `vkAcquireNextImageKHR` at the end of `Present` (a temporary
  timer; the flush took 0.06 ms, `vkQueuePresentKHR` 0.09 ms), and it was CPU time: NVIDIA's driver spins inside the
  call while it waits. In a 1280×720 window the call took 0.002 ms.
- **It waits for the display, not for the GPU.** Acquired at the end of the next frame instead, about 6 ms later
  and long after the GPU had finished (2.7–3.2 ms a frame since D98), the call still took 2.1–3.5 ms (4.3 at most).
  With frames presented as fast as the display shows them, the presentation engine holds the images until the
  display's next refresh frees one.
- **The deferral alone saves no CPU, but improves the pacing:** the frame interval's worst per 600 frames went
  7.31 → 6.73 ms (median of the song's windows). The sleeping alone saves the CPU but worsens it (7.50 ms). Together
  they save the CPU and pace better than before (7.04–7.12 ms).

**Results** (GHMetalRecomp, the career's first song, the bot; 3440×1440, 160 FPS, ultrawide, SMAA; per 5 s window
during the song, 19–23 windows a run; the variants compared in one build through a temporary switch):

| Variant | Process | Renderer thread | Interval, worst per 600 frames (median) |
|---|---|---|---|
| Before (acquired at `Present`'s end, waiting in the driver) | 2.17 cores | 0.78 | 7.31 ms |
| Deferred, waiting in the driver | 2.14 | 0.79 | 6.73 ms |
| Acquired at `Present`'s end, sleeping | 1.71 | 0.38 | 7.50 ms |
| **Deferred, sleeping (patch `0007`)** | **1.60, 1.67** (two runs) | **0.37, 0.38** | **7.12, 7.11 ms** |
| Patch `0007`, the clean build | 1.68 | 0.39 | 7.04 ms |

- The average interval stayed at 6.26 ms in every variant; the images are the same.
- After it, 60 % of the renderer thread's stack samples wait on the queue's condition variable (idle), and no stack
  sample is in the acquisition.
- **Earlier tries:**
  - 5 swap chain images: the acquisition still waited in the songs (process 2.05 cores).
  - A sleeping wait for the GPU before `Present` (on the frame fence): 1.67 cores, but the pacing worse (the average
    interval 6.29–6.31 ms, the worst 8.8–9.7 ms): it waited for the wrong thing.
  - Sleeping 100 µs or 50 µs at a time: no change (1.71 and 1.67 cores, 7.23 and 7.14 ms).
- **The test of every game** (recomp-tools, 1280×720 windows, run 2026-10-25_163922): all OK, observers 4171/4171
  (Kameo), 1288/1288 (GRAW), 1128/1128 (POTCAWE); renderer CPU Kameo 5.80 → 5.75 ms, GRAW 2.41 → 2.35, POTCAWE
  3.14 → 3.15.

**Limits:** the composition starts up to 200 µs (and the timer's wake-up) after the display frees an image; the
measured pacing is still better than before.

**Revisit if:** a driver blocks in `vkAcquireNextImageKHR` with a timeout of 0 (the specification says it returns
`VK_NOT_READY`), or presentation moves to a thread of its own or to `VK_KHR_present_wait`.

## D100 — A shader's float constants copied a run at a time (2026-10-26)

**Decision:** before each draw, the float constants a shader uses are copied into its constant buffer one run of
consecutive registers at a time (one `memcpy` per run), not one register at a time. The constant that D93 swaps for a
texture's host size is overwritten in the copy; the object motion's copy (D48) takes the same runs.

**Why:**
- **Where the time went.** Kameo, Gustavo's spot (the save "Kameo" in the Enchanted Kingdom, the camera turned with R3
  twice to the meadow; 3440×1440, 120 FPS), about 3,850 draws a frame: the renderer thread took 5.8–6.0 ms a frame,
  70 % of a frame at 120 FPS. Its stack samples resolved to source lines (a sampler with `SymGetLineFromAddr64`):
  the loop that gathered the float constants register by register (a bit scan and a 16-byte copy for each) took
  about 14 % of the thread's samples, more than any other single part; DiligentCore's `SetPipelineState`,
  `DrawIndexed` and `CommitShaderResources` together 19 %.
- **Shaders use ranges.** Matrices, bones and lights are consecutive registers, which the register file keeps in
  order: a run is copied as it is.

**Results** (the same spot, one session, the old and the new copy switched by a temporary flag file every 18 s,
three rounds; the log's 600-frame windows inside each phase):

| Copy | Renderer CPU per frame | Draws per frame |
|---|---|---|
| Register by register | 6.03, 6.08, 6.01, 6.08, 6.06, 5.93, 6.01 ms | 3,754–3,946 |
| **A run at a time** | **5.10, 5.04, 4.97, 4.95, 5.06, 4.98 ms** | 3,819–3,871 |

After it the copy is 3 % of the thread's samples. What is left of a draw's CPU, by share of the thread's samples:
DiligentCore's pipeline switch, draw and descriptor sets (7.8, 6.8, 4.6 %), the system constants (3.0 %), the
textures' views, requests and samplers (6 %), the vertex data's ranges (2.2 %), the primitive processor (1.8 %), the
pipeline lookup (1.3 %); outside the draw, the register records (5 %) and the timing of every command (2 %).

**Revisit if:** a shader's constants are mostly isolated registers (the runs are then single registers: no worse).

## D101 — Draws into offscreen targets wait for their pipelines (2026-10-27)

**Decision:** a draw whose first target (the first color target, else depth) is not of the game's output size waits
for its shaders' translations (D34) and its pipeline (D28) instead of being skipped, up to 3 s
(`native_x_renderer_wait_offscreen_pipelines`, on). The scene's draws, into targets of the output's size, are still
skipped while theirs are created. The log's creation line counts the draws that waited and for how long.

**Why:**
- **A skipped draw can leave a target undefined for good.** A target is created with undefined contents; the game
  draws or clears it before resolving it. When the draw that fills it is skipped, the resolve copies garbage, and a
  texture the game renders once keeps it: GH Metallica's post-processing mask (128x64, D96), drawn at the career's
  first song load, and its 320x180 blurred copies of the scene. Gustavo's first run of the released beta (an empty
  pipeline storage) showed the song's intro in blocks; the same with an empty storage here covered the whole opening
  cinematic in stripes, for about 25 s.
- **Only a first run, or a new scene, has pipelines to create** (D42 prepares the stored ones at startup), so the
  waits are short and rare; offscreen passes are few next to the scene's draws, which keep D28's "a frame late".

**Evidence:** GHMetalRecomp `local_docs/16-first-run-garbage.md`. GH Metallica, the career's first song at
3440x1440, the pipeline storage deleted before the run: without D101 (`ghmetallica_293`) stripes over the opening
cinematic; with it (`ghmetallica_294`) a clean image, and 1 to 12 offscreen draws waited per 600 frames, 1.5 to 32 ms
in all, at most 7.7 ms for one draw.

**Revisit if:** a game's scene is drawn into a target of another size than its output (the scene's draws would wait),
or a game draws offscreen passes every frame with new shaders often enough for the waits to show.

## D102 — A segment change outside any tracked call is not read as packets (2026-10-27)

**Decision:** the tracker reads nothing for a `KickSegment` or `NewSegment` called outside any tracked call, as it
already did for one inside a call (rule 10 of xdk-d3d-reference.md).

**Why:**
- **The kick's gap.** GH Aerosmith (XDK 2.0.6683) kicks command buffer segments outside the D3D calls the tracker
  hooks, several times a frame. The kick writes ten dwords, `INVALIDATE_STATE` and two `EVENT_WRITE_SHD` marks, and
  continues with the write pointer aligned up to 32 bytes, as XDK 2417's kick does (GRAW). The 4 to 7 dwords in
  between are never written: they hold packets from an earlier lap of the ring, which the GPU does not execute. The
  tracker read such a kick like any call, from the write pointer at its entry to the one at its return, so it read
  them as packets: most often a header whose length runs past the end ("not fully" read calls in the observer's
  report), now and then a complete old register write.
- **The stage dark for a frame.** Such writes were the window offset and scissor (`PA_SC_WINDOW_OFFSET`,
  `PA_SC_WINDOW_SCISSOR_TL`, `PA_SC_WINDOW_SCISSOR_BR`) of an earlier pass: the bloom's 320x180, the crowd sheets'
  256x512, a 128x64 one. The scene's draws up to the next write of the scissor were clipped to that size, so most of
  the stage stayed black, and what came after it (the HUD, part of the scene) was drawn. Gustavo saw the stage flicker
  in songs, with NativeXRenderer only, at 60 and 160 FPS: about one frame in 450.

**Evidence:** GHAeroRecomp, a Quickplay song at 160 FPS, with temporary probes (removed). The scene's brightness read
back at its first resolve: 25 to 43 single dark frames per song. The constants, fetch constants and render state of
every draw into the scene's target, kept for the last 12 frames: in 23 of 25 dark frames, 28 to 212 scene draws had
an earlier pass's `PA_SC_WINDOW_SCISSOR_BR` (0x00B40140, 320x180, in the first) where both neighbors had 0x02D00500,
and nothing else differed. The window registers' writes the tracker read, per frame: the dark frames' came from a
kick's scan, whose bytes were the two marks followed by an old `00022080 00000000 00000000 00B40140` (or 0x02000100,
0x00800040). Not the GPU: with the GPU finishing each frame
before the next, the dark frames stayed. With D102: no dark frame in a whole song, none in 1,000 screenshots (3 to 7
before), and the observer at 100 % over 7 frames of a song, no call "not fully" read.

**Revisit if:** a game writes state packets inside a kick it calls on its own: those would be lost (the observer's
render state mismatches would show them).

## D103 — D24S8 depth on GPUs without D24_UNORM_S8_UINT (2026-10-27)

**Decision:**
- The guest's D24S8 depth targets are `D24_UNORM_S8_UINT` where DiligentCore reports that format as a depth target
  that can also be sampled (`GetTextureFormatInfoExt`), and `D32_FLOAT_S8X24_UINT` otherwise, as D24FS8's always
  are; the SDK's Vulkan backend chooses the same way. The log says so once when it takes the fallback.
- What read the guest's depth format from the host format reads it from the guest's: the packed depth words of D64
  (`HostTarget::depth_float24`), which are unorm24 for D24S8 whatever the host stores.
- Stored pipelines (D42) whose depth format is `D24_UNORM_S8_UINT` are prepared with the fallback on such a device
  (a storage written on another GPU): the same state its draws ask for.
- The depth bias stays the guest format's, as on the SDK's Vulkan backend. With a float depth target Vulkan scales
  the constant factor by the primitive's exponent, `2^(e-23)`: unorm24's step `2^-24` for depths from 0.5 to 1,
  smaller nearer the camera.

**Why:** AMD's Vulkan drivers do not offer `D24_UNORM_S8_UINT` as a depth attachment (the specification requires
it or `D32_SFLOAT_S8_UINT`; Mesa's bug 106528 was closed WONTFIX, and vkd3d falls back the same way).
NativeXRenderer created it without asking, so on AMD the depth targets of the games that use D24S8 (GH Metallica,
GH Van Halen, GH Aerosmith, Kameo; GRAW and POTCAWE use D24FS8 only) would not be created, or would be created by an
invalid call. Found by reading the code; not seen on an AMD machine.

**Evidence:** GHMetalRecomp `local_docs/18-amd-depth-format.md`. On the RTX 4070 Ti with the Khronos profiles layer
hiding `D24_UNORM_S8_UINT` (`exclude_formats`, profile emulation off): GH Metallica's career, its first song drawn
as without the layer (the scene, the band, the highway), the observer 452 of 452 draws; with the validation layer
above it, no error over five depth targets taking the fallback (300 to 470 draws a frame), only the two known ones
(a uniform block's layout in shader modules, a queue family index). Test-Games all OK as it was
(`runs/2026-10-27_185200`), and with the layer hiding the format (`2026-10-27_190525`: Kameo 4152/4152, GRAW
1285/1285, POTCAWE 1128/1128, GH Metallica and GH Van Halen 7/7, the images within their limits).

**Revisit if:** a player with an AMD GPU sees depth problems, the bias first (decals, shadows fighting nearer the
camera), or a game reads a D24S8 depth resolve another way than D64's.

## D104 — The image's aspect ratio can be another than the output's (2026-10-28)

**Decision:**
- The cvar `native_x_renderer_aspect` (0 by default, read at startup) asks for the image's aspect ratio while
  `native_x_renderer_fill_screen` is on: 0 is the output's (D43 as it was); another, such as 2.3889 (21:9) or
  3.5556 (32:9), is rendered at that ratio. The scale is per axis as in D43, its factor that ratio over the game's;
  the vertical scale is the largest at which the image fits the output on both axes,
  `min(output height / 720, output width / (1280 * factor))`, or the resolution scale forced, never below 1.
- The presentation does not change: it already scales the front buffer to fit the swap chain and centers it, so the
  bars are above and below when the image is wider than the output, at the sides when it is narrower.
- `AspectFactor()` is that factor, so a game's hooks widen its cameras and keep its 2D layer at 16:9 for the image,
  not the screen. A game's own overlay that covers parts of the image (GH Metallica's menu bars) computes the image's
  rectangle in the output the same way.
- The launchers' Format row (RecompLauncher 3.0) offers 16:9, 21:9, 32:9 and the output's own; it writes
  `native_x_renderer_fill_screen = false` for 16:9 and the ratio otherwise (0 for the screen's own, so the image
  follows another monitor), and computes the render size the same way.

**Why:** Gustavo asked for the format by name (21:9, 32:9) instead of "fill the screen", for a 32:9 screen to choose
16:9, 21:9 or 32:9, and for a 16:9 screen to show 21:9 with bars (and his 21:9 screen to preview 32:9). Everything
after the render scale already worked for any factor: the targets, the resolves and the presentation take their sizes
from it.

**Evidence:** GH Metallica in a 1720×720 window at 3.5556: render scale 2.0 × 1.0 (2560×720, the vertical scale held
at 1), the image 1720×484 with bars above and below, the boot video and the main menu in their 16:9 box in its
center with the menu bars at its sides, the career's first song with the stage across the image and the notes on
their lanes. In a 1280×720 window at 2.3889: 1720×720 rendered (factor 1.3438), shown at 1280×536; menus and a song
as expected. No error in either log.

**Revisit if:** a game draws something the output's size rather than the image's (a full-screen overlay of its own
outside the hooks would cover the bars), or the ratio needs changing while the game runs (it is read at startup, as
`native_x_renderer_fill_screen`).

## D105 — 1_5_5_5 textures (2026-10-28)

**Decision:**
- Guest textures in k_1_5_5_5 are loaded as the SDK's D3D12 backend does: the load shader
  `texture_load_r5g5b5a1_b5g5r5a1_cs` swaps red and blue into `TEX_FORMAT_B5G5R5A1_UNORM` (DXGI's layout: blue in bits
  0-4, green 5-9, red 10-14, alpha 15), sampled with the RGBA swizzle. Until now the format had no host format (D23)
  and its bindings sampled the white placeholder.
- DiligentCore patch 0008 maps `TEX_FORMAT_B5G5R5A1_UNORM` to `VK_FORMAT_A1R5G5B5_UNORM_PACK16`, which is that layout
  (and the SDK's Vulkan backend's format for 1_5_5_5), instead of `VK_FORMAT_B5G5R5A1_UNORM_PACK16` (alpha in bit 0,
  blue in the top bits). A1R5G5B5 is one of the formats every Vulkan device samples; B5G5R5A1 is optional. Nothing
  else in NativeXRenderer used the format (resolves of 1_5_5_5 surfaces stay RGBA8, `HostTextureFormat`).

**Why:** Guitar Hero: Van Halen's career intro drew Eddie Van Halen's guitar white (lavender under the stage lights)
with only its relief: the instrument's color textures are 1_5_5_5 (2D, tiled, packed mips, 256×128 to 512×256), and
the Xenos path showed the striped finish. Patching the mapping keeps the 16-bit data as it is, like patch 0002 did for
BC1, rather than a new load shader that expands it to RGBA8.

**Evidence:** GHVanHRecomp, Career > Single > Guitar > Easy, the intro: 6 to 10 placeholder bindings a frame, all
fetch constants of format 3 (a temporary log in `Textures::FindView`, removed: base addresses 0x0F250000,
0x117AD000, 0x11845000…, none of them a resolve), the guitar white (`ghvanhalen_029`, `_030`); with the change
0 placeholders a frame and the guitar as on the Xenos path (`ghvanhalen_031`). DXT3A_AS_1_1_1_1 is still not
supported.

**Revisit if:** a 1_5_5_5 texture's alpha looks inverted or its colors swapped (the load shader and the mapping are
the SDK's D3D12 and Vulkan ones), or a device fails to create the format (it must not: it is mandatory).

## D106 — The present mode: V-Sync without tearing, or immediate (2026-10-29)

**Decision:**
- `native_x_renderer_present_mode` (empty by default): how frames reach the screen. Empty keeps D99's: MAILBOX, else
  IMMEDIATE (DiligentCore's order for a sync interval of 0). `"vsync"`: MAILBOX, else FIFO: never torn; FIFO, the
  mode that waits for the display, only on a driver that offers no MAILBOX. `"immediate"`: IMMEDIATE, else MAILBOX: each
  frame shown as it is ready (tearing; what a G-Sync or FreeSync display wants). FIFO where the surface offers none of
  them. `Device::Present` still presents with a sync interval of 0: the game paces itself.
- DiligentCore patch 0009: `ISwapChainVk::SetPresentModes` takes the modes to try, in order, in place of the ones the
  sync interval picks (FIFO_RELAXED, FIFO with VSync; MAILBOX, IMMEDIATE, FIFO without); a change creates the swap
  chain again at the next Present. `Device::ApplyPresentMode` gives it the option's list before each Present when the
  option changed (the log says which: "present mode: ..." then DiligentCore's "Using VK_PRESENT_MODE_... swap chain
  present mode").
- The games' launchers write it: a V-Sync row (RecompLauncher 3.4, on by default: `"vsync"`, off: `"immediate"`), with
  the SDK's presenters' own options for the Xenos path (`d3d12_allow_variable_refresh_rate_and_tearing`,
  `vulkan_allow_present_mode_immediate`, `vulkan_allow_present_mode_fifo_relaxed`: false with V-Sync).

**Why:** a Guitar Hero: Metallica player asked for a V-Sync option. The SDK's `vsync` is the emulated
console's vertical blank (the game's pacing), not the display's. NativeXRenderer presented MAILBOX on NVIDIA (no
tearing), but IMMEDIATE on a driver without MAILBOX; the Xenos path tears by design (Xenia's presenters). DiligentCore's
VSync gives FIFO_RELAXED first, which shows a late frame at once, torn: hence the patch rather than a sync interval of
1.

**Evidence:** GH Metallica at 1280x720 on an RTX 4070 Ti (`ghmetallica_386`-`388`): empty and `"vsync"` MAILBOX,
`"immediate"` IMMEDIATE (the intro drawn). The FIFO fallback is DiligentCore's own preference loop (not seen: NVIDIA
offers MAILBOX).

**Revisit if:** a driver without MAILBOX shows the game's frame rate held below its own with V-Sync (FIFO waits for
the display: a game set above the display's rate is held to it), or a display's variable refresh rate needs the
present interval rather than IMMEDIATE.

## D107 — V-Sync is FIFO (2026-10-30)

**Decision:** `native_x_renderer_present_mode = "vsync"` presents with FIFO only: each frame shown once, at a vertical
blank, never torn. D106's order (MAILBOX, else FIFO) is `"mailbox"`. The launchers' V-Sync row writes `"vsync"` with
the SDK's `vblank_follows_display` (recomp change 61), which marks the game's vertical blank on the monitor's own, so
a game that paces itself by vertical blanks never produces frames faster than the display shows them.

**Why:** measured with PresentMon 2.6.0 in Guitar Hero: Metallica, 30 s of a song at 3440x1440 on a 159.96 Hz display,
RTX 4070 Ti (GHMetalRecomp's `local_docs/24-frame-pacing.md`). NVIDIA presents Vulkan through a DXGI swap chain, and
its MAILBOX behaves as a three-image queue: with the game's timer at 160.000 Hz, a hair faster than the display, the
queue filled and every frame waited about three refreshes, 16-19 ms from present to display (none dropped). FIFO:
5.8 ms; FIFO with the vertical blank following the display: 4.0 ms, no frame dropped or shown twice, presents within
0.5 ms of the period 92 % of the time. MAILBOX's lower latency assumes a game that renders several frames a refresh,
which these games, paced by their own vertical blank, never do.

**Evidence:** runs m160 / m160b (MAILBOX), f160 (FIFO), l160 (MAILBOX + vblank following), lf160 (FIFO + vblank
following) in GHMetalRecomp's `local_docs/evidence/14_frame_pacing`.

**Revisit if:** a driver's FIFO queues frames when the game runs at the display's rate without
`vblank_follows_display` (a game whose launcher does not write it), or a variable refresh rate display with V-Sync on
shows the game held below its frame rate.

## D108 — New data uploaded faster: the guest memory mapped ahead, one scratch buffer for the texture loads (2026-10-30, Gustavo)

**Decision:**
- **The guest memory's pages are read once on a thread of their own when the draw path starts** (`GuestMemory::
  MapAhead`, `native_x_renderer_map_guest_memory`, on): every page of the view the mirror copies from
  (`TranslatePhysical`), 64 KB at a time with `ReadProcessMemory`, below normal priority. A page the SDK keeps
  without access fails the read instead of raising the exception its handlers take for the game's own accesses.
- **A page never copied (or dropped) is not hashed** before its copy: it differs whatever its data. The copy still
  hashes the snapshot it uploads.
- **One scratch buffer for every texture load**, grown to the largest a texture needs (a power of two, at least
  4 MB), instead of one created per load. The loads run one after another; DiligentCore's transitions (the load
  shader's writes, the copies into the texture that read it, the next load's writes) put the barriers between them.

**Why** (Guitar Hero 5, 3440×1440, 160 FPS; GH5Recomp's `local_docs/02-port-from-gh-van-halen.md`):
- **The longest frames outside the start were new screens' data.** When Party Play's song began, one frame took the
  renderer 56–61 ms: 466 draws, 33–36 ms of them in the texture requests, 24.8–27.6 ms of that copying 37.6 MB of
  guest memory into the mirror. Split further with temporary timers: the first read of each page through this view
  14.5 ms (the game wrote the pages through another view, so the system maps each on first touch: reading one byte
  per page alone took 10.9 ms, 1.1 µs a page; copying them all after that 3.1 ms), protecting the pages again
  3.8 ms, DiligentCore's `UpdateBuffer` 3.3 ms. `PrefetchVirtualMemory` and `VirtualLock` did not map them faster
  (15 ms with the latter). Of the texture loads themselves: the buffer created per load 1.1 ms in all (and 54 ms
  once, when DiligentCore took a new block of device memory), the textures created 3.0–4.1 ms, the load shaders
  2.9–3.2 ms, the copies into the textures 2.1–2.3 ms, the textures' own hashes 2.1–2.2 ms.
- **Mapping ahead costs about 130 ms on a thread of its own, once** (16 pages without access in GH5), and memory:
  the pages the game has not used yet become resident then instead of when it fills them (the venue's textures are
  such pages). At GH5's first screen the game took 120–300 MB more of the system's memory (1.42–1.54 GB, 1.25–1.30 GB
  without it; two runs each); its working set shows 380 MB more, as it counts twice a page mapped both through the
  game's views and through this one.

**Results** (GH5, the same route, the slow frame logged by temporary timers): the first read of the new pages
10.9 → 0.2 ms; that frame's renderer time 56–61 → 38 ms, its interval 62–67 → 44 ms. In the song, unchanged. The
images as before (the menus, the song, 0 placeholders).

**Revisit if:** a game's guest memory is larger than the mirror's 512 MB, or a system with little memory trims the
mapped pages (they would be mapped again on first read, as before).

## D109 — D97's frame counts scaled by the frame rate (2026-10-30, Gustavo)

**Decision:** a page is left unwatched when it is found written again 8 times, each within 16 frames **at 60 FPS**,
and watched again after 120 frames **at 60 FPS** unchanged: both counts are multiplied by the frame rate over 60
(from 1 to 4), the frame's time smoothed over about 64 frames (`GuestMemory::EndFrame`).

**Why:** at 160 FPS GH5 made about 275 watched pages valid again every frame, in about 44 calls (protections), and its
threads faulted on them as they rewrote them. Counted by how soon each came back: 180–345 a frame after 17–32 frames,
40–70 within 4, 30–70 after 9–16. The pages a ring buffer rewrites every 17–32 frames at 160 FPS (106–200 ms) were
never left unwatched with D97's 16 frames, which at 60 FPS were 267 ms.

**Results** (GH5, 3440×1440, 160 FPS): pages left unwatched 1,400 → 5,800; the renderer thread in the song about
4.05 → 3.72 ms a frame; the process 3.27 → 3.06 cores in the main menu, 3.29 → 3.11 in the song. The pages are
hashed instead (4.3–5.1 MB a frame, from 0.6) and copied in more runs (about 280 copies a frame, from 131): what
"hash only what a draw reads" ([performance.md](performance.md#what-to-do-next-in-order)) would cut.

**In the other games** (D108 and D109 together, against the same builds without them, the same routes; the renderer
thread's time a frame, and the longest frames of the run):
- GH Metallica (160 FPS, 3440×1440, Ace of Spades): 4.77 → 4.83 ms, frame interval 6.52 → 6.56 ms (the song is
  heavier than 160 FPS allows, with or without them), hashed 4.3 → 5.2 MB a frame. Within the runs' variation.
- GH Van Halen (160 FPS, the career's first song): 4.41 → 4.19 ms; the start's longest frame 254 → 230 ms.
- GH Aerosmith (160 FPS, the first screen): 2.80 → 2.88 ms; the start's 208 → 181 ms and 34 → 23 ms.
- Kameo (the first level, about 85 FPS): 9.84 → 9.59 ms; the opening video's longest frames 31–38 → 32–39 ms.
- GRAW (about 118 FPS): 4.10 → 4.01 ms, hashed 6.1 → 3.0 MB a frame; the loading's 381 → 215 ms, 162 ms → none
  over 20, 127 → 67 ms.
- POTCAWE (60 FPS: D109 changes nothing): 5.80 → 5.86 ms; the loading's 823 → 728, 139 → 133, 108 → 90 ms.
- The same images at each game's spot, no new errors or warnings.

## D110 — Unwatched pages compared with a copy on the CPU, not by hash (2026-10-30, Gustavo)

**Decision:** a page left unwatched (D97) is compared with a copy of what the mirror holds, kept on the CPU, instead
of by hash (`native_x_renderer_compare_unwatched`, on). The copy is an address range of the mirror's size, reserved,
and committed 64 KB at a time as the unwatched pages are first copied through it; each page at its own offset, so a
run of pages is uploaded from it in one `UpdateBuffer`. A changed page's compare stops at its first difference, and
the copy uploaded needs no hash. A page's copy stops counting when the page is watched again, uploaded by the hash
path (`Upload`) or restored by an inline draw (`WriteIfChanged`); a page uploaded through it has no hash, so once
watched again it is copied at its first request. If committing fails, the pages are compared by hash from then on.
The log's uploads line gives the copy's size.

**Why:** Gustavo chose performance.md's "hash only what a draw reads" next. Measured first (the SDK's range request
callback gives every requested byte range): in GH5's and GH Metallica's songs at 160 FPS the draws requested 86–87 %
and 83 % of the bytes hashed (500–800 of 900–1,400 pages whole), so that would have saved a few hundredths of a ms.
The same measurement showed most unwatched pages changed at each request (about 45 % in GH5, 75 % in GH Metallica),
and every changed page was hashed twice: to compare it, then the snapshot uploaded. Per frame in GH5's song, of
`UploadRanges`' 1.0–1.2 ms: the compare hashes 0.34–0.42 ms, `UpdateBuffer` 0.24–0.29, the snapshot hashes 0.12–0.14,
the snapshot copies 0.05, `MakeValid` 0.03.

**Results** (the same build, the option on and off, two runs each, `UploadRanges` timed, every song window):
- GH5, Party Play's song, 3440×1440 / 160 FPS: 1.024 → 0.957 ms a frame (44 windows each), per MB compared 0.219 →
  0.205 ms (−6 %).
- GH Metallica, Ace of Spades, 160 FPS: 1.477 → 1.338 ms (20 windows each), per MB 0.332 → 0.298 ms (−10 %).
- About 1.5 % and 3 % of the renderer thread: less than the hashes' time suggested, as the compare reads the copy
  (not in the cache) as well as the page the game just wrote. The copy took 28 MB in GH5's song.
- The images as before; the other NativeXRenderer games' Test-Games as before.

**Revisit if:** a game leaves unwatched many pages that seldom change (the compare reads twice the memory a hash did:
`native_x_renderer_compare_unwatched=false` to compare), or memory is short: the copy is never decommitted, so it
holds every page ever left unwatched (a ring buffer's, in these games).

## D111 — Binding sets: each set of textures and samplers a pipeline draws with keeps its own shader resource binding (2026-10-30, Gustavo)

**Decision:** `native_x_renderer_binding_cache` (on, read at startup). A guest draw pipeline's textures and samplers
are mutable variables instead of dynamic ones, and each set of objects the pipeline draws with (per stage, the
textures then the samplers, in the order of the shader's bindings) gets a shader resource binding of its own, its
variables set once when it is created (`PipelineCache::FindSet`). A draw first compares its objects with the set the
pipeline used last, then looks for them by their hash, then creates the set. It commits the set's binding when it
is not the one the last draw committed, or one of its textures is no longer readable (D82's test, on the set's
views); DiligentCore then neither allocates nor writes a descriptor set for it. The sets hold their objects, so a
pointer in a key cannot belong to another object while the set is kept. Sets not used for 600 frames are dropped
with D55's release; above 16,384 sets kept, those not used in the last 8 frames are dropped at the end of each
frame (a warning, logged once). The object motion pipelines (D48) keep dynamic variables. The log's draw line gives
the sets created in the frame and the sets kept.

**Why:** Gustavo chose bindless descriptors next (performance.md, item 5). Profiled first (the renderer thread's
busy time, inclusive): in GH5's song at 160 FPS `CommitShaderResources` 6.4 % (the dynamic descriptor set's
allocation 2.0 %, the writes of every texture and sampler descriptor 3.1 %) and the variables' `Set` 1.3 %; in
Kameo's first level (4,100 draws) the commit and the readable test 7.1 %. That is all bindless could remove: the
pipelines' binds (`SetPipelineState` 7.6 and 9.4 %) and the draws with their descriptor sets bound again for the
constants' dynamic offsets (`DrawIndexed` 7.6 and 9.7 %) stay. Bindless needs the translated SPIR-V rewritten,
explicit resource signatures (D18 reversed) and a DiligentCore patch for update-after-bind; binding sets remove the
same allocation and writes with none of that. Gustavo: "Caché primero, bindless después".

**Results** (the same build, the option on and off, two runs each, the windows aligned on the same routes):
- GH5, Party Play's song, 3440×1440 / 160 FPS: the renderer thread 3.78 → 3.59 ms a frame (−5 %), lower in every
  window; about 420–450 sets kept; no variable set in the song (the sets made at its start).
- Kameo's first level: 9.46 → 8.89 ms a frame (−6 %), per 1,000 draws 2.29 → 2.16 ms; at most 321 sets kept.
- The limit's path checked with the limit at 64 (dropping and making sets every frame in GH5's menus and song): the
  same images, no error, the renderer's time as with the cache.
- The images as before; the other NativeXRenderer games' Test-Games as before, their renderer thread lower (GRAW
  4.01 → 3.73 ms, POTCAWE 5.83 → 5.09, GH Aerosmith 2.8–2.9 → 2.6–2.8).
- **The first run compiles every stored pipeline again**: the textures' variable type changes the pipelines'
  descriptor set layouts, so DiligentCore's pipeline cache from earlier runs (D42) does not match. GRAW: its 1,203
  stored pipelines created in 23.4 s instead of 1.0 s, a first frame of 8 s (Test-Games' route went astray); GH5's
  first frame 329 ms. Once: the cache saved then serves the next runs. Turning the option off or on does it again.

**Revisit if:** a game makes new texture views or samplers all the time (a new set per draw: the sets grow to the
limit and hold their textures' memory for up to 600 frames, or 8 past the limit); the binding sets' descriptor sets
fill DiligentCore's descriptor pools (more pools are made, never freed); a draw path change commits a binding set the
release may have dropped (the release forgets the last committed set); or bindless is done (performance.md), which
would replace this.

## D112 — A depth resolve through an MSAA surface reads the single-sampled depth drawn at its EDRAM (2026-10-31, Gustavo)

**Decision:** D95's alias for depth surfaces too. At a resolve from a multisampled depth surface,
`RenderTargets::FindMsaaAlias` looks for a single-sampled depth target at the render scale with the same EDRAM base,
depth format and the pitch the samples take; when it was written after the surface's own target, the copy reads it at
half the size (2×2 blocks for 4x MSAA, 1×2 for 2x) with the nearest pixel of each block (`kDepthScaled`), not the
average, whichever sample the resolve's fragment bits select. The first time, the log says so ("a depth resolve
through a 640x360 surface with 4x MSAA reads the 1280x720 target ... (D112)").

**Why:** Gustavo (2026-10-31): Guitar Hero 5's title screen with NativeXRenderer was blurred all over and its models
looked transparent; not on the Xenos path. GH5's depth of field and motion blur read a 320×180 depth (`k_24_8`) that
the engine makes every other frame: the scene's 1280×720 depth (EDRAM base 720) resolved through a 640×360 4x MSAA
depth surface at the same base (flags `0x14`: the depth, sample 0), a draw that writes that into a 320×180 depth
surface, and a resolve of it. D95 handled color only (depth had not been seen), so the 640×360 resolve read a host
target nothing drew into: the depth of field blurred the whole image, and the motion the game reconstructs from the
depth smeared the models. Found with the trace (the 320×180 texture is resolved only in alternate frames, from
`0x82571D8C`) and the observer (every draw's state matched the device's).

**Results:** GH5's title screen and a Quickplay song, 1720×720 window: the depth of field and the motion blur as on
the Xenos path in the same camera shots of the song (screenshots of both). The pixel taken is one host pixel of each
block at the render scale, not the guest's sample 0: at most a pixel's offset in a depth the game only uses to blur.

**Revisit if:** a game needs a specific sample of such a depth downsample, or resolves depth through a 2x MSAA surface
(1×2 blocks; only 4x seen).

## D113 — A depth target takes the words a color target drew at its EDRAM (2026-10-31, Gustavo)

**Decision:**
- Before a draw uses a single-sampled depth target, or a resolve reads it, `RenderTargets::FindColorOverDepth` looks
  for a k_8_8_8_8 color target of its kind at the same EDRAM base, with its pitch and one sample, drawn after the
  depth target's last write. On the console that draw wrote 32-bit words where the depth surface's are, and the depth
  surface then holds them; here the depth target gets their depth (`CarryColorIntoDepth` in `renderer.cpp`). The
  stencil is not carried.
- In the EDRAM a depth surface's tiles (80 samples wide) have their 40-sample halves swapped from a color surface's,
  so a game writing depth through a color surface swaps them in its shader. Taking the words back
  (`Blitter::Source::kPackedToDepth`) swaps them again, each pixel from the other half of its tile.
- But when the draw into the color target read a depth resolve's words (D64; `Textures::TakePackedRead`, recorded on
  the color target with the resolve's counter), the depth target gets that resolve's own depth instead, pixel for
  pixel (`kDepthToDepth`). At most render scales the game's offset of 40 pixels is not a whole number of host pixels
  (53.75 at 1.34375): its shader reads the words one host pixel off in half of the columns, and the depth taken back
  from them was a pixel off there.
- A fetch of a depth resolve's words as k_8_8_8_8 samples the nearest texel: filtering between texels mixes the bytes
  of different depths, and the game's texel centers fall between the host's at another scale.
- The first time, the log says which way ("the 1280x720 depth at EDRAM base 720 takes the words a 1280x720 k_8_8_8_8
  color target drawn at the same base wrote: the depth of the resolve those words were read from (D113)").

**Why:** Gustavo (2026-10-31), after D112: with NativeXRenderer Guitar Hero 5's band still looked transparent, on the
title screen and in songs; the drum kit too. RenderDoc captures of two frames in a row showed GH5 drawing its scene
over two frames: the first draws the stage and the crowd, resolves the color and the 1280×720 depth (into a `k_24_8`
texture, which a fetch reads as k_8_8_8_8: D64) and shows the last frame's image again; the second clears the color and
the depth, draws one full-screen triangle into a k_8_8_8_8 surface at the depth surface's EDRAM base (720) and the
scene's color surface (RT0 and RT1), from the depth resolve's words and the color resolve, then draws the band over
the depth surface at base 720, and the screen effects. NativeXRenderer kept the color surface at base 720 as a target
of its own: the depth target stayed as the clear left it, and what the second frame drew was tested against that
instead of the first frame's depth (the scene copy the screen effects read already showed the drum kit and the
drummer see-through). Taking the words back without the swap gave the
drum kit bands of wrong depth 40 pixels wide; with the swap, a pixel off in half the columns, still bands on the
cymbals. The Xenos path's render target cache does the same transfer, with the swap, between its EDRAM's color and
depth views.

**Results:** GH5 at 1720×720 (render scale 1.34375 × 1): a Quickplay song's band, drum kit and cymbals solid, as on
the Xenos path in the same camera shots; in a capture of two frames, the depth taken back equal to the first frame's
at all 3,440 pixels of two rows compared. The title screen's drummer and drums solid. One more full-screen depth copy
every other frame.

**Revisit if:** a game writes depth through a color surface at another EDRAM base than the depth surface's, with MSAA,
or in a 64-bit format; or needs its stencil (only the depth is taken); or draws into the color surface from a depth
resolve's words and then changes them (the resolve's depth would be taken instead of the words).

## D114 — Alpha to coverage blended only on surfaces the game draws with MSAA (2026-11-01, Gustavo)

**Decision:** `native_x_renderer_alpha_to_coverage` gets a third value, `auto`, the new default: a draw with alpha to
coverage blends by the alpha (D69) when the guest's surface has MSAA (`RB_SURFACE_INFO.msaa_samples`), and keeps the
translated shader's 2×2 dither when the game draws it at 1x. `blend` (D69 everywhere) and `dither` stay. The log's
line says which.

**Why:** Gustavo (2026-10-31): in GH Metallica James's hair sometimes looked see-through. The same shot of For Whom the
Bell Tolls (career, 1720×720) in three runs: the Xenos path and `dither` drew the hair and the beard's edge solid,
`blend` let the background through them. D69 was made for GRAW, which draws with 4x MSAA in the guest: there alpha to
coverage covers a fraction of the samples and the resolve blends them, which blending by the alpha gives without the
dither's dots. GH Metallica (and Guitar Hero 5) draw their scene at 1x, where the console itself dithers in 2×2 and
the objects look solid at a distance; blending made them translucent where their alpha is below 1.

**Results:** GH Metallica, the same shot: James's hair and beard solid, as on the Xenos path. GRAW (4x MSAA in the
guest) keeps D69's blend.

**Revisit if:** a game draws alpha to coverage at 1x with a pattern that shows (thin geometry dotted at the render
scale): `blend` brings D69 back for it.

## D115 — The image scaled into the window by AMD FSR 1 or NVIDIA Image Scaling, and sharpened (2026-11-03, Gustavo)

**Decision:**
- **Two settings, read at startup.** `native_x_renderer_scaling` (`bilinear`, the default and the old behavior;
  `fsr`; `nis`) is how presenting scales the front buffer into the window when the internal resolution (D54) is
  lower. `native_x_renderer_sharpness` (0 to 1, 0 none; -1, the default, the filter's own) sharpens it, at any
  internal resolution. `src/scaling.{h,cpp}`, called from `PresentFrontBuffer` after the gamma ramp (D36).
- **The filters**, all MIT, vendored under `thirdparty/` unchanged and compiled at startup with glslang (D16):
  - AMD FidelityFX Super Resolution 1 (`thirdparty/fsr1`): EASU, then RCAS. RCAS at 0.2 stops by default (AMD's);
    a sharpness s gives 2 × (1 − s) stops; 0 leaves RCAS out. At the same size, RCAS alone.
  - NVIDIA Image Scaling 1.0.3 (`thirdparty/nis`): NVScaler, a compute shader writing an RGBA16F storage image,
    copied into the window; NVSharpen at the same size. Sharpness 0.5 by default (NVIDIA's). Block and thread
    group sizes from NIS_Config.h's NISOptimizer for the adapter's vendor; its coefficients as two RGBA32F
    textures.
  - With the bilinear stretch, AMD FidelityFX CAS (`thirdparty/cas`) when a sharpness is given: it scales by
    itself up to 4× the pixels; beyond, the image is stretched first.
- **Passes of at most 2× per axis.** EASU and NVScaler are made for up to 2×: a larger ratio (a 720-line image on
  a 4K screen, 3×) is split into as few passes as keep each within it, the ratio shared evenly, sharpening in the
  last. A rectangle smaller than the image (a small window) keeps the stretch: none of them shrinks.
- **Where they read.** The image after the gamma ramp: 16-bit float, the front buffer's swizzle applied, in the
  display's gamma, which is what FSR 1, NIS and CAS expect. HUD included (the game composes it into the front
  buffer); the SDK's overlay is drawn after them. Without the gamma ramp (SDK change missing) the stretch.
- **AMD's constants on the GPU.** FsrEasuCon, FsrRcasCon and CasSetup work on the CPU or the GPU: the shaders call
  them from a small uniform block (input and output sizes, sharpness), so AMD's headers are not compiled as C++.

**Why:** Gustavo (2026-11-03), after reviewing King Kong Recompiled's upscalers (`GHMetalRecomp/local_docs/26`,
`27`): the cheap part first. A lower internal resolution was stretched bilinearly with the HUD (D54's limit), so
text and edges went soft. King Kong does the same in the SDK's presenter (its SDK patch 0008: FSR 1 and CAS built
without AMD's SDK, NIS added); NativeXRenderer presents through its own swap chain, so the filters are its own.

**Results:** GH Metallica, 3440×1440 borderless, internal 1720×720 (2×), the settled main menu (RTX 4070 Ti, 60
FPS; scratchpad `scaling/`):
- **Sharpness of the menu's text** (mean absolute Laplacian of the same crop; the four images aligned to the pixel):
  bilinear 7.9, CAS 0.6 9.7, FSR 1 16.4, NIS 16.0.
- **GPU per frame** (the renderer's timer): bilinear 0.65 ms, CAS 0.69, FSR 1 0.79, NIS 0.85.
- **Checked:** 16:9 with bars (1280×720 into a 2560×1440 rectangle at an offset, a song with FSR 1: highway, band
  and HUD right), the same size (NVSharpen; RCAS alone), the log's line per size ("scaling: 1720x720 into 3440x1440
  with AMD FSR 1: 1 EASU pass, RCAS").
- **Not seen in a game:** the passes beyond 2× (no 4K screen here; the internal resolution cannot go below 720
  lines).

**Revisit if:** a game composes its HUD apart from the 3D image at a different size (the filters would then belong
on the 3D image only); an HDR output (FSR 1 and NIS here assume SDR).

## D116 — AMD FSR 3.1 beside DLSS: its native AA and quality modes, on any GPU (2026-11-03, Gustavo)

**Decision:**
- **A second provider for the temporal upscaler** (D47-D49). `native_x_renderer_antialiasing` takes `fsr_native`
  (FSR 3.1 at the render resolution, in place of DLAA) and `fsr_quality`, `fsr_balanced`, `fsr_performance`,
  `fsr_ultra_performance` (experimental, as the DLSS modes, D53). The same view hooks (`BeginView`,
  `EndViewGeometry`), jitter (through the viewport), motion vectors (the camera's and D48's objects'), reduced
  targets and place in the frame; `Upscaler::Provider`, `src/fsr.{h,cpp}`.
- **AMD's FidelityFX API, not DiligentCore.** DiligentCore's SuperResolution module has no temporal FSR. The API's
  headers are vendored (`thirdparty/ffx_api`, MIT); AMD's signed `amd_fidelityfx_vk.dll` (FidelityFX SDK 1.1.4, FSR
  3.1.4, the last SDK with a Vulkan library) is fetched when configuring, checked by its SHA-256, copied next to the
  game's executable (`native_x_renderer_copy_runtime`) and loaded at run time. Without it, FSR is reported missing and
  SMAA is used.
- **On DiligentCore's command buffer.** FSR records into `IDeviceContextVk::GetVkCommandBuffer()`. The color, the depth
  and the motion vectors are put in DiligentCore's shader resource state and the output in unordered access, and FSR
  is told so: its Vulkan backend returns every external resource to the state it was given (`UnregisterResourcesVK`),
  so DiligentCore's tracking stays true. Then the submission and `InvalidateState`, as for NGX (D47).
- **Its inputs.** The depth copied to R32_FLOAT (FSR makes its own image views; a depth-stencil image would need its
  depth aspect), brought to 0 at the near plane and 1 at the far one with the view's depth range, as the motion
  vectors read it. The motion vectors and the jitter as DLSS takes them (input pixels, scale 1). HDR with auto
  exposure for a float scene, gamma-encoded LDR otherwise. The camera's near and far planes and vertical field of
  view, for FSR's heuristics, found in the view-projection matrix (clip z = a × view z + b). AMD's input sizes (ratios
  1, 1.5, 1.7, 2, 3) and jitter phase count (8 × the ratio squared).
- **The device.** With an `fsr` mode the Vulkan device enables 16-bit float shaders where the GPU has them
  (`ShaderFloat16`): FidelityFX picks its 16-bit shaders by what the GPU supports, not by what the device enabled.
  Other modes create the device as before.
- **The loader's functions with a fallback.** FidelityFX looks some functions up by their extension's name
  (`vkGetBufferMemoryRequirements2KHR`) and calls them unchecked; the core function of the same name is given where
  the device did not enable the extension (King Kong Recompiled found this).

**Why:** Gustavo (2026-11-03), from the review of King Kong Recompiled (`GHMetalRecomp/local_docs/26`, `27`): DLSS runs
only on NVIDIA RTX; FSR 3.1 gives AMD, Intel and GTX players the same antialiasing and upscaling. FSR 4 (King Kong's
other AMD option) is Direct3D 12 only.

**Results:** POTCAWE, Beckett's dock, 3440×1440 borderless (render scale 2.6875 × 2, shadows at 4), 60 FPS, RTX 4070
Ti; GPU load and power from nvidia-smi over 10 s, the renderer's GPU time (with the mid-frame submission, D47; good
for comparing the two providers). POTCAWE's submodule was pointed at a local build for the runs and put back after.

| | GPU | Power | Renderer's GPU time |
|---|---|---|---|
| SMAA | 33.5 % | 75 W | 4.45 ms |
| DLAA | | | 6.15 ms |
| FSR 3.1 Native AA | 41.6 % | 113 W | 6.05 ms |
| DLSS Quality (2293×960) | 37.1 % | 85 W | 5.7 ms |
| FSR 3.1 Quality (2293×960) | 33.9 % | 78 W | 5.35 ms |

- **Images:** FSR 3.1 Native AA as clean as DLAA (the palm fronds, the barrels' edges); FSR 3.1 Quality softer than
  DLSS Quality, as expected of FSR. No warning or error from FidelityFX or DiligentCore. The log names AMD's provider
  ("FSR 3.1: AMD's 3.1.4 upscaler, 2293x960 to 3440x1440, HDR").
- **Not checked:** an AMD or Intel GPU (none here); FSR's own sharpening (off: the presenting's sharpening, D115,
  comes after the HUD instead).

**Revisit if:** an AMD GPU shows FSR's 64-wide shaders failing (FidelityFX asks for a required subgroup size where the
GPU offers it, and DiligentCore does not enable `subgroupSizeControl`); a game's post-processing reads the scene's
alpha (FSR leaves the output's alpha empty; King Kong copied the input's alpha back).

## D117 — The camera's motion vectors through one reprojection matrix made in double precision (2026-11-03, Gustavo)

**Decision:** the camera's motion vector pass (D47) takes one matrix, this frame's clip position to the last frame's:
this frame's view-projection matrix inverted, then the last frame's, multiplied on the CPU in double precision
(`MotionConstants::reprojection`). Before, the pixel shader applied the two in turn in single precision (the inverse
to a position in the world, divided by its w, then the last frame's matrix). After a reset the matrix is the identity.

**Why:** Gustavo (2026-11-03) found DLSS Quality softer than SMAA and FSR 3.1 Quality in D116's comparison images.
POTCAWE's dock with a temporary probe (not committed), DLAA: jitter given to NGX as it was, negated, or zero, the
motion vectors cleared, and D48's objects left out. Cleared motion vectors doubled the texture detail and steadied
the image; the objects' pass changed nothing; zero jitter smeared the rows (DLSS needs it). The camera was still (1959
of 2400 frames with the same matrix, bit for bit), so the camera's motion should have been zero. It was not: the way
to the world and back, in single precision, with a reversed depth near 0 for the far pixels (a far plane far away),
left each pixel a fraction of a pixel off, at random from frame to frame. DLSS and FSR took that for motion: they
moved their history by it and blurred, and DLSS's edges came out toothed. With the matrix made once in double, the
same matrix on both frames gives the identity, and no motion. FSR read the same vectors, so it gains as much.

**Results:** POTCAWE, Beckett's dock, 3440×1440 borderless, the still camera after the route. The variance of the
Laplacian on three 220×140 crops (a barrel, the town, the tower's wall; higher is sharper), and the wall's mean change
between two screenshots 1 s apart (0-255; lower is steadier). Before is the D116 build, after this one; SMAA for
reference: barrel 833, town 134, wall 696.

| | Barrel | Town | Wall | Wall's change |
|---|---|---|---|---|
| DLAA | 263 → 549 | 38 → 73 | 274 → 513 | 1.51 → 0.14 |
| DLSS Quality | 463 → 551 | 54 → 75 | 383 → 368 | 1.25 → 0.11 |
| FSR 3.1 Native AA | 313 → 805 | 59 → 110 | 390 → 820 | 0.96 → 0.08 |
| FSR 3.1 Quality | 164 → 545 | 36 → 81 | 226 → 445 | 1.20 → 0.15 |

- **In motion:** the camera turned with the right stick, screenshots while it turns: DLAA's edges (the pillar, the palm
  fronds) clean, no double image or trail, smoother than SMAA's.
- **D116's "FSR 3.1 Quality softer than DLSS Quality"** came from these motion vectors: FSR suffered more from them.

**Revisit if:** a game's still camera gives a matrix that changes by tiny amounts every frame (an idle sway too small
to see) and DLSS or FSR blur there: that is real motion and is passed on; a threshold under which it counts as none
would be the next step.

## D118 — The camera's reprojection by a vote of the main view's draws (2026-11-03, Gustavo)

**Decision:** the temporal upscalers' camera motion (D47, D117) can come from the draws instead of the game's view hook
(`native_x_renderer_camera_vote`, off by default): King Kong Recompiled's method (its `CameraReprojectionVote`, MIT;
`src/camera_vote.{h,cpp}`).
- Each draw of the main 3D view that writes depth and is matched with its last-frame self (D48's key and closest
  constants; `ObjectMotion::Match` now also gives the matched constants when the draw did not move) looks for a
  perspective matrix in its vertex shader constants: the first four consecutive registers whose clip z and w rows are
  almost parallel (z = k w, 0.8 < k < 1.25, within 5 % of w), read as the rows of clip = M x position (each register a
  dp4: King Kong's games) or transposed (each register an input axis of position x M: POTCAWE's c0-c3).
- It offers last frame's M times the inverse of this frame's (double precision, normalized to r[15] = 1): this frame's
  clip space to the last frame's, the world cancelling out for whatever did not move on its own. Offers within 2e-3 of
  their scale are counted together (16 candidates at most; consecutive identical offers join without another
  inversion); the most voted one, its members' mean, is the camera's, and replaces the reprojection D117 makes from
  the hook's matrix (its transpose: the motion pass takes rows for a row vector).
- Where the hook's matrix is there too, Upscaler::Apply measures the difference: the largest distance, in output
  pixels, between where the two put points across the screen and the depth range (z 0.5 to 0.9999). The periodic log
  line says how many views voted, the offers and votes, and that difference.

**Why:** Gustavo (2026-11-03): the upscalers in the games without view hooks (GH Metallica first), compared in POTCAWE
with what the hook gives, "to see that it loses no quality". This is the camera half of the plan
(`GHMetalRecomp/local_docs/27-upscalers-plan.md`, step 3); the view's start and its upscale point still come from the
hooks.

**Results:** POTCAWE, Beckett's dock, 3440x1440, DLAA with the vote: about 990 offers per view, all for the winner
(every draw's first perspective matrix is the shared c0-c3, the view-projection the hook gives); its difference from the
hook's reprojection 0.000 px average and max over 600-frame windows, the camera still and turning (the right stick held
5 s). The image as with the hook: Laplacian variance barrel 547 / town 72 / wall 504 against 549 / 73 / 513 (D117's
run; run-to-run noise).

**Revisit if:** a game's first perspective matrix is not the camera's for most draws (a light's, a shadow's, a
previous-frame copy for motion blur): the vote then needs the register chosen by more than the first match; a game
with a reversed-depth projection (k near 0) is not found by the test.

## D119 — The main 3D view found from the draws, up to an upscale point the profile names (2026-11-03, Gustavo)

**Decision:** DLSS and FSR 3.1 for a game without view hooks (D47's BeginView / EndViewGeometry). `Profile::view_hooks`
says whether the game marks its views (POTCAWE: true); otherwise, with `Profile::upscale_resolve_caller`:
- **The view's start:** the first draw into a target of the output size that writes depth with a perspective matrix in
  its vertex constants (the camera vote's test, D118). It and the next draws of that size are the main view's: their
  color and depth targets kept for the upscaler, jittered, matched for D48 and offered to the vote (which is on).
- **Its end:** the first copy of the output-size scene color whose Resolve returns to `upscale_resolve_caller` (the
  command now keeps the call's return address): DLSS or FSR 3.1 run on the view before it, so the copy takes their
  image. No view again until Present (GH Metallica's note highway, drawn after that copy with its own camera, stays
  out: no jitter, no history for the notes).
- **The jitter only after a frame whose view reached that point** (King Kong Recompiled's rule): a 3D scene that never
  gets there (GH Metallica's loading screen) is not shaken.
- `native_x_renderer_views`: hooks, auto (to compare where there are hooks), or empty for the profile's choice. A game
  with neither hooks nor an upscale point gets SMAA (DLSS would never run). The upscaling modes' reduced targets are
  chosen before a view's first draw, which the draws tell only once drawn: those modes need hooks for now (native AA
  instead, logged).

**Why:** Gustavo (2026-11-03): the camera vote first in POTCAWE, comparing what the hooks give, then GH Metallica. The
upscale point cannot be found by a rule alone: POTCAWE copies its scene in the middle of the view (0x823FB79C, for the
water) before the post-processing's copy (0x823FA2A4, a function every copy goes through, told apart by size and
order); GH Metallica's scene is first copied after its in-scene effects, the copy SMAA uses (0x8251F04C). The profile
names it, as it names the scene resolve.

**Results:**
- POTCAWE, `native_x_renderer_views = auto` (its hooks not followed; their matrix kept only for the log): DLAA on every
  frame of the dock, the vote 0.000 px from the hooks' reprojection, D48 matching the same draws (1003, 154 moved);
  Laplacian variance barrel 549 / town 72 / wall 510 against the hooks' 549 / 73 / 513; turning the camera, clean.
- GH Metallica, a song at 3440x1440, DLAA: applied on 600 of 600 frames (the menus' 3D background too); about 300
  offers per view, 98 % for the winner; D48 finds 287 of 288 draws moved (each object has its own world-view-projection
  and the camera always moves), so it draws their motion. GPU 2.88 ms (SMAA) -> 3.96 ms a frame; the renderer's CPU
  5.2-6.0 -> 8.7-9.5 ms, the motion draws: a draw whose own reprojection is the camera's needs none (next).

**Revisit if:** a game's first output-size depth-writing perspective draw is not its scene (a 3D HUD drawn first);
a game copies its scene through the upscale point's caller more than once before its post-processing.

## D120 — Draws that moved with the camera only get no motion draw (2026-11-03, Gustavo)

**Decision:** with the camera vote on (D118), a main-view draw that D48 finds moved is left to the camera's motion,
with no motion draw, when:
- its constants changed only in its perspective matrix (the vote's) and in the registers the view shares: not in
  bones or other registers of its own (`ObjectMotion::ChangedOnlyIn`);
- and that matrix's reprojection (last clip from this clip, which `CameraVote::Offer` now returns) is the camera's
  within 0.1 pixel of the game's frame (`CameraVote::DistancePx`, the largest distance at 45 points across the
  screen and the depth range, the one the log already used to compare the vote with the view hooks').

The camera's reprojection is the vote's, which is only known at the view's end: during the frame the leader stands for
it once 8 draws agree and they are half the offers or more; the draws kept before that (the frame's first ones) carry
their reprojection and are dropped at DrawObjectMotion if it is the winner's.

The constants kept for D48 and the vote are copied from the register file instead of from the mapped constant buffer
just written: that buffer is write-combined memory, and reading it back was a sixth of the renderer thread's time
with DLAA in a GH Metallica song.

**Why:** Gustavo (2026-11-03), the cost of D119's DLAA in GH Metallica: every object there has its own
world-view-projection and the camera never stops, so D48 found nearly every draw moved (287 of 288) and drew each one
again for its motion: +3.5 ms of renderer CPU a frame, and 160 FPS lost. Those draws moved only as the camera did; the
depth already gives their motion.

**Results** (GH Metallica, a song at 3440x1440, 160 FPS, V-Sync; the frame interval and the renderer's CPU per 600
frames; the route samples the threads, which costs a little):
- Motion draws 287-306 -> 3-8 a frame (14-28 in a venue with more draws; up to 285 in a frame whose vote has no
  leader in time); 160-310 a frame left to the camera's motion.
- DLAA: frame interval 8.8-9.7 -> 6.50-6.77 ms, renderer CPU 8.2-8.8 -> 5.82-6.05 ms; SMAA on the same route
  6.25 ms, 4.83-4.88 ms. Without sampling, in a busier venue: DLAA 6.9-8.2 ms (renderer 6.3-7.5), SMAA 6.25-7.07 ms
  (renderer 4.3-6.0).
- DLAA's own cost left, from the sampled renderer thread: D48's matching 2.2 %, the upscaler's submit and NGX's
  evaluation 1.8 %, the vote's distances 0.9 %, its offers 0.7 % (about 0.4 ms a frame at 160 FPS).
- POTCAWE with its view hooks: the vote is off by default there, so nothing is skipped; only the constants' copy changes.

**Revisit if:** a game moves an object with its camera in a way the matrix alone does not tell (constants that only
look shared); draws that bend vertices by a value outside the registers (time in a vertex fetch), whose motion the
matrix does not show.

## D121 — The upscaling modes without view hooks: reduced targets from the frame's start (2026-11-03, Gustavo)

**Decision:** with a view found from the draws (D119) and an upscaling mode (DLSS or FSR 3.1 Quality to Ultra
Performance, D49), the targets of the output size are the reduced ones from the frame's start when the last frame's
view reached the upscale point (King Kong Recompiled's rule), until that point; then the render scale's, as after
EndViewGeometry. The view itself (its jitter, its targets for the upscaler) still starts at its first perspective
draw. Also:
- At the upscale point the upscaler runs before the copy's source is chosen, so the copy reads its image at the render
  scale (before, the reduced target, without the upscaler's image).
- A view drawn at the render scale (the first after a frame without one) is left as drawn, its vote discarded: the
  upscaler takes the reduced size.
- A frame that does not reach the point has its reduced targets brought back at Present (D49's case of a view
  without EndViewGeometry): one frame scaled up bilinearly, then none reduced.
- `RenderTargets::FindMsaaAlias` looks at the reduced targets too: GH Metallica resolves its 1280x720 scene through a
  640x360 4x MSAA surface (D95) inside the view, which then read the last frame's scene at the render scale.

**Why:** Gustavo (2026-11-03), after D120: the upscaling modes for GH Metallica, whose views are not hooked. The
reduced targets must be chosen before the view's clears, and the draws tell the view only once drawn.

**Results:** GH Metallica, a song at 3440x1440 and 160 FPS: DLSS Quality draws the view at 2293x960, FSR 3.1 Quality
too; the band, the stage and the crowd clean, the note highway and the HUD sharp (drawn after the point). GPU per
frame in the same venue: SMAA 2.5-3.3 ms, DLAA 3.95-4.23, DLSS Quality 3.67-3.92, FSR 3.1 Quality 3.22-3.56 (the game
is light; the upscaler's own cost at 3440x1440 is most of it). The renderer's CPU is the same as DLAA's (D120).
POTCAWE with DLSS Quality: the same with its hooks and with `native_x_renderer_views = auto` (Laplacian variance barrel
542 / 547, town 73 / 73, wall 370 / 369; D117's 551 / 75 / 368).

**Revisit if:** a game draws a 2D background into its output-size target before the scene (drawn at the reduced
scale now); a game alternates frames with and without a view (each view after one without is left unscaled).

## D122 — DLSS's CPU cost cut again: no submission before the upscaler, cheaper matching (2026-11-03, Gustavo)

**Decision:** five cuts to what DLSS / FSR 3.1 add to the renderer thread:
- **No submission before the upscaler.** `Upscaler::Apply` flushed the context before `InvalidateState` because
  DiligentCore's `InvalidateState` dropped the command buffer. DiligentCore patch 0010: `InvalidateState` keeps the
  command buffer, its pending barriers, its command count and the open queries' counts, and forgets only the cached
  bindings (pipelines, descriptor sets, vertex and index buffers, render pass); after a Flush it does what it did.
  The frame stays one submission.
- **The vote's comparison** (`Close`) took the largest value through an initializer list, which became a vectorized
  min/max per element: plain `std::max`.
- **The draw's camera test** (D120) is `CameraVote::WithinPx`: 15 points (the corners at +-0.75 and the centre, at z
  0.5, 0.99, 0.9999), squared distances, the first beyond 0.1 px ending it; `DistancePx` (45 points) stays for the
  log's comparison with the view hooks.
- **`RenderTargets::FindColorOverDepth`** (D113, asked by every draw with depth, with or without DLSS) walked every
  target: the k_8_8_8_8 single-sampled color targets at the depth's base and pitch are one run of the ordered map,
  found with `lower_bound`.
- **`ObjectMotion::Match`** compares a draw's constants as runs of registers the view does not share (one `memcmp`
  each instead of one per register), skips the distance when its key has one entry last frame (it is that object
  however far), skips the view's comparison of a register already found mixed, and finds the last frame's entries in
  a sorted (key, entry) index made at the frame's end instead of a hash map that allocated a node per draw.
  `ChangedOnlyIn` uses the same runs.

**Why:** Gustavo (2026-11-03): DLAA's renderer CPU in GH Metallica's busiest venue kept it under 160 FPS. Sampled
there, the DLAA-only functions were the flush and submission 1.7 % of the renderer thread, the vote's min/max 0.7 %,
the distances 1.1 %, the color-over-depth search 1.1 % (0.7 % with SMAA), the matching 1.9 %.

**Results** (GH Metallica, the busiest venue of the test route, 3440x1440, 160 FPS, V-Sync, sampled; the log's
renderer CPU per 600 frames from frame 21000, averaged): DLAA 5.69 -> 5.26-5.33 ms, frame interval 6.56 -> 6.40-6.44
ms; SMAA 4.52 ms, 6.27 ms. In the samples the flush, the min/max and the color-over-depth search are gone, the vote's
offers 1.4 -> 0.3-0.6 %; the matching stayed at 2-3 % (it depends on the scene's draws). DLAA's own CPU left is about
0.4 ms a frame: NGX's evaluation (inside NVIDIA's code, 1.8-2.8 %), the matching, the motion draws.
The GPU side, measured with timestamps around the upscaler's passes (a temporary probe): the camera's motion vectors
0.03 ms, the objects' 0.01 ms, the output's copy 0.035 ms; DLSS itself 1.21-1.25 ms for DLAA (preset K) and
1.54-1.58 ms for Performance (preset M, 1720x720 in) at 3440x1440, against SMAA's 0.16-0.18 ms. The game's whole 3D
view costs less than that, so no DLSS mode draws GH Metallica for less GPU time than SMAA.
On this PC the NVIDIA App's global DLSS override was on: NGX loaded its own DLSS (310.9.16, not the 310.5.3 the game
ships) and its presets (`DRS Overridden Preset`: K for DLAA, Quality and Balanced, M for Performance, L for Ultra
Performance), whatever the application asks: a render preset option (tried: DiligentCore's DLSS creation setting
NGX's `DLSS.Hint.Render.Preset.*`) changed nothing here, and was left out.

**Revisit if:** another library records into the command buffer and changes graphics state the context does not
rebind (vertex buffers are rebound, dynamic viewport and scissor are set by the next draw).

## D123 — Frame generation: AMD FSR 3.1's frame interpolation, without its swap chain (2026-11-03, Gustavo)

**Decision:** `native_x_renderer_frame_generation` (off by default): a frame between each two of the game's, presented
before the game's, through the FidelityFX API's frame generation (the `amd_fidelityfx_vk.dll` FSR 3.1 already loads,
SDK 1.1.4, its `ffx_framegeneration.h` vendored), `src/framegen.{h,cpp}`:
- **Without AMD's frame interpolation swap chain.** FidelityFX offers one that replaces the application's swap chain
  (its own present thread and pacing); DiligentCore owns ours. Its frame generation context also runs alone: with
  `FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY` it only interpolates, into a texture of ours, on our command
  buffer (the provider's source, `ffx_provider_framegeneration.cpp`, checked).
- **Its inputs are the temporal upscaler's** (D47-D49): after DLSS or FSR 3.1, the view's depth (the R32_FLOAT copy FSR
  takes, now made for DLSS too), its motion vectors, the jitter and the camera go through FidelityFX's prepare pass.
  Frame generation needs DLSS or FSR 3.1 on (logged otherwise).
- **At Present** the frame is composed into an image of its own (`ComposePicture`, the letterbox, the guest's gamma
  ramp, FSR 1 / NIS / CAS as before), the frame between the last one and it is generated, and both are copied into the
  swap chain's back buffer and presented, the generated one first, the SDK's dialogs drawn over each.
- **The HUD:** the scene before it, the profile's `scene_resolve_caller` (SMAA's point, D41), is copied when it is
  resolved and composed the same way; FidelityFX takes the HUD from where the frame differs from it and does not move
  it with the scene.
- **V-Sync is on while it is** (`Device::ForceVsync`): FIFO shows the two a refresh apart. Without it, MAILBOX
  replaced the generated frame with the game's; waiting half a game frame on the renderer's thread between them made
  frames come 60 ms apart now and then.
- Not with the guest's swaps (D86) for now. The game must run at half the display's rate.

**Why:** Gustavo (2026-11-03): DLSS cannot raise the frame rate where the renderer's CPU is the limit (D122); frame
generation does. AMD's first: it allows V-Sync on Vulkan (NVIDIA's frame generation does not, Streamline's DLSS-G
guide) and runs on any recent GPU. POTCAWE first (its HUD almost still), GH Metallica next (its note highway).

**Results** (POTCAWE, Beckett's dock, 3440x1440, DLAA, `potcawe_60fps`, a 160 Hz display):
- Every game frame gets one: 600 generated in 600; presents 8.33 ms apart (RTSS: 120 FPS), the longest 18 ms (as
  without frame generation); the renderer's thread 4.0 ms per present (two a game frame; 7.1 ms a game frame without).
- The generated frames, shown alone (a temporary probe): the dock, the water and Jack turning clean; the HUD smeared
  along the camera's turn without the HUD-less picture, clean with it.
- The HUD-less copy as a transfer (`CopyTexture`) made frames come 60 ms apart every few seconds (bisected with probes:
  the copy, not the composition nor FidelityFX); drawn with the blitter, as the renderer's other copies, it does not.
  Why is not known.
- At 120 FPS on a 160 Hz display FIFO cannot space the frames evenly; a 120 Hz mode (or a game at 80) can.

**Revisit if:** a game's HUD is not drawn after its scene resolve; the display's rate is not twice the game's; the
guest's swaps are used (D86).

## D124 — Frame generation: the motion of the 2D layer drawn after the view (2026-11-03, Gustavo)

**Decision:** `Profile::overlay_motion` (`src/overlay_motion.{h,cpp}`): in a game whose 2D layer after the main 3D view
is drawn with inline vertices whose first two floats are their position in pixels of the output canvas, frame
generation (D123) gets that layer's motion from its vertices:
- each such draw after the view's end (an inline draw, a quad or triangle list) is matched with the last frame's draw of
  the same kind (vertex and pixel shaders, vertex count, stride, primitive type) whose center is closest, within 64
  canvas pixels; its vertices' movement since then is drawn at Present into the view's motion vectors, over the scene's,
  with the near plane's depth into the depth copy (FidelityFX dilates the motion by the nearest depth). A draw without
  a match stands still; the HUD, standing still, keeps its place. The HUD-less picture is not used then.
- `Profile::overlay_motion_adjust`: a game callback, on the renderer's thread, with the frame's draws (vertex shader
  hash, positions, last positions, matched), for movement the vertices do not show.
- Frame generation's prepare pass moved to Present (the view's inputs kept from its end), after that motion is drawn.
- A view found from the draws (D119) has no view matrix: the camera's near and far planes and field of view for
  FidelityFX come from the world-view-projection of the view's first draw (`DrawPath::scene_start_matrix`;
  `CameraFromViewProjection`, now public: a world transform leaves them as they are). GH Metallica: near 0.1, far 400,
  44.5 degrees (the defaults were 0.1, 1000, 57).

GH Metallica's profile: `overlay_motion`; its adjustment (`src/native_x_renderer_ghmetallica.cpp`) moves the note
highway's board: its 1600-vertex grid and its texture coordinates stand still while the texture it shows (drawn each
frame into a 768x1024 target) scrolls, so the beat lines (matched, moving with that texture) give, at each height, where
the board's surface was a frame ago, and how far it spread from the highway's middle.

**Why:** Gustavo (2026-11-03): in GH Metallica the note highway must be interpolated as the scene is. A song frame's
trace: after the view, the bloom composite (a rectangle list in clip space) and 63-80 inline draws into the output: the
board's grid, the strings, the beat lines, the notes, the strike buttons and the HUD, all in pixels of the 1280x720
canvas (a 522-vertex strip placed by its shader, the sustain tails, is not: left out, it takes the board's motion
under it, which is its own). Logged over three frames (a temporary probe): beat lines and notes 0.6 to 9 pixels lower
each frame, in the same draw order; strike buttons, strings and HUD still; the board's positions and texture
coordinates the same.

**Results** (GH Metallica, a song at 3440x1440, DLAA, `ghmetallica_frame_rate = 80`, V-Sync on the 160 Hz display):
- 600 frames generated in 600; ~64 2D draws a frame, 99.9 % matched.
- The last real frame, the generated one and this one dumped at the same moment (a temporary probe): beat lines, notes,
  strike buttons, the board's art and the HUD where they belong in the generated frame. Without this, the board's art
  was a blend of the two frames and the beat lines doubled (taking the stage's motion), and with the scene resolve as
  the HUD-less picture (before the bloom composite) the whole stage was blurred and blocky.
- Where a still string crosses a moving beat line, the line's pixels under it keep the string's (no) motion: a gap of
  a few pixels in the generated frame.
- 159 FPS shown (6.29 ms average), the renderer's thread 3.68 ms per present (7.4 per game frame), GPU 3.13 ms per
  present (frame generation about 2.3 ms of it at 3440x1440), 54 % busy and 134 W; DLAA at 160 without it 157 FPS,
  5.36 ms a frame, 66 %, 156 W; at 80 without it 33 %, 85 W.

**Revisit if:** a game's 2D layer is not in canvas pixels or is drawn from vertex buffers; draws of one kind come
closer than they move in a frame; GH Metallica's vertex shaders change (the adjustment knows the board's and the
beat lines' by their hashes).

## D125 — A minimized window's frames are still ended (2026-11-04, Gustavo)

**Decision:** while the window has no area (`Device::UpdateSize` false: minimized, as Alt+Tab from exclusive fullscreen
leaves it), `ExecutePresent` ends the frame without showing it: `Device::FinishHiddenFrame` signals the frame's fence,
submits the work recorded, and calls the immediate context's `FinishFrame` and the device's `ReleaseStaleResources` (what
DiligentCore's `SwapChainVkImpl::Present` does for a minimized swap chain), then waits for the frame
`kMaxFramesInFlight` back as `Present` does.

**Why:** Gustavo (2026-11-04): GH Metallica closed three times when he pressed Alt+Tab in a song (exclusive fullscreen,
3840x2160, 120 FPS). Its logs: from the Alt+Tab on, a 16 MB host-visible page created every 30-80 ms, ~940 MB in two
seconds, then "Space in dynamic heap is exhausted!" and an access violation in memcpy (VCRUNTIME140, WER). The present
command returned before anything when `UpdateSize` failed, so the frames went on being drawn (the game keeps running)
but none was submitted or finished: DiligentCore releases a frame's dynamic memory and its uploads' staging only at the
end of a frame.

**Results:** the game minimized for 15 s on a hidden desktop (WM_SYSCOMMAND SC_MINIMIZE, then SC_RESTORE; the title
screen, which uploads far less than a song): host-visible pages up to 464 MB before, released only at the restore;
16 MB after (one page), the frames counted while minimized, the game drawing again at the restore. Not seen in a song
yet (Gustavo's test).

## D126 — 16-bit fixed-point color targets keep the Xenos' -32 to 32 range (2026-11-05, Claude for Battlefield 1943)

**Decision:** k_16_16 and k_16_16_16_16 render targets stay RG16_SNORM and RGBA16_SNORM, holding a 32nd of the
guest's value: a draw's color exponent bias gets -5 for them, a clear of one divides its color by 32, and a resolve
from one multiplies by 32 (exponent bias +5) unless its destination format is k_16_16 or k_16_16_16_16 too, whose
texels then hold the bits the Xenos' resolve writes. This is the SDK's `snorm16_render_target_full_range`.

**Why:** truncated to -1 to 1, HDR values clipped at 1. Battlefield 1943 draws its sky and haze at half resolution
into a k_16_16_16_16 target, resolves it to a 16_16_16_16_FLOAT texture and composites it into the scene: the
horizon band was white before the post pass (grey after its grade, (149, 145, 150) where D3D12 shows (160, 215,
233)). After: (158, 213, 231), and the sky above as before (run nat23 against runs\ref2).

## D127 — A resolve kept over memory the game released is not kept (2026-11-05, Claude for Battlefield 1943)

**Decision:** a resolved texture whose memory is reported allocated, released, reprotected or read into (an exact
range) with its data unchanged is no longer marked a resolve's again at once (D74). It is unwatched and hashed again
two frames later (`ResolvedTextures::RecheckKept`, from `EndFrame`): kept and watched again only if its memory is
then allocated for writes in one of the views of physical memory (0xA0000000, 0xC0000000, 0xE0000000) and its data
is unchanged; otherwise invalidated. A texture's "data unchanged" check (D35 for textures) hashes what the mirror
holds (`GuestMemory::MirrorHash`, from the hashes of its pages' last copies) instead of guest memory.

**Why:** the SDK reports a release before it happens, and it watches a view's pages only where that view has them
allocated. Battlefield 1943 releases its loading screen's memory, where it had resolved a frame, allocates it again
and copies the soldier's textures in through the 0xE0000000 view. Marked a resolve's again while released, the
pages were watched nowhere and valid in the mirror: the copy was never seen and the arm and hands drew black (run
nat22 after: as D3D12). Hashing guest memory, a texture could also take new data's hash with the mirror's old data,
and every later load found it unchanged.

## D128 — Offscreen draws wait for their pipelines only into new targets; elsewhere within a budget (2026-11-06, Claude for Battlefield 1943)

**Decision:** D101's wait stays for a draw into an offscreen target created in the last 2 frames (up to 3 s, so a
pass drawn once is not left undefined). A draw into an older offscreen target waits only while the frame's waits
together stay under `native_x_renderer_pipeline_wait_budget_ms` (2 ms), then is skipped: the target keeps its last
frame's image. 0 restores D101 for every offscreen draw. With 16 hardware threads or more, 4 translation workers
(2 before) and 6 DiligentCore pipeline compilation threads (4 before).

**Why:** TekRantGaming's first watched play test (runs\playtest-20261106-001025, 9 minutes, three rounds): 130 of the 148
renderer hitches over 25 ms were D101 waits, 22.9 s of the 23.8 s of hitch time, up to 1.1 s for one draw and 6.8 s
in one 10-second window. Battlefield 1943 draws its shadow maps, half-resolution sky and other passes into offscreen
targets every frame, and needed 1,368 new pipelines in the session. On a cold storage at the Wake Island spawn
(runs waitnew2 against waitold2): 5 hitches and 138 ms of waits against 11 and 489 ms.

## D129 — A recorded draw keeps its surfaces as they were when it was recorded (2026-11-06, Claude for Battlefield 1943)

**Decision:** the registry of draws recorded on recording devices keeps each surface's fields (surface info, color
or depth info, size) as read when the draw was recorded, not the surface object's address; a recording drops the
older ones its range overlaps; and a surface wider or taller than 8192 is ignored with a warning (its draws are
dropped).

**Why:** in the same play test, from 00:19:02 while TekRantGaming flew, replayed draws read surface objects the game had
released: render targets of 28290x73731 and the like, an 8 GB device memory page (a 1 s freeze), 40 out of memory
errors and 67 GB allocated by DiligentCore's memory manager.

## D130 — 16-bit fixed-point color targets are half floats (2026-11-06, Claude for Battlefield 1943; replaces D126)

**Decision:** k_16_16 and k_16_16_16_16 render targets are RG16_FLOAT and RGBA16_FLOAT on the host and hold the
guest's values as they are: no exponent bias in draws, clears as given. A resolve from one into a k_16_16 or
k_16_16_16_16 texture takes a 32nd of the values (exponent bias -5), whose bits then read as the Xenos' would.

**Why:** D126 divided every channel by 32 into SNORM, alpha too, as the SDK's snorm16_render_target_full_range
does ("losing blending correctness"). Blending with source or destination alpha was then wrong. In TekRantGaming's play test,
flying through clouds, the sky showed black and white blocks; the clouds are particles blended into Battlefield
1943's half-resolution k_16_16_16_16 target. Half floats hold the -32 to 32 range and blend exactly. At the Wake
Island spawn the image is as with D126 (run f16a against runs
ef2: whole frame (86.5, 125.6, 154.9) against
(87.1, 125.9, 155.2)). The clouds in flight are not checked yet.

## D131 — A replayed draw renders into the targets bound on the device that runs the buffer (2026-11-06, Claude for Battlefield 1943; replaces part of D129)

**Decision:** a draw replayed from a recorded command buffer (RunCommandBuffer) renders into the surfaces bound on
the device that runs the buffer at that moment, as any draw of that device. The recording device's surfaces (D129's
snapshot) are used only when the running device has none bound. `native_x_renderer_replay_recorded_targets` (off)
brings back the snapshot for diagnosis.

**Why:** on the console a recorded buffer holds draws and their state, not the render targets; Battlefield 1943
replays the same recorded draws (trees, buildings) into its shadow maps and into the scene. With the snapshot, the
draws went wherever the recording device happened to point when they were recorded. Twelve frames in a row on
Guadalcanal (run gcrd1): frame 8928 drew 61 of the scene's draws into the 1024x1024 shadow depth target (898 scene
draws instead of 959, 803 shadow draws instead of 742), frame 8931 lost 67 shadow casters. In play the trees lost
their leaves for a frame (run gc1, one shot in four) and the shadows switched off and on (TekRantGaming's sixth test, his
videos and marked clips). After (run gcrd2): 968 to 971 scene draws and 743 shadow draws in every frame; run gc2:
twelve shots, none missing anything.

## D132 — Ambient occlusion: King Kong Recompiled's GTAO at the profile's end of the opaque draws (2026-11-06, Claude for Battlefield 1943)

**Decision:** XeGTAO as King Kong Recompiled made it (`src/gtao.cpp`, `src/gtao_shaders.h`; port notes in
`docs/gtao_port_notes.md`): a depth prefilter into 4 mips, normals from depth with smoothing, the main pass (Low 2x2,
Medium 3x3, High 9x3 slices and steps), two edge aware denoise passes, half resolution above a render scale of 1.5,
then a multiply into the scene's colour. It runs just before the Resolve the profile names `ao_resolve_caller`, the
output-size depth copy right after the main view's opaque draws, so water, particles and a first person weapon drawn
after it are never darkened. The camera is a vote of the view's depth-writing draws with a perspective matrix
(`DrawPath::TakeProjection`, KK's AoCameraFinder), with that camera's viewport depth range; the radius is KK's 6, 8
or 12 near plane distances from the depth curve's own near plane. Settings: `native_ao_mode` (0, 1, 2 the occlusion
only; on), `native_ao_strength` (1 to 3; 3), `native_ao_samples` (1 to 3; 2), live; KK's tuning flags too. Named
apart from KK's `ao_mode` and `ao_strength`, which the SDK's Xenos plugin also defines (defaults 0 and 1). With no
upscaler the main view is found from the draws (D119) and the camera vote is on.

**Why:** TekRantGaming's King Kong knowledge pack, roadmap item 1. Battlefield 1943 (runs ao5 to ao9, default1): the view's
first draw is the weapon's depth (near 0.1, far 7.5), so the first draw's camera gave flat grey; 1264 of the view's
draws share the world's projection (near 21.34, far 6000), whose viewport range is 0.0047 down to 0: the game stores
0.1 / distance for the weapon and the world alike, and depth read as 0 to 1 again gave flat grey. With the vote and the
range the occlusion shows under the tanks and jeeps and in the towers' corners. Cost at 3200x1800 (half resolution):
1.08 ms Medium, 1.62 ms High on an RTX 4070 Ti; SMAA still on every frame.

## D133 — Global illumination: King Kong Recompiled's SSGI after the ambient occlusion (2026-11-06, Claude for Battlefield 1943)

**Decision:** one bounce of indirect light from the lit scene colour (`src/ssgi.cpp`, `src/ssgi_shaders.h`; port notes
in `docs/ssgi_port_notes.md`), a visibility bitmask gather over GTAO's slices with XeGTAO's depth passes, added to the
scene's colour at the same moment as the ambient occlusion (D132) and with the same camera vote and depth range
(`ApplyOpaqueEffects`). With the AO on, it only adds light (KK's rule). Settings, live: `native_gi_mode` (on),
`native_gi_quality` (Medium), `native_gi_distance` (Medium, 32 m), `native_gi_intensity`, and KK's tuning flags; the
port's own `native_gi_ambient`, `native_gi_gamma` and `native_gi_sunlit` stand for the sun and the ambient light the
game does not tell yet (the pack's sun table is to come).

**Why:** the King Kong knowledge pack, the lighting order's third item. Battlefield 1943 (runs gi0 to gi2, Guadalcanal):
applied on every frame, 1.50 ms at 3200x1800 Medium on an RTX 4070 Ti (KK measured 0.65 to 0.97 ms at 1440p).

## D134 — The stored pipelines' progress at start, for a game's setup screen (2026-11-06, Claude for Battlefield 1943)

**Decision:** `native_x_renderer::StorageProgress(queued, done, loading, finished)`: how many stored pipelines (D42)
there are, how many DiligentCore has finished compiling (each created pipeline's status polled once a frame in
`DrawPath::PublishStoredProgress`, a pipeline that could not be created or whose shader failed counted as done),
whether the storage was read, and whether all are done. Finished at once with no storage, no draw path, or with
NativeXRenderer off. Atomics, read from any thread. The log line "all N stored pipelines compiled M ms after the
first draw" marks the end.

**Why:** Battlefield 1943 now writes the storage on a first start from the player's game files and our list of
translations and pipelines (no shader code shipped), so the first session makes none during play (run shnxr5: 1688
pipelines, 0 created and 0 draws skipped in a Wake Island match on a fresh cache, against 1,368 created and about
31,000 draws skipped in TekRantGaming's first session). Their compilation still takes about 17 s with the driver's own cache
cold (run shnxr3), 1.7 s warm; the game's shader screen (its King Kong style setup page) waits on this so the player
does not meet those 17 s as missing objects in the menus or the first match.

## D135 — Pipelines compile on all but two hardware threads, at normal priority only until the stored ones are made (2026-11-06, Claude for Battlefield 1943)

**Decision:** DiligentCore's compilation pool is ours (`pAsyncShaderCompilationThreadPool`): the hardware's threads
less two, from 4 to 16 (`native_x_renderer_compile_threads`, 0 = that), each thread registered as it starts
(`RegisterCompileThread`). They run at normal priority while the stored pipelines are made at start and below normal
once they are (`SetStoredPipelinesFinished`), so in play the game's own threads come first. With no data in
DiligentCore's pipeline cache at start, the stored pipelines count as unsaved, so the cache is written once they are
compiled (before, only pipelines new to the run counted, and a first start that met none in play never wrote it);
the cache is not saved while any stored pipeline still compiles.

**Why:** Battlefield 1943's first start, 1,688 stored pipelines with the driver's own cache cold (a fresh
`__GL_SHADER_DISK_CACHE_PATH` on NVIDIA), RTX 4070 Ti, 16 threads: 49 s on six threads (D128), 46 s on fourteen below
normal, 24 s on fourteen at normal priority (run first1), the CPU at 100% throughout: it is the driver's compilation.
Warm: 1.4 s (1.7 s on six threads); below normal at start made it 3 to 5 s, the game's threads starving the pool.
Our cache with the driver's cold: 35 s against 46 s, so it is kept.

## D136 — The renderer's slow frames by phase; the renderer's and command processor's threads above normal, as an option (2026-11-07, Claude for Battlefield 1943)

**Decision:** the hitch line (`native_x_renderer_hitch_ms`) gives the renderer's time by phase: draws, clears,
resolves, the opaque effects and upscaling, present. `native_x_renderer_raise_priority` (off) puts the renderer's
thread and the guest GPU's command processor thread (raised the first time it passes the signal gate) one step above
normal priority on Windows.

**Why:** Battlefield 1943 at its default 60 FPS limit (runs hitch3, hitch4): the slow renderer frames are the draw
phase, 12 to 28 ms for about 2,500 draws in frames where it is usually 3 to 5 ms, so the thread looked starved by the
game's spinning job threads (all at normal priority). Raised, two A/B pairs gave 138 against 76 and 257 against 222
slow renderer frames, with other programs using most of the CPU during the tests: no gain shown, so it stays off.

## D137 — Object motion pipelines stored with the others (2026-11-07, Claude for Battlefield 1943)

**Decision:** a new object motion pipeline (D48) goes into the storage as a pipeline record whose pixel shader hash is
all ones (`PipelineStorage::kMotionPixelHash`; earlier builds find no such shader and skip it), with the motion state
itself. At start they are kept apart and made only once a draw has asked for object motion (DLSS or FSR 3.1), else
dropped, so other settings do not compile them. Each waits for its draw's own vertex shader to be translated first
(`GetMotionShader` answers neither ready nor pending until then).

**Why:** Battlefield 1943 with DLAA (runs motion1 to motion5, Guadalcanal, fresh caches): about 100 motion pipelines
were compiled in every first match, because only DiligentCore's pipeline cache kept them. Stored and shipped in the
game's list: 1,805 made at start, 20 new in the match (recorded for the next list); SMAA: 1,704 at start, the motion
ones skipped, 0 in the match.

## D138 — Tick interpolation of the world-view-projections (2026-11-07, Claude for Battlefield 1943; off for now)

**Decision:** `native_x_renderer_tick_interpolation` (off). Each main view draw that writes depth (D48's key; the
objects of one key matched by the closest matrix of the last frame, a second pass with the same matrix taking the same
output) keeps its world-view-projection (the first four perspective-looking registers, the camera vote's) as it was
after its last two changes. A change that came after at least one frame without one (a drift below a ten thousandth
of the largest entry does not count) is a tick step; while stepped, the GPU's copy of the matrix is the two blended by
the time since the change (by the frames' own times, look.cpp's rule), one tick behind. Not below 40 FPS, not across a
jump (an entry moving by more than a quarter of itself plus a tenth of its row's largest).

**Why:** Battlefield 1943's vehicles (run seek4 to vcam5, a Sherman on Guadalcanal): with the game's camera held to
the tick in a vehicle (bf_vehicle_camera_tick), every world-view-projection steps with the tick; blended, 330 to 360
draws a frame moved between the ticks while driving, 1 to 2 changed every frame, the longest frame 17.4 ms at 60 FPS.
Off because 16 to 40 draws a frame are new or jump: drawn with the tick's camera next to blended neighbours, the
terrain showed a grey wedge on a hill (vcam5 against vcam5off). Next: those draws get the camera's own blend, the
clip space correction of the matched static draws.
D138, continued (same night): a draw that is not blended (new, or a jump) in a view that moves in the tick gets the
camera's blend: ((1 - f) D + f I) times its matrix, D the tick's clip space step (last matrix times the inverse of
this one) voted by the blended draws (up to 48 a frame, the static majority agreeing within 2e-3), else the last
frame's. Run vcam7: 330 to 366 blended, 39 to 59 given the camera's blend, but the terrain still showed gaps by the
bunker (a grey patch, sky through a seam) against vcam5off. Blending every perspective-looking block of a draw instead
of the first darkened the frame (vcam8: other blocks are shadow and light projections). So still off: the terrain's
matrix is likely not the block the camera vote finds for its shaders; next, find which registers place the terrain
(NXR_VS_CONSTANTS on its shader).
D136, measured again on a quiet PC (the same night, the other programs gone, CPU at 2%): unlimited frame rate on
Guadalcanal, the same match script: off 5 frames over 20 ms after the spawn, about 110 FPS (run pace3); on 58, 68 to
109 FPS (pace4). Raised, the renderer's and the command processor's threads take the CPU from the game's own threads
that feed them. It stays off.
D138, the terrain's gaps found (run vcam14, NXR_TICK_TRACE=3): 15 to 20 main view draws a frame have no
perspective-looking block at all: their vertex shaders (16 or 17 registers from c0) hold three-row matrices, c0 to c2,
c3 to c5 and on, a skinned mesh's bone palette (whose last column, -21.8, -6.9, 2.6, moves with the camera), so they
keep the tick's camera beside blended neighbours. Next: blend the whole vertex constant set of a matched draw (bones
included) rather than one block, matched and changed by the whole set.
D138, the whole constant set tried (run vcam15): worse. Shaders carry values that change every frame (wind, water,
time), so most draws changed every frame and were not blended, and a draw's depth and colour passes no longer looked
the same. Next: per register, blend only those that step with the tick and pass the ones that change every frame.
D138, per register tried (runs vcam16 to vcam19): each register that changed after a frame without a change blended,
those changing every frame passed. 400 to 670 draws a frame blended in a Sherman, no jumps, the traced object's steps
2.8, 2.1, 1.9, 2.0 a frame against 0 and 3.6 unblended, the frame time unchanged; turned on only in a vehicle. But the
terrain lost its texture in patches (flat grey, vcam19): its patch constants (offsets and scales that change at ticks
as the camera moves) were blended too. Generic blending of constants cannot tell a placement from a parameter; what
is left to try is blending only registers that are part of a matrix the vertex shader multiplies the position by
(from the translated shader's use of the constants), or the game's own render transforms. Reverted to the first block
version, off.
D138, last try of the night (runs vcam20, vcam21 against vcam21off): blending only the perspective block of draws
that have one, and every register of those without (bone palettes). 940 to 1,170 draws a frame blended in a Sherman,
but six of nine screenshots were covered in a dark grey haze that the same drive without D138 never showed. Reverted,
off. Do not enable until a draw's registers can be told apart by what the shader does with them.

## D139 — A draw's colour kept within its target's guest range; NaN as 0 (2026-11-07, Claude for Battlefield 1943)

**Decision:** for a colour target that NativeXRenderer keeps in a host format wider than the guest's, the draw's
colour bias is given negative (`draw.cpp`, the sign bit of `color_exp_bias`), and the translated pixel shader (our SDK
copy, `spirv_translator_rb.cpp`) then makes its colour what the Xenos takes as the blending source: NaN as 0, clamped
to `edram_rt_clamp` (low colour, low alpha, high colour, high alpha: `GuestColorRange` in `targets.cpp`, the values of
the SDK's `GetPSIColorFormatInfo`): `2_10_10_10_FLOAT` 0 to 31.875 and alpha 0 to 1, the 16-bit fixed point formats
-32 to 32, the 16-bit float ones the half float range. Copies (resolves) from 16-bit fixed point targets clamp to -32
to 32 too, NaN as 0 (`blit.cpp`). Other renderers' biases stay positive and nothing changes for them. `NXR_D139=0`
turns it off.

**Why:** Battlefield 1943's testers saw smoke and fire very bright and flickering late in a match. RenderDoc captures of
grenade explosions on Guadalcanal (runs nadeRd0n, fix off): the scene target is guest format 3 (7e3, top 31.875), kept
as RGBA16F; one fragment of an explosion wrote 70.1 red and the blends reached 87.9 (1,287 pixels over the top), where
the Xenos keeps 31.875 at most. With the clamp (runs nadeRd1n, six captures) the scene target's highest value was 26.0.

**Limits:**
- The exact EDRAM path (`render_target_path_d3d12=rov`, `dxbc_translator_om.cpp`) clamps the blending source only for
  the fixed point parts of a format when the target blends (7e3: its 2-bit alpha, not its colour) and clamps the
  blend's result when it packs it. A host float target cannot clamp a blend's result, so blends still add past the top
  within a draw; clamping the 7e3 source colour at 31.875 keeps the stored values nearer the Xenos' than leaving it
  free. The resolves bring the values to the guest range before anything samples them (D50).
- Seen in game (runs nadeBurst-ref, -fix, -nofix: screenshots about eight a second, the same grenade script under the
  exact EDRAM path, D139 on and off): the fire looks alike in all three. The smoke after it is light grey in the
  reference and near black with NativeXRenderer, with D139 on or off: a separate fault (the smoke draw's vertex
  shader outputs a colour of 0.04 at alpha 0.8 to 0.99, run nadeRd1n event 6712), followed up on its own.

## D140 — A resolve whose memory a draw reads as vertices is also written into the guest memory mirror (2026-11-08, Claude for Battlefield 1943)

**Decision:** when a draw's vertex fetch range overlaps the guest memory of a resolved texture that is still valid and
was resolved this frame or the last (`ResolvedTextures::VertexSource`, a binary search in an index of the textures by
address, rebuilt when the registry's generation changes), that texture is marked `vertex_data`. From its next resolve
on, the resolve also writes its texels into the guest memory mirror at its guest address (`Blitter::WriteToMirror`, a
compute pass on the mirror's read-write view): the guest's layout (linear rows at the fetch constant's pitch, or the
Xenos 2D tiling, the SDK's GetTiledOffset2D), its endianness, each guest texel taken from the middle of its host texels;
then the mirror goes back to its read state. Formats whose components are whole 32-bit words: k_32_FLOAT,
k_32_32_FLOAT, k_32_32_32_32_FLOAT (others are logged once and left as before). The renderer's 600-frame line counts
the resolves written so.

**Why:** Battlefield 1943's smoke and dust were near black, and the testers saw smoke and fire very bright and
flickering late in a match. Its particle vertex shader multiplies the sun's colour (c198) by a per particle factor it
fetches as one float per vertex from a second stream (fetch constant 93); the game computes that factor on the GPU into
a 256x32 k_32_32_32_32_FLOAT target (one texel per particle, its four vertices' values) and resolves it into one of three
rotating 128 KB buffers each frame (runs nadeVR, nadeRd1n, nadeRdRef). Resolves stay on the host here (D70), so the
mirror held what guest memory held: zeros (no sunlight: black smoke; the reference's values are 0.8 to 1.0), and later
whatever the memory held before (a factor of any size: very bright, flickering smoke and fire). The SDK's renderers
resolve into their shared memory buffer, which their vertex fetches read: the same grenades there show light grey,
sunlit dust (runs nadeBurst-ref, -refeng, -vk).

**Limits:** the texture is found by a draw after the resolve that frame, so its first resolve is not written (one
frame of the old data, once per buffer). Memory written by the CPU after the resolve invalidates the texture as before
(D70) and is not overwritten. Only the first slice of a 2D target is written (no cube faces, no array slices: none
seen read as vertices).

**Checked:** run nadeBurst-d140 (screenshot bursts of the same grenades): the dust is light and sunlit as in the exact
EDRAM reference; the log finds the three buffers and writes one resolve a frame. Run nadeRdD140 (RenderDoc, captures
at frames 3463 and 3494): the smoke draw's factor stream holds 0.38 to 1.0 (at 0xc27b000, 8 zeros in 2048 values; at
0xc716000, none), where it held zeros before.

## D141 — Antialiasing keeps its image sized textures for two sizes (2026-11-08, Claude for Battlefield 1943)

**Decision:** SMAA and FXAA keep their textures (the input's copy in the image's format, SMAA's edges and blending
weights) per image size and format, two sets (`Antialiasing::EnsureTextures`): an image whose size and format match a
set uses it; another makes a set in place of the one used longest ago; a set not used in the last 1,200 applications is freed. The
frame generation's HUD-less copy (D123) keeps two copies the same way. The 600 frame log counts the sets made.

**Why:** one set was made again whenever an image differed from the last. Battlefield 1943 resolves its finished
scene twice in some frames, in two formats or sizes (its loading screen, Loading complete, the spawn screen: 1,200
applications in 600 frames, against 600 in play), so each frame freed and allocated about 96 MB of device memory
twice: DiligentCore's memory manager created and destroyed a 64 MB and a 32 MB page each time (1,000 to 2,000 page
events in a match's first 10 s on every map; after a server's level change 60 to 185 a second for the 50 s of Loading
complete, runs rotperf1 and rotplay1). Fast here, where it did not show in the frame rate; a driver or a card where
such allocations are slow would pay for each one in every such frame.

## D143 — Object motion finds a draw's last frame by hash and by bounds (2026-11-09, Claude for Battlefield 1943)

**Decision:** `ObjectMotion::Match` (D48) keeps, for each draw, a hash of its vertex shader constants that the view
does not share and their bounds: for four groups of those constants in order, the sums of their x, y, z and w. Both sit
beside the key in the last frame's index. A draw that did not move is found by its hash, without reading the other
draws' constants. For one that moved, the last frame's draw closest in its bounds is compared whole first; every other
draw of the key whose bounds alone are further than the closest so far is skipped (the difference of two sums is at
most the sum of the differences, so it cannot be closer); the rest are compared whole, stopping every 8 registers once
past the closest. A float sum's rounding is left a thousandth of room. When the shared registers change at a frame's
end, that frame's hashes and bounds are made again without them (each draw's register numbers are kept for it). A
motion draw (D48) no longer copies the last frame's constants: they are ObjectMotion's own until the frame ends, after
the motion draws. The 600 frame log gives the matching's and the motion draws' CPU time, the draws compared and those
ruled out by their bounds. `NXR_MOTION_DUMP=<file>[,<first frame>,<frames>]` records Match's inputs for a benchmark.

**Why:** Battlefield 1943's Guadalcanal draws one mesh in up to 115 places a frame (its palms and plants), half of them
with 256 constants, and every object's own world-view-projection changes with the camera, so nearly all of them
"moved": each was compared whole with every draw of its key, about 33,000 comparisons of up to 1,024 floats a frame.
That was up to 9 ms of the renderer's thread a frame (54% of it in the long frames of a heavy view, run heavy1), and
with about 3,900 draws in view it made the renderer's thread, not the GPU, the limit (TekRantGaming's play on 9 Nov: 24 to 34
ms frames, the renderer's thread 23 to 32 ms). On 1,300 recorded frames of that view (942,696 draws) the matching took
6.5 ms a frame on average (99th percentile 19 ms, worst 20 ms) before and 1.1 ms after (6.5 ms, 14 ms); 59 of the
942,696 answers differ, draws at equal distances.
D143, continued (same day): a second view (run dq1, standing by a bunker on Guadalcanal while the camera turns) still
took 9.8 ms a frame: its plant batches (256 constants, up to 69 draws of one key) change in every constant with the
camera, the bounds' sums of many objects differ little from batch to batch, and about 600 draws a frame had no bounds
at all (an unused register with garbage, 1e30 or not a number). Now values are clamped to +-1e6 (not a number: 0)
before the bounds are made (a clamped difference is never more than the distance counts, so they are always valid),
the sums stay in double, and the bounds also hold a sample of 8 whole constants spread over the draw's (their distance
is part of the whole too, and does not average out). Past 2 ms of matching in a frame, a draw is compared whole only
with the 4 of its key closest in their bounds. On 900 recorded frames of that view: 6.6 ms a frame before D143 (99th
percentile 13.8, worst 14.9), 1.8 ms after (4.7, 5.9); 8% of the answers differ from comparing all, at the same
distance (the ratio of their distances 1.000 at the median and the 90th percentile, 1.03 at the 99th). In play, the
view went from 58 to 113 FPS with DLSS Quality. `NXR_MOTION_DUMP=<file>,auto,<frames>` records from the first 30
frames running whose matching took over 5 ms.

## D144 — Grass: decided over frames, in rings, drawn by an indirect draw of the tufts in view (2026-11-09, Claude for Battlefield 1943)

**Decision:** the grass prototype keeps a state per cell from frame to frame (slots wrapped on the grid, so a cell keeps
its slot as the grid follows the camera; a stamp made again when the grid or the level changes): the share of frames
its ground looked green (from five pixels round its root, eased by 0.15 a frame) and its colour. A cell grows past 0.7
and stops below 0.3; while its root is hidden or off screen it keeps what it had. The placement appends the tufts that
grow and are in view (atomic count) and the draw is indirect, so only those are drawn (before, every cell of the grid
was drawn, 18 vertices each, once per depth partition). Past 45 m the grass is drawn in rings, each twice as far with
cells twice the size and tufts up to 4 times larger, every ring as many cells (`native_grass_radius`: 45, 90, 180,
360, 1440 m are the game's menu's Low to Full map; six rings at most). Blades are at least about 0.6 of a pixel wide.
`native_grass_steady` false decides from one pixel each frame, as before; the 600 frame log gives the tufts drawn and
how many came or went a frame.

**Why:** TekRantGaming, 9 Nov: the grass flickered badly. Each frame decided whether a tuft grows from the one pixel the game
drew at its root, which falls on another texel of the ground's texture as the camera moves, and changes with light,
fog and anything passing: on a Guadalcanal walk 18.4 of 59 tufts came or went every frame; steady, 1.1 of 2,124 (and
0.0 to 0.3 a frame on later walks). Its GPU time stays 0.03 to 0.09 ms a frame at 45 m and 0.04 to 0.06 ms at 1440 m
on green ground; with every cell growing (`native_grass_debug` 2), 9,455 tufts to 1440 m take 0.65 ms.

## D145 — Grass: the game's own plant pictures (2026-11-09, Claude for Battlefield 1943)

**Decision:** `native_grass_textures` names a file of plant pictures ("BFVG", version 1: kinds, size, each kind's
average colour and width over height, then each kind's RGBA at size x size) that the game makes from the player's own
files (Battlefield 1943: its beach grass strip in two halves, its undergrowth set's long leaf, fern and leafy weed, a
jungle leaf and a leafy branch, each cropped to its picture; never shipped). Loaded into a texture array once it is
there (looked for every 300 frames), with mips made here: colour averaged over opaque texels, each mip's alpha scaled
so as much of it passes the alpha test as at full size. A tuft is three crossed cards as before, each kind its own
width from its picture's proportions and its own size (grass 80% of tufts, the long leaf 5%, fern 7%, weed 4.5%, jungle
leaf 1%, branch 1.5%); the picture's colour over its average, times the ground's lit colour at its root (light, shadow
and fog from the game's own drawing), its foot darker. Without the file, the blades drawn by the shader as before.
`native_grass_debug` 5 grows them on any ground in their own colours.

**Why:** TekRantGaming, 9 Nov: the tufts were not textured and all one thing. 5,952 textured plants (debug 5, to 180 m) take
0.19 ms of GPU time a frame.

## D146 — Grass: one plant in every ring, dissolved not shrunk, none under water, twelve kinds (2026-11-09, Claude for Battlefield 1943)

**Decision:**
- **The same plant in every ring.** A cell of ring L stands for one cell of the first ring: the child (of four) kept
  by hash at each ring, down to the first (`Representative`). Its place, kind, size and random values are that first
  ring cell's, so a tuft handed from one ring to the next is the same plant in the same place. A ring draws from 0.85
  of the last ring's radius to 0.85 of its own (the last to its own); it keeps state from 0.65 of the last ring's
  radius, so a tuft has its history when handed on.
- **Thinning dissolves.** The three of four cells the next ring does not keep (`Kept`) fade from 0.6 to 0.85 of the
  ring's radius, and every tuft at the last ring's edge from 0.8 to 1. A fade is a per pixel dissolve (a noise of the
  screen position, shifted per tuft), never a smaller tuft. Size grows with the distance itself (1 + (d - 20) / 70,
  1 to 4), never by ring: no tuft changes size at a ring's edge.
- **No grass under water.** At the end of the opaque draws, after the grass, the scene's colour and depth are kept at
  half size (`kSnapCS`). At the finished scene's copy (the profile's `scene_resolve_caller`, after the water and the
  other transparent draws) `Grass::MarkWater` compares (`kWaterCS`): where nothing nearer was drawn since (the game
  clears the depth before its first person weapon) and the colour changed and turned blue, the water covered the
  ground. The next frame's placement sets that cell's score to 0 at once. By the finished scene's copy the renderer
  no longer has the depth target as the scene's, so the snapshot keeps it. `native_grass_water` (on) turns it off.
- **Steadier decisions.** The green score eases at 0.08 a frame (was 0.15); a tuft grows above 0.8 and goes below
  0.2 (was 0.7 and 0.3).
- **Leaf edges blended** over a pixel (alpha from the alpha's own screen rate of change), so far leaves shimmer less
  than with a hard alpha test.
- **Twelve kinds** (`vegetation-2.bin`, made again from the player's files; the file of seven is left as it is): the
  seven of D145, then a tall leafy weed (MEBushLeaves_01), two small leafy bushes (AcaciaLeaves_01, BeechLeaves_02)
  and a young palm's frond, green and dried (PalmTreeFrond_01's two halves, each turned to stand on its stem). Grass
  is 62 to 92% of a patch; the others come in patches of 7 m, each patch with a plant of its own.
- On screen: a tuft is drawn if its root or its top is in view (a near tuft whose root is below the view).

**Why:** TekRantGaming, 9 Nov: the new grass still flickered and popped, grew as he came closer (the old ring fade shrank
tufts and the rings' sizes jumped), and grew under the sea (the sea floor is drawn green before the water). He asked
for more kinds.

**Measured** (runs g2_*, 3200 x 1800, 90 fps cap):
- Wake, from the carrier, debug 5 (plants on any ground): 1,338 tufts a frame with the water check off, 46 with it on
  (487 places a frame seen under water).
- Guadalcanal, normal, the same walk: 0 to 2.0 tufts a frame came or went of 106 to 2,408 drawn (0 to 2.7 with the
  old easing and margin); GPU time 0.15 to 0.19 ms a frame; 88.9 to 90 fps, the longest frame 36.8 ms.
- Debug 5, 180 m: 12 plant pictures loaded, 1,768 to 5,008 tufts, 0.18 to 0.30 ms a frame.

## D147 — Grass: none below the level's water, the far ground covered, whole tufts fading (2026-11-09, Claude for Battlefield 1943)

**Decision:**
- **The level's water height.** `native_grass_water_level` (set by the game when a level loads, from the level's own
  water mesh in the player's files: every Battlefield 1943 level has one flat one, Guadalcanal 173, Wake Island 215,
  Iwo Jima 5.3, Coral Sea 61.5): no grass where the ground is below it + 0.3 m, and no state kept there. The water's
  picture (D146) is then not made; it stays only for a level whose height is not known.
- **The far ground covered.** Past near_d (0.425 of the first ring's radius, 19 m) the share of tufts shown falls as
  1 / d^2 of the first ring's density and the tufts grow as d^0.9 (to 8 times), with the distance itself. Within a
  ring the quarter the next ring keeps always shows; the other three quarters show at (wanted share x 4^ring - 1/4) /
  (3/4), which is all of them at the ring's start and none at its hand on, so the density falls smoothly instead of
  in a step at each ring (D146's thinning from 0.6 to 0.85 of a ring left the far ground bare). Tall kinds lose their
  extra height as they grow (a far tall weed is not 12 m tall).
- **Whole tufts fade.** Each tuft shows at its own share (a random value per tuft) and comes in over a short stretch
  (0.12 of the share), blended as a whole; D146's per pixel dissolve sparkled as the view moved.
- **Density** 0.7 (was 0.85): TekRantGaming, a little less.
- **Developer camera tour** (the game's `BF_DEV_CAMERA_TOUR`): the camera moved to random spots of the level, inland
  and at the coast, at eye height, its ground from `GrassProbe` (a height the placement reads from the heightmap,
  read back a few frames later). Runs g3_*_tour.

**Why:** TekRantGaming, 9 Nov: grass still under the shallows and the deep sea; still some flicker; the density a little too
high, and the far ground bare until he came closer; test all over each map, not only at the spawn.

**Measured** (runs g3_gc_tour3, g3_wake_tour, g3_iwo_tour, g3_coral_tour, 3200 x 1800, 90 fps cap; 9 spots on each
map, every third at the coast; the level's water read on each: 173, 215, 5.3, 61.5):
- No grass under water at any coast spot (shallows over sand, wet sand, open sea) on any of the four maps.
- 0.0 to 0.3 tufts a frame came or went.
- GPU time at most 0.28 ms a frame.

## D148 — Grass: tufts fade, never pop; one decision across rings; a pop count (2026-11-09, Claude for Battlefield 1943)

**Decision:**
- **Presence, not a yes or no.** Each tuft keeps its presence (0 to 1) in its state. Its score eases to the ground's
  look with a time constant (`native_grass_settle`, 0.15 s); the presence it wants is smoothstep(0.35, 0.65, score),
  and it moves there by at most a whole tuft in `native_grass_fade_time` (0.2 s). A decision that changes fades the tuft
  in or out; a root seen for the first time starts from a score of a quarter, so its tuft fades in. Under water a tuft
  goes at once. The opacity drawn is presence x the distance fade; part faded tufts are in the second list (below).
- **One decision across rings** (`native_grass_handoff`): while the nearer ring keeps a tuft's state (within its
  radius) the further ring takes it (score, colour, presence); a tuft new to a ring takes the further ring's.
- **On screen by its ball** (`native_grass_bounds`): a tuft is drawn while a ball round it (as wide as its widest
  card) may be in view; the root or top test dropped wide near tufts at the screen's edge.
- **Fading tufts in their own pass** (`native_grass_fade_pass`): drawn after the others without writing depth, so a
  half faded tuft hides nothing behind it; the distance fade's stretch is 0.2 of the share each side
  (`native_grass_fade_band`, 0.06 before).
- **The pop count** (`native_grass_pops`, developer): each tuft's last frame, keyed by its first ring cell (a slot by
  one hash, the tuft's own key by another), and counted when it was on screen in both frames (its ball) and its
  opacity jumped by more than a quarter: by its decision, by its root seen for the first time, or by its fade. Logged
  with the counts.

**Why:** TekRantGaming, 9 Nov: the grass still flickered and popped in and out as he walked. The first count, by the root
only and with slots shared between tufts, read 6 to 14 pops a frame for both the old and new code alike (the shared
slots); keyed, it read 0.3 to 0.9 a frame, and by the ball 0.3 to 0.5 more from roots seen at last (behind a crest, a
rock, a bush): at 90 frames a second, 50 to 100 tufts popping every second. Not the cause: the state was made again
once a run (at the start), and the terrain's 2048 x 2048 DXT1 texture is its intensity map (Guadal_Canal_Intensitymap),
not a colour map, so there is no map of where green ground is to decide from.

**Measured** (3200 x 1800, 90 fps cap, radius 180 m):
- Walk on Guadalcanal (g5_reveal_walk, before; g6_walk, after): pops a frame by decision 0.28 to 0.42 before, 0.00
  after; by roots seen at last 0.28 to 0.49 before, 0.00 after; by fade 0.17 at most in both (the first seconds).
- Camera tour of Guadalcanal, 9 spots turning (g6_gc_tour): 0.00 a frame of every kind after its first window.
- GPU time 0.02 to 0.08 ms a frame on that tour.

## D149 — HD textures from a pack (2026-11-09, Claude for Battlefield 1943)

**Decision:** `native_x_renderer_hd_textures` (off by default) with `native_x_renderer_hd_pack` (a "BFHD" file, read once:
its index in memory, each texture's data read when needed). After every load of a game texture's first level
(LoadTextureDataFromResidentMemoryImpl), its key is computed from guest memory: XXH3 of its first 64 KB, xor its guest
format << 56, width << 32 and height << 16. A DXT1, DXT3 or DXT5 2D texture whose first level is at least 64 KB and whose
key is in the pack gets the pack's texture: made once for the session (BC1 or BC3, every mip, the pack's size: twice the
game's), and GetView hands out its view instead of the game texture's (not for a signed reading). The game texture
itself is untouched: a texture whose memory the game fills again with another picture gets its own view back at its
next load (its key no longer matches). The views live for the session (the binding sets keep views by pointer).

Pack (little endian): "BFHD", version 1, count, 0; per texture 56 bytes: key, guest format, width, height, HD width, HD
height, mips, block format (1 or 3), 0, data offset, data size; then each texture's mips, largest first, blocks row by
row. Made by `tools/hdpack/make_pack.py` from the game's own export (a test build with `BF_DEV_HD_EXPORT`: each world
texture as a PNG with its key; not menus, fonts, minimaps, intensity maps, baked lighting or masks, particles or normal
maps): Real-ESRGAN (realesrgan-x4plus) at 4x, a Lanczos filter to 2x, the broad colour (a blur of 3 pixels) from the
original scaled up and the detail from the model (the model turned a warm gravel grey), alpha from the original, mips
with alpha weighted colour, encoded by range fit. Never in git: it is made from the game's own textures.

**Why:** TekRantGaming, 9 Nov 2026: an HD texture pack, imported in the launcher and toggled in the settings; made here (his call).

**Measured:** a pack of 30 textures (46 MB): 5 of them drawn on Guadalcanal in a 5 spot tour (run h1_test), keys
matching; the scene drawn as without it.

## D150 — Optimisation for a GTX 1060 / RX 580 at 1080p (2026-11-09, Claude for Battlefield 1943)

**Decision:**
- **Ambient occlusion at half resolution from a render scale of 1.5 (1080p) up** (KK's auto had full resolution up to
  1.5). Its output is upsampled by depth (kOutput).
- **Global illumination Low its own cost:** one slice of five steps (was medium's two of six, a shorter reach only).
- **A GPU benchmark** (`GpuBenchmarkStart`, `GpuBenchmarkResult`): a compute pass of 1920 x 1088 threads, each 256
  dependent multiply adds and 32 texel reads, four times between a duration query, ten times a frame apart, the
  shortest kept (the GPU idles in the launcher and its clock rises with the work). Logged. The game's first start auto
  detect sorts the GPU by its time against an RTX 4070 Ti's (5.83 ms): fast up to 4 times (today's defaults), middle up
  to 10 (60 FPS at most, shadows Original, AO and GI Low, GI Near), slow past it (720p, 60 FPS, AO and GI off, no grass).
- **When it runs (the game, 9 Nov 2026 afternoon):** after the stored pipelines are made and 3 s of steady frames (none
  over 50 ms). Started at the menu's first frame, it ran while they compiled (a first start, or the first after an
  update: 13.3 ms on the 4070 Ti, run smoke122), and 2 s after them, the game still loading, it read 8.7 (smoke122b):
  the GPU idles between uneven frames and its clock falls; after 3 s of steady frames, 5.848 (smoke122c). The launcher shows before the renderer exists (a request
  there was dropped), so the first start's auto detect, made in the launcher, has no time yet and keeps the fast tier:
  the tiers apply only once the time is taken after Play (open).

**Why:** TekRantGaming, 9 Nov 2026: optimise; the game should run on a GTX 1060 or an RX 580 at 1080p, 60 FPS, with the
defaults.

**Measured** (camera tours of Guadalcanal at a render scale of 1.5, an RTX 4070 Ti, 90 FPS cap):
- AO 0.73 ms a frame at full resolution, 0.35 at half (runs o1_defaults, o2_cheap); the two looked alike.
- GI 0.47 ms at Medium, 0.40 at the new Low (o4_gilow): most of its cost is outside the gather.
- Grass 0.05 ms at Medium (90 m).
- Benchmark 5.830, 5.829, 5.827, 5.826, 5.846 ms on five starts.
- Device memory 1.15 GB at 1080p with the defaults (867 MB with every effect off).
- Not costs: object motion (D143) runs only with a temporal upscaler, so not with FXAA; the renderer's CPU 0.6 to 1.9 ms
  a frame. The frame's "GPU" span in the renderer log (FrameTimer) counts the GPU's idle gaps too (3 ms with 15 draws
  in a menu): not a cost measure.

## D151 — Grass: where it grows from the level's own ground map, never from the screen (2026-11-09, Claude for Battlefield 1943)

**Decision:**
- **The level's ground map** (`GrassMap`): the game reads the level's TerrainMaterialMap from the player's files when
  the level loads (Battlefield 1943: 4096 x 4096 cells of 0.5 m, each a ground material: dirt, grass, rock, water...)
  and gives the renderer each grass cell's distance to other ground (m x 16, a byte), over the box round the grass.
  While a level's map is being read no grass is drawn; without one (another game, a level without a map) the grass
  decides from the screen as before (D144 to D148). `native_grass_map` (on) turns it off.
- **Decided from the world alone.** On the map a tuft grows where the ground is grass, flat enough and above the
  water, and nowhere within `native_grass_edge` (1.5 m) of other ground; over the next `native_grass_edge_fade` (2.5 m)
  the tufts thin in and grow to their full size. Nothing about it depends on the view or on time: a tuft never comes,
  goes or moves as the camera turns or moves; only the distance fades (D146, D147) change it, smoothly.
- **Only the colour from the screen:** the ground's light at the root (nine pixels 3 apart at its depth), eased while
  the root is seen; a tuft whose root has not been seen yet takes its ring's average of the last frame's roots seen.
- `native_grass_debug` 6: tufts on any ground coloured by the map (green grass, amber within the edge, red other).

**Why:** TekRantGaming, 9 Nov 2026: the vegetation still flickered and bits of it seemed to teleport about as he moved; turning
to ground he had not seen, he watched the grass spawn in; and it was worst at the edges of the grass texture. All three
came from deciding from the screen: a root's look changes with the view (most at an edge, where its few pixels mix
grass and sand), and ground never seen has no decision yet, so its tufts faded in.

**Measured** (Guadalcanal, 3200 x 1800, radius 180 m):
- The map read in 34 to 40 ms (Guadalcanal 57,094 m2 of grass; Wake Island 57,737; Iwo Jima 44,219; Coral Sea has none).
- The same walk (runs m2_walk_map_a, m2_walk_screen_a): tufts that came or went per frame 0.0 in every window with the
  map, 0.1 to 2.6 from the screen (each a 0.2 s fade, D148: what TekRantGaming saw as flicker and tufts moving about); pops (a
  jump of a quarter or more in a frame) 0 with both after the spawn.
- Debug 6 tour (m1_gc_debug): the map's grass is the lush grass layer (spot 8); the sandy green ground is rock or dirt in
  the map (spots 4, 6, 9), so it has no tufts now.
- Tours of Wake Island and Iwo Jima (m4_wake, m4_iwo) and Guadalcanal (m3_gc_tour): dense tufts on the map's grass (Wake
  spot 1, Iwo spot 4), stopping short of the roads; none on the sandy or olive green ground the map calls dirt or rock;
  none on Coral Sea. 0 tufts came or went a frame on every map; GPU time 0.030 to 0.045 ms a frame.
- Not done: the terrain's own grass layer (its PatchMaskShaders' CG_Grass1, masks in the level's MaskTileAtlas, placed by
  its Visual.res: per heightfield 128 x 128 patches of 16 m with tile numbers, then 64 x 64 records of 20 bytes) would
  reach the sandy green ground too; its patch table is not read yet.

## D152 — Grass on a level's map: every tuft laid out once, drawn at full density to the whole map (2026-11-09, Claude for Battlefield 1943)

**Decision:**
- **Laid out once.** With a level's grass map (D151), every tuft on it is laid out when the map comes (`kAllGenCS`: a
  thread per first ring cell over the map's box, the rings' own rules, plants and random values): its place, size and
  plant, up to 196,608 tufts. Made again only when the map, the density, the edge or the plants change.
- **Drawn at full density.** Each frame `kAllCullCS` takes every laid out tuft within `native_grass_radius` and in view
  (flat enough, above the water, as the rings), with the rings' colour (the root's light, eased; a distance band's
  average until its root is seen) into the same two draw lists. No thinning with distance and no growing: a tuft is
  the same size and there whatever the distance. It fades only at the distance's edge and as it shrinks on screen
  from 1.5 to 0.5 times `native_grass_pixels` (1.5 px): a tuft of a pixel or two only shimmers.
- **`native_grass_radius` defaults to the whole map** (1440 m; the menu's and F8's Full map). Below it the distance's
  edge fades over its last 15%.
- **Each tuft drawn only in the depth partitions it reaches** (the vertex shader drops a tuft wholly outside the
  partition's distances, 0.1 / depth): it was drawn in all three (both modes).
- Without the map passes, or without a map, the rings as before (D144 to D151).

**Why:** TekRantGaming, 9 Nov 2026 (0.1.23, "by far the best version"): walking forward, more grass appeared as he came
closer; he asked for the grass to be drawn much further, the whole map as an option. The rings thinned the tufts past
19 m as 1 / d^2 (D147) and grew them, so walking towards grass made more of it come in.

**Measured** (an RTX 4070 Ti, 3200 x 1800, 90 FPS cap; camera tours m5_gc_tour, m5_wake, m6_gc_full):
- Laid out: Guadalcanal 114,273 tufts, Wake Island 103,699.
- Drawn a frame at the whole map: 5,000 to 80,000 tufts (D151's rings on the same tour, m3_gc_tour: 190 to 630).
- GPU time 0.05 to 0.25 ms a frame (the longest frame's 0.77 ms); D151's rings 0.03 to 0.045 ms. 90 FPS throughout.
- No tuft came or went; pops by decision or a root seen 0; pops by fade only at the tour's teleports (every tuft in
  view at once).

## D153 — The GPU's time taken at its full clock (2026-11-09, Claude for Battlefield 1943)

**Decision:** the benchmark (D150) keeps the GPU fully busy: each run is about 40 ms of this GPU's work (four dispatches
timed, the rest before them untimed: 4 to 64 dispatches, from the last run's time), for 3 s and sixteen runs at least,
the shortest kept. The reference is the RTX 4070 Ti's at its full clock, 5.58 ms. The game asks for it only while a
first start's auto detect waits for it (and in test builds), so a player's start has no heavy load otherwise.

**Why:** TekRantGaming chose (9 Nov 2026) a first start auto detect that works. Taken in a few short runs at the game's start
the time read 5.85, 11.45, 8.6 and 8.3 ms on the same RTX 4070 Ti: its clock was still rising (375 MHz, then 1.3 to
1.6 GHz, partly busy; run ad3), and a slower GPU, busy throughout, would have read faster than it is against it.

**Measured** (runs ad4 to ad6, the clock logged every 100 ms): fully busy, the clock rose from 585 MHz to 2,775 MHz in
about 2.5 s; the time 5.576 and 5.575 ms on two starts.

## D154 — Models placed from the tick's camera, drawn from the frame's (2026-11-10, Claude for Battlefield 1943)

**Decision:** `SetAttachedCameraMove(position, move)`: the game records, for each frame it draws, its camera's position
and the move (column form) from the camera its tick placed first person models from to the camera it shows. A draw in
a near projection (the first perspective block's near plane under half a metre) whose object holds the eye within half
a metre, with a world matrix in three consecutive registers (rigid rows, the position in the fourth floats) that puts
the eye within 5 mm of a recorded camera (the last 16, none older than half a second), has that world matrix and its
world-view-projection moved by the record's move in the GPU's copy of its constants. Draws with no match (on foot, no
records) are left as they are.

**Why:** Battlefield 1943 places a tank's vision slit (and a bomber's sight) at each of its 30 Hz ticks from the camera
that tick sees, and draws it so on the frames between, where the game's camera is blended on every frame (its
bf_smooth_motion). Every frame dumped (NXR_FRAME_DUMP, run vtankparts3): the slit's edge at 54, 68 and 86% of the
screen by the frame's place in the tick while the tank drove and its turret turned, the game's own view 93%; in the
slit's constants (NXR_NEAR_DUMP, vtanknear5) its world matrix (c16 to c18) held for the whole tick while the eye moved
8 cm a frame through it and jumped back 16 cm at each tick. It is in no part tree, skeleton or stored matrix near the
camera that the game side could blend (runs vtanknear*, vtankinst*): only in the draw constants.

**Measured:** moved, the slit's edges at 6% and 93 to 95% on every frame of the same drive at 90 FPS (vtankfpd154; the
game's own 6% and 93%), and on all 200 frames dumped (vtankd154b); 5,016 to 5,374 draws at the eye moved in 10 s, none
on foot (their 728 a frame outside vehicles unmatched). The Corsair's cockpit, the jeep's first person and the boat
unchanged (vplaned154, vjeepfpd154, vboatd154).

**Also:** NXR_NEAR_DUMP=<seconds>,<frames> (a diagnosis aid): every draw in a near projection, its shader and its float
constants, on that many frames.

## D155 — A resolve read as vertices for the first time is written into the mirror before that draw (2026-11-10, Claude for Battlefield 1943)

**Decision:** each resolve keeps its place in guest memory (`ResolvedTexture::last_layout`: the mirror write's
fields). When a draw is the first found reading a resolve's memory as vertices (D140 marks it then), the draw path
writes that newest resolve's texels into the guest memory mirror right away (`Blitter::WriteToMirror`, the renderer's
blitter handed to the draw path), then binds its own targets again (the compute pass bound its own). D140's limit,
"its first resolve is not written (one frame of the old data, once per buffer)", is gone.

**Why:** TekRantGaming's 0.1.27 test: smoke and dust "flashing", smoke in general "flickering again". Every frame dumped
around a burning tank (run tankfire1): in one frame the smoke over a third of the view drawn light grey and lit, dark
before and after (its particles' sun factor, D140's stream, read from memory a resolve had not written). Each time a
factor buffer is found (three at a match's first smoke, and again whenever the game makes its buffers anew) that
frame read whatever the mirror held. The dumps since show the draws reading only the buffer resolved in their own
frame (tankfire3, NXR_D140_LOG: 1,200 frames, none otherwise), so a buffer's first frame was the one that could not.

**Checked:** run tankfire4: the three buffers written before their first draws (D155's log), no frame of bright smoke
in 600 frames dumped, the burning tank's fire and smoke drawn as before.

**Also:** NXR_D140_LOG (a diagnosis aid): each mirror write and each draw reading a resolve as vertices.

## D156 — A model placed from the tick's camera is known by holding that camera, not the frame's (2026-11-10, Claude for Battlefield 1943)

**Decision:** D154's record of a frame's camera also holds the position of the camera the models were placed from
(`SetAttachedCameraMove(position, move, placed)`). A draw in a near projection is moved when one of its world
matrices puts the eye at a recorded frame camera (within 5 mm, as before) and its object holds that record's placed
camera within half a metre. Before, the object had to hold the frame's eye within half a metre.

**Why:** TekRantGaming's 0.1.27 test: on Wake Island's air raid the bomb sight "flashes like the tank's view". Every frame
dumped in the bomb sight (run raiddump2): the sight's porthole full in one frame, pushed up out of the picture in the
next, in turn. The bombers fly far further in a tick than a tank: on the frames drawn a whole tick behind the newest
tick (one of every two at 60 FPS) the sight was over half a metre from the frame's eye, so D154 did not count it as a
model at the eye and left it where the tick put it (D154 off, run raiddump3: pushed up by half a tick and by a whole
tick in turn). Measured from the camera it was placed from, it is at the eye on every frame, at any speed.

**Measured:** the bomb sight steady on all 400 frames dumped (raiddump4: no one frame change); the tank's vision slit
at 6% and 93 to 94% on every frame of the usual drive (vtankd156, as D154's vtankfpd154); the Corsair's cockpit and
the jeep's first person unchanged (vplaned156, vjeepd156: no one frame change in 200 frames of driving). A model drawn
from the frame's own camera (a part the game side blends) holds the frame's eye but not the tick's once they are half a
metre apart, so it is no longer moved by the step between them, as it could be before.

## D157 — A spatial upscaler's mode: the render scale from the screen divided by a ratio (2026-11-11, Claude for Battlefield 1943)

**Decision:** `native_x_renderer_upscale_ratio` (0 by default: no change), read at startup with the render scale
(D37): when the scale comes from the screen (`native_x_renderer_resolution_scale` 0), it is divided by this ratio and
kept at least 1, so the game draws fewer pixels for D115's FSR 1 or NIS to bring up to the screen. A forced render
scale wins over it. The log's render scale line says so ("from the output divided by 1.50,
native_x_renderer_upscale_ratio").

**Why:** TekRantGaming (2026-11-11), for Battlefield 1943 Reborn 0.2.1: AMD FSR 1 and NVIDIA NIS as King Kong Recompiled offers
them, an Upscaler row and an Upscaler mode row (Native, Quality, Balanced, Performance: AMD's ratios 1, 1.5, 1.7, 2).
King Kong rounds the drawn size to its renderer's 720p steps; a ratio of the screen's own scale gives the mode's real
size here (a 2560x1440 screen at Quality: 1707x960, where 720p steps would give 1280x720). The game decides when the
ratio applies (an upscaler chosen); the renderer only divides.

## D158 — Whether NIS runs on this GPU: asked at startup, NIS that cannot run gives FSR 1 (2026-11-11, Claude for Battlefield 1943)

**Decision:**
- **NIS's compute pipelines are made at Initialize** (they do not depend on the window's format), with its shaders,
  so a GPU that cannot run NIS shows at startup, not at the first frame.
- **NIS chosen but not running gives AMD FSR 1** (logged), as King Kong Recompiled's presenters without NIS give FSR,
  instead of the plain stretch.
- **`native_x_renderer_check_nis`** (off by default): with another filter chosen, NIS is set up once at startup in a
  Scaling of its own and let go (`Scaling::Supported`, its time logged: "scaling check: NVIDIA Image Scaling runs on
  this GPU (N ms)"). **`NisSupport(runs, gpu)`** gives the answer and the GPU's name once known (from NIS's own setup
  when it is the chosen filter, or from the check).

**Why:** TekRantGaming (2026-11-11), for Battlefield 1943 Reborn: NVIDIA NIS must not be offered on devices it does not run on.
The game's launcher shows its settings before the renderer has a device, so it offers NIS by the answer saved from the
last start.

## D159 — The window's real size when the SDK's is out of date (X11): the surface's size kept and given to the SDK (2026-11-11, Claude for Battlefield 1943)

**Decision:**
- On Linux, `Device::UpdateSize` asks the swap chain for the SDK's window size, as before. When the swap chain keeps
  another size (an X11 surface, under XWayland too, has a size of its own: DiligentCore takes `currentExtent`
  whatever it is asked), that size is not asked for again until the SDK's size changes, and the SDK is told the
  surface's size: `HostWindow::real_pixel_size`, an `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` of our own, which the SDK
  takes on its UI thread as it takes SDL's (`WindowSDL::HandleWindowEvent`). Logged, at most five times: "the
  window's size as the SDK has it, WxH, is not its surface's, WxH".
- Windows (the client rectangle is real) and Wayland (a Wayland surface takes the size asked) are unchanged.

**Why:** on the ROG Ally (KDE Plasma at 185%, the game under XWayland in exclusive Fullscreen 1920x1080@119.93), 3
game starts of 9 kept the SDK at the scaled desktop's maximised 1038x506, an SDL size event lost in the emulated mode
switch, while the window and its surface were 1920x1080. UpdateSize asked for 1038x506 at every frame and DiligentCore
made the swap chain again at 1920x1080 each time (about 90 times a second, 947 times in one run), and the menus were
laid out for 1038x506 (squeezed) while the picture filled 1920x1080. The swap chain's own size is the truth for
drawing; the SDK's comes from events. Found by TekRantGaming's ask to look into it (Battlefield 1943 Reborn 0.2.1).

## D160 — The game's pictures drawn again when its state changes: the patcher's textures offered again (2026-11-11, Claude for Battlefield 1943)

**Decision:**
- The texture patcher (D142, `SetTexturePatcher`) gets `again`, and returning true now means the texture is the
  patcher's, whether it filled `rgba` or not (an empty `rgba` leaves the texture as it is). `GuestTextures` keeps the
  textures the patcher took (`patched_`, dropped when the patcher no longer takes one or the texture is destroyed).
- `RepatchTextures()` (any thread) counts a request. Before the next draw binds its textures (`RequestTextures`),
  each texture taken is offered again with `again` true: at once when it is not outdated (its memory watch has not
  fired, so its guest data is the data it was taken with), and an outdated one at its load (the patcher's first look
  at new data, `again` false) or, when the load finds its data unchanged, there with `again` true. So `again` always
  means the same data, and the patcher need not check it again.

**Why:** Battlefield 1943's keyboard prompts (GitHub issue 1 of the game): in a vehicle the keys that press the
controller's buttons are not a soldier's on foot (W throttles, the left trigger there; the right mouse button fires
the second weapon), so a prompt atlas patched once at its load named the zoom key (the mouse) for a landing boat's
"THROTTLE TO RELEASE BOAT". The game now asks for the atlases again when the seat, a key, a remap or the prompt style
changes, and they show the new pictures before the next draw (runs issue1_boat, issue1_live2: in and out of a landing
boat, a style and a key changed in a match). Cost: one upload of each atlas taken (at most 1 MB) per request, on
those changes only.

## D161 — The game's GPU for NativeXRenderer's device too: the SDK's vulkan_device as DiligentCore's adapter (2026-11-11, Claude for Battlefield 1943)

**Decision:** `Device::Create` gives DiligentCore the SDK's `vulkan_device` as `EngineVkCreateInfo::AdapterId`
when it is set (0 or more): the player's choice, or the high-performance GPU Battlefield 1943 points it at before the
SDK's provider is made (its `gpu_preference.cpp`, King Kong's fix: DXGI's high-performance rank or discrete before
integrated, never a software or layered device). The index counts `vkEnumeratePhysicalDevices`' order, which is both
the SDK's `VulkanInstance` order and DiligentCore's `m_PhysicalDevices`. Left at -1 (one GPU, or the first already
the best), DiligentCore keeps its own choice (discrete, then integrated), which then agrees with the SDK's.

**Why:** players with an integrated and a dedicated GPU had poor performance (TekRantGaming, 11 Nov 2026): the SDK's
provider took the first device the loader lists, the integrated GPU on a hybrid laptop. The game now points
`vulkan_device` at the dedicated one; NativeXRenderer's own device must be on that same GPU, never a second one.

## D162 — Night: the game's composite drawn as a moonlit night, before the HUD (2026-11-12, Claude for Battlefield 1943)

**Decision:** `native_x_renderer_night` (off by default; `native_x_renderer_night_brightness`, 1 the default moonlight,
and `native_x_renderer_night_haze`, the metres at which the haze hides about two thirds of the view) grades the
game's own day as a night (`src/night.h`, header only, used by `renderer.cpp`). The profile's scene copy
(`scene_resolve_caller`, D41) arms it, and it runs on the first draw of the output's size after that copy: the game's
composite, the picture with its bloom and its exposure applied (Battlefield 1943's bloom chain draws only smaller
targets in between), before the HUD drawn over it. The main view's depth is kept at the view's end (`UpscaleScene`)
for it. Per pixel, in linear light: the colour mostly taken out, a cool blue, a fifth of the light (moonlight); a
haze toward a dark navy sky with distance (the game's depth is 0.1 / distance), which also turns the day sky into a
night sky with its clouds faintly lit. The pass copies the target and draws the graded picture back (the
antialiasing's way), then tells the draw path its targets changed.

**Why:** Battlefield 1943 Reborn's Night Ops servers (TekRantGaming, 11 Nov 2026: night versions of Wake Island, Iwo
Jima and Guadalcanal in a rotation of their own, "cant be pitch black due to it being unplayable"). Graded at the scene
copy itself, before the bloom chain, the game's automatic exposure (its 1x1 luminance) brightened it back: light grey
ground and a bright cyan sea (runs night_wake2, night_gc2, night_iwo2). On the composite it stays as graded (run
night_wake3). Keeping the brightest pixels as they were (meant for muzzle flashes) kept sunlit sand, rock and grass
glowing too, so flashes are darkened like the rest and stay the brightest things on screen.

**On foot (the same day, TekRantGaming's first test: in a vehicle "honestly perfect", in first person "terrible"):**
the game clears the depth before it draws the first person weapon, so on foot the view's depth at its end held only the
weapon and everything else read as far away: all haze, a flat navy over the whole world (run nightzip1). The world's
depth is now also copied at the profile's ambient occlusion depth copy (`ao_resolve_caller`, D132: the end of the
view's opaque draws, before the weapon), once a frame, into an R32 float texture (the blitter's depth copy, whatever the
AO settings), and the haze takes per pixel the nearer of the two depths, each read with its own share of its texture.
On foot it now looks as it does in a vehicle (runs nightfp1 to nightfp4: Wake Island and Iwo Jima on the carrier, the
landing boat, AMD FSR 1 at half the resolution). The renderer's log counts the night's passes that had the world's
depth.

**Not done:** stars and a moon, the sun's shadows (they stay, as moon shadows), lights of their own for muzzle flashes
and explosions. (A Night Ops server's say is the game's: Battlefield 1943 Reborn turns the setting on when its player
joins one.)

## D163 — Night lights: the effects' own light, lamps from the level, a model's lamps, what flashes light (2026-11-12, Claude for Battlefield 1943)

**Decision:** with `native_x_renderer_night` on, the night pass (D162) also draws light, all of it in its one full
screen pass after the game's composite, plus a 4 x 2 pass before it:
- **The effects' own light** (`native_x_renderer_night_glow`, 1): Battlefield 1943 draws its emissive particles (muzzle
  flashes, tracers, fire, sparks, hit flashes, fireballs: its emitters' Emissive flag) into a half size layer and lays
  it over the scene in one draw (run nightrd2: the muzzle flash, 11 sprites, premultiplied). A colour copy of half the
  output's size after the world's opaque draws, before the scene's copy, whose last draw blended is that layer (its
  sky's half size pass draws without blending; every copy goes through the scene copy's caller): copied with its mips
  and added back as drawn plus a soft glow from two mips, the pixels of the first person weapon (drawn over the
  effects) left out. Its grey smoke and dust (drawn in the same layer) would glow white: only what is coloured or very
  bright counts (`Hot`).
- **What the effects light:** the pass before finds up to four of the brightest hot places of that layer's coarse mip
  (about 48 texels across), each at the nearest surface round it (its depth), a little toward the camera: point lights
  for the night pass, strength from the light found, capped.
- **The level's lamps** (`NightLamps`, `native_x_renderer_night_lamps`, 1): the game reads them from the level's own
  files (Battlefield 1943 Reborn: night_lamps.cpp, Wake Island's 92 lampposts with two downward lamps each and its
  bomb lamp, 185 lamps). Each frame the nearest 24 whose light reaches into the view (a sphere against the frustum's
  planes) light the surfaces, and the nearest 24 bulbs in view glow where no nearer pixel hides them.
- **A model's lamps** (`NightModels`): a main view draw with a given index count (a jeep body's levels of detail) is
  that model; its world matrix is three rigid rows in its vertex shader's constants from a given register
  (Battlefield 1943: c16 the camera, then a matrix per part from c17 in the parts' alphabetical order; the jeeps' and
  the landing boat's main body the third, c23 to c25, run nightboat4); its lamps are placed each frame from it.
- **Where a pixel is:** the terrain's world-view-projection (D151's capture, now also at night) kept at the AO's
  moment; the world from its x, y and w rows' inverse (the depth partitions differ only in z), the stored depth 0.1 /
  distance, DiligentCore's viewport the Direct3D way round (NDC y +1 at the top: run nightlamp2 had it upside down).
  Its facing from its neighbours' places. A surface's colour for the light is its day colour with the sun's bright and
  shaded brought closer.

**Why:** TekRantGaming, 12 Nov 2026, after testing Night Ops: no muzzle flash and no bullet, explosions muted with no
flash, "use our global illumination to light a small area around the player when they fire", the telegraph poles'
lamps on Wake Island and any other light there, the jeeps' headlights; night only, cheap.

**Results:** Wake Island at night: the lampposts' lamps glow and light the ground, rocks and sandbags round them in warm
light (nightlamp4); tracers, the muzzle flash and hit flashes bright and coloured again (nightfire4); each shot lights
the gun, the hand and what is near in orange (nightfire7); lamps placed on the landing boats move with them and light
the cranes ahead (nightboat5, a test's lamps). GPU 11.41 -> 11.91 ms a frame at 3200 x 1800, RTX 4070 Ti, the lamps in
view (nightlamp5off, nightlamp5); none of it runs in the day.

**Not done:** an explosion not yet seen in a test (the grenades fell beyond the carrier's deck); the jeeps' headlights
not seen on a jeep (no jeep reached in a test; the placement checked on the boats); fires' own flickering light.

## D164 — A pixel shader's own position given as the host pixel's centre for a game that samples one texel on (2026-11-12, Claude for Battlefield 1943)

**Decision:** a game profile may set `param_gen_texel_centre`. Its pixel shaders that read their pixel position
(Xenos PsParamGen, the shader modification's param_gen_enable) then get, on each axis, (floor(position) + 0.5) /
render scale - 1, at least 0, instead of the SDK translator's floor(position) / render scale: the SDK translator's
new modification bit `param_gen_texel_centre` (sdk/src-bf, spirv_translator.h and .cpp). A stored pipeline or
translation of such a shader takes this run's way when the storage is read, so lists stored before still match the
draws. `native_x_renderer_param_gen_texel_centre` (on) is the switch. Battlefield 1943's profile sets it.

**Why:** TekRantGaming, 12 Nov 2026: at Wake Island's ship spawn "a black outline cel shading is around the ship" when
looking at the water, and palm leaves and buildings with "a slight white hue glow around them" from some angles. A
RenderDoc capture of the spawn's view (run rdship1, 3200 x 1800, render scale 2.5): two host rows of the water right
above a landing craft's hull were (0, 0, 0, 0) at the scene's end. Their history: the water's draw (41,352 indices,
premultiplied blend) wrote 0 there, opaque water everywhere else. The water reads the scene's depth copy at
(VPOS + 1) / (1280, 720) (its constant 26 and an offset of one texel, from the shader's trace at the pixel): with
VPOS = floor(position) / 2.5 that is host texel h + 2.5, two rows down, inside the hull, so the water there saw
nothing behind it and drew nothing over the black under it. The game's half resolution layers (the sky and haze,
the effects) are composited the same way: half a texel of the half size on, one texel of the full size, so with the
SDK's position they lay a texel of the half size down and to the right: haze over the edges of what stands against
the sky. On the Xbox 360 the same reads were half a pixel off at 720p, a one pixel line; with the host pixel's centre
less one guest pixel the read is at the host pixel itself, at any scale and at 1.

**Results:** the same view (runs lang1 before, d164a after): the rows above the hull from (19, 44, 71) and (36, 40, 46)
to the water's own (49, 99, 146) and (66, 108, 150); the black line along the hull gone; the hills' edge against the
sky without its dark line and the palms without their haze fringe.

D163, tuned (2026-11-12 morning): right under a lamppost the ground looked like the day, sun shadows and all (run
nadenight2, Guadalcanal's spawn 5 m from a lamppost, looking down: the view's mean 112 of 255 against 54 with the lamps
off, nadenight3). A lamp's light now rolls off with its own strength (lamps / (1 + 1.5 * its brightest channel)), and
the surface's colour for a light takes less of the day's sun (lin * 0.5 / (luma + 0.2), was 0.6 / (luma + 0.3)): the
same view 89 (nadenight4), a warm pool rather than the day; light seen from afar is weak enough to keep its strength.

## D165 — A resolve the game reads on the CPU: its guest bytes set to zero at the call (2026-11-13, Claude for Battlefield 1943)

**Decision:** a game profile may name resolves whose destination the game then reads on the CPU
(`Profile::zeroed_resolves`: the Resolve call's return address; its caller's when every resolve goes through one
function of the game's, 0 for any; the destination's size, 0 for any; its guest format, -1 for any). For a matching
resolve the destination's guest bytes are set to zero in `CaptureResolve`, on the game's own thread as it makes the
call, when they are not zero already. The resolve itself stays on the host as every other (D70). `NXR_RESOLVE_LOG`
logs each kind of resolve (caller, its caller, size, format) once and, for the small ones, how many found guest
bytes that were not zero; `NXR_D165_OFF` leaves the bytes alone (to compare).

**Why:** TekRantGaming, 13 Nov 2026: "when you first play your first game it looks perfect but ... the longer I play
and especially on maps that are not wake island ... the extra vegetation starts to behave strangely and starts
appearing on roads and places it should not be". Battlefield 1943 has its own undergrowth: for each cell near the
camera it draws a 64 x 64 mask (guest format 6), resolves it, and CPU jobs read the texture's memory to place the
plants. A resolve never reaches guest memory here (D70; D140 writes only what a draw reads as vertices), so the jobs
read whatever the memory held: zeros in a fresh process, so the first level has no game undergrowth (the look
players know, with the grass of D151), and from the second level on the heap's leftovers: plants on roads and in
yards, some black. Run veg2 (Wake Island, then Guadalcanal in the same process, D165 off): Wake Island's 340 mask
resolves all found zeros; Guadalcanal's found guest bytes set in 260 of its first 260 (15973 of 16384 bytes in
one), and the Japanese base's yard had grass clumps and black fronds on its sand (shot_180). Reading the resolve
back into guest memory instead would bring the console's own undergrowth, on top of D151's grass and at a readback
for every cell: not what players have had since the first release, and not asked for.

**Results:** run veg3 (the same two levels, D165 on): the first dirty mask is logged ("held 15621 bytes of another
use: set to zero before the game reads it"), every one after it is zeroed, and the same base's yard is bare sand.
Wake Island as the first level is unchanged (its masks were zero already: nothing is written). Battlefield 1943's
profile names the masks by the game's resolve function (0x82586834, the scene copy's too), 64 x 64, format 6; they
come from two places in the game (0x827A1124 and 0x827A1150), any caller is taken.

## D166 — Ultrawide without hooks round the 2D views: the HUD placed draw by draw (2026-11-12, pull request 11 of the players' repository, for Battlefield 1943)

**Decision:** `Profile::box_2d_after_composite` (with `widens_cameras`, D43) for a game whose frame is the scene's copy, a
bloom chain into smaller targets, one composite of the output's size, then the 2D layer. After the composite every
draw of the output's size is a 2D draw, and each is placed on its own (`DrawPath::SetHudClassify`, `ClassifyHudDraw`):
- Its place comes from its inline quad (Battlefield 1943: corners -1..1, or the canvas's pixels) and its vertex shader's
  constants, tried as two rows (x' = r.(x, y, 0, 1)) or a column major matrix; a transform with no x or y term is none.
- Across the whole canvas: stretched with the 3D view. Past both edges (a quad larger than the screen): drawn twice,
  stretched then boxed (`kOversize`). From one canvas edge (and wider than 0.15): from the screen's edge to where its
  inner edge is in the box, the canvas's edge pinned to the screen's (`kExtendLeft` / `kExtendRight`).
- A mesh from vertex buffers with a 3D matrix (third row not (0, 0, 1, 0), the scope's mask): drawn as the 3D view.
- A point the game projected to its HUD (`NoteHudMarker`, from the game's world-to-HUD functions) in the last 0.2 s:
  its centre where the full width puts it, its size the box's (`kPoint`).
- Inside an edge-anchored panel drawn before it: where the panel went. The next glyph or part of a line (same shader,
  same row, starting where the last ended): where the last went. Otherwise, with `native_x_renderer_hud_layout` edges,
  its centre beyond 0.4 of the canvas: boxed against that edge; else in the middle box.
- A menu frame (a backdrop over exactly the canvas, or the game's `NoteGameMenu` in the last 1.5 s, and no 3D overlay):
  no edges, no markers. A frame with no scene's copy (a loading screen) is placed the same way.
`native_x_renderer_debug_hud` logs the decisions every 10 s.

**Why:** the 0.4.0 ultrawide work (12 Nov 2026), played at 3440x1440: a boxed 16:9 HUD was asked for, then the HUD at the edges. Every
Battlefield 1943 HUD element is the same 6-vertex quad placed by its constants (run 12 Nov 20:17), so the raw vertices
said nothing; the scope's mask is a 3D mesh; the flags' markers are projected by the game with the widened camera into
its 1280x720 HUD; the in-game HUD draws a large invisible container, so area was no sign of a menu.

**Added when it was merged into Battlefield 1943 0.4.0 (2026-11-13, Claude for Battlefield 1943), from its review and
the first runs at 21:9, 32:9, 16:10 and 4:3:**
- A scissor never starts left of or above the target (`on_target` in `DrawInternal`): an edge anchored draw maps the
  canvas's x 0 to a negative host x, and Vulkan requires a scissor offset of 0 or more.
- A cube resolve's texture is square on the host: with a scale per axis both axes take the smaller one
  (`ResolvedTextures::GetOrCreate`) and the resolve scales its copy into the face (`ExecuteResolve`). Before, the
  texture could not be made (width 258, height 192 for the game's 128x128 cube at 21:9) and the cube stayed as it was.
- A world marker (`kPoint`) is moved down as well as across: on an output taller than 16:9 the box sits in the middle
  of the height, and a marker moved across only sat up to 5 percent of the height off its flag.
- How a draw's place was found is a kind and a register, worded only for the debug log: no string per HUD draw.

## D167 — Pull request 16's renderer part: SMAA presets, Intel CMAA 2, a TAA, AMD FSR 4.1 and 3.1 from the FSR SDK 2.4.0, Intel XeSS 3, HUD scale, world marker pieces (2026-11-13, ThatZaliaSGuy, for Battlefield 1943 Reborn 0.4.1)

**Decision:** taken from Battlefield 1943 Reborn's pull request 16 (merged at TekRantGaming's word, reviewed line by
line):
- Antialiasing on the finished picture: FXAA, SMAA at the reference implementation's four presets, and Intel CMAA 2
  (two presets) as compute passes ported to GLSL (`src/cmaa_shaders.h`, Apache 2.0). It switches while the game runs.
- Temporal upscalers on the 3D view before the game's post-processing: AMD FSR 4.1 and FSR 3.1 through the FSR SDK
  2.4.0's loader (`amd_fidelityfx_loader_vk.dll`, which finds `amd_fidelityfx_upscaler_vk.dll`), Intel XeSS 3
  (`libxess.dll`; its instance and device extensions and features added when an XeSS mode is chosen), NVIDIA DLSS as
  before, and a plain TAA of the renderer's own (`src/taa.cpp`). The libraries are fetched when configuring, each
  checked by its SHA256, copied next to the game and loaded only from its folder. FidelityFX SDK 1.1.4's library,
  which frame generation used, is no longer fetched (frame generation has not been offered since 9 Nov).
- HUD scale and minimap size for the boxed HUD; a world marker's pieces follow its point across a wider screen only
  when they are marker sized (a plane's gunsight came apart on ultrawide: the fix of the withdrawn pull request 12);
  a draw from the crosshair sheet is never a marker.
- An HD pack may hold the HUD's, the deploy screen's and the icon atlases' pictures; the game draws its prompts and
  flag letters into the pack's picture at the pack's size.
- While anisotropic filtering is forced (2x and up), a game's negative texture LOD bias is dropped.

**Changed in the review (Claude for Battlefield 1943 Reborn):** an HD pack entry with impossible sizes is left out
(16384 texels a side, 16 levels, 1 GB; a crafted pack wrapped a size check and crashed every start), the HUD's HD
copies are at most 8 times the game's size, the vendor libraries are never loaded by a bare name (no executable
path: none), and XeSS is used only when its library has every function called.

**Not verified at the merge:** XeSS adds its feature structures to DiligentCore's chain without removing duplicates
(Intel's header asks the caller to); FSR 4 itself (it needs a Radeon RX 9000: elsewhere AMD's library runs FSR 3.1
under the "FSR 4" name).

**Seen in play after the merge (13 Nov 2026, a GeForce RTX 4070 Ti, Wake Island, one run a mode, runs v41g-*):**
SMAA High, CMAA 2 and CMAA 2 Ultra, FSR 3.1 and "FSR 4" (AMD's library offers 3.1.6 and 2.3.4 on this GPU and runs
3.1.6), XeSS (the device's chain held each feature structure once: 1000059000, 1000351000, 51 and 53; Intel's library
from the SDK 3.0.2 tag, its hash as pinned, reports its upscaler as 2.0.2), DLAA and TAA: each applied every frame in
play, a clean picture, no errors. FSR 4 on a Radeon RX 9000 and XeSS on other drivers are still not seen.

## D168 — The picture in picture sniper scope (2026-11-13, Claude for Battlefield 1943 Reborn 0.4.1)

**Context.** TekRantGaming asked for "a modern sniper scope implementation" as a switch in the game's new Settings >
Enhancements page, "Picture in picture", off by default and marked experimental. The game draws one view a frame: with
a sniper scope up, the whole 3D view is the zoomed one, composed black but for a round view, and the reticle is drawn
with the HUD. There is no second, unzoomed view to put round a lens.

**Decision** (src/scope.h, renderer.cpp, draw.cpp; the game's side in bf/src/fov.cpp):
- The game reports its camera every frame while the switch is on and the soldier is on foot (its own input mapping's
  seat): right, up and back, the tangent of half its own field of view, and the zoom (the half angles' tangents, now
  over its own: a sight's about 0.6 to 0.8, the Springfield's 0.22). The renderer watches each frame's draws while it
  does.
- Not zoomed (above 0.97): at the game's composite (the first draw of the output's size after the scene's copy) the
  finished scene is kept with its camera, into a half float texture whose alpha is the world's share: 0 on the first
  person weapon, which the view's depth at its end holds alone on foot (nearer than 3 m, the rest cleared to far:
  D162's test). Its mips are made once when the scope comes up.
- Zoomed as a sniper scope (below 0.6): right after the game's composite of the zoomed view (vertex shader
  962547C8BB566F8B with pixel shader 8DD620FF8D8BB8FA at the output's size; while the scope comes up, its black pass
  589877255C25EA63), one pass: a lens of 0.3 of the height keeps the middle of the game's picture; round it the kept
  view, turned to where the camera looks now, a coarser mip, 45% darker, the weapon's gap filled by push and pull (a
  coarse level's world colour refined by each finer level by its share); a dark rim. The HUD is drawn over it as always.
- The reticle, eight of the HUD sprite shaders' quads (BCFA50F3EA22BAE7 with 90AC5F14E9463FB2) stretched by their
  constants into lines across the screen, is cut to the lens's square while the lens is drawn: a quad a quarter of the
  screen or more one way in clip space (DrawPath::HudClipExtent, the HUD classifier's transform search without its
  side effects). Its lines run through the centre, so the square cuts them at the circle.

**Findings on the way** (runs v41scope5 to v41scope27):
- The first hook, the full screen pass after two half size ones, is the scope's raise only. Steady scoped frames are
  the composite and the HUD: armed at the composite, the pass ran in 9 of about 470 scoped frames (v41scope12); armed
  every frame, in all of them (v41scope17: 471 of 600, the rest the raise).
- The composite's shaders are the same without a scope (seen 600 times in 600 frames, v41scope26), so the zoom alone
  tells a scope's frame from another, and only on foot: a tank's view is 0.9 (v41piptank) and the air raid's bomber
  view is a seat of its own.
- A research probe copying from outside a texture (the luminance chain's 3 x 3 one) lost the Vulkan device
  (v41scope13); the probes keep inside each texture now.
- With the switch on and no scope, the GPU's frame time is the same as with it off (about 10.1 ms both, 3200 x 1800,
  GeForce RTX 4070 Ti, v41pipcost-false and -true).

**Research switches:** NXR_SCOPE_RESEARCH=N logs the Nth scoped composite's frame (every draw after the composite,
every resolve with its destination, the front buffer shown, and the output's pixels after each draw);
NXR_SCOPE_DEBUG=1 paints the world round the lens red; NXR_SCOPE_MASK=vs:ps,... adds shader pairs to follow.

**Limits:** the world round the lens is a still from when the scope came up, turned with the aim: what moves there
stays where it was until the scope comes down, and a long turn reaches past its edge (shown fading). The weapon's gap
is a blur of what was round it. The shaders are Battlefield 1943's own.

**Not in Battlefield 1943 Reborn 0.4.1:** taken out of the update at TekRantGaming's word (13 Nov 2026, 20:20, "drop
the sniper change from this update"). The renderer's part stays: it runs only while the game reports its view, which
a release build never does (the game's CMake BF_SCOPE_PIP, off by default: no Enhancements row, no report).

## D169 — Night: a sky of the renderer's own (stars, the Milky Way, a moon), each level's own (2026-11-14, Claude for Battlefield 1943 Reborn)

**Context.** TekRantGaming's night maps brief (14 Nov 2026): a unique night sky per map, stars, moon, clouds and ambient
level, darkness kept. D162 grades the game's day sky into a flat dark blue; there was no sky of the night's own.

**Decision** (src/night.h, renderer.cpp, draw.cpp, grass.h; the game's side in bf/src/night_lamps.cpp):
- The night pass draws the sky where the view shows nothing within 5 km (the stored depth under 2e-5): stars on three
  layers of cube face grids (sparse bright, then denser faint ones, more of them in the band), a faint patchy Milky Way
  with darker lanes, a moon disc with a little texture, a darker rim and a soft halo, and a haze low on the sky. Where
  the day sky was a grey or white cloud (little saturation) the night sky is dimmed to the level's share, so the
  game's own clouds keep covering it; it fades out toward the horizon.
- Its directions come from the sky dome's draw (vertex shader 00AB67BFF5297087): its camera at c0 to c3, rows, clip =
  row . (direction, 1) without translation (the dome goes with the eye). The terrain's camera (D163) is missing when
  the view looks up at the sky with no ground in it (research run nsky6: frame 2100 had the dome and no terrain), so the
  dome's is taken whichever way the view looks, the first of each frame (DrawPath::SetSkyCapture, SkyCamera), and the
  terrain's is the fallback.
- NightSky(NightSkyStyle): the level's look, kept until set again: the moon's direction, size and colour, the stars'
  brightness and number (the faint layers go first), the Milky Way's strength, the horizon haze's colour and height,
  the sky's share behind clouds, a seed for a different star field. The game sets it per level folder when the level
  loads: Wake Island a clear Pacific night with a bright Milky Way and a small white moon; Guadalcanal damp, fewer
  stars in a grey green haze, a bigger yellowed moon low; Iwo Jima dusty, few stars, a low reddened moon, a warm brown
  horizon. native_x_renderer_night_sky scales it all (0 none).
- Tests: native_x_renderer_night_debug 3 shows the sky test (red), the clear sky test (green) and the direction's
  height (blue). NXR_SKY_PROBE=1 or 2 logs the shaders and matrices drawn at the output's size, 3 the dome's vertex and
  pixel constants (the research).
- The moon stands where the game's sun is: the dome's pixel shader constant 9 is a unit vector, the light's direction
  (from the sun); its negation is toward the sun (Iwo Jima: 8.5 degrees up). A moon put there rose over the sea's
  glint (run niwo3), so the game's own shadows, the sea's glint and the moon agree on every level
  (NightSkyStyle::moon_at_sun; the level's moon_direction when the dome gives none). The moon is dimmed only a quarter
  where the day sky was white (near the sun it reads as cloud), and shows down to the horizon.

**Checked** (runs nsky9 to nsky11, ngc1, niwo1 to niwo4, nday1, nperf_sky0 and 1): stars straight overhead (nsky11; with
the terrain's camera alone that view was blank); the three skies distinct; a day map unchanged (no stars, no grade);
the frame interval at the display's cap (11.11 ms, 90 Hz) with the sky on and off, 3200 x 1800, GeForce RTX 4070 Ti.

## D170 — Night: firelight that wavers, turning beams, and the levels' buildings lit (2026-11-14, Claude for Battlefield 1943 Reborn)

**Context.** The same brief: purposeful lights on buildings, bases, docks and machinery, fire that reads as fire.

**Decision** (renderer.cpp BuildNightCamera, native_x_renderer.h NightLamp; bf/src/night_lamps.cpp):
- NightLamp gains `flicker` (0 steady to 1 a fire's) and `spin` (radians a second round the world's up axis). Each
  frame's copy of the lamps is animated before they are chosen: three unrelated sine waves and a slow breath scale a
  flickering lamp's colour (never below 0.3 of it, never a strobe); a spinning spot's axis turns. Each lamp's phase
  comes from where it stands, so no two fires waver together and every player's game sees the same.
- A long light (range over 60 m) falls off with its range (1 / (1 + 100 d^2 / range^2)) instead of a lamp's (1 / (1 +
  d^2 / 4)), under which a beam was nothing at 150 m.
- The game's table of lit models gains colours (fire, lantern, red aviation light, the default warm electric lamp), a
  hanging work lamp shape, and a share (the houses: 45% lived in, chosen by where they stand). New: the houses' fire
  emitters, Iwo Jima's lighthouse (two beams turning over the sea, dipping a quarter so they reach the water 150 m out), the control towers' cabs and roof lights, the guard
  and watch towers' lanterns, the radio masts' and water towers' red lights, the hangars' and sheds' work lamps, the
  repair platforms', lanterns on the jetties and Guadalcanal's harbour, dim lanterns inside the bunkers and pillboxes,
  lit rooms in some houses, the storerooms and factories. Each from the model's own mesh part bounds
  (tools/level_objects.py for where they stand).
- No shadows: a light inside walls is kept within them (its range shorter than the distance to the ground outside),
  so it shows through doors and slits without lighting the ground on the far side of a wall.

**Findings:** the houses' fire emitters (Houseeffects_Fire, 42 on Wake) stand inside the houses and draw no flames
while the house stands (runs nfire1 and nfire2, night debug 2: only the doorway lit), so they are a stove's glow, not
firelight. Explosions at night (run nnade2, Guadalcanal, grenades every 3 s): the fireball bright and coloured, the
ground round it lit, its smoke and dust moonlit; D163's open item closed. Lampposts 5.5 from 7 (a wall beside one read
close to daylight, ngc1).

**Tests:** BF_DEV_CAMERA_POINTS=x,y,z,tx,ty,tz;... (the game's look.cpp, dev builds): the camera at each stop looking at
its point, with BF_DEV_CAMERA_TOUR's dwell; the view's row 2 is its back (BF_DEV_CAMERA_AT had the point behind).

## D171 — Few cores: no recorder thread and no spinning waits; the temporal upscalers' libraries not asked (2026-11-15, Claude for Battlefield 1943 Reborn)

**Why:** Battlefield 1943 Reborn's weak processor test (its client held to two cores of a Ryzen 7 7800X3D, one thread
each, Wake Island at 90 FPS, 50 s) went from a 1% low of 82.5 FPS on 14 Nov to 34 to 37 FPS after pull request 28's
recorder (the draws' DiligentCore calls on a thread of their own): on two cores that thread competes with the game's own
threads. With every core free it gains nothing where the GPU is the limit, and helps where the renderer's thread is
(the same PC at render scale 1: 1% low 196.9 FPS with it, 163.7 without; renderer CPU 2.21 against 2.82 ms a frame).

**What:**
- cpu_cores.cpp: `AvailableCores()`, the physical cores among the processors the process may run on (its affinity; a
  dual core with Hyper-Threading is 2), read again at most once a second.
- The recorder's thread starts only with at least `native_x_renderer_record_thread_cores` (4) cores
  (`native_x_renderer_record_thread` still turns it off everywhere); the choice is logged.
- The command queue's and the recorder's waits spin before they sleep only with at least `native_x_renderer_spin_cores`
  (4) cores; on fewer the thread a spin waits for usually needs the very core the spin holds.
- `native_x_renderer_check_temporal` (on): the start's question to AMD's FSR and Intel's XeSS libraries about what this
  GPU runs (for a menu's upscaler choices) can be left out. Battlefield 1943 Reborn offers no temporal upscaler since
  0.4.4, turns it off and no longer ships those libraries (161 MB).

**Measured** (two cores, 90 FPS cap, 50 s each; others' load 0.7 to 1.0 core):

| | 1% low | 0.1% low | frames over twice the average | client CPU |
|---|---|---|---|---|
| Recorder on, spins (before) | 34.0 to 36.7 FPS | 21.8 to 23.0 | 19 to 26 | 1.28 cores |
| Recorder off, spins | 60.7 to 78.0 FPS (four runs) | 20.3 to 38.6 | 2 to 7 | 1.00 to 1.04 |
| Recorder off, no spins (now) | 70.9 to 74.1 FPS (two runs; a third, 35.1, had a 240 ms stall) | 29.2 to 30.0 | 3 to 5 | 1.03 |

Four cores at 90 FPS: 1% low 89.4 FPS with and without the recorder (it stays on). All sixteen threads, no cap:
229.7 FPS, 1% low 193.9 (223.6 and 189.1 before; the GPU is the limit). Unlimited frame rate on two cores stays poor
either way (47 to 71 FPS average, 1% low about 13): the processor is simply full.

## D172 — The dynamic heap at 64 MB, not 256 (2026-11-15, Claude for Battlefield 1943 Reborn)

**Why:** DiligentCore's dynamic heap (the draws' constant buffers and the uploads, `EngineVkCreateInfo::DynamicHeapSize`)
is one host-visible, host-coherent buffer made at the start and kept: on a discrete card that is pinned system memory,
on integrated graphics memory shared with the system. At 256 MB it was the third largest block in Battlefield 1943
Reborn's process (all of it resident), for a game that also wants to run on 8 GB PCs. Measured on a Ryzen 7 7800X3D
and RTX 4070 Ti (DiligentCore warns "Space in dynamic heap is almost exhausted" whenever it must wait for the GPU to free
space): 16 MB, 314 such waits, all during the level's load; 32 MB, 91, all during the load; 64 MB, none on any of the
four maps, nor with the HD texture pack, nor over two 14 spot camera tours at the Ultra settings with the pack, and the
load took as long (7.3 s on Wake Island). Play itself never needed 16 MB.

**What:** `native_x_renderer_dynamic_heap_mb` (64; 8 to 1024) sets the size. The working set in a match went from
2.9 GB to 2.7 GB (private memory 4.58 to 4.39 GB), the frame rate the same. If a game needs more, DiligentCore only
waits for the GPU (and says so in the log), it does not fail.

## D173 — The pipeline cache let go once the stored pipelines are compiled (2026-11-15, Claude for Battlefield 1943 Reborn)

**Why:** DiligentCore's pipeline cache (D42, D45) holds the driver's binaries of every pipeline compiled with it:
80 MB for Battlefield 1943 Reborn's 2,200 stored pipelines, in system memory for the whole run, while a match uses a
few hundred. The drivers keep caches of their own on disk: on an RTX 4070 Ti, a start with no pipeline cache at all
compiled the 2,200 in 594 ms, with it 640 ms.

**What:** once the stored pipelines are compiled and nothing is left unsaved (a first start saves the cache first,
D135), the renderer lets go of the cache (`native_x_renderer_keep_pipeline_cache`, false, keeps it); compilations in
flight hold their own reference. The file is still read at the next start, for a driver without a cache of its own;
pipelines new in play are compiled without it and not added to the file (their records are, D42). Measured, a start
with the cache file there: the heap 608 to 527 MB resident, the working set 2.62 to 2.54 GB, the same frame rate.

## D174 — The guest memory mirror as a sparse buffer (2026-11-15, Claude for Battlefield 1943 Reborn)

**Why:** the mirror of the guest's 512 MB of physical memory (D35, the SDK's SharedMemory design) was an ordinary
buffer: 512 MB of graphics memory on every card, a quarter of a 2 GB one, though only what the draws read is ever
copied in. Battlefield 1943 Reborn wrote 116 to 124 MB of it in 64 KB blocks (228 MB in 4 MB blocks) over 14 spot
camera tours of Guadalcanal and Wake Island. The SDK's Xenos path binds its own shared memory sparsely for the same
reason (tested here with that off: +504 MB).

**What:** where the device has sparse buffers, timeline semaphores and a queue that binds sparse memory
(`SparseResources` and `NativeFence` are now asked for, optional), the mirror is a `USAGE_SPARSE` buffer and
`GuestMemory::EnsureMirror` binds 1 MB of a sparse `IDeviceMemory` to each 1 MB block before anything first writes
there: the three copies into the mirror (`Upload`, `UploadUnwatched`, `WriteIfChanged`) and the two compute passes that
write resolves into it (D140, D155; a span rounded up to 32 rows of 16 bytes a texel). The bind is recorded in order
with the copies (the recorder's `Do`): DiligentCore flushes the context, binds, signals the fence, and the context's
next submission waits for it on the GPU. Blocks are never unbound. Without the support, or with
`native_x_renderer_sparse_guest_memory` false, the full buffer as before; the log says which and why. A draw reads only
ranges requested first, which are copied (so bound) before; an unbound block reads as zeros.

**Measured** (RTX 4070 Ti, Wake Island): the game's dedicated graphics memory 2,085 MB to 1,745 MB, 162 MB of the
mirror bound after a minute of play (164 MB on a Guadalcanal tour with the HD texture pack); every draw drawn (836
of 836); screenshots of the same camera spots the same; no errors. Frame rates in interleaved runs, sparse against full:
all cores 225.2 and 228.4 FPS against 218.2 and 229.3; two cores 170.7 and 175.0 against 172.6 and 171.1 (the same
within noise).

## D175 — Night: the lights follow the day and night cycle; the old night look only on request (2026-11-16, Claude for Battlefield 1943 Reborn)

**Why:** pull request 32 (D-numbers in its own notes: the renderer's own sky, clouds, weather and the day and night
cycle, `SkyLightAt`) darkens the game's light at night with a moon and stars of its own, so the separate night maps
(D162's moonlit grade over the finished picture, D169's sky) are no longer wanted: TekRantGaming, 16 Nov 2026, the night
maps removed, the lights kept, and Night Ops servers kept as the cycle held at night. The lights (D163: the levels'
lamps, the effects' light, the models' lamps; D170: firelight, beams, lit buildings) ran only with
`native_x_renderer_night`, which nothing turns on any more.

**What:** `g_cycle_dark`, set with the outdoor light from the world's hour (`SkyLightAt`'s sun elevation): 0 by day,
a smoothstep from 4 degrees up to 6 degrees under, 1 after; 0 when the world has no day and night. `NightLightsAmount`
is 1 with `native_x_renderer_night` (the old look, kept for tests), else that darkness, and `NightPassOn` (above 0.01)
replaces the cvar at every switch of the night pass: the world's depth copy, the effects' layer, the night camera,
the models' lamps, the pass's arming and its log. The pass's new `NightSettings::grade` (the shader's `skyp.z`) is 1
for the old look and 0 for the cycle: the scene stays as the cycle drew it (no moonlit grade, no haze, no D169 sky),
and only the lamps, the lamp glows and the effects' light are added, their strengths times the darkness. D169's sky
capture stays with the old look only.

**Tested** (Battlefield 1943 Reborn dev build, local servers, Wake Island at the airfield): a Night Ops server's world
held at 0 h, 24 lamps lit round the player and their bulbs glowing (run n175isl); a normal server at 18:45, the
airfield's lamps lit (c175dusk); at noon no night pass at all (c175noon).

## D176 — Night: the old night look removed, the lights kept (2026-11-16, Claude for Battlefield 1943 Reborn)

**Why:** after D175 nothing switched the separate night look on (D162's moonlit grade and haze over the game's day,
D169's night sky and each level's sky style): the day and night cycle draws the night. Kept "off", it was a second
night beside the cycle's. The lights (D163, D170) stay and follow the cycle (D175).

**What:** gone: `native_x_renderer_night`, `native_x_renderer_night_brightness`, `native_x_renderer_night_haze`,
`native_x_renderer_night_sky`; the night pass's grade, haze and sky (its shader's stars, Milky Way and moon, their
constants); the sky dome's camera capture (`SkyCamera`, `SetSkyCapture`); `NightSky` and `NightSkyStyle` in the
public header; in the game, each level's sky style and the menu's switch for a Night Ops server's night. The night pass
now only adds light to the scene as the cycle drew it: the lamps, their bulbs' glows, the effects' light.
`NightLightsAmount` is the cycle's darkness alone: with the sun's elevation 70 sin(pi (hour - 6) / 12) degrees, the
lights start at 17:47, are full at 18:20, start to fade at 05:40 and are out at 06:13. A bulb's glow still dims with
distance as the old haze had it (a third left at 700 m), now a constant. `native_x_renderer_night_lamps`,
`native_x_renderer_night_glow` and `native_x_renderer_night_debug` (1, 2) stay.

**Tested** (dev build, local play, Wake Island, the camera at an airfield lamppost with BF_DEV_CAMERA_POINTS): 17:30 no
night pass, the lamps unlit (run lampc_175); 18:00 the lamps coming on, 24 lit (lampc_18); 0:00 full (lampc_0); 06:30
no night pass (lampc_65). Not tested: a Night Ops server, other maps, the models' lamps (jeeps), the effects' light.

## D177 — Night's lights: a bulb's glow, sodium street lamps, the ground lit as by day's colours; tank headlights (2026-11-16, Claude for Battlefield 1943 Reborn)

**Why:** the user, 16 Nov 2026: "The light balls look goofy ... have the light sources glow and cast light like sodium
... lightbulbs would from street lights, along with tanks and wheeled vehicles". The bulbs' glows were soft balls with a
faint flat disc round them, and at the cycle's midnight a lamp lit almost nothing: a surface's colour for the light was
taken from the picture, which the cycle had already darkened.

**What:**
- The glow (night.h): a small hot core (white hot in its middle) and a thin halo falling off to nothing at its edge,
  six times the bulb's radius; no ball, no disc. A lamp with a direction (a hooded street lamp, a headlight) shows its
  bulb to those it shines toward, less from the side, only its spill from behind.
- The lit surface: `NightSettings::scene_gain` (1 by day to 12 at full dark) brings the picture back to its day
  brightness before its colour is taken for the lamps' light.
- The game (night_lamps.cpp): the lampposts and the work lamps under roofs are sodium lamps (linear 1, 0.56, 0.17),
  the lampposts stronger and reaching 22 m. The Sherman (two headlights) and the Chi-Ha (one) join the jeeps as models
  with lamps: their one material's draw by its index counts (29745, 14685, 7416; 29418, 14535, 7281), the Hull's
  matrix at c26 and c17. BF_DEV_TANK_LAMPS (dev builds) moves their headlights for placing by eye.

**Tested** (dev build, local play, Wake Island's airfield at 0 h): the street lamps' amber pools on the ground and the
walls, small bright bulbs (runs lampd_0, tank_a1). NOT tested: any vehicle's lamps. Local play spawns no vehicles at
the airfield, so the tanks' index counts, matrix registers and headlight positions are from the game's files and real
dimensions only, and the jeeps' were not seen either.

## D178 — Night's lights: a vehicle's lamps only while someone is in it; a coloured bulb keeps its colour (2026-11-16, Claude for Battlefield 1943 Reborn)

**Why:** the user, 16 Nov 2026: "If there were colored lights, retain their color ... Vehicle lights should only be on
when the vehicle is occupied". D177's bulb core was mostly white whatever the lamp's colour (a red mast light's bulb
read pink), and every jeep and tank drawn had its headlights on, parked and empty ones too.

**What:** the bulb's core is the lamp's colour, 30% toward white in its middle (was 70%). A model's lamps
(`NightModels`) are placed only when the model is taken as occupied: the player's own, while the game says the player
sits in a vehicle (`NightVehicleSeated`, from Battlefield 1943 Reborn's LocalSeated on every pad read), is the model
with lamps nearest the camera within 14 m; any other, one that drives (over 0.5 m/s between frames, its lamps kept 6 s
after it stops), followed frame to frame by where it is. The game gives no other players' seats to the renderer: a
vehicle another player sits in without driving stays dark, and one pushed or thrown by a blast lights for 6 s.

**Tested:** the street lamps and the red mast light at Wake's airfield at 0 h (run lampe_0). NOT tested: any
vehicle (none spawn in the scripted local play); see D177.

## D179 — The renderer's sky kept on frames with an effect; the effects' light without the layer's ground (2026-11-16, Claude for Battlefield 1943 Reborn)

**Why:** the user, 16 Nov 2026: "when shooting and explosions trigger, they hide our sky and show the original sky",
"sky is flashing to the muzzle flash or explosion effects". With a sky of the renderer's own the game's sky draws are
passed over by their pixel shaders (draw.cpp, SetSkipGameSky). On a frame with an effect drawn (a muzzle flash, an
explosion) the game draws its sky's tint and painted clouds with another shader, 10FA030CD9308DE2 in place of
471C5A0705715DB7 (and A6B4F7A4D9245222, 91B1B2D558571E77, 234707FCE022853D in place of 579628CCAF403E2B,
560DAF673AF554F2, 49750AD0B93EF9C5, which are not the sky's): it was not on the list, so the game's sky came back for
those frames, over the day's sky and brightest at night (Wake Island's deck firing at 21 h: the sky's mean from 24, 31,
51 to 35, 58, 107 on flash frames, runs skyfx_n2 and skyfx_n8).

**What:** 10FA030CD9308DE2 is passed over and tinted as 471C5A0705715DB7 is. The night pass's effects' light (D163)
also keeps the half size effects' layer as it was before the effects were drawn into it (at the first draw into it
after the sky dome's) and uses the layer less that (`Night::Subtract`): the effects alone, whatever the layer held.

**Tested:** firing on Wake Island's carrier deck at 21 h and 14 h, eight screenshots each over the bursts: the sky's
mean unchanged on flash frames (24 to 26, 31 to 34, 51 to 55 at night; runs skyfx_ok_21, skyfx_ok_14). Not tested:
explosions, other maps, other weapons; other shader variants may exist.

## D180 — Sky: the eye's place in a view without terrain; taller, fuller clouds; cloud height from 100 m (2026-11-16, Claude for Battlefield 1943 Reborn)

**Why:** the user, 16 Nov 2026: flying up toward the clouds with the nose up "they never get closer. But when I pitch
to fly back down, the clouds all of a sudden jump down to the height they are supposed to be"; "clouds should also be
more vertically dense. Right now they look thin and whispy"; "lower the cloud height limit to 100m". In a view without
terrain (ApplyLateSky) the eye was the one the terrain's camera last gave: a climbing plane's sky stayed where the
climb began.

**What:** the draw path keeps the camera's place from every lit draw of the world's (the pixel constant the lit
shaders take it from; not the first person weapon's, at 0): `DrawPath::WorldCamera`. A view without terrain takes it
when it is from the last two frames; else the eye goes on from where it last was at the speed it last had
(`TrackSkyEye`, twenty seconds at the most). The cloud layer is 2800 m deep in its own scale (was 1800) and of a
taller kind (0.62, was 0.5). The cloud height's least is 100 m (was 200) in Custom Match, the admin panel, the hosts
(gm_host.cpp, server/gmhost.py) and the manager.

**Tested:** looking straight up from Wake Island's deck: the eye from the lit draws is the terrain's (-548.0 230.4
15.0 both; run cloudup3). The clouds from the deck and from 300 m (runs cloudthick1, cloudthick2). NOT tested: a climb
in a plane (the test camera does not pitch): whether a cockpit's view gives lit draws of the world's, or runs on the
kept speed; the frame cost of the deeper layer.

## D181 — The turned shadows' depth about the map's middle, halved (2026-11-16, Claude for Battlefield 1943 Reborn)

**Why:** the user, 16 Nov 2026: "Dynamic shadows around objects and buildings also decouple from their positions, as
if they get moved around by the player camera", at any time of day. With the shadows turned to the clock's sun
(LatchShadowTurn) what cast a shadow, and whether the ground was lit at all, went by where the camera looked: on Wake
Island's deck at 9 h the plane beside the player had its shadow in one direction of the view and none in the others,
and the deck was sunlit or all in shade (run shad_pan9). Every caster was turned (NXR_SHADOW_LOG: none missed). The
game keeps the camera near the far end of each cascade's depth (0.93 of the first's 167 m; 0.84 of 200 m; 0.60 of 386
m), which its own sun needs no more of, and the turn about the cascade's middle tilts the depth across the map (the
turned depth's x and y parts: 0.02 and 0.04 of the range a clip unit in the first cascade, 0.12 and 0.24 in the
third): the ground and what stands on it left the map's depth on one side or another.

**What:** the turned depth is halved and put about 0.5 where the camera's depth is (z' = 0.5 + 0.5 (z turned - the
camera's depth)), in the casters' matrices and the receivers' alike, so half the cascade's depth lies either side of
the camera whatever the turn. The game's depth bias, in the map's units, is twice as long in the world.

**Tested:** Wake Island's deck, the view turned in steps: at 9 h the plane's shadow falls the same way in every
direction of the view (run shad_pan9b against shad_pan9); at 17 h shadows in the views that show them (shad_pan17).
Not tested: on the island among buildings while walking, other maps, the shadows' edges for the longer bias.

## D182 — Sky reflections dimmed at night: the cube map of a day's sky times the specular constant's w (2026-11-17, Claude for Battlefield 1943 Reborn)

**Why:** the user, 17 Nov 2026: "Building windows with glass are reflecting daylight when it's night". 71 of the
game's pixel shaders sample outdoorLightReflectionEnvmap, a cube map of a day's sky (the sea, shiny props, and what
glass uses it), and multiply it by outdoorLightKeySpecularColorAndHemisphereVisibility's w (their disassembly,
NXR_DUMP_PS: tfetchCube from tf1, then times c6.w). The day and night cycle scaled that constant's colour by the sun's
light but left its w, so the reflection stayed a day's sky at night.

**What:** tools/light_constants.py writes a last column into bf_lights.inc, 1 for those shaders; their w is multiplied
by the painted sky's factor (OutdoorLight::painted, luminance weights), the same picture of an afternoon dimmed as the
game's own painted sky is: 1 at noon, about 0.09 at night, lower and redder at dusk, greyer under an overcast. A log
line every 3000 frames names a draw's strength and its factor. NXR_DUMP_PS takes a list of hashes.

**Tested:** Wake Island's airfield at 21 h (run glass_n3): the sea's shader and a prop's, strength 1.000 times 0.093.
The airfield's building windows do not use the cube map (painted panes: unchanged with all 12 cube map shaders drawn
there skipped, run glass_skip). Not tested: other maps, the look at dusk and dawn.

**Then the breakable glass** (the user: "every map, any building with a breakable glass window"): the material
shaders_beach/glass/glass_t01 (the air tower, guard towers, factory, warehouse, lighthouse) is drawn by pixel shader
A98B19DAA168DDAE (and its sibling 5FD03DF368EEF02E, the same constants), which take no outdoor light: a cube map of a
day's sky (sampler_Texture19) and a dirt texture, the colour times colorScale.x and nothing else, alpha unscaled. Its
colorScale.x now gets the same factor (SkyReflectionLeft, shared with the cube map shaders above). Found with
NXR_PS_LIST=<s> (every pixel shader of one frame) and NXR_SKIP_PS; NXR_SKY_REFLECTION_KEEP=1 leaves both as the game
has them. Tested at 21 h on Wake Island's air tower (BF_DEV_CAMERA_AT on the cab, the view turned by rsr taps): the
panes 57.8 with the reflection kept, 51.5 with it dimmed (runs tower_nl_k, tower_nl_f, the same frame); what is left
is D170's dim lantern in the cab, meant to be there. 5FD03DF368EEF02E was not seen drawn, so its constant's use is
assumed from the shared table.

## D183 — Night's lamps: the lamp's own glass lit, no glow in the air; a bloom and a subtle lens flare, each switchable (2026-11-17, Claude for Battlefield 1943 Reborn)

**Why:** the user, 17 Nov 2026: "make the vehicle and lamp light sources illuminate without that floating light ball
... color the mesh the light is coming from when lit and give it a bloom with a clean and subtle camera lens flare
from looking directly at the light source, maybe with some soft halos or cataract effects on the camera lens.
Togglable from the Enhancements menu tab". D177's bulb was a sprite (a hot core and a halo) drawn where the lamp's
point projects, a ball wherever the point is not exactly on the glass.

**What** (night.h, renderer.cpp BuildNightCamera):
- A sprite carries the bulb's place in the world and its radius (the lamp's `glow`). The night pass already rebuilds
  each pixel's place; the surface within the bulb's radius of the lamp glows in the lamp's colour (its brightest
  channel held near 1.3, so a sodium lamp's glass stays amber). Where no mesh is near the point nothing glows.
- Bloom (`native_x_renderer_lamp_bloom`): a small glare and a short halo round the lamp, only as much as its middle is
  in view (LampSeen: the scene's distance at three points across it against the bulb's, the lamp's own housing within
  three bulb radii not hiding it).
- Lens flare (`native_x_renderer_lens_flare`): the four strongest lamps (the sprite's colour's luminance over
  1 + (distance / 35 m)^2) flare: four faint ghosts on the line through the picture's middle (amber, cyan, violet,
  white), a thin halo ring and a fainter outer one round the light, and a milky veil near it (the lens's haze), in the
  lamp's colour, stronger the nearer the light is to the picture's middle and only while its middle is in view.
- Settings > Enhancements gains a Night lights section with both switches (on by default, applied at once), and the
  Lighting (AO, GI) and Textures (HD textures) sections moved there from Graphics, whose subtitle now reads
  "PRESETS, UPSCALING, IMAGE".

**Tested** (dev build, Wake Island's airfield at 0 h): the lampposts' heads lit amber with no ball (runs flare_a2,
flare_a3; flare_a1 had the flare's rings too strong and the glass burnt white, both toned down); both switches off
(flare_off, the same view): the light round the lamps 44 against 24, the wider glow 29 against 16, a far corner the
same. The jeep's and the Sherman's headlights lit small in their own glass, the beams on the ground (veh_f1, with
NXR_VEHICLE_LAMPS_ON). Not tested: the menu page on screen, the flare looked at straight on at a street lamp's face
(the hooded lamps show little from below), other maps, frame cost.

## D184 — The lightning's strikes for the game's thunder; some strike closer (2026-11-17, Claude for Battlefield 1943 Reborn)

**Why:** the game's weather sounds (bf/src/weather_audio.cpp, the user's own recordings) play each strike's thunder
after the time its sound takes to come, so they need the sky's own strikes, not a second schedule; and the user's
close-strike crack needs strikes within 1.5 km, which the sky never made (1.5 to 10 km).

**What** (sky.cpp): one slot's flash is drawn by FlashInSlot, used by SkyFlashAt (the sky's light, unchanged) and by
the new public LightningStrikes(from, to, weather, out, max): each strike starting in (from, to] on the world's clock,
its place from the middle of the eye's 4 km square, its strokes and strength. The distance's one random draw is split:
18% of strikes 300 m to 1.5 km off, the rest 1.5 to 10 km as before, so every other number of a slot is unchanged.

**Tested:** a storm at weather 97% in local play (run wsnd_1): strikes 4.2 to 9.4 km off logged with their thunder's
delay, the thunder heard in the recorded mix at those moments. Not seen: a close strike's flash.

## D185 — Thunder from where the sky flashed (2026-11-17, Claude for Battlefield 1943 Reborn)

**Why:** the user, 17 Nov 2026: "I want the thunder and lightning sound to come from their sources." D184's thunder
was the same in both ears, its distance taken from the middle of the eye's 4 km square.

**What:** NoteListenerView / ListenerView (public, any thread): the camera the sky and the fog last ran with (its eye,
and its right and forward from the view's matrix rows), noted by both of the renderer's sky paths. The game's weather
sounds (bf/src/weather_audio.cpp) place each strike where sky.cpp lights it (the square's middle plus the strike's
offset), take its delay from there to the eye, and pan its thunder by its bearing from the camera's facing as it
plays (equal power, at most 85% to one side, following a turn within 50 ms); behind the camera it is a little quieter
and duller (a low pass to about 3.5 kHz straight behind). Part of the file's own stereo is kept: 20% for a close crack,
35% for a distant strike, 55% for the clouds' rumble. Without a camera noted in the last second: as before. The rain,
the wind and the distant rumble stay all round. Up to 16 thunders wait at once (6 dropped some: far thunder waits up
to half a minute).

**Tested** (dev build, local play on Wake Island, weather 72%, the mix recorded): a strike 99 degrees to the left
7.9 dB louder on the left, one 152 degrees (behind, left) 2.6 dB, one 9 degrees to the right 1.1 dB louder on the right;
in a 97% storm one 122 degrees to the right 5.2 dB louder on the right. Not tested: the pan following the camera's
turn while a thunder plays; a vehicle's view; headphones by ear.

## D186 — No rain on the underside of roofs (2026-11-17, Claude for Battlefield 1943 Reborn)

**Why:** the user, 17 Nov 2026: "The rain impacts can be seen on the inside of the roofs when inside buildings". The
fog pass's cover test looks 30 cm above what is seen; above a thin roof's or platform's underside that is already out
in the rain over it, so the drops' kick-up splashes (which only asked for flat, open ground) and the wet darkening
showed there.

**What** (sky.cpp kFogPS): what faces down is never open to the rain (no wet darkening); the rings and the kick-ups
only on what lies under the eye (a flat surface seen from beneath faces down).

**Tested** (dev build, Wake Island's air tower, the camera under its platform looking up, weather 95%): white
splashes on the platform's underside before (run roof_a0), none after (roof_a1). Not looked at again: the rings and
splashes on open ground (unchanged in the shader for what is below the eye); a building's interior ceiling.

## D187 — The weather's sounds in the game's own 5.1, ducked under its loud moments, muffled under cover (2026-11-17, Claude for Battlefield 1943 Reborn)

**Why:** the user, 17 Nov 2026, choosing the weather's sounds mixed into the game's output and reacting to the world
over going through the game's sound engine (a much larger piece of research), and asking for a ducking of our own
"similar to the game's at those events (explosions, etc)".

**What:**
- bf/src/audio_settings.cpp's XAudioSubmitRenderDriverFrame hook reads each frame (each channel's 256 samples one
  after the other: front left, front right, centre, LFE, back left, back right, as the SDK's audio reads it), takes
  the game's loudness in it (RMS of the five full channels), applies the master volume and the menus' mute (its ramp
  now along each channel's samples; it ran along the whole frame), and hands it to weather_audio.cpp, which adds the
  weather and writes it back. No audio device of the weather's own any more.
- The loops' stereo: left in the front left and at 0.7 in the back left, so the right. A strike's thunder (D185) is
  panned with equal power between the two corner speakers either side of its bearing (-30, 30, -110, 110 degrees),
  part of the file's stereo kept; behind the camera quieter and duller as before. On a stereo device the SDK's fold
  (front, back at 0.707) gives the same left and right.
- Ducking: the game's loudness over about 60 ms against its usual (followed from the first frame, rising over 3 s,
  falling over 1.5 s, never under -50 dB); 6 dB over it and more turns the weather down 0.8 dB a dB, to 12 dB at most,
  within 12 ms, held 0.25 s, let go at 10 dB a second. (Over each 5 ms frame alone, every peak of the game's sound
  counted and the weather pumped 4 dB all the time: run w51_a.)
- Under cover (NativeXRenderer's EyeCovered: the sky's look round the camera about twice a second, now public): the
  rain 45% quieter and low passed to about 1.2 kHz, the wind and the far rumble less so, thunder a little duller and
  quieter; 0.8 s to change.

**Tested** (dev build, local play, weather 95%, the weather's part recorded as a stereo device hears it): thunder
left/right as its bearing (3311 m at -56 degrees +3.4 dB left; 1804 m at 62 degrees 4.7 dB right; run w51_a); the
soldier's two second bursts of fire ducked the weather 4.5 dB, nothing ducked between (w51_b); under the air tower
(under cover logged) the rain 6.2 dB quieter with 4.9 dB less of its highs than in the open (w51_e against w51_b).
Both variants build. Not tested: an explosion near; a 5.1 device; by ear. The camera under the tower looking straight
up was not found under cover (w51_c), looking level it was (w51_d, w51_e).

## D188 — The renderer's sky on every map; the first person arms lit evenly at night (2026-11-17, Claude for Battlefield 1943 Reborn)

**Why:** the user, 17 Nov 2026: "run those tests, and confirm weather, clouds and lamp/vehicle lights properly work on
those maps too", then "What are those patchy shadows on the player arms?". At 21 h in hosted matches (run_map.ps1):
Guadalcanal showed the game's daylit blue sky and painted clouds, Iwo Jima its painted orange sunset, Coral Sea its
painted clouds, each over dark ground with our stars (runs map1_night, map2_night, map3_night). Only Wake Island's sky
shaders were known (D-sky: 8ABF56776060E64C, 0365AC9437C0240E, E059F66662ECF0A4, 471C5A0705715DB7, E20CB2628975A77D).

**What** (draw.cpp, with a sky of the renderer's own):
- Iwo Jima's and Coral Sea's tint 1106AB346E573399 and painted clouds 3206097F1E8CC042 (vertex shader
  63302D0C99305DF8) and Guadalcanal's tint EE301688055DFECB and painted clouds 1CE8C9384750D860 are skipped in the
  output-sized target, as Wake's; Guadalcanal's haze 38DEA3BE39271907 always, as Wake's (run map1_skip with
  NXR_SKIP_PS found them: the renderer's night sky and clouds with them skipped). Not in the painted sky's dimming
  for a game sky (their constants not looked at).
- night.h: the first person arms and weapon (nearer than 3 m and than the world's depth) are lit by the lamps and the
  effects' lights evenly (0.6), not by the facing taken from the depth's neighbours, which on their few large
  triangles is each triangle's own: near a lamp they showed in patches.
- NXR_SKY_DRAWS=<frame>:<count> dumps that many draws.

**Tested** (dev build, hosted LAN matches at 21 h, every vehicle's lamps lit): Iwo Jima (map2_night2) and Guadalcanal
(map1_night3, map1_arms) with the renderer's dark sky, stars and clouds; Guadalcanal's 390 lamps and Iwo Jima's 229
found and lit, vehicle lamps placed (Sherman, jeep models); the arms smooth beside a lamppost (map1_arms against
map1_skip). Not yet: Coral Sea after the change, the storm and the day on each map, a game sky (renderer's sky off).

## D189 — A strong storm: the rain driven by the wind, the view's sway and blurred edges (2026-11-17, Claude for Battlefield 1943 Reborn)

**Why:** the user, 17 Nov 2026: "Wind setting should also drastically affect rain slant at 100% storm strength. We
should create a minor screen wobble/lean and slight vignette blur on edges of player camera that sells the effect of a
strong storm."

**What** (sky.cpp, renderer.cpp, game_menu.cpp):
- The rain's lean (the constants' rain.x, the tangent): the wind times 0.7, and from 80% of the weather up to 2.1 at
  100% (65 degrees from upright at full wind). And a bug found on the way: the fog pass's local `vec3 rain` (the far
  rain shafts' colour, the fog's colour times 0.3) hid the constants' `rain` from there on, so the near streaks leant
  by about a tenth whatever the wind (runs gale_t2 to gale_t4: about 5 to 10 degrees at full wind; the far shafts,
  above it, had the right lean). Renamed rain_far: at full wind the streaks now run at a steep slant seen from the
  side (gale_t5; the shader's own maths: 21 to 65 degrees on screen by the view's heading).
- The sway (native_x_renderer_storm_sway, on; Settings > Graphics > Sky "Storm sway"): from 80% of the weather and a
  fresh wind up (a quarter under cover; three seconds to come and go), the lens pass (which now also runs without drops
  when the sway is on, and draws no drops with Rain on the lens off) turns the picture about its middle by gusts of up
  to about 0.55 degrees over several seconds and a small tremble (0.12 degrees), a touch larger so its corners stay
  filled; the drops on the lens and the HUD do not turn, the crosshair's aim stays. Its edges blurred toward the corners
  (eight taps up to 0.8% of the view's height) and up to 11% darker.

**Tested** (dev build, hosted LAN match on Wake Island's carrier, weather and wind 100%): the view leant and its edges
blurred (gale_a against gale_off with the sway off; the first blur, 1.2% and 16% darker, too strong, toned down); the
rain's slant (gale_t5). Not tested: how the sway feels in motion, a vehicle's view, cost.

## D190 — The sea's reflection of the renderer's own (2026-11-17, Claude for Battlefield 1943 Reborn)

**Why:** the user, 17 Nov 2026: "Can we turn those reflections off and use our own SSR on them and the water?" The
sea's shaders reflect a day sky's cube map (D182 only dims it): in a storm Guadalcanal's sea stayed sunny turquoise
(run map1_storm).

**What** (`native_x_renderer_reflections`, on, with the renderer's sky; Settings > Graphics > Sky "Sea reflections"):
- The draw path leaves the cube map out of the eight sea shaders (two a level, found by their constant tables:
  sampler_Diffuse_Water2 with the depth buffer and a distortion: Wake 60BD629ABC6BFCFC, BEAD02FECCE1E2EA;
  Guadalcanal 2715129AEB1C7AE7, ED75062BA2E66F5D; Iwo Jima C7F420106FAD9628, FEFCD04010C7CA86; Coral Sea
  1139C2856EADF277, F819F26C2C47D01B): their specular constant's w is 0 while this is on.
- The fog pass (which now runs whenever this is on) copies the scene first and, where the view's ray meets the sea's
  plane (Sky::ReadSea's height) before what the depth holds, reflects: the surface up, rippled by five small waves whose
  phases a drifting noise shifts and two octaves of a noise's slope (more with the wind, calmer far off); the reflected
  ray marched through the depth (40 steps from about 0.4 m, growing 13% a step) to the first place it passes behind what
  is drawn; else the sky drawn in that direction if it is in the view, else the sky's palette that way. Fresnel (2% to
  all) of it replaces as much of the game's water colour, before the fog over it.

**Tested** (dev build, hosted LAN matches): Guadalcanal at 11 h, weather 30% (runs ssr_g1, ssr_g2): the rocks and the
buoys mirrored, the open sea the grey sky's colour; the waves' even bands of ssr_g1 broken up in ssr_g2. Wake Island
at 21 h (ssr_w_night): the sea dark under the night sky. Not yet: the window glass (the next step), a storm's sea, cost,
inland water (only the sea's plane is found).

**D190, later the same day: the sea from its own draws.** Flat ground at the sea's height read as the sea from the
plane alone: Guadalcanal's base shone with puddles (run glass_fac). The sea's eight shaders' draws are now kept (as the
object motion's are, D48) and drawn again before the fog into an R32 float mask with the view's depth tested equal (the
game's sea runs on under the island; drawn untested its base was still the sea, run seamask_a); the fog pass reflects
only where the mask has the sea and the plane agrees. Not with MSAA (the plane alone then). The base's ground plain and
the shore's reflection kept (run seamask_c, seamask_d).

## D191 — The window glass's own reflection: started, experimental, off (2026-11-17, Claude for Battlefield 1943 Reborn)

**What:** `native_x_renderer_glass_reflections` (off). The glass_t01 shaders' draws kept and drawn again (no depth
test, the nearest pane kept) into an R32 float mask of their depth; with the lens pass (D189's, which now also runs for
this), where a pane is nearer than the world's depth from before the weapon (the night's copy, now also taken while the
reflections are on) and not behind what the view's end depth holds, its place and facing from the mask and the
reflected ray marched as the sea's; the game's cube map left out of the glass's colorScale. NXR_GLASS_DEBUG=1 paints
the panes (magenta seen, cyan hidden), =2 the mask's coverage (yellow). BF_DEV_FIND_OBJECT=<text> logs where a level
places objects whose blueprint has that text.

**Why off:** not yet seen working. glass_t01 is far rarer than D182's note had it: none on Wake Island's guard towers
or the air tower (one draw in a whole run there), every frame on Guadalcanal from its base, where its coverage showed
on the base's guard tower and a building but the panes never passed the visibility test on screen (runs glass_cov,
glass_cov2). With it on and not working the panes lose the cube map and show nothing; off, they keep D182's dimmed one.

**D189, later the same day: a 16:9 frame for the edges, tried and taken back.** Blurring a wider view's extra sides
whole, out to a 16:9 frame's bounds (to even out its wider field of view), was built and tested at 21:9 (runs
gale_169, gale_169b), then reverted at the user's word ("Revert the 16:9 boundary to the edges it was before"): the
edges are again the view's own, with the eight taps out to 0.8% of the height.

**D190, the evening of the same day: what the user saw on the sea, fixed.**
- "Flags also now have a water shader on them": what is see-through drawn over the sea after it (a flag, smoke, spray)
  writes no depth, so the sea's mask still held the sea there. DrawPath copies the picture just after the sea's last
  draw (before the next draw that is not the sea's, or at the fog if the sea was last); the fog pass reflects only where
  the picture is still that (NXR_SEA_DEBUG=1 paints what is left out in red; none on Wake's shores, run sea_dbg_a: no
  flag over the sea was in view, so the flags themselves are not yet seen fixed).
- "isn't being used at near-field": with the mask the plane test is dropped, and the sea's surface is where it is drawn
  (its waves): the crests near the eye, above the plane, were left out.
- "weird reflections": the ripples' slope is held to 0.08 near (0.18 far), and a hit is narrowed by five halvings
  between the last two steps; the beach and the hills broke up into bright smears before (runs sea_fix_a, sea_fix_b).
- "hiding the shoreline wash animations": the wash is the sea's own shader's foam. No reflection where the water is
  under 0.6 m deep (the world's depth before the sea, the night's copy, against the sea's surface: to 1.2 m a sandbar
  out at sea showed as a dark ring) nor where the water's colour is near white.
- "isn't taking into account our sky ... much more blue": two causes. Where the reflection found no sky in the view it
  took the palette, a clear sky's; now it looks up the picture from that way for the sky drawn there, and the palette
  is greyed by the clouds' cover. And Wake Island's and Coral Sea's sea shaders blend in a second day sky cube map of
  their own (sampler_Texture19 at fetch 6, the glass's picture, D182; their disassembly: tfetchCube tf6 times c249.x
  into a screen blend), strongest at a grazing look: it is bound as a 4x4 cube of the renderer's sky (zenith above,
  horizon around, as the view shows them, times 0.62, greyed by the cover, darker below) while the reflection is on.
  Guadalcanal's and Iwo Jima's sea shaders have no such cube. Runs shore_off, shore_on to shore_on5 (fixed views).
- Tried and not it: the mask's depth tested at-or-nearer with a bias (the horizon's blue was the cube, not the mask).
  Kept: GREATER_EQUAL with a bias of 8, as safe as EQUAL against the island's ground and not missing by a rounding.

**D189, the evening of the same day: a downpour.** The user: "make the rain itself more visible/dense at 100%
weather". From 75% of the weather up: more drops in each column, columns 1.8 times as close, drops a third closer one
after another and 45% longer, brighter (light 0.45 to 0.9, brightening behind 0.45 to 0.8), and a sixth cylinder 57 m
out. Run rain_gale_b against rain_gale_a (a gale on Wake Island).

**D191, the evening of the same day: the glass, looked at again; still off.** On Wake Island's air tower cab
(BF_DEV_CAMERA_AT 316.9,244,176.1, 14 m back) the one glass draw a frame is kept and drawn again into the mask, but
nothing of it lands: not with its own state (NXR_GLASS_DEBUG=3: the panes coloured by why they fail, none shown), nor
with neither culling nor depth clipping, the depth range 0 to 1 and no scissor (=4: coverage only, none). Its draw's
viewport has the depth range 0.005 to 0 (logged), its 288 bytes of vertex constants look like a transform, its 234
indices are the guest's. The sea's draws, kept and drawn the same way, work. Not found yet why; the reflection on
glass stays off (the panes keep D182's dimmed cube map).

**Not a fault: Iwo Jima's far island at night.** Its terrain is the terrain's own shaders (skipping the two unlit
candidates of that frame changed nothing, runs iwo_skip_*): the slope facing the moon, about twice the shaded
ground's brightness, with no fog in those runs to haze it. Left as it is.

**Inland water:** only the eight sea shaders sample water textures (every level's pixel shaders' constant tables);
each level has the one water mesh at the sea's height, so there is no other water to reflect.

## D192 — Lightning bolts to the ground (2026-11-18, Claude for Battlefield 1943 Reborn)

**What:** the user: "We should have bolts from the import of Beachheads weather I thought". The sky's import
(cc456b51) took Beachhead's sky and clouds only, and the lightning (7010ddcb) was a light inside the clouds; Beachhead's
bolts (lightning.gd: _build_bolt, _jag, _ribbon, its bolt shader) are now brought over. Each lightning slot
(FlashInSlot) draws, after its other numbers (so every flash, its place and its thunder stay as they were), whether it
strikes the ground: always within 1.5 km (where the thunder is a close strike's crack), out to 7 km for 35% of the
flashes at the storm's start to 75% at its worst; the rest stay in the cloud. Its channel, from the slot's own seed:
from 30 m into the cloud base straight down to the sea, jagged by five halvings with sideways offsets (Beachhead's
0.16 of the length, 0.55 less each time), three to five branches running down and out, some splitting once more; at
most 72 pieces, Beachhead's widths (0.45 m + 1.3 mm a metre off; branches 0.45, twigs 0.28 of it).

**How it is drawn:** Sky::FillBolt lays the pieces out in the view's pixels each frame (cut 1 m in front of the eye,
those off the view left out, a box around the rest) into the sky's constants; the fog's pass draws, in that box, a
white-hot core in a blue glow (Beachhead's shader: the core 1, the glow 2.6 times as wide and 0.22 to 0.3 as bright),
at least about a pixel and a half wide (dimmer for it), hidden where the view's depth is nearer, through the haze
between, added to the picture. Its light: each stroke a sharp one (risen in 10 ms, gone in about 0.2 s, 0.12 s apart:
it flickers, as Beachhead's return strokes) and faint between them until the flash is over. The rain is not lit by
it (the strobe of 14 Nov stays away). None seen from in or over the clouds. NXR_BOLT=<metres> holds one on, that far
straight ahead of the camera (a developer's check).

## D193 — The storm's sway a light shake, on foot and in the open only (2026-11-18, Claude for Battlefield 1943 Reborn)

**What:** the user: "The screen wobble is incorrect. It should be lightly shaking the player camera when outside and
not in a vehicle." D189's sway was a slow lean of the picture (gusts to about 0.6 degrees over seconds), a quarter of
it under cover and all of it in vehicles. Now it is a quick jitter: the picture moved by up to about 0.16% of the
view's height (2 pixels at 1440) and turned by up to about 0.09 degrees, at 6 to 18 Hz, in buffets that swell and
ease over a few seconds; the picture is drawn larger by the most the shake can take, so its corners stay filled
without it breathing. It and D189's blurred edges come only while the player is on foot (the game's pad read tells
the renderer: SetEyeOnFoot, from look.cpp's LocalOnFoot and not seated) and the eye is in the open (the rain's
cover); two seconds to come, half a second to go (getting into a vehicle or under a roof ends it at once). Still the
lens pass's picture, not the game's camera: the aim and the crosshair do not move. The log says when it starts and
stops.

## D194 — The storm's mist on the ground; the storm's edges seen (2026-11-18, Claude for Battlefield 1943 Reborn)

**The mist.** The user: "low but thick Ground fog should be all over the map being kicked up by that 100% storm
weather". From 75% of the weather (gale.x, full at 100%) the ground fog is thicker (+0.022 a metre at its floor: a sight
of about 60 m in it at a soldier's eye), lower (its top to 12 m, from the 28 to 60 m the storm's mist had raised it to)
and everywhere: its patches filled in (at least 55% of their density) and drifting four times as fast. Its floor is
the ground under what each pixel sees (2 m under that point, 0 for the sky and the sea) rather than the sea, so it lies
over the island's higher ground too, not only along the shore; below 75% of the weather it is the sea's as before.

**The edges.** The user: "I didn't see the edge vignette blur in that last test run". It was only about 11 pixels at
1440 at the most, and since D193 it went with the shake, off in a vehicle (the run was mostly in one). Now it has its
own strength (in the open, on foot or in a vehicle; none under a roof), starts nearer the middle, reaches 1.6% of the
view's height with two rings of eight places, and darkens to 15%.

## D196 — The ADS depth of field: gathered at half size without grain, graded along the gun; a strength; no dot on the bazookas (2026-11-18, Claude for Battlefield 1943 Reborn)

The user: "The ADS Depth of Field is also very low quality." Then, while it was being done: "less strong toward the
front of the weapon (front iron sight should be clear), and stronger toward the back of the weapon (still keeping the
sights clear when Iron Sights are enabled in settings) ... for both US and Japanese rifles and SMGs", "remove the
crosshair dot on the bazookas when ADS-ing" and "an ADS Depth of Field strength toggle in options with Low, Default,
Strong settings".

**What was wrong** (run ads_before1, the Thompson aimed on Wake Island's carrier, 3440×1440; ads_before_rif, the
Garand, built from the old pass for the comparison): the soft gun was covered in grain a pixel apart, and its soft
edge over the deck and the boats was stippled. Each pixel turned its spiral of 24 taps differently (interleaved
gradient noise) over a 16 pixel disc, a tap per 33 pixels or so, and nothing smoothed it after. The lens (sharp from
1.4 m) also blurred the Garand's rear aperture and the front sight.

**Four passes now** (dof.cpp):
- the colour and each pixel's blur radius at half size: of four pixels the largest radius, the colour leaning to the
  blurred ones;
- the gather at half size over a Vogel disc (the golden angle between taps): the same pattern in every pixel and every
  frame, so no grain and no shimmer. Twice the taps for each quality (low 24, medium 48, high 80; the setting as it
  was), each a quarter of the cost. Which taps count is the old rule; a sharp texel no longer adds its own colour to
  the gun's blur spreading over it;
- a fill: a small tent of four bilinear taps, premultiplied by how much shows;
- over the view at full size, blended (no copy of the colour), by each pixel's own radius from the full size depth or
  by how far the gun's blur spreads over it.
Aiming at 3440×1440 the frame rate went from about 165 to 185 frames a second with the old pass to about 195 to 200
(ads_before1 and ads_after1, the same scene).

**Graded along the gun.** Measured with NXR_DOF_SHOW (the distance in coloured bands; 2 on the left half of the view
only): with Iron Sight the Thompson's and Type 100's rear sights and receivers are 0.15 to 0.35 m from the eye, their
front sights 0.5 to 1 m; the Garand's and Type 5's rear aperture rings are nearer than 0.15 m and reach about a
quarter of the view's height from its middle. So the radius is none from 0.7 m on, the most at 0.15 m and nearer,
eased between; and none within 26% of the view's height of its middle, whole from 40% (the sights line up in the
middle, the rings round it). The stock, the receiver below the sights and the hands go soft; the sights, front and
rear, stay sharp.

**Strength:** Settings, Camera, "ADS depth of field strength": Low, Default, Strong, 0.6, 1 and 1.6 times the blur
(native_x_renderer_ads_dof_strength, defined in dof.cpp).

**No dot on the bazookas:** fov.cpp's AdsDepthOfField logs each weapon's name the first time it is held ("BF ADS:
holding ...": thompson, garand, m18_us, nambu100, type5, m18_jap, wrench_us, us-hg) and, for a launcher (m18,
bazooka, rocket, launcher, type 4), hides the crosshair while the zoom is over 0.3 (HideCrosshair, as the SMGs' Iron
Sight does). The sight's own frame stays.

**Results:** runs ads_after1 (Thompson, Zoom In), ads_us_inf2 (Thompson with Iron Sight, then the M18), ads_us_rif2
(Garand), ads_strong_rif (Garand, Strong), ads_jp_inf (Type 100 with Iron Sight, then the Japanese M18) and ads_jp_rif
(Type 5), all on Wake Island's carrier: the blur is smooth, without grain; the sights and what is aimed at are sharp;
no dot on either launcher.

## D197 — Soft sun shadows: the game's shadow compare given a penumbra that widens with the blocker's distance (2026-11-18, Claude for Battlefield 1943 Reborn)

The user: "Is there a way we can also drastically improve shadow and lighting quality?"

**What the game does (measured):** one sun shadow map, a stacked depth texture of three 1024 x 1024 layers, a cascade
a layer (Wake Island at 10:30: 23, 69 and 348 m across, 374, 462 and 949 m deep after D181), drawn at the render scale
(2752 x 2048 at 3440 x 1440) or at the "Shadow quality" scale (D92) when that is larger. 725 of the lit pixel shaders
read it as their fetch constant 0 (outdoorLightShadowmapTexture) and filter it themselves: four point fetches half a
texel round the receiver's place (`tfetch2D ... OffsetX=±0.5, OffsetY=±0.5`), `sgt` against the receiver's depth, a
`dp4` with `getWeights2D`'s bilinear weights. Every shadow's edge is one texel wide whatever casts it: razor sharp near
the camera (1 cm texels), stair-stepped where the texels show (a hangar's sunlit patch, a sandbag wall on a slope, a
power cable's double line), and the same at a post's foot as under a treetop. AO (GTAO, D132) and GI (SSGI, D133)
already exist and are on in the Medium to Ultra presets: they darken corners and contacts and add one bounce; the
shadows were the weakest part left. "Shadow quality High (2x)" does nothing at 3440 x 1440 natively: the render scale
(2.69) is already larger; Ultra gives 4096².

**What:** in the translator (sdk/src-bf, `SpirvShaderTranslator::SetSoftShadowFetchConstant`, Profile::
soft_shadow_fetch_constant = 0 for Battlefield 1943):
- a point fetch at a texel offset from that fetch constant into a temporary register keeps its coordinates without
  the offset (u, v, layer) in a function variable; once four such fetches have written the register's four components,
  an `sgt` or `sge` that compares it with another operand (the receiver's depth) is replaced, when
  `SystemConstants::soft_shadow[0].w` is above 0, by one shadowed share in every component (the game's weights add up
  to one, so its `dp4` passes it through). Any other write to the register ends it. Off (0), the game's own compare.
- The share: a blocker search (the 2 x 2 texels under the receiver, gathered, and eight gathers on a disc as wide as the
  penumbra of a blocker 40 m up) averages the depths nearer the light than the receiver; with no blocker, the 2 x 2
  texels' bilinear share as the game had it (a thin lone caster keeps its shadow); else 16 bilinear gathers
  (OpImageGather, each weighted by its own fractions) on a fixed golden-angle disc whose radius is the blockers' distance
  times the sun's penumbra per metre, between 1.5 and 24 texels. Each tap's depth is biased by its distance times the
  cascade's depth per uv times 2 (a receiver 63 degrees from facing the light), so slopes do not shadow themselves.
- Per cascade the shader takes depth units per uv and per metre from the receivers' transforms (DrawPath::
  UpdateSoftShadows, shadows.cpp, once a frame where the rain's cover takes them: a row's length is its units per
  metre). The map's size from the image (OpImageQuerySizeLod), so it holds at any shadow scale.
- `native_x_renderer_soft_shadows`: off, soft (the sun's disc, 0.53 degrees: 0.0093 per metre) or softer (twice),
  live; Settings > Enhancements > Lighting > Soft shadows. Default soft.
- The new system constant changes the translator's header: the bf builds compile against the installed copy
  (`sdk/src-bf/out/install/.../spirv_translator.h`), which build-local.cmd installs from the source; copied there by
  hand for rebuild-dev / rebuild-local until then. No prebuilt library holds the class.
- Developer aids: NXR_SOFT_SHADOW=<penumbra>,<farthest m>,<slope>,<smallest>,<largest texels>;
  NXR_SOFT_SHADOW_AB=<s> switches off and on by turns, logged, for pairs of screenshots from one run.

**Tested** (3440 x 1440, RX 7900 XTX, hosted LAN match, `.handoff/scripts/run_shadows_alt.ps1` and
`run_shadows_ab.ps1`; run_map's new kinds `sun`, 10:30 clear, and `dusk`, 17:30 clear):
- Wake Island's airfield, 10:30 and 17:30 (runs sa1_w_sun, sa1_w_dusk, sa2_w_sun): the water tower's lattice shadow
  soft away from its legs and sharp at their feet; the hangar's sunlit patch and a power cable's shadow without stairs
  or the doubled line; a hut's eave and an outside stair soft on the wall. No acne on roofs, walls or the slopes.
- Guadalcanal, 10:30 and 17:30 (sa1_g_sun, sa1_g_dusk, sa2_g_dusk): a sandbag wall's shadow on a slope, a fence's
  rails, branches on the ground. At 0.012 per metre (the first try) a branch 6 m up all but lost its shadow: the
  default is now the sun's own 0.0093.
- **Cost** (GPU per frame, the same five views, separate runs off and soft): Wake Island (ss1_w_sun_off, ss1_w_sun_soft, at the first
  try's 0.012: the taps are the same) 4.76 / 5.13 / 4.97 / 5.38 /
  4.83 ms off against 4.96 / 5.33 / 5.24 / 5.71 / 4.99 ms soft (+0.16 to +0.33 ms, 3 to 6 %); Guadalcanal (ss2_g_sun_off,
  ss2_g_sun_soft) 4.97 / 4.59 / 5.00 / 4.62 / 4.20 ms against 5.15 / 4.80 / 5.09 / 4.74 / 4.25 ms (+0.05 to +0.21 ms).
- Both builds (rebuild-dev, rebuild-local) pass.

**Not done:** screen-space contact shadows (the first cascade's texels are about 1 cm at this resolution, so small near
casters are in the map already; what it misses, grass and the first person weapon, would need the sun's share of each
pixel's light, which a pass after the scene does not know); AO only on the ambient light (same reason). Not tested:
night (moon shadows go through the same compare), storms, Iwo Jima and Coral Sea, in motion (the fixed disc has no
noise to flicker; the penumbra follows the cascades' fit, which moves with the camera).

**D190, 18 Nov 2026: the far sea and the foam.** The user: "Water near the horizon is really pale blue during the day"
and "the SSR hides the seafoam animation that's on the original water waves". Where a reflected ray leaves the picture
the sky is looked for up the picture: the far sea past the game's draw distance holds no depth either, and was taken
for the sky, so the horizon mirrored the game's own pale far water at a grazing look's full strength. Only what lies
above the horizon (its view ray) counts as sky now. And what of the game's water is brighter than the water round it
(four places 6 pixels off at 1440) is added back over the reflection, so its crests and foam show through. Runs hz_on
and hz_off (Wake Island, 11 h): the far sea takes the overcast's grey, the foam patches the same as without the
reflection.

## D198 — Bots: players of the server's own, driven through the game's own input path (2026-11-18, Claude for Battlefield 1943 Reborn)

**The ask** (TekRantGaming, 18 Nov 2026): bots for offline play and for servers that fill empty slots, playing like
players; a difficulty that also sets how aggressive they are; every objective (flags, vehicles, planes, boats, AA guns,
Air Superiority); flanking, cover under fire, squads; a Co-op mode (bots only on the chosen team, players only on the
other); Team and All chat lines of our own, on or off; settings in the game's menu and the server's.

**What the game has.** No AI: the engine's AI data types are still in the image (AISettingsData, PathfindingSettings,
BotPriorityConfigData, the spawn point's PlayerType SSHumanPlayer / SSAiPlayer), but nothing that thinks for a soldier,
paths or aims. What it does have is a server that takes a player with no connection: the spawn point's SSAiPlayer
branch (`sub_8236BC98`) makes one with the player manager's own factory.

**Decision: bots are server players of our own** (`bf/src/bots.cpp`), in the server process only, so the same code
serves offline play (a private match is a hidden dedicated server) and real servers, and every client sees ordinary
players: names on the scoreboard, kills in the feed, soldiers in the world. Each bot is made by the player manager's
factory (`sub_82387EF0`, `sub_8238EFD8`), picks its team through the game's own pick (`sub_82399AE8`), gets a kit
from the level's TeamData (its soldier data at player+500, as a human's deploy sets it) and spawns through the spawn
point's own routine (`sub_8236B688`). Once a server tick it gets input as a client's message would bring it: its own
EntryInput (player+280) filled with the stick's analog values, digital bits 40 and 43, the absolute aim and a tick,
then the soldier's input step (vtable +260), as the connection calls it. Not chosen: a client process per bot (a whole
game each), or moving soldiers by hand (the game's own physics, weapons and network already handle a driven soldier).

**Found on the way** (runs bots_s0a..x, each a hosted LAN match on Wake Island with the bot lines in its server log):
- The game's AI player flag (player+532) must stay off: with it a soldier turns to the input's aim but never moves (its
  own update feeds it for the removed AI's movement). It also spared a player the idle kick, which a hook now does for
  bots.
- Without a kit the spawn routine reads the null page; the round's start (`sub_8238EB68`) re-teams and respawns every
  listed player, bots included, at one spawn point.
- The aim convention, from a human's input: yaw and pitch in radians, forward = (-sin yaw, 0, cos yaw).

**Step 0 tested** (`BF_BOTS=4`, run bots_step0, `.handoff/scripts/run_bots.ps1`): four bots join, two a team, with
their team's kits, spawn at their team's spawn points (the US carrier, the Japanese base), walk at about 4.5 m/s and
turn as sent, before and after the round's start; seen by the player's game as soldiers of either side; no null page
read, no skipped call, no crash. Both builds pass.

**Names** (the user, 18 Nov 2026: "random faction-appropriate first and last names"): our own lists of common 1940s
names, American given and last names for the Marines, Japanese family and given names (family first, as written then)
for Japan; at most 15 characters, as a player's name (`server/plasma.py`), so no "[BOT]" in the name (a bot marker
can come as a clan tag later); none twice on the server. A bot keeps its side through the round's start (a hook on the
team pick), so its name and kit stay its side's. Run bots_names (six bots: Eugene Ross, Fujita Makoto, Ed Hughes,
Endo Makoto, Harold Gray...). Not yet seen on a client's scoreboard.

**Step 1 so far** (runs bots_s1a..l, Guadalcanal, 8 to 12 bots, hosted LAN match): bots spawn at their side's spawn
points (the round's start skips them: its loop passes a player whose +516 is set), go for the nearest flag their side
does not hold and capture it, see, aim at and shoot enemies, kill and die and spawn again. What they use:
- The game's own ray test (the character physics' world, soldier+304 -> +104, its caster at vtable +144), 0.008 to
  0.012 ms a ray. It hits soldiers too (its filter keeps out no character).
- A navigation grid of our own, 2 m cells learned by rays as the bots need them (no navigation data ships with the
  game), A* over it with a budget of rays a tick; steps the bots got stuck on are closed for a minute. Recast/Detour
  (zlib) was looked at: it needs the level's collision triangles (the game's Havok data, a study of its own); fed our
  own ray samples it would know no more than the grid. Kept for later.
- The flags (CapturePointEntityData and the server's entity pointing at it: +0x80 the team capturing, +0xA0 the
  progress, held at 100). The player's HUD showed the bots' captures ("Your team captured Village").
- Sight: up to three enemies a think, rays to head, chest and hips; an enemy lost is fought for 2.5 s more.
- Stats (the user: bot kills must not count toward a player's service record; a Bots counter instead): a kill of a
  bot counts as "botkills" and adds no score; a death to a bot is not counted; bots get no stats or awards of their
  own (the game's award and death scoring look up a score record a bot has none of: skipped, run bots_s1h's crash).
  The service record's Bots counter on the backend and the menu comes with the settings step.
Known: some US bots stay at Guadalcanal's base (the hangars); the capture area's player list is not read yet.

**Learning, kept per map** (the user, 18 Nov 2026: bots should get better after every match played against them, and
"Pretty much all ground is walkable, just feed them the data on objects you can't walk through ... and the water
swimmable"). In the server's user data, bots/<map>.txt and bots/<map>.grid, loaded with the level, saved every 20 s
and at a level change:
- The level's grid, scanned once: from every spawn point and flag outward over every step the collision allows (the
  game's ray test is that collision), 250 rays a tick, within 120 m of them. Guadalcanal: 157,525 cells and 1.2 million
  steps in 86 s while the match ran, the server's tick at 0.36 ms of work on average (run bots_learn3); the next match
  loaded it whole and cast almost no rays (bots_learn4). Water is swimmable at three times a step's cost once its
  height is known (the character physics' +304 holds it only near the water).
- The ground soldiers walked and how much each step was used (a human's walk 1, a bot's 0.5), kills made from and
  deaths at each cell by side, where humans waited near enemies, and the steps humans walked in the minute before they
  took a flag (its ways in). Used live: popular steps and a flag's ways in cost less, cells where the bot's side died
  cost more; at a flag a bot holds spots its side fought and waited from and watches where the enemy's kills came
  from. Runs bots_learn1..4: each loaded the one before's (1,773 then 3,119 then 3,390 cells walked).

**The players' style, and the match's own memory** (the user: "learn from a player, and adapt that playstyle into
the next match"; "Could bots learn on-the-fly live in a match?"). Measured from every human's input and events while
it plays, kept in bots/style.txt (all maps; an older match's sums count 0.7 at each load) and blended with the match
being played: time sprinting and still (over half second windows: a player's position comes with its game's
messages, and per tick a walk read as a sprint), firing and with the sights, burst lengths, kill distances, kills
from the victim's side or back, time attacking or defending near flags, kits at each spawn. The bots take it, each
with its own 0.75 to 1.25 of every trait: their fighting range, bursts, sights, side steps, sprinting, defending
share and kits. The match's own memory: deaths and the enemy's firing spots of the last minutes (fading over 120 s)
make a bot's routes keep away and its watch turn there. The game's input concepts, from a player pressing each button
(run bots_calib): A 12 and 29, B 13 and 16, X 20, Y 9 and 19, LB 18, RB 17, LT 11 (sights), RT the analog 7, the
left stick analog 0 and 1 (left -1), the right stick 5 (up +) and 4 (right +); the D-pad none on foot. Runs
bots_learn5..6 (23 bots: 0.5 ms of the tick's work, no error) and bots_style2 (a still, never sprinting test player
read so).

**The players' vehicles** (the user: "I'll be using vehicles on them, so that should give them a good stepping
stone"): by vehicle and seat (the player's controlled entity, +312, when not its soldier; the vehicle's name from its
data, +0x14 -> +0xC; the seat, +292, 0 the driver's): the time spent in it, the cells it went through with their
height (a sample every half second; a plane's its flying height) and where it killed from, and how far. A kill from
a vehicle is kept out of the soldiers' kill spots and the infantry style. Run bots_veh2: a test player in a
Corsair, 42 s and 75 cells learned. For the bots' own vehicles to come.

**Not done yet** (the plan's next steps): navigation, the soldiers' fighting, flags, vehicles, planes, boats, AA guns,
squads, flanking and cover, difficulty, chat, the settings and the server's back-fill and Co-op, and keeping bots out
of stats. Known: the round's start puts bots on the team its own balance picks, whatever their kit; the TeamData scan
takes about half a second on the server thread once a level.

## D199 — The day and night cycle's light in every lit shader of the game's, not only those test runs had recorded (2026-11-18, Claude for Battlefield 1943 Reborn)

The user: "Can we make sure that player characters correctly use our night time lighting and don't still just take the
original day sky lighting?"

**What was wrong:** the cycle's light reaches the game's shaders through bf_lights.inc (D-cycle, D182): each shader's
registers for the sun's, the sky's and the ground's light and the shadows' transforms, by the runtime's hash.
tools/light_constants.py made it from bf/data/shaders/bf1943-shaders.list, which holds only the shaders test runs had
recorded. Read from the game's own archives, 832 pixel shaders take the outdoor light; 105 were not in the table and
kept the day's full light at any hour. Our runs' pipelines pair 31 of them with skinned vertex shaders (what soldiers
and arms are drawn with); two were drawn on the carrier at Wake Island's spawn (run chr_n1, NXR_PS_LIST), one draw each.
13 skinned vertex shaders our runs had seen (and one other) were missing too: their shadows' transforms were not
turned with the moved sun or moon (D-cycle's turn), so they looked the shadow map up as for the game's own sun.

**What:**
- tools/xxh3.py: XXH3 64 in plain Python (the runtime's shader hash; checked against 1,125 recorded shaders, all equal).
- tools/light_constants.py also reads every pixel shader container in the game's archives (the header gives the
  microcode's place, after the constant table's part and the literal constants, and its size) and names it by its
  microcode's XXH3; and with `--storage` NativeXRenderer's shader storages from test runs: a vertex shader there that
  the list lacks gets the constant table of the files' containers of its size that differ from it in an eighth of the
  dwords at most (the vertex fetch fields, filled in at bind time), when they all agree.
- bf_lights.inc regenerated: 119 rows added (105 pixel, 14 vertex), none changed. No code change: the rows go through
  the same ApplyOutdoorLight as the others (and D182's sky reflection for the 23 of them that sample the cube map).

**Tested** (dev build, hosted LAN match with 8 bots, Wake Island at 21 h): the first person hands and gun were already
the night's (in the table before: a hand about 34 of 255 against about 200 by day) and are unchanged (runs chr_n1,
chr_n3, the same frame); the two newly covered shaders are drawn on that view (chr_n6) and skipping them changes
nothing on screen there (chr_n5), so what they draw was not seen. Both builds pass.

**Not tested:** a soldier of another player's seen at night: the host's own client draws no bot's body, by day or night
(runs chr_d4, chr_n4: their markers on the deck, no bodies), so it needs a second client or a real match. Vertex
shaders that no test run recorded stay out of the table (their hash cannot be made from the files); a run on each map
with `--storage` adds those it meets.

## D200 — Sharper copies of the game's 8 bit textures and patched atlases (2026-11-19, Claude for Battlefield 1943 Reborn)

**Decision:** two upscaler hooks beside the patcher (D142), for a game whose text and icons were made for 720p:
- `SetTextureUpscaler`: after each load of the base level of a k_8 2D texture with a single mip, the hook gets its
  size and guest data and may return a picture 2 to 8 times as large (one byte a texel). It is made into an R8 texture
  once for that data (kept for the session in the HD pack's map, under a key of its own) and `GetView` hands out its
  view in place of the game texture's, whether `native_x_renderer_hd_textures` is on or not (`hd_own`).
- `SetTextureUpscalerRgba`: for the 8_8_8_8 textures the patcher takes, where the HD pack has no picture, `PatchHd`
  asks the hook for one (R, G, B, A rows, as a pack's) and the patcher draws its own pictures into it at that scale,
  as into a pack's. A texture the patcher took and left as it is gets that larger picture too. The pack's picture,
  where there is one, still comes first.
- `SetTextureUpscalerImage`: any other k_8_8_8_8 or DXT4_5 2D texture with a single mip may get an RGBA8 copy 2 to 8
  times as large the same way (an 8_8_8_8 one's bytes swapped as an HD pack's are). Battlefield 1943 Reborn's hook
  takes four pictures it knows by their data: the crosshairs' sheet, the minimap's compass (DXT5, unpacked first) and
  two icons (run texdump1 found them: every texture the game loaded in a match, dumped).
- `SetUpscaledTextures`: whether the upscalers' copies are drawn. They are made either way (the user's option is
  then only a choice of view, so it applies at once in both directions); a pack's picture keeps following
  `native_x_renderer_hd_textures`.
- Battlefield 1943 Reborn's hooks (bf/src/sharp_text.cpp; Enhancements > Textures > HUD scale, `bf_hud_scale`:
  Original (Xbox 360), the default, or High-Res, which `SetUpscaledTextures` follows at once): a font atlas
  (its data the same as one of the game's Fonts/.../*_texture.res, read from the player's build_overlay-00.fbrb at the
  start) is enlarged by Catmull-Rom and its edges brought back to about one screen pixel; the five UI atlases
  hud_glyphs.cpp patches the same way, colour premultiplied, the coverage tightened only at sharp edges and between
  the local lowest and highest coverage. The scale is the screen's height over 720, rounded up, 2 to 4.

**Why:** the user, 19 Nov 2026: "Can we increase the resolution quality of the font?", then "Can the icons be done the
same way too?". Then the crosshair, the minimap's compass and the ammo counter (the counter was in the
patched atlases already). Then: "simply by toggling a 'HUD Scale' option to 'High-Res' from 'Original (Xbox 360)'
... we're not actually including the higher-res assets in our client, the clients build them from their own files". The game's glyphs and icons at 1440p were 720p pictures drawn twice as large.

**Measured:** runs sharp2 to sharp4 (Guadalcanal, 3440 x 1440, scale 2): six of the game's fonts (headline_large,
headline_medium, headline_small, list_item, normal_medium, normal_small) and the five atlases drawn from the copies;
the squad list's letters and kit badges, the minimap's icons and the flags' outlines sharp where they were soft, the
icons' dark outlines kept (run sharp3, tightened about one half, had thinned them). Run sharp5: the crosshair's rings and the compass's
letters and ticks sharp too. Nothing of the game's is shipped or
stored: the copies are made in memory from what the game loaded.

## D201 — The game's sky passed over on frames with an effect on every map: its variants known by the sky's own shape (2026-11-19, Claude for Battlefield 1943 Reborn)

**Why:** the user, 19 Nov 2026: "The bug with gunshots and explosions that I thought we fixed is happening on
Guadalcanal. We need to ensure this is fixed across the board." D179 found Wake Island's tint drawn by another pixel
shader on frames with an effect (10FA030CD9308DE2 for 471C5A0705715DB7) and listed it; the other maps' variants were
never looked for. On Guadalcanal at 21 h, firing the Thompson (run gfire1, NXR_SKY_PHASE_LOG): its tint
1CE8C9384750D860 (vertex shader 65BD09519504B2B5, 240 vertices) is drawn by 062964B870D68445 on those frames, same
vertex shader and vertices, so the game's daylit blue sky and painted clouds showed round the renderer's night sky on
every flash (gfire1 shot 73: the sky's mean blue 34 to 49).

**What** (draw.cpp, with a sky of the renderer's own):
- 062964B870D68445 is passed over in the output sized target, as Guadalcanal's tint is.
- Each known tint and painted cloud draw of the sky that is passed over leaves its shape: its vertex shader and its
  vertex count. Any other pixel shader drawing one of those shapes into the output sized target within the 24 draws
  after the dome's is the same part of the sky drawn by a variant, and is passed over too (logged once a shader:
  "draws the sky's shape ... passed over as the sky's"). Iwo Jima's and Coral Sea's effect variants, never seen, are
  passed over the same way.
- NXR_SKY_PHASE_LOG=1 (a diagnosis aid): each pixel shader drawn in the 8 draws before and the 24 after the dome, once,
  with its vertex shader, vertices and target.
- NXR_GUN_TRACE=<seconds>:<lines> (a diagnosis aid): the screen place of the first bones of the skinned models at the
  eye, each frame (for the first person arms' steps at the game's tick, not yet resolved). BF_DEV_BONELOG_AFTER (the
  game's side) starts BF_DEV_BONELOG later.

**Tested:** Guadalcanal at 21 h, walking, turning and firing (run gfire2): the renderer's sky, stars and clouds on
every screenshot with a muzzle flash (the sky's mean 27 to 39 blue, no daylit frame). Not tested: Iwo Jima and Coral
Sea while firing, explosions (the same variants are expected), a game sky (the renderer's sky off: these draws are
then drawn, as before).

## D202 — Detail distance in Enhancements: what the game's level of detail scale reaches, and what it costs (2026-11-19, Claude for Battlefield 1943 Reborn)

**Why:** the user, 19 Nov 2026: "Can we also increase the render distance of LOD too, so that essentially the full
model is rendered farther out?" It already could: since 13 Nov the game side (frame_rate.cpp, LodWorker) writes
1 / bf_lod_distance into GlobalLodScale (+0x10) of the game's mesh settings object (class 0x82040568, found in its
heap by its layout once a level is loaded, rewritten every half second), 2x by default, a slider from 1x to 8x in
Graphics > Image. Every mesh's distance to the camera is multiplied by it before its level of detail is chosen, so
each model keeps its full mesh, and small things stay drawn at all, that many times as far. The row was not where
the user looked for the game's look features, and what each step gives and costs had never been measured.

**What:**
- The Detail distance slider moved to Settings > Enhancements, under Distance; 1x reads "1x (original)", default 2x
  as before. Its help says what 2x and 8x show and cost.
- The mesh settings object's other members are logged once found: as the game runs MaxBillboardCount 8192,
  BillboardScale 1, ShadowMaxLod 0, LodBlendOverlap 1.

**Measured** (the same parked views in one run per factor, 10:30 clear, 3440x1440, RX 7900 XTX, frame unlocked;
runs lod_w1/2/4/8 on Wake Island, lod_g1/2/4/8 on Guadalcanal; per view the renderer's draw calls, GPU time and the
frame's time, which is the CPU's here):

| View | 1x | 2x | 4x | 8x |
|---|---|---|---|---|
| Wake, the island from the US carrier (860 m) | 884 draws, GPU 4.41, frame 4.94 ms | 888, 4.43, 4.96 | 888, 4.42, 4.95 | 888, 4.43, 4.95 |
| Wake, across the island to the airfield | 2213, 4.88, 5.40 | 2234, 4.89, 5.40 | 2626, 4.97, 5.47 | 4507, 5.28, 7.74 |
| Wake, high over the airfield | 1451, 4.46, 4.99 | 1451, 4.46, 4.99 | 1501, 4.51, 5.10 | 1827, 4.59, 5.13 |
| Guadalcanal, a hill over the island | 2795, 4.73, 5.22 | 2954, 4.83, 5.63 | 3688, 5.02, 6.90 | 4767, 5.07, 7.96 |
| Guadalcanal, across the bay | 2652, 4.92, 5.41 | 2739, 4.99, 5.52 | 3147, 5.14, 6.25 | 4565, 5.22, 7.71 |

- 1x to 2x: from the carrier Wake Island's trees and buildings appear (at 1x the island is bare); nothing measurable
  in cost (at most 0.4 ms on one view).
- 2x to 8x: twice the draws on views across an island, 2.3 to 2.7 ms more a frame, almost all of it the CPU's (the
  GPU 0.2 to 0.4 ms). What it adds: small trees and bushes in the middle distance and, on Guadalcanal, the far
  forests drawn as full trees; at 4x and below they are still flat pictures (billboards). The view from the carrier
  is the same at 2x and 8x.
- 4x: up to 1.7 ms more, little to see over 2x in these views. 2x stays the default.

**Not the lever:** BillboardScale (+0x1C) is the size of those flat pictures, not where trees switch to them: at 4 the
far forest stood four times as tall, at 0.25 a quarter (runs lod_bb4, lod_bb025). Render.EdgeModelLodScale is the
PlayStation 3's and does nothing here (13 Nov). The terrain's own level of detail (GeometryLodScale) is still not
covered: its live settings object has not been found.

**Tested:** both maps, four views each, at 1x, 2x, 4x and 8x; the row shown in Enhancements (run lod_menu1). Not
tested: Iwo Jima and Coral Sea, a plane in flight (the distances change fastest there), weaker CPUs (8x is CPU bound
on this one).

## D203 — The first person gun's lag and springs at 60 ticks a second as at the Xbox's 30 (2026-11-19, Claude for Battlefield 1943 Reborn)

**Why:** the user, 19 Nov 2026: "Player hand model and weapons also banded around like the tickrate bug was back
that we fixed on Wake Island ... Both [turning and walking]. It's like a choppy bouncing around ... ALL 30Hz events
can be increased properly to 60Hz." The soldier's own movement (bf_move_tick_scale) and the swimmers' exit
(bf_swim_exit_tick_scale) were already fixed this way on 18 Nov; the gun had its own counts.

**Found:** the gun's lag and bounce are the weapon camera's (levels-common objects/weapons/handheld/common/
*weaponcamera.dbx: FirstPersonCameraData, with a WeaponSpringEffect and a CameraSpringEffect, each a
WeaponLagSpringEffectData). A hardware watch on the held weapon's spring data while walking and turning (run
gs_watch1; tools BF_DUMP_VIRT and BF_WATCH_VIRT, below) found its one reader, about 60 times a second:
- sub_821E68E8(effect, input, zoomed), once a tick: six lag values (the move and turn sticks times the camera data's
  Move/Rotate modifiers, zoomed or not) go toward their targets by the camera data's ReleaseModifier (+60; 0.2 to
  0.3) every tick, s += (t - s) R, with no time step;
- then sub_821E6678 steps both spring effects' six springs, each an axis {data {Damping +0, Constant +4}, position,
  velocity}: v += -K x dt; v -= D v; x += v dt. The spring itself follows the tick's time; its damping is a share
  taken every tick.
At 60 both shares were taken twice as often, so the gun's lag let go of the stick and its springs lost their swing
in about half the ticks' time: a different, stepped motion from the Xbox's.

**What:** tick_rate.cpp, a hook on sub_821E68E8: for the call, R and every spring axis's D are
1 - (1 - share)^(30 / rate) (the same share in a second as 30 ticks of it), then put back (the data is shared by
every weapon with that camera; one call at a time). Nothing changes at 30. Setting bf_gun_tick_scale (on).

**Left as they are:** the jump, land and stance kicks (sub_821EC380, sub_821EC400, sub_821EC480 add a set velocity to
the springs) come once per event, from the soldier's event handlers, at most one per 0.2 s of game time: no count.

**Measured** (runs gun_30, gun_60off, gun_60 on Wake Island's carrier: the same stick turns and walks; BF_DEV_GUN_LOG
logs each call's lag and springs; time from each channel's peak to 10% of it, and its overshoot past zero):

| Channel | 30 Hz | 60 Hz before | 60 Hz now |
|---|---|---|---|
| The gun's bounce (offset y): peak, to 10%, overshoot | -0.047, 0.30 s, 17% | -0.031, 0.59 s, 26% | -0.049, 0.28 s, 16% |
| The gun's turn lag (roll): to 10% | 0.43 s | 0.72 s | 0.41 s |
| The gun's turn lag (yaw): to 10% | 0.50 s | 0.75 s | 0.50 s |
| The gun's walk lag (offset x): to 10% | 0.43 s | 0.72 s | 0.43 s |
| The camera's spring (pitch, the spawn's landing): peak, to 10%, overshoot | 0.035, 0.30 s, 20% | 0.024, 0.48 s, 0% | 0.036, 0.27 s, 21% |

Each tick's step is half as large at 60 (the largest step 30% of a channel's peak at 30, 16% at 60).

**Also the camera's recoil** (the same audit, the same switch): CameraRecoilData (each weapon's firing data:
SpringConstant 150 to 200, SpringDamping 0.2 to 0.4, SpringMinThresholdAngle), stepped once a tick by sub_822D5BB0
from the soldier weapon's update: two springs, each {data, position, target, velocity}: v += -K (x - t) dt; v -= D v;
x += v dt, the damping a share every tick again (found by the same shape, x - D x, in a search of the whole game:
22 such sites in 12 functions, the rest a HUD projection and shares already times dt). The hook on sub_822D5BB0 scales
D the same way. Runs rc_30, rc_60off, rc_60 (three bursts; the spring that moves, after each burst):

| The view's kick | 30 Hz | 60 Hz before | 60 Hz now |
|---|---|---|---|
| Peak, to 10%, overshoot past the target | -0.013, 0.16 s, 10 to 11% | +0.011, 0.03 s, 86 to 96% | -0.012, 0.10 to 0.12 s, 12% |

At 60 before, the kick swung back almost as far as it went and was gone in a few ticks. Now peak and overshoot are
the Xbox's; it settles a little sooner because the spring's own steps are finer at 60 (K = 200 is a stiff spring for
a 30th of a second step, so 30's own result is coarse): an exact match would need the 30 Hz steps themselves.

**Looked at and left** (the same search for per tick shares: x - c x, x += (t - x) c, x *= c with c from data): the
weapon's spread (GunSwayData: DecreasePerSecond, RecoilAmplitudeDecreaseFactor; its GunSwayLag values are all 0),
the soldier's landing penalty (sub_8251DC60: once, when a timer counted in seconds runs out), interpolations between
two data values (sub_82297940, sub_82552B48 and others), and shares already times dt (sub_82475858, sub_82282C38).

**Open:** StaticCameraData's AverageFilterFrames (10 in the planes' and gunners' camera data) is a running average
over that many calls (sub_822AD948 with sub_82226240: the yaw and pitch sticks of a seat camera that does not
AccumulateInput). If it runs once a tick, its smoothing lasts half as long at 60; but it never ran in a Corsair's
cockpit with the game's own free look held (G, runs sc_30, sc_60off, sc_60), so which seats use it, and how often, is
not known yet, and nothing was changed. (Those runs also showed --bf_tick_rate=30 on the command line does not make a
hosted match 30 Hz: the host profile's setting does, runs\swtmpl30.)

**Tools** (dev builds, dev_mem_dump.cpp): BF_DUMP_VIRT=<s>:<from>:<to> writes the guest's virtual pages in use (the
data objects and heaps, which the physical dump does not have) as records of an address and 4096 bytes;
BF_WATCH_VIRT=<address>[+<address>...]:<start s>:<seconds> a hardware watch on reads and writes of up to four guest
addresses (with BF_DEV_PDB=1 for the function names).

**Tested:** on foot on Wake Island, turning, walking and firing, at 30, 60 without and 60 with the fix. Not tested:
aiming down sights (the zoomed modifiers), other weapons' cameras (the shotgun's and the assault rifles' have the same
code, other numbers), a controller against the mouse.

## D204 — The sea's reflection at the horizon: the water no longer mirrors itself, and the far sea reflects as a flat sea does (2026-11-19, Claude for Battlefield 1943 Reborn)

**Why:** the user, 19 Nov 2026: "with SSR on the water, the horizon looks awful and blindingly bright blue at the
horizon line." On Wake Island at 10:30, clear (run sea_hz_true), the far water in a band about 40 pixels tall under
the horizon was a clipped pale cyan (217 254 254) under a sky of 172 174 197; with the reflection off (sea_hz_false)
the game's own far water there is 142 210 255.

**Found** (debug views 7 to 12 of the fog pass, below; runs sea_dbg7, sea_dbg8, sea_dbg9, one parked view, switched
every 8 s): the sky the sea mirrors was right (166 167 182), but
- the march hit the water itself: at a grazing look the reflected ray climbs about a centimetre a metre and passed
  behind the game's own waves further out, so the far water mirrored its own colour (what was added there 208 254 254);
- the strength from the rippled facing alone stayed about half at the horizon, and the foam check (water near white)
  took the far water's pale cyan for foam and cut it again; the rest was the game's own water, which with the
  renderer's small sky cube in it (D190) is itself a clipped pale cyan there (201 254 254);
- the crests added back (D190 addendum) counted the darker sky 6 pixels above the horizon as "the water round it".

**What** (sky.cpp, the fog pass):
- What the march meets within a metre of the sea, or on the sea's own draws (its mask), is passed and the march goes
  on: the sea no longer mirrors itself; a hull's lowest metre is not mirrored either.
- From 120 m to 600 m off, the Fresnel strength goes from the rippled facing to the flat sea's (the ripples still
  turn the reflected ray): near 1 at a grazing look, so the far sea shows the sky it mirrors, not the game's water.
- The foam check fades out over the same distances (far shores are left to the shallows' check).
- A place above the horizon is not "water round it" for the crests added back: the pixel's own colour stands in.
- native_x_renderer_sky_debug 7 to 12 (diagnosis): 7 the sky mirrored, 8 the strength (red the palette's sky, green
  the picture's, blue a march hit), 9 the water's own colour, 10 the reflection as added, 11 the crests added back,
  12 the strength by Fresnel alone, as drawn, and where the march passed the sea itself. These views keep the game's
  sky skipped (renderer.cpp), so the frame is the one played.

**Tested:** the same views before and after (runs lod_w2, lod_g2 against seafix2_w2, seafix2_g2: Wake Island and
Guadalcanal, four views each, 10:30 clear): the horizon's water now 168 172 199 under a sky of 169 168 185 (one row
at the line a little bluer, 183 207 255); the near water, its crests, the shallows and the shore foam as before.
Not tested: dusk and night, a storm, Iwo Jima and Coral Sea, from a plane high up.

## D205 — The turned shadows from a plane: the turn's pivot on the ground, not in the air at the camera (2026-11-19, Claude for Battlefield 1943 Reborn)

**Why:** the user, 19 Nov 2026: "It's really noticeable when flying over buildings, their shadows gets skewed or
stuck to the players camera when flying around the map and project over the terrain as you fly." With the day and
night cycle on, the game's shadows are turned to the renderer's sun (D181): each cascade's clip matrix is rotated
about a pivot at the cascade's middle, at the camera's depth in it. On foot that point is a metre or two over the
ground. From a plane it is in the air, 40 m and more over what is seen: the turned cascades then cover other ground
than the game chose its casters for (it culls them for its own sun), and what reached the map fell across the ground
in the wrong place, moving with the camera.

**Seen** (Wake Island's airfield, a developer camera gliding 35 to 60 m over it, BF_DEV_CAMERA_GLIDE=1, new):
at 10:50 the houses cast almost no shadows with the turn (run sh_fly_true) and did with the game's own direction
(sh_fly_false); at 17:50 large blurred dark patches lay across the airfield and the hillside, belonging to no
building, and moved with the camera (sh_dusk_cam).

**What** (draw.cpp, LatchShadowTurn):
- The pivot's depth is where the cascade's middle line (clip x = y = 0) meets the ground a metre up: the ground under
  the camera from the grass's height map (GrassCameraGround, asked through the developer tour's probe whenever it is
  free, ids from 2^23 up so a float holds them; the tour's asks come first), never under the sea (the height map
  holds the sea floor: 187.7 m under the US carrier's deck at 230), so a known sea level is needed (Sky::SeaLevel,
  passed on each frame).
- Only from 20 m over the ground or sea, blended in up to 40 m: on foot, on a deck or on a roof the pivot is the
  camera's as before (on the carrier the sea is 17 m down, run sh_dusk_ground: the pivot 16 m under the deck).
- Without a height map's answer or a sea level, the camera's depth as before.
- NXR_SHADOW_PIVOT_CAMERA=1 (diagnosis): the camera's depth always. NXR_SHADOW_LOG also logs the pivot.

**Tested:** the same glide at 17:50 (sh_dusk_cam against sh_dusk_ground3): the stray patches on the airfield and the
hillside gone or much fainter; the pivot logged on the airfield's ground (230 to 235 m) and the sea (214.7 m) in
flight, the camera's depth on the carrier's deck. Not tested: a real plane at speed, other maps, the houses' own
shadows at noon from the air (short and under their eaves either way), what the game culls for its own sun (still
its choice: a building it leaves out casts no turned shadow).

## D206 — The squad leader's order ring is a world marker too (2026-11-19, Claude for Battlefield 1943 Reborn)

**Why:** the user, 19 Nov 2026 (a screenshot: the red ring away from the E flag's icon): "the squad-leader command
to move to the E flag, but instead staying locked over the flag icon, it drifts as the camera moves. This should be
fixed the same way we fixed the in-world flag icons." A HUD draw is placed where the widened 3D view shows its point
(HudMode::kPoint) only when it is 6 to 44 canvas pixels each way and within 48 of a point the game projected. The
order is the flag's icon with a red ring round it; the ring is 44 to 45 canvas pixels (run ring1, NXR_HUD_MARKER_LOG):
at 45 it was placed by the HUD's own rules, and kept on the icon only when it came right after it ("as the last").
Otherwise, in the screen's left or right part, it went with that edge, away from the flag.

**What** (draw.cpp, ClassifyHudDraw): a square draw (sides within 1.2 of each other) may be up to 56 canvas pixels
and still be a world marker; other shapes keep 44. NXR_HUD_MARKER_LOG=1 (diagnosis): each draw that keeps moving at
one size and is not taken for a world marker, once a size and sheet, with its size, sheet, vertex shader and the
nearest projected point. NXR_HUD_MARKER_44=1: the old limit.

**Tested:** Guadalcanal with six bots, the player their squad leader, orders given with Back toward flags, then a slow
pan (runs ring2, ring_new; ring_old with the old limit): the ring stays round its flag's icon; the ring is no longer in
the log's list of moving draws not taken. A Corsair's gunsight on the carrier whole (run ring_plane). Not reproduced
in a run: the ring apart from its icon as in the user's screenshot (in the runs' draw order the ring came right after
the icon, so the old rule kept it there too); a scope's mil dots near a flag, and the other planes' gunsights.

## D207 — The weather's sound under the game's: the rain and the wind 18 dB down, the thunder over the gunfire, the wind with the weather (2026-11-19, Claude for Battlefield 1943 Reborn)

**Why:** the user, 19 Nov 2026: "Rain and wind completely drown out gunfire and explosions almost entirely. Lightning and
thunder can barely be heard too, and lightning should be relatively loud, especially if close. Weather percentage
should also affect wind %, unless it's manually adjusted, so 100% weather should result in 100% wind." Measured (run
wsnd1, Guadalcanal at 15 h, weather 100%, every volume full, the soldier firing): the game's own sound about -30 dB RMS
while firing and -46 to -58 between, the storm's rain and wind -20 dB (D187's levels), so 10 dB over the gunfire and
more over everything else; D187's turn down (12 dB at most, against a usual loudness that rose over 3 s) could not
make up that. A strike 781 m off reached -13.5 dB with the rain under it; a distant one did not rise over the rain.

**What** (bf/src/weather_audio.cpp, bf/src/game_menu_launcher.inc, bf/src/world_sync.cpp):
- The rain, the wind and the far rumble are mixed 18 dB down; the far rumble's own gain 6 dB over the rain's (0.78
  squared, 0.55 before). The strikes' thunder 6 dB down, with its gains raised: a close crack 2.2 at 150 m to 1.6 at
  1.5 km, a distant one 1.9 to 1.0 at 5 km, the clouds' 0.4 to 1.2 (1.0 the most before).
- The game's loud moments: 3 dB over the usual turns the rain and the wind down a dB a dB (6 dB and 0.8 before), and in
  a loud moment they also go down until the game stands 12 dB over them, 18 dB at the most (12 before); the usual
  rises over 10 s (3 before), so a firefight stays a loud moment. The thunder is turned down 3 dB at most.
- The wind follows the weather (Custom Match's world page and the admin's F9 panel) until its own row is set apart
  from it; set back to the weather's value it follows again. Custom Match keeps that as private_match_wind_manual.
- The tests' log line gives the rain and the wind's loudness beside the game's.

**Measured** (run wsnd2, the same match): the rain and the wind -34 dB, -47 to -50 while firing (the gunfire -28 to -32,
about 17 dB over them); the close strike -20 dB RMS (10 dB over the gunfire); a distant strike 3.5 km off 1 dB over the
rain, so the distant and cloud gains were raised after it (not measured again). The wind at 100% with the weather at
100% and the wind's setting untouched (the host's log line). Not tested: the F9 panel's following, a sheltered
camera, other volume settings.

## D208 — The weather as a tropical hurricane over the level: the storm local, lightning only under it and near the players (2026-11-19, Claude for Battlefield 1943 Reborn)

**Why:** the user, 19 Nov 2026: "the weather in general is should be based on a tropical hurricane, so the higher
percentage, the more the storm is building up, and at 100% weather, the hurricane completely engulfs the map. The
clouds and sky should replicate the tropical hurricane. in a localized manner. Weather should also feel localized
too, so areas of the map that aren't directly covered by the storm, just experience the wind. Thunder and lighting
only happen directly under the storm itself too." Then: "Lightning bolts should also have a higher chance to strike
near a group of players, resulting in the close lightning audio being loud enough to startle the player."

**What** (storm.h, the model; renderer.cpp, sky.cpp, sky.h, the public header, bf/src/weather_audio.cpp):
- The match's weather (WorldState::storm) is the hurricane's stage. From 25% it is out at sea, 14 km from the level's
  middle with a rain shield 3.5 km across; it comes in as the weather rises, to 1.8 km off the middle with a 9 km shield
  at 100% (the eyewall and the inner bands over the whole level, the calm eye just off it). Its strength at a place:
  0.15 in the eye, full at the eyewall (1.8 eyes out), falling to nothing at the shield's edge, with two logarithmic
  rain bands spiralling in (strength under half between them); its bearing turns a few degrees over a match. The
  level's middle is its capture points' (SetMapArea, from the game side twice a second).
- WorldState::weather (World(), what CurrentWorldState gives) is the weather where the camera is: the storm's strength
  there or the weather outside it (the match's, 0.3 at most). The rain, the mist, the light, the wetness, the steep
  shadows and the rain's sound follow it; the wind stays the match's.
- The clouds: the cover and the density outside the storm from the weather outside it; the cloud shader adds the
  storm's own at each place (cover toward a whole deck, its three layers merged, density up to 1.9 times), so the
  storm's deck and its gaps are seen from outside and overhead.
- Lightning: each strike, after its slot's usual numbers, is kept only where the storm is strong (with the chance
  smoothstep(0.45, 0.85, its strength there)); three in ten fall 60 to 400 m from a group of players (two or more
  within 120 m, the camera's player counted; each group's middle rounded to 50 m and sorted, so each player's game,
  which works the strikes out from the world's clock, takes the same) and are bolts. A far strike lights the scene
  less (a quarter at 9 km). The thunder: a strike within 400 m a crack of gain 3.5 at 150 m to 2.8 (2.2 at the most
  before); the far rumble also heard outside the storm (from 85% of the match's weather).
- A log line every 10 s: the hurricane's centre, its distance from the camera, the weather there.

**Tested** (runs storm60, storm85, storm100: Guadalcanal at 15 h, on foot, turning): at 60% the centre 8.9 km off, the
weather here 30% (broken cloud, no rain); at 85% 3.1 to 3.5 km, 33 to 53% here (a dark deck overhead with gaps of
blue between the bands, no rain); at 100% 1.8 to 2.2 km, 96 to 99% here (the full storm, rain, the wet gun). No shader
error. Not yet seen: a strike near players and its crack's loudness, the storm's wall from outside at a distance,
the other maps, and two players' games agreeing on the strikes.

## D209 — Custom Match: game mode, a map rotation of its own, tickets, match time and a dynamic storm (2026-11-19, Claude for Battlefield 1943 Reborn)

**Why:** the user, 19 Nov 2026: "add an option for servers to enable dynamic storm, so the weather gradually changes
over the time of the match"; "add match time and team ticket amount settings, including disabling match time, which
then the match only ends if one team loses all their tickets"; "add a map rotation selection as a submenu in create
match, so if enabled, hosters can chose from the selection of maps to add to the rotation individually, or click
'rotate all maps'"; "an option to select the game mode".

**What the game has** (static study of the level data and the image):
- Game modes: each level holds one mode's objects and no other's (capture points, two TicketCounterEntityData, a
  HotZone; no world part by mode, no mode switch in the image: GameModeTypes are Frostbite's Multiplayer_Day1/Day2 and
  SinglePlayer). Wake Island, Guadalcanal and Iwo Jima are Conquest, Coral Sea Air Superiority. So the mode is offered
  as what it is: it chooses the maps (Conquest, Air Superiority, All modes); no mode is offered on a map without it.
- Tickets: TicketCounterEntityData's InitialTicketCount (+136; 130 on the Conquest maps, 100 on Coral Sea), TeamId
  (+156). The server's ServerTicketCounterEntity (vtable 0x820150A0, made by sub_8232C780, destroyed by sub_82321E88)
  keeps the tickets as a float at +132 (+0x84, BF_DEV_TICKETS's), sent to the players in 16 bits (sub_8231FA90); its
  reset at a round's start (vtable +0x78, sub_8232C890) reads InitialTicketCount again. Every loss goes through
  sub_82335388(counter, tickets) (clamped at 0, marked for the network), and sub_82332748 sends the game's "tickets
  gone" event at 0, which ends the round. A player's HUD bar is the team's tickets over the player's own copy of
  InitialTicketCount (sub_824083E8 through the getter sub_8222B120, a getter shared with other data types).
- Time: no level has a time limit and nothing in the image ends a round on time (the only time limits are awards' and
  the trial's 1800 s validator). A round ended only on tickets, and still does with Match time off.

**What** (bf/src/match_rules.cpp new; gm_host.cpp, world_sync.cpp/.h, private_match.cpp/.h, server_rotation.cpp,
game_menu_launcher.inc, docs/SERVER.md):
- The server (`--bf_match_tickets`, `--bf_match_time_limit` in minutes): the tickets are written into its copy of each
  counter's InitialTicketCount as the counter is made (so its reset, its bleed and its "tickets low" percentages use
  them); once the round's time is up, sub_82335388(counter, 0) for the team with fewer tickets: the game's own end of
  round, winner and rotation. Level: the next ticket lost decides. The clock starts with the round's counters (made or
  reset) and stops at the round's end (RoundOver).
- The match's host (gm_host.cpp, `--bf_world_dynamic_storm`): the dynamic storm takes the weather from its base (the
  Weather setting, or where it was when switched on) up to a full storm and back, 0.5 - 0.5 cos over the round's time
  limit (else over 30 minutes, repeating), with a slow swell (6%, 290 s) on the way; each new round starts it again.
  The wind goes with it while it equals the weather (D207's rule). BF43WRLD and BF43WRLR carry, after the cloud
  height, the storm's byte, and the answer then the tickets, the time limit and the round's seconds left; a game or
  host from before reads none (server/gmhost.py ignores the extra byte).
- A player's game: the HUD's ticket bars over the match's tickets (sub_8222B120 answers them for sub_824083E8's call
  only, by its return address 0x8240848C); a match with a time limit shows the time left over the ticket bars (red in
  the last minute, OVERTIME when level at 0). The F9 panel has a Dynamic storm switch (last row), and its Weather row
  moves with the storm; moving the weather by hand switches the storm off.
- Create Match: Game mode, Map (the mode's maps, while the rotation is off), Map rotation (opens its own rows:
  Rotation on or off, Rotate all maps, a switch a map, the mode's other maps marked "Air Superiority only" or
  "Conquest only"), Tickets (the map's own, 25 to 1000), Match time (off, 5 to 120 minutes), and Dynamic storm in
  Weather and time. The rail scrolls when its rows are more than it holds (the selection kept in view, the wheel, an
  arrow at the end with more). Saved as private_match_mode, _rotation, _rotation_maps, _tickets, _time_limit,
  _dynamic_storm; the old private_match_map = 4 (all four in turn) becomes a rotation of all four in All modes, and a
  map outside the saved mode brings its mode (the test scripts' private_match_map keeps working).

**Tested** (dev build, LAN Custom Match hosted at the start, runs mo1 to mo4):
- mo1 (Guadalcanal and Iwo Jima in turn, 300 tickets, 1 minute, storm on, 4 bots): both teams 300 instead of 130; the
  storm in the host's log 0, 10, 36, 68, 94, 100% at 30 s, then 93, 66, 34, 8, 0% at 60 s, the wind with it; the time
  left over the ticket bars (shot_50: 0:14, red). Nobody had died at the minute: "both teams at 300 tickets: the next
  ticket lost decides", and the round went on.
- mo2 (Guadalcanal and Coral Sea in All modes, 300 tickets, 2 minutes, the player's suicide): at 2 minutes "the US has
  299 tickets to 300 and loses the round", the game's end of round screen ("Your team lost the match"), then Coral Sea
  by the rotation after 45 s with 300 tickets a team instead of 100.
- mo3 (the page, driven by the test's keys): the rail with Game mode, Map rotation (2 maps), Tickets, Match time,
  scrolled; the rotation's rows (Coral Sea "Air superiority only" under Conquest); Dynamic storm in Weather and time.
- mo4 (Iwo Jima alone, 25 tickets, no time limit): 25 a team; the HUD's bars over 25 (the level's 130) in the player's
  log; no clock.
- Both builds (rebuild-dev, rebuild-local) pass.

**Not tested:** the F9 panel's storm switch and the host taking it; a new round restarting the storm (mo1's round did
not end); OVERTIME's end by the next ticket; the HUD bar's fraction seen with tickets lost (only the log line); mouse
use and the wheel on the page; Air Superiority mode alone. The server package's gmhost.py and manager pass none of the
new options yet (a dedicated server takes the switches on its command line, but its players' bars then use the
level's count and show no clock). The clock also shows on the deploy and loading screens of a running round.

## D210 — 30 FPS at 60 ticks a second: the game's timer runs free, so a 30 FPS player walks as at 240 (2026-11-19, Claude for Battlefield 1943 Reborn)

**Why:** the user, 19 Nov 2026: "a smooth and playable experience no matter the framerate a player gets", and a
30 FPS check. In the main session's runs fps30 and fps240 (a hosted LAN match on Guadalcanal at 60 ticks, on foot) a
soldier at 240 FPS walked 4.5 m a second; at 30 FPS 0.2 to 0.8 m, the camera going forward a few frames, then back
8 mm, then forward again.

**Found:** choosing 30 FPS in the menu (SetFrameRate, frame_rate.cpp) also turns the game's original timing back on:
Game.VsyncEnable on, the timer's mode 2, frames locked to the screen and one game tick a frame. At 30 ticks a second
that is the Xbox's game. At 60 (tick_rate.cpp) the tick is a sixtieth of a second, so a 30 FPS player's game ran
30 ticks of a sixtieth a second, half time (run fr_30 before the fix: 29.8 client ticks a second), while its server
ran 60 a second in real time and pulled the soldier back to where it really was every few frames. Every other rate
(60 to 240, unlimited) is our own limit with the game's timer free (mode 0): it counts its ticks from the elapsed
time and runs as many as are due each frame, two at 30 FPS.

**What:** 30 FPS locks to the original timing only at 30 ticks a second; at any other tick rate it is our limit at 30
with the timer free, like the other rates. The choice is applied again in the frame limiter (LimitFrame) if the tick
rate changes after it (a server joined at another rate).

**Measured** (runs fr_30, fr_45, fr_60, fr_240: the hosted match on Wake Island's carrier at 60 ticks, the same 6 s
walk; the camera's place every frame; 45 FPS is the 60 setting held to 22.2 ms frames with BF_DEV_SLOW_FRAMES, as a
slow computer would be):

| Frame rate | Frames a second | Walk, net (path) | Speed in its middle | Frames stepping back |
|---|---|---|---|---|
| 30 | 30.2 | 23.86 m (24.01) | 4.40 m/s | 0 |
| 45 | 43.5 | 23.86 m (24.03) | 4.40 m/s | 0 |
| 60 | 60.2 | 23.86 m (24.04) | 4.40 m/s | 0 |
| 240 | 177.5 | 23.68 m (23.92) | 4.40 m/s | 3, at most 2.7 mm |

Before, at 30: 0.2 to 0.8 m a second. With the tick rate at 30, 30 FPS is the original timing as before.

**Not tested:** joining another's 60 Hz server at 30 FPS (the re-application when the rate changes after the choice),
a vehicle at 30 FPS, a dedicated server (it keeps the game's own timing: unchanged).

## D211 — The weather eased to its setting; clouds at three heights; storm towers, a volutus and lightning inside the clouds (2026-11-19, Claude for Battlefield 1943 Reborn)

**Why:** the user, 19 Nov 2026: "The cloud weather change should happen smoothly, not immediate and jumpy like it
currently does. Clouds should be able to form on multiple altitude layers, including cirrus high above. Cloud
generation should match photoreal topical clouds too, including volutus clouds." With a reference picture of a
tropical storm: bolts branching through dark clouds.

**What:**
- renderer.cpp, World(): the weather, the cloud cover, the fogs, the wind and the cloud base move toward what is set
  with a time constant of 8 s, at most 4% a second (the wind 5%, the base 25 m a second): a whole change of the
  weather takes about 25 s. Taken at once on the first look, when the world's source changes and after 5 s unseen.
- sky.cpp, the sky: an altocumulus layer 5 km up (small rounded cells in patches, lit toward the sun), its cover 0.22
  to 0.52 with the weather outside the storm; the cirrus (10 km) thickens as the hurricane comes (0.12 to 0.4).
- The low layer in the storm's bands takes the cumulonimbus kind (the whole layer's depth).
- A volutus: a roll cloud low in the layer along the hurricane's gust front, just outside its rain shield (a tube
  450 of the layer's units round, broken along its length, eroded at its edges), from 33% to about 95% of the match's
  weather.
- Lightning inside the clouds: an in-cloud strike within 9 km now shows its channel crawling along under the cloud
  base, 1.5 to 4 km, with four to six branches (70% of a ground bolt's light).
- storm.h: the weather outside the hurricane at most 0.15 (0.3 in D208 laid a flat grey sheet over the whole sky).

**Tested** (Guadalcanal at 15 h): runs sky45, sky70, sky100 (no shader errors; the grey sheet at 45% and 70%), sky70b
after the change outside the storm: tropical cumulus with blue between, the cirrus's streaks. Not seen in these runs:
the altocumulus clearly, the volutus (the camera's spot did not face it), a crawler bolt, the easing of a change made
during a match (the weather was set at the start).

## D212 — The hurricane's wall seen from outside it; brighter lightning in the clouds; no rain's sight or full sound over the clouds (2026-11-19, Claude for Battlefield 1943 Reborn)

**Why:** tuning D211 (the user: "Tune the clouds first"), and the user, 19 Nov 2026: "rain also shouldn't show when the
camera is above the clouds, and the rain audio should only be full loud when on the ground, and quiet above the
clouds. distant thunder and lighting can still be heard though." Aimed views (runs tune60b, tune100, wall60): the
low layer's clouds fade into the horizon's haze a few degrees up, so the hurricane a few kilometres off was not seen
at all; the held crawler (NXR_CRAWLER) thin and faint across a storm.

**What** (sky.cpp, bf/src/weather_audio.cpp):
- The sky draws the storm's wall toward the hurricane, over the angle its dense part (three quarters of the rain
  shield) takes, while the eye is outside it: its towers at the eyewall, 3 km tall as the storm forms and 10 km at
  full strength, an anvil's broad flat top, billowed in the weather map at two scales alike across and up, dark
  blue-grey in the sky's own colours with the top lit, its foot in the haze.
- A crawler (D211) 1.6 times wider and 0.9 of a bolt's light (0.7).
- The rain's sight over the clouds: the fade by the eye's height over the sea (over the world's 0 before: on
  Guadalcanal, its sea at 173 m, the fade sat 173 m low).
- The rain's sound: from 0.85 of the cloud base's height over the level's water to 200 m over it, the three rain loops
  fall to 15%; the thunder, the strikes and the far rumble as before.
- NXR_CRAWLER=<m> (a developer's check): an in-cloud strike's channel held that far ahead.

**Tested:** Guadalcanal at 15 h and 60% (runs wall60, a pale see-through pillar 70 degrees tall, then wall60b and
wall60c after the changes: a dark storm bank towering behind the cumulus toward the hurricane); at 100% the held
crawler across the sky (tune100). Not tested: the rain's sight and sound from a plane over the clouds.

## D213 — The hurricane the level's size; the clouds change faster under it (2026-11-19, Claude for Battlefield 1943 Reborn)

**Why:** the user, 19 Nov 2026: "3km and 10km might be too large for the scale of this game. We can generalize the
hurricane without making it too massive in scale. It should never be bigger than the map itself." Then: "The cloud
change rate should also update with the % of the storm to match realistic movement."

**What:**
- The game side gives the level's reach with its middle (SetMapArea(x, z, reach): its capture points' farthest from
  their middle, and 300 m; Guadalcanal about 710 m). storm.h sizes the hurricane by it: 4 reaches out at sea with a
  shield of 0.45 reaches from a quarter of the weather, coming in to 0.3 of a reach off the middle with a shield of 1.5
  reaches at 100% (about the level's own size; D208's 1.8 km off and 9 km). Full strength out to 0.6 of the shield,
  fading to its edge; between the bands at least 0.75 (runs scale100 and scaleb100: the shield's old falloff and 0.4,
  then 0.55, left the US base at 15% and 55% under a 100% storm).
- The storm's wall (D212): its towers 0.6 km tall as it forms, 2 km at full strength (3 and 10 km before).
- Lightning's strikes, the storm being small now, are placed within its rain shield (placed in the eye's 4 km square
  and kept only under the storm, most were lost); kept by its strength there, smoothstep(0.3, 0.7).
- The clouds' forming clock runs at the cloud change rate times 1 to 3 with the match's weather (from 25% to 100%):
  the host's (gm_host.cpp) and the players' games between its answers alike (storm.h RateFactor).

**Tested:** runs scale60, scale100, scaleb75, scaleb100 (Guadalcanal at 15 h): at 100% the shield 1065 m, its centre
614 m from the US base, the strikes 5 to 7 in a minute and a half within it. Not yet tested: the bands' 0.75 floor
on screen, the clouds' faster forming.

## D214 — The clouds' shadows on the ground (2026-11-19, Claude for Battlefield 1943 Reborn)

**Why:** the user, 19 Nov 2026: "Can we get clouds to cast shadows onto the map from the direction of the sun?" The
game's own shadows know nothing of the renderer's clouds.

**What** (sky.cpp): the clouds' density code (cloud_density and what it uses) is shared (kCloudShared) between the
clouds' pass and the fog's pass, which gets the clouds' shape and detail volumes (bindings 8 and 9). For each point
the fog's pass sees (not the sky), by day (the sun over 3 degrees, fully from 7), the clouds' own density, the cheap
kind, toward the sun at a quarter and three quarters of the layer's depth; exp(-2.2 x their sum) of the direct sun
gets through, and what shows of the scene is taken down by that share of three quarters (the sky's light stays):
the same clouds that are drawn, the hurricane's included, and their shadows moving with them.
native_x_renderer_cloud_shadows (on) switches it.

**Measured:** Guadalcanal at 11 h, cloud cover 55%, the same view on and off (runs cs_on, cs_off): the ground's mean
142 against 156 under the clouds over it, the far hill in the sun the same. Not tested: the shadows' edges moving over
the ground from a plane, other maps, the cost (two density samples a pixel).

## D215 — The clouds' shadows darker (2026-11-19, Claude for Battlefield 1943 Reborn)

**Why:** the user, 19 Nov 2026, after D214: "Let's make cloud shadow darker than that please."

**What** (sky.cpp, the fog's pass): the light through the clouds exp(-4.5 x their density) (2.2 before), and at the most
88% of what shows taken off (75%).

**Measured** (Guadalcanal at 11 h, cloud cover 55%, the same view): the ground's mean 123 against 156 without the
shadows (run cs_dark; D214's 142, run cs_on).

D215, the same day: "Darker still" (7.5 and 0.93: the ground's mean 103.5, 34% darker, run cs_dark2), then "That's a little
too dark": 6.0 and 0.91, between the two (not measured again; about 28% by the two around it).
Then "A bit darker": 6.75 and 0.92 (about 31%, between 28% and the 34% found too dark; not measured again).

## D218 — Shadows from high up: the turn fades out from 60 to 150 m, and receivers the light table misses are turned too (2026-11-19, Claude for Battlefield 1943 Reborn)

**Why:** the user, 19 Nov 2026 (relayed, two screenshots from a Corsair's chase view a few hundred metres over Wake
Island, the day cycle moving through the morning, shadow scale 4): "the ground shadows dislocate from their objects
and follow the plane camera. We should fix this on all maps for all cameras." The screenshots showed dark shapes
like palm groves and buildings lying out on the sea west and north of the island. That session's log: the game's
shadows turned by 119.5 degrees round to the sky's sun (D181), with D205's pivot.

**Looked for, not found** (each with NXR_SHADOW_LOG):
- casters left unturned: none, on all four maps from 250 to 500 m up (runs hs_*) and in a fast glide (hs_glide);
- receivers the light table does not list (their vertex shaders' runtime names come only from recorded runs, and few
  runs were in the air): none found, though a search for them by their transforms' values is now in (below);
- the turn read from a vehicle's own space (the last lit draw's transforms taken as the world's): every lit draw of
  a frame had the same transforms, in the Corsair too (vote_on, vote_off; tried and taken out again);
- the user's settings (shadow scale 4, AO and GI as theirs, 8:48 with a 108 degree turn, weather 27%) on a camera
  gliding 180 m up at about 150 m/s: no shapes on the sea (hs_user2). The large dark areas in hs_user are the new
  cloud shadows (D214), which came after the user's capture.

**What** (draw.cpp, LatchShadowTurn and ApplyOutdoorLight):
- From 60 m over the ground or the sea (the grass's height map, D205's probe, and the sea level) to 150 m, the turn
  fades out (smoothly, by the angle); higher up the shadows fall as the game casts them. The game chooses its casters
  and fits its cascades for its own sun, so its own direction always matches them. The light on lit surfaces stays the
  sky's sun's (the shadows and the light disagree a little from high up). On foot, on decks and roofs and in low
  flight nothing changes; with no sea level known, no fade.
- A vertex shader the light table does not list whose constants hold this frame's cascade transforms (exact values,
  all three cascades' rows at +0, +4, +8, as a listed receiver got them this frame) gets the turn there from then on,
  logged once ("a receiver the light table does not list"). NXR_SHADOW_UNLISTED_OFF=1 (diagnosis): not.

**Tested:** all four maps from 250 to 500 m at 8:48, shadow scale 4 (runs hs2_w, hs2_g, hs2_i, hs2_c): no turn latched
up there, no shapes on the sea; on the carrier's deck at spawn the turn is kept whole (1.00). Not tested: the user's
own case (not reproduced), a real plane in flight between 60 and 150 m, other cameras (bomb view, free look).

## D195 — Night: lamps' orbs only on their glass, lamps lit across the level, the effects not daylit, the rain's tunnel softened, lens drops on a vehicle's glass, buoys and lighthouses (2026-11-18 and 19, Claude for Battlefield 1943 Reborn)

**Why:** the user, 18 Nov 2026, with screenshots: the floating orbs on lamps, headlights, towers and bunkers; the effects
(explosions, bullets, water splashes, smoke) lit by daylight at night and in storms and hidden outside the rain's
cone; plane droplets on the cockpit glass; then "the light project by vehicle headlights and lamps/light sources" seen
"from across the entire map", "keep the buoys that mark the edges of the maps lit" and "The lighthouses should also
have a realistic rotating long light". A red cockpit glow was added and then taken out again at the user's word (19 Nov:
"let's remove and delete that tank and plane interior red light we made").

**What:**
- night.h: a lamp's bloom and flare only where its glass is (GlassAt; the far glow from 60 to 150 m); room for 64
  sprites and 96 lights; a lighthouse's narrow far-reaching light (a cone under 10 degrees, 150 m or more) as a beam in
  the air, out to 1600 m.
- renderer.cpp: every lamp whose light reaches the view, however far (was its range and 250 m), every bulb (was 400 m),
  their light through the haze exp(-d / 1800); a vehicle's lens drops kept to its glass by the depths (in a vehicle).
- The effects' particle shaders (0F79C073..., 2787E655..., 56764E04..., 66C95D39..., 92E4718A..., CB440507...,
  DE60E17F..., E20CB262...): their colour scale times 0.35 of the key light and 0.65 of the sky's when not added
  (draw.cpp, committed with 0e9368d2).
- sky.cpp: the rain's streaks fade near the eye and out (no hard edge to its cone).
- night_lamps.cpp: the large buoys' red lamp.

**Tested:** partly, in the user's own builds since 18 Nov; the orbs, the far lamps, the lighthouses and the buoys not
in a run of their own at night.

## D217 — Vehicles wet in the rain; drops raindrop-sized and plenty; the runs by the rain and 60 times a second (2026-11-19, Claude for Battlefield 1943 Reborn)

**Why:** the user, 19 Nov 2026: "I though we applied weapon the wetness shader to vehicle externals too", "just
vehicles", "The wetness drops on vehicles should be rain-drop sized, and plenty", "more frequent droplet runs on
vehicles and the player weapons too", "The droplets on the thompson are dens enough, but not on things like the
Bazooka", "The droplet runs should be a lot more frequent and faster when directly in the rain, but when under cover,
the drying begins", "Speed and frequency of all droplet runs (including the player camera) should be tied to weather
percentage too", "The wetness shaders should all animate at a smooth 60fps", "Is the wetness not bump shaded anymore?"

**What:**
- draw.cpp: a draw whose index count is a vehicle body's (the night lamps' models: the jeeps', the Sherman's, the
  Chi-Ha's; SetWetModels) is wet as the first person weapon is: its colour (the highest numbered DXT1 texture of 1024
  texels or more: the Sherman's texture 4, runs vtint1 and vtint4) drawn with a wet copy, its other large textures
  only for raised drops if the shader finds them a normal map, and the wet shine.
- A vehicle's copy twice its texture's size with cells a quarter as large: drops of about 5 to 14 mm; drops in 0.6 of
  the cells (0.45 on a weapon, 0.25 before); a long weapon's texture (the bazooka's 1024 x 512) has cells smaller by
  its height over its width; the drops seeded by a fixed key, so the colour's and the normal map's line up.
- The runs: in up to 85% of the columns by how wet and how hard it rains, on a clock 0.3 to 2.5 times the world's by
  how wet (building in the rain, drying under cover) and 0.6 to 1.4 times by how hard it rains here, summed so a
  change of pace never jumps; the lens's drops on a clock 0.4 to 2.4 times by the rain, more runs the harder it rains.
- The wet copies made anew 60 times a second (15).
- Also: the clouds' shadows (D214) fall to 15% of their strength as the weather here closes in (under a 100% storm the
  tank and the ground were left nearly black, runs vwet1 and vwet2); the storm's deck (D208) filled in from 30% of
  its strength, whole by 70% (run stormsky1: broken cumulus at 75%); the part of D214's shader split that commit
  missed.

**Tested** (Guadalcanal, a 100% storm): runs vwet5 (the Thompson with plenty of raised drops, the Sherman covered in
small glinting drops at its own brightness, 51 against 29 before), stormsky2 (the deck grey toward the storm). Not
tested: the bazooka (the scripted weapon switch did not switch), the jeeps and the Chi-Ha, the runs' pace by eye.

## D219 — The first person arms and gun take the soft shadows and the day's light: checked, nothing to change (2026-11-19, Claude for Battlefield 1943 Reborn)

The user, via the main session, 19 Nov 2026: "Does the new lighting and soft shadows not cast on the player hands and
weapons? If not, we need to fix that."

**What was checked** (dev build, hosted LAN match, Wake Island's carrier spawn by day; `NXR_WEAPON_DUMP=<seconds>`, a
new aid: on two frames, each stage of every draw ClassifyWeapon takes for the weapon's, its shader, its light table row
and c0 to c11):
- The arms and gun are four draws a frame. Two are depth only (vertex shaders 24B86F2A71BCEE10, 3F2CFC0513DC9F30, no
  pixel shader). The two seen are drawn by vertex shaders 2CB905C57FA0080E and 574AF3705A349158 with 21 pixel shaders
  between them (our runs' pipelines): every one samples outdoorLightShadowmapTexture (fetch constant 0, so D197's soft
  filter applies) and takes the outdoor light; all are in bf_lights.inc (D199), the vertex shaders with their shadow
  transforms at c0, c4, c8, so the moved sun's turn reaches them too.
- Their shadow transforms are the world's own (the same rows as the world's draws in that frame) and their bone
  matrices place them at the camera's place in the world (-548, 230, 15 on the carrier): they look the shadow map up
  where they are, as the world does.
- On screen: the forearm 178 of 255 on the sunlit deck, 104 in the landing boat under the davit (run hw_w1); at the
  spawn by the hour 78 at 7:30, 129 at 9:00, 176 at 14:00, the gun 97, 120, 156 (runs hw_h7.50 to hw_h14.00). At
  night D188 lights them evenly from the lamps; D214/D215's cloud shadows darken them with everything else.

**Not done:** a clean walk from full sun into a building's shadow (two tries left the shadow band or the deck); the
shadow seen on the arms is inferred from the shaders and the boat's frame.

## D220 — The first person arms' and gun's bones eased at each tick while the aim moves them (bf_smooth_bones_aim) (2026-11-19, Claude for Battlefield 1943 Reborn)

**Why:** the user, 19 Nov 2026: "Player hand model and weapons also banded around ... Let's also do the Animation
smoothing on player hand model and weapons", then, asked whether to keep it: "Keep it". With Animation smoothing
(bf_smooth_bones) on, the bone nearest the eye was left unblended ("moves between ticks") for as long as the view turned
or the soldier walked (run gbones2): the aim moves those bones every frame, so the blend left them as the game had them.

**What** (bf/src/tick_blend.cpp): with bf_smooth_bones, a bone the game also moves every frame has only its change at the
tick eased in: shown = now - (1 - f) * jump, jump the first value after the tick less the last before it; what changes
between ticks passes at once (EaseTickJump, for the bones placed by position and rotation and those with a whole local
matrix, the latter made square again at the game's row lengths); also a frame two to four ticks after the last.
bf_smooth_bones_aim (on) switches it. Dev aids: BF_DEV_BONELOG_AFTER (start the bone log later), the bone's offset in the
camera's frame in the log (dev_matscan.cpp: the view's rows).

**Measured:** runs gbones3 to gbones5 (Guadalcanal, turning and walking): every such frame eased; the followed bone moved
under 1 mm a frame against the camera with or without it, so no difference was measurable there. The gun's choppy
bounce itself was D203's (its springs at 60 ticks). Kept at the user's word.

## D221 — A new round's weather taken at once, and what the last round's rain left dried (2026-11-20, Claude for Battlefield 1943 Reborn)

**Why:** TekRantGaming, 20 Nov 2026, on Night Ops (the storm builds with the tickets; each round starts as a clear
night): "towards the end of a game, we have a full on storm, and then the game would end and load the next match. And
briefly, for the first moments of the new match, especially during the loading, you could hear the storm. And then once
you were in, you could actually see the storm, and then the clouds would begin to retreat." His log: the storm's line at
100% at the level's load, then 82%, then 42% about 20 s later. D211's easing (at most 4% a second) took the host's clear
night as a change to ease toward, from the last round's full storm: 25 s of it on screen in the next round.

**What:**
- `SnapWorld(seconds)` (native_x_renderer.h): for that long the world is drawn as SetWorldState last set it, at once,
  not eased; each jump so taken (the weather, cloud cover or fogs) is counted and logged ("world (D221): a new round's
  taken at once: weather 100% to 0% ..."). Easing within a round is unchanged.
- The sky (Sky::DryForNewRound): at each such jump, and at each SnapWorld call, the lens's rain and splashes, the wet
  weapon and hands, the storm's sway and blurred edges go to 0 at once, and the clouds' frames start again (their blend
  would show the last round's clouds for a moment).
- The game (bf/src/weather_audio.cpp, from auto_join.cpp's view of the game's menu events): the round's end
  (WinningTeamIsSet) silences the weather's sounds and drops the thunder on its way; the next level's load in a match
  (playerRoleReady; also a change of map without a round's end) silences them and calls SnapWorld until the level is in;
  the level in (the second gameIsReady) brings them back and calls SnapWorld(20 s), so a host that says the new round's
  weather a few seconds late is still taken at once. The host (server/gmhost.py) reads the round from its server's
  match.txt on players' asks too (at most twice a second), not only on its 5 s heartbeat.

A first join keeps the existing rule (the first look and a change of the world's source are taken at once), so a
player joining mid-storm sees the storm at once; a round that ends clear changes nothing.

**Tested** (local Night Ops server on Guadalcanal, its tickets set to 1 so the host went to the full storm, a suicide
ended the round, the same map next; BF_DEV_WEATHER_SOUND_LOG=1 mixes the weather at full volume into
BF_DEV_WEATHER_SOUND_OUT's file and not the speakers): runs rsnap1 and rsnap2. The join came mid-storm and showed it at
once. The round's end silenced the weather within a second; the recording was digital silence for the 25 s of the end of
round screen and the load (the storm stood at about -20 dB RMS before it). The host's clear night came a second into
the load and was taken at once ("weather 100% to 0%, clouds 90% to 15%, fog 50% to 0%"); the new round's first frame
(shot_104 in both) a clear starry night, the rifle dry, no storm line after; only the light breeze heard (-76 dB).
Run rsnap3, a regular server's round change (its weather unchanged): no jump taken, the next round as before.

## D224 — The storm's type: a thunderstorm by default, the hurricane chosen (2026-11-20, Claude for Battlefield 1943 Reborn)

**Why:** the user, 20 Nov 2026: "Let's also make the Hurricane and storm-type that can be selected, but by default we
just run the regular storm in the same localized fashion. I want to add more storm types later too." Since D208 the
match's weather was always a hurricane (an eye, its wall, spiral rain bands).

**What:**
- storm.h: `StormType` (kStorm 0, kHurricane 1, kStormTypes), `StormTypeName`, a type in `Storm` and in `Of()`. Every
  type is placed and sized by the weather as D208 and D213 have it (out at sea from 25%, over the level at 100%, never
  bigger than it), so the weather's percentage means the same whatever the type. Each type is its own case in `At()`
  and in the GLSL's `storm_at`: a new type is a number, its name and its two cases.
- kStorm, the default: a thunderstorm, one cell with no eye (the eye's radius 0, so the storm's towers seen from
  outside, D212, stand at its middle): full strength out to 0.55 of its rain shield, fading to its edge, with heavier
  cells in it (three lobes that drift round with the clock), never under 0.8 of full inside the core. The towers, the
  anvil, the volutus (D211), the lightning (placed in its rain shield and kept by its strength, D213) are the same as
  the hurricane's.
- kHurricane: D208's model unchanged.
- The clouds' constants: storm_b.z is 1 plus the type while there is a storm (0 none); every `> 0.5` test of it stays.
- The match's world (gm_host.cpp `bf_world_storm_type`, 0; world_sync.cpp; the public `WorldState::storm_type`): a byte
  last in both the "BF43WRLD" ask and the "BF43WRLR" answer. A game or host from before it sends and reads none, so it
  means a thunderstorm; the server package's gmhost.py sends none, so its servers (Night Ops too) have the
  thunderstorm, sized and placed as the hurricane was.
- Custom Match: Weather and time > Storm type (STORM, HURRICANE; `private_match_storm_type`, passed as
  `--bf_world_storm_type`). The F9 panel: a last row, Storm type (Storm, Hurricane). Tests:
  `native_x_renderer_storm_type` with `native_x_renderer_world_own`.

**Tested** (dev build, hosted LAN matches on Guadalcanal at 15 h, weather 75%; runs stype0, stype1): the host took
the type from Custom Match ("storm type storm", "storm type hurricane" in its log), each player's game drew it ("a storm
centred at 529, 767 ... its eye 0 m, its rain shield 872 m"; the hurricane's eye 78 m), no shader errors. Not tested:
the F9 row switching it in a running match, a dedicated server with the flag.

## D225 — Thunder by what struck and how far; the storm's wall only out from under it (2026-11-20, Claude for Battlefield 1943 Reborn)

**Why:** the user, 20 Nov 2026: "why is none of the other audio variants playing for distant thunder or lightning
too? I only hear the same lightning audio, and its the one meant for lightning strikes being extremely close to the
player." Since D213 the storm is the level's size, so every strike falls within about 1.5 km; the thresholds of D184
and D207 (a close crack within 1.5 km, a distant strike to 5 km, the clouds beyond) gave every one the close crack
(runs stype0, stype1, stypeb0, stypeb1, merge20_smoke: 27 strikes 507 to 1409 m off, all Lightning_strike-close.mp3;
one at 3.3 km, before the level's middle was known, the distant one). And a view from just inside the storm's rain
shield (stypeb0, stypeb1) showed its wall (D212) as a flat dark slab at the horizon with hard ends.

**What:**
- The public `LightningStrike` says whether it is a bolt to the ground (`bolt`, FlashInSlot's). weather_audio.cpp: a
  flash inside the clouds plays the clouds' rumble (Lightning_clouds.mp3; 1.5 overhead to 0.5 at 3 km); a bolt within
  350 m the close crack (3.5 at 150 m to 2.8 at 350 m: D208's startling one, near the players); a bolt further off the
  distant strike (Lightning_strike-distant.mp3; 2.2 at 350 m to 0.8 at 3 km). The delay is the sound's time as before.
- The storm's wall seen from outside (sky.cpp, D212): shown only out from under the storm, fading in from its rain
  shield's edge to 1.35 times it (was: outside its dense part, three quarters of the shield, where its deck still hid
  all but a strip of it), its sides fading over 40% of its width (15%).
- Its foot a curtain of rain: falling streaks, lighter and hazier toward the horizon (under a near deck only its
  lowest degrees show; as a flat dark slab they read as nothing, run stypec1). And none from near the cloud base up
  (0.7 to 1.05 of the base's height over the sea): drawn as from the ground, from a camera 2.7 km up it stood out of
  the cloud deck as a dark rectangular column (run hurrup_a); up there the clouds below show the storm.
- D223's lightning inside the clouds (still uncommitted, with D221 to D223): softer and wider (a glow 900 across and a core of 520,
  14 and 20; was 800 and 240, 14 and 55), shaped by the cloud's density (0.25 to 1.35 of it): the bright core 240
  across lit a ball that the flat base and top of an even deck cut into a rounded box (run stypeb1).

## D226 — The hurricane as one is seen from above: a clear eye, a dense core, open spiral bands (2026-11-20, Claude for Battlefield 1943 Reborn)

**Why:** the user, 20 Nov 2026: "The Hurricane shape also looks like a nipple, or a puckered anus... Can we make that
more realistic?" Since D213 sized the storm to the level, D208's eye (0.09 of the rain shield) was about 80 m across
at 90% weather on Guadalcanal, smaller than one of the clouds; its strength rose over 1.8 eyes like a cone, and the
cloud deck followed it into a funnel with the level's own cumulus left inside.

**What** (storm.h, sky.cpp):
- The eye 0.18 of the rain shield. Where it rains (At, storm_at): calm inside 0.85 of the eye (0.12 of the storm),
  full by 1.25 of it; the central dense overcast full out to 0.4 of the shield; then two spiral bands, open (about 20
  degrees) and anticlockwise, at least 0.75 between them as D213 has it, so at 100% the level stays covered.
- The clouds' own look (`storm_look`, GLSL): the eyewall rising steeply (0.85 to 1.05 of the eye), the central dense
  overcast whole out to 0.38 of the shield, the bands with clearer gaps between them (a quarter of the cloud there);
  and `storm_eye`: the cover taken away inside the eye, so it is clear sky to the sea. The cloud's cover, its layers,
  its density and its kind (the towers) follow the look; D223's dark storm clouds too. A thunderstorm's look is its
  strength (D224).
- The storm's towers seen from outside (D212) stand at the new eyewall (1.1 eyes).

## D227 — A GPU particle fluid: threejs-particle-fluids' air and tracers as GLSL compute passes (2026-11-20, Claude for Battlefield 1943 Reborn)

**Why:** the user, 20 Nov 2026: the hurricane should "look and spin like a real hurricane", form from spinning clouds,
"controlled particle-based fluid dynamics into the clouds" with planes pushing the cloud and leaving wispy trails, the
ground fog particle based too, from "a system we could incorporate ... threejs-particle-fluids. We just need to make
sure it stays performant." The plan, step 1 of 6: `source/.handoff/FLUID-WEATHER-PLAN.md` (not in git).

**What** (`src/fluid.h`, `src/fluid.cpp`; MIT notice: `THIRD_PARTY_NOTICES.md`): `FluidSim`, in 2D (x and z) or 3D.
One step: forces and the prediction (gravity, a pull toward a low fading past a reach, swirl round it, a Coriolis term,
drag; in 2D an updraft that takes the air reaching the centre out of the layer and returns it at a source radius, the
overturning that the Coriolis term turns into spin); a spatial hash grid (each particle's cell hashed into a table of
twice the particles, counted with atomics, one 256 wide workgroup's scan, a scatter) and neighbour lists (up to 64,
each bucket visited once); the density constraint as in the library (XPBD's lambda, only against compression, a
relaxation of a share of the gradient norm at rest), its corrections at most a quarter of h an iteration; velocities
from the solved positions, at most `max_speed`; vorticity confinement and XSPH viscosity; tracers emitted at up to 16
emitters into a ring of slots and carried by the air's interpolated velocity until their life is over; their density
splatted trilinearly with fixed point atomics into an R32F volume. Every pass is followed by a UAV barrier on the
buffers. The rest density is the kernel summed over a lattice at the rest spacing (mass 1). The 18 storage buffers are
bound as plain ones (`SHADER_VARIABLE_FLAG_NO_DYNAMIC_BUFFERS`): as Diligent's default dynamic storage buffers the
pipelines passed the AMD driver's limit of 8 (run fluid1). GPU time per step in the log every 300 steps;
`LogState` (a developer's, it waits for the GPU) logs the particles' and tracers' state.

**Test** (`NXR_FLUID_TEST=1`, a developer's: a 2D storm of 16,384 particles 24 m apart in a 4 km square, 32,768
tracers fed on a ring at 1.2 km, 30 steps a second, its density over the picture's top left corner):
- run fluid2: 0.26 ms of GPU time a step on the RX 7900 XTX (3 density iterations; the longest 0.4 to 0.65 ms).
- runs fluid2 to fluid4: still tracers; fluid4's state: the air at 7 to 13 m/s, the first step throwing particles at
  650 m/s (the relaxation near nothing on a slightly packed start), and the tracers' ring reused every 11 s (96 a step
  into 32,768 slots) before they could go anywhere. Then a relaxation of 0.2 of the norm, the correction and speed
  caps, 16 tracers a step: run fluid5, the air at about 6 m/s, nothing not finite, the tracers living their 60 s and
  their plumes bending into commas. A few particles still at the 120 m/s cap; the storm's own tuning is step 3.

## D228 — A storm's real radar as its pattern: Hurricane Michael baked eye-centred (2026-11-20, Claude for Battlefield 1943 Reborn)

**Why:** the user, 20 Nov 2026: "The hurricane shape should look and spin like a real hurricane. Use real recorded
weather radar data to base storms from." Michael chosen by the user (a compact Category 5 at landfall, a clear eye and
tight bands, well covered by coastal radars); the fluid weather plan's step 2.

**What:**
- `tools/storm_bake.py`: from the Iowa Environmental Mesonet's archive of the NWS NEXRAD composite reflectivity (n0q,
  0.005 degrees a pixel; public domain), twelve frames, 17:00 to 17:55 UTC on 10 Oct 2018 (27 MB, not in git). Each
  frame: the eye found (the clear disc inside the brightest ring of the smoothed field), the frame re-centred on it out
  to 150 km, 256 x 256, north up; the sectors past the radars' reach (no or weak echo, under 10 dBZ, joined to the
  disc's rim, widened 4 pixels) filled from the storm turned about its eye, half a turn (its two main bands lie on each
  other) else a quarter; 15 to 55 dBZ as 0 to 255. (Filled from the nearest bearing with data instead, the gap's own
  dark edge was copied in and the sectors stayed black: bakes 2 and 3.) The eyes found track the storm north-north-east
  at about 29 km/h, at 30.0 N 85.5 W at 17:30 UTC: the landfall's place and time.
- `bf/data/weather/storms/michael_2018.bin` (786,464 bytes; README beside it): "BFSTORM1", sizes, the eye's radius in
  pixels (8.4), km a pixel (1.17), minutes between frames (5), the frames.
- The game reads it at the start (`bf/src/storm_patterns.cpp`) and hands it to the renderer
  (`native_x_renderer::SetStormPattern`), which makes it a 2D array texture (R8, a layer a frame) on its thread.
- `DebugTextureView` (`src/fluid.h`): a developer's view of a 3D volume's slice or an array's layer over a corner of
  the picture; the fluid's density view uses it too. `NXR_STORM_PATTERN_DEBUG=1` shows the pattern, a frame a second.

**Tested:** run pattern1 (dev build, Guadalcanal): the game read the pattern, the renderer made its texture ("12
frames of 256x256 ... made"), and the view showed it beside the fluid test. Not yet used by the clouds: that is step 3.

## D229 — The storm made of the sky's own clouds: a flow map carried by the fluid (2026-11-20, Claude for Battlefield 1943 Reborn)

**Why:** the user, 20 Nov 2026: "The volumetric clouds and sky should be part of this PBF system, right? That way when
a storm forms, it forms FROM the clouds, pulling them in." The first try (tracers placed by the radar pattern and
splatted into a density the clouds read, run fstorm2) drew a smooth grey disc with a hole: the specks mixed into an
even field and the clouds lost their shapes to D217's deck fill. A cloud-cover field carried by the air (run fstorm3f)
showed nothing either: at this scale the sky's weather map is almost flat, and the clouds' shapes come from the 3D
shape noise, which did not move.

**What** (fluid.h/.cpp, renderer.cpp `StepStormClouds`, sky.cpp `cloud_density`):
- `FluidSim::Config::cloud_dims` (2D only): the air's velocity splatted onto a grid (a tent of 1.5 cells, fixed point
  atomics) and resolved; a flow map (RGBA32F, 256 x 256) carried along it each step (semi-Lagrangian, a midpoint trace
  back): two phases, each the offset back to where the air at a point was when its phase began. A phase starts again
  every 90 s, the two half a period apart, each with no share as it does, so what they carry moves with the air and is
  never stretched past a phase.
- The convection (RGBA16F, r): carried too and drawn toward the storm's pattern (D228) over 20 s: scattered cells
  blended by the organisation into Michael's radar, its radius to a power so the radar's eye is the storm's (0.18 of
  the shield) and its outer bands fall inside the shield (in place of a zoom that showed only its solid core). Its g
  and b: the sky's weather map read where the flow map says, both phases blended (baked here so the march reads it
  once).
- The storm's sim is the same 16,384 particles in the storm's own frame (its shield 1,000 units); no tracers. 0.25
  ms a step on the RX 7900 XTX (all of it: the air, the grid and both fields).
- The clouds (`cloud_density`): inside the field (2.8 shields across round the storm, fading at its edge) the weather
  map is the carried one and the shape noise, its foot and the detail are read at the flowed places, the two phases'
  shapes blended with their contrast kept (an even blend of two noises is flatter than either); the convection is
  the storm's strength there in place of D226's drawn look. Outside the field, nothing changes. `storm_c` carries the
  field's on, phase a's share, its width and how open the eye is; `native_x_renderer_fluid_storm` off: D226's drawn
  storm.
- The fog pass's clouds-over textures (D221) are now copies the fog owns, stamped with the frame they were laid in,
  and new cloud targets are cleared as they are made. These were meant to fix a device loss (a GPU page fault, a read,
  in the level's first seconds, storm runs since D226), and do not: the clean runs that seemed to show it (bis_noback,
  bis_noread, fix_c, fstorm7) were chance; runs above90 to above90c and sp_off1 to 3 lost the device with them in. The
  fault's address is one no object made after the device was ever bound to (the binding report, below). Open.
- Diagnosis aids: `NXR_DEVICE_FAULT=1` (device.cpp: VK_EXT_device_fault, a lost device's report in the log; it said
  "GPU page fault detected", a read; with it VK_EXT_device_address_binding_report, every memory bind kept and the
  fault's address looked up in them); `NXR_SKY_DEFINES=A,B` (the sky's shaders compiled with those #defines, for
  bisects); `NXR_STORM_FLUID_DEBUG=1` (the convection and the flow map, a checkerboard as carried, over the picture);
  `NXR_STORM_FLUID_LOG=1` (the particles' state every 10 s); `NXR_STORM_FLOW_SPEED` (the flow's speed, for tuning).

**Tested:** run fstorm7 (dev build, Guadalcanal, hurricane at 90%, 150 s): the flow map wound into a spiral round
the eye, the convection in Michael's rings across the shield. The device loss above still happens in most storm runs.
Not yet seen from over the clouds, nor a storm building up.

## D230 — Every texel read of the sky's shaders stays inside its texture: the storm's device loss (2026-11-20, Claude for Battlefield 1943 Reborn)

**Why:** most storm runs lost the GPU on the AMD driver (RX 7900 XTX) within the level's first seconds (D229's open
item): "GPU page fault detected", a read at about 0x2282_xxxxxx to 0x228A_xxxxxx, far from every reported memory bind,
and by the breadcrumbs (VK_AMD_buffer_marker) always inside the fog pass ("fog and rain"). Ruled out before by
batches: the cloud shadows, the fluid, the recorder thread, the OBS layer, the cloud march's changes, the sea's
reflection, lightning; the race check found nothing.

**Cause:** the fog pass reads texels at places worked out in floats with nothing keeping them inside the texture:
the rain's kick-up reads the depth eight pixels under the pixel with no check at all, its loop leaves on
`q.y < 0.0 || q.y >= view.y`, which a place that is not a number passes, and the reflection's march reads at
`ivec2(uv * view.xy)` for a `uv` up to 1. Vulkan leaves a `texelFetch` outside the texture undefined without robust
image access; an integer made of a NaN is as far outside as a place can be. Which of the reads it was is not
established: all of them are held now.

**What:** `WithSafeFetch` (sky.cpp) puts one line after `#version` in every pixel shader of the sky:
`#define texelFetch(t, p, l) texelFetch(t, clamp(ivec2(p), ivec2(0), textureSize(t, l) - 1), l)`. All 23 reads of the
sky, clouds, fog, sea, lens and wetness passes go through it, and any added later. `NXR_SKY_FETCH_RAW=1` (a
developer's) compiles them as before. The shader test (`NXR_SHADER_TEST=1`) compiles what the game compiles.

**Tested** (dev build, Wake Island, hurricane at 90% from above, 60 s, lightning off, `NXR_DEVICE_FAULT=1`; two
batches): with it 0 of 12 runs lost the device (d230fix_1 to 4, d230fixb_1 to 8); the same build with
`NXR_SKY_FETCH_RAW=1` lost it in 3 of 6 (d230raw_1, d230rawb_1, d230rawb_3: the same fault, "fog and rain" in
progress). The shader test: 0 failed. Not tested: other maps and storm types, long sessions, lightning on, other
GPUs. The other passes' `texelFetch` (night.h, dof.cpp, taa.cpp and the rest) are as they were.

## D231 — The ground fog as a surface fog that is pushed about (2026-11-21, Claude for Battlefield 1943 Reborn; in progress)

**Why:** the user, 21 Nov 2026: "low-cost, close to the ground fog that can be pushed around by any dynamic object,
bullet shots, explosions, vehicles, characters, wind and rain, but regenerates itself"; "surface fog ... thick and
shin-high, like a bog or swamp"; "more wispy like in The Division"; "should also be elevation based"; "it shouldn't flow
through objects, but around them"; "use the heightmap of the terrain".

**What:** the ground fog setting is no longer a share of the tall haze layer (that is the storm's mist alone now). It
is a low layer of billows on the terrain or the sea (sky.cpp, the fog pass): the ground's height from the terrain's
heightmap (grass.h's TerrainFrame, kept for it), eight steps through the clouds' own noise volumes, lit by height, in
patches (sparseness), low ground only (to 14 m over the sea). `ground_fog.h`: a field round the eye (256 x 256 over
192 m, stepped at 60 Hz) carries how much of it is where: moved by what the game says moves (tick_blend.cpp's part
trees and skinned roots, `SetFogMovers`), holed where the player's own shots strike (recoil.cpp, `FogShot`), steered
round what stands 25 cm to 1.8 m over the terrain (a map marked from the view's depth), back to rest in about 12 s.
Settings: density (the old ground fog), `ground_fog_height` (0.2 to 5 m, 0.5) and `ground_fog_sparse` (0 to 1, 0.7), in
the match's world (gm_host.cpp, world_sync.cpp: two floats appended to the packets), the F9 menu and cvars.

**Tested** (dev build, Wake Island, on foot; runs gfog_1 to gfog_16): no device lost in any; the shader test 0 failed.
Seen in screenshots: the fog on the terrain only and off walls and vehicles (gfog_11), a path walked through it
(gfog_9, gfog_14), a hole where a burst strikes that fills again in seconds (gfog_16). **Not done or not tested:** its
GPU time; water; night with the new look; the other players' shots and explosions (no hook gives their places); the
Custom Match screen and server/gmhost.py do not have the two new settings; fog flowing round an object in motion.

## D232 — The clouds' mass as particles, the same in every game (2026-11-21, Claude for Battlefield 1943 Reborn; a developer's first slice, off by default)

**Why:** the user, 21 Nov 2026: "You positive we couldn't make our clouds part of the particle system with the
storm?"; the Vortex Plume of threejs-particle-fluids as the model; "The simulation should be stepped at 60 Hz"; LOD
("Lower more static at distance, more particles at close"); "so the client's clouds and fog are in sync".

**What:** `cloud_mass.h` (`native_x_renderer_cloud_particles`, off): 131,072 tracers over 24 km round the eye are the
clouds' mass; their count in a 256 x 256 grid, blurred and eased, is read by cloud_density in place of the weather
map's cover. A tracer keeps no state: its birth is a hash of its 1.5 km square of the world, its number and the turn
of its life by the match's clouds' clock, and it has gone with the clouds' wind and round and into the storm for its
age: every game works out the same ones. The far, coarse level only (20 Hz, no fluid). The storm's fluid and the fog
step at 60 Hz. The fog's gusts go by the match's clock.

**Tested:** two separate runs' cloud decks at the same moment differ by 0.8 (mean grey level of 255; 3.3 between shots
8 s apart, 6 to 7 against other skies: runs cmass_d1, cmass_d2); no device lost. **Not good yet:** with it on the deck
is thin scraps and stretched streaks, not clouds (the user: "The clouds don't look anything like clouds yet"). Not
measured: its GPU time. Not built: the near level (the air as a fluid round the player, a plane's wake).

## D233 — The electric weapon skin, ours: on the gun's own mesh, a spiral of smoke with lightning in it (2026-11-22, Claude for Battlefield 1943 Reborn)

**Why:** the user, 21 and 22 Nov 2026, of the skin that came with the merge: "There's a weird texture ... looks really
bad"; "Let's get rid of the repaint entirely, and do our own"; "Can you make it a reactive skin on the model? it looks
like screenspace"; "keep the vortex locked to the weapon model"; "Reloading should also make a cool lightning effect
when the new magazine is inserted that electrifies the weapon"; "make sure the clouds never obstruct the weapon's
ironsights"; "more of a uniform spiral around both guns"; "commit weapon skin as an unlock, and only shows if equipped
from main menu's unlock screen".

**What:** `weapon_skin.cpp`. The Thompson's and the Type 100's colour texture is copied with dark metal and gold wood
(its alpha marks the wood). The gun's own draws are kept and drawn again (`DrawPath::KeepSkinDraw`, `DrawSkinOver`:
the motion pass's copy of the vertex shader, depth equal) into a target of the depth's size with the storm's light
by the texture's own coordinates, so the lightning lies on the model. The gun's place is from its bones (the body's
and the magazine's, `kGuns`), moved as the Iron Sight option moves the gun's picture (`WeaponLayerMoveNow`). Round
it, 512 puffs of smoke as sprites in two bands fed in a third of the way along the gun, a turn and a fifth to the
muzzle on either gun, a third slower along than at first, settling 4 mm off the gun's own section there (`kShape`, 12
stretches a gun, from the exported models); lit from within by five bolts that are not drawn (a stroke, then a glow
of a third of a second). A new magazine going in (its bone back within 8 mm after 4 cm out) runs a charge over the
gun. While aiming the top of the gun and the sight picture are kept clear. By night the overlay goes on after the
night pass. Shown only while unlocked (`bf_skin_electric`) and equipped (`bf_skin_equipped`), set on the main menu's
new Unlockables screen (`UnlockablesPage`). `weapon_export.cpp` (a developer's, `NXR_INTERNAL_TOOLS`,
`NXR_WEAPON_EXPORT=<folder>`): the first person weapons' meshes, bones and textures written out, for this and for
skins to come; what it writes is the game's own art and stays out of the repository.

**Tested** (dev build, Wake Island, 3440 x 1440, RX 7900 XTX; runs skin37 to skin51, unlock_*): the offline shader test
0 failed; no device lost. Its GPU time 0.04 ms for the texture (0.08 on the Type 100) and 0.13 ms for the overlay.
Seen in screenshots: both guns by day at rest, in a reload and aiming with Iron Sight; a flash in the smoke by night
at its real timing (skin40_real); the Unlockables screen locked, equipped and taken off; a match with it unlocked and
not equipped (no skin) and equipped (skin). **Not done or not tested:** (since 23 Nov 2026 it is unlocked by
playing a full match under a hurricane: bf/src/fov.cpp SkinMatch); other players do not see it (planned: the
skin's number in the relay's state, the third person gun's draws matched by their texture); its motion was judged
from stills only; night and aiming were not run again after the last changes to the spiral; the plain Zoom In aim.
Tried and dropped on the way, each on the user's word: a fluid's density volume marched round the gun (0.2 to 0.8 ms,
"blobs of snow"), six-way lit sprites baked here.

## D234 — The hurricane grown from the sky's own clouds, its eye clear, and the planes' wakes (2026-11-23, Claude for Battlefield 1943 Reborn; in progress)

**Why:** the user, 23 Nov 2026: "be like PBF so the hurricane can be built from those clouds actually swirling and
forming together"; "The eye of the hurricane looks covered. It should be a clear hole to the sky"; "The storm is too
tall, which results in it looking like a giant cylinder vs a swirling hurricane"; "if a plane were to fly through a
cloud, it should punch out the other side pulling a short wisp trail like it realistically would". Step 3 of the fluid
weather plan (D229) drew its convection toward Michael's radar pattern in 20 s, so the spiral was a picture the fluid
barely moved: from above the shape stayed put while the texture slid.

**What:** fluid.cpp's cloud pass: the convection's r is a cloud amount carried by the fluid that lasts (no phases). It
grows, in about 12 s and faster at the eyewall, out of the clouds the sky has where the air brought them from (the
sky's shape volume, bound through `Sky::CloudShape` and `FluidSim::SetCloudShape`, cut by the sky's cover), lets go
over about 30 s while carried on round, so its trails are wound into arms; it gathers where the flow converges and is
cleared in the eye as it opens. The radar's pattern only scales how readily it grows (0.72 to 1.27). A level starts
with no storm cloud: it forms over the first minute or two. sky.cpp: the eye's density is cleared fully (it kept 3%,
still opaque over the layer's depth), and storm.h's rain floor in the eye (0.12) is gone, in `At()` and `storm_at`
both. A hurricane's deck is low and wide (no deeper than 0.3 of its rain shield's radius at the wall, under half that
at its edge), its columns heaped by the shape noise and the gathered amount, in the drawn look's spiral bands, filled
through most of its depth; the storm darkening only where the deck hides the sun, so its top is white. With the
clouds' mass (D232) the rain shafts are marched only inside the storm's rain shield. The wakes: renderer.cpp
`WakeInputs` cuts the paths of the camera (moving 35 to 400 m/s) and of the ground fog's movers with a plane's radius
(within 110 m) into 150 m pieces while in the clouds' heights, kept 45 s, the newest 24, the piece still being flown
among them (`Sky::Inputs::wake`, the uniforms `wake_seg`, `wake_n`). The cloud shader finds once a pixel which pieces
its ray passes near; only samples in that span read `wake_density`, at 12 m steps: a hole with a torn edge, 22 m
across plus 0.5 m a second, held 15 s and closed by 45 s, and for 30 s a tongue of the cloud drawn out past the far
side where there was none. `NXR_WAKE_DEBUG=1` tints the rays near a wake red.

**Tested** (dev build, Guadalcanal, 3440 x 1440, RX 7900 XTX; runs st_form1, st_eye1, st_final_storm, st_wake3,
st_fair_after): no device lost; the offline shader test 0 failed. Seen: three arms merging into one spiral round the
eye between 60 and 140 s (from above); down through the eye to the sea, and blue sky inside the eyewall from the
ground at its centre; a tunnel along a flight path from inside a cloud (before the torn edge). The fluid step 0.24 to
0.27 ms (as D229); the wakes about 0.07 ms with 16 pieces (rough); fair weather looks as before (no before shot
taken; by the code every change is gated on a hurricane, its eye or a wake). **Not done or not tested:** the centre
round the eye reads as a smooth disc; D211's roll cloud draws a hard white arc round the storm from above; from the
side far off it is a low grey bank; the storm forming as the match's weather rises (only a fixed 90% from the level's
start); a wake's groove seen from above, the wisp tongue, a plane flown by the player, other players' planes beyond
110 m (no feed for them); the wakes' cost from a pilot's seat; the blocky rain patch near the horizon in the default
mode (its far shafts are 20 steps over long distances, not held to the storm's cylinder as with the mass).

**Then, the same day** (the user: "The underside of the center of the storm protrudes like a nipple"; runs rp_a, rp_b,
d234v to d234v4, dev build, Guadalcanal, a hurricane at 90%): the far "blocky rain patch" was not rain (the same with
the far shafts off, run rp_b) but D212's storm wall, a dome painted on the sky behind the storm, now a thunderstorm's
only: a hurricane's deck is drawn by the clouds. The protrusion was a ring of the ordinary low clouds round the eye (the
storm's cloud is cleared a little further out than the eye clears the sky's), on their own base 400 m under the storm's
and sunlit down the eye: the clouds within 1.8 eyes of the centre now take the storm's base and the hurricane's height
cap. D211's volutus is a thunderstorm's only (round a hurricane, a smooth white ring at the shield's edge from above).
The smooth disc round the eye: its columns' heights also by the shape at a quarter of its scale (turrets), so from above
it has texture. Tried and dropped: the cover lowered over the deck's upper part and the fill's top worn by the noise (no
change seen), and the eyewall's rain in the fog pass to hide the eye (that pass does not run outside the storm; inside
it a grey wall). Seen: side and top views after; the offline shader test 0 failed; no device lost. **Not checked:** the
turrets' GPU time; a thunderstorm after these; the ground view inside the storm (a grey wall in runs d234v3 and d234v4
alike, the D194 storm mist or the camera's pitch, not looked into); still open from above: the wake, the far side view.

**The wake from outside** (runs wk1 to wk8, fair weather, cover 40 to 60%): `NXR_WAKE_TEST=x,y,z,dx,dy,dz,speed,after,seconds`
flies a plane that is not drawn along a straight line (a mover, as a near plane is), so its wake can be watched from
anywhere. Seen from about a kilometre (with `NXR_WAKE_DEBUG=1` to find it): the 12 m steps near a wake drew the reach's
edge as a seam and, eased in, the finely marched clouds stood in ribs like contour lines; the finer steps are gone (the
hole is now 28 m round growing a metre a second, wider than the march's steps, whose start is dithered each frame), and
its edge is torn by the detail's 3D noise (a 2D noise with the height added layered it). The ribs are gone (run wk7).
**Not seen:** the hole itself from outside (the path ran inside the cumulus: buried in cloud, as in life); the tunnel
from inside again since the finer steps went (the camera at 1,000 m was over the cloud tops, run wk8); the wisp
tongue; other players' planes beyond 110 m. Found on the way, not from this work by the code: in fair weather, hard
kinked creases round flat shelves of cloud seen from above (runs wk2 to wk4, the same place with the plane higher).

## D235 — The clouds' shadows half as dark; the shapes on the sea looked into (2026-11-24, Claude for Battlefield 1943 Reborn)

The user, 24 Nov 2026 (relayed, with a screenshot on foot by a beach looking out to sea): "Object shadows are still
dislocating themselves (seen over the water here). These shadows might be screen-space, which is why they dislodge and
move with the player camera", then "It specifically happens at later times of day and when flying a plane", "this
has been happening even before we built soft shadows into the game", and "Cloud shadows are also far too dark, and
should be about 50% light than they currently are".

**What** (sky.cpp, the fog's pass): the share of the scene the clouds' shadow takes halved, 0.89 to 0.445 (D221's
light through the clouds, exp(-5 x density), unchanged): about 12% darker under a cloud instead of about 24%, the
shadows' shapes and edges as they were.

**Looked into, not fixed** (dev build, the user's own graphics and weather settings, Guadalcanal at 17:30, a developer
camera gliding 40 to 80 m over the coast and the sea, BF_DEV_CAMERA_GLIDE):
- The dark shapes on the water in the glide were the clouds' shadows, drawn out by the low sun: the same with the sun's
  turn off (`--native_x_renderer_sky_sun_moves=false`, run sea_f_noturn) and with the shadow pass' casters clipped
  (sea_f_clip) as with neither (sea_f_base).
- The shadow pass already clips its casters' depth (a log line said so): no caster lies flattened on the map's near
  plane, the first suspect.
- The fog's pass has depth over all of the sea to the horizon (its distance view, `native_x_renderer_sky_debug=4`, run
  sea_dbg): the clouds' shadows on the water are worked out at the sea's surface everywhere, not in patches. A change
  that assumed patches (shadows where the depth held nothing) was tried and taken out.
- Not reproduced: the user's own view on foot (blocky dark shapes near the sun's glitter, moving with the camera).
  Next: the user's map, hour and place; the clouds' shadows off for a frame of it (`native_x_renderer_cloud_shadows`).

**Test runs:** the shared test profile (`runs\vtmpl\user`) still had `shaders_prepared_for = 0.4.7.1` while the dev
build is 0.4.7.4: every test run was a first start after an update, started the game behind the shader page and could
not host its LAN match ("The LAN match could not be started"). Set to 0.4.7.4 (not in git).

## D237 — No Sherman headlights on Japanese soldiers: the tank's farthest level of detail left out of the lamp models (2026-11-24, Claude for Battlefield 1943 Reborn)

The user, 24 Nov 2026: "Bots also occasionally still have vehicle headlights when running around as infantry", with a
screenshot on Guadalcanal at night (an enemy bot on foot through a sniper scope; two lamp glows and two pools of light
on the ground, no vehicle) and "We were not near a vehicle, no".

**Why:** a model's lamps (D163, D177) are placed on any output-sized draw whose index count is one of the models' and
whose matrix in the model's register is rigid, and lit while it moves (D178). Every mesh part in every level's files
(`*.res`, each part's name followed by its triangle count) was read for the twelve counts: eleven belong to their own
vehicle only (and its depth-only twin), but 7416, the Sherman's farthest level of detail, is also the Japanese
infantry's nearest head (Jap_Infantry_Mesh's HeadShape_head and Jap_Rifleman_Mesh's Jap_Head1, 2472 triangles). A
running Japanese soldier drawn near the camera matched it, and when the bone matrix in c26 happened to be a clean
rotation, the Sherman's two headlights were placed on him and lit.

**What** (night_lamps.cpp): the Sherman's lamps on its levels of detail 0 and 1 only (29745, 14685 indices). A far
Sherman (its lamps reach 30 m) shows none.

**Ruled out first:** a tank's depth-only twin (same count) placing lamps elsewhere: at the Wake Island airfield at night
with every vehicle's lamps forced on (NXR_VEHICLE_LAMPS_ON), the same three vehicles' lamps, at the same places,
with and without draws that write no colour (runs lamp_before, lamp_after).

**Not tested:** the fix in a match with a Japanese soldier near the camera at night (the bots stayed away from the
test camera); both builds pass.

## D239 — The game's shadows cast from the sky's sun at their source; the turn only as a fallback (2026-11-24, Claude for Battlefield 1943 Reborn)

**Why:** the user, 24 Nov 2026, of building shadows as the camera moves: "You see those shadows on building flicker
in and out though, right!?" With the day and night cycle, D181 turned the game's shadow cascades to the sky's sun
after the game had built them. The game still chose what casts shadows, and fitted its cascades, for its own fixed
sun (on Wake Island its shadows fall from 63.4 degrees up, its light from 35); turned, a cascade's box no longer
held every caster that throws a shadow into view, so buildings' shadows came and went with the camera. D218 had
already faded the turn out from high up for the same reason.

**Found** (research, runs sun_find1, sun_early1 to 3, sun_view1, sun_src1 and 2): the game's shadow setup,
sub_825E1DB0 (once a frame), hands sub_82620D20 (r3 the rotation it builds, r4 a vector; called from 0x825E1FE0) its
shadow ray, from the sun to the ground: on Wake Island 0.4454 -0.8945 -0.0391. Written over before the call, the game
builds its cascades and culls its casters for that ray (sun_src2: with the ray turned by 90 degrees, a pole's shadow
on the airfield went away, and the game's own cascade transforms followed). Writing the game's light direction or the
12 KB block it is copied from (0x20010150, by 825EE600) changed nothing.

**What:**
- `bf/src/shadow_sun.cpp` hooks sub_82620D20; called from the shadow setup (the link register inside 0x825E1DB0 to
  0x825E2830), it reads the game's ray and writes the one `native_x_renderer::ShadowSourceDirection` gives back,
  logged once ("BF shadows: the game's shadow ray ... given the sky's sun's ... (D239)").
- `ShadowSourceDirection` (draw.cpp) keeps D181's rule for where the shadows fall: the game's shadow direction moved
  by the sky's sun's change from the game's own light (its elevation held between 40 and 75 degrees, steeper as the sun
  stands high), the azimuth by the same difference. The sky's sun and the game's light come from the last frame's
  LatchShadowTurn; with no day cycle (the light not turned), no game light seen yet, or
  `native_x_renderer_sun_source` false, it returns false and the game's ray is left alone.
- LatchShadowTurn turns nothing while the source was fed in the last 500 ms; the game's cascades already fall from
  the sky's sun, and the turn's direction is set to it for the receivers' light. When the feed stops (a dedicated
  server has no renderer; the cvar off), D181's turn with D205's pivot and D218's fade comes back as before.
- `native_x_renderer_sun_source` (default true): off, the old turn (for comparison).

**Tested:** Wake Island at dusk, a camera gliding on foot height through the airfield and along the cliffs, the
source on and off (runs sun_glide_true, sun_glide_false): the hook fed every frame after the first (the turn's own
log never came again), the game's 63.4 degree ray replaced by the sky's sun's 40 degree one.
All four maps at 8:48, three parked views 250 to 500 m up, the source on and off (runs ss_true_*, ss_false_*): fed on
every map (the sky's sun's ray 75 degrees up), no shapes on the sea, the two alike within 0.2% of the pixels (from up
there a building's shadow is a few pixels). The Iwo Jima run with the source off lost its
device on a swap chain's re-creation after a 10 s stall (the old path, not this change). Not measured: the flicker
frame by frame on foot (the user's test); a real plane in flight; night and storms.

## D238 — The horizon: no grey wall; the sea fades into the sky, the far clouds and the far world in the haze (2026-11-24, Claude for Battlefield 1943 Reborn)

The user, 24 Nov 2026, with a screenshot of a hard line between the sea and the sky: "We really need to make our
horizon blend neatly like the SDK renderer does, but for all times of day"; then "Any way we can get rid of the grey
wall at the horizon, so we can make it look like infinite sea against the color of the sky like the original skybox
does with haze?" and "I noticed the distant fog only fills the skybox, it does not actually fill the level's world".

**Why:** three things stood at the horizon. The far cloud deck, tens of kilometres off and squeezed into a band just
over the horizon, was drawn whole: a flat grey-lavender sheet on the pale far sea (run hz2_on_1050, zoomed). The sky's
lowest colour was the haze's grey (fog_color) at any weather. And with the fog setting at 0 nothing hazed anything: the
sea the deck sees is mostly within a kilometre or two (its drawn edge met the sky in a line), and the far world kept
its colours under a hazy sky. With no fog, ground fog over 0, weather or reflections the fog pass did not run at all.

**What:**
- The sky pass: a cloud fades into the sky behind it by its distance, taken as 1.5 km up (exp(-d / 30 km): a tenth
  gone overhead, half 20 km off, all of it at the horizon).
- `low_haze()` (the shared sky code): the sky at and under the horizon is the horizon's own colour times 1.06 on a
  clear day, the haze's grey only as there is haze (1 - exp(-1500 fog_a.x): the fog setting, the weather's mist).
- The fog pass, at its end (`native_x_renderer_horizon_haze`, default true; bit 128 of the fog parts off): everything
  drawn is hazed toward the sky as the sky pass draws it just over the horizon that way (`low_haze` with the sun's and
  the moon's glow), by haze_depth with 1/6000 per metre from 400 m and a 1200 m scale height (a tenth at 1 km, a
  quarter at 2, half at 5; less up high). The sea's last degree under the horizon (what lies on its plane, or nothing
  past its draws: not islands or ships) fades into that sky whatever its distance, as a skybox's painted haze.
- Past the sea's draws, under the horizon, the colour of the farthest sea drawn below on the screen (steps toward
  where the view looks lower, then halved; only when what is found lies on the sea's plane), with the pass's own fog
  and rain over it: the sea goes on to the horizon. The scene copy (D190's) is made for it whenever the pass runs.
- `FogWanted` is true with the haze on, so the pass runs with every setting at 0 (its cost: one full-view pass and the
  scene copy, which ran only with fog or ground fog set before).

**Tested:** Wake Island's carrier deck, the user's settings (fog 0, ground fog 10%), 10:30, 17:30 and 21:00 (runs
hz2_off_*, hz3_on_*, hz4_on_*). Before: the grey sheet of far cloud over a bright cyan sea at 10:30, the sea's edge a
line at every hour. After: the far clouds melt into the haze, the sea fades into the sky's colour at the horizon
(lavender-blue by day, lilac into pink at dusk, dark blue at night), the far carrier and the island keep their colours.
Not seen: from a plane in flight; in a storm; the cost measured.

## D240 — The sea reflects the hour's sky without the renderer's reflections too (2026-11-24, Claude for Battlefield 1943 Reborn)

The user, 24 Nov 2026: "Can the water's color be adjusted to reflect time of day as well, without needing to have
SSR enabled (which is a bit bugged)?" and "Is it possible to replace the waters cube map with one of our sky, like
SSR one does?"

**Why:** without `native_x_renderer_reflections` every map's sea shaders reflected the game's day sky cube map (their
outdoorLightReflectionEnvmap, and Wake Island's and Coral Sea's own at fetch 6), only dimmed for the hour (D182): a
blue day's sky in the sea at dusk and at night. D190's small cube of the renderer's sky stood in for it only with the
reflections on.

**What:** `native_x_renderer_sea_sky` (default true): with the renderer's sky on, SeaSkyView gives the sea's shaders
D190's cube (the hour's zenith and horizon as shown) for every cube map they take, not only fetch 6, and the sea's
specular w is no longer dimmed for the hour but taken at 0.6 (whole, the sea by day was a pale glare of the horizon's
colour: run hz_after_1050). With the reflections on, as before (none of the cube in the sea; the fog pass reflects).

**Tested:** Wake Island's carrier deck at 10:30, 17:30 and 21:00, the same build with it on and off (runs hz_after_*,
hz_nosea_*): at dusk the sea lilac under the pink sky (dark teal without), at night darker (8 19 41 to 1 7 26 in the
near sea), by day at 0.6 a lighter blue (runs hz4_on_*). Not seen: the other maps' seas; SSR on (unchanged code path).

## D241 — The ground's relief lit by a lower sun while the sky's stands high: the tyre ruts back (2026-11-24, Claude for Battlefield 1943 Reborn)

**Why:** the report (relayed): "certain terrain texture layers, like the vehicle tracks, disappear from the player
camera view"; the user: Wake Island, flags E and C. RenderDoc on both renderers at flag C (runs rd_ours, rd6_sdk): the
SDK's frame and ours draw the ground there with byte-identical textures, the same samplers and the same pixel shader
constants but one, the light direction (c1: the SDK the game's -0.816 0.574 0.072, 35 degrees up; ours the sky's sun,
65 degrees up at 10:30, turned 82.5 degrees round). The ruts are relief in the ground's normal maps, drawn for the
game's fixed low sun; under a high sun a rut's two sides are lit almost alike (their difference goes with the cosine of
the sun's height, against the flat ground's sine: a third as visible at 65 degrees as at 35). With the game's light
(NXR_GAME_LIGHT=1, a dev switch kept local) ours showed the same crisp ruts (gamelight3_none). Ruled out on the way: missing draws
(the CPU-built track strips pass their depth test, 40 of 40 vertices; the terrain's dirt map at 0xd400000 is black in
the SDK too, loading screen leftovers in a 64 texel strip), texture content, samplers, the dynamic strips (where bots
drove). A hosted custom match forces day and night on (game_menu.cpp ApplyServerLook), whatever the cvar says.

**What:**
- `tools/ground_shaders.py` reads the game's shader constant tables (as light_constants.py) and writes
  `bf_ground.inc`: the pixel shaders that draw the ground, the terrain's (they sample `sampler_heightmap`, 240) and the
  ground patches' (generic `sampler_Texture<n>` samplers and `sampler_OcclusionTexture` only, 90; no shader samples the
  occlusion map with named samplers, so props are not taken). 330 of 1,098.
- ApplyOutdoorLight: for those shaders, while the sky's sun stands higher than the game's own light (its elevation,
  25 to 50 degrees, else 35), the light direction is lowered toward it at the sun's own azimuth, halfway by default
  (`native_x_renderer_ground_relief_strength`, 0.5), and the key colour made stronger by sin(true) / sin(lowered), so
  flat ground is lit as before. `native_x_renderer_ground_relief` (default true) turns it off. Shadows stay the sky's.
- Halfway, not the whole way: the whole way the terrain's own slopes turned from the sun went much darker (a 30 degree
  slope's light a quarter of the sky's at 65 degrees; Guadalcanal's cliff black-streaked, Iwo Jima's beach slope 10%
  darker, runs rel1_true, rel2_true). Halfway: the ruts' contrast 1.8 times the sky's sun's, such slopes about 30% less.

**Tested:** flag C at 10:50 (relief_false_none, relief_true_none, relh_none): the ruts visible again, the ground's mean
brightness 188.3 off and 187.2 on. Guadalcanal and Iwo Jima tours at 10:50, off, half, whole (rel1_*, rel2_*, relh1,
relh2): the lower half of the view within 5% at half. Not tested: Coral Sea, mornings and evenings (below the game's
height nothing changes), a match played through.

## D242 — The horizon: a pale blue sky over the sea, and the sea's fade found by the level's water (2026-11-24, Claude for Battlefield 1943 Reborn)

**Why:** the user, 24 Nov 2026: "can we get rid of the grey band on the sky box at the horizon, so the water and the sky
blend better at that point?", then with a screenshot: "this greyness behind the clouds on the sky. The sky transitions
from bottom to top, grey to blue". Two things after D238: the sky palette's clear-day horizon colour was a grey
lavender (0.6 0.66 0.76, its red as its green, darker than the blue above it) filling the sky's lowest 17 degrees; and
D238's fade of the sea's last degree into that sky did not run where the eye is low. It found the sea by the distance to
the sea's plane (97% of it), and the sea's height was learned as the lowest flat ground in sight: Iwo Jima 4.4 m for the
game's water drawn at 5.3 (an eye a few metres up then sees its sea at 70% of the plane's distance), Wake Island's
flag C 233.4 m (the airfield; the sea is at 215). The bright sea ran up to a duller sky in a hard line (run rel2_false).

**What** (sky.cpp):
- The sea's height is the level's water when it is known (`native_grass_water_level`, from the level's water mesh as
  it loads; else learned as before): Wake Island 215.00 m, Iwo Jima 5.30.
- The fog pass finds the sea by height: within 1 m of that level (its waves), in the fade and in the search for the
  farthest sea past the sea's draws (3 m faded a carrier's waterline into a pale seam, run hzi_sun). The fade covers the
  sea's last two degrees under the horizon (one was a few pixels).
- kHorizon, a clear day's horizon, 0.6 0.75 0.96: a pale sky blue, red under green (dusk and night mix their own over
  it as before).

**Tested:** Iwo Jima's beach at 10:50 and dusk (hzi_sun, hzi2, hzi_dusk, hzi3_dusk): the hard line gone, the sea fading
up into the sky. Wake Island's beach at 10:50 (hzw_sun, hzw2, hzw3): the sky from pale to deep blue, no grey behind the
far clouds, the sea's far band meeting it in a pale glow. Not seen: SSR on, a storm, from a plane, Guadalcanal and
Coral Sea.

## D243 — The terrain's tyre-track pass is not the sky's haze; the verify report lists what no draw call stands for (2026-11-24, Claude for Battlefield 1943 Reborn)

**Why:** the user, 24 Nov 2026: "The tire marks aren't flattening, they pop in and out of existence", then side by side
at Wake Island's flag A: "all the tire tracks are perfect in the SDK renderer", and "I now fear our renderer is missing
far more things than just this issue". RenderDoc at flag A (runs rdA_sdk, rdA_ours): the SDK draws eight blended
terrain draws over the sand (index counts 180 to 3249, premultiplied alpha, no depth write) that ours never drew.

**The audit** (`native_x_renderer_verify`, observer.cpp): its report now groups, by vertex and pixel shader, index
count and targets, the draw packets the GPU ran that no Direct3D draw call stands for, and the recorded command
buffers' draws the tracker did not replay. At flag A over two frames (run audA2): 806 of 874 ring draw packets matched
to a draw call, all with the rebuilt state; the 68 others the library's own rectangles (prim 8, 3 vertices: clears,
resolves, the bloom chain's steps); the recorded buffers' 1008 not replayed all single points with a depth-only pixel
shader. No class of geometry is lost between the game and the renderer.

**The cause:** the own sky's rule (D179) skipped pixel shader 0365AC9437C0240E as "a haze of 588 and 180 vertices
blended over what is there", seen before the dome in the sky's runs. The game's constant table says otherwise: the
outdoor light, the sun's shadow map, `sampler_heightmap`, six textures (the tyre tread among them) and the dynamic
mask's tile and indirection maps: the terrain's tyre-track pass, the eight draws. 38DEA3BE39271907, "Guadalcanal's
haze", samples three generic textures as Wake Island's track strips do (617653D17C0D6DD5).

**What:** neither is skipped any more (draw.cpp); the dome's shaders still are. Ours now draws the eight with the
SDK's index counts (run dlA).

**Also found** (not changed here): at flag A the tracks seen in the SDK are mostly the ground's own relief under the
game's low sun (D241): with the game's light (NXR_GAME_LIGHT, a dev switch kept local) ours shows them; with the day
and night's sun at 10:50, even with D241 at half, the sand road is plain; at 16:00 they show faintly.

**Tested:** flag A before and after (runs popA, popA_d243, dlA, dlA_sdk); Guadalcanal at night (gnight_d243): the
renderer's night sky, no daylit haze back. Not tested: Iwo Jima and Coral Sea's sky with the change.

## D244 — The ground lit from the game's own sun while the day and night's stands higher (2026-11-24, Claude for Battlefield 1943 Reborn)

**Why:** after D241 (the relief lit by a lower sun, half way) the tyre tracks at Wake Island's flags A and C still
showed faintly at 10:30; with the game's own light (NXR_GAME_LIGHT, a dev switch) they showed as in the SDK. The user
was shown half (D241), "full height" (the sun lowered the whole way: black blotches over the sand at 10:30) and the
game's sun at 8:48, 10:30, 16:00 and dusk at both flags (runs cmp_*), and chose "Yes, game's sun"; then, shown that it
darkens whole hills on Iwo Jima and Coral Sea (a beach hill from 134 to 71, a slope to 53: runs gs2_game, gs3_game) and
a half way version for the terrain: "I liked the hills dark, it looks closer to the SDK renderer".

**What** (draw.cpp, ApplyOutdoorLight): for the ground's shaders (bf_ground.inc, now with each one's kind: 1 the
terrain's, 2 a ground patch's; tools/ground_shaders.py) and the tyre-track passes (D243: 0365AC9437C0240E and
0A7847F161DA00A7, the same samplers on another level), while the sky's sun stands higher than the game's light, the
light comes from the game's own direction, blended in over 15 degrees above its height (dawn and dusk keep the hour's),
the key colour raised by as much as flat ground lost (sin of the hour's height over the new one's), so flat ground is as
bright as the hour. `native_x_renderer_ground_relief_game_sun` (default true; off: D241's half way lowering),
`native_x_renderer_ground_relief_game_sun_weight` (patches and tracks, 1) and `..._terrain_weight` (the terrain, 1;
0.5 kept hills within a tenth, runs gs2_w05, gs3_w05). Shadows and everything else keep the sky's sun.

**Tested:** flags A and C at four hours (cmp_*), the four maps' tours at 10:30. Not tested: a match played through.

## D245 — The sea lit as the game lights it, as bright as the hour (2026-11-24, Claude for Battlefield 1943 Reborn)

**Why:** the user: "Now we just need to tune the water color closer to the SDK look at the same time of day too", then
"I thought we already agreed on the game's sun for this". Wake Island's open sea from flag A at 10:30, parked the same in
both renderers (runs sea4_w_sdk, sea4_w_ours): mid sea SDK (18, 76, 109), ours (71, 125, 191); just under the horizon
(36, 135, 165) and (158, 235, 254). With the game's painted sky reflected instead of the hour's (sea_sky off) ours was
(49, 103, 152); the horizon haze changed nothing (sea2_seasky0, sea2_haze0). Iwo Jima's sea draws green in the SDK (its
bug), so Wake is the reference.

**What** (draw.cpp): `native_x_renderer_sea_game_light` (default true): the sea's shaders keep the game's own light
direction and light colours (sky, ground, sun, its specular), all scaled by one factor, the hour's sun's brightness (its
key colour's luminance, at most 1), and reflect the game's own painted sky dimmed for the hour (SkyReflectionLeft;
SeaSkyView no longer replaces it).

**Tested:** the same view at 10:30 (run seaG_sun): mid sea (44, 89, 131), near (18, 74, 120) against the SDK's (9, 65,
105); dusk deep navy, night dark (seaG_dusk, seaG_night). Not tested: the other maps' seas against the SDK (Iwo Jima's is
broken there), SSR on.

## D246 — The sea's fade into the sky only far out (2026-11-24, Claude for Battlefield 1943 Reborn)

**Why:** the user, with screenshots of a beach from eye height: "water just now has this weird translucent layer over
objects and terrain at eye-level". D242 took as sea, for the fade of its last two degrees into the sky, whatever lies
within 1 m of the water's level; from near that level the band reaches tens of metres out, and the rocks, the beach and
the obstacles at the waterline took the sky's colour.

**What** (sky.cpp): that fade grows from 300 m to 700 m away; past the sea's draws (nothing drawn) as before.

**Tested:** Iwo Jima's beach at eye height (run eyelvl2): the shore's objects in their own colours, the far sea still
fading into the sky.

## D247 — Per-draw work that belongs to the frame: a getenv on every lit draw, the outdoor light at every draw (2026-11-24, Claude for Battlefield 1943 Reborn)

**Why:** the user, 24 Nov 2026: "Any improvements we can make to NativeXRenderer? Make it more accurate, efficient,
etc?" A profile of the renderer's thread (runs nxrp_day and nxrp_storm, Wake Island's camera tour: a sampler of whole
stacks from outside the game, `.handoff/scripts/tsample`) showed that in busy views the thread, not the GPU, sets the
frame rate: it worked 4.5 to 7.5 ms a frame, the frame interval followed it, the GPU was at 3 to 4 ms. A quarter of it
was two kinds of work done for every draw that change only once a frame:
- **14 to 15 %:** `getenv("NXR_SHADOW_LOG")` in ApplyOutdoorLight (D239's research line), at every lit draw's pixel
  stage: its "said once" flag only turns true when the variable is set, so while unset the C runtime walked the
  environment block every time. The same at the lamp models' `NXR_MODEL_LAMP_DUMP` (night).
- **10.6 % by day, 14.2 % in a storm:** ExecuteDraw's outdoor light: `World()` (a clock, a mutex, the storm), `SkyLightAt`
  twice, `SkyFlashAt` (the storm again, a mutex), `SkyNoonAzimuth`, the sea sky's colours and a dozen settings, the same
  for every draw of the frame.

**What:** both environment variables read once (draw.cpp); the outdoor light and the world's flags worked out at the
frame's first draw and kept for its others (renderer.cpp, keyed on the frames presented). A lightning flash now lights
a whole frame alike instead of changing within it.

**Tested:** the same two tours before and after (runs nxrp2_day, nxrp2_storm), the windows of 1,000 draws or more: the
renderer's thread 4.41 → 2.80 ms a frame by day and 4.38 → 2.95 in the storm (median); the frame interval 5.10 → 4.19
and 4.95 → 4.17 ms (now at the 240 FPS cap in most windows), at 2,000 draws or more 5.84 → 4.53 and 6.63 → 4.62 ms; the
GPU unchanged (3.0, 3.5 ms); the thread idle 17 → 35 % of the time. The storm's light, rain and mist as before
(shot_57). What is left: `.handoff/NXR-IMPROVEMENTS.md` (MoveAttached's matrix search per draw, 5 %; the recorder's spin).

## D248 — D154's attached models: a world matrix tested against the recorded cameras first (2026-11-24, Claude for Battlefield 1943 Reborn)

**Why:** after D247, MoveAttached (D154) was 5 to 6 % of the renderer's thread in Wake Island's camera tour (runs
nxrp2_day, nxrp2_storm). For every model whose eye is within 25 m, every three consecutive registers that are rigid,
orthogonal rows (a soldier's bones, every one) got a 4 x 4 inverse (Mat4dInvert, 0.7 to 0.9 %) and FindAttachedMove,
which takes a lock and reads the clock (0.4 %), and which takes no record whose frame camera is more than 5 mm from the
eye those rows put in the world: nearly all of them.

**What** (draw.cpp): the 16 records' frame cameras copied once per call (AttachedPositions, one lock), and each triplet's
eye in the world tested against them first, with FindAttachedMove's own arithmetic and 5 mm; only a triplet near one goes
on to the rigid rows' checks, the inverse and FindAttachedMove, which still decides as before. The same models move.

**Tested:** the day tour again (run nxrp3_day): Mat4dInvert and FindAttachedMove gone from the thread's profile;
MoveAttached 6.0 → 5.4 % of its samples, within a single run's noise: the gain is about 1 to 1.5 % of the thread
(0.04 ms a frame). Not tested in a vehicle: the tank run's soldier never got in (run nxrp3_tank, on foot, 0 moved as
always on foot). What is left of MoveAttached is the search for the projection block (CameraVote::FindMatrix, about
2 %); a cache of which shaders have none would skip it, but could miss a frame of a vehicle's sight: not done.

## D249 — What our own rules leave out of the game's frame, logged and checked against the game's constant tables (2026-11-24, Claude for Battlefield 1943 Reborn)

**Why:** D243 found that the own sky's rule had taken every map's tyre-track pass, and the user wrote "I now fear our
renderer is missing far more things than just this issue". Draws lost between the game and the renderer are audited
since D243 (the verify report); the draws the renderer itself leaves out on purpose were not.

**What:**
- draw.cpp: every distinct draw of the game's that one of our rules leaves out is logged once ("rule skip: <rule>: PS,
  VS, vertices, target, draws after the sky dome"; at most 300 a run, in every log, players' included). The rules are
  the own sky's (the dome, Wake Island's tint and painted clouds, the other maps', D201's sky shapes) and the crosshair's
  while aiming. Nothing else drops a game draw on purpose (the other early returns are failures, already counted).
- tools/skip_audit.py: those lines (or given hashes) with each pixel shader's samplers and constants from the game's
  own constant tables, flagged where they are not the sky's (the heightmap, the sun's shadow map or direction, surface
  maps). D243's tyre-track shader comes out flagged; the other D243 shader as generic samplers to look at.
- NXR_E20C_LOG and NXR_SKY_SPRITES_NEAR (diagnosis aids, below).

**Found:** the four maps by day and by night, on foot, firing and turning (runs skipa_m0 to m3, 8 runs): 11 pixel
shaders left out, all sky-like by their tables: the tints and painted clouds (a distortion map, two cloud textures,
`time`), the dome's two passes (the depth buffer, the half size environment), and D201's catch-all took one variant
(C7149298A2818B64, the tints' samplers). E20CB2628975A77D, the painted clouds' sprites, is also the effects' particle
shader (D195), so it was checked further: at the output's size its draws come 2 to 64 draws after the dome, every one
with one texture, the map's sky sheet (format 59, 2048 x 1024; runs skipe_m0 to m3), and drawn they change nothing
seen (skipe_m0 against skipc_all); the effects draw with it into the half size effects layer, which the rule leaves. A
muzzle flash seen in one run with the sprites partly drawn and not in the other was the shot's timing, not the rule.
No rule takes more than the sky; nothing changed.

**Kept for later:** the audit lines in players' logs show a new map variant or a rule that grew, by name.

## D250 — The recorder woken only while it sleeps (2026-11-24, Claude for Battlefield 1943 Reborn)

**Why:** item 4 of the profile of 24 Nov 2026 (`.handoff/NXR-IMPROVEMENTS.md`): the recorder's thread (the
DiligentCore calls, recorder.cpp) was most of a core busy, about half of it its spin before it sleeps (2048 pauses,
cpu_cores.h), and the renderer's thread, which sets the frame rate in busy views, woke it at every publish
(`published_.notify_one()`, a call into the system, about 1 % of its samples) whether it slept or spun.

**What:** Publish wakes it only while it sleeps (`consumer_sleeping_`, sequentially consistent with its sleep, as the
command queue's consumer does). The spin's length stays 2048; `NXR_RECORDER_SPINS=<n>` (a diagnosis aid) sets it.

**Tested:** Wake Island's storm tour on the same build (runs nxrp4_s2048, nxrp4_s256), the windows of 1,000 draws or
more: the wake gone from the renderer's thread's profile; its time a frame 2.94 ms median (2.95 before, nxrp2_storm),
the frame interval 4.19 ms (the 240 FPS cap). With 256 pauses the recorder was 45 % busy against 59 %, but the
renderer's thread waited for it longer (Recorder::Drain 4.7 % of its samples against 3.6 %), the frames the same only
because they were at the cap: the longer spin stays, the cheaper side for the thread that sets the frame rate.

## D251 — The storm's shake at the picture's own size (2026-11-24, Claude for Battlefield 1943 Reborn)

**Why:** the user, 24 Nov 2026: "The screen shake from the storm wind should just be a screen shake, but it looks like
it's warping the perspective too, like lens distortion is being turned on and off constantly." To keep its corners
filled, D193's shake drew the picture larger by a margin that followed the shake's strength (`sway_`, which follows the
storm at the eye and the wind's gusts, and goes when cover or a vehicle takes it), and D222's strike shove added up to
about 5.5 % more, gone within a second. So the view zoomed in and out under the shake: the field of view breathing.

**What:** the lens pass shakes the picture (moved, turned about its middle) at its own size; the few pixels at an edge
the shake uncovers (about 5 at 3440x1440 in the wind, some 25 at a close strike's peak, under D194's blurred edges) are
the picture's own, mirrored. `drift.x` only says the shake is on.

**Tested:** Wake Island in a storm on foot, the shake forced on (run shake_d251b, `NXR_SHAKE_TEST`, a local aid):
the shake on, the lens pass's shader built, no seam at the picture's edges at 3440x1440 under the blurred edges.
The breathing itself is gone by construction (nothing scales the picture any more); not yet seen by the user.

## D252 — Planar sea reflections: the world drawn again, mirrored in the water (2026-11-26, Claude for Battlefield 1943 Reborn)

The user, 19 Nov 2026: Enhancements > Lighting > Reflections should be "Off / SSR / Planar"; 25 Nov 2026: "Do this
task here", then "Any optimizations we can do to get planar to run better?", "Do 1 and 2" (every other frame; the
terrain under the water left out), "Make sure we're rendering the shoreline wash effects from the game", and "Finish
planar, make it performant".

**What:**
- The main view's draws that write depth (but the first person weapon's, the sea's, and the game's depth pre-pass
  without a pixel shader) are kept as the glass and sea masks keep theirs: pipeline state, viewport, scissor, index
  buffer, both stages' packed float constants (lit by ApplyOutdoorLight as the real draw, so the mirror has the hour's
  and the night's light), the system, bool/loop and fetch constants, and the draw's texture and sampler objects (a
  binding set of the mirrored pipeline).
- Replayed just before the fog's pass into a target of half the view's size (`native_x_renderer_planar_scale` 0.5;
  RGBA16F with a D32 depth target of its own), each draw's world-view-projection (the perspective block whose field of
  view is the camera's: a shader that indexes its constants carries leftover blocks, c0 to c3 with 2.51 against the
  scene's 1.29) times R = VPq M VP_p^-1: M the reflection in the sea's plane, VP the terrain's (one camera for every
  depth partition; the partition's own depth row a w + b read from the draw's matrix, so the near partition, which has
  no terrain, is mirrored too). The triangles' winding turned round.
- What is under the water is clipped by an oblique near plane, not a user clip plane (NativeXRenderer's device has no
  clip distances: DiligentCore enables none): the depth row is w - k h (k 0.05, h the real point's height over the
  water), so under the water z > w; larger is nearer, as the view's depth, and every partition draws into one range.
- The fog's pass takes the reflection in place of the SSR march: at the pixel moved by the waves' tilt (the rippled
  reflected ray's point 20 m + 0.2 x the distance along it, its height turned down), where the reflection's depth holds
  something, else the sky's palette that way. Fresnel, the foam and the D204 far fade as SSR's.
- In the shallows the reflection fades out by the water's depth from the terrain's heightmap when the world's depth
  under the sea is not copied (it is only for the night's pass): by day the reflection lay over the game's own water at
  the waterline, now kept within 0.1 to 0.6 m of water as SSR keeps it at night.
- The setting: `native_x_renderer_reflection_mode` (0 off, 1 SSR, 2 planar; -1 as `native_x_renderer_reflections`
  says), Reflections: Off / SSR / Planar.
- Its cost (the GPU's is the draws' own, about 1.7 us each here; the vertices and pixels hardly count: runs pl_max0 to
  pl_max60, half and full size alike): drawn every other frame (`native_x_renderer_planar_every`, default 2, the last
  one shown between); terrain patches wholly a metre or more under the water left out (each patch's corner and step
  from c20 to c22, the heightmap's highest point per 16 x 16 texel cell read once from the game's memory, 6 ms for
  2048 x 2048); small models (under 2,000 vertices, with a world matrix of their own) whose mirrored origin is behind
  the eye, far off the picture or beyond 600 m left out (`native_x_renderer_planar_cull`); none at all when the game drew
  none of its sea this frame.
- Measuring aids: NXR_PLANAR_DEBUG (1 the replay unmirrored, 2 mirrored, over the whole view), NXR_PLANAR_LOG,
  NXR_PLANAR_MAX, NXR_PLANAR_LIST, NXR_TERRAIN_PATCH_LOG, NXR_DRAW_TWICE.

**Measured** (Wake Island by day, five fixed camera stops, 3440x1440, RX 7900 XTX, medians of the windows with 500
draws or more): SSR GPU 3.08 ms, frame 4.17 ms (the 240 FPS cap). Planar first: 4.64 and 5.19; every frame with the
underwater patches out 4.76 and 5.38; every other frame 4.06 and 4.64; with the culling 3.91 and 4.52 (runs pl_c1,
pl_c2, pl_c3, pl_c4). The island's and the carriers' reflections the same before and after the savings (runs pl_look,
pl_look2).

**Tested:** Wake Island from a few metres over the sea toward the shore and the American carrier (Off, SSR, Planar from
one place each: the shore mirrored as a picture, not SSR's streaks), on the carrier's deck, at the beach in the
shallows (the game's water kept at the waterline), at dusk and at night (the mirror as dark as the world). Not tested:
the other maps, a plane in flight, storms, MSAA. Not found: the beach's white wash (the user's reference shot: a foam
band over the water), which is missing with any reflection setting; looked into next.

## D254 — Weather off: the game's own sky for day and night; the weather system only when a match turns it on (2026-11-28, Claude for Battlefield 1943 Reborn)

The user, 28 Nov 2026: "for custom match and servers; if Weather is off, then the default game sky should be used for
day and night (graded for proper night-time). Weather system and volumetric clouds only used if Weather is enabled in
custom match/server settings." Asked what Weather off means on a server (which had a day and night switch and a weather
strength, but no Weather switch): "A new Weather switch".

- **What is Weather.** A Custom Match: its Weather and time switch (the Weather tab), or an event (Night Ops,
  Daybreak). A server: the new Weather switch in the server manager (`weather_system`, gmhost.py `--weather-system`),
  or an event. Off on a new server. A server saved before the switch gets it on when its weather, fog or ground fog
  was over 0, so a server set up with rain keeps it.
- **What the host says.** The world answer's flags byte gains 64 (this host says whether the weather system is on)
  and 32 (it is), from gm_host.cpp (`bf_world_weather_system`) and gmhost.py alike. With Weather off the host's world
  has no rain, fog, ground fog or dynamic storm, also after an admin's change in game.
- **The look.** A joined match whose host says so decides the server's look (game_menu.cpp `TickServerLook`) for as
  long as it is joined. Weather on: all six look settings on, as an event's look. Weather off: Volumetric clouds, Rain on
  the lens and Wet weapon and hands off; Day and night, Lamp glow and Lens flare follow the match's day and night
  switch. With the renderer's sky off and day and night on, the game's painted sky is tinted by the hour
  (`light.game_sky`, D182), the night's grading. Weather off and day and night off is the released game's look, as
  regular servers had. The match this PC hosts is decided at the join; any other on its host's first answer after it.
  A host from before this (no flag 64) keeps the old rule: a player hosted match on, an event on, other servers off.
- **Not changed:** the Custom Match's Weather tab still has the time of day, so a Custom Match with Weather off plays at
  the map's afternoon (16:00), with the game's own sky; night with Weather off is only on a server.

## D255 — The weapon while aiming: the Thompson's skin smoke beside its receiver; a rifle's narrow view still the weapon's (2026-11-28, Claude for Battlefield 1943 Reborn)

**Why:** the user, 28 Nov 2026: "the new skin is disappearing when a player ADSs with iron sights ... it should only keep
the iron sight view clear with the rest of the animation showing around the weapon fine when ADS", checked "for both
Type 100 and Thompson"; "The Wetness shader on the weapons when it's raining should also still show when zooming or
ADSing for all weapons too" (the scoped rifle's optic fills the screen: left out); then, of the first attempt's shots:
"The Type 100 was perfect. The Thompson just needed to show it's smoke on the side like the Type-100".

**Found** (NXR_SKIN_AIM_LOG=1, a new diagnosis aid: while aiming, each distinct depth range, projection and outcome of
ClassifyWeapon's draws, and twice a second what the skin's overlay goes by):
- The skin's cloud: the overlay had the gun every frame, its barrel's line 0.31 to 1.03 m straight down the middle of the
  view (run aim_smg). Seen end on, the Thompson's wide receiver at the eye hides every puff within a few centimetres of
  the gun, the whole cloud; the Type 100's narrower one leaves wisps showing at its sides (aim_jp).
- The wetness: the weapon's draws are told by their own projection in the nearest depth range (D217), which took
  fields of view of 19 degrees or more only. Aimed in, the M1 Garand's view narrows to 10 to 12 degrees (py 9 to 11.4,
  run aim_rifle): its draws were no longer the weapon's, and its wetness went. The SMGs' (py up to 4.5) stayed.

**What:**
- weapon_skin.cpp `Around`: while aiming (gun_up.w), the Thompson's storm only (mg.x 0) stands out to just past its
  receiver's outline as seen, 8.5 cm a metre of its distance from the eye. The Type 100 as before.
- weapon_skin.cpp `GunPoints`: only the muzzle need be in front of the eye (the back of the receiver may reach it).
- draw.cpp `ClassifyWeapon`: the nearest range's projections down to 6 degrees (py under 20); the world's stay at 19.

**Tested** (dev build, Wake Island by day in rain, Iron Sight on, the skin worn; runs aim_smg, aim_jp, aim_rifle): the
Thompson aimed in with smoke both sides of its receiver and the sight picture clear; the Type 100 as before (the team
changed by BF_MENU_STEPS' changeteam); the Garand's draws the weapon's all the way in (py up to 11.4, none out of range,
its wet copy drawn with NXR_WET_TINT=99). The Garand's wetness was forced on for the check, not seen in real rain.
