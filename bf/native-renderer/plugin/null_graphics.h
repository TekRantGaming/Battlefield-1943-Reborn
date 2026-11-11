// A GPU that draws nothing, for dedicated servers (DECISIONS.md, 5 Nov 2026: servers render nothing). The base
// command processor still reads the game's command stream, so fences, interrupts, swaps and the ring buffer behave
// as the game expects; draws, copies and shaders are dropped, and there is no window, presenter or host GPU.

#pragma once

#include <memory>
#include <string>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>

namespace bf1943::gpu {

class NullGraphicsSystem : public rex::graphics::GraphicsSystem {
 public:
  std::string name() const override { return "null"; }
  // No window: nothing to present.
  rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override;

 protected:
  void CreateProvider(bool with_presentation) override {}

 private:
  std::unique_ptr<rex::graphics::CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace bf1943::gpu
