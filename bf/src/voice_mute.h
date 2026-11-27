// Per player voice mute in the scoreboards (TekRantGaming, 17 Nov 2026: "allows users to mute individual users so
// that there coms dont come across on voice chat anymore. This needs to be intergrated into the scoreboard in game and
// on the endgame score screen too and should have a speaker or mic icon and an icon for being muted too to visualy show
// the status of that players mute status"). The mute itself is comms.cpp's (comms.h: Muted, SetMuted); this is where
// players see and change it:
//   - the game's own scoreboards (the one held in a match, the deploy screen's, the end of round screen's): a speaker
//     in each row's V column (the column the console showed voice in), lit while that player is heard, struck through
//     once muted. The game's own row cursor picks the player and its own Y prompt ("SEE GAMER CARD" on the console,
//     a gamer card the PC never had) mutes or unmutes: the game sends showGamerCard for the selected row, and we take
//     it (auto_join.cpp's command hook). Its words are "MUTE / UNMUTE" here. A click on a speaker works too where the
//     game shows a pointer (the deploy screen).
//   - our pause's Scoreboard (game_menu_launcher.inc): the same speaker in each row, a row cursor and X (keyboard F) or
//     a click on the speaker.
// Rows are found from the game's own text as it draws it (clan_tag_colours.cpp's text hook hands each run here): its
// "V" and "Gamertag" headings and each player's name, with where they land on the screen.
#pragma once

#include <cstdint>
#include <string>

struct ImDrawList;
struct ImGuiIO;
struct ImVec2;

namespace bf1943::mute {

enum class Voice { kQuiet, kTalking, kMuted };
Voice VoiceOf(const std::string& name);  // the scoreboards' state for a player ("[TAG]Name" or "Name")

// The speaker icon (our own drawing, voice_mute_icon.cpp): `h` tall, centred on `c`, in `ink` (dimmer while quiet);
// talking adds its two waves in `accent`, moving; muted strikes it through in `strike`. `seconds` drives the waves.
// `halo` (0 none): a thin outline of that colour around every shape, for a background that may be dark or light.
void DrawIcon(ImDrawList* d, const ImVec2& c, float h, Voice v, unsigned ink, unsigned accent, unsigned strike,
              double seconds, float alpha = 1.0f, unsigned halo = 0);

// clan_tag_colours.cpp's text hook, the game's thread: one run of the game's text about to be drawn (its renderer and
// its line list).
void NoteGameText(const uint8_t* base, uint32_t renderer, uint32_t lines);
// The same run's first glyph (where its pen starts, in the run's own units).
void NoteGameGlyph(int32_t pen_x, int32_t pen_y);
// The ping column's room (voice_mute.cpp): called for every glyph of the run, before it is added, with its pen (the
// word at `pen_at` is its x, which this may move). False: the glyph is not drawn (a row's dot leaders past the cut).
bool LayoutGlyph(uint8_t* base, uint32_t pen_at, int32_t pen_y);
// Ends the run started by NoteGameText.
void EndGameText();
// A run of the game's text this hook may rewrite in place before it is drawn (the "SEE GAMER CARD" prompt's words):
// true when it did; RestoreGameText puts the game's own back after the draw.
bool RewriteGameText(uint8_t* base, uint32_t lines);
void RestoreGameText(uint8_t* base);

// comms_ui.cpp, each frame over the match (our menus closed): the icons over the game's scoreboards, and a click on one.
void DrawOverGame(ImGuiIO& io);
// auto_join.cpp's command hook: the game's "showGamerCard <arg>" (its scoreboards' Y). True when taken (a mute toggled).
bool TakeGamerCard(const char* arg);

}  // namespace bf1943::mute
