// The backend's server list for our menu (server/plasma.py, class Registry): one JSON object per line over TCP.
// list: every listed server with name, address, map, players, description, official; find: one server by its address
// (Direct connect); reserve: the player's next Quick Match joins that game. While the browser is open each listed
// server is pinged over UDP (its host answers BF43PING with BF43PONG, server/gmhost.py).
//
// The version gate (TekRantGaming, 13 Nov 2026: "server version check for latest version and only to allow latest
// version on and reject and prompt outdated clicnt connections give them a popup advising and a button to update now
// or close"; server/plasma.py VERSION_GATE): list, find and reserve say this game's version and platform. The list's
// answer names the version players need, if any (required_version), and reserve answers "outdated" in place of
// "reserved" for a game older than it: the menu then shows its update pop-up (game_menu.cpp Join, AskToUpdate).
//
// Maintenance mode (TekRantGaming, 15 Nov 2026; server/plasma.py maintenance_now): this game says "notice": 1, and
// while the backend's switch is on its list answer carries notice_kind, notice_title, notice_text and notice_seconds
// (the real servers stay listed), and reserve answers "closed" with a title and a message. Any refusal of reserve with
// a "message" ("closed", "full", later ones) is shown in the notice pop-up (game_menu.cpp NoticePopup). A notice is kept
// while the backend cannot be reached (a deploy restarts it), until its end time.

#pragma once

#include "night_ops.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

REXCVAR_DECLARE(std::string, bf_backend_registry);
REXCVAR_DECLARE(std::string, bf_online_host);

