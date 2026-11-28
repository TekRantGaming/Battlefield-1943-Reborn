/**
 * @file        hid/input_internal.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/input_keys.h>
#include <rex/hid/binding.h>
#include <rex/hid/input_driver.h>
#include <rex/hid/pad.h>
#include <rex/hid/input_system.h>
#include <rex/setting.h>
#include <rex/types.h>

namespace rex::hid::detail {

inline constexpr u32 kMaxUsers = rex::hid::IInputSystem::kMaxUsers;
inline constexpr u32 kSyntheticOrdinal = UINT32_MAX;
inline constexpr i32 kThumbDeadzone = 7849;
inline constexpr u8 kTriggerThreshold = 30;
inline constexpr i32 kThumbKeyThreshold = 0x4E00;
inline constexpr u8 kTriggerKeyThreshold = 0x1F;
inline constexpr u32 kRepeatDelayMs = 400;
inline constexpr u32 kRepeatRateMs = 100;
inline constexpr u8 kSubtypeUSBKeyboard = 0x00;
inline constexpr u8 kSubtypeGamepad = 0x01;

inline constexpr u16 kKeystrokeKeyDown = 0x0001;
inline constexpr u16 kKeystrokeKeyUp = 0x0002;
inline constexpr u16 kKeystrokeRepeat = 0x0004;
inline constexpr u16 kKeystrokeShift = 0x0008;
inline constexpr u16 kKeystrokeCtrl = 0x0010;
inline constexpr u16 kKeystrokeAlt = 0x0020;
inline constexpr u16 kKeystrokeValidUnicode = 0x1000;

/** A pad inside the merged list, with the source that reported it and its ID there. */
struct DeviceKey {
  u32 source = 0;
  rex::hid::PadDeviceID id = rex::hid::kInvalidPadDeviceID;
  friend bool operator==(const DeviceKey&, const DeviceKey&) = default;
};

/** One pad of the merged list. */
struct Device {
  DeviceKey key;
  rex::hid::PadDevice pad;
  bool synthetic = false; /**< Keyboard and mouse and the idle pad take no ordinal. */
  bool counts = true;     /**< Counts as a connected pad of its user. */
  u32 ordinal = 0;
};

/**
 * ORs the buttons, takes the larger trigger and the larger stick deflection.
 *
 * @param dst Receives the merge.
 * @param src The state merged in.
 */
void MergeInto(rex::hid::PadState& dst, const rex::hid::PadState& src);

/** Battlefield 1943 Reborn patch: a test holds keyboard keys (bf::SetTestKey); they count without the focus. */
bool TestKeysHeld();
/** Battlefield 1943 Reborn patch: bf::LastKey* set to nothing (a read whose keyboard state is left out). */
void ClearLastKeys();

/**
 * Returns whether a state is at rest. A state is at rest when no button, trigger or stick passes
 * its threshold.
 */
bool IsNeutral(const rex::hid::PadState& state);

/**
 * Applies the radial stick deadzones.
 *
 * @param state The state, changed in place.
 * @param left The left stick deadzone, 0 to 1.
 * @param right The right stick deadzone, 0 to 1.
 */
void ApplyDeadzones(rex::hid::PadState& state, f64 left, f64 right);

/**
 * Returns whether two devices are one physical pad. Two devices are one pad when both have the same
 * XInput slot, or both have a serial and the same vendor, product and serial.
 */
bool IsSamePad(const rex::hid::PadIdentity& a, const rex::hid::PadIdentity& b);

/** Tracks the device of each user that last moved its pad off rest. */
class ActiveDeviceTracker {
 public:
  void Observe(u32 user, const DeviceKey& key, const rex::hid::PadState& state);
  bool GetActive(u32 user, DeviceKey& out) const;
  void Forget(const DeviceKey& key);

 private:
  std::array<DeviceKey, kMaxUsers> active_{};
  std::array<bool, kMaxUsers> has_{};
};

/**
 * Lists the devices in order, gives new pads the lowest free ordinal and keeps the ordinal of pads
 * in the previous list. Devices that take no ordinal come last.
 *
 * @param previous The list from the last enumeration.
 * @param seen The enumerated list, ordered on return.
 */
void AssignOrdinals(const std::vector<Device>& previous, std::vector<Device>& seen);

