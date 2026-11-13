// The pause in the new look, Version A1 (TekRantGaming, 12 Nov 2026: "Build it"), included inside GameMenu
// (game_menu.cpp) after game_menu_settings.inc. In place of the game's pause, as before (TekRantGaming, 10 Nov 2026: a
// replacement whose rows do what they say, by the game's own commands): the match goes on behind, darkened; the rows
// in a column at the left under PAUSED in the game's own lettering; the field report card at the right (the server,
// its map and movement, the time in the field, the squad, the comms with their keys, the last chat lines, a private
// match's reach); Exit Game and Suicide ask first; My Squad finds an open squad or leaves it. Settings opens the
// settings page (game_menu_settings.inc) and comes back here. The mockup (runs\ui-designs, build2.py a1_pause) is
// 1920 x 1080; sizes here are its at 720 lines, on the Stage.

  double match_began_ = -1;  // the match's first frame (UpdateWalk): the field report's time in the field

  // The server joined, from the list (null while it is not listed).
  const ServerInfo* JoinedServer(std::vector<ServerInfo>& keep) {
    keep = list_.servers();
    for (const ServerInfo& sv : keep)
      if (!joined_gid_.empty() && sv.gid == joined_gid_) return &sv;
    return nullptr;
  }

  // A card over the field with a question: the eyebrow, the title in the game's lettering, the text, a green button
  // (`yes`, its button) and a paper one (`no`, its button). Returns 1 for the first, 0 for the second, -1 for neither.
  int PauseCard(ImDrawList* d, ImGuiIO& io, const Stage& st, const Input& in, bool opened_now, const std::string& eyebrow,
                const std::string& title, const std::string& text, const std::string& yes, kit::Button yes_button,
                const std::string& no, kit::Button no_button) {
    const kit::Fonts& f = kit::GetFonts();
    d->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(6, 7, 5, 120));
    const ImVec2 a = st.P(540, 240), b = st.P(1160, 470);
    kit::PaperArt(d, a, b);
    d->AddRectFilled(a, ImVec2(a.x + st(5), b.y), kit::kStamp);
    kit::Spaced(d, f.typed, st(8), ImVec2(a.x + st(28), a.y + st(22)), kit::kInkSection, eyebrow, st(2));
    kit::GameText(d, kit::GameFont::kHeadline, st(28), ImVec2(a.x + st(28), a.y + st(40)), kit::kInk, title);
    kit::Wrapped(d, f.ui, st(12), ImVec2(a.x + st(28), a.y + st(88)), kit::kInk, text, b.x - a.x - st(56));
    const float yw = kit::GameTextWidth(kit::GameFont::kText, st(14), yes, st(1)) + st(70);
    const ImVec2 y0(a.x + st(28), b.y - st(66)), y1(y0.x + yw, b.y - st(26));
    const float nw = kit::GameTextWidth(kit::GameFont::kText, st(14), no, st(1)) + st(70);
    const ImVec2 n0(y1.x + st(14), y0.y), n1(n0.x + nw, y1.y);
    const bool yes_click = GreenButton(d, st, y0, y1, yes_button, yes);
    const bool nh = ImGui::IsMouseHoveringRect(n0, n1);
    d->AddRectFilled(n0, n1, nh ? kit::kPaperLight : kit::kPaperDim);
    d->AddRect(n0, n1, kit::kBorder, 0, 0, 1.0f);
    const float gw = kit::Glyph(d, PromptStyle(), no_button, KeyFor(no_button), ImVec2(n0.x + st(12), (n0.y + n1.y) * 0.5f), st(20));
    kit::GameText(d, kit::GameFont::kText, st(14), ImVec2(n0.x + st(12) + gw + st(10), n0.y + (st(40) - st(14)) * 0.5f), kit::kInk, no,
                  st(1));
    auto pressed = [&in](kit::Button bt) {
      return bt == kit::Button::kA ? in.accept : bt == kit::Button::kB ? in.back : bt == kit::Button::kX ? in.x : bt == kit::Button::kY ? in.y : false;
    };
    // The A that opened the card this frame is not its answer (the old pause: YES on Exit Game).
    if (yes_click || (pressed(yes_button) && !(opened_now && yes_button == kit::Button::kA))) return 1;
    if ((nh && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) || pressed(no_button) || (no_button != kit::Button::kB && in.back)) return 0;
    return -1;
  }

  // The field report: the server, its map and movement, the time in the field, the squad, the comms and their keys,
  // the last chat lines, a private match hosted here.
  void FieldReport(ImDrawList* d, const Stage& st, const Input& in, const ServerInfo* sv, const std::string& map, int minutes) {
    const kit::Fonts& f = kit::GetFonts();
    const ImVec2 a = st.P(800, 93), b = st.P(1232, 503);
    kit::PaperArt(d, a, b);
    const float x = a.x + st(20), x1 = b.x - st(20), wrap = x1 - x;
    float y = a.y + st(18);
    const std::string server = sv ? sv->name : joining_.empty() ? "This server" : joining_;
    std::string eyebrow = "FIELD REPORT \xC2\xB7 " + Upper(server.substr(0, 30));
    eyebrow += fmt::format(" \xC2\xB7 {}HZ", sv ? sv->Tick() : TickRate());  // 0.4.2: the match's tick rate after its name
    if (sv) eyebrow += sv->official ? " \xC2\xB7 OFFICIAL" : " \xC2\xB7 COMMUNITY";
    kit::Spaced(d, f.typed, st(8), ImVec2(x, y), kit::kInkSection, eyebrow, st(2));
    y += st(22);
    kit::GameText(d, kit::GameFont::kHeadline, st(24), ImVec2(x, y), kit::kInk, Upper(map));
    y += st(24) + st(8);
    std::string line;
    if (sv) line = fmt::format("{} / {} PLAYERS \xC2\xB7 ", sv->players, sv->max_players);
    line += fmt::format("{} MIN IN THE FIELD", minutes);
    kit::Spaced(d, f.typed, st(7.5f), ImVec2(x, y), kit::kInkSection, line, st(1.5f));
    y += st(22);
    auto rule = [&](const char* head) {
      d->AddLine(ImVec2(x, y), ImVec2(x1, y), kit::kRule, 1.0f);
      y += st(10);
      kit::Spaced(d, f.typed, st(8), ImVec2(x, y), kit::kInkSection, head, st(2));
      y += st(18);
    };
    // The squad (comms.h: the game's own squad number, read with the pad).
    rule("YOUR SQUAD");
    const int squad = comms::Squad();
    d->AddText(f.ui_bold, st(12), ImVec2(x, y), kit::kInk, squad > 0 ? fmt::format("Squad {}", squad).c_str() : "Not in a squad");
    d->AddText(f.ui, st(10.5f), ImVec2(x, y + st(17)), kit::kInkFaint,
               squad > 0 ? "My Squad leaves it; your squad mates are spawn points." : "My Squad finds an open one: spawn on your squad mates.");
    y += st(36);
    // The comms: the voice mode and its key, the text chat's keys.
    rule("COMMS");
    {
      const std::string voice = Cvar("bf_voice_mode");
      const std::string ptt = Cvar("keybind_push_to_talk");
      const std::string vt = voice == "push" ? fmt::format("VOICE: PUSH TO TALK, HOLD {}", Upper(ptt.empty() ? "B" : ptt))
                             : voice == "open" ? "VOICE: OPEN MIC" : "VOICE OFF";
      float cx = x;
      cx += Chip(d, st, ImVec2(cx, y), st(18), vt, voice == "off" ? kit::kPaperDim : kit::kStrip, voice == "off" ? kit::kInkFaint : kit::kStripText,
                 voice == "off") + st(6);
      if (Truthy(Cvar("bf_text_chat"))) {
        const std::pair<std::string, const char*> keys[] = {{Cvar("keybind_chat_all"), "ALL"}, {Cvar("keybind_chat_team"), "TEAM"},
                                                            {Cvar("keybind_chat_squad"), "SQUAD"}};
        for (const auto& [k, name] : keys) {
          const std::string t = Upper(k.empty() ? "?" : k) + " " + name;
          if (cx + ChipWidth(st, t) > x1) break;
          cx += Chip(d, st, ImVec2(cx, y), st(18), t, kit::kPaper, kit::kInk, true) + st(6);
        }
      } else {
        Chip(d, st, ImVec2(cx, y), st(18), "TEXT CHAT OFF", kit::kPaperDim, kit::kInkFaint, true);
      }
      if (!comms::Connected()) {
        y += st(24);
        kit::Spaced(d, f.typed, st(7), ImVec2(x, y), kit::kStamp, "THE SERVER'S CHAT RELAY HAS NOT ANSWERED YET", st(1.5f));
        y += st(4);
      }
      y += st(28);
    }
    // The last chat lines (the newest last), the channel in its colour.
    {
      const std::vector<comms::ChatLine> lines = comms::RecentChat(4);
      if (lines.empty()) {
        kit::Spaced(d, f.typed, st(7.5f), ImVec2(x, y), kit::kInkSection, "NO CHAT YET THIS MATCH", st(1.5f));
        y += st(18);
      }
      for (const comms::ChatLine& l : lines) {
        const ImU32 col = l.notice ? kit::kInkSection : l.channel == comms::kSquad ? kit::kGreenDark : l.channel == comms::kTeam ? IM_COL32(30, 70, 150, 255) : kit::kInk;
        const std::string head = l.notice ? "" : std::string(l.channel == comms::kSquad ? "[Squad] " : l.channel == comms::kTeam ? "[Team] " : "") + l.name + ": ";
        const float hw = kit::TextWidth(f.ui_bold, st(10.5f), head);
        d->AddText(f.ui_bold, st(10.5f), ImVec2(x, y), col, head.c_str());
        d->AddText(f.ui, st(10.5f), ImVec2(x + hw, y), kit::kInk, l.text.c_str(), nullptr, wrap - hw);
        y += st(15);
      }
    }
    // A private match hosted on this PC: its players, its reach, the address for friends (Y copies it).
    if (pm_hosting_ && pm_joining_) {
      const bf1943::PrivateMatchStatus& pm = pm_status_;
      y += st(4);
      rule(fmt::format("HOSTED ON THIS PC \xC2\xB7 {} {}", pm.players, pm.players == 1 ? "PLAYER" : "PLAYERS").c_str());
      const std::string reach = pm.reachable == "true"    ? "Friends outside your home can reach it."
                                : pm.reachable == "false" ? fmt::format("Friends outside your home cannot reach it yet: open UDP {} on your router.", pm.port)
                                                          : fmt::format("Friends on the internet need UDP {} open on your router.", pm.port);
      y += kit::Wrapped(d, f.ui, st(10.5f), ImVec2(x, y), kit::kInk, reach + " They join with Direct connect in the server browser.", wrap) + st(8);
      const bool copied = ImGui::GetTime() - pm_copied_at_ < 3;
      const float gw = kit::Glyph(d, PromptStyle(), kit::Button::kY, KeyFor(kit::Button::kY), ImVec2(x, y + st(10)), st(20));
      kit::Spaced(d, f.ui_bold, st(9), ImVec2(x + gw + st(8), y + st(4)), copied ? kit::kGreenDark : kit::kInk,
                  copied ? "ADDRESS COPIED: SEND IT TO YOUR FRIENDS" : "COPY THE ADDRESS FOR FRIENDS", st(1.2f));
      if (in.y) CopyPrivateAddress();
    }
  }

  // The SMG's sight sliders (0.4.2, with Settings > Gameplay > Sight alignment on and an SMG in the hands): in place
  // of the map and the report, the match undimmed behind them and the aim held, so the gun is seen moving. Each slider
  // follows the mouse while its button is down; the values are the gun's own and saved (fov.cpp).
  int sight_drag_ = -1;
  // Whether the sight alignment is being tuned: an internal build's only (BF_INTERNAL_TOOLS; never a release's).
  bool SightTuning() {
#if defined(BF_INTERNAL_TOOLS)
    return Truthy(Cvar("bf_smg_sight_tune")) && Truthy(Cvar("bf_smg_sights"));
#else
    return false;
#endif
  }
  bool SightSliders(ImDrawList* d, ImGuiIO& io, const Stage& st) {
    double v[6];
    const std::string gun = SightTuning() ? SmgSightTuneGun(v) : std::string();
    g_smg_tune_hold.store(!gun.empty(), std::memory_order_relaxed);
    if (gun.empty()) {
      sight_drag_ = -1;
      return false;
    }
    const kit::Fonts& f = kit::GetFonts();
    const ImVec2 a = st.P(880, 93), b = st.P(1232, 470);
    kit::PaperArt(d, a, b);
    const float x = a.x + st(18), x1 = b.x - st(18);
    float y = a.y + st(16);
    kit::Spaced(d, f.typed, st(8), ImVec2(x, y), kit::kInkSection, "SIGHT ALIGNMENT \xC2\xB7 " + Upper(gun), st(2));
    y += st(20);
    y += kit::Wrapped(d, f.ui, st(10.5f), ImVec2(x, y), kit::kInk,
                      "Drag a slider until the front post's tip sits in the middle of the view. The aim is held "
                      "while this is open. Saved for this gun.", x1 - x) + st(10);
    struct Row {
      const char* name;
      double lo, hi;
      const char* unit;
      int digits;
    };
    static const Row kRows[6] = {{"Left / right", -0.15, 0.15, "m", 4}, {"Down / up", -0.10, 0.10, "m", 4},
                                 {"Zoom (back / forward)", -0.25, 0.35, "m", 3}, {"Yaw (turn)", -15, 15, "deg", 2},
                                 {"Pitch (tilt)", -15, 15, "deg", 2},  {"Roll", -15, 15, "deg", 2}};
    if (!io.MouseDown[0]) sight_drag_ = -1;
    bool changed = false;
    for (int i = 0; i < 6; ++i) {
      const Row& r = kRows[i];
      const std::string value = fmt::format("{:+.{}f} {}", v[i], r.digits, r.unit);
      d->AddText(f.ui_bold, st(11), ImVec2(x, y), kit::kInk, r.name);
      d->AddText(f.ui_bold, st(11), ImVec2(x1 - kit::TextWidth(f.ui_bold, st(11), value), y), kit::kInk, value.c_str());
      const float ty = y + st(26);
      const ImVec2 t0(x, ty - st(9)), t1(x1, ty + st(9));
      if (ImGui::IsMouseHoveringRect(t0, t1) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) sight_drag_ = i;
      if (sight_drag_ == i) {
        const double at = std::clamp(double((io.MousePos.x - x) / (x1 - x)), 0.0, 1.0);
        const double next = r.lo + (r.hi - r.lo) * at;
        if (std::abs(next - v[i]) > 1e-6) v[i] = next, changed = true;
      }
      const float fx = x + (x1 - x) * float(std::clamp((v[i] - r.lo) / (r.hi - r.lo), 0.0, 1.0));
      const float zx = x + (x1 - x) * float((0.0 - r.lo) / (r.hi - r.lo));
      d->AddRectFilled(ImVec2(x, ty - st(1.5f)), ImVec2(x1, ty + st(1.5f)), kit::kRule);
      d->AddRectFilled(ImVec2(zx - 1, ty - st(5)), ImVec2(zx + 1, ty + st(5)), kit::kInkFaint);  // zero
      d->AddRectFilled(ImVec2(fx - st(3), ty - st(8)), ImVec2(fx + st(3), ty + st(8)), sight_drag_ == i ? kit::kStamp : kit::kInk);
      y += st(46);
    }
    // Reset: the built in values.
    const ImVec2 r0(x, y), r1(x + st(120), y + st(24));
    const bool over = ImGui::IsMouseHoveringRect(r0, r1);
    d->AddRectFilled(r0, r1, over ? kit::kPaperLight : kit::kPaperDim);
    d->AddRect(r0, r1, kit::kBorder, 0, 0, 1.0f);
    d->AddText(f.ui_bold, st(10.5f), ImVec2(r0.x + st(12), r0.y + (st(24) - st(10.5f) * 1.45f) * 0.5f), kit::kInk, "Reset this gun");
    const bool reset = over && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (changed || reset) SmgSightTuneSet(v, reset);
    return true;
  }

  void PauseA1Page(ImDrawList* d, ImGuiIO& io, const Input& in_raw) {
    NotePage("pause");
    Input in = in_raw;
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    const ImVec2 sz = io.DisplaySize;
    const int count = int(std::size(kPauseRows));
    // The match behind, darkened; the column at the left darker, with a thin rule at its edge.
    double tune_values[6];
    const bool tuning = SightTuning() && !SmgSightTuneGun(tune_values).empty();
    if (!tuning) d->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(6, 7, 5, 120));
    // 0.4.2 (a screen wider than 16:9): the column stands at the screen's left edge and the report at its right edge
    // (lst, rst: the stage moved there), and the map between them takes the room: as large as the height allows.
    Stage lst = st, rst = st;
    lst.ox = 0, rst.ox = sz.x - 1280 * st.u;
    const float cx1 = lst.P(427, 0).x;
    d->AddRectFilled(ImVec2(0, 0), ImVec2(cx1, sz.y), IM_COL32(13, 15, 11, 236));
    d->AddRectFilled(ImVec2(cx1, 0), ImVec2(cx1 + std::max(1.0f, st(1)), sz.y), IM_COL32(241, 234, 211, 40));
    // The logo, ghosted; the player's card and the match's line at the right (the chrome without its logo).
    const float lw = kit::ArtAt(d, "logo", lst.P(48, 45), st(56), IM_COL32(255, 255, 255, 70));
    if (lw > 0)
      kit::GameText(d, kit::GameFont::kHeadlineMedium, st(22), ImVec2(lst.P(48, 0).x + lw + st(10), lst.P(0, 52).y), IM_COL32(201, 183, 122, 90),
                    "REBORN");
    std::vector<ServerInfo> keep;
    const ServerInfo* sv = JoinedServer(keep);
    const std::string map = joining_map_.empty() ? "Wake Island" : joining_map_;
    const int minutes = match_began_ > 0 ? int((ImGui::GetTime() - match_began_) / 60) : 0;
    const bool edit = MenuChrome(d, io, rst, fmt::format("MATCH RUNNING \xC2\xB7 {} MIN IN THE FIELD", minutes), true, false);
    kit::Spaced(d, f.typed, st(8.5f), lst.P(48, 105), kit::kKhakiDim, fmt::format("IN THE FIELD \xC2\xB7 {} \xC2\xB7 {} MIN", Upper(map), minutes), st(2));
    kit::GameText(d, kit::GameFont::kHeadline, st(44), lst.P(48, 118), kit::kCream, "MENU");
    d->AddRectFilled(lst.P(48, 190), lst.P(90, 194), kit::kStamp);
    // The prompts first: a click on one is that button's press for the rest of the frame.
    Prompts(d, lst, lst.P(48, 662), {{kit::Button::kA, "SELECT"}, {kit::Button::kB, pause_confirm_ >= 0 ? "BACK" : "RESUME"}}, in, 15);
    MenuFoot(d, rst);
    // ---- the rows
    const bool rows_have_input = pause_confirm_ < 0;
    if (rows_have_input) {
      if (in.up) pause_row_ = (pause_row_ + count - 1) % count;
      if (in.down) pause_row_ = (pause_row_ + 1) % count;
    }
    pause_row_ = std::clamp(pause_row_, 0, count - 1);
    int clicked = -1;
    float y = lst.P(0, 238).y;
    for (int i = 0; i < count; ++i) {
      const bool compact = i >= kPauseExit;
      if (i == kPauseExit) y += st(12);
      const float h = compact ? st(36) : st(43);
      const ImVec2 a(lst.P(48, 0).x, y), b(lst.P(400, 0).x, y + h);
      const bool sel = i == pause_row_;
      const bool hover = rows_have_input && ImGui::IsMouseHoveringRect(a, b);
      if (hover && io.MouseDelta.x + io.MouseDelta.y != 0) pause_row_ = i;
      if (sel) {
        kit::PaperArt(d, a, b);
        d->AddRectFilled(a, ImVec2(a.x + st(5), b.y), kit::kStamp);
      }
      d->AddRectFilled(ImVec2(a.x, b.y - 1), ImVec2(b.x, b.y), IM_COL32(241, 234, 211, 20));
      kit::Spaced(d, f.typed, st(8), ImVec2(a.x + st(24), a.y + h * 0.5f - st(5)), sel ? kit::kInkSection : kit::kKhakiDim,
                  fmt::format("{:02d}", i + 1), st(1.5f));
      const float cap = compact ? st(14) : st(19);
      const bool danger = i == kPauseSuicide;
      kit::GameText(d, kit::GameFont::kHeadlineMedium, cap, ImVec2(a.x + st(52), a.y + (h - cap) * 0.5f),
                    sel ? (danger ? kit::kRed : kit::kInk) : danger ? kit::kStamp : kit::kKhaki, kPauseRows[i]);
      std::string note;
      if (i == kPauseSuicide) note = "RESPAWN AT A FLAG";
      else if (i == kPauseSquad) note = comms::Squad() > 0 ? fmt::format("SQUAD {}", comms::Squad()) : "NO SQUAD";
      else if (i == kPauseTeam) note = "TO THE OTHER SIDE";
      if (sel) {
        kit::Glyph(d, PromptStyle(), kit::Button::kA, "Enter", ImVec2(b.x - st(18) - st(20), a.y + h * 0.5f), st(20));
      } else if (!note.empty()) {
        const float nw = kit::SpacedWidth(f.typed, st(7.5f), note, st(1.5f));
        kit::Spaced(d, f.typed, st(7.5f), ImVec2(b.x - st(18) - nw, a.y + h * 0.5f - st(5)), kit::kKhakiDim, note, st(1.5f));
      }
      y += h;
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = i;
    }
    // ---- the SMG's sight sliders, or the field report and the map beside it
    if (SightSliders(d, io, rst)) {
    } else {
      FieldReport(d, rst, in, sv, map, minutes);
      const float mx0 = lst.P(436, 0).x, mx1 = rst.P(788, 0).x;
      // 4:3 ("a large 4:3 aspect, not a 1:1 square"): as wide as the room, as tall as the page allows
      const float mw = std::max(st(260), std::min(mx1 - mx0 - st(16), st(640 - 93 - 78) * 4.0f / 3.0f));
      ImVec2 box[2];
      box[0] = ImVec2((mx0 + mx1 - mw - st(16)) * 0.5f, st.P(0, 93).y);
      box[1] = ImVec2(box[0].x + mw + st(16), box[0].y + mw * 0.75f + st(78));
      PauseMap(d, io, st, in, rows_have_input, box);
    }
    // ---- the actions
    if (rows_have_input && in.accept) clicked = pause_row_;
    if (rows_have_input && in.back) {  // Start closed it already (OnDraw)
      ClosePause("B or Esc");
      return;
    }
    if (clicked >= 0) pause_row_ = clicked, PauseAct(clicked);
    if (rows_have_input && (edit || in.view) && pause_confirm_ < 0 && page_ == Page::kPause) return BeginAccount(false);
    // ---- the cards: My Squad, and the questions of Exit Game and Suicide
    if (pause_confirm_ == kPauseSquad) {
      const int r = PauseCard(d, io, st, in, clicked >= 0, "MY SQUAD", "SQUAD UP",
                              "Join an open squad to spawn on your squad mates, or leave the one you are in.", "FIND AN OPEN SQUAD",
                              kit::Button::kA, "LEAVE SQUAD", kit::Button::kX);
      if (r >= 0 && !(r == 0 && in.back && !in.x)) {  // B alone closes the card; X (or its click) leaves the squad
        const bool find = r == 1;
        REXLOG_INFO("BF pause: {} (the game's own command)", find ? "find an open squad" : "leave the squad");
        GameCommand(find ? "triggerLookingForSquad" : "leaveSquad", "");
        pause_confirm_ = -1;
        open_ = false;
        return;
      }
      if (in.back) pause_confirm_ = -1;
    } else if (pause_confirm_ == kPauseTeam) {
      const int r = PauseCard(d, io, st, in, clicked >= 0, "CHANGE TEAM", "SWITCH SIDES?",
                              "You join the other team. Your soldier dies and you spawn at your new team's base; it "
                              "counts as a death. A server may refuse it to keep the teams even.",
                              "YES", kit::Button::kA, "NO", kit::Button::kB);
      if (r == 1) {
        pause_confirm_ = -1;
        ChangeTeamNow();
        open_ = false;
        return;
      }
      if (r == 0) pause_confirm_ = -1;
    } else if (pause_confirm_ >= 0) {
      const bool exit = pause_confirm_ == kPauseExit;
      const int r = PauseCard(d, io, st, in, clicked >= 0, exit ? "EXIT GAME" : "SUICIDE", exit ? "LEAVE THIS MATCH?" : "END THIS LIFE?",
                              exit ? (pm_hosting_ ? "You go back to the main menu, and your custom match ends for everyone in it."
                                                  : "You go back to the main menu. Your score in this round stays with the server.")
                                   : "Your soldier dies and you pick a new spawn point. It counts as a death.",
                              "YES", kit::Button::kA, "NO", kit::Button::kB);
      if (r == 1) {
        REXLOG_INFO("BF pause: {} chosen (the game's own command)", exit ? "Exit Game" : "Suicide");
        pause_confirm_ = -1;
        if (exit) {
          Leave();
          return;
        }
        SuicideNow();
        open_ = false;
        return;
      }
      if (r == 0) pause_confirm_ = -1;
    }
  }
