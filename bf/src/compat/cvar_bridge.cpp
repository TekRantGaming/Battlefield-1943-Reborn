// Battlefield 1943 Reborn, SDK v0.11+ port: see cvar_bridge.h.
#include "cvar_bridge.h"

#include <atomic>
#include <mutex>
#include <set>
#include <utility>

#include <rex/logging.h>
#include <rex/settings_registry.h>

namespace rex::cvar::bridge {

namespace {

// The SDK cvars the game reads or sets by their v0.10 names, where the SDK's own old names (src/base/settings/
// aliases.cpp) do not cover them. An empty new name: no v0.11 setting (the SDK patch that made it is not ported yet).
constexpr std::pair<std::string_view, std::string_view> kNames[] = {
    {"anisotropic_override", "gpu.anisotropic_override"},
    {"cache_root", "app.cache_root"},
    {"user_data_root", "app.user_root"},
    {"bind_achievements", "ui.bind.achievements"},
    {"d3d12_allow_variable_refresh_rate_and_tearing", "gpu.d3d12.allow_tearing"},
    {"gpu_allow_invalid_fetch_constants", "gpu.xenos.allow_invalid_fetch_constants"},
    {"license_mask", "kernel.license_mask"},
    {"occlusion_query_enable", "gpu.xenos.occlusion_query_enable"},
    {"present_letterbox", "gpu.xenos.present_letterbox"},
    {"store_shaders", "gpu.xenos.store_shaders"},
    {"vulkan_allow_present_mode_immediate", "gpu.vulkan.allow_present_mode_immediate"},
    {"vulkan_allow_present_mode_mailbox", "gpu.vulkan.allow_present_mode_mailbox"},
    {"vulkan_device", "gpu.vulkan.device"},
    {"d3d12_adapter", "gpu.d3d12.adapter"},
    {"fullscreen", "ui.fullscreen"},
    {"log_file", "log.file_name"},
    {"mnk_mode", "hid.mnk_mode"},
    {"mnk_mouse", "hid.mnk_mouse"},
    {"mnk_sensitivity", "hid.mnk_sensitivity"},
    {"protect_zero", "runtime.protect_zero"},
    // Our v0.10 SDK patches, kept in the port's SDK.
    {"guest_invalid_call_skip", "runtime.guest_invalid_call_skip"},
    {"job_wait_floor_ms", "runtime.job_wait_floor_ms"},
    {"window_start", "ui.window_start"},
    {"cl", "kernel.command_line"},  // the guest's command line (a dedicated server's -dedicated -level ...)
};

void WarnOnce(std::string_view name, std::string_view what) {
  static std::mutex mutex;
  static std::set<std::string, std::less<>> warned;
  std::lock_guard lock(mutex);
  if (warned.emplace(name).second) {
    REXLOG_WARN("Settings: {} ({}): {}", name, NewName(name), what);
  }
}

}  // namespace

std::string_view NewName(std::string_view old_name) {
  for (const auto& [from, to] : kNames) {
    if (from == old_name) return to;
  }
  return old_name;
}

namespace {
std::atomic<bool> g_title_launched{false};
}  // namespace

void SetTitleLaunched() { g_title_launched.store(true, std::memory_order_relaxed); }

bool Set(std::string_view old_name, std::string_view value) {
  const std::string_view name = NewName(old_name);
  if (name.empty()) {
    WarnOnce(old_name, "no SDK setting in this build");
    return false;
  }
  const rex::SettingLayer layer =
      g_title_launched.load(std::memory_order_relaxed) ? rex::SettingLayer::kRuntime : rex::SettingLayer::kTitle;
  auto result = REX_SETTINGS().SetText(layer, name, value);
  if (!result.has_value()) {
    WarnOnce(old_name, result.error().GetDescription());
    return false;
  }
  return true;
}

std::optional<std::string> Get(std::string_view old_name) {
  const std::string_view name = NewName(old_name);
  if (name.empty()) return std::nullopt;
  auto found = REX_SETTINGS().Find(name);
  // The keyboard's bindings (keybind_a ... keybind_guide) are hid.keybind_a ... in v0.11. Its old names
  // (src/base/settings/aliases.cpp) are followed by SetText, not by Find, so every read of them gave nothing: the
  // vehicles' key sets were seeded empty at the first start (issue 90: in a vehicle the keyboard did nothing), and the
  // bindings' infantry rows and the HUD's key prompts read no keys (0.4.8.2).
  if ((!found.has_value() || !*found) && name.starts_with("keybind_"))
    found = REX_SETTINGS().Find("hid." + std::string(name));
  if (!found.has_value() || !*found) return std::nullopt;
  return (*found)->GetText();
}

}  // namespace rex::cvar::bridge
