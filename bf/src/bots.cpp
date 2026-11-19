// Bots (TekRantGaming, 18 Nov 2026: "add bots ... for offline play, and servers back-filling empty player slots").
//
// Step 0 (D198): a dedicated server holds players of its own, with no game behind them, spawns their soldiers and
// moves them with input it makes itself. The study and the plan: side-session note bots-findings.md.
//
// What the game has: no AI. The engine's AI data types are still in the image (AISettingsData, PathfindingSettings,
// BotPriorityConfigData, the spawn point's PlayerType SSHumanPlayer / SSAiPlayer), but no code that thinks for a
// soldier. What it does have, all on the server, and what a bot uses of it:
//   - ServerPlayer (vtable 0x8201A990, 640 bytes) made by the player manager's factory sub_82387EF0(manager, flag),
//     named and listed by sub_8238EFD8(manager, player, eastl string* name). A connection does both through
//     sub_82392658; the spawn point's SSAiPlayer branch (sub_8236BC98) does them for a player with no connection.
//   - Its input: the EntryInput at player+280 (sub_8236F230 sets it). A connection makes one (360 bytes, sub_822C1478)
//     and fills it from each client message (sub_823AF4A0: sub_822C15D0(input, state)), then calls the soldier's
//     input step, vtable +260 (sub_822F05C8: the entry's player's input into the soldier). A bot gets its own input,
//     filled once a server tick, and the same call. The state as a client sends it (run bots_s0k, a human's): the
//     analog concepts (0 forward, 1 strafe...), digital bits 40 (aim valid) and 43 (from the client), byte +122 =
//     0x09, the absolute aim yaw +136 and pitch +140 in radians (forward = (-sin yaw, 0, cos yaw)), the tick +152.
//     The copy into the input keeps the analog values only, so the rest goes into its current state (+352) after it.
//   - The kit: the player's soldier data at +500, set before the spawn (a human's comes from its deploy screen). The
//     level's TeamData objects (vtable 0x82062498; +0x0C its path, Objects/Teams/Us or Objects/Teams/RU (Japan),
//     +0x34 the team, +0x30 its kits, 3 objects of vtable 0x820631D4: Support, Assault, Specialist) give a kit's
//     soldier data at kit+0x3C (run bots_s0u: Objects/Soldiers/US/Support's is the human's +500).
//   - ServerSoldierSpawnEntity (vtable 0x82019CB0, made by sub_82362A40; team at +52, data at +20) spawns a player's
//     soldier there: sub_8236B688(entity, player, 1, 1, 0), as its event handler sub_8236D558 does for a human. Without
//     a kit it reads the null page (run bots_s0c). The round's start (sub_8238EB68) also spawns every listed player.
//   - NOT the game's AI player flag (player+532, sub_8236F768): with it a soldier takes its facing from the input but
//     never moves (runs bots_s0h..w: its own update feeds it for the removed AI's movement). Without it the bots
//     walk (run bots_s0x: 4.5 m/s, the heading as sent). It also spared a player the idle kick (sub_8237F1F8), which
//     the hook below does for the bots instead.
//
// BF_BOTS=<n> (or bf_bots) on a server: n bots, each on the side with fewer players as it joins, never more than the
// server has room for besides its human players (bf_gm_max_players). Everything here runs on the server thread, from the server's tick (server_rotation.cpp).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <array>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <queue>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <string>
#include <vector>

#include "bf1943_pch.h"

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
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(bool, server);  // server_mode.cpp
REXCVAR_DECLARE(std::string, user_data_root);
REXCVAR_DECLARE(int32_t, bf_gm_max_players);  // gm_host.cpp: the most players a match holds
REXCVAR_DEFINE_INT32(bf_bots, 0, "BF1943/Server", "Bots: how many bots play on this server (0: none)");
REXCVAR_DEFINE_BOOL(bf_bots_squad_humans, true, "BF1943/Server",
                    "Bots: a player in no squad is put in a squad with its side's bots, as its leader");

namespace bf1943 {
std::string CurrentMapTitle();  // server_rotation.cpp: the map now ("" without a rotation)
}  // namespace bf1943
using bf1943::CurrentMapTitle;

REX_EXTERN(sub_82387EF0);  // the player manager's factory (manager, flag): a new ServerPlayer, or 0 when full
REX_EXTERN(sub_821D51C8);  // the game's string assign (string, C text)
REX_EXTERN(sub_8238EFD8);  // the player manager's add (manager, player, eastl string* name)
REX_EXTERN(sub_822C1478);  // EntryInput's constructor (360 bytes)
REX_EXTERN(sub_822BD4A8);  // EntryInputState's reset (176 bytes)
REX_EXTERN(sub_822BD518);  // EntryInputState: set analog concept r4 to f1, clamped
REX_EXTERN(sub_822C15D0);  // EntryInput: take this state (input, state)
REX_EXTERN(sub_8240DFE0);  // the scoring's join (scoring, player): the player's record and its replicated score
REX_EXTERN(sub_82399AE8);  // the game's team pick (player, team, find a squad, keep the team's cap)
REX_EXTERN(sub_8236B688);  // ServerSoldierSpawnEntity: spawn this player's soldier here (entity, player, 1, 1, 0)
REX_EXTERN(sub_82398B98);  // a player into a vehicle's seat (player, vehicle, seat): free seats only, as a client's
REX_EXTERN(sub_822A7BB0);  // an entity's transform (4 x 4 floats, the position at +48)
REX_EXTERN(sub_8237B320);  // a player's squad (player, squad; 24: none): the first into an empty squad leads it
REX_EXTERN(sub_82375280);  // a player made squad leader or not (player, leader, r5 the player's +132 bit 0x10)

namespace {
using Clock = std::chrono::steady_clock;

constexpr uint32_t kGameContext = 0x82AF56F0;
constexpr uint32_t kServerPlayerVtable = 0x8201A990;
constexpr uint32_t kSoldierSpawnVtable = 0x82019CB0;
constexpr uint32_t kTeamDataVtable = 0x82062498;
constexpr uint32_t kKitVtable = 0x820631D4;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = base + a;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
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
  Store32(base, a, v);
}
bool InImage(uint32_t a) { return a >= 0x82000000u && a < 0x82DB0000u; }

// Guest memory of the game's heaps that is there to read (team_balance.cpp's check).
bool Readable(uint32_t address, uint32_t size) {
  if (address < 0x00100000u || address >= 0xFFF00000u || (address & 3) || address + size < address) return false;
  auto* state = rex::system::kernel_state();
  rex::memory::Memory* memory = state ? state->memory() : nullptr;
  if (!memory) return false;
  for (uint32_t page = address & ~0xFFFu; page < address + size;) {
    rex::memory::BaseHeap* heap = memory->LookupHeap(page);
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(page, &info)) return false;
    if (!(info.state & rex::memory::kMemoryAllocationCommit) || !(info.protect & rex::memory::kMemoryProtectRead))
      return false;
    const uint32_t next = info.base_address + info.region_size;
    if (next <= page) return false;
    page = next;
  }
  return true;
}

// Guest memory committed in the host's view (the level's data, outside the heaps Readable knows).
bool Mapped(const uint8_t* base, uint32_t address, uint32_t size) {
#if defined(_WIN32)
  MEMORY_BASIC_INFORMATION mbi{};
  if (address < 0x10000 || !VirtualQuery(base + address, &mbi, sizeof(mbi))) return false;
  return mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
         uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base) + mbi.RegionSize >= uint64_t(address) + size;
#else
  return Readable(address, size);
#endif
}

void CallGuest(PPCContext& ctx, uint8_t* base, uint32_t target);

uint32_t Alloc(uint32_t size) {
  const uint32_t a = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
  if (a) std::memset(rex::system::kernel_state()->memory()->TranslateVirtual(a), 0, size);
  return a;
}

// The bots' names: a first and a last name of the bot's side, common names of the 1940s (our own lists, no real
// person meant), at most 15 characters as a player's name (server/plasma.py), different from every name on the
// server. Marines: given name first; Japanese soldiers: family name first, as their names were written then.
constexpr const char* kUsFirst[] = {
    "James", "John",  "Robert", "William", "Charles", "George", "Joseph", "Edward", "Frank",  "Thomas", "Harold",
    "Walter", "Henry", "Paul",  "Richard", "Raymond", "Donald", "Jack",   "Albert", "Ralph",  "Eugene", "Carl",
    "Louis", "Earl",  "Fred",   "Roy",     "Leo",     "Joe",    "Bill",   "Hank",   "Gus",    "Lou",    "Ed",
    "Al",    "Bud",   "Tom",    "Sam",     "Vic",     "Ernie",  "Stan",   "Mike",   "Pete",   "Dan",    "Ray"};
constexpr const char* kUsLast[] = {
    "Smith",   "Johnson", "Miller",  "Davis",  "Wilson",   "Anderson", "Taylor",  "Moore",   "Jackson", "Martin",
    "Clark",   "Lewis",   "Walker",  "Hall",   "Young",    "King",     "Wright",  "Hill",    "Scott",   "Green",
    "Baker",   "Adams",   "Nelson",  "Carter", "Mitchell", "Roberts",  "Turner",  "Parker",  "Evans",   "Collins",
    "Stewart", "Murphy",  "Cook",    "Rogers", "Morgan",   "Bell",     "Kelly",   "Howard",  "Ward",    "Brooks",
    "Gray",    "Hughes",  "Price",   "Barnes", "Ross",     "Kowalski", "Russo",   "Sullivan", "Schmidt", "Novak",
    "Garcia",  "Lopez",   "Flynn",   "Doyle",  "Becker",   "Larson",   "Olsen",   "Burke",   "Hayes",   "Fisher"};
constexpr const char* kJpFamily[] = {
    "Sato",     "Suzuki",  "Takahashi", "Tanaka",  "Watanabe", "Ito",      "Yamamoto", "Nakamura", "Kobayashi",
    "Kato",     "Yoshida", "Yamada",    "Sasaki",  "Yamaguchi", "Matsumoto", "Inoue",   "Kimura",   "Hayashi",
    "Shimizu",  "Mori",    "Abe",       "Ikeda",   "Hashimoto", "Ishikawa", "Ogawa",    "Okada",    "Goto",
    "Hasegawa", "Murata",  "Endo",      "Aoki",    "Fujita",    "Nishimura", "Fukuda",  "Ota",      "Miura",
    "Okamoto",  "Matsuda", "Nakagawa",  "Harada",  "Ono",       "Takeda",   "Ueda",     "Sugiyama", "Kondo"};
constexpr const char* kJpGiven[] = {
    "Hiroshi", "Takeshi", "Kenji",  "Saburo", "Ichiro", "Jiro",   "Shiro",  "Isamu",  "Minoru", "Kiyoshi",
    "Masao",   "Tadashi", "Yoshio", "Haruo",  "Shigeru", "Osamu", "Noboru", "Akira",  "Katsuo", "Fumio",
    "Makoto",  "Toshio",  "Yukio",  "Kazuo",  "Hideo",  "Teruo",  "Sadao",  "Goro",   "Isao",   "Mitsuo",
    "Tetsuo",  "Yasuo",   "Masaru", "Susumu", "Takashi", "Hajime", "Tomio", "Kunio",  "Shoji",  "Eiji"};
struct Bot {
  uint32_t player = 0, input = 0, state = 0;
  int index = 0;
  int team = 0;  // the bot's side, kept through the round's start (the team pick hook below)
  std::string name;
  Clock::time_point died{}, next_log{};
  uint32_t last_soldier = 0;
  int spawn_tries = 0;
  float yaw = 0, pitch = 0;
  uint32_t tick = 0;
  // The brain (Think, 4 times a second) and what it asks of the next ticks (Drive).
  Clock::time_point next_think{}, next_look{}, detour_until{}, burst_until{}, burst_pause_until{};
  float forward = 0, strafe = 0, want_yaw = 0, want_pitch = 0;
  bool fire = false;
  uint32_t enemy = 0;                 // the enemy soldier it fights (0 none)
  float enemy_pos[3] = {};            // where it was last seen
  Clock::time_point enemy_seen_at{};
  int goal = -1;                      // the flag it goes for (index in g.flags)
  float goal_pos[3] = {};
  float detour_yaw = 0;
  float check_pos[3] = {};
  Clock::time_point check_at{};
  float aim_error_yaw = 0, aim_error_pitch = 0;
  // Its own take on the players' style: each trait times 0.75 to 1.25 (sprint, range, burst, sights, attack, flank).
  float jitter[6] = {1, 1, 1, 1, 1, 1};
  int kit_choice = -1;     // its kit for the next spawn (the players' kits by how often they took them)
  bool ads = false;        // aiming down the sights in this fight
  bool sprinter = true;    // sprints on its way to this goal
  Clock::time_point next_ride{};  // when it looks for a vehicle to ride in next
  bool defending = false;  // its goal is a flag its side holds
  std::vector<std::array<float, 3>> path;  // the way to its goal (cell centres), the next first
  float path_goal[3] = {};
  Clock::time_point replan_at{};
  bool sprint = false;
  Clock::time_point goal_until{}, wander_at{}, no_path_log{};
  bool was_in_area = false;
  bool watching = false;  // at a flag, facing where the enemy came from
  float watch_yaw = 0;
  int no_reach = 0;  // plans in a row that ended short of the goal
};

// A capture point: its data (CapturePointEntityData, vtable 0x8205CF60: the position at +0x60, the name at +0x11C,
// ShowOnMinimap +0x12E, UnableToChangeTeam +0x135) and the server's entity for it (the object whose +0x14 is the
// data, vtable 0x820190D0). Its fields (run bots_s1c, watched while bots took flags): +0x80 the team capturing it,
// +0x88..+0x90 the players in its area, +0x9C the capture's rate (+10 a second while its team is in the area, -10
// when it is left), +0xA0 the capture's progress (100: taken), +0xA8 set for a moment as a capture starts. A flag
// is held by +0x80's team once +0xA0 is 100 (run bots_s1e: the player's HUD showed them so).
struct Flag {
  uint32_t data = 0, entity = 0;
  float pos[3] = {};
  std::string name;
  bool takeable = true;
};

struct State {
  std::mutex mutex;  // the spawn points are noted while the level loads; the rest is the server thread's
  std::set<uint32_t> spawns;
  std::vector<Bot> bots;
  Clock::time_point level_at{};
  bool level_seen = false;
  uint32_t manager = 0, scanned_manager = 0;
  std::map<uint32_t, std::vector<uint32_t>> team_kits;  // team (1 US, 2 Japan) -> its kit objects
  std::vector<Flag> flags;
  std::set<uint32_t> vehicle_spawns;  // spawn points that seat their soldier in a vehicle
  // The level's vehicles (ServerVehicleEntity, vtable 0x82013068: its constructor sub_8230A580 and destructor
  // sub_822FF5B0 hooked below). Every tank, jeep, boat, plane and fixed gun the game treats as a vehicle
  // (sub_822F1CF8, the game's own "is a vehicle" test, compares its type with 0x82D55EF0, that class's).
  std::set<uint32_t> vehicles;
  uint32_t ray_scratch = 0;  // guest memory for the ray test's vectors (16 byte aligned)
  uint64_t rays = 0;         // ray tests made, and the time they took (logged every 10 s)
  double ray_ms = 0;
} g;

