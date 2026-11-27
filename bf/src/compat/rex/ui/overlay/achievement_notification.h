// Battlefield 1943 Reborn, SDK v0.11+ port (phase 3): v0.10's rex/ui/overlay/achievement_notification.h. Our popup
// (achievement_toast.h) derives from it; the app hands it the title's achievement events (bf1943_app.cpp) and turns
// the SDK's own popup off (ui.achievement_toast).
#pragma once

#include <rex/system/achievement_store.h>
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class AchievementNotificationDialog : public ImGuiDialog {
 public:
  // Any thread.
  virtual void Push(const rex::system::AchievementEvent& event) = 0;

 protected:
  using ImGuiDialog::ImGuiDialog;
};

}  // namespace rex::ui
