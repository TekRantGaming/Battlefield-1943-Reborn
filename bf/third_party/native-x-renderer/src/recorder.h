// NativeXRenderer - the recorder: the immediate context's calls of the draw path, made on a thread of their own in the
// order they were recorded (internal).

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "Buffer.h"
#include "DeviceContext.h"

namespace native_x_renderer {

class Recorder {
 public:
  ~Recorder() { Stop(); }
  void Initialize(Diligent::IDeviceContext* context);
  void Start();
  void Stop();
  bool threaded() const { return thread_.joinable(); }

  // The context for direct use: what was recorded is done first.
  Diligent::IDeviceContext* context() {
    if (threaded() && std::this_thread::get_id() != thread_id_) Drain();
    return context_;
  }

  // Until EndInline, calls are made at once, on the calling thread, after what was recorded before.
  void BeginInline() {
    Drain();
    inline_ = true;
  }
  void EndInline() { inline_ = false; }

  // `f(context)`, after what was recorded before it. Its captures are destroyed after it ran.
  template <typename F>
  void Do(F&& f) {
    if (inline_ || !threaded()) {
      f(context_);
      if (!threaded()) Consume();
      return;
    }
    using T = std::decay_t<F>;
    static_assert(alignof(T) <= kAlign);
    new (Allocate(sizeof(T), [](void* p, Diligent::IDeviceContext* c) {
      T* t = static_cast<T*>(p);
      (*t)(c);
      t->~T();
    })) T(std::forward<F>(f));
  }
  // Memory for the data of the calls recorded next.
  uint8_t* Bytes(size_t size) { return static_cast<uint8_t*>(Allocate(size, nullptr)); }
  void UpdateBuffer(Diligent::IBuffer* buffer, uint64_t offset, uint64_t size, const void* data);

  // D229: the race check's view (NXR_RACE_CHECK, device.cpp): the thread has run all it was handed and runs nothing
  // now; the caller is the thread; the context without draining (the check's self-test only).
  bool Idle() const {
    return !running_.load(std::memory_order_acquire) &&
           executed_.load(std::memory_order_acquire) >= published_.load(std::memory_order_acquire);
  }
  bool OnThread() const { return threaded() && std::this_thread::get_id() == thread_id_; }
  Diligent::IDeviceContext* raw_context() { return context_; }

  // Hands what was recorded to the thread.
  void Publish();
  // Publishes and waits until it is done.
  void Drain();

 private:
  static constexpr size_t kAlign = 16;
  static constexpr uint32_t kBlockSize = 4u << 20;
  using RunFn = void (*)(void*, Diligent::IDeviceContext*);
  struct Header {
    RunFn run;
    uint32_t size;
  };
  struct Block {
    explicit Block(uint32_t capacity) : capacity(capacity), data(new (std::align_val_t(kAlign)) uint8_t[capacity]) {}
    ~Block() { ::operator delete[](data, std::align_val_t(kAlign)); }
    const uint32_t capacity;
    uint8_t* const data;
    std::atomic<uint32_t> published{0};
    std::atomic<Block*> next{nullptr};
  };

  void* Allocate(size_t size, RunFn run);
  Block* NewBlock(uint32_t size);
  void Recycle(Block* block);
  void Consume();
  void Run();

  Diligent::IDeviceContext* context_ = nullptr;
  std::thread thread_;
  std::thread::id thread_id_;
  std::atomic<bool> stop_{false};
  bool inline_ = false;

  Block* write_block_ = nullptr;
  uint32_t write_pos_ = 0;
  uint64_t recorded_ = 0;
  std::atomic<uint64_t> published_{0};
  // D250: set while the recorder's thread sleeps on published_: only then does Publish wake it.
  std::atomic<bool> consumer_sleeping_{false};

  Block* read_block_ = nullptr;
  Block* retired_ = nullptr;
  uint32_t read_pos_ = 0;
  uint64_t run_ = 0;
  std::atomic<uint64_t> executed_{0};
  std::atomic<bool> drain_waiting_{false};
  std::atomic<bool> running_{false};  // D229: a call is running on the thread

  std::mutex free_mutex_;
  std::vector<Block*> free_;
};

}  // namespace native_x_renderer
