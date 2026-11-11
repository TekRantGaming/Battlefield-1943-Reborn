// How far the GPU plugin is with the pipelines from the shader storage (InitializeShaderStorage), for a host's
// "preparing the shaders" screen: the plugin exports rex_gpu_shader_storage_progress (plugin_main.cpp).
#pragma once

#include <atomic>
#include <cstdint>

namespace rex::graphics {

struct ShaderStorageProgress {
  std::atomic<uint32_t> queued{0};    // pipelines from the storage queued for creation
  std::atomic<uint32_t> done{0};      // of those, created
  std::atomic<uint32_t> loading{0};   // 1 while the storage's pipelines are being created
  std::atomic<uint32_t> finished{0};  // 1 once the storage has been loaded (nothing left to make)
};

inline ShaderStorageProgress& GetShaderStorageProgress() {
  static ShaderStorageProgress progress;
  return progress;
}

}  // namespace rex::graphics
