// Display modes, from King Kong (kk/src/display_modes.cpp, tag v2.1.0); see display_modes.h.

#include "display_modes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

#include <rex/logging.h>
#include <rex/ui/display_mode.h>

#if !defined(_WIN32)
#include <SDL3/SDL.h>
#include <cstring>
#endif

namespace bf1943 {

namespace {

// Linux under XWayland on a scaled desktop (KDE at 185% on a ROG Ally, its X11 apps "scaled by the system"): the X11
// desktop is the scaled size (1038x584 for a 1920x1080 display), so a borderless window at it is drawn that small and
// stretched by the compositor, soft. The display's modes still hold its own size (1920x1080@119.93), which XWayland
// emulates for a fullscreen window without changing the display, as the exclusive Fullscreen does: Borderless takes the
// largest of them there (TekRantGaming, 11 Nov 2026: "fix the soft picture in Borderless on scaled Linux desktops"). Nothing
// elsewhere: Windows, X11 itself (a smaller desktop there is the player's own choice) and Wayland's own driver.
std::optional<DisplayMode> ScaledDesktopNativeMode(const DisplayMode& desktop, const std::vector<DisplayMode>& modes) {
#if defined(_WIN32)
  (void)desktop, (void)modes;
  return std::nullopt;
#else
  const char* wayland = std::getenv("WAYLAND_DISPLAY");
  const char* driver = SDL_GetCurrentVideoDriver();
  if (!wayland || !*wayland || !driver || std::strcmp(driver, "x11") != 0) return std::nullopt;
  if (modes.empty() || desktop.width <= 0 || desktop.height <= 0) return std::nullopt;
  DisplayMode largest = modes.front();  // SDL's order: largest first
  for (const DisplayMode& m : modes)
    if (m.width * m.height > largest.width * largest.height) largest = m;
  if (largest.width <= desktop.width && largest.height <= desktop.height) return std::nullopt;
  for (const DisplayMode& m : modes)  // its highest rate
    if (m.width == largest.width && m.height == largest.height && m.refresh > largest.refresh) largest.refresh = m.refresh;
  return largest;
#endif
}

std::string Hz(float refresh) {
  char buf[32];
  // 60, 144; 59.94, 119.88 keep their decimals.
  if (std::fabs(refresh - std::round(refresh)) < 0.05f) std::snprintf(buf, sizeof(buf), "%d", int(std::lround(refresh)));
  else std::snprintf(buf, sizeof(buf), "%.2f", refresh);
  return buf;
}

DisplayMode FromSdk(const rex::ui::DisplayMode& m) { return {m.width, m.height, m.refresh_rate}; }

}  // namespace

std::string DisplayMode::Value() const {
  return std::to_string(width) + "x" + std::to_string(height) + "@" + Hz(refresh);
}

std::string DisplayMode::Label() const {
  return std::to_string(width) + " \xC3\x97 " + std::to_string(height) + "  \xC2\xB7  " + Hz(refresh) + " Hz";
}

std::string DisplayMode::RateLabel() const { return Hz(refresh) + " Hz"; }

std::optional<DisplayMode> ParseDisplayMode(const std::string& value) {
  DisplayMode m;
  const size_t x = value.find('x'), at = value.find('@');
  if (x == std::string::npos) return std::nullopt;
  m.width = std::atoi(value.c_str());
  m.height = std::atoi(value.c_str() + x + 1);
  if (at != std::string::npos) m.refresh = float(std::atof(value.c_str() + at + 1));
  if (m.width < 320 || m.height < 200 || m.width > 16384 || m.height > 16384) return std::nullopt;
  return m;
}

std::vector<DisplayMode> ListDisplayModes(int monitor, const rex::ui::Window* window) {
  std::vector<DisplayMode> out;
  std::unordered_set<std::string> seen;
  for (const rex::ui::DisplayMode& m : rex::ui::GetFullscreenDisplayModes(monitor, window)) {
    const DisplayMode mode = FromSdk(m);
    // Two rates that print the same (59.94 and 59.95) are one choice.
    if (mode.width >= 640 && mode.height >= 480 && seen.insert(mode.Value()).second) out.push_back(mode);
  }
  return out;  // SDL's order: largest first, then the highest refresh rate
}

DisplayMode DesktopDisplayMode(int monitor, const rex::ui::Window* window) {
  return FromSdk(rex::ui::GetDesktopDisplayMode(monitor, window));
}

float HighestRefreshAt(const std::vector<DisplayMode>& modes, int width, int height) {
  float best = 0;
  for (const DisplayMode& m : modes)
    if (m.width == width && m.height == height) best = std::max(best, m.refresh);
  return best;
}

bool DesktopBelowHighest(const DisplayMode& desktop, float highest) {
  return desktop.width > 0 && desktop.refresh > 0 && highest > 0 && highest - desktop.refresh >= 1.0f;
}

std::vector<MonitorInfo> ListDisplays() {
  std::vector<MonitorInfo> out;
  for (const rex::ui::DisplayInfo& d : rex::ui::GetDisplays())
    out.push_back({d.name, d.desktop.width, d.desktop.height, d.primary});
  return out;
}

void ApplyFullscreenMode(rex::ui::Window* window, int monitor, const std::string& value) {
  if (!window) return;
  auto want = ParseDisplayMode(value);
  std::vector<DisplayMode> modes;
  for (const rex::ui::DisplayMode& m : rex::ui::GetFullscreenDisplayModes(monitor, window)) modes.push_back(FromSdk(m));
  {
    const DisplayMode desktop = DesktopDisplayMode(monitor, window);
    const float highest = HighestRefreshAt(modes, desktop.width, desktop.height);
    REXLOG_INFO("BF screen refresh: desktop {} Hz at {}x{}, the display's highest at that size {} Hz{}",
                Hz(desktop.refresh), desktop.width, desktop.height, highest > 0 ? Hz(highest) : "unknown",
                DesktopBelowHighest(desktop, highest) ? " (Windows keeps the screen below its highest)" : "");
  }
  if (want) {
    const float highest = HighestRefreshAt(modes, want->width, want->height);
    if (highest > 0 && std::fabs(highest - want->refresh) >= 0.01f) want->refresh = highest;
  }
  if (!want) {
    const DisplayMode desktop = DesktopDisplayMode(monitor, window);
    if (const auto native = ScaledDesktopNativeMode(desktop, modes)) {
      rex::ui::DisplayMode chosen;
      if (rex::ui::SetFullscreenDisplayMode(window, monitor, native->width, native->height, native->refresh, &chosen)) {
        REXLOG_INFO("BF fullscreen mode: desktop (borderless) on a scaled desktop ({}x{} for the display's {}x{}): the "
                    "display's own {} (XWayland emulates it, the display is not changed)",
                    desktop.width, desktop.height, native->width, native->height, FromSdk(chosen).Value());
        return;
      }
    }
    rex::ui::SetFullscreenDisplayMode(window, 0, 0, 0, 0.0f);  // borderless at the desktop's resolution
    REXLOG_INFO("BF fullscreen mode: desktop (borderless)");
    return;
  }
  rex::ui::DisplayMode chosen;
  if (!rex::ui::SetFullscreenDisplayMode(window, monitor, want->width, want->height, want->refresh, &chosen)) {
    REXLOG_WARN("BF fullscreen mode {}: not available; desktop instead", value);
    return;
  }
  REXLOG_INFO("BF fullscreen mode {} (exclusive; asked for {})", FromSdk(chosen).Value(), value);
}

}  // namespace bf1943
