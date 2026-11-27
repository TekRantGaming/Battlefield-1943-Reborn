// Battlefield 1943 Reborn, SDK v0.11+ port: the SDK cvars the game reads through REXCVAR_DECLARE / REXCVAR_GET. In
// v0.10 the SDK defined their storage; here each read takes the v0.11 setting's current value (cvar_bridge.cpp). A
// write through REXCVAR_SET would not reach the setting (the game makes none; it uses SetFlagByName).
#include <charconv>
#include <cstdint>
#include <string>

#include <rex/string/numeric.h>

#include "cvar_bridge.h"

namespace {
std::string Text(const char* name) { return rex::cvar::bridge::Get(name).value_or(""); }
}  // namespace

bool& FLAGS_mnk_mode_storage_() {
  thread_local bool v;
  v = rex::string::from_string<bool>(Text("mnk_mode"), false);
  return v;
}
bool& FLAGS_mnk_mouse_storage_() {
  thread_local bool v;
  v = rex::string::from_string<bool>(Text("mnk_mouse"), false);
  return v;
}
// Read by NativeXRenderer for every sampler of every draw (textures.cpp): each read through the bridge took the
// registry's lock, its lookup by name and the text (about 15% of the renderer's thread on Guadalcanal, 0.7 ms a frame;
// the port's phase 8, 26 Nov 2026). Taken again every 1024 reads per thread (several times a frame while the game
// draws; a clock check at each read cost 5% itself): a change in Settings applies within the frame.
int32_t& FLAGS_anisotropic_override_storage_() {
  thread_local int32_t v = 3;
  thread_local uint32_t reads = 0;
  if ((reads++ & 1023) == 0) {
    const std::string t = Text("anisotropic_override");
    v = 3;  // v0.10's default, when the setting is not there
    std::from_chars(t.data(), t.data() + t.size(), v);
  }
  return v;
}
std::string& FLAGS_cache_root_storage_() {
  thread_local std::string v;
  v = Text("cache_root");
  return v;
}
std::string& FLAGS_user_data_root_storage_() {
  thread_local std::string v;
  v = Text("user_data_root");
  return v;
}
bool& FLAGS_protect_zero_storage_() {
  thread_local bool v;
  v = rex::string::from_string<bool>(Text("protect_zero"), true);
  return v;
}
