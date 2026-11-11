// rexgpu-bfnative entry points: Vulkan only. Until the native passes take over (M2, M3) the plugin's graphics
// system is the Xenos Vulkan renderer compiled into it, so M0 presents the Xenos picture through this plugin.

#include <string_view>

#include <rex/graphics/shader_storage_progress.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

#include "null_graphics.h"

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) { return rex::system::kGpuPluginAbiVersion; }

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion) {
    REXLOG_ERROR("rexgpu-bfnative: host requested ABI {}, plugin is ABI {}", abi_version,
                 rex::system::kGpuPluginAbiVersion);
    return nullptr;
  }
  if (!info || info->struct_size < sizeof(rex::system::GpuCreateInfo)) {
    REXLOG_ERROR("rexgpu-bfnative: invalid GpuCreateInfo");
    return nullptr;
  }
  const std::string_view backend = info->backend ? info->backend : "any";
  // "null": a dedicated server's GPU, which reads the command stream and draws nothing (null_graphics.h).
  if (backend == "null") return new bf1943::gpu::NullGraphicsSystem();
  if (backend != "any" && backend != "vulkan") {
    REXLOG_ERROR("rexgpu-bfnative: backend '{}' refused, this plugin is Vulkan only", backend);
    return nullptr;
  }
  REXLOG_INFO("rexgpu-bfnative: Vulkan (M0: the Xenos renderer inside the native plugin)");
  return new rex::graphics::vulkan::VulkanGraphicsSystem();
}

// The game's shader screen, as rexgpu-xenos exports it (shader_storage_progress.h).
extern "C" REX_GPU_PLUGIN_EXPORT void rex_gpu_shader_storage_progress(uint32_t* queued, uint32_t* done,
                                                                      uint32_t* loading, uint32_t* finished) {
  auto& p = rex::graphics::GetShaderStorageProgress();
  if (queued) *queued = p.queued.load();
  if (done) *done = p.done.load();
  if (loading) *loading = p.loading.load();
  if (finished) *finished = p.finished.load();
}
