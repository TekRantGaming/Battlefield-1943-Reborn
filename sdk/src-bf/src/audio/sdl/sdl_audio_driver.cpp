/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <SDL3/SDL.h>

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");
REXCVAR_DEFINE_STRING(audio_device, "", "Audio",
                      "Playback device for the game's audio, by its name; empty: the system default");

namespace rex::audio::sdl {

SDLAudioDriver::SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SDLAudioDriver::~SDLAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
}

bool SDLAudioDriver::Initialize() {
  // Set audio category for proper OS audio handling
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");

  // Set app name for audio device identification
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "rexglue");

  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXAPU_ERROR("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
    return false;
  }
  sdl_initialized_ = true;

  // A stream of our own, bound to a logical device (not SDL_OpenAudioDeviceStream's, whose binding is fixed), so it
  // can move to another device while the game runs. Its input side is set by MoveTo, from the device's layout.
  SDL_AudioSpec spec = {};
  spec.freq = frame_frequency_;
  spec.format = SDL_AUDIO_F32LE;
  spec.channels = frame_channels_;
  sdl_device_channels_ = frame_channels_;
  sdl_stream_ = SDL_CreateAudioStream(&spec, &spec);
  if (!sdl_stream_) {
    REXAPU_ERROR("SDL_CreateAudioStream() failed: {}", SDL_GetError());
    return false;
  }
  if (!SDL_SetAudioStreamGetCallback(sdl_stream_, SDLCallback, this)) {
    REXAPU_ERROR("SDL_SetAudioStreamGetCallback() failed: {}", SDL_GetError());
    return false;
  }
  SDL_AudioDeviceID target = WantedDevice();
  if (!MoveTo(target, "start") &&
      (target == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK || !MoveTo(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, "start"))) {
    return false;
  }
  watcher_ = std::thread([this] { Watch(); });
  return true;
}

SDL_AudioDeviceID SDLAudioDriver::WantedDevice() {
  const std::string wanted = REXCVAR_GET(audio_device);
  if (wanted.empty()) {
    return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
  }
  SDL_AudioDeviceID found = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
  int count = 0;
  SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
  for (int i = 0; devices && i < count; ++i) {
    const char* name = SDL_GetAudioDeviceName(devices[i]);
    if (name && wanted == name) {
      found = devices[i];
      break;
    }
  }
  SDL_free(devices);
  return found;
}

bool SDLAudioDriver::MoveTo(SDL_AudioDeviceID target, const char* why) {
  // The device's own format (no spec): its channel count decides the mix, as when the stream was opened at 6.
  const SDL_AudioDeviceID device = SDL_OpenAudioDevice(target, nullptr);
  if (!device) {
    REXAPU_ERROR("SDL_OpenAudioDevice() failed ({}): {}", why, SDL_GetError());
    return false;
  }
  SDL_AudioSpec obtained_spec = {};
  if (!SDL_GetAudioDeviceFormat(device, &obtained_spec, nullptr)) {
    REXAPU_WARN("SDL_GetAudioDeviceFormat() failed: {}", SDL_GetError());
    obtained_spec.channels = frame_channels_;
    obtained_spec.freq = frame_frequency_;
  }
  // A 1-channel device gets the stereo fold too, then SDL collapses to mono.
  // Handing it a 6ch stream instead would use SDL's own downmix.
  const uint8_t channels = obtained_spec.channels <= 2 ? 2 : 6;
  SDL_AudioSpec input = {};
  input.freq = frame_frequency_;
  input.format = SDL_AUDIO_F32LE;
  input.channels = channels;
  SDL_LockAudioStream(sdl_stream_);  // the callback runs with it held: no frame is split between two layouts
  SDL_SetAudioStreamFormat(sdl_stream_, &input, nullptr);
  sdl_device_channels_ = channels;
  SDL_UnlockAudioStream(sdl_stream_);
  if (sdl_device_) {
    SDL_UnbindAudioStream(sdl_stream_);
  }
  if (!SDL_BindAudioStream(device, sdl_stream_)) {
    REXAPU_ERROR("SDL_BindAudioStream() failed ({}): {}", why, SDL_GetError());
    SDL_CloseAudioDevice(device);
    if (sdl_device_) {
      SDL_BindAudioStream(sdl_device_, sdl_stream_);  // back where it was
    }
    return false;
  }
  if (sdl_device_) {
    SDL_CloseAudioDevice(sdl_device_);
  }
  sdl_device_ = device;
  sdl_target_ = target;

  // The endpoint layout decides which mix the callback runs, and it is the
  // first thing worth knowing when a report says the balance is wrong on one
  // speaker setup and right on another.
  const char* device_name = SDL_GetAudioDeviceName(device);
  REXAPU_INFO("audio endpoint '{}'{} ({}): {} ch, {} Hz, format 0x{:04X}; submitting {} ch",
              device_name ? device_name : "?",
              target == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ? " (system default)" : "", why,
              obtained_spec.channels, obtained_spec.freq,
              static_cast<uint32_t>(obtained_spec.format), static_cast<int>(sdl_device_channels_));

  if (!SDL_ResumeAudioDevice(device)) {
    REXAPU_ERROR("SDL_ResumeAudioDevice() failed: {}", SDL_GetError());
  }
  return true;
}

