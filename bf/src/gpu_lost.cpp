// When the graphics driver resets the GPU (TekRantGaming, 21 Nov 2026: "it froze my game when i loaded into the
// server"; Windows logged NVIDIA's nvlddmkm event 153 as Wake Island's first frame was drawn). The renderer's device is
// lost then: nothing new is drawn, but the game runs on, so the player saw a frozen picture and had to end the game in
// the Task Manager. Now the renderer notices (native_x_renderer::DeviceLost, renderer.cpp WatchDeviceLoss) and this
// tells the player in a message box of the system's own, which needs no GPU, then closes the game as Quit does.
//
// Test runs (BF_DEV_SCRIPTED=1, tools\run_game.ps1) only log and close: no box on the PC's screen.
// NXR_TEST_DEVICE_LOST=<s> makes the renderer take its device as lost that many seconds after its first frame.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

#include <rex/logging.h>

#include <native_x_renderer/native_x_renderer.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <SDL3/SDL.h>
#endif

namespace bf1943 {
namespace {

constexpr const char* kTitle = "Battlefield 1943 Reborn: graphics driver reset";
constexpr const char* kText =
    "Your graphics driver stopped responding and reset the graphics card, so the game can no longer draw anything "
    "(the picture froze).\n\n"
    "Battlefield 1943 Reborn closes now. Start it again to keep playing.\n\n"
    "If this keeps happening, update or reinstall your graphics driver.";

bool Scripted() {
  const char* v = std::getenv("BF_DEV_SCRIPTED");
  return v && *v == '1';
}

}  // namespace

// Started once by the player's game (bf1943_app.h OnCreateDialogs). `in_ui` runs a job on the UI thread; `close` closes
// the game as the window's close button does.
void WatchGpuDeviceLoss(std::function<void(std::function<void()>)> in_ui, std::function<void()> close) {
  static std::atomic<bool> started{false};
  if (started.exchange(true)) return;
  std::thread([in_ui = std::move(in_ui), close = std::move(close)] {
    while (!native_x_renderer::DeviceLost()) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const bool box = !Scripted();
    REXLOG_ERROR("BF GPU: the graphics device was lost (the driver reset the GPU): {}the game closes",
                 box ? "the player told, " : "a scripted run: no message box, ");
    in_ui([box, close] {
      if (box) {
#if defined(_WIN32)
        MessageBoxA(nullptr, kText, kTitle, MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
#else
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kTitle, kText, nullptr);
#endif
      }
      REXLOG_INFO("BF GPU: closing after the lost device");
      close();
    });
  }).detach();
}

}  // namespace bf1943
