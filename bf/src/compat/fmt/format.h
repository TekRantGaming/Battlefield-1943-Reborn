// Battlefield 1943 Reborn, SDK v0.11+ port: v0.11 no longer ships fmt (its logging formats with std::format). The game
// uses fmt::format and fmt::ptr only, so these map onto the standard library.
#pragma once

#include <format>
#include <iterator>
#include <string>

namespace fmt {
using std::format;
using std::format_string;
using std::format_to;
using std::format_to_n;
using std::vformat;

template <typename T>
const void* ptr(T* p) {
  return static_cast<const void*>(p);
}
}  // namespace fmt
