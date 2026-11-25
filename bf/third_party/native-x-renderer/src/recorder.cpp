// NativeXRenderer - see recorder.h.

#include "recorder.h"

#include <rex/thread.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

#include "cpu_cores.h"

namespace native_x_renderer {
void Recorder::Initialize(Diligent::IDeviceContext* context) {
  context_ = context;
  if (!write_block_) write_block_ = read_block_ = NewBlock(kBlockSize);
}

void Recorder::Start() {
  if (threaded() || !write_block_) return;
  stop_.store(false, std::memory_order_relaxed);
  thread_ = std::thread([this] {
    rex::thread::set_current_thread_name("NXRRecorder");
    Run();
  });
  thread_id_ = thread_.get_id();
}

void Recorder::Stop() {
  if (!threaded()) return;
  Drain();
  Do([](Diligent::IDeviceContext*) {});
  stop_.store(true, std::memory_order_release);
  Publish();
  thread_.join();
}

void* Recorder::Allocate(size_t size, RunFn run) {
  static_assert(sizeof(Header) == kAlign);
  const uint32_t need = uint32_t(sizeof(Header) + ((size + kAlign - 1) & ~(kAlign - 1)));
  if (write_pos_ + need > write_block_->capacity) {
    Block* block = NewBlock(std::max(kBlockSize, need));
    write_block_->published.store(write_pos_, std::memory_order_release);
    write_block_->next.store(block, std::memory_order_release);
    write_block_ = block;
    write_pos_ = 0;
  }
  auto* header = reinterpret_cast<Header*>(write_block_->data + write_pos_);
  header->run = run;
  header->size = need;
  write_pos_ += need;
  if (run) ++recorded_;
  return header + 1;
}

Recorder::Block* Recorder::NewBlock(uint32_t size) {
  if (size == kBlockSize) {
    std::lock_guard lock(free_mutex_);
    if (!free_.empty()) {
      Block* block = free_.back();
      free_.pop_back();
      block->published.store(0, std::memory_order_relaxed);
      block->next.store(nullptr, std::memory_order_relaxed);
      return block;
    }
  }
  return new Block(size);
}

void Recorder::Recycle(Block* block) {
  if (block->capacity == kBlockSize) {
    std::lock_guard lock(free_mutex_);
    if (free_.size() < 8) return free_.push_back(block);
  }
  delete block;
}

void Recorder::UpdateBuffer(Diligent::IBuffer* buffer, uint64_t offset, uint64_t size, const void* data) {
  uint8_t* copy = Bytes(size);
  std::memcpy(copy, data, size);
  Do([=](Diligent::IDeviceContext* c) {
    c->UpdateBuffer(buffer, offset, size, copy, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  });
}

void Recorder::Publish() {
  write_block_->published.store(write_pos_, std::memory_order_release);
  if (!threaded()) return Consume();
  // D250: woken only while it sleeps (a wake is a call into the system, at every publish it was 1 % of the renderer's
  // thread). Sequentially consistent with Run's sleep: one of the two sees the other.
  published_.store(recorded_, std::memory_order_seq_cst);
  if (consumer_sleeping_.load(std::memory_order_seq_cst)) published_.notify_one();
}

void Recorder::Drain() {
  Publish();
  const uint64_t target = recorded_;
  if (executed_.load(std::memory_order_acquire) >= target) return;
  const uint32_t spins = SpinWaitsAllowed() ? 4096 : 0;  // cpu_cores.h
  for (uint32_t i = 0; i < spins && executed_.load(std::memory_order_acquire) < target; ++i) Pause();
  drain_waiting_.store(true, std::memory_order_seq_cst);
  for (uint64_t e; (e = executed_.load(std::memory_order_seq_cst)) < target;) executed_.wait(e, std::memory_order_acquire);
  drain_waiting_.store(false, std::memory_order_relaxed);
}

// A block is reused once the one after it is done too: the data of a call may be in the block before the call's.
void Recorder::Consume() {
  for (;;) {
    const uint32_t end = read_block_->published.load(std::memory_order_acquire);
    while (read_pos_ < end) {
      auto* header = reinterpret_cast<Header*>(read_block_->data + read_pos_);
      read_pos_ += header->size;
      if (!header->run) continue;
      running_.store(true, std::memory_order_release);
      header->run(header + 1, context_);
      running_.store(false, std::memory_order_release);
      executed_.store(++run_, std::memory_order_seq_cst);
      if (drain_waiting_.load(std::memory_order_seq_cst)) executed_.notify_all();
    }
    Block* next = read_block_->next.load(std::memory_order_acquire);
    if (!next) return;
    if (read_pos_ < read_block_->published.load(std::memory_order_acquire)) continue;
    if (retired_) Recycle(retired_);
    retired_ = read_block_;
    read_block_ = next;
    read_pos_ = 0;
  }
}

void Recorder::Run() {
  for (;;) {
    const uint64_t seen = published_.load(std::memory_order_acquire);
    Consume();
    if (run_ < seen) continue;
    if (stop_.load(std::memory_order_acquire)) break;
    // NXR_RECORDER_SPINS=<n> (a diagnosis aid, D250): the spin's length (2048 pauses).
    static const uint32_t spin_length = [] {
      const char* v = std::getenv("NXR_RECORDER_SPINS");
      return v && *v ? uint32_t(std::atoi(v)) : 2048u;
    }();
    const uint32_t spins = SpinWaitsAllowed() ? spin_length : 0;  // cpu_cores.h
    for (uint32_t i = 0; i < spins && published_.load(std::memory_order_acquire) == seen; ++i) Pause();
    consumer_sleeping_.store(true, std::memory_order_seq_cst);
    if (published_.load(std::memory_order_seq_cst) == seen) published_.wait(seen, std::memory_order_acquire);
    consumer_sleeping_.store(false, std::memory_order_relaxed);
  }
}

}  // namespace native_x_renderer
