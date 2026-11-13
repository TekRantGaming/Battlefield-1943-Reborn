// A backend of its own inside a LAN match's server (13 Nov 2026: "We need to add Network Type as Online or LAN option
// to the Create Private Match submenu", LAN as PC games have it: no internet, no account service, the players of one
// network). The game still signs in and finds its match as on the console, so somebody has to answer: here the
// hosted server's own process does, with the part of our backend (server/plasma.py, ours) a single match needs,
// written again in C++ so the host needs no Python:
//   FESL (TCP 18241)      the sign-in: fsys/Hello, acct/NuXBL360Login, the stats and associations the menu asks for,
//                         pnow/Start (Quick Match: the one match here)
//   Theater (TCP 18246)   CONN, USER, GDAT, EGAM (the joiner is announced to our GameManager host, gm_host.cpp, over
//                         its registry connection, then gets the match's address in EGEG), PENT, PLVT
//   Registry (TCP 18250)  our own JSON lines: register and status from gm_host.cpp, list, find, reserve and name from
//                         the players' menus (server_list.h)
//   Discovery (UDP 18250) "BF43LAN?" answered with "BF43LAN!" and the match's line, for a list of the network's matches
// Every player gets, as the match's and Theater's address, the address of this PC it reached us on (plasma.py's
// --lan, own_address): 127.0.0.1 for the host's own game, the PC's network address for the others.
// Packets (plasma.py's head): 4 letters, a big endian word (the transmission in its top byte, the packet's number
// below), the big endian length with this 12 byte head, then "key=value" lines and a zero byte. A packet above 8064
// bytes goes in parts (base64 chunks).
// Started by gm_host.cpp before it registers, when bf_lan_backend is set (private_match.cpp sets it for a LAN match).

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "server_list.h"  // JsonField; winsock2.h

REXCVAR_DEFINE_BOOL(bf_lan_backend, false, "BF1943",
                    "A dedicated server (--server) answers the sign-in and matchmaking itself, for players of its own "
                    "network with no internet (a LAN match: FESL 18241, Theater 18246, the registry 18250)");

#if defined(_WIN32)
namespace bf1943 {
namespace {

using Fields = std::vector<std::pair<std::string, std::string>>;
constexpr uint32_t kRequest = 0xC0000000, kResponse = 0x80000000, kMultiResponse = 0xB0000000, kMultiRequest = 0xF0000000;
constexpr size_t kMultiLimit = 8064, kMultiChunk = 6000;

std::string Get(const Fields& f, const std::string& key, const std::string& fallback = {}) {
  for (const auto& [k, v] : f)
    if (k == key) return v;
  return fallback;
}

std::string Base64(const std::string& in) {
  static const char* kT = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < in.size(); i += 3) {
    const uint32_t a = uint8_t(in[i]), b = i + 1 < in.size() ? uint8_t(in[i + 1]) : 0, c = i + 2 < in.size() ? uint8_t(in[i + 2]) : 0;
    const uint32_t v = a << 16 | b << 8 | c;
    out += kT[v >> 18], out += kT[(v >> 12) & 63];
    out += i + 1 < in.size() ? kT[(v >> 6) & 63] : '=';
    out += i + 2 < in.size() ? kT[v & 63] : '=';
  }
  return out;
}
std::string Unbase64(const std::string& in) {
  std::string out;
  uint32_t v = 0;
  int bits = 0;
  for (char ch : in) {
    int d = ch >= 'A' && ch <= 'Z' ? ch - 'A' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26 : ch >= '0' && ch <= '9' ? ch - '0' + 52
            : ch == '+' ? 62 : ch == '/' ? 63 : -1;
    if (d < 0) continue;
    v = v << 6 | uint32_t(d), bits += 6;
    if (bits >= 8) bits -= 8, out += char((v >> bits) & 0xFF);
  }
  return out;
}
std::string Replaced(std::string s, const std::string& from, const std::string& to) {
  for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
  return s;
}

Fields Parse(const std::string& body) {
  Fields out;
  size_t at = 0;
  const size_t end = body.find('\0') == std::string::npos ? body.size() : body.find('\0');
  while (at < end) {
    size_t nl = body.find('\n', at);
    if (nl == std::string::npos || nl > end) nl = end;
    const std::string line = body.substr(at, nl - at);
    at = nl + 1;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string v = line.substr(eq + 1);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    out.push_back({line.substr(0, eq), v});
  }
  return out;
}
std::string Build(const std::string& kind, uint32_t transmission, uint32_t id, const Fields& fields) {
  std::string body;
  for (size_t i = 0; i < fields.size(); ++i) {
    const auto& [k, v] = fields[i];
    body += (i ? "\n" : "") + k + "=" + (v.find(' ') != std::string::npos ? "\"" + v + "\"" : v);
  }
  body += '\0';
  const uint32_t word = (transmission & 0xFF000000) | (id & 0xFFFFFF), length = uint32_t(12 + body.size());
  std::string out = (kind + "    ").substr(0, 4);
  for (uint32_t w : {word, length})
    for (int s = 24; s >= 0; s -= 8) out += char((w >> s) & 0xFF);
  return out + body;
}
std::string JsonQ(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\', out += c;
    else if (static_cast<unsigned char>(c) >= 0x20) out += c;
  }
  return out + "\"";
}
std::string Printable(const std::string& s, size_t most) {
  std::string out;
  for (char c : s)
    if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F && out.size() < most) out += c;
  return out;
}

