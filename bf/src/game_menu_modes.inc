// The server type page (hotfix 0.4.8.1; TekRantGaming, 27 Nov 2026, his pick of three mockups: version 1 "Dossier
// cards", runs/ui-designs/mode-select), included inside GameMenu (game_menu.cpp). Server Browser and Quick Play open it
// first: four tall paper cards, ALL, CLASSIC, NIGHT OPS and DAYBREAK, each with a picture from the 0.4.8 trailer's
// own footage (bf/data/modes, his recordings), a short line and the players on servers of that kind. ALL is the
// browser as before; the other three the browser with only that kind of server (browser_event_), or Quick Play into
// the busiest server of that kind with room. Back from the browser comes back here. A controller, the keyboard and
// the mouse all work: left and right (or LB and RB) choose, A or Enter or a click opens, B or Esc goes back.

  // The kinds, in the cards' order; `event` is the browser's type filter (browser_event_) for each.
  struct ModeCard {
    const char* title;  // in the game's headline lettering
    const char* tag;    // the typed line under the picture
    const char* line1;  // the short description, two lines (TekRantGaming's mockup wording)
    const char* line2;
    const char* picture;  // bf/data/modes/<picture>.jpg
    int event;
  };
  static constexpr ModeCard kModeCards[] = {
      {"ALL", "EVERY SERVER", "Every server of every kind,", "the full list as it is today.", "all", kEventAll},
      {"CLASSIC", "THE ORIGINAL", "The game as it always was:", "daylight battles on the islands.", "classic", kEventClassic},
      {"NIGHT OPS", "AFTER DARK", "Fight under the moon while a", "storm builds as tickets fall.", "night_ops", kEventNight},
      {"DAYBREAK", "FIRST LIGHT", "Battle at sunrise, with weather", "that turns as the round goes on.", "daybreak", kEventDaybreak},
  };
  int modes_row_ = 0;           // the chosen card (kept: the page opens on the kind chosen last)
  bool modes_quick_ = false;    // opened by Quick Play (a card joins) or by Server Browser (a card opens the list)
  bool browser_from_modes_ = false;  // the browser was opened from this page: Back comes back here
  std::string modes_error_;     // Quick Play's word (no server of that kind with room...): under the cards for a while
  double modes_error_at_ = -100;  // when it was shown (ImGui's clock)
  double modes_error_seen_ = 0;   // the menu_error_at_ last taken from Quick Play (it sets menu_error_ for Home)

  // Opens the page: from Home's or the main menu's Quick Play and Server Browser. Builds without Night Ops have only
  // one kind of server: straight on, as before.
  void OpenModes(bool quick) {
    if (!kNightOps) return quick ? QuickPlay() : OpenBrowser();
    page_ = Page::kModes, modes_quick_ = quick, modes_error_.clear(), modes_error_seen_ = menu_error_at_;
    modes_row_ = std::clamp(modes_row_, 0, int(std::size(kModeCards)) - 1);
    if (!list_.asked() || list_.servers().empty()) list_.RefreshNow();  // the counts on the cards
    REXLOG_INFO("BF menu: server type page ({})", quick ? "Quick Play" : "Server Browser");
  }

  // Back to the menu row that opened the page.
  void BackFromModes() {
    BackToMenu(modes_quick_ ? kRowQuick : kRowBrowser, MainRowOf(modes_quick_ ? "QUICK MATCH" : "SERVER BROWSER"));
  }

  void ChooseMode(int i) {
    const ModeCard& m = kModeCards[i];
    modes_row_ = i;
    REXLOG_INFO("BF menu: server type {} ({})", m.title, modes_quick_ ? "Quick Play" : "Server Browser");
    if (modes_quick_) {
      quick_event_ = m.event;
      return QuickPlay();
    }
    browser_event_ = m.event;
    OpenBrowser();
    browser_from_modes_ = true;
    // On the tab that has that kind: Reborn's or Community's, whichever lists it when the open one has none.
    if (browser_tab_ == kTabOfficial || browser_tab_ == kTabCommunity) {
      int official = 0, community = 0;
      for (const ServerInfo& sv : list_.servers())
        if (EventPasses(m.event, sv)) ++(sv.official ? official : community);
      if (browser_tab_ == kTabOfficial && !official && community) browser_tab_ = kTabCommunity;
      if (browser_tab_ == kTabCommunity && !community && official) browser_tab_ = kTabOfficial;
    }
  }

  void ModesPage(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage(modes_quick_ ? "server type (quick play)" : "server type (server browser)");
    list_.Want();
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    MenuBackdrop(d, io.DisplaySize, st);
    bool online_ok = false;
    const std::string status = OnlineStatus(online_ok);
    const bool edit = MenuChrome(d, io, st, status, online_ok);
    const int n = int(std::size(kModeCards));
    // ---- the players and servers of each kind, from the live list
    int players[4] = {0, 0, 0, 0}, servers[4] = {0, 0, 0, 0};
    for (const ServerInfo& sv : list_.servers())
      for (int i = 0; i < n; ++i)
        if (EventPasses(kModeCards[i].event, sv)) players[i] += std::max(0, sv.players), ++servers[i];
    // ---- the title block
    {
      const std::string eyebrow =
          !online_ok ? "PACIFIC THEATRE \xC2\xB7 THE SERVER LIST IS NOT REACHABLE RIGHT NOW"
          : modes_quick_ ? "PACIFIC THEATRE \xC2\xB7 CHOOSE A KIND OF SERVER \xC2\xB7 QUICK PLAY JOINS THE BUSIEST WITH ROOM"
                         : "PACIFIC THEATRE \xC2\xB7 CHOOSE A KIND OF SERVER";
      kit::Spaced(d, f.typed, st(10.5f), st.P(48, 91), kit::kKhakiDim, eyebrow, st(2));
      kit::GameText(d, kit::GameFont::kHeadline, st(40), st.P(48, 105), kit::kCream, modes_quick_ ? "QUICK PLAY" : "SERVER BROWSER");
    }
    // ---- the prompts (drawn first: a click on one is that button's press for the rest of the frame)
    Prompts(d, st, st.P(48, 662), {{kit::Button::kA, modes_quick_ ? "PLAY" : "OPEN"}, {kit::Button::kDpad, "CHOOSE", "Left"},
                                   {kit::Button::kB, "BACK"}},
            in, 15);
    MenuFoot(d, st);
    // ---- the cards (the mockup's 1920 x 1080 pixels over 1.5): 282 wide, 18 apart, the chosen one lifted 8
    if (in.left || in.prev_tab) modes_row_ = (modes_row_ + n - 1) % n;
    if (in.right || in.next_tab) modes_row_ = (modes_row_ + 1) % n;
    modes_row_ = std::clamp(modes_row_, 0, n - 1);
    int clicked = -1;
    const float gap = st(18.67f), cw = (st(1184) - gap * float(n - 1)) / float(n);
    for (int i = 0; i < n; ++i) {
      const ModeCard& m = kModeCards[i];
      const bool sel = modes_row_ == i;
      const float x0 = st.P(48, 0).x + float(i) * (cw + gap);
      const ImVec2 a(x0, st.P(0, sel ? 197 : 208).y), b(x0 + cw, st.P(0, sel ? 629 : 621).y);
      const bool hover = ImGui::IsMouseHoveringRect(a, b);
      if (hover && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) modes_row_ = i;
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = i;
      // The card: a shadow, the paper, the chosen one's green frame.
      d->AddRectFilled(ImVec2(a.x + st(2), a.y + st(6)), ImVec2(b.x + st(2), b.y + st(9)), IM_COL32(0, 0, 0, sel ? 110 : 80), st(2));
      if (sel) d->AddRectFilled(ImVec2(a.x - st(4), a.y - st(4)), ImVec2(b.x + st(4), b.y + st(4)), kit::kGreen);
      kit::PaperArt(d, a, b);
      d->AddRect(a, b, kit::kBorder, 0, 0, 1.0f);
      // The picture, its foot darkened under the title.
      const ImVec2 p0(a.x + st(8), a.y + st(8)), p1(b.x - st(8), p0.y + st(sel ? 277 : 269));
      if (!kit::DrawImageFile(d, ExecutableDir() / "data" / "modes" / (std::string(m.picture) + ".jpg"), p0, p1, false, false, 800,
                              sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(228, 228, 228, 255)))
        d->AddRectFilled(p0, p1, IM_COL32(40, 46, 42, 255));  // (a missing file: a dark plate, the title still on it)
      const float fade = (p1.y - p0.y) * 0.45f;
      d->AddRectFilledMultiColor(ImVec2(p0.x, p1.y - fade), p1, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 150),
                                 IM_COL32(0, 0, 0, 150));
      d->AddRect(p0, p1, IM_COL32(0, 0, 0, 90), 0, 0, 1.0f);
      const float tcap = st(30);  // (GameText sizes by the capitals: the mockup's 64 px line is 30 here)
      kit::GameText(d, kit::GameFont::kHeadline, tcap, ImVec2(p0.x + st(13) + st(1.5f), p1.y - st(7) - tcap + st(2)), IM_COL32(0, 0, 0, 150),
                    m.title);
      kit::GameText(d, kit::GameFont::kHeadline, tcap, ImVec2(p0.x + st(13), p1.y - st(7) - tcap), kit::kCream, m.title);
      // The typed tag and the two lines.
      kit::Spaced(d, f.typed, st(8.5f), ImVec2(a.x + st(16), p1.y + st(13)), kit::kInkSection, m.tag, st(2.3f));
      const float lcap = st(12.5f);
      kit::GameText(d, kit::GameFont::kText, lcap, ImVec2(a.x + st(15), p1.y + st(30)), kit::kInk, m.line1);
      kit::GameText(d, kit::GameFont::kText, lcap, ImVec2(a.x + st(15), p1.y + st(30) + st(17)), kit::kInk, m.line2);
      // The foot: the players on that kind of server; on the chosen card the green bar with A OPEN (or PLAY).
      const ImVec2 f0(a.x, b.y - st(47)), f1 = b;
      if (sel) {
        d->AddRectFilled(f0, f1, kit::kGreen);
        d->AddRectFilled(ImVec2(f0.x, f1.y - st(3.3f)), f1, kit::kGreenShadow);
      } else {
        d->AddLine(ImVec2(f0.x, f0.y), ImVec2(f1.x, f0.y), kit::kRule, 1.0f);
      }
      const float fy = (f0.y + f1.y - (sel ? st(3.3f) : 0)) * 0.5f;
      const ImU32 ink = sel ? kit::kCream : kit::kInk, faint = sel ? kit::kCream : kit::kInkFaint;
      float fx = f0.x + st(15);
      d->AddCircleFilled(ImVec2(fx + st(3.3f), fy), st(5.3f), sel ? IM_COL32(241, 234, 211, 64) : IM_COL32(93, 138, 50, 64), 16);
      d->AddCircleFilled(ImVec2(fx + st(3.3f), fy), st(3.3f), sel ? kit::kCream : kit::kGreen, 16);
      fx += st(16);
      const std::string count = online_ok || list_.asked() ? std::to_string(players[i]) : "-";
      const float csz = st(26);
      d->AddText(f.ui_bold, csz, ImVec2(fx, fy - csz * 0.72f), ink, count.c_str());
      fx += kit::TextWidth(f.ui_bold, csz, count) + st(8);
      kit::Spaced(d, f.typed, st(8.33f), ImVec2(fx, fy - st(9.5f)), faint, players[i] == 1 ? "PLAYER" : "PLAYERS", st(2));
      kit::Spaced(d, f.typed, st(8.33f), ImVec2(fx, fy + st(1)), faint,
                  fmt::format("{} {}", servers[i], servers[i] == 1 ? "SERVER" : "SERVERS"), st(2));
      if (sel) {
        const char* go = modes_quick_ ? "PLAY" : "OPEN";
        const float gw = kit::SpacedWidth(f.typed, st(9.33f), go, st(1.7f));
        const float gx = f1.x - st(15) - gw;
        kit::Spaced(d, f.typed, st(9.33f), ImVec2(gx, fy - st(6)), kit::kCream, go, st(1.7f));
        const float glw = kit::Glyph(d, PromptStyle(), kit::Button::kA, KeyFor(kit::Button::kA), ImVec2(0, fy), st(18), false);
        kit::Glyph(d, PromptStyle(), kit::Button::kA, KeyFor(kit::Button::kA), ImVec2(gx - st(7) - glw, fy), st(18));
      }
    }
    // Quick Play's word when no server of that kind has room (or the list does not answer): what it told Home.
    if (menu_error_at_ != modes_error_seen_) {
      modes_error_seen_ = menu_error_at_;
      if (!menu_error_.empty()) modes_error_ = menu_error_, modes_error_at_ = ImGui::GetTime();
    }
    if (!modes_error_.empty() && ImGui::GetTime() - modes_error_at_ < 8.0) {
      const float tw = kit::TextWidth(f.ui_bold, st(13.5f), modes_error_);
      const ImVec2 e0(st.P(640, 0).x - tw * 0.5f - st(14), st.P(0, 636).y), e1(st.P(640, 0).x + tw * 0.5f + st(14), st.P(0, 656).y);
      d->AddRectFilled(e0, e1, kit::kStrip);
      d->AddRectFilled(e0, ImVec2(e0.x + st(4), e1.y), kit::kStamp);
      d->AddText(f.ui_bold, st(13.5f), ImVec2(e0.x + st(14), e0.y + (st(20) - st(13.5f) * 1.45f) * 0.5f), kit::kStripText, modes_error_.c_str());
    }
    if (edit || in.view) return BeginAccount(false);
    if (in.back) return BackFromModes();
    if (in.accept) clicked = modes_row_;
    if (clicked >= 0) ChooseMode(clicked);
  }
