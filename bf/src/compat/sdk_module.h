// Battlefield 1943 Reborn, SDK v0.11+ port: an SDK module's DLL found under any build's name.
//
// v0.11 builds one DLL per module (rexkernel, rexxam, rexbase, ...), named by the build: rexkernel.dll in a release,
// rexkernelrd.dll in RelWithDebInfo (the developer build), rexkerneld.dll in Debug (CMAKE_<CONFIG>_POSTFIX in the
// SDK's CMakeLists). A lookup by the release's name alone found nothing in the developer build, so every original
// kernel call fetched through it was null (the first smoke run of the port, 25 Nov 2026: a call to 0 two seconds in).
#pragma once

#if defined(_WIN32)
#include <windows.h>

#include <string>

namespace bf1943 {

// The loaded module whose base name is `base` ("rexkernel"), or null.
inline HMODULE SdkModule(const char* base) {
  for (const char* suffix : {"", "rd", "d"}) {
    if (HMODULE module = GetModuleHandleA((std::string(base) + suffix + ".dll").c_str())) return module;
  }
  return nullptr;
}

}  // namespace bf1943
#endif
