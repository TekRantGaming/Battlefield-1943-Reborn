// bf1943 - ReXGlue Recompiled Project
//
// The game's app on the SDK's TitleApp (SDK v0.11+ port, phase 3; .handoff/SDK-0.11-PORT-PLAN.md). Under v0.10 it was a
// ReXApp with our SDK copy's extra hooks; their work now sits in TitleApp's hooks and in GameMain (main.cpp):
//   v0.10 OnConfigurePaths, OnPreSetup     -> OnConfigure (paths, logs, the renderer, settings at the title layer),
//                                             OnSettingsSettled (the player's folder read: the command line applied)
//   v0.10 OnPostSetup, OnCreateDialogs     -> OnInit (the window and the UI system run; our menus and overlays)
//   v0.10 OnFinalizePaths (setup, name, launcher before the game)  -> GameMain: RunFrame until the pages are done
//   v0.10 OnPreLaunchModule, OnPostLoadXexImage                    -> OnTitleLaunch (loaded, not yet started)
//   v0.10 OnShutdown (the player told why the game did not start)  -> ReportError, OnTitleExit
//   v0.10 OnWindowMinimized, OnWindowRestored, OnWindowFocusChanged -> a UI event listener
//   v0.10 CreateAchievementNotificationDialog -> our toast, given the title's achievement events (OnTitleLaunch)

#pragma once

#include <rex/app/title_app.h>
#include <rex/cvar.h>
#include <rex/kernel/title_exit.h>
#include <rex/kernel/title_runtime.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/settings_registry.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/ui_system.h>
#include <rex/xam/achievement_manager.h>

#include <native_x_renderer/native_x_renderer.h>

#include "compat/ui_compat.h"
#include "gpu_preference.h"
#include "game_install.h"
#include "private_match.h"
#include "report.h"
#include "achievement_toast.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace bf1943 {
void InstallFpeGuard();  // fpe_guard.cpp
#if defined(BF_DEV_TOOLS)
bool DevPadOn();                 // dev_pad.cpp: BF_DEV_PAD is set
rex::IModule* DevPadModule();    // dev_pad.cpp: its input driver's module (null without BF_DEV_PAD)
#endif
void RaiseTimerResolution();  // frame_rate.cpp
void SetPacingWindow(void* native_window);  // frame_rate.cpp: the frame limit while minimised
void NoteWindowMinimized(bool minimized);   // frame_rate.cpp (not Windows: from the window's events)
void ExtractGameArt(const std::filesystem::path& game, const std::filesystem::path& out_dir);  // shader_prep.cpp
void SetLevelGameRoot(const std::filesystem::path& game);  // shader_prep.cpp: the level's water height for the grass
void HdExport(const std::filesystem::path& game);          // shader_prep.cpp: developer, the HD pack's source
void HdExportUi(const std::filesystem::path& game);        // shader_prep.cpp: BF_HD_EXPORT_UI, the menus' and HUD's
void PrepareShaders(const std::filesystem::path& data_dir, const std::filesystem::path& game,
                    const std::filesystem::path& cache);  // shader_prep.cpp
std::filesystem::path ExecutableDir();                          // shader_prep.cpp
std::filesystem::path PlayerRoot();  // shader_prep.cpp: beside the exe, or on Linux beside the AppImage file
void LoadMenuFonts(ImFontAtlas* atlas);                         // game_menu.cpp
void ApplyStartupDisplaySettings(const std::filesystem::path& user_dir);  // game_menu.cpp
void ApplyPlayerIdentity(const std::filesystem::path& user_dir);           // game_menu.cpp
void FinishNameCheck();                                                    // game_menu.cpp
bool NameNeeded(const std::filesystem::path& user_dir);                    // game_menu.cpp
void BeginNameSetup(std::function<void()> done);                           // game_menu.cpp
bool LauncherFirst();                                                      // game_menu.cpp
void BeginLauncher(std::function<void()> start);                           // game_menu.cpp
void SetPlayerIdentity(const std::string& name, uint32_t id);              // live_layers.cpp
std::string StartupGpuPlugin(const std::filesystem::path& user_dir);      // game_menu.cpp
void HideServerWindows();                                                // server_mode.cpp
void CreatePerfOverlay(rex::ui::ImGuiDrawer* drawer);                    // perf_overlay.cpp
void CreateCommsOverlay(rex::ui::ImGuiDrawer* drawer);                  // comms_ui.cpp
void CreateWorldAdminPanel(rex::ui::ImGuiDrawer* drawer);               // world_sync.cpp: the match admin's panel (F9)
void CreateUiHider(rex::ui::ImGuiDrawer* drawer);  // free_cam.cpp: the hidden HUD and UI (made last: drawn last)
void CreateSpawnMouseOverlay(rex::ui::ImGuiDrawer* drawer);             // flash_mouse.cpp
void CreateGraphicsPanel(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window);  // f8_panel.cpp
void InstallHudGlyphs();                                                     // hud_glyphs.cpp
void InstallSharpText(const std::filesystem::path& game_dir);                // sharp_text.cpp
void InstallVehicleBinds();  // vehicle_binds.cpp: the keyboard's keys from the seat's binding set
void InstallLook(rex::ui::Window* window);                               // look.cpp
void InstallPlaytestMark(rex::ui::Window* window);                       // playtest_mark.cpp: BF_MARK_FILE
void InstallCrashReports(const std::filesystem::path& user_root);        // crash_report.cpp
void TellStartFailure(rex::SelfQuitCause cause, const char* reason, double since_launch,
                      const std::filesystem::path& log, int exit_code, void* native_window);  // start_failure.cpp
