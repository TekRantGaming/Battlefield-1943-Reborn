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

#include <algorithm>
#include <condition_variable>
#include <forward_list>
#include <mutex>

#include <disruptorplus/multi_threaded_claim_strategy.hpp>
#include <disruptorplus/ring_buffer.hpp>
#include <disruptorplus/sequence_barrier.hpp>
#include <disruptorplus/spin_wait_strategy.hpp>

#include <rex/assert.h>
#include <rex/platform.h>
#include <rex/thread.h>
#include <rex/thread/timer_queue.h>

#if REX_PLATFORM_WIN32
#include "platform_win.h"
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace dp = disruptorplus;

namespace rex::thread {

using WaitItem = TimerQueueWaitItem;

class TimerQueue {
 public:
  using clock = WaitItem::clock;
  static_assert(clock::is_steady);

 public:
  TimerQueue()
      : buffer_(kWaitCount),
        wait_strategy_(),
        claim_strategy_(kWaitCount, wait_strategy_),
        consumed_(wait_strategy_) {
    claim_strategy_.add_claim_barrier(consumed_);
    dispatch_thread_ =
        std::jthread([this](std::stop_token stop_token) { TimerThreadMain(stop_token); });
  }

  ~TimerQueue() {
    dispatch_thread_.request_stop();

    // Kick dispatch thread to check stop token
    auto wait_item = std::make_shared<WaitItem>(nullptr, nullptr, this, clock::time_point::min(),
                                                clock::duration::zero());
    wait_item->Disarm();
    QueueTimer(std::move(wait_item));

    // std::jthread auto-joins on destruction
  }

  void TimerThreadMain(std::stop_token stop_token) {
    dp::sequence_t next_sequence = 0;
    const auto comp = [](const std::shared_ptr<WaitItem>& left,
                         const std::shared_ptr<WaitItem>& right) {
      return left->due_ < right->due_;
    };

    set_current_thread_name("rex::thread::TimerQueue");

    while (!stop_token.stop_requested()) {
      {
        // Consume new wait items and add them to sorted wait queue
#if REX_PLATFORM_WIN32
        // Battlefield 1943 Reborn, 14 Nov 2026: the spin wait strategy's wait (yields in a loop, a 1 ms sleep every
        // 20th) kept this thread busy between the 1 ms timestamp ticks, and on a PC with few cores took them from
        // the game's threads. It sleeps on the queue's event (QueueTimer) and a high resolution timer set for the
        // next due item instead, as the POSIX path below sleeps on its condition variable.
        // The ring is polled only when the event says something was queued: a poll that finds nothing spins through
        // the spin_wait's busy loops (thousands of pause instructions) and asks the system for the processor count
        // (std::thread::hardware_concurrency), which on the 1 ms timestamp timer was most of an idle server's CPU.
        bool queued = false;
        if (wait_queue_.empty()) {
          queued = WaitForSingleObject(wake_event_, INFINITE) == WAIT_OBJECT_0;
        } else {
          const auto due = wait_queue_.front()->due_;
          const auto now = clock::now();
          if (due > now) {
            const auto ticks = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
                                   due - now)
                                   .count();
            LARGE_INTEGER relative;
            relative.QuadPart = -std::max<int64_t>(1, ticks);
            if (due_timer_ && SetWaitableTimer(due_timer_, &relative, 0, nullptr, nullptr, FALSE)) {
              const HANDLE handles[2] = {wake_event_, due_timer_};
              queued = WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0;
            } else {
              queued = WaitForSingleObject(wake_event_, DWORD(std::max<int64_t>(1, (ticks + 9999) / 10000))) ==
                       WAIT_OBJECT_0;
            }
          } else {
            queued = WaitForSingleObject(wake_event_, 0) == WAIT_OBJECT_0;
          }
        }
        dp::sequence_t available =
            queued ? claim_strategy_.wait_until_published(next_sequence, next_sequence - 1,
                                                          clock::time_point::min())
                   : next_sequence - 1;
#else
        // Sleep until the next timer is due or a new one is queued. The spin wait strategy's
        // wait yielded in a loop and kept this thread at about a fifth of a CPU core even with
        // nothing due (task 085, ROG Ally). The ring is only polled (a past timeout) after
        // QueueTimer woke the thread; an item another producer has not published yet is
        // picked up at that producer's own wake-up.
        bool queued;
        {
          std::unique_lock<std::mutex> lock(wake_mutex_);
          const auto woken = [this] { return wake_pending_; };
          if (wait_queue_.empty()) {
            wake_cv_.wait(lock, woken);
          } else {
            wake_cv_.wait_until(lock, wait_queue_.front()->due_, woken);
          }
          queued = wake_pending_;
          wake_pending_ = false;
        }
        dp::sequence_t available =
            queued ? claim_strategy_.wait_until_published(next_sequence, next_sequence - 1,
                                                          clock::time_point::min())
                   : next_sequence - 1;
#endif

        // Check for timeout
        if (available != next_sequence - 1) {
          std::forward_list<std::shared_ptr<WaitItem>> wait_items;
          do {
            wait_items.push_front(std::move(buffer_[next_sequence]));
          } while (next_sequence++ != available);

          consumed_.publish(available);

          wait_items.sort(comp);
          wait_queue_.merge(wait_items, comp);
        }
      }

      {
        // Check wait queue, invoke callbacks and reschedule
        std::forward_list<std::shared_ptr<WaitItem>> wait_items;
        while (!wait_queue_.empty() && wait_queue_.front()->due_ <= clock::now()) {
          auto wait_item = std::move(wait_queue_.front());
          wait_queue_.pop_front();

          // Ensure that it isn't disarmed
          auto state = WaitItem::State::kIdle;
          if (wait_item->state_.compare_exchange_strong(state, WaitItem::State::kInCallback,
                                                        std::memory_order_acq_rel)) {
            // Possibility to dispatch to a thread pool here
            assert_not_null(wait_item->callback_);
            wait_item->callback_(wait_item->userdata_);

            if (wait_item->interval_ != clock::duration::zero() &&
                wait_item->state_.load(std::memory_order_acquire) !=
                    WaitItem::State::kInCallbackSelfDisarmed) {
              // Item is recurring and didn't self-disarm during callback:
              wait_item->due_ += wait_item->interval_;
              wait_item->state_.store(WaitItem::State::kIdle, std::memory_order_release);
              wait_item->state_.notify_all();
              wait_items.push_front(std::move(wait_item));
            } else {
              wait_item->state_.store(WaitItem::State::kDisarmed, std::memory_order_release);
              wait_item->state_.notify_all();
            }
          } else {
            // Specifically, kInCallback is illegal here
            assert_true(WaitItem::State::kDisarmed == state);
          }
        }
        wait_items.sort(comp);
        wait_queue_.merge(wait_items, comp);
      }
    }
  }

