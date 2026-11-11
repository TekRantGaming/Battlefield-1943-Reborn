# NativeXRenderer in Battlefield 1943

The map of the XDK Direct3D library linked into Battlefield 1943's `default.xex`, in the format of Guitar Hero:
Metallica's map (GHMetalRecomp's `docs/native-x-renderer-ghmetallica.md`), for NativeXRenderer
(`deps/native-x-renderer`). The hook lines are in `bf/src/native_x_renderer_bf1943_hooks.inc`.

**Status (2026-11-05):** static analysis only. Nothing here was run in the game. Every address below was read in the
generated code (`bf/generated/default`), indexed with NativeXRenderer's `tools/ppc_index.py`, and matched against GH
Metallica's map (same XDK, 2.0.7978) by the device offsets, packets, immediates, size, shape and callers. "Known" means
at least two independent pieces of evidence; "likely" means the role is clear but a detail is not confirmed.

**Main findings:**
- **The device layout is GH Metallica's exactly.** The draw's flush has the same six dirty masks at `+0` to `+40`,
  the same bit ranges, the same helpers' arguments and the same shadow offsets. Every field this work read (about
  80) sits at GH Metallica's offset. The device is 24064 bytes, as there. No difference found.
- **The resource objects are GH Metallica's too**: texture fetch constant `+28`, surface info `+24`/`+28`/`+32`,
  size `+36`, buffer base and size `+24`/`+28`, fence `+8`, query tile count `+148`.
- **The library code is the same, at other addresses.** Function sizes match GH Metallica's to one or two
  instructions (DrawIndexedVertices 275 against 274, DrawVertices 251 against 252, BeginVertices 293 against 294,
  DrawVerticesUP 18 against 18, SetRenderTarget 211 against 212, ClearF 74 against 74, SetPredication 92 against
  92, BeginTiling 222 against 220, EndTiling 300 against 302, SetVertexShader 115 against 116, SetPixelShader 111
  against 112).
- **Less of the library is linked.** No BeginIndexedVertices, no EndVertices function, no DrawIndexedVerticesUP, no
  Clear (D3DCOLOR form), no BeginZPass/EndZPass, no typed vertex or index buffer Lock/Unlock thunks. SetStreamSource
  is a short form without the stride.
