// Developer aid (BF_DEV_TOOLS builds): BF_DEV_STACKS="45,55" logs every guest thread's registers and guest call stack
// at those seconds after start, to find where a thread is stuck. Each thread's PPCContext is read while it runs, so
// a sample can be torn; take two and compare. Stack frames: [r1] is the caller's r1, the saved LR is at back - 8.

#if defined(BF_DEV_TOOLS)

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>

#include "dev_guest_memory.h"

namespace bf1943 {
namespace {

void DumpThreads() {
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel) return;
  const uint8_t* base = REX_KERNEL_MEMORY()->virtual_membase();
  for (auto& thread : REX_KERNEL_OBJECTS()->GetObjectsByType<rex::system::XThread>()) {
    auto* ts = thread->thread_state();
    PPCContext* ctx = ts ? ts->context() : nullptr;
    if (!ctx) continue;
    std::string stack;
    uint32_t sp = ctx->r1.u32;
    for (int i = 0; i < 20; ++i) {
      const uint32_t back = dev::SafeLoad32(base, sp);
      if (back <= sp || back - sp > 0x100000) break;
      stack += fmt::format(" {:08X}", dev::SafeLoad32(base, back - 8));
      sp = back;
    }
    // The handle (the runtime's "on thread 0x..." in an access violation's line), the start address and the context
    // it was given, to tell the game's worker threads apart.
    REXLOG_INFO("BF STACKS thread {}{}{} handle={:08X} start={:08X}({:08X}) '{}' lr={:08X} ctr={:08X} r1={:08X} "
                "r3={:08X}:{}",
                thread->thread_id(), thread->main_thread() ? " main" : "", thread->is_guest_thread() ? " guest" : "",
                thread->handle(), thread->creation_params()->start_address, thread->creation_params()->start_context,
                thread->name(), static_cast<uint32_t>(ctx->lr), ctx->ctr.u32, ctx->r1.u32, ctx->r3.u32, stack);
  }
}

}  // namespace

void StartStackSampler() {
  const char* v = std::getenv("BF_DEV_STACKS");
  if (!v || !*v) return;
  std::vector<int> times;
  for (const char* p = v; *p;) {
    times.push_back(std::atoi(p));
    while (*p && *p != ',') ++p;
    if (*p == ',') ++p;
  }
  std::thread([times] {
    const auto start = std::chrono::steady_clock::now();
    for (int t : times) {
      std::this_thread::sleep_until(start + std::chrono::seconds(t));
      REXLOG_INFO("BF STACKS sample at {} s", t);
      DumpThreads();
    }
  }).detach();
}

}  // namespace bf1943

#endif  // BF_DEV_TOOLS