int WantedBots() {
  static const int env = [] {
    const char* v = std::getenv("BF_BOTS");
    return v && *v ? std::atoi(v) : -1;
  }();
  return std::clamp(env >= 0 ? env : REXCVAR_GET(bf_bots), 0, 24);
}

bool IsBot(uint32_t player) {
  for (const Bot& b : g.bots)
    if (b.player == player) return true;
  return false;
}

uint32_t Manager(const uint8_t* base) {
  if (!(Load32(base, 0x82AF574C) & 1)) return 0;  // the game context not made yet (sub_82349D78's guard)
  const uint32_t manager = Load32(base, kGameContext + 40);
  if (!Readable(manager, 128) || !InImage(Load32(base, manager))) return 0;
  return manager;
}

// The players in the manager's list (+84..+88), as team_balance.cpp reads them.
std::vector<uint32_t> ListedPlayers(const uint8_t* base, uint32_t manager) {
  std::vector<uint32_t> out;
  const uint32_t begin = Load32(base, manager + 84), end = Load32(base, manager + 88);
  if (begin == end || end < begin || end - begin > 64 * 4 || !Readable(begin, end - begin)) return out;
  for (uint32_t at = begin; at < end; at += 4) out.push_back(Load32(base, at));
  return out;
}

// An eastl string {begin, end, capacity end} with its text after it, in memory of ours (kept: the game copies it).
uint32_t GuestString(uint8_t* base, const std::string& text) {
  const uint32_t size = uint32_t(std::min<size_t>(text.size(), 60));
  const uint32_t s = Alloc(16 + 64);
  if (!s) return 0;
  std::memcpy(base + s + 16, text.data(), size);
  Store32(base, s, s + 16), Store32(base, s + 4, s + 16 + size), Store32(base, s + 8, s + 16 + size + 1);
  return s;
}

std::string GuestText(const uint8_t* base, uint32_t begin, uint32_t end) {
  if (end <= begin || end - begin > 96 || !Mapped(base, begin, end - begin)) return "?";
  return std::string(reinterpret_cast<const char*>(base + begin), end - begin);
}

// The vehicle a player sits in, and its seat: its controlled entity (+312) when that is not its soldier; the vehicle's
// name from its data object (+0x14, its name at +0xC: native_input.cpp, run kb_probe_g2), the seat the entry the
// player holds (+292: 0 the driver's). "" on foot.
std::string VehicleKey(const uint8_t* base, uint32_t player, uint32_t* vehicle_out = nullptr) {
  const uint32_t so = Load32(base, player + 288), ctl = Load32(base, player + 312);
  if (!ctl || ctl == so || !Readable(ctl, 0x20)) return {};
  const uint32_t data = Load32(base, ctl + 0x14);
  std::string name;
  if (Readable(data, 0x10)) {
    const uint32_t s = Load32(base, data + 0xC);
    for (uint32_t i = 0; i < 40 && Mapped(base, s + i, 1); ++i) {
      const char c = char(base[s + i]);
      if (!c) break;
      name += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    }
  }
  if (name.empty()) name = "vehicle";
  const uint32_t seat = Load32(base, player + 292);
  if (vehicle_out) *vehicle_out = ctl;
  return name + (seat == 0 ? "|driver" : "|seat" + std::to_string(seat < 16 ? seat : 15));
}

// A player's name: the eastl string at +0x18 (team_balance.cpp's NameOf).
std::string PlayerName(const uint8_t* base, uint32_t player) {
  return GuestText(base, Load32(base, player + 0x18), Load32(base, player + 0x1C));
}

// A new name for a bot of this team, none of the names taken.
std::string PickName(int team, const std::set<std::string>& taken) {
  static std::mt19937 rng{std::random_device{}()};
  for (int tries = 0; tries < 400; ++tries) {
    std::string name;
    if (team == 2) {
      name = std::string(kJpFamily[rng() % std::size(kJpFamily)]) + " " + kJpGiven[rng() % std::size(kJpGiven)];
    } else {
      name = std::string(kUsFirst[rng() % std::size(kUsFirst)]) + " " + kUsLast[rng() % std::size(kUsLast)];
    }
    if (name.size() <= 15 && !taken.count(name)) return name;
  }
  return std::string(team == 2 ? "Heitai " : "Private ") + std::to_string(taken.size() + 1);
}

bool Position(PPCContext& outer, uint8_t* base, uint32_t entity, float out[3]) {
  PPCContext call = outer;
  call.r3.u64 = entity;
  sub_822A7BB0(call, base);
  const uint32_t m = call.r3.u32;
  if (!Readable(m, 64)) return false;
  for (int i = 0; i < 3; ++i) out[i] = LoadF(base, m + 48 + 4 * i);
  return true;
}

// A vehicle's seats (ServerVehicleEntity): its entries at +144, their count from vtable +236 (sub_822F1F48 counts the
// taken ones so), each entry's player at +284 (0: free).
uint32_t SeatCount(PPCContext& outer, uint8_t* base, uint32_t vehicle) {
  const uint32_t vt = Load32(base, vehicle);
  if (!InImage(vt)) return 0;
  PPCContext call = outer;
  call.r3.u64 = vehicle;
  CallGuest(call, base, Load32(base, vt + 236));
  return std::min<uint32_t>(call.r3.u32, 16);
}
uint32_t SeatPlayer(const uint8_t* base, uint32_t vehicle, uint32_t seat) {
  const uint32_t entries = Load32(base, vehicle + 144);
  if (!Readable(entries + 4 * seat, 4)) return 0;
  const uint32_t entry = Load32(base, entries + 4 * seat);
  return Readable(entry, 288) ? Load32(base, entry + 284) : 0;
}
// A vehicle's name from its data (+0x14, the name at +0xC; VehicleKey's).
std::string VehicleName(const uint8_t* base, uint32_t vehicle) {
  const uint32_t data = Load32(base, vehicle + 0x14);
  std::string name;
  if (Readable(data, 0x10)) {
    const uint32_t s = Load32(base, data + 0xC);
    for (uint32_t i = 0; i < 40 && Readable(s + i, 1); ++i) {
      const char c = char(base[s + i]);
      if (!c) break;
      name += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    }
  }
  return name;
}

// Every committed guest range outside the image (modern_mode.cpp's walk).
template <typename Fn>
void ForEachGuestRange(const uint8_t* base, Fn&& fn) {
#if defined(_WIN32)
  for (uint64_t a = 0x00010000; a < 0x100000000ull;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) break;
    const uint64_t start = std::max<uint64_t>(uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base), a);
    const uint64_t end = std::min<uint64_t>(uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base) + mbi.RegionSize,
                                            0x100000000ull);
    const bool image = start >= 0x80000000ull && start < 0xA0000000ull;
    if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && !image && end > start)
      fn(start, end);
    if (end <= a) break;
    a = end;
  }
#endif
}

