// Our Create Private Match (docs/MENUS-PLAN.md section 7). On the Xbox 360 the host's console asked EA for a game it
// hosted; on PC a match needs a dedicated server, so the player's game starts one of our own on this PC: the same
// bf1943.exe with --server, headless and hidden, on a loopback address of its own (127.0.0.11, as the server package's
// first server), with our GameManager host inside it (gm_host.cpp) on UDP 18300 (or the next free port). The host
// registers it with the main backend, not listed; our menu then joins it as the server browser joins a server
// (game_menu.cpp), and friends join it by Direct connect with the address its page copies. The server ends with the
// player's game (bf_gm_parent_pid) or when the player leaves the match.

#include <random>

#include "private_match.h"
#include "world_sync.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "server_list.h"  // JsonField; winsock2.h

#if !defined(_WIN32)
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>

extern char** environ;
#endif

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
#else
  pid_t pid = 0;
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

// Test runs (BF_LOOPBACK_ONLY=1, tools\run_game.ps1): the match listens on 127.0.0.1 only, as the tests' own sockets,
// so Windows Firewall never asks on an unattended PC.
bool LoopbackOnly() {
  const char* v = std::getenv("BF_LOOPBACK_ONLY");
  return v && *v == '1';
}

// A UDP port nothing on this PC holds (18300, else the next nine): a server package's servers may run here too.
uint16_t FreePort() {
  for (uint16_t port = 18300; port < 18310; ++port) {
#if defined(_WIN32)
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 18300;
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
    const int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return 18300;
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(LoopbackOnly() ? INADDR_LOOPBACK : INADDR_ANY);
    const bool free = bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0;
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
    if (free) return port;
  }
  return 18300;
}

// The server's command line after the program, for both platforms. `quoted`: the value goes in quotes on Windows'
// one line (paths and names with spaces); Linux passes each as its own argument and needs none.
struct Arg {
  std::string flag, value;
  bool quoted = false;
};

#if defined(_WIN32)

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
      const bool test_setting = _wcsnicmp(entry.c_str(), L"BF_", 3) == 0;
#if defined(BF_DEV_TOOLS)
      // A server's own test aid (BF_DEV_MOVE_SPAWNS: the vehicles' night lamps test hosts a LAN match whose US carrier
      // spawns are moved to the airfield's flag; BF_BOTS and BF_BOT_*: the bots' tests) goes to the server too.
      const bool for_server = _wcsnicmp(entry.c_str(), L"BF_DEV_MOVE_SPAWNS", 18) == 0 ||
                              _wcsnicmp(entry.c_str(), L"BF_BOTS=", 8) == 0 ||
                              _wcsnicmp(entry.c_str(), L"BF_BOT_", 7) == 0;
#else
      const bool for_server = false;
#endif
      if ((test_setting && !for_server) || _wcsnicmp(entry.c_str(), L"SDL_WINDOW_ACTIVATE_WHEN_SHOWN=", 31) == 0)
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

std::wstring CommandLine(const std::string& exe, const std::vector<Arg>& args) {
  std::string line = "\"" + exe + "\"";
  for (const Arg& a : args) {
    line += " " + a.flag;
    if (!a.value.empty() || a.quoted) line += "=" + (a.quoted ? "\"" + a.value + "\"" : a.value);
  }
  return Wide(line);
}
#else
// This program, as the kernel started it (an AppImage's run copy included: its libraries' path is in the environment
// we pass on).
std::string OwnExecutable() {
  std::error_code ec;
  const fs::path p = fs::read_symlink("/proc/self/exe", ec);
  return ec ? std::string() : p.string();
}

// As Windows' ServerEnvironment: ours without the test settings, plus the server's loopback address.
std::vector<std::string> ServerEnvironment() {
  std::vector<std::string> env;
  for (char** at = environ; at && *at; ++at) {
    const std::string entry = *at;
    if (entry.rfind("BF_", 0) == 0 || entry.rfind("SDL_WINDOW_ACTIVATE_WHEN_SHOWN=", 0) == 0) continue;
    env.push_back(entry);
  }
  for (const char* add : {"BF_LOOPBACK_ONLY=1", "BF_LOOPBACK_ADDR=127.0.0.11", "SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0"})
    env.push_back(add);
  return env;
}

