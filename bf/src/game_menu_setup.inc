// The first start in the new look, Version A1 (TekRantGaming, 12 Nov 2026, 18:30: "fundamentally change the
// onboarding and setup process ... a page that lets them know that voice chat is used in game ... open mic ... the
// default ... push to talk with the keybind customizable ... both teams can hear each other ... directional audio ...
// an advisory, saying that this is an early playtest"). Nine steps: 1 game files, 2 soldier name (their pages as
// they were, game_menu_launcher.inc), then in this file's frame (the steps in a rail at the left, the page on paper at
// the right): 3 Display, 4 Graphics, 5 Controls (the settings rows, game_menu_settings.inc), 6 button prompts, 7 voice
// chat, 8 shaders (ShadersPage, game_menu_launcher.inc: the game starts behind it with the settings just chosen and
// its pipelines are made, so no join ever shows a shader page; TekRantGaming, 13 Nov 2026), 9 ready. Until v0.4.0
// the shaders were step 2 in name only (made at the first join) and a Modern and Classic page stood before Ready:
// Modern is gone ("we are dropping support for modern and going full in on classic being the default"). Included
// inside GameMenu (game_menu.cpp) after game_menu_settings.inc.

  static constexpr int kSetupSteps = 9;
  struct SetupStepRow {
    const char* name;
    const char* sub;
  };
  static constexpr SetupStepRow kSetupRows[kSetupSteps] = {
      {"GAME FILES", "YOUR COPY OF THE GAME"},    {"SOLDIER NAME", "WHAT PLAYERS SEE"},
      {"DISPLAY", "WINDOW, RESOLUTION, FRAME RATE"}, {"GRAPHICS", "PRESET, UPSCALING, LIGHTING"},
      {"CONTROLS", "MOUSE, KEYBOARD, CONTROLLER"}, {"BUTTON PROMPTS", "XBOX, PLAYSTATION, KEYBOARD"},
      {"VOICE CHAT", "OPEN MIC OR PUSH TO TALK"}, {"SHADERS", "PREPARED ONCE, NOW"},
      {"READY", "THE PLAYTEST, THEN PLAY"}};
  static constexpr const char* kSetupNext[kSetupSteps] = {"SOLDIER NAME", "DISPLAY", "GRAPHICS", "CONTROLS", "BUTTON PROMPTS",
                                                          "VOICE CHAT", "SHADERS", "READY", ""};
  int voice_choice_ = -1;  // 8: 0 open mic (the recommended one), 1 push to talk; -1 not looked at yet

  struct SetupPanel {
    ImVec2 a, b;
  };
  // The frame: the backdrop and the logo, the step bar, the steps in a rail at the left (the current one on paper,
  // the done ones with a green number), the paper at the right for the page (returned), the foot.
  SetupPanel SetupFrame(ImDrawList* d, ImGuiIO& io, const Stage& st, int step) {
    const kit::Fonts& f = kit::GetFonts();
    MenuBackdrop(d, io.DisplaySize, st);
    kit::Brand(d, st.P(40, 28), st.u);
    kit::StepBar(d, st.P(1240, 34), st.u, step, kSetupSteps);
    const ImVec2 ra = st.P(48, 93), rb = st.P(400, 650);
    d->AddRectFilled(ra, rb, IM_COL32(13, 15, 11, 204));
    d->AddRect(ra, rb, IM_COL32(241, 234, 211, 30), 0, 0, 1.0f);
    kit::Spaced(d, f.typed, st(10), ImVec2(ra.x + st(20), ra.y + st(16)), kit::kKhakiDim, "FIRST START \xC2\xB7 ABOUT TWO MINUTES", st(2));
    kit::GameText(d, kit::GameFont::kHeadline, st(30), ImVec2(ra.x + st(20), ra.y + st(32)), kit::kCream, "SETUP");
    float y = ra.y + st(78);
    const float h = st(40);
    for (int i = 0; i < kSetupSteps; ++i) {
      const int n = i + 1;
      const bool cur = n == step, done = n < step;
      const ImVec2 a(ra.x, y), b(rb.x, y + h);
      if (cur) {
        kit::PaperArt(d, a, b);
        d->AddRectFilled(a, ImVec2(a.x + st(5), b.y), kit::kStamp);
      }
      d->AddRectFilled(ImVec2(a.x, b.y - 1), ImVec2(b.x, b.y), IM_COL32(241, 234, 211, 20));
      const ImVec2 n0(a.x + st(20), a.y + st(10)), n1(n0.x + st(20), n0.y + st(20));
      if (done) d->AddRectFilled(n0, n1, kit::kGreen);
      else d->AddRect(n0, n1, cur ? kit::kInk : kit::kKhakiDim, 0, 0, 1.0f);
      const std::string num = std::to_string(n);
      const float nw = kit::TextWidth(f.ui_bold, st(11.5f), num);
      d->AddText(f.ui_bold, st(11.5f), ImVec2((n0.x + n1.x - nw) * 0.5f, n0.y + st(2.5f)), done ? kit::kCream : cur ? kit::kInk : kit::kKhakiDim,
                 num.c_str());
      kit::GameText(d, kit::GameFont::kHeadlineMedium, st(12), ImVec2(a.x + st(52), a.y + st(8)),
                    cur ? kit::kInk : done ? kit::kKhaki : kit::kKhakiDim, kSetupRows[i].name);
      kit::Spaced(d, f.typed, st(7.5f), ImVec2(a.x + st(52), a.y + st(26)), cur ? kit::kInkSection : kit::kKhakiDim, kSetupRows[i].sub,
                  st(1.2f));
      y += h;
    }
    kit::Spaced(d, f.typed, st(8.75f), ImVec2(ra.x + st(20), rb.y - st(36)), kit::kKhakiDim, "NOTHING IS SENT ANYWHERE.", st(1.5f));
    kit::Spaced(d, f.typed, st(8.75f), ImVec2(ra.x + st(20), rb.y - st(22)), kit::kKhakiDim, "NO GAME FILES ARE INCLUDED.", st(1.5f));
    const SetupPanel p{st.P(427, 93), st.P(1232, 650)};
    kit::PaperArt(d, p.a, p.b);
    MenuFoot(d, st);
    return p;
  }
  // The page's title block on the paper: the step line, the title in the game's lettering, a line of text. Returns
  // the y under it.
  float SetupTitle(ImDrawList* d, const Stage& st, const SetupPanel& p, int step, const std::string& title, const std::string& text) {
    const kit::Fonts& f = kit::GetFonts();
    const std::string next = kSetupNext[std::clamp(step, 1, kSetupSteps) - 1];
    kit::Spaced(d, f.typed, st(10), ImVec2(p.a.x + st(24), p.a.y + st(18)), kit::kInkSection,
                fmt::format("STEP {} OF {}{}", step, kSetupSteps, next.empty() ? "" : " \xC2\xB7 NEXT: " + next), st(2));
    kit::GameText(d, kit::GameFont::kHeadline, st(26), ImVec2(p.a.x + st(24), p.a.y + st(34)), kit::kInk, title);
    float y = p.a.y + st(34) + st(26) + st(12);
    if (!text.empty()) y += kit::Wrapped(d, f.ui, st(14.25f), ImVec2(p.a.x + st(24), y), kit::kInk, text, p.b.x - p.a.x - st(48)) + st(10);
    return y;
  }
  // A choice card: paper on the paper, a green border and a filled ring when chosen (none of that when it is not
  // `pickable`: a card that only tells), a chip, a title in the game's lettering, a `strong` line, a line of text and
  // points. Returns true when clicked.
  bool ChoiceCard(ImDrawList* d, const Stage& st, ImVec2 a, ImVec2 b, bool chosen, const char* chip, ImU32 chip_fill,
                  const std::string& title, ImU32 title_col, const std::string& text, const std::vector<const char*>& points,
                  const std::string& strong = "", bool pickable = true) {
    const kit::Fonts& f = kit::GetFonts();
    const bool hover = pickable && ImGui::IsMouseHoveringRect(a, b);
    d->AddRectFilled(a, b, chosen ? kit::kPaperLight : hover ? kit::kPaper : kit::kPaperCard);
    d->AddRect(a, b, chosen ? kit::kGreen : kit::kBorder, 0, 0, chosen ? st(2.5f) : 1.0f);
    if (pickable) {
      const ImVec2 m(b.x - st(24), a.y + st(24));
      d->AddCircle(m, st(8), chosen ? kit::kGreen : kit::kBorder, 20, st(2));
      if (chosen) d->AddCircleFilled(m, st(4.5f), kit::kGreen, 16);
    }
    float y = a.y + st(18);
    if (chip && *chip) {
      Chip(d, st, ImVec2(a.x + st(20), y), st(18), chip, chip_fill, kit::kStripText, false);
      y += st(28);
    }
    kit::GameText(d, kit::GameFont::kHeadline, st(20), ImVec2(a.x + st(20), y), title_col, title);
    y += st(20) + st(12);
    const float wrap = b.x - a.x - st(40);
    if (!strong.empty()) {
      d->AddText(f.ui_bold, st(13), ImVec2(a.x + st(20), y), kit::kInk, strong.c_str());
      y += st(22);
    }
    y += kit::Wrapped(d, f.ui, st(13.25f), ImVec2(a.x + st(20), y), kit::kInk, text, wrap) + st(8);
    for (const char* pt : points) {
      d->AddRectFilled(ImVec2(a.x + st(20), y + st(5)), ImVec2(a.x + st(25), y + st(10)), kit::kInk);
      y += kit::Wrapped(d, f.ui, st(12.75f), ImVec2(a.x + st(32), y), kit::kInkSoft, pt, wrap - st(12)) + st(4);
    }
    return hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  }

  // ---- 3 to 5: Display, Graphics and Controls (the settings rows in the frame)
  void SetupSettingsA1Page(ImDrawList* d, ImGuiIO& io, Input in) {
    static const char* kNames[] = {"DISPLAY", "GRAPHICS", "CONTROLS"};
    static const char* kAbout[] = {
        "How the game fills your screen and how fast it draws. Auto detect picks these for your PC; change any row with "
        "left and right.",
        "The picture's quality. A preset sets every row at once: move over them with left and right, and A uses the "
        "one you are on. Medium suits a GTX 1060 or an RX 580 at 1080p, High an RTX 2060 and up. Auto detect chooses "
        "one for your graphics card.",
        "The mouse, the keyboard and the controller. Keyboard and mouse play works beside a controller; the keys are "
        "under Keyboard bindings."};
    NotePage(setup_tab_ == 0 ? "setup display" : setup_tab_ == 1 ? "setup graphics" : "setup controls");
    const Stage st = StageOf(io);
    tab_ = setup_tab_ == 0 ? kTabDisplay : setup_tab_ == 1 ? kTabGraphics : kTabControls;
    CaptureKeyInto(in);
    const int step = 3 + setup_tab_;
    const SetupPanel p = SetupFrame(d, io, st, step);
    const bool custom = tab_ == kTabGraphics && sub_ == kSubNone && CurrentGraphicsPreset() < 0 &&
                        !settings_.Get("graphics_preset", "").empty();
    std::vector<Cue> cues = {{kit::Button::kA, tab_ == kTabGraphics && on_preset_strip_ ? "USE THIS PRESET" : "CHANGE"},
                             {kit::Button::kRB, "NEXT PAGE"},
                             {kit::Button::kStart, "AUTO DETECT", "P"}};
    if (custom) cues.push_back({kit::Button::kX, "BACK TO " + Upper(settings_.Get("graphics_preset", ""))});
    cues.push_back({kit::Button::kY, "SKIP THE SETTINGS"});
    cues.push_back({kit::Button::kB, "BACK"});
    Prompts(d, st, st.P(48, 662), cues, in, 15);
    const float y = SetupTitle(d, st, p, step, kNames[setup_tab_], kAbout[setup_tab_]);
    const ImVec2 pa(p.a.x + st(16), y), pb(p.b.x - st(16) - st(226), p.b.y - st(16));
    const ImVec2 ha(pb.x + st(12), y), hb(p.b.x - st(16), p.b.y - st(16));
    if (skip_popup_) {
      Input none;
      SettingsBody(d, io, st, none, pa, pb, ha, hb);
      SkipQuestion(d, io, st, in);
      return;
    }
    Input list_in = in;
    list_in.back = list_in.next_tab = list_in.prev_tab = list_in.start = list_in.y = list_in.x = false;
    SettingsBody(d, io, st, list_in, pa, pb, ha, hb);
    if (in.x) PresetBack();
    if (in.back && SubPageBack()) return;
    SetupKeys(in);
  }

  // ---- 6: the button prompts (King Kong's page, with PlayStation 3 for the PlayStation 2)
  void PromptsA1Page(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("setup prompts");
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    const SetupPanel p = SetupFrame(d, io, st, 6);
    Prompts(d, st, st.P(48, 662), {{kit::Button::kA, "CHOOSE"}, {kit::Button::kRB, "NEXT PAGE"}, {kit::Button::kB, "BACK"}}, in, 15);
    const float y0 = SetupTitle(d, st, p, 6, "BUTTON PROMPTS",
                                "The buttons the menus and the game show you. Automatic follows whatever you play with: the "
                                "controller's own buttons, or the keyboard's keys. Change it any time in Settings, Controls.");
    static const std::pair<const char*, const char*> kOptions[] = {
        {"auto", "Automatic"},    {"xbox360", "Xbox 360"}, {"xbox_series", "Xbox Series"},       {"ps3", "PlayStation 3"},
        {"ps5", "PlayStation 5"}, {"steam_deck", "Steam Deck"}, {"keyboard", "Keyboard and mouse"}};
    std::string cur = Cvar("bf_button_prompts");
    if (cur == "ps2") cur = "ps3";
    const int n = int(std::size(kOptions));
    if (in.up) prompts_row_ = (prompts_row_ + n - 1) % n;
    if (in.down) prompts_row_ = (prompts_row_ + 1) % n;
    prompts_row_ = std::clamp(prompts_row_, 0, n - 1);
    const float lx0 = p.a.x + st(16), lx1 = p.a.x + st(390), rh = st(40);
    int clicked = -1;
    for (int i = 0; i < n; ++i) {
      const ImVec2 r0(lx0, y0 + st(8) + float(i) * rh), r1(lx1, r0.y + rh);
      const bool hover = ImGui::IsMouseHoveringRect(r0, r1);
      if (hover && io.MouseDelta.x + io.MouseDelta.y != 0) prompts_row_ = i;
      const bool sel = prompts_row_ == i, chosen = cur == kOptions[i].first;
      if (sel) {
        d->AddRectFilled(r0, r1, kit::kGreen);
        d->AddRectFilled(r0, ImVec2(r0.x + st(5), r1.y), kit::kStamp);
      }
      d->AddRectFilled(ImVec2(r0.x, r1.y - 1), ImVec2(r1.x, r1.y), IM_COL32(34, 36, 27, 28));
      const ImU32 ink = sel ? kit::kCream : kit::kInk;
      const ImVec2 m(r0.x + st(26), r0.y + rh * 0.5f);  // the chosen one's ring
      d->AddCircle(m, st(7), ink, 20, st(1.8f));
      if (chosen) d->AddCircleFilled(m, st(3.5f), ink, 16);
      d->AddText(f.ui_bold, st(13), ImVec2(r0.x + st(46), r0.y + (rh - st(13) * 1.45f) * 0.5f), ink, kOptions[i].second);
      if (i == 0) {
        const std::string now = std::string("Now: ") + kit::StyleName(kit::StyleFor("auto", device_ == Device::kKeyboard));
        const float nw = kit::TextWidth(f.ui, st(11.5f), now);
        d->AddText(f.ui, st(11.5f), ImVec2(r1.x - st(14) - nw, r0.y + (rh - st(10) * 1.45f) * 0.5f), sel ? kit::kCream : kit::kInkFaint, now.c_str());
      }
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = i;
    }
    {  // the preview: the face buttons, the bumpers and the triggers in the highlighted style
      const ImVec2 a(p.a.x + st(410), y0 + st(8)), b(p.b.x - st(16), p.b.y - st(16));
      d->AddRectFilled(a, b, kit::kPaperCard);
      d->AddRect(a, b, kit::kBorder, 0, 0, 1.0f);
      kit::Spaced(d, f.typed, st(10), ImVec2(a.x + st(18), a.y + st(16)), kit::kInkSection, "PREVIEW", st(2));
      const kit::Style style = kit::StyleFor(kOptions[prompts_row_].first, device_ == Device::kKeyboard);
      const std::pair<kit::Button, const char*> kShown[] = {
          {kit::Button::kA, "Enter"}, {kit::Button::kB, "Esc"},  {kit::Button::kX, "F"},   {kit::Button::kY, "R"},
          {kit::Button::kLB, "Q"},    {kit::Button::kRB, "E"},   {kit::Button::kLT, "RMB"}, {kit::Button::kRT, "LMB"},
          {kit::Button::kStart, "P"}, {kit::Button::kBack, "Tab"}};
      float x = a.x + st(18), y = a.y + st(56);
      for (size_t i = 0; i < std::size(kShown); ++i) {
        if (i == 4 || i == 8) x = a.x + st(18), y += st(62);
        const float gw = kit::Glyph(d, style, kShown[i].first, kShown[i].second, ImVec2(x, y + st(18)), st(36));
        x += std::max(gw, st(36)) + st(22);
      }
      kit::Wrapped(d, f.ui, st(12), ImVec2(a.x + st(18), b.y - st(40)), kit::kInkFaint,
                   std::string(kit::StyleName(style)) + ": what the prompts look like.", b.x - a.x - st(36));
    }
    if (in.accept) clicked = prompts_row_;
    if (clicked >= 0) {
      prompts_row_ = clicked;
      SetCvar("bf_button_prompts", kOptions[clicked].first);
      REXLOG_INFO("BF setup: button prompts {}", kOptions[clicked].first);
    }
    in.accept = in.start = in.y = false;
    SetupKeys(in);
  }

  // ---- 7: voice chat (TekRantGaming: open mic recommended and the default, push to talk with its key, both teams
  // hear each other within a distance, directional)
  void VoicePage(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("setup voice");
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    if (voice_choice_ < 0) voice_choice_ = settings_.Get("cvar.bf_voice_mode", "") == "push" ? 1 : 0;  // open mic unless chosen before
    CaptureKeyInto(in);
    const SetupPanel p = SetupFrame(d, io, st, 7);
    std::vector<Cue> cues = {{kit::Button::kA, "GOT IT", "", true}};
    if (voice_choice_ == 1) cues.push_back({kit::Button::kX, "CHANGE THE KEY"});
    cues.push_back({kit::Button::kB, "BACK"});
    Prompts(d, st, st.P(48, 662), cues, in, 15);
    const int range = int(std::lround(std::atof(Cvar("bf_voice_range").c_str())));
    const float y0 = SetupTitle(d, st, p, 7, "VOICE CHAT",
                                fmt::format("Voice chat is on in every match, and it is how a squad wins. Players near you hear you: "
                                            "both teams, within about {} metres, from where you stand, fading with distance. Pick "
                                            "how your mic works.",
                                            range > 0 ? range : 50));
    const std::string key = Cvar("keybind_push_to_talk");
    const float cw = (p.b.x - p.a.x - st(48) - st(16)) * 0.5f, ch = st(222);
    const ImVec2 a0(p.a.x + st(24), y0 + st(4)), a1(a0.x + cw, a0.y + ch);
    const ImVec2 b0(a1.x + st(16), a0.y), b1(b0.x + cw, a1.y);
    int clicked = -1;
    if (ChoiceCard(d, st, a0, a1, voice_choice_ == 0, "RECOMMENDED", kit::kGreen, "OPEN MIC", kit::kInk,
                   "Your mic is live whenever you speak louder than the room: no key to hold, with a controller or a keyboard.",
                   {"Your voice goes out as you talk; a short pause closes it.",
                    "Too sensitive? Settings, Audio has the sensitivity and the volume.", "Mute yourself any time there too."}))
      clicked = 0;
    if (ChoiceCard(d, st, b0, b1, voice_choice_ == 1, "", 0, "PUSH TO TALK", kit::kInk,
                   "Hold a key while you talk, and your mic is off otherwise.",
                   {"Needs a keyboard key: with a controller, Open mic is the way to talk.", "The key can be changed here and in Settings, Controls."}))
      clicked = 1;
    {  // the key, on the push to talk card
      const ImVec2 k(b0.x + st(20), b1.y - st(44));
      kit::Spaced(d, f.typed, st(9.5f), k, kit::kInkSection, capture_.empty() ? "HOLD" : "PRESS A KEY (ESC KEEPS THE OLD ONE)", st(1.5f));
      if (capture_.empty()) {
        const float gw = kit::Glyph(d, kit::Style::kKeyboard, kit::Button::kKey, key.empty() ? "B" : key, ImVec2(k.x + st(40), k.y + st(5)), st(22));
        kit::Spaced(d, f.typed, st(9.5f), ImVec2(k.x + st(40) + gw + st(12), k.y), kit::kInkSection,
                    device_ == Device::kKeyboard ? "F CHANGES IT" : "X CHANGES IT", st(1.5f));
      }
    }
    {  // what everyone should know, in a strip
      const ImVec2 s0(p.a.x + st(24), p.b.y - st(74)), s1(p.b.x - st(24), p.b.y - st(20));
      d->AddRectFilled(s0, s1, kit::kPaperDim);
      d->AddRectFilled(s0, ImVec2(s0.x + st(4), s1.y), kit::kStamp);
      kit::Spaced(d, f.typed, st(9.5f), ImVec2(s0.x + st(16), s0.y + st(10)), kit::kStamp, "BOTH TEAMS HEAR YOU", st(1.5f));
      kit::Wrapped(d, f.ui, st(12), ImVec2(s0.x + st(16), s0.y + st(24)), kit::kInk,
                   fmt::format("Everyone within about {} metres hears you, friend or enemy, from your direction: careful near the "
                               "enemy. Text chat is there too (T to all, Y to your team, U to your squad), and all of it can be "
                               "changed or turned off in Settings, Audio.",
                               range > 0 ? range : 50),
                   s1.x - s0.x - st(32));
    }
    if (in.left) voice_choice_ = 0;
    if (in.right) voice_choice_ = 1;
    if (clicked >= 0) voice_choice_ = clicked;
    if (in.x && voice_choice_ == 1 && capture_.empty()) capture_ = "keybind_push_to_talk";
    if (in.accept || in.next_tab) {
      SetCvar("bf_voice_mode", voice_choice_ == 1 ? "push" : "open");
      REXLOG_INFO("BF setup: voice chat {} (push to talk key {})", voice_choice_ == 1 ? "push to talk" : "open mic", key);
      SetupNext();
      return;
    }
    if (in.back && capture_.empty()) SetupBack();
  }

  // ---- 9: ready (the playtest's advisory, the communities, then the main menu)
  void ReadyPage(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("setup ready");
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    const SetupPanel p = SetupFrame(d, io, st, 9);
    const bool restart = RestartPending();
    Prompts(d, st, st.P(48, 662),
            {{kit::Button::kA, restart ? "FINISH AND RESTART" : "INTO THE MAIN MENU", "", true}, {kit::Button::kX, "JOIN OUR DISCORD"},
             {kit::Button::kY, steam_added_ ? "ADDED TO STEAM" : "ADD TO STEAM"}, {kit::Button::kB, "BACK"}},
            in, 15);
    float y = SetupTitle(d, st, p, 9, "THANK YOU, " + Upper(settings_.Get("player_name", "SOLDIER")),
                         "You are in an early playtest of Battlefield 1943 Reborn. The menus you see are a placeholder; the game "
                         "behind them is what we are testing. What helps us most right now: how stable the servers and the game "
                         "are for you, and every bug you find. New content comes once that is solid.");
    const float wrap = p.b.x - p.a.x - st(48);
    {  // what to do when something breaks
      const ImVec2 s0(p.a.x + st(24), y), s1(p.b.x - st(24), y + st(50));
      d->AddRectFilled(s0, s1, kit::kPaperDim);
      d->AddRectFilled(s0, ImVec2(s0.x + st(4), s1.y), kit::kStamp);
      kit::Spaced(d, f.typed, st(9.5f), ImVec2(s0.x + st(16), s0.y + st(10)), kit::kStamp, "FOUND A BUG?", st(1.5f));
      kit::Wrapped(d, f.ui, st(12), ImVec2(s0.x + st(16), s0.y + st(24)), kit::kInk,
                   "Report an issue on the main menu shows your log file and opens our Discord: tell us what you did, what you "
                   "saw and what you expected.",
                   s1.x - s0.x - st(32));
      y = s1.y + st(16);
    }
    if (restart) {
      y += kit::Wrapped(d, f.ui, st(12), ImVec2(p.a.x + st(24), y), kit::kRed,
                        "Some of your graphics settings take effect when the game starts, so it starts again when you finish "
                        "(a few seconds).",
                        wrap) +
           st(12);
    }
    kit::Spaced(d, f.typed, st(10), ImVec2(p.a.x + st(24), y), kit::kInkSection, "STAY IN THE LOOP \xC2\xB7 THE OFFICIAL COMMUNITIES", st(2));
    y += st(18);
    const float cw = (wrap - st(16)) * 0.5f, ch = st(164);
    int card = -1;
    if (InfoCard(d, st, ImVec2(p.a.x + st(24), y), ImVec2(p.a.x + st(24) + cw, y + ch), 0, kit::Button::kX, "COMMUNITY",
                 "Join our Discord", "Updates, servers, playtest news and bug reports: discord.gg/8eakYSvBZS"))
      card = 0;
    if (InfoCard(d, st, ImVec2(p.a.x + st(24) + cw + st(16), y), ImVec2(p.b.x - st(24), y + ch), 1, kit::Button::kY, "SHORTCUT",
                 steam_added_ ? "Added to Steam" : "Add to Steam",
                 steam_added_ ? "In your Steam library, with the game's artwork."
                              : "A shortcut with the game's artwork and controller support in Big Picture."))
      card = 1;
    y += ch + st(14);
    kit::Wrapped(d, f.ui, st(12.75f), ImVec2(p.a.x + st(24), y), kit::kInkFaint,
                 "Thank you for playing. Everything you chose is in Settings, any time.", wrap);
    CheckSteamOnce();
    if (SteamDialog(d, io, st, in)) return;
    if (in.x || card == 0) OpenUrl(kDiscordUrl);
    if (in.y || card == 1) AddToSteam();
    if (in.accept) SetupNext();
    else if (in.back) SetupBack();
  }
