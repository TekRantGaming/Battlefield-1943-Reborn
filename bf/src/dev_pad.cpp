// Developer aid (BF_DEV_TOOLS builds only): a scripted virtual controller for unattended runs. When BF_DEV_PAD is
// set, the game gets an input system with only this pad in it, so a real controller (maybe TekRantGaming's hands) never
// mixes in, and no window focus is needed: the driver answers whether or not the window is active.
//
//   BF_DEV_PAD=<seconds>:<button>,<seconds>:<button>,...   seconds count from the game's first pad read; each press
//   is held for 0.2 seconds, or <button>/<seconds> holds it longer. Buttons: start back a b x y up down left right
//   lb rb lt rt ls rs, and the left stick held fully over: lsr lsl lsu lsd (right, left, up, down; walk tests).
//
// Same script format as Sneak King's dev_pad.cpp, but as an input driver (no title addresses): the runtime calls it
// for XamInputGetState and GetCapabilities; SDK v0.11+ makes the keystrokes from the button changes itself.

#if defined(BF_DEV_TOOLS)

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <array>
#include <span>
#include <string_view>

#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/driver_module.h>
#include <rex/driver_registration.h>
#include <rex/hid/input_driver.h>
#include <rex/hid/pad.h>
#include <rex/logging.h>
#include <rex/module.h>

