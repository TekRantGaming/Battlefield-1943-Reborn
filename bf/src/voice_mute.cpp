// The scoreboards' voice mute (voice_mute.h): the speaker icons over the game's own scoreboards, and the game's Y
// ("showGamerCard") turned into mute and unmute.
//
// Where the game's rows are: its text, as it is drawn (clan_tag_colours.cpp's hook on sub_826AB588 hands each run
// here). A run's place is its first glyph's pen (sub_826AB240: r7 points at the pen's x, r8 is its y, both in the
// movie's 1280 x 720 stage units, the y at the top of the line) through the renderer's matrix for that run (renderer +
// 128, sixteen floats set by sub_826ABAC0 from the text command): clip x = x m0 + y m4 + m12, clip y = x m1 + y m5 +
// m13 (runs probe1, 17 Nov 2026: the scoreboard's "V" heading at clip -0.748, 0.302, its first row's name at -0.644,
// 0.236, matching the screenshot). Each frame's runs are kept for a moment; the overlay (comms_ui.cpp, our UI thread)
// reads the last ones. A table is a "V" heading with a "Gamertag" heading on its line to its right; its rows are the
// runs below that start where "Gamertag" starts, 20 stage units apart. Each row's name is its text up to the dots
// that lead to the kills ("Soldier6400 . . . . ."), the clan tag included as the game shows it ("[IWO]Kestrel").
//
// The game's Y on its scoreboards calls showGamerCard with the selected row's name (probe1: "showGamerCard
// (Soldier1696)" after down, down, right, Y), on the console the player's gamer card; here it mutes or unmutes that
// player (auto_join.cpp's command hook calls TakeGamerCard, and the command goes no further). Its prompt says
// "SEE GAMER CARD"; the words are rewritten to "MUTE / UNMUTE" in the run as it is drawn (the game's own buffer, put
// back right after), so the prompt keeps the game's own look and the player's prompt style.

#include "voice_mute.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "comms.h"
#include "launcher_kit.h"
#include "scoreboard_ping.h"
#include "world_sync.h"

REXCVAR_DECLARE(bool, server);
REXCVAR_DEFINE_BOOL(bf_scoreboard_mute, true, "BF1943/Voice",
                    "The game's scoreboards show each player's voice (a speaker, struck through once muted), and their Y "
                    "mutes or unmutes the player picked");
REXCVAR_DEFINE_BOOL(bf_scoreboard_ping, true, "BF1943/Voice",
                    "The game's scoreboards and the pause's Scoreboard show each player's ping (bars and ms; BOT for a bot)");

namespace bf1943 {
bool InMatch();  // auto_join.cpp
}