namespace bf1943 {

const char* BfVersion();  // pipeline_upload.cpp
uint32_t OnlinePlayerId();  // live_layers.cpp

// This game's version and platform, as the fields of a registry request.
inline std::string ClientFields() {
#if defined(_WIN32)
  const char* platform = "windows";
#else
  const char* platform = "linux";
#endif
  return std::string("\"version\": \"") + BfVersion() + "\", \"platform\": \"" + platform + "\", \"notice\": 1";
}

// A join's fields ("reserve", "queue"): those, and who asks. Others can share this address (a household, a LAN party),
// so the backend checks bans as this player (server/plasma.py asker: believed only where this id signed in).
inline std::string JoinFields() {
  char id[9];
  std::snprintf(id, sizeof(id), "%08X", OnlinePlayerId());
  return std::string("\"id\": \"") + id + "\", " + ClientFields();
}

int TickRate();  // tick_rate.cpp: this game's simulation rate (bf_tick_rate: 30, or 60)
bool SetTickRate(int rate);  // the rate of the server about to be joined; false when this run cannot change to it
bool TickRateFixed();        // the game's timers are made: the rate is this run's

struct ServerInfo {
  std::string gid, name, address, map;
  // A server of an older version that plays the retired Modern movement and aiming (modern_mode.cpp): never listed
  // and never found, since v0.4.0's game plays the original everywhere and would be pulled about on it.
  bool retired_mode = false;
  int players = 0, max_players = 0;
  // Its bots now (a backend and host from 21 Nov 2026; 0 from older ones): shown before the players, "(4) 0 / 24"
  // (TekRantGaming). Not players: they leave a slot to a joining player (bots.cpp's backfill keeps two free and gives
  // up one for each player who joins), so they never make a server full, and HAS PLAYERS means people.
  int bots = 0;
  // Players waiting in its queue (0.4.7.4, server/plasma.py QUEUE); -1: a backend without the queue (a full server
  // there is refused, and the browser does not offer it).
  int queue = -1;
  bool Full() const { return max_players > 0 && players >= max_players; }
  std::string description, next_map;
  // Listed with the Reborn servers: "official" from the backend. A promoted community server (TekRantGaming, 16 Nov
  // 2026: someone else's machine he shows with ours, and can send back to the Community tab) says "promoted" too: it
  // is on the Reborn tab, marked TRUSTED COMMUNITY SERVER (PROMOTED before 0.4.7.4), and never said to be run by TRG
  // Studios. Reborn(): one of ours.
  bool official = false;
  bool promoted = false;
  bool Reborn() const { return official && !promoted; }
  bool night = false;  // a Night Ops server: its maps drawn at night (NativeXRenderer D162) while playing there
  bool daybreak = false;  // a Daybreak server (26 Nov 2026): each round from sunrise, a storm building (gmhost.py)
  std::string region;  // where it is, a region code (regions.h); empty: not said (an older server)
  int tick_hz = 0;     // its tick rate; 0: not said
  // 0.4.1: a server is only listed, found and joined by a game simulating at its rate (a server that says none runs
  // the game's own 30): a 30 Hz game on a 60 Hz server, or the other way round, is pulled about by every correction.
  // (13 Nov 2026, later: every server is listed whatever its rate; the game takes the server's when it joins.)
  bool OurTick() const { return (tick_hz > 0 ? tick_hz : 30) == TickRate(); }
  // The rate it runs at: 30, 60, or the experimental 75 or 100 (one that says none, or another, runs 30).
  int Tick() const { return tick_hz == 60 || tick_hz == 75 || tick_hz == 100 ? tick_hz : 30; }
  int ping_ms = -1;  // -1: not measured (an older server does not answer)
  // 13 Nov 2026: hosted from a player's own game (Create Private Match), not a dedicated server: its description
  // begins with kPlayerHostTag (the backend passes descriptions through as they are), taken off here. The browser
  // draws a person before its name, a server rack before the others.
  bool player_hosted = false;
  // A match with a password (Custom Match): its description says "[locked]" the same way. Its host only lets in a
  // player whose game showed the password first (KnockMatch, gm_host.cpp).
  bool locked = false;
};
inline constexpr const char* kPlayerHostTag = "[player host]";
inline constexpr const char* kLockedTag = "[locked]";
// The tags at a description's start taken off it, into the server's fields.
inline void TakeDescriptionTags(ServerInfo& s) {
  for (bool again = true; again;) {
    again = false;
    while (!s.description.empty() && s.description.front() == ' ') s.description.erase(s.description.begin());
    for (const auto& [tag, field] : {std::pair<const char*, bool ServerInfo::*>{kPlayerHostTag, &ServerInfo::player_hosted},
                                     std::pair<const char*, bool ServerInfo::*>{kLockedTag, &ServerInfo::locked}}) {
      if (s.description.rfind(tag, 0) != 0) continue;
      s.*field = true;
      s.description.erase(0, std::strlen(tag));
      again = true;
    }
  }
}
// A match's password as it goes over the network: not the words themselves (a 64 bit FNV-1a of them, in hex). It
// keeps a password out of plain sight on the wire; it is no more than that.
inline std::string MatchPasswordHash(const std::string& password) {
  uint64_t h = 1469598103934665603ull;
  for (const char c : "bf1943-match:" + password) h = (h ^ uint8_t(c)) * 1099511628211ull;
  char text[20];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(h));
  return text;
}
// The password shown to a match's host before joining: "BF43PASS", the player's name, a zero byte and the hash, to
// the match's own UDP port; "BF43PSOK" back lets that player in for the next two minutes. 1 let in, 0 refused, -1
// no answer (three tries, 0.5 s each).
inline int KnockMatch(const std::string& address, const std::string& name, const std::string& password) {
  const size_t colon = address.rfind(':');
  if (colon == std::string::npos) return -1;
  sockaddr_in to{};
  to.sin_family = AF_INET, to.sin_port = htons(uint16_t(std::atoi(address.c_str() + colon + 1)));
  if (inet_pton(AF_INET, address.substr(0, colon).c_str(), &to.sin_addr) != 1) return -1;
  const auto s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#if defined(_WIN32)
  if (s == INVALID_SOCKET) return -1;
  DWORD wait_ms = 500;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&wait_ms), sizeof(wait_ms));
#else
  if (s < 0) return -1;
  timeval wait{0, 500000};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
