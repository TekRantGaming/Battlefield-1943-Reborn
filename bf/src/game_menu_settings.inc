// The settings in the new look, Version A1 (TekRantGaming, 12 Nov 2026: "Build it"), included inside GameMenu
// (game_menu.cpp) after game_menu_browser.inc: one page from Home, from the main menu and from the pause, and the
// same rows in the first start's Display, Graphics and Controls steps (game_menu_setup.inc). The five categories in
// a rail at the left (the game's own lettering, a typed line under each), the rows on paper in the middle (their
// sections, the chosen row green with the red tab, the Graphics preset as a strip of buttons), the chosen row's help
// on the card at the right, the PC's own card under the rail (its graphics card, its display and the preset the auto
// detect chose). The rows themselves are King Kong's (RowsOf). The mockup (runs\ui-designs, build2.py a1_settings)
// is 1920 x 1080; sizes here are its at 720 lines, on the Stage.

  static constexpr const char* kTabSubs[kTabCount] = {"WINDOW, RESOLUTION, FRAME RATE", "PRESETS, UPSCALING, LIGHTING",
                                                      "VOLUME, VOICE CHAT", "MOUSE, KEYBOARD, CONTROLLER, AIM ASSIST",
                                                      "NAME, LANGUAGE, CHAT",
                                                      // the sniper scope only in a build with it (scope_pip.h)
                                                      kScopePip ? "LETTERED FLAGS, SNIPER SCOPE" : "LETTERED FLAGS"};

  // Back where the settings were opened from: the pause (a match running), else the menu (Home before the game runs).
  void BackFromSettings() {
    capture_.clear();
    sub_ = kSubNone;
    if (from_pause_) {
      from_pause_ = false;
      page_ = Page::kPause;
      return;
    }
    page_ = pre_game_ ? Page::kHome : Page::kMain;
    from_home_ = false;
    home_row_ = kRowSettings;
  }

  // The graphics card's name as the card has room for: "NVIDIA GeForce RTX 4070 Ti" is "RTX 4070 Ti".
  static std::string ShortGpuName(std::string gpu) {
    for (const char* word : {"NVIDIA ", "GeForce ", "AMD ", "Radeon ", "Intel(R) ", "Intel ", "(R) ", "(TM) "})
      for (size_t at; (at = gpu.find(word)) != std::string::npos;) gpu.erase(at, std::strlen(word));
    return gpu;
  }

  // Keyboard bindings: the next key goes to the row being bound (Esc leaves it); the page gets no input meanwhile.
  void CaptureKeyInto(Input& in) {
    if (CaptureBind()) in = {};
  }

  // The Graphics preset strip (TekRantGaming, 13 Nov 2026: "if you navigate them it changes the below settings can we
  // make it so you need to select it even put a buton prompt on them ... right now if I select ultra it goes back to
  // potato"): left and right only move a highlight over the presets, A (Enter) or a click makes the highlighted one
  // the preset. -1: not on the strip (the highlight starts on the preset in use).
  int preset_cursor_ = -1;
  bool on_preset_strip_ = false;  // the strip is the chosen row (the last frame's): the prompts say what A does there
  // A slider held with the mouse (TekRantGaming, 13 Nov 2026: "the in game settings we made cant be moved with the
  // mouse on the sliders"): its row and name while the left button is down, the value following the mouse.
  int slider_drag_ = -1;
  std::string slider_drag_name_;

  // The rows of tab_ (and sub_) on the paper pa..pb, the chosen row's help on the card ha..hb; up, down, left, right,
  // A, the wheel, clicks and a drag on a slider act on them. Shared by the settings page and the first start's
  // settings steps.
  void SettingsBody(ImDrawList* d, ImGuiIO& io, const Stage& st, Input& in, ImVec2 pa, ImVec2 pb, ImVec2 ha, ImVec2 hb) {
    const kit::Fonts& f = kit::GetFonts();
    const bool graphics = tab_ == kTabGraphics && sub_ == kSubNone;
    if (!graphics) preset_cursor_ = -1;
    on_preset_strip_ = false;
    if (!io.MouseDown[0]) slider_drag_ = -1;
#if defined(BF_DEV_TOOLS)
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))  // the tests' clicks (BF_MENU_STEPS click/, mdown/): where they landed
      REXLOG_INFO("BF settings: click at stage {:.0f} {:.0f}", (io.MousePos.x - st.ox) / st.u, (io.MousePos.y - st.oy) / st.u);
