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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
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

#include "dev_guest_memory.h"
#include "guest_readable.h"
#include "player_limits.h"

REXCVAR_DECLARE(bool, server);  // server_mode.cpp

namespace bf1943::botchat {  // bot_chat.cpp: the bots' text chat
struct Who {
  std::string name;
  int team = 0, squad = 0;
  bool bot = false;
  bool leader = false;
  std::string goal;
};
void Kill(const Who& killer, const Who& victim, float distance, bool from_vehicle, const std::vector<Who>& bots);
void Flags(const std::vector<std::pair<std::string, int>>& flags, const std::vector<Who>& bots);
void Tick(const std::vector<Who>& bots);
void Bail(const Who& who, bool plane);
void Rally(const Who& leader);
}  // namespace bf1943::botchat
namespace bf1943 {
std::string CapturePointTitle(const std::string& name);  // capture_points.cpp: "Village" for CQ_MP_BASE_CANAL_VILLAGE
}
REXCVAR_DECLARE(std::string, user_data_root);
REXCVAR_DECLARE(int32_t, bf_gm_max_players);  // gm_host.cpp: the most players a match holds
REXCVAR_DEFINE_INT32(bf_bots, 0, "BF1943/Server", "Bots: how many bots play on this server (0: none)");
REXCVAR_DEFINE_BOOL(bf_bots_log_detail, false, "BF1943/Server",
                    "Bots: log each bot's moves, sights, flags, deaths and spawns (tests; off, the log keeps joins and "
                    "what they learn)");
// Off by default since 20 Nov 2026 (the user: "Humans shouldn't automatically be forced into a squad, but bots should
// join if a human creates a squad on their own. Humans joining a bot-created squad should automatically promote them to
// squad leader").
REXCVAR_DEFINE_BOOL(bf_bots_dynamic_aggression, true, "BF1943/Server",
                    "Bots: losing to the players (kills and flags), they press harder: squads attack together, flank, "
                    "share what they see, regroup before an attack, aim a little better");
// The bots' wait while no human is on the server (BotsTickImpl): off for a test server that must run as a full one around
// the clock (the Linux server optimisation of 27 Nov 2026, US Central at 64 bots: TekRantGaming, "Ensure bots remain
// active even when no human players are connected"). Also BF_BOTS_PLAY_ALONE=1 in the server's environment.
REXCVAR_DEFINE_BOOL(bf_bots_play_alone, false, "BF1943/Server",
                    "Bots: play on while no human is on the server (tests of a full server; off: they wait until a "
                    "player joins)");
REXCVAR_DEFINE_BOOL(bf_bots_squad_humans, false, "BF1943/Server",
                    "Bots: a human in no squad is put into a squad with bots (off: players choose their own; bots fill "
                    "theirs)");
// The shared bot data (TekRantGaming, 26 Nov 2026: "we do this for the official servers only, and then we can, when we
// do like hot fixes and releases, also include updated bot information for community servers"). The official servers
// hand what their bots learn to the main backend (bots/outbox/, sent by server/gmhost.py), which merges it
// (server/botdata.py) and sends the merged set back to them (bots/network/); every release carries the latest set
// beside the program (data/bots/). The bots use the newer of the two over this server's own learning.
REXCVAR_DEFINE_BOOL(bf_bots_shared_data, true, "BF1943/Server",
                    "Bots: start from the shared bot data (the official servers' merged learning: the set shipped with "
                    "the game, or on an official server the latest one); off: this server's own learning only");
REXCVAR_DEFINE_BOOL(bf_bots_share, false, "BF1943/Server",
                    "Bots (official servers; the server manager sets it): hand what is learned here to the shared set");
REXCVAR_DEFINE_INT32(bf_bots_share_minutes, 15, "BF1943/Server",
                     "Bots: minutes between two hand-offs of what was learned to the shared set");
REXCVAR_DEFINE_INT32(bf_bots_share_min_players, 3, "BF1943/Server",
                     "Bots: a hand-off to the shared set waits until this many different human players have played the "
                     "map since the last one (never one player's routes alone)");

namespace bf1943 {
std::string CurrentMapTitle();  // server_rotation.cpp: the map now ("" without a rotation)
}  // namespace bf1943
using bf1943::CurrentMapTitle;

REX_EXTERN(sub_82387EF0);  // the player manager's factory (manager, flag): a new ServerPlayer, or 0 when full
// The player manager's removal (manager, player): a player out of its list (+84), its id and slot freed, and the
// player deleted (its vtable +4). What the server does to a player who left, about 30 s after (run bfstudy3: a
// human's ServerPlayer deleted from 82388114, inside it). Backfill's way out for a bot.
REX_EXTERN(sub_82387FA0);
// The scoring's "player left" (scoring, player): its record of the player gone. A connection's teardown
// (sub_823A9EF0) calls it just before sub_82387FA0; a bot left without it kept a record of a deleted player, and the
// next round's scoring walk crashed the server (run levelchg3: sub_82402F00 > sub_82391098, a read past the heap).
REX_EXTERN(sub_82410950);
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

// What the bots cost the server's tick (TekRantGaming, 20 Nov 2026: "These bots are quite heavy on the CPU"): the time
// in BotsTick and its parts, logged every 30 s while bots play.
struct BotCost {
  double total = 0, worst = 0, learn = 0, scan = 0, think = 0, drive = 0, save = 0, input_step = 0;
  int ticks = 0, searches = 0;
  Clock::time_point since{};
} cost;
struct CostTimer {
  double* into;
  Clock::time_point t0 = Clock::now();
  explicit CostTimer(double* d) : into(d) {}
  ~CostTimer() { *into += std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }
};


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
  return bf1943::GuestCommittedReadable(memory, address, size);  // guest_readable.h (was a QueryRegionInfo walk)
}

#if !defined(_WIN32)
// Linux: the guest's 4 GB as the process maps it (/proc/self/maps), readable ranges only, in guest addresses (read at
// each call: the level's scans and its kits' checks, a few times a level).
std::vector<std::pair<uint64_t, uint64_t>> GuestHostRanges(const uint8_t* base) {
  std::vector<std::pair<uint64_t, uint64_t>> ranges;
  std::ifstream maps("/proc/self/maps");
  std::string line;
  const uint64_t b = reinterpret_cast<uint64_t>(base);
  while (std::getline(maps, line)) {
    uint64_t lo = 0, hi = 0;
    char perms[8] = {};
    if (std::sscanf(line.c_str(), "%llx-%llx %7s", reinterpret_cast<unsigned long long*>(&lo),
                    reinterpret_cast<unsigned long long*>(&hi), perms) != 3 || perms[0] != 'r')
      continue;
    if (hi <= b || lo >= b + 0x100000000ull) continue;
    ranges.push_back({std::max<uint64_t>(lo, b) - b, std::min<uint64_t>(hi, b + 0x100000000ull) - b});
  }
  return ranges;
}
#endif

// Guest memory committed in the host's view (the level's data, outside the heaps Readable knows).
bool Mapped(const uint8_t* base, uint32_t address, uint32_t size) {
#if defined(_WIN32)
  MEMORY_BASIC_INFORMATION mbi{};
  if (address < 0x10000 || !VirtualQuery(base + address, &mbi, sizeof(mbi))) return false;
  return mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
         uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base) + mbi.RegionSize >= uint64_t(address) + size;
#else
  // One byte of each page read through the kernel (dev_guest_memory.h: process_vm_readv fails on memory that is not
  // mapped or not readable), about a microsecond. Until 27 Nov 2026 every call read and parsed the whole of
  // /proc/self/maps (GuestHostRanges, 250+ lines): the player and flag names of each tick made it 3 to 5 ms of the
  // server's tick with the bots waiting and the most of its main thread with them playing (US Central, perf: 81% of
  // the thread in GuestHostRanges). Windows had the same fix for its VirtualQuery on 20 Nov (squad_colors.cpp).
  // (From the Linux server optimisation, branch linux-server-optim b30ae988.)
  if (address < 0x10000) return false;
  return bf1943::dev::GuestReadable(base, address, size);
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
  bool kit_warned = false;  // "no live kit" logged (once until it has one again)
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
  Clock::time_point melee_until{};  // hitting something in its way with the knife (a breakable fence) until then
  // Driving a tank (TankTick): its waypoint, when it last checked it was moving, the gun's next shot.
  float tank_way[3] = {}, tank_check_pos[3] = {};
  Clock::time_point tank_think{}, tank_check_at{}, tank_fire_at{}, tank_turn_until{}, tank_reverse_until{};
  float tank_turn = 0;
  int tank_goal = -1;
  // Flying a plane (PlaneTick): where it was a moment ago (its speed), whether it has left the ground.
  float plane_last[3] = {};
  float plane_start_y = 0;  // its height where it got in (the take-off's reference when the ground ray finds nothing)
  Clock::time_point plane_last_at{};
  float plane_speed = 0;
  bool plane_flying = false;
  int melee_tries = 0;              // knife attempts at this spot in a row
  float check_pos[3] = {};
  Clock::time_point check_at{};
  float aim_error_yaw = 0, aim_error_pitch = 0;
  // Its own take on the players' style: each trait times 0.75 to 1.25 (sprint, range, burst, sights, attack, flank).
  float jitter[6] = {1, 1, 1, 1, 1, 1};
  int kit_choice = -1;     // its kit for the next spawn (the players' kits by how often they took them)
  bool ads = false;        // aiming down the sights in this fight
  bool sprinter = true;    // sprints on its way to this goal
  Clock::time_point next_ride{};  // when it looks for a vehicle to ride in next
  // Riding (Ride): the vehicle it sits in, where it got on, where the vehicle last moved and when, when its driver's
  // seat went empty, and the enter/exit button (digital concept 13, B: run boatbtn2) held until press_until.
  uint32_t ride = 0;
  // A place in a vehicle where the soldier keeps its own weapon (a landing craft's standing places): the game leaves
  // the soldier as what the player controls (+312), so it is found by the vehicle's seats instead (StandingIn).
  uint32_t standing = 0;
  int standing_seat = -1, standing_tries = 0;
  Clock::time_point standing_check{};
  float ride_from[3] = {}, ride_last[3] = {};
  // Where the vehicle was 5 and 10 s ago (its way over 10 s): a boat grinding in the surf moves a little all the
  // time, so "not moved 0.75 m" never came for it (27 Nov 2026).
  float ride_was5[3] = {}, ride_was10[3] = {};
  Clock::time_point ride_win_at{};
  bool ride_slow = false;  // under 4 m in the last 10 s
  Clock::time_point ride_at{}, ride_moved{}, no_driver_since{}, press_until{}, next_press{};
  uint32_t off_vehicle = 0;          // the vehicle it just got off: not boarded again until off_until
  Clock::time_point off_until{};
  // Boats (TekRantGaming, 20 Nov 2026: "id like them to use the boats and also know how to get in and out of them"):
  // the boat it walks to (to drive it, or ride in it with a bot of its side driving), until boat_until; a driver's
  // way to the beach (BoatRoute), how long it waits for passengers, and its backing off something it ran into.
  uint32_t boat = 0;
  Clock::time_point boat_until{}, boat_check{}, boat_wait_until{}, reverse_until{}, boat_log{};
  std::vector<std::array<float, 3>> boat_path;
  int reverses = 0;
  int presses = 0;  // enter presses at the boat it walks to (the game refusing it: given up after 4)
  Clock::time_point life_at{};  // when its soldier spawned: boats are for the first 45 s of a life, once
  bool boated = false;
  bool paused = false;            // waiting: no human on the server (BotsTick)
  uint64_t test_bits = 0;         // digital concepts a test sets this tick (BF_BOT_WEAPONTEST), bit c for concept c
  // Its weapons (Weapons): the slot in its hands (0 the first, as it spawns), a slot asked for (SelectWeapon1 to 4,
  // concepts 30 to 33) until select_until, no shot before switch_done (the swap takes a moment), its heavy weapon's next
  // shot, and this life's heavy shots by kind (at, rifle grenade, thrown).
  int weapon_slot = 0, select_slot = -1;
  Clock::time_point select_until{}, switch_done{}, heavy_next{}, heavy_fire_until{};
  int heavy_shots[3] = {};
  bool leaving = false;           // backfill: a player needs its slot; it leaves when dead (now, or when it next dies)
  bool evening = false;           // leaving to even the teams (with leaving): a bot joins the other side in its place
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
  // Bailing out (BailCheck): the vehicle whose health it watches, its health then and when, how fast it falls (a
  // second), whether it is getting out (since when), and when it opens its parachute (out of a plane).
  uint32_t hp_vehicle = 0;
  float hp_last = 0, hp_rate = 0;
  Clock::time_point hp_at{}, hp_since{}, bail_since{}, stay_log{};
  // Out of a plane: watched 20 s (until chute_until) for its fall; a press (chute_press, which buttons) while it
  // still falls fast, until chute_press_until; its height and when, for the speed.
  Clock::time_point chute_until{}, chute_next{}, chute_press_until{}, chute_y_at{}, chute_log{};
  float chute_y = 0, fall_speed = 0;
  int chute_tries = 0;
  bool bailing = false;
  uint32_t target_vehicle = 0;  // the enemy vehicle its tank's gun is on (TankTick), 0 none
  // Pressing on (its side's aggression, Pressure): a flank point on the way to its flag, a leader's regroup short of
  // the flag it attacks, its last word of an enemy seen, and whether its goal's place was moved off its flag.
  float flank_pos[3] = {};
  int flank_goal = -1, regroup_goal = -1;
  bool flank_done = false, goal_moved_off = false;
  Clock::time_point regroup_until{}, contact_at{};
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
  std::set<uint32_t> team_data;  // the live TeamData objects (their constructors and destructor), for the bots' kits
  std::vector<Bot> bots;
  Clock::time_point level_at{};
  bool level_seen = false;
  uint32_t manager = 0, scanned_manager = 0;
  std::map<uint32_t, std::vector<uint32_t>> team_kits;  // team (1 US, 2 Japan) -> its kit objects
  std::vector<Flag> flags;
  bool sit_out = false;  // a level bots cannot play (SitsOut): none of them on it
  std::set<uint32_t> vehicle_spawns;  // spawn points that seat their soldier in a vehicle
  // The level's vehicles (ServerVehicleEntity, vtable 0x82013068: its constructor sub_8230A580 and destructor
  // sub_822FF5B0 hooked below). Every tank, jeep, boat, plane and fixed gun the game treats as a vehicle
  // (sub_822F1CF8, the game's own "is a vehicle" test, compares its type with 0x82D55EF0, that class's).
  std::set<uint32_t> vehicles;
  std::set<uint32_t> ended_vehicles;  // vehicles destroyed since (their addresses), until one is made there again
  // Squads (the user, 19 Nov 2026: "All bot squad-leaders should be able to issue commands and play the map tactically,
  // moving with their squad to capture objectives"): each squad's leader by (side, squad), and a bot leader's order
  // for its squad: the flag, whether to hold it, and until when (made in BotsTick's squads every 2 s).
  struct Order {
    int flag = -1;
    bool defend = false;
    Clock::time_point until{};
  };
  std::map<std::pair<int, uint32_t>, uint32_t> leaders;
  std::map<std::pair<int, uint32_t>, Order> orders;
  std::set<std::string> boat_names;  // the level's boats by name (IsBoat: "LCVP"), for the routes learned in them
  std::map<uint32_t, Clock::time_point> stranded;  // boats a bot left stuck (a reef, the lagoon): not taken until then
  uint32_t ray_scratch = 0;  // guest memory for the ray test's vectors (16 byte aligned)
  int searches_left = 0;     // path searches this tick may still make (kSearchesPerTick)
  uint8_t* base = nullptr;   // guest memory (BotsTick), for BotsLevelChange
  int humans = 0;            // human players on the server
  bool making = false;       // MakeBot is seating a new bot (its team pick is the bot's, not a real player's)
  Clock::time_point evened_at{};  // the last bot sent to even the teams (one every 10 s at most)
  uint32_t soldier_data_vtable = 0;  // the kits' soldier data's class (LiveKit), from the first level's kits
  uint64_t rays = 0;         // ray tests made, and the time they took (logged every 10 s)
  double ray_ms = 0;
} g;

// The pressure on each side's bots (the user, 22 Nov 2026: "if players against the bots are doing much better, bots
// ramp up their aggression and tactics, working with other bots to try and win against the players"): the humans'
// kills of a side's bots against that side's bots' kills of humans, and the flags each side holds, as they stand over
// the last few minutes (the kills fade: half gone in 2 minutes). A side's aggression (0 to 1) follows it, full in
// 90 s at the most, down again over 3 minutes: with the humans ahead by about 0.15 or less (in kills, (theirs - ours) /
// (theirs + ours + 4), and flags, (theirs - ours) / flags, weighted 0.6 each) it is 0; it is 1 at 0.75. Only against
// sides with humans on them (bf_bots_dynamic_aggression). What it does is in Think, TankTick's boarding and the
// squads' orders: at 0.25 the bots tell each other where they saw an enemy (Contacts, for 5 s), and they attack more
// than they defend; at 0.5 bot-led squads share an attack flag, a leader waits short of it for its squad (12 s at
// most), two in three bots come at a flag from the side, a third tank may be taken; at 0.6 a bot hunts an enemy
// another saw within 45 m. All along it fights closer, aims up to 30% better, sprints more and pauses less.
struct Pressure {
  double human_kills[3] = {}, bot_kills[3] = {};  // by the bots' side (1 US, 2 Japan)
  float level[3] = {}, target[3] = {};
  int quarter[3] = {};    // the level's last logged quarter
  bool rallied[3] = {};   // a leader called the push (at 0.5), until it falls to 0.25
  Clock::time_point at{};
  struct Contact {
    float pos[3];
    Clock::time_point at;
  };
  std::vector<Contact> contacts[3];
} pressure;
float Aggression(int side) { return side == 1 || side == 2 ? pressure.level[side] : 0.0f; }

// Air Superiority (Coral Sea) is fought in planes, and bots play on foot: they spawned on the carriers, found no way
// and died there, at 3 ms a tick for 10 of them (run coral_red, 22 Nov 2026). They sit such a level out and come back
// on the next. Told by the rotation's title; without a rotation by the mode's own flag (only then: the scan of guest
// memory can still find the last level's).
bool SitsOut(const std::string& title, const std::vector<Flag>& flags) {
  if (!title.empty()) return title == "Coral Sea";
  for (const Flag& f : flags)
    if (f.name == "CQ_MP_AIRSUPERIORITY") return true;
  return false;
}

int WantedBots() {
  static const int env = [] {
    const char* v = std::getenv("BF_BOTS");
    return v && *v ? std::atoi(v) : -1;
  }();
  return std::clamp(env >= 0 ? env : REXCVAR_GET(bf_bots), 0, bf1943::kMaxServerSize);  // (the room is the server size's)
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
  if (begin == end || end < begin || end - begin > uint32_t(bf1943::kPlayerListLimit) * 4 || !Readable(begin, end - begin))
    return out;
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
std::string DataText(const uint8_t* base, uint32_t s);

// A data object's name: its asset path (+0xC..+0x10, as a kit's), else a C string at +0xC (as a vehicle's data's).
std::string DataName(const uint8_t* base, uint32_t obj) {
  if (!Readable(obj, 0x14)) return {};
  std::string n = GuestText(base, Load32(base, obj + 0xC), Load32(base, obj + 0x10));
  if (n == "?" || n.empty()) n = DataText(base, Load32(base, obj + 0xC));
  for (char c : n)
    if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) return {};
  return n;
}

// A kit's weapons (the user, 23 Nov 2026: "bots should be able to use grenades and switch weapons too, such as using
// the bazooka to shoot at tanks or low flying planes, or the rifle grenade launch to hit infantry at far"): its
// soldier data's (SoldierEntityData, vtable 0x8205AD34) Weapons list at +0x1B8 (26 Nov 2026; the class's field
// table at 0x82D3F198; MeleeWeaponIndex +0x13C), in slot order, each weapon's kind told by its name. A slot is picked with SelectWeapon1 to
// 4 (concepts 30 to 33: run weap1, the game took 31 and 32 at once; Y, the pad's swap, is 9 and 19).
struct KitWeapons {
  int at = -1, gl = -1, thrown = -1;  // the slots of its anti-tank weapon, rifle grenade and thrown explosive (-1 none)
  std::string text;                    // for the log
};
std::unordered_map<uint32_t, KitWeapons> kit_weapons;  // by soldier data; forgotten at a level change
const KitWeapons& WeaponsOf(const uint8_t* base, uint32_t data) {
  static const KitWeapons none;
  if (!Readable(data, 0x1C0)) return none;
  if (const auto it = kit_weapons.find(data); it != kit_weapons.end()) return it->second;
  KitWeapons k;
  // The list is at +0x1B8 (26 Nov 2026, run fx_wakeB's probe of every kit's soldier data: "+1B8 counted 3: Thompson,
  // US_Pineapple_Gren, M18"). +0x1A8, read until then, held the same six words for
  // every kit and a count of 2191647088, so no bot ever found its anti-tank weapon (TekRantGaming, 26 Nov 2026: "if I
  // am in a tank bots shoot at me rather than using their class equipment correctly").
  // Its count is the word at +0x1B0 (+0x1B4 the room for them): 4 on every kit, the knife last (run fx_kits1). The word
  // before the list held 3 by chance on three kits and garbage on the others.
  const uint32_t list = Load32(base, data + 0x1B8);
  const uint32_t count = Load32(base, data + 0x1B0);
  const uint32_t n = count >= 1 && count <= 8 && Readable(list, 4 * count) ? count : 0;
  for (uint32_t i = 0; i < n && Readable(list + 4 * i, 4); ++i) {
    const uint32_t w = Load32(base, list + 4 * i);
    std::string name = DataName(base, w);
    if (name.empty() && Readable(w, 0x18)) name = DataName(base, Load32(base, w + 0x14));
    std::string low = name;
    for (char& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
    const std::string leaf = low.substr(low.rfind('/') == std::string::npos ? 0 : low.rfind('/') + 1);
    const char* kind = "";
    const bool rifle_grenade = leaf.find("_rgl") != std::string::npos || leaf.find("riflegrenade") != std::string::npos ||
                               leaf.find("rifle_grenade") != std::string::npos || leaf.find("grenadelauncher") != std::string::npos;
    if (leaf.find("bazooka") != std::string::npos || leaf.find("rocket") != std::string::npos || leaf == "m18" ||
        leaf.rfind("m18_", 0) == 0 || leaf.find("type4") != std::string::npos || leaf.find("antitank") != std::string::npos ||
        leaf.find("type99_at") != std::string::npos) {
      if (k.at < 0 && i < 4) k.at = int(i);
      kind = " (anti-tank)";
    } else if (rifle_grenade) {
      if (k.gl < 0 && i < 4) k.gl = int(i);
      kind = " (rifle grenade)";
    } else if (leaf.find("gren") != std::string::npos || leaf.find("dynamite") != std::string::npos) {
      if (k.thrown < 0 && i < 4) k.thrown = int(i);
      kind = " (thrown)";
    }
    k.text += fmt::format(" {} '{}'{}", i, name.empty() ? fmt::format("{:08X}", w) : name, kind);
  }
  k.text += fmt::format("; {} listed, melee index {}", count, int32_t(Load32(base, data + 0x13C)));
  // A list not readable yet (three of Wake's six kits at the level's start, run fx_tank1) is read again later: kept
  // as known only once it holds weapons, each soldier data logged once either way.
  static std::unordered_map<uint32_t, int> logged;  // data -> 1 empty logged, 2 weapons logged
  int& said = logged[data];
  if (n == 0) {
    if (said == 0) {
      said = 1;
      REXLOG_INFO("BF bots: weapons of soldier data {:08X} not readable yet:{}", data, k.text);
#if defined(BF_DEV_TOOLS)
      std::string dump;
      for (uint32_t o = 0x180; o < 0x1E0; o += 4) dump += fmt::format(" +{:X}:{:08X}", o, Load32(base, data + o));
      REXLOG_INFO("BF bots: soldier data {:08X} words:{}", data, dump);
      if (Readable(list - 8, 32)) {
        std::string at;
        for (int o = -8; o < 24; o += 4) {
          const uint32_t w = Load32(base, uint32_t(int64_t(list) + o));
          std::string nm = DataName(base, w);
          if (nm.empty() && Readable(w, 0x18)) nm = DataName(base, Load32(base, w + 0x14));
          at += fmt::format(" {:+d}:{:08X}{}", o, w, nm.empty() ? "" : "'" + nm + "'");
        }
        REXLOG_INFO("BF bots: soldier data {:08X} +1B8 -> {:08X}:{}", data, list, at);
      }
#endif
    }
    static KitWeapons empty;
    empty = k;
    return empty;
  }
  if (said < 2) {
    said = 2;
    REXLOG_INFO("BF bots: weapons of soldier data {:08X}:{}", data, k.text);
  }
  return kit_weapons.emplace(data, k).first->second;
}

// The vehicle a player sits in, and its seat: its controlled entity (+312) when that is not its soldier; the vehicle's
// name from its data object (+0x14, its name at +0xC: native_input.cpp, run kb_probe_g2), the seat the entry the
// player holds (+292: 0 the driver's). "" on foot.
std::string VehicleName(const uint8_t* base, uint32_t vehicle);
bool IsTank(const std::string& name);  // the Sherman and the Japanese tanks, by name (below)
std::string VehicleKey(const uint8_t* base, uint32_t player, uint32_t* vehicle_out = nullptr) {
  const uint32_t so = Load32(base, player + 288), ctl = Load32(base, player + 312);
  if (!ctl || ctl == so || !Readable(ctl, 0x20)) return {};
  // The vehicle's name as KindOf has it (read once per vehicle type: a character at a time through Mapped read the
  // process's memory map 40 times a sample on Linux, and some names came out as their first letter, "C|driver").
  std::string name = VehicleName(base, ctl);
  if (name.empty()) name = "vehicle";
  const uint32_t seat = Load32(base, player + 292);
  if (vehicle_out) *vehicle_out = ctl;
  return name + (seat == 0 ? "|driver" : "|seat" + std::to_string(seat < 16 ? seat : 15));
}

// A player's name: the eastl string at +0x18 (team_balance.cpp's NameOf).
std::string PlayerName(const uint8_t* base, uint32_t player) {
  return GuestText(base, Load32(base, player + 0x18), Load32(base, player + 0x1C));
}

// A bot's name is its own, with no tag (TekRantGaming, 20 Nov 2026, after the scoreboards' ping: "update all bots to no
// longer use the bot tag, because we'll just be showing your version of the bot tag within the ping window"; until then
// every bot wore "[BOT]"). Players' games tell a bot from a player by the match host's list of its joined players
// (world_sync.cpp IsBot): a scoreboard row not on it is a bot, and its ping cell says BOT.

// A new name for a bot of this team (up to 15 letters), none of the names taken.
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
// An entity's heading (the way its forward axis, the transform's third row, points), as a bot's yaw: forward =
// (-sin yaw, 0, cos yaw).
bool Heading(PPCContext& outer, uint8_t* base, uint32_t entity, float* yaw) {
  PPCContext call = outer;
  call.r3.u64 = entity;
  sub_822A7BB0(call, base);
  const uint32_t m = call.r3.u32;
  if (!Readable(m, 64)) return false;
  const float fx = LoadF(base, m + 32), fz = LoadF(base, m + 40);
  if (fx * fx + fz * fz < 1e-4f) return false;
  *yaw = std::atan2(-fx, fz);
  return true;
}
// The same, read once a server tick (the optimization pass: every bot's think read every soldier's and vehicle's
// place again through the game, thousands of calls a second); TickPositions() empties it each tick.
std::unordered_map<uint32_t, std::array<float, 3>> tick_positions;
void TickPositions() { tick_positions.clear(); }
bool CachedPosition(PPCContext& outer, uint8_t* base, uint32_t entity, float out[3]) {
  if (const auto it = tick_positions.find(entity); it != tick_positions.end()) {
    std::memcpy(out, it->second.data(), 12);
    return true;
  }
  if (!Position(outer, base, entity, out)) return false;
  tick_positions[entity] = {out[0], out[1], out[2]};
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
// A C string of the game's data, 40 characters at most. Readable takes 4 byte aligned addresses only (a name read a
// character at a time stopped at its first: "L" for the LCVP, US Central 20 Nov), so the aligned span is checked once;
// a string outside the heaps (the level's data) is checked as mapped.
std::string DataText(const uint8_t* base, uint32_t s) {
  std::string out;
  if (!Readable(s & ~3u, 48) && !Mapped(base, s, 40)) return out;
  for (uint32_t i = 0; i < 40; ++i) {
    const char c = char(base[s + i]);
    if (!c) break;
    out += c;
  }
  return out;
}
// A vehicle's name and whether it is a boat, by its data (+0x14; its name at +0xC, VehicleKey's; its class at +0x80,
// an object whose name comes first: "Light_Boat" for the landing craft, run vach_scan1 of 18 Nov 2026). Kept by the
// data object; forgotten at a level change.
struct VehicleKind {
  std::string name;
  bool boat = false;
};
std::unordered_map<uint32_t, VehicleKind> vehicle_kinds;
const VehicleKind& KindOf(const uint8_t* base, uint32_t vehicle) {
  static const VehicleKind none;
  const uint32_t data = Readable(vehicle, 0x18) ? Load32(base, vehicle + 0x14) : 0;
  if (!Readable(data, 0x84)) return none;
  const auto it = vehicle_kinds.find(data);
  if (it != vehicle_kinds.end()) return it->second;
  VehicleKind k;
  for (char c : DataText(base, Load32(base, data + 0xC))) k.name += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
  const std::string cls = DataText(base, Load32(base, data + 0x80));
  k.boat = cls.find("Boat") != std::string::npos || cls.find("boat") != std::string::npos;
  REXLOG_INFO("BF bots: vehicle '{}', class '{}'{}", k.name, cls, k.boat ? ": a boat" : "");
  return vehicle_kinds.emplace(data, k).first->second;
}
std::string VehicleName(const uint8_t* base, uint32_t vehicle) { return KindOf(base, vehicle).name; }
bool IsBoat(const uint8_t* base, uint32_t vehicle) {
  const VehicleKind& k = KindOf(base, vehicle);
  if (k.boat) g.boat_names.insert(k.name);
  return k.boat;
}
// A vehicle's health: the least of its zones' (0 to 1; -1 not known). A ServerVehicleEntity keeps it at +244, made in
// its vtable +136 (sub_82315A58) from its data's (+20) front, rear, left and right VehicleHealthZoneData (+0x160,
// +0x1A0, +0x1E0, +0x200; the class's own field table, VehicleEntityData's at 0x82D3F900) and its parts': a vector
// at +20..+24 of 12 byte zones, each its health now (+0) and its zone data (+4: MaxHealth at +16, the field table at
// 0x82D3F6C0). A hit or a repair changes +0 (sub_822F2988: clamped to 0 and MaxHealth). Found by reading the code,
// 22 Nov 2026.
float VehicleHealth(const uint8_t* base, uint32_t vehicle) {
  if (!vehicle || !Readable(vehicle, 248)) return -1.0f;
  const uint32_t h = Load32(base, vehicle + 244);
  if (!Readable(h, 32)) return -1.0f;
  const uint32_t b = Load32(base, h + 20), e = Load32(base, h + 24);
  if (!b || e <= b || (e - b) % 12 || e - b > 16 * 12 || !Readable(b, e - b)) return -1.0f;
  float least = 2.0f;
  for (uint32_t z = b; z < e; z += 12) {
    const uint32_t data = Load32(base, z + 4);
    if (!Readable(data, 20)) continue;
    const float now = LoadF(base, z), most = LoadF(base, data + 16);
    if (!std::isfinite(now) || !std::isfinite(most) || most <= 0.0f) continue;
    least = std::min(least, std::clamp(now / most, 0.0f, 1.0f));
  }
  return least > 1.5f ? -1.0f : least;
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
#else
  // Linux (US Central's test of 20 Nov 2026: every bot joined with no kit and never spawned, the scan having found
  // nothing): the process's own map of committed, readable memory within the guest's 4 GB (GuestHostRanges).
  for (const auto& [start, end] : GuestHostRanges(base)) {
    const bool image = start >= 0x80000000ull && start < 0xA0000000ull;
    if (!image && end > start) fn(start, end);
  }
#endif
}

// The teams' kits, from the level's TeamData objects (a scan of guest memory: about half a second, once a level; on a
// thread of its own since the optimization pass, LevelScan).
void ScanTeams(const uint8_t* base, std::map<uint32_t, std::vector<uint32_t>>& team_kits) {
  team_kits.clear();
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
      team_kits[team] = kits;
    }
  });
  REXLOG_INFO("BF bots: kits found in {:.0f} ms", std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
}