#endif
  const std::string packet = "BF43PASS" + name + std::string(1, '\0') + MatchPasswordHash(password);
  int answer = -1;
  for (int attempt = 0; attempt < 3 && answer < 0; ++attempt) {
    sendto(s, packet.data(), int(packet.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    char reply[16] = {};
    const int n = int(recv(s, reply, sizeof(reply), 0));
    if (n >= 8 && std::memcmp(reply, "BF43PSOK", 8) == 0) answer = 1;
    else if (n >= 8 && std::memcmp(reply, "BF43PSNO", 8) == 0) answer = 0;
  }
#if defined(_WIN32)
  closesocket(s);
#else
  close(s);
#endif
  return answer;
}

// The end of the JSON object or array that starts at `at` (strings and nesting respected), or npos.
inline size_t JsonEnd(const std::string& s, size_t at) {
  int depth = 0;
  bool str = false;
  for (size_t i = at; i < s.size(); ++i) {
    const char c = s[i];
    if (str) {
      if (c == '\\') ++i;
      else if (c == '"') str = false;
    } else if (c == '"') {
      str = true;
    } else if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      if (--depth == 0) return i;
    }
  }
  return std::string::npos;
}

// A JSON string's text from its opening quote (the escapes Python's json writes, \uXXXX as UTF-8).
inline std::string JsonString(const std::string& s, size_t at) {
  std::string out;
  for (size_t i = at + 1; i < s.size() && s[i] != '"'; ++i) {
    if (s[i] != '\\' || i + 1 >= s.size()) {
      out += s[i];
      continue;
    }
    const char e = s[++i];
    if (e == 'n' || e == 't' || e == 'r') out += ' ';
    else if (e == 'u' && i + 4 < s.size()) {
      const unsigned v = unsigned(std::strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16));
      i += 4;
      if (v < 0x80) out += char(v);
      else if (v < 0x800) out += char(0xC0 | (v >> 6)), out += char(0x80 | (v & 0x3F));
      else out += char(0xE0 | (v >> 12)), out += char(0x80 | ((v >> 6) & 0x3F)), out += char(0x80 | (v & 0x3F));
    } else {
      out += e;
    }
  }
  return out;
}

// A small reader for the registry's "servers" reply; the values are plain strings and numbers.
inline std::string JsonField(const std::string& obj, const char* key) {
  const std::string k = std::string("\"") + key + "\":";
  size_t at = obj.find(k);
  if (at == std::string::npos) return {};
  at += k.size();
  while (at < obj.size() && obj[at] == ' ') ++at;
  if (at < obj.size() && obj[at] == '"') return JsonString(obj, at);
  if (obj.compare(at, 4, "true") == 0) return "true";
  if (obj.compare(at, 5, "false") == 0) return "false";
  if (at < obj.size() && obj[at] == '[') {  // ["host", port]
    const size_t end = obj.find(']', at);
    std::string inner = obj.substr(at + 1, end - at - 1), out;
    for (char c : inner)
      if (c != '"' && c != ' ') out += c == ',' ? ':' : c;
    return out;
  }
  size_t end = at;
  while (end < obj.size() && (isdigit(static_cast<unsigned char>(obj[end])) || obj[end] == '-')) ++end;
  return obj.substr(at, end - at);
}

// One request to the registry at `target` (host:port); the reply's first line. The game's own registry when `target`
// is empty (bf_backend_registry, else bf_online_host's port 18250).
inline bool RegistryRequest(const std::string& request, std::string& reply, std::string target = {}) {
  if (target.empty()) target = REXCVAR_GET(bf_backend_registry);
  if (target.empty()) target = REXCVAR_GET(bf_online_host) + ":18250";
  const size_t colon = target.rfind(':');
  if (colon == std::string::npos) return false;
  addrinfo hints{}, *found = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(target.substr(0, colon).c_str(), target.substr(colon + 1).c_str(), &hints, &found) || !found)
    return false;
  auto s = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
  bool ok = false;
