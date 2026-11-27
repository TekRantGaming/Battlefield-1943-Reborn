// Answer the Xbox LIVE base service (XAM app 0xFC) as a logged on console that knows EA's servers.
//
// Battlefield 1943 reached EA's servers through Xbox LIVE Server Platform: at start it asks
// XOnlineGetServiceInfo for two EA services, 0x45410004 and 0x45410805 ("EA" in the high word; the executable also
// has a "360FeslServiceId" setting), through XMsgInProcessCall(0xFC, 0x58007, service id, &XONLINE_SERVICE_INFO).
// The runtime answers X_ONLINE_E_LOGON_NOT_LOGGED_ON (0x80151802) like Xenia, the game then knows no EA server, and
// Multiplayer fails with reason 14 ("Failed to connect to Xbox LIVE") about 6 s after it is chosen.
//
// Answered here (messages as named in Xenia's xlivebase_app.cc):
//   0x00058007 XOnlineGetServiceInfo(id, &info)  -> the service at BF_ONLINE_HOST (default 127.0.0.1), port
//                                                   BF_ONLINE_PORT_<id in hex> or BF_ONLINE_PORT (default 0)
//   0x00058003 XLiveBaseLogonGetHR               -> XONLINE_S_LOGON_CONNECTION_ESTABLISHED
//   0x00058006 XLiveBaseGetNatType(&type)        -> open NAT (1)
// Everything else goes to the runtime. Each answer is logged the first time.
//
// The game then opens an Xbox secure connection to that address. On PC there is no Xbox security gateway: plain
// IP needs no key exchange, so
//   XNetConnect(caller, address)                  -> success
//   XNetGetConnectStatus(caller, address)         -> XNET_CONNECT_STATUS_CONNECTED (2)
// (the runtime stubs both: XNetConnect fails, the status stays pending).
// Peer to peer (a console hosted private match) gives each player's XNADDR through Theater; the game turns it into
// an address with the session key id, and back again for what it receives. The runtime stubs both to fail (the
// host then drops every joiner with "XNetXnAddrToInAddr failed", run duo8). Without a gateway the XNADDR's own IP
// is the address:
//   XNetXnAddrToInAddr(caller, &xnaddr, &xnkid, &address)   -> xnaddr.ina (the first 4 bytes)
//   XNetInAddrToXnAddr(caller, address, &xnaddr, &xnkid)    -> the XNADDR and key id seen with that address
//
// XONLINE_SERVICE_INFO: DWORD dwServiceID, IN_ADDR serviceIP (network order), WORD wServicePort, WORD wReserved.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <rex/logging.h>
#include <rex/ppc/context.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <dlfcn.h>
#include <netdb.h>
#endif

#include <rex/cvar.h>
REXCVAR_DECLARE(std::string, bf_online_host);  // game_menu.cpp: the main backend

namespace {

constexpr uint32_t kAppXLiveBase = 0xFC;
constexpr uint32_t kGetServiceInfo = 0x00058007;
constexpr uint32_t kLogonGetHR = 0x00058003;
constexpr uint32_t kGetNatType = 0x00058006;
constexpr uint32_t kLogonConnectionEstablished = 0x001510F0;  // XONLINE_S_LOGON_CONNECTION_ESTABLISHED

PPCFunc* RuntimeExport(const char* name) {
#if defined(_WIN32)
  static HMODULE runtime = GetModuleHandleA("rexruntime.dll");
  return reinterpret_cast<PPCFunc*>(GetProcAddress(runtime, name));
#else
  return reinterpret_cast<PPCFunc*>(dlsym(RTLD_NEXT, name));
#endif
}

uint32_t Load32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  uint8_t* p = base + address;
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

void Store16(uint8_t* base, uint32_t address, uint16_t value) {
  base[address] = uint8_t(value >> 8);
  base[address + 1] = uint8_t(value);
}

// The main backend (bf_online_host, or BF_ONLINE_HOST for tests): an IPv4 address or a name, resolved once, as a host
// order number; 127.0.0.1 when unset or not found.
uint32_t OnlineHost() {
  static const uint32_t host = [] {
    std::string v = REXCVAR_GET(bf_online_host);
    if (const char* e = std::getenv("BF_ONLINE_HOST"); e && *e) v = e;
    unsigned x[4];
    if (std::sscanf(v.c_str(), "%u.%u.%u.%u", &x[0], &x[1], &x[2], &x[3]) == 4 && x[0] < 256 && x[1] < 256 &&
        x[2] < 256 && x[3] < 256) {
      return uint32_t(x[0] << 24 | x[1] << 16 | x[2] << 8 | x[3]);
    }
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);  // counted: harmless when the runtime has it started already
#endif
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET;
    if (!v.empty() && getaddrinfo(v.c_str(), nullptr, &hints, &found) == 0 && found) {
      const uint32_t a = ntohl(reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr.s_addr);
      freeaddrinfo(found);
      REXLOG_INFO("BF online: the main backend {} is {}.{}.{}.{}", v, a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
      return a;
    }
    REXLOG_WARN("BF online: the main backend '{}' was not found, using 127.0.0.1", v);
    return 0x7F000001u;
  }();
  return host;
}

uint16_t OnlinePort(uint32_t service) {
  char name[40];
  std::snprintf(name, sizeof(name), "BF_ONLINE_PORT_%08X", service);
  for (const char* key : {static_cast<const char*>(name), "BF_ONLINE_PORT"}) {
    if (const char* v = std::getenv(key); v && *v) return static_cast<uint16_t>(std::strtoul(v, nullptr, 0));
  }
  return 0;
}

bool FirstTime(uint32_t key) {
  static std::mutex mutex;
  static std::set<uint32_t> seen;
  std::lock_guard<std::mutex> lock(mutex);
  return seen.insert(key).second;
}

}  // namespace