// Four times a second: the setting changed, the chosen device went (the system default plays until it is back) or
// came back, or the system default moved to a device of the other layout (stereo against 5.1: SDL follows the
// default itself, the mix is ours to change).
void SDLAudioDriver::Watch() {
  std::string last_wanted = REXCVAR_GET(audio_device);
  int failures = 0;
  std::unique_lock<std::mutex> lock(watch_mutex_);
  while (!watch_wake_.wait_for(lock, std::chrono::milliseconds(250), [this] { return watch_stop_; })) {
    const std::string wanted = REXCVAR_GET(audio_device);
    const SDL_AudioDeviceID target = WantedDevice();
    const char* why = nullptr;
    if (target != sdl_target_) {
      why = wanted != last_wanted ? "the setting changed"
            : target == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ? "the chosen device is not there"
                                                          : "the chosen device is back";
    } else {
      if (wanted != last_wanted) {
        REXAPU_INFO("audio: the setting names '{}'{}", wanted.empty() ? "the system default" : wanted,
                    !wanted.empty() && target == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ? ", the system default plays" : "");
        last_wanted = wanted;
      }
      SDL_AudioSpec now = {};
      if (SDL_GetAudioDeviceFormat(sdl_device_, &now, nullptr) &&
          (now.channels <= 2 ? 2 : 6) != sdl_device_channels_) {
        why = "the device's layout changed";
      }
    }
    if (!why) {
      continue;
    }
    if (failures > 0 && --failures > 0) {
      continue;  // a device that would not open: tried again every few seconds, not four times a second
    }
    if (MoveTo(target, why)) {
      last_wanted = wanted;
    } else {
      failures = 20;
    }
  }
}

void SDLAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  float* output_frame;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[frame_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, input_frame, frame_samples_ * sizeof(float));

  static uint32_t sdl_submit_count = 0;
  if (sdl_submit_count < 10) {
    REXAPU_DEBUG("SDLAudioDriver::SubmitFrame: frame_ptr={:08X} queued_count={}", frame_ptr,
                 frames_queued_.size() + 1);
    sdl_submit_count++;
  }

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
    PROFILE_BUFFER_QUEUE_DEPTH(static_cast<int64_t>(frames_queued_.size()));
  }
}

void SDLAudioDriver::Shutdown() {
  if (watcher_.joinable()) {
    {
      std::lock_guard<std::mutex> lock(watch_mutex_);
      watch_stop_ = true;
    }
    watch_wake_.notify_all();
    watcher_.join();
  }
  if (sdl_stream_) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
  }
  if (sdl_device_) {
    SDL_CloseAudioDevice(sdl_device_);
    sdl_device_ = 0;
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

void SDLAudioDriver::SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                 [[maybe_unused]] int total_amount) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback called with nullptr.");
    return;
  }
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  const int sample_count =
      static_cast<int>(channel_samples_ * std::max<uint8_t>(driver->sdl_device_channels_, 1));
  const int len = static_cast<int>(sizeof(float) * sample_count);
  float* data = SDL_stack_alloc(float, sample_count);
  if (!data) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback failed to allocate {} samples", sample_count);
    return;
  }
  // Snapshot once. A change mid-callback would split the frame across two mixes.
  const StereoFold fold = GetStereoFold();
  const SurroundMix mix = GetSurroundMix();
  const float gain = GetOutputGain();
  while (additional_amount > 0) {
    static uint32_t sdl_callback_count = 0;
    std::unique_lock<std::mutex> guard(driver->frames_mutex_);
    if (driver->frames_queued_.empty()) {
      if (sdl_callback_count < 10) {
        REXAPU_DEBUG("SDLCallback: no frames queued (silence)");
        sdl_callback_count++;
      }
      std::memset(data, 0, len);
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed while filling silence: {}", SDL_GetError());
        break;
      }
      additional_amount -= len;
    } else {
      auto buffer = driver->frames_queued_.front();
      driver->frames_queued_.pop();
      if (REXCVAR_GET(audio_mute)) {
        std::memset(data, 0, len);
      } else {
        switch (driver->sdl_device_channels_) {
          case 2:
            conversion::sequential_6_BE_to_interleaved_2_LE(data, buffer, channel_samples_, fold,
                                                            gain);
            break;
          case 6:
            conversion::sequential_6_BE_to_interleaved_6_LE(data, buffer, channel_samples_, mix,
                                                            gain);
            break;
          default:
            assert_unhandled_case(driver->sdl_device_channels_);
            break;
        }
      }
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed: {}", SDL_GetError());
        driver->frames_unused_.push(buffer);
        break;
      }
      driver->frames_unused_.push(buffer);

      auto ret = driver->semaphore_->Release(1, nullptr);
      assert_true(ret);
      additional_amount -= len;
    }
  }
  SDL_stack_free(data);
}

}  // namespace rex::audio::sdl
