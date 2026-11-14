/**
 * @file        ui/windowed_app_context_sdl.h
 * @brief       SDL3 implementation of the windowed app UI loop context
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <SDL3/SDL_events.h>

#include <rex/ui/windowed_app_context.h>

namespace rex::ui {

class WindowSDL;

class SDLWindowedAppContext final : public WindowedAppContext {
 public:
  SDLWindowedAppContext() = default;
  ~SDLWindowedAppContext() override;

  // Initializes the SDL video subsystem and registers the custom event types.
  // Must be called (and must succeed) before any other use.
  bool Initialize();

  void NotifyUILoopOfPendingFunctions() override;
  void PlatformQuitFromUIThread() override;

  int RunMainMessageLoop();

  // Custom SDL event type carrying a coalesced paint request for a window
  // (user.windowID identifies the target).
  uint32_t paint_event_type() const { return paint_event_type_; }

  // Called with each mouse movement (SDL window coordinates) on the thread that
  // sends it, ahead of the UI loop: on Windows that is SDL's raw input thread
  // while the pointer is locked. A game reading the mouse every frame then does
  // not depend on when the UI thread, which also paints, reads its events.
  using MouseMotionWatch = void (*)(float dx, float dy);
  static void SetMouseMotionWatch(MouseMotionWatch watch);

  // Window registry for event routing. UI thread only.
  void RegisterWindow(SDL_WindowID id, WindowSDL* window) { windows_.emplace(id, window); }
  void UnregisterWindow(SDL_WindowID id) { windows_.erase(id); }

 private:
  WindowSDL* GetWindow(SDL_WindowID id) const {
    auto it = windows_.find(id);
    return it != windows_.end() ? it->second : nullptr;
  }

  void ProcessEvent(SDL_Event& event);
  void ProcessQuitRequest();
  static bool SDLCALL WatchEvent(void* userdata, SDL_Event* event);

  std::unordered_map<SDL_WindowID, WindowSDL*> windows_;
  uint32_t wakeup_event_type_ = 0;
  // A video driver with no blocking event wait (offscreen, dummy: a headless server): SDL_WaitEvent would poll every
  // 1 ms, so the loop waits here instead, woken by queued work, and looks at SDL's events at least every 100 ms.
  std::atomic<bool> headless_{false};
  std::mutex headless_mutex_;
  std::condition_variable headless_cv_;
  bool headless_wake_ = false;
  uint32_t paint_event_type_ = 0;
  uint32_t synchronously_handled_quit_events_ = 0;
  bool event_watch_registered_ = false;
};

}  // namespace rex::ui