// The level's capture points: their data objects, then the server's entities that point at them.
constexpr uint32_t kFlagDataVtable = 0x8205CF60;
void ScanFlags(const uint8_t* base, std::vector<Flag>& flags) {
  flags.clear();
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
      flags.push_back(f);
      datas.insert(a);
    }
  });
  if (datas.empty()) return;
  ForEachGuestRange(base, [&](uint64_t start, uint64_t end) {
    for (uint64_t a64 = start; a64 + 0x100 <= end; a64 += 4) {
      const uint32_t a = uint32_t(a64);
      const uint32_t d = Load32(base, a + 0x14);
      if (!datas.count(d) || !InImage(Load32(base, a))) continue;
      for (Flag& f : flags)
        if (f.data == d && !f.entity) f.entity = a;
    }
  });
  // The level's data is in memory more than once (run bots_s1a: three copies); the server's entities point at one.
  flags.erase(std::remove_if(flags.begin(), flags.end(), [](const Flag& f) { return !f.entity; }), flags.end());
  for (const Flag& f : flags) {
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
// The optimization pass (TekRantGaming, 20 Nov 2026: "make sure they don't hammer the system whilst they're in use"):
// the rays' time budgets a server tick (60 a second): the level's scan 0.5 ms while people play (a full scan of Wake
// takes several minutes then, once a map: it is saved), 2 ms on an empty server; the path searches' rays 0.6 ms; and
// at most kSearchesPerTick path searches a tick.
constexpr double kScanMsPlayed = 0.5, kScanMsAlone = 2.0, kPathRayMs = 0.6;
constexpr int kSearchesPerTick = 2;
constexpr int kBackfillSpare = 2;  // slots the bots leave free for joining players (backfill)

// Each cell keeps its own 8 steps (the optimization pass, 20 Nov 2026: the steps' own map held 2.8 million entries
// on Wake, about 100 MB, and its growth stalled the server's tick by up to 250 ms): a bit a direction for open and
// for closed, and for a step a ray found closed (a soldier may have stood there) the time it is tried again from.
struct NavCell {
  float y = 0;
  uint8_t state = 0;  // 0 not known, 1 walkable, 2 not walkable, 3 swimmable (water; y its surface)
  uint8_t open = 0, closed = 0, retry = 0;  // steps by direction (bit d): open, closed, closed by a ray (tried again)
  float retry_at = 0;  // Nav seconds from which its ray closed steps are tried again
};
struct Nav {
  std::unordered_map<uint64_t, NavCell> cells;
  Clock::time_point epoch = Clock::now();
  Clock::time_point ray_deadline{};  // the rays' time budget: none after it this tick
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
float NavNow() { return std::chrono::duration<float>(Clock::now() - nav.epoch).count(); }
bool RaysSpent() { return nav.rays_left <= 0 || Clock::now() > nav.ray_deadline; }
// A step's key (StepKey: the cell's key shifted by 3, the direction in the low 3) back to its cell: x keeps 29 bits.
int StepCellX(uint64_t k) { return int32_t(uint32_t(k >> 35) << 3) >> 3; }
int StepCellZ(uint64_t k) { return int(uint32_t(k >> 3)); }
void SetStep(int ix, int iz, int d, bool open, float retry_at = -1.0f) {
  NavCell& c = nav.cells[CellKey(ix, iz)];
  const uint8_t bit = uint8_t(1 << d);
  if (open) {
    c.open |= bit, c.closed &= uint8_t(~bit), c.retry &= uint8_t(~bit);
  } else {
    c.closed |= bit, c.open &= uint8_t(~bit);
    if (retry_at >= 0) c.retry |= bit, c.retry_at = std::max(c.retry_at, retry_at);
  }
}
int CellOf(float v) { return int(std::floor(v / kCell)); }
float CellCenter(int i) { return (float(i) + 0.5f) * kCell; }
constexpr int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, kDz[8] = {0, 0, 1, -1, 1, -1, 1, -1};
constexpr int kBack[8] = {1, 0, 3, 2, 7, 6, 5, 4};  // the opposite direction

// The cell's ground near the height y_ref: a ray from 1.6 m above (over the highest step, under a shelter's roof:
// from 3 m, run bots_s1e's spawns under a roof found the roof) down to 5 m below. False: out of this tick's budget.
bool ProbeCell(PPCContext& ctx, uint8_t* base, uint32_t soldier, int ix, int iz, float y_ref, NavCell* out) {
  auto it = nav.cells.find(CellKey(ix, iz));
  if (it != nav.cells.end() && it->second.state != 0) {
    *out = it->second;
    return true;
  }
  if (RaysSpent()) return false;
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
  NavCell& slot = nav.cells[CellKey(ix, iz)];  // (its steps, if any were known, kept)
  slot.y = c.y, slot.state = c.state;
  *out = slot;
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
// A cell a bot swims in: the sea's (state 3), or one walked as ground that is under the water. A swimmer's walk was
// learned as ground (LearnWalk before the water's height was known, and LoadLearned makes every walked cell ground):
// on Wake 1,310 such cells 1.3 m under the surface drew lines of "land" across the lagoon and out from the carriers,
// routes the bots took into the water (TekRantGaming, 26 Nov 2026: "a pool of bots in the middle of the map in the
// water"). Read when used, so it holds whenever the water's height becomes known.
// Each level's water height, as players' games read it from the level's water mesh (shader_prep.cpp), by the map's file
// name (MapFileName); -1e9 for one not known (found when a bot is first near the water, NoteWater).
float KnownWater(const std::string& map) {
  if (map == "Wake_Island") return 215.0f;
  if (map == "Guadalcanal") return 173.0f;
  if (map == "Iwo_Jima") return 5.3f;
  if (map == "Coral_Sea") return 61.5f;
  return -1e9f;
}
bool SwimCell(const NavCell& c) { return c.state == 3 || (c.state == 1 && c.y < nav.water - 0.9f); }
// How much more a swum step costs than a walked one: swimming is slow, and a swimmer cannot shoot (TekRantGaming,
// 26 Nov 2026: bots "try to swim over to the other side of the island instead of correctly traveling the island or
// taking a boat"). At 3 the 160 m swim across Wake's lagoon beat its 612 m walk round; at 10 a swim wins only where the
// walk round is ten times longer (a short channel).
constexpr float kSwimCost = 10.0f;

// The step from cell a (ground ya) to its neighbour in direction d. 0: out of budget, 1 open, 2 closed.
// The ray test hits soldiers too (its filter keeps out no character: run bots_s1g, every step out of a bot's own cell
// closed), so a step out of the searching bot's own cell (own) goes by the ground alone and is not kept, and a
// closed step is tried again 20 s on (a soldier who stood there has moved).
int Step(PPCContext& ctx, uint8_t* base, uint32_t soldier, int ix, int iz, float ya, int d, bool own = false) {
  const uint8_t bit = uint8_t(1 << d);
  if (!own) {
    const auto it = nav.cells.find(CellKey(ix, iz));
    if (it != nav.cells.end()) {
      const NavCell& a = it->second;
      if (a.open & bit) return 1;
      if ((a.closed & bit) && (!(a.retry & bit) || NavNow() < a.retry_at)) return 2;
    }
  }
  NavCell b;
  if (!ProbeCell(ctx, base, soldier, ix + kDx[d], iz + kDz[d], ya, &b)) return 0;
  int r = 2;
  const float rise = b.y - ya, limit = d < 4 ? kMaxStep : kMaxStep * 1.3f;
  const bool passable = b.state == 1 || b.state == 3;
  if (own) return passable && std::fabs(rise) <= limit ? 1 : 2;
  bool by_ray = false;
  if (passable && std::fabs(rise) <= limit) {
    if (RaysSpent()) return 0;
    --nav.rays_left;
    const float ax = CellCenter(ix), az = CellCenter(iz), bx = CellCenter(ix + kDx[d]), bz = CellCenter(iz + kDz[d]);
    const float h = std::max(ya, b.y) + 0.9f;  // knee to chest of the higher end
    const float from[3] = {ax, h, az}, to[3] = {bx, h, bz};
    float frac = 1.0f;
    r = Ray(ctx, base, soldier, from, to, &frac) && frac < 0.99f ? 2 : 1;
    by_ray = r == 2;
  }
  // Closed by the ground (too steep, no ground, a low roof) for good; closed by a ray (a wall, or a soldier who stood
  // there) tried again 20 s on.
  SetStep(ix, iz, d, r == 1, by_ray ? NavNow() + 20.0f : -1.0f);
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

// The layers (the shared bot data, 26 Nov 2026). `own` is this server's own learning, its file bots/<map>.txt as
// before. `learn`, what the bots use, is own + the shared set (the official servers' merged learning, SharedPick) -
// `sent`, what of own an official server already handed to the shared set (bots/sent/<map>.txt: in the shared set
// already, or soon), so nothing counts twice. Every new thing learned goes into both learn and own (Note).
Learned own, sent;
template <class F>
void Note(F&& f) {
  f(learn), f(own);
  learn.dirty = true;
}

std::string MapFileName(const std::string& map) {
  std::string name;
  for (char c : map) name += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
  return name;
}

std::filesystem::path BotsDir() {
  const std::filesystem::path root = REXCVAR_GET(user_data_root);
  return root.empty() ? std::filesystem::path() : root / "bots";
}

std::filesystem::path LearnedFile() {
  if (BotsDir().empty() || learn.map.empty()) return {};
  return BotsDir() / (MapFileName(learn.map) + ".txt");
}

// The program's own folder (the shipped set is in its data/bots/, as the packages lay it out).
std::filesystem::path ProgramDir() {
#if defined(_WIN32)
  wchar_t p[MAX_PATH * 2] = {};
  const DWORD n = GetModuleFileNameW(nullptr, p, DWORD(std::size(p)));
  return n ? std::filesystem::path(p).parent_path() : std::filesystem::path();
#else
  std::error_code ec;
  const std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path() : p.parent_path();
#endif
}

// A shared set's version: its version.txt's "version <n>" (server/botdata.py writes it last), 0 without one.
int64_t SharedVersion(const std::filesystem::path& dir) {
  std::ifstream f(dir / "version.txt");
  std::string word;
  long long v = 0;
  if (!(f >> word >> v) || word != "version" || v < 0) return 0;
  return v;
}

// The shared set the bots start from: the network's (bots/network/, an official server's host keeps it up to date)
// or the one shipped with the program (data/bots/), the newer; none with bf_bots_shared_data off.
struct SharedPick {
  std::filesystem::path dir;
  int64_t version = 0;
  const char* from = "";
} shared_pick;
SharedPick PickShared() {
  SharedPick best;
  if (!REXCVAR_GET(bf_bots_shared_data)) return best;
  const std::filesystem::path program = ProgramDir();
  const std::pair<std::filesystem::path, const char*> places[] = {
      {BotsDir().empty() ? std::filesystem::path() : BotsDir() / "network", "the official servers' latest"},
      {program.empty() ? std::filesystem::path() : program / "data" / "bots", "shipped with the game"}};
  for (const auto& [dir, from] : places) {
    if (dir.empty()) continue;
    const int64_t v = SharedVersion(dir);
    if (v > best.version) best = SharedPick{dir, v, from};
  }
  return best;
}

int CellX(uint64_t key) { return int(uint32_t(key >> 32)); }
int CellZ(uint64_t key) { return int(uint32_t(key)); }

// Steps' keys hold the cell in their upper bits (the cell key shifted by 3, the direction in the low 3).
uint64_t StepKey(int ix, int iz, int d) { return (CellKey(ix, iz) << 3) ^ uint64_t(d); }

// One store in the files' text form (the header line, then a line per entry; read back by ReadLearned).
void WriteLearned(std::ostream& f, const Learned& learn, const std::string& header) {
  f << header << '\n';
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
      if (c.second > 0)
        f << "vc " << key << ' ' << CellX(k) << ' ' << CellZ(k) << ' ' << c.first / c.second << ' ' << c.second << '\n';
    for (const auto& [k, w] : v.kills_at) f << "vk " << key << ' ' << CellX(k) << ' ' << CellZ(k) << ' ' << w << '\n';
  }
}

// One store from a file, every value checked (bots-learning.md, 26 Nov 2026: a weight of -1 or less, as a host's edit
// could leave, made the route search's costs -inf or NaN): finite and not negative, cells within 2 million metres,
// names short; anything else is left out. False when there is no such file.
bool ReadLearned(const std::filesystem::path& file, Learned& out) {
  if (file.empty()) return false;
  std::ifstream f(file);
  if (!f) return false;
  const auto fine = [](double v) { return std::isfinite(v) && v >= 0 && v < 1e9; };
  const auto cell_ok = [](int x, int z) { return std::abs(x) <= (1 << 20) && std::abs(z) <= (1 << 20); };
  const auto name_ok = [](const std::string& s) { return !s.empty() && s.size() <= 64; };
  // A step's x as WriteLearned writes it: CellX(key >> 3) keeps 29 bits, so a cell left of 0 comes out as x + 2^29
  // (StepKey of either gives the same key). Back to its own value, for the checks.
  const auto step_x = [](int x) {
    x &= 0x1FFFFFFF;
    return x >= (1 << 28) ? x - (1 << 29) : x;
  };
  std::string line;
  std::getline(f, line);
  size_t lines = 0;
  while (std::getline(f, line) && ++lines <= 8000000) {
    std::istringstream in(line);
    std::string kind;
    in >> kind;
    int x = 0, z = 0, d = 0, side = 0;
    float v = 0;
    if (kind == "c" && (in >> x >> z >> v)) {
      if (cell_ok(x, z) && std::isfinite(v) && std::fabs(v) < 1e5f) out.walk_y[CellKey(x, z)] = v;
    } else if (kind == "s" && (in >> x >> z >> d >> v) && d >= 0 && d < 8) {
      x = step_x(x);
      if (cell_ok(x, z) && fine(v)) out.step_use[StepKey(x, z, d)] = v;
    } else if ((kind == "k" || kind == "d" || kind == "w") && (in >> side >> x >> z >> v) && (side == 1 || side == 2)) {
      auto& m = kind == "k" ? out.kills[side - 1] : kind == "d" ? out.deaths[side - 1] : out.waits[side - 1];
      if (cell_ok(x, z) && fine(v)) m[CellKey(x, z)] = v;
    } else if (kind == "a") {
      std::string flag;
      if (in >> flag >> x >> z >> d >> v && d >= 0 && d < 8 && name_ok(flag) && cell_ok(x = step_x(x), z) && fine(v))
        out.ways_in[flag][StepKey(x, z, d)] = v;
    } else if (kind == "vt" || kind == "vc" || kind == "vk") {
      std::string key;
      double a = 0, b = 0, c = 0;
      if (!(in >> key) || !name_ok(key)) continue;
      if (kind == "vt" && (in >> a >> b >> c) && fine(a) && fine(b) && fine(c)) {
        auto& vu = out.vehicles[key];
        vu.time = a, vu.kills = b, vu.kill_dist = c;
      }
      if (kind == "vc" && (in >> x >> z >> a >> b) && cell_ok(x, z) && std::isfinite(a) && std::fabs(a) < 1e5 &&
          fine(b) && b > 0)
        out.vehicles[key].cells[CellKey(x, z)] = {float(a * b), float(b)};
      if (kind == "vk" && (in >> x >> z >> a) && cell_ok(x, z) && fine(a))
        out.vehicles[key].kills_at[CellKey(x, z)] = float(a);
    }
  }
  return true;
}

// dst += sign * src, entry by entry (the walked ground's heights are not counts: only added where dst has none).
void AddInto(std::unordered_map<uint64_t, float>& dst, const std::unordered_map<uint64_t, float>& src, float sign) {
  for (const auto& [k, w] : src) dst[k] += sign * w;
}
void Combine(Learned& dst, const Learned& src, float sign) {
  if (sign > 0)
    for (const auto& [k, y] : src.walk_y) dst.walk_y.emplace(k, y);
  AddInto(dst.step_use, src.step_use, sign);
  for (int s = 0; s < 2; ++s) {
    AddInto(dst.kills[s], src.kills[s], sign);
    AddInto(dst.deaths[s], src.deaths[s], sign);
    AddInto(dst.waits[s], src.waits[s], sign);
  }
  for (const auto& [flag, steps] : src.ways_in) AddInto(dst.ways_in[flag], steps, sign);
  for (const auto& [key, v] : src.vehicles) {
    auto& d = dst.vehicles[key];
    d.time += sign * v.time, d.kills += sign * v.kills, d.kill_dist += sign * v.kill_dist;
    for (const auto& [k, c] : v.cells) {
      auto& dc = d.cells[k];
      dc.first += sign * c.first, dc.second += sign * c.second;
    }
    AddInto(d.kills_at, v.kills_at, sign);
  }
}

// What is left after a subtraction: nothing below zero (float sums), no empty entries.
void Tidy(Learned& l) {
  const auto tidy = [](std::unordered_map<uint64_t, float>& m) {
    for (auto it = m.begin(); it != m.end();) it = it->second > 1e-4f ? std::next(it) : m.erase(it);
  };
  tidy(l.step_use);
  for (int s = 0; s < 2; ++s) tidy(l.kills[s]), tidy(l.deaths[s]), tidy(l.waits[s]);
  for (auto it = l.ways_in.begin(); it != l.ways_in.end();) {
    tidy(it->second);
    it = it->second.empty() ? l.ways_in.erase(it) : std::next(it);
  }
  for (auto it = l.vehicles.begin(); it != l.vehicles.end();) {
    auto& v = it->second;
    v.time = std::max(0.0, v.time), v.kills = std::max(0.0, v.kills), v.kill_dist = std::max(0.0, v.kill_dist);
    for (auto c = v.cells.begin(); c != v.cells.end();) c = c->second.second > 1e-4f ? std::next(c) : v.cells.erase(c);
    tidy(v.kills_at);
    it = v.time <= 1e-4 && v.cells.empty() && v.kills_at.empty() ? l.vehicles.erase(it) : std::next(it);
  }
}

// What `now` holds beyond `before`: the counts' differences, the walked cells `before` has not.
Learned Delta(const Learned& now, const Learned& before) {
  Learned d = now;
  d.walk_y.clear();
  for (const auto& [k, y] : now.walk_y)
    if (!before.walk_y.count(k)) d.walk_y.emplace(k, y);
  Combine(d, before, -1.0f);
  Tidy(d);
  return d;
}

size_t Entries(const Learned& l) {
  size_t n = l.walk_y.size() + l.step_use.size();
  for (int s = 0; s < 2; ++s) n += l.kills[s].size() + l.deaths[s].size() + l.waits[s].size();
  for (const auto& [f, steps] : l.ways_in) n += steps.size();
  for (const auto& [k, v] : l.vehicles) n += 1 + v.cells.size() + v.kills_at.size();
  return n;
}

void SaveStyle();
// The bots' files are written on a thread of their own (the optimization pass, 20 Nov 2026: a save on the server's
// tick wrote hundreds of thousands of lines and stalled it), one at a time; the tick only takes a copy (of own, this
// server's own learning: the shared set is never written into it).
std::mutex save_mutex;
void SaveLearned() {
  const std::filesystem::path file = LearnedFile();
  if (file.empty()) return;
  own.map = learn.map;
  auto snap = std::make_shared<Learned>(own);
  learn.dirty = false, learn.saved_at = Clock::now();
  SaveStyle();
  std::thread([file, snap] {
    std::lock_guard lock(save_mutex);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path tmp = file.string() + ".new";
    {
      std::ofstream f(tmp, std::ios::trunc);
      if (!f) return;
      WriteLearned(f, *snap, "BF1943 Reborn bots, what they learned on " + snap->map + " (1)");
    }
    std::filesystem::rename(tmp, file, ec);
  }).detach();
}

// A file written whole (beside, then renamed over the old: a reader never sees half of it).
void WriteWhole(const std::filesystem::path& file, const std::string& text) {
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  const std::filesystem::path tmp = file.string() + ".new";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << text;
  }
  std::filesystem::rename(tmp, file, ec);
}

// "BGR2" from 0.4.8.1. "BGR1" (0.4.8): its servers kept the last level's water height over a level change, so only
// Wake Island's (the first level of every set, its water found on it) are good; the others are left out and the level
// is mapped again.
constexpr uint32_t kGridMagic = 0x42475232, kGridMagicOld = 0x42475231;

// An official server's level grid for the shared set (a scan finished here, not yet handed): a copy into the outbox,
// once per grid (bots/sent/<map>.grid.txt keeps the size handed).
void ShareGrid(const std::string& map) {
  if (!REXCVAR_GET(bf_bots_share) || BotsDir().empty() || map.empty()) return;
  const std::string name = MapFileName(map);
  const std::filesystem::path grid = BotsDir() / (name + ".grid"), marker = BotsDir() / "sent" / (name + ".grid.txt");
  std::thread([grid, marker, name] {
    std::lock_guard lock(save_mutex);
    std::error_code ec;
    const auto size = std::filesystem::file_size(grid, ec);
    if (ec || size < 16) return;
    uint32_t head[2] = {};
    {
      std::ifstream g(grid, std::ios::binary);
      if (!g.read(reinterpret_cast<char*>(head), 8) || head[0] != kGridMagic || head[1] != 1) return;  // finished, 0.4.8.1's only
    }
    std::string handed;
    {
      std::ifstream m(marker);
      std::getline(m, handed);
    }
    if (handed == std::to_string(size)) return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::filesystem::path out = grid.parent_path() / "outbox" / (name + "." + std::to_string(ms) + ".grid");
    std::filesystem::create_directories(out.parent_path(), ec);
    std::filesystem::copy_file(grid, out.string() + ".new", std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return;
    std::filesystem::rename(out.string() + ".new", out, ec);
    if (!ec) WriteWhole(marker, std::to_string(size) + "\n");
  }).detach();
}

// What earlier matches learned on this map: this server's own, the shared set over it (less what of own is in it
// already), into the store, and the walked ground into the grid.
void LoadLearned(const std::string& map) {
  learn = Learned{}, own = Learned{}, sent = Learned{};
  learn.map = own.map = sent.map = map;
  learn.saved_at = Clock::now();
  const std::string name = MapFileName(map);
  if (!ReadLearned(LearnedFile(), own))
    REXLOG_INFO("BF bots: nothing learned on {} yet ({})", map, LearnedFile().string());
  // What of own went to the shared set (official servers): never more than own holds (a file of own deleted).
  if (!BotsDir().empty() && ReadLearned(BotsDir() / "sent" / (name + ".txt"), sent)) {
    const Learned beyond = Delta(own, sent);
    Learned capped = own;
    Combine(capped, beyond, -1.0f);
    for (const auto& [k, y] : beyond.walk_y) capped.walk_y.erase(k);
    Tidy(capped);
    capped.map = map;
    sent = std::move(capped);
  }
  Learned shared;
  shared_pick = PickShared();
  const bool have_shared = !shared_pick.dir.empty() && ReadLearned(shared_pick.dir / (name + ".txt"), shared);
  learn = own;
  learn.map = map;
  learn.saved_at = Clock::now();
  if (have_shared) {
    Combine(learn, shared, 1.0f);
    Combine(learn, sent, -1.0f);
    Tidy(learn);
    REXLOG_INFO("BF bots: the shared bot data {} ({}) on {}: {} entries (this server's own: {}, of it handed already: {})",
                shared_pick.version, shared_pick.from, map, Entries(shared), Entries(own), Entries(sent));
  } else if (!shared_pick.dir.empty()) {
    REXLOG_INFO("BF bots: the shared bot data {} ({}) has nothing on {}", shared_pick.version, shared_pick.from, map);
  }
  nav.cells.reserve(nav.cells.size() + learn.walk_y.size());
  for (const auto& [k, y] : learn.walk_y) {
    NavCell& c = nav.cells[k];
    c.y = y, c.state = 1;
  }
  for (const auto& [k, w] : learn.step_use) SetStep(StepCellX(k), StepCellZ(k), int(k & 7), true);
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
  ShareGrid(map);
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
    if (!own.walk_y.count(cell)) own.walk_y[cell] = y, learn.dirty = true;  // (own: this server's file, Note)
    if (!learn.walk_y.count(cell)) learn.walk_y[cell] = y;
    if (cx == px && cz == pz) continue;
    for (int d = 0; d < 8; ++d) {
      if (kDx[d] != cx - px || kDz[d] != cz - pz) continue;
      const uint64_t there = StepKey(px, pz, d);
      SetStep(px, pz, d, true), SetStep(cx, cz, kBack[d], true);
      Note([&](Learned& l) { l.step_use[there] += weight; });
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
// The shared set's style (the official servers' players, merged: SharedPick's style.txt), over this server's own;
// style_handed: what of this match's style an official server handed to the shared set already.
Style style_net, style_handed;

// A measure from all three: this server's long term, this match, the shared set.
double StyleSum(double Style::*f) { return style_long.*f + style_match.*f + style_net.*f; }

// A trait from the long term and this match together, with a prior (what a player does before anything is known)
// worth `prior_weight` of the measure's own unit.
double StyleRatio(double Style::*num, double Style::*den, double prior, double prior_weight) {
  const double n = StyleSum(num), d = StyleSum(den);
  return (n + prior * prior_weight) / (d + prior_weight);
}
double SprintShare() { return StyleRatio(&Style::sprint, &Style::time, 0.45, 120); }
double StillShare() { return StyleRatio(&Style::still, &Style::time, 0.2, 120); }
double AdsShare() { return StyleRatio(&Style::ads, &Style::fire, 0.5, 20); }
double BurstLength() { return StyleRatio(&Style::burst_time, &Style::bursts, 0.4, 10); }
double KillDistance() { return StyleRatio(&Style::kill_dist, &Style::kills, 35.0, 3); }
double FlankShare() { return StyleRatio(&Style::flank_kills, &Style::kills, 0.2, 5); }
double AttackShare() {
  const double a = StyleSum(&Style::attack), d = StyleSum(&Style::defend);
  return (a + 0.65 * 120) / (a + d + 120);
}
int StyleKit() {
  double w[3], total = 0;
  for (int k = 0; k < 3; ++k) total += w[k] = style_long.kit[k] + style_match.kit[k] + style_net.kit[k] + 1.0;  // never none
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

// A style file's fields into s, each times `scale`; false without the file. Values checked: finite, not negative.
bool ReadStyle(const std::filesystem::path& file, Style& s, double scale) {
  if (file.empty()) return false;
  std::ifstream f(file);
  if (!f) return false;
  std::string line;
  std::getline(f, line);
  while (std::getline(f, line)) {
    std::istringstream in(line);
    std::string name;
    double v = 0;
    if (!(in >> name >> v) || !std::isfinite(v) || v < 0 || v > 1e12) continue;
    v *= scale;
    for (const auto& [n, field] : kStyleFields)
      if (name == n) s.*field = v;
    for (int k = 0; k < 3; ++k)
      if (name == "kit" + std::to_string(k)) s.kit[k] = v;
  }
  return true;
}

void LoadStyle() {
  style_long = Style{}, style_match = Style{}, style_net = Style{}, style_handed = Style{};
  // The shared set's (SharedPick, chosen as the level's learning loaded): as merged, its own fading done there.
  if (!shared_pick.dir.empty() && ReadStyle(shared_pick.dir / "style.txt", style_net, 1.0))
    REXLOG_INFO("BF bots: the shared bot data's style: {:.0f} min of the official servers' players", style_net.time / 60);
  const bool mine = ReadStyle(StyleFile(), style_long, 0.7);  // an older match counts less
  if (!mine && style_net.time <= 0) return;
  REXLOG_INFO("BF bots: the players' style so far ({:.0f} min measured): sprint {:.0f}%, still {:.0f}%, sights {:.0f}% of "
              "fire, bursts {:.2f} s, kills at {:.0f} m, {:.0f}% from the side or back, attacking {:.0f}% of the time",
              (style_long.time + style_net.time) / 60, SprintShare() * 100, StillShare() * 100, AdsShare() * 100,
              BurstLength(), KillDistance(), FlankShare() * 100, AttackShare() * 100);
}

// The hand-off to the shared set (official servers, bf_bots_share; the server manager sets it). Every
// bf_bots_share_minutes and at each level change: what this server learned on the map since the last hand-off
// (own - sent) into bots/outbox/<map>.<ms>.txt, and this match's style since the last one into
// bots/outbox/style.<ms>.txt; the host (server/gmhost.py) sends them to the main backend and deletes them. Only once
// bf_bots_share_min_players different human players have played the map since the last hand-off: a hand-off is never
// one player's routes (the privacy page). Their names are counted here in memory only (hashes), never written.
Clock::time_point shared_at{};
std::map<std::string, std::unordered_set<size_t>> share_players;  // map -> the humans who played it since a hand-off
std::set<std::pair<std::string, uint32_t>> share_noted;           // (map, player) counted already (each looked up once)
void NoteSharePlayer(const uint8_t* base, uint32_t player) {
  if (!REXCVAR_GET(bf_bots_share) || learn.map.empty() || !share_noted.insert({learn.map, player}).second) return;
  const std::string name = PlayerName(base, player);
  if (!name.empty()) share_players[learn.map].insert(std::hash<std::string>{}(name));
}

void ShareLearned(const std::string& why) {
  shared_at = Clock::now();
  if (!REXCVAR_GET(bf_bots_share) || learn.map.empty() || BotsDir().empty()) return;
  auto& players = share_players[learn.map];
  const int need = std::max(0, REXCVAR_GET(bf_bots_share_min_players));
  if (int(players.size()) < need) {
    if (REXCVAR_GET(bf_bots_log_detail))
      REXLOG_INFO("BF bots: nothing handed to the shared set yet ({}): {} of {} players on {}", why, players.size(), need,
                  learn.map);
    return;
  }
  own.map = learn.map;
  auto delta = std::make_shared<Learned>(Delta(own, sent));
  Style sd;
  for (const auto& [n, field] : kStyleFields) sd.*field = std::max(0.0, style_match.*field - style_handed.*field);
  for (int k = 0; k < 3; ++k) sd.kit[k] = std::max(0.0, style_match.kit[k] - style_handed.kit[k]);
  const size_t entries = Entries(*delta);
  if (entries == 0 && sd.time < 1) return;
  players.clear();
  share_noted.clear();
  sent = own;
  style_handed = style_match;
  auto snap_sent = std::make_shared<Learned>(sent);
  const std::string name = MapFileName(learn.map), map = learn.map;
  const std::filesystem::path dir = BotsDir();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();
  std::thread([=] {
    std::lock_guard lock(save_mutex);
    if (Entries(*delta)) {
      std::ostringstream text;
      WriteLearned(text, *delta, "BF1943 Reborn bots, what they learned on " + map + " (1)");
      WriteWhole(dir / "outbox" / (name + "." + std::to_string(ms) + ".txt"), text.str());
    }
    if (sd.time >= 1) {
      std::ostringstream text;
      text << "BF1943 Reborn bots, the human players' style (1)\n";
      for (const auto& [n, field] : kStyleFields) text << n << ' ' << sd.*field << '\n';
      for (int k = 0; k < 3; ++k) text << "kit" << k << ' ' << sd.kit[k] << '\n';
      WriteWhole(dir / "outbox" / ("style." + std::to_string(ms) + ".txt"), text.str());
    }
    // What is handed now counts as sent: the next shared set holds it (bots/sent/, read at the next level load).
    std::ostringstream text;
    WriteLearned(text, *snap_sent, "BF1943 Reborn bots, handed to the shared set on " + map + " (1)");
    WriteWhole(dir / "sent" / (name + ".txt"), text.str());
  }).detach();
  REXLOG_INFO("BF bots: handed to the shared set ({}): {} entries learned on {}, {:.0f} s of the players' style", why,
              entries, map, sd.time);
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
  struct Row {
    int32_t x, z;
    float y;
    uint8_t state, open, closed;
  };
  auto rows = std::make_shared<std::vector<Row>>();
  rows->reserve(nav.cells.size());
  for (const auto& [k, c] : nav.cells)
    if (c.state != 0) rows->push_back(Row{CellX(k), CellZ(k), c.y, c.state, c.open, uint8_t(c.closed & ~c.open)});
  const uint32_t done = nav.scan_done ? 1u : 0u;
  std::thread([file, rows, done] {
    std::lock_guard lock(save_mutex);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path tmp = file.string() + ".new";
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      if (!f) return;
      const uint32_t magic = kGridMagic;
      const uint64_t count = rows->size();
      f.write(reinterpret_cast<const char*>(&magic), 4);
      f.write(reinterpret_cast<const char*>(&done), 4);
      f.write(reinterpret_cast<const char*>(&count), 8);
      std::vector<char> out;
      out.reserve(rows->size() * 15);
      auto put = [&](const void* v, size_t n) { out.insert(out.end(), static_cast<const char*>(v), static_cast<const char*>(v) + n); };
      for (const Row& r : *rows) put(&r.x, 4), put(&r.z, 4), put(&r.y, 4), put(&r.state, 1), put(&r.open, 1), put(&r.closed, 1);
      f.write(out.data(), std::streamsize(out.size()));
    }
    std::filesystem::rename(tmp, file, ec);
  }).detach();
}

bool LoadGridFile(const std::filesystem::path& file, const char* what) {
  if (file.empty()) return false;
  std::ifstream f(file, std::ios::binary);
  if (!f) return false;
  uint32_t magic = 0, done = 0;
  uint64_t count = 0;
  f.read(reinterpret_cast<char*>(&magic), 4);
  f.read(reinterpret_cast<char*>(&done), 4);
  f.read(reinterpret_cast<char*>(&count), 8);
  if (!f || (magic != kGridMagic && magic != kGridMagicOld) || count > 20000000) return false;
  if (magic == kGridMagicOld && MapFileName(learn.map) != "Wake_Island") {
    REXLOG_INFO("BF bots: the level's grid ({}) left out: a 0.4.8 one, made with the last level's water height", what);
    return false;
  }
  nav.cells.reserve(nav.cells.size() + count);
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
    NavCell& have = nav.cells[k];
    if (have.state == 0) have.y = c.y, have.state = c.state;  // what was walked (already in) stands over the scan's
    nav.scanned.insert(k);
    have.open |= open;
    have.closed |= uint8_t(closed & ~have.open);
  }
  nav.scan_done = nav.scan_done || done != 0;
  REXLOG_INFO("BF bots: the level's grid loaded ({}): {} cells{}", what, count,
              done != 0 ? " (the whole level)" : " (a scan in part)");
  return true;
}

// This server's own grid; when it has none, or only a scan in part, the shared set's finished one (the official
// servers mapped the level already: no scan of its own needed here).
void LoadGrid() {
  LoadGridFile(GridFile(), "this server's own");
  if (!nav.scan_done && !shared_pick.dir.empty() && !learn.map.empty())
    LoadGridFile(shared_pick.dir / (MapFileName(learn.map) + ".grid"), shared_pick.from);
}

// Starts the scan from these places (each a cell and its ground's height).
void ScanFrom(const float at[3]) {
  nav.scan_min[0] = std::min(nav.scan_min[0], at[0] - 120.0f), nav.scan_max[0] = std::max(nav.scan_max[0], at[0] + 120.0f);
  nav.scan_min[1] = std::min(nav.scan_min[1], at[2] - 120.0f), nav.scan_max[1] = std::max(nav.scan_max[1], at[2] + 120.0f);
  const int x = CellOf(at[0]), z = CellOf(at[2]);
  const uint64_t k = CellKey(x, z);
  NavCell& c = nav.cells[k];
  if (c.state == 0) c.y = at[1], c.state = 1;
  if (nav.scanned.insert(k).second) nav.scan.push_back(k);
}

// One tick of the scan: cells taken from its queue, their 8 steps found (rays within its own budget), the new cells
// reached queued. False when there was nothing to do.
bool ScanTick(PPCContext& ctx, uint8_t* base, uint32_t soldier, double budget_ms) {
  if (nav.scan_done || nav.scan.empty() || !soldier) return false;
  nav.scan_rays_left = kScanRaysPerTick;
  // Its own time budget (the optimization pass: with players on the server a little each tick, alone more).
  const auto keep_deadline = nav.ray_deadline;
  nav.ray_deadline = Clock::now() + std::chrono::microseconds(int64_t(budget_ms * 1000));
  while (!nav.scan.empty() && Clock::now() <= nav.ray_deadline) {
    const uint64_t k = nav.scan.front();
    const auto c = nav.cells.find(k);
    if (c == nav.cells.end() || (c->second.state != 1 && c->second.state != 3)) {
      nav.scan.pop_front();
      continue;
    }
    const float cy = c->second.y;  // (Step may add cells and move the map's entries)
    // Step() spends the path budget: the scan lends it its own for the call.
    const int keep = nav.rays_left;
    nav.rays_left = nav.scan_rays_left;
    bool starved = false;
    for (int d = 0; d < 8; ++d) {
      const int st = Step(ctx, base, soldier, CellX(k), CellZ(k), cy, d);
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
  nav.ray_deadline = keep_deadline;
  if (nav.scan.empty()) {
    nav.scan_done = true;
    REXLOG_INFO("BF bots: the level is mapped: {} cells in {:.0f} s", nav.cells.size(),
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
      const float swim = SwimCell(c) ? kSwimCost : 1.0f;  // swimming: slow, and no shooting
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

// A kit a bot may spawn with: a live Kit object whose soldier data (+0x3C, what a player's +500 holds) is still that
// kind of object. US Central, 21 Nov 2026 (0.4.8-test13): after an hour, four restarts of Wake on the empty server
// (PreRound, "not enough players") and many players dropped, the US bots' spawns failed while the game built their
// soldier from that data (sub_8236B688 -> sub_82360E28 -> the soldier data's vtable +8, which was zero: the server's
// calls to 0 at 0x82354150); every game that joined then died at the load (a null call after sub_8220BA08) until the
// server restarted. Which object freed that data is not known yet; a bot no longer spawns from it (and the refusal
// logs say what it held). The soldier data's vtable is learnt from the level's kits at its start (g.soldier_data_vtable).
bool LiveKit(const uint8_t* base, uint32_t kit) {
  if (!kit || !Readable(kit, 0x40) || Load32(base, kit) != kKitVtable) return false;
  const uint32_t soldier = Load32(base, kit + 0x3C);
  if (!soldier || !Readable(soldier, 0x10)) return false;
  const uint32_t vtable = Load32(base, soldier);
  return g.soldier_data_vtable ? vtable == g.soldier_data_vtable : InImage(vtable);
}

// Each kit, its soldier data and that data's vtable (the level's start, a refused spawn).
std::string KitsText(const uint8_t* base, const std::map<uint32_t, std::vector<uint32_t>>& team_kits) {
  std::string s;
  for (const auto& [team, kits] : team_kits) {
    s += fmt::format(" team {}:", team);
    for (uint32_t k : kits) {
      const uint32_t data = Readable(k, 0x40) ? Load32(base, k + 0x3C) : 0;
      s += fmt::format(" {:08X}->{:08X}({:08X})", k, data, Readable(data, 4) ? Load32(base, data) : 0);
    }
  }
  return s;
}

// The kits of the live TeamData objects (their constructors' hooks below fill g.team_data, the destructor's empties
// it), by team: kits that are there for certain, where the scan of guest memory can also find a TeamData left in
// freed memory.
std::map<uint32_t, std::vector<uint32_t>> LiveTeamKits(const uint8_t* base, std::string* where) {
  std::vector<uint32_t> teams;
  {
    std::lock_guard lock(g.mutex);
    teams.assign(g.team_data.begin(), g.team_data.end());
  }
  std::map<uint32_t, std::vector<uint32_t>> kits;
  for (uint32_t a : teams) {
    if (!Readable(a, 0x38) || Load32(base, a) != kTeamDataVtable) continue;
    const uint32_t team = Load32(base, a + 0x34);
    if (team < 1 || team > 2) continue;
    const uint32_t list = Load32(base, a + 0x30);
    std::vector<uint32_t> found;
    for (int k = 0; k < 3 && Readable(list, 12); ++k)
      if (const uint32_t kit = Load32(base, list + 4 * k); LiveKit(base, kit)) found.push_back(kit);
    if (where) *where += fmt::format(" team {} {:08X}", team, a);
    if (!found.empty()) kits[team] = found;
  }
  return kits;
}

// The bot's kit for its team now (a bot the round's start moved to the other team gets that team's). False when the
// team has no live kit: nothing is written, and the bot does not spawn yet.
bool ChooseKit(uint8_t* base, Bot& bot) {
  const auto it = g.team_kits.find(Load32(base, bot.player + 48));
  if (it == g.team_kits.end() || it->second.empty()) return false;
  if (bot.kit_choice < 0) bot.kit_choice = StyleKit();
  const uint32_t kit = it->second[size_t(bot.kit_choice) % it->second.size()];
  if (!LiveKit(base, kit)) return false;
  Store32(base, bot.player + 500, Load32(base, kit + 0x3C));
  return true;
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
  // Its own user id: the online user at +0x18 (the name, then at +0x28 a 64 bit id; a connection's player gets its
  // signed in user, sub_8236F550, the test player's id was 11 in run endnames6). The end of round results find each
  // score record's player by that id (sub_823754B8 compares +0x28, sub_823C1648), so with every bot at 0 each bot's
  // row took the first bot's name ("[BOT]Joe Mitchell" for all eight, run endnames7; the user, 20 Nov 2026: "All the
  // bots on the end screen scoreboard had the same names"). Ids of their own before the score record is made below,
  // 0x00B07000 in the high word, which no account has (the backend's ids are small numbers).
  {
    static uint32_t next_id = 0;
    Store32(base, bot.player + 0x28, 0x00B07000u);
    Store32(base, bot.player + 0x2C, ++next_id);
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
  // Its thinking spread over the ticks: each bot a little after the one before.
  bot.next_think = Clock::now() + std::chrono::milliseconds((index * 37) % 250);
  bot.next_ride = Clock::now() + std::chrono::milliseconds((index * 53) % 500);
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

bool IsPlane(const std::string& name);
struct Enemy {
  uint32_t soldier;
  float pos[3];
  float dist;
  uint32_t player = 0;
};

// What the bot decides, 4 times a second: an enemy it sees to fight, else the flag to go for, and how to get there
// past what is in the way (rays ahead at waist and chest height, one down ahead for a drop).
// A boat's way from where it is to the beach nearest a flag, over the water human players drove boats on (the cells
// learned of every "<boat>|driver", Learned::vehicles): the learned cell nearest the flag is the landing (where the
// players beached), and the way there links cells up to 14 m apart (a sample each half second, 10 m apart at speed), shortest
// first. Points 15 m apart and more. Empty when nothing was learned near enough: the driver heads straight for the
// flag instead.
std::vector<std::array<float, 3>> BoatRoute(const float from[3], const float to[3]) {
  struct Node {
    float p[3];
  };
  std::vector<Node> nodes;
  for (const auto& [key, vu] : learn.vehicles) {
    const size_t bar = key.find('|');
    if (bar == std::string::npos || key.compare(bar, std::string::npos, "|driver") != 0 ||
        !g.boat_names.count(key.substr(0, bar)))
      continue;
    for (const auto& [k, c] : vu.cells)
      if (c.second > 0) nodes.push_back({{CellCenter(CellX(k)), c.first / c.second, CellCenter(CellZ(k))}});
  }
  if (nodes.size() < 10) return {};
  int start = -1, end = -1;
  float ds = 40.0f, de = 1e9f;
  for (int i = 0; i < int(nodes.size()); ++i) {
    const float a = Dist2D(nodes[i].p, from), b = Dist2D(nodes[i].p, to);
    if (a < ds) ds = a, start = i;
    if (b < de) de = b, end = i;
  }
  if (start < 0 || end < 0 || de > Dist2D(from, to) - 50.0f) return {};
  constexpr float kLink = 14.0f, kBucket = 15.0f;
  std::unordered_map<uint64_t, std::vector<int>> buckets;
  auto bucket = [&](float x, float z) {
    return (uint64_t(uint32_t(int(std::floor(x / kBucket)))) << 32) | uint32_t(int(std::floor(z / kBucket)));
  };
  for (int i = 0; i < int(nodes.size()); ++i) buckets[bucket(nodes[i].p[0], nodes[i].p[2])].push_back(i);
  std::vector<float> dist(nodes.size(), 1e30f);
  std::vector<int> prev(nodes.size(), -1);
  using Item = std::pair<float, int>;
  std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
  dist[start] = 0, open.push({0.0f, start});
  while (!open.empty()) {
    const auto [d, i] = open.top();
    open.pop();
    if (d > dist[i]) continue;
    if (i == end) break;
    const int bx = int(std::floor(nodes[i].p[0] / kBucket)), bz = int(std::floor(nodes[i].p[2] / kBucket));
    for (int ox = -1; ox <= 1; ++ox)
      for (int oz = -1; oz <= 1; ++oz) {
        const auto it = buckets.find((uint64_t(uint32_t(bx + ox)) << 32) | uint32_t(bz + oz));
        if (it == buckets.end()) continue;
        for (int j : it->second) {
          const float step = Dist2D(nodes[i].p, nodes[j].p);
          if (step > kLink || d + step >= dist[j]) continue;
          dist[j] = d + step, prev[j] = i;
          open.push({dist[j], j});
        }
      }
  }
  if (dist[end] >= 1e29f) return {};
  std::vector<std::array<float, 3>> way;
  for (int i = end; i >= 0; i = prev[i]) way.push_back({nodes[i].p[0], nodes[i].p[1], nodes[i].p[2]});
  std::reverse(way.begin(), way.end());
  std::vector<std::array<float, 3>> out;
  for (size_t i = 0; i < way.size(); ++i)
    if (i + 1 == way.size() || out.empty() || Dist2D(out.back().data(), way[i].data()) >= 15.0f) out.push_back(way[i]);
  return out;
}

// How far of its planned way a bot would swim: the way's legs walked in 2 m steps (a long straight leg, a swim from
// the carrier's side to the beach, has no point of its own on the water), each step on a cell the grid knows as water.
float SwimAhead(const Bot& bot, const float pos[3]) {
  float m = 0;
  float from[3] = {pos[0], pos[1], pos[2]};
  for (const auto& pt : bot.path) {
    const float len = Dist2D(from, pt.data());
    const int steps = std::min(1000, int(len / kCell));
    for (int i = 1; i <= steps; ++i) {
      const float t = float(i) / float(steps);
      const auto c = nav.cells.find(
          CellKey(CellOf(from[0] + (pt[0] - from[0]) * t), CellOf(from[2] + (pt[2] - from[2]) * t)));
      if (c != nav.cells.end() && SwimCell(c->second)) m += len / float(steps);
    }
    from[0] = pt[0], from[1] = pt[1], from[2] = pt[2];
  }
  return m;
}

// How much of the straight line to a place is sea: 2 m steps on cells the grid knows as water, or does not know at all
// (the open sea between a carrier and the island is never mapped: no spawn point or flag leads there; run boatdrive6,
// the US bots' planned ways crossed it and counted no swim).
float StraightWater(const float from[3], const float to[3]) {
  const float len = Dist2D(from, to);
  const int steps = std::min(2000, int(len / kCell));
  float m = 0;
  for (int i = 1; i <= steps; ++i) {
    const float t = float(i) / float(steps);
    const auto c = nav.cells.find(CellKey(CellOf(from[0] + (to[0] - from[0]) * t), CellOf(from[2] + (to[2] - from[2]) * t)));
    if (c == nav.cells.end() || SwimCell(c->second)) m += len / float(steps);
  }
  return m;
}

// The way to each flag over the whole level (TekRantGaming, 26 Nov 2026, Wake: bots swim across the lagoon and gather
// in the water in the middle). A bot's path search (FindPath) looks at kExpandPerSearch cells; Wake's walks round the
// lagoon need 14,000 to 31,000 (the saved grid of run size64h, searched offline), so a search toward a flag across
// the lagoon ended at the cell nearest the flag it had seen: the shore, then the water, toward the flag. Here, every
// few minutes, a snapshot of the grid is taken on the server's thread and a thread of its own works out, for each flag,
// every cell's cost to reach it (Dijkstra, walked steps 1, swum kSwimCost, as FindPath; learned costs left to it). A
// bot far from its flag then plans its usual search only to a point about 80 m along that way (field::Waypoint), which
// the search always reaches; near the flag it searches to it as before. A cell the field does not reach (a scan still
// under way, a bot on a carrier's deck) goes the old way.
namespace field {
constexpr int kStepsAhead = 40;            // the waypoint: 40 cells (80 m) along the way
constexpr float kFrom = 70.0f;             // used from this far from the flag
constexpr double kRebuildSeconds = 180.0;  // the grid learns as the match goes on
struct Snapshot {
  int x0 = 0, z0 = 0, w = 0, h = 0;
  std::vector<uint8_t> kind, open;  // kind 0 unknown, 1 ground, 2 blocked, 3 swum
  std::vector<float> y;
  std::vector<std::array<float, 3>> flags;
};
struct Fields {
  std::string map;
  int x0 = 0, z0 = 0, w = 0, h = 0;
  std::vector<uint8_t> open, kind;
  std::vector<float> y;
  std::vector<std::vector<float>> to_flag;  // by flag index: each cell's cost to reach it (+inf: not reached)
};
std::mutex mutex;
std::shared_ptr<const Fields> ready;  // the latest, for the bots
std::atomic<bool> busy{false};
std::atomic<int> generation{0};  // a level change: a build under way is thrown away
Clock::time_point built_at{};
std::string built_map;

float StepLen(int d) { return d < 4 ? kCell : kCell * 1.41421f; }

void Build(std::shared_ptr<Snapshot> snap, std::string map, int gen) {
  const auto t0 = Clock::now();
  auto out = std::make_shared<Fields>();
  out->map = map, out->x0 = snap->x0, out->z0 = snap->z0, out->w = snap->w, out->h = snap->h;
  const int w = snap->w, h = snap->h;
  const size_t n = size_t(w) * size_t(h);
  size_t reached_total = 0;
  for (const auto& f : snap->flags) {
    std::vector<float> dist(n, std::numeric_limits<float>::infinity());
    using Item = std::pair<float, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    // Seeds: the passable cells within 4 m of the pole (it may stand on a roof or in a hut), else within 12 m.
    for (float r : {4.0f, 12.0f}) {
      const int cr = int(std::ceil(r / kCell));
      const int fx = CellOf(f[0]) - snap->x0, fz = CellOf(f[2]) - snap->z0;
      for (int dz = -cr; dz <= cr; ++dz)
        for (int dx = -cr; dx <= cr; ++dx) {
          const int x = fx + dx, z = fz + dz;
          if (x < 0 || z < 0 || x >= w || z >= h || float(dx * dx + dz * dz) * kCell * kCell > r * r) continue;
          const uint32_t i = uint32_t(z) * uint32_t(w) + uint32_t(x);
          if (snap->kind[i] != 1 && snap->kind[i] != 3) continue;
          dist[i] = 0, open.push({0.0f, i});
        }
      if (!open.empty()) break;
    }
    size_t reached = 0;
    while (!open.empty()) {
      if (generation.load() != gen) return;  // the level changed
      const auto [d0, i] = open.top();
      open.pop();
      if (d0 > dist[i]) continue;
      ++reached;
      const int x = int(i % uint32_t(w)), z = int(i / uint32_t(w));
      for (int d = 0; d < 8; ++d) {
        const int mx = x + kDx[d], mz = z + kDz[d];
        if (mx < 0 || mz < 0 || mx >= w || mz >= h) continue;
        const uint32_t m = uint32_t(mz) * uint32_t(w) + uint32_t(mx);
        if (snap->kind[m] != 1 && snap->kind[m] != 3) continue;
        if (!(snap->open[m] & (1 << kBack[d]))) continue;  // the step from m into i
        const float c = d0 + StepLen(d) * (snap->kind[i] == 3 ? kSwimCost : 1.0f);
        if (c < dist[m]) dist[m] = c, open.push({c, m});
      }
    }
    reached_total += reached;
    out->to_flag.push_back(std::move(dist));
  }
  out->open = std::move(snap->open), out->kind = std::move(snap->kind), out->y = std::move(snap->y);
  if (generation.load() != gen) return;
  {
    std::lock_guard lock(mutex);
    ready = out;
  }
  busy = false;
  REXLOG_INFO("BF bots: the ways to {} flags worked out over {} x {} cells ({} reached in all) in {:.0f} ms",
              out->to_flag.size(), w, h, reached_total,
              std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
}

// The server's thread, each tick: a new build when there is none for this level or the last is kRebuildSeconds old.
void Tick(const std::string& map) {
  if (map.empty() || g.flags.empty() || nav.cells.size() < 1000 || nav.water < -1e8f || busy.load()) return;
  const auto now = Clock::now();
  if (map == built_map && now - built_at < std::chrono::duration<double>(kRebuildSeconds)) return;
  built_map = map, built_at = now;
  auto snap = std::make_shared<Snapshot>();
  int x0 = INT32_MAX, z0 = INT32_MAX, x1 = INT32_MIN, z1 = INT32_MIN;
  for (const auto& [k, c] : nav.cells) {
    if (c.state != 1 && c.state != 3) continue;
    const int x = CellX(k), z = CellZ(k);
    x0 = std::min(x0, x), z0 = std::min(z0, z), x1 = std::max(x1, x), z1 = std::max(z1, z);
  }
  if (x0 > x1 || int64_t(x1 - x0 + 1) * int64_t(z1 - z0 + 1) > 8000000) return;  // none, or a level far too big
  snap->x0 = x0, snap->z0 = z0, snap->w = x1 - x0 + 1, snap->h = z1 - z0 + 1;
  const size_t n = size_t(snap->w) * size_t(snap->h);
  snap->kind.assign(n, 0), snap->open.assign(n, 0), snap->y.assign(n, 0.0f);
  for (const auto& [k, c] : nav.cells) {
    if (c.state == 0) continue;
    const int x = CellX(k) - x0, z = CellZ(k) - z0;
    if (x < 0 || z < 0 || x >= snap->w || z >= snap->h) continue;
    const size_t i = size_t(z) * size_t(snap->w) + size_t(x);
    snap->kind[i] = c.state == 2 ? 2 : SwimCell(c) ? 3 : 1;
    snap->open[i] = c.open, snap->y[i] = c.y;
  }
  for (const Flag& f : g.flags) snap->flags.push_back({f.pos[0], f.pos[1], f.pos[2]});
  busy = true;
  std::thread(Build, snap, map, generation.load()).detach();
}

// A level change: what was worked out is the old level's.
void Reset() {
  ++generation;
  busy = false;
  built_map.clear();
  std::lock_guard lock(mutex);
  ready.reset();
}

// A point about 80 m along the best way from `pos` to flag `flag` (true), or nothing known (false).
bool Waypoint(int flag, const float pos[3], float out[3]) {
  std::shared_ptr<const Fields> f;
  {
    std::lock_guard lock(mutex);
    f = ready;
  }
  if (!f || flag < 0 || size_t(flag) >= f->to_flag.size()) return false;
  const std::vector<float>& dist = f->to_flag[size_t(flag)];
  auto index = [&](int x, int z) -> int64_t {
    x -= f->x0, z -= f->z0;
    return x < 0 || z < 0 || x >= f->w || z >= f->h ? -1 : int64_t(z) * f->w + x;
  };
  // The bot's cell, or the best of its neighbours (it may stand on a cell's edge, or one the grid has blocked).
  int cx = CellOf(pos[0]), cz = CellOf(pos[2]);
  int64_t cur = index(cx, cz);
  if (cur < 0 || !std::isfinite(dist[size_t(cur)])) {
    cur = -1;
    float best = std::numeric_limits<float>::infinity();
    for (int d = 0; d < 8; ++d) {
      const int ax = CellOf(pos[0]) + kDx[d], az = CellOf(pos[2]) + kDz[d];
      const int64_t m = index(ax, az);
      if (m >= 0 && dist[size_t(m)] < best) best = dist[size_t(m)], cur = m, cx = ax, cz = az;
    }
    if (cur < 0) return false;
  }
  for (int step = 0; step < kStepsAhead && dist[size_t(cur)] > 0.0f; ++step) {
    int64_t next = -1;
    float best = dist[size_t(cur)];
    int nx = cx, nz = cz;
    for (int d = 0; d < 8; ++d) {
      if (!(f->open[size_t(cur)] & (1 << d))) continue;
      const int64_t m = index(cx + kDx[d], cz + kDz[d]);
      if (m < 0 || dist[size_t(m)] >= best) continue;
      best = dist[size_t(m)], next = m, nx = cx + kDx[d], nz = cz + kDz[d];
    }
    if (next < 0) break;
    cur = next, cx = nx, cz = nz;
  }
  out[0] = CellCenter(cx), out[1] = f->y[size_t(cur)], out[2] = CellCenter(cz);
  return true;
}
}  // namespace field

void Think(PPCContext& ctx, uint8_t* base, Bot& bot, uint32_t soldier, const float pos[3],
           const std::vector<uint32_t>& listed, Clock::time_point now) {
  const int team = int(Load32(base, bot.player + 48));
  const float aggro = Aggression(team);
  const float aim_error = kAimError * (1.0f - 0.3f * aggro);
  const float eye[3] = {pos[0], pos[1] + 1.6f, pos[2]};
  // Enemies: every other team's living soldier in range, nearest first.
  std::vector<Enemy> enemies;
  for (uint32_t p : listed) {
    if (p == bot.player || !Readable(p, 0x140)) continue;
    const int t = int(Load32(base, p + 48));
    if (t == team || (t != 1 && t != 2)) continue;
    const uint32_t so = Load32(base, p + 288);
    if (!so || !Readable(so, 0x140)) continue;
    Enemy e{so, {}, 0, p};
    if (!CachedPosition(ctx, base, so, e.pos)) continue;
    e.dist = Dist2D(pos, e.pos);
    if (e.dist < kSightRange) enemies.push_back(e);
  }
  std::sort(enemies.begin(), enemies.end(), [](const Enemy& x, const Enemy& y) { return x.dist < y.dist; });
  // Sight: within the field of view (or close enough to hear), and nothing of the level between the eyes and the
  // enemy's chest (the ray stops short of the enemy so its own body does not count).
  const Enemy* seen = nullptr;
  int rays = 0;
  // An enemy in a tank (TekRantGaming, 26 Nov 2026: "if I am in a tank bots shoot at me rather than using their class
  // equipment correctly"): fought only with what hurts a tank, the kit's anti-tank weapon or its rifle grenade while it
  // has shots of it left this life. A bot with neither leaves the tank alone and goes on with its flag (its gun only
  // scratched the paint, and it stood still to do it).
  const KitWeapons& kit_w = WeaponsOf(base, Load32(base, bot.player + 500));
  const bool hurts_tanks = (kit_w.at >= 0 && bot.heavy_shots[0] < 5) || (kit_w.gl >= 0 && bot.heavy_shots[1] < 6);
  auto in_tank = [&](uint32_t player, uint32_t soldier_of) {
    const uint32_t c = player && Readable(player, 0x140) ? Load32(base, player + 312) : 0;
    return c && c != soldier_of && IsTank(VehicleName(base, c));
  };
  for (const Enemy& e : enemies) {
    if (rays >= 3) break;
    if (!hurts_tanks && in_tank(e.player, e.soldier)) continue;
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
    // Its word to the others of its side (a second apart at most): where it sees an enemy.
    if (fresh && aggro >= 0.25f && now - bot.contact_at > std::chrono::seconds(1) && (team == 1 || team == 2) &&
        pressure.contacts[team].size() < 64) {
      bot.contact_at = now;
      pressure.contacts[team].push_back({{seen->pos[0], seen->pos[1], seen->pos[2]}, now});
    }
    if (seen->soldier != bot.enemy) {
      bot.enemy = seen->soldier;
      bot.aim_error_yaw = (Rand01() - 0.5f) * 4 * aim_error, bot.aim_error_pitch = (Rand01() - 0.5f) * 2 * aim_error;
      if (REXCVAR_GET(bf_bots_log_detail)) REXLOG_INFO("BF bots: {} sees an enemy {:.0f} m away", bot.name, seen->dist);
    }
    const float chest[3] = {seen->pos[0], seen->pos[1] + 1.2f, seen->pos[2]};
    // The aim settles on the target: the error shrinks while it keeps the enemy in sight.
    const float settle = 0.7f - 0.15f * aggro;
    bot.aim_error_yaw *= settle, bot.aim_error_pitch *= settle;
    bot.want_yaw = YawTo(eye, chest) + bot.aim_error_yaw + (Rand01() - 0.5f) * aim_error;
    bot.want_pitch = PitchTo(eye, chest) + bot.aim_error_pitch + (Rand01() - 0.5f) * aim_error * 0.5f;
    // The range the players kill at (its own take on it): closer in when further, back off when much nearer;
    // side-steps as often as the players move rather than stand.
    const float range = std::clamp(float(KillDistance()) * bot.jitter[1] * (1.0f - 0.3f * aggro), 8.0f, 150.0f);
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
      bot.burst_pause_until = bot.burst_until + std::chrono::milliseconds(int((200 + Rand01() * 400) * (1.0f - 0.4f * aggro)));
    }
    bot.fire = now < bot.burst_until && now - bot.enemy_seen_at < std::chrono::milliseconds(500);
    // Its kit's heavier weapons (Weapons): the anti-tank weapon at an enemy in a vehicle within 150 m (a plane only
    // within 120 m and 40 m over the bot: low), the rifle grenade at an enemy on foot 35 to 140 m off, a thrown
    // explosive at one 8 to 25 m off that it has just lost behind something. Five, six and three a life at most. The
    // shot arcs: aimed up by the angle that carries it that far (at 70, 40 and 18 m/s); one shot, then 2.5 to 3.5 s.
    {
      const KitWeapons& kw = WeaponsOf(base, Load32(base, bot.player + 500));
      const uint32_t ctl = seen->player && Readable(seen->player, 0x140) ? Load32(base, seen->player + 312) : 0;
      const bool in_vehicle = ctl && ctl != seen->soldier;
      const bool plane = in_vehicle && IsPlane(VehicleName(base, ctl));
      int want = 0, kind = -1;
      const bool tank = in_vehicle && IsTank(VehicleName(base, ctl));
      if (kw.at >= 0 && in_vehicle && bot.heavy_shots[0] < 5 && seen->dist < (plane ? 120.0f : 150.0f) &&
          (!plane || seen->pos[1] - pos[1] < 40.0f))
        want = kw.at, kind = 0;
      else if (kw.gl >= 0 && tank && bot.heavy_shots[1] < 6 && seen->dist > 15.0f && seen->dist < 140.0f)
        want = kw.gl, kind = 1;  // a rifle grenade at a tank (26 Nov 2026), its hull's middle
      else if (kw.gl >= 0 && !in_vehicle && fresh && bot.heavy_shots[1] < 6 && seen->dist > 35.0f && seen->dist < 140.0f)
        want = kw.gl, kind = 1;
      else if (kw.thrown >= 0 && !in_vehicle && !fresh && bot.heavy_shots[2] < 3 && seen->dist > 8.0f && seen->dist < 25.0f)
        want = kw.thrown, kind = 2;
      if (want != bot.weapon_slot) {
        bot.weapon_slot = want, bot.select_slot = want;
        bot.select_until = now + std::chrono::milliseconds(250);
        bot.switch_done = now + std::chrono::milliseconds(900);
        if (REXCVAR_GET(bf_bots_log_detail))
          REXLOG_INFO("BF bots: {} takes weapon slot {} ({}, {:.0f} m)", bot.name, want + 1,
                      kind == 0 ? "anti-tank, at a vehicle" : kind == 1 ? "rifle grenade" : kind == 2 ? "thrown" : "its gun",
                      seen->dist);
      }
      if (kind >= 0) {
        static constexpr float kSpeed[3] = {70.0f, 40.0f, 18.0f};
        const float v = kSpeed[kind];
        const float arc = 0.5f * std::asin(std::min(1.0f, 9.81f * seen->dist / (v * v)));
        bot.want_pitch = PitchTo(eye, chest) + arc;
        if (in_vehicle) bot.want_pitch = PitchTo(eye, seen->pos) + arc;  // a vehicle: its middle
        bot.ads = false;
        bot.fire = now < bot.heavy_fire_until;
        const bool aimed = std::fabs(WrapAngle(bot.want_yaw - bot.yaw)) < 0.05f && std::fabs(bot.want_pitch - bot.pitch) < 0.05f;
        if (aimed && now >= bot.switch_done && now >= bot.heavy_next) {
          bot.heavy_fire_until = now + std::chrono::milliseconds(kind == 2 ? 400 : 250);
          bot.heavy_next = now + std::chrono::milliseconds(kind == 0 ? 3500 : kind == 1 ? 3000 : 2500);
          ++bot.heavy_shots[kind];
          bot.fire = true;
          if (REXCVAR_GET(bf_bots_log_detail))
            REXLOG_INFO("BF bots: {} fires its {} at {:.0f} m", bot.name,
                        kind == 0 ? "anti-tank weapon" : kind == 1 ? "rifle grenade" : "thrown explosive", seen->dist);
        }
      }
    }
    bot.sprint = false;
    bot.path.clear();
    return;
  }
  if (bot.enemy && REXCVAR_GET(bf_bots_log_detail)) REXLOG_INFO("BF bots: {} lost sight of its enemy", bot.name);
  bot.enemy = 0, bot.fire = false, bot.strafe = 0, bot.ads = false;
  bot.want_pitch = 0;
  if (bot.weapon_slot != 0 && now >= bot.switch_done) {  // the fight over: its gun again
    bot.weapon_slot = 0, bot.select_slot = 0;
    bot.select_until = now + std::chrono::milliseconds(250);
    bot.switch_done = now + std::chrono::milliseconds(900);
  }
  // The flag: the nearest one its team does not hold; with all held, the nearest one (to hold it). A bot keeps its
  // flag until its team holds it (no turning back and forth as others come and go), or 60 s at most.
  int best = -1;
  float best_d = 1e9f;
  const bool keep = bot.goal >= 0 && bot.goal < int(g.flags.size()) && now < bot.goal_until &&
                    FlagOwner(base, g.flags[bot.goal]) != team;
  if (keep) {
    best = bot.goal, best_d = Dist2D(pos, g.flags[best].pos);
  } else if (bot.goal_until < now && Rand01() > (AttackShare() + (1.0 - AttackShare()) * 0.6 * aggro) * bot.jitter[4]) {
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
  // Its squad (the squads' orders and leaders, BotsTick): a bot-led squad goes for its leader's order; a member far from
  // its leader (over 30 m, the leader nearer the flag) catches up with it first. In a player's squad: the flag the
  // player is at (within 50 m), else with the player.
  bool following = false;
  float follow[3] = {};
  {
    const uint32_t sq = Load32(base, bot.player + 52);
    const auto lead = sq < 24 ? g.leaders.find({team, sq}) : g.leaders.end();
    if (lead != g.leaders.end()) {
      const uint32_t leader = lead->second;
      float lp[3] = {};
      const uint32_t lso = Load32(base, leader + 288);
      const bool alive = lso && Readable(lso, 0x140) && Position(ctx, base, lso, lp);
      if (!IsBot(leader)) {
        if (alive) {
          int by_flag = -1;
          float by_d = 50.0f;
          for (size_t i = 0; i < g.flags.size(); ++i)
            if (g.flags[i].takeable && Dist2D(lp, g.flags[i].pos) < by_d) by_d = Dist2D(lp, g.flags[i].pos), by_flag = int(i);
          if (by_flag >= 0) best = by_flag, best_d = Dist2D(pos, g.flags[size_t(by_flag)].pos);
          else following = true, std::copy(lp, lp + 3, follow);
        }
      } else if (const auto order = g.orders.find({team, sq}); order != g.orders.end() && order->second.flag >= 0 &&
                                    size_t(order->second.flag) < g.flags.size()) {
        best = order->second.flag, best_d = Dist2D(pos, g.flags[size_t(best)].pos);
        if (leader != bot.player && alive && Dist2D(pos, lp) > 30.0f && Dist2D(lp, g.flags[size_t(best)].pos) + 10.0f < best_d)
          following = true, std::copy(lp, lp + 3, follow);
      }
    }
  }
  if (best != bot.goal && best >= 0) {
    bot.goal = best;
    bot.goal_until = now + std::chrono::seconds(60);
    bot.defending = FlagOwner(base, g.flags[best]) == team;
    bot.sprinter = Rand01() < SprintShare() * 1.6 * bot.jitter[0] * (1.0 + 0.5 * aggro);
    std::memcpy(bot.goal_pos, g.flags[best].pos, sizeof(bot.goal_pos));
    bot.goal_moved_off = false;
    if (REXCVAR_GET(bf_bots_log_detail))
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
  if (in_area && !bot.was_in_area && REXCVAR_GET(bf_bots_log_detail))
    REXLOG_INFO("BF bots: {} is in flag '{}''s area", bot.name, g.flags[best].name);
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
  if (REXCVAR_GET(bf_bots_log_detail) && now >= bot.next_log)
    REXLOG_INFO("BF bots: {} squad plan: following {} ({:.0f} {:.0f}), at flag {}, goal {:.0f} {:.0f}", bot.name, following,
                follow[0], follow[2], at_flag, bot.goal_pos[0], bot.goal_pos[2]);
  if (following && !at_flag) {
    // Toward its leader, a few metres off its side; there, it stays.
    const float side = (bot.index % 2 ? 1.0f : -1.0f) * 4.0f;
    bot.goal_pos[0] = follow[0] + side, bot.goal_pos[1] = follow[1], bot.goal_pos[2] = follow[2] + side;
    if (Dist2D(pos, follow) < 7.0f) std::copy(pos, pos + 3, bot.goal_pos);
  }
  // Pressing on (its side's aggression, Pressure): an enemy another bot saw near it, faced on the way (from 0.25) and
  // hunted within 45 m (from 0.6, not while holding a flag); a flag attacked from the side (from 0.5: two bots in
  // three, a point 30 to 60 m off the straight way, until 55 m from the flag); a leader short of the flag it attacks
  // (50 to 100 m off) waits for two of its squad within 25 m of it, 12 s at most.
  bool moved_off = following && !at_flag;
  const Pressure::Contact* contact = nullptr;
  if (aggro >= 0.25f && (team == 1 || team == 2)) {
    float cd = 60.0f;
    for (const auto& c : pressure.contacts[team])
      if (now - c.at < std::chrono::seconds(5) && Dist2D(pos, c.pos) < cd) cd = Dist2D(pos, c.pos), contact = &c;
    if (contact && aggro >= 0.6f && cd < 45.0f && !(at_flag && bot.defending) && !following) {
      std::copy(contact->pos, contact->pos + 3, bot.goal_pos);
      moved_off = true;
    }
  }
  if (!moved_off && aggro >= 0.5f && !bot.defending && !at_flag && bot.index % 3 != 0) {
    if (bot.flank_goal != best) {
      // The point: off the straight way at 40% of the way from the flag, to the bot's own side of it.
      const float dx = pos[0] - g.flags[best].pos[0], dz = pos[2] - g.flags[best].pos[2];
      const float len = std::max(std::sqrt(dx * dx + dz * dz), 1.0f);
      const float off = std::clamp(len * 0.4f, 30.0f, 60.0f) * (bot.index % 2 ? 1.0f : -1.0f);
      bot.flank_pos[0] = g.flags[best].pos[0] + dx * 0.4f + (-dz / len) * off;
      bot.flank_pos[1] = g.flags[best].pos[1];
      bot.flank_pos[2] = g.flags[best].pos[2] + dz * 0.4f + (dx / len) * off;
      bot.flank_goal = best, bot.flank_done = best_d < 80.0f;
    }
    if (!bot.flank_done && (Dist2D(pos, bot.flank_pos) < 15.0f || best_d < 55.0f)) bot.flank_done = true;
    if (!bot.flank_done) {
      std::copy(bot.flank_pos, bot.flank_pos + 3, bot.goal_pos);
      moved_off = true;
    }
  }
  if (!moved_off && aggro >= 0.5f && !bot.defending && !following && (base[bot.player + 132] & 0x80) && best_d > 50.0f &&
      best_d < 100.0f) {
    if (bot.regroup_goal != best) bot.regroup_goal = best, bot.regroup_until = now + std::chrono::seconds(12);
    const uint32_t sq = Load32(base, bot.player + 52);
    int members = 0, close = 0;
    for (const Bot& o : g.bots) {
      if (o.player == bot.player || Load32(base, o.player + 52) != sq || int(Load32(base, o.player + 48)) != team) continue;
      const uint32_t os = Load32(base, o.player + 288);
      float op[3];
      if (!os || !CachedPosition(ctx, base, os, op)) continue;
      ++members;
      if (Dist2D(op, pos) < 25.0f) ++close;
    }
    if (now < bot.regroup_until && close < std::min(2, members)) {
      std::copy(pos, pos + 3, bot.goal_pos);
      moved_off = true;
    }
  }
  if (!moved_off && bot.goal_moved_off && !at_flag) std::copy(g.flags[best].pos, g.flags[best].pos + 3, bot.goal_pos);
  bot.goal_moved_off = moved_off;
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
  // At most kSearchesPerTick searches a server tick (the optimization pass: 23 bots replanning in one tick cost tens
  // of milliseconds); a bot left over keeps its path and plans at its next think. A stuck bot plans at once.
  const bool want_plan = bot.path.empty() || goal_moved || now >= bot.replan_at || stuck;
  if (want_plan && (g.searches_left > 0 || stuck)) {
    --g.searches_left;
    bool complete = false, starved = false;
    // Stuck with a path: the step it was taking is closed for a minute (something the rays did not see: a railing, a
    // fence's gap too small, a slope too steep), and the next plan goes round it.
    if (stuck && !bot.path.empty()) {
      const int cx = CellOf(pos[0]), cz = CellOf(pos[2]);
      const int ddx = (CellOf(bot.path.front()[0]) > cx) - (CellOf(bot.path.front()[0]) < cx);
      const int ddz = (CellOf(bot.path.front()[2]) > cz) - (CellOf(bot.path.front()[2]) < cz);
      for (int d = 0; d < 8; ++d)
        if (kDx[d] == ddx && kDz[d] == ddz) SetStep(cx, cz, d, false, NavNow() + 60.0f);
    }
    const auto ways = learn.ways_in.find(g.flags[best].name);
    ++cost.searches;
    // Far from a goal at its flag: the search to a point along the level's best way there (field::Waypoint).
    float plan_to[3] = {bot.goal_pos[0], bot.goal_pos[1], bot.goal_pos[2]};
    bool by_field = false;
    if (Dist2D(pos, bot.goal_pos) > field::kFrom && Dist2D(bot.goal_pos, g.flags[best].pos) < field::kFrom &&
        field::Waypoint(best, pos, plan_to))
      by_field = Dist2D(plan_to, bot.goal_pos) > 4.0f;
    if (!by_field) std::copy(bot.goal_pos, bot.goal_pos + 3, plan_to);
    bot.path = FindPath(ctx, base, soldier, pos, plan_to, &complete, &starved, team,
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
          const auto own = nav.cells.find(CellKey(cx, cz));
          const int st = own == nav.cells.end() ? -1 : (own->second.open >> d) & 1 ? 1 : (own->second.closed >> d) & 1 ? 2 : -1;
          around += fmt::format(" [{},{}: {} y {:.1f} step {}]", kDx[d], kDz[d],
                                c == nav.cells.end() ? -1 : int(c->second.state),
                                c == nav.cells.end() ? 0.0f : c->second.y, st);
        }
        REXLOG_INFO("BF bots: {} finds no way from {:.1f} {:.1f} {:.1f} (water {:.1f}):{}", bot.name, pos[0], pos[1],
                    pos[2], nav.water, around);
      }
      bot.detour_yaw = Rand01() * 2 * kPi - kPi;
      bot.detour_until = now + std::chrono::seconds(2);
    }
    // Stuck before something low (the user, 19 Nov 2026: "They should now be able to melee breakable fences to get
    // through the obstacle"): blocked within 1.5 m at the waist (0.8 m) but clear over the head (2.4 m), as a fence,
    // a railing or a low wall is: the knife at it for 2 s (B, the knife's button on foot: the HUD's knife picture is
    // button_b; digital concepts 13 and 16), twice at most in a row before going round as below. A fence the game lets
    // break falls; a wall stays, and the next try goes round.
    bool fence = false;
    if (stuck && bot.melee_tries < 2) {
      const float fx = -std::sin(bot.yaw), fz = std::cos(bot.yaw);
      const float low[3] = {pos[0], pos[1] + 0.8f, pos[2]}, low_to[3] = {pos[0] + fx * 1.5f, pos[1] + 0.8f, pos[2] + fz * 1.5f};
      const float high[3] = {pos[0], pos[1] + 2.4f, pos[2]}, high_to[3] = {pos[0] + fx * 1.5f, pos[1] + 2.4f, pos[2] + fz * 1.5f};
      float f_low = 1.0f, f_high = 1.0f;
      if (Ray(ctx, base, soldier, low, low_to, &f_low) && f_low < 0.99f && Ray(ctx, base, soldier, high, high_to, &f_high) &&
          f_high > 0.99f) {
        fence = true;
        ++bot.melee_tries;
        bot.melee_until = now + std::chrono::seconds(2);
        REXLOG_INFO("BF bots: {} knifes something low in its way at {:.1f} {:.1f} {:.1f} ({:.1f} m)", bot.name, pos[0],
                    pos[1], pos[2], f_low * 1.5f);
      }
    }
    if (!stuck) bot.melee_tries = 0;
    if (stuck && REXCVAR_GET(bf_bots_log_detail))
      REXLOG_INFO("BF bots: {} is stuck at {:.1f} {:.1f} {:.1f}{}", bot.name, pos[0], pos[1], pos[2],
                  fence ? ", something low ahead: the knife" : ", a side step");
    if (stuck && !fence) {
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
  const bool alert = contact != nullptr;  // an enemy another bot saw close by: faced as it goes
  if ((at_flag && bot.watching || alert) && now >= bot.detour_until) {
    // Facing the watched way, moving the wanted way: forward and sideways from the angle between them (strafe -1
    // went to +x facing +z, run bots_s0k).
    const float watch = alert ? YawTo(pos, contact->pos) : bot.watch_yaw;
    const float rel = WrapAngle(want - watch);
    const float speed = at_flag ? 0.6f : 0.85f;
    bot.want_yaw = watch;
    bot.forward = Dist2D(pos, bot.goal_pos) < 1.5f ? 0.0f : speed * std::cos(rel);
    bot.strafe = Dist2D(pos, bot.goal_pos) < 1.5f ? 0.0f : speed * std::sin(rel);
  }
  // Sprint on the way, not at the flag, when turning hard, or watching for an enemy.
  bot.sprint = bot.sprinter && !at_flag && !alert && best_d > 20.0f && std::fabs(WrapAngle(want - bot.yaw)) < 0.5f;
}

// The tanks bots drive (the user, 19 Nov 2026: "understand how to use the plane and the tank"): the US Sherman and the
// Japanese tank, by name.
bool IsTank(const std::string& name) {
  return name == "Sherman" || name.rfind("Chi", 0) == 0 || name.rfind("chi", 0) == 0 || name.rfind("Type97", 0) == 0 ||
         name.rfind("HaGo", 0) == 0 || name.rfind("Ha_Go", 0) == 0;
}
void VehicleDrive(PPCContext& outer, uint8_t* base, Bot& bot, double dt, const float (&analog)[8]);
bool IsPlane(const std::string& name) { return name == "Corsair" || name == "Zero"; }

// A bot flying a plane, every tick (runs plane1 and plane2, a Corsair: input 0 the throttle, 5 the pitch with +1 the
// nose down, 6 the roll with +1 to the left (the right wing up), 4 the rudder with +1 to the left, 7 the guns; the
// plane holds its attitude, about 40 m/s, off the ground some 8 s after full throttle). Its target: the nearest enemy
// soldier near a flag its side does not hold, else that flag. It takes off (full throttle, level until 25 m/s, then
// the nose up to 30 m over the ground), cruises 100 m over the flags, banks into its turns (the bank wanted grows with
// the heading's error, the roll closes the gap; the stick pulled with the bank), dives on its target from 350 m and
// fires once lined up, and pulls up 75 m over the ground or 90 m from the target. Under 60 m over the ground and
// heading down, it levels its wings and climbs, whatever else it wanted.
void PlaneTick(PPCContext& ctx, uint8_t* base, Bot& bot, uint32_t plane, uint32_t soldier,
               const std::vector<uint32_t>& listed, Clock::time_point now, double dt) {
  float at[3] = {}, hdg = 0;
  if (!Position(ctx, base, plane, at) || !Heading(ctx, base, plane, &hdg)) return;
  PPCContext mc = ctx;
  mc.r3.u64 = plane;
  sub_822A7BB0(mc, base);
  const uint32_t m = mc.r3.u32;
  if (!Readable(m, 64)) return;
  const float nose = LoadF(base, m + 36), bank = std::asin(std::clamp(LoadF(base, m + 4), -1.0f, 1.0f));
  if (bot.plane_last_at != Clock::time_point{}) {
    const double el = std::chrono::duration<double>(now - bot.plane_last_at).count();
    if (el > 0.2) {
      const float d[3] = {at[0] - bot.plane_last[0], at[1] - bot.plane_last[1], at[2] - bot.plane_last[2]};
      bot.plane_speed = float(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / el);
      std::copy(at, at + 3, bot.plane_last);
      bot.plane_last_at = now;
    }
  } else {
    std::copy(at, at + 3, bot.plane_last);
    bot.plane_last_at = now;
    bot.plane_start_y = at[1];
  }
  // The ground under it (a ray 400 m down). A ray that finds nothing (the Japanese airfield on Guadalcanal: the
  // player's match of 23 Nov 2026, "400 m up" on the runway, so the pilot banked at 19 m/s and crashed, every take-off)
  // leaves its height over where it got in instead.
  float ground = at[1] - 400.0f;
  bool ground_known = false;
  {
    const float from[3] = {at[0], at[1] - 2.0f, at[2]}, to[3] = {at[0], at[1] - 400.0f, at[2]};
    float f = 1.0f;
    if (Ray(ctx, base, soldier, from, to, &f) && f < 1.0f) ground = at[1] - 2.0f - 398.0f * f, ground_known = true;
  }
  const float height = ground_known ? at[1] - ground : std::max(0.0f, at[1] - bot.plane_start_y);
  const int team = int(Load32(base, bot.player + 48));
  // The target.
  int flag = -1;
  float flag_d = 1e9f, top = -1e9f;
  for (size_t i = 0; i < g.flags.size(); ++i) {
    top = std::max(top, g.flags[i].pos[1]);
    if (!g.flags[i].takeable || FlagOwner(base, g.flags[i]) == team) continue;
    const float d = Dist2D(at, g.flags[i].pos);
    if (d < flag_d) flag_d = d, flag = int(i);
  }
  float target[3] = {at[0], at[1], at[2]};
  bool enemy = false;
  if (flag >= 0) std::copy(g.flags[size_t(flag)].pos, g.flags[size_t(flag)].pos + 3, target);
  float best = 1e9f;
  for (uint32_t p : listed) {
    if (!Readable(p, 0x140)) continue;
    const int t = int(Load32(base, p + 48));
    if (t == team || (t != 1 && t != 2)) continue;
    const uint32_t so = Load32(base, p + 288);
    float ep[3];
    if (!so || !Position(ctx, base, so, ep)) continue;
    const float near_flag = flag >= 0 ? Dist2D(ep, g.flags[size_t(flag)].pos) : 0.0f;
    if (near_flag > 120.0f) continue;
    const float d = Dist2D(at, ep);
    if (d < best) best = d, enemy = true, std::copy(ep, ep + 3, target);
  }
  float analog[8] = {};
  analog[0] = 1.0f;
  float want = YawTo(at, target), nose_want = 0.0f;
  const float cruise = (top > -1e8f ? top : at[1]) + 100.0f;
  const float tdx = target[0] - at[0], tdz = target[2] - at[2], tdy = target[1] + 1.0f - at[1];
  const float flat = std::sqrt(tdx * tdx + tdz * tdz), dist = std::sqrt(flat * flat + tdy * tdy);
  if (!bot.plane_flying) {
    // The take-off run: wings level, straight on, the nose up once fast enough.
    want = hdg;
    nose_want = bot.plane_speed > 25.0f ? 0.25f : nose;
    if (height > 30.0f && bot.plane_speed > 25.0f) bot.plane_flying = true;
  } else if (enemy && flat < 350.0f && flat > 90.0f && height > 75.0f) {
    nose_want = std::clamp(tdy / std::max(dist, 1.0f), -0.7f, 0.2f);  // the dive
  } else {
    nose_want = std::clamp((cruise - at[1]) / 150.0f, -0.3f, 0.35f);
  }
  const float err = WrapAngle(want - hdg);
  float bank_want = bot.plane_flying ? std::clamp(err * 1.5f, -1.0f, 1.0f) : 0.0f;
  if (height < 60.0f && nose < 0.1f && bot.plane_flying) bank_want = 0.0f, nose_want = 0.45f;  // the ground
  analog[6] = std::clamp((bank_want - bank) * 2.0f, -1.0f, 1.0f);
  analog[5] = std::clamp(-(nose_want - nose) * 3.0f - std::fabs(bank) * 0.3f, -1.0f, 1.0f);
  analog[4] = bot.plane_flying ? std::clamp(err * 0.5f, -0.5f, 0.5f) : std::clamp(err * 1.0f, -1.0f, 1.0f);
  if (enemy && bot.plane_flying && dist < 350.0f && std::fabs(err) < 0.15f &&
      std::fabs(nose - tdy / std::max(dist, 1.0f)) < 0.15f)
    analog[7] = 1.0f;
  if (now >= bot.next_log) {
    bot.next_log = now + std::chrono::seconds(2);
    REXLOG_INFO("BF bots: {} flies at {:.0f} {:.0f} {:.0f} ({:.0f} m up, {:.0f} m/s), heading {:.2f} nose {:.2f} bank {:.2f}, "
                "{} {:.0f} m{}",
                bot.name, at[0], at[1], at[2], height, bot.plane_speed, hdg, nose, bank, enemy ? "enemy" : "flag", flat,
                analog[7] > 0 ? ", firing" : "");
  }
  // The aim follows the nose (the guns are fixed).
  bot.yaw = hdg, bot.pitch = std::asin(std::clamp(nose, -1.0f, 1.0f));
  VehicleDrive(ctx, base, bot, dt, analog);
}

// A bot driving a tank, every tick (run tank1: input 0 the throttle, 4 the steering, +1 to the left; the turret aimed
// by the absolute aim, input 7 its gun). Its goal: the nearest flag its side does not hold (else the nearest it holds),
// stopping 18 m short to fight from there. The way: toward the goal, or along the cells the players drove this tank
// over (what it learned) when one of them, 12 to 45 m off, is nearer the goal; rays ahead at the hull's height
// (straight, then six ways up to a radian either side) steer it round what is in front; not moving for 3 s, it backs
// off 1.5 s and turns hard 2 s (26 Nov 2026; a hard turn only before, and it dug in: 85 stuck in 6 min, run fx_guad1).
// The gun: the nearest enemy soldier within 150 m it can see, aimed at, a shot every 2.5 s once the aim has settled.
void TankTick(PPCContext& ctx, uint8_t* base, Bot& bot, uint32_t vehicle, uint32_t soldier,
              const std::vector<uint32_t>& listed, Clock::time_point now, double dt) {
  float at[3] = {}, hdg = 0;
  if (!Position(ctx, base, vehicle, at) || !Heading(ctx, base, vehicle, &hdg)) return;
  const int team = int(Load32(base, bot.player + 48));
  if (now >= bot.tank_think) {
    bot.tank_think = now + std::chrono::milliseconds(300);
    int best = -1, held = -1;
    float best_d = 1e9f, held_d = 1e9f;
    for (size_t i = 0; i < g.flags.size(); ++i) {
      if (!g.flags[i].takeable) continue;
      const float d = Dist2D(at, g.flags[i].pos);
      if (FlagOwner(base, g.flags[i]) != team) {
        if (d < best_d) best_d = d, best = int(i);
      } else if (d < held_d) {
        held_d = d, held = int(i);
      }
    }
    bot.tank_goal = best >= 0 ? best : held;
    if (bot.tank_goal >= 0) {
      const float* goal = g.flags[size_t(bot.tank_goal)].pos;
      std::copy(goal, goal + 3, bot.tank_way);
      const auto it = learn.vehicles.find(VehicleKey(base, bot.player));
      if (it != learn.vehicles.end()) {
        float best_score = Dist2D(at, goal);
        for (const auto& [key, c] : it->second.cells) {
          const float cp[3] = {CellX(key) * kCell + kCell * 0.5f, 0.0f, CellZ(key) * kCell + kCell * 0.5f};
          const float d = Dist2D(at, cp);
          if (d < 12.0f || d > 45.0f) continue;
          const float score = Dist2D(cp, goal) + 0.3f * d;
          if (score < best_score - 4.0f) best_score = score, bot.tank_way[0] = cp[0], bot.tank_way[2] = cp[2];
        }
      }
    }
  }
  float analog[8] = {};
  const float to_goal = bot.tank_goal >= 0 ? Dist2D(at, g.flags[size_t(bot.tank_goal)].pos) : 0.0f;
  float want = YawTo(at, bot.tank_way);
  // What is ahead: rays at the hull's height, straight and to either side.
  auto clear = [&](float yaw) {
    const float from[3] = {at[0], at[1] + 1.4f, at[2]};
    const float to[3] = {at[0] - std::sin(yaw) * 10.0f, at[1] + 1.4f, at[2] + std::cos(yaw) * 10.0f};
    float f = 1.0f;
    Ray(ctx, base, soldier, from, to, &f);
    return f;
  };
  const float ahead = clear(hdg);
  if (ahead < 0.7f) {
    // Something within 7 m ahead (TekRantGaming, 26 Nov 2026: "bots driving will often run into things"): the clearest
    // of six ways round it, up to a radian either side, the one nearest the way it wants when two are as clear (only
    // two rays half a radian off before, and a turn of 0.8 whatever they found).
    float best_clear = -1.0f, best_yaw = hdg;
    for (float off : {0.35f, -0.35f, 0.7f, -0.7f, 1.05f, -1.05f}) {
      const float c = clear(hdg + off);
      const float score = c - 0.15f * std::fabs(WrapAngle(hdg + off - want));
      if (score > best_clear) best_clear = score, best_yaw = hdg + off;
    }
    want = best_yaw + (WrapAngle(best_yaw - hdg) > 0 ? 0.25f : -0.25f);
  }
  if (now - bot.tank_check_at > std::chrono::seconds(3)) {
    if (REXCVAR_GET(bf_bots_log_detail))
      REXLOG_INFO("BF bots: {} drives {} at {:.0f} {:.0f} {:.0f}, {:.0f} m to its flag, moved {:.1f} m in 3 s, clear ahead "
                  "{:.0f}%{}",
                  bot.name, VehicleName(base, vehicle), at[0], at[1], at[2], to_goal,
                  bot.tank_check_at == Clock::time_point{} ? 0.0f : Dist2D(at, bot.tank_check_pos), ahead * 100,
                  now < bot.tank_turn_until ? ", turning hard" : "");
    if (bot.tank_check_at != Clock::time_point{} && Dist2D(at, bot.tank_check_pos) < 1.0f && to_goal > 18.0f &&
        now >= bot.tank_turn_until) {
      // Stuck: back off 1.5 s first (pushing on only dug in), its hull swinging, then the hard turn the other way 2 s.
      bot.tank_turn = Rand01() < 0.5f ? 1.0f : -1.0f;
      bot.tank_reverse_until = now + std::chrono::milliseconds(1500);
      bot.tank_turn_until = now + std::chrono::milliseconds(3500);
      REXLOG_INFO("BF bots: {} is stuck in {} at {:.0f} {:.0f} ({:.0f} m to its flag): backs off and turns", bot.name,
                  VehicleName(base, vehicle), at[0], at[2], to_goal);
    }
    std::copy(at, at + 3, bot.tank_check_pos);
    bot.tank_check_at = now;
  }
  const float err = WrapAngle(want - hdg);
  const bool reversing = now < bot.tank_reverse_until;
  analog[4] = reversing ? -bot.tank_turn : now < bot.tank_turn_until ? bot.tank_turn : std::clamp(err * 1.5f, -1.0f, 1.0f);
  analog[0] = reversing                                          ? -1.0f
              : to_goal < 18.0f && now >= bot.tank_turn_until ? 0.0f
              : std::fabs(err) > 1.2f || ahead < 0.4f         ? 0.4f
                                                              : 1.0f;
  // The gun.
  const float gun[3] = {at[0], at[1] + 2.5f, at[2]};
  float target[3] = {}, best_d = 150.0f;
  bool have = false;
  for (uint32_t p : listed) {
    if (!Readable(p, 0x140)) continue;
    const int t = int(Load32(base, p + 48));
    if (t == team || (t != 1 && t != 2)) continue;
    const uint32_t so = Load32(base, p + 288);
    float ep[3];
    if (!so || !Position(ctx, base, so, ep)) continue;
    const float d = Dist2D(at, ep);
    if (d >= best_d) continue;
    const float aim[3] = {ep[0], ep[1] + 1.0f, ep[2]};
    float f = 1.0f;
    const float k = std::max(0.0f, (d - 1.5f) / std::max(d, 0.1f));
    const float to[3] = {gun[0] + (aim[0] - gun[0]) * k, gun[1] + (aim[1] - gun[1]) * k, gun[2] + (aim[2] - gun[2]) * k};
    if (Ray(ctx, base, soldier, gun, to, &f) && f < 0.97f) continue;
    best_d = d, have = true;
    std::copy(aim, aim + 3, target);
  }
  // Enemy vehicles too (a vehicle whose driver is of the other side), their hull's middle.
  bot.target_vehicle = 0;
  {
    std::vector<uint32_t> vs;
    {
      std::lock_guard lock(g.mutex);
      vs.assign(g.vehicles.begin(), g.vehicles.end());
    }
    for (uint32_t v : vs) {
      if (v == vehicle) continue;
      const uint32_t driver = SeatPlayer(base, v, 0);
      if (!driver || !Readable(driver, 0x40)) continue;
      const int t = int(Load32(base, driver + 48));
      if (t == team || (t != 1 && t != 2)) continue;
      float vp[3];
      if (!Position(ctx, base, v, vp)) continue;
      const float d = Dist2D(at, vp);
      if (d >= best_d) continue;
      const float aim[3] = {vp[0], vp[1] + 1.2f, vp[2]};
      float f = 1.0f;
      const float k = std::max(0.0f, (d - 3.0f) / std::max(d, 0.1f));
      const float to[3] = {gun[0] + (aim[0] - gun[0]) * k, gun[1] + (aim[1] - gun[1]) * k, gun[2] + (aim[2] - gun[2]) * k};
      if (Ray(ctx, base, soldier, gun, to, &f) && f < 0.97f) continue;
      best_d = d, have = true, bot.target_vehicle = v;
      std::copy(aim, aim + 3, target);
    }
  }
  if (have) {
    bot.want_yaw = YawTo(at, target);
    bot.want_pitch = std::atan2(target[1] - gun[1], std::max(best_d, 1.0f));
  } else {
    bot.want_yaw = hdg, bot.want_pitch = 0;
  }
  const float step = float(kTurnRate * 0.6 * dt);
  bot.yaw = WrapAngle(bot.yaw + std::clamp(WrapAngle(bot.want_yaw - bot.yaw), -step, step));
  bot.pitch += std::clamp(bot.want_pitch - bot.pitch, -step, step);
  if (have && std::fabs(WrapAngle(bot.want_yaw - bot.yaw)) < 0.05f && now >= bot.tank_fire_at) {
    bot.tank_fire_at = now + std::chrono::milliseconds(2500);
    analog[7] = 1.0f;
  }
  VehicleDrive(ctx, base, bot, dt, analog);
}

// One server tick of a seated bot's input (its vehicle reads its player's input itself): the analog concepts a driver
// uses (run veh1, the player in a Sherman: LT 0 throttle, RT 7 fire, the left stick's vertical 4, its horizontal 6, the
// right stick's horizontal 5), the aim valid and from the client bits, and the absolute aim (the turret's).
void VehicleDrive(PPCContext& outer, uint8_t* base, Bot& bot, double dt, const float (&analog)[8]) {
  for (int c : {0, 1, 4, 5, 6, 7}) {
    PPCContext call = outer;
    call.r3.u64 = bot.state, call.r4.u64 = uint64_t(c), call.f1.f64 = analog[c];
    sub_822BD518(call, base);
  }
  StoreF(base, bot.state + 128, float(dt));
  PPCContext call = outer;
  call.r3.u64 = bot.input, call.r4.u64 = bot.state;
  sub_822C15D0(call, base);
  const uint32_t cur = Load32(base, bot.input + 352);
  if (Readable(cur, 176)) {
    base[cur + 122] |= 0x09;
    if (Clock::now() < bot.press_until) base[cur + 126] |= 0x20;  // digital concept 13: enter/exit (bailing out)
    StoreF(base, cur + 136, bot.yaw);
    StoreF(base, cur + 140, bot.pitch);
    Store32(base, cur + 152, ++bot.tick);
  }
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
    if (Clock::now() < bot.press_until) base[cur + 126] |= 0x20;  // digital concept 13: enter/exit (B, run boatbtn2)
    // A test's concepts (the input state's digital word at +120, concept c its bit c: sub_822BA4A0, the game's read of
    // a concept, analog 0 to 7 at +8 + 4c, every other a bit of that word).
    for (int c = 8; c < 64; ++c)
      if (bot.test_bits >> c & 1) base[cur + 120 + 7 - c / 8] |= uint8_t(1 << (c % 8));
    if (bot.select_slot >= 0 && bot.select_slot < 4 && Clock::now() < bot.select_until) {
      const int c = 30 + bot.select_slot;  // SelectWeapon1 to 4
      base[cur + 120 + 7 - c / 8] |= uint8_t(1 << (c % 8));
    }
    // Out of a plane, the parachute (the main loop's watch below): A, digital concepts 12 and 29 (A's pair in run
    // bots_calib). Run bail3: A opened it (40 m/s down to 8.7); RB (concept 17, EIAToggleParachute in the input enum's
    // table at 0x82D3D530) did not.
    if (Clock::now() < bot.chute_press_until) base[cur + 126] |= 0x10, base[cur + 124] |= 0x20;
    // The knife (B: digital concepts 13 and 16), pressed a quarter of each half second while at a fence.
    if (std::chrono::steady_clock::now() < bot.melee_until && bot.tick % 30 < 8) base[cur + 126] |= 0x20, base[cur + 125] |= 0x01;
    StoreF(base, cur + 136, bot.yaw);
    StoreF(base, cur + 140, bot.pitch);
    Store32(base, cur + 152, ++bot.tick);
  }
  // The soldier's input step, vtable +260, for its entry 0 (what a connection calls after each input message).
  const uint32_t vtable = Load32(base, soldier);
  if (!InImage(vtable)) return;
  call = outer;
  call.r3.u64 = soldier, call.r4.u64 = 0;
  // (vtable 82013A40 +260: sub_822F05C8, the step a human's input runs too, 60 a second each, about 0.05 ms: run
  // botsstep2 of the optimization pass. Most of what a bot costs is this, as a human player's soldier costs.)
  CostTimer t(&cost.input_step);
  CallGuest(call, base, Load32(base, vtable + 260));
}

// A bot in a vehicle's seat, each server tick: it sits still (no stick, no trigger) and gets off where a passenger
// would. Its input goes through the vehicle's own input step for its seat (vtable +260 with the entry's index: the
// step reads the entry's player at +284 and runs only for the entity that player controls, sub_822F05C8), so the
// enter/exit button (digital concept 13) does what it does for a player: the game takes it out of its seat.
// Off when: the vehicle has come 25 m or more from where it got on and stood still 2 s (a landing craft at the
// beach); its driver's seat has been empty 1 s (the driver got off: the boat goes nowhere); the vehicle has not moved
// for 20 s (27 Nov 2026, whoever drives); or it has sat 3 minutes with the vehicle still 5 s. A place where the soldier
// keeps its own weapon goes by StandingRide instead.
std::string SeatsText(PPCContext& ctx, uint8_t* base, uint32_t v, uint32_t soldier_of_bot);  // below
// The vehicle's way over the last 10 s, every 5 s (ride_slow), from a bot's seat.
void RideWindow(Bot& bot, const float at[3], Clock::time_point now) {
  if (bot.ride_win_at == Clock::time_point{}) {
    bot.ride_win_at = now;
    std::copy(at, at + 3, bot.ride_was5), std::copy(at, at + 3, bot.ride_was10);
    bot.ride_slow = false;
    return;
  }
  if (now - bot.ride_win_at < std::chrono::seconds(5)) return;
  bot.ride_win_at = now;
  bot.ride_slow = now - bot.ride_at > std::chrono::seconds(12) && Dist2D(at, bot.ride_was10) < 4.0f;
  std::copy(bot.ride_was5, bot.ride_was5 + 3, bot.ride_was10);
  std::copy(at, at + 3, bot.ride_was5);
}
void Ride(PPCContext& outer, uint8_t* base, Bot& bot, uint32_t vehicle, Clock::time_point now, double dt) {
  float at[3] = {};
  if (!CachedPosition(outer, base, vehicle, at)) return;
  if (bot.ride != vehicle) {
    bot.ride = vehicle, bot.ride_at = bot.ride_moved = now, bot.no_driver_since = {}, bot.boated = true;
    bot.press_until = bot.next_press = {};
    bot.ride_win_at = {};
    std::memcpy(bot.ride_from, at, 12);
    std::memcpy(bot.ride_last, at, 12);
  }
  const float d[3] = {at[0] - bot.ride_last[0], at[1] - bot.ride_last[1], at[2] - bot.ride_last[2]};
  if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 0.75f * 0.75f) {
    std::memcpy(bot.ride_last, at, 12);
    bot.ride_moved = now;
  }
  int seat = -1;
  const uint32_t entries = Load32(base, vehicle + 144);
  const uint32_t count = Readable(entries, 4) ? std::min<uint32_t>((Load32(base, vehicle + 148) - entries) / 4, 16) : 0;
  for (uint32_t i = 0; i < count; ++i)
    if (SeatPlayer(base, vehicle, i) == bot.player) seat = int(i);
  if (seat < 0) {
    static std::map<uint32_t, Clock::time_point> said;
    if (REXCVAR_GET(bf_bots_log_detail) && now - said[bot.player] > std::chrono::seconds(5)) {
      said[bot.player] = now;
      REXLOG_INFO("BF bots: {} controls {:08X} ({}), not one of its seats ({} entries)", bot.name, vehicle,
                  VehicleName(base, vehicle), count);
    }
    return;
  }
  const uint32_t driver = SeatPlayer(base, vehicle, 0);
  if (driver) bot.no_driver_since = {};
  else if (bot.no_driver_since == Clock::time_point{}) bot.no_driver_since = now;
  const auto still = now - bot.ride_moved;
  RideWindow(bot, at, now);
  const char* why = nullptr;
  float throttle = 0, steer = 0, brake = 0;
  if (seat != 0 && REXCVAR_GET(bf_bots_log_detail) && now >= bot.boat_log) {
    bot.boat_log = now + std::chrono::seconds(3);
    REXLOG_INFO("BF bots: {} rides {} (seat {}) at {:.0f} {:.0f}, still {:.1f} s; seats:{}", bot.name,
                VehicleName(base, vehicle), seat, at[0], at[2], std::chrono::duration<double>(still).count(),
                SeatsText(outer, base, vehicle, Load32(base, bot.player + 288)));
  }
  if (seat == 0 && IsBoat(base, vehicle)) {
    // Driving a boat: passengers first (bots of its side on their way to it, 8 s at most, or till it is full), then
    // along the way the players drove (BoatRoute) to the beach nearest its flag, or straight for the flag. Ran into
    // something: backs off 2.5 s, three times at most. Stopped near its flag, or at its way's end: on the beach, off.
    if (bot.ride_at == now) {
      bot.boat_path.clear();
      if (bot.goal >= 0 && bot.goal < int(g.flags.size())) {
        bot.boat_path = BoatRoute(at, g.flags[bot.goal].pos);
        const bool learned = !bot.boat_path.empty();
        if (!learned) bot.boat_path.push_back({g.flags[bot.goal].pos[0], g.flags[bot.goal].pos[1], g.flags[bot.goal].pos[2]});
        REXLOG_INFO("BF bots: {} drives {} for flag '{}' ({:.0f} m), {}", bot.name, VehicleName(base, vehicle),
                    g.flags[bot.goal].name, Dist2D(at, g.flags[bot.goal].pos),
                    learned ? fmt::format("the players' way, {} points", bot.boat_path.size()) : std::string("straight"));
      }
      bot.boat_wait_until = now + std::chrono::seconds(8), bot.reverses = 0, bot.reverse_until = {};
    }
    const uint32_t n = SeatCount(outer, base, vehicle);
    uint32_t taken = 0;
    for (uint32_t i = 0; i < n; ++i) taken += SeatPlayer(base, vehicle, i) ? 1 : 0;
    const float* goal = bot.goal >= 0 && bot.goal < int(g.flags.size()) ? g.flags[bot.goal].pos : nullptr;
    while (bot.boat_path.size() > 1 && Dist2D(at, bot.boat_path.front().data()) < 12.0f)
      bot.boat_path.erase(bot.boat_path.begin());
    if (bot.boat_path.empty() || !goal) {
      why = "no way to drive";
    } else if (now < bot.boat_wait_until && taken < n) {
      bot.ride_moved = now;  // waiting is not being stuck
    } else {
      float heading = 0;
      Heading(outer, base, vehicle, &heading);
      const float want = YawTo(at, bot.boat_path.front().data());
      const float diff = WrapAngle(want - heading);
      if (now < bot.reverse_until) {
        brake = 1.0f, steer = diff > 0 ? -1.0f : 1.0f;  // backing: the stern swings the other way
      } else {
        steer = std::clamp(diff * 2.0f, -1.0f, 1.0f);
        throttle = std::fabs(diff) > 1.2f ? 0.4f : 1.0f;
      }
      const bool at_end = bot.boat_path.size() == 1 && Dist2D(at, bot.boat_path.front().data()) < 20.0f;
      if ((still > std::chrono::seconds(3) || bot.ride_slow) && now >= bot.reverse_until) {
        if (Dist2D(at, goal) < 200.0f || at_end || bot.boat_path.size() == 1 && Dist2D(at, goal) < 260.0f)
          why = "on the beach";
        else if (bot.reverses < 3) {
          ++bot.reverses;
          bot.reverse_until = now + std::chrono::milliseconds(2500);
          bot.ride_moved = now;
          if (bot.boat_path.size() > 1) bot.boat_path.erase(bot.boat_path.begin());  // that point is blocked: the next
          REXLOG_INFO("BF bots: {} backs {} off something ({:.0f} m from its flag)", bot.name,
                      VehicleName(base, vehicle), Dist2D(at, goal));
        } else {
          why = "the boat is stuck";
          g.stranded[vehicle] = now + std::chrono::minutes(5);
        }
      }
      if (now >= bot.boat_log) {
        bot.boat_log = now + std::chrono::seconds(5);
        REXLOG_INFO("BF bots: {} drives {} at {:.0f} {:.0f}: heading {:.2f}, wants {:.2f}, {:.0f} m to its flag, {} points "
                    "left, {} aboard",
                    bot.name, VehicleName(base, vehicle), at[0], at[2], heading, want, Dist2D(at, goal),
                    bot.boat_path.size(), taken);
      }
    }
  } else if (bot.no_driver_since != Clock::time_point{} && now - bot.no_driver_since > std::chrono::seconds(1))
    why = "its driver got off";
  else if (still > std::chrono::seconds(20) || (bot.ride_slow && now - bot.ride_at > std::chrono::seconds(20)))
    why = "the vehicle has not moved for 20 s";  // 27 Nov 2026: never a whole match in a parked vehicle
  else if (Dist2D(at, bot.ride_from) > 25.0f && bot.ride_slow)
    why = "the vehicle stopped";  // in the surf: under 4 m in 10 s
  else if (driver && IsBot(driver))
    ;  // a bot drives: off when it gets off, on the beach
  else if (Dist2D(at, bot.ride_from) > 25.0f && still > std::chrono::seconds(2))
    why = "the vehicle stopped";
  else if (now - bot.ride_at > std::chrono::minutes(3) && still > std::chrono::seconds(5))
    why = "a long wait";
  if (why && now >= bot.next_press) {
    bot.press_until = now + std::chrono::milliseconds(250);
    bot.next_press = now + std::chrono::milliseconds(1500);  // still seated then: pressed again
    REXLOG_INFO("BF bots: {} gets off {} (seat {}): {}", bot.name, VehicleName(base, vehicle), seat, why);
  }
  // The analog concepts in a boat (run boatin1, a player driving): 0 the throttle (LT), 4 the steering (the left
  // stick, -1 left), 2 the brake and reverse (LB), 7 the gun (RT).
  if (why) throttle = steer = brake = 0;
  PPCContext call = outer;
  const std::pair<uint32_t, float> analog[] = {{0u, throttle}, {1u, 0.0f}, {2u, brake}, {4u, steer}, {7u, 0.0f}};
  for (const auto& [c, v] : analog) {
    call = outer;
    call.r3.u64 = bot.state, call.r4.u64 = c, call.f1.f64 = v;
    sub_822BD518(call, base);
  }
  StoreF(base, bot.state + 128, float(dt));
  call = outer;
  call.r3.u64 = bot.input, call.r4.u64 = bot.state;
  sub_822C15D0(call, base);
  const uint32_t cur = Load32(base, bot.input + 352);
  if (Readable(cur, 176)) {
    base[cur + 122] |= 0x09;
    if (now < bot.press_until) base[cur + 126] |= 0x20;
    StoreF(base, cur + 136, bot.yaw);
    StoreF(base, cur + 140, bot.pitch);
    Store32(base, cur + 152, ++bot.tick);
  }
  const uint32_t vtable = Load32(base, vehicle);
  if (!InImage(vtable)) return;
  call = outer;
  call.r3.u64 = vehicle, call.r4.u64 = uint32_t(seat);
  CostTimer t(&cost.input_step);
  CallGuest(call, base, Load32(base, vtable + 260));
}

// A bot in a place where it keeps its own weapon (TekRantGaming, 27 Nov 2026: "when bots use boats when they reach land
// and stop not all the bots exit and it results in bots just sitting in boats the full match doing nothing"). A landing
// craft's fourth place keeps the soldier as what its player controls (+312), so the bot was taken for one on foot: it
// rode along, then walked against the beached boat the rest of the match and never pressed to get off (run fx_wakeD:
// one bot, 128 stuck checks at one spot by the south beach). Found by the vehicles' seats, a quarter second apart.
// Tests' detail (bf_bots_log_detail): a vehicle's seats, each one's player and what that player controls.
std::string SeatsText(PPCContext& ctx, uint8_t* base, uint32_t v, uint32_t soldier_of_bot) {
  std::string out;
  const uint32_t n = SeatCount(ctx, base, v);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t p = SeatPlayer(base, v, i);
    if (!p || !Readable(p, 0x140)) continue;
    const uint32_t ctl = Load32(base, p + 312), so = Load32(base, p + 288);
    out += fmt::format(" [{}: {} ctl {}{}]", i, PlayerName(base, p), ctl == v ? "the vehicle" : ctl == so ? "its soldier" : fmt::format("{:08X}", ctl),
                       so == soldier_of_bot ? " (this bot)" : "");
  }
  return out;
}
uint32_t StandingIn(PPCContext& ctx, uint8_t* base, Bot& bot, uint32_t soldier, Clock::time_point now) {
  if (now < bot.standing_check) return bot.standing;
  bot.standing_check = now + std::chrono::milliseconds(250);
  std::vector<uint32_t> vs;
  {
    std::lock_guard lock(g.mutex);
    vs.assign(g.vehicles.begin(), g.vehicles.end());
  }
  bot.standing = 0, bot.standing_seat = -1;
  float sp[3];
  if (!CachedPosition(ctx, base, soldier, sp)) return 0;
  for (uint32_t v : vs) {
    float vp[3];
    if (!Readable(v, 0x100) || !CachedPosition(ctx, base, v, vp) || Dist2D(vp, sp) > 6.0f) continue;  // on it, or not
    const uint32_t n = SeatCount(ctx, base, v);
    for (uint32_t i = 0; i < n; ++i)
      if (SeatPlayer(base, v, i) == bot.player) {
        // A seat's player is left there after it got off (run fx_boat1: a bot off seat 2 still named in seat 5 while
        // it swam and fought 100 m away): only a soldier on the vehicle itself counts.
        if (std::fabs(vp[1] - sp[1]) > 4.0f) continue;
        bot.standing = v, bot.standing_seat = int(i);
        return v;
      }
  }
  return 0;
}

// Such a bot, each tick: it stands still in its place and gets off where a seated passenger would (Ride's rules), with
// the enter/exit button on its soldier's own input (Drive presses it while press_until).
void StandingRide(PPCContext& ctx, uint8_t* base, Bot& bot, uint32_t vehicle, uint32_t soldier, Clock::time_point now,
                  double dt) {
  float at[3] = {};
  if (!CachedPosition(ctx, base, vehicle, at)) return;
  if (bot.ride != vehicle) {
    bot.ride = vehicle, bot.ride_at = bot.ride_moved = now, bot.no_driver_since = {}, bot.boated = true;
    bot.press_until = bot.next_press = {};
    bot.ride_win_at = {};
    std::memcpy(bot.ride_from, at, 12);
    std::memcpy(bot.ride_last, at, 12);
    bot.boat = 0;  // (on its way to a boat: this is one)
    bot.standing_tries = 0;
    REXLOG_INFO("BF bots: {} rides in {} standing (seat {}, its own weapon in hand)", bot.name, VehicleName(base, vehicle),
                bot.standing_seat);
  }
  const float d[3] = {at[0] - bot.ride_last[0], at[1] - bot.ride_last[1], at[2] - bot.ride_last[2]};
  if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 0.75f * 0.75f) {
    std::memcpy(bot.ride_last, at, 12);
    bot.ride_moved = now;
  }
  const uint32_t driver = SeatPlayer(base, vehicle, 0);
  if (driver) bot.no_driver_since = {};
  else if (bot.no_driver_since == Clock::time_point{}) bot.no_driver_since = now;
  const auto still = now - bot.ride_moved;
  RideWindow(bot, at, now);
  const char* why = nullptr;
  if (bot.no_driver_since != Clock::time_point{} && now - bot.no_driver_since > std::chrono::seconds(1))
    why = "its driver got off";
  else if (Dist2D(at, bot.ride_from) > 25.0f && still > std::chrono::seconds(2))
    why = "the vehicle stopped";
  else if (Dist2D(at, bot.ride_from) > 25.0f && bot.ride_slow)
    why = "the vehicle stopped";
  else if (still > std::chrono::seconds(20) || (bot.ride_slow && now - bot.ride_at > std::chrono::seconds(20)))
    why = "the vehicle has not moved for 20 s";
  if (why && now >= bot.next_press) {
    bot.press_until = now + std::chrono::milliseconds(250);
    bot.next_press = now + std::chrono::milliseconds(1500);  // still in its place then: pressed again
    ++bot.standing_tries;
    REXLOG_INFO("BF bots: {} gets off {} (standing, seat {}, try {}): {}", bot.name, VehicleName(base, vehicle),
                bot.standing_seat, bot.standing_tries, why);
    // No button gets a soldier out of such a place (run fx_boat3: its own input, the vehicle's input step for the
    // place, and a jump, 17 tries). The server's seating moves it to a seat of the vehicle that is free (the others
    // have got off), and from a seat it gets off as any passenger (Ride).
    const uint32_t n = SeatCount(ctx, base, vehicle);
    for (uint32_t seat : {1u, 2u}) {  // (not the driver's: it would drive the boat off the beach)
      if (seat >= n || int(seat) == bot.standing_seat || SeatPlayer(base, vehicle, seat)) continue;
      PPCContext call = ctx;
      call.r3.u64 = bot.player, call.r4.u64 = vehicle, call.r5.u64 = seat;
      sub_82398B98(call, base);
      const bool moved = Load32(base, bot.player + 312) == vehicle;
      REXLOG_INFO("BF bots: {} moves to seat {} of {} to get off: {}", bot.name, seat, VehicleName(base, vehicle),
                  moved ? "seated" : "refused");
      if (moved) {
        bot.standing = 0, bot.standing_seat = -1, bot.ride = 0;  // Ride takes it from here (its own start)
        bot.press_until = {};
        return;
      }
    }
  }
  if (REXCVAR_GET(bf_bots_log_detail) && now >= bot.boat_log) {
    bot.boat_log = now + std::chrono::seconds(3);
    float sp[3] = {};
    CachedPosition(ctx, base, soldier, sp);
    REXLOG_INFO("BF bots: {} standing in {} at {:.0f} {:.0f}, its soldier {:.1f} m from it at {:.1f} {:.1f} {:.1f}; seats:{}",
                bot.name, VehicleName(base, vehicle), at[0], at[2], Dist2D(sp, at), sp[0], sp[1], sp[2],
                SeatsText(ctx, base, vehicle, soldier));
  }
  bot.forward = bot.strafe = 0, bot.sprint = bot.ads = bot.fire = false;
  bot.want_yaw = bot.yaw, bot.want_pitch = 0;
  // Getting off: the enter/exit button on its soldier's input (Drive), and every other press A (a jump) as well.
  if (why && now < bot.press_until && (bot.standing_tries % 2) == 1) bot.chute_press_until = bot.press_until;
  Drive(ctx, base, bot, soldier, dt);
  // And the same button through the vehicle's own input step for that place, as a seated passenger's (Ride).
  if (now < bot.press_until) {
    const uint32_t cur = Load32(base, bot.input + 352);
    if (Readable(cur, 176)) base[cur + 126] |= 0x20;
    const uint32_t vtable = Load32(base, vehicle);
    if (InImage(vtable) && bot.standing_seat >= 0) {
      PPCContext call = ctx;
      call.r3.u64 = vehicle, call.r4.u64 = uint32_t(bot.standing_seat);
      CallGuest(call, base, Load32(base, vtable + 260));
    }
  }
}

// Bailing out (the user, 22 Nov 2026: "Bots should also be able to bail out of vehicles if they have the chance to
// before it explodes, and only if it helps them complete an objective. Preservation of their life is a dual-primary"):
// each tick in a seat, its vehicle's health (VehicleHealth) and how fast it falls. Doomed: under 12%, or falling at a
// rate that ends it within 3 s (under 45%) or 6 s (under 30%). Then it gets out (the enter/exit button, pressed again
// every 1.5 s, 6 s at most) unless staying does more for its side, while above 6%: its tank's gun on an enemy vehicle
// weaker than its own and nearly done (under 25%), or its vehicle in the area of a flag its side has nearly taken (75%
// or more); or unless getting out would not help: a plane 4 to 35 m up (too low for the parachute, too high to jump),
// or open sea round it with no flag within 300 m (it would swim nowhere: a new spawn is sooner). Out of a plane, its
// parachute opens 1.2 s on (Drive). True when it starts getting out.
bool BailCheck(PPCContext& ctx, uint8_t* base, Bot& bot, uint32_t vehicle, uint32_t soldier, Clock::time_point now) {
  const float hp = VehicleHealth(base, vehicle);
  if (hp < 0.0f) return false;
  if (bot.hp_vehicle != vehicle) {
    bot.hp_vehicle = vehicle, bot.hp_last = hp, bot.hp_rate = 0, bot.hp_at = bot.hp_since = now, bot.bailing = false;
    return false;
  }
  const double el = std::chrono::duration<double>(now - bot.hp_at).count();
  if (el >= 0.25) {
    const float loss = float(std::max(0.0f, bot.hp_last - hp) / el);
    bot.hp_rate = hp > bot.hp_last + 0.001f ? 0.0f : bot.hp_rate * 0.5f + loss * 0.5f;  // repaired: not falling
    bot.hp_last = hp, bot.hp_at = now;
  }
  if (bot.bailing) {
    if (now - bot.bail_since > std::chrono::seconds(6)) {
      bot.bailing = false, bot.chute_until = {};
      REXLOG_INFO("BF bots: {} could not get out of {}", bot.name, VehicleName(base, vehicle));
    } else if (now >= bot.next_press) {
      bot.press_until = now + std::chrono::milliseconds(250);
      bot.next_press = now + std::chrono::milliseconds(1500);
    }
    return false;
  }
  if (hp <= 0.0f) return false;  // already gone
  const float left = bot.hp_rate > 0.002f ? hp / bot.hp_rate : 1e9f;  // seconds to go at this rate
  bool doomed = hp < 0.12f || (hp < 0.45f && left < 3.0f) || (hp < 0.3f && left < 6.0f);
#if defined(BF_DEV_TOOLS)
  // BF_BOT_BAILTEST=1 (tests): a driver's vehicle counts as doomed 25 s after it got in (the exit and the parachute).
  static const bool bail_test = std::getenv("BF_BOT_BAILTEST") != nullptr;
  if (bail_test && Load32(base, bot.player + 292) == 0 && now - bot.hp_since > std::chrono::seconds(25) &&
      (!IsPlane(VehicleName(base, vehicle)) || bot.plane_flying))
    doomed = true;
#endif
  if (!doomed) return false;
  float at[3] = {};
  if (!Position(ctx, base, vehicle, at)) return false;
  const int team = int(Load32(base, bot.player + 48));
  const bool plane = IsPlane(VehicleName(base, vehicle));
  float height = 0;
  if (plane) {
    height = 400.0f;
    const float from[3] = {at[0], at[1] - 2.0f, at[2]}, to[3] = {at[0], at[1] - 400.0f, at[2]};
    float f = 1.0f;
    if (Ray(ctx, base, soldier, from, to, &f) && f < 1.0f) height = 2.0f + 398.0f * f;
  }
  const char* stay = nullptr;
  if (plane && height > 4.0f && height < 35.0f) stay = "too low for the parachute";
  if (!stay && hp > 0.06f) {
    if (bot.target_vehicle) {
      const float theirs = VehicleHealth(base, bot.target_vehicle);
      if (theirs >= 0.0f && theirs < 0.25f && theirs < hp) stay = "finishing an enemy vehicle";
    }
    for (const Flag& f : g.flags) {
      if (stay) break;
      if (!f.takeable || !f.entity || !Readable(f.entity, 0xA4) || FlagOwner(base, f) == team) continue;
      if (int(Load32(base, f.entity + 0x80)) == team && LoadF(base, f.entity + 0xA0) >= 75.0f &&
          InCaptureArea(base, f, bot.player, soldier))
        stay = "nearly taken a flag";
    }
  }
  if (!stay) {
    int nearest = -1;
    float nd = 1e9f;
    for (size_t i = 0; i < g.flags.size(); ++i)
      if (g.flags[i].takeable && Dist2D(at, g.flags[i].pos) < nd) nd = Dist2D(at, g.flags[i].pos), nearest = int(i);
    const auto c = nav.cells.find(CellKey(CellOf(at[0]), CellOf(at[2])));
    const bool sea = c == nav.cells.end() || SwimCell(c->second);
    if (sea && nd > 300.0f && (nearest < 0 || StraightWater(at, g.flags[size_t(nearest)].pos) > 150.0f))
      stay = "open sea, no flag near";
  }
  if (stay) {
    if (now >= bot.stay_log) {
      bot.stay_log = now + std::chrono::seconds(5);
      REXLOG_INFO("BF bots: {} stays in {} at {:.0f}% health (falling {:.0f}% a second): {}", bot.name,
                  VehicleName(base, vehicle), hp * 100, bot.hp_rate * 100, stay);
    }
    return false;
  }
  bot.bailing = true, bot.bail_since = now;
  bot.press_until = now + std::chrono::milliseconds(250);
  bot.next_press = now + std::chrono::milliseconds(1500);
  if (plane && height >= 35.0f) {
    bot.chute_until = now + std::chrono::seconds(20), bot.chute_next = now + std::chrono::milliseconds(400);
    bot.chute_tries = 0, bot.chute_y_at = {}, bot.fall_speed = 0;
  } else {
    bot.chute_until = {};
  }
  bot.off_vehicle = vehicle, bot.off_until = now + std::chrono::seconds(60);
  REXLOG_INFO("BF bots: {} bails out of {} at {:.0f}% health (falling {:.0f}% a second){}", bot.name,
              VehicleName(base, vehicle), hp * 100, bot.hp_rate * 100,
              plane ? fmt::format(", {:.0f} m up", height) : std::string());
  return true;
}
}  // namespace

// The game's team pick (sub_82399AE8(player, team, find a squad, keep the cap)): a bot stays on its own side. The
// round's start re-picks every player's team (run bots_step0: two Japanese bots on the US carrier), and a bot's name
// and kit are its side's.
// A team's cap (sub_82375918, read only by the team pick): the level's TeamData says 12 (the game's own 24, 12 a
// side). On a bigger server (server sizes, docs/SERVER-SIZES.md) it is half the size and 2 more, so both sides fill
// (run sbstudy8: on a 32 player server six bots got no team, and the scoreboards listed 12 a side). The pick still
// puts each player on the side with fewer.
REX_EXTERN(__imp__sub_82375918);
REX_HOOK_RAW(sub_82375918) {
  __imp__sub_82375918(ctx, base);
  if (REXCVAR_GET(server)) {
    const int size = std::clamp(int(REXCVAR_GET(bf_gm_max_players)), 2, bf1943::kMaxServerSize);
    if (size > bf1943::kDefaultServerSize) {
      const uint32_t want = uint32_t((size + 1) / 2 + 2);
      if (ctx.r3.u32 < want) {
        static bool told = false;
        if (!told) {
          told = true;
          REXLOG_INFO("BF server size: a team's cap {} -> {}", ctx.r3.u32, want);
        }
        ctx.r3.u64 = want;
      }
    }
  }
}

REX_EXTERN(__imp__sub_82399AE8);
REX_HOOK_RAW(sub_82399AE8) {
  if (REXCVAR_GET(server)) {
    bool bot = false, leaving = false;
    for (const Bot& b : g.bots) {
      leaving = leaving || b.leaving;
      if (b.player != ctx.r3.u32) continue;
      bot = true;
      if (b.team && int(ctx.r4.s32) != b.team) ctx.r4.u64 = uint32_t(b.team);
    }
    // Backfill: a bot on its way out (it leaves at its next death) still holds its team's place; a human player's
    // pick then passes over the team's cap, so a joining player always gets a side (the game takes up to 64).
    if (!bot && leaving && ctx.r6.u32) {
      REXLOG_INFO("BF bots: a player's team pick passes over the team's cap while a bot is leaving");
      ctx.r6.u64 = 0;
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
    g.ended_vehicles.erase(vehicle);
  }
}
REX_EXTERN(__imp__sub_822FF5B0);
REX_HOOK_RAW(sub_822FF5B0) {
  if (REXCVAR_GET(server)) {
    std::lock_guard lock(g.mutex);
    g.vehicles.erase(ctx.r3.u32);
    if (g.ended_vehicles.size() < 4096) g.ended_vehicles.insert(ctx.r3.u32);
  }
  __imp__sub_822FF5B0(ctx, base);
}

// A player's entry tracker (player+228) as it moves to a new entity (sub_823EEBC8(tracker, entity, seat): the soldier at
// a spawn, a vehicle's seat): it first leaves the one it was in (+76), through that entity's listener list (+68). A
// vehicle destroyed with the player still on record there left that pointer to freed memory, and the next spawn wrote
// through it (run lead1: the server's crash at a bot's spawn, in sub_822B4DE0 from here, its plane gone): a vehicle
// ended since is forgotten first, as if the player had left it.
REX_EXTERN(__imp__sub_823EEBC8);
REX_HOOK_RAW(sub_823EEBC8) {
  if (REXCVAR_GET(server) && Readable(ctx.r3.u32, 96)) {
    const uint32_t tracker = ctx.r3.u32, before = Load32(base, tracker + 76);
    bool ended = false;
    if (before) {
      std::lock_guard lock(g.mutex);
      ended = g.ended_vehicles.count(before) != 0;
    }
    if (ended || (before && (!Readable(before, 0x80) || !InImage(Load32(base, before))))) {
      REXLOG_INFO("BF bots: a player's record of the vehicle {:08X} it was in, ended, forgotten", before);
      Store32(base, tracker + 76, 0);
      Store32(base, tracker + 80, 0);
    }
  }
  __imp__sub_823EEBC8(ctx, base);
}

REX_HOOK_RAW(sub_82362A40) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_82362A40(ctx, base);
  if (!REXCVAR_GET(server)) return;
  std::lock_guard lock(g.mutex);
  g.spawns.insert(self);
}

// The level's TeamData objects (vtable 0x82062498), for the bots' kits (LiveTeamKits): made by sub_82A72598 (this, as
// a new one: cap 12 at +24, squad size 4 at +28) or sub_82A72600 (a copy: the object is r4), gone with sub_827796E0.
REX_EXTERN(__imp__sub_82A72598);
REX_HOOK_RAW(sub_82A72598) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_82A72598(ctx, base);
  if (!REXCVAR_GET(server)) return;
  std::lock_guard lock(g.mutex);
  g.team_data.insert(self);
}
REX_EXTERN(__imp__sub_82A72600);
REX_HOOK_RAW(sub_82A72600) {
  const uint32_t self = ctx.r4.u32;
  __imp__sub_82A72600(ctx, base);
  if (!REXCVAR_GET(server) || !self) return;
  std::lock_guard lock(g.mutex);
  g.team_data.insert(self);
}
REX_EXTERN(__imp__sub_827796E0);
REX_HOOK_RAW(sub_827796E0) {
  const uint32_t self = ctx.r3.u32;
  if (REXCVAR_GET(server)) {
    std::lock_guard lock(g.mutex);
    g.team_data.erase(self);
  }
  __imp__sub_827796E0(ctx, base);
}

namespace bf1943 {

// Team balance (team_balance.cpp): whether a listed player is one of our bots (balance counts the real players).
bool IsBotPlayer(uint32_t player) { return IsBot(player); }
// Team balance moving a real player to `team`, which the game refused (the side at its cap): a bot there leaves to even
// the teams (a dead one first, else at its next death), as the even teams rule above does. While a bot is leaving, a
// real player's team pick passes over the cap (the sub_82399AE8 hook), so the move then goes through; the bot's place
// is filled on the other side. True when a bot of that side is leaving (now or already).
bool BotsMakeRoom(uint8_t* base, int team) {
  Bot* pick = nullptr;
  for (Bot& b : g.bots) {
    if (int(Load32(base, b.player + 48)) != team) continue;
    if (b.leaving) return true;
    if (b.life_at == Clock::time_point{}) continue;  // not spawned yet: never taken out (the even teams rule's note)
    if (!pick || (!Load32(base, b.player + 288) && Load32(base, pick->player + 288))) pick = &b;
  }
  if (!pick) return false;
  pick->leaving = true, pick->evening = true;
  REXLOG_INFO("BF bots: {} will leave to make room on {} for a real player (team balance)", pick->name,
              team == 1 ? "the US" : "Japan");
  return true;
}

// A kill (server_stats.cpp's hook on the game's kill handling, before it): where the killer stood and where the victim
// fell, by side (a human killer's counts 1, a bot's 0.5).
// A player as the chat names it: its name, team, squad (the chat's: 0 none, else the game's + 1), whether a bot and a
// squad leader (bit 0x80 of +132), and a bot's goal flag by its title.
botchat::Who ChatWho(const uint8_t* base, uint32_t p) {
  botchat::Who w;
  if (!Readable(p, 0x220)) return w;
  w.name = PlayerName(base, p);
  w.team = int(Load32(base, p + 48));
  const uint32_t sq = Load32(base, p + 52);
  w.squad = sq < 24 ? int(sq) + 1 : 0;
  w.leader = (base[p + 132] & 0x80) != 0;
  for (const Bot& b : g.bots)
    if (b.player == p) {
      w.bot = true;
      if (b.goal >= 0 && size_t(b.goal) < g.flags.size()) w.goal = CapturePointTitle(g.flags[size_t(b.goal)].name);
    }
  return w;
}
std::vector<botchat::Who> ChatBots(const uint8_t* base) {
  std::vector<botchat::Who> out;
  for (const Bot& b : g.bots) out.push_back(ChatWho(base, b.player));
  return out;
}

void BotsNoteKill(PPCContext& ctx, uint8_t* base, uint32_t killer, uint32_t victim) {
  // The weapon (the kill handler's r7, sub_82409708; ctx is the hook's): named for a bot's kills.
  if (REXCVAR_GET(server) && killer && killer != victim && IsBot(killer)) {
    const uint32_t w = ctx.r7.u32;
    std::string name = DataName(base, w);
    if (name.empty() && Readable(w, 0x18)) name = DataName(base, Load32(base, w + 0x14));
    REXLOG_INFO("BF bots: {} killed {} with {}", PlayerName(base, killer), PlayerName(base, victim),
                name.empty() ? fmt::format("{:08X}", w) : name);
  }
  if (REXCVAR_GET(server) && Readable(victim, 0x220)) {  // the chat's word on it (bot_chat.cpp)
    float a[3] = {}, b[3] = {};
    const uint32_t ks = killer && Readable(killer, 0x220) ? Load32(base, killer + 288) : 0;
    const uint32_t vs = Load32(base, victim + 288);
    const float d = ks && vs && Position(ctx, base, ks, a) && Position(ctx, base, vs, b) ? Dist2D(a, b) : 0.0f;
    const bool vehicle = ks && !VehicleKey(base, killer).empty();
    botchat::Kill(killer ? ChatWho(base, killer) : botchat::Who{}, ChatWho(base, victim), d, vehicle, ChatBots(base));
  }
  if (REXCVAR_GET(server) && killer && killer != victim && Readable(killer, 0x140) && Readable(victim, 0x140)) {
    const int ks = int(Load32(base, killer + 48)), vs = int(Load32(base, victim + 48));
    if (ks != vs && (ks == 1 || ks == 2) && (vs == 1 || vs == 2)) {
      const bool killer_bot = IsBot(killer), victim_bot = IsBot(victim);
      if (victim_bot && !killer_bot) pressure.human_kills[vs] += 1;
      if (killer_bot && !victim_bot) pressure.bot_kills[ks] += 1;
    }
  }
  if (!REXCVAR_GET(server) || learn.map.empty() || !Readable(victim, 0x140)) return;
  const int vside = int(Load32(base, victim + 48));
  float vp[3] = {};
  const uint32_t vso = Load32(base, victim + 288);
  if (vso && Position(ctx, base, vso, vp) && (vside == 1 || vside == 2))
    Note([&](Learned& l) { l.deaths[vside - 1][CellKey(CellOf(vp[0]), CellOf(vp[2]))] += 1.0f; });
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
        Note([&](Learned& l) {
          auto& vu = l.vehicles[vkey];
          vu.kills += 1, vu.kill_dist += std::min(Dist2D(at, vp), 1000.0f);
          vu.kills_at[CellKey(CellOf(at[0]), CellOf(at[2]))] += 1.0f;
        });
        REXLOG_INFO("BF bots: a player's kill from {} at {:.0f} m", vkey, Dist2D(at, vp));
      }
    }
    return;
  }
  if (kso && kso != vso && Position(ctx, base, kso, kp) && (kside == 1 || kside == 2) && kside != vside) {
    const float kw = IsBot(killer) ? 0.5f : 1.0f;
    Note([&](Learned& l) { l.kills[kside - 1][CellKey(CellOf(kp[0]), CellOf(kp[2]))] += kw; });
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
  vehicle_kinds.clear();
  kit_weapons.clear();
  pressure = Pressure{};
  if (learn.dirty) SaveLearned();
  if (!learn.map.empty() && !nav.cells.empty()) SaveGrid();
  ShareLearned("level change");
  learn.map.clear();
  std::lock_guard lock(g.mutex);
  g.spawns.clear();
  // (g.team_data stays: the game's restart of a level keeps its TeamData, run kitcyc on US Central, 21 Nov 2026: no
  // constructor ran at three restarts of Wake; their constructors and destructor keep it.)
  g.vehicle_spawns.clear();
  // The bots stay: the game keeps its players through a level change, so a bot forgotten here stayed in its player
  // list undriven and was counted as a human (US Central, 0.4.8-test3, 20 Nov 2026: "room for 0 beside 22 human
  // player(s)" after Wake's second round). Each keeps its player, input, name and side and starts the level afresh.
  for (Bot& b : g.bots) {
    // Its kit was the old level's (soldier data at +500, freed with it): taken off, so the new level's setup reads
    // none of the old level's memory, and chosen again from the new level's kits before its next spawn.
    if (g.base && b.player && Readable(b.player, 0x210)) Store32(g.base, b.player + 500, 0);
    Bot fresh;
    fresh.player = b.player, fresh.input = b.input, fresh.state = b.state;
    fresh.index = b.index, fresh.team = b.team, fresh.name = b.name, fresh.leaving = b.leaving;
    std::memcpy(fresh.jitter, b.jitter, sizeof(fresh.jitter));
    fresh.died = Clock::now() - std::chrono::seconds(10);
    fresh.next_think = Clock::now() + std::chrono::milliseconds((b.index * 37) % 250);
    fresh.next_ride = Clock::now() + std::chrono::milliseconds((b.index * 53) % 500);
    b = fresh;
  }
  g.scanned_manager = 0;  // the new level's kits and flags found again
  g.flags.clear();
  g.orders.clear();  // (their flags were the old level's)
  g.leaders.clear();
  g.stranded.clear();
  g.level_seen = false;
  nav.cells.clear();
  nav.scan.clear(), nav.scanned.clear(), nav.scan_started = nav.scan_done = false;
  // The new level's water found on it (0.4.8 kept the last level's: Wake's sea, 215 m, on Guadalcanal, whose sea is at
  // 173, and on Iwo Jima: their land "under water", mapped as sea in no time (139 cells in 0 s), and the bots swam it).
  nav.water = -1e9f;
  field::Reset();
  nav.scan_min[0] = nav.scan_min[1] = 1e9f, nav.scan_max[0] = nav.scan_max[1] = -1e9f;
}

// The bots on the server now, for the server browser's PLAYERS column (TekRantGaming, 21 Nov 2026: "(4)0/24", the
// bots in brackets before the players): every bot on the scoreboard, those waiting on an empty server and one that is
// leaving for a joining player included (it plays until it next dies). The in-game host (gm_host.cpp) reads it here;
// a dedicated server's host (server/gmhost.py) from <user data>/bots.txt, "<count>", written at once when it changes
// and again every 10 s so the host can tell a live count from one a stopped server left (it takes one older than
// 30 s for none). Written from the server's first tick, 0 on a server without bots.
static std::atomic<int> g_bots_playing{0};
int BotsPlaying() { return g_bots_playing.load(); }
static void NoteBotsPlaying() {
  static Clock::time_point next{}, rewrite{};
  static int written = -1;
  const auto now = Clock::now();
  if (now < next) return;
  next = now + std::chrono::seconds(1);
  const int n = WantedBots() > 0 ? int(g.bots.size()) : 0;
  g_bots_playing.store(n);
  if (n == written && now < rewrite) return;
  const std::filesystem::path dir = REXCVAR_GET(user_data_root);
  if (dir.empty()) return;
  const std::filesystem::path file = dir / "bots.txt", tmp = dir / "bots.txt.new";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return;
    f << n << "\n";
  }
  std::error_code ec;
  std::filesystem::rename(tmp, file, ec);
  if (ec) return;
  if (n != written) REXLOG_INFO("BF bots: {} bot(s) on the server (bots.txt, for the server browser)", n);
  written = n, rewrite = now + std::chrono::seconds(10);
}

// Every server tick (server_rotation.cpp, after the game's own tick).
static void BotsTickImpl(PPCContext& ctx, uint8_t* base);
void BotsTick(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(server)) return;
  NoteBotsPlaying();
  if (WantedBots() <= 0) return;
#if defined(BF_DEV_TOOLS)
  // BF_BOT_DETAIL=1 (tests): bf_bots_log_detail on in a test's hosted server (its moves, sights, flags, deaths).
  static const bool detail = [] {
    const char* v = std::getenv("BF_BOT_DETAIL");
    if (v && *v == '1') rex::cvar::SetFlagByName("bf_bots_log_detail", "true");
    return true;
  }();
  (void)detail;
#endif
  const auto t0 = Clock::now();
  BotsTickImpl(ctx, base);
  const auto t1 = Clock::now();
  const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  cost.total += ms, cost.worst = std::max(cost.worst, ms), ++cost.ticks;
  if (cost.since == Clock::time_point{}) cost.since = t1;
  if (t1 - cost.since >= std::chrono::seconds(30)) {
    const double n = std::max(1, cost.ticks);
    REXLOG_INFO("BF bots: cost over {:.0f} s with {} bots: {:.3f} ms a tick on average, {:.1f} ms the worst; learning "
                "{:.3f}, mapping {:.3f}, thinking {:.3f}, steering {:.3f} (the game's input step {:.3f}), saving {:.3f} ms a "
                "tick; {} path searches",
                std::chrono::duration<double>(t1 - cost.since).count(), g.bots.size(), cost.total / n, cost.worst,
                cost.learn / n, cost.scan / n, cost.think / n, cost.drive / n, cost.input_step / n, cost.save / n, cost.searches);
    cost = BotCost{};
    cost.since = t1;
  }
}

static void BotsTickImpl(PPCContext& ctx, uint8_t* base) {
  g.base = base;
  const int wanted = WantedBots();
  const auto now = Clock::now();
  TickPositions();
  g.searches_left = kSearchesPerTick;
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
    // The level's kits and flags: scanned on a thread of their own (the optimization pass: the scans of guest memory
    // held the server's tick for about 2.3 s at each level's start); the bots wait for them.
    struct LevelScan {
      uint32_t manager = 0;
      std::map<uint32_t, std::vector<uint32_t>> kits;
      std::vector<Flag> flags;
      std::atomic<bool> done{false};
    };
    static std::shared_ptr<LevelScan> scan;
    if (!scan || scan->manager != manager) {
      scan = std::make_shared<LevelScan>();
      scan->manager = manager;
      std::thread([base, s = scan] {
        const auto t0 = Clock::now();
        ScanTeams(base, s->kits);
        ScanFlags(base, s->flags);
        REXLOG_INFO("BF bots: the level's kits and flags found in {:.0f} ms (on a thread of its own)",
                    std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
        s->done = true;
      }).detach();
    }
    if (!scan->done) return;
    g.team_kits = scan->kits;
    g.flags = scan->flags;
    scan.reset();
    // The kits of the live TeamData (LiveTeamKits above) in place of the scan's, which can find one left in freed
    // memory; the scan's only when no constructor was seen. Both logged, to tell them apart.
    {
      std::string where;
      const auto live = LiveTeamKits(base, &where);
      auto list = [](const std::map<uint32_t, std::vector<uint32_t>>& m) {
        std::string s;
        for (const auto& [team, kits] : m) {
          s += fmt::format(" team {}:", team);
          for (uint32_t k : kits) s += fmt::format(" {:08X}", k);
        }
        return s;
      };
      if (live.empty()) {
        REXLOG_WARN("BF bots: no live TeamData seen ({}): the scan's kits{}", where.empty() ? "none" : where,
                    list(g.team_kits));
      } else {
        REXLOG_INFO("BF bots: kits of the live TeamData ({}):{}; the scan's{}{}", where, list(live), list(g.team_kits),
                    live == g.team_kits ? " (the same)" : ": the scan's not taken");
        g.team_kits = live;
      }
      // The soldier data's vtable, when every kit's agrees (the first level's, kept: the class does not change).
      if (!g.soldier_data_vtable) {
        std::set<uint32_t> vtables;
        for (const auto& [team, kits] : g.team_kits)
          for (uint32_t k : kits)
            if (const uint32_t data = Readable(k, 0x40) ? Load32(base, k + 0x3C) : 0; Readable(data, 4))
              vtables.insert(Load32(base, data));
        if (vtables.size() == 1 && InImage(*vtables.begin())) g.soldier_data_vtable = *vtables.begin();
      }
      REXLOG_INFO("BF bots: the level's kits and their soldier data:{} (the soldier data's vtable {:08X})",
                  KitsText(base, g.team_kits), g.soldier_data_vtable);
      for (const auto& [team, kits] : g.team_kits)
        for (uint32_t k : kits)
          if (const uint32_t data = Readable(k, 0x40) ? Load32(base, k + 0x3C) : 0; data)
            REXLOG_INFO("BF bots: kit {} weapons:{}", GuestText(base, Load32(base, k + 0x0C), Load32(base, k + 0x10)),
                        WeaponsOf(base, data).text);
    }
    g.scanned_manager = manager;
    // What earlier matches learned here: by the map's title (the rotation's), else its first flag's name.
    std::string map = CurrentMapTitle();
    g.sit_out = SitsOut(map, g.flags);
    if (g.sit_out)
      REXLOG_INFO("BF bots: Air Superiority ({}): the bots sit this level out, they cannot fly",
                  map.empty() ? "no rotation, by its flag" : map);
    if (map.empty() && !g.flags.empty()) map = g.flags.front().name;
    if (!map.empty() && map != learn.map) {
      if (learn.dirty) SaveLearned();
      ShareLearned("level change");
      // The level's water height known from the start: the server has no water mesh (a player's game logs it, "BF
      // level <name>: its water at ..."), and until a bot stood near the water the mapping took the seabed within a
      // step's reach for ground (run botrot2: Guadalcanal's scan box all ground, its sea included).
      if (const float known = KnownWater(MapFileName(map)); known > nav.water) nav.water = known;
      LoadLearned(map);
      LoadGrid();
      LoadStyle();
      recent_deaths[0].clear(), recent_deaths[1].clear(), recent_fire[0].clear(), recent_fire[1].clear();
    }
  }
  // Bots still listed (a player the game dropped is no longer ours to drive).
  std::vector<uint32_t> listed = ListedPlayers(base, manager);
  bool removed = false;
  for (auto it = g.bots.begin(); it != g.bots.end();) {
    if (std::find(listed.begin(), listed.end(), it->player) == listed.end()) {
      REXLOG_INFO("BF bots: {} is no longer in the player list", it->name);
      it = g.bots.erase(it);
    } else if (it->leaving && !Load32(base, it->player + 288)) {
      // Backfill: out through the game's own removal, as a player who left; dead, so no soldier of its own is left
      // behind. Its input is ours (Alloc): taken off the player first and freed after.
      REXLOG_INFO("BF bots: {} leaves ({})", it->name,
                  g.sit_out ? "the bots sit this level out" : it->evening ? "to even the teams" : "room for a player");
      const uint32_t input = it->input, state = it->state;
      Store32(base, it->player + 280, 0);
      if (const uint32_t scoring = Load32(base, kGameContext + 68); Readable(scoring, 16)) {
        PPCContext left = ctx;
        left.r3.u64 = scoring, left.r4.u64 = it->player;
        sub_82410950(left, base);
      }
      PPCContext call = ctx;
      call.r3.u64 = manager, call.r4.u64 = it->player;
      sub_82387FA0(call, base);
      if (input) REX_KERNEL_MEMORY()->SystemHeapFree(input);
      if (state) REX_KERNEL_MEMORY()->SystemHeapFree(state);
      it = g.bots.erase(it);
      removed = true;
    } else {
      ++it;
    }
  }
  // (A bot that left is no longer listed: the list read again, or it would count as a human now.)
  if (removed) listed = ListedPlayers(base, manager);
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
    g.humans = humans;
    // Backfill (TekRantGaming, 20 Nov 2026: "when a player joins, they'll replace a bot that's in the game, and when
    // a player leaves, a bot will fill their slot"): the bots take the room the humans leave but kBackfillSpare slots,
    // so a joining player always gets in and gets a team; past that, bots leave (the side with more players first).
    const int room = std::max(0, int(REXCVAR_GET(bf_gm_max_players)) - humans - kBackfillSpare);
    const int target = g.sit_out ? 0 : std::min(wanted, room);
    {
      int staying = 0;
      for (const Bot& b : g.bots) staying += b.leaving ? 0 : 1;
      while (staying > target) {
        // The side with more players gives one up: a dead bot first (it leaves at once), else a living one (when it
        // next dies; until then it plays on).
        const int more = side[1] >= side[2] ? 1 : 2;
        Bot* pick = nullptr;
        for (Bot& b : g.bots) {
          if (b.leaving || int(Load32(base, b.player + 48)) != more) continue;
          if (!pick || (!Load32(base, b.player + 288) && Load32(base, pick->player + 288))) pick = &b;
        }
        if (!pick)
          for (Bot& b : g.bots)
            if (!b.leaving) {
              pick = &b;
              break;
            }
        if (!pick) break;
        pick->leaving = true;
        --side[int(Load32(base, pick->player + 48)) == 2 ? 2 : 1];
        --staying;
        if (!g.sit_out)
          REXLOG_INFO("BF bots: {} will leave for a player ({} humans, {} bots wanted, room for {})", pick->name, humans,
                      wanted, target);
      }
      // A player left again: a bot that was leaving and is still here stays after all.
      for (Bot& b : g.bots)
        if (b.leaving && !b.evening && staying < target) {
          b.leaving = false, ++staying;
          REXLOG_INFO("BF bots: {} stays after all (a player left)", b.name);
        }
    }
    static int told = -1;
    if (!g.sit_out && target < wanted && told != target) {
      told = target;
      REXLOG_INFO("BF bots: {} wanted, room for {} beside {} human player(s) ({} at most)", wanted, target, humans,
                  REXCVAR_GET(bf_gm_max_players));
    }
    // Even teams (TekRantGaming, 27 Nov 2026, hotfix 0.4.8.1: "one team can have lots of real players, and the other
    // team can have like 3 real players and the rest filled by bots. We need to be able to team balance between real
    // players and bots as well"): team balance (team_balance.cpp) evens the real players; the bots keep the totals even.
    // A side with 2 or more players more than the other gives up one bot (a dead one first, else one at its next
    // death), and the fill below puts a new bot on the other side once it has left. One at a time.
    // Only with real players on (the bots wait otherwise), at most one every 10 s, and never a bot that has not
    // spawned yet (test bal1: one taken out in the tick it was made crashed the server in the game's removal).
    if (!g.sit_out && humans > 0 && now - g.evened_at >= std::chrono::seconds(10)) {
      bool pending = false;
      for (const Bot& b : g.bots) pending = pending || (b.leaving && b.evening);
      if (!pending && std::abs(side[1] - side[2]) >= 2) {
        const int more = side[1] > side[2] ? 1 : 2;
        Bot* pick = nullptr;
        for (Bot& b : g.bots) {
          if (b.leaving || b.life_at == Clock::time_point{} || int(Load32(base, b.player + 48)) != more) continue;
          if (!pick || (!Load32(base, b.player + 288) && Load32(base, pick->player + 288))) pick = &b;
        }
        if (pick) {
          pick->leaving = true, pick->evening = true, g.evened_at = now;
          REXLOG_INFO("BF bots: {} will leave to even the teams ({} US, {} Japan); a bot joins {} in its place", pick->name,
                      side[1], side[2], more == 1 ? "Japan" : "the US");
        }
      }
    }
    while (int(g.bots.size()) < target) {  // (the leaving ones are counted: they are still in the list)
      const int team = side[1] <= side[2] ? 1 : 2;
      g.making = true;
      const bool made = MakeBot(ctx, base, manager, int(g.bots.size()), team, listed);
      g.making = false;
      if (!made) break;
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
  if (now - ray_log > std::chrono::seconds(60)) {
    if (ray_log != Clock::time_point{})
      REXLOG_INFO("BF bots: {} ray tests in 60 s, {:.3f} ms each; {} grid cells known", g.rays,
                  g.rays ? g.ray_ms / double(g.rays) : 0.0, nav.cells.size());
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
  const auto learn_t0 = Clock::now();
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
    if (!so || Load32(base, p + 312) != so || !CachedPosition(ctx, base, so, s.pos)) continue;
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
      NoteSharePlayer(base, p);  // (the hand-off's count of players: a hash, in memory only)
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
        if (enemy_near)
          Note([&](Learned& l) { l.waits[me.side - 1][CellKey(CellOf(me.pos[0]), CellOf(me.pos[2]))] += float(dt); });
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
      Note([&](Learned& l) { l.vehicles[key].time += dt; });
      if (now - sampled[p] < std::chrono::milliseconds(500)) continue;
      sampled[p] = now;
      float at[3];
      if (!Position(ctx, base, veh, at)) continue;
      Note([&](Learned& l) {
        auto& c = l.vehicles[key].cells[CellKey(CellOf(at[0]), CellOf(at[2]))];
        c.first += at[1], c.second += 1;
      });
      static std::set<std::string> told;
      if (told.insert(key).second) REXLOG_INFO("BF bots: a player in {} (learning its way)", key);
    }
  }
  cost.learn += std::chrono::duration<double, std::milli>(Clock::now() - learn_t0).count();
  if (learn.dirty && now - learn.saved_at > std::chrono::seconds(20)) {
    CostTimer t(&cost.save);
    SaveLearned();
  }
  if (REXCVAR_GET(bf_bots_share) &&
      now - shared_at > std::chrono::minutes(std::max(1, REXCVAR_GET(bf_bots_share_minutes)))) {
    CostTimer t(&cost.save);
    ShareLearned("every " + std::to_string(std::max(1, REXCVAR_GET(bf_bots_share_minutes))) + " minutes");
  }
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
    bool scanned;
    {
      CostTimer t(&cost.scan);
      scanned = ScanTick(ctx, base, any_soldier, g.humans > 0 ? kScanMsPlayed : kScanMsAlone);
    }
    if (scanned && now - grid_saved > std::chrono::seconds(60)) {
      grid_saved = now;
      CostTimer t(&cost.save);
      SaveGrid();
      REXLOG_INFO("BF bots: mapping: {} cells known, {} to look at", nav.cells.size(), nav.scan.size());
    }
    CostTimer t(&cost.scan);
    field::Tick(learn.map);  // the ways to the flags over the whole level (its snapshot here, the work on a thread)
  }
  // Squads (the plan's step 2d; the vehicle bindings session for the user, 18 Nov 2026: "make bots join the game's own
  // squads ... so a human player ends up in a squad with bots"). The game's team pick looks only for an open squad
  // (sub_82394FE0) and makes one only for a player with +592 and +596 set, which a bot has not: every bot stayed in
  // squad 24 (none), and so did the players. Every 2 s: a bot in no squad goes into a squad of its side with room (4
  // a squad: TeamData's MaxPlayersPerSquad), one with a human first, then one with bots, else a squad no one uses; a
  // human in no squad goes into a squad of its side with bots and leads it (bf_bots_squad_humans, off by default:
  // players choose their own, and bots fill a squad a player made first). A squad with a human in it is led by a human:
  // a player who joins a squad a bot leads takes the lead from it.
  // The chat (bot_chat.cpp): the flags' owners once a second, then its own tick (answers, lines due).
  {
    static Clock::time_point flags_at{};
    const std::vector<botchat::Who> chat_bots = ChatBots(base);
    if (now - flags_at > std::chrono::seconds(1)) {
      flags_at = now;
      std::vector<std::pair<std::string, int>> owners;
      for (const Flag& f : g.flags)
        if (f.takeable) owners.push_back({CapturePointTitle(f.name), FlagOwner(base, f)});
      botchat::Flags(owners, chat_bots);
    }
    botchat::Tick(chat_bots);
  }
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
#if defined(BF_DEV_TOOLS)
      // BF_BOT_JOINSQUAD=1 (tests): the player joins a bot squad as a player would, its lead not given here (the
      // promotion below takes it).
      static const bool dev_join = std::getenv("BF_BOT_JOINSQUAD") != nullptr;
#else
      constexpr bool dev_join = false;
#endif
      if (human && !REXCVAR_GET(bf_bots_squad_humans) && !dev_join) continue;
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
      if (human && !dev_join) {
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
    // The pressure on each side's bots and their aggression (Pressure), and a leader's call when it reaches half.
    {
      const double el = pressure.at == Clock::time_point{} ? 0.0 : std::chrono::duration<double>(now - pressure.at).count();
      pressure.at = now;
      const double keep = std::exp(-el / 173.0);  // half gone in 2 minutes
      int humans_on[3] = {}, held[3] = {}, takeable = 0;
      for (uint32_t q : listed) {
        const int t = int(Load32(base, q + 48));
        if ((t == 1 || t == 2) && !IsBot(q)) ++humans_on[t];
      }
      for (const Flag& f : g.flags)
        if (f.takeable) ++takeable, ++held[FlagOwner(base, f)];
      for (int side = 1; side <= 2; ++side) {
        pressure.human_kills[side] *= keep, pressure.bot_kills[side] *= keep;
        const int other = 3 - side;
        float want = 0;
        if (REXCVAR_GET(bf_bots_dynamic_aggression) && humans_on[other] > 0) {
          const double hk = pressure.human_kills[side], bk = pressure.bot_kills[side];
          const float kills = float((hk - bk) / (hk + bk + 4.0));
          const float flags = takeable ? float(held[other] - held[side]) / float(takeable) : 0.0f;
          want = std::clamp((0.6f * kills + 0.6f * flags - 0.15f) / 0.6f, 0.0f, 1.0f);
        }
        pressure.target[side] = want;
        float& lv = pressure.level[side];
        lv = want > lv ? std::min(want, lv + float(el) / 90.0f) : std::max(want, lv - float(el) / 180.0f);
#if defined(BF_DEV_TOOLS)
        // BF_BOT_AGGRO=<0..1> (tests): both sides' aggression held there (a test's player cannot get ahead of the bots).
        if (static const char* forced = std::getenv("BF_BOT_AGGRO"); forced)
          lv = std::clamp(float(std::atof(forced)), 0.0f, 1.0f);
#endif
        if (const int q = int(lv * 4.0f + 0.001f); q != pressure.quarter[side]) {
          pressure.quarter[side] = q;
          REXLOG_INFO("BF bots: {}'s aggression {:.2f} (the players' kills of its bots {:.1f}, its bots' of players {:.1f}; "
                      "flags {} to {} of {})",
                      side == 1 ? "the US" : "Japan", lv, pressure.human_kills[side], pressure.bot_kills[side],
                      held[side], held[other], takeable);
        }
        if (lv >= 0.5f && !pressure.rallied[side]) {
          for (const auto& [key, lead] : g.leaders)
            if (key.first == side && IsBot(lead)) {
              botchat::Rally(ChatWho(base, lead));
              pressure.rallied[side] = true;
              break;
            }
        } else if (lv < 0.25f) {
          pressure.rallied[side] = false;
        }
        auto& cs = pressure.contacts[side];
        cs.erase(std::remove_if(cs.begin(), cs.end(), [&](const Pressure::Contact& c) { return now - c.at > std::chrono::seconds(6); }),
                 cs.end());
      }
    }
    // Each squad's leader now, and a bot leader's order: the side's bot-led squads share the flags out, a flag no other
    // squad of the side has before one that one has (an enemy or nobody's to take, or one of the side's own with an
    // enemy within 60 m of it to hold), the nearest to the leader among those; an order holds until its flag is taken
    // (an attack) or quiet (a defence), or 45 s. A player's squad gets no order: its bots stay with the player.
    {
      g.leaders.clear();
      for (const auto& [key, m] : squads)
        for (uint32_t q : m)
          if (base[q + 132] & 0x80) g.leaders[key] = q;
      std::map<int, std::map<int, int>> taken;  // side -> flag -> squads on it
      for (auto it = g.orders.begin(); it != g.orders.end();) {
        const auto lead = g.leaders.find(it->first);
        if (lead == g.leaders.end() || !IsBot(lead->second) || it->second.flag < 0 || size_t(it->second.flag) >= g.flags.size())
          it = g.orders.erase(it);
        else
          ++it;
      }
      auto threatened = [&](int side, const Flag& f) {
        for (uint32_t q : listed) {
          const int t = int(Load32(base, q + 48));
          if (t == side || (t != 1 && t != 2)) continue;
          const uint32_t so = Load32(base, q + 288);
          float ep[3];
          if (so && Position(ctx, base, so, ep) && Dist2D(ep, f.pos) < 60.0f) return true;
        }
        return false;
      };
      for (auto& [key, order] : g.orders) {
        const Flag& f = g.flags[size_t(order.flag)];
        const int owner = FlagOwner(base, f);
        const bool done = order.defend ? !threatened(key.first, f) : owner == key.first;
        if (done || now >= order.until) order.flag = -1;
        else ++taken[key.first][order.flag];
      }
      for (const auto& [key, lead] : g.leaders) {
        if (!IsBot(lead)) continue;
        State::Order& order = g.orders[key];
        if (order.flag >= 0) continue;
        const uint32_t so = Load32(base, lead + 288);
        float lp[3];
        if (!so || !Position(ctx, base, so, lp)) continue;
        int best = -1;
        float best_score = 1e9f;
        bool best_defend = false;
        for (size_t i = 0; i < g.flags.size(); ++i) {
          const Flag& f = g.flags[i];
          if (!f.takeable) continue;
          const bool ours = FlagOwner(base, f) == key.first;
          if (ours && !threatened(key.first, f)) continue;
          // Pressing (the side's aggression, Pressure): squads join each other's attacks, and hold less.
          const float a = Aggression(key.first);
          const float score = Dist2D(lp, f.pos) + 400.0f * (ours ? 1.0f : std::max(0.0f, 1.0f - 1.4f * a)) * float(taken[key.first][int(i)]) +
                              (ours ? -50.0f + 150.0f * a : 0.0f);
          if (score < best_score) best_score = score, best = int(i), best_defend = ours;
        }
        if (best < 0) continue;
        order.flag = best, order.defend = best_defend, order.until = now + std::chrono::seconds(45);
        ++taken[key.first][best];
        REXLOG_INFO("BF bots: squad {} of {} ({} leads): {} '{}'", key.second, key.first == 1 ? "the US" : "Japan",
                    PlayerName(base, lead), best_defend ? "hold" : "take", g.flags[size_t(best)].name);
      }
    }
    // A squad with a human in it that a bot leads (a player joined a squad the bots made): the first human there leads.
    for (const auto& [key, m] : squads) {
      uint32_t human = 0;
      bool human_leads = false, bot_leads = false;
      for (uint32_t q : m) {
        const bool leads = (base[q + 132] & 0x80) != 0;
        if (IsBot(q)) {
          bot_leads = bot_leads || leads;
        } else {
          if (!human) human = q;
          human_leads = human_leads || leads;
        }
      }
      if (!human || human_leads || !bot_leads) continue;
      for (uint32_t q : m) {
        if (!IsBot(q) || !(base[q + 132] & 0x80)) continue;
        PPCContext call = ctx;
        call.r3.u64 = q, call.r4.u64 = 0, call.r5.u64 = (base[q + 132] >> 4) & 1;
        sub_82375280(call, base);
      }
      PPCContext call = ctx;
      call.r3.u64 = human, call.r4.u64 = 1, call.r5.u64 = (base[human + 132] >> 4) & 1;
      sub_82375280(call, base);
      REXLOG_INFO("BF bots: {} joined squad {} of {} and leads it now", PlayerName(base, human), key.second,
                  key.first == 1 ? "the US" : "Japan");
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
          if (now - t < std::chrono::seconds(60)) Note([&](Learned& l) { l.ways_in[f.name][step] += 1.0f; }), ++learned;
      }
      if (learned) {
        learn.dirty = true;
        REXLOG_INFO("BF bots: learned a way into '{}' ({} steps)", f.name, learned);
      }
    }
    holders[f.entity] = h;
  }
  // The path searches' rays: a time budget for this tick's thinking as well as a count.
  nav.ray_deadline = Clock::now() + std::chrono::microseconds(int64_t(kPathRayMs * 1000));
  // No human on the server (TekRantGaming, 20 Nov 2026: "Yes, pause them"): the bots stand still and do not think,
  // steer, spawn or learn until a player joins (about 10% of a core saved on an empty server with 24 bots; the bots
  // play as backfill all the time). The level's mapping goes on meanwhile, so a map is ready when players come.
  static const bool alone_env = [] {
    const char* v = std::getenv("BF_BOTS_PLAY_ALONE");
    return v && *v == '1';
  }();
  const bool pause = g.humans == 0 && !alone_env && !REXCVAR_GET(bf_bots_play_alone);
  static bool told_pause = false;
  if (pause != told_pause) {
    told_pause = pause;
    REXLOG_INFO("BF bots: {}", pause ? "no players on the server: the bots wait" : "a player is on the server: the bots play");
  }
  // The mapping's ray tests go through a soldier, so on an empty server at a level not mapped yet (a fresh server, or
  // the next level after the last player left) none could run and the level stayed unmapped until players came, who
  // then met bots routed over a part of it (hotfix 0.4.8.1). One bot spawns for it (and stands, paused like the rest).
  const Bot* mapper = nullptr;
  if (pause && !nav.scan_done && !learn.map.empty()) {
    bool any = false;
    for (const Bot& b : g.bots) any = any || Load32(base, b.player + 288) != 0;
    for (const Bot& b : g.bots)
      if (!any && !b.leaving) {
        mapper = &b;
        break;
      }
    static bool told_mapper = false;
    if (mapper && !told_mapper) {
      told_mapper = true;
      REXLOG_INFO("BF bots: no players and the level not mapped: {} spawns for the mapping", mapper->name);
    }
  }
  for (Bot& bot : g.bots) {
    const uint32_t soldier = Load32(base, bot.player + 288);
    if (pause && &bot != mapper) {
      // Its last orders let go once (no walking, firing or sprinting left held), then nothing until a player joins.
      if (!bot.paused && soldier && soldier == bot.last_soldier && Load32(base, bot.player + 312) == soldier) {
        bot.forward = bot.strafe = 0.0f;
        bot.fire = bot.sprint = bot.ads = false;
        bot.want_yaw = bot.yaw, bot.want_pitch = bot.pitch;
        Drive(ctx, base, bot, soldier, dt);
      }
      bot.paused = true;
      continue;
    }
    if (bot.paused) {  // a player joined: think again at once (spread over the ticks), a fresh plan
      bot.paused = false;
      bot.next_think = now + std::chrono::milliseconds((bot.index * 37) % 250);
      bot.replan_at = now;
      bot.died = std::min(bot.died, now - std::chrono::seconds(5));
    }
    if (soldier != bot.last_soldier) {
      if (!soldier) {
        bot.died = now;
        bot.kit_choice = -1;  // a kit chosen again for the next life
        if (REXCVAR_GET(bf_bots_log_detail)) REXLOG_INFO("BF bots: {} died", bot.name);
      } else {
        bot.life_at = now, bot.boated = false;
        bot.weapon_slot = 0, bot.select_slot = -1, bot.select_until = bot.switch_done = bot.heavy_next = {};
        bot.heavy_shots[0] = bot.heavy_shots[1] = bot.heavy_shots[2] = 0;
      }
      bot.last_soldier = soldier;
    }
    if (!soldier) {
      if (bot.leaving) continue;  // backfill: it leaves (above) instead of spawning again
      if (now - bot.died < std::chrono::seconds(5) || bot.spawn_tries > 20) continue;
      if (!ChooseKit(base, bot)) {
        // No live kit for its team (freed with a level the scan saw): the level's kits found again, no spawn now.
        if (!bot.kit_warned) {
          bot.kit_warned = true;
          REXLOG_WARN("BF bots: {} has no live kit of team {}: not spawned; the level's kits found again", bot.name,
                      Load32(base, bot.player + 48));
        }
        bot.died = now;
        g.scanned_manager = 0;
        continue;
      }
      bot.kit_warned = false;
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
      if (REXCVAR_GET(bf_bots_log_detail) || !spawned) {
        const uint32_t kit = Load32(base, bot.player + 500);
        REXLOG_INFO("BF bots: {} spawns at {:.1f} {:.1f} {:.1f}: {} (soldier data {:08X}, vtable {:08X})", bot.name, p[0],
                    p[1], p[2], !spawned ? "refused" : seated ? "in a vehicle" : "on foot", kit,
                    Readable(kit, 4) ? Load32(base, kit) : 0);
        static int told_refused = 0;
        if (!spawned && told_refused++ < 20) REXLOG_INFO("BF bots: the kits at that refusal:{}", KitsText(base, g.team_kits));
      }
      if (spawned) bot.spawn_tries = 0;
      continue;
    }
#if defined(BF_DEV_TOOLS)
    // BF_BOT_TANKTEST=1 (tests): the first bot is seated in the nearest Sherman's driver seat, then driven through set
    // inputs, 3 s each, its vehicle's place and heading logged (which analog concept steers, and which way).
    static const char* test_env = std::getenv("BF_BOT_VEHTEST");
    static const std::string test_vehicle = test_env ? test_env : (std::getenv("BF_BOT_TANKTEST") ? "Sherman" : "");
    static const bool tank_test = !test_vehicle.empty();
    if (tank_test && &bot == &g.bots.front()) {
      static Clock::time_point began{};
      const uint32_t controls = Load32(base, bot.player + 312);
      if (controls && controls != soldier) {
        if (began == Clock::time_point{}) began = now;
        const double t = std::chrono::duration<double>(now - began).count();
        static constexpr float kPhases[][8] = {
            {1, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0}, {1, 0, 0, 0, -1, 0, 0, 0}, {1, 0, 0, 0, 0, 0, 1, 0},
            {1, 0, 0, 0, 0, 0, -1, 0}, {0, 0, 0, 0, 0, 1, 0, 0}, {-1, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 1}};
        // A plane: full throttle 10 s (the take-off run), then each stick 2 s with full throttle, level 2 s between.
        // (run plane1: input 5 at +1 put the nose down, the plane dived in: here a climb first, short rolls after.)
        static constexpr float kPlane[][8] = {
            {1, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0, 0, 0},
            {1, 0, 0, 0, 0, -0.4f, 0, 0}, {1, 0, 0, 0, 0, -0.4f, 0, 0}, {1, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0, 0.5f, 0},
            {1, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0, -0.5f, 0}, {1, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0.5f, 0, 0, 0},
            {1, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, -0.5f, 0, 0, 0}, {1, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0, 0, 1}};
        const bool plane = test_vehicle != "Sherman";
        const int count = plane ? int(std::size(kPlane)) : int(std::size(kPhases));
        const int phase = std::min(int(t / (plane ? 2.0 : 3.0)), count - 1);
        float a[8];
        if (plane) std::copy(std::begin(kPlane[phase]), std::end(kPlane[phase]), a);
        else std::copy(std::begin(kPhases[phase]), std::end(kPhases[phase]), a);
        VehicleDrive(ctx, base, bot, dt, a);
        static Clock::time_point logged{};
        if (now - logged > std::chrono::milliseconds(500)) {
          logged = now;
          float at[3] = {}, yaw = 0;
          Position(ctx, base, controls, at);
          Heading(ctx, base, controls, &yaw);
          PPCContext mc = ctx;
          mc.r3.u64 = controls;
          sub_822A7BB0(mc, base);
          const uint32_t m = mc.r3.u32;
          const float right_y = Readable(m, 64) ? LoadF(base, m + 4) : 0, fwd_y = Readable(m, 64) ? LoadF(base, m + 36) : 0;
          REXLOG_INFO("BF bots tank test: phase {} (a0 {} a4 {} a5 {} a6 {} a7 {}) at {:.2f} {:.2f} {:.2f} heading {:.3f} "
                      "nose {:.3f} right_y {:.3f}",
                      phase, a[0], a[4], a[5], a[6], a[7], at[0], at[1], at[2], yaw, fwd_y, right_y);
        }
        continue;
      }
      static bool seated_once = false;
      if (!seated_once) {
        std::vector<uint32_t> vs;
        {
          std::lock_guard lock(g.mutex);
          vs.assign(g.vehicles.begin(), g.vehicles.end());
        }
        float me[3] = {};
        Position(ctx, base, soldier, me);
        uint32_t best = 0;
        float best_d = 1e9f;
        for (uint32_t v : vs) {
          float at[3];
          if (VehicleName(base, v) != test_vehicle || SeatPlayer(base, v, 0) || !Position(ctx, base, v, at)) continue;
          if (Dist2D(at, me) < best_d) best_d = Dist2D(at, me), best = v;
        }
        if (best) {
          seated_once = true;
          PPCContext call = ctx;
          call.r3.u64 = bot.player, call.r4.u64 = best, call.r5.u64 = 0;
          sub_82398B98(call, base);
          REXLOG_INFO("BF bots tank test: {} into the Sherman {:08X} {:.0f} m off: {}", bot.name, best, best_d,
                      Load32(base, bot.player + 312) == best ? "seated" : "refused");
        }
      }
    }
#endif
    // In a vehicle: a tank's or a plane's driver drives it (TankTick, PlaneTick); a boat's driver and any passenger
    // ride (Ride: the landing craft driven to the beach, and off where a passenger would get off).
    if (const uint32_t controls = Load32(base, bot.player + 312); controls && controls != soldier) {
      if (BailCheck(ctx, base, bot, controls, soldier, now))
        botchat::Bail(ChatWho(base, bot.player), IsPlane(VehicleName(base, controls)));
      if (Load32(base, bot.player + 292) == 0) {
        const std::string kind = VehicleName(base, controls);
        if (IsTank(kind)) {
          TankTick(ctx, base, bot, controls, soldier, listed, now, dt);
          continue;
        }
        if (IsPlane(kind)) {
          PlaneTick(ctx, base, bot, controls, soldier, listed, now, dt);
          continue;
        }
      }
      Ride(ctx, base, bot, controls, now, dt);
      continue;
    }
    if (const uint32_t carrier = StandingIn(ctx, base, bot, soldier, now)) {  // in a vehicle with its own weapon in hand
      StandingRide(ctx, base, bot, carrier, soldier, now, dt);
      continue;
    }
    if (bot.hp_vehicle) {
      if (bot.bailing) REXLOG_INFO("BF bots: {} got out of {}", bot.name, VehicleName(base, bot.hp_vehicle));
      bot.hp_vehicle = 0, bot.bailing = false;
    }
    if (bot.ride) {
      REXLOG_INFO("BF bots: {} is off {}", bot.name, VehicleName(base, bot.ride));
      bot.off_vehicle = bot.ride, bot.off_until = now + std::chrono::seconds(30);
      bot.ride = 0, bot.press_until = {};
      bot.boat_check = now + std::chrono::seconds(60);  // on its feet a while: no boat back out
    }
    // A free tank close by (it spawned at its base, by its side's tanks): the bot takes its driver's seat, two bots of
    // a side at most driving tanks. Looked for every 2 s.
#if defined(BF_DEV_TOOLS)
    static const bool no_ride_test = std::getenv("BF_BOT_WEAPONTEST") != nullptr;  // the test's bot stays on foot
    if (no_ride_test && &bot == &g.bots.front()) bot.next_ride = now + std::chrono::hours(1);
#endif
    if (now >= bot.next_ride && Rand01() < 0.25f) {
      int driving = 0, flying = 0;
      for (const Bot& o : g.bots) {
        const uint32_t oc = Load32(base, o.player + 312), os = Load32(base, o.player + 288);
        if (oc && oc != os && Load32(base, o.player + 292) == 0 && Load32(base, o.player + 48) == Load32(base, bot.player + 48))
          ++(IsPlane(VehicleName(base, oc)) ? flying : driving);
      }
      float me[3] = {};
      const int tank_cap = Aggression(int(Load32(base, bot.player + 48))) >= 0.5f ? 3 : 2;
      if ((driving < tank_cap || flying < 1) && Position(ctx, base, soldier, me)) {
        std::vector<uint32_t> vs;
        {
          std::lock_guard lock(g.mutex);
          vs.assign(g.vehicles.begin(), g.vehicles.end());
        }
        for (uint32_t v : vs) {
          float vp[3];
          const std::string kind = VehicleName(base, v);
          const bool tank = IsTank(kind), plane = IsPlane(kind);
          if ((!tank || driving >= tank_cap) && (!plane || flying >= 1)) continue;
          if (v == bot.off_vehicle && now < bot.off_until) continue;  // just got out of it
          if (const float hp = VehicleHealth(base, v); hp >= 0.0f && hp < 0.4f) continue;  // burning, or nearly
          if (SeatPlayer(base, v, 0) || !Position(ctx, base, v, vp) || Dist2D(vp, me) > (plane ? 40.0f : 20.0f)) continue;
          bot.plane_flying = false, bot.plane_last_at = {};
          PPCContext call = ctx;
          call.r3.u64 = bot.player, call.r4.u64 = v, call.r5.u64 = 0;
          sub_82398B98(call, base);
          REXLOG_INFO("BF bots: {} takes the {}'s driver's seat ({:.0f} m off): {}", bot.name, VehicleName(base, v),
                      Dist2D(vp, me), Load32(base, bot.player + 312) == v ? "seated" : "refused");
          break;
        }
      }
    }
    float pos[3] = {};
    if (!CachedPosition(ctx, base, soldier, pos)) continue;
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
        if (v == bot.off_vehicle && now < bot.off_until) continue;  // just got off it
        if (const float hp = VehicleHealth(base, v); hp >= 0.0f && hp < 0.3f) continue;  // about to go up
        const uint32_t driver = SeatPlayer(base, v, 0);
        if (!driver || IsBot(driver) || !Readable(driver, 0x40) || Load32(base, driver + 48) != side) continue;
        float at[3];
        if (!CachedPosition(ctx, base, v, at) || Dist2D(at, pos) > 6.0f || std::fabs(at[1] - pos[1]) > 4.0f) continue;
        // Every free seat in turn (the game refuses some: US Central 20 Nov, a landing craft's seats 3 to 5); none
        // taken: the enter/exit button, as a player at the boat's side would press it, the game's own pick.
        const uint32_t n = IsBoat(base, v) ? std::min<uint32_t>(SeatCount(ctx, base, v), 3) : SeatCount(ctx, base, v);
        bool seated = false, tried = false;
        for (uint32_t seat = 1; seat < n && !seated; ++seat) {
          if (SeatPlayer(base, v, seat)) continue;
          PPCContext call = ctx;
          call.r3.u64 = bot.player, call.r4.u64 = v, call.r5.u64 = seat;
          sub_82398B98(call, base);
          seated = Load32(base, bot.player + 312) == v;
          if (seated || !tried)
            REXLOG_INFO("BF bots: {} rides in {} (seat {}), {}", bot.name, VehicleName(base, v), seat,
                        seated ? "seated" : "refused");
          tried = true;
        }
        if (!seated && tried && now >= bot.next_press && !IsBoat(base, v)) {  // (a boat: see the boats below)
          bot.press_until = now + std::chrono::milliseconds(250);
          bot.next_press = now + std::chrono::milliseconds(1500);
          REXLOG_INFO("BF bots: {} presses enter at {}", bot.name, VehicleName(base, v));
        }
        break;
      }
      if (const uint32_t controls = Load32(base, bot.player + 312); controls && controls != soldier) continue;
    }
    // Boats, in the first 45 s of a life and once (the way off a carrier; ashore, a beached boat taken again was
    // driven at the land, run boatdrive8): a bot whose way to its flag swims 60 m or more (or with its flag 250 m
    // off, has no way there yet, or 150 m of the straight line is sea) takes a boat within 60 m instead (one with no driver and
    // no human player within 6 m of it, to drive; or one a bot of its side drives, to ride), walks to it and gets in
    // (the driver's seat if free, else a passenger's; refused: the enter/exit button). 25 s to reach it.
    if (!bot.boat && !bot.boated && now - bot.life_at < std::chrono::seconds(45) && now >= bot.boat_check &&
        bot.goal >= 0) {
      bot.boat_check = now + std::chrono::seconds(3);
      // Only for a long way (the beach's shallows count as water: a bot just landed 100 m from its flag took the
      // next beached boat back out, run boatdrive5).
      const bool far_off = Dist2D(pos, bot.goal_pos) > 250.0f;
      const bool no_way = bot.path.empty() || Dist2D(bot.path.back().data(), bot.goal_pos) > 50.0f;
      if (far_off && (no_way || SwimAhead(bot, pos) > 60.0f || StraightWater(pos, bot.goal_pos) > 150.0f)) {
        std::vector<uint32_t> vs;
        {
          std::lock_guard lock(g.mutex);
          vs.assign(g.vehicles.begin(), g.vehicles.end());
        }
        const uint32_t side = Load32(base, bot.player + 48);
        float best = 60.0f;
        for (uint32_t v : vs) {
          if ((v == bot.off_vehicle && now < bot.off_until) || !IsBoat(base, v)) continue;
          if (const auto st = g.stranded.find(v); st != g.stranded.end() && now < st->second) continue;
          float at[3];
          if (!CachedPosition(ctx, base, v, at)) continue;
          const float d = Dist2D(at, pos);
          if (d >= best) continue;
          const uint32_t driver = SeatPlayer(base, v, 0);
          if (driver && (!IsBot(driver) || !Readable(driver, 0x40) || Load32(base, driver + 48) != side)) continue;
          const uint32_t n = SeatCount(ctx, base, v);
          uint32_t taken = 0;
          for (uint32_t i = 0; i < n; ++i) taken += SeatPlayer(base, v, i) ? 1 : 0;
          if (taken >= n) continue;
          bool human_near = false;
          for (uint32_t q : listed) {
            if (driver || IsBot(q) || !Readable(q, 0x140)) continue;
            const uint32_t so = Load32(base, q + 288);
            float qp[3];
            if (so && CachedPosition(ctx, base, so, qp) && Dist2D(qp, at) < 6.0f) human_near = true;
          }
          if (human_near) continue;  // a player at its side is about to take it
          best = d, bot.boat = v;
        }
        if (bot.boat) {
          bot.boat_until = now + std::chrono::seconds(25), bot.presses = 0;
          REXLOG_INFO("BF bots: {} heads for {} ({:.0f} m): its flag {:.0f} m off, {:.0f} m of it sea", bot.name,
                      VehicleName(base, bot.boat), best, Dist2D(pos, bot.goal_pos), StraightWater(pos, bot.goal_pos));
        }
      }
    }
    if (bot.boat) {
      bool keep = now < bot.boat_until;
      {
        std::lock_guard lock(g.mutex);
        keep = keep && g.vehicles.count(bot.boat);
      }
      float at[3] = {};
      keep = keep && CachedPosition(ctx, base, bot.boat, at);
      const uint32_t driver = keep ? SeatPlayer(base, bot.boat, 0) : 0;
      keep = keep && (!driver || IsBot(driver));
      if (keep && Dist2D(at, pos) < 5.0f && std::fabs(at[1] - pos[1]) < 6.0f) {
        // Seats 0 to 2 only (the driver's and the two guns'): the server's seating sometimes takes a landing craft's
        // places 3 to 5 too, where the soldier keeps its own weapon and no button gets it out (run fx_boat5).
        const uint32_t n = std::min<uint32_t>(SeatCount(ctx, base, bot.boat), 3);
        for (uint32_t seat = 0; seat < n; ++seat) {
          if (SeatPlayer(base, bot.boat, seat)) continue;
          PPCContext call = ctx;
          call.r3.u64 = bot.player, call.r4.u64 = bot.boat, call.r5.u64 = seat;
          sub_82398B98(call, base);
          if (Load32(base, bot.player + 312) == bot.boat) {
            REXLOG_INFO("BF bots: {} gets in {} (seat {})", bot.name, VehicleName(base, bot.boat), seat);
            bot.boated = true;
            break;
          }
        }
        if (Load32(base, bot.player + 312) == bot.boat) {
          bot.boat = 0;
          continue;
        }
        // Every seat the server's seating takes is full (it refuses a landing craft's places 3 to 5): no enter button
        // either, the game's own pick, which put the bot in a place where it keeps its own weapon and which it then
        // never left (27 Nov 2026, run fx_boat2: pressed to get off for three minutes on the beach). It walks or swims.
        // BF_BOT_STANDTEST=1 (developer builds, tests of the way out of such a place): the button as before.
        bool stand_test = false;
#if defined(BF_DEV_TOOLS)
        static const bool stand_env = std::getenv("BF_BOT_STANDTEST") != nullptr;
        stand_test = stand_env;
#endif
        if (!stand_test) keep = false;
        else if (now >= bot.next_press) {
          bot.press_until = now + std::chrono::milliseconds(250);
          bot.next_press = now + std::chrono::milliseconds(1500);
          if (++bot.presses > 4) keep = false;
          else REXLOG_INFO("BF bots: {} presses enter at {} (BF_BOT_STANDTEST)", bot.name, VehicleName(base, bot.boat));
        }
      }
      if (!keep) {
        REXLOG_INFO("BF bots: {} gives up on {}", bot.name, VehicleName(base, bot.boat));
        bot.off_vehicle = bot.boat, bot.off_until = now + std::chrono::minutes(3);
        bot.boat = 0;
      } else {
        bot.want_yaw = YawTo(pos, at), bot.want_pitch = 0;
        bot.forward = Dist2D(at, pos) < 2.0f ? 0.0f : 1.0f, bot.strafe = 0, bot.fire = false, bot.ads = false;
        bot.sprint = Dist2D(at, pos) > 8.0f;
        CostTimer t(&cost.drive);
        Drive(ctx, base, bot, soldier, dt);
        continue;
      }
    }
#if defined(BF_DEV_TOOLS)
    // BF_BOT_WEAPONTEST=1 (tests): the first bot, on foot, stands and presses each weapon concept in turn (5 s apart, 8 s
    // after the match starts: Y, the pad's weapon swap (the user, 23 Nov 2026; concepts 9 AltFire and 19
    // CycleSecondaryInventory together in run bots_calib), then 19, 9, 18 CyclePrimaryInventory, 20 SelectMeleeWeapon,
    // 21 Reload, 30 to 33 SelectWeapon1 to 4, then Y twice more), firing a second after each; what changed in its
    // soldier and in the objects the soldier points to is logged after the press and after the shot (small numbers, and
    // pointers with the name of their data): which concept picks which weapon, where the weapon in hand and its ammo are.
    static const bool weapon_test = std::getenv("BF_BOT_WEAPONTEST") != nullptr;
    if (weapon_test && &bot == &g.bots.front() && soldier && Load32(base, bot.player + 312) == soldier) {
      static constexpr uint64_t kY = 1ull << 9 | 1ull << 19;
      static constexpr uint64_t kConcepts[] = {kY, 1ull << 19, 1ull << 9, 1ull << 18, 1ull << 20, 1ull << 21, 1ull << 30,
                                               1ull << 31, 1ull << 32, 1ull << 33, kY, kY};
      static Clock::time_point began{};
      static std::map<uint32_t, std::vector<uint32_t>> before;
      static int marks = 0;  // the step's snapshots logged: bit 0 the press's, bit 1 the shot's
      static int step_at = -1;
      if (began == Clock::time_point{}) began = now + std::chrono::seconds(8);
      auto name_of = [&](uint32_t ptr) {
        if (!Readable(ptr, 0x18)) return std::string();
        const uint32_t data = Load32(base, ptr + 0x14);
        std::string n = Readable(data, 0x10) ? DataText(base, Load32(base, data + 0xC)) : std::string();
        if (n.empty() && Readable(ptr, 0x10)) n = DataText(base, Load32(base, ptr + 0xC));
        return n;
      };
      auto snapshot = [&]() {
        std::map<uint32_t, std::vector<uint32_t>> m;
        auto take = [&](uint32_t obj, uint32_t words) {
          if (!Readable(obj, words * 4) || m.count(obj)) return;
          auto& v = m[obj];
          v.resize(words);
          for (uint32_t k = 0; k < words; ++k) v[k] = Load32(base, obj + 4 * k);
        };
        take(soldier, 0x400);
        for (uint32_t k = 0; k < 0x400; ++k) {
          const uint32_t ptr = Load32(base, soldier + 4 * k);
          if (ptr != soldier && ptr >= 0x40000000 && !InImage(ptr) && Readable(ptr, 0x200)) take(ptr, 0x80);
        }
        return m;
      };
      auto diff = [&](const char* when, uint64_t which) {
        const auto now_m = snapshot();
        std::string out;
        int n = 0;
        for (const auto& [obj, v] : now_m) {
          const auto it = before.find(obj);
          if (it == before.end()) continue;
          for (size_t k = 0; k < v.size() && k < it->second.size() && n < 60; ++k) {
            const uint32_t a = it->second[k], b = v[k];
            if (a == b) continue;
            const bool small = a < 64 && b < 64;
            const std::string na = small ? std::string() : name_of(a), nb = small ? std::string() : name_of(b);
            if (!small && na.empty() && nb.empty()) continue;
            out += fmt::format(" [{}{:08X}+{:#x}: {:X}{} -> {:X}{}]", obj == soldier ? "soldier " : "", obj, 4 * k, a,
                               na.empty() ? "" : " " + na, b, nb.empty() ? "" : " " + nb);
            ++n;
          }
        }
        std::string bits;
        for (int c = 0; c < 64; ++c)
          if (which >> c & 1) bits += fmt::format("{}{}", bits.empty() ? "" : "+", c);
        REXLOG_INFO("BF bots weapon test: concept {} {}:{}", bits, when, out.empty() ? " nothing" : out);
        before = now_m;
      };
      bot.forward = bot.strafe = 0, bot.sprint = bot.ads = false, bot.fire = false, bot.test_bits = 0;
      bot.want_pitch = 0, bot.want_yaw = bot.yaw;
      const double t = std::chrono::duration<double>(now - began).count();
      const int step = t >= 0 ? int(t / 5.0) : -1;
      if (step >= 0 && step < int(std::size(kConcepts))) {
        const double in = t - step * 5.0;
        if (step != step_at) {
          step_at = step, marks = 0;
          before = snapshot();
        }
        if (in >= 0.2 && in < 0.45) bot.test_bits = kConcepts[step];
        if (in >= 1.5 && !(marks & 1)) marks |= 1, diff("after the press", kConcepts[step]);
        if (in >= 2.0 && in < 2.6) bot.fire = true;
        if (in >= 4.0 && !(marks & 2)) marks |= 2, diff("after the shot", kConcepts[step]);
      }
      Drive(ctx, base, bot, soldier, dt);
      continue;
    }
#endif
    // Out of a plane (BailCheck): its fall speed, and A for the parachute once it falls (over 5 m/s, from 0.4 s after
    // it got out), again 1.5 s on while still over 12 m/s (an open parachute comes down at 8.7, run bail3), three
    // tries; its fall logged every half second (tests' detail).
    if (now < bot.chute_until) {
      if (bot.chute_y_at == Clock::time_point{}) {
        bot.chute_y = pos[1], bot.chute_y_at = now;
      } else if (const double el = std::chrono::duration<double>(now - bot.chute_y_at).count(); el >= 0.2) {
        bot.fall_speed = float((bot.chute_y - pos[1]) / el);
        bot.chute_y = pos[1], bot.chute_y_at = now;
      }
      if (now >= bot.chute_next && bot.fall_speed > (bot.chute_tries ? 12.0f : 5.0f) && bot.chute_tries < 3) {
        ++bot.chute_tries;
        bot.chute_press_until = now + std::chrono::milliseconds(250);
        bot.chute_next = now + std::chrono::milliseconds(1500);
        REXLOG_INFO("BF bots: {} opens its parachute (try {}), falling {:.0f} m/s at {:.0f} m", bot.name, bot.chute_tries,
                    bot.fall_speed, pos[1]);
      }
      if (REXCVAR_GET(bf_bots_log_detail) && now >= bot.chute_log) {
        bot.chute_log = now + std::chrono::milliseconds(500);
        REXLOG_INFO("BF bots: {} falls at {:.1f} m/s, {:.1f} m", bot.name, bot.fall_speed, pos[1]);
      }
      if (bot.chute_tries > 0 && bot.fall_speed < 1.0f && now >= bot.chute_next) bot.chute_until = {};  // down
    }
    if (now >= bot.next_think) {
      bot.next_think = now + std::chrono::milliseconds(250);
      CostTimer t(&cost.think);
      Think(ctx, base, bot, soldier, pos, listed, now);
    }
    {
      CostTimer t(&cost.drive);
      Drive(ctx, base, bot, soldier, dt);
    }
    if (now >= bot.next_log && REXCVAR_GET(bf_bots_log_detail)) {
      bot.next_log = now + std::chrono::seconds(3);
      REXLOG_INFO("BF bots: {} (team {}) at {:.1f} {:.1f} {:.1f}, heading {:.2f}{}{}", bot.name,
                  Load32(base, bot.player + 48), pos[0], pos[1], pos[2], bot.yaw, bot.enemy ? ", fighting" : "",
                  bot.fire ? ", firing" : "");
    }
  }
}

}  // namespace bf1943
