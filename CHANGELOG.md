# Changelog

## 0.2.3 (11 November 2026): network packets only from your server

An **alpha playtest** build: expect bugs, crashes and changes, and stats that reset. 0.2.0, 0.2.1 and 0.2.2 installs
update to it by themselves at their next start, on Windows and Linux.

**Security**
- **The game listens only to the server you play on.** Battlefield 1943 Reborn has never connected players to each
  other: your game talks only to the server you join, and no player is ever sent another player's IP address. But the
  game's network port took packets from any address, so a PC that is directly on the internet (no router, or with
  ports forwarded) could be sent packets by anyone. Now a packet reaches the game only from an address the game
  contacted itself in the last 10 minutes (the server you play on); anything else is dropped before the game reads it.
  Dedicated servers still take players as before. If it ever gets in your way, `bf_net_only_from_server = false` in
  `bf1943.toml` turns it off.

**Known issues**
- Avoid flying with mouse and keyboard for now: take a boat to get to land, or use a controller to fly. This will be
  fixed in a later update; mouse and keyboard work everywhere else.
- With keyboard and mouse a tank aims the opposite way up and down to the soldier on foot, and one invert setting
  covers both ([issue 3](../../issues/3)).
- Voice chat has no echo cancelling: with Open mic, use headphones. Push to talk and the chat keys are keyboard keys:
  with a controller only (Steam Deck, ROG Ally), use Open mic, or map a back button to B in Steam Input.
- The first person weapon and hands still move 30 times a second at high frame rates.
- Stats, awards and leaderboards reset when the official backend restarts.
- Linux: the Vulkan renderer needs X11 or XWayland; the game's tutorial (Help) can drop back to the menu at once;
  dedicated servers on Linux are not tested yet.

## 0.2.2 (11 November 2026): voice chat, text chat and a profanity filter

An **alpha playtest** build: expect bugs, crashes and changes, and stats that reset. 0.2.0 and 0.2.1 installs update
to it by themselves at their next start, on Windows and Linux.

**New**
- **Proximity voice chat.** Talk to the players around you, on both teams: full volume within 4 m, fading to silent
  at 50 m, and from their side (left or right of you). Hold **B** to talk (push to talk, the default), or choose
  **Open mic** (it sends while you speak) or **Off** in Settings > Audio > Voice chat, with Voice volume and
  Microphone sensitivity beside it. The game opens your microphone only when you first use it. A green MIC shows
  while your voice goes out, and the names of the players you hear show top left.
- **Text chat.** Messages show top left in a match: **T** to everyone, **Y** to your team, **U** to your squad; Tab
  changes the channel, Enter sends, Esc closes. Team and squad messages reach only your team and squad, from the
  deploy screen on. Settings > Gameplay > Text chat turns it off; Settings > Controls > Keyboard bindings has the keys.
- **Profanity filter.** In text chat, words with profanity show as stars. A soldier name with profanity cannot be
  used: the name page refuses it, and a player who already has one chooses a new name at their next start.
- Voice and text chat need the server's update: the official servers have it. Community servers need the 0.2.2
  server package; on an older server the game says chat is not available there yet.
- Server package: the crash fix the official servers got on 11 November (a server could stop when a game object list
  had an empty slot), and a watchdog that starts a stopped server or backend again within about 15 seconds.

