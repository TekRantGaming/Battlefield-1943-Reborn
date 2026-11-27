// Battlefield 1943 Reborn, SDK v0.11+ port (phase 4): see rex/system/kernel_state.h.
#include <rex/system/kernel_state.h>

#include <atomic>
#include <mutex>

namespace rex::system {

KernelState::KernelState(::rex::kernel::TitleRuntime& runtime)
    : runtime_(&runtime), memory_(runtime) {}

KernelState* kernel_state() {
  // One facade per runtime. Hooks call this on hot paths, so the usual case takes no lock: the runtime the facade
  // was made for is compared with the current one. A relaunch (UnloadTitle, LaunchTitle) gives a new runtime and a new
  // facade; the old one is kept (a few dozen bytes), since another thread may still hold its pointer.
  static std::atomic<::rex::kernel::TitleRuntime*> cached_runtime{nullptr};
  static std::atomic<KernelState*> cached_state{nullptr};
  ::rex::kernel::TitleRuntime* runtime = ::rex::kernel::GetProcessRuntime();
  if (!runtime) return nullptr;
  if (runtime == cached_runtime.load(std::memory_order_acquire)) {
    return cached_state.load(std::memory_order_acquire);
  }
  if (!runtime->IsLoaded()) return nullptr;
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  if (runtime != cached_runtime.load(std::memory_order_relaxed)) {
    cached_state.store(new KernelState(*runtime), std::memory_order_release);
    cached_runtime.store(runtime, std::memory_order_release);
  }
  return cached_state.load(std::memory_order_acquire);
}

}  // namespace rex::system
