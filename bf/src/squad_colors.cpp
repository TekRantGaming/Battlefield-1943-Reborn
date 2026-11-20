// The squad's colour (TekRantGaming, 18 Nov 2026: "change the color of squad UI, player and squad mate icons in the
// world and on the map/minimap to a brighter Green").
//
// The game's HUD icons are white pictures in its icon atlas, tinted when drawn by their kind's colour from the level's
// HUD data (run hud_green1: the squad mates' icons and the squad's list drawn blue). The colours as drawn (runs
// hud_green6, sub_8241FA58's r4 for each kind on the minimap): UIHudIcon_SquadMember (4) 0.510 0.663 0.875, the
// player's own (LocalPlayer, 0) 0.439 0.561 0.322; the vehicles' icons white (their pictures coloured). Those two
// colours, as floats wherever they are in guest memory (the level's HUD data and its copies), become a brighter green
// a few seconds after each level is up (a thread of its own: reading all of guest memory takes about a second), and a
// map's icon drawn in either is drawn in the green (the hook, for any copy made later). The player's game only.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "capture_points.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DECLARE(bool, server);
REXCVAR_DEFINE_BOOL(bf_hud_squad_icons_smooth, true, "BF1943",
                    "Each squad mate's icon in the world follows him on every frame");
REXCVAR_DEFINE_BOOL(bf_hud_squad_bright_green, true, "BF1943",
                    "The squad's and the player's own icons on the HUD, the minimap and the maps in a brighter green");

namespace bf1943 {
bool InMatch();      // auto_join.cpp
bool LevelLoaded();  // auto_join.cpp
}  // namespace bf1943

namespace {

constexpr float kGreen[3] = {0.30f, 1.0f, 0.36f};

uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
float LoadF(const uint8_t* base, uint32_t a) {
  const uint32_t v = Load32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void StoreF(uint8_t* base, uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  uint8_t* p = base + a;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

bool Mapped(const uint8_t* base, uint32_t address, uint32_t size) {
#if defined(_WIN32)
  MEMORY_BASIC_INFORMATION mbi{};
  if (address < 0x10000 || !VirtualQuery(base + address, &mbi, sizeof(mbi))) return false;
  return mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
         uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base) + mbi.RegionSize >= uint64_t(address) + size;
#else
  (void)base, (void)address, (void)size;
  return false;
#endif
}

// The colours replaced: the squad's light blue and the player's olive, as the game's floats (big endian words).
constexpr uint32_t kSquadBlue[3] = {0x3F028F5C, 0x3F29BA5E, 0x3F600000};
constexpr uint32_t kPlayerOlive[3] = {0x3EE0C49C, 0x3F0F9DB2, 0x3EA4DD2F};

bool Is(const uint8_t* base, uint32_t a, const uint32_t (&c)[3]) {
  return Load32(base, a) == c[0] && Load32(base, a + 4) == c[1] && Load32(base, a + 8) == c[2];
}

// Every copy of the two colours in guest memory, made green.
void Scan(uint8_t* base) {
#if defined(_WIN32)
  const auto t0 = std::chrono::steady_clock::now();
  int squad = 0, player = 0, flash = 0;
  for (uint64_t a = 0x00010000; a < 0x100000000ull;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) break;
    const uint64_t start = std::max<uint64_t>(uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base), a);
    const uint64_t end = std::min<uint64_t>(start + mbi.RegionSize, 0x100000000ull);
    const bool image = start >= 0x80000000ull && start < 0xA0000000ull;
    const bool writable = mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY);
    if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && writable && !image)
      for (uint64_t p = start; p + 12 <= end; p += 4) {
        const uint32_t at = uint32_t(p);
        // The Flash HUD's own copies (run hud_names3): its text fields' colour 0xFF82A9DF (alpha, red, green, blue;
        // the squad list's names) and its scripts' value 0x0082A9DF (the colour the HUD's script gives the list).
        if (const uint32_t v = Load32(base, at); v == 0xFF82A9DF || v == 0x0082A9DF) {
          const uint32_t g = uint32_t(kGreen[0] * 255) << 16 | uint32_t(kGreen[1] * 255) << 8 | uint32_t(kGreen[2] * 255);
          const uint32_t nv = (v & 0xFF000000u) | g;
          uint8_t* q = base + at;
          q[0] = uint8_t(nv >> 24), q[1] = uint8_t(nv >> 16), q[2] = uint8_t(nv >> 8), q[3] = uint8_t(nv);
          ++flash;
          continue;
        }
        const bool sq = Is(base, at, kSquadBlue), pl = !sq && Is(base, at, kPlayerOlive);
        if (!sq && !pl) continue;
        for (int c = 0; c < 3; ++c) StoreF(base, at + 4 * c, kGreen[c]);
        ++(sq ? squad : player);
      }
    if (end <= a) break;
    a = end;
  }
  REXLOG_INFO("BF HUD: the squad's colour made green in {} places and {} of the Flash HUD's, the player's in {}, in {:.0f} ms",
              squad, flash, player,
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
#else
  (void)base;
#endif
}

