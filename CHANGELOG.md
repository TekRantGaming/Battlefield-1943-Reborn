# Changelog

## 0.4.6.3 (14 November 2026): a US Central warning and a new version pop-up

**New**
- **US Central play test warning.** The US Central servers are play testing a new server build. Before you join one,
  a warning says so and recommends the EU servers for a stable game. Connect unlocks after 5 seconds, and Cancel takes
  you back.
- **New version pop-up.** When a new version is out, a pop-up says so on the main menu, with the download's progress,
  Update and Cancel, and a reminder that the servers may turn away an older version. Update shows the download and
  installs it; Cancel leaves it until the next start (the Update button still has it).

## 0.4.6.2 (14 November 2026): the game starts at 60 Hz again

**Changed**
- **The game starts at 60 Hz again**, as in 0.4.6. It still joins a 30 Hz server by starting again at that rate.

## 0.4.6.1 (14 November 2026): the game starts at 30 Hz again

**Changed**
- **The game starts at 30 Hz again**, the original game's rate. It still joins a 60 Hz server by starting again at
  that rate, as before 0.4.6.

## 0.4.6 (14 November 2026): 60 Hz everywhere, Custom Match passwords, Iron Sight and motion blur

An **alpha playtest** build: expect bugs, crashes and changes, and stats that reset. Earlier versions update to it by
themselves, and the servers only let the newest version in.

**New**
- **Custom Match passwords.** Turn on Password under the match's name and type one: the server browser shows a padlock
  by the match, and players are asked for the password before they join. By ThatZaliaSGuy ([pull request 20](../../pull/20)).
- **SMG aiming: Iron Sight or Zoom In.** In Settings, the Thompson and the Type 100 can bring their own sights up to
  your eye instead of the game's zoom (what you see only: aim and spread are unchanged). By ThatZaliaSGuy ([pull request 20](../../pull/20)).
- **Motion blur of its own.** Off, Low, Medium or High, a strength slider that looks the same at every frame rate, and
  blur for moving soldiers and vehicles on or off. The blur when you are hit or near an explosion keeps its own
  switch. By ThatZaliaSGuy ([pull request 20](../../pull/20)).
- **Controller: vehicle camera sensitivity.** A right stick sensitivity of its own for land vehicles and boats. By ThatZaliaSGuy ([pull request 20](../../pull/20)).
- **Animation smoothing** (experimental, off by default): in Graphics. By ThatZaliaSGuy ([pull request 20](../../pull/20)).

**Changed**
- **The game now plays at 60 Hz.** It starts at 60 ticks a second, the official servers' rate, so it joins them
  straight away. Quick Play picks a 60 Hz server first, and the experimental server warning is gone for 30 and
  60 Hz servers (a 75 or 100 Hz server still warns you first).
- **Custom Match: 60 Hz or 30 Hz.** 60 Hz is the default; 30 Hz is the original game's rate.
- **Server hosts:** the server manager now defaults to 60 Hz (recommended), and each server can have its own tick
  rate in its Edit page: 60, 30, or the experimental 75 and 100.
- **Menus:** folder tabs in the server browser and Controls; the pause menu is now "Menu", shows the match's tick rate
  and uses a wide screen's width with a larger map; Settings categories light up under the mouse, and HUD scale and
  Minimap size show the game's own crosshair and compass at the size you pick; Keyboard bindings and Remap gamepad are
  one page with a controller column. By ThatZaliaSGuy ([pull request 20](../../pull/20)).
- **LAN matches:** after one, the game starts again on the main servers, and hosting a LAN match while the game runs
  restarts it to host. By ThatZaliaSGuy ([pull request 20](../../pull/20)).
- **Hotfix updates.** The game, the servers and the server manager now understand hotfix versions such as 0.4.6.1 and
  0.4.6.2, so small fixes can be sent out quickly between updates.

**Fixed**
- **The mouse can click Back** on Stats & Awards, Leaderboards, Achievements, Join friends and the pause menu's
  Scoreboard ([issue 19](../../issues/19)).
- **Menu music on Linux:** the AppImage now plays the menu music ([issue 21](../../issues/21)).

## 0.4.5 (14 November 2026): bug fixes

**Fixed**
- General bug fixes.

## 0.4.4 (14 November 2026): temporal upscalers removed

**Changed**
- **The temporal upscalers are removed.** AMD FSR 3.1, Intel XeSS, NVIDIA DLSS and TAA are no longer in Settings, and
  if you had one on, your game goes back to its normal antialiasing. The spatial upscalers AMD FSR 1 and NVIDIA NIS stay.

## 0.4.3 (14 November 2026): AMD FSR 4 removed