// ---- what the backend knows (one match's worth), under g_mutex
struct Game {
  std::string gid, name, map, next_map, mode = "classic", xnkid, xnkey;
  uint32_t uid = 0, tick = 0;
  int port = 0, players = 0, max_players = 24;
  bool listed = false;
  SOCKET host = INVALID_SOCKET;  // its registry connection: joiners are announced there
};
struct Session {
  std::string persona, xuid;
  uint32_t user_id = 0;
};
std::mutex g_mutex;
std::map<std::string, Game> g_games;                // by GID
std::map<std::string, Session> g_sessions;          // by the login's key
std::map<std::string, std::string> g_reservations;  // a player's address -> GID
std::atomic<uint32_t> g_user_ids{11}, g_pids{7}, g_game_ids{201}, g_pnow_ids{1};
std::mt19937 g_random{std::random_device{}()};
std::mutex g_registry_write;  // a match's registry connection is written by its own thread and by a joiner's

std::string RandomBytes(size_t n) {
  std::string out;
  std::lock_guard lock(g_mutex);
  for (size_t i = 0; i < n; ++i) out += char(g_random() & 0xFF);
  return out;
}

bool SendAll(SOCKET s, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const int n = send(s, data.data() + sent, int(data.size() - sent), 0);
    if (n <= 0) return false;
    sent += size_t(n);
  }
  return true;
}
bool ReadExactly(SOCKET s, std::string& out, size_t n) {
  out.resize(n);
  size_t got = 0;
  while (got < n) {
    const int r = recv(s, out.data() + got, int(n - got), 0);
    if (r <= 0) return false;
    got += size_t(r);
  }
  return true;
}
std::string PeerIp(SOCKET s) {
  sockaddr_in a{};
  int n = sizeof(a);
  char text[32] = {};
  if (getpeername(s, reinterpret_cast<sockaddr*>(&a), &n) == 0) inet_ntop(AF_INET, &a.sin_addr, text, sizeof(text));
  return text;
}
// The address of this PC the player reached us on: the one that works for that player.
std::string OwnIp(SOCKET s) {
  sockaddr_in a{};
  int n = sizeof(a);
  char text[32] = {};
  if (getsockname(s, reinterpret_cast<sockaddr*>(&a), &n) == 0) inet_ntop(AF_INET, &a.sin_addr, text, sizeof(text));
  return *text ? text : "127.0.0.1";
}
// An XNADDR for an address (gm_host.cpp's XnAddr, gmhost.py's xnaddr): ina, 6 zero bytes, a MAC of CC bytes, 20 zeros.
std::string XnAddrOf(const std::string& ip) {
  in_addr a{};
  inet_pton(AF_INET, ip.c_str(), &a);
  return std::string(reinterpret_cast<const char*>(&a), 4) + std::string(6, '\0') + std::string(6, '\xCC') + std::string(20, '\0');
}

