// Battlefield 1943 Reborn, SDK v0.11+ port (phase 3): v0.10's rex/ui/imgui_drawer.h.
//
// v0.10's ImGuiDrawer held the dialogs and drew them. In v0.11 the UI system holds the dialogs (IUISystem::AddDialog)
// and whoever presents draws them (DrawOverlays). This keeps the game's handle on the UI system under the old name.
#pragma once

#include <rex/ui/ui_system.h>

struct ImGuiIO;

namespace rex::ui {

class ImGuiDrawer {
 public:
  explicit ImGuiDrawer(IUISystem& ui) : ui_(&ui) {}

  ImGuiIO& GetIO() { return ui_->GetImGuiIO(); }
  bool HasDialogs() const { return ui_->HasDialogs(); }
  IUISystem& ui() const { return *ui_; }

 private:
  IUISystem* ui_;
};

}  // namespace rex::ui
