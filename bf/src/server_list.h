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

#pragma once

#include "night_ops.h"

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>

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

// This game's version and platform, as the fields of a registry request.
inline std::string ClientFields() {
#if defined(_WIN32)
  const char* platform = "windows";
#else
  const char* platform = "linux";
#endif
  return std::string("\"version\": \"") + BfVersion() + "\", \"platform\": \"" + platform + "\"";
}

int TickRate();  // tick_rate.cpp: this game's simulation rate (bf_tick_rate: 30, or 60)

struct ServerInfo {
  std::string gid, name, address, map;
  // A server of an older version that plays the retired Modern movement and aiming (modern_mode.cpp): never listed
  // and never found, since v0.4.0's game plays the original everywhere and would be pulled about on it.
  bool retired_mode = false;
  int players = 0, max_players = 0;
  std::string description, next_map;
  bool official = false;
  bool night = false;  // a Night Ops server: its maps drawn at night (NativeXRenderer D162) while playing there
  std::string region;  // where it is, a region code (regions.h); empty: not said (an older server)
  int tick_hz = 0;     // its tick rate; 0: not said
  // 0.4.1: a server is only listed, found and joined by a game simulating at its rate (a server that says none runs
  // the game's own 30): a 30 Hz game on a 60 Hz server, or the other way round, is pulled about by every correction.
  bool OurTick() const { return (tick_hz > 0 ? tick_hz : 30) == TickRate(); }
  int ping_ms = -1;  // -1: not measured (an older server does not answer)
  // 13 Nov 2026: hosted from a player's own game (Create Private Match), not a dedicated server: its description
  // begins with kPlayerHostTag (the backend passes descriptions through as they are), taken off here. The browser
  // draws a person before its name, a server rack before the others.
  bool player_hosted = false;
};
inline constexpr const char* kPlayerHostTag = "[player host]";

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

  // The browser is open: ping the servers (until 3 s after it closes); RefreshNow fetches the list again now.
  void WantPings() { want_pings_until_ = Now() + 3.0; }
  void RefreshNow() { refresh_now_ = true; }

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
    return !out.gid.empty() && !out.retired_mode && out.OurTick();
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
      if (!sv.retired_mode && sv.OurTick()) out.push_back(std::move(sv));
      at = reply.find("{\"gid\"", end);
    }
    return reply.find("\"servers\"") != std::string::npos;
  }

  // kOutdated: the backend's version gate refused this game; `required` (if given) gets the version players need.
  enum class Reserved { kYes, kNo, kOutdated };
  Reserved Reserve(const std::string& gid, std::string* required = nullptr) {
    std::string reply;
    if (!RegistryRequest("{\"op\": \"reserve\", \"gid\": \"" + gid + "\", " + ClientFields() + "}", reply)) return Reserved::kNo;
    if (JsonField(reply, "op") == "outdated") {
      if (required) *required = JsonField(reply, "required");
      return Reserved::kOutdated;
    }
    return reply.find("reserved") != std::string::npos ? Reserved::kYes : Reserved::kNo;
  }

 private:
  void Run() {
    while (!stop_) {
      std::string reply;
      std::vector<ServerInfo> found;
      reachable_ = RegistryRequest("{\"op\": \"list\", " + ClientFields() + "}", reply);
      for (size_t at = reply.find("{\"gid\""); at != std::string::npos;) {
        const size_t end = JsonEnd(reply, at);
        if (end == std::string::npos) break;
        ServerInfo sv = Parse(reply.substr(at, end - at + 1));
        if (!sv.retired_mode && sv.OurTick()) found.push_back(std::move(sv));
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
      }
      for (int i = 0; i < 50 && !stop_ && !refresh_now_.exchange(false); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

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
    s.description = JsonField(obj, "description");
    if (s.description.rfind(kPlayerHostTag, 0) == 0) {
      s.player_hosted = true;
      s.description.erase(0, std::strlen(kPlayerHostTag));
      while (!s.description.empty() && s.description.front() == ' ') s.description.erase(s.description.begin());
    }
    s.next_map = JsonField(obj, "next_map");
    s.official = JsonField(obj, "official") == "true";
    s.night = kNightOps && JsonField(obj, "night") == "true";  // night_ops.h: none without Night Ops
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
};

}  // namespace bf1943