- **The engine bypasses the D3D shadow for shaders and constants** (the XDK's GPU ownership calls), when a renderer
  option is set, which the M1 counts show is the case at run time: shaders through a GpuLoadShaders form
  (`sub_821802A8`, about 350 a frame) and constants through a GpuBeginShaderConstantF4 form (`sub_82180220`, about
  1,540 a frame), both writing packets straight into the command buffer outside every call NativeXRenderer has.
  **This is the main integration gap** (section 5).
- **The engine records D3D command buffers on command buffer devices** (a second kind of device created with type 2)
  and runs them on the main device with RunCommandBuffer. NativeXRenderer has no calls for these (section 5).
- **The 576 single vertex memexport draws a frame are SetShaderGPRAllocation's prebuilt buffer**: 24 one point
  draws, written once at device init, replayed by each of the 24 SetShaderGPRAllocation calls a frame. That explains
  M1's replayed indirect buffer (section 5).

## Contents

1. [The executable](#1-the-executable)
2. [The D3D entry points](#2-the-d3d-entry-points)
3. [The device layout (XDK 2.0.7978)](#3-the-device-layout-xdk-207978)
4. [Resource objects](#4-resource-objects)
5. [Engine behavior](#5-engine-behavior)
6. [Other library functions the engine calls](#6-other-library-functions-the-engine-calls)
7. [Open questions](#7-open-questions)

## 1. The executable

- XDK 2.0.7978, confirmed in the library: the device init `sub_8216C468` passes `0x1F2A0003` (`lis r3,7978`
  at `0x8216C49C`, `ori r3,r3,3` at `0x8216C4AC`) to VdInitializeEngines, as GH Metallica's `sub_821CFB18` does.
- **Layout.** The D3D library sits in `0x82160000` to about `0x821805F0`, with a few CRT functions mixed in by the
  linker (`sub_82160000` and `sub_8217B990` are vector constructor iterators, `sub_82166368` is `fabs`,
  `sub_821805F0` is `mftb`). From `0x821805F0` on the code is not D3D (math helpers). Roughly:
  - `0x82160310` to `0x82161120`: Swap, Synchronize, Present, the SET_BIN helper.
  - `0x82161120` to `0x82163A10`: the RT0 helper, the float viewport setter, the window scissor writer, then about
    90 render state setters and getters (`0x82161700` to `0x82163A00`, named by `rs_setters.py`).
  - `0x82163A10` to `0x82164580`: SetScissorRect, SetViewport, SetIndices, SetRenderTarget and its alias,
    SetDepthStencilSurface, GetRenderTarget, SetStreamSource (short form).
  - `0x82164580` to `0x82165F00`: the command buffer (reservation, allocator, NewSegment, the kick and its tail,
    fences, waits, callbacks).
  - `0x82165F00` to `0x8216E800`: device init, CreateDevice, the bindings reset, Present's internal draws, the
    gamma ramp and scaler.
  - `0x82170720` to `0x82172788`: XGraphics style texture helpers, the texture Lock and Unlock thunks.
  - `0x82172788` to `0x82173F78`: SetTexture, SetPredication, BeginTiling, EndTiling, queries.
  - `0x82176308` to `0x82177CF8`: AddRef/Release, command buffers (create, begin, end, run, predication).
  - `0x82178310` to `0x8217B898`: FlushCpuRange, the flush helpers, the program flush, constant setters,
    SetPixelShader, SetVertexShader, SetVertexDeclaration, SetShaderGPRAllocation.
  - `0x8217BDB8` to `0x8217F260`: the tiling replay, the query packets, the clear's internal draws, ClearF.
  - `0x8217F388` to `0x821800A8`: **the draws** (BeginVertices, DrawVerticesUP, DrawVertices, DrawIndexedVertices).
  - `0x821800A8` to `0x821805F0`: the GPU ownership forms (GpuDisownAll, GpuBeginShaderConstantF4, GpuLoadShaders).
- **The device pointer**: `Direct3D_CreateDevice` `sub_8216B890` is called with type 1 at `0x8259296C` (returned
  into `wrapper+44`), and the engine copies it to the global `[0x82AF7D00]` (`lwz r11,44(r31)`; `stw r11,32000(r27)`
  at `0x825929DC`/`0x825929E0`). The renderer passes `[[renderer+672]+0]` or `[wrapper+44]` as r3; the command
  buffer code reads the global (`lis r25,-32081`; `lwz r3,32000(r25)`).
- **Imports**: VdSwap thunk `0x829FA85C`, called only from Present (`0x82160B0C`).
- `bf/native-renderer/d3d_entry_points.txt` lists 149 functions in `0x82160000` to `0x82181000` with outside
  callers. Some are not D3D (the CRT bits above, everything from `0x821805F0`). The ones that matter are in the
  tables below.

## 2. The D3D entry points

Signatures are `(dev = r3, ...)`. "Engine" counts `bl`/`b` sites outside the library, in distinct functions;
"library" counts sites inside it. GHM is GH Metallica's address.

### Draws

Every draw runs the same flush inline (masks `+0` to `+40`, section 3), then writes `VGT_INDX_OFFSET` (`0x2102`,
`li r11,8450`) straight into the command buffer and a bin predicated `DRAW_INDX` (`0xC0032201` with an index
buffer, `0xC0012201` auto indexed); counts over 65535 are split. With tiling targets bound (`+10940 & 0x01`) the
draw is wrapped in `SET_BIN_MASK_LO`, `EVENT_WRITE_EXT` (26, an extents record in `+13248`) and `EVENT_WRITE` (25),
as in GH Metallica.

| Call | Battlefield | GHM | Evidence | Engine / library | Confidence |
|---|---|---|---|---|---|
| DrawIndexedVertices | `sub_8217FC58` | `sub_821C1E30` | Flush `0x8217FC68` to `0x8217FE90` (VS `0x4000` at `dev+1920` `0x8217FC98`, PS `0x4400` at `dev+6016` `0x8217FCB0`); `VGT_INDX_OFFSET` = r5 (`0x8217FEC0` to `0x8217FED0`); index buffer `[+12444]` (`0x8217FE98`), base `[ib+24]` (`0x8217FEFC`), 32 bit indices from `[ib+0]` bit 31 (`0x8217FF10`, `0x8217FF3C`); `DRAW_INDX` 4 dwords (`0x8217FF8C` to `0x8217FFA8`); 275 instructions (GHM 274). | 1 in 1 (`sub_82598298`) / 0 | known |
| DrawVertices | `sub_8217F868` | `sub_821C1A40` | Same flush; `VGT_INDX_OFFSET` = r5 (`0x8217FAC4` to `0x8217FAD4`); `DRAW_INDX` 2 dwords (`0x8217FB28` to `0x8217FB40`); 251 instructions (GHM 252). | 2 in 1 (`sub_82598298`) / 0 | known |
| DrawVerticesUP | `sub_8217F820` | `sub_821C1440` | BeginVertices (`0x8217F840`), `memcpy` `sub_827BE2D0` (`0x8217F854`), inlined end `[+48] = [+13444]` (`0x8217F858`/`0x8217F85C`); 18 instructions (GHM 18). | 6 in 6 (`sub_82598298`, `sub_8259A188`, `sub_8259EAE8`, `sub_8259F1E8`, `sub_8259F3C0`, `sub_8259F6E0`) / 0 | known |
| BeginVertices | `sub_8217F388` | `sub_821C0F98` | Stream 0 stride byte `+12536` set to `stride/4` (`0x8217F3A0`/`0x8217F3A4`) and restored (`0x8217F60C`); patch bit `+16 |= 0x80000` when a VS is bound and the stride differs from `+11840` (`0x8217F3B8` to `0x8217F3D4`); fetch slot 31 dirty bit cleared before the flush (`rldicr r11,r11,0,62`, `0x8217F3E0`); vertex memory from the allocator `sub_82164B58(dev, dwords, 16)` (`0x8217F630`); `VGT_INDX_OFFSET` = 0, fetch constant `0x000548BA` (`0x8217F65C`), three zeros to `0x5000`, `DRAW_INDX` after the write pointer; saves `+13444`/`+13448`/`+13456` (`0x8217F808` to `0x8217F810`); 293 instructions (GHM 294). | 0 / 1 (DrawVerticesUP only) | known |
| BeginIndexedVertices | none | `sub_821C1488` | No store to `+13452`/`+13460` anywhere, no second allocation form. | none | known absent |
| EndVertices | none | `sub_821C1430` | No `lwz rX,13444(dev); stw rX,48(dev)` function; the only `13444` uses in the whole image are BeginVertices' store and DrawVerticesUP's inlined end (`ppc_index.py grep`). | none | known absent |
| (DrawIndexedVerticesUP) | none | `sub_821C19A0` | Not linked (no BeginIndexedVertices to call). | none | known absent |

Other draw packet writers, none reachable as a draw call from the engine: `sub_8216B718` (`DRAW_INDX`, device
init), `sub_8216AB88`, `sub_8216D448`, `sub_8216DCC0`, `sub_821765B8`, `sub_82176710` (`DRAW_INDX_2`, Present's and
the scaler's internal draws), `sub_8217E0D0` (the clear's aliased surface draw, GHM `sub_821DAB50`), and
`sub_8217ABD0` (SetShaderGPRAllocation's prebuilt buffers, section 5).

### Render targets, clears, resolves, tiling, predication, viewport, scissor

| Call | Battlefield | GHM | Evidence | Engine / library | Confidence |
|---|---|---|---|---|---|
| SetRenderTarget | `sub_82163C20` | `sub_821C5308` | Slot `+12448 + 4 * index` (`addi r11,r4,3112`, `0x82163C2C`; `stwx` `0x82163C3C`); `[surface+28]` to RB_COLOR_INFO `+10372` (RT1 to 3 at `+10380...`, `addi r10,r11,2593`, `0x82163C54` to `0x82163C74`) with the dirty bit of `+16`; for RT0 the RT0 helper `sub_82161120` (RB_SURFACE_INFO `+10368` from `[surface+24]`, PA_SC_AA_CONFIG `+10684`, default scissor, then `b` SetViewport) (`0x82163DB8`); RB_COLOR_MASK `+10460` from the per RT masks `+11868` to `+11880`; 211 instructions (GHM 212). **Alias:** the engine calls `sub_82163C18` (`b 0x82163c20`). | 0 direct, **15 in 5 through the alias** (`sub_82586358`, `sub_82586450`, `sub_82586598`, `sub_825B9928`, `sub_8273CE98`) / 6 in 5 | known |
| SetDepthStencilSurface | `sub_82163F70` | `sub_821C5658` | `stw r4,12464(r3)` (`0x82163F88`); RT0 helper when no RT0 (`0x82163FA4`); RB_DEPTH_INFO `+10376` from `[surface+28]` (`0x82163FA8`/`0x82163FAC`); RB_HIZCONTROL `+10560` from `[surface+32]` (`0x82163FBC`/`0x82163FC0`); `+10942` bit 5 from the HiZ word (`0x82163FC8`); RB_DEPTHCONTROL `+10548` from `+11884`/`+11888`. | 5 in 5 / 5 in 4 | known |
| Clear | none | `sub_821DBE18` | Not linked: the engine's clear method `sub_82589790` (reached through a vtable) builds the D3D flags and tail calls ClearF with a float4 color, Z 1.0 or 0.0 and stencil 0 (`0x82589820`). No `vupkd3d128` color unpack calls ClearF. | none | known absent |
| ClearF | `sub_8217F260` | `sub_821DBCF0` | A NULL rect (`0x8217F270`) becomes the bound target's size `[surface+36]` (width minus one in bits 18 to 31, height minus one in bits 3 to 17, `0x8217F28C` to `0x8217F2AC`) or the tiled size `+13188`/`+13192` (`0x8217F35C`/`0x8217F360`), then the clear body `sub_8217EBB0` (`0x8217F370`; viewport `+12656`, scissor `+12684`, `IM_LOAD_IMMEDIATE`, the aliased surface fast clear `sub_8217E648` to `sub_8217E3B8` to `sub_8217E0D0`); 74 instructions (GHM 74). | 2 in 2 (`sub_82589790`, `sub_825927C8`) / 1 (BeginTiling) | known |
| Resolve | `sub_821685D0` | `sub_821DC640` | Flags bits 0 to 2 (`clrlwi r15,r4,29`, `0x821685E0`), 4 = depth from `+12464` (`0x82168620`), RT from `+12448 + 4 * index` (`addi r11,r15,3112`, `0x82168628`); sample select from `[surface+24]` (`0x82168634`); destination fetch constant `dest+28` to `+48` (`0x82168680` to `0x821686C8`); RB_COPY_CONTROL / DEST_BASE / DEST_PITCH / DEST_INFO into the shadow `+10776` to `+10788` (`0x82168940` to `0x82168964`), `+10820`; `li r28,8984` (`0x2318`, `0x821693FC`); `EVENT_WRITE`, `IM_LOAD_IMMEDIATE`, `WAIT_REG_MEM`. | 14 in 10 / 3 in 2 (EndTiling ×2, `sub_8216D448`) | known |
| BeginTiling | `sub_82172B98` | `sub_821D03E0` | Count to `+12756` (`0x82172BB4`); rects to `+12760 + 16 * i`, 32 aligned corners to `+13000 + 8 * i`; flags `+13196` (`0x82172C5C`), max extents `+13188`/`+13192` (`0x82172C64`); ClearF unless `flags & 1` (`0x82172D50`); clear Z to `+13216` (`0x82172DA4`); reference surface `+12732` (`0x82172DDC`), saved RT0 to 3 and depth `+12736` to `+12752`, `+12728` (`0x82172DF4`); tiling header `+13368` (`0x82172E78`, `lis r10,-31232` = `0x86000000`); `EVENT_WRITE(25)` (`0x82172EC0`); calls SetViewport, SetScissorRect, the SET_BIN helper `sub_82160FF8`, the recording start `sub_8217D658`; 222 instructions (GHM 220). | 1 in 1 (`sub_82583770`, a pass through wrapper; its callers pass flags 0, NULL clear color and stencil 0, and call it only when the tile count is over 1) / 0 | known |
| EndTiling | `sub_82172F10` | `sub_821D0750` | NULL rects become `+12760` (`addi r5,r3,12760`, `0x82172F58`); per tile SetPredication (`0x82172FF4`, `0x821730C0`, `0x82173100`) and **Resolve** (`0x8217306C`, `0x8217309C`) with the tiling clear values `+13200`/`+13220` (`0x82173040`/`0x82173044`); InsertCallback `sub_82165EA8` (`0x82173148`); replay submission `sub_8217D838` (`0x82173378`); 300 instructions (GHM 302). | 1 in 1 (`sub_825837B0`, a pass through wrapper; its callers pass a destination texture from a virtual call and flags `(r31 << 26) & 0xFC000000` or 0) / 0 | known |
| BeginZPass, EndZPass | none | none | Not linked, as in GH Metallica: the functions after EndTiling are CreateQuery `sub_821734E8` (a 156 byte query object), its AddRef `sub_821735D0` and Release `sub_821735E8`, then QueryIssue and GetData. | none | likely absent |
| SetPredication | `sub_82172A28` | `sub_821D0270` | `+12716` = mask or minus one for 0 (`0x82172B38`, `0x82172B28`), `+12724` = mask not 0 (`0x82172A4C`, `0x82172B40`), `+12720 = 0` (`0x82172B50`), `+10932` = mask (`0x82172B54`), `+10936 = 0` (`0x82172B5C`); recomputes `+10940 & 0x01` from the saved targets; `SET_BIN_MASK_LO(mask)` (`0xC0006000`, `0x82172B6C` to `0x82172B7C`); 92 instructions (GHM 92). | 10 in 5 (`sub_825983F8`, `sub_8259CAB8`, `sub_825A8FC0`, `sub_825B8ED0`, `sub_825EEED0`) / 5 in 2 (Swap, EndTiling ×4) | known |
| SetViewport | `sub_82163B08` | `sub_821C51D8` | Loads `{X, Y, W, H}` (u32) and MinZ/MaxZ (`0x82163B14` to `0x82163B28`), converts with `fcfid`, calls the float setter `sub_821614A0` (`0x82163B70`), which stores six floats at `+12656` to `+12676` (`0x82161620` to `0x82161668`) and `+12680 = 0` (`0x82161658`). The RT0 helper ends with `b 0x82163b08` (`0x821611AC`), as GHM's. | 4 in 3 (`sub_82586680`, `sub_825983F8`, `sub_825A8FC0`) / 4 in 3 | known |
| SetScissorRect | `sub_82163A10` | `sub_821C4838` | Reads the viewport `+12656` to `+12668` (`0x82163A24` to `0x82163A4C`); stores the rect at `+12684` to `+12696` (`0x82163A7C` to `0x82163A90`); intersects with the viewport when `+11864` (`0x82163A78`; its setter `sub_82162210`, `stw r4,11864(r3)` then `b 0x82163a10`); PA_SC_WINDOW_SCISSOR_TL/BR `+10436`/`+10440` (`0x82163AE4`/`0x82163AE8`), then the window scissor writer `sub_821611B0` (`0x82163AEC`). | 1 in 1 (`sub_825A8FC0`) plus 3 in 2 through `sub_82162210` / 4 in 3 | known |

### Shaders, vertex declaration, stream source, indices, textures, constants

| Call | Battlefield | GHM | Evidence | Engine / library | Confidence |
|---|---|---|---|---|---|
| SetVertexShader | `sub_8217B540` | `sub_821C0998` | `+16 |= 0x80000` (`0x8217B55C` to `0x8217B564`); old shader fenced with `[+10908]` (`0x8217B574` to `0x8217B580`); `stw r29,12704(r30)` (`0x8217B5E0`); `+10942 &= 0x7F` (`0x8217B5E4`/`0x8217B5E8`); literal block `vs + 872 + [vs+892]` (`0x8217B5F0` to `0x8217B604`): its group mask cleared from `+0` (`andc`, `0x8217B610`), bool bit 56 of `+32` (`0x8217B624` to `0x8217B634`), records copied into `dev + 1152 + offset` (`0x8217B640` to `0x8217B69C`); 115 instructions (GHM 116). | 3 in 3 (`sub_82586970`, `sub_825A8FC0`, `sub_825B8ED0`) / 1 (`sub_8216BB10`) | known |
| SetPixelShader | `sub_8217B198` | `sub_821C07D8` | Old shader fenced (`0x8217B1A4` to `0x8217B218`); `stw r29,12700(r30)` (`0x8217B21C`); `+16 |= 0x100000` then `0x20000` (`0x8217B228`, `0x8217B230`); literal block from `ps + 40 + [ps+60]`; 111 instructions (GHM 112). | 2 in 2 (`sub_82586900`, `sub_825A8FC0`) / 1 | known |
| SetVertexDeclaration | `sub_8217B710` | `sub_821C0B68` | The whole function: `stw r4,11824(r3)`; `ld r11,16(r3)`; `oris r11,r11,8`; `std r11,16(r3)`; `blr` (`0x8217B710` to `0x8217B720`). The program flush reads `+11824`. | 7 in 6 / 0 | known |
| SetStreamSource | `sub_821644B0` | `sub_821C49A8` | **Short form**: writes vertex fetch constant `95 - stream` (`subfic r11,r4,95`, `0x821644BC`; slot `(95 - s) / 3` at `+1152 + 24 * slot`, `0x821644C8` to `0x821644D8`): dword 0 = `[vb+24] + offset` (`0x82164538`, `0x82164540`, physical with type bits), dword 1 = `[vb+28] - offset` (`0x8216453C`, `0x82164544`); sets its own fetch dirty bit in `+24` (`0x82164568` to `0x82164570`). **No stride argument, no buffer pointer at `+12468`, no stride byte at `+12536`, no caller supplied dirty bit** (GHM's form has all four; 51 instructions against GHM's 106). The engine's stream binder `sub_825981B8` calls it once per stream (`0x8259826C`), right after SetIndices. | 1 in 1 (called in a loop) / 0 | known (short form) |
| SetIndices | `sub_82163B88` | `sub_821C4B50` | Old buffer `[+12444]` (`0x82163B94`) fenced: `[ib+8] = [+10908]` (`0x82163BA8` to `0x82163BB4`) or queued on the deferred list `+13524`/`+13528` when `+10912` matches (`0x82163BBC` to `0x82163C08`); `stw r29,12444(r31)` (`0x82163C0C`). | 2 in 2 (`sub_82586100`, `sub_825981B8`) / 1 | known |
| SetTexture | `sub_82172788` | `sub_821BFA10` | Pointer `+12552 + 4 * i` (`addi r11,r4,3138`, `0x82172794`; `stwx` `0x8217288C`); copies `texture+28` to `+48` (`0x821727AC` to `0x821727CC`) into fetch slot i at `+1152 + 24 * i` (`addi r11,r4,48; mulli r11,r11,24`, `0x821727B0`/`0x821727C0`), keeping the sampler bits; per sampler bytes `+11958 + i` (`0x8217281C`), `+11984 + i` (`0x82172860`); `+24 |= r6` (`0x82172878` to `0x82172888`); old texture fenced (`0x82172898` to `0x821728F8`). | 7 in 7 (`sub_82583C08`, `sub_82584310`, `sub_82586100`, `sub_82598A68`, `sub_8259D0B0`, `sub_825A4F08`, `sub_825B03E8`) / 2 in 2 | known |
| SetVertexShaderConstantF | `sub_8217AD60` | `sub_821C0550` | `+1920 + 16 * start` (`addi r10,r4,120; rlwinm r10,r10,4,0,27`, `0x8217AD60`/`0x8217AD68`); vector copy loop; `+0 |= r7` (`0x8217AE28` to `0x8217AE30`); 54 instructions (GHM 54). | 6 in 6 / 0 | known |
| SetPixelShaderConstantF | `sub_8217AE38` | `sub_821C0628` | `+6016 + 16 * start` (`addi r10,r4,376`, `0x8217AE38`); `+8 |= r7` (`0x8217AF00` to `0x8217AF08`); 54 instructions. | 7 in 6 / 0 | known |
| Render state setters | `0x82161700` to `0x82163A00` | `0x821C2570` to `0x821C3E40` | One function per state plus a getter after each; 84 write the register shadow (`rs_setters.py` with the shadow groups below), 37 have engine callers (blend controls, depth control, stencil ref mask, color mask, poly offset, PA_SU_SC_MODE_CNTL, the tessellation levels...). They write only the shadow and its dirty bits. | 118 sites in 17 functions | known |

### Resources, command buffer, queries, frame

| Call | Battlefield | GHM | Evidence | Engine / library | Confidence |
|---|---|---|---|---|---|
| VertexBufferLock, VertexBufferUnlock | none | `sub_821C7410`, `sub_821C64C0` | No typed thunk: the common unlock `sub_821761B0` has only the two texture callers below, and the resource wait `sub_82175EB8` is reached only from the texture locks. The engine keeps its own buffer objects and flushes them itself (section 5). | none | likely absent |
| IndexBufferLock, IndexBufferUnlock | none | `sub_821C7460`, `sub_821C64D0` | As above. | none | likely absent |
| TextureUnlock | `sub_821722D0` | `sub_821BDA28` | The whole function: `lwz r11,48(r3)`; `lwz r10,32(r3)`; both `& ~0xFFF`; `b 0x821761b0` (the common unlock, which flushes the range with FlushCpuRange at `0x8217624C`/`0x821762C8`). `sub_82172708` does the same for an object whose texture is at `+24` (GHM `sub_821BDA40`). | 15 in 4 (`sub_8258E840`, `sub_825918E0`, `sub_827B8558`, `sub_8280EB68`) / 0 | known |
| FlushCpuRange | `sub_82178310` | `sub_821D00A8` | Returns for `0x7F100000` to `0x86FFFFFF` (`lis r11,32528`, `lis r11,-30976`, `0x82178310` to `0x82178324`); `dcbf` over `[start, end)` in 128 byte lines, unrolled by 8 (`0x82178354` to `0x821783AC`); `sync` (`0x821783B0`). | 0 / 18 in 15 (the unlocks, the allocator, the program flush, constants) | known |
| KickSegment | `sub_821658B0` | `sub_821C91C0` | Returns the write pointer; outside a tiling recording (`+13232 == 0`, `0x821658D8`) and outside the command buffer device mode (`+10940 & 0x80`, `0x821658E4`) a submission record (`sub_82165240`, `sub_82164B58`, `sub_821656B0`); then the kick tail `sub_82165528` (`0x82165940`): the segment fence `sub_82165438` (`INVALIDATE_STATE`, two `EVENT_WRITE_SHD`, `+10908 += 2`) and the submission, then **continues in the same memory aligned to 32 bytes** (`+14928` = new start, `[+48]` = start minus 4, `0x82165668` to `0x821656A0`), or NewSegment past the threshold (`0x8216568C`). In a tiling recording it appends `[0x81000000 | dwords, address]` to the tiling header `+13368` (`0x821655C4` to `0x82165620`); on a command buffer device, to the command buffer object's list (`+13516`, `+13536`, `0x8216555C` to `0x821655BC`). | 0 / 123 in 63 | known |
| NewSegment | `sub_82164CF8` | `sub_821C8E08` | `(dwords + 14) * 4` bytes (`addi r11,r4,14`, `0x82164D34`) or the minimum `+14912` (`0x82164D54`); `+14928` = start (`0x82164E38`), `[+48]` = start minus 4 (`0x82164E40`), `+52` = end (`0x82164E5C`), `+56` = end minus 160 (`0x82164E58`/`0x82164E60`). Callers: Present (`0x82160F3C`), the reservation `sub_82164580` (`0x821645CC`), the kick tail (`0x8216568C`). | 0 / 3 in 3 | known |
| SetShaderGPRAllocation | `sub_8217B898` | `sub_821C0CF0` | Defaults 64/64 (`0x8217B8BC`/`0x8217B8C0`); packed GPRs into `+10920` (`0x8217B8E4`); three `INDIRECT_BUFFER`s (`0xC0013F00`, `0x8217B90C` to `0x8217B914`, bin predicated when `+12724 & flags`) to the buffers prebuilt at device init (`+13784`/`+13788` at `0x8217B924`/`0x8217B934`, `+13796 + 8 * n` at `0x8217B944`, `+14816`/`+14820`). | 6 in 5 (`sub_82586240`, `sub_825896F8`, `sub_825927C8`, `sub_825983F8`, `sub_825B4D78` ×2) / 6 in 3 | known |
| QueryIssue | `sub_82173830` | `sub_821CC368` | `[q+0]` = device (`0x8217383C`); BEGIN (`flags & 2`): tiles `[q+148]` = `+12756` when tiling else 1 (`0x82173858` to `0x82173864`), packets through `sub_8217DBC0` (`EVENT_WRITE_ZPD`, RB_SAMPLE_COUNT_ADDR, bin predicated) for type 9, or `EVENT_WRITE(25)` for type 10 (`0x821738A8` to `0x821738B8`); END: marker `0xFFFFFEED` (`li r11,-275`, `0x82173954`, `0x82173A08`) by the CPU or by `MEM_WRITE` + `WAIT_REG_MEM`; END fence `[+10908]` (`0x82173A9C`). | 1 in 1 (`sub_827A11E8`) / 0 | known |
| QueryGetData | `sub_82173AB8` | `sub_821CD008` | Pending while the END fence `[q+24]` (`0x82173AF8`) is not reached by the write back block `[[+10896]]` (`0x82173B04`); sums per tile (`[q+148]`, `0x82173B74`) through the `0xC0000000` view (`addis r11,r11,-16384`, `0x82173B98`), byte reversed (`lwbrx`, `0x82173BC0`...). | 1 in 1 (`sub_827A2850`) / 0 | known |
| Swap | `sub_82160310` | `sub_821D0F40` | With no front buffer a `WAIT_REG_MEM` on `[+10900]+4` (`0x8216038C` to `0x821603B8`, `0x82160468` to `0x821604B4`); present interval `+13596` mapped 0/1 to 1, 2 to 2, 4 to 3, else 0 (`0x821603C0` to `0x821603FC`); descriptor `frontBuffer | interval << 8 | threshold` from `+11860` (`0x82160400` to `0x8216041C`); GPU callback `0x82160160` through `sub_82165AA8` (`0x82160430` to `0x82160444`); `+10941 |= 0x10` (`0x821604C4` to `0x821604CC`). **Synchronize** is `sub_82160848` (`li r4,0; b 0x82160310`). | 0 direct, 1 through Synchronize (`sub_82586060`) / 4 in 4 | known |
| Present | `sub_82160850` | `sub_821D1418` | `+16560 += 1` (`0x8216086C` to `0x82160880`); copies the front buffer's fetch constant (`addi r4,r28,28`, 24 bytes, `0x82160874` to `0x82160890`) and 52 bytes into `+14844` (`0x821608D4` to `0x821608F8`); **calls VdSwap** (`0x82160B0C`); the scaler parameters `+13640` (`0x82160A10`, `0x82160C58`); NewSegment(`[+14912] >> 2`) (`0x82160F30` to `0x82160F3C`). r4 is the front buffer. | 1 in 1 (`sub_82586060`: Synchronize `0x82586078`, Resolve `0x825860C4`, Present `0x825860E8`) / 1 (the worker `sub_8217D418`) | known |

**The worker** `sub_8217D418` (no callers, a thread entry) takes the device's critical section `+14944`
(`0x8217D4F0`) and calls Swap and **Present(dev, dev+14844, 0)** (`0x8217D518`), GHM `sub_821D2F28`.

## 3. The device layout (XDK 2.0.7978)

The device is 24064 bytes: `Direct3D_CreateDevice` `sub_8216B890` allocates 24192 (`li r3,24192`, `0x8216B8AC`),
aligns to 128 (`addi r10,r11,131; rlwinm r31,r10,0,0,24`, `0x8216B8C8`/`0x8216B8D0`) and clears 24064
(`li r5,24064`, `0x8216B8CC`). GH Metallica allocates 24064 with alignment 128.

DrawIndexedVertices' flush, instruction for instruction GH Metallica's:

```
+0  VS constants  -> sub_82178D98(dev, mask, 0x4000, dev+1920)                    0x8217FCA0
+8  PS constants  -> sub_82178D98(dev, mask, 0x4400, dev+6016)                    0x8217FCC0
+16 bits 17-20    -> program flush sub_82179DE8                                   0x8217FCE4
    & [+40]       -> tile dependent flush sub_82178668(dev, mask, [+10560])       0x8217FD08
    bits 0-11     -> 0x2200 group, dev+10548 (12)                                 0x8217FD2C
    bits 12-16    -> 0x2180 group, dev+10528 (5)                                  0x8217FD4C
    bits 42-57    -> 0x2000 group, dev+10368 (16)                                 0x8217FD78
    bits 21-41    -> 0x2100 group, dev+10444 (21)                                 0x8217FDA0
+24 bits 0-31     -> fetch flush sub_82178C40 (rldicr r4,r28,32,31), dev+1152    0x8217FDC4
    bits 34-54    -> 0x2280 group, dev+10596 (21)                                 0x8217FDF0
+32 bits 0-37     -> 0x2300 group, dev+10680 (38)                                 0x8217FE1C
    bits 38-45    -> 0x2380 group, dev+10832 (8)                                  0x8217FE44
    bits 49-54    -> misc flush sub_82178AC0 (clip planes)                        0x8217FE64
    bit 56        -> bool/loop constants 0x4900, dev+10112                        0x8217FE8C
```

The group helper is `sub_82178A00` (GHM's register group helper), called with `(dev, mask shifted to the top, first
register, dev + offset)`. The bit ranges come from the same immediates as GH Metallica's (`clrldi r11,r30,52`;
`rlwinm r11,r30,0,15,19`; `0xFFFF << 42`; `lis r12,-32; clrldi r12,r12,22`; `0x1FFFFF << 34`; `clrldi r11,r27,26`;
`0xFF << 38`; `63 << 49`; `1 << 56`).

**Verdict: the layout matches GH Metallica's (7978) exactly.** Every field below was read in Battlefield's code at
GH Metallica's offset. No field was found at another offset.

| Field | Offset | Battlefield evidence |
|---|---|---|
| Dirty masks (u64): VS, PS, groups and program, fetch (low half, slot 0 = bit 31) and `0x2280`, `0x2300`/`0x2380`/misc/bool, tile dependent | `+0`, `+8`, `+16`, `+24`, `+32`, `+40` | The flush above; SetTexture `+24 |= r6`; BeginVertices clears slot 31's bit 0 (`0x8217F3E0`) |
| Command buffer write pointer (last dword written) / segment end / threshold | `+48` / `+52` / `+56` | NewSegment `0x82164E40`, `0x82164E5C`, `0x82164E60`; draws `0x8217FEA8`/`0x8217FEAC`/`0x82180078` |
| Fetch constants (32 × 24 bytes) | `+1152` | SetTexture `0x821727C0`; SetStreamSource `0x821644D8`; SetVertexShader `0x8217B640` |
| VS / PS float constants | `+1920` / `+6016` | Flush `0x8217FC98` / `0x8217FCB0`; `sub_8217AD60` / `sub_8217AE38` |
| Bool, loop constants | `+10112` (VS bool), `+10128` (PS bool), `+10144`, `+10208` (loops) | Flush `0x8217FE7C`; `sub_8217AF10` (`addi r11,r11,2528`), `sub_8217AF70` (`2532`), `sub_8217AFD0` (`2535`), `sub_8217B020` (`2551`) |
| Register shadow `0x2000`/`0x2100`/`0x2180`/`0x2200`/`0x2280`/`0x2300`/`0x2380` | `+10368`/`+10444`/`+10528`/`+10548`/`+10596`/`+10680`/`+10832` | Flush `0x8217FD68`, `0x8217FD90`, `0x8217FD3C`, `0x8217FD1C`, `0x8217FDE0`, `0x8217FE0C`, `0x8217FE34` |
| PA_SC_WINDOW_SCISSOR_TL / BR | `+10436` / `+10440` | SetScissorRect `0x82163AE4`/`0x82163AE8` |
| RB_HIZCONTROL; RB_COPY_* shadow | `+10560`; `+10776` to `+10788` | SetDepthStencilSurface `0x82163FC0`; Resolve `0x82168940` to `0x82168964` |
| Write back block pointer; swap block pointer | `+10896`; `+10900` | Kick `0x821658C4`; GetData `0x82173B04`; Swap `0x8216038C` |
| Fence counter; deferred fence mask | `+10908`; `+10912` | SetIndices `0x82163BA8`, `0x82163BBC`; segment fence `sub_82165438` |
| GPR allocation; predication copies | `+10920`; `+10932`, `+10936` | `0x8217B8E4`; SetPredication `0x82172B54`, `0x82172B5C` |
| Flag bytes | `+10940` (`0x01` tiling targets bound, `0x20` tiling, `0x80` command buffer device), `+10941`, `+10942`, `+10943` | Draws `0x8217FF80`; SetPredication `0x82172B24`; CreateDevice `0x8216B954`; Swap `0x821604CC` |
| Vertex declaration | `+11824` | SetVertexDeclaration `0x8217B710` |
| Patched stream strides; stream strides | `+11840 + s`; `+12536 + s` | BeginVertices `0x8217F394`, `0x8217F3A0` |
| Packed render state word (threshold bits 23 to 29); scissor test enable | `+11860`; `+11864` | Swap `0x82160400`; `sub_82162210`, SetScissorRect `0x82163A78` |
| Per RT color write masks; depth/stencil enables | `+11868 + 4 * i`; `+11884`, `+11888` | SetRenderTarget; `sub_82161E38`, `sub_82161EE0` |
| Per sampler bytes | `+11958 + i`, `+11984 + i` | SetTexture `0x8217281C`, `0x82172860` |
| Visibility select bytes | `+12130[+12195]` | Draws `0x8217FE9C`, `0x8217FF04` |
| Index buffer; RT0 to RT3; depth | `+12444`; `+12448` to `+12460`; `+12464` | SetIndices `0x82163C0C`; SetRenderTarget `0x82163C2C`; SetDepthStencilSurface `0x82163F88` |
| Vertex buffers, one per stream (16) | `+12468 + 4 * s` | Only the bindings reset `sub_8216BB10` (`0x8216BBB0`, 16 streams); the short SetStreamSource never writes it |
| Textures | `+12552 + 4 * i` | SetTexture `0x82172794` |
| Viewport (six floats), `+12680 = 0`; scissor rect | `+12656` to `+12676`; `+12684` to `+12696` | `sub_821614A0` `0x82161620` to `0x82161668`; SetScissorRect `0x82163A7C` |
| Pixel shader; vertex shader | `+12700`; `+12704` | `0x8217B21C`; `0x8217B5E0` |
| Predication mask, `+12720`, predication active | `+12716`, `+12720`, `+12724` | SetPredication |
| Tiling block: `+12728`, reference surface, saved targets, tile count, rects, corners, extents, flags, clear values | `+12728`, `+12732`, `+12736` to `+12752`, `+12756`, `+12760`, `+13000`, `+13188`/`+13192`, `+13196`, `+13200`/`+13216`/`+13220` | BeginTiling (section 2) |
| Tiling recording; extents records; tiling header | `+13232`; `+13248` (`+13256`/`+13260`); `+13368` (`+13376`/`+13380`) | Kick `0x821658D8`; draws `0x8217FFB0`; kick tail `0x821655F0` |
| BeginVertices: end of packets, data, dword count | `+13444`, `+13448`, `+13456` | `0x8217F808` to `0x8217F810` |
| Command buffer being recorded (BeginCommandBuffer) | `+13516` | `sub_82177190` (`stw r30,13516(r31)`, `0x82177264`); kick tail `0x8216555C`; NewSegment `0x82164D3C` |
| Deferred fence list | `+13524`, `+13528` | SetIndices `0x82163BCC` to `0x82163C08` |
| Present interval; scaler parameters | `+13596`; `+13640` | `sub_82162FB0` (`stw r4,13596(r3)`); Swap `0x821603C0`; Present `0x82160A10` |
| Prebuilt GPR buffers | `+13780` to `+13788`, `+13796 + 8 * n`, `+14816`/`+14820` | `sub_8217ABD0` `0x8217AC04` to `0x8217AD44` |
| Front buffer copy; segment minimum; segment start; critical section | `+14844`; `+14912`; `+14928`; `+14944` | Present `0x821608D4`; NewSegment `0x82164D54`, `0x82164E38`; worker `0x8217D4F0` |
| Present count | `+16560` | Present `0x8216086C` |
| Thread owner | `+10888` | CreateDevice `0x8216B8FC`; `sub_8216BA28`, `sub_8216BA68` (likely Acquire/ReleaseThreadOwnership) |

**Profile values** (identical to GH Metallica's `DeviceLayout`): masks `0`/`8`/`24`, `fetch_dirty_slot0_bit = 31`,
`command_buffer_write = 48`, fetch `1152`, VS `1920`, PS `6016`, bool/loop `10112`, the seven groups above,
`fence_counter = 10908`, `write_back_block = 10896`, `flags = 10940`, declaration `11824`, index buffer `12444`,
targets `12448`, depth `12464`, vertex buffers `12468` (never written here), textures `12552`, viewport `12656`,
shaders `12700`/`12704`, predication `12716`, tiles `12756`/`12760`. `inline_draw_end` stays unset: DrawVerticesUP
is the only place that commits an inline draw, and it is hooked.

## 4. Resource objects

The same as GH Metallica's (and POTCAWE's), each read here.

| Object | Field | Offset | Battlefield evidence |
|---|---|---|---|
| Any resource | Common word (low 4 bits the type, 4 = surface; bit 31 of an index buffer = 32 bit indices) | `+0` | DrawIndexedVertices `0x8217FF10`/`0x8217FF3C`; Release `sub_82176440` (`clrlwi r10,r11,28; cmplwi cr6,r10,4`) |
| Any resource | Reference count (atomic) | `+4` | AddRef `sub_821763C8`, Release `sub_82176440` (`lwarx`/`stwcx.` on `r3+4`) |
| Any resource | Fence of its last use | `+8` | SetIndices `0x82163BB4`; SetTexture `0x821728A4`; SetVertexShader `0x8217B580` |
| Texture | Fetch constant, 6 dwords (`+28` to `+48`; size in dword 2 at `+36`) | `+28` | SetTexture `0x821727AC` to `0x821727CC`; Resolve `0x82168680` to `0x821686C8`; Present `0x82160874`; TextureUnlock reads `+32`/`+48` |
| Surface | RB_SURFACE_INFO; RB_COLOR_INFO or RB_DEPTH_INFO; RB_HIZCONTROL | `+24`; `+28`; `+32` | RT0 helper `0x82161120`; SetRenderTarget `0x82163C58`; SetDepthStencilSurface `0x82163FA8`, `0x82163FBC` |
| Surface | Size: width minus one in bits 18 to 31, height minus one in bits 3 to 17 | `+36` | ClearF `0x8217F28C` to `0x8217F2AC` (`rlwinm r10,r9,14,18,31`; `rlwinm r9,r9,29,17,31`) |
| Vertex / index buffer | Base address (type bits included); size | `+24`; `+28` | SetStreamSource `0x82164538`/`0x8216453C`; DrawIndexedVertices `0x8217FEFC` |
| Query | Device; type (8, 9 occlusion, 10 extents); reference count; state; END fence; tile count; count | `+0`; `+4`; `+12`; `+16`; `+24`; `+148`; `+152` | CreateQuery `sub_821734E8` (`0x82173520` to `0x82173534`); QueryIssue `0x8217383C`, `0x82173864`; GetData `0x82173AF8` |
| Vertex shader | Literal block `vs + 872 + [vs+892]` | | SetVertexShader `0x8217B5F0` to `0x8217B604` |
| Pixel shader | Literal block `ps + 40 + [ps+60]`; program block `ps + [ps+64]` | | SetPixelShader; GpuLoadShaders `0x82180314` |

`ObjectLayout` for the profile: `texture_fetch = 28`, `surface_info = 24`, `surface_color_depth_info = 28`,
`surface_size = 36` (width minus one `{18, 14}`, height minus one `{3, 15}`), `buffer_address = 24`,
`buffer_size = 28`.

## 5. Engine behavior

| Topic | What the engine does |
|---|---|
| **GPU ownership of shaders and constants** | The engine has two paths, chosen by a byte of a renderer config object, `[[renderer+672]+44]+200` (read at `0x825A9E68`, `0x825A7F44`, `0x825B8EF8`, `0x825B925C` and in `sub_8259CAB8`, `sub_8259D290`, `sub_825B79B0`, `sub_825EE490`). **With it set** (the M1 counts say it is, at run time): at the start of a draw list (`sub_825B8ED0`, `0x825B8F04` to `0x825B8FA8`) the engine zeroes the VS and PS constant masks `+0`/`+8`, clears bits 19, 20, 16, 15, 14 and 3 of `+16` (vertex program, pixel program, SQ_PROGRAM_CNTL, SQ_CONTEXT_MISC, SQ_INTERPOLATOR_CNTL, RB_MODECONTROL) and bit 56 of `+32` (bool/loop) inline, then calls SetVertexShader(dev, NULL): the XDK's GpuOwn calls, inlined. Shaders are then loaded by **`sub_821802A8`** (likely GpuLoadShaders(dev, vs, ps, fetch), `0x825A9EA4`, about 350 a frame): `IM_LOAD` for the pixel and vertex microcode, `SET_CONSTANT` of SQ_PROGRAM_CNTL / SQ_CONTEXT_MISC / SQ_INTERPOLATOR_CNTL (`0x40180`) and RB_MODECONTROL (`0x40208`), straight into the command buffer. Constants come from **`sub_82180220`** (likely GpuBeginShaderConstantF4(dev, kind, first, vec4 count), kind 0 = VS, 1 = PS; 8 sites in `sub_8259CAB8`, `sub_8259D290`, `sub_825A7E10`, `sub_825B79B0`; about 1,540 a frame): it reserves `count * 4 + 5` dwords (`0x82180240`), pads with type 2 packets to align the data to 16 bytes, writes a `SET_CONSTANT` header (`0xC0002D00 | count * 4 << 16 | [+12724]`, `0x82180278` to `0x82180288`) and the register offset, **moves the write pointer past the data** (`0x8218029C`) and returns the data pointer, which the engine fills after the call. There is no end call. At the end of the list `sub_82180200` (likely GpuDisownAll) sets all five masks to all ones (`0x825B926C`). **With it clear**, the engine calls SetPixelShader / SetVertexShader (`0x825A9EC8`, `0x825A9EF4`) and SetVertexShaderConstantF / SetPixelShaderConstantF with a mask it computes, always from c0 (`0x825A7FF0` to `0x825A802C`). |
| Why it matters | NativeXRenderer reads only the packets a hooked call writes, between the write pointer at its entry and at its exit (`src/state.cpp`, `Enter`/`Leave`). The GpuLoadShaders packets come from an unhooked call, and the constant data is written after `sub_82180220` returns, outside any call. On the GPU path the draws' own packets carry no programs and no constants, and the shadow's constants are not what the GPU uses. Two ways: (a) a library change, with calls for GpuLoadShaders (its packets are its own, read at its exit) and for GpuBeginShaderConstantF4 (a begin whose data must be read later, at the next device call, as the inline draws' end is); or (b) clear the engine's config byte `+200` so the engine takes the D3D path, which every existing call covers. Its writer was not found statically (no `stb` at `+200` in `0x82560000` to `0x82620000`; it may be written as part of a word or a struct copy). |
| Inline sampler states | The engine patches fetch constant dwords 3 and 5 of a slot (`dev + 1164 + 24 * slot`, `dev + 1172 + 24 * slot`) and sets the slot's bit in `+24` itself, without a call: `sub_825987B8` (`0x825988E4` to `0x825988F8`...), `sub_82583C08` (`0x82583D94` to `0x82583DA8`...), `sub_825983F8` (`0x825985E4` to `0x825985F8`), `sub_8259C520` (`0x8259C894` to `0x8259C8A8`). NativeXRenderer reads fetch constants from the shadow at draw time, so these are covered. No inline write to the write pointer, the register shadow groups, or the current objects was found. |
| BeginVertices / EndVertices | The engine never calls BeginVertices: it is reached only from DrawVerticesUP (6 engine sites), which inlines the end. No EndVertices function and no inlined end in the engine (the only `13444` uses are `0x8217F808` and `0x8217F858`). So `inline_draw_end` stays unset and EndVertices is not hooked, as in POTCAWE. |
| DrawIndexedVerticesUP and the inline draw end | Not linked. |
| Draw submitter | Every draw goes through one engine function, `sub_82598298` (from `sub_825B79B0` and `sub_825B8ED0` ×2): DrawIndexedVertices, DrawVertices ×2 or DrawVerticesUP by a draw record's kind (`[rec+48]`), with the primitive table at `0x820C3140` (`lis r11,-32244; addi r11,r11,12608`). DrawVerticesUP also from five other functions (`sub_8259A188`, `sub_8259EAE8`, `sub_8259F1E8`, `sub_8259F3C0`, `sub_8259F6E0`), each of which also calls SetVertexDeclaration. |
| Streams and indices | The engine's stream binder `sub_825981B8` calls SetIndices (`0x82598218`, cached in the engine at `+12068`) and the short SetStreamSource per stream (`0x8259826C`). Strides are not passed to the library: the vertex shader is not patched per stride through SetStreamSource. |
| **Command buffer devices and recorded command buffers** | Besides the main device (type 1, `0x8259296C`), the engine creates **command buffer devices** with type 2 (`0x825B65B4`, in a loop of `li r31,256` over 912 byte job records, `0x825B6598`): CreateDevice skips the device init for them and sets `+10940 |= 0x80` (`0x8216B954`), the mode in which the kick never submits and appends to the command buffer object instead. The engine records on them on other threads, under `sub_8216BA28` / `sub_8216BA68` (likely Acquire/ReleaseThreadOwnership, `+10888`): `sub_82177110` (likely CreateCommandBuffer, `0x825B9BF0`), `sub_82177190` (likely BeginCommandBuffer, `+13516` = the buffer, `0x825B9C90`), then targets and draws, `sub_82177780` (likely EndCommandBuffer, `0x8259D57C`) and **`sub_82177CF8`** (likely RunCommandBuffer(dev, buffer, predication), on the global device, when the config byte `+208` is set, `0x8259D5A4`), which flushes the main device's state and emits the recorded segments as `INDIRECT_BUFFER`s (`sub_82165788` to `sub_82165058`). `sub_82177B58` (likely SetCommandBufferPredication) writes 64 bit tile masks (`SET_BIN_MASK_LO` and `_HI`) and marks the buffer `+108 |= 64`. NativeXRenderer has no calls for these: hooked draws on a command buffer device would be seen at record time, on the recording thread, with a write pointer in the command buffer's memory, and drawn then instead of at RunCommandBuffer. M1 accounted for 1,438 of the frame log's 1,440 draws with 862 hooked builder calls and the 576 below, so in Wake Island every draw the GPU ran was issued once through a hooked builder; whether recording happens per frame is open (section 7). |
| **The single vertex memexport draws** (M1) | They are **SetShaderGPRAllocation's**: at device init `sub_8217ABD0` (from `sub_8216C468`) allocates 8192 bytes of physical memory (`0x8217ABEC`), writes the shader loads (`sub_8217A8E0`, the shader built into the executable) and, into the third buffer (`+14816`/`+14820`), **24 `DRAW_INDX_2` packets of one point each** (`0xC0003600`, `0x00010081`, `li r9,24`, `0x8217ACEC` to `0x8217AD10`). Each SetShaderGPRAllocation call runs the three buffers with `INDIRECT_BUFFER`; 24 calls a frame give M1's 576 draws in one indirect buffer (`1FC9BD20`) replayed 24 times. NativeXRenderer's rule for them: nothing to draw (the host has no GPR split). |
| Other engine packets outside hooked calls | `sub_82165DE0` (likely InsertFence: `+10928`, then a kick), `sub_82165D60` (likely BlockUntilIdle: `WAIT_UNTIL` `0x05C8` = `0x20000`, then a fence wait), `sub_82165EA8` (InsertCallback: `WAIT_REG_MEM` + interrupt through `sub_82165AA8`; GHM `sub_821C94E8`), `sub_8216BA78` (an `EVENT_WRITE(15)`, with `WAIT_UNTIL` idle when r4 = 1; 3 engine sites), `sub_82177B58` above, and the GPU ownership packets above. |
| Clears | Only ClearF, from the engine's clear method `sub_82589790` (flags rebuilt from the engine's own bits, Z 1.0 or 0.0, stencil 0) and `sub_825927C8`; BeginTiling's own clear. |
| Resolves | 14 sites in 10 functions; the frame ends with Synchronize, a Resolve into the front buffer and Present (`sub_82586060`). |
| **Tiling** | BeginTiling and EndTiling through pass through wrappers (`sub_82583770`, `sub_825837B0`) from `sub_825DCF78`, `sub_825EEED0` and `sub_827A0638`, taken only when the tile list holds more than one rect (`cmplwi cr6,r5,1; ble`). BeginTiling gets flags 0, a NULL clear color, stencil 0 (so it clears). **EndTiling gets a destination texture** (from a virtual call) and resolve flags 0 or `(r31 << 26) & 0xFC000000` (likely an exponent bias), with NULL rects, NULL clear color, stencil 0 and params 0: EndTiling resolves each tile itself, like GH Metallica's engine (NativeXRenderer's open question there, D60). |
| Queries | Created by `sub_821734E8` and issued and read in `sub_827A11E8` / `sub_827A2850` only. |
| GPR allocation | 6 sites in 5 engine functions, about 24 calls a frame (M1). |
| Present | `sub_82586060`: Synchronize, Resolve to the front buffer, Present(dev, frontBuffer, 0). The present interval setter `sub_82162FB0` is called from `sub_82586240`. The worker `sub_8217D418` can present again on its own thread. |
| Buffer memory | The engine's own vertex and index buffer objects flush their memory themselves with a `dcbf` loop (`sub_825835B8`: `[obj+60]` for `[obj+12]` bytes; `sub_825849C8`: `[obj+56]`; both vtable methods). No XDK buffer Lock/Unlock is called. |
| Threads | Command buffer recording runs on other threads than the render thread (thread ownership calls in `sub_825837E8`, `sub_8259D538`, `sub_825A80E0`, `sub_825B93A8`, `sub_825B9928`). NativeXRenderer expects device calls on one thread. |

## 6. Other library functions the engine calls

Not in `calls.inc`; listed for the integration and for reading traces.

| Battlefield | Role | GHM | Engine sites | Confidence |
|---|---|---|---|---|
| `sub_82163C18` | SetRenderTarget alias (`b 0x82163c20`) | `sub_821C5930` | 15 in 5 | known |
| `sub_82160848` | Synchronize (`li r4,0; b Swap`) | `sub_821D1410` | 1 | known |
| `sub_82162FB0` | Present interval setter (`stw r4,13596(r3)`) | `sub_821C3DE0` | 1 | known |
| `sub_8216B890` | Direct3D_CreateDevice (type 1 main, type 2 command buffer device) | `sub_821C5FE0` | 2 | known |
| `sub_82164248`, `sub_82164288` | GetRenderTarget, GetDepthStencilSurface (AddRef `sub_821763C8`) | | 2, 2 | likely |
| `sub_82176440`, `sub_821763C8` | Release, AddRef (atomic count at `+4`) | | 65 in 33, 12 in 10 | known |
| `sub_821644B0` | SetStreamSource short form | (`sub_821C49A8`) | 1 | known |
| `sub_8217AF10`, `sub_8217AF70`, `sub_8217AFD0`, `sub_8217B020` | VS/PS bool and int constant setters (`+10112`, `+10128`, `+10144`, `+10208`; `+32` bit 56) | | 3 each | likely |
| `sub_821721E8`, `sub_821726E8`, `sub_82172708` | Texture LockRect, surface LockRect (texture at `+24`), surface's texture unlock | (`sub_821BDA40` for the last) | 10 in 5, 5 in 3, 5 in 4 | likely |
| `sub_82180220` | GpuBeginShaderConstantF4 form | none | 8 in 4 | likely |
| `sub_821802A8` | GpuLoadShaders form | none | 1 | likely |
| `sub_82180200` | GpuDisownAll (all masks to all ones) | none | 1 | likely |
| `sub_82177110`, `sub_82177190`, `sub_82177780`, `sub_82177CF8`, `sub_82177B58` | Create, Begin, End, Run command buffer; command buffer predication | none | 1, 1, 1, 1, 5 in 4 | likely |
| `sub_8216BA28`, `sub_8216BA68` | Acquire/ReleaseThreadOwnership (`+10888`) | | 5, 5 | likely |
| `sub_82165DE0`, `sub_82165D60`, `sub_82165EA8` | InsertFence, BlockUntilIdle, InsertCallback | `sub_821C94E8` (the last) | 1, 1, 2 | likely |
| `sub_82162210` | Scissor test enable (`+11864`, then SetScissorRect) | `sub_821C51C8` | 3 in 2 | known |
| `sub_821734E8`, `sub_821735D0`, `sub_821735E8` | CreateQuery, query AddRef, query Release | | 1, 6, 7 | likely |

Internal helpers matched to GH Metallica's: the reservation `sub_82164580` (GHM `sub_821C93F8`), the allocator
`sub_82164B58` (`sub_821C82F0`), the kick tail `sub_82165528` (`sub_821C9038`), the segment fence `sub_82165438`
(`sub_821C8780`), the device init `sub_8216C468` (`sub_821CFC20`), the program flush `sub_82179DE8`
(`sub_821CB180`), the float constant flush `sub_82178D98` (`sub_821CA358`), the fetch flush `sub_82178C40`
(`sub_821CA200`), the misc flush `sub_82178AC0` (`sub_821CA080`), the tile dependent flush `sub_82178668`
(`sub_821C9C28`), the RT0 helper `sub_82161120` (`sub_821C5278`), the float viewport setter `sub_821614A0`
(`sub_821C4F70`), the window scissor writer `sub_821611B0` (`sub_821C2280`), the SET_BIN helper `sub_82160FF8`
(`sub_821C4E78`), the query packets `sub_8217DBC0` (`sub_821D23E8`), the common unlock `sub_821761B0`
(`sub_821C6178`), the clear body `sub_8217EBB0` (`sub_821DB640`), the fast clear draw `sub_8217E0D0`
(`sub_821DAB50`), the tiling replay submission `sub_8217D838` (`sub_821D30C0`), the swap callback `sub_82160160`
(`sub_821D0D90`), the worker `sub_8217D418` (`sub_821D2F28`).

## 7. Open questions

1. **The GPU ownership path.** Who sets the config byte `[[renderer+672]+44]+200`, and can it be cleared without
   other effects (the D3D path uploads constants from c0 every time, which costs more CPU)? The trace would show the
   switch: SetVertexShader/SetPixelShader and the constant setters replace `sub_821802A8` and `sub_82180220`.
2. **Command buffers.** How often does the engine record and run them in a match (the trace's thread summary, and
   hooks counting `sub_82177190`/`sub_82177CF8`)? If recorded once and run every frame, NativeXRenderer needs
   RunCommandBuffer support; if recorded every frame on job threads, it needs to tell the recording devices apart
   (their `+10940 & 0x80`) and defer their draws to the run.
3. **EndTiling with a destination** (NativeXRenderer D60), as in GH Metallica. Does the game tile at all in a match
   (more than one rect)?
4. **SetStreamSource's missing stride.** The trace prints r7 as the stride: it is whatever the caller left there.
5. **The command buffer devices' count**: 256 iterations at `0x825B6598` would be 256 devices of 24 KB; read the
   loop bounds again if the memory matters.
6. Not checked: `sub_8216BB10` (likely the bindings reset: unbinds 16 streams, targets, shaders, declaration),
   `sub_8216BA78` (EVENT_WRITE 15), Present's internal draws, the meaning of the EndTiling flags `r31 << 26`.
