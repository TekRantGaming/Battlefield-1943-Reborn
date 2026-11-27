// NativeXRenderer - the SDK's ImGui overlays on NativeXRenderer's swap chain (internal).
//
// The SDK draws its dialogs (XamShowKeyboardUI's keyboard, message boxes, the game's own ImGui dialogs) with Dear
// ImGui. In SDK v0.11+ its UI system holds them, and a renderer that presents itself asks it for one overlay frame
// per presented frame (IUISystem::DrawOverlays, D75): Dear ImGui builds the frame on the UI thread, where it takes the
// window's input, and hands its draw data to this IOverlayRenderer on the renderer's thread, which draws it over the
// frame before presenting it. Dear ImGui 1.92's textures (the font atlas, the achievement icons) come with the draw
// data and are made here.
#pragma once

#include <rex/overlay_renderer.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"
#include "TextureView.h"

struct ImTextureData;

namespace rex::ui {
class Window;
}  // namespace rex::ui

namespace native_x_renderer {

class Overlay final : public rex::IOverlayRenderer {
 public:
  bool Initialize(Diligent::IRenderDevice* device, Diligent::TEXTURE_FORMAT target_format);
  // Draws the UI system's dialogs, if any, over `target`. On the renderer's thread, once per presented frame.
  void Present(Diligent::IDeviceContext* context, Diligent::ITextureView* target, uint32_t width, uint32_t height,
               rex::ui::Window* window);

  void RenderOverlays(ImDrawData& draw_data) override;
  // Before this overlay goes (another renderer takes the window): Dear ImGui's textures are made again by the next
  // one. On the UI thread, between frames.
  void ReleaseTextures();

 private:
  struct TextureEntry {
    Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
  };

  bool Upload(ImTextureData& texture);
  void UpdateTextures(ImDrawData& draw_data);
  bool MakeWhite();

  Diligent::IRenderDevice* device_ = nullptr;
  Diligent::RefCntAutoPtr<Diligent::IPipelineState> pipeline_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> vertices_, indices_;
  uint32_t vertex_capacity_ = 0, index_capacity_ = 0;
  // Dear ImGui's textures by their ImTextureData; a replaced or destroyed one is kept a few frames (the GPU may
  // still read it), as the SDK's own overlay does.
  std::unordered_map<const ImTextureData*, std::unique_ptr<TextureEntry>> textures_;
  struct Retired {
    uint64_t frame;
    std::unique_ptr<TextureEntry> entry;
  };
  std::vector<Retired> retired_;
  uint64_t frame_ = 0;
  std::unique_ptr<TextureEntry> white_;  // for the draws without a texture

  // The frame RenderOverlays draws into, set by Present for the length of its DrawOverlays call.
  Diligent::IDeviceContext* context_ = nullptr;
  Diligent::ITextureView* target_ = nullptr;
  uint32_t width_ = 0, height_ = 0;
};

}  // namespace native_x_renderer
