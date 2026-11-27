/**
 * @file        platform/seh.h
 * @brief       Platform-specific SEH implementation details
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#pragma once

#include <atomic>
#include <cstdint>

#include <rex/types.h>

namespace rex {
class SehException;
}

namespace rex::platform {

inline std::atomic<bool> g_seh_initialized{false};

/// Thread-local SEH state for capturing exception info.
struct SehThreadState {
  u32 code = 0;
  uintptr_t info[2] = {0, 0};
};

/// Get the thread-local SEH state.
SehThreadState& seh_thread_state();

/// SEH filter function - captures exception info and determines whether to handle.
/// Returns non-zero if the exception should be handled.
int seh_filter(u32 code, void* exception_pointers);

/// Re-raise the captured exception.
[[noreturn]] void seh_rethrow();

/// Initialize SEH signal handlers (POSIX only, no-op on Windows).
void seh_initialize();

/// POSIX: called on a fatal signal the SEH handlers do not take (outside an SEH region, or rethrown), before the
/// process ends with the default action: a game's crash report. The arguments are the signal handler's (int, siginfo_t*,
/// ucontext_t*; null when rethrown). It runs in the signal handler; null: none.
using FatalSignalHook = void (*)(int sig, void* info, void* ucontext);
void set_fatal_signal_hook(FatalSignalHook hook);

/// Thread-local flag for POSIX: true when in SEH-protected code.
bool& seh_active();

}  // namespace rex::platform
