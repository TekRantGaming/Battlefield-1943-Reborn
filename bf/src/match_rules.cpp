// A match's rules (the user, 19 Nov 2026: "add match time and team ticket amount settings, including disabling match
// time, which then the match only ends if one team loses all their tickets"): each team's tickets and a time limit,
// on a dedicated server (Create Match passes them, private_match.cpp).
//
// What the game has (static study, D209): each level holds two TicketCounterEntityData (one a team; TeamId at +156,
// InitialTicketCount at +136: 130 on Wake Island, Guadalcanal and Iwo Jima, 100 on Coral Sea). The server's
// ServerTicketCounterEntity (vtable 0x820150A0, 160 bytes, made by sub_8232C780, destroyed by sub_82321E88; its data
// at +20) keeps the team's tickets as a float at +132 (0x84, as BF_DEV_TICKETS found), and +140 is set once the team
// has run out. Its reset at a round's start (vtable +0x78, sub_8232C890) reads the data's InitialTicketCount again.
// Every loss goes through sub_82335388(counter, tickets): it marks the counter for the network (16 bits a team, so
// up to 65535), keeps it at 0 or more, and sub_82332748 then sends the game's own "tickets gone" event at 0, which
// ends the round (the end of round screen, the winner, the rotation's next map: server_rotation.cpp).
// The game has no time limit of its own (no level has one, and nothing in the image ends a round on time), so a
// match without one ends only on tickets, as before.
//
// So: the tickets are written into the server's copy of each counter's InitialTicketCount as the counter is made
// (its own start and every reset read it, and so do the ticket percentages its bleed and its "tickets low" voice
// use), and once the time limit has passed the team with fewer tickets loses the rest through sub_82335388(counter,
// 0): the game's own end. With both teams level the next ticket lost decides. The clock starts with the round's
// counters (a level's load, or its restart) and stops at the round's end.
//
// A player's game: the HUD's ticket bars are each team's tickets over its own copy of the level's InitialTicketCount
// (sub_824083E8, through the getter sub_8222B120), so with the host's count from the match's world (world_sync.cpp)
// the getter answers that count there.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "bf1943_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "world_sync.h"

REXCVAR_DECLARE(bool, server);  // server_mode.cpp
REXCVAR_DEFINE_INT32(bf_match_tickets, 0, "BF1943/Server",
                     "Each team's tickets at a round's start; 0: the level's own (130, Coral Sea 100)");
REXCVAR_DEFINE_DOUBLE(bf_match_time_limit, 0.0, "BF1943/Server",
                      "Minutes a round lasts before the team with fewer tickets loses; 0: no limit (a round ends only "
                      "when a team has no tickets left)");

REX_EXTERN(sub_82335388);  // the game's tickets of a counter (counter, f1 tickets): the network, the round's end at 0

