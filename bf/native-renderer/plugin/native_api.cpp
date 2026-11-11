// The plugin's side of BfNativeApi (native_api.h). M1: counts each library call per frame, the engine's own calls
// apart from those the library makes inside itself, and with BF_NATIVE_COUNT=<frames> logs that many frames from
// BF_NATIVE_COUNT_AT=<seconds> after the first frame on (default 90; REX_DEV_FRAME_LOG takes seconds after launch,
// so the two logs meet in the same frames) (docs/native-renderer.md, "M1").

#include "plugin/native_api.h"

#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

namespace {

std::mutex g_mutex;
std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_counts;  // this frame: address: engine calls, nested calls
uint64_t g_frames = 0;

int EnvInt(const char* name, int fallback) {
  const char* v = std::getenv(name);
  return v && *v ? std::atoi(v) : fallback;
}

void OnCall(uint32_t address, uint32_t depth, uint32_t, uint32_t, uint32_t, uint32_t) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto& count = g_counts[address];
  ++(depth ? count.second : count.first);
}

void OnFrameEnd() {
  static const int frames_to_log = EnvInt("BF_NATIVE_COUNT", 0);
  static const int log_at = EnvInt("BF_NATIVE_COUNT_AT", 90);
  static const auto first = std::chrono::steady_clock::now();
  static int logged = 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_frames;
  if (logged < frames_to_log && std::chrono::steady_clock::now() - first >= std::chrono::seconds(log_at)) {
    // The library's draw packet builders (tools/d3d_entry_points.py, DRAW_BUILDERS): their calls are the frame's
    // draws, to compare with REX_DEV_FRAME_LOG's count.
    static constexpr uint32_t kDrawBuilders[] = {0x8216B718, 0x8217F388, 0x8217F868, 0x8217FC58,
                                                 0x8216AB88, 0x8216D448, 0x8216DCC0, 0x821765B8,
                                                 0x82176710, 0x8217ABD0, 0x8217E0D0};
    std::string text, draws_text;
    uint32_t engine = 0, nested = 0, draws = 0;
    for (const auto& [address, count] : g_counts) {
      text += count.second ? fmt::format(" {:08X}:{}+{}", address, count.first, count.second)
                           : fmt::format(" {:08X}:{}", address, count.first);
      engine += count.first;
      nested += count.second;
      for (uint32_t builder : kDrawBuilders) {
        if (address == builder) {
          draws += count.first + count.second;
          draws_text += fmt::format(" {:08X}:{}+{}", address, count.first, count.second);
        }
      }
    }
    REXLOG_INFO("bfnative: frame {}: {} draws ({}), {} engine calls, {} nested, {} entry points:{}", g_frames, draws,
                draws_text, engine, nested, g_counts.size(), text);
    ++logged;
  }
  g_counts.clear();
}

const BfNativeApi kApi = {BF_NATIVE_API_VERSION, sizeof(BfNativeApi), OnCall, OnFrameEnd};

}  // namespace

extern "C" REX_GPU_PLUGIN_EXPORT const BfNativeApi* bf_native_get_api() { return &kApi; }
