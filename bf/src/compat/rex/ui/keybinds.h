// Battlefield 1943 Reborn, SDK v0.11+ port (phase 3): v0.10's rex/ui/keybinds.h.
//
// v0.11 binds keys only to its overlays (ui.bind.<name>). The game's own binds (F2 metrics, NativeXRenderer's F10) are
// kept here: each is a cvar of the game's registry holding its key, as in v0.10, and a press of that key in the window
// runs its action on the UI thread (compat/ui_compat.cpp). UnregisterBind on the SDK's own v0.10 binds (debug overlay,
// settings, console) clears their v0.11 settings (ui.bind.debug, ui.bind.settings, ui.bind.console).
#pragma once

#include <functional>
#include <string_view>

#include <rex/ui/virtual_key.h>

namespace rex::ui {

void RegisterBind(std::string_view name, std::string_view default_key, std::string_view description,
                  std::function<void()> action);
void UnregisterBind(std::string_view name);

}  // namespace rex::ui
