// Battlefield 1943 Reborn, SDK v0.11+ port (phase 6): v0.10's rex/input/mnk/mnk_input_driver.h, the calls the game
// makes. v0.11's keyboard and mouse pad is part of its input system (src/hid/keyboard_mouse_pad.cpp); our additions
// to it are rex/hid/bf_mnk.h (our SDK patch). These keep v0.10's names; the bind source keeps v0.10's form (compat
// input_compat.cpp turns it into v0.11's).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <rex/hid/bf_mnk.h>

namespace rex::input::mnk {

inline void SetMouseLookActive(bool active) { ::rex::hid::bf::SetMouseLookActive(active); }
inline bool IsMouseLookActive() { return ::rex::hid::bf::IsMouseLookActive(); }
inline uint16_t LastKeyButtons() { return ::rex::hid::bf::LastKeyButtons(); }
inline bool LastKeyAxes() { return ::rex::hid::bf::LastKeyAxes(); }
inline uint8_t LastKeyTrigger(bool right) { return ::rex::hid::bf::LastKeyTrigger(right); }
inline int16_t LastKeyLeftStick(bool y) { return ::rex::hid::bf::LastKeyLeftStick(y); }
inline int16_t LastKeyRightStick(bool y) { return ::rex::hid::bf::LastKeyRightStick(y); }
inline void SetTestKey(uint16_t virtual_key, bool down) { ::rex::hid::bf::SetTestKey(virtual_key, down); }

// v0.10's form: given a keybind_* setting's name and its own value, the keys to use.
using BindSource = std::string (*)(std::string_view name, const std::string& own);
void SetBindSource(BindSource source);

}  // namespace rex::input::mnk
