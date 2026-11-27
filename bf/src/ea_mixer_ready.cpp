// Tells the SDK's audio worker when EA's mixer has its next packet ready (rex::audio::SetRenderReadyCheck), so a
// render callback waits a moment for a late mixer instead of playing a silent frame (audio_render_wait_ms).
//
// EA's mixer thread ("RWAudioCore Dac", loop sub_82190470) mixes one 256-sample tick at a time (sub_82190390, its
// object in r3) into one of two 6 KB slots and queues it on a single XAudio voice: two packets, 10.7 ms of audio.
// Slot states at object+0x3100 + i*4: 0 free (XAudio is done with it), 1 mixed, 2 queued in XAudio. When both are
// free, the voice has nothing left and the guest's XAudio renders a frame of silence. On the console the thread has a
// hardware thread of its own; here, when many sounds start at once (explosions, a busy fight), its update before a
// tick takes 5-20 ms, the queue runs dry, and the mix gets 5 ms holes (counted in combat: 1.7-3.5 a minute; with this
// and a 12 ms wait: 0-0.4).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/audio/audio_system.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>

namespace {

std::atomic<uint32_t> g_mixer{0};         // EA's mixer object (guest address)
std::atomic<uint8_t*> g_base{nullptr};    // guest memory
std::atomic<int64_t> g_last_tick_us{0};   // when the mixer last began a tick

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint32_t Load32(const uint8_t* base, uint32_t at) {
  uint32_t v;
  std::memcpy(&v, base + at, 4);
  return __builtin_bswap32(v);
}

// Ready when a packet is queued in XAudio. A mixer that has not ticked for 100 ms (not started, paused, shut down)
// counts as ready, so nothing ever waits on it.
bool EaMixerReady() {
  const uint32_t mixer = g_mixer.load(std::memory_order_relaxed);
  const uint8_t* base = g_base.load(std::memory_order_relaxed);
  if (!mixer || !base) return true;
  if (NowUs() - g_last_tick_us.load(std::memory_order_relaxed) > 100000) return true;
  return Load32(base, mixer + 0x3100) == 2 || Load32(base, mixer + 0x3104) == 2;
}

const bool g_registered = [] {
  rex::audio::SetRenderReadyCheck(&EaMixerReady);
  return true;
}();

}  // namespace

// EA's mix tick (r3 = the mixer object): note the object and when it ticked.
REX_EXTERN(__imp__sub_82190390);
REX_HOOK_RAW(sub_82190390) {
  g_mixer.store(ctx.r3.u32, std::memory_order_relaxed);
  g_base.store(base, std::memory_order_relaxed);
  g_last_tick_us.store(NowUs(), std::memory_order_relaxed);
  __imp__sub_82190390(ctx, base);
}
