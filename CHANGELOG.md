# Changelog

## 0.4.8.3 (28 November 2026): a hotfix

Earlier versions update to it by themselves, and the servers only let the newest version in. Every update builds your
shaders again from scratch at the first start.

**Keyboard and mouse**
- **The keyboard works in vehicles again.** Since 0.4.8.1 the vehicles' key sets could be saved empty, so the keys did
  nothing in a vehicle. An empty set is filled once from your infantry keys; a set with any key of your own is left as
  it is.
- **The HUD shows keyboard prompts again,** not the controller's buttons.
- **The middle mouse button and mouse buttons 4 and 5** can be bound again on the Controls page.
- **A held key no longer turns a vehicle the wrong way** when the game window loses focus.

**Bots**
- **Bots knife through the fences** they get stuck on.
- **Far fewer bots in the water on Wake Island:** a boat's crew stays aboard until a shore is near, and bots on a
  carrier wait for the next boat instead of swimming.
- **Bots no longer get into a vehicle with an enemy in any of its seats.**

**Graphics**
- **Reflections is now Original or Enhanced** (the planar reflection). If you had SSR, you now have Enhanced.
- **The electric skin aimed in** looks as it does at the hip; only the sights are kept clear.
- **Rifles stay wet in the rain** when you aim down the sights.
- **The vehicle field of view no longer jumps** for a moment in planes and in vehicles with their own slider.

**Custom Match**
- **Weather and time decides the sky:** on, the weather system and volumetric clouds; off, the game's own sky, by day
  and at night.

**For server hosts**
- **A Weather switch for each server** in the server manager: the weather system and volumetric clouds without
  running Night Ops or Daybreak. It is off for a new server; a server that already had rain or fog set gets it on.
  Night Ops and Daybreak always have it, and the official Classic servers stay as they are.
- **An updated server stays listed:** updates leave no older program files behind, so the build check passes.

**Linux**
- **With RecompLauncher, your settings, saves, logs and crash reports survive updates:** they are kept in the
  launcher's data folder, copied once from the old one. By [@Xarishark](https://github.com/Xarishark) ([pull request 88](../../pull/88)).

**Thanks**
- Thanks to ThatZaliaSGuy for this hotfix's keyboard, bots, graphics and weather work ([pull request 98](../../pull/98)); to Xarishark for the
  RecompLauncher data folder; and to everyone who tested and reported.

## 0.4.8.2 (27 November 2026): a hotfix

Earlier versions update to it by themselves, and the servers only let the newest version in. Every update builds your
shaders again from scratch at the first start.

**Fixed**
- **Keyboard and mouse work again.** 0.4.8.1 turned Keyboard & mouse off for every player who had never changed it,
  leaving a controller only. It is on again, and a keyboard, mouse and controller all work together as before. If
  you turned it off yourself in Settings, Controls, it stays off.

## 0.4.8.1 (27 November 2026): a hotfix

Earlier versions update to it by themselves, and the servers only let the newest version in. Every update builds your
shaders again from scratch at the first start.

**Menus**
- **A server type page** before the Server Browser and Quick Play: All, Classic, Night Ops and Daybreak.
- **In the Server Browser:** a BOTS column (Yes or No), a TYPE column with Classic, Night Ops and Daybreak icons, and
  long server names that scroll, with a controller too.
- **The Server Browser no longer jumps back** to the main menu at the first start after an update.

**Custom Match**
- **Server type:** Classic, Night Ops or Daybreak, run as on the servers.
- **Server size:** 24, 32 or 64, and the bots fill up to the match's size.
- **Tickets on Auto** by default, as on the servers.

**Bots**
- **No more swimming bots** on Guadalcanal and Iwo Jima: each map uses its own water level.
- **Bots learn new maps** on empty servers.
- **Guadalcanal's servers no longer slow down** through a round: fewer stuck bots and faster path finding.
- **The official servers' bot data** comes with the game and the server packages.
- **Team balance counts real players;** the bots keep the team totals even.

**Graphics, moved once for everyone when you update**
- **Transparent edges Smooth** and **Animation smoothing off** for everyone, whatever your preset.
- **331 more ready-made shaders** from players' 0.4.8 uploads (3,371 in all): less shader stutter on every map.
- **Planar sea reflections:** Settings, Graphics, Enhancements, Reflections is now Off, SSR or Planar.
- **Graphics cards with a 128 MB buffer limit** draw correctly.

