// Our GameManager host inside a dedicated server (bf_gm_backend set): the work of server/gmhost.py done in the
// server's own process, so a player's PC can host a match with nothing else installed (our Create Private Match,
// docs/MENUS-PLAN.md section 7; private_match.cpp starts the server). The same layers and messages as gmhost.py,
// ported line for line (its header comment and docs/02-dedicated-server-join.md): the EA tunnel, the link's reliable
// and unreliable sequence numbers, the game link's kinds, the GameManager hello, roster and join, and the game's own
// datagrams passed to the server's game socket (UDP 1003) and back. One thread: one UDP socket for every player
// (bf_gm_listen), one socket per player to the game server, and one TCP link to the backend's server registry (one JSON
// object per line, plasma.py's Registry: register and status out; registered, reachable, join and refused in).
//
// The server package keeps gmhost.py (the official key, the test delay). This host has neither: it is for a match a
// player's own game starts, not listed by default (friends join by Direct connect), and it ends with that game
// (bf_gm_parent_pid).

#define FD_SETSIZE 256  // up to 24 players' sockets to the game server, the players' socket, the backend's link

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "server_list.h"  // JsonField, JsonString; winsock2.h

#if defined(_WIN32)
#include <natupnp.h>
#include <oleauto.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#endif

REXCVAR_DECLARE(bool, server);          // server_mode.cpp
REXCVAR_DEFINE_STRING(bf_gm_backend, "", "BF1943",
                      "A dedicated server's own GameManager host: the backend's server registry (host:port). Empty: "
                      "none (the server package runs server/gmhost.py)");
REXCVAR_DEFINE_STRING(bf_gm_listen, "0.0.0.0:18300", "BF1943", "Where players' games reach our GameManager host (UDP)");
REXCVAR_DEFINE_STRING(bf_gm_name, "Private match", "BF1943", "The match's name in the backend's list");
REXCVAR_DEFINE_STRING(bf_gm_description, "", "BF1943", "The match's description in the server browser");
REXCVAR_DEFINE_BOOL(bf_gm_listed, false, "BF1943", "Listed in the server browser (else joined by Direct connect only)");
REXCVAR_DEFINE_INT32(bf_gm_max_players, 24, "BF1943", "The most players the backend sends to the match");
REXCVAR_DEFINE_STRING(bf_gm_status_file, "", "BF1943",
                      "The host's state as JSON for the game that started this server (registered, game, address, "
                      "reachable, players)");
REXCVAR_DEFINE_BOOL(bf_gm_upnp, false, "BF1943", "Ask the router to forward bf_gm_listen's port to this PC (UPnP)");
REXCVAR_DEFINE_STRING(bf_gm_region, "", "BF1943",
                      "A dedicated server's own GameManager host: where the server is, one of the region codes "
                      "(regions.h: gb, de, us-east...); the server browser shows its name and flag");
REXCVAR_DEFINE_INT32(bf_gm_tick, 30, "BF1943",
                     "A dedicated server's own GameManager host: the tick rate in Hz told to the server browser (the "
                     "game's own 30 today)");
REXCVAR_DEFINE_INT32(bf_gm_parent_pid, 0, "BF1943",
                     "End this server when that process (the player's game that started it) ends");

namespace bf1943 {
const char* BfVersion();  // pipeline_upload.cpp
int TickRate();           // tick_rate.cpp: the simulation rate this server runs at (bf_tick_rate)
std::vector<std::string> RotationTitles();  // server_rotation.cpp: the maps in turn, by title
std::string CurrentMapTitle();              // server_rotation.cpp: the one running now
}  // namespace bf1943

namespace {
using bf1943::JsonField;
using Clock = std::chrono::steady_clock;
using Bytes = std::string;  // binary data

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
void CloseSocket(Socket s) { close(s); }
#endif

constexpr int kVoice = 0, kLinkPort = 1, kControl = 7;
constexpr uint32_t kInit = 1, kConn = 2, kUnrel = 0x80, kData = 0x100;
constexpr uint8_t kStream = 6, kDatagram = 7, kSyncFlag = 0x40;
constexpr double kResendSeconds = 0.3;
const Bytes kPing = "BF43PING", kProbe = "BF43PRBE", kPong = "BF43PONG";

// ---- byte helpers (big endian; GameManager's integers have their sign bit flipped: value + 0x80, + 0x8000, ...)

void Put16(Bytes& b, uint32_t v) {
  b += char((v >> 8) & 0xFF);
  b += char(v & 0xFF);
}
void Put32(Bytes& b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) b += char((v >> s) & 0xFF);
}
void Put64(Bytes& b, uint64_t v) {
  for (int s = 56; s >= 0; s -= 8) b += char((v >> s) & 0xFF);
}
uint32_t Get16(const Bytes& b, size_t at) { return uint32_t(uint8_t(b[at])) << 8 | uint8_t(b[at + 1]); }
uint32_t Get32(const Bytes& b, size_t at) { return Get16(b, at) << 16 | Get16(b, at + 2); }
Bytes I8(int64_t v) { return Bytes(1, char((v + 0x80) & 0xFF)); }
Bytes I16(int64_t v) {
  Bytes b;
  Put16(b, uint32_t((v + 0x8000) & 0xFFFF));
  return b;
}
Bytes I32(int64_t v) {
  Bytes b;
  Put32(b, uint32_t((v + 0x80000000LL) & 0xFFFFFFFF));
  return b;
}
Bytes Text(const std::string& s) { return I32(int64_t(s.size())) + s; }
Bytes U16(uint32_t v) {
  Bytes b;
  Put16(b, v);
  return b;
}
Bytes U32(uint32_t v) {
  Bytes b;
  Put32(b, v);
  return b;
}
Bytes U64(uint64_t v) {
  Bytes b;
  Put64(b, v);
  return b;
}

