// Battlefield 1943 Reborn, SDK v0.11+ port: the game's cvar registry (compat/cvar.cpp) passes the names it does not hold
// to the SDK's settings (rex::SettingsRegistry), under the SDK's v0.11 names.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace rex::cvar::bridge {

// The v0.11 setting for an SDK cvar's v0.10 name, or the name itself when none is listed (the SDK's own old names
// then still apply through SettingsRegistry::SetText). Empty for a v0.10 name with no v0.11 setting.
std::string_view NewName(std::string_view old_name);

// Sets an SDK setting by its v0.10 name. False when there is no such setting or it refused the value (logged once per
// name). Before the title launches at the title layer, below the config file, the environment and the command line
// (v0.10's startup settings never changed an option given on the command line: v0.11 applies some, ui.fullscreen
// among them, to the open window at once); from the launch on at the runtime layer, as v0.10's SetFlagByName.
bool Set(std::string_view old_name, std::string_view value);

// The title launched (Bf1943App::OnTitleLaunch): Set uses the runtime layer from now on.
void SetTitleLaunched();

// An SDK setting's value as text, by its v0.10 name; nothing when there is no such setting.
std::optional<std::string> Get(std::string_view old_name);

}  // namespace rex::cvar::bridge
