// Decodes the XMA voices EA's mixer starts in one control step in parallel, after the step (rex::audio's deferred
// kicks), instead of one after another inside it.
//
// EA's mixer thread ("RWAudioCore Dac") runs a control step (sub_8218DBF8: per-tick callbacks, among them SndPlayer1's
// scheduler, which starts the sounds the game asked for) before each 256-sample mix tick. Starting an XMA voice feeds
// its input buffers and kicks the context (XMAEnableContext); on the console the hardware decoder then works beside
// the CPU, here the kick decodes on the kicking thread. In a busy fight one step starts up to 48 sounds, each decoding
// several frames up front, and the step ran 5-20 ms: past the ~10 ms of audio EA keeps queued, so the mix got 5 ms
// holes (the render wait in ea_mixer_ready.cpp turns those into a late frame). Deferring the step's kicks to its end
// and decoding them on worker threads (audio_xma_kick_threads) keeps EA's lock short; the mix tick that follows finds
// the voices decoded, as it would on the console.

#include <rex/audio/xma/decoder.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>

REX_EXTERN(__imp__sub_8218DBF8);
REX_HOOK_RAW(sub_8218DBF8) {
  rex::audio::BeginDeferredXmaKicks();
  __imp__sub_8218DBF8(ctx, base);
  rex::audio::EndDeferredXmaKicks();
}