// The teams' kits, from the level's TeamData objects (a scan of guest memory: about half a second, once a level).
// TODO(bots): off the server thread, or from the level's loading.
void ScanTeams(const uint8_t* base) {
  g.team_kits.clear();
  const auto t0 = Clock::now();
  ForEachGuestRange(base, [&](uint64_t start, uint64_t end) {
    for (uint64_t a64 = start; a64 + 0x38 <= end; a64 += 4) {
      const uint32_t a = uint32_t(a64);
      if (Load32(base, a) != kTeamDataVtable) continue;
      const uint32_t team = Load32(base, a + 0x34);
      const std::string path = GuestText(base, Load32(base, a + 0x0C), Load32(base, a + 0x10));
      if (team < 1 || team > 2) continue;
      std::vector<uint32_t> kits;
      std::string names;
      const uint32_t list = Load32(base, a + 0x30);
      for (int k = 0; k < 3 && Mapped(base, list, 12); ++k) {
        const uint32_t kit = Load32(base, list + 4 * k);
        if (!Mapped(base, kit, 0x40) || Load32(base, kit) != kKitVtable) continue;
        kits.push_back(kit);
        names += " " + GuestText(base, Load32(base, kit + 0x0C), Load32(base, kit + 0x10));
      }
      REXLOG_INFO("BF bots: team {} ({}) kits:{}", team, path, names);
      g.team_kits[team] = kits;
    }
  });
  REXLOG_INFO("BF bots: kits found in {:.0f} ms", std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
}

// The level's capture points: their data objects, then the server's entities that point at them.
constexpr uint32_t kFlagDataVtable = 0x8205CF60;
void ScanFlags(const uint8_t* base) {
  g.flags.clear();
  std::set<uint32_t> datas;
  ForEachGuestRange(base, [&](uint64_t start, uint64_t end) {
    for (uint64_t a64 = start; a64 + 0x140 <= end; a64 += 4) {
      const uint32_t a = uint32_t(a64);
      if (Load32(base, a) != kFlagDataVtable) continue;
      Flag f;
      f.data = a;
      for (int i = 0; i < 3; ++i) f.pos[i] = LoadF(base, a + 0x60 + 4 * i);
      f.name = GuestText(base, Load32(base, a + 0x11C), Load32(base, a + 0x120));
      f.takeable = base[a + 0x135] == 0 && base[a + 0x12E] != 0;
      g.flags.push_back(f);
      datas.insert(a);
    }
  });
  if (datas.empty()) return;
  ForEachGuestRange(base, [&](uint64_t start, uint64_t end) {
    for (uint64_t a64 = start; a64 + 0x100 <= end; a64 += 4) {
      const uint32_t a = uint32_t(a64);
      const uint32_t d = Load32(base, a + 0x14);
      if (!datas.count(d) || !InImage(Load32(base, a))) continue;
      for (Flag& f : g.flags)
        if (f.data == d && !f.entity) f.entity = a;
    }
  });
  // The level's data is in memory more than once (run bots_s1a: three copies); the server's entities point at one.
  g.flags.erase(std::remove_if(g.flags.begin(), g.flags.end(), [](const Flag& f) { return !f.entity; }), g.flags.end());
  for (const Flag& f : g.flags) {
    std::string w;
    for (uint32_t o = 0x60; o < 0xA0 && f.entity; o += 4) w += fmt::format(" {:08X}", Load32(base, f.entity + o));
    REXLOG_INFO("BF bots: flag '{}' at {:.1f} {:.1f} {:.1f} {} entity {:08X} (vtable {:08X}) +60..:{}", f.name, f.pos[0],
                f.pos[1], f.pos[2], f.takeable ? "takeable" : "fixed", f.entity, f.entity ? Load32(base, f.entity) : 0, w);
  }
}

int FlagOwner(const uint8_t* base, const Flag& f) {
  if (!f.entity || !Readable(f.entity, 0x80)) return 0;
  const uint32_t team = Load32(base, f.entity + 0x80);
  if (team < 1 || team > 2 || !(LoadF(base, f.entity + 0xA0) >= 99.9f)) return 0;
  return int(team);
}

// Whether the bot stands in the flag's capture area: the server's list of who is in it (+0x88..+0x8C, 8 bytes each)
// names its player or its soldier.
bool InCaptureArea(const uint8_t* base, const Flag& f, uint32_t player, uint32_t soldier) {
  if (!f.entity || !Readable(f.entity, 0x90)) return false;
  const uint32_t b = Load32(base, f.entity + 0x88), e = Load32(base, f.entity + 0x8C);
  if (!b || e <= b || e - b > 64 * 8 || !Readable(b, e - b)) return false;
  for (uint32_t a = b; a < e; a += 4) {
    const uint32_t v = Load32(base, a);
    if (v == player || v == soldier) return true;
  }
  return false;
}

// The game's ray test against the level (the deep water test's, sub_8252FB10): the physics world of a soldier's
// character physics (soldier+304, its +104), its ray caster (vtable +144), cast (vtable +0: from, to, result, 2,
// filter {ignored physics, 0}); the result's fraction at +44 (1: nothing hit), the hit point at +0.
bool Ray(PPCContext& outer, uint8_t* base, uint32_t soldier, const float from[3], const float to[3], float* fraction,
         float hit[3] = nullptr) {
  *fraction = 1.0f;
  const uint32_t phys = Load32(base, soldier + 304);
  if (!Readable(phys, 0x120)) return false;
  const uint32_t world = Load32(base, phys + 104);
  if (!Readable(world, 16) || !InImage(Load32(base, world))) return false;
  if (!g.ray_scratch) g.ray_scratch = (Alloc(256 + 16) + 15) & ~15u;
  const uint32_t s = g.ray_scratch;
  if (!s) return false;
  std::memset(base + s, 0, 128);
  for (int i = 0; i < 3; ++i) StoreF(base, s + 4 * i, from[i]), StoreF(base, s + 16 + 4 * i, to[i]);
  Store32(base, s + 32 + 36, 0xFFFFFFFFu), Store32(base, s + 32 + 40, 0xFFFFFFFFu);
  StoreF(base, s + 32 + 44, 1.0f);
  Store32(base, s + 96, phys);
  PPCContext call = outer;
  call.r3.u64 = world;
  CallGuest(call, base, Load32(base, Load32(base, world) + 144));
  const uint32_t caster = call.r3.u32;
  if (!Readable(caster, 8) || !InImage(Load32(base, caster))) return false;
  call = outer;
  call.r3.u64 = caster, call.r4.u64 = s, call.r5.u64 = s + 16, call.r6.u64 = s + 32, call.r7.u64 = 2,
  call.r8.u64 = s + 96;
  const auto t0 = Clock::now();
  CallGuest(call, base, Load32(base, Load32(base, caster)));
  ++g.rays, g.ray_ms += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  const bool got = (call.r3.u32 & 0xFF) != 0;
  if (got) {
    *fraction = std::clamp(LoadF(base, s + 32 + 44), 0.0f, 1.0f);
    if (hit)
      for (int i = 0; i < 3; ++i) hit[i] = LoadF(base, s + 32 + 4 * i);
  }
  return got;
}

// Navigation: a grid of 2 m cells over the level, learned by ray tests as the bots need it and shared by all of
// them (no navigation data ships with the game: its AI's pathfinding is gone). A cell holds its ground height, found
// by a ray down near the height of the cell it was reached from (so slopes, steps and decks are followed); it is not
// walkable without ground, or with its ground under the water (the character physics' water height, physics+304).
// A step between neighbours is open when the heights differ by at most kMaxStep and a ray at knee and chest height
// between them hits nothing. A* over it, a budget of new rays per server tick so a long search spreads over ticks.
constexpr float kCell = 2.0f, kMaxStep = 1.4f;
constexpr int kRaysPerTick = 300, kExpandPerSearch = 4000, kScanRaysPerTick = 250;

struct NavCell {
  float y = 0;
  uint8_t state = 0;  // 0 not known, 1 walkable, 2 not walkable, 3 swimmable (water; y its surface)
};
struct Nav {
  std::unordered_map<uint64_t, NavCell> cells;
  std::unordered_map<uint64_t, uint8_t> steps;  // (cell, direction) -> 1 open, 2 closed
  std::unordered_map<uint64_t, Clock::time_point> closed_at;  // a closed step is tried again 20 s on
  int rays_left = 0;                            // this tick's budget
  // The level's scan (the user, 18 Nov 2026: "Pretty much all ground is walkable, just feed them the data on objects
  // you can't walk through ... and the water swimmable"): from the spawn points and flags outward over every step the
  // collision allows, a budget of rays a tick, until the whole reachable level is known; kept per map (bots/<map>.grid).
  std::deque<uint64_t> scan;
  std::unordered_set<uint64_t> scanned;
  bool scan_started = false, scan_done = false;
  float scan_min[2] = {1e9f, 1e9f}, scan_max[2] = {-1e9f, -1e9f};  // the area: the spawns' and flags', 120 m round
  Clock::time_point scan_began{};
  int scan_rays_left = 0;
  float water = -1e9f;
  uint64_t rays = 0;
  double ray_ms = 0;
} nav;

uint64_t CellKey(int ix, int iz) { return (uint64_t(uint32_t(ix)) << 32) | uint32_t(iz); }
int CellOf(float v) { return int(std::floor(v / kCell)); }
float CellCenter(int i) { return (float(i) + 0.5f) * kCell; }
constexpr int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, kDz[8] = {0, 0, 1, -1, 1, -1, 1, -1};
constexpr int kBack[8] = {1, 0, 3, 2, 7, 6, 5, 4};  // the opposite direction

// The cell's ground near the height y_ref: a ray from 1.6 m above (over the highest step, under a shelter's roof:
// from 3 m, run bots_s1e's spawns under a roof found the roof) down to 5 m below. False: out of this tick's budget.
bool ProbeCell(PPCContext& ctx, uint8_t* base, uint32_t soldier, int ix, int iz, float y_ref, NavCell* out) {
  auto it = nav.cells.find(CellKey(ix, iz));
  if (it != nav.cells.end()) {
    *out = it->second;
    return true;
  }
  if (nav.rays_left <= 0) return false;
  --nav.rays_left;
  const float cx = CellCenter(ix), cz = CellCenter(iz);
  const float from[3] = {cx, y_ref + 1.6f, cz}, to[3] = {cx, y_ref - 5.0f, cz};
  float frac = 1.0f, hit[3] = {};
  NavCell c;
  if (Ray(ctx, base, soldier, from, to, &frac, hit) && frac < 1.0f) {
    c.y = from[1] + (to[1] - from[1]) * frac;
    // Headroom: a ray up from the ground; a ceiling under 1.7 m (a wall's top, a table) is no place to stand.
    const float up_from[3] = {cx, c.y + 0.3f, cz}, up_to[3] = {cx, c.y + 1.8f, cz};
    float uf = 1.0f;
    const bool low = Ray(ctx, base, soldier, up_from, up_to, &uf) && uf < 1.0f;
    c.state = low ? 2 : 1;
    if (c.y < nav.water - 0.9f) c.state = 3, c.y = nav.water - 0.2f;  // under the water: a swimmer's, at its surface
  } else {
    c.state = to[1] < nav.water ? 3 : 2;  // no ground within reach: deep water, or a drop
    if (c.state == 3) c.y = nav.water - 0.2f;
  }
  nav.cells[CellKey(ix, iz)] = c;
  *out = c;
  return true;
}

// The water's height: the character physics' +304, which holds it only near the water (0 elsewhere, run bots_learn3:
// "water at 0.0" on Guadalcanal, whose sea is at 173). The highest seen counts; until one is seen, no cell is water
// (a sea's cells have no ground in reach and are not walkable: the scan stops at the shore).
void NoteWater(const uint8_t* base, uint32_t phys) {
  if (!Readable(phys, 0x140)) return;
  const float w = LoadF(base, phys + 304);
  if (std::isfinite(w) && w != 0.0f && w > nav.water) nav.water = w;
}

// The step from cell a (ground ya) to its neighbour in direction d. 0: out of budget, 1 open, 2 closed.
// The ray test hits soldiers too (its filter keeps out no character: run bots_s1g, every step out of a bot's own cell
// closed), so a step out of the searching bot's own cell (own) goes by the ground alone and is not kept, and a
// closed step is tried again 20 s on (a soldier who stood there has moved).
int Step(PPCContext& ctx, uint8_t* base, uint32_t soldier, int ix, int iz, float ya, int d, bool own = false) {
  const uint64_t key = (CellKey(ix, iz) << 3) ^ uint64_t(d);  // a cell's key shifted, its direction in the low bits
  auto it = nav.steps.find(key);
  if (it != nav.steps.end() && !own) {
    if (it->second == 1) return 1;
    auto c = nav.closed_at.find(key);
    if (c == nav.closed_at.end() || Clock::now() - c->second < std::chrono::seconds(20)) return 2;
  }
  NavCell b;
  if (!ProbeCell(ctx, base, soldier, ix + kDx[d], iz + kDz[d], ya, &b)) return 0;
  int r = 2;
  const float rise = b.y - ya, limit = d < 4 ? kMaxStep : kMaxStep * 1.3f;
  const bool passable = b.state == 1 || b.state == 3;
  if (own) return passable && std::fabs(rise) <= limit ? 1 : 2;
  if (passable && std::fabs(rise) <= limit) {
    if (nav.rays_left <= 0) return 0;
    --nav.rays_left;
    const float ax = CellCenter(ix), az = CellCenter(iz), bx = CellCenter(ix + kDx[d]), bz = CellCenter(iz + kDz[d]);
    const float h = std::max(ya, b.y) + 0.9f;  // knee to chest of the higher end
    const float from[3] = {ax, h, az}, to[3] = {bx, h, bz};
    float frac = 1.0f;
    r = Ray(ctx, base, soldier, from, to, &frac) && frac < 0.99f ? 2 : 1;
  }
  nav.steps[key] = uint8_t(r);
  if (r == 2) nav.closed_at[key] = Clock::now();
  return r;
}

float Dist2D(const float a[3], const float b[3]);
float Rand01();

// What the bots learn from the matches played (the user, 18 Nov 2026: "every time I do a test play against them, we
// updated what they've learned ... making them better after every match"), per map, kept in a file in the server's
// user data (bots/<map>.txt) and added to by every match:
//   - the ground soldiers walked (cells and the steps between them: walkable for sure, whatever the rays say), and how
//     much each step was used (a human's walk counts 1, a bot's 0.5): the routes players take;
//   - where kills were made from and where soldiers died, by side;
//   - where human players waited (stood 3 s and more with an enemy within 60 m): their ambush and watch spots;
//   - the steps human players walked in the minute before they took a flag: the ways in that worked, by flag.
// The bots use it: popular steps and a flag's ways in cost less in their route search, cells where their side died
// cost more; at a flag they hold the spots their side fought and waited from; at rest they watch where the enemy's
// kills came from.
struct Learned {
  std::string map;  // its file's name; empty until a level is known
  std::unordered_map<uint64_t, float> walk_y;     // cell -> the ground's height a soldier walked at
  std::unordered_map<uint64_t, float> step_use;   // step -> how much soldiers used it
  std::unordered_map<uint64_t, float> kills[2];   // by the killer's side (0 US, 1 Japan): cell -> kills made from it
  std::unordered_map<uint64_t, float> deaths[2];  // by the victim's side: cell -> deaths there
  std::unordered_map<uint64_t, float> waits[2];   // by side: cell -> seconds a human waited there near an enemy
  std::map<std::string, std::unordered_map<uint64_t, float>> ways_in;  // flag -> step -> use before a capture
  // The human players' vehicles (the user, 18 Nov 2026: "I'll be using vehicles on them, so that should give them a
  // good stepping stone"), by vehicle and seat ("Sherman|driver", "Sherman|seat1"): the time spent in it, where it went
  // (each cell's height on average, for a plane its flying height) and where it killed from, and how far.
  struct VehicleUse {
    double time = 0, kills = 0, kill_dist = 0;
    std::unordered_map<uint64_t, std::pair<float, float>> cells;  // cell -> (heights' sum, samples)
    std::unordered_map<uint64_t, float> kills_at;
  };
  std::map<std::string, VehicleUse> vehicles;
  bool dirty = false;
  Clock::time_point saved_at{};
} learn;

std::filesystem::path LearnedFile() {
  const std::filesystem::path root = REXCVAR_GET(user_data_root);
  if (root.empty() || learn.map.empty()) return {};
  std::string name;
  for (char c : learn.map) name += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
  return root / "bots" / (name + ".txt");
}

int CellX(uint64_t key) { return int(uint32_t(key >> 32)); }
int CellZ(uint64_t key) { return int(uint32_t(key)); }

void SaveStyle();
void SaveLearned() {
  const std::filesystem::path file = LearnedFile();
  if (file.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  const std::filesystem::path tmp = file.string() + ".new";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return;
    f << "BF1943 Reborn bots, what they learned on " << learn.map << " (1)\n";
    for (const auto& [k, y] : learn.walk_y) f << "c " << CellX(k) << ' ' << CellZ(k) << ' ' << y << '\n';
    for (const auto& [k, w] : learn.step_use)
      f << "s " << CellX(k >> 3) << ' ' << CellZ(k >> 3) << ' ' << (k & 7) << ' ' << w << '\n';
    const char* kinds[3] = {"k", "d", "w"};
    const std::unordered_map<uint64_t, float>* maps[3][2] = {
        {&learn.kills[0], &learn.kills[1]}, {&learn.deaths[0], &learn.deaths[1]}, {&learn.waits[0], &learn.waits[1]}};
    for (int kind = 0; kind < 3; ++kind)
      for (int side = 0; side < 2; ++side)
        for (const auto& [k, w] : *maps[kind][side])
          f << kinds[kind] << ' ' << side + 1 << ' ' << CellX(k) << ' ' << CellZ(k) << ' ' << w << '\n';
    for (const auto& [flag, steps] : learn.ways_in)
      for (const auto& [k, w] : steps)
        f << "a " << flag << ' ' << CellX(k >> 3) << ' ' << CellZ(k >> 3) << ' ' << (k & 7) << ' ' << w << '\n';
    for (const auto& [key, v] : learn.vehicles) {
      f << "vt " << key << ' ' << v.time << ' ' << v.kills << ' ' << v.kill_dist << '\n';
      for (const auto& [k, c] : v.cells)
        f << "vc " << key << ' ' << CellX(k) << ' ' << CellZ(k) << ' ' << c.first / c.second << ' ' << c.second << '\n';
      for (const auto& [k, w] : v.kills_at) f << "vk " << key << ' ' << CellX(k) << ' ' << CellZ(k) << ' ' << w << '\n';
    }
  }
  std::filesystem::rename(tmp, file, ec);
  learn.dirty = false, learn.saved_at = Clock::now();
  SaveStyle();
}

// Steps' keys hold the cell in their upper bits (the cell key shifted by 3, the direction in the low 3).
uint64_t StepKey(int ix, int iz, int d) { return (CellKey(ix, iz) << 3) ^ uint64_t(d); }

// What earlier matches learned on this map: into the store, and the walked ground into the grid.
void LoadLearned(const std::string& map) {
  learn = Learned{};
  learn.map = map;
  learn.saved_at = Clock::now();
  const std::filesystem::path file = LearnedFile();
  std::ifstream f(file);
  if (!f) {
    REXLOG_INFO("BF bots: nothing learned on {} yet ({})", map, file.string());
    return;
  }
  std::string line;
  std::getline(f, line);
  while (std::getline(f, line)) {
    std::istringstream in(line);
    std::string kind;
    in >> kind;
    int x = 0, z = 0, d = 0, side = 0;
    float v = 0;
    if (kind == "c" && (in >> x >> z >> v)) {
      learn.walk_y[CellKey(x, z)] = v;
    } else if (kind == "s" && (in >> x >> z >> d >> v) && d >= 0 && d < 8) {
      learn.step_use[StepKey(x, z, d)] = v;
    } else if ((kind == "k" || kind == "d" || kind == "w") && (in >> side >> x >> z >> v) && (side == 1 || side == 2)) {
      auto& m = kind == "k" ? learn.kills[side - 1] : kind == "d" ? learn.deaths[side - 1] : learn.waits[side - 1];
      m[CellKey(x, z)] = v;
    } else if (kind == "a") {
      std::string flag;
      if (in >> flag >> x >> z >> d >> v && d >= 0 && d < 8) learn.ways_in[flag][StepKey(x, z, d)] = v;
    } else if (kind == "vt" || kind == "vc" || kind == "vk") {
      std::string key;
      double a = 0, b = 0, c = 0;
      if (!(in >> key)) continue;
      auto& vu = learn.vehicles[key];
      if (kind == "vt" && (in >> a >> b >> c)) vu.time = a, vu.kills = b, vu.kill_dist = c;
      if (kind == "vc" && (in >> x >> z >> a >> b)) vu.cells[CellKey(x, z)] = {float(a * b), float(b)};
      if (kind == "vk" && (in >> x >> z >> a)) vu.kills_at[CellKey(x, z)] = float(a);
    }
  }
  for (const auto& [k, y] : learn.walk_y) nav.cells[k] = NavCell{y, 1};
  for (const auto& [k, w] : learn.step_use) nav.steps[k] = 1;
  float kills = 0, deaths = 0;
  for (int s = 0; s < 2; ++s) {
    for (const auto& [k, w] : learn.kills[s]) kills += w;
    for (const auto& [k, w] : learn.deaths[s]) deaths += w;
  }
  for (const auto& [key, vu] : learn.vehicles)
    REXLOG_INFO("BF bots: learned on {}: {} used {:.0f} s by the players, {} cells, {:.0f} kills at {:.0f} m", map, key,
                vu.time, vu.cells.size(), vu.kills, vu.kills > 0 ? vu.kill_dist / vu.kills : 0.0);
  REXLOG_INFO("BF bots: learned on {} so far: {} cells walked, {} steps, {:.0f} kills and {:.0f} deaths placed, {} wait "
              "spots, ways into {} flags",
              map, learn.walk_y.size(), learn.step_use.size(), kills, deaths,
              learn.waits[0].size() + learn.waits[1].size(), learn.ways_in.size());
}

float Learnt(const std::unordered_map<uint64_t, float>& m, uint64_t key) {
  const auto it = m.find(key);
  return it == m.end() ? 0.0f : it->second;
}

// A soldier walked from a to b (weight 1 a human, 0.5 a bot): the cells and steps crossed, walkable for sure.
void LearnWalk(const float a[3], const float b[3], float weight, std::deque<std::pair<Clock::time_point, uint64_t>>* trail) {
  const float dist = Dist2D(a, b);
  int px = CellOf(a[0]), pz = CellOf(a[2]);
  const int n = int(dist / 0.7f) + 1;
  const auto now = Clock::now();
  for (int k = 1; k <= n; ++k) {
    const float t = float(k) / float(n);
    const float x = a[0] + (b[0] - a[0]) * t, y = a[1] + (b[1] - a[1]) * t, z = a[2] + (b[2] - a[2]) * t;
    const int cx = CellOf(x), cz = CellOf(z);
    const uint64_t cell = CellKey(cx, cz);
    NavCell& c = nav.cells[cell];
    if (y < nav.water - 0.9f) {
      if (c.state != 3) c.y = nav.water - 0.2f, c.state = 3;
    } else if (c.state != 1) {
      c.y = y, c.state = 1;
    }
    if (!learn.walk_y.count(cell)) learn.walk_y[cell] = y, learn.dirty = true;
    if (cx == px && cz == pz) continue;
    for (int d = 0; d < 8; ++d) {
      if (kDx[d] != cx - px || kDz[d] != cz - pz) continue;
      const uint64_t there = StepKey(px, pz, d), back = StepKey(cx, cz, kBack[d]);
      nav.steps[there] = 1, nav.steps[back] = 1;
      learn.step_use[there] += weight, learn.dirty = true;
      if (trail) trail->push_back({now, there});
    }
    px = cx, pz = cz;
  }
}

// A step's cost factor from what was learned: soldiers' routes cheaper (down to about 0.6), a flag's ways in cheaper
// still for a bot going there, cells where the bot's side died dearer.
struct Recent {
  float weight = 0;
  Clock::time_point at{};
};
float RecentDeaths(int side, uint64_t cell);
const std::unordered_map<uint64_t, Recent>& RecentFire(int side);
float RecentWeight(const std::unordered_map<uint64_t, Recent>& m, uint64_t cell);
float LearnedCost(uint64_t step, uint64_t cell, int side, const std::unordered_map<uint64_t, float>* ways) {
  float f = 1.0f / (1.0f + 0.12f * std::log2(1.0f + Learnt(learn.step_use, step)));
  if (ways) f *= 1.0f / (1.0f + 0.25f * std::log2(1.0f + Learnt(*ways, step)));
  if (side == 1 || side == 2) {
    f *= 1.0f + 0.35f * std::log2(1.0f + Learnt(learn.deaths[side - 1], cell));
    f *= 1.0f + 0.8f * RecentDeaths(side, cell);  // where its side died in the last minutes (this match)
  }
  return f;
}

// A spot near a flag its side fought or waited from (weighted by how much), or false when none is known.
bool LearnedSpot(const float flag[3], int side, float radius, float out[3]) {
  if (side != 1 && side != 2) return false;
  std::vector<std::pair<uint64_t, float>> spots;
  float total = 0;
  for (const auto* m : {&learn.kills[side - 1], &learn.waits[side - 1]})
    for (const auto& [k, w] : *m) {
      const float c[3] = {CellCenter(CellX(k)), 0, CellCenter(CellZ(k))};
      if (Dist2D(c, flag) > radius || w < 0.5f) continue;
      spots.push_back({k, w}), total += w;
    }
  if (spots.empty()) return false;
  float r = Rand01() * total;
  for (const auto& [k, w] : spots) {
    if ((r -= w) > 0) continue;
    out[0] = CellCenter(CellX(k)), out[2] = CellCenter(CellZ(k));
    const auto y = learn.walk_y.find(k);
    out[1] = y != learn.walk_y.end() ? y->second : flag[1];
    return true;
  }
  return false;
}

// Where the enemy's kills came from near a place (weighted pick): where a bot at rest there keeps watch.
bool LearnedThreat(const float at[3], int side, float radius, float out[3]) {
  if (side != 1 && side != 2) return false;
  const auto& m = learn.kills[2 - side];  // the other side's
  std::vector<std::pair<uint64_t, float>> spots;
  float total = 0;
  std::unordered_map<uint64_t, float> all(m.begin(), m.end());
  for (const auto& [k, r] : RecentFire(3 - side)) all[k] += 5.0f * RecentWeight(RecentFire(3 - side), k);
  for (const auto& [k, w] : all) {
    const float c[3] = {CellCenter(CellX(k)), 0, CellCenter(CellZ(k))};
    const float d = Dist2D(c, at);
    if (d > radius || d < 8.0f) continue;
    spots.push_back({k, w}), total += w;
  }
  if (spots.empty()) return false;
  float r = Rand01() * total;
  for (const auto& [k, w] : spots) {
    if ((r -= w) > 0) continue;
    out[0] = CellCenter(CellX(k)), out[1] = at[1], out[2] = CellCenter(CellZ(k));
    return true;
  }
  return false;
}

// The human players' style (the user, 18 Nov 2026: "We still need my play data so they learn from a player, and adapt
// that playstyle into the next match"; "Could bots learn on-the-fly live in a match?"): measured from every human's
// input and events while it plays, kept in the server's user data (bots/style.txt, all maps) and blended into the
// match as it is played. Older matches count less (their sums scaled by 0.7 at each load). The bots take it with a
// variety of their own (Bot::jitter), so they play the way the humans do without all playing alike.
struct Style {
  double time = 0;                      // seconds on foot, measured
  double sprint = 0, still = 0;         // seconds sprinting (faster than 5.5 m/s), standing still
  double fire = 0, ads = 0;             // seconds firing; of them, seconds aiming down the sights (LT, concept 11)
  double bursts = 0, burst_time = 0;    // bursts of fire and their total length
  double kills = 0, kill_dist = 0;      // kills and their total distance
  double flank_kills = 0;               // kills from the victim's side or back (more than 100 degrees off its facing)
  double attack = 0, defend = 0;        // seconds near a flag the side does not hold / holds
  double kit[3] = {0, 0, 0};            // spawns with each kit (the level's order: Support, Assault, Specialist)
} style_long, style_match;

// A trait from the long term and this match together, with a prior (what a player does before anything is known)
// worth `prior_weight` of the measure's own unit.
double StyleRatio(double Style::*num, double Style::*den, double prior, double prior_weight) {
  const double n = style_long.*num + style_match.*num, d = style_long.*den + style_match.*den;
  return (n + prior * prior_weight) / (d + prior_weight);
}
double SprintShare() { return StyleRatio(&Style::sprint, &Style::time, 0.45, 120); }
double StillShare() { return StyleRatio(&Style::still, &Style::time, 0.2, 120); }
double AdsShare() { return StyleRatio(&Style::ads, &Style::fire, 0.5, 20); }
double BurstLength() { return StyleRatio(&Style::burst_time, &Style::bursts, 0.4, 10); }
double KillDistance() { return StyleRatio(&Style::kill_dist, &Style::kills, 35.0, 3); }
double FlankShare() { return StyleRatio(&Style::flank_kills, &Style::kills, 0.2, 5); }
double AttackShare() {
  const double a = style_long.attack + style_match.attack, d = style_long.defend + style_match.defend;
  return (a + 0.65 * 120) / (a + d + 120);
}
int StyleKit() {
  double w[3], total = 0;
  for (int k = 0; k < 3; ++k) total += w[k] = style_long.kit[k] + style_match.kit[k] + 1.0;  // never none
  double r = Rand01() * total;
  for (int k = 0; k < 3; ++k)
    if ((r -= w[k]) <= 0) return k;
  return 2;
}

std::filesystem::path StyleFile() {
  const std::filesystem::path root = REXCVAR_GET(user_data_root);
  return root.empty() ? std::filesystem::path() : root / "bots" / "style.txt";
}

// The fields by name, for the file.
const std::pair<const char*, double Style::*> kStyleFields[] = {
    {"time", &Style::time},     {"sprint", &Style::sprint},       {"still", &Style::still},
    {"fire", &Style::fire},     {"ads", &Style::ads},             {"bursts", &Style::bursts},
    {"burst_time", &Style::burst_time}, {"kills", &Style::kills}, {"kill_dist", &Style::kill_dist},
    {"flank_kills", &Style::flank_kills}, {"attack", &Style::attack}, {"defend", &Style::defend}};

void SaveStyle() {
  const std::filesystem::path file = StyleFile();
  if (file.empty() || style_match.time < 5) return;
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  std::ofstream f(file.string() + ".new", std::ios::trunc);
  if (!f) return;
  f << "BF1943 Reborn bots, the human players' style (1)\n";
  for (const auto& [name, field] : kStyleFields) f << name << ' ' << style_long.*field + style_match.*field << '\n';
  for (int k = 0; k < 3; ++k) f << "kit" << k << ' ' << style_long.kit[k] + style_match.kit[k] << '\n';
  f.close();
  std::filesystem::rename(file.string() + ".new", file, ec);
}

void LoadStyle() {
  style_long = Style{}, style_match = Style{};
  std::ifstream f(StyleFile());
  if (!f) return;
  std::string line;
  std::getline(f, line);
  while (std::getline(f, line)) {
    std::istringstream in(line);
    std::string name;
    double v = 0;
    if (!(in >> name >> v) || !std::isfinite(v)) continue;
    v *= 0.7;  // an older match counts less
    for (const auto& [n, field] : kStyleFields)
      if (name == n) style_long.*field = v;
    for (int k = 0; k < 3; ++k)
      if (name == "kit" + std::to_string(k)) style_long.kit[k] = v;
  }
  REXLOG_INFO("BF bots: the players' style so far ({:.0f} min measured): sprint {:.0f}%, still {:.0f}%, sights {:.0f}% of "
              "fire, bursts {:.2f} s, kills at {:.0f} m, {:.0f}% from the side or back, attacking {:.0f}% of the time",
              style_long.time / 60, SprintShare() * 100, StillShare() * 100, AdsShare() * 100, BurstLength(),
              KillDistance(), FlankShare() * 100, AttackShare() * 100);
}

// The match's own memory (on the fly): deaths and the enemy's firing spots of the last minutes, fading with a time of
// 120 s. A bot's routes keep away from where its side died lately; at a flag it watches where it was shot from.
std::unordered_map<uint64_t, Recent> recent_deaths[2], recent_fire[2];  // by the victim's side / the killer's side
float RecentWeight(const std::unordered_map<uint64_t, Recent>& m, uint64_t cell);
const std::unordered_map<uint64_t, Recent>& RecentFire(int side) { return recent_fire[std::clamp(side, 1, 2) - 1]; }
float RecentDeaths(int side, uint64_t cell) { return side == 1 || side == 2 ? RecentWeight(recent_deaths[side - 1], cell) : 0.0f; }
float RecentWeight(const std::unordered_map<uint64_t, Recent>& m, uint64_t cell) {
  const auto it = m.find(cell);
  if (it == m.end()) return 0;
  return it->second.weight * float(std::exp(-std::chrono::duration<double>(Clock::now() - it->second.at).count() / 120.0));
}
void NoteRecent(std::unordered_map<uint64_t, Recent>& m, uint64_t cell, float w) {
  // The cell and its eight neighbours: a death marks the ground about it.
  for (int d = -1; d < 8; ++d) {
    const uint64_t k = d < 0 ? cell : CellKey(CellX(cell) + kDx[d], CellZ(cell) + kDz[d]);
    const float before = RecentWeight(m, k);
    Recent& r = m[k];
    r.weight = before + (d < 0 ? w : w * 0.5f);
    r.at = Clock::now();
  }
}

// The level's grid, as scanned, in a binary file beside what was learned (bots/<map>.grid): its cells, each with its
// ground, its state and its steps (8 bits open, 8 bits closed), and whether the scan was finished.
std::filesystem::path GridFile() {
  std::filesystem::path f = LearnedFile();
  if (!f.empty()) f.replace_extension(".grid");
  return f;
}

void SaveGrid() {
  const std::filesystem::path file = GridFile();
  if (file.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  const std::filesystem::path tmp = file.string() + ".new";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return;
    const uint32_t magic = 0x42475231, done = nav.scan_done ? 1u : 0u;  // "BGR1"
    const uint64_t count = nav.cells.size();
    f.write(reinterpret_cast<const char*>(&magic), 4);
    f.write(reinterpret_cast<const char*>(&done), 4);
    f.write(reinterpret_cast<const char*>(&count), 8);
    for (const auto& [k, c] : nav.cells) {
      uint8_t open = 0, closed = 0;
      for (int d = 0; d < 8; ++d) {
        const auto st = nav.steps.find(StepKey(CellX(k), CellZ(k), d));
        if (st == nav.steps.end()) continue;
        if (st->second == 1) open |= uint8_t(1 << d);
        else if (st->second == 2) closed |= uint8_t(1 << d);
      }
      const int32_t x = CellX(k), z = CellZ(k);
      f.write(reinterpret_cast<const char*>(&x), 4);
      f.write(reinterpret_cast<const char*>(&z), 4);
      f.write(reinterpret_cast<const char*>(&c.y), 4);
      f.write(reinterpret_cast<const char*>(&c.state), 1);
      f.write(reinterpret_cast<const char*>(&open), 1);
      f.write(reinterpret_cast<const char*>(&closed), 1);
    }
  }
  std::filesystem::rename(tmp, file, ec);
}

void LoadGrid() {
  std::ifstream f(GridFile(), std::ios::binary);
  if (!f) return;
  uint32_t magic = 0, done = 0;
  uint64_t count = 0;
  f.read(reinterpret_cast<char*>(&magic), 4);
  f.read(reinterpret_cast<char*>(&done), 4);
  f.read(reinterpret_cast<char*>(&count), 8);
  if (!f || magic != 0x42475231 || count > 20000000) return;
  for (uint64_t i = 0; i < count && f; ++i) {
    int32_t x = 0, z = 0;
    NavCell c;
    uint8_t open = 0, closed = 0;
    f.read(reinterpret_cast<char*>(&x), 4);
    f.read(reinterpret_cast<char*>(&z), 4);
    f.read(reinterpret_cast<char*>(&c.y), 4);
    f.read(reinterpret_cast<char*>(&c.state), 1);
    f.read(reinterpret_cast<char*>(&open), 1);
    f.read(reinterpret_cast<char*>(&closed), 1);
    const uint64_t k = CellKey(x, z);
    if (!nav.cells.count(k)) nav.cells[k] = c;  // what was walked (already in) stands over what was scanned
    nav.scanned.insert(k);
    for (int d = 0; d < 8; ++d) {
      const uint64_t sk = StepKey(x, z, d);
      if (open & (1 << d)) nav.steps[sk] = 1;
      else if ((closed & (1 << d)) && !nav.steps.count(sk)) nav.steps[sk] = 2;
    }
  }
  nav.scan_done = done != 0;
  REXLOG_INFO("BF bots: the level's grid loaded: {} cells{}", count, nav.scan_done ? " (the whole level)" : " (a scan in part)");
}

// Starts the scan from these places (each a cell and its ground's height).
void ScanFrom(const float at[3]) {
  nav.scan_min[0] = std::min(nav.scan_min[0], at[0] - 120.0f), nav.scan_max[0] = std::max(nav.scan_max[0], at[0] + 120.0f);
  nav.scan_min[1] = std::min(nav.scan_min[1], at[2] - 120.0f), nav.scan_max[1] = std::max(nav.scan_max[1], at[2] + 120.0f);
  const int x = CellOf(at[0]), z = CellOf(at[2]);
  const uint64_t k = CellKey(x, z);
  if (!nav.cells.count(k)) nav.cells[k] = NavCell{at[1], 1};
  if (nav.scanned.insert(k).second) nav.scan.push_back(k);
}

// One tick of the scan: cells taken from its queue, their 8 steps found (rays within its own budget), the new cells
// reached queued. False when there was nothing to do.
bool ScanTick(PPCContext& ctx, uint8_t* base, uint32_t soldier) {
  if (nav.scan_done || nav.scan.empty() || !soldier) return false;
  nav.scan_rays_left = kScanRaysPerTick;
  while (!nav.scan.empty()) {
    const uint64_t k = nav.scan.front();
    const auto c = nav.cells.find(k);
    if (c == nav.cells.end() || (c->second.state != 1 && c->second.state != 3)) {
      nav.scan.pop_front();
      continue;
    }
    // Step() spends the path budget: the scan lends it its own for the call.
    const int keep = nav.rays_left;
    nav.rays_left = nav.scan_rays_left;
    bool starved = false;
    for (int d = 0; d < 8; ++d) {
      const int st = Step(ctx, base, soldier, CellX(k), CellZ(k), c->second.y, d);
      if (st == 0) {
        starved = true;
        break;
      }
      const uint64_t nk = CellKey(CellX(k) + kDx[d], CellZ(k) + kDz[d]);
      const float nx = CellCenter(CellX(nk)), nz = CellCenter(CellZ(nk));
      const bool inside = nx >= nav.scan_min[0] && nx <= nav.scan_max[0] && nz >= nav.scan_min[1] && nz <= nav.scan_max[1];
      if (st == 1 && inside && nav.scanned.insert(nk).second) nav.scan.push_back(nk);
    }
    nav.scan_rays_left = nav.rays_left;
    nav.rays_left = keep;
    if (starved) break;  // this cell again next tick, its steps found so far kept
    nav.scan.pop_front();
  }
  if (nav.scan.empty()) {
    nav.scan_done = true;
    REXLOG_INFO("BF bots: the level is mapped: {} cells, {} steps, in {:.0f} s", nav.cells.size(), nav.steps.size(),
                std::chrono::duration<double>(Clock::now() - nav.scan_began).count());
    SaveGrid();
  }
  return true;
}

// A* from pos toward goal over the grid. The path (cell centres at their ground heights, the first step first) to
// the goal, or, when the search ran out of budget or found no way, to the reached cell nearest it.
std::vector<std::array<float, 3>> FindPath(PPCContext& ctx, uint8_t* base, uint32_t soldier, const float pos[3],
                                           const float goal[3], bool* complete, bool* starved_out = nullptr,
                                           int side = 0, const std::unordered_map<uint64_t, float>* ways = nullptr) {
  struct Node {
    float g, f;
    uint64_t parent;
    int ix, iz;
    float y;
  };
  const int sx = CellOf(pos[0]), sz = CellOf(pos[2]), gx = CellOf(goal[0]), gz = CellOf(goal[2]);
  auto h = [&](int ix, int iz) {
    const float dx = float(std::abs(ix - gx)), dz = float(std::abs(iz - gz));
    return kCell * (std::max(dx, dz) + 0.414f * std::min(dx, dz));
  };
  std::unordered_map<uint64_t, Node> nodes;
  using Open = std::pair<float, uint64_t>;
  std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;
  const uint64_t start = CellKey(sx, sz);
  nodes[start] = Node{0, h(sx, sz), start, sx, sz, pos[1]};
  open.push({nodes[start].f, start});
  std::unordered_set<uint64_t> closed;
  uint64_t best = start;
  float best_h = h(sx, sz);
  bool found = false, starved = false;
  int expanded = 0;
  while (!open.empty() && expanded < kExpandPerSearch) {
    const uint64_t k = open.top().second;
    open.pop();
    if (!closed.insert(k).second) continue;
    const Node n = nodes[k];
    ++expanded;
    const float hn = h(n.ix, n.iz);
    if (hn < best_h) best_h = hn, best = k;
    if (n.ix == gx && n.iz == gz) {
      found = true, best = k;
      break;
    }
    for (int d = 0; d < 8; ++d) {
      const int nx = n.ix + kDx[d], nz = n.iz + kDz[d];
      const uint64_t nk = CellKey(nx, nz);
      if (closed.count(nk)) continue;
      const int st = Step(ctx, base, soldier, n.ix, n.iz, n.y, d, k == start);
      if (st == 0) {
        starved = true;
        continue;
      }
      if (st != 1) continue;
      const NavCell& c = nav.cells[nk];
      const float swim = c.state == 3 ? 3.0f : 1.0f;  // swimming: slow, and no shooting
      const float cost = ((d < 4 ? kCell : kCell * 1.414f) + std::max(0.0f, c.y - n.y) * 1.5f) *
                         LearnedCost(StepKey(n.ix, n.iz, d), nk, side, ways) * swim;
      const float g2 = n.g + cost;
      auto it = nodes.find(nk);
      if (it != nodes.end() && it->second.g <= g2) continue;
      nodes[nk] = Node{g2, g2 + h(nx, nz), k, nx, nz, c.y};
      open.push({g2 + h(nx, nz), nk});
    }
    if (starved) break;
  }
  *complete = found;
  if (starved_out) *starved_out = starved;
  std::vector<std::array<float, 3>> path;
  for (uint64_t k = best; k != start;) {
    const Node& n = nodes[k];
    path.push_back({CellCenter(n.ix), n.y, CellCenter(n.iz)});
    k = n.parent;
    if (path.size() > 4000) break;
  }
  std::reverse(path.begin(), path.end());
  return path;
}

// The bot's kit for its team now (a bot the round's start moved to the other team gets that team's).
void ChooseKit(uint8_t* base, Bot& bot) {
  const auto it = g.team_kits.find(Load32(base, bot.player + 48));
  if (it == g.team_kits.end() || it->second.empty()) return;
  if (bot.kit_choice < 0) bot.kit_choice = StyleKit();
  const uint32_t kit = it->second[size_t(bot.kit_choice) % it->second.size()];
  Store32(base, bot.player + 500, Load32(base, kit + 0x3C));
}

bool MakeBot(PPCContext& outer, uint8_t* base, uint32_t manager, int index, int team,
             const std::vector<uint32_t>& listed) {
  Bot bot;
  bot.index = index;
  bot.team = team;
  std::set<std::string> taken;
  for (uint32_t p : listed)
    if (Readable(p, 0x20)) taken.insert(PlayerName(base, p));
  for (const Bot& b : g.bots) taken.insert(b.name);
  bot.name = PickName(bot.team, taken);
  for (float& j : bot.jitter) j = 0.75f + 0.5f * Rand01();
  PPCContext call = outer;
  call.r3.u64 = manager, call.r4.u64 = 0;
  sub_82387EF0(call, base);
  bot.player = call.r3.u32;
  if (!bot.player || !Readable(bot.player, 640) || Load32(base, bot.player) != kServerPlayerVtable) {
    REXLOG_INFO("BF bots: the player manager made no player for {} (full?)", bot.name);
    return false;
  }
  const uint32_t name = GuestString(base, bot.name);
  call = outer;
  call.r3.u64 = manager, call.r4.u64 = bot.player, call.r5.u64 = name;
  sub_8238EFD8(call, base);
  // Its name where a connection puts a human's too (+0x18, the online name): the clients' name tags over soldiers
  // read that one, and showed the game's placeholder ">>><<<" for a bot (the user, 19 Nov 2026, run names10: the
  // client's player held the name at +8 and ">>><<<" at +0x18).
  if (name) {
    call = outer;
    call.r3.u64 = bot.player + 0x18, call.r4.u64 = name + 16;
    sub_821D51C8(call, base);
  }
  // Its own input, as a connection gives its player one.
  bot.input = Alloc(360 + 16);
  bot.state = Alloc(176 + 16);
  call = outer;
  call.r3.u64 = bot.input;
  sub_822C1478(call, base);
  call = outer;
  call.r3.u64 = bot.state;
  sub_822BD4A8(call, base);
  Store32(base, bot.player + 280, bot.input);
  // Its team, through the game's own pick (a squad, the team's cap).
  call = outer;
  call.r3.u64 = bot.player, call.r4.u64 = uint32_t(bot.team), call.r5.u64 = 1, call.r6.u64 = 1;
  sub_82399AE8(call, base);
  ChooseKit(base, bot);
  // Its score, as a connection's player gets one when it joins (sub_823AEF90: sub_8240DFE0(the game context's scoring
  // at +68, player)): the scoring's record for its id and the SvPlayerScore the clients' scoreboards list (the user,
  // 19 Nov 2026: "none of the bots show up on the in-match scoreboard").
  if (const uint32_t scoring = Load32(base, kGameContext + 68); Readable(scoring, 16)) {
    call = outer;
    call.r3.u64 = scoring, call.r4.u64 = bot.player;
    sub_8240DFE0(call, base);
  }
  REXLOG_INFO("BF bots: {} joins: team {}, squad {}, kit {:08X}", bot.name, Load32(base, bot.player + 48),
              Load32(base, bot.player + 52), Load32(base, bot.player + 500));
  bot.died = Clock::now() - std::chrono::seconds(10);
  g.bots.push_back(bot);
  return true;
}

// A spawn point of the bot's team: the game's ServerSoldierSpawnEntity, team at +52.
uint32_t SpawnFor(const uint8_t* base, int team, int nth) {
  std::vector<uint32_t> ours;
  {
    std::lock_guard lock(g.mutex);
    for (uint32_t e : g.spawns)
      if (!g.vehicle_spawns.count(e) && Readable(e, 0x100) && Load32(base, e) == kSoldierSpawnVtable &&
          int(Load32(base, e + 52)) == team &&
          Readable(Load32(base, e + 20), 0x110))
        ours.push_back(e);
  }
  if (ours.empty()) return 0;
  return ours[size_t(nth) % ours.size()];
}

// A guest function by its address (a virtual method), as the game's own bctrl calls it.
void CallGuest(PPCContext& ctx, uint8_t* base, uint32_t target) { REX_CALL_INDIRECT_FUNC(target); }

constexpr float kPi = 3.14159265f;
float WrapAngle(float a) { return std::remainder(a, 2 * kPi); }
// The yaw that faces from a to b (forward = (-sin yaw, 0, cos yaw), run bots_s0k) and the pitch (up positive).
float YawTo(const float a[3], const float b[3]) { return std::atan2(-(b[0] - a[0]), b[2] - a[2]); }
float PitchTo(const float a[3], const float b[3]) {
  const float dx = b[0] - a[0], dz = b[2] - a[2];
  return std::atan2(b[1] - a[1], std::sqrt(dx * dx + dz * dz));
}
float Dist2D(const float a[3], const float b[3]) {
  const float dx = b[0] - a[0], dz = b[2] - a[2];
  return std::sqrt(dx * dx + dz * dz);
}
float Rand01() {
  static std::mt19937 rng{std::random_device{}()};
  return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
}

// Step 1's settings (difficulty comes in its own step): how far a bot sees, its field of view, aim error, turn rate.
constexpr float kAtFlag = 12.0f;
constexpr float kSightRange = 90.0f, kFov = 0.75f * kPi, kAimError = 0.035f, kTurnRate = 3.5f;

struct Enemy {
  uint32_t soldier;
  float pos[3];
  float dist;
};

// What the bot decides, 4 times a second: an enemy it sees to fight, else the flag to go for, and how to get there
// past what is in the way (rays ahead at waist and chest height, one down ahead for a drop).
void Think(PPCContext& ctx, uint8_t* base, Bot& bot, uint32_t soldier, const float pos[3],
           const std::vector<uint32_t>& listed, Clock::time_point now) {
  const int team = int(Load32(base, bot.player + 48));
  const float eye[3] = {pos[0], pos[1] + 1.6f, pos[2]};
  // Enemies: every other team's living soldier in range, nearest first.
  std::vector<Enemy> enemies;
  for (uint32_t p : listed) {
    if (p == bot.player || !Readable(p, 0x140)) continue;
    const int t = int(Load32(base, p + 48));
    if (t == team || (t != 1 && t != 2)) continue;
    const uint32_t so = Load32(base, p + 288);
    if (!so || !Readable(so, 0x140)) continue;
    Enemy e{so, {}, 0};
    if (!Position(ctx, base, so, e.pos)) continue;
    e.dist = Dist2D(pos, e.pos);
    if (e.dist < kSightRange) enemies.push_back(e);
  }
  std::sort(enemies.begin(), enemies.end(), [](const Enemy& x, const Enemy& y) { return x.dist < y.dist; });
  // Sight: within the field of view (or close enough to hear), and nothing of the level between the eyes and the
  // enemy's chest (the ray stops short of the enemy so its own body does not count).
  const Enemy* seen = nullptr;
  int rays = 0;
  for (const Enemy& e : enemies) {
    if (rays >= 3) break;
    const float off = std::fabs(WrapAngle(YawTo(pos, e.pos) - bot.yaw));
    if (off > kFov / 2 && e.dist > 12.0f && e.soldier != bot.enemy) continue;
    // Head, chest and hips: any one in sight is enough (a single ray lost the enemy behind every leaf, run bots_s1i).
    bool visible = false;
    for (float hgt : {1.2f, 1.6f, 0.8f}) {
      const float aim[3] = {e.pos[0], e.pos[1] + hgt, e.pos[2]};
      const float d[3] = {aim[0] - eye[0], aim[1] - eye[1], aim[2] - eye[2]};
      const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      if (len < 0.5f) {
        visible = true;
        break;
      }
      const float k = std::max(0.0f, (len - 0.8f) / len);
      const float to[3] = {eye[0] + d[0] * k, eye[1] + d[1] * k, eye[2] + d[2] * k};
      float frac = 1.0f;
      if (!Ray(ctx, base, soldier, eye, to, &frac) || frac >= 0.98f) {
        visible = true;
        break;
      }
    }
    ++rays;
    if (!visible) continue;
    seen = &e;
    break;
  }
  // An enemy lost from sight is still fought for 2.5 s: aimed at where it was last seen, fired at for half a second.
  Enemy remembered{};
  if (!seen && bot.enemy && now - bot.enemy_seen_at < std::chrono::milliseconds(2500)) {
    remembered.soldier = bot.enemy;
    std::memcpy(remembered.pos, bot.enemy_pos, sizeof(remembered.pos));
    remembered.dist = Dist2D(pos, bot.enemy_pos);
  }
  const bool fresh = seen != nullptr;
  if (!seen && remembered.soldier) seen = &remembered;
  if (seen) {
    if (fresh) bot.enemy_seen_at = now, std::memcpy(bot.enemy_pos, seen->pos, sizeof(bot.enemy_pos));
    if (seen->soldier != bot.enemy) {
      bot.enemy = seen->soldier;
      bot.aim_error_yaw = (Rand01() - 0.5f) * 4 * kAimError, bot.aim_error_pitch = (Rand01() - 0.5f) * 2 * kAimError;
      REXLOG_INFO("BF bots: {} sees an enemy {:.0f} m away", bot.name, seen->dist);
    }
    const float chest[3] = {seen->pos[0], seen->pos[1] + 1.2f, seen->pos[2]};
    // The aim settles on the target: the error shrinks while it keeps the enemy in sight.
    bot.aim_error_yaw *= 0.7f, bot.aim_error_pitch *= 0.7f;
    bot.want_yaw = YawTo(eye, chest) + bot.aim_error_yaw + (Rand01() - 0.5f) * kAimError;
    bot.want_pitch = PitchTo(eye, chest) + bot.aim_error_pitch + (Rand01() - 0.5f) * kAimError * 0.5f;
    // The range the players kill at (its own take on it): closer in when further, back off when much nearer;
    // side-steps as often as the players move rather than stand.
    const float range = std::clamp(float(KillDistance()) * bot.jitter[1], 8.0f, 150.0f);
    bot.forward = seen->dist > range * 1.25f ? 0.7f : seen->dist < range * 0.55f && range > 20.0f ? -0.5f : 0.0f;
    if (now > bot.detour_until) {
      const bool move = Rand01() > StillShare();
      bot.strafe = move ? (Rand01() < 0.5f ? -0.7f : 0.7f) : 0.0f;
      bot.detour_until = now + std::chrono::milliseconds(int(600 + Rand01() * 900));
    }
    if (fresh && seen->soldier != bot.enemy) bot.ads = Rand01() < AdsShare() * bot.jitter[3];
    const bool on_target = std::fabs(WrapAngle(bot.want_yaw - bot.yaw)) < 0.08f;
    // Bursts: 0.25 to 0.6 s of fire, then a pause.
    if (on_target && now > bot.burst_pause_until && now > bot.burst_until) {
      const double burst = std::clamp(BurstLength() * bot.jitter[2] * (0.6 + 0.8 * Rand01()), 0.12, 2.0);
      bot.burst_until = now + std::chrono::milliseconds(int(burst * 1000));
      bot.burst_pause_until = bot.burst_until + std::chrono::milliseconds(int(200 + Rand01() * 400));
    }
    bot.fire = now < bot.burst_until && now - bot.enemy_seen_at < std::chrono::milliseconds(500);
    bot.sprint = false;
    bot.path.clear();
    return;
  }
  if (bot.enemy) REXLOG_INFO("BF bots: {} lost sight of its enemy", bot.name);
  bot.enemy = 0, bot.fire = false, bot.strafe = 0, bot.ads = false;
  bot.want_pitch = 0;
  // The flag: the nearest one its team does not hold; with all held, the nearest one (to hold it). A bot keeps its
  // flag until its team holds it (no turning back and forth as others come and go), or 60 s at most.
  int best = -1;
  float best_d = 1e9f;
  const bool keep = bot.goal >= 0 && bot.goal < int(g.flags.size()) && now < bot.goal_until &&
                    FlagOwner(base, g.flags[bot.goal]) != team;
  if (keep) {
    best = bot.goal, best_d = Dist2D(pos, g.flags[best].pos);
  } else if (bot.goal_until < now && Rand01() > AttackShare() * bot.jitter[4]) {
    // Defending, as the players do part of the time: its side's nearest held flag.
    for (size_t i = 0; i < g.flags.size(); ++i) {
      const Flag& f = g.flags[i];
      if (!f.takeable || FlagOwner(base, f) != team) continue;
      const float d = Dist2D(pos, f.pos);
      if (d < best_d) best_d = d, best = int(i);
    }
  }
  if (best < 0) {
    for (int pass = 0; pass < 2 && best < 0; ++pass)
      for (size_t i = 0; i < g.flags.size(); ++i) {
        const Flag& f = g.flags[i];
        if (!f.takeable) continue;
        if (pass == 0 && FlagOwner(base, f) == team) continue;
        const float d = Dist2D(pos, f.pos);
        if (d < best_d) best_d = d, best = int(i);
      }
  }
  if (best != bot.goal && best >= 0) {
    bot.goal = best;
    bot.goal_until = now + std::chrono::seconds(60);
    bot.defending = FlagOwner(base, g.flags[best]) == team;
    bot.sprinter = Rand01() < SprintShare() * 1.6 * bot.jitter[0];
    std::memcpy(bot.goal_pos, g.flags[best].pos, sizeof(bot.goal_pos));
    REXLOG_INFO("BF bots: {} goes for flag '{}' ({:.0f} m, held by {})", bot.name, g.flags[best].name, best_d,
                FlagOwner(base, g.flags[best]));
  }
  if (best < 0) {
    bot.forward = 0;
    return;
  }
  // At the flag: in its capture area (the game's own list), or within kAtFlag of its pole (which may stand on a roof):
  // wander about it, a new spot now and then.
  const bool in_area = InCaptureArea(base, g.flags[best], bot.player, soldier);
  if (in_area && !bot.was_in_area) REXLOG_INFO("BF bots: {} is in flag '{}''s area", bot.name, g.flags[best].name);
  bot.was_in_area = in_area;
  const bool at_flag = in_area || best_d < kAtFlag;
  if (!at_flag && bot.no_reach >= 2) {
    // The pole itself out of reach (two plans in a row ended short): a spot 6 to 14 m round it instead.
    const float a = Rand01() * 2 * kPi, r = 6.0f + Rand01() * 8.0f;
    bot.goal_pos[0] = g.flags[best].pos[0] - std::sin(a) * r;
    bot.goal_pos[2] = g.flags[best].pos[2] + std::cos(a) * r;
    bot.no_reach = 0;
  }
  if (at_flag && (Dist2D(pos, bot.goal_pos) < 2.0f || now >= bot.wander_at)) {
    // A spot its side fought or waited from (what was learned), two times in three when there is one; else anywhere
    // about the flag.
    if (!(Rand01() < 0.67f && LearnedSpot(g.flags[best].pos, team, 22.0f, bot.goal_pos))) {
      bot.goal_pos[0] = g.flags[best].pos[0] + (Rand01() - 0.5f) * 16;
      bot.goal_pos[1] = pos[1];
      bot.goal_pos[2] = g.flags[best].pos[2] + (Rand01() - 0.5f) * 16;
    }
    bot.wander_at = now + std::chrono::seconds(6 + int(Rand01() * 6));
    // And it watches where the enemy's kills came from about here.
    float threat[3];
    bot.watching = LearnedThreat(g.flags[best].pos, team, 90.0f, threat);
    if (bot.watching) bot.watch_yaw = YawTo(pos, threat);
  }
  if (!at_flag) bot.watching = false;
  // The way there: a path over the navigation grid, planned again every 3 s, when the goal moves, when the path is
  // used up, or when the bot is stuck.
  const uint32_t phys = Load32(base, soldier + 304);
  NoteWater(base, phys);
  bool stuck = false;
  if (now - bot.check_at > std::chrono::seconds(2)) {
    stuck = bot.check_at != Clock::time_point{} && Dist2D(pos, bot.check_pos) < 0.6f && bot.forward > 0.5f;
    std::memcpy(bot.check_pos, pos, sizeof(bot.check_pos));
    bot.check_at = now;
  }
  const bool goal_moved = Dist2D(bot.goal_pos, bot.path_goal) > 4.0f;
  if (bot.path.empty() || goal_moved || now >= bot.replan_at || stuck) {
    bool complete = false, starved = false;
    // Stuck with a path: the step it was taking is closed for a minute (something the rays did not see: a railing, a
    // fence's gap too small, a slope too steep), and the next plan goes round it.
    if (stuck && !bot.path.empty()) {
      const int cx = CellOf(pos[0]), cz = CellOf(pos[2]);
      const int ddx = (CellOf(bot.path.front()[0]) > cx) - (CellOf(bot.path.front()[0]) < cx);
      const int ddz = (CellOf(bot.path.front()[2]) > cz) - (CellOf(bot.path.front()[2]) < cz);
      for (int d = 0; d < 8; ++d)
        if (kDx[d] == ddx && kDz[d] == ddz) {
          const uint64_t key = (CellKey(cx, cz) << 3) ^ uint64_t(d);
          nav.steps[key] = 2;
          nav.closed_at[key] = now + std::chrono::seconds(40);
        }
    }
    const auto ways = learn.ways_in.find(g.flags[best].name);
    bot.path = FindPath(ctx, base, soldier, pos, bot.goal_pos, &complete, &starved, team,
                        ways != learn.ways_in.end() ? &ways->second : nullptr);
    if (!complete && !starved && !bot.path.empty() && Dist2D(bot.path.back().data(), bot.goal_pos) > 4.0f) ++bot.no_reach;
    if (complete) bot.no_reach = 0;
    std::memcpy(bot.path_goal, bot.goal_pos, sizeof(bot.path_goal));
    bot.replan_at = now + std::chrono::seconds(complete ? 6 : 2);
    if (bot.path.empty() && starved) bot.replan_at = now + std::chrono::milliseconds(500);  // the budget, next ticks
    if (bot.path.empty() && !starved && !stuck) {
      // No way found from here (a spawn inside walls the grid has closed): a walk in a random direction for 2 s.
      if (now >= bot.no_path_log) {
        bot.no_path_log = now + std::chrono::seconds(10);
        std::string around;
        const int cx = CellOf(pos[0]), cz = CellOf(pos[2]);
        for (int d = 0; d < 8; ++d) {
          const auto c = nav.cells.find(CellKey(cx + kDx[d], cz + kDz[d]));
          const auto st = nav.steps.find((CellKey(cx, cz) << 3) ^ uint64_t(d));
          around += fmt::format(" [{},{}: {} y {:.1f} step {}]", kDx[d], kDz[d],
                                c == nav.cells.end() ? -1 : int(c->second.state),
                                c == nav.cells.end() ? 0.0f : c->second.y, st == nav.steps.end() ? -1 : int(st->second));
        }
        REXLOG_INFO("BF bots: {} finds no way from {:.1f} {:.1f} {:.1f} (water {:.1f}):{}", bot.name, pos[0], pos[1],
                    pos[2], nav.water, around);
      }
      bot.detour_yaw = Rand01() * 2 * kPi - kPi;
      bot.detour_until = now + std::chrono::seconds(2);
    }
    if (stuck) {
      // Stuck on something the grid does not know (a vehicle, a soldier, a corner): a side step first.
      bot.detour_yaw = WrapAngle(bot.yaw + (Rand01() < 0.5f ? -1 : 1) * (1.2f + Rand01()));
      bot.detour_until = now + std::chrono::milliseconds(900);
    }
  }
  // Points already reached drop off; the bot heads for the next one.
  while (!bot.path.empty() && Dist2D(pos, bot.path.front().data()) < 1.4f) bot.path.erase(bot.path.begin());
  float want = bot.path.empty() ? YawTo(pos, bot.goal_pos) : YawTo(pos, bot.path.front().data());
  if (now < bot.detour_until) want = bot.detour_yaw;
  bot.want_yaw = want;
  bot.forward = at_flag ? 0.6f : 1.0f;
  if (at_flag && bot.watching && now >= bot.detour_until) {
    // Facing the watched way, moving the wanted way: forward and sideways from the angle between them (strafe -1
    // went to +x facing +z, run bots_s0k).
    const float rel = WrapAngle(want - bot.watch_yaw);
    bot.want_yaw = bot.watch_yaw;
    bot.forward = Dist2D(pos, bot.goal_pos) < 1.5f ? 0.0f : 0.6f * std::cos(rel);
    bot.strafe = Dist2D(pos, bot.goal_pos) < 1.5f ? 0.0f : 0.6f * std::sin(rel);
  }
  // Sprint on the way, not at the flag or when turning hard.
  bot.sprint = bot.sprinter && !at_flag && best_d > 20.0f && std::fabs(WrapAngle(want - bot.yaw)) < 0.5f;
}

// One server tick of input for the bot's soldier, as a client's message would bring it.
void Drive(PPCContext& outer, uint8_t* base, Bot& bot, uint32_t soldier, double dt) {
  // Turn toward what the brain wants, at most kTurnRate radians a second.
  const float step = float(kTurnRate * dt);
  bot.yaw = WrapAngle(bot.yaw + std::clamp(WrapAngle(bot.want_yaw - bot.yaw), -step, step));
  bot.pitch += std::clamp(bot.want_pitch - bot.pitch, -step, step);
  PPCContext call = outer;
  call.r3.u64 = bot.state, call.r4.u64 = 0, call.f1.f64 = bot.forward;
  sub_822BD518(call, base);
  call = outer;
  call.r3.u64 = bot.state, call.r4.u64 = 1, call.f1.f64 = bot.strafe;
  sub_822BD518(call, base);
  call = outer;
  call.r3.u64 = bot.state, call.r4.u64 = 7, call.f1.f64 = bot.fire ? 1.0 : 0.0;  // the trigger
  sub_822BD518(call, base);
  StoreF(base, bot.state + 128, float(dt));
  call = outer;
  call.r3.u64 = bot.input, call.r4.u64 = bot.state;
  sub_822C15D0(call, base);
  const uint32_t cur = Load32(base, bot.input + 352);
  if (Readable(cur, 176)) {
    base[cur + 122] |= 0x09;  // digital bits 40 (aim valid) and 43 (from the client)
    if (bot.sprint) base[cur + 125] |= 0x40;  // digital concept 22: sprint
    if (bot.ads && bot.enemy) base[cur + 126] |= 0x08;  // digital concept 11: aim down the sights (LT, run bots_calib)
    StoreF(base, cur + 136, bot.yaw);
    StoreF(base, cur + 140, bot.pitch);
    Store32(base, cur + 152, ++bot.tick);
  }
  // The soldier's input step, vtable +260, for its entry 0 (what a connection calls after each input message).
  const uint32_t vtable = Load32(base, soldier);
  if (!InImage(vtable)) return;
  call = outer;
  call.r3.u64 = soldier, call.r4.u64 = 0;
  CallGuest(call, base, Load32(base, vtable + 260));
}
}  // namespace

