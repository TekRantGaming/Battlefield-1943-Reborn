// NativeXRenderer - Intel XeSS (Xe Super Sampling) on DiligentCore's Vulkan device (internal; 13 Nov 2026).
//
// A third temporal upscaler beside DLSS and FSR (upscaler.h), on any vendor's GPU. Intel's XeSS SDK 3.0.2: its headers
// (thirdparty/xess) and its libxess.dll, fetched when configuring and copied next to the game's executable, loaded at
// run time. XeSS needs Vulkan extensions enabled when the device is made, so the device is prepared for it only when
// XeSS is the antialiasing chosen (PrepareDevice, from device.cpp); if the device cannot be made that way it is made
// without, and XeSS reports itself unavailable. It takes the inputs FSR takes (fsr.h's Inputs): the colour, the depth
// copy, the motion vectors in input pixels and the jitter.

#pragma once

#include <cstdint>

#include "DeviceContext.h"
#include "EngineFactoryVk.h"
#include "RenderDevice.h"
#include "Texture.h"

#include "fsr.h"

namespace native_x_renderer {

class Xess {
 public:
  using Inputs = Fsr::Inputs;
  ~Xess();

  // Before the Vulkan device is made: XeSS's instance and device extensions added to `create_info` (the strings are
  // the library's, kept loaded). False (logged) when the library is missing or the GPU is not one it runs on;
  // `create_info` is then as it was.
  static bool PrepareDevice(Diligent::EngineVkCreateInfo& create_info);
  // The device could not be made with what PrepareDevice added (it is made again without): no XeSS.
  static void DeviceFailed();

  // False (logged) when the device was not prepared or the library lacks an entry point.
  bool Load(Diligent::IRenderDevice* device);
  // Records the upscale into `context`'s command buffer. `quality`: 0 native AA, 1 quality, 2 balanced, 3 performance,
  // 4 ultra performance (AMD's ratios, fsr.h). XeSS's context is made on the first call and again when the output's
  // size, the colour's range or the quality change.
  bool Dispatch(Diligent::IDeviceContext* context, const Inputs& in, int quality);

 private:
  Diligent::IRenderDevice* device_ = nullptr;
  void* context_ = nullptr;  // xess_context_handle_t
  uint32_t size_[2] = {};
  bool hdr_ = false;
  int quality_ = -1;
};

}  // namespace native_x_renderer
