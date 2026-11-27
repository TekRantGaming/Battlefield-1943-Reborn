// Each player's stats on our dedicated server (server_stats.h).
//
// The counts are kept by player name, added up between two of the server's status beats (gm_host.cpp, every 5 s) and
// sent then as one message; the backend (server/player_stats.py) adds them to each player's all time, month and week
// totals and keeps them only from servers it trusts (official ones).
//
// Where they come from: the game's own bookkeeping on the server, read from the generated code on 16 Nov 2026
// (docs/STATS.md). Every hook below acts on the dedicated server only (players' games run the same code).
//   sub_823F8960  every score award: (r3 scoring, r4 player, f1 points, r6 label string, r7 category). The score, and
//                 melee kills, assists, flag captures, defends, repairs and heals by their label.
//   sub_82409708  the kill handler: (r3 scoring, r4 killer or 0, r5 victim, r7 weapon, r9 & 0xFF headshot). Kills,
//                 deaths, team kills, suicides, headshots, and the kills made from a vehicle.
//   sub_823F7518  a player's round result: (r3 scoring, r4 player, r5 & 0xFF lost). Rounds, wins and losses.
//   sub_824144A0  the server's event dispatcher (r3, r4 event: +136 type, +144 player, +156 a name): spawn (17, with
//                 the kit), into (15) and out of (16) a vehicle, a player gone (1). The time in each kit and vehicle.
// A player the game marks at +532 (it gets no score record) is left out. The best kill streak is counted here.

#include "server_stats.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "bf1943_pch.h"
#include "guest_readable.h"

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(bool, server);
REXCVAR_DEFINE_STRING(bf_stats_file, "", "BF1943/Server",
                      "Each player's stats: the file their counts go to every 5 s, one JSON line each, for the server's "
                      "host to send to the backend (gmhost.py --stats-file; the server manager sets both)");

namespace bf1943 {
std::string CurrentMapTitle();  // server_rotation.cpp
bool BotIsPlayer(uint32_t player);  // bots.cpp: one of the server's bots
void BotsNoteKill(PPCContext& ctx, uint8_t* base, uint32_t killer, uint32_t victim);  // bots.cpp: what bots learn
}  // namespace bf1943

namespace {
using Clock = std::chrono::steady_clock;

struct Counts {
  std::map<std::string, long long> n;  // player_stats.py COUNTS
  int streak = 0;                      // kills since the last death (this life)
  int best = 0;                        // the best streak since the last message
  double score = 0;                    // points not sent yet (fractions, and a team kill's minus)
};
// A player's life: the kit since the spawn, the vehicle since getting in.
struct Life {
  const char* kit = nullptr;
  const char* vehicle = nullptr;
  Clock::time_point alive_since{}, kit_since{}, vehicle_since{};
  bool alive = false;
};
std::mutex g_mutex;
std::map<std::string, Counts> g_players;  // by name as the game has it
std::map<std::string, Life> g_lives;

void AddLocked(const std::string& name, const std::string& what, long long n) {
  if (name.empty() || n <= 0) return;
  if (g_players.size() >= 128 && !g_players.count(name)) return;
  Counts& c = g_players[name];
  c.n[what] += n;
  if (what == "kills") {
    c.streak += int(n);
    c.best = std::max(c.best, c.streak);
  } else if (what == "deaths") {
    c.streak = 0;
  }
}

// Whole seconds from `since` to `now` into `what`; `since` moves on by them.
void TimeInto(const std::string& name, Clock::time_point& since, const std::string& what, Clock::time_point now) {
  const long long secs = std::chrono::duration_cast<std::chrono::seconds>(now - since).count();
  if (secs <= 0) return;
  since += std::chrono::seconds(secs);
  AddLocked(name, what, secs);
}
void CloseVehicle(const std::string& name, Life& l, Clock::time_point now) {
  if (l.vehicle) TimeInto(name, l.vehicle_since, std::string("veh_") + l.vehicle, now);
  l.vehicle = nullptr;
}
void TimeSoFar(const std::string& name, Life& l, Clock::time_point now) {
  if (!l.alive) return;
  if (l.vehicle) TimeInto(name, l.vehicle_since, std::string("veh_") + l.vehicle, now);
  if (l.kit) TimeInto(name, l.kit_since, std::string("kit_") + l.kit, now);
  TimeInto(name, l.alive_since, "seconds", now);
}
void CloseLife(const std::string& name, Life& l, Clock::time_point now) {
  TimeSoFar(name, l, now);
  l.vehicle = nullptr, l.kit = nullptr, l.alive = false;
}

std::string MapKey(const std::string& title) {
  std::string t;
  for (char c : title) t += char(std::tolower(static_cast<unsigned char>(c)));
  if (t.find("wake") != std::string::npos) return "wake";
  if (t.find("iwo") != std::string::npos) return "iwo";
  if (t.find("guadal") != std::string::npos) return "guadalcanal";
  if (t.find("coral") != std::string::npos) return "coral";
  return "";
}

std::string Quote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (static_cast<unsigned char>(c) >= 32) out += c;
  }
  return out + "\"";
}

