// The game's own rules for its sockets' addresses (SDK v0.11+ port, phase 4).
//
// Under v0.10 this file replaced the runtime's recv, send, recvfrom, sendto, getpeername, getsockname and
// WSAWaitForMultipleEvents (socket_overlapped.cpp its WSARecvFrom and WSAGetOverlappedResult), because the runtime did
// not set the guest's last socket error, swapped ports, stubbed getpeername and overlapped receives, and waited for
// "all" where the guest asked for "any". v0.11 does all of that itself, and our SDK copy fixes the two it still had
// wrong (getpeername, the wait type: sdk/src-bf/src/xam/xam_net.cpp). What stays is the game's, applied through the
// SDK's net hooks (rex/xam/net_hooks.h, our SDK patch) in every path that reaches the host socket:
// - sends to a match this PC hosts go through 127.0.0.1 (private_match.h), and a reply reads as from the public
//   address again;
// - players accept packets only from addresses they sent to (bf_net_only_from_server);
// - Linux: ports below 1024 move up by kHostPortShift on the host, and back for the guest;
// - test runs (BF_LOOPBACK_ONLY=1, tools\run_game.ps1) bind to 127.0.0.1, so Windows Firewall never asks.
// Addresses and ports below are the host's, in network byte order.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/xam/net_hooks.h>

#include "private_match.h"

REXCVAR_DEFINE_BOOL(bf_net_only_from_server, true, "BF1943",
                    "The game's network packets accepted only from addresses it sent to (the server it plays on); "
                    "off: from anyone, as before 0.2.3");
REXCVAR_DECLARE(bool, server);  // server_mode.cpp

namespace {

bool TraceOn() {
  static const bool on = [] {
    const char* v = std::getenv("BF_NET_TRACE");
    return v && *v && *v != '0';
  }();
  return on;
}

std::string FormatAddress(uint32_t ip, uint16_t port) {
  const auto* a = reinterpret_cast<const uint8_t*>(&ip);
  const auto* p = reinterpret_cast<const uint8_t*>(&port);
  return fmt::format("{}.{}.{}.{}:{}", a[0], a[1], a[2], a[3], p[0] << 8 | p[1]);
}

#if defined(BF_DEV_TOOLS)
// BF_DEV_SENDS=1 (tests): datagrams sent to each address, logged every 5 s (a dedicated server still sending to a
// player whose game is gone, or not). Counts the sends asked for (v0.10 counted the ones that succeeded).
void DevCountSend(uint32_t ip, uint16_t port) {
  static const bool sends = std::getenv("BF_DEV_SENDS") != nullptr;
  if (!sends) return;
  static std::mutex mutex;
  static std::map<std::string, uint32_t> counts;
  static auto since = std::chrono::steady_clock::now();
  std::lock_guard lock(mutex);
  ++counts[FormatAddress(ip, port)];
  const auto now = std::chrono::steady_clock::now();
  if (now - since > std::chrono::seconds(5)) {
    std::string line;
    for (const auto& [address, n] : counts) line += " " + address + " " + std::to_string(n);
    REXLOG_INFO("BF dev sends in 5 s:{}", line);
    counts.clear();
    since = now;
  }
}
#endif

}  // namespace

// Ports below 1024 moved on Linux (private_match.h).
namespace bf1943 {
namespace {
[[maybe_unused]] uint16_t Swap16(uint16_t v) { return uint16_t(v >> 8 | v << 8); }
[[maybe_unused]] bool LoopbackIp(uint32_t ip) { return reinterpret_cast<const uint8_t*>(&ip)[0] == 127; }
}  // namespace
uint16_t HostBindPort(uint16_t port) {
#if defined(_WIN32)
  return port;
#else
  const uint16_t p = Swap16(port);
  return p > 0 && p < 1024 ? Swap16(uint16_t(p + kHostPortShift)) : port;
#endif
}
uint16_t HostSendPort(uint32_t ip, uint16_t port) {
#if defined(_WIN32)
  (void)ip;
  return port;
#else
  return LoopbackIp(ip) ? HostBindPort(port) : port;
#endif
}
uint16_t GuestOwnPort(uint16_t port) {
#if defined(_WIN32)
  return port;
#else
  const uint16_t p = Swap16(port);
  return p > kHostPortShift && p < kHostPortShift + 1024 ? Swap16(uint16_t(p - kHostPortShift)) : port;
#endif
}
uint16_t GuestSourcePort(uint32_t ip, uint16_t port) {
#if defined(_WIN32)
  (void)ip;
  return port;
#else
  return LoopbackIp(ip) ? GuestOwnPort(port) : port;
#endif
}

// Packets only from where the game sent (TekRantGaming, 11 Nov 2026): a packet reaches the game only from an address
// and port it sent to in the last 10 minutes (the server it plays on: players always speak first), and any other is
// dropped before the game sees it (socket_overlapped.cpp's WSARecvFrom, recvfrom below).
// Players only: a dedicated server answers whoever reaches it. Addresses and ports in network order, the host's.
namespace {
struct SentTo {
  std::mutex mutex;
  std::map<uint64_t, std::chrono::steady_clock::time_point> when;
  uint64_t dropped = 0;
  std::chrono::steady_clock::time_point logged{};
};
SentTo& sent_to() {
  static SentTo s;
  return s;
}
uint64_t AddressKey(uint32_t ip, uint16_t port) { return uint64_t(ip) << 16 | port; }
constexpr auto kAnswerWindow = std::chrono::minutes(10);
}  // namespace

void NoteSent(uint32_t ip, uint16_t port) {
  SentTo& s = sent_to();
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(s.mutex);
  s.when[AddressKey(ip, port)] = now;
  if (s.when.size() > 4096) {
    for (auto it = s.when.begin(); it != s.when.end();) it = now - it->second > kAnswerWindow ? s.when.erase(it) : ++it;
  }
}

bool SenderAllowed(uint32_t ip, uint16_t port) {
  static const bool server = REXCVAR_GET(server);
  if (server || !REXCVAR_GET(bf_net_only_from_server)) return true;
  SentTo& s = sent_to();
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(s.mutex);
  const auto it = s.when.find(AddressKey(ip, port));
  if (it != s.when.end() && now - it->second < kAnswerWindow) return true;
  ++s.dropped;
  // The first one at once, then at most a line every 10 s while more come.
  if (s.dropped == 1 || now - s.logged > std::chrono::seconds(10)) {
    s.logged = now;
    const uint8_t* a = reinterpret_cast<const uint8_t*>(&ip);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&port);
    REXLOG_INFO("BF NET dropped {} packet(s) so far from addresses this game never sent to (this one from "
                "{}.{}.{}.{}:{})",
                s.dropped, a[0], a[1], a[2], a[3], p[0] << 8 | p[1]);
  }
  return false;
}
}  // namespace bf1943

