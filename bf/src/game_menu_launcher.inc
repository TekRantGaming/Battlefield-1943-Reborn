// The launcher's pages in Version 1 "Field Report" (TekRantGaming's pick, 8 Nov 2026; launcher_kit.h), included inside
// GameMenu (game_menu.cpp). The first time setup (docs/LAUNCHER-PLAN.md, section 2.1): 1 game files, 2 shaders,
// 3 account, 4 to 6 Display, Graphics and Controls, 7 button prompts, 8 Modern and Classic, 9 all set; then Home on
// every start. Every page works with a controller, the keyboard and the mouse; its prompts are clickable and show the
// device used last. Sizes are the mockups' pixels on a 1280 x 720 stage, centred on the screen.

  struct Stage {
    float u = 1, ox = 0, oy = 0;
    ImVec2 P(float x, float y) const { return ImVec2(ox + x * u, oy + y * u); }
    float operator()(float v) const { return v * u; }
  };
  static Stage StageOf(const ImGuiIO& io) {
    Stage st;
    st.u = std::max(0.1f, std::min(io.DisplaySize.y / 720.0f, io.DisplaySize.x / 1280.0f));
    st.ox = (io.DisplaySize.x - 1280 * st.u) * 0.5f;
    st.oy = (io.DisplaySize.y - 720 * st.u) * 0.5f;
    return st;
  }

  // The keyboard's key for each button on our pages (shown in the prompts with the keyboard's style).
  static const char* KeyFor(kit::Button b) {
    switch (b) {
      case kit::Button::kA: return "Enter";
      case kit::Button::kB: return "Esc";
      case kit::Button::kX: return "F";
      case kit::Button::kY: return "R";
      case kit::Button::kStart: return "P";
      case kit::Button::kBack: return "Tab";
      case kit::Button::kLB: return "Q";
      case kit::Button::kRB: return "E";
      default: return "";
    }
  }
  static void PressInto(Input& in, kit::Button b) {
    switch (b) {
      case kit::Button::kA: in.accept = true; break;
      case kit::Button::kB: in.back = true; break;
      case kit::Button::kX: in.x = true; break;
      case kit::Button::kY: in.y = true; break;
      case kit::Button::kStart: in.start = true; break;
      case kit::Button::kBack: in.view = true; break;
      case kit::Button::kLB: in.prev_tab = true; break;
      case kit::Button::kRB: in.next_tab = true; break;
      default: break;
    }
  }
  kit::Style PromptStyle() { return kit::StyleFor(Cvar("bf_button_prompts"), device_ == Device::kKeyboard); }

  struct Cue {
    kit::Button b;
    std::string label;
    std::string key;  // the keyboard's key, when not KeyFor(b)
    bool green = false;
  };
  // The prompts on their tape strips from `at` to the right; a click is that button's press.
  float Prompts(ImDrawList* d, const Stage& st, ImVec2 at, const std::vector<Cue>& ps, Input& in, float size = 18) {
    float x = at.x;
    const kit::Style style = PromptStyle();
    for (const Cue& p : ps) {
      const kit::Strip s = kit::PromptStrip(d, ImVec2(x, at.y), st.u, style, p.b, p.key.empty() ? KeyFor(p.b) : p.key,
                                            p.label, size, p.green);
      if (s.clicked) PressInto(in, p.b);
      x = s.b.x + st(10);
    }
    return x - at.x;
  }
  float PromptsWidth(const Stage& st, const std::vector<Cue>& ps, float size = 18) {
    float w = 0;
    const kit::Style style = PromptStyle();
    for (const Cue& p : ps) {
      const kit::Strip s = kit::PromptStrip(nullptr, ImVec2(0, 0), st.u, style, p.b, p.key.empty() ? KeyFor(p.b) : p.key,
                                            p.label, size, p.green, false);
      w += s.b.x - s.a.x + st(10);
    }
    return w;
  }

  void NotePage(const char* name) {
    if (logged_page_ == name) return;
    logged_page_ = name;
    REXLOG_INFO("BF launcher: page {}", name);
  }

  // The backdrop (the game's own picture shows through once BF's menu scene is up: the mockups' "art from your game
  // files"), the name at the top left, the step and the stamp.
  void Chrome(ImDrawList* d, ImGuiIO& io, const Stage& st, int step, float over_game = 0) {
    // The new look (Version A1, game_menu_home.inc): the game's own menu picture with the olive wash (the maps from
    // the air until the art is there), its logo with REBORN beside it (our brand until the game files are in: the
    // first setup's pages).
    (void)over_game;
    MenuBackdrop(d, io.DisplaySize, st);
    kit::Brand(d, st.P(40, 28), st.u);  // the logo once the art is there
    if (step > 0) kit::StepBar(d, st.P(1240, 34), st.u, step, kSetupSteps);
  }
  void StampCorner(ImDrawList* d, const Stage& st) {
    if (kit::WatermarkOn()) kit::Stamp(d, st.P(1240 - 128, 720 - 22 - 33), st.u, -6);
  }
  // Whether the game's own menu scene is behind us (its picture, not its films).
  bool SceneBehind() const { return MenuDriverStage() >= 2 && MenuDriverStage() != 5; }

  static void OpenUrl(const std::string& url) {
    REXLOG_INFO("BF launcher: opening {}", url);
    if (const char* v = std::getenv("BF_DEV_NO_OPEN"); v && *v == '1') return;  // tests: logged only
#if defined(_WIN32)
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    // The desktop's own opener (in a Flatpak it asks the portal). Detached: a browser outlives the game.
    if (fork() == 0) {
      setsid();
      execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
#endif
  }
  static constexpr const char* kDiscordUrl = "https://discord.gg/8eakYSvBZS";  // TRG Studios' invite (King Kong's)

  // ---- the setup's order

  // After the shaders: the setup from its account page (until it was finished once), else Home. A setup closed half
  // way counts as done at the next start once a name was chosen (TekRantGaming's rule for King Kong, 26 Oct 2026).
  // The first start's shaders come right after the game files, before the onboarding, and an update's before the main
  // menu (AfterIntro; TekRantGaming, 16 Nov 2026).
  void AfterShaders() {
    just_installed_ = false;
    // Play started the game from the launcher: our Getting ready page stays while the game boots and signs in behind
    // it (its films, title and menus never show, TekRantGaming's 10 Nov brief); then our main menu (StartingPage).
    if (played_) {
      played_ = false;
      if (join_after_shaders_) {  // a server picked on Home (the shader page came first): its Joining page now
        join_after_shaders_ = false;
        page_ = Page::kJoining;
        return;
      }
      page_ = Page::kStarting, starting_at_ = ImGui::GetTime();
      REXLOG_INFO("BF launcher: the game's shaders are ready; it signs in behind our page");
      return;
    }
    if (SkipLauncher()) {
      page_ = Page::kMain, row_ = 0;
      return;
    }
    const bool named = settings_.Get("player_name_chosen", "") == "1";
    if (settings_.Get("launcher_setup", "") == "done" && named) {
      page_ = Page::kHome, home_row_ = 0;
      return;
    }
    if (RenameRequired()) return;
    setup_active_ = true;
    settings_.values["launcher_setup"] = "started";
    settings_.Save();
    BeginAccount(true);
  }
  // Test runs that join by themselves (BF_AUTO_JOIN=1) go straight to BF's menu with our list, unless they test the
  // launcher (BF_LAUNCHER_TEST=1).
  static bool SkipLauncher() {
    const char* a = std::getenv("BF_AUTO_JOIN");
    const char* t = std::getenv("BF_LAUNCHER_TEST");
    return a && *a == '1' && !(t && *t == '1');
  }
  void SetupNext() {
    switch (page_) {
      case Page::kName: page_ = Page::kSetupSettings, setup_tab_ = 0; break;
      case Page::kSetupSettings:
        if (setup_tab_ < 2) ++setup_tab_;
        else page_ = Page::kPrompts, prompts_row_ = 0;
        break;
      case Page::kPrompts: page_ = Page::kVoice; break;
      case Page::kVoice:
        // Ready. The shaders were prepared before the onboarding, right after the game files (AfterIntro): no shader
        // step here since v0.4.7.
        page_ = Page::kAllSet, allset_row_ = 0;
        break;
      case Page::kAllSet:
        settings_.values["launcher_setup"] = "done";
        settings_.values["last_version"] = BfVersion();  // a first install has nothing new to be told (What's new)
        settings_.Save();
        setup_active_ = false;
        whats_new_pending_ = false;
        REXLOG_INFO("BF launcher: setup finished");
        page_ = Page::kHome, home_row_ = 0;
        // Settings the game takes only at the start were chosen: it starts again now, so the first match already
        // runs with them (and its frame rate is the one the player will keep). Before the game runs (the launcher
        // first) they apply at Play: only the window's own (monitor, size, VSync) would need it.
        if (RestartPending()) RestartForSettings("the setup's end");
        break;
      default: break;
    }
    row_ = 0, scroll_ = 0, sub_ = kSubNone;
  }
  void SetupBack() {
    switch (page_) {
      case Page::kSetupSettings:
        if (setup_tab_ > 0) --setup_tab_;
        else BeginAccount(true);
        break;
      case Page::kPrompts: page_ = Page::kSetupSettings, setup_tab_ = 2; break;
      case Page::kVoice: page_ = Page::kPrompts; break;
      case Page::kAllSet: page_ = Page::kVoice; break;
      default: break;
    }
    row_ = 0, scroll_ = 0, sub_ = kSubNone;
  }
  int SetupStep() const {
    switch (page_) {
      case Page::kSetup: return 1;
      case Page::kName: return 2;
      case Page::kSetupSettings: return 3 + setup_tab_;
      case Page::kPrompts: return 6;
      case Page::kVoice: return 7;
      case Page::kAllSet: return 8;
      default: return 0;
    }
  }
  bool Settled() const { return ImGui::GetTime() >= settle_until_; }

  // The recommended settings for this PC (Start on the settings pages): the frame rate at the screen's refresh rate,
  // FXAA, the render resolution from the screen, shadows High, light sharpening, and ambient occlusion and global
  // illumination on at Medium (TekRantGaming, 6 Nov: effects on at Medium). The window mode is left as it is.
  // The GPU's time for the renderer's fixed workload against an RTX 4070 Ti's (D150: timed once at the start): 1 there,
  // about 5 to 6 for a GTX 1060 or an RX 580; 0 while not known.
  static constexpr double kGpuReferenceMs = 5.58;  // an RTX 4070 Ti at its full clock (runs ad5, ad6: 5.576, 5.575 ms)
  double GpuSlowness() {
    double ms = 0;
    const bool timed = kGpuReferenceMs > 0 && native_x_renderer::GpuBenchmarkResult(&ms) && ms > 0;
#if defined(BF_DEV_TOOLS)
    // BF_DEV_GPU_SLOWNESS=<times> (tests): as if the GPU took that many times the reference's time, once it is timed
    // (a test's auto detect waits for the time as a player's does).
    if (const char* v = std::getenv("BF_DEV_GPU_SLOWNESS"); v && *v) return timed ? std::atof(v) : 0.0;
#endif
    return timed ? ms / kGpuReferenceMs : 0.0;
  }
  // The GPU's tier (TekRantGaming, 9 Nov 2026: the game should run on a GTX 1060 or an RX 580 at 1080p, 60 FPS, with the
  // defaults): fast (2) up to 4 times an RTX 4070 Ti's time (an RTX 2060, 2070 or 3060 and up), middle (1) up to 10 (a
  // GTX 1060, 1660 or an RX 580: about 5 to 6), slow (0) past it (integrated graphics). Not known: fast.
  static int TierOf(double slowness) { return slowness <= 4.0 ? 2 : slowness <= 10.0 ? 1 : 0; }
  // A tier's runtime settings. The middle tier also draws the grass to 180 m (the whole map's every tuft, D152, is
  // the fast tier's default).
  // The upscaler's mode as King Kong's hardware tiers have it (hardware_recommendation.h: low Performance, mid and high
  // Quality; TekRantGaming, 11 Nov 2026, v0.2.1), with the upscaler at its default, AMD FSR 1 (King Kong's presets put it back to
  // its default). No render resolution any more (TekRantGaming, 11 Nov 2026): the game draws the display's, or with the
  // upscaler the mode's share of it, so a slow GPU's lighter picture is FSR 1 at Performance.
  // Since 12 Nov 2026 the tiers are the Graphics presets (GraphicsPresets): slow Low, middle Medium, fast High, so the
  // Graphics preset row names what the auto detect chose. Their texture filtering (2x, 8x, 16x) and the fast tier's
  // grass to the whole map (its default before) are the presets'; the sharpness and AO strength as before.
  static constexpr const char* kTierPresets[] = {"Low", "Medium", "High"};
  static std::vector<std::pair<const char*, const char*>> TierSettings(int tier) {
    std::vector<std::pair<const char*, const char*>> s = {{"native_x_renderer_sharpness", "0.250000"}};
    if (tier > 0) s.push_back({"native_ao_strength", "2"});
    for (const auto& p : GraphicsPresets())
      if (std::string(p.name) == kTierPresets[std::clamp(tier, 0, 2)]) s.insert(s.end(), p.settings.begin(), p.settings.end());
    return s;
  }
  // A tier's settings applied, its frame rate too: the highest that divides into 30 up to the screen's refresh rate
  // (a starting choice, not a limit: every rate and Unlimited stay offered), 60 at most below the fast tier (a rate
  // it cannot keep comes unevenly).
  void ApplyTier(int tier, double slowness, const char* when) {
    rex::ui::Window* window = window_ ? window_() : nullptr;
    const int monitor = std::atoi(settings_.Get("monitor", "0").c_str());
    const DisplayMode desktop = DesktopDisplayMode(monitor, window);
    const int refresh = int(std::lround(desktop.refresh));
    std::string fps = "60";
    const int most = refresh > 0 ? refresh + 1 : 61;  // 59.94 Hz gives 60
    for (const std::string& r : OfferedRates())
      if (r != "unlimited" && std::atoi(r.c_str()) > std::atoi(fps.c_str()) && std::atoi(r.c_str()) <= most &&
          (tier == 2 || std::atoi(r.c_str()) <= 60))
        fps = r;
    settings_.values["frame_rate"] = fps;
    settings_.values["vsync"] = "off";
    settings_.Save();
    ApplyFrameRate();
    for (const auto& [name, value] : TierSettings(tier)) SetCvar(name, value);
    static const char* kTiers[] = {"slow: the Low preset (FSR 1 Quality, AO and GI off, grass to 45 m)",
                                   "middle: the Medium preset (FSR 1 Quality, shadows Original, AO and GI Low, grass to 180 m)",
                                   "fast: the High preset (FSR 1 Quality, shadows High, soft shadows and SSR, AO and GI at "
                                   "Medium, grass to the whole map)"};
    REXLOG_INFO("BF launcher: auto detect{}: {} FPS for a {} Hz screen, FXAA; the GPU {} ({:.1f} times an RTX 4070 Ti's "
                "time), so {}",
                when, fps, refresh, slowness > 0 ? "timed" : "not timed yet", slowness, kTiers[tier]);
  }

  // The recommended settings for this PC (Start on the settings pages). The launcher shows before the game, and the
  // GPU is only timed in the game (its renderer starts at Play), so the first start's auto detect is always made
  // before its time is known: the fast tier then, and the GPU's own once it is timed (AutoDetectWhenTimed), if the
  // settings it made are still as it left them.
  void AutoDetect() {
    const double slowness = GpuSlowness();
    ApplyTier(TierOf(slowness), slowness, "");
    settings_.values["auto_detect_gpu"] = slowness > 0 ? "done" : "pending";
    settings_.values["auto_detect_fps"] = settings_.Get("frame_rate", "");
    settings_.Save();
  }
  // Each frame: an auto detect made before the GPU's time was known, finished once it is (the GPU is timed only while
  // one waits: OnDraw starts it). Not a slower tier when the player changed any of the settings it made since.
  void AutoDetectWhenTimed() {
    if (settings_.Get("auto_detect_gpu", "") != "pending") return;
    double ms = 0;
    if (!native_x_renderer::GpuBenchmarkResult(&ms)) return;
    settings_.values["auto_detect_gpu"] = "done";
    settings_.Save();
    const double slowness = GpuSlowness();
    const int tier = TierOf(slowness);
    if (slowness <= 0 || tier == 2) {
      REXLOG_INFO("BF launcher: auto detect: the GPU timed at {:.1f} times an RTX 4070 Ti's time: the fast tier stays",
                  slowness);
      return;
    }
    bool untouched = settings_.Get("frame_rate", "") == settings_.Get("auto_detect_fps", "");
    for (const auto& [name, value] : TierSettings(2)) untouched = untouched && Cvar(name) == value;
    if (!untouched) {
      REXLOG_INFO("BF launcher: auto detect: the GPU timed at {:.1f} times an RTX 4070 Ti's time, but the settings were "
                  "changed since: kept",
                  slowness);
      return;
    }
    ApplyTier(tier, slowness, " (the GPU timed in the game)");
  }

  // Each frame until known: the renderer's answer at this start to whether NVIDIA NIS runs on this GPU (D158), saved
  // with the GPU's name for the Upscaler row (TekRantGaming, 11 Nov 2026: NIS not shown to devices it does not run on).
  void NoteNisSupport() {
    if (nis_noted_) return;
    bool runs = false;
    std::string gpu;
    if (!native_x_renderer::NisSupport(&runs, &gpu)) return;
    nis_noted_ = true;
    const std::string value = runs ? "1" : "0";
    // The default spatial upscaler (TekRantGaming, 20 Nov 2026: FSR 1 by default, "if we detect that they have a
    // compatible GPU, we then make it NVIDIA NIS"): while the player has not picked one (upscaler_auto) and the
    // upscaler is still the default AMD FSR 1, an NVIDIA card that runs NIS (this start's check, D158: its shaders and
    // pipelines made on this GPU) gets NVIDIA NIS, from the next start (the renderer reads it when it starts). Any
    // other GPU keeps AMD FSR 1, which runs everywhere.
    if (settings_.Get("upscaler_auto", "") == "1") {
      std::string lower = gpu;
      for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
      const bool nvidia = lower.find("nvidia") != std::string::npos;
      settings_.values.erase("upscaler_auto");
      if (runs && nvidia && settings_.Get("cvar.native_x_renderer_scaling", "") == "fsr") {
        settings_.values["nis_support"] = value, settings_.values["nis_support_gpu"] = gpu;
        SetCvar("native_x_renderer_scaling", "nis");  // saves
        REXLOG_INFO("BF upscaler: {} is an NVIDIA card that runs NVIDIA NIS: the default upscaler is NVIDIA NIS from the "
                    "next start", gpu);
      } else {
        settings_.Save();
        REXLOG_INFO("BF upscaler: {}: the default upscaler stays AMD FSR 1 ({})", gpu,
                    !nvidia ? "not an NVIDIA card" : !runs ? "NVIDIA NIS does not run on it" : "another upscaler is chosen");
      }
    }
    if (settings_.Get("nis_support", "") == value && settings_.Get("nis_support_gpu", "") == gpu) return;
    settings_.values["nis_support"] = value;
    settings_.values["nis_support_gpu"] = gpu;
    settings_.Save();
    REXLOG_INFO("BF upscaler: NVIDIA NIS {} on {}: {} in the Upscaler row from now on", runs ? "runs" : "does not run",
                gpu, runs ? "offered" : "not offered");
  }
  bool nis_noted_ = false;

  // Each frame until known: which temporal upscalers run on this GPU (the renderer's answer at this start), saved for
  // the Upscaler row; a choice that does not run here becomes one that does (TemporalFallback).
  void NoteTemporalSupport() {
    if (temporal_noted_) return;
    bool fsr4 = false, fsr3 = false, xess = false, dlss = false;
    if (!native_x_renderer::TemporalSupport(&fsr4, &fsr3, &xess, &dlss)) return;
    temporal_noted_ = true;
    (void)fsr4;  // FSR 4 is no longer offered (0.4.3)
    const std::string value =
        std::string(",") + (fsr3 ? "fsr," : "") + (xess ? "xess," : "") + (dlss ? "dlss," : "") + "taa,";
    if (settings_.Get("temporal_support", "") != value) {
      settings_.values["temporal_support"] = value;
      settings_.Save();
      REXLOG_INFO("BF upscaler: the Upscaler row offers {}", value);
    }
    const std::string t = Cvar("bf_temporal");
    if (const std::string to = TemporalFallback(t, value); to != t) {
      REXLOG_INFO("BF upscaler: {} does not run on this GPU: {} from the next start", t, to);
      settings_.values["cvar.bf_temporal"] = to;
      settings_.Save();
    }
  }
  bool temporal_noted_ = false;

  // ---- a small question over the page: Cancel focused (King Kong's Skip all). True once answered; `yes` the second.
  bool Question(ImDrawList* d, ImGuiIO& io, const Stage& st, const std::string& title, const std::string& text,
                const std::string& no, const std::string& yes, Input& in, bool& answered_yes) {
    d->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(10, 12, 8, 170));
    const kit::Fonts& f = kit::GetFonts();
    const ImVec2 a = st.P(390, 230), b = st.P(890, 470);
    {
      kit::Turn turn(d, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), -0.4f);
      kit::Paper(d, a, b, st.u);
      kit::Tape(d, ImVec2((a.x + b.x) * 0.5f, a.y), st(110), st(26), -3);
      kit::Heading(d, st(40), ImVec2(a.x + st(30), a.y + st(28)), kit::kInk, title, st(1.5f), st(440));
      kit::Wrapped(d, f.ui, st(18), ImVec2(a.x + st(30), a.y + st(84)), kit::kInkSoft, text, st(440));
    }
    const int buttons = yes.empty() ? 1 : 2;  // one: an OK
    if (buttons == 1) popup_row_ = 0;
    else if (in.left || in.right || in.up || in.down) popup_row_ = 1 - popup_row_;
    int clicked = -1;
    float x = a.x + st(30);
    for (int i = 0; i < buttons; ++i) {
      const std::string& label = i == 0 ? no : yes;
      const ImVec2 c0(x, b.y - st(70)), c1(x + st(200), b.y - st(26));
      const bool hover = ImGui::IsMouseHoveringRect(c0, c1);
      if (hover && io.MouseDelta.x + io.MouseDelta.y != 0) popup_row_ = i;
      const bool sel = popup_row_ == i;
      if (sel) d->AddRectFilled(ImVec2(c0.x, c0.y + st(4)), ImVec2(c1.x, c1.y + st(4)), kit::kGreenShadow);
      d->AddRectFilled(c0, c1, sel ? kit::kGreen : kit::kPaperCard);
      if (!sel) d->AddRect(c0, c1, kit::kBorder, 0, 0, st(2));
      const float tw = kit::TextWidth(f.ui_bold, st(20), label);
      d->AddText(f.ui_bold, st(20), ImVec2((c0.x + c1.x - tw) * 0.5f, c0.y + st(8)), sel ? IM_COL32(255, 255, 255, 255) : kit::kInk,
                 label.c_str());
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = i;
      x += st(220);
    }
    if (in.accept) clicked = popup_row_;
    if (in.back) clicked = 0;
    in = {};
    if (clicked < 0) return false;
    answered_yes = clicked == 1;
    return true;
  }

  // ---- 1: the game files (King Kong's stage 0 made for this game; no game art yet)

  // Linux: our own browser for the game files. Windows asks its own pickers (BrowseForPackage, BrowseForFolder);
  // Linux has none, so the two cards did nothing there (TekRantGaming, 11 Nov 2026: a player "can only highlight options for
  // game data. It won't actually move to the picker"). Ours works with the mouse, the keyboard and a controller, also in
  // Steam's Game Mode, where another program's dialog would not show. A opens a folder or picks a file, X takes the
  // folder shown (an unpacked game, or the folder holding the package), Left goes up, LB and RB the places, B back.
  struct BrowseEntry {
    std::string name;
    bool dir = false;
    uint64_t size = 0;
  };
  bool browsing_ = false;
  fs::path browse_dir_;
  std::vector<BrowseEntry> browse_entries_;  // ".." first (but at the top of the disk), then folders, then files
  int browse_row_ = 0, browse_top_ = 0, browse_place_ = 0;
  std::string browse_note_;

  static fs::path BrowseHome() {
    const char* h = std::getenv("HOME");
    return h && *h ? fs::path(h) : fs::path("/");
  }
  // Home, Downloads, Desktop, the removable drives (USB drives and SD cards mount there), the whole computer.
  static std::vector<std::pair<std::string, fs::path>> BrowsePlaces() {
    std::vector<std::pair<std::string, fs::path>> out;
    std::error_code ec;
    const fs::path home = BrowseHome();
    out.push_back({"Home", home});
    for (const char* sub : {"Downloads", "Desktop"})
      if (fs::is_directory(home / sub, ec)) out.push_back({sub, home / sub});
    const char* user = std::getenv("USER");
    for (const fs::path& p : {fs::path("/run/media") / (user ? user : ""), fs::path("/run/media"),
                              fs::path("/media") / (user ? user : ""), fs::path("/media"), fs::path("/mnt")}) {
      if (fs::is_directory(p, ec) && !fs::is_empty(p, ec)) {
        out.push_back({"Drives", p});
        break;
      }
    }
    out.push_back({"Computer", fs::path("/")});
    return out;
  }
  static std::string LowerText(std::string t) {
    for (char& c : t) c = char(std::tolower(static_cast<unsigned char>(c)));
    return t;
  }
  void BrowseLoad(const fs::path& dir) {
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
      browse_note_ = "That folder cannot be opened.";
      return;
    }
    std::vector<BrowseEntry> dirs, files;
    for (; it != fs::directory_iterator() && dirs.size() + files.size() < 4000; it.increment(ec)) {
      if (ec) break;
      const std::string name = it->path().filename().string();
      if (name.empty() || name[0] == '.') continue;  // hidden
      std::error_code e2;
      BrowseEntry en{name, it->is_directory(e2), 0};
      if (!en.dir) {
        if (!it->is_regular_file(e2)) continue;
        en.size = it->file_size(e2);
      }
      (en.dir ? dirs : files).push_back(std::move(en));
    }
    auto by_name = [](const BrowseEntry& a, const BrowseEntry& b) { return LowerText(a.name) < LowerText(b.name); };
    std::sort(dirs.begin(), dirs.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);
    browse_entries_.clear();
    if (dir != dir.root_path()) browse_entries_.push_back({"..", true, 0});
    browse_entries_.insert(browse_entries_.end(), dirs.begin(), dirs.end());
    browse_entries_.insert(browse_entries_.end(), files.begin(), files.end());
    browse_dir_ = dir;
    browse_row_ = browse_entries_.size() > 1 && browse_entries_[0].name == ".." ? 1 : 0;
    browse_top_ = 0;
    browse_note_.clear();
  }
  void BrowseOpen() {
    browsing_ = true;
    fs::path start = fs::path(settings_.Get("setup_browse_dir", ""));
    std::error_code ec;
    if (start.empty() || !fs::is_directory(start, ec)) start = BrowseHome();
    BrowseLoad(start);
    settle_until_ = ImGui::GetTime() + 0.25;  // the press that opened it does not act in it
    REXLOG_INFO("BF setup: our file browser opened at {}", browse_dir_.string());
  }
  static std::string SizeText(uint64_t b) {
    char t[32];
    if (b >= (1ull << 30)) std::snprintf(t, sizeof(t), "%.1f GB", double(b) / double(1ull << 30));
    else if (b >= (1ull << 20)) std::snprintf(t, sizeof(t), "%.0f MB", double(b) / double(1ull << 20));
    else std::snprintf(t, sizeof(t), "%.0f KB", double(b) / 1024.0);
    return t;
  }
  // Drawn over the files page. True once something is picked (`picked`); the browser is then closed.
  bool BrowseDraw(ImDrawList* d, ImGuiIO& io, const Stage& st, Input in, fs::path& picked) {
    const kit::Fonts& f = kit::GetFonts();
    if (!Settled()) in = {};
    const auto places = BrowsePlaces();
    const ImVec2 a = st.P(100, 64), b = st.P(1180, 636);
    kit::Paper(d, a, b, st.u);
    const float x = a.x + st(36), w = b.x - a.x - st(72);
    float y = a.y + st(26);
    kit::Spaced(d, f.typed, st(15), ImVec2(x, y), kit::kInkFaint, "FIRST TIME SETUP \xC2\xB7 YOUR GAME FILES", st(2));
    y += st(22);
    kit::Heading(d, st(40), ImVec2(x, y), kit::kInk, "CHOOSE YOUR GAME FILES", st(2), w);
    y += st(50);
    kit::Wrapped(d, f.ui, st(17), ImVec2(x, y), kit::kInkSoft,
                 "Open folders with A. Pick your package file (the single file in 5841097E/000D0000), or press X in the "
                 "folder that holds it, or in an unpacked game folder.",
                 w);
    y += st(46);
    // The places, as tabs: a click, or LB and RB.
    int place_clicked = -1;
    {
      float px = x;
      for (int i = 0; i < int(places.size()); ++i) {
        const std::string label = Upper(places[size_t(i)].first);
        const float tw = kit::TextWidth(f.ui_bold, st(16), label) + st(26);
        const ImVec2 t0(px, y), t1(px + tw, y + st(30));
        const bool here = browse_dir_ == places[size_t(i)].second;
        const bool hover = ImGui::IsMouseHoveringRect(t0, t1);
        d->AddRectFilled(t0, t1, here ? kit::kGreen : hover ? kit::kPaperDim : kit::kPaperCard);
        d->AddRect(t0, t1, kit::kBorder, 0, 0, st(1.5f));
        d->AddText(f.ui_bold, st(16), ImVec2(t0.x + st(13), t0.y + st(6)), here ? IM_COL32(255, 255, 255, 255) : kit::kInk,
                   label.c_str());
        if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) place_clicked = i;
        px = t1.x + st(8);
      }
    }
    y += st(40);
    // The folder shown, its start cut when it is too long.
    {
      std::string p = browse_dir_.string();
      const float pw = w;
      while (p.size() > 4 && kit::TextWidth(f.typed, st(16), p) > pw) p = "..." + p.substr(4);
      d->AddText(f.typed, st(16), ImVec2(x, y), kit::kInk, p.c_str());
    }
    y += st(26);
    // The list.
    const float row_h = st(30), list_top = y, list_bottom = b.y - st(56);
    const int n = int(browse_entries_.size());
    const int visible = std::max(1, int((list_bottom - list_top) / row_h));
    if (n && io.MouseWheel != 0 && ImGui::IsMouseHoveringRect(ImVec2(x, list_top), ImVec2(x + w, list_bottom)))
      browse_row_ = std::clamp(browse_row_ - int(io.MouseWheel * 3), 0, n - 1);
    if (in.up && n) browse_row_ = (browse_row_ + n - 1) % n;
    if (in.down && n) browse_row_ = (browse_row_ + 1) % n;
    browse_row_ = std::clamp(browse_row_, 0, std::max(0, n - 1));
    if (browse_row_ < browse_top_) browse_top_ = browse_row_;
    if (browse_row_ >= browse_top_ + visible) browse_top_ = browse_row_ - visible + 1;
    d->AddRectFilled(ImVec2(x, list_top), ImVec2(x + w, list_bottom), kit::kPaperLight);
    d->AddRect(ImVec2(x, list_top), ImVec2(x + w, list_bottom), kit::kBorder, 0, 0, st(1.5f));
    int clicked = -1;
    for (int i = browse_top_; i < n && i < browse_top_ + visible; ++i) {
      const BrowseEntry& e = browse_entries_[size_t(i)];
      const ImVec2 r0(x, list_top + float(i - browse_top_) * row_h), r1(x + w, r0.y + row_h);
      const bool hover = ImGui::IsMouseHoveringRect(r0, r1);
      if (hover && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) browse_row_ = i;
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = i;
      const bool sel = i == browse_row_;
      if (sel) d->AddRectFilled(r0, r1, kit::kGreen);
      const ImU32 ink = sel ? IM_COL32(255, 255, 255, 255) : kit::kInk;
      const ImVec2 ic(r0.x + st(12), r0.y + st(7));
      if (e.dir) {  // a folder
        const ImVec2 pts[] = {ImVec2(ic.x, ic.y + st(2)), ImVec2(ic.x + st(7), ic.y + st(2)), ImVec2(ic.x + st(9), ic.y + st(5)),
                              ImVec2(ic.x + st(18), ic.y + st(5)), ImVec2(ic.x + st(18), ic.y + st(16)), ImVec2(ic.x, ic.y + st(16))};
        d->AddPolyline(pts, 6, ink, ImDrawFlags_Closed, st(1.8f));
      } else {  // a file
        d->AddRect(ImVec2(ic.x + st(3), ic.y), ImVec2(ic.x + st(15), ic.y + st(17)), ink, 0, 0, st(1.8f));
      }
      const std::string label = e.name == ".." ? ".. (up one folder)" : e.name;
      d->AddText(f.ui, st(18), ImVec2(r0.x + st(42), r0.y + st(4)), ink, label.c_str());
      if (!e.dir) {
        const std::string sz = SizeText(e.size);
        d->AddText(f.typed, st(15), ImVec2(r1.x - st(14) - kit::TextWidth(f.typed, st(15), sz), r0.y + st(7)), ink, sz.c_str());
      }
    }
    if (!n) d->AddText(f.ui, st(18), ImVec2(x + st(16), list_top + st(8)), kit::kInkSoft, "This folder is empty.");
    if (!browse_note_.empty())
      d->AddText(f.ui_bold, st(17), ImVec2(x, list_bottom + st(10)), kit::kRed, browse_note_.c_str());
    // What the input does.
    auto open_row = [&](int i) -> bool {  // true: a file picked
      if (i < 0 || i >= n) return false;
      const BrowseEntry e = browse_entries_[size_t(i)];
      if (e.name == "..") BrowseLoad(browse_dir_.parent_path());
      else if (e.dir) BrowseLoad(browse_dir_ / e.name);
      else return picked = browse_dir_ / e.name, true;
      return false;
    };
    bool done = false;
    if (clicked >= 0) done = open_row(clicked);
    else if (in.accept) done = open_row(browse_row_);
    else if (in.right && n && browse_entries_[size_t(browse_row_)].dir) open_row(browse_row_);
    else if (in.left && browse_dir_ != browse_dir_.root_path()) BrowseLoad(browse_dir_.parent_path());
    else if (in.x) picked = browse_dir_, done = true;
    else if (place_clicked >= 0 || in.prev_tab || in.next_tab) {
      const int count = int(places.size());
      browse_place_ = place_clicked >= 0 ? place_clicked : (browse_place_ + (in.next_tab ? 1 : count - 1)) % count;
      BrowseLoad(places[size_t(browse_place_)].second);
    } else if (in.back) {
      browsing_ = false;
      settle_until_ = ImGui::GetTime() + 0.25;
      REXLOG_INFO("BF setup: our file browser closed (nothing picked)");
      return false;
    }
    if (!done) return false;
    browsing_ = false;
    settings_.values["setup_browse_dir"] = browse_dir_.string();
    settings_.Save();
    settle_until_ = ImGui::GetTime() + 0.35;
    REXLOG_INFO("BF setup: our file browser picked {}", picked.string());
    return true;
  }

  void FilesPage(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("files");
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    const bool busy = setup_progress_ && !setup_progress_->finished;
    if (setup_progress_ && setup_progress_->finished) {
      if (setup_progress_->failed) {
        std::lock_guard lock(setup_progress_->mutex);
        setup_error_ = setup_progress_->error;
        setup_progress_.reset();
      } else {
        setup_progress_.reset();
        FinishSetup(setup_dir_);
        return;
      }
    }
    Chrome(d, io, st, 1);
    if (!Settled()) in = {};
    const std::vector<Cue> prompts = busy ? std::vector<Cue>{}
                                     : browsing_ ? std::vector<Cue>{{kit::Button::kA, "Open"},
                                                                    {kit::Button::kX, "Use this folder"},
                                                                    {kit::Button::kRB, "Places"},
                                                                    {kit::Button::kB, "Back"}}
                                                 : std::vector<Cue>{{kit::Button::kA, files_card_ == 2 ? "Watch" : "Choose game files"},
                                                                    {kit::Button::kX, "Choose a folder"},
                                                                    {kit::Button::kY, "Where do I find them?"},
                                                                    {kit::Button::kStart, "Video tutorial"},
                                                                    {kit::Button::kB, "Quit"}};
    Prompts(d, st, st.P(150, 720 - 30 - 42), prompts, in);
    // 0.4.7.4: the paper taller for the video tutorial's button (game_menu_tutorial.inc) over the two cards.
    const ImVec2 a = st.P(150, 70), b = st.P(1130, 566);
    int act = -1;  // 0 the package, 1 a folder, 2 the video tutorial
    {
      kit::Turn turn(d, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), -0.4f);
      kit::Paper(d, a, b, st.u);
      const float x = a.x + st(44), w = st(892);
      float y = a.y + st(34);
      kit::Spaced(d, f.typed, st(15), ImVec2(x, y), kit::kInkFaint, "FIRST TIME SETUP \xC2\xB7 STEP 1 OF 8", st(2));
      y += st(24);
      kit::Heading(d, st(58), ImVec2(x, y), kit::kInk, "BRING YOUR OWN COPY", st(2), w);
      y += st(68);
      y += kit::Wrapped(d, f.ui, st(19), ImVec2(x, y), kit::kInkSoft,
                        "Battlefield 1943 Reborn runs on the game files from your own Xbox 360 copy. Point us to them "
                        "once and we set everything up. Nothing of the game is downloaded or shared.",
                        st(820)) +
           st(22);
      const float cw = (w - st(22)) * 0.5f, ch = st(104);
      static const char* kTitles[] = {"Your game package", "An unpacked game folder"};
      static const char* kLines[] = {"The single file in 5841097E\\000D0000, from your console's drive or a backup you made",
                                     "A folder holding default.xex"};
      // The video tutorial (TekRantGaming, 18 Nov 2026): a long button over the two cards, as wide as both; up from
      // the cards reaches it, down goes back to them.
      if (!busy && !browsing_) {
        if (in.up && files_card_ != 2) files_card_ = 2;
        else if (in.down && files_card_ == 2) files_card_ = 0;
        else if ((in.left || in.right) && files_card_ != 2) files_card_ = 1 - files_card_;
      }
      {
        const ImVec2 t0(x, y), t1(x + w, y + st(46));
        const bool hover = !busy && !browsing_ && ImGui::IsMouseHoveringRect(t0, t1);
        if (hover && io.MouseDelta.x + io.MouseDelta.y != 0) files_card_ = 2;
        const bool sel = files_card_ == 2;
        if (sel) {
          d->AddRectFilled(ImVec2(t0.x, t0.y + st(5)), ImVec2(t1.x, t1.y + st(5)), kit::kGreenShadow);
          d->AddRectFilled(t0, t1, kit::kGreen);
        } else {
          d->AddRectFilled(t0, t1, kit::kPaperCard);
          d->AddRect(t0, t1, kit::kBorder, 0, 0, st(2));
        }
        const ImU32 ink = sel ? IM_COL32(255, 255, 255, 255) : kit::kInk;
        NewsPlayMark(d, ImVec2(t0.x + st(36), (t0.y + t1.y) * 0.5f), st(22));
        const float cap = st(20);
        kit::GameText(d, kit::GameFont::kHeadline, cap, ImVec2(t0.x + st(68), (t0.y + t1.y - cap) * 0.5f), ink, "WATCH VIDEO TUTORIAL", st(1));
        const kit::Style style = PromptStyle();
        const float gw = kit::Glyph(nullptr, style, kit::Button::kStart, KeyFor(kit::Button::kStart), ImVec2(0, 0), st(22), false);
        kit::Glyph(d, style, kit::Button::kStart, KeyFor(kit::Button::kStart), ImVec2(t1.x - st(16) - gw, (t0.y + t1.y) * 0.5f), st(22));
        const std::string note = "3 MINUTES \xC2\xB7 A FAN MADE GUIDE BY LEOTSEIL GAMING";
        kit::Spaced(d, f.typed, st(11), ImVec2(t1.x - st(32) - gw - kit::SpacedWidth(f.typed, st(11), note, st(1.5f)), (t0.y + t1.y) * 0.5f - st(7)),
                    sel ? IM_COL32(255, 255, 255, 220) : kit::kInkFaint, note, st(1.5f));
        if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) act = 2;
        y = t1.y + st(16);
      }
      for (int i = 0; i < 2; ++i) {
        const ImVec2 c0(x + float(i) * (cw + st(22)), y), c1(c0.x + cw, y + ch);
        const bool hover = !busy && !browsing_ && ImGui::IsMouseHoveringRect(c0, c1);
        if (hover && io.MouseDelta.x + io.MouseDelta.y != 0) files_card_ = i;
        const bool sel = files_card_ == i;
        if (sel) {
          d->AddRectFilled(ImVec2(c0.x, c0.y + st(5)), ImVec2(c1.x, c1.y + st(5)), kit::kGreenShadow);
          d->AddRectFilled(c0, c1, kit::kGreen);
        } else {
          d->AddRectFilled(c0, c1, kit::kPaperCard);
          d->AddRect(c0, c1, kit::kBorder, 0, 0, st(2));
        }
        const ImU32 ink = sel ? IM_COL32(255, 255, 255, 255) : kit::kInk;
        const ImVec2 ic(c0.x + st(18), c0.y + st(16));
        if (i == 0) {  // a document
          d->AddRect(ImVec2(ic.x + st(7), ic.y + st(4)), ImVec2(ic.x + st(33), ic.y + st(36)), ink, st(2), 0, st(2.5f));
          for (float ly : {13.0f, 19.0f, 25.0f})
            d->AddLine(ImVec2(ic.x + st(12), ic.y + st(ly)), ImVec2(ic.x + st(ly == 25 ? 22 : 28), ic.y + st(ly)), ink, st(2.5f));
        } else {  // a folder
          const ImVec2 pts[] = {ImVec2(ic.x + st(4), ic.y + st(11)), ImVec2(ic.x + st(16), ic.y + st(11)),
                                ImVec2(ic.x + st(20), ic.y + st(15)), ImVec2(ic.x + st(36), ic.y + st(15)),
                                ImVec2(ic.x + st(36), ic.y + st(34)), ImVec2(ic.x + st(4), ic.y + st(34))};
          d->AddPolyline(pts, 6, sel ? ink : IM_COL32(61, 74, 42, 255), ImDrawFlags_Closed, st(2.5f));
        }
        d->AddText(f.ui_bold, st(24), ImVec2(c0.x + st(72), c0.y + st(12)), ink, kTitles[i]);
        kit::Wrapped(d, f.typed, st(15), ImVec2(c0.x + st(72), c0.y + st(50)), ink, kLines[i], cw - st(90));
        if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) act = i;
      }
      y += ch + st(24);
      // The check's box: what happens, with its bar.
      std::string line = "Choose your game package or folder. Nothing is copied until the files check out.";
      float fraction = 0;
      ImU32 line_ink = kit::kInkSoft;
      if (busy) {
        const double done = double(setup_progress_->done), total = std::max<double>(1.0, double(setup_progress_->total));
        fraction = float(std::clamp(done / total, 0.0, 1.0));
        line = "Package found. Title 5841097E. Installing: " + std::to_string(int(fraction * 100)) + "%, " +
               std::to_string(uint64_t(done) >> 20) + " of " + std::to_string(uint64_t(total) >> 20) + " MB.";
      } else if (!setup_error_.empty()) {
        line = setup_error_, line_ink = kit::kRed;
      } else if (files_help_) {
        line = "On your Xbox 360 the game is a single file in Content\\0000000000000000\\5841097E\\000D0000. Copy it "
               "to a USB drive formatted on the console (a tool such as Velocity or Horizon reads it), or use a backup "
               "of your own disc's content. An unpacked folder with default.xex works too.";
      }
      const float lh = kit::WrappedHeight(f.typed, st(16), line, w - st(36));
      d->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + st(28) + lh + st(20)), kit::kPaperDim);
      kit::Wrapped(d, f.typed, st(16), ImVec2(x + st(18), y + st(14)), line_ink, line, w - st(36));
      const float by = y + st(14) + lh + st(10);
      d->AddRectFilled(ImVec2(x + st(18), by), ImVec2(x + w - st(18), by + st(10)), IM_COL32(34, 36, 27, 38));
      if (fraction > 0) d->AddRectFilled(ImVec2(x + st(18), by), ImVec2(x + st(18) + (w - st(36)) * fraction, by + st(10)), kit::kGreen);
    }
    StampCorner(d, st);
    if (busy) return;
    fs::path picked;
    if (browsing_) {  // our browser (Linux) over the page: it has the input; nothing else here acts
      if (!BrowseDraw(d, io, st, in, picked)) return;
    } else {
      if (in.y) files_help_ = !files_help_;
      if (in.back) {
        if (quit_) quit_();
        return;
      }
      if (in.accept) act = files_card_;
      if (in.x) act = 1;
      if (in.start) act = 2;
      if (act == 2) {
        OpenTutorial(in.start ? "Start or P" : in.accept ? "A or Enter" : "a click");
        return;
      }
#if defined(BF_DEV_TOOLS)
      // BF_DEV_SETUP_PICK=<path> (tests): picked for the player a few seconds after the page shows.
      static bool dev_picked = false;
      static const double pick_at = std::getenv("BF_DEV_SETUP_PICK_AT") ? std::atof(std::getenv("BF_DEV_SETUP_PICK_AT")) : 4.0;
      if (const char* v = std::getenv("BF_DEV_SETUP_PICK"); v && *v && !dev_picked && ImGui::GetTime() > pick_at)
        dev_picked = true, picked = v;
#endif
      if (picked.empty()) {
        if (act < 0) return;
        files_card_ = act;
#if defined(_WIN32)
        picked = Pick(act == 0 ? BrowseForPackage : BrowseForFolder);
        settle_until_ = ImGui::GetTime() + 0.35;  // the press that closed the dialog does not act again
        if (picked.empty()) return;
#else
        BrowseOpen();  // Linux: our own browser (no system picker to ask)
        return;
#endif
      }
    }
    const install::Source source = install::Inspect(picked);
    if (!source.ok) {
      setup_error_ = source.error;
      return;
    }
    setup_error_.clear();
    if (source.extracted) {
      settings_.values["game_dir"] = source.path.string();
      settings_.Save();
      FinishSetup(source.path);
      return;
    }
    REXLOG_INFO("BF setup: installing {} ({}) into {}", source.path.filename().string(), source.name, setup_dir_.string());
    setup_progress_ = std::make_shared<install::Progress>();
    std::thread([progress = setup_progress_, package = source.path, dir = setup_dir_] {
      install::Extract(package, dir, *progress);
    }).detach();
  }

  // ---- 2: the shaders (King Kong's DrawShaderScreen): one bar, no Continue, no Skip

  void ShadersPage(ImDrawList* d, ImGuiIO& io, Input in) {
    ShaderProgress p;
    if (shader_progress_) shader_progress_(&p.queued, &p.done, &p.loading, &p.finished);
    else p.finished = 1;  // a GPU plugin without the storage's progress: nothing to wait for
    // The game was started for this page (PrepareShadersAtStart): until its renderer exists the storage's progress
    // says finished (nothing to wait for), which is not the pipelines made. At most a minute, should it never come.
    if (shaders_wait_from_ >= 0) {
      if (!REXCVAR_GET(native_x_renderer) || native_x_renderer::Renders() || ImGui::GetTime() - shaders_wait_from_ > 60) shaders_wait_from_ = -1;
      else p = {};
    }
    bool forced = false;
#if defined(BF_DEV_TOOLS)
    // BF_SHADER_SCREEN_TEST=<s>: the screen for that many seconds with a made up count (screenshots; the driver
    // keeps its pipelines, so a real first start can't be repeated here).
    static const double test_for = [] {
      const char* v = std::getenv("BF_SHADER_SCREEN_TEST");
      return v && *v ? std::atof(v) : 0.0;
    }();
    if (ImGui::GetTime() < test_for) {
      p.loading = 1, p.finished = 0, p.queued = 1688, forced = true;
      p.done = uint32_t(1688 * std::min(1.0, ImGui::GetTime() / test_for));
    }
#endif
    // King Kong's rule (CheckShadersAfterUpdate): this version's shaders were prepared at an earlier start, so no
    // shader page; the renderer goes on making its stored pipelines behind the menus (OnDraw logs when they are done).
    const bool behind = !p.finished && !forced && (ShadersPreparedAtStart() || shaders_prepared_hint_);
    if (p.finished || behind) {
      if (shaders_shown_) REXLOG_INFO("BF shader screen: {} pipelines made", p.done);
      if (behind) {
        shaders_behind_ = true, shaders_behind_from_ = ImGui::GetTime();
      } else if (settings_.Get("shaders_prepared_for", "") != BfVersion()) {
        // King Kong's marker: written once the page has made them all, never for a page left before the end.
        settings_.values["shaders_prepared_for"] = BfVersion();
        settings_.Save();
        REXLOG_INFO("BF shaders: prepared for v{} ({} pipelines); no shader page again until an update", BfVersion(),
                    p.done);
      }
      AfterShaders();
#if defined(BF_DEV_TOOLS)
      if (const char* v = std::getenv("BF_MENU_PAGE"); v && std::string(v) == "settings") page_ = Page::kSettings;
      if (const char* v = std::getenv("BF_MENU_TAB"); v && *v) tab_ = std::clamp(std::atoi(v), 0, int(kTabCount) - 1);
#endif
      return;
    }
    if (ImGui::GetTime() < 0.6) return;  // nothing to show yet (a quick start draws no screen)
    NotePage("shaders");
    if (!shaders_shown_)
      REXLOG_INFO("BF shader screen: shown ({}); nothing else is drawn over it until it is done",
                  shaders_update_ || ShadersAfterUpdate() ? "after an update, over a fresh cache"
                                                          : "the first start, right after the game files");
    shaders_shown_ = true;
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    // The full screen page on its own, on the first start (right after the game files, before the onboarding) and
    // after an update (TekRantGaming, 16 Nov 2026: the setup's frame and its step 8 are gone). OnDraw draws no pop-up,
    // toast or video over it.
    static const char* const kAbout = "Your graphics card is building every shader the game uses, once, so nothing stutters or pops "
                                      "in while you play. This happens after you install and after each update.";
    const bool full = std::atoi(Cvar("native_x_renderer_compile_threads").c_str()) > 0;
    Chrome(d, io, st, 0);
    Prompts(d, st, st.P(40, 720 - 26 - 40), {{kit::Button::kA, full ? "FULL CPU: ON" : "FULL CPU: OFF"}}, in, 16);
    const float fraction = p.loading && p.queued ? std::clamp(float(p.done) / float(p.queued), 0.0f, 1.0f) : -1.0f;
    shader_bar_ = fraction < 0 ? 0 : std::max(shader_bar_, shader_bar_ + (fraction - shader_bar_) * 0.15f);
    if (fraction >= 0 && shader_started_ < 0) shader_started_ = ImGui::GetTime(), shader_started_done_ = p.done;
    const ImVec2 a = st.P(240, 130), b = st.P(1040, 500);
    int clicked = -1;
    {
      std::optional<kit::Turn> turn;
      const float x = a.x + st(44), w = st(712);
      float y = a.y + st(34);
      turn.emplace(d, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), 0.4f);
      kit::Paper(d, a, b, st.u, false);
      // An update known from the menu's marker (PrepareShadersAtStart) before the game's own check has run.
      kit::Spaced(d, f.typed, st(15), ImVec2(x, y), kit::kInkFaint,
                  shaders_update_ || ShadersAfterUpdate() ? "UPDATED TO VERSION " + std::string(BfVersion()) : "BEFORE YOU PLAY",
                  st(2));
      y += st(24);
      kit::Heading(d, st(56), ImVec2(x, y), kit::kInk, "PREPARING THE SHADERS", st(2), w);
      y += st(68);
      y += kit::Wrapped(d, f.ui, st(19), ImVec2(x, y), kit::kInkSoft, kAbout, w) + st(20);
      std::string count = "Checking", left;
      if (fraction >= 0) {
        count = std::to_string(p.done) + " of " + std::to_string(p.queued) + " pipelines";
        const double took = ImGui::GetTime() - shader_started_;
        const double made = double(p.done) - double(shader_started_done_);
        if (took > 2 && made > 0) {
          const int secs = int((double(p.queued) - double(p.done)) / (made / took));
          left = secs < 60 ? "about " + std::to_string(std::max(secs, 1)) + " seconds left"
                           : "about " + std::to_string((secs + 30) / 60) + " minutes left";
        }
      }
      d->AddText(f.ui, st(17), ImVec2(x, y), kit::kInk, count.c_str());
      d->AddText(f.typed, st(17), ImVec2(x + w - kit::TextWidth(f.typed, st(17), left), y + st(3)), kit::kInk, left.c_str());
      y += st(32);
      const ImVec2 b0(x, y), b1(x + w, y + st(22));
      d->AddRectFilled(b0, b1, IM_COL32(34, 36, 27, 38));
      const float fx = fraction >= 0 ? b0.x + (b1.x - b0.x) * shader_bar_ : 0;
      if (fraction >= 0 && fx > b0.x) {  // striped green, as the mockup
        d->AddRectFilled(b0, ImVec2(fx, b1.y), kit::kGreen);
        d->PushClipRect(b0, ImVec2(fx, b1.y), true);
        for (float sx = b0.x - st(28); sx < fx; sx += st(28))
          d->AddQuadFilled(ImVec2(sx, b1.y), ImVec2(sx + st(14), b1.y), ImVec2(sx + st(14) + st(22), b0.y),
                           ImVec2(sx + st(22), b0.y), IM_COL32(79, 122, 41, 255));
        d->PopClipRect();
      } else if (fraction < 0) {  // before the count is known: a moving segment
        const float t = float(std::fmod(ImGui::GetTime() * 0.6, 1.0));
        d->AddRectFilled(ImVec2(b0.x + w * std::max(0.0f, t * 1.3f - 0.3f), b0.y),
                         ImVec2(b0.x + w * std::min(1.0f, t * 1.3f), b1.y), kit::kGreen);
      }
      d->AddRect(b0, b1, kit::kInk, 0, 0, st(2));
      y += st(36);
      d->AddText(f.typed, st(15), ImVec2(x, y), kit::kInkFaint, "Your PC may be slower until this finishes.");
      y += st(34);
      // "Damn it, I paid for the full CPU" (King Kong's box): every core compiles, from the next start (the
      // compile threads are made with the device).
      const ImVec2 c0(x, y), c1(x + w, y + st(62));
      d->AddRectFilled(c0, c1, kit::kPaperDim);
      const ImVec2 k0(x + st(14), y + st(14)), k1(k0.x + st(26), k0.y + st(26));
      d->AddRectFilled(k0, k1, kit::kPaper);
      d->AddRect(k0, k1, kit::kInk, 0, 0, st(3));
      if (full) {
        const ImVec2 t0(k0.x + st(5), k0.y + st(13)), t1(k0.x + st(11), k0.y + st(20)), t2(k0.x + st(22), k0.y + st(6));
        d->AddLine(t0, t1, kit::kGreen, st(4)), d->AddLine(t1, t2, kit::kGreen, st(4));
      }
      d->AddText(f.ui, st(20), ImVec2(k1.x + st(14), y + st(8)), kit::kInk, "Damn it, I paid for the full CPU");
      d->AddText(f.typed, st(14), ImVec2(k1.x + st(14), y + st(38)), kit::kInkFaint,
                 "Uses every CPU core. Faster, but some PCs crash. From the next start.");
      if (ImGui::IsMouseHoveringRect(c0, c1) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = 0;
    }
    StampCorner(d, st);
    if (in.accept) clicked = 0;
    if (clicked == 0) {
      const unsigned all = std::max(4u, std::thread::hardware_concurrency());
      SetCvar("native_x_renderer_compile_threads", full ? "0" : std::to_string(all));
      REXLOG_INFO("BF launcher: full CPU for the shaders {}", full ? "off" : "on");
    }
  }

  // ---- 3: the account (the soldier name; the on screen keys for a controller)

  // The name from Settings or Home (done empty) or in the setup (setup true).
  void BeginAccount(bool setup) {
    name_setup_ = setup;
    name_text_ = settings_.Get("player_name", "");
    // The name made up at an earlier start (Soldier and four digits): the player types their own.
    if (name_text_.size() == 11 && name_text_.rfind("Soldier", 0) == 0 && settings_.Get("player_name_chosen", "") != "1")
      name_text_.clear();
    // With a controller in hand the keys start open (TekRantGaming, 9 Nov: the controller could not reach them).
    name_key_ = 10, name_keys_ = device_ == Device::kPad, name_caps_ = name_text_.empty();  // on Q
    name_error_.clear();
    TagBegin();  // the clan tag box (game_menu_clan.inc)
    code_mode_ = false, acct_show_code_ = false;
    NpBegin();  // the page's own state (game_menu_account.inc)
    if (!acct_busy_.load()) acct_message_.clear();
    if (settings_.Get("player_name_refused", "") == "1") name_error_ = "Your soldier name is not allowed. Choose a new one.";
    if (page_ != Page::kName) name_from_ = page_;
    page_ = Page::kName, open_ = true;
  }
  // The saved name was refused at the start (profanity; ApplyPlayerIdentity in game_menu.cpp) after the setup was
  // done: Home with the name page over it, saying why, and no Back until a new name is saved. BF signs in only then
  // (LauncherWalks waits for a chosen name), under the new name. A setup not yet done asks for the name in its turn.
  bool RenameRequired() {
    if (settings_.Get("player_name_refused", "") != "1" || settings_.Get("launcher_setup", "") != "done") return false;
    page_ = Page::kHome, home_row_ = 0;
    BeginAccount(false);  // its error line says why
    name_required_ = true;
    REXLOG_INFO("BF name: the name page for a new name (the saved one was refused)");
    return true;
  }
  static bool NameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
  void NameType(char c) {
    if (code_mode_) return CodeType(c);
    if (tag_mode_) return TagType(c);
    if (!player_name::CanType(name_text_) || !NameChar(c)) return;
    if (std::isalpha(static_cast<unsigned char>(c)))
      c = name_caps_ ? char(std::toupper(static_cast<unsigned char>(c))) : char(std::tolower(static_cast<unsigned char>(c)));
    name_text_ += c;
    name_error_.clear();
    if (std::isalpha(static_cast<unsigned char>(c)) && name_caps_ && name_text_.size() == 1) name_caps_ = false;
  }
  // The trademark sign at the name's end (player_name.h: once, last, its 3 bytes within the 15): typed with Alt+0153,
  // pasted, or the on screen keys' TM.
  void NameTypeTm() {
    if (code_mode_ || tag_mode_ || !player_name::CanTypeTm(name_text_)) return;
    name_text_ += player_name::kTm;
    name_error_.clear(), name_caps_ = false;
  }
  // The clipboard's text into the box with the typing, as the keyboard types (TekRantGaming, 16 Nov 2026: a recovery
  // code could only be typed letter by letter). A recovery code replaces its box, spaces and dashes left out; from a
  // copied "Recovery code: ..." line (or the whole key file) only the code.
  void NamePaste(std::string text, const char* how) {
    if (code_mode_) {
      std::string lower;
      for (char ch : text) lower += char(std::tolower(static_cast<unsigned char>(ch)));
      if (const size_t at = lower.find("code:"); at != std::string::npos) {
        text = text.substr(at + 5);
        if (const size_t eol = text.find_first_of("\r\n"); eol != std::string::npos) text.resize(eol);
      }
      name_text_.clear();
    }
    int n = 0;
    for (size_t i = 0; i < text.size(); ++i) {
      const char ch = text[i];
      if (!code_mode_ && !tag_mode_ && text.compare(i, player_name::kTm.size(), player_name::kTm) == 0) {
        NameTypeTm(), i += player_name::kTm.size() - 1, ++n;
        continue;
      }
      if (static_cast<unsigned char>(ch) >= 128 || ch == '\r' || ch == '\n') continue;
      if (code_mode_) CodeType(ch);
      else if (tag_mode_) TagType(ch, true);
      else if (NameChar(ch) && player_name::CanType(name_text_)) name_text_ += ch, name_error_.clear(), name_caps_ = false;
      ++n;
    }
    name_keys_ = false;
    REXLOG_INFO("BF launcher: {} characters pasted into the {} ({})", n,
                code_mode_ ? "recovery code" : tag_mode_ ? "clan tag" : "soldier name", how);
  }
  // The box with the typing emptied (the right click menu's CLEAR).
  void NameClear() {
    if (tag_mode_ && !code_mode_) tag_text_.clear();
    else name_text_.clear(), name_caps_ = true;
    name_error_.clear(), tag_refusal_.clear();
  }
  void NameDelete() {
    if (tag_mode_ && !code_mode_) return TagDelete();
    player_name::PopBack(name_text_);
    if (name_text_.empty()) name_caps_ = true;
  }
  void NameRandom() {
    static const char* kFirst[] = {"Wake", "Iwo", "Pacific", "Coral", "Marine", "Raider", "Gunner", "Atoll",
                                   "Sand", "Jungle", "Corsair", "Zero", "Ridge", "Harbor", "Island", "Tide"};
    static const char* kSecond[] = {"Fox", "Hawk", "Wolf", "Shark", "Viper", "Ranger", "Tiger", "Falcon",
                                    "Cobra", "Bear", "Eagle", "Lynx", "Mako", "Raven", "Bison", "Orca"};
    std::random_device r;
    name_text_ = std::string(kFirst[r() % 16]) + kSecond[r() % 16] + std::to_string(10 + r() % 90);
    name_error_.clear(), name_caps_ = false;
  }
  // The name in the field saved (false, with the error line saying why, when it cannot be).
  bool NameCommit() {
    const std::string name = name_text_;
    if (player_name::PlainChars(name) < 3) {
      name_error_ = "A name needs 3 to 15 letters, numbers or underscores.";
      return false;
    }
    // Our profanity filter (text_filter.h): such a name is never saved; the player chooses another.
    if (!text_filter::NameAllowed(name)) {
      name_error_ = "That name is not allowed. Choose another one.";
      REXLOG_INFO("BF name: a name with profanity was refused on the name page");
      return false;
    }
    if (!TagCheck(name)) return false;  // the clan tag with it (game_menu_clan.inc)
    const bool changed = name != settings_.Get("player_name", "") || settings_.Get("player_name_chosen", "") != "1";
    settings_.values["player_name"] = name;
    settings_.values["player_name_chosen"] = "1";
    settings_.values.erase("player_name_refused");
    name_required_ = false;
    settings_.Save();
    TagSave();
    if (changed) REXLOG_INFO("BF name: the player chose {}", name);
    // Before BF signs in (the setup on a first start) the name is used at once; after, from the next start.
    if (!signed_in_once_) {
      if (changed || name_ready_at_ == 0) NameChosen(name), name_ready_at_ = ImGui::GetTime();
    } else if (changed) {
      name_saved_ = name;
    }
    return true;
  }
  void NameSave() {
    if (code_mode_) {  // the field holds a recovery code: Save uses it
      RecoverAccount();
      return;
    }
    if (!NameCommit()) return;
    if (name_setup_) SetupNext();
    else page_ = name_from_;
  }

  // ---- Protect your name (account_key.h; server/plasma.py accounts). The name page protects the saved name: the
  // game makes the account's recovery code (or uses the one it has: a protected name moves to the new one), the backend
  // keeps the key's public half with the name. The code is shown once and saved to a text file in the user folder; on
  // a new PC "Recovery code" on this page takes the name and the progress back.
  bool AccountOnline() const { return !std::string(REXCVAR_GET(bf_online_host)).empty(); }
  bool NameProtected() const {
    const std::string acct = settings_.Get("account_name", ""), name = settings_.Get("player_name", "");
    if (settings_.Get("account_code", "").empty() || acct.size() != name.size() || acct.empty()) return false;
    for (size_t i = 0; i < acct.size(); ++i)
      if (std::tolower(static_cast<unsigned char>(acct[i])) != std::tolower(static_cast<unsigned char>(name[i]))) return false;
    return true;
  }
  fs::path RecoveryFile() const { return settings_.path.parent_path() / "BF1943 Reborn recovery code.txt"; }
  void SaveRecoveryFile(const std::string& name, const std::string& code) {
    std::ofstream out(RecoveryFile(), std::ios::binary | std::ios::trunc);
    out << "Battlefield 1943 Reborn: the recovery code of your protected soldier name\r\n\r\n"
        << "Name: " << name << "\r\nRecovery code: " << account::FormatCode(code) << "\r\n\r\n"
        << "Keep this code somewhere safe. On a new PC or after a reinstall, open the soldier name page\r\n"
        << "(Settings, or EDIT on Home), choose Recovery code and type it in: your name and your progress come back.\r\n"
        << "Anyone with this code can use your name, so do not share it.\r\n";
    REXLOG_INFO("BF account: the recovery code saved to {}", out ? RecoveryFile().string() : std::string("(not saved)"));
  }
  void AccountMessage(std::string text, bool bad) { acct_message_ = std::move(text), acct_message_bad_ = bad; }
  void ProtectName() {
    if (acct_busy_.load()) return;
    if (!AccountOnline()) return AccountMessage("Protecting a name needs the online service.", true);
    if (!NameCommit()) return;  // the name line says why
    std::string code = settings_.Get("account_code", "");
    if (account::NormalizeCode(code).empty()) code = account::NewCode();
#if defined(BF_DEV_TOOLS)
    // BF_DEV_EXAMPLE_CODE=1 (tests on a test backend only): the example code the page's pictures may show, never a real one.
    if (const char* v = std::getenv("BF_DEV_EXAMPLE_CODE"); v && *v == '1' && settings_.Get("account_code", "").empty())
      code = "ABCDEFGHJKMNPQRSTVWX";
#endif
    if (code.empty()) return AccountMessage("This PC gave no random numbers for a code. Try again.", true);
    char id[9];
    std::snprintf(id, sizeof(id), "%08X", OnlinePlayerId());
    const std::string name = settings_.Get("player_name", ""), fields = name + "|" + id;
    acct_busy_ = true;
    AccountMessage("Protecting " + name + "...", false);
    REXLOG_INFO("BF account: protecting the name {}", name);
    std::thread([this, code, name, fields] {
      std::string reply;
      AccountResult r;
      r.code = code, r.name = name;
      r.answered = SignedRegistryRequest(code, "claim", fields, reply);
      r.ok = r.answered && JsonField(reply, "ok") == "true";
      r.why = JsonField(reply, "why");
      if (r.ok) r.name = JsonField(reply, "name");
      r.ready = true;
      std::lock_guard lock(acct_mutex_);
      acct_result_ = r;
    }).detach();
  }
  void RecoverAccount() {
    if (acct_busy_.load()) return;
    const std::string code = account::NormalizeCode(name_text_);
    if (code.empty()) {
      name_error_ = "A recovery code has 20 letters and numbers.";
      return;
    }
    if (!AccountOnline()) return AccountMessage("A recovery code needs the online service.", true);
    acct_busy_ = true;
    AccountMessage("Looking up your recovery code...", false);
    REXLOG_INFO("BF account: a recovery code typed in");
    std::thread([this, code] {
      std::string reply;
      AccountResult r;
      r.recover = true, r.code = code;
      r.answered = SignedRegistryRequest(code, "recover", "", reply);
      r.ok = r.answered && JsonField(reply, "ok") == "true";
      r.why = JsonField(reply, "why");
      r.name = JsonField(reply, "name");
      r.progress = JsonField(reply, "progress_text");
      r.ready = true;
      std::lock_guard lock(acct_mutex_);
      acct_result_ = r;
    }).detach();
  }
  // A claim's or a recovery's outcome, on the menu's thread (the settings are its).
  void ApplyAccountResult() {
    AccountResult r;
    {
      std::lock_guard lock(acct_mutex_);
      if (!acct_result_.ready) return;
      r = acct_result_;
      acct_result_ = {};
    }
    acct_busy_ = false;
    if (!r.answered) {
      // The name is saved; the switch goes off, so Save goes on without protection (the page says so).
      np_pending_save_ = false;
      if (!r.recover) np_protect_ = false;
      REXLOG_WARN("BF account: no answer from the backend ({}){}", r.recover ? "recovery" : "protect",
                  r.why.empty() ? std::string() : ": " + r.why);
      if (r.recover)
        return AccountMessage(r.why == "lan" ? "A recovery code needs the Reborn servers, not a LAN server."
                                             : "No answer from the Reborn servers. Nothing is lost: try again in a moment.",
                              true);
      return AccountMessage(r.why == "lan" ? "Names are protected by the Reborn servers, not by a LAN server. Your name is saved: "
                                             "Save again goes on without protection."
                                           : "No answer from the Reborn servers. Your name is saved: Save again goes on without "
                                             "protection (protect it later from EDIT on Home), or switch it on and try again.",
                            true);
    }
    if (!r.recover) {
      if (!r.ok) {
        np_pending_save_ = false;
        REXLOG_INFO("BF account: protecting {} refused: {}", r.name, r.why);
        return AccountMessage(r.why == "taken"    ? "That name is protected by another player. Choose another one."
                              : r.why == "team_name" ? "That name is kept for the Battlefield 1943 Reborn team. Choose "
                                                       "another one."
                              : r.why == "in_use" ?"Another player is using that name online right now. Choose another "
                                                    "one, or try again later."
                              : r.why == "too_many" ? "Too many names were protected from your connection this hour. "
                                                      "Try again later."
                                                    : "The online service did not take that name.",
                              true);
      }
      const bool first = settings_.Get("account_code", "") != r.code;
      settings_.values["account_code"] = r.code;
      settings_.values["account_name"] = r.name;
      settings_.Save();
      SetAccount(r.code, r.name);
      SaveRecoveryFile(r.name, r.code);
      acct_online_ = 1;
      acct_show_code_ = first;
      REXLOG_INFO("BF account: {} is protected{}", r.name, first ? " (a new account)" : " (the account moved to it)");
      SendLeaderboard("the name protected");  // the totals so far go to the account
      // A new code: its card in the rail, shown once, with the tick before Continue (game_menu_account.inc). A protection
      // moved to the new name keeps its code: Save goes on at once.
      if (first) {
        np_card_ = 1, np_ticked_ = false;
        REXLOG_INFO("BF name page: the recovery code's card (shown once)");
        return AccountMessage("", false);
      }
      if (np_pending_save_ && page_ == Page::kName) {
        NpFinish();
        return AccountMessage("", false);
      }
      return AccountMessage(first ? "Your name is protected. Keep the recovery code above: it is also saved in your "
                                    "game's user folder as \"BF1943 Reborn recovery code.txt\"."
                                  : "Your protected name is now " + r.name + ". Your recovery code stays the same.",
                            false);
    }
    if (!r.ok) {
      REXLOG_INFO("BF account: the recovery code matches no account");
      return AccountMessage("No account has that recovery code. Check it and try again.", true);
    }
    settings_.values["account_code"] = r.code;
    settings_.values["account_name"] = r.name;
    code_mode_ = false;
    if (!r.progress.empty()) TakeBackProgress(r.progress, "the recovery code");
    if (r.name.empty()) {
      settings_.Save();
      SetAccount(r.code, "");
      name_text_ = settings_.Get("player_name", "");
      REXLOG_INFO("BF account: recovered an account whose name was freed (unused too long)");
      return AccountMessage("Your account is back, but its name went unused too long and was freed. Choose a name and "
                            "protect it again.",
                            true);
    }
    name_text_ = r.name;
    SetAccount(r.code, r.name);
    // A clan tag the page refuses never stops a recovery (16 Nov 2026: "dev" in the box left the recovered name and
    // the account unsaved after "Welcome back"): the name is saved without it.
    if (!NameCommit()) {
      REXLOG_INFO("BF account: the clan tag \"{}\" left out so the recovered name is saved", tag_text_);
      tag_text_.clear(), name_error_.clear();
      NameCommit();
    }
    acct_online_ = 1;
    SaveRecoveryFile(r.name, r.code);
    REXLOG_INFO("BF account: recovered {} with its progress", r.name);
    AccountMessage("Welcome back, " + r.name + ". Your name and your progress are back on this PC" +
                       (signed_in_once_ ? std::string(" (the name from your next start).") : std::string(".")),
                   false);
  }
  // The account's progress (score|seconds|captures|Label=events,points;...|1,7,...) into this PC's: the larger of each
  // total, the awards together. Never into a test profile.
  void TakeBackProgress(const std::string& text, const char* from) {
    if (settings_.Get("test_progression", "") == "1") return;
    std::vector<std::string> parts;
    size_t at = 0;
    for (size_t bar; (bar = text.find('|', at)) != std::string::npos; at = bar + 1) parts.push_back(text.substr(at, bar - at));
    parts.push_back(text.substr(at));
    if (parts.size() != 5) return;
    int raised = 0, unlocked = 0;
    const double seconds = std::atof(parts[1].c_str());
    if (seconds > std::atof(settings_.Get("match_seconds", "0").c_str()))
      settings_.values["match_seconds"] = std::to_string(int64_t(seconds)), ++raised;
    for (size_t i = 0; i <= parts[3].size();) {
      size_t end = parts[3].find(';', i);
      if (end == std::string::npos) end = parts[3].size();
      const std::string item = parts[3].substr(i, end - i);
      i = end + 1;
      const size_t eq = item.find('='), comma = item.find(',');
      if (eq == std::string::npos || comma == std::string::npos || comma < eq) continue;
      const std::string key = "stat." + item.substr(0, eq), old = settings_.Get(key, "0,0");
      const size_t old_comma = old.find(',');
      const long long n = std::atoll(item.c_str() + eq + 1), pts = std::atoll(item.c_str() + comma + 1);
      const long long on = std::atoll(old.c_str()),
                      opts = old_comma == std::string::npos ? 0 : std::atoll(old.c_str() + old_comma + 1);
      if (n > on || pts > opts) settings_.values[key] = fmt::format("{},{}", std::max(n, on), std::max(pts, opts)), ++raised;
    }
    if (auto* k = REX_KERNEL_STATE()) {
      reborn_achievements::Ensure();  // Reborn's own among the account's awards too
      for (size_t i = 0; i < parts[4].size();) {
        size_t end = parts[4].find(',', i);
        if (end == std::string::npos) end = parts[4].size();
        const unsigned long id = std::strtoul(parts[4].substr(i, end - i).c_str(), nullptr, 10);
        i = end + 1;
        if (id && !k->achievements().IsUnlocked(uint32_t(id)) &&
            k->achievements().UnlockAchievement(uint32_t(id)) == rex::system::AchievementUnlockResult::kUnlocked)
          ++unlocked;
      }
    }
    if (raised) settings_.Save();
    REXLOG_INFO("BF account: progress from {}: {} totals raised, {} awards unlocked", from, raised, unlocked);
  }
  // Recovery code typing: letters and digits, upper case, 20 at most (spaces and dashes left out).
  void CodeType(char c) {
    if (!std::isalnum(static_cast<unsigned char>(c)) || name_text_.size() >= size_t(account::kCodeLength)) return;
    name_text_ += char(std::toupper(static_cast<unsigned char>(c)));
    name_error_.clear();
  }
  void BeginCodeMode(bool on) {
    code_mode_ = on;
    name_text_ = on ? std::string() : settings_.Get("player_name", "");
    name_error_.clear();
    acct_show_code_ = false;
    if (on) AccountMessage("", false);
  }
#if defined(BF_DEV_TOOLS)
  // Tests (a background window gets no typing): whether the game window's text input is on (the SDK starts SDL's text
  // input from it), logged when it changes. Checked from the name page's frames, so a change shows the frame after.
  void DevTextInputCheck() {
    rex::ui::Window* w = window_ ? window_() : nullptr;
    if (!w) return;
    static int last = -1;
    const bool on = w->IsTextInputActive();
    if (int(on) != last) {
      last = int(on);
      REXLOG_INFO("BF dev: the window's text input is {}", on ? "on" : "off");
    }
  }
#endif
  // The on screen keys, QWERTY (TekRantGaming, 8 Nov): the numbers, three rows of letters with the underscore, then
  // Capitals and Done. Ten columns; a key's span is in columns. One keyboard for every page that takes text from a
  // controller: the soldier name, and the Custom Match page's server name and password (16 Nov 2026). `space`: the
  // last row with a space bar between Capitals and Done (the match's name).
  struct OnKey {
    const char* label;
    char ch;   // the character typed, or 0: a special key (kind)
    int kind;  // 0 a character, 1 Delete, 2 Capitals, 3 Done, 4 the trademark sign (the soldier name's keys only)
    int row, col, span;
  };
  static const std::vector<OnKey>& OnKeys(bool space, bool tm = false) {
    static const auto make = [](bool with_space, bool with_tm) {
      std::vector<OnKey> v;
      static const char* kRows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL_", "ZXCVBNM"};
      static char labels[40][2] = {};
      int n = 0;
      for (int r = 0; r < 4; ++r)
        for (int c = 0; kRows[r][c]; ++c, ++n) {
          labels[n][0] = kRows[r][c];
          v.push_back({labels[n], kRows[r][c], 0, r, c, 1});
        }
      if (with_tm) {  // the soldier name's keys: TM after M, Delete one column narrower (player_name.h)
        v.push_back({"\xE2\x84\xA2", 0, 4, 3, 7, 1});
        v.push_back({"DEL", 0, 1, 3, 8, 2});
      } else {
        v.push_back({"DEL", 0, 1, 3, 7, 3});
      }
      if (with_space) {
        v.push_back({"CAPS", 0, 2, 4, 0, 3});
        v.push_back({"SPACE", ' ', 0, 4, 3, 4});
        v.push_back({"DONE", 0, 3, 4, 7, 3});
      } else {
        v.push_back({"CAPS", 0, 2, 4, 0, 5});
        v.push_back({"DONE", 0, 3, 4, 5, 5});
      }
      return v;
    };
    static const std::vector<OnKey> kPlain = make(false, false), kSpaced = make(true, false), kName = make(false, true);
    return space ? kSpaced : tm ? kName : kPlain;
  }
  // The keys drawn with their title at `k0` and `hint` under them; while `open`, the pad moves `key` along the row,
  // and up or down to the key nearest above or below. Returns the key clicked, or pressed with A while open, or -1.
  int OnScreenKeys(ImDrawList* d, const Stage& st, ImVec2 k0, const Input& in, int& key, bool open, bool caps, bool space,
                   const char* hint, bool tm = false) {
    const kit::Fonts& f = kit::GetFonts();
    const std::vector<OnKey>& keys = OnKeys(space, tm);
    const int all = int(keys.size());
    if (key < 0 || key >= all) key = 10;
    kit::Spaced(d, f.typed, st(14), k0, kit::kKhakiText, "ON SCREEN KEYBOARD", st(2));
    const float cell = st(36), gap = st(4.5f), kh = st(40);
    int clicked = -1;
    for (int k = 0; k < all; ++k) {
      const OnKey& on = keys[size_t(k)];
      const ImVec2 c0(k0.x + float(on.col) * (cell + gap), k0.y + st(26) + float(on.row) * (kh + gap));
      const ImVec2 c1(c0.x + float(on.span) * cell + float(on.span - 1) * gap, c0.y + kh);
      std::string label = on.label;
      if (std::isalpha(static_cast<unsigned char>(on.ch)) && !caps)
        label[0] = char(std::tolower(static_cast<unsigned char>(label[0])));
      const bool hover = ImGui::IsMouseHoveringRect(c0, c1);
      const bool sel = open && k == key;
      d->AddRectFilled(c0, c1, sel ? kit::kGreen : hover ? kit::kPaper : kit::kPaperCard);
      if (on.kind == 2 && caps) d->AddRectFilled(ImVec2(c0.x, c1.y - st(3)), c1, sel ? kit::kCream : kit::kGreen);
      const float size = on.kind == 0 && on.ch != ' ' ? st(18) : st(15);
      d->AddText(f.ui, size, ImVec2((c0.x + c1.x - kit::TextWidth(f.ui, size, label)) * 0.5f, c0.y + (kh - size * 1.48f) * 0.5f),
                 sel ? IM_COL32(255, 255, 255, 255) : kit::kInk, label.c_str());
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = k, key = k;
    }
    if (hint)
      kit::Wrapped(d, f.typed, st(13), ImVec2(k0.x, k0.y + st(26) + 5 * (kh + gap) + st(6)), kit::kKhakiText, hint, st(400));
    if (!open) return clicked;
    const OnKey& now = keys[size_t(key)];
    if (in.left || in.right) {
      std::vector<int> row;
      for (int k = 0; k < all; ++k)
        if (keys[size_t(k)].row == now.row) row.push_back(k);
      const int at = int(std::find(row.begin(), row.end(), key) - row.begin()), n = int(row.size());
      key = row[size_t((at + (in.right ? 1 : n - 1)) % n)];
    }
    if (in.up || in.down) {
      const int to = (now.row + (in.down ? 1 : 4)) % 5;
      const float mid = float(now.col) + float(now.span) * 0.5f;
      int best = key;
      float best_d = 1e9f;
      for (int k = 0; k < all; ++k) {
        if (keys[size_t(k)].row != to) continue;
        const float dist = std::fabs(float(keys[size_t(k)].col) + float(keys[size_t(k)].span) * 0.5f - mid);
        if (dist < best_d) best_d = dist, best = k;
      }
      key = best;
    }
    if (in.accept) clicked = key;
    return clicked;
  }
  // The rank: a first version from the player's own time in matches (Settings file, "match_seconds"), until a stats
  // service keeps the game's own score. The ranks of the US Marines the game's soldiers wear.
  static constexpr const char* kRanks[] = {
      "Private",          "Private First Class", "Lance Corporal",       "Corporal",           "Sergeant",
      "Staff Sergeant",   "Gunnery Sergeant",    "Master Sergeant",      "First Sergeant",     "Master Gunnery Sergeant",
      "Sergeant Major",   "Warrant Officer",     "Chief Warrant Officer", "Second Lieutenant", "First Lieutenant",
      "Captain",          "Major",               "Lieutenant Colonel",   "Colonel",            "Brigadier General",
      "Major General",    "Lieutenant General",  "General"};
  int Rank(const Settings& record, double seconds) {
    const std::string value = record.Get("rank", "");
    const char* end = value.data() + value.size();
    int requested = 0;
    const auto parsed = std::from_chars(value.data(), end, requested);
    if (parsed.ec == std::errc{} && parsed.ptr == end && requested >= 1 && requested <= int(std::size(kRanks)))
      return requested;
    static constexpr int kMinutes[] = {0,   10,  25,  45,  70,   100,  140,  190,  250,  320,  400, 500,
                                       620, 760, 920, 1100, 1300, 1550, 1850, 2200, 2600, 3100, 3700};
    const double minutes = seconds / 60.0;
    int rank = 1;
    for (int i = 0; i < int(std::size(kMinutes)); ++i)
      if (minutes >= kMinutes[i]) rank = i + 1;
    return rank;
  }
  int Rank() { return Rank(settings_, std::atof(settings_.Get("match_seconds", "0").c_str())); }
  std::string RankLine() {
    const int r = Rank();
    return "Rank " + std::to_string(r) + ", " + kRanks[r - 1];
  }

  // ---- 4 to 6: the settings pages, and Settings from Home: the rows on paper, the selected row's help beside them.

  // True when B leaves it. `tabs`: Home's Settings (LB and RB switch the tab); else one setup page.
  bool FrSettings(ImDrawList* d, ImGuiIO& io, const Stage& st, float top, Input& in, bool tabs) {
    const kit::Fonts& f = kit::GetFonts();
    if (CaptureBind()) in = {};  // the bindings: the next key or mouse button goes to the slot that waits
    if (tabs && sub_ == kSubNone) {
      if (in.prev_tab) tab_ = (tab_ + kTabCount - 1) % kTabCount, row_ = 0, scroll_ = 0;
      if (in.next_tab) tab_ = (tab_ + 1) % kTabCount, row_ = 0, scroll_ = 0;
    }
    const int tab = tabs ? tab_ : (setup_tab_ == 0 ? kTabDisplay : setup_tab_ == 1 ? kTabGraphics : kTabControls);
    const std::vector<SettingRow> rows = RowsOf(tab);
    const int count = int(rows.size());
    if (count == 0) return in.back;
    if (in.up) row_ = (row_ + count - 1) % count;
    if (in.down) row_ = (row_ + 1) % count;
    row_ = std::clamp(row_, 0, count - 1);
    const ImVec2 a = st.P(40, top), b = st.P(740, 640);
    kit::Paper(d, a, b, st.u, false);
    const float rh = st(33), sh = st(30), list_top = a.y + st(4), list_bottom = b.y - st(8);
    std::vector<float> ys(size_t(count), 0.0f);
    float total = 0;
    {
      std::string section;
      for (int i = 0; i < count; ++i) {
        if (rows[size_t(i)].section != section) section = rows[size_t(i)].section, total += sh;
        ys[size_t(i)] = total;
        total += rh;
      }
    }
    const float view = list_bottom - list_top;
    if (ImGui::IsMouseHoveringRect(a, b) && io.MouseWheel != 0)
      scroll_ = std::clamp(scroll_ - io.MouseWheel * rh * 2, 0.0f, std::max(0.0f, total - view));
    if (in.up || in.down || in.prev_tab || in.next_tab) {
      if (ys[size_t(row_)] - scroll_ < sh) scroll_ = std::max(0.0f, ys[size_t(row_)] - sh);
      if (ys[size_t(row_)] + rh - scroll_ > view) scroll_ = ys[size_t(row_)] + rh - view;
    }
    d->PushClipRect(ImVec2(a.x, list_top), ImVec2(b.x, list_bottom), true);
    std::string section;
    const float x0 = a.x, x1 = b.x;
    for (int i = 0; i < count; ++i) {
      const SettingRow& it = rows[size_t(i)];
      const float y = list_top + ys[size_t(i)] - scroll_;
      if (it.section != section) {
        section = it.section;
        kit::Spaced(d, f.typed, st(13), ImVec2(x0 + st(16), y - sh + st(10)), kit::kInkSection, Upper(section), st(2));
        d->AddLine(ImVec2(x0 + st(16), y - st(2)), ImVec2(x1 - st(16), y - st(2)), kit::kRule, st(1));
      }
      if (y + rh < list_top || y > list_bottom) continue;
      const ImVec2 r0(x0, y), r1(x1, y + rh);
      const bool hover = ImGui::IsMouseHoveringRect(r0, r1) && ImGui::IsMouseHoveringRect(a, b);
      if (hover && io.MouseDelta.x + io.MouseDelta.y != 0) row_ = i;
      const bool sel = i == row_;
      if (sel) d->AddRectFilled(r0, r1, kit::kGreen);
      const ImU32 ink = sel ? IM_COL32(255, 255, 255, 255) : it.disabled ? kit::kInkFaint : kit::kInk;
      d->AddText(f.ui, st(19), ImVec2(x0 + st(16), y + st(3)), ink, it.name.c_str());
      std::string shown = it.slider ? (it.format ? it.format(it.val) : std::to_string(it.val))
                          : it.options.empty() ? it.value
                                               : it.options[size_t(std::clamp(it.index, 0, int(it.options.size()) - 1))];
      const bool changeable = !it.disabled && (it.choose || it.slider);
      if (sel && changeable) shown = "<  " + shown + "  >";
      const float vw = kit::TextWidth(f.ui, st(19), shown), vx = x1 - st(16) - vw;
      d->AddText(f.ui, st(19), ImVec2(vx, y + st(3)), sel ? IM_COL32(255, 255, 255, 255) : changeable ? kit::kInk : kit::kInkFaint,
                 shown.c_str());
      if (Pending(it)) {  // changed, taken at the next start
        const float tw = kit::SpacedWidth(f.typed, st(12), "NEXT START", st(1));
        kit::Spaced(d, f.typed, st(12), ImVec2(vx - st(14) - tw, y + st(8)), sel ? IM_COL32(255, 255, 255, 255) : kit::kRed,
                    "NEXT START", st(1));
      }
      if (it.slider) {
        const float t0 = x0 + (x1 - x0) * 0.42f, t1 = vx - st(18), ty = y + rh * 0.5f;
        if (t1 > t0) {
          const float fr = (it.val - it.lo) / std::max(1e-6f, it.hi - it.lo), fx = t0 + (t1 - t0) * fr;
          d->AddRectFilled(ImVec2(t0, ty - st(1.5f)), ImVec2(t1, ty + st(1.5f)), sel ? IM_COL32(170, 200, 130, 255) : kit::kRule);
          d->AddRectFilled(ImVec2(t0, ty - st(1.5f)), ImVec2(fx, ty + st(1.5f)), sel ? IM_COL32(255, 255, 255, 255) : kit::kInk);
          d->AddRectFilled(ImVec2(fx - st(3), ty - st(8)), ImVec2(fx + st(3), ty + st(8)), sel ? IM_COL32(255, 255, 255, 255) : kit::kInk);
        }
      }
      const bool clicked = hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
      if (!sel || it.disabled) continue;
      if (it.activate && (in.accept || clicked)) {
        it.activate();
      } else if (it.slider && (in.left || in.right)) {
        it.set(std::clamp(it.val + (in.left ? -it.step : it.step), it.lo, it.hi));
      } else if (it.slider && clicked) {  // a click on the track sets the value there
        const float t0 = x0 + (x1 - x0) * 0.42f, t1 = vx - st(18);
        const float mx = io.MousePos.x;
        if (t1 > t0 && mx >= t0 && mx <= t1) {
          const float v = it.lo + (it.hi - it.lo) * (mx - t0) / (t1 - t0);
          it.set(std::clamp(std::round(v / it.step) * it.step, it.lo, it.hi));
        }
      } else if (it.choose && !it.options.empty() && (in.left || in.right || in.accept || clicked)) {
        const int n = int(it.options.size());
        it.choose((std::clamp(it.index, 0, n - 1) + (in.left ? n - 1 : 1)) % n);
      }
    }
    d->PopClipRect();
    // The selected row's help.
    const SettingRow& it = rows[size_t(row_)];
    const ImVec2 h0 = st.P(780, top);
    const float hw = st(460), wrap = hw - st(48);
    const float hh = st(22) + st(26) + kit::WrappedHeight(f.ui, st(20), it.help, wrap) +
                     (it.note.empty() ? 0 : st(14) + kit::WrappedHeight(f.typed, st(14), it.note, wrap)) + st(26);
    {
      kit::Turn turn(d, ImVec2(h0.x + hw * 0.5f, h0.y + hh * 0.5f), 0.6f);
      kit::Paper(d, h0, ImVec2(h0.x + hw, h0.y + hh), st.u, false);
      kit::Spaced(d, f.typed, st(13), ImVec2(h0.x + st(24), h0.y + st(22)), kit::kInkSection, Upper(it.name), st(2));
      float y = h0.y + st(48);
      y += kit::Wrapped(d, f.ui, st(20), ImVec2(h0.x + st(24), y), kit::kInk, it.help, wrap);
      if (!it.note.empty()) kit::Wrapped(d, f.typed, st(14), ImVec2(h0.x + st(24), y + st(14)), kit::kRed, it.note, wrap);
    }
    if (!in.back) return false;
    if (sub_ != kSubNone) {
      row_ = sub_ == kSubRemap ? 14 : 15;
      sub_ = kSubNone;
      return false;
    }
    return true;
  }

  // The setup's Display, Graphics and Controls (King Kong's SettingsPageItems: the same rows as Settings).
  void SetupSettingsPage(ImDrawList* d, ImGuiIO& io, Input in) {
    static const char* kNames[] = {"DISPLAY", "GRAPHICS", "CONTROLS"};
    static const char* kNext[] = {"Next: Graphics, Controls, Button prompts", "Next: Controls, Button prompts",
                                  "Next: Button prompts"};
    NotePage(setup_tab_ == 0 ? "setup display" : setup_tab_ == 1 ? "setup graphics" : "setup controls");
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    Chrome(d, io, st, 4 + setup_tab_, SceneBehind() ? 0.82f : 0);
    SetupHelpers(d, st, in);
    Prompts(d, st, st.P(40, 720 - 26 - 40),
            {{kit::Button::kA, "CHANGE"}, {kit::Button::kB, "BACK"}, {kit::Button::kRB, "NEXT PAGE"}}, in, 16);
    const float w = kit::Heading(d, st(54), st.P(40, 92), kit::kCream, kNames[setup_tab_], st(2));
    d->AddText(f.typed, st(15), ImVec2(st.P(40, 0).x + w + st(18), st.P(0, 120).y), kit::kKhakiText, kNext[setup_tab_]);
    if (skip_popup_) {
      Input none;
      FrSettings(d, io, st, 160, none, false);
      SkipQuestion(d, io, st, in);
      StampCorner(d, st);
      return;
    }
    Input list_in = in;
    list_in.back = false, list_in.next_tab = list_in.prev_tab = false;
    FrSettings(d, io, st, 160, list_in, false);
    StampCorner(d, st);
    SetupKeys(in);
  }
  // Top right on the settings pages: Start Auto detect, Y Skip all, RB Next page.
  void SetupHelpers(ImDrawList* d, const Stage& st, Input& in) {
    const std::vector<Cue> helpers = {{kit::Button::kStart, "Auto detect"}, {kit::Button::kY, "Skip all"},
                                         {kit::Button::kRB, "Next page"}};
    const float w = PromptsWidth(st, helpers, 14);
    Prompts(d, st, ImVec2(st.P(1240, 0).x - w + st(10), st.P(0, 58).y), helpers, in, 14);
  }
  void SkipQuestion(ImDrawList* d, ImGuiIO& io, const Stage& st, Input& in) {
    bool yes = false;
    if (!Question(d, io, st, "SKIP THE SETTINGS?", "A badly set up game may play worse. You can change every setting later in Settings.",
                  "Cancel", "Skip all", in, yes))
      return;
    skip_popup_ = false;
    if (yes) {
      REXLOG_INFO("BF launcher: setup settings skipped");
      page_ = Page::kVoice, row_ = 0, scroll_ = 0;  // the voice chat page is never skipped: players must be told
    }
  }
  // The settings pages' buttons: Start Auto detect, Y Skip all, RB Next page (a held RB repeating within 0.35 s is
  // not another page), B one page back.
  void SetupKeys(Input& in) {
    if (in.start) {
      AutoDetect();
      page_ = Page::kVoice, row_ = 0, scroll_ = 0;  // the voice chat page is never skipped: players must be told
      return;
    }
    if (in.y) {
      skip_popup_ = true, popup_row_ = 0;
      return;
    }
    if (in.next_tab && ImGui::GetTime() >= next_page_at_) {
      next_page_at_ = ImGui::GetTime() + 0.35;
      SetupNext();
      return;
    }
    if (in.back && capture_.empty()) SetupBack();
  }

  // ---- 7: the button prompts (King Kong's page, with PlayStation 3 for the PlayStation 2)

  void PromptsPage(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("setup prompts");
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    Chrome(d, io, st, 7, SceneBehind() ? 0.82f : 0);
    SetupHelpers(d, st, in);
    Prompts(d, st, st.P(40, 720 - 26 - 40),
            {{kit::Button::kA, "CHOOSE"}, {kit::Button::kB, "BACK"}, {kit::Button::kRB, "NEXT PAGE"}}, in, 16);
    kit::Heading(d, st(54), st.P(40, 92), kit::kCream, "BUTTON PROMPTS", st(2));
    static const std::pair<const char*, const char*> kOptions[] = {
        {"auto", "Automatic"},    {"xbox360", "Xbox 360"}, {"xbox_series", "Xbox Series"},       {"ps3", "PlayStation 3"},
        {"ps5", "PlayStation 5"}, {"steam_deck", "Steam Deck"}, {"keyboard", "Keyboard and mouse"}};
    std::string cur = Cvar("bf_button_prompts");
    if (cur == "ps2") cur = "ps3";
    const int n = int(std::size(kOptions));
    if (!skip_popup_) {
      if (in.up) prompts_row_ = (prompts_row_ + n - 1) % n;
      if (in.down) prompts_row_ = (prompts_row_ + 1) % n;
    }
    const ImVec2 a = st.P(40, 160), b = st.P(560, 640);
    kit::Paper(d, a, b, st.u, false);
    kit::Spaced(d, f.typed, st(13), ImVec2(a.x + st(16), a.y + st(12)), kit::kInkSection, "CHOOSE YOUR BUTTON PROMPTS", st(2));
    int clicked = -1;
    for (int i = 0; i < n; ++i) {
      const ImVec2 r0(a.x, a.y + st(40) + float(i) * st(56)), r1(b.x, r0.y + st(52));
      const bool hover = ImGui::IsMouseHoveringRect(r0, r1);
      if (hover && io.MouseDelta.x + io.MouseDelta.y != 0) prompts_row_ = i;
      const bool sel = prompts_row_ == i, chosen = cur == kOptions[i].first;
      if (sel) d->AddRectFilled(r0, r1, kit::kGreen);
      const ImU32 ink = sel ? IM_COL32(255, 255, 255, 255) : kit::kInk;
      d->AddText(f.ui, st(22), ImVec2(r0.x + st(48), r0.y + st(6)), ink, kOptions[i].second);
      if (i == 0) {
        const std::string now = std::string("Now: ") + kit::StyleName(kit::StyleFor("auto", device_ == Device::kKeyboard));
        d->AddText(f.typed, st(13), ImVec2(r0.x + st(48), r0.y + st(34)), sel ? IM_COL32(230, 240, 220, 255) : kit::kInkFaint,
                   now.c_str());
      }
      // the chosen one's mark
      const ImVec2 m(r0.x + st(24), r0.y + st(22));
      d->AddCircle(m, st(9), ink, 20, st(2.5f));
      if (chosen) d->AddCircleFilled(m, st(5), ink, 16);
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = i;
    }
    // PREVIEW: the face buttons, the bumpers and the triggers in the chosen style.
    const ImVec2 p0 = st.P(600, 160), p1 = st.P(1240, 520);
    {
      kit::Turn turn(d, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), 0.6f);
      kit::Paper(d, p0, p1, st.u, false);
      kit::Tape(d, ImVec2((p0.x + p1.x) * 0.5f, p0.y), st(110), st(26), 2);
      kit::Spaced(d, f.typed, st(13), ImVec2(p0.x + st(24), p0.y + st(22)), kit::kInkSection, "PREVIEW", st(2));
      const kit::Style style = kit::StyleFor(kOptions[prompts_row_].first, device_ == Device::kKeyboard);
      const std::pair<kit::Button, const char*> kShown[] = {
          {kit::Button::kA, "Enter"}, {kit::Button::kB, "Esc"},  {kit::Button::kX, "F"},   {kit::Button::kY, "R"},
          {kit::Button::kLB, "Q"},    {kit::Button::kRB, "E"},   {kit::Button::kLT, "RMB"}, {kit::Button::kRT, "LMB"},
          {kit::Button::kStart, "P"}, {kit::Button::kBack, "Tab"}};
      float x = p0.x + st(24), y = p0.y + st(70);
      for (size_t i = 0; i < std::size(kShown); ++i) {
        if (i == 4 || i == 8) x = p0.x + st(24), y += st(80);
        const float gw = kit::Glyph(d, style, kShown[i].first, kShown[i].second, ImVec2(x, y + st(24)), st(48));
        x += std::max(gw, st(48)) + st(28);
      }
      d->AddText(f.typed, st(14), ImVec2(p0.x + st(24), p1.y - st(40)), kit::kInkFaint,
                 "You can change these any time in Settings, Controls.");
    }
    StampCorner(d, st);
    if (skip_popup_) {
      SkipQuestion(d, io, st, in);
      return;
    }
    if (in.accept) clicked = prompts_row_;
    if (clicked >= 0) {
      prompts_row_ = clicked;
      SetCvar("bf_button_prompts", kOptions[clicked].first);
    }
    in.accept = false;
    SetupKeys(in);
  }

  // ---- 9: all set (King Kong's finish page)

  void AllSetPage(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("setup all set");
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    Chrome(d, io, st, 9, SceneBehind() ? 0.82f : 0);
    const bool restart = RestartPending();
    Prompts(d, st, st.P(150, 720 - 30 - 42),
            {{kit::Button::kA, restart ? "Finish and restart" : "Finish setup", "", true}, {kit::Button::kX, "Join our Discord"},
             {kit::Button::kY, steam_added_ ? "Added to Steam" : "Add to Steam"}, {kit::Button::kB, "Back"}},
            in);
    const ImVec2 a = st.P(150, 96), b = st.P(1130, 520);
    int clicked = -1;
    {
      kit::Turn turn(d, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), 0.4f);
      kit::Paper(d, a, b, st.u);
      kit::Tape(d, ImVec2(a.x + st(110), a.y + st(4)), st(110), st(26), -4);
      kit::Tape(d, ImVec2(b.x - st(110), a.y + st(4)), st(110), st(26), 3);
      const float x = a.x + st(44), w = st(892);
      float y = a.y + st(40);
      kit::Spaced(d, f.typed, st(15), ImVec2(x, y), kit::kInkFaint, "FIRST TIME SETUP \xC2\xB7 STEP 8 OF 8", st(2));
      y += st(24);
      kit::Heading(d, st(62), ImVec2(x, y), kit::kInk, "YOU'RE ALL SET, " + Upper(settings_.Get("player_name", "SOLDIER")),
                   st(2), w);
      y += st(76);
      y += kit::Wrapped(d, f.ui, st(20), ImVec2(x, y), kit::kInkSoft,
                        "Thanks for installing Battlefield 1943 Reborn. This is an early playtest: things will change, "
                        "and some will break. Tell us what you find with Report an issue on Home. Every setting you "
                        "chose is in Settings, any time.",
                        w) +
           st(restart ? 10 : 30);
      if (restart)
        y += kit::Wrapped(d, f.typed, st(15), ImVec2(x, y), kit::kRed,
                          "Some of your graphics settings take effect when the game starts, so it starts again when "
                          "you finish (a few seconds).",
                          w) +
             st(18);
      // Discord and Add to Steam, as King Kong's finish page.
      const float cw = (w - st(22)) * 0.5f, ch = st(110);
      for (int i = 0; i < 2; ++i) {
        const ImVec2 c0(x + float(i) * (cw + st(22)), y), c1(c0.x + cw, y + ch);
        const bool hover = ImGui::IsMouseHoveringRect(c0, c1);
        d->AddRectFilled(c0, c1, hover ? kit::kPaper : kit::kPaperCard);
        d->AddRect(c0, c1, kit::kBorder, 0, 0, st(2));
        DrawCardIcon(d, st, i, ImVec2(c0.x + st(20), c0.y + st(24)));
        d->AddText(f.ui_bold, st(24), ImVec2(c0.x + st(84), c0.y + st(18)), kit::kInk,
                   i == 0 ? "Join our Discord" : steam_added_ ? "Added to Steam" : "Add to Steam");
        kit::Wrapped(d, f.typed, st(14), ImVec2(c0.x + st(84), c0.y + st(54)), kit::kInkFaint,
                     i == 0 ? "News, help and the playtest's bug reports."
                            : steam_added_ ? "Battlefield 1943 Reborn is in your Steam library."
                                           : "Play it from your Steam library, with its artwork.",
                     cw - st(100));
        if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = i;
      }
    }
    StampCorner(d, st);
    CheckSteamOnce();
    if (SteamDialog(d, io, st, in)) return;
    if (in.x || clicked == 0) OpenUrl(kDiscordUrl);
    if (in.y || clicked == 1) AddToSteam();
    if (in.accept) SetupNext();
    else if (in.back) SetupBack();
  }
  // The Discord bubble and the Steam picture of the Home cards (drawn; no logos).
  // The real Discord (0) and Steam (1) marks, white on their own colours as King Kong's launcher draws them (TekRantGaming, 11
  // Nov 2026): Discord's blurple, Steam's dark blue (King Kong's BrandIcon colours).
  void DrawCardIcon(ImDrawList* d, const Stage& st, int which, ImVec2 at) {
    const ImVec2 a(at.x + st(2), at.y + st(1)), b(a.x + st(44), a.y + st(44));
    d->AddRectFilled(a, b, which == 0 ? IM_COL32(88, 101, 242, 255) : IM_COL32(27, 40, 56, 255), st(8));
    const ImVec2 m0(a.x + st(7), a.y + st(7)), m1(b.x - st(7), b.y - st(7));
    if (which == 0) kit::DrawMask(d, "discord", kDiscordIcon, kDiscordIconSize, m0, m1, IM_COL32(255, 255, 255, 255));
    else kit::DrawMask(d, "steam", kSteamIcon, kSteamIconSize, m0, m1, IM_COL32(255, 255, 255, 255));
  }

  // ---- Home (every start once the setup is done)

  // Server browser (TekRantGaming, 10 Nov 2026: a server picked here joins straight after the game's hidden sign in, as King
  // Kong's chapter select starts its chapter): Join reserves it and starts the game (Join, pending_join_).
  static constexpr const char* kHomeRows[] = {"PLAY", "SERVER BROWSER", "UNLOCKABLES", "SETTINGS", "UPDATE",
                                              "REPORT AN ISSUE", "MORE FROM TRG STUDIOS", "QUIT"};
  void HomePage(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("home");
    UpdateTick();
    StartProjectArt();  // More from TRG Studios' pictures, on a worker, once a run (projects_art.cpp)
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    const ImVec2 sz = io.DisplaySize;
    // The new look (TekRantGaming, 10 Nov 2026; docs/MENUS-PLAN.md section 6): the maps from the air behind the main menu's
    // column, as the main menu and the pause.
    MenuBackdrop(d, sz, StageOf(io));  // the new look's backdrop
    const float cx0 = st.P(40, 0).x, cx1 = cx0 + st(400);
    d->AddRectFilled(ImVec2(0, 0), ImVec2(cx1, sz.y), IM_COL32(15, 17, 13, 226));
    d->AddRectFilled(ImVec2(cx1, 0), ImVec2(cx1 + st(1.4f), sz.y), IM_COL32(201, 183, 122, 110));
    // Top right: the players and servers online (the backend's list, every 5 s).
    {
      const auto servers = list_.servers();
      const ServerNotice notice = list_.notice();
      const bool up = list_.reachable() && !notice.active();
      const std::string t = notice.active() ? "SERVERS CLOSED \xC2\xB7 " + Upper(notice.title)
                            : up ? fmt::format("{} PLAYERS  /  {} {}", list_.players_online(), servers.size(),
                                             servers.size() == 1 ? "SERVER ONLINE" : "SERVERS ONLINE")
                        : !list_.asked() ? "CONNECTING"
                               : "SERVERS NOT REACHABLE";
      const float tw = kit::SpacedWidth(f.typed, st(13), t, st(2));
      const float tx = st.P(1240, 0).x - tw, ty = st.P(0, 96).y;  // under the soldier's card
      d->AddCircleFilled(ImVec2(tx - st(14), ty + st(8)), st(4), up ? kit::kGreenBright : kit::kStamp);
      kit::Spaced(d, f.typed, st(13), ImVec2(tx, ty), kit::kKhakiText, t, st(2));
    }
    // Under it: the flying notice (TekRantGaming, 11 Nov 2026), as on the main menu (a server picked here skips that menu).
    FlyingNotice(d, ImVec2(st.P(1240, 0).x, st.P(0, 130).y), st(420), st.u * 720.0f / 1080.0f);
    // Top right: the soldier name, the rank badge and Edit.
    bool edit = false;
    {
      const std::string name = settings_.Get("player_name", "Soldier");
      const std::string rank = "RANK " + std::to_string(Rank()) + " \xC2\xB7 " + Upper(kRanks[Rank() - 1]);
      const std::string tag = clan::OwnTag();  // [TAG]Name (clan_tags.h)
      const float nw = std::max(clan::TaggedWidth(f.ui_bold, st(22), name, tag), kit::SpacedWidth(f.typed, st(13), rank, 0));
      const std::vector<Cue> e = {{kit::Button::kBack, "Edit"}};
      const float ew = PromptsWidth(st, e, 14);
      const float w = st(12) + st(44) + st(14) + nw + st(18) + ew + st(10);
      const ImVec2 a(st.P(1240, 0).x - w, st.P(0, 24).y), b(st.P(1240, 0).x, st.P(0, 84).y);
      d->AddRectFilled(a, b, IM_COL32(15, 17, 13, 214));
      d->AddRectFilled(a, ImVec2(a.x + st(4), b.y), kit::kStamp);
      RankBadge(d, st, ImVec2(a.x + st(14) + st(22), a.y + st(30)), Rank());
      clan::DrawTagged(d, f.ui_bold, st(22), ImVec2(a.x + st(72), a.y + st(7)), kit::kCream, name, tag, kit::kGold);
      d->AddText(f.typed, st(13), ImVec2(a.x + st(72), a.y + st(38)), kit::kKhakiText, rank.c_str());
      Input edit_in;
      Prompts(d, st, ImVec2(b.x - ew - st(8), a.y + st(18)), e, edit_in, 14);
      edit = edit_in.view;
    }
    // The column: the brand, BATTLEFIELD 1943 stencilled, REBORN in gold, a red rule.
    kit::Spaced(d, f.typed, st(12), st.P(40, 46), kit::kKhakiDim, "PACIFIC THEATRE, 1943", st(3));
    Text(d, f.stencil, st(40), ImVec2(cx0 - st(2), st.P(0, 66).y), kit::kCream, "BATTLEFIELD 1943");
    kit::Spaced(d, f.stencil, st(26), st.P(40, 120), kit::kGold, "REBORN", st(12));
    d->AddRectFilled(st.P(40, 160), st.P(80, 164), kit::kStamp);
    // The rows: PLAY first and larger, then the others; numbered, the chosen one with the red tab.
    const int n = int(std::size(kHomeRows));
    if (!quit_popup_) {
      if (in.up) home_row_ = (home_row_ + n - 1) % n;
      if (in.down) home_row_ = (home_row_ + 1) % n;
    }
    int clicked = -1;
    float y = st.P(0, 188).y;
    for (int i = 0; i < n; ++i) {
      const bool sel = home_row_ == i;
      const float h = i == 0 ? st(58) : st(44);
      const ImVec2 a(cx0 - st(18), y), b(cx1, y + h);
      const bool hover = ImGui::IsMouseHoveringRect(a, b);
      if (hover && io.MouseDelta.x + io.MouseDelta.y != 0 && !quit_popup_) home_row_ = i;
      if (sel) {
        d->AddRectFilled(a, b, IM_COL32(201, 183, 122, 26));
        d->AddRectFilled(a, ImVec2(a.x + st(5), b.y), kit::kStamp);
      }
      d->AddRectFilled(ImVec2(cx0, b.y - st(0.7f)), ImVec2(cx1 - st(26), b.y), IM_COL32(201, 183, 122, 30));
      const bool locked = i == 2;
      kit::Spaced(d, f.typed, st(10), ImVec2(cx0, a.y + h * 0.5f - st(6)), sel ? kit::kStamp : kit::kKhakiDim,
                  fmt::format("{:02d}", i + 1), st(1.5f));
      if (i == 0) {
        const float gw = kit::Glyph(d, PromptStyle(), kit::Button::kA, "Enter", ImVec2(cx0 + st(32), a.y + h * 0.5f), st(26));
        kit::Spaced(d, f.ui_bold, st(32), ImVec2(cx0 + st(32) + gw + st(12), a.y + st(9)), kit::kCream, "PLAY", st(3));
      } else {
        kit::Spaced(d, f.ui_bold, st(21), ImVec2(cx0 + st(32), a.y + st(10)),
                    locked ? kit::kKhakiDim : sel ? kit::kCream : kit::kKhakiText, kHomeRows[i], st(2));
      }
      std::string note;
      if (locked) note = "COMING SOON";
      if (i == 4) note = UpdateNote();
      if (!note.empty())
        d->AddText(f.typed, st(11), ImVec2(cx1 - st(26) - kit::TextWidth(f.typed, st(11), note), a.y + h * 0.5f - st(6)),
                   locked ? kit::kKhakiDim : kit::kGreenBright, note.c_str());
      y += h;
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !quit_popup_) clicked = i;
    }
    // Bottom right: Join our Discord (X) and Add to Steam (Y).
    int card = -1;
    for (int i = 0; i < 2; ++i) {
      const ImVec2 a = st.P(1240 - 2 * 262 + float(i) * 262, 720 - 74 - 74), b(a.x + st(250), a.y + st(58));
      const bool hover = ImGui::IsMouseHoveringRect(a, b);
      d->AddRectFilled(a, b, IM_COL32(15, 17, 13, hover ? 236 : 206));
      d->AddRectFilled(a, ImVec2(a.x + st(3), b.y), hover ? kit::kStamp : IM_COL32(201, 183, 122, 120));
      DrawCardIcon(d, st, i, ImVec2(a.x + st(12), a.y + st(7)));
      d->AddText(f.ui, st(19), ImVec2(a.x + st(66), a.y + st(5)), kit::kCream,
                 i == 0 ? "Join our Discord" : steam_added_ ? "Added to Steam" : "Add to Steam");
      const kit::Button gb = i == 0 ? kit::Button::kX : kit::Button::kY;
      const float gw = kit::Glyph(d, PromptStyle(), gb, KeyFor(gb), ImVec2(a.x + st(66), a.y + st(41)), st(17));
      d->AddText(f.typed, st(12), ImVec2(a.x + st(72) + gw, a.y + st(34)), kit::kKhakiText,
                 i == 0 ? "discord.gg/8eakYSvBZS" : steam_added_ ? "In your library" : "Shortcut, art, controller");
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !quit_popup_) card = i;
    }
    Prompts(d, st, st.P(40, 720 - 24 - 34), {{kit::Button::kA, "SELECT"}, {kit::Button::kB, "QUIT"}}, in, 15);
    if (kit::WatermarkOn()) kit::Stamp(d, st.P(1240 - 130, 720 - 26 - 34), st.u, -6, 1.05f);
    CheckSteamOnce();
    if (SteamDialog(d, io, st, in)) return;
    if (quit_popup_) {
      bool yes = false;
      if (Question(d, io, st, "QUIT THE GAME?", "Battlefield 1943 Reborn closes. Your settings are saved.", "Cancel", "Quit",
                   in, yes)) {
        quit_popup_ = false;
        REXLOG_INFO("BF launcher: quit {}", yes ? "chosen" : "cancelled");
        if (yes && quit_) quit_();
      }
      return;
    }
    if (edit) return BeginAccount(false);
    if (in.view) return BeginAccount(false);
    if (in.x || card == 0) OpenUrl(kDiscordUrl);
    if (in.y || card == 1) AddToSteam();
    if (in.back) {
      quit_popup_ = true, popup_row_ = 0;
      return;
    }
    if (in.accept) clicked = home_row_;
    if (clicked < 0) return;
    home_row_ = clicked;
    REXLOG_INFO("BF launcher: home {}", kHomeRows[clicked]);
    switch (clicked) {
      case 0: return Play();
      case 1: return OpenBrowser();
      case 2: page_ = Page::kUnlockables; return;
      case 3: page_ = Page::kSettings, from_home_ = true, tab_ = 0, row_ = 0, scroll_ = 0, sub_ = kSubNone; return;
      case 4: return UpdateNow();
      case 5: page_ = Page::kReport; return;
      case 6: page_ = Page::kProjects, project_ = 0; return;
      case 7: quit_popup_ = true, popup_row_ = 0; return;
      default: return;
    }
  }
  // Stats & Awards, ours (in place of the game's screen, docs/MENUS-PLAN.md): the soldier's rank and time in matches,
  // and every kind of score event the player has had (auto_join.cpp counts them from the game's own HUD events; the
  // settings file keeps them as stat.<label> = events,points).
  Settings test_profile_;
  bool test_profile_active_ = false;

  // Dev builds only (the review of pull request 16): a player's game never shows a made up rank or awards.
  void LoadTestProfile() {
#if defined(BF_DEV_TOOLS)
    test_profile_ = Settings{settings_.path.parent_path() / "bf1943-test-profile.txt", {}};
    test_profile_.Load();
    test_profile_active_ = test_profile_.Get("enabled", "false") == "true";
#endif
  }

  // Stats & Awards, Leaderboards and Achievements: game_menu_progress.inc (his picks S1, L1, M1 of 16 Nov 2026).
  Input in_copy_;
  int ach_scroll_ = 0;
  // Leaderboards, ours (in place of the game's EA screen, docs/MENUS-PLAN.md): every player's own totals as their
  // games send them to our backend (stats_put after each save of Stats & Awards), ranked by score; asked every 15 s
  // on a worker while the page is open.
  struct LbRow {
    std::string name;
    std::string tag;  // the clan tag the backend says (clan_tags.h)
    int score = 0, seconds = 0, captures = 0;
  };
  double lb_asked_at_ = -100;  // below 0: the board asked again at once (game_menu_progress.inc)
  bool lb_from_pause_ = false;  // opened from the pause: B goes back there

  // Posted at the start (the totals saved in the profile, which before went out only with the next score event in a
  // match) and after each save of new score events. Tried up to four times (now, then 5, 15 and 30 s later) and each
  // outcome logged, as nothing showed whether a post had arrived.
  bool stats_sent_at_start_ = false;
  void SendLeaderboard(const char* why = "new score events") {
    double total = 0;
    int captures = 0;
    for (const auto& [key, value] : settings_.values) {
      if (key.rfind("stat.", 0) != 0) continue;
      const size_t comma = value.find(',');
      if (comma != std::string::npos) total += std::atof(value.c_str() + comma + 1);
      if (key == "stat.CQ_StringForFlagCapture") captures = std::atoi(value.c_str());
    }
    std::string name;
    for (char c : settings_.Get("player_name", ""))
      if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_') name += c;
    if (name.empty()) return;
    const bool test = settings_.Get("test_progression", "") == "1";
    reborn_achievements::Ensure();  // the awards below with Reborn's own
    std::string req = fmt::format(
        "{{\"op\": \"stats_put\", \"name\": \"{}\", \"score\": {:.0f}, \"seconds\": {:.0f}, \"captures\": {}", name,
        total, std::atof(settings_.Get("match_seconds", "0").c_str()), captures);
    if (test) {
      req += fmt::format(", \"rank\": {}, \"achievements\": [", Rank());
      bool first = true;
      if (auto* k = REX_KERNEL_STATE()) {
        for (const auto& a : k->achievements().ListAchievements()) {
          if (!k->achievements().IsUnlocked(a.id)) continue;
          req += (first ? "" : ",") + std::to_string(a.id);
          first = false;
        }
      }
      req += "]";
    }
    req += "}";
    const double seconds = std::atof(settings_.Get("match_seconds", "0").c_str());
    // A protected name (Protect your name): its totals go signed, with each stat and the awards, to its account too.
    std::string code, fields;
    if (!test && NameProtected()) {
      code = settings_.Get("account_code", "");
      std::string stats, awards;
      for (const auto& [key, value] : settings_.values) {
        if (key.rfind("stat.", 0) != 0 || key.size() > 5 + 48) continue;
        const std::string label = key.substr(5);
        if (label.empty() || !std::all_of(label.begin(), label.end(), [](char c) {
              return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
            }))
          continue;
        const size_t comma = value.find(',');
        const long long n = std::atoll(value.c_str()),
                        pts = comma == std::string::npos ? 0 : std::llround(std::atof(value.c_str() + comma + 1));
        stats += fmt::format("{}{}={},{}", stats.empty() ? "" : ";", label, std::max(0LL, n), pts);
      }
      if (auto* k = REX_KERNEL_STATE()) {
        for (const auto& a : k->achievements().ListAchievements())
          if (k->achievements().IsUnlocked(a.id)) awards += (awards.empty() ? "" : ",") + std::to_string(a.id);
      }
      fields = fmt::format("{}|{:.0f}|{:.0f}|{}|{}|{}", settings_.Get("account_name", name), total, seconds, captures,
                           stats, awards);
    }
    std::thread([req, test, name, total, seconds, why, code, fields] {
      static constexpr int kWait[] = {0, 5, 15, 30};
      for (int attempt = 0; attempt < int(std::size(kWait)); ++attempt) {
        if (kWait[attempt]) std::this_thread::sleep_for(std::chrono::seconds(kWait[attempt]));
        std::string reply;
        // A test profile's made-up rank and awards go to a backend on this PC only.
        const bool local = std::string(REXCVAR_GET(bf_backend_registry)).rfind("127.", 0) == 0;
        const bool answered = code.empty() ? (!test || local) && RegistryRequest(req, reply)
                                           : SignedRegistryRequest(code, "stats_put", fields, reply);
        const bool ok = answered && reply.find("\"ok\": true") != std::string::npos;
        if (ok) {
          REXLOG_INFO("BF leaderboard: stats posted for {} ({}: score {:.0f}, {:.0f} s played{})", name, why, total,
                      seconds, test ? ", test profile, local backend only" : code.empty() ? "" : ", signed, to the account");
          return;
        }
        if (answered) {  // the backend refused it: sending the same again will not help
          REXLOG_WARN("BF leaderboard: stats for {} refused by the backend ({}): {}", name, why,
                      reply.substr(0, 200));
          return;
        }
        REXLOG_WARN("BF leaderboard: no answer from the backend posting stats for {} ({}), try {} of {}", name, why,
                    attempt + 1, std::size(kWait));
      }
    }).detach();
  }

  // Join Match With Friends, ours (in place of the game's Xbox friends screen, docs/MENUS-PLAN.md): the players signed
  // in now, from our backend, with the server each plays on (its GameManager host reports who joined); A joins that
  // server as the browser does. Asked every 5 s on a worker while the page is open.
  struct FriendRow {
    std::string name, gid, server, map;
  };
  std::mutex fr_mutex_;
  std::vector<FriendRow> fr_rows_;
  bool fr_ok_ = false, fr_answered_ = false;
  double fr_asked_at_ = -100;
  std::atomic<bool> fr_busy_{false};
  int fr_row_ = 0;

  void FriendsPage(ImDrawList* d, ImGuiIO& io, const Input& in) {
    const ImVec2 sz = io.DisplaySize;
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    if (ImGui::GetTime() - fr_asked_at_ > 5 && !fr_busy_.load()) {
      fr_asked_at_ = ImGui::GetTime();
      fr_busy_ = true;
      std::thread([this] {
        std::string reply;
        const bool ok = RegistryRequest("{\"op\": \"players\"}", reply);
        std::vector<FriendRow> rows;
        for (size_t at = ok ? reply.find("\"rows\"") : std::string::npos; at != std::string::npos && rows.size() < 200;) {
          const size_t a = reply.find('{', at), b = a == std::string::npos ? a : reply.find('}', a);
          if (a == std::string::npos || b == std::string::npos) break;
          const std::string obj = reply.substr(a, b - a + 1);
          FriendRow r{JsonField(obj, "name"), JsonField(obj, "gid"), JsonField(obj, "server"), JsonField(obj, "map")};
          if (!r.name.empty()) clan::Note(r.name, JsonField(obj, "tag")), rows.push_back(r);
          at = b + 1;
        }
        std::lock_guard lock(fr_mutex_);
        fr_rows_ = std::move(rows), fr_ok_ = ok, fr_answered_ = true;
        fr_busy_ = false;
      }).detach();
    }
    MenuBackdrop(d, sz, StageOf(io));  // the new look's backdrop
    d->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(12, 14, 11, 186));
    kit::Spaced(d, f.typed, st(12), st.P(40, 46), kit::kKhakiDim, "JOIN MATCH WITH FRIENDS", st(3));
    Text(d, f.stencil, st(40), ImVec2(st.P(40, 0).x - st(2), st.P(0, 66).y), kit::kCream, "PLAYERS ONLINE");
    d->AddRectFilled(st.P(40, 120), st.P(80, 124), kit::kStamp);
    std::vector<FriendRow> rows;
    bool ok = false, answered = false;
    {
      std::lock_guard lock(fr_mutex_);
      rows = fr_rows_, ok = fr_ok_, answered = fr_answered_;
    }
    const std::string mine = settings_.Get("player_name", "");
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const FriendRow& r) { return r.name == mine; }), rows.end());
    const int n = int(rows.size());
    if (n) {
      if (in.up) fr_row_ = (fr_row_ + n - 1) % n;
      if (in.down) fr_row_ = (fr_row_ + 1) % n;
    }
    fr_row_ = std::clamp(fr_row_, 0, std::max(0, n - 1));
    kit::Spaced(d, f.typed, st(14), st.P(40, 140), kit::kKhakiText,
                !answered ? "ASKING THE SERVERS..." : !ok ? "THE SERVERS ARE NOT REACHABLE"
                : n == 0 ? "NOBODY ELSE IS SIGNED IN RIGHT NOW" : fmt::format("{} {} SIGNED IN", n, n == 1 ? "PLAYER" : "PLAYERS"),
                st(2));
    const float x0 = st.P(40, 0).x, x1 = st.P(1240, 0).x;
    float y = st.P(0, 176).y;
    int clicked = -1;
    for (int i = 0; i < n && y < st.P(0, 640).y; ++i) {
      const FriendRow& r = rows[size_t(i)];
      const bool sel = i == fr_row_;
      const ImVec2 a(x0, y), b(x1, y + st(48));
      const bool hover = ImGui::IsMouseHoveringRect(a, b);
      if (hover && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) fr_row_ = i;
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = i;
      d->AddRectFilled(a, b, IM_COL32(15, 17, 13, sel ? 236 : 200));
      if (sel) d->AddRectFilled(a, ImVec2(a.x + st(4), b.y), kit::kStamp);
      clan::DrawTagged(d, f.ui_bold, st(21), ImVec2(a.x + st(24), a.y + st(10)), sel ? kit::kCream : kit::kKhakiText, r.name,
                       clan::TagOf(r.name), kit::kGold);
      const std::string where = r.gid.empty() ? (r.server.empty() ? "IN THE MENUS" : Upper(r.server))  // a private match
                                              : Upper(r.server) + (r.map.empty() ? "" : "  /  " + Upper(r.map));
      d->AddText(f.typed, st(15), ImVec2(b.x - st(24) - kit::TextWidth(f.typed, st(15), where), a.y + st(15)),
                 r.gid.empty() ? kit::kKhakiDim : kit::kKhakiText, where.c_str());
      y += st(54);
    }
    Input cue = in;
    Prompts(d, st, st.P(40, 720 - 24 - 34), {{kit::Button::kA, "JOIN"}, {kit::Button::kB, "BACK"}}, cue, 15);
    if ((in.accept || cue.accept) && n) clicked = fr_row_;  // the strips' clicks too (issue 19)
    if (clicked >= 0 && clicked < n && !rows[size_t(clicked)].gid.empty()) {
      for (const auto& sv : list_.servers())
        if (sv.gid == rows[size_t(clicked)].gid) {
          REXLOG_INFO("BF friends: joining {} on {}", rows[size_t(clicked)].name, sv.name);
          ping_accepted_address_ = sv.address;  // to play with a friend: no far server question (FarServerWarned)
          Join(sv);
          return;
        }
    }
    if (in.back || cue.back) BackToMenu(kRowBrowser, 2);  // back to the menu (no Join friends row since 12 Nov 2026)
  }
  // The pause's Scoreboard, ours (in place of the game's, docs/MENUS-PLAN.md): every player on this server by score,
  // as their games send it to our backend (op match_score from MatchScoreTick), asked every 3 s on a worker.
  std::mutex sb_mutex_;
  std::vector<LbRow> sb_rows_;
  bool sb_ok_ = false, sb_answered_ = false;
  double sb_asked_at_ = -100;
  std::atomic<bool> sb_busy_{false};
  int sb_row_ = 0;  // the row cursor (the voice mute)
  int sb_scroll_ = 0;  // the first row shown (a 32 or 64 player server has more rows than the page holds)

  void ScoreboardPage(ImDrawList* d, ImGuiIO& io, const Input& in) {
    const ImVec2 sz = io.DisplaySize;
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    if (ImGui::GetTime() - sb_asked_at_ > 3 && !sb_busy_.load()) {
      sb_asked_at_ = ImGui::GetTime();
      sb_busy_ = true;
      const std::string req = fmt::format("{{\"op\": \"scoreboard\", \"gid\": \"{}\"}}", joined_gid_);
      std::thread([this, req] {
        std::string reply;
        const bool ok = RegistryRequest(req, reply);
        std::vector<LbRow> rows;
        for (size_t at = ok ? reply.find("\"rows\"") : std::string::npos; at != std::string::npos && rows.size() < size_t(bf1943::kPlayerListLimit);) {
          const size_t a = reply.find('{', at), b = a == std::string::npos ? a : reply.find('}', a);
          if (a == std::string::npos || b == std::string::npos) break;
          const std::string obj = reply.substr(a, b - a + 1);
          LbRow r;
          r.name = JsonField(obj, "name");
          r.score = std::atoi(JsonField(obj, "score").c_str());
          r.tag = JsonField(obj, "tag");
          if (!r.name.empty()) clan::Note(r.name, r.tag), r.tag = clan::TagOf(r.name), rows.push_back(r);
          at = b + 1;
        }
        std::lock_guard lock(sb_mutex_);
        sb_rows_ = std::move(rows), sb_ok_ = ok, sb_answered_ = true;
        sb_busy_ = false;
      }).detach();
    }
    // Over the match, darkened (as the pause).
    d->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(8, 9, 7, 214));
    kit::Spaced(d, f.typed, st(12), st.P(40, 46), kit::kKhakiDim, Upper(joining_.empty() ? "THIS SERVER" : joining_), st(3));
    Text(d, f.stencil, st(40), ImVec2(st.P(40, 0).x - st(2), st.P(0, 66).y), kit::kCream, "SCOREBOARD");
    d->AddRectFilled(st.P(40, 120), st.P(80, 124), kit::kStamp);
    std::vector<LbRow> rows;
    bool ok = false, answered = false;
    {
      std::lock_guard lock(sb_mutex_);
      rows = sb_rows_, ok = sb_ok_, answered = sb_answered_;
    }
    kit::Spaced(d, f.typed, st(14), st.P(40, 140), kit::kKhakiText,
                !answered ? "ASKING THE SERVER..." : !ok ? "THE SERVERS ARE NOT REACHABLE"
                : fmt::format("{} {}", rows.size(), rows.size() == 1 ? "PLAYER" : "PLAYERS"), st(2));
    const std::string mine = settings_.Get("player_name", "");
    // A row cursor and each player's voice (voice_mute.h): a speaker lit while that player is heard, struck through
    // once muted; X (the keyboard's F), the prompt's strip or a click on the speaker mutes or unmutes the player under
    // the cursor. The player's own row has no mute; its speaker lights while their own voice goes out.
    const int n = int(rows.size());
    if (n) {
      if (in.up) sb_row_ = (sb_row_ + n - 1) % n;
      if (in.down) sb_row_ = (sb_row_ + 1) % n;
    }
    sb_row_ = std::clamp(sb_row_, 0, std::max(0, n - 1));
    const float x0 = st.P(40, 0).x, x1 = st.P(1240, 0).x;
    float y = st.P(0, 176).y;
    // The rows the page holds (10); more scroll (server sizes, docs/SERVER-SIZES.md): the cursor kept in view, the
    // wheel over the list moves it.
    const int shown = std::max(1, int((st.P(0, 640).y - y) / st(44)));
    if (n && io.MouseWheel != 0 && ImGui::IsMouseHoveringRect(ImVec2(x0, y), ImVec2(x1, st.P(0, 640).y)))
      sb_row_ = std::clamp(sb_row_ - int(io.MouseWheel * 3), 0, n - 1);
    if (sb_row_ < sb_scroll_) sb_scroll_ = sb_row_;
    if (sb_row_ >= sb_scroll_ + shown) sb_scroll_ = sb_row_ - shown + 1;
    sb_scroll_ = std::clamp(sb_scroll_, 0, std::max(0, n - shown));
    int toggled = -1;
    // The columns' words over the score and the ping; the ping furthest right (TekRantGaming, 20 Nov 2026), the score
    // before it.
    const bool show_ping = REXCVAR_GET(bf_scoreboard_ping);
    const float score_right = show_ping ? x1 - st(24) - ping::CellWidth(st(20)) - st(40) : x1 - st(24);
    if (n) {
      const float pc = x1 - st(24) - ping::CellWidth(st(20)) * 0.5f;
      if (show_ping)
        kit::Spaced(d, f.typed, st(11), ImVec2(pc - kit::SpacedWidth(f.typed, st(11), "PING", st(2)) * 0.5f, st.P(0, 156).y),
                    kit::kKhakiDim, "PING", st(2));
      kit::Spaced(d, f.typed, st(11), ImVec2(score_right - kit::SpacedWidth(f.typed, st(11), "SCORE", st(2)), st.P(0, 156).y),
                  kit::kKhakiDim, "SCORE", st(2));
    }
    const double now = ImGui::GetTime();
    if (n > shown) {  // which rows these are, by the column words
      const std::string range = fmt::format("{}-{} OF {}", sb_scroll_ + 1, std::min(n, sb_scroll_ + shown), n);
      kit::Spaced(d, f.typed, st(11), ImVec2(x0 + st(24), st.P(0, 156).y), kit::kKhakiDim, range, st(2));
    }
    for (int i = sb_scroll_; i < n && i < sb_scroll_ + shown; ++i) {
      const LbRow& r = rows[size_t(i)];
      const bool me = !mine.empty() && r.name == mine;
      const bool sel = i == sb_row_;
      const ImVec2 a(x0, y), b(x1, y + st(40));
      const bool hover = ImGui::IsMouseHoveringRect(a, b);
      if (hover && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) sb_row_ = i;
      d->AddRectFilled(a, b, IM_COL32(15, 17, 13, me || sel ? 236 : 200));
      if (sel) d->AddRectFilled(a, ImVec2(a.x + st(4), b.y), kit::kStamp);
      const ImU32 ink = me || sel ? kit::kCream : kit::kKhakiText;
      d->AddText(f.typed, st(15), ImVec2(x0 + st(24), y + st(11)), ink, std::to_string(i + 1).c_str());
      // The speaker between the place and the name.
      const ImVec2 icon(x0 + st(68), y + st(20));
      const mute::Voice voice = me ? (comms::Transmitting() ? mute::Voice::kTalking : mute::Voice::kQuiet)
                                   : mute::VoiceOf(r.name);
      const bool icon_hover = !me && ImGui::IsMouseHoveringRect(ImVec2(icon.x - st(16), a.y), ImVec2(icon.x + st(16), b.y));
      if (icon_hover) d->AddRectFilled(ImVec2(icon.x - st(15), a.y + st(5)), ImVec2(icon.x + st(15), b.y - st(5)),
                                       IM_COL32(207, 198, 168, 40), st(3));
      mute::DrawIcon(d, icon, st(24), voice, ink, IM_COL32(136, 208, 96, 255), kit::kStamp, now);
      if (icon_hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) toggled = i;
      clan::DrawTagged(d, f.ui_bold, st(19), ImVec2(x0 + st(98), y + st(8)), ink, r.name, r.tag, kit::kGold);
      if (!me && voice == mute::Voice::kMuted) {
        const float nx = x0 + st(98) + clan::TaggedWidth(f.ui_bold, st(19), r.name, r.tag) + st(14);
        kit::Spaced(d, f.typed, st(12), ImVec2(nx, y + st(14)), kit::kStamp, "MUTED", st(2));
      }
      const std::string sc = std::to_string(r.score);
      d->AddText(f.typed, st(16), ImVec2(score_right - kit::TextWidth(f.typed, st(16), sc), y + st(10)), ink, sc.c_str());
      // The player's ping (scoreboard_ping.h): bars and ms at the right edge; BOT for a bot; nothing when not known.
      if (show_ping) {
        const bool bot = world::IsBot(r.name);
        ping::DrawCell(d, f.ui_bold, x1 - st(24), y + st(20), st(20), bot ? -1 : world::PingOf(r.name), bot, false, ink);
      }
      y += st(44);
    }
    const bool can_mute = n && !(rows[size_t(sb_row_)].name == mine && !mine.empty());
    const bool muted_now = can_mute && comms::Muted(rows[size_t(sb_row_)].name);
    Input cue = in;
    std::vector<Cue> cues;
    if (can_mute) cues.push_back({kit::Button::kX, muted_now ? "UNMUTE" : "MUTE"});
    cues.push_back({kit::Button::kB, "BACK"});
    Prompts(d, st, st.P(40, 720 - 24 - 34), cues, cue, 15);
    if (can_mute && (in.x || cue.x)) toggled = sb_row_;
    if (toggled >= 0 && toggled < n && !(rows[size_t(toggled)].name == mine && !mine.empty())) {
      const bool m = comms::ToggleMuted(rows[size_t(toggled)].name);
      REXLOG_INFO("BF scoreboard: {} {} from the pause's Scoreboard", rows[size_t(toggled)].name, m ? "muted" : "unmuted");
    }
    if (in.back || cue.back) page_ = Page::kPause, pause_row_ = kPauseScoreboard;  // the strip's click too (issue 19)
  }
  // ---- Create Private Match, ours (in place of the game's console hosted match, docs/MENUS-PLAN.md section 7): our own
  // dedicated server on this PC (private_match.cpp) with our GameManager host inside it (gm_host.cpp), not listed in
  // the server browser. This game joins it once its host has registered it, as the browser joins a server; friends
  // join it with Direct connect and the address the pause copies. It ends when the player leaves the match (back on
  // our main menu) or quits the game.
  static constexpr const char* kPmMapKeys[] = {"wake", "guadalcanal", "iwo", "coral"};
  static constexpr const char* kPmMapNames[] = {"WAKE ISLAND", "GUADALCANAL", "IWO JIMA", "CORAL SEA", "ALL FOUR IN TURN"};
  static constexpr const char* kPmMapTitles[] = {"Wake Island", "Guadalcanal", "Iwo Jima", "Coral Sea"};
  // 13 Nov 2026: Network type (Online, or LAN: the match's own backend, this network's players, no internet).
  // Later that day: the match's name, whether the server browser shows it, and the router port as a box to tick
  // (UPnP) with the port typed when it is not ticked.
  static constexpr int kPmMap = 0, kPmName = 1, kPmNet = 2, kPmListed = 3, kPmUpnp = 4, kPmPort = 5, kPmHost = 6, kPmTick = 7,
                       kPmPass = 8, kPmPassText = 9,
                       // 14 Nov 2026, the match's world ("cloud coverage, rate, weather percentage, day/night
                       // adjustment and time scale settings and sliders ... in the create match menu"): one row
                       // that opens its own list in the rail (the rail has no room for six more rows).
                       kPmWorld = 10, kPmDayNight = 11, kPmTime = 12, kPmDayLen = 13, kPmCloud = 14, kPmRate = 15,
                       kPmWeather = 16, kPmFog = 17, kPmGroundFog = 18, kPmWind = 19, kPmCloudHeight = 20, kPmWorldBack = 21,
                       // 18 Nov 2026, bots (bots.cpp; the user: "Let me play with them"): how many play.
                       kPmBots = 22,
                       // 19 Nov 2026 ("add match time and team ticket amount settings ... a map rotation selection as a
                       // submenu in create match ... an option to select the game mode"; "an option for servers to
                       // enable dynamic storm"): the game mode, the rotation's own list of rows (its switch, Rotate all
                       // maps, a row a map), the tickets, the time limit, and the storm in the world's rows.
                       kPmMode = 23, kPmRotation = 24, kPmTickets = 25, kPmTimeLimit = 26, kPmStorm = 27,
                       kPmRotOn = 28, kPmRotAll = 29, kPmRotMap = 30 /* to 33, a map each */, kPmRotBack = 34,
                       // 21 Nov 2026, the page of its own: Weather and time (the Weather tab's switch).
                       kPmWeatherOn = 35,
                       // D224 (20 Nov 2026): the storm's type, in the Weather tab's rows.
                       kPmStormType = 36,
                       // 27 Nov 2026 (TekRantGaming: "custom match screen parity in settings and bot counts ... a toggle
                       // for daybreak and nightops presets like the servers have"): the server type (Classic, Night
                       // Ops, Daybreak) and the server size (24, 32, 64), as the server manager offers them.
                       kPmType = 37, kPmSize = 38, kPmRows = 39;
  static constexpr const char* kPmTypeNames[] = {"CLASSIC", "NIGHT OPS", "DAYBREAK"};
  // The game modes. Each map has one in the game's data (its level holds that mode's objects and no other's: Wake
  // Island, Guadalcanal and Iwo Jima Conquest, Coral Sea Air Superiority), so the mode chooses the maps that play it.
  static constexpr const char* kPmModeNames[] = {"CONQUEST", "AIR SUPERIORITY", "ALL MODES"};
  static bool PmMapInMode(int mode, int map) { return mode == 2 || (mode == 0 ? map < 3 : map == 3); }
  // 27 Nov 2026, as the server manager's "Tickets, each team": Auto first (0: the map's own scaled to the match's size,
  // match_rules.cpp AutoTickets: 130 at 24, 175 at 32, 345 at 64), then the map's own (-1: 130, Coral Sea 100, whatever
  // the size), then the counts. (Before, 0 was the map's own: the same as Auto at 24 players, the only size then.)
  static constexpr int kPmTicketSteps[] = {0, -1, 25, 50, 75, 100, 150, 200, 250, 300, 400, 500, 750, 1000};
  static int PmTicketAt(int now) {  // Auto, the map's own, or the count's step at or under `now`
    if (now <= 0) return now == 0 ? 0 : 1;
    int at = 2;
    for (int k = 2; k < int(std::size(kPmTicketSteps)); ++k)
      if (kPmTicketSteps[k] <= now) at = k;
    return at;
  }
  // Auto's count for a map's own (match_rules.cpp AutoTickets at the page's size).
  int PmAutoTickets(int own) const {
    if (pm_size_ <= 24) return own;
    return std::clamp(int(std::lround(double(own) * pm_size_ / 24.0 / 5.0)) * 5, 1, 9999);
  }
  // The Weather tab: Weather and time on, and the match no event (Night Ops' and Daybreak's schedules set the world).
  bool PmWeatherTab() const { return pm_weather_on_ && pm_event_ == 0; }
  static constexpr int kPmTimeSteps[] = {0, 5, 10, 15, 20, 25, 30, 40, 45, 60, 90, 120};
  int pm_mode_ = 0;            // kPmModeNames
  bool pm_rotation_ = false;   // the maps in turn (pm_rot_maps_) rather than the one map
  int pm_rot_maps_ = 0xF;      // the rotation's maps, a bit each (kPmMapKeys' order)
  int pm_tickets_ = 0;         // each team's tickets; 0: the map's own
  int pm_time_limit_ = 0;      // minutes; 0: none, the round ends on tickets only
  bool pm_storm_ = false;      // the dynamic storm
  int pm_storm_type_ = 0;      // D224: kPmStormTypeNames
  static constexpr const char* kPmStormTypeNames[] = {"STORM", "HURRICANE"};
  bool pm_maps_view_ = false;  // the rail shows the rotation's rows
  // The rail scrolls when its rows are more than it holds: the selection kept in view, the wheel over it.
  float pm_scroll_ = 0;
  bool pm_scroll_follow_ = true;
  static int PmStep(const int* steps, int count, int now, int step) {
    int at = 0;
    for (int k = 0; k < count; ++k)
      if (steps[k] <= now) at = k;
    return steps[std::clamp(at + step, 0, count - 1)];
  }
  // The rotation's maps of the chosen mode, in turn.
  std::vector<int> PmRotationMaps() const {
    std::vector<int> out;
    for (int m = 0; m < 4; ++m)
      if (PmMapInMode(pm_mode_, m) && (pm_rot_maps_ >> m & 1)) out.push_back(m);
    return out;
  }
  bool PmRotationOn() const { return pm_rotation_ && !PmRotationMaps().empty(); }
  // The maps the match plays, in turn: the rotation's order (21 Nov 2026: the Levels list adds a map at its end, the
  // Rotation column moves it up or down), the chosen mode's only; one map plays round after round.
  std::vector<int> PmMatchMaps() const {
    std::vector<int> maps;
    for (int m : pm_order_)
      if (m >= 0 && m < 4 && PmMapInMode(pm_mode_, m)) maps.push_back(m);
    if (maps.empty()) maps = {PmMapOfMode(0, 1)};
    return maps;
  }
  // The Custom Match page as its own page (TekRantGaming, 21 Nov 2026, his pick of runs/ui-designs/custom-match,
  // Version G): the settings at the left under the Match tab (and the Weather tab, which exists only while Weather and
  // time is on), the map's picture with the game mode and the Levels in the middle, the Rotation in its order with
  // Host online and Host on LAN at the right, the chosen row's help along the foot.
  std::vector<int> pm_order_ = {0};  // the rotation in the order it plays (kPmMapKeys' numbers)
  bool pm_weather_on_ = false;       // Weather and time: the Weather tab's settings; off, the map's usual afternoon
  int pm_tab_ = 0;                   // 0 Match, 1 Weather
  int pm_col_ = 0;                   // the column the pad is in: 0 the settings, 1 the levels, 2 the rotation
  int pm_lv_row_ = 0;                // the levels: -1 the game mode, 0 to 3 the maps
  int pm_rot_row_ = 0;               // the rotation: its maps, then Host online and Host on LAN
  bool pm_grab_ = false;             // the rotation's map under the cursor is carried: up and down move it
  bool pm_dial_drag_ = false;        // the time of day's arc dragged with the mouse
  bool PmInOrder(int m) const { return std::find(pm_order_.begin(), pm_order_.end(), m) != pm_order_.end(); }
  // The rotation kept to the mode's maps, each once, never empty.
  void PmFitOrder() {
    std::vector<int> out;
    for (int m : pm_order_)
      if (m >= 0 && m < 4 && PmMapInMode(pm_mode_, m) && std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
    if (out.empty()) out.push_back(PmMapOfMode(0, 1));
    pm_order_ = out;
  }
  // The world the match has with Weather and time off: today's starting values (the map's usual afternoon).
  bool PmWorldDefault() const {
    return !pm_dn_ && std::fabs(pm_time_ - 16.0f) < 0.01f && pm_cloud_ == 35 && pm_rate_ == 33 && pm_weather_ == 0 && pm_fog_ == 0 &&
           pm_ground_fog_ == 0 && !pm_wind_manual_ && pm_cloud_height_ == 450 && !pm_storm_;  // the wind follows the weather
  }
  std::string PmHeadline() const {
    const std::vector<int> maps = PmMatchMaps();
    if (maps.size() == 1) return kPmMapNames[maps[0]];
    return fmt::format("{} MAPS IN TURN", maps.size());
  }
  // The first map of the chosen mode at or after `from`, going `step` (1 or -1).
  int PmMapOfMode(int from, int step) const {
    for (int k = 0; k < 4; ++k) {
      const int m = ((from + step * k) % 4 + 4) % 4;
      if (PmMapInMode(pm_mode_, m)) return m;
    }
    return 0;
  }
  int pm_bots_ = 0;  // bots in the match, each on the smaller side, within the room the players leave (0: none)
  int pm_size_ = 32;   // the match's size (kServerSizes; the servers' default, 32)
  int pm_event_ = 0;   // kPmTypeNames: 0 Classic, 1 Night Ops, 2 Daybreak (gm_host.cpp bf_world_event)
  bool pm_world_view_ = false;  // the rail shows the world's rows
  bool pm_dn_ = false;          // day and night
  float pm_time_ = 16.0f;       // the hour the match starts at
  int pm_daylen_ = 0;
  int pm_cloud_ = 35, pm_rate_ = 33, pm_weather_ = 0, pm_fog_ = 0, pm_ground_fog_ = 0, pm_wind_ = 30;  // per cent
  int pm_cloud_height_ = 450;  // the cloud base over the sea, metres
  // The wind follows the weather (19 Nov 2026, the user: "Weather percentage should also affect wind %, unless it's
  // manually adjusted, so 100% weather should result in 100% wind") until the wind's own row is set apart from it;
  // set back to the weather's value, it follows again.
  bool pm_wind_manual_ = false;
  void PmWeatherChanged() {
    if (!pm_wind_manual_) pm_wind_ = pm_weather_;
  }
  void PmWindChanged() { pm_wind_manual_ = pm_wind_ != pm_weather_; }
  // A password for the match (13 Nov 2026): a switch under the name, and its field while it is on.
  bool pm_pass_on_ = false;
  std::string pm_pass_;
  // The match's tick rate: 60 by default again and no longer experimental ("can we finalize that now that it works
  // well and make it default again?"), 30 the console's. 0.4.2 ("remove 75Hz and 100Hz from the
  // Client, but keep it for dedicated server"): a match hosted from here is 30 or 60; a dedicated server can still run
  // 75 or 100 (server/manager.py, --bf_tick_rate), and the game still joins one at its rate.
  static constexpr int kPmTicks[] = {30, 60};
  static constexpr int kPmTickCount = int(std::size(kPmTicks));
  int pm_tick_ = 60;
  bool pm_lan_ = false, pm_listed_ = false;
  std::string pm_name_, pm_port_text_ = "18300";
  // The row takes typing now: the name, or the port when it is the player's to choose.
  bool PmPortTyped() const { return pm_lan_ || !pm_upnp_; }
  bool PmTyping() const {
    return !pm_hosting_ && (pm_edit_ >= 0 || (pm_col_ == 0 && pm_tab_ == 0 &&
                                              (pm_row_ == kPmName || pm_row_ == kPmPassText || (pm_row_ == kPmPort && !pm_upnp_))));
  }
  // The server name and the password from a controller (TekRantGaming, 16 Nov 2026: "when controller A is pressed on
  // server name bring up on screen keyboard to allow editing and same with password edit"): A on either row opens
  // the on screen keys (OnScreenKeys, the soldier name's) over the map's picture with a copy of the text; DONE or
  // Start writes it back, B leaves it as it was. The password shows as stars unless Y shows it.
  int pm_edit_ = -1;  // the row the keys edit (kPmName or kPmPassText), or -1
  std::string pm_edit_text_;
  int pm_edit_key_ = 10;
  bool pm_edit_caps_ = false, pm_pass_shown_ = false;
  // The rows' limits, the same for the keyboard's typing and the on screen keys.
  static size_t PmMost(int row) { return row == kPmName ? 32 : row == kPmPassText ? 24 : 5; }
  static bool PmCharOk(int row, unsigned c) {
    return row == kPmName       ? (c >= 32 && c < 127 && c != '"' && c != '\\')
           : row == kPmPassText ? (c > 32 && c < 127 && c != '"' && c != '\\')
                                : (c >= '0' && c <= '9');
  }
  std::string PmPassShown(const std::string& pass) const { return pm_pass_shown_ ? pass : std::string(pass.size(), '*'); }
  // The port too (24 Nov 2026: a controller could not change it at all), digits only (PmCharOk).
  std::string& PmEditTarget(int row) { return row == kPmName ? pm_name_ : row == kPmPort ? pm_port_text_ : pm_pass_; }
  static const char* PmEditWhat(int row) { return row == kPmName ? "server name" : row == kPmPort ? "port" : "password"; }
  void PmEditOpen(int row) {
    pm_edit_ = row, pm_edit_key_ = row == kPmPort ? 0 : 10;  // the port's keys start on the 1
    pm_edit_text_ = PmEditTarget(row);
    pm_edit_caps_ = row == kPmName && pm_edit_text_.empty();
    REXLOG_INFO("BF private match: the on screen keys open on the {}", PmEditWhat(row));
  }
  void PmEditClose(bool save) {
    if (pm_edit_ < 0) return;
    if (save) {
      PmEditTarget(pm_edit_) = pm_edit_text_;
      SavePrivateMatchChoices();
    }
    REXLOG_INFO("BF private match: the on screen keys {} the {} ({} characters)", save ? "saved" : "cancelled",
                PmEditWhat(pm_edit_), PmEditTarget(pm_edit_).size());
    pm_edit_ = -1;
  }
  int pm_row_ = 0, pm_map_ = 0;
  bool pm_upnp_ = true;
  // The world's rows are sliders with a box for the exact value (the user, 16 Nov 2026: "day and weather controls in
  // custom match menu to be sliders ... box values for being able to type in exact values"): the row dragged now, the
  // row whose box is typed in and what was typed.
  int pm_drag_ = -1, pm_slide_edit_ = -1;
  std::string pm_slide_text_;
  struct PmSlide {
    float lo, hi, step;  // the slider's ends and its steps
    float most;          // the most a typed value may be
  };
  static bool PmSlideRow(int row) { return row == kPmTime || row == kPmDayLen || (row >= kPmCloud && row <= kPmCloudHeight); }
  static PmSlide PmSlideOf(int row) {
    if (row == kPmTime) return {0.0f, 23.75f, 0.25f, 23.99f};
    if (row == kPmDayLen) return {0.0f, 240.0f, 1.0f, 1440.0f};
    if (row == kPmCloudHeight) return {100.0f, 1500.0f, 10.0f, 1500.0f};
    return {0.0f, 100.0f, 1.0f, 100.0f};
  }
  float PmSlideGet(int row) const {
    switch (row) {
      case kPmTime: return pm_time_;
      case kPmDayLen: return float(pm_daylen_);
      case kPmCloud: return float(pm_cloud_);
      case kPmRate: return float(pm_rate_);
      case kPmWeather: return float(pm_weather_);
      case kPmFog: return float(pm_fog_);
      case kPmGroundFog: return float(pm_ground_fog_);
      case kPmWind: return float(pm_wind_);
      case kPmCloudHeight: return float(pm_cloud_height_);
    }
    return 0.0f;
  }
  // `exact`: a typed value (kept as typed, within the row's bounds); else the slider's, on its steps.
  void PmSlideSet(int row, float v, bool exact) {
    const PmSlide s = PmSlideOf(row);
    if (!exact) v = s.lo + std::round((v - s.lo) / s.step) * s.step;
    v = std::clamp(v, s.lo, exact ? s.most : s.hi);
    const int n = int(std::lround(v));
    switch (row) {
      case kPmTime: pm_time_ = v; break;
      case kPmDayLen: pm_daylen_ = n; break;
      case kPmCloud: pm_cloud_ = n; break;
      case kPmRate: pm_rate_ = n; break;
      case kPmWeather: pm_weather_ = n, PmWeatherChanged(); break;
      case kPmFog: pm_fog_ = n; break;
      case kPmGroundFog: pm_ground_fog_ = n; break;
      case kPmWind: pm_wind_ = n, PmWindChanged(); break;
      case kPmCloudHeight: pm_cloud_height_ = n; break;
    }
  }
  std::string PmSlideText(int row) const {
    switch (row) {
      case kPmTime: return fmt::format("{:02d}:{:02d}", int(pm_time_), int(std::lround(pm_time_ * 60.0f)) % 60);
      case kPmDayLen: return pm_daylen_ == 0 ? std::string("STILL") : fmt::format("{} MIN", pm_daylen_);
      case kPmCloudHeight: return fmt::format("{} M", pm_cloud_height_);
    }
    return fmt::format("{}%", int(PmSlideGet(row)));
  }
  bool pm_hosting_ = false;  // our server runs for this game
  bool pm_joining_ = false;  // this game's join to it started
  double pm_started_at_ = 0, pm_read_at_ = -100, pm_copied_at_ = -100;
  std::string pm_error_;
  bf1943::PrivateMatchStatus pm_status_;

  // The page's choices, kept in the settings file from one match to the next.
  void LoadPrivateMatchChoices() {
    pm_map_ = std::clamp(std::atoi(settings_.Get("private_match_map", "0").c_str()), 0, 4);
    pm_upnp_ = settings_.Get("private_match_upnp", "1") != "0";
    pm_lan_ = settings_.Get("private_match_lan", "0") == "1";
    pm_listed_ = settings_.Get("private_match_listed", "0") == "1";
    pm_name_ = settings_.Get("private_match_name", "");
    pm_port_text_ = settings_.Get("private_match_port", "18300");
    pm_pass_on_ = settings_.Get("private_match_password_on", "0") == "1";
    pm_pass_ = settings_.Get("private_match_password", "");
    pm_tick_ = std::atoi(settings_.Get("private_match_tick", "60").c_str());
    pm_size_ = std::atoi(settings_.Get("private_match_size", "32").c_str());
    if (std::find(std::begin(bf1943::kServerSizes), std::end(bf1943::kServerSizes), pm_size_) == std::end(bf1943::kServerSizes))
      pm_size_ = 32;
    pm_event_ = kNightOps ? std::clamp(std::atoi(settings_.Get("private_match_event", "0").c_str()), 0, 2) : 0;
    pm_bots_ = std::clamp(std::atoi(settings_.Get("private_match_bots", "0").c_str()), 0, pm_size_);
    pm_mode_ = std::clamp(std::atoi(settings_.Get("private_match_mode", "0").c_str()), 0, 2);
    pm_rotation_ = settings_.Get("private_match_rotation", "0") == "1";
    pm_rot_maps_ = std::clamp(std::atoi(settings_.Get("private_match_rotation_maps", "15").c_str()), 0, 15);
    if (pm_map_ == 4) pm_map_ = 0, pm_rotation_ = true, pm_rot_maps_ = 0xF, pm_mode_ = 2;  // before 19 Nov: all four in turn
    if (!PmMapInMode(pm_mode_, pm_map_)) pm_mode_ = pm_map_ == 3 ? 1 : 0;  // the map chosen keeps its mode
    pm_tickets_ = std::clamp(std::atoi(settings_.Get("private_match_tickets", "0").c_str()), -1, 9999);
    pm_time_limit_ = std::clamp(std::atoi(settings_.Get("private_match_time_limit", "0").c_str()), 0, 600);
    pm_storm_ = settings_.Get("private_match_dynamic_storm", "0") == "1";
    pm_storm_type_ = std::clamp(std::atoi(settings_.Get("private_match_storm_type", "0").c_str()), 0, 1);
    pm_maps_view_ = false;
    if (std::find(std::begin(kPmTicks), std::end(kPmTicks), pm_tick_) == std::end(kPmTicks)) pm_tick_ = 60;
    pm_edit_ = -1;  // the on screen keys never left open from an earlier visit
    pm_dn_ = settings_.Get("private_match_day_night", "0") == "1";
    pm_time_ = std::clamp(float(std::atof(settings_.Get("private_match_time", "16").c_str())), 0.0f, 23.5f);
    pm_daylen_ = std::atoi(settings_.Get("private_match_day_minutes", "0").c_str());
    pm_daylen_ = std::clamp(pm_daylen_, 0, 1440);  // any length (the slider, or typed)
    pm_cloud_ = std::clamp(std::atoi(settings_.Get("private_match_cloud_cover", "35").c_str()), 0, 100);
    pm_rate_ = std::clamp(std::atoi(settings_.Get("private_match_cloud_rate", "33").c_str()), 0, 100);
    pm_weather_ = std::clamp(std::atoi(settings_.Get("private_match_weather", "0").c_str()), 0, 100);
    pm_fog_ = std::clamp(std::atoi(settings_.Get("private_match_fog", "0").c_str()), 0, 100);
    pm_ground_fog_ = std::clamp(std::atoi(settings_.Get("private_match_ground_fog", "0").c_str()), 0, 100);
    pm_wind_ = std::clamp(std::atoi(settings_.Get("private_match_wind", "30").c_str()), 0, 100);
    pm_wind_manual_ = settings_.Get("private_match_wind_manual", "0") == "1";
    PmWeatherChanged();
    pm_cloud_height_ = std::clamp(std::atoi(settings_.Get("private_match_cloud_height", "450").c_str()), 100, 1500);
    // The rotation's order (21 Nov 2026); before it, the map, or the rotation's maps in the game's order.
    pm_order_.clear();
    const std::string order = settings_.Get("private_match_rotation_order", "");
    for (const char c : order)
      if (c >= '0' && c <= '3') pm_order_.push_back(c - '0');
    if (order.empty()) pm_order_ = PmRotationOn() ? PmRotationMaps() : std::vector<int>{std::clamp(pm_map_, 0, 3)};
    PmFitOrder();
    // Weather and time (21 Nov 2026): before it the world's settings always applied, so a player who had changed them
    // finds it on.
    const std::string custom = settings_.Get("private_match_weather_custom", "");
    pm_weather_on_ = custom.empty() ? !PmWorldDefault() : custom == "1";
    pm_pass_on_ = !pm_pass_.empty();  // the password is on while it has one (no switch since 21 Nov 2026)
    pm_tab_ = 0, pm_col_ = 0, pm_lv_row_ = 0, pm_rot_row_ = 0, pm_grab_ = false, pm_dial_drag_ = false;
  }
  void SavePrivateMatchChoices() {
    pm_pass_on_ = !pm_pass_.empty();
    settings_.values["private_match_map"] = std::to_string(pm_map_);
    settings_.values["private_match_upnp"] = pm_upnp_ ? "1" : "0";
    settings_.values["private_match_lan"] = pm_lan_ ? "1" : "0";
    settings_.values["private_match_listed"] = pm_listed_ ? "1" : "0";
    settings_.values["private_match_name"] = pm_name_;
    settings_.values["private_match_port"] = pm_port_text_;
    settings_.values["private_match_tick"] = std::to_string(pm_tick_);
    settings_.values["private_match_bots"] = std::to_string(pm_bots_);
    settings_.values["private_match_size"] = std::to_string(pm_size_);
    settings_.values["private_match_event"] = std::to_string(pm_event_);
    settings_.values["private_match_mode"] = std::to_string(pm_mode_);
    settings_.values["private_match_rotation"] = pm_rotation_ ? "1" : "0";
    settings_.values["private_match_rotation_maps"] = std::to_string(pm_rot_maps_);
    settings_.values["private_match_tickets"] = std::to_string(pm_tickets_);
    settings_.values["private_match_time_limit"] = std::to_string(pm_time_limit_);
    settings_.values["private_match_dynamic_storm"] = pm_storm_ ? "1" : "0";
    settings_.values["private_match_storm_type"] = std::to_string(pm_storm_type_);
    settings_.values["private_match_password_on"] = pm_pass_on_ ? "1" : "0";
    settings_.values["private_match_password"] = pm_pass_;
    settings_.values["private_match_day_night"] = pm_dn_ ? "1" : "0";
    settings_.values["private_match_time"] = fmt::format("{:.2f}", pm_time_);
    settings_.values["private_match_day_minutes"] = std::to_string(pm_daylen_);
    settings_.values["private_match_cloud_cover"] = std::to_string(pm_cloud_);
    settings_.values["private_match_cloud_rate"] = std::to_string(pm_rate_);
    settings_.values["private_match_weather"] = std::to_string(pm_weather_);
    settings_.values["private_match_fog"] = std::to_string(pm_fog_);
    settings_.values["private_match_ground_fog"] = std::to_string(pm_ground_fog_);
    settings_.values["private_match_wind"] = std::to_string(pm_wind_);
    settings_.values["private_match_wind_manual"] = pm_wind_manual_ ? "1" : "0";
    settings_.values["private_match_cloud_height"] = std::to_string(pm_cloud_height_);
    std::string order;
    for (int m : pm_order_) order += char('0' + m);
    settings_.values["private_match_rotation_order"] = order;
    settings_.values["private_match_weather_custom"] = pm_weather_on_ ? "1" : "0";
    // The older rows' settings follow the order, for a build from before 21 Nov 2026.
    if (!pm_order_.empty()) settings_.values["private_match_map"] = std::to_string(pm_order_.front());
    settings_.values["private_match_rotation"] = pm_order_.size() > 1 ? "1" : "0";
    int bits = 0;
    for (int m : pm_order_) bits |= 1 << m;
    settings_.values["private_match_rotation_maps"] = std::to_string(bits);
    settings_.Save();
  }

  void HostPrivateMatch() {
    bf1943::PrivateMatchOptions o;
    PmFitOrder();
    for (int m : PmMatchMaps()) o.maps.push_back(kPmMapKeys[m]);
    o.tickets = pm_tickets_, o.time_limit = pm_time_limit_, o.dynamic_storm = PmWeatherTab() && pm_storm_;
    o.storm_type = PmWeatherTab() ? pm_storm_type_ : 0;
    o.size = pm_size_, o.event = pm_event_;
    o.weather_system = PmWeatherTab() || pm_event_ != 0;  // D254: Weather and time on, or Night Ops or Daybreak
    o.upnp = pm_upnp_;
    // The test launcher (tools	est_progression.py: the test_progression setting, its own backend on this PC's
    // loopback) already answers sign-in, matchmaking and the server list on the ports a LAN match's own backend
    // needs (TCP 18241 and 18246), and the game is signed in to it: a LAN match is hosted through that backend there,
    // not through one of its own (16 Nov 2026: hosting from the test launcher was refused, "another program on this PC
    // already uses TCP 18241").
    const bool test_backend = settings_.Get("test_progression", "") == "1" &&
                              rex::cvar::GetFlagByName("bf_backend_registry").rfind("127.0.0.1:", 0) == 0;
    if (pm_lan_ && test_backend) REXLOG_INFO("BF private match: a LAN match under the test launcher: through its backend");
    o.lan = pm_lan_ && !test_backend;
    if (o.lan) {
      // The game signs in to the match's own backend, which it can only choose before it starts (once it runs it is
      // signed in to the main one).
      if (!pre_game_ && own_backend_ != "127.0.0.1") {
        // 0.4.2: no refusal; the game starts again and hosts the match from Home (game_menu.cpp HostLanAtStartTick).
        REXLOG_INFO("BF private match: a LAN match with the game already running: starting again to host it");
        SavePrivateMatchChoices();
        SetEnv("BF_HOST_LAN_AT_START", "1"), SetEnv("BF_RESTARTED", "1");
        if (update::RestartSelf() && quit_) quit_();
        else pm_error_ = "The game could not start again. Close it, start it again and host the LAN match first.";
        return;
      }
      UseOwnBackend("127.0.0.1");
    }
    const std::string me = settings_.Get("player_name", "");
    // Through our profanity filter (text_filter.h): a listed match's name is everybody's to read.
    o.name = text_filter::Censor(!pm_name_.empty() ? pm_name_ : me.empty() ? "Custom match" : me + "'s match");
    pm_pass_on_ = !pm_pass_.empty();  // a password typed is on; empty, anybody joins
    o.password = pm_pass_;
    o.tick = pm_tick_;
    o.bots = std::clamp(pm_bots_, 0, pm_size_);
    // The match always has its clock's light and sky (16 Nov 2026: the switch "should just pause the time progression",
    // not bring the game's own afternoon back): off, the time stands still at the time of day.
    o.day_night = true, o.time = pm_time_, o.day_minutes = pm_dn_ ? pm_daylen_ : 0;
    o.cloud_cover = pm_cloud_ / 100.0f, o.cloud_rate = pm_rate_ / 100.0f, o.weather = pm_weather_ / 100.0f;
    o.fog = pm_fog_ / 100.0f, o.ground_fog = pm_ground_fog_ / 100.0f, o.wind = pm_wind_ / 100.0f;
    o.cloud_height = float(pm_cloud_height_);
    if (!PmWeatherTab()) {  // Weather and time off (or an event's): the map's usual afternoon (the page's starting values)
      o.time = 16.0f, o.day_minutes = 0, o.cloud_cover = 0.35f, o.cloud_rate = 0.33f, o.weather = 0, o.fog = 0, o.ground_fog = 0;
      o.wind = 0, o.cloud_height = 450.0f;  // the wind with the weather, as the page's starting values
      // Night Ops and Daybreak: the wind a server of the manager has by default (30%; Daybreak's build drives its own).
      if (pm_event_) o.wind = 0.3f;
    }
    // The host's own game runs at the match's rate, set before its timers are made. Once a match was played at
    // another, this run cannot change: the server would start, and the host's join would have to start the game again,
    // which ends the server with it (the review of pull request 16). Said instead.
    if (!SetTickRate(pm_tick_)) {
      pm_error_ = fmt::format("This run plays at {} Hz. To host at {} Hz, start the game again first.", TickRate(), pm_tick_);
      return;
    }
    // In everybody's browser only when Show in browser is on (as before 0.4.1: a custom match is joined by its address
    // unless its host says otherwise; the review of pull request 16 found it always listed, the row never shown). A
    // LAN match is on its network's LAN tab.
    o.listed = !pm_lan_ && pm_listed_;
    if (PmPortTyped()) {
      const int port = std::atoi(pm_port_text_.c_str());
      if (port < 1024 || port > 65535) {
        pm_error_ = "The port is a number from 1024 to 65535 (18300 is the usual one).";
        return;
      }
      o.port = uint16_t(port);
    }
    SavePrivateMatchChoices();
    pm_error_.clear();
    std::string why;
    if (!bf1943::StartPrivateMatch(o, &why)) {
      pm_error_ = why;
      return;
    }
    pm_hosting_ = true, pm_joining_ = false, pm_started_at_ = ImGui::GetTime(), pm_read_at_ = -100;
    pm_status_ = {};
    REXLOG_INFO("BF private match: hosting ({}, {}, {}, size {}, {} bots; tickets {}, time limit {} min, dynamic storm {}, "
                "storm type {})",
                PmHeadline(), kPmModeNames[pm_mode_], kPmTypeNames[pm_event_], pm_size_, o.bots,
                pm_tickets_ > 0 ? std::to_string(pm_tickets_) : pm_tickets_ == 0 ? std::string("auto") : std::string("the map's own"),
                pm_time_limit_, o.dynamic_storm ? "on" : "off", kPmStormTypeNames[pm_storm_type_]);
  }

  void EndPrivateMatch() {
    if (!pm_hosting_) return;
    bf1943::StopPrivateMatch();
    pm_hosting_ = pm_joining_ = false;
  }

  // The address friends give Direct connect: the one the backend saw our server register from, and its port.
  void CopyPrivateAddress() {
    if (pm_status_.address.empty()) return;
    // A LAN match: this PC's address on its network (the match's own reads 127.0.0.1 here).
    std::string address = pm_status_.address;
    if (pm_lan_)
      if (const std::string lan = bf1943::LanAddress(); !lan.empty()) address = lan;
    ImGui::SetClipboardText(address.c_str());
    pm_copied_at_ = ImGui::GetTime();
    REXLOG_INFO("BF private match: the address copied for friends");
  }

  // Every frame: our server's state, the join once its host has registered it, and its end when the player is back on
  // our main menu (the match left, or the join failed).
  void PrivateMatchTick() {
    if (!pm_hosting_) return;
    const double now = ImGui::GetTime();
    if (now - pm_read_at_ > 0.5) pm_read_at_ = now, pm_status_ = bf1943::ReadPrivateMatchStatus();
    if (pm_joining_ && page_ == Page::kMain && MenuDriverStage() == 2) {
      REXLOG_INFO("BF private match: the player is back on the main menu: the match ends");
      EndPrivateMatch();
      return;
    }
    if (pm_joining_ || pm_status_.state != "registered" || pm_status_.gid.empty()) return;
    // This PC's own public address, as the backend gives it to joiners, reached through 127.0.0.1 (private_match.h).
    const size_t colon = pm_status_.address.rfind(':');
    in_addr ip{};
    if (colon != std::string::npos && inet_pton(AF_INET, pm_status_.address.substr(0, colon).c_str(), &ip) == 1)
      bf1943::SetSelfHostedAddress(ip.s_addr, htons(uint16_t(std::atoi(pm_status_.address.c_str() + colon + 1))));
    ServerInfo sv;
    sv.gid = pm_status_.gid;
    const std::string me = settings_.Get("player_name", "");
    sv.name = me.empty() ? "Custom match" : me + "'s match";
    sv.address = pm_status_.address;
    sv.map = kPmMapTitles[PmMatchMaps().front()];
    sv.max_players = 24;
    // Its tick rate is the one it was started at. (Left unsaid, Join took it for a 30 Hz server and set this game to
    // 30 against a 60 Hz match: 13 Nov 2026, "I cannot walk at 60Hz anymore, and player partially falls through
    // carrier".)
    sv.tick_hz = pm_tick_;
    // The host's own game shows its match the password like any other (its join is then let in).
    if (pm_pass_on_ && !pm_pass_.empty()) KnockMatch(sv.address, me, pm_pass_);
    pm_joining_ = true;
    exp_accepted_address_ = sv.address;  // its rate chosen on the Custom Match page, with that page's warning
    REXLOG_INFO("BF private match: registered as game {}; joining it", sv.gid);
    Join(sv);
  }

  // ---- Custom Match, a page of its own (TekRantGaming's pick, 21 Nov 2026: runs/ui-designs/custom-match Version G,
  // with the time of day's play button made a Time passes switch under it). Three columns on the menu's backdrop: the
  // settings at the left (the Match tab, and the Weather tab while Weather and time is on), the chosen map's picture
  // with the game mode and the Levels in the middle, the Rotation at the right in the order it plays with Host online
  // and Host on LAN under it; the chosen row's help along the foot. View (Tab) goes from column to column, LB and RB
  // (Q and E) from tab to tab, Start (P) hosts online. Sizes are the Stage's (720 lines).
  static const char* PmTitle(int row) {
    switch (row) {
      case kPmName: return "SERVER NAME";
      case kPmPassText: return "PASSWORD";
      case kPmType: return "SERVER TYPE";
      case kPmSize: return "SERVER SIZE";
      case kPmTickets: return "TICKETS";
      case kPmTimeLimit: return "MATCH TIME";
      case kPmBots: return "BOTS";
      case kPmTick: return "TICK RATE";
      case kPmListed: return "SHOW IN BROWSER";
      case kPmUpnp: return "UPNP";
      case kPmPort: return "PORT";
      case kPmWeatherOn: return "WEATHER AND TIME";
      case kPmTime: return "TIME OF DAY";
      case kPmDayNight: return "TIME PASSES";
      case kPmDayLen: return "A DAY LASTS";
      case kPmCloud: return "CLOUD COVER";
      case kPmRate: return "CLOUD CHANGE RATE";
      case kPmCloudHeight: return "CLOUD HEIGHT";
      case kPmWeather: return "WEATHER";
      case kPmStorm: return "DYNAMIC STORM";
      case kPmStormType: return "STORM TYPE";
      case kPmWind: return "WIND";
      case kPmFog: return "DISTANCE FOG";
      case kPmGroundFog: return "GROUND FOG";
    }
    return "";
  }
  static const char* PmAbout(int row) {
    switch (row) {
      case kPmName:
        return "The match's name in the server browser. Type it with the keyboard, or A for the on screen keys; left empty it is "
               "your name's match. On a controller, right moves over to the Levels.";
      case kPmPassText:
        return "The password players type to join: up to 24 letters, digits or signs, no spaces. Empty, anybody who sees the "
               "match can join; the browser shows a padlock by a match with one. Y (F2) shows it.";
      case kPmType:
        return "The match's type, as on our servers. Classic: the time and weather you set. Night Ops: 18:30 under the stars, "
               "the clouds gather as the tickets fall and a storm rolls in for the end. Daybreak: sunrise at 06:10 and the same "
               "building storm. Every round starts clear again; F9 still changes the weather for the round.";
      case kPmSize:
        return "How many players the match holds: 24 as in the original game, 32 as our servers by default, or 64. The bots fill "
               "up to it and Auto tickets grow with it. Each player's game needs this version or later for 32 or 64.";
      case kPmTickets:
        return "Each team's tickets at a round's start: a team loses one for each of its soldiers who dies, and more while the "
               "other team holds most flags. Auto, as on our servers: the map's own (130, Coral Sea 100) grown with the "
               "match's size, 175 at 32 and 345 at 64. The map's own keeps 130 whatever the size.";
      case kPmTimeLimit:
        return "How long a round lasts: when it is up, the team with fewer tickets loses. Every player sees the time left over the "
               "ticket bars. Off, a round ends only when a team has no tickets left.";
      case kPmBots:
        return "Computer players who fill the match, half on each side, up to its size: they take flags, drive, fly and fight "
               "like players and keep out of everybody's stats. A player who joins takes a bot's place (two places are always "
               "kept free). They sit out Coral Sea.";
      case kPmTick:
        return "How many times a second the match is simulated. 60 is the default; 30 is the original game's and asks less of your "
               "PC and of every player's connection. A player's game takes the match's rate when it joins.";
      case kPmListed:
        return "Online matches. Yes: everybody sees your match on the server browser's Community tab and can join. No: only friends "
               "you give the address to (Direct connect). A LAN match is always found by the PCs of your network.";
      case kPmUpnp:
        return "Online matches. On, your router is asked to open the match's port and the port is chosen for you. Off, you open the "
               "port below on your router yourself.";
      case kPmPort:
        return "The UDP port players reach your match on, when UPnP is off or the match is a LAN one: 18300 unless you want another. "
               "For an Online match, open it on your router for friends outside your home. Type it, or A for the on screen keys.";
      case kPmWeatherOn:
        return "Off: the map's usual afternoon, 16:00 and fair, and no Weather tab. On: a Weather tab appears beside Match with the "
               "time of day, clouds, weather, wind and fog. F9 changes them while the match runs. Night Ops and Daybreak set "
               "them by their own schedule.";
      case kPmTime:
        return "The hour the match starts at: left and right move the sun, or the moon at night, a quarter hour; drag it with the "
               "mouse, or type it (18:30).";
      case kPmDayNight:
        return "On, the match's clock runs: the sun goes over, then the moon, through dusk and night. Off, the time stands still at "
               "the time of day above.";
      case kPmDayLen:
        return "How many minutes a whole day and night take in the match while the time passes. Left and right, or type up to 1440.";
      case kPmCloud: return "How much of the sky the clouds take, for players who chose the volumetric sky.";
      case kPmRate: return "How fast the clouds form and dissolve. Lower is calmer and closer to real weather.";
      case kPmCloudHeight:
        return "How high over the sea the clouds begin, for players who chose the volumetric sky. Low, the planes fly through them "
               "and can hide in them.";
      case kPmWeather:
        return "From fair to a storm: more and denser cloud, a greyer sky and less sun; from about half, mist and far rain shafts; a "
               "little higher, rain; from about two thirds, lightning in the clouds.";
      case kPmStorm:
        return "On, the weather builds from the Weather setting to a full storm over each round and eases again (over the match time, "
               "else over half an hour). The wind goes with it unless set apart.";
      case kPmStormType:
        return "What the weather builds where it is strong: a thunderstorm (towering clouds, heavy rain and lightning over one "
               "stretch of the map), or a hurricane (a calm eye, its wall and spiral rain bands). Both come in from the sea as the "
               "weather rises and cover the map at 100%.";
      case kPmWind: return "From calm to a gale: the stronger the wind, the further the rain leans. Follows the weather until set apart.";
      case kPmFog: return "A haze that hides what is far: at 100% the sight is about a kilometre.";
      case kPmGroundFog: return "A low layer of fog over the sea and the shore, in patches that drift.";
    }
    return "";
  }
  // The sun's height at the time of day, -1 (midnight) to 1 (noon).
  float PmSunHeight() const { return std::sin(3.14159265f * (pm_time_ - 6.0f) / 12.0f); }
  // The map's picture tinted for the time of day on the Weather tab (a hint, not the game's own light).
  ImU32 PmSkyTint() const {
    const float s = PmSunHeight();
    auto mix = [](float a, float b, float k) { return int(a + (b - a) * std::clamp(k, 0.0f, 1.0f)); };
    if (s >= 0.25f) return IM_COL32(255, 255, 255, 255);
    if (s >= 0.0f) return IM_COL32(255, mix(200, 255, s / 0.25f), mix(150, 255, s / 0.25f), 255);
    const float k = -s / 0.25f;
    return IM_COL32(mix(255, 64, k), mix(200, 80, k), mix(150, 124, k), 255);
  }
  const char* PmDayPart() const {
    const float s = PmSunHeight();
    return s >= 0.25f ? "DAY" : s >= -0.05f ? (pm_time_ < 12 ? "DAWN" : "DUSK") : "NIGHT";
  }

  void PrivateMatchPage(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("private match");
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    MenuBackdrop(d, io.DisplaySize, st);
    bool online_ok = false;
    const std::string status = OnlineStatus(online_ok);
    MenuChrome(d, io, st, status, online_ok);
    MenuFoot(d, st);
    const ImVec2 la = st.P(48, 107), lb = st.P(475, 579);    // the settings
    const ImVec2 pa = st.P(491, 107), pb = st.P(864, 320);   // the map's picture
    const ImVec2 va = st.P(491, 331), vb = st.P(864, 579);   // the game mode and the levels
    const ImVec2 ra = st.P(880, 107), rb = st.P(1232, 480);  // the rotation
    const ImVec2 o0 = st.P(880, 491), o1 = st.P(1232, 535);  // Host online
    const ImVec2 h0 = st.P(880, 541), h1 = st.P(1232, 579);  // Host on LAN
    const ImVec2 ba = st.P(48, 589), bb = st.P(1232, 645);   // the chosen row's help
    const ImU32 rail = IM_COL32(13, 15, 11, 214), edge = IM_COL32(241, 234, 211, 30), rule = IM_COL32(241, 234, 211, 18);
    const ImU32 dim_ink = IM_COL32(120, 116, 100, 150);
    if (!PmWeatherTab()) pm_tab_ = 0;
    PmFitOrder();
    int n = int(pm_order_.size());
    pm_rot_row_ = std::clamp(pm_rot_row_, 0, n + 1);
    pm_lv_row_ = std::clamp(pm_lv_row_, -1, 3);
    if (pm_rot_row_ >= n) pm_grab_ = false;
    const bool editing = pm_edit_ >= 0;

    // ---- the map's picture: the map under the cursor, else the rotation's first; on the Weather tab, tinted for the
    // time of day
    auto draw_picture = [&]() {
      int pic = pm_order_.front();
      if (pm_col_ == 1 && pm_lv_row_ >= 0) pic = pm_lv_row_;
      if (pm_col_ == 2 && pm_rot_row_ < n) pic = pm_order_[size_t(pm_rot_row_)];
      d->PushClipRect(pa, pb, true);
      bool drawn = false;
      if (pm_tab_ == 1)
        drawn = kit::DrawImageFile(d, UserDir() / "cache" / "extracted" / "maps" / (std::string(kPmMapKeys[pic]) + ".png"), pa, pb,
                                   false, false, 0, PmSkyTint());
      if (!drawn) MapPicture(d, st.u * 1.5f, pa, pb, kPmMapTitles[pic]);
      d->PopClipRect();
      d->AddRect(pa, pb, edge, 0, 0, 1.0f);
      const std::string chip = pm_tab_ == 1 ? fmt::format("{} \xC2\xB7 {} \xC2\xB7 {}", PmSlideText(kPmTime), PmDayPart(), kPmMapNames[pic])
                                            : fmt::format("{} \xC2\xB7 {} \xC2\xB7 YOUR OWN SERVER", kPmMapNames[pic],
                                                          pic == 3 ? "AIR SUPERIORITY" : "CONQUEST");
      const float cw = kit::SpacedWidth(f.ui_bold, st(9), chip, st(1));
      const ImVec2 c0(pa.x + st(14), pa.y + st(14)), c1(c0.x + cw + st(22), c0.y + st(22));
      d->AddRectFilled(c0, c1, kit::kStrip);
      kit::Spaced(d, f.ui_bold, st(9), ImVec2(c0.x + st(11), c0.y + st(4)), kit::kStripText, chip, st(1));
    };
    draw_picture();

    // ---- while our server starts: its steps in the settings' place, the wait on paper over the levels
    if (pm_hosting_) {
      d->AddRectFilled(la, lb, rail);
      d->AddRect(la, lb, edge, 0, 0, 1.0f);
      kit::Spaced(d, f.typed, st(8), ImVec2(la.x + st(18), la.y + st(14)), kit::kKhakiDim, "HOSTED ON THIS PC", st(2));
      kit::GameText(d, kit::GameFont::kHeadline, st(24), ImVec2(la.x + st(18), la.y + st(28)), kit::kCream, "CUSTOM MATCH");
      kit::PaperArt(d, va, vb);
      PrivateMatchStarting(d, st, in, ImVec2(la.x, la.y + st(68)), lb, va, vb);
      return;
    }

    // ---- the prompts first: a click on one is that button's press for the rest of the frame
    std::vector<Cue> cues;
    if (editing) {
      cues = {{kit::Button::kA, "TYPE"}, {kit::Button::kX, "DELETE", "Backspace"}};
      if (pm_edit_ == kPmPassText) cues.push_back({kit::Button::kY, pm_pass_shown_ ? "HIDE" : "SHOW", "F2"});
      cues.push_back({kit::Button::kStart, "DONE", "", true});
      cues.push_back({kit::Button::kB, "CANCEL"});
    } else {
      if (pm_col_ == 0) {
        const bool text_row = pm_row_ == kPmName || pm_row_ == kPmPassText || pm_row_ == kPmPort;
        // A slider's box is typed into, so a controller's A steps it as on the other rows (it opened a box only a
        // keyboard could fill).
        if (!text_row)
          cues.push_back({kit::Button::kA, pm_tab_ == 1 && PmSlideRow(pm_row_) && device_ == Device::kKeyboard ? "TYPE IT" : "CHANGE"});
        else if (device_ != Device::kKeyboard) cues.push_back({kit::Button::kA, "EDIT"});
        if (pm_row_ == kPmPassText) cues.push_back({kit::Button::kY, pm_pass_shown_ ? "HIDE" : "SHOW", "F2"});
        if (PmWeatherTab()) cues.push_back({kit::Button::kLB, "MATCH / WEATHER"});
      } else if (pm_col_ == 1) {
        if (pm_lv_row_ >= 0 && PmMapInMode(pm_mode_, pm_lv_row_))
          cues.push_back({kit::Button::kA, PmInOrder(pm_lv_row_) ? (n > 1 ? "TAKE OUT" : "THE ONLY MAP") : "ADD TO ROTATION"});
        else if (pm_lv_row_ < 0) cues.push_back({kit::Button::kA, "CHANGE"});
      } else if (pm_rot_row_ < n) {
        cues.push_back({kit::Button::kA, pm_grab_ ? "PUT DOWN" : "MOVE"});
        if (n > 1 && !pm_grab_) cues.push_back({kit::Button::kX, "TAKE OUT"});
      } else {
        cues.push_back({kit::Button::kA, pm_rot_row_ == n ? "HOST ONLINE" : "HOST ON LAN"});
      }
      cues.push_back({kit::Button::kBack, "COLUMNS"});
      cues.push_back({kit::Button::kStart, "HOST ONLINE", "", true});
      cues.push_back({kit::Button::kB, pm_grab_ ? "PUT DOWN" : pm_col_ != 0 ? "SETTINGS" : pm_tab_ == 1 ? "MATCH" : "BACK"});
    }
    Prompts(d, st, st.P(48, 662), cues, in, 15);
    // F2 (the keyboard's Y here: R types a letter) shows or hides the password.
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && ((pm_col_ == 0 && pm_row_ == kPmPassText) || pm_edit_ == kPmPassText)) in.y = true;
    Input ekeys;  // while the keys are open, the page takes nothing: the keys get this frame's presses
    if (editing) ekeys = in, in = {};
    if (!editing && in.y && pm_col_ == 0 && pm_row_ == kPmPassText) pm_pass_shown_ = !pm_pass_shown_;

    auto host = [&](bool lan) {
      pm_lan_ = lan;
      REXLOG_INFO("BF private match: Host {} pressed ({}, rotation {})", lan ? "on LAN" : "online", PmHeadline(), n);
      HostPrivateMatch();
    };
    auto set_tab = [&](int t) {
      pm_tab_ = t, pm_col_ = 0, pm_row_ = t ? kPmTime : kPmWeatherOn, pm_slide_edit_ = -1;
      REXLOG_INFO("BF private match: the {} tab", t ? "Weather" : "Match");
    };
    if (!editing && PmWeatherTab() && (in.prev_tab || in.next_tab)) set_tab(1 - pm_tab_);
    if (!editing && in.view) pm_col_ = (pm_col_ + 1) % 3, pm_grab_ = false, pm_slide_edit_ = -1;
    if (!editing && in.start) {
      host(false);
      return;
    }

    // ---- the settings' rows of the tab shown
    std::vector<int> order;
    if (pm_tab_ == 0) {
      order = {kPmName, kPmPassText};
      if (kNightOps) order.push_back(kPmType);  // night_ops.h: not in every build
      for (const int r : {kPmSize, kPmTickets, kPmTimeLimit, kPmBots, kPmTick, kPmListed, kPmUpnp}) order.push_back(r);
      if (!pm_upnp_) order.push_back(kPmPort);
      order.push_back(kPmWeatherOn);
    } else {
      order = {kPmTime, kPmDayNight, kPmDayLen, kPmCloud, kPmRate, kPmCloudHeight, kPmWeather, kPmStormType, kPmStorm, kPmWind, kPmFog, kPmGroundFog};
    }
    if (std::find(order.begin(), order.end(), pm_row_) == order.end()) pm_row_ = pm_tab_ == 0 && pm_row_ == kPmPort ? kPmUpnp : order.front();
    const int at_row = int(std::find(order.begin(), order.end(), pm_row_) - order.begin());
    if (pm_col_ == 0) {
      const int m = int(order.size());
      if (in.up) pm_row_ = order[size_t((at_row + m - 1) % m)], pm_slide_edit_ = -1;
      if (in.down) pm_row_ = order[size_t((at_row + 1) % m)], pm_slide_edit_ = -1;
    }
    // Across the columns with left and right wherever they change nothing (TekRantGaming, 24 Nov 2026: "the custom
    // match screen needs to work a little bit better with controller. Can you just make sure that the controller can
    // navigate the page properly?"): the levels' and the rotation's rows, and the settings' rows that are typed into
    // (name, password, port). Before, only View (Tab) went across. The cursor lands on the same map: a level row on its
    // place in the rotation, a rotation row on its level.
    auto go_col = [&](int c) {
      if (c == pm_col_) return;
      if (c == 1) {
        if (pm_col_ == 2 && pm_rot_row_ < n) pm_lv_row_ = pm_order_[size_t(pm_rot_row_)];
        pm_lv_row_ = std::max(pm_lv_row_, 0);
      }
      if (c == 2 && pm_col_ == 1 && pm_lv_row_ >= 0) {
        const auto it = std::find(pm_order_.begin(), pm_order_.end(), pm_lv_row_);
        pm_rot_row_ = it != pm_order_.end() ? int(it - pm_order_.begin()) : std::min(pm_rot_row_, n - 1);
      }
      pm_col_ = c, pm_grab_ = false, pm_slide_edit_ = -1;
      REXLOG_INFO("BF private match: the pad in the {} column", c == 0 ? "settings" : c == 1 ? "levels" : "rotation");
    };
    if (!editing) {
      const bool typed_row = pm_col_ == 0 && pm_tab_ == 0 && (pm_row_ == kPmName || pm_row_ == kPmPassText || pm_row_ == kPmPort);
      if (typed_row && in.right) go_col(1), in.right = false;
      else if (pm_col_ == 1 && pm_lv_row_ >= 0 && (in.left || in.right)) go_col(in.left ? 0 : 2), in.left = in.right = false;
      else if (pm_col_ == 2 && !pm_grab_ && in.left) go_col(1), in.left = false;
    }
    if (PmTyping()) {  // the keyboard types straight into the row (as the browser's address)
      ImGuiContext& g = *ImGui::GetCurrentContext();
      g.PlatformImeData.WantTextInput = true;
      g.PlatformImeData.ViewportId = ImGui::GetMainViewport()->ID;
      // With the on screen keys open: their copy (a real keyboard's typing then closes them with it kept, as on the
      // soldier name's page, and goes on in the row).
      const int row = editing ? pm_edit_ : pm_row_;
      std::string& text = editing ? pm_edit_text_ : pm_row_ == kPmName ? pm_name_ : pm_row_ == kPmPassText ? pm_pass_ : pm_port_text_;
      bool typed = false;
      for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const ImWchar c = io.InputQueueCharacters[i];
        if (PmCharOk(row, unsigned(c)) && text.size() < PmMost(row)) text += char(c), typed = true;
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !text.empty()) text.pop_back();
      if (editing && typed) PmEditClose(true);
      if (!editing && typed) SavePrivateMatchChoices();
      in.left = in.right = false;
    }
    if (pm_slide_edit_ >= 0 && (pm_slide_edit_ != pm_row_ || pm_col_ != 0)) pm_slide_edit_ = -1;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) pm_drag_ = -1, pm_dial_drag_ = false;
    if (pm_tab_ == 1 && pm_col_ == 0 && PmSlideRow(pm_row_)) {
      // A digit typed on a slider's row starts its box; Enter takes the value, Esc or B leaves it as it was.
      ImGuiContext& g = *ImGui::GetCurrentContext();
      g.PlatformImeData.WantTextInput = true;
      g.PlatformImeData.ViewportId = ImGui::GetMainViewport()->ID;
      for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const ImWchar c = io.InputQueueCharacters[i];
        if (!((c >= '0' && c <= '9') || c == ':' || c == '.')) continue;
        if (pm_slide_edit_ != pm_row_) pm_slide_edit_ = pm_row_, pm_slide_text_.clear();
        if (pm_slide_text_.size() < 6) pm_slide_text_ += char(c);
      }
      if (pm_slide_edit_ == pm_row_) {
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
          if (pm_slide_text_.empty()) pm_slide_edit_ = -1;
          else pm_slide_text_.pop_back();
        }
        if (in.accept) {
          if (!pm_slide_text_.empty()) {
            const size_t colon = pm_slide_text_.find(':');
            float v = float(std::atof(pm_slide_text_.c_str()));
            if (pm_row_ == kPmTime && colon != std::string::npos)
              v = float(std::atoi(pm_slide_text_.c_str())) + float(std::atoi(pm_slide_text_.c_str() + colon + 1)) / 60.0f;
            PmSlideSet(pm_row_, v, true);
            SavePrivateMatchChoices();
          }
          pm_slide_edit_ = -1, in.accept = false;
        }
        if (in.back) pm_slide_edit_ = -1, in.back = false;
        in.left = in.right = false;
      }
    }
    auto change = [&](int row, int step) {
      if (row == kPmTickets)
        pm_tickets_ = kPmTicketSteps[std::clamp(PmTicketAt(pm_tickets_) + step, 0, int(std::size(kPmTicketSteps)) - 1)];
      if (row == kPmTimeLimit) pm_time_limit_ = PmStep(kPmTimeSteps, int(std::size(kPmTimeSteps)), pm_time_limit_, step);
      if (row == kPmBots) pm_bots_ = std::clamp(pm_bots_ + step, 0, pm_size_);
      if (row == kPmType && kNightOps) {
        pm_event_ = (pm_event_ + 3 + step) % 3;
        REXLOG_INFO("BF private match: server type {}", kPmTypeNames[pm_event_]);
      }
      if (row == kPmSize) {
        const int count = int(std::size(bf1943::kServerSizes));
        int at = 0;
        for (int k = 0; k < count; ++k)
          if (bf1943::kServerSizes[k] == pm_size_) at = k;
        pm_size_ = bf1943::kServerSizes[(at + count + step) % count];
        pm_bots_ = std::min(pm_bots_, pm_size_);
        REXLOG_INFO("BF private match: server size {}", pm_size_);
      }
      if (row == kPmTick) {
        int at = 0;
        for (int k = 0; k < kPmTickCount; ++k)
          if (kPmTicks[k] == pm_tick_) at = k;
        pm_tick_ = kPmTicks[(at + kPmTickCount + step) % kPmTickCount];
      }
      if (row == kPmListed) pm_listed_ = !pm_listed_;
      if (row == kPmUpnp) pm_upnp_ = !pm_upnp_;
      if (row == kPmWeatherOn && pm_event_ == 0) {
        pm_weather_on_ = !pm_weather_on_;
        REXLOG_INFO("BF private match: Weather and time {}", pm_weather_on_ ? "on: the Weather tab" : "off: the map's usual afternoon");
      }
      if (row == kPmStorm) pm_storm_ = !pm_storm_;
      if (row == kPmStormType) pm_storm_type_ = (pm_storm_type_ + 2 + step) % 2;
      if (row == kPmDayNight) {
        pm_dn_ = !pm_dn_;
        if (pm_dn_ && pm_daylen_ == 0) pm_daylen_ = 24;  // the time passes: a day of 24 minutes unless set
      }
      if (row == kPmTime) pm_time_ = std::fmod(std::round(pm_time_ * 4.0f) / 4.0f + 24.0f + 0.25f * step, 24.0f);
      if (row == kPmDayLen && pm_dn_) pm_daylen_ = std::clamp(pm_daylen_ + 5 * step, 5, 240);
      if (row == kPmCloud) pm_cloud_ = std::clamp(pm_cloud_ + 5 * step, 0, 100);
      if (row == kPmRate) pm_rate_ = std::clamp(pm_rate_ <= 5 && step < 0 ? pm_rate_ - 1 : pm_rate_ < 5 ? pm_rate_ + 1 : pm_rate_ + 5 * step, 0, 100);
      if (row == kPmWeather) pm_weather_ = std::clamp(pm_weather_ + 5 * step, 0, 100), PmWeatherChanged();
      if (row == kPmFog) pm_fog_ = std::clamp(pm_fog_ + 5 * step, 0, 100);
      if (row == kPmGroundFog) pm_ground_fog_ = std::clamp(pm_ground_fog_ + 5 * step, 0, 100);
      if (row == kPmWind) pm_wind_ = std::clamp(pm_wind_ + 5 * step, 0, 100), PmWindChanged();
      if (row == kPmCloudHeight) pm_cloud_height_ = std::clamp(pm_cloud_height_ + 50 * step, 100, 1500);
      SavePrivateMatchChoices();
    };
    if (pm_col_ == 0 && in.left) change(pm_row_, -1);
    if (pm_col_ == 0 && in.right) change(pm_row_, 1);

    // ---- the levels and the rotation from the pad
    auto set_mode = [&](int mode) {
      if (mode == pm_mode_) return;
      pm_mode_ = mode;
      PmFitOrder();
      n = int(pm_order_.size());
      REXLOG_INFO("BF private match: game mode {}; rotation {}", kPmModeNames[pm_mode_], PmHeadline());
      SavePrivateMatchChoices();
    };
    auto toggle_level = [&](int m) {
      if (!PmMapInMode(pm_mode_, m)) return;
      const auto it = std::find(pm_order_.begin(), pm_order_.end(), m);
      if (it != pm_order_.end()) {
        if (n <= 1) return;  // the rotation keeps one map
        pm_order_.erase(it);
      } else {
        pm_order_.push_back(m);
      }
      n = int(pm_order_.size());
      REXLOG_INFO("BF private match: {} {} the rotation ({})", kPmMapTitles[m], PmInOrder(m) ? "added to" : "taken out of", PmHeadline());
      SavePrivateMatchChoices();
    };
    auto move_in_order = [&](int step) {
      const int to = pm_rot_row_ + step;
      if (pm_rot_row_ >= n || to < 0 || to >= n) return;
      std::swap(pm_order_[size_t(pm_rot_row_)], pm_order_[size_t(to)]);
      pm_rot_row_ = to;
      REXLOG_INFO("BF private match: {} moved to place {} of the rotation", kPmMapTitles[pm_order_[size_t(to)]], to + 1);
      SavePrivateMatchChoices();
    };
    if (pm_col_ == 1) {
      if (in.up) pm_lv_row_ = pm_lv_row_ <= -1 ? 3 : pm_lv_row_ - 1;
      if (in.down) pm_lv_row_ = pm_lv_row_ >= 3 ? -1 : pm_lv_row_ + 1;
      if (pm_lv_row_ < 0 && (in.left || in.right)) set_mode((pm_mode_ + (in.right ? 1 : 2)) % 3);
      if (in.accept) {
        if (pm_lv_row_ >= 0) toggle_level(pm_lv_row_);
        else set_mode((pm_mode_ + 1) % 3);
        in.accept = false;
      }
    } else if (pm_col_ == 2) {
      if (pm_grab_) {
        if (in.up) move_in_order(-1);
        if (in.down) move_in_order(1);
        if (in.accept || in.back) pm_grab_ = false, in.accept = in.back = false;
      } else {
        if (in.up) pm_rot_row_ = (pm_rot_row_ + n + 1) % (n + 2);
        if (in.down) pm_rot_row_ = (pm_rot_row_ + 1) % (n + 2);
        if (in.x && pm_rot_row_ < n && n > 1) {
          toggle_level(pm_order_[size_t(pm_rot_row_)]);
          pm_rot_row_ = std::min(pm_rot_row_, n - 1);
        }
        if (in.accept) {
          in.accept = false;
          if (pm_rot_row_ < n) pm_grab_ = true;
          else {
            host(pm_rot_row_ == n + 1);
            return;
          }
        }
      }
    }

    // ---- the settings column: its tabs, then its rows
    d->AddRectFilled(la, lb, rail);
    d->AddRect(la, lb, edge, 0, 0, 1.0f);
    const float tab_h = st(47);
    {
      const float bump = st(43);
      const float tw = (lb.x - la.x - bump * 2) * 0.5f;
      d->AddRectFilled(ImVec2(la.x, la.y + tab_h - 1), ImVec2(lb.x, la.y + tab_h), edge);
      if (PmWeatherTab()) {
        kit::Glyph(d, PromptStyle(), kit::Button::kLB, "Q", ImVec2(la.x + st(9), la.y + tab_h * 0.5f), st(17));
        kit::Glyph(d, PromptStyle(), kit::Button::kRB, "E", ImVec2(lb.x - bump + st(9), la.y + tab_h * 0.5f), st(17));
      }
      for (int t = 0; t < (PmWeatherTab() ? 2 : 1); ++t) {
        const ImVec2 t0(la.x + bump + tw * t, la.y), t1(t0.x + tw, la.y + tab_h);
        const bool on = pm_tab_ == t;
        const bool hover = ImGui::IsMouseHoveringRect(t0, t1);
        if (on) {
          kit::PaperArt(d, t0, t1);
          d->AddRectFilled(ImVec2(t0.x, t1.y - st(4)), t1, kit::kStamp);
        } else if (hover) {
          d->AddRectFilled(t0, t1, IM_COL32(241, 234, 211, 14));
        }
        kit::Spaced(d, f.typed, st(7.5f), ImVec2(t0.x + st(14), t0.y + st(20)), on ? kit::kInkSection : kit::kKhakiDim,
                    t ? "02" : "01", st(1.5f));
        kit::GameText(d, kit::GameFont::kHeadlineMedium, st(19), ImVec2(t0.x + st(36), t0.y + st(14)), on ? kit::kInk : kit::kCream,
                      t ? "WEATHER" : "MATCH");
        if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !editing && !on) set_tab(t);
      }
    }
    int clicked = -1;  // a settings row clicked (its change)
    const bool in_left = io.MousePos.x >= la.x && io.MousePos.x < lb.x && io.MousePos.y >= la.y + tab_h && io.MousePos.y < lb.y;
    const std::string caret = std::fmod(ImGui::GetTime(), 1.0) < 0.5 ? "_" : " ";
    const float right = lb.x - st(14);
    auto draw_switch = [&](bool on, float mid, bool sel, float at_right) {  // an on/off switch, ON or OFF said beside it
      const float sw = st(26), sh2 = st(13);
      const ImVec2 s0(at_right - sw, mid - sh2 * 0.5f), s1(at_right, mid + sh2 * 0.5f);
      d->AddRectFilled(s0, s1, on ? kit::kGreen : IM_COL32(120, 116, 100, 140), sh2 * 0.5f);
      d->AddCircleFilled(ImVec2(on ? s1.x - sh2 * 0.5f : s0.x + sh2 * 0.5f, mid), sh2 * 0.5f - st(2), kit::kCream, 20);
      const std::string said = on ? "ON" : "OFF";
      const float tw = kit::SpacedWidth(f.typed, st(7.5f), said, st(1.5f));
      kit::Spaced(d, f.typed, st(7.5f), ImVec2(s0.x - st(8) - tw, mid - st(5)), sel ? kit::kInkSection : kit::kKhakiDim, said, st(1.5f));
      return s0.x - st(8) - tw;
    };
    auto value_box = [&](const std::string& v, float x1, float mid, bool sel, bool off, bool typing) {  // a value in its box
      const float w = std::max(st(46), kit::SpacedWidth(f.typed, st(7.5f), v, st(1.5f)) + st(14));
      const ImVec2 b0(x1 - w, mid - st(9)), b1(x1, mid + st(9));
      d->AddRectFilled(b0, b1, typing ? IM_COL32(34, 36, 27, 30) : IM_COL32(0, 0, 0, sel ? 0 : 60));
      d->AddRect(b0, b1, typing ? kit::kStamp : off ? IM_COL32(120, 116, 100, 90) : sel ? kit::kInk : IM_COL32(241, 234, 211, 70), 0, 0, 1.0f);
      const float vw = kit::SpacedWidth(f.typed, st(7.5f), v, st(1.5f));
      kit::Spaced(d, f.typed, st(7.5f), ImVec2((b0.x + b1.x - vw) * 0.5f, mid - st(5)), off ? dim_ink : sel ? kit::kInk : kit::kKhaki, v, st(1.5f));
      return b0.x;
    };
    auto unit_text = [&](const std::string& u, float x1, float mid, bool sel) {
      const float uw = kit::SpacedWidth(f.typed, st(7), u, st(1.5f));
      kit::Spaced(d, f.typed, st(7), ImVec2(x1 - uw, mid - st(4.5f)), sel ? kit::kInkSection : kit::kKhakiDim, u, st(1.5f));
      return x1 - uw;
    };
    // A slider's track from x0 to x1 at `mid`: `share` of it filled; a click or a drag on it sets `row` (true while so).
    auto track = [&](int row, float share, float x0, float x1, float mid, bool sel, bool off) {
      d->AddRectFilled(ImVec2(x0, mid - st(1)), ImVec2(x1, mid + st(1)), sel ? IM_COL32(34, 36, 27, 70) : IM_COL32(241, 234, 211, 50));
      const float knob = x0 + (x1 - x0) * std::clamp(share, 0.0f, 1.0f);
      if (!off) d->AddRectFilled(ImVec2(x0, mid - st(1)), ImVec2(knob, mid + st(1)), sel ? kit::kStamp : kit::kKhakiDim);
      d->AddRectFilled(ImVec2(knob - st(2.5f), mid - st(6)), ImVec2(knob + st(2.5f), mid + st(6)), off ? dim_ink : sel ? kit::kInk : kit::kKhaki);
      const bool on_track = ImGui::IsMouseHoveringRect(ImVec2(x0 - st(6), mid - st(9)), ImVec2(x1 + st(6), mid + st(9)));
      if (!editing && !off && on_track && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) pm_drag_ = row, pm_col_ = 0, pm_row_ = row;
      return pm_drag_ == row && !off ? std::clamp((io.MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f) : -1.0f;
    };
    float y = la.y + tab_h;
    int number = 0;
    const std::string me = settings_.Get("player_name", "");
    auto row_frame = [&](int i, float h) {  // the row's place, its selection, its number; true when selected
      const ImVec2 r0(la.x, y), r1(lb.x, y + h);
      const bool hover = !editing && in_left && ImGui::IsMouseHoveringRect(r0, r1);
      if (hover && io.MouseDelta.x + io.MouseDelta.y != 0) pm_row_ = i, pm_col_ = 0;
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && pm_drag_ < 0) clicked = i, pm_col_ = 0;
      const bool sel = pm_row_ == i && pm_col_ == 0;
      if (sel) {
        kit::PaperArt(d, r0, r1);
        d->AddRectFilled(r0, ImVec2(r0.x + st(4), r1.y), kit::kStamp);
      } else if (pm_row_ == i) {
        d->AddRectFilled(r0, r1, IM_COL32(241, 234, 211, 12));  // the column's place while another column is used
      }
      d->AddRectFilled(ImVec2(r0.x, r1.y - 1), ImVec2(r1.x, r1.y), rule);
      return sel;
    };
    auto number_title = [&](int i, float title_mid, bool sel, float cap) {
      kit::Spaced(d, f.typed, st(7.5f), ImVec2(la.x + st(14), title_mid - st(4.5f)), sel ? kit::kInkSection : kit::kKhakiDim,
                  fmt::format("{:02d}", ++number), st(1.5f));
      kit::GameText(d, kit::GameFont::kHeadlineMedium, cap, ImVec2(la.x + st(38), title_mid - cap * 0.5f), sel ? kit::kInk : kit::kKhaki,
                    PmTitle(i));
    };
    if (pm_tab_ == 0) {
      // The plain rows share what the rail has left (27 Nov 2026: Server type and Server size made two more), at most
      // their old 37 and at least 26.
      float fixed = 0;
      int plain = 0;
      for (const int i : order) {
        if (i == kPmName || i == kPmPassText) fixed += 57;
        else if (i == kPmWeatherOn) fixed += 44;
        else if (i == kPmPort) fixed += 30;
        else ++plain;
      }
      const float plain_h = std::clamp((lb.y - la.y - tab_h - st(fixed)) / float(std::max(plain, 1)), st(26), st(37));
      for (const int i : order) {
        const float h = i == kPmName || i == kPmPassText ? st(57) : i == kPmWeatherOn ? st(44) : i == kPmPort ? st(30) : plain_h;
        const bool sel = row_frame(i, h);
        const float mid = y + (i == kPmName || i == kPmPassText ? st(15) : i == kPmWeatherOn ? st(16) : h * 0.5f);
        if (i == kPmPort) {
          kit::Spaced(d, f.typed, st(8), ImVec2(la.x + st(38), mid - st(5)), sel ? kit::kInk : kit::kKhaki, "PORT", st(2));
          value_box(pm_port_text_ + (sel ? caret : ""), right, mid, sel, false, false);
          y += h;
          continue;
        }
        number_title(i, mid, sel, st(i == kPmWeatherOn ? 16 : 15));
        if (i == kPmName || i == kPmPassText) {
          // The text's field, the row's width under the title.
          const ImVec2 f0(la.x + st(38), y + st(28)), f1(right, y + h - st(7));
          d->AddRectFilled(f0, f1, sel ? IM_COL32(34, 36, 27, 22) : IM_COL32(0, 0, 0, 70));
          d->AddRect(f0, f1, sel ? kit::kInk : IM_COL32(241, 234, 211, 70), 0, 0, 1.0f);
          std::string shown;
          bool hint = false;
          if (i == kPmName) {
            hint = pm_name_.empty() && !sel;
            shown = !pm_name_.empty() ? pm_name_ + (sel ? caret : "") : sel ? caret : me.empty() ? "Custom match" : me + "'s match";
          } else {
            hint = pm_pass_.empty() && !sel;
            shown = hint ? std::string("None: anybody can join") : PmPassShown(pm_pass_) + (sel ? caret : "");
          }
          d->PushClipRect(f0, f1, true);
          d->AddText(f.ui_bold, st(11), ImVec2(f0.x + st(8), f0.y + (f1.y - f0.y - st(11) * 1.45f) * 0.5f),
                     hint ? IM_COL32(241, 234, 211, 110) : sel ? kit::kInk : kit::kKhaki, shown.c_str());
          d->PopClipRect();
        } else if (i == kPmType || i == kPmSize) {
          std::string v = i == kPmType ? std::string(pm_event_ == 1 ? "NIGHT OPS \xC2\xB7 18:30" : pm_event_ == 2 ? "DAYBREAK \xC2\xB7 06:10" : "CLASSIC")
                                       : std::to_string(pm_size_) + (pm_size_ == 24 ? " \xC2\xB7 ORIGINAL" : pm_size_ == 32 ? " \xC2\xB7 DEFAULT" : "");
          if (sel) v = "<  " + v + "  >";
          const float vw = kit::SpacedWidth(f.typed, st(7.5f), v, st(1.5f));
          kit::Spaced(d, f.typed, st(7.5f), ImVec2(right - vw, mid - st(5)), sel ? kit::kInk : kit::kKhaki, v, st(1.5f));
          if (i == kPmSize) unit_text("PLAYERS", right - vw - st(12), mid, sel);
        } else if (i == kPmTickets) {
          const int own = pm_mode_ == 1 ? 100 : 130;
          const float u0 = unit_text(pm_tickets_ > 0 ? "A TEAM" : pm_tickets_ == 0 ? (pm_mode_ == 2 ? "AUTO \xC2\xB7 EACH MAP" : "AUTO \xC2\xB7 A TEAM")
                                                     : pm_mode_ == 2 ? "EACH MAP'S" : "MAP'S OWN", right, mid, sel);
          const float b0 = value_box(pm_tickets_ > 0 ? std::to_string(pm_tickets_)
                                     : pm_mode_ == 2 ? std::string(pm_tickets_ == 0 ? "AUTO" : "OWN")
                                     : std::to_string(pm_tickets_ == 0 ? PmAutoTickets(own) : own),
                                     u0 - st(8), mid, sel, false, false);
          const int at = PmTicketAt(pm_tickets_);
          const float s = track(i, float(at) / float(std::size(kPmTicketSteps) - 1), b0 - st(118), b0 - st(12), mid, sel, false);
          if (s >= 0) {
            const int pick = kPmTicketSteps[int(std::lround(s * float(std::size(kPmTicketSteps) - 1)))];
            if (pick != pm_tickets_) pm_tickets_ = pick, SavePrivateMatchChoices();
          }
        } else if (i == kPmTimeLimit) {
          value_box(pm_time_limit_ ? fmt::format("{} MIN", pm_time_limit_) : std::string("OFF"), right, mid, sel, false, false);
          if (!pm_time_limit_) unit_text("TICKETS ONLY", right - st(60), mid, sel);
        } else if (i == kPmBots) {
          const float u0 = unit_text(pm_bots_ ? fmt::format("{} A SIDE", (pm_bots_ + 1) / 2) : std::string("NONE"), right, mid, sel);
          const float b0 = value_box(std::to_string(pm_bots_), u0 - st(8), mid, sel, false, false);
          const float s = track(i, float(pm_bots_) / float(pm_size_), b0 - st(118), b0 - st(12), mid, sel, false);
          if (s >= 0 && int(std::lround(s * pm_size_)) != pm_bots_) pm_bots_ = int(std::lround(s * pm_size_)), SavePrivateMatchChoices();
        } else if (i == kPmTick) {
          std::string v = std::to_string(pm_tick_) + (pm_tick_ == 60 ? " HZ \xC2\xB7 DEFAULT" : " HZ \xC2\xB7 ORIGINAL");
          if (sel) v = "<  " + v + "  >";
          const float vw = kit::SpacedWidth(f.typed, st(7.5f), v, st(1.5f));
          kit::Spaced(d, f.typed, st(7.5f), ImVec2(right - vw, mid - st(5)), sel ? kit::kInk : kit::kKhaki, v, st(1.5f));
        } else if (i == kPmListed) {
          const float x = draw_switch(pm_listed_, mid, sel, right);
          unit_text("ONLINE ONLY", x - st(12), mid, sel);
        } else if (i == kPmUpnp) {
          const float b0 = value_box(pm_upnp_ ? "PORT AUTO" : "PORT BELOW", right, mid, sel, true, false);
          draw_switch(pm_upnp_, mid, sel, b0 - st(10));
        } else if (i == kPmWeatherOn) {
          kit::Spaced(d, f.typed, st(6.5f), ImVec2(la.x + st(38), mid + st(12)), sel ? kit::kInkSection : kit::kKhakiDim,
                      pm_event_ ? (pm_event_ == 1 ? "SET BY NIGHT OPS' SCHEDULE" : "SET BY DAYBREAK'S SCHEDULE")
                      : pm_weather_on_ ? "THE WEATHER TAB IS OPEN" : "ON ADDS A WEATHER TAB", st(1.5f));
          draw_switch(PmWeatherTab(), y + h * 0.5f, sel, right);
        }
        y += h;
      }
    } else {
      // The time of day: the sun's path over the horizon (golden by day, dotted under it by night), the sun riding it
      // and the moon below it at night; the hour large at the top left with Time passes under it; A day lasts at the
      // right. Three rows in one panel: Time of day, Time passes, A day lasts.
      const ImVec2 p0(la.x + st(10), y + st(8)), p1(lb.x - st(10), y + st(8) + st(126));
      const ImU32 panel = IM_COL32(27, 27, 24, 255);
      const bool dial_sel = pm_col_ == 0 && (pm_row_ == kPmTime || pm_row_ == kPmDayNight || pm_row_ == kPmDayLen);
      d->AddRectFilled(p0, p1, panel, st(9));
      d->AddRect(p0, p1, dial_sel ? kit::kGreen : IM_COL32(255, 255, 255, 34), st(9), 0, dial_sel ? st(2) : 1.0f);
      if (dial_sel) d->AddRectFilled(ImVec2(p0.x, p0.y + st(8)), ImVec2(p0.x + st(3.5f), p1.y - st(8)), kit::kStamp);
      // the hour
      const bool time_sel = pm_col_ == 0 && pm_row_ == kPmTime;
      const ImVec2 t0(p0.x + st(14), p0.y + st(8));
      const bool typing_time = pm_slide_edit_ == kPmTime;
      const std::string hour = typing_time ? pm_slide_text_ + caret : PmSlideText(kPmTime);
      d->AddText(f.ui_bold, st(30), t0, typing_time ? kit::kStamp : IM_COL32(244, 241, 230, 255), hour.c_str());
      const float hour_w = kit::TextWidth(f.ui_bold, st(30), hour);
      if (time_sel)
        kit::Spaced(d, f.typed, st(6.5f), ImVec2(t0.x + hour_w + st(12), t0.y + st(16)), kit::kKhakiDim, "<  >  OR TYPE IT", st(1.5f));
      // Time passes, under the hour
      {
        const bool sel = pm_col_ == 0 && pm_row_ == kPmDayNight;
        const ImVec2 s0(t0.x - st(4), p0.y + st(46)), s1(t0.x + st(160), p0.y + st(64));
        if (sel) d->AddRect(s0, s1, kit::kGreen, st(3), 0, st(1.5f));
        const float sw = st(24), sh = st(12), mid = (s0.y + s1.y) * 0.5f;
        const ImVec2 w0(t0.x, mid - sh * 0.5f), w1(t0.x + sw, mid + sh * 0.5f);
        d->AddRectFilled(w0, w1, pm_dn_ ? kit::kGreen : IM_COL32(120, 116, 100, 140), sh * 0.5f);
        d->AddCircleFilled(ImVec2(pm_dn_ ? w1.x - sh * 0.5f : w0.x + sh * 0.5f, mid), sh * 0.5f - st(2), kit::kCream, 20);
        kit::Spaced(d, f.typed, st(7.5f), ImVec2(w1.x + st(8), mid - st(5)), sel ? kit::kCream : kit::kKhaki,
                    pm_dn_ ? "TIME PASSES" : "TIME PASSES \xC2\xB7 OFF", st(1.5f));
        if (!editing && ImGui::IsMouseHoveringRect(s0, s1)) {
          if (io.MouseDelta.x + io.MouseDelta.y != 0) pm_row_ = kPmDayNight, pm_col_ = 0;
          if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = kPmDayNight, pm_col_ = 0;
        }
      }
      // A day lasts, at the right
      {
        const bool sel = pm_col_ == 0 && pm_row_ == kPmDayLen;
        const std::string cap = "A DAY LASTS";
        const float cw = kit::SpacedWidth(f.typed, st(6.5f), cap, st(1.5f));
        kit::Spaced(d, f.typed, st(6.5f), ImVec2(p1.x - st(14) - cw, p0.y + st(12)), kit::kKhakiDim, cap, st(1.5f));
        const bool typing = pm_slide_edit_ == kPmDayLen;
        const std::string v = typing ? pm_slide_text_ + caret : pm_dn_ ? PmSlideText(kPmDayLen) : std::string("STILL");
        const float vw = std::max(st(62), kit::SpacedWidth(f.typed, st(7.5f), v, st(1.5f)) + st(14));
        const ImVec2 b0(p1.x - st(14) - vw, p0.y + st(24)), b1(p1.x - st(14), p0.y + st(42));
        d->AddRect(b0, b1, typing ? kit::kStamp : sel ? kit::kGreen : IM_COL32(241, 234, 211, pm_dn_ ? 90 : 40), 0, 0, sel ? st(1.5f) : 1.0f);
        const float tw = kit::SpacedWidth(f.typed, st(7.5f), v, st(1.5f));
        kit::Spaced(d, f.typed, st(7.5f), ImVec2((b0.x + b1.x - tw) * 0.5f, b0.y + st(4)), pm_dn_ || typing ? kit::kKhaki : dim_ink, v, st(1.5f));
        if (!editing && ImGui::IsMouseHoveringRect(ImVec2(b0.x, p0.y + st(8)), b1)) {
          if (io.MouseDelta.x + io.MouseDelta.y != 0) pm_row_ = kPmDayLen, pm_col_ = 0;
          if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && pm_dn_) pm_row_ = kPmDayLen, pm_col_ = 0, pm_slide_edit_ = kPmDayLen, pm_slide_text_.clear();
        }
      }
      // the sun's path
      {
        const float x0 = p0.x + st(18), x1 = p1.x - st(18), hy = p0.y + st(94), amp = st(22);
        auto at = [&](float t) { return ImVec2(x0 + (x1 - x0) * t / 24.0f, hy - amp * std::sin(3.14159265f * (t - 6.0f) / 12.0f)); };
        d->AddLine(ImVec2(p0.x + st(10), hy), ImVec2(p1.x - st(10), hy), IM_COL32(255, 255, 255, 70), st(1));
        for (int h = 0; h <= 24; h += 6) {
          const float x = at(float(h)).x;
          d->AddLine(ImVec2(x, hy - st(3)), ImVec2(x, hy + st(3)), IM_COL32(255, 255, 255, 90), st(1));
          const std::string label = fmt::format("{:02d}", h);
          const float lw = kit::TextWidth(f.typed, st(7), label);
          d->AddText(f.typed, st(7), ImVec2(x - lw * 0.5f, p1.y - st(13)), IM_COL32(241, 234, 211, 90), label.c_str());
        }
        for (float t = 0; t <= 24.001f; t += 0.4f)  // the night: dots under the horizon
          if (t < 6 || t > 18) d->AddCircleFilled(at(t), st(0.9f), IM_COL32(241, 234, 211, 120), 6);
        d->PathClear();  // the day: the arc's glow, the arc, the light under it
        for (float t = 6; t <= 18.001f; t += 0.2f) d->PathLineTo(at(t));
        d->PathFillConvex(IM_COL32(227, 169, 79, 40));
        for (float t = 6; t <= 18.001f; t += 0.2f) d->PathLineTo(at(t));
        d->PathStroke(IM_COL32(227, 169, 79, 70), 0, st(4));
        for (float t = 6; t <= 18.001f; t += 0.2f) d->PathLineTo(at(t));
        d->PathStroke(IM_COL32(239, 184, 92, 255), 0, st(1.6f));
        const ImVec2 c = at(pm_time_);
        if (pm_time_ >= 6 && pm_time_ <= 18) {  // the sun
          d->AddCircleFilled(c, st(13), IM_COL32(255, 212, 94, 40), 24);
          d->AddCircleFilled(c, st(9.5f), IM_COL32(255, 212, 94, 70), 24);
          for (int k = 0; k < 8; ++k) {
            const float a = 3.14159265f * 0.25f * float(k);
            d->AddLine(ImVec2(c.x + std::cos(a) * st(8.5f), c.y + std::sin(a) * st(8.5f)),
                       ImVec2(c.x + std::cos(a) * st(12), c.y + std::sin(a) * st(12)), IM_COL32(255, 212, 94, 255), st(1.6f));
          }
          d->AddCircleFilled(c, st(6), IM_COL32(255, 212, 94, 255), 24);
          d->AddCircle(c, st(6), IM_COL32(255, 243, 196, 255), 24, st(1));
        } else {  // the moon: a crescent (its disc, then the panel's colour over most of it)
          d->AddCircleFilled(c, st(11), IM_COL32(207, 216, 255, 34), 24);
          d->AddCircleFilled(c, st(6.5f), IM_COL32(244, 244, 240, 255), 24);
          d->AddCircleFilled(ImVec2(c.x + st(3.2f), c.y - st(2.4f)), st(5.6f), panel, 24);
        }
        // the mouse: a click or a drag over the path sets the time (a quarter hour's steps)
        const ImVec2 a0(x0 - st(8), hy - amp - st(12)), a1(x1 + st(8), hy + amp + st(8));
        if (!editing && ImGui::IsMouseHoveringRect(a0, a1)) {
          if (io.MouseDelta.x + io.MouseDelta.y != 0 && !pm_dial_drag_) pm_row_ = kPmTime, pm_col_ = 0;
          if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) pm_dial_drag_ = true, pm_row_ = kPmTime, pm_col_ = 0;
        }
        if (pm_dial_drag_) {
          const float t = std::round(std::clamp((io.MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f) * 96.0f) / 4.0f;
          if (std::fabs(t - pm_time_) > 0.01f) pm_time_ = std::min(t, 23.75f), SavePrivateMatchChoices();
        }
        if (!editing && ImGui::IsMouseHoveringRect(p0, ImVec2(p1.x - st(90), p0.y + st(42))) && io.MouseDelta.x + io.MouseDelta.y != 0)
          pm_row_ = kPmTime, pm_col_ = 0;
      }
      y = p1.y + st(8);
      number = 1;
      for (const int i : order) {
        if (i == kPmTime || i == kPmDayNight || i == kPmDayLen) continue;
        const float h = st(31);  // (nine rows under the dial since the storm's type, D224: 35 each ran past the column)
        const bool sel = row_frame(i, h);
        const float mid = y + h * 0.5f;
        number_title(i, mid, sel, st(14));
        if (i == kPmStorm) {
          draw_switch(pm_storm_, mid, sel, right);
        } else if (i == kPmStormType) {
          std::string v = kPmStormTypeNames[pm_storm_type_];
          if (sel) v = "<  " + v + "  >";
          const float vw = kit::SpacedWidth(f.typed, st(7.5f), v, st(1.5f));
          kit::Spaced(d, f.typed, st(7.5f), ImVec2(right - vw, mid - st(5)), sel ? kit::kInk : kit::kKhaki, v, st(1.5f));
        } else {
          const PmSlide s = PmSlideOf(i);
          const bool typing = pm_slide_edit_ == i;
          const float b0 = value_box(typing ? pm_slide_text_ + caret : PmSlideText(i), right, mid, sel, false, typing);
          const float tx1 = b0 - st(12), tx0 = b0 - st(118);
          if (i == kPmWind && !pm_wind_manual_) unit_text("FOLLOWS WEATHER", tx0 - st(10), mid, sel);
          const float share = (PmSlideGet(i) - s.lo) / (s.hi - s.lo);
          const float v = track(i, share, tx0, tx1, mid, sel, false);
          if (v >= 0) {
            const float before = PmSlideGet(i);
            PmSlideSet(i, s.lo + (s.hi - s.lo) * v, false);
            if (PmSlideGet(i) != before) SavePrivateMatchChoices();
          }
          if (!editing && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(ImVec2(b0, mid - st(9)), ImVec2(right, mid + st(9))))
            pm_row_ = i, pm_col_ = 0, pm_slide_edit_ = i, pm_slide_text_.clear(), clicked = -2;
        }
        y += h;
      }
    }

    // ---- the middle: the game mode and the levels
    d->AddRectFilled(va, vb, rail);
    d->AddRect(va, vb, edge, 0, 0, 1.0f);
    const bool in_mid = io.MousePos.x >= va.x && io.MousePos.x < vb.x && io.MousePos.y >= va.y && io.MousePos.y < vb.y;
    {
      const float sx0 = va.x + st(10), sx1 = vb.x - st(10), sy0 = va.y + st(9), sy1 = va.y + st(38);
      const float sw = (sx1 - sx0) / 3.0f;
      for (int k = 0; k < 3; ++k) {
        const ImVec2 s0(sx0 + sw * k, sy0), s1(sx0 + sw * (k + 1), sy1);
        const bool on = pm_mode_ == k;
        if (on) {
          d->AddRectFilled(s0, s1, kit::kGreen);
          d->AddRectFilled(ImVec2(s0.x, s1.y - st(3)), s1, kit::kGreenShadow);
        }
        d->AddRect(s0, s1, IM_COL32(241, 234, 211, on ? 0 : 50), 0, 0, 1.0f);
        const std::string name = kPmModeNames[k];
        const float nw = kit::GameTextWidth(kit::GameFont::kText, st(10.5f), name, st(0.5f));
        kit::GameText(d, kit::GameFont::kText, st(10.5f), ImVec2((s0.x + s1.x - nw) * 0.5f, s0.y + st(9)), on ? kit::kCream : kit::kKhaki, name,
                      st(0.5f));
        const bool hover = !editing && ImGui::IsMouseHoveringRect(s0, s1);
        if (hover && io.MouseDelta.x + io.MouseDelta.y != 0) pm_col_ = 1, pm_lv_row_ = -1;
        if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) pm_col_ = 1, pm_lv_row_ = -1, set_mode(k);
      }
      if (pm_col_ == 1 && pm_lv_row_ < 0) d->AddRect(ImVec2(sx0 - st(3), sy0 - st(3)), ImVec2(sx1 + st(3), sy1 + st(3)), kit::kCream, 0, 0, st(1.5f));
    }
    for (int m = 0; m < 4; ++m) {
      const float h = st(50);
      const ImVec2 r0(va.x, va.y + st(47) + h * m), r1(vb.x, r0.y + h);
      const bool hover = !editing && in_mid && ImGui::IsMouseHoveringRect(r0, r1);
      if (hover && io.MouseDelta.x + io.MouseDelta.y != 0) pm_col_ = 1, pm_lv_row_ = m;
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) pm_col_ = 1, pm_lv_row_ = m, toggle_level(m);
      const bool sel = pm_col_ == 1 && pm_lv_row_ == m;
      const bool fits = PmMapInMode(pm_mode_, m), in_rot = PmInOrder(m);
      if (sel) {
        kit::PaperArt(d, r0, r1);
        d->AddRectFilled(r0, ImVec2(r0.x + st(4), r1.y), kit::kStamp);
      }
      d->AddRectFilled(ImVec2(r0.x, r0.y - 1), ImVec2(r1.x, r0.y), rule);
      const ImVec2 t0(r0.x + st(12), r0.y + st(8)), t1(t0.x + st(68), r1.y - st(8));
      MapPicture(d, st.u * 0.5f, t0, t1, kPmMapTitles[m]);
      if (!fits) d->AddRectFilled(t0, t1, IM_COL32(13, 15, 11, 160));
      const ImU32 ink = !fits ? dim_ink : sel ? kit::kInk : kit::kCream;
      kit::GameText(d, kit::GameFont::kHeadlineMedium, st(16), ImVec2(t1.x + st(12), r0.y + st(10)), ink, kPmMapNames[m]);
      kit::Spaced(d, f.typed, st(6.5f), ImVec2(t1.x + st(12), r0.y + st(33)), !fits ? dim_ink : sel ? kit::kInkSection : kit::kKhakiDim,
                  m == 3 ? "AIR SUPERIORITY" : "CONQUEST", st(1.5f));
      std::string end;
      if (!fits) end = m == 3 ? "AIR SUP. ONLY" : "CONQUEST ONLY";
      else if (in_rot) end = fmt::format("IN THE ROTATION \xC2\xB7 {}", int(std::find(pm_order_.begin(), pm_order_.end(), m) - pm_order_.begin()) + 1);
      else if (sel) end = "ADD";
      const float ew = kit::SpacedWidth(f.typed, st(7), end, st(1.5f));
      const float mid = (r0.y + r1.y) * 0.5f;
      kit::Spaced(d, f.typed, st(7), ImVec2(r1.x - st(14) - ew, mid - st(4.5f)), !fits ? dim_ink : sel ? kit::kInkSection : kit::kKhaki, end,
                  st(1.5f));
      if (sel && fits && !in_rot) kit::Glyph(d, PromptStyle(), kit::Button::kA, "Enter", ImVec2(r1.x - st(22) - ew - st(18), mid), st(16));
    }

    // ---- the right: the rotation in its order, Host online and Host on LAN
    d->AddRectFilled(ra, rb, rail);
    d->AddRect(ra, rb, edge, 0, 0, 1.0f);
    kit::GameText(d, kit::GameFont::kHeadline, st(23), ImVec2(ra.x + st(16), ra.y + st(14)), kit::kCream, "ROTATION");
    {
      const std::string cap = "IN TURN, THEN AGAIN";
      const float cw = kit::SpacedWidth(f.typed, st(7.5f), cap, st(2));
      kit::Spaced(d, f.typed, st(7.5f), ImVec2(rb.x - st(16) - cw, ra.y + st(22)), kit::kKhakiDim, cap, st(2));
    }
    d->AddRectFilled(ImVec2(ra.x, ra.y + st(50)), ImVec2(rb.x, ra.y + st(51)), edge);
    const bool in_right = io.MousePos.x >= ra.x && io.MousePos.x < rb.x && io.MousePos.y >= ra.y && io.MousePos.y < rb.y;
    for (int k = 0; k < n; ++k) {
      const int m = pm_order_[size_t(k)];
      const float h = st(47);
      const ImVec2 r0(ra.x, ra.y + st(51) + h * k), r1(rb.x, r0.y + h);
      const bool sel = pm_col_ == 2 && pm_rot_row_ == k;
      // the mouse: ▲ and ▼ on the chosen row move its map; elsewhere a click chooses the row
      const ImVec2 up0(r1.x - st(52), r0.y + st(12)), up1(up0.x + st(18), r1.y - st(12));
      const ImVec2 dn0(r1.x - st(30), r0.y + st(12)), dn1(dn0.x + st(18), r1.y - st(12));
      const bool hover = !editing && in_right && ImGui::IsMouseHoveringRect(r0, r1);
      if (hover && io.MouseDelta.x + io.MouseDelta.y != 0 && !pm_grab_) pm_col_ = 2, pm_rot_row_ = k;
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (sel && ImGui::IsMouseHoveringRect(up0, up1)) move_in_order(-1);
        else if (sel && ImGui::IsMouseHoveringRect(dn0, dn1)) move_in_order(1);
        else pm_col_ = 2, pm_rot_row_ = k;
      }
      if (sel) {
        kit::PaperArt(d, r0, r1);
        d->AddRectFilled(r0, ImVec2(r0.x + st(4), r1.y), kit::kStamp);
        if (pm_grab_) d->AddRect(ImVec2(r0.x + 1, r0.y + 1), ImVec2(r1.x - 1, r1.y - 1), kit::kGreen, 0, 0, st(2));
      }
      d->AddRectFilled(ImVec2(r0.x, r1.y - 1), ImVec2(r1.x, r1.y), rule);
      const ImVec2 t0(r0.x + st(14), r0.y + st(8)), t1(t0.x + st(62), r1.y - st(8));
      MapPicture(d, st.u * 0.5f, t0, t1, kPmMapTitles[m]);
      d->AddRectFilled(ImVec2(t0.x - st(5), t0.y - st(5)), ImVec2(t0.x + st(9), t0.y + st(9)), kit::kStamp);
      const std::string num = std::to_string(k + 1);
      d->AddText(f.ui_bold, st(10), ImVec2(t0.x - st(5) + (st(14) - kit::TextWidth(f.ui_bold, st(10), num)) * 0.5f, t0.y - st(5) + st(0.5f)),
                 IM_COL32(255, 255, 255, 255), num.c_str());
      kit::GameText(d, kit::GameFont::kHeadlineMedium, st(15), ImVec2(t1.x + st(12), r0.y + st(9)), sel ? kit::kInk : kit::kCream, kPmMapNames[m]);
      kit::Spaced(d, f.typed, st(6.5f), ImVec2(t1.x + st(12), r0.y + st(30)), sel ? kit::kInkSection : kit::kKhakiDim,
                  m == 3 ? "AIR SUPERIORITY" : "CONQUEST", st(1.5f));
      if (sel) {  // ▲ ▼ for the mouse, and what A does
        for (int a = 0; a < 2; ++a) {
          const ImVec2 b0 = a ? dn0 : up0, b1 = a ? dn1 : up1;
          const bool can = a ? k < n - 1 : k > 0;
          const bool over = ImGui::IsMouseHoveringRect(b0, b1);
          d->AddRect(b0, b1, IM_COL32(34, 36, 27, can ? (over ? 255 : 140) : 40), 0, 0, 1.0f);
          const ImVec2 c((b0.x + b1.x) * 0.5f, (b0.y + b1.y) * 0.5f);
          const float s = st(4);
          if (a) d->AddTriangleFilled(ImVec2(c.x - s, c.y - s * 0.6f), ImVec2(c.x + s, c.y - s * 0.6f), ImVec2(c.x, c.y + s * 0.8f), IM_COL32(34, 36, 27, can ? 220 : 50));
          else d->AddTriangleFilled(ImVec2(c.x - s, c.y + s * 0.6f), ImVec2(c.x + s, c.y + s * 0.6f), ImVec2(c.x, c.y - s * 0.8f), IM_COL32(34, 36, 27, can ? 220 : 50));
        }
        const std::string said = pm_grab_ ? "MOVING" : "";
        if (!said.empty()) {
          const float w = kit::SpacedWidth(f.typed, st(7), said, st(1.5f));
          kit::Spaced(d, f.typed, st(7), ImVec2(up0.x - st(10) - w, (r0.y + r1.y) * 0.5f - st(4.5f)), kit::kGreen, said, st(1.5f));
        }
      }
    }
    kit::Wrapped(d, f.ui, st(10.5f), ImVec2(ra.x + st(16), rb.y - st(36)), kit::kKhakiDim,
                 n == 1 ? "One map: it plays round after round. Add more from the Levels to play them in turn."
                        : "The match plays these top to bottom, the next after each round, then again from the top.",
                 rb.x - ra.x - st(32));
    {  // Host online, Host on LAN
      const bool sel_o = pm_col_ == 2 && pm_rot_row_ == n, sel_l = pm_col_ == 2 && pm_rot_row_ == n + 1;
      if (GreenButton(d, st, o0, o1, kit::Button::kStart, "HOST ONLINE") && !editing) {
        host(false);
        return;
      }
      if (sel_o) d->AddRect(ImVec2(o0.x - st(3), o0.y - st(3)), ImVec2(o1.x + st(3), o1.y + st(6)), kit::kCream, 0, 0, st(1.5f));
      const bool over = ImGui::IsMouseHoveringRect(h0, h1);
      d->AddRectFilled(h0, h1, over ? IM_COL32(34, 36, 30, 240) : IM_COL32(20, 20, 18, 235));
      d->AddRect(h0, h1, sel_l ? kit::kCream : IM_COL32(241, 234, 211, 40), 0, 0, sel_l ? st(1.5f) : 1.0f);
      const std::string lan = "HOST ON LAN - NO INTERNET";  // (the game's lettering has no middle dot)
      const float cap = st(13), lw = kit::GameTextWidth(kit::GameFont::kText, cap, lan, st(1));
      kit::GameText(d, kit::GameFont::kText, cap, ImVec2((h0.x + h1.x - lw) * 0.5f, h0.y + (h1.y - h0.y - cap) * 0.5f), kit::kStripText, lan, st(1));
      if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !editing) {
        host(true);
        return;
      }
    }

    // ---- the help along the foot: the chosen row's, or the error of the last try to host
    {
      kit::PaperArt(d, ba, bb);
      std::string title, text;
      if (pm_col_ == 0) {
        title = PmTitle(pm_row_), text = PmAbout(pm_row_);
      } else if (pm_col_ == 1) {
        if (pm_lv_row_ < 0) {
          title = "GAME MODE";
          text = "The kind of match. Each map has its own in the game: Conquest on Wake Island, Guadalcanal and Iwo Jima (take and hold the "
                 "flags), Air Superiority on Coral Sea (fighters only). All modes: any of the four in one rotation.";
        } else {
          const int m = pm_lv_row_;
          title = kPmMapNames[m];
          text = !PmMapInMode(pm_mode_, m) ? fmt::format("{}'s level holds {} only: choose that game mode, or All modes, to play it.",
                                                         kPmMapTitles[m], m == 3 ? "Air Superiority" : "Conquest")
                 : PmInOrder(m) ? fmt::format("{} plays {} in the rotation. A takes it out (the rotation keeps one map at least).",
                                              kPmMapTitles[m], int(std::find(pm_order_.begin(), pm_order_.end(), m) - pm_order_.begin()) + 1)
                                : fmt::format("{}: {}. A puts it at the end of the rotation; the match plays its maps in turn, the next "
                                              "one after each round.", kPmMapTitles[m], m == 3 ? "Air Superiority" : "Conquest");
        }
      } else if (pm_rot_row_ < n) {
        title = kPmMapNames[pm_order_[size_t(pm_rot_row_)]];
        text = pm_grab_ ? "Up and down move it in the order; A or B puts it down."
                        : "A picks it up to move it in the order (or the arrows with the mouse); X (F) takes it out.";
      } else if (pm_rot_row_ == n) {
        title = "HOST ONLINE";
        text = "Your PC runs the match's server while you play, and you join it. Friends join from the server browser, or with Direct "
               "connect: the pause copies the address. The match ends when you leave it.";
      } else {
        title = "HOST ON LAN";
        text = "For the PCs of your own network, with no internet needed: your PC answers the sign-in and the match list itself, and "
               "the match is on their browser's LAN tab.";
      }
      kit::Spaced(d, f.typed, st(7.5f), ImVec2(ba.x + st(16), ba.y + st(9)), kit::kInkSection, "THE CHOSEN ROW", st(2));
      kit::GameText(d, kit::GameFont::kHeadlineMedium, st(17), ImVec2(ba.x + st(16), ba.y + st(24)), kit::kInk, title);
      const bool error = !pm_error_.empty();
      const float tx = ba.x + st(250);
      const float wrap = bb.x - tx - st(18);
      const std::string shown = error ? pm_error_ : text;
      const float th = kit::WrappedHeight(f.ui, st(12), shown, wrap);
      kit::Wrapped(d, f.ui, st(12), ImVec2(tx, ba.y + std::max(st(6), (bb.y - ba.y - th) * 0.5f)), error ? kit::kRed : kit::kInkFaint, shown, wrap);
    }

    // ---- the on screen keys over the middle and the right while the name or the password is typed
    if (pm_edit_ >= 0) PrivateMatchKeys(d, st, pa, ImVec2(rb.x, vb.y), ekeys);
    // A (not the keyboard's Enter) on the name, the password or the port opens the on screen keys.
    if (pm_col_ == 0 && pm_tab_ == 0 && in.accept && !accept_from_key_ &&
        (pm_row_ == kPmName || pm_row_ == kPmPassText || (pm_row_ == kPmPort && !pm_upnp_))) {
      PmEditOpen(pm_row_);
      in.accept = false;
    }
    // Enter on a slider's row opens its box for typing (left and right step it; a controller's A steps it too).
    if (pm_col_ == 0 && in.accept && accept_from_key_ && pm_tab_ == 1 && PmSlideRow(pm_row_) && !(pm_row_ == kPmDayLen && !pm_dn_)) {
      pm_slide_edit_ = pm_row_, pm_slide_text_.clear(), in.accept = false;
    }
    if (clicked >= 0 && clicked == pm_drag_) clicked = -1;  // the click that started a slider's drag
    if (clicked >= 0 && (PmSlideRow(clicked) || clicked == kPmName || clicked == kPmPassText || clicked == kPmPort)) clicked = -1;
    if (pm_col_ == 0 && in.accept && pm_row_ != kPmName && pm_row_ != kPmPassText && pm_row_ != kPmPort) clicked = pm_row_;
    if (clicked >= 0 && !editing) {
      pm_row_ = clicked, pm_col_ = 0;
      change(clicked, 1);
    }
    if (in.back) {
      if (pm_col_ != 0) go_col(0);   // the levels or the rotation: back to the settings (it used to leave the page)
      else if (pm_tab_ == 1) set_tab(0);  // the Weather tab: back to Match
      else BackToMenu(kRowPrivateMatch, MainRowOf("CUSTOM MATCH")), pm_error_.clear();  // back to the menu
    }
  }

  // The on screen keys over the map's picture (a..b) while the server name or the password is edited: its field with
  // the copy being typed (the password in stars unless shown), the keys under it. `k`: this frame's presses.
  void PrivateMatchKeys(ImDrawList* d, const Stage& st, ImVec2 a, ImVec2 b, const Input& k) {
    const kit::Fonts& f = kit::GetFonts();
    const bool name = pm_edit_ == kPmName, port = pm_edit_ == kPmPort;
    d->AddRectFilled(a, b, IM_COL32(13, 15, 11, 255));  // opaque: the picture's chip showed through the heading
    d->AddRect(a, b, IM_COL32(241, 234, 211, 30), 0, 0, 1.0f);
    const float x = a.x + st(30), w = b.x - a.x - st(60);
    kit::GameText(d, kit::GameFont::kHeadlineMedium, st(19), ImVec2(x, a.y + st(18)), kit::kCream,
                  name ? "SERVER NAME" : port ? "PORT" : "PASSWORD");
    const ImVec2 n0(x, a.y + st(48)), n1(x + w, a.y + st(92));
    d->AddRectFilled(n0, n1, kit::kPaperLight);
    d->AddRect(n0, n1, kit::kGreen, 0, 0, st(3));
    const std::string shown = name || port ? pm_edit_text_ : PmPassShown(pm_edit_text_);
    const std::string count = std::to_string(pm_edit_text_.size()) + " / " + std::to_string(PmMost(pm_edit_));
    const float count_w = kit::TextWidth(f.typed, st(12), count);
    d->PushClipRect(n0, ImVec2(n1.x - count_w - st(24), n1.y), true);
    d->AddText(f.typed, st(22), ImVec2(x + st(12), n0.y + st(9)), kit::kInk, shown.c_str());
    if (std::fmod(ImGui::GetTime(), 1.0) < 0.6) {
      const float cx = x + st(14) + kit::TextWidth(f.typed, st(22), shown);
      d->AddRectFilled(ImVec2(cx, n0.y + st(9)), ImVec2(cx + st(2.5f), n1.y - st(9)), kit::kInk);
    }
    d->PopClipRect();
    d->AddText(f.typed, st(12), ImVec2(n1.x - st(12) - count_w, n0.y + st(15)), kit::kInkFaint, count.c_str());
    kit::Spaced(d, f.typed, st(8), ImVec2(x, n1.y + st(9)), kit::kKhakiDim,
                name   ? "UP TO 32 LETTERS, DIGITS, SPACES OR SIGNS. LEFT EMPTY, IT IS YOUR NAME'S MATCH."
                : port ? "1024 TO 65535, THE DIGITS ONLY. 18300 UNLESS YOU WANT ANOTHER."
                : pm_pass_shown_ ? "UP TO 24 LETTERS, DIGITS OR SIGNS, NO SPACES. Y HIDES IT."
                                      : "UP TO 24 LETTERS, DIGITS OR SIGNS, NO SPACES. Y SHOWS IT.",
                st(1.5f));
    // The keys, as the soldier name's: their width is ten keys (36 each, 4.5 apart).
    const ImVec2 k0((a.x + b.x) * 0.5f - st(200), a.y + st(126));
    Input keys = k;
    if (k.start) keys.accept = false;  // Start is A too on this page: here it is Done only
    const int clicked = OnScreenKeys(d, st, k0, keys, pm_edit_key_, true, pm_edit_caps_, name,
                                     "With a keyboard, just type: the keys close and the row takes it.");
    std::string& text = pm_edit_text_;
    if (clicked >= 0) {
      const OnKey& key = OnKeys(name)[size_t(clicked)];
      if (key.kind != 0 || name || port) REXLOG_INFO("BF private match: on screen key {}", key.label);  // not the password's
      if (key.kind == 0) {
        char c = key.ch;
        if (std::isalpha(static_cast<unsigned char>(c)))
          c = pm_edit_caps_ ? char(std::toupper(static_cast<unsigned char>(c))) : char(std::tolower(static_cast<unsigned char>(c)));
        if (PmCharOk(pm_edit_, static_cast<unsigned char>(c)) && text.size() < PmMost(pm_edit_)) text += c;
        // The name's first letter a capital, then small ones (as the soldier name's keys).
        if (name && std::isalpha(static_cast<unsigned char>(c)) && pm_edit_caps_ && text.size() == 1) pm_edit_caps_ = false;
      } else if (key.kind == 1) {
        if (!text.empty()) text.pop_back();
      } else if (key.kind == 2) {
        pm_edit_caps_ = !pm_edit_caps_;
      } else {
        PmEditClose(true);
        return;
      }
    }
    if (k.x && !text.empty()) text.pop_back();
    if (k.y && !name) pm_pass_shown_ = !pm_pass_shown_;
    if (k.start) PmEditClose(true);
    else if (k.back) PmEditClose(false);
  }

  // While our server starts: its steps in the rail (ra..rb under the heading), the wait on the band (band..hb), then
  // the join (PrivateMatchTick); B ends it.
  void PrivateMatchStarting(ImDrawList* d, const Stage& st, Input& in, ImVec2 ra, ImVec2 rb, ImVec2 band, ImVec2 hb) {
    const kit::Fonts& f = kit::GetFonts();
    const bf1943::PrivateMatchStatus& status = pm_status_;
    const double t = ImGui::GetTime() - pm_started_at_;
    std::string error;
    if (!status.running && t > 3) error = "Your server stopped. Its log: user\\private_match\\server.log.";
    else if (status.state == "no backend" && pm_lan_)
      error = "The LAN match could not start its own services: another program on this PC already uses TCP 18241, 18246 "
              "or 18250 (another match, or a test backend: the test launcher runs one). Close it and host again.";
    else if (status.state == "no backend") error = "The main backend does not answer. Check your internet connection.";
    else if (status.state == "port in use")
      error = fmt::format("UDP {} is in use on this PC: another server runs here.", status.port);
    else if (status.state == "refused") error = "The main backend refused the match: " + status.why;
    else if (t > 150) error = "Your server did not come up in time. Its log: user\\private_match\\server.log.";
    std::vector<Cue> cues = {{kit::Button::kB, error.empty() ? "CANCEL" : "BACK"}};
    Prompts(d, st, st.P(48, 662), cues, in, 15);
    const bool registered = status.state == "registered";
    static constexpr const char* kSteps[] = {"STARTING YOUR SERVER", "LOADING THE MAP", "JOINING YOUR MATCH"};
    const int at = !status.running ? 0 : registered ? 2 : 1;
    const float h = st(41);
    for (int i = 0; i < 3; ++i) {
      const ImVec2 r0(ra.x, ra.y + h * i), r1(rb.x, ra.y + h * (i + 1));
      const bool done = i < at, now = i == at && error.empty();
      if (now) {
        kit::PaperArt(d, r0, r1);
        d->AddRectFilled(r0, ImVec2(r0.x + st(5), r1.y), kit::kStamp);
      }
      d->AddRectFilled(ImVec2(r0.x, r1.y - 1), ImVec2(r1.x, r1.y), IM_COL32(241, 234, 211, 20));
      const ImVec2 c(r0.x + st(30), r0.y + h * 0.5f);
      if (done) d->AddCircleFilled(c, st(5), kit::kGreenBright);
      else d->AddCircle(c, st(5), now ? kit::kInk : kit::kKhakiDim, 0, st(1.5f));
      if (now) {  // a turning arc over the current step
        const float a0 = float(ImGui::GetTime()) * 5.0f;
        d->PathArcTo(c, st(9), a0, a0 + 2.2f, 16);
        d->PathStroke(kit::kStamp, 0, st(2));
      }
      const float cap = st(16);
      kit::GameText(d, kit::GameFont::kHeadlineMedium, cap, ImVec2(r0.x + st(52), r0.y + (h - cap) * 0.5f),
                    now ? kit::kInk : done ? kit::kKhaki : kit::kKhakiDim, kSteps[i]);
    }
    kit::Spaced(d, f.typed, st(9), ImVec2(band.x + st(20), band.y + st(14)), kit::kInkSection,
                fmt::format("STARTING \xC2\xB7 {} S", int(t)), st(2));
    kit::GameText(d, kit::GameFont::kHeadline, st(26), ImVec2(band.x + st(20), band.y + st(29)), kit::kInk,
                  PmHeadline());
    kit::Wrapped(d, f.ui, st(11), ImVec2(band.x + st(20), band.y + st(64)), error.empty() ? kit::kInkFaint : kit::kRed,
                 error.empty() ? std::string("Your PC is starting the match's server. You join it as soon as it is up.") : error,
                 hb.x - band.x - st(40));
    if (in.back) {
      REXLOG_INFO("BF private match: {} on the starting page", error.empty() ? "cancelled" : "back after: " + error);
      EndPrivateMatch();
      pm_error_ = error;
    }
  }

  // The pause's card while this game hosts the match: its players, whether friends outside can reach it, and Y to copy
  // the address for Direct connect.
  void PrivateMatchPauseCard(ImDrawList* d, ImGuiIO& io, const Input& in) {
    const ImVec2 sz = io.DisplaySize;
    const kit::Fonts& f = kit::GetFonts();
    const float u = sz.y / 1080.0f;
    const bf1943::PrivateMatchStatus& st = pm_status_;
    if (in.y) CopyPrivateAddress();
    const float w = 640 * u;
    const ImVec2 ca(sz.x * 0.95f - w, sz.y * 0.60f), cb(sz.x * 0.95f, sz.y * 0.60f + 236 * u);
    d->AddRectFilled(ca, cb, IM_COL32(15, 17, 13, 222));
    d->AddRectFilled(ca, ImVec2(ca.x + 6 * u, cb.y), kit::kStamp);
    kit::Spaced(d, f.typed, 16 * u, ImVec2(ca.x + 30 * u, ca.y + 22 * u), kit::kKhakiDim,
                fmt::format("HOSTED ON THIS PC  /  {} {}", st.players, st.players == 1 ? "PLAYER" : "PLAYERS"), 3 * u);
    kit::Spaced(d, f.ui_bold, 30 * u, ImVec2(ca.x + 30 * u, ca.y + 48 * u), kit::kCream, "YOUR CUSTOM MATCH", 3 * u);
    const std::string reach = st.reachable == "true"  ? "Friends outside your home can reach it."
                              : st.reachable == "false" ? fmt::format("Friends outside your home cannot reach it yet: "
                                                                      "open UDP {} on your router.", st.port)
                                                        : "Friends on the internet need UDP " + std::to_string(st.port) +
                                                              " open on your router (asked for you if you chose so).";
    kit::Wrapped(d, f.typed, 19 * u, ImVec2(ca.x + 30 * u, ca.y + 96 * u), kit::kKhakiText,
                 reach + " They join with Direct connect in the server browser.", w - 60 * u);
    const float py = cb.y - 40 * u;
    const float gw = kit::Glyph(d, PromptStyle(), kit::Button::kY, "R", ImVec2(ca.x + 30 * u, py), 30 * u);
    const bool copied = ImGui::GetTime() - pm_copied_at_ < 3;
    kit::Spaced(d, f.ui_bold, 22 * u, ImVec2(ca.x + 42 * u + gw, py - 12 * u), copied ? kit::kGreenBright : kit::kKhakiText,
                copied ? "ADDRESS COPIED: SEND IT TO YOUR FRIENDS" : "COPY THE ADDRESS FOR FRIENDS", 2 * u);
  }

#if defined(BF_DEV_TOOLS)
  // Tests, with BF_AUTO_JOIN=1: --bf_dev_auto_host hosts our Create Private Match at the main menu with its defaults
  // (the router never asked); --bf_dev_auto_direct=<address> (a friend's test game) joins that address by Direct
  // connect, asked again every 5 s until the match is there. Returns true once it has joined or hosted.
  std::shared_ptr<std::atomic<int>> dev_direct_job_;
  std::shared_ptr<ServerInfo> dev_direct_found_;
  double dev_direct_at_ = -100;
  bool DevAutoPrivate() {
    if (MenuDriverStage() != 2) return false;
    const std::string direct = REXCVAR_GET(bf_dev_auto_direct);
    if (REXCVAR_GET(bf_dev_auto_host)) {
      LoadPrivateMatchChoices();  // the test profile's: LAN or online, tick rate, password
      pm_upnp_ = false;
      page_ = Page::kPrivateMatch, pm_row_ = kPmHost;
      HostPrivateMatch();
      return true;
    }
    if (direct.empty()) return false;
    if (dev_direct_job_ && dev_direct_job_->load() == 1) {
      REXLOG_INFO("BF dev: Direct connect to {} found {}", direct, dev_direct_found_->name);
      const ServerInfo sv = *dev_direct_found_;
      dev_direct_job_.reset();
      Join(sv);
      return true;
    }
    if ((!dev_direct_job_ || dev_direct_job_->load() == 2) && ImGui::GetTime() - dev_direct_at_ > 5) {
      dev_direct_at_ = ImGui::GetTime();
      const size_t colon = direct.rfind(':');
      const std::string text = direct.substr(0, colon);
      const int port = colon == std::string::npos ? 18300 : std::atoi(direct.c_str() + colon + 1);
      auto job = std::make_shared<std::atomic<int>>(0);
      auto found = std::make_shared<ServerInfo>();
      dev_direct_job_ = job, dev_direct_found_ = found;
      std::thread([this, job, found, text, port] { job->store(list_.Find(text, port, *found) ? 1 : 2); }).detach();
    }
    return false;
  }
#endif

  // ---- Help, ours (the rest of the game's Help & Options, its settings being our Settings): the game's own tutorial
  // (its startTutorial command), its controller layouts, and the credits.
  int help_row_ = 0, ctl_tab_ = 0;
  bool ctl_from_pause_ = false;  // the controls opened from the pause: B goes back there
  bool tutorial_ = false;        // the join page shows the tutorial's load
  float credits_scroll_ = 0;

  // Help in the new look (Version A1, game_menu_home.inc): the three rows in a rail, the chosen one's card beside them
  // with its button; from Home before the game runs too (the tutorial then starts the game first).
  void HelpPage(ImDrawList* d, ImGuiIO& io, const Input& in_raw) {
    NotePage("help");
    Input in = in_raw;
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    MenuBackdrop(d, io.DisplaySize, st);
    bool online_ok = false;
    const std::string status = OnlineStatus(online_ok);
    const bool edit = MenuChrome(d, io, st, status, online_ok);
    kit::Spaced(d, f.typed, st(10.5f), st.P(48, 91), kit::kKhakiDim, "FIELD MANUAL \xC2\xB7 THE TUTORIAL, THE CONTROLS, THE CREDITS", st(2));
    kit::GameText(d, kit::GameFont::kHeadline, st(40), st.P(48, 105), kit::kCream, "HELP");
    Prompts(d, st, st.P(48, 662), {{kit::Button::kA, "SELECT"}, {kit::Button::kB, "BACK"}}, in, 15);
    MenuFoot(d, st);
    static constexpr const char* kRows[] = {"TUTORIAL", "CONTROLS", "CREDITS"};
    static constexpr const char* kNotes[] = {"THE GAME'S TRAINING GROUND", "EVERY BUTTON, EVERY SEAT", "WHO MADE IT"};
    static constexpr const char* kButtons[] = {"START THE TUTORIAL", "SHOW THE CONTROLS", "ROLL THE CREDITS"};
    static constexpr const char* kAbout[] = {
        "The game's own tutorial: its training ground on Wake Island, played offline at your own pace. Before the game "
        "runs it starts the game first, then the tutorial.",
        "What every button does, as the game sets it out: on foot, in vehicles and boats, and in planes, in your own "
        "button style.",
        "Who made Battlefield 1943 Reborn, the original game's makers, and the parts it is built with."};
    const int count = 3;
    if (in.up) help_row_ = (help_row_ + count - 1) % count;
    if (in.down) help_row_ = (help_row_ + 1) % count;
    help_row_ = std::clamp(help_row_, 0, count - 1);
    const float h = st(43);
    const ImVec2 ra = st.P(48, 167), rb(st.P(400, 0).x, ra.y + h * float(count));
    d->AddRectFilled(ra, rb, IM_COL32(13, 15, 11, 204));
    d->AddRect(ra, rb, IM_COL32(241, 234, 211, 30), 0, 0, 1.0f);
    int clicked = -1;
    for (int i = 0; i < count; ++i) {
      const ImVec2 a(ra.x, ra.y + h * float(i)), b(rb.x, ra.y + h * float(i + 1));
      const bool sel = i == help_row_;
      const bool hover = ImGui::IsMouseHoveringRect(a, b);
      if (hover && io.MouseDelta.x + io.MouseDelta.y != 0) help_row_ = i;
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = i;
      if (sel) {
        kit::PaperArt(d, a, b);
        d->AddRectFilled(a, ImVec2(a.x + st(5), b.y), kit::kStamp);
      }
      d->AddRectFilled(ImVec2(a.x, b.y - 1), ImVec2(b.x, b.y), IM_COL32(241, 234, 211, 20));
      kit::Spaced(d, f.typed, st(10), ImVec2(a.x + st(24), a.y + h * 0.5f - st(5)), sel ? kit::kInkSection : kit::kKhakiDim,
                  fmt::format("{:02d}", i + 1), st(1.5f));
      kit::GameText(d, kit::GameFont::kHeadlineMedium, st(19), ImVec2(a.x + st(52), a.y + (h - st(19)) * 0.5f),
                    sel ? kit::kInk : kit::kKhaki, kRows[i]);
      if (sel) {
        kit::Glyph(d, PromptStyle(), kit::Button::kA, "Enter", ImVec2(b.x - st(18) - st(20), a.y + h * 0.5f), st(20));
      } else {
        const float nw = kit::SpacedWidth(f.typed, st(9.5f), kNotes[i], st(1.5f));
        kit::Spaced(d, f.typed, st(9.5f), ImVec2(b.x - st(18) - nw, a.y + h * 0.5f - st(5)), kit::kKhakiDim, kNotes[i], st(1.5f));
      }
    }
    {  // the chosen row's card
      const ImVec2 a = st.P(427, 167), b = st.P(1232, 400);
      kit::PaperArt(d, a, b);
      kit::Spaced(d, f.typed, st(10), ImVec2(a.x + st(24), a.y + st(22)), kit::kInkSection,
                  std::string("FIELD MANUAL \xC2\xB7 ") + kRows[help_row_], st(2));
      kit::GameText(d, kit::GameFont::kHeadline, st(28), ImVec2(a.x + st(24), a.y + st(40)), kit::kInk, kRows[help_row_]);
      kit::Wrapped(d, f.ui, st(14.25f), ImVec2(a.x + st(24), a.y + st(86)), kit::kInk, kAbout[help_row_], b.x - a.x - st(48) - st(170));
      if (GreenButton(d, st, ImVec2(a.x + st(24), b.y - st(64)), ImVec2(a.x + st(24) + st(230), b.y - st(24)), kit::Button::kA,
                      kButtons[help_row_]))
        clicked = help_row_;
      d->PushClipRect(a, b, true);
      kit::ArtAt(d, "corsair", ImVec2(b.x - st(160), b.y - st(70)), st(64));
      d->PopClipRect();
    }
    if (in.accept) clicked = help_row_;
    if (clicked >= 0) {
      help_row_ = clicked;
      REXLOG_INFO("BF help: {}", kRows[clicked]);
      if (clicked == 0) BeginTutorial();
      else if (clicked == 1) page_ = Page::kControls, ctl_tab_ = 0, ctl_from_pause_ = false;
      else page_ = Page::kCredits, credits_scroll_ = 0;
      return;
    }
    if (edit || in.view) return BeginAccount(false);
    if (in.back) page_ = pre_game_ ? Page::kHome : Page::kMain, home_row_ = kRowHelp, row_ = MainRowOf("HELP");
  }

  // The tutorial from Help: before the game runs it starts the game first (as a server picked on Home does), the
  // tutorial following its sign in (OnDraw, tutorial_pending_); with the game at its menu, at once.
  bool tutorial_pending_ = false;
  void BeginTutorial() {
    if (pre_game_ && start_game_) {
      REXLOG_INFO("BF help: the tutorial picked before the game ran: the game starts, the tutorial follows its sign in");
      tutorial_pending_ = true, tutorial_ = true;
      joining_ = "The game's tutorial", joining_map_ = "Wake Island";
      joined_gid_.clear();
      comms::NoteJoined("");
      world::NoteJoined("");
      failed_ = false, joined_from_start_ = true;
      page_ = Page::kJoining;
      StartGameFromLauncher();
      return;
    }
    StartTutorialMatch();
  }

  // The tutorial: the game's own command, then our joining page until the training level is in (as a join).
  void StartTutorialMatch() {
    REXLOG_INFO("BF help: the tutorial (the game's startTutorial)");
    tutorial_ = true;
    joining_ = "The game's tutorial";
    joining_map_ = "Wake Island";
    joined_gid_.clear();
    comms::NoteJoined("");  // no server: no voice or text chat in the tutorial
    world::NoteJoined("");
    failed_ = false;
    joined_from_start_ = false;
    page_ = Page::kJoining;
    bf1943::StartTutorial();
  }

  // The game's controller layouts (its Help & Options > Controls: setup for soldier, vehicles and airplanes; runs
  // help3 and helpveh1), each button shown in the player's own button style.
  struct CtlRow {
    kit::Button button;
    const char* action;
  };
  void ControlsPage(ImDrawList* d, ImGuiIO& io, const Input& in) {
    NotePage("controls");
    const ImVec2 sz = io.DisplaySize;
    const kit::Fonts& f = kit::GetFonts();
    const float u = sz.y / 1080.0f;
    using B = kit::Button;
    static const std::vector<CtlRow> kSoldier = {
        {B::kLS, "Left stick: move; press to sprint"}, {B::kRS, "Right stick: look; press to crouch"}, {B::kRT, "Fire"}, {B::kLT, "Zoom"},
        {B::kRB, "Toggle weapon"}, {B::kLB, "Throw grenade, explosives"}, {B::kA, "Jump"},
        {B::kB, "Enter, use, pick up"}, {B::kX, "Reload"}, {B::kY, "Draw knife"},
        {B::kBack, "Command (press), score (hold)"}, {B::kStart, "In game menu"}};
    static const std::vector<CtlRow> kVehicles = {
        {B::kLS, "Left stick: turn"}, {B::kRS, "Right stick: look, move the turret"}, {B::kLT, "Throttle"}, {B::kLB, "Brake"},
        {B::kRT, "Fire"}, {B::kRB, "Secondary fire"}, {B::kA, "Change seat"}, {B::kB, "Exit"},
        {B::kDpad, "Toggle camera"}, {B::kBack, "Command (press), score (hold)"}, {B::kStart, "In game menu"}};
    static const std::vector<CtlRow> kPlanes = {
        {B::kLS, "Left stick: yaw"}, {B::kRS, "Right stick: pitch and roll"}, {B::kLT, "Throttle"}, {B::kLB, "Hold for free look"},
        {B::kRT, "Fire"}, {B::kRB, "Drop bombs"}, {B::kB, "Exit"}, {B::kDpad, "Toggle camera"},
        {B::kBack, "Command (press), score (hold)"}, {B::kStart, "In game menu"}};
    static constexpr const char* kTabs[] = {"ON FOOT", "VEHICLES AND BOATS", "AIRPLANES"};
    if (in.prev_tab || in.left) ctl_tab_ = (ctl_tab_ + 2) % 3;
    if (in.next_tab || in.right) ctl_tab_ = (ctl_tab_ + 1) % 3;
    if (ctl_from_pause_) {
      d->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(8, 9, 7, 238));  // over the match, darkened (more than the pause: text)
    } else {
      MenuBackdrop(d, sz, StageOf(io));  // the new look's backdrop
      d->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(12, 14, 11, 186));
    }
    const float x0 = sz.x * 0.05f, x1 = sz.x * 0.95f;
    kit::Brand(d, ImVec2(x0, 58 * u), u * 0.9f);
    kit::Spaced(d, f.typed, 20 * u, ImVec2(x0, 150 * u), kit::kKhakiDim, "THE GAME'S OWN LAYOUT", 4 * u);
    Text(d, f.stencil, 78 * u, ImVec2(x0 - 4 * u, 174 * u), kit::kCream, "CONTROLS");
    d->AddRectFilled(ImVec2(x0, 270 * u), ImVec2(x0 + 64 * u, 276 * u), kit::kStamp);
    // The tabs.
    float tx = x0;
    for (int i = 0; i < 3; ++i) {
      const float w = kit::SpacedWidth(f.ui_bold, 24 * u, kTabs[i], 3 * u);
      const ImVec2 a(tx - 14 * u, 300 * u), b(tx + w + 14 * u, 346 * u);
      if (ImGui::IsMouseHoveringRect(a, b) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ctl_tab_ = i;
      if (i == ctl_tab_) d->AddRectFilled(ImVec2(a.x, b.y - 4 * u), ImVec2(b.x, b.y), kit::kStamp);
      kit::Spaced(d, f.ui_bold, 24 * u, ImVec2(tx, 310 * u), i == ctl_tab_ ? kit::kCream : kit::kKhakiDim, kTabs[i], 3 * u);
      tx += w + 56 * u;
    }
    // The rows, in two columns: the button in the player's style (a keyboard player sees the pad's; its keys are in
    // Settings), then what it does.
    const kit::Style style = PromptStyle() == kit::Style::kKeyboard ? kit::Style::kXboxSeries : PromptStyle();
    const std::vector<CtlRow>& rows = ctl_tab_ == 0 ? kSoldier : ctl_tab_ == 1 ? kVehicles : kPlanes;
    const size_t half = (rows.size() + 1) / 2;
    const float colw = (x1 - x0) * 0.5f, rh = 66 * u, top = 380 * u;
    for (size_t i = 0; i < rows.size(); ++i) {
      const float cx = x0 + (i < half ? 0 : colw), cy = top + rh * float(i < half ? i : i - half);
      d->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + colw - 30 * u, cy + rh - 10 * u), IM_COL32(15, 17, 13, 200));
      const float gw = kit::Glyph(d, style, rows[i].button, "", ImVec2(cx + 20 * u, cy + (rh - 10 * u) * 0.5f), 38 * u);
      kit::Spaced(d, f.ui_bold, 24 * u, ImVec2(cx + 40 * u + std::max(gw, 60 * u), cy + 14 * u), kit::kKhakiText,
                  Upper(rows[i].action), 2 * u);
    }
    kit::Wrapped(d, f.typed, 18 * u, ImVec2(x0, top + rh * float(half) + 16 * u), kit::kKhakiDim,
                 "Keyboard and mouse: every key is in Settings, Controls, Keyboard bindings. Buttons can be swapped in "
                 "Settings, Controls, Button remapping.",
                 x1 - x0);
    const float py = sz.y - 64 * u;
    float x = x0;
    x += kit::Glyph(d, PromptStyle(), kit::Button::kLB, "Q", ImVec2(x, py), 34 * u) + 6 * u;
    x += kit::Glyph(d, PromptStyle(), kit::Button::kRB, "E", ImVec2(x, py), 34 * u) + 10 * u;
    x += kit::Spaced(d, f.ui_bold, 24 * u, ImVec2(x, py - 14 * u), kit::kKhakiText, "SECTION", 3 * u) + 34 * u;
    x += kit::Glyph(d, PromptStyle(), kit::Button::kB, "Esc", ImVec2(x, py), 34 * u) + 10 * u;
    kit::Spaced(d, f.ui_bold, 24 * u, ImVec2(x, py - 14 * u), kit::kKhakiText, "BACK", 3 * u);
    if (in.back) {
      if (ctl_from_pause_) page_ = Page::kPause, pause_row_ = kPauseControls, ctl_from_pause_ = false;
      else page_ = Page::kHelp, help_row_ = 1;
    }
  }

  // The credits: this project, the original game's makers (its own credits roll lives in its text tables, which our
  // menus do not read yet), and the parts it is built with (their licences in the folder 'licences').
  void CreditsPage(ImDrawList* d, ImGuiIO& io, const Input& in) {
    NotePage("credits");
    const ImVec2 sz = io.DisplaySize;
    const kit::Fonts& f = kit::GetFonts();
    const float u = sz.y / 1080.0f;
    MenuBackdrop(d, sz, StageOf(io));  // the new look's backdrop
    d->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(12, 14, 11, 196));
    struct Line {
      int kind;  // 0 heading, 1 name, 2 text, 3 gap
      const char* text;
    };
    static constexpr Line kLines[] = {
        {0, "BATTLEFIELD 1943 REBORN"},
        {1, "TRG Studios (TekRantGaming)"},
        {2, "A fan project: your own copy of the Xbox 360 game on PC, with its servers, menus and renderer made new."},
        {3, ""},
        {0, "THE ORIGINAL GAME"},
        {1, "Battlefield 1943, by DICE, published by Electronic Arts (2009)"},
        {2, "This project is not made, endorsed or supported by Electronic Arts or DICE. No game files are included:"},
        {2, "the game's pictures, sounds and text come from your own copy, on your PC only."},
        {3, ""},
        {0, "BUILT WITH"},
        {2, "ReXGlue SDK (the recompiler and its runtime) and the parts it is built on"},
        {2, "NativeXRenderer and King Kong Recompiled's renderer work"},
        {2, "DiligentCore, SPIRV-Cross, volk"},
        {2, "AMD FidelityFX Super Resolution 1 and CAS, AMD FidelityFX SDK, NVIDIA DLSS, NVIDIA Image Scaling, SMAA, XeGTAO"},
        {2, "Oswald font (SIL Open Font License), button prompts (CC0), vgmstream's EA block layout"},
        {3, ""},
        {2, "Every licence is in the folder 'licences' next to bf1943.exe."}};
    const float x0 = sz.x * 0.08f;
    kit::Brand(d, ImVec2(sz.x * 0.05f, 58 * u), u * 0.9f);
    Text(d, f.stencil, 78 * u, ImVec2(sz.x * 0.05f - 4 * u, 150 * u), kit::kCream, "CREDITS");
    d->AddRectFilled(ImVec2(sz.x * 0.05f, 246 * u), ImVec2(sz.x * 0.05f + 64 * u, 252 * u), kit::kStamp);
    float y = 300 * u;
    for (const Line& l : kLines) {
      if (l.kind == 0) {
        kit::Spaced(d, f.typed, 20 * u, ImVec2(x0, y), kit::kKhakiDim, l.text, 4 * u);
        y += 40 * u;
      } else if (l.kind == 1) {
        kit::Spaced(d, f.ui_bold, 30 * u, ImVec2(x0, y), kit::kCream, Upper(l.text), 3 * u);
        y += 48 * u;
      } else if (l.kind == 2) {
        d->AddText(f.typed, 19 * u, ImVec2(x0, y), kit::kKhakiText, l.text);
        y += 30 * u;
      } else {
        y += 22 * u;
      }
    }
    const float py = sz.y - 64 * u;
    const float gw = kit::Glyph(d, PromptStyle(), kit::Button::kB, "Esc", ImVec2(sz.x * 0.05f, py), 34 * u);
    kit::Spaced(d, f.ui_bold, 24 * u, ImVec2(sz.x * 0.05f + gw + 10 * u, py - 14 * u), kit::kKhakiText, "BACK", 3 * u);
    if (in.back) page_ = Page::kHelp, help_row_ = 2;
  }

  // A small chevron badge for the rank (drawn: a shield with one to three stripes, stars from rank 14).
  void RankBadge(ImDrawList* d, const Stage& st, ImVec2 c, int rank) {
    const ImVec2 pts[] = {ImVec2(c.x - st(18), c.y - st(18)), ImVec2(c.x + st(18), c.y - st(18)),
                          ImVec2(c.x + st(18), c.y + st(4)),  ImVec2(c.x, c.y + st(20)),
                          ImVec2(c.x - st(18), c.y + st(4))};
    d->AddConvexPolyFilled(pts, 5, IM_COL32(61, 74, 42, 255));
    d->AddPolyline(pts, 5, kit::kGold, ImDrawFlags_Closed, st(2));
    if (rank >= 14) {
      const int stars = std::min(3, (rank - 12) / 3 + 1);
      for (int i = 0; i < stars; ++i) d->AddCircleFilled(ImVec2(c.x + (float(i) - (stars - 1) * 0.5f) * st(10), c.y - st(2)), st(3.5f), kit::kGold);
      return;
    }
    const int stripes = std::min(3, 1 + (rank - 1) / 4);
    for (int i = 0; i < stripes; ++i) {
      const float y = c.y - st(10) + float(i) * st(7);
      const ImVec2 v[] = {ImVec2(c.x - st(11), y), ImVec2(c.x, y + st(6)), ImVec2(c.x + st(11), y)};
      d->AddPolyline(v, 3, kit::kGold, 0, st(3));
    }
  }

  // Play: BF's online menu with our list (signed in by now, under Home); until it is up, the starting notice.
  void Play() {
    if (pre_game_ && start_game_) {  // the launcher before the game: the game starts now
      REXLOG_INFO("BF launcher: Play: the game starts");
      pre_game_ = false, played_ = true, started_from_launcher_ = true;
      NoteStartValues();  // the renderer takes its settings now
      page_ = Page::kShaders, shaders_shown_ = false;
      shaders_prepared_hint_ = ShadersPreparedForPlay();  // prepared before: this page goes on at once (ShadersPage)
      auto start = std::move(start_game_);
      start();
      return;
    }
    if (MenuDriverStage() == 2) {
      page_ = Page::kMain, row_ = 0;
    } else {
      page_ = Page::kStarting, starting_at_ = ImGui::GetTime();
    }
  }

  void StartingPage(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("starting");
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    const int stage = MenuDriverStage();
    if (stage == 2) {
      page_ = Page::kMain, row_ = 0;
      return;
    }
    Chrome(d, io, st, 0);
    const bool late = ImGui::GetTime() - starting_at_ > 60 || stage == 6;
    Prompts(d, st, st.P(390, 720 - 30 - 42), {{kit::Button::kB, "Back"}}, in);
    const ImVec2 a = st.P(390, 250), b = st.P(890, 450);
    {
      kit::Turn turn(d, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), -0.4f);
      kit::Paper(d, a, b, st.u);
      kit::Tape(d, ImVec2((a.x + b.x) * 0.5f, a.y), st(110), st(26), -3);
      kit::Heading(d, st(44), ImVec2(a.x + st(32), a.y + st(34)), kit::kInk, "GETTING READY", st(2));
      const std::string line = late ? "The servers did not answer. Check your connection, then try again from Home."
                               : stage < 1 ? "The game is starting." : "Signing in to the servers.";
      kit::Wrapped(d, f.ui, st(20), ImVec2(a.x + st(32), a.y + st(96)), late ? kit::kRed : kit::kInkSoft, line, st(360));
      if (!late) {  // a turning arrow, as BF's own notices
        const ImVec2 c(b.x - st(56), b.y - st(56));
        const float r = st(22), t = float(ImGui::GetTime()) * 4.0f;
        d->PathArcTo(c, r, t, t + 5.0f, 32);
        d->PathStroke(kit::kInk, 0, st(5));
      }
    }
    StampCorner(d, st);
    if (in.back) page_ = Page::kHome;
  }

  // ---- More from TRG Studios (King Kong's projects row as its own page)

  struct Project {
    const char* name;
    const char* owner;
    const char* repo;
    const char* initials;
    ImU32 cover;
  };
  static constexpr Project kFeatured = {"King Kong Remastered", "TekRantGaming", "king-kong-recompiled", "KK",
                                        IM_COL32(43, 59, 44, 255)};
  // King Kong's list (kk/src/workshop_logic.h, kProjects), the owner's names; their pictures fetched as King Kong does
  // (projects_art.cpp, TekRantGaming 11 Nov 2026), the drawn covers until then.
  void StartProjectArt() {
    std::vector<projects::Repo> repos = {{kFeatured.owner, kFeatured.repo}};
    for (const Project& p : kProjects) repos.push_back({p.owner, p.repo});
    projects::StartArt(UserDir() / "cache", std::move(repos));
  }
  static constexpr Project kProjects[] = {
      {"Point Blank Recompiled", "TekRantGaming", "point-blank-recompiled", "PB", IM_COL32(140, 109, 42, 255)},
      {"Simpsons Wrestling Recompiled", "TekRantGaming", "simpsons-wrestling-recompiled", "SW", IM_COL32(52, 64, 122, 255)},
      {"Earthworm Jim HD Recompiled", "TekRantGaming", "earthworm-jim-hd-recompiled", "EJ", IM_COL32(106, 122, 44, 255)},
      {"Outpost Kaloki X Recompiled", "TekRantGaming", "outpost-kaloki-x-recompiled", "OK", IM_COL32(79, 107, 58, 255)},
      {"Super Mario Sunshine", "chasem-dev", "sms-launcher", "SM", IM_COL32(40, 90, 140, 255)},
      {"Dig Dug Native", "TekRantGaming", "digdug-native", "DD", IM_COL32(150, 60, 40, 255)},
      {"South Park: Tenorman's Revenge Recompiled", "TekRantGaming", "south-park-tenormans-revenge-recompiled", "SP",
       IM_COL32(122, 82, 50, 255)},
      {"South Park: Let's Go Tower Defense Play! Recompiled", "TekRantGaming", "south-park-tower-defense-recompiled", "TD",
       IM_COL32(77, 92, 102, 255)},
      {"AVP Evolution: PC Edition", "TekRantGaming", "avp-evolution-pc-edition", "AVP", IM_COL32(36, 49, 58, 255)},
      {"Cartoon Network Racing Recompiled", "TekRantGaming", "cartoon-network-racing-recompiled", "CN",
       IM_COL32(138, 43, 34, 255)},
      {"Muppet RaceMania Recompiled", "TekRantGaming", "muppet-racemania-recompiled", "MR", IM_COL32(181, 102, 42, 255)},
      {"Trials: ReVolution Collection", "TekRantGaming", "Trials-ReVolution-Collection", "TR", IM_COL32(28, 28, 24, 255)},
      {"Angry Birds: The Re-Hatched Collection", "TekRantGaming", "angry-birds-re-hatched-collection", "AB",
       IM_COL32(168, 50, 42, 255)},
      {"The Whopping Recompilation Collection", "TekRantGaming", "whopping-recompilation-collection", "WR",
       IM_COL32(184, 71, 28, 255)}};
  static constexpr int kGridCols = 7;

  void ProjectsPage(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("projects");
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    MenuBackdrop(d, io.DisplaySize, st);
    kit::Heading(d, st(44), st.P(40, 22), kit::kCream, "MORE FROM TRG STUDIOS", st(2));
    const int n = int(std::size(kProjects));
    // project_: -1 the featured one, 0..n-1 the grid, n the TRG Launcher banner.
    if (in.up) project_ = project_ == n ? n - kGridCols + 0 : project_ < kGridCols ? -1 : project_ - kGridCols;
    if (in.down) project_ = project_ == -1 ? 0 : project_ + kGridCols >= n ? n : project_ + kGridCols;
    if (in.left && project_ >= 0 && project_ < n) project_ = std::max(0, project_ - 1);
    if (in.right && project_ >= 0 && project_ < n) project_ = std::min(n - 1, project_ + 1);
    project_ = std::clamp(project_, -1, n);
    int clicked = -2;
    {
      const ImVec2 a = st.P(40, 84), b = st.P(1240, 216);
      const bool hover = ImGui::IsMouseHoveringRect(a, b);
      if (hover && io.MouseDelta.x + io.MouseDelta.y != 0) project_ = -1;
      kit::Turn turn(d, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), -0.3f);
      kit::Paper(d, a, b, st.u, false);
      if (project_ == -1) d->AddRect(ImVec2(a.x - st(3), a.y - st(3)), ImVec2(b.x + st(3), b.y + st(3)), kit::kGreenBright, 0, 0, st(4));
      const ImVec2 c0(a.x + st(18), a.y + st(14)), c1(c0.x + st(104), c0.y + st(104));
      d->AddRectFilled(c0, c1, kFeatured.cover);
      // Its picture once the worker has it (projects_art.cpp: King Kong's own SteamGridDB icon), else the drawn cover.
      if (!kit::DrawImageFile(d, projects::ArtFile(UserDir() / "cache", kFeatured.repo), c0, c1, false, true, 480)) {
        const ImVec2 hill[] = {ImVec2(c0.x, c0.y + st(80)), ImVec2(c0.x + st(22), c0.y + st(46)), ImVec2(c0.x + st(40), c0.y + st(64)),
                               ImVec2(c0.x + st(62), c0.y + st(26)), ImVec2(c0.x + st(84), c0.y + st(60)),
                               ImVec2(c0.x + st(104), c0.y + st(40)), ImVec2(c1.x, c1.y), ImVec2(c0.x, c1.y)};
        d->AddConcavePolyFilled(hill, 8, IM_COL32(20, 28, 21, 255));
        d->AddCircleFilled(ImVec2(c0.x + st(78), c0.y + st(24)), st(12), IM_COL32(215, 185, 106, 255));
      }
      kit::Spaced(d, f.typed, st(13), ImVec2(c1.x + st(20), a.y + st(24)), kit::kInkFaint, "FEATURED", st(2));
      d->AddText(f.ui, st(34), ImVec2(c1.x + st(20), a.y + st(40)), kit::kInk, kFeatured.name);
      d->AddText(f.ui, st(17), ImVec2(c1.x + st(20), a.y + st(88)), IM_COL32(61, 59, 49, 255),
                 "Peter Jackson's King Kong, version 2.0.0, out now");
      Input view_in;
      Prompts(d, st, ImVec2(b.x - st(200), a.y + st(44)), {{kit::Button::kA, "VIEW ON GITHUB"}}, view_in, 16);
      if ((hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) || view_in.accept) clicked = -1;
    }
    const float tile_w = st(1200) / float(kGridCols);
    for (int i = 0; i < n; ++i) {
      const int r = i / kGridCols, c = i % kGridCols;
      const ImVec2 cell(st.P(40, 0).x + float(c) * tile_w, st.P(0, 236 + float(r) * 172).y);
      // 16:9 tiles (the projects' art is mostly wide: SteamGridDB grids at 2:1, README banners wider).
      const ImVec2 c0(cell.x + (tile_w - st(160)) * 0.5f, cell.y + st(10)), c1(c0.x + st(160), c0.y + st(90));
      const ImVec2 hit0(cell.x, cell.y), hit1(cell.x + tile_w, cell.y + st(160));
      const bool hover = ImGui::IsMouseHoveringRect(hit0, hit1);
      if (hover && io.MouseDelta.x + io.MouseDelta.y != 0) project_ = i;
      const bool sel = project_ == i;
      d->AddRectFilled(ImVec2(c0.x + st(2), c0.y + st(6)), ImVec2(c1.x + st(2), c1.y + st(8)), IM_COL32(0, 0, 0, 90));
      d->AddRectFilled(c0, c1, kProjects[i].cover);
      // The project's picture, got as King Kong gets it (projects_art.cpp), once it is in the cache; its initials until.
      if (!kit::DrawImageFile(d, projects::ArtFile(UserDir() / "cache", kProjects[i].repo), c0, c1, false, true, 480)) {
        const float iw = kit::SpacedWidth(f.stencil, st(34), kProjects[i].initials, 0);
        kit::Spaced(d, f.stencil, st(34), ImVec2((c0.x + c1.x - iw) * 0.5f, c0.y + st(22)), IM_COL32(241, 234, 211, 255),
                    kProjects[i].initials, 0);
      }
      if (sel) d->AddRect(ImVec2(c0.x - st(4), c0.y - st(4)), ImVec2(c1.x + st(4), c1.y + st(4)), kit::kGreenBright, 0, 0, st(4));
      const float wrap = tile_w - st(12);
      const float tw = std::min(wrap, kit::TextWidth(f.ui, st(14), kProjects[i].name));
      d->AddText(f.ui, st(14), ImVec2(cell.x + (tile_w - tw) * 0.5f, c1.y + st(6)), sel ? IM_COL32(255, 255, 255, 255) : kit::kBackText,
                 kProjects[i].name, nullptr, wrap);
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = i;
    }
    {
      const ImVec2 a = st.P(40, 590), b = st.P(1240, 656);
      const bool hover = ImGui::IsMouseHoveringRect(a, b);
      if (hover && io.MouseDelta.x + io.MouseDelta.y != 0) project_ = n;
      d->AddRectFilled(a, b, kit::kStrip);
      if (project_ == n) d->AddRect(a, b, kit::kGreenBright, 0, 0, st(3));
      d->AddRectFilled(ImVec2(a.x + st(16), a.y + st(10)), ImVec2(a.x + st(62), a.y + st(56)), IM_COL32(201, 150, 58, 255));
      d->AddText(f.ui_bold, st(17), ImVec2(a.x + st(22), a.y + st(20)), kit::kStrip, "TRG");
      d->AddText(f.ui, st(20), ImVec2(a.x + st(78), a.y + st(6)), kit::kStripText, "TRG Launcher");
      d->AddText(f.ui, st(14), ImVec2(a.x + st(78), a.y + st(36)), IM_COL32(207, 198, 168, 255),
                 "Free for anyone to use in their own projects");
      const char* link = "github.com/TekRantGaming/trg-launcher";
      d->AddText(f.ui, st(17), ImVec2(b.x - st(16) - kit::TextWidth(f.ui, st(17), link), a.y + st(20)), IM_COL32(232, 196, 106, 255), link);
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = n;
    }
    Prompts(d, st, st.P(40, 720 - 18 - 36), {{kit::Button::kA, "VIEW ON GITHUB"}, {kit::Button::kB, "BACK"}}, in, 15);
    if (kit::WatermarkOn()) kit::Stamp(d, st.P(1240 - 118, 18 + 30), st.u, -5, 0.8f);
    if (in.accept) clicked = project_;
    if (clicked != -2) {
      const std::string url = clicked == -1 ? std::string("https://github.com/") + kFeatured.owner + "/" + kFeatured.repo
                              : clicked == n ? std::string("https://github.com/TekRantGaming/trg-launcher")
                                             : std::string("https://github.com/") + kProjects[clicked].owner + "/" +
                                                   kProjects[clicked].repo;
      OpenUrl(url);
    }
    if (in.back) page_ = Page::kHome;
  }

  // ---- Unlockables: what the player has unlocked, and whether it is worn (the user, 22 Nov 2026: "commit weapon skin
  // as an unlock, and only shows if equipped from main menu's unlock screen"). One so far: the Electric weapon skin
  // (fov.cpp: bf_skin_electric unlocked, bf_skin_equipped worn). Locked, it says how it is unlocked and cannot be
  // equipped; unlocked, A equips it or takes it off, saved with the settings.
  void UnlockablesPage(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("unlockables");
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    MenuBackdrop(d, io.DisplaySize, st);
    kit::Heading(d, st(44), st.P(40, 22), kit::kCream, "UNLOCKABLES", st(2));
    const bool unlocked = Truthy(Cvar("bf_skin_electric"));
    const bool equipped = unlocked && Cvar("bf_skin_equipped") == "electric";
    const ImVec2 a = st.P(40, 96), b = st.P(1240, 268);
    const bool hover = ImGui::IsMouseHoveringRect(a, b);
    kit::Paper(d, a, b, st.u, false);
    d->AddRect(ImVec2(a.x - st(3), a.y - st(3)), ImVec2(b.x + st(3), b.y + st(3)), unlocked ? kit::kGreenBright : kit::kKhakiDim, 0, 0,
               st(4));
    // Its swatch: the skin's own colours (dark metal, gold wood) and a stroke of its lightning.
    const ImVec2 c0(a.x + st(18), a.y + st(18)), c1(c0.x + st(136), c0.y + st(136));
    d->AddRectFilled(c0, c1, IM_COL32(22, 26, 36, 255));
    d->AddRectFilled(ImVec2(c0.x, c0.y + st(92)), c1, IM_COL32(196, 150, 52, 255));
    const ImVec2 bolt[] = {ImVec2(c0.x + st(84), c0.y + st(10)), ImVec2(c0.x + st(54), c0.y + st(52)), ImVec2(c0.x + st(76), c0.y + st(56)),
                           ImVec2(c0.x + st(46), c0.y + st(104))};
    d->AddPolyline(bolt, 4, unlocked ? IM_COL32(170, 214, 255, 255) : IM_COL32(110, 118, 132, 255), 0, st(4));
    const float tx = c1.x + st(22);
    kit::Spaced(d, f.typed, st(13), ImVec2(tx, a.y + st(22)), kit::kInkFaint, "WEAPON SKIN", st(2));
    d->AddText(f.ui, st(34), ImVec2(tx, a.y + st(38)), kit::kInk, "Electric");
    d->AddText(f.ui, st(17), ImVec2(tx, a.y + st(84)), IM_COL32(61, 59, 49, 255),
               "For your Thompson and Type 100: dark metal, gold wood, and a storm of smoke and lightning round the gun.");
    d->AddText(f.ui, st(15), ImVec2(tx, a.y + st(112)), IM_COL32(61, 59, 49, 255),
               unlocked ? "Unlocked. Only your own game draws it: other players do not see it yet."
                        : "Locked. To unlock it: play one full Night Ops match, from its start to its end.");
    const char* state = equipped ? "EQUIPPED" : unlocked ? "NOT EQUIPPED" : "LOCKED";
    const float sw = kit::SpacedWidth(f.stencil, st(26), state, st(2));
    kit::Spaced(d, f.stencil, st(26), ImVec2(b.x - st(24) - sw, a.y + st(24)), equipped ? kit::kGreenDark : kit::kStamp, state, st(2));
    std::vector<Cue> cues;
    if (unlocked) cues.push_back({kit::Button::kA, equipped ? "TAKE OFF" : "EQUIP"});
    cues.push_back({kit::Button::kB, "BACK"});
    Prompts(d, st, st.P(40, 720 - 18 - 36), cues, in, 15);
    if (kit::WatermarkOn()) kit::Stamp(d, st.P(1240 - 118, 18 + 30), st.u, -5, 0.8f);
    if (unlocked && (in.accept || (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))) {
      const char* now = equipped ? "none" : "electric";
      rex::cvar::SetFlagByName("bf_skin_equipped", now);
      SetSetting("cvar.bf_skin_equipped", now);
      REXLOG_INFO("BF weapon skins: the electric skin {}", equipped ? "taken off" : "equipped");
    }
    if (in.back) BackToMenu(kRowUnlockables, 3);
  }

  // ---- Report an issue: the form that files on GitHub is game_menu_report.inc (ReportPage).

  // ---- Settings from Home: the five tabs in the launcher's look

  void HomeSettingsPage(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("settings");
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    Chrome(d, io, st, 0, SceneBehind() ? 0.84f : 0);
    // Changed settings taken at the start: Start restarts the game now (they show NEXT START on their rows).
    const bool restart = RestartPending();
    std::vector<Cue> cues = {{kit::Button::kA, "CHANGE"}, {kit::Button::kLB, "SECTION"}, {kit::Button::kRB, "SECTION"},
                             {kit::Button::kB, "BACK"}};
    if (restart) cues.push_back({kit::Button::kStart, "RESTART TO APPLY", "P", true});
    Prompts(d, st, st.P(40, 720 - 26 - 40), cues, in, 16);
    kit::Heading(d, st(54), st.P(40, 70), kit::kCream, "SETTINGS", st(2));
    if (restart && sub_ == kSubNone)
      d->AddText(f.typed, st(15), st.P(40, 134), kit::kKhakiText,
                 "Settings marked NEXT START take effect when the game starts again.");
    if (restart && in.start) {
      RestartForSettings("Settings from Home");
      return;
    }
    // The tabs: a strip each, the current one green; a click switches.
    float x = st.P(330, 0).x;
    for (int t = 0; t < kTabCount; ++t) {
      const std::string name = kTabNames[t];
      const float tw = kit::SpacedWidth(f.ui, st(17), name, st(1));
      const ImVec2 a(x, st.P(0, 92).y), b(x + tw + st(28), st.P(0, 126).y);
      const bool cur = t == tab_ && sub_ == kSubNone;
      d->AddRectFilled(a, b, cur ? kit::kGreen : kit::kStrip);
      kit::Spaced(d, f.ui, st(17), ImVec2(a.x + st(14), a.y + st(4)), kit::kStripText, name, st(1));
      if (ImGui::IsMouseHoveringRect(a, b) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        tab_ = t, row_ = 0, scroll_ = 0, sub_ = kSubNone;
      x = b.x + st(8);
    }
    if (sub_ != kSubNone)
      d->AddText(f.typed, st(15), st.P(330, 134), kit::kKhakiText,
                 sub_ == kSubRemap ? "Controls, Button remapping" : "Controls, Keyboard bindings");
    if (FrSettings(d, io, st, 160, in, true)) page_ = Page::kHome;
    StampCorner(d, st);
  }

  // ---- Add to Steam (King Kong's way) and the updates: filled in by later phases of the plan.
  // Add to Steam, King Kong's way (steam_shortcut.cpp): a question first; then on a worker Steam is closed (it writes
  // its own list as it exits; up to 60 s), the shortcut is added with Steam Input off, and Steam opens again.
  struct SteamJob {
    std::atomic<int> state{0};  // 5 downloading the artwork, 1 closing Steam, 2 adding, 3 done, 4 failed
    std::mutex mutex;
    std::string error;
    bool added = false;
    int art = 0;  // artwork pictures saved, of the 5
  };
  std::shared_ptr<SteamJob> steam_job_;
  bool steam_ask_ = false, steam_checked_ = false;
  bool steam_running_at_ask_ = false;  // Steam running when the question opened (its words and button)
  std::shared_ptr<std::atomic<int>> steam_has_;  // the check at the first Home: -1 running, 0 no, 1 yes
  void AddToSteam() {
    if (steam_job_ && (steam_job_->state.load() < 3 || steam_job_->state.load() == 5)) return;
    steam_job_.reset();
    steam_running_at_ask_ = steam::SteamRunning();
    steam_ask_ = true, popup_row_ = 1;
  }
  void CheckSteamOnce() {
    if (steam_checked_) return;
    steam_checked_ = true;
    steam_has_ = std::make_shared<std::atomic<int>>(-1);
    std::thread([has = steam_has_] {
      const auto where = steam::Locate();
      has->store(steam::StartedFromSteam() || (where && steam::HasShortcut(*where)) ? 1 : 0);
    }).detach();
  }
  void StartSteamJob() {
    auto job = std::make_shared<SteamJob>();
    steam_job_ = job;
    job->state = 5;
    REXLOG_INFO("BF launcher: adding the game to Steam");
    std::thread([job] {
      auto fail = [&](const std::string& why) {
        std::lock_guard lock(job->mutex);
        job->error = why;
        job->state = 4;
        REXLOG_INFO("BF launcher: Add to Steam failed: {}", why);
      };
#if !defined(_WIN32)
      // A Flatpak sees Steam's folder read only (and not Steam's own process): its shortcut cannot be written from
      // inside. Steam's own way does the same, and the package's desktop entry is in its list.
      if (const char* id = std::getenv("FLATPAK_ID"); id && *id)
        return fail("The Flatpak cannot change Steam's files. In Steam's desktop mode choose Games, Add a Non-Steam "
                    "Game to My Library, and tick Battlefield 1943 Reborn in the list.");
#endif
      const auto where = steam::Locate();
      if (!where) return fail("Steam wasn't found on this computer, or nobody has signed in to it yet.");
      // The artwork first (TekRantGaming, 11 Nov 2026: Battlefield 1943's from SteamGridDB, as King Kong's), so Steam is
      // closed only while its list is written.
      const std::vector<steam::ArtPiece> art = steam::DownloadArt();
      job->state = 1;
      if (steam::SteamRunning()) {
        steam::CloseSteam();
        for (int i = 0; i < 600 && steam::SteamRunning(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (steam::SteamRunning()) return fail("Steam didn't close. Close Steam yourself, then choose Add to Steam again.");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // its last writes (King Kong's wait)
      }
      job->state = 2;
      const steam::Result r = steam::AddShortcut(*where, art);
      if (!r.ok) return fail(r.error);
      job->added = true;
      job->art = r.art;
      steam::OpenSteam();
      job->state = 3;
    }).detach();
  }
  // The question and the progress over Home or the last setup page; true while it is up (it takes the input).
  bool SteamDialog(ImDrawList* d, ImGuiIO& io, const Stage& st, Input& in) {
    if (steam_has_ && steam_has_->load() == 1) steam_added_ = true;
    if (steam_ask_) {
      bool yes = false;
      // King Kong's words (launcher.cpp OpenSteamPrompt), with Steam Input off said too.
      const std::string text = std::string("Adds Battlefield 1943 Reborn to your Steam library as a non Steam game, with "
                                           "artwork from SteamGridDB and Steam Input off (the game reads your controller "
                                           "itself), so you can start it from Steam, Big Picture or a Steam Deck.") +
                               (steam_running_at_ask_ ? " Steam reads its list of games only when it starts, so it closes "
                                                        "for a moment and opens again afterwards."
                                                      : "");
      if (Question(d, io, st, "ADD TO STEAM?", text, "Cancel", steam_running_at_ask_ ? "Close Steam and add" : "Add to Steam",
                   in, yes)) {
        steam_ask_ = false;
        if (yes) {
          if (steam::StartedFromSteam()) steam_added_ = true;
          else StartSteamJob();
        }
      }
      return true;
    }
    if (!steam_job_) return false;
    const int state = steam_job_->state.load();
    std::string text;
    if (state == 5) text = "Downloading its artwork from SteamGridDB...";
    else if (state == 1) text = "Closing Steam...";
    else if (state == 2) text = "Adding Battlefield 1943 Reborn to your library...";
    else if (state == 3) {
      // King Kong's success texts.
      const int art = steam_job_->art;
      text = art >= 5   ? "Battlefield 1943 Reborn is in your Steam library, with its artwork."
             : art > 0  ? "Battlefield 1943 Reborn is in your Steam library, with " + std::to_string(art) +
                             " of its 5 artwork pictures; the others didn't download."
                        : "Battlefield 1943 Reborn is in your Steam library, but its artwork didn't download, so Steam "
                          "shows a plain tile.";
      text += " Steam is opening again.";
    } else {
      std::lock_guard lock(steam_job_->mutex);
      text = steam_job_->error;
    }
    if (state == 3) steam_added_ = true;
    bool ok = false;
    const bool finished = state == 3 || state == 4;  // 5 is the download, before 1
    if (finished) {
      if (Question(d, io, st, state == 3 ? "ADDED TO STEAM" : "NOT ADDED", text, "OK", "", in, ok)) steam_job_.reset();
    } else {
      Input none;
      bool unused = false;
      Question(d, io, st, "ADDING TO STEAM", text, "Please wait", "", none, unused);
      in = {};
    }
    return true;
  }
  // ---- Updates (update.cpp, King Kong's way; always on, TekRantGaming): checked at the start and every 6 hours while Home
  // is open; a newer release downloads by itself, and installs at once from the update page while Home is showing.
  std::string UpdateNote() {
    auto& u = update::Get();
    const auto state = update::State(u.state.load());
    const std::string v = BfVersion();
    if (u.finishing) {  // the first start check fetching this version's files again
      const uint64_t total = u.total.load();
      return total ? "FINISHING THE UPDATE " + std::to_string(int(100.0 * double(u.bytes.load()) / double(total))) + "%"
                   : "FINISHING THE UPDATE";
    }
    switch (state) {
      case update::State::kIdle:
      case update::State::kChecking: return "CHECKING \xC2\xB7 " + v;
      case update::State::kNone: {
        std::lock_guard lock(u.mutex);
        return (u.message == "Up to date" ? "UP TO DATE \xC2\xB7 " : "NOT CHECKED \xC2\xB7 ") + v;
      }
      case update::State::kDownloading: {
        const uint64_t total = u.total.load();
        return total ? "DOWNLOADING " + std::to_string(int(100.0 * double(u.bytes.load()) / double(total))) + "%"
                     : "DOWNLOADING";
      }
      case update::State::kChecked:
      case update::State::kInstalling:
      case update::State::kInstalled: {
        std::lock_guard lock(u.mutex);
        return "READY: VERSION " + u.version;
      }
      case update::State::kFailed: return "FAILED \xC2\xB7 A RETRIES";
    }
    return v;
  }
  void UpdateNow() {
    const auto state = update::State(update::Get().state.load());
    // A release found: the update page, which shows its download and installs it (0.4.6.3: also while downloading).
    if (state == update::State::kChecked || state == update::State::kDownloading) update_asked_ = true, page_ = Page::kUpdate;
    else if (state == update::State::kNone || state == update::State::kFailed) CheckForUpdate("the Update row");
  }
  void CheckForUpdate(const char* why) {
    update_checked_at_ = ImGui::GetTime();
    REXLOG_INFO("BF update: checking ({})", why);
    update::CheckInBackground();
  }
  // Every frame on the main menu and its pages: the 6 hourly check, and a newer release found (downloading, or here)
  // brings up the new version pop-up on the main menu, once a run for each version (before the game runs and after it).
  // Update there opens the update page (its download, then the install, then the game starts again); Cancel leaves it
  // for this run (the Update row still offers it). Before 0.4.6.3 a checked release installed at once, unasked, and
  // its download only showed on the Update row (TekRantGaming, 14 Nov 2026: "you can see it downloading, but it's not
  // really visible").
  void UpdateTick() {
    const double now = ImGui::GetTime();
    if (now - update_checked_at_ > 6 * 3600) CheckForUpdate("six hours since the last one");
    auto& u = update::Get();
    const auto state = update::State(u.state.load());
    if ((state != update::State::kDownloading && state != update::State::kChecked) || u.finishing.load()) return;
    std::string version;
    {
      std::lock_guard lock(u.mutex);
      version = u.version;
    }
    if (version.empty() || !update::IsNewerVersion(version, BfVersion()) || version == new_version_shown_) return;
    if ((page_ != Page::kHome && page_ != Page::kMain) || quit_popup_ || update_popup_ || steam_ask_ || steam_job_ ||
        whats_new_open_ || exp_popup_ || tick_popup_ || pw_popup_ || own_popup_ || new_version_popup_ || notice_popup_ ||
        news_open_)  // (after the news window is closed)
      return;
    new_version_popup_ = true, new_version_shown_ = version, popup_row_ = 1;  // Update
    REXLOG_INFO("BF update: version {} found (this is {}): the new version pop-up", version, BfVersion());
  }
  // The new version pop-up (TekRantGaming, 14 Nov 2026: "a pop-up that comes up saying new game version released,
  // cancel an update, and then a disclaimer as well that says you may be rejected from the servers without updating
  // to the latest version"). The download goes on behind it; its progress shows here.
  void NewVersionPopup(ImDrawList* d, ImGuiIO& io, Input in) {
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    auto& u = update::Get();
    const auto state = update::State(u.state.load());
    d->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(10, 12, 8, 190));
    const ImVec2 a = st.P(330, 170), b = st.P(950, 480);
    {
      kit::Turn turn(d, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), -0.3f);
      kit::Paper(d, a, b, st.u);
      kit::Tape(d, ImVec2((a.x + b.x) * 0.5f, a.y), st(110), st(26), -3);
      kit::Heading(d, st(36), ImVec2(a.x + st(30), a.y + st(26)), kit::kInk, "NEW GAME VERSION RELEASED", st(1.5f), st(560));
      kit::Spaced(d, f.typed, st(11), ImVec2(a.x + st(32), a.y + st(72)), kit::kStamp,
                  fmt::format("VERSION {} \xC2\xB7 THIS GAME {}", new_version_shown_, BfVersion()), st(1.5f));
      float y = a.y + st(100);
      const char* text = "A new version of Battlefield 1943 Reborn is out. Update now: it downloads, installs, and the "
                         "game starts again by itself.";
      kit::Wrapped(d, f.ui, st(17), ImVec2(a.x + st(30), y), kit::kInkSoft, text, st(560));
      y += kit::WrappedHeight(f.ui, st(17), text, st(560)) + st(14);
      const char* warning = "You may be rejected from the servers without updating to the latest version.";
      kit::Wrapped(d, f.ui_bold, st(17), ImVec2(a.x + st(30), y), kit::kRed, warning, st(560));
      y += kit::WrappedHeight(f.ui_bold, st(17), warning, st(560)) + st(18);
      // The download's bar (TekRantGaming, 14 Nov 2026: "instead of using text to show the percentage, add a loading
      // bar here that shows users exactly what's happening"), as the update page draws it, and a line under it.
      const uint64_t got = u.bytes.load(), total = u.total.load();
      const bool ready = state == update::State::kChecked;
      const float w = st(560);
      const ImVec2 b0(a.x + st(30), y), b1(a.x + st(30) + w, y + st(20));
      d->AddRectFilled(b0, b1, IM_COL32(34, 36, 27, 38));
      if (ready || total) {
        const double done = ready ? 1.0 : std::min(1.0, double(got) / double(total));
        d->AddRectFilled(b0, ImVec2(b0.x + w * float(done), b1.y), kit::kGreen);
      } else {  // not sized yet: a moving stripe
        const float t = float(std::fmod(ImGui::GetTime() * 0.6, 1.0));
        d->AddRectFilled(ImVec2(b0.x + w * std::max(0.0f, t * 1.3f - 0.3f), b0.y),
                         ImVec2(b0.x + w * std::min(1.0f, t * 1.3f), b1.y), kit::kGreen);
      }
      d->AddRect(b0, b1, kit::kInk, 0, 0, st(2));
      const std::string line =
          ready ? std::string("Downloaded. Update installs it and starts the game again.")
          : total ? fmt::format("Downloading in the background: {:.0f} MB of {:.0f} MB.", double(got) / 1048576.0,
                                double(total) / 1048576.0)
                  : std::string("Downloading in the background.");
      d->AddText(f.ui, st(15), ImVec2(b0.x, b1.y + st(6)), kit::kInkSoft, line.c_str());
    }
    if (in.left || in.right || in.up || in.down) popup_row_ = 1 - popup_row_;
    int clicked = -1;
    float x = a.x + st(30);
    for (int i = 0; i < 2; ++i) {
      const std::string label = i == 0 ? "Cancel" : "Update";
      const ImVec2 c0(x, b.y - st(70)), c1(x + st(250), b.y - st(26));
      const bool hover = ImGui::IsMouseHoveringRect(c0, c1);
      if (hover && io.MouseDelta.x + io.MouseDelta.y != 0) popup_row_ = i;
      const bool sel = popup_row_ == i;
      if (sel) d->AddRectFilled(ImVec2(c0.x, c0.y + st(4)), ImVec2(c1.x, c1.y + st(4)), kit::kGreenShadow);
      d->AddRectFilled(c0, c1, sel ? kit::kGreen : kit::kPaperCard);
      if (!sel) d->AddRect(c0, c1, kit::kBorder, 0, 0, st(2));
      const float tw = kit::TextWidth(f.ui_bold, st(20), label);
      d->AddText(f.ui_bold, st(20), ImVec2((c0.x + c1.x - tw) * 0.5f, c0.y + st(8)), sel ? IM_COL32(255, 255, 255, 255) : kit::kInk,
                 label.c_str());
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = i;
      x += st(270);
    }
    if (in.accept) clicked = popup_row_;
    if (in.back) clicked = 0;
    if (clicked < 0) return;
    new_version_popup_ = false;
    if (clicked == 0) {
      new_version_declined_ = new_version_shown_;
      REXLOG_INFO("BF update: the new version pop-up answered Cancel (version {} not installed this run)", new_version_shown_);
      return;
    }
    REXLOG_INFO("BF update: the new version pop-up answered Update (version {})", new_version_shown_);
    update_asked_ = true;
    page_ = Page::kUpdate;
  }
  // The check at the main menu (TekRantGaming, 13 Nov 2026: "the updater seemed to have failed for a user that had
  // skipped a few versions but worked the moment they hit the update button on the menu can we make it so the client
  // checks for an update once they hit the main menu. kong dose this the same way"). The start's own check runs while
  // the first pages are up and can end without an answer, before the menu is reached or after (it waits up to 20 s).
  // While the main menu shows, a check without an answer is made again by itself, what the Update row did by hand: at
  // once, then after 30 s, 1, 2 and 5 minutes, then every 10; a download that failed three times a run. And every
  // arrival at the menu (back from a match, from Settings) asks again when the last answer is over ten minutes old,
  // so a release that came out during a match is found at the menu, before a server refuses the old version.
  void UpdateOnMenu() {
    const double now = ImGui::GetTime();
    const bool arrived = now - menu_shown_at_ > 0.5;  // not drawn on the frames before: another page was up
    menu_shown_at_ = now;
    auto& u = update::Get();
    const auto state = update::State(u.state.load());
    bool up_to_date = false;
    {
      std::lock_guard lock(u.mutex);
      up_to_date = u.message == "Up to date";
    }
    const bool download_failed = state == update::State::kFailed;
    if (download_failed || (state == update::State::kNone && !up_to_date)) {
      static constexpr double kWaits[] = {0, 30, 60, 120, 300, 600};
      const double wait = kWaits[std::min<size_t>(size_t(update_retries_), std::size(kWaits) - 1)];
      if ((download_failed && update_retries_ >= 3) || now - update_tried_at_ < wait) return;
      ++update_retries_, update_tried_at_ = now;
      CheckForUpdate(download_failed ? "at the main menu, the last download failed" : "at the main menu, the last check had no answer");
    } else if (arrived && state == update::State::kNone && now - update_checked_at_ > 600) {
      CheckForUpdate("the main menu reached, the last answer is over ten minutes old");
    }
  }

  // ---- What's new (TekRantGaming, 13 Nov 2026: "patch notes popup like kong has when game updates make sure it only
  // shows after the updates shaders have finished exact same as kong"). King Kong's rules (its launcher.cpp and
  // launcher_policy.h ShowWhatsNewNow): once after an update, never on a first install; only on the main menu, with
  // no other pop-up up, the setup done and this version's shaders prepared (after an update the shader page comes
  // first, PrepareShadersAtStart); the notes of every version since the one run before (this version's only when
  // that is not known); Close, and Every version for the whole changelog. Seen is remembered when it opens
  // (last_version), so a start ended on the shader page still shows it the next time.
  // Developer builds: a scripted test run shows none, unless BF_DEV_WHATS_NEW=<the version run before> asks: the
  // notes since that version, the build counted as the changelog's newest one (a developer build's own version has
  // no notes).
  void WhatsNewTick() {
    if (!whats_new_pending_ || whats_new_open_ || update_popup_ || quit_popup_ || steam_ask_ || steam_job_ || notice_popup_ ||
        queue_popup_)
      return;
    std::string current = BfVersion();
#if defined(BF_DEV_TOOLS)
    const char* dev = std::getenv("BF_DEV_WHATS_NEW");
    if (dev && *dev) {
      whats_new_from_ = dev;
      if (!Changelog().empty()) current = Changelog().front().version;
    } else if (const char* scripted = std::getenv("BF_DEV_SCRIPTED"); scripted && *scripted == '1') {
      whats_new_pending_ = false;
      return;
    }
#endif
    if (!ShadersPreparedForPlay()) return;  // the update's shaders first
    const auto state = update::State(update::Get().state.load());
    if (state == update::State::kChecked || state == update::State::kInstalling || state == update::State::kInstalled) return;
    whats_new_pending_ = false;
    settings_.values["last_version"] = BfVersion();
    settings_.Save();
    whats_new_shown_ = ChangelogSince(whats_new_from_, current);
    if (whats_new_shown_.empty()) {
      REXLOG_INFO("BF launcher: What's new: no notes for v{} (after v{}): not shown", current, whats_new_from_.empty() ? "?" : whats_new_from_);
      return;
    }
    whats_new_open_ = true, whats_new_all_ = false, whats_new_scroll_ = 0;
    REXLOG_INFO("BF launcher: What's new opened (v{} after v{}: {} version{})", current, whats_new_from_.empty() ? "?" : whats_new_from_,
                whats_new_shown_.size(), whats_new_shown_.size() == 1 ? "" : "s");
  }
  std::vector<const ChangelogEntry*> whats_new_shown_;

  // Words drawn one after another from `at`, wrapped at `wrap`, the first `bold_words` of them in bold. Returns the
  // height; draws nothing without `d`.
  static float WordsWrapped(ImDrawList* d, const std::string& lead, const std::string& text, ImVec2 at, float wrap, float size,
                            ImU32 lead_col, ImU32 col) {
    const kit::Fonts& f = kit::GetFonts();
    const float line = size * 1.45f, space = kit::TextWidth(f.ui, size, " ");
    float x = 0, y = 0;
    for (int part = 0; part < 2; ++part) {
      const std::string& s = part == 0 ? lead : text;
      ImFont* font = part == 0 ? f.ui_bold : f.ui;
      for (size_t i = 0; i < s.size();) {
        while (i < s.size() && s[i] == ' ') ++i;
        const size_t end = std::min(s.find(' ', i), s.size());
        if (end == i) break;
        const std::string word = s.substr(i, end - i);
        const float w = kit::TextWidth(font, size, word);
        if (x > 0 && x + w > wrap) x = 0, y += line;
        if (d) d->AddText(font, size, ImVec2(at.x + x, at.y + y), part == 0 ? lead_col : col, word.c_str());
        x += w + space;
        i = end;
      }
    }
    return y + line;
  }

  void WhatsNewPopup(ImDrawList* d, ImGuiIO& io, Input in) {
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    d->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(10, 12, 8, 190));
    const ImVec2 a = st.P(250, 56), b = st.P(1030, 664);  // King Kong's reader is 780 wide
    kit::Paper(d, a, b, st.u, false);
    kit::Tape(d, ImVec2((a.x + b.x) * 0.5f, a.y), st(110), st(26), -2);
    const float x0 = a.x + st(34), x1 = b.x - st(34);
    // The entries: the versions since the last one run, or every version.
    std::vector<const ChangelogEntry*> all;
    if (whats_new_all_)
      for (const ChangelogEntry& e : Changelog()) all.push_back(&e);
    const std::vector<const ChangelogEntry*>& entries = whats_new_all_ ? all : whats_new_shown_;
    const ChangelogEntry* first = entries.empty() ? nullptr : entries.front();
    kit::Spaced(d, f.typed, st(10.5f), ImVec2(x0, a.y + st(26)), kit::kInkSection,
                whats_new_all_ ? "BATTLEFIELD 1943 REBORN \xC2\xB7 THE CHANGELOG" : "BATTLEFIELD 1943 REBORN \xC2\xB7 UPDATED", st(2));
    kit::GameText(d, kit::GameFont::kHeadline, st(30), ImVec2(x0, a.y + st(42)), kit::kInk,
                  whats_new_all_ ? std::string("EVERY VERSION") : "WHAT'S NEW IN " + Upper(first ? first->version : std::string(BfVersion())));
    d->AddRectFilled(ImVec2(x0, a.y + st(86)), ImVec2(x1, a.y + st(87)), kit::kRule);
    // The notes, scrolled.
    const ImVec2 v0(x0, a.y + st(96)), v1(x1, b.y - st(64));
    const float wrap = v1.x - v0.x - st(18), view = v1.y - v0.y;
    const bool several = entries.size() > 1;
    float total = 0;
    for (int pass = 0; pass < 2; ++pass) {  // 0 measures, 1 draws
      ImDrawList* out = pass == 1 ? d : nullptr;
      if (pass == 1) {
        whats_new_scroll_ = std::clamp(whats_new_scroll_, 0.0f, std::max(0.0f, total - view));
        d->PushClipRect(v0, v1, true);
      }
      float y = v0.y - (pass == 1 ? whats_new_scroll_ : 0.0f);
      const float y_start = y;
      for (const ChangelogEntry* e : entries) {
        // A version's own heading when more than one is listed, its date and what it is about under it.
        if (several) {
          if (out) kit::GameText(out, kit::GameFont::kHeadlineMedium, st(17), ImVec2(v0.x, y), kit::kInk, Upper(e->version));
          y += st(24);
        }
        const std::string about = Upper(e->date) + (e->title.empty() ? "" : " \xC2\xB7 " + Upper(e->title));
        if (!about.empty()) {
          if (out) kit::Wrapped(out, f.typed, st(10), ImVec2(v0.x, y), kit::kInkSection, about, wrap);
          y += kit::WrappedHeight(f.typed, st(10), about, wrap) + st(10);
        }
        for (const ChangelogLine& l : ChangelogLines(e->body)) {
          if (l.kind == ChangelogLine::kHead) {
            y += st(6);
            if (out) {
              kit::Spaced(out, f.ui_bold, st(11.5f), ImVec2(v0.x, y), kit::kStamp, Upper(l.text), st(2));
              out->AddRectFilled(ImVec2(v0.x, y + st(17)), ImVec2(v0.x + wrap, y + st(18)), kit::kRule);
            }
            y += st(26);
          } else if (l.kind == ChangelogLine::kBullet) {
            if (out) out->AddRectFilled(ImVec2(v0.x + st(2), y + st(6.5f)), ImVec2(v0.x + st(7), y + st(11.5f)), kit::kInk);
            y += WordsWrapped(out, l.lead, l.text, ImVec2(v0.x + st(16), y), wrap - st(16), st(11.5f), kit::kInk, kit::kInkSoft) + st(5);
          } else {
            y += WordsWrapped(out, "", l.text, ImVec2(v0.x, y), wrap, st(11.5f), kit::kInk, kit::kInkSoft) + st(7);
          }
        }
        y += st(16);
      }
      if (pass == 0) total = y - y_start;
      else d->PopClipRect();
    }
    if (total > view) {  // how far down the notes are
      const float track = view, thumb = std::max(st(30), track * view / total);
      const float top = v0.y + (track - thumb) * (whats_new_scroll_ / std::max(1.0f, total - view));
      d->AddRectFilled(ImVec2(v1.x - st(4), v0.y), ImVec2(v1.x, v1.y), IM_COL32(34, 36, 27, 24));
      d->AddRectFilled(ImVec2(v1.x - st(4), top), ImVec2(v1.x, top + thumb), kit::kInkSection);
    }
    d->AddRectFilled(ImVec2(x0, v1.y + st(8)), ImVec2(x1, v1.y + st(9)), kit::kRule);
    Prompts(d, st, ImVec2(x0, b.y - st(46)),
            {{kit::Button::kA, "CLOSE", "", true}, {kit::Button::kX, whats_new_all_ ? "THIS UPDATE" : "EVERY VERSION"}}, in, 14);
    if (total > view) {
      const std::string hint = "UP AND DOWN SCROLL";
      kit::Spaced(d, f.typed, st(9.5f), ImVec2(x1 - kit::SpacedWidth(f.typed, st(9.5f), hint, st(1.5f)), b.y - st(34)), kit::kInkSection, hint, st(1.5f));
    }
    // Up and down (held: the menu's own repeat), the bumpers a page, the wheel.
    if (in.up) whats_new_scroll_ -= st(70);
    if (in.down) whats_new_scroll_ += st(70);
    if (in.prev_tab) whats_new_scroll_ -= view * 0.85f;
    if (in.next_tab) whats_new_scroll_ += view * 0.85f;
    if (io.MouseWheel != 0) whats_new_scroll_ -= io.MouseWheel * st(60);
    if (in.x) whats_new_all_ = !whats_new_all_, whats_new_scroll_ = 0;
    if (in.accept || in.back) {
      whats_new_open_ = false;
      REXLOG_INFO("BF launcher: What's new closed");
    }
  }

  // ---- The version gate's pop-up (server_list.h; TekRantGaming, 13 Nov 2026: "reject and prompt outdated clicnt
  // connections give them a popup advising and a button to update now or close"). Over the page that asked to join.
  void AskToUpdate(const std::string& needs, const std::string& server) {
    update_popup_ = true, update_needs_ = needs, update_server_ = server;
    popup_row_ = 1;  // Update now
    REXLOG_INFO("BF join: refused by the version gate: {} needs v{}, this is v{}: the update pop-up", server.empty() ? "the server" : server,
                needs.empty() ? "?" : needs, BfVersion());
  }
  void UpdatePopup(ImDrawList* d, ImGuiIO& io, Input in) {
    const Stage st = StageOf(io);
    const std::string text =
        update_needs_.empty()
            ? fmt::format("The servers play a newer version than this game's, {}. Update now: it takes a moment, and the game "
                          "starts again by itself.",
                          BfVersion())
            : fmt::format("The servers play version {}, and this game is version {}. Update now: it takes a moment, and the "
                          "game starts again by itself.",
                          update_needs_, BfVersion());
    bool yes = false;
    if (!Question(d, io, st, "UPDATE NEEDED", text, "Close", "Update now", in, yes)) return;
    update_popup_ = false;
    REXLOG_INFO("BF join: the update pop-up answered: {}", yes ? "Update now" : "Close");
    if (!yes) return;
    // Update now: the update page, which shows the check, the download and the install, then starts the game again.
    update_asked_ = true;
    const auto state = update::State(update::Get().state.load());
    if (state == update::State::kNone || state == update::State::kFailed) CheckForUpdate("Update now on the pop-up");
    page_ = Page::kUpdate;
  }
  // ---- The backend's words for a player it does not let in (server_list.h ServerNotice; TekRantGaming, 15 Nov 2026:
  // maintenance periods players learn about in the game, not only on Discord): maintenance mode's notice, a full
  // server, any refusal of a join with a message. Over the page that asked to join, one OK.
  bool notice_popup_ = false;
  ServerNotice notice_shown_;
  ServerNotice removed_notice_;   // moderation: the server's words when it took this player off it (comms kKicked)
  bool removed_pending_ = false;  // shown once the menus are back after the match
  void ShowNotice(const ServerNotice& n, const std::string& asked) {
    notice_popup_ = true, notice_shown_ = n, popup_row_ = 0;
    if (notice_shown_.title.empty()) notice_shown_.title = n.kind == "full" ? "SERVER FULL" : "COULD NOT JOIN";
    REXLOG_INFO("BF join: {} refused by the backend ({}): {}: {}", asked.empty() ? "the join" : asked,
                n.kind.empty() ? "maintenance" : n.kind, notice_shown_.title, n.text);
  }
  void NoticePopup(ImDrawList* d, ImGuiIO& io, Input in) {
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    d->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(10, 12, 8, 190));
    // A refusal of this one join (a full server, one gone while its queue waited), moderation's (a ban or kick:
    // "banned", docs/MODERATION.md) or maintenance mode's (any other kind).
    const bool banned = notice_shown_.kind == "banned";
    const bool full = notice_shown_.kind == "full" || notice_shown_.kind == "gone" || banned;
    const std::string eyebrow = banned ? "MODERATION \xC2\xB7 COULD NOT JOIN"
                                : full ? "THE SERVER LIST \xC2\xB7 COULD NOT JOIN" : "THE SERVER LIST \xC2\xB7 SERVERS CLOSED";
    const std::string foot = full ? "" : "New joins are closed for now. Players already in a match play on.";
    const float w = st(560);
    const float text_h = kit::WrappedHeight(f.ui, st(18), notice_shown_.text, w);
    const float foot_h = foot.empty() ? 0 : kit::WrappedHeight(f.ui, st(14), foot, w) + st(12);
    const float h = st(26) + st(22) + st(48) + text_h + st(16) + foot_h + st(70) + st(20);
    const float top = std::max(st.P(0, 120).y, st.P(0, 360).y - h * 0.5f);
    const ImVec2 a(st.P(330, 0).x, top), b(st.P(950, 0).x, top + h);
    {
      kit::Turn turn(d, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), -0.3f);
      kit::Paper(d, a, b, st.u);
      kit::Tape(d, ImVec2((a.x + b.x) * 0.5f, a.y), st(110), st(26), -3);
      float y = a.y + st(26);
      kit::Spaced(d, f.typed, st(11), ImVec2(a.x + st(32), y), kit::kStamp, eyebrow, st(1.5f));
      y += st(22);
      kit::Heading(d, st(36), ImVec2(a.x + st(30), y), kit::kInk, Upper(notice_shown_.title), st(1.5f), w);
      y += st(48);
      kit::Wrapped(d, f.ui, st(18), ImVec2(a.x + st(30), y), kit::kInkSoft, notice_shown_.text, w);
      y += text_h + st(16);
      if (!foot.empty()) kit::Wrapped(d, f.ui, st(14), ImVec2(a.x + st(30), y), kit::kInkFaint, foot, w);
    }
    const ImVec2 c0(a.x + st(30), b.y - st(70)), c1(a.x + st(230), b.y - st(26));
    const bool hover = ImGui::IsMouseHoveringRect(c0, c1);
    d->AddRectFilled(ImVec2(c0.x, c0.y + st(4)), ImVec2(c1.x, c1.y + st(4)), kit::kGreenShadow);
    d->AddRectFilled(c0, c1, kit::kGreen);
    const float tw = kit::TextWidth(f.ui_bold, st(20), "OK");
    d->AddText(f.ui_bold, st(20), ImVec2((c0.x + c1.x - tw) * 0.5f, c0.y + st(8)), IM_COL32(255, 255, 255, 255), "OK");
    if (in.accept || in.back || (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left))) {
      notice_popup_ = false;
      REXLOG_INFO("BF join: the backend's notice closed");
    }
  }
  void UpdatePage(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("update");
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    auto& u = update::Get();
    const auto state = update::State(u.state.load());
    if (state == update::State::kChecked) update::InstallInBackground();
    if (state == update::State::kInstalled && !relaunched_) {
      relaunched_ = true;
      REXLOG_INFO("BF launcher: update installed; starting again");
      if (update::RelaunchSelf() && quit_) quit_();
    }
    Chrome(d, io, st, 0, SceneBehind() ? 0.86f : 0);
    std::string version, message;
    {
      std::lock_guard lock(u.mutex);
      version = u.version, message = u.message;
    }
    // Reached by Update now on the version gate's pop-up (update_asked_), the page is up from the check on: looking
    // for the release, downloading it, installing it. A check that found nothing newer, or could not ask, says so.
    const bool looking = state == update::State::kIdle || state == update::State::kChecking;
    const bool downloading = state == update::State::kDownloading;
    const bool nothing = state == update::State::kNone;
    const bool failed = state == update::State::kFailed || nothing;
    if (failed) Prompts(d, st, st.P(240, 720 - 30 - 42), {{kit::Button::kA, "Try again"}, {kit::Button::kB, "Back"}}, in);
    const ImVec2 a = st.P(240, 200), b = st.P(1040, 480);
    {
      kit::Turn turn(d, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), -0.4f);
      kit::Paper(d, a, b, st.u);
      kit::Tape(d, ImVec2((a.x + b.x) * 0.5f, a.y + st(4)), st(110), st(26), 2);
      const float x = a.x + st(44), w = st(712);
      kit::Spaced(d, f.typed, st(15), ImVec2(x, a.y + st(40)), kit::kInkFaint,
                  "VERSION " + std::string(BfVersion()) + " \xC2\xB7 AUTOMATIC UPDATE", st(2));
      const uint64_t got = u.bytes.load(), total = u.total.load();
      const std::string heading = nothing       ? (message == "Up to date" ? "NO NEWER VERSION YET" : "UPDATE NOT FOUND")
                                  : failed      ? "UPDATE NOT INSTALLED"
                                  : looking     ? "LOOKING FOR THE UPDATE"
                                  : downloading ? "DOWNLOADING " + version
                                                : "UPDATING TO " + version;
      kit::Heading(d, st(52), ImVec2(x, a.y + st(64)), kit::kInk, heading, st(2), w);
      std::string line;
      if (nothing && message == "Up to date")
        line = fmt::format("Version {} is the newest one out{}. Try again in a few minutes.", BfVersion(),
                           update_needs_.empty() ? "" : ", and the servers ask for " + update_needs_);
      else if (nothing) line = message.empty() ? "Could not check for updates." : message + " Check your connection, then try again.";
      else if (failed) line = message;
      else if (looking) line = "Asking for the newest version.";
      else if (downloading)
        line = total ? fmt::format("{:.0f} MB of {:.0f} MB. It installs by itself when it is here.", double(got) / 1048576.0,
                                   double(total) / 1048576.0)
                     : std::string("Downloading the new version.");
      else if (state == update::State::kInstalled) line = message;
      else line = "Installing the new version. The game starts again by itself in a moment.";
      kit::Wrapped(d, f.ui, st(20), ImVec2(x, a.y + st(134)), failed && !nothing ? kit::kRed : kit::kInkSoft, line, w);
      if (!failed) {
        const ImVec2 b0(x, a.y + st(206)), b1(x + w, a.y + st(226));
        d->AddRectFilled(b0, b1, IM_COL32(34, 36, 27, 38));
        if (downloading && total) {  // how much of it is here
          d->AddRectFilled(b0, ImVec2(b0.x + w * float(std::min(1.0, double(got) / double(total))), b1.y), kit::kGreen);
        } else {
          const float t = float(std::fmod(ImGui::GetTime() * 0.6, 1.0));
          d->AddRectFilled(ImVec2(b0.x + w * std::max(0.0f, t * 1.3f - 0.3f), b0.y), ImVec2(b0.x + w * std::min(1.0f, t * 1.3f), b1.y),
                           kit::kGreen);
        }
        d->AddRect(b0, b1, kit::kInk, 0, 0, st(2));
      }
    }
    StampCorner(d, st);
    if (failed && in.accept) CheckForUpdate("Try again on the update page");
    if (failed && in.back) update_asked_ = false, page_ = pre_game_ ? Page::kHome : Page::kMain;
  }