void StartPipelineUploads(const std::filesystem::path& user_dir,
                          const std::filesystem::path& cache_dir);       // pipeline_upload.cpp
void BeginGameSetup(const std::filesystem::path& game_dir, std::function<void(std::filesystem::path)> done);  // game_menu
std::filesystem::path SavedGameDir(const std::filesystem::path& user_dir);  // game_menu.cpp
void CheckShadersAfterUpdate(const std::filesystem::path& user_dir, const std::filesystem::path& cache);  // game_menu
const native_x_renderer::Profile& NativeXRendererProfile();              // native_x_renderer_bf1943.cpp
void CreateGameMenu(rex::ui::ImGuiDrawer* drawer, const std::filesystem::path& user_dir,
                    std::function<void(bool)> fullscreen, std::function<void()> quit,
                    std::function<rex::ui::Window*()> window,
                    std::function<std::filesystem::path()> game_dir);  // game_menu.cpp
void WatchGpuDeviceLoss(std::function<void(std::function<void()>)> in_ui, std::function<void()> close);  // gpu_lost.cpp
#if defined(BF_DEV_TOOLS)
void StartStackSampler();  // dev_stacks.cpp
#endif
}  // namespace bf1943

REXCVAR_DECLARE(bool, server);  // server_mode.cpp
REXCVAR_DECLARE(std::string, install_game);  // game_install.cpp
REXCVAR_DECLARE(bool, install_progress);  // game_install.cpp

// Developer A/B switch (speed comparisons): BF_AB_OFF lists pieces to leave out (overlay, input, audio).
inline bool AbOff(const char* piece) {
  const char* v = std::getenv("BF_AB_OFF");
  return v && std::string(v).find(piece) != std::string::npos;
}

namespace bf1943 {
// A setting of the SDK's, as text (empty when there is none).
inline std::string SdkSetting(std::string_view name) {
  auto found = REX_SETTINGS().Find(name);
  return found.has_value() && *found ? (*found)->GetText() : std::string();
}
}  // namespace bf1943

class Bf1943App : public rex::app::TitleApp {
 public:
  Bf1943App();

  // The pages before the game (the first start's setup, the player's name, the launcher): true once one is showing,
  // and PreGameDone() turns true when it hands the start over (GameMain runs frames until then).
  bool BeginPreGame();
  bool PreGameDone() const { return pre_game_done_; }
  void NoteLaunched() { launched_at_ = std::chrono::steady_clock::now(); }
  // Before Shutdown: what the game registered on the title (its achievement events, OnTitleAttached) let go while the
  // title is still loaded. Shutdown unloads it (TitleApp::OnStop, final) before OnShutdown runs, and a registration
  // released after that reached the freed achievement manager (the port's quit, 25 Nov 2026: a crash in rexxam).
  void ReleaseTitleHooks() {
    toast_events_.Reset();
    title_attached_.Reset();
  }
  // The game folder the pages settled on, for TitleRuntime::Load (title.game_root).
  void ApplyGameRoot();

 protected:
  rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override;
  // The work that reads the player's folder: every settings layer applied by then (our SDK copy's hook).
  rex::Result<void> OnSettingsSettled(rex::SettingsRegistry& settings) override;
  // A dedicated server needs no window (TitleApp's own requirements do): a Linux VPS has no display, and v0.11's UI
  // module refused to start there ("the app needs a window"). With ui.driver = none (OnConfigure) SDL's video is not
  // started at all, so its offscreen driver's event loop (800 wakeups a second, v0.10) is gone too.
  rex::AppSysRequirements GetRequirements() const override {
    rex::AppSysRequirements needs = TitleApp::GetRequirements();
    if (REXCVAR_GET(server)) needs.ui = false;
    return needs;
  }
  rex::Result<void> OnInit() override;
  void OnRegisterModules(std::vector<rex::IModule*>& modules) override;
  rex::Result<void> OnTitleLaunch(rex::kernel::TitleRuntime& title) override;
  void OnTitleExit(rex::kernel::TitleExitReason reason) override;
  void OnShutdown() override;
  void ReportError(std::string_view message) override;

 private:
  // The window's events the game follows: minimised and restored (the frame limit while minimised).
  class WindowEvents final : public rex::ui::IUIEventListener {
   public:
    void OnUIEvent(const rex::ui::UIEvent& event) override;
    void OnInputEvent(rex::InputEvent& event) override { (void)event; }
  };

  std::filesystem::path UserRoot() const { return rex::cvar::GetFlagByName("user_data_root"); }
  std::filesystem::path CacheRoot() const;
  std::filesystem::path LogFile() const;
  std::filesystem::path GameDir() const;
  void ExtractArt(const std::filesystem::path& game) const;
  void TellFailure(rex::SelfQuitCause cause, const char* reason);

  std::filesystem::path game_dir_;
  bool pre_game_done_ = false;
  std::unique_ptr<bf1943::AchievementToast> toast_;  // our achievement popup (none on a dedicated server)
  rex::ScopeGuard toast_events_;                      // the title's achievement events to the toast
  rex::ScopeGuard title_attached_;                    // NativeXRenderer's start (TitleRuntime::OnTitleAttached)
  WindowEvents window_events_;
  rex::ScopeGuard window_listener_;
  std::chrono::steady_clock::time_point launched_at_{};  // NoteLaunched; zero while the game was never launched
};

// main.cpp: the program's start (REX_TITLE_APP_MAIN).
int Bf1943Main(Bf1943App& app, const rex::CLIArgList& args);
