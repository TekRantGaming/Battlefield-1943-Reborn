/**
 * @file        gpu/xenos/bfnative/bfnative_xenos_graphics_driver.h
 * @brief       The Xenos driver of a renderer that draws the title's frames itself
 *
 * @license     BSD 3-Clause License
 *
 * @remarks     Battlefield 1943 Reborn's patch of the SDK. The driver "bfnative" (rexgpu-xenos-bfnative) runs the
 *              guest's command stream as the null driver does: fences, memory writes, interrupts and swaps, no draws,
 *              no host GPU and no presenter. NativeXRenderer, in the program, draws and presents the frames from the
 *              title's Direct3D calls, and steers this driver's command processor through the hooks the module
 *              exports (rex/gpu/bfnative_hooks.h). v0.10's rexgpu-bfnative plugin, as v0.11's driver module.
 */

#pragma once

#include <string>

#include "gpu/xenos/xenos_graphics_driver_base.h"

namespace rex::gpu::bfnative {

class BfNativeXenosGraphicsDriver final : public XenosGraphicsDriverBase {
 public:
  BfNativeXenosGraphicsDriver();
  ~BfNativeXenosGraphicsDriver() override;

  std::string_view GetName() const override { return "bfnative"; }

  /** Accepts the window and builds no presenter: the program's renderer presents. */
  [[nodiscard]] Result<void> SetSurface(const NativeWindow& window) override;

  /** The base's detach (the command processor stopped), then the program's detach observer (SetDetachObserver). */
  void DetachTitle() override;

  /** The title's physical memory and write watches while it is attached, else null. */
  IPhysicalMemory* GetPhysicalMemory() const { return physical_memory_; }
  IPhysicalWriteWatch* GetPhysicalWriteWatch() const { return write_watch_; }

 protected:
  void CreateProvider(bool with_presentation) override;

 private:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

/** The driver while one exists (the hooks act on its command processor), else null. */
BfNativeXenosGraphicsDriver* GetActiveDriver();

}  // namespace rex::gpu::bfnative