// ---- guest memory

uint32_t L32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
bool Plausible(uint32_t a) { return a >= 0x00100000u && a < 0xFFF00000u && !(a & 3); }
// Committed, readable guest memory (as team_balance.cpp).
bool Readable(uint32_t address, uint32_t size) {
  if (address < 0x00100000u || address + size < address) return false;
  auto* state = rex::system::kernel_state();
  rex::memory::Memory* memory = state ? state->memory() : nullptr;
  if (!memory) return false;
  return bf1943::GuestCommittedReadable(memory, address, size);  // guest_readable.h (was a QueryRegionInfo walk)
}
// An eastl string object (begin, end) as text.
std::string Eastl(const uint8_t* base, uint32_t obj, uint32_t max = 96) {
  if (!Plausible(obj) || !Readable(obj, 8)) return {};
  const uint32_t b = L32(base, obj), e = L32(base, obj + 4);
  if (!b || e <= b || e - b > max || !Readable(b, e - b)) return {};
  std::string s(reinterpret_cast<const char*>(base + b), e - b);
  if (const size_t z = s.find('\0'); z != std::string::npos) s.resize(z);
  return s;
}
// A real player (not one the game marks at +532, not one of our bots: bots.cpp) and its name.
std::string PlayerName(const uint8_t* base, uint32_t player) {
  if (!Plausible(player) || !Readable(player, 0x220) || base[player + 532] || bf1943::BotIsPlayer(player)) return {};
  return Eastl(base, player + 0x18, 64);
}
// "CQ_StringForFlagCapture" -> "flagcapture", "melee kill" -> "meleekill"
std::string Squash(const std::string& s) {
  std::string t;
  for (char c : s)
    if (std::isalnum(static_cast<unsigned char>(c))) t += char(std::tolower(static_cast<unsigned char>(c)));
  for (const char* p : {"cqstringfor", "stringfor"})
    if (t.rfind(p, 0) == 0) t = t.substr(std::strlen(p));
  return t;
}
bool Has(const std::string& t, std::initializer_list<const char*> keys) {
  for (const char* k : keys)
    if (t.find(k) != std::string::npos) return true;
  return false;
}
// The kits by the engine's names (Bad Company's: the game's strings have UIHudIcon_Kit Assault, Support, Recon,
// Demolition, Specialist). Seen in TekRantGaming's kill test of 16 Nov 2026: the deploy screen's first kit, Infantry,
// spawns as "Support"; Rifleman (the rifle with its grenade launcher) as "Assault"; Scout as Recon.
const char* KitKey(const std::string& name) {
  const std::string t = Squash(name);
  if (Has(t, {"scout", "sniper", "recon"})) return "scout";
  if (Has(t, {"rifle", "assault"})) return "rifleman";
  if (Has(t, {"infantry", "support"})) return "infantry";
  return nullptr;
}
const char* VehicleKey(const std::string& name) {
  const std::string t = Squash(name);
  if (Has(t, {"corsair", "zero", "plane", "f4u", "a6m", "fighter", "bomber", "aircraft", "dauntless"})) return "plane";
  if (Has(t, {"tank", "sherman", "chiha", "type97"})) return "tank";
  if (Has(t, {"jeep", "willys", "kurogane", "truck"})) return "jeep";
  if (Has(t, {"boat", "lcvp", "landing", "higgins", "daihatsu"})) return "boat";
  return nullptr;
}
void NoteName(const char* what, const std::string& name, const char* key) {  // each kit or vehicle name, once
  static std::set<std::string> seen;  // under g_mutex
  if (seen.size() < 200 && seen.insert(std::string(what) + "|" + name).second)
    REXLOG_INFO("BF server stats: {} \"{}\" counts as {}", what, name, key ? key : "none");
}
}  // namespace