**Changed**
- **AMD FSR 4 is no longer in the Upscaler row.** If you had chosen it, your game uses AMD FSR 3.1 instead, or Off
  where FSR 3.1 does not run on your card. AMD FSR 3.1, Intel XeSS, NVIDIA DLSS and TAA stay.

## 0.4.2 (13 November 2026): only the upscalers your graphics card runs

**Fixed**
- **The Upscaler row lists only what your graphics card runs.** AMD FSR 4 shows on a Radeon RX 9000 only, AMD FSR 3.1
  and Intel XeSS where AMD's and Intel's own libraries say they run on your card, and NVIDIA DLSS on a GeForce RTX.
  TAA works everywhere. If you had chosen one your card does not run, FSR 4 becomes FSR 3.1, and the others Off.

## 0.4.1 (13 November 2026): the mouse in every vehicle seat, Custom Match with LAN, more antialiasing and upscalers, Enhancements, players on the pause map

An **alpha playtest** build: expect bugs, crashes and changes, and stats that reset. Earlier versions update to it by
themselves, and the servers only let the newest version in.

**New**
- **Settings > Enhancements.** A new page for what Reborn adds to the game. Lettered flags is its first row: turn it
  off and every map and the HUD show the game's own plain flags again.
- **Mouse buttons 4 and 5 can be bound.** Settings > Controls > Keyboard bindings now takes the mouse's side buttons,
  and its middle button, which it could not take before. The HUD's prompts show them too ([issue 14](../../issues/14)).
- **EU Central.** Servers can now say they are in EU Central, with the European Union's flag in the server browser.
  The official servers move to it.
- **Mouse and keyboard in every vehicle seat.** Tank guns, turrets and AA guns turn with the mouse as planes fly:
  the mouse's speed is the gun's speed, and a still mouse leaves the gun still (this also fixes the AA gun of
  [issue 13](../../issues/13)). The game knows your seat from the game itself, and drivers without a gun look around
  freely. Settings > Controls has plain Invert camera, Invert vehicle and Invert flight switches; your 0.4.0 settings
  are carried over. By ThatZaliaSGuy ([pull request 16](../../pull/16)).
- **Custom Match** (was Create Private Match): a name, the map, the tick rate, Online or LAN, Show in browser, UPnP or
  a port of your own. A LAN match needs no internet: your PC answers the sign-in and the match list, and the match is
  on the server browser's new LAN tab. By ThatZaliaSGuy ([pull request 16](../../pull/16)).
- **Bindings with two keys and two mouse buttons** for each action, the mouse wheel too; hold Esc to clear one. By
  ThatZaliaSGuy ([pull request 16](../../pull/16)).
- **More antialiasing and upscalers.** SMAA (four presets) and Intel CMAA 2 beside FXAA, and an Upscaling section with
  AMD FSR 4 and FSR 3.1, Intel XeSS, NVIDIA DLSS and a TAA. By ThatZaliaSGuy ([pull request 16](../../pull/16)).
- **HUD scale and minimap size**, **Change Team** in the pause menu, and **the deploy screen with the mouse** (kits,
  buttons and spawn points to click). By ThatZaliaSGuy ([pull request 16](../../pull/16)).
- **Experimental 60 Hz servers.** A server can now run at 60 ticks a second (75 and 100 are there too). They are
  experimental: joining one from the server browser warns you first, and Quick Play never picks one. Your game takes
  the server's rate when it joins. The rate itself by ThatZaliaSGuy ([pull request 16](../../pull/16)).
- **Players on the pause menu's map.** Your team mates are blue arrows (your squad green) pointing the way each one
  faces, also in an air raid's bomber view. An enemy is a red arrow, only while the game itself shows him on your
  map.

