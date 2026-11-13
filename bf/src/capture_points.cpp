// Capture point letters (TekRantGaming, 13 Nov 2026: "modern battlefield games have the capture points lettered A, B,
// C and so on and I want this in our version too but lets not remove the flags can we have a letter be inside the
// flag ... make sure that the assigned letter to each capture point remains the exact same on each map between
// restarts and different play sessions").
//
// The game's capture points (the research of 13 Nov, checked in a match, run cplog1):
//   - each is a ClientCapturePointEntity (vtable 0x8200AD70): its data at +0x14 (CapturePointEntityData, vtable
//     0x8205CF60: the transform at +0x30 with the position at +0x60, Index at +0xA4, ControlPointName at +0x11C,
//     ShowOnMinimap at +0x12E, UnableToChangeTeam at +0x135), the team that owns it at +0x78 (0 none, 1, 2);
//   - the level's list: W = [[0x82AF3AE0] + 4], id = u16 [0x82D54904], the entity pointers from
//     [[W + 0x30A4] + id * 16] to [[W + 0x30A4] + id * 16 + 4] (sub_8229F450).
// The letter rule: of the level's points a team can take (UnableToChangeTeam 0) that the map shows (ShowOnMinimap 1),
// in the order of the level file's own Index (then its name), A, B, C... The level file never changes, so a point
// has the same letter in every match: Wake Island's A is its South Base, B the Beach, C the Airfield, D the North
// Base, E the North Village.
//
// The flags the game draws, all from the atlas menu/minimap/icons0 (512 x 256): the row under the ticket bars
// (sub_82457EE8), the markers in the world (sub_8246F960) and the ones on the minimap and the deploy screen's map
// (sub_824354B0). Each asks sub_826A4E48 (r4 the icon, r6 the frame's rectangle out: MinU, MinV, MaxU, MaxV) where
// its picture is in the atlas. The letters are part of the picture: the atlas's empty right hand side gets a copy of
// each flag (white, blue, red) with each letter drawn in (hud_glyphs.cpp's patcher calls DrawFlagLetters), and the
// hook below answers a flag's lookup with its lettered copy. So a letter moves, fades and scales with its flag, and
// a flag being taken blinks as before (its blank frame is left alone).
//   - a marker in the world and one on a map: the letter of the capture point at its place; the game's own choice
//     of white, blue or red stays;
//   - the row: the game's row is a tally (so many blue, then white, then red). With letters it is the points in
//     letter order, each in its owner's colour, as the newer Battlefield games show them.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "capture_points.h"
#include "comms.h"

#if defined(BF_DEV_TOOLS)
#include "dev_guest_memory.h"
#endif

REXCVAR_DEFINE_BOOL(bf_flag_letters, true, "BF1943",
                    "Letters in the capture point flags (A, B, C... the same point has the same letter in every match): "
                    "the row under the ticket bars, the markers in the world and the maps");

