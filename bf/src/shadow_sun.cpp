// The game's shadows from the sky's sun at their source (D239). With the day and night cycle, NativeXRenderer used to
// turn the game's shadow cascades to the sky's sun after the game had built them (D181): the game still chose what
// casts shadows, and fitted its cascades, for its own fixed sun, so as the camera moved buildings' shadows came and
// went (the user, 24 Nov 2026: "You see those shadows on building flicker in and out though, right!?").
//
// The game's shadow setup, sub_825E1DB0 (0x825E1DB0 to 0x825E2830, called once a frame), hands sub_82620D20 (r3 the
// rotation it builds, r4 a vector) its shadow ray, from the sun to the ground: on Wake Island 0.4454 -0.8945 -0.0391,
// 63.4 degrees up (research 24 Nov 2026: the game's light direction found in its memory, the cascades' views it is
// copied into, the copy's source watched, runs sun_find1, sun_early1 to 3, sun_view1, sun_src1). Written over before
// the call, the game builds its cascades, and culls what casts shadows, for that ray (run sun_src2: the pole's shadow on
// the airfield gone with the ray turned by 90 degrees). Here the ray is the one NativeXRenderer derives for the sky's sun
// (ShadowSourceDirection: the turn's own rule, D181); it then turns nothing. Not on a dedicated server (no renderer).

#include <cstdint>
#include <cstring>

#include <native_x_renderer/native_x_renderer.h>
#include <rex/hook.h>
#include <rex/logging.h>

namespace {
float LoadFloatBe(const uint8_t* p) {
  const uint32_t bits = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}
void StoreFloatBe(uint8_t* p, float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  p[0] = uint8_t(bits >> 24), p[1] = uint8_t(bits >> 16), p[2] = uint8_t(bits >> 8), p[3] = uint8_t(bits);
}
}  // namespace

REX_EXTERN(__imp__sub_82620D20);
REX_HOOK_RAW(sub_82620D20) {
  const uint32_t caller = uint32_t(ctx.lr);
  if (caller >= 0x825E1DB0 && caller < 0x825E2830 && ctx.r4.u32) {
    uint8_t* v = base + ctx.r4.u32;
    const float game_ray[3] = {LoadFloatBe(v), LoadFloatBe(v + 4), LoadFloatBe(v + 8)};
    float ray[3];
    if (native_x_renderer::ShadowSourceDirection(game_ray, ray)) {
      for (int i = 0; i < 3; ++i) StoreFloatBe(v + 4 * i, ray[i]);
      static bool said = false;
      if (!said) {
        said = true;
        REXLOG_INFO("BF shadows: the game's shadow ray {:.4f} {:.4f} {:.4f} given the sky's sun's {:.4f} {:.4f} {:.4f} (D239)",
                    game_ray[0], game_ray[1], game_ray[2], ray[0], ray[1], ray[2]);
      }
    }
  }
  __imp__sub_82620D20(ctx, base);
}
