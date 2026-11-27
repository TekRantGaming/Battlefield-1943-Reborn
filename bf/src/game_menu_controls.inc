// Controls by device (TekRantGaming, 16 Nov 2026: "when you open the control settings page I want an image of a
// keyboard and mouse and an image of a controller; you click on one of them and then you get the full control
// settings and everything for what you clicked on only, instead of a page that has both"). His pick of the two
// mockups was C1 "Kit Issue" (runs\ui-designs\build_controls.py, out\c1_*.png), included inside GameMenu
// (game_menu.cpp) before game_menu_settings.inc:
//   - Settings > Controls opens on a choice of two paper cards, a keyboard and mouse and a controller drawn in the
//     menus' ink (our own line drawings, no art from the game or anyone else), the card of the device used last
//     chosen. Left and right (or the mouse) choose, A (Enter) or a click opens; B (Esc) leaves the settings.
//   - The page behind a card is the settings page's own list (SettingsBody) with that device's rows only, sorted
//     into sections, its bindings at the end: two keys and two mouse buttons for each action on the keyboard's, the
//     controller's button for each action on the controller's. The help card draws the device with the chosen row's
//     keys or buttons lit. B (Esc) goes back to the choice.
//   - Button prompts and Camera turning work for both, so both pages have them. Every row is still the one
//     ControlsRows, KeyRows or RemapRows made (the same settings and cvars), only sorted by device.

  int ctl_device_ = -1;    // the Controls tab: -1 the choice, 0 the keyboard and mouse's page, 1 the controller's
  bool ctl_fresh_ = true;  // the choice not shown since Controls was opened: its cursor starts on the device used last
  bool ctl_bind_row_ = false;  // the chosen row is a binding (last frame's): the A prompt says BIND
  float help_top_ = 0;     // room left under the help card's title for a picture (SettingsBody)
  // The bindings shown: the infantry's or a vehicle kind's (vehicle_binds.h). BF_DEV_BIND_SET=<0..3> (tests): the
  // tab the page opens on.
#if defined(BF_DEV_TOOLS)
  int bind_set_ = [] {
    const char* v = std::getenv("BF_DEV_BIND_SET");
    return v ? std::atoi(v) : 0;
  }();
#else
  int bind_set_ = 0;