**Fixed**
- **Laptops with two graphics cards** (King Kong Recompiled's fix): the game now runs on the dedicated graphics card
  instead of the integrated one, which made it slow on some laptops. A graphics card you chose yourself still wins.

**Known issues**
- Avoid flying with mouse and keyboard for now: take a boat to get to land, or use a controller to fly. This will be
  fixed in a later update; mouse and keyboard work everywhere else.
- With keyboard and mouse a tank aims the opposite way up and down to the soldier on foot, and one invert setting
  covers both ([issue 3](../../issues/3)).
- Voice chat has no echo cancelling: with Open mic, use headphones. Push to talk and the chat keys are keyboard keys:
  with a controller only (Steam Deck, ROG Ally), use Open mic, or map a back button to B in Steam Input.
- The first person weapon and hands still move 30 times a second at high frame rates.
- Stats, awards and leaderboards reset when the official backend restarts.
- Linux: the Vulkan renderer needs X11 or XWayland; the game's tutorial (Help) can drop back to the menu at once;
  dedicated servers on Linux are not tested yet.

## 0.2.1 (11 November 2026): AMD FSR 1 and NVIDIA NIS, Add to Steam with artwork, prompts that show your keys

An **alpha playtest** build: expect bugs, crashes and changes, and stats that reset. 0.2.0 installs update to it by
themselves at their next start, on Windows and Linux.

**New**
- **AMD FSR 1 and NVIDIA NIS.** Settings > Graphics has an **Upscaler** row: Off, AMD FSR 1 or NVIDIA NIS. Both scale
  the picture up to your screen and sharpen it, for a clearer image when the game draws fewer pixels than your screen
  has, and both work on any graphics card, on Windows and on Linux. With one of them the **Upscaler mode** row
  (Native, Quality, Balanced, Performance) says how many pixels the game draws: your screen's size divided by 1, 1.5,
  1.7 or 2, never below the console's 720p. Off is the plain smooth scaling, with the Sharpness row as before. Applies
  at the next start.
- New installs start with AMD FSR 1 at Quality (the first start's auto detect picks Performance for slow graphics
  cards). An existing install keeps its picture until you choose an upscaler.
- **No more Render resolution row:** the game draws at the resolution you choose for your display (the largest 16:9
  picture it holds, never more), and with an upscaler at the Upscaler mode's share of it. An install that had picked
  720p with no upscaler (the auto detect's choice for slow graphics cards) moves to AMD FSR 1 at Performance, which
  draws the same 720p for a 1080p screen, so its frame rate stays and the picture gets sharper.
- **NVIDIA NIS only where it runs:** the game checks once at each start whether NIS runs on your graphics card, and
  the Upscaler row offers it only after a check has found that it does (FSR 1 and Off before that). On a ROG Ally (AMD,
  Linux) it runs, at the same cost as FSR 1.
- **Add to Steam with artwork:** the game's capsule, wide grid, hero, logo and icon from SteamGridDB, downloaded when
  you press Add to Steam (never shipped), with Steam Input off as before. Steam closes only while its list is written,
  then opens again. The button and the Discord one show the real Steam and Discord marks.
- **More from TRG Studios has its pictures:** each project's SteamGridDB artwork, or the first picture of its GitHub
  README, fetched when the launcher opens and kept in your user folder's cache (refreshed once a day).
- **193 more pipelines prepared before you play:** the pipelines players' games built in 0.1.33 and 0.2.0 (sent with
  Share graphics data, on all four maps) are now in the game's own list, so the shader screen builds them too and
  fewer things pop in the first time you see them.

**Fixed**
- **The shader screen shows only after you install and after each update.** Before, a slower PC could see it on every
  start while the game made its pipelines again; now a start after the first one goes straight on, and the game
  finishes them behind the menus in a few seconds. After an update the shader screen builds every pipeline again,
  once, keeping the ones your game learned while you played.
- **Keyboard and mouse prompts name the key that does the action** ([issue 1](../../issues/1)). In a vehicle the
  prompts now show the vehicle keys: W to throttle (a landing boat's THROTTLE TO RELEASE BOAT showed the mouse, the
  zoom key on foot), S to brake and the right mouse button for the second weapon (a plane's bombs, a tank's coax gun);
  on foot they show the keys on foot again. They follow the keys you bind and the buttons you remap, and a bind with
  two keys shows the first (Up for CHANGE CAMERAS, where the controller's direction pad showed before). A key or
  prompt style changed in a match shows at once, no longer from the next map. The comma, period and backslash keys
  have pictures now, and the slash key no longer shows a backslash.
- Linux: in exclusive Fullscreen on a desktop with display scaling (KDE at 185% on a ROG Ally), about one start in
  three laid the menus out for the scaled desktop's size (squeezed) and rebuilt the picture's swap chain 90 times a
  second. The game now takes the window's real size from the graphics driver when the desktop's report is late.
- Linux: Borderless on a desktop with display scaling gave the game only the scaled desktop's size (1038x584 on a ROG
  Ally), stretched up by the desktop, so the picture and the menus were soft. Borderless now uses the display's own
  resolution there (1920x1080), as Fullscreen does, without changing the display.

**Known issues**
- Avoid flying with mouse and keyboard for now: take a boat to get to land, or use a controller to fly. This will be
  fixed in a later update; mouse and keyboard work everywhere else.
- With keyboard and mouse a tank aims the opposite way up and down to the soldier on foot, and one invert setting
  covers both ([issue 3](../../issues/3)).
- The first person weapon and hands still move 30 times a second at high frame rates.
- Stats, awards and leaderboards reset when the official backend restarts.
- Linux: the Vulkan renderer needs X11 or XWayland; the game's tutorial (Help) can drop back to the menu at once;
  dedicated servers on Linux are not tested yet.
- No HDR, no ultrawide picture (letterbox or stretch).
- The builds are not code signed yet: Windows 11's Smart App Control can refuse `rexruntime.dll` (Bad Image,
  0xc0e90002). It is not a broken download.

## 0.2.0 (11 November 2026): Linux, and updates that install themselves

An **alpha playtest** build: expect bugs, crashes and changes, and stats that reset.

**11 November, the Linux AppImage replaced with three fixes:** its first start setup now opens a file browser to
choose your game files (the two choices only highlighted before); controllers work in the menus on every Linux device
(a ROG Ally's built in controller did nothing there; with several pads connected, every one now drives the player); and
in a folder where another game keeps its own `game` folder (King Kong Recompiled's AppImage in Downloads, say) it asks
for your game files and keeps them in `bf1943-game` (it took the other game's files for its own and skipped the setup
before). Its name no longer carries the version: `BF1943-Reborn-linux-x86_64.AppImage` in every release, so a Steam
shortcut keeps working when a new download goes over the old file. The Windows downloads are unchanged.

**New**
- **Linux.** Battlefield 1943 Reborn runs on Linux as an AppImage (`BF1943-Reborn-linux-x86_64.AppImage`). It
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
