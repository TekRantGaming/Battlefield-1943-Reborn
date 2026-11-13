// Driving the game's own menus under ours (TekRantGaming, 4 Nov 2026: our menu replaces BF's, King Kong style).
//
// The game boots as usual and the player presses Start on its title (tests: we do); from the profile's settings on
// we press through BF's sign in on player 1's pad until the online menu is up, then empty BF's button list (loadMovie("", "_root.menuMC.startMc") on its
// Flash player) so its animated scene and logo stay under our menu
// (game_menu.cpp) is drawn over it. While our menu is open the game's pad reads come back empty, so BF's hidden
// menu never moves. Joining a picked server: A (online: the EA login, then "presenceConnected"), A (Quick Match:
// "queryBegin"), done when loading starts ("onStartLoading"); the backend has already reserved the server for the
// player's next Quick Match. Steps timed by the menu's own events, as the dev pad script that joined in runs ded10
// and mp1. BF_AUTO_JOIN=1 joins at once (test runs).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <algorithm>
#include <string>
#include <vector>
#include <array>
#include <cstdio>
#include <cmath>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/ui/keybinds.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include "capture_points.h"
#include "comms.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

REXCVAR_DEFINE_BOOL(bf_skip_loading_complete, true, "BF1943",
                    "After a join, the game's Loading complete goes straight on to the deploy screen (its own Start)");
REXCVAR_DEFINE_BOOL(bf_join_open_squad, false, "BF1943",
                    "Entering a match joins an open squad (the game's Start in open squad at Loading complete)");

