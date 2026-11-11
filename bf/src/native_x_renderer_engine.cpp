// Battlefield 1943's engine side of NativeXRenderer (docs/native-x-renderer-bf1943.md, section 5).
//
// The engine's renderer has a config object, [[renderer+672]+44], with two bytes that take its draws around the XDK's
// Direct3D state: +200, the GPU ownership path (shaders through GpuLoadShaders, sub_821802A8, and constants written into
// the command buffer after GpuBeginShaderConstantF4, sub_82180220, returns, both outside any call NativeXRenderer
// hooks), and +208, which runs the draw lists recorded on command buffer devices on other threads
// (RunCommandBuffer, sub_82177CF8). bf_engine_d3d_path clears +200 at the start of each draw list (sub_825B8ED0,
// renderer = r3), so the engine sets shaders and constants with SetVertexShader / SetPixelShader and the constant
// setters, as its other path does: calls NativeXRenderer hooks (run nxr3: 230 shader sets and 1,556 constant calls a
// frame instead of 34 and 44). +208 stays: cleared, the recorded lists (the island's terrain and buildings in Wake
// Island) were not drawn at all (run nxr3); the recorded command buffers need NativeXRenderer support instead.

#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(bf_engine_d3d_path, false, "BF1943/Renderer",
                    "The engine sets shaders and constants through the XDK Direct3D calls (no GPU ownership "
                    "path), as NativeXRenderer needs");

namespace {
uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
}  // namespace

REX_EXTERN(__imp__sub_825B8ED0);
REX_HOOK_RAW(sub_825B8ED0) {
  if (REXCVAR_GET(bf_engine_d3d_path)) {
    const uint32_t renderer = ctx.r3.u32;
    const uint32_t holder = renderer ? Load32(base, renderer + 672) : 0;
    const uint32_t config = holder ? Load32(base, holder + 44) : 0;
    if (config && base[config + 200]) {
      REXLOG_INFO("BF renderer config {:08X}: GPU ownership path turned off", config);
      base[config + 200] = 0;
    }
  }
  __imp__sub_825B8ED0(ctx, base);
}
