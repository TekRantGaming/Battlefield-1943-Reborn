// Developer aid: when the runtime stops the game with its own breakpoint (rex::debug::Break, reached from the guest
// imports DbgBreakPoint, RtlRaiseException, KeBugCheck and KeBugCheckEx), write the guest registers and the guest
// call stack to break.log beside the exe before the process dies. The crash itself is left to happen.
//
// Guest stack frames (PowerPC, as the recompiled code builds them): [r1] holds the caller's r1 (the back chain) and
// the caller's frame keeps the saved LR at back chain - 8 ("stw r12,-8(r1)" before "stwu r1,-N(r1)").
//
// BF_CRASH_TRACE=1 also logs access violations the same way (the runtime may handle some itself, so it is opt in).

#if defined(_WIN32) && defined(BF_DEV_TOOLS)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <rex/ppc/context.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

namespace {

// Reads nothing (returns 0) from a page that is not committed and readable: this runs inside an exception handler.
uint32_t LoadGuest32(const uint8_t* membase, uint32_t address) {
  const uint8_t* p = membase + address;
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
      (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
      reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize < p + 4)
    return 0;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

bool PlausibleStack(uint32_t address) { return address >= 0x70000000 && address < 0x80000000 && (address & 3) == 0; }

LONG CALLBACK BreakpointHandler(EXCEPTION_POINTERS* ep) {
  static const bool trace_crashes = [] {
    const char* v = std::getenv("BF_CRASH_TRACE");
    return v && *v && *v != '0';
  }();
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_BREAKPOINT && !(trace_crashes && code == EXCEPTION_ACCESS_VIOLATION))
    return EXCEPTION_CONTINUE_SEARCH;
  static std::atomic<int> hits{0};
  if (hits.fetch_add(1) >= 4) return EXCEPTION_CONTINUE_SEARCH;
  FILE* f = std::fopen("break.log", "a");
  if (!f) return EXCEPTION_CONTINUE_SEARCH;
  std::setvbuf(f, nullptr, _IONBF, 0);  // keep what was written if anything below faults
  std::fprintf(f, "breakpoint at host rip=%p thread=%lu\n", reinterpret_cast<void*>(ep->ContextRecord->Rip),
               GetCurrentThreadId());
  auto* ts = rex::runtime::ThreadState::Get();
  PPCContext* ctx = ts ? ts->context() : nullptr;
  const uint8_t* membase = (ts && ts->memory()) ? ts->memory()->virtual_membase() : nullptr;
  if (ctx) {
    std::fprintf(f, "guest thread %u lr=%08llX ctr=%08llX r1=%08X\n", ts->thread_id(),
                 static_cast<unsigned long long>(ctx->lr), static_cast<unsigned long long>(ctx->ctr.u64), ctx->r1.u32);
    std::fprintf(f, "r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r30=%08X r31=%08X\n", ctx->r3.u32, ctx->r4.u32,
                 ctx->r5.u32, ctx->r6.u32, ctx->r7.u32, ctx->r8.u32, ctx->r30.u32, ctx->r31.u32);
    if (membase) {
      uint32_t sp = ctx->r1.u32;
      for (int i = 0; i < 24 && PlausibleStack(sp); ++i) {
        const uint32_t back = LoadGuest32(membase, sp);
        if (!PlausibleStack(back) || back <= sp) break;
        std::fprintf(f, "  frame %2d sp=%08X saved lr=%08X\n", i, back, LoadGuest32(membase, back - 8));
        sp = back;
      }
      // RtlRaiseException(record): the exception code and the address the guest gave.
      if (ctx->r3.u32 >= 0x40000000 && ctx->r3.u32 < 0x90000000)
        std::fprintf(f, "  *r3: %08X %08X %08X %08X\n", LoadGuest32(membase, ctx->r3.u32),
                     LoadGuest32(membase, ctx->r3.u32 + 4), LoadGuest32(membase, ctx->r3.u32 + 8),
                     LoadGuest32(membase, ctx->r3.u32 + 12));
    }
  } else {
    std::fprintf(f, "no guest context on this thread\n");
  }
  std::fclose(f);
  return EXCEPTION_CONTINUE_SEARCH;
}

const PVOID g_handler = AddVectoredExceptionHandler(1, BreakpointHandler);

}  // namespace

#endif  // _WIN32 && BF_DEV_TOOLS