**Changed**
- **Mouse flying**: Normal is now a flight stick, pull back to climb (as a flight sim's), the other way round from
  0.4.0; Invert flight turns it round. The air raid's bombers steer left and right only.
- **Aim assist** is never applied with the mouse; a controller's is at 75% by default, with a slider.
- **Graphics settings wait for Apply.** On the Graphics page your changes take effect with Apply settings, beside
  Restore defaults; Controls opens Keyboard bindings and Button remapping with buttons. By ThatZaliaSGuy
  ([pull request 16](../../pull/16)).
- **Distant textures a little softer, without the sparkle,** whenever anisotropic filtering is on (every preset but
  Low): the game's own sharpening of far textures is left out, as anisotropic filtering does that job. By
  ThatZaliaSGuy ([pull request 16](../../pull/16)).
- **Larger text in the menus.** The small captions and the rows' text are easier to read on every screen.
- **Less CPU for the same frames**, from a lock the game takes thousands of times a frame: about 5% less on Linux (a
  ROG Ally), and a little less on Windows.

**Fixed**
- **A crash on dedicated servers**: a read of an empty memory address on one of the game's threads no longer closes
  a server, as it has not closed the game since 0.3.0.
- **Sights on ultrawide screens**: a sniper scope's crosshair and mil dots and a plane's gunsight stay in one piece and
  in place, and a world marker never takes pieces of the HUD with it. By ThatZaliaSGuy
  ([pull request 16](../../pull/16)).

## 0.4.0 (13 November 2026): a new look, ultrawide screens, mouse flying, lettered flags, a map in the pause menu

An **alpha playtest** build: expect bugs, crashes and changes, and stats that reset. Earlier versions update to it by
themselves, and the servers now only let the newest version in.

**New**
- **A new look for every menu.** The main menu, the server browser, Settings, the pause menu and the first start are
  redrawn with the game's own lettering and artwork. Quick Play joins the busiest server with room.
- **Lettered capture points.** Every flag has its letter inside it: in the row under the ticket bars, on the markers
  in the world, on the minimap and on the deploy map. A point keeps its letter in every match, so Wake Island's
  Airfield is always C. The row now shows the points in order, each in the colour of the team that holds it.
- **A map in the pause menu.** The whole island with the lettered flags in their owners' colours, what your team can
  see, and an arrow for where you are and where you look: in an air raid, your bombers. One button changes between
  the island and the whole battle with the carriers.
- **Ultrawide screens.** On a 21:9, 32:9 or other wide screen the game fills the screen with a wider view: more to
  each side, nothing stretched. Settings > Display > HUD layout puts the minimap and ammo at the screen's sides
  (Edges) or keeps the whole HUD in 16:9 in the middle (Centered). Menus, the deploy screen and loading screens stay
  together in the middle. Aspect ratio: Fill screen or Letterbox 16:9, at the next start. By [@ThatZaliaSGuy](https://github.com/ThatZaliaSGuy)
  ([pull request 11](../../pull/11)).
- **Mouse flying.** In planes the mouse moves the stick: right and left roll, forward and back pitch, and the stick
  springs back to the centre when the mouse stops. A and D keep the rudder. Which way is up follows Vertical fly
  (Settings > Controls > Game controls). By [@ThatZaliaSGuy](https://github.com/ThatZaliaSGuy) ([pull request 11](../../pull/11)).
- **Free look in vehicles.** In a driver's or passenger's seat the mouse turns the view one to one, as on foot; in
  third person it turns round the vehicle. Only your own view moves. By [@ThatZaliaSGuy](https://github.com/ThatZaliaSGuy)
  ([pull request 11](../../pull/11)).
- **Sharper mouse aim in vehicles.** Gun aim and plane steering go straight into the game's own input, not through
  a controller stick: no dead zone, and a gun stops when the mouse stops. A controller works as before. By
  [@ThatZaliaSGuy](https://github.com/ThatZaliaSGuy) ([pull request 11](../../pull/11)).
- **Create Private Match** is back on the main menu: a match on your own PC for you and your friends, who join with
  Direct connect. Put back by [@ThatZaliaSGuy](https://github.com/ThatZaliaSGuy) ([pull request 11](../../pull/11)).
- **What's new.** After an update the game shows you that version's notes, once, when its shaders are ready.
- **Server regions and tick rate.** The server browser shows where each server is, with its flag, and its tick rate
  in Hz. A region filter is beside the search box.
- **Server messages.** A server can tell everyone on it about a game update, a restart or anything else: the message
  shows in a box at the top of the screen for ten seconds, with a sound.
- **LAN servers.** Direct connect now also reaches a server that runs on its own, off the main server list: type its
  address, as its host gave it to you.
- **A new server manager** for hosts: a page in your browser with your servers, their players, the region, LAN
  only mode, messages to the players with a countdown, and updates that install themselves.
- **Update needed.** Servers only let the newest version in. An older game gets a message with Update now, which
  downloads and installs the update and starts the game again.
- **Voice chat is louder.** Every voice now plays at the same level whatever its microphone, and Voice volume goes up
  to 500%.
- **Vehicle field of view** has its own slider, and your weapon and hands widen with the field of view.
- **Button prompts follow what you use.** Pick up the controller or touch the keyboard and mouse mid match, and the
  prompts in the menus and the HUD change with you.
- A message tells you when the game could not start, with where its log is.

**Changed**
- **Classic only.** Modern movement and aiming is gone: every server plays the game as it was, and nothing asks you
  to choose.
- **Shaders are prepared at the start**, as a step of the first start and once after each update, never when you
  join a server.
- **Graphics presets** are chosen with A or a click; moving over them no longer changes your settings.
- **GI distance** in Settings goes up to Far. Full is in the F8 panel.
- Settings sliders can be dragged with the mouse.

**Fixed**
- **Plants on roads.** From the second map of a session on, grass and black fronds could stand on roads and in
  yards. Every map now looks as the first one did.
- **Aim toggle on the keyboard** now stays on until you press it again ([issue 9](../../issues/9)).
- **Mouse aiming in vehicles** went the wrong way with the game's Vertical look set to Inverted.
- **Updates that did not start**: when the check at the start got no answer, the main menu now asks again by itself.
- **Linux: rough lettering.** The chat, the voice list and the performance overlay were drawn in a small pixel font
  on Linux. They now use a proper one.
- **Flying with the mouse.** A plane's control surfaces were taken for a gun turret, so the mouse could not fly a
  plane. By [@ThatZaliaSGuy](https://github.com/ThatZaliaSGuy) ([pull request 11](../../pull/11)).
- General bug fixes.

**Known issues**
- Voice chat has no echo cancelling: with Open mic, use headphones.
- The first person weapon and hands still move 30 times a second at high frame rates.

## 0.3.0 (12 November 2026): mouse aiming in vehicles, graphics presets, smooth play at low frame rates

An **alpha playtest** build: expect bugs, crashes and changes, and stats that reset. 0.2.0 to 0.2.3 installs update
to it by themselves at their next start, on Windows and Linux.

**New**
- **Mouse aiming in vehicles.** A tank's gun, the jeep's machine gun and the landing craft's guns follow the mouse
  directly instead of acting like a joystick: the gun turns to where you aim as fast as the vehicle allows, a fast
  turn is no longer cut short, and moving the mouse up aims up, as on foot. Vehicles have their own invert setting:
  Settings > Controls > Mouse vertical in vehicles ([issue 3](../../issues/3), [issue 5](../../issues/5)).
- **Graphics presets.** Settings > Graphics starts with Graphics preset: Potato, Low, Medium, High or Ultra (Custom
  once you change a row). Potato and Low are for older and low end PCs, and Ultra draws at full resolution with the
  highest shadows, ambient occlusion and global illumination. The automatic graphics settings now pick Low, Medium or
  High.
- **Aim assist on or off**, for the controller and for keyboard and mouse: Settings > Controls. Both are on by
  default ([issue 4](../../issues/4)).
- **Game language**: Settings > Gameplay > Game language: English, French, German, Italian, Spanish or Japanese, the
  game's own translations of its menus, HUD and loading screens. It applies at the next start
  ([issue 2](../../issues/2)).
- A crash now writes where it happened into `user\bf1943.log`, so a log sent with a report tells us much more.

**Fixed**
- **Stutter at low frame rates.** Below 30 FPS the motion smoothing jumped back every few frames, which made the game
  stutter on slower PCs. It now stays smooth: at 24 FPS the camera moves about three to four times more evenly.
- **A crash some players had right after a match ended**: a read of an empty memory address on one of the game's
  threads no longer closes the game.
- **A black outline round ships against the water** (Wake Island's carrier spawn) and **a white glow round palm
  leaves and roofs** against the sky: the water and the haze read the picture one pixel off at our higher
  resolutions. Fixed for every resolution.
- **Effects that vanish for a moment** the first time they show up: 226 more graphics pipelines are now prepared
  before you play, many of them from the data players shared.
- Changing seat in a vehicle no longer swings the gun from mouse movement made before you got there.
- Server package: Start finds a Python that works and says plainly when there is none.

**Known issues**
- Avoid flying with mouse and keyboard for now: take a boat to get to land, or use a controller to fly. This will be
  fixed in a later update; mouse and keyboard work everywhere else.
- Voice chat has no echo cancelling: with Open mic, use headphones. Push to talk and the chat keys are keyboard keys:
  with a controller only (Steam Deck, ROG Ally), use Open mic, or map a back button to B in Steam Input.
- The first person weapon and hands still move 30 times a second at high frame rates.
- Stats, awards and leaderboards reset when the official backend restarts.
- Linux: the Vulkan renderer needs X11 or XWayland; the game's tutorial (Help) can drop back to the menu at once;
  dedicated servers on Linux are not tested yet.

## 0.2.3 (11 November 2026): general bug fixes

An **alpha playtest** build: expect bugs, crashes and changes, and stats that reset. 0.2.0, 0.2.1 and 0.2.2 installs
update to it by themselves at their next start, on Windows and Linux.

**Fixed**
- General bug fixes.

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
