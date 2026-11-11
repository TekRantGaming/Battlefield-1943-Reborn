// Particles (smoke, fire, dust, exhaust) above 30 FPS (TekRantGaming's play test, 7 Nov 2026: a car's exhaust "shot in front
// of the player" and stretching far; smoke and fire very bright and flickering).
//
// The game's particle manager (the EmitterManager, constructed by sub_82739978, vtable 0x82054584) steps every
// emitter once a frame in sub_82739138(manager, f1 = the frame's time, r5, r6 = the camera), called from the client's
// frame (sub_8224EDE8), with the frame's own time (the byte at manager+322, a fixed 1/30 step, is 0: runs args30,
// args110). The emitters follow the entities they sit on (a car, a tank, a burning wreck), and those move only in the
// game's 30 Hz tick: above 30 FPS an emitter stands still for a frame or more and then jumps a whole tick in one
// frame's time, so whatever the particles take from its motion comes in bursts of two to four times its speed.
//
// bf_effects_tick: the particle manager steps on the frames a game tick ran, with the time since its last step, as at
// 30 FPS (the console's rhythm); the frames between draw the particles where that step left them (the quads still face
// the camera of each frame). Without a manager the client calls sub_825CFEE8(r5, 1) itself; a held frame does the
// same and hands back no job to wait for, as that path does.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(bf_effects_tick, true, "BF1943",
                    "Particles step with the game's 30 Hz tick, as at 30 FPS: no bursts of speed from emitters on moving "
                    "vehicles above 30 FPS");
REXCVAR_DECLARE(bool, bf_unlock_fps);  // frame_rate.cpp
REXCVAR_DECLARE(bool, server);         // server_mode.cpp

namespace bf1943 {
uint64_t ClientTicks();  // vehicle_camera.cpp
bool InMatch();          // auto_join.cpp
#if defined(BF_DEV_TOOLS)
void DevMarkNamed(const char* what);  // tick_blend.cpp: BF_DEV_TIMELINE
#endif
}  // namespace bf1943

namespace {
uint64_t g_last_ticks = ~0ull;  // the client tick count at the manager's last step
double g_pending = 0;           // the frames' time since then
uint64_t g_stepped = 0, g_held = 0;

bool EffectsTick() {
#if defined(BF_DEV_TOOLS)
  // BF_EFFECTS_TICK=0 or 1 overrides the setting (A/B tests); BF_EFFECTS_TICK_FLIP=<s> turns it on and off every s
  // seconds instead, logging each change (both in one drive, for screenshots of the same path).
  static const int dev = [] {
    const char* v = std::getenv("BF_EFFECTS_TICK");
    return v && *v ? (*v == '1' ? 1 : 0) : -1;
  }();
  static const double flip = [] {
    const char* v = std::getenv("BF_EFFECTS_TICK_FLIP");
    return v && *v ? std::atof(v) : 0.0;
  }();
  if (flip > 0) {
    static const auto start = std::chrono::steady_clock::now();
    const bool on = int(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() / flip) % 2;
    static int last = -1;
    if (int(on) != last) {
      last = on;
      REXLOG_INFO("BF effects: tick {}", on ? "on" : "off");
    }
    return on;
  }
  if (dev >= 0) return dev == 1;
#endif
  return REXCVAR_GET(bf_effects_tick);
}
}  // namespace

