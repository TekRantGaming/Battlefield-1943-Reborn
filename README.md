# Battlefield 1943 Reborn

Battlefield 1943, the Xbox 360 game, running natively on Windows, with its online multiplayer brought back on servers
anyone can run.

This is a fan project by TekRantGaming. It is not made, endorsed or supported by Electronic Arts or DICE. It contains
no game files: you need your own copy of the Xbox 360 game (Xbox Live Arcade, title ID 5841097E).

> Status: early development, private. No release has been published yet.

## What you get

Two downloads come with each release, on the [Releases](../../releases) page:

| Download | For | What it is |
|---|---|---|
| `BF1943-Reborn-<version>-windows.zip` | players | the game for Windows: the first start installs your copy once, then pick a server and play |
| `BF1943-Reborn-Server-<version>-windows.zip` | server hosts | a dedicated server with a setup window: name, map rotation, ports; listed for everyone |

## What it does today

- Runs natively on PC at any frame rate, with the game's own 30 Hz simulation and a smooth view between its updates.
- A Vulkan renderer (NativeXRenderer) at your screen's resolution, with SMAA, ambient occlusion and global
  illumination, DLAA and FSR 3.1, and a field of view setting.
- Online play restored: our own backend stands in for EA's retired servers, with a server browser and quick match.
- Keyboard and mouse, and controllers.

See [CHANGELOG.md](CHANGELOG.md) for what changes between releases.

## Requirements

- Windows 10 or 11, 64 bit.
- A graphics card with Vulkan (NVIDIA GTX 10 series, AMD RX 5000 series, Intel Arc or newer recommended).
- Your own copy of Battlefield 1943 for Xbox 360.
- For a server: [Python 3.12](https://www.python.org/downloads/) as well (the backend is a Python program).

## Hosting a server

See [docs/SERVER.md](docs/SERVER.md). The official servers: [docs/OFFICIAL-SERVERS.md](docs/OFFICIAL-SERVERS.md).

## Credits

- **ReXGlue SDK** (BSD 3 Clause), derived from Xenia: the recompiler and runtime.
- **NativeXRenderer** by Gustavo Garcia (MIT): the Vulkan renderer.
- **GHMetalRecomp** (MIT): its NativeXRenderer profile, which Battlefield's started from.
- **King Kong Recompiled** by TekRantGaming and contributors (MIT): the ambient occlusion, global illumination and
  camera vote, and much of what we learned.
- **DiligentCore** (Apache 2.0), **SPIRV Cross** (Apache 2.0), **volk** (MIT).
- **XeGTAO** by Intel (MIT), **SMAA** (MIT style), **AMD FidelityFX** FSR 3.1, FSR 1 and CAS (MIT), **NVIDIA Image
  Scaling** (MIT), **NVIDIA DLSS** (NVIDIA RTX SDKs licence).

Every release includes the full licence texts.

Battlefield and Battlefield 1943 are trademarks of Electronic Arts Inc. All rights to the game belong to their owners.
