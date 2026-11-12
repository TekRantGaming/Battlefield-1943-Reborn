// Developer aid (BF_DEV_TOOLS builds, Windows): where the game keeps the local player's team and squad, for voice and
// text chat's team and squad channels (comms.cpp, docs/COMMS-PLAN.md).
//
//   BF_DEV_TEAMSCAN=<seconds>       that long after the soldier is first in the world: scan a; then the game's own
//                                   leaveSquad, 15 s later scan b; then triggerLookingForSquad, 20 s later scan c
//   BF_DEV_TEAMSCAN_OUT=<folder>    where teamscan_<player name>_<a|b|c>.bin go (default: the working folder)
//
// Each scan writes records of (kind, address, start, size) and the bytes from start: kind 0 a copy of the player's
// name in writable guest memory (0x400 bytes before it, 0x800 from it), kind 1 a word that points at one of those
// copies (an object holding the name's text; 0x400 before, 0x800 from), kind 2 the local soldier entity (0x1000 from
// it). Two clients on different teams (tools\run_dedicated_test.ps1 -PeerPad) differ in the team's word, and a word
// that is a squad number in a and c but 0 in b is the squad's.

#if defined(BF_DEV_TOOLS)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <fmt/format.h>
#include <rex/logging.h>

namespace bf1943 {
const std::string& PlayerName();  // live_layers.cpp
uint32_t LocalSoldierEntity();    // look.cpp
void GameCommand(const std::string& name, const std::string& args);  // auto_join.cpp

namespace {
struct Region {
  uint32_t start, end;
};

// Every writable range of guest memory: the virtual heaps below 0x80000000 and the physical views above 0xA0000000
// (the soldier entities live at 0xC0000000 and up; the views show the same physical memory, so a copy can be found
// in more than one).
std::vector<Region> WritableRegions(const uint8_t* base) {
  std::vector<Region> out;
#if defined(_WIN32)
  for (uint64_t a = 0x00100000; a < 0xFFFF0000ull;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) break;
    const uint64_t start = uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base);
    const uint64_t end = std::min<uint64_t>(start + mbi.RegionSize, 0xFFFF0000ull);
    const bool writable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                          (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
    if (writable && start >= 0x00100000ull) out.push_back({uint32_t(start), uint32_t(end)});
    a = start + mbi.RegionSize;
  }
#else
  (void)base;
#endif
  return out;
}

bool Inside(const std::vector<Region>& regions, uint32_t a, uint32_t size) {
  for (const Region& r : regions)
    if (a >= r.start && uint64_t(a) + size <= r.end) return true;
  return false;
}

void Scan(const uint8_t* base, const char* tag) {
  const std::string name = PlayerName();
  if (name.size() < 4) {
    REXLOG_INFO("BF dev teamscan {}: no player name yet", tag);
    return;
  }
  const std::vector<Region> regions = WritableRegions(base);
  std::vector<uint32_t> hits;
  for (const Region& r : regions) {
    const uint8_t* p = base + r.start;
    const size_t n = r.end - r.start;
    for (size_t at = 0; at + name.size() <= n && hits.size() < 96;) {
      const void* f = std::memchr(p + at, name[0], n - name.size() + 1 - at);
      if (!f) break;
      at = size_t(static_cast<const uint8_t*>(f) - p);
      if (std::memcmp(p + at, name.data(), name.size()) == 0) hits.push_back(r.start + uint32_t(at));
      ++at;
    }
  }
  std::vector<uint32_t> pointers, sorted = hits;
  std::sort(sorted.begin(), sorted.end());
  const uint32_t lo = sorted.empty() ? 1 : sorted.front(), hi = sorted.empty() ? 0 : sorted.back();
  for (const Region& r : regions)
    for (uint32_t a = (r.start + 3) & ~3u; uint64_t(a) + 4 <= r.end && pointers.size() < 256; a += 4) {
      const uint8_t* m = base + a;
      const uint32_t w = uint32_t(m[0]) << 24 | uint32_t(m[1]) << 16 | uint32_t(m[2]) << 8 | m[3];
      if (w < lo || w > hi) continue;
      if (std::binary_search(sorted.begin(), sorted.end(), w)) pointers.push_back(a);
    }
  const char* dir = std::getenv("BF_DEV_TEAMSCAN_OUT");
  const std::string path = std::string(dir && *dir ? dir : ".") + "/teamscan_" + name + "_" + tag + ".bin";
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    REXLOG_WARN("BF dev teamscan {}: cannot write {}", tag, path);
    return;
  }
  int records = 0;
  auto record = [&](uint32_t kind, uint32_t address, uint32_t before, uint32_t after) {
    uint32_t start = address >= before ? address - before : 0, size = before + after;
    while (size > 0x100 && !Inside(regions, start, size)) start += 0x100, size -= 0x100;  // the region's edge
    if (!Inside(regions, start, size)) return;
    const uint32_t head[4] = {kind, address, start, size};
    std::fwrite(head, 4, 4, f);
    std::fwrite(base + start, 1, size, f);
    ++records;
  };
  for (uint32_t h : hits) record(0, h, 0x400, 0x800);
  for (uint32_t p : pointers) record(1, p, 0x400, 0x800);
  if (const uint32_t soldier = LocalSoldierEntity()) record(2, soldier, 0, 0x1000);
  // The player object (comms2, 11 Nov): its name string at +0 (begin, end, capacity), two vtables just before it
  // (0x8200C960, 0x8200C94C), the team at +0x28 and the squad at +0x2C. Which words point at it (level 1), and which
  // words point into the objects holding those (level 2): a path from a global to the local player.
  auto load = [&](uint32_t a) {
    const uint8_t* m = base + a;
    return uint32_t(m[0]) << 24 | uint32_t(m[1]) << 16 | uint32_t(m[2]) << 8 | m[3];
  };
  auto aliases = [](uint32_t a) {  // the physical views show the same memory at three addresses
    if (a < 0xA0000000u) return std::vector<uint32_t>{a};
    const uint32_t phys = a & 0x1FFFFFFFu;
    return std::vector<uint32_t>{0xA0000000u | phys, 0xC0000000u | phys, 0xE0000000u | phys};
  };
  // Words whose value falls in one of `ranges` (each [from, to], any view), at most max of them; only in the image
  // (0x82000000 to 0x84000000: the game's globals) when image_only.
  auto find_into = [&](const std::vector<std::pair<uint32_t, uint32_t>>& wanted, size_t max, bool image_only) {
    std::vector<std::pair<uint32_t, uint32_t>> ranges, out;
    for (const auto& [from, to] : wanted)
      for (uint32_t a : aliases(from)) ranges.push_back({a, a + (to - from)});
    for (const Region& r : regions) {
      if (image_only && (r.end <= 0x82000000u || r.start >= 0x84000000u)) continue;
      for (uint32_t a = (r.start + 3) & ~3u; uint64_t(a) + 4 <= r.end && out.size() < max; a += 4) {
        const uint32_t w = load(a);
        if (w < 0x00100000u) continue;
        for (const auto& [lo2, hi2] : ranges)
          if (w >= lo2 && w <= hi2) {
            out.push_back({a, w});
            break;
          }
      }
    }
    return out;
  };
  for (uint32_t p : pointers) {
    if (p < 8 || load(p - 8) != 0x8200C960u || load(p - 4) != 0x8200C94Cu) continue;
    REXLOG_INFO("BF dev teamscan {}: player object at {:08X}: id {} team {} squad {} flags {:08X}", tag, p - 8,
                load(p + 0x24), load(p + 0x28), load(p + 0x2C), load(p + 0x7C));
    const auto level1 = find_into({{p - 0x48, p + 0x10}}, 24, false);
    std::vector<std::pair<uint32_t, uint32_t>> holders;
    for (const auto& [l1, w1] : level1) {
      record(3, l1, 0x400, 0x400);
      const bool global = l1 >= 0x82000000u && l1 < 0x84000000u;
      REXLOG_INFO("BF dev teamscan {}:   level 1: {:08X} holds {:08X} (the name field {:+d}){}", tag, l1, w1,
                  int64_t(w1 & 0x1FFFFFFF) - int64_t(p & 0x1FFFFFFF), global ? " GLOBAL" : "");
      if (!global) holders.push_back({l1 - 0x800, l1});
    }
    // Globals that point into an object holding a level 1 word (up to 0x800 before it).
    for (const auto& [l2, w2] : find_into(holders, 48, true)) {
      std::string nearby;
      for (const auto& [l1, w1] : level1)
        if ((w2 & 0x1FFFFFFF) <= (l1 & 0x1FFFFFFF) && (l1 & 0x1FFFFFFF) - (w2 & 0x1FFFFFFF) <= 0x800)
          nearby += fmt::format(" {:08X}{:+d}", l1, int64_t(l1 & 0x1FFFFFFF) - int64_t(w2 & 0x1FFFFFFF));
      REXLOG_INFO("BF dev teamscan {}:     level 2 GLOBAL: {:08X} holds {:08X}: level 1 at{}", tag, l2, w2, nearby);
    }
    break;  // the aliases are the same object
  }
  std::fclose(f);
  std::string where;
  for (uint32_t h : hits) where += " " + std::to_string(h);
  REXLOG_INFO("BF dev teamscan {}: {} copies of {}, {} pointers to them, soldier {:08X}: {} records in {}", tag,
              hits.size(), name, pointers.size(), LocalSoldierEntity(), records, path);
}
}  // namespace

