# Weapon skins

Reborn's first unlockable is the **electric skin**: the player's own Thompson (US) and Type 100 (Japan) look as if
electricity runs through them. It is unlocked by playing one full Night Ops match: join near its start (within two
minutes of the round's start, and at least five minutes played) and stay to its end. Daybreak matches do not count.
Once unlocked, equip it from the main menu's Unlockables screen.

## What it looks like

The gun's metal is dark, its wood is gold, and a spiral of storm smoke plumes round it from the receiver to the muzzle,
lit from within by lightning. A new magazine going in runs a charge over the gun. A subtle bloom glows round the light,
and in the rain the gun's wetness (darkening, beads and runs) is drawn over the skin.

## Only you see it

The skin is drawn by your own game only, on your own first person weapon. Nothing about it is sent to the server, so
other players see their own guns, and yours, as always. Your hands are not changed.

## Turning it on for testing

The unlock turns on a setting that is off by default; for testing it can be turned on by hand:

- `cvar.bf_skin_electric = true` in `bf1943-settings.txt`, or
- `--bf_skin_electric=true` on the command line.

The unlock (`SkinMatchSample` and `SkinMatchRoundOver` in `bf/src/fov.cpp`, fed by the world answer's Night Ops flag in
`bf/src/world_sync.cpp`) calls `bf1943::UnlockElectricSkin()`, which turns the setting on and saves it.
`BF_DEV_SKIN_MATCH_SECONDS=<s>` sets another least time for tests.

## How it works

A skin has three layers, all drawn by the renderer (`bf/third_party/native-x-renderer/src/weapon_skin.cpp`, decision
D233 in `bf/third_party/native-x-renderer/docs/decisions.md`):

1. **The texture.** In the gun's first person draws the colour texture is swapped for a copy made over by the skin
   (`WeaponSkinView`, `WeaponSkinMake`): an RGBA16F texture, made again 30 times a second so it can move. The
   electric skin's copy is dark metal and gold wood (the original's alpha marks the wood). The rain's wetness is made
   from the skinned copy, so the water is on top.
2. **The mesh pass.** The gun's own draws are kept and drawn again (`DrawPath::KeepSkinDraw`, `DrawSkinOver`) with the
   skin's pixel shader, depth tested equal, so light lies on the model itself (the lightning on the gun). Not drawn with
   MSAA (its target would have to be multisampled).
3. **The overlay.** Over the picture before the HUD (`WeaponSkinBloom`): the storm's smoke as particles (a compute pass,
   then sprites), its bolts, and a bloom. The cloud is fixed to the gun by the gun's own bones
   (`WeaponSkinGunFrame`), so it follows the gun in a reload and when the Iron Sight option moves it.

What feeds them:

- `bf/src/fov.cpp` (`WeaponSkin`) knows the weapon in your hands each frame by its data's name. For the Thompson
  (`thompson`) and the Type 100 (`nambu100`), while the skin is unlocked (`bf_skin_electric`) and worn
  (`bf_skin_equipped = electric`, set on the Unlockables screen), it tells the renderer the skin
  (`SetWeaponSkin(1)`, held a quarter second) and which gun it is (`SetWeaponSkinMagazine`: 0 the Thompson, 1 the
  Type 100; the shaders' `mg.x`).
- `DrawPath::ClassifyWeapon` (`draw.cpp`) tells the first person weapon's draws apart: the game draws what is within
  7.5 m in a depth range of its own, and the weapon with a field of view of its own. `SkinView` then takes the gun's
  colour texture (the largest DXT1 one; never the hands').
- Each gun's shape is a `GunModel` in `kGuns`, measured from the weapon export (`NXR_WEAPON_EXPORT`, a developer's
  build, its `weapon.obj` and `info.txt`): the bone the body is on, and in that bone's space the back of the receiver,
  the muzzle, the magazine's top and end, and where the magazine's bone rests when it is in (a reload's charge is told
  by it). The export is the game's own art and stays out of the repository.

## Aiming down the sights

The rule, settled in review on 28 Nov 2026 (D255): **aimed in, the skin looks exactly
as it does from the hip, and only the sights are kept clear.** The animation does not change, move or stand off the
gun.

- **What is cleared while aiming:** the sight picture, a small circle at the middle of the view (the particles' pixel
  shader: `clear`, from 3.5 to 8.5% of the view's height, scaled by how far the aim is in, `gun_up.w = AimNow()`).
  Per gun, more where its sight line needs it: on the Type 100 the top of the gun along its sight line (the particle
  vertex shader's `over_top`, by the puff's angle round the barrel, for `mg.x` 1 only). The Thompson needs nothing
  more.
- **What is not done:** moving the cloud off the gun while aiming, or letting the gun and hands stop hiding it. Both
  were tried and both put smoke round the sights instead of round the gun.
- Seen down the sights the gun is end on, so the gun itself hides much of the cloud behind it (all of it on the
  Thompson, whose receiver is wide). That is expected: the cloud is as it would be.
- **The gun must stay recognised while aiming.** Aimed in, the weapon's own view narrows (a Thompson to 25 degrees, an
  M1 Garand to 10 to 12), so `ClassifyWeapon` takes the weapon's projections down to 6 degrees. The same rule keeps the
  rain's wetness on every weapon while aiming. The scoped rifles are left out: their optic fills the screen.
- `GunPoints` only needs the muzzle in front of the eye: aimed in, the back of the receiver comes to the eye.

## Making a new skin

Follow the electric skin's pattern, and the aiming rule above:

1. **Its own number and setting.** `SetWeaponSkin(n)` with a new number, a new value for `bf_skin_equipped`, and an
   entry on the Unlockables screen (`UnlockablesPage`). The shaders branch on the skin's number; the texture, mesh pass
   and overlay are only drawn while `WeaponSkinNow()` says a skin is worn.
2. **Each gun it covers** needs its weapon name in `fov.cpp`'s `WeaponSkin`, a `GunModel` measured from the export,
   and a gun number the shaders can tell apart (today `SetWeaponSkinMagazine` / `mg.x` only tells two guns).
3. **The aiming rule:** clear only the sight picture, plus, per gun, whatever lies on its sight line (look at the gun
   aimed in and decide with the user from screenshots). Never move or change the animation for aiming.
4. **Check it** for every gun the skin covers, from the hip and aimed in, with the Iron Sight option (`bf_smg_sights`)
   on and off, by day and by night, and in the rain (the wetness on top). Only your own game draws it.

### Testing it

- Settings in the test profile: `cvar.bf_skin_electric = true`, `cvar.bf_skin_equipped = electric`,
  `cvar.bf_smg_sights = true` (and the profile's `shaders_prepared_for` set to the build's version, or the first start
  counts as an update and a LAN match cannot be hosted at the start).
- Aim with the match script's left trigger, e.g. `BF_MATCH_SCRIPT=52:lt/9,64:lt/9`. Its clock runs about 18 seconds
  behind the screenshots' (`-Shots`), so put the aimed shots about 18 seconds later than the aim's times.
- The Japanese guns: switch sides with `BF_MENU_STEPS=68:changeteam,80:bfa,82:bfa,84:bfa` (the developer's Change Team
  and the presses that spawn again). Another kit: `BF_SPAWN_PRESSES=a,lsl,a,a` (the left stick on the spawn screen).
- `NXR_SKIN_AIM_LOG=1` logs, while aiming, each weapon draw's depth range, projection and whether it was taken for the
  weapon's, and twice a second what the overlay goes by (the gun's back and muzzle, the aim).
- `NXR_WET_TINT=99` forces the weapon's wet copies, to check the wetness without waiting for rain.

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