// One FESL or Theater connection.
struct Conn {
  SOCKET s = INVALID_SOCKET;
  bool theater = false;
  std::mutex write;  // the keepalive thread writes too
  std::atomic<bool> open{true};
  uint32_t user_id = 0;
  std::string persona, lkey, xuid = "0", name = "Player";
  uint32_t uid = 0;

  bool Send(const std::string& kind, uint32_t transmission, uint32_t id, const Fields& fields) {
    const std::string packet = Build(kind, transmission, id, fields);
    std::lock_guard lock(write);
    if (packet.size() <= kMultiLimit) return SendAll(s, packet);
    // In parts: each carries a base64 chunk of the payload ("=" written %3d), the base64's whole size and the
    // payload's.
    const std::string payload = packet.substr(12);
    std::vector<std::string> chunks;
    size_t total = 0;
    for (size_t i = 0; i < payload.size(); i += kMultiChunk) {
      chunks.push_back(Replaced(Base64(payload.substr(i, kMultiChunk)), "=", "%3d"));
      total += chunks.back().size();
    }
    for (const std::string& c : chunks)
      if (!SendAll(s, Build(kind, kMultiResponse, id,
                            {{"data", c}, {"size", std::to_string(total)}, {"decodedSize", std::to_string(payload.size())}})))
        return false;
    return true;
  }
};