namespace bf1943 {
namespace {

struct Press {
  double at;
  uint32_t buttons;  // XInput button bits, plus 0x10000 left trigger, 0x20000 right trigger, 0x3C0000 left stick
  double hold;
};

struct Button {
  const char* name;
  uint32_t mask;
  uint16_t vk;  // VK_PAD_* (v0.10 made its keystrokes; v0.11's input system makes them from the state)
};

constexpr Button kButtons[] = {
    {"up", 0x0001, 0x5810},   {"down", 0x0002, 0x5811}, {"left", 0x0004, 0x5812},  {"right", 0x0008, 0x5813},
    {"start", 0x0010, 0x5814}, {"back", 0x0020, 0x5815}, {"ls", 0x0040, 0x5816},    {"rs", 0x0080, 0x5817},
    {"lb", 0x0100, 0x5805},   {"rb", 0x0200, 0x5804},   {"a", 0x1000, 0x5800},     {"b", 0x2000, 0x5801},
    {"x", 0x4000, 0x5802},    {"y", 0x8000, 0x5803},    {"lt", 0x10000, 0x5806},   {"rt", 0x20000, 0x5807},
    {"lsr", 0x40000, 0},      {"lsl", 0x80000, 0},      {"lsu", 0x100000, 0},      {"lsd", 0x200000, 0},
};

std::vector<Press> ParseScript(const std::string& s) {
  std::vector<Press> presses;
  size_t pos = 0;
  while (pos < s.size()) {
    const size_t comma = s.find(',', pos);
    const std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    const size_t colon = item.find(':');
    if (colon != std::string::npos) {
      std::string name = item.substr(colon + 1);
      double hold = 0.2;
      const size_t slash = name.find('/');
      if (slash != std::string::npos) {
        hold = std::atof(name.substr(slash + 1).c_str());
        name = name.substr(0, slash);
      }
      uint32_t mask = 0;
      for (const Button& b : kButtons) {
        if (name == b.name) mask = b.mask;
      }
      if (mask) {
        presses.push_back({std::atof(item.substr(0, colon).c_str()), mask, hold});
      } else {
        REXLOG_WARN("BF dev pad: unknown button '{}'", name);
      }
    }
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return presses;
}

// SDK v0.11+: an input driver ("devpad") in a module of the program's own (DevPadModule, App::OnRegisterModules); the
// app selects it alone (hid.drivers) when BF_DEV_PAD is set. The input system makes the keystrokes from its state.
class DevPadDriver final : public rex::hid::IInputDriver {
 public:
  DevPadDriver() {
    const char* script = std::getenv("BF_DEV_PAD");
    presses_ = ParseScript(script ? script : "");
    REXLOG_INFO("BF dev pad: {} scripted presses, real pads ignored for this run", presses_.size());
  }

  std::string_view GetName() const override { return "devpad"; }
  rex::Result<void> Initialize(rex::IDriverContext&) override { return {}; }
  void Shutdown() override {}
  uint64_t GetDeviceRevision() const override { return 1; }

  void EnumerateDevices(std::vector<rex::hid::PadDevice>& out) override {
    rex::hid::PadDevice device;
    device.id = kId;
    device.name = "BF dev pad";
    out.push_back(std::move(device));
  }

  rex::Result<rex::hid::PadState> GetDeviceState(rex::hid::PadDeviceID id) override {
    if (id != kId) return std::unexpected(rex::Error(rex::ErrorCategory::kNotFound, "not the dev pad"));
    std::lock_guard<std::mutex> lock(mutex_);
    if (++state_reads_ % 300 == 1) REXLOG_INFO("BF dev pad: state read {}", state_reads_);
    const uint32_t buttons = Update();
    rex::hid::PadState state;
    state.buttons = uint16_t(buttons);
    state.left_trigger = (buttons & 0x10000) ? 255 : 0;
    state.right_trigger = (buttons & 0x20000) ? 255 : 0;
    state.thumb_lx = int16_t((buttons & 0x40000) ? 32767 : (buttons & 0x80000) ? -32768 : 0);
    state.thumb_ly = int16_t((buttons & 0x100000) ? 32767 : (buttons & 0x200000) ? -32768 : 0);
    return state;
  }

  rex::Result<rex::hid::PadCapabilities> GetDeviceCapabilities(rex::hid::PadDeviceID id) override {
    if (id != kId) return std::unexpected(rex::Error(rex::ErrorCategory::kNotFound, "not the dev pad"));
    rex::hid::PadCapabilities caps;
    caps.supported.buttons = 0xF3FF;
    caps.supported.left_trigger = 0xFF;
    caps.supported.right_trigger = 0xFF;
    caps.supported.thumb_lx = caps.supported.thumb_ly = caps.supported.thumb_rx = caps.supported.thumb_ry =
        int16_t(0xFFC0);
    caps.motors.left_motor = caps.motors.right_motor = 0xFFFF;
    return caps;
  }

  rex::Result<void> SetDeviceVibration(rex::hid::PadDeviceID id, const rex::hid::PadVibration&) override {
    if (id != kId) return std::unexpected(rex::Error(rex::ErrorCategory::kNotFound, "not the dev pad"));
    return {};
  }

 private:
  static constexpr rex::hid::PadDeviceID kId{0xBF1943};

  // The script's buttons at this moment.
  uint32_t Update() {
    const auto now = std::chrono::steady_clock::now();
    if (!started_) {
      started_ = true;
      start_ = now;
    }
    const double t = std::chrono::duration<double>(now - start_).count();
    uint32_t buttons = 0;
    for (const Press& p : presses_) {
      if (t >= p.at && t < p.at + p.hold) buttons |= p.buttons;
    }
    if (buttons != last_) {
      REXLOG_INFO("BF dev pad: {:.1f} s buttons {:05X}", t, buttons);
      last_ = buttons;
    }
    return buttons;
  }

  std::vector<Press> presses_;
  std::mutex mutex_;
  bool started_ = false;
  std::chrono::steady_clock::time_point start_;
  uint32_t last_ = 0;
  uint32_t state_reads_ = 0;
};

std::unique_ptr<rex::IDriver> CreateDevPad() { return std::make_unique<DevPadDriver>(); }

constexpr std::array<rex::DriverRegistration, 1> kDrivers = {
    rex::DriverRegistration{rex::DriverType::kInput, "devpad", {}, &CreateDevPad}};

class DevPadProvider final : public rex::IModule, public rex::IDriverModule {
 public:
  REX_MODULE_SERVES(::rex::IDriverModule)
  std::string_view GetName() const override { return "bf1943-devpad"; }
  rex::Result<void> Connect(rex::IInterfaceFactory&) override { return {}; }
  void Disconnect() override {}
  rex::Result<void> Init() override { return rex::Ok(); }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view name, uint32_t version) override {
    return rex::FindModuleInterface<rex::IDriverModule>(*this, name, version);
  }
  std::span<const rex::DriverRegistration> GetDrivers() const override { return kDrivers; }
};

}  // namespace

bool DevPadOn() {
  const char* script = std::getenv("BF_DEV_PAD");
  return script && *script;
}

rex::IModule* DevPadModule() {
  static DevPadProvider provider;
  return DevPadOn() ? &provider : nullptr;
}

}  // namespace bf1943

#endif  // BF_DEV_TOOLS