namespace bf1943 {

void ApplyControllerSettings(uint8_t* gamepad, bool menu_open, bool seated);  // input_settings.cpp
void LookPad(uint8_t* gamepad, bool seated);                     // look.cpp
bool LocalOnFoot();                                              // look.cpp: the vehicle seeker
bool LocalSeated(const uint8_t* base);                           // look.cpp: in a vehicle, any seat
void HudGlyphsTick(bool seated);                                 // hud_glyphs.cpp: the prompts drawn again on a change
void NoteLevelChange();                                          // modern_mode.cpp: scan the new level's values
#if defined(BF_DEV_TOOLS)
extern std::atomic<float> g_dev_camera[3];                       // look.cpp: the local camera's position
extern std::atomic<float> g_dev_view_pos[3];                     // dev_matscan.cpp: the rendered camera's
void DevTeamScan(const uint8_t* base, bool in_world);              // dev_team_scan.cpp: BF_DEV_TEAMSCAN
#endif
bool TickBlendNearestTree(uint32_t parts, const float from[3], float out[3], double* dist);  // tick_blend.cpp
// game_menu.cpp
bool GameMenuOpen();
void SpawnMousePad(uint8_t* gamepad, bool allowed, bool out_of_world);  // flash_mouse.cpp: the deploy screen's pointer
bool SpawnMouseCommand(const char* name, const char* args);    // flash_mouse.cpp: true, not to be passed on
bool GameMenuEnabled();
void GameMenuPad(uint16_t buttons, int16_t lx, int16_t ly);
void ShowGameMenu();
void ShowPauseMenu();
void PressBf(const std::vector<uint16_t>& presses);
bool GameMenuPassing();
void NoteMatchDevice(bool keyboard);  // game_menu.cpp: the device used in the match, for the HUD's prompts
void NoteGameMenuSignal();  // game_menu.cpp: BF's own menu screen is up
void ShowLostNotice();
bool LauncherLetsGameSignIn();  // game_menu.cpp: BF signs in behind the launcher

namespace {

using Clock = std::chrono::steady_clock;

PPCFunc* RuntimeExport(const char* name) {
#if defined(_WIN32)
  static HMODULE runtime = GetModuleHandleA("rexruntime.dll");
  return reinterpret_cast<PPCFunc*>(GetProcAddress(runtime, name));
#else
  return reinterpret_cast<PPCFunc*>(dlsym(RTLD_NEXT, name));
#endif
}

constexpr uint16_t kStart = 0x0010, kA = 0x1000;

enum class Stage { kBoot, kTitleDone, kAtMenu, kOnline, kQuickMatch, kInGame, kFailed, kReturning };

struct Driver {
  std::mutex mutex;
  Stage stage = Stage::kBoot;
  Clock::time_point stage_at = Clock::now(), press_until, next_press;
  uint16_t press = 0;
  int step = 0, tries = 0;
  std::string server;
  bool seen_settings = false, seen_presence = false, seen_query = false, seen_loading = false;
  bool seen_ready = false;            // "gameIsReady" after the level started loading
  bool load_screen = false;           // the level's loading screen is up (its first loadTimeUpdate after the load began)
  bool entered = false;               // Loading complete's Start sent for the player (bf_skip_loading_complete)
  bool spawning = false;              // the spawn presses are queued (LevelReady)
  Clock::time_point spawn_at, ready_at;
  Clock::time_point last_load_update;  // the loading screen's last "loadTimeUpdate"
  std::deque<std::pair<double, uint16_t>> script;  // presses for BF's pause menu: seconds to wait, buttons
  Clock::time_point script_at;
  bool left = false;              // EA's layer left the game (we left, were kicked, or the connection dropped)
  uint32_t disconnect_reason = 0;  // the game client's disconnect reason: 0 ok, 4 kicked, ...
  bool ended = false, lost = false;  // the connection ended; and without BF leaving (the server went away)
  bool exit_chosen = false;          // the player chose Exit Game (no notice follows)
  bool player_ok = false;            // the player's own A on BF's autosave notice (the sign in's A)
  int dialogs = 0;                    // the game's dialogs open now (its menu's numDialogs command)
  Clock::time_point dialog_at{};      // when the first of them opened
  int notice_presses = 0;             // our A presses on the autosave notice in this start
  Clock::time_point notice_at{};      // the last of them
  Clock::time_point ended_at;
  std::deque<std::string> flash_calls;  // ActionScript calls to make, from the next pad read
  uint32_t ui = 0;                      // the UI object, from the game's own calls
} g;

std::atomic<bool> g_walking{false};  // pressing buttons for the player (signing in, joining)

// TekRantGaming, 7 Nov 2026: the player sees the game's own films and title and presses Start themselves (no bare menu over
// them), and spawns from BF's own screens after its own loading screen (picking a class). Test runs (BF_AUTO_JOIN=1,
// dev builds) still press through the title and spawn by themselves.
bool AutoTest() {
#if defined(BF_DEV_TOOLS)
  static const bool on = [] {
    const char* v = std::getenv("BF_AUTO_JOIN");
    const char* s = std::getenv("BF_SPAWN_PRESSES");
    return (v && *v == '1') || (s && *s);
  }();
  return on;
#else
  return false;
#endif
}

void Enter(Stage s) {
  g.stage = s;
  g.stage_at = Clock::now();
  g.step = 0;
  g.tries = 0;
  g.notice_presses = 0;
  REXLOG_INFO("BF menu driver: stage {}", static_cast<int>(s));
}

void Press(uint16_t buttons, int ms = 300) {
  g.press = buttons;
  g.press_until = Clock::now() + std::chrono::milliseconds(ms);
}

double Since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

uint16_t Step() {
  std::lock_guard<std::mutex> lock(g.mutex);
  const auto now = Clock::now();
  if (g.seen_loading && (g.stage == Stage::kOnline || g.stage == Stage::kQuickMatch)) {
    Enter(Stage::kInGame);
    g_walking = false;
  }
  if (!g.script.empty() && Since(g.script_at) >= g.script.front().first) {
    Press(g.script.front().second, 200);  // the dev pad's hold, which BF's pause menu took (run pz2)
    REXLOG_INFO("BF menu driver: press {:04X} ({} left)", g.script.front().second, g.script.size() - 1);
    g.script.pop_front();
    g.script_at = now;
  }
  // The level is loaded: BF waits on "Loading complete" for A, then the spawn screen (A, A): the presses of the pause
  // test that spawned in every test run. Without them the server ends the connection about 50 s later (run lj1).
  // The joining screen stays up until they are done (LevelReady). BF_PAUSE_TEST runs press on their own.
  static const bool pause_test = [] {
    const char* v = std::getenv("BF_PAUSE_TEST");
    return v && *v;
  }();
  // BF_PLAYER_SPAWN=1 (tests): joined by the test, spawned as a player would (nothing pressed).
  static const bool player_spawn = [] {
    const char* v = std::getenv("BF_PLAYER_SPAWN");
    return v && *v == '1';
  }();
  if (g.stage == Stage::kInGame && g.seen_ready && !g.spawning && !pause_test && AutoTest() && !player_spawn &&
      (Since(g.last_load_update) > 1.0 || Since(g.ready_at) > 4.0)) {
    g.spawning = true;
    g.spawn_at = now;
    // BF_SPAWN_PRESSES (dev builds, tests): the spawn screen's presses instead, buttons separated by commas (a, b, up,
    // down, left, right), 2 s apart: another spawn point than the default, the carrier.
    std::string presses = "a,a,a";
#if defined(BF_DEV_TOOLS)
    if (const char* v = std::getenv("BF_SPAWN_PRESSES"); v && *v) presses = v;
#endif
    for (size_t pos = 0, i = 0; pos <= presses.size(); ++i) {
      const size_t comma = std::min(presses.find(',', pos), presses.size());
      const std::string b = presses.substr(pos, comma - pos);
      // lsr, lsl: the left stick right or left (the spawn screen's kit choice), carried in the two button bits XInput
      // leaves unused and turned into the stick where the driver's press joins the pad (13 Nov 2026).
      const uint16_t button = b == "b" ? 0x2000 : b == "up" ? 0x0001 : b == "down" ? 0x0002 : b == "left" ? 0x0004
                              : b == "right" ? 0x0008 : b == "lsr" ? 0x0400 : b == "lsl" ? 0x0800 : kA;
      g.script.push_back({i ? 2.0 : 0.5, button});
      pos = comma + 1;
    }
    g.script_at = now;
    g_walking = true;
    REXLOG_INFO("BF menu driver: level loaded, spawning");
  }
  // Loading complete for the player: the game's own Start (enterInGame), or Start in open squad, once the level is
  // ready after a join (TekRantGaming, 10 Nov 2026: a join loads straight into the server; docs/MENUS-PLAN.md). Test runs
  // press through it as before.
  if (g.stage == Stage::kInGame && g.seen_ready && !g.entered && (!AutoTest() || player_spawn) && !pause_test &&
      REXCVAR_GET(bf_skip_loading_complete) && Since(g.ready_at) > 0.5) {
    g.entered = true;
    if (REXCVAR_GET(bf_join_open_squad)) {
      g.flash_calls.push_back("cmd:svSetKey|autoLookForSquad,1");
      g.flash_calls.push_back("cmd:svSetValue|1");
    }
    g.flash_calls.push_back("cmd:enterInGame|");
    REXLOG_INFO("BF menu driver: Loading complete: on to the deploy screen{}",
                REXCVAR_GET(bf_join_open_squad) ? " in an open squad" : "");
  }
  if (g.stage == Stage::kInGame && g.script.empty() && now >= g.press_until) g_walking = false;
  // The connection ended and BF did not leave on its own (run pz11, the server stopped): BF went back to its menu
  // with a "Connection problem" notice.
  if (g.ended && !g.left && g.stage == Stage::kInGame && Since(g.ended_at) > 2) {
    g.lost = true;
    g.left = true;
    REXLOG_INFO("BF menu driver: connection to the server lost");
    ShowLostNotice();
  }
  if (g.left && g.stage == Stage::kInGame) {  // out of the match: back to our menu once BF's menu is up again
    g.left = g.ended = false;
    g.script.clear();
    Enter(Stage::kReturning);
    g_walking = true;
  }
  switch (g.stage) {
    case Stage::kReturning:
      // A kick or a lost connection leaves a notice over BF's online menu: A dismisses it (one A only, the next one
      // would start a Quick Match, run pz12). Then BF's list is emptied again.
      // Any way out but Exit Game leaves one: an autokick at Loading complete gives reason 0 and BF's Kicked notice,
      // which our menu covered (run rot1mapb; its UI sends no event for it, run kicktrace1).
      if (g.step == 0 && (g.lost || g.disconnect_reason != 0 || !g.exit_chosen) && Since(g.stage_at) > 5)
        Press(kA), g.step = 1;
      else if (Since(g.stage_at) > 10) {
        g.lost = false;
        // BF's own list stays (our list is drawn over it, and its rows are pressed for the player).
        Enter(Stage::kAtMenu);
        g_walking = false;
        ShowGameMenu();
      }
      break;
    case Stage::kBoot:  // the films and the title screen: the player's Start (tests: ours every 2 s) until the profile's
                        // settings are loaded; then the sign in is ours, the player's pad held back meanwhile
      // The player's pad stays theirs: BF's autosave notice ("When you see this icon...", A OK) comes next, and the
      // player's A on it is the sign in's A (TekRantGaming, 7 Nov: the notice would not let him pass while we held the pad).
      // Behind the launcher's pages (game_menu_launcher.inc) the films and the title are skipped for the player once a
      // name is chosen, and the sign in goes on by itself: Play then finds BF's online menu up.
      if (g.seen_settings) Enter(Stage::kTitleDone), g_walking = AutoTest() || LauncherLetsGameSignIn();
      else if ((AutoTest() || LauncherLetsGameSignIn()) && now >= g.next_press)
        Press(kStart), g.next_press = now + std::chrono::milliseconds(500);
#if defined(BF_DEV_TOOLS)
      else if (const char* v = std::getenv("BF_PLAYER_START"); v && *v) {
        // BF_PLAYER_START=<s>,<s>... (tests of the start): Start at those seconds after the driver began, as the
        // player's own press.
        static const auto began = now;
        static size_t next = 0;
        std::vector<double> at;
        for (const char* c = v; *c;) {
          at.push_back(std::atof(c));
          while (*c && *c != ',') ++c;
          if (*c) ++c;
        }
        if (next < at.size() && Since(began) >= at[next]) {
          ++next;
          Press(kStart, 200);
          REXLOG_INFO("BF menu driver: test press of Start, as the player's");
        }
      }
#endif
      break;
    case Stage::kTitleDone:  // Start at 3 s and 7 s, A at 15 s (online: the EA login), as in runs ded10 and aj1
#if defined(BF_DEV_TOOLS)
      // BF_PLAYER_OK=<s> (tests of the start): the player's A on the autosave notice that long after it came.
      if (const char* v = std::getenv("BF_PLAYER_OK"); v && *v && g.step < 3 && !g.player_ok &&
          Since(g.stage_at) > std::atof(v)) {
        Press(kA, 200), g.player_ok = true;
        REXLOG_INFO("BF menu driver: test press of A on the autosave notice, as the player's");
      }
#endif
      if (g.step < 3 && g.player_ok) g.step = 3, g_walking = true;  // signed in by the player's A: ours now
      // A hidden sign in (our launcher, tests): the autosave notice opens with the profile's settings (numDialogs 1,
      // 0.4 s after the title's Start, run boottrace1), so its A goes one second after it is seen instead of at 15 s
      // (the two Starts before it did nothing there): about 14 s off every start. The timed presses below stay for a
      // start that shows no notice, and the player's own sign in keeps them (the notice stays up to be read).
      // Not in the stage's first second either: started from our launcher's Play the notice can be up before the
      // title is done, and an A at the stage's first moment was not taken; the sign in then waited 45 s for the
      // retry below and TekRantGaming gave up on "can't connect" (11 Nov, run pcopy2: 56 s from Play to the sign in). While
      // the notice stays up after our A, it is pressed again every 4 s (up to four times).
      else if (g.step < 3 && g_walking && g.dialogs > 0 && Since(g.dialog_at) > 1.0 && Since(g.stage_at) > 1.0) {
        Press(kA), g.step = 3, g.notice_presses = 1, g.notice_at = now;
        REXLOG_INFO("BF menu driver: the autosave notice is up: its A");
      }
      else if (g.step == 3 && !g.seen_presence && g.notice_presses > 0 && g.notice_presses < 4 && g.dialogs > 0 &&
               Since(g.notice_at) > 4.0) {
        Press(kA), ++g.notice_presses, g.notice_at = now;
        REXLOG_INFO("BF menu driver: the autosave notice is still up: its A again");
      }
      else if (g.step == 0 && Since(g.stage_at) > 3) Press(kStart), g.step = 1;
      else if (g.step == 1 && Since(g.stage_at) > 7) Press(kStart), g.step = 2;
      else if (g.step == 2 && Since(g.stage_at) > 15) Press(kA), g.step = 3, g_walking = true;
      else if (g.step == 3 && Since(g.stage_at) > 45) g.step = 2, g.stage_at = now - std::chrono::seconds(10);
      else if (g.step == 3 && g.seen_presence) {
        // Online, with BF's Quick Match row selected underneath: hide BF's content and open our menu.
        if (const char* v = std::getenv("BF_FLASH_TEST"); v && *v) {  // dev experiments: paths separated by commas
          const std::string list = v;
          for (size_t at = 0; at < list.size();) {
            const size_t comma = std::min(list.find(','), list.size()) == list.size() ? list.size() : list.find(',', at);
            const size_t end = comma == std::string::npos ? list.size() : comma;
            if (end > at) g.flash_calls.push_back(list.substr(at, end - at));
            at = end + 1;
          }
        }
        // BF's button list (menuMC.startMc) is emptied; its animated scene and logo stay, our menu sits over them.
        // BF's own list stays (our list is drawn over it, and its rows are pressed for the player).
        Enter(Stage::kAtMenu);
        g_walking = false;
        ShowGameMenu();
      }
      break;
    case Stage::kOnline:  // BF's menu is reloaded (StartAutoJoin); then the game's own Quick Match command until the
                          // query starts (the backend's reservation makes it the server picked; docs/MENUS-PLAN.md)
      // The command goes 1 s after the reload (it was 4 s while our driver pressed BF's own Quick Match row; the
      // command itself needs no row), then again 5 s apart until the query starts.
      if (g.seen_query) Enter(Stage::kQuickMatch);
      else if (Since(g.stage_at) > 1 + 5.0 * g.tries && g.tries < 4)
        g.flash_calls.push_back("cmd:startQuickMatch|1"), ++g.tries;
      else if (g.tries >= 4 && Since(g.stage_at) > 30) Enter(Stage::kFailed), g_walking = false;
      break;
    case Stage::kQuickMatch:  // waiting for the level to start loading
      if (Since(g.stage_at) > 60) Enter(Stage::kFailed), g_walking = false;
      break;
    default:
      break;
  }
  return now < g.press_until ? g.press : 0;
}

bool TraceOn() {
  static const bool on = [] {
    const char* v = std::getenv("BF_NET_TRACE");
    return v && *v && *v != '0';
  }();
  return on;
}

}  // namespace

// For game_menu.cpp: a click on the title screen, as the pad's Start (the game reads only the
// pad there).
void PressStartForPlayer() {
  std::lock_guard<std::mutex> lock(g.mutex);
  if (g.stage == Stage::kBoot) Press(kStart, 200);
  else if (g.stage == Stage::kTitleDone && g.step < 3) Press(kA, 200), g.player_ok = true;  // the autosave notice's OK
}

// The real pad's A during the sign in (the pad read): the autosave notice's OK, pressed as ours (the pad is held
// from then on, which blanked the player's own A in 0.1.9: the notice never closed, TekRantGaming), so ours is not pressed
// again.
void NotePlayerButtons(uint16_t pressed) {
  std::lock_guard<std::mutex> lock(g.mutex);
  if (g.stage == Stage::kTitleDone && g.step < 3 && (pressed & kA) && !g_walking) g.player_ok = true, Press(kA, 200);
}

// For game_menu.cpp: join (the server is reserved already), and the overlay's status.
void StartAutoJoin(const std::string& server) {
  std::lock_guard<std::mutex> lock(g.mutex);
  g.server = server;
  g.seen_query = g.seen_loading = g.seen_ready = g.spawning = g.entered = g.load_screen = false;
  g.ended = g.lost = g.exit_chosen = false;
  g.disconnect_reason = 0;
  // Our menu emptied BF's button list; reload BF's menu (as leaving a match does) so Quick Match is there again.
  g.flash_calls.push_back("_root.exitMainMenu");
  g.flash_calls.push_back("_root.enterMainMenu");
  Enter(Stage::kOnline);
  g_walking = true;
}

// For game_menu.cpp: the game's own tutorial (its Help & Options > Tutorial: the startTutorial command loads the
// training level, Levels/Wake_island_tutorial, run tutorial1), then the match's stages as after a join: its load,
// Loading complete, in the level.
void StartTutorial() {
  std::lock_guard<std::mutex> lock(g.mutex);
  g.server = "Tutorial";
  g.seen_query = g.seen_loading = g.seen_ready = g.spawning = g.entered = g.load_screen = false;
  g.ended = g.lost = g.exit_chosen = false;
  g.disconnect_reason = 0;
  g.flash_calls.push_back("cmd:startTutorial|");
  Enter(Stage::kQuickMatch);  // its load next (onStartLoading), as a join's
  g_walking = true;
}

// For our menus: a command as the game's own menu sends it (sub_826B6628: "startQuickMatch", "1"), run on the game's
// thread at its next pad read (FlashCalls).
void GameCommand(const std::string& name, const std::string& args) {
  std::lock_guard<std::mutex> lock(g.mutex);
  g.flash_calls.push_back("cmd:" + name + "|" + args);
}

// For our menus: an ActionScript function of the game's menu called with no arguments ("_root.enterMainMenu").
void FlashCall(const std::string& path) {
  std::lock_guard<std::mutex> lock(g.mutex);
  g.flash_calls.push_back(path);
}

// For the pause menu (game_menu.cpp): BF's own pause menu ("Frontline Report": My Squad, Scoreboard, Help & Options,
// Leaderboards, Achievements, Exit Game, Suicide; B resumes), walked from Start. 1: its scoreboard, 2: leave the
// match (Exit Game, then A on its confirmation, run pz2).
void RunPauseAction(int action) {
  std::lock_guard<std::mutex> lock(g.mutex);
  constexpr uint16_t kDown = 0x0002;
  g.script.clear();
  g.script.push_back({0.2, kStart});
  if (action == 1) {
    g.script.push_back({0.8, kDown});
    g.script.push_back({0.5, kA});
  } else if (action == 3) {  // dev test: spawn from Loading complete (no Start)
    g.script.clear();
    for (int i = 0; i < 3; ++i) g.script.push_back({i ? 2.0 : 0.1, kA});
  } else if (action == 4) {
    // Exit Game from BF's pause that is already open (our list over it): its sixth row, then the confirmation.
    g.script.clear();
    for (int i = 0; i < 5; ++i) g.script.push_back({i ? 0.45 : 0.15, kDown});
    g.script.push_back({1.0, kA});
    g.script.push_back({3.0, kA});
    g.script.push_back({4.0, kA});
  } else if (action == 2) {
    // The timing of run pz2: BF's pause menu takes presses about a second apart.
    for (int i = 0; i < 5; ++i) g.script.push_back({i ? 1.0 : 3.0, kDown});
    // Exit Game, then its confirmation: it comes up about 2.5 s later (run pz2). A second A in case the first came
    // early; leaving clears the rest of the script (Step), so it never reaches BF's menu.
    g.script.push_back({2.0, kA});
    g.script.push_back({4.0, kA});
    g.script.push_back({4.0, kA});
  }
  g.script_at = Clock::now();
  g_walking = true;
}

// Leaving the match (Exit Game) and Suicide, as the game's own pause rows do: their commands.
void LeaveMatch() {
  std::lock_guard<std::mutex> lock(g.mutex);
  g.exit_chosen = true;
  g.flash_calls.push_back("cmd:doExitLevel|");
}
void SuicideNow() {
  std::lock_guard<std::mutex> lock(g.mutex);
  g.flash_calls.push_back("cmd:suicide|");
}

// The player chose Exit Game in the pause (game_menu.cpp): no notice is left to dismiss after it.
void MarkExitChosen() {
  std::lock_guard<std::mutex> lock(g.mutex);
  g.exit_chosen = true;
}

// BF's own buttons, one after another (our pause list over BF's: its row's presses). 0.45 s apart: BF's pause list
// took presses 2 s apart in run origpause3 and about 1 s apart in run pz2; checked faster in run menu1a.
void PressBfTimed(const std::vector<std::pair<double, uint16_t>>& presses) {
  std::lock_guard<std::mutex> lock(g.mutex);
  g.script.clear();
  for (const auto& p : presses) g.script.push_back(p);
  g.script_at = Clock::now();
  g_walking = true;
}
bool BfPressesDone() {
  std::lock_guard<std::mutex> lock(g.mutex);
  return g.script.empty() && Clock::now() >= g.press_until;
}
void PressBf(const std::vector<uint16_t>& presses) {
  std::vector<std::pair<double, uint16_t>> timed;
  for (size_t i = 0; i < presses.size(); ++i) timed.push_back({i ? 0.3 : 0.15, presses[i]});
  PressBfTimed(timed);
}

bool InMatch() {
  std::lock_guard<std::mutex> lock(g.mutex);
  return g.stage == Stage::kInGame;
}

// For the joining page: the level's loading screen is up (until then the game's own menu, its "Joining game" box,
// is still on screen for a second or two after the load began: our page stays over it).
bool LoadingScreenShown() {
  std::lock_guard<std::mutex> lock(g.mutex);
  return g.seen_loading && g.load_screen;
}

// The level has loaded (BF's Loading complete, or later): for the dev pause test.
bool LevelLoaded() {
  std::lock_guard<std::mutex> lock(g.mutex);
  return g.stage == Stage::kInGame && g.seen_ready && (Since(g.last_load_update) > 1.0 || Since(g.ready_at) > 4.0);
}

// For the joining screen: the level has loaded and the spawn presses are done (Step), or a minute has gone by in the
// match.
bool LevelReady() {
  std::lock_guard<std::mutex> lock(g.mutex);
  if (g.stage != Stage::kInGame) return false;
  return (g.spawning && g.script.empty() && Since(g.spawn_at) > 5.5) || Since(g.stage_at) > 60;
}

int MenuDriverStage() {
  std::lock_guard<std::mutex> lock(g.mutex);
  return static_cast<int>(g.stage);
}

}  // namespace bf1943