void Fesl(Conn& c, const std::string& kind, uint32_t id, const Fields& f) {
  const std::string txn = Get(f, "TXN"), key = kind + "/" + txn;
  if (key == "fsys/Hello") {
    const std::string theater = OwnIp(c.s);
    std::string sub = Get(f, "clientString", "beach");
    sub = sub.substr(0, sub.find('-'));
    for (char& ch : sub) ch = char(toupper(static_cast<unsigned char>(ch)));
    c.Send("fsys", kResponse, id,
           {{"domainPartition.domain", Get(f, "sku", "xenon")}, {"messengerIp", theater}, {"messengerPort", "0"},
            {"domainPartition.subDomain", sub}, {"TXN", "Hello"}, {"activityTimeoutSecs", "0"},
            {"curTime", "Nov-13-2026 12:00:00 UTC"}, {"theaterIp", theater}, {"theaterPort", "18246"}});
    c.Send("fsys", kRequest, 0, {{"TXN", "MemCheck"}, {"memcheck.[]", "0"}, {"type", "0"}, {"salt", "123456789"}});
  } else if (key == "fsys/MemCheck" || key == "fsys/Ping") {
  } else if (key == "fsys/Goodbye") {
    c.Send(kind, kResponse, id, {{"TXN", txn}});
    c.open = false;
  } else if (key == "acct/NuXBL360Login") {
    // The menu's "Connecting" dialog needs a moment before the answer (plasma.py's login delay).
    std::this_thread::sleep_for(std::chrono::seconds(2));
    c.user_id = g_user_ids.fetch_add(1);
    c.persona = Get(f, "gamertag", Get(f, "name", "Player" + std::to_string(c.user_id)));
    {
      // Two players of one name: the later one gets a number (a host finds a joiner by name).
      std::lock_guard lock(g_mutex);
      std::string wanted = c.persona;
      for (int n = 2; n < 100; ++n) {
        bool taken = false;
        for (const auto& [k, s] : g_sessions) taken = taken || _stricmp(s.persona.c_str(), wanted.c_str()) == 0;
        if (!taken) break;
        wanted = c.persona.substr(0, 13) + std::to_string(n);
      }
      c.persona = wanted;
      char lkey[32];
      std::snprintf(lkey, sizeof(lkey), "BF1943LKEY%08u", c.user_id);
      c.lkey = lkey;
      g_sessions[c.lkey] = {c.persona, Get(f, "xuid", "0"), c.user_id};
    }
    REXLOG_INFO("BF LAN backend: {} signed in from {}", c.persona, PeerIp(c.s));
    c.Send(kind, kResponse, id,
           {{"TXN", txn}, {"lkey", c.lkey}, {"userId", std::to_string(c.user_id)}, {"personaName", c.persona}});
  } else if (key == "fsys/GetPingSites") {
    c.Send(kind, kResponse, id, {{"TXN", txn}, {"pingSite.[]", "0"}, {"minPingSitesToPing", "0"}});
  } else if (key == "asso/GetAssociations") {
    c.Send(kind, kResponse, id,
           {{"TXN", txn}, {"domainPartition.domain", Get(f, "domainPartition.domain", "xenon")},
            {"domainPartition.subDomain", Get(f, "domainPartition.subDomain", "BEACH")},
            {"owner.id", std::to_string(c.user_id)}, {"owner.type", "1"}, {"type", Get(f, "type")},
            {"owner.name", c.persona.empty() ? "Player" : c.persona}, {"maxListSize", "100"}, {"members.[]", "0"}});
  } else if (key == "acct/GetTelemetryToken") {
    c.Send(kind, kResponse, id,
           {{"TXN", txn}, {"filters", ""}, {"enabled", ""}, {"disabled", ""},
            {"telemetryToken", Base64("127.0.0.1,9946,enUS,bf1943-no-telemetry")}});
  } else if (key == "rank/GetStats") {
    const int count = std::atoi(Get(f, "keys.[]", "0").c_str());
    Fields out = {{"TXN", txn}, {"stats.[]", std::to_string(count)}};
    for (int i = 0; i < count; ++i) {
      const std::string stat = Get(f, "keys." + std::to_string(i));
      out.push_back({"stats." + std::to_string(i) + ".key", stat});
      // pm_minplayers empty (0.0 hangs the game, plasma.py); the rest zero.
      out.push_back({"stats." + std::to_string(i) + ".value", stat == "pm_minplayers" ? "" : "0.0"});
    }
    c.Send(kind, kResponse, id, out);
  } else if (key == "pnow/Start") {
    // Quick Match: the match the player's menu picked ("reserve"), else the one match here.
    const uint32_t session = g_pnow_ids.fetch_add(1);
    const std::string partition = Get(f, "partition.partition", "/xenon/BEACH");
    c.Send(kind, kResponse, id, {{"TXN", "Start"}, {"id.id", std::to_string(session)}, {"id.partition", partition}});
    Fields status = {{"TXN", "Status"}, {"id.id", std::to_string(session)}, {"id.partition", partition}, {"sessionState", "COMPLETE"}};
    std::string gid;
    {
      std::lock_guard lock(g_mutex);
      if (auto it = g_reservations.find(PeerIp(c.s)); it != g_reservations.end() && g_games.count(it->second)) gid = it->second;
      else if (!g_games.empty()) gid = g_games.begin()->first;
    }
    if (!gid.empty()) {
      status.insert(status.end(), {{"props.{}", "3"}, {"props.{resultType}", "JOIN"}, {"props.{avgFit}", "1.0"},
                                   {"props.{games}.[]", "1"}, {"props.{games}.0.gid", gid}, {"props.{games}.0.lid", "1"}});
    } else {
      status.insert(status.end(), {{"props.{}", "1"}, {"props.{resultType}", "NOSERVER"}});
    }
    c.Send(kind, kResponse, id, status);
  } else {
    c.Send(kind, kResponse, id, {{"TXN", txn}});
  }
}