// The id a tunnel goes by: a hash of the user id's decimal text (the module's own id, +0x10).
uint32_t TunnelIdent(uint32_t user_id) {
  uint32_t h = 0;
  for (char c : std::to_string(user_id)) h = (h >> 27) ^ (h << 5) ^ uint8_t(c);
  return h;
}

// An XNADDR: ina, inaOnline, wPortOnline, abEnet, abOnline (36 bytes), for an address in host order.
Bytes XnAddr(uint32_t ip) { return U32(ip) + Bytes(6, '\0') + Bytes(6, '\xCC') + Bytes(20, '\0'); }

// A roster player as a console host writes it (cap1: the host's own entry and the joiner's).
Bytes PlayerBlob(uint32_t pid, int slot, const std::string& name, uint64_t xuid, const Bytes& address, uint32_t port,
                 uint64_t stamp) {
  return I32(pid) + I32(slot) + I32(slot) + Text(name) + I8(0) + U64(stamp) + U64(xuid) + I8(0) + I8(2) + address +
         U16(port) + Bytes("\x00\x80", 2) + Bytes(7, '\0');
}

std::string Hex(const Bytes& b, size_t most = 40) {
  std::string out;
  for (size_t i = 0; i < b.size() && i < most; ++i) out += fmt::format("{:02X}", uint8_t(b[i]));
  return out;
}

Bytes Base64Decode(const std::string& in) {
  static const std::string kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  Bytes out;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=') break;
    const size_t v = kAlphabet.find(c);
    if (v == std::string::npos) continue;
    acc = acc << 6 | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += char((acc >> bits) & 0xFF);
    }
  }
  return out;
}

std::string JsonQuote(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') out += '\\', out += char(c);
    else if (c < 0x20 || c >= 0x7F) out += fmt::format("\\u{:04x}", c);
    else out += char(c);
  }
  return out + "\"";
}

bool ParseAddress(const std::string& text, sockaddr_in& out) {
  const size_t colon = text.rfind(':');
  if (colon == std::string::npos) return false;
  out = {};
  out.sin_family = AF_INET;
  out.sin_port = htons(uint16_t(std::atoi(text.c_str() + colon + 1)));
  return inet_pton(AF_INET, text.substr(0, colon).c_str(), &out.sin_addr) == 1;
}

std::string FormatAddress(const sockaddr_in& a) {
  char ip[32] = {};
  inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
  return fmt::format("{}:{}", ip, ntohs(a.sin_port));
}

bool SameAddress(const sockaddr_in& a, const sockaddr_in& b) {
  return a.sin_port == b.sin_port && a.sin_addr.s_addr == b.sin_addr.s_addr;
}

// A UDP socket bound to `address`, not blocking, that ignores Windows' unreachable reports: Windows answers an ICMP
// port unreachable for an earlier send (a player whose game closed) by failing the socket's next receive with
// WSAECONNRESET (gmhost.py's udp_socket, TekRantGaming's list of 9 Nov 2026, item 6).
Socket UdpSocket(const sockaddr_in& address) {
  Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kNoSocket) return s;
#if defined(_WIN32)
  for (DWORD code : {DWORD(0x9800000C), DWORD(0x9800000F)}) {  // SIO_UDP_CONNRESET, SIO_UDP_NETRESET (TTL expired)
    BOOL off = FALSE;
    DWORD got = 0;
    WSAIoctl(s, code, &off, sizeof(off), nullptr, 0, &got, nullptr, nullptr);
  }
  u_long nonblocking = 1;
  ioctlsocket(s, FIONBIO, &nonblocking);
#else
  fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
  if (bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    CloseSocket(s);
    return kNoSocket;
  }
  return s;
}

struct Player {
  uint32_t pid = 0;
  std::string name, ticket;
  uint64_t xuid = 0;
  Bytes address;  // its XNADDR (36 bytes)
  Clock::time_point expected_at{};
};

class Host;

// One player's link to us.
struct Peer {
  struct Unacked {
    uint32_t seq;
    Bytes payload;
    Clock::time_point sent;
  };
  enum class State { kLink, kRoster, kJoined };

  Host* host = nullptr;
  sockaddr_in addr{};
  uint32_t connident = 0;
  bool connected = false;
  bool peer_bound = false;    // we have the client's control id
  bool client_bound = false;  // the client says it has ours (its control's bound byte)
  uint32_t next_seq = kData;  // our next reliable seq
  uint32_t peer_ack = kData - 1;  // the last reliable seq we have from the client, in order
  uint32_t unrel = 0;
  std::vector<Unacked> unacked;
  std::optional<Player> player;
  State state = State::kLink;
  Socket server = kNoSocket;  // our socket to the dedicated server for this client
  Clock::time_point clock0 = Clock::now(), last_heard = Clock::now();
  bool rejoin = false;  // set by Link: the client linked again with a new connection id
  bool left = false;    // set by Message: the player left the game (dropped once its datagram is read)

