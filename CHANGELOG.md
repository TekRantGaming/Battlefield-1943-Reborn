# Changelog

## 0.2.0 (11 November 2026): Linux, and updates that install themselves

An **alpha playtest** build: expect bugs, crashes and changes, and stats that reset.

**New**
- **Linux.** Battlefield 1943 Reborn runs on Linux as an AppImage (`BF1943-Reborn-0.2.0-linux-x86_64.AppImage`). It
  was played in a real match on the official Playtest server from a ROG Ally (Bazzite). Put it in a folder of its own:
  its `game` and `user` folders go beside it.
- **Updates install themselves**, on Windows and Linux. At every start the game looks at the latest release here; a
  newer one is downloaded, checked against the release's `SHA256SUMS.txt`, installed and started. On Windows only the
  files that changed are replaced, and if any cannot be, every one is put back; files an older build had and this one
  does not are removed only while unchanged; the first start of a new version checks every file again. On Linux the
  AppImage replaces itself. Your `game` and `user` folders are never touched. 0.1.33 and older cannot update
  themselves: download this one once.
- **Choose your movement.** Quick Match and the Server Browser now ask which movement you want, Classic or Modern,
  every time, with what each one changes, and show only the servers that play it. There is no movement setting any
  more: your game plays what the server you join plays, so nobody plays Modern on a Classic server. The choice works
  with the mouse, the keyboard and a controller.
- A notice on Home and the main menu: avoid flying with mouse and keyboard for now.

**Changed**
- Performance metrics are off unless you turn them on (Settings > Gameplay, or F2). Installs where an earlier build
  turned them on get them off once.
- Create Private Match is gone from the main menu for now.
- After Play in the launcher the game signs in within about 12 seconds (it could take up to a minute).
- Planes fly as in 0.1.33: the new keyboard and mouse flying modes that were tried are not in this build.

**Known issues**
- Avoid flying with mouse and keyboard for now: take a boat to get to land, or use a controller to fly. This will be
  fixed in a later update; mouse and keyboard work everywhere else.
- The first person weapon and hands still move 30 times a second at high frame rates.
- Stats, awards and leaderboards reset when the official backend restarts.
- Linux: the Vulkan renderer needs X11 or XWayland; the game's tutorial (Help) can drop back to the menu at once;
  dedicated servers on Linux are not tested yet.
- No HDR, no ultrawide picture (letterbox or stretch).
- The builds are not code signed yet: Windows 11's Smart App Control can refuse `rexruntime.dll` (Bad Image,
  0xc0e90002). It is not a broken download.

## 0.1.33 preview (11 November 2026): the first public release

The first build on this page, and an **alpha playtest** build: expect bugs, crashes and changes, and stats that reset.
Earlier preview builds (0.1.1 to 0.1.32) went to the private playtest only.

**Since 0.1.32**
- The runtime's own debug overlays (F3, F4 and the backtick key) no longer open in game.
- The window is titled Battlefield 1943 Reborn.
- New installs show the full performance metrics in the corner; F2 switches between off, FPS only and full.

**What the build holds**
- All four maps online, up to 24 players a server, on our own backend: the server browser (official and community
  servers), Play online, Create Private Match and Direct connect.
- Classic and Modern playstyles, each server playing one of them.
- Any frame rate (a limit from 30 to 240 or none), with the camera turning on every frame and soldiers, vehicles and
  vehicle cameras moving smoothly between the game's 30 updates a second.
- Vulkan graphics: render resolution up to 2160p, shadows up to four times the original, FXAA, sharpening, texture
  filtering up to 16x, ambient occlusion, global illumination, and grass on Wake Island, Guadalcanal and Iwo Jima.
- Keyboard and mouse with rebindable keys beside controllers, the mouse aiming vehicle guns and turrets directly;
  controller sensitivity, response, dead zone, inversion, vibration and button remapping; the game's own control
  layouts.
- Our first start setup, main menu, pause menu and settings; Stats and Awards, Achievements and Leaderboards pages.
- The dedicated server package, with its setup window, map rotation and playstyle, listed for everyone.

**Known issues**
- Flying a plane with the mouse is very hard: the mouse moves the plane's stick only while it moves, and S holds free
  look in a plane. A new mouse flying mode is being worked on. Controllers fly as on the Xbox 360.
- The first person weapon and hands still move 30 times a second at high frame rates.
- Stats, awards and leaderboards reset when the official backend restarts.
- No HDR, no ultrawide picture (letterbox or stretch), Windows only.
- The builds are not code signed yet: Windows 11's Smart App Control can refuse `rexruntime.dll` (Bad Image,
  0xc0e90002). It is not a broken download.
