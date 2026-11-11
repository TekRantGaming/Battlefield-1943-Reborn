// Report player 1 as signed in to Xbox LIVE, layer by layer (playbook chapter 14, as in the Doritos Crash Course 2
// port's live_fix.cpp). Without this, Multiplayer in the main menu says "You need to be signed into Xbox LIVE to
// access Xbox LIVE game play features".
//
// The runtime signs a local profile in as "signed in locally" (1), denies every privilege, reports no network cable
// and a loopback title address. Battlefield 1943 calls these imports both through the XDK's thin wrappers
// (sub_827D57D8 sign in state, sub_827D7528 sign in info, sub_827D58F8 XUID, sub_824836B0 link status,
// sub_824836A0 title address) and directly from game code (sub_829473A0, sub_82484718), so the imports themselves
// are replaced here: the exe's definitions win over the runtime DLL's, and each one calls the DLL's original.
//
//   XamUserGetSigninState(user)                     -> 2 (signed in to LIVE)
//   XamUserGetSigninInfo(user, flags, info)         -> info state 2, XUSER_INFO_FLAG_LIVE_ENABLED, online XUID
//   XamUserGetXUID(user, type, &xuid)               -> online XUID: 0x0009 in the top 16 bits
//   XamUserCheckPrivilege(user, privilege, &result) -> granted
//   XNetGetEthernetLinkStatus(caller)               -> link active, 100 Mbps, full duplex
//   XNetGetTitleXnAddr(caller, &addr)               -> + Ethernet, DHCP, gateway, DNS, online
//   XNotifyGetNext(listener, id, &id, &param)       -> once per listener that covers the LIVE area:
//                 XN_LIVE_CONNECTIONCHANGED with XONLINE_S_LOGON_CONNECTION_ESTABLISHED. Without it, Multiplayer
//                 waits about 6 s and says "Failed to connect to Xbox LIVE" (no network call is made first).
//   XamNotifyCreateListener(areas, ...)             -> unchanged, but records which listeners cover LIVE
//
// Only a user the runtime already reports as signed in is changed. No connection to any service is made here.
//
// The player's own name and XUID (SetPlayerIdentity, from the settings file: game_menu.cpp): the runtime's profile
// is "User" with one fixed XUID on every PC, so every player logged in with the same name, the backend renamed all
// but the first, and a renamed player's game crashed joining a dedicated server (it finds itself in the roster by
// its gamertag: read of guest 0 at stage 5, TekRantGaming 6 Nov 2026, run hetzner-rename2). XamUserGetName and the sign in
// info's name give the player's name; the XUID's low word is the player's id.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include <rex/logging.h>
#include <rex/ppc/context.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

std::string g_player_name;  // empty: the runtime's own ("User")
uint32_t g_player_id = 0;   // 0: the runtime's own XUID

constexpr uint32_t kSignedInLocally = 1;
constexpr uint32_t kSignedInToLive = 2;

// The runtime DLL's own export of an import, the one our definition hides.
PPCFunc* Original(const char* name) {
#if defined(_WIN32)
  static HMODULE runtime = GetModuleHandleA("rexruntime.dll");
  auto* f = reinterpret_cast<PPCFunc*>(GetProcAddress(runtime, name));
#else
  auto* f = reinterpret_cast<PPCFunc*>(dlsym(RTLD_NEXT, name));
#endif
  if (!f) REXLOG_ERROR("BF LIVE: runtime export {} not found", name);
  return f;
}