namespace bf1943 {
namespace {

#include "capture_letters.inc"

uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t Load16(const uint8_t* base, uint32_t a) { return uint32_t(base[a]) << 8 | base[a + 1]; }
float LoadF(const uint8_t* base, uint32_t a) {
  const uint32_t u = Load32(base, a);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
void StoreF(uint8_t* base, uint32_t a, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  base[a] = uint8_t(u >> 24), base[a + 1] = uint8_t(u >> 16), base[a + 2] = uint8_t(u >> 8), base[a + 3] = uint8_t(u);
}
// A guest address an object can be at (the image's data, the heaps): never a small number or an odd one.
bool Plausible(uint32_t a) { return a >= 0x00100000u && a < 0xFFF00000u && !(a & 3); }

constexpr uint32_t kEntityVtable = 0x8200AD70, kDataVtable = 0x8205CF60;
constexpr uint32_t kRowCaller = 0x824580EC;      // sub_82457EE8: the row's flag, its slot in r31, their count in r28
constexpr uint32_t kWorldCaller = 0x8246FCE8;    // sub_8246F960: a marker in the world, its item in r31 (place +0x40)
constexpr uint32_t kMinimapCaller = 0x824356FC;  // sub_824354B0: an icon on a map, its place at r28 - 0x30

// The atlas: the game's flag pictures, and where their lettered copies go (its right hand side is empty from 394).
constexpr int kAtlasW = 512, kAtlasH = 256, kFlag = 32;
struct Texel {
  int x, y;
};
constexpr Texel kBig[3] = {{288, 130}, {328, 32}, {241, 212}};    // white (no owner), blue (friendly), red (enemy)
constexpr Texel kSmall[3] = {{373, 58}, {352, 136}, {373, 72}};   // the maps' 20 x 14 ones, the same order
constexpr Texel Lettered(int kind, int letter) { return {400 + kind * 36, letter * kFlag}; }

std::mutex g_mutex;
std::vector<CapturePoint> g_points;  // the level's points as last read, lettered ones first in letter order
std::chrono::steady_clock::time_point g_read{};
std::atomic<bool> g_atlas_ready{false};  // the lettered copies are in the atlas the game draws from

// Each lettered point's flag as the game's own maps draw it now (its minimap asks for the white, blue or red small
// flag, blinking or not, every frame, for every point wherever it is): the row and the pause menu's map show the
// same, so a point never has two colours on one screen.
struct FlagState {
  int kind = -1;
  bool blink = false;
  std::chrono::steady_clock::time_point seen{};
};
FlagState g_flag_state[kLetterCount];
constexpr uint32_t kSmallFlagIcon = 0x89, kBlankX = 352, kBlankY = 116;  // the icons 0x89 white, 0x8A blue, 0x8B red

// The minimap's icons as the game last drew them, by the game's own item (the pause menu's map).
struct MapIconSeen {
  MapIcon icon;
  uint64_t order = 0;
  std::chrono::steady_clock::time_point seen{};
};
std::map<uint32_t, MapIconSeen> g_map_icons;
uint64_t g_map_order = 0;

// The level's capture points, read from the game's own list. Every pointer is checked before it is followed; a
// list that does not look right gives none.
std::vector<CapturePoint> ReadPoints(const uint8_t* base) {
  std::vector<CapturePoint> out;
  const uint32_t holder = Load32(base, 0x82AF3AE0);
  if (!Plausible(holder)) return out;
  const uint32_t world = Load32(base, holder + 4);
  if (!Plausible(world)) return out;
  const uint32_t table = Load32(base, world + 0x30A4);
  if (!Plausible(table)) return out;
  const uint32_t id = Load16(base, 0x82D54904);
  if (id > 0x400) return out;
  const uint32_t begin = Load32(base, table + id * 16), end = Load32(base, table + id * 16 + 4);
  if (!Plausible(begin) || !Plausible(end) || end < begin || end - begin > 64 * 4) return out;
  for (uint32_t at = begin; at < end; at += 4) {
    const uint32_t entity = Load32(base, at);
    if (!Plausible(entity) || Load32(base, entity) != kEntityVtable) continue;
    const uint32_t data = Load32(base, entity + 0x14);
    if (!Plausible(data) || Load32(base, data) != kDataVtable) continue;
    CapturePoint p;
    p.entity = entity;
    p.order = int(out.size());
    p.index = int(Load32(base, data + 0xA4));
    p.x = LoadF(base, data + 0x60), p.y = LoadF(base, data + 0x64), p.z = LoadF(base, data + 0x68);
    p.capturable = base[data + 0x135] == 0 && base[data + 0x12E] != 0;
    const uint32_t owner = Load32(base, entity + 0x78);
    p.owner = owner <= 2 ? int(owner) : 0;
    if (const uint32_t name = Load32(base, data + 0x11C); name >= 0x00100000u && name < 0xFFF00000u) {
      for (uint32_t i = 0; i < 63; ++i) {
        const char c = char(base[name + i]);
        if (!c) break;
        if (c < 32 || c > 126) {
          p.name.clear();
          break;
        }
        p.name += c;
      }
    }
    out.push_back(std::move(p));
  }
  // The letters: the points a team can take, by the level file's Index, then by name.
  std::stable_sort(out.begin(), out.end(), [](const CapturePoint& a, const CapturePoint& b) {
    if (a.capturable != b.capturable) return a.capturable;
    if (a.index != b.index) return a.index < b.index;
    return a.name < b.name;
  });
  int letter = 0;
  for (CapturePoint& p : out) p.letter = p.capturable && letter < 26 ? letter++ : -1;
  return out;
}

// The points, read again when the last reading is a quarter of a second old (an owner changes with a capture).
std::vector<CapturePoint> Points(const uint8_t* base) {
  std::lock_guard lock(g_mutex);
  const auto now = std::chrono::steady_clock::now();
  if (now - g_read > std::chrono::milliseconds(250)) {
    g_read = now;
    std::vector<CapturePoint> next = ReadPoints(base);
    if (next.size() != g_points.size()) {
      std::string letters;
      for (const CapturePoint& p : next)
        if (p.letter >= 0) letters += fmt::format(" {} {}", char('A' + p.letter), p.name);
      REXLOG_INFO("BF flags: {} capture points in this level{}{}", next.size(), letters.empty() ? "" : ":", letters);
    }
    g_points = std::move(next);
  }
  return g_points;
}

// The letter of the capture point at this place on the ground (a marker stands a few metres above its point), or -1.
int LetterAt(const std::vector<CapturePoint>& points, float x, float z) {
  int best = -1;
  float best_d = 20.0f * 20.0f;
  for (const CapturePoint& p : points) {
    if (p.letter < 0) continue;
    const float d = (p.x - x) * (p.x - x) + (p.z - z) * (p.z - z);
    if (d < best_d) best_d = d, best = p.letter;
  }
  return best;
}

// Which of three pictures a frame's rectangle is (by its first corner, in texels), or -1.
int KindOf(const Texel* set, float u, float v) {
  const int x = int(std::lround(u * kAtlasW)), y = int(std::lround(v * kAtlasH));
  for (int k = 0; k < 3; ++k)
    if (set[k].x == x && set[k].y == y) return k;
  return -1;
}

void StoreRect(uint8_t* base, uint32_t out, float x0, float y0, float x1, float y1) {
  StoreF(base, out, x0 / kAtlasW), StoreF(base, out + 4, y0 / kAtlasH);
  StoreF(base, out + 8, x1 / kAtlasW), StoreF(base, out + 12, y1 / kAtlasH);
}

// The flag state of a letter, if the game drew it in the last half second.
bool FreshState(int letter, FlagState& out) {
  if (letter < 0 || letter >= kLetterCount) return false;
  std::lock_guard lock(g_mutex);
  out = g_flag_state[letter];
  return out.kind >= 0 && std::chrono::steady_clock::now() - out.seen < std::chrono::milliseconds(500);
}

// After the game's own lookup: a flag's rectangle becomes its lettered copy's.
void Letter(uint8_t* base, uint32_t lr, uint32_t icon, uint32_t blink, uint32_t out, uint32_t r31, uint32_t r28) {
  if (!Plausible(out)) return;
  const bool letters = REXCVAR_GET(bf_flag_letters) && g_atlas_ready.load(std::memory_order_relaxed);
  const float u = LoadF(base, out), v = LoadF(base, out + 4);
  if (lr == kRowCaller) {
    if (!letters || KindOf(kBig, u, v) < 0) return;
    const std::vector<CapturePoint> points = Points(base);
    const int team = comms::Team();
    int lettered = 0;
    for (const CapturePoint& p : points) lettered += p.letter >= 0;
    // The row as the points in letter order: only when it has one flag a point (else the game's own tally stays).
    if (r28 != uint32_t(lettered) || r31 >= uint32_t(lettered) || lettered > kLetterCount) return;
    for (const CapturePoint& p : points) {
      if (p.letter != int(r31)) continue;
      // The colour the game's own maps give the point now, and its blinking while it is being taken; without a map
      // drawn, its owner's (the player's team known).
      FlagState state;
      int kind = -1;
      if (FreshState(p.letter, state)) {
        kind = state.kind;
        if (state.blink) {  // four frames a second, as the game's flags blink (the icons' FrameRate)
          const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count();
          if ((ms / 250) % 2 == 1) {
            StoreRect(base, out, float(kBlankX), float(kBlankY), float(kBlankX + 20), float(kBlankY + 20));
            return;
          }
        }
      } else if (team >= 1) {
        kind = p.owner == 0 ? 0 : p.owner == team ? 1 : 2;
      }
      if (kind < 0) return;
      const Texel t = Lettered(kind, p.letter);
      StoreRect(base, out, float(t.x), float(t.y), float(t.x + kFlag), float(t.y + kFlag));
    }
    return;
  }
  if (lr == kWorldCaller) {
    const int kind = KindOf(kBig, u, v);
    if (!letters || kind < 0 || !Plausible(r31)) return;
    const int letter = LetterAt(Points(base), LoadF(base, r31 + 0x40), LoadF(base, r31 + 0x48));
    if (letter < 0 || letter >= kLetterCount) return;
    const Texel t = Lettered(kind, letter);
    StoreRect(base, out, float(t.x), float(t.y), float(t.x + kFlag), float(t.y + kFlag));
    return;
  }
  if (lr == kMinimapCaller) {
    if (!Plausible(r28)) return;
    const bool flag_icon = icon >= kSmallFlagIcon && icon <= kSmallFlagIcon + 2;
    const int kind = KindOf(kSmall, u, v);
    if (!flag_icon && kind < 0) return;
    const int letter = LetterAt(Points(base), LoadF(base, r28 - 0x30), LoadF(base, r28 - 0x28));
    if (letter < 0 || letter >= kLetterCount) return;
    if (flag_icon) {  // what the game shows for this point now (also between two blinks, when the frame is blank)
      std::lock_guard lock(g_mutex);
      g_flag_state[letter] = {int(icon - kSmallFlagIcon), (blink & 0xFF) != 0, std::chrono::steady_clock::now()};
    }
    if (!letters || kind < 0) return;
    // The maps draw a 20 x 14 flag: the part of the lettered copy around its flag with those proportions (30 x 21
    // texels, so the letter is sharper than a 20 x 14 picture of it could be).
    const Texel t = Lettered(kind, letter);
    StoreRect(base, out, float(t.x + 1), float(t.y + 5), float(t.x + 31), float(t.y + 26));
  }
}

// An icon the minimap is about to draw (its rectangle as the game will use it, a flag's letter included).
void NoteMapIcon(const uint8_t* base, uint32_t out, uint32_t item) {
  if (!Plausible(out) || !Plausible(item)) return;
  MapIcon icon;
  icon.u0 = LoadF(base, out), icon.v0 = LoadF(base, out + 4), icon.u1 = LoadF(base, out + 8), icon.v1 = LoadF(base, out + 12);
  const int x = int(std::lround(icon.u0 * kAtlasW)), y = int(std::lround(icon.v0 * kAtlasH));
  if (x == int(kBlankX) && y == int(kBlankY)) return;  // the atlas's blank picture (between two blinks, or not shown)
  if (!(icon.u1 > icon.u0 && icon.v1 > icon.v0)) return;
  icon.x = LoadF(base, item - 0x30), icon.z = LoadF(base, item - 0x28);
  if (!std::isfinite(icon.x) || !std::isfinite(icon.z)) return;
  icon.flag = x >= 396 || KindOf(kSmall, icon.u0, icon.v0) >= 0;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(g_mutex);
  if (g_map_icons.size() > 512)
    for (auto it = g_map_icons.begin(); it != g_map_icons.end();)
      it = now - it->second.seen > std::chrono::seconds(2) ? g_map_icons.erase(it) : std::next(it);
  MapIconSeen& s = g_map_icons[item];
  s.icon = icon, s.order = ++g_map_order, s.seen = now;
}

#if defined(BF_DEV_TOOLS)
// BF_DEV_CPLOG=1: the capture point list every five seconds, and each caller and icon of the atlas lookup once, with
// what the registers the letters need hold (the research's reading of the code, checked in a match).
bool DevLog() {
  static const bool on = [] {
    const char* v = std::getenv("BF_DEV_CPLOG");
    return v && *v && *v != '0';
  }();
  return on;
}
struct Seen {
  uint32_t count = 0;
  float uv[4] = {};
  uint32_t r31 = 0, r28 = 0, r25 = 0, r24 = 0, r7 = 0;
  float pos_a[3] = {}, pos_b[3] = {};
};
std::map<uint64_t, Seen> g_seen;  // (lr, icon)
std::chrono::steady_clock::time_point g_logged{};
std::mutex g_dev_mutex;

void DevNote(const uint8_t* base, uint32_t lr, uint32_t icon, uint32_t out, uint32_t r31, uint32_t r28, uint32_t r25,
             uint32_t r24, uint32_t r7, bool found) {
  std::lock_guard lock(g_dev_mutex);
  Seen& s = g_seen[uint64_t(lr) << 32 | icon];
  ++s.count;
  s.r31 = r31, s.r28 = r28, s.r25 = r25, s.r24 = r24, s.r7 = r7;
  if (found && dev::GuestReadable(base, out, 16))
    for (int i = 0; i < 4; ++i) s.uv[i] = LoadF(base, out + i * 4);
  if (lr == kWorldCaller && dev::GuestReadable(base, r31 + 0x40, 12))
    for (int i = 0; i < 3; ++i) s.pos_a[i] = LoadF(base, r31 + 0x40 + i * 4);
  if (lr == kMinimapCaller && dev::GuestReadable(base, r28 - 0x30, 12))
    for (int i = 0; i < 3; ++i) s.pos_b[i] = LoadF(base, r28 - 0x30 + i * 4);
  const auto now = std::chrono::steady_clock::now();
  if (now - g_logged < std::chrono::seconds(5)) return;
  g_logged = now;
  const std::vector<CapturePoint> points = ReadPoints(base);
  REXLOG_INFO("BF capture points: {} in the level's list", points.size());
  for (const CapturePoint& p : points)
    REXLOG_INFO("BF capture points:   {} index {} {} at {:.1f} {:.1f} {:.1f} owner {} entity {:08X}{}",
                p.letter >= 0 ? std::string(1, char('A' + p.letter)) : std::string("-"), p.index, p.name, p.x, p.y, p.z,
                p.owner, p.entity, p.capturable ? "" : " (not for taking)");
  for (auto& [key, v] : g_seen) {
    REXLOG_INFO("BF capture points: lookup from {:08X} icon {:#x} x{}: uv {:.4f} {:.4f} {:.4f} {:.4f} (texels {:.0f} "
                "{:.0f} to {:.0f} {:.0f}) r31 {:08X} r28 {:08X} r25 {:08X} r24 {:08X} r7 {} pos {:.1f} {:.1f} {:.1f} / "
                "{:.1f} {:.1f} {:.1f}",
                uint32_t(key >> 32), uint32_t(key), v.count, v.uv[0], v.uv[1], v.uv[2], v.uv[3], v.uv[0] * 512,
                v.uv[1] * 256, v.uv[2] * 512, v.uv[3] * 256, v.r31, v.r28, v.r25, v.r24, v.r7, v.pos_a[0], v.pos_a[1],
                v.pos_a[2], v.pos_b[0], v.pos_b[1], v.pos_b[2]);
    v.count = 0;
  }
}
#endif

}  // namespace

std::vector<CapturePoint> CapturePoints() {
  std::lock_guard lock(g_mutex);
  std::vector<CapturePoint> out = g_points;
  const auto now = std::chrono::steady_clock::now();
  for (CapturePoint& p : out) {
    if (p.letter < 0 || p.letter >= kLetterCount) continue;
    const FlagState& s = g_flag_state[p.letter];
    if (s.kind >= 0 && now - s.seen < std::chrono::milliseconds(500)) p.shown = s.kind, p.contested = s.blink;
  }
  return out;
}

void CapturePointsTick(const uint8_t* base, bool level_loaded) {
  if (level_loaded) {
    Points(base);
    return;
  }
  std::lock_guard lock(g_mutex);
  g_points.clear();
  g_map_icons.clear();
  for (FlagState& s : g_flag_state) s = {};
}

std::vector<MapIcon> MapIcons() {
  std::vector<std::pair<uint64_t, MapIcon>> seen;
  {
    std::lock_guard lock(g_mutex);
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [item, s] : g_map_icons)
      if (now - s.seen < std::chrono::milliseconds(330)) seen.push_back({s.order, s.icon});
  }
  std::sort(seen.begin(), seen.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<MapIcon> out;
  out.reserve(seen.size());
  for (const auto& [order, icon] : seen) out.push_back(icon);
  return out;
}

FlagRect LetteredFlag(int kind, int letter) {
  const Texel t = Lettered(std::clamp(kind, 0, 2), std::clamp(letter, 0, kLetterCount - 1));
  return {t.x, t.y, kFlag, kFlag};
}

std::string CapturePointTitle(const std::string& name) {
  std::string s = name;
  for (const char* prefix : {"CQ_MP_BASE_", "CQ_MP_"})
    if (s.rfind(prefix, 0) == 0) {
      s = s.substr(std::strlen(prefix));
      break;
    }
  for (const char* map : {"WAKE_", "CANAL_", "IWO_", "CORAL_"})
    if (s.rfind(map, 0) == 0 && s.size() > std::strlen(map)) s = s.substr(std::strlen(map));
  if (s == "AIRSUPERIORITY") return "Air Superiority";
  bool start = true;
  for (char& c : s) {
    if (c == '_') {
      c = ' ', start = true;
      continue;
    }
    c = start ? char(std::toupper(static_cast<unsigned char>(c))) : char(std::tolower(static_cast<unsigned char>(c)));
    start = false;
  }
  return s;
}

void FlagLettersReady() { g_atlas_ready.store(true, std::memory_order_relaxed); }

// hud_glyphs.cpp's patcher, for the game's icon atlas (512 x 256, as R, G, B, A rows): the lettered copies of the
// three flags, a column a colour and a row a letter, in its empty right hand side. A copy is the game's own flag with
// the letter laid over it: dark on the white flag, white with a dark edge on the blue and the red one; only where
// the flag is (its transparent surround stays clear).
// 13 Nov 2026 (HD packs): `scale` times the size for an HD copy of the atlas (hud_glyphs.cpp), every place `scale`
// times as far and the letters' masks read between their texels.
int DrawFlagLetters(std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, int scale) {
  const int s = std::max(1, scale), aw = kAtlasW * s, ah = kAtlasH * s;
  if (width != uint32_t(aw) || height != uint32_t(ah) || rgba.size() < size_t(aw) * ah * 4) return 0;
  // The room the copies need must be empty in this atlas (another build of the game might use it). An upscaled copy
  // may carry a trace of its neighbours' edges there.
  const int clear = s > 1 ? 16 : 0;
  for (int y = 0; y < ah; ++y)
    for (int x = 396 * s; x < aw; ++x)
      if (rgba[(size_t(y) * aw + x) * 4 + 3] > clear) {
        REXLOG_WARN("BF flags: the icon atlas is not empty where the lettered flags go ({}, {}): no letters", x, y);
        return 0;
      }
  static constexpr uint8_t kInk[3][3] = {{28, 28, 26}, {250, 248, 240}, {250, 248, 240}};
  static constexpr uint8_t kEdge[3][3] = {{0, 0, 0}, {8, 20, 44}, {52, 10, 6}};
  auto mask = [&](int letter, int which, int x, int y) {  // the mask at an HD texel, between the mask's own
    const float fx = (x + 0.5f) / s - 0.5f, fy = (y + 0.5f) / s - 0.5f;
    const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    const float ax = fx - x0, ay = fy - y0;
    auto at = [&](int mx, int my) {
      mx = std::clamp(mx, 0, kLetterW - 1), my = std::clamp(my, 0, kLetterH - 1);
      return float(kLetterMask[letter][which][my * kLetterW + mx]);
    };
    const float top = at(x0, y0) * (1 - ax) + at(x0 + 1, y0) * ax;
    const float bottom = at(x0, y0 + 1) * (1 - ax) + at(x0 + 1, y0 + 1) * ax;
    return (top * (1 - ay) + bottom * ay) / 255.0f;
  };
  int drawn = 0;
  for (int kind = 0; kind < 3; ++kind)
    for (int letter = 0; letter < kLetterCount; ++letter) {
      const Texel from = kBig[kind], to = Lettered(kind, letter);
      for (int y = 0; y < kFlag * s; ++y)
        std::memcpy(&rgba[(size_t(to.y * s + y) * aw + to.x * s) * 4],
                    &rgba[(size_t(from.y * s + y) * aw + from.x * s) * 4], size_t(kFlag) * s * 4);
      for (int y = 0; y < kLetterH * s; ++y)
        for (int x = 0; x < kLetterW * s; ++x) {
          uint8_t* px = &rgba[(size_t((to.y + kLetterY) * s + y) * aw + (to.x + kLetterX) * s + x) * 4];
          if (px[3] == 0) continue;
          const float edge = kind == 0 ? 0.0f : mask(letter, 1, x, y) * 0.55f;
          const float ink = mask(letter, 0, x, y);
          for (int c = 0; c < 3; ++c) {
            float value = px[c] * (1.0f - edge) + kEdge[kind][c] * edge;
            value = value * (1.0f - ink) + kInk[kind][c] * ink;
            px[c] = uint8_t(std::clamp(value + 0.5f, 0.0f, 255.0f));
          }
        }
      ++drawn;
    }
  return drawn;
}

}  // namespace bf1943

REX_EXTERN(__imp__sub_826A4E48);
REX_HOOK_RAW(sub_826A4E48) {
  const uint32_t lr = uint32_t(ctx.lr), out = ctx.r6.u32, r31 = ctx.r31.u32, r28 = ctx.r28.u32;
  const uint32_t icon = ctx.r4.u32, r7 = ctx.r7.u32;
#if defined(BF_DEV_TOOLS)
  const uint32_t r25 = ctx.r25.u32, r24 = ctx.r24.u32;
#endif
  __imp__sub_826A4E48(ctx, base);
#if defined(BF_DEV_TOOLS)
  if (bf1943::DevLog()) bf1943::DevNote(base, lr, icon, out, r31, r28, r25, r24, r7, ctx.r3.u32 != 0);
#endif
  if (ctx.r3.u32 != 0 && (lr == bf1943::kRowCaller || lr == bf1943::kWorldCaller || lr == bf1943::kMinimapCaller)) {
    bf1943::Letter(base, lr, icon, r7, out, r31, r28);
    if (lr == bf1943::kMinimapCaller) bf1943::NoteMapIcon(base, out, r28);
  }
}
