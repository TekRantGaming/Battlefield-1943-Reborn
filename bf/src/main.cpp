// bf1943 - ReXGlue Recompiled Project

#include "generated/default/bf1943_init.h"

#include "bf1943_app.h"

REX_DEFINE_APP(bf1943, Bf1943App::Create)

#ifdef _WIN32
// Hybrid laptops (Intel/AMD iGPU + NVIDIA/AMD dGPU): the drivers start an exe that exports these on the discrete GPU
// (King Kong's fix, its commit 371e6771; TekRantGaming, 11 Nov 2026: players' games ran on the integrated GPU).
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

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
