#pragma once
// Night Ops (TekRantGaming, 11 Nov 2026; built 12 Nov: NativeXRenderer D162 and D163, the server flag): a Night Ops
// server's maps drawn at night in players' games. Not in releases until he says it is ready (12 Nov 2026, for 0.3.0:
// "dont include night ops its not ready yet"). CMake's BF_NIGHT_OPS (off by default) builds it in, for the private
// test builds; without it the game lists no server as a night one.
// 16 Nov 2026 (TekRantGaming: the night maps gone, the day and night cycle of pull request 32 in their place): a Night
// Ops server's night is its world, the cycle held at midnight (server/gmhost.py's World); the renderer's old night
// look (D162) is never switched on, and the levels' lamps are read in every build (they light the cycle's nights, D175).
#if defined(BF_NIGHT_OPS)
inline constexpr bool kNightOps = true;
#else
inline constexpr bool kNightOps = false;
#endif
