// Battlefield 1943 Reborn, SDK v0.11+ port (phase 3): v0.10's rex::ui::Window, on v0.11's UI system.
//
// v0.11 keeps rex/ui/window.h for its window types and has no window object: the window's title and fullscreen are
// settings (ui.title, ui.fullscreen), its handles come from IUISystem::GetNativeWindow, its events from a listener.
// This facade gives the game v0.10's calls on top of that; compat/ui_compat.cpp makes it once the UI system runs.
#pragma once

#include_next <rex/ui/window.h>

#include <string_view>

#include <rex/input_keys.h>

struct SDL_Window;

namespace rex::ui {

class IUISystem;

// v0.10's input events, as the game's listeners read them (look.cpp, playtest_mark.cpp).
class MouseEvent {
 public:
  MouseEvent(float x, float y, float dx, float dy, float scroll_y) : x_(x), y_(y), dx_(dx), dy_(dy), scroll_y_(scroll_y) {}
  float x() const { return x_; }
  float y() const { return y_; }
  float dx() const { return dx_; }
  float dy() const { return dy_; }
  float scroll_y() const { return scroll_y_; }

 private:
  float x_, y_, dx_, dy_, scroll_y_;
};

class KeyEvent {
 public:
  KeyEvent(::rex::VirtualKey key, bool repeat) : key_(key), repeat_(repeat) {}
  ::rex::VirtualKey virtual_key() const { return key_; }
  // v0.10: the key was already down (a repeat).
  bool prev_state() const { return repeat_; }

 private:
  ::rex::VirtualKey key_;
  bool repeat_;
};

class WindowInputListener {
 public:
  virtual ~WindowInputListener() = default;
  virtual void OnMouseMove(MouseEvent& e) { (void)e; }
  virtual void OnMouseWheel(MouseEvent& e) { (void)e; }
  virtual void OnKeyDown(KeyEvent& e) { (void)e; }
};

class Window {
 public:
  explicit Window(IUISystem& ui) : ui_(&ui) {}

  void SetTitle(std::string_view title);
  void SetFullscreen(bool fullscreen);
  bool IsFullscreen() const;
  // As the window's close button: the program shuts down (exit code 0).
  void RequestClose();
  // The HWND on Windows, the X11 window or Wayland surface elsewhere; null when there is no window.
  void* GetNativeWindowHandle() const;
  SDL_Window* sdl_window() const;
  bool IsTextInputActive() const;
  // v0.10's DPI: 96 at 100 % scaling.
  float GetDpi() const;
  static constexpr float GetMediumDpi() { return 96.0f; }
  // Listeners get the window's mouse motion, wheel and key presses on the UI thread. z_order is v0.10's and unused.
  void AddInputListener(WindowInputListener* listener, int z_order = 0);
  void RemoveInputListener(WindowInputListener* listener);

  IUISystem& ui() const { return *ui_; }

 private:
  IUISystem* ui_;
};

// v0.10's SDL window class: the same window (sdl_window()).
using WindowSDL = Window;

}  // namespace rex::ui
