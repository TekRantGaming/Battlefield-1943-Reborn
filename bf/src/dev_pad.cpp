// Developer aid (BF_DEV_TOOLS builds only): a scripted virtual controller for unattended runs. When BF_DEV_PAD is
// set, the game gets an input system with only this pad in it, so a real controller (maybe TekRantGaming's hands) never
// mixes in, and no window focus is needed: the driver answers whether or not the window is active.
//
//   BF_DEV_PAD=<seconds>:<button>,<seconds>:<button>,...   seconds count from the game's first pad read; each press
//   is held for 0.2 seconds, or <button>/<seconds> holds it longer. Buttons: start back a b x y up down left right
//   lb rb lt rt ls rs, and the left stick held fully over: lsr lsl lsu lsd (right, left, up, down; walk tests).
//
// Same script format as Sneak King's dev_pad.cpp, but as an input driver (no title addresses): the runtime calls it
// for XamInputGetState, GetCapabilities and GetKeystroke. Keystrokes are made from the button changes.

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

#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>

namespace bf1943 {
namespace {

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::DeviceId;
using rex::input::DeviceInfo;

struct Press {
  double at;
  uint32_t buttons;  // XInput button bits, plus 0x10000 left trigger, 0x20000 right trigger, 0x3C0000 left stick
  double hold;
};

struct Button {
  const char* name;
  uint32_t mask;
  uint16_t vk;  // VK_PAD_* for keystrokes
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

class DevPadDriver final : public rex::input::InputDriver {
 public:
  explicit DevPadDriver(std::vector<Press> presses) : InputDriver(nullptr, 0), presses_(std::move(presses)) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    DeviceInfo info;
    info.id = kId;
    info.name = "BF dev pad";
    info.guid = "bf-dev-pad";
    out.push_back(info);
  }

  X_RESULT GetDeviceState(DeviceId id, rex::input::X_INPUT_STATE* out) override {
    if (id != kId) return X_ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard<std::mutex> lock(mutex_);
    if (++state_reads_ % 300 == 1) REXLOG_INFO("BF dev pad: state read {}", state_reads_);
    const uint32_t buttons = Update();
    std::memset(out, 0, sizeof(*out));
    out->packet_number = packet_;
    out->gamepad.buttons = uint16_t(buttons);
    out->gamepad.left_trigger = (buttons & 0x10000) ? 255 : 0;
    out->gamepad.right_trigger = (buttons & 0x20000) ? 255 : 0;
    out->gamepad.thumb_lx = int16_t((buttons & 0x40000) ? 32767 : (buttons & 0x80000) ? -32768 : 0);
    out->gamepad.thumb_ly = int16_t((buttons & 0x100000) ? 32767 : (buttons & 0x200000) ? -32768 : 0);
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags, rex::input::X_INPUT_CAPABILITIES* out) override {
    (void)flags;
    if (id != kId) return X_ERROR_DEVICE_NOT_CONNECTED;
    std::memset(out, 0, sizeof(*out));
    out->type = rex::input::XINPUT_DEVTYPE_GAMEPAD;
    out->sub_type = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
    out->gamepad.buttons = 0xF3FF;
    out->gamepad.left_trigger = 0xFF;
    out->gamepad.right_trigger = 0xFF;
    out->gamepad.thumb_lx = int16_t(0xFFC0);
    out->gamepad.thumb_ly = int16_t(0xFFC0);
    out->gamepad.thumb_rx = int16_t(0xFFC0);
    out->gamepad.thumb_ry = int16_t(0xFFC0);
    out->vibration.left_motor_speed = 0xFFFF;
    out->vibration.right_motor_speed = 0xFFFF;
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId id, rex::input::X_INPUT_VIBRATION* vibration) override {
    (void)vibration;
    return id == kId ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags, rex::input::X_INPUT_KEYSTROKE* out) override {
    (void)flags;
    if (id != kId) return X_ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard<std::mutex> lock(mutex_);
    if (++keystroke_reads_ % 300 == 1) REXLOG_INFO("BF dev pad: keystroke read {}", keystroke_reads_);
    Update();
    if (keystrokes_.empty()) return X_ERROR_EMPTY;
    *out = keystrokes_.front();
    keystrokes_.pop_front();
    return X_ERROR_SUCCESS;
  }

 private:
  static constexpr DeviceId kId = DeviceId(0xBF1943);

  // The script's buttons at this moment; button changes bump the packet number and queue keystrokes.
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
      for (const Button& b : kButtons) {
        const bool was = last_ & b.mask, is = buttons & b.mask;
        if (was == is || !b.vk) continue;  // the sticks make no keystrokes
        rex::input::X_INPUT_KEYSTROKE k{};
        k.virtual_key = b.vk;
        k.flags = is ? rex::input::X_INPUT_KEYSTROKE_KEYDOWN : rex::input::X_INPUT_KEYSTROKE_KEYUP;
        k.user_index = 0;
        if (keystrokes_.size() < 64) keystrokes_.push_back(k);
      }
      last_ = buttons;
      ++packet_;
    }
    return buttons;
  }

  std::vector<Press> presses_;
  std::mutex mutex_;
  bool started_ = false;
  std::chrono::steady_clock::time_point start_;
  uint32_t last_ = 0;
  uint32_t packet_ = 0;
  uint32_t state_reads_ = 0;
  uint32_t keystroke_reads_ = 0;
  std::deque<rex::input::X_INPUT_KEYSTROKE> keystrokes_;
};

}  // namespace

// The input system for this run: only the scripted pad when BF_DEV_PAD is set, else the runtime's default
// (SDL pads plus the NOP stand in).
std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode) {
  const char* script = std::getenv("BF_DEV_PAD");
  if (tool_mode || !script || !*script) {
    auto system = rex::input::CreateDefaultInputSystem(tool_mode);
#if !defined(_WIN32)
    // As bf1943_app.h's CreateSharedInputSystem for players: on Linux every pad is the one player's.
    system->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
#endif
    return system;
  }
  auto presses = ParseScript(script);
  REXLOG_INFO("BF dev pad: {} scripted presses, real pads ignored for this run", presses.size());
  auto system = std::make_unique<rex::input::InputSystem>(nullptr);
  system->AddDriver(std::make_unique<DevPadDriver>(std::move(presses)));
  system->SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());  // as the default system does
  return system;
}

}  // namespace bf1943

#endif  // BF_DEV_TOOLS
