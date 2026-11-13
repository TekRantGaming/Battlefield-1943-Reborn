// Telling the player when the game could not start (TekRantGaming, 13 Nov 2026: "Yes, add it").
//
// A player's 0.3.0 closed 2 s after its launcher came up, 8 starts of 8: the game's module did not launch, the app
// quit by itself and its teardown crashed on the way out (the SDK copy's ReXApp::OnDestroy, mended the same night).
// Mended, the app left without a word: the reason was only in the log. Now, when the app quits by itself because the
// game could not start (ReXApp::OnShutdown, bf1943_app.h, with the SDK's SelfQuitWhy), the game's window goes and a
// message box says so in plain words, with where the log is and a button that shows it in its folder. A controller
// works too (A, B or Start close it, X shows the log: a couch player has no mouse), and the box never keeps the
// process for ever (ten minutes at most: a box that cannot be seen, in Steam's gaming mode say, must not leave the
// game running unseen).
//
// A dedicated server tells nobody (its log has it). Scripted test runs (BF_DEV_SCRIPTED, developer builds) only log
// what the player would be told, unless BF_DEV_START_BOX=1 asks for the box itself; BF_DEV_START_BOX_PAD=<s>:<a|x>
// presses that controller button on it after <s> seconds (tests).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h>
#else
#include <spawn.h>
extern char** environ;
#endif

#include <SDL3/SDL.h>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/rex_app.h>

namespace bf1943 {
namespace {

std::string Utf8(const std::filesystem::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

// The log's folder in the file manager, the log itself picked out where the platform can.
void OpenLogFolder(const std::filesystem::path& log) {
  REXLOG_INFO("BF start failure: showing {}", Utf8(log));
  if (const char* v = std::getenv("BF_DEV_NO_OPEN"); v && *v == '1') return;  // tests: logged only
#if defined(_WIN32)
  ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + log.wstring() + L"\"").c_str(), nullptr,
                SW_SHOWNORMAL);
#else
  const std::string folder = log.parent_path().string();
  const char* argv[] = {"xdg-open", folder.c_str(), nullptr};
  pid_t pid;
  posix_spawnp(&pid, "xdg-open", nullptr, nullptr, const_cast<char* const*>(argv), environ);
#endif
}

// Every connected controller's buttons, merged, as XInput's bits (A 0x1000, B 0x2000, X 0x4000, Start 0x0010).
uint16_t PadButtons() {
#if defined(_WIN32)
  using GetStateFn = DWORD(WINAPI*)(DWORD, void*);
  static const GetStateFn get_state = [] {
    HMODULE m = LoadLibraryA("xinput1_4.dll");
    if (!m) m = LoadLibraryA("xinput9_1_0.dll");
    return m ? reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState")) : nullptr;
  }();
  if (!get_state) return 0;
  uint16_t all = 0;
  for (DWORD i = 0; i < 4; ++i) {
    struct {
      DWORD packet;
      WORD buttons;
      BYTE lt, rt;
      SHORT lx, ly, rx, ry;
    } state{};
    if (get_state(i, &state) == 0) all |= state.buttons;
  }
  return all;
#else
  // SDL's gamepads, as the launcher reads them on Linux (game_menu.cpp HostPad): buttons by position.
  static const bool ok = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  if (!ok) return 0;
  SDL_UpdateGamepads();
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  uint16_t all = 0;
  for (int i = 0; ids && i < count; ++i) {
    SDL_Gamepad* g = SDL_GetGamepadFromID(ids[i]);
    if (!g) g = SDL_OpenGamepad(ids[i]);
    if (!g) continue;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH)) all |= 0x1000;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_EAST)) all |= 0x2000;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_WEST)) all |= 0x4000;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START)) all |= 0x0010;
  }
  SDL_free(ids);
  return all;
#endif
}

[[noreturn]] void Leave(int exit_code) {
  rex::FlushLogging();
  std::_Exit(exit_code);
}

}  // namespace