// EA's layer leaves the game (sub_824BDC50(game)): after Exit Game, a kick or a lost connection.
REX_EXTERN(__imp__sub_824BDC50);
REX_HOOK_RAW(sub_824BDC50) {
  {
    std::lock_guard<std::mutex> lock(bf1943::g.mutex);
    bf1943::g.left = true;
  }
  REXLOG_INFO("BF menu driver: left the game");
  __imp__sub_824BDC50(ctx, base);
}

// The game client's connection to the server ends (sub_8237CE40(connection, reason)): 0 ok, 4 kicked, 6 error.
REX_EXTERN(__imp__sub_8237CE40);
REX_HOOK_RAW(sub_8237CE40) {
  {
    std::lock_guard<std::mutex> lock(bf1943::g.mutex);
    bf1943::g.disconnect_reason = ctx.r4.u32;
    bf1943::g.ended = true;
    bf1943::g.ended_at = bf1943::Clock::now();
  }
  REXLOG_INFO("BF menu driver: game connection ended, reason {}", ctx.r4.u32);
  __imp__sub_8237CE40(ctx, base);
}

REX_EXTERN(sub_82434578);  // a call message from the UI's pool
REX_EXTERN(sub_821D51C8);  // string assign
REX_EXTERN(sub_8242F710);  // queue a call message
REX_EXTERN(sub_82958C20);  // dev probe: an APT string from text
REX_EXTERN(sub_82986340);  // dev probe: the root scope
REX_EXTERN(sub_8297DD10);  // dev probe: look a path up
REX_EXTERN(sub_8295C470);  // dev probe: release an APT string
REX_EXTERN(sub_82954348);  // the Flash player's loadMovie(player, url, target)
REX_EXTERN(sub_824253A0);  // resolve a call (ui, text, &flag, &target): marks the message valid
REX_EXTERN(sub_82986450);  // call a script function (name, &result, scope: `this`, argc, argv)

// The game's calls into its Flash menu by function name (sub_8243BB70(ui, name)), e.g. presenceConnected. Our own
// calls (hideContent) go out right after one of the game's, on the same thread with the same UI object.
// The menu's commands to the game: the Flash player's FSCommand callback sub_826B6628(name, args) (the pointer at
// 0x82D13C78, called by the APT runtime's GetURL for "FSCommand:<name>"). It hands the name to sub_826B5B10, which
// runs a few itself (sounds, the 3D scene) and passes the others to the game's handler sub_8246D918 (the UI object's
// method +0x28), which looks the name up in the table at 0x82020080 (name, number: startQuickMatch 0x75, suicide
// 0x84, doExitLevel 0x18, ...) and acts on its number. Our menus call it the same way (GameCommand below), so a
// button of ours does what the game's own does. BF_NET_TRACE or BF_DEV_CMDLOG: each command logged.
REX_EXTERN(__imp__sub_826B6628);
REX_HOOK_RAW(sub_826B6628) {
  static const bool log = [] {
    const char* v = std::getenv("BF_DEV_CMDLOG");
    return (v && *v == '1') || bf1943::TraceOn();
  }();
  // numDialogs <n>: how many of the game's dialogs are open (the sign in's autosave notice, MenuDriver's kTitleDone).
  if (const uint32_t n = ctx.r3.u32; n && std::memcmp(base + n, "numDialogs", 11) == 0) {
    const int open = ctx.r4.u32 ? std::atoi(reinterpret_cast<const char*>(base + ctx.r4.u32)) : 0;
    std::lock_guard<std::mutex> lock(bf1943::g.mutex);
    if (open > 0 && bf1943::g.dialogs == 0) bf1943::g.dialog_at = std::chrono::steady_clock::now();
    bf1943::g.dialogs = open;
  }
  if (log) {
    auto text = [&](uint32_t p) {
      std::string out;
      for (uint32_t i = 0; p && i < 120 && base[p + i]; ++i) out += char(base[p + i]);
      return out;
    };
    const std::string name = text(ctx.r3.u32), args = text(ctx.r4.u32);
    static const std::set<std::string> noisy = {"getPressedKeys", "enableAnalog", "refreshArray", "setArrayIndex"};
    static std::atomic<int> lines{0};
    if (!noisy.count(name) && lines.fetch_add(1) < 3000)
      REXLOG_INFO("BF menu command {} ({}) lr {:08X}", name, args, uint32_t(ctx.lr));
  }
  if (bf1943::SpawnMouseCommand(ctx.r3.u32 ? reinterpret_cast<const char*>(base + ctx.r3.u32) : nullptr,
                                ctx.r4.u32 ? reinterpret_cast<const char*>(base + ctx.r4.u32) : nullptr))
    return;
  __imp__sub_826B6628(ctx, base);
}

// The engine's startup cards (the ESRB notice and the island pictures, with the logo sound): sub_824364D8(sequence,
// target, f1 the frame time) adds the frame time to its clock at +8 and draws the card that clock is on, 2.0 s a
// card, 10.7 s before the menu movie starts (runs bootfiles1 to 5). Our launcher plays the same cards before Home, so
// in a hidden boot (behind our Getting ready page, and test runs) that clock runs 20 times as fast.
REXCVAR_DEFINE_BOOL(bf_fast_startup_cards, true, "BF1943",
                    "Behind our launcher the game's own startup cards pass quickly (ours were shown before Home)");
REX_EXTERN(__imp__sub_824364D8);
REX_HOOK_RAW(sub_824364D8) {
  if (REXCVAR_GET(bf_fast_startup_cards) && (bf1943::AutoTest() || bf1943::LauncherLetsGameSignIn()))
    ctx.f1.f64 *= 20.0;
  __imp__sub_824364D8(ctx, base);
}

// The HUD's score events, sub_8244DE18(f1 the points, r3 the HUD, r4 the label, r6 the event): each of the local
// player's, for our Stats & Awards (docs/MENUS-PLAN.md). Strings are (begin, end, capacity) words. BF_DEV_SCORELOG=1:
// each one logged.
namespace bf1943 {
namespace {
std::mutex g_score_mutex;
std::map<std::string, std::pair<int, double>> g_score_events;  // label -> (events, points) since the last take
}  // namespace
// For our menu (game_menu.cpp): the score events since the last call, by label (suicide, CQ_StringForFlagCapture...).
std::map<std::string, std::pair<int, double>> TakeScoreEvents() {
  std::lock_guard lock(g_score_mutex);
  std::map<std::string, std::pair<int, double>> out;
  out.swap(g_score_events);
  return out;
}
}  // namespace bf1943

REX_EXTERN(__imp__sub_8244DE18);
REX_HOOK_RAW(sub_8244DE18) {
  {  // the player's totals for Stats & Awards
    std::string label;
    const uint32_t at = ctx.r4.u32;
    if (at >= 0x1000) {
      const uint32_t b = uint32_t(base[at]) << 24 | uint32_t(base[at + 1]) << 16 | uint32_t(base[at + 2]) << 8 | base[at + 3];
      const uint32_t e = uint32_t(base[at + 4]) << 24 | uint32_t(base[at + 5]) << 16 | uint32_t(base[at + 6]) << 8 | base[at + 7];
      if (b >= 0x1000 && e >= b && e - b <= 60)
        for (uint32_t i = b; i < e; ++i) label += char(base[i]);
    }
    if (!label.empty()) {
      std::lock_guard lock(bf1943::g_score_mutex);
      auto& v = bf1943::g_score_events[label];
      ++v.first, v.second += ctx.f1.f64;
    }
  }
#if defined(BF_DEV_TOOLS)
  static const bool log = std::getenv("BF_DEV_SCORELOG") != nullptr;
  if (log) {
    auto word = [&](uint32_t a) {
      return a < 0x1000 ? 0u : uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
    };
    auto str = [&](uint32_t at) {  // a string object at `at`
      std::string out;
      const uint32_t b = word(at), e = word(at + 4);
      if (!b || e < b || e - b > 80) return out;
      for (uint32_t i = b; i < e; ++i) out += char(base[i]);
      return out;
    };
    const uint32_t ev = ctx.r6.u32 >= 0x1000 ? ctx.r6.u32 : 0;  // the event (r6: f1 is the first argument)
    REXLOG_INFO("BF score event: {:.0f} points, label \"{}\"; event {:08X}: {:02X} {:08X} \"{}\" \"{}\" \"{}\" {:02X} {:08X} {:08X}",
                ctx.f1.f64, str(ctx.r4.u32), ev, ev ? base[ev] : 0, ev ? word(ev + 4) : 0, ev ? str(ev + 8) : "",
                ev ? str(ev + 0x14) : "", ev ? str(ev + 0x20) : "", ev ? base[ev + 0x2C] : 0, ev ? word(ev + 0x30) : 0,
                ev ? word(ev + 0x34) : 0);
  }
#endif
  __imp__sub_8244DE18(ctx, base);
}

REX_EXTERN(__imp__sub_8243BB70);
REX_HOOK_RAW(sub_8243BB70) {
  std::string name;
  for (uint32_t i = 0, p = ctx.r4.u32; p && i < 64 && base[p + i]; ++i) name += char(base[p + i]);
#if defined(BF_DEV_TOOLS)
  {  // BF_DEV_UICALL=<name>: that call's argument registers and the 64 bytes at each, every 5 s (the scoreboard's data)
    static const std::string want = [] {
      const char* v = std::getenv("BF_DEV_UICALL");
      return std::string(v ? v : "");
    }();
    static auto at = std::chrono::steady_clock::now();
    if (!want.empty() && name == want && std::chrono::steady_clock::now() - at > std::chrono::seconds(5)) {
      at = std::chrono::steady_clock::now();
      auto dump = [&](uint32_t a) {
        std::string out;
        if (a < 0x1000 || a >= 0xF0000000u) return out;
        for (uint32_t i = 0; i < 64; ++i) out += fmt::format("{:02X}", base[a + i]) + (i % 4 == 3 ? " " : "");
        return out;
      };
      REXLOG_INFO("BF dev UI call {}: r5 {:08X} [{}] r6 {:08X} [{}] r7 {:08X}", name, ctx.r5.u32, dump(ctx.r5.u32),
                  ctx.r6.u32, dump(ctx.r6.u32), ctx.r7.u32);
    }
  }
#endif
  if (bf1943::TraceOn()) {
    // The frequent ones (load progress, the match's objects, the analog sticks) are left out, so the menu's own
    // events (screens opening and closing) stay in the log.
    static const std::set<std::string> noisy = {"loadTimeUpdate", "uMPObj", "autoJoinUpdate", "couch", "leftAnalog",
                                               "rightAnalog", "uS", "vBarAmt", "cqTickets"};
    static std::atomic<int> calls{0};
    if (!noisy.count(name) && calls.fetch_add(1) < 2000)
      REXLOG_INFO("BF UI call {} (lr {:08X})", name, static_cast<uint32_t>(ctx.lr));
  }
#if defined(BF_DEV_TOOLS)
  // BF_UI_RATES=1 (investigation): each second, how often the game called each of its UI's functions.
  static const bool rates = [] {
    const char* v = std::getenv("BF_UI_RATES");
    return v && *v == '1';
  }();
  if (rates) {
    static std::mutex m;
    static std::map<std::string, int> counts;
    static auto since = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m);
    ++counts[name];
    if (std::chrono::steady_clock::now() - since > std::chrono::seconds(1)) {
      std::string text;
      for (const auto& [n, c] : counts) text += " " + n + "=" + std::to_string(c);
      REXLOG_INFO("BF UI rates:{}", text);
      counts.clear();
      since = std::chrono::steady_clock::now();
    }
  }