namespace {

void OnSend(uint32_t& ip, uint16_t& port) {
  const uint32_t guest_ip = ip;
  const uint16_t guest_port = port;
  bf1943::SelfHostedSend(ip, port);        // a match this PC hosts: through 127.0.0.1
  port = bf1943::HostSendPort(ip, port);   // Linux: a moved port on this machine
  bf1943::NoteSent(ip, port);              // it may answer (SenderAllowed)
  if (TraceOn()) {  // where each send went (the first 200)
    static std::atomic<int> traced{0};
    if (traced.fetch_add(1) < 200)
      REXLOG_INFO("BF NET send to {}{}", FormatAddress(guest_ip, guest_port),
                  ip != guest_ip || port != guest_port ? " as " + FormatAddress(ip, port) : std::string());
  }
#if defined(BF_DEV_TOOLS)
  DevCountSend(guest_ip, guest_port);
#endif
}

bool OnReceive(uint32_t& ip, uint16_t& port) {
  if (!bf1943::SenderAllowed(ip, port)) return false;
  port = bf1943::GuestSourcePort(ip, port);  // Linux: a moved port here
  bf1943::SelfHostedReceive(ip, port);        // a match this PC hosts: its replies read as from its public address
  return true;
}

void OnBind(uint32_t& ip, uint16_t& port) {
  // Test runs (BF_LOOPBACK_ONLY=1): a bind to every interface goes to this client's loopback address instead,
  // BF_LOOPBACK_ADDR (join tests with two clients) or 127.0.0.1.
  static const bool loopback_only = [] {
    const char* v = std::getenv("BF_LOOPBACK_ONLY");
    return v && *v == '1';
  }();
  static const uint32_t loopback = [] {
    const char* v = std::getenv("BF_LOOPBACK_ADDR");
    unsigned a = 127, b = 0, c = 0, d = 1;
    if (v) std::sscanf(v, "%u.%u.%u.%u", &a, &b, &c, &d);
    uint32_t out = 0;
    auto* o = reinterpret_cast<uint8_t*>(&out);
    o[0] = uint8_t(a), o[1] = uint8_t(b), o[2] = uint8_t(c), o[3] = uint8_t(d);
    return out;
  }();
  const uint32_t guest_ip = ip;
  const uint16_t guest_port = port;
  if (loopback_only && ip == 0) ip = loopback;
  port = bf1943::HostBindPort(port);  // Linux: a port below 1024 moved up (private_match.h)
  if (port != guest_port) {
    const auto* g = reinterpret_cast<const uint8_t*>(&guest_port);
    const auto* h = reinterpret_cast<const uint8_t*>(&port);
    REXLOG_INFO("BF SOCKET bind: port {} is {} on this host (ports below 1024 need root)", g[0] << 8 | g[1],
                h[0] << 8 | h[1]);
  }
  REXLOG_INFO("BF SOCKET bind {}{}", FormatAddress(guest_ip, guest_port),
              ip != guest_ip ? " at " + FormatAddress(ip, port) : std::string());
}

void OnName(uint32_t& ip, uint16_t& port, bool peer) {
  port = peer ? bf1943::GuestSourcePort(ip, port) : bf1943::GuestOwnPort(port);
}

// Set when the program starts, before the title's first socket call (rexxam is loaded by then: the program links it).
const bool g_registered = [] {
  rex::xam::NetHooks hooks;
  hooks.send = OnSend;
  hooks.receive = OnReceive;
  hooks.bind = OnBind;
  hooks.name = OnName;
  rex::xam::SetNetHooks(hooks);
  return true;
}();

}  // namespace
