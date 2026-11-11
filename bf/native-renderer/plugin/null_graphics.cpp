// The GPU that draws nothing (null_graphics.h).

#include "null_graphics.h"

#include <rex/logging.h>

namespace bf1943::gpu {
using rex::X_STATUS;  // X_STATUS_SUCCESS names it unqualified
namespace {

class NullCommandProcessor : public rex::graphics::CommandProcessor {
 public:
  using CommandProcessor::CommandProcessor;

  void IssueSwap(uint32_t, uint32_t, uint32_t) override {}

 protected:
  bool SetupContext() override { return true; }
  void ShutdownContext() override {}
  rex::graphics::Shader* LoadShader(rex::graphics::xenos::ShaderType, uint32_t, const uint32_t*, uint32_t) override {
    return nullptr;
  }
  bool IssueDraw(rex::graphics::xenos::PrimitiveType, uint32_t, IndexBufferInfo*, bool) override { return true; }
  bool IssueCopy() override { return true; }
};

}  // namespace

rex::X_STATUS NullGraphicsSystem::SetupPresentation(rex::ui::WindowedAppContext*) {
  REXLOG_INFO("rexgpu-bfnative: null GPU (a dedicated server draws nothing)");
  return X_STATUS_SUCCESS;
}

std::unique_ptr<rex::graphics::CommandProcessor> NullGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<NullCommandProcessor>(this, kernel_state_);
}

}  // namespace bf1943::gpu
