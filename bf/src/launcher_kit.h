// The launcher's look, Version 1 "Field Report" (TekRantGaming's pick, 8 Nov 2026; the mockups on the canvas
// NuRomvsn2iGAKwH3kQ13eH): an olive backdrop with a map grid, field report paper with tape, stencil headings, typed
// captions, black tape strips for the button prompts and the red EARLY PLAYTEST stamp. And the button glyphs: King
// Kong's per button pictures (Xelu's and Kenney's CC0 prompts, bf/glyphs), drawn from ImGui's font atlas, so a prompt
// costs nothing per frame; the Xbox 360's are drawn as the game draws its own (coloured discs).
//
// Sizes are the mockups' pixels at 720 lines: every function takes `u`, the display's height / 720.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <imgui.h>

namespace bf1943::kit {

// ---- colours (the mockups' CSS)
constexpr ImU32 kPaper = IM_COL32(236, 229, 209, 255);      // #ece5d1
constexpr ImU32 kPaperDim = IM_COL32(220, 211, 184, 255);   // #dcd3b8, a box on the paper
constexpr ImU32 kPaperCard = IM_COL32(228, 220, 196, 255);  // #e4dcc4, a card or a row
constexpr ImU32 kPaperLight = IM_COL32(246, 241, 226, 255); // #f6f1e2, a text field
constexpr ImU32 kRule = IM_COL32(201, 191, 159, 255);       // #c9bf9f
constexpr ImU32 kBorder = IM_COL32(185, 174, 140, 255);     // #b9ae8c
constexpr ImU32 kInk = IM_COL32(34, 36, 27, 255);           // #22241b
constexpr ImU32 kInkSoft = IM_COL32(43, 42, 36, 255);       // #2b2a24
constexpr ImU32 kInkFaint = IM_COL32(90, 84, 64, 255);      // #5a5440
constexpr ImU32 kInkSection = IM_COL32(107, 100, 80, 255);  // #6b6450
constexpr ImU32 kGreen = IM_COL32(93, 138, 50, 255);        // #5d8a32, the selection
constexpr ImU32 kGreenShadow = IM_COL32(52, 82, 26, 255);   // #34521a
constexpr ImU32 kGreenBright = IM_COL32(143, 179, 79, 255); // #8fb34f, progress
constexpr ImU32 kGreenDark = IM_COL32(61, 107, 34, 255);    // #3d6b22, (MODERN)
constexpr ImU32 kBrown = IM_COL32(122, 75, 28, 255);        // #7a4b1c, (CLASSIC)
constexpr ImU32 kStrip = IM_COL32(20, 20, 18, 255);         // #141412, a prompt's tape
constexpr ImU32 kStripText = IM_COL32(242, 239, 230, 255);  // #f2efe6
constexpr ImU32 kCream = IM_COL32(241, 234, 211, 255);      // #f1ead3, headings on the backdrop
constexpr ImU32 kKhaki = IM_COL32(217, 207, 174, 255);      // #d9cfae
constexpr ImU32 kKhakiDim = IM_COL32(169, 154, 104, 255);   // #a99a68
constexpr ImU32 kKhakiText = IM_COL32(207, 198, 168, 255);  // #cfc6a8
constexpr ImU32 kBackText = IM_COL32(233, 226, 200, 255);   // #e9e2c8
constexpr ImU32 kGold = IM_COL32(201, 183, 122, 255);       // #c9b77a
constexpr ImU32 kStamp = IM_COL32(206, 62, 48, 242);        // the stamp's ink
constexpr ImU32 kStampEdge = IM_COL32(196, 52, 40, 217);
constexpr ImU32 kRed = IM_COL32(176, 52, 40, 255);          // an error on the paper

// ---- fonts: the mockups' Big Shoulders Stencil Display, Special Elite and Oswald (all SIL OFL). Each is taken from
// <exe>\fonts when it is there; otherwise the nearest one on the PC: Windows' Stencil, Courier New Bold, and our
// Oswald.
struct Fonts {
  ImFont* stencil = nullptr;  // headings
  ImFont* typed = nullptr;    // captions, typed notes
  ImFont* ui = nullptr;       // Oswald Medium: rows and text
  ImFont* ui_bold = nullptr;  // Oswald Bold: buttons
  float stencil_squeeze = 1;  // a wide stand in (Windows' Stencil) drawn narrower, as the condensed original
};
const Fonts& GetFonts();
void LoadFonts(ImFontAtlas* atlas, const std::filesystem::path& exe_dir, ImFont* ui, ImFont* ui_bold);

// ---- drawing helpers
float TextWidth(ImFont* f, float size, const std::string& t);
// Text with letter spacing `track` (CSS letter-spacing), optionally squeezed for the stencil stand in.
float Spaced(ImDrawList* d, ImFont* f, float size, ImVec2 at, ImU32 col, const std::string& t, float track);
float SpacedWidth(ImFont* f, float size, const std::string& t, float track);
// A stencil heading (squeezed if the font is the stand in), fitted to max_w by shrinking. Returns its width.
float Heading(ImDrawList* d, float size, ImVec2 at, ImU32 col, const std::string& t, float track, float max_w = 0);
// Wrapped text; returns its height.
float Wrapped(ImDrawList* d, ImFont* f, float size, ImVec2 at, ImU32 col, const std::string& t, float wrap);
float WrappedHeight(ImFont* f, float size, const std::string& t, float wrap);

// Everything drawn while one of these lives is turned by `degrees` round `centre` (the mockups' slightly turned
// papers and stamps).
class Turn {
 public:
  Turn(ImDrawList* d, ImVec2 centre, float degrees);
  ~Turn();

