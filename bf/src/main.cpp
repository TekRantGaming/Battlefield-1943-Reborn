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
