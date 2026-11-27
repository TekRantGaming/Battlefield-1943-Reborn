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

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <stack>
#include <thread>

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

#include <SDL3/SDL.h>

namespace rex::audio::sdl {

class SDLAudioDriver : public AudioDriver {
 public:
  SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~SDLAudioDriver() override;

  bool Initialize();
  void SubmitFrame(uint32_t frame_ptr) override;
  void Shutdown();

 protected:
  static void SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                          int total_amount);

  // The device the audio_device cvar names (by its name), or the system default when it names none or its device
  // is not there. Moving there: the stream is bound to a new logical device, so a change of the setting, a device
  // unplugged and the same device plugged in again all take effect while the game runs (the watcher thread).
  SDL_AudioDeviceID WantedDevice();
  bool MoveTo(SDL_AudioDeviceID target, const char* why);
  void Watch();

  rex::thread::Semaphore* semaphore_ = nullptr;

  SDL_AudioStream* sdl_stream_ = nullptr;
  SDL_AudioDeviceID sdl_device_ = 0;  // the logical device the stream is bound to
  SDL_AudioDeviceID sdl_target_ = 0;  // what it was opened on: a physical device, or the default's id
  bool sdl_initialized_ = false;
  uint8_t sdl_device_channels_ = 0;   // changed only with the stream locked (the callback runs so)
  std::thread watcher_;
  std::mutex watch_mutex_;
  std::condition_variable watch_wake_;
  bool watch_stop_ = false;

  static const uint32_t frame_frequency_ = 48000;
  static const uint32_t frame_channels_ = 6;
  static const uint32_t channel_samples_ = 256;
  static const uint32_t frame_samples_ = frame_channels_ * channel_samples_;
  static const uint32_t frame_size_ = sizeof(float) * frame_samples_;
  std::queue<float*> frames_queued_ = {};
  std::stack<float*> frames_unused_ = {};
  std::mutex frames_mutex_ = {};
};

}  // namespace rex::audio::sdl