// The game's team pick (sub_82399AE8(player, team, find a squad, keep the cap)): a bot stays on its own side. The
// round's start re-picks every player's team (run bots_step0: two Japanese bots on the US carrier), and a bot's name
// and kit are its side's.
REX_EXTERN(__imp__sub_82399AE8);
REX_HOOK_RAW(sub_82399AE8) {
  if (REXCVAR_GET(server)) {
    for (const Bot& b : g.bots)
      if (b.player == ctx.r3.u32 && b.team && int(ctx.r4.s32) != b.team) {
        ctx.r4.u64 = uint32_t(b.team);
        break;
      }
  }
  __imp__sub_82399AE8(ctx, base);
}

// The round's start (sub_8238EB68): it spawns every listed player at one spawn point, skipping a player whose byte
// +516 is set. The bots are skipped for it (+516 set for the call, then as it was): they spawn at their own side's
// spawn points, as their deaths' respawns do.
REX_EXTERN(__imp__sub_8238EB68);
REX_HOOK_RAW(sub_8238EB68) {
  std::vector<std::pair<uint32_t, uint8_t>> kept;
  if (REXCVAR_GET(server))
    for (const Bot& b : g.bots)
      if (Readable(b.player, 0x210)) kept.push_back({b.player, base[b.player + 516]}), base[b.player + 516] = 1;
  __imp__sub_8238EB68(ctx, base);
  for (const auto& [p, v] : kept) base[p + 516] = v;
}

