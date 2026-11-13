// The players on the pause menu's map (0.4.1, TekRantGaming 13 Nov 2026: "friendly and enemy units dont show on the
// pause menu map they should", and "the pause menu map dose now show active players on the map in the bomber view";
// enemies: "Only spotted ones").
//
// The game's minimap draws a team mate as a small white arrow it tints (the icon atlas's 32 x 32 picture at 277, 66;
// the player's own is the one at 305, 0), and only while that minimap is drawn: the pause menu's map showed them
// untinted at half size, white on the island, and nothing at all in an air raid's bomber view. So the map now reads
// the players from the game's own list, as the minimap does:
//   - the player manager (the global at 0x82AF3AE4; comms.cpp ReadTeam) holds the local player at +0xB8 and every
//     player of the match from the pointer at +0x98 to the one at +0x9C (run v41units5, 13 Nov: three players, the
//     same three in two scans 15 s apart);
//   - a player (vtables 0x8200C960 and 0x8200C94C) has its team at +0x30 and its squad at +0x34 (24: none), and its
//     soldier at +0x138 (vtable 0x82007518), the soldier's feet at +0x3D0 (x, y, z: 1.6 m under the camera, which is
//     at +0x2E0).
// A team mate is always on the map. An enemy only while the game shows him to this player: its minimap drew a
// player's arrow where he is a moment ago (the game's own rule of who is seen; in a bomber view, with no minimap, the
// last sighting is kept a few seconds).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>

#include "capture_points.h"
#include "comms.h"

#if defined(BF_DEV_TOOLS)
#include "dev_guest_memory.h"
#endif

