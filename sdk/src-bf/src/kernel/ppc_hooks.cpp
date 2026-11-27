#include "kernel/ppc_hooks.h"

#include <atomic>
#include <mutex>
#include <optional>

#include <rex/assert.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/heap.h>
#include <rex/logging/log_category.h>
#include <rex/ppc/context.h>
#include <rex/ppc/hook.h>
#include <rex/ppc/kernel_hook.h>
#include <rex/ppc/recompiled_code.h>

#include "kernel/settings_access.h"
#include "kernel/title_state.h"

namespace rex::kernel {

namespace {

ppc::BoundThread ReadBoundThread() {
  const ThreadState* state = ThreadState::Get();
  if (!state) {
    return {nullptr, nullptr};
  }
  GuestAddressSpace* memory = state->GetAddressSpace();
  return {state->GetContext(), memory ? memory->GetVirtualMembase() : nullptr};
}

FunctionDispatcher* GetBoundFunctionDispatcher() {
  ThreadState* state = ThreadState::Get();
  TitleRuntime* instance = state != nullptr ? state->GetRuntime() : nullptr;
  return instance != nullptr ? StateOf(*instance).GetFunctionDispatcher() : nullptr;
}

// Battlefield 1943 Reborn patch (v0.10's guest_invalid_call_skip; TekRantGaming, 15 Nov 2026): a dedicated server
// stopped with every player in it each time the game called through a broken function pointer (null, a freed object's,
// a stack address): official servers 13 to 15 Nov, targets 0x00000000, 0x1F993A61, 0x1F8E9ED0, 0x7018E740. With
// runtime.guest_invalid_call_skip the call is skipped instead (it returns 0), logged with its caller, as the game's null
// page reads give zeros.
std::atomic<u32> g_invalid_calls_skipped{0};

void SkipInvalidCall(PPCContext& ctx, u8* /*base*/) {
  const u32 n = ++g_invalid_calls_skipped;
  // The first 20, then at each power of two: a call made every frame does not fill the log.
  if (n <= 20 || (n & (n - 1)) == 0) {
    REXCPU_ERROR("Call to invalid or unregistered function at guest address 0x{:08X} skipped (caller's return "
                 "address 0x{:08X}); {} skipped so far",
                 ctx.last_indirect_target, static_cast<u32>(ctx.lr), n);
  }
  ctx.r3.u64 = 0;
}

PPCFunc* ResolveBoundFunction(u32 guest_address) {
  FunctionDispatcher* dispatcher = GetBoundFunctionDispatcher();
  PPCFunc* func = dispatcher ? dispatcher->GetFunction(guest_address) : nullptr;
  if (!func && dispatcher && GetRuntimeSettings().guest_invalid_call_skip.Get()) {
    return &SkipInvalidCall;
  }
  return func;
}

#ifndef NDEBUG
void CheckHookRegisterFile(const PPCContext& ctx, const char* name) {
  const auto* thread_state = ThreadState::Get();
  if (thread_state && &ctx != thread_state->GetContext()) {
    REXCPU_ERROR("Hook {} runs on a register file that is not its thread's", name);
  }
  REX_ASSERT(!thread_state || &ctx == thread_state->GetContext(),
             "A hook runs on a register file that is not its thread's");
  REX_ASSERT(!thread_state || ctx.thread_state == thread_state,
             "A hook runs on a register file another thread state owns");
}
#endif

void CheckKernelDetourLocalWrites(const char* name, const PPCContext& at_entry,
                                  const PPCContext& at_exit, std::atomic_flag& warned) {
  const ThreadState* state = ThreadState::Get();
  if (state == nullptr || state->GetRuntime() == nullptr) {
    return;
  }
  ppc::detail::CheckDetourLocalWrites(name, StateOf(*state->GetRuntime()).GetCodegenFlags(),
                                      at_entry, at_exit, warned);
}

void CheckKernelDetourArguments(const char* name, PPCFunc* host, u8 declared_gpr,
                                u16 declared_fpr) {
  const ThreadState* state = ThreadState::Get();
  if (state == nullptr || state->GetRuntime() == nullptr) {
    return;
  }
  const std::optional<ppc::PPCCallUsage> usage = StateOf(*state->GetRuntime()).GetCallUsage(host);
  if (usage) {
    ppc::detail::CheckDetourArguments(name, *usage, declared_gpr, declared_fpr);
  }
}

std::mutex g_hooks_mutex;
u32 g_hooks_count = 0;

const ppc::KernelHooks kHooks = {
    ReadBoundThread,
    ResolveBoundFunction,
#ifndef NDEBUG
    CheckHookRegisterFile,
#else
    nullptr,
#endif
    CheckKernelDetourLocalWrites,
    CheckKernelDetourArguments,
};

}  // namespace

void RegisterPPCHooks() {
  std::lock_guard lock(g_hooks_mutex);
  if (g_hooks_count++ == 0) {
    ppc::SetKernelHooks(&kHooks);
  }
}

void UnregisterPPCHooks() {
  std::lock_guard lock(g_hooks_mutex);
  REX_ASSERT(g_hooks_count != 0, "The PPC hooks are released more often than registered");
  if (--g_hooks_count == 0) {
    ppc::SetKernelHooks(nullptr);
  }
}

}  // namespace rex::kernel
