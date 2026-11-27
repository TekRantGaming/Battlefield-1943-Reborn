// Automatic team balance on a dedicated server (TekRantGaming, 15 Nov 2026: matches ended 7 US against 3 Japanese).
//
// The game's own balancer is its AutoTeamEntity (every level has one: from 6 players, a gap of more than 2): it only
// picks the team of a player who arrives (sub_8239A210, the smaller team) and never moves anyone already playing, so
// teams drift apart as players leave the losing side, friends join each other and players use Change Team to join the
// winners. Battlefield's own servers and the community's balancers (BF2's autobalance.py, BF1's 2018 balancer, the
// MULTIbalancer plugin) settled on the same rules, which this follows (side-session note team-balance-study.md):
//   - only a player who has died is moved, at the deploy screen: no kill, so no death, no ticket and no points lost;
//   - the gap must be bf_balance_gap (2) or more, with bf_balance_min_players (4) or more on the server, for
//     bf_balance_delay (20 s) before anyone moves; one move at a time, counted again after each;
//   - who: first a player who used Change Team to join the bigger side this round, then one in no squad, then the
//     newest arrival; never one moved in the last 15 minutes;
//   - a Change Team that would leave a gap of bf_balance_gap or more is refused (bf_balance_block_stacking);
//   - nothing between a round's end and the next level's load.
// Real players and bots (TekRantGaming, 27 Nov 2026, hotfix 0.4.8.1: "one team can have lots of real players, and the
// other team can have like 3 real players and the rest filled by bots"): the counts above are the REAL players only.
// Bots are never moved here; they keep the totals even themselves (bots.cpp's even teams rule: a bot on the side with
// more players leaves and one joins the other side). A real player moved to a side at its cap first has a bot there
// leave (bf1943::BotsMakeRoom), and the move goes through once it is leaving. Without bots it is as before.
// The move is the game's own (as its join with a friend does, sub_8239A418): the player leaves its team and squad, then
// sub_82399AE8 seats it on the other team (with a squad there, and the team's cap kept), on the server thread.
//
// Players are told by the server's host (server/gmhost.py, its comms): each move, refusal and warning is a line in
// bf_balance_events (JSON, one object a line), which the host turns into a chat line to everyone and a message box for
// the player moved (the server broadcast's box, which every game since v0.4.0 shows).
//
// Game data used (static study, bf/generated/default): the game context 0x82AF56F0 (sub_82349D78), its player manager
// at +40, the players from +84 to +88; a player's team at +48 (1 US, 2 Japan), squad at +52 (24: none), its soldier
// at +288 (its method at vtable +68; 0: dead); sub_82374F90 sets the team, sub_8237B320 the squad; sub_8239DE08
// (connection, team) is the server's handling of a player's own selectTeam (via the message handler sub_823EB888).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "bf1943_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "player_limits.h"

REXCVAR_DECLARE(bool, server);  // server_mode.cpp
REXCVAR_DEFINE_BOOL(bf_team_balance, false, "BF1943/Server",
                    "A dedicated server evens its teams: a player who has died on the bigger team moves to the other "
                    "(no points lost). The server manager turns it on (server.json team_balance)");
REXCVAR_DEFINE_INT32(bf_balance_gap, 2, "BF1943/Server", "Team balance: how many more players one team has before "
                                                         "anyone moves (2 to 6)");
REXCVAR_DEFINE_INT32(bf_balance_min_players, 4, "BF1943/Server", "Team balance: players needed on the server");
REXCVAR_DEFINE_DOUBLE(bf_balance_delay, 20.0, "BF1943/Server",
                      "Team balance: seconds the teams stay uneven before a player moves");
REXCVAR_DEFINE_BOOL(bf_balance_block_stacking, true, "BF1943/Server",
                    "Team balance: refuse a Change Team that would leave the teams uneven");
REXCVAR_DEFINE_STRING(bf_balance_events, "", "BF1943/Server",
                      "Team balance: the file its moves, refusals and warnings are written to, for the server's host "
                      "(gmhost.py --balance-file) to tell the players");

REX_EXTERN(sub_82374F90);  // the game's Player::setTeam(player, team)
REX_EXTERN(sub_8237B320);  // the game's squad change (player, squad; 24: none)
REX_EXTERN(sub_82399AE8);  // the game's team pick (player, team, find a squad, keep the team's cap): true when seated

namespace bf1943 {
bool IsBotPlayer(uint32_t player);             // bots.cpp
bool BotsMakeRoom(uint8_t* base, int team);    // bots.cpp: a bot on `team` leaves; true when one is leaving
}  // namespace bf1943

