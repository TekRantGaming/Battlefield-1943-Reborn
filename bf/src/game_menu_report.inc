// Report an issue, the form that files on GitHub (TekRantGaming, 16 Nov 2026: "it needs to have git hub integration
// for feedback with template to automatically submit data give user text boxes n stuff you can expand upon this").
// Included inside GameMenu (game_menu.cpp) after game_menu_home.inc. Three steps on one paper sheet, the mockups'
// R1 to R3 (runs\ui-designs\build_report.py): 1 the report (the type, the title, what happened, the steps, expected
// and actual, how often, a contact; the on screen keyboard at the right for a controller), 2 what gets sent (each
// attached part with its tick and the issue exactly as GitHub shows it), 3 send (the relay, GitHub in the browser, or
// copy). What is sent, the scrubbing and the routes are report.cpp's; docs/REPORTS.md has the design.

  // ---- the state
  enum RpField { kRpType, kRpTitle, kRpWhat, kRpSteps, kRpExpected, kRpActual, kRpOften, kRpContact };
  bool rp_active_ = false;    // the page's state is set up (a fresh visit loads the draft)
  int rp_step_ = 0;           // 0 the report, 1 what gets sent, 2 send
  report::Draft rp_;
  int rp_field_ = kRpType;    // the field the cursor is on (step 1)
  bool rp_keys_ = false;      // the on screen keys are open
  int rp_key_ = 13;           // the key the cursor is on
  bool rp_shift_ = false;     // the next letter a capital (one press), set by itself at a sentence's start
  bool rp_want_shot_ = true;  // Y on step 1: the screenshot attached or not
  std::string rp_error_;      // what is missing (step 1's foot), until the next change
  bool rp_dirty_ = false;
  double rp_saved_at_ = 0;
  // Step 2: the parts, gathered on a thread (the log's scrubbing takes a moment), the player's ticks over them.
  struct RpGather {
    std::atomic<bool> done{false};
    std::vector<report::Part> parts;
    report::Draft scrubbed;  // what the player typed, scrubbed too
  };
  std::shared_ptr<RpGather> rp_gather_;
  std::vector<report::Part> rp_parts_;
  report::Draft rp_sent_draft_;  // the scrubbed draft the parts were gathered with
  std::map<std::string, bool> rp_ticks_;  // the player's ticks by part id
  int rp_row_ = 0;           // step 2: the part, or rp_parts_.size() for the preview
  float rp_scroll_ = 0;      // the preview's scroll (stage units)
  struct RpLine {
    int style;               // 0 text, 1 heading, 2 table row, 3 folded summary, 4 code, 5 more, 6 rule, 7 foot, 8 picture
    std::string a, b;
  };
  std::vector<RpLine> rp_lines_;
  int rp_lines_version_ = -1, rp_version_ = 0;
  float rp_lines_width_ = 0;
  // Step 3.
  int rp_send_row_ = 0;
  std::shared_ptr<report::Send> rp_job_;
  std::string rp_note_;      // a line under the buttons (copied, the link written...)
  double rp_note_at_ = -100;
  double rp_retry_at_ = 0;   // the kept reports' next try (Home's row)

  // ---- small helpers
  static void RpPopUtf8(std::string& s) {
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
    if (!s.empty()) s.pop_back();
  }
  static void RpAppendUtf8(std::string& s, unsigned c) {
    if (c < 0x80) s += char(c);
    else if (c < 0x800) s += char(0xC0 | (c >> 6)), s += char(0x80 | (c & 0x3F));
    else s += char(0xE0 | (c >> 12)), s += char(0x80 | ((c >> 6) & 0x3F)), s += char(0x80 | (c & 0x3F));
  }
  // `t` in lines no wider than `w` (words kept whole unless one alone is wider), its own new lines kept.
  static std::vector<std::string> RpWrap(ImFont* f, float size, const std::string& t, float w) {
    std::vector<std::string> out;
    auto width = [&](const std::string& s) { return f->CalcTextSizeA(size, FLT_MAX, 0, s.c_str()).x; };
    size_t at = 0;
    for (;;) {
      const size_t nl = t.find('\n', at);
      const std::string para = t.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
      std::string line;
      size_t i = 0;
      while (i < para.size()) {
        size_t j = para.find(' ', i);
        const std::string word = para.substr(i, j == std::string::npos ? std::string::npos : j - i + 1);
        if (width(line + word) <= w || line.empty()) {
          line += word;
          // A word wider than the line alone: cut where it fits.
          while (width(line) > w && line.size() > 1) {
            size_t cut = line.size() - 1;
            while (cut > 1 && width(line.substr(0, cut)) > w) --cut;
            while (cut > 1 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
            out.push_back(line.substr(0, cut));
            line = line.substr(cut);
          }
        } else {
          out.push_back(line);
          line = word;
        }
        if (j == std::string::npos) break;
        i = j + 1;
      }
      out.push_back(line);
      if (nl == std::string::npos) break;
      at = nl + 1;
    }
    return out;
  }
  std::vector<int> RpFields() const {
    if (rp_.type == report::kSuggestion) return {kRpType, kRpTitle, kRpWhat, kRpSteps, kRpContact};
    return {kRpType, kRpTitle, kRpWhat, kRpSteps, kRpExpected, kRpActual, kRpOften, kRpContact};
  }
  std::string* RpText(int field) {
    switch (field) {
      case kRpTitle: return &rp_.title;
      case kRpWhat: return &rp_.what;
      case kRpSteps: return &rp_.steps;
      case kRpExpected: return &rp_.expected;
      case kRpActual: return &rp_.actual;
      case kRpContact: return &rp_.contact;
      default: return nullptr;
    }
  }
  static size_t RpMax(int field) {
    switch (field) {
      case kRpTitle: return report::kTitleMax;
      case kRpWhat: return report::kWhatMax;
      case kRpSteps: return report::kLongMax;
      case kRpContact: return report::kContactMax;
      default: return report::kShortMax;
    }
  }
  static bool RpMulti(int field) { return field == kRpWhat || field == kRpSteps; }
  // While the keyboard types into a field of the form (game_menu.cpp: its letters are then not the launcher's keys).
  bool ReportTyping() const {
    return page_ == Page::kReport && rp_step_ == 0 && rp_field_ != kRpType && rp_field_ != kRpOften;
  }
  void RpChanged() { rp_dirty_ = true, rp_error_.clear(), ++rp_version_; }
  void RpType(unsigned c) {
    std::string* t = RpText(rp_field_);
    if (!t || t->size() >= RpMax(rp_field_)) return;
    if (c == '\n' && !RpMulti(rp_field_)) return;
    if (rp_shift_ && c < 128 && std::isalpha(int(c))) c = unsigned(std::toupper(int(c)));
    rp_shift_ = false;
    RpAppendUtf8(*t, c);
    // A capital next at a sentence's start (the on screen keys).
    rp_shift_ = t->size() >= 2 && (t->back() == ' ' && ((*t)[t->size() - 2] == '.' || (*t)[t->size() - 2] == '?' || (*t)[t->size() - 2] == '!'));
    rp_shift_ = rp_shift_ || t->back() == '\n';
    RpChanged();
  }
  void RpDelete() {
    if (std::string* t = RpText(rp_field_); t && !t->empty()) RpPopUtf8(*t), RpChanged();
  }
  void RpFocus(int field) {
    rp_field_ = field;
    const std::string* t = RpText(field);
    rp_shift_ = t && (t->empty() || t->back() == '\n');
  }

  // A fresh visit: the draft back, step 1. From Home and the main menu (and the old launcher's row).
  void ReportBegin() {
    rp_active_ = true, rp_step_ = 0, rp_keys_ = false, rp_error_.clear(), rp_job_.reset(), rp_note_.clear();
    rp_ = report::Draft{};
    const bool had = report::LoadDraft(UserDir(), rp_);
    RpFocus(had ? kRpWhat : kRpType);
    if (!had) rp_field_ = kRpType;
    rp_ticks_.clear(), rp_want_shot_ = true, ++rp_version_;
    REXLOG_INFO("BF report: the page opened ({}; relay {}; {})", had ? "a draft back" : "a new report",
                report::Endpoint().empty() ? "none, GitHub in the browser" : report::Endpoint(),
                report::DryRun() ? "DRY RUN" : "live");
#if defined(BF_DEV_TOOLS)
    // BF_DEV_REPORT_SELFTEST=1: the scrubbing's cases, in the log.
    if (const char* v = std::getenv("BF_DEV_REPORT_SELFTEST"); v && *v == '1') {
      std::string failures;
      if (report::ScrubSelfTest(failures)) REXLOG_INFO("BF report: scrub self test passed");
      else REXLOG_ERROR("BF report: scrub self test FAILED:\n{}", failures);
    }
    // BF_DEV_REPORT_FILL=1 (tests): a made up report typed for the player, a crash.
    if (const char* v = std::getenv("BF_DEV_REPORT_FILL"); v && *v == '1') {
      rp_.type = report::kCrash;
      rp_.title = "Game closes when a Corsair lands on the carrier";
      rp_.what = "I was landing the Corsair on the US carrier on Wake Island. The moment the wheels touched the deck the "
                 "screen froze for a second, then the game closed with no message. My files are in "
                 "C:\\Users\\" + std::string(std::getenv("USERNAME") ? std::getenv("USERNAME") : "someone") +
                 "\\AppData and the server was 203.0.113.24:18300.";
      rp_.steps = "1. Join All Maps 2 on Wake Island as US\n2. Take a Corsair from the carrier and land back on it";
      rp_.expected = "The plane lands and stops";
      rp_.often = 1;
      rp_.contact = "TestSoldier";
      RpChanged();
    }
#endif
  }
  void ReportLeave() {
    if (rp_dirty_) report::SaveDraft(UserDir(), rp_), rp_dirty_ = false;
    rp_active_ = false, rp_keys_ = false;
    RpBackHome();
  }
  // Home (MenuPage makes it the main menu itself once the game's menu is up), as the Projects page goes back. Not
  // BackToMenu: with the game started behind Home for its shaders (pre_game_ false, its menu not up yet) that went to
  // a main menu that never drew (run rep_relay1).
  void RpBackHome() {
    page_ = Page::kHome, home_row_ = kRowReport, row_ = kRowReport;
  }

  // Home's row: the kept reports' count, and their next try every 10 minutes (report.cpp RetryPending).
  int rp_pending_ = 0;
  double rp_counted_at_ = -100;
  std::string ReportRowNote() {
    const double now = ImGui::GetTime();
    if (now >= rp_retry_at_) {
      rp_retry_at_ = now + 600;
      report::RetryPending(UserDir());
    }
    if (now - rp_counted_at_ > 3) rp_counted_at_ = now, rp_pending_ = report::PendingCount(UserDir());
    return rp_pending_ > 0 ? fmt::format("{} WAITING", rp_pending_) : std::string();
  }

  // F12 with the game running: the window saved for a report (report.cpp TakeScreenshot), unless F12 is one of the
  // player's own bindings. Said in the corner for two seconds.
  double rp_shot_at_ = -100;
  bool rp_shot_ok_ = false;
  void ReportShotTick(ImDrawList* d, const ImGuiIO& io) {
    if (!pre_game_ && ImGui::IsKeyPressed(ImGuiKey_F12, false) && !comms::TypingOrJustClosed()) {
      bool bound = false;
      for (const auto& [k, v] : settings_.values)
        bound = bound || (k.rfind("cvar.keybind_", 0) == 0 && v.find("F12") != std::string::npos);
      if (!bound) rp_shot_ok_ = report::TakeScreenshot(UserDir()), rp_shot_at_ = ImGui::GetTime();
    }
    if (ImGui::GetTime() - rp_shot_at_ < 2.0 && d) {
      const kit::Fonts& f = kit::GetFonts();
      const std::string t = rp_shot_ok_ ? "SCREENSHOT SAVED FOR A REPORT (F12)" : "COULD NOT TAKE A SCREENSHOT";
      const float s = io.DisplaySize.y / 720.0f;
      const float w = kit::SpacedWidth(f.typed, 11 * s, t, 2 * s);
      const ImVec2 a(io.DisplaySize.x - w - 40 * s, 20 * s), b(io.DisplaySize.x - 16 * s, 44 * s);
      d->AddRectFilled(a, b, IM_COL32(20, 20, 18, 220));
      d->AddRectFilled(a, ImVec2(a.x + 4 * s, b.y), rp_shot_ok_ ? kit::kGreen : kit::kStamp);
      kit::Spaced(d, f.typed, 11 * s, ImVec2(a.x + 14 * s, a.y + 6 * s), kit::kStripText, t, 2 * s);
    }
  }

  // ---- drawing pieces
  // The sheet and its bar: the heading, what it does, the three steps (a click goes to one).
  void RpSheet(ImDrawList* d, const Stage& st) {
    const kit::Fonts& f = kit::GetFonts();
    const ImVec2 a = st.P(40, 22), b = st.P(1240, 640);
    kit::PaperArt(d, a, b);
    d->AddRect(a, b, kit::kBorder, 0, 0, 1.0f);
    const ImVec2 b0 = a, b1(b.x, a.y + st(54));
    d->AddRectFilled(b0, b1, kit::kStrip);
    d->AddRectFilled(ImVec2(a.x, b1.y), ImVec2(b.x, b1.y + st(4)), kit::kStamp);
    kit::GameText(d, kit::GameFont::kHeadlineMedium, st(26), ImVec2(a.x + st(20), a.y + st(13)), kit::kCream, "REPORT AN ISSUE");
    const float hw = kit::GameTextWidth(kit::GameFont::kHeadlineMedium, st(26), "REPORT AN ISSUE");
    const std::string what = report::DryRun() ? "DRY RUN \xC2\xB7 NOTHING LEAVES THIS PC"
                             : report::Endpoint().empty() ? "FILES ON GITHUB" : "FILES ON GITHUB \xC2\xB7 NO ACCOUNT NEEDED";
    kit::Spaced(d, f.typed, st(9.5f), ImVec2(a.x + st(36) + hw, a.y + st(22)), report::DryRun() ? kit::kStamp : kit::kGold, what, st(2));
    static const char* kSteps[] = {"1  YOUR REPORT", "2  WHAT GETS SENT", "3  SEND"};
    float x = b.x - st(14);
    for (int i = 2; i >= 0; --i) {
      const float w = kit::SpacedWidth(f.ui_bold, st(11), kSteps[i], st(1.5f)) + st(20);
      const ImVec2 s0(x - w, a.y + st(14)), s1(x, a.y + st(40));
      d->AddRectFilled(s0, s1, i == rp_step_ ? kit::kGreen : IM_COL32(34, 35, 29, 255));
      kit::Spaced(d, f.ui_bold, st(11), ImVec2(s0.x + st(10), s0.y + st(5)), i == rp_step_ ? kit::kStripText : kit::kKhakiDim,
                  kSteps[i], st(1.5f));
      if (ImGui::IsMouseHoveringRect(s0, s1) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) RpGoStep(i);
      x = s0.x - st(5);
    }
  }
  // A field's label: "TITLE *" or "STEPS TO REPRODUCE · OPTIONAL".
  void RpLabel(ImDrawList* d, const Stage& st, ImVec2 at, const std::string& name, bool required, const std::string& note = "") {
    const kit::Fonts& f = kit::GetFonts();
    kit::Spaced(d, f.typed, st(9), at, kit::kInkSection, name, st(2));
    float x = at.x + kit::SpacedWidth(f.typed, st(9), name, st(2)) + st(6);
    if (required) kit::Spaced(d, f.typed, st(9), ImVec2(x, at.y), kit::kStamp, "*", 0), x += st(12);
    if (!note.empty()) kit::Spaced(d, f.typed, st(8.5f), ImVec2(x, at.y + st(0.5f)), kit::kKhakiDim, "\xC2\xB7 " + note, st(1));
  }
  // A row of chips (the type, how often); the chosen one green, the cursor's row outlined. Returns a clicked chip.
  int RpChips(ImDrawList* d, const Stage& st, ImVec2 at, const std::vector<std::string>& names, int chosen, bool focused) {
    const kit::Fonts& f = kit::GetFonts();
    int clicked = -1;
    float x = at.x;
    const float h = st(26);
    for (int i = 0; i < int(names.size()); ++i) {
      const std::string t = Upper(names[i]);
      const float w = kit::SpacedWidth(f.ui_bold, st(10.5f), t, st(1)) + st(20);
      const ImVec2 c0(x, at.y), c1(x + w, at.y + h);
      const bool on = i == chosen;
      d->AddRectFilled(c0, c1, on ? kit::kGreen : kit::kPaperLight);
      d->AddRect(c0, c1, on ? kit::kGreenShadow : kit::kBorder, 0, 0, 1.0f);
      kit::Spaced(d, f.ui_bold, st(10.5f), ImVec2(c0.x + st(10), c0.y + st(6)), on ? kit::kStripText : kit::kInkFaint, t, st(1));
      if (ImGui::IsMouseHoveringRect(c0, c1) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = i;
      x = c1.x + st(6);
    }
    if (focused) d->AddRect(ImVec2(at.x - st(4), at.y - st(4)), ImVec2(x - st(2), at.y + h + st(4)), kit::kStamp, 0, 0, st(2));
    return clicked;
  }
  // A text field: its box, its text wrapped (the end shown while it is typed into), the caret, the count. True when
  // clicked.
  bool RpBox(ImDrawList* d, const Stage& st, ImVec2 a, ImVec2 b, const std::string& text, const std::string& hint, size_t max,
             bool focused, bool multi) {
    const kit::Fonts& f = kit::GetFonts();
    d->AddRectFilled(a, b, kit::kPaperLight);
    if (focused) {
      d->AddRect(a, b, kit::kGreen, 0, 0, st(2.5f));
      d->AddRect(ImVec2(a.x - st(3), a.y - st(3)), ImVec2(b.x + st(3), b.y + st(3)), IM_COL32(93, 138, 50, 60), 0, 0, st(3));
    } else {
      d->AddRect(a, b, kit::kBorder, 0, 0, 1.0f);
    }
    const float size = st(13.5f), line_h = size * 1.32f, pad = st(9);
    const float w = b.x - a.x - pad * 2 - st(52);
    std::vector<std::string> lines = RpWrap(f.ui, size, text, w);
    const int fit = std::max(1, int((b.y - a.y - pad * 1.2f) / line_h));
    int first = 0;
    if (int(lines.size()) > fit) first = focused ? int(lines.size()) - fit : 0;
    d->PushClipRect(a, b, true);
    float y = multi ? a.y + pad * 0.7f : a.y + (b.y - a.y - line_h) * 0.5f;
    if (text.empty() && !hint.empty()) d->AddText(f.ui, size, ImVec2(a.x + pad, y), IM_COL32(154, 146, 122, 255), hint.c_str());
    for (int i = first; i < int(lines.size()) && i < first + fit; ++i, y += line_h)
      d->AddText(f.ui, size, ImVec2(a.x + pad, y), kit::kInk, lines[i].c_str());
    if (focused && std::fmod(ImGui::GetTime(), 1.0) < 0.6) {
      const std::string& last = lines.empty() ? std::string() : lines.back();
      const float cy = (text.empty() ? (multi ? a.y + pad * 0.7f : a.y + (b.y - a.y - line_h) * 0.5f) : y - line_h);
      const float cx = a.x + pad + kit::TextWidth(f.ui, size, last) + st(1);
      d->AddRectFilled(ImVec2(cx, cy + st(1)), ImVec2(cx + st(2), cy + line_h - st(1)), kit::kInk);
    }
    if (!focused && int(lines.size()) > fit && !text.empty())
      kit::Spaced(d, f.typed, st(8), ImVec2(b.x - st(60), b.y - st(30)), kit::kInkSection, "MORE...", st(1));
    d->PopClipRect();
    const std::string count = fmt::format("{} / {}", text.size(), max);
    kit::Spaced(d, f.typed, st(7.5f), ImVec2(b.x - st(8) - kit::SpacedWidth(f.typed, st(7.5f), count, st(1)), b.y - st(13)),
                text.size() >= max ? kit::kStamp : kit::kInkSection, count, st(1));
    return ImGui::IsMouseHoveringRect(a, b) && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  }

  // The on screen keys: 11 columns, five rows. kind 0 a character, 1 shift, 2 space, 3 delete, 4 new line, 5 done.
  struct RpKey {
    const char* label;
    char ch, shifted;
    int kind, row, col, span;
  };
  static const std::vector<RpKey>& RpKeys() {
    static const std::vector<RpKey> keys = [] {
      std::vector<RpKey> v;
      static const char* base[] = {"1234567890-", "qwertyuiop?", "asdfghjkl'!", "zxcvbnm,."};
      static const char* shifted[] = {"!@#%&*()/:+", "QWERTYUIOP\"", "ASDFGHJKL;=", "ZXCVBNM_#"};
      static char labels[64][2] = {};
      int n = 0;
      for (int r = 0; r < 4; ++r)
        for (int c = 0; base[r][c]; ++c, ++n) {
          labels[n][0] = base[r][c];
          v.push_back({labels[n], base[r][c], shifted[r][c], 0, r, r == 3 ? c + 2 : c, 1});
        }
      v.push_back({"SHIFT", 0, 0, 1, 3, 0, 2});
      v.push_back({"NEW LINE", 0, 0, 4, 4, 0, 3});
      v.push_back({"SPACE", ' ', ' ', 2, 4, 3, 4});
      v.push_back({"DEL", 0, 0, 3, 4, 7, 2});
      v.push_back({"DONE", 0, 0, 5, 4, 9, 2});
      return v;
    }();
    return keys;
  }
  // Draws the keys at a..(a.x + w); returns a clicked key or -1.
  int RpKeyboard(ImDrawList* d, const Stage& st, ImVec2 a, float w) {
    const kit::Fonts& f = kit::GetFonts();
    const auto& keys = RpKeys();
    const float gap = st(4), kh = st(30), cw = (w - st(20) - gap * 10) / 11;
    const ImVec2 b(a.x + w, a.y + st(34) + 5 * (kh + gap) + st(6));
    d->AddRectFilled(a, b, IM_COL32(29, 31, 25, 255));
    kit::Spaced(d, f.typed, st(8.5f), ImVec2(a.x + st(10), a.y + st(10)), kit::kKhakiDim, "ON SCREEN KEYBOARD", st(2));
    if (device_ != Device::kKeyboard) {  // the controller's buttons on the keys (a keyboard just types)
      const kit::Style style = PromptStyle();
      float x = b.x - st(10);
      const std::pair<kit::Button, const char*> hints[] = {{kit::Button::kX, "DELETE"}, {kit::Button::kY, "SPACE"}, {kit::Button::kLB, "SHIFT"}};
      for (const auto& [btn, label] : hints) {
        const float tw = kit::SpacedWidth(f.typed, st(8), label, st(1.5f));
        x -= tw;
        kit::Spaced(d, f.typed, st(8), ImVec2(x, a.y + st(11)), kit::kKhakiDim, label, st(1.5f));
        x -= st(4);
        x -= kit::Glyph(d, style, btn, KeyFor(btn), ImVec2(0, 0), st(15), false);
        kit::Glyph(d, style, btn, KeyFor(btn), ImVec2(x, a.y + st(15)), st(15));
        x -= st(12);
      }
    }
    int clicked = -1;
    for (int k = 0; k < int(keys.size()); ++k) {
      const RpKey& key = keys[k];
      const ImVec2 k0(a.x + st(10) + float(key.col) * (cw + gap), a.y + st(34) + float(key.row) * (kh + gap));
      const ImVec2 k1(k0.x + float(key.span) * cw + float(key.span - 1) * gap, k0.y + kh);
      const bool sel = rp_keys_ && k == rp_key_;
      const bool hover = ImGui::IsMouseHoveringRect(k0, k1);
      const ImU32 fill = sel ? kit::kGreen : key.kind == 5 ? kit::kStamp : key.kind != 0 && key.kind != 2 ? kit::kRule
                         : hover ? kit::kPaperLight : kit::kPaperCard;
      d->AddRectFilled(k0, k1, rp_keys_ ? fill : (fill & 0x00FFFFFF) | 0xB0000000);
      if (key.kind == 1 && rp_shift_) d->AddRectFilled(ImVec2(k0.x, k1.y - st(3)), k1, sel ? kit::kCream : kit::kGreen);
      std::string label = key.kind == 0 ? std::string(1, rp_shift_ ? key.shifted : key.ch) : key.label;
      const bool white = sel || key.kind == 5;
      ImFont* font = key.kind == 0 ? f.ui : f.ui_bold;
      const float size = key.kind == 0 ? st(15) : st(10.5f);
      d->AddText(font, size, ImVec2((k0.x + k1.x - kit::TextWidth(font, size, label)) * 0.5f, k0.y + (kh - size * 1.4f) * 0.5f),
                 white ? IM_COL32(255, 255, 255, 255) : kit::kInk, label.c_str());
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = k, rp_key_ = k;
    }
    return clicked;
  }
  void RpPressKey(int k) {
    const RpKey& key = RpKeys()[size_t(k)];
    switch (key.kind) {
      case 0: RpType(unsigned(static_cast<unsigned char>(rp_shift_ ? key.shifted : key.ch))); break;
      case 1: rp_shift_ = !rp_shift_; break;
      case 2: RpType(' '); break;
      case 3: RpDelete(); break;
      case 4: if (RpMulti(rp_field_)) RpType('\n'); else RpNextField(1); break;
      default: rp_keys_ = false; break;
    }
  }
  void RpMoveKey(const Input& in) {
    const auto& keys = RpKeys();
    const RpKey& now = keys[size_t(rp_key_)];
    if (in.left || in.right) {
      std::vector<int> row;
      for (int k = 0; k < int(keys.size()); ++k)
        if (keys[k].row == now.row) row.push_back(k);
      std::sort(row.begin(), row.end(), [&](int x, int y) { return keys[x].col < keys[y].col; });
      const int at = int(std::find(row.begin(), row.end(), rp_key_) - row.begin()), n = int(row.size());
      rp_key_ = row[size_t((at + (in.right ? 1 : n - 1)) % n)];
    }
    if (in.up || in.down) {
      const int to = (now.row + (in.down ? 1 : 4)) % 5;
      const float mid = float(now.col) + float(now.span) * 0.5f;
      int best = rp_key_;
      float best_d = 1e9f;
      for (int k = 0; k < int(keys.size()); ++k) {
        if (keys[k].row != to) continue;
        const float dist = std::fabs(float(keys[k].col) + float(keys[k].span) * 0.5f - mid);
        if (dist < best_d) best_d = dist, best = k;
      }
      rp_key_ = best;
    }
  }
  void RpNextField(int dir) {
    const std::vector<int> fields = RpFields();
    int at = int(std::find(fields.begin(), fields.end(), rp_field_) - fields.begin());
    at = std::clamp(at + dir, 0, int(fields.size()) - 1);
    RpFocus(fields[size_t(at)]);
    if (!RpText(rp_field_)) rp_keys_ = false;
  }

  // ---- steps
  void RpGoStep(int step) {
    if (step == rp_step_) return;
    if (step > 0 && rp_step_ == 0) {
      rp_error_ = report::Validate(rp_);
      if (!rp_error_.empty()) {
        size_t title = 0;
        for (char c : rp_.title) title += c != ' ' && c != '\n';
        RpFocus(title < report::kTitleMin ? kRpTitle : kRpWhat);
        REXLOG_INFO("BF report: not yet: {}", rp_error_);
        return;
      }
    }
    if (rp_dirty_) report::SaveDraft(UserDir(), rp_), rp_dirty_ = false;
    rp_keys_ = false;
    if (step > 0 && (rp_step_ == 0 || (!rp_gather_ && rp_parts_.empty()))) RpStartGather();  // what was typed may have changed
    if (step == 1) rp_row_ = 0, rp_scroll_ = 0;
    if (step == 2) rp_send_row_ = 0, rp_job_.reset();
    rp_step_ = step;
    REXLOG_INFO("BF report: step {}", step + 1);
  }
  report::Context RpContext() {
    report::Context c;
    c.user_dir = UserDir();
    c.player_name = settings_.Get("player_name", "");
    c.account_code = settings_.Get("account_code", "");
    c.tick_rate = TickRate();
    const ImVec2 sz = ImGui::GetIO().DisplaySize;
    const bool full = settings_.Get("fullscreen", "off") == "on";
    const int hz = int(std::lround(ScreenRefresh()));
    c.display = fmt::format("{} x {} {}{}", int(sz.x), int(sz.y),
                            full ? (settings_.Get("fullscreen_mode", "desktop") == "desktop" ? "borderless" : "fullscreen") : "windowed",
                            hz > 0 ? fmt::format(", {} Hz", hz) : "");
    static const char* kKeys[] = {"graphics_preset", "frame_rate", "fullscreen", "fullscreen_mode", "monitor", "vsync",
                                  "window_width", "window_height", "auto_detect_gpu", "auto_detect_fps", "temporal_support",
                                  "nis_support"};
    for (const char* k : kKeys)
      if (const auto it = settings_.values.find(k); it != settings_.values.end()) c.settings.push_back(*it);
    for (const auto& [k, v] : settings_.values)
      if (k.rfind("cvar.native_x_renderer_", 0) == 0 || k == "cvar.bf_max_fps" || k == "cvar.bf_unlock_fps" ||
          k.rfind("cvar.d3d12_", 0) == 0 || k.rfind("cvar.vulkan_", 0) == 0)
        c.settings.push_back({k.substr(5), v});
    return c;
  }
  void RpStartGather() {
    auto job = std::make_shared<RpGather>();
    rp_gather_ = job, rp_parts_.clear();
    std::thread([job, c = RpContext(), d = rp_] {
      try {
        job->parts = report::Gather(c, d.type);
        const report::Scrubber sc = report::MakeScrubber(c);
        job->scrubbed = d;
        for (std::string* t : {&job->scrubbed.title, &job->scrubbed.what, &job->scrubbed.steps, &job->scrubbed.expected,
                               &job->scrubbed.actual, &job->scrubbed.contact})
          *t = report::Scrub(*t, sc);
      } catch (const std::exception& e) {
        // Never the game's end: the report goes with what was typed only (the scrubbing failed, so nothing typed
        // either is sent unchecked: the fields say so).
        REXLOG_ERROR("BF report: gathering the details failed ({})", e.what());
        job->parts.clear();
        report::Part p{"game", "Game"};
        p.summary = "The details could not be gathered";
        job->parts.push_back(p);
        job->scrubbed = report::Draft{};
        job->scrubbed.type = d.type;
        job->scrubbed.title = "(the report could not be checked)";
      }
      job->done = true;
    }).detach();
  }
  // The gathered parts in, with the player's ticks (and step 1's screenshot choice) over them.
  bool RpParts() {
    if (rp_gather_ && rp_gather_->done.load()) {
      rp_parts_ = std::move(rp_gather_->parts);
      rp_sent_draft_ = rp_gather_->scrubbed;
      rp_gather_.reset();
      for (report::Part& p : rp_parts_) {
        if (p.id == "screenshot" && p.available) p.on = rp_want_shot_;
        if (const auto it = rp_ticks_.find(p.id); it != rp_ticks_.end() && p.available) p.on = it->second;
      }
      ++rp_version_;
      REXLOG_INFO("BF report: {} parts gathered", rp_parts_.size());
    }
    return !rp_gather_ && !rp_parts_.empty();
  }

  // ---- the page
  void ReportPage(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("report");
    if (!rp_active_) ReportBegin();
    const Stage st = StageOf(io);
    Chrome(d, io, st, 0, SceneBehind() ? 0.82f : 0);
    d->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(10, 12, 8, 150));
    RpSheet(d, st);
    if (rp_dirty_ && ImGui::GetTime() - rp_saved_at_ > 3.0) {
      report::SaveDraft(UserDir(), rp_);
      rp_dirty_ = false, rp_saved_at_ = ImGui::GetTime();
    }
    switch (rp_step_) {
      case 0: RpFormStep(d, io, st, in); break;
      case 1: RpReviewStep(d, io, st, in); break;
      default: RpSendStep(d, io, st, in); break;
    }
    StampCorner(d, st);
  }

  // Step 1: the report.
  void RpFormStep(ImDrawList* d, ImGuiIO& io, const Stage& st, Input in) {
    const kit::Fonts& f = kit::GetFonts();
    const bool text_field = RpText(rp_field_) != nullptr;
    const bool keyboard_enter = in.accept && accept_from_key_;
    const bool ctrl_enter = io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
    // The keyboard types straight into the field the cursor is on (the window asked for text input every frame, as the
    // name page does).
    if (text_field) {
      ImGuiContext& g = *ImGui::GetCurrentContext();
      g.PlatformImeData.WantTextInput = true;
      g.PlatformImeData.ViewportId = ImGui::GetMainViewport()->ID;
      for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const ImWchar c = io.InputQueueCharacters[i];
        if (c >= 32 && c != 127) {
          rp_shift_ = false;  // the keyboard's own case, not the on screen keys' capital
          RpType(unsigned(c));
          rp_keys_ = false;  // a real keyboard types: the on screen keys close
        }
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) RpDelete();
      // Ctrl+V or Shift+Insert (paste_keys.h: io.KeyCtrl, waited for here before, is never set in our menus).
      if (paste::Pressed())
        if (const std::string clip = paste::Text(); !clip.empty())
          for (const char* c = clip.c_str(); *c; ++c)
            if (*c != '\r' && (static_cast<unsigned char>(*c) >= 32 || *c == '\n')) {
              std::string* t = RpText(rp_field_);
              if (t->size() >= RpMax(rp_field_)) break;
              if (*c == '\n' && !RpMulti(rp_field_)) *t += ' ';
              else *t += *c;
              RpChanged();
            }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) RpNextField(io.KeyShift ? -1 : 1), rp_keys_ = false;
    // ---- the left column: the fields
    const std::vector<int> fields = RpFields();
    const float x0 = st.P(64, 0).x, x1 = st.P(716, 0).x;
    float y = st.P(0, 92).y;
    int clicked_field = -1;
    for (int field : fields) {
      const bool focused = rp_field_ == field;
      switch (field) {
        case kRpType: {
          RpLabel(d, st, ImVec2(x0, y), "TYPE OF ISSUE", true);
          std::vector<std::string> names;
          for (int t = 0; t < report::kTypeCount; ++t) names.push_back(report::TypeName(t));
          const int c = RpChips(d, st, ImVec2(x0, y + st(14)), names, rp_.type, focused);
          if (c >= 0) rp_.type = c, RpFocus(kRpType), RpChanged();
          y += st(52);
          break;
        }
        case kRpOften: {
          RpLabel(d, st, ImVec2(x0, y), "HOW OFTEN", false);
          std::vector<std::string> names(std::begin(report::kOften), std::end(report::kOften));
          const int c = RpChips(d, st, ImVec2(x0, y + st(14)), names, rp_.often, focused);
          if (c >= 0) rp_.often = c, RpFocus(kRpOften), RpChanged();
          y += st(52);
          break;
        }
        case kRpExpected: {  // with What happened instead beside it
          const float mid = (x0 + x1) * 0.5f;
          RpLabel(d, st, ImVec2(x0, y), "WHAT YOU EXPECTED", false, "OPTIONAL");
          RpLabel(d, st, ImVec2(mid + st(8), y), "WHAT HAPPENED INSTEAD", false, "OPTIONAL");
          if (RpBox(d, st, ImVec2(x0, y + st(14)), ImVec2(mid - st(8), y + st(46)), rp_.expected, "", report::kShortMax,
                    rp_field_ == kRpExpected, false))
            clicked_field = kRpExpected;
          if (RpBox(d, st, ImVec2(mid + st(8), y + st(14)), ImVec2(x1, y + st(46)), rp_.actual, "If it is not said above",
                    report::kShortMax, rp_field_ == kRpActual, false))
            clicked_field = kRpActual;
          y += st(56);
          break;
        }
        case kRpActual: break;  // drawn with Expected
        default: {
          const bool idea = rp_.type == report::kSuggestion;
          const char* name = field == kRpTitle ? "TITLE" : field == kRpWhat ? (idea ? "YOUR IDEA" : "WHAT HAPPENED")
                             : field == kRpSteps ? (idea ? "WHY IT HELPS" : "STEPS TO REPRODUCE") : "CONTACT";
          const bool required = field == kRpTitle || field == kRpWhat;
          const std::string note = field == kRpTitle ? "ONE LINE, WHAT WENT WRONG" : field == kRpContact ? "OPTIONAL, IF YOU WANT A REPLY"
                                   : required ? "" : "OPTIONAL";
          RpLabel(d, st, ImVec2(x0, y), name, required, note);
          const float h = field == kRpWhat ? st(80) : field == kRpSteps ? st(60) : st(32);
          const std::string hint = field == kRpContact ? "A Discord or GitHub name, shown on the public report. Empty stays anonymous."
                                   : field == kRpSteps && !idea ? "1. Join a server..." : "";
          if (RpBox(d, st, ImVec2(x0, y + st(14)), ImVec2(x1, y + st(14) + h), *RpText(field), hint, RpMax(field), focused,
                    RpMulti(field)))
            clicked_field = field;
          y += st(14) + h + st(10);
        }
      }
    }
    if (clicked_field >= 0) {
      RpFocus(clicked_field);
      if (device_ != Device::kKeyboard) rp_keys_ = true;
    }
    // ---- the right column: the screenshot, the keys, what is attached
    const float rx = st.P(744, 0).x, rw = st(472);
    {
      RpLabel(d, st, ImVec2(rx, st.P(0, 92).y), "SCREENSHOT", false, "OPTIONAL");
      const ImVec2 a(rx, st.P(0, 106).y), b(rx + rw, st.P(0, 176).y);
      d->AddRectFilled(a, b, kit::kPaperLight);
      d->AddRect(a, b, kit::kBorder, 0, 0, 1.0f);
      const fs::path shot = report::NewestScreenshot(UserDir(), 24);
      float tx = a.x + st(10);
      if (!shot.empty() && kit::DrawImageFile(d, shot, ImVec2(a.x + st(6), a.y + st(6)), ImVec2(a.x + st(6) + st(103), b.y - st(6)), false,
                                              false, 320, rp_want_shot_ ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 80)))
        tx = a.x + st(120);
      const std::string head = shot.empty() ? "No screenshot yet" : rp_want_shot_ ? "Your newest screenshot, attached" : "Not attached";
      d->AddText(f.ui_bold, st(13), ImVec2(tx, a.y + st(8)), kit::kInk, head.c_str());
      const std::string sub = shot.empty() ? "Press F12 in a match to save one, then come back here. It is attached by itself."
                                           : shot.filename().string() + ". Y attaches or leaves it out.";
      kit::Wrapped(d, f.ui, st(11.5f), ImVec2(tx, a.y + st(28)), kit::kInkFaint, sub, b.x - tx - st(10));
      if (!shot.empty() && ImGui::IsMouseHoveringRect(a, b) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) in.y = true;
      if (in.y && !rp_keys_ && !shot.empty()) {
        rp_want_shot_ = !rp_want_shot_, rp_ticks_.erase("screenshot"), ++rp_version_;
        in.y = false;
      }
    }
    const int key = RpKeyboard(d, st, ImVec2(rx, st.P(0, 188).y), rw);
    if (key >= 0 && text_field) rp_keys_ = true, RpPressKey(key);
    {
      const ImVec2 a(rx, st.P(0, 420).y), b(rx + rw, st.P(0, 520).y);
      d->AddRectFilled(a, b, kit::kPaperLight);
      d->AddRect(a, b, kit::kBorder, 0, 0, 1.0f);
      RpLabel(d, st, ImVec2(a.x + st(12), a.y + st(10)), "SENT WITH IT", false);
      kit::Wrapped(d, f.ui, st(12), ImVec2(a.x + st(12), a.y + st(26)), kit::kInkFaint,
                   "Game and PC details, display and graphics settings, the last match, a crash report from the last week if "
                   "there is one, and the end of your log. Private details are removed. The next step shows all of it, and "
                   "any part can be left out.",
                   rw - st(24));
    }
    if (const std::string notice = RpOverlayNotice(); !notice.empty())
      kit::Wrapped(d, f.ui, st(11.5f), ImVec2(rx, st.P(0, 530).y), kit::kStamp, notice, rw);
    // ---- the foot
    std::vector<Cue> cues;
    if (rp_keys_) {
      cues = {{kit::Button::kA, "Type"}, {kit::Button::kX, "Delete"}, {kit::Button::kY, "Space"}, {kit::Button::kB, "Done typing", "Esc"}};
    } else {
      if (text_field && device_ != Device::kKeyboard) cues.push_back({kit::Button::kA, "Type"});
      if (!text_field) cues.push_back({kit::Button::kDpad, "Choose", "Left"});
      cues.push_back({kit::Button::kRB, "Next: what gets sent", "Ctrl+Enter", true});
      cues.push_back({kit::Button::kB, "Back", "Esc"});
    }
    const float pw = Prompts(d, st, st.P(64, 720 - 30 - 36), cues, in, 15);
    const std::string help = !rp_error_.empty() ? Upper(rp_error_)
                             : rp_keys_ ? "THE D-PAD MOVES ON THE KEYS \xC2\xB7 LB SHIFT \xC2\xB7 DONE OR B CLOSES THEM"
                             : device_ == Device::kKeyboard ? "JUST TYPE \xC2\xB7 TAB NEXT FIELD \xC2\xB7 ENTER A NEW LINE \xC2\xB7 CTRL+V PASTES"
                                                            : "UP AND DOWN PICK A FIELD \xC2\xB7 LEFT AND RIGHT PICK A CHOICE";
    kit::Spaced(d, f.typed, st(8.5f), ImVec2(st.P(64, 0).x + pw + st(10), st.P(0, 720 - 30 - 36 + 12).y),
                rp_error_.empty() ? kit::kKhakiText : IM_COL32(255, 140, 120, 255), help, st(1.5f));
    // ---- the input
    if (rp_keys_) {
      RpMoveKey(in);
      if (in.accept && !keyboard_enter) RpPressKey(rp_key_);
      if (in.x) RpDelete();
      if (in.y) RpType(' ');
      if (in.prev_tab) rp_shift_ = !rp_shift_;
      if (in.back || in.start) rp_keys_ = false;
      if (keyboard_enter) {  // a real keyboard's Enter while the keys are open: as on the field
        rp_keys_ = false;
        if (RpMulti(rp_field_)) RpType('\n');
        else RpNextField(1);
      }
      return;
    }
    if (ctrl_enter || in.next_tab || (in.start && !text_field)) return RpGoStep(1);
    if (in.start) return RpGoStep(1);
    if (in.up) RpNextField(-1);
    if (in.down) RpNextField(1);
    if ((in.left || in.right) && (rp_field_ == kRpType || rp_field_ == kRpOften)) {
      if (rp_field_ == kRpType) rp_.type = (rp_.type + (in.right ? 1 : report::kTypeCount - 1)) % report::kTypeCount;
      else rp_.often = ((rp_.often < 0 ? (in.right ? -1 : 0) : rp_.often) + (in.right ? 1 : report::kOftenCount - 1)) % report::kOftenCount;
      RpChanged();
    }
    if (in.accept) {
      if (keyboard_enter && text_field) {
        if (RpMulti(rp_field_)) RpType('\n');
        else RpNextField(1);
      } else if (text_field) {
        rp_keys_ = true;
      } else {
        RpNextField(1);
      }
    }
    if (in.x && text_field) RpDelete();
    if (in.back) ReportLeave();
  }

  // The overlays known to crash the game at its first frame (the old Report page's notice, 12 Nov 2026), once a second.
  std::string RpOverlayNotice() {
#if defined(_WIN32)
#include "overlay_hooks.inc"
    static double checked = -1;
    static std::string notice;
    if (const double now = ImGui::GetTime(); checked < 0 || now - checked > 1) {
      checked = now;
      std::vector<std::string> names;
      for (const HookedOverlay& h : HookedOverlays())
        if (h.product == "RivaTuner Statistics Server" || h.product == "OBS Studio") names.push_back(h.product);
      notice.clear();
      if (!names.empty()) {
        std::string who;
        for (size_t i = 0; i < names.size(); ++i) who += (i == 0 ? "" : " and ") + names[i];
        notice = who + (names.size() == 1 ? " is" : " are") +
                 " hooked into the game. If the game crashes at its first frame, close it or turn its overlay off for "
                 "bf1943.exe, and say so in your report.";
      }
    }
    return notice;
#else
    return {};
#endif
  }

  // Step 2: what gets sent.
  void RpBuildPreview(float width, float key_w, const kit::Fonts& f, float size) {
    if (rp_lines_version_ == rp_version_ && rp_lines_width_ == width) return;
    rp_lines_version_ = rp_version_, rp_lines_width_ = width;
    rp_lines_.clear();
    const std::string body = report::Body(rp_sent_draft_, rp_parts_);
    std::vector<std::string> body_lines;
    for (size_t at = 0; at <= body.size();) {
      const size_t nl = std::min(body.find('\n', at), body.size());
      body_lines.push_back(body.substr(at, nl - at));
      at = nl + 1;
    }
    bool in_details = false, in_fence = false;
    int folded = 0;
    auto flush_more = [&] {
      if (folded > 3) rp_lines_.push_back({5, fmt::format("... {} more lines, folded away on GitHub", folded - 3), ""});
      folded = 0;
    };
    for (const std::string& line : body_lines) {
      if (line.rfind("<details><summary>", 0) == 0) {
        const size_t end = line.find("</summary>");
        rp_lines_.push_back({3, "+ " + line.substr(18, end == std::string::npos ? std::string::npos : end - 18), ""});
        in_details = true, folded = 0;
        continue;
      }
      if (line == "</details>") {
        flush_more();
        in_details = false;
        continue;
      }
      if (line.rfind("```", 0) == 0) {
        in_fence = !in_fence;
        continue;
      }
      if (in_details) {
        if (in_fence && ++folded <= 3) rp_lines_.push_back({4, line.size() > 90 ? line.substr(0, 90) + "..." : line, ""});
        continue;
      }
      if (line.rfind("### ", 0) == 0) {
        rp_lines_.push_back({1, line.substr(4), ""});
        continue;
      }
      if (line == "| | |" || line == "|---|---|") continue;
      if (line.size() > 2 && line[0] == '|') {
        const size_t mid = line.find(" | ", 2);
        if (mid != std::string::npos) {
          std::string v = line.substr(mid + 3);
          if (v.size() >= 2 && v.compare(v.size() - 2, 2, " |") == 0) v.resize(v.size() - 2);
          const std::vector<std::string> wrapped = RpWrap(f.ui, size, v, width - key_w);
          for (size_t i = 0; i < wrapped.size(); ++i) rp_lines_.push_back({2, i == 0 ? line.substr(2, mid - 2) : "", wrapped[i]});
          continue;
        }
      }
      if (line == "<!-- screenshot -->") {
        rp_lines_.push_back({8, "", ""});
        continue;
      }
      if (line == "---") {
        rp_lines_.push_back({6, "", ""});
        continue;
      }
      if (line.size() > 2 && line.front() == '_' && line.back() == '_') {
        rp_lines_.push_back({7, line.substr(1, line.size() - 2), ""});
        continue;
      }
      if (line.empty()) {
        if (!rp_lines_.empty() && rp_lines_.back().style == 0 && !rp_lines_.back().a.empty()) rp_lines_.push_back({0, "", ""});
        continue;
      }
      for (const std::string& w : RpWrap(f.ui, size, line, width)) rp_lines_.push_back({0, w, ""});
    }
    flush_more();
  }
  float st_preview_key_ = 90;  // the table's key column (stage units, scaled when drawn)

  void RpReviewStep(ImDrawList* d, ImGuiIO& io, const Stage& st, Input in) {
    const kit::Fonts& f = kit::GetFonts();
    std::vector<Cue> cues = {{kit::Button::kA, "Tick / untick"}, {kit::Button::kLB, "Back to the report"},
                             {kit::Button::kRB, "Next: send", "", true}, {kit::Button::kB, "Back", "Esc"}};
    Prompts(d, st, st.P(64, 720 - 30 - 36), cues, in, 15);
    if (!RpParts()) {
      kit::Spaced(d, f.typed, st(11), st.P(64, 100), kit::kInkSection, "GATHERING THE DETAILS...", st(2));
      if (in.back || in.prev_tab) RpGoStep(0);
      return;
    }
    // ---- the left column: the parts and their ticks
    const float x0 = st.P(64, 0).x, x1 = st.P(560, 0).x;
    RpLabel(d, st, ImVec2(x0, st.P(0, 92).y), "SENT WITH YOUR REPORT", false, "A TICKS OR UNTICKS");
    const int n = int(rp_parts_.size());
    rp_row_ = std::clamp(rp_row_, 0, n);
    float y = st.P(0, 108).y;
    const ImVec2 card0(x0, y);
    const float row_h = st(46);
    d->AddRectFilled(card0, ImVec2(x1, y + row_h * float(n) + st(8)), kit::kPaperLight);
    d->AddRect(card0, ImVec2(x1, y + row_h * float(n) + st(8)), kit::kBorder, 0, 0, 1.0f);
    y += st(4);
    int clicked = -1;
    for (int i = 0; i < n; ++i) {
      const report::Part& p = rp_parts_[size_t(i)];
      const ImVec2 a(x0 + st(4), y), b(x1 - st(4), y + row_h);
      const bool sel = rp_row_ == i;
      if (sel) {
        d->AddRectFilled(a, b, IM_COL32(93, 138, 50, 36));
        d->AddRect(a, b, kit::kGreen, 0, 0, st(2));
      }
      const ImVec2 c0(a.x + st(10), a.y + st(8)), c1(c0.x + st(16), c0.y + st(16));
      const bool on = p.on && p.available;
      d->AddRectFilled(c0, c1, on ? kit::kGreen : p.available ? IM_COL32(255, 255, 255, 255) : kit::kPaperDim);
      d->AddRect(c0, c1, on ? kit::kGreenShadow : p.available ? kit::kInk : kit::kRule, 0, 0, st(1.5f));
      if (on) {
        const ImVec2 pts[] = {ImVec2(c0.x + st(3.5f), c0.y + st(8)), ImVec2(c0.x + st(7), c0.y + st(11.5f)), ImVec2(c0.x + st(12.5f), c0.y + st(4.5f))};
        d->AddPolyline(pts, 3, IM_COL32(255, 255, 255, 255), 0, st(2.2f));
      }
      d->AddText(f.ui_bold, st(13), ImVec2(c1.x + st(10), a.y + st(5)), p.available ? kit::kInk : kit::kInkSection, p.name.c_str());
      if (!p.text.empty() && p.available) {
        const std::string kb = fmt::format("{:.1f} KB", double(p.text.size()) / 1024.0);
        kit::Spaced(d, f.typed, st(8), ImVec2(b.x - st(8) - kit::SpacedWidth(f.typed, st(8), kb, st(1)), a.y + st(8)), kit::kInkSection, kb, st(1));
      }
      d->PushClipRect(ImVec2(c1.x + st(10), a.y), ImVec2(b.x - st(6), b.y), true);
      d->AddText(f.ui, st(11), ImVec2(c1.x + st(10), a.y + st(24)), kit::kInkFaint, p.summary.c_str());
      d->PopClipRect();
      if (ImGui::IsMouseHoveringRect(a, b)) {
        if (io.MouseDelta.x + io.MouseDelta.y != 0) rp_row_ = i;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = i;
      }
      if (i + 1 < n) d->AddRectFilled(ImVec2(a.x + st(6), b.y - 1), ImVec2(b.x - st(6), b.y), kit::kRule);
      y += row_h;
    }
    {
      const float sy = y + st(18);
      const ImVec2 a(x0, sy), b(x1, sy + st(58));
      d->AddRectFilled(a, b, IM_COL32(206, 62, 48, 22));
      d->AddRectFilled(a, ImVec2(a.x + st(4), b.y), kit::kStamp);
      kit::Wrapped(d, f.ui, st(11.5f), ImVec2(a.x + st(14), a.y + st(8)), kit::kInkFaint,
                   "Removed before anything is sent: your Windows user name and PC name, folder paths in your user folder, "
                   "IP addresses, your name's recovery code, your install id, keys and tokens, e-mail addresses. What you "
                   "see on the right is exactly what is sent.",
                   b.x - a.x - st(24));
    }
    // ---- the right column: the issue as GitHub shows it
    const ImVec2 pa = st.P(584, 92), pb = st.P(1216, 600);
    d->AddRectFilled(pa, pb, IM_COL32(255, 255, 255, 255), st(4));
    d->AddRect(pa, pb, IM_COL32(208, 215, 222, 255), st(4), 0, 1.0f);
    const ImU32 gh_ink = IM_COL32(31, 35, 40, 255), gh_dim = IM_COL32(89, 99, 110, 255), gh_rule = IM_COL32(216, 222, 228, 255);
    float py = pa.y + st(12);
    const float px = pa.x + st(16), pw = pb.x - pa.x - st(32);
    {
      const std::string title = report::Title(rp_sent_draft_);
      for (const std::string& w : RpWrap(f.ui, st(17), title, pw)) {
        d->AddText(f.ui, st(17), ImVec2(px, py), gh_ink, w.c_str());
        py += st(22);
      }
      float lx = px;
      auto label = [&](const std::string& t, ImU32 bg, ImU32 fg) {
        const float w = kit::TextWidth(f.ui_bold, st(10), t) + st(14);
        d->AddRectFilled(ImVec2(lx, py + st(2)), ImVec2(lx + w, py + st(18)), bg, st(8));
        d->AddText(f.ui_bold, st(10), ImVec2(lx + st(7), py + st(3)), fg, t.c_str());
        lx += w + st(6);
      };
      label("Open", IM_COL32(31, 136, 61, 255), IM_COL32(255, 255, 255, 255));
      label(report::TypeLabel(rp_sent_draft_.type), IM_COL32(182, 2, 5, 255), IM_COL32(255, 255, 255, 255));
      label("from-game", IM_COL32(237, 237, 237, 255), IM_COL32(51, 51, 51, 255));
      label("needs-triage", IM_COL32(251, 202, 4, 255), IM_COL32(51, 51, 51, 255));
      py += st(26);
      d->AddRectFilled(ImVec2(pa.x, py), ImVec2(pb.x, py + 1), gh_rule);
      py += st(6);
    }
    const float body_top = py, size = st(11.5f), line_h = st(15);
    RpBuildPreview(pw, st(st_preview_key_), f, size);
    float total = 0;
    for (const RpLine& l : rp_lines_) total += l.style == 1 ? line_h * 1.6f : l.style == 8 ? st(78) : line_h;
    const float view_h = pb.y - body_top - st(8);
    const float max_scroll = std::max(0.0f, total - view_h);
    if (ImGui::IsMouseHoveringRect(pa, pb) && io.MouseWheel != 0) rp_scroll_ -= io.MouseWheel * st(40);
    rp_scroll_ = std::clamp(rp_scroll_, 0.0f, max_scroll);
    d->PushClipRect(ImVec2(pa.x, body_top), ImVec2(pb.x, pb.y - st(4)), true);
    py = body_top + st(4) - rp_scroll_;
    const fs::path shot = [&] {
      for (const report::Part& p : rp_parts_)
        if (p.id == "screenshot" && p.on && p.available) return p.file;
      return fs::path();
    }();
    for (const RpLine& l : rp_lines_) {
      const float h = l.style == 1 ? line_h * 1.6f : l.style == 8 ? st(78) : line_h;
      if (py + h >= body_top - st(10) && py <= pb.y) {
        switch (l.style) {
          case 1:
            d->AddText(f.ui_bold, st(13), ImVec2(px, py + st(4)), gh_ink, l.a.c_str());
            d->AddRectFilled(ImVec2(px, py + h - st(2)), ImVec2(px + pw, py + h - st(1)), gh_rule);
            break;
          case 2:
            if (!l.a.empty()) d->AddText(f.ui_bold, size, ImVec2(px, py), gh_ink, l.a.c_str());
            d->AddText(f.ui, size, ImVec2(px + st(st_preview_key_), py), gh_ink, l.b.c_str());
            break;
          case 3: d->AddText(f.ui_bold, size, ImVec2(px, py), gh_ink, l.a.c_str()); break;
          case 4: d->AddText(f.typed, st(9.5f), ImVec2(px + st(12), py + st(1)), gh_dim, l.a.c_str()); break;
          case 5: d->AddText(f.ui, st(10.5f), ImVec2(px + st(12), py), gh_dim, l.a.c_str()); break;
          case 6: d->AddRectFilled(ImVec2(px, py + h * 0.5f), ImVec2(px + pw, py + h * 0.5f + 1), gh_rule); break;
          case 7: d->AddText(f.ui, st(10.5f), ImVec2(px, py), gh_dim, l.a.c_str()); break;
          case 8:
            if (shot.empty() || !kit::DrawImageFile(d, shot, ImVec2(px, py + st(4)), ImVec2(px + st(128), py + st(76)), false, false, 320))
              d->AddText(f.ui, size, ImVec2(px, py), gh_dim, "[the screenshot]");
            break;
          default: d->AddText(f.ui, size, ImVec2(px, py), gh_ink, l.a.c_str()); break;
        }
      }
      py += h;
    }
    d->PopClipRect();
    if (max_scroll > 0) {  // the scroll bar
      const float track = pb.y - body_top - st(8);
      const float knob = std::max(st(20), track * view_h / total);
      const float at = body_top + st(4) + (track - knob) * (rp_scroll_ / max_scroll);
      d->AddRectFilled(ImVec2(pb.x - st(6), at), ImVec2(pb.x - st(3), at + knob), rp_row_ == n ? kit::kGreen : IM_COL32(175, 184, 193, 255), st(2));
    }
    kit::Spaced(d, f.typed, st(8), ImVec2(pa.x, pb.y + st(8)), rp_row_ == n ? kit::kGreenBright : kit::kInkSection,
                rp_row_ == n ? "THE PREVIEW: UP AND DOWN SCROLL IT \xC2\xB7 B BACK TO THE LIST"
                             : "DOWN PAST THE LAST PART, OR THE MOUSE WHEEL, SCROLLS THE PREVIEW",
                st(1.5f));
    // ---- the input
    auto toggle = [&](int i) {
      report::Part& p = rp_parts_[size_t(i)];
      if (!p.available) return;
      p.on = !p.on;
      rp_ticks_[p.id] = p.on;
      if (p.id == "screenshot") rp_want_shot_ = p.on;
      ++rp_version_;
      REXLOG_INFO("BF report: {} {}", p.name, p.on ? "ticked" : "unticked");
    };
    if (clicked >= 0) rp_row_ = clicked, toggle(clicked);
    if (rp_row_ == n) {  // in the preview
      if (in.down) rp_scroll_ += st(60);
      if (in.up) {
        if (rp_scroll_ <= 0) rp_row_ = n - 1;
        else rp_scroll_ -= st(60);
      }
      if (in.back) {
        rp_row_ = n - 1;
        in.back = false;
      }
    } else {
      if (in.up) rp_row_ = std::max(0, rp_row_ - 1);
      if (in.down) rp_row_ = std::min(n, rp_row_ + 1);
      if (in.accept) toggle(rp_row_);
    }
    if (in.prev_tab || in.back) return RpGoStep(0);
    if (in.next_tab || in.start) return RpGoStep(2);
  }

  // Step 3: send.
  void RpSendStep(ImDrawList* d, ImGuiIO& io, const Stage& st, Input in) {
    const kit::Fonts& f = kit::GetFonts();
    (void)io;
    if (!RpParts()) {
      kit::Spaced(d, f.typed, st(11), st.P(64, 100), kit::kInkSection, "GATHERING THE DETAILS...", st(2));
      if (in.back || in.prev_tab) RpGoStep(1);
      return;
    }
    const bool relay = !report::Endpoint().empty() || report::DryRun();
    enum { kSendNow, kBrowser, kCopy };
    std::vector<int> rows;
    if (relay) rows.push_back(kSendNow);
    rows.push_back(kBrowser), rows.push_back(kCopy);
    rp_send_row_ = std::clamp(rp_send_row_, 0, int(rows.size()) - 1);
    const int state = rp_job_ ? rp_job_->state.load() : -1;
    const bool done = state == report::Send::kSent || state == report::Send::kDuplicate;
    // ---- the left column: the three ways
    const float x0 = st.P(64, 0).x, x1 = st.P(600, 0).x;
    RpLabel(d, st, ImVec2(x0, st.P(0, 92).y), "HOW TO SEND IT", false);
    const ImVec2 ca(x0, st.P(0, 108).y), cb(x1, st.P(0, 108 + 96 * float(rows.size()) + 16).y);
    d->AddRectFilled(ca, cb, kit::kPaperLight);
    d->AddRect(ca, cb, kit::kBorder, 0, 0, 1.0f);
    float y = ca.y + st(14);
    int clicked = -1;
    const kit::Style style = PromptStyle();
    for (int i = 0; i < int(rows.size()); ++i) {
      const int r = rows[size_t(i)];
      const kit::Button btn = r == kSendNow ? kit::Button::kA : r == kBrowser ? kit::Button::kY : kit::Button::kX;
      const char* head = r == kSendNow ? (report::DryRun() ? "SEND NOW (DRY RUN)" : "SEND NOW")
                         : r == kBrowser ? "OPEN ON GITHUB WITH MY ACCOUNT" : "COPY THE REPORT";
      const char* sub = r == kSendNow ? "No GitHub account needed" : r == kBrowser ? "Your browser, the form filled in: you press Submit"
                                                                                   : "For Discord, or anywhere else";
      const char* text =
          r == kSendNow ? "Sent to our report service, which files it on GitHub for you. If the same crash is already "
                          "filed, yours is added to it instead."
          : r == kBrowser ? "For players with a GitHub account who want replies by e-mail. What does not fit in the link "
                            "(the log) is copied: paste it into the Log box. The screenshot's folder opens to drag it in."
                          : "The whole report as text on your clipboard.";
      const float hw = kit::SpacedWidth(f.ui_bold, st(12.5f), head, st(1.2f));
      const ImVec2 b0(x0 + st(14), y), b1(b0.x + std::max(hw, kit::TextWidth(f.ui, st(9.5f), sub)) + st(52), y + st(38));
      const bool sel = rp_send_row_ == i;
      const ImU32 fill = r == kSendNow ? kit::kGreen : r == kBrowser ? kit::kStrip : kit::kPaperLight;
      d->AddRectFilled(b0, b1, fill);
      if (r == kCopy) d->AddRect(b0, b1, kit::kInk, 0, 0, st(1.5f));
      if (sel) d->AddRect(ImVec2(b0.x - st(4), b0.y - st(4)), ImVec2(b1.x + st(4), b1.y + st(4)), kit::kStamp, 0, 0, st(2.5f));
      const ImU32 ink = r == kCopy ? kit::kInk : kit::kStripText;
      kit::Glyph(d, style, btn, KeyFor(btn), ImVec2(b0.x + st(10), (b0.y + b1.y) * 0.5f), st(20));
      kit::Spaced(d, f.ui_bold, st(12.5f), ImVec2(b0.x + st(38), b0.y + st(5)), ink, head, st(1.2f));
      d->AddText(f.ui, st(9.5f), ImVec2(b0.x + st(38), b0.y + st(23)), ink, sub);
      kit::Wrapped(d, f.ui, st(11), ImVec2(x0 + st(14), b1.y + st(8)), kit::kInkFaint, text, x1 - x0 - st(28));
      if (ImGui::IsMouseHoveringRect(b0, b1)) {
        if (ImGui::GetIO().MouseDelta.x + ImGui::GetIO().MouseDelta.y != 0) rp_send_row_ = i;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = r;
      }
      y += st(96);
    }
    if (!rp_note_.empty() && ImGui::GetTime() - rp_note_at_ < 12)
      kit::Wrapped(d, f.ui_bold, st(11.5f), ImVec2(x0, cb.y + st(10)), kit::kGreenDark, rp_note_, x1 - x0);
    // ---- the right column: what happened
    {
      const ImVec2 a = st.P(624, 92), b = st.P(1216, 280);
      std::string kicker, head, text;
      ImU32 edge = kit::kRule;
      switch (state) {
        case report::Send::kSending:
          kicker = "SENDING", head = "ONE MOMENT", text = "Your report is on its way to the report service.";
          break;
        case report::Send::kSent:
          edge = kit::kGreen, kicker = "SENT", head = "THANK YOU, SOLDIER";
          text = fmt::format("Filed as issue #{}: {}. A opens it.", rp_job_->issue, rp_job_->url);
          break;
        case report::Send::kDuplicate:
          edge = IM_COL32(217, 160, 58, 255), kicker = "ALREADY KNOWN", head = "ADDED TO AN ISSUE";
          text = fmt::format("This is already issue #{}. Your details were added to it, which tells us how many players it "
                             "hits. A opens it.",
                             rp_job_->issue);
          break;
        case report::Send::kKept:
          edge = kit::kStamp, kicker = "NOT SENT YET", head = "SAVED FOR LATER";
          text = "The report service did not answer. Your report is kept and sent by itself the next time the game "
                 "starts online. Nothing you typed is lost. A tries again; GitHub in the browser and Copy work now.";
          break;
        case report::Send::kRefused:
          edge = kit::kStamp, kicker = "NOT SENT", head = "REFUSED";
          text = rp_job_->message + " GitHub in the browser and Copy still work.";
          break;
        case report::Send::kDryRun:
          edge = kit::kStamp, kicker = "DRY RUN", head = "NOTHING WAS SENT";
          text = "The exact request and the issue it would create were written to " + rp_job_->message +
                 " in your user folder (and the request beside it, .json and .http).";
          break;
        default:
          kicker = "READY", head = "CHECKED AND READY";
          text = fmt::format("{} \xC2\xB7 {} parts attached, {:.0f} KB. Private details removed.",
                             report::Title(rp_sent_draft_), std::count_if(rp_parts_.begin(), rp_parts_.end(),
                                                                          [](const report::Part& p) { return p.on && p.available; }),
                             double(report::RequestJson(rp_sent_draft_, rp_parts_).size()) / 1024.0);
      }
      d->AddRectFilled(a, b, kit::kPaperLight);
      d->AddRect(a, b, kit::kBorder, 0, 0, 1.0f);
      d->AddRectFilled(a, ImVec2(a.x + st(6), b.y), edge);
      kit::Spaced(d, f.typed, st(9), ImVec2(a.x + st(22), a.y + st(16)), edge == kit::kRule ? kit::kInkSection : edge, kicker, st(2));
      kit::GameText(d, kit::GameFont::kHeadlineMedium, st(24), ImVec2(a.x + st(22), a.y + st(32)), kit::kInk, head);
      kit::Wrapped(d, f.ui, st(12.5f), ImVec2(a.x + st(22), a.y + st(70)), kit::kInkFaint, text, b.x - a.x - st(40));
    }
    // ---- the foot
    std::vector<Cue> cues;
    if (relay) cues.push_back({kit::Button::kA, done ? "Open the issue" : state == report::Send::kKept ? "Try again" : "Send now"});
    cues.push_back({kit::Button::kY, "GitHub instead"});
    cues.push_back({kit::Button::kX, "Copy"});
    cues.push_back({kit::Button::kB, done ? "Back to the menu" : "Back", "Esc"});
    Prompts(d, st, st.P(64, 720 - 30 - 36), cues, in, 15);
    // ---- the input
    if (in.up) rp_send_row_ = std::max(0, rp_send_row_ - 1);
    if (in.down) rp_send_row_ = std::min(int(rows.size()) - 1, rp_send_row_ + 1);
    int act = clicked;
    if (in.accept) act = rows[size_t(rp_send_row_)];
    if (in.y) act = kBrowser;
    if (in.x) act = kCopy;
    if (act == kSendNow && relay) {
      if (done) {
        OpenUrl(rp_job_->url);
      } else if (state != report::Send::kSending) {
        if (state == report::Send::kKept && !rp_job_->kept.empty()) {  // Try again: the kept copy goes, a new one if it fails
          std::error_code ec;
          fs::remove(rp_job_->kept, ec);
        }
        rp_job_ = report::StartSend(UserDir(), rp_sent_draft_, rp_parts_);
        REXLOG_INFO("BF report: sending \"{}\" ({})", report::Title(rp_sent_draft_), report::DryRun() ? "dry run" : report::Endpoint());
      }
    } else if (act == kBrowser) {
      std::string rest;
      const std::string url = report::BrowserUrl(rp_sent_draft_, rp_parts_, rest);
      if (!rest.empty()) ImGui::SetClipboardText(rest.c_str());
      if (report::DryRun()) {
        const fs::path file = report::DryRunUrl(UserDir(), url, rest);
        rp_note_ = "DRY RUN: the browser was not opened. The link (" + std::to_string(url.size()) + " characters) is in reports\\" +
                   file.filename().string() + ".";
      } else {
        OpenUrl(url);
        rp_note_ = rest.empty() ? "Opened in your browser: press Submit new issue there."
                                : "Opened in your browser. The log is on your clipboard: paste it into the Log box, then press Submit.";
#if defined(_WIN32)
        for (const report::Part& p : rp_parts_)
          if (p.id == "screenshot" && p.on && p.available)
            if (const char* v = std::getenv("BF_DEV_NO_OPEN"); !(v && *v == '1'))
              ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + p.file.wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
#endif
      }
      rp_note_at_ = ImGui::GetTime();
      REXLOG_INFO("BF report: GitHub in the browser ({} bytes in the link, {} copied)", url.size(), rest.size());
    } else if (act == kCopy) {
      ImGui::SetClipboardText(report::CopyText(rp_sent_draft_, rp_parts_).c_str());
      rp_note_ = "Copied: paste it into Discord's bug reports channel, or anywhere.", rp_note_at_ = ImGui::GetTime();
      REXLOG_INFO("BF report: copied to the clipboard");
    }
    // A sent report's draft is done with, and a kept one's too (it goes by itself; Try again resends it).
    if ((done || state == report::Send::kKept) && rp_active_ && !rp_.title.empty()) {
      report::ClearDraft(UserDir());
      rp_ = report::Draft{}, rp_dirty_ = false;
      REXLOG_INFO("BF report: {} as issue #{} ({})", state == report::Send::kSent ? "filed" : "added to", rp_job_->issue, rp_job_->url);
    }
    if (in.back) {
      if (done) {
        rp_active_ = false;
        RpBackHome();
      } else {
        RpGoStep(1);
      }
      return;
    }
    if (in.prev_tab) RpGoStep(1);
  }
