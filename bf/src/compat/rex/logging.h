// Battlefield 1943 Reborn, SDK v0.11+ port: v0.10's rex/logging.h. The REXLOG_* macros are now in
// rex/logging/log_category.h. v0.10's header also brought fmt, which many files rely on: fmt/format.h is the compat
// one (std::format).
#pragma once

#include <fmt/format.h>
#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>
