// The soldier name page, "P2: one page and a briefing" (TekRantGaming's pick, 24 Nov 2026: players found the old name
// page confusing; "clear instructions on how a protected name works and how to recover an account with the recovery
// code", mouse, keyboard and controller; the clan tag left of the name as the game shows it; no text cut off). Part of
// the GameMenu class (game_menu.cpp includes it in the class body after game_menu_clan.inc).
//
// The page: a paper form at the left with two tabs, NEW SOLDIER and I HAVE A RECOVERY CODE (LB / RB, F3, a click). The
// New soldier tab: the clan tag box, then the soldier name box; a line with the name's rules or what is wrong; the
// PROTECT THIS NAME switch, on by default (X, F2, a click), so Save protects; Save and continue (Start, Enter) and
// Random name (Y, F5). A dark rail at the right: the briefing on how protection works, the on screen keys while a
// controller types, and after a first protection the recovery code's card (shown once: Copy, Open the folder, a tick
// for "I have saved my recovery code" before Continue). The recovery tab: the code in four boxes of five, Paste, and
// where the code is. The account's logic is game_menu_launcher.inc's (ProtectName, RecoverAccount, ApplyAccountResult).
//
// Every line is wrapped in its box or made smaller until it fits (NpFit): nothing is cut off at any display size.

  // ---- state
  enum NpFocus { kNpTag = 0, kNpName = 1, kNpSwitch = 2, kNpSave = 3, kNpRandom = 4 };
  int np_focus_ = kNpName;
  bool np_protect_ = true;        // the PROTECT THIS NAME switch
  int np_card_ = 0;               // the code card in the rail: 0 none, 1 a new code (the tick before Continue), 2 shown again
  bool np_ticked_ = false;        // "I have saved my recovery code"
  bool np_pending_save_ = false;  // Save asked for a protection: the page goes on when it is done
  double np_flash_at_ = -10;      // Continue pressed before the tick: the tick box flashes
  std::string np_note_;           // the card's last action (copied, folder opened)
  std::string np_keep_;           // the name typed before the recovery tab, back with the New soldier tab
  double np_note_at_ = -10;

  static constexpr ImU32 kNpRail = IM_COL32(13, 15, 11, 222), kNpRailEdge = IM_COL32(241, 234, 211, 34);
  static constexpr ImU32 kNpIconBox = IM_COL32(43, 45, 36, 255);

  // The page's state when it opens (BeginAccount).
  void NpBegin() {
    np_focus_ = kNpName, np_card_ = 0, np_ticked_ = false, np_pending_save_ = false, np_note_.clear();
    np_protect_ = AccountOnline();
  }

  // ---- drawing helpers

  // The size at which `t` in `f` is no wider than `w` (at most `size`).
  static float NpFit(ImFont* f, float size, const std::string& t, float w, float track = 0) {
    const float tw = track > 0 ? kit::SpacedWidth(f, size, t, track) : kit::TextWidth(f, size, t);
    return tw > w && tw > 0 ? size * w / tw : size;
  }
  static float NpFitGame(kit::GameFont g, float cap, const std::string& t, float w, float track = 0) {
    const float tw = kit::GameTextWidth(g, cap, t, track);
    return tw > w && tw > 0 ? cap * w / tw : cap;
  }
  // A path broken at any character so each line fits `w` (a path has few spaces to wrap at). Returns the height.
  static float NpPathLines(ImDrawList* d, ImFont* f, float size, ImVec2 at, ImU32 col, const std::string& t, float w,
                           bool draw) {
    float y = at.y;
    const float lh = size * 1.25f;
    size_t i = 0;
    while (i < t.size()) {
      size_t n = 1;
      while (i + n < t.size() && kit::TextWidth(f, size, t.substr(i, n + 1)) <= w) ++n;
      // Break after a backslash when one is near the end of the line.
      if (i + n < t.size())
        if (const size_t bs = t.rfind('\\', i + n - 1); bs != std::string::npos && bs >= i + n / 2) n = bs - i + 1;
      if (draw) d->AddText(f, size, ImVec2(at.x, y), col, t.substr(i, n).c_str());
      y += lh, i += n;
    }
    return y - at.y;
  }
  // The small pictures of the briefing and the card, `s` square from `a`.
  static void NpIcon(ImDrawList* d, int kind, ImVec2 a, float s, ImU32 col) {
    const auto P = [&](float x, float y) { return ImVec2(a.x + x * s, a.y + y * s); };
    const float t = std::max(1.5f, s * 0.08f);
    switch (kind) {
      case 0:  // a padlock
        d->AddRect(P(0.24f, 0.46f), P(0.76f, 0.84f), col, 0, 0, t);
        d->PathArcTo(P(0.5f, 0.46f), s * 0.17f, IM_PI, 2 * IM_PI, 12);
        d->PathStroke(col, 0, t);
        d->AddCircleFilled(P(0.5f, 0.64f), s * 0.05f, col);
        break;
      case 1:  // two chevrons, a rank
        d->AddPolyline(std::array<ImVec2, 3>{P(0.2f, 0.3f), P(0.5f, 0.48f), P(0.8f, 0.3f)}.data(), 3, col, 0, t * 1.3f);
        d->AddPolyline(std::array<ImVec2, 3>{P(0.2f, 0.54f), P(0.5f, 0.72f), P(0.8f, 0.54f)}.data(), 3, col, 0, t * 1.3f);
        break;
      case 2:  // a key
        d->AddCircle(P(0.32f, 0.5f), s * 0.15f, col, 16, t);
        d->AddLine(P(0.47f, 0.5f), P(0.86f, 0.5f), col, t);
        d->AddLine(P(0.72f, 0.5f), P(0.72f, 0.66f), col, t);
        d->AddLine(P(0.83f, 0.5f), P(0.83f, 0.62f), col, t);
        break;
      case 3:  // no connection: the signal's arcs, struck through
        for (float r : {0.42f, 0.28f, 0.14f}) {
          d->PathArcTo(P(0.5f, 0.78f), s * r, IM_PI * 1.22f, IM_PI * 1.78f, 10);
          d->PathStroke(col, 0, t);
        }
        d->AddLine(P(0.18f, 0.2f), P(0.82f, 0.84f), col, t);
        break;
      case 4:  // a pen
        d->AddLine(P(0.22f, 0.78f), P(0.72f, 0.28f), col, t * 2.2f);
        d->AddTriangleFilled(P(0.16f, 0.84f), P(0.2f, 0.7f), P(0.3f, 0.8f), col);
        break;
      case 5:  // two sheets, a copy
        d->AddRect(P(0.36f, 0.34f), P(0.8f, 0.84f), col, 0, 0, t);
        d->AddLine(P(0.22f, 0.66f), P(0.22f, 0.18f), col, t);
        d->AddLine(P(0.22f, 0.18f), P(0.62f, 0.18f), col, t);
        break;
      case 6:  // a file with a folded corner
        d->AddPolyline(std::array<ImVec2, 6>{P(0.26f, 0.14f), P(0.58f, 0.14f), P(0.76f, 0.32f), P(0.76f, 0.86f), P(0.26f, 0.86f),
                                             P(0.26f, 0.14f)}.data(), 6, col, 0, t);
        d->AddPolyline(std::array<ImVec2, 3>{P(0.58f, 0.14f), P(0.58f, 0.32f), P(0.76f, 0.32f)}.data(), 3, col, 0, t);
        break;
      case 7:  // where: a pin
        d->PathArcTo(P(0.5f, 0.38f), s * 0.2f, IM_PI * 0.8f, IM_PI * 2.2f, 14);
        d->PathLineTo(P(0.5f, 0.86f));
        d->PathFillConvex(col);
        d->AddCircleFilled(P(0.5f, 0.38f), s * 0.07f, kNpIconBox);
        break;
      default: break;
    }
  }
  // A tick (ok) or a cross in a disc, `s` across, its left middle at `at`.
  static void NpMark(ImDrawList* d, ImVec2 at, float s, bool ok) {
    const ImVec2 c(at.x + s * 0.5f, at.y);
    d->AddCircleFilled(c, s * 0.5f, ok ? kit::kGreen : kit::kRed, 20);
    const float t = std::max(1.5f, s * 0.11f), r = s * 0.22f;
    if (ok) {
      d->AddPolyline(std::array<ImVec2, 3>{ImVec2(c.x - r, c.y), ImVec2(c.x - r * 0.25f, c.y + r * 0.75f), ImVec2(c.x + r, c.y - r * 0.7f)}.data(),
                     3, IM_COL32(255, 255, 255, 255), 0, t);
    } else {
      d->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), IM_COL32(255, 255, 255, 255), t);
      d->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), IM_COL32(255, 255, 255, 255), t);
    }
  }
  // A chip: a filled strip with a short label in the game's lettering, its left top at `at`, `h` tall. Its width.
  float NpChip(ImDrawList* d, const Stage& st, ImVec2 at, float h, ImU32 fill, const std::string& t, float max_w = 0,
               bool draw = true) {
    float cap = h * 0.5f;
    const float pad = st(8), track = st(1);
    const float tick = fill == kit::kGreen ? h * 0.62f : 0;  // a protected name's chip starts with a drawn tick
    if (max_w > 0) cap = NpFitGame(kit::GameFont::kText, cap, t, max_w - pad * 2 - tick, track);
    const float w = kit::GameTextWidth(kit::GameFont::kText, cap, t, track) + pad * 2;
    if (draw) {
      d->AddRectFilled(at, ImVec2(at.x + w + tick, at.y + h), fill);
      if (tick > 0) {
        const float tx = at.x + pad, ty = at.y + h * 0.5f, r = h * 0.2f;
        d->AddPolyline(std::array<ImVec2, 3>{ImVec2(tx, ty), ImVec2(tx + r * 0.8f, ty + r * 0.8f), ImVec2(tx + r * 2.1f, ty - r * 0.9f)}.data(),
                       3, IM_COL32(255, 255, 255, 255), 0, std::max(1.5f, h * 0.1f));
      }
      kit::GameText(d, kit::GameFont::kText, cap, ImVec2(at.x + pad + tick, at.y + (h - cap) * 0.5f), IM_COL32(255, 255, 255, 255), t, track);
    }
    return w + tick;
  }
  // A button on the page: the device's glyph (the keyboard's key or keys when it was used last), the label in the game's
  // lettering. kind 0 green, 1 dark (on the rail), 2 ink outline (on the paper). Fits `max_w` by making the label
  // smaller. Measures only with draw false.
  struct NpBtn {
    ImVec2 a, b;
    bool clicked = false;
  };
  NpBtn NpButton(ImDrawList* d, const Stage& st, ImVec2 at, float h, kit::Button button, const std::vector<std::string>& keys,
                 const std::string& label, int kind, bool enabled, bool focused, float max_w = 0, bool draw = true) {
    const kit::Style style = PromptStyle();
    const bool keyboard = style == kit::Style::kKeyboard;
    const float gh = h * 0.5f, pad = st(12), gap = st(10), track = st(1);
    float gw = 0;
    if (keyboard && !keys.empty()) {
      for (size_t i = 0; i < keys.size(); ++i)
        gw += kit::Glyph(nullptr, style, button, keys[i], ImVec2(0, 0), gh, false) + (i ? st(3) : 0);
    } else {
      gw = kit::Glyph(nullptr, style, button, keys.empty() ? KeyFor(button) : keys[0], ImVec2(0, 0), gh, false);
    }
    float cap = h * 0.34f;
    if (max_w > 0) cap = NpFitGame(kit::GameFont::kText, cap, label, max_w - pad * 2 - gw - gap, track);
    const float w = pad + gw + gap + kit::GameTextWidth(kit::GameFont::kText, cap, label, track) + pad + st(2);
    NpBtn r;
    r.a = at, r.b = ImVec2(at.x + w, at.y + h);
    if (!draw) return r;
    const bool hover = enabled && ImGui::IsMouseHoveringRect(r.a, r.b);
    ImU32 fill = 0, ink = kit::kCream;
    if (kind == 0) {
      fill = !enabled ? IM_COL32(120, 116, 100, 110) : hover ? IM_COL32(106, 154, 60, 255) : kit::kGreen;
      if (enabled) d->AddRectFilled(ImVec2(r.a.x, r.b.y), ImVec2(r.b.x, r.b.y + st(3)), kit::kGreenShadow);
      ink = enabled ? kit::kCream : IM_COL32(241, 234, 211, 150);
    } else if (kind == 1) {
      fill = hover ? IM_COL32(52, 54, 44, 255) : kNpIconBox;
      ink = enabled ? kit::kStripText : IM_COL32(242, 239, 230, 110);
    } else {
      fill = hover ? kit::kPaperDim : IM_COL32(0, 0, 0, 0);
      ink = enabled ? kit::kInk : IM_COL32(34, 36, 27, 110);
    }
    if (fill) d->AddRectFilled(r.a, r.b, fill);
    if (kind == 2) d->AddRect(r.a, r.b, ink, 0, 0, st(1.5f));
    if (kind == 1) d->AddRect(r.a, r.b, IM_COL32(241, 234, 211, 50), 0, 0, 1.0f);
    float x = r.a.x + pad;
    const float my = (r.a.y + r.b.y) * 0.5f;
    if (keyboard && !keys.empty()) {
      for (size_t i = 0; i < keys.size(); ++i) x += kit::Glyph(d, style, button, keys[i], ImVec2(x, my), gh) + st(3);
      x -= st(3);
    } else {
      x += kit::Glyph(d, style, button, keys.empty() ? KeyFor(button) : keys[0], ImVec2(x, my), gh);
    }
    kit::GameText(d, kit::GameFont::kText, cap, ImVec2(x + gap, my - cap * 0.5f), ink, label, track);
    if (focused) d->AddRect(ImVec2(r.a.x - st(3), r.a.y - st(3)), ImVec2(r.b.x + st(3), r.b.y + st(kind == 0 ? 6 : 3)), kit::kStamp, 0, 0, st(2));
    r.clicked = hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    return r;
  }

  // ---- the page's parts

  bool NpSameAsSaved() const {
    const std::string saved = settings_.Get("player_name", "");
    return name_text_.size() == saved.size() &&
           std::equal(name_text_.begin(), name_text_.end(), saved.begin(), [](char p, char q) {
             return std::tolower(static_cast<unsigned char>(p)) == std::tolower(static_cast<unsigned char>(q));
           });
  }
  // The tabs across the paper's top: true when the tab was clicked to the other one.
  bool NpTabs(ImDrawList* d, const Stage& st, float x, float y, float w) {
    const kit::Fonts& f = kit::GetFonts();
    const kit::Style style = PromptStyle();
    const float h = st(40);
    const bool code_off = NameProtected();  // this PC's name is protected already: a code is for a new PC
    bool clicked = false;
    float tx = x;
    const char* labels[2] = {"NEW SOLDIER", "I HAVE A RECOVERY CODE"};
    for (int i = 0; i < 2; ++i) {
      const bool on = (i == 1) == code_mode_;
      const kit::Button b = i == 0 ? kit::Button::kLB : kit::Button::kRB;
      const float gh = st(18);
      const float gw = kit::Glyph(nullptr, style, b, "F3", ImVec2(0, 0), gh, false);
      float size = NpFit(f.ui_bold, st(17), labels[i], (w - st(20)) * 0.5f - gw - st(36), st(1));
      const float tw = gw + st(10) + kit::SpacedWidth(f.ui_bold, size, labels[i], st(1)) + st(28);
      const ImVec2 a(tx, y), b2(tx + tw, y + h);
      const bool hover = ImGui::IsMouseHoveringRect(a, b2);
      if (on) d->AddRectFilled(a, b2, kit::kInk);
      else if (hover) d->AddRectFilled(a, b2, kit::kPaperDim);
      const ImU32 ink = on ? kit::kCream : (i == 1 && code_off) ? IM_COL32(107, 100, 80, 120) : kit::kInkSection;
      // The glyph before the first tab's words, after the second's (the shoulder buttons' sides).
      if (i == 0) {
        kit::Glyph(d, style, b, "F3", ImVec2(a.x + st(14), y + h * 0.5f), gh);
        kit::Spaced(d, f.ui_bold, size, ImVec2(a.x + st(14) + gw + st(10), y + (h - size * 1.3f) * 0.5f), ink, labels[i], st(1));
      } else {
        const float lw = kit::SpacedWidth(f.ui_bold, size, labels[i], st(1));
        kit::Spaced(d, f.ui_bold, size, ImVec2(a.x + st(14), y + (h - size * 1.3f) * 0.5f), ink, labels[i], st(1));
        kit::Glyph(d, style, b, "F3", ImVec2(a.x + st(14) + lw + st(10), y + h * 0.5f), gh);
      }
      if (hover && !on && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = true;
      tx = b2.x + st(4);
    }
    d->AddRectFilled(ImVec2(x, y + h), ImVec2(x + w, y + h + st(2.5f)), kit::kInk);
    return clicked;
  }
  // The heading with its chip at the right; the line under it. Returns the height used.
  float NpHeading(ImDrawList* d, const Stage& st, float x, float y, float w, const std::string& title, const std::string& chip,
                  ImU32 chip_fill, const std::string& sub) {
    const kit::Fonts& f = kit::GetFonts();
    const float ch = st(24);
    const float cw = chip.empty() ? 0 : NpChip(d, st, ImVec2(0, 0), ch, chip_fill, chip, w * 0.42f, false);
    const float hw = kit::Heading(d, st(46), ImVec2(x, y), kit::kInk, title, st(1), w - cw - (cw > 0 ? st(16) : 0));
    if (!chip.empty()) NpChip(d, st, ImVec2(x + hw + st(16), y + st(46) * 0.5f - ch * 0.5f), ch, chip_fill, chip, w * 0.42f);
    float hgt = st(54);
    if (!sub.empty()) hgt += kit::Wrapped(d, f.ui, st(17), ImVec2(x, y + hgt), kit::kInkFaint, sub, w) + st(6);
    return hgt;
  }
  // A text box on the paper; true when clicked (a right click opens PASTE / CLEAR).
  bool NpBox(ImDrawList* d, const Stage& st, ImVec2 a, ImVec2 b, const std::string& text, bool active, bool focused,
             const std::string& count, bool centred = false, float text_size = 0, bool caret = true) {
    const kit::Fonts& f = kit::GetFonts();
    d->AddRectFilled(a, b, active ? kit::kPaperLight : kit::kPaperDim);
    d->AddRect(a, b, active ? kit::kGreen : kit::kInkFaint, 0, 0, st(active ? 3 : 1.5f));
    if (focused && !active) d->AddRect(ImVec2(a.x - st(3), a.y - st(3)), ImVec2(b.x + st(3), b.y + st(3)), kit::kStamp, 0, 0, st(2));
    const float cw = count.empty() ? 0 : kit::TextWidth(f.typed, st(12), count) + st(14);
    const float room = b.x - a.x - st(28) - cw;
    float size = text_size > 0 ? text_size : st(28);
    if (!text.empty()) size = std::min(size, NpFit(f.typed, size, text, room));
    const float tw = kit::TextWidth(f.typed, size, text);
    const float tx = centred ? (a.x + b.x - tw) * 0.5f : a.x + st(14);
    const float ty = (a.y + b.y) * 0.5f - size * 0.62f;
    d->AddText(f.typed, size, ImVec2(tx, ty), kit::kInk, text.c_str());
    if (active && caret && std::fmod(ImGui::GetTime(), 1.0) < 0.6) {
      const float cx = tx + tw + st(2);
      d->AddRectFilled(ImVec2(cx, a.y + st(12)), ImVec2(cx + st(3), b.y - st(12)), kit::kInk);
    }
    if (!count.empty())
      d->AddText(f.typed, st(12), ImVec2(b.x - st(10) - kit::TextWidth(f.typed, st(12), count), (a.y + b.y) * 0.5f - st(7)), kit::kInkFaint,
                 count.c_str());
    return ImGui::IsMouseHoveringRect(a, b) && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  }
  bool NpBoxRightClicked(ImVec2 a, ImVec2 b) {
    return ImGui::IsMouseHoveringRect(a, b) && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
  }

  // What the name line says: the rules when all is well, else what is wrong. ok false when something is.
  std::string NpNameLine(bool& ok) const {
    ok = false;
    if (!name_error_.empty()) return name_error_;
    if (!tag_refusal_.empty()) return tag_refusal_;
    const bool same = NpSameAsSaved();
    if (acct_online_ == 3 && same && !NameProtected())
      return "This name is kept for the Battlefield 1943 Reborn team: nobody else can play under it or a name like it. Choose a name of your own.";
    if (acct_online_ == 2 && same && !NameProtected())
      return "Another player protected this name: online you would play as it with a number added. Choose another one.";
    if (!name_text_.empty() && !text_filter::NameAllowed(name_text_)) return "That name is not allowed. Choose another one.";
    ok = true;
    if (tag_mode_) return "Clan tag: 2 to 4 letters or numbers, or leave it empty. Shown before your name, like [TAG]Name.";
    return "3 to 15 letters, numbers or _ (underscore). The clan tag is optional.";
  }

  // The briefing in the rail (`compact`: under the on screen keys). Returns its height; draws nothing with draw false.
  float NpBriefing(ImDrawList* d, const Stage& st, float x, float y, float w, float k, bool compact, bool draw) {
    const kit::Fonts& f = kit::GetFonts();
    struct Point {
      int icon;
      const char* title;
      std::string text;
    };
    std::vector<Point> pts;
    std::string title;
    if (code_mode_) {
      title = "WHERE IS MY CODE?";
      pts = {{7, "On your old PC", "In the game folder, then user, then \"BF1943 Reborn recovery code.txt\". Open it and copy the "
                                   "line that starts with Recovery code."},
             {4, "Where you kept it", "On paper, a photo, or a password manager: wherever you saved it when you protected your name."},
             {2, "Twenty letters and numbers", "Like ABCDE FGHJK MNPQR STVWX. Spaces, dashes and small letters do not matter; O and 0, "
                                               "I and 1 count the same."},
             {3, "No code?", "A protected name cannot come back without its code: nobody can reset it. Make a new soldier on the "
                             "first tab instead."}};
    } else {
      title = "HOW PROTECTION WORKS";
      const std::string n = name_text_.empty() ? std::string("your name") : name_text_;
      pts = {{0, "Only you, online", "A protected name is yours on every Reborn server. Not protected, someone else could take it, and "
                                     "you would join with a number added."},
             {1, "Your rank goes with it", "Your rank, stats and awards are kept on the Reborn servers too, not only on this PC."},
             {2, "A recovery code, not a password", "No email, no password. Save shows a 20 character code once and keeps a copy "
                                                   "in the game folder. On a new PC: the I HAVE A RECOVERY CODE tab."},
             {3, "On a LAN or offline?", "Protection needs the Reborn servers. Your name still works; protect it later from EDIT "
                                         "on Home or in Settings."}};
    }
    float yy = y;
    if (!compact) {
      if (draw) kit::Spaced(d, f.typed, st(10.5f * k), ImVec2(x, yy), kit::kKhakiDim, code_mode_ ? "RECOVERY CODE" : "BRIEFING", st(2));
      yy += st(20 * k);
    }
    {
      const float hs = st((compact ? 22.0f : 32.0f) * k);
      if (draw) kit::Heading(d, hs, ImVec2(x, yy), kit::kCream, title, st(1), w);
      yy += hs * 1.12f + st(10 * k);
    }
    const float ib = st((compact ? 26.0f : 36.0f) * k), tx = x + ib + st(12), tw = w - ib - st(12);
    for (const Point& p : pts) {
      if (compact && p.icon == 3) continue;
      const float ts = NpFit(f.ui_bold, st(16.5f * k), p.title, tw, st(0.6f));
      const float bs = st((compact ? 14.0f : 15.5f) * k);
      const float th = ts * 1.3f + (compact ? 0 : kit::WrappedHeight(f.ui, bs, p.text, tw));
      if (draw) {
        d->AddRectFilled(ImVec2(x, yy), ImVec2(x + ib, yy + ib), kNpIconBox);
        NpIcon(d, p.icon, ImVec2(x + ib * 0.15f, yy + ib * 0.15f), ib * 0.7f, kit::kCream);
        kit::Spaced(d, f.ui_bold, ts, ImVec2(tx, yy), kit::kCream, p.title, st(0.6f));
        if (!compact) kit::Wrapped(d, f.ui, bs, ImVec2(tx, yy + ts * 1.3f), kit::kKhaki, p.text, tw);
      }
      yy += std::max(ib, th) + st((compact ? 8.0f : 14.0f) * k);
    }
    return yy - y;
  }

  // The recovery code's card in the rail. Returns its height; draws nothing with draw false. `act` gets what was clicked:
  // 1 Copy, 2 Open the folder, 3 the tick, 4 Continue (or Close).
  float NpCodeCard(ImDrawList* d, const Stage& st, float x, float y, float w, float k, bool draw, int& act) {
    const kit::Fonts& f = kit::GetFonts();
    const std::string name = settings_.Get("account_name", "");
    const std::string code = account::FormatCode(settings_.Get("account_code", ""));
    float yy = y;
    {  // the chips
      const float ch = st(24 * k);
      const std::string c1 = name + " IS PROTECTED";
      float cx = x;
      const float w1 = NpChip(d, st, ImVec2(cx, yy), ch, kit::kGreen, c1, np_card_ == 1 ? w * 0.66f : w, draw);
      cx += w1 + st(8);
      if (np_card_ == 1) NpChip(d, st, ImVec2(cx, yy), ch, kit::kStamp, "SHOWN ONCE", x + w - cx, draw);
      yy += ch + st(12 * k);
    }
    {
      const float hs = st(34 * k);
      if (draw) kit::Heading(d, hs, ImVec2(x, yy), kit::kCream, "YOUR RECOVERY CODE", st(1), w);
      yy += hs * 1.1f + st(6 * k);
    }
    {
      const std::string t = np_card_ == 1
                                ? "This is the only time the game shows it. It is the only key to your name: nobody can reset it or "
                                  "send it to you, and anyone who has it can take your name. Keep it private."
                                : "The only key to your name: nobody can reset it or send it to you. Anyone who has it can take your "
                                  "name, so keep it private.";
      const float bs = st(15 * k);
      if (draw) kit::Wrapped(d, f.ui, bs, ImVec2(x, yy), kit::kKhaki, t, w);
      yy += kit::WrappedHeight(f.ui, bs, t, w) + st(12 * k);
    }
    {  // the code, in four groups on paper
      const float bh = st(54 * k), gap = st(8);
      if (draw) kit::PaperArt(d, ImVec2(x, yy), ImVec2(x + w, yy + bh));
      std::vector<std::string> groups;
      for (size_t i = 0; i < code.size();) {
        const size_t sp = code.find(' ', i);
        groups.push_back(code.substr(i, sp == std::string::npos ? std::string::npos : sp - i));
        i = sp == std::string::npos ? code.size() : sp + 1;
      }
      if (!groups.empty()) {
        const float gw = (w - st(16) - gap * float(groups.size() - 1)) / float(groups.size());
        float size = st(26 * k);
        for (const std::string& g : groups) size = std::min(size, NpFit(f.typed, size, g, gw - st(10)));
        for (size_t i = 0; i < groups.size(); ++i) {
          const ImVec2 a(x + st(8) + float(i) * (gw + gap), yy + st(8)), b(a.x + gw, yy + bh - st(8));
          if (draw) {
            d->AddRectFilled(a, b, kit::kPaperLight);
            d->AddRect(a, b, kit::kBorder, 0, 0, st(1));
            const float tw = kit::TextWidth(f.typed, size, groups[i]);
            d->AddText(f.typed, size, ImVec2((a.x + b.x - tw) * 0.5f, (a.y + b.y) * 0.5f - size * 0.6f), kit::kInk, groups[i].c_str());
          }
        }
      }
      yy += bh + st(14 * k);
    }
    {  // the ways to keep it
      const float ib = st(28 * k), tx = x + ib + st(10), tw = w - ib - st(10), bs = st(15 * k);
      const std::string path = RecoveryFile().string();
      struct Way {
        int icon;
        std::string text;
      };
      const Way ways[3] = {{4, "Write it down, or take a photo of it with your phone."},
                           {5, "Copy it into a password manager or a note you keep."},
                           {6, "A copy is saved on this PC:"}};
      for (const Way& wy : ways) {
        float th = kit::WrappedHeight(f.ui, bs, wy.text, tw);
        if (draw) {
          d->AddRectFilled(ImVec2(x, yy), ImVec2(x + ib, yy + ib), kNpIconBox);
          NpIcon(d, wy.icon, ImVec2(x + ib * 0.15f, yy + ib * 0.15f), ib * 0.7f, kit::kCream);
          kit::Wrapped(d, f.ui, bs, ImVec2(tx, yy + std::max(0.0f, (ib - th) * 0.5f)), kit::kCream, wy.text, tw);
        }
        if (wy.icon == 6) {
          const float ps = st(12.5f * k);
          const float py = yy + std::max(th, ib * 0.5f) + st(4);
          const float ph = NpPathLines(d, f.typed, ps, ImVec2(tx + st(6), py + st(4)), kit::kCream, path, tw - st(12), false);
          if (draw) {
            d->AddRectFilled(ImVec2(tx, py), ImVec2(tx + tw, py + ph + st(8)), IM_COL32(241, 234, 211, 26));
            NpPathLines(d, f.typed, ps, ImVec2(tx + st(6), py + st(4)), kit::kCream, path, tw - st(12), true);
          }
          th = py - yy + ph + st(8);
          const std::string warn = "Deleting the game folder deletes this file too: keep a copy somewhere else.";
          const float ws = st(14 * k);
          if (draw) kit::Wrapped(d, f.ui, ws, ImVec2(tx, yy + th + st(4)), kit::kKhakiDim, warn, tw);
          th += st(4) + kit::WrappedHeight(f.ui, ws, warn, tw);
        }
        yy += std::max(ib, th) + st(10 * k);
      }
    }
    {  // Copy, Open the folder
      const float bh = st(36 * k);
      const float half = (w - st(10)) * 0.5f;
      const NpBtn c = NpButton(d, st, ImVec2(x, yy), bh, kit::Button::kY, {"C"}, "COPY CODE", 1, true, false, half, draw);
      const NpBtn o = NpButton(d, st, ImVec2(c.b.x + st(10), yy), bh, kit::Button::kX, {"O"}, "OPEN THE FOLDER", 1, true, false,
                               x + w - c.b.x - st(10), draw);
      if (c.clicked) act = 1;
      if (o.clicked) act = 2;
      yy += bh + st(8 * k);
      if (draw && !np_note_.empty() && ImGui::GetTime() - np_note_at_ < 4)
        kit::Spaced(d, f.typed, st(10 * k), ImVec2(x, yy), kit::kGreenBright, np_note_, st(1.5f));
      yy += st(16 * k);
    }
    {  // the tick, then Continue
      const float rh = st(46 * k);
      if (np_card_ == 1) {
        const ImVec2 a(x, yy), b(x + w, yy + rh);
        const bool flash = !np_ticked_ && ImGui::GetTime() - np_flash_at_ < 1.2 && std::fmod(ImGui::GetTime() * 4, 1.0) < 0.6;
        const bool hover = ImGui::IsMouseHoveringRect(a, b);
        const kit::Style style = PromptStyle();
        const float gw = kit::Glyph(nullptr, style, kit::Button::kA, "Space", ImVec2(0, 0), st(18 * k), false);
        const std::string label = "I have saved my recovery code";
        const float box = st(24 * k);
        const float ts = NpFit(f.ui_bold, st(17 * k), label, w - box - gw - st(40));
        if (draw) {
          d->AddRectFilled(a, b, hover ? IM_COL32(52, 54, 44, 255) : IM_COL32(0, 0, 0, 0));
          d->AddRect(a, b, np_ticked_ ? kit::kGreen : flash ? kit::kStamp : IM_COL32(206, 62, 48, 170), 0, 0, st(2));
          const ImVec2 b0(x + st(12), yy + (rh - box) * 0.5f), b1(b0.x + box, b0.y + box);
          d->AddRect(b0, b1, kit::kCream, 0, 0, st(2));
          if (np_ticked_)
            d->AddPolyline(std::array<ImVec2, 3>{ImVec2(b0.x + box * 0.2f, b0.y + box * 0.52f), ImVec2(b0.x + box * 0.42f, b0.y + box * 0.76f),
                                                 ImVec2(b0.x + box * 0.82f, b0.y + box * 0.24f)}.data(),
                           3, kit::kGreenBright, 0, st(3));
          kit::Spaced(d, f.ui_bold, ts, ImVec2(b1.x + st(12), yy + (rh - ts * 1.3f) * 0.5f), kit::kCream, label, 0);
          kit::Glyph(d, style, kit::Button::kA, "Space", ImVec2(x + w - st(12) - gw, yy + rh * 0.5f), st(18 * k));
        }
        if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) act = 3;
        yy += rh + st(12 * k);
      }
      const float bh = st(42 * k);
      const bool can = np_card_ == 2 || np_ticked_;
      const NpBtn go = NpButton(d, st, ImVec2(x, yy), bh, np_card_ == 2 ? kit::Button::kB : kit::Button::kStart,
                                {np_card_ == 2 ? "Esc" : "Enter"}, np_card_ == 2 ? "CLOSE" : "CONTINUE", 0, can, false, w, false);
      const NpBtn g2 = NpButton(d, st, ImVec2(x + w - (go.b.x - go.a.x), yy), bh, np_card_ == 2 ? kit::Button::kB : kit::Button::kStart,
                                {np_card_ == 2 ? "Esc" : "Enter"}, np_card_ == 2 ? "CLOSE" : "CONTINUE", 0, can, false, w, draw);
      if (g2.clicked) act = 4;
      yy += bh + st(4);
      if (np_card_ == 1 && !np_ticked_) {
        const std::string t = "Continue after the tick.";
        if (draw) kit::Spaced(d, f.typed, st(11.5f * k), ImVec2(x, yy - bh * 0.5f - st(7)), kit::kKhakiDim, t, st(1));
      }
    }
    return yy - y;
  }

  // ---- actions

  void NpFinish() {  // the page done: the setup's next step, or back where it was opened from
    np_pending_save_ = false;
    if (name_setup_) SetupNext();
    else page_ = name_from_;
  }
  void NpSave() {
    if (code_mode_) return RecoverAccount();
    if (acct_busy_.load()) return;
    const bool protect = np_protect_ && AccountOnline() && !(NameProtected() && NpSameAsSaved());
    REXLOG_INFO("BF name page: Save ({})", protect ? "with protection" : "without protection");
    if (protect) {
      np_pending_save_ = true;
      ProtectName();  // saves the name first (NameCommit); ApplyAccountResult goes on
      if (!acct_busy_.load()) np_pending_save_ = false;  // the name could not be saved: its line says why
      return;
    }
    if (!NameCommit()) return;
    NpFinish();
  }
  void NpCopyCode() {
    const std::string code = account::FormatCode(settings_.Get("account_code", ""));
    if (code.empty()) return;
    // A scripted test never writes the PC's clipboard (BF_DEV_SCRIPTED): it is logged only.
    if (const char* v = std::getenv("BF_DEV_SCRIPTED"); !(v && *v == '1')) ImGui::SetClipboardText(code.c_str());
    np_note_ = "COPIED", np_note_at_ = ImGui::GetTime();
    REXLOG_INFO("BF name page: the recovery code copied");
  }
  void NpOpenFolder() {
    const fs::path file = RecoveryFile();
    np_note_ = "FOLDER OPENED", np_note_at_ = ImGui::GetTime();
    REXLOG_INFO("BF name page: the recovery code's folder opened ({})", file.parent_path().string());
    if (const char* v = std::getenv("BF_DEV_NO_OPEN"); v && *v == '1') return;  // tests: logged only
#if defined(_WIN32)
    ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + file.wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
#else
    OpenUrl(file.parent_path().string());
#endif
  }
  // The tabs: the recovery code's box holds the code; the name typed so far comes back with the New soldier tab.
  void NpCodeTab(bool on) {
    if (on) np_keep_ = name_text_;
    BeginCodeMode(on);
    if (!on && !np_keep_.empty()) name_text_ = np_keep_, np_keep_.clear();
    np_focus_ = kNpName;
  }
  void NpCardAct(int act) {
    if (act == 1) NpCopyCode();
    else if (act == 2) NpOpenFolder();
    else if (act == 3 && np_card_ == 1) {
      np_ticked_ = !np_ticked_;
      REXLOG_INFO("BF name page: \"I have saved my recovery code\" {}", np_ticked_ ? "ticked" : "unticked");
    } else if (act == 4) {
      if (np_card_ == 1 && !np_ticked_) {
        np_flash_at_ = ImGui::GetTime();
        REXLOG_INFO("BF name page: Continue before the tick");
        return;
      }
      REXLOG_INFO("BF name page: the code card closed ({})", np_card_ == 1 ? "continue" : "close");
      const bool go_on = np_card_ == 1 && np_pending_save_;
      np_card_ = 0;
      acct_message_.clear();
      if (go_on) NpFinish();
    }
  }

  // ---- the page
  void AccountPage(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("account");
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    Chrome(d, io, st, name_setup_ ? 2 : 0, SceneBehind() ? 0.82f : 0);
    // The window's typing (see the old page: ImGui gives typed characters only while text input is asked for).
    {
      ImGuiContext& g = *ImGui::GetCurrentContext();
      g.PlatformImeData.WantTextInput = true;
      g.PlatformImeData.ViewportId = ImGui::GetMainViewport()->ID;
    }
    const bool pad = device_ == Device::kPad;
    const bool card = np_card_ != 0;
    const bool code_off = NameProtected();  // a recovery code is for a new PC (this one's name is protected already)
    // ---- the keyboard's own keys
    const bool enter_key = in.accept && accept_from_key_;
    if (!card) {
      if (io.InputQueueCharacters.Size > 0 && !typed_logged_) {
        typed_logged_ = true;
        REXLOG_INFO("BF launcher: the keyboard typed on the name page");
      }
      for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const ImWchar ch = io.InputQueueCharacters[i];
        bool typed = false;
        if (code_mode_) {
          if (ch < 128 && !code_off) CodeType(char(ch)), typed = true;
        } else if (tag_mode_) {
          if (ch < 128) TagType(char(ch), true), typed = true;
        } else if (ch == player_name::kTmCodePoint) {
          NameTypeTm(), typed = true;
        } else if (ch < 128 && NameChar(char(ch)) && player_name::CanType(name_text_)) {
          name_text_ += char(ch), name_error_.clear(), name_caps_ = false, typed = true;
        }
        if (typed) name_keys_ = false, np_focus_ = code_mode_ ? kNpName : tag_mode_ ? kNpTag : kNpName;
      }
      if (paste::Pressed() && !(code_mode_ && code_off)) NamePaste(paste::Text(), "keys");
      if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !(code_mode_ && code_off)) NameDelete();
    }
    const bool f2 = ImGui::IsKeyPressed(ImGuiKey_F2, false), f3 = ImGui::IsKeyPressed(ImGuiKey_F3, false);
    const bool f4 = ImGui::IsKeyPressed(ImGuiKey_F4, false), f5 = ImGui::IsKeyPressed(ImGuiKey_F5, false);
    const bool tab_key = ImGui::IsKeyPressed(ImGuiKey_Tab, false) || in.view;
    const bool shift = ImGui::GetIO().KeyShift;
    const bool ctrl_c = paste::CtrlDown() && ImGui::IsKeyPressed(ImGuiKey_C, false);

    // ---- the prompts (a click on one is that button's press for this frame)
    std::vector<Cue> cues;
    if (card) {
      cues = {{kit::Button::kY, "Copy code", "C"}, {kit::Button::kX, "Open the folder", "O"}};
      if (np_card_ == 1) {
        cues.push_back({kit::Button::kA, np_ticked_ ? "Untick" : "Tick", "Space"});
        cues.push_back({kit::Button::kStart, "Continue", "Enter", true});
      } else {
        cues.push_back({kit::Button::kB, "Close", "Esc"});
      }
    } else if (name_keys_) {
      cues = {{kit::Button::kA, "Type"}, {kit::Button::kX, "Delete", "Backspace"},
              {kit::Button::kStart, code_mode_ ? "Bring it back" : name_setup_ ? "Save and continue" : "Save", "Enter", true},
              {kit::Button::kB, "Done typing", "Esc"}};
    } else if (code_mode_) {
      if (pad && !code_off) cues.push_back({kit::Button::kA, "Type the code"});
      if (!code_off && pad) cues.push_back({kit::Button::kY, "Paste"});
      if (!code_off) cues.push_back({kit::Button::kStart, "Bring it back", "Enter", true});
      cues.push_back({kit::Button::kLB, "New soldier", "F3"});
      cues.push_back({kit::Button::kB, "Back", "Esc"});
    } else {
      if (pad) cues.push_back({kit::Button::kA, np_focus_ <= kNpName ? "Edit" : "Select"});
      cues.push_back({kit::Button::kBack, "Tag / name", "Tab"});
      if (AccountOnline() && !(code_off && NpSameAsSaved()))
        cues.push_back({kit::Button::kX, np_protect_ ? "Protect: on" : "Protect: off", "F2"});
      else if (code_off && NpSameAsSaved())
        cues.push_back({kit::Button::kX, "Show code", "F2"});
      cues.push_back({kit::Button::kY, "Random name", "F5"});
      cues.push_back({kit::Button::kRB, "Recovery code", "F3"});
      cues.push_back({kit::Button::kStart, name_setup_ ? "Save and continue" : "Save", "Enter", true});
      if (!name_setup_ && !name_required_) cues.push_back({kit::Button::kB, "Back", "Esc"});
    }
    {  // the prompts fitted to the page's width: smaller lettering when they would run past it
      float size = 16;
      const float room = st(kit::WatermarkOn() ? 1232 - 48 - 215 : 1232 - 48);  // clear of the EARLY PLAYTEST stamp
      while (size > 11 && PromptsWidth(st, cues, size) > room) size -= 0.5f;
      Prompts(d, st, st.P(48, 652), cues, in, size);
    }
    if (in.accept || in.start || in.back || in.x || in.y || in.prev_tab || in.next_tab)
      REXLOG_INFO("BF name page: {} from the {}{}{}",
                  in.accept ? "A" : in.start ? "Start" : in.back ? "B" : in.x ? "X" : in.y ? "Y" : in.prev_tab ? "LB" : "RB",
                  enter_key ? "keyboard" : pad ? "controller" : "keyboard or mouse", name_keys_ ? " (keys open)" : "",
                  card ? " (code card)" : "");

    // ---- the paper
    const ImVec2 fa = st.P(48, 106), fb = st.P(770, 638);
    const ImVec2 ra = st.P(786, 106), rb = st.P(1232, kit::WatermarkOn() ? 616 : 638);  // clear of the EARLY PLAYTEST stamp
    kit::PaperArt(d, fa, fb);
    const float x = fa.x + st(28), w = fb.x - fa.x - st(56);
    float y = fa.y + st(20);
    int clicked_focus = -1;
    bool switch_tab = NpTabs(d, st, x, y, w) && !card && !name_keys_;
    y += st(40 + 18);
    if (name_setup_) {
      kit::Spaced(d, f.typed, st(10.5f), ImVec2(x, y), kit::kInkFaint,
                  code_mode_ ? "STEP 2 OF 8 \xC2\xB7 RETURNING SOLDIER" : "STEP 2 OF 8 \xC2\xB7 YOUR SOLDIER", st(2));
      y += st(18);
    }
    bool save_clicked = false, random_clicked = false, paste_clicked = false, switch_clicked = false;
    if (!code_mode_) {
      // -- New soldier
      const bool same = NpSameAsSaved();
      std::string chip = "NOT PROTECTED";
      ImU32 chip_fill = kit::kInk;
      if (code_off && same) chip = "PROTECTED", chip_fill = kit::kGreen;
      else if (acct_online_ == 3 && same) chip = "TEAM NAME", chip_fill = kit::kRed;
      else if (acct_online_ == 2 && same) chip = "ANOTHER PLAYER'S", chip_fill = kit::kRed;
      y += NpHeading(d, st, x, y, w, "YOUR SOLDIER NAME", chip, chip_fill,
                     signed_in_once_ && !name_setup_ ? "Shown on the scoreboard and in the server list, from your next start."
                                                     : "Shown on the scoreboard and in the server list.");
      y += st(6);
      // The clan tag box, then the soldier name box ([TAG]Name, as the game shows it).
      const float tw = st(160), gap = st(14), nx = x + tw + gap, nw = w - tw - gap;
      kit::Spaced(d, f.typed, st(10.5f), ImVec2(x, y), tag_mode_ ? kit::kInk : kit::kInkFaint, "CLAN TAG", st(2));
      kit::Spaced(d, f.typed, st(10.5f), ImVec2(nx, y), tag_mode_ ? kit::kInkFaint : kit::kInk, "SOLDIER NAME", st(2));
      y += st(18);
      const bool menu_click = BoxMenu(st);
      const ImVec2 t0(x, y), t1(x + tw, y + st(54)), n0(nx, y), n1(nx + nw, y + st(54));
      const bool show_focus = !card && !name_keys_;
      if (NpBox(d, st, t0, t1, tag_text_, tag_mode_ && np_focus_ <= kNpName && !card, show_focus && np_focus_ == kNpTag,
                std::to_string(tag_text_.size()) + "/" + std::to_string(text_filter::kTagMax)) && !menu_click && !card)
        clicked_focus = kNpTag;
      if (NpBox(d, st, n0, n1, name_text_, !tag_mode_ && np_focus_ <= kNpName && !card, show_focus && np_focus_ == kNpName,
                std::to_string(player_name::Chars(name_text_)) + " / " + std::to_string(kNameMax)) && !menu_click && !card)
        clicked_focus = kNpName;
      if (!card && !menu_click) {
        for (int b = 0; b < 2; ++b)
          if (NpBoxRightClicked(b ? n0 : t0, b ? n1 : t1)) {
            TagSwitch(b == 0), np_focus_ = b ? kNpName : kNpTag;
            box_menu_ = true, box_menu_at_ = ImGui::GetIO().MousePos, box_menu_direct_ = false;
            REXLOG_INFO("BF launcher: the name page's right click menu opened");
          }
      }
      y += st(54 + 12);
      {  // the name's line
        bool ok = true;
        const std::string line = NpNameLine(ok);
        const float ms = st(20), lx = x + ms + st(10), lw = w - ms - st(10), ls = st(16);
        const float lh = kit::WrappedHeight(f.ui, ls, line, lw);
        NpMark(d, ImVec2(x, y + std::min(lh, ls * 1.3f) * 0.5f), ms, ok);
        kit::Wrapped(d, f.ui, ls, ImVec2(lx, y), ok ? kit::kInkFaint : kit::kRed, line, lw);
        y += std::max(ms, lh) + st(14);
      }
      {  // PROTECT THIS NAME
        const bool online = AccountOnline();
        const bool prot_same = code_off && same;
        std::string title = "PROTECT THIS NAME", text;
        if (!online) {
          title = "PROTECTION IS OFF HERE";
          text = "Names are protected by the Reborn servers, which this game does not use. Your name still works here.";
        } else if (prot_same) {
          title = "THIS NAME IS PROTECTED";
          text = "Only you can use it online, and your rank and stats are kept on the Reborn servers. Your recovery code brings "
                 "them to another PC.";
        } else if (code_off || (!settings_.Get("account_code", "").empty() && !settings_.Get("account_name", "").empty())) {
          title = "MOVE MY PROTECTION HERE";
          text = "Your protected name is " + settings_.Get("account_name", "") +
                 ". With this on, Save moves the protection to this name; your recovery code stays the same.";
        } else {
          text = "Recommended. Save makes your recovery code: free, no email and no password.";
        }
        const float sw = st(48), sh = st(26), pad_x = st(16);
        const kit::Style style = PromptStyle();
        const float gw = prot_same ? 0 : kit::Glyph(nullptr, style, kit::Button::kX, "F2", ImVec2(0, 0), st(20), false);
        const float tx = x + pad_x + sw + st(16), tww = w - (tx - x) - pad_x - (gw > 0 ? gw + st(14) : 0);
        const float ts = NpFit(f.ui_bold, st(17), title, tww, st(1));
        const float bs = st(15.5f);
        const float th = ts * 1.3f + st(2) + kit::WrappedHeight(f.ui, bs, text, tww);
        const float ch = std::max(st(64), th + st(22));
        const ImVec2 c0(x, y), c1(x + w, y + ch);
        const bool hover = !card && online && !prot_same && ImGui::IsMouseHoveringRect(c0, c1);
        d->AddRectFilled(c0, c1, hover ? kit::kPaperDim : kit::kPaperLight);
        d->AddRect(c0, c1, show_focus && np_focus_ == kNpSwitch ? kit::kStamp : kit::kBorder, 0, 0,
                   st(show_focus && np_focus_ == kNpSwitch ? 2.5f : 1.5f));
        const ImVec2 s0(x + pad_x, y + ch * 0.5f - sh * 0.5f), s1(s0.x + sw, s0.y + sh);
        if (prot_same) {
          d->AddCircleFilled(ImVec2(s0.x + sw * 0.5f, y + ch * 0.5f), sh * 0.62f, kit::kGreen, 24);
          NpMark(d, ImVec2(s0.x + sw * 0.5f - sh * 0.5f, y + ch * 0.5f), sh, true);
        } else {
          const bool on = np_protect_ && online;
          d->AddRectFilled(s0, s1, on ? kit::kGreen : IM_COL32(120, 116, 100, online ? 150 : 70), sh * 0.5f);
          const float kx = on ? s1.x - sh * 0.5f : s0.x + sh * 0.5f;
          d->AddCircleFilled(ImVec2(kx, s0.y + sh * 0.5f), sh * 0.36f, IM_COL32(255, 255, 255, online ? 255 : 150), 20);
        }
        kit::Spaced(d, f.ui_bold, ts, ImVec2(tx, y + (ch - th) * 0.5f), online ? kit::kInk : kit::kInkFaint, title, st(1));
        kit::Wrapped(d, f.ui, bs, ImVec2(tx, y + (ch - th) * 0.5f + ts * 1.3f + st(2)), kit::kInkFaint, text, tww);
        if (gw > 0) kit::Glyph(d, style, kit::Button::kX, "F2", ImVec2(c1.x - pad_x - gw, y + ch * 0.5f), st(20));
        if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) switch_clicked = true;
        y += ch + st(18);
      }
      {  // Save and continue, Random name
        const float bh = st(44);
        const bool busy = acct_busy_.load();
        const std::string save = busy ? "PROTECTING..." : name_setup_ ? "SAVE AND CONTINUE" : "SAVE";
        const NpBtn s = NpButton(d, st, ImVec2(x, y), bh, kit::Button::kStart, {"Enter"}, save, 0, !busy && !card,
                                 show_focus && np_focus_ == kNpSave, w * 0.58f);
        const NpBtn r = NpButton(d, st, ImVec2(s.b.x + st(14), y), bh, kit::Button::kY, {"F5"}, "RANDOM NAME", 2, !busy && !card,
                                 show_focus && np_focus_ == kNpRandom, x + w - s.b.x - st(14));
        save_clicked = s.clicked, random_clicked = r.clicked;
        y += bh + st(16);
      }
    } else {
      // -- I have a recovery code
      y += NpHeading(d, st, x, y, w, "BRING YOUR SOLDIER BACK", "", 0,
                     code_off ? "This PC's name, " + settings_.Get("account_name", "") +
                                    ", is protected already. A recovery code is for bringing a protected name to a new PC."
                     : !AccountOnline() ? "A recovery code needs the Reborn servers, which this game does not use."
                                        : "Type or paste the code you got when you protected your name: your name, rank and awards "
                                          "come to this PC. Spaces, dashes and small letters are fine.");
      y += st(6);
      if (!code_off && AccountOnline()) {
        const std::string count = "RECOVERY CODE \xC2\xB7 " + std::to_string(name_text_.size()) + " / " + std::to_string(account::kCodeLength);
        kit::Spaced(d, f.typed, st(10.5f), ImVec2(x, y), kit::kInk, count, st(2));
        y += st(18);
        const float gap = st(10), bw = (w - gap * 3) / 4;
        const int cur = std::min(3, int(name_text_.size()) / 5);
        float gsize = st(26);
        gsize = NpFit(f.typed, gsize, "WWWWW", bw - st(24));
        for (int i = 0; i < 4; ++i) {
          const ImVec2 a(x + float(i) * (bw + gap), y), b(a.x + bw, y + st(54));
          const std::string part = i * 5 < int(name_text_.size()) ? name_text_.substr(size_t(i) * 5, 5) : std::string();
          if (NpBox(d, st, a, b, part, i == cur && !card, show_focus_code() && i == cur, "", true, gsize, i == cur) && !card)
            clicked_focus = kNpName;
        }
        y += st(54 + 12);
        {
          const bool bad = !name_error_.empty() || (acct_message_bad_ && !acct_message_.empty());
          const std::string line = !name_error_.empty() ? name_error_
                                   : bad               ? acct_message_
                                                       : "Twenty letters and numbers, in four groups of five.";
          const float ms = st(20), lx = x + ms + st(10), lw = w - ms - st(10), ls = st(16);
          const float lh = kit::WrappedHeight(f.ui, ls, line, lw);
          NpMark(d, ImVec2(x, y + std::min(lh, ls * 1.3f) * 0.5f), ms, !bad);
          kit::Wrapped(d, f.ui, ls, ImVec2(lx, y), bad ? kit::kRed : kit::kInkFaint, line, lw);
          y += std::max(ms, lh) + st(16);
        }
        const float bh = st(44);
        const bool busy = acct_busy_.load(), full = name_text_.size() == size_t(account::kCodeLength);
        const NpBtn s = NpButton(d, st, ImVec2(x, y), bh, kit::Button::kStart, {"Enter"}, busy ? "LOOKING IT UP..." : "BRING IT BACK", 0,
                                 full && !busy, show_focus_code() && np_focus_ == kNpSave, w * 0.58f);
        const NpBtn p2 = NpButton(d, st, ImVec2(s.b.x + st(14), y), bh, kit::Button::kY, {"Ctrl", "V"}, "PASTE", 2, !busy, false,
                                  x + w - s.b.x - st(14));
        save_clicked = s.clicked, paste_clicked = p2.clicked;
        y += bh + st(16);
      }
    }
    // The page's message (a protection or a recovery on its way, done, or refused), under the buttons.
    if (!acct_message_.empty() && !(code_mode_ && acct_message_bad_)) {
      const float ms = st(16);
      kit::Wrapped(d, f.ui, ms, ImVec2(x, y), acct_message_bad_ ? kit::kRed : kit::kGreenDark, acct_message_, w);
    }

    // ---- the rail
    d->AddRectFilled(ra, rb, kNpRail);
    d->AddRect(ra, rb, card ? kit::kGreen : kNpRailEdge, 0, 0, card ? st(2) : 1.0f);
    const float rx = ra.x + st(22), rw = rb.x - ra.x - st(44), ry = ra.y + st(20), rh = rb.y - ra.y - st(40);
    int card_act = 0;
    int key_clicked = -1;
    if (card) {
      float k = 1;
      for (int i = 0; i < 12; ++i, k *= 0.95f) {
        int dummy = 0;
        if (NpCodeCard(d, st, rx, ry, rw, k, false, dummy) <= rh) break;
      }
      NpCodeCard(d, st, rx, ry, rw, k, true, card_act);
    } else if (name_keys_) {
      key_clicked = OnScreenKeys(d, st, ImVec2(rx, ry), in, name_key_, name_keys_, name_caps_, false, nullptr, !code_mode_);
      const float ky = ry + st(26) + 5 * (st(40) + st(4.5f)) + st(8);
      const std::string hint = "A types, X deletes, B closes the keys, Start saves. With a keyboard, just type.";
      const float hh = kit::Wrapped(d, f.ui, st(14.5f), ImVec2(rx, ky), kit::kKhakiText, hint, rw);
      const float by = ky + hh + st(16);
      float k = 1;
      for (int i = 0; i < 12; ++i, k *= 0.93f)
        if (NpBriefing(d, st, rx, by, rw, k, true, false) <= rb.y - st(16) - by) break;
      NpBriefing(d, st, rx, by, rw, k, true, true);
    } else {
      float k = 1;
      for (int i = 0; i < 12; ++i, k *= 0.95f)
        if (NpBriefing(d, st, rx, ry, rw, k, false, false) <= rh) break;
      NpBriefing(d, st, rx, ry, rw, k, false, true);
    }
    StampCorner(d, st);

    // ---- what the presses do
    if (card) {
      if (in.y || ctrl_c || ImGui::IsKeyPressed(ImGuiKey_C, false)) card_act = 1;
      if (in.x || ImGui::IsKeyPressed(ImGuiKey_O, false) || f4) card_act = 2;
      if (np_card_ == 1 && ((in.accept && !enter_key) || ImGui::IsKeyPressed(ImGuiKey_Space, false))) card_act = 3;
      if (in.start || enter_key) card_act = 4;
      if (np_card_ == 2 && in.back) card_act = 4;
      if (card_act) NpCardAct(card_act);
      return;
    }
    if (key_clicked >= 0) {  // the on screen keys
      const std::vector<OnKey>& keys = OnKeys(false, !code_mode_);
      const OnKey& key = keys[size_t(key_clicked)];
      REXLOG_INFO("BF launcher: on screen key {}", key.label);
      if (key.kind == 0) NameType(key.ch);
      else if (key.kind == 1) NameDelete();
      else if (key.kind == 2) name_caps_ = !name_caps_;
      else if (key.kind == 4) NameTypeTm();
      else name_keys_ = false;  // DONE: the keys close (Start saves)
    }
    if (name_keys_) {
      if (in.x) NameDelete();
      if (in.back) {
        name_keys_ = false;
        return;
      }
      if (in.start) NpSave();
      return;
    }
    if (clicked_focus >= 0) {
      np_focus_ = clicked_focus;
      if (!code_mode_) TagSwitch(clicked_focus == kNpTag);
    }
    // Tabs: LB / RB, F3, a click.
    if ((in.prev_tab && code_mode_) || (in.next_tab && !code_mode_) || f3) switch_tab = true;
    if (switch_tab && !acct_busy_.load()) {
      NpCodeTab(!code_mode_);
      if (code_mode_) TagSwitch(false);
      np_focus_ = kNpName;
      REXLOG_INFO("BF name page: the {} tab", code_mode_ ? "recovery code" : "new soldier");
      return;
    }
    if (code_mode_) {
      // The recovery tab: the code box, then Bring it back and Paste.
      if (in.up || in.down) np_focus_ = np_focus_ == kNpName ? kNpSave : kNpName;
      if (pad && in.accept && np_focus_ == kNpName && !code_off && AccountOnline()) name_keys_ = true, name_key_ = 10;
      else if ((pad && in.accept && np_focus_ == kNpSave) || in.start || enter_key || save_clicked) {
        if (!code_off && AccountOnline()) NpSave();
      }
      if ((in.y || paste_clicked) && !code_off) NamePaste(paste::Text(), "button");
      if (in.back) NpCodeTab(false);
      return;
    }
    // The New soldier tab: the boxes (row 0), the switch (row 1), the buttons (row 2).
    if (in.up || in.down) {
      const int row = np_focus_ <= kNpName ? 0 : np_focus_ == kNpSwitch ? 1 : 2;
      const int to = std::clamp(row + (in.down ? 1 : -1), 0, 2);
      np_focus_ = to == 0 ? (tag_mode_ ? kNpTag : kNpName) : to == 1 ? kNpSwitch : kNpSave;
    }
    if (in.left || in.right) {
      if (np_focus_ <= kNpName) np_focus_ = np_focus_ == kNpTag ? kNpName : kNpTag, TagSwitch(np_focus_ == kNpTag);
      else if (np_focus_ >= kNpSave) np_focus_ = np_focus_ == kNpSave ? kNpRandom : kNpSave;
    }
    if (tab_key || f4) {  // Tab (Shift+Tab), the View button, F4: the other box
      TagSwitch(!tag_mode_);
      np_focus_ = tag_mode_ ? kNpTag : kNpName;
      (void)shift;
    }
    const bool toggle = in.x || f2 || switch_clicked || (in.accept && np_focus_ == kNpSwitch);
    if (toggle) {
      if (NameProtected() && NpSameAsSaved()) {
        np_card_ = 2, np_ticked_ = true;
        REXLOG_INFO("BF name page: the recovery code shown again");
        return;
      }
      if (AccountOnline()) {
        np_protect_ = !np_protect_;
        REXLOG_INFO("BF name page: Protect this name {}", np_protect_ ? "on" : "off");
      }
    }
    if (in.y || f5 || random_clicked || (in.accept && np_focus_ == kNpRandom)) {
      if (tag_mode_) TagSwitch(false);
      NameRandom();
      np_focus_ = kNpName;
    }
    if (pad && in.accept && !enter_key && np_focus_ <= kNpName) {
      name_keys_ = true;  // A on a box: the on screen keys for it
      return;
    }
    // Enter with no name yet (too short to save) opens the keys for a controller, as A does: Steam's Enter for a
    // controller's A can come just before the A itself.
    if (enter_key && pad && name_text_.size() < 3 && np_focus_ <= kNpName) {
      name_keys_ = true;
      return;
    }
    if (in.start || save_clicked || (enter_key && np_focus_ != kNpSwitch && np_focus_ != kNpRandom) ||
        (in.accept && !enter_key && np_focus_ == kNpSave)) {
      NpSave();
      return;
    }
    if (in.back && !name_setup_ && !name_required_) page_ = name_from_;
#if defined(BF_DEV_TOOLS)
    // BF_DEV_NAME=<name> (tests): typed for the player 5 s after the page first shows, then saved.
    static double dev_at = -1;
    if (dev_at < 0) dev_at = ImGui::GetTime();
    if (const char* v = std::getenv("BF_DEV_NAME"); v && *v && ImGui::GetTime() - dev_at > 5.0) {
      dev_at = 1e30;
      name_text_ = player_name::Clean(v);
      if (const char* t = std::getenv("BF_DEV_TAG")) {  // BF_DEV_TAG=<tag>: the clan tag box too ("-": emptied)
        tag_text_.clear();
        for (const char* c = t; *c && *c != '-'; ++c) tag_text_ += *c;
      }
      NpSave();
    }
#endif
  }
  bool show_focus_code() const { return np_card_ == 0 && !name_keys_ && np_focus_ == kNpName; }
