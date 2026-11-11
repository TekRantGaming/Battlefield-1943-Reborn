/**
 * @file        ui/display_mode.h
 * @brief       Fullscreen display modes (exclusive fullscreen at a chosen resolution)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <vector>

namespace rex::ui {

class Window;

// A display mode, in pixels.
struct DisplayMode {
  int width = 0;
  int height = 0;
  float refresh_rate = 0.0f;  // Hz (0: unknown)
};

// A display: its name, its mode on the desktop and whether it's the primary one.
struct DisplayInfo {
  std::string name;
  DisplayMode desktop;
  bool primary = false;
};

// Every display, in the order the monitor cvar counts them (monitor = index + 1). Call on the UI thread.
std::vector<DisplayInfo> GetDisplays();

// The fullscreen display modes of a display, largest first (then the highest refresh rate), each
// size and rate once. `monitor` is 1-based in the order the monitor cvar uses; 0 is the display
// `window` is on (the primary display without a window). Call on the UI thread.
std::vector<DisplayMode> GetFullscreenDisplayModes(int monitor, const Window* window);

// The display's mode on the desktop (width 0 when unknown). `monitor` as for GetFullscreenDisplayModes.
// Call on the UI thread.
DisplayMode GetDesktopDisplayMode(int monitor, const Window* window);

// Sets the mode `window` uses whenever it is fullscreen. With a size, the closest mode the display
// has to width x height at refresh_rate (0: any), as exclusive fullscreen; with width 0, borderless
// at the desktop's resolution (the default). Applies at once when the window is fullscreen. Returns
// false when the mode could not be set (the window then stays borderless); `chosen` gets the mode
// that was set. `monitor` as for GetFullscreenDisplayModes. Call on the UI thread.
bool SetFullscreenDisplayMode(Window* window, int monitor, int width, int height, float refresh_rate,
                              DisplayMode* chosen = nullptr);

}  // namespace rex::ui