REX_EXTERN(sub_825CFEE8);  // the client's call without a particle manager
REX_EXTERN(__imp__sub_82739138);
REX_HOOK_RAW(sub_82739138) {
  const bool gate = EffectsTick() && REXCVAR_GET(bf_unlock_fps) && !REXCVAR_GET(server) && bf1943::InMatch();
  if (gate) {
    const uint64_t ticks = bf1943::ClientTicks();
    g_pending += ctx.f1.f64;
    // A held frame (no tick since the last step), unless a hitch left a long time waiting.
    if (ticks == g_last_ticks && g_pending < 0.25) {
      ++g_held;
#if defined(BF_DEV_TOOLS)
      bf1943::DevMarkNamed("fx-held");
      if (std::getenv("BF_DEV_FXLOG")) REXLOG_INFO("BF fx: held (ticks {})", ticks);
#endif
      ctx.r3.u64 = ctx.r5.u64;
      ctx.r4.u64 = 1;
      sub_825CFEE8(ctx, base);
      ctx.r3.u64 = 0;
      return;
    }
    g_last_ticks = ticks;
    ctx.f1.f64 = g_pending;
    g_pending = 0;
  } else {
    g_last_ticks = ~0ull;
    g_pending = 0;
  }
  ++g_stepped;
#if defined(BF_DEV_TOOLS)
  bf1943::DevMarkNamed("fx");
  if (std::getenv("BF_DEV_FXLOG")) REXLOG_INFO("BF fx: stepped {:.4f} s (ticks {})", ctx.f1.f64, bf1943::ClientTicks());
#endif
#if defined(BF_DEV_TOOLS)
  // BF_CALL_RATE=1: every 5 s, the steps and held frames, and the manager's fixed step flag and time scale.
  {
    static const bool rate = [] {
      const char* v = std::getenv("BF_CALL_RATE");
      return v && *v == '1';
    }();
    static auto at = std::chrono::steady_clock::now();
    static uint64_t stepped0 = 0, held0 = 0;
    if (rate && std::chrono::steady_clock::now() - at > std::chrono::seconds(5)) {
      at = std::chrono::steady_clock::now();
      const uint32_t self = ctx.r3.u32;
      uint32_t scale_bits = uint32_t(base[self + 328]) << 24 | base[self + 329] << 16 | base[self + 330] << 8 |
                            base[self + 331];
      float scale;
      std::memcpy(&scale, &scale_bits, 4);
      REXLOG_INFO("BF effects: {} steps, {} held in 5 s (this {:08X} fixed step flag {} step {:.4f} scale {})",
                  g_stepped - stepped0, g_held - held0, self, base[self + 322], ctx.f1.f64, scale);
      stepped0 = g_stepped, held0 = g_held;
    }
  }
#endif
#if defined(BF_DEV_TOOLS)
  // BF_DEV_EMITTER_DUMP=<seconds>,<frames>,<file>: from that time (since the first manager step), the manager and its
  // emitters on that many consecutive frames, as hex words: for finding where an emitter keeps its position and the
  // motion its particles take (run jeep tests).
  {
    static const struct Dump {
      double at = -1;
      int frames = 0;
      std::string file;
    } dump = [] {
      Dump d;
      if (const char* v = std::getenv("BF_DEV_EMITTER_DUMP"); v && *v) {
        std::string t = v;
        const size_t c1 = t.find(','), c2 = t.find(',', c1 + 1);
        if (c1 != std::string::npos && c2 != std::string::npos) {
          d.at = std::atof(t.substr(0, c1).c_str());
          d.frames = std::atoi(t.substr(c1 + 1, c2 - c1 - 1).c_str());
          d.file = t.substr(c2 + 1);
        }
      }
      return d;
    }();
    static const auto start = std::chrono::steady_clock::now();
    static int dumped = 0;
    if (dump.at >= 0 && dumped < dump.frames &&
        std::chrono::steady_clock::now() - start > std::chrono::duration<double>(dump.at)) {
      auto load = [&](uint32_t a) {
        return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
      };
      if (FILE* f = std::fopen(dump.file.c_str(), dumped ? "a" : "w")) {
        const uint32_t m = ctx.r3.u32;
        std::fprintf(f, "frame %d dt %.6f ticks %llu manager %08X\n", dumped, ctx.f1.f64,
                     static_cast<unsigned long long>(bf1943::ClientTicks()), m);
        for (uint32_t list : {0x60u, 0x70u, 0x80u, 0x90u, 0xA0u, 0xB0u}) {
          const uint32_t b = load(m + list), e = load(m + list + 4);
          if (!b || e < b || e - b > 4 * 512) continue;
          std::fprintf(f, " list +%02X: %u\n", list, (e - b) / 4);
          for (uint32_t i = 0; i < (e - b) / 4 && i < 24; ++i) {
            const uint32_t em = load(b + 4 * i);
            if (em < 0x40000000) continue;
            std::fprintf(f, "  emitter %08X:", em);
            for (uint32_t k = 0; k < 0x1C0; k += 4) std::fprintf(f, " %08X", load(em + k));
            std::fprintf(f, "\n");
          }
        }
        std::fclose(f);
      }
      ++dumped;
    }
  }
#endif
  __imp__sub_82739138(ctx, base);
}