namespace {
using Clock = std::chrono::steady_clock;

constexpr uint32_t kCounterVtable = 0x820150A0;  // ServerTicketCounterEntity
constexpr uint32_t kHudInitialTicketsReturn = 0x8240848C;  // sub_824083E8's call of sub_8222B120 (the HUD's bars)

uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = base + a;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
float LoadF(const uint8_t* base, uint32_t a) {
  const uint32_t bits = Load32(base, a);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
void StoreF(uint8_t* base, uint32_t a, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  Store32(base, a, bits);
}
bool Plausible(uint32_t a) { return a >= 0x00100000u && a < 0xFFF00000u && !(a & 3); }

const char* TeamName(int team) { return team == 1 ? "the US" : team == 2 ? "Japan" : "a team"; }

struct State {
  std::mutex mutex;
  std::vector<uint32_t> counters;  // the server's ticket counters now
  Clock::time_point round_start{};
  bool running = false;  // a round runs (its counters made or reset, its end not yet)
  bool ended = false;    // the time limit ended it
  bool tie_said = false;
} g;

std::atomic<uint32_t> g_round{0};         // counts the rounds started (the match's world restarts its storm)
std::atomic<float> g_seconds_left{-1.0f};  // the round's time left; -1: no limit or no round

int Tickets() { return std::clamp(REXCVAR_GET(bf_match_tickets), 0, 9999); }
double LimitSeconds() { return std::clamp(REXCVAR_GET(bf_match_time_limit), 0.0, 600.0) * 60.0; }

// A counter just made or reset: the match's tickets into its data (and its count), and the round's clock from now.
void CounterStarts(uint8_t* base, uint32_t counter, bool reset) {
  const uint32_t data = Load32(base, counter + 20);
  const int team = Plausible(data) ? int(Load32(base, data + 156)) : 0;
  const int own = Plausible(data) ? int(Load32(base, data + 136)) : 0;
  const int tickets = Tickets();
  if (tickets > 0 && Plausible(data)) {
    Store32(base, data + 136, uint32_t(tickets));
    StoreF(base, counter + 132, float(tickets));
  }
  {
    std::lock_guard lock(g.mutex);
    if (!reset && std::find(g.counters.begin(), g.counters.end(), counter) == g.counters.end())
      g.counters.push_back(counter);
    const auto now = Clock::now();
    // The two counters of a round start within the same moment: one new round.
    if (!g.running || now - g.round_start > std::chrono::seconds(5)) {
      g_round.fetch_add(1);
      g.tie_said = false;
    }
    g.round_start = now, g.running = true, g.ended = false;
  }
  REXLOG_INFO("BF match rules: {}'s tickets {} ({} the level's {}){}", TeamName(team), tickets > 0 ? tickets : own,
              tickets > 0 ? "instead of" : "", own, reset ? ", the round's reset" : "");
}
}  // namespace

namespace bf1943 {

// For our GameManager host (gm_host.cpp): the rules and the round's clock, to tell every player's game.
struct MatchClock {
  int tickets = 0;          // 0: the level's own
  int time_limit = 0;       // minutes; 0: none
  float seconds_left = -1;  // -1: no limit, or no round running
  uint32_t round = 0;       // counts the rounds started
};
MatchClock CurrentMatchClock() {
  MatchClock c;
  c.tickets = Tickets();
  c.time_limit = int(std::lround(LimitSeconds() / 60.0));
  c.seconds_left = g_seconds_left.load();
  c.round = g_round.load();
  return c;
}

// A round's end (server_rotation.cpp RoundOver): the clock stops.
void MatchRulesRoundOver() {
  std::lock_guard lock(g.mutex);
  if (g.running) REXLOG_INFO("BF match rules: the round is over");
  g.running = false;
  g_seconds_left = -1.0f;
}

// The server's tick (server_rotation.cpp): the time limit.
void MatchRulesTick(PPCContext& ctx, uint8_t* base) {
  const double limit = LimitSeconds();
  std::vector<uint32_t> counters;
  {
    std::lock_guard lock(g.mutex);
    if (!g.running || limit <= 0) {
      g_seconds_left = -1.0f;
      return;
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - g.round_start).count();
    g_seconds_left = float(std::max(0.0, limit - elapsed));
    if (elapsed < limit || g.ended) return;
    counters = g.counters;
  }
  // Time is up: the team with fewer tickets loses the rest.
  uint32_t loser = 0;
  float fewest = 0, most = 0;
  int found = 0;
  for (uint32_t c : counters) {
    if (Load32(base, c) != kCounterVtable || Load32(base, c + 140) != 0) continue;
    const float t = LoadF(base, c + 132);
    if (!found || t < fewest) fewest = t, loser = c;
    if (!found || t > most) most = t;
    ++found;
  }
  if (found < 2) return;  // a team already out: the game ends the round itself
  if (fewest == most) {
    std::lock_guard lock(g.mutex);
    if (!g.tie_said) REXLOG_INFO("BF match rules: time is up with both teams at {:.0f} tickets: the next ticket lost decides", most);
    g.tie_said = true;
    return;
  }
  const uint32_t data = Load32(base, loser + 20);
  REXLOG_INFO("BF match rules: time is up ({:.0f} min): {} has {:.0f} tickets to {:.0f} and loses the round", limit / 60,
              TeamName(Plausible(data) ? int(Load32(base, data + 156)) : 0), fewest, most);
  {
    std::lock_guard lock(g.mutex);
    g.ended = true;
  }
  PPCContext call = ctx;
  call.r3.u64 = loser;
  call.f1.f64 = 0.0;
  sub_82335388(call, base);
}

}  // namespace bf1943

// The server's ticket counter is made (sub_8232C780(counter, ?, data)): the match's tickets.
REX_EXTERN(__imp__sub_8232C780);
REX_HOOK_RAW(sub_8232C780) {
  const uint32_t counter = ctx.r3.u32;
  __imp__sub_8232C780(ctx, base);
  if (REXCVAR_GET(server) && Load32(base, counter) == kCounterVtable) CounterStarts(base, counter, false);
}

// Its reset at a round's start (vtable +0x78): the match's tickets again (its data keeps them), the clock from now.
REX_EXTERN(__imp__sub_8232C890);
REX_HOOK_RAW(sub_8232C890) {
  const uint32_t counter = ctx.r3.u32;
  __imp__sub_8232C890(ctx, base);
  if (REXCVAR_GET(server) && Load32(base, counter) == kCounterVtable) CounterStarts(base, counter, true);
}

// Its destructor: no longer one of the round's.
REX_EXTERN(__imp__sub_82321E88);
REX_HOOK_RAW(sub_82321E88) {
  if (REXCVAR_GET(server)) {
    std::lock_guard lock(g.mutex);
    g.counters.erase(std::remove(g.counters.begin(), g.counters.end(), ctx.r3.u32), g.counters.end());
    if (g.counters.empty()) g.running = false, g_seconds_left = -1.0f;
  }
  __imp__sub_82321E88(ctx, base);
}

// A player's game: the HUD's ticket bars over the match's tickets (the host's, from the match's world) rather than the
// level's own count. Only for that one call: the getter is shared with other data types (sub_82468EF8's).
REX_EXTERN(__imp__sub_8222B120);
REX_HOOK_RAW(sub_8222B120) {
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_8222B120(ctx, base);
  if (lr != kHudInitialTicketsReturn || REXCVAR_GET(server)) return;
  const bf1943::world::Status s = bf1943::world::Current();
  if (!s.answered || s.tickets <= 0) return;
  static std::atomic<int> said{-1};
  if (said.exchange(s.tickets) != s.tickets)
    REXLOG_INFO("BF match rules: the HUD's ticket bars over the match's {} tickets (the level's {})", s.tickets, ctx.r3.u32);
  ctx.r3.u64 = uint32_t(s.tickets);
}
