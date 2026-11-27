// Battlefield 1943 Reborn, SDK v0.11+ port (phase 6): v0.10's bind source on v0.11's (rex/hid/bf_mnk.h).
//
// v0.10's source was given the setting's own value and returned it when it had nothing else (vehicle_binds.cpp's
// DriverBind). v0.11's pad keeps the setting's own keys parsed, so its source only answers whether it has others: the
// game's source is given a marker as "its own", and returning it means none.

#include <rex/input/mnk/mnk_input_driver.h>

#include <atomic>
#include <string>
#include <string_view>

namespace rex::input::mnk {
namespace {

std::atomic<BindSource> g_source{nullptr};
const std::string kOwn = "\x01own";

bool Adapter(std::string_view name, std::string& keys) {
  const BindSource source = g_source.load(std::memory_order_relaxed);
  if (!source) return false;
  std::string answer = source(name, kOwn);
  if (answer == kOwn) return false;
  keys = std::move(answer);
  return true;
}

}  // namespace

void SetBindSource(BindSource source) {
  g_source.store(source, std::memory_order_relaxed);
  ::rex::hid::bf::SetBindSource(source ? &Adapter : nullptr);
}

}  // namespace rex::input::mnk
