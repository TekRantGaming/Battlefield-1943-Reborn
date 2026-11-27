// Battlefield 1943 Reborn, SDK v0.11+ port (phase 5): NativeXRenderer's IGraphicsSystem on the bfnative driver's hooks
// (compat/rex/system/interfaces/graphics.h), and the player's picture settings on the display gamma ramp.
#include <rex/system/interfaces/graphics.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

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

// The player's picture adjustment (Settings > Display > Picture), as King Kong's (picture_adjust.h). v0.10 defined
// these in our SDK copy's command processor.
REXCVAR_DEFINE_DOUBLE(bf_brightness, 0.0, "GPU", "Brightness of the game's picture, -50 to 50 (0: the game's own)");
REXCVAR_DEFINE_DOUBLE(bf_gamma, 1.0, "GPU", "Gamma of the game's picture, 0.5 to 1.5 (1: the game's own)");

namespace rex::system {

namespace {

// out = pow(clamp((x - lo) / (1 - lo), 0, 1), 1 / g), lo = -b * 0.2: brightness moves the black level, gamma the
// middle tones; white stays white.
float PictureCurve(float x, float brightness, float gamma) {
  const float lo = -brightness * 0.2f;
  float y = std::clamp((x - lo) / (1.0f - lo), 0.0f, 1.0f);
  if (gamma != 1.0f) y = std::pow(y, 1.0f / gamma);
  return y;
}

const ::rex::gpu::bfnative::Hooks* FindHooks() {
  using ::rex::gpu::bfnative::GetHooksFn;
  GetHooksFn get = nullptr;
#if defined(_WIN32)
  // The build configuration's suffix (rd: RelWithDebInfo, d: Debug), as the SDK names its modules.
  for (const char* name : {"rexgpu-xenos-bfnative.dll", "rexgpu-xenos-bfnativerd.dll", "rexgpu-xenos-bfnatived.dll"}) {
    if (HMODULE module = GetModuleHandleA(name)) {
      get = reinterpret_cast<GetHooksFn>(GetProcAddress(module, ::rex::gpu::bfnative::kBfNativeHooksSymbol));
      if (get) break;
    }
  }
#else
  // The SDK loads its driver modules privately (RTLD_LOCAL): RTLD_DEFAULT does not see their symbols (the port's first
  // Linux client, 25 Nov 2026: nothing drew). The module already loaded, by its name in each build.
  for (const char* name : {"librexgpu-xenos-bfnative.so", "librexgpu-xenos-bfnativerd.so", "librexgpu-xenos-bfnatived.so"}) {
    if (void* module = dlopen(name, RTLD_NOW | RTLD_NOLOAD)) {
      get = reinterpret_cast<GetHooksFn>(dlsym(module, ::rex::gpu::bfnative::kBfNativeHooksSymbol));
      dlclose(module);  // the SDK's own reference keeps it loaded
      if (get) break;
    }
  }
  if (!get) get = reinterpret_cast<GetHooksFn>(dlsym(RTLD_DEFAULT, ::rex::gpu::bfnative::kBfNativeHooksSymbol));
#endif
  const ::rex::gpu::bfnative::Hooks* hooks = get ? get() : nullptr;
  if (hooks && (hooks->version != ::rex::gpu::bfnative::kHooksVersion ||
                hooks->size < sizeof(::rex::gpu::bfnative::Hooks))) {
    REXLOG_ERROR("BF renderer: the bfnative driver's hooks are version {} (size {}), the game's {}: not used",
                 hooks->version, hooks->size, ::rex::gpu::bfnative::kHooksVersion);
    return nullptr;
  }
  return hooks;
}

}  // namespace

bool IGraphicsSystem::GetGammaRamp(GammaRamp& ramp) {
  if (!hooks_->GetGammaRamp(&ramp)) return false;
  const float b = float(std::clamp(REXCVAR_GET(bf_brightness), -50.0, 50.0) / 50.0);
  const float g = float(std::clamp(REXCVAR_GET(bf_gamma), 0.5, 1.5));
  // The version changes with the settings too, so the renderer uploads the ramp again.
  static std::mutex mutex;
  static float last_b = 0.0f, last_g = 1.0f;
  static uint64_t settings_version = 0;
  {
    std::lock_guard lock(mutex);
    if (b != last_b || g != last_g) last_b = b, last_g = g, ++settings_version;
    ramp.version += settings_version << 32;
  }
  if (std::fabs(b) < 0.001f && std::fabs(g - 1.0f) < 0.001f) return true;
  for (uint32_t i = 0; i < 256; ++i) {
    // DC_LUT_30_COLOR: blue in bits 0:9, green in 10:19, red in 20:29.
    uint32_t c = ramp.table[i], out = c & ~0x3FFFFFFFu;
    for (uint32_t shift : {0u, 10u, 20u}) {
      const uint32_t v = (c >> shift) & 0x3FF;
      out |= (uint32_t(std::lround(PictureCurve(v / 1023.0f, b, g) * 1023.0f)) & 0x3FF) << shift;
    }
    ramp.table[i] = out;
  }
  for (uint32_t i = 0; i < 128; ++i) {
    for (uint32_t j = 0; j < 3; ++j) {
      // DC_LUT_PWL_DATA: base in bits 0:15, delta in 16:31; bits 0:5 of both hardwired to zero.
      const uint32_t s = ramp.pwl[i][j], base = s & 0xFFFF, delta = s >> 16;
      const float v0 = base / 65535.0f, v1 = std::min(1.0f, (base + delta) / 65535.0f);
      const uint32_t b0 = uint32_t(std::lround(PictureCurve(v0, b, g) * 65535.0f)) & 0xFFC0;
      const uint32_t b1 = uint32_t(std::lround(PictureCurve(v1, b, g) * 65535.0f)) & 0xFFC0;
      ramp.pwl[i][j] = b0 | ((b1 > b0 ? (b1 - b0) & 0xFFC0 : 0) << 16);
    }
  }
  return true;
}

IGraphicsSystem* bfnative_graphics() {
  // Looked up until the driver's module is there (it loads with the systems, after the program starts).
  static std::atomic<const ::rex::gpu::bfnative::Hooks*> found{nullptr};
  const ::rex::gpu::bfnative::Hooks* hooks = found.load(std::memory_order_acquire);
  if (!hooks) {
    hooks = FindHooks();
    if (!hooks) return nullptr;
    found.store(hooks, std::memory_order_release);
  }
  if (!hooks->IsActive()) return nullptr;
  static IGraphicsSystem graphics(*hooks);
  return &graphics;
}

}  // namespace rex::system