  void Note(const std::string& text) const;
  Bytes Datagram(const std::vector<std::pair<int, Bytes>>& subs) const;
  void SendSubs(std::vector<std::pair<int, Bytes>> subs);
  std::pair<int, Bytes> LinkPacket(uint32_t seq, const Bytes& body) const {
    return {kLinkPort, U32(seq) + U32(seq >= kUnrel ? peer_ack : connident) + body};
  }
  Bytes Sync() const {
    const uint32_t now = uint32_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - clock0).count() & 0xFFFFFFFF);
    return U32(now) + U32(now) + U16(0);
  }
  void SendReliable(const Bytes& data, uint8_t kind = kStream, bool with_sync = false) {
    const Bytes payload = data + (with_sync ? Sync() : Bytes()) + Bytes(1, char(kind | (with_sync ? kSyncFlag : 0)));
    const uint32_t seq = next_seq++;
    unacked.push_back({seq, payload, Clock::now()});
    SendSubs({LinkPacket(seq, payload)});
  }
  void SendUnreliable(const Bytes& data, uint8_t kind = kDatagram) {
    const uint32_t seq = kUnrel + (unrel & 0x7F);
    ++unrel;
    SendSubs({LinkPacket(seq, data + Bytes(1, char(kind)))});
  }
  void SendAck() { SendSubs({LinkPacket(next_seq, Bytes())}); }
  void Gm(const Bytes& body) { SendReliable(Bytes(1, '\x01') + body); }  // a GameManager message (SendData flag 1)
  void Resend() {
    const auto now = Clock::now();
    for (auto& item : unacked) {
      if (std::chrono::duration<double>(now - item.sent).count() >= kResendSeconds) {
        item.sent = now;
        SendSubs({LinkPacket(item.seq, item.payload)});
      }
    }
  }
  void DropAcked(uint32_t ack) {
    unacked.erase(std::remove_if(unacked.begin(), unacked.end(), [&](const Unacked& u) { return u.seq <= ack; }),
                  unacked.end());
  }

  void Receive(const Bytes& data);
  void Link(const Bytes& sub);
  void Payload(const Bytes& p);
  void Message(const Bytes& m);
  void SendRoster();
  Bytes JoinerBlob(uint64_t stamp) const {
    return PlayerBlob(player->pid, 1, player->name, player->xuid, player->address, 1000, stamp);
  }
  void SendJoin() {
    const uint32_t pid = player->pid;
    Gm(I8(5) + I32(pid) + JoinerBlob(0xD85));
    Gm(I8(8) + I32(pid));
    static const Bytes kTail("\x80\x00\x00\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x04\x00\x80\x00\x00\x00\x00\x00"
                             "\x00\x00\x00",
                             25);
    Gm(I8(9) + I32(pid) + kTail);
    Gm(I8(0x15) + Bytes(1, '\x01') + I32(PeerHostPid()) + Bytes(1, '\0'));
    state = State::kJoined;
    Note("joined: game packets now go to the server");
  }
  uint32_t PeerHostPid() const;
  void ToServer(const Bytes& body);
  void FromServer(const Bytes& body) { SendUnreliable(Bytes(1, '\0') + body); }
};

class Host {
 public:
  // The registration's identity (the backend's reply) and where things are.
  uint32_t user_id = 0, pid = 0, ident = 0;
  uint64_t xuid = 0;
  std::string name;
  sockaddr_in listen{}, public_addr{}, game_server{};
  Socket sock = kNoSocket;
  std::map<std::string, Player> players;  // ticket -> player, from the backend's join notices
  std::vector<std::unique_ptr<Peer>> peers;
  Clock::time_point pruned_at = Clock::now();

  void Note(const std::string& text) const { REXLOG_INFO("BF GM host: {}", text); }

  void Expect(const std::string& ticket, uint32_t player_pid, const std::string& player_name, uint64_t player_xuid,
              const std::string& address_b64) {
    Bytes address = Base64Decode(address_b64).substr(0, 36);
    if (address.size() != 36) address = XnAddr(0);
    Player p;
    p.pid = player_pid, p.name = player_name, p.xuid = player_xuid, p.address = address, p.ticket = ticket;
    p.expected_at = Clock::now();
    players[ticket] = p;
  }

  Peer* Find(const sockaddr_in& a) {
    for (auto& p : peers)
      if (SameAddress(p->addr, a)) return p.get();
    return nullptr;
  }
  Peer* Add(const sockaddr_in& a) {
    auto p = std::make_unique<Peer>();
    p->host = this, p->addr = a;
    peers.push_back(std::move(p));
    return peers.back().get();
  }
  void Drop(const sockaddr_in& a) {
    for (auto it = peers.begin(); it != peers.end(); ++it) {
      if (SameAddress((*it)->addr, a)) {
        if ((*it)->server != kNoSocket) CloseSocket((*it)->server);
        peers.erase(it);
        return;
      }
    }
  }
  void DropAll() {
    for (auto& p : peers)
      if (p->server != kNoSocket) CloseSocket(p->server);
    peers.clear();
  }