namespace bf1943 {
namespace {

using Clock = std::chrono::steady_clock;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
float LoadF(const uint8_t* base, uint32_t a) {
  const uint32_t u = Load32(base, a);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
bool Plausible(uint32_t a) { return a >= 0x00100000u && a < 0xFFF00000u && !(a & 3); }
bool IsPlayer(const uint8_t* base, uint32_t p) {
  return Plausible(p) && Load32(base, p) == 0x8200C960u && Load32(base, p + 4) == 0x8200C94Cu;
}

constexpr uint32_t kManager = 0x82AF3AE4;
constexpr uint32_t kSoldierVtable = 0x82007518;
constexpr int kMaxPlayers = 64;

std::mutex g_mutex;
std::vector<MapPlayer> g_players;          // as last read (a few times a second)
std::map<uint32_t, Clock::time_point> g_sighted;  // an enemy (his player object): when the minimap last showed him
Clock::time_point g_read{};

// The minimap's player arrows of the last third of a second (the team mates' and a seen enemy's; not the player's own).
std::vector<MapIcon> MinimapArrows(const std::vector<MapIcon>& icons) {
  std::vector<MapIcon> out;
  for (const MapIcon& i : icons)
    if (int(std::lround(i.u0 * 512)) == 277 && int(std::lround(i.v0 * 256)) == 66) out.push_back(i);
  return out;
}

}  // namespace

std::vector<MapPlayer> MapPlayers() {
  std::lock_guard lock(g_mutex);
  if (Clock::now() - g_read > std::chrono::seconds(1)) return {};  // none read lately: no match, or a menu
  return g_players;
}

void MapPlayersTick(const uint8_t* base, bool level_loaded) {
  const auto now = Clock::now();
  if (!level_loaded) {
    std::lock_guard lock(g_mutex);
    g_players.clear(), g_sighted.clear();
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    if (now - g_read < std::chrono::milliseconds(100)) return;
  }
  const uint32_t manager = Load32(base, kManager);
  if (!Plausible(manager)) return;
  const uint32_t local = Load32(base, manager + 0xB8);
  if (!IsPlayer(base, local)) return;
  const uint32_t my_team = Load32(base, local + 0x30), my_squad = Load32(base, local + 0x34);
  const uint32_t begin = Load32(base, manager + 0x98), end = Load32(base, manager + 0x9C);
  if (!Plausible(begin) || !Plausible(end) || end < begin || (end - begin) / 4 > kMaxPlayers) return;
  const std::vector<MapIcon> icons = MapIcons();
  const std::vector<MapIcon> arrows = MinimapArrows(icons);
  // The minimap draws its arrows every frame while it is up: a sighting older than a moment is over, unless no
  // minimap is drawn now (a bomber view): then the last one is kept a little.
  const auto keep = icons.empty() ? std::chrono::seconds(5) : std::chrono::milliseconds(600);
  std::vector<MapPlayer> players, everyone;
  [[maybe_unused]] std::string skipped;  // the dev log's: the players not on the map, and why
  std::lock_guard lock(g_mutex);
  // First every player and where he is; then who each minimap arrow is (below).
  for (uint32_t e = begin; e < end; e += 4) {
    const uint32_t p = Load32(base, e);
    if (p == local || !IsPlayer(base, p)) continue;
    const uint32_t team = Load32(base, p + 0x30), squad = Load32(base, p + 0x34);
    if (team < 1 || team > 2) continue;  // a spectator, or a player still joining
    const uint32_t soldier = Load32(base, p + 0x138);
    if (!Plausible(soldier) || Load32(base, soldier) != kSoldierVtable) {  // dead, or not spawned
#if defined(BF_DEV_TOOLS)
      skipped += fmt::format(" [{:08X} team {} soldier {:08X} vtable {:08X}]", p, team, soldier,
                             Plausible(soldier) ? dev::SafeLoad32(base, soldier) : 0u);
#endif
      continue;
    }
    MapPlayer m;
    m.id = p;
    m.x = LoadF(base, soldier + 0x3D0), m.y = LoadF(base, soldier + 0x3D4), m.z = LoadF(base, soldier + 0x3D8);
    // The way he faces: his transform's third row (right +0x3A0, up +0x3B0, forward +0x3C0, place +0x3D0; run
    // v41units6), on the ground.
    m.fx = LoadF(base, soldier + 0x3C0), m.fz = LoadF(base, soldier + 0x3C8);
    if (!std::isfinite(m.fx) || !std::isfinite(m.fz) || std::hypot(m.fx, m.fz) < 0.2f) m.fx = m.fz = 0;
    if (!std::isfinite(m.x) || !std::isfinite(m.z) || std::fabs(m.x) > 4096 || std::fabs(m.z) > 4096) continue;
    m.enemy = my_team >= 1 && my_team <= 2 && team != my_team;
    m.squad = !m.enemy && squad < 24 && squad == my_squad;
    everyone.push_back(m);
  }
  // Seen: the game's minimap draws an arrow where a player it shows is (at his feet: run v41units6, to a centimetre).
  // Each arrow is the nearest player's, within 1.5 m (a team mate beside an enemy is not the enemy: run v41units7).
  for (const MapIcon& a : arrows) {
    MapPlayer* nearest = nullptr;
    float best = 1.5f;
    for (MapPlayer& m : everyone)
      if (const float d = std::hypot(a.x - m.x, a.z - m.z); d < best) best = d, nearest = &m;
    if (nearest && nearest->enemy) g_sighted[nearest->id] = now;
  }
  for (MapPlayer& m : everyone) {
    if (m.enemy) {
      const auto it = g_sighted.find(m.id);
      m.spotted = it != g_sighted.end() && now - it->second < keep;
    }
    if (!m.enemy || m.spotted) players.push_back(m);
  }
  for (auto it = g_sighted.begin(); it != g_sighted.end();)
    it = now - it->second > std::chrono::seconds(30) ? g_sighted.erase(it) : std::next(it);
  g_players = std::move(players);
  g_read = now;
#if defined(BF_DEV_TOOLS)
  // BF_DEV_PAUSEMAP=1: every player the map was given, every 3 s.
  static const bool dev = std::getenv("BF_DEV_PAUSEMAP") != nullptr;
  static Clock::time_point logged{};
  if (dev && now - logged > std::chrono::seconds(3)) {
    logged = now;
    std::string text;
    for (const MapPlayer& m : everyone)
      text += fmt::format(" [{:08X} {} at {:.1f} {:.1f} {:.1f}{}]", m.id, m.enemy ? "enemy" : m.squad ? "squad" : "team",
                          m.x, m.y, m.z, m.enemy ? (m.spotted ? " seen" : " not seen") : "");
    std::string seen;
    for (const MapIcon& a : arrows) seen += fmt::format(" ({:.1f} {:.1f} colour {:08X})", a.x, a.z, a.color);
    REXLOG_INFO("BF pause map: {} players for the map (of {} in the match, team {}):{}; the minimap's {} arrows:{}; "
                "not on it:{}",
                g_players.size(), (end - begin) / 4, my_team, text, arrows.size(), seen, skipped.empty() ? " none" : skipped);
    // Which way the third row points: the local soldier's against the camera's back (a soldier faces where he looks).
    {
      const uint32_t mine = Load32(base, local + 0x138);
      float cam[3], right[3], back[3];
      if (Plausible(mine) && Load32(base, mine) == kSoldierVtable && comms::Camera(cam, right, back))
        REXLOG_INFO("BF pause map: the local soldier's third row ({:.2f} {:.2f} {:.2f}), the camera's back ({:.2f} {:.2f} "
                    "{:.2f}); his feet ({:.1f} {:.1f} {:.1f}), the camera ({:.1f} {:.1f} {:.1f})", LoadF(base, mine + 0x3C0),
                    LoadF(base, mine + 0x3C4), LoadF(base, mine + 0x3C8), back[0], back[1], back[2],
                    LoadF(base, mine + 0x3D0), LoadF(base, mine + 0x3D4), LoadF(base, mine + 0x3D8), cam[0], cam[1], cam[2]);
      else
        REXLOG_INFO("BF pause map: the local soldier {:08X} (vtable {:08X})", mine, Plausible(mine) ? dev::SafeLoad32(base, mine) : 0u);
    }
    // Which way a soldier faces: the four rows before and at his feet (a transform's right, up, forward, place?).
    if (!everyone.empty()) {
      const uint32_t soldier = Load32(base, everyone.front().id + 0x138);
      std::string rows;
      if (dev::GuestReadable(base, soldier + 0x3A0, 0x40))
        for (uint32_t r = 0x3A0; r <= 0x3D0; r += 0x10)
          rows += fmt::format(" +{:X}=({:.2f} {:.2f} {:.2f} {:.2f})", r, LoadF(base, soldier + r), LoadF(base, soldier + r + 4),
                              LoadF(base, soldier + r + 8), LoadF(base, soldier + r + 12));
      REXLOG_INFO("BF pause map: {:08X}'s soldier {:08X}:{}", everyone.front().id, soldier, rows);
    }
  }
#endif
}

}  // namespace bf1943
