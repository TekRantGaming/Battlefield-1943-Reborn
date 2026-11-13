// NativeXRenderer - temporal antialiasing of its own (internal; 13 Nov 2026).
//
// The plain one beside DLSS, FSR and XeSS (upscaler.h), with no library and on any GPU: the 3D view is drawn with the
// same sub-pixel jitter the others get, and each frame is blended into a history that follows the picture's motion
// (the upscaler's motion vectors) and is kept within what the new frame shows around each pixel. At the render
// resolution only (no upscaling). It takes FSR's inputs (fsr.h's Inputs); the depth is not used.

#pragma once

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Shader.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"

#include "fsr.h"

namespace native_x_renderer {

class Taa {
 public:
  using Inputs = Fsr::Inputs;
  bool Load(Diligent::IRenderDevice* device);
  // The resolve into `in.output` (a render target of the colour's size), then that kept as the next frame's history.
  bool Dispatch(Diligent::IDeviceContext* context, const Inputs& in);

 private:
  Diligent::IRenderDevice* device_ = nullptr;
  Diligent::RefCntAutoPtr<Diligent::IShader> vs_, ps_;
  Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso_;
  Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> history_;
  Diligent::TEXTURE_FORMAT format_ = Diligent::TEX_FORMAT_UNKNOWN;
};

}  // namespace native_x_renderer
