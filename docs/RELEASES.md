# How a release is made

A release is a git tag (`v<version>`, for example `v0.2.0`) with four files attached on the Releases page, published
as a normal release: GitHub's `releases/latest`, which the game's updater reads, never returns a prerelease or a draft.
The files are built from the development repository with its packaging scripts; this repository holds the public
pages and the releases only.

The updater reads the latest release's tag (the version, compared with the running program's), the file whose name
**ends** `windows-x64.zip` (Windows) or `linux-x86_64.AppImage` (Linux), and `SHA256SUMS.txt`. So those names must
stay as they are, no other file's name may contain them, and the program's own version must be the tag's exactly.

| File | Built by | Holds |
|---|---|---|
| `BF1943-Reborn-<version>-windows-x64.zip` | `tools\make_packages.ps1 -Version <version> -AppImage <file>` | `bf1943.exe` at the top (started by itself: no batch file or script), its libraries, `data`, `fonts`, `glyphs`, the player guide, the licences, and the updater's lists `bf-files.txt` (every file with its size and SHA256) and `bf-retired.txt` (files earlier releases had that this one does not) |
| `BF1943-Reborn-Server-<version>-windows.zip` | the same run | `bin`, `server` (the backend and the hosts), the setup window and its scripts, the server guide, the licences |
| `BF1943-Reborn-<version>-linux-x86_64.AppImage` | `tools/linux/build_linux.sh` and `build_appimage.sh` (WSL), added by `-AppImage` | the Linux game in one file |
| `SHA256SUMS.txt` | the same run | the SHA256 of both zips, the AppImage and the game's three Windows binaries |

## Before a release goes up

1. Built from a clean commit with the release preset (RelWithDebInfo) at the release's version
   (`cmake -DBF_VERSION=<version>`), for Windows and for Linux.
2. `make_packages.ps1` ends with `tools\check_package.ps1`, which refuses the release unless the player zip equals its
   `bf-files.txt`, the file names and `SHA256SUMS.txt` are right for the updaters, the required files are there and
   nothing stale is. No zip holds any game file (the packaging scripts refuse anything that looks like one).
3. A play test on the release build: join, play a round, leave, join again (the owner may skip it for a build).
4. A server test: start, a client joins every map, stop.
5. The updater: an installed copy of the release updates itself to a test build served on the PC (the development
   repository's update test), on Windows and on Linux.
6. Both zips scanned on VirusTotal; the links go in the release notes.
7. The release notes are the changelog's section for that version.
8. The LGPL parts (FFmpeg's decoders and libmspack, built into the runtime): before a public release, the source of
   our modified ReXGlue SDK is published too, as the LGPL asks of a library built into another.
9. Owner sign off (TekRantGaming) before the release is published.