#endif
    const int preset = graphics ? CurrentGraphicsPreset() : 0;
    const std::string last_preset = settings_.Get("graphics_preset", "");
    const std::vector<SettingRow> rows = RowsOf(tab_);
    const int count = int(rows.size());
    kit::PaperArt(d, pa, pb);
    kit::PaperArt(d, ha, hb);
    if (count == 0) return;
    if (in.up) row_ = (row_ + count - 1) % count;
    if (in.down) row_ = (row_ + 1) % count;
    row_ = std::clamp(row_, 0, count - 1);
    float top = pa.y + st(6);
    if (sub_ != kSubNone) {  // a page under Controls: its name over its rows
      kit::Spaced(d, f.typed, st(10), ImVec2(pa.x + st(18), pa.y + st(14)), kit::kInkSection,
                  sub_ == kSubRemap ? "CONTROLS \xC2\xB7 BUTTON REMAPPING" : "CONTROLS \xC2\xB7 REMAP", st(2));
      top = pa.y + st(32);
    }
    const float list_bottom = pb.y - st(6), rh = st(33), sh = st(28), strip_h = st(58);
    auto is_strip = [](const SettingRow& r) { return r.name == "Graphics preset" && !r.options.empty(); };
    auto extra_h = [&](const SettingRow& r) { return r.preview == 1 ? st(132) : r.preview == 2 ? st(112) : 0.0f; };  // a picture over the row (0.4.2)
    std::vector<float> ys(size_t(count), 0.0f);
    float total = 0;
    {
      std::string section;
      for (int i = 0; i < count; ++i) {
        if (rows[size_t(i)].section != section) section = rows[size_t(i)].section, total += sh;
        ys[size_t(i)] = total;
        total += is_strip(rows[size_t(i)]) ? strip_h : rh + extra_h(rows[size_t(i)]);
      }
    }
    const float view = list_bottom - top;
    if (ImGui::IsMouseHoveringRect(pa, pb) && io.MouseWheel != 0) scroll_ -= io.MouseWheel * rh * 2;
    if (in.up || in.down || in.prev_tab || in.next_tab) {  // the chosen row kept in view, its section's name with it
      const float h = is_strip(rows[size_t(row_)]) ? strip_h : rh + extra_h(rows[size_t(row_)]);
      if (ys[size_t(row_)] - scroll_ < sh) scroll_ = std::max(0.0f, ys[size_t(row_)] - sh);
      if (ys[size_t(row_)] + h - scroll_ > view) scroll_ = ys[size_t(row_)] + h - view;
    }
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, total - view));
    d->PushClipRect(ImVec2(pa.x, top), ImVec2(pb.x, list_bottom), true);
    std::string section;
    const float x0 = pa.x, x1 = pb.x, tx0 = pa.x + st(18), tx1 = pb.x - st(18), size = st(13);
    for (int i = 0; i < count; ++i) {
      const SettingRow& it = rows[size_t(i)];
      const float y_row = top + ys[size_t(i)] - scroll_, ex = extra_h(it);
      const float y = y_row + ex;  // the row's own line, under its picture
      const float h = is_strip(it) ? strip_h : rh;
      if (it.section != section) {
        section = it.section;
        kit::Spaced(d, f.typed, st(10), ImVec2(tx0, y_row - sh + st(9)), kit::kInkSection, Upper(section), st(2));
        d->AddLine(ImVec2(tx0, y_row - st(3)), ImVec2(tx1, y_row - st(3)), kit::kRule, 1.0f);
        if (!it.slots.empty()) {  // the bindings' columns: two keys, two mouse buttons
          const float bw = st(66), gap = st(6), pair = st(14);
          const float mouse_x = tx1 - 2 * bw - gap, keys_x = mouse_x - pair - 2 * bw - gap;
          if (it.slots.size() == 5)
            kit::Spaced(d, f.typed, st(7), ImVec2(keys_x - 2 * pair - bw, y_row - sh + st(10)), kit::kInkSection, "CONTROLLER", st(1.2f));
          kit::Spaced(d, f.typed, st(8), ImVec2(keys_x, y_row - sh + st(9)), kit::kInkSection, "KEYBOARD", st(2));
          kit::Spaced(d, f.typed, st(8), ImVec2(mouse_x, y_row - sh + st(9)), kit::kInkSection, "MOUSE", st(2));
        }
      }
      if (y + h < top || y_row > list_bottom) continue;
      const ImVec2 r0(x0, y_row), r1(x1, y + h);
      const bool hover = ImGui::IsMouseHoveringRect(r0, r1) && ImGui::IsMouseHoveringRect(ImVec2(pa.x, top), ImVec2(pb.x, list_bottom));
      if (hover && io.MouseDelta.x + io.MouseDelta.y != 0 && slider_drag_ < 0) row_ = i;
      const bool sel = i == row_;
      const bool clicked = hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
      if (is_strip(it)) {  // the presets as buttons (Custom is none of them)
        // Navigating never changes a setting: left and right move the highlight (it stops at the ends: right of Ultra
        // is not Potato), and A, Enter or a click makes the highlighted preset the one in use. The dark button is the
        // preset in use; the framed one with the A prompt is the highlight.
        std::vector<int> shown;
        for (int o = 0; o < int(it.options.size()); ++o)
          if (it.options[size_t(o)] != "Custom") shown.push_back(o);
        const int shown_n = int(shown.size());
        const bool can = !it.disabled && it.choose && shown_n > 0;
        auto at_of = [&](int option) {  // an option's place among the shown ones, or -1
          for (int k = 0; k < shown_n; ++k)
            if (shown[size_t(k)] == option) return k;
          return -1;
        };
        if (!sel) {
          preset_cursor_ = -1;
        } else if (shown_n > 0 && at_of(preset_cursor_) < 0) {
          // Arriving on the row: the highlight starts on the preset in use (Custom: on the one last chosen).
          preset_cursor_ = at_of(it.index) >= 0 ? it.index : shown.front();
          if (at_of(it.index) < 0)
            for (int o : shown)
              if (it.options[size_t(o)] == last_preset) preset_cursor_ = o;
        }
        if (sel && can && (in.left || in.right)) {
          preset_cursor_ = shown[size_t(std::clamp(at_of(preset_cursor_) + (in.left ? -1 : 1), 0, shown_n - 1))];
          REXLOG_INFO("BF settings: {} / {} highlight {} (in use: {})", kTabNames[tab_], it.name,
                      it.options[size_t(preset_cursor_)], it.options[size_t(std::clamp(it.index, 0, int(it.options.size()) - 1))]);
        }
        const float gap = st(8), bw = (tx1 - tx0 - gap * float(std::max(1, shown_n) - 1)) / float(std::max(1, shown_n));
        float bx = tx0;
        int clicked_opt = -1;
        for (int o : shown) {
          const ImVec2 b0(bx, y + st(10)), b1(bx + bw, y + h - st(10));
          const bool on = it.index == o;
          const bool bh = ImGui::IsMouseHoveringRect(b0, b1);
          if (bh && sel && can && io.MouseDelta.x + io.MouseDelta.y != 0) preset_cursor_ = o;  // the mouse highlights too
          const bool cur = sel && can && preset_cursor_ == o;
          d->AddRectFilled(b0, b1, on ? kit::kStrip : cur ? kit::kPaperLight : kit::kPaperDim);
          d->AddRect(b0, b1, cur ? kit::kStamp : on ? kit::kStrip : kit::kBorder, 0, 0, cur ? st(2.5f) : 1.0f);
          const std::string& label = it.options[size_t(o)];
          const float lw = kit::TextWidth(f.ui_bold, st(13.75f), label);
          // The prompt on the highlighted one while it is not the preset in use: A (Enter), then its name.
          const float gh = st(17);
          const float gw = cur && !on ? kit::Glyph(nullptr, PromptStyle(), kit::Button::kA, KeyFor(kit::Button::kA), ImVec2(0, 0), gh, false) +
                                            st(7)
                                      : 0.0f;
          const float lx = (b0.x + b1.x - lw - gw) * 0.5f;
          if (gw > 0) kit::Glyph(d, PromptStyle(), kit::Button::kA, KeyFor(kit::Button::kA), ImVec2(lx, (b0.y + b1.y) * 0.5f), gh);
          d->AddText(f.ui_bold, st(13.75f), ImVec2(lx + gw, b0.y + (b1.y - b0.y - st(12) * 1.45f) * 0.5f),
                     on ? kit::kStripText : kit::kInk, label.c_str());
          if (bh && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked_opt = o;
          bx += bw + gap;
        }
        if (sel) d->AddRectFilled(r0, ImVec2(r0.x + st(5), r1.y), kit::kStamp);  // the red tab marks the chosen row
        if (!can) continue;
        on_preset_strip_ = sel;
        int next = -1;
        if (clicked_opt >= 0) next = clicked_opt, row_ = i, preset_cursor_ = clicked_opt;
        else if (sel && in.accept && at_of(preset_cursor_) >= 0) next = preset_cursor_;
        if (next >= 0) {
          REXLOG_INFO("BF settings: {} / {} -> {}", kTabNames[tab_], it.name, it.options[size_t(next)]);
          it.choose(next);
        }
        continue;
      }
      if (sel) {
        d->AddRectFilled(r0, r1, kit::kGreen);
        d->AddRectFilled(r0, ImVec2(r0.x + st(5), r1.y), kit::kStamp);
      }
      const ImU32 ink = sel ? kit::kCream : it.disabled ? kit::kInkFaint : kit::kInk;
      const float ty = y + (rh - size * 1.45f) * 0.5f;
      if (it.preview) {
        // 0.4.2: what the row changes, drawn at its size. 1: the crosshairs on foot, at a gun and in a plane, at the
        // HUD's scale; 2: the minimap small, medium and large, the chosen one lit.
        const ImVec2 p0(tx0, y_row + st(6)), p1(tx1, y_row + ex - st(2));
        d->AddRectFilled(p0, p1, IM_COL32(38, 44, 36, 255));
        d->AddRect(p0, p1, kit::kBorder, 0, 0, 1.0f);
        const float cy = (p0.y + p1.y) * 0.5f - st(5), third = (p1.x - p0.x) / 3.0f;
        const ImU32 white = IM_COL32(245, 245, 240, 255), dim = IM_COL32(245, 245, 240, 90);
        auto label = [&](int k, const char* t, bool lit) {
          const float w = kit::SpacedWidth(f.typed, st(7), t, st(1.5f));
          kit::Spaced(d, f.typed, st(7), ImVec2(p0.x + third * (float(k) + 0.5f) - w * 0.5f, p1.y - st(13)),
                      lit ? kit::kCream : kit::kKhakiDim, t, st(1.5f));
        };
        // The game's own art ("use the actual crosshair and minimap meshes"): the sprites of its crosshair movie's
        // atlas and its minimap's compass, extracted from the player's game files (shader_prep.cpp ExtractMenuArt), at
        // half their size on the game's 720 line screen, so the three keep their sizes against each other.
        const fs::path art = UserDir() / "cache" / "extracted" / "menu";
        // A sprite about c (corner: one quarter of a whole, its inner corner at c, mirrored by fx and fy).
        auto sprite = [&](const char* name, ImVec2 c, float k, bool corner, bool fx, bool fy, ImU32 tint) {
          ImVec2 u0, u1;
          int w = 0, h = 0;
          if (!kit::ImageFileUv(art / name, u0, u1, w, h) || w < 1 || h < 1) return false;
          const float sw = float(w) * st.u * k, sh2 = float(h) * st.u * k;
          const ImVec2 at = corner ? ImVec2(fx ? c.x : c.x - sw, fy ? c.y : c.y - sh2) : ImVec2(c.x - sw * 0.5f, c.y - sh2 * 0.5f);
          d->AddImage(io.Fonts->TexRef, at, ImVec2(at.x + sw, at.y + sh2), ImVec2(fx ? u1.x : u0.x, fy ? u1.y : u0.y),
                      ImVec2(fx ? u0.x : u1.x, fy ? u0.y : u1.y), tint);
          return true;
        };
        if (it.preview == 1) {
          const float k = 0.5f * it.val / 100.0f;
          const ImU32 full = IM_COL32(255, 255, 255, 255);
          const ImVec2 c0(p0.x + third * 0.5f, cy), c1(p0.x + third * 1.5f, cy), c2(p0.x + third * 2.5f, cy);
          sprite("xhair_68.png", c0, k, false, false, false, full);  // on foot: the soldier's cross
          sprite("xhair_61.png", c1, k, false, false, false, full);  // a gun's ring
          sprite("xhair_58.png", c1, k, false, false, false, full);
          for (int q = 0; q < 4; ++q) sprite("xhair_105.png", c2, k, true, (q & 1) != 0, (q & 2) != 0, full);  // a plane's sight: four quarters
          sprite("xhair_58.png", c2, k, false, false, false, full);
          label(0, "ON FOOT", true), label(1, "TURRET", true), label(2, "PLANE", true);
        } else {
          static const char* kNames[3] = {"SMALL", "MEDIUM", "LARGE"};
          for (int k = 0; k < 3; ++k) {
            const bool on = it.index == k;
            const float size = 0.27f * (0.75f + 0.25f * float(k));
            const ImVec2 c(p0.x + third * (float(k) + 0.5f), cy);
            sprite("minimap_ring.png", c, size * 0.86f, false, false, false, on ? IM_COL32(58, 92, 120, 255) : IM_COL32(48, 62, 72, 200));
            if (!sprite("compass.png", c, size, false, false, false, on ? white : IM_COL32(255, 255, 255, 120)))
              d->AddCircle(c, st(30) * size / 0.27f, on ? white : dim, 40, 1.0f);
            d->AddTriangleFilled(ImVec2(c.x, c.y - st(5)), ImVec2(c.x - st(3.5f), c.y + st(4)), ImVec2(c.x + st(3.5f), c.y + st(4)),
                                 on ? IM_COL32(122, 196, 70, 255) : dim);
            label(k, kNames[k], on);
          }
        }
      }
      if (it.slots.size() == 5) d->PushClipRect(ImVec2(tx0, y), ImVec2(tx1 - 5 * st(66) - 4 * st(6) - 2 * st(14) - st(8), y + rh), true);
      d->AddText(f.ui, size, ImVec2(tx0, ty), ink, it.name.c_str());
      if (it.slots.size() == 5) d->PopClipRect();
      if (!it.slots.empty()) {
        // A binding: its four slots as boxes (13 Nov 2026). A click on one, or A on the one left and right are on,
        // makes it wait for a press; the waiting one is red.
        const int n = int(it.slots.size());
        const float bw = n == 5 ? st(66) : st(82), gap = st(6), pair = st(14);
        for (int k = 0; k < n; ++k) {
          // from the right: the mouse's pair, the keyboard's pair, and (five slots) the controller's one
          const int from_right = n - 1 - k;
          const float right = tx1 - float(from_right) * (bw + gap) - (from_right >= 4 ? 2 * pair : from_right >= 2 ? pair : 0.0f);
          const ImVec2 b0(right - bw, y + st(5)), b1(right, y + rh - st(5));
          const bool over = hover && ImGui::IsMouseHoveringRect(b0, b1);
          const bool waiting = it.slot_capture == k, cursor = sel && it.slot_cursor == k;
          d->AddRectFilled(b0, b1, waiting ? kit::kStamp : cursor || over ? kit::kPaperLight : kit::kPaperDim);
          d->AddRect(b0, b1, cursor ? kit::kStamp : kit::kBorder, 0, 0, cursor ? st(2.0f) : 1.0f);
          const std::string label = waiting ? "PRESS..." : it.slots[size_t(k)].empty() ? "-" : it.slots[size_t(k)];
          const float lw = kit::TextWidth(f.ui_bold, st(12), label);
          d->AddText(f.ui_bold, st(12), ImVec2((b0.x + b1.x - lw) * 0.5f, b0.y + (b1.y - b0.y - st(12) * 1.45f) * 0.5f),
                     waiting ? kit::kCream : it.slots[size_t(k)].empty() ? kit::kInkFaint : kit::kInk, label.c_str());
          if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && it.slot_click && capture_.empty()) {
            row_ = i;
            REXLOG_INFO("BF settings: {} / {} slot {} waits for a press", kTabNames[tab_], it.name, k + 1);
            it.slot_click(k);
          }
        }
        if (sel && !it.disabled && capture_.empty()) {
          if (it.nudge && (in.left || in.right)) it.nudge(in.left ? -1 : 1);
          if (it.activate && in.accept) it.activate();
        }
        continue;
      }
      std::string shown = it.slider ? (it.format ? it.format(it.val) : std::to_string(it.val))
                          : it.options.empty() ? it.value
                                               : it.options[size_t(std::clamp(it.index, 0, int(it.options.size()) - 1))];
      const bool changeable = !it.disabled && (it.choose || it.slider);
      if (sel && changeable) shown = "<   " + shown + "   >";
      const float vw = kit::TextWidth(f.ui_bold, size, shown), vx = tx1 - vw;
      d->AddText(f.ui_bold, size, ImVec2(vx, ty), sel ? kit::kCream : changeable ? kit::kInk : kit::kInkFaint, shown.c_str());
      if (Pending(it)) {  // changed, taken at the next start
        const float tw = kit::SpacedWidth(f.typed, st(8.75f), "NEXT START", st(1.5f));
        kit::Spaced(d, f.typed, st(8.75f), ImVec2(vx - st(14) - tw, y + rh * 0.5f - st(4)), sel ? kit::kCream : kit::kStamp, "NEXT START",
                    st(1.5f));
      }
      const float t0 = x0 + (x1 - x0) * 0.48f, tyc = y + rh * 0.5f;
      float t1 = vx - st(16);
      if (it.slider) {
        // The track ends where the widest value this slider shows would begin, so it stays put while the value
        // changes under a drag (99% to 100% moved it) and when the row is chosen (the arrows beside the value).
        float widest = vw;
        for (const float edge : {it.lo, it.hi})
          widest = std::max(widest, kit::TextWidth(f.ui_bold, size, "<   " + (it.format ? it.format(edge) : std::to_string(edge)) + "   >"));
        t1 = std::max(tx1 - widest - st(16), std::min(t1, t0 + st(120)));
      }
      if (it.slider && t1 > t0) {  // the slider's track left of its value
        const float fr = (it.val - it.lo) / std::max(1e-6f, it.hi - it.lo), fx = t0 + (t1 - t0) * fr;
        d->AddRectFilled(ImVec2(t0, tyc - st(1.5f)), ImVec2(t1, tyc + st(1.5f)), sel ? IM_COL32(255, 255, 255, 90) : kit::kRule);
        d->AddRectFilled(ImVec2(t0, tyc - st(1.5f)), ImVec2(fx, tyc + st(1.5f)), sel ? kit::kCream : it.disabled ? kit::kInkFaint : kit::kInk);
        d->AddRectFilled(ImVec2(fx - st(2.5f), tyc - st(7)), ImVec2(fx + st(2.5f), tyc + st(7)), sel ? kit::kCream : kit::kInk);
      }
      // The mouse on a slider: a press on its track takes hold of it (and chooses the row), and while the button is
      // down the value follows the mouse, off the row too; a click alone sets the value there.
      if (it.slider && !it.disabled && it.set && t1 > t0) {
        if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.MousePos.x >= t0 - st(12) && io.MousePos.x <= t1 + st(12))
          slider_drag_ = i, slider_drag_name_ = it.name, row_ = i;
        if (slider_drag_ == i && slider_drag_name_ == it.name) {
          const float at = std::clamp((io.MousePos.x - t0) / (t1 - t0), 0.0f, 1.0f);
          const float v = std::clamp(std::round((it.lo + (it.hi - it.lo) * at) / it.step) * it.step, it.lo, it.hi);
          if (std::fabs(v - it.val) > it.step * 0.25f) {
            REXLOG_INFO("BF settings: {} / {} -> {} (mouse)", kTabNames[tab_], it.name, it.format ? it.format(v) : fmt::format("{}", v));
            it.set(v);
          }
          continue;
        }
      }
      if (!sel || it.disabled) continue;
      // Each change logged, for the player's log and the tests.
      if (it.activate && (in.accept || clicked)) {
        REXLOG_INFO("BF settings: {} / {} opened", kTabNames[tab_], it.name);
        it.activate();
      } else if (it.slider && (in.left || in.right)) {
        const float v = std::clamp(it.val + (in.left ? -it.step : it.step), it.lo, it.hi);
        REXLOG_INFO("BF settings: {} / {} -> {}", kTabNames[tab_], it.name, it.format ? it.format(v) : fmt::format("{}", v));
        it.set(v);
      } else if (it.choose && !it.options.empty() && (in.left || in.right || in.accept || clicked)) {
        const int n = int(it.options.size()), next = (std::clamp(it.index, 0, n - 1) + (in.left ? n - 1 : 1)) % n;
        REXLOG_INFO("BF settings: {} / {} -> {}", kTabNames[tab_], it.name, it.options[size_t(next)]);
        it.choose(next);
      }
    }
    d->PopClipRect();
    // ---- the chosen row's help on the card at the right (and, on Graphics, the preset the rows add up to)
    {
      const SettingRow& it = rows[size_t(row_)];
      const float wrap = hb.x - ha.x - st(36);
      float y = ha.y + st(18);
      kit::Spaced(d, f.typed, st(10), ImVec2(ha.x + st(18), y), kit::kInkSection, Upper(it.name), st(2));
      y += st(20);
      y += kit::Wrapped(d, f.ui, st(13.25f), ImVec2(ha.x + st(18), y), kit::kInk, it.help, wrap) + st(14);
      auto note_box = [&](const std::string& t) {  // a boxed note with the red tab
        const float nh = kit::WrappedHeight(f.ui, st(12), t, wrap - st(28)) + st(20);
        const ImVec2 n0(ha.x + st(18), y), n1(hb.x - st(18), y + nh);
        d->AddRectFilled(n0, n1, kit::kPaperDim);
        d->AddRectFilled(n0, ImVec2(n0.x + st(3), n1.y), kit::kStamp);
        kit::Wrapped(d, f.ui, st(12), ImVec2(n0.x + st(14), n0.y + st(10)), kit::kInk, t, wrap - st(28));
        y = n1.y + st(12);
      };
      if (!it.note.empty()) note_box(it.note);
      if (graphics) {
        const auto& presets = GraphicsPresets();
        kit::Spaced(d, f.typed, st(10), ImVec2(ha.x + st(18), y), kit::kInkSection,
                    preset >= 0 ? "PRESET: " + Upper(presets[size_t(preset)].name) : "PRESET: CUSTOM", st(2));
        y += st(20);
        if (preset < 0 && !last_preset.empty())
          note_box(fmt::format("A changed row made the preset Custom. {} puts the {} preset back.",
                               device_ == Device::kKeyboard ? KeyFor(kit::Button::kX) : "X", last_preset));
      }
    }
  }

  // X on the Graphics page: the preset back once a row made it Custom.
  void PresetBack() {
    const std::string last = settings_.Get("graphics_preset", "");
    if (tab_ != kTabGraphics || sub_ != kSubNone || CurrentGraphicsPreset() >= 0 || last.empty()) return;
    const auto& presets = GraphicsPresets();
    for (size_t i = 0; i < presets.size(); ++i)
      if (last == presets[i].name) ApplyGraphicsPreset(i, " (back to the preset)");
  }
  // B on a page under Controls: back to the Controls row it came from. True when taken.
  bool SubPageBack() {
    if (sub_ == kSubNone) return false;
    const char* back_to = sub_ == kSubRemap ? "Button remapping" : "Keyboard bindings";
    sub_ = kSubNone;
    const std::vector<SettingRow> page = ControlsRows();
    row_ = 0, scroll_ = 0;
    for (size_t i = 0; i < page.size(); ++i)
      if (page[i].name == back_to) row_ = int(i);
    return true;
  }

  void SettingsA1Page(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("settings");
    UpdateTick();
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    const ImVec2 sz = io.DisplaySize;
    MenuBackdrop(d, sz, st);
    bool online_ok = false;
    const std::string status = OnlineStatus(online_ok);
    const bool edit = MenuChrome(d, io, st, status, online_ok);
    CaptureKeyInto(in);
    const bool restart = RestartPending();
    kit::Spaced(d, f.typed, st(10.5f), st.P(48, 91), restart ? kit::kStamp : kit::kKhakiDim,
                restart ? "SETTINGS MARKED NEXT START TAKE EFFECT WHEN THE GAME STARTS AGAIN"
                        : "EVERYTHING APPLIES IN GAME \xC2\xB7 SOME AT THE NEXT START",
                st(2));
    kit::GameText(d, kit::GameFont::kHeadline, st(40), st.P(48, 105), kit::kCream, "SETTINGS");
    // ---- the categories: LB and RB (Q and E), or a click on the rail
    if (sub_ == kSubNone) {
      if (in.prev_tab) tab_ = (tab_ + kTabCount - 1) % kTabCount, row_ = 0, scroll_ = 0;
      if (in.next_tab) tab_ = (tab_ + 1) % kTabCount, row_ = 0, scroll_ = 0;
    }
    tab_ = std::clamp(tab_, 0, int(kTabCount) - 1);
    const bool graphics = tab_ == kTabGraphics && sub_ == kSubNone;
    const bool controls = tab_ == kTabControls && sub_ == kSubNone;
    // Graphics changes wait for Apply on this page, and Controls has its two pages as buttons (game_menu.cpp, at
    // Cvar): both only while this page draws.
    struct PageFlags {
      bool& defer;
      bool& buttons;
      ~PageFlags() { defer = false, buttons = false; }
    } page_flags{defer_graphics_, controls_buttons_};
    defer_graphics_ = graphics, controls_buttons_ = true;
    if (!graphics && !gfx_pending_.empty()) {
      REXLOG_INFO("BF settings: GRAPHICS left with {} changes not applied: dropped", gfx_pending_.size());
      gfx_pending_.clear();
    }
    const std::string last_preset = settings_.Get("graphics_preset", "");
    const bool preset_back = graphics && CurrentGraphicsPreset() < 0 && !last_preset.empty();
    // ---- the prompts (first: a click on one is that button's press for the rest of the frame)
    std::vector<Cue> cues = {{kit::Button::kA, graphics && on_preset_strip_ ? "USE THIS PRESET" : "CHANGE"},
                             {kit::Button::kLB, "SECTION"},
                             {kit::Button::kRB, "SECTION"}};
    if (preset_back) cues.push_back({kit::Button::kX, "BACK TO " + Upper(last_preset)});
    // (No prompts for the page's own buttons, 13 Nov 2026: "Remove the extra buttons on the bottom bar". A
    // controller's Y and X still press them: Apply and Restore defaults on Graphics, Remap gamepad and Keybinds on
    // Controls.)
    if (restart) cues.push_back({kit::Button::kStart, "RESTART TO APPLY", "P", true});
    cues.push_back({kit::Button::kB, "BACK"});
    Prompts(d, st, st.P(48, 662), cues, in, 15);
    MenuFoot(d, st);
    if (restart && in.start) {
      RestartForSettings("Settings");
      return;
    }
    // ---- the rail: the five categories, the PC's card under them
    const ImVec2 ra = st.P(48, 167), rb = st.P(267, 640);
    d->AddRectFilled(ra, rb, IM_COL32(13, 15, 11, 204));
    d->AddRect(ra, rb, IM_COL32(241, 234, 211, 30), 0, 0, 1.0f);
    {
      float y = ra.y;
      // 0.4.2: a category under the mouse is lit, as the menus' rows are; its caption wraps inside the rail.
      const float cap = st(8), track = st(1.2f), room = rb.x - ra.x - st(32);
      auto caption_lines = [&](const std::string& t) {
        std::vector<std::string> lines(1);
        for (size_t at = 0; at < t.size();) {
          const size_t end = std::min(t.find(' ', at), t.size());
          const std::string word = t.substr(at, end - at);
          const std::string tried = lines.back().empty() ? word : lines.back() + " " + word;
          if (!lines.back().empty() && kit::SpacedWidth(f.typed, cap, tried, track) > room) lines.push_back(word);
          else lines.back() = tried;
          at = end + 1;
        }
        return lines;
      };
      for (int t = 0; t < kTabCount; ++t) {
        const std::vector<std::string> lines = caption_lines(kTabSubs[t]);
        const float h = st(53) + st(10) * float(lines.size() - 1);
        const ImVec2 a(ra.x, y), b(rb.x, y + h);
        const bool sel = t == tab_;
        const bool over = !sel && ImGui::IsMouseHoveringRect(a, b);
        if (sel) {
          kit::PaperArt(d, a, b);
          d->AddRectFilled(a, ImVec2(a.x + st(5), b.y), kit::kStamp);
        } else if (over) {
          d->AddRectFilled(a, b, IM_COL32(241, 234, 211, 30));
          d->AddRectFilled(a, ImVec2(a.x + st(5), b.y), IM_COL32(241, 234, 211, 110));
        }
        d->AddRectFilled(ImVec2(a.x, b.y - 1), ImVec2(b.x, b.y), IM_COL32(241, 234, 211, 20));
        kit::GameText(d, kit::GameFont::kHeadlineMedium, st(15), ImVec2(a.x + st(20), a.y + st(10)),
                      sel ? kit::kInk : over ? kit::kCream : kit::kKhaki, kTabNames[t]);
        for (size_t l = 0; l < lines.size(); ++l)
          kit::Spaced(d, f.typed, cap, ImVec2(a.x + st(20), a.y + st(34) + st(10) * float(l)),
                      sel ? kit::kInkSection : over ? kit::kKhaki : kit::kKhakiDim, lines[l], track);
        if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) tab_ = t, row_ = 0, scroll_ = 0, sub_ = kSubNone;
        y += h;
      }
      // The PC: its graphics card (as the renderer named it for the NIS check), its display, the preset the auto
      // detect chose (the GPU's tier once it is timed in the game; the fast tier's before).
      const float py = y + st(16);
      kit::Spaced(d, f.typed, st(9.5f), ImVec2(ra.x + st(20), py), kit::kKhakiDim, "YOUR PC \xC2\xB7 AUTO DETECT", st(2));
      rex::ui::Window* window = window_ ? window_() : nullptr;
      const int monitor = std::atoi(settings_.Get("monitor", "0").c_str());
      const DisplayMode desktop = DesktopDisplayMode(monitor, window);
      const std::string gpu = ShortGpuName(settings_.Get("nis_support_gpu", ""));
      const double slowness = GpuSlowness();
      const std::string chosen = settings_.Get("auto_detect_gpu", "").empty() ? "Not run yet"
                                 : slowness > 0 ? std::string(kTierPresets[TierOf(slowness)])
                                                : std::string(kTierPresets[2]) + " (not timed yet)";
      auto pc_row = [&](float yy, const char* k, const std::string& v) {
        d->AddText(f.ui, st(11.5f), ImVec2(ra.x + st(20), yy), kit::kKhakiText, k);
        float size = st(11.5f);
        const float room = rb.x - ra.x - st(40) - kit::TextWidth(f.ui, st(11.5f), k) - st(10);
        float vw = kit::TextWidth(f.ui_bold, size, v);
        if (vw > room && vw > 0) size *= std::max(0.7f, room / vw), vw = kit::TextWidth(f.ui_bold, size, v);
        d->AddText(f.ui_bold, size, ImVec2(rb.x - st(20) - vw, yy + (st(11.5f) - size) * 0.7f), kit::kKhaki, v.c_str());
        d->AddRectFilled(ImVec2(ra.x + st(20), yy + st(21)), ImVec2(rb.x - st(20), yy + st(22)), IM_COL32(241, 234, 211, 20));
      };
      pc_row(py + st(18), "Graphics card", gpu.empty() ? "Not known yet" : gpu);
      pc_row(py + st(45), "Display",
             desktop.width > 0 ? fmt::format("{} \xC3\x97 {} \xC2\xB7 {} Hz", desktop.width, desktop.height, int(std::lround(desktop.refresh)))
                               : "Not known");
      pc_row(py + st(72), "Preset chosen for you", chosen);
    }
    // ---- the rows and the help card, with the page's own buttons: dark, as the prompts under the page
    ImVec2 pa = st.P(288, 167), pb = st.P(907, 640);
    auto dark_button = [&](ImVec2 a, ImVec2 b, const std::string& label, bool lit) {
      const bool over = ImGui::IsMouseHoveringRect(a, b);
      d->AddRectFilled(a, b, over ? IM_COL32(34, 38, 28, 240) : IM_COL32(13, 15, 11, 230));
      d->AddRect(a, b, lit ? kit::kStamp : IM_COL32(241, 234, 211, over ? 110 : 50), 0, 0, lit ? st(1.5f) : 1.0f);
      const float size = st(10), w = kit::SpacedWidth(f.ui_bold, size, label, st(1.5f));
      kit::Spaced(d, f.ui_bold, size, ImVec2((a.x + b.x - w) * 0.5f, a.y + (b.y - a.y - size * 1.45f) * 0.5f),
                  lit || over ? kit::kCream : kit::kKhaki, label, st(1.5f));
      return over && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && capture_.empty();
    };
    bool open_keys = false, open_remap = false, apply = false, defaults = false;
    if (controls) {  // Keybinds and Remap gamepad over the rows
      // 0.4.2: manila folder tabs standing on the rows' sheet (each opens its own page).
      const float h = st(28), size = st(10);
      float x = pa.x + st(6);
      auto folder_tab = [&](const std::string& label) {
        const float lw = kit::SpacedWidth(f.ui_bold, size, label, st(1.5f)), w = lw + st(44);
        const ImVec2 a(x, pa.y), b(x + w, pa.y + h);
        const bool over = ImGui::IsMouseHoveringRect(a, b);
        kit::FolderTab(d, a, b, st.u, false, over);
        kit::Spaced(d, f.ui_bold, size, ImVec2(a.x + (w - lw) * 0.5f, a.y + (h - size * 1.45f) * 0.5f), kit::kInk, label,
                    st(1.5f));
        x += w - st(3);
        return over && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && capture_.empty();
      };
      open_keys = folder_tab("REMAP");  // 0.4.2: one page, the controller's column beside the keyboard's and the mouse's
      pa.y += h;
    }
    if (graphics) {  // Apply settings and Restore defaults under the rows
      const float h = st(30), gap = st(8), w = (pb.x - pa.x - gap) * 0.5f;
      pb.y -= h + st(8);
      const size_t waiting = gfx_pending_.size();
      apply = dark_button(ImVec2(pa.x, pb.y + st(8)), ImVec2(pa.x + w, pb.y + st(8) + h),
                          waiting ? fmt::format("APPLY SETTINGS ({})", waiting) : std::string("APPLY SETTINGS"), waiting > 0);
      defaults = dark_button(ImVec2(pa.x + w + gap, pb.y + st(8)), ImVec2(pb.x, pb.y + st(8) + h), "RESTORE DEFAULTS", false);
    }
    SettingsBody(d, io, st, in, pa, pb, st.P(928, 167), st.P(1232, 640));
    // ---- the actions
    if (edit || in.view) return BeginAccount(false);
    if (controls && (open_keys || in.x)) sub_ = kSubKeys, row_ = 0, scroll_ = 0;
    else if (controls && open_remap) sub_ = kSubRemap, row_ = 0, scroll_ = 0;
    if (graphics && (apply || in.y)) ApplyGraphicsPending();
    else if (graphics && (defaults || (in.x && !preset_back))) RestoreGraphicsDefaults();
    else if (in.x && preset_back) PresetBack();

    if (in.back && !SubPageBack()) BackFromSettings();
  }