bool Running() {
  if (g.pid <= 0) return false;
  int status = 0;
  const pid_t r = waitpid(g.pid, &status, WNOHANG);
  if (r == 0) return true;
  if (r == g.pid) {
    REXLOG_INFO("BF private match: our server ended ({} {})", WIFSIGNALED(status) ? "signal" : "exit code",
                WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
    g.pid = 0;
  }
  return false;
}

// SIGTERM, up to 5 s (as Windows' wait), then SIGKILL; always reaped.
void StopLocked() {
  if (g.pid <= 0) return;
  kill(g.pid, SIGTERM);
  for (int i = 0; i < 50; ++i) {
    if (waitpid(g.pid, nullptr, WNOHANG) == g.pid) {
      g.pid = 0;
      REXLOG_INFO("BF private match: our server stopped");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  kill(g.pid, SIGKILL);
  waitpid(g.pid, nullptr, 0);
  g.pid = 0;
  REXLOG_INFO("BF private match: our server stopped (killed after 5 s)");
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
#if defined(_WIN32)
  wchar_t exe_w[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, exe_w, DWORD(std::size(exe_w)));
  const std::string exe = fs::path(exe_w).string();
  const uint32_t own_pid = GetCurrentProcessId();
#else
  const std::string exe = OwnExecutable();
  if (exe.empty()) {
    if (why) *why = "The game could not find its own program to start the server.";
    return false;
  }
  const uint32_t own_pid = uint32_t(getpid());
#endif
  std::string registry = REXCVAR_GET(bf_backend_registry);
  if (registry.empty()) registry = REXCVAR_GET(bf_online_host) + ":18250";
  if (o.lan) registry = "127.0.0.1:18250";  // the match's own backend, in its own process (lan_backend.cpp)
  std::string rotation;
  for (const auto& m : o.maps) rotation += (rotation.empty() ? "" : ",") + m;
  if (rotation.empty()) rotation = "wake";
  g.port = o.port ? o.port : FreePort();
  const std::string first = o.maps.empty() ? "wake" : o.maps.front();
  const fs::path dir = MatchDir();
  // A key of this match's own: the server takes this game, which shows it, for an admin of its world.
  std::string admin_key;
  {
    std::random_device rd;
    for (int i = 0; i < 4; ++i) admin_key += fmt::format("{:08x}", uint32_t(rd()));
  }
  world::SetAdminKey(admin_key);
  std::vector<Arg> args = {
      {"--game_data_root", g.game.string(), true},
      {"--user_data_root", (dir / "user").string(), true},
      {"--log_file", (dir / "server.log").string(), true},
      {"--fullscreen", "false"},
      {"--window_start", "hidden"},
      {"--server", ""},
      {"--cl", std::string("-dedicated -level ") + LevelOf(first), true},
      {"--bf_map_rotation", rotation},
      {"--bf_round_end_delay", "45"},
      // At the rate this game runs at: its own game joins it without starting again.
      {"--bf_tick_rate", std::to_string(o.tick ? o.tick : TickRate())},
      {"--bf_bots", std::to_string(std::clamp(o.bots, 0, 24))},
      {"--bf_gm_backend", registry},
      {"--bf_gm_listen", (LoopbackOnly() ? "127.0.0.1:" : "0.0.0.0:") + std::to_string(g.port)},
      {"--bf_gm_name", Clean(o.name.empty() ? std::string("Private match") : o.name), true},
      // Never in a test run: the router is the PC owner's (and a loopback host has nothing to forward).
      // Hosted from a player's game: said in the description's first words (server_list.h kPlayerHostTag).
      {"--bf_gm_description", "[player host]", true},
      {"--bf_gm_listed", o.listed && !o.lan ? "true" : "false"},
      {"--bf_gm_upnp", o.upnp && !o.lan && !LoopbackOnly() ? "true" : "false"},
  };
  if (o.lan) args.push_back({"--bf_lan_backend", "true"});
  if (!o.password.empty()) args.push_back({"--bf_gm_password", Clean(o.password), true});
  const auto number = [](float v) {  // as a stream writes it (the command line before Linux hosting)
    std::ostringstream out;
    out << v;
    return out.str();
  };
  for (Arg a : std::initializer_list<Arg>{
           {"--bf_world_day_night", o.day_night ? "true" : "false"},
           {"--bf_world_time", number(o.time)},
           {"--bf_world_day_minutes", number(o.day_minutes)},
           {"--bf_world_cloud_cover", number(o.cloud_cover)},
           {"--bf_world_cloud_rate", number(o.cloud_rate)},
           {"--bf_world_weather", number(o.weather)},
           {"--bf_world_fog", number(o.fog)},
           {"--bf_world_ground_fog", number(o.ground_fog)},
           {"--bf_world_wind", number(o.wind)},
           {"--bf_world_cloud_height", number(o.cloud_height)},
           {"--bf_world_dynamic_storm", o.dynamic_storm ? "true" : "false"},
           {"--bf_world_storm_type", std::to_string(std::clamp(o.storm_type, 0, 1))},  // D224
           // The page's "map's own" (0) is the level's own count, never scaled (-1; match_rules.cpp).
           {"--bf_match_tickets", o.tickets > 0 ? std::to_string(std::min(o.tickets, 9999)) : std::string("-1")},
           {"--bf_match_time_limit", std::to_string(std::clamp(o.time_limit, 0, 600))},
           {"--bf_world_admin_key", admin_key},
           {"--bf_gm_status_file", (dir / "status.json").string(), true},
           {"--bf_gm_parent_pid", std::to_string(own_pid)},
       })
    args.push_back(std::move(a));
#if defined(_WIN32)
  std::wstring command = CommandLine(exe, args);
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
  const uint32_t server_pid = pi.dwProcessId;
#else
  std::vector<std::string> argv_text = {exe};
  for (const Arg& a : args) argv_text.push_back(a.value.empty() && !a.quoted ? a.flag : a.flag + "=" + a.value);
  std::vector<std::string> env_text = ServerEnvironment();
  std::vector<char*> argv, envp;
  for (auto& t : argv_text) argv.push_back(t.data());
  for (auto& t : env_text) envp.push_back(t.data());
  argv.push_back(nullptr), envp.push_back(nullptr);
  // In the program's folder (as Windows' working folder), its output to /dev/null (it has its own log file).
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addchdir_np(&actions, fs::path(exe).parent_path().c_str());
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  pid_t pid = 0;
  const int err = posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  if (err != 0) {
    if (why) *why = fmt::format("The server could not start ({}).", std::strerror(err));
    REXLOG_WARN("BF private match: the server did not start ({})", std::strerror(err));
    return false;
  }
  g.pid = pid;
  const uint32_t server_pid = uint32_t(pid);
#endif
  REXLOG_INFO("BF private match: our server started (PID {}, maps {}, UDP {}{})", server_pid, rotation, g.port,
              o.lan ? ", a LAN match with its own backend" : o.upnp && !LoopbackOnly() ? ", router port asked" : "");
  return true;
}

void StopPrivateMatch() {
  SetSelfHostedAddress(0, 0);
  std::lock_guard lock(g.mutex);
  StopLocked();
}

PrivateMatchStatus ReadPrivateMatchStatus() {
  PrivateMatchStatus st;
  fs::path file;
  {
    std::lock_guard lock(g.mutex);
#if defined(_WIN32)
    st.running = g.process && WaitForSingleObject(g.process, 0) == WAIT_TIMEOUT;
#else
    st.running = Running();
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
#else
  const int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) return {};
#endif
  std::string out;
  for (const char* target : {"192.168.255.254", "10.255.255.254", "172.31.255.254", "8.8.8.8"}) {
    sockaddr_in to{};
    to.sin_family = AF_INET, to.sin_port = htons(9);
    inet_pton(AF_INET, target, &to.sin_addr);
    sockaddr_in own{};
    socklen_t n = sizeof(own);
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
#if defined(_WIN32)
  closesocket(s);
#else
  close(s);
#endif
  return out;
}

}  // namespace bf1943
