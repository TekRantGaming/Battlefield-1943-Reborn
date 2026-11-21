# Weapon skins

Reborn's first unlockable is the **electric skin**: the player's own Thompson (US) and Type 100 (Japan) look as if
electricity runs through them. It is planned to be unlocked by being struck by lightning in Night Ops' storm. That
unlock is not built yet.

## What it looks like

The gun's metal is near black with a faint marbling, its wood is gold, and blue-white lightning arcs branch over it.
The arcs crawl, pulse and crackle (the skin is redrawn 30 times a second). A subtle blue bloom glows round the arcs, and
in the rain the gun's wetness (darkening, beads and runs) is drawn over the skin.

## Only you see it

The skin is drawn by your own game only, on your own first person weapon. Nothing about it is sent to the server, so
other players see their own guns, and yours, as always. Your hands are not changed.

## Turning it on for testing

There is no unlock yet, so the skin is behind a setting that is off by default:

- `cvar.bf_skin_electric = true` in `bf1943-settings.txt`, or
- `--bf_skin_electric=true` on the command line.

When the unlock is built, it calls `bf1943::UnlockElectricSkin()` (`bf/src/fov.cpp`), which turns the setting on and
saves it.

## How it works

- `bf/src/fov.cpp` knows the weapon in your hands each frame; for the Thompson (`thompson`) and the Type 100
  (`nambu100`) it tells the renderer to draw the skin (`native_x_renderer::SetWeaponSkin`).
- The renderer (`bf/third_party/native-x-renderer/src/weapon_skin.cpp`) swaps the gun's colour texture in its first
  person draws for a skinned copy, made between draws. The wetness's copy is made from the skinned one, so the water is
  on top. The bloom is a small pass over the picture before the HUD.

## Cost

Measured at 3840 x 2160 on an RTX 4070 Ti (`native_x_renderer_gpu_sections`): the skin 0.03 to 0.05 ms a frame on the
Thompson and 0.07 to 0.08 ms on the Type 100, the bloom about 0.05 ms.

## Limits

The arcs can only be so bright: brighter than about 2.5 times white (or white, once the gun is wet) overflows what the
game draws into, and shows as orange and red fringes. So in the dark, and most of all on a wet gun at night, the arcs
are dim: on the Type 100 at Japan's base in the late storm they only just show.

## Still to test

- **A true glow for the arcs.** The arcs are part of the gun's colour, so the game's lighting dims them at night. A glow
  that lights the arcs themselves after the game's shading (an emissive layer) would keep them bright in the dark and
  in the rain. It has not been built or tested yet.
- Seeing another player who wears the skin from a second game (both on the same side, facing each other). By design they
  see a plain gun; the two-player tests so far had both players at different bases.
