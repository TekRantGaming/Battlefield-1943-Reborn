// NativeXRenderer - see guest_textures.h.

#include "guest_textures.h"

#include "guest_memory.h"
#include "spirv_patch.h"
#include "targets.h"

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/hash.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/math.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <atomic>
#include <unordered_map>
#include <vector>

#include <xxhash.h>

#include <native_x_renderer/native_x_renderer.h>

#include "GraphicsAccessories.hpp"
#include "Shader.h"

REXCVAR_DEFINE_BOOL(native_x_renderer_hd_textures, false, "NativeXRenderer",
                    "HD textures (D149): the game's textures drawn from the HD pack (native_x_renderer_hd_pack) where "
                    "it has them. Read when a texture loads; off is read every frame");
REXCVAR_DEFINE_STRING(native_x_renderer_hd_pack, "", "NativeXRenderer",
                      "The HD texture pack's file (\"BFHD\", tools\\hdpack\\make_pack.py), read once");

namespace native_x_renderer {

namespace {
// The HD texture pack (D149): its index, read once; each texture's data read when a game texture with its key loads.
// The key: XXH3 of the texture's first 64 KB in guest memory (the game's .res data as it is), xor its guest format
// << 56, width << 32 and height << 16.
struct HdEntry {
  uint32_t guest_format = 0, width = 0, height = 0, hd_width = 0, hd_height = 0, mips = 0, bc = 0;
  uint64_t offset = 0, size = 0;
};
class HdPack {
 public:
  bool Open(const std::string& path) {
    file_.open(path, std::ios::binary);
    if (!file_) return false;
    char head[16];
    if (!file_.read(head, 16) || std::memcmp(head, "BFHD", 4) != 0) return false;
    uint32_t version, count;
    std::memcpy(&version, head + 4, 4);
    std::memcpy(&count, head + 8, 4);
    if (version != 1 || count > 1000000) return false;
    std::vector<uint8_t> index(size_t(count) * 56);
    if (!file_.read(reinterpret_cast<char*>(index.data()), std::streamsize(index.size()))) return false;
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* e = &index[size_t(i) * 56];
      uint64_t key;
      HdEntry entry;
      uint32_t u[9];
      std::memcpy(&key, e, 8);
      std::memcpy(u, e + 8, 36);
      std::memcpy(&entry.offset, e + 40, 8);
      std::memcpy(&entry.size, e + 48, 8);
      entry.guest_format = u[0], entry.width = u[1], entry.height = u[2], entry.hd_width = u[3];
      entry.hd_height = u[4], entry.mips = u[5], entry.bc = u[6];
      // Sizes a pack can hold (16384 texels a side, 16 levels, 1 GB an entry): another entry is left out, so no size
      // worked out from it can wrap (the review of pull request 16: a crafted pack crashed the game at every start).
      if (entry.hd_width == 0 || entry.hd_height == 0 || entry.hd_width > 16384 || entry.hd_height > 16384 ||
          entry.mips == 0 || entry.mips > 16 || entry.size > (uint64_t(1) << 30))
        continue;
      entries_[key] = entry;
    }
    return true;
  }
  const HdEntry* Find(uint64_t key) const {
    auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : &it->second;
  }
  bool Read(const HdEntry& e, std::vector<uint8_t>& out) {
    std::lock_guard lock(mutex_);
    out.resize(size_t(e.size));
    file_.clear();
    file_.seekg(std::streamoff(e.offset));
    return bool(file_.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())));
  }
  size_t count() const { return entries_.size(); }

 private:
  std::ifstream file_;
  std::mutex mutex_;
  std::unordered_map<uint64_t, HdEntry> entries_;
};

HdPack* HdPackOpen() {
  static std::once_flag once;
  static std::unique_ptr<HdPack> pack;
  std::call_once(once, [] {
    const std::string path = REXCVAR_GET(native_x_renderer_hd_pack);
    if (path.empty()) return;
    auto p = std::make_unique<HdPack>();
    if (p->Open(path)) {
      REXLOG_INFO("[native_x_renderer] HD textures: {} in the pack {} (D149)", p->count(), path);
      pack = std::move(p);
    } else {
      REXLOG_WARN("[native_x_renderer] HD textures: the pack {} could not be read", path);
    }
  });
  return pack.get();
}
std::atomic<TexturePatchFn> g_texture_patcher{nullptr};  // D142
std::atomic<uint32_t> g_repatch_count{0};                // D160: RepatchTextures calls
}  // namespace

// The SDK's precompiled SPIR-V texture load shaders (generated with Xenia's `xb buildshaders`).
namespace load_shaders {
#include "vulkan_spirv/texture_load_128bpb_cs.h"
#include "vulkan_spirv/texture_load_16bpb_cs.h"
#include "vulkan_spirv/texture_load_32bpb_cs.h"
#include "vulkan_spirv/texture_load_64bpb_cs.h"
#include "vulkan_spirv/texture_load_8bpb_cs.h"
#include "vulkan_spirv/texture_load_bgrg8_rgb8_cs.h"
#include "vulkan_spirv/texture_load_ctx1_cs.h"
#include "vulkan_spirv/texture_load_depth_float_cs.h"
#include "vulkan_spirv/texture_load_depth_unorm_cs.h"
#include "vulkan_spirv/texture_load_dxn_rg8_cs.h"
#include "vulkan_spirv/texture_load_dxt1_rgba8_cs.h"
#include "vulkan_spirv/texture_load_dxt3_rgba8_cs.h"
#include "vulkan_spirv/texture_load_dxt3a_cs.h"
#include "vulkan_spirv/texture_load_dxt5_rgba8_cs.h"
#include "vulkan_spirv/texture_load_dxt5a_r8_cs.h"
#include "vulkan_spirv/texture_load_gbgr8_rgb8_cs.h"
#include "vulkan_spirv/texture_load_r10g11b11_rgba16_cs.h"
#include "vulkan_spirv/texture_load_r11g11b10_rgba16_cs.h"
#include "vulkan_spirv/texture_load_r4g4b4a4_rgba8_cs.h"
#include "vulkan_spirv/texture_load_r5g5b5a1_b5g5r5a1_cs.h"
#include "vulkan_spirv/texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs.h"
#include "vulkan_spirv/texture_load_r5g6b5_rgba8_cs.h"
}  // namespace load_shaders

using namespace Diligent;
namespace rg = rex::graphics;
namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;

namespace {

using rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
using rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB;
using rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG;
using rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR;

// Swaps red and blue in a host format swizzle: for formats whose DiligentCore Vulkan format stores them in
// the opposite order to the DXGI format the load shader writes (RGB10A2_UNORM is A2R10G10B10 there).
constexpr uint32_t SwapRedBlue(uint32_t swizzle) {
  uint32_t result = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t component = (swizzle >> (3 * i)) & 0b111;
    if (component == 0) {
      component = 2;
    } else if (component == 2) {
      component = 0;
    }
    result |= component << (3 * i);
  }
  return result;
}

// Names for the load shaders' resources (their SPIR-V is stripped): set 0 is the destination, set 1 the
// source, both binding 0 (the SDK's Vulkan backend, kLoadDescriptorSetIndex*), plus the push constants.
const std::vector<spirv::ResourceName> kLoadShaderNames = {
    {0, 0, false, "XeTextureLoadDest"},
    {1, 0, false, "XeTextureLoadSource"},
    {0, 0, true, "XeTextureLoadConstants"},
};

}  // namespace

