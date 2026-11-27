// Players' pings on the scoreboards (TekRantGaming, 20 Nov 2026: "I'd like to see your active ping reported in the
// scoreboard at the end. But like how Call of Duty Black Ops 1 would show everyone's connection and ping, and World at
// War would even show you the number as well"). Each player's connection as four rising bars, more lit for a better
// one (the server browser's steps and colours), with the number in ms beside them. A bot has no connection: its cell
// says BOT, never a number (world_sync.h IsBot; bots wear no tag since 20 Nov 2026). A player whose ping is not known
// (an older game, an older server) gets no cell.
//
// The pings come from world_sync.h (OwnPing, PingOf). Drawn on the game's own scoreboards (in a match, the deploy
// screen's, the end of round screen's: voice_mute.cpp finds their rows) and on our pause's Scoreboard.
#pragma once

#include <string>

struct ImDrawList;
struct ImFont;

namespace bf1943::ping {

// Over this, the join warns that the server is far away (game_menu.cpp PingWarned).
constexpr int kFarMs = 90;

int Bars(int ms);         // 4 under 60 ms, 3 under 120, 2 under 200, else 1; 0 unknown
unsigned Colour(int ms);  // green, amber, red (the server browser's)

// The four bars, `h` tall, from x `left` with their feet on y `base`; unlit ones in `off`. Returns their width.
float DrawBars(ImDrawList* d, float left, float base, float h, int ms, unsigned off);

// A player's cell, `h` tall, its right edge at `right`, its middle at `mid`: the bars and the number ("45"), or BOT.
// `tag`: on a small dark tag of its own (over the game's scoreboards, whose rows are paper, green or black); else in
// `ink` on the row as it is. Returns the width drawn (0: nothing, a ping not known).
float DrawCell(ImDrawList* d, ImFont* font, float right, float mid, float h, int ms, bool bot, bool tag, unsigned ink);
// The cell's width at `h` (the same for every row, so the column lines up).
float CellWidth(float h);

}  // namespace bf1943::ping
