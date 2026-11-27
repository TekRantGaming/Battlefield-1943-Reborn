/**
 * @file        kernel/bf_audio.h
 * @brief       The title's hand in the audio worker and the XMA decoder
 *
 * @license     BSD 3-Clause License
 *
 * @remarks     Battlefield 1943 Reborn's patch of the SDK (v0.10's rex/audio additions, PR #60, kept for v0.11's APU
 *              and XMA decoder in src/kernel): a render callback that waits a moment for a late game mixer instead of
 *              rendering a silent frame, and XMA kicks made in a burst decoded together, in parallel, afterwards.
 */

#pragma once

#include <rex/platform.h>

namespace rex::kernel::bf {

/**
 * A title's check, run before each render callback: true when its own mixer has the next packet ready for the guest's
 * XAudio. While it says no, the callback waits for it, up to 12 ms (the output queue holds several frames, so a short
 * wait is not heard; rendering at once would give a silent frame). After a wait that ran out it does not wait again
 * until the check has said yes once. Null: never wait.
 */
using RenderReadyCheck = bool (*)();
REX_KERNEL_API void SetRenderReadyCheck(RenderReadyCheck check);
/** The title's mixer finished a tick (its check may now say yes): wakes a render callback waiting for it. */
REX_KERNEL_API void NotifyGameMixerTick();

/**
 * Deferred XMA kicks, for a title whose audio thread starts many sounds at once. The console decoded XMA beside the
 * CPU; here a kick (XMAEnableContext) decodes on the kicking thread. Between Begin and End, kicks made on the calling
 * thread only enable their contexts; End decodes them all in parallel (4 worker threads and the caller) and returns
 * once they are done. Kicks on other threads, or outside Begin and End, decode at once as before.
 */
REX_KERNEL_API void BeginDeferredXmaKicks();
REX_KERNEL_API void EndDeferredXmaKicks();

}  // namespace rex::kernel::bf
