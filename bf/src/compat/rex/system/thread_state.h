// Battlefield 1943 Reborn, SDK v0.11+ port (phase 4): v0.10's rex/system/thread_state.h. ThreadState is
// rex::kernel::ThreadState in v0.11, with v0.11's accessor names (GetContext, GetAddressSpace, GetThreadID).
#pragma once

#include <rex/kernel/thread_state.h>
#include <rex/system/kernel_state.h>

namespace rex::runtime {
using ThreadState = ::rex::kernel::ThreadState;
}  // namespace rex::runtime
