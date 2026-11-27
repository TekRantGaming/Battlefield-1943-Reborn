// Battlefield 1943 Reborn, SDK v0.11+ port (phase 3): v0.10's rex/ui/imgui_dialog.h, on v0.11's IDialog.
//
// A dialog adds itself to the UI system when made and leaves it when destroyed or closed, as in v0.10. Its hooks keep
// v0.10's names (OnDraw, OnShow, OnClose, Close) and run on the UI thread, as before.
#pragma once

#include <rex/ui/imgui_drawer.h>
#include <rex/ui/ui_system.h>
#include <rex/utility.h>

struct ImGuiIO;

namespace rex::ui {

class ImGuiDialog : public IDialog {
 public:
  ~ImGuiDialog() override = default;

  bool Draw(ImGuiIO& io) final {
    if (closing_) return false;
    OnDraw(io);
    return !closing_;
  }
  void OnClose() override {}
  bool NeedsContinuousPaint() const override { return true; }
  bool WantsExclusiveInput() const override { return false; }
  void OnShow() override {}
  bool OnToggle() override { return false; }

 protected:
  explicit ImGuiDialog(ImGuiDrawer* drawer) : drawer_(drawer) {
    if (drawer_) guard_ = drawer_->ui().AddDialog(*this);
  }

  ImGuiDrawer* imgui_drawer() const { return drawer_; }
  ImGuiIO& GetIO() { return drawer_->GetIO(); }
  // Closes the dialog after this frame (v0.10's has_close_pending_).
  void Close() { closing_ = true; }

  virtual void OnDraw(ImGuiIO& io) { (void)io; }

 private:
  ImGuiDrawer* drawer_;
  ScopeGuard guard_;
  bool closing_ = false;
};

}  // namespace rex::ui
