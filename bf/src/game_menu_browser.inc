// The server browser in the new look, Version A1 (TekRantGaming, 12 Nov 2026: "Build it"), included inside GameMenu
// (game_menu.cpp) after game_menu_home.inc. Four tabs (Official, Community, Favourites, Direct connect): the table on
// paper at the left (the chosen row green with the red tab), the chosen server's card at the right (its map's picture
// with the map's name in the game's own lettering, the chips, the description, Join and the favourite), the search
// box and the filter chips above. Every server is listed: until v0.4.0 only those of the movement the player chose,
// with a Classic or Modern question before the browser and a switch on it (TekRantGaming, 13 Nov 2026: "we are
// dropping support for modern and going full in on classic"). The mockup (runs\ui-designs, build2.py a1_browser) is
// 1920 x 1080; sizes here are its at 720 lines, on the Stage.

  // 13 Nov 2026: a LAN tab, the matches hosted on this network (Create Private Match, Network type LAN), found by
  // a broadcast their hosts answer (lan_backend.cpp).
  enum BrowserTab { kTabOfficial, kTabCommunity, kTabFavourites, kTabLan, kTabDirect, kBrowserTabCount };

  // The network asked for its LAN matches every 3 s while the tab shows: "BF43LAN?" to UDP 18250, broadcast and to
  // this PC itself; each answer ("BF43LAN!" and the match's line) is a match at the address it came from.
  struct LanScan {
    std::mutex mutex;
    std::vector<ServerInfo> found;
    std::atomic<bool> busy{false};
  };
  std::shared_ptr<LanScan> lan_scan_ = std::make_shared<LanScan>();
  double lan_scanned_at_ = -100;
  void LanScanTick(bool now) {
    const double t = ImGui::GetTime();
    if (lan_scan_->busy || (!now && t - lan_scanned_at_ < 3.0)) return;
    lan_scanned_at_ = t;
    lan_scan_->busy = true;
    std::thread([scan = lan_scan_] {
      std::vector<ServerInfo> found;
#if defined(_WIN32)
      const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      if (s != INVALID_SOCKET) {
        BOOL yes = TRUE;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
        DWORD wait_ms = 150;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&wait_ms), sizeof(wait_ms));
        for (const char* target : {"255.255.255.255", "127.0.0.1"}) {
          sockaddr_in to{};
          to.sin_family = AF_INET, to.sin_port = htons(18250);
          inet_pton(AF_INET, target, &to.sin_addr);
          sendto(s, "BF43LAN?", 8, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
        while (std::chrono::steady_clock::now() < until) {
          char in[2048];
          sockaddr_in from{};
          int n = sizeof(from);
          const int got = recvfrom(s, in, sizeof(in) - 1, 0, reinterpret_cast<sockaddr*>(&from), &n);
          if (got < 9 || std::memcmp(in, "BF43LAN!", 8) != 0) continue;
          // From this network only (private, link local or loopback addresses).
          const uint32_t from_ip = ntohl(from.sin_addr.s_addr);
          if (!((from_ip >> 24) == 10 || (from_ip >> 24) == 127 || (from_ip >> 20) == 0xAC1 || (from_ip >> 16) == 0xC0A8 ||
                (from_ip >> 16) == 0xA9FE))
            continue;
          const std::string line(in + 8, size_t(got - 8));
          char ip[32] = {};
          inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
          ServerInfo sv;
          sv.gid = JsonField(line, "gid"), sv.name = JsonField(line, "name"), sv.map = JsonField(line, "map");
          sv.players = std::atoi(JsonField(line, "players").c_str());
          sv.max_players = std::atoi(JsonField(line, "max_players").c_str());
          sv.tick_hz = std::atoi(JsonField(line, "tick").c_str());
          sv.next_map = JsonField(line, "next_map");
          sv.description = JsonField(line, "description"), sv.player_hosted = true;
          TakeDescriptionTags(sv);  // locked, with a password
          const std::string address = JsonField(line, "address");  // ":port" (the address is the answer's own)
          sv.address = std::string(ip) + address.substr(std::min(address.size(), address.rfind(':') == std::string::npos
                                                                                     ? address.size()
                                                                                     : address.rfind(':')));
          bool have = false;
          for (const ServerInfo& o : found) have = have || o.address == sv.address;
          // This PC's own match answers twice (the broadcast and the loopback): the network's address is kept.
          for (ServerInfo& o : found)
            if (o.gid == sv.gid && o.name == sv.name && o.address.rfind("127.", 0) == 0 && sv.address.rfind("127.", 0) != 0)
              o.address = sv.address, have = true;
          if (!have && !(sv.address.rfind("127.", 0) == 0 &&
                         std::any_of(found.begin(), found.end(), [&](const ServerInfo& o) { return o.gid == sv.gid && o.name == sv.name; })))
            found.push_back(std::move(sv));
        }
        closesocket(s);
      }
#endif
      std::lock_guard lock(scan->mutex);
      scan->found = std::move(found);
      scan->busy = false;
    }).detach();
  }
  std::vector<ServerInfo> LanServers() {
    std::lock_guard lock(lan_scan_->mutex);
    return lan_scan_->found;
  }

  // Before a server's name: a rack for a dedicated server, a person for a match hosted from a player's game.
  float HostIcon(ImDrawList* d, const Stage& st, ImVec2 at, float h, bool player, ImU32 ink) {
    const float w = st(13), cy = at.y + h * 0.5f;
    if (player) {
      d->AddCircleFilled(ImVec2(at.x + w * 0.5f, cy - st(3.2f)), st(2.7f), ink, 16);
      d->PathArcTo(ImVec2(at.x + w * 0.5f, cy + st(5.6f)), st(5.2f), 3.14159265f, 6.2831853f, 16);
      d->PathFillConvex(ink);
    } else {
      for (int k = 0; k < 3; ++k) {
        const float y = cy - st(6) + float(k) * st(4.3f);
        d->AddRectFilled(ImVec2(at.x, y), ImVec2(at.x + w, y + st(3.3f)), ink);
        d->AddRectFilled(ImVec2(at.x + st(1.6f), y + st(1.1f)), ImVec2(at.x + st(2.8f), y + st(2.3f)), IM_COL32(120, 190, 110, 255));
      }
    }
    return w + st(7);
  }
  bool browser_has_players_ = false, browser_not_full_ = false, browser_night_ = false;  // the filter chips
  std::string browser_region_;       // the region chip: only this region's servers (a code of regions.h); empty: all
  std::string browser_search_;       // the search box: a part of a server's name or map
  bool browser_typing_ = false;      // the search box has the keyboard (its letters are letters, not the prompts' keys)
  bool servers_from_pause_ = false;  // opened from the pause (a match running): Back goes there
  double browser_click_at_ = -100;   // the last click on a row: a second on the same row within 0.45 s joins
  int browser_click_row_ = -1;
  std::string browser_note_;         // "REFRESHING THE LIST", "KEPT IN FAVOURITES": under the table for a moment
  double browser_note_at_ = -100;

  static std::string Lower(std::string t) {
    for (char& c : t) c = char(std::tolower(static_cast<unsigned char>(c)));
    return t;
  }
  void BrowserNote(const std::string& t) { browser_note_ = t, browser_note_at_ = ImGui::GetTime(); }

  // ---- the favourites: the servers' addresses (the same across restarts; the gid is the backend's), in the settings
  static std::string ServerKey(const ServerInfo& sv) { return sv.address.empty() ? sv.gid : sv.address; }
  std::vector<std::string> Favourites() const {
    std::vector<std::string> out;
    const std::string all = settings_.Get("favourite_servers", "");
    for (size_t at = 0; at < all.size();) {
      const size_t end = std::min(all.find(';', at), all.size());
      if (end > at) out.push_back(all.substr(at, end - at));
      at = end + 1;
    }
    return out;
  }
  bool IsFavourite(const ServerInfo& sv) const {
    const std::vector<std::string> favs = Favourites();
    return std::find(favs.begin(), favs.end(), ServerKey(sv)) != favs.end();
  }
  void ToggleFavourite(const ServerInfo& sv) {
    std::vector<std::string> favs = Favourites();
    const std::string key = ServerKey(sv);
    const auto it = std::find(favs.begin(), favs.end(), key);
    const bool add = it == favs.end();
    if (add) favs.push_back(key);
    else favs.erase(it);
    std::string all;
    for (const std::string& k : favs) all += (all.empty() ? "" : ";") + k;
    settings_.values["favourite_servers"] = all.substr(0, 1000);
    settings_.Save();
    BrowserNote(add ? "KEPT IN THE FAVOURITES" : "TAKEN OUT OF THE FAVOURITES");
    REXLOG_INFO("BF browser: {} {} ({})", add ? "a favourite:" : "no longer a favourite:", sv.name, key);
  }

  // The tab's servers, through the search and the chips, the busiest first (or the nearest); counts[]: the Official,
  // Community and Favourites totals before the search and the chips.
  std::vector<ServerInfo> BrowserServers(const std::vector<ServerInfo>& all, int counts[3]) {
    const std::vector<std::string> favs = Favourites();
    const std::string search = Lower(browser_search_);
    std::vector<ServerInfo> out;
    for (const ServerInfo& sv : all) {
      const bool fav = std::find(favs.begin(), favs.end(), ServerKey(sv)) != favs.end();
      ++counts[sv.official ? 0 : 1];
      if (fav) ++counts[2];
      if (browser_tab_ == kTabFavourites ? !fav : sv.official != (browser_tab_ == kTabOfficial)) continue;
      if (browser_has_players_ && sv.players <= 0) continue;
      if (browser_not_full_ && sv.max_players > 0 && sv.players >= sv.max_players) continue;
      if (browser_night_ && !sv.night) continue;
      if (!browser_region_.empty() && sv.region != browser_region_) continue;
      if (!search.empty() && Lower(sv.name + " " + sv.map).find(search) == std::string::npos) continue;
      out.push_back(sv);
    }
    std::stable_sort(out.begin(), out.end(), [this](const ServerInfo& p, const ServerInfo& q) {
      if (sort_by_ping_) {
        const int a = p.ping_ms < 0 ? 1 << 20 : p.ping_ms, b = q.ping_ms < 0 ? 1 << 20 : q.ping_ms;
        if (a != b) return a < b;
      }
      return p.players > q.players;
    });
    return out;
  }

  // A small label on a tape (`fill` and `ink`; a border on the paper ones); returns its width, `clicked` the mouse.
  float ChipWidth(const Stage& st, const std::string& t) {
    return kit::SpacedWidth(kit::GetFonts().ui_bold, st(9.25f), t, st(1.2f)) + st(18);
  }
  float Chip(ImDrawList* d, const Stage& st, ImVec2 at, float h, const std::string& t, ImU32 fill, ImU32 ink, bool border,
             bool* clicked = nullptr) {
    const kit::Fonts& f = kit::GetFonts();
    const float size = st(9.25f), track = st(1.2f);
    const float w = ChipWidth(st, t);
    const ImVec2 b(at.x + w, at.y + h);
    d->AddRectFilled(at, b, fill);
    if (border) d->AddRect(at, b, kit::kBorder, 0, 0, 1.0f);
    kit::Spaced(d, f.ui_bold, size, ImVec2(at.x + st(9), at.y + (h - size * 1.45f) * 0.5f), ink, t, track);
    if (clicked) *clicked = ImGui::IsMouseHoveringRect(at, b) && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    return w;
  }

  // Four bars at `at` (left, middle): green under 60 ms, amber under 120, red above; not measured: all faint.
  float PingBars(ImDrawList* d, const Stage& st, ImVec2 at, int ping_ms, bool on_green) {
    const int bars = ping_ms < 0 ? 0 : ping_ms < 60 ? 4 : ping_ms < 120 ? 3 : ping_ms < 200 ? 2 : 1;
    const ImU32 on = on_green ? kit::kCream : ping_ms < 60 ? kit::kGreen : ping_ms < 120 ? IM_COL32(190, 150, 30, 255) : kit::kRed;
    const ImU32 off = on_green ? IM_COL32(255, 255, 255, 80) : IM_COL32(34, 36, 27, 45);
    for (int k = 0; k < 4; ++k) {
      const float x = at.x + float(k) * st(4.5f), h = st(4 + 2.5f * float(k)), base = at.y + st(6);
      d->AddRectFilled(ImVec2(x, base - h), ImVec2(x + st(3), base), k < bars ? on : off);
    }
    return st(20);
  }

  // Back where the browser was opened from: the pause (a match running), else the menu (Home before the game runs).
  void BackFromBrowser() {
    browser_typing_ = false, direct_keys_ = false;
    if (servers_from_pause_) servers_from_pause_ = false, page_ = Page::kPause;
    else page_ = pre_game_ ? Page::kHome : Page::kMain;
    servers_from_home_ = false;
    home_row_ = kRowBrowser;
  }

  void BrowserPage(ImDrawList* d, ImGuiIO& io, Input in) {
    NotePage("server browser");
    UpdateTick();
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    const ImVec2 sz = io.DisplaySize;
    MenuBackdrop(d, sz, st);
    list_.WantPings();
    bool online_ok = false;
    const std::string status = OnlineStatus(online_ok);
    const bool edit = MenuChrome(d, io, st, status, online_ok);
    // ---- the tabs: LB and RB (Q and E), or a click
    browser_tab_ = std::clamp(browser_tab_, 0, kBrowserTabCount - 1);
    if (in.prev_tab) browser_tab_ = (browser_tab_ + kBrowserTabCount - 1) % kBrowserTabCount;
    if (in.next_tab) browser_tab_ = (browser_tab_ + 1) % kBrowserTabCount;
    if (in.prev_tab || in.next_tab) row_ = 0, scroll_ = 0, direct_keys_ = false, browser_typing_ = false;
    const bool direct = browser_tab_ == kTabDirect;
    // ---- the search box's keyboard: letters into it; Enter or Esc out of it
    if (browser_typing_ && !direct) {
      ImGuiContext& g = *ImGui::GetCurrentContext();
      g.PlatformImeData.WantTextInput = true;
      g.PlatformImeData.ViewportId = ImGui::GetMainViewport()->ID;
      for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const ImWchar c = io.InputQueueCharacters[i];
        if (c >= 32 && c < 127 && browser_search_.size() < 40) browser_search_ += char(c), row_ = 0, scroll_ = 0;
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !browser_search_.empty()) browser_search_.pop_back(), row_ = 0;
      if (in.accept || in.back) browser_typing_ = false;
      in.accept = in.back = in.left = in.right = in.x = in.y = false;
    }
    const std::vector<ServerInfo> all = list_.servers();
    int counts[3] = {0, 0, 0};
    const bool lan = browser_tab_ == kTabLan;
    if (lan) LanScanTick(in.x);
    const std::vector<ServerInfo> lan_found = LanServers();
    const std::vector<ServerInfo> listed = BrowserServers(all, counts);
    const std::vector<ServerInfo>& servers = lan ? lan_found : listed;
    // ---- the title block
    {
      const int players = list_.players_online();
      const std::string eyebrow =
          !online_ok ? "PACIFIC THEATRE \xC2\xB7 THE SERVER LIST IS NOT REACHABLE RIGHT NOW"
                     : fmt::format("PACIFIC THEATRE \xC2\xB7 {} {} \xC2\xB7 {} {} ONLINE", all.size(),
                                   all.size() == 1 ? "SERVER" : "SERVERS", players, players == 1 ? "PLAYER" : "PLAYERS");
      kit::Spaced(d, f.typed, st(10.5f), st.P(48, 91), kit::kKhakiDim, eyebrow, st(2));
      kit::GameText(d, kit::GameFont::kHeadline, st(40), st.P(48, 105), kit::kCream, "SERVER BROWSER");
    }
    {
      static const char* kTabNames[kBrowserTabCount] = {"Official", "Community", "Favourites", "LAN", "Direct connect"};
      // 0.4.2: manila folder tabs; the open one in front, the others behind it and a little lower.
      const float y0 = st.P(0, 170).y, h = st(34), size = st(13);  // standing on the table's sheet (204)
      struct TabBox {
        ImVec2 a, b;
        float lw, cw;
        std::string count;
      };
      std::vector<TabBox> boxes;
      float x = st.P(48, 0).x;
      for (int t = 0; t < kBrowserTabCount; ++t) {
        const float lw = kit::TextWidth(f.ui_bold, size, kTabNames[t]);
        const std::string count = t < 3 ? std::to_string(counts[t]) : t == kTabLan && lan ? std::to_string(lan_found.size()) : "";
        const float cw = count.empty() ? 0 : kit::TextWidth(f.ui_bold, st(9.25f), count) + st(10);
        const float w = st(22) + lw + (count.empty() ? st(22) : st(10) + cw + st(14));
        boxes.push_back({ImVec2(x, y0), ImVec2(x + w, y0 + h), lw, cw, count});
        x += w - st(3);  // each a little over the next
      }
      auto draw_tab = [&](int t) {
        const bool cur = t == browser_tab_;
        const TabBox& box = boxes[t];
        const ImVec2 a(box.a.x, box.a.y + (cur ? 0 : st(4))), b2 = box.b;
        const bool over = !cur && ImGui::IsMouseHoveringRect(a, b2);
        kit::FolderTab(d, a, b2, st.u, cur, over);
        const float th = b2.y - a.y;
        d->AddText(f.ui_bold, size, ImVec2(a.x + st(22), a.y + (th - size * 1.45f) * 0.5f),
                   cur ? kit::kInk : IM_COL32(34, 36, 27, 215), kTabNames[t]);
        if (!box.count.empty()) {
          const ImVec2 c0(a.x + st(22) + box.lw + st(10), a.y + (th - st(15)) * 0.5f), c1(c0.x + box.cw, c0.y + st(15));
          d->AddRectFilled(c0, c1, cur ? kit::kStrip : IM_COL32(34, 36, 27, 60));
          d->AddText(f.ui_bold, st(8), ImVec2(c0.x + st(5), c0.y + (st(15) - st(8) * 1.45f) * 0.5f),
                     cur ? kit::kStripText : kit::kInk, box.count.c_str());
        }
        if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
          browser_tab_ = t, row_ = 0, scroll_ = 0, direct_keys_ = false, browser_typing_ = false;
      };
      const int front = browser_tab_;
      for (int t = kBrowserTabCount - 1; t >= 0; --t)
        if (t != front) draw_tab(t);
      draw_tab(front);
    }
    // ---- the prompts (drawn first: a click on one is that button's press for the rest of the frame)
    std::vector<Cue> cues;
    if (!direct)
      cues = {{kit::Button::kA, "JOIN"}, {kit::Button::kX, "REFRESH"}, {kit::Button::kY, "FAVOURITE"},
              {kit::Button::kLB, "TABS"}, {kit::Button::kB, "BACK"}};
    else if (direct_keys_)
      cues = {{kit::Button::kA, "TYPE"}, {kit::Button::kX, "DELETE"}, {kit::Button::kStart, "CONNECT", "Enter"},
              {kit::Button::kB, "DONE TYPING"}};
    else
      cues = {{kit::Button::kA, "TYPE"}, {kit::Button::kStart, "CONNECT", "Enter"}, {kit::Button::kLB, "TABS"},
              {kit::Button::kB, "BACK"}};
    Prompts(d, st, st.P(48, 662), cues, in, 15);
    MenuFoot(d, st);
    const ImVec2 ta = st.P(48, 204), tb = st.P(872, 640), pa = st.P(887, 204), pb = st.P(1232, 640);
    if (direct) {
      DirectPanel(d, io, st, in, ta, tb, pa, pb);
      if (edit || in.view) BeginAccount(false);
      return;
    }
    // ---- the filter chips from the right, then the search box up to them
    {
      const float cy = st.P(0, 166).y, ch = st(20);
      float cx = st.P(1232, 0).x;
      auto chip_right = [&](const std::string& t, bool on, bool* clicked) {
        cx -= ChipWidth(st, t);
        Chip(d, st, ImVec2(cx, cy), ch, t, on ? kit::kStrip : kit::kPaper, on ? kit::kStripText : kit::kInk, !on, clicked);
        cx -= st(8);
      };
      bool c_sort = false, c_night = false, c_full = false, c_players = false, c_region = false;
      chip_right(sort_by_ping_ ? "SORT: PING" : "SORT: PLAYERS", false, &c_sort);
      if (kNightOps) chip_right("NIGHT OPS", browser_night_, &c_night);  // night_ops.h: not in every build
      chip_right("NOT FULL", browser_not_full_, &c_full);
      chip_right("HAS PLAYERS", browser_has_players_, &c_players);
      // The region chip (TekRantGaming, 13 Nov 2026: servers say their region): a click goes through the regions
      // the listed servers are in, then back to all of them.
      std::vector<std::string> regions;
      for (const ServerInfo& sv : all)
        if (!sv.region.empty() && std::find(regions.begin(), regions.end(), sv.region) == regions.end()) regions.push_back(sv.region);
      std::sort(regions.begin(), regions.end());
      if (!browser_region_.empty() && std::find(regions.begin(), regions.end(), browser_region_) == regions.end()) browser_region_.clear();
      if (!regions.empty()) {
        const Region* r = FindRegion(browser_region_);
        chip_right("REGION: " + (browser_region_.empty() ? std::string("ALL") : Upper(r ? r->name : browser_region_)), !browser_region_.empty(),
                   &c_region);
      }
      if (c_region) {
        const auto at = std::find(regions.begin(), regions.end(), browser_region_);
        browser_region_ = browser_region_.empty() ? regions.front() : at + 1 == regions.end() ? std::string() : *(at + 1);
      }
      if (c_sort) sort_by_ping_ = !sort_by_ping_;
      if (c_night) browser_night_ = !browser_night_;
      if (c_full) browser_not_full_ = !browser_not_full_;
      if (c_players) browser_has_players_ = !browser_has_players_;
      if (c_sort || c_night || c_full || c_players || c_region) row_ = 0, scroll_ = 0;
      const ImVec2 s0 = st.P(679, 159), s1(cx - st(2), st.P(0, 193).y);
      const bool typing = browser_typing_;
      d->AddRectFilled(s0, s1, typing ? kit::kPaperLight : kit::kPaper);
      d->AddRect(s0, s1, typing ? kit::kGreen : kit::kBorder, 0, 0, typing ? st(2) : 1.0f);
      const ImVec2 mc(s0.x + st(16), (s0.y + s1.y) * 0.5f - st(1.5f));  // the magnifier
      d->AddCircle(mc, st(4.5f), kit::kInkSection, 16, st(1.6f));
      d->AddLine(ImVec2(mc.x + st(3.3f), mc.y + st(3.3f)), ImVec2(mc.x + st(7.5f), mc.y + st(7.5f)), kit::kInkSection, st(2));
      const float size = st(13.75f);
      const std::string shown = browser_search_.empty() ? (typing ? "" : "Search servers") : browser_search_;
      const ImVec2 tp(s0.x + st(30), s0.y + (s1.y - s0.y - size * 1.45f) * 0.5f);
      d->AddText(f.ui_bold, size, tp, browser_search_.empty() ? kit::kInkSection : kit::kInk, shown.c_str());
      if (typing && std::fmod(ImGui::GetTime(), 1.0) < 0.6) {
        const float x = tp.x + kit::TextWidth(f.ui_bold, size, browser_search_) + st(1);
        d->AddRectFilled(ImVec2(x, s0.y + st(9)), ImVec2(x + st(1.5f), s1.y - st(9)), kit::kInk);
      }
      bool cleared = false;
      if (!browser_search_.empty()) {  // the cross clears it
        const ImVec2 x0(s1.x - st(26), s0.y), x1 = s1;
        const ImVec2 c((x0.x + x1.x) * 0.5f, (x0.y + x1.y) * 0.5f);
        d->AddLine(ImVec2(c.x - st(4), c.y - st(4)), ImVec2(c.x + st(4), c.y + st(4)), kit::kInkSection, st(1.6f));
        d->AddLine(ImVec2(c.x - st(4), c.y + st(4)), ImVec2(c.x + st(4), c.y - st(4)), kit::kInkSection, st(1.6f));
        cleared = ImGui::IsMouseHoveringRect(x0, x1) && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
      }
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) browser_typing_ = ImGui::IsMouseHoveringRect(s0, s1) && !cleared;
      if (cleared) browser_search_.clear(), row_ = 0, scroll_ = 0;
    }
    // ---- the table
    const float head = st(30), rh = st(36);
    kit::PaperArt(d, ta, tb);
    // (The region with its flag and the tick rate: TekRantGaming, 13 Nov 2026.)
    const float colx[6] = {st(14), st(296), st(432), st(566), st(676), st(736)};
    static const char* kHeads[6] = {"SERVER", "MAP", "REGION", "PLAYERS", "TICK", "PING"};
    for (int c = 0; c < 6; ++c)
      kit::GameText(d, kit::GameFont::kListHeader, st(10), ImVec2(ta.x + colx[c], ta.y + (head - st(10)) * 0.5f), kit::kInkFaint,
                    kHeads[c], st(0.6f));
    d->AddRectFilled(ImVec2(ta.x, ta.y + head - st(2)), ImVec2(tb.x, ta.y + head), kit::kInk);
    const int n = int(servers.size());
    if (n) {
      if (in.up) row_ = (row_ + n - 1) % n;
      if (in.down) row_ = (row_ + 1) % n;
    }
    row_ = std::clamp(row_, 0, std::max(0, n - 1));
    const float top = ta.y + head, area = tb.y - top, rows_h = float(n) * rh;
    if (ImGui::IsMouseHoveringRect(ta, tb) && io.MouseWheel != 0) scroll_ -= io.MouseWheel * rh * 2;
    if (in.up || in.down) {  // the chosen row kept in view
      const float ry = float(row_) * rh;
      if (ry < scroll_) scroll_ = ry;
      if (ry + rh > scroll_ + area) scroll_ = ry + rh - area;
    }
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, rows_h - area));
    d->PushClipRect(ImVec2(ta.x, top), tb, true);
    int clicked = -1;
    bool join = false;
    for (int i = 0; i < n; ++i) {
      const ServerInfo& sv = servers[size_t(i)];
      const float y = top + float(i) * rh - scroll_;
      if (y + rh < top || y > tb.y) continue;
      const ImVec2 r0(ta.x, y), r1(tb.x, y + rh);
      const bool sel = i == row_;
      const bool hover = ImGui::IsMouseHoveringRect(r0, r1) && ImGui::IsMouseHoveringRect(ImVec2(ta.x, top), tb);
      const bool full = sv.max_players > 0 && sv.players >= sv.max_players;
      if (sel) {
        d->AddRectFilled(r0, r1, kit::kGreen);
        d->AddRectFilled(r0, ImVec2(r0.x + st(5), r1.y), kit::kStamp);
      } else if (hover) {
        d->AddRectFilled(r0, r1, IM_COL32(34, 36, 27, 16));
      }
      d->AddLine(ImVec2(ta.x, r1.y - 0.5f), ImVec2(tb.x, r1.y - 0.5f), IM_COL32(34, 36, 27, 30), 1.0f);
      const ImU32 ink = sel ? kit::kCream : full ? kit::kInkSection : kit::kInk;
      {  // the name, and NIGHT
        const float cap = st(12);
        d->PushClipRect(ImVec2(ta.x, top), ImVec2(ta.x + colx[1] - st(10), tb.y), true);
        float iw = HostIcon(d, st, ImVec2(ta.x + colx[0], y), rh, sv.player_hosted, ink);
        if (sv.locked) {  // a padlock: the match has a password, asked for on joining
          const float lx = ta.x + colx[0] + iw, cy = y + rh * 0.5f, w = st(9);
          d->AddRectFilled(ImVec2(lx, cy - st(1)), ImVec2(lx + w, cy + st(6)), ink);
          d->PathArcTo(ImVec2(lx + w * 0.5f, cy - st(1)), st(3), 3.14159265f, 6.2831853f, 12);
          d->PathStroke(ink, 0, st(1.6f));
          iw += w + st(7);
        }
        {  // the server's tick rate, after the icon: your game takes it when it joins
          const std::string hz = std::to_string(sv.Tick()) + " HZ";
          const float hs = st(7.5f), hw = kit::SpacedWidth(f.ui_bold, hs, hz, st(1)) + st(10), hh = st(14);
          const ImVec2 h0(ta.x + colx[0] + iw, y + (rh - hh) * 0.5f), h1(h0.x + hw, h0.y + hh);
          const bool sixty = sv.Tick() >= 60;  // 60 and the experimental 75 and 100: green
          d->AddRectFilled(h0, h1, sel ? IM_COL32(255, 255, 255, 40) : sixty ? IM_COL32(74, 112, 58, 255) : IM_COL32(34, 36, 27, 40));
          kit::Spaced(d, f.ui_bold, hs, ImVec2(h0.x + st(5), h0.y + (hh - hs * 1.45f) * 0.5f),
                      sel || sixty ? kit::kCream : kit::kInkSection, hz, st(1));
          iw += hw + st(8);
        }
        const float nw = iw + kit::GameText(d, kit::GameFont::kText, cap, ImVec2(ta.x + colx[0] + iw, y + (rh - cap) * 0.5f), ink, sv.name);
        if (sv.night && kNightOps)
          Chip(d, st, ImVec2(ta.x + colx[0] + nw + st(12), y + (rh - st(16)) * 0.5f), st(16), "NIGHT", IM_COL32(40, 46, 88, 255),
               kit::kStripText, false);
        d->PopClipRect();
      }
      {  // the map: its picture and its name
        const float x = ta.x + colx[1];
        MapPicture(d, st.u * 0.5f, ImVec2(x, y + st(8)), ImVec2(x + st(34), y + rh - st(8)), sv.map);
        kit::GameText(d, kit::GameFont::kText, st(11), ImVec2(x + st(44), y + (rh - st(11)) * 0.5f), ink,
                      sv.map.empty() ? "Wake Island" : sv.map);
      }
      {  // the region: its country's flag and its name (an older server says none)
        const float x = ta.x + colx[2];
        const Region* r = FindRegion(sv.region);
        d->PushClipRect(ImVec2(x, std::max(y, top)), ImVec2(ta.x + colx[3] - st(8), std::min(y + rh, tb.y)), true);
        if (r) {
          DrawFlag(d, r->country, ImVec2(x, y + rh * 0.5f - st(7)), ImVec2(x + st(21), y + rh * 0.5f + st(7)));
          kit::GameText(d, kit::GameFont::kText, st(11), ImVec2(x + st(28), y + (rh - st(11)) * 0.5f), ink, r->name);
        } else {
          kit::GameText(d, kit::GameFont::kText, st(11), ImVec2(x, y + (rh - st(11)) * 0.5f), sel ? kit::kCream : kit::kInkSection,
                        sv.region.empty() ? std::string("?") : sv.region);
        }
        d->PopClipRect();
      }
      {  // the players: a bar and the count
        const float x = ta.x + colx[3], bw = st(42);
        const ImVec2 b0(x, y + rh * 0.5f - st(2)), b1(x + bw, y + rh * 0.5f + st(2));
        d->AddRectFilled(b0, b1, sel ? IM_COL32(255, 255, 255, 70) : IM_COL32(34, 36, 27, 30));
        const float fill = sv.max_players > 0 ? std::clamp(float(sv.players) / float(sv.max_players), 0.0f, 1.0f) : 0.0f;
        if (fill > 0) d->AddRectFilled(b0, ImVec2(b0.x + bw * fill, b1.y), sel ? kit::kCream : kit::kGreen);
        kit::GameText(d, kit::GameFont::kText, st(11), ImVec2(x + bw + st(10), y + (rh - st(11)) * 0.5f), ink,
                      fmt::format("{} / {}", sv.players, sv.max_players));
      }
      // the tick rate (an older server says none)
      kit::GameText(d, kit::GameFont::kText, st(11), ImVec2(ta.x + colx[4], y + (rh - st(11)) * 0.5f), ink,
                    sv.tick_hz > 0 ? fmt::format("{} Hz", sv.tick_hz) : std::string("?"));
      {  // the ping
        const float x = ta.x + colx[5];
        const float pw = PingBars(d, st, ImVec2(x, y + rh * 0.5f), sv.ping_ms, sel);
        kit::GameText(d, kit::GameFont::kText, st(11), ImVec2(x + pw + st(6), y + (rh - st(11)) * 0.5f), ink,
                      sv.ping_ms < 0 ? "?" : fmt::format("{} ms", sv.ping_ms));
      }
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const double now = ImGui::GetTime();
        if (browser_click_row_ == i && now - browser_click_at_ < 0.45) join = true;  // a double click
        browser_click_row_ = i, browser_click_at_ = now, clicked = i;
      }
    }
    d->PopClipRect();
    if (clicked >= 0) row_ = clicked;
    if (!n) {
      std::string why;
      if (lan)
        why = "No match found on your network yet: it is asked every 3 seconds. A friend hosts one with Create Private "
              "Match, Network type LAN; their PC's firewall has to let the game answer.";
      else if (!online_ok) why = "The server list is not reachable right now. It is tried again every few seconds.";
      else if (!browser_search_.empty()) why = fmt::format("Nothing matches \"{}\".", browser_search_);
      else if (browser_tab_ == kTabFavourites)
        why = fmt::format("No favourites yet. {} on a server keeps it here.",
                          device_ == Device::kKeyboard ? KeyFor(kit::Button::kY) : "Y");
      else if (browser_has_players_ || browser_not_full_ || browser_night_) why = "No server passes the filters. Switch one off.";
      else
        why = fmt::format("No {} server right now. The list refreshes every 5 seconds.",
                          browser_tab_ == kTabOfficial ? "official" : "community");
      kit::Wrapped(d, f.ui, st(13.75f), ImVec2(ta.x + st(20), top + st(20)), kit::kInkFaint, why, tb.x - ta.x - st(40));
    }
    if (ImGui::GetTime() - browser_note_at_ < 2.5)
      kit::Spaced(d, f.typed, st(10), ImVec2(ta.x, tb.y + st(8)), kit::kKhakiText, browser_note_, st(2));
    // ---- the chosen server's card: its map, the chips, the name, the description, Join, the favourite
    kit::PaperArt(d, pa, pb);
    const ServerInfo* sv = n ? &servers[size_t(row_)] : nullptr;
    const ImVec2 p1(pb.x, pa.y + st(166));
    d->PushClipRect(pa, p1, true);
    MapPicture(d, st.u, pa, p1, sv ? sv->map : "Wake Island");
    d->PopClipRect();
    d->AddRectFilledMultiColor(ImVec2(pa.x, p1.y - st(84)), p1, IM_COL32(10, 12, 8, 0), IM_COL32(10, 12, 8, 0),
                               IM_COL32(10, 12, 8, 210), IM_COL32(10, 12, 8, 210));
    kit::GameText(d, kit::GameFont::kHeadline, st(26), ImVec2(pa.x + st(18), p1.y - st(46)), kit::kCream,
                  Upper(sv ? (sv->map.empty() ? "Wake Island" : sv->map) : "Wake Island"));
    const bool fav = sv && IsFavourite(*sv);
    if (!sv) {
      kit::Spaced(d, f.typed, st(10), ImVec2(pa.x + st(18), p1.y + st(18)), kit::kInkSection, "NO SERVER CHOSEN", st(2));
      kit::Wrapped(d, f.ui, st(12.75f), ImVec2(pa.x + st(18), p1.y + st(36)), kit::kInkFaint,
                   "Pick a server in the table to see it here.", pb.x - pa.x - st(36));
    } else {
      const bool full = sv->max_players > 0 && sv->players >= sv->max_players;
      const ImVec2 j0(pa.x + st(18), pb.y - st(18) - st(40)), j1(pb.x - st(18) - st(40) - st(8), pb.y - st(18));
      d->PushClipRect(pa, ImVec2(pb.x, j0.y - st(10)), true);
      float x = pa.x + st(18), y = p1.y + st(16);
      x += Chip(d, st, ImVec2(x, y), st(18), sv->official ? "OFFICIAL" : "COMMUNITY", kit::kStrip, kit::kStripText, false) + st(6);
      if (sv->night && kNightOps)
        x += Chip(d, st, ImVec2(x, y), st(18), "NIGHT OPS", IM_COL32(40, 46, 88, 255), kit::kStripText, false) + st(6);
      if (fav) Chip(d, st, ImVec2(x, y), st(18), "FAVOURITE", kit::kGold, kit::kInk, false);
      y += st(18) + st(14);
      kit::GameText(d, kit::GameFont::kText, st(13), ImVec2(pa.x + st(18), y), kit::kInk, sv->name);
      y += st(13) + st(10);
      std::string line = fmt::format("{} / {} PLAYERS", sv->players, sv->max_players);
      if (sv->ping_ms >= 0) line += fmt::format(" \xC2\xB7 {} MS", sv->ping_ms);
      if (full) line += " \xC2\xB7 FULL";
      kit::Spaced(d, f.typed, st(9.5f), ImVec2(pa.x + st(18), y), full ? kit::kStamp : kit::kInkSection, line, st(1.5f));
      y += st(20);
      const std::string desc = !sv->description.empty() ? sv->description
                               : sv->official              ? "An official server, run by TRG Studios."
                                                           : "A community server.";
      y += kit::Wrapped(d, f.ui, st(12), ImVec2(pa.x + st(18), y), kit::kInk, desc, pb.x - pa.x - st(36)) + st(12);
      auto row = [&](const char* k, const std::string& v) {
        d->AddLine(ImVec2(pa.x + st(18), y), ImVec2(pb.x - st(18), y), kit::kRule, 1.0f);
        y += st(7);
        d->AddText(f.ui, st(12), ImVec2(pa.x + st(18), y), kit::kInkFaint, k);
        const float vw = kit::TextWidth(f.ui_bold, st(12), v);
        d->AddText(f.ui_bold, st(12), ImVec2(pb.x - st(18) - vw, y), kit::kInk, v.c_str());
        y += st(24);
      };
      {  // where it is, with the flag before the name
        const Region* r = FindRegion(sv->region);
        const float flag_y = y + st(7);
        row("Region", r ? r->name : sv->region.empty() ? "Not said" : sv->region);
        if (r) {
          const float vw = kit::TextWidth(f.ui_bold, st(12), r->name);
          const ImVec2 f0(pb.x - st(18) - vw - st(27), flag_y + st(1));
          DrawFlag(d, r->country, f0, ImVec2(f0.x + st(20), f0.y + st(13)));
        }
      }
      row("Tick rate", sv->tick_hz > 0 ? fmt::format("{} Hz", sv->tick_hz) : std::string("Not said"));
      row("Next map", sv->next_map.empty() ? "Not announced" : sv->next_map);
      row("Run by", sv->official ? "TRG Studios" : "A player");
      d->PopClipRect();
      if (full) {
        d->AddRectFilled(j0, j1, kit::kPaperDim);
        d->AddRect(j0, j1, kit::kBorder, 0, 0, 1.0f);
        kit::GameText(d, kit::GameFont::kText, st(14), ImVec2(j0.x + st(14), j0.y + (st(40) - st(14)) * 0.5f), kit::kInkSection,
                      "FULL");
      } else if (GreenButton(d, st, j0, j1, kit::Button::kA, "JOIN")) {
        join = true;
      }
      const ImVec2 y0(j1.x + st(8), j0.y), y1(pb.x - st(18), j1.y);
      const bool yh = ImGui::IsMouseHoveringRect(y0, y1);
      d->AddRectFilled(y0, y1, fav ? kit::kGold : yh ? kit::kPaperLight : kit::kPaperDim);
      d->AddRect(y0, y1, kit::kBorder, 0, 0, 1.0f);
      const float gw = kit::Glyph(nullptr, PromptStyle(), kit::Button::kY, KeyFor(kit::Button::kY), ImVec2(0, 0), st(20), false);
      kit::Glyph(d, PromptStyle(), kit::Button::kY, KeyFor(kit::Button::kY), ImVec2((y0.x + y1.x - gw) * 0.5f, (y0.y + y1.y) * 0.5f),
                 st(20));
      if (yh && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) in.y = true;
    }
    // ---- the actions
    if (edit || in.view) return BeginAccount(false);
    if (in.x) list_.RefreshNow(), BrowserNote(lan ? "ASKING YOUR NETWORK" : "REFRESHING THE LIST");
    if (in.y && sv && !lan) ToggleFavourite(*sv);
    if (in.accept && sv) join = true;
    if (join && sv) {
      if (sv->max_players > 0 && sv->players >= sv->max_players) {
        BrowserNote("THAT SERVER IS FULL");
      } else {
        REXLOG_INFO("BF browser: join {} ({})", sv->name, sv->map);
        if (lan) {  // a LAN match is on its host's own backend
          const ServerInfo pick = *sv;
          direct_host_ = pick.address.substr(0, pick.address.rfind(':'));
          JoinOnOwnBackend(direct_host_, pick);
          return;
        }
        Join(*sv);
        return;
      }
    }
    if (in.back) BackFromBrowser();
  }

  // ---- Direct connect in the new look: the address typed (the keyboard, or the on screen keys with a controller) on
  // the left paper with the recent servers under it; the keys on the right card.
  void DirectPanel(ImDrawList* d, ImGuiIO& io, const Stage& st, Input& in, ImVec2 ta, ImVec2 tb, ImVec2 pa, ImVec2 pb) {
    const kit::Fonts& f = kit::GetFonts();
    if (in.start) in.accept = false;  // Start connects (BF's own pages make it A as well: OnDraw)
    {  // the keyboard types straight in (as on the name page, the window is asked for text input)
      ImGuiContext& g = *ImGui::GetCurrentContext();
      g.PlatformImeData.WantTextInput = true;
      g.PlatformImeData.ViewportId = ImGui::GetMainViewport()->ID;
    }
    auto allowed = [](unsigned c) { return std::isalnum(int(c)) || c == '.' || c == ':' || c == '-'; };
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
      const ImWchar c = io.InputQueueCharacters[i];
      if (c < 128 && allowed(c) && direct_text_.size() < 64) direct_text_ += char(c), direct_keys_ = false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !direct_text_.empty()) direct_text_.pop_back();
    if (ImGui::IsKeyPressed(ImGuiKey_V) && io.KeyCtrl)
      if (const char* clip = ImGui::GetClipboardText())
        for (const char* c = clip; *c && direct_text_.size() < 64; ++c)
          if (allowed(static_cast<unsigned char>(*c))) direct_text_ += *c;
    // The finished lookup: the server found joins (and is kept among the recent ones), or the reason shows.
    if (direct_job_ && direct_job_->load() != 0) {
      if (direct_job_->load() == 1) {
        const ServerInfo sv = *direct_found_;
        std::string recent = sv.name + "|" + direct_text_;
        for (const std::string& r : RecentServers())
          if (r.substr(r.find('|') + 1) != direct_text_) recent += ";" + r;
        settings_.values["recent_servers"] = recent.substr(0, 400);
        settings_.Save();
        direct_job_.reset();
        Join(sv);
        return;
      }
      if (direct_job_->load() == 3) {  // found on its own backend at that address (a LAN only server)
        const ServerInfo sv = *direct_found_;
        std::string recent = sv.name + "|" + direct_text_;
        for (const std::string& r : RecentServers())
          if (r.substr(r.find('|') + 1) != direct_text_) recent += ";" + r;
        settings_.values["recent_servers"] = recent.substr(0, 400);
        settings_.Save();
        direct_job_.reset();
        JoinOnOwnBackend(direct_host_, sv);
        return;
      }
      direct_error_ = "No server answers at that address, on the main backend or on a backend of its own. Check the address and its port.";
      direct_job_.reset();
    }
    // ---- the left paper: the heading, the field and Connect, the recent servers
    kit::PaperArt(d, ta, tb);
    const float x = ta.x + st(26);
    float y = ta.y + st(24);
    kit::Spaced(d, f.typed, st(10), ImVec2(x, y), kit::kInkSection, "JOIN A SERVER BY ITS ADDRESS", st(2));
    y += st(18);
    kit::GameText(d, kit::GameFont::kHeadline, st(30), ImVec2(x, y), kit::kInk, "DIRECT CONNECT");
    y += st(30) + st(14);
    y += kit::Wrapped(d, f.ui, st(13.25f), ImVec2(x, y), kit::kInkFaint,
                      "For a server that is not listed, such as a friend's custom match or a LAN server. Ask its host for the "
                      "address.",
                      st(440)) +
         st(16);
    const ImVec2 f0(x, y), f1(x + st(430), y + st(42));
    d->AddRectFilled(f0, f1, kit::kPaperLight);
    d->AddRect(f0, f1, kit::kGreen, 0, 0, st(2));
    const float size = st(17);
    d->AddText(f.ui_bold, size, ImVec2(f0.x + st(12), f0.y + (st(42) - size * 1.45f) * 0.5f), kit::kInk, direct_text_.c_str());
    if (std::fmod(ImGui::GetTime(), 1.0) < 0.6) {
      const float cx = f0.x + st(13) + kit::TextWidth(f.ui_bold, size, direct_text_);
      d->AddRectFilled(ImVec2(cx, f0.y + st(10)), ImVec2(cx + st(2), f1.y - st(10)), kit::kInk);
    }
    bool connect = GreenButton(d, st, ImVec2(f1.x + st(12), f0.y), ImVec2(f1.x + st(12) + st(150), f1.y), kit::Button::kStart,
                               "CONNECT", "Enter");
    y += st(42) + st(12);
    const std::string state = direct_job_               ? "LOOKING FOR THE SERVER..."
                              : !direct_error_.empty() ? Upper(direct_error_)
                                                       : "AN ADDRESS AND PORT, OR A NAME. THE PORT IS 18300 IF YOU LEAVE IT OUT.";
    kit::Spaced(d, f.typed, st(9.5f), ImVec2(x, y), direct_error_.empty() || direct_job_ ? kit::kInkSection : kit::kStamp, state,
                st(1.5f));
    y += st(30);
    const std::vector<std::string> recent = RecentServers();
    if (!recent.empty()) {  // a click (Y with a controller) fills the address in
      kit::Spaced(d, f.typed, st(10), ImVec2(x, y), kit::kInkSection, "RECENT", st(2));
      y += st(18);
      for (size_t i = 0; i < recent.size() && i < 4; ++i) {
        const std::string& r = recent[i];
        const std::string name = r.substr(0, r.find('|')), address = r.substr(r.find('|') + 1);
        const ImVec2 r0(x, y), r1(tb.x - st(26), y + st(34));
        const bool hover = ImGui::IsMouseHoveringRect(r0, r1);
        if (hover) d->AddRectFilled(r0, r1, IM_COL32(34, 36, 27, 16));
        d->AddLine(ImVec2(r0.x, r1.y), ImVec2(r1.x, r1.y), kit::kRule, 1.0f);
        d->AddText(f.ui_bold, st(13.75f), ImVec2(x + st(8), y + (st(34) - st(12) * 1.45f) * 0.5f), kit::kInk, name.c_str());
        const float aw = kit::SpacedWidth(f.typed, st(10), address, st(1.5f));
        kit::Spaced(d, f.typed, st(10), ImVec2(r1.x - st(8) - aw, y + st(12)), kit::kInkSection, address, st(1.5f));
        if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) direct_text_ = address, direct_error_.clear();
        y += st(34);
      }
      if (in.y) direct_text_ = recent[0].substr(recent[0].find('|') + 1), direct_error_.clear();
    }
    // ---- the right card: the on screen keys, and what the buttons do
    kit::PaperArt(d, pa, pb);
    kit::Spaced(d, f.typed, st(10), ImVec2(pa.x + st(18), pa.y + st(18)), kit::kInkSection, "ON SCREEN KEYS", st(2));
    kit::Wrapped(d, f.ui, st(12), ImVec2(pa.x + st(18), pa.y + st(32)), kit::kInkFaint,
                 "With a controller: A opens them and types, X deletes, Start connects. A keyboard types straight in.",
                 pb.x - pa.x - st(36));
    static const char* kDigits[] = {"1", "2", "3", "DEL", "4", "5", "6", ".", "7", "8", "9", ":", "ABC", "0", "-", "GO"};
    static const char* kLetters[] = {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "A", "S", "D", "F", "G",
                                     "H", "J", "K", "L", ".", "Z", "X", "C", "V", "B", "N", "M", "123", "DEL", "GO"};
    const int cols = direct_letters_ ? 10 : 4, count = direct_letters_ ? 30 : 16;
    const char* const* keys = direct_letters_ ? kLetters : kDigits;
    const float kx = pa.x + st(18), kw = pb.x - pa.x - st(36), gap = st(5);
    const float cw = (kw - gap * float(cols - 1)) / float(cols), ch = direct_letters_ ? st(40) : st(48);
    const float ky = pa.y + st(78);
    direct_key_ = std::clamp(direct_key_, 0, count - 1);
    int pressed = -1;
    for (int k = 0; k < count; ++k) {
      const ImVec2 k0(kx + float(k % cols) * (cw + gap), ky + float(k / cols) * (ch + gap));
      const ImVec2 k1(k0.x + cw, k0.y + ch);
      const bool sel = direct_keys_ && k == direct_key_;
      const bool hover = ImGui::IsMouseHoveringRect(k0, k1);
      d->AddRectFilled(k0, k1, sel ? kit::kGreen : hover ? kit::kPaperLight : kit::kPaperDim);
      d->AddRect(k0, k1, sel ? kit::kGreenShadow : kit::kBorder, 0, 0, 1.0f);
      const std::string label = keys[k];
      const float ls = label.size() > 1 ? st(9) : st(14);
      const float lw = kit::TextWidth(f.ui_bold, ls, label);
      d->AddText(f.ui_bold, ls, ImVec2((k0.x + k1.x - lw) * 0.5f, k0.y + (ch - ls * 1.45f) * 0.5f), sel ? kit::kCream : kit::kInk,
                 label.c_str());
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) pressed = k, direct_key_ = k;
    }
    if (direct_keys_) {
      if (in.left) direct_key_ = (direct_key_ / cols) * cols + (direct_key_ % cols + cols - 1) % cols;
      if (in.right) direct_key_ = (direct_key_ / cols) * cols + (direct_key_ % cols + 1) % cols;
      if (in.up) direct_key_ = (direct_key_ + count - cols) % count;
      if (in.down) direct_key_ = (direct_key_ + cols) % count;
      if (in.accept) pressed = direct_key_;
      if (in.x && !direct_text_.empty()) direct_text_.pop_back();
    } else if (in.accept && !ImGui::IsKeyPressed(ImGuiKey_Enter) && !ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
      direct_keys_ = true;  // a controller's A opens the keys
    }
    if (pressed >= 0) {
      const std::string k = keys[pressed];
      if (k == "DEL") {
        if (!direct_text_.empty()) direct_text_.pop_back();
      } else if (k == "ABC" || k == "123") {
        direct_letters_ = !direct_letters_, direct_key_ = 0;
      } else if (k == "GO") {
        connect = true;
      } else if (direct_text_.size() < 64) {
        direct_text_ += direct_letters_ ? char(std::tolower(static_cast<unsigned char>(k[0]))) : k[0];
      }
    }
    if (in.start || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) connect = true;
    if (connect && !direct_job_) DirectConnect();
    if (in.back) {
      if (direct_keys_) direct_keys_ = false;
      else BackFromBrowser();
    }
    in.accept = in.back = false;  // taken here
  }