/**
 * Assigns devices to users.
 *
 * @param shared True when every device feeds user 0.
 * @return The devices of each user in list order. Devices without an ordinal go to user 0.
 */
std::array<std::vector<DeviceKey>, kMaxUsers> AssignUsers(const std::vector<Device>& devices,
                                                          bool shared);

/** Makes the keystrokes of one user from the sequence of pad states it reads. */
class KeystrokeSynth {
 public:
  /**
   * Takes the next keystroke for a state.
   *
   * @param now_ms A millisecond clock.
   * @return The keystroke without its user, or kNotFound.
   */
  Result<rex::hid::PadKeystroke> Next(const rex::hid::PadState& state, u32 now_ms);

 private:
  enum class Repeat : u8 { kIdle, kWaiting, kRepeating };
  u64 held_ = 0;
  Repeat repeat_ = Repeat::kIdle;
  u32 repeat_index_ = 0;
  u32 repeat_time_ = 0;
};

/** The pad that answers when nothing else does. */
class IdlePad final : public rex::hid::IInputDriver {
 public:
  std::string_view GetName() const override { return "idle"; }
  Result<void> Initialize(IDriverContext&) override { return {}; }
  void Shutdown() override {}
  u64 GetDeviceRevision() const override { return 1; }
  void EnumerateDevices(std::vector<rex::hid::PadDevice>& out) override;
  Result<rex::hid::PadState> GetDeviceState(rex::hid::PadDeviceID id) override;
  Result<rex::hid::PadCapabilities> GetDeviceCapabilities(rex::hid::PadDeviceID id) override;
  Result<void> SetDeviceVibration(rex::hid::PadDeviceID id,
                                  const rex::hid::PadVibration& vibration) override;
};

/** Returns the capabilities of a pad that reports every button and axis. */
rex::hid::PadCapabilities MakeFullPadCapabilities();

/** The keyboard and mouse bindings, in the order of kKeyBindDescs. */
enum KeyBind : u8 {
  kBindA,
  kBindB,
  kBindX,
  kBindY,
  kBindLeftShoulder,
  kBindRightShoulder,
  kBindLeftTrigger,
  kBindRightTrigger,
  kBindLStickPress,
  kBindRStickPress,
  kBindBack,
  kBindStart,
  kBindDPadUp,
  kBindDPadDown,
  kBindDPadLeft,
  kBindDPadRight,
  kBindLStickUp,
  kBindLStickDown,
  kBindLStickLeft,
  kBindLStickRight,
  kBindRStickUp,
  kBindRStickDown,
  kBindRStickLeft,
  kBindRStickRight,
  kBindGuide,
  kBindCount,
};

/** The keys of the keyboard and mouse pad, settings named hid.keybind_*. */
// Battlefield 1943 Reborn patch: the game's keys as defaults (our v0.10 SDK's: Space, E, R, F, the mouse's
// buttons on the triggers, WASD, IJKL).
inline constexpr std::array<hid::BindingConfig, kBindCount> kKeyBindDescs = {{
    {"keybind_a", "Space", "Keys for the A button", 0, PadAxes::kNone},
    {"keybind_b", "E", "Keys for the B button", 0, PadAxes::kNone},
    {"keybind_x", "R", "Keys for the X button", 0, PadAxes::kNone},
    {"keybind_y", "F", "Keys for the Y button", 0, PadAxes::kNone},
    {"keybind_left_shoulder", "G", "Keys for the left shoulder", 0, PadAxes::kNone},
    {"keybind_right_shoulder", "Q", "Keys for the right shoulder", 0, PadAxes::kNone},
    {"keybind_left_trigger", "RMB", "Keys for the left trigger", 0, PadAxes::kNone},
    {"keybind_right_trigger", "LMB", "Keys for the right trigger", 0, PadAxes::kNone},
    {"keybind_lstick_press", "Shift", "Keys for the left stick press", 0, PadAxes::kNone},
    {"keybind_rstick_press", "C,Control", "Keys for the right stick press", 0, PadAxes::kNone},
    {"keybind_back", "Tab", "Keys for the back button", 0, PadAxes::kNone},
    {"keybind_start", "Return", "Keys for the start button", 0, PadAxes::kNone},
    {"keybind_dpad_up", "Up,V", "Keys for D pad up", 0, PadAxes::kNone},
    {"keybind_dpad_down", "Down", "Keys for D pad down", 0, PadAxes::kNone},
    {"keybind_dpad_left", "Left", "Keys for D pad left", 0, PadAxes::kNone},
    {"keybind_dpad_right", "Right", "Keys for D pad right", 0, PadAxes::kNone},
    {"keybind_lstick_up", "W", "Keys for the left stick up", 0, PadAxes::kNone},
    {"keybind_lstick_down", "S", "Keys for the left stick down", 0, PadAxes::kNone},
    {"keybind_lstick_left", "A", "Keys for the left stick left", 0, PadAxes::kNone},
    {"keybind_lstick_right", "D", "Keys for the left stick right", 0, PadAxes::kNone},
    {"keybind_rstick_up", "I", "Keys for the right stick up", 0, PadAxes::kNone},
    {"keybind_rstick_down", "K", "Keys for the right stick down", 0, PadAxes::kNone},
    {"keybind_rstick_left", "J", "Keys for the right stick left", 0, PadAxes::kNone},
    {"keybind_rstick_right", "L", "Keys for the right stick right", 0, PadAxes::kNone},
    {"keybind_guide", "", "Keys for the guide button", 0, PadAxes::kNone},
}};

