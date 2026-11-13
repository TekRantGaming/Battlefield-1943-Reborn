// Our Create Private Match (docs/MENUS-PLAN.md section 7). On the Xbox 360 the host's console asked EA for a game it
// hosted; on PC a match needs a dedicated server, so the player's game starts one of our own on this PC: the same
// bf1943.exe with --server, headless and hidden, on a loopback address of its own (127.0.0.11, as the server package's
// first server), with our GameManager host inside it (gm_host.cpp) on UDP 18300 (or the next free port). The host
// registers it with the main backend, not listed; our menu then joins it as the server browser joins a server
// (game_menu.cpp), and friends join it by Direct connect with the address its page copies. The server ends with the
// player's game (bf_gm_parent_pid) or when the player leaves the match.

#include "private_match.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "server_list.h"  // JsonField; winsock2.h

REXCVAR_DECLARE(bool, server);
REXCVAR_DECLARE(std::string, bf_backend_registry);
namespace bf1943 {
int TickRate();  // tick_rate.cpp
}
REXCVAR_DECLARE(std::string, bf_online_host);

namespace bf1943 {
namespace {
namespace fs = std::filesystem;

struct State {
  std::mutex mutex;
  fs::path game, user;
#if defined(_WIN32)
  HANDLE process = nullptr;
#endif
  uint16_t port = 0;
} g;

std::atomic<uint64_t> g_self{0};  // ip << 16 | port, network order; 0: none

fs::path MatchDir() { return g.user / "private_match"; }

const char* LevelOf(const std::string& key) {
  if (key == "guadalcanal") return "Levels/Guadal_Canal";
  if (key == "iwo") return "Levels/Iwo_Jima_s";
  if (key == "coral") return "Levels/Coral_sea";
  return "Levels/Wake_island_s";
}

#if defined(_WIN32)
// Test runs (BF_LOOPBACK_ONLY=1, tools\run_game.ps1): the match listens on 127.0.0.1 only, as the tests' own sockets,
// so Windows Firewall never asks on an unattended PC.
bool LoopbackOnly() {
  const char* v = std::getenv("BF_LOOPBACK_ONLY");
  return v && *v == '1';
}

// A UDP port nothing on this PC holds (18300, else the next nine): a server package's servers may run here too.
uint16_t FreePort() {
  for (uint16_t port = 18300; port < 18310; ++port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 18300;
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(LoopbackOnly() ? INADDR_LOOPBACK : INADDR_ANY);
    const bool free = bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0;
    closesocket(s);
    if (free) return port;
  }
  return 18300;
}

std::wstring Wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

// This process's environment for the server, without our test settings (BF_*: the developer pad, the join driver, a
// test's own loopback address), plus the server's own loopback address and a window that never takes the focus.
std::wstring ServerEnvironment() {
  std::wstring block;
  if (wchar_t* env = GetEnvironmentStringsW()) {
    for (const wchar_t* at = env; *at; at += wcslen(at) + 1) {
      const std::wstring entry = at;
      if (_wcsnicmp(entry.c_str(), L"BF_", 3) == 0 || _wcsnicmp(entry.c_str(), L"SDL_WINDOW_ACTIVATE_WHEN_SHOWN=", 31) == 0)
        continue;
      block += entry;
      block += L'\0';
    }
    FreeEnvironmentStringsW(env);
  }
  for (const wchar_t* add : {L"BF_LOOPBACK_ONLY=1", L"BF_LOOPBACK_ADDR=127.0.0.11", L"SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0"}) {
    block += add;
    block += L'\0';
  }
  block += L'\0';
  return block;
}

void StopLocked() {
  if (!g.process) return;
  TerminateProcess(g.process, 0);
  WaitForSingleObject(g.process, 5000);
  CloseHandle(g.process);
  g.process = nullptr;
  REXLOG_INFO("BF private match: our server stopped");
}
#endif

std::string Clean(const std::string& s) {
  std::string out;
  for (char c : s)
    if (c != '"' && c != '\\' && static_cast<unsigned char>(c) >= 0x20) out += c;
  return out.substr(0, 40);
}
}  // namespace

void NotePrivateMatchPaths(const fs::path& game, const fs::path& user) {
  std::lock_guard lock(g.mutex);
  g.game = game, g.user = user;
}

bool StartPrivateMatch(const PrivateMatchOptions& o, std::string* why) {
#if defined(_WIN32)
  std::lock_guard lock(g.mutex);
  StopLocked();
  if (g.game.empty() || g.user.empty()) {
    if (why) *why = "The game's folders are not known yet.";
    return false;
  }
  std::error_code ec;
  fs::create_directories(MatchDir() / "user", ec);
  fs::remove(MatchDir() / "status.json", ec);
  fs::remove(MatchDir() / "user" / "current_map.txt", ec);
  wchar_t exe[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
  std::string registry = REXCVAR_GET(bf_backend_registry);
  if (registry.empty()) registry = REXCVAR_GET(bf_online_host) + ":18250";
  if (o.lan) registry = "127.0.0.1:18250";  // the match's own backend, in its own process (lan_backend.cpp)
  std::string rotation;
  for (const auto& m : o.maps) rotation += (rotation.empty() ? "" : ",") + m;
  if (rotation.empty()) rotation = "wake";
  g.port = o.port ? o.port : FreePort();
  const std::string first = o.maps.empty() ? "wake" : o.maps.front();
  const fs::path dir = MatchDir();
  std::ostringstream args;
  args << "\"" << fs::path(exe).string() << "\""
       << " --game_data_root=\"" << g.game.string() << "\""
       << " --user_data_root=\"" << (dir / "user").string() << "\""
       << " --log_file=\"" << (dir / "server.log").string() << "\""
       << " --fullscreen=false --window_start=hidden --server"
       << " --cl=\"-dedicated -level " << LevelOf(first) << "\""
       << " --bf_map_rotation=" << rotation << " --bf_round_end_delay=45"
       // At the rate this game runs at: its own game joins it without starting again.
       << " --bf_tick_rate=" << (o.tick ? o.tick : TickRate())
       << " --bf_gm_backend=" << registry << " --bf_gm_listen=" << (LoopbackOnly() ? "127.0.0.1:" : "0.0.0.0:") << g.port
       << " --bf_gm_name=\"" << Clean(o.name.empty() ? std::string("Private match") : o.name) << "\""
       // Never in a test run: the router is the PC owner's (and a loopback host has nothing to forward).
       // Hosted from a player's game: said in the description's first words (server_list.h kPlayerHostTag).
       << " --bf_gm_description=\"[player host]\""
       << " --bf_gm_listed=" << (o.listed && !o.lan ? "true" : "false") << " --bf_gm_upnp=" << (o.upnp && !o.lan && !LoopbackOnly() ? "true" : "false")
       << (o.lan ? " --bf_lan_backend=true" : "")
       << (o.password.empty() ? std::string() : " --bf_gm_password=\"" + Clean(o.password) + "\"")
       << " --bf_gm_status_file=\"" << (dir / "status.json").string() << "\""
       << " --bf_gm_parent_pid=" << GetCurrentProcessId();
  std::wstring command = Wide(args.str());
  std::wstring environment = ServerEnvironment();
  const std::wstring work = fs::path(exe).parent_path().wstring();
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                      CREATE_UNICODE_ENVIRONMENT, environment.data(), work.c_str(),
                      &si, &pi)) {
    if (why) *why = fmt::format("The server could not start (error {}).", GetLastError());
    REXLOG_WARN("BF private match: the server did not start (error {})", GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  g.process = pi.hProcess;
  REXLOG_INFO("BF private match: our server started (PID {}, maps {}, UDP {}{})", pi.dwProcessId, rotation, g.port,
              o.lan ? ", a LAN match with its own backend" : o.upnp && !LoopbackOnly() ? ", router port asked" : "");
  return true;
#else
  if (why) *why = "Hosting a match needs Windows for now.";
  return false;
#endif
}

void StopPrivateMatch() {
  SetSelfHostedAddress(0, 0);
#if defined(_WIN32)
  std::lock_guard lock(g.mutex);
  StopLocked();
#endif
}

PrivateMatchStatus ReadPrivateMatchStatus() {
  PrivateMatchStatus st;
  fs::path file;
  {
    std::lock_guard lock(g.mutex);
#if defined(_WIN32)
    st.running = g.process && WaitForSingleObject(g.process, 0) == WAIT_TIMEOUT;
#endif
    st.port = g.port;
    file = MatchDir() / "status.json";
  }
  std::ifstream in(file);
  if (!in) return st;
  std::stringstream text;
  text << in.rdbuf();
  const std::string json = text.str();
  st.state = JsonField(json, "state");
  st.gid = JsonField(json, "gid");
  st.address = JsonField(json, "address");
  st.map = JsonField(json, "map");
  st.why = JsonField(json, "why");
  st.reachable = JsonField(json, "reachable");
  if (st.reachable.empty()) st.reachable = "null";
  st.players = std::atoi(JsonField(json, "players").c_str());
  return st;
}

void SetSelfHostedAddress(uint32_t ip, uint16_t port) {
  g_self = ip && port ? (uint64_t(ip) << 16 | port) : 0;
  if (ip && port) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&ip);
    REXLOG_INFO("BF private match: our game reaches its own match at {}.{}.{}.{}:{} through 127.0.0.1", b[0], b[1], b[2],
                b[3], ntohs(port));
  }
}

