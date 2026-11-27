/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <bit>

#include "kernel/xma/context.h"
#include "kernel/xma/decoder.h"
#include <rex/platform/perf.h>
#include <rex/logging/log_category.h>
#include <rex/apu/audio_driver.h>
#include <rex/math.h>
#include <rex/ppc/mmio.h>
#include <rex/memory/stream.h>
#include <rex/string/text.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/runtime_settings.h>
#include <rex/platform/sync.h>
#include <rex/kernel/bf_audio.h>
#include <rex/platform/thread.h>

#include <algorithm>
#include <string>
#include "kernel/settings_access.h"

extern "C" {
#include "libavutil/log.h"
}  // extern "C"

namespace rex::kernel {

XMADecoder::XMADecoder(kernel::FunctionDispatcher* function_dispatcher)
    : memory_(function_dispatcher->GetAddressSpace()), function_dispatcher_(function_dispatcher) {}

XMADecoder::~XMADecoder() {
  Shutdown();
}

void AVLogCallback([[maybe_unused]] void* avcl, int level, const char* fmt, va_list va) {
  if (!GetRuntimeSettings().ffmpeg_verbose.Get() && level > AV_LOG_WARNING) {
    return;
  }

  string::StringBuffer buff;
  buff.AppendVarargs(fmt, va);
  auto msg = buff.ToStringView();

  switch (level) {
    case AV_LOG_ERROR:
      REXAPU_ERROR("ffmpeg: {}", msg);
      break;
    case AV_LOG_WARNING:
      REXAPU_WARN("ffmpeg: {}", msg);
      break;
    case AV_LOG_INFO:
      REXAPU_INFO("ffmpeg: {}", msg);
      break;
    case AV_LOG_VERBOSE:
    case AV_LOG_DEBUG:
    default:
      REXAPU_DEBUG("ffmpeg: {}", msg);
      break;
  }
}

// Battlefield 1943 Reborn patch (bf_audio.h): the decoder Begin/End act on, and this thread's deferred kicks.
namespace {
constexpr int kKickThreads = 4;
XMADecoder* g_decoder = nullptr;
thread_local bool t_defer_kicks = false;
thread_local std::vector<u32> t_kicked;
}  // namespace

X_STATUS XMADecoder::Setup() {
  g_decoder = this;
  av_log_set_callback(AVLogCallback);

  rex::ppc::AttachMMIO(rex::ppc::MMIOBlock::kAPU,
                       {this, MMIOReadRegisterThunk, MMIOWriteRegisterThunk});

  context_data_first_ptr_ = GetAddressSpace()->SystemHeapAlloc(
      sizeof(XMA_CONTEXT_DATA) * kContextCount, 256, kernel::kSystemHeapPhysical);
  context_data_last_ptr_ = context_data_first_ptr_ + (sizeof(XMA_CONTEXT_DATA) * kContextCount - 1);
  register_file_[XMARegister::kContextArrayAddress] =
      GetAddressSpace()->GetPhysicalAddress(context_data_first_ptr_);

  for (size_t i = 0; i < kContextCount; ++i) {
    u32 context_ptr = context_data_first_ptr_ + i * sizeof(XMA_CONTEXT_DATA);
    XMAContext& context = contexts_[i];
    if (context.Setup(i, GetAddressSpace(), context_ptr)) {
      REX_ASSERT_ALWAYS();
    }
  }
  register_file_[XMARegister::kNextContextIndex] = 1;
  context_bitmap_.Resize(kContextCount);

  work_event_ = rex::platform::Event::CreateAutoResetEvent(false);
  REX_ASSERT(work_event_ != nullptr);
  worker_exited_ = rex::platform::Event::CreateManualResetEvent(false);
  worker_ = rex::platform::Spawn("XMA Decoder", [this](const std::stop_token& stop) {
    WorkerThreadMain(stop);
    worker_exited_->Set();
  });

  return X_STATUS_SUCCESS;
}

void XMADecoder::WorkerThreadMain(const std::stop_token& stop) {
  while (!stop.stop_requested()) {
    bool did_work = false;
    for (u32 n = 0; n < kContextCount && !stop.stop_requested(); n++) {
      XMAContext& context = contexts_[n];
      bool worked = context.Work();
      if (worked) {
        context.SignalWorkDone();
        REX_PERF_INC("xma_frames_decoded");
      }
      did_work = did_work || worked;
    }

    if (paused_) {
      pause_fence_.Signal();
      resume_fence_.Wait();
    }

    if (did_work) {
      continue;
    }
    rex::platform::Wait(work_event_.get(), false);
  }
}

void XMADecoder::Shutdown() {
  rex::ppc::DetachMMIO(rex::ppc::MMIOBlock::kAPU, this);
  if (g_decoder == this) g_decoder = nullptr;
  {
    std::lock_guard<std::mutex> lock(kick_mutex_);
    kick_stop_ = true;
  }
  kick_cv_.notify_all();
  for (auto& thread : kick_threads_) thread.join();
  kick_threads_.clear();
  if (!worker_.joinable()) {
    return;
  }

  worker_.request_stop();
  work_event_->Set();
  if (paused_) {
    Resume();
  }

  if (rex::platform::Wait(worker_exited_.get(), false, std::chrono::milliseconds(2000)) ==
      rex::platform::WaitResult::kTimeout) {
    REXAPU_WARN("XMA: Worker thread did not exit within 2s, waiting for it");
  }
  worker_.join();

  if (context_data_first_ptr_) {
    GetAddressSpace()->SystemHeapFree(context_data_first_ptr_);
  }

  context_data_first_ptr_ = 0;
  context_data_last_ptr_ = 0;
}

int XMADecoder::GetContextID(u32 context_ptr) const {
  REX_STATIC_ASSERT_SIZE(XMA_CONTEXT_DATA, 64);
  if (context_ptr < context_data_first_ptr_ || context_ptr > context_data_last_ptr_) {
    return -1;
  }
  REX_ASSERT((context_ptr & 0x3F) == 0);
  return static_cast<int>((context_ptr - context_data_first_ptr_) >> 6);
}

u32 XMADecoder::AllocateContext() {
  size_t index = context_bitmap_.AcquireEntry();
  if (index == static_cast<size_t>(-1)) {
    return 0;
  }

  XMAContext& context = contexts_[index];
  REX_ASSERT(!context.IsAllocated());
  context.SetAllocated(true);
  return context.GetGuestPtr();
}

void XMADecoder::ReleaseContext(u32 context_ptr) {
  auto context_id = GetContextID(context_ptr);
  if (context_id < 0) {
    return;
  }

  XMAContext& context = contexts_[context_id];
  if (!context.IsAllocated()) {
    return;
  }
  context.Release();
  context_bitmap_.ReleaseEntry(context_id);
}

bool XMADecoder::BlockOnContext(u32 context_ptr, bool poll) {
  auto context_id = GetContextID(context_ptr);
  if (context_id < 0) {
    return true;
  }

  XMAContext& context = contexts_[context_id];
  return context.WaitIdle(poll);
}

bool XMADecoder::SignalContext(u32 base_register, u32 context_physical_address) {
  const u32 array_base = GetContextArrayAddress();
  if (context_physical_address < array_base) {
    return false;
  }
  const u32 hw_index = (context_physical_address - array_base) / sizeof(XMA_CONTEXT_DATA);
  if (hw_index >= kContextCount) {
    return false;
  }
  WriteRegister(base_register + (hw_index >> 5) * 4, std::byteswap(1u << (hw_index & 0x1F)));
  return true;
}

u32 XMADecoder::ReadRegister(u32 addr) {
  auto r = (addr & 0xFFFF) / 4;

  REX_ASSERT(r < XMARegisterFile::kRegisterCount);

  switch (r) {
    case XMARegister::kContextArrayAddress:
      break;
    case XMARegister::kCurrentContextIndex: {
      u32& current_context_index = register_file_[XMARegister::kCurrentContextIndex];
      u32& next_context_index = register_file_[XMARegister::kNextContextIndex];
      current_context_index = next_context_index;
      next_context_index = (next_context_index + 1) % kContextCount;
      break;
    }
    default:
      const auto register_info = register_file_.GetRegisterInfo(r);
      if (register_info) {
        REXAPU_WARN_ONCE(unhandled_read_once_, r, "XMA: Read from unhandled register ({:04X}, {})",
                         r, register_info->name);
      } else {
        REXAPU_WARN_ONCE(unhandled_read_once_, r, "XMA: Read from unknown register ({:04X})", r);
      }
      break;
  }

  return std::byteswap(register_file_[r]);
}

void XMADecoder::WriteRegister(u32 addr, u32 value) {
  REX_PROFILE_SCOPE("apu");

  u32 r = (addr & 0xFFFF) / 4;
  value = std::byteswap(value);

  REX_ASSERT(r < XMARegisterFile::kRegisterCount);
  register_file_[r] = value;

  if (r >= XMARegister::Context0Kick && r <= XMARegister::Context9Kick) {
    // XMAEnableContext writes the kick registers.
    u32 base_context_id = (r - XMARegister::Context0Kick) * 32;
    u32 kicked_value = value;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        u32 context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        context.Enable();
      }
    }
    if (t_defer_kicks) {
      // Battlefield 1943 Reborn patch (bf_audio.h): decoded at EndDeferredKicks with the thread's other kicks, in
      // parallel. The worker sweep is not woken: the guest may disable and refill the context before End, and would
      // then wait for a decode already running.
      for (int i = 0; kicked_value && i < 32; ++i, kicked_value >>= 1) {
        if (kicked_value & 1) {
          const u32 context_id = base_context_id + i;
          if (std::find(t_kicked.begin(), t_kicked.end(), context_id) == t_kicked.end()) t_kicked.push_back(context_id);
        }
      }
      return;
    }
    // Decode inline, a worker sweep would stall the audio thread 50 to 100ms in kick bursts.
    for (int i = 0; kicked_value && i < 32; ++i, kicked_value >>= 1) {
      if (kicked_value & 1) {
        u32 context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        if (context.Work()) {
          context.SignalWorkDone();
        }
      }
    }
    work_event_->Set();
  } else if (r >= XMARegister::Context0Lock && r <= XMARegister::Context9Lock) {
    // XMADisableContext writes the lock registers.
    u32 base_context_id = (r - XMARegister::Context0Lock) * 32;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        u32 context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        context.Disable();
        // Block until an in flight decode finishes before the guest may modify the context.
        context.WaitIdle(false);
      }
    }
  } else if (r >= XMARegister::Context0Clear && r <= XMARegister::Context9Clear) {
    u32 base_context_id = (r - XMARegister::Context0Clear) * 32;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        u32 context_id = base_context_id + i;
        XMAContext& context = contexts_[context_id];
        context.Clear();
      }
    }
  } else {
    // Register 0x0601 receives 0x02000000 and 0x03000000 around a lock operation.
    const auto register_info = register_file_.GetRegisterInfo(r);
    if (register_info) {
      REXAPU_WARN_ONCE(unhandled_write_once_, r,
                       "XMA: Write to unhandled register ({:04X}, {}) value {:08X}", r,
                       register_info->name, value);
    } else {
      REXAPU_WARN_ONCE(unhandled_write_once_, r,
                       "XMA: Write to unknown register ({:04X}) value {:08X}", r, value);
    }
  }
}

