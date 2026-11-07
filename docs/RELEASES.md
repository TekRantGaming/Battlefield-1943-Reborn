# How a release is made

A release is a git tag (`v<major>.<minor>.<patch>`, for example `v0.1.0`) with two files attached on the Releases page.
The files are built from the development repository with its packaging scripts; this repository holds the public
pages and the releases only.

| File | Built by | Holds |
|---|---|---|
| `BF1943-Reborn-<version>-windows.zip` | `tools\make_packages.ps1 -Version <version>` | `bf1943.exe` at the top (started by itself: no batch file or script), its libraries, `data`, `fonts`, the player guide, the licences |
| `BF1943-Reborn-Server-<version>-windows.zip` | the same run | `bin`, `server` (the backend and the hosts), `Start servers.bat`, `Stop servers.bat`, the server guide, the licences |

## Before a release goes up

1. Built from a clean commit with the release preset (RelWithDebInfo), the map file kept for crash reports.
2. Neither zip holds any game file (the packaging scripts refuse anything that looks like one).
3. A play test on the release build: join, play a round, leave, join again.
4. A server test: start, a client joins every map, stop.
5. Both zips scanned on VirusTotal; the links go in the release notes.
6. The release notes are the changelog's section for that version.
7. The LGPL parts (FFmpeg's decoders and libmspack, built into the runtime): before a public release, the source of
   our modified ReXGlue SDK is published too, as the LGPL asks of a library built into another.
8. Owner sign off (TekRantGaming) before the release is published.
