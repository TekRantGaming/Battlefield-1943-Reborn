// Battlefield 1943 Reborn, SDK v0.11+ port (phase 3): fullscreen display modes (exclusive fullscreen at a chosen
// resolution). Our v0.10 SDK patch (src/ui/window_sdl.cpp there), moved into the game unchanged: it is all SDL, and
// the game links SDL3.dll itself in v0.11. The window is the compat facade's (Window::sdl_window).
#include <rex/ui/display_mode.h>

#include <cmath>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/logging.h>
#include <rex/ui/window.h>

namespace rex::ui {

namespace {

SDL_Window* SdlWindowOf(const Window* window) { return window ? window->sdl_window() : nullptr; }

// `monitor` as the monitor cvar (1-based, SDL's order); 0 or out of range: the window's display.
SDL_DisplayID DisplayFor(int monitor, SDL_Window* window) {
  SDL_DisplayID id = 0;
  if (monitor > 0) {
    int count = 0;
    if (SDL_DisplayID* ids = SDL_GetDisplays(&count)) {
      if (monitor <= count) {
        id = ids[monitor - 1];
      }
      SDL_free(ids);
    }
  }
  if (!id && window) {
    id = SDL_GetDisplayForWindow(window);
  }
  return id ? id : SDL_GetPrimaryDisplay();
}

// SDL's sizes are in points where the display's density is above 1 (macOS); the modes here are pixels.
DisplayMode ToPixels(const SDL_DisplayMode& m) {
  const float density = m.pixel_density > 0.0f ? m.pixel_density : 1.0f;
  return {int(std::lround(m.w * density)), int(std::lround(m.h * density)), m.refresh_rate};
}

}  // namespace

std::vector<DisplayMode> GetFullscreenDisplayModes(int monitor, const Window* window) {
  std::vector<DisplayMode> out;
  int count = 0;
  SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(DisplayFor(monitor, SdlWindowOf(window)), &count);
  if (!modes) {
    return out;
  }
  for (int i = 0; i < count; ++i) {
    const DisplayMode m = ToPixels(*modes[i]);
    // SDL lists a size and rate again for each pixel format.
    const bool seen = std::any_of(out.begin(), out.end(), [&](const DisplayMode& o) {
      return o.width == m.width && o.height == m.height && std::fabs(o.refresh_rate - m.refresh_rate) < 0.01f;
    });
    if (!seen) {
      out.push_back(m);
    }
  }
  SDL_free(modes);
  return out;
}

std::vector<DisplayInfo> GetDisplays() {
  std::vector<DisplayInfo> out;
  int count = 0;
  if (SDL_DisplayID* ids = SDL_GetDisplays(&count)) {
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    for (int i = 0; i < count; ++i) {
      DisplayInfo d;
      const char* name = SDL_GetDisplayName(ids[i]);
      d.name = name ? name : "";
      if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(ids[i])) {
        d.desktop = ToPixels(*mode);
      }
      d.primary = ids[i] == primary;
      out.push_back(std::move(d));
    }
    SDL_free(ids);
  }
  return out;
}

DisplayMode GetDesktopDisplayMode(int monitor, const Window* window) {
  const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(DisplayFor(monitor, SdlWindowOf(window)));
  return mode ? ToPixels(*mode) : DisplayMode{};
}

DisplayMode GetCurrentDisplayMode(int monitor, const Window* window) {
  const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(DisplayFor(monitor, SdlWindowOf(window)));
  return mode ? ToPixels(*mode) : DisplayMode{};
}

bool SetFullscreenDisplayMode(Window* window, int monitor, int width, int height, float refresh_rate,
                              DisplayMode* chosen) {
  SDL_Window* sdl_window = SdlWindowOf(window);
  if (!sdl_window) {
    REXLOG_WARN("Fullscreen mode: the window is not open");
    return false;
  }
  if (width <= 0 || height <= 0) {
    SDL_SetWindowFullscreenMode(sdl_window, nullptr);
    return true;
  }
  const SDL_DisplayID display = DisplayFor(monitor, sdl_window);
  const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
  const float density = desktop && desktop->pixel_density > 0.0f ? desktop->pixel_density : 1.0f;
  SDL_DisplayMode mode{};
  // A mode the list offers (GetFullscreenDisplayModes, in pixels) is taken by its own pixel size. Dividing by
  // the desktop's density is wrong where the modes do not share it: KDE Wayland at 150% lists its modes in
  // pixels and the desktop in points, and 1280x720 became 864x486. The nearest rate wins (the highest with none).
  bool listed = false;
  int count = 0;
  if (SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count)) {
    float best = 0.0f;
    for (int i = 0; i < count; ++i) {
      const DisplayMode m = ToPixels(*modes[i]);
      if (m.width != width || m.height != height) {
        continue;
      }
      const float off = refresh_rate > 0.0f ? std::fabs(m.refresh_rate - refresh_rate) : -m.refresh_rate;
      if (!listed || off < best) {
        mode = *modes[i];
        best = off;
        listed = true;
      }
    }
    SDL_free(modes);
  }
  if (!listed && !SDL_GetClosestFullscreenDisplayMode(display, int(std::lround(width / density)),
                                                      int(std::lround(height / density)), refresh_rate, true, &mode)) {
    REXLOG_WARN("Fullscreen mode {}x{}: the display has no such mode ({})", width, height, SDL_GetError());
    SDL_SetWindowFullscreenMode(sdl_window, nullptr);
    return false;
  }
  if (!SDL_SetWindowFullscreenMode(sdl_window, &mode)) {
    REXLOG_WARN("Fullscreen mode {}x{}: {}", width, height, SDL_GetError());
    SDL_SetWindowFullscreenMode(sdl_window, nullptr);
    return false;
  }
  if (chosen) {
    *chosen = ToPixels(mode);
  }
  return true;
}

}  // namespace rex::ui