// From the pad read (auto_join.cpp), the game's thread: in_world while the soldier is on foot or seated.
void DevTeamScan(const uint8_t* base, bool in_world) {
  static const double after = [] {
    const char* v = std::getenv("BF_DEV_TEAMSCAN");
    return v && *v ? std::atof(v) : -1.0;
  }();
  if (after < 0) return;
  using Clock = std::chrono::steady_clock;
  static Clock::time_point first{};
  static int step = 0;
  const auto now = Clock::now();
  if (first == Clock::time_point{}) {
    if (!in_world) return;
    first = now;
    // The menu's command table (auto_join.cpp: name, number pairs at 0x82020080): every name, once.
    std::string names;
    for (uint32_t e = 0x82020080; e < 0x82020080 + 8 * 400; e += 8) {
      const uint8_t* m = base + e;
      const uint32_t at = uint32_t(m[0]) << 24 | uint32_t(m[1]) << 16 | uint32_t(m[2]) << 8 | m[3];
      const uint32_t num = uint32_t(m[4]) << 24 | uint32_t(m[5]) << 16 | uint32_t(m[6]) << 8 | m[7];
      if (at < 0x82000000u || at >= 0x84000000u) break;
      std::string n;
      for (uint32_t i = 0; i < 48 && base[at + i]; ++i) n += char(base[at + i]);
      names += fmt::format(" {}={:X}", n, num);
    }
    REXLOG_INFO("BF dev teamscan: the menu's commands:{}", names);
  }
  // Each scan runs on a worker (a full pass over guest memory takes seconds: the game thread must not wait), and the
  // squad command after it goes out once it is done.
  static std::atomic<int> done{-1};
  static int commanded = -1;
  if (done.load() == step - 1 && commanded < step - 1) {
    commanded = step - 1;
    if (commanded == 0) {
      GameCommand("leaveSquad", "");
      REXLOG_INFO("BF dev teamscan: leaveSquad sent");
    } else if (commanded == 1) {
      GameCommand("triggerLookingForSquad", "");
      REXLOG_INFO("BF dev teamscan: triggerLookingForSquad sent");
    }
  }
  static constexpr double kAt[] = {0, 15, 35};
  if (step >= 3 || done.load() != step - 1 || now - first < std::chrono::duration<double>(after + kAt[step])) return;
  const int s = step++;
  std::thread([base, s] {
    Scan(base, s == 0 ? "a" : s == 1 ? "b" : "c");
    done.store(s);
  }).detach();
}
}  // namespace bf1943

#endif
