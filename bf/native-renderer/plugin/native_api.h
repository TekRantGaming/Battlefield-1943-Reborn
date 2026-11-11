// The C interface between the game's Direct3D hooks (bf/src/native_hooks.cpp) and rexgpu-bfnative, the way King
// Kong's native renderer does it (native-renderer/plugin/native_api.h, tag v2.1.0). The hooks find it through the
// plugin's export bf_native_get_api when gpu_plugin is "bfnative".
//
// M1: the hooks report every library call (OnCall) and each frame's end (OnFrameEnd); the plugin counts them. The
// drawing calls come in M2.
#pragma once

#include <cstdint>

#define BF_NATIVE_API_VERSION 1

extern "C" {

struct BfNativeApi {
  uint32_t version;  // BF_NATIVE_API_VERSION
  uint32_t size;     // sizeof(BfNativeApi)
  // One Direct3D library call: its guest address (the hook table's entry), how many hooked library calls it runs
  // inside (0: the engine called it), r3 to r6.
  void (*OnCall)(uint32_t address, uint32_t depth, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6);
  // The game presented a frame (VdSwap).
  void (*OnFrameEnd)();
};

using BfNativeGetApiFn = const BfNativeApi* (*)();

}  // extern "C"
