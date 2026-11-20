// NativeXRenderer - the sun's soft shadows (D197): the numbers the translated shaders' soft filter takes
// (SpirvShaderTranslator::SetSoftShadowFetchConstant, SystemConstants::soft_shadow), from the game's cascades.

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "draw.h"

// The user, 18 Nov 2026: "Is there a way we can also drastically improve shadow and lighting quality?"
REXCVAR_DEFINE_STRING(native_x_renderer_soft_shadows, "off", "NativeXRenderer",
                      "The sun's shadows (D197): off (the game's own edge, one texel of its shadow map wide), soft (a "
                      "penumbra that widens with the distance to what casts it) or softer; taken at once. Off by default "
                      "(TekRantGaming, 20 Nov 2026)");

namespace native_x_renderer {

namespace {
// The light's penumbra per metre between a blocker and the receiver: the sun's disc, 0.53 degrees across (0.0093), and
// "softer" twice that (a hazier sun). At 0.012 a branch 6 m up all but lost its shadow (run sa1_g_dusk).
constexpr float kSoftPenumbra = 0.0093f, kSofterPenumbra = 0.0186f;
// The blocker search reaches blockers this far above the receiver (metres); the slope bias is that of a receiver
// 63 degrees from facing the light (tangent 2); the radius is kept between these numbers of texels.
constexpr float kFarthest = 40.0f, kSlope = 2.0f, kSmallestTexels = 1.5f, kLargestTexels = 24.0f;
}  // namespace

// Once a frame, when the cascades' transforms are taken (shadow_map_.cascades: x and y within -0.5 to 0.5 inside a
// cascade, z the depth compared): per cascade, depth units per uv unit and per metre, from the rows' lengths (the
// cascades are orthographic: a row's length is its units per metre).
void DrawPath::UpdateSoftShadows() {
  std::memset(soft_shadow_, 0, sizeof(soft_shadow_));
  if (soft_shadow_fetch_constant_ == UINT32_MAX) return;
  const std::string& mode = REXCVAR_GET(native_x_renderer_soft_shadows);
  float penumbra = mode == "soft" ? kSoftPenumbra : mode == "softer" ? kSofterPenumbra : 0.0f;
  float farthest = kFarthest, slope = kSlope, smallest = kSmallestTexels, largest = kLargestTexels;
  // NXR_SOFT_SHADOW=<penumbra per metre>,<farthest m>,<slope>,<smallest>,<largest texels> (a developer's tuning).
  static const char* tuning = std::getenv("NXR_SOFT_SHADOW");
  if (penumbra > 0.0f && tuning && *tuning) {
    float p = penumbra, f = farthest, s = slope, a = smallest, b = largest;
    if (std::sscanf(tuning, "%f,%f,%f,%f,%f", &p, &f, &s, &a, &b) >= 1) {
      penumbra = p, farthest = f, slope = s, smallest = a, largest = b;
    }
  }
  // NXR_SOFT_SHADOW_AB=<seconds> (a developer's comparison): off and on by turns, each that long by the wall clock,
  // each change logged, so screenshots of one run show the same view both ways.
  static const int ab = [] {
    const char* v = std::getenv("NXR_SOFT_SHADOW_AB");
    return v && *v ? std::max(1, std::atoi(v)) : 0;
  }();
  if (ab && penumbra > 0.0f) {
    static int last = -1;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const int phase = int((now / ab) & 1);
    if (phase != last) REXLOG_INFO("[native_x_renderer] soft shadows A/B: {}", phase ? "on" : "off");
    last = phase;
    if (!phase) penumbra = 0.0f;
  }
  if (!(penumbra > 0.0f)) return;
  float depth_per_uv[3], depth_per_metre[3];
  for (int i = 0; i < 3; ++i) {
    const float (*rows)[4] = shadow_map_.cascades[i];
    const float uv = std::sqrt(rows[0][0] * rows[0][0] + rows[0][1] * rows[0][1] + rows[0][2] * rows[0][2]);
    const float depth = std::sqrt(rows[2][0] * rows[2][0] + rows[2][1] * rows[2][1] + rows[2][2] * rows[2][2]);
    if (!(uv > 1e-7f) || !(depth > 1e-9f) || !std::isfinite(uv + depth)) return;  // no cascades yet: the game's
    depth_per_uv[i] = depth / uv, depth_per_metre[i] = depth;
  }
  for (int i = 0; i < 3; ++i) soft_shadow_[0][i] = depth_per_uv[i], soft_shadow_[1][i] = depth_per_metre[i];
  soft_shadow_[0][3] = 1.0f;
  soft_shadow_[1][3] = penumbra;
  soft_shadow_[2][0] = farthest, soft_shadow_[2][1] = slope, soft_shadow_[2][2] = smallest, soft_shadow_[2][3] = largest;
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("[native_x_renderer] soft shadows (D197): cascades {:.1f}, {:.1f}, {:.1f} m across and {:.1f}, {:.1f}, "
                "{:.1f} m deep; penumbra {:.4f} per metre, blockers searched to {:.0f} m, slope {:.2f}, radius {:.1f} to "
                "{:.1f} texels",
                1.0f / (depth_per_metre[0] / depth_per_uv[0]), 1.0f / (depth_per_metre[1] / depth_per_uv[1]),
                1.0f / (depth_per_metre[2] / depth_per_uv[2]), 1.0f / depth_per_metre[0], 1.0f / depth_per_metre[1],
                1.0f / depth_per_metre[2], penumbra, farthest, slope, smallest, largest);
  }
}

}  // namespace native_x_renderer