// The scoring's per-player records: the game context's scoring (+68), its records by player id (+8; the id at
// player+260, vtable +44). A connection's player gets one; a bot has none, and the death scoring
// sub_823F86F0(scoring, player, ...) wrote through the missing record (run bots_s1h: the server's crash when a bot
// died). It is skipped for a player without a record. TODO(bots): records of their own (scoreboard points).
REX_EXTERN(__imp__sub_823F86F0);
REX_HOOK_RAW(sub_823F86F0) {
  if (REXCVAR_GET(server)) {
    const uint32_t scoring = ctx.r3.u32, player = ctx.r4.u32;
    const uint32_t records = Readable(scoring, 16) ? Load32(base, scoring + 8) : 0;
    const uint32_t id = Readable(player, 0x110) ? Load32(base, player + 260) : 0xFFFFFFFFu;
    if (!records || id > 255 || !Readable(records + 4 * id, 4) || !Load32(base, records + 4 * id)) {
      if (IsBot(player)) return;
    }
  }
  __imp__sub_823F86F0(ctx, base);
}

// The idle kick's check (sub_8237F1F8(player, dt): a player whose input stays still too long is kicked): never a bot.
REX_EXTERN(__imp__sub_8237F1F8);
REX_HOOK_RAW(sub_8237F1F8) {
  if (REXCVAR_GET(server) && IsBot(ctx.r3.u32)) return;
  __imp__sub_8237F1F8(ctx, base);
}

