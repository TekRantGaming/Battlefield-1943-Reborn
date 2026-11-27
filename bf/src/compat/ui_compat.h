// Battlefield 1943 Reborn, SDK v0.11+ port (phase 3): the game's one window facade and dialog host on v0.11's UI system.
// The app attaches them once the systems run (OnInit) and detaches them at shutdown.
#pragma once

#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>

namespace rex::ui::compat {

void Attach(IUISystem& ui);
void Detach();
// Null before Attach, and the window also when the program runs without one (a dedicated server).
Window* GetWindow();
ImGuiDrawer* GetDrawer();
IUISystem* GetUI();

}  // namespace rex::ui::compat