namespace {
using Clock = std::chrono::steady_clock;

constexpr uint32_t kGameContext = 0x82AF56F0;  // sub_82349D78's object (made at the game's start)
constexpr uint32_t kNoSquad = 24;
constexpr double kMovedRest = 15 * 60;  // seconds a moved player is left alone
constexpr double kChoseFor = 30 * 60;   // a Change Team to the bigger side counts this long (the round)

uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
bool Plausible(uint32_t a) { return a >= 0x00100000u && a < 0xFFF00000u && !(a & 3); }
bool InImage(uint32_t a) { return a >= 0x82000000u && a < 0x82DB0000u; }

// Guest memory that is there to read (committed and readable) from `address` for `size` bytes. The first test run
// (tb_dump1) read the player list before the server's level had made it, and the server crashed on a read of guest
// 3F800000 (a float's 1.0 taken for a pointer).
bool Readable(uint32_t address, uint32_t size) {
  if (!Plausible(address) || address + size < address) return false;
  auto* state = rex::system::kernel_state();
  rex::memory::Memory* memory = state ? state->memory() : nullptr;
  if (!memory) return false;
  // Committed pages only: a reserved page answers QueryProtect with its protection too, and a read of it crashed the
  // second run (tb_dump2: guest 01000000).
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

// What we keep of a player, by its object's address (a new player in a reused object: told apart by its arrival).
struct Seen {
  Clock::time_point arrived{}, moved{}, chose_bigger{};
  bool was_moved = false, chose = false;
  int team = 0;
  bool present = false;
};

struct State {
  std::mutex mutex;  // the tick and the selectTeam hook both run on the server thread; the mutex is for the logs only
  std::map<uint32_t, Seen> seen;
  Clock::time_point uneven_since{}, last_check{}, last_move{};
  bool warned = false, round_over = false;
  int seq = 0;
  bool file_started = false;
} g;

struct Player {
  uint32_t addr = 0;
  int team = 0;
  uint32_t squad = kNoSquad;
  bool bot = false;  // one of our bots (bots.cpp): not counted, never moved
};

// The server's players (empty before the game context is made, or when its list looks wrong).
std::vector<Player> Players(const uint8_t* base) {
  std::vector<Player> out;
  if (!(Load32(base, 0x82AF574C) & 1)) return out;  // the context not made yet (sub_82349D78's guard)
  const uint32_t manager = Load32(base, kGameContext + 40);
  if (!Readable(manager, 96) || !InImage(Load32(base, manager))) return out;
  const uint32_t begin = Load32(base, manager + 84), end = Load32(base, manager + 88);
  if (begin == end || end < begin || end - begin > uint32_t(bf1943::kPlayerListLimit) * 4 || !Readable(begin, end - begin))
    return out;
  for (uint32_t at = begin; at < end; at += 4) {
    const uint32_t p = Load32(base, at);
    if (!Readable(p, 0x300) || !InImage(Load32(base, p))) continue;
    Player pl;
    pl.addr = p, pl.team = int(Load32(base, p + 48)), pl.squad = Load32(base, p + 52);
    pl.bot = bf1943::IsBotPlayer(p);
    out.push_back(pl);
  }
  return out;
}

// The player's soldier (0 while dead): its method at vtable +68, as the game's suicide reads it (sub_8239DDA0), which
// for a server player (vtable 0x8201A990) is sub_821CA758: the word at +288. Any other kind of player is taken as
// alive, so it is never moved.
constexpr uint32_t kServerPlayerVtable = 0x8201A990;
uint32_t SoldierOf(PPCContext& ctx, uint8_t* base, uint32_t player) {
  (void)ctx;
  if (!Readable(player, 292) || Load32(base, player) != kServerPlayerVtable) return 1;
  return Load32(base, player + 288);
}

// The player's name: an eastl string at +0x18 (begin, end, capacity; run tb_dump3's dump, a server player of vtables
// 0x8201A990 and 0x8201A978: "Soldier1671" from +0x18 to +0x1C), the name its game signed in with.
std::string NameOf(const uint8_t* base, uint32_t player);

const char* TeamName(int team) { return team == 1 ? "US" : team == 2 ? "Japan" : "no team"; }

std::string JsonText(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') out += '\\', out += char(c);
    else if (c < 0x20) out += ' ';
    else out += char(c);
  }
  return out + "\"";
}

// One event for the server's host (gmhost.py --balance-file): {"seq", "kind", "name", "team", "to", "us", "japan"}.
void Event(const std::string& kind, const std::string& name, int team, int to, int n1, int n2) {
  const std::string path = REXCVAR_GET(bf_balance_events);
  if (path.empty()) return;
  std::ofstream f(path, g.file_started ? std::ios::app : std::ios::trunc);
  g.file_started = true;
  if (!f) return;
  f << "{\"seq\": " << ++g.seq << ", \"kind\": " << JsonText(kind) << ", \"name\": " << JsonText(name)
    << ", \"team\": " << team << ", \"to\": " << to << ", \"us\": " << n1 << ", \"japan\": " << n2 << "}\n";
}

// The game's own move of a player to `to` (sub_8239A418's order: no team, no squad, then the pick with a squad and the
// team's cap). Back as it was when the game refuses (the team at its cap).
bool Move(PPCContext& outer, uint8_t* base, uint32_t player, int to) {
  const int from = int(Load32(base, player + 48));
  const uint32_t squad = Load32(base, player + 52);
  PPCContext call = outer;
  call.r3.u64 = player, call.r4.u64 = 0;
  sub_82374F90(call, base);
  call = outer;
  call.r3.u64 = player, call.r4.u64 = kNoSquad;
  sub_8237B320(call, base);
  call = outer;
  call.r3.u64 = player, call.r4.u64 = uint32_t(to), call.r5.u64 = 1, call.r6.u64 = 1;
  sub_82399AE8(call, base);
  if ((call.r3.u32 & 0xFF) && int(Load32(base, player + 48)) == to) return true;
  call = outer;
  call.r3.u64 = player, call.r4.u64 = uint32_t(from);
  sub_82374F90(call, base);
  call = outer;
  call.r3.u64 = player, call.r4.u64 = squad;
  sub_8237B320(call, base);
  return false;
}

#if defined(BF_DEV_TOOLS)
// BF_DEV_BALANCE_DUMP=1 (tests): each player's object once, as words and its printable runs, to find its name.
void DevDump(PPCContext& ctx, uint8_t* base, const std::vector<Player>& players) {
  static const bool on = [] {
    const char* v = std::getenv("BF_DEV_BALANCE_DUMP");
    return v && *v == '1';
  }();
  static std::map<uint32_t, int> done;
  if (!on) return;
  for (const Player& p : players) {
    if (done[p.addr]++ || !Readable(p.addr, 0x840)) continue;
    std::string words, runs;
    for (uint32_t off = 0; off < 0x400; off += 4) {
      if (off % 32 == 0) words += fmt::format("\n  +{:03X}:", off);
      words += fmt::format(" {:08X}", Load32(base, p.addr + off));
    }
    for (uint32_t off = 0; off < 0x800; ++off) {
      uint32_t n = 0;
      while (n < 40 && base[p.addr + off + n] >= 0x20 && base[p.addr + off + n] < 0x7F) ++n;
      if (n >= 4) runs += fmt::format(" +{:03X}'{}'", off, std::string(reinterpret_cast<const char*>(base + p.addr + off), n)), off += n;
      // a word pointing at text (an eastl string's begin)
    }
    std::string ptrs;
    for (uint32_t off = 0; off < 0x400; off += 4) {
      const uint32_t q = Load32(base, p.addr + off);
      if (!Readable(q, 40)) continue;
      uint32_t n = 0;
      while (n < 32 && base[q + n] >= 0x20 && base[q + n] < 0x7F) ++n;
      if (n >= 4 && base[q + n] == 0)
        ptrs += fmt::format(" +{:03X}->'{}'", off, std::string(reinterpret_cast<const char*>(base + q), n));
    }
    REXLOG_INFO("BF team balance dump: player {:08X} team {} squad {} soldier {:08X}; text{}; text pointers{}; words{}",
                p.addr, p.team, p.squad, SoldierOf(ctx, base, p.addr), runs, ptrs, words);
  }
}
#endif
}  // namespace

