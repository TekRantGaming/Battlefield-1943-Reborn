<p align="center">
  <img src="docs/images/alpha-playtest-banner.png" alt="Battlefield 1943 Reborn: early playtest, alpha, subject to change" width="100%">
</p>

> [!WARNING]
> **Alpha playtest.** Battlefield 1943 Reborn is in early playtesting. Expect bugs, crashes and big changes between
> builds, and stats that reset. Tell us what you find on our [Discord](https://discord.gg/8eakYSvBZS).

# Battlefield 1943 Reborn

Battlefield 1943, the 2009 Xbox 360 game by DICE, running natively on Windows, with its online multiplayer brought
back on servers that anyone can run.

This is a fan project by TekRantGaming. It is not made, endorsed or supported by Electronic Arts or DICE. It contains
no game files: you need your own copy of the Xbox 360 game (Xbox Live Arcade, title ID `5841097E`).

**Latest build: [0.1.33 preview](../../releases/latest)** &nbsp;·&nbsp; **Discord: [discord.gg/8eakYSvBZS](https://discord.gg/8eakYSvBZS)**

- [Download and play](#download-and-play)
- [Requirements](#requirements)
- [Features](#features)
- [Controls](#controls)
- [Settings](#settings)
- [Online play and servers](#online-play-and-servers)
- [Known issues](#known-issues)
- [How it works](#how-it-works)
- [Privacy](#privacy)
- [Credits and licences](#credits-and-licences)
- [Legal](#legal)

## Download and play

1. Download `BF1943-Reborn-<version>-windows-x64.zip` from the [Releases](../../releases/latest) page and unzip it
   somewhere you can write to (for example `C:\Games`, not Program Files).
2. Start `bf1943.exe`.
3. The first time, select your own Battlefield 1943 (Xbox 360) package: the single file in `5841097E\000D0000` from
   your console's storage, or a folder that holds it. It is installed into the folder `game`, once. The first start
   then prepares the game's shaders, so effects do not stutter the first time you see them.
4. Choose your name and your playstyle (Classic or Modern, see [Features](#features)).
5. Pick a server, or press **Play online** to join the busiest one.

Your settings, logs and crash reports stay in the folder `user` next to the game. To update to a new build, copy its
files over your current folder: your `game` and `user` folders stay.

If Windows says *"rexruntime.dll is either not designed to run on Windows or it contains an error"* (Bad Image,
0xc0e90002), it is not a broken download: the builds are not code signed yet, and Windows 11's Smart App Control has
not cleared this build's files.

## Requirements

- Windows 10 or 11, 64 bit.
- A graphics card with Vulkan (NVIDIA GTX 10 series, AMD RX 5000 series, Intel Arc or newer recommended).
- Your own copy of Battlefield 1943 for Xbox 360: its package file from your console.
- To host a server: Windows (10, 11 or Server) and [Python 3.12](https://www.python.org/downloads/).

## Features

**The game**
- All four maps: Wake Island, Guadalcanal, Iwo Jima and Coral Sea.
- Up to 24 players a server, on foot, in jeeps, tanks, boats and planes, with the air raid, as in the original.
- Two playstyles. **Classic** is the original game. **Modern** has quicker starts and stops, faster aiming with no
  turn lag and quicker shots after a sprint or a jump, with aim assist kept. Each server plays one or the other, so
  nobody has an edge: Play online picks servers that play your choice, and your game always plays what the server you
  join plays.
- The game's own tutorial (Help), and our Stats and Awards, Achievements and Leaderboards pages.

**Online**
- A server browser with the official servers and everyone else's (Community), each with its map and players.
- **Play online** joins the busiest server that plays your playstyle.
- **Create Private Match** hosts a match on your own PC: friends join it with Direct connect in the server browser.
- A server package for anyone to host their own dedicated servers, listed for everyone.

**On PC**
- A native Windows program with a Vulkan renderer, at your screen's resolution and refresh rate.
- Any frame rate: a limit from 30 to 240 or none, and VSync. The game still runs its world 30 times a second as on
  the console, but the camera turns on every frame, and soldiers, vehicles and vehicle cameras move smoothly between
  the game's updates.
- Graphics: render resolution up to 2160p (three times the console's 720p), shadow quality up to four times the
  original, FXAA, sharpening (AMD CAS), texture filtering up to 16x, ambient occlusion (Intel's XeGTAO, with strength
  and quality settings) and global illumination (one bounce, with quality and distance settings).
- Grass and shrubs on Wake Island, Guadalcanal and Iwo Jima, placed from each level's own data, drawn as far as the
  whole map.
- Field of view, motion blur on or off, camera shake and its strength, brightness and gamma.
- Keyboard and mouse beside controllers, with rebindable keys and the mouse aiming vehicle guns and turrets directly.
- Volume for master, music, effects and voices, menu music and sounds.
- Performance metrics in the corner (F2: off, FPS only or full), and crash and hang reports in `user\crashes`.

## Controls

Keyboard and mouse are on by default, beside any controller. Every key can be changed in Settings > Controls >
Keyboard bindings.

| Key | On foot | In a vehicle |
|---|---|---|
| W A S D | move | W throttle, S brake, A D turn |
| Mouse | look and aim | aim the guns and turret |
| Left mouse button | fire | fire |
| Right mouse button | aim down sights | second weapon (bombs in a plane) |
| Space | jump | change seat |
| E | enter a vehicle, use, pick up | get out |
| R | reload | |
| F | draw knife (the Infantry class's wrench) | |
| G | grenade, explosives | free look in a plane |
| Q or the mouse wheel | switch weapon | second weapon (bombs in a plane) |
| Shift | sprint | |
| C or Ctrl | crouch | |
| V or Up | | vehicle camera (the bomb sight in the air raid) |
| Tab | command (tap), score (hold) | the same |
| Esc or Enter | pause menu | pause menu |
| F2 | performance metrics | the same |

Planes: W is the throttle, A and D the rudder, the mouse pitches and rolls, the left mouse button fires and the right
drops bombs. On a controller planes fly as on the Xbox 360: left trigger throttle, right stick pitch and roll, left
stick rudder, right bumper bombs, left bumper free look.

Controllers: Xbox controllers play as on the console, with the game's own button layouts (Normal, Southpaw, Lefty)
for soldiers, land vehicles and planes. Button prompts can show Xbox 360, Xbox Series, PlayStation 3, PlayStation 5
or Steam Deck buttons, or the keys.

## Settings

Everything is in our Settings menu, from the main menu and the pause menu.

- **Display**: window mode (windowed or borderless), monitor, window size, resolution, refresh rate, aspect ratio
  (the game is 16:9: letterbox or stretch), brightness, gamma, VSync, frame rate limit, field of view, motion blur,
  camera shake.
- **Graphics**: antialiasing (FXAA), render resolution, shadow quality, sharpness, texture filtering, ambient
  occlusion, global illumination, grass distance, HD textures.
- **Audio**: master, music, effects and voices volume, menu music and sounds, the game's sound system and
  voiceover language.
- **Controls**: Keyboard and mouse on or off, button prompts, camera turning (every frame or 30 times a second as on
  the Xbox 360), mouse sensitivity (with its own for aiming down sights and for vehicles), aim hold or toggle, mouse
  vertical, controller camera sensitivity, response, dead zone, inversion, vibration and its strength, button
  remapping, keyboard bindings, and the game's own control layouts, vertical look and vertical fly.
- **Gameplay**: your name, your playstyle, performance metrics, and sharing (see [Privacy](#privacy)).

## Online play and servers

EA's servers for Battlefield 1943 are gone. Reborn brings online play back with its own main backend, which every
player logs in to and which lists every server.

- **Official servers** are run by TekRantGaming. During the playtest the official list holds the Playtest Server;
  others come and go for test events.
- **Community servers**: anyone can host dedicated servers with `BF1943-Reborn-Server-<version>-windows.zip`. Each
  server has its own name, description, maps, playstyle and pause between rounds, and shows in everyone's server
  browser under Community. See [docs/SERVER.md](docs/SERVER.md).
- **Private matches**: Create Private Match hosts on your PC. Friends outside your home need UDP port 18300 open on
  your router; the game asks the router for it (UPnP) unless you choose to open it yourself.

## Known issues

This is an alpha. The main ones in 0.1.33 preview:

- **Flying a plane with the mouse is very hard.** The mouse moves the plane's stick only while it moves, and S holds
  free look in a plane. A new mouse flying mode is being worked on. Controllers fly as on the Xbox 360.
- The first person weapon and hands still move 30 times a second at high frame rates (the view itself turns on every
  frame).
- Stats, awards and leaderboards are kept only while the official backend runs: they reset when it restarts.
- No HDR, and no ultrawide picture (the game is 16:9: letterbox or stretch).
- Windows only for now.
- The builds are not code signed yet (see [Download and play](#download-and-play)).
- HD textures can be imported (Settings > Graphics), but no texture pack has been published.

Report anything else on [Discord](https://discord.gg/8eakYSvBZS), with the files from `user\crashes` for a crash.

## How it works

- **Static recompilation.** The Xbox 360 program is translated ahead of time into native 64 bit Windows code with the
  ReXGlue SDK (derived from the Xenia emulator), and runs on its runtime (`rexruntime.dll`), which stands in for the
  console's system services. The game's own code runs natively; it is not emulated.
- **NativeXRenderer.** The game's Xbox 360 graphics work is drawn with Vulkan by NativeXRenderer
  (`rexgpu-bfnative.dll`), with this project's additions: ambient occlusion, global illumination, grass, sharper
  shadows and motion between the game's updates.
- **Online.** The game talked to EA's servers and Xbox LIVE. Reborn answers it with its own backend
  (`server\plasma.py`: login, server list, stats), and each dedicated server has a host (`server\gmhost.py`) that links
  players to it. A dedicated server is the game itself, run as a server with no window.
- **Our menus.** The first start setup, main menu, server browser, pause menu and settings are this project's own,
  drawn over the game, which keeps its own HUD.

The development repository is private for now. This repository holds the releases, the changelog and the guides.

## Privacy

- **Share graphics data** (on by default, Settings > Gameplay > Sharing) sends the graphics pipelines your game builds
  (render settings and shader numbers: no game files, no name, nothing about you) to our server, at most every 15
  minutes and only when something new was built, so the next update has them ready and everyone gets less stutter.
- Playing online, the backend sees the name you chose and your IP address, as any game server does.
- The game's own Telemetry setting changes nothing: its reports went to EA's servers, which are gone.
- Settings, logs and crash reports stay on your PC, in the folder `user`.

## Credits and licences

- **ReXGlue SDK** (BSD 3 Clause), derived from Xenia: the recompiler and runtime.
- **NativeXRenderer** by Gustavo Garcia (MIT): the Vulkan renderer.
- **GHMetalRecomp** (MIT): its NativeXRenderer profile, which Battlefield's started from.
- **King Kong Recompiled** by TekRantGaming and contributors (MIT): the ambient occlusion, global illumination and
  camera work, and much of what we learned.
- **DiligentCore** (Apache 2.0), **SPIRV Cross** (Apache 2.0), **volk** (MIT): the renderer's foundations.
- **XeGTAO** by Intel (MIT), **AMD FidelityFX** (MIT), **NVIDIA Image Scaling** (MIT), **SMAA** (MIT style) and
  **NVIDIA DLSS** (NVIDIA RTX SDKs licence): libraries the renderer is built with.
- **FFmpeg** (its decoders only) and **libmspack**, both LGPL 2.1, in the runtime.
- **vgmstream** (ISC): the layout of the game's menu music.
- **Oswald** (SIL Open Font Licence): the menus' font.
- Button prompts: **Xelu's Free Controllers and Keyboard Prompts** by Nicolae Berbece (CC0) and **Input Prompts** by
  Kenney (CC0).

Every download includes the full licence texts in its `licences` folder.

## Legal

Battlefield and Battlefield 1943 are trademarks of Electronic Arts Inc. All rights to the game belong to their owners.
This project is not affiliated with Electronic Arts or DICE, and it contains no game files or game assets: everyone
installs their own copy. Please do not share game files.