**Controls**
- **Gun, passenger and stationary gun seats** use their vehicle's key bindings.
- **W no longer aims down the sights** in gun seats, and the zoom key works there again.
- **Seat keys 1 to 9** switch seats in ground vehicles and boats (new rows on the Ground vehicles and Boats tabs).

**Fixes**
- **A sound crash fixed,** and the wait for the game's mixer no longer keeps a CPU core busy.
- **No more crash when quitting** after a match.
- **Linux:** no more hang when quitting.
- **ROG Ally:** the menus are the right size at the first start.

**For server hosts**
- **Night Ops and Daybreak work on every 0.4.8.1 server,** Linux included, straight from the package and after
  updates: no files to add by hand.
- **A new option:** bots keep playing with no humans on the server (off by default).
- **Linux servers:** faster and steadier with many bots, and no more crashes on hosts with little shared memory.
- **General bug fixes.**

**The project**
- **The game now runs on ReXGlue SDK v0.11.**
- Thanks to ThatZaliaSGuy for the SDK v0.11 port, the seat and zoom controls, the sound crash fix and planar
  reflections; and to everyone who tested and reported.

## 0.4.8 (27 November 2026): Night Ops, Daybreak, weather, bots and bigger servers

Earlier versions update to it by themselves, and the servers only let the newest version in. Every update builds your
shaders again from scratch at the first start.

**Night Ops and Daybreak**
- **Night Ops:** a new kind of server for night battles. The round is held at dusk, and a storm builds as the tickets
  fall: a clear, starry night at first, then the clouds gather, the rain rolls in and the final push is fought in the
  full storm.
- **Daybreak:** a new server type. The round starts as the sun rises, and the weather can bring rain or a storm as the
  battle goes on.
- **Lights at night:** lit building interiors, street and work lamps, control towers, red lights on masts, buoys at the
  map's edge, vehicle headlights, and Iwo Jima's lighthouse. Muzzle flashes, tracers, fire and explosions light what is
  round them.
- **In the Server Browser:** Night Ops servers carry a NIGHT mark, with a filter of their own.

**Weather and sky**
- **Where:** on Night Ops and Daybreak servers and in Custom Match; regular servers keep the original look.
- **A day and night cycle:** the sun moves, with its light and shadows on everything; at night, the moon and the stars.
- **A sky of its own** with clouds you can fly through, thunderstorms that roll in from the sea with lightning and
  thunder, and **the hurricane**, with a calm eye and spiral rain bands.
- **Fog, rain and wet surfaces:** ground fog that drifts over the shore, rain where the storm is, and guns, hands,
  jeeps and tanks that get wet and dry again.
- **Your own settings:** Lightning and Storm shake (Settings, Graphics, Weather) and Weather volume (Settings, Audio).

**Bigger servers and bots**
- **Servers of 24, 32 or 64 players:** 24 stays as the original, 32 is the new default for new servers. Tickets scale
  with the size, and the pause Scoreboard scrolls.
- **Bots fill the empty seats** where the server's owner turns them on, and give way when a player joins. They play
  the flags, use their kit, ride and drive vehicles, take the landing craft to the beach, and show as BOT on the
  scoreboard. They sit out Coral Sea. Kills of bots are not counted in your stats.

**Custom Match, rebuilt**
- **A page of its own** with Match and Weather tabs: game mode, tickets, match time, bots, an ordered map rotation,
  the time of day and the storm. Host online or on LAN, with a controller, mouse or keyboard. The host gets F9 for the
  time and the weather while the match runs.

**Unlockables**
- **The electric weapon skin** for the Thompson and the Type 100: dark metal, gold wood, smoke and lightning, and a
  charge that runs over the gun on a reload. Unlock it by playing one full Night Ops match, then equip it from the main
  menu's Unlockables. For now only your own game shows it.

**Controls**
- **Planes on keyboard and mouse:** yaw, pitch and roll can each go on two keys, two mouse buttons or a mouse movement
  (Settings, Controls, Keyboard & Mouse, Bindings, Air vehicles), with a plane mouse sensitivity and invert options of
  their own.
- **Bindings for each kind of vehicle:** Infantry, Ground vehicles, Air vehicles and Boats each have their own keys.

**Graphics, moved once for everyone when you update**
- **The spatial upscaler is on in every preset** at Quality: NVIDIA NIS on an NVIDIA card that runs it, AMD FSR 1 on
  any other card.