  void Datagram(const Bytes& data, const sockaddr_in& from) {
    // The server browser's ping and the backend's reachability probe (SERVERS-PLAN.md sections 5 and 6): answered at
    // once, never a game packet (those start with a flag byte, 0 or 1).
    if (data.compare(0, 8, kPing) == 0 || data.compare(0, 8, kProbe) == 0) {
      const Bytes reply = kPong + data.substr(8, 32);
      sendto(sock, reply.data(), int(reply.size()), 0, reinterpret_cast<const sockaddr*>(&from), sizeof(from));
      return;
    }
    Peer* peer = Find(from);
    if (!peer) {
      peer = Add(from);
      peer->Note("new client");
    }
    peer->Receive(data);
    if (peer->rejoin) {
      // A connected client's GameManager linking again with a new connection id (it left the game and joins again
      // from the same address and port): its old session is dropped and a fresh one takes the datagram.
      peer->Note("linking again with a new connection: its old session is dropped");
      Drop(from);
      peer = Add(from);
      peer->Note("new client (rejoin)");
      peer->Receive(data);
    }
    if (peer->left) Drop(from);
  }

  int PlayerCount() const {
    int n = 0;
    for (const auto& p : peers) n += p->state == Peer::State::kJoined;
    return n;
  }
  std::vector<std::string> PlayerNames() const {
    std::vector<std::string> out;
    for (const auto& p : peers)
      if (p->state == Peer::State::kJoined && p->player && out.size() < 64) out.push_back(p->player->name);
    return out;
  }

  void Tick() {
    const auto now = Clock::now();
    std::vector<sockaddr_in> silent;
    for (auto& p : peers) {
      p->Resend();
      if (std::chrono::duration<double>(now - p->last_heard).count() > 30) silent.push_back(p->addr);
    }
    for (const auto& a : silent) {
      if (Peer* p = Find(a)) p->Note("silent for 30 s, dropped");
      Drop(a);
    }
    // A join the backend announced whose client never linked (its game failed to connect): forgotten after 5 minutes.
    if (std::chrono::duration<double>(now - pruned_at).count() > 10) {
      pruned_at = now;
      for (auto it = players.begin(); it != players.end();) {
        if (std::chrono::duration<double>(now - it->second.expected_at).count() > 300) it = players.erase(it);
        else ++it;
      }
    }
  }
};

void Peer::Note(const std::string& text) const { host->Note(FormatAddress(addr) + " " + text); }
uint32_t Peer::PeerHostPid() const { return host->pid; }

Bytes Peer::Datagram(const std::vector<std::pair<int, Bytes>>& subs) const {
  Bytes heads, bodies;
  for (const auto& [port, d] : subs) {
    Put16(heads, uint32_t(d.size() << 4 | uint32_t(port)));
    bodies += d;
  }
  const Bytes body = heads + bodies;
  return U16(uint32_t(body.size())) + body;
}

