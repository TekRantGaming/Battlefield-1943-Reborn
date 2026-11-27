// Battlefield 1943 Reborn, SDK v0.11+ port (phase 4): v0.10's rex/system/flags.h. The game reads one of its
// cvars, protect_zero (runtime.protect_zero in v0.11: compat/sdk_cvars.cpp).
#pragma once

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, protect_zero);
