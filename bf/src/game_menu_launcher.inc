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
  void AfterShaders() {
    if (setup_shader_step_) {  // the first start's step 8 is done: Ready
      setup_shader_step_ = false;
      page_ = Page::kAllSet, allset_row_ = 0, row_ = 0, scroll_ = 0;
      return;
    }
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
        // Step 8, the shaders: prepared now, with the settings the pages before chose (PrepareShadersAtStart), then
        // Ready. Already prepared (the setup run again over an install that has them): straight to Ready.
        if (PrepareShadersAtStart("the first start's step 8")) setup_shader_step_ = true;
        else page_ = Page::kAllSet, allset_row_ = 0;
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
      case Page::kShaders: return 8;
      case Page::kAllSet: return 9;
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
  // A tier's settings applied, its frame rate too: the highest offered that divides into 30 (the screen's and a limit
  // outside the game allowing), 60 at most below the fast tier (a rate it cannot keep comes unevenly).
  void ApplyTier(int tier, double slowness, const char* when) {
    rex::ui::Window* window = window_ ? window_() : nullptr;
    const int monitor = std::atoi(settings_.Get("monitor", "0").c_str());
    const DisplayMode desktop = DesktopDisplayMode(monitor, window);
    const int refresh = int(std::lround(desktop.refresh));
    std::string fps = "60";
    for (const std::string& r : OfferedRates())
      if (r != "unlimited" && std::atoi(r.c_str()) > std::atoi(fps.c_str()) && (tier == 2 || std::atoi(r.c_str()) <= 60))
        fps = r;
    settings_.values["frame_rate"] = fps;
    settings_.values["vsync"] = "off";
    settings_.Save();
    ApplyFrameRate();
    for (const auto& [name, value] : TierSettings(tier)) SetCvar(name, value);
    static const char* kTiers[] = {"slow: the Low preset (FSR 1 Performance, AO and GI off, grass to 45 m)",
                                   "middle: the Medium preset (FSR 1 Quality, shadows Original, AO and GI Low, grass to 180 m)",
                                   "fast: the High preset (FSR 1 Quality, shadows High, AO and GI at Medium, grass to the whole map)"};
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
    if (settings_.Get("nis_support", "") == value && settings_.Get("nis_support_gpu", "") == gpu) return;
    settings_.values["nis_support"] = value;
    settings_.values["nis_support_gpu"] = gpu;
    settings_.Save();
    REXLOG_INFO("BF upscaler: NVIDIA NIS {} on {}: {} in the Upscaler row from now on", runs ? "runs" : "does not run",
                gpu, runs ? "offered" : "not offered");
  }
  bool nis_noted_ = false;

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
                                                 : std::vector<Cue>{{kit::Button::kA, "Choose game files"},
                                                                    {kit::Button::kX, "Choose a folder"},
                                                                    {kit::Button::kY, "Where do I find them?"},
                                                                    {kit::Button::kB, "Quit"}};
    Prompts(d, st, st.P(150, 720 - 30 - 42), prompts, in);
    const ImVec2 a = st.P(150, 86), b = st.P(1130, 520);
    int act = -1;  // 0 the package, 1 a folder
    {
      kit::Turn turn(d, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), -0.4f);
      kit::Paper(d, a, b, st.u);
      const float x = a.x + st(44), w = st(892);
      float y = a.y + st(34);
      kit::Spaced(d, f.typed, st(15), ImVec2(x, y), kit::kInkFaint, "FIRST TIME SETUP \xC2\xB7 STEP 1 OF 9", st(2));
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
      if (!busy && !browsing_ && (in.left || in.right || in.up || in.down)) files_card_ = 1 - files_card_;
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
    shaders_shown_ = true;
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    // The first start's step 8 is drawn in the setup's frame (game_menu_setup.inc: the steps' rail, the page on
    // paper); after an update the page stands alone, as before.
    const bool setup = setup_shader_step_;
    static const char* const kAbout = "Your graphics card is building every shader the game uses, once, so nothing stutters or pops "
                                      "in while you play. This happens after you install and after each update.";
    const bool full = std::atoi(Cvar("native_x_renderer_compile_threads").c_str()) > 0;
    SetupPanel panel{};
    if (setup) {
      panel = SetupFrame(d, io, st, 8);
      Prompts(d, st, st.P(48, 662), {{kit::Button::kA, full ? "FULL CPU: ON" : "FULL CPU: OFF"}}, in, 15);
    } else {
      Chrome(d, io, st, 0);
      Prompts(d, st, st.P(40, 720 - 26 - 40), {{kit::Button::kA, full ? "FULL CPU: ON" : "FULL CPU: OFF"}}, in, 16);
    }
    const float fraction = p.loading && p.queued ? std::clamp(float(p.done) / float(p.queued), 0.0f, 1.0f) : -1.0f;
    shader_bar_ = fraction < 0 ? 0 : std::max(shader_bar_, shader_bar_ + (fraction - shader_bar_) * 0.15f);
    if (fraction >= 0 && shader_started_ < 0) shader_started_ = ImGui::GetTime(), shader_started_done_ = p.done;
    const ImVec2 a = st.P(240, 130), b = st.P(1040, 500);
    int clicked = -1;
    {
      std::optional<kit::Turn> turn;
      float x = a.x + st(44), w = st(712), y = a.y + st(34);
      if (setup) {
        x = panel.a.x + st(24), w = panel.b.x - panel.a.x - st(48);
        y = SetupTitle(d, st, panel, 8, "PREPARING THE SHADERS", kAbout) + st(8);
      } else {
        turn.emplace(d, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), 0.4f);
        kit::Paper(d, a, b, st.u, false);
        kit::Spaced(d, f.typed, st(15), ImVec2(x, y), kit::kInkFaint,
                    ShadersAfterUpdate() ? "UPDATED TO VERSION " + std::string(BfVersion()) : "BEFORE YOU PLAY", st(2));
        y += st(24);
        kit::Heading(d, st(56), ImVec2(x, y), kit::kInk, "PREPARING THE SHADERS", st(2), w);
        y += st(68);
        y += kit::Wrapped(d, f.ui, st(19), ImVec2(x, y), kit::kInkSoft, kAbout, w) + st(20);
      }
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
    if (!setup) StampCorner(d, st);  // the setup's frame has its own
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
    if (int(name_text_.size()) >= kNameMax || !NameChar(c)) return;
    if (std::isalpha(static_cast<unsigned char>(c)))
      c = name_caps_ ? char(std::toupper(static_cast<unsigned char>(c))) : char(std::tolower(static_cast<unsigned char>(c)));
    name_text_ += c;
    name_error_.clear();
    if (std::isalpha(static_cast<unsigned char>(c)) && name_caps_ && name_text_.size() == 1) name_caps_ = false;
  }
  void NameDelete() {
    if (!name_text_.empty()) name_text_.pop_back();
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
  void NameSave() {
    const std::string name = name_text_;
    if (name.size() < 3) {
      name_error_ = "A name needs 3 to 15 letters, numbers or underscores.";
      return;
    }
    // Our profanity filter (text_filter.h): such a name is never saved; the player chooses another.
    if (!text_filter::NameAllowed(name)) {
      name_error_ = "That name is not allowed. Choose another one.";
      REXLOG_INFO("BF name: a name with profanity was refused on the name page");
      return;
    }
    const bool changed = name != settings_.Get("player_name", "") || settings_.Get("player_name_chosen", "") != "1";
    settings_.values["player_name"] = name;
    settings_.values["player_name_chosen"] = "1";
    settings_.values.erase("player_name_refused");
    name_required_ = false;
    settings_.Save();
    REXLOG_INFO("BF name: the player chose {}", name);
    // Before BF signs in (the setup on a first start) the name is used at once; after, from the next start.
    if (!signed_in_once_) NameChosen(name), name_ready_at_ = ImGui::GetTime();
    else if (changed) name_saved_ = name;
    if (name_setup_) SetupNext();
    else page_ = name_from_;
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
  void AccountPage(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("account");
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    Chrome(d, io, st, name_setup_ ? 2 : 0, SceneBehind() ? 0.82f : 0);
    // The keyboard's own typing (always, as typed), and the pad on the keys after A. The window gets typed characters
    // only while ImGui asks for text input (the SDK starts SDL's text input from ImGui's IME data, which only ImGui's
    // own text fields set): this page asks for it every frame, or no key typed anything (TekRantGaming, 8 Nov).
    {
      ImGuiContext& g = *ImGui::GetCurrentContext();
      g.PlatformImeData.WantTextInput = true;
      g.PlatformImeData.ViewportId = ImGui::GetMainViewport()->ID;
    }
    if (io.InputQueueCharacters.Size > 0 && !typed_logged_) {
      typed_logged_ = true;
      REXLOG_INFO("BF launcher: the keyboard typed on the name page");
    }
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
      const ImWchar ch = io.InputQueueCharacters[i];
      if (ch < 128 && NameChar(char(ch)) && int(name_text_.size()) < kNameMax) {
        name_text_ += char(ch);
        name_error_.clear(), name_caps_ = false;
        name_keys_ = false;  // a real keyboard types: the on screen keys close, so its Enter saves
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) NameDelete();
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) NameRandom();
    std::vector<Cue> prompts;
    if (name_keys_) {
      prompts = {{kit::Button::kA, "Type"}, {kit::Button::kX, "Delete", "Backspace"},
                 {kit::Button::kStart, name_setup_ ? "Save and continue" : "Save", "Enter", true},
                 {kit::Button::kB, "Done typing", "Esc"}};
    } else if (device_ == Device::kKeyboard) {
      // The keyboard types straight into the name: no Edit (its Enter saves).
      prompts = {{kit::Button::kY, "Random name", "Tab"},
                 {kit::Button::kStart, name_setup_ ? "Save and continue" : "Save", "Enter", true}};
      if (!name_setup_ && !name_required_) prompts.push_back({kit::Button::kB, "Back", "Esc"});
    } else {
      prompts = {{kit::Button::kA, "Edit name", "Enter"}, {kit::Button::kY, "Random name", "Tab"},
                 {kit::Button::kStart, name_setup_ ? "Save and continue" : "Save", "Enter", true}};
      if (!name_setup_ && !name_required_) prompts.push_back({kit::Button::kB, "Back", "Esc"});
    }
    Prompts(d, st, st.P(120, 720 - 30 - 42), prompts, in);
    // Enter here is the keyboard's after the merge with the controller (MergeInput), not the raw key: Steam's desktop
    // layout gives a controller's A as Enter too, which saved an empty name instead of opening the keys. With the on
    // screen keys open, Enter types the key under the cursor like A (Start saves).
    const bool enter = in.accept && accept_from_key_;
    if (in.accept || in.start || in.back || in.x || in.y)
      REXLOG_INFO("BF launcher: name page {} from the {}{}",
                  in.accept ? "A" : in.start ? "Start" : in.back ? "B" : in.x ? "X" : "Y",
                  in.accept && accept_from_key_ ? "keyboard" : device_ == Device::kPad ? "controller" : "keyboard or mouse",
                  name_keys_ ? " (keys open)" : "");
    const ImVec2 a = st.P(120, 110), b = st.P(820, 560);
    {
      kit::Turn turn(d, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), -0.5f);
      kit::Paper(d, a, b, st.u, false);
      const float x = a.x + st(40), w = st(620);
      float y = a.y + st(32);
      kit::Spaced(d, f.typed, st(15), ImVec2(x, y), kit::kInkFaint,
                  name_setup_ ? "STEP 2 OF 9 \xC2\xB7 ACCOUNT" : "ACCOUNT", st(2));
      y += st(24);
      kit::Heading(d, st(52), ImVec2(x, y), kit::kInk, "CHOOSE YOUR SOLDIER NAME", st(2), w);
      y += st(64);
      y += kit::Wrapped(d, f.ui, st(18), ImVec2(x, y), kit::kInkSoft,
                        signed_in_once_ && !name_setup_
                            ? "This is the name other players see on the scoreboard and in the server list, from your "
                              "next start. If someone on a server already uses it, a number is added for that server."
                            : "This is the name other players see on the scoreboard and in the server list. If someone "
                              "on a server already uses it, a number is added for that server.",
                        w) +
           st(20);
      kit::Spaced(d, f.typed, st(14), ImVec2(x, y), kit::kInkFaint, "SOLDIER NAME", st(2));
      y += st(22);
      const ImVec2 n0(x, y), n1(x + w, y + st(58));
      d->AddRectFilled(n0, n1, kit::kPaperLight);
      d->AddRect(n0, n1, kit::kGreen, 0, 0, st(3));
      d->AddText(f.typed, st(30), ImVec2(x + st(14), y + st(11)), kit::kInk, name_text_.c_str());
      if (std::fmod(ImGui::GetTime(), 1.0) < 0.6) {
        const float cx = x + st(16) + kit::TextWidth(f.typed, st(30), name_text_);
        d->AddRectFilled(ImVec2(cx, y + st(12)), ImVec2(cx + st(3), y + st(46)), kit::kInk);
      }
      const std::string count = std::to_string(name_text_.size()) + " / " + std::to_string(kNameMax);
      d->AddText(f.typed, st(14), ImVec2(x + w - st(14) - kit::TextWidth(f.typed, st(14), count), y + st(22)),
                 kit::kInkFaint, count.c_str());
      y += st(66);
      d->AddText(f.typed, st(14), ImVec2(x, y), name_error_.empty() ? kit::kInkFaint : kit::kRed,
                 name_error_.empty() ? "3 to 15 letters, numbers or underscores" : name_error_.c_str());
      y += st(34);
      d->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + st(64)), kit::kPaperDim);
      d->AddText(f.ui, st(20), ImVec2(x + st(14), y + st(8)), kit::kInk, RankLine().c_str());
      d->AddText(f.typed, st(14), ImVec2(x + st(14), y + st(38)), kit::kInkFaint, "Your rank grows as you play.");
    }
    // The on screen keys, QWERTY (TekRantGaming, 8 Nov): the numbers, three rows of letters with the underscore, then
    // Capitals and Done. Ten columns; a key's span is in columns.
    struct OnKey {
      const char* label;
      char ch;  // the character typed, or 0: a special key (kind)
      int kind;  // 0 a character, 1 Delete, 2 Capitals, 3 Done
      int row, col, span;
    };
    static const std::vector<OnKey> kOnKeys = [] {
      std::vector<OnKey> v;
      static const char* kRows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL_", "ZXCVBNM"};
      static char labels[40][2] = {};
      int n = 0;
      for (int r = 0; r < 4; ++r)
        for (int c = 0; kRows[r][c]; ++c, ++n) {
          labels[n][0] = kRows[r][c];
          v.push_back({labels[n], kRows[r][c], 0, r, c, 1});
        }
      v.push_back({"DEL", 0, 1, 3, 7, 3});
      v.push_back({"CAPS", 0, 2, 4, 0, 5});
      v.push_back({"DONE", 0, 3, 4, 5, 5});
      return v;
    }();
    const int kAll = int(kOnKeys.size());
    if (name_key_ < 0 || name_key_ >= kAll) name_key_ = 10;
    const ImVec2 k0 = st.P(846, 150);
    kit::Spaced(d, f.typed, st(14), k0, kit::kKhakiText, "ON SCREEN KEYBOARD", st(2));
    const float cell = st(36), gap = st(4.5f), kh = st(40);
    auto key_rect = [&](int k, ImVec2& c0, ImVec2& c1) {
      const OnKey& key = kOnKeys[k];
      c0 = ImVec2(k0.x + float(key.col) * (cell + gap), k0.y + st(26) + float(key.row) * (kh + gap));
      c1 = ImVec2(c0.x + float(key.span) * cell + float(key.span - 1) * gap, c0.y + kh);
    };
    int clicked = -1;
    for (int k = 0; k < kAll; ++k) {
      ImVec2 c0, c1;
      key_rect(k, c0, c1);
      const OnKey& key = kOnKeys[k];
      std::string label = key.label;
      if (std::isalpha(static_cast<unsigned char>(key.ch)) && !name_caps_)
        label[0] = char(std::tolower(static_cast<unsigned char>(label[0])));
      const bool hover = ImGui::IsMouseHoveringRect(c0, c1);
      const bool sel = name_keys_ && k == name_key_;
      d->AddRectFilled(c0, c1, sel ? kit::kGreen : hover ? kit::kPaper : kit::kPaperCard);
      if (key.kind == 2 && name_caps_) d->AddRectFilled(ImVec2(c0.x, c1.y - st(3)), c1, sel ? kit::kCream : kit::kGreen);
      const float size = key.kind == 0 ? st(18) : st(15);
      d->AddText(f.ui, size, ImVec2((c0.x + c1.x - kit::TextWidth(f.ui, size, label)) * 0.5f, c0.y + (kh - size * 1.48f) * 0.5f),
                 sel ? IM_COL32(255, 255, 255, 255) : kit::kInk, label.c_str());
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = k, name_key_ = k;
    }
    kit::Wrapped(d, f.typed, st(13), ImVec2(k0.x, k0.y + st(26) + 5 * (kh + gap) + st(6)), kit::kKhakiText,
                 "With a keyboard, just type. With a controller, move onto these keys with the D-pad or A.", st(400));
    StampCorner(d, st);
    // The pad on the keys: along the row, and up or down to the key nearest above or below.
    const bool pad_accept = in.accept && !enter;
    if (name_keys_) {
      const OnKey& now = kOnKeys[name_key_];
      if (in.left || in.right) {
        std::vector<int> row;
        for (int k = 0; k < kAll; ++k)
          if (kOnKeys[k].row == now.row) row.push_back(k);
        const int at = int(std::find(row.begin(), row.end(), name_key_) - row.begin()), n = int(row.size());
        name_key_ = row[(at + (in.right ? 1 : n - 1)) % n];
      }
      if (in.up || in.down) {
        const int to = (now.row + (in.down ? 1 : 4)) % 5;
        const float mid = float(now.col) + float(now.span) * 0.5f;
        int best = name_key_;
        float best_d = 1e9f;
        for (int k = 0; k < kAll; ++k) {
          if (kOnKeys[k].row != to) continue;
          const float dist = std::fabs(float(kOnKeys[k].col) + float(kOnKeys[k].span) * 0.5f - mid);
          if (dist < best_d) best_d = dist, best = k;
        }
        name_key_ = best;
      }
      if (in.accept) clicked = name_key_;
      if (in.x) NameDelete();
      if (in.back) name_keys_ = false, in.back = false;
    } else if (pad_accept || in.up || in.down || in.left || in.right) {
      // A, or a direction (the D-pad or the stick), goes onto the keys (TekRantGaming, 9 Nov: the D-pad did nothing here).
      name_keys_ = true;
    }
    if (clicked >= 0) {
      const OnKey& key = kOnKeys[clicked];
      REXLOG_INFO("BF launcher: on screen key {}", key.label);
      if (key.kind == 0) {
        NameType(key.ch);
      } else if (key.kind == 1) {
        NameDelete();
      } else if (key.kind == 2) {
        name_caps_ = !name_caps_;
      } else {
        NameSave();
        return;
      }
    }
    if (!name_keys_ && in.y) NameRandom();
    // Enter with no name yet (too short to save) opens the keys, as A does: Steam's Enter for a controller's A can come
    // just before the A itself, which the merge then drops.
    if (enter && !name_keys_ && name_text_.size() < 3) {
      name_keys_ = true;
    } else if (in.start || (enter && !name_keys_)) {
      NameSave();
      return;
    }
    if (in.back && !name_setup_ && !name_required_) page_ = name_from_;
