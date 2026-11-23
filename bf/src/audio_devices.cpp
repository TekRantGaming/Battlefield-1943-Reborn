// Audio devices: see audio_devices.h.

#include "audio_devices.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(bf_voice_output_device, "", "BF1943/Audio",
                      "Playback device for the voice chat's voices, by its name; empty: the system default");
REXCVAR_DEFINE_STRING(bf_voice_input_device, "", "BF1943/Audio",
                      "Microphone for the voice chat, by its name; empty: the system default");

namespace bf1943::audio_devices {
namespace {

bool Recording(Role role) { return role == Role::kChatIn; }
SDL_AudioDeviceID DefaultOf(Role role) {
  return Recording(role) ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
}

// The device a name is now, or 0 (not there).
SDL_AudioDeviceID Find(bool recording, const std::string& name) {
  int count = 0;
  SDL_AudioDeviceID* devices = recording ? SDL_GetAudioRecordingDevices(&count) : SDL_GetAudioPlaybackDevices(&count);
  SDL_AudioDeviceID found = 0;
  for (int i = 0; devices && i < count && !found; ++i) {
    const char* n = SDL_GetAudioDeviceName(devices[i]);
    if (n && name == n) found = devices[i];
  }
  SDL_free(devices);
  return found;
}

// What the role should play on (or record from) now: the chosen device, or the system default.
SDL_AudioDeviceID Wanted(Role role, std::string* name) {
  *name = rex::cvar::GetFlagByName(CvarOf(role));
  if (name->empty()) return DefaultOf(role);
  const SDL_AudioDeviceID id = Find(Recording(role), *name);
  return id ? id : DefaultOf(role);
}

std::string Label(SDL_AudioDeviceID device, SDL_AudioDeviceID target, Role role) {
  const char* n = SDL_GetAudioDeviceName(device);
  return fmt::format("'{}'{}", n ? n : "?", target == DefaultOf(role) ? " (system default)" : "");
}

struct Routed {
  SDL_AudioStream* stream;
  Role role;
  SDL_AudioDeviceID target;  // a physical device, or the default's id
  SDL_AudioDeviceID device;  // the logical device the stream is bound to
  std::string wanted;        // the setting when it was bound
  std::string who;
  int retry_in = 0;          // a device that would not open: ticks before the next try
};

// Never destroyed: the watcher may still be running when the program's statics go.
struct State {
  std::mutex mutex;
  std::vector<Routed> streams;
  bool watching = false;
};
State& S() {
  static State* s = new State;
  return *s;
}

bool Bind(Routed& r, SDL_AudioDeviceID target, const char* why) {
  const SDL_AudioDeviceID device = SDL_OpenAudioDevice(target, nullptr);
  if (!device) {
    REXLOG_WARN("BF audio: {} could not open its device ({}): {}", r.who, why, SDL_GetError());
    return false;
  }
  if (r.device) SDL_UnbindAudioStream(r.stream);
  if (!SDL_BindAudioStream(device, r.stream)) {
    REXLOG_WARN("BF audio: {} could not move ({}): {}", r.who, why, SDL_GetError());
    SDL_CloseAudioDevice(device);
    if (r.device) SDL_BindAudioStream(r.device, r.stream);  // back where it was
    return false;
  }
  if (r.device) SDL_CloseAudioDevice(r.device);
  r.device = device, r.target = target;
  SDL_ResumeAudioDevice(device);
  REXLOG_INFO("BF audio: {} on {} ({})", r.who, Label(device, target, r.role), why);
  return true;
}

// Four times a second, every stream: its setting changed, its chosen device went (the system default meanwhile) or
// came back. SDL itself moves a stream on the system default when the default changes.
void Watch() {
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    std::lock_guard lock(S().mutex);
    if (!SDL_WasInit(SDL_INIT_AUDIO)) continue;  // the program ending
    for (Routed& r : S().streams) {
      std::string wanted;
      const SDL_AudioDeviceID target = Wanted(r.role, &wanted);
      if (target == r.target) {
        r.wanted = wanted, r.retry_in = 0;
        continue;
      }
      if (r.retry_in > 0 && --r.retry_in > 0) continue;
      const char* why = wanted != r.wanted                ? "the setting changed"
                        : target == DefaultOf(r.role) ? "the chosen device is not there"
                                                      : "the chosen device is back";
      if (Bind(r, target, why)) r.wanted = wanted;
      else r.retry_in = 20;  // every 5 s, not four times a second
    }
  }
}

}  // namespace

const char* CvarOf(Role role) {
  switch (role) {
    case Role::kChatOut: return "bf_voice_output_device";
    case Role::kChatIn: return "bf_voice_input_device";
    default: return "audio_device";  // the runtime's (its SDL driver plays the game's audio there)
  }
}

std::vector<std::string> Names(bool recording) {
  std::vector<std::string> names;
  if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return names;  // the launcher before any sound
  int count = 0;
  SDL_AudioDeviceID* devices = recording ? SDL_GetAudioRecordingDevices(&count) : SDL_GetAudioPlaybackDevices(&count);
  for (int i = 0; devices && i < count; ++i)
    if (const char* n = SDL_GetAudioDeviceName(devices[i]); n && *n && std::find(names.begin(), names.end(), n) == names.end())
      names.emplace_back(n);
  SDL_free(devices);
  return names;
}

SDL_AudioStream* OpenStream(Role role, const SDL_AudioSpec& spec, SDL_AudioStreamCallback callback, void* user,
                            const char* who) {
  SDL_AudioStream* stream = SDL_CreateAudioStream(&spec, &spec);  // the device's side is set by the binding
  if (!stream) return nullptr;
  if (callback && !SDL_SetAudioStreamGetCallback(stream, callback, user)) {
    SDL_DestroyAudioStream(stream);
    return nullptr;
  }
  Routed r{stream, role, 0, 0, {}, who};
  const SDL_AudioDeviceID target = Wanted(role, &r.wanted);
  // A chosen device that will not open: the system default rather than no sound at all.
  if (!Bind(r, target, "opened") && (target == DefaultOf(role) || !Bind(r, DefaultOf(role), "opened"))) {
    SDL_DestroyAudioStream(stream);
    return nullptr;
  }
  std::lock_guard lock(S().mutex);
  S().streams.push_back(std::move(r));
  if (!S().watching) S().watching = true, std::thread(Watch).detach();
  return stream;
}

void CloseStream(SDL_AudioStream* stream) {
  if (!stream) return;
  SDL_AudioDeviceID device = 0;
  {
    std::lock_guard lock(S().mutex);
    auto& v = S().streams;
    if (auto it = std::find_if(v.begin(), v.end(), [&](const Routed& r) { return r.stream == stream; }); it != v.end())
      device = it->device, v.erase(it);
  }
  SDL_DestroyAudioStream(stream);
  if (device) SDL_CloseAudioDevice(device);
}

}  // namespace bf1943::audio_devices
