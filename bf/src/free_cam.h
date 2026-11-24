// The admin's free camera and the hidden HUD and UI (free_cam.cpp; the F9 panel, world_sync.cpp, turns them on).
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace bf1943::freecam {

// The client whose free camera flies (its start hook); 0 before a level.
uint32_t Client();
// The free camera flies now / was asked for (the next pad read switches the view). Any thread.
bool Active();
bool Wanted();
void Set(bool on);
// The game's HUD and every overlay of ours hidden while our menus and the F9 panel are closed. Any thread.
bool HideUi();
void SetHideUi(bool hide);
// look.cpp: the mouse's motion; true while the free camera takes it (the soldier's aim then gets none).
bool TakeMouse(float dx, float dy);
// auto_join.cpp's pad read, player 1, with the game's X_INPUT_GAMEPAD (big endian) as it would get it: while the camera
// flies it is read and emptied. `input`: false while our menu or the chat box has the pad (the camera stays still).
void Pad(PPCContext& ctx, uint8_t* base, uint8_t* pad, bool input);

}  // namespace bf1943::freecam
