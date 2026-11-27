// The game's app on the SDK's TitleApp (bf1943_app.h has the map from v0.10's hooks).
#include "bf1943_app.h"

#include <rex/system/interfaces/graphics.h>

#include "generated/default/bf1943_init.h"
#include "compat/cvar_bridge.h"

#include <imgui.h>

#include <rex/app/app.h>
#include <rex/cli.h>

namespace {

// At the title layer: the config file, the environment and the command line still win (test runs' --fullscreen=false).
void SetSdk(rex::SettingsRegistry& settings, std::string_view name, std::string_view value) {
  if (auto r = settings.SetText(rex::SettingLayer::kTitle, name, value); !r.has_value())
    REXLOG_WARN("BF setup: {} = {} not set: {}", name, value, r.error().GetDescription());
}

double SecondsSince(std::chrono::steady_clock::time_point t) {
  if (t == std::chrono::steady_clock::time_point{}) return -1.0;
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

#if !defined(BF_DEV_TOOLS)
// The player's folder: "user" beside the exe, or on Linux RecompLauncher's data folder when it starts the game
// (RECOMPLAUNCHER_DATA, <install>/recompdata). The launcher runs the unpacked AppImage, so beside the exe is
// <install>/usr/bin, which every update replaces. The first start there copies what an earlier one kept in
// usr/bin/user (not its cache); the old files stay.
std::filesystem::path PlayerUserDir() {
  const std::filesystem::path beside = bf1943::PlayerRoot() / "user";
#if defined(__linux__)
  if (const char* data = std::getenv("RECOMPLAUNCHER_DATA"); data && *data) {
    const std::filesystem::path dir = data;
    std::error_code ec;
    if (!std::filesystem::exists(dir / "bf1943-settings.txt", ec) && std::filesystem::is_directory(beside, ec)) {
      std::filesystem::create_directories(dir, ec);
      for (const auto& e : std::filesystem::directory_iterator(beside, ec)) {
        if (e.path().filename() == "cache") continue;
        std::error_code c;
        std::filesystem::copy(e.path(), dir / e.path().filename(),
                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_existing, c);
        if (c) REXLOG_WARN("BF setup: {} not copied to {}: {}", e.path().string(), dir.string(), c.message());
      }
      REXLOG_INFO("BF setup: the player's folder {} copied to RecompLauncher's {}", beside.string(), dir.string());
    }
    return dir;
  }
#endif
  return beside;
}
#endif

}  // namespace

Bf1943App::Bf1943App() : TitleApp("bf1943", kPPCImageConfig) {}

std::filesystem::path Bf1943App::CacheRoot() const {
  const std::filesystem::path cache = rex::cvar::GetFlagByName("cache_root");
  return cache.empty() ? UserRoot() / "cache" : cache;
}

std::filesystem::path Bf1943App::LogFile() const {
  const std::filesystem::path log = bf1943::SdkSetting("log.file_name");
  return log.is_absolute() ? log : UserRoot() / "bf1943.log";
}

// The game folder: title.game_root (v0.10's --game_data_root), else the folder chosen at setup (an unpacked game used
// where it is), else "game" beside the exe (the player's download: <root>\bf1943.exe, <root>\game), or beside its bin
// folder (the server package: <root>\bin\bf1943.exe, <root>\game). Where "game" holds another game (King Kong
// Recompiled's AppImage keeps its own there: both AppImages in Downloads), ours is "bf1943-game".
std::filesystem::path Bf1943App::GameDir() const {
  const std::filesystem::path given = bf1943::SdkSetting("title.game_root");
  if (!given.empty()) return given;
  const std::filesystem::path saved = bf1943::SavedGameDir(UserRoot());
  if (bf1943::install::Installed(saved) && !bf1943::install::OtherGame(saved)) return saved;
  const std::filesystem::path exe = bf1943::PlayerRoot();
  const std::filesystem::path root = exe.filename() == "bin" ? exe.parent_path() : exe;
  const std::filesystem::path game = bf1943::install::GameFolderIn(root);
  if (bf1943::install::OtherGame(root / "game"))
    REXLOG_INFO("BF setup: {} holds another game (title {:08X}), so ours is {}", (root / "game").string(),
                bf1943::install::XexTitleId(root / "game" / "default.xex"), game.string());
  return game;
}

// The paths, the log, the renderer and the SDK's settings the game sets, at the title layer: the config file, the
// environment and the command line still win.
rex::Result<void> Bf1943App::OnConfigure(rex::SettingsRegistry& settings) {
  const bool server = REXCVAR_GET(server);
  SetSdk(settings, "ui.title", "Battlefield 1943 Reborn");  // the game's name, not the SDK's "bf1943"
#if !defined(BF_DEV_TOOLS)
  // The player's data beside the exe in "user" (cache and log there too): the player's download starts bf1943.exe
  // with no options (TekRantGaming, 7 Nov 2026). The server package passes its own. Under RecompLauncher on Linux:
  // its data folder (PlayerUserDir).
  if (!server) {
    const std::filesystem::path user = PlayerUserDir();
    std::error_code ec;
    std::filesystem::create_directories(user, ec);
    SetSdk(settings, "app.user_root", user.string());
    SetSdk(settings, "app.cache_root", (user / "cache").string());
    SetSdk(settings, "app.log_root", user.string());
    SetSdk(settings, "log.file_name", (user / "bf1943.log").string());
    // The release always draws with NativeXRenderer.
    rex::cvar::SetFlagByName("native_x_renderer", "true");
    rex::cvar::SetFlagByName("bf_engine_d3d_path", "true");
    // A player's log names every frame over 100 ms and what it waited on (NativeXRenderer's hitch lines).
    if (std::atof(rex::cvar::GetFlagByName("native_x_renderer_hitch_ms").c_str()) <= 0.0)
      rex::cvar::SetFlagByName("native_x_renderer_hitch_ms", "100");
  }
#endif
  // (The work that reads the player's folder is in OnSettingsSettled: here the command line is not applied yet.)
  // Arcade games run as their trial unless the license says the game was bought. Players install from their own copy.
  SetSdk(settings, "kernel.license_mask", "1");
  // The SDK's own overlays stay shut (TekRantGaming, 10 Nov 2026): F3 its debug overlay, F4 its settings, the backtick
  // its console (both of those change any setting), and F7 its achievements (the F8 panel's compare key). Our own
  // achievement popup replaces the SDK's (achievement_toast.h). BF_DEV_SDK_BINDS=1 (tests): kept.
#if defined(BF_DEV_TOOLS)
  const bool keep_sdk_binds = std::getenv("BF_DEV_SDK_BINDS") != nullptr;
#else
  constexpr bool keep_sdk_binds = false;
#endif
  if (!keep_sdk_binds)
    for (const char* bind : {"ui.bind.debug", "ui.bind.settings", "ui.bind.console", "ui.bind.achievements"})
      SetSdk(settings, bind, "");
  SetSdk(settings, "ui.achievement_toast", "false");  // (our SDK copy's setting)
  // The game's address space stays mapped to the process's end (our SDK copy's runtime.keep_guest_memory_at_exit): our
  // host threads (the level of detail's, the bots', the scanners') read the game's memory until the process ends, as
  // under v0.10. Released under them at the quit, one read crashed it (the port's phase 8, 25 Nov 2026).
  SetSdk(settings, "runtime.keep_guest_memory_at_exit", "true");
  // The renderer (phase 5 of the port: NativeXRenderer as the driver module rexgpu-xenos-bfnative). A dedicated server
  // draws nothing: the null Xenos driver.
  if (server) {
    SetSdk(settings, "gpu.driver", "null");
    SetSdk(settings, "hid.mnk_mode", "false");  // a dedicated server has no player
    SetSdk(settings, "ui.driver", "none");      // and no window (GetRequirements): headless, also with a display
    // Its window is never shown (our SDK copy's ui.window_start), so nothing pops up over the player's screen.
    SetSdk(settings, "ui.window_start", "hidden");
    // It skips a guest call through a broken function pointer instead of stopping with every player in it (our SDK
    // copy's runtime.guest_invalid_call_skip; All Maps 3, 14 and 15 Nov 2026: four such stops, by null, freed and stack
    // addresses). Logged with the caller; the client keeps the SDK's stop.
    SetSdk(settings, "runtime.guest_invalid_call_skip", "true");
  } else {
    // (The developer build's choice from the player's settings: OnSettingsSettled.)
    SetSdk(settings, "gpu.driver", "bfnative");
#if !defined(_WIN32)
    SetSdk(settings, "hid.shared_pads", "true");  // on Linux every pad is the one player's (v0.10's SharedAssignment)
#endif
#if defined(BF_DEV_TOOLS)
    // BF_DEV_PAD: the scripted pad alone (dev_pad.cpp), so a real controller never mixes in.
    if (bf1943::DevPadOn()) SetSdk(settings, "hid.drivers", "devpad");
#endif
  }
  return {};
}

// The work that reads the player's folder, once the command line, the config file and the environment are applied (our
// SDK copy's hook; OnConfigure runs before them). In OnConfigure, UserRoot() was the SDK's default root in every run
// given --user_data_root (test runs, the server package): the player's settings were not applied at the start, nor
// their one-time updates (v0.4.8's defaults), nor the previous log kept (the port's phase 8, 25 Nov 2026).
rex::Result<void> Bf1943App::OnSettingsSettled(rex::SettingsRegistry& settings) {
  const bool server = REXCVAR_GET(server);
  // The last run's log kept as bf1943.previous.log before this one opens over it: a crash's report sends the log of the
  // run that crashed (report.cpp).
  if (!server) bf1943::report::KeepPreviousLog(LogFile());
  bf1943::ApplyStartupDisplaySettings(UserRoot());  // before the window is made
  bf1943::ApplyPlayerIdentity(UserRoot());          // the player's own name and XUID
#if defined(BF_DEV_TOOLS)
  // In the developer build a gpu.driver on the command line or in bf1943.toml still wins (vulkan: reference frames).
  if (!server) SetSdk(settings, "gpu.driver", bf1943::StartupGpuPlugin(UserRoot()) == "xenos" ? "vulkan" : "bfnative");
#else
  (void)settings;
#endif
  // Every graphics device on the high-performance GPU (King Kong's fix, gpu_preference.cpp), unless the player chose
  // one (gpu.vulkan.device, from their settings above). Hybrid laptops ran the game on the integrated GPU
  // (TekRantGaming, 11 Nov 2026).
  if (!server) bf1943::PreferHighPerformanceGpu(bf1943::GpuApi::kVulkan);
  return {};
}

// The program's own driver modules: BF_DEV_PAD's scripted pad (developer builds).
void Bf1943App::OnRegisterModules(std::vector<rex::IModule*>& modules) {
  TitleApp::OnRegisterModules(modules);
#if defined(BF_DEV_TOOLS)
  if (rex::IModule* pad = bf1943::DevPadModule()) modules.push_back(pad);
#endif
}

void Bf1943App::WindowEvents::OnUIEvent(const rex::ui::UIEvent& event) {
  switch (event.GetType()) {
    case rex::ui::UIEvent::Type::kMinimized:
      bf1943::NoteWindowMinimized(true);
      break;
    case rex::ui::UIEvent::Type::kRestored:
    case rex::ui::UIEvent::Type::kFocus:
      bf1943::NoteWindowMinimized(false);
      break;
    default:
      break;
  }
}

// The window and the UI system run: our menus and overlays, the crash reports, the renderer.
rex::Result<void> Bf1943App::OnInit() {
  const bool server = REXCVAR_GET(server);
  bf1943::InstallCrashReports(UserRoot());  // first: a crash from here on leaves a report
  bf1943::RaiseTimerResolution();           // 1 ms sleeps for the game's waits and our frame limit (frame_rate.cpp)
  // --install_game (the server package's setup) or --install_progress (RecompLauncher's, which leaves the package out
  // once the game is installed; pull request 70): the game unpacked into its folder, then the process ends.
  if (!REXCVAR_GET(install_game).empty() || REXCVAR_GET(install_progress))
    std::exit(bf1943::install::InstallFromCommandLine(GameDir()));
  rex::ui::IUISystem& ui = GetUI();
  rex::ui::compat::Attach(ui);
  rex::ui::Window* window = rex::ui::compat::GetWindow();
  rex::ui::ImGuiDrawer* drawer = rex::ui::compat::GetDrawer();
  window_listener_ = ui.AddEventListener(window_events_);
  if (window) bf1943::SetPacingWindow(window->GetNativeWindowHandle());
  bf1943::InstallFpeGuard();    // after the runtime's own signal handlers
  bf1943::HideServerWindows();  // a dedicated server shows no window (server_mode.cpp)
  // A headless UI (a dedicated server, ui.driver = none: GetRequirements) has no Dear ImGui and no fonts.
  if (!ui.IsHeadless()) bf1943::LoadMenuFonts(&ui.GetFonts());
  // The achievement popup, made with the menus: the Achievements page's Test the popup works on Home before the game
  // runs. The title's events reach it once the game is loaded (OnTitleLaunch).
  if (!server && drawer) {
    bf1943::SetAchievementUserDir(UserRoot());
    toast_ = std::make_unique<bf1943::AchievementToast>(drawer);
  }
  // Our main menu inside the game (game_menu.cpp), drawn over BF's own menu scene.
  bf1943::CreateGameMenu(
      drawer, UserRoot(),
      [this](bool on) {
        GetUI().PostTask([on] {
          if (auto* w = rex::ui::compat::GetWindow()) w->SetFullscreen(on);
        });
      },
      // Quit as the window's close button does.
      [] { rex::app::RequestShutdown(0); }, [] { return rex::ui::compat::GetWindow(); },
      [this] { return game_dir_; });
  // A graphics driver reset (gpu_lost.cpp): the player told, and the game closed, instead of a frozen picture.
  if (!server)
    bf1943::WatchGpuDeviceLoss([this](std::function<void()> job) { GetUI().PostTask(std::move(job)); },
                               [] { rex::app::RequestShutdown(0); });
  if (!server && !AbOff("overlay")) bf1943::CreatePerfOverlay(drawer);  // Settings > Gameplay > Performance metrics, F2
  if (!server) bf1943::CreateCommsOverlay(drawer);            // text chat top left, voice chat (comms_ui.cpp)
  if (!server) bf1943::CreateWorldAdminPanel(drawer);         // F9, a match's admin (world_sync.cpp)
  if (!server) bf1943::CreateSpawnMouseOverlay(drawer);       // the deploy screen's pointer
  if (!server) bf1943::CreateGraphicsPanel(drawer, window);   // F8 (f8_panel.cpp)
  if (!server) bf1943::CreateUiHider(drawer);                 // last of our overlays: it hides them (free_cam.cpp)
  if (!server) bf1943::InstallHudGlyphs();                    // the game's prompts in the chosen style
  if (!server) bf1943::InstallVehicleBinds();                 // each vehicle kind's keys (vehicle_binds.cpp)
  bf1943::InstallLook(window);           // the camera and look system (look.cpp)
  bf1943::InstallPlaytestMark(window);   // R marks a moment in a watched play test (playtest_mark.cpp)
#if defined(BF_DEV_TOOLS)
  bf1943::StartStackSampler();  // BF_DEV_STACKS
#endif
  return {};
}

void Bf1943App::ExtractArt(const std::filesystem::path& game) const {
  if (REXCVAR_GET(server)) return;
  const auto cache = CacheRoot();
  // Pictures and the menu music for our menus from the player's game files, once (shader_prep.cpp, game_music.cpp).
  bf1943::ExtractGameArt(game, cache / "extracted");
  bf1943::SetLevelGameRoot(game);  // the level's water height for the grass
  bf1943::HdExport(game);          // developer: BF_DEV_HD_EXPORT, the HD pack's source pictures
  bf1943::HdExportUi(game);        // BF_HD_EXPORT_UI: the menu and HUD pictures for the pack
  // NXR_SHADER_TEST=1 (a developer's check): the renderer's CMAA shaders compiled, the failures the exit code, no game.
  if (const char* v = std::getenv("NXR_SHADER_TEST"); v && *v == '1') std::_Exit(native_x_renderer::TestUtilityShaders());
  // The grass's plants from the game's own textures (made there, NativeXRenderer grass.cpp loads them).
  rex::cvar::SetFlagByName("native_grass_textures", (cache / "extracted" / "vegetation-2.bin").string());
}

// The first start's setup (no game installed), the player's name and the launcher, before the game: true when one of
// them shows (GameMain runs frames until it hands the start over). A dedicated server goes straight on.
bool Bf1943App::BeginPreGame() {
  const bool server = REXCVAR_GET(server);
  game_dir_ = GameDir();
  bf1943::NotePrivateMatchPaths(game_dir_, UserRoot());  // our private match's server
  const bool installed = bf1943::install::Installed(game_dir_) || server;
  const bool name = !server && bf1943::NameNeeded(UserRoot());
  // A player's start: the launcher before the game (TekRantGaming, 8 Nov evening), which starts it at Play.
  const bool launcher = !server && bf1943::LauncherFirst();
  if (installed) ExtractArt(game_dir_);
  auto start = [this] {
    pre_game_done_ = true;
  };
  auto then_name = [this, start, name, launcher, installed](std::filesystem::path game) {
    game_dir_ = std::move(game);
    if (!installed) ExtractArt(game_dir_);  // just installed by the first start's setup
    if (name && bf1943::NameNeeded(UserRoot())) bf1943::BeginNameSetup(start);
    else if (launcher) bf1943::BeginLauncher(start);
    else start();
  };
  if (installed && !name && !launcher) {
    pre_game_done_ = true;
    return false;
  }
  if (installed) {
    then_name(game_dir_);
  } else {
    REXLOG_INFO("BF setup: no game in {}: the first start's setup", game_dir_.string());
    bf1943::BeginGameSetup(game_dir_, then_name);
  }
  return true;
}

// The game is loaded and not started yet: the player's name for the game, the shaders from the player's game files into
// the storage before the renderer opens it, and the title's achievement events to our popup.
rex::Result<void> Bf1943App::OnTitleLaunch(rex::kernel::TitleRuntime& title) {
  rex::cvar::bridge::SetTitleLaunched();  // the game's SDK settings at the runtime layer from now on (cvar_bridge.h)
  // NativeXRenderer (native_x_renderer_bf1943.cpp), once the title is attached and before the guest starts (our SDK
  // copy's TitleRuntime::OnTitleAttached): Launch attaches the bfnative driver, whose command processor then gives it
  // the guest's memory and hooks (rex/system/interfaces/graphics.h). Here, before Launch, there is no processor yet
  // (the port's first smoke run, 25 Nov 2026: nothing drew). A dedicated server draws nothing at all.
  if (!REXCVAR_GET(server)) {
    // The game's text sharper (sharp_text.cpp: HUD scale High-Res), as in 0.4.8: here, not in OnInit, because the game
    // folder is known only after the first start's setup (BeginPreGame); before the renderer starts, as before.
    bf1943::InstallSharpText(game_dir_);
    title_attached_ = title.OnTitleAttached([this] {
      native_x_renderer::StopPreGamePresenter();  // its device off the window first
      native_x_renderer::Initialize(bf1943::NativeXRendererProfile(), rex::system::bfnative_graphics(),
                                    rex::ui::compat::GetWindow(), CacheRoot());
    });
  }
  bf1943::FinishNameCheck();  // the backend's answer to the player's name, so the game only ever sees one name
  const auto cache = CacheRoot();
  // First, from the marker and the storage as they were: after an update the old shader cache is deleted and the
  // shader page builds a fresh set (game_menu.cpp, update.cpp).
  if (!REXCVAR_GET(server)) bf1943::CheckShadersAfterUpdate(UserRoot(), cache);
  bf1943::PrepareShaders(bf1943::ExecutableDir() / "data" / "shaders", game_dir_, cache);
  // The pipelines this game builds, for the next update's shader list (Settings > Gameplay > Share graphics data).
  bf1943::StartPipelineUploads(UserRoot(), cache);
  if (toast_) {
    auto* toast = toast_.get();
    toast_events_ = title.GetAchievementManager().RegisterNotificationCallback(
        [toast](const rex::xam::AchievementEvent& event) { toast->Push(event); });
  }
  return {};
}

void Bf1943App::ApplyGameRoot() {
  if (auto r = REX_SETTINGS().SetText("title.game_root", game_dir_.string()); !r.has_value())
    REXLOG_ERROR("BF setup: the game folder {} not set: {}", game_dir_.string(), r.error().GetDescription());
}

void Bf1943App::TellFailure(rex::SelfQuitCause cause, const char* reason) {
  if (REXCVAR_GET(server)) return;  // a dedicated server has nobody to tell
  rex::ui::Window* window = rex::ui::compat::GetWindow();
  bf1943::TellStartFailure(cause, reason, SecondsSince(launched_at_), LogFile(), 1,
                           window ? window->GetNativeWindowHandle() : nullptr);
}

// The game ended by itself. One that ended while it was starting is told to the player (start_failure.cpp), then the
// program ends, as under v0.10 (TitleApp's own would offer a restart).
void Bf1943App::OnTitleExit(rex::kernel::TitleExitReason reason) {
  if (reason == rex::kernel::TitleExitReason::kNormalExit)
    TellFailure(rex::SelfQuitCause::kGuestExited, "the game's main thread ended");
  rex::app::RequestShutdown(reason == rex::kernel::TitleExitReason::kNormalExit ? 0 : 1);
}

// A start that failed (App::Init or LaunchTitle): the player is told with where the log is (start_failure.cpp;
// TekRantGaming, 13 Nov 2026: a player's game closed 2 s after its launcher came up and said nothing), in place of
// TitleApp's own message box.
void Bf1943App::ReportError(std::string_view message) {
  REXLOG_ERROR("BF start: {}", message);
  const std::string reason(message);
  const auto phase = GetPhase();
  TellFailure(phase == rex::app::AppPhase::kTitleLaunch ? rex::SelfQuitCause::kModuleLaunchFailed
                                                        : rex::SelfQuitCause::kInitFailed,
              reason.c_str());
}

void Bf1943App::OnShutdown() {
  toast_events_.Reset();
  window_listener_.Reset();
  rex::ui::compat::Detach();
}

// The program's start: the game's own options out of the command line (its cvars, compat/cvar.cpp) and the rest to the
// SDK; then the pages before the game, the game, and the end.
int Bf1943Main(Bf1943App& app, const rex::CLIArgList& args) {
  std::vector<std::string> tokens{"bf1943"};
  for (const std::string& a : args.GetAll()) tokens.push_back(a);
  std::vector<char*> argv;
  for (std::string& t : tokens) argv.push_back(t.data());
  rex::cvar::Init(static_cast<int>(argv.size()), argv.data());
  const std::vector<std::string> rest = rex::cvar::UnparsedArgs();  // the SDK's options and the positionals
  rex::cvar::ApplyEnvironment();
  const std::filesystem::path config = bf1943::ExecutableDir() / "bf1943.toml";
  std::error_code ec;
  if (std::filesystem::exists(config, ec)) rex::cvar::LoadConfig(config);
  rex::cvar::FinalizeInit();
  if (app.Init(rex::CLIArgList::Parse(rest))) {
    // The pages' frames on the window (NativeXRenderer's pre-game presenter: under v0.11 no presenter draws them
    // before the renderer starts with the title). A dedicated server shows none.
    const bool present = !REXCVAR_GET(server) &&
                         native_x_renderer::StartPreGamePresenter(rex::ui::compat::GetWindow());
    if (app.BeginPreGame()) {
      while (!app.PreGameDone() && app.RunFrame()) {
        if (present) native_x_renderer::PresentPreGameFrame();
      }
    }
    if (!app.PreGameDone()) native_x_renderer::StopPreGamePresenter();  // no game: the program ends
    if (app.PreGameDone()) app.ApplyGameRoot();
    if (app.PreGameDone() && app.LaunchTitle()) {
      app.NoteLaunched();
      app.Run();
    }
  }
  native_x_renderer::StopPreGamePresenter();  // a launch that failed before the renderer started (else nothing to do)
  app.ReleaseTitleHooks();
  return app.Shutdown();
}