 private:
  ImDrawList* d_;
  int start_;
  ImVec2 centre_;
  float degrees_;
};

// The olive backdrop with its map grid. With `over_game` above 0 the game's own picture shows through (the
// mockups' "Backdrop: art from your game files"): it is darkened by that much instead of covered.
void Backdrop(ImDrawList* d, ImVec2 size, float u, float over_game);
// The paper: its shadow, a slightly torn outline, the grain. Draw it inside a Turn to tilt it.
void Paper(ImDrawList* d, ImVec2 a, ImVec2 b, float u, bool torn = true);
void Tape(ImDrawList* d, ImVec2 centre, float w, float h, float degrees);
// "BATTLEFIELD 1943 REBORN" at the top left of every launcher page.
void Brand(ImDrawList* d, ImVec2 at, float u);
// "N of M" with its bar, its right edge at `right_top`.
void StepBar(ImDrawList* d, ImVec2 right_top, float u, int step, int of);
// The EARLY PLAYTEST / ALPHA · SUBJECT TO CHANGE stamp, centred at `centre`. `scale` 1 is the launcher's.
void Stamp(ImDrawList* d, ImVec2 centre, float u, float degrees, float scale = 1, bool backed = false);
bool WatermarkOn();  // bf_playtest_watermark

// ---- button glyphs
enum class Button { kA, kB, kX, kY, kLB, kRB, kLT, kRT, kStart, kBack, kDpad, kDpadUp, kLS, kRS, kKey };
enum class Style { kXbox360, kXboxSeries, kPlayStation3, kPlayStation5, kSteamDeck, kKeyboard };
// The style from the Button prompts setting ("auto" follows the device last used: a controller's Xbox 360 look, or
// the keyboard's keys).
Style StyleFor(const std::string& setting, bool keyboard_last);
const char* StyleName(Style s);
// One button, `h` tall, its left middle at `at`; `key` is the keyboard's key for it (a key name as the glyph files
// have them: Enter, Esc, E, F8...). Returns the width; draws nothing with draw false.
float Glyph(ImDrawList* d, Style style, Button b, const std::string& key, ImVec2 at, float h, bool draw = true);
void SetGlyphDir(const std::filesystem::path& dir);  // <exe>\glyphs
// A PNG file (made from the player's game files: the map pictures) drawn to fill a..b, cropped to its shape, or with
// stretch its whole picture squeezed to a..b (the game's square startup cards, drawn 16:9 as the console showed them).
// False while the file is not there (nothing drawn).
// `fit`: King Kong's DrawCover (a shape far from the box's shown whole over itself cropped and dimmed, else cropped);
// `max_width`: kept no wider than that in the font atlas (0: as it is).
bool DrawImageFile(ImDrawList* d, const std::filesystem::path& file, ImVec2 a, ImVec2 b, bool stretch = false,
                   bool fit = false, int max_width = 0, ImU32 tint = IM_COL32(255, 255, 255, 255));
// The picture file's size in pixels (0 x 0 while it is not there or not decoded yet).
ImVec2 ImageFileSize(const std::filesystem::path& file);
// The picture file's place in the font atlas (the texture coordinates of its whole picture) and its size in pixels,
// for a caller that draws parts of it with ImGui::GetIO().Fonts->TexRef (the pause menu's map and its icons). False
// while the file is not there.
bool ImageFileUv(const std::filesystem::path& file, ImVec2& uv0, ImVec2& uv1, int& w, int& h);

// ---- the game's own menu art and fonts (TekRantGaming, 12 Nov 2026: the player's game files hold the menus' logo, atlases,
// insignia, portraits, brush strokes and fonts; shader_prep.cpp ExtractMenuArt cuts them into user\cache\extracted\menu
// after the game is installed, bf_font.cpp reads the fonts). Never shipped; until they are there, the launcher
// draws with its own fonts and colours.
void SetMenuArtDir(const std::filesystem::path& dir);  // user\cache\extracted\menu (the user folder is three up)
const std::filesystem::path& MenuArtDir();
// A piece of the menu art by its name (logo, backdrop, brush_bar, portrait_us_rifleman, rank_07, glyph_a, flag_us,
// compass...), drawn to fill a..b (cropped to the box's shape, or with stretch squeezed into it), tinted. False
// (nothing drawn) until the art is there.
bool Art(ImDrawList* d, const char* name, ImVec2 a, ImVec2 b, bool stretch = false, ImU32 tint = IM_COL32(255, 255, 255, 255));
// A piece of art `h` tall at its own shape, its left middle at `at`; returns the width (0 when not there).
float ArtAt(ImDrawList* d, const char* name, ImVec2 at, float h, ImU32 tint = IM_COL32(255, 255, 255, 255), bool draw = true);
ImVec2 ArtSize(const char* name);
// The card paper (the game's own, from its logo cards) over a..b: the plain paper colour until it is there.
void PaperArt(ImDrawList* d, ImVec2 a, ImVec2 b);
// A manila folder's tab (0.4.2): slanted sides, rounded shoulders, standing on the line b.y. `front` is the open
// folder's (the paper's colour, no line under it); the others are a darker manila behind it, lighter under the
// pointer. The label's ink goes on either.
void FolderTab(ImDrawList* d, ImVec2 a, ImVec2 b, float u, bool front, bool over);
// The game's fonts: headline_large (Artbrush, headings), headline_medium (rows), normal_large and normal_medium
// (Eurostile Bold Condensed: buttons, labels), list_header (Century Schoolbook Bold Italic: table headers), list_item,
// headnumbers. Text with its capitals `cap` pixels high from `at` (the capitals' top left), `track` pixels added after
// each glyph; until the font is ready, our nearest font (the stencil, Oswald, the typed one). Returns the width.
enum class GameFont { kHeadline, kHeadlineMedium, kText, kTextMedium, kListHeader, kListItem, kNumbers };
float GameText(ImDrawList* d, GameFont f, float cap, ImVec2 at, ImU32 col, const std::string& t, float track = 0);
float GameTextWidth(GameFont f, float cap, const std::string& t, float track = 0);
bool GameFontReady(GameFont f);
// An alpha mask (`size` x `size`, one byte a pixel; King Kong's brand marks, steam_icon.inc, discord_icon.inc) drawn
// into a .. b in `tint`, kept in the font atlas under `key`. False when the atlas has no room for it.
bool DrawMask(ImDrawList* d, const char* key, const unsigned char* mask, int size, ImVec2 a, ImVec2 b, ImU32 tint);

// A prompt on its black tape strip: the glyph, then the label (as the mockups, "A  CHOOSE GAME FILES"). Returns its
// rectangle's width; `hovered` and the return of Clicked tell the mouse.
struct Strip {
  ImVec2 a, b;
  bool hovered = false, clicked = false;
};
Strip PromptStrip(ImDrawList* d, ImVec2 at, float u, Style style, Button button, const std::string& key,
                  const std::string& label, float size = 18, bool green = false, bool draw = true);

}  // namespace bf1943::kit