void Peer::SendSubs(std::vector<std::pair<int, Bytes>> subs) {
  if (!client_bound)
    subs.insert(subs.begin(), {kControl, Bytes(1, '\0') + U32(host->ident) + Bytes(1, char(peer_bound ? 1 : 0))});
  const Bytes d = Datagram(subs);
  sendto(host->sock, d.data(), int(d.size()), 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
}

void Peer::Receive(const Bytes& data) {
  last_heard = Clock::now();
  if (data.size() < 4) return;
  std::vector<std::pair<size_t, int>> heads;  // (length, port index)
  size_t at = 2, total = 0;
  while (at + 2 <= data.size()) {
    const uint32_t h = Get16(data, at);
    heads.push_back({h >> 4, int(h & 15)});
    total += h >> 4;
    at += 2;
    if (at + total >= data.size()) break;
  }
  for (const auto& [n, port] : heads) {
    const Bytes sub = at < data.size() ? data.substr(at, n) : Bytes();
    at += n;
    if (port == kControl && sub.size() >= 6) {
      if (!peer_bound) {
        peer_bound = true;
        Note(fmt::format("tunnel: client id {:08X}", Get32(sub, 1)));
      }
      if (sub[5] && !client_bound) {
        client_bound = true;
        Note("tunnel bound both ways");
      }
    } else if (port == kLinkPort && sub.size() >= 8) {
      Link(sub);
      if (rejoin || left) return;
    }
    // port 0: voice chat (COd connect, PNG ping, DSC): no voice server here
  }
}

void Peer::Link(const Bytes& sub) {
  const uint32_t seq_word = Get32(sub, 0), ack = Get32(sub, 4);
  const uint32_t seq = seq_word & 0x0FFFFFFF;
  Bytes body = sub.substr(8);
  // The seq's top nibble counts older payloads appended for redundancy, each followed by its size byte (the payload
  // for seq - k at index k).
  std::vector<Bytes> parts;
  for (uint32_t i = 0; i < (seq_word >> 28); ++i) {
    if (body.empty()) break;
    const size_t n = uint8_t(body.back());
    const size_t start = n + 1 > body.size() ? 0 : body.size() - 1 - n;
    parts.insert(parts.begin(), body.substr(start, body.size() - 1 - start));
    body.resize(start);
  }
  parts.insert(parts.begin(), body);
  if (seq == kInit) {
    if (connected && ack != connident) {
      // A new link from a client we had: the old session's sequence numbers and its socket to the server are stale
      // (TekRantGaming could not rejoin after leaving a game: every INIT was answered, every packet after it dropped).
      rejoin = true;
      return;
    }
    connident = ack;
    Note(fmt::format("link INIT {:08X}", ack));
    SendSubs({{kLinkPort, U32(kConn) + U32(connident)}});
    if (!connected) {
      connected = true;
      SendReliable(Bytes(), 0, true);  // the host's first data packet: a sync (cap1)
    }
    return;
  }
  if (seq == kConn) {
    connected = true;
    return;
  }
  if (seq >= kData) {
    DropAcked(ack);
    bool got = false;
    for (size_t k = parts.size(); k-- > 0;) {
      if (seq - uint32_t(k) == peer_ack + 1 && !parts[k].empty()) {
        ++peer_ack;
        got = true;
        Payload(parts[k]);
      }
    }
    if (got) SendAck();
  } else if (seq >= kUnrel) {
    DropAcked(ack);
    if (!parts[0].empty()) Payload(parts[0]);
  }
}

void Peer::Payload(const Bytes& p) {
  uint8_t kind = uint8_t(p.back());
  Bytes data = p.substr(0, p.size() - 1);
  if (kind & kSyncFlag) data = data.size() > 10 ? data.substr(0, data.size() - 10) : Bytes();
  kind &= 0x3F;
  if (data.empty()) return;
  const uint8_t flag = uint8_t(data[0]);
  const Bytes body = data.substr(1);
  if (kind == kStream && flag == 1) Message(body);
  else if (kind == kDatagram) ToServer(body);
  else Note(fmt::format("game link kind {} flag {}: {}", kind, flag, Hex(body)));
}

void Peer::Message(const Bytes& m) {
  if (m.empty()) return;
  const int t = int(uint8_t(m[0])) - 0x80;
  if (t == 0) {  // hello: u16 version, the ticket as text
    if (m.size() < 7) return;
    const size_t n = size_t(Get32(m, 3) - 0x80000000u);
    const std::string ticket = m.substr(7, std::min(n, m.size() - 7));
    auto it = host->players.find(ticket);
    if (it == host->players.end()) {
      Note("hello with unknown ticket " + ticket);
      return;
    }
    player = it->second;
    host->players.erase(it);
    Note(fmt::format("hello from {} (PID {})", player->name, player->pid));
    SendRoster();
  } else if (t == 4) {  // roster ack
    Note("roster ack");
    if (player) SendJoin();
  } else if (t == 1) {  // the player leaves the game
    Note("left the game");
    left = true;  // dropped by Host::Datagram once this datagram is read
  } else {
    Note(fmt::format("GameManager message {}: {}", t, Hex(m)));
  }
}

void Peer::SendRoster() {
  const Bytes host_blob = PlayerBlob(host->pid, 0, host->name, host->xuid, XnAddr(ntohl(host->public_addr.sin_addr.s_addr)),
                                     6001, 0xB86);
  const Bytes game = I8(2) + I16(0x14) + Text(host->name) + I16(0) + I8(1) + I8(1) + I8(1) + Bytes(2, '\0') +
                     Bytes("\0\0\x01\0", 4) + I8(0) + Bytes(1, '\x01') + Bytes("\x00\x10\x01\x00\x00\x00", 6) +
                     Bytes(8, '\0') + U16(2) + Bytes(1, '\x01') + host_blob;
  Gm(game);
  Gm(I8(3) + JoinerBlob(0xD83));
  state = State::kRoster;
}

void Peer::ToServer(const Bytes& body) {
  if (server == kNoSocket) {
    sockaddr_in local = host->game_server;
    local.sin_port = 0;  // its own port on the game server's loopback address
    server = UdpSocket(local);
    if (server == kNoSocket) {
      Note("no socket to the game server");
      return;
    }
  }
  const Bytes d = U16(uint32_t(body.size())) + body;  // its game socket takes VDP datagrams (u16 length first)
  sendto(server, d.data(), int(d.size()), 0, reinterpret_cast<const sockaddr*>(&host->game_server),
         sizeof(host->game_server));
}

// ---- the backend's link (one JSON object per line) and the state file for the game that started this server

struct Link {
  Socket s = kNoSocket;
  std::string in;
  bool Send(const std::string& line) {
    const std::string out = line + "\n";
    size_t at = 0;
    while (at < out.size()) {
      const int n = send(s, out.data() + at, int(out.size() - at), 0);
      if (n <= 0) return false;
      at += size_t(n);
    }
    return true;
  }
  // The next whole line, waiting up to `ms`; false when the link closed or nothing came.
  bool ReadLine(std::string& line, int ms) {
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
      const size_t nl = in.find('\n');
      if (nl != std::string::npos) {
        line = in.substr(0, nl);
        in.erase(0, nl + 1);
        return true;
      }
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
      if (left <= 0) return false;
      fd_set read;
      FD_ZERO(&read);
      FD_SET(s, &read);
      timeval tv{long(left / 1000), long((left % 1000) * 1000)};
      if (select(int(s) + 1, &read, nullptr, nullptr, &tv) <= 0) return false;
      if (!Fill()) return false;
    }
  }
  bool Fill() {
    char buffer[4096];
    const int n = recv(s, buffer, sizeof(buffer), 0);
    if (n <= 0) return false;
    in.append(buffer, size_t(n));
    return in.size() < (1 << 20);
  }
};