#endif
  // BF's own menu screens (its pause, Help & Options and the screens under them) call "couch" about once a second
  // while one is up, and never in play (run pausehelp6): the pause menu's sign that BF's own pause is open.
  if (name == "couch") bf1943::NoteGameMenuSignal();
  {
    std::lock_guard<std::mutex> lock(bf1943::g.mutex);
    if (name == "settingsLoaded") bf1943::g.seen_settings = true;
    if (name == "presenceConnected") bf1943::g.seen_presence = true;
    if (name == "queryBegin") bf1943::g.seen_query = true;
    if (name == "onStartLoading") bf1943::g.seen_loading = true;
    if (name == "gameIsReady" && bf1943::g.seen_loading) {
      const auto now = std::chrono::steady_clock::now();
      if (!bf1943::g.seen_ready) {
        bf1943::g.seen_ready = true, bf1943::g.ready_at = now;
      } else if (bf1943::g.stage == bf1943::Stage::kInGame && now - bf1943::g.ready_at > std::chrono::seconds(10)) {
        // A second Loading complete in the same match: the server changed the level (its map rotation). BF sends no
        // onStartLoading then, only playerRoleReady and, once the new level is in, gameIsReady (run rottrace1).
        // Modern mode scans the new level's values (its last scan's addresses were the old level's), and test runs
        // press through Loading complete again (without, BF's autokick drops them 20 s later: runs rotperf1,
        // rotplay1).
        bf1943::g.ready_at = now;
        bf1943::g.spawning = false;
        bf1943::g.entered = false;
        bf1943::NoteLevelChange();
        REXLOG_INFO("BF menu driver: the server changed the level");
      }
    }
    if (name == "loadTimeUpdate") {
      bf1943::g.last_load_update = std::chrono::steady_clock::now();
      if (bf1943::g.seen_loading && !bf1943::g.load_screen) {
        bf1943::g.load_screen = true;  // our joining page may give way now: the game's own menu is gone (runs launchaudio)
        REXLOG_INFO("BF menu driver: the level's loading screen is up");
      }
    }
    bf1943::g.ui = ctx.r3.u32;
  }
  __imp__sub_8243BB70(ctx, base);
}

namespace bf1943 {
namespace {

// Our ActionScript calls (_root.hideContent), made on the game's thread from its pad read. sub_8243BB70 always
// calls _root.gameEvent(name), an event for the menu's listeners; we build the same message with our own function
// path instead: sub_82434578 takes a message from the pool, sub_821D51C8 sets its function path (+0x0C), +0x04 is
// the UI's movie (ui + 0x41A78), and sub_8242F710 queues it (ui + 0x41A6C) for the menu, no arguments.
void FlashCalls(PPCContext& ctx, uint8_t* base) {
  std::string path;
  uint32_t ui;
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    if (g.flash_calls.empty() || !g.ui) return;
    path = g.flash_calls.front();
    g.flash_calls.pop_front();
    ui = g.ui;
  }
  static uint32_t scratch = 0;  // 64 bytes of text, then the message pointer for the queue call
  if (!scratch) scratch = REX_KERNEL_MEMORY()->SystemHeapAlloc(128);
  std::memset(base + scratch, 0, 128);
  std::memcpy(base + scratch, path.c_str(), std::min<size_t>(path.size(), 63));
  PPCContext call = ctx;
  if (path.rfind("probe:", 0) == 0) {
    // Dev: does a path resolve in BF's Flash player? As sub_82986450 does: an APT string (sub_82958C20), the root
    // scope (sub_82986340(0)), the lookup (sub_8297DD10(stack 0x82D13BB8, scope, 0, &string, 1, 1, 0)), with the
    // player made current like the API wrappers do (0x82D135E8, 0x82D135EC).
    const std::string target = path.substr(6);
    std::memset(base + scratch, 0, 128);
    std::memcpy(base + scratch + 16, target.c_str(), std::min<size_t>(target.size(), 100));
    auto load = [&](uint32_t a) { return uint32_t(base[a]) << 24 | base[a + 1] << 16 | base[a + 2] << 8 | base[a + 3]; };
    auto store = [&](uint32_t a, uint32_t v) { for (int i = 0; i < 4; ++i) base[a + i] = uint8_t(v >> (24 - 8 * i)); };
    const uint32_t holder = load(ui + 0x14);
    if (!holder) return;
    const uint32_t player = load(holder + 4), was_e8 = load(0x82D135E8), was_ec = load(0x82D135EC);
    store(0x82D135E8, player);
    store(0x82D135EC, player);
    call.r3.u64 = scratch;  // the APT string
    call.r4.u64 = scratch + 16;
    sub_82958C20(call, base);
    call = ctx;
    call.r3.u64 = 0;
    sub_82986340(call, base);
    const uint32_t scope = call.r3.u32;
    call = ctx;
    call.r3.u64 = 0x82D13BB8;
    call.r4.u64 = scope;
    call.r5.u64 = 0;
    call.r6.u64 = scratch;
    call.r7.u64 = 1;
    call.r8.u64 = 1;
    call.r9.u64 = 0;
    sub_8297DD10(call, base);
    const uint32_t found = call.r3.u32;
    REXLOG_INFO("BF menu driver: probe {} -> {:08X} [{:08X} {:08X} {:08X}]", target, found, found ? load(found) : 0,
                found ? load(found + 4) : 0, found ? load(found + 8) : 0);
    call = ctx;
    call.r3.u64 = load(scratch);
    sub_8295C470(call, base);
    store(0x82D135E8, was_e8);
    store(0x82D135EC, was_ec);
    return;
  }
  if (path.rfind("method:", 0) == 0) {
    // A method of one of the menu's objects, run on that object ("method:<object path>|<function>", no arguments).
    // The queued call below runs a function with `this` the movie's root (sub_82986450 hands its scope to the call as
    // `this`, and the queue gives it none), which is right for a function that only uses names in scope and wrong for
    // one compiled to use `this`: the deploy screen's onSpawnDown played its sound and changed nothing (13 Nov 05:00).
    // So here the object is looked up first (as the probe above does) and given as the scope: sub_82986450(function
    // name, no result, the object, 0 arguments).
    const std::string body = path.substr(7);
    const size_t bar = body.find('|');
    if (bar == std::string::npos) return;
    const std::string object = body.substr(0, bar), function = body.substr(bar + 1);
    auto load = [&](uint32_t a) { return uint32_t(base[a]) << 24 | base[a + 1] << 16 | base[a + 2] << 8 | base[a + 3]; };
    auto store = [&](uint32_t a, uint32_t v) { for (int i = 0; i < 4; ++i) base[a + i] = uint8_t(v >> (24 - 8 * i)); };
    const uint32_t holder = load(ui + 0x14);
    if (!holder) return;
    std::memset(base + scratch, 0, 128);
    std::memcpy(base + scratch + 16, object.c_str(), std::min<size_t>(object.size(), 60));
    std::memcpy(base + scratch + 80, function.c_str(), std::min<size_t>(function.size(), 40));
    const uint32_t player = load(holder + 4), was_e8 = load(0x82D135E8), was_ec = load(0x82D135EC);
    store(0x82D135E8, player);
    store(0x82D135EC, player);
    call.r3.u64 = scratch;  // the APT string of the object's path
    call.r4.u64 = scratch + 16;
    sub_82958C20(call, base);
    call = ctx;
    call.r3.u64 = 0;
    sub_82986340(call, base);
    const uint32_t root = call.r3.u32;
    call = ctx;
    call.r3.u64 = 0x82D13BB8;
    call.r4.u64 = root;
    call.r5.u64 = 0;
    call.r6.u64 = scratch;
    call.r7.u64 = 1;
    call.r8.u64 = 1;
    call.r9.u64 = 0;
    sub_8297DD10(call, base);
    const uint32_t found = call.r3.u32;
    call = ctx;
    call.r3.u64 = load(scratch);
    sub_8295C470(call, base);
    // Only a movie clip is taken as a scope: an object of the root's own class, or of the class the deploy screen's
    // clips have (0x820BEDB0: the probe of _root.contentMC.loader1.spawnmap, 13 Nov).
    const bool ok = root >= 0x10000 && found >= 0x10000 && found < 0xF0000000 &&
                    (load(found) == load(root) || load(found) == 0x820BEDB0u);
    if (ok) {
      call = ctx;
      call.r3.u64 = scratch + 80;  // the function's name
      call.r4.u64 = 0;
      call.r5.u64 = found;
      call.r6.u64 = 0;
      call.r7.u64 = 0;
      sub_82986450(call, base);
    }
    static int logged = 0;
    if (!ok || logged < 4) {
      ++logged;
      REXLOG_INFO("BF menu driver: method {} on {} ({:08X}, class {:08X}; the root {:08X}, class {:08X}): {}", function,
                  object, found, found >= 0x10000 && found < 0xF0000000 ? load(found) : 0, root,
                  root >= 0x10000 ? load(root) : 0, ok ? "called" : "not called, not a clip of the root's kind");
    }
    store(0x82D135E8, was_e8);
    store(0x82D135EC, was_ec);
    return;
  }
  if (path.rfind("cmd:", 0) == 0) {
    // A command as the game's own menu sends it ("cmd:<name>|<args>"): sub_826B6628(name, args), on the game's thread.
    const std::string body = path.substr(4);
    const size_t bar = body.find('|');
    const std::string name = body.substr(0, bar), args = bar == std::string::npos ? "" : body.substr(bar + 1);
    std::memset(base + scratch, 0, 128);
    std::memcpy(base + scratch, name.c_str(), std::min<size_t>(name.size(), 47));
    std::memcpy(base + scratch + 48, args.c_str(), std::min<size_t>(args.size(), 79));
    call.r3.u64 = scratch;
    call.r4.u64 = scratch + 48;
    sub_826B6628(call, base);
    REXLOG_INFO("BF game command {} ({}) sent", name, args);
    return;
  }
  if (path.rfind("unload:", 0) == 0) {
    // loadMovie("", target) on BF's Flash player (sub_82954348(player, url, target), as the UI's shutdown does with
    // _level0): empties that clip. The player is *(ui + 0x14) + 4.
    const std::string target = path.substr(7);
    std::memset(base + scratch, 0, 128);
    std::memcpy(base + scratch + 8, target.c_str(), std::min<size_t>(target.size(), 100));
    const uint32_t holder = uint32_t(base[ui + 0x14]) << 24 | base[ui + 0x15] << 16 | base[ui + 0x16] << 8 | base[ui + 0x17];
    if (!holder) return;
    const uint32_t player = uint32_t(base[holder + 4]) << 24 | base[holder + 5] << 16 | base[holder + 6] << 8 | base[holder + 7];
    call.r3.u64 = player;
    call.r4.u64 = scratch;      // ""
    call.r5.u64 = scratch + 8;  // target
    sub_82954348(call, base);
    REXLOG_INFO("BF menu driver: emptied {} in the Flash menu", target);
    return;
  }
  call.r3.u64 = ui;  // the pool is the UI's own
  sub_82434578(call, base);
  const uint32_t msg = call.r3.u32;
  if (!msg) {
    REXLOG_WARN("BF menu driver: no call message for {}", path);
    return;
  }
  call = ctx;
  call.r3.u64 = msg + 0x0C;
  call.r4.u64 = scratch;
  sub_821D51C8(call, base);
  std::memcpy(base + msg + 4, base + ui + 0x41A78, 4);
  call = ctx;  // the call's text (+0x18), resolved as sub_8243BB70 does; only a resolved message is run
  call.r3.u64 = msg + 0x18;
  call.r4.u64 = scratch;
  sub_821D51C8(call, base);
  call = ctx;
  call.r3.u64 = ui;
  call.r4.u64 = msg + 0x18;
  call.r5.u64 = scratch + 72;  // flag byte
  call.r6.u64 = scratch + 76;  // target word
  sub_824253A0(call, base);
  if (call.r3.u8 != 0) {
    base[msg + 2] = base[scratch + 72];
    base[msg + 1] = 1;
    std::memcpy(base + msg + 8, base + scratch + 76, 4);
  }  // else: not one of the names the menu treats specially, nothing to mark
  for (int i = 0; i < 4; ++i) base[scratch + 64 + i] = uint8_t(msg >> (24 - 8 * i));
  call = ctx;
  call.r3.u64 = ui + 0x41A6C;
  call.r4.u64 = scratch + 64;
  sub_8242F710(call, base);
  REXLOG_INFO("BF menu driver: called {} in the Flash menu", path);
}

}  // namespace
}  // namespace bf1943

