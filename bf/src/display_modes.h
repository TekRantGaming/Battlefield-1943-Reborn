// Display modes for the Display settings, from King Kong (kk/src/display_modes.h, tag v2.1.0): the fullscreen mode
// is "desktop" (a borderless window at the desktop's resolution, the SDK's own fullscreen) or one of the monitor's
// display modes, "2560x1440@144", for exclusive fullscreen at that size and the display's highest refresh rate. The
// SDK keeps the mode per window (rex::ui::SetFullscreenDisplayMode) and uses it whenever the window is fullscreen.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rex::ui {
class Window;
}

namespace bf1943 {

struct DisplayMode {
  int width = 0, height = 0;  // pixels
  float refresh = 0;          // Hz (0: unknown)
  std::string Value() const;      // "2560x1440@144"
  std::string Label() const;      // "2560 x 1440  .  144 Hz"
  std::string RateLabel() const;  // "144 Hz"
};

struct MonitorInfo {
  std::string name;
  int width = 0, height = 0;
  bool primary = false;
};

// "2560x1440@144" -> the mode; nullopt for "desktop" or anything it can't read.
std::optional<DisplayMode> ParseDisplayMode(const std::string& value);

// The display modes of a display (monitor: 1-based in the SDK's order; 0: the display `window` is on), largest
// first, each size and rate once. UI thread.
std::vector<DisplayMode> ListDisplayModes(int monitor, const rex::ui::Window* window);

// The display's mode on the desktop (width 0 when unknown). UI thread.
DisplayMode DesktopDisplayMode(int monitor, const rex::ui::Window* window);

// The highest refresh rate in `modes` at width x height (0 when there is no mode of that size).
float HighestRefreshAt(const std::vector<DisplayMode>& modes, int width, int height);

// True when the desktop runs clearly below the display's highest rate at its size (59.94 against 60 is not).
bool DesktopBelowHighest(const DisplayMode& desktop, float highest);

// Every display, in the order the monitor setting counts them (monitor = index + 1). UI thread.
std::vector<MonitorInfo> ListDisplays();

// Sets the window's fullscreen mode from a fullscreen mode value ("desktop" or a mode) on `monitor`; a mode the
// display doesn't have falls back to the closest one, or to desktop. Exclusive fullscreen always runs at the
// display's highest refresh rate at that size. Logs what it chose. UI thread.
void ApplyFullscreenMode(rex::ui::Window* window, int monitor, const std::string& value);

}  // namespace bf1943
