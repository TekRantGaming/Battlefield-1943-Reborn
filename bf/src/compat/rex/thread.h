// Battlefield 1943 Reborn, SDK v0.11+ port: v0.10's rex/thread.h. Of it, the game and NativeXRenderer use only
// rex::thread::set_current_thread_name, which is rex::platform::SetCurrentThreadName in v0.11.
#pragma once

#include <string_view>

#include <rex/platform/thread.h>

namespace rex::thread {
inline void set_current_thread_name(std::string_view name) { ::rex::platform::SetCurrentThreadName(name); }
}  // namespace rex::thread
