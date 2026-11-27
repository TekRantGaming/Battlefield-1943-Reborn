// Battlefield 1943 Reborn, SDK v0.11+ port (phase 3): v0.10's window, its input listeners and the game's key binds, on
// v0.11's UI system (compat/rex/ui/window.h, keybinds.h, ui_compat.h).
#include "ui_compat.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/app/app.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/settings_registry.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/ui_system.h>
#include <rex/ui/window.h>

namespace rex::ui {

namespace {

std::string SettingText(std::string_view name) {
  auto found = REX_SETTINGS().Find(name);
  return found.has_value() && *found ? (*found)->GetText() : std::string();
}

void SetSetting(std::string_view name, std::string_view text) {
  if (auto r = REX_SETTINGS().SetText(name, text); !r.has_value())
    REXLOG_WARN("BF UI: setting {} to {} failed: {}", name, text, r.error().GetDescription());
}

struct Bind {
  std::string name;  // the cvar holding its key
  std::function<void()> action;
};

// The window's input, passed on to v0.10 style listeners and the game's binds. One for the program.
class Dispatcher final : public IUIEventListener {
 public:
  void OnUIEvent(const UIEvent& event) override { (void)event; }

  void OnInputEvent(InputEvent& event) override {
    std::vector<WindowInputListener*> listeners;
    {
      std::lock_guard lock(mutex);
      listeners = this->listeners;
    }
    switch (event.type) {
      case InputEvent::Type::kMotion: {
        MouseEvent e(event.x, event.y, event.dx, event.dy, 0.0f);
        for (auto* l : listeners) l->OnMouseMove(e);
        break;
      }
      case InputEvent::Type::kWheel: {
        MouseEvent e(event.x, event.y, 0.0f, 0.0f, event.scroll_y);
        for (auto* l : listeners) l->OnMouseWheel(e);
        break;
      }
      case InputEvent::Type::kKeyDown: {
        KeyEvent e(event.key, event.repeat);
        for (auto* l : listeners) l->OnKeyDown(e);
        if (!event.repeat) RunBinds(event.key);
        break;
      }
      default:
        break;
    }
  }

  void RunBinds(::rex::VirtualKey key) {
    std::vector<std::function<void()>> actions;
    {
      std::lock_guard lock(mutex);
      for (const Bind& b : binds) {
        const std::string text = ::rex::cvar::GetFlagByName(b.name);
        if (!text.empty() && ::rex::ParseVirtualKey(text) == key) actions.push_back(b.action);
      }
    }
    for (auto& a : actions) a();
  }

  std::mutex mutex;
  std::vector<WindowInputListener*> listeners;
  std::vector<Bind> binds;
  ScopeGuard guard;
  bool attached = false;
};

Dispatcher& dispatcher() {
  static Dispatcher d;
  return d;
}

struct Compat {
  IUISystem* ui = nullptr;
  std::unique_ptr<Window> window;
  std::unique_ptr<ImGuiDrawer> drawer;
};

Compat& CompatState() {
  static Compat c;
  return c;
}

}  // namespace

void Window::SetTitle(std::string_view title) { SetSetting("ui.title", title); }

void Window::SetFullscreen(bool fullscreen) { SetSetting("ui.fullscreen", fullscreen ? "true" : "false"); }

bool Window::IsFullscreen() const { return SettingText("ui.fullscreen") == "true"; }

void Window::RequestClose() { ::rex::app::RequestShutdown(0); }

void* Window::GetNativeWindowHandle() const {
  auto native = ui_->GetNativeWindow();
  return native.has_value() ? native->window : nullptr;
}

SDL_Window* Window::sdl_window() const {
  auto native = ui_->GetNativeWindow();
  return native.has_value() ? static_cast<SDL_Window*>(native->sdl) : nullptr;
}

bool Window::IsTextInputActive() const {
  SDL_Window* w = sdl_window();
  return w && SDL_TextInputActive(w);
}

float Window::GetDpi() const {
  SDL_Window* w = sdl_window();
  const float scale = w ? SDL_GetWindowDisplayScale(w) : 0.0f;
  return (scale > 0.0f ? scale : 1.0f) * GetMediumDpi();
}

void Window::AddInputListener(WindowInputListener* listener, int z_order) {
  (void)z_order;
  Dispatcher& d = dispatcher();
  std::lock_guard lock(d.mutex);
  if (std::find(d.listeners.begin(), d.listeners.end(), listener) == d.listeners.end()) d.listeners.push_back(listener);
}

void Window::RemoveInputListener(WindowInputListener* listener) {
  Dispatcher& d = dispatcher();
  std::lock_guard lock(d.mutex);
  d.listeners.erase(std::remove(d.listeners.begin(), d.listeners.end(), listener), d.listeners.end());
}

void RegisterBind(std::string_view name, std::string_view default_key, std::string_view description,
                  std::function<void()> action) {
  // The key as a string cvar of the game's registry (v0.10's bind cvar), unless already there.
  if (!::rex::cvar::GetFlagInfo(name)) {
    auto key = std::make_shared<std::string>(default_key);
    ::rex::cvar::FlagEntry entry;
    entry.name = std::string(name);
    entry.description = std::string(description);
    entry.category = "Binds";
    entry.type = ::rex::cvar::FlagType::String;
    entry.default_value = std::string(default_key);
    entry.setter = [key](std::string_view v) {
      *key = std::string(v);
      return true;
    };
    entry.getter = [key] { return *key; };
    ::rex::cvar::RegisterFlag(std::move(entry));
  }
  Dispatcher& d = dispatcher();
  std::lock_guard lock(d.mutex);
  d.binds.push_back({std::string(name), std::move(action)});
}

void UnregisterBind(std::string_view name) {
  // v0.10's names of the SDK's own overlays' binds, and their v0.11 settings.
  if (name == "bind_debug_overlay") return SetSetting("ui.bind.debug", "");
  if (name == "bind_settings") return SetSetting("ui.bind.settings", "");
  if (name == "bind_console") return SetSetting("ui.bind.console", "");
  if (name == "bind_achievements") return SetSetting("ui.bind.achievements", "");
  Dispatcher& d = dispatcher();
  std::lock_guard lock(d.mutex);
  d.binds.erase(std::remove_if(d.binds.begin(), d.binds.end(), [&](const Bind& b) { return b.name == name; }),
                d.binds.end());
}

namespace compat {

void Attach(IUISystem& ui) {
  Compat& c = CompatState();
  c.ui = &ui;
  if (ui.HasWindow()) c.window = std::make_unique<Window>(ui);
  c.drawer = std::make_unique<ImGuiDrawer>(ui);
  Dispatcher& d = dispatcher();
  if (!d.attached) {
    d.guard = ui.AddEventListener(d);
    d.attached = true;
  }
}

void Detach() {
  Compat& c = CompatState();
  dispatcher().guard.Reset();
  dispatcher().attached = false;
  c.window.reset();
  c.drawer.reset();
  c.ui = nullptr;
}

Window* GetWindow() { return CompatState().window.get(); }
ImGuiDrawer* GetDrawer() { return CompatState().drawer.get(); }
IUISystem* GetUI() { return CompatState().ui; }

}  // namespace compat

}  // namespace rex::ui
