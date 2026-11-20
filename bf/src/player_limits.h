#pragma once
// Server sizes (TekRantGaming, 20 Nov 2026: "We should support the game's default. We should then support 32, 64, and
// 128."; then, with what 128 takes known: "Drop 128 for now"): the sizes a server can be set to, and the longest
// player list our readers of the game's lists accept (a longer one is a bad read, not a server). docs/SERVER-SIZES.md
// has what each size takes and why 128 is not offered.
namespace bf1943 {
constexpr int kServerSizes[] = {24, 32, 64};
constexpr int kDefaultServerSize = 24;  // the game's own
constexpr int kMaxServerSize = 64;      // the game's own Game.MaxPlayerCount: its player ids are 6 bits
constexpr int kPlayerListLimit = 128;   // players a list may hold before it reads as broken
}  // namespace bf1943
