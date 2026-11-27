#include "kernel/apu.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>

#include <rex/platform/perf.h>
#include <rex/logging/log_category.h>
#include <rex/apu/audio_driver.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/object/object.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/platform/sync.h>
#include <rex/kernel/bf_audio.h>
#include <rex/platform/thread.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "kernel/thread_wait.h"
#include "kernel/xma/decoder.h"
#include "kernel/title_state.h"

namespace rex::kernel {

namespace {

// Battlefield 1943 Reborn patch (bf_audio.h, v0.10's PR #60).
// A client's callbacks no closer than 3/4 of a frame (256 samples at 48 kHz): a burst of them emptied EA's queue.
constexpr auto kFramePeriod = std::chrono::microseconds(256 * 1000000 / 48000);
constexpr auto kMinCallbackSpacing = kFramePeriod * 3 / 4;
// Before a render callback, how long to wait for the title's mixer (SetRenderReadyCheck).
constexpr auto kRenderWait = std::chrono::milliseconds(12);
std::atomic<rex::kernel::bf::RenderReadyCheck> g_render_ready{nullptr};
// WaitForGameMixer sleeps on this until the title's mixer ticks (bf::NotifyGameMixerTick).
std::mutex g_mixer_tick_mutex;
std::condition_variable g_mixer_tick_cv;
// The audio log line, every 30 s while frames are rendered (the worker thread only): frames, waits for the title's
// mixer, their time, and waits that ran out.
u64 g_stat_frames = 0, g_stat_waits = 0, g_stat_waits_out = 0, g_stat_wait_us = 0;
// The output's underruns (rex::apu AudioStats: a mix period with nothing queued, silence played) at the last line, so
// each line says how many came in its 30 s: a player's log shows whether the sound broke up (issue 74, 0.4.8.1).
u64 g_stat_underruns_said = 0;
std::chrono::steady_clock::time_point g_stat_since{};

// While the title's check says its mixer has no packet ready, wait for it, up to kRenderWait. After a wait that ran
// out, not again until the check has said yes once, so a mixer that stopped can never slow every callback.
void WaitForGameMixer(bool& armed) {
  const auto ready = g_render_ready.load(std::memory_order_relaxed);
  if (!ready) {
    return;
  }
  if (ready()) {
    armed = true;
    return;
  }
  if (!armed) {
    return;
  }
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + kRenderWait;
  bool ok = false;
  {
    // Asleep until the mixer ticks (NotifyGameMixerTick), looking again at least every millisecond in case a tick's
    // notice comes from a mixer the title does not report. (v0.10 in 0.4.8 looked every 100 us with a zero sleep, a
    // spin that held a core while the mixer ran late: issue 74.)
    std::unique_lock lock(g_mixer_tick_mutex);
    while (!(ok = ready())) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) break;
      g_mixer_tick_cv.wait_for(lock, std::min<std::chrono::steady_clock::duration>(deadline - now,
                                                                                std::chrono::milliseconds(1)));
    }
  }
  ++g_stat_waits;
  g_stat_wait_us += u64(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
  if (!ok) {
    ++g_stat_waits_out;
    armed = false;
    REXAPU_DEBUG("Audio worker: the title's mixer was not ready after {} ms; rendering anyway", kRenderWait.count());
  }
}

constexpr std::chrono::milliseconds kWorkerShutdownTimeout{500};
constexpr u32 kDefaultQueuedFrames = 8;

}  // namespace

APU::APU(TitleRuntime& runtime, rex::apu::IAudioSystem* audio)
    : runtime_(runtime),
      audio_(audio),
      queued_frames_(audio ? audio->GetSettings().max_queued_frames.Get() : kDefaultQueuedFrames) {
  for (u32 i = 0; i < kMaxClients; ++i) {
    semaphores_[i] = platform::Semaphore::Create(0, static_cast<i32>(queued_frames_));
    wait_handles_[i] = semaphores_[i].get();
  }
  shutdown_event_ = platform::Event::CreateAutoResetEvent(false);
  wait_handles_[kMaxClients] = shutdown_event_.get();
  xma_decoder_ = std::make_unique<XMADecoder>(StateOf(runtime).GetFunctionDispatcher());
  resume_event_ = platform::Event::CreateAutoResetEvent(false);
}

APU::~APU() {
  Shutdown();
}

Result<void> APU::Setup() {
  if (X_STATUS status = xma_decoder_->Setup(); status != X_STATUS_SUCCESS) {
    return Err(StatusError(status));
  }
  worker_running_ = true;
  auto thread = std::make_unique<KernelThread>(runtime_, 128 * 1024, 0, [this]() {
    WorkerMain();
    return 0;
  });
  thread->SetName("Audio Worker");
  auto launched = runtime_.GetThreadRegistry().Launch(std::move(thread));
  if (!launched) {
    worker_running_ = false;
    return std::unexpected(launched.error());
  }
  worker_thread_ = *launched;
  return Ok();
}

