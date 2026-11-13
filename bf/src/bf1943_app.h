// bf1943 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/cvar.h>
#if !defined(_WIN32)
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#endif
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>

#include <native_x_renderer/native_x_renderer.h>

#include "gpu_preference.h"
#include "game_install.h"
#include "private_match.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <thread>
#include <utility>

namespace bf1943 {
void InstallFpeGuard();  // fpe_guard.cpp
void RaiseTimerResolution();  // frame_rate.cpp
void ExtractGameArt(const std::filesystem::path& game, const std::filesystem::path& out_dir);  // shader_prep.cpp
void SetLevelGameRoot(const std::filesystem::path& game);  // shader_prep.cpp: the level's water height for the grass
void HdExport(const std::filesystem::path& game);          // shader_prep.cpp: developer, the HD pack's source
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
void CreateSpawnMouseOverlay(rex::ui::ImGuiDrawer* drawer);             // flash_mouse.cpp
void CreateGraphicsPanel(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window);  // f8_panel.cpp
void InstallHudGlyphs();                                                     // hud_glyphs.cpp
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
#if defined(BF_DEV_TOOLS)
std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode);  // dev_pad.cpp
void StartStackSampler();  // dev_stacks.cpp
#endif
#if !defined(_WIN32)
// Linux: every controller drives the one player, merged with the keyboard and mouse (the runtime's own assignment
// gives each pad a player of its own, and the game reads player 1's only). A handheld shows several pads at once: on
// the ROG Ally InputPlumber presents its built in controller beside other virtual ones, and the game read none of
// its presses on the menus (TekRantGaming, 11 Nov 2026). Battlefield 1943 has one player a console, so nothing is lost.
inline std::unique_ptr<rex::system::IInputSystem> CreateSharedInputSystem(bool tool_mode) {
  auto system = rex::input::CreateDefaultInputSystem(tool_mode);
  system->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
  return system;
}
#endif
}  // namespace bf1943

REXCVAR_DECLARE(bool, server);  // server_mode.cpp
REXCVAR_DECLARE(std::string, install_game);  // game_install.cpp

// Developer A/B switch (speed comparisons): BF_AB_OFF lists pieces to leave out (overlay, input, audio).
inline bool AbOff(const char* piece) {
  const char* v = std::getenv("BF_AB_OFF");
  return v && std::string(v).find(piece) != std::string::npos;
}

