// NativeXRenderer - the host device: DiligentCore (Vulkan), its immediate context, and a swap chain on the
// game's window (internal).

#pragma once

#include <cstdint>
#include <string>

#include "DeviceContext.h"
#include "Fence.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "SwapChain.h"

namespace native_x_renderer {

// D135: the pipeline compilation threads (DiligentCore's pool) register themselves; below normal priority once the
// stored pipelines are made, so the game's threads come first in play.
void RegisterCompileThread();
void SetCompileThreadsLow(bool low);

#if !defined(_WIN32)
// The game window on Linux (renderer.cpp fills it from the SDK's SDL window): Wayland's display and surface, or X11's
// display and window id, and the window's size in pixels as the SDK last saw it.
struct HostWindow {
  void* display = nullptr;          // wl_display* (Wayland) or Display* (X11)
  void* wayland_surface = nullptr;  // wl_surface*
  uint32_t x11_window = 0;
  uint32_t (*pixel_size)(bool height) = nullptr;
  // D159: the window's real size when the SDK's is out of date (an X11 surface keeps its own size): the SDK is told.
  void (*real_pixel_size)(uint32_t width, uint32_t height) = nullptr;
};
#endif

class Device {
 public:
  // Creates the Vulkan device and a swap chain on the window (HWND on Windows, a HostWindow* on Linux). Logs and
  // returns false on failure. `half_precision`: the device's 16-bit float shaders enabled where the GPU has them, for
  // AMD's FSR 3.1, which picks its 16-bit shaders by what the GPU supports (D116).
  bool Create(void* native_window, bool half_precision = false);

  Diligent::IRenderDevice* device() const { return device_; }
  Diligent::IDeviceContext* context() const { return context_; }
  Diligent::ISwapChain* swap_chain() const { return swap_chain_; }

  // Follows the window's client size. Returns false while the window has no area (minimized): skip
  // the frame then.
  bool UpdateSize();

  // Presents without waiting for vertical sync: the game paces itself (its present interval and the
  // SDK's vblank), and a second wait here would halve the frame rate. Then waits until the GPU has
  // finished the frame kMaxFramesInFlight frames back: without vertical sync DiligentCore's swap chain
  // (mailbox) never blocks, and a GPU slower than the game would fall further behind every frame, until
  // DiligentCore's dynamic heap fills up. Returns the time waited, in milliseconds.
  // The present modes are native_x_renderer_present_mode's (D106, D107: V-Sync is FIFO, which waits for the
  // display; the SDK's vblank_follows_display keeps the game from running ahead of it).
  // `submit_ms`: with NXR_PRESENT_SPLIT=1 (a diagnosis aid), the frame is submitted before the swap chain's Present
  // and this is how long the submit took; 0 otherwise.
  double Present(double* submit_ms = nullptr);

  // Ends a frame that is not shown, while the window has no area (minimized: Alt+Tab from exclusive fullscreen): its
  // work submitted and the frame finished as Present does, so DiligentCore releases the frame's dynamic memory and
  // stale resources, then the same wait for the GPU (D125). Returns the time waited, in milliseconds.
  double FinishHiddenFrame();

  static constexpr uint64_t kMaxFramesInFlight = 2;
  // V-Sync whatever native_x_renderer_present_mode says (frame generation, D123: FIFO shows the generated frame and
  // the game's a refresh apart; without it the game's would take the generated one's place).
  void ForceVsync(bool on) {
    force_vsync_ = on;
    present_mode_applied_ = false;
  }

  // For the log: the frames presented and completed (frame fence), and DiligentCore's command queue fence
  // (next and completed values), which its dynamic heap and queries wait on.
  struct FenceState {
    uint64_t frames_presented, frames_completed, queue_next, queue_completed;
  };
  FenceState GetFenceState();
  // Frames presented so far, and the last one the GPU finished.
  uint64_t frames_presented() const { return frame_; }
  uint64_t frames_completed() const { return frame_fence_->GetCompletedValue(); }

 private:
  // Waits until the GPU has finished the frame kMaxFramesInFlight frames back (Present's wait).
  double WaitForFramesInFlight();
  // NXR_FRAME_DUMP (a diagnosis aid): the frame about to be presented, written to a file.
  void DumpFrame();
  Diligent::RefCntAutoPtr<Diligent::ITexture> dump_staging_;

  void* window_ = nullptr;
  Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device_;
  Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context_;
  Diligent::RefCntAutoPtr<Diligent::ISwapChain> swap_chain_;
  Diligent::RefCntAutoPtr<Diligent::IFence> frame_fence_;
  uint64_t frame_ = 0;
  // native_x_renderer_present_mode as the swap chain was last given it.
  void ApplyPresentMode();
  std::string present_mode_;
  bool present_mode_applied_ = false;
  bool force_vsync_ = false;
  // D159: a size the surface did not take (UpdateSize), not asked for again until the SDK's size changes.
  uint32_t refused_width_ = 0, refused_height_ = 0;
  int real_size_notes_ = 0;
};

}  // namespace native_x_renderer