namespace bf1943 {

// A round's end (server_rotation.cpp RoundOver): nothing moves until the next level has loaded.
void TeamBalanceRoundOver() {
  std::lock_guard lock(g.mutex);
  g.round_over = true;
}
// A new level (server_rotation.cpp): balance again; the round's Change Team choices are forgotten.
void TeamBalanceLevelChange() {
  std::lock_guard lock(g.mutex);
  g.round_over = false, g.uneven_since = {}, g.warned = false;
  for (auto& [addr, s] : g.seen) s.chose = false;
}

// Once a second on the server's tick (server_rotation.cpp, after the game's own tick).
void TeamBalanceTick(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(server)) return;
  const auto now = Clock::now();
  if (now - g.last_check < std::chrono::seconds(1)) return;
  g.last_check = now;
  const std::vector<Player> players = Players(base);
#if defined(BF_DEV_TOOLS)
  DevDump(ctx, base, players);
#endif
  std::lock_guard lock(g.mutex);
  for (auto& [addr, s] : g.seen) s.present = false;
  int n1 = 0, n2 = 0;  // the real players on each side (bots: bots.cpp keeps the totals even)
  for (const Player& p : players) {
    Seen& s = g.seen[p.addr];
    if (s.arrived == Clock::time_point{}) s.arrived = now, s.team = p.team;
    s.present = true;
    if (p.bot) continue;
    n1 += p.team == 1, n2 += p.team == 2;
  }
  for (auto it = g.seen.begin(); it != g.seen.end();) it = it->second.present ? std::next(it) : g.seen.erase(it);
#if defined(BF_DEV_TOOLS)
  // BF_DEV_HUMANS_TEAM=1|2 (tests, dev builds; hotfix 0.4.8.1): each real player is put on that side once, while it has
  // no soldier yet, so the real players are stacked and the balance below must even them again.
  static const int stack_on = [] {
    const char* v = std::getenv("BF_DEV_HUMANS_TEAM");
    return v && (*v == '1' || *v == '2') ? *v - '0' : 0;
  }();
  if (stack_on) {
    static std::map<uint32_t, bool> stacked;
    for (const Player& p : players) {
      if (p.bot || stacked[p.addr] || (p.team != 1 && p.team != 2) || SoldierOf(ctx, base, p.addr) != 0) continue;
      stacked[p.addr] = true;
      if (p.team == stack_on) continue;
      const bool ok = Move(ctx, base, p.addr, stack_on);
      REXLOG_INFO("BF team balance: BF_DEV_HUMANS_TEAM: {} put on {} for the test: {}", NameOf(base, p.addr),
                  TeamName(stack_on), ok ? "moved" : "the game refused");
      if (ok) g.seen[p.addr].arrived = now;
      return;  // counted again next second
    }
  }
#endif
  if (!REXCVAR_GET(bf_team_balance) || g.round_over) return;
  const int gap = std::clamp(REXCVAR_GET(bf_balance_gap), 2, 6);
  const int need = std::max(2, REXCVAR_GET(bf_balance_min_players));
  if (n1 + n2 < need || std::abs(n1 - n2) < gap) {
    if (g.uneven_since != Clock::time_point{})
      REXLOG_INFO("BF team balance: teams even again ({} US, {} Japan)", n1, n2);
    g.uneven_since = {}, g.warned = false;
    return;
  }
  const int bigger = n1 > n2 ? 1 : 2, smaller = 3 - bigger;
  if (g.uneven_since == Clock::time_point{}) {
    g.uneven_since = now;
    REXLOG_INFO("BF team balance: teams uneven ({} US, {} Japan); a player who dies on {} moves in {:.0f} s", n1, n2,
                TeamName(bigger), REXCVAR_GET(bf_balance_delay));
  }
  // The bigger team is warned half way through the delay (a switch that is undone at once never warns anyone; run
  // tb_move3 warned in the same second as the move), and players move once the delay is over.
  const double uneven_for = std::chrono::duration<double>(now - g.uneven_since).count();
  const double delay = std::max(0.0, REXCVAR_GET(bf_balance_delay));
  if (!g.warned && uneven_for >= delay / 2) {
    g.warned = true;
    Event("warn", "", bigger, smaller, n1, n2);
  }
  if (uneven_for < delay) return;
  // One move every 2 s at most (the counts settle; the moved player's game gets its new team).
  if (now - g.last_move < std::chrono::seconds(2)) return;
  struct Pick {
    uint32_t addr;
    int rank;
    Clock::time_point arrived;
  };
  std::vector<Pick> picks;
  for (const Player& p : players) {
    if (p.team != bigger || p.bot) continue;
    const Seen& s = g.seen[p.addr];
    if (s.was_moved && std::chrono::duration<double>(now - s.moved).count() < kMovedRest) continue;
    if (SoldierOf(ctx, base, p.addr) != 0) continue;  // alive: never killed to balance
    const bool chose = s.chose && std::chrono::duration<double>(now - s.chose_bigger).count() < kChoseFor;
    picks.push_back({p.addr, chose ? 0 : p.squad == kNoSquad ? 1 : 2, s.arrived});
  }
  if (picks.empty()) return;  // nobody on the bigger team is down: wait for a death
  std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) {
    return a.rank != b.rank ? a.rank < b.rank : a.arrived > b.arrived;  // then the newest arrival
  });
  const uint32_t who = picks.front().addr;
  const std::string name = NameOf(base, who);
  if (!Move(ctx, base, who, smaller)) {
    // The side at its cap: a bot there leaves (bots.cpp) and the next try, 2 s on, passes over the cap.
    const bool room = bf1943::BotsMakeRoom(base, smaller);
    REXLOG_INFO("BF team balance: the game refused to move {} to {} ({} US, {} Japan real players){}", name,
                TeamName(smaller), n1, n2, room ? ": a bot there makes room, trying again" : "");
    g.last_move = now;
    return;
  }
  Seen& s = g.seen[who];
  s.was_moved = true, s.moved = now, s.chose = false, s.team = smaller;
  g.last_move = now;
  const int m1 = n1 + (smaller == 1) - (bigger == 1), m2 = n2 + (smaller == 2) - (bigger == 2);
  REXLOG_INFO("BF team balance: {} moved from {} to {} while down ({} US, {} Japan real players now; {} s uneven)", name,
              TeamName(bigger), TeamName(smaller), m1, m2, int(uneven_for));
  Event("moved", name, bigger, smaller, m1, m2);
}

