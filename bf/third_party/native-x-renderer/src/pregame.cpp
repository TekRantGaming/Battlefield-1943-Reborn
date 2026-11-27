// NativeXRenderer - the pages before the title on the game's window, until the renderer starts (SDK v0.11+).
//
// A program's pages before the title (Battlefield 1943 Reborn's launcher, its first start's setup and name pages)
// are the UI system's Dear ImGui dialogs. Under SDK v0.10 the GPU plugin's presenter drew them. Under v0.11 the
// bfnative driver has no presenter and NativeXRenderer starts once the title is attached (its command processor), so
// nothing drew them: a black window (the port's first smoke run, 25 Nov 2026). Here a device and swap chain of their
// own, with the renderer's overlay, present them: one frame per call, on the UI thread, which is where the UI system
// builds its frame. Released before the renderer makes its own device on the same window (Initialize): its choices
// (antialiasing, scaling, render scale) are the pages' to make first.
#include <native_x_renderer/native_x_renderer.h>

#include <memory>

#include <rex/logging.h>

#include <ui_compat.h>  // the game's window (bf/src/compat)

#include "device.h"
#include "overlay.h"

using namespace Diligent;

namespace native_x_renderer {

#if !defined(_WIN32)
namespace renderer {
void* PreGameHostWindow(rex::ui::Window* window);  // renderer.cpp: its LinuxHostWindow
}  // namespace renderer
#endif

namespace {

struct PreGame {
  std::unique_ptr<Device> device;  // declared first: released last
  Overlay overlay;
  rex::ui::Window* window = nullptr;
  uint64_t frames = 0;
};
std::unique_ptr<PreGame> g_pre_game;

}  // namespace

bool StartPreGamePresenter(rex::ui::Window* window) {
  if (g_pre_game) return true;
#if defined(_WIN32)
  void* native_window = window ? window->GetNativeWindowHandle() : nullptr;
#else
  void* native_window = renderer::PreGameHostWindow(window);
#endif
  if (!native_window) {
    REXLOG_WARN("[native_x_renderer] pages before the game: no window to present on; they are not drawn");
    return false;
  }
  auto pre_game = std::make_unique<PreGame>();
  pre_game->device = std::make_unique<Device>();
  if (!pre_game->device->Create(native_window) ||
      !pre_game->overlay.Initialize(pre_game->device->device(),
                                    pre_game->device->swap_chain()->GetDesc().ColorBufferFormat)) {
    REXLOG_ERROR("[native_x_renderer] pages before the game: no device or overlay; they are not drawn");
    return false;
  }
  pre_game->window = window;
  g_pre_game = std::move(pre_game);
  REXLOG_INFO("[native_x_renderer] pages before the game: presented on a device of their own until the renderer starts");
  return true;
}

void PresentPreGameFrame() {
  if (!g_pre_game) return;
  PreGame& p = *g_pre_game;
  Device& device = *p.device;
  if (!device.UpdateSize()) return;  // minimized: nothing to show
  // The back buffer before the swap chain's description: a new swap chain replaces it (issue 26).
  ITextureView* back_buffer = device.BackBuffer();
  if (!back_buffer) return;
  const SwapChainDesc& desc = device.swap_chain()->GetDesc();
  IDeviceContext* context = device.context();
  context->SetRenderTargets(1, &back_buffer, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  static constexpr float kBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  context->ClearRenderTarget(back_buffer, kBlack, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  p.overlay.Present(context, back_buffer, desc.Width, desc.Height, p.window);
  device.Present();
  ++p.frames;
}

void StopPreGamePresenter() {
  if (!g_pre_game) return;
  PreGame& p = *g_pre_game;
  // Dear ImGui's textures name this overlay's entries: made again by the next renderer's.
  p.overlay.ReleaseTextures();
  if (IDeviceContext* context = p.device->context()) {
    context->Flush();
    context->WaitForIdle();
  }
  p.device->device()->IdleGPU();
  REXLOG_INFO("[native_x_renderer] pages before the game: {} frames presented; their device released", p.frames);
  g_pre_game.reset();
}

}  // namespace native_x_renderer