- **Soft shadows and SSR reflections** on the High and Ultra presets; **Animation smoothing** on and **HUD scale
  High-Res** for everyone; **Transparent edges Original** and **Storm shake off** for everyone.
- **Sharp menu text** with Windows display scaling, a **Vehicle FOV**, and a message instead of a frozen picture if
  your graphics driver resets.
- **HD textures** stay out of Settings: they are not ready yet.

**Smoother play**
- **60 ticks that feel like 30:** movement, jumping, swimming, the gun's sway and recoil behave as they did at the
  original 30, so your sights stay lined up while you move.

**Sound**
- **Choose your devices** for game sound, chat output and chat input in Settings, Audio.

**Menus**
- **Every player's ping** on the scoreboards, and a warning before you join a server far away.
- **A new name page** at the first start, which now works fully with a controller.
- **A stormy news window.**

**For server hosts**
- **In the server manager,** Windows and Linux:
- **Players, bots and tickets** on each server's page, **Night Ops and Daybreak** switches, your own **ban list,
  kicks and mutes**, a **Dashboard**, an optional **public community page**, and **F9 for admins** in the match.
- **General bug fixes.**

**The project**
- The port's own code is now under the **GNU GPL version 3**; copies of 0.4.7.5 and earlier stay under MIT.
- Thanks to ThatZaliaSGuy for the sky, the weather and the hurricane, bots and Custom Match rules, vehicle bindings,
  wetness and night lights; to Xarishark for RecompLauncher support; and to everyone who tested and reported.

## 0.4.7.5 (25 November 2026): a hotfix

Earlier versions update to it by themselves, and the servers only let the newest version in.

**Fixed**
- **General bug fixes.**