void APU::WorkerMain() {
  std::array<std::chrono::steady_clock::time_point, kMaxClients> next_callback{};  // (Battlefield 1943 Reborn patch)
  bool wait_armed = true;
  while (worker_running_) {
    auto result = platform::WaitAny(wait_handles_.data(), wait_handles_.size(), true,
                                    std::chrono::milliseconds(500));
    if (result.first == platform::WaitResult::kFailed) {
      REXAPU_WARN("Audio worker: WaitAny failed");
      continue;
    }

    if (result.first == platform::WaitResult::kSuccess && result.second == kMaxClients) {
      if (paused_) {
        pause_fence_.Signal();
        platform::Wait(resume_event_.get(), false);
      }
      continue;
    }

    bool pumped = false;
    if (result.first == platform::WaitResult::kSuccess) {
      const auto index = result.second;
      u32 client_callback;
      u32 client_callback_arg;
      {
        std::lock_guard lock(mutex_);
        client_callback = clients_[index].callback;
        client_callback_arg = clients_[index].wrapped_callback_arg;
      }
      if (client_callback) {
        // Battlefield 1943 Reborn patch (bf_audio.h): spaced, and after the title's mixer if it is a moment late.
        auto now = std::chrono::steady_clock::now();
        if (now < next_callback[index]) {
          platform::SleepFor(std::chrono::duration_cast<std::chrono::microseconds>(next_callback[index] - now));
          now = next_callback[index];
        }
        next_callback[index] = now + kMinCallbackSpacing;
        WaitForGameMixer(wait_armed);
        REX_PROFILE_SCOPE_TEXT("apu", "rex::kernel::APU->client_callback");
        u64 args[] = {client_callback_arg};
        StateOf(runtime_).GetFunctionDispatcher()->Execute(
            KernelThread::GetCallingThread()->GetThreadState(), client_callback, args,
            std::size(args));
        ++g_stat_frames;
        if (const auto stat_now = std::chrono::steady_clock::now(); g_stat_since == std::chrono::steady_clock::time_point{}) {
          g_stat_since = stat_now;
        } else if (stat_now - g_stat_since >= std::chrono::seconds(30)) {
          const u64 underruns = audio_ ? audio_->GetStats().underruns : 0;
          REXAPU_INFO("Audio: over 30 s, {} frames rendered; the game's mixer waited for {} times, {:.1f} ms in all, {} "
                      "ran out; {} underruns (silence played)", g_stat_frames, g_stat_waits, g_stat_wait_us / 1000.0,
                      g_stat_waits_out, underruns - std::min(underruns, g_stat_underruns_said));
          g_stat_underruns_said = underruns;
          g_stat_frames = g_stat_waits = g_stat_waits_out = g_stat_wait_us = 0;
          g_stat_since = stat_now;
        }
      }
      pumped = true;
    }

    if (!worker_running_) {
      break;
    }

    if (!pumped) {
      REX_PROFILE_SCOPE_TEXT("apu", "Sleep");
      platform::SleepFor(std::chrono::milliseconds(500));
    }
  }
  worker_running_ = false;
}

void APU::StopWorker() {
  {
    std::lock_guard pause_lock(pause_mutex_);
    if (worker_running_) {
      xma_decoder_->Shutdown();
      worker_running_ = false;
      shutdown_event_->Set();
    }
    paused_ = false;
    if (parked_) {
      parked_ = false;
      resume_event_->Set();
    }
  }
  if (worker_thread_) {
    {
      if (ThreadPin pin = runtime_.GetThreadRegistry().FindByHandle(worker_thread_)) {
        pin->RequestStop();
        std::array<ThreadPin, 1> threads{std::move(pin)};
        WaitForThreadsToStop(threads, kWorkerShutdownTimeout, [](const ThreadPin&) {
          REXAPU_WARN("Audio worker still running after {} ms", kWorkerShutdownTimeout.count());
        });
      }
    }
    CloseOwned(runtime_.GetObjectTable(), worker_thread_);
    worker_thread_ = 0;
  }
}

void APU::Shutdown() {
  StopWorker();
  std::lock_guard lock(mutex_);
  for (Client& client : clients_) {
    if (!client.in_use) {
      continue;
    }
    audio_->CloseStream(client.stream);
    if (client.wrapped_callback_arg) {
      StateOf(runtime_).GetAddressSpace()->SystemHeapFree(client.wrapped_callback_arg);
    }
    client = {};
  }
}

