# Changelog

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
