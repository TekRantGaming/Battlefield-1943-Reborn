// bf1943 - ReXGlue Recompiled Project

#include "bf1943_app.h"

// The game's app (bf1943_app.h) and its start (Bf1943Main, bf1943_app.cpp): the pages before the game run between
// App::Init and TitleApp::LaunchTitle.
REX_TITLE_APP_MAIN(Bf1943App, Bf1943Main)

// Hybrid laptops (Intel/AMD iGPU + NVIDIA/AMD dGPU): the drivers start an exe that exports NvOptimusEnablement and
// AmdPowerXpressRequestHighPerformance on the discrete GPU (King Kong's fix; TekRantGaming, 11 Nov 2026). SDK v0.11+
// exports them itself (rexglue_optimus.cpp.in, the same values), so the game's own copies went (a duplicate symbol).

#if defined(__linux__)
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

// An abort's stack in the error output (the server manager keeps it as game.out; 14 Nov 2026: the US server's abort at
// 07:27 left a core cut short at 1 GB, all of it the guest's memory, and no stack). Each frame as module(+offset), for
// addr2line on the build's own binary; then the abort goes on (its core written as before).
namespace {
void AbortStack(int sig) {
  static const char kHead[] = "BF crash: SIGABRT; the stack:\n";
  [[maybe_unused]] ssize_t w = write(2, kHead, sizeof(kHead) - 1);
  void* frames[64];
  const int n = backtrace(frames, 64);
  backtrace_symbols_fd(frames, n, 2);
  signal(sig, SIG_DFL);
  raise(sig);
}
struct AbortStackInstall {
  AbortStackInstall() {
    void* warm[1];
    backtrace(warm, 1);  // libgcc loaded now, not in the handler
    struct sigaction sa {};
    sa.sa_handler = AbortStack;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGABRT, &sa, nullptr);
  }
} g_abort_stack_install;
}  // namespace
#endif