// XamInputGetState(user, flags, state): our presses on player 1's pad; while our menu is open the game gets an
// empty pad and our menu gets the real one. X_INPUT_STATE (big endian): u32 packet number, u16 buttons, u8 left
// trigger, u8 right trigger, four i16 sticks.
namespace bf1943 {
#if defined(BF_DEV_TOOLS)
void DevTypesTick(const uint8_t* base);  // dev_types.cpp: BF_DEV_TYPES
#endif
void ModernModeTick(uint8_t* base);  // modern_mode.cpp
uint16_t PulsedButtons();             // input_settings.cpp: our presses on the player's pad
bool TakeEscapePress();               // game_menu.cpp: the keyboard's Esc with our menu closed
void WheelNotch(int direction);                    // look.cpp: a notch of the mouse wheel
void PressEscapeForTest();            // game_menu.cpp: the keyboard's Esc, for the match script
#if defined(BF_DEV_TOOLS) && defined(_WIN32)
void TriggerRenderDocCapture();       // dev_renderdoc.cpp: the match script's "rdoc"
#endif
}  // namespace bf1943
#if defined(BF_DEV_TOOLS)
namespace {
// The tests' keys (the scripts' "key=<name>" words): the keys held now against those held before.
void ScriptKeys(std::set<std::string>& down, const std::set<std::string>& now) {
  if (now == down) return;
  for (const std::string& k : down)
    if (!now.count(k)) rex::input::mnk::SetTestKey(uint16_t(rex::ui::ParseVirtualKey(k)), false);
  for (const std::string& k : now)
    if (!down.count(k)) {
      rex::input::mnk::SetTestKey(uint16_t(rex::ui::ParseVirtualKey(k)), true);
      REXLOG_INFO("BF script: key {} down", k);
    }
  down = now;
}
}  // namespace
#endif
extern "C" REX_FUNC(__imp__XamInputGetState) {
#if defined(BF_DEV_TOOLS)
  bf1943::DevTypesTick(base);
  {  // BF_DEV_PEEK=<addr>[,<addr>...]: the words at those guest addresses, at 20 s and 60 s after the first pad read;
     // <addr>><offset>>... follows pointers (the word read, plus the offset, read again: 82CAE5BC>14>0>28).
    static const std::vector<std::vector<uint32_t>> peek = [] {
      std::vector<std::vector<uint32_t>> out;
      for (const char* c = std::getenv("BF_DEV_PEEK"); c && *c;) {
        std::vector<uint32_t> chain;
        const char* end = std::strchr(c, ',');
        const std::string item(c, end ? end : c + std::strlen(c));
        for (size_t pos = 0; pos <= item.size();) {
          const size_t gt = std::min(item.find('>', pos), item.size());
          chain.push_back(uint32_t(std::strtoul(item.substr(pos, gt - pos).c_str(), nullptr, 16)));
          pos = gt + 1;
        }
        out.push_back(chain);
        c = end ? end + 1 : nullptr;
      }
      return out;
    }();
    static const auto first = std::chrono::steady_clock::now();
    static int done = 0;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - first).count();
    // BF_DEV_COMMANDS=<s>:<name>[|<args>],...: game commands sent that many seconds after the first pad read.
    static std::vector<std::pair<double, std::string>> commands = [] {
      std::vector<std::pair<double, std::string>> out;
      for (const char* c = std::getenv("BF_DEV_COMMANDS"); c && *c;) {
        const char* end = std::strchr(c, ',');
        const std::string item(c, end ? end : c + std::strlen(c));
        const size_t colon = item.find(':');
        if (colon != std::string::npos) out.push_back({std::atof(item.substr(0, colon).c_str()), item.substr(colon + 1)});
        c = end ? end + 1 : nullptr;
      }
      return out;
    }();
    for (auto it = commands.begin(); it != commands.end();) {
      if (t < it->first) {
        ++it;
        continue;
      }
      if (it->second.rfind("flash:", 0) == 0) {  // an ActionScript function of the menu's ("flash:_root.enterMainMenu")
        bf1943::FlashCall(it->second.substr(6));
      } else {
        const size_t bar = it->second.find('|');
        bf1943::GameCommand(it->second.substr(0, bar), bar == std::string::npos ? "" : it->second.substr(bar + 1));
      }
      it = commands.erase(it);
    }
    if (!peek.empty() && done < 2 && t > (done ? 60.0 : 20.0)) {
      ++done;
      std::string line;
      auto word = [&](uint32_t a) {
        return a < 0x1000 ? 0u : uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
      };
      for (const auto& chain : peek) {
        uint32_t v = word(chain[0]);
        line += fmt::format(" {:08X}={:08X}", chain[0], v);
        for (size_t i = 1; i < chain.size() && v; ++i) {
          v = word(v + chain[i]);
          line += fmt::format(">+{:X}={:08X}", chain[i], v);
        }
      }
      REXLOG_INFO("BF dev peek ({:.0f} s):{}", t, line);
    }
  }
#endif
  bf1943::ModernModeTick(base);
  static PPCFunc* original = bf1943::RuntimeExport("__imp__XamInputGetState");
  // The driver runs with our menu, and not under the dev pad's scripts (they walk the menus themselves).
  static const bool driver = [] {
    const char* pad = std::getenv("BF_DEV_PAD");
    return bf1943::GameMenuEnabled() && !(pad && *pad);
  }();
  const uint32_t user = ctx.r3.u32, state = ctx.r5.u32;
  if (bf1943::TraceOn()) {  // each caller once: which user index and flags BF reads with
    static std::mutex mutex;
    static std::set<uint64_t> seen;
    const uint64_t key = uint64_t(user) << 48 ^ uint64_t(ctx.r4.u32) << 32 ^ ctx.lr;
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.insert(key).second)
      REXLOG_INFO("BF pad read: user {:X} flags {:X} (lr {:08X})", user, ctx.r4.u32, uint32_t(ctx.lr));
  }
  if (original) original(ctx, base);
  // Our calls into the game's menu (Flash calls, game commands) run on every player 1 read, with or without our menu.
  if (user == 0 && state) bf1943::FlashCalls(ctx, base);
  if (!driver || user != 0 || !state) return;
  const bool connected = ctx.r3.u32 == 0;
  static int was_connected = -1;
  if (int(connected) != was_connected) {
    was_connected = connected;
    REXLOG_INFO("BF menu driver: real pad {}", connected ? "connected" : "not connected");
  }
  uint8_t* p = base + state;
  static uint16_t last_real = 0;
#if defined(BF_DEV_TOOLS)
  // BF_REAL_PAD=<s>:<button>[/<hold s>],... (tests): buttons on the real pad's state, from the process start, as if
  // pressed on the controller (a, b, start, up, down), before anything of ours reads it. BF_REAL_PAD_FROM=seated: from the
  // moment the soldier first sits in a vehicle instead (BF_SEEK_VEHICLE gets it there), and from then on each time it
  // gets in or out of one is logged.
  if (connected) {
    struct Item {
      double at, hold;
      uint16_t button;
    };
    static const std::vector<Item> real_script = [] {
      std::vector<Item> out;
      const char* v = std::getenv("BF_REAL_PAD");
      for (const char* c = v ? v : ""; *c;) {
        Item it{std::atof(c), 0.15, 0};
        while (*c && *c != ':') ++c;
        if (*c) ++c;
        it.button = !std::strncmp(c, "start", 5) ? 0x0010
                    : !std::strncmp(c, "up", 2)   ? 0x0001
                    : !std::strncmp(c, "down", 4) ? 0x0002
                    : *c == 'b'                   ? 0x2000
                                                  : 0x1000;
        while (*c && *c != ',' && *c != '/') ++c;
        if (*c == '/') it.hold = std::atof(++c);
        while (*c && *c != ',') ++c;
        if (*c) ++c;
        out.push_back(it);
      }
      return out;
    }();
    static const bool from_seated = [] {
      const char* v = std::getenv("BF_REAL_PAD_FROM");
      return v && std::string(v) == "seated";
    }();
    using RClock = std::chrono::steady_clock;
    static RClock::time_point began = from_seated ? RClock::time_point{} : RClock::now();
    const bool clock_on = began != RClock::time_point{};
    double t = clock_on ? std::chrono::duration<double>(RClock::now() - began).count() : -1.0;
    if (from_seated) {
      static int was_seated = -1;
      const bool seated = bf1943::LocalSeated(base);
      if (seated && !clock_on) {
        began = RClock::now(), t = 0;
        REXLOG_INFO("BF real pad: the soldier is in a vehicle: the script's clock starts");
      }
      if (began != RClock::time_point{} && int(seated) != was_seated)
        REXLOG_INFO("BF real pad: {} ({:.1f} s on the script's clock)", seated ? "in a vehicle" : "out of the vehicle", t);
      was_seated = seated;
    }
    uint16_t fake = 0;
    for (const Item& it : real_script)
      if (t >= 0 && t >= it.at && t < it.at + it.hold) fake |= it.button;
    static uint16_t logged_fake = 0;
    if (fake != logged_fake) {
      if (fake) REXLOG_INFO("BF real pad: {:04X} down ({:.1f} s)", fake, t);
      logged_fake = fake;
    }
    // A change bumps the packet number, as the controller's own does.
    static uint16_t last_fake = 0;
    static uint32_t fake_packets = 0;
    if (fake != last_fake) ++fake_packets, last_fake = fake;
    const uint16_t b = uint16_t(p[4] << 8 | p[5]) | fake;
    p[4] = uint8_t(b >> 8), p[5] = uint8_t(b);
    const uint32_t n = (uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]) + fake_packets;
    p[0] = uint8_t(n >> 24), p[1] = uint8_t(n >> 16), p[2] = uint8_t(n >> 8), p[3] = uint8_t(n);
  }