#if defined(_WIN32)
  if (s != INVALID_SOCKET) {
    DWORD timeout = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
  if (s >= 0) {
    timeval timeout{2, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    // Join tests with two games on one PC (tools\run_game.ps1 -PeerPad): the second one's own loopback address
    // (BF_LOOPBACK_ADDR), so the backend keys its reservation by the address its game logs in from.
    if (const char* own = std::getenv("BF_LOOPBACK_ADDR"); own && *own) {
      sockaddr_in local{};
      local.sin_family = AF_INET;
      if (inet_pton(AF_INET, own, &local.sin_addr) == 1)
        bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local));
    }
    if (connect(s, found->ai_addr, static_cast<int>(found->ai_addrlen)) == 0) {
      const std::string line = request + "\n";
      send(s, line.data(), static_cast<int>(line.size()), 0);
      char buffer[4096];
      int n;
      while ((n = recv(s, buffer, sizeof(buffer), 0)) > 0) {
        reply.append(buffer, n);
        if (reply.find('\n') != std::string::npos) {
          ok = true;
          break;
        }
      }
    }
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
  }
  freeaddrinfo(found);
  return ok;
}

// The backend's notice: maintenance mode's (from the list), or one refusal's (from reserve).
struct ServerNotice {
  std::string kind, title, text;
  bool active() const { return !text.empty(); }
};

// The join queue (0.4.7.4; TekRantGaming, 18 Nov 2026: "a server que system with a popup showing you your position in
// the que that updates there should also be a close button"; server/plasma.py QUEUE). A full server's place: reserve
// says "queue": 1, and a backend with the queue answers "queued" with a token, the place and how many wait. A backend
// from before it answers "full" as ever (the SERVER FULL notice).
struct QueuePlace {
  std::string gid, token;
  int position = 0, total = 0;
};

// The place's watch: asks the backend every 2 s on a thread of its own (its answers take up to 2 s each, so never in
// the menu's frame) whether the turn has come. Close gives the place up at once (queue_leave); a game that stops
// asking loses it on the backend after 15 s. A place the backend lost (it started again) is asked for again at the end.
class QueueWatch {
 public:
  enum class State { kIdle, kWaiting, kTurn, kRefused, kNoAnswer };
  struct Snapshot {
    State state = State::kIdle;
    QueuePlace place;
    ServerNotice refusal;  // kRefused: the backend's words (the server gone, maintenance, its queue full)
  };
  ~QueueWatch() { Stop(false); }
  void Start(const QueuePlace& place);
  // leave: the place is given up on the backend (sent on a thread of its own; nothing waits for it).
  void Stop(bool leave);
  Snapshot Get() {
    if (!shared_) return {};
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->snap;
  }

 private:
  struct Shared {
    std::mutex mutex;
    std::atomic<bool> stop{false};
    Snapshot snap;
  };
  static void Run(std::shared_ptr<Shared> s);
  std::shared_ptr<Shared> shared_;
};

class ServerList {
 public:
  ServerList() {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    thread_ = std::thread([this] { Run(); });
  }
  ~ServerList() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

  std::vector<ServerInfo> servers() {
    std::lock_guard<std::mutex> lock(mutex_);
    return servers_;
  }
  bool reachable() const { return reachable_; }
  int players_online() const { return players_online_; }
  // The version players need, from the list's last answer; empty: none asked (no gate, or the backend is older).
  std::string required_version() {
    std::lock_guard<std::mutex> lock(mutex_);
    return required_version_;
  }

