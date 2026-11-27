/**
 * @file        gpu/xenos/bfnative/bfnative_xenos_graphics_driver.cpp
 * @brief       The Xenos driver of a renderer that draws the title's frames itself, and its hooks
 *
 * @license     BSD 3-Clause License
 *
 * @remarks     Battlefield 1943 Reborn's patch of the SDK (see bfnative_xenos_graphics_driver.h).
 */

#include "gpu/xenos/bfnative/bfnative_xenos_graphics_driver.h"

#include <atomic>

#include <rex/gpu/bfnative_hooks.h>

#include "gpu/xenos/command_processor.h"
#include "gpu/xenos/null/null_command_processor.h"

#if defined(_WIN32)
#define REX_BFNATIVE_EXPORT extern "C" __declspec(dllexport)
#else
#define REX_BFNATIVE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace rex::gpu::bfnative {

namespace {

std::atomic<BfNativeXenosGraphicsDriver*> g_driver{nullptr};
std::atomic<u64> g_swaps_issued{0};

CommandProcessor* Processor() {
  BfNativeXenosGraphicsDriver* driver = g_driver.load(std::memory_order_acquire);
  return driver ? driver->GetCommandProcessor() : nullptr;
}

bool IsActive() {
  return Processor() != nullptr;
}

bool SetDrawObserver(DrawObserver observer, void* user) {
  CommandProcessor* cp = Processor();
  if (!cp) {
    return false;
  }
  cp->SetDrawObserver(observer, user);
  return true;
}

bool SetOcclusionQueryResolver(OcclusionQueryResolver resolver, void* user) {
  CommandProcessor* cp = Processor();
  if (!cp) {
    return false;
  }
  cp->SetOcclusionQueryResolver(resolver, user);
  return true;
}

bool SetSignalGate(SignalGate gate, void* user, KickTag kick_tag) {
  CommandProcessor* cp = Processor();
  if (!cp) {
    return false;
  }
  if (gate || kick_tag) {
    cp->SetSignalGate(gate, user, kick_tag);
  }
  return true;
}

void NoteSwapIssued() {
  g_swaps_issued.fetch_add(1, std::memory_order_acq_rel);
}

bool GetSwapCounts(u64* issued, u64* executed) {
  CommandProcessor* cp = Processor();
  if (issued) {
    *issued = g_swaps_issued.load(std::memory_order_acquire);
  }
  if (executed) {
    *executed = cp ? cp->GetSwapsExecuted() : 0;
  }
  return cp != nullptr;
}

bool SetSwapObserver(SwapObserver observer, void* user) {
  CommandProcessor* cp = Processor();
  if (!cp) {
    return false;
  }
  cp->SetSwapObserver(observer, user);
  return true;
}

bool GetGammaRamp(GammaRamp* ramp) {
  CommandProcessor* cp = Processor();
  if (!cp || !ramp) {
    return false;
  }
  cp->GetGammaRamp(*ramp);
  return true;
}

IPhysicalMemory* GetPhysicalMemory() {
  BfNativeXenosGraphicsDriver* driver = g_driver.load(std::memory_order_acquire);
  return driver ? driver->GetPhysicalMemory() : nullptr;
}

IPhysicalWriteWatch* GetPhysicalWriteWatch() {
  BfNativeXenosGraphicsDriver* driver = g_driver.load(std::memory_order_acquire);
  return driver ? driver->GetPhysicalWriteWatch() : nullptr;
}

std::atomic<DetachObserver> g_detach_observer{nullptr};
std::atomic<void*> g_detach_user{nullptr};

bool SetDetachObserver(DetachObserver observer, void* user) {
  g_detach_user.store(user, std::memory_order_relaxed);
  g_detach_observer.store(observer, std::memory_order_release);
  return true;
}

const XenosSettings* GetXenosSettings() {
  CommandProcessor* cp = Processor();
  return cp ? &cp->GetXenosSettings() : nullptr;
}

const Hooks kHooks = {kHooksVersion,   sizeof(Hooks),  IsActive,         SetDrawObserver, SetOcclusionQueryResolver,
                      SetSignalGate,   NoteSwapIssued, GetSwapCounts,    SetSwapObserver, GetGammaRamp,
                      GetPhysicalMemory, GetPhysicalWriteWatch, GetXenosSettings, SetDetachObserver};

}  // namespace

BfNativeXenosGraphicsDriver::BfNativeXenosGraphicsDriver() {
  g_driver.store(this, std::memory_order_release);
}

BfNativeXenosGraphicsDriver::~BfNativeXenosGraphicsDriver() {
  BfNativeXenosGraphicsDriver* self = this;
  g_driver.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
}

Result<void> BfNativeXenosGraphicsDriver::SetSurface(const NativeWindow&) {
  return Ok();
}

void BfNativeXenosGraphicsDriver::CreateProvider(bool) {}

void BfNativeXenosGraphicsDriver::DetachTitle() {
  const bool attached = physical_memory_ != nullptr;
  XenosGraphicsDriverBase::DetachTitle();
  if (!attached) {
    return;
  }
  if (DetachObserver observer = g_detach_observer.load(std::memory_order_acquire)) {
    observer(g_detach_user.load(std::memory_order_relaxed));
  }
}

std::unique_ptr<CommandProcessor> BfNativeXenosGraphicsDriver::CreateCommandProcessor() {
  return std::make_unique<null::NullCommandProcessor>(this, title_info_, *physical_memory_, *write_watch_,
                                                      *guest_interrupts_);
}

BfNativeXenosGraphicsDriver* GetActiveDriver() {
  return g_driver.load(std::memory_order_acquire);
}

}  // namespace rex::gpu::bfnative

REX_BFNATIVE_EXPORT const rex::gpu::bfnative::Hooks* rex_bfnative_hooks() {
  return &rex::gpu::bfnative::kHooks;
}