Socket Connect(const std::string& target) {
  const size_t colon = target.rfind(':');
  if (colon == std::string::npos) return kNoSocket;
  addrinfo hints{}, *found = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(target.substr(0, colon).c_str(), target.substr(colon + 1).c_str(), &hints, &found) || !found)
    return kNoSocket;
  Socket s = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
  if (s != kNoSocket && connect(s, found->ai_addr, int(found->ai_addrlen)) != 0) {
    CloseSocket(s);
    s = kNoSocket;
  }
  freeaddrinfo(found);
  return s;
}

struct Status {
  std::string state = "starting", gid, address, why, map;
  std::string reachable = "null";  // true, false, or null (not known yet, or a server on the backend's own machine)
  bool listed = false;
  int players = 0;
};

void WriteStatus(const Status& st) {
  const std::string path = REXCVAR_GET(bf_gm_status_file);
  if (path.empty()) return;
  const std::string json = fmt::format(
      "{{\"state\": {}, \"gid\": {}, \"listed\": {}, \"address\": {}, \"reachable\": {}, \"players\": {}, \"map\": {}, "
      "\"why\": {}, \"at\": {}}}",
      JsonQuote(st.state), JsonQuote(st.gid), st.listed ? "true" : "false", JsonQuote(st.address), st.reachable,
      st.players, JsonQuote(st.map), JsonQuote(st.why),
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  std::error_code ec;
  const std::filesystem::path p(path), tmp = p.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << json;
  }
  std::filesystem::rename(tmp, p, ec);
}

#if defined(_WIN32)
// The router's port for this match (UPnP, Windows' own NAT traversal object, as the server package's
// start_servers.ps1): asked once at the start, on its own thread (the router's discovery takes seconds).
void OpenRouterPort(uint16_t port, std::string local_ip) {
  std::thread([port, local_ip] {
    static const GUID kClsidUPnPNAT = {0xAE1E00AA, 0x3FD5, 0x403C, {0x8A, 0x27, 0x2B, 0xBD, 0xC3, 0x0C, 0xD0, 0xE1}};
    static const GUID kIidIUPnPNAT = {0xB171C812, 0xCC76, 0x485A, {0x94, 0xD8, 0xB6, 0xB3, 0xA2, 0x79, 0x4E, 0x99}};
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUPnPNAT* nat = nullptr;
    IStaticPortMappingCollection* mappings = nullptr;
    std::string result = "no UPnP router answered";
    if (SUCCEEDED(CoCreateInstance(kClsidUPnPNAT, nullptr, CLSCTX_ALL, kIidIUPnPNAT, reinterpret_cast<void**>(&nat))) &&
        nat && SUCCEEDED(nat->get_StaticPortMappingCollection(&mappings)) && mappings) {
      const std::wstring client(local_ip.begin(), local_ip.end());
      BSTR protocol = SysAllocString(L"UDP"), internal = SysAllocString(client.c_str()),
           description = SysAllocString(L"Battlefield 1943 Reborn private match");
      IStaticPortMapping* mapping = nullptr;
      const HRESULT hr = mappings->Add(port, protocol, port, internal, VARIANT_TRUE, description, &mapping);
      result = SUCCEEDED(hr) ? fmt::format("UDP {} forwarded to {}", port, local_ip)
                             : fmt::format("the router refused UDP {} (0x{:08X})", port, uint32_t(hr));
      if (mapping) mapping->Release();
      SysFreeString(protocol), SysFreeString(internal), SysFreeString(description);
      mappings->Release();
    }
    if (nat) nat->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    REXLOG_INFO("BF GM host: router port (UPnP): {}", result);
  }).detach();
}

// The address this PC uses toward `to` (its LAN address behind a router): what UPnP forwards the port to.
std::string LocalAddressToward(const std::string& target) {
  sockaddr_in to{};
  if (!ParseAddress(target, to)) {
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET;
    const size_t colon = target.rfind(':');
    if (getaddrinfo(target.substr(0, colon).c_str(), nullptr, &hints, &found) || !found) return {};
    to = *reinterpret_cast<sockaddr_in*>(found->ai_addr);
    to.sin_port = htons(18250);
    freeaddrinfo(found);
  }
  Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kNoSocket) return {};
  std::string out;
  sockaddr_in me{};
  int size = sizeof(me);
  if (connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 0 &&
      getsockname(s, reinterpret_cast<sockaddr*>(&me), &size) == 0) {
    char ip[32] = {};
    inet_ntop(AF_INET, &me.sin_addr, ip, sizeof(ip));
    out = ip;
  }
  CloseSocket(s);
  return out;
}

// bf_gm_parent_pid: this server ends with the player's game that started it (a match nobody hosts any more).
void WatchParent(uint32_t pid) {
  HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!parent) {
    REXLOG_WARN("BF GM host: the game that started this server (PID {}) is not running: the server ends", pid);
    TerminateProcess(GetCurrentProcess(), 0);
    return;
  }
  std::thread([parent, pid] {
    WaitForSingleObject(parent, INFINITE);
    REXLOG_INFO("BF GM host: the game that started this server (PID {}) ended: the server ends", pid);
    TerminateProcess(GetCurrentProcess(), 0);
  }).detach();
}
#endif