// Who gives a bot its soldier (the caller's return address: 8236B988 ours, 8238ED7C the round's start).
REX_EXTERN(__imp__sub_8238BF88);
REX_HOOK_RAW(sub_8238BF88) {
  if (REXCVAR_GET(server) && IsBot(ctx.r3.u32))
    REXLOG_INFO("BF bots: player {:08X} takes soldier {:08X} (lr {:08X})", ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr));
  __imp__sub_8238BF88(ctx, base);
}

// Each ServerSoldierSpawnEntity as it is made (the level's loading).
REX_EXTERN(__imp__sub_82362A40);
// The vehicles as the server makes and ends them (g.vehicles).
REX_EXTERN(__imp__sub_8230A580);
REX_HOOK_RAW(sub_8230A580) {
  const uint32_t vehicle = ctx.r3.u32;
  __imp__sub_8230A580(ctx, base);
  if (REXCVAR_GET(server)) {
    std::lock_guard lock(g.mutex);
    g.vehicles.insert(vehicle);
  }
}
REX_EXTERN(__imp__sub_822FF5B0);
REX_HOOK_RAW(sub_822FF5B0) {
  if (REXCVAR_GET(server)) {
    std::lock_guard lock(g.mutex);
    g.vehicles.erase(ctx.r3.u32);
  }
  __imp__sub_822FF5B0(ctx, base);
}

