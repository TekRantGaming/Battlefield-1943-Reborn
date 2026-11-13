  // ---- The pause menu's map (TekRantGaming, 13 Nov 2026: "the current pause menu doesn't have a map which makes
  // bombers hard to use"). The level's own map picture (the one the game's minimap and deploy screen draw: 1024 x
  // 1024 over 2048 m, +x to the left and +z up; shader_prep.cpp cuts it into cache\extracted\minimap), with:
  //   - the capture points as their lettered flags, each in its owner's colour (capture_points.h);
  //   - the icons the game's own minimap draws at this moment (the vehicles, the team, the bases: what the player
  //     may see and nothing more), from the game's icon atlas;
  //   - the player: an arrow where the rendered camera is, pointing where it looks (in an air raid that is the
  //     bombers' sight, so the arrow is the bombers).
  // X, or a click on the map, changes between the island and the whole battle with the carriers.
  // The map is turned as the game's own big maps are (its deploy screen's and its pause screen's: the HUD's minimap
  // states Spawn and Overview in hud_root_cq have CameraRotation -27): the level seen from above with +x to the right
  // and a little up, +z down and a little to the right. So a player finds the points where the deploy screen has them.
  bool pause_map_wide_ = false;

  void PauseMap(ImDrawList* d, ImGuiIO& io, const Stage& st, const Input& in, bool has_input) {
    const std::vector<CapturePoint> points = CapturePoints();
    if (points.empty()) return;  // no level's world yet, or one without capture points
    const kit::Fonts& f = kit::GetFonts();
    const std::string folder = LoadedLevelFolder();
    const ImVec2 a = st.P(436, 120), b = st.P(788, 534);
    kit::PaperArt(d, a, b);
    const float x0 = a.x + st(8), x1 = b.x - st(8), side = x1 - x0;
    float y = a.y + st(9);
    // The map's own axes (metres to the right and down), from the world's x and z, and back.
    static const float kCos = std::cos(27.0f * 3.14159265f / 180.0f), kSin = std::sin(27.0f * 3.14159265f / 180.0f);
    auto turned = [](float wx, float wz) { return ImVec2(wx * kCos + wz * kSin, -wx * kSin + wz * kCos); };
    auto world = [](ImVec2 m) { return ImVec2(m.x * kCos - m.y * kSin, m.x * kSin + m.y * kCos); };
    // What the map shows now: the points a team can take, or every point (the carriers and bases too).
    float lx0 = 1e9f, lx1 = -1e9f, ly0 = 1e9f, ly1 = -1e9f, ax0 = 1e9f, ax1 = -1e9f, ay0 = 1e9f, ay1 = -1e9f;
    int lettered = 0;
    for (const CapturePoint& p : points) {
      const ImVec2 m = turned(p.x, p.z);
      ax0 = std::min(ax0, m.x), ax1 = std::max(ax1, m.x), ay0 = std::min(ay0, m.y), ay1 = std::max(ay1, m.y);
      if (p.letter < 0) continue;
      lx0 = std::min(lx0, m.x), lx1 = std::max(lx1, m.x), ly0 = std::min(ly0, m.y), ly1 = std::max(ly1, m.y);
      ++lettered;
    }
    const bool can_zoom = lettered >= 2 && lettered < int(points.size());
    const bool wide = pause_map_wide_ || lettered < 2;
    const float cx = wide ? (ax0 + ax1) * 0.5f : (lx0 + lx1) * 0.5f, cy = wide ? (ay0 + ay1) * 0.5f : (ly0 + ly1) * 0.5f;
    float half = wide ? std::max(ax1 - ax0, ay1 - ay0) * 0.5f + 130.0f : std::max(lx1 - lx0, ly1 - ly0) * 0.5f + 120.0f;
    half = std::clamp(half, 220.0f, 1024.0f);
    // Never past the picture (it covers 1024 m each way from the level's middle): the view is drawn in from its
    // edge until its four corners are on the picture. What is then outside stays at the map's edge (below).
    for (int i = 0; i < 60; ++i) {
      float reach = 0;
      for (const ImVec2 corner : {ImVec2(-half, -half), ImVec2(half, -half), ImVec2(half, half), ImVec2(-half, half)}) {
        const ImVec2 w = world(ImVec2(cx + corner.x, cy + corner.y));
        reach = std::max(reach, std::max(std::fabs(w.x), std::fabs(w.y)));
      }
      if (reach <= 1022.0f || half <= 150.0f) break;
      half *= 0.97f;
    }
    // ---- the head: what the map shows, and the button that changes it
    kit::Spaced(d, f.typed, st(8), ImVec2(x0 + st(2), y), kit::kInkSection, wide ? "MAP \xC2\xB7 THE WHOLE BATTLE" : "MAP \xC2\xB7 THE ISLAND", st(2));
    if (can_zoom) {
      const std::string hint = wide ? "THE ISLAND" : "THE WHOLE BATTLE";
      const float tw = kit::SpacedWidth(f.typed, st(7.5f), hint, st(1.5f));
      const float gw = kit::Glyph(d, PromptStyle(), kit::Button::kX, KeyFor(kit::Button::kX), ImVec2(0, 0), st(14), false);
      const float gx = x1 - st(2) - tw - st(6) - gw;
      kit::Glyph(d, PromptStyle(), kit::Button::kX, KeyFor(kit::Button::kX), ImVec2(gx, y + st(4)), st(14));
      kit::Spaced(d, f.typed, st(7.5f), ImVec2(gx + gw + st(6), y), kit::kInkSection, hint, st(1.5f));
    }
    y += st(17);
    const ImVec2 m0(x0, y), m1(x1, y + side);
    const float scale = side / (2.0f * half);
    auto to_screen = [&](float wx, float wz) {
      const ImVec2 m = turned(wx, wz);
      return ImVec2(m0.x + (m.x - cx + half) * scale, m0.y + (m.y - cy + half) * scale);
    };
    // A mark outside the view stays at the map's edge, as the game's minimap keeps its flags at its rim.
    auto kept = [&](ImVec2 c, float margin) {
      return ImVec2(std::clamp(c.x, m0.x + margin, m1.x - margin), std::clamp(c.y, m0.y + margin, m1.y - margin));
    };
    // ---- the picture (the sea's own blue until it is in the cache)
    const fs::path dir = UserDir() / "cache" / "extracted" / "minimap";
    d->AddRectFilled(m0, m1, IM_COL32(27, 49, 74, 255));
    ImVec2 uv0, uv1;
    int pw = 0, ph = 0;
    if (!folder.empty() && kit::ImageFileUv(dir / (folder + ".png"), uv0, uv1, pw, ph) && pw > 1 && ph > 1) {
      const float eu = 0.5f / float(pw), ev = 0.5f / float(ph);  // never the atlas's neighbour at the picture's edge
      // The picture has the world's +x to its left and +z up, 2048 m across: each corner of the map's square, in the
      // world, is a place in it.
      auto at = [&](float mx, float my) {
        const ImVec2 w = world(ImVec2(cx + mx, cy + my));
        const float u = 0.5f - w.x / 2048.0f, v = 0.5f - w.y / 2048.0f;
        return ImVec2(uv0.x + (uv1.x - uv0.x) * std::clamp(u, eu, 1.0f - eu), uv0.y + (uv1.y - uv0.y) * std::clamp(v, ev, 1.0f - ev));
      };
      d->AddImageQuad(io.Fonts->TexRef, m0, ImVec2(m1.x, m0.y), m1, ImVec2(m0.x, m1.y), at(-half, -half), at(half, -half),
                      at(half, half), at(-half, half));
    }
    d->PushClipRect(m0, m1, true);
    // ---- the game's own map icons, then the capture points' flags over them
    ImVec2 iuv0, iuv1;
    int iw = 0, ih = 0;
    const bool icons = kit::ImageFileUv(dir / MapIconsFileName(), iuv0, iuv1, iw, ih) && iw == 512 && ih == 256;
    auto sprite = [&](float tx0, float ty0, float tx1, float ty1, ImVec2 c, float w, float h) {
      const ImVec2 s0(iuv0.x + (iuv1.x - iuv0.x) * tx0 / 512.0f, iuv0.y + (iuv1.y - iuv0.y) * ty0 / 256.0f);
      const ImVec2 s1(iuv0.x + (iuv1.x - iuv0.x) * tx1 / 512.0f, iuv0.y + (iuv1.y - iuv0.y) * ty1 / 256.0f);
      d->AddImage(io.Fonts->TexRef, ImVec2(std::round(c.x - w * 0.5f), std::round(c.y - h * 0.5f)),
                  ImVec2(std::round(c.x - w * 0.5f) + w, std::round(c.y - h * 0.5f) + h), s0, s1);
    };
    if (icons) {
      for (const MapIcon& i : MapIcons()) {
        if (i.flag) continue;  // the points' own flags are drawn below, larger and lettered
        const float tw = (i.u1 - i.u0) * 512.0f, th = (i.v1 - i.v0) * 256.0f;
        if (tw < 2 || th < 2 || tw > 80 || th > 80) continue;
        const float k = st(0.5f) * (tw > 40 ? 0.5f : 1.0f);  // the atlas's 64 texel pictures are drawn as its 32 ones
        sprite(i.u0 * 512.0f, i.v0 * 256.0f, i.u1 * 512.0f, i.v1 * 256.0f, to_screen(i.x, i.z), tw * k, th * k);
      }
    }
    // A point's colour: as the game's own maps show it now; else by its owner: blue the player's team, red the other
    // (before the team is known, blue the first team and red the second).
    const int team = comms::Team();
    auto kind_of = [team](const CapturePoint& p) {
      if (p.shown >= 0) return p.shown;
      return p.owner == 0 ? 0 : team > 0 ? (p.owner == team ? 1 : 2) : p.owner;
    };
    for (const CapturePoint& p : points) {
      if (p.letter < 0) continue;
      const int kind = kind_of(p);
      if (p.contested && int(ImGui::GetTime() * 4.0) % 2 == 1) continue;  // being taken: it blinks, as in the game
      const ImVec2 c = kept(to_screen(p.x, p.z), st(10));
      if (icons) {
        const FlagRect r = LetteredFlag(kind, p.letter);
        sprite(float(r.x), float(r.y), float(r.x + r.w), float(r.y + r.h), c, st(28), st(28));
      } else {  // the icon picture is not in the cache yet: a disc in the owner's colour with the letter
        const ImU32 fill = kind == 0 ? IM_COL32(236, 232, 220, 255) : kind == 1 ? IM_COL32(40, 96, 170, 255) : IM_COL32(176, 52, 40, 255);
        d->AddCircleFilled(c, st(9), fill, 24);
        d->AddCircle(c, st(9), IM_COL32(16, 18, 14, 255), 24, std::max(1.0f, st(1.2f)));
        const char letter[2] = {char('A' + p.letter), 0};
        const float lw = kit::TextWidth(f.ui_bold, st(11), letter);
        d->AddText(f.ui_bold, st(11), ImVec2(c.x - lw * 0.5f, c.y - st(11) * 0.72f), kind == 0 ? kit::kInk : IM_COL32(255, 255, 255, 255), letter);
      }
    }
    // ---- the player: where the camera is, and where it looks
    float pos[3], right[3], back[3];
    if (comms::Camera(pos, right, back)) {
      float dx = -back[0], dz = -back[2];
      if (std::hypot(dx, dz) < 0.35f) {  // looking steeply down (a bomb sight) or up: the top of the view leads
        const float ux = back[1] * right[2] - back[2] * right[1], uz = back[0] * right[1] - back[1] * right[0];
        const float sign = back[1] > 0 ? 1.0f : -1.0f;  // back points up when the camera looks down
        dx = ux * sign, dz = uz * sign;
      }
      const ImVec2 heading = turned(dx, dz);  // on the map
      const float len = std::hypot(heading.x, heading.y);
      const ImVec2 c = kept(to_screen(pos[0], pos[2]), st(7));
      if (len > 0.001f) {
        const ImVec2 dir(heading.x / len, heading.y / len), perp(-dir.y, dir.x);
        const float r = st(8);
        const ImVec2 tip(c.x + dir.x * r, c.y + dir.y * r);
        const ImVec2 l(c.x - dir.x * r * 0.72f + perp.x * r * 0.66f, c.y - dir.y * r * 0.72f + perp.y * r * 0.66f);
        const ImVec2 rr(c.x - dir.x * r * 0.72f - perp.x * r * 0.66f, c.y - dir.y * r * 0.72f - perp.y * r * 0.66f);
        const ImVec2 notch(c.x - dir.x * r * 0.3f, c.y - dir.y * r * 0.3f);
        const ImVec2 shape[4] = {tip, l, notch, rr};
        d->AddTriangleFilled(tip, l, notch, IM_COL32(122, 196, 70, 255));
        d->AddTriangleFilled(tip, notch, rr, IM_COL32(122, 196, 70, 255));
        d->AddPolyline(shape, 4, IM_COL32(12, 20, 8, 255), ImDrawFlags_Closed, std::max(1.0f, st(1.3f)));
      } else {
        d->AddCircleFilled(c, st(4), IM_COL32(122, 196, 70, 255), 16);
      }
    }
    d->PopClipRect();
    d->AddRect(m0, m1, kit::kInk, 0, 0, std::max(1.0f, st(1)));
    // ---- under the map: each point's letter in its owner's colour, and its name
    {
      float lx = x0 + st(2), ly = m1.y + st(8);
      const float size = st(9.5f), line = st(15);
      for (const CapturePoint& p : points) {
        if (p.letter < 0) continue;
        const int kind = kind_of(p);
        const char letter[2] = {char('A' + p.letter), 0};
        const std::string name = CapturePointTitle(p.name);
        const float w = st(15) + st(5) + kit::TextWidth(f.ui, size, name) + st(12);
        if (lx + w > x1 + st(2) && lx > x0 + st(2)) lx = x0 + st(2), ly += line;
        const ImVec2 c0(lx, ly), c1(lx + st(15), ly + st(12));
        d->AddRectFilled(c0, c1, kind == 0 ? IM_COL32(250, 248, 240, 255) : kind == 1 ? IM_COL32(40, 96, 170, 255) : kit::kRed);
        d->AddRect(c0, c1, kit::kInk, 0, 0, 1.0f);
        const float lw = kit::TextWidth(f.ui_bold, size, letter);
        d->AddText(f.ui_bold, size, ImVec2((c0.x + c1.x - lw) * 0.5f, ly + (st(12) - size * 1.45f) * 0.5f),
                   kind == 0 ? kit::kInk : IM_COL32(255, 255, 255, 255), letter);
        d->AddText(f.ui, size, ImVec2(c1.x + st(5), ly + (st(12) - size * 1.45f) * 0.5f), kit::kInk, name.c_str());
        lx += w;
      }
    }
    // ---- X, or a click on the map: the island, or the whole battle
    if (has_input && can_zoom && (in.x || (ImGui::IsMouseHoveringRect(m0, m1) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))) {
      pause_map_wide_ = !pause_map_wide_;
      REXLOG_INFO("BF pause: the map shows {}", pause_map_wide_ ? "the whole battle" : "the island");
    }
  }
