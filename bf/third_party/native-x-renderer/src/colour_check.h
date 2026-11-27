// A test of a pass's two ways on the same picture (the Night Ops performance pass, 20 Nov 2026): the scene's colour
// kept before the pass, the pass drawn this way and read back, the colour put back, the pass drawn the old way and read
// back, the two compared byte by byte. Used by BF_AO_DISCARD_CHECK (GTAO's apply and SSGI's composite leaving the
// pixels they do not change). Only for single sample colour; the frame keeps the old way's picture.
#pragma once

#include "DeviceContext.h"
#include "GraphicsAccessories.hpp"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Texture.h"
#include <rex/logging.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace native_x_renderer {

class ColourCheck {
 public:
  // Whether this pass is checked this time (every 60th, with the switch): the colour then kept.
  bool Begin(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, Diligent::ITexture* color) {
    using namespace Diligent;
    static const bool on = [] {
      const char* v = std::getenv("BF_AO_DISCARD_CHECK");
      return v && *v == '1';
    }();
    if (!on || !color || (count_++ % 60) != 0) return false;
    const TextureDesc& cd = color->GetDesc();
    const uint32_t size = GetTextureFormatAttribs(cd.Format).GetElementSize();
    if (cd.SampleCount != 1 || size == 0) {
      if (!said_) REXLOG_INFO("[native_x_renderer] colour check: not for format {} with {} samples", int(cd.Format), cd.SampleCount);
      said_ = true;
      return false;
    }
    pixel_ = size;
    for (RefCntAutoPtr<ITexture>* t : {std::addressof(keep_), std::addressof(staging_[0]), std::addressof(staging_[1])}) {
      if (*t && (*t)->GetDesc().Width == cd.Width && (*t)->GetDesc().Height == cd.Height && (*t)->GetDesc().Format == cd.Format)
        continue;
      t->Release();
      TextureDesc d;
      d.Name = "NativeXRenderer colour check";
      d.Type = RESOURCE_DIM_TEX_2D;
      d.Width = cd.Width, d.Height = cd.Height, d.Format = cd.Format;
      if (t == std::addressof(keep_)) {
        d.BindFlags = BIND_SHADER_RESOURCE;
      } else {
        d.Usage = USAGE_STAGING, d.CPUAccessFlags = CPU_ACCESS_READ;
      }
      device->CreateTexture(d, nullptr, &(*t));
      if (!*t) return false;
    }
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
    CopyTextureAttribs keep(color, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, keep_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->CopyTexture(keep);
    return true;
  }
  // After this way's pass: its picture read back, the colour as it was put back.
  void TakeFirst(Diligent::IDeviceContext* context, Diligent::ITexture* color) {
    using namespace Diligent;
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
    CopyTextureAttribs first(color, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, staging_[0], RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->CopyTexture(first);
    CopyTextureAttribs back(keep_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, color, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->CopyTexture(back);
  }
  // After the old way's pass: both compared and logged.
  void Finish(Diligent::IDeviceContext* context, Diligent::ITexture* color, const char* what) {
    using namespace Diligent;
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
    CopyTextureAttribs second(color, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, staging_[1], RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->CopyTexture(second);
    context->WaitForIdle();
    const TextureDesc& cd = color->GetDesc();
    MappedTextureSubresource ma, mb;
    context->MapTextureSubresource(staging_[0], 0, 0, MAP_READ, MAP_FLAG_NONE, nullptr, ma);
    context->MapTextureSubresource(staging_[1], 0, 0, MAP_READ, MAP_FLAG_NONE, nullptr, mb);
    uint64_t differ = 0;
    int most = 0;
    if (ma.pData && mb.pData) {
      const uint32_t bytes = uint32_t(std::min<uint64_t>(std::min<uint64_t>(ma.Stride, mb.Stride), uint64_t(cd.Width) * pixel_));
      for (uint32_t y = 0; y < cd.Height; ++y) {
        const uint8_t* pa = static_cast<const uint8_t*>(ma.pData) + size_t(y) * ma.Stride;
        const uint8_t* pb = static_cast<const uint8_t*>(mb.pData) + size_t(y) * mb.Stride;
        if (std::memcmp(pa, pb, bytes) == 0) continue;
        for (uint32_t x = 0; x + pixel_ <= bytes; x += pixel_) {
          int d = 0;
          for (uint32_t c = 0; c < pixel_; ++c) d = std::max(d, std::abs(int(pa[x + c]) - int(pb[x + c])));
          if (d) ++differ, most = std::max(most, d);
        }
      }
    }
    if (ma.pData) context->UnmapTextureSubresource(staging_[0], 0, 0);
    if (mb.pData) context->UnmapTextureSubresource(staging_[1], 0, 0);
    REXLOG_INFO("[native_x_renderer] {} check: {}x{} format {}: {} pixels differ, by {} at most (a byte)", what, cd.Width,
                cd.Height, int(cd.Format), differ, most);
  }

 private:
  Diligent::RefCntAutoPtr<Diligent::ITexture> keep_, staging_[2];
  uint64_t count_ = 0;
  uint32_t pixel_ = 4;
  bool said_ = false;
};

}  // namespace native_x_renderer