  // 13 Nov 2026 ("The Game shouldn't try to connect to the master server unless they go to the server browser or
  // click Quick Play"): the list is asked for only while something wants it, and for 12 s after: the server browser,
  // Quick Play, the friends page, a match being played. Until then nothing is sent to the backend from here, and
  // asked() is false (the menus say so instead of "not reachable"). A test that joins by itself (BF_AUTO_JOIN) always
  // wants it.
  void Want() { wanted_until_ = Now() + 12.0; }
  bool asked() const { return answers_.load() > 0; }
  uint64_t answers() const { return answers_.load(); }  // the list's answers so far (a count)
  // The browser is open: ping the servers (until 3 s after it closes); RefreshNow fetches the list again now.
  void WantPings() { want_pings_until_ = Now() + 3.0, Want(); }
  void RefreshNow() { Want(), refresh_now_ = true; }
  // This run now uses another backend (a server's own: game_menu.cpp UseOwnBackend, a LAN match). The last answer's
  // version gate was the other backend's: forgotten, so a join until the new list answers asks the reservation only.
  void BackendChanged() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      required_version_.clear();
    }
    RefreshNow();
  }

  // Direct connect: the server at host:port (a name is looked up first) if the backend knows it: the game's own
  // backend, or the one whose registry is `registry` (host:port; a server on a backend of its own).
  bool Find(const std::string& host, int port, ServerInfo& out, const std::string& registry = {}) {
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) || !found) return false;
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr, ip, sizeof(ip));
    freeaddrinfo(found);
    std::string reply;
    if (!RegistryRequest("{\"op\": \"find\", \"address\": \"" + std::string(ip) + "\", \"port\": " +
                             std::to_string(port) + ", " + ClientFields() + "}",
                         reply, registry))
      return false;
    const size_t at = reply.find("{\"gid\"");
    if (at == std::string::npos) return false;
    out = Parse(reply.substr(at, JsonEnd(reply, at) - at + 1));
    if (out.gid.empty() || out.retired_mode) return false;
    // Its ping, as the list's (the far server warning, game_menu.cpp FarServerWarned, asks it of Direct connect too).
    out.ping_ms = PingOne(out.address);
    return true;
  }

  // One server's ping: two UDP pings to its host (one may be lost), the first answer within 1 s; -1 none.
  static int PingOne(const std::string& a) {
    const size_t colon = a.rfind(':');
    if (colon == std::string::npos) return -1;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(uint16_t(std::atoi(a.c_str() + colon + 1)));
    if (inet_pton(AF_INET, a.substr(0, colon).c_str(), &to.sin_addr) != 1) return -1;
    auto s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#if defined(_WIN32)
    if (s == INVALID_SOCKET) return -1;
#else
    if (s < 0) return -1;
#endif
    double sent[2] = {0, 0};
    for (uint32_t i = 0; i < 2; ++i) {
      char packet[16] = {'B', 'F', '4', '3', 'P', 'I', 'N', 'G'};
      const uint32_t id[2] = {0xD1EC7u, i};
      std::memcpy(packet + 8, id, 8);
      sent[i] = Now();
      sendto(s, packet, 16, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }
    int best = -1;
    const double until = Now() + 1.0;
    while (Now() < until) {
      fd_set read;
      FD_ZERO(&read);
      FD_SET(s, &read);
      timeval wait{0, long(std::max(0.0, until - Now()) * 1e6)};
      if (select(int(s) + 1, &read, nullptr, nullptr, &wait) <= 0) break;
      char reply[64];
      const int n = recvfrom(s, reply, sizeof(reply), 0, nullptr, nullptr);
      if (n < 16 || std::memcmp(reply, "BF43PONG", 8) != 0) continue;
      uint32_t id[2];
      std::memcpy(id, reply + 8, 8);
      if (id[0] != 0xD1EC7u || id[1] > 1) continue;
      const int ms = int(std::lround((Now() - sent[id[1]]) * 1000.0));
      best = ms;  // the first answer is enough
      break;
    }
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
    return best;
  }

  // The servers the backend at `host` lists (its registry on port 18250), for Direct connect to a server that runs
  // on its own backend (the server package's LAN only mode: such a server is not on the main backend's list).
  static bool ListAt(const std::string& host, std::vector<ServerInfo>& out) {
    std::string reply;
    if (!RegistryRequest("{\"op\": \"list\", " + ClientFields() + "}", reply, host + ":18250")) return false;
    for (size_t at = reply.find("{\"gid\""); at != std::string::npos;) {
      const size_t end = JsonEnd(reply, at);
      if (end == std::string::npos) break;
      ServerInfo sv = Parse(reply.substr(at, end - at + 1));
      if (!sv.retired_mode) out.push_back(std::move(sv));
      at = reply.find("{\"gid\"", end);
    }
    return reply.find("\"servers\"") != std::string::npos;
  }

  // Maintenance mode's notice (empty: none): the list's last answer's, or the last one known while the backend
  // cannot be reached, until its end time (or for 10 minutes without one).
  ServerNotice notice() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!reachable_ && notice_.active() && Now() > notice_until_) notice_ = {};
    return notice_;
  }

  // kOutdated: the backend's version gate refused this game; `required` (if given) gets the version players need.
  // kRefused: refused with words for the player (maintenance, a full server): `refusal` (if given) gets them.
  // kQueued (with `place`: the game waits in a full server's queue): `place` gets the place.
  enum class Reserved { kYes, kNo, kOutdated, kRefused, kQueued };
  static std::string ReserveRequest(const std::string& gid, bool queue) {
    return "{\"op\": \"reserve\", \"gid\": \"" + gid + "\", " + (queue ? "\"queue\": 1, " : "") + JoinFields() + "}";
  }
  static QueuePlace ReadPlace(const std::string& reply, const std::string& gid) {
    QueuePlace p;
    p.gid = JsonField(reply, "gid");
    if (p.gid.empty()) p.gid = gid;
    p.token = JsonField(reply, "token");
    p.position = std::atoi(JsonField(reply, "position").c_str());
    p.total = std::atoi(JsonField(reply, "total").c_str());
    return p;
  }
  Reserved Reserve(const std::string& gid, std::string* required = nullptr, ServerNotice* refusal = nullptr,
                   QueuePlace* place = nullptr) {
    std::string reply;
    if (!RegistryRequest(ReserveRequest(gid, place != nullptr), reply)) return Reserved::kNo;
    const std::string op = JsonField(reply, "op");
    if (op == "queued" && place) {  // a full server's queue (server/plasma.py QUEUE): the place
      *place = ReadPlace(reply, gid);
      return Reserved::kQueued;
    }
    if (op == "outdated") {
      if (required) *required = JsonField(reply, "required");
      return Reserved::kOutdated;
    }
    if (op != "reserved" && !JsonField(reply, "message").empty()) {
      if (refusal) *refusal = {op, JsonField(reply, "title"), JsonField(reply, "message")};
      if (op == "closed") {  // maintenance went on since the list's last answer: the list says it too, at once
        RefreshNow();
      }
      return Reserved::kRefused;
    }
    return reply.find("reserved") != std::string::npos ? Reserved::kYes : Reserved::kNo;
  }

 private:
  void Run() {
    // (For an hour on 13 Nov 2026 the list was asked for only while a page wanted it. Taken back the same day: "if
    // the master server is reachable, it should connect like before"; what must not need it is hosting a LAN match,
    // which has a backend of its own. Want() and asked() stay: the menus say Connecting until the first answer, and
    // Quick Play waits for it.)
    while (!stop_) {
      std::string reply;
      std::vector<ServerInfo> found;
      reachable_ = RegistryRequest("{\"op\": \"list\", " + ClientFields() + "}", reply);
      answers_.fetch_add(1);
      for (size_t at = reply.find("{\"gid\""); at != std::string::npos;) {
        const size_t end = JsonEnd(reply, at);
        if (end == std::string::npos) break;
        ServerInfo sv = Parse(reply.substr(at, end - at + 1));
        if (!sv.retired_mode) found.push_back(std::move(sv));
        at = reply.find("{\"gid\"", end);
      }
      if (Now() < want_pings_until_) Ping(found);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& s : found)  // the last ping, until this round's answer
          if (s.ping_ms < 0)
            for (const auto& old : servers_)
              if (old.gid == s.gid) s.ping_ms = old.ping_ms;
      }
      // The players signed in (the launcher's Home); an older backend has no count: the players in matches.
      int online = -1;
      if (const std::string v = JsonField(reply, "players_online"); !v.empty()) online = std::atoi(v.c_str());
      if (online < 0) {
        online = 0;
        for (const auto& s : found) online += s.players;
      }
      players_online_ = online;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        servers_ = std::move(found);
        if (reachable_) required_version_ = JsonField(reply, "required_version");
        if (reachable_) {
          // (The notice's fields come before "servers" in the answer: the first of each name is the notice's.)
          notice_ = {JsonField(reply, "notice_kind"), JsonField(reply, "notice_title"), JsonField(reply, "notice_text")};
          const int seconds = std::atoi(JsonField(reply, "notice_seconds").c_str());
          notice_until_ = Now() + (seconds > 0 ? seconds : 600);
        }
      }
      for (int i = 0; i < 50 && !stop_ && !refresh_now_.exchange(false); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  std::atomic<double> wanted_until_{0.0};
  std::atomic<uint64_t> answers_{0};
  static double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  static ServerInfo Parse(const std::string& obj) {
    ServerInfo s;
    s.gid = JsonField(obj, "gid");
    s.name = JsonField(obj, "name");
    s.address = JsonField(obj, "address");
    s.map = JsonField(obj, "map");
    s.retired_mode = JsonField(obj, "mode") == "modern";
    s.players = std::atoi(JsonField(obj, "players").c_str());
    s.max_players = std::atoi(JsonField(obj, "max_players").c_str());
    s.bots = std::max(0, std::atoi(JsonField(obj, "bots").c_str()));
    if (const std::string q = JsonField(obj, "queue"); !q.empty()) s.queue = std::atoi(q.c_str());
    s.description = JsonField(obj, "description");
    TakeDescriptionTags(s);
    s.next_map = JsonField(obj, "next_map");
    s.official = JsonField(obj, "official") == "true";
    s.promoted = s.official && JsonField(obj, "promoted") == "true";
    s.night = kNightOps && JsonField(obj, "night") == "true";  // night_ops.h: none without Night Ops
    s.daybreak = kNightOps && !s.night && JsonField(obj, "daybreak") == "true";  // the same build switch
    s.region = JsonField(obj, "region");
    s.tick_hz = std::atoi(JsonField(obj, "tick").c_str());
    return s;
  }

  // One UDP ping to each server's host; the answers within 1 s give each its time.
  void Ping(std::vector<ServerInfo>& servers) {
    auto s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#if defined(_WIN32)
    if (s == INVALID_SOCKET) return;
#else
    if (s < 0) return;
#endif
    std::vector<double> sent(servers.size(), 0);
    const uint32_t round = ++ping_round_;
    for (size_t i = 0; i < servers.size(); ++i) {
      const std::string& a = servers[i].address;
      const size_t colon = a.rfind(':');
      if (colon == std::string::npos) continue;
      sockaddr_in to{};
      to.sin_family = AF_INET;
      to.sin_port = htons(uint16_t(std::atoi(a.c_str() + colon + 1)));
      if (inet_pton(AF_INET, a.substr(0, colon).c_str(), &to.sin_addr) != 1) continue;
      char packet[16] = {'B', 'F', '4', '3', 'P', 'I', 'N', 'G'};
      const uint32_t id[2] = {round, uint32_t(i)};
      std::memcpy(packet + 8, id, 8);
      sent[i] = Now();
      sendto(s, packet, 16, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }
    const double until = Now() + 1.0;
    while (Now() < until) {
      fd_set read;
      FD_ZERO(&read);
      FD_SET(s, &read);
      const double left = until - Now();
      timeval wait{0, long(std::max(0.0, left) * 1e6)};
      if (select(int(s) + 1, &read, nullptr, nullptr, &wait) <= 0) break;
      char reply[64];
      const int n = recvfrom(s, reply, sizeof(reply), 0, nullptr, nullptr);
      if (n < 16 || std::memcmp(reply, "BF43PONG", 8) != 0) continue;
      uint32_t id[2];
      std::memcpy(id, reply + 8, 8);
      if (id[0] == round && id[1] < servers.size() && sent[id[1]] > 0)
        servers[id[1]].ping_ms = int(std::lround((Now() - sent[id[1]]) * 1000.0));
    }
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
  }

  std::thread thread_;
  std::atomic<bool> stop_{false}, reachable_{false}, refresh_now_{false};
  std::atomic<double> want_pings_until_{0};
  uint32_t ping_round_ = 0;
  std::atomic<int> players_online_{0};
  std::mutex mutex_;
  std::vector<ServerInfo> servers_;
  std::string required_version_;
  ServerNotice notice_;
  double notice_until_ = 0;
};

inline void QueueWatch::Start(const QueuePlace& place) {
  Stop(false);
  shared_ = std::make_shared<Shared>();
  shared_->snap.state = State::kWaiting;
  shared_->snap.place = place;
  std::thread([s = shared_] { Run(s); }).detach();
}

inline void QueueWatch::Stop(bool leave) {
  if (!shared_) return;
  shared_->stop = true;
  if (leave) {
    std::string token;
    {
      std::lock_guard<std::mutex> lock(shared_->mutex);
      token = shared_->snap.place.token;
    }
    std::thread([token] {
      std::string reply;
      const bool ok = RegistryRequest("{\"op\": \"queue_leave\", \"token\": \"" + token + "\"}", reply);
      REXLOG_INFO("BF join: the queue's place given up on the backend: {}", ok ? JsonField(reply, "op") : "no answer");
    }).detach();
  }
  shared_.reset();
}

inline void QueueWatch::Run(std::shared_ptr<Shared> s) {
  using Clock = std::chrono::steady_clock;
  auto answered = Clock::now();
  while (!s->stop) {
    for (int i = 0; i < 20 && !s->stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (s->stop) break;
    QueuePlace place;
    {
      std::lock_guard<std::mutex> lock(s->mutex);
      place = s->snap.place;
    }
    std::string reply;
    bool ok = RegistryRequest("{\"op\": \"queue\", \"gid\": \"" + place.gid + "\", \"token\": \"" + place.token + "\", " +
                                  JoinFields() + "}",
                              reply);
    std::string op = ok ? JsonField(reply, "op") : "";
    if (op == "queue_lost") {
      // The backend has no such place (it started again, or this game did not ask for 15 s): asked for again, at the
      // end of the queue (or straight in, when there is room now).
      REXLOG_WARN("BF join: the backend lost this game's place in the queue of {}: asked for again", place.gid);
      reply.clear();
      ok = RegistryRequest(ServerList::ReserveRequest(place.gid, true), reply);
      op = ok ? JsonField(reply, "op") : "";
    }
    if (s->stop) break;
    std::lock_guard<std::mutex> lock(s->mutex);
    if (!ok) {
      if (Clock::now() - answered > std::chrono::seconds(20)) s->snap.state = State::kNoAnswer;
      if (s->snap.state != State::kWaiting) break;
      continue;
    }
    answered = Clock::now();
    if (op == "queued") {
      s->snap.place = ServerList::ReadPlace(reply, place.gid);
    } else if (op == "reserved") {
      if (const std::string gid = JsonField(reply, "gid"); !gid.empty()) s->snap.place.gid = gid;
      s->snap.state = State::kTurn;
      break;
    } else {
      // Gone, maintenance, its queue full: the backend's words; anything else without words is said as gone.
      const std::string text = JsonField(reply, "message");
      s->snap.refusal = {op, JsonField(reply, "title"),
                         text.empty() ? "The server could not be joined. Pick another server." : text};
      if (s->snap.refusal.title.empty()) s->snap.refusal.title = "COULD NOT JOIN";
      s->snap.state = State::kRefused;
      break;
    }
  }
}

}  // namespace bf1943