void Theater(Conn& c, const std::string& kind, uint32_t transmission, const Fields& f) {
  const std::string tid = Get(f, "TID");
  if (kind == "CONN") {
    c.Send("CONN", 0, 0, {{"TIME", std::to_string(time(nullptr))}, {"TID", tid}, {"activityTimeoutSecs", "0"}, {"PROT", Get(f, "PROT", "2")}});
  } else if (kind == "USER") {
    {
      std::lock_guard lock(g_mutex);
      if (auto it = g_sessions.find(Get(f, "LKEY")); it != g_sessions.end())
        c.name = it->second.persona, c.uid = it->second.user_id, c.xuid = it->second.xuid;
    }
    c.Send("USER", 0, 0, {{"NAME", c.name}, {"TID", tid}});
  } else if (kind == "PING") {
  } else if (kind == "GDAT" || kind == "EGAM") {
    Game g;
    bool found = false;
    {
      std::lock_guard lock(g_mutex);
      if (auto it = g_games.find(Get(f, "GID")); it != g_games.end()) g = it->second, found = true;
    }
    const std::string ip = OwnIp(c.s);  // this PC as the player reaches it
    if (kind == "GDAT") {
      if (!found) {
        c.Send("GDAT", 0, 0, {{"TID", tid}});
        return;
      }
      c.Send("GDAT", 0, 0,
             {{"GID", g.gid}, {"LID", "1"}, {"I", ip}, {"P", std::to_string(g.port)}, {"HU", std::to_string(g.uid)},
              {"HN", g.name}, {"N", g.name}, {"TYPE", "G"}, {"JOIN", "O"}, {"MAX-PLAYERS", std::to_string(g.max_players)},
              {"AP", std::to_string(g.players)}, {"UGID", "NOGUID"}, {"B-version", "RETAIL429432"}, {"B-U-type", "RANKED"},
              {"TID", tid}});
      return;
    }
    c.Send("EGAM", 0, 0, {{"TID", tid}, {"LID", Get(f, "LID")}, {"GID", Get(f, "GID")}});
    if (!found) return;
    const uint32_t pid = g_pids.fetch_add(1);
    char ticket[24];
    {
      std::lock_guard lock(g_mutex);
      std::snprintf(ticket, sizeof(ticket), "0x%x", unsigned(g_random() & 0x7FFFFFFF));
    }
    // Our GameManager host learns the player first (its hello names the ticket), then the player gets the address.
    const std::string join = "{\"op\": \"join\", \"ticket\": " + JsonQ(ticket) + ", \"pid\": " + std::to_string(pid) +
                             ", \"name\": " + JsonQ(c.name) + ", \"xuid\": " + JsonQ(c.xuid) + ", \"xnaddr\": " +
                             JsonQ(Get(f, "R-XNADDR")) + "}\n";
    if (g.host != INVALID_SOCKET) {
      std::lock_guard lock(g_registry_write);
      SendAll(g.host, join);
    }
    REXLOG_INFO("BF LAN backend: {} joins the match (PID {}), told {}:{}", c.name, pid, ip, g.port);
    c.Send("EGEG", 0, 0,
           {{"PL", "xenon"}, {"TICKET", ticket}, {"PID", std::to_string(pid)}, {"HUID", std::to_string(g.uid)}, {"I", ip},
            {"P", std::to_string(g.port)}, {"INT-IP", ip}, {"INT-PORT", std::to_string(g.port)}, {"EKEY", "NOENCYRPTIONKEY"},
            {"UGID", "NOGUID"}, {"LID", "1"}, {"GID", g.gid}, {"XNADDR", Base64(XnAddrOf(ip))}, {"XNKID", g.xnkid},
            {"XNKEY", g.xnkey}});
  } else if (kind == "PENT" || kind == "PLVT") {
    c.Send(kind, 0, 0, {{"TID", tid}, {"PID", Get(f, "PID")}, {"LID", Get(f, "LID")}, {"GID", Get(f, "GID")}});
  } else if (transmission != 0) {
    c.Send(kind, 0, 0, {{"TID", tid}});
  }
}