#endif
  // Our own presses on the player's pad (the mouse wheel's weapon switch: input_settings.cpp), the packet number
  // bumped when they change so the game sees them.
  if (connected) {
    static uint16_t last_pulse = 0;
    static uint32_t pulse_packets = 0;
    const uint16_t pulse = bf1943::PulsedButtons();
    if (pulse != last_pulse) ++pulse_packets, last_pulse = pulse;
    if (pulse || pulse_packets) {
      const uint16_t b = uint16_t(p[4] << 8 | p[5]) | pulse;
      p[4] = uint8_t(b >> 8), p[5] = uint8_t(b);
      const uint32_t n = (uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]) + pulse_packets;
      p[0] = uint8_t(n >> 24), p[1] = uint8_t(n >> 16), p[2] = uint8_t(n >> 8), p[3] = uint8_t(n);
    }
  }
  if (connected) {
    const uint16_t buttons = uint16_t(p[4] << 8 | p[5]);
    const bool passing = bf1943::GameMenuPassing();  // BF's own pause screen has the pad: its Start is BF's
    // Our menu reads the keyboard itself (arrows, Enter, Esc, the mouse): the buttons and sticks the keys gave the
    // pad (Keyboard & mouse's bindings: E is B, Space is A) are left out of what it takes, so a key bound to a game
    // action does not also act in the menu.
    const bool key_axes = rex::input::mnk::LastKeyAxes();
    // Whether our menu was open before it gets this read's presses: the Start that closes it must not open it again
    // if the menu closes between here and the check below.
    // The chat box's Enter or Esc (comms_ui.cpp) is not the pause's Start.
    const bool menu_was_open = bf1943::GameMenuOpen() || bf1943::comms::TypingOrJustClosed();
    // The device the player uses in the match (TekRantGaming, 12 Nov 2026: the prompts follow the keyboard and mouse
    // or the controller as they change hands mid match; game_menu.cpp NoteMatchDevice, the HUD's prompts): a key or a
    // mouse button through the keyboard and mouse driver (the buttons, sticks and triggers it gives), else a press on
    // the controller itself or a push of its sticks or triggers (not our own pulses, nor a stick's drift). With our
    // menu open the menu decides (TrackDevice). The mouse's movement counts in look.cpp.
    if (!menu_was_open) {
      const uint16_t key_buttons = rex::input::mnk::LastKeyButtons();
      const uint8_t key_lt = rex::input::mnk::LastKeyTrigger(false), key_rt = rex::input::mnk::LastKeyTrigger(true);
      const auto pushed = [&](int at) { return std::abs(int(int16_t(p[at] << 8 | p[at + 1]))) > 12000; };
      const bool pad_buttons = (buttons & ~key_buttons & ~bf1943::PulsedButtons()) != 0;
      const bool pad_axes = (!key_axes && (pushed(8) || pushed(10) || pushed(12) || pushed(14))) ||
                            (!key_lt && p[6] > 60) || (!key_rt && p[7] > 60);  // p[6], p[7]: the triggers
      if (key_buttons || key_axes || key_lt || key_rt) bf1943::NoteMatchDevice(true);
      else if (pad_buttons || pad_axes) bf1943::NoteMatchDevice(false);
    }
    bf1943::GameMenuPad(uint16_t(buttons & ~rex::input::mnk::LastKeyButtons()), key_axes ? 0 : int16_t(p[8] << 8 | p[9]),
                        key_axes ? 0 : int16_t(p[10] << 8 | p[11]));
    // Start in a match: our pause opens (game_menu.cpp, PausePage), in place of the game's, which never opens (the
    // Start is taken from the game's pad). The keyboard's Esc too (game_menu.cpp sees it while our menu is closed).
    const bool esc = bf1943::TakeEscapePress();
    const bool start = ((buttons & 0x0010) && !(last_real & 0x0010)) || esc;
    bf1943::NotePlayerButtons(uint16_t(buttons & ~last_real));
    last_real = buttons;
    // Only with the soldier in the world (on foot or seated): at BF's loading screen, Loading complete and the
    // spawn screen, which are the player's own now, Start stays BF's.
    const bool in_world = bf1943::LocalOnFoot() || bf1943::LocalSeated(base);
    // Just after the soldier comes into the world BF ignores Start (run pausehelp2: 0.7 s after the spawn its pause
    // never opened, and ours sat over the match): for 2 s Start does nothing (TekRantGaming, 9 Nov: "if you press pause too
    // quickly at the start of a game ... some hitching").
    static std::chrono::steady_clock::time_point in_world_since{};
    static bool was_in_world = false;
    if (in_world && !was_in_world) in_world_since = std::chrono::steady_clock::now();
    was_in_world = in_world;
    const bool settled = std::chrono::steady_clock::now() - in_world_since > std::chrono::seconds(2);
    if (start && in_world && !settled && !passing && !menu_was_open)
      REXLOG_INFO("BF menu driver: Start in the first 2 s after the spawn: left to the game (it ignores it)");
    if (start && !passing && !menu_was_open && !bf1943::GameMenuOpen() && !bf1943::g_walking && bf1943::InMatch() &&
        in_world && settled) {
      REXLOG_INFO("BF menu driver: the pause opens ({})", esc ? "the keyboard's Esc" : "Start");
#if defined(BF_DEV_TOOLS)
      // BF_DEV_GAME_PAUSE=1 (investigation): the game's own pause instead of ours, to trace its rows' commands.
      static const bool game_pause = std::getenv("BF_DEV_GAME_PAUSE") != nullptr;
      if (game_pause) {
        bf1943::PressBf({0x0010});
      } else {
        bf1943::ShowPauseMenu();
      }
#else
      bf1943::ShowPauseMenu();
#endif
      p[5] &= ~0x10;
    }
  }
  // While the chat box is open (and just after it closes) the game gets no input, as with our menus open: the keys
  // typed are the chat's.
  const bool menu = bf1943::GameMenuOpen() || bf1943::comms::TypingOrJustClosed();
  const uint16_t press = bf1943::Step();
  // No real pad: always a connected one with only our presses, as the dev pad does. BF keeps the state in its pad
  // object (sub_821CEF88) and drops the pad on an error.
  if (!connected) {
    ctx.r3.u64 = 0;
    std::memset(p, 0, 16);
  }
  static uint16_t last = 0;
  static uint32_t packet = 0x10000;
  // The game sees only our presses while our menu is open or the driver walks BF's menus. The packet number always
  // carries our offset, so it never goes back.
  const bool hidden = menu || bf1943::g_walking;
  // The buttons and triggers held as our menu closes stay hidden from the game until they are let go: it read an empty
  // pad while the menu was open, so the B or Start that closed the menu would be a new press to it (TekRantGaming, 10 Nov
  // 2026: B out of our pause got him out of his vehicle, and a second Start showed the game's own pause; the A on
  // RESUME would jump). BF_DEV_PAD_LEAK=1 (tests): passed on as before, to show those bugs.
  {
    static bool was_hidden = false, held_lt = false, held_rt = false;
    static uint16_t held = 0;
    constexpr uint8_t kTriggerDown = 30;  // XInput's trigger threshold
#if defined(BF_DEV_TOOLS)
    static const bool leak = std::getenv("BF_DEV_PAD_LEAK") != nullptr;
#else
    constexpr bool leak = false;
#endif
    const uint16_t real = uint16_t(p[4] << 8 | p[5]);
    if (was_hidden && !hidden && !leak) {
      held = real, held_lt = p[6] > kTriggerDown, held_rt = p[7] > kTriggerDown;
      if (held || held_lt || held_rt)
        REXLOG_INFO("BF menu driver: our menu closed with {:04X}{}{} held: kept from the game until let go", held,
                    held_lt ? " LT" : "", held_rt ? " RT" : "");
    }
    was_hidden = hidden;
    held &= real;
    held_lt = held_lt && p[6] > kTriggerDown, held_rt = held_rt && p[7] > kTriggerDown;
    if (!hidden && (held || held_lt || held_rt)) {
      const uint16_t b = real & ~held;
      p[4] = uint8_t(b >> 8), p[5] = uint8_t(b);
      if (held_lt) p[6] = 0;
      if (held_rt) p[7] = 0;
    }
  }
  if (hidden) std::memset(p + 4, 0, 12);
  static const bool input_off = [] {
    const char* v = std::getenv("BF_AB_OFF");
    return v && std::string(v).find("input") != std::string::npos;
  }();
  const bool seated = bf1943::LocalSeated(base);
  bf1943::comms::NoteInWorld(seated || bf1943::LocalOnFoot());  // voice goes out only from the world
  bf1943::comms::ReadTeam(base, bf1943::LevelLoaded());  // team and squad chat (comms.cpp), the deploy screen too
  bf1943::CapturePointsTick(base, bf1943::LevelLoaded());  // the flags' letters and the pause menu's map
#if defined(BF_DEV_TOOLS)
  bf1943::DevTeamScan(base, seated || bf1943::LocalOnFoot());  // BF_DEV_TEAMSCAN (dev_team_scan.cpp)
#endif
  if (!input_off)  // Settings > Controls
    bf1943::ApplyControllerSettings(p + 4, menu || bf1943::g_walking, seated);
  bf1943::HudGlyphsTick(seated);  // hud_glyphs.cpp: the keyboard prompts follow the seat's keys
  if (!menu && !bf1943::g_walking) bf1943::LookPad(p + 4, seated);
  // The deploy screen's pointer (flash_mouse.cpp): a level is loaded and the player is in no seat and not on foot.
  bf1943::SpawnMousePad(p + 4, bf1943::LevelLoaded() && !menu && !hidden && !bf1943::g_walking,
                        !seated && !bf1943::LocalOnFoot());
  if (press & 0x0C00) {  // the driver's left stick (BF_SPAWN_PRESSES lsr, lsl)
    const int v = press & 0x0400 ? 32767 : -32768;
    p[8] = uint8_t(uint16_t(v) >> 8), p[9] = uint8_t(v);
  }
  const uint16_t buttons = uint16_t(p[4] << 8 | p[5]) | uint16_t(press & ~0x0C00);
  p[4] = uint8_t(buttons >> 8);
  p[5] = uint8_t(buttons);