REX_HOOK_RAW(sub_82362A40) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_82362A40(ctx, base);
  if (!REXCVAR_GET(server)) return;
  std::lock_guard lock(g.mutex);
  g.spawns.insert(self);
}

namespace bf1943 {

// A kill (server_stats.cpp's hook on the game's kill handling, before it): where the killer stood and where the victim
// fell, by side (a human killer's counts 1, a bot's 0.5).
void BotsNoteKill(PPCContext& ctx, uint8_t* base, uint32_t killer, uint32_t victim) {
  if (!REXCVAR_GET(server) || learn.map.empty() || !Readable(victim, 0x140)) return;
  const int vside = int(Load32(base, victim + 48));
  float vp[3] = {};
  const uint32_t vso = Load32(base, victim + 288);
  if (vso && Position(ctx, base, vso, vp) && (vside == 1 || vside == 2))
    learn.deaths[vside - 1][CellKey(CellOf(vp[0]), CellOf(vp[2]))] += 1.0f, learn.dirty = true;
  if (!killer || killer == victim || !Readable(killer, 0x140)) return;
  const int kside = int(Load32(base, killer + 48));
  float kp[3] = {};
  const uint32_t kso = Load32(base, killer + 288);
  // A kill from a vehicle: the vehicle's (where and how far), kept out of the soldiers' kill spots and the players'
  // infantry style; the enemy's recent firing spot all the same.
  uint32_t kveh = 0;
  const std::string vkey = VehicleKey(base, killer, &kveh);
  if (!vkey.empty()) {
    float at[3];
    if (kveh && Position(ctx, base, kveh, at) && (kside == 1 || kside == 2) && kside != vside) {
      NoteRecent(recent_fire[kside - 1], CellKey(CellOf(at[0]), CellOf(at[2])), 1.0f);
      if (!IsBot(killer)) {
        auto& vu = learn.vehicles[vkey];
        vu.kills += 1, vu.kill_dist += std::min(Dist2D(at, vp), 1000.0f);
        vu.kills_at[CellKey(CellOf(at[0]), CellOf(at[2]))] += 1.0f, learn.dirty = true;
        REXLOG_INFO("BF bots: a player's kill from {} at {:.0f} m", vkey, Dist2D(at, vp));
      }
    }
    return;
  }
  if (kso && kso != vso && Position(ctx, base, kso, kp) && (kside == 1 || kside == 2) && kside != vside) {
    learn.kills[kside - 1][CellKey(CellOf(kp[0]), CellOf(kp[2]))] += IsBot(killer) ? 0.5f : 1.0f, learn.dirty = true;
    NoteRecent(recent_fire[kside - 1], CellKey(CellOf(kp[0]), CellOf(kp[2])), 1.0f);
    if (!IsBot(killer)) {
      // A human's kill: its range, and whether it came from the victim's side or back (the victim's facing: a bot's
      // own, a human's input's aim).
      style_match.kills += 1, style_match.kill_dist += std::min(Dist2D(kp, vp), 300.0f);
      float facing = 0;
      bool known = false;
      for (const Bot& b : g.bots)
        if (b.player == victim) facing = b.yaw, known = true;
      if (!known) {
        const uint32_t in = Load32(base, victim + 280), st = in && Readable(in, 360) ? Load32(base, in + 352) : 0;
        if (st && Readable(st, 176)) facing = LoadF(base, st + 136), known = true;
      }
      if (known && std::fabs(WrapAngle(YawTo(vp, kp) - facing)) > 1.75f) style_match.flank_kills += 1;
    }
  }
  if (vso && (vside == 1 || vside == 2)) NoteRecent(recent_deaths[vside - 1], CellKey(CellOf(vp[0]), CellOf(vp[2])), 1.0f);
}

// One of our bots (server_stats.cpp keeps them out of the players' stats). The server thread's.
bool BotIsPlayer(uint32_t player) { return player && IsBot(player); }

// A new level: the old one's spawn points and players are gone.
void BotsLevelChange() {
  if (learn.dirty) SaveLearned();
  if (!learn.map.empty() && !nav.cells.empty()) SaveGrid();
  learn.map.clear();
  std::lock_guard lock(g.mutex);
  g.spawns.clear();
  g.vehicle_spawns.clear();
  g.bots.clear();
  g.level_seen = false;
  nav.cells.clear(), nav.steps.clear(), nav.closed_at.clear();
  nav.scan.clear(), nav.scanned.clear(), nav.scan_started = nav.scan_done = false;
  nav.scan_min[0] = nav.scan_min[1] = 1e9f, nav.scan_max[0] = nav.scan_max[1] = -1e9f;
}

// Every server tick (server_rotation.cpp, after the game's own tick).
void BotsTick(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(server)) return;
  const int wanted = WantedBots();
  if (wanted <= 0) return;
  const auto now = Clock::now();
  static Clock::time_point last = now;
  const double dt = std::clamp(std::chrono::duration<double>(now - last).count(), 0.0, 0.1);
  last = now;
  const uint32_t manager = Manager(base);
  if (!manager) return;
  if (manager != g.manager) {  // a new manager: a new game; nothing of ours is in it
    g.manager = manager;
    g.bots.clear();
  }
  if (!g.level_seen) {
    size_t n;
    {
      std::lock_guard lock(g.mutex);
      n = g.spawns.size();
    }
    if (!n) return;
    g.level_seen = true, g.level_at = now;
    REXLOG_INFO("BF bots: a level with {} soldier spawn points; {} bots in 10 s", n, wanted);
  }
  if (now - g.level_at < std::chrono::seconds(10)) return;
  if (g.scanned_manager != manager) {
    g.scanned_manager = manager;
    ScanTeams(base);
    ScanFlags(base);
    // What earlier matches learned here: by the map's title (the rotation's), else its first flag's name.
    std::string map = CurrentMapTitle();
    if (map.empty() && !g.flags.empty()) map = g.flags.front().name;
    if (!map.empty() && map != learn.map) {
      if (learn.dirty) SaveLearned();
      LoadLearned(map);
      LoadGrid();
      LoadStyle();
      recent_deaths[0].clear(), recent_deaths[1].clear(), recent_fire[0].clear(), recent_fire[1].clear();
    }
  }
  // Bots still listed (a player the game dropped is no longer ours to drive).
  const std::vector<uint32_t> listed = ListedPlayers(base, manager);
  for (auto it = g.bots.begin(); it != g.bots.end();) {
    if (std::find(listed.begin(), listed.end(), it->player) == listed.end()) {
      REXLOG_INFO("BF bots: {} is no longer in the player list", it->name);
      it = g.bots.erase(it);
    } else {
      ++it;
    }
  }
  // As many as asked for, within the room the humans leave (the user's match of 19 Nov 2026: 23 bots and the player
  // made 24, the US side was full and the 23rd bot got no team and never spawned); each on the side with fewer players
  // as it joins (the humans' sides counted too).
  {
    int humans = 0, side[3] = {0, 0, 0};
    for (uint32_t p : listed) {
      if (!IsBot(p)) ++humans;
      const uint32_t t = Load32(base, p + 48);
      if (t == 1 || t == 2) ++side[t];
    }
    const int room = std::max(0, int(REXCVAR_GET(bf_gm_max_players)) - humans);
    const int target = std::min(wanted, room);
    static int told = -1;
    if (target < wanted && told != target) {
      told = target;
      REXLOG_INFO("BF bots: {} wanted, room for {} beside {} human player(s) ({} at most)", wanted, target, humans,
                  REXCVAR_GET(bf_gm_max_players));
    }
    while (int(g.bots.size()) < target) {
      const int team = side[1] <= side[2] ? 1 : 2;
      if (!MakeBot(ctx, base, manager, int(g.bots.size()), team, listed)) break;
      ++side[team];
    }
  }
#if defined(BF_DEV_TOOLS)
  {  // BF_BOT_VEHLOG=1 (tests): the level's vehicles, their places and seats, every 20 s.
    static const bool dev = std::getenv("BF_BOT_VEHLOG") != nullptr;
    static Clock::time_point logged{};
    if (dev && now - logged > std::chrono::seconds(20)) {
      logged = now;
      std::vector<uint32_t> vs;
      {
        std::lock_guard lock(g.mutex);
        vs.assign(g.vehicles.begin(), g.vehicles.end());
      }
      for (uint32_t v : vs) {
        float at[3] = {};
        Position(ctx, base, v, at);
        const uint32_t n = SeatCount(ctx, base, v);
        std::string seats;
        for (uint32_t i = 0; i < n; ++i) seats += SeatPlayer(base, v, i) ? " taken" : " free";
        REXLOG_INFO("BF bots vehicle: {:08X} {} at {:.1f} {:.1f} {:.1f}, {} seats:{}", v, VehicleName(base, v), at[0],
                    at[1], at[2], n, seats);
      }
    }
  }
#endif
  nav.rays_left = kRaysPerTick;
  static Clock::time_point ray_log{};
  if (now - ray_log > std::chrono::seconds(10)) {
    if (ray_log != Clock::time_point{})
      REXLOG_INFO("BF bots: {} ray tests in 10 s, {:.3f} ms each; {} grid cells, {} steps known", g.rays,
                  g.rays ? g.ray_ms / double(g.rays) : 0.0, nav.cells.size(), nav.steps.size());
    ray_log = now, g.rays = 0, g.ray_ms = 0;
  }
  // BF_BOT_INPUTLOG=1 (tests): each human's input as it changes, its digital concepts (the bits set) and its analog
  // ones not at rest: which concept each button is, for the bots' own crouch, zoom, reload and the style's measures.
  static const bool input_log = [] {
    const char* v = std::getenv("BF_BOT_INPUTLOG");
    return v && *v == '1';
  }();
  if (input_log) {
    static std::map<uint32_t, std::string> last_input;
    for (uint32_t p : listed) {
      if (IsBot(p) || !Readable(p, 0x140)) continue;
      const uint32_t in = Load32(base, p + 280), st = in && Readable(in, 360) ? Load32(base, in + 352) : 0;
      if (!st || !Readable(st, 176)) continue;
      std::string what;
      for (int c = 0; c < 64; ++c)
        if (base[st + 120 + 7 - c / 8] & (1 << (c % 8))) what += fmt::format(" d{}", c);
      for (int c = 0; c < 8; ++c) {
        const float v = LoadF(base, st + 8 + 4 * c);
        if (std::fabs(v) > 0.2f) what += fmt::format(" a{}={:.1f}", c, v);
      }
      if (what != last_input[p]) {
        last_input[p] = what;
        REXLOG_INFO("BF bots input: {} {}", PlayerName(base, p), what.empty() ? "(rest)" : what);
      }
    }
  }
  // Learning from the match (Learned): every soldier on foot's walk (a human's counts 1, a bot's 0.5), and a
  // human's trail of the last 90 s and its waits near enemies.
  struct Seen {
    float pos[3];
    int side;
    bool human;
  };
  std::vector<Seen> seen;
  std::map<uint32_t, size_t> seen_of;
  for (uint32_t p : listed) {
    if (!Readable(p, 0x140)) continue;
    const uint32_t so = Load32(base, p + 288);
    Seen s{{}, int(Load32(base, p + 48)), !IsBot(p)};
    if (!so || Load32(base, p + 312) != so || !Position(ctx, base, so, s.pos)) continue;
    seen_of[p] = seen.size();
    seen.push_back(s);
  }
  static std::map<uint32_t, std::array<float, 3>> walked;
  static std::map<uint32_t, std::deque<std::pair<Clock::time_point, uint64_t>>> trails;
  static std::map<uint32_t, double> still;
  for (auto it = walked.begin(); it != walked.end();) it = seen_of.count(it->first) ? std::next(it) : walked.erase(it);
  for (const auto& [p, i] : seen_of) {
    const Seen& me = seen[i];
    auto& trail = trails[p];
    while (!trail.empty() && now - trail.front().first > std::chrono::seconds(90)) trail.pop_front();
    auto it = walked.find(p);
    if (it == walked.end()) {
      walked[p] = {me.pos[0], me.pos[1], me.pos[2]};
      continue;
    }
    const float moved = Dist2D(it->second.data(), me.pos);
    if (me.human) {
      // The style: its time on foot, sprinting, still, firing (and with the sights), its bursts, attacking or
      // defending (the nearest flag within 40 m its side holds or not), and its kit at each spawn.
      static std::map<uint32_t, double> firing;
      static std::map<uint32_t, uint32_t> last_soldier;
      const uint32_t in = Load32(base, p + 280), st = in && Readable(in, 360) ? Load32(base, in + 352) : 0;
      const bool fire = st && Readable(st, 176) && LoadF(base, st + 8 + 4 * 7) > 0.5f;
      const bool sights = st && Readable(st, 176) && (base[st + 126] & 0x08);
      style_match.time += dt;
      // Speed over half second windows (a player's position comes with its game's messages, not every tick: per tick
      // a walk looked like a sprint, run bots_learn5): faster than 5.8 m/s sprinting, slower than 0.3 m/s still.
      static std::map<uint32_t, std::array<double, 3>> window;  // distance, time, firing time
      static std::map<uint32_t, std::array<float, 3>> last_pos;  // this tick's step (the walk's point lags behind)
      auto lp = last_pos.find(p);
      const float step = lp == last_pos.end() ? 0.0f : std::min(Dist2D(lp->second.data(), me.pos), 10.0f);
      last_pos[p] = {me.pos[0], me.pos[1], me.pos[2]};
      auto& w = window[p];
      w[0] += step, w[1] += dt, w[2] += fire ? dt : 0.0;
      if (w[1] >= 0.5) {
        const double speed = w[0] / w[1];
        if (speed > 5.8) style_match.sprint += w[1];
        if (speed < 0.3 && w[2] == 0) style_match.still += w[1];
        w = {0, 0, 0};
      }
      if (fire) style_match.fire += dt, firing[p] += dt;
      if (fire && sights) style_match.ads += dt;
      if (!fire && firing[p] > 0) style_match.bursts += 1, style_match.burst_time += std::min(firing[p], 3.0), firing[p] = 0;
      int near_flag = -1;
      float near_d = 40.0f;
      for (size_t f = 0; f < g.flags.size(); ++f)
        if (g.flags[f].takeable && Dist2D(g.flags[f].pos, me.pos) < near_d)
          near_d = Dist2D(g.flags[f].pos, me.pos), near_flag = int(f);
      if (near_flag >= 0) (FlagOwner(base, g.flags[near_flag]) == me.side ? style_match.defend : style_match.attack) += dt;
      const uint32_t so = Load32(base, p + 288);
      if (so != last_soldier[p]) {
        last_soldier[p] = so;
        const auto kits = g.team_kits.find(uint32_t(me.side));
        if (kits != g.team_kits.end())
          for (size_t k = 0; k < kits->second.size() && k < 3; ++k)
            if (Load32(base, kits->second[k] + 0x3C) == Load32(base, p + 500)) style_match.kit[k] += 1;
      }
      // Waiting: still for 3 s and more with an enemy within 60 m.
      still[p] = moved < 0.05f ? still[p] + dt : 0.0;
      if (still[p] > 3.0 && (me.side == 1 || me.side == 2)) {
        bool enemy_near = false;
        for (const Seen& o : seen)
          if (o.side != me.side && (o.side == 1 || o.side == 2) && Dist2D(o.pos, me.pos) < 60.0f) enemy_near = true;
        if (enemy_near) learn.waits[me.side - 1][CellKey(CellOf(me.pos[0]), CellOf(me.pos[2]))] += float(dt), learn.dirty = true;
      }
    }
    if (moved < 0.3f) continue;  // standing: keep the last point
    if (moved < 12.0f && std::fabs(me.pos[1] - it->second[1]) < 3.0f)  // a walk, not a spawn or a fall
      LearnWalk(it->second.data(), me.pos, me.human ? 1.0f : 0.5f, me.human ? &trail : nullptr);
    it->second = {me.pos[0], me.pos[1], me.pos[2]};
  }
  // The humans in vehicles: time in each vehicle and seat, and where it went (a sample every half second).
  {
    static std::map<uint32_t, Clock::time_point> sampled;
    for (uint32_t p : listed) {
      if (IsBot(p) || !Readable(p, 0x140)) continue;
      uint32_t veh = 0;
      const std::string key = VehicleKey(base, p, &veh);
      if (key.empty()) continue;
      auto& vu = learn.vehicles[key];
      vu.time += dt, learn.dirty = true;
      if (now - sampled[p] < std::chrono::milliseconds(500)) continue;
      sampled[p] = now;
      float at[3];
      if (!Position(ctx, base, veh, at)) continue;
      auto& c = vu.cells[CellKey(CellOf(at[0]), CellOf(at[2]))];
      c.first += at[1], c.second += 1;
      static std::set<std::string> told;
      if (told.insert(key).second) REXLOG_INFO("BF bots: a player in {} (learning its way)", key);
    }
  }
  if (learn.dirty && now - learn.saved_at > std::chrono::seconds(20)) SaveLearned();
  // The level's scan: once the water's height is known (a bot's physics), from every spawn point and flag.
  {
    uint32_t any_soldier = 0;
    for (const Bot& b : g.bots)
      if (const uint32_t so = Load32(base, b.player + 288)) {
        any_soldier = so;
        const uint32_t phys = Load32(base, so + 304);
        NoteWater(base, phys);
        break;
      }
    if (!nav.scan_started && !nav.scan_done && any_soldier && !learn.map.empty()) {
      nav.scan_started = true, nav.scan_began = now;
      nav.scanned.clear();  // a scan loaded in part: walked again from the start (its cells known: few rays)
      std::vector<uint32_t> spawns;
      {
        std::lock_guard lock(g.mutex);
        spawns.assign(g.spawns.begin(), g.spawns.end());
      }
      for (uint32_t e : spawns) {
        float at[3];
        if (Readable(e, 0x100) && Load32(base, e) == kSoldierSpawnVtable && Position(ctx, base, e, at)) ScanFrom(at);
      }
      for (const Flag& f : g.flags) ScanFrom(f.pos);
      for (const auto& [k, y] : learn.walk_y) {  // the cells already known go on from where they are
        if (nav.scanned.insert(k).second) nav.scan.push_back(k);
      }
      REXLOG_INFO("BF bots: mapping the level from {} places (water at {:.1f})", nav.scan.size(), nav.water);
    }
    static Clock::time_point grid_saved{};
    if (ScanTick(ctx, base, any_soldier) && now - grid_saved > std::chrono::seconds(60)) {
      grid_saved = now;
      SaveGrid();
      REXLOG_INFO("BF bots: mapping: {} cells known, {} to look at", nav.cells.size(), nav.scan.size());
    }
  }
  // Squads (the plan's step 2d; the vehicle bindings session for the user, 18 Nov 2026: "make bots join the game's own
  // squads ... so a human player ends up in a squad with bots"). The game's team pick looks only for an open squad
  // (sub_82394FE0) and makes one only for a player with +592 and +596 set, which a bot has not: every bot stayed in
  // squad 24 (none), and so did the players. Every 2 s: a bot in no squad goes into a squad of its side with room (4
  // a squad: TeamData's MaxPlayersPerSquad), one with a human first, then one with bots, else a squad no one uses; a
  // human in no squad goes into a squad of its side with bots and leads it (bf_bots_squad_humans).
  static Clock::time_point squads_at{};
  if (now - squads_at > std::chrono::seconds(2)) {
    squads_at = now;
    constexpr int kSquadSize = 4;
    std::map<std::pair<int, uint32_t>, std::vector<uint32_t>> squads;  // (side, squad) -> members
    std::set<uint32_t> used;                                           // squad numbers in use by either side
    for (uint32_t p : listed) {
      if (!Readable(p, 0x140)) continue;
      const int side = int(Load32(base, p + 48));
      const uint32_t sq = Load32(base, p + 52);
      if ((side == 1 || side == 2) && sq < 24) squads[{side, sq}].push_back(p), used.insert(sq);
    }
    auto has_human = [&](const std::vector<uint32_t>& m) {
      return std::any_of(m.begin(), m.end(), [](uint32_t q) { return !IsBot(q); });
    };
    auto put = [&](uint32_t p, uint32_t sq, int side) {
      PPCContext call = ctx;
      call.r3.u64 = p, call.r4.u64 = sq;
      sub_8237B320(call, base);
      squads[{side, sq}].push_back(p), used.insert(sq);
    };
    for (uint32_t p : listed) {
      if (!Readable(p, 0x140) || Load32(base, p + 52) < 24) continue;
      const int side = int(Load32(base, p + 48));
      if (side != 1 && side != 2) continue;
      const bool human = !IsBot(p);
      if (human && !REXCVAR_GET(bf_bots_squad_humans)) continue;
      // The squad: for a human, its side's with bots and room (most bots first); for a bot, one with a human and
      // room, then one with bots and room, then a new one.
      int best_sq = -1, best_rank = -1;
      for (const auto& [key, m] : squads) {
        if (key.first != side || int(m.size()) >= kSquadSize) continue;
        const bool with_human = has_human(m);
        const int rank = human ? (with_human ? -1 : int(m.size())) : (with_human ? 10 : 5) + int(m.size());
        if (rank > best_rank) best_rank = rank, best_sq = int(key.second);
      }
      if (best_sq < 0) {
        if (human) continue;  // no bot squad of its side: the player chooses its own
        for (uint32_t sq = 0; sq < 24; ++sq)
          if (!used.count(sq)) {
            best_sq = int(sq);
            break;
          }
      }
      if (best_sq < 0) continue;
      put(p, uint32_t(best_sq), side);
      std::string who = PlayerName(base, p);  // +0x18 (a bot's is set there too, MakeBot)
      for (const Bot& b : g.bots)
        if (b.player == p) who = b.name;
      REXLOG_INFO("BF bots: {} joins squad {} of {}", who, best_sq, side == 1 ? "the US" : "Japan");
      if (human) {
        // The player leads it: the bots in it no longer do.
        for (uint32_t q : squads[{side, uint32_t(best_sq)}]) {
          if (q == p || !(base[q + 132] & 0x80)) continue;
          PPCContext call = ctx;
          call.r3.u64 = q, call.r4.u64 = 0, call.r5.u64 = (base[q + 132] >> 4) & 1;
          sub_82375280(call, base);
        }
        if (!(base[p + 132] & 0x80)) {
          PPCContext call = ctx;
          call.r3.u64 = p, call.r4.u64 = 1, call.r5.u64 = (base[p + 132] >> 4) & 1;
          sub_82375280(call, base);
        }
        REXLOG_INFO("BF bots: {} leads squad {} ({} in it)", PlayerName(base, p), best_sq,
                    squads[{side, uint32_t(best_sq)}].size());
      }
    }
  }
  // Each flag's holder, logged when it changes.
  static std::map<uint32_t, int> holders;
  for (const Flag& f : g.flags) {
    const int h = FlagOwner(base, f);
    auto it = holders.find(f.entity);
    if (it != holders.end() && it->second != h) {
      REXLOG_INFO("BF bots: flag '{}' now held by {}", f.name, h == 1 ? "US" : h == 2 ? "Japan" : "nobody");
      // Taken: the humans of the side that took it, there now, walked a way in that worked (their last minute).
      int learned = 0;
      for (const auto& [p, i] : seen_of) {
        if (!seen[i].human || seen[i].side != h || Dist2D(seen[i].pos, f.pos) > 25.0f) continue;
        for (const auto& [t, step] : trails[p])
          if (now - t < std::chrono::seconds(60)) learn.ways_in[f.name][step] += 1.0f, ++learned;
      }
      if (learned) {
        learn.dirty = true;
        REXLOG_INFO("BF bots: learned a way into '{}' ({} steps)", f.name, learned);
      }
    }
    holders[f.entity] = h;
  }
  for (Bot& bot : g.bots) {
    const uint32_t soldier = Load32(base, bot.player + 288);
    if (soldier != bot.last_soldier) {
      if (!soldier) {
        bot.died = now;
        bot.kit_choice = -1;  // a kit chosen again for the next life
        REXLOG_INFO("BF bots: {} died", bot.name);
      }
      bot.last_soldier = soldier;
    }
    if (!soldier) {
      if (now - bot.died < std::chrono::seconds(5) || bot.spawn_tries > 20) continue;
      ChooseKit(base, bot);
      if (!Load32(base, bot.player + 500)) continue;  // no kit: the spawn would read the null page
      const uint32_t at = SpawnFor(base, int(Load32(base, bot.player + 48)), bot.spawn_tries);
      ++bot.spawn_tries;
      bot.died = now;  // the next try 5 s on
      if (!at) {
        REXLOG_INFO("BF bots: no spawn point of team {} for {}", Load32(base, bot.player + 48), bot.name);
        continue;
      }
      PPCContext call = ctx;
      call.r3.u64 = at, call.r4.u64 = bot.player, call.r5.u64 = 1, call.r6.u64 = 1, call.r7.u64 = 0;
      sub_8236B688(call, base);
      float p[3] = {};
      Position(ctx, base, at, p);
      const uint32_t spawned = Load32(base, bot.player + 288);
      // Seated: the player controls something else than its soldier (+312): a spawn point that puts its soldier in a
      // vehicle (SoldierSpawnEntityData's Vehicle). Bots do not use it until they can drive (the spawn data logged).
      const uint32_t controls = Load32(base, bot.player + 312);
      const bool seated = spawned && controls && controls != spawned;
      if (seated) {
        g.vehicle_spawns.insert(at);
        std::string w;
        const uint32_t data = Load32(base, at + 20);
        for (uint32_t o = 0x80; o < 0x110 && Readable(data, 0x110); o += 4) w += fmt::format(" {:08X}", Load32(base, data + o));
        REXLOG_INFO("BF bots: spawn point {:08X} seats its soldier in {:08X} (vtable {:08X}); data +80..:{}", at, controls,
                    Readable(controls, 4) ? Load32(base, controls) : 0, w);
      }
      REXLOG_INFO("BF bots: {} spawns at {:.1f} {:.1f} {:.1f}: {}", bot.name, p[0], p[1], p[2],
                  !spawned ? "refused" : seated ? "in a vehicle" : "on foot");
      if (spawned) bot.spawn_tries = 0;
      continue;
    }
    // In a vehicle: a passenger's seat, given up only with its life for now (bots do not drive yet), no input of ours.
    if (const uint32_t controls = Load32(base, bot.player + 312); controls && controls != soldier) continue;
    float pos[3] = {};
    if (!Position(ctx, base, soldier, pos)) continue;
    // Riding along (the user, 19 Nov 2026: "get into friendly boats passing by close enough if a seat is available"):
    // a vehicle of its own side that a human drives, within 6 m (a swimmer's reach, or a step to it), gets the bot into
    // its first free seat other than the driver's, by the server's own seating (sub_82398B98, as a client's request).
    // Checked twice a second.
    if (now >= bot.next_ride) {
      bot.next_ride = now + std::chrono::milliseconds(500);
      std::vector<uint32_t> vs;
      {
        std::lock_guard lock(g.mutex);
        vs.assign(g.vehicles.begin(), g.vehicles.end());
      }
      const uint32_t side = Load32(base, bot.player + 48);
      for (uint32_t v : vs) {
        const uint32_t driver = SeatPlayer(base, v, 0);
        if (!driver || IsBot(driver) || !Readable(driver, 0x40) || Load32(base, driver + 48) != side) continue;
        float at[3];
        if (!Position(ctx, base, v, at) || Dist2D(at, pos) > 6.0f || std::fabs(at[1] - pos[1]) > 4.0f) continue;
        const uint32_t n = SeatCount(ctx, base, v);
        for (uint32_t seat = 1; seat < n; ++seat) {
          if (SeatPlayer(base, v, seat)) continue;
          PPCContext call = ctx;
          call.r3.u64 = bot.player, call.r4.u64 = v, call.r5.u64 = seat;
          sub_82398B98(call, base);
          REXLOG_INFO("BF bots: {} rides in {} (seat {}), {}", bot.name, VehicleName(base, v), seat,
                      Load32(base, bot.player + 312) == v ? "seated" : "refused");
          break;
        }
        break;
      }
      if (const uint32_t controls = Load32(base, bot.player + 312); controls && controls != soldier) continue;
    }
    if (now >= bot.next_think) {
      bot.next_think = now + std::chrono::milliseconds(250);
      Think(ctx, base, bot, soldier, pos, listed, now);
    }
    Drive(ctx, base, bot, soldier, dt);
    if (now >= bot.next_log) {
      bot.next_log = now + std::chrono::seconds(3);
      REXLOG_INFO("BF bots: {} (team {}) at {:.1f} {:.1f} {:.1f}, heading {:.2f}{}{}", bot.name,
                  Load32(base, bot.player + 48), pos[0], pos[1], pos[2], bot.yaw, bot.enemy ? ", fighting" : "",
                  bot.fire ? ", firing" : "");
    }
  }
}

}  // namespace bf1943