#define CALL_ORIGINAL(name)                         \
  do {                                              \
    static PPCFunc* original = Original(#name);     \
    if (original) original(ctx, base);              \
  } while (0)

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

// Join tests on one PC (tools\run_game.ps1 -PeerPad): BF_LOOPBACK_ADDR is this client's own loopback address
// (127.0.0.2 for the peer), used for its title XNADDR and its binds (socket_fixes.cpp); its XUID gets that last
// byte added so the two players differ.
uint32_t LoopbackAddress() {
  static const uint32_t address = [] {
    const char* v = std::getenv("BF_LOOPBACK_ADDR");
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!v || std::sscanf(v, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0u;
    return a << 24 | b << 16 | c << 8 | d;
  }();
  return address;
}

// The high word of an XUID, made an online one.
void MakeOnlineXuid(uint8_t* base, uint32_t xuid) {
  const uint32_t high = Load32(base, xuid);
  if (high >> 16 != 0x0009) Store32(base, xuid, 0x00090000 | (high & 0xFFFF));
  if (g_player_id) Store32(base, xuid + 4, g_player_id + (LoopbackAddress() & 0xFF ? (LoopbackAddress() & 0xFF) - 1 : 0));
  else if (LoopbackAddress() & 0xFF) {
    static uint32_t low = Load32(base, xuid + 4) + ((LoopbackAddress() & 0xFF) - 1);
    Store32(base, xuid + 4, low);
  }
}

// The player's name into a guest char buffer of `size` bytes (NUL ended, the rest zero).
void StoreName(uint8_t* base, uint32_t address, uint32_t size) {
  if (g_player_name.empty() || !address || !size) return;
  std::memset(base + address, 0, size);
  std::memcpy(base + address, g_player_name.data(), std::min<size_t>(g_player_name.size(), size - 1));
}

void LogOnce(bool& logged, const char* layer) {
  if (!logged) REXLOG_INFO("BF LIVE: answering {} as signed in to Xbox LIVE", layer);
  logged = true;
}

}  // namespace

extern "C" REX_FUNC(__imp__XamUserGetSigninState) {
  CALL_ORIGINAL(__imp__XamUserGetSigninState);
  if (ctx.r3.u32 == kSignedInLocally) {
    ctx.r3.u64 = kSignedInToLive;
    static bool logged;
    LogOnce(logged, "sign in state");
  }
}

extern "C" REX_FUNC(__imp__XamUserGetSigninInfo) {
  const uint32_t info = ctx.r5.u32;
  CALL_ORIGINAL(__imp__XamUserGetSigninInfo);
  // X_USER_SIGNIN_INFO: xuid at +0, flags at +8, signin state at +12.
  if (ctx.r3.u32 != 0 || !info || Load32(base, info + 12) != kSignedInLocally) return;
  Store32(base, info + 12, kSignedInToLive);
  Store32(base, info + 8, Load32(base, info + 8) | 1);  // XUSER_INFO_FLAG_LIVE_ENABLED
  MakeOnlineXuid(base, info);
  StoreName(base, info + 24, 16);  // X_USER_SIGNIN_INFO: name[16] at +24
  static bool logged;
  LogOnce(logged, "sign in info");
}

extern "C" REX_FUNC(__imp__XamUserGetName) {
  const uint32_t user = ctx.r3.u32, buffer = ctx.r4.u32, size = ctx.r5.u32;
  CALL_ORIGINAL(__imp__XamUserGetName);
  if (ctx.r3.u32 != 0 || user != 0) return;
  StoreName(base, buffer, std::min<uint32_t>(size, 16));
}

extern "C" REX_FUNC(__imp__XamUserGetXUID) {
  const uint32_t user = ctx.r3.u32, xuid = ctx.r5.u32;
  CALL_ORIGINAL(__imp__XamUserGetXUID);
  if (ctx.r3.u32 != 0 || user != 0 || !xuid) return;
  MakeOnlineXuid(base, xuid);
  static bool logged;
  LogOnce(logged, "XUID");
}

extern "C" REX_FUNC(__imp__XamUserCheckPrivilege) {
  const uint32_t privilege = ctx.r4.u32, result = ctx.r5.u32;
  CALL_ORIGINAL(__imp__XamUserCheckPrivilege);
  if (ctx.r3.u32 != 0 || !result) return;
  Store32(base, result, 1);
  static bool logged;
  if (!logged) REXLOG_INFO("BF LIVE: granting privileges (first asked: {})", privilege);
  logged = true;
}

// XNET_ETHERNET_LINK_ACTIVE | XNET_ETHERNET_LINK_100MBPS | XNET_ETHERNET_LINK_FULL_DUPLEX
extern "C" REX_FUNC(__imp__NetDll_XNetGetEthernetLinkStatus) {
  CALL_ORIGINAL(__imp__NetDll_XNetGetEthernetLinkStatus);
  if (ctx.r3.u32 == 0) {
    ctx.r3.u64 = 0x01 | 0x02 | 0x08;
    static bool logged;
    LogOnce(logged, "network link");
  }
}

// XNET_GET_XNADDR_ETHERNET | _DHCP | _GATEWAY | _DNS | _ONLINE on top of what the runtime reports (STATIC), unless
// it is pending (0) or none (1).
extern "C" REX_FUNC(__imp__NetDll_XNetGetTitleXnAddr) {
  CALL_ORIGINAL(__imp__NetDll_XNetGetTitleXnAddr);
  if (ctx.r3.u32 > 1) {
    ctx.r3.u64 = ctx.r3.u32 | 0x02 | 0x08 | 0x20 | 0x40 | 0x80;
    // XNADDR: ina (IPv4, network order) at +0.
    if (LoopbackAddress() && ctx.r4.u32) Store32(base, ctx.r4.u32, LoopbackAddress());
    static bool logged;
    LogOnce(logged, "title address");
  }
}

namespace {
constexpr uint64_t kNotifyAreaLive = 0x2;                    // XNOTIFY_LIVE
constexpr uint32_t kLiveConnectionChanged = 0x02000001;      // XN_LIVE_CONNECTIONCHANGED
constexpr uint32_t kLogonConnectionEstablished = 0x001510F0; // XONLINE_S_LOGON_CONNECTION_ESTABLISHED
std::mutex g_listener_mutex;
std::set<uint32_t> g_live_listeners;  // listener handles still owed the "connection established" notice
}  // namespace

extern "C" REX_FUNC(__imp__XamNotifyCreateListener) {
  const uint64_t areas = ctx.r3.u64;
  CALL_ORIGINAL(__imp__XamNotifyCreateListener);
  if (ctx.r3.u32 && (areas & kNotifyAreaLive)) {
    std::lock_guard<std::mutex> lock(g_listener_mutex);
    g_live_listeners.insert(ctx.r3.u32);
  }
}

extern "C" REX_FUNC(__imp__XNotifyGetNext) {
  const uint32_t listener = ctx.r3.u32, filter = ctx.r4.u32, id_ptr = ctx.r5.u32, param_ptr = ctx.r6.u32;
  CALL_ORIGINAL(__imp__XNotifyGetNext);
  if (ctx.r3.u32 != 0 || !id_ptr || (filter != 0 && filter != kLiveConnectionChanged)) return;
  {
    std::lock_guard<std::mutex> lock(g_listener_mutex);
    if (!g_live_listeners.erase(listener)) return;
  }
  Store32(base, id_ptr, kLiveConnectionChanged);
  if (param_ptr) Store32(base, param_ptr, kLogonConnectionEstablished);
  ctx.r3.u64 = 1;
  REXLOG_INFO("BF LIVE: sent listener {:08X} the Xbox LIVE connection as established", listener);
}

namespace bf1943 {
// The player id the backend sees: the XUID's low word as MakeOnlineXuid writes it (game_menu.cpp's name check).
uint32_t OnlinePlayerId() {
  return g_player_id ? g_player_id + (LoopbackAddress() & 0xFF ? (LoopbackAddress() & 0xFF) - 1 : 0) : 0;
}
const std::string& PlayerName() { return g_player_name; }
uint32_t PlayerId() { return g_player_id; }
void SetPlayerIdentity(const std::string& name, uint32_t id) {
  g_player_name = name.substr(0, 15);
  g_player_id = id;
  REXLOG_INFO("BF LIVE: playing as {} (id {:08X})", g_player_name, id);
}
}  // namespace bf1943
