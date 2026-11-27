// Battlefield 1943 Reborn, SDK v0.11+ port (phase 6): v0.10's rex/audio/audio_system.h, the call the game makes. Our
// SDK patch keeps it for v0.11's audio worker (rex/kernel/bf_audio.h).
#pragma once

#include <rex/kernel/bf_audio.h>

namespace rex::audio {
using RenderReadyCheck = ::rex::kernel::bf::RenderReadyCheck;
inline void SetRenderReadyCheck(RenderReadyCheck check) { ::rex::kernel::bf::SetRenderReadyCheck(check); }
inline void NotifyGameMixerTick() { ::rex::kernel::bf::NotifyGameMixerTick(); }
}  // namespace rex::audio