void ServeConn(SOCKET s, bool theater) {
  Conn c;
  c.s = s, c.theater = theater;
  // The game drops a connection silent for four minutes: a ping every 30 s, as EA's servers sent.
  std::thread pinger([&c] {
    for (int i = 0; c.open; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      if (i % 60 != 59 || !c.open) continue;
      if (c.theater) c.Send("PING", 0, 0, {{"TID", "0"}});
      else c.Send("fsys", kRequest, 0, {{"TXN", "Ping"}});
    }
  });
  std::string multi_data;
  uint32_t multi_id = ~0u;
  size_t multi_b64 = 0;
  while (c.open) {
    std::string head, body;
    if (!ReadExactly(s, head, 12)) break;
    auto be = [&](size_t at) {
      return uint32_t(uint8_t(head[at])) << 24 | uint32_t(uint8_t(head[at + 1])) << 16 | uint32_t(uint8_t(head[at + 2])) << 8 |
             uint32_t(uint8_t(head[at + 3]));
    };
    const std::string kind = head.substr(0, 4);
    const uint32_t word = be(4), length = be(8);
    if (length < 12 || length > (1u << 20)) break;
    if (length > 12 && !ReadExactly(s, body, length - 12)) break;
    const uint32_t transmission = word & 0xFF000000, id = word & 0xFFFFFF;
    Fields fields = Parse(body);
    if (transmission == kMultiResponse || transmission == kMultiRequest) {
      const std::string part = Replaced(Get(fields, "data"), "%3d", "=");
      if (multi_id != id) multi_id = id, multi_data.clear(), multi_b64 = 0;
      multi_data += Unbase64(part), multi_b64 += part.size();
      if (multi_b64 < size_t(std::atoll(Get(fields, "size", "0").c_str()))) continue;
      multi_id = ~0u;
      fields = Parse(multi_data);
    }
    if (theater) Theater(c, kind, transmission, fields);
    else Fesl(c, kind, id, fields);
  }
  c.open = false;
  pinger.join();
  if (!c.lkey.empty()) {
    std::lock_guard lock(g_mutex);
    if (!theater) REXLOG_INFO("BF LAN backend: {} signed out", c.persona);
    // The login stays known a while for Theater's USER (the game links after its sign-in); one match's worth of
    // players is no burden, so it is simply kept.
  }
  closesocket(s);
}

std::string Listing(const Game& g, const std::string& ip) {
  return "{\"gid\": " + JsonQ(g.gid) + ", \"name\": " + JsonQ(g.name) + ", \"address\": [" + JsonQ(ip) + ", " +
         std::to_string(g.port) + "], \"map\": " + JsonQ(g.map) + ", \"players\": " + std::to_string(g.players) +
         ", \"max_players\": " + std::to_string(g.max_players) + ", \"mode\": " + JsonQ(g.mode) +
         ", \"description\": \"A LAN match\", \"official\": false, \"next_map\": " + JsonQ(g.next_map) +
         ", \"maps\": [], \"night\": false, \"region\": \"\", \"tick\": " + std::to_string(g.tick) + "}";
}

