// Battlefield 1943 Reborn, SDK v0.11+ port (phase 5): v0.10's rex/system/interfaces/graphics.h, for NativeXRenderer.
//
// Under v0.10 NativeXRenderer steered the SDK's graphics system through our additions to IGraphicsSystem. In v0.11 the
// Xenos driver "bfnative" (rexgpu-xenos-bfnative, our SDK patch) runs the guest's command stream and exports the same
// hooks as a C table (rex/gpu/bfnative_hooks.h). This IGraphicsSystem is NativeXRenderer's view of that table, with
// v0.10's calls; bfnative_graphics() returns it while the driver runs (else null). The player's brightness and gamma
// (bf_brightness, bf_gamma) are applied to the ramp here, as v0.10's command processor did.
#pragma once

#include <cstdint>

#include <rex/gpu/bfnative_hooks.h>

namespace rex::ui {
class Presenter;  // v0.10's; the bfnative driver has none
}  // namespace rex::ui

namespace rex::system {

using GpuDrawInfo = ::rex::gpu::bfnative::DrawInfo;
using GpuDrawObserver = ::rex::gpu::bfnative::DrawObserver;
using OcclusionQueryResolver = ::rex::gpu::bfnative::OcclusionQueryResolver;
using GpuSignalGate = ::rex::gpu::bfnative::SignalGate;
using GpuKickTag = ::rex::gpu::bfnative::KickTag;
using GpuSwapObserver = ::rex::gpu::bfnative::SwapObserver;
using GammaRamp = ::rex::gpu::bfnative::GammaRamp;

class IGraphicsSystem {
 public:
  explicit IGraphicsSystem(const ::rex::gpu::bfnative::Hooks& hooks) : hooks_(&hooks) {}

  bool SetDrawObserver(GpuDrawObserver observer, void* user) { return hooks_->SetDrawObserver(observer, user); }
  // The bfnative driver never draws on the host: off is all there is.
  bool SetHostRendering(bool enabled) { return !enabled; }
  bool SetOcclusionQueryResolver(OcclusionQueryResolver resolver, void* user) {
    return hooks_->SetOcclusionQueryResolver(resolver, user);
  }
  bool SetSignalGate(GpuSignalGate gate, void* user, GpuKickTag kick_tag = nullptr) {
    return hooks_->SetSignalGate(gate, user, kick_tag);
  }
  void NoteSwapIssued() { hooks_->NoteSwapIssued(); }
  bool GetSwapCounts(uint64_t& issued, uint64_t& executed) { return hooks_->GetSwapCounts(&issued, &executed); }
  bool SetSwapObserver(GpuSwapObserver observer, void* user) { return hooks_->SetSwapObserver(observer, user); }
  // Called when the title detaches, after the command processor stopped: the renderer releases its listeners on the
  // title's memory there, or the title's unload waits for them for ever (the port's quit, 25 Nov 2026).
  bool SetDetachObserver(void (*observer)(void* user), void* user) { return hooks_->SetDetachObserver(observer, user); }
  // The guest's ramp with the player's picture settings applied (compat/graphics_hooks.cpp).
  bool GetGammaRamp(GammaRamp& ramp);
  ui::Presenter* presenter() const { return nullptr; }
  // The title's physical memory and write watches (null before the title attaches): the renderer's guest memory mirror
  // is an SDK SharedMemory on them.
  ::rex::IPhysicalMemory* physical_memory() const { return hooks_->GetPhysicalMemory(); }
  ::rex::IPhysicalWriteWatch* write_watch() const { return hooks_->GetPhysicalWriteWatch(); }
  // The driver's gpu.xenos settings, for the renderer's texture cache (null before the title attaches).
  const ::rex::gpu::XenosSettings* xenos_settings() const { return hooks_->GetXenosSettings(); }

 private:
  const ::rex::gpu::bfnative::Hooks* hooks_;
};

// The bfnative driver's hooks while its module is loaded and its command processor runs, else null.
IGraphicsSystem* bfnative_graphics();

}  // namespace rex::system
