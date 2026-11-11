// Settings > Audio, King Kong's page: Master volume scales every audio frame the game hands the system
// (XAudioSubmitRenderDriverFrame: 256 samples on 6 channels, big endian floats), so it applies at once to all
// game audio. Music, effects and voices need the game's own sound groups found first (the menu shows them greyed).
// XAudioGetVoiceCategoryVolume, the system's volume per voice category, is logged once per category, to see
// whether the game asks for one (a way to its music volume).

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

REXCVAR_DEFINE_DOUBLE(bf_master_volume, 100.0, "BF1943/Audio", "How loud all game audio is, 0 to 100 (%)");

namespace {

PPCFunc* RuntimeExport(const char* name) {
#if defined(_WIN32)
  static HMODULE runtime = GetModuleHandleA("rexruntime.dll");
  return reinterpret_cast<PPCFunc*>(GetProcAddress(runtime, name));
#else
  return reinterpret_cast<PPCFunc*>(dlsym(RTLD_NEXT, name));
#endif
}

constexpr uint32_t kFrameSamples = 256 * 6;

}  // namespace

namespace bf1943 {
// Our menus outside a match (game_menu.cpp): the game's own sound is silent there (its start's logo sound and title
// music behind our Getting ready page, its hidden menu's music; TekRantGaming, 10 Nov: they played over ours). The mute's
// gain moves a step a frame, about 0.3 s from one end to the other, so it never clicks.
std::atomic<bool> g_game_audio_muted{false};
void SetGameAudioMuted(bool muted) { g_game_audio_muted.store(muted, std::memory_order_relaxed); }
}  // namespace bf1943

// XAudioSubmitRenderDriverFrame(driver, samples): each sample scaled by the master volume before the system has it.
extern "C" REX_FUNC(__imp__XAudioSubmitRenderDriverFrame) {
  static PPCFunc* original = RuntimeExport("__imp__XAudioSubmitRenderDriverFrame");
  static const bool off = [] {
    const char* v = std::getenv("BF_AB_OFF");
    return v && std::string(v).find("audio") != std::string::npos;
  }();
  if (off) {
    if (original) original(ctx, base);
    return;
  }
  const double volume = std::clamp(REXCVAR_GET(bf_master_volume), 0.0, 100.0) / 100.0;
  // The menus' mute, ramped across this frame from the last frame's gain to this one's (256 frames of 6 channels,
  // about 5.3 ms at 48 kHz; 56 of them from silent to full).
  static float gain = 1.0f;
  const float from = gain, target = bf1943::g_game_audio_muted.load(std::memory_order_relaxed) ? 0.0f : 1.0f;
  if (gain < target) gain = std::min(target, gain + 1.0f / 56.0f);
  else if (gain > target) gain = std::max(target, gain - 1.0f / 56.0f);
  if (ctx.r4.u32 && (volume < 0.999 || from < 0.999f || gain < 0.999f)) {
    uint8_t* p = base + ctx.r4.u32;
    for (uint32_t i = 0; i < kFrameSamples; ++i, p += 4) {
      uint32_t bits = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
      float v;
      std::memcpy(&v, &bits, 4);
      const float g = from + (gain - from) * (float(i / 6) / 256.0f);
      v = float(v * volume * g);
      std::memcpy(&bits, &v, 4);
      p[0] = uint8_t(bits >> 24), p[1] = uint8_t(bits >> 16), p[2] = uint8_t(bits >> 8), p[3] = uint8_t(bits);
    }
  }
#if defined(BF_DEV_TOOLS)
  // BF_DEV_AUDIO_LEVEL=1: the frames' RMS level, logged every 5 s (tests of the volume rows without listening).
  static const bool level = std::getenv("BF_DEV_AUDIO_LEVEL") != nullptr;
  if (level && ctx.r4.u32) {
    static double sum = 0;
    static uint64_t n = 0;
    static auto last = std::chrono::steady_clock::now();
    const uint8_t* q = base + ctx.r4.u32;
    for (uint32_t i = 0; i < kFrameSamples; ++i, q += 4) {
      uint32_t bits = uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | q[3];
      float v;
      std::memcpy(&v, &bits, 4);
      sum += double(v) * v, ++n;
    }
    if (std::chrono::steady_clock::now() - last > std::chrono::seconds(5)) {
      REXLOG_INFO("BF audio level: RMS {:.5f} over {} samples", n ? std::sqrt(sum / double(n)) : 0.0, n);
      sum = 0, n = 0, last = std::chrono::steady_clock::now();
    }
  }
#endif
  if (original) original(ctx, base);
}

// XAudioGetVoiceCategoryVolume(category, &volume): which categories the game asks about (once each).
extern "C" REX_FUNC(__imp__XAudioGetVoiceCategoryVolume) {
  static PPCFunc* original = RuntimeExport("__imp__XAudioGetVoiceCategoryVolume");
  static std::atomic<uint32_t> seen{0};
  const uint32_t category = ctx.r3.u32;
  if (category < 32 && !(seen.fetch_or(1u << category) & (1u << category)))
    REXLOG_INFO("BF audio: the game asks for voice category {}'s volume (lr {:08X})", category, uint32_t(ctx.lr));
  if (original) original(ctx, base);
}