namespace bf1943::mute {
namespace {

using Clock = std::chrono::steady_clock;
double Seconds() {
  static const auto start = Clock::now();
  return std::chrono::duration<double>(Clock::now() - start).count();
}

uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint16_t Load16(const uint8_t* base, uint32_t a) { return uint16_t(base[a] << 8 | base[a + 1]); }
void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  for (int i = 0; i < 4; ++i) base[a + i] = uint8_t(v >> (24 - 8 * i));
}
void Store16(uint8_t* base, uint32_t a, uint16_t v) { base[a] = uint8_t(v >> 8), base[a + 1] = uint8_t(v); }
float LoadF(const uint8_t* base, uint32_t a) {
  const uint32_t u = Load32(base, a);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
bool Plausible(uint32_t a) { return a >= 0x00100000u && a < 0xFFF00000u; }

// One run of the game's text as drawn: where its line starts (clip space) and how big a stage unit is there.
struct GameRun {
  std::string text;
  float x = 0, y = 0;  // the first glyph's pen, clip space
  float ux = 0, uy = 0;  // one stage unit across and down, clip space (the run's scale; uy is negative: down)
  double at = 0;
};

// The run being drawn now (the game's thread).
struct Pending {
  bool active = false, pen = false;
  GameRun run;
  float m[16] = {};
};
thread_local Pending g_pending;

std::mutex g_mutex;
std::vector<GameRun> g_runs;  // the last quarter second's runs that could be a heading or a row

// The ping column's room (TekRantGaming, 20 Nov 2026: "push the ping information to the furthest right of the
// scoreboard"). The game's tables have no space right of their Scr. column, so while the ping column shows, the game's
// own Kls. and Scr. columns (headings and numbers) are drawn kShift stage units further left and each row's dot
// leaders end before the kills (kCut): the ping cell then takes the Scr. column's old place at the right edge. Done
// glyph by glyph as the game draws (LayoutGlyph, the game's thread) from the tables the overlay found in the frames
// just before (DrawOverGame publishes them, clip space).
constexpr float kShift = 65.0f;        // stage units the Kls. and Scr. columns move left
constexpr float kColumnsFrom = 240.0f;  // a run starting this far right of the name heading is Kls. or Scr. (to kColumnsTo)
constexpr float kColumnsTo = 410.0f;  // the other table's # column starts at about 421
constexpr float kCut = 236.0f;          // a row's dot leaders end here (the kills, moved, start after it)
constexpr float kPingRight = 396.0f;    // the ping cell's right edge (the Scr. numbers' old right edge)
struct LayoutTable {
  float name_x = 0, name_y = 0;  // the name heading's pen, clip space
  float ux = 0, uy = 0;          // a stage unit in clip space (uy negative: down)
  float bottom = 0;              // stage units below the name heading that its rows reach
};
std::mutex g_layout_mutex;
std::vector<LayoutTable> g_layout;
double g_layout_at = -100;
// The run being drawn (the game's thread): its matrix, the tables, and what is done to it.
struct LayoutRun {
  bool active = false, first = true, dotted = false;
  int mode = 0;  // 1 moved left (Kls., Scr.), 2 a row's name with its dot leaders cut
  float m[16] = {};
  float cut_x = 0, ux = 1;
  std::vector<LayoutTable> tables;
};
thread_local LayoutRun g_lrun;

// The rewritten prompt (RewriteGameText), put back after its draw.
struct Rewrite {
  bool active = false;
  uint32_t line = 0, end = 0;
  std::vector<uint16_t> chars;
};
thread_local Rewrite g_rewrite;

// A run's characters as one line of plain text (13, 10 and 189 skipped as the renderer does), at most `max`.
bool RunText(const uint8_t* base, uint32_t head, std::string& out, size_t max) {
  out.clear();
  int n = 0;
  for (uint32_t line = Load32(base, head); line != head && Plausible(line) && n < 4; ++n, line = Load32(base, line)) {
    const uint32_t b = Load32(base, line + 8), e = Load32(base, line + 12);
    if (!Plausible(b) || e < b || e - b > 2 * max + 64) return false;
    for (uint32_t p = b; p + 2 <= e; p += 2) {
      const uint16_t c = Load16(base, p);
      if (c == 13 || c == 10 || c == 189) continue;
      if (out.size() >= max) return false;
      // A soldier name's trademark sign (player_name.h) as UTF-8: the game's text holds it as one character, or as
      // its three bytes one by one; either way the row's name reads as the player list's.
      if (c == 0x2122) out += "\xE2\x84\xA2";
      else if (c >= 0x80 && c < 0x100) out += char(c);
      else out += c >= 32 && c < 127 ? char(c) : '?';
    }
  }
  return !out.empty();
}

// A row's player name: its text up to the dot leaders (". . . ." to the kills), trimmed.
std::string RowName(const std::string& text) {
  size_t end = text.size();
  for (size_t i = 0; i + 1 < text.size(); ++i)
    if ((text[i] == ' ' && text[i + 1] == '.') || (text[i] == '.' && text[i + 1] == ' ') ||
        (text[i] == '.' && text[i + 1] == '.')) {
      end = i;
      break;
    }
  std::string s = text.substr(0, end);
  while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
  while (!s.empty() && s.front() == ' ') s.erase(s.begin());
  return s;
}

bool NameHeading(const std::string& t) { return t == "Gamertag" || t == "GAMERTAG" || t == "GAMER TAG" || t == "Gamer Tag"; }

}  // namespace

Voice VoiceOf(const std::string& name) {
  if (comms::Muted(name)) return Voice::kMuted;
  return comms::Talking(name) ? Voice::kTalking : Voice::kQuiet;
}

void NoteGameText(const uint8_t* base, uint32_t renderer, uint32_t lines) {
  g_pending.active = false;
  g_lrun.active = false;
  if (!REXCVAR_GET(server) && REXCVAR_GET(bf_scoreboard_ping) && Plausible(renderer) && Plausible(lines)) {
    std::lock_guard lock(g_layout_mutex);
    if (!g_layout.empty() && Seconds() - g_layout_at < 0.25) {
      g_lrun.active = true, g_lrun.first = true, g_lrun.mode = 0;
      g_lrun.tables = g_layout;
      for (int i = 0; i < 16; ++i) g_lrun.m[i] = LoadF(base, renderer + 128 + 4 * i);
    }
  }
  if (g_lrun.active) {
    std::string t;
    g_lrun.dotted = RunText(base, lines, t, 64) && t.find(" . .") != std::string::npos;
  }
  if (REXCVAR_GET(server) || !(REXCVAR_GET(bf_scoreboard_mute) || REXCVAR_GET(bf_scoreboard_ping)) || !Plausible(renderer) ||
      !Plausible(lines))
    return;
  std::string text;
  if (!RunText(base, lines, text, 64)) return;
  // Only what can be a heading or a row: a "V", a name heading, or a name with its dot leaders.
  if (!(text == "V" || NameHeading(text) || text.find(" . .") != std::string::npos)) return;
  g_pending.active = true, g_pending.pen = false;
  g_pending.run.text = std::move(text);
  for (int i = 0; i < 16; ++i) g_pending.m[i] = LoadF(base, renderer + 128 + 4 * i);
}

void NoteGameGlyph(int32_t pen_x, int32_t pen_y) {
  if (!g_pending.active || g_pending.pen) return;
  g_pending.pen = true;
  const float* m = g_pending.m;
  const float x = float(pen_x), y = float(pen_y);
  g_pending.run.x = x * m[0] + y * m[4] + m[12];
  g_pending.run.y = x * m[1] + y * m[5] + m[13];
  g_pending.run.ux = m[0], g_pending.run.uy = m[5];
}

bool LayoutGlyph(uint8_t* base, uint32_t pen_at, int32_t pen_y) {
  if (!g_lrun.active) return true;
  const float* m = g_lrun.m;
  const int32_t px = int32_t(Load32(base, pen_at));
  const float cx = float(px) * m[0] + float(pen_y) * m[4] + m[12];
  if (g_lrun.first) {
    g_lrun.first = false;
    const float cy = float(px) * m[1] + float(pen_y) * m[5] + m[13];
    for (const LayoutTable& t : g_lrun.tables) {
      if (t.ux == 0 || t.uy == 0 || m[0] == 0) continue;
      const float rx = (cx - t.name_x) / t.ux, ry = (cy - t.name_y) / t.uy;
      if (ry < -6 || ry > t.bottom) continue;
      if (!g_lrun.dotted && rx > kColumnsFrom && rx < kColumnsTo) {
        // Kls. or Scr. (a heading or a number): the run's pen moved left; its later glyphs follow from it.
        Store32(base, pen_at, uint32_t(px + int32_t(std::lround(-kShift * t.ux / m[0]))));
        g_lrun.mode = 1;
        return true;
      }
      if (g_lrun.dotted && std::fabs(rx) < 4) {
        g_lrun.mode = 2, g_lrun.cut_x = t.name_x + kCut * t.ux, g_lrun.ux = t.ux;
        break;
      }
    }
  }
  // A row's dot leaders past the cut: not drawn.
  if (g_lrun.mode == 2 && (cx - g_lrun.cut_x) * g_lrun.ux > 0) return false;
  return true;
}

void EndGameText() {
  g_lrun.active = false;
  if (!g_pending.active) return;
  g_pending.active = false;
  if (!g_pending.pen || !std::isfinite(g_pending.run.x) || !std::isfinite(g_pending.run.y)) return;
  g_pending.run.at = Seconds();
  std::lock_guard lock(g_mutex);
  const double old = g_pending.run.at - 0.25;
  g_runs.erase(std::remove_if(g_runs.begin(), g_runs.end(), [old](const GameRun& r) { return r.at < old; }), g_runs.end());
  if (g_runs.size() < 600) g_runs.push_back(g_pending.run);
}

bool RewriteGameText(uint8_t* base, uint32_t head) {
  g_rewrite.active = false;
  if (REXCVAR_GET(server) || !REXCVAR_GET(bf_scoreboard_mute) || !Plausible(head)) return false;
  const uint32_t line = Load32(base, head);
  if (!Plausible(line) || line == head || Load32(base, line) != head) return false;  // one line only
  const uint32_t b = Load32(base, line + 8), e = Load32(base, line + 12);
  static const char kWords[] = "SEE GAMER CARD";
  constexpr uint32_t kLen = sizeof(kWords) - 1;
  if (!Plausible(b) || e != b + 2 * kLen) return false;
  for (uint32_t i = 0; i < kLen; ++i)
    if (Load16(base, b + 2 * i) != uint16_t(kWords[i])) return false;
  // The same number of letters, so the game's strip behind them fits as it did.
  static const char kMute[] = "MUTE / UNMUTE";
  constexpr uint32_t kNew = sizeof(kMute) - 1;
  static_assert(kNew <= kLen);
  g_rewrite.active = true, g_rewrite.line = line, g_rewrite.end = e;
  g_rewrite.chars.resize(kLen);
  for (uint32_t i = 0; i < kLen; ++i) g_rewrite.chars[i] = Load16(base, b + 2 * i);
  for (uint32_t i = 0; i < kNew; ++i) Store16(base, b + 2 * i, uint16_t(kMute[i]));
  Store32(base, line + 12, b + 2 * kNew);
  return true;
}

void RestoreGameText(uint8_t* base) {
  if (!g_rewrite.active) return;
  g_rewrite.active = false;
  const uint32_t b = Load32(base, g_rewrite.line + 8);
  for (size_t i = 0; i < g_rewrite.chars.size(); ++i) Store16(base, b + 2 * uint32_t(i), g_rewrite.chars[i]);
  Store32(base, g_rewrite.line + 12, g_rewrite.end);
}

void DrawOverGame(ImGuiIO& io) {
  const bool show_mute = REXCVAR_GET(bf_scoreboard_mute), show_ping = REXCVAR_GET(bf_scoreboard_ping);
  if (!(show_mute || show_ping) || !InMatch()) return;
  // Only the game's latest frame of the scoreboard: the runs drawn around its last "V" heading, and only while the
  // game is still drawing it (its last heading no older than about one and a half of its frames). With a fixed 0.12 s
  // the speakers stayed a few frames over the match after the scoreboard closed (Ally run lnx1, shot 58).
  static double last_heading = -1, cadence = 1.0 / 30.0;  // our UI thread only
  static bool shown = false;
  const double now_s = Seconds();
  std::vector<GameRun> runs;
  double latest = -1;
  {
    std::lock_guard lock(g_mutex);
    for (const GameRun& r : g_runs)
      if (r.text == "V") latest = std::max(latest, r.at);
    for (const GameRun& r : g_runs)
      if (latest >= 0 && std::fabs(r.at - latest) < 0.015) runs.push_back(r);
  }
  if (latest > last_heading + 0.004) {
    if (last_heading > 0 && latest - last_heading < 0.25) cadence = 0.8 * cadence + 0.2 * (latest - last_heading);
    last_heading = latest;
  }
  const double age = latest < 0 ? 1e9 : now_s - latest;
  const bool live = age < std::min(0.1, 1.6 * cadence + 0.006);
  if (live != shown) {
    shown = live;
    if (live) REXLOG_INFO("BF scoreboard: speakers shown (the game draws a scoreboard, a frame every {:.0f} ms)", cadence * 1000);
    else REXLOG_INFO("BF scoreboard: speakers hidden, {:.0f} ms after the game's last scoreboard frame", std::min(age, 9.999) * 1000);
  }
  if (!live || runs.empty()) return;
  const ImVec2 size = io.DisplaySize;
  auto screen = [&](float cx, float cy) { return ImVec2((cx + 1.0f) * 0.5f * size.x, (1.0f - cy) * 0.5f * size.y); };
  // The tables: a "V" heading with a name heading on its line, to its right.
  struct Table {
    ImVec2 v, name;  // the headings' pens on the screen
    float unit;      // a stage unit in screen pixels (down)
    LayoutTable clip;  // the name heading in clip space (LayoutGlyph)
  };
  std::vector<Table> tables;
  for (const GameRun& v : runs) {
    if (v.text != "V") continue;
    const ImVec2 vp = screen(v.x, v.y);
    const float unit = std::fabs(v.uy) * 0.5f * size.y;
    if (unit <= 0) continue;
    for (const GameRun& g : runs) {
      if (!NameHeading(g.text)) continue;
      const ImVec2 gp = screen(g.x, g.y);
      if (std::fabs(gp.y - vp.y) < 4 * unit && gp.x > vp.x && gp.x - vp.x < 140 * unit) {
        bool dup = false;
        for (const Table& t : tables) dup = dup || (std::fabs(t.v.x - vp.x) < unit && std::fabs(t.v.y - vp.y) < unit);
        LayoutTable lt;
        lt.name_x = g.x, lt.name_y = g.y, lt.ux = v.ux, lt.uy = v.uy;
        if (!dup) tables.push_back({vp, gp, unit, lt});
      }
    }
  }
  if (tables.empty()) return;
  ImDrawList* d = ImGui::GetBackgroundDrawList();  // under our own overlays (chat, the broadcast box)
  const double now = Seconds();
  // Each row: a name run under a table's headings, starting where its name heading starts.
  struct Row {
    std::string name;
    ImVec2 pen;
    size_t table;
  };
  std::vector<Row> rows;
  for (const GameRun& r : runs) {
    if (r.text.find(" . .") == std::string::npos) continue;
    const ImVec2 p = screen(r.x, r.y);
    for (size_t t = 0; t < tables.size(); ++t) {
      const float u = tables[t].unit;
      if (std::fabs(p.x - tables[t].name.x) > 4 * u || p.y <= tables[t].name.y + 8 * u || p.y > tables[t].name.y + 520 * u)
        continue;
      const std::string name = RowName(r.text);
      if (name.empty()) continue;
      bool dup = false;
      for (const Row& o : rows) dup = dup || (o.table == t && std::fabs(o.pen.y - p.y) < 2 * u);
      if (!dup) rows.push_back({name, p, t});
    }
  }
  // The ping column (scoreboard_ping.h): a cell in each row on the name's dot leaders, right before the kills (the
  // name heading's pen to the kills' last digit is about 335 stage units, run probe1's screenshots; the longest name
  // with its clan tag, about 20 letters, ends near 185), and "Ping" over it on the headings' line.
  if (show_ping) {
    const kit::Fonts& f = kit::GetFonts();
    std::vector<LayoutTable> layout;
    for (size_t ti = 0; ti < tables.size(); ++ti) {
      const Table& t = tables[ti];
      const float u = t.unit, h = 17.0f * u, right = t.name.x + kPingRight * u;
      bool any = false;
      float bottom = 0;
      for (const Row& row : rows) {
        if (row.table != ti) continue;
        bottom = std::max(bottom, (row.pen.y - t.name.y) / u + 22.0f);
        const bool bot = ping::IsBot(row.name);
        const int ms = bot ? -1 : world::PingOf(row.name);
        any = ping::DrawCell(d, f.ui_bold, right, row.pen.y + 10.0f * u, h, ms, bot, true, 0) > 0 || any;
      }
      if (!any) continue;  // nobody's ping known here (an older server): the game's columns as they are, no heading
      LayoutTable lt = t.clip;
      lt.bottom = bottom;
      layout.push_back(lt);
      const char* word = "Ping";
      const float cap = 11.0f * u, cx = right - ping::CellWidth(h) * 0.5f;
      const ImU32 ink = IM_COL32(30, 28, 22, 255);
      if (kit::GameFontReady(kit::GameFont::kListHeader)) {
        const float w = kit::GameTextWidth(kit::GameFont::kListHeader, cap, word);
        kit::GameText(d, kit::GameFont::kListHeader, cap, ImVec2(cx - w * 0.5f, t.name.y + 2.0f * u), ink, word);
      } else {
        const ImVec2 w = f.ui->CalcTextSizeA(16.0f * u, 1e9f, 0, word);
        d->AddText(f.ui, 16.0f * u, ImVec2(cx - w.x * 0.5f, t.name.y + 1.0f * u), ink, word);
      }
    }
    std::lock_guard lock(g_layout_mutex);
    g_layout = std::move(layout), g_layout_at = Seconds();
  }
  if (!show_mute) return;
  for (const Row& row : rows) {
    const Table& t = tables[row.table];
    const float u = t.unit;
    // The "V" column: its letter is about 9 units wide; a row's line is 20 units from its pen down.
    const ImVec2 c(t.v.x + 4.5f * u, row.pen.y + 10.0f * u);
    const bool me = comms::IsMe(row.name);
    const Voice v = me ? (comms::Transmitting() ? Voice::kTalking : Voice::kQuiet) : VoiceOf(row.name);
    // Ink as the game's own rows: dark on its paper with a paper coloured edge (so it reads on the selected row's
    // green bar too), the waves green, the slash red; the player's own row, which the game draws on a black bar, in
    // the paper's colour.
    if (me)
      DrawIcon(d, c, 18.0f * u, v, IM_COL32(244, 238, 220, 255), IM_COL32(150, 226, 110, 255), IM_COL32(230, 60, 46, 255),
               now, 1.0f, IM_COL32(16, 16, 12, 255));
    else
      DrawIcon(d, c, 18.0f * u, v, IM_COL32(30, 28, 22, 255), IM_COL32(28, 124, 24, 255), IM_COL32(206, 34, 24, 255), now,
               1.0f, IM_COL32(242, 236, 216, 255));
  }
}

bool TakeGamerCard(const char* arg) {
  if (!REXCVAR_GET(bf_scoreboard_mute) || !arg) return false;
  const std::string name = RowName(arg);
  if (name.empty()) return true;  // never a gamer card: the PC has none
  if (comms::IsMe(name)) {
    REXLOG_INFO("BF scoreboard: Y on your own row ({}): nothing to mute", name);
    return true;
  }
  const bool m = comms::ToggleMuted(name);
  REXLOG_INFO("BF scoreboard: {} {} from the game's scoreboard (its Y)", comms::PlainName(name), m ? "muted" : "unmuted");
  return true;
}

}  // namespace bf1943::mute
