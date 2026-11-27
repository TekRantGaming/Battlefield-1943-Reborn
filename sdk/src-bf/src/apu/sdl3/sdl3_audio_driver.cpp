#include "apu/sdl3/sdl3_audio_driver.h"

#include <algorithm>
#include <array>
#include <format>

#include <SDL3/SDL.h>

#include <rex/apu/audio_driver.h>
#include <rex/logging/log_category.h>
#include <rex/platform/perf.h>
#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>

namespace rex::apu::sdl3 {

namespace {

std::unique_ptr<IDriver> CreateDriver() {
  return std::make_unique<SDL3AudioDriver>();
}

void SDLCALL OnPull(void* userdata, SDL_AudioStream* stream, int additional_amount, int) {
  static_cast<SDL3AudioDriver*>(userdata)->Pull(stream, additional_amount);
}

constexpr std::array<DriverRegistration, 1> kDrivers = {
    DriverRegistration{DriverType::kAudio, "sdl3", {}, &CreateDriver}};

}  // namespace

std::string_view SDL3AudioDriver::GetName() const {
  return "sdl3";
}

Result<void> SDL3AudioDriver::Initialize(IDriverContext& context) {
  context_ = &context;
  if (auto found = context.GetSettingsRegistry().Find("apu.device")) {
    device_setting_ = *found;
  }
  Result<void> result = Ok();
  const Result<void> ran = context.RunOnUIThread([&result] {
    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      result = Err(ErrorCategory::kPlatform,
                   std::format("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError()));
    }
  });
  if (!ran) {
    return ran;
  }
  return result;
}

void SDL3AudioDriver::Shutdown() {
  CloseDevice();
  if (context_) {
    if (const auto quit = context_->RunOnUIThread([] { SDL_QuitSubSystem(SDL_INIT_AUDIO); });
        !quit) {
      REXAPU_WARN("SDL audio was not stopped, {}", quit.error().message);
    }
    context_ = nullptr;
  }
}

Result<u32> SDL3AudioDriver::OpenDevice(u32 channels, AudioFill fill) {
  CloseDevice();
  fill_ = std::move(fill);
  SDL_AudioSpec spec = {};
  spec.freq = static_cast<int>(kAudioSampleRate);
  spec.format = SDL_AUDIO_F32;
  spec.channels = static_cast<int>(channels);
  channels_ = channels;
  opened_name_ = WantedName();
  target_ = WantedDevice(opened_name_);
  stream_ = SDL_OpenAudioDeviceStream(target_, &spec, &OnPull, this);
  if (!stream_ && target_ != SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) {
    REXAPU_WARN("the audio device '{}' did not open ({}), the system default plays", opened_name_, SDL_GetError());
    target_ = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    stream_ = SDL_OpenAudioDeviceStream(target_, &spec, &OnPull, this);
  }
  if (!stream_) {
    channels_ = 0;
    return Err<u32>(ErrorCategory::kPlatform,
                    std::format("SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError()));
  }
  SDL_AudioSpec device = {};
  if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &device, nullptr) &&
      device.channels <= 2 && channels > 2) {
    SDL_DestroyAudioStream(stream_);
    spec.channels = 2;
    channels_ = 2;
    stream_ = SDL_OpenAudioDeviceStream(target_, &spec, &OnPull, this);
    if (!stream_) {
      channels_ = 0;
      return Err<u32>(ErrorCategory::kPlatform,
                      std::format("SDL_OpenAudioDeviceStream() stereo failed: {}", SDL_GetError()));
    }
  }
  opened_multichannel_ = channels_ > 2;
  {
    const char* name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream_));
    REXAPU_INFO("audio endpoint '{}'{}: {} ch", name ? name : "?",
                target_ == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ? " (system default)" : "", channels_);
  }
  if (!SDL_ResumeAudioStreamDevice(stream_)) {
    const std::string why = SDL_GetError();
    CloseDevice();
    return Err<u32>(ErrorCategory::kPlatform,
                    std::format("SDL_ResumeAudioStreamDevice() failed: {}", why));
  }
  return channels_;
}

void SDL3AudioDriver::CloseDevice() {
  if (stream_) {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }
  channels_ = 0;
  fill_ = nullptr;
}

bool SDL3AudioDriver::IsDeviceLost() const {
  if (!stream_) {
    return false;
  }
  if (SDL_GetAudioStreamDevice(stream_) == 0) {
    return true;
  }
  // Battlefield 1943 Reborn patch (v0.10's audio_device), four times a second: the setting changed, the chosen device
  // went (the system default plays until it is back) or came back, or the device moved to the other layout (stereo
  // against surround: SDL follows the default device itself, the mix is ours). The audio system then reopens.
  const auto now = std::chrono::steady_clock::now();
  if (now < next_check_) {
    return false;
  }
  next_check_ = now + std::chrono::milliseconds(250);
  const std::string name = WantedName();
  const u32 target = WantedDevice(name);
  if (target != target_) {
    REXAPU_INFO("audio: {}, moving the output",
                name != opened_name_ ? "the setting changed"
                : target == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ? "the chosen device is not there"
                                                              : "the chosen device is back");
    return true;
  }
  SDL_AudioSpec device = {};
  if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &device, nullptr) &&
      (device.channels > 2) != opened_multichannel_) {
    REXAPU_INFO("audio: the device's layout changed ({} ch), reopening", device.channels);
    return true;
  }
  return false;
}

std::string SDL3AudioDriver::WantedName() const {
  return device_setting_ ? device_setting_->GetText() : std::string();
}

u32 SDL3AudioDriver::WantedDevice(const std::string& name) const {
  if (name.empty()) {
    return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
  }
  SDL_AudioDeviceID found = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
  int count = 0;
  SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
  for (int i = 0; devices && i < count; ++i) {
    const char* device_name = SDL_GetAudioDeviceName(devices[i]);
    if (device_name && name == device_name) {
      found = devices[i];
      break;
    }
  }
  SDL_free(devices);
  return found;
}

void SDL3AudioDriver::Pull(SDL_AudioStream* stream, int additional_amount) {
  const u32 samples = static_cast<u32>(std::max(additional_amount, 0)) / sizeof(f32);
  const u32 count = samples - samples % channels_;
  if (count == 0 || !fill_) {
    return;
  }
  scratch_.resize(count);
  fill_(std::span<f32>(scratch_.data(), count), channels_);
  SDL_PutAudioStreamData(stream, scratch_.data(), static_cast<int>(count * sizeof(f32)));
}

std::string_view SDL3AudioProvider::GetName() const {
  return "rexapu-sdl3";
}

Result<void> SDL3AudioProvider::Connect(IInterfaceFactory&) {
  rex::memory::SetTagName(rex::apu::kAPU, "APU");
  for (const char* name : {"audio_frame_latency_us", "buffer_queue_depth"}) {
    if (auto counter = rex::platform::RegisterCounter(name); !counter) {
      REXAPU_WARN("{}", counter.error().message);
    }
  }
  return Ok();
}

void SDL3AudioProvider::Disconnect() {}

Result<void> SDL3AudioProvider::Init() {
  return Ok();
}

void SDL3AudioProvider::Shutdown() {}

std::span<const DriverRegistration> SDL3AudioProvider::GetDrivers() const {
  return kDrivers;
}

}  // namespace rex::apu::sdl3

REX_DECLARE_MODULE(rex::apu::sdl3::SDL3AudioProvider)
