// Battlefield 1943 Reborn, SDK v0.11+ port (phase 3): what the game still takes from v0.10's rex/rex_app.h. ReXApp is
// gone (the game's app is a TitleApp, bf1943_app.h); the causes of a start that failed were our SDK copy's, and are
// the game's own now (bf1943_app.cpp tells start_failure.cpp which).
#pragma once

#include <rex/logging.h>

namespace rex {

enum class SelfQuitCause {
  kNone,
  kInitFailed,          ///< App::Init: the environment, the systems, the window
  kRuntimeSetupFailed,  ///< (v0.10's runtime setup; App::Init in v0.11)
  kModuleLaunchFailed,  ///< TitleApp::LaunchTitle: the executable did not load or start
  kGuestExited,         ///< the guest's main thread ended
};

inline void FlushLogging() { ::rex::log::Flush(); }

}  // namespace rex
