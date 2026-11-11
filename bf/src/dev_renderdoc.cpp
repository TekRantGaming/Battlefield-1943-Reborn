// Developer aid (BF_DEV_TOOLS builds only): BF_RENDERDOC_AT=<seconds> triggers one RenderDoc frame capture that many
// seconds after the first frame, when the game runs under RenderDoc (tools\run_game.ps1 with BF_RENDERDOC set to
// renderdoccmd.exe, which injects it). BF_RENDERDOC_FRAMES=<n> captures n frames in a row instead, one file each
// (TriggerMultiFrameCapture, API 1.1.0), to compare consecutive frames (flicker). Uses RenderDoc's in-application API
// (thirdparty/renderdoc/renderdoc_app.h, MIT), found through the injected renderdoc.dll.

#if defined(BF_DEV_TOOLS) && defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdlib>

#include <rex/logging.h>

#include "renderdoc_app.h"

namespace bf1943 {

namespace {
// API 1.1.0 appends TriggerMultiFrameCapture to 1.0.1's table (our header has 1.0.1 only).
typedef void(RENDERDOC_CC* pTriggerMultiFrameCapture)(uint32_t numFrames);
struct Api110 {
  RENDERDOC_API_1_0_1 base;
  pTriggerMultiFrameCapture TriggerMultiFrameCapture;
};
constexpr int kApiVersion110 = 10100;
}  // namespace

// A capture now (BF_RENDERDOC_FRAMES frames, else one): BF_RENDERDOC_AT, or the match script's "rdoc" (auto_join.cpp).
void TriggerRenderDocCapture() {
  HMODULE module = GetModuleHandleA("renderdoc.dll");
  if (!module) {
    REXLOG_WARN("BF RenderDoc: not running under RenderDoc, no capture");
    return;
  }
  auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
  const char* frames_env = std::getenv("BF_RENDERDOC_FRAMES");
  const int frames = frames_env && *frames_env ? std::atoi(frames_env) : 1;
  if (frames > 1 && get_api) {
    Api110* api = nullptr;
    if (get_api(RENDERDOC_Version(kApiVersion110), reinterpret_cast<void**>(&api)) && api) {
      api->TriggerMultiFrameCapture(uint32_t(frames));
      REXLOG_INFO("BF RenderDoc: {} frame captures triggered", frames);
      return;
    }
  }
  RENDERDOC_API_1_0_1* api = nullptr;
  if (!get_api || !get_api(eRENDERDOC_API_Version_1_0_1, reinterpret_cast<void**>(&api)) || !api) {
    REXLOG_WARN("BF RenderDoc: no API");
    return;
  }
  api->TriggerCapture();
  REXLOG_INFO("BF RenderDoc: frame capture triggered");
}

void RenderDocFrame() {
  static const double at = [] {
    const char* v = std::getenv("BF_RENDERDOC_AT");
    return v && *v ? std::atof(v) : -1.0;
  }();
  if (at < 0) return;
  static const auto first = std::chrono::steady_clock::now();
  static bool done = false;
  if (done || std::chrono::duration<double>(std::chrono::steady_clock::now() - first).count() < at) return;
  done = true;
  TriggerRenderDocCapture();
}

}  // namespace bf1943

#endif  // BF_DEV_TOOLS && _WIN32