class Bf1943App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Bf1943App>(new Bf1943App(ctx, "bf1943",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    bf1943::InstallCrashReports(paths_.user_data_root);  // first: a crash from here on leaves a report
    bf1943::RaiseTimerResolution();  // 1 ms sleeps for the game's waits and our frame limit (frame_rate.cpp)
    bf1943::ApplyStartupDisplaySettings(paths_.user_data_root);  // before the window is made
    bf1943::ApplyPlayerIdentity(paths_.user_data_root);          // the player's own name and XUID
    // Every graphics device on the high-performance GPU (King Kong's fix, gpu_preference.cpp): before the GPU plugin
    // makes its provider below, unless the player chose one (vulkan_device, d3d12_adapter). Hybrid laptops ran the game
    // on the integrated GPU (TekRantGaming, 11 Nov 2026). NativeXRenderer's own device takes the same vulkan_device.
    if (!REXCVAR_GET(server)) {
      bf1943::PreferHighPerformanceGpu(bf1943::GpuApi::kVulkan);
#if defined(_WIN32) && defined(BF_DEV_TOOLS)
      bf1943::PreferHighPerformanceGpu(bf1943::GpuApi::kD3D12);  // the Xenos plugin's D3D12 backend (developer builds)
#endif
    }
    // The renderer: our Vulkan one (rexgpu-bfnative); a dedicated server's draws nothing (its null backend). In the
    // developer build a gpu_plugin on the command line or in bf1943.toml still wins (xenos: reference frames).
    // A dedicated server's window is never shown (our SDK copy's window_start), so nothing pops up over the player's
    // screen; before, it showed and was hidden a second later (server_mode.cpp).
    if (REXCVAR_GET(server)) rex::cvar::SetFlagByName("window_start", "hidden");
    // Keyboard & mouse is on by default for players (our SDK copy); a dedicated server has no player.
    if (REXCVAR_GET(server)) rex::cvar::SetFlagByName("mnk_mode", "false");
    if (REXCVAR_GET(server) && !config.graphics) {
      config.graphics = rex::system::LoadGpuPlugin("bfnative", "null");
      config.gpu_plugin = "bfnative";
    }
#if !defined(BF_DEV_TOOLS)
    config.gpu_plugin = "bfnative";
#endif
    if (!config.graphics && config.gpu_plugin.empty()) config.gpu_plugin = bf1943::StartupGpuPlugin(paths_.user_data_root);
#if defined(BF_DEV_TOOLS)
    // BF_GPU_BACKEND=vulkan or d3d12: the Xenos plugin's backend for A/B tests (the default picks D3D12 first).
    if (const char* backend = std::getenv("BF_GPU_BACKEND"); backend && *backend && !config.graphics) {
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, backend);
      REXLOG_INFO("BF GPU backend: {}", backend);
    }
#endif
    if (!config.audio_factory)
      config.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
    // Arcade games run as their trial unless the license says the game was bought. Players install from their own
    // copy, so the full game is always unlocked.
    rex::cvar::SetFlagByName("license_mask", "1");
#if defined(BF_DEV_TOOLS)
    // BF_DEV_PAD scripted pad; without BF_DEV_PAD this is the runtime's default input system (on Linux every pad is
    // the one player's, as below).
    config.input_factory = bf1943::CreateInputSystem;
#elif !defined(_WIN32)
    config.input_factory = bf1943::CreateSharedInputSystem;
#endif
  }

  void OnPostSetup() override {
    // The window's title (TekRantGaming, 10 Nov 2026): the game's name, not the SDK's "bf1943" and its build stamp.
    if (window()) window()->SetTitle("Battlefield 1943 Reborn");
    bf1943::InstallFpeGuard();  // after the runtime's own signal handlers
    bf1943::HideServerWindows();  // a dedicated server shows no window (server_mode.cpp)
    // NativeXRenderer (native_x_renderer_bf1943.cpp), before the guest starts. Off unless its settings are on
    // (--native_x_renderer, _trace, _verify); a dedicated server draws nothing at all.
    if (!REXCVAR_GET(server)) {
      native_x_renderer::Initialize(bf1943::NativeXRendererProfile(), runtime() ? runtime()->graphics_system() : nullptr,
                                    window(), runtime() ? runtime()->cache_root() : std::filesystem::path());
    }
#if defined(BF_DEV_TOOLS)
    bf1943::StartStackSampler();  // BF_DEV_STACKS
#endif
  }

  // The player's download starts from bf1943.exe itself (TekRantGaming, 7 Nov 2026: an exe, never a .bat or script; the old
  // play.ps1 passed these). Without --user_data_root the player's data goes beside the exe in "user" (cache and log
  // there too), and the release always draws with our Vulkan renderer. The server package still passes its own.
  // Just before the game starts: the backend's answer to the player's name (game_menu.cpp, a number added when another
  // player online has it), so the game only ever sees one name.
  void OnPreLaunchModule() override { bf1943::FinishNameCheck(); }

  // The game's module is launched: the time, for a game that ends while it is still starting (OnShutdown).
  void OnPostLaunchModule(rex::system::XThread* thread) override {
    (void)thread;
    launched_at_ = std::chrono::steady_clock::now();
  }

  // The app is quitting by itself (the SDK copy's ReXApp::OnDestroy; a window close never comes here). When that is
  // because the game could not start, the player is told, with where the log is (start_failure.cpp; TekRantGaming,
  // 13 Nov 2026: a player's game closed 2 s after its launcher came up and said nothing). A dedicated server has
  // nobody to tell.
  void OnShutdown() override {
    if (REXCVAR_GET(server)) return;
    const double since_launch =
        launched_at_ == std::chrono::steady_clock::time_point{}
            ? -1.0
            : std::chrono::duration<double>(std::chrono::steady_clock::now() - launched_at_).count();
    std::filesystem::path log = rex::cvar::GetFlagByName("log_file");
    if (log.empty() && !paths_.user_data_root.empty()) log = paths_.user_data_root / "bf1943.log";
    bf1943::TellStartFailure(rex::SelfQuitWhy(), rex::SelfQuitReason(), since_launch, log, rex::SelfQuitCode(),
                             window() ? window()->GetNativeWindowHandle() : nullptr);
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
#if !defined(BF_DEV_TOOLS)
    if (REXCVAR_GET(user_data_root).empty() && !REXCVAR_GET(server)) {
      const std::filesystem::path user = bf1943::PlayerRoot() / "user";
      std::error_code ec;
      std::filesystem::create_directories(user, ec);
      paths.user_data_root = user;
      if (REXCVAR_GET(cache_root).empty()) paths.cache_root = user / "cache";
      rex::cvar::SetFlagByName("log_file", (user / "bf1943.log").string());
    }
    if (!REXCVAR_GET(server)) {
      rex::cvar::SetFlagByName("native_x_renderer", "true");
      rex::cvar::SetFlagByName("bf_engine_d3d_path", "true");
      // A player's log names every frame over 100 ms and what it waited on (NativeXRenderer's hitch lines), for play
      // test reports.
      if (std::atof(rex::cvar::GetFlagByName("native_x_renderer_hitch_ms").c_str()) <= 0.0)
        rex::cvar::SetFlagByName("native_x_renderer_hitch_ms", "100");
    }
#endif
    paths_ = paths;
  }

  // --install_game (the server package's setup): the game unpacked into its folder, then the process ends.
  void OnPostInitLogging() override {
    if (!REXCVAR_GET(install_game).empty()) std::exit(bf1943::install::InstallFromCommandLine(GameDir(paths_)));
  }

  // The game folder: --game_data_root, else the folder chosen at setup (an unpacked game used where it is), else
  // "game" beside the exe (the player's download: <root>\bf1943.exe, <root>\game), or beside its bin folder (the
  // server package: <root>\bin\bf1943.exe, <root>\game). Where "game" holds another game (King Kong Recompiled's
  // AppImage keeps its own there: both AppImages in Downloads), ours is "bf1943-game" (game_install.h GameFolderIn).
  std::filesystem::path GameDir(const rex::PathConfig& p) const {
    if (!p.game_data_root.empty()) return p.game_data_root;
    const std::filesystem::path saved = bf1943::SavedGameDir(p.user_data_root);
    if (bf1943::install::Installed(saved) && !bf1943::install::OtherGame(saved)) return saved;
    const std::filesystem::path exe = bf1943::PlayerRoot();
    const std::filesystem::path root = exe.filename() == "bin" ? exe.parent_path() : exe;
    const std::filesystem::path game = bf1943::install::GameFolderIn(root);
    if (bf1943::install::OtherGame(root / "game"))
      REXLOG_INFO("BF setup: {} holds another game (title {:08X}), so ours is {}", (root / "game").string(),
                  bf1943::install::XexTitleId(root / "game" / "default.xex"), game.string());
    return game;
  }

  // The first start (King Kong's setup idea): without the game installed, our menu's setup page asks for the
  // player's package and installs it, then the runtime starts as on every later start. A dedicated server is
  // installed by its package's setup (--install_game) and never asks.
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
    rex::PathConfig paths = defaults;
    paths.game_data_root = GameDir(defaults);
    paths_ = paths;
    bf1943::NotePrivateMatchPaths(paths.game_data_root, paths.user_data_root);  // our private match's server
    const bool installed = bf1943::install::Installed(paths.game_data_root) || REXCVAR_GET(server);
    const bool name = bf1943::NameNeeded(paths.user_data_root);
    // A player's start: the launcher before the game (TekRantGaming, 8 Nov evening), which starts it at Play.
    const bool launcher = !REXCVAR_GET(server) && bf1943::LauncherFirst();
    // Pictures and the menu music for our menus from the player's game files, once, on a worker: before the game runs,
    // so the launcher already has them (shader_prep.cpp, game_music.cpp).
    auto extract = [](const rex::PathConfig& p) {
      if (REXCVAR_GET(server)) return;
      const auto cache = p.cache_root.empty() ? p.user_data_root / "cache" : p.cache_root;
      bf1943::ExtractGameArt(p.game_data_root, cache / "extracted");
      // The level's water height for the grass, read when the game opens a level (shader_prep.cpp).
      bf1943::SetLevelGameRoot(p.game_data_root);
      bf1943::HdExport(p.game_data_root);  // developer: BF_DEV_HD_EXPORT, the HD pack's source pictures
      // An imported HD texture pack (Settings > Graphics): the renderer reads it when HD textures are on (D149).
      {
        std::error_code ec;
        const auto pack = p.user_data_root / "hd" / "bf1943-hd.bfhd";
        if (std::filesystem::exists(pack, ec)) rex::cvar::SetFlagByName("native_x_renderer_hd_pack", pack.string());
      }
      // The grass's plants from the game's own textures (made there, NativeXRenderer grass.cpp loads them).
      rex::cvar::SetFlagByName("native_grass_textures", (cache / "extracted" / "vegetation-2.bin").string());
    };
    if (installed) extract(paths);
    if (installed && !name && !launcher) return paths;
    // The pages finish inside the menu's drawing: the runtime starts after that paint (the renderer takes the window
    // from the presenter; done in the paint, the presenter's swap chain went away under it, a driver crash).
    // The player's name comes after the install (TekRantGaming, 7 Nov 2026: the launcher offers to create a username).
    auto start = [this, resume](rex::PathConfig p) {
      paths_ = p;
      app_context().CallInUIThreadDeferred([p, resume] { resume(p); });
    };
    auto then_name = [start, name, launcher, extract, installed, user = paths.user_data_root](rex::PathConfig p) {
      if (!installed) extract(p);  // just installed by the first start's setup
      if (name && bf1943::NameNeeded(user)) bf1943::BeginNameSetup([start, p] { start(p); });
      else if (launcher) bf1943::BeginLauncher([start, p] { start(p); });
      else start(p);
    };
    if (installed) {
      then_name(paths);
      return std::nullopt;
    }
    REXLOG_INFO("BF setup: no game in {}: the first start's setup", paths.game_data_root.string());
    bf1943::BeginGameSetup(paths.game_data_root, [paths, then_name](std::filesystem::path game) mutable {
      paths.game_data_root = std::move(game);
      then_name(paths);
    });
    return std::nullopt;
  }

  void OnConfigureFonts(ImFontAtlas* atlas) override { bf1943::LoadMenuFonts(atlas); }

  // Our main menu inside the game (game_menu.cpp), drawn over BF's own menu scene.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    bf1943::CreateGameMenu(
        drawer, paths_.user_data_root,
        [this](bool on) {
          app_context().CallInUIThreadDeferred([this, on] {
            if (window()) window()->SetFullscreen(on);
          });
        },
        // Quit as the window's close button does (ReXApp::OnClosing: the title ended, the log flushed, a hard exit):
        // the runtime's own quit waited on the running game for ever (run quit2).
        [this] {
          app_context().CallInUIThreadDeferred([this] {
            if (window()) window()->RequestClose();
            else app_context().QuitFromUIThread();
          });
        },
        [this] { return window(); }, [this] { return paths_.game_data_root; });
    if (!AbOff("overlay")) bf1943::CreatePerfOverlay(drawer);  // Settings > Gameplay > Performance metrics, F2
    if (!REXCVAR_GET(server)) bf1943::CreateCommsOverlay(drawer);  // text chat top left, voice chat (comms_ui.cpp)
    if (!REXCVAR_GET(server)) bf1943::CreateSpawnMouseOverlay(drawer);  // the deploy screen's pointer
    if (!REXCVAR_GET(server)) bf1943::CreateGraphicsPanel(drawer, window());  // F8 (f8_panel.cpp)
    if (!REXCVAR_GET(server)) bf1943::InstallHudGlyphs();  // the game's prompts in the chosen style (hud_glyphs.cpp)
    bf1943::InstallLook(window());  // the camera and look system (look.cpp)
    bf1943::InstallPlaytestMark(window());  // R marks a moment in a watched play test (playtest_mark.cpp)
    // The SDK's own overlays stay shut (TekRantGaming, 10 Nov 2026): F3 its debug overlay, F4 its settings and the backtick
    // its console (both of those change any setting). Their binds lose their actions, so no key saved in a settings
    // file or given on the command line brings them back. (F7, its achievements, is the F8 panel's compare key.)
    // BF_DEV_SDK_BINDS=1 (tests): kept, to show the old behaviour.
