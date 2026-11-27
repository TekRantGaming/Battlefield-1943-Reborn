/**
 * @file        hid/bf_mnk.h
 * @brief       The keyboard and mouse pad's additions for a title's own input code
 *
 * @license     BSD 3-Clause License
 *
 * @remarks     Battlefield 1943 Reborn's patch of the SDK (v0.10's rex/input/mnk additions, kept for v0.11's
 *              keyboard and mouse pad, src/hid/keyboard_mouse_pad.cpp): what the keys gave the pad at its last read
 *              (the game's own menus leave a key bound to a game action out of the pad state they take), keys held
 *              by tests without the window's focus, the mouse's look turned on and off by the title, and a bind's
 *              keys asked of the title at each read (a set of keys for each kind of vehicle).
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <rex/platform.h>

namespace rex::hid::bf {

/** The mouse moves the right stick (hid.mnk_mouse) and the pointer is locked only while this is on (default). */
REX_HID_API void SetMouseLookActive(bool active);
REX_HID_API bool IsMouseLookActive();

/** The buttons (XInput bits) the keys gave the pad at its last read, and whether a key pushed a stick or a trigger. */
REX_HID_API uint16_t LastKeyButtons();
REX_HID_API bool LastKeyAxes();
/** The triggers (0 or 255) and the left stick (-32767 to 32767, up positive) the keys gave at the last read. */
REX_HID_API uint8_t LastKeyTrigger(bool right);
REX_HID_API int16_t LastKeyLeftStick(bool y);
/** The right stick at the last read: the keys' and the mouse's own (hid.mnk_mouse), -32767 to 32767. */
REX_HID_API int16_t LastKeyRightStick(bool y);

/** Tests: a key (a VirtualKey below 256) held as if pressed, with or without the window's focus. */
REX_HID_API void SetTestKey(uint16_t virtual_key, bool down);

/**
 * Where a bind's keys come from at each read: given a hid.keybind_* binding's name (keybind_a...), return true and
 * the keys as that setting's text ("Space", "C,Control") to use them instead of the setting's own; false keeps its own.
 */
using BindSource = bool (*)(std::string_view name, std::string& keys);
REX_HID_API void SetBindSource(BindSource source);

}  // namespace rex::hid::bf
