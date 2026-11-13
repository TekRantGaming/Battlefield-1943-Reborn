#pragma once
// The picture in picture sniper scope (TekRantGaming, 13 Nov 2026: "add a modern sniper scope implementation", Settings
// > Enhancements, experimental, off by default; NativeXRenderer D168, fov.cpp). Taken out of 0.4.1 at his word (13 Nov
// 2026, 20:20: "drop the sniper change from this update"). CMake's BF_SCOPE_PIP (off by default) builds it in, for the
// private test builds and development; without it Enhancements has no Sniper scope row and the game never reports its
// view to the renderer, whatever bf_scope_pip says, so the renderer's scope pass never runs.
#if defined(BF_SCOPE_PIP)
inline constexpr bool kScopePip = true;
#else
inline constexpr bool kScopePip = false;
#endif