namespace bf1943 {

void server_stats::Add(const std::string& name, const char* what, long long n) {
  std::lock_guard lock(g_mutex);
  AddLocked(name, what, n);
}

std::string TakeServerStatsMessage(const std::string& map) {
  if (!REXCVAR_GET(server)) return {};
  std::map<std::string, Counts> taken;
  {
    std::lock_guard lock(g_mutex);
    const auto now = Clock::now();
    for (auto& [name, l] : g_lives) TimeSoFar(name, l, now);  // the lives still going, so far
    for (auto& [name, c] : g_players) {
      if (c.score >= 1) {
        const long long whole = (long long)c.score;
        c.n["score"] += whole, c.score -= double(whole);
      }
      if (c.n.empty()) continue;
      taken[name] = c;
      c.n.clear(), c.best = c.streak;  // the streak goes on in this life
    }
  }
  if (taken.empty()) return {};
  const std::string key = MapKey(map);
  std::string rows;
  for (const auto& [name, c] : taken) {
    std::string row = "{\"name\": " + Quote(name);
    if (!key.empty()) row += ", \"map\": \"" + key + "\"";
    for (const auto& [k, v] : c.n) row += fmt::format(", \"{}\": {}", k, std::min(v, 100000LL));
    if (c.best > 0) row += fmt::format(", \"streak\": {}", c.best);
    rows += (rows.empty() ? "" : ", ") + row + "}";
  }
  return "{\"op\": \"player_stats\", \"rows\": [" + rows + "]}";
}

void ServerStatsTick() {
  static Clock::time_point last{};
  const auto now = Clock::now();
  if (now - last < std::chrono::seconds(5)) return;
  last = now;
  const std::string file = REXCVAR_GET(bf_stats_file);
  if (file.empty() || !REXCVAR_GET(server)) return;
  const std::string line = TakeServerStatsMessage(CurrentMapTitle());
  if (line.empty()) return;
  // Kept small: past 1 MB the file starts again (the host reads from its start when it shrinks).
  std::error_code ec;
  const bool fresh = std::filesystem::file_size(file, ec) > (1u << 20) && !ec;
  std::ofstream f(file, fresh ? std::ios::trunc : std::ios::app);
  f << line << '\n';
  if (!f) REXLOG_WARN("BF server stats: {} could not be written", file);
}

}  // namespace bf1943

// ---- the hooks

// Set while the game handles a kill whose victim is a bot (the server thread's, as the awards it gives meanwhile).
bool g_bot_victim = false;

REX_EXTERN(__imp__sub_823F8960);
REX_HOOK_RAW(sub_823F8960) {
  // A bot gets no award: the game would look up its score record, which a bot has none of (bots.cpp; run bots_s1d,
  // a null read after a bot's "flagcapture").
  if (REXCVAR_GET(server) && bf1943::BotIsPlayer(ctx.r4.u32)) return;
  if (REXCVAR_GET(server)) {
    const std::string name = PlayerName(base, ctx.r4.u32);
    if (!name.empty()) {
      const double points = ctx.f1.f64;
      const std::string label = Squash(Eastl(base, ctx.r6.u32));
      const char* what = nullptr;
      if (label == "meleekill") what = "melee";
      else if (label == "killassist" || label == "squadkillassist") what = "assists";
      else if (label == "flagcapture" || label == "capturedcapturepoint") what = "captures";
      else if (label == "defensivekillbonus") what = "defends";
      else if (label.find("repair") != std::string::npos) what = "repairs";
      else if (label.find("healing") != std::string::npos) what = "heals";
      std::lock_guard lock(g_mutex);
      if (g_bot_victim) what = nullptr;  // a bot's death: no score, no counts (the kill counts as a bot kill)
      if (!g_bot_victim && std::isfinite(points) && std::abs(points) < 10000) g_players[name].score += points;
      if (what) AddLocked(name, what, 1);
      static std::set<std::string> logged;
      if (logged.size() < 64 && logged.insert(label).second)
        REXLOG_INFO("BF server stats: award \"{}\" ({:+.0f}){}", label, points, what ? std::string(" counts as ") + what : std::string());
    }
  }
  __imp__sub_823F8960(ctx, base);
}

