#pragma once
// Night Ops (TekRantGaming, 11 Nov 2026; built 12 Nov: NativeXRenderer D162 and D163, the server flag): a Night Ops
// server's maps drawn at night in players' games. Not in releases until he says it is ready (12 Nov 2026, for 0.3.0:
// "dont include night ops its not ready yet"). CMake's BF_NIGHT_OPS (off by default) builds it in, for the private
// test builds; without it the game lists no server as a night one, never turns the renderer's night on (and turns
// it off if a setting asked for it) and reads no lamps from the levels.
#if defined(BF_NIGHT_OPS)
inline constexpr bool kNightOps = true;
#else
inline constexpr bool kNightOps = false;
#endif