// One registration with the backend, then the match until the backend's link ends (then registered again, as
// gmhost.py's serve()).
void Serve() {
  const std::string backend = REXCVAR_GET(bf_gm_backend);
  Host host;
  if (!ParseAddress(REXCVAR_GET(bf_gm_listen), host.listen)) {
    REXLOG_ERROR("BF GM host: bf_gm_listen {} is not an address", REXCVAR_GET(bf_gm_listen));
    return;
  }
  host.public_addr = host.listen;
  host.name = REXCVAR_GET(bf_gm_name);
  // The dedicated server's game socket: its own loopback address (BF_LOOPBACK_ADDR, private_match.cpp), port 1003.
  {
    const char* v = std::getenv("BF_LOOPBACK_ADDR");
    ParseAddress(std::string(v && *v ? v : "127.0.0.1") + ":1003", host.game_server);
  }
  Status st;
  st.listed = REXCVAR_GET(bf_gm_listed);
  bool upnp_asked = false;
  for (;;) {
    Link link;
    link.s = Connect(backend);
    if (link.s == kNoSocket) {
      st.state = "no backend", st.why = backend;
      WriteStatus(st);
      REXLOG_WARN("BF GM host: backend {} not reachable, retrying", backend);
      std::this_thread::sleep_for(std::chrono::seconds(5));
      continue;
    }
    // Listening before the backend is told: its reachability probe comes at once.
    host.sock = UdpSocket(host.listen);
    if (host.sock == kNoSocket) {
      st.state = "port in use", st.why = REXCVAR_GET(bf_gm_listen);
      WriteStatus(st);
      REXLOG_ERROR("BF GM host: cannot listen on {} (another server there?)", REXCVAR_GET(bf_gm_listen));
      CloseSocket(link.s);
      std::this_thread::sleep_for(std::chrono::seconds(5));
      continue;
    }
#if defined(_WIN32)
    // A host on a loopback address (tests) has nothing for a router to forward.
    if (REXCVAR_GET(bf_gm_upnp) && !upnp_asked && (ntohl(host.listen.sin_addr.s_addr) >> 24) != 127) {
      upnp_asked = true;
      const std::string local = LocalAddressToward(backend);
      if (!local.empty()) OpenRouterPort(ntohs(host.listen.sin_port), local);
    }
#endif
    std::vector<std::string> titles = bf1943::RotationTitles();
    std::string current = bf1943::CurrentMapTitle();
    if (current.empty()) current = titles.empty() ? "Wake Island" : titles[0];
    std::string maps;
    for (const auto& t : titles) maps += (maps.empty() ? "" : ", ") + JsonQuote(t);
    // "mode": every server plays the game's own movement and aiming since v0.4.0 (Modern was retired); the field
    // stays for the players still on an older version, whose browsers list servers by it. "version": this server's.
    std::string reg = fmt::format(
        "{{\"op\": \"register\", \"name\": {}, \"port\": {}, \"map\": {}, \"max_players\": {}, \"mode\": \"classic\", "
        "\"version\": {}, \"region\": {}, \"tick\": {}, \"description\": {}, \"maps\": [{}], \"listed\": {}",
        JsonQuote(host.name), ntohs(host.public_addr.sin_port), JsonQuote(current), REXCVAR_GET(bf_gm_max_players),
        JsonQuote(bf1943::BfVersion()), JsonQuote(REXCVAR_GET(bf_gm_region).substr(0, 16)), bf1943::TickRate(),
        JsonQuote(REXCVAR_GET(bf_gm_description).substr(0, 200)), maps, st.listed ? "true" : "false");
    // A host on one address of its own (tests) says where; one on every address is found by the backend.
    if (host.listen.sin_addr.s_addr != htonl(INADDR_ANY)) {
      char ip[32] = {};
      inet_ntop(AF_INET, &host.public_addr.sin_addr, ip, sizeof(ip));
      reg += fmt::format(", \"address\": [\"{}\", {}]", ip, ntohs(host.public_addr.sin_port));
    }
    reg += "}";
    std::string reply;
    if (!link.Send(reg) || !link.ReadLine(reply, 15000)) {
      REXLOG_WARN("BF GM host: the backend did not answer the registration, retrying");
      CloseSocket(host.sock), CloseSocket(link.s);
      std::this_thread::sleep_for(std::chrono::seconds(5));
      continue;
    }
    if (JsonField(reply, "op") == "refused") {
      st.state = "refused", st.why = JsonField(reply, "why");
      WriteStatus(st);
      REXLOG_WARN("BF GM host: the backend refused the server: {}", st.why);
      CloseSocket(host.sock), CloseSocket(link.s);
      std::this_thread::sleep_for(std::chrono::seconds(30));
      continue;
    }
    host.user_id = uint32_t(std::strtoul(JsonField(reply, "uid").c_str(), nullptr, 10));
    host.pid = uint32_t(std::strtoul(JsonField(reply, "pid").c_str(), nullptr, 10));
    host.ident = TunnelIdent(host.user_id);
    host.xuid = 0x0009000000000000ull + host.user_id;
    st.state = "registered", st.gid = JsonField(reply, "gid"), st.address = JsonField(reply, "address");
    st.listed = JsonField(reply, "listed") != "false", st.reachable = "null", st.why.clear(), st.map = current;
    WriteStatus(st);
    host.Note(fmt::format("listening on {} for game server {}; registered with the backend as game {}{}",
                          FormatAddress(host.listen), FormatAddress(host.game_server), st.gid,
                          st.listed ? "" : " (not listed: Direct connect only)"));

    auto beat_at = Clock::now(), tick_at = Clock::now();
    bool alive = true;
    std::vector<char> buffer(65536);  // a datagram of any size, as gmhost.py reads them
    while (alive) {
      fd_set read;
      FD_ZERO(&read);
      FD_SET(link.s, &read);
      FD_SET(host.sock, &read);
      Socket top = std::max(link.s, host.sock);
      for (const auto& p : host.peers) {
        if (p->server == kNoSocket) continue;
        FD_SET(p->server, &read);
        top = std::max(top, p->server);
      }
      timeval tv{0, 50000};
      const int ready = select(int(top) + 1, &read, nullptr, nullptr, &tv);
      if (ready > 0 && FD_ISSET(host.sock, &read)) {
        for (int i = 0; i < 256; ++i) {  // everything queued, not one datagram a wake
          sockaddr_in from{};
          socklen_t size = sizeof(from);
          const int n = recvfrom(host.sock, buffer.data(), int(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from),
                                 &size);
          if (n <= 0) break;
          host.Datagram(Bytes(buffer.data(), size_t(n)), from);
        }
      }
      if (ready > 0) {
        for (size_t i = 0; i < host.peers.size(); ++i) {
          Peer* p = host.peers[i].get();
          if (p->server == kNoSocket || !FD_ISSET(p->server, &read)) continue;
          for (int k = 0; k < 256; ++k) {
            const int n = recv(p->server, buffer.data(), int(buffer.size()), 0);
            if (n < 2) break;
            const size_t length = Get16(Bytes(buffer.data(), 2), 0);
            p->FromServer(Bytes(buffer.data() + 2, std::min(length, size_t(n) - 2)));
          }
        }
      }
      if (ready > 0 && FD_ISSET(link.s, &read)) {
        if (!link.Fill()) {
          host.Note("backend link lost");
          alive = false;
        }
        std::string line;
        while (alive && link.ReadLine(line, 0)) {
          const std::string op = JsonField(line, "op");
          if (op == "reachable") {
            // null: it answers, but it runs on the backend's own machine, so whether players outside reach it is
            // not known (the backend's probe skips the machine's firewall)
            const std::string ok = JsonField(line, "ok");
            st.reachable = ok == "true" ? "true" : ok == "false" ? "false" : "null";
            WriteStatus(st);
            host.Note(fmt::format("backend: {} at {}",
                                  ok == "true"    ? "players can reach this server"
                                  : ok == "false" ? "players cannot reach this server"
                                                  : "this server answers on the backend's own machine",
                                  JsonField(line, "address")));
          } else if (op == "join") {
            host.Expect(JsonField(line, "ticket"), uint32_t(std::strtoul(JsonField(line, "pid").c_str(), nullptr, 10)),
                        JsonField(line, "name"), std::strtoull(JsonField(line, "xuid").c_str(), nullptr, 10),
                        JsonField(line, "xnaddr"));
            host.Note(fmt::format("backend: {} joins (PID {})", JsonField(line, "name"), JsonField(line, "pid")));
          }
        }
      }
      const auto now = Clock::now();
      if (now - tick_at >= std::chrono::milliseconds(100)) {
        tick_at = now;
        host.Tick();
      }
      if (alive && now - beat_at >= std::chrono::seconds(5)) {
        beat_at = now;
        // The map now running (server_rotation.cpp) and the next of the rotation.
        current = bf1943::CurrentMapTitle();
        if (current.empty()) current = st.map;
        if (current != st.map) host.Note("map now " + current);
        st.map = current;
        titles = bf1943::RotationTitles();
        const auto it = std::find(titles.begin(), titles.end(), current);
        const std::string upcoming = it == titles.end() ? "" : titles[size_t((it - titles.begin() + 1)) % titles.size()];
        std::string names;
        for (const auto& n : host.PlayerNames()) names += (names.empty() ? "" : ", ") + JsonQuote(n);
        st.players = host.PlayerCount();
        if (!link.Send(fmt::format("{{\"op\": \"status\", \"players\": {}, \"map\": {}, \"next_map\": {}, \"names\": [{}]}}",
                                   st.players, JsonQuote(current), JsonQuote(upcoming), names))) {
          host.Note("backend link lost (status)");
          alive = false;
        }
        WriteStatus(st);
      }
    }
    host.DropAll();
    host.players.clear();
    CloseSocket(host.sock), CloseSocket(link.s);
    host.sock = kNoSocket;
    st.state = "reconnecting";
    WriteStatus(st);
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
}
}  // namespace

namespace bf1943 {
bool StartLanBackend();  // lan_backend.cpp: a LAN match's own backend (bf_lan_backend), before this host registers
// From the dedicated server's tick (server_rotation.cpp): our GameManager host starts once the server runs its level,
// when bf_gm_backend is set.
void GmHostStart() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (!REXCVAR_GET(server) || REXCVAR_GET(bf_gm_backend).empty()) return;
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    if (const int parent = REXCVAR_GET(bf_gm_parent_pid); parent > 0) WatchParent(uint32_t(parent));
#endif
    REXLOG_INFO("BF GM host: starting (backend {}, players reach {}, {})", REXCVAR_GET(bf_gm_backend),
                REXCVAR_GET(bf_gm_listen), REXCVAR_GET(bf_gm_listed) ? "listed" : "not listed");
    StartLanBackend();
    std::thread(Serve).detach();
  });
}
}  // namespace bf1943