#if defined(BF_DEV_TOOLS)
    const bool keep_sdk_binds = std::getenv("BF_DEV_SDK_BINDS") != nullptr;
#else
    constexpr bool keep_sdk_binds = false;
#endif
    if (!keep_sdk_binds)
      for (const char* bind : {"bind_debug_overlay", "bind_settings", "bind_console"}) rex::ui::UnregisterBind(bind);
#if defined(BF_DEV_TOOLS)
    // BF_DEV_KEY_PROBE=<s> (tests): that long after start, F3, F4 and the backtick go to the binds as a key press in the
    // window does (ReXApp::OnKeyDown), then F2 (ours, the check); whether a bind took each is logged.
    if (const char* v = std::getenv("BF_DEV_KEY_PROBE"); v && *v) {
      const double after = std::atof(v);
      std::thread([this, after] {
        std::this_thread::sleep_for(std::chrono::milliseconds(int64_t(after * 1000)));
        app_context().CallInUIThreadDeferred([this] {
          const std::pair<const char*, rex::ui::VirtualKey> keys[] = {{"F3", rex::ui::VirtualKey::kF3},
                                                                      {"F4", rex::ui::VirtualKey::kF4},
                                                                      {"Backtick", rex::ui::VirtualKey::kOem3},
                                                                      {"F2", rex::ui::VirtualKey::kF2}};
          for (const auto& [name, vk] : keys) {
            rex::ui::KeyEvent e(window(), vk, 0, false, false, false, false, false);
            const bool taken = rex::ui::ProcessKeyEvent(e);
            REXLOG_INFO("BF dev key probe: {} {}", name, taken ? "taken by a bind" : "does nothing");
          }
        });
      }).detach();
    }
#endif
  }

  // Shaders from the player's game files into the runtime's storage, before the runtime opens it.
  void OnPostLoadXexImage() override {
    const auto cache = paths_.cache_root.empty() ? paths_.user_data_root / "cache" : paths_.cache_root;
    // First, as King Kong does (shader_prepared.h), from the marker and the storage as they were: after an update the
    // shader page makes every pipeline again (nothing deleted); a version already prepared shows no shader page
    // (game_menu.cpp, update.cpp).
    if (!REXCVAR_GET(server)) bf1943::CheckShadersAfterUpdate(paths_.user_data_root, cache);
    bf1943::PrepareShaders(bf1943::ExecutableDir() / "data" / "shaders", paths_.game_data_root, cache);
    // The pipelines this game builds, for the next update's shader list (Settings > Gameplay > Share graphics data).
    bf1943::StartPipelineUploads(paths_.user_data_root, cache);
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;

 private:
  rex::PathConfig paths_;
  std::chrono::steady_clock::time_point launched_at_{};  // OnPostLaunchModule; zero while the game was never launched
};
