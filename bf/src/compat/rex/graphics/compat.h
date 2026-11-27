// Battlefield 1943 Reborn, SDK v0.11+ port (phase 5): v0.10's rex::graphics names for NativeXRenderer.
//
// v0.11 moved the Xenos definitions (registers, formats, fetch constants, texture layout) to rex::xenos (public
// headers, the static library rexxenos) and the GPU's code (shaders and their SPIR-V translator, the texture cache,
// the primitive processor, shared memory) to rex::gpu (private headers, the static library rexgpu-xenos-base, which
// NativeXRenderer links). The old names here point at the new ones; the v0.10 header paths include this.
#pragma once

#include <cstdint>
#include <utility>

#include <rex/xenos/fetch.h>
#include <rex/xenos/format.h>
#include <rex/xenos/register.h>
#include <rex/xenos/state.h>
#include <rex/xenos/texture.h>
#include <rex/xenos/ucode.h>

namespace rex::gpu {}

namespace rex::graphics {

using namespace ::rex::gpu;

namespace xenos {
using namespace ::rex::xenos;
using xe_gpu_texture_fetch_t = ::rex::xenos::TextureFetch;
using xe_gpu_vertex_fetch_t = ::rex::xenos::VertexFetch;
}  // namespace xenos
// v0.10's swizzle macros (v0.11: rex::xenos::REXGPU_SWIZZLE_*).
#define XE_GPU_TEXTURE_SWIZZLE_RRRR (::rex::xenos::REXGPU_SWIZZLE_RRRR)
#define XE_GPU_TEXTURE_SWIZZLE_RGGG (::rex::xenos::REXGPU_SWIZZLE_RGGG)
#define XE_GPU_TEXTURE_SWIZZLE_RGBB (::rex::xenos::REXGPU_SWIZZLE_RGBB)
#define XE_GPU_TEXTURE_SWIZZLE_RGBA (::rex::xenos::REXGPU_SWIZZLE_RGBA)

namespace reg = ::rex::xenos::reg;
#define XE_GPU_MAKE_TEXTURE_SWIZZLE(x, y, z, w) REXGPU_SWIZZLE(x, y, z, w)
using ::rex::xenos::FormatInfo;
using ::rex::xenos::RegisterFile;

// v0.10's texture_util functions are rex::xenos's in v0.11; its TextureGuestLayout is GuestTextureLayout.
namespace texture_util {
using namespace ::rex::xenos;
using TextureGuestLayout = ::rex::xenos::GuestTextureLayout;
}  // namespace texture_util

// v0.10's register constants (XE_GPU_REG_*) are the enumerators of rex::xenos::Register in v0.11.
#define BF_COMPAT_REG(name) \
  inline constexpr uint32_t XE_GPU_REG_##name = std::to_underlying(::rex::xenos::Register::name);
BF_COMPAT_REG(PA_SC_WINDOW_OFFSET)
BF_COMPAT_REG(PA_SC_WINDOW_SCISSOR_BR)
BF_COMPAT_REG(PA_SC_WINDOW_SCISSOR_TL)
BF_COMPAT_REG(RB_ALPHA_REF)
BF_COMPAT_REG(RB_BLENDCONTROL0)
BF_COMPAT_REG(RB_BLEND_ALPHA)
BF_COMPAT_REG(RB_BLEND_BLUE)
BF_COMPAT_REG(RB_BLEND_GREEN)
BF_COMPAT_REG(RB_BLEND_RED)
BF_COMPAT_REG(RB_DEPTHCONTROL)
BF_COMPAT_REG(RB_STENCILREFMASK)
BF_COMPAT_REG(RB_STENCILREFMASK_BF)
BF_COMPAT_REG(RB_SURFACE_INFO)
BF_COMPAT_REG(SHADER_CONSTANT_000_X)
BF_COMPAT_REG(SHADER_CONSTANT_256_X)
BF_COMPAT_REG(SHADER_CONSTANT_BOOL_000_031)
BF_COMPAT_REG(SHADER_CONSTANT_FETCH_00_0)
BF_COMPAT_REG(VGT_DMA_BASE)
BF_COMPAT_REG(VGT_DMA_SIZE)
BF_COMPAT_REG(VGT_DRAW_INITIATOR)
BF_COMPAT_REG(VGT_HOS_MAX_TESS_LEVEL)
BF_COMPAT_REG(VGT_HOS_MIN_TESS_LEVEL)
BF_COMPAT_REG(VGT_INDX_OFFSET)
BF_COMPAT_REG(VGT_MAX_VTX_INDX)
BF_COMPAT_REG(VGT_MIN_VTX_INDX)
#undef BF_COMPAT_REG

}  // namespace rex::graphics