bool SelfHostedSend(uint32_t& ip, uint16_t port) {
  const uint64_t self = g_self.load(std::memory_order_relaxed);
  if (!self || uint16_t(self & 0xFFFF) != port || uint32_t(self >> 16) != ip) return false;
  ip = htonl(INADDR_LOOPBACK);
  return true;
}

bool SelfHostedReceive(uint32_t& ip, uint16_t port) {
  const uint64_t self = g_self.load(std::memory_order_relaxed);
  if (!self || uint16_t(self & 0xFFFF) != port || ip != htonl(INADDR_LOOPBACK)) return false;
  ip = uint32_t(self >> 16);
  return true;
}

// The address the network's other PCs reach this one at: the local address of a UDP socket pointed at a private
// address (nothing is sent), which is the adapter the network's traffic leaves by.
std::string LanAddress() {
#if defined(_WIN32)
  const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return {};
  std::string out;
  for (const char* target : {"192.168.255.254", "10.255.255.254", "172.31.255.254", "8.8.8.8"}) {
    sockaddr_in to{};
    to.sin_family = AF_INET, to.sin_port = htons(9);
    inet_pton(AF_INET, target, &to.sin_addr);
    sockaddr_in own{};
    int n = sizeof(own);
    char text[32] = {};
    if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) != 0 ||
        getsockname(s, reinterpret_cast<sockaddr*>(&own), &n) != 0)
      continue;
    inet_ntop(AF_INET, &own.sin_addr, text, sizeof(text));
    const std::string ip = text;
    if (ip.empty() || ip.rfind("127.", 0) == 0 || ip == "0.0.0.0") continue;
    // A private address first; a public one only when the PC has no other.
    const bool is_private = ip.rfind("192.168.", 0) == 0 || ip.rfind("10.", 0) == 0 || ip.rfind("172.", 0) == 0 ||
                            ip.rfind("169.254.", 0) == 0;
    if (out.empty() || is_private) out = ip;
    if (is_private) break;
  }
  closesocket(s);
  return out;
#else
  return {};
#endif
}

}  // namespace bf1943