/** The keyboard and mouse as a pad, or as a keyboard that sends its keys as keystrokes. */
class KeyboardMousePad final : public rex::hid::IInputDriver {
 public:
  KeyboardMousePad(rex::hid::InputSettings& settings, hid::IBindingSet& bindings,
                   const std::atomic<bool>& focused);
  ~KeyboardMousePad() override;

  std::string_view GetName() const override { return "keyboard_mouse"; }
  Result<void> Initialize(IDriverContext&) override { return {}; }
  void Shutdown() override {}
  u64 GetDeviceRevision() const override;
  void EnumerateDevices(std::vector<rex::hid::PadDevice>& out) override;
  Result<rex::hid::PadState> GetDeviceState(rex::hid::PadDeviceID id) override;
  Result<rex::hid::PadCapabilities> GetDeviceCapabilities(rex::hid::PadDeviceID id) override;
  Result<void> SetDeviceVibration(rex::hid::PadDeviceID id,
                                  const rex::hid::PadVibration& vibration) override;

  /** Reports whether the keyboard feeds keystrokes and no pad. */
  bool IsPassthrough() const;

  /**
   * Takes the next raw keystroke queued in passthrough.
   *
   * @return The keystroke, or an error when none is queued.
   */
  Result<rex::hid::PadKeystroke> TakeKeystroke();

  bool IsPointerLockWanted() const { return pointer_lock_.load(std::memory_order_relaxed); }

  void OnEvent(InputEvent& event);

  /** Logs bindings that did not parse and reloads the keys. UI thread only. */
  void RefreshBinds();

 private:
  bool IsEnabled() const;
  void OnKey(const InputEvent& event, bool down);
  void OnChar(const InputEvent& event);
  void OnButton(const InputEvent& event, bool down);
  void OnMotion(const InputEvent& event);
  void OnFocusLost();
  void SetKeyState(u16 key, bool down);
  void PushKeystroke(const rex::hid::PadKeystroke& keystroke);
  bool IsPressed(size_t index) const;
  /** Battlefield 1943 Reborn patch: a bind source's keys for a binding, parsed once per text. */
  const std::vector<KeyChord>& SourceChords(size_t index, const std::string& keys) const;

  rex::hid::InputSettings& settings_;
  hid::IBindingSet& bindings_;
  const std::atomic<bool>& focused_;
  mutable std::mutex mutex_;
  std::array<std::vector<KeyChord>, kBindCount> binds_;
  u32 bind_generation_ = 0;
  bool key_down_[256] = {};
  /** The keys IsPressed sees at a read: key_down_ and the tests' keys (bf_mnk.h). */
  bool held_[256] = {};
  mutable std::array<std::pair<std::string, std::vector<KeyChord>>, kBindCount> source_chords_;
  f32 mouse_dx_ = 0.0f;
  f32 mouse_dy_ = 0.0f;
  std::deque<rex::hid::PadKeystroke> queue_;
  std::atomic<bool> pointer_lock_{false};
  mutable std::atomic<u32> listed_mode_{0};
  mutable std::atomic<u64> revision_{1};
};

}  // namespace rex::hid::detail