extern "C" REX_FUNC(__imp__XMsgInProcessCall) {
  const uint32_t app = ctx.r3.u32, message = ctx.r4.u32, param1 = ctx.r5.u32, param2 = ctx.r6.u32;
  if (app == kAppXLiveBase && message == kGetServiceInfo && param2) {
    const uint32_t host = OnlineHost();
    const uint16_t port = OnlinePort(param1);
    Store32(base, param2, param1);
    Store32(base, param2 + 4, host);
    Store16(base, param2 + 8, port);
    Store16(base, param2 + 10, 0);
    ctx.r3.u64 = 0;
    if (FirstTime(param1))
      REXLOG_INFO("BF online: service {:08X} is at {}.{}.{}.{} port {}", param1, host >> 24, (host >> 16) & 0xFF,
                  (host >> 8) & 0xFF, host & 0xFF, port);
    return;
  }
  if (app == kAppXLiveBase && message == kLogonGetHR) {
    ctx.r3.u64 = kLogonConnectionEstablished;
    if (FirstTime(message)) REXLOG_INFO("BF online: Xbox LIVE logon answered as connected");
    return;
  }
  if (app == kAppXLiveBase && message == kGetNatType && param1) {
    Store32(base, param1, 1);  // XONLINE_NAT_OPEN
    ctx.r3.u64 = 0;
    if (FirstTime(message)) REXLOG_INFO("BF online: NAT type answered as open");
    return;
  }
  static PPCFunc* original = RuntimeExport("__imp__XMsgInProcessCall");
  if (original) original(ctx, base);
}

extern "C" REX_FUNC(__imp__NetDll_XNetConnect) {
  const uint32_t address = ctx.r4.u32;
  ctx.r3.u64 = 0;
  if (FirstTime(0x10000000 ^ address))
    REXLOG_INFO("BF online: secure connection to {}.{}.{}.{} answered as made", address >> 24, (address >> 16) & 0xFF,
                (address >> 8) & 0xFF, address & 0xFF);
}

extern "C" REX_FUNC(__imp__NetDll_XNetGetConnectStatus) {
  (void)base;
  ctx.r3.u64 = 2;  // XNET_CONNECT_STATUS_CONNECTED
}

namespace {
// XNADDR (36 bytes) and XNKID (8 bytes) by address, from XNetXnAddrToInAddr, for the reverse lookup.
struct SecureAddress {
  uint8_t xnaddr[36];
  uint8_t xnkid[8];
};
std::mutex g_secure_mutex;
std::map<uint32_t, SecureAddress> g_secure;
}  // namespace

extern "C" REX_FUNC(__imp__NetDll_XNetXnAddrToInAddr) {
  const uint32_t xnaddr = ctx.r4.u32, xnkid = ctx.r5.u32, out = ctx.r6.u32;
  if (!xnaddr || !out) {
    ctx.r3.u64 = 0x80070057;  // E_INVALIDARG
    return;
  }
  const uint32_t address = Load32(base, xnaddr);
  {
    std::lock_guard<std::mutex> lock(g_secure_mutex);
    SecureAddress& entry = g_secure[address];
    std::memcpy(entry.xnaddr, base + xnaddr, 36);
    if (xnkid) std::memcpy(entry.xnkid, base + xnkid, 8);
  }
  Store32(base, out, address);
  ctx.r3.u64 = 0;
  if (FirstTime(0x20000000 ^ address))
    REXLOG_INFO("BF online: secure address {}.{}.{}.{} used as is", address >> 24, (address >> 16) & 0xFF,
                (address >> 8) & 0xFF, address & 0xFF);
}

extern "C" REX_FUNC(__imp__NetDll_XNetInAddrToXnAddr) {
  const uint32_t address = ctx.r4.u32, xnaddr = ctx.r5.u32, xnkid = ctx.r6.u32;
  std::lock_guard<std::mutex> lock(g_secure_mutex);
  auto it = g_secure.find(address);
  if (it == g_secure.end()) {
    ctx.r3.u64 = 0x80070057;  // not an address this console was given
    return;
  }
  if (xnaddr) std::memcpy(base + xnaddr, it->second.xnaddr, 36);
  if (xnkid) std::memcpy(base + xnkid, it->second.xnkid, 8);
  ctx.r3.u64 = 0;
}
