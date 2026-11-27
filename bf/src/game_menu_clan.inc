// Clan tags on the name page (clan_tags.h; TekRantGaming, 16 Nov 2026: "can be left blank or can have custom text,
// shows up next to a username ... like this [clan]Username"). Part of the GameMenu class (game_menu.cpp includes it in
// the class body, after game_menu_launcher.inc's AccountPage, which calls these).
//
// The page's field is two boxes in a row as the name is shown, [TAG]Name: a short CLAN TAG box, then the SOLDIER NAME
// one. RB (F4, or a click on a box) moves the typing between them; the keyboard, the on screen keys and Delete type
// into the one that has it. Save checks the tag with our filter first (text_filter.h CheckTag: 2 to 4 letters or
// digits, not Dev or a lookalike, no profanity, not profanity with the name) and says why; the backend checks again
// and decides (server/plasma.py put_tag), and its refusal shows on the page's error line. A tag saved after the
// game signed in is worn at once (the backend is told then); a name, from the next start.

  std::string tag_text_;       // the clan tag box
  bool tag_mode_ = false;      // the typing goes to the clan tag
  std::string tag_refusal_;    // the backend's words for a refused tag, kept for the page
  bool box_menu_ = false;      // a box's right click menu is open (BoxMenu), at box_menu_at_
  ImVec2 box_menu_at_;
  bool box_menu_direct_ = false;  // that menu is Direct connect's address box's (game_menu_browser.inc DirectPanel)

  static bool TagChar(char c) { return static_cast<unsigned char>(c) < 128 && std::isalnum(static_cast<unsigned char>(c)); }
  void TagBegin() {
    tag_text_ = settings_.Get("clan_tag", "");
    tag_mode_ = false, box_menu_ = false;
    if (std::string r = clan::TakeRefusal(); !r.empty()) tag_refusal_ = r;
  }
  // A key of the on screen keys (in the case Capitals says), or one typed (`as_typed`: as the keyboard gave it).
  void TagType(char c, bool as_typed = false) {
    if (int(tag_text_.size()) >= text_filter::kTagMax || !TagChar(c)) return;
    if (std::isalpha(static_cast<unsigned char>(c)) && !as_typed)
      c = name_caps_ ? char(std::toupper(static_cast<unsigned char>(c))) : char(std::tolower(static_cast<unsigned char>(c)));
    tag_text_ += c;
    name_error_.clear(), tag_refusal_.clear();
  }
  void TagDelete() {
    if (!tag_text_.empty()) tag_text_.pop_back();
    name_error_.clear(), tag_refusal_.clear();
  }
  // RB, F4 or a click: the typing to the other box.
  void TagSwitch(bool to_tag) {
    if (code_mode_ || tag_mode_ == to_tag) return;
    tag_mode_ = to_tag;
    name_caps_ = to_tag ? true : name_text_.empty();  // tags are usually capitals
    REXLOG_INFO("BF launcher: name page typing into the {}", to_tag ? "clan tag" : "soldier name");
  }
  // The tag in the box checked for the name about to be saved (false, with the error line saying why).
  bool TagCheck(const std::string& name) {
    text_filter::TagProblem p = text_filter::CheckTag(tag_text_, name);
    // The developers' own tag, typed by the player whose protected name this is (its account on this PC): asked for
    // as Dev, and the backend decides (it gives Dev to the team's names only and tells anyone else why; TekRantGaming,
    // 16 Nov 2026: the page refused his own Dev). Lookalikes (D3V, Devs) stay refused here.
    if (p == text_filter::TagProblem::kReserved && TagIsDevWord() && AccountHoldsName(name)) {
      tag_text_ = "Dev";
      p = text_filter::TagProblem::kNone;
    }
    if (p == text_filter::TagProblem::kNone) return true;
    name_error_ = text_filter::TagProblemText(p);
    tag_mode_ = true;
    REXLOG_INFO("BF clan tag: \"{}\" refused on the name page ({})", tag_text_, text_filter::TagProblemCode(p));
    return false;
  }
  bool TagIsDevWord() const {
    if (tag_text_.size() != 3) return false;
    std::string lower;
    for (char c : tag_text_) lower += char(std::tolower(static_cast<unsigned char>(c)));
    return lower == "dev";
  }
  // Whether this PC's account (its recovery code) holds this name: the name protected or recovered here.
  bool AccountHoldsName(const std::string& name) const {
    const std::string acct = settings_.Get("account_name", "");
    if (settings_.Get("account_code", "").empty() || acct.empty() || acct.size() != name.size()) return false;
    for (size_t i = 0; i < acct.size(); ++i)
      if (std::tolower(static_cast<unsigned char>(acct[i])) != std::tolower(static_cast<unsigned char>(name[i]))) return false;
    return true;
  }
  // Saved with the name (after TagCheck): the backend is told at once when the game is signed in, else after the
  // start's name check.
  void TagSave() {
    const bool changed = tag_text_ != settings_.Get("clan_tag", "");
    if (tag_text_.empty()) settings_.values.erase("clan_tag");
    else settings_.values["clan_tag"] = tag_text_;
    settings_.Save();
    clan::SetSavedTag(tag_text_);
    if (!changed) return;
    REXLOG_INFO("BF clan tag: the player chose \"{}\"", tag_text_);
    tag_refusal_.clear();
    if (!signed_in_once_) return;
    // A new name is played from the next start: its tag goes with it then, not now under the name played this start
    // (16 Nov 2026: a recovered developer's Dev went out under the old name and was refused as reserved).
    const std::string saved = settings_.Get("player_name", ""), now = PlayerName();
    bool same = saved.size() == now.size();
    for (size_t i = 0; same && i < saved.size(); ++i)
      same = std::tolower(static_cast<unsigned char>(saved[i])) == std::tolower(static_cast<unsigned char>(now[i]));
    if (!same) {
      REXLOG_INFO("BF clan tag: worn with the name {} from the next start", saved);
      return;
    }
    clan::SendOwnTag(now, OnlinePlayerId());
  }

  // The boxes (or the recovery code's one box), at x, y over w; y moves past them.
  void NameFields(ImDrawList* d, const Stage& st, float x, float& y, float w) {
    const kit::Fonts& f = kit::GetFonts();
    const bool blink_on = std::fmod(ImGui::GetTime(), 1.0) < 0.6;
    const bool menu_click = BoxMenu(st);  // a click the right click menu took is not a box's
    auto box = [&](float bx, float bw, const std::string& text, bool active, int max) {
      const ImVec2 n0(bx, y), n1(bx + bw, y + st(58));
      d->AddRectFilled(n0, n1, active ? kit::kPaperLight : kit::kPaperDim);
      d->AddRect(n0, n1, active ? kit::kGreen : kit::kInkFaint, 0, 0, st(active ? 3 : 1.5f));
      d->AddText(f.typed, st(30), ImVec2(bx + st(14), y + st(11)), kit::kInk, text.c_str());
      if (active && blink_on) {
        const float cx = bx + st(16) + kit::TextWidth(f.typed, st(30), text);
        d->AddRectFilled(ImVec2(cx, y + st(12)), ImVec2(cx + st(3), y + st(46)), kit::kInk);
      }
      const size_t shown = &text == &name_text_ ? player_name::Chars(text) : text.size();  // the sign is one
      const std::string count = std::to_string(shown) + " / " + std::to_string(max);
      d->AddText(f.typed, st(14), ImVec2(bx + bw - st(14) - kit::TextWidth(f.typed, st(14), count), y + st(22)),
                 kit::kInkFaint, count.c_str());
      // A click on a box puts the typing there; a right click also opens the box's menu (PASTE, CLEAR).
      if (ImGui::IsMouseHoveringRect(n0, n1) && !menu_click) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) TagSwitch(&text == &tag_text_);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
          TagSwitch(&text == &tag_text_);
          box_menu_ = true, box_menu_at_ = ImGui::GetIO().MousePos, box_menu_direct_ = false;
          REXLOG_INFO("BF launcher: the name page's right click menu opened");
        }
      }
    };
    if (code_mode_) {
      kit::Spaced(d, f.typed, st(14), ImVec2(x, y), kit::kInkFaint, "RECOVERY CODE", st(2));
      y += st(22);
      box(x, w, name_text_, true, account::kCodeLength);
      y += st(66);
      return;
    }
    const float tw = st(170), gap = st(16), nx = x + tw + gap, nw = w - tw - gap;
    kit::Spaced(d, f.typed, st(14), ImVec2(x, y), tag_mode_ ? kit::kInk : kit::kInkFaint, "CLAN TAG", st(2));
    kit::Spaced(d, f.typed, st(14), ImVec2(nx, y), tag_mode_ ? kit::kInkFaint : kit::kInk, "SOLDIER NAME", st(2));
    // The tag as others will see it: the Dev tag (the developers') in its colours, the backend's word.
    const std::string worn = clan::OwnTag();
    if (clan::OwnTagConfirmed() && clan::IsDevTag(worn)) {
      const float ww = clan::TaggedWidth(f.typed, st(14), "", worn);
      clan::DrawTagged(d, f.typed, st(14), ImVec2(x + tw - ww, y), kit::kInk, "", worn, kit::kInk);
    }
    y += st(22);
    box(x, tw, tag_text_, tag_mode_, text_filter::kTagMax);
    box(nx, nw, name_text_, !tag_mode_, kNameMax);
    y += st(66);
  }
  // The right click menu of a name page box (TekRantGaming, 16 Nov 2026: "a right click did not give a context menu"),
  // and of Direct connect's address box (20 Nov 2026): PASTE and CLEAR for the box with the typing, at the mouse, over everything. A click anywhere closes it. True when
  // this frame's click was the menu's (an item, or closing it).
  bool BoxMenu(const Stage& st) {
    if (!box_menu_) return false;
    ImDrawList* fd = ImGui::GetForegroundDrawList();
    const kit::Fonts& f = kit::GetFonts();
    static const char* const kItems[] = {"PASTE", "CLEAR"};
    const float iw = st(180), ih = st(42);
    const ImVec2 p0 = box_menu_at_, p1(p0.x + iw, p0.y + ih * 2);
    fd->AddRectFilled(ImVec2(p0.x + st(4), p0.y + st(4)), ImVec2(p1.x + st(4), p1.y + st(4)), IM_COL32(0, 0, 0, 90));
    int hit = -1;
    for (int i = 0; i < 2; ++i) {
      const ImVec2 a(p0.x, p0.y + ih * i), b(p1.x, p0.y + ih * (i + 1));
      const bool hover = ImGui::IsMouseHoveringRect(a, b);
      fd->AddRectFilled(a, b, hover ? kit::kPaperLight : kit::kPaperDim);
      if (hover) fd->AddRectFilled(a, ImVec2(a.x + st(4), b.y), kit::kGreen);
      kit::Spaced(fd, f.typed, st(16), ImVec2(a.x + st(16), a.y + st(12)), kit::kInk, kItems[i], st(2));
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) hit = i;
    }
    fd->AddRect(p0, p1, kit::kInk, 0, 0, st(1.5f));
    if (hit == 0) box_menu_direct_ ? DirectPaste(paste::Text(), "menu") : NamePaste(paste::Text(), "menu");
    if (hit == 1) {
      if (box_menu_direct_) direct_text_.clear(), direct_error_.clear();
      else NameClear();
    }
    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    if (clicked) box_menu_ = false;
    return clicked;
  }
  // The error line's words when nothing is wrong: what the box with the typing takes.
  const char* NameFieldHint() const {
    if (code_mode_) return "20 letters and numbers";
    return tag_mode_ ? "Clan tag: 2 to 4 letters or numbers, or leave it blank (RB or F4: back to the name)"
                     : "3 to 15 letters, numbers or underscores (RB or F4: your clan tag)";
  }
