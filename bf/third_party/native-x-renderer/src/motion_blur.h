// NativeXRenderer - motion blur of its own (internal; 0.4.2).
//
// In place of a game's own (Battlefield 1943: "remove 1943's current implementation of motion blur, and replace it
// with better quality camera motion blur with quality and a strength slider, and object-based motion blur on/off").
// At the end of the main 3D view, on its colour before the game's post-processing reads it:
//   - each pixel's motion since the last frame, in pixels: the camera's from the view's depth and the camera's
//     reprojection (the camera vote's, D118), then the motion of the objects that moved on their own drawn over it
//     (DrawPath::DrawObjectMotion, D48), as the upscalers take them;
//   - the colour gathered along that motion, about the pixel: `samples` taps, each weighed by how far its own motion
//     reaches (a still thing in front of a moving one stays sharp, a moving thing spreads over what is behind), with
//     a different offset in each pixel so the taps do not show as steps.
// The motion is brought to a fixed exposure (a sixtieth of a second at full strength) whatever the frame rate, so the
// blur looks the same at 60 and at 240 frames a second.

#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Shader.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"

#include "camera_vote.h"

namespace native_x_renderer {

class MotionBlur {
 public:
  struct Inputs {
    Diligent::ITexture* color = nullptr;         // the view's colour: blurred in place
    Diligent::ITexture* depth = nullptr;         // its depth target
    Diligent::ITextureView* depth_srv = nullptr;
    uint32_t width = 0, height = 0;              // the view's part of the colour, in its pixels
    uint32_t depth_width = 0, depth_height = 0;  // and of the depth (smaller with an upscaler)
    const Mat4d* reprojection = nullptr;         // column form: last clip = r x this clip; null: no camera blur
    float z_min = 0.0f, z_max = 1.0f;            // the depth the view's viewport writes at the near and far planes
    std::function<void(Diligent::ITextureView*)> object_motion;  // draws the objects' own motion; empty: none
    float strength = 0.5f;                       // 0 to 1: the exposure, of a sixtieth of a second
    int samples = 10;
    float frame_seconds = 1.0f / 60.0f;          // this frame's time
  };
  bool Load(Diligent::IRenderDevice* device);
  bool Apply(Diligent::IDeviceContext* context, const Inputs& in);
  uint32_t TakeApplied() {
    const uint32_t n = applied_;
    applied_ = 0;
    return n;
  }

 private:
  Diligent::IRenderDevice* device_ = nullptr;
  Diligent::RefCntAutoPtr<Diligent::IShader> vs_, velocity_ps_, blur_ps_;
  Diligent::RefCntAutoPtr<Diligent::IPipelineState> velocity_pso_, blur_pso_;
  Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> velocity_srb_, blur_srb_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> velocity_, source_;
  Diligent::TEXTURE_FORMAT format_ = Diligent::TEX_FORMAT_UNKNOWN;
  uint32_t applied_ = 0;
  bool failed_ = false;
};

// The developer's shader check (aa.cpp's TestUtilityShaders): how many of its shaders failed to compile.
int TestMotionBlurShaders();

}  // namespace native_x_renderer