// The registry: our host's registration and the players' menus, a JSON object a line.
void ServeRegistry(SOCKET s) {
  std::string buffer, gid;
  char chunk[2048];
  const std::string peer = PeerIp(s), own = OwnIp(s);
  for (;;) {
    size_t nl;
    while ((nl = buffer.find('\n')) == std::string::npos) {
      const int n = recv(s, chunk, sizeof(chunk), 0);
      if (n <= 0) goto done;
      buffer.append(chunk, size_t(n));
      if (buffer.size() > (1u << 20)) goto done;
    }
    {
      const std::string m = buffer.substr(0, nl);
      buffer.erase(0, nl + 1);
      const std::string op = JsonField(m, "op");
      std::string reply;
      if (op == "register" && gid.empty()) {
        Game g;
        g.gid = std::to_string(g_game_ids.fetch_add(1));
        g.uid = 900 + uint32_t(std::atoi(g.gid.c_str()) - 200);
        g.name = Printable(JsonField(m, "name"), 48);
        if (g.name.empty()) g.name = "LAN match";
        g.port = std::atoi(JsonField(m, "port").c_str());
        if (g.port <= 0) g.port = 18300;
        g.map = JsonField(m, "map");
        if (const int most = std::atoi(JsonField(m, "max_players").c_str()); most > 0) g.max_players = most;
        g.mode = JsonField(m, "mode") == "modern" ? "modern" : "classic";
        g.tick = uint32_t(std::atoi(JsonField(m, "tick").c_str()));
        g.listed = true;  // on its own network every match is listed: there is no other way to find it
        g.xnkid = Base64(RandomBytes(8)), g.xnkey = Base64(RandomBytes(16));
        g.host = s;
        gid = g.gid;
        const uint32_t pid = g_pids.fetch_add(1);
        {
          std::lock_guard lock(g_mutex);
          g_games[gid] = g;
        }
        REXLOG_INFO("BF LAN backend: the match \"{}\" registered (game {}, UDP {})", g.name, gid, g.port);
        reply = "{\"op\": \"registered\", \"gid\": " + JsonQ(gid) + ", \"uid\": " + std::to_string(g.uid) + ", \"pid\": " +
                std::to_string(pid) + ", \"official\": false, \"listed\": true, \"address\": " +
                JsonQ(own + ":" + std::to_string(g.port)) + "}\n" +
                // On this machine: whether other PCs reach it cannot be told from here (the firewall).
                "{\"op\": \"reachable\", \"ok\": null, \"local\": true, \"address\": " + JsonQ(own + ":" + std::to_string(g.port)) + "}\n";
      } else if (op == "status" && !gid.empty()) {
        std::lock_guard lock(g_mutex);
        if (auto it = g_games.find(gid); it != g_games.end()) {
          it->second.players = std::atoi(JsonField(m, "players").c_str());
          if (const std::string map = JsonField(m, "map"); !map.empty()) it->second.map = map;
          it->second.next_map = JsonField(m, "next_map");
        }
      } else if (op == "list" || op == "find") {
        std::lock_guard lock(g_mutex);
        std::string servers, first;
        for (const auto& [id, g] : g_games) {
          if (first.empty()) first = Listing(g, own);
          servers += (servers.empty() ? "" : ", ") + Listing(g, own);
        }
        if (op == "list") reply = "{\"op\": \"servers\", \"servers\": [" + servers + "], \"players_online\": " + std::to_string(g_sessions.size()) + "}\n";
        else reply = "{\"op\": \"found\", \"server\": " + (first.empty() ? std::string("null") : first) + "}\n";
      } else if (op == "reserve") {
        std::lock_guard lock(g_mutex);
        const std::string want = JsonField(m, "gid");
        if (g_games.count(want)) {
          g_reservations[peer] = want;
          reply = "{\"op\": \"reserved\", \"gid\": " + JsonQ(want) + "}\n";
        }
      } else if (op == "name") {
        reply = "{\"op\": \"name\", \"name\": " + JsonQ(Printable(JsonField(m, "name"), 15)) + "}\n";
      } else if (op == "players" || op == "scoreboard") {
        reply = "{\"op\": " + JsonQ(op) + ", \"rows\": []}\n";
      } else if (op == "leaderboard") {
        reply = "{\"op\": \"leaderboard\", \"rows\": [], \"total\": 0}\n";
      } else if (op == "match_score" || op == "stats_put") {
        reply = "{\"op\": " + JsonQ(op) + ", \"ok\": false}\n";
      }
      if (!reply.empty()) {
        std::lock_guard lock(g_registry_write);
        if (!SendAll(s, reply)) goto done;
      }
    }
  }
done:
  if (!gid.empty()) {
    std::lock_guard lock(g_mutex);
    g_games.erase(gid);
    REXLOG_INFO("BF LAN backend: the match {} is gone (its host disconnected)", gid);
  }
  closesocket(s);
}

