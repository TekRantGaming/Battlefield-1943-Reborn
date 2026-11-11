// Battlefield 1943 (XDK 2.0.7978): the XDK Direct3D entry points NativeXRenderer can hook, one line each.
// Every address and its evidence is in docs/native-x-renderer-bf1943.md. Only calls present in calls.inc.
// Not linked in this executable: BeginIndexedVertices, EndVertices (DrawVerticesUP inlines it), Clear (the engine
// calls ClearF itself), BeginZPass, EndZPass, the typed vertex and index buffer Lock and Unlock thunks.

// Draws
NATIVE_X_RENDERER_HOOK(8217FC58, DrawIndexedVertices)  // flush +0..+40, VGT_INDX_OFFSET = r5, ib [+12444], DRAW_INDX 4 dwords at 8217FF8C
NATIVE_X_RENDERER_HOOK(8217F868, DrawVertices)  // same flush, VGT_INDX_OFFSET = r5 at 8217FAD4, DRAW_INDX 2 dwords
NATIVE_X_RENDERER_HOOK(8217F820, DrawVerticesUP)  // BeginVertices, memcpy, inline end [+48] = [+13444] at 8217F858
NATIVE_X_RENDERER_HOOK(8217F388, BeginVertices)  // stride byte +12536, fetch 0x48BA, saves +13444/+13448/+13456; only nested in DrawVerticesUP

// Render targets, clears, resolves, tiling, predication, viewport, scissor
NATIVE_X_RENDERER_HOOK(82163C20, SetRenderTarget)  // slot +12448 + 4 * index, [surface+28] to +10372; the engine calls the alias 82163C18
NATIVE_X_RENDERER_HOOK(82163F70, SetDepthStencilSurface)  // +12464, [surface+28] to +10376, [surface+32] to +10560
NATIVE_X_RENDERER_HOOK(8217F260, ClearF)  // NULL rect becomes [surface+36] or +13188/+13192, body 8217EBB0
NATIVE_X_RENDERER_HOOK(821685D0, Resolve)  // RB_COPY shadow +10776..+10788, dest fetch dest+28..+48
NATIVE_X_RENDERER_HOOK(82172B98, BeginTiling)  // count +12756, rects +12760, flags +13196, ClearF at 82172D50
NATIVE_X_RENDERER_HOOK(82172F10, EndTiling)  // Resolve per tile at 8217306C/8217309C, SetPredication, replay 8217D838
NATIVE_X_RENDERER_HOOK(82172A28, SetPredication)  // +12716, +12724, +10932, SET_BIN_MASK_LO at 82172B6C
NATIVE_X_RENDERER_HOOK(82163B08, SetViewport)  // D3DVIEWPORT9 integers to floats, then 821614A0 stores +12656..+12676
NATIVE_X_RENDERER_HOOK(82163A10, SetScissorRect)  // +12684..+12696, clipped by +11864, PA_SC_WINDOW_SCISSOR +10436/+10440

// Shaders, vertex input, textures, constants
NATIVE_X_RENDERER_HOOK(8217B540, SetVertexShader)  // +12704, literal block vs + 872 + [vs+892]
NATIVE_X_RENDERER_HOOK(8217B198, SetPixelShader)  // +12700, +16 |= 0x100000 and 0x20000
NATIVE_X_RENDERER_HOOK(8217B710, SetVertexDeclaration)  // stw r4,11824(r3); +16 |= 0x80000
NATIVE_X_RENDERER_HOOK(821644B0, SetStreamSource)  // short form: vertex fetch 95 - stream only, no stride, no +12468
NATIVE_X_RENDERER_HOOK(82163B88, SetIndices)  // +12444, old buffer fenced with [+10908]
NATIVE_X_RENDERER_HOOK(82172788, SetTexture)  // +12552 + 4 * i, fetch +1152 + 24 * i from texture+28, +24 |= r6
NATIVE_X_RENDERER_HOOK(8217AD60, SetVertexShaderConstantF)  // +1920 + 16 * start, +0 |= r7
NATIVE_X_RENDERER_HOOK(8217AE38, SetPixelShaderConstantF)  // +6016 + 16 * start, +8 |= r7

// Resources
NATIVE_X_RENDERER_HOOK(821722D0, TextureUnlock)  // b 821761B0(tex, [tex+32] & ~0xFFF, [tex+48] & ~0xFFF)
NATIVE_X_RENDERER_HOOK(82178310, FlushCpuRange)  // dcbf over [start, end) in 128 byte lines, sync, skips 0x7F100000..0x86FFFFFF

// Command buffer, queries, frame
NATIVE_X_RENDERER_HOOK(821658B0, KickSegment)  // +13232 and +10940 & 0x80 checks, kick tail 82165528 continues aligned to 32 bytes
NATIVE_X_RENDERER_HOOK(82164CF8, NewSegment)  // (dwords + 14) * 4, +14928, +48 = start - 4, +52, +56 = end - 160
NATIVE_X_RENDERER_HOOK(8217B898, SetShaderGPRAllocation)  // +10920 (defaults 64/64), INDIRECT_BUFFER to +13784/+13796/+14816
NATIVE_X_RENDERER_HOOK(82173830, QueryIssue)  // [q+0] device, tiles [q+148], ZPD packets 8217DBC0, marker 0xFFFFFEED
NATIVE_X_RENDERER_HOOK(82173AB8, QueryGetData)  // END fence [q+24] against [[+10896]+0], byte reversed sums
NATIVE_X_RENDERER_HOOK(82160310, Swap)  // interval +13596, threshold +11860, GPU callback 82160160; Synchronize is 82160848
NATIVE_X_RENDERER_HOOK(82160850, Present)  // +16560 += 1, frontBuffer+28 copied, VdSwap at 82160B0C, NewSegment at 82160F3C

// Command buffers (bf1943 branch of NativeXRenderer: kReplay)
NATIVE_X_RENDERER_HOOK(82177CF8, RunCommandBuffer)  // [buffer] from +13516; kicks, then submits the recorded segments (82165788)
