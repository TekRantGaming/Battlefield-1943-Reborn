// Battlefield 1943 Reborn, SDK v0.11+ port (phase 6): v0.10's rex/audio/xma/decoder.h, the calls the game makes. Our
// SDK patch keeps them for v0.11's XMA decoder (rex/kernel/bf_audio.h).
#pragma once

#include <rex/kernel/bf_audio.h>

namespace rex::audio {
inline void BeginDeferredXmaKicks() { ::rex::kernel::bf::BeginDeferredXmaKicks(); }
inline void EndDeferredXmaKicks() { ::rex::kernel::bf::EndDeferredXmaKicks(); }
}  // namespace rex::audio