// A client player's soldier's feet as they are now (its soldier at +0x138, its vtable 0x82007518, its feet at +0x3D0;
// map_players.cpp): `at` left as it is without one.
void SoldierNow(const uint8_t* base, uint32_t player, float (&at)[3]) {
  if (!base || !Mapped(base, player + 0x138, 4)) return;
  const uint32_t so = Load32(base, player + 0x138);
  if (Mapped(base, so, 0x3E0) && Load32(base, so) == 0x82007518)
    for (int i = 0; i < 3; ++i) at[i] = LoadF(base, so + 0x3D0 + 4 * i);
}

std::atomic<bool> g_scanning{false};
}  // namespace

namespace bf1943 {

// Whether the squad's colour is drawn green (hud_glyphs.cpp: the squad list's kit badges in the HUD's atlas).
bool SquadBrightGreen() { return !REXCVAR_GET(server) && REXCVAR_GET(bf_hud_squad_bright_green); }

// An icon a map is about to draw (flash_mouse.cpp's hook on sub_8241FA58: r4 its colour, four floats, a copy for
// this draw): the squad's or the player's colour drawn in the green. A vehicle's icon takes the colour of who is in it
// (run hud_kit7: the player's own jeep drawn in his colour, so green), so a squad mate's vehicle is green too.
void SquadColorsMapIcon(uint8_t* base, uint32_t vec) {
  if (!REXCVAR_GET(bf_hud_squad_bright_green) || !Mapped(base, vec, 16)) return;
  if (!Is(base, vec, kSquadBlue) && !Is(base, vec, kPlayerOlive)) return;
  for (int c = 0; c < 3; ++c) StoreF(base, vec + 4 * c, kGreen[c]);
}

// A marker in the world about to be placed on the screen (capture_points.cpp's hook on the icon lookup from
// sub_8246F960; its item, its place at +0x40, is projected right after, by sub_8241DF90). A squad mate's icon (the
// user, 18 Nov 2026: "the in-world squadmate names are nice and smooth when following the squadmates, but the in-world
// kit icons are not") is placed 1.6 m over his feet, but its place moves only now and then while his soldier moves on
// every frame (run hud_kit7: up to a tenth of a metre behind, then caught up). The place of a marker 1.2 to 2.0 m over
// a team mate whose feet are within 1.5 m of it, the nearest team mate being in the squad, is his feet now plus that
// height (1.6 m when within 0.15 of it).
void SquadColorsWorldMarker(uint8_t* base, uint32_t item) {
  if (!REXCVAR_GET(bf_hud_squad_icons_smooth) || REXCVAR_GET(server) || !Mapped(base, item + 0x40, 12)) return;
  const float x = LoadF(base, item + 0x40), y = LoadF(base, item + 0x44), z = LoadF(base, item + 0x48);
  // The team mates' feet, found once a frame for every marker of it (the user, 20 Nov 2026: 27 FPS on US Central's bot
  // server: this ran for each marker over every player, copying the list and checking each soldier's memory twice, so
  // with a dozen friendly bots round the player it cost the game's thread 10 to 25 ms a frame; runs fpsab_*).
  struct Mate {
    float at[3];
    bool squad;
  };
  thread_local std::vector<Mate> mates;
  thread_local std::chrono::steady_clock::time_point built{};
  if (const auto now = std::chrono::steady_clock::now(); now - built > std::chrono::milliseconds(3)) {
    built = now;
    mates.clear();
    for (const MapPlayer& m : MapPlayers()) {
      if (m.enemy) continue;
      Mate mate{{m.x, m.y, m.z}, m.squad};
      SoldierNow(base, m.id, mate.at);
      mates.push_back(mate);
    }
  }
  float best = 1.5f, feet[3] = {};
  bool squad = false;
  for (const Mate& m : mates) {
    const float d = std::hypot(m.at[0] - x, m.at[2] - z), up = y - m.at[1];
    if (d >= best || up < 1.2f || up > 2.0f) continue;
    best = d, squad = m.squad;
    std::copy(m.at, m.at + 3, feet);
  }
  if (!squad) return;
  const float up = y - feet[1];
  StoreF(base, item + 0x40, feet[0]);
  StoreF(base, item + 0x44, feet[1] + (std::fabs(up - 1.6f) < 0.15f ? 1.6f : up));
  StoreF(base, item + 0x48, feet[2]);
}

// From the pad hook (auto_join.cpp), each frame: a few seconds after a level is up, the scan, once a level.
void SquadColorsTick(uint8_t* base) {
  if (REXCVAR_GET(server) || !REXCVAR_GET(bf_hud_squad_bright_green)) return;
  static bool was_up = false;
  static std::chrono::steady_clock::time_point up_at{};
  static bool done = false;
  const bool up = InMatch() && LevelLoaded();
  if (up && !was_up) up_at = std::chrono::steady_clock::now(), done = false;
  was_up = up;
  if (!up || done || g_scanning.load() || std::chrono::steady_clock::now() - up_at < std::chrono::seconds(4)) return;
  done = true;
  g_scanning = true;
  std::thread([base] {
    Scan(base);
    g_scanning = false;
  }).detach();
}

}  // namespace bf1943