void XMADecoder::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  if (work_event_) {
    work_event_->Set();
  }
  pause_fence_.Wait();
}

void XMADecoder::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  resume_fence_.Signal();
}

}  // namespace rex::kernel

// Battlefield 1943 Reborn patch (bf_audio.h, v0.10's PR #60): deferred kicks.
namespace rex::kernel {

void XMADecoder::BeginDeferredKicks() { t_defer_kicks = true; }

void XMADecoder::EndDeferredKicks() {
  if (!t_defer_kicks) return;
  t_defer_kicks = false;
  if (t_kicked.empty()) return;
  auto batch = std::make_shared<KickBatch>();
  batch->ids.swap(t_kicked);
  if (batch->ids.size() == 1) {
    DecodeKicked(batch->ids[0]);
    return;
  }
  std::lock_guard<std::mutex> one_at_a_time(kick_end_mutex_);
  {
    std::lock_guard<std::mutex> lock(kick_mutex_);
    if (kick_threads_.empty() && !kick_stop_) {
      for (int i = 0; i < kKickThreads; ++i) kick_threads_.emplace_back([this, i] { KickThreadMain(i); });
    }
    kick_batch_ = batch;
    kick_generation_++;
  }
  kick_cv_.notify_all();
  RunKickBatch(*batch);  // the caller decodes too
  std::unique_lock<std::mutex> lock(kick_mutex_);
  kick_done_cv_.wait(lock, [&] { return batch->done.load() == batch->ids.size(); });
}

void XMADecoder::KickThreadMain(int index) {
  rex::platform::SetCurrentThreadName("XMA Kick " + std::to_string(index));
  u64 seen = 0;
  std::unique_lock<std::mutex> lock(kick_mutex_);
  while (true) {
    kick_cv_.wait(lock, [&] { return kick_stop_ || kick_generation_ != seen; });
    if (kick_stop_) return;
    seen = kick_generation_;
    std::shared_ptr<KickBatch> batch = kick_batch_;
    lock.unlock();
    RunKickBatch(*batch);
    lock.lock();
  }
}

void XMADecoder::RunKickBatch(KickBatch& batch) {
  const u32 count = static_cast<u32>(batch.ids.size());
  for (u32 i; (i = batch.next.fetch_add(1)) < count;) {
    DecodeKicked(batch.ids[i]);
    if (batch.done.fetch_add(1) + 1 == count) {
      std::lock_guard<std::mutex> lock(kick_mutex_);  // the caller checks done under this lock
      kick_done_cv_.notify_all();
    }
  }
}

// What an inline kick does, then waiting for any pass still running on the context (the worker sweep may have taken
// it first; Work returns at once when the context is no longer enabled).
void XMADecoder::DecodeKicked(u32 context_id) {
  XMAContext& context = contexts_[context_id];
  if (context.Work()) {
    context.SignalWorkDone();
  }
  context.WaitIdle(false);
}

}  // namespace rex::kernel

namespace rex::kernel::bf {
void BeginDeferredXmaKicks() {
  if (g_decoder) g_decoder->BeginDeferredKicks();
}
void EndDeferredXmaKicks() {
  if (g_decoder) g_decoder->EndDeferredKicks();
}
}  // namespace rex::kernel::bf
