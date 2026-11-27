// NativeXRenderer - see commands.h.

#include "commands.h"

#include <rex/logging.h>
#include <rex/thread.h>

#include <chrono>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

#include "cpu_cores.h"

namespace native_x_renderer {
namespace {

using Clock = std::chrono::steady_clock;

// Waits on a condition variable are bounded: a missed wake-up costs at most this long.
constexpr auto kWaitSlice = std::chrono::milliseconds(5);

}  // namespace

CommandQueue::CommandQueue() = default;

CommandQueue::~CommandQueue() { Stop(); }

void CommandQueue::Start(bool threaded, Executor execute, Poller poll) {
  slots_.resize(kSlots);
  execute_ = execute;
  poll_ = poll;
  threaded_ = threaded;
  if (!threaded) return;
  running_.store(true, std::memory_order_release);
  thread_ = std::thread([this] { Run(); });
}

void CommandQueue::Stop() {
  if (!thread_.joinable()) return;
  stop_.store(true, std::memory_order_seq_cst);
  bool finished;
  {
    std::unique_lock lock(mutex_);
    produced_cv_.notify_all();
    finished = consumed_cv_.wait_for(lock, std::chrono::seconds(1), [this] { return finished_; });
  }
  // A thread stuck in the driver at exit is left to the process's end.
  if (finished) {
    thread_.join();
  } else {
    thread_.detach();
  }
}

Command& CommandQueue::Next() {
  uint64_t produced = produced_.load(std::memory_order_relaxed);
  if (threaded_ && produced - consumed_seen_ >= kSlots) {
    consumed_seen_ = consumed_.load(std::memory_order_acquire);
    if (produced - consumed_seen_ >= kSlots) {
      WaitConsumed(produced - kSlots + 1, producer_waits_, producer_wait_ns_);
      consumed_seen_ = consumed_.load(std::memory_order_acquire);
    }
  }
  Command& command = slots_[produced % kSlots];
  command.Reset();
  return command;
}

void CommandQueue::Publish() {
  uint64_t produced = produced_.load(std::memory_order_relaxed);
  if (!threaded_) {
    execute_(slots_[produced % kSlots]);
    produced_.store(produced + 1, std::memory_order_relaxed);
    consumed_.store(produced + 1, std::memory_order_relaxed);
    return;
  }
  // Sequentially consistent with the renderer's thread going to sleep (Run): one of the two sees the other.
  produced_.store(produced + 1, std::memory_order_seq_cst);
  if (consumer_sleeping_.load(std::memory_order_seq_cst)) {
    std::lock_guard lock(mutex_);
    produced_cv_.notify_one();
  }
}

void CommandQueue::Wake() {
  if (!threaded_) return;
  wake_requested_.store(true, std::memory_order_seq_cst);
  if (consumer_sleeping_.load(std::memory_order_seq_cst)) {
    std::lock_guard lock(mutex_);
    produced_cv_.notify_one();
  }
}

const Command* CommandQueue::Peek(uint32_t ahead) const {
  if (!threaded_ || ahead >= kSlots) return nullptr;
  uint64_t index = running_index_ + ahead;
  if (index >= produced_.load(std::memory_order_acquire)) return nullptr;
  return &slots_[index % kSlots];
}

void CommandQueue::WaitIdle(uint64_t limit) {
  if (!threaded_) return;
  WaitConsumed(std::min(limit, produced_.load(std::memory_order_seq_cst)), gate_waits_, gate_wait_ns_);
}

void CommandQueue::WaitConsumed(uint64_t target, std::atomic<uint64_t>& waits, std::atomic<uint64_t>& wait_ns) {
  if (consumed_.load(std::memory_order_acquire) >= target || !running_.load(std::memory_order_acquire)) return;
  auto start = Clock::now();
  // The thread is usually a few commands away: spin briefly before sleeping (with enough cores: cpu_cores.h).
  const uint32_t spins = SpinWaitsAllowed() ? 4096 : 0;
  for (uint32_t i = 0; i < spins && consumed_.load(std::memory_order_acquire) < target; ++i) Pause();
  if (consumed_.load(std::memory_order_acquire) < target) {
    std::unique_lock lock(mutex_);
    while (running_.load(std::memory_order_acquire)) {
      // Ask the thread to wake us at `target`, then check: sequentially consistent with its store of
      // `consumed_` and its load of `wake_at_` (Run), so one of the two sees the other.
      uint64_t wake_at = wake_at_.load(std::memory_order_seq_cst);
      while (target < wake_at && !wake_at_.compare_exchange_weak(wake_at, target, std::memory_order_seq_cst)) {
      }
      if (consumed_.load(std::memory_order_seq_cst) >= target) break;
      consumed_cv_.wait_for(lock, kWaitSlice);
    }
  }
  const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
  waits.fetch_add(1, std::memory_order_relaxed);
  wait_ns.fetch_add(ns, std::memory_order_relaxed);
  // The diagnostics' running totals (never taken: the per-frame record reads their difference).
  (&wait_ns == &producer_wait_ns_ ? producer_total_ns_ : gate_total_ns_).fetch_add(ns, std::memory_order_relaxed);
}

CommandQueue::Waits CommandQueue::Take(std::atomic<uint64_t>& waits, std::atomic<uint64_t>& wait_ns) {
  Waits result;
  result.count = waits.exchange(0, std::memory_order_relaxed);
  result.ms = double(wait_ns.exchange(0, std::memory_order_relaxed)) / 1e6;
  return result;
}

void RaiseCurrentThreadPriority() {
#if defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
}

void CommandQueue::Complete(uint64_t consumed) {
  consumed_.store(consumed, std::memory_order_seq_cst);
  if (consumed >= wake_at_.load(std::memory_order_seq_cst)) {
    std::lock_guard lock(mutex_);
    wake_at_.store(UINT64_MAX, std::memory_order_seq_cst);
    consumed_cv_.notify_all();
  }
}

void CommandQueue::Run() {
  rex::thread::set_current_thread_name("NativeXRenderer");
  if (raise_priority_) RaiseCurrentThreadPriority();  // D136
  uint64_t consumed = consumed_.load(std::memory_order_relaxed);
  while (!stop_.load(std::memory_order_acquire)) {
    if (produced_.load(std::memory_order_acquire) == consumed) {
      // Empty: spin briefly (the game's thread is usually inside its next call), then sleep (spin only with enough
      // cores: cpu_cores.h).
      const uint32_t spins = SpinWaitsAllowed() ? 1024 : 0;
      for (uint32_t i = 0; i < spins && produced_.load(std::memory_order_acquire) == consumed; ++i) Pause();
      if (produced_.load(std::memory_order_acquire) == consumed) {
        std::unique_lock lock(mutex_);
        consumer_sleeping_.store(true, std::memory_order_seq_cst);
        // Sequentially consistent with Wake: one of the two sees the other.
        while (produced_.load(std::memory_order_seq_cst) == consumed && !stop_.load(std::memory_order_acquire) &&
               !wake_requested_.load(std::memory_order_seq_cst)) {
          produced_cv_.wait_for(lock, kWaitSlice);
          if (poll_) break;  // the poller also runs every slice
        }
        consumer_sleeping_.store(false, std::memory_order_relaxed);
      }
      if (poll_) {
        wake_requested_.store(false, std::memory_order_relaxed);
        poll_();
      }
      continue;
    }
    running_index_ = consumed;
    execute_(slots_[consumed % kSlots]);
    ++consumed;
    Complete(consumed);
  }
  std::lock_guard lock(mutex_);
  running_.store(false, std::memory_order_release);
  finished_ = true;
  consumed_cv_.notify_all();
}

}  // namespace native_x_renderer