Result<u32> APU::RegisterClient(u32 callback, u32 callback_arg) {
  if (!audio_) {
    REXAPU_ERROR("the program has no audio system, a render client is refused");
    return Err<u32>(StatusError(X_STATUS_UNSUCCESSFUL));
  }
  std::lock_guard lock(mutex_);
  auto free = std::ranges::find_if(clients_, [](const Client& c) { return !c.in_use; });
  if (free == clients_.end()) {
    return Err<u32>(StatusError(X_STATUS_NO_MEMORY));
  }
  const u32 index = static_cast<u32>(free - clients_.begin());
  platform::Semaphore* semaphore = semaphores_[index].get();
  auto stream = audio_->OpenStream([semaphore] { semaphore->Release(1, nullptr); });
  if (!stream) {
    REXAPU_ERROR("the audio system refused a stream, {}", stream.error().message);
    return Err<u32>(StatusError(X_STATUS_UNSUCCESSFUL));
  }
  semaphore->Release(static_cast<i32>(queued_frames_), nullptr);
  const u32 ptr = StateOf(runtime_).GetAddressSpace()->SystemHeapAlloc(0x4);
  *StateOf(runtime_).GetAddressSpace()->TranslateVirtual<be_u32*>(ptr) = callback_arg;
  *free = {*stream, callback, callback_arg, ptr, true};
  return index;
}

void APU::UnregisterClient(u32 index) {
  REX_PROFILE_SCOPE("apu");
  std::lock_guard lock(mutex_);
  if (index >= kMaxClients || !clients_[index].in_use) {
    return;
  }
  audio_->CloseStream(clients_[index].stream);
  StateOf(runtime_).GetAddressSpace()->SystemHeapFree(clients_[index].wrapped_callback_arg);
  clients_[index] = {};
  platform::Semaphore* semaphore = semaphores_[index].get();
  while (platform::Wait(semaphore, false, std::chrono::milliseconds(0)) ==
         platform::WaitResult::kSuccess) {}
}

void APU::SubmitFrame(u32 index, u32 samples_ptr) {
  REX_PROFILE_SCOPE("apu");
  std::lock_guard lock(mutex_);
  if (index >= kMaxClients || !clients_[index].in_use) {
    return;
  }
  const auto* samples =
      StateOf(runtime_).GetAddressSpace()->TranslateVirtual<const be_f32*>(samples_ptr);
  std::array<f32, rex::apu::kAudioFrameSamples> frame;
  for (size_t i = 0; i < frame.size(); ++i) {
    frame[i] = samples[i];
  }
  if (auto submitted = audio_->Submit(clients_[index].stream, frame);
      !submitted && !clients_[index].refused_logged) {
    clients_[index].refused_logged = true;
    REXAPU_WARN("the audio system refused a frame for client {}, {}", index,
                submitted.error().message);
  }
}

u32 APU::GetOutputChannels() const {
  return audio_ ? audio_->GetOutputChannels() : 2;
}

void APU::SetPaused(bool paused) {
  std::lock_guard pause_lock(pause_mutex_);
  if (paused_ == paused) {
    return;
  }
  paused_ = paused;
  if (paused) {
    if (worker_running_) {
      shutdown_event_->Set();
      pause_fence_.Wait();
      parked_ = true;
    }
    xma_decoder_->Pause();
  } else {
    if (parked_) {
      parked_ = false;
      resume_event_->Set();
    }
    xma_decoder_->Resume();
  }
}

u32 APU::CountCredits(u32 index) {
  platform::Semaphore* semaphore = semaphores_[index].get();
  u32 count = 0;
  while (platform::Wait(semaphore, false, std::chrono::milliseconds(0)) ==
         platform::WaitResult::kSuccess) {
    ++count;
  }
  if (count > 0) {
    semaphore->Release(static_cast<i32>(count), nullptr);
  }
  return count;
}

bool APU::TakeCredit(u32 index) {
  return platform::Wait(semaphores_[index].get(), false, std::chrono::milliseconds(0)) ==
         platform::WaitResult::kSuccess;
}

}  // namespace rex::kernel

namespace rex::kernel::bf {
void SetRenderReadyCheck(RenderReadyCheck check) { g_render_ready.store(check, std::memory_order_relaxed); }
void NotifyGameMixerTick() {
  // The lock orders this notice after a waiter's check of ready(), so none is missed between the two.
  { std::lock_guard lock(g_mixer_tick_mutex); }
  g_mixer_tick_cv.notify_all();
}
}  // namespace rex::kernel::bf