SOCKET Listen(uint16_t port) {
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return s;
  BOOL yes = TRUE;
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof(yes));
  sockaddr_in a{};
  a.sin_family = AF_INET, a.sin_port = htons(port), a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(s, 16) != 0) {
    closesocket(s);
    return INVALID_SOCKET;
  }
  return s;
}

template <typename F>
void Accept(SOCKET listener, F&& serve) {
  std::thread([listener, serve] {
    for (;;) {
      const SOCKET s = accept(listener, nullptr, nullptr);
      if (s == INVALID_SOCKET) return;
      BOOL yes = TRUE;
      setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
      std::thread([s, serve] { serve(s); }).detach();
    }
  }).detach();
}

// Discovery: a broadcast "BF43LAN?" on UDP 18250 is answered with "BF43LAN!" and the match's line.
void Discovery() {
  const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return;
  sockaddr_in a{};
  a.sin_family = AF_INET, a.sin_port = htons(18250), a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    closesocket(s);
    return;
  }
  for (;;) {
    char in[64];
    sockaddr_in from{};
    int n = sizeof(from);
    const int got = recvfrom(s, in, sizeof(in), 0, reinterpret_cast<sockaddr*>(&from), &n);
    if (got < 8 || std::memcmp(in, "BF43LAN?", 8) != 0) {
      if (got < 0 && WSAGetLastError() != WSAECONNRESET) break;
      continue;
    }
    std::string out = "BF43LAN!";
    {
      std::lock_guard lock(g_mutex);
      // The address is the one the asker sees this answer come from.
      if (!g_games.empty()) out += Listing(g_games.begin()->second, "");
    }
    if (out.size() > 8) sendto(s, out.data(), int(out.size()), 0, reinterpret_cast<sockaddr*>(&from), sizeof(from));
  }
  closesocket(s);
}

}  // namespace

// From gm_host.cpp, before it registers. False (logged) when a port is taken: another backend runs on this PC.
bool StartLanBackend() {
  if (!REXCVAR_GET(bf_lan_backend)) return true;
  static bool started = false, ok = false;
  if (started) return ok;
  started = true;
  const SOCKET fesl = Listen(18241), theater = Listen(18246), registry = Listen(18250);
  if (fesl == INVALID_SOCKET || theater == INVALID_SOCKET || registry == INVALID_SOCKET) {
    REXLOG_ERROR("BF LAN backend: a port is taken (TCP 18241, 18246 or 18250): another backend or LAN match runs on this PC");
    for (SOCKET s : {fesl, theater, registry})
      if (s != INVALID_SOCKET) closesocket(s);
    return false;
  }
  Accept(fesl, [](SOCKET s) { ServeConn(s, false); });
  Accept(theater, [](SOCKET s) { ServeConn(s, true); });
  Accept(registry, [](SOCKET s) { ServeRegistry(s); });
  std::thread(Discovery).detach();
  REXLOG_INFO("BF LAN backend: answering sign-in (TCP 18241), matchmaking (18246), the server list (18250) and "
              "discovery (UDP 18250) for this network");
  return ok = true;
}

}  // namespace bf1943
#else
namespace bf1943 {
bool StartLanBackend() { return !REXCVAR_GET(bf_lan_backend); }
}  // namespace bf1943
#endif
