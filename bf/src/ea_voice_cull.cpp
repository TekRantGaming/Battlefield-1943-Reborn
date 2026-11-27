// EA's mixer stops playing sounds when it thinks the CPU is overloaded; here that happens on any delay, so sounds cut
// out in busy moments. bf_ea_cpu_cull (default off) keeps EA's check; off, no sound is stopped for CPU load.
//
// Before each 256-sample mix tick (5.33 ms), sub_82183548 averages the time EA's mixer thread spent in its last three
// control steps and mix ticks (measured with the timebase, i.e. wall clock), adds each voice node's estimated cost, and
// when that exceeds the CPU budget (a percentage of a tick, mixer core +0xCC, checked only below 100 %) calls
// sub_82183680. That stops the lowest-priority voice nodes (priority below 100; node state +0x47 = 2) one by one until
// the excess is covered. The game's sound manager then sees the voice stopped and starts the sound again from the
// beginning about 45 ms later (SoundManager_UpdateVoiceLimiting sets the sound's restart bit).
//
// On the console the thread had a hardware thread of its own and the time was its own work. Here the same clock also
// counts waits that are not EA's work: being preempted, waiting for a lock, XMA decoding that the console's hardware
// did beside the CPU. A single 16 ms control step (one slow step in a busy fight is enough) puts the three-tick average
// over the budget, and a playing sound is stopped. Measured with a test stall of EA's control step every 2 s in the
// menu: 12 ms never stops a voice, 16 ms stops one every time (40-50 ms of silence, then the sound restarts). EA's mix
// itself takes about 4 % of one core on a PC, so there is no load worth stopping sounds for.

#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "diagnostics.h"

REXCVAR_DEFINE_BOOL(bf_ea_cpu_cull, false, "BF1943/Audio",
                    "Let EA's mixer stop sounds when its thread looks overloaded (the console's behaviour). Off: "
                    "a slow moment no longer cuts sounds out");

// sub_82183680(f1 = the excess load): stops voice nodes until it is covered.
REX_EXTERN(__imp__sub_82183680);
REX_HOOK_RAW(sub_82183680) {
  static std::atomic<uint64_t> times{0};
  const uint64_t n = ++times;
  const bool cull = REXCVAR_GET(bf_ea_cpu_cull);
  bf1943::diagnostics::NoteVoiceCulled();  // the black box's frames, the scenes' audio part
  {
    const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t slow = bf1943::diagnostics::LastSlowFrameUs();
    if (slow && now_us - slow < 200000) bf1943::diagnostics::NoteCulledNearSlow();
  }
  if (n <= 20 || (n & (n - 1)) == 0)
    REXLOG_INFO("BF audio: EA's mixer looked overloaded ({} times so far){}", n,
                cull ? "; stopping its lowest-priority sounds" : "; no sound stopped");
  if (cull) __imp__sub_82183680(ctx, base);
}
