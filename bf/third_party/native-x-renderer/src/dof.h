// NativeXRenderer - depth of field of its own (internal; after 0.4.5).
//
// In place of a game's own (Battlefield 1943: "remove all of the game's native depth of field, and make our own higher
// quality DoF with toggle-able ADS Depth of Field"; the game's was a 5 pixel Gaussian over everything near while a
// weapon's zoom was in). While the player aims down the sights (the game says how far the zoom is in, SetAdsBlend),
// what is nearer than the focus is blurred, more the nearer it is: the gun at the eye goes soft and what is aimed at
// stays sharp, as a lens focused on the target shows it.
//   - a pixel's blur radius comes from its distance (the view's depth, Battlefield 1943's as 0.1 / metres): none from
//     0.7 m on (a front sight), the most at 0.15 m and nearer (the back of the gun at the eye), eased between, times
//     the zoom's blend; and none in the middle of the view, where the sights line up (D196: "less strong toward the
//     front of the weapon ... stronger toward the back ... keeping the sights clear");
//   - NXR_DOF_SHOW=1 paints the distance under 3 m in bands instead (red under 0.15 m, orange, yellow, green, cyan,
//     blue, magenta, white from 1.4 m); 2 on the left half of the view only;
//   - the colour is gathered at half size over a disc of the largest radius: `taps` points on a Vogel spiral, the
//     same in every pixel and frame (D196: at first at full size, turned differently in each pixel, which left the
//     blur grainy). A tap counts where its own blur reaches this texel, so the soft gun spreads over the sharp world
//     behind its edge and the sharp world does not leak into the gun;
//   - a small tent smooths the gathered texels, and the result is blended over the full size colour by each pixel's
//     own radius (from the full size depth: the gun's edge against what is in focus stays where it is) or by how far
//     the gun's blur spreads over it.

#pragma once

#include <cstdint>

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Shader.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"

namespace native_x_renderer {

// How far the game's zoom is in, 0 to 1, as it last said (SetAdsBlend, within a quarter second; else 0).
float AdsBlendNow();

class DepthOfField {
 public:
  struct Inputs {
    Diligent::ITexture* color = nullptr;  // the view's colour: blurred in place
    Diligent::ITexture* depth = nullptr;
    Diligent::ITextureView* depth_srv = nullptr;
    uint32_t width = 0, height = 0;              // the view's part of the colour, in its pixels
    uint32_t depth_width = 0, depth_height = 0;  // and of the depth
    float blend = 0.0f;                          // the zoom, 0 to 1
    int taps = 24;
  };
  bool Load(Diligent::IRenderDevice* device);
  bool Apply(Diligent::IDeviceContext* context, const Inputs& in);

 private:
  Diligent::IRenderDevice* device_ = nullptr;
  struct Pass {
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
  };
  Diligent::RefCntAutoPtr<Diligent::IShader> vs_, composite_ps_;
  Pass passes_[3];  // the half size colour and radius, the gather, the fill
  Pass composite_;  // made for the colour's format
  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> half_, blur_;  // half the view's size
  Diligent::TEXTURE_FORMAT format_ = Diligent::TEX_FORMAT_UNKNOWN;
  bool failed_ = false, said_ = false;
};

// The developer's shader check (aa.cpp's TestUtilityShaders): how many of its shaders failed to compile.
int TestDofShaders();

}  // namespace native_x_renderer