// A player's own Change Team on the server (bf_balance_block_stacking): refused when it would leave the teams a gap of
// bf_balance_gap or more apart (with bf_balance_min_players or more on the server). Returns false to refuse.
bool TeamBalanceAllowSwitch(uint8_t* base, uint32_t player, int to) {
  if (!REXCVAR_GET(server) || !Readable(player, 0x300)) return true;
  const int from = int(Load32(base, player + 48));
  if (to == from || (to != 1 && to != 2) || (from != 1 && from != 2)) return true;  // a joiner's first team: the game's
  const std::vector<Player> players = Players(base);
  int n[3] = {0, 0, 0};  // the real players (bots even the totals themselves)
  for (const Player& p : players)
    if (!p.bot && (p.team == 1 || p.team == 2)) ++n[p.team];
  const int gap = std::clamp(REXCVAR_GET(bf_balance_gap), 2, 6);
  const bool on = REXCVAR_GET(bf_team_balance) && REXCVAR_GET(bf_balance_block_stacking);
  const bool enough = n[1] + n[2] >= std::max(2, REXCVAR_GET(bf_balance_min_players));
  const int after_to = n[to] + 1, after_from = n[from] - 1;
  const std::string name = NameOf(base, player);
  std::lock_guard lock(g.mutex);
  if (on && enough && after_to - after_from >= gap) {
    REXLOG_INFO("BF team balance: {} asked to change to {}: refused ({} US, {} Japan)", name, TeamName(to), n[1], n[2]);
    Event("refused", name, from, to, n[1], n[2]);
    return false;
  }
  Seen& s = g.seen[player];
  if (after_to > after_from) s.chose = true, s.chose_bigger = Clock::now();
  REXLOG_INFO("BF team balance: {} changes to {} ({} US, {} Japan before)", name, TeamName(to), n[1], n[2]);
  return true;
}

}  // namespace bf1943

namespace {
std::string NameOf(const uint8_t* base, uint32_t player) {
  const uint32_t begin = Load32(base, player + 0x18), end = Load32(base, player + 0x1C);
  if (end > begin && end - begin <= 64 && Readable(begin, end - begin)) {
    std::string name(reinterpret_cast<const char*>(base + begin), end - begin);
    if (std::all_of(name.begin(), name.end(), [](char c) { return c != 0; })) return name;
  }
  return fmt::format("player {:08X}", player);
}
}  // namespace

// The server's handling of a player's own selectTeam: (connection, team), the connection's player at +1712.
REX_EXTERN(__imp__sub_8239DE08);
REX_HOOK_RAW(sub_8239DE08) {
  if (REXCVAR_GET(server)) {
    const uint32_t player = Readable(ctx.r3.u32 + 1712, 4) ? REX_LOAD_U32(ctx.r3.u32 + 1712) : 0;
    if (player && !bf1943::TeamBalanceAllowSwitch(base, player, int(ctx.r4.s32))) return;
  }
  __imp__sub_8239DE08(ctx, base);
}