// Indexed by xenos::TextureFormat. The SDK's D3D12 table (DiligentCore's formats are DXGI's), with these
// changes:
//   - formats whose DiligentCore Vulkan mapping does not match the DXGI layout: 2_10_10_10 keeps the format
//     and swaps red and blue in the swizzle; 5_6_5 and 4_4_4_4 are expanded to RGBA8 (as the SDK's Vulkan
//     backend does when the 16-bit formats are unsupported); DXT3A_AS_1_1_1_1 is not supported yet;
//   - Y'CbCr formats use their RGB8 decompression.
// 1_5_5_5 is the D3D12 table's B5G5R5A1 (D105): with NativeXRenderer's DiligentCore patch 0008, B5G5R5A1_UNORM is
// Vulkan's A1R5G5B5_UNORM_PACK16, Direct3D's bit layout, as in the SDK's Vulkan backend.
// The signed formats are those the SDK's Vulkan backend reads from the same bits through a view of another format
// (D85): 8- and 16-bit normalized components. The others the SDK loads into a separate signed texture
// (10_11_11, 11_11_10) or does not support signed: their signed bindings read the unsigned data.
// DXT1 stays compressed as BC1 (D33): with NativeXRenderer's DiligentCore patch 0002, BC1_UNORM is Vulkan's BC1
// with 1-bit alpha, as in Direct3D.
// Block-compressed formats fall back to decompression when their size is not a multiple of the block, or
// when the device has no BC formats (GetHostFormat).
const GuestTextures::HostFormat GuestTextures::kHostFormats[64] = {
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_1_REVERSE
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_1
    {TEX_FORMAT_R8_UNORM, kLoadShaderIndex8bpb, false, XE_GPU_TEXTURE_SWIZZLE_RRRR, TEX_FORMAT_R8_SNORM},  // k_8
    {TEX_FORMAT_B5G5R5A1_UNORM, kLoadShaderIndexR5G5B5A1ToB5G5R5A1, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_1_5_5_5
    {TEX_FORMAT_RGBA8_UNORM, kLoadShaderIndexR5G6B5ToRGBA8, false, XE_GPU_TEXTURE_SWIZZLE_RGBB},  // k_5_6_5
    // k_6_5_5: green bits in blue, blue in green (DXGI B5G6R5 is Vulkan R5G6B5; DiligentCore's B5G6R5 is
    // Vulkan B5G6R5): red and blue swapped on top.
    {TEX_FORMAT_B5G6R5_UNORM, kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle, false,
     SwapRedBlue(XE_GPU_MAKE_TEXTURE_SWIZZLE(R, B, G, G))},
    {TEX_FORMAT_RGBA8_UNORM, kLoadShaderIndex32bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGBA,
     TEX_FORMAT_RGBA8_SNORM},  // k_8_8_8_8
    {TEX_FORMAT_RGB10A2_UNORM, kLoadShaderIndex32bpb, false, SwapRedBlue(XE_GPU_TEXTURE_SWIZZLE_RGBA)},  // k_2_10_10_10
    {TEX_FORMAT_R8_UNORM, kLoadShaderIndex8bpb, false, XE_GPU_TEXTURE_SWIZZLE_RRRR, TEX_FORMAT_R8_SNORM},  // k_8_A
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_8_B
    {TEX_FORMAT_RG8_UNORM, kLoadShaderIndex16bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGGG, TEX_FORMAT_RG8_SNORM},  // k_8_8
    {TEX_FORMAT_RGBA8_UNORM, kLoadShaderIndexGBGR8ToRGB8, false, XE_GPU_TEXTURE_SWIZZLE_RGBB},  // k_Cr_Y1_Cb_Y0_REP
    {TEX_FORMAT_RGBA8_UNORM, kLoadShaderIndexBGRG8ToRGB8, false, XE_GPU_TEXTURE_SWIZZLE_RGBB},  // k_Y1_Cr_Y0_Cb_REP
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_16_16_EDRAM
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_8_8_8_8_A
    {TEX_FORMAT_RGBA8_UNORM, kLoadShaderIndexRGBA4ToRGBA8, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_4_4_4_4
    {TEX_FORMAT_RGBA16_UNORM, kLoadShaderIndexR11G11B10ToRGBA16, false, XE_GPU_TEXTURE_SWIZZLE_RGBB},  // k_10_11_11
    {TEX_FORMAT_RGBA16_UNORM, kLoadShaderIndexR10G11B11ToRGBA16, false, XE_GPU_TEXTURE_SWIZZLE_RGBB},  // k_11_11_10
    {TEX_FORMAT_BC1_UNORM, kLoadShaderIndex64bpb, true, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_DXT1
    {TEX_FORMAT_BC2_UNORM, kLoadShaderIndex128bpb, true, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_DXT2_3
    {TEX_FORMAT_BC3_UNORM, kLoadShaderIndex128bpb, true, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_DXT4_5
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_16_16_16_16_EDRAM
    {TEX_FORMAT_R32_FLOAT, kLoadShaderIndexDepthUnorm, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_24_8
    {TEX_FORMAT_R32_FLOAT, kLoadShaderIndexDepthFloat, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_24_8_FLOAT
    {TEX_FORMAT_R16_UNORM, kLoadShaderIndex16bpb, false, XE_GPU_TEXTURE_SWIZZLE_RRRR, TEX_FORMAT_R16_SNORM},  // k_16
    {TEX_FORMAT_RG16_UNORM, kLoadShaderIndex32bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGGG,
     TEX_FORMAT_RG16_SNORM},  // k_16_16
    {TEX_FORMAT_RGBA16_UNORM, kLoadShaderIndex64bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGBA,
     TEX_FORMAT_RGBA16_SNORM},  // k_16_16_16_16
    {TEX_FORMAT_R16_FLOAT, kLoadShaderIndex16bpb, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_16_EXPAND
    {TEX_FORMAT_RG16_FLOAT, kLoadShaderIndex32bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_16_16_EXPAND
    {TEX_FORMAT_RGBA16_FLOAT, kLoadShaderIndex64bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_16_16_16_16_EXPAND
    {TEX_FORMAT_R16_FLOAT, kLoadShaderIndex16bpb, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_16_FLOAT
    {TEX_FORMAT_RG16_FLOAT, kLoadShaderIndex32bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_16_16_FLOAT
    {TEX_FORMAT_RGBA16_FLOAT, kLoadShaderIndex64bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_16_16_16_16_FLOAT
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_32
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_32_32
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_32_32_32_32
    {TEX_FORMAT_R32_FLOAT, kLoadShaderIndex32bpb, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_32_FLOAT
    {TEX_FORMAT_RG32_FLOAT, kLoadShaderIndex64bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_32_32_FLOAT
    {TEX_FORMAT_RGBA32_FLOAT, kLoadShaderIndex128bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_32_32_32_32_FLOAT
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_32_AS_8
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_32_AS_8_8
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_16_MPEG
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_16_16_MPEG
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_8_INTERLACED
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_32_AS_8_INTERLACED
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_32_AS_8_8_INTERLACED
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_16_INTERLACED
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_16_MPEG_INTERLACED
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_16_16_MPEG_INTERLACED
    {TEX_FORMAT_BC5_UNORM, kLoadShaderIndex128bpb, true, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_DXN
    {TEX_FORMAT_RGBA8_UNORM, kLoadShaderIndex32bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGBA,
     TEX_FORMAT_RGBA8_SNORM},  // k_8_8_8_8_AS_16_16_16_16
    {TEX_FORMAT_BC1_UNORM, kLoadShaderIndex64bpb, true, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_DXT1_AS_16_16_16_16
    {TEX_FORMAT_BC2_UNORM, kLoadShaderIndex128bpb, true, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_DXT2_3_AS_16_16_16_16
    {TEX_FORMAT_BC3_UNORM, kLoadShaderIndex128bpb, true, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_DXT4_5_AS_16_16_16_16
    {TEX_FORMAT_RGB10A2_UNORM, kLoadShaderIndex32bpb, false, SwapRedBlue(XE_GPU_TEXTURE_SWIZZLE_RGBA)},  // k_2_10_10_10_AS_16_16_16_16
    {TEX_FORMAT_RGBA16_UNORM, kLoadShaderIndexR11G11B10ToRGBA16, false, XE_GPU_TEXTURE_SWIZZLE_RGBB},  // k_10_11_11_AS_16_16_16_16
    {TEX_FORMAT_RGBA16_UNORM, kLoadShaderIndexR10G11B11ToRGBA16, false, XE_GPU_TEXTURE_SWIZZLE_RGBB},  // k_11_11_10_AS_16_16_16_16
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGBB},  // k_32_32_32_FLOAT
    {TEX_FORMAT_R8_UNORM, kLoadShaderIndexDXT3A, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_DXT3A
    {TEX_FORMAT_BC4_UNORM, kLoadShaderIndex64bpb, true, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_DXT5A
    {TEX_FORMAT_RG8_UNORM, kLoadShaderIndexCTX1, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_CTX1
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_DXT3A_AS_1_1_1_1
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_8_8_8_8_GAMMA_EDRAM
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_2_10_10_10_FLOAT_EDRAM
};

const GuestTextures::HostFormat& GuestTextures::GetHostFormat(TextureKey key) const {
  const HostFormat& host_format = kHostFormats[uint32_t(key.format)];
  if (!host_format.block_compressed) return host_format;
  // Decompressed when the size is not a multiple of the block (as the SDK's D3D12 backend does), or without
  // BC formats on the device (as its Vulkan backend does).
  const rg::FormatInfo* info = rg::FormatInfo::Get(key.format);
  if (block_compression_ && !(key.GetWidth() & (info->block_width - 1)) &&
      !(key.GetHeight() & (info->block_height - 1))) {
    return host_format;
  }
  static const HostFormat kDXT1{TEX_FORMAT_RGBA8_UNORM, kLoadShaderIndexDXT1ToRGBA8, false,
                               XE_GPU_TEXTURE_SWIZZLE_RGBA};
  static const HostFormat kDXT2_3{TEX_FORMAT_RGBA8_UNORM, kLoadShaderIndexDXT3ToRGBA8, false,
                                  XE_GPU_TEXTURE_SWIZZLE_RGBA};
  static const HostFormat kDXT4_5{TEX_FORMAT_RGBA8_UNORM, kLoadShaderIndexDXT5ToRGBA8, false,
                                  XE_GPU_TEXTURE_SWIZZLE_RGBA};
  static const HostFormat kDXN{TEX_FORMAT_RG8_UNORM, kLoadShaderIndexDXNToRG8, false, XE_GPU_TEXTURE_SWIZZLE_RGGG};
  static const HostFormat kDXT5A{TEX_FORMAT_R8_UNORM, kLoadShaderIndexDXT5AToR8, false,
                                 XE_GPU_TEXTURE_SWIZZLE_RRRR};
  switch (key.format) {
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT1_AS_16_16_16_16:
      return kDXT1;
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16:
      return kDXT2_3;
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16:
      return kDXT4_5;
    case xenos::TextureFormat::k_DXN:
      return kDXN;
    case xenos::TextureFormat::k_DXT5A:
      return kDXT5A;
    default:
      return host_format;
  }
}

// One guest texture on the host: a 2D array (1D, 2D, stacked), 3D or cube texture with its mips.
class GuestTextures::HostTexture final : public rg::TextureCache::Texture {
 public:
  HostTexture(GuestTextures& cache, const TextureKey& key, RefCntAutoPtr<ITexture> texture, uint64_t size,
              TEXTURE_FORMAT signed_format, TEXTURE_FORMAT unsigned_format)
      : Texture(cache, key), texture_(std::move(texture)) {
    view_ = texture_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    if (signed_format != TEX_FORMAT_UNKNOWN) {
      TextureViewDesc desc;
      desc.ViewType = TEXTURE_VIEW_SHADER_RESOURCE;
      desc.Format = signed_format;
      texture_->CreateView(desc, &signed_view_);
      // A typeless texture's default view is not always the unsigned reading: DiligentCore's for R16_TYPELESS (and the
      // other 16-bit typeless formats) is the half float one, so a k_16 texture read as half floats (Battlefield 1943's
      // terrain heightmap: 0x3114, 0.1917 unsigned, came out 0.1587, and the terrain stood as a black mountain). The
      // unsigned view is made with its own format.
      if (unsigned_format != TEX_FORMAT_UNKNOWN) {
        desc.Format = unsigned_format;
        texture_->CreateView(desc, &unsigned_view_);
        if (unsigned_view_) view_ = unsigned_view_;
      }
    }
    SetHostMemoryUsage(size);
    ++cache.stats_.textures;
    cache.stats_.host_bytes += size;
  }
  ~HostTexture() override {
    auto& cache = static_cast<GuestTextures&>(texture_cache());
    auto& stats = cache.stats_;
    --stats.textures;
    stats.host_bytes -= GetHostMemoryUsage();
    cache.patched_.erase(this);  // D160
  }
  ITexture* texture() const { return texture_; }
  ITextureView* view() const { return view_; }
  // The bits read as signed normalized values (D85); null when the format has no such reading.
  ITextureView* signed_view() const { return signed_view_; }

  // The guest data of the last load.
  std::optional<uint64_t> base_hash, mips_hash;
  // The HD pack's texture for that data (D149), or null.
  ITextureView* hd_view = nullptr;
  // The HD picture with the patcher's pictures drawn in (Patch), this texture's own: `hd_view` is then its view.
  RefCntAutoPtr<ITexture> hd_patched;
  // D160: to be offered to the patcher again when its data is next found unchanged (it was outdated at the request).
  bool repatch = false;

 private:
  RefCntAutoPtr<ITexture> texture_;
  ITextureView* view_ = nullptr;
  RefCntAutoPtr<ITextureView> signed_view_;
  RefCntAutoPtr<ITextureView> unsigned_view_;
};

GuestTextures::GuestTextures(const rg::RegisterFile& registers, GuestMemory& memory, ResolvedTextures& resolved,
                             IRenderDevice* device, IDeviceContext* context)
    : TextureCache(registers, memory, 1, 1),
      memory_(memory),
      resolved_(resolved),
      device_(device),
      context_(context) {}

GuestTextures::~GuestTextures() { DestroyAllTextures(true); }

bool GuestTextures::Initialize() {
  block_compression_ = device_->GetDeviceInfo().Features.TextureCompressionBC == DEVICE_FEATURE_STATE_ENABLED;
  if (!block_compression_) REXLOG_WARN("[native_x_renderer] no BC texture formats: compressed textures are decompressed");
  struct Code {
    LoadShaderIndex index;
    const uint32_t* words;
    size_t bytes;
    const char* name;
  };
#define XN_LOAD_SHADER(index, array) {index, load_shaders::array, sizeof(load_shaders::array), #array}
  const Code codes[] = {
      XN_LOAD_SHADER(kLoadShaderIndex8bpb, texture_load_8bpb_cs),
      XN_LOAD_SHADER(kLoadShaderIndex16bpb, texture_load_16bpb_cs),
      XN_LOAD_SHADER(kLoadShaderIndex32bpb, texture_load_32bpb_cs),
      XN_LOAD_SHADER(kLoadShaderIndex64bpb, texture_load_64bpb_cs),
      XN_LOAD_SHADER(kLoadShaderIndex128bpb, texture_load_128bpb_cs),
      XN_LOAD_SHADER(kLoadShaderIndexR5G5B5A1ToB5G5R5A1, texture_load_r5g5b5a1_b5g5r5a1_cs),
      XN_LOAD_SHADER(kLoadShaderIndexR5G6B5ToRGBA8, texture_load_r5g6b5_rgba8_cs),
      XN_LOAD_SHADER(kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle, texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs),
      XN_LOAD_SHADER(kLoadShaderIndexRGBA4ToRGBA8, texture_load_r4g4b4a4_rgba8_cs),
      XN_LOAD_SHADER(kLoadShaderIndexGBGR8ToRGB8, texture_load_gbgr8_rgb8_cs),
      XN_LOAD_SHADER(kLoadShaderIndexBGRG8ToRGB8, texture_load_bgrg8_rgb8_cs),
      XN_LOAD_SHADER(kLoadShaderIndexR10G11B11ToRGBA16, texture_load_r10g11b11_rgba16_cs),
      XN_LOAD_SHADER(kLoadShaderIndexR11G11B10ToRGBA16, texture_load_r11g11b10_rgba16_cs),
      XN_LOAD_SHADER(kLoadShaderIndexDXT1ToRGBA8, texture_load_dxt1_rgba8_cs),
      XN_LOAD_SHADER(kLoadShaderIndexDXT3ToRGBA8, texture_load_dxt3_rgba8_cs),
      XN_LOAD_SHADER(kLoadShaderIndexDXT5ToRGBA8, texture_load_dxt5_rgba8_cs),
      XN_LOAD_SHADER(kLoadShaderIndexDXNToRG8, texture_load_dxn_rg8_cs),
      XN_LOAD_SHADER(kLoadShaderIndexDXT3A, texture_load_dxt3a_cs),
      XN_LOAD_SHADER(kLoadShaderIndexDXT5AToR8, texture_load_dxt5a_r8_cs),
      XN_LOAD_SHADER(kLoadShaderIndexCTX1, texture_load_ctx1_cs),
      XN_LOAD_SHADER(kLoadShaderIndexDepthUnorm, texture_load_depth_unorm_cs),
      XN_LOAD_SHADER(kLoadShaderIndexDepthFloat, texture_load_depth_float_cs),
  };
#undef XN_LOAD_SHADER
  for (const Code& code : codes) {
    std::vector<uint32_t> spirv = spirv::PrepareForDiligent(code.words, code.bytes / 4, kLoadShaderNames);
    ShaderCreateInfo shader_info;
    shader_info.Desc.Name = code.name;
    shader_info.Desc.ShaderType = SHADER_TYPE_COMPUTE;
    shader_info.EntryPoint = "main";
    shader_info.ByteCode = spirv.data();
    shader_info.ByteCodeSize = spirv.size() * 4;
    RefCntAutoPtr<IShader> shader;
    device_->CreateShader(shader_info, &shader);
    if (!shader) {
      REXLOG_ERROR("[native_x_renderer] texture load shader {}: DiligentCore did not accept it", code.name);
      continue;
    }
    ComputePipelineStateCreateInfo pso_info;
    std::string pso_name = std::string("NativeXRenderer ") + code.name;
    pso_info.PSODesc.Name = pso_name.c_str();
    pso_info.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
    pso_info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
    pso_info.pCS = shader;
    LoadPipeline& pipeline = load_pipelines_[code.index];
    device_->CreateComputePipelineState(pso_info, &pipeline.pso);
    if (!pipeline.pso) {
      REXLOG_ERROR("[native_x_renderer] texture load pipeline {}: DiligentCore did not create it", code.name);
      continue;
    }
    pipeline.pso->CreateShaderResourceBinding(&pipeline.srb, true);
    pipeline.dest = pipeline.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "XeTextureLoadDest");
    pipeline.source = pipeline.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "XeTextureLoadSource");
    pipeline.constants = pipeline.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "XeTextureLoadConstants");
    if (!pipeline.dest || !pipeline.source || !pipeline.constants) {
      REXLOG_ERROR("[native_x_renderer] texture load pipeline {}: resources not found by name", code.name);
      pipeline = {};
    }
  }
  return true;
}

void GuestTextures::RequestTextures(uint32_t used_texture_mask) {
  // D160: the patcher's textures offered again after RepatchTextures, before this draw binds anything. Only one whose
  // data is known unchanged since its load: one not outdated (its memory watch has not fired). An outdated one is
  // offered at its next load (the patcher's first look at the new data) or, when its data is found unchanged there,
  // again (LoadTextureDataFromResidentMemoryImpl).
  if (const uint32_t repatch = g_repatch_count.load(std::memory_order_acquire); repatch != repatched_) {
    repatched_ = repatch;
    const std::vector<HostTexture*> textures(patched_.begin(), patched_.end());
    for (HostTexture* texture : textures) {
      if (texture->outdated_mask() & Texture::kOutdatedBitBase) texture->repatch = true;
      else Patch(*texture, true);
    }
  }
  // Marking all 32 written computed every used binding again in every draw (BindingInfoFromFetchConstant, the
  // host format's swizzle): about 5 % of the renderer thread in Kameo's first level (D83).
  const uint32_t* fetch_constants = &register_file()[rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0];
  uint32_t remaining = used_texture_mask;
  uint32_t index;
  while (rex::bit_scan_forward(remaining, &index)) {
    remaining &= ~(UINT32_C(1) << index);
    const uint32_t* fetch = fetch_constants + index * 6;
    std::array<uint32_t, 6>& seen = seen_[index];
    if ((seen_mask_ & (UINT32_C(1) << index)) && !std::memcmp(seen.data(), fetch, sizeof(seen))) continue;
    std::memcpy(seen.data(), fetch, sizeof(seen));
    seen_mask_ |= UINT32_C(1) << index;
    TextureFetchConstantWritten(index);
  }
  // The texture data's pages stay watched: the cache learns of the game's writes there through the watches (D97).
  memory_.BeginTextureRequests();
  TextureCache::RequestTextures(used_texture_mask);
  memory_.EndTextureRequests();
}

void GuestTextures::UpdateTextureBindingsImpl(uint32_t fetch_constant_mask) {
  (void)fetch_constant_mask;
  ++generation_;
}

ITextureView* GuestTextures::GetView(uint32_t fetch_constant, xenos::FetchOpDimension dimension, bool is_signed,
                                     bool& other_dimension) {
  other_dimension = false;
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant);
  if (!binding || !binding->texture) return nullptr;
  xenos::DataDimension data = binding->key.dimension;
  bool compatible;
  switch (dimension) {
    case xenos::FetchOpDimension::k3DOrStacked:
      compatible = data == xenos::DataDimension::k3D;
      break;
    case xenos::FetchOpDimension::kCube:
      compatible = data == xenos::DataDimension::kCube;
      break;
    default:  // 1D and 2D bindings sample 2D arrays
      compatible = data == xenos::DataDimension::k1D || data == xenos::DataDimension::k2DOrStacked;
      break;
  }
  if (!compatible) {
    other_dimension = true;
    return nullptr;
  }
  auto* texture = static_cast<HostTexture*>(binding->texture);
  texture->MarkAsUsed();
  // A signed binding reads the same bits as signed normalized values, as the SDK's Vulkan backend does (D85). The
  // unsigned view there turned negative values into large positive ones: Kameo's smoke distortion map (8_8, signed)
  // jumped from -0.01 to +1.0 across its zero crossings.
  if (is_signed && texture->signed_view()) return texture->signed_view();
  if (texture->hd_view && !is_signed && REXCVAR_GET(native_x_renderer_hd_textures)) return texture->hd_view;
  return texture->view();
}

// D149: the HD pack's texture for this game texture's data as it is now, made the first time (kept for the session,
// by its key), or null. DXT1, DXT3 and DXT5 2D textures whose first level is at least 64 KB.
// 13 Nov 2026 (Battlefield 1943's menu and HUD pictures): 8_8_8_8 textures too (the pack's block format 0: RGBA8, not
// compressed), and first levels from 4 KB up; the key is then of the whole first level (of its first 64 KB above).
ITextureView* GuestTextures::HdView(HostTexture& host_texture) {
  if (!REXCVAR_GET(native_x_renderer_hd_textures)) return nullptr;
  const TextureKey key = host_texture.key();
  const uint32_t format = uint32_t(key.format), w = key.GetWidth(), h = key.GetHeight();
  if ((format != 18 && format != 19 && format != 20 && format != 6) ||
      key.dimension != xenos::DataDimension::k2DOrStacked || key.GetDepthOrArraySize() != 1 ||
      host_texture.GetGuestBaseSize() < 4096) {
    return nullptr;
  }
  const size_t hashed = size_t(std::min<uint64_t>(host_texture.GetGuestBaseSize(), 65536));
  HdPack* pack = HdPackOpen();
  if (!pack) return nullptr;
  const uint8_t* guest = memory_.memory().TranslatePhysical<const uint8_t*>(key.base_page << 12);
  const uint64_t content = XXH3_64bits(guest, hashed) ^ (uint64_t(format) << 56) ^ (uint64_t(w) << 32) ^
                           (uint64_t(h) << 16);
  if (auto it = hd_textures_.find(content); it != hd_textures_.end())
    return it->second ? it->second->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
  const HdEntry* e = pack->Find(content);
  // NXR_HD_LOG=1: every texture the pack was asked for and did not have (what a pack's maker has to match).
  static const bool log_misses = [] {
    const char* v = std::getenv("NXR_HD_LOG");
    return v && *v == '1';
  }();
  if (!e && log_misses)
    REXLOG_INFO("[native_x_renderer] HD texture {:016X} not in the pack: {}x{} format {}, base {} bytes at {:08X}, "
                "tiled {}, packed mips {}, levels {}",
                content, w, h, format, host_texture.GetGuestBaseSize(), uint32_t(key.base_page) << 12,
                bool(key.tiled), bool(key.packed_mips), uint32_t(key.mip_max_level) + 1);
  RefCntAutoPtr<ITexture> texture;
  if (e && e->width == w && e->height == h && e->mips > 0 && e->mips <= 14 &&
      (e->bc == 0 || ((e->bc == 1 || e->bc == 3) && block_compression_))) {
    std::vector<uint8_t> data;
    if (pack->Read(*e, data)) {
      const uint32_t block = e->bc == 1 ? 8 : 16;
      const bool plain = e->bc == 0;  // RGBA8, a row after a row
      // The host holds a guest 8_8_8_8 texture's words byte swapped (B, G, R, A) and the draws' swizzle is for that.
      if (plain)
        for (size_t i = 0; i + 3 < data.size(); i += 4) std::swap(data[i], data[i + 2]);
      std::vector<TextureSubResData> levels(e->mips);
      uint64_t at = 0;
      bool fits = true;
      for (uint32_t m = 0; m < e->mips; ++m) {
        const uint32_t mw = std::max(1u, e->hd_width >> m), mh = std::max(1u, e->hd_height >> m);
        const uint64_t stride = plain ? uint64_t(mw) * 4 : uint64_t((mw + 3) / 4) * block;
        const uint64_t bytes = plain ? stride * mh : stride * ((mh + 3) / 4);
        if (at + bytes > data.size()) {
          fits = false;
          break;
        }
        levels[m].pData = data.data() + at;
        levels[m].Stride = stride;
        at += bytes;
      }
      if (fits) {
        TextureDesc desc;
        desc.Name = "NativeXRenderer HD texture";
        desc.Type = RESOURCE_DIM_TEX_2D_ARRAY;
        desc.ArraySize = 1;
        desc.Width = e->hd_width;
        desc.Height = e->hd_height;
        desc.MipLevels = e->mips;
        desc.Format = plain ? TEX_FORMAT_RGBA8_UNORM : e->bc == 1 ? TEX_FORMAT_BC1_UNORM : TEX_FORMAT_BC3_UNORM;
        desc.Usage = USAGE_IMMUTABLE;
        desc.BindFlags = BIND_SHADER_RESOURCE;
        TextureData init{levels.data(), e->mips};
        device_->CreateTexture(desc, &init, &texture);
        if (texture) {
          ++stats_.hd_textures;
          stats_.hd_bytes += data.size();
          if (stats_.hd_textures <= 5 || stats_.hd_textures % 25 == 0 || log_misses)
            REXLOG_INFO("[native_x_renderer] HD texture {:016X}: {}x{} for the game's {}x{} (format {}); {} HD textures "
                        "made, {} MB",
                        content, e->hd_width, e->hd_height, w, h, format, stats_.hd_textures, stats_.hd_bytes >> 20);
        }
      }
    }
  }
  hd_textures_[content] = texture;  // null too: looked for once
  return texture ? texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

void GuestTextures::BeginFrame(uint64_t frame, uint64_t completed_frame) {
  CompletedSubmissionUpdated(completed_frame);
  BeginSubmission(frame);
  TextureCache::BeginFrame();
  ++generation_;  // the textures are marked used again in the new submission (D83)
}

uint32_t GuestTextures::GetHostFormatSwizzle(TextureKey key) const { return GetHostFormat(key).swizzle; }

uint32_t GuestTextures::GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const {
  return dimension == xenos::DataDimension::k3D ? 2048 : 16384;
}

uint32_t GuestTextures::GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const {
  return dimension == xenos::DataDimension::kCube ? 6 : 2048;
}

std::unique_ptr<rg::TextureCache::Texture> GuestTextures::CreateTexture(TextureKey key) {
  const HostFormat& host_format = GetHostFormat(key);
  if (host_format.format == TEX_FORMAT_UNKNOWN || host_format.load_shader == kLoadShaderIndexUnknown ||
      !load_pipelines_[host_format.load_shader].pso) {
    ++stats_.unsupported;
    return nullptr;
  }
  TextureDesc desc;
  std::string name = "NativeXRenderer texture " + std::to_string(key.base_page << 12) + " format " +
                     std::to_string(uint32_t(key.format));
  desc.Name = name.c_str();
  uint32_t depth_or_array_size = key.GetDepthOrArraySize();
  switch (key.dimension) {
    case xenos::DataDimension::k3D:
      desc.Type = RESOURCE_DIM_TEX_3D;
      desc.Depth = depth_or_array_size;
      break;
    case xenos::DataDimension::kCube:
      desc.Type = RESOURCE_DIM_TEX_CUBE;
      desc.ArraySize = 6;
      break;
    default:
      desc.Type = RESOURCE_DIM_TEX_2D_ARRAY;
      desc.ArraySize = depth_or_array_size;
      break;
  }
  desc.Width = key.GetWidth();
  desc.Height = key.GetHeight();
  desc.MipLevels = key.mip_max_level + 1;
  desc.Format = host_format.format;
  // With a signed reading, the typeless format: DiligentCore creates the image with a mutable format, its default
  // view of the unsigned format, and allows a view of the signed one (D85).
  if (host_format.signed_format != TEX_FORMAT_UNKNOWN) {
    switch (host_format.format) {
      case TEX_FORMAT_R8_UNORM:
        desc.Format = TEX_FORMAT_R8_TYPELESS;
        break;
      case TEX_FORMAT_RG8_UNORM:
        desc.Format = TEX_FORMAT_RG8_TYPELESS;
        break;
      case TEX_FORMAT_RGBA8_UNORM:
        desc.Format = TEX_FORMAT_RGBA8_TYPELESS;
        break;
      case TEX_FORMAT_R16_UNORM:
        desc.Format = TEX_FORMAT_R16_TYPELESS;
        break;
      case TEX_FORMAT_RG16_UNORM:
        desc.Format = TEX_FORMAT_RG16_TYPELESS;
        break;
      case TEX_FORMAT_RGBA16_UNORM:
        desc.Format = TEX_FORMAT_RGBA16_TYPELESS;
        break;
      default:
        break;
    }
  }
  desc.Usage = USAGE_DEFAULT;
  desc.BindFlags = BIND_SHADER_RESOURCE;
  RefCntAutoPtr<ITexture> texture;
  device_->CreateTexture(desc, nullptr, &texture);
  if (!texture) {
    REXLOG_ERROR("[native_x_renderer] could not create {} ({}x{}x{}, {} mips)", name, desc.Width, desc.Height,
                 depth_or_array_size, desc.MipLevels);
    return nullptr;
  }
  // The host size, mips included: the SDK's cache destroys unused textures above its memory limits.
  uint64_t size = 0;
  for (uint32_t level = 0; level < desc.MipLevels; ++level) size += GetMipLevelProperties(desc, level).MipSize;
  size *= desc.GetArraySize();
  const bool typeless = desc.Format != host_format.format;
  return std::make_unique<HostTexture>(*this, key, std::move(texture), size,
                                       typeless ? host_format.signed_format : TEX_FORMAT_UNKNOWN,
                                       typeless ? host_format.format : TEX_FORMAT_UNKNOWN);
}

bool GuestTextures::TryLoadTextureDataFromResolveCache(Texture& texture, bool load_base, bool load_mips) {
  (void)load_mips;
  // Resolve destinations hold no data in guest memory (resolves write host textures only): skip the load.
  // GetView's callers bind the resolved texture instead, unless it was invalidated or the format is one a resolve
  // does not write (D70).
  return load_base && ResolvableFormat(uint32_t(texture.key().format)) &&
         resolved_.FindNewest(texture.key().base_page << 12) != nullptr;
}

bool GuestTextures::LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base, bool load_mips) {
  // SharedMemory invalidates a whole 256 KB block around a CPU write (fewer access violations to catch), so
  // CPU data that a game keeps next to a texture and writes every frame makes the texture outdated with its
  // data intact. What did not change is not loaded again.
  //
  // The data compared is what the load reads, the mirror's (by the hashes of its pages' last copies), not guest
  // memory's: the SDK copies the pages to the mirror before this, and a game streaming a texture in from another
  // thread can write the data in between. Hashed from guest memory, the texture then took the new data's hash with
  // the old data (zeros: Battlefield 1943's soldier textures drew black), and every later load found it unchanged.
  auto& host_texture = static_cast<HostTexture&>(texture);
  TextureKey key = host_texture.key();
  uint64_t base_hash = 0, mips_hash = 0;
  if (load_base) {
    base_hash = memory_.MirrorHash(key.base_page << 12, host_texture.GetGuestBaseSize());
    load_base = !base_hash || host_texture.base_hash != base_hash;
  }
  if (load_mips) {
    mips_hash = memory_.MirrorHash(key.mip_page << 12, host_texture.GetGuestMipsSize());
    load_mips = !mips_hash || host_texture.mips_hash != mips_hash;
  }
  if (GuestMemory::TracePage() &&
      (key.base_page == GuestMemory::TracePage() || key.mip_page == GuestMemory::TracePage()) &&
      GuestMemory::TraceLine()) {
    const uint8_t* p = memory_.memory().TranslatePhysical<const uint8_t*>(GuestMemory::TracePage() << 12);
    REXLOG_INFO("[native_x_renderer] trace {:08X}: texture {}x{} format {} base page {:X} mip page {:X}: base hash {:016X} "
                "(had {:016X}) load {}, mips hash {:016X} (had {:016X}) load {}; guest data {:02X}{:02X}{:02X}{:02X}",
                GuestMemory::TracePage() << 12, key.GetWidth(), key.GetHeight(), uint32_t(key.format),
                uint32_t(key.base_page), uint32_t(key.mip_page), base_hash, host_texture.base_hash.value_or(0),
                load_base, mips_hash, host_texture.mips_hash.value_or(0), load_mips, p[0], p[1], p[2], p[3]);
  }
  if (!load_base && !load_mips) {
    ++stats_.unchanged;
    if (host_texture.repatch) Patch(host_texture, true);  // D160: asked for while it was outdated, its data the same
    return true;
  }
  {
    // NXR_TEX_LOADS=1 (a bring-up aid): every load after the first of a texture, with what changed.
    static const bool log_loads = [] {
      const char* v = std::getenv("NXR_TEX_LOADS");
      return v && *v == '1';
    }();
    static uint32_t logged = 0;
    if (log_loads && (host_texture.base_hash || host_texture.mips_hash) && logged < 20000) {
      ++logged;
      REXLOG_INFO("[native_x_renderer] texture reload: base {:08X} mips {:08X} {}x{}x{} format {} levels {} tiled {}: "
                  "base {} ({:016X} -> {:016X}) mips {} ({:016X} -> {:016X})",
                  uint32_t(key.base_page) << 12, uint32_t(key.mip_page) << 12, key.GetWidth(), key.GetHeight(),
                  key.GetDepthOrArraySize(), uint32_t(key.format), uint32_t(key.mip_max_level) + 1, bool(key.tiled),
                  load_base, host_texture.base_hash.value_or(0), base_hash, load_mips, host_texture.mips_hash.value_or(0),
                  mips_hash);
    }
  }
  if (!LoadFromMemory(host_texture, load_base, load_mips)) return false;
  if (load_base) host_texture.base_hash = base_hash;
  if (load_base && !host_texture.hd_patched) host_texture.hd_view = HdView(host_texture);
  if (load_mips) host_texture.mips_hash = mips_hash;
  return true;
}

// VulkanTextureCache::LoadTextureDataFromResidentMemoryImpl, at resolution scale 1, on DiligentCore: the load
// shader reads the guest memory mirror and writes the host layout into a scratch buffer, which is then
// copied into the texture level by level.
bool GuestTextures::LoadFromMemory(HostTexture& host_texture, bool load_base, bool load_mips) {
  TextureKey key = host_texture.key();
  const HostFormat& host_format = GetHostFormat(key);
  LoadShaderIndex load_shader = host_format.load_shader;
  if (load_shader == kLoadShaderIndexUnknown) return false;
  LoadPipeline& pipeline = load_pipelines_[load_shader];
  if (!pipeline.pso) return false;
  const LoadShaderInfo& load_shader_info = GetLoadShaderInfo(load_shader);

  const texture_util::TextureGuestLayout& guest_layout = host_texture.guest_layout();
  bool is_3d = key.dimension == xenos::DataDimension::k3D;
  uint32_t width = key.GetWidth(), height = key.GetHeight();
  uint32_t depth_or_array_size = key.GetDepthOrArraySize();
  uint32_t depth = is_3d ? depth_or_array_size : 1;
  uint32_t array_size = is_3d ? 1 : depth_or_array_size;
  const rg::FormatInfo* guest_format_info = rg::FormatInfo::Get(key.format);
  uint32_t block_width = guest_format_info->block_width, block_height = guest_format_info->block_height;
  uint32_t bytes_per_block = guest_format_info->bytes_per_block();
  uint32_t level_first = load_base ? 0 : 1;
  uint32_t level_last = load_mips ? key.mip_max_level : 0;
  if (level_first > level_last) return true;
  uint32_t level_packed = guest_layout.packed_level;
  uint32_t level_stored_first = std::min(level_first, level_packed);
  uint32_t level_stored_last = std::min(level_last, level_packed);
  uint32_t loop_level_first, loop_level_last;
  if (level_packed == 0) {
    loop_level_first = uint32_t(level_first != 0);
    loop_level_last = uint32_t(level_last != 0);
  } else {
    loop_level_first = level_stored_first;
    loop_level_last = level_stored_last;
  }

  // The host layout of the scratch buffer.
  uint32_t host_block_width = host_format.block_compressed ? block_width : 1;
  uint32_t host_block_height = host_format.block_compressed ? block_height : 1;
  uint32_t host_x_blocks_per_thread = UINT32_C(1) << load_shader_info.guest_x_blocks_per_thread_log2;
  if (!host_format.block_compressed) host_x_blocks_per_thread *= block_width;
  struct HostLayout {
    uint64_t offset_bytes, slice_size_bytes;
    uint32_t x_pitch_blocks, y_pitch_blocks;
  };
  HostLayout host_layout_base{};
  HostLayout host_layout_mips[xenos::kTextureMaxMips]{};
  uint64_t host_buffer_size = 0;
  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = level_packed == 0 ? 0 : loop_level;
    HostLayout& level_host_layout = is_base ? host_layout_base : host_layout_mips[level];
    level_host_layout.offset_bytes = host_buffer_size;
    uint32_t level_x_texels, level_y_texels, level_z_texels;
    if (level == level_packed) {
      const texture_util::TextureGuestLayout::Level& packed = is_base ? guest_layout.base : guest_layout.mips[level];
      level_x_texels = packed.x_extent_blocks * block_width;
      level_y_texels = packed.y_extent_blocks * block_height;
      level_z_texels = packed.z_extent;
    } else {
      level_x_texels = std::max(width >> level, UINT32_C(1));
      level_y_texels = std::max(height >> level, UINT32_C(1));
      level_z_texels = std::max(depth >> level, UINT32_C(1));
    }
    level_host_layout.x_pitch_blocks =
        rex::round_up((level_x_texels + (host_block_width - 1)) / host_block_width, host_x_blocks_per_thread);
    level_host_layout.y_pitch_blocks = (level_y_texels + (host_block_height - 1)) / host_block_height;
    level_host_layout.slice_size_bytes = uint64_t(load_shader_info.bytes_per_host_block) *
                                         level_host_layout.x_pitch_blocks * level_host_layout.y_pitch_blocks *
                                         level_z_texels;
    host_buffer_size += level_host_layout.slice_size_bytes * array_size;
  }
  if (!host_buffer_size) return true;

  // One scratch buffer for every load, grown to the largest a texture needed (power of two, at least 4 MB): a buffer
  // created for each load cost 1.1 ms in all for GH5's 37.6 MB of venue textures in one frame, and 54 ms once, when
  // DiligentCore took a new block of device memory for them (D108).
  // The loads that use it run one after another: DiligentCore's transitions (the load's UAV writes, the copies that
  // read it, the next load's writes) put the barriers between them.
  if (!scratch_ || scratch_size_ < host_buffer_size) {
    uint64_t size = std::max<uint64_t>(UINT64_C(4) << 20, scratch_size_);
    while (size < host_buffer_size) size <<= 1;
    BufferDesc scratch_desc;
    scratch_desc.Name = "NativeXRenderer texture load";
    scratch_desc.Size = size;
    scratch_desc.Usage = USAGE_DEFAULT;
    scratch_desc.BindFlags = BIND_UNORDERED_ACCESS;
    scratch_desc.Mode = BUFFER_MODE_RAW;
    scratch_.Release();
    scratch_size_ = 0;
    device_->CreateBuffer(scratch_desc, nullptr, &scratch_);
    if (!scratch_) return false;
    scratch_size_ = size;
  }
  IBuffer* scratch = scratch_;

  // The guest data: views of the guest memory mirror at the base and mip pages.
  uint64_t source_alignment = UINT64_C(1) << load_shader_info.source_bpe_log2;
  auto make_source_view = [&](uint32_t start, uint32_t size) -> RefCntAutoPtr<IBufferView> {
    uint64_t range = rex::align(uint64_t(size), std::max(source_alignment, uint64_t(16)));
    if (!size || range > uint64_t(rg::SharedMemory::kBufferSize) - start) return {};
    BufferViewDesc view_desc;
    view_desc.ViewType = BUFFER_VIEW_SHADER_RESOURCE;
    view_desc.ByteOffset = start;
    view_desc.ByteWidth = range;
    RefCntAutoPtr<IBufferView> view;
    memory_.buffer()->CreateView(view_desc, &view);
    return view;
  };
  RefCntAutoPtr<IBufferView> source_base, source_mips;
  if (level_first == 0) {
    source_base = make_source_view(key.base_page << 12, host_texture.GetGuestBaseSize());
    if (!source_base) return false;
  }
  if (level_last != 0) {
    source_mips = make_source_view(key.mip_page << 12, host_texture.GetGuestMipsSize());
    if (!source_mips) return false;
  }
  memory_.PrepareForRead(context_);

  context_->SetPipelineState(pipeline.pso);
  pipeline.dest->Set(scratch->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
  LoadConstants constants{};
  constants.is_tiled_3d_endian_scale = uint32_t(key.tiled) | (uint32_t(is_3d) << 1) |
                                       (uint32_t(key.endianness) << 2) | (UINT32_C(1) << 4) | (UINT32_C(1) << 7);
  uint32_t guest_x_blocks_per_group_log2 = load_shader_info.GetGuestXBlocksPerGroupLog2();
  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = level_packed == 0 ? 0 : loop_level;
    pipeline.source->Set(is_base ? source_base : source_mips);
    constants.guest_offset = is_base ? 0 : guest_layout.mip_offsets_bytes[level];
    const texture_util::TextureGuestLayout::Level& level_guest_layout =
        is_base ? guest_layout.base : guest_layout.mips[level];
    uint32_t level_guest_pitch = level_guest_layout.row_pitch_bytes;
    if (key.tiled) level_guest_pitch /= bytes_per_block;
    constants.guest_pitch_aligned = level_guest_pitch;
    constants.guest_z_stride_block_rows_aligned = level_guest_layout.z_slice_stride_block_rows;
    uint32_t level_width, level_height, level_depth;
    if (level == level_packed) {
      level_width = level_guest_layout.x_extent_blocks * block_width;
      level_height = level_guest_layout.y_extent_blocks * block_height;
      level_depth = level_guest_layout.z_extent;
    } else {
      level_width = std::max(width >> level, UINT32_C(1));
      level_height = std::max(height >> level, UINT32_C(1));
      level_depth = std::max(depth >> level, UINT32_C(1));
    }
    constants.size_blocks[0] = (level_width + (block_width - 1)) / block_width;
    constants.size_blocks[1] = (level_height + (block_height - 1)) / block_height;
    constants.size_blocks[2] = level_depth;
    constants.height_texels = level_height;
    uint32_t group_count_x = (constants.size_blocks[0] + ((UINT32_C(1) << guest_x_blocks_per_group_log2) - 1)) >>
                             guest_x_blocks_per_group_log2;
    uint32_t group_count_y = (constants.size_blocks[1] + ((UINT32_C(1) << kLoadGuestYBlocksPerGroupLog2) - 1)) >>
                             kLoadGuestYBlocksPerGroupLog2;
    const HostLayout& level_host_layout = is_base ? host_layout_base : host_layout_mips[level];
    constants.host_offset = uint32_t(level_host_layout.offset_bytes);
    constants.host_pitch = load_shader_info.bytes_per_host_block * level_host_layout.x_pitch_blocks;
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      pipeline.constants->SetInlineConstants(&constants, 0, sizeof(constants) / 4);
      context_->CommitShaderResources(pipeline.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      DispatchComputeAttribs dispatch{group_count_x, group_count_y, constants.size_blocks[2]};
      context_->DispatchCompute(dispatch);
      constants.guest_offset += level_guest_layout.array_slice_stride_bytes;
      constants.host_offset += uint32_t(level_host_layout.slice_size_bytes);
    }
  }

  // Into the texture, level by level and slice by slice (packed mips are inside the packed level's data).
  for (uint32_t level = level_first; level <= level_last; ++level) {
    const HostLayout& level_host_layout = level != 0 ? host_layout_mips[std::min(level, level_packed)] : host_layout_base;
    uint64_t offset = level_host_layout.offset_bytes;
    if (level >= level_packed) {
      uint32_t offset_x, offset_y, offset_z;
      texture_util::GetPackedMipOffset(width, height, depth, key.format, level, offset_x, offset_y, offset_z);
      if (!host_format.block_compressed) {
        offset_x *= block_width;
        offset_y *= block_height;
      }
      offset += uint64_t(load_shader_info.bytes_per_host_block) *
                (offset_x + uint64_t(level_host_layout.x_pitch_blocks) *
                                (offset_y + uint64_t(level_host_layout.y_pitch_blocks) * offset_z));
    }
    uint32_t row_pitch = load_shader_info.bytes_per_host_block * level_host_layout.x_pitch_blocks;
    // DiligentCore copies with a tightly packed image height (bufferImageHeight 0), while a packed level's
    // rows are its whole extent: 3D levels are copied one depth slice at a time.
    uint64_t depth_stride = uint64_t(row_pitch) * level_host_layout.y_pitch_blocks;
    Box box;
    box.MaxX = std::max(width >> level, UINT32_C(1));
    box.MaxY = std::max(height >> level, UINT32_C(1));
    uint32_t level_depth = std::max(depth >> level, UINT32_C(1));
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      for (uint32_t z = 0; z < level_depth; ++z) {
        box.MinZ = z;
        box.MaxZ = z + 1;
        TextureSubResData data;
        data.pSrcBuffer = scratch;
        data.SrcOffset = offset + level_host_layout.slice_size_bytes * slice + depth_stride * z;
        data.Stride = row_pitch;
        data.DepthStride = depth_stride;
        context_->UpdateTexture(host_texture.texture(), level, slice, box, data,
                                RESOURCE_STATE_TRANSITION_MODE_TRANSITION, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      }
    }
  }
  ++stats_.loads;
  stats_.loaded_bytes += host_buffer_size;
  if (load_base) Patch(host_texture, false);
  return true;
}

// D142: the game's own pictures over this texture's (SetTexturePatcher). D160: the textures the patcher takes are
// kept, to be offered again after RepatchTextures (`again`); one it no longer takes (other data at that address,
// before its load) is dropped.
void GuestTextures::Patch(HostTexture& host_texture, bool again) {
  host_texture.repatch = false;
  const TextureKey key = host_texture.key();
  // Its own HD picture (below) is of the patcher's last pictures: without them, the pack's as it is.
  auto drop_hd = [&] {
    if (!host_texture.hd_patched) return;
    host_texture.hd_patched.Release();
    host_texture.hd_view = HdView(host_texture);
  };
  TexturePatchFn patch = g_texture_patcher.load(std::memory_order_acquire);
  if (!patch || uint32_t(key.format) != 6 /* k_8_8_8_8 */ || key.mip_max_level != 0 ||
      key.GetDepthOrArraySize() != 1) {
    patched_.erase(&host_texture);
    drop_hd();
    return;
  }
  const uint32_t w = key.GetWidth(), h = key.GetHeight();
  if (host_texture.GetGuestBaseSize() < uint64_t(w) * h * 4) {
    patched_.erase(&host_texture);
    drop_hd();
    return;
  }
  const uint8_t* guest = memory_.memory().TranslatePhysical<const uint8_t*>(key.base_page << 12);
  std::vector<uint8_t> rgba;
  if (!patch(w, h, guest, rgba, again, 1, nullptr)) {
    patched_.erase(&host_texture);
    drop_hd();
    return;
  }
  patched_.insert(&host_texture);
  if (rgba.size() != size_t(w) * h * 4) {
    drop_hd();
    return;
  }
  PatchHd(host_texture, again);
  Box box;
  box.MaxX = w, box.MaxY = h;
  TextureSubResData data;
  data.pData = rgba.data();
  data.Stride = uint64_t(w) * 4;
  context_->UpdateTexture(host_texture.texture(), 0, 0, box, data, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                          RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  REXLOG_INFO("[native_x_renderer] the game's pictures written over the {}x{} texture at {:08X} ({})", w, h,
              uint32_t(key.base_page) << 12, again ? "again, D160" : "D142");
}

// 13 Nov 2026: the HD pack's picture of a texture the patcher filled, with the patcher's pictures drawn into it at
// its size (Battlefield 1943's HUD and deploy screen atlases: the pack's copy alone would show the Xbox 360's buttons
// and no flag letters). The texture's own HD texture, made again at each patch; its smaller levels are made here
// (colour weighted by alpha, so a transparent surround does not darken an edge).
void GuestTextures::PatchHd(HostTexture& host_texture, bool again) {
  auto none = [&] {
    if (!host_texture.hd_patched) return;
    host_texture.hd_patched.Release();
    host_texture.hd_view = HdView(host_texture);
  };
  TexturePatchFn patch = g_texture_patcher.load(std::memory_order_acquire);
  HdPack* pack = REXCVAR_GET(native_x_renderer_hd_textures) && patch ? HdPackOpen() : nullptr;
  if (!pack) return none();
  const TextureKey key = host_texture.key();
  const uint32_t w = key.GetWidth(), h = key.GetHeight();
  const uint8_t* guest = memory_.memory().TranslatePhysical<const uint8_t*>(key.base_page << 12);
  const size_t hashed = size_t(std::min<uint64_t>(host_texture.GetGuestBaseSize(), 65536));
  const uint64_t content = XXH3_64bits(guest, hashed) ^ (uint64_t(6) << 56) ^ (uint64_t(w) << 32) ^ (uint64_t(h) << 16);
  const HdEntry* e = pack->Find(content);
  if (!e || e->bc != 0 || e->width != w || e->height != h || e->hd_width % w != 0 || e->hd_width / w < 2 ||
      e->hd_width / w > 8 || e->hd_height != h * (e->hd_width / w))
    return none();
  const uint32_t scale = e->hd_width / w, hw = e->hd_width, hh = e->hd_height;
  std::vector<uint8_t> base;
  if (!pack->Read(*e, base) || base.size() < size_t(hw) * hh * 4) return none();
  std::vector<std::vector<uint8_t>> chain(1);
  if (!patch(w, h, guest, chain[0], again, scale, base.data()) || chain[0].size() != size_t(hw) * hh * 4) return none();
  std::vector<std::pair<uint32_t, uint32_t>> sizes{{hw, hh}};
  while (sizes.back().first > 1 || sizes.back().second > 1) {
    const auto [sw, sh] = sizes.back();
    const uint32_t dw = std::max(1u, sw / 2), dh = std::max(1u, sh / 2);
    const std::vector<uint8_t>& s = chain.back();
    std::vector<uint8_t> d(size_t(dw) * dh * 4);
    for (uint32_t y = 0; y < dh; ++y)
      for (uint32_t x = 0; x < dw; ++x) {
        uint32_t sum[4] = {0, 0, 0, 0}, plain[3] = {0, 0, 0};
        for (uint32_t j = 0; j < 2; ++j)
          for (uint32_t i = 0; i < 2; ++i) {
            const uint8_t* p = &s[(size_t(std::min(sh - 1, y * 2 + j)) * sw + std::min(sw - 1, x * 2 + i)) * 4];
            for (int c = 0; c < 3; ++c) sum[c] += uint32_t(p[c]) * p[3], plain[c] += p[c];
            sum[3] += p[3];
          }
        uint8_t* o = &d[(size_t(y) * dw + x) * 4];
        for (int c = 0; c < 3; ++c) o[c] = uint8_t(sum[3] ? (sum[c] + sum[3] / 2) / sum[3] : (plain[c] + 2) / 4);
        o[3] = uint8_t((sum[3] + 2) / 4);
      }
    chain.push_back(std::move(d));
    sizes.push_back({dw, dh});
  }
  std::vector<TextureSubResData> levels(chain.size());
  for (size_t m = 0; m < chain.size(); ++m) levels[m].pData = chain[m].data(), levels[m].Stride = uint64_t(sizes[m].first) * 4;
  TextureDesc desc;
  desc.Name = "NativeXRenderer HD texture, patched";
  desc.Type = RESOURCE_DIM_TEX_2D_ARRAY;
  desc.ArraySize = 1;
  desc.Width = hw;
  desc.Height = hh;
  desc.MipLevels = uint32_t(chain.size());
  desc.Format = TEX_FORMAT_RGBA8_UNORM;
  desc.Usage = USAGE_IMMUTABLE;
  desc.BindFlags = BIND_SHADER_RESOURCE;
  TextureData init{levels.data(), uint32_t(levels.size())};
  RefCntAutoPtr<ITexture> texture;
  device_->CreateTexture(desc, &init, &texture);
  if (!texture) return none();
  host_texture.hd_patched = texture;
  host_texture.hd_view = texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  REXLOG_INFO("[native_x_renderer] HD texture {:016X}: {}x{} with the game's pictures for its {}x{} texture at {:08X}{}",
              content, hw, hh, w, h, uint32_t(key.base_page) << 12, again ? " (again)" : "");
}

void SetTexturePatcher(TexturePatchFn fn) { g_texture_patcher.store(fn, std::memory_order_release); }
void RepatchTextures() { g_repatch_count.fetch_add(1, std::memory_order_acq_rel); }

}  // namespace native_x_renderer
