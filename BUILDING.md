# Building Battlefield 1943 Reborn from source

**No game files are in this repository, and none of the game's code.** The code generator translates the game's
program on your own PC, from your own copy. You need Battlefield 1943 for Xbox 360 (Xbox Live Arcade, title ID
`5841097E`): its package file from your console's storage, the single file in `5841097E\000D0000`. Please do not
share game files, and never commit anything made from them (`bf/assets`, `bf/generated`, extracted pictures or sounds).

## What is where

| Folder | What it holds |
|---|---|
| `bf/src` | The port: our menus and launcher, online services, the server mode, fixes and hooks into the recompiled code |
| `bf/native-renderer` | `rexgpu-bfnative`, the Vulkan GPU plugin, built inside the SDK |
| `bf/third_party/native-x-renderer` | NativeXRenderer (MIT, Gustavo Garcia) with this project's changes; DiligentCore is a submodule |
| `bf/data`, `bf/fonts`, `bf/glyphs` | Our shader list (where each shader is in your game files, and pipeline descriptions; no shader code), the Oswald font, the CC0 button prompts |
| `bf/*.toml` | The code generator's manifest, and the functions it cannot find by itself |
| `sdk/src-bf` | Our copy of the ReXGlue SDK (BSD 3 Clause, derived from Xenia) with its third party libraries, among them the FFmpeg and libmspack sources (LGPL 2.1) |
| `server` | The backend (`plasma.py`: login, server list, stats) and the dedicated server's host (`gmhost.py`), Python 3.12 |
| `tools` | Packaging, the server package's scripts, the Linux build, and the helpers used to study the game |
| `docs` | The players' guides and the technical notes |

Third party code and its licences: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Windows

You need:

- Visual Studio 2022 Build Tools with **Desktop development with C++**, **C++ Clang Compiler for Windows** and
  **C++ CMake tools for Windows** (CMake and Ninja). The build scripts load Visual Studio's environment and put its
  clang first on the path; another clang on the path breaks the build.
- Git and Python 3.12.
- Your own Battlefield 1943 package.

From a command prompt:

1. Get the code with its submodules (DiligentCore and its own). Some of the SDK's third party files have long names
   (167 characters inside the repository), so either clone into a short folder such as `C:\src`, or let git use long
   paths first, as below:

   ```bat
   git config --global core.longpaths true
   git clone --recursive https://github.com/TekRantGaming/Battlefield-1943-Reborn.git
   cd Battlefield-1943-Reborn
   ```

2. Build the SDK. It installs into `sdk\src-bf\out\install\win-amd64` (the code generator `rexglue.exe`, the runtime
   and the GPU plugin):

   ```bat
   sdk\src-bf\build_bf.bat
   ```

3. Unpack your package and give the code generator the game's program. Both folders are ignored by git:

   ```bat
   python -I tools\stfs_extract.py "<your package file>" input\pkg
   mklink /J bf\assets input\pkg
   ```

4. Translate the game's code. This writes `bf\generated` (about 170 MB, ignored by git):

   ```bat
   cd bf
   ..\sdk\src-bf\out\install\win-amd64\bin\rexglue.exe codegen bf1943_manifest.toml
   ```

5. Build the game:

   ```bat
   build.bat bf-relwithdebinfo
   ```

   The first configure applies NativeXRenderer's patches (`bf/third_party/native-x-renderer/diligent_patches`) to the
   DiligentCore submodule, which must be at its pinned commit. The game is in `bf\out\build\bf-relwithdebinfo`:
   `bf1943.exe`, its libraries, `data`, `fonts` and `glyphs`. `build.bat bf-dev` adds the developer aids that
   release builds leave out.

6. Start `bf1943.exe`. The first start asks for your package and installs the game into a `game` folder beside the
   program. A build from source looks for the main backend at `127.0.0.1`: run your own with
   `python -I server\plasma.py` ([docs/SERVER.md](docs/SERVER.md)), or copy `bf1943.toml` from a release download
   next to `bf1943.exe` to play on the official servers.

Release packages: `powershell -File tools\make_packages.ps1 -Version <version> -OnlineHost <address>` (add
`-AppImage <file>` for the Linux file). They go to `runs\release\<version>`; see [docs/RELEASES.md](docs/RELEASES.md).

## Linux (WSL)

The Linux build runs in WSL from the Windows checkout, after Windows steps 1 to 5: they unpack the game, generate
the code and patch DiligentCore, and the Linux build uses all three.

You need WSL with Ubuntu 24.04 (`wsl -d Ubuntu-24.04`), clang 20, CMake, Ninja, rsync, git, g++ and the ReXGlue SDK's
Linux prerequisites (its [wiki](https://github.com/rexglue/rexglue-sdk/wiki)).

```bash
export BF_LINUX_MAIN=/mnt/f/BF-1942-MP-Decomp   # your checkout as WSL sees it (this one is the default)
bash tools/linux/build_linux.sh all Release
```

The script copies the sources to `~/bf-linux` (much faster than building on `/mnt`), builds the SDK, then the game.
Logs: `~/bf-linux/*.log`.

The AppImage:

```bash
export BF_ONLINE_HOST=<main backend address>
export BF_LICENCES=<the licences folder of a Windows package>
export BF_APPIMAGETOOL=<appimagetool-x86_64.AppImage>
bash tools/linux/build_appimage.sh <version> Release
```

It writes `~/bf-linux/dist/BF1943-Reborn-linux-x86_64.AppImage`. The name has no version, so Steam shortcuts keep
working after an update.
