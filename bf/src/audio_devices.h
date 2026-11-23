// Audio devices (TekRantGaming, 23 Nov 2026: "so users can set what device they have to be output or input devices
// for both game sound and the game chat"): Settings > Audio picks, by name, the device for the game's sound, for the
// voice chat's voices and for its microphone. An empty setting is the system default, which SDL follows when it
// changes. The game's own audio (the runtime's SDL driver) reads the audio_device cvar itself; the streams opened
// here (menu music and sounds, the achievement chime, the voice chat) are kept on their role's device by a watcher.
#pragma once

#include <string>
#include <vector>

#include <SDL3/SDL.h>

namespace bf1943::audio_devices {

enum class Role { kGame, kChatOut, kChatIn };

// The cvar each role's choice is saved in ("" = the system default).
const char* CvarOf(Role role);

// The devices there now, by the names Windows (or PulseAudio and PipeWire) gives them, for the Settings rows.
std::vector<std::string> Names(bool recording);

// A stream on the role's device, kept there: moved when the setting changes, to the system default while the chosen
// device is unplugged, and back to it when it returns (checked four times a second). `spec` is our side of the
// stream (what we put in for playback, what we take out for recording); `callback` the playback's get callback (or
// null: data put in by hand). The device plays at once. Null when no device would open (SDL_GetError says why).
SDL_AudioStream* OpenStream(Role role, const SDL_AudioSpec& spec, SDL_AudioStreamCallback callback, void* user,
                            const char* who);
// In place of SDL_DestroyAudioStream for a stream OpenStream gave; its callback has stopped once this returns.
void CloseStream(SDL_AudioStream* stream);

}  // namespace bf1943::audio_devices
