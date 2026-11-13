// The main menu in the new look, Version A1 "Field Dispatch, issued" (TekRantGaming, 12 Nov 2026: "version A1 is the
// best option. I would like you to go ahead and build this now ... the play button should probably just be changed to
// the quick play option. And anything that was in the play menu is going to need to be brought into the main menu").
// Included inside GameMenu (game_menu.cpp), after game_menu_launcher.inc. One page before the game runs (Home) and
// after it (its main menu): the game's own logo, the Artbrush rows (bf_font.cpp), the card paper, the busiest server
// with its map's picture as the Quick Play card, the player's portrait and insignia, the game's button glyphs: all
// from the player's game files (shader_prep.cpp ExtractMenuArt), never shipped. The mockups (runs\ui-designs,
// build2.py a1_home) are 1920 x 1080; sizes here are theirs at 720 lines (two thirds), on the Stage.

  struct MenuRow {
    const char* name;
    bool compact;  // the lesser rows (Windows headers define `small`)
  };
  // (TekRantGaming, 12 Nov 2026 18:10: "drop join friends and replace it with the unlockables button".)
  // Create Private Match is back (TekRantGaming, 13 Nov 2026, on pull request 11: "merge all of it"; it had left
  // the menu on 11 Nov): our own server on this PC, joined at once, friends by Direct connect (PrivateMatchPage).
  static constexpr MenuRow kMenuRows[] = {{"QUICK PLAY", false},  {"SERVER BROWSER", false}, {"CUSTOM MATCH", false},
                                          {"UNLOCKABLES", false},
                                          {"STATS & AWARDS", false}, {"LEADERBOARDS", false}, {"ACHIEVEMENTS", false},
                                          {"SETTINGS", false},    {"HELP", false},           {"UPDATE", true},
                                          {"REPORT AN ISSUE", true}, {"MORE FROM TRG STUDIOS", true}, {"QUIT", true}};
  enum MenuRowId {
    kRowQuick, kRowBrowser, kRowPrivateMatch, kRowUnlockables, kRowStats, kRowLeaderboards, kRowAchievements,
    kRowSettings, kRowHelp, kRowUpdate, kRowReport, kRowProjects, kRowQuit, kMenuRowCount
  };
  std::string menu_error_;      // Quick Play found no server: said on the card until the next press
  double menu_error_at_ = -100;

  // The backdrop of the new look: the game's own menu picture (the islands from the air, its clouds) with the olive
  // wash and the map grid of the mockups; the maps from the air (MainBackdrop) until the art is there.
  void MenuBackdrop(ImDrawList* d, ImVec2 sz, const Stage& st) {
    if (kit::Art(d, "backdrop", ImVec2(0, 0), sz)) {
      d->AddRectFilledMultiColor(ImVec2(0, 0), sz, IM_COL32(18, 22, 14, 232), IM_COL32(18, 22, 14, 90),
                                 IM_COL32(18, 22, 14, 90), IM_COL32(18, 22, 14, 232));
    } else {
      MainBackdrop(d, sz);
      d->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(12, 14, 11, 150));
    }
    const float step = st(53.3f);  // the mockups' 80 px grid
    const ImU32 line = IM_COL32(241, 234, 211, 10);
    for (float x = std::fmod(st.ox, step); x < sz.x; x += step) d->AddLine(ImVec2(x, 0), ImVec2(x, sz.y), line, 1.0f);
    for (float y = std::fmod(st.oy, step); y < sz.y; y += step) d->AddLine(ImVec2(0, y), ImVec2(sz.x, y), line, 1.0f);
  }

  // The rank insignia: the game's own (Awards/images/ranks, 25 of them), our drawn badge until the art is there.
  void RankInsignia(ImDrawList* d, const Stage& st, ImVec2 centre, float h, int rank) {
    char name[16];
    std::snprintf(name, sizeof(name), "rank_%02d", std::clamp(rank, 1, 25));
    const float w = kit::ArtAt(nullptr, name, centre, h, IM_COL32(255, 255, 255, 255), false);
    if (w > 0) kit::ArtAt(d, name, ImVec2(centre.x - w * 0.5f, centre.y), h);
    else RankBadge(d, st, centre, rank);
  }

  // The frame every new look page shares: the game's logo with REBORN beside it, the player's card at the right
  // (the insignia, the name, the rank, Edit), the players and servers online under it. Returns true when Edit was
  // pressed with the mouse.
  bool MenuChrome(ImDrawList* d, ImGuiIO& io, const Stage& st, const std::string& status, bool status_ok, bool logo = true) {
    const kit::Fonts& f = kit::GetFonts();
    (void)io;
    // The logo (the menu's own, white with the stars and 1943) or our brand until it is there.
    const float lw = logo ? kit::ArtAt(d, "logo", st.P(48, 17 + 28), st(56)) : 1.0f;
    if (!logo) {
    } else if (lw > 0) {
      kit::GameText(d, kit::GameFont::kHeadlineMedium, st(22), ImVec2(st.P(48, 0).x + lw + st(10), st.P(0, 52).y), kit::kGold,
                    "REBORN");
    } else {
      kit::Brand(d, st.P(48, 28), st.u);
    }
    // The player's card.
    const std::string name = settings_.Get("player_name", "Soldier");
    const std::string rank = "RANK " + std::to_string(Rank()) + " \xC2\xB7 " + Upper(kRanks[Rank() - 1]);
    const float nw = std::max(kit::TextWidth(f.ui_bold, st(14), name), kit::SpacedWidth(f.typed, st(10), rank, st(1.5f)));
    const float ew = kit::SpacedWidth(f.ui_bold, st(10.25f), "EDIT", st(1.5f));
    const float w = st(10) + st(30) + st(12) + nw + st(14) + ew + st(14);
    const ImVec2 a(st.P(1232, 0).x - w, st.P(0, 22).y), b(st.P(1232, 0).x, st.P(0, 70).y);
    const bool hover = ImGui::IsMouseHoveringRect(a, b);
    kit::PaperArt(d, a, b);
    d->AddRect(a, b, kit::kBorder, 0, 0, 1.0f);
    RankInsignia(d, st, ImVec2(a.x + st(10) + st(15), (a.y + b.y) * 0.5f), st(30), Rank());
    d->AddText(f.ui_bold, st(14), ImVec2(a.x + st(52), a.y + st(7)), kit::kInk, name.c_str());
    kit::Spaced(d, f.typed, st(10), ImVec2(a.x + st(52), a.y + st(29)), kit::kInkSection, rank, st(1.5f));
    d->AddRectFilled(ImVec2(b.x - ew - st(24), a.y + st(10)), ImVec2(b.x - ew - st(23), b.y - st(10)), kit::kRule);
    kit::Spaced(d, f.ui_bold, st(10.25f), ImVec2(b.x - ew - st(12), a.y + st(18)), hover ? kit::kStamp : kit::kInkSection, "EDIT",
                st(1.5f));
    // The line under it: players and servers online, or a match running.
    const float sw = kit::SpacedWidth(f.typed, st(11.25f), status, st(2));
    d->AddCircleFilled(ImVec2(b.x - sw - st(11), st.P(0, 84).y), st(3), status_ok ? kit::kGreenBright : kit::kStamp);
    kit::Spaced(d, f.typed, st(11.25f), ImVec2(b.x - sw, st.P(0, 79).y), kit::kKhakiText, status, st(2));
    return hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  }

  // The playtest stamp and the version, at the foot of the new look pages.
  void MenuFoot(ImDrawList* d, const Stage& st) {
    const kit::Fonts& f = kit::GetFonts();
    const std::string v = Upper(UpdateNote());  // "UP TO DATE · 0.3.0", "DOWNLOADING 40%"...
    const float vw = kit::SpacedWidth(f.typed, st(10), v, st(2));
    kit::Spaced(d, f.typed, st(10), ImVec2(st.P(1232 - 300, 0).x - vw, st.P(0, 720 - 24 - 34 + 14).y), kit::kKhakiDim, v, st(2));
    if (kit::WatermarkOn()) kit::Stamp(d, st.P(1240 - 118, 720 - 26 - 30), st.u, -4, 0.92f);
  }

  // The players' and servers' line for the chrome.
  std::string OnlineStatus(bool& ok) {
    const auto servers = list_.servers();
    ok = list_.reachable();
    // On a server's own backend (Direct connect to a LAN only server): said, since its list is not the main one.
    const std::string where = own_backend_.empty() ? std::string() : "BACKEND " + Upper(own_backend_) + " \xC2\xB7 ";
    if (!list_.asked()) {  // nothing has been asked of the servers yet (server_list.h Want)
      ok = true;
      return where + "CONNECTING TO THE SERVERS";
    }
    if (!ok) return where + "SERVERS NOT REACHABLE";
    return where + fmt::format("{} {} ONLINE \xC2\xB7 {} {}", list_.players_online(), list_.players_online() == 1 ? "PLAYER" : "PLAYERS",
                               servers.size(), servers.size() == 1 ? "SERVER" : "SERVERS");
  }

  // The total score from the stat.<label> lines (as StatsPage adds them up).
  double ScoreTotal() {
    double total = 0;
    for (const auto& [key, value] : settings_.values) {
      if (key.rfind("stat.", 0) != 0) continue;
      const size_t comma = value.find(',');
      if (comma != std::string::npos) total += std::atof(value.c_str() + comma + 1);
    }
    return total;
  }
  // How far the player is to the next rank (0 to 1), and the minutes left to it (0 at the top rank).
  float RankProgress(int& minutes_left) {
    static constexpr int kMinutes[] = {0,   10,  25,  45,  70,   100,  140,  190,  250,  320,  400, 500,
                                       620, 760, 920, 1100, 1300, 1550, 1850, 2200, 2600, 3100, 3700};
    const double minutes = std::atof(settings_.Get("match_seconds", "0").c_str()) / 60.0;
    const int r = Rank();
    if (r >= int(std::size(kMinutes))) {
      minutes_left = 0;
      return 1;
    }
    const double from = kMinutes[r - 1], to = kMinutes[r];
    minutes_left = int(std::max(0.0, to - minutes) + 0.999);
    return float(std::clamp((minutes - from) / std::max(1.0, to - from), 0.0, 1.0));
  }

  // The busiest server with room (the Quick Play card); null with none listed.
  const ServerInfo* QuickPlayServer(std::vector<ServerInfo>& keep) {
    keep = list_.servers();
    const ServerInfo* best = nullptr;
    for (const auto& sv : keep)
      // As Quick Play chooses (QuickJoin): never an experimental tick rate.
      if (sv.Tick() == 30 && sv.players < sv.max_players && (!best || sv.players > best->players)) best = &sv;
    return best;
  }

  // Back to the menu from its pages: Home before the game runs (its row `home_row`), the main menu after.
  void BackToMenu(int home_row, int main_row) {
    page_ = pre_game_ ? Page::kHome : Page::kMain;
    home_row_ = home_row, row_ = main_row;
  }

  void QuickPlay() {
    REXLOG_INFO("BF menu: Quick Play");
    if (!list_.asked() || list_.servers().empty()) {  // the list first: the join follows its answer (OnDraw)
      quick_asked_at_ = list_.answers(), quick_waiting_ = ImGui::GetTime() + 8.0;
      list_.RefreshNow();
      menu_error_ = "Looking for a server...", menu_error_at_ = ImGui::GetTime();
      return;
    }
    if (QuickJoin()) return;
    menu_error_ = "No server has room right now. Try again in a moment, or open the server browser.";
    menu_error_at_ = ImGui::GetTime();
  }
  // The server browser, at once (until v0.4.0 a playstyle question came first: Modern is gone, TekRantGaming, 13 Nov
  // 2026).
  void OpenBrowser() {
    page_ = Page::kServers, row_ = 0, scroll_ = 0, servers_from_home_ = pre_game_;
    list_.RefreshNow();
    REXLOG_INFO("BF menu: server browser");
  }

  // A green button with a glyph and a label in the game's own lettering; true when clicked. `key`: the keyboard's
  // key shown, when not KeyFor(button).
  bool GreenButton(ImDrawList* d, const Stage& st, ImVec2 a, ImVec2 b, kit::Button button, const std::string& label,
                   const char* key = nullptr) {
    const bool hover = ImGui::IsMouseHoveringRect(a, b);
    d->AddRectFilled(ImVec2(a.x, b.y), ImVec2(b.x, b.y + st(3)), kit::kGreenShadow);
    d->AddRectFilled(a, b, hover ? IM_COL32(106, 154, 60, 255) : kit::kGreen);
    const float h = b.y - a.y;
    const float gw = kit::Glyph(d, PromptStyle(), button, key ? key : KeyFor(button), ImVec2(a.x + st(12), (a.y + b.y) * 0.5f), st(20));
    const float cap = st(14);
    kit::GameText(d, kit::GameFont::kText, cap, ImVec2(a.x + st(12) + gw + st(10), a.y + (h - cap) * 0.5f), kit::kCream, label,
                  st(1));
    return hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  }

  // A paper card with a typed eyebrow, a heading and a line or two of text; `glyph` (if any) at its top right.
  // Returns true when clicked.
  bool InfoCard(ImDrawList* d, const Stage& st, ImVec2 a, ImVec2 b, int icon, kit::Button glyph, const std::string& eyebrow,
                const std::string& heading, const std::string& text) {
    const kit::Fonts& f = kit::GetFonts();
    const bool hover = ImGui::IsMouseHoveringRect(a, b);
    kit::PaperArt(d, a, b);
    if (hover) d->AddRect(a, b, kit::kGreen, 0, 0, st(2));
    if (icon >= 0) DrawCardIcon(d, st, icon, ImVec2(a.x + st(14), a.y + st(14)));
    if (glyph != kit::Button::kKey)
      kit::Glyph(d, PromptStyle(), glyph, KeyFor(glyph), ImVec2(b.x - st(16) - st(20), a.y + st(14) + st(10)), st(20));
    // (Under the icon, which ends 59 units down: at 56 the eyebrow sat behind it, TekRantGaming, 12 Nov 2026.)
    kit::Spaced(d, f.typed, st(10), ImVec2(a.x + st(18), a.y + st(70)), kit::kInkSection, eyebrow, st(2));
    d->AddText(f.ui_bold, st(15), ImVec2(a.x + st(18), a.y + st(84)), kit::kInk, heading.c_str());
    kit::Wrapped(d, f.ui, st(12.75f), ImVec2(a.x + st(18), a.y + st(108)), kit::kInkFaint, text, b.x - a.x - st(36));
    return hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  }

  void MenuPage(ImDrawList* d, ImGuiIO& io, Input in) {
    // Home while the game is not at its own menu yet: before it runs, and while it starts behind this page (started
    // for the shader page, PrepareShadersAtStart): a launcher page covers the game's films, title and sign in and
    // reads the controller itself; a server picked meanwhile is joined once the game's menu is up (Join's pending_).
    const bool home = pre_game_ || MenuDriverStage() < 2;
    page_ = home ? Page::kHome : Page::kMain;
    NotePage(home ? "home" : "main menu");
    UpdateOnMenu();
    UpdateTick();
    WhatsNewTick();
    OwnBackendTick();
    StartProjectArt();
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    const ImVec2 sz = io.DisplaySize;
    MenuBackdrop(d, sz, st);
    bool online_ok = false;
    const std::string status = OnlineStatus(online_ok);
    const bool edit = MenuChrome(d, io, st, status, online_ok);
    // ---- the rail: the rows, the chosen one on paper with the red tab and its glyph
    const int n = kMenuRowCount;
    if (!quit_popup_) {
      if (in.up) home_row_ = (home_row_ + n - 1) % n;
      if (in.down) home_row_ = (home_row_ + 1) % n;
    }
    home_row_ = std::clamp(home_row_, 0, n - 1);
    const ImVec2 ra = st.P(48, 93), rb = st.P(400, 620);
    d->AddRectFilled(ra, rb, IM_COL32(13, 15, 11, 204));
    d->AddRect(ra, rb, IM_COL32(241, 234, 211, 30), 0, 0, 1.0f);
    int clicked = -1;
    float y = ra.y;
    for (int i = 0; i < n; ++i) {
      if (i == kRowUpdate || i == kRowQuit) y += st(12);  // a rule's worth of air between the groups
      const MenuRow& row = kMenuRows[i];
      const float h = row.compact ? st(32) : st(41);
      const ImVec2 a(ra.x, y), b(rb.x, y + h);
      const bool sel = home_row_ == i;
      const bool hover = ImGui::IsMouseHoveringRect(a, b);
      if (hover && io.MouseDelta.x + io.MouseDelta.y != 0 && !quit_popup_) home_row_ = i;
      if (sel) {
        kit::PaperArt(d, a, b);
        d->AddRectFilled(a, ImVec2(a.x + st(5), b.y), kit::kStamp);
      }
      d->AddRectFilled(ImVec2(a.x, b.y - 1), ImVec2(b.x, b.y), IM_COL32(241, 234, 211, 20));
      const bool locked = i == kRowUnlockables;
      kit::Spaced(d, f.typed, st(10), ImVec2(a.x + st(24), a.y + h * 0.5f - st(5)), sel ? kit::kInkSection : kit::kKhakiDim,
                  fmt::format("{:02d}", i + 1), st(1.5f));
      const float cap = row.compact ? st(13) : st(19);
      kit::GameText(d, kit::GameFont::kHeadlineMedium, cap, ImVec2(a.x + st(52), a.y + (h - cap) * 0.5f),
                    sel ? kit::kInk : locked ? kit::kKhakiDim : kit::kKhaki, row.name);
      std::string note;
      ImU32 note_col = sel ? kit::kInkSection : kit::kKhakiDim;
      if (locked) note = "COMING SOON";
      else if (i == kRowUpdate) note = Upper(UpdateNote()), note_col = sel ? kit::kGreenDark : kit::kGreenBright;
      else if (i == kRowHelp) note = "TUTORIAL, CONTROLS, CREDITS";
      if (sel) {
        kit::Glyph(d, PromptStyle(), kit::Button::kA, "Enter", ImVec2(b.x - st(18) - st(20), a.y + h * 0.5f), st(20));
        if (!note.empty() && i != kRowUnlockables && i != kRowUpdate) note.clear();  // the glyph has the room
      }
      if (!note.empty()) {
        const float nw = kit::SpacedWidth(f.typed, st(9.5f), note, st(1.5f));
        kit::Spaced(d, f.typed, st(9.5f), ImVec2(b.x - st(18) - (sel ? st(28) : 0) - nw, a.y + h * 0.5f - st(5)), note_col, note,
                    st(1.5f));
      }
      y += h;
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !quit_popup_) clicked = i;
    }
    // ---- the Quick Play card: the busiest server with room and its map's picture
    {
      std::vector<ServerInfo> keep;
      const ServerInfo* best = QuickPlayServer(keep);
      const ImVec2 ha = st.P(427, 93), hb = st.P(1232, 380);
      const ImVec2 band(ha.x, hb.y - st(110));
      d->PushClipRect(ha, hb, true);
      MapPicture(d, st.u * 1.5f, ha, ImVec2(hb.x, band.y), best ? best->map : "Wake Island");
      d->PopClipRect();
      {  // the chip
        const std::string chip = "QUICK PLAY \xC2\xB7 THE BUSIEST SERVER WITH ROOM";
        const float cw = kit::SpacedWidth(f.ui_bold, st(10.25f), chip, st(1));
        const ImVec2 c0(ha.x + st(20), ha.y + st(20)), c1(c0.x + cw + st(22), c0.y + st(22));
        d->AddRectFilled(c0, c1, kit::kStrip);
        kit::Spaced(d, f.ui_bold, st(10.25f), ImVec2(c0.x + st(11), c0.y + st(4)), kit::kStripText, chip, st(1));
      }
      kit::PaperArt(d, band, hb);
      const std::string eyebrow = !online_ok ? "THE SERVER LIST IS NOT REACHABLE RIGHT NOW"
                                  : !best      ? "NO SERVER WITH ROOM RIGHT NOW"
                                               : fmt::format("{} \xC2\xB7 {} / {} PLAYERS{}", best->official ? "OFFICIAL" : "COMMUNITY",
                                                             best->players, best->max_players,
                                                             best->ping_ms >= 0 ? fmt::format(" \xC2\xB7 {} MS", best->ping_ms) : "");
      kit::Spaced(d, f.typed, st(11.25f), ImVec2(band.x + st(20), band.y + st(14)), kit::kInkSection, eyebrow, st(2));
      kit::GameText(d, kit::GameFont::kHeadline, st(32), ImVec2(band.x + st(20), band.y + st(31)), kit::kInk,
                    Upper(best ? (best->map.empty() ? "Wake Island" : best->map) : "Wake Island"));
      const std::string sub = best ? best->name + (best->next_map.empty() ? "" : " \xC2\xB7 next map " + best->next_map)
                                   : "Quick Play joins the busiest server with room.";
      d->AddText(f.ui, st(13.75f), ImVec2(band.x + st(20), band.y + st(74)), kit::kInkFaint, sub.c_str());
      // Quick Play at the right.
      const ImVec2 q0(hb.x - st(20) - st(170), band.y + st(18)), q1(q0.x + st(170), q0.y + st(40));
      if (GreenButton(d, st, q0, q1, kit::Button::kA, "QUICK PLAY")) clicked = kRowQuick;
      if (!menu_error_.empty() && ImGui::GetTime() - menu_error_at_ < 8.0) {
        d->AddText(f.ui, st(12.75f), ImVec2(band.x + st(20), band.y + st(92)), kit::kRed, menu_error_.c_str());
      }
    }
    // ---- the playtest notice, a strip
    {
      const ImVec2 a = st.P(427, 396), b = st.P(1232, 437);
      kit::PaperArt(d, a, b);
      d->AddRectFilled(a, ImVec2(a.x + st(5), b.y), kit::kStamp);
      float x = a.x + st(16);
      const float pw = kit::ArtAt(d, "corsair", ImVec2(x, (a.y + b.y) * 0.5f), st(30));
      x += (pw > 0 ? pw : 0) + st(14);
      kit::Spaced(d, f.typed, st(10.5f), ImVec2(x, a.y + st(16)), kit::kStamp, "PLAYTEST NOTICE", st(2));
      x += kit::SpacedWidth(f.typed, st(10.5f), "PLAYTEST NOTICE", st(2)) + st(18);
      // (Until 13 Nov 2026 it told players to avoid flying with mouse and keyboard; pull request 11's mouse flying
      // is in, on TekRantGaming's word.)
      const std::string t = "New: planes fly with the mouse. Left and right roll, back and forward pitch, A and D the rudder. "
                            "Tell us how it feels.";
      kit::Wrapped(d, f.ui, st(13.25f), ImVec2(x, a.y + st(12)), kit::kInk, t, b.x - x - st(14));
    }
    // ---- the service record, Discord and Steam
    {
      const ImVec2 a = st.P(427, 453), b = st.P(779, 643);
      kit::PaperArt(d, a, b);
      d->PushClipRect(a, b, true);
      kit::ArtAt(d, "portrait_us_rifleman", ImVec2(a.x + st(8), b.y - st(87)), st(176));
      d->PopClipRect();
      RankInsignia(d, st, ImVec2(b.x - st(16) - st(32), a.y + st(16) + st(32)), st(64), Rank());
      const float tx = a.x + st(131);
      kit::Spaced(d, f.typed, st(10), ImVec2(tx, a.y + st(18)), kit::kInkSection, "SERVICE RECORD", st(2));
      const std::string name = settings_.Get("player_name", "Soldier");
      d->AddText(f.ui_bold, st(15), ImVec2(tx, a.y + st(32)), kit::kInk, name.c_str());
      const double seconds = std::atof(settings_.Get("match_seconds", "0").c_str());
      const std::string l1 = fmt::format("{} \xC2\xB7 rank {} \xC2\xB7 Rifleman", kRanks[Rank() - 1], Rank());
      const std::string l2 = fmt::format("{:.0f} score \xC2\xB7 {} h {:02d} min in the field", ScoreTotal(), int(seconds / 3600),
                                         int(seconds / 60) % 60);
      d->AddText(f.ui, st(12.75f), ImVec2(tx, a.y + st(56)), kit::kInkFaint, l1.c_str());
      d->AddText(f.ui, st(12.75f), ImVec2(tx, a.y + st(72)), kit::kInkFaint, l2.c_str());
      int left = 0;
      const float progress = RankProgress(left);
      const ImVec2 p0(tx, a.y + st(96)), p1(b.x - st(118), a.y + st(102));
      d->AddRectFilled(p0, p1, IM_COL32(34, 36, 27, 30));
      d->AddRectFilled(p0, ImVec2(p0.x + (p1.x - p0.x) * progress, p1.y), kit::kGreen);
      const std::string next = Rank() < int(std::size(kRanks)) ? fmt::format("{} MIN TO {}", left, Upper(kRanks[Rank()])) : "THE TOP RANK";
      kit::Spaced(d, f.typed, st(8.75f), ImVec2(tx, a.y + st(108)), kit::kInkSection, next, st(1.5f));
    }
    int card = -1;
    if (InfoCard(d, st, st.P(795, 453), st.P(1005, 643), 0, kit::Button::kX, "COMMUNITY", "Join our Discord",
                 "Playtest news, servers and bug reports: discord.gg/8eakYSvBZS"))
      card = 0;
    if (InfoCard(d, st, st.P(1021, 453), st.P(1232, 643), 1, kit::Button::kY, "SHORTCUT", steam_added_ ? "Added to Steam" : "Add to Steam",
                 steam_added_ ? "In your Steam library, with the game's artwork." : "A shortcut with the game's artwork and controller support in Big Picture."))
      card = 1;
    // ---- the foot
    Prompts(d, st, st.P(48, 720 - 24 - 34), {{kit::Button::kA, "SELECT"}, {kit::Button::kB, "QUIT"}}, in, 15);
    MenuFoot(d, st);
    CheckSteamOnce();
    if (SteamDialog(d, io, st, in)) return;
    if (quit_popup_) {
      bool yes = false;
      if (Question(d, io, st, "QUIT THE GAME?", "Battlefield 1943 Reborn closes. Your settings are saved.", "Cancel", "Quit",
                   in, yes)) {
        quit_popup_ = false;
        REXLOG_INFO("BF menu: quit {}", yes ? "chosen" : "cancelled");
        if (yes && quit_) quit_();
      }
      return;
    }
    if (edit || in.view) return BeginAccount(false);
    if (in.x || card == 0) OpenUrl(kDiscordUrl);
    if (in.y || card == 1) AddToSteam();
    if (in.back) {
      quit_popup_ = true, popup_row_ = 0;
      return;
    }
    if (in.accept) clicked = home_row_;
    if (clicked < 0) return;
    home_row_ = clicked;
    REXLOG_INFO("BF menu: {}", kMenuRows[clicked].name);
    switch (clicked) {
      case kRowQuick: return QuickPlay();
      case kRowBrowser: return OpenBrowser();
      case kRowPrivateMatch:
        page_ = Page::kPrivateMatch, pm_row_ = 0, pm_error_.clear();
        LoadPrivateMatchChoices();
        return;
      case kRowStats: page_ = Page::kStats; return;
      case kRowLeaderboards: page_ = Page::kLeaderboards, lb_from_pause_ = false, lb_asked_at_ = -100; return;
      case kRowAchievements: page_ = Page::kAchievements, ach_scroll_ = 0, ach_from_pause_ = false; return;
      case kRowSettings:
        // (from Home also while the game starts behind it: a launcher page reads the controller itself)
        page_ = Page::kSettings, from_home_ = home, from_pause_ = false, tab_ = 0, row_ = 0, scroll_ = 0, sub_ = kSubNone;
        return;
      case kRowHelp: page_ = Page::kHelp, help_row_ = 0; return;
      case kRowUnlockables: return;  // nothing yet (TekRantGaming, 8 Nov)
      case kRowUpdate: return UpdateNow();
      case kRowReport: page_ = Page::kReport; return;
      case kRowProjects: page_ = Page::kProjects, project_ = 0; return;
      case kRowQuit: quit_popup_ = true, popup_row_ = 0; return;
      default: return;
    }
  }