#endif

  // 18 Nov 2026 (the user: "separate keymapping controls for Ground, air, and boat vehicles (turrets stay on Infantry
  // control layer)"): the first row of the bindings chooses whose keys the rows below are. A vehicle kind's set has
  // the controller's buttons and sticks but Start; chat and voice are the same everywhere (the infantry's tab).
  // The controller's page has the same row (the user: "a version of all of this the same for controller/gamepad
  // remapping too"), for its buttons and sticks.
  SettingRow BindSetRow(const char* section, bool pad = false) {
    SettingRow r{section, pad ? "Buttons for" : "Keys for",
                 pad ? "Whose buttons and sticks the rows below are. Infantry: on foot, and in a gunner's or a "
                       "passenger's seat and the stationary guns. Ground vehicles: driving a tank or a jeep. Air "
                       "vehicles: flying a plane. Boats: driving a landing boat. Each starts as the infantry's were."
                     : "Whose keys the bindings below are. Infantry: on foot, and in a gunner's or a passenger's seat and "
                       "the stationary guns. Ground vehicles: driving a tank or a jeep. Air vehicles: flying a plane "
                       "(its pitch, yaw and roll rows also take the mouse's movement). Boats: driving a landing boat. "
                       "Each starts as the infantry's keys were.",
                 {}};
    for (int s = 0; s < bf1943::kBindSetCount; ++s) r.options.push_back(bf1943::BindSetName(s));
    r.index = std::clamp(bind_set_, 0, int(bf1943::kBindSetCount) - 1);
    r.choose = [this](int i) {
      bind_set_ = std::clamp(i, 0, int(bf1943::kBindSetCount) - 1);
      capture_.clear();
    };
    return r;
  }
  std::string ResetBindsName() const {
    return bind_set_ <= 0 ? std::string("Reset infantry bindings")
                          : std::string("Reset ") + bf1943::BindSetName(bind_set_) + " bindings";
  }

  // The actions BF reads, the SDK's keybind_* settings, each named by what BF does with it (BF's own Help & Options >
  // Controls for the soldier, vehicles and planes, runs help3 and helpveh1; in a vehicle the walk keys are the
  // throttle and the brake and the zoom key the second weapon, input_settings.cpp). Shared by KeyRows and the two
  // device pages.
  static constexpr std::pair<const char*, const char*> kBindActions[] = {
      {"keybind_lstick_up", "Move forward / throttle"},
      {"keybind_lstick_down", "Move back / brake"},
      {"keybind_lstick_left", "Move left / turn left"},
      {"keybind_lstick_right", "Move right / turn right"},
      {"keybind_right_trigger", "Fire"},
      {"keybind_left_trigger", "Zoom / second weapon"},
      {"keybind_a", "Jump / change seat"},
      {"keybind_b", "Enter, use, pick up / exit"},
      {"keybind_x", "Reload"},
      {"keybind_y", "Draw knife"},
      {"keybind_left_shoulder", "Grenade, explosives"},
      {"keybind_right_shoulder", "Toggle weapon / second weapon"},
      {"keybind_lstick_press", "Sprint"},
      {"keybind_rstick_press", "Crouch"},
      {"keybind_back", "Command (tap), score (hold)"},
      {"keybind_start", "Game menu"},
      {"keybind_dpad_up", "Vehicle camera (D-pad up)"},
      {"keybind_free_look", "Free-Look"},  // planes' and ground vehicles' only (vehicle_binds.h BindInSet)
      {"keybind_dpad_down", "D-pad down"},
      {"keybind_dpad_left", "D-pad left"},
      {"keybind_dpad_right", "D-pad right"},
      {"keybind_rstick_up", "Look up (mouse camera off)"},
      {"keybind_rstick_down", "Look down (mouse camera off)"},
      {"keybind_rstick_left", "Look left (mouse camera off)"},
      {"keybind_rstick_right", "Look right (mouse camera off)"},
      {"keybind_push_to_talk", "Push to talk (voice chat)"},
      {"keybind_chat_all", "Chat to everyone"},
      {"keybind_chat_team", "Chat to your team"},
      {"keybind_chat_squad", "Chat to your squad"},
      {"keybind_mark_problem", "Mark a problem for the log"}};
  // The controller's button behind each button action (the bf_remap_* settings say what each button presses).
  static constexpr std::pair<const char*, const char*> kPadOfBind[] = {
      {"keybind_a", "a"}, {"keybind_b", "b"}, {"keybind_x", "x"}, {"keybind_y", "y"},
      {"keybind_left_shoulder", "lb"}, {"keybind_right_shoulder", "rb"}, {"keybind_left_trigger", "lt"},
      {"keybind_right_trigger", "rt"}, {"keybind_lstick_press", "ls"}, {"keybind_rstick_press", "rs"},
      {"keybind_back", "back"}, {"keybind_start", "start"}, {"keybind_dpad_up", "up"}, {"keybind_dpad_down", "down"},
      {"keybind_dpad_left", "left"}, {"keybind_dpad_right", "right"}};
  static constexpr std::pair<const char*, const char*> kPadButtonNames[] = {
      {"a", "A"}, {"b", "B"}, {"x", "X"}, {"y", "Y"}, {"lb", "LB"}, {"rb", "RB"}, {"lt", "LT"}, {"rt", "RT"},
      {"ls", "LS"}, {"rs", "RS"}, {"back", "Back"}, {"start", "Start"}, {"up", "D-Up"}, {"down", "D-Down"},
      {"left", "D-Left"}, {"right", "D-Right"}};
  static constexpr const char* kPadButtonLong[] = {"A", "B", "X", "Y", "Left bumper", "Right bumper", "Left trigger",
                                                   "Right trigger", "Left stick press", "Right stick press", "Back",
                                                   "Start", "D-pad up", "D-pad down", "D-pad left", "D-pad right"};

  // 13 Nov 2026: the wheel's notches are mouse buttons for the bindings that are a button (look.cpp WheelNotch); the
  // first visit to the bindings puts the wheel where it was, on Toggle Weapon.
  void EnsureWheelBindable() {
    if (REXCVAR_GET(bf_wheel_bindable)) return;
    bool named = false;
    for (const auto& [cvar, label] : kBindActions)
      named = named || (bf1943::BindInSet(bf1943::kBindInfantry, cvar) && Cvar(cvar).find("Wheel") != std::string::npos);
    if (!named) {
      const std::string now = Cvar("keybind_right_shoulder");
      SetCvar("keybind_right_shoulder", now.empty() ? "WheelUp,WheelDown" : now + ",WheelUp,WheelDown");
    }
    SetCvar("bf_wheel_bindable", "true");
  }

  // The set the bindings' rows show now.
  int BindSet() const { return std::clamp(bind_set_, 0, int(bf1943::kBindSetCount) - 1); }
  // The controller's button (index in kPadButtonNames) that presses a button action ("a", "lt", ...) in the shown
  // set, or -1.
  int PadButtonOf(const std::string& action) {
    for (int b = 0; b < int(std::size(kPadButtonNames)); ++b)
      if (Cvar(bf1943::RemapCvar(BindSet(), kPadButtonNames[b].first)) == action) return b;
    return -1;
  }
  // A press of another button for an action in the shown set: the action that had that button takes this one's.
  void SwapPadButton(const std::string& action, int to_button) {
    const int set = BindSet(), had = PadButtonOf(action);
    const std::string to = bf1943::RemapCvar(set, kPadButtonNames[to_button].first), other = Cvar(to);
    SetCvar(to, action);
    if (had >= 0) SetCvar(bf1943::RemapCvar(set, kPadButtonNames[had].first), other);
    REXLOG_INFO("BF settings: the {} controller's {} now does {} ({} does {})", bf1943::BindSetName(set),
                kPadButtonNames[to_button].second, action, had >= 0 ? kPadButtonNames[had].second : "nothing else",
                had >= 0 ? other : std::string("-"));
  }
  void ResetPadButtons(int set) {
    for (const auto& [id, shown] : kPadButtonNames) SetCvar(bf1943::RemapCvar(set, id), id);
  }

  // The controller's stick remapping rows of the shown set (bf_axis_*, input_settings.cpp AxisRemap): each of the game's
  // stick axes, named by what it does in that set, and the controller's axis it gets.
  static constexpr std::pair<const char*, const char*> kStickSources[] = {
      {"Left stick X", "lx"},  {"Left stick X, inverted", "-lx"},  {"Left stick Y", "ly"},
      {"Left stick Y, inverted", "-ly"}, {"Right stick X", "rx"}, {"Right stick X, inverted", "-rx"},
      {"Right stick Y", "ry"}, {"Right stick Y, inverted", "-ry"}, {"Nothing", "none"}};
  // Each a slot as the keys' are (the stick axis's glyph in its box): a click on it, or A, waits for a stick to be
  // pushed (CapturePad); left and right go through the axes.
  void PadStickRows(std::vector<SettingRow>& rows, const char* section) {
    const int set = BindSet();
    const int n = int(std::size(kStickSources));
    for (int a = 0; a < 4; ++a) {
      const std::string cvar = bf1943::AxisCvar(set, bf1943::kStickAxes[a]);
      std::string v = Cvar(cvar);
      if (v.empty()) v = bf1943::kStickAxes[a];
      int index = a * 2;
      for (int o = 0; o < n; ++o)
        if (v == kStickSources[o].second) index = o;
      SettingRow r{section, bf1943::AxisLabel(set, a),
                   std::string("The controller's stick axis for this (") + kStickSources[a * 2].first +
                       " as it starts). Click the box (or A on it), then push the stick the way that should be right "
                       "or up: pushed left or down, the axis is inverted. Left and right go through the axes. Esc keeps "
                       "what it has; hold Esc for none. The names are the game's Normal layouts; its own Southpaw and "
                       "the other layouts move them again. In a plane yaw, roll and pitch can each be on any stick."};
      r.pad_glyphs = true;
      r.slots = {v == "none" ? std::string() : v};
      r.value = kStickSources[index].first;
      const bool chosen = int(rows.size()) == row_;
      r.slot_cursor = chosen ? 0 : -1;
      r.slot_capture = capture_ == "axis:" + cvar ? 0 : -1;
      auto wait = [this, cvar](int) {
        slot_cursor_ = 0;
        capture_ = "axis:" + cvar, pad_capture_at_ = ImGui::GetTime(), pad_armed_ = false;
      };
      r.slot_click = wait;
      r.activate = [wait] { wait(0); };
      r.nudge = [this, cvar, index, n](int by) {
        const char* to = kStickSources[(index + by + n) % n].second;
        SetCvar(cvar, to);
        REXLOG_INFO("BF settings: {} now gets the controller's {}", cvar, to);
      };
      rows.push_back(r);
    }
    SettingRow reset{section, std::string("Reset ") + (set == 0 ? "infantry" : bf1943::BindSetName(set)) + " sticks",
                     "Every stick axis of this tab back to its own. The other tabs keep theirs."};
    reset.activate = [this, set] {
      for (const char* a : bf1943::kStickAxes) SetCvar(bf1943::AxisCvar(set, a), a);
      REXLOG_INFO("BF settings: the {} sticks back to their own", bf1943::BindSetName(set));
    };
    rows.push_back(reset);
  }

  // ---- Plane flight (TekRantGaming, 23 Nov 2026: "a option for keybinds for the plane for kbm remap of pitch yaw and
  // roll to keboard and mouse"): the six ways a plane turns, each its own row in the bindings' Air vehicles tab (its
  // own section until 24 Nov 2026, FlightInBindings), with a box for the mouse's movement and the bindings' two keys
  // and two mouse buttons. They are the settings that fly a plane already, only gathered and named by what they do:
  // the keys are the Air vehicles set's right stick (pitch, roll) and left stick left and right (the rudder),
  // vehicle_binds.cpp; the mouse's box is what bf_plane_mouse_x and _y fly with the two Invert flight settings
  // (plane_mouse.cpp). So the defaults are today's
  // (I and K pitch, J and L roll, A and D yaw, the mouse's left and right roll, its forward and back pitch) and
  // nothing changes for anyone who leaves them. A key or mouse button that something else in a plane also uses is
  // marked, and the help card says what with.
  struct FlightAction {
    const char* label;
    const char* axis;  // what bf_plane_mouse_x and _y call it
    int sign;          // its way in the game's concepts (plane_mouse.cpp): +1 roll right, yaw right, nose down
    const char* bind;  // the infantry's name of its keys; the Air vehicles set's copy is the one used
  };
  static constexpr FlightAction kFlightActions[] = {
      {"Pitch up (climb)", "pitch", -1, "keybind_rstick_up"},  {"Pitch down (dive)", "pitch", 1, "keybind_rstick_down"},
      {"Yaw left (rudder)", "yaw", -1, "keybind_lstick_left"}, {"Yaw right (rudder)", "yaw", 1, "keybind_lstick_right"},
      {"Roll left", "roll", -1, "keybind_rstick_left"},        {"Roll right", "roll", 1, "keybind_rstick_right"}};
  static constexpr int kFlightCount = int(std::size(kFlightActions));
  bool capture_flight_ = false;   // the key slot waiting is a Plane flight row's (the Bindings rows share its setting)
  double flight_capture_at_ = 0;  // when a mouse box began to wait
  bool flight_armed_ = false;     // the mouse's buttons let go since it began: its movement counts
  float flight_dx_ = 0, flight_dy_ = 0;

  // The keys of a flight action: the Air vehicles set's. The game turns a plane's pitch round with its own Invert
  // flight (the controller's, GstInput Scheme3FlipY, native_input.cpp), and the keys' stick goes through it as a
  // controller's does: with it Inverted the right stick's down key is the one that climbs.
  std::string FlightKeyCvar(int i) {
    std::string bind = kFlightActions[i].bind;
    if (i < 2 && GameValue("GstInput", "Scheme3FlipY", "1") == "0")
      bind = i == 0 ? "keybind_rstick_down" : "keybind_rstick_up";
    return bf1943::BindCvar(bf1943::kBindAir, bind);
  }
  // The mouse's movement that does a flight action now: "Right", "Left", "Back" (towards you) or "Forward"; empty
  // when the mouse flies none of it. The mouse's right adds to its axis's concept and its back takes away, each turned
  // round by its Invert flight (plane_mouse.cpp).
  std::string FlightMouseOf(int i) {
    const FlightAction& a = kFlightActions[i];
    if (!Truthy(Cvar("bf_plane_mouse_aim"))) return "";
    if (Cvar("bf_plane_mouse_x") == a.axis)
      return (a.sign > 0) != Truthy(Cvar("bf_mouse_invert_x_flight")) ? "Right" : "Left";
    if (Cvar("bf_plane_mouse_y") == a.axis)
      return (a.sign < 0) != Truthy(Cvar("bf_mouse_invert_y_flight")) ? "Back" : "Forward";
    return "";
  }
  // The mouse moved `m` (+1 right or back, -1 left or forward) along x or y for a flight action: that axis flies it,
  // the way round that makes this movement do it. The other axis, if it flew the same, takes what this one flew (so
  // both never fly one, as the controller page's swap).
  void SetFlightMouse(int i, bool x, int m) {
    const FlightAction& a = kFlightActions[i];
    const std::string mine = x ? "bf_plane_mouse_x" : "bf_plane_mouse_y", other = x ? "bf_plane_mouse_y" : "bf_plane_mouse_x";
    const std::string had = Cvar(mine);
    if (Cvar(other) == a.axis) SetCvar(other, had.empty() || had == a.axis ? "none" : had);
    SetCvar(mine, a.axis);
    if (x) SetCvar("bf_mouse_invert_x_flight", m != a.sign ? "true" : "false");
    else SetCvar("bf_mouse_invert_y_flight", m == a.sign ? "true" : "false");
    if (!Truthy(Cvar("bf_plane_mouse_aim"))) SetCvar("bf_plane_mouse_aim", "true");
    REXLOG_INFO("BF settings: plane flight: {} on the mouse's {} {} (mouse x {}{}, y {}{})", a.label, x ? "x" : "y",
                m > 0 ? (x ? "right" : "back") : (x ? "left" : "forward"), Cvar("bf_plane_mouse_x"),
                Truthy(Cvar("bf_mouse_invert_x_flight")) ? " inverted" : "", Cvar("bf_plane_mouse_y"),
                Truthy(Cvar("bf_mouse_invert_y_flight")) ? " inverted" : "");
  }
  // A mouse box waiting (capture_ "flight:<n>"): once the mouse's buttons are let go, the next clear movement goes to
  // the action. Esc let go at once keeps what it has, Esc held takes the mouse off that axis; 8 s with nothing keeps it.
  bool CaptureFlightAxis() {
    const int i = std::clamp(std::atoi(capture_.c_str() + 7), 0, kFlightCount - 1);
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyDown(ImGuiKey_Escape)) {
      if (esc_held_since_ < 0) esc_held_since_ = ImGui::GetTime();
      if (ImGui::GetTime() - esc_held_since_ >= 0.6) {
        for (const char* axis : {"bf_plane_mouse_x", "bf_plane_mouse_y"})
          if (Cvar(axis) == kFlightActions[i].axis) SetCvar(axis, "none");
        REXLOG_INFO("BF settings: plane flight: the mouse flies no {} now", kFlightActions[i].axis);
        capture_.clear();
        esc_held_since_ = -1;
        esc_swallow_ = true;
      }
      return true;
    }
    if (esc_held_since_ >= 0) {
      esc_held_since_ = -1;
      capture_.clear();
      return true;
    }
    if (ImGui::GetTime() - flight_capture_at_ > 8.0) {
      REXLOG_INFO("BF settings: plane flight: {} kept (the mouse was not moved)", kFlightActions[i].label);
      capture_.clear();
      return true;
    }
    if (!flight_armed_) {
      flight_armed_ = !ImGui::IsAnyMouseDown();
      flight_dx_ = flight_dy_ = 0;
      return true;
    }
    flight_dx_ += io.MouseDelta.x, flight_dy_ += io.MouseDelta.y;
    const float need = std::max(24.0f, io.DisplaySize.y * 0.04f);
    if (std::max(std::fabs(flight_dx_), std::fabs(flight_dy_)) < need) return true;
    const bool x = std::fabs(flight_dx_) >= std::fabs(flight_dy_);
    SetFlightMouse(i, x, (x ? flight_dx_ : flight_dy_) > 0 ? 1 : -1);
    capture_.clear();
    return true;
  }
  void StartFlightAxis(int i) {
    capture_ = "flight:" + std::to_string(i), capture_flight_ = false;
    flight_capture_at_ = ImGui::GetTime(), flight_armed_ = false, flight_dx_ = flight_dy_ = 0;
  }
  // A binding's keys, every one (BindSlots keeps two of each kind).
  static std::vector<std::string> BindTokens(const std::string& value) {
    std::vector<std::string> out;
    for (size_t at = 0; at <= value.size();) {
      size_t comma = value.find(',', at);
      if (comma == std::string::npos) comma = value.size();
      std::string k = value.substr(at, comma - at);
      at = comma + 1;
      while (!k.empty() && k.front() == ' ') k.erase(k.begin());
      while (!k.empty() && k.back() == ' ') k.pop_back();
      if (!k.empty()) out.push_back(k);
    }
    return out;
  }
  // Every key and mouse button read in a plane's seat, and what each does there: the Air vehicles set's own keys and
  // the ones every seat shares (the game menu, chat, voice). The flight actions by their names here.
  std::map<std::string, std::vector<std::pair<std::string, std::string>>> PlaneKeyUses() {
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> uses;
    std::string flight[kFlightCount];
    for (int f = 0; f < kFlightCount; ++f) flight[f] = FlightKeyCvar(f);
    for (const auto& [infantry, label] : kBindActions) {
      const std::string cvar = bf1943::BindCvar(bf1943::kBindAir, infantry);
      if (cvar.empty()) continue;
      std::string what = bf1943::BindLabel(bf1943::kBindAir, infantry, label);
      for (int f = 0; f < kFlightCount; ++f)
        if (flight[f] == cvar) what = kFlightActions[f].label;
      for (const std::string& k : BindTokens(Cvar(cvar))) uses[k].push_back({cvar, what});
    }
    return uses;
  }

  // The six rows, each: the mouse's box, then two keys and two mouse buttons (the five boxes of the combined list,
  // its first column the mouse's movement in place of the controller's button). A click on a box, or A on the one
  // left and right are on, waits: the mouse box for a movement, the others for a press. A clash is drawn red and named
  // in the row's note. The clashes are listed in a row of their own after them.
  void FlightRows(std::vector<SettingRow>& rows, const char* section) {
    const auto uses = PlaneKeyUses();
    const std::string mx = Cvar("bf_plane_mouse_x"), my = Cvar("bf_plane_mouse_y");
    const bool both = Truthy(Cvar("bf_plane_mouse_aim")) && mx == my && (mx == "roll" || mx == "pitch" || mx == "yaw");
    std::vector<std::string> all_clashes;
    for (int i = 0; i < kFlightCount; ++i) {
      const FlightAction& a = kFlightActions[i];
      const std::string cvar = FlightKeyCvar(i);
      SettingRow r{section, a.label,
                   "What flies this in a plane, as the pilot: the mouse's movement (first box), two keys and two mouse "
                   "buttons. Click a box (or A on it). The mouse's box then waits for the mouse to be moved the way "
                   "that should do this: that way of the mouse flies it, and the opposite way the opposite action "
                   "(the other mouse axis takes over what this one flew). The other boxes wait for a key or a mouse "
                   "button. Esc keeps what a box has; hold Esc to clear it. Keys and the mouse fly together: an axis "
                   "both fly takes the stronger. The names are the game's Normal plane controls."};
      r.slots.push_back(FlightMouseOf(i));
      const std::array<std::string, 4> keys = BindSlots(Cvar(cvar));
      r.slots.insert(r.slots.end(), keys.begin(), keys.end());
      r.slot_header = "MOUSE MOVE";
      std::string clashes;
      for (int k = 1; k < 5; ++k) {
        const std::string& key = r.slots[size_t(k)];
        const auto it = key.empty() ? uses.end() : uses.find(key);
        if (it == uses.end()) continue;
        std::string with;
        for (const auto& [other, what] : it->second)
          if (other != cvar) with += (with.empty() ? "" : ", ") + what;
        if (with.empty()) continue;
        r.slot_clash |= 1u << k;
        clashes += (clashes.empty() ? "" : " ") + key + " also: " + with + ".";
        all_clashes.push_back(key + " (" + a.label + ", " + with + ")");
      }
      if (both && mx == a.axis) {
        r.slot_clash |= 1u;
        clashes += std::string(clashes.empty() ? "" : " ") + "Both of the mouse's axes fly " + a.axis + ".";
        if (a.sign < 0) all_clashes.push_back(std::string("the mouse's two axes (both ") + a.axis + ")");
      }
      if (!clashes.empty()) r.note = "CLASH: " + clashes + " Both work at once; change one if that is not meant.";
      const bool chosen = int(rows.size()) == row_;
      r.slot_cursor = chosen ? std::clamp(slot_cursor_, 0, 4) : -1;
      r.slot_capture = capture_ == "flight:" + std::to_string(i) ? 0
                       : capture_flight_ && capture_ == cvar   ? capture_slot_ + 1
                                                                 : -1;
      r.slot_click = [this, i, cvar](int slot) {
        slot_cursor_ = std::clamp(slot, 0, 4);
        if (slot_cursor_ == 0) StartFlightAxis(i);
        else capture_ = cvar, capture_slot_ = slot_cursor_ - 1, capture_flight_ = true;
      };
      r.activate = [this, i, cvar] {
        if (slot_cursor_ <= 0) StartFlightAxis(i);
        else capture_ = cvar, capture_slot_ = std::clamp(slot_cursor_, 1, 4) - 1, capture_flight_ = true;
      };
      r.nudge = [this](int by) { slot_cursor_ = std::clamp(slot_cursor_ + by, 0, 4); };
      // The pause's panel shows the boxes as text, the chosen one in brackets (KeyRows).
      for (int s = 0; s < 5; ++s) {
        std::string shown = r.slot_capture == s ? (s == 0 ? "MOVE" : "PRESS")
                            : r.slots[size_t(s)].empty() ? "-"
                            : s == 0                     ? "Mouse " + r.slots[0]
                                                         : r.slots[size_t(s)];
        if ((r.slot_clash >> s) & 1u) shown += "!";
        if (chosen && r.slot_cursor == s) shown = "[" + shown + "]";
        r.value += (s == 0 ? "" : s == 1 || s == 3 ? "   |   " : "  ") + shown;
      }
      rows.push_back(r);
    }
    SettingRow clash{section, "Clashes",
                     "Keys, mouse buttons or mouse axes the flight rows share with something else in a plane: the "
                     "Air vehicles bindings, Free-Look, the game menu, chat and voice. Each clash is marked red in its "
                     "row. A clash is allowed (both happen at once), only shown."};
    if (all_clashes.empty()) {
      clash.value = "None";
    } else {
      clash.value = std::to_string(all_clashes.size()) + (all_clashes.size() == 1 ? " clash" : " clashes");
      std::string list;
      for (const std::string& c : all_clashes) list += (list.empty() ? "" : "; ") + c;
      clash.note = "CLASH: " + list + ".";
    }
    rows.push_back(clash);
  }
  // The section's last row: the flight rows and the plane mouse's settings back to their defaults.
  void FlightResetRow(std::vector<SettingRow>& rows, const char* section) {
    SettingRow reset{section, "Reset plane flight",
                     "Pitch, yaw and roll back to their defaults: I and K pitch, A and D yaw, J and L roll, the mouse's "
                     "left and right roll and its forward and back pitch, not inverted (Free-Look in planes too), at "
                     "sensitivity 1. The other "
                     "Air vehicles bindings (throttle, fire, bombs...) keep theirs."};
    reset.activate = [this] {
      std::vector<std::string> names = {"bf_plane_mouse_aim",        "bf_plane_mouse_x",         "bf_plane_mouse_y",
                                        "bf_mouse_invert_x_flight", "bf_mouse_invert_y_flight", "bf_plane_mouse_sensitivity",
                                        "bf_mouse_invert_y_air_look"};
      for (const FlightAction& a : kFlightActions) names.push_back(bf1943::BindCvar(bf1943::kBindAir, a.bind));
      for (const std::string& n : names)
        if (const auto* info = rex::cvar::GetFlagInfo(n)) SetCvar(n, info->default_value);
      REXLOG_INFO("BF settings: plane flight back to its defaults");
    };
    rows.push_back(reset);
  }
  // TekRantGaming, 24 Nov 2026: "The brand new plane keybinds we have currently are in their own section. Move them
  // into the overall bindings section." On the Air vehicles tab of the bindings (the keyboard's page and the pause's
  // Keyboard bindings) the six flight rows, then their Clashes row, take the place of that set's own pitch, yaw and
  // roll rows (the same settings: FlightKeyCvar), where the first of them would be. True when `infantry` is one of
  // them (its row is the flight rows' now); `shown` keeps them to once.
  bool FlightInBindings(int set, const char* infantry, std::vector<SettingRow>& rows, const char* section, bool& shown) {
    if (set != bf1943::kBindAir) return false;
    bool flight = false;
    for (const FlightAction& a : kFlightActions) flight = flight || std::string_view(infantry) == a.bind;
    if (!flight) return false;
    if (!shown) FlightRows(rows, section), shown = true;
    return true;
  }

  // ---- the rows of each device's page
  // ControlsRows' rows (its Mapping rows are left out while the settings page draws, controls_buttons_), each given
  // the section it has on its device's page, or none when it belongs to the other device.
  std::string DeviceSection(int device, const SettingRow& r) {
    const std::string& n = r.name;
    const bool invert = n.rfind("Invert", 0) == 0;
    if (r.section == "Input") return device == 1 && n == "Keyboard & mouse" ? "" : "Input";
    // The mouse's plane rows (its axes and sensitivity in Mouse, the flight inverts in Invert) since 24 Nov 2026: the
    // flight rows themselves are in Bindings, the Air vehicles tab (KeyboardBindRows).
    if (r.section == "Keyboard & mouse") return device != 0 ? "" : invert ? "Invert" : "Mouse";
    if (r.section == "Controller") {
      if (device != 1) return "";
      if (invert || n == "Left stick" || n == "Camera horizontal") return "Invert";
      if (n == "Aim" || n == "Aim assist" || n == "Aim assist strength") return "Aim";
      if (n.rfind("Vibration", 0) == 0) return "Vibration";
      return "Look";
    }
    if (r.section == "Game controls") {
      if (device != 1) return "";
      return n == "Game vibration" ? "Vibration" : n == "Game sensitivity" ? "Look" : "Game layouts";
    }
    return "";
  }

  std::vector<SettingRow> DeviceControlsRows(int device) {
    static const char* kKeyboardSections[] = {"Input", "Mouse", "Invert"};
    static const char* kPadSections[] = {"Input", "Look", "Invert", "Aim", "Vibration", "Game layouts"};
    std::vector<SettingRow> all = ControlsRows();
    std::vector<SettingRow> rows;
    auto sorted = [&](const char* section) {
      for (const SettingRow& r : all)
        if (DeviceSection(device, r) == section) {
          SettingRow copy = r;
          copy.section = section;
          if (copy.name == "Keyboard & mouse")
            copy.help = "Controllers always work. With this on, a keyboard and mouse work too, at the same time: the "
                        "mouse turns the camera and stays in the game window while you play (our menu frees it). Your "
                        "keys are under Bindings on this page.";
          rows.push_back(std::move(copy));
        }
    };
    if (device == 0)
      for (const char* s : kKeyboardSections) sorted(s);
    else
      for (const char* s : kPadSections) sorted(s);
    if (device == 0) KeyboardBindRows(rows);
    else PadButtonRows(rows);
    for (const SettingRow& r : all)  // the keys while playing (F2, the performance metrics) on both
      if (r.section == "Keys while playing") rows.push_back(r);
    return rows;
  }

  // The keyboard's page: each action's two keys and two mouse buttons (the 13 Nov bindings without the controller's
  // column, which is on the controller's page). A click on a slot, or A on the one left and right are on, waits for
  // a press (CaptureBind).
  void KeyboardBindRows(std::vector<SettingRow>& rows) {
    EnsureWheelBindable();
    rows.push_back(BindSetRow("Bindings"));
    const int set = std::clamp(bind_set_, 0, int(bf1943::kBindSetCount) - 1);
    bool flight_shown = false;
    for (const auto& [infantry, label] : kBindActions) {
      if (!bf1943::BindInSet(set, infantry)) continue;
      if (FlightInBindings(set, infantry, rows, "Bindings", flight_shown)) continue;
      const std::string cvar = bf1943::BindCvar(set, infantry);
      SettingRow r{"Bindings", bf1943::BindLabel(set, infantry, label),
                   "Two keys and two mouse buttons for each action: any of them does it. Click a slot (or A on it), "
                   "then press the key or the mouse button; the mouse columns also take the side buttons and, for the "
                   "button actions, a wheel notch. Esc keeps what it has; hold Esc to clear the slot."};
      const std::string name = cvar;
      const bool chosen = int(rows.size()) == row_;
      const std::array<std::string, 4> slots = BindSlots(Cvar(cvar));
      r.slots.assign(slots.begin(), slots.end());
      r.slot_cursor = chosen ? std::clamp(slot_cursor_, 0, 3) : -1;
      r.slot_capture = capture_ == cvar && !capture_flight_ ? capture_slot_ : -1;
      r.slot_click = [this, name](int slot) {
        slot_cursor_ = std::clamp(slot, 0, 3);
        capture_ = name, capture_slot_ = slot_cursor_, capture_flight_ = false;
      };
      r.activate = [this, name] {
        capture_ = name, capture_slot_ = std::clamp(slot_cursor_, 0, 3), capture_flight_ = false;
      };
      r.nudge = [this](int by) { slot_cursor_ = std::clamp(slot_cursor_ + by, 0, 3); };
      rows.push_back(r);
    }
    SettingRow reset{"Bindings", ResetBindsName(),
                     "Every key and mouse button of this tab back to its default. The other tabs keep theirs, and the "
                     "controller's buttons theirs (they are on the controller's page)."};
    reset.activate = [this, set] {
      for (const auto& [infantry, label] : kBindActions) {
        if (!bf1943::BindInSet(set, infantry)) continue;
        const std::string cvar = bf1943::BindCvar(set, infantry);
        if (const auto* info = rex::cvar::GetFlagInfo(cvar)) SetCvar(cvar, info->default_value);
      }
      REXLOG_INFO("BF settings: the {} keys and mouse buttons back to their defaults", bf1943::BindSetName(set));
    };
    rows.push_back(reset);
    if (set == bf1943::kBindAir) FlightResetRow(rows, "Bindings");
  }

  // The controller's page: each button action's button, in a box as the keys' (its glyph; 18 Nov 2026). A click on
  // it, or A, waits for the next press of the controller (CapturePad); left and right go through the buttons. The
  // action that had the button takes this one's, so every action keeps one (0.4.2's swap). The sticks' own actions
  // have none to change, and chat and voice have no button.
  void PadButtonRows(std::vector<SettingRow>& rows) {
    rows.push_back(BindSetRow("Buttons", true));
    const int set = BindSet();
    for (const auto& [cvar, label] : kBindActions) {
      std::string action;
      for (const auto& [bind, id] : kPadOfBind)
        if (std::string(cvar) == bind) action = id;
      if (action.empty()) continue;
      SettingRow r{"Buttons", bf1943::PadLabel(set, action, label),
                   "The controller's button for this action. Click the box (or A on it), then press the button: the "
                   "action that had it takes this one's, so every action keeps one. Left and right go through the "
                   "buttons. Esc keeps what it has; hold Esc for no button."};
      const int b = PadButtonOf(action);
      r.pad_glyphs = true;
      r.slots = {b >= 0 ? std::string(kPadButtonNames[b].first) : std::string()};
      r.value = b >= 0 ? kPadButtonLong[b] : "None";
      const bool chosen = int(rows.size()) == row_;
      r.slot_cursor = chosen ? 0 : -1;
      r.slot_capture = capture_ == "pad:" + action ? 0 : -1;
      auto wait = [this, action](int) {
        slot_cursor_ = 0;
        capture_ = "pad:" + action, pad_capture_at_ = ImGui::GetTime(), pad_armed_ = false;
      };
      r.slot_click = wait;
      r.activate = [wait] { wait(0); };
      r.nudge = [this, action](int by) {
        const int n = int(std::size(kPadButtonNames)), had = PadButtonOf(action);
        SwapPadButton(action, ((had < 0 ? 0 : had) + by + n) % n);
      };
      rows.push_back(r);
    }
    SettingRow reset{"Buttons", set == 0 ? std::string("Reset infantry buttons")
                                         : std::string("Reset ") + bf1943::BindSetName(set) + " buttons",
                     "Every controller button of this tab back to its own action. The other tabs keep theirs, and the "
                     "keys theirs."};
    reset.activate = [this, set] {
      ResetPadButtons(set);
      REXLOG_INFO("BF settings: the {} controller's buttons back to their own actions", bf1943::BindSetName(set));
    };
    rows.push_back(reset);
    PadStickRows(rows, "Sticks");
  }

  // A controller slot's part as its glyph in the prompts' style (the keyboard's: the Xbox Series'), `h` tall, centred
  // on c: a button ("a", "lt", "up", ...), a D-pad direction (the D-pad with its arm marked: the glyphs have only up),
  // or a stick axis ("lx", "-ry": the stick's left and right or up and down, "INV" after an inverted one). Its width.
  float PadSlotGlyph(ImDrawList* d, const std::string& part, ImVec2 c, float h, bool draw = true) {
    const kit::Style style = PromptStyle() == kit::Style::kKeyboard ? kit::Style::kXboxSeries : PromptStyle();
    const bool inverted = !part.empty() && part[0] == '-';
    const std::string id = inverted ? part.substr(1) : part;
    float w = 0;
    auto at = [&](float width) { return ImVec2(c.x - width * 0.5f, c.y); };
    if (id == "lx" || id == "ly" || id == "rx" || id == "ry") {
      const std::string name = std::string(id[0] == 'l' ? "stick" : "stick_r") + (id[1] == 'x' ? "_lr" : "_ud");
      const kit::Fonts& f = kit::GetFonts();
      const float gw = kit::GlyphPicture(nullptr, style, name, ImVec2(0, 0), h, false);
      const float tw = inverted ? kit::SpacedWidth(f.typed, h * 0.36f, "INV", h * 0.05f) + h * 0.18f : 0.0f;
      w = (gw > 0 ? gw : kit::Glyph(nullptr, style, id[0] == 'l' ? kit::Button::kLS : kit::Button::kRS, "", ImVec2(0, 0), h, false)) + tw;
      if (draw) {
        const ImVec2 p = at(w);
        const float drawn = gw > 0 ? kit::GlyphPicture(d, style, name, p, h)
                                   : kit::Glyph(d, style, id[0] == 'l' ? kit::Button::kLS : kit::Button::kRS, "", p, h);
        if (inverted)
          kit::Spaced(d, f.typed, h * 0.36f, ImVec2(p.x + drawn + h * 0.18f, c.y - h * 0.24f), kit::kStamp, "INV", h * 0.05f);
      }
      return w;
    }
    static const std::pair<const char*, kit::Button> kParts[] = {
        {"a", kit::Button::kA},       {"b", kit::Button::kB},         {"x", kit::Button::kX},   {"y", kit::Button::kY},
        {"lb", kit::Button::kLB},     {"rb", kit::Button::kRB},       {"lt", kit::Button::kLT}, {"rt", kit::Button::kRT},
        {"ls", kit::Button::kLS},     {"rs", kit::Button::kRS},       {"back", kit::Button::kBack},
        {"start", kit::Button::kStart}, {"up", kit::Button::kDpadUp}, {"down", kit::Button::kDpad},
        {"left", kit::Button::kDpad}, {"right", kit::Button::kDpad}};
    for (const auto& [name, button] : kParts) {
      if (id != name) continue;
      w = kit::Glyph(nullptr, style, button, "", ImVec2(0, 0), h, false);
      if (draw) {
        const ImVec2 p = at(w);
        kit::Glyph(d, style, button, "", p, h);
        // The D-pad's other arms: a red mark on the one meant.
        const float k = h * 0.3f, s = h * 0.13f;
        const ImVec2 m(p.x + w * 0.5f, c.y);
        if (id == "down") d->AddTriangleFilled(ImVec2(m.x - s, m.y + k - s), ImVec2(m.x + s, m.y + k - s), ImVec2(m.x, m.y + k + s * 0.6f), kit::kStamp);
        if (id == "left") d->AddTriangleFilled(ImVec2(m.x - k + s, m.y - s), ImVec2(m.x - k + s, m.y + s), ImVec2(m.x - k - s * 0.6f, m.y), kit::kStamp);
        if (id == "right") d->AddTriangleFilled(ImVec2(m.x + k - s, m.y - s), ImVec2(m.x + k - s, m.y + s), ImVec2(m.x + k + s * 0.6f, m.y), kit::kStamp);
      }
      return w;
    }
    return 0;
  }

  // ---- the drawings: a keyboard with its arrows, a mouse, a controller (C1's, build_controls.py), in ink on paper.
  // A lit part is green, the chosen one red.
  struct DeviceLit {
    std::set<std::string> keys, mouse, pad;  // keys by the SDK's names, mouse parts (lmb rmb wheel m4 m5), pad parts
    std::string key_sel, mouse_sel, pad_sel;
    bool mouse_all = false;  // the whole mouse lit (its own settings)
  };
  static constexpr ImU32 kLitInk = IM_COL32(111, 125, 74, 255);  // a lit part: the olive the paper's green fades to

  // The key's place on a 18.5 by 6.4 keys grid, its label and the SDK's name for it.
  struct ArtKey {
    float x, y, w;
    const char* label;
    const char* id;
  };
  static const std::vector<ArtKey>& KeyboardKeys() {
    static const std::vector<ArtKey> keys = [] {
      std::vector<ArtKey> k;
      auto row = [&](float y, float x, std::initializer_list<std::tuple<float, const char*, const char*>> ks) {
        for (const auto& [w, label, id] : ks) {
          if (label) k.push_back({x, y, w, label, id});
          x += w;
        }
      };
      row(0, 0, {{1, "Esc", "Escape"}, {1, nullptr, nullptr}, {1, "F1", "F1"}, {1, "F2", "F2"}, {1, "F3", "F3"},
                 {1, "F4", "F4"}, {0.5f, nullptr, nullptr}, {1, "F5", "F5"}, {1, "F6", "F6"}, {1, "F7", "F7"},
                 {1, "F8", "F8"}, {0.5f, nullptr, nullptr}, {1, "F9", "F9"}, {1, "F10", "F10"}, {1, "F11", "F11"},
                 {1, "F12", "F12"}});
      row(1.4f, 0, {{1, "`", "Backtick"}, {1, "1", "1"}, {1, "2", "2"}, {1, "3", "3"}, {1, "4", "4"}, {1, "5", "5"},
                    {1, "6", "6"}, {1, "7", "7"}, {1, "8", "8"}, {1, "9", "9"}, {1, "0", "0"}, {1, "-", "Minus"},
                    {1, "=", "Plus"}, {2, "Back", "Backspace"}});
      row(2.4f, 0, {{1.5f, "Tab", "Tab"}, {1, "Q", "Q"}, {1, "W", "W"}, {1, "E", "E"}, {1, "R", "R"}, {1, "T", "T"},
                    {1, "Y", "Y"}, {1, "U", "U"}, {1, "I", "I"}, {1, "O", "O"}, {1, "P", "P"}, {1, "[", "LBracket"},
                    {1, "]", "RBracket"}, {1.5f, "\\", "Backslash"}});
      row(3.4f, 0, {{1.75f, "Caps", "CapsLock"}, {1, "A", "A"}, {1, "S", "S"}, {1, "D", "D"}, {1, "F", "F"},
                    {1, "G", "G"}, {1, "H", "H"}, {1, "J", "J"}, {1, "K", "K"}, {1, "L", "L"}, {1, ";", "Semicolon"},
                    {1, "'", "Quote"}, {2.25f, "Enter", "Return"}});
      row(4.4f, 0, {{2.25f, "Shift", "Shift"}, {1, "Z", "Z"}, {1, "X", "X"}, {1, "C", "C"}, {1, "V", "V"},
                    {1, "B", "B"}, {1, "N", "N"}, {1, "M", "M"}, {1, ",", "Comma"}, {1, ".", "Period"},
                    {1, "/", "Slash"}, {2.75f, "Shift", "Shift"}});
      row(5.4f, 0, {{1.25f, "Ctrl", "Control"}, {1.25f, "", ""}, {1.25f, "Alt", "Alt"}, {6.25f, "Space", "Space"},
                    {1.25f, "Alt", "Alt"}, {1.25f, "Fn", ""}, {1.25f, "", ""}, {1.25f, "Ctrl", "Control"}});
      row(1.4f, 15.5f, {{1, "Ins", "Insert"}, {1, "Home", "Home"}, {1, "PgUp", "PageUp"}});
      row(2.4f, 15.5f, {{1, "Del", "Delete"}, {1, "End", "End"}, {1, "PgDn", "PageDown"}});
      row(4.4f, 16.5f, {{1, "\xE2\x96\xB2", "Up"}});
      row(5.4f, 15.5f, {{1, "\xE2\x97\x80", "Left"}, {1, "\xE2\x96\xBC", "Down"}, {1, "\xE2\x96\xB6", "Right"}});
      return k;
    }();
    return keys;
  }

  // A small label centred on c: below 16 pixels the menu's fonts drew nothing in the first test (run ctl1, keys under
  // about 13 px were blank), so it is drawn at 16 and its vertices scaled down to `size` round the centre.
  static void SmallLabel(ImDrawList* d, ImFont* font, float size, ImVec2 c, ImU32 col, const char* t) {
    const float drawn = std::max(size, 16.0f), k = size / drawn;
    const float w = kit::TextWidth(font, drawn, t);
    const int start = d->VtxBuffer.Size;
    d->AddText(font, drawn, ImVec2(c.x - w * 0.5f, c.y - drawn * 0.72f), col, t);
    if (k < 1)
      for (int i = start; i < d->VtxBuffer.Size; ++i)
        d->VtxBuffer[i].pos = ImVec2(c.x + (d->VtxBuffer[i].pos.x - c.x) * k, c.y + (d->VtxBuffer[i].pos.y - c.y) * k);
  }

  // The keyboard, its top left at `at`, `u` a key's width (18.5 keys wide, 6.4 high, its frame a third of a key out).
  void KeyboardArt(ImDrawList* d, ImVec2 at, float u, const DeviceLit& lit) {
    const kit::Fonts& f = kit::GetFonts();
    const float gap = u * 0.12f, line = std::max(1.0f, u * 0.075f);
    const ImVec2 f0(at.x - u * 0.35f, at.y - u * 0.35f), f1(at.x + 18.85f * u, at.y + 6.75f * u);
    d->AddRectFilled(f0, f1, IM_COL32(233, 225, 201, 255), u * 0.35f);
    d->AddRect(f0, f1, kit::kInk, u * 0.35f, 0, line * 1.4f);
    for (const ArtKey& k : KeyboardKeys()) {
      const ImVec2 a(at.x + k.x * u + gap * 0.5f, at.y + k.y * u + gap * 0.5f), b(a.x + k.w * u - gap, a.y + u - gap);
      const bool sel = *k.id && lit.key_sel == k.id, on = *k.id && lit.keys.count(k.id);
      d->AddRectFilled(a, b, sel ? kit::kStamp : on ? kLitInk : kit::kPaperLight, u * 0.12f);
      d->AddRect(a, b, kit::kInk, u * 0.12f, 0, sel ? line * 1.6f : line);
      const float size = u * (std::strlen(k.label) <= 3 ? 0.36f : 0.27f);
      if (size >= 5.0f && *k.label)
        SmallLabel(d, f.ui_bold, size, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), sel || on ? kit::kCream : kit::kInkFaint,
                   k.label);
    }
  }

  // The mouse from above, `h` high (0.66 h wide), its top left at `at`.
  void MouseArt(ImDrawList* d, ImVec2 at, float h, const DeviceLit& lit) {
    const float w = h * 0.66f, line = std::max(1.2f, h * 0.012f), r = w * 0.48f;
    auto fill = [&](const char* part) {
      return lit.mouse_sel == part ? kit::kStamp : lit.mouse.count(part) || lit.mouse_all ? kLitInk : kit::kPaperLight;
    };
    // the side buttons (on the left, as a right hand's mouse has them)
    const ImVec2 s0(at.x - w * 0.07f, at.y + h * 0.47f), s1(at.x + w * 0.04f, at.y + h * 0.59f);
    const ImVec2 t0(at.x - w * 0.07f, at.y + h * 0.61f), t1(at.x + w * 0.04f, at.y + h * 0.73f);
    d->AddRectFilled(s0, s1, fill("m5"), w * 0.03f), d->AddRect(s0, s1, kit::kInk, w * 0.03f, 0, line);
    d->AddRectFilled(t0, t1, fill("m4"), w * 0.03f), d->AddRect(t0, t1, kit::kInk, w * 0.03f, 0, line);
    const ImVec2 b0(at.x, at.y), b1(at.x + w, at.y + h);
    d->AddRectFilled(b0, b1, lit.mouse_all ? kLitInk : kit::kPaperLight, r);
    const float split = at.y + h * 0.42f, mid = at.x + w * 0.5f;
    d->AddRectFilled(b0, ImVec2(mid, split), fill("lmb"), r, ImDrawFlags_RoundCornersTopLeft);
    d->AddRectFilled(ImVec2(mid, at.y), ImVec2(b1.x, split), fill("rmb"), r, ImDrawFlags_RoundCornersTopRight);
    d->AddRect(b0, b1, kit::kInk, r, 0, line * 1.3f);
    d->AddLine(ImVec2(b0.x + line, split), ImVec2(b1.x - line, split), kit::kInk, line);
    d->AddLine(ImVec2(mid, at.y), ImVec2(mid, split), kit::kInk, line);
    const ImVec2 w0(mid - w * 0.07f, at.y + h * 0.12f), w1(mid + w * 0.07f, at.y + h * 0.32f);
    d->AddRectFilled(w0, w1, fill("wheel"), w * 0.07f), d->AddRect(w0, w1, kit::kInk, w * 0.07f, 0, line);
  }

  // The controller (a generic two stick pad, no maker's marks), its box 600 by 420 at scale `s` from `at`.
  void PadArt(ImDrawList* d, ImVec2 at, float s, const DeviceLit& lit) {
    const kit::Fonts& f = kit::GetFonts();
    auto P = [&](float x, float y) { return ImVec2(at.x + x * s, at.y + y * s); };
    const float line = std::max(1.2f, 2.4f * s);
    const ImU32 body = IM_COL32(236, 228, 204, 255);
    auto fill = [&](const char* part) {
      return lit.pad_sel == part ? kit::kStamp : lit.pad.count(part) ? kLitInk : kit::kPaperLight;
    };
    auto box = [&](float x0, float y0, float x1, float y1, float r, const char* part, ImDrawFlags fl = 0) {
      d->AddRectFilled(P(x0, y0), P(x1, y1), fill(part), r * s, fl);
      d->AddRect(P(x0, y0), P(x1, y1), kit::kInk, r * s, fl, line);
    };
    box(130, 36, 218, 74, 16, "lt", ImDrawFlags_RoundCornersTop), box(382, 36, 470, 74, 16, "rt", ImDrawFlags_RoundCornersTop);
    box(116, 76, 238, 106, 14, "lb"), box(362, 76, 484, 106, 14, "rb");
    // the body and its grips as one shape: each drawn a line wider in ink, then all of them filled
    const float deg = 3.14159265f / 180.0f, o = line;
    d->AddRectFilled(ImVec2(P(100, 98).x - o, P(100, 98).y - o), ImVec2(P(500, 266).x + o, P(500, 266).y + o), kit::kInk, 84 * s + o);
    d->AddEllipseFilled(P(160, 262), ImVec2(74 * s + o, 108 * s + o), kit::kInk, 24 * deg, 48);
    d->AddEllipseFilled(P(440, 262), ImVec2(74 * s + o, 108 * s + o), kit::kInk, -24 * deg, 48);
    d->AddRectFilled(P(100, 98), P(500, 266), body, 84 * s);
    d->AddEllipseFilled(P(160, 262), ImVec2(74 * s, 108 * s), body, 24 * deg, 48);
    d->AddEllipseFilled(P(440, 262), ImVec2(74 * s, 108 * s), body, -24 * deg, 48);
    auto stick = [&](float x, float y, const char* part) {
      d->AddCircleFilled(P(x, y), 38 * s, body, 40), d->AddCircle(P(x, y), 38 * s, kit::kInk, 40, line);
      d->AddCircleFilled(P(x, y), 28 * s, fill(part), 40), d->AddCircle(P(x, y), 28 * s, kit::kInk, 40, line);
      d->AddCircle(P(x, y), 19 * s, kit::kInk, 32, line * 0.6f);
    };
    stick(185, 178, "ls"), stick(365, 258, "rs");
    // the D-pad: its two arms outlined, then filled over the join
    d->AddRect(P(234, 222), P(256, 294), kit::kInk, 3 * s, 0, line), d->AddRect(P(209, 247), P(281, 269), kit::kInk, 3 * s, 0, line);
    d->AddRectFilled(ImVec2(P(234, 222).x + line * 0.5f, P(234, 222).y + line * 0.5f), ImVec2(P(256, 294).x - line * 0.5f, P(256, 294).y - line * 0.5f), fill("dpad"), 3 * s);
    d->AddRectFilled(ImVec2(P(209, 247).x + line * 0.5f, P(209, 247).y + line * 0.5f), ImVec2(P(281, 269).x - line * 0.5f, P(281, 269).y - line * 0.5f), fill("dpad"), 3 * s);
    box(253, 171, 279, 185, 7, "back"), box(321, 171, 347, 185, 7, "start");
    d->AddCircleFilled(P(300, 132), 17 * s, kit::kPaperLight, 32), d->AddCircle(P(300, 132), 17 * s, kit::kInk, 32, line);
    d->AddCircle(P(300, 132), 9 * s, kit::kInk, 24, line * 0.6f);
    static const std::tuple<const char*, const char*, float, float> kFace[] = {
        {"y", "Y", 428, 140}, {"x", "X", 393, 175}, {"b", "B", 463, 175}, {"a", "A", 428, 210}};
    for (const auto& [part, label, x, y] : kFace) {
      d->AddCircleFilled(P(x, y), 18 * s, fill(part), 32), d->AddCircle(P(x, y), 18 * s, kit::kInk, 32, line);
      const float size = 20 * s;
      const bool on = lit.pad_sel == part || lit.pad.count(part);
      if (size >= 5.0f) SmallLabel(d, f.ui_bold, size, P(x, y), on ? kit::kCream : kit::kInk, label);
    }
  }

  // ---- the pictures (TekRantGaming, 16 Nov 2026: "a nice, high-quality image of the controller that's actually being
  // used, so the PlayStation one, the Xbox version too, etc. For Steam Deck, just use the Steam controller, the new one.
  // And ... an image of the keyboard and mouse as well that matches"): our own shaded drawings
  // (runs\ui-designs\build_devices.py) in glyphs\devices beside the exe, no maker's logos, with anchors.txt: each
  // picture's size and its buttons' places, so the chosen one is lit over it. The line drawings above stay for a
  // missing file.
  struct DevicePicture {
    float w = 0, h = 0;
    std::map<std::string, std::vector<float>> parts;  // part x y radius; grid x y key; box x y width height
  };
  const DevicePicture* DevicePictureOf(const std::string& name) {
    static std::map<std::string, DevicePicture> table;
    static bool read = false;
    if (!read) {
      read = true;
      std::ifstream in(ExecutableDir() / "glyphs" / "devices" / "anchors.txt");
      for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // a Windows line end read on Linux
        if (line.empty() || line[0] == '#') continue;
        std::istringstream words(line);
        std::string id;
        DevicePicture p;
        if (!(words >> id >> p.w >> p.h)) continue;
        std::string part;
        std::vector<float>* at = nullptr;
        for (std::string w; words >> w;) {
          char* end = nullptr;
          const float v = std::strtof(w.c_str(), &end);
          if (end && *end == 0 && at) at->push_back(v);
          else at = &p.parts[w];
        }
        table[id] = std::move(p);
      }
      REXLOG_INFO("BF settings: {} device pictures", table.size());
    }
    const auto it = table.find(name);
    return it == table.end() ? nullptr : &it->second;
  }
  fs::path DevicePictureFile(const std::string& name) { return ExecutableDir() / "glyphs" / "devices" / (name + ".png"); }
  // The controller in use, as the prompts show it: the Button prompts setting, or (Automatic, or the keyboard's
  // prompts chosen) the controller last pressed (game_menu.cpp PadFamily). The Steam Deck's is the Steam Controller.
  std::string PadPictureName() {
    std::string v = Cvar("bf_button_prompts");
    if (v == "keyboard") v = "auto";
    switch (kit::StyleFor(v, false)) {
      case kit::Style::kXbox360: return "xbox360";
      case kit::Style::kPlayStation3: return "ps3";
      case kit::Style::kPlayStation5: return "ps5";
      case kit::Style::kSteamDeck: return "steam";
      default: return "xbox_series";
    }
  }

  // A part lit over a picture: green, or red for the chosen one (a ring, filled a little).
  static void LitRing(ImDrawList* d, ImVec2 c, float r, bool chosen) {
    const ImU32 col = chosen ? IM_COL32(220, 60, 46, 255) : IM_COL32(143, 199, 79, 255);
    d->AddCircleFilled(c, r * 1.12f, chosen ? IM_COL32(220, 60, 46, 70) : IM_COL32(143, 199, 79, 55), 40);
    d->AddCircle(c, r * 1.12f, col, 40, std::max(2.0f, r * 0.13f));
  }

  // The controller in use, centred in a box `w` by at most `max_h` from `at`, its lit buttons ringed. Its height
  // (0 when the picture is missing: the caller draws the line drawing).
  float PadPicture(ImDrawList* d, ImVec2 at, float w, float max_h, const DeviceLit& lit, bool draw = true) {
    const std::string name = PadPictureName();
    const DevicePicture* p = DevicePictureOf(name);
    if (!p || p->w <= 0 || p->h <= 0) return 0;
    const float k = std::min(w / p->w, max_h / p->h), pw = p->w * k, ph = p->h * k;
    if (!draw) return ph;
    const ImVec2 a(at.x + (w - pw) * 0.5f, at.y), b(a.x + pw, a.y + ph);
    if (!kit::DrawImageFile(d, DevicePictureFile(name), a, b, true, false, 1600)) return 0;
    auto ring = [&](const std::string& part, bool chosen) {
      const auto it = p->parts.find(part);
      if (it == p->parts.end() || it->second.size() < 3) return;
      LitRing(d, ImVec2(a.x + it->second[0] * k, a.y + it->second[1] * k), it->second[2] * k, chosen);
    };
    for (const std::string& part : lit.pad) ring(part, false);
    if (!lit.pad_sel.empty()) ring(lit.pad_sel, true);
    return ph;
  }

  // The keyboard picture `w` wide from `at`, its lit keys over it; its height (0: missing).
  float KeyboardPicture(ImDrawList* d, ImVec2 at, float w, const DeviceLit& lit, bool draw = true) {
    const DevicePicture* p = DevicePictureOf("keyboard");
    if (!p || p->w <= 0) return 0;
    const auto grid = p->parts.find("grid");
    if (grid == p->parts.end() || grid->second.size() < 3) return 0;
    const float k = w / p->w, h = p->h * k;
    if (!draw) return h;
    if (!kit::DrawImageFile(d, DevicePictureFile("keyboard"), at, ImVec2(at.x + w, at.y + h), true, false, 1800)) return 0;
    const float gx = at.x + grid->second[0] * k, gy = at.y + grid->second[1] * k, u = grid->second[2] * k, gap = u * 0.06f;
    for (const ArtKey& key : KeyboardKeys()) {
      if (!*key.id) continue;
      const bool sel = lit.key_sel == key.id, on = lit.keys.count(key.id) > 0;
      if (!sel && !on) continue;
      const ImVec2 k0(gx + key.x * u + gap, gy + key.y * u + gap), k1(gx + (key.x + key.w) * u - gap, gy + (key.y + 1) * u - gap);
      d->AddRectFilled(k0, k1, sel ? IM_COL32(220, 60, 46, 150) : IM_COL32(143, 199, 79, 120), u * 0.1f);
      d->AddRect(k0, k1, sel ? IM_COL32(240, 80, 60, 255) : IM_COL32(160, 214, 92, 255), u * 0.1f, 0, std::max(1.5f, u * 0.05f));
    }
    return h;
  }

  // The mouse picture, its body `h` high, the body's top left at `at`; its lit parts over it. False: missing.
  bool MousePicture(ImDrawList* d, ImVec2 at, float h, const DeviceLit& lit) {
    const DevicePicture* p = DevicePictureOf("mouse");
    if (!p) return false;
    const auto box = p->parts.find("box");
    if (box == p->parts.end() || box->second.size() < 4) return false;
    const float k = h / box->second[3];
    const ImVec2 a(at.x - box->second[0] * k, at.y - box->second[1] * k);
    if (!kit::DrawImageFile(d, DevicePictureFile("mouse"), a, ImVec2(a.x + p->w * k, a.y + p->h * k), true, false, 800)) return false;
    // the lit parts as MouseArt places them, see through
    const float w = box->second[2] * k, r = w * 0.48f, split = at.y + h * 0.42f, mid = at.x + w * 0.5f;
    auto tint = [&](const char* part) -> ImU32 {
      if (lit.mouse_sel == part) return IM_COL32(220, 60, 46, 150);
      if (lit.mouse.count(part)) return IM_COL32(143, 199, 79, 120);
      return 0;
    };
    if (ImU32 c = tint("lmb")) d->AddRectFilled(at, ImVec2(mid, split), c, r, ImDrawFlags_RoundCornersTopLeft);
    if (ImU32 c = tint("rmb")) d->AddRectFilled(ImVec2(mid, at.y), ImVec2(at.x + w, split), c, r, ImDrawFlags_RoundCornersTopRight);
    if (ImU32 c = tint("wheel")) d->AddRectFilled(ImVec2(mid - w * 0.08f, at.y + h * 0.11f), ImVec2(mid + w * 0.08f, at.y + h * 0.33f), c, w * 0.08f);
    if (ImU32 c = tint("m5")) d->AddRectFilled(ImVec2(at.x - w * 0.09f, at.y + h * 0.47f), ImVec2(at.x + w * 0.06f, at.y + h * 0.59f), c, w * 0.03f);
    if (ImU32 c = tint("m4")) d->AddRectFilled(ImVec2(at.x - w * 0.09f, at.y + h * 0.61f), ImVec2(at.x + w * 0.06f, at.y + h * 0.73f), c, w * 0.03f);
    if (lit.mouse_all) d->AddRect(ImVec2(at.x - 3, at.y - 3), ImVec2(at.x + w + 3, at.y + h + 3), IM_COL32(160, 214, 92, 255), r, 0, std::max(2.0f, h * 0.012f));
    return true;
  }

  // The keyboard above the mouse (the help card), `w` wide; its height.
  float KeyboardMouseArt(ImDrawList* d, ImVec2 at, float w, const DeviceLit& lit, bool draw = true) {
    if (const float kh = KeyboardPicture(d, at, w, lit, false); kh > 0) {  // the pictures
      const float mh = w * 0.34f, gap = w * 0.02f;
      if (draw) {
        KeyboardPicture(d, at, w, lit);
        MousePicture(d, ImVec2(at.x + (w - mh * 0.66f) * 0.5f, at.y + kh + gap), mh, lit);
      }
      return kh + gap + mh * 1.08f;
    }
    const float u = w / 19.2f, kh = 7.1f * u, mh = std::min(w * 0.42f, 6.2f * u);
    if (draw) {
      KeyboardArt(d, ImVec2(at.x + 0.35f * u, at.y + 0.35f * u), u, lit);
      MouseArt(d, ImVec2(at.x + (w - mh * 0.66f) * 0.5f, at.y + kh + u * 0.6f), mh, lit);
    }
    return kh + u * 0.6f + mh;
  }

  // What the chosen row of a device page lights in its drawing.
  DeviceLit LitOf(int device, const SettingRow& r) {
    DeviceLit lit;
    if (device == 0) {
      if (!r.slots.empty()) {
        for (size_t k = 0; k < r.slots.size(); ++k) {
          const std::string& v = r.slots[k];
          if (v.empty()) continue;
          const bool cursor = int(k) == r.slot_cursor || int(k) == r.slot_capture;
          if (k == 0 && r.slots.size() == 5 && !r.slot_header.empty()) {  // a flight row's mouse movement
            lit.mouse_all = true;
          } else if (MouseKeyName(v)) {
            const std::string part = v == "LMB" ? "lmb" : v == "RMB" ? "rmb" : v == "MB4" || v == "Mouse4" ? "m4"
                                     : v == "MB5" || v == "Mouse5" ? "m5" : "wheel";
            if (cursor) lit.mouse_sel = part;
            else lit.mouse.insert(part);
          } else {
            for (size_t at = 0; at <= v.size();) {  // "Shift+Up": both keys
              size_t plus = v.find('+', at);
              if (plus == std::string::npos) plus = v.size();
              std::string key = v.substr(at, plus - at);
              if (key == "Ctrl") key = "Control";
              if (cursor) lit.key_sel = key;
              lit.keys.insert(key);
              at = plus + 1;
            }
          }
        }
      } else if (r.section == "Mouse" || r.section == "Invert") {
        lit.mouse_all = true;
      }
      return lit;
    }
    if (r.section == "Buttons" && r.pad_glyphs && !r.slots.empty() && !r.slots[0].empty()) {
      std::string id = r.slots[0];
      if (id == "up" || id == "down" || id == "left" || id == "right") id = "dpad";
      lit.pad_sel = id;
    } else if (r.section == "Sticks" && r.pad_glyphs && !r.slots.empty() && !r.slots[0].empty()) {
      lit.pad_sel = r.slots[0].find('l') != std::string::npos ? "ls" : "rs";  // the stick its axis comes from
    } else if (r.section == "Look" || r.section == "Invert") {
      lit.pad.insert(r.name == "Left stick" ? "ls" : "rs");
    } else if (r.section == "Aim") {
      lit.pad.insert("lt");
    }
    return lit;
  }

  // ---- a device's page: its rows (pa..pb), and on the help card (ha..hb) the device drawn under the chosen row's
  // name, its keys or buttons lit (the help's words under the picture). Settings > Controls and the first start's
  // Controls step both draw it (TekRantGaming, 16 Nov 2026: the setup's Controls page should look as Settings').
  void ControlsDevicePage(ImDrawList* d, ImGuiIO& io, const Stage& st, Input in, ImVec2 pa, ImVec2 pb, ImVec2 ha,
                          ImVec2 hb, int device_page) {
    const float art_w = hb.x - ha.x - st(36);
    const float pad_s = art_w / 600.0f * 0.95f;
    // the controller's picture (PadPicture), or its line drawing when the file is missing
    const float pad_h = PadPicture(d, ha, art_w, st(220), DeviceLit{}, false);
    const float art_h = device_page == 0 ? KeyboardMouseArt(d, ha, art_w, DeviceLit{}, false)
                        : pad_h > 0     ? pad_h
                                        : 410.0f * pad_s;
    help_top_ = art_h + st(16);
    SettingsBody(d, io, st, in, pa, pb, ha, hb);
    help_top_ = 0;
    const std::vector<SettingRow> rows = RowsOf(tab_);
    if (!rows.empty()) {
      const SettingRow& r = rows[size_t(std::clamp(row_, 0, int(rows.size()) - 1))];
      ctl_bind_row_ = !r.slots.empty();
      const DeviceLit lit = LitOf(device_page, r);
      const ImVec2 at(ha.x + st(18), ha.y + st(40));
      if (device_page == 0) KeyboardMouseArt(d, at, art_w, lit);
      else if (pad_h <= 0 || PadPicture(d, at, art_w, st(220), lit) <= 0)
        PadArt(d, ImVec2(at.x + (art_w - 600 * pad_s) * 0.5f, at.y - 30 * pad_s), pad_s, lit);
    }
  }

  // ---- the choice: two cards over the rows' and the help card's place (a..b)
  void ControlsChoice(ImDrawList* d, ImGuiIO& io, const Stage& st, const Input& in, ImVec2 a, ImVec2 b) {
    const kit::Fonts& f = kit::GetFonts();
    if (ctl_fresh_) row_ = KeyboardLast() ? 0 : 1, ctl_fresh_ = false;
    row_ = std::clamp(row_, 0, 1);
    if (in.left || in.up) row_ = 0;
    if (in.right || in.down) row_ = 1;
    const int last = KeyboardLast() ? 0 : 1;
    const float gap = st(21), w = (b.x - a.x - gap) * 0.5f;
    int open = -1;
    static const char* kKick[2] = {"01 \xC2\xB7 KEYBOARD AND MOUSE", "02 \xC2\xB7 CONTROLLER"};
    static const char* kName[2] = {"KEYBOARD & MOUSE", "CONTROLLER"};
    static const std::pair<const char*, const char*> kList[2][4] = {
        {{"MOUSE", "Sensitivity, aiming, vehicles, hold or toggle aim"},
         {"INVERT", "On foot, vehicles, planes"},
         {"PLANE FLIGHT", "Pitch, yaw, roll on keys or the mouse"},
         {"BINDINGS", "28 actions \xC2\xB7 two keys, two mouse buttons"}},
        {{"LOOK", "Sensitivity, response, dead zone, invert"},
         {"AIM", "Hold or toggle, aim assist and its strength"},
         {"FEEL", "Vibration, the game's own layouts"},
         {"BUTTONS", "16 buttons, each to any action"}}};
    for (int k = 0; k < 2; ++k) {
      const ImVec2 c0(a.x + float(k) * (w + gap), a.y), c1(c0.x + w, b.y);
      const bool over = ImGui::IsMouseHoveringRect(c0, c1);
      if (over && io.MouseDelta.x + io.MouseDelta.y != 0) row_ = k;
      if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) open = k;
      const bool sel = row_ == k;
      kit::PaperArt(d, c0, c1);
      d->AddRect(c0, c1, kit::kBorder, 0, 0, st(1.3f));
      kit::Spaced(d, f.typed, st(9), ImVec2(c0.x + st(21), c0.y + st(19)), kit::kInkSection, kKick[k], st(2.2f));
      if (k == last) {  // the stamp on the device used last
        const char* t = "LAST USED";
        const float size = st(15), tw = kit::GameTextWidth(kit::GameFont::kHeadlineMedium, size, t);
        const ImVec2 s1(c1.x - st(20), c0.y + st(14)), s0(s1.x - tw - st(18), s1.y);
        const ImVec2 s2(s1.x, s1.y + size + st(14));
        d->AddRect(s0, s2, kit::kStamp, 0, 0, st(2));
        kit::GameText(d, kit::GameFont::kHeadlineMedium, size, ImVec2(s0.x + st(9), s0.y + st(7)), kit::kStamp, t);
      }
      // the drawing, centred in its band
      const ImVec2 p0(c0.x + st(22), c0.y + st(44)), p1(c1.x - st(22), c0.y + st(250));
      DeviceLit none;
      if (k == 0) {
        const float kw = (p1.x - p0.x) * 0.80f;
        if (const float kh = KeyboardPicture(d, p0, kw, none, false); kh > 0) {  // the pictures, the mouse beside
          const float mh = kh * 0.92f, total = kw + mh * 0.66f * 0.75f;
          const ImVec2 kat((p0.x + p1.x - total) * 0.5f, (p0.y + p1.y - kh) * 0.5f);
          KeyboardPicture(d, kat, kw, none);
          MousePicture(d, ImVec2(kat.x + kw - mh * 0.66f * 0.25f, kat.y + kh * 0.30f), mh, none);
        } else {
          const float u = kw / 19.2f, mh = 6.4f * u;
          const float total = kw + u * 1.2f + mh * 0.66f, x = (p0.x + p1.x - total) * 0.5f, y = (p0.y + p1.y - 7.1f * u) * 0.5f;
          KeyboardArt(d, ImVec2(x + 0.35f * u, y + 0.35f * u), u, none);
          MouseArt(d, ImVec2(x + kw + u * 1.2f, y + (7.1f * u - mh) * 0.5f), mh, none);
        }
      } else {
        const float ph = PadPicture(d, p0, p1.x - p0.x, p1.y - p0.y, none, false);
        if (ph <= 0 || PadPicture(d, ImVec2(p0.x, (p0.y + p1.y - ph) * 0.5f), p1.x - p0.x, p1.y - p0.y, none) <= 0) {
          const float s = std::min((p1.x - p0.x) / 600.0f, (p1.y - p0.y) / 400.0f) * 0.92f;
          PadArt(d, ImVec2((p0.x + p1.x) * 0.5f - 300 * s, (p0.y + p1.y) * 0.5f - 200 * s), s, none);
        }
      }
      kit::GameText(d, kit::GameFont::kHeadline, st(30), ImVec2(c0.x + st(20), c0.y + st(266)), kit::kInk, kName[k]);
      float y = c0.y + st(316);
      for (const auto& [key, value] : kList[k]) {
        kit::Spaced(d, f.typed, st(8), ImVec2(c0.x + st(21), y + st(5)), kit::kInkSection, key, st(1.8f));
        const float vw = kit::TextWidth(f.ui_bold, st(12), value);
        d->AddText(f.ui_bold, st(12), ImVec2(c1.x - st(21) - vw, y), kit::kInk, value);
        d->AddLine(ImVec2(c0.x + st(21), y + st(24)), ImVec2(c1.x - st(21), y + st(24)), kit::kRule, 1.0f);
        y += st(30);
      }
      // the band at the foot: OPEN, green with the red tab on the chosen card
      const ImVec2 o0(c0.x, c1.y - st(41)), o1 = c1;
      d->AddRectFilled(o0, o1, sel ? kit::kGreen : kit::kPaperDim);
      if (sel) d->AddRectFilled(o0, ImVec2(o0.x + st(5), o1.y), kit::kStamp);
      float x = o0.x + st(21);
      if (sel) x += kit::Glyph(d, PromptStyle(), kit::Button::kA, KeyFor(kit::Button::kA), ImVec2(x, (o0.y + o1.y) * 0.5f), st(21)) + st(9);
      kit::Spaced(d, f.ui_bold, st(13.5f), ImVec2(x, o0.y + (st(41) - st(13.5f) * 1.45f) * 0.5f), sel ? kit::kCream : kit::kInkFaint,
                  "OPEN", st(2));
      if (k == last) {
        const float tw = kit::SpacedWidth(f.typed, st(8), "LAST USED", st(1.8f));
        kit::Spaced(d, f.typed, st(8), ImVec2(o1.x - st(21) - tw, o0.y + st(16)), sel ? kit::kCream : kit::kInkSection, "LAST USED", st(1.8f));
      }
      if (sel) d->AddRect(c0, c1, kit::kGreen, 0, 0, st(3));
    }
    if (open < 0 && in.accept) open = row_;
    if (open >= 0) {
      REXLOG_INFO("BF settings: CONTROLS / {} opened", open == 0 ? "keyboard and mouse" : "controller");
      ctl_device_ = open, row_ = 0, scroll_ = 0, slot_cursor_ = 0;
    }
  }

  // The rail's card under the categories while Controls is open: the two devices, the one used last marked.
  void ControlsDevicesCard(ImDrawList* d, const Stage& st, ImVec2 ra, ImVec2 rb, float py) {
    const kit::Fonts& f = kit::GetFonts();
    kit::Spaced(d, f.typed, st(9.5f), ImVec2(ra.x + st(20), py), kit::kKhakiDim, "YOUR DEVICES", st(2));
    const bool keyboard = KeyboardLast();
    auto device_row = [&](float yy, const char* name, bool last, bool open) {
      d->AddText(f.ui, st(11.5f), ImVec2(ra.x + st(20), yy), open ? kit::kCream : kit::kKhakiText, name);
      if (last) {
        const float tw = kit::SpacedWidth(f.typed, st(8), "LAST USED", st(1.6f));
        kit::Spaced(d, f.typed, st(8), ImVec2(rb.x - st(20) - tw, yy + st(3)), kit::kGreenBright, "LAST USED", st(1.6f));
      }
      d->AddRectFilled(ImVec2(ra.x + st(20), yy + st(21)), ImVec2(rb.x - st(20), yy + st(22)), IM_COL32(241, 234, 211, 20));
    };
    device_row(py + st(18), "Keyboard and mouse", keyboard, ctl_device_ == 0);
    device_row(py + st(45), "Controller", !keyboard, ctl_device_ == 1);
  }