**Thanks**
- To Dickson Wu ([@DicksonWu654](https://github.com/DicksonWu654)) from SEAL & CertiK, for reporting an issue to us
  privately.

The known issues of 0.4.7.4, below, still apply.

## 0.4.7.4 (18 November 2026): a hotfix

Earlier versions update to it by themselves, and the servers only let the newest version in.

**Known issues, please read**
- **Invisible players or black textures.** We are aware that some players can become invisible, or some textures can
  turn black. This is being investigated. If it happens to you, go to Settings, Graphics and choose
  **Rebuild shaders**: your shaders are built again the next time the game starts, and this should fix it. If it does
  not, please be patient for the next hotfix.
- **Can't load into a server?** Third party overlays such as RivaTuner, Overwolf and others can stop the game joining.
  Turn them off if you cannot load into a server.
- **Bots:** please check the FAQ in our Discord about bots coming to the game.

**New**
- **A bigger map in the pause menu.** The pause now opens on a large map of the whole battle. LB and RB (Q and E)
  switch to the Field Report, and X zooms in on the island.
- **Server queues.** Join a full server and you get a place in its queue, with a popup showing your position as it
  moves up, and a Close button to leave it. You join by yourself when your turn comes. The server browser shows each
  server's queue and offers Join Queue on a full one.
- **Rebuild shaders,** in Settings, Graphics: deletes your saved shaders and builds them again at the next start.
- **Five vehicle achievements,** on either side: Steel Behemoth (a tank), Road Warrior (a jeep), Naval Warfare (a
  boat), Air Superiority (a fighter plane) and Death From Above (call in the bombers from an air raid bunker).
- **A new achievement sound,** made on your PC from the Battlefield theme in your own game files.
- **Watch video tutorial.** The first screen that asks for your game files has a button that plays our setup video
  right in the launcher.
- **A trademark sign in your name.** Your soldier name can now end with one ™ (Alt+0153, paste it, or the ™ key on the
  controller keyboard).
- **VSync switches at once,** with no restart, and the frame limit follows your screen's refresh rate. By [@DiXN](https://github.com/DiXN)
  ([pull request 46](../../pull/46)).
- **Linux: host your own Custom Match,** online or on your LAN. By [@SmokeDoggy044](https://github.com/SmokeDoggy044) ([pull request 48](../../pull/48)).

**Changes**
- **Suicide is now REDEPLOY** in the pause menu.
- **Community servers we vouch for** now read TRUSTED COMMUNITY SERVER, and ours read REBORN OFFICIAL SERVER.
- **Server owners:** the server manager's Settings has an Automatic updates switch, on by default. If you turn it off,
  updated players may not be able to join until you update.

**Fixes**
- **Fresh shaders after every update.** Each update now deletes all your saved shaders and builds them again, even if
  your settings were reset, and a half deleted shader folder is built again in full.
- **Sound:** plane engines no longer drop out every few seconds ([pull request 49](../../pull/49)), the Effects volume
  now covers gunfire too ([pull request 50](../../pull/50)), and sounds are no longer cut short at their end
  ([pull request 52](../../pull/52)). By [@SmokeDoggy044](https://github.com/SmokeDoggy044).
- **Smoother rendering:** more of the drawing work spread across your CPU, and faster lookups. By [@DiXN](https://github.com/DiXN).
- **Linux:** VSync no longer stalls a hidden window, and Wayland is used where it is available. By [@DiXN](https://github.com/DiXN).
- **HD textures** are taken out of Settings for now.
- **General bug fixes.**

## 0.4.7.3 (17 November 2026): a hotfix

Earlier versions update to it by themselves, and the servers only let the newest version in.

- **Mute any player's voice** from the scoreboard or the end of round screen: a speaker in each row lights while they
  talk and shows a slash once muted, and your mutes are kept between games.
- **Achievements unlock as soon as their progress is complete,** including progress made before 0.4.7.
- **Report an issue** now keeps your graphics driver's version, and the Contact box says that the name you enter is
  shown publicly.
- **Linux: smoother frame times on kernel 6.11 and newer,** as the faster memory lookup from pull request 28 is now
  really in the Linux builds. By [@DiXN](https://github.com/DiXN) ([pull request 28](../../pull/28)).

## 0.4.7.2 (17 November 2026): a hotfix

Earlier versions update to it by themselves, and the servers only let the newest version in.

- **Fewer server crashes.** Fixed the crashes that took official servers down mid match after 0.4.7.1, and guarded
  the rest of the same kind.
- **Servers recover by themselves.** A server that stops responding now restarts on its own, and Linux servers no
  longer freeze when they hit an error.
- **Steadier connections.** Players stay in their match when a server briefly loses its link to the main backend.
- **General bug fixes.**

## 0.4.7.1 (17 November 2026): a hotfix

Earlier versions update to it by themselves, and the servers only let the newest version in.

- **Promoted community servers.** When you join a community server we've promoted to the Reborn list, you now see
  whose server it is, where to report problems, and the server's own description. Tick "Don't show this again" to skip
  it next time.
- **Fewer server crashes.** Fixed two crashes that could take a whole server down mid match.

## 0.4.7 (16 November 2026): stats, leaderboards and achievements, clan tags, protect your name, a news window

An **alpha playtest** build: expect bugs, crashes and changes, and stats that reset. Earlier versions update to it by
themselves, and the servers only let the newest version in.

**Your progress**
- **Stats & Awards.** Your own service record on the main menu: your rank and the next one, kills, deaths, K/D,
  score, flags, rounds won, headshots and repairs, your time in each kit and vehicle, your results on each map and
  your latest awards. Counted by the Reborn servers from the game itself, for all time, this month and this week.
- **Leaderboards are back.** Score, Kills, K/D, Flags and Time, for all time, this month and this week (LB and RB
  switch boards, X the period), with your own row pinned under the table. The K/D board needs 50 kills.
- **Achievements: all 12, plus 3 new Reborn ones, with a popup and sound.** Each shows your progress and how many
  players have it. Reborn's own: I am reborn (your first multiplayer match), I am forever (a match on a community
  server) and I am inevitable (a Custom Match you host). Settings > Audio has Achievement popups and their volume.

**Your name**
- **Clan tags: [TAG]YourName.** 2 to 4 letters or digits before your name, set on your name page (RB, F4 or a click
  moves between the tag and the name). They show on the menus, the scoreboard, the leaderboards, the kill feed, the
  name tags and in chat. Tags that pretend to be the developers or staff, and rude ones, are refused, and the page
  says why.
- **Protect your name with a recovery code: no email or password.** On your name page, X (F2) protects your name and
  gives you a code, also saved as "BF1943 Reborn recovery code.txt" in your user folder. On a new PC, LB (F3) and the
  code bring back your name, rank and stats. Anyone else using a protected name plays with a number added.
- **Paste your recovery code.** Ctrl+V or Shift+Insert, or a right click and PASTE, in the recovery code, tag and name
  boxes.

**Better matches**
- **Fairer teams, with no points lost when moved.** On Reborn servers, when one team has 2 or more players more than
  the other, players waiting to spawn are moved one at a time, and their score comes with them. A Change Team that
  would leave the teams 2 or more apart is refused, and the scoreboard's team names follow your team after a switch.
  Server hosts: Team balance is on by default for managed servers.
- **Clear messages for full servers and maintenance.** A full server says SERVER FULL with its player count, and while
  the servers are closed for maintenance, the main menu and the server browser say SERVERS CLOSED with the reason.
- **Fewer server crashes.**

**New in the menus**
- **News window.** It opens once at each start with the state of the Reborn servers, our news, what is coming up and
  our videos (they play in the game on Windows; on Linux and the Steam Deck, A opens them on YouTube). The NEWS row on
  the main menu opens it again.
- **Report an issue, with no GitHub account needed.** A three step form on the main menu: say what happened, tick
  what goes with it (you see all of it before it is sent), then send it from the game, open it on GitHub with your
  own account, or copy it.
- **Easier controls setup by device.** Settings > Controls, and the first start's Controls step, open on two cards,
  Keyboard & Mouse or Controller, each with only its own settings and bindings and a picture of your device.
- **New button prompts for every controller**, from Kenney's Input Prompts: Xbox Series, PlayStation 3,
  PlayStation 5, Steam Deck and the keyboard and mouse (the Xbox 360 style keeps the game's own). **Plus the original
  menu sounds** in our launcher, menus, pause menu and on screen keys, made from your own game files. Settings > Audio
  has Menu sounds and their volume.
- **Discord status.** Your Discord profile shows the menus, or your map, mode, team, server and match time; never a
  server address or another player's name. Settings > Gameplay > Discord status.
- **Custom Match:** on a controller, A on Server name or Password opens the on screen keys.

**Smoother and lighter**
- **Much less memory**: about 340 MB less graphics memory and about 190 MB less system memory in a match.
- **Better frame rates, especially on older PCs.** On PCs with few cores the game sleeps instead of spinning while it
  waits; the renderer draws on a thread of its own with four cores or more; an unlimited frame rate is really
  unlimited, and VSync off no longer holds you to your screen's rate. Future frame rendering is a new choice in
  Settings > Display (off by default: more frames, a little more input lag). The menus run at 60 FPS and 30 when idle,
  and a minimised game at 30. With work by [@DiXN](https://github.com/DiXN) ([pull request 28](../../pull/28)).
- **Faster startup and a smaller download.** The window opens sooner, the download is 161 MB smaller, and the Visual C++
  runtime now comes with the game.
- **Audio crackling fixed.** By [@DiXN](https://github.com/DiXN) ([pull request 28](../../pull/28)).
- **Shaders prepared once, right after setup** (and once after each update), on a page of their own.
- **Older processors** (Intel Core 2, AMD Phenom II) get a clear message at start instead of the game closing.

**Linux and Steam Deck**
- **Works on more Linux versions, including Ubuntu 22.04 and Linux Mint 21** (and Pop!_OS 22.04, Zorin 17, Debian 12
  and newer).
- **Wayland:** the window is drawn at its real size, and the Linux build is leaner. By [@DiXN](https://github.com/DiXN)
  ([pull request 28](../../pull/28)).

**Also**
- **Deploy map clicks land where you click**, on every map. By [@Erubadhron](https://github.com/Erubadhron)
  ([pull request 31](../../pull/31)).
- **The server browser's Official tab is now Reborn**, and our servers carry a REBORN badge.
- General bug fixes.

**Thanks** to everyone who played and reported, and to DiXN, Erubadhron, ThatZaliaSGuy (server stall diagnostics,
[pull request 22](../../pull/22)) and Kenney.

## 0.4.6.4 (15 November 2026): no more closing at the shaders page

**Fixed**
- **The game no longer closes at the shaders page of its first start.** On some PCs the graphics driver, or an
  overlay hooked into the game, refuses to set up the game's display a second time, and the game set it up twice as
  it started. It now sets it up once, as it will show it; if a later change still fails, it sets it up afresh with
  V-Sync on instead of closing. Reported by MrMister5305 ([issue 26](../../issues/26)).

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
- **The game now plays at 60 Hz.** It starts at 60 ticks a second, the Project-Official Servers' rate, so it joins them
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
  The Project-Official Servers move to it.
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
- Voice and text chat need the server's update: the Project-Official Servers have it. Community servers need the 0.2.2
  server package; on an older server the game says chat is not available there yet.
- Server package: the crash fix the Project-Official Servers got on 11 November (a server could stop when a game object list
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