  std::weak_ptr<WaitItem> QueueTimer(std::shared_ptr<WaitItem> wait_item) {
    auto wait_item_weak = std::weak_ptr<WaitItem>(wait_item);

    // Mitigate callback flooding
    wait_item->due_ = std::max(clock::now() - wait_item->interval_, wait_item->due_);

    auto sequence = claim_strategy_.claim_one();
    buffer_[sequence] = std::move(wait_item);
    claim_strategy_.publish(sequence);
#if REX_PLATFORM_WIN32
    SetEvent(wake_event_);
#else
    {
      std::lock_guard<std::mutex> lock(wake_mutex_);
      wake_pending_ = true;
    }
    wake_cv_.notify_one();
#endif

    return wait_item_weak;
  }

  std::jthread::id dispatch_thread_id() const { return dispatch_thread_.get_id(); }

 private:
  // This ring buffer will be used to introduce timers queued by the public API
  static constexpr size_t kWaitCount = 512;
  dp::ring_buffer<std::shared_ptr<WaitItem>> buffer_;
  dp::spin_wait_strategy wait_strategy_;
  dp::multi_threaded_claim_strategy<dp::spin_wait_strategy> claim_strategy_;
  dp::sequence_barrier<dp::spin_wait_strategy> consumed_;

  // This is a _sorted_ (ascending due_) list of active timers managed by a
  // dedicated thread
  std::forward_list<std::shared_ptr<WaitItem>> wait_queue_;
#if REX_PLATFORM_WIN32
  // Wakes the dispatch thread when a timer is queued; the timer wakes it when the next one is due (TimerThreadMain).
  // Made before dispatch_thread_ starts (members are made in declaration order).
  HANDLE wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HANDLE due_timer_ = [] {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    return timer ? timer : CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  }();
#else
  // Wakes the dispatch thread when a timer is queued (see TimerThreadMain).
  std::mutex wake_mutex_;
  std::condition_variable wake_cv_;
  bool wake_pending_ = false;
#endif
  std::jthread dispatch_thread_;
};

rex::thread::TimerQueue timer_queue_;

void TimerQueueWaitItem::Disarm() {
  State state;

  // Special case for calling from a callback itself
  if (std::this_thread::get_id() == parent_queue_->dispatch_thread_id()) {
    state = State::kInCallback;
    if (state_.compare_exchange_strong(state, State::kInCallbackSelfDisarmed,
                                       std::memory_order_acq_rel)) {
      // If we are self disarming from the callback set this special state and
      // exit
      return;
    }
    // Normal case can handle the rest
  }

  state = State::kIdle;
  // Classes which hold WaitItems will often call Disarm() to cancel them during
  // destruction. This may lead to race conditions when the dispatch thread
  // executes a callback which accesses memory that is freed simultaneously due
  // to this. Therefore, we need to guarantee that no callbacks will be running
  // once Disarm() has returned.
  while (!state_.compare_exchange_weak(state, State::kDisarmed, std::memory_order_acq_rel)) {
    if (state == State::kDisarmed) {
      break;
    }
    if (state == State::kInCallback || state == State::kInCallbackSelfDisarmed) {
      // Wait for callback to complete - dispatch thread will notify
      state_.wait(state, std::memory_order_acquire);
    }
    state = State::kIdle;
  }
}

std::weak_ptr<WaitItem> QueueTimerOnce(std::function<void(void*)> callback, void* userdata,
                                       WaitItem::clock::time_point due) {
  return timer_queue_.QueueTimer(std::make_shared<WaitItem>(
      std::move(callback), userdata, &timer_queue_, due, WaitItem::clock::duration::zero()));
}

std::weak_ptr<WaitItem> QueueTimerRecurring(std::function<void(void*)> callback, void* userdata,
                                            WaitItem::clock::time_point due,
                                            WaitItem::clock::duration interval) {
  return timer_queue_.QueueTimer(
      std::make_shared<WaitItem>(std::move(callback), userdata, &timer_queue_, due, interval));
}

}  // namespace rex::thread
