/**
 * @file        gpu/bfnative_hooks.h
 * @brief       The Xenos command processor's hooks for a renderer that draws the title's frames itself
 *
 * @license     BSD 3-Clause License
 *
 * @remarks     Battlefield 1943 Reborn's patch of the SDK (v0.10's IGraphicsSystem additions, kept for v0.11).
 *              NativeXRenderer, in the program, draws the guest's frames from the title's Direct3D calls. The
 *              Xenos driver "bfnative" (rexgpu-xenos-bfnative) runs the guest's command stream for its signals only
 *              (fences, interrupts, swaps), draws nothing and has no presenter, and gives the renderer these hooks
 *              through a C table, so nothing C++ crosses the module boundary. The program finds the table with
 *              the module's export kBfNativeHooksSymbol.
 */

#pragma once

#include <cstdint>

namespace rex {
class IPhysicalMemory;
class IPhysicalWriteWatch;
}  // namespace rex
namespace rex::gpu {
class XenosSettings;
}  // namespace rex::gpu

namespace rex::gpu::bfnative {

/** A draw packet the command processor executed. The pointers are valid only during the callback. */
struct DrawInfo {
  uint32_t packet_address;     /**< Physical address of the draw packet's type 3 header. */
  uint32_t opcode;             /**< PM4_DRAW_INDX or PM4_DRAW_INDX_2. */
  const uint32_t* registers;   /**< The register file after the packet. */
  uint32_t register_count;
  uint64_t bin_mask;           /**< Predicated tiling: the packet ran because bin_mask & bin_select != 0. */
  uint64_t bin_select;
  bool issued;                 /**< False when the draw was dropped (always, in the bfnative driver). */
  uint64_t vertex_shader_hash; /**< 0 when none. */
  uint64_t pixel_shader_hash;
  const void* vertex_shader_constants; /**< Shader::ConstantRegisterMap, null until analyzed. */
  const void* pixel_shader_constants;
  uint8_t vertex_shader_memexport; /**< eM# written by memexport, 0 for none. */
  uint8_t pixel_shader_memexport;
};

using DrawObserver = void (*)(void* user, const DrawInfo& info);
/** Answers a guest occlusion query that ended: the samples that passed, or false for the fake sample count. */
using OcclusionQueryResolver = bool (*)(void* user, uint32_t sample_count_address, uint64_t* samples);
/** Called before the command processor shows the guest its progress (a fence, an interrupt, a swap). */
using SignalGate = void (*)(void* user, uint64_t tag);
/** Called when the guest moves the ring buffer's write pointer: the tag the gate gets for what came before. */
using KickTag = uint64_t (*)(void* user);
/** Called after every swap the command processor executed, with the number executed so far. */
using SwapObserver = void (*)(void* user, uint64_t swaps_executed);
using DetachObserver = void (*)(void* user);

/** The guest's display gamma ramp (DC_LUT_30_COLOR table and DC_LUT_PWL_DATA), as the guest wrote it. */
struct GammaRamp {
  uint32_t table[256];
  uint32_t pwl[128][3];
  uint64_t version; /**< Changes whenever the guest writes either of them. */
};

inline constexpr uint32_t kHooksVersion = 2;

/** The table the bfnative driver module exports. Every call is safe from any thread. */
struct Hooks {
  uint32_t version; /**< kHooksVersion */
  uint32_t size;    /**< sizeof(Hooks) */
  /** True once the driver runs a command processor: the setters below then take effect. */
  bool (*IsActive)();
  bool (*SetDrawObserver)(DrawObserver observer, void* user);
  bool (*SetOcclusionQueryResolver)(OcclusionQueryResolver resolver, void* user);
  /** gate null with kick_tag null answers whether gates are supported, and sets nothing. */
  bool (*SetSignalGate)(SignalGate gate, void* user, KickTag kick_tag);
  /** The title's VdSwap wrote a swap into the ring buffer. */
  void (*NoteSwapIssued)();
  bool (*GetSwapCounts)(uint64_t* issued, uint64_t* executed);
  bool (*SetSwapObserver)(SwapObserver observer, void* user);
  bool (*GetGammaRamp)(GammaRamp* ramp);
  /**
   * The title's physical memory and its write watches, as the driver got them when the title attached (null before):
   * the renderer mirrors guest memory on them (an SDK SharedMemory of its own). These two are the SDK's C++
   * interfaces (rex/guest.h), which already cross module boundaries.
   */
  IPhysicalMemory* (*GetPhysicalMemory)();
  IPhysicalWriteWatch* (*GetPhysicalWriteWatch)();
  /** The driver's gpu.xenos settings, for the renderer's own texture cache (one set registered, the driver's). */
  const XenosSettings* (*GetXenosSettings)();
  /**
   * Called when the title detaches, after the command processor stopped and before the title context is released
   * (which waits for every write listener on the title's memory to go): the renderer releases what it holds on the
   * title's memory there (its SharedMemory's and PrimitiveProcessor's listeners). On the detaching thread. (Version 2.)
   */
  bool (*SetDetachObserver)(DetachObserver observer, void* user);
};

using GetHooksFn = const Hooks* (*)();
inline constexpr const char* kBfNativeHooksSymbol = "rex_bfnative_hooks";

}  // namespace rex::gpu::bfnative
