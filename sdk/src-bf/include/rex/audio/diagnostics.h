/**
 * @file        rex/audio/diagnostics.h
 * @brief       Audio diagnostics for players' logs (audio_diagnostics)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, audio_diagnostics);

// Counters bumped where sound goes wrong (the output running dry, an XMA voice starved of input or a bad frame, the
// game's mixer late), summed up in the log every 5 seconds when something did, with an all-clear line every minute.
// Each Note is a few relaxed atomic operations; the lines are written from the audio thread that hands frames over
// (NoteFrameIn), never from the device's callback.
namespace rex::audio::diag {

inline bool On() { return REXCVAR_GET(audio_diagnostics); }

// The output driver (sdl_audio_driver.cpp).
void NoteDevice(const char* name, int channels, int rate, int buffer_frames, uint32_t game_frame_samples);
void NoteFrameIn(size_t queued);                        // a game frame queued for the device (the summary's clock)
void NoteFrameOut(size_t queued_after, int64_t chunk_us);  // the device took one
void NoteSilentChunk();                                 // the device wanted a frame and none was there

// The XMA decoder (xma_context.cpp).
void NoteXmaStarved(int64_t ms);  // a voice's output waited this long for input, then input came
void NoteXmaBadFrame();           // an unreadable frame (error 4)
void NoteXmaNoBits();             // a frame with no bits to copy (its input buffer skipped)
void NoteXmaInputWait();          // a frame waiting for the next input buffer (normal, counted)

// The render callback's wait for the game's mixer (audio_system.cpp WaitForGameMixer).
void NoteMixerWait(bool ready, int64_t waited_us);

// Where the player is ("playing", "loading": the game's own names), shown in the summary lines. A string that lives
// for the whole run (a literal).
void SetScene(const char* scene);

// The run's totals so far (the game's per-scene summaries take the difference).
struct Totals {
  int64_t underruns = 0, silent_chunks = 0, starved = 0, bad_frames = 0, mixer_late = 0;
};
Totals RunTotals();
// 27 Nov 2026: the audio threads' time, so a log says where an underrun's time went: the game's render callback (run
// by the audio worker for each packet: EA's mixer filling it) and the XMA decoder's work. Each Note is two relaxed
// atomics; a game's diagnostics take the totals (and the longest) since their last take, once a frame.
enum class Timed : int { kRenderCallback, kXmaWork, kCount };
void NoteTime(Timed what, int64_t ns);
struct TimeTotals {
  int64_t count = 0, total_ns = 0, max_ns = 0;
};
void TakeTimes(TimeTotals out[int(Timed::kCount)]);

}  // namespace rex::audio::diag
