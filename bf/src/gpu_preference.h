#pragma once
// Every graphics device the game makes on the same high-performance GPU (King Kong's fix, its commit 371e6771, task
// 203; the selection order is gpu_choice.h): the SDK's provider (our bfnative plugin's Vulkan device, the launcher's
// pages; the Xenos plugin's D3D12 or Vulkan one in developer builds) and NativeXRenderer's own Vulkan device, which
// takes the same vulkan_device (bf/third_party/native-x-renderer/src/device.cpp, D161).

namespace bf1943 {

enum class GpuApi { kD3D12, kVulkan };

// Once per process and API, before an SDK provider is made (Bf1943App::OnPreSetup): unless the player set
// d3d12_adapter / vulkan_device (config, command line or environment), point it at the high-performance GPU. The
// value is applied as a command-line value so it is never written to a settings file (adapter numbers change when a
// GPU is added or removed). Logs the choice and the adapter list.
void PreferHighPerformanceGpu(GpuApi api);

}  // namespace bf1943
