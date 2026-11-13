// The level's capture points and their letters (capture_points.cpp): the flags' letters in the HUD and the map in the
// pause menu.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bf1943 {

struct CapturePoint {
  uint32_t entity = 0;  // the game's object (guest address)
  int index = 0;        // the level file's Index
  int order = 0;        // its place in the level's own list of points (0 first), before the sort by letter
  std::string name;     // the level file's ControlPointName (CQ_MP_BASE_WAKE_AIRFIELD)
  float x = 0, y = 0, z = 0;
  bool capturable = false;  // a team can take it, and the map shows it
  int letter = -1;          // 0 for A; -1: none (a team's own base)
  int owner = 0;            // the team that holds it: 0 none, 1, 2
  // How the game's own maps show it to this player now (the flag its minimap draws for the point): 0 white (no
  // owner), 1 blue (the player's team), 2 red (the other team); -1 while the game draws no map. And whether that
  // flag blinks (the point is being taken).
  int shown = -1;
  bool contested = false;
};

// The level's points as last read from the game (the lettered ones first, in letter order); none outside a match.
std::vector<CapturePoint> CapturePoints();

// From the pad hook (auto_join.cpp), at the game's pad reads: the points are read again (a few times a second) while
// a level is loaded, and forgotten without one.
void CapturePointsTick(const uint8_t* base, bool level_loaded);

// For the game's icon atlas (512 x 256, R, G, B, A rows): draws the lettered copies of the flags into its empty
// right hand side; how many it drew (0: this is not that atlas as we know it). hud_glyphs.cpp's patcher does it to
// the atlas the game draws from, and then says so (FlagLettersReady: from then on the game's flags are answered with
// their lettered copies); shader_prep.cpp does it to the copy the pause menu's map draws its icons from.
int DrawFlagLetters(std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, int scale = 1);
void FlagLettersReady();

// The lettered copy of a flag in that atlas, in texels: `kind` 0 white (no owner), 1 blue (friendly), 2 red (enemy).
struct FlagRect {
  int x, y, w, h;
};
FlagRect LetteredFlag(int kind, int letter);
// A point's plain flag on the maps (no letter, as the game draws it with the letters off): 20 x 14 texels.
FlagRect PlainFlag(int kind);

// A capture point's name for players, from the level file's (CQ_MP_BASE_WAKE_SOUTH_BASE: "South Base").
std::string CapturePointTitle(const std::string& name);

// An icon the game's own minimap drew a moment ago (it draws every one the player may see, wherever it is: the
// vehicles, the team, the bases): the pause menu's map shows the same ones.
struct MapIcon {
  float x = 0, z = 0;                    // its place in the world
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;  // its picture in the icon atlas (0 to 1)
  bool flag = false;                     // a capture point's flag (the map draws those itself, from CapturePoints)
  uint32_t color = 0;                    // the tint the minimap draws it with (its entry's word at +8, as read)
};
std::vector<MapIcon> MapIcons();  // those of the last third of a second, in the game's drawing order

// A player on the pause menu's map (map_players.cpp): every team mate, and an enemy while the game shows him (its
// minimap draws him). Read from the game's own player list, so also when no minimap is drawn (a bomber view).
struct MapPlayer {
  uint32_t id = 0;  // the game's player object
  float x = 0, y = 0, z = 0;
  float fx = 0, fz = 0;  // the way he faces on the ground (0, 0: not known)
  bool enemy = false;    // the other team's (shown only while seen)
  bool squad = false;    // in the player's own squad
  bool spotted = false;  // an enemy the game's minimap shows (or showed a moment ago)
};
std::vector<MapPlayer> MapPlayers();  // as last read (a few times a second); none outside a match
// From the pad hook (auto_join.cpp), at the game's pad reads, ten times a second at most.
void MapPlayersTick(const uint8_t* base, bool level_loaded);

}  // namespace bf1943