#if defined(BF_DEV_TOOLS)
  // BF_WALK_TEST=<seconds>: in a match, the left stick full right from that many seconds after joining (frame rate
  // tests: how the soldier moves at different rates).
  static const double walk_after = [] {
    const char* v = std::getenv("BF_WALK_TEST");
    return v && *v ? std::atof(v) : -1.0;
  }();
  static std::chrono::steady_clock::time_point joined{};
  if (walk_after >= 0 && bf1943::InMatch()) {
    if (!joined.time_since_epoch().count()) joined = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - joined > std::chrono::duration<double>(walk_after)) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXLOG_INFO("BF walk test: left stick right (pad connected {}, walking {}, menu {})", connected,
                    bool(bf1943::g_walking), menu);
      }
      // BF_WALK_STICK: lx (default, strafe right: the open deck at the USMC carrier spawn), lxl (strafe left),
      // ly (forward), lyb (back), rx (turn right), ry (look up).
      static const std::string stick = [] {
        const char* v = std::getenv("BF_WALK_STICK");
        return std::string(v ? v : "lx");
      }();
      const int stick_offset = stick == "ly" || stick == "lyb" ? 10 : stick == "rx" ? 12 : stick == "ry" ? 14 : 8;
      // BF_WALK_AMOUNT=<0..1>: that much of the push (default all of it).
      static const double amount = [] {
        const char* v = std::getenv("BF_WALK_AMOUNT");
        return v && *v ? std::clamp(std::atof(v), 0.0, 1.0) : 1.0;
      }();
      const int16_t push = int16_t((stick == "lxl" || stick == "lyb" ? -32768.0 : 32767.0) * amount);
      p[stick_offset] = uint8_t(uint16_t(push) >> 8), p[stick_offset + 1] = uint8_t(uint16_t(push));
      ++packet;
    }
  }
  // BF_MATCH_SCRIPT=<seconds>:<input>[/<hold seconds>],... (tests): inputs from that many seconds after the match
  // began (the first in match pad read), each held 0.2 s or the given time. Inputs: a b x y lb rb lt rt up down left
  // right, and sticks held fully over: lsu lsd lsl lsr (left), rsu rsd rsl rsr (right). E.g. enter the carrier's
  // landing boat (b) and drive it (lsu) to Wake's beach.
  struct ScriptInput {
    double at, hold;
    std::string what;
  };
  static const std::vector<ScriptInput> match_script = [] {
    std::vector<ScriptInput> out;
    const char* v = std::getenv("BF_MATCH_SCRIPT");
    const std::string sc = v ? v : "";
    for (size_t pos = 0; pos < sc.size();) {
      const size_t comma = std::min(sc.find(',', pos), sc.size());
      const std::string item = sc.substr(pos, comma - pos);
      const size_t colon = item.find(':'), slash = item.find('/');
      if (colon != std::string::npos) {
        ScriptInput in;
        in.at = std::atof(item.substr(0, colon).c_str());
        in.what = item.substr(colon + 1, slash == std::string::npos ? std::string::npos : slash - colon - 1);
        in.hold = slash == std::string::npos ? 0.2 : std::atof(item.substr(slash + 1).c_str());
        out.push_back(in);
      }
      pos = comma + 1;
    }
    return out;
  }();
  // BF_SEEK_VEHICLE=1 (tests): from the first in match pad read, walk a square spiral (forward, a quarter turn right,
  // each leg 1.5 s longer) pressing B every 0.5 s until the soldier is in a vehicle (its on foot camera stops for
  // 1 s after a press); then BF_VEHICLE_SCRIPT (BF_MATCH_SCRIPT's form) from the moment it got in. The vehicles
  // stand near the bases' spawn points, so this reaches one without knowing the map.
  static const bool seek = [] {
    const char* v = std::getenv("BF_SEEK_VEHICLE");
    return v && *v == '1';
  }();
  static const std::vector<ScriptInput> vehicle_script = [] {
    std::vector<ScriptInput> out;
    const char* v = std::getenv("BF_VEHICLE_SCRIPT");
    const std::string sc = v ? v : "";
    for (size_t pos = 0; pos < sc.size();) {
      const size_t comma = std::min(sc.find(',', pos), sc.size());
      const std::string item = sc.substr(pos, comma - pos);
      const size_t colon = item.find(':'), slash = item.find('/');
      if (colon != std::string::npos) {
        ScriptInput in;
        in.at = std::atof(item.substr(0, colon).c_str());
        in.what = item.substr(colon + 1, slash == std::string::npos ? std::string::npos : slash - colon - 1);
        in.hold = slash == std::string::npos ? 0.2 : std::atof(item.substr(slash + 1).c_str());
        out.push_back(in);
      }
      pos = comma + 1;
    }
    return out;
  }();
  // BF_SEEK_AFTER=<s>: the seeker stands still that long first (Wake Island's air raid test: the airfield's flag
  // taken before it walks to the bunker).
  static const double seek_after = [] {
    const char* v = std::getenv("BF_SEEK_AFTER");
    return v && *v ? std::atof(v) : 0.0;
  }();
  static std::chrono::steady_clock::time_point seek_first{};
  bool seek_waiting = false;
  if (seek && bf1943::InMatch() && !menu && !bf1943::g_walking && seek_after > 0) {
    const auto now = std::chrono::steady_clock::now();
    if (!seek_first.time_since_epoch().count()) seek_first = now;
    seek_waiting = std::chrono::duration<double>(now - seek_first).count() < seek_after;
  }
  if (seek && !seek_waiting && bf1943::InMatch() && !menu && !bf1943::g_walking) {
    using SClock = std::chrono::steady_clock;
    static SClock::time_point began{}, last_press{}, off_foot_since{}, in_vehicle_at{};
    static bool in_vehicle = false;
    const auto now = SClock::now();
    if (!began.time_since_epoch().count()) began = now;
    const bool on_foot = bf1943::LocalOnFoot();
    // BF_SEEK_FAST=1: a fast vehicle (a jeep, not a tank): after getting in, the left trigger (drive) for 3 s, or until
    // 21 m (a Sherman makes 16.5 in 3 s, a jeep 21 in about 2.6); a slow one it gets out of again, and presses no B
    // within 7 m of where that vehicle stood or where the drive left it.
    static const bool seek_fast = [] {
      const char* v = std::getenv("BF_SEEK_FAST");
      return v && *v == '1';
    }();
    static std::vector<std::array<float, 3>> slow_vehicles;
    static SClock::time_point probe_until{};
    static float probe_from[3] = {};
    static bool probing = false;
    static bool walked_off = true;  // on foot for a while since the last slow vehicle
    // Getting out of a slow vehicle: B held 0.3 s (a frame's press can fall between the game's 30 Hz ticks), again
    // every 2 s while still seated.
    static SClock::time_point exit_at{};
    bool seek_busy = false;  // this read's input is the exit's or the probe's: no spiral, no vehicle script
    if (exit_at.time_since_epoch().count() && !in_vehicle) {
      const double since_exit = std::chrono::duration<double>(now - exit_at).count();
      if (on_foot && !bf1943::LocalSeated(base)) {
        exit_at = {};
      } else {
        if (since_exit > 2.0) exit_at = now;
        if (since_exit < 0.3) {
          const uint16_t b = uint16_t(p[4] << 8 | p[5]) | 0x2000;
          p[4] = uint8_t(b >> 8), p[5] = uint8_t(b);
        }
        ++packet;
        seek_busy = true;
      }
    }
    const float view[3] = {bf1943::g_dev_view_pos[0].load(), bf1943::g_dev_view_pos[1].load(),
                           bf1943::g_dev_view_pos[2].load()};
    if (in_vehicle && probing && !seek_busy) {
      const float pdx = view[0] - probe_from[0], pdz = view[2] - probe_from[2];
      if (now < probe_until && pdx * pdx + pdz * pdz < 21.0f * 21.0f) {
        p[6] = 0xFF;  // the left trigger: drive
        ++packet;
        seek_busy = true;
      } else {
        probing = false;
        const float dx = view[0] - probe_from[0], dz = view[2] - probe_from[2];
        const float moved = std::sqrt(dx * dx + dz * dz);
        if (moved < 21.0f) {
          // Where it stood, and where the drive left it.
          slow_vehicles.push_back({probe_from[0], probe_from[1], probe_from[2]});
          slow_vehicles.push_back({view[0], view[1], view[2]});
          in_vehicle = false;
          walked_off = false;
          last_press = now;
          exit_at = now;
          REXLOG_INFO("BF seek: {:.1f} m in 3 s, a slow vehicle: out again ({} passed over)", moved,
                      slow_vehicles.size() / 2);
          seek_busy = true;
        } else {
          in_vehicle_at = now;
          REXLOG_INFO("BF seek: {:.1f} m, a fast vehicle: the vehicle script starts (at {:.1f}, {:.1f}, {:.1f})", moved,
                      view[0], view[1], view[2]);
        }
      }
    }
    if (seek_busy) {
    } else if (!in_vehicle) {
      if (on_foot) off_foot_since = {};
      else if (!off_foot_since.time_since_epoch().count()) off_foot_since = now;
      const double since_press = std::chrono::duration<double>(now - last_press).count();
      // In a vehicle the camera stays where the soldier was; dead (the enlist screen) it goes far away.
      static float foot_pos[3] = {};
      const float cam[3] = {bf1943::g_dev_view_pos[0].load(), bf1943::g_dev_view_pos[1].load(),
                            bf1943::g_dev_view_pos[2].load()};
      if (on_foot) std::memcpy(foot_pos, cam, sizeof(cam));
      const float gx = cam[0] - foot_pos[0], gy = cam[1] - foot_pos[1], gz = cam[2] - foot_pos[2];
      const bool near_foot = gx * gx + gy * gy + gz * gz < 15.0f * 15.0f;
      // Seated (the soldier's character physics off, look.cpp): a vehicle for sure, not the enlist screen.
      static SClock::time_point seated_since{};
      if (!bf1943::LocalSeated(base)) seated_since = {};
      else if (!seated_since.time_since_epoch().count()) seated_since = now;
      const bool seated = seated_since.time_since_epoch().count() &&
                          std::chrono::duration<double>(now - seated_since).count() > 1.0;
      // After getting out of a slow vehicle: on foot again for 1.5 s before anything counts as getting in.
      static SClock::time_point foot_again{};
      if (on_foot && !bf1943::LocalSeated(base)) {
        if (!foot_again.time_since_epoch().count()) foot_again = now;
      } else {
        foot_again = {};
      }
      if (foot_again.time_since_epoch().count() && std::chrono::duration<double>(now - foot_again).count() > 1.5)
        walked_off = true;
      const bool settled = slow_vehicles.empty() || walked_off;
      if (!settled) {
      } else if (seated || (off_foot_since.time_since_epoch().count() && last_press.time_since_epoch().count() && near_foot &&
                     std::chrono::duration<double>(now - off_foot_since).count() > 1.0 && since_press < 3.0)) {
        in_vehicle = true;
        in_vehicle_at = now;
        REXLOG_INFO("BF seek: in a vehicle {:.1f} s after the match began",
                    std::chrono::duration<double>(now - began).count());
        if (seek_fast) {
          probing = true;
          probe_until = now + std::chrono::seconds(3);
          std::memcpy(probe_from, view, sizeof(view));
          in_vehicle_at = now + std::chrono::hours(1);  // the vehicle script waits for the probe
        }
      } else if (on_foot && !bf1943::LocalSeated(base)) {
        const float pos[3] = {bf1943::g_dev_camera[0].load(), bf1943::g_dev_camera[1].load(),
                              bf1943::g_dev_camera[2].load()};
        // BF_SEEK_PARTS=<n>: to the nearest vehicle whose part tree has n parts (the trees the game updates,
        // tick_blend.cpp; BF_DEV_TREELIST lists which count is where), not a spiral: walks 0.5 s at a time, takes its
        // heading from the walk, and turns toward the vehicle in between (0.45 s of full stick a quarter turn; which
        // way the stick turns in world terms is found from the first turn). Blocked (under 0.4 m in a walk), a quarter
        // turn right first.
        static const uint32_t seek_parts = [] {
          const char* v = std::getenv("BF_SEEK_PARTS");
          return v && *v ? uint32_t(std::atoi(v)) : 0u;
        }();
        float target[3] = {};
        double target_dist = 0;
        static float sought[3] = {};
        static bool have_sought = false;
        // BF_SEEK_POINT=<x,y,z>[;<x,y,z>...]: to that place instead (a vehicle that is not a part tree: Wake
        // Island's air raid bunker), by the places before it in turn (each passed within 3 m): a way round walls.
        static const std::vector<std::array<float, 3>> seek_points = [] {
          std::vector<std::array<float, 3>> out;
          const char* v = std::getenv("BF_SEEK_POINT");
          for (const char* c = v; c && *c;) {
            std::array<float, 3> pt{};
            if (std::sscanf(c, "%f,%f,%f", &pt[0], &pt[1], &pt[2]) == 3) out.push_back(pt);
            c = std::strchr(c, ';');
            if (c) ++c;
          }
          return out;
        }();
        static size_t seek_leg = 0;
        const std::array<float, 4> seek_point = [&] {
          std::array<float, 4> out{};
          if (seek_points.empty()) return out;
          while (seek_leg + 1 < seek_points.size()) {
            const float dx = seek_points[seek_leg][0] - pos[0], dz = seek_points[seek_leg][2] - pos[2];
            if (dx * dx + dz * dz > 9.0f) break;
            ++seek_leg;
            REXLOG_INFO("BF seek: at way point {} of {}", seek_leg, seek_points.size());
          }
          out[0] = seek_points[seek_leg][0], out[1] = seek_points[seek_leg][1], out[2] = seek_points[seek_leg][2];
          out[3] = 1;
          return out;
        }();
        if (seek_point[3] != 0) {
          std::memcpy(target, seek_point.data(), sizeof(target));
          double d2 = 0;
          for (int c = 0; c < 3; ++c) d2 += (double(target[c]) - pos[c]) * (double(target[c]) - pos[c]);
          target_dist = std::sqrt(d2);
          std::memcpy(sought, target, sizeof(target)), have_sought = true;
        } else if (seek_parts && bf1943::TickBlendNearestTree(seek_parts, pos, target, &target_dist)) {
          std::memcpy(sought, target, sizeof(target)), have_sought = true;
        } else if (have_sought) {  // a parked vehicle drops out of the game's updates after a while (run planelist1)
          std::memcpy(target, sought, sizeof(target));
          double d2 = 0;
          for (int c = 0; c < 3; ++c) d2 += (double(target[c]) - pos[c]) * (double(target[c]) - pos[c]);
          target_dist = std::sqrt(d2);
        }
        const bool steering = (seek_parts || seek_point[3] != 0) && have_sought;
        if (steering) {
          static bool started = false, have_heading = false, logged = false;
          static int phase = 0;  // 0 walking, 1 turning
          static SClock::time_point until{};
          static float from[2] = {};
          static double heading[2] = {}, sign = 1, commanded = 0;
          if (!logged) {
            logged = true;
            REXLOG_INFO("BF seek: walking to the nearest {} part vehicle, {:.1f} m away at {:.1f} {:.1f} {:.1f}",
                        seek_parts, target_dist, target[0], target[1], target[2]);
          }
          const double tx = double(target[0]) - pos[0], tz = double(target[2]) - pos[2];
          static SClock::time_point logged_at{};
          if (now - logged_at > std::chrono::seconds(2)) {
            logged_at = now;
            REXLOG_INFO("BF seek: at {:.1f} {:.1f} {:.1f}, {:.1f} m from {:.1f} {:.1f}", pos[0], pos[1], pos[2], target_dist,
                        target[0], target[2]);
          }
          if (!started || now >= until) {
            if (!started || phase == 1) {  // a turn ended (or the first walk): walk
              started = true, phase = 0;
              from[0] = pos[0], from[1] = pos[2];
              until = now + std::chrono::milliseconds(500);
            } else {  // a walk ended: its heading, and the turn toward the vehicle
              const double wx = double(pos[0]) - from[0], wz = double(pos[2]) - from[1];
              const double wl = std::sqrt(wx * wx + wz * wz);
              if (wl > 0.4) {
                const double hx = wx / wl, hz = wz / wl;
                if (commanded != 0 && have_heading) {  // the turn before this walk: which way did it go?
                  const double rot = std::atan2(heading[0] * hz - heading[1] * hx, heading[0] * hx + heading[1] * hz);
                  if (std::abs(rot) > 0.2 && (rot > 0) != (commanded > 0)) {
                    sign = -sign;
                    REXLOG_INFO("BF seek: the stick turns the other way ({:.2f} for {:.2f}): sign {}", rot, commanded,
                                sign);
                  }
                }
                heading[0] = hx, heading[1] = hz, have_heading = true;
                const double angle = std::atan2(hx * tz - hz * tx, hx * tx + hz * tz);
                if (std::abs(angle) > 0.3 && target_dist > 1.5) {
                  phase = 1, commanded = angle;
                  until = now + std::chrono::milliseconds(int(std::min(1.0, std::abs(angle) / 1.5708 * 0.45) * 1000));
                } else {
                  commanded = 0;
                  from[0] = pos[0], from[1] = pos[2];
                  until = now + std::chrono::milliseconds(500);
                }
              } else {  // blocked: a quarter turn right, then on
                phase = 1, commanded = 0, have_heading = false;
                until = now + std::chrono::milliseconds(450);
              }
            }
          }
          if (phase == 1) {
            const bool right = commanded == 0 || (commanded > 0) == (sign > 0);
            p[12] = right ? 0x7F : 0x80, p[13] = right ? 0xFF : 0x00;
          } else {
            p[10] = 0x7F, p[11] = 0xFF;  // left stick up
          }
        }
        // Stuck (a wall): under 0.3 m in 1.2 s of walking turns a quarter for 0.6 s.
        static SClock::time_point check_at{}, turn_until{};
        static float check_pos[3] = {};
        if (!check_at.time_since_epoch().count() || now - check_at > std::chrono::milliseconds(1200)) {
          const float dx = pos[0] - check_pos[0], dz = pos[2] - check_pos[2];
          if (check_at.time_since_epoch().count() && dx * dx + dz * dz < 0.09f && now > turn_until)
            turn_until = now + std::chrono::milliseconds(600);
          std::memcpy(check_pos, pos, sizeof(pos));
          check_at = now;
        }
        // The spiral: leg k (0, 1, 2...) lasts 1.5 + 1.5 k seconds forward, then 0.45 s of full right stick.
        // BF_SEEK_TURN=<s> first turns right that long (another vehicle than the nearest straight ahead).
        static const double first_turn = [] {
          const char* v = std::getenv("BF_SEEK_TURN");
          return v && *v ? std::atof(v) : 0.0;
        }();
        double t = std::chrono::duration<double>(now - began).count() - first_turn, leg = 1.5;
        bool turning = now < turn_until || t < 0;
        if (t < 0) t = 0;
        for (; !turning;) {
          if (t < leg) break;
          t -= leg;
          if (t < 0.45) {
            turning = true;
            break;
          }
          t -= 0.45;
          leg += 1.5;
        }
        if (steering) {
        } else if (turning) {
          p[12] = 0x7F, p[13] = 0xFF;  // right stick right
        } else {
          p[10] = 0x7F, p[11] = 0xFF;  // left stick up
        }
        uint16_t held = 0;
        bool near_slow = false;
        for (const auto& v : slow_vehicles) {
          const float dx = view[0] - v[0], dz = view[2] - v[2];
          near_slow = near_slow || dx * dx + dz * dz < 7.0f * 7.0f;
        }
        if (near_slow || (steering && target_dist > 8.0) ||
            (seek_point[3] != 0 && seek_leg + 1 < seek_points.size())) {  // not on the way: only at the vehicle sought
        } else if (since_press > 0.5) {
          last_press = now;
          held = 0x2000;  // B
        } else if (since_press < 0.15) {
          held = 0x2000;
        }
        const uint16_t b = uint16_t(p[4] << 8 | p[5]) | held;
        p[4] = uint8_t(b >> 8), p[5] = uint8_t(b);
        ++packet;
      }
    } else {
      const double t = std::chrono::duration<double>(now - in_vehicle_at).count();
      uint16_t held = 0;
      static std::set<std::string> vehicle_keys_down;
      std::set<std::string> vehicle_keys_now;
      for (const ScriptInput& in : vehicle_script) {
        if (t < in.at || t >= in.at + in.hold) continue;
        const std::string& w = in.what;
        if (w.rfind("key=", 0) == 0) {
          vehicle_keys_now.insert(w.substr(4));
          continue;
        }
        if (w.size() >= 3 && (w[0] == 'l' || w[0] == 'r') && w[1] == 's' &&
            (w.size() == 3 || (w[3] >= '0' && w[3] <= '9'))) {
          const int axis = (w[0] == 'l' ? 8 : 12) + (w[2] == 'u' || w[2] == 'd' ? 2 : 0);
          const bool negative = w[2] == 'd' || w[2] == 'l';
          // "rsu": the stick all the way; "rsu25": a quarter of the way (a controller's gentle input, 13 Nov 2026).
          const int percent = w.size() > 3 ? std::clamp(std::atoi(w.c_str() + 3), 1, 100) : 100;
          const int v = (negative ? -32768 : 32767) * percent / 100;
          p[axis] = uint8_t(uint16_t(v) >> 8), p[axis + 1] = uint8_t(v);
          continue;
        }
        held |= w == "a" ? 0x1000 : w == "b" ? 0x2000 : w == "x" ? 0x4000 : w == "y" ? 0x8000 : w == "lb" ? 0x0100
                : w == "rb" ? 0x0200 : w == "up" ? 0x0001 : w == "down" ? 0x0002 : w == "left" ? 0x0004
                : w == "right" ? 0x0008 : 0;
        if (w == "lt") p[6] = 0xFF;
        if (w == "rt") p[7] = 0xFF;
      }
      ScriptKeys(vehicle_keys_down, vehicle_keys_now);
      const uint16_t b = uint16_t(p[4] << 8 | p[5]) | held;
      p[4] = uint8_t(b >> 8), p[5] = uint8_t(b);
      ++packet;
    }
  }
  if (!match_script.empty() && bf1943::InMatch() && !menu && !bf1943::g_walking) {
    static std::chrono::steady_clock::time_point began{};
    if (!began.time_since_epoch().count()) began = std::chrono::steady_clock::now();
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    uint16_t held = 0;
    // "key=<name>" (the SDK's key names, keybinds.cpp: key=W, key=LMB, key=Space): that key held on the keyboard, through
    // Keyboard & mouse's bindings (tests: BF_TEST_MNK=1), from the next pad read.
    static std::set<std::string> keys_down;
    std::set<std::string> keys_now;
    for (const ScriptInput& in : match_script) {
      if (t < in.at || t >= in.at + in.hold) continue;
      const std::string& w = in.what;
      if (w.rfind("key=", 0) == 0) {
        keys_now.insert(w.substr(4));
        continue;
      }
      // "esc": the keyboard's Esc as our menu sees it (game_menu.cpp), once at that moment: the pause opens.
      if (w == "esc") {
        static std::vector<double> pressed;
        if (std::find(pressed.begin(), pressed.end(), in.at) == pressed.end()) {
          pressed.push_back(in.at);
          bf1943::PressEscapeForTest();
          REXLOG_INFO("BF match script: Esc");
        }
        continue;
      }
      // "cvar=<name>=<value>": a setting changed once at that moment (tests: a live switch during a match).
      if (w.rfind("cvar=", 0) == 0) {
        static std::vector<double> set_at;
        const size_t eq = w.find('=', 5);
        if (eq != std::string::npos && std::find(set_at.begin(), set_at.end(), in.at) == set_at.end()) {
          set_at.push_back(in.at);
          rex::cvar::SetFlagByName(w.substr(5, eq - 5), w.substr(eq + 1));
          REXLOG_INFO("BF match script: {} set to {}", w.substr(5, eq - 5), w.substr(eq + 1));
        }
        continue;
      }
      // "wheel": a notch of the mouse wheel, once at that moment (look.cpp WheelNotch).
      if (w == "wheel") {
        static std::vector<double> turned;
        if (std::find(turned.begin(), turned.end(), in.at) == turned.end()) {
          turned.push_back(in.at);
          bf1943::WheelNotch(1);
          REXLOG_INFO("BF match script: wheel notch");
        }
        continue;
      }
      if (w.size() == 3 && (w[0] == 'l' || w[0] == 'r') && w[1] == 's') {
        const int axis = (w[0] == 'l' ? 8 : 12) + (w[2] == 'u' || w[2] == 'd' ? 2 : 0);
        const bool negative = w[2] == 'd' || w[2] == 'l';
        p[axis] = negative ? 0x80 : 0x7F, p[axis + 1] = negative ? 0x00 : 0xFF;
        continue;
      }
#if defined(_WIN32)
      // "rdoc": a RenderDoc capture at that moment (dev_renderdoc.cpp; the game started under RenderDoc).
      if (w == "rdoc") {
        static std::vector<double> fired;
        if (std::find(fired.begin(), fired.end(), in.at) == fired.end()) {
          fired.push_back(in.at);
          bf1943::TriggerRenderDocCapture();
        }
        continue;
      }
#endif
      held |= w == "a" ? 0x1000 : w == "b" ? 0x2000 : w == "x" ? 0x4000 : w == "y" ? 0x8000 : w == "lb" ? 0x0100
              : w == "rb" ? 0x0200 : w == "up" ? 0x0001 : w == "down" ? 0x0002 : w == "left" ? 0x0004
              : w == "right" ? 0x0008 : w == "ls" ? 0x0040 : w == "rs" ? 0x0080 : w == "back" ? 0x0020 : 0;
      if (w == "lt") p[6] = 0xFF;
      if (w == "rt") p[7] = 0xFF;
    }
    ScriptKeys(keys_down, keys_now);
    const uint16_t b = uint16_t(p[4] << 8 | p[5]) | held;
    p[4] = uint8_t(b >> 8), p[5] = uint8_t(b);
    ++packet;
    if (held) bf1943::NoteMatchDevice(false);  // the script's presses are the controller's (the HUD's prompts)
  }
  // BF_DEV_PADLOG=1 (tests): each change of the buttons, triggers or left stick the game gets, on foot or seated
  // (the keyboard's mappings; the right stick is left out, the mouse moves it all the time in a vehicle).
  {
    static const bool padlog = std::getenv("BF_DEV_PADLOG") != nullptr;
    static uint8_t logged[8] = {};
    if (padlog && std::memcmp(logged, p + 4, 8)) {
      std::memcpy(logged, p + 4, 8);
      REXLOG_INFO("BF pad: buttons {:04X} lt {} rt {} lx {} ly {} ({})", uint16_t(p[4] << 8 | p[5]), p[6], p[7],
                  int16_t(p[8] << 8 | p[9]), int16_t(p[10] << 8 | p[11]),
                  bf1943::LocalSeated(base) ? "seated" : bf1943::LocalOnFoot() ? "on foot" : "neither");
    }
  }
#endif
  if (press != last) ++packet, last = press;
  const uint32_t n = (uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]) + packet;
  p[0] = uint8_t(n >> 24);
  p[1] = uint8_t(n >> 16);
  p[2] = uint8_t(n >> 8);
  p[3] = uint8_t(n);
}
