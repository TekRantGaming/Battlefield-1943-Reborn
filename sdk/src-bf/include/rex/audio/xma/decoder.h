/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/register_file.h>
#include <rex/bit.h>
#include <rex/kernel.h>
#include <rex/system/xthread.h>

namespace rex::runtime {
class FunctionDispatcher;
}  // namespace rex::runtime

namespace rex::audio {

struct XMA_CONTEXT_DATA;

// Deferred kicks, for a game whose audio thread starts many sounds at once. The console decoded XMA in hardware beside
// the CPU, so a kick (XMAEnableContext) cost the game nothing; here each kick decodes inline on the kicking thread
// (a new sound decodes several frames up front). EA's mixer in BF1943 starts up to 48 sounds in one step, which then
// runs past its ~10 ms of buffered audio. Between Begin and End, kicks made on the calling thread only enable their contexts; End decodes
// them all in parallel (audio_xma_kick_threads workers plus the caller) and returns once they are done, so the guest
// finds them decoded the next time it looks after End, as it would on the console. Kicks on other threads, and on
// this thread outside Begin/End, decode inline as before. With audio_xma_kick_threads = 0 Begin/End do nothing.
void BeginDeferredXmaKicks();
void EndDeferredXmaKicks();

class XmaDecoder {
 public:
  explicit XmaDecoder(runtime::FunctionDispatcher* function_dispatcher);
  ~XmaDecoder();

  memory::Memory* memory() const { return memory_; }
  runtime::FunctionDispatcher* function_dispatcher() const { return function_dispatcher_; }

  X_STATUS Setup(system::KernelState* kernel_state);
  void Shutdown();

  uint32_t context_array_ptr() const { return register_file_[XmaRegister::ContextArrayAddress]; }

  uint32_t AllocateContext();
  void ReleaseContext(uint32_t guest_ptr);
  bool BlockOnContext(uint32_t guest_ptr, bool poll);

  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

  // See BeginDeferredXmaKicks above.
  void BeginDeferredKicks();
  void EndDeferredKicks();

 protected:
  int GetContextId(uint32_t guest_ptr);

 private:
  void WorkerThreadMain();

  struct KickBatch {
    std::vector<uint32_t> ids;
    std::atomic<uint32_t> next{0}, done{0};
  };
  void KickThreadMain(int index);
  void RunKickBatch(KickBatch& batch);
  void DecodeKicked(uint32_t context_id);

  static uint32_t MMIOReadRegisterThunk(void* ppc_context, XmaDecoder* as, uint32_t addr) {
    return as->ReadRegister(addr);
  }
  static void MMIOWriteRegisterThunk(void* ppc_context, XmaDecoder* as, uint32_t addr,
                                     uint32_t value) {
    as->WriteRegister(addr, value);
  }

 protected:
  memory::Memory* memory_ = nullptr;
  runtime::FunctionDispatcher* function_dispatcher_ = nullptr;

  std::atomic<bool> worker_running_ = {false};
  system::object_ref<system::XHostThread> worker_thread_;
  std::unique_ptr<rex::thread::Event> work_event_ = nullptr;

  std::atomic<bool> paused_ = false;
  rex::thread::Fence pause_fence_;   // Signaled when worker paused.
  rex::thread::Fence resume_fence_;  // Signaled when resume requested.

  XmaRegisterFile register_file_;

  static const uint32_t kContextCount = 320;
  XmaContext contexts_[kContextCount];
  bit::BitMap context_bitmap_;

  uint32_t context_data_first_ptr_ = 0;
  uint32_t context_data_last_ptr_ = 0;

  // Deferred kicks: worker threads (started on the first batch) and the batch they share.
  std::mutex kick_mutex_;  // guards kick_batch_, kick_generation_, kick_stop_; the done wait
  std::condition_variable kick_cv_, kick_done_cv_;
  std::shared_ptr<KickBatch> kick_batch_;
  uint64_t kick_generation_ = 0;
  bool kick_stop_ = false;
  std::vector<std::thread> kick_threads_;
  std::mutex kick_end_mutex_;  // one End at a time
};

}  // namespace rex::audio
