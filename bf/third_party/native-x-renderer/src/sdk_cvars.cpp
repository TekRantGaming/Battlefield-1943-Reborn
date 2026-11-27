// NativeXRenderer - the frame counter internal.h's NextFrame bumps (decision D15).
//
// Under SDK v0.10 NativeXRenderer compiled part of the SDK's graphics code itself, and this file forwarded that
// code's cvars to the GPU plugin's registered values, looked up again once per frame (D68). SDK v0.11+ links that
// code as static libraries whose settings are the bfnative driver's XenosSettings (rex/gpu/bfnative_hooks.h), so
// nothing is forwarded any more; the counter stays for NextFrame's callers.

#include <atomic>
#include <cstdint>

namespace native_x_renderer::sdk {
std::atomic<uint32_t> g_frame_generation{1};
void NextFrame() { g_frame_generation.fetch_add(1, std::memory_order_relaxed); }
}  // namespace native_x_renderer::sdk