// `since_launch`: seconds since the game's module was launched, negative when it never was. `log`: the log file (may
// be empty). `exit_code`: what the process ends with (the box's controller and time limit end it themselves).
// `native_window`: the game's window (rex::ui::Window::GetNativeWindowHandle: an HWND on Windows), hidden first.
void TellStartFailure(rex::SelfQuitCause cause, const char* reason, double since_launch,
                      const std::filesystem::path& log, int exit_code, void* native_window) {
  using rex::SelfQuitCause;
  const char* what = nullptr;
  switch (cause) {
    case SelfQuitCause::kNone:
      return;
    case SelfQuitCause::kModuleLaunchFailed:
      what = "The game could not start.\n\nBattlefield 1943's program file did not load. Your game files may be "
             "incomplete or damaged.";
      break;
    case SelfQuitCause::kRuntimeSetupFailed:
      what = "The game could not start.\n\nIts graphics, sound or files could not be set up.";
      break;
    case SelfQuitCause::kInitFailed:
      what = "Battlefield 1943 Reborn could not start.\n\nIts window or graphics could not be set up.";
      break;
    case SelfQuitCause::kGuestExited:
      // A game that ran and then ended is no failure to start: only one that ended while it was starting.
      if (!(since_launch >= 0 && since_launch < 45.0)) return;
      what = "The game closed by itself while it was starting.";
      break;
  }
  std::string text = what;
  if (!log.empty()) text += "\n\nThe log has the details:\n" + Utf8(log);
  text += "\n\nIf this keeps happening, send us that file: Report an issue on the main menu has our Discord.";
  std::string one_line = what;
  for (char& c : one_line)
    if (c == '\n') c = ' ';
  REXLOG_ERROR("BF start failure ({}{}): the player is told: {}", reason ? reason : "no reason given",
               since_launch >= 0 ? fmt::format(", {:.1f} s after the launch", since_launch) : std::string(), one_line);
  rex::FlushLogging();
#if defined(BF_DEV_TOOLS)
  {
    const char* scripted = std::getenv("BF_DEV_SCRIPTED");
    const char* box = std::getenv("BF_DEV_START_BOX");
    if (scripted && *scripted == '1' && !(box && *box == '1')) {
      REXLOG_INFO("BF start failure: a scripted test run, no message box (BF_DEV_START_BOX=1 shows it)");
      return;
    }
  }
#endif
  text += "\n\nOn a controller: A closes this, X shows the log.";

  // The game's window goes first: a box over a fullscreen game can end up behind it, and the picture left in the
  // window is a frozen page of the launcher. By its own handle on Windows: the window is the runtime's SDL's, and
  // this program's own copy of SDL knows no windows (run sqmsg2: SDL_GetWindows found none, the window stayed).
#if defined(_WIN32)
  if (native_window) ShowWindow(static_cast<HWND>(native_window), SW_HIDE);
#else
  (void)native_window;
  {
    int count = 0;
    if (SDL_Window** windows = SDL_GetWindows(&count)) {
      for (int i = 0; i < count; ++i) SDL_HideWindow(windows[i]);
      SDL_free(windows);
    }
  }
#endif

  // The controller and the time limit, beside the box (which holds this thread until it is answered): either ends
  // the process, and the box with it.
  std::thread([log, exit_code] {
    using Clock = std::chrono::steady_clock;
    const auto began = Clock::now();
    double script_at = -1;
    char script_button = 0;
#if defined(BF_DEV_TOOLS)
    if (const char* v = std::getenv("BF_DEV_START_BOX_PAD"); v && *v) {
      script_at = std::atof(v);
      if (const char* colon = std::strchr(v, ':')) script_button = colon[1];
    }
#endif
    uint16_t before = PadButtons();  // held from before the box: not a press on it
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      const double t = std::chrono::duration<double>(Clock::now() - began).count();
      const uint16_t now = PadButtons();
      uint16_t pressed = now & ~before;
      before = now;
      if (script_at >= 0 && t >= script_at) pressed |= script_button == 'x' ? 0x4000 : 0x1000, script_at = -1;
      if (pressed & 0x4000) {
        REXLOG_INFO("BF start failure: the box answered by the controller's X (show the log)");
        if (!log.empty()) OpenLogFolder(log);
        Leave(exit_code);
      }
      if (pressed & (0x1000 | 0x2000 | 0x0010)) {
        REXLOG_INFO("BF start failure: the box closed by the controller");
        Leave(exit_code);
      }
      if (t > 600.0) {
        REXLOG_INFO("BF start failure: the box was not answered in ten minutes: leaving");
        Leave(exit_code);
      }
    }
  }).detach();

  enum { kClose = 0, kOpenFolder = 1 };
  const SDL_MessageBoxButtonData buttons[] = {
      {0, kOpenFolder, "Show the log"},  // short: the box cuts a long caption off (run sqmsg3)
      {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, kClose, "Close"},
  };
  SDL_MessageBoxData box{};
  box.flags = SDL_MESSAGEBOX_ERROR;
  box.window = nullptr;
  box.title = "Battlefield 1943 Reborn";
  box.message = text.c_str();
  box.numbuttons = log.empty() ? 1 : 2;
  box.buttons = log.empty() ? buttons + 1 : buttons;
  int answer = kClose;
  if (!SDL_ShowMessageBox(&box, &answer)) {
    REXLOG_WARN("BF start failure: the message box could not be shown: {}", SDL_GetError());
    return;
  }
  REXLOG_INFO("BF start failure: the box answered: {}", answer == kOpenFolder ? "show the log" : "close");
  if (answer == kOpenFolder && !log.empty()) OpenLogFolder(log);
}

}  // namespace bf1943