#if defined(BF_DEV_TOOLS)
    // BF_DEV_NAME=<name> (tests): typed for the player 5 s after the page first shows, then saved.
    static double dev_at = -1;
    if (dev_at < 0) dev_at = ImGui::GetTime();
    if (const char* v = std::getenv("BF_DEV_NAME"); v && *v && ImGui::GetTime() - dev_at > 5.0) {
      dev_at = 1e30;
      name_text_.clear();
      for (const char* c = v; *c; ++c)
        if (NameChar(*c)) name_text_ += *c;
      NameSave();
    }
#endif
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
      kit::Spaced(d, f.typed, st(15), ImVec2(x, y), kit::kInkFaint, "FIRST TIME SETUP \xC2\xB7 STEP 9 OF 9", st(2));
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
      const bool up = list_.reachable();
      const std::string t = up ? fmt::format("{} PLAYERS  /  {} {}", list_.players_online(), servers.size(),
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
      const float nw = std::max(kit::TextWidth(f.ui_bold, st(22), name), kit::SpacedWidth(f.typed, st(13), rank, 0));
      const std::vector<Cue> e = {{kit::Button::kBack, "Edit"}};
      const float ew = PromptsWidth(st, e, 14);
      const float w = st(12) + st(44) + st(14) + nw + st(18) + ew + st(10);
      const ImVec2 a(st.P(1240, 0).x - w, st.P(0, 24).y), b(st.P(1240, 0).x, st.P(0, 84).y);
      d->AddRectFilled(a, b, IM_COL32(15, 17, 13, 214));
      d->AddRectFilled(a, ImVec2(a.x + st(4), b.y), kit::kStamp);
      RankBadge(d, st, ImVec2(a.x + st(14) + st(22), a.y + st(30)), Rank());
      d->AddText(f.ui_bold, st(22), ImVec2(a.x + st(72), a.y + st(7)), kit::kCream, name.c_str());
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
      case 2: return;  // Unlockables: nothing yet (TekRantGaming, 8 Nov)
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

  void StatsPage(ImDrawList* d, ImGuiIO& io, const Input& in) {
    const ImVec2 sz = io.DisplaySize;
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    MenuBackdrop(d, sz, StageOf(io));  // the new look's backdrop
    d->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(12, 14, 11, 170));
    const float cx0 = st.P(40, 0).x, cx1 = cx0 + st(400);
    d->AddRectFilled(ImVec2(0, 0), ImVec2(cx1, sz.y), IM_COL32(15, 17, 13, 226));
    d->AddRectFilled(ImVec2(cx1, 0), ImVec2(cx1 + st(1.4f), sz.y), IM_COL32(201, 183, 122, 110));
    kit::Spaced(d, f.typed, st(12), st.P(40, 46), kit::kKhakiDim,
                test_profile_active_ ? "STATS & AWARDS / TEST PROFILE" : "STATS & AWARDS", st(3));
    Text(d, f.stencil, st(40), ImVec2(cx0 - st(2), st.P(0, 66).y), kit::kCream, "SERVICE RECORD");
    d->AddRectFilled(st.P(40, 120), st.P(80, 124), kit::kStamp);
    // The soldier.
    const std::string name = settings_.Get("player_name", "Soldier");
    const Settings& record = test_profile_active_ ? test_profile_ : settings_;
    const double raw_seconds = std::atof(record.Get("match_seconds", "0").c_str());
    const double seconds = std::isfinite(raw_seconds) ? std::clamp(raw_seconds, 0.0, 2147483647.0) : 0.0;
    const int rank_number = Rank(record, seconds);
    RankBadge(d, st, st.P(62, 172), rank_number);
    d->AddText(f.ui_bold, st(26), st.P(96, 150), kit::kCream, name.c_str());
    const std::string rank = "RANK " + std::to_string(rank_number) + " \xC2\xB7 " + Upper(kRanks[rank_number - 1]);
    d->AddText(f.typed, st(14), st.P(96, 184), kit::kKhakiText, rank.c_str());
    // The totals, from the stat.<label> lines.
    struct Line {
      std::string label;
      int events;
      double points;
    };
    std::vector<Line> lines;
    double total = 0;
    for (const auto& [key, value] : record.values) {
      if (key.rfind("stat.", 0) != 0) continue;
      const size_t comma = value.find(',');
      const char* count_end = value.data() + std::min(comma, value.size());
      int events = 0;
      const auto count = std::from_chars(value.data(), count_end, events);
      if (count.ec != std::errc{} || count.ptr != count_end || events < 0) continue;
      const double points = comma == std::string::npos ? 0 : std::atof(value.c_str() + comma + 1);
      if (!std::isfinite(points)) continue;
      const Line l{key.substr(5), events, points};
      total += l.points;
      lines.push_back(l);
    }
    std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.points > b.points; });
    auto pretty = [](std::string s) {  // CQ_StringForFlagCapture -> FLAG CAPTURE
      for (const char* p : {"CQ_StringFor", "StringFor", "CQ_"})
        if (s.rfind(p, 0) == 0) s = s.substr(std::strlen(p));
      std::string out;
      for (size_t i = 0; i < s.size(); ++i) {
        if (i && std::isupper(static_cast<unsigned char>(s[i])) && std::islower(static_cast<unsigned char>(s[i - 1])))
          out += ' ';
        out += char(std::toupper(static_cast<unsigned char>(s[i] == '_' ? ' ' : s[i])));
      }
      return out;
    };
    float y = st.P(0, 232).y;
    auto row = [&](const std::string& k, const std::string& v) {
      kit::Spaced(d, f.typed, st(13), ImVec2(cx0, y), kit::kKhakiDim, k, st(2));
      d->AddText(f.ui_bold, st(24), ImVec2(cx0, y + st(18)), kit::kCream, v.c_str());
      y += st(62);
    };
    row("TIME IN MATCHES", fmt::format("{} H {:02d} MIN", int(seconds / 3600), int(seconds / 60) % 60));
    row("SCORE", fmt::format("{:.0f}", total));
    // Every kind of event, at the right.
    const ImVec2 pa(st.P(470, 140)), pb(st.P(1240, 640));
    d->AddRectFilled(pa, pb, IM_COL32(15, 17, 13, 214));
    d->AddRectFilled(pa, ImVec2(pa.x + st(4), pb.y), kit::kStamp);
    kit::Spaced(d, f.typed, st(13), ImVec2(pa.x + st(28), pa.y + st(22)), kit::kKhakiDim, "IN THE FIELD", st(3));
    float ly = pa.y + st(56);
    if (lines.empty())
      kit::Wrapped(d, f.typed, st(15), ImVec2(pa.x + st(28), ly), kit::kKhakiText,
                   "Nothing yet. Captures, kills and the rest of your score show here after your first match.",
                   pb.x - pa.x - st(56));
    for (const Line& l : lines) {
      if (ly > pb.y - st(40)) break;
      d->AddText(f.ui_bold, st(20), ImVec2(pa.x + st(28), ly), kit::kCream, pretty(l.label).c_str());
      const std::string v = fmt::format("{} x   {:+.0f}", l.events, l.points);
      d->AddText(f.typed, st(16), ImVec2(pb.x - st(28) - kit::TextWidth(f.typed, st(16), v), ly + st(3)), kit::kKhakiText,
                 v.c_str());
      d->AddRectFilled(ImVec2(pa.x + st(28), ly + st(34)), ImVec2(pb.x - st(28), ly + st(35)), IM_COL32(201, 183, 122, 30));
      ly += st(44);
    }
    Prompts(d, st, st.P(40, 720 - 24 - 34), {{kit::Button::kB, "BACK"}}, in_copy_ = in, 15);
    if (in.back) BackToMenu(kRowStats, 4);  // back on the menu's Stats & Awards row
  }
  Input in_copy_;
  // Achievements, ours (in place of the Xbox screen the game opened): the game's own list from its title data, with
  // what this player unlocked (the runtime's achievement manager keeps the game's own unlocks), scrolled with the pad.
  int ach_scroll_ = 0;
  void AchievementsPage(ImDrawList* d, ImGuiIO& io, const Input& in) {
    const ImVec2 sz = io.DisplaySize;
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    MenuBackdrop(d, sz, StageOf(io));  // the new look's backdrop
    d->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(12, 14, 11, 186));
    std::vector<rex::system::AchievementInfo> list;
    int unlocked = 0, score = 0, total = 0;
    auto* k = REX_KERNEL_STATE();
    const std::string default_state = test_profile_.Get("achievements", "actual");
    auto is_unlocked = [&](uint32_t id) {
      if (test_profile_active_) {
        const std::string state = test_profile_.Get("achievement." + std::to_string(id), default_state);
        if (state == "unlocked") return true;
        if (state == "locked") return false;
      }
      return k && k->achievements().IsUnlocked(id);
    };
    if (k) {
      list = k->achievements().ListAchievements();
      for (const auto& a : list) {
        total += int(a.gamerscore);
        if (is_unlocked(a.id)) ++unlocked, score += int(a.gamerscore);
      }
    }
    kit::Spaced(d, f.typed, st(12), st.P(40, 46), kit::kKhakiDim,
                test_profile_active_ ? "BATTLEFIELD 1943 / TEST PROFILE" : "BATTLEFIELD 1943", st(3));
    Text(d, f.stencil, st(40), ImVec2(st.P(40, 0).x - st(2), st.P(0, 66).y), kit::kCream, "ACHIEVEMENTS");
    d->AddRectFilled(st.P(40, 120), st.P(80, 124), kit::kStamp);
    const std::string head = fmt::format("{} OF {} UNLOCKED  /  {} OF {} POINTS", unlocked, list.size(), score, total);
    kit::Spaced(d, f.typed, st(14), st.P(40, 140), kit::kKhakiText, head, st(2));
    const int per = 7;
    const int pages = std::max(1, int(list.size() + per - 1) / per);
    if (in.down || in.next_tab) ach_scroll_ = std::min(ach_scroll_ + 1, pages - 1);
    if (in.up || in.prev_tab) ach_scroll_ = std::max(ach_scroll_ - 1, 0);
    if (io.MouseWheel < 0) ach_scroll_ = std::min(ach_scroll_ + 1, pages - 1);
    if (io.MouseWheel > 0) ach_scroll_ = std::max(ach_scroll_ - 1, 0);
    float y = st.P(0, 176).y;
    const float x0 = st.P(40, 0).x, x1 = st.P(1240, 0).x;
    if (list.empty())
      kit::Wrapped(d, f.typed, st(15), ImVec2(x0, y), kit::kKhakiText,
                   "The game's achievements are not loaded in this session.", x1 - x0);
    for (int i = ach_scroll_ * per; i < int(list.size()) && i < (ach_scroll_ + 1) * per; ++i) {
      const auto& a = list[size_t(i)];
      const bool got = is_unlocked(a.id);
      const ImVec2 ra(x0, y), rb(x1, y + st(62));
      d->AddRectFilled(ra, rb, IM_COL32(15, 17, 13, got ? 226 : 180));
      d->AddRectFilled(ra, ImVec2(ra.x + st(4), rb.y), got ? kit::kStamp : IM_COL32(201, 183, 122, 60));
      d->AddText(f.ui_bold, st(20), ImVec2(ra.x + st(24), ra.y + st(8)), got ? kit::kCream : kit::kKhakiDim,
                 a.label.c_str());
      const std::string& desc = got || a.unachieved_description.empty() ? a.description : a.unachieved_description;
      d->AddText(f.typed, st(13), ImVec2(ra.x + st(24), ra.y + st(36)), got ? kit::kKhakiText : kit::kKhakiDim, desc.c_str());
      const std::string pts = test_profile_active_
          ? fmt::format("ID {}  /  {}{}", a.id, a.gamerscore, got ? "  UNLOCKED" : "")
          : fmt::format("{}{}", a.gamerscore, got ? "  UNLOCKED" : "");
      d->AddText(f.typed, st(14), ImVec2(rb.x - st(24) - kit::TextWidth(f.typed, st(14), pts), ra.y + st(22)),
                 got ? kit::kGreenBright : kit::kKhakiDim, pts.c_str());
      y += st(68);
    }
    if (pages > 1)
      kit::Spaced(d, f.typed, st(12), ImVec2(x1 - st(120), st.P(0, 140).y), kit::kKhakiDim,
                  fmt::format("PAGE {} OF {}", ach_scroll_ + 1, pages), st(2));
    Input cue = in;
    Prompts(d, st, st.P(40, 720 - 24 - 34), {{kit::Button::kB, "BACK"}}, cue, 15);
    if (in.back) {  // back where it was opened: the pause, or the main menu's Achievements row
      if (ach_from_pause_) page_ = Page::kPause, pause_row_ = kPauseAchievements, ach_from_pause_ = false;
      else BackToMenu(kRowAchievements, 6);
    }
  }
  // Leaderboards, ours (in place of the game's EA screen, docs/MENUS-PLAN.md): every player's own totals as their
  // games send them to our backend (stats_put after each save of Stats & Awards), ranked by score; asked every 15 s
  // on a worker while the page is open.
  struct LbRow {
    std::string name;
    int score = 0, seconds = 0, captures = 0;
  };
  std::mutex lb_mutex_;
  std::vector<LbRow> lb_rows_;
  int lb_total_ = 0;
  bool lb_ok_ = false, lb_answered_ = false;
  double lb_asked_at_ = -100;
  std::atomic<bool> lb_busy_{false};
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
    std::thread([req, test, name, total, seconds, why] {
      static constexpr int kWait[] = {0, 5, 15, 30};
      for (int attempt = 0; attempt < int(std::size(kWait)); ++attempt) {
        if (kWait[attempt]) std::this_thread::sleep_for(std::chrono::seconds(kWait[attempt]));
        std::string reply;
        // A test profile's made-up rank and awards go to a backend on this PC only.
        const bool local = std::string(REXCVAR_GET(bf_backend_registry)).rfind("127.", 0) == 0;
        const bool answered = (!test || local) && RegistryRequest(req, reply);
        const bool ok = answered && reply.find("\"ok\": true") != std::string::npos;
        if (ok) {
          REXLOG_INFO("BF leaderboard: stats posted for {} ({}: score {:.0f}, {:.0f} s played{})", name, why, total,
                      seconds, test ? ", test profile, local backend only" : "");
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

  void LeaderboardsPage(ImDrawList* d, ImGuiIO& io, const Input& in) {
    const ImVec2 sz = io.DisplaySize;
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    if (ImGui::GetTime() - lb_asked_at_ > 15 && !lb_busy_.load()) {
      lb_asked_at_ = ImGui::GetTime();
      lb_busy_ = true;
      std::thread([this] {
        std::string reply;
        const bool ok = RegistryRequest("{\"op\": \"leaderboard\"}", reply);
        std::vector<LbRow> rows;
        if (ok) {
          size_t at = reply.find("\"rows\"");
          while (at != std::string::npos && rows.size() < 50) {
            const size_t a = reply.find('{', at), b = a == std::string::npos ? a : reply.find('}', a);
            if (a == std::string::npos || b == std::string::npos) break;
            const std::string obj = reply.substr(a, b - a + 1);
            LbRow r;
            r.name = JsonField(obj, "name");
            r.score = std::atoi(JsonField(obj, "score").c_str());
            r.seconds = std::atoi(JsonField(obj, "seconds").c_str());
            r.captures = std::atoi(JsonField(obj, "captures").c_str());
            if (!r.name.empty()) rows.push_back(r);
            at = b + 1;
          }
        }
        std::lock_guard lock(lb_mutex_);
        lb_rows_ = std::move(rows);
        lb_total_ = ok ? std::atoi(JsonField(reply, "total").c_str()) : 0;
        lb_ok_ = ok, lb_answered_ = true;
        lb_busy_ = false;
      }).detach();
    }
    MenuBackdrop(d, sz, StageOf(io));  // the new look's backdrop
    d->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(12, 14, 11, 186));
    kit::Spaced(d, f.typed, st(12), st.P(40, 46), kit::kKhakiDim, "EVERY PLAYER, BY SCORE", st(3));
    Text(d, f.stencil, st(40), ImVec2(st.P(40, 0).x - st(2), st.P(0, 66).y), kit::kCream, "LEADERBOARDS");
    d->AddRectFilled(st.P(40, 120), st.P(80, 124), kit::kStamp);
    std::vector<LbRow> rows;
    int total = 0;
    bool ok = false, answered = false;
    {
      std::lock_guard lock(lb_mutex_);
      rows = lb_rows_, total = lb_total_, ok = lb_ok_, answered = lb_answered_;
    }
    const float x0 = st.P(40, 0).x, x1 = st.P(1240, 0).x;
    const std::string mine = settings_.Get("player_name", "");
    kit::Spaced(d, f.typed, st(14), st.P(40, 140), kit::kKhakiText,
                !answered ? "ASKING THE SERVERS..." : !ok ? "THE SERVERS ARE NOT REACHABLE" : fmt::format("{} {}", total, total == 1 ? "PLAYER" : "PLAYERS"),
                st(2));
    float y = st.P(0, 176).y;
    const float cols[] = {x0 + st(24), x0 + st(90), x1 - st(420), x1 - st(260), x1 - st(110)};
    const char* heads[] = {"#", "SOLDIER", "SCORE", "TIME", "FLAGS"};
    for (int c = 0; c < 5; ++c) kit::Spaced(d, f.typed, st(11), ImVec2(cols[c], y), kit::kKhakiDim, heads[c], st(2));
    y += st(24);
    for (size_t i = 0; i < rows.size() && y < st.P(0, 640).y; ++i) {
      const LbRow& r = rows[i];
      const bool me = !mine.empty() && r.name == mine;
      d->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + st(40)), IM_COL32(15, 17, 13, me ? 236 : 200));
      if (me) d->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + st(4), y + st(40)), kit::kStamp);
      const ImU32 ink = me ? kit::kCream : kit::kKhakiText;
      d->AddText(f.typed, st(15), ImVec2(cols[0], y + st(11)), ink, std::to_string(i + 1).c_str());
      d->AddText(f.ui_bold, st(19), ImVec2(cols[1], y + st(8)), ink, r.name.c_str());
      d->AddText(f.typed, st(15), ImVec2(cols[2], y + st(11)), ink, std::to_string(r.score).c_str());
      d->AddText(f.typed, st(15), ImVec2(cols[3], y + st(11)), ink,
                 fmt::format("{}H {:02d}M", r.seconds / 3600, (r.seconds / 60) % 60).c_str());
      d->AddText(f.typed, st(15), ImVec2(cols[4], y + st(11)), ink, std::to_string(r.captures).c_str());
      y += st(44);
    }
    Input cue = in;
    Prompts(d, st, st.P(40, 720 - 24 - 34), {{kit::Button::kB, "BACK"}}, cue, 15);
    if (in.back) {
      if (lb_from_pause_) page_ = Page::kPause, pause_row_ = kPauseLeaderboards, lb_from_pause_ = false;
      else BackToMenu(kRowLeaderboards, 7);  // back on the menu's Leaderboards row
    }
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
          if (!r.name.empty()) rows.push_back(r);
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
      d->AddText(f.ui_bold, st(21), ImVec2(a.x + st(24), a.y + st(10)), sel ? kit::kCream : kit::kKhakiText, r.name.c_str());
      const std::string where = r.gid.empty() ? (r.server.empty() ? "IN THE MENUS" : Upper(r.server))  // a private match
                                              : Upper(r.server) + (r.map.empty() ? "" : "  /  " + Upper(r.map));
      d->AddText(f.typed, st(15), ImVec2(b.x - st(24) - kit::TextWidth(f.typed, st(15), where), a.y + st(15)),
                 r.gid.empty() ? kit::kKhakiDim : kit::kKhakiText, where.c_str());
      y += st(54);
    }
    Input cue = in;
    Prompts(d, st, st.P(40, 720 - 24 - 34), {{kit::Button::kA, "JOIN"}, {kit::Button::kB, "BACK"}}, cue, 15);
    if (in.accept && n) clicked = fr_row_;
    if (clicked >= 0 && clicked < n && !rows[size_t(clicked)].gid.empty()) {
      for (const auto& sv : list_.servers())
        if (sv.gid == rows[size_t(clicked)].gid) {
          REXLOG_INFO("BF friends: joining {} on {}", rows[size_t(clicked)].name, sv.name);
          Join(sv);
          return;
        }
    }
    if (in.back) BackToMenu(kRowBrowser, 2);  // back to the menu (no Join friends row since 12 Nov 2026)
  }
  // The pause's Scoreboard, ours (in place of the game's, docs/MENUS-PLAN.md): every player on this server by score,
  // as their games send it to our backend (op match_score from MatchScoreTick), asked every 3 s on a worker.
  std::mutex sb_mutex_;
  std::vector<LbRow> sb_rows_;
  bool sb_ok_ = false, sb_answered_ = false;
  double sb_asked_at_ = -100;
  std::atomic<bool> sb_busy_{false};

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
        for (size_t at = ok ? reply.find("\"rows\"") : std::string::npos; at != std::string::npos && rows.size() < 64;) {
          const size_t a = reply.find('{', at), b = a == std::string::npos ? a : reply.find('}', a);
          if (a == std::string::npos || b == std::string::npos) break;
          const std::string obj = reply.substr(a, b - a + 1);
          LbRow r;
          r.name = JsonField(obj, "name");
          r.score = std::atoi(JsonField(obj, "score").c_str());
          if (!r.name.empty()) rows.push_back(r);
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
    const float x0 = st.P(40, 0).x, x1 = st.P(1240, 0).x;
    float y = st.P(0, 176).y;
    for (size_t i = 0; i < rows.size() && y < st.P(0, 640).y; ++i) {
      const bool me = !mine.empty() && rows[i].name == mine;
      d->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + st(40)), IM_COL32(15, 17, 13, me ? 236 : 200));
      if (me) d->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + st(4), y + st(40)), kit::kStamp);
      const ImU32 ink = me ? kit::kCream : kit::kKhakiText;
      d->AddText(f.typed, st(15), ImVec2(x0 + st(24), y + st(11)), ink, std::to_string(i + 1).c_str());
      d->AddText(f.ui_bold, st(19), ImVec2(x0 + st(90), y + st(8)), ink, rows[i].name.c_str());
      const std::string sc = std::to_string(rows[i].score);
      d->AddText(f.typed, st(16), ImVec2(x1 - st(24) - kit::TextWidth(f.typed, st(16), sc), y + st(10)), ink, sc.c_str());
      y += st(44);
    }
    Input cue = in;
    Prompts(d, st, st.P(40, 720 - 24 - 34), {{kit::Button::kB, "BACK"}}, cue, 15);
    if (in.back) page_ = Page::kPause, pause_row_ = kPauseScoreboard;
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
                       kPmRows = 8;
  // The match's tick rate: 30 by default (the console's), 60, 75 and 100 experimental.
  static constexpr int kPmTicks[] = {30, 60, 75, 100};
  int pm_tick_ = 30;
  bool pm_lan_ = false, pm_listed_ = false;
  std::string pm_name_, pm_port_text_ = "18300";
  // The row takes typing now: the name, or the port when it is the player's to choose.
  bool PmPortTyped() const { return pm_lan_ || !pm_upnp_; }
  bool PmTyping() const { return !pm_hosting_ && (pm_row_ == kPmName || (pm_row_ == kPmPort && PmPortTyped())); }
  int pm_row_ = 0, pm_map_ = 0;
  bool pm_upnp_ = true;
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
    pm_tick_ = std::atoi(settings_.Get("private_match_tick", "30").c_str());
    if (std::find(std::begin(kPmTicks), std::end(kPmTicks), pm_tick_) == std::end(kPmTicks)) pm_tick_ = 30;
  }
  void SavePrivateMatchChoices() {
    settings_.values["private_match_map"] = std::to_string(pm_map_);
    settings_.values["private_match_upnp"] = pm_upnp_ ? "1" : "0";
    settings_.values["private_match_lan"] = pm_lan_ ? "1" : "0";
    settings_.values["private_match_listed"] = pm_listed_ ? "1" : "0";
    settings_.values["private_match_name"] = pm_name_;
    settings_.values["private_match_port"] = pm_port_text_;
    settings_.values["private_match_tick"] = std::to_string(pm_tick_);
    settings_.Save();
  }

  void HostPrivateMatch() {
    bf1943::PrivateMatchOptions o;
    if (pm_map_ < 4) o.maps = {kPmMapKeys[pm_map_]};
    else o.maps = {"wake", "guadalcanal", "iwo", "coral"};
    o.upnp = pm_upnp_;
    o.lan = pm_lan_;
    if (pm_lan_) {
      // The game signs in to the match's own backend, which it can only choose before it starts (once it runs it is
      // signed in to the main one).
      if (!pre_game_ && own_backend_ != "127.0.0.1") {
        pm_error_ = "A LAN match is started from here before the game itself runs: close the game, start it again and "
                    "host the LAN match first.";
        return;
      }
      UseOwnBackend("127.0.0.1");
    }
    const std::string me = settings_.Get("player_name", "");
    // Through our profanity filter (text_filter.h): a listed match's name is everybody's to read.
    o.name = text_filter::Censor(!pm_name_.empty() ? pm_name_ : me.empty() ? "Custom match" : me + "'s match");
    o.tick = pm_tick_;
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
    REXLOG_INFO("BF private match: hosting ({})", kPmMapNames[pm_map_]);
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
    sv.map = pm_map_ < 4 ? kPmMapTitles[pm_map_] : "Wake Island";
    sv.max_players = 24;
    // Its tick rate is the one it was started at. (Left unsaid, Join took it for a 30 Hz server and set this game to
    // 30 against a 60 Hz match: 13 Nov 2026, "I cannot walk at 60Hz anymore, and player partially falls through
    // carrier".)
    sv.tick_hz = pm_tick_;
    pm_joining_ = true;
    exp_accepted_address_ = sv.address;  // its rate chosen on the Custom Match page, with that page's warning
    REXLOG_INFO("BF private match: registered as game {}; joining it", sv.gid);
    Join(sv);
  }

  // Create Private Match as the main menu is drawn (13 Nov 2026: "look more like the main menu theming, with the
  // options on the left side panel in the same font"): the menu's backdrop and chrome, the dark rail at the left with
  // the rows in the game's own lettering (the chosen one on paper with the red tab and its glyph, its value typed at
  // the right), and at the right the chosen map's picture over a paper band with the chosen row's help and the green
  // button, where the main menu has its Quick Play card. Sizes are the Stage's (720 lines).
  void PrivateMatchPage(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("private match");
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    MenuBackdrop(d, io.DisplaySize, st);
    bool online_ok = false;
    const std::string status = OnlineStatus(online_ok);
    MenuChrome(d, io, st, status, online_ok);
    MenuFoot(d, st);
    // ---- the rail and its heading
    const ImVec2 ra = st.P(48, 93), rb = st.P(400, 620);
    d->AddRectFilled(ra, rb, IM_COL32(13, 15, 11, 204));
    d->AddRect(ra, rb, IM_COL32(241, 234, 211, 30), 0, 0, 1.0f);
    kit::Spaced(d, f.typed, st(8), ImVec2(ra.x + st(24), ra.y + st(18)), kit::kKhakiDim, "HOSTED ON THIS PC", st(2));
    kit::GameText(d, kit::GameFont::kHeadline, st(26), ImVec2(ra.x + st(24), ra.y + st(32)), kit::kCream, "CUSTOM MATCH");
    d->AddRectFilled(ImVec2(ra.x, ra.y + st(76)), ImVec2(rb.x, ra.y + st(77)), IM_COL32(241, 234, 211, 30));
    const float rows_y = ra.y + st(77);
    // ---- the picture card at the right: the chosen map from the air, the paper band under it
    // (13 Nov 2026: down to the rail's foot, "getting a cleaner full view of the map"; it ended at 380.)
    const ImVec2 ha = st.P(427, 93), hb = st.P(1232, 620);
    const ImVec2 band(ha.x, hb.y - st(110));
    d->PushClipRect(ha, hb, true);
    MapPicture(d, st.u * 1.5f, ha, ImVec2(hb.x, band.y), pm_map_ < 4 ? kPmMapTitles[pm_map_] : "Wake Island");
    d->PopClipRect();
    {  // the chip
      const std::string chip = pm_lan_ ? "YOUR OWN SERVER \xC2\xB7 ON YOUR NETWORK'S LAN TAB"
                                       : "YOUR OWN SERVER \xC2\xB7 ON THE COMMUNITY TAB";
      const float cw = kit::SpacedWidth(f.ui_bold, st(9), chip, st(1));
      const ImVec2 c0(ha.x + st(20), ha.y + st(20)), c1(c0.x + cw + st(22), c0.y + st(22));
      d->AddRectFilled(c0, c1, kit::kStrip);
      kit::Spaced(d, f.ui_bold, st(9), ImVec2(c0.x + st(11), c0.y + st(4)), kit::kStripText, chip, st(1));
    }
    kit::PaperArt(d, band, hb);
    if (pm_hosting_) {
      PrivateMatchStarting(d, st, in, ImVec2(ra.x, rows_y), rb, band, hb);
      return;
    }
    // The prompts first: a click on one is that button's press for the rest of the frame.
    std::vector<Cue> cues = {{kit::Button::kA, pm_row_ == kPmHost ? "HOST" : "CHANGE"}, {kit::Button::kB, "BACK"}};
    Prompts(d, st, st.P(48, 662), cues, in, 15);
    // The rows in the panel's order (13 Nov 2026, after the first look at it: the name first with its field under it,
    // UPnP and the port as a smaller pair under Network type, and no UPnP at all for a LAN match).
    std::vector<int> order = {kPmName, kPmMap, kPmTick, kPmNet};
    if (!pm_lan_) order.push_back(kPmListed);
    if (!pm_lan_) order.push_back(kPmUpnp);
    order.insert(order.end(), {kPmPort, kPmHost});
    {
      int at = 0;
      for (int k = 0; k < int(order.size()); ++k)
        if (order[size_t(k)] == pm_row_) at = k;
      if (std::find(order.begin(), order.end(), pm_row_) == order.end()) pm_row_ = kPmNet, at = 3;  // UPnP went with LAN
      const int n = int(order.size());
      if (in.up) pm_row_ = order[size_t((at + n - 1) % n)];
      if (in.down) pm_row_ = order[size_t((at + 1) % n)];
    }
    if (PmTyping()) {  // the keyboard types straight into the row (as the browser's address)
      ImGuiContext& g = *ImGui::GetCurrentContext();
      g.PlatformImeData.WantTextInput = true;
      g.PlatformImeData.ViewportId = ImGui::GetMainViewport()->ID;
      std::string& text = pm_row_ == kPmName ? pm_name_ : pm_port_text_;
      const size_t most = pm_row_ == kPmName ? 32 : 5;
      for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const ImWchar c = io.InputQueueCharacters[i];
        const bool ok = pm_row_ == kPmName ? (c >= 32 && c < 127 && c != '"' && c != '\\') : (c >= '0' && c <= '9');
        if (ok && text.size() < most) text += char(c);
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !text.empty()) text.pop_back();
      in.left = in.right = false;
    }
    auto change = [&](int row, int step) {
      if (row == kPmMap) pm_map_ = (pm_map_ + 5 + step) % 5;
      if (row == kPmTick) {
        int at = 1;
        for (int k = 0; k < 4; ++k)
          if (kPmTicks[k] == pm_tick_) at = k;
        pm_tick_ = kPmTicks[(at + 4 + step) % 4];
      }
      if (row == kPmNet) pm_lan_ = !pm_lan_;
      if (row == kPmListed && !pm_lan_) pm_listed_ = !pm_listed_;
      if (row == kPmUpnp && !pm_lan_) pm_upnp_ = !pm_upnp_;
      SavePrivateMatchChoices();
    };
    if (in.left && pm_row_ != kPmHost) change(pm_row_, -1);
    if (in.right && pm_row_ != kPmHost) change(pm_row_, 1);
    // ---- the rows, as the main menu's. By the row's own number (kPm...): its title, and its help in the band.
    static constexpr const char* kTitles[] = {"MAP",  "SERVER NAME", "NETWORK TYPE",  "SHOW IN BROWSER",
                                              "UPNP", "PORT",        "HOST THE MATCH", "TICK RATE"};
    int clicked = -1;
    float y = rows_y;
    int number = 0;
    const std::string me = settings_.Get("player_name", "");
    for (const int i : order) {
      const bool sub = i == kPmUpnp || i == kPmPort;  // the smaller pair under Network type
      if (i == kPmHost) y += st(12);                  // a rule's worth of air before the action
      const float h = i == kPmName ? st(74) : sub ? st(29) : st(41);
      const float title_h = i == kPmName ? st(38) : h;
      const ImVec2 r0(ra.x, y), r1(rb.x, y + h);
      const bool hover = ImGui::IsMouseHoveringRect(r0, r1);
      if (hover && io.MouseDelta.x + io.MouseDelta.y != 0) pm_row_ = i;
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = i;
      const bool sel = i == pm_row_;
      if (sel) {
        kit::PaperArt(d, r0, r1);
        d->AddRectFilled(r0, ImVec2(r0.x + st(5), r1.y), kit::kStamp);
      }
      // No rule between Network type and its pair, nor inside the pair: they read as one block.
      const bool block = i == kPmNet || (i == kPmUpnp);
      if (!block) d->AddRectFilled(ImVec2(r0.x, r1.y - 1), ImVec2(r1.x, r1.y), IM_COL32(241, 234, 211, 20));
      const ImU32 dim = sel ? kit::kInkSection : kit::kKhakiDim;
      if (!sub)
        kit::Spaced(d, f.typed, st(8), ImVec2(r0.x + st(24), r0.y + title_h * 0.5f - st(5)), dim,
                    fmt::format("{:02d}", ++number), st(1.5f));
      if (sub) {
        // A short tick joins the pair to Network type above it.
        d->AddRectFilled(ImVec2(r0.x + st(58), r0.y - st(2)), ImVec2(r0.x + st(59), r0.y + h * 0.5f), IM_COL32(241, 234, 211, sel ? 0 : 60));
        d->AddRectFilled(ImVec2(r0.x + st(58), r0.y + h * 0.5f - 1), ImVec2(r0.x + st(68), r0.y + h * 0.5f), IM_COL32(241, 234, 211, sel ? 0 : 60));
        kit::Spaced(d, f.typed, st(8.5f), ImVec2(r0.x + st(76), r0.y + h * 0.5f - st(5.5f)), sel ? kit::kInk : kit::kKhaki,
                    kTitles[i], st(2));
      } else {
        const float cap = st(19);
        kit::GameText(d, kit::GameFont::kHeadlineMedium, cap, ImVec2(r0.x + st(52), r0.y + (title_h - cap) * 0.5f),
                      sel ? kit::kInk : kit::kKhaki, kTitles[i]);
      }
      const float right = r1.x - st(18), mid = r0.y + title_h * 0.5f;
      auto value_text = [&](std::string value, bool arrows, bool off) {
        if (arrows && sel && !off) value = "<  " + value + "  >";
        const float vw = kit::SpacedWidth(f.typed, st(7.5f), value, st(1.5f));
        kit::Spaced(d, f.typed, st(7.5f), ImVec2(right - vw, mid - st(5)), off ? IM_COL32(120, 116, 100, 150) : dim, value, st(1.5f));
      };
      const std::string caret = std::fmod(ImGui::GetTime(), 1.0) < 0.5 ? "_" : " ";
      if (i == kPmHost) {
        if (sel) kit::Glyph(d, PromptStyle(), kit::Button::kA, "Enter", ImVec2(right - st(20), mid), st(20));
      } else if (i == kPmName) {
        // The name's field: a box the row's width, under the title.
        const ImVec2 f0(r0.x + st(52), r0.y + st(40)), f1(r1.x - st(18), r1.y - st(9));
        d->AddRectFilled(f0, f1, sel ? IM_COL32(34, 36, 27, 22) : IM_COL32(0, 0, 0, 70));
        d->AddRect(f0, f1, sel ? kit::kInk : IM_COL32(241, 234, 211, 70), 0, 0, 1.0f);
        const bool empty = pm_name_.empty();
        const std::string shown = !empty ? pm_name_ + (sel ? caret : "") : sel ? caret : me.empty() ? "Custom match" : me + "'s match";
        d->PushClipRect(f0, f1, true);
        d->AddText(f.ui_bold, st(11), ImVec2(f0.x + st(9), f0.y + (f1.y - f0.y - st(11) * 1.45f) * 0.5f),
                   empty && !sel ? IM_COL32(241, 234, 211, 110) : sel ? kit::kInk : kit::kKhaki, shown.c_str());
        d->PopClipRect();
      } else if (i == kPmMap) {
        value_text(kPmMapNames[pm_map_], true, false);
      } else if (i == kPmTick) {
        value_text(std::to_string(pm_tick_) + (pm_tick_ == 30 ? "Hz - Default" : "Hz - Experimental"), true, false);
      } else if (i == kPmNet) {
        value_text(pm_lan_ ? "LAN" : "ONLINE", true, false);
      } else if (i == kPmListed) {
        value_text(pm_lan_ ? "ON YOUR NETWORK" : pm_listed_ ? "YES" : "NO", true, pm_lan_);
      } else if (i == kPmUpnp) {
        // An on/off switch: on, the router is asked to open the port.
        const float sw = st(26), sh2 = st(13);
        const ImVec2 s0(right - sw, mid - sh2 * 0.5f), s1(right, mid + sh2 * 0.5f);
        const ImU32 track = pm_upnp_ ? kit::kGreen : IM_COL32(120, 116, 100, 140);
        d->AddRectFilled(s0, s1, track, sh2 * 0.5f);
        const float knob = sh2 * 0.5f - st(2);
        d->AddCircleFilled(ImVec2(pm_upnp_ ? s1.x - sh2 * 0.5f : s0.x + sh2 * 0.5f, mid), knob, kit::kCream, 20);
        const std::string said = pm_upnp_ ? "ON" : "OFF";
        const float tw = kit::SpacedWidth(f.typed, st(7.5f), said, st(1.5f));
        kit::Spaced(d, f.typed, st(7.5f), ImVec2(s0.x - st(8) - tw, mid - st(5)), dim, said, st(1.5f));
      } else if (i == kPmPort) {
        // The port's field: typed when it is the player's to choose, greyed (Automatic) while the router is asked.
        const bool off = !PmPortTyped();
        const ImVec2 f0(right - st(70), r0.y + st(5)), f1(right, r1.y - st(5));
        d->AddRect(f0, f1, off ? IM_COL32(120, 116, 100, 90) : sel ? kit::kInk : IM_COL32(241, 234, 211, 70), 0, 0, 1.0f);
        const std::string shown = off ? "AUTOMATIC" : pm_port_text_ + (sel ? caret : "");
        const float vw = kit::SpacedWidth(f.typed, st(7.5f), shown, st(1.5f));
        kit::Spaced(d, f.typed, st(7.5f), ImVec2((f0.x + f1.x - vw) * 0.5f, mid - st(5)),
                    off ? IM_COL32(120, 116, 100, 150) : dim, shown, st(1.5f));
      }
      y += h;
      // An experimental tick rate (every one above 30: TekRantGaming, 13 Nov 2026): the warning right under the
      // row, in red.
      if (i == kPmTick && pm_tick_ != 30) {
        const std::string warning = "Physics, weapons, vehicles and timers may behave oddly or break, your PC and every "
                                    "player's connection work harder, and players on older versions cannot join. Use "
                                    "30 Hz for a normal match.";
        const float wx = ra.x + st(52), wrap = rb.x - st(18) - wx, size = st(10);
        const ImU32 red = IM_COL32(236, 92, 76, 255);  // a red that reads on the dark rail
        const float wh = st(6) + st(14) + kit::WrappedHeight(f.ui, size, warning, wrap) + st(10);
        d->AddRectFilled(ImVec2(ra.x, y), ImVec2(ra.x + st(5), y + wh), kit::kStamp);
        kit::Spaced(d, f.ui_bold, st(9), ImVec2(wx, y + st(6)), red, fmt::format("WARNING: {} HZ IS EXPERIMENTAL", pm_tick_),
                    st(1.5f));
        kit::Wrapped(d, f.ui, size, ImVec2(wx, y + st(6) + st(14)), red, warning, wrap);
        d->AddRectFilled(ImVec2(ra.x, y + wh - 1), ImVec2(rb.x, y + wh), IM_COL32(241, 234, 211, 20));
        y += wh;
      }
    }
    // ---- the band: the chosen row's help, the green button
    {
      static constexpr const char* kAbout[] = {
          "The map your match plays. All four in turn: the next map after each round, as on the official servers.",
          "The match's name in the server browser. Type it with the keyboard; left empty it is your name's match.",
          "Online: your match is on everybody's server browser, on the Community tab. LAN: for the PCs of your own "
          "network, with no internet needed; your PC answers the sign-in and the match list itself, and the match is "
          "on their browser's LAN tab (or they type your PC's network address in Direct connect).",
          "Yes: everybody sees your match on the server browser's Community tab and can join. No: only friends you "
          "give the address to (Direct connect). A LAN match is always found by the PCs of your network.",
          "On, your router is asked to open the match's port (UPnP) and the port is chosen for you. Off, you open "
          "the port below on your router yourself.",
          "The UDP port players reach your match on, when UPnP is off or the match is a LAN one: 18300 unless "
          "you want another. For an Online match, open it on your router for friends outside your home.",
          "Your PC runs the match's server while you play, and you join it. Friends join with Direct connect in the "
          "server browser: the pause copies the address for you. The match ends when you leave it.",
          "How many times a second the match is simulated. 30, the original game's, is the default. 60, 75 and 100 "
          "are experimental: physics and timing may break, and they ask more of your PC and of every player's "
          "connection. A player's game takes the match's rate when it joins."};
      kit::Spaced(d, f.typed, st(9), ImVec2(band.x + st(20), band.y + st(14)), kit::kInkSection, kTitles[pm_row_], st(2));
      kit::GameText(d, kit::GameFont::kHeadline, st(26), ImVec2(band.x + st(20), band.y + st(29)), kit::kInk,
                    kPmMapNames[pm_map_]);
      const ImVec2 q0(hb.x - st(20) - st(190), band.y + st(18)), q1(q0.x + st(190), q0.y + st(40));
      const bool error = !pm_error_.empty();
      kit::Wrapped(d, f.ui, st(11), ImVec2(band.x + st(20), band.y + st(64)), error ? kit::kRed : kit::kInkFaint,
                   error ? pm_error_ : std::string(kAbout[pm_row_]), q0.x - band.x - st(40));
      if (GreenButton(d, st, q0, q1, kit::Button::kA, "HOST THE MATCH")) clicked = kPmHost;
    }
    if (in.accept) clicked = pm_row_;
    if (clicked >= 0) {
      pm_row_ = clicked;
      if (clicked == kPmHost) HostPrivateMatch();
      else change(clicked, 1);
    }
    if (in.back) BackToMenu(kRowPrivateMatch, MainRowOf("CUSTOM MATCH")), pm_error_.clear();  // back to the menu
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
                  kPmMapNames[pm_map_]);
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
      SetJoinedServerNight(false);
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
    SetJoinedServerNight(false);  // nor a Night Ops server's night
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

  // ---- Report an issue: for now where to report (Discord); later the GitHub issues page.

  void ReportPage(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("report");
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    Chrome(d, io, st, 0, SceneBehind() ? 0.82f : 0);
    Prompts(d, st, st.P(240, 720 - 30 - 42),
            {{kit::Button::kA, "Open our Discord"}, {kit::Button::kX, "Show my log file"}, {kit::Button::kB, "Back"}}, in);
    const ImVec2 a = st.P(240, 110), b = st.P(1040, 570);
    {
      kit::Turn turn(d, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), -0.4f);
      kit::Paper(d, a, b, st.u);
      kit::Tape(d, ImVec2((a.x + b.x) * 0.5f, a.y + st(4)), st(110), st(26), 2);
      const float x = a.x + st(44), w = st(712);
      float y = a.y + st(40);
      kit::Spaced(d, f.typed, st(15), ImVec2(x, y), kit::kInkFaint, "EARLY PLAYTEST \xC2\xB7 FEEDBACK", st(2));
      y += st(24);
      kit::Heading(d, st(56), ImVec2(x, y), kit::kInk, "REPORT AN ISSUE", st(2), w);
      y += st(70);
      y += kit::Wrapped(d, f.ui, st(20), ImVec2(x, y), kit::kInkSoft,
                        "Found a bug, a crash or something that feels wrong? Tell us in the playtest's bug reports "
                        "channel on our Discord. Say what you did, what you saw and what you expected.",
                        w) +
           st(18);
      y += kit::Wrapped(d, f.ui, st(20), ImVec2(x, y), kit::kInkSoft,
                        "Your log helps us most: attach the file bf1943.log from your user folder (Show my log file "
                        "opens it). Crash reports are in the crashes folder beside it.",
                        w) +
           st(22);
#if defined(_WIN32)
      // An overlay hooked into the game, and what to do if the game crashes at its first frame: a player's v0.3.0 did,
      // 8 starts of 8, inside RivaTuner Statistics Server's hook, and nothing said so (12 Nov 2026). The table and the
      // check are crash_report.cpp's (overlay_hooks.inc, included here so this page's edit stays within it; the log
      // has the same names). Looked up once a second: a hook can come after the start.
      {
#include "overlay_hooks.inc"
        static double checked = -1;
        static std::string notice;
        if (const double now = ImGui::GetTime(); checked < 0 || now - checked > 1) {
          checked = now;
          std::vector<std::string> names;
          bool rtss = false;
          for (const HookedOverlay& h : HookedOverlays()) {
            // Only the two seen in a crash's stack: Steam's overlay hooks into every game Steam starts (ours too,
            // Add to Steam), and a notice about it would alarm most players for nothing.
            if (h.product != "RivaTuner Statistics Server" && h.product != "OBS Studio") continue;
            names.push_back(h.product);
            rtss = rtss || h.product == "RivaTuner Statistics Server";
          }
          std::string who;
          for (size_t i = 0; i < names.size(); ++i)
            who += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
          const bool one = names.size() == 1;
          notice.clear();
          if (!names.empty()) {
            notice = who + (one ? " is" : " are") +
                     " hooked into the game. If the game crashes at its first frame, close " + (one ? "it" : "them");
            if (rtss)
              notice += std::string(" (and MSI Afterburner) or exclude bf1943.exe in ") + (one ? "its" : "their") +
                        " profiles.";
            else
              notice += std::string(" or turn ") + (one ? "its overlay" : "their overlays") + " off for bf1943.exe.";
          }
        }
        if (!notice.empty()) y += kit::Wrapped(d, f.ui, st(20), ImVec2(x, y), kit::kInkSoft, notice, w) + st(22);
      }
#endif
      d->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + st(56)), kit::kPaperDim);
      kit::Wrapped(d, f.typed, st(15), ImVec2(x + st(16), y + st(10)), kit::kInkFaint,
                   "Soon this button opens the issue page on GitHub instead.", w - st(32));
    }
    StampCorner(d, st);
    if (in.accept) OpenUrl(kDiscordUrl);
    if (in.x) {
      const fs::path log = settings_.path.parent_path() / "bf1943.log";
      REXLOG_INFO("BF launcher: showing {}", log.string());
#if defined(_WIN32)
      if (const char* v = std::getenv("BF_DEV_NO_OPEN"); !(v && *v == '1'))
        ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + log.wstring() + L"\"").c_str(), nullptr,
                      SW_SHOWNORMAL);
#endif
    }
    if (in.back) page_ = Page::kHome;
  }

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
    if (state == update::State::kChecked) page_ = Page::kUpdate;
    else if (state == update::State::kNone || state == update::State::kFailed) CheckForUpdate("the Update row");
  }
  void CheckForUpdate(const char* why) {
    update_checked_at_ = ImGui::GetTime();
    REXLOG_INFO("BF update: checking ({})", why);
    update::CheckInBackground();
  }
  // Every frame on the main menu and its pages: the 6 hourly check, and a checked release installs at once while the
  // main menu is showing (before the game runs and after it: the program starts again either way).
  void UpdateTick() {
    const double now = ImGui::GetTime();
    if (now - update_checked_at_ > 6 * 3600) CheckForUpdate("six hours since the last one");
    if (update::State(update::Get().state.load()) == update::State::kChecked && (page_ == Page::kHome || page_ == Page::kMain) &&
        !quit_popup_ && !update_popup_ && !steam_ask_ && !steam_job_)
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
    if (!whats_new_pending_ || whats_new_open_ || update_popup_ || quit_popup_ || steam_ask_ || steam_job_) return;
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