REX_EXTERN(__imp__sub_82409708);
REX_HOOK_RAW(sub_82409708) {
  if (REXCVAR_GET(server)) {
    const uint32_t killer = ctx.r4.u32, victim = ctx.r5.u32;
    const bool headshot = (ctx.r9.u32 & 0xFF) != 0;
    const std::string kname = killer ? PlayerName(base, killer) : std::string();
    const std::string vname = PlayerName(base, victim);
    const auto now = Clock::now();
    std::lock_guard lock(g_mutex);
    // A bot killed is no kill for the stats and no score (the user, 18 Nov 2026: "We also don't want bot kills to
    // count toward player's overall service record score. There should be a dedicated Bots counter"): it counts as
    // a bot kill, and the awards the game gives for it while handling this kill add no score (g_bot_victim).
    if (!kname.empty() && killer != victim && bf1943::BotIsPlayer(victim)) AddLocked(kname, "botkills", 1);
    if (!kname.empty() && killer != victim && !bf1943::BotIsPlayer(victim)) {
      if (!vname.empty() && L32(base, killer + 48) == L32(base, victim + 48)) {
        AddLocked(kname, "teamkills", 1);
      } else {
        AddLocked(kname, "kills", 1);
        if (headshot) AddLocked(kname, "headshots", 1);
        if (auto it = g_lives.find(kname); it != g_lives.end() && it->second.vehicle)
          AddLocked(kname, std::string("vk_") + it->second.vehicle, 1);
      }
    }
    // A death to a bot is no death for the stats either (a player's K/D stays the players' own).
    if (!vname.empty() && !(killer && bf1943::BotIsPlayer(killer))) {
      AddLocked(vname, "deaths", 1);
      if (killer == victim) AddLocked(vname, "suicides", 1);
      CloseLife(vname, g_lives[vname], now);
    }
    static int logged = 0;
    if (logged < 20) {
      ++logged;
      REXLOG_INFO("BF server stats: {} killed {}{}", kname.empty() ? "(the world)" : kname, vname.empty() ? "?" : vname,
                  headshot ? " (headshot)" : "");
    }
  }
  bf1943::BotsNoteKill(ctx, base, ctx.r4.u32, ctx.r5.u32);  // where it happened: what the bots learn
  g_bot_victim = REXCVAR_GET(server) && bf1943::BotIsPlayer(ctx.r5.u32);
  __imp__sub_82409708(ctx, base);
  g_bot_victim = false;
}

REX_EXTERN(__imp__sub_823F7518);
REX_HOOK_RAW(sub_823F7518) {
  if (REXCVAR_GET(server)) {
    const std::string name = PlayerName(base, ctx.r4.u32);
    if (!name.empty()) {
      const bool lost = (ctx.r5.u32 & 0xFF) != 0;
      std::lock_guard lock(g_mutex);
      AddLocked(name, "rounds", 1);
      AddLocked(name, lost ? "losses" : "wins", 1);
      REXLOG_INFO("BF server stats: round over, {} {}", name, lost ? "lost" : "won");
    }
  }
  __imp__sub_823F7518(ctx, base);
}

REX_EXTERN(__imp__sub_824144A0);
REX_HOOK_RAW(sub_824144A0) {
  if (REXCVAR_GET(server)) {
    const uint32_t ev = ctx.r4.u32;
    if (Plausible(ev) && Readable(ev, 192)) {
      const uint32_t type = L32(base, ev + 136);
      if (type == 17 || type == 15 || type == 16 || type == 1) {
        const std::string name = PlayerName(base, L32(base, ev + 144));
        if (!name.empty()) {
          const auto now = Clock::now();
          const std::string what = type == 1 ? std::string() : Eastl(base, ev + 156);
          std::lock_guard lock(g_mutex);
          Life& l = g_lives[name];
          if (type == 17) {  // a spawn, with its kit
            CloseLife(name, l, now);
            l.kit = KitKey(what), l.alive = true, l.alive_since = l.kit_since = now;
            NoteName("kit", what, l.kit);
          } else if (type == 15) {  // into a vehicle (a seat change: out, then in)
            CloseVehicle(name, l, now);
            l.vehicle = VehicleKey(what), l.vehicle_since = now;
            NoteName("vehicle", what, l.vehicle);
          } else if (type == 16) {
            CloseVehicle(name, l, now);
          } else {  // gone from the server
            CloseLife(name, l, now);
            g_lives.erase(name);
          }
        }
      }
    }
  }
  __imp__sub_824144A0(ctx, base);
}
