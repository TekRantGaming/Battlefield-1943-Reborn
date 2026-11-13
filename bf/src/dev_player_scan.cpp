// Developer aid (BF_DEV_TOOLS builds): where the game keeps every player and where each one is, for the pause menu's
// map (0.4.1, TekRantGaming 13 Nov 2026: "friendly and enemy units dont show on the pause menu map").
//
//   BF_DEV_PLAYERSCAN=<seconds>   that long after the soldier is first in the world, and again 15 s later: the player
//                                 manager's words that point at players and its lists of them; for the local player,
//                                 the objects one and two pointers away that hold a position near the camera.
//
// The player manager (the global at 0x82AF3AE4) holds the local player at +0xB8 (comms.cpp ReadTeam); a player starts
// with the vtables 0x8200C960 and 0x8200C94C, its name (begin, end, capacity words) at +8, its id at +0x2C, its team at
// +0x30 and its squad at +0x34. Two scans 15 s apart in a match with other players tell a list from a slot and a
// position from a constant.

#if defined(BF_DEV_TOOLS)

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include <rex/logging.h>

#include "comms.h"
#include "dev_guest_memory.h"

namespace bf1943 {
namespace {

// Reads that never fault: a word that is not a pointer, or one to memory not mapped, reads 0 (the first scan followed
// one and closed the game: run v41units4).
uint32_t Load(const uint8_t* base, uint32_t a) { return dev::SafeLoad32(base, a); }
// An object's words read once its whole range is known to be readable (one check, not one a word).
uint32_t Raw(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
bool Readable(const uint8_t* base, uint32_t a, uint32_t size) { return dev::GuestReadable(base, a, size); }
float LoadF(const uint8_t* base, uint32_t a) {
  const uint32_t w = Raw(base, a);
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}
bool Plausible(uint32_t a) { return a >= 0x00100000u && a < 0xFFF00000u && !(a & 3); }
bool IsPlayer(const uint8_t* base, uint32_t p) {
  return Plausible(p) && Load(base, p) == 0x8200C960u && Load(base, p + 4) == 0x8200C94Cu;
}
std::string NameOf(const uint8_t* base, uint32_t player) {
  const uint32_t b = Load(base, player + 8), e = Load(base, player + 12);
  if (!Plausible(b) || e < b || e - b > 64 || !Readable(base, b, e - b)) return {};
  return std::string(reinterpret_cast<const char*>(base + b), e - b);
}
std::string Describe(const uint8_t* base, uint32_t p) {
  return fmt::format("{:08X} {} id {} team {} squad {}", p, NameOf(base, p), Load(base, p + 0x2C), Load(base, p + 0x30),
                     Load(base, p + 0x34));
}

// The places in `object` (up to `size` bytes) of three floats near `at` (within 4 m each way).
std::string NearPositions(const uint8_t* base, uint32_t object, uint32_t size, const float at[3]) {
  std::string out;
  if (!Readable(base, object, size)) return out;
  for (uint32_t k = 0; k + 12 <= size; k += 4) {
    const float x = LoadF(base, object + k), y = LoadF(base, object + k + 4), z = LoadF(base, object + k + 8);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
    if (std::fabs(x - at[0]) < 4 && std::fabs(y - at[1]) < 4 && std::fabs(z - at[2]) < 4 && std::fabs(x) > 1)
      out += fmt::format(" +{:X}=({:.1f} {:.1f} {:.1f})", k, x, y, z);
  }
  return out;
}

void Scan(const uint8_t* base, int pass) {
  const uint32_t manager = Load(base, 0x82AF3AE4);
  if (!Plausible(manager)) {
    REXLOG_INFO("BF player scan {}: no player manager", pass);
    return;
  }
  float cam[3], right[3], back[3];
  const bool have_cam = comms::Camera(cam, right, back);
  REXLOG_INFO("BF player scan {}: manager {:08X}, camera {}", pass, manager,
              have_cam ? fmt::format("{:.1f} {:.1f} {:.1f}", cam[0], cam[1], cam[2]) : std::string("not known"));
  if (!Readable(base, manager, 0x404)) return;
  for (uint32_t o = 0; o < 0x400; o += 4) {
    const uint32_t w = Raw(base, manager + o);
    if (IsPlayer(base, w)) REXLOG_INFO("BF player scan {}: manager+{:X} -> player {}", pass, o, Describe(base, w));
    const uint32_t end = Raw(base, manager + o + 4);
    if (Plausible(w) && Plausible(end) && end > w && (end - w) % 4 == 0 && (end - w) / 4 <= 64) {
      int players = 0;
      std::string list;
      if (!Readable(base, w, end - w)) continue;
      for (uint32_t e = w; e < end; e += 4) {
        const uint32_t p = Raw(base, e);
        if (IsPlayer(base, p)) ++players, list += " [" + Describe(base, p) + "]";
      }
      if (players >= 1) REXLOG_INFO("BF player scan {}: manager+{:X} a list of {}, {} players:{}", pass, o, (end - w) / 4, players, list);
    }
  }
  const uint32_t local = Load(base, manager + 0xB8);
  if (!IsPlayer(base, local) || !have_cam) return;
  REXLOG_INFO("BF player scan {}: local {}", pass, Describe(base, local));
  // One and two pointers away from the local player: the objects with a position near the camera.
  if (!Readable(base, local, 0x300)) return;
  for (uint32_t o = 0; o < 0x300; o += 4) {
    const uint32_t a = Raw(base, local + o);
    if (!Plausible(a) || a == local) continue;
    const std::string near_a = NearPositions(base, a, 0x600, cam);
    if (!near_a.empty()) REXLOG_INFO("BF player scan {}: player+{:X} -> {:08X} (vtable {:08X}):{}", pass, o, a, Load(base, a), near_a);
    if (!Readable(base, a, 0x200)) continue;
    for (uint32_t o2 = 0; o2 < 0x200; o2 += 4) {
      const uint32_t b = Raw(base, a + o2);
      if (!Plausible(b) || b == a || b == local) continue;
      const std::string near_b = NearPositions(base, b, 0x400, cam);
      if (!near_b.empty())
        REXLOG_INFO("BF player scan {}: player+{:X} -> {:08X} +{:X} -> {:08X} (vtable {:08X}):{}", pass, o, a, o2, b,
                    Load(base, b), near_b);
    }
  }
}

}  // namespace

void DevPlayerScan(const uint8_t* base, bool in_world) {
  static const double at = [] {
    const char* v = std::getenv("BF_DEV_PLAYERSCAN");
    return v && *v ? std::atof(v) : -1.0;
  }();
  if (at < 0) return;
  static std::chrono::steady_clock::time_point since{};
  static int done = 0;
  if (!in_world || done >= 2) return;
  const auto now = std::chrono::steady_clock::now();
  if (since == std::chrono::steady_clock::time_point{}) since = now;
  const double s = std::chrono::duration<double>(now - since).count();
  if ((done == 0 && s >= at) || (done == 1 && s >= at + 15)) Scan(base, ++done);
}

}  // namespace bf1943

#endif
