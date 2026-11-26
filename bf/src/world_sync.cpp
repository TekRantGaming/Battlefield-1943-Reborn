// The match's world followed by this game, and the admin's panel (14 Nov 2026: "cloud coverage, rate, weather
// percentage, day/night adjustment and time scale settings and sliders for servers and in the create match menu ...
// in-game admin controls for whoever creates a custom match ... giving player IDs admin panel access in-game").
//
// The match's host keeps the world (gm_host.cpp's World in a match a player's game hosts, server/gmhost.py's in the
// server package). This game asks the host for it every two seconds on a UDP socket of its own to the host's port
// (the port players already reach), and hands the answer to the renderer (native_x_renderer::SetWorldState), which
// then draws the host's hour, clouds and weather instead of the player's own sky settings. A host that keeps none
// does not answer: the player's own settings stay.
//
//   ask     "BF43WRLD", version 1, flags (1: a change follows), the player's name and a key (a length byte and text
//           each); for a change: day and night (a byte), then floats: hour, a day's minutes, cloud cover, cloud
//           rate, weather, distance fog, ground fog, wind, and (15 Nov 2026) the cloud base's height in metres
//   answer  "BF43WRLR", version, flags (1 day and night, 2 the asker is an admin), the same eight floats as they are
//           now, the clouds' two clocks as doubles (drift seconds, forming), and the cloud base's height (a float)
// 19 Nov 2026, last in both: the dynamic storm (a byte, 1 on), and in the answer then the match's rules: tickets (16 bits,
// 0 the level's own), the time limit in minutes (16 bits, 0 none) and the round's seconds left (a float, -1 none).
// The cloud height came later and stands last in both: a host from before it sends and reads none (450 m then).
// D224 (20 Nov 2026), last in both: the storm's type, a byte (0 a thunderstorm, 1 a hurricane); none: a thunderstorm.
// Night Ops' admin (24 Nov 2026): a change ends with a u16 of the settings the admin moved (world_sync.h Touched); an
// ask with flag 8 is "Back to Night Ops"; the answer's flags add 4 (a Night Ops server) and 8 (an admin's settings hold
// some of its schedule for the rest of the round). Hosts and games from before read none of them.
// Big endian. An admin is the game that shows the host's key (the game that started a private match has it), or a
// joined player whose ID is in the host's list (bf_world_admins; a dedicated server's --admins).
//
// The panel (F9, for an admin only): the same settings as sliders, sent as they are changed.
//
// Players' pings (20 Nov 2026, the scoreboards' ping, world_sync.h OwnPing and PingOf): every second this game sends
// "BF43PING", "PNG1" and a u32 count on the same socket; every host answers BF43PONG with the same bytes at once (the
// server browser's ping), and the time it took is a sample. The ping is the median of the last five. An ask without a
// change sets flag 4 and puts the ping after the key (u16 ms, 0xFFFF unknown): an older host reads neither. A host
// that keeps them answers such an ask with a second datagram, "BF43PNGL": version, a count, then for each player a
// u16 ms (0xFFFF not known) and a name (a length byte and text). Version 2 lists every joined player, so a scoreboard
// row not on it is a bot (IsBot; bots wear no tag since 20 Nov 2026). Only an ask with flag 4 gets it, so an older game
// (its 128 byte buffer) never sees one.

#include "world_sync.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <native_x_renderer/native_x_renderer.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

#include "comms.h"
#include "free_cam.h"
#include "launcher_kit.h"

namespace bf1943 {
const std::string& PlayerName();  // live_layers.cpp: the name played under online
uint32_t OnlinePlayerId();        // live_layers.cpp: the player's ID as a match's host sees it
bool InMatch();                   // auto_join.cpp
bool LauncherShowing();           // game_menu.cpp
void StartWeatherAudio();         // weather_audio.cpp
void LoadStormPatterns();         // storm_patterns.cpp (D228)
void SkinMatchSample(bool night_ops, int time_limit, float time_left);  // fov.cpp
}  // namespace bf1943

namespace bf1943::world {
double Now() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

namespace {

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
void CloseSocket(Socket s) { close(s); }
#endif

std::mutex g_mutex;
std::string g_joined, g_key;
Status g_status;
bool g_have_request = false;
Settings g_request;
unsigned g_request_touched = 0;  // every setting moved since the last change went out (sent three times)
int g_request_sends = 0;
int g_back_sends = 0;  // Night Ops' "Back to Night Ops": asked until an answer says nothing is held (at most 3)
std::atomic<bool> g_started{false};

// The pings (g_mutex): this game's own, and the host's list of every player's (names lower case, without a clan tag).
std::atomic<int> g_own_ping{-1};
std::vector<std::pair<std::string, int>> g_board;  // -1: a joined player whose ping is not known
bool g_board_complete = false;                     // version 2: every joined player is on it (IsBot)
double g_board_at = -100;

std::string BoardKey(const std::string& name) {
  std::string s = comms::PlainName(name);
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

void PutF32(std::string& b, float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  for (int s = 24; s >= 0; s -= 8) b += char((bits >> s) & 0xFF);
}
void PutText(std::string& b, const std::string& s) {
  const size_t n = std::min<size_t>(s.size(), 64);
  b += char(n);
  b.append(s, 0, n);
}
float GetF32(const uint8_t* p) {
  const uint32_t bits = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  float v;
  std::memcpy(&v, &bits, 4);
  return std::isfinite(v) ? v : 0.0f;
}
double GetF64(const uint8_t* p) {
  uint64_t bits = 0;
  for (int i = 0; i < 8; ++i) bits = bits << 8 | p[i];
  double v;
  std::memcpy(&v, &bits, 8);
  return std::isfinite(v) ? v : 0.0;
}

void Run() {
#if defined(_WIN32)
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  Socket sock = kNoSocket;
  sockaddr_in to{};
  std::string target;
  double next_ask = 0, last_answer = -100;
  bool valid = false;
  // The pings: the ones out (count, sent at), the last five round trips, and when the next goes.
  std::vector<std::pair<uint32_t, double>> pings_out;
  std::vector<int> samples;
  uint32_t ping_count = 0;
  double next_ping = 0, last_pong = -100;
  bool said_ping = false;
  for (;;) {
    // Waits for the socket (at most 50 ms), so a ping's answer is timed as it comes, not on a 50 ms step.
    if (sock != kNoSocket) {
      fd_set read;
      FD_ZERO(&read);
      FD_SET(sock, &read);
      timeval wait{0, 50000};
      select(int(sock) + 1, &read, nullptr, nullptr, &wait);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::string joined, key;
    Settings request;
    unsigned touched = 0;
    bool have_request = false, back = false;
    {
      std::lock_guard lock(g_mutex);
      joined = g_joined, key = g_key;
      if (g_have_request && g_request_sends > 0) have_request = true, request = g_request, touched = g_request_touched;
      back = !have_request && g_back_sends > 0;
    }
    const double now = Now();
    if (joined != target) {
      target = joined;
      if (sock != kNoSocket) CloseSocket(sock), sock = kNoSocket;
      last_answer = -100, next_ask = 0;
      pings_out.clear(), samples.clear(), next_ping = 0, last_pong = -100, said_ping = false;
      g_own_ping = -1;
      {
        std::lock_guard lock(g_mutex);
        g_board.clear(), g_board_at = -100, g_board_complete = false;
      }
      if (!target.empty()) {
        const size_t colon = target.rfind(':');
        addrinfo hints{}, *found = nullptr;
        hints.ai_family = AF_INET, hints.ai_socktype = SOCK_DGRAM;
        if (colon != std::string::npos &&
            getaddrinfo(target.substr(0, colon).c_str(), target.substr(colon + 1).c_str(), &hints, &found) == 0 && found) {
          std::memcpy(&to, found->ai_addr, sizeof(to));
          freeaddrinfo(found);
          sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
          if (sock != kNoSocket) {
            // Test runs on one PC (BF_LOOPBACK_ONLY): from this client's own loopback address (BF_LOOPBACK_ADDR), as
            // its game's and its comms' sockets, so the host knows the asker for a joined player (its ping, an admin).
            if (const char* lo = std::getenv("BF_LOOPBACK_ONLY"); lo && *lo == '1') {
              sockaddr_in me{};
              me.sin_family = AF_INET;
              const char* a = std::getenv("BF_LOOPBACK_ADDR");
              inet_pton(AF_INET, a && *a ? a : "127.0.0.1", &me.sin_addr);
              bind(sock, reinterpret_cast<const sockaddr*>(&me), sizeof(me));
            }
#if defined(_WIN32)
            u_long on = 1;
            ioctlsocket(sock, FIONBIO, &on);
#else
            fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif
          }
        }
      }
    }
    // Lost for eight seconds (or no match): the player's own settings again.
    if (valid && (sock == kNoSocket || now - last_answer > 8.0)) {
      valid = false;
      native_x_renderer::SetWorldState(native_x_renderer::WorldState{});
      std::lock_guard lock(g_mutex);
      g_status = Status{};
      REXLOG_INFO("BF world: the match's host no longer answers: the player's own sky settings again");
    }
    if (sock == kNoSocket) continue;
    // No answer to a ping for eight seconds: unknown again.
    if (now - last_pong > 8.0 && g_own_ping.load() >= 0) g_own_ping = -1, samples.clear();
    if (now >= next_ping && !have_request) {
      next_ping = now + 1.0;
      std::string ping = "BF43PINGPNG1";
      const uint32_t id = ++ping_count;
      for (int s = 24; s >= 0; s -= 8) ping += char((id >> s) & 0xFF);
      sendto(sock, ping.data(), int(ping.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
      pings_out.emplace_back(id, Now());
      if (pings_out.size() > 8) pings_out.erase(pings_out.begin());
    }
    if (now >= next_ask || have_request || back) {
      next_ask = now + (have_request || back ? 0.3 : 2.0);
      std::string ask = "BF43WRLD";
      ask += char(1);
      ask += char(have_request ? 1 : back ? 8 : 4);  // 4: the ping follows the key; 8: Back to Night Ops
      PutText(ask, PlayerName());
      PutText(ask, key);
      if (back) {
        std::lock_guard lock(g_mutex);
        --g_back_sends;
      } else if (!have_request) {
        const int own = g_own_ping.load();
        const uint32_t ms = own < 0 ? 0xFFFF : uint32_t(std::min(own, 0xFFFE));
        ask += char(ms >> 8), ask += char(ms & 0xFF);
      }
      if (have_request) {
        pings_out.clear();  // the wait below would add to their times
        ask += char(request.day_night ? 1 : 0);
        for (float v : {request.hour, request.day_minutes, request.cloud_cover, request.cloud_rate, request.weather,
                        request.fog, request.ground_fog, request.wind})
          PutF32(ask, v);
        PutF32(ask, request.cloud_height);
        ask += char(request.dynamic_storm ? 1 : 0);  // 19 Nov 2026, last: a host from before it reads none
        ask += char(std::clamp(request.storm_type, 0, 1));  // D224, last
        PutF32(ask, request.ground_fog_height);  // 21 Nov 2026, last: a host from before it reads none
        PutF32(ask, request.ground_fog_sparse);
        // 24 Nov 2026, last: the settings the admin moved (u16; world_sync.h Touched). A Night Ops server takes only
        // those, so a value its schedule moved since this game's last answer is never set back; others read none.
        ask += char((touched >> 8) & 0xFF), ask += char(touched & 0xFF);
        std::lock_guard lock(g_mutex);
        if (--g_request_sends <= 0) g_have_request = false, g_request_touched = 0;
      }
      sendto(sock, ask.data(), int(ask.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
      if (have_request || back) std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    for (;;) {
      uint8_t buf[2048];
      sockaddr_in from{};
#if defined(_WIN32)
      int from_size = sizeof(from);
#else
      socklen_t from_size = sizeof(from);
#endif
      const int n = int(recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &from_size));
      if (n <= 0) break;
      if (from.sin_addr.s_addr != to.sin_addr.s_addr || from.sin_port != to.sin_port) continue;
      const double got_at = Now();
      // A ping's answer: its round trip, and the ping now (the median of the last five).
      if (n >= 16 && std::memcmp(buf, "BF43PONGPNG1", 12) == 0) {
        const uint32_t id = uint32_t(buf[12]) << 24 | uint32_t(buf[13]) << 16 | uint32_t(buf[14]) << 8 | buf[15];
        for (size_t i = 0; i < pings_out.size(); ++i) {
          if (pings_out[i].first != id) continue;
          samples.push_back(int(std::lround((got_at - pings_out[i].second) * 1000.0)));
          if (samples.size() > 5) samples.erase(samples.begin());
          pings_out.erase(pings_out.begin(), pings_out.begin() + std::ptrdiff_t(i) + 1);
          std::vector<int> sorted = samples;
          std::sort(sorted.begin(), sorted.end());
          g_own_ping = sorted[sorted.size() / 2];
          last_pong = got_at;
          if (!said_ping && samples.size() >= 3) {
            said_ping = true;
            REXLOG_INFO("BF ping: {} ms to the match's host {}", g_own_ping.load(), target);
          }
          break;
        }
        continue;
      }
      // The host's list of every player's ping (an answer to an ask with flag 4).
      if (n >= 10 && std::memcmp(buf, "BF43PNGL", 8) == 0 && (buf[8] == 1 || buf[8] == 2)) {
        std::vector<std::pair<std::string, int>> board;
        const int count = buf[9];
        int at = 10;
        for (int i = 0; i < count && at + 3 <= n; ++i) {
          const int ms = int(buf[at]) << 8 | buf[at + 1], len = buf[at + 2];
          at += 3;
          if (at + len > n) break;
          const std::string name(reinterpret_cast<const char*>(buf + at), size_t(len));
          at += len;
          if (!name.empty()) board.emplace_back(BoardKey(name), ms == 0xFFFF ? -1 : ms);
        }
        static bool said_board = false;
        if (!said_board) {
          said_board = true;
          REXLOG_INFO("BF ping: the match's host keeps players' pings ({} now)", board.size());
        }
        std::lock_guard lock(g_mutex);
        g_board = std::move(board), g_board_at = got_at, g_board_complete = buf[8] == 2;
        continue;
      }
      if (n < 8 + 2 + 32 + 16 || std::memcmp(buf, "BF43WRLR", 8) != 0 || buf[8] != 1) continue;
      Status s;
      s.answered = true;
      s.now.day_night = (buf[9] & 1) != 0;
      s.admin = (buf[9] & 2) != 0;
      s.night_ops = (buf[9] & 4) != 0, s.held = (buf[9] & 8) != 0;  // 24 Nov 2026
      s.daybreak = s.night_ops && (buf[9] & 16) != 0;                // 26 Nov 2026: the event is Daybreak
      s.now.hour = GetF32(buf + 10), s.now.day_minutes = GetF32(buf + 14), s.now.cloud_cover = GetF32(buf + 18);
      s.now.cloud_rate = GetF32(buf + 22), s.now.weather = GetF32(buf + 26);
      s.now.fog = GetF32(buf + 30), s.now.ground_fog = GetF32(buf + 34), s.now.wind = GetF32(buf + 38);
      if (n >= 62) s.now.cloud_height = std::clamp(GetF32(buf + 58), 100.0f, 3000.0f);
      // 19 Nov 2026, last: the dynamic storm, and the match's rules (tickets, time limit, the round's time left).
      if (n >= 71) {
        s.now.dynamic_storm = (buf[62] & 1) != 0;
        s.tickets = int(buf[63]) << 8 | buf[64];
        s.time_limit = int(buf[65]) << 8 | buf[66];
        s.time_left = GetF32(buf + 67);
      }
      if (n >= 72) s.now.storm_type = std::clamp(int(buf[71]), 0, 1);  // D224
      if (n >= 80) {  // 21 Nov 2026: the surface fog's depth and how sparse it is
        s.now.ground_fog_height = std::clamp(GetF32(buf + 72), 0.2f, 5.0f);
        s.now.ground_fog_sparse = std::clamp(GetF32(buf + 76), 0.0f, 1.0f);
      }
      s.answered_at = now;
      native_x_renderer::WorldState w;
      w.valid = true;
      w.cloud_height = s.now.cloud_height;
      w.day_night = s.now.day_night;
      w.hour = s.now.hour, w.day_minutes = s.now.day_minutes;
      w.cloud_cover = s.now.cloud_cover, w.cloud_rate = s.now.cloud_rate, w.weather = s.now.weather;
      w.fog = s.now.fog, w.ground_fog = s.now.ground_fog, w.wind = s.now.wind;
      w.ground_fog_height = s.now.ground_fog_height, w.ground_fog_sparse = s.now.ground_fog_sparse;
      w.cloud_seconds = GetF64(buf + 42), w.cloud_evolve = GetF64(buf + 50);
      w.storm_type = s.now.storm_type;
      native_x_renderer::SetWorldState(w);
      bool was_admin, was_held;
      {
        std::lock_guard lock(g_mutex);
        was_admin = g_status.admin, was_held = g_status.held;
        g_status = s;
        if (!s.held) g_back_sends = 0;  // on Night Ops' schedule: "Back to Night Ops" done
      }
      // Night Ops: an admin's settings now hold its schedule, or it is back on it (every player's game says so).
      if (s.night_ops && valid && s.held != was_held)
        REXLOG_INFO("BF world: {} {}: {:.2f} h, a day in {:.0f} min, clouds {:.0f}%, weather {:.0f}%, fog {:.0f}%, "
                    "ground fog {:.0f}%",
                    s.EventName(), s.held ? "held by an admin's settings for the rest of the round" : "on its schedule again",
                    s.now.hour, s.now.day_minutes, s.now.cloud_cover * 100, s.now.weather * 100, s.now.fog * 100,
                    s.now.ground_fog * 100);
      bf1943::SkinMatchSample(s.night_ops && !s.daybreak, s.time_limit, s.time_left);  // fov.cpp: the skin's unlock
      if (!valid)
        REXLOG_INFO("BF world: the match's host keeps its world: day and night {}, {:.1f} h, a day in {:.0f} min, clouds "
                    "{:.0f}% at rate {:.0f}%, weather {:.0f}%, fog {:.0f}%, ground fog {:.0f}%, wind {:.0f}%, cloud height {:.0f} m{}",
                    s.now.day_night ? "on" : "off", s.now.hour, s.now.day_minutes, s.now.cloud_cover * 100,
                    s.now.cloud_rate * 100, s.now.weather * 100, s.now.fog * 100, s.now.ground_fog * 100, s.now.wind * 100,
                    s.now.cloud_height, s.admin ? "; this game is an admin (F9)" : "");
      else if (s.admin && !was_admin)
        REXLOG_INFO("BF world: this game is an admin of the match's world (F9)");
      valid = true, last_answer = now;
    }
  }
}

void StartOnce() {
  if (g_started.exchange(true)) return;
  LoadStormPatterns();  // storm_patterns.cpp (D228): the storms' radar patterns for the renderer
  std::thread(Run).detach();
  StartWeatherAudio();  // weather_audio.cpp: the rain, wind and thunder follow this world
}

// ---------------------------------------------------------------------------------------------------------------
// The admin's panel (F9): the f8 panel's look, mouse and keys.

// Where the panel's rows are (in the 720 high stage's units `u`): its top, a row's height (as high as fit the screen
// with the title and the help below, 44 at most), and a row's track under its label.
constexpr int kPanelRows = 16;
struct Layout {
  float y0, rh, track;
};
Layout PanelLayout(float u) {
  Layout l;
  l.y0 = 72 * u;
  l.rh = std::min(44 * u, (720 * u - l.y0 - 48 * u - 64 * u - 6 * u) / kPanelRows);
  l.track = std::min(30 * u, l.rh - 9 * u);
  return l;
}

class AdminPanel final : public rex::ui::ImGuiDialog {
 public:
  explicit AdminPanel(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  std::atomic<bool> open{false};

 protected:
  void OnDraw(ImGuiIO& io) override {
    const Status status = Current();
#if defined(BF_DEV_TOOLS)
    // BF_DEV_WORLD_PANEL=<seconds> (a test run has no keyboard or mouse): that long after this game became an admin
    // the panel opens by itself and is used as a player would, with a pretend pointer and keys: the weather's track
    // is clicked at 60% and dragged to 40%, the cloud height's box clicked and 777 typed, the time's box clicked and
    // 18:30:15 typed, each taken with Enter; what the host then keeps is logged (weather 40%, 777 m, 18:30:15 and on).
    if (const char* v = std::getenv("BF_DEV_WORLD_PANEL"); v && *v && status.admin) {
      static const double since = ImGui::GetTime();
      static int stage = 0;
      const double after = ImGui::GetTime() - since - std::atof(v);
      // Where the rows below are drawn: a row's track at a fraction, and its value's box.
      const float du = io.DisplaySize.y / 720.0f;
      const float tx0 = 16 * du + 14 * du, tx1 = 16 * du + 420 * du - 14 * du;
      const Layout lay = PanelLayout(du);
      auto row_y = [&](int i) { return lay.y0 + 48 * du + lay.rh * i; };
      auto track = [&](int i, float at) { return ImVec2(tx0 + (tx1 - tx0) * at, row_y(i) + lay.track); };
      auto box = [&](int i) { return ImVec2(16 * du + 416 * du - 30 * du, row_y(i) + 11 * du); };
      auto type = [&](const char* text) {
        for (const char* c = text; *c; ++c) io.AddInputCharacter(ImWchar(*c));
        io.AddKeyEvent(ImGuiKey_Enter, true);
      };
      // 24 Nov 2026 (Night Ops' admin): BF_DEV_WORLD_PANEL_TIME the time typed (18:30:15 if none), the panel closed
      // two seconds after the log, and BF_DEV_WORLD_PANEL_BACK=<s> (from the same start) opens it again and clicks
      // "Back to Night Ops" (a Night Ops server's last row), then logs what the host keeps and closes it.
      const char* typed_time = std::getenv("BF_DEV_WORLD_PANEL_TIME");
      const char* back_env = std::getenv("BF_DEV_WORLD_PANEL_BACK");
      const double back_at = back_env && *back_env ? std::atof(back_env) : 1e9;
      const double kAt[] = {0.0, 3.0, 3.3, 3.6, 6.0, 6.2, 6.4, 8.0, 8.2, 8.4, 11.0, 13.0,
                            back_at, back_at + 0.5, back_at + 0.7, back_at + 4.0};
      if (stage < int(std::size(kAt)) && after > kAt[stage]) {
        switch (stage) {
          case 0: open = true, edit_ = status.now, edited_at_ = -100; break;
          case 1: io.MousePos = track(5, 0.6f), io.MouseDown[0] = true; break;  // weather: pressed at 60%
          case 2: io.MousePos = track(5, 0.4f); break;                          // dragged to 40%
          case 3: io.MouseDown[0] = false; break;
          case 4: io.MousePos = box(10), io.MouseDown[0] = true; break;  // the cloud height's box
          case 5: io.MouseDown[0] = false, type("777"); break;
          case 6: io.AddKeyEvent(ImGuiKey_Enter, false); break;
          case 7: io.MousePos = box(1), io.MouseDown[0] = true; break;  // the time's box
          case 8: io.MouseDown[0] = false, type(typed_time && *typed_time ? typed_time : "18:30:15"); break;
          case 9: io.AddKeyEvent(ImGuiKey_Enter, false); break;
          case 10:
          case 15: {
            const long t = std::lround(double(status.now.hour) * 3600.0) % 86400;
            REXLOG_INFO("BF world: the panel's test (pointer and keys){}: the host keeps weather {:.0f}%, cloud height "
                        "{:.0f} m, time {:02d}:{:02d}:{:02d}, clouds {:.0f}%, fog {:.0f}%{}",
                        stage == 15 ? " after Back to Night Ops" : "", status.now.weather * 100,
                        status.now.cloud_height, t / 3600, t / 60 % 60, t % 60, status.now.cloud_cover * 100,
                        status.now.fog * 100,
                        !status.night_ops ? ""
                        : status.held     ? fmt::format("; {} held by an admin", status.EventName())
                                          : fmt::format("; {} on its schedule", status.EventName()));
            if (stage == 15) open = false;
            break;
          }
          case 11: open = false, io.MousePos = ImVec2(-1, -1); break;
          case 12: open = true, edit_ = status.now, edited_at_ = -100; break;
          case 13: io.MousePos = ImVec2(16 * du + 420 * du - 12 * du - 90 * du, lay.y0 + 20 * du), io.MouseDown[0] = true; break;
          case 14: io.MouseDown[0] = false; break;
        }
        ++stage;
      }
    }
#endif
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false) && !LauncherShowing()) {
      if (open) open = false;
      else if (status.answered && status.admin) open = true, edit_ = status.now, edited_at_ = -100;
      else told_at_ = ImGui::GetTime();
    }
    const float u = io.DisplaySize.y / 720.0f;
    const kit::Fonts& f = kit::GetFonts();
    ImDrawList* d = ImGui::GetForegroundDrawList();
    // A match with a time limit (match_rules.cpp): the round's time left, for every player, over the ticket bars; at
    // 0 with both teams level, overtime until the next ticket lost.
    if (status.answered && status.time_left >= 0 && InMatch() && !LauncherShowing()) {
      const long left = long(std::ceil(std::max(0.0, double(status.time_left) - (Now() - status.answered_at))));
      char clock[32];
      if (left > 0) std::snprintf(clock, sizeof(clock), "%ld:%02ld", left / 60, left % 60);
      else std::snprintf(clock, sizeof(clock), "OVERTIME");
      const float size = 17 * u, w = kit::TextWidth(f.ui_bold, size, clock) + 20 * u;
      const ImVec2 a(io.DisplaySize.x * 0.5f - w * 0.5f, 34 * u), b(a.x + w, a.y + 23 * u);
      d->AddRectFilled(a, b, IM_COL32(18, 22, 14, 190));
      d->AddText(f.ui_bold, size, ImVec2(a.x + 10 * u, a.y + 2.5f * u),
                 left > 0 && left <= 60 ? IM_COL32(236, 92, 76, 255) : kit::kCream, clock);
    }
    if (!open) {
      // F9 without the right: said for four seconds, with the ID a server's owner would add.
      if (ImGui::GetTime() - told_at_ < 4 && told_at_ > 0) {
        char line[160];
        if (!status.answered) std::snprintf(line, sizeof(line), "This match has no world settings to change.");
        else std::snprintf(line, sizeof(line), "Admins only. Your player ID is %08X: the server's owner can add it.", OnlinePlayerId());
        const float w = kit::TextWidth(f.ui, 15 * u, line) + 24 * u;
        const ImVec2 a(io.DisplaySize.x * 0.5f - w * 0.5f, 60 * u), b(a.x + w, a.y + 30 * u);
        d->AddRectFilled(a, b, IM_COL32(18, 22, 14, 225));
        d->AddText(f.ui, 15 * u, ImVec2(a.x + 12 * u, a.y + 6 * u), kit::kCream, line);
      }
      return;
    }
    // Esc leaves a box being typed in first, then closes the panel.
    const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (escape && typing_ >= 0) typing_ = -1;
    else if (LauncherShowing() || !status.answered || !status.admin || escape) {
      open = false, typing_ = -1;
      return;
    }
    // The host's values, except for a second after a change of ours (its answer is on the way).
    if (ImGui::GetTime() - edited_at_ > 1.5 && drag_ < 0) edit_ = status.now;
    if (edit_.day_minutes > 0) kept_minutes_ = edit_.day_minutes;

    struct Row {
      const char* label;
      float* value;
      float lo, hi, step;
      const char* help;
    };
    // The first row pauses the clock (16 Nov 2026: turning the day and night off "should just pause the time
    // progression", not bring the game's own afternoon back): the time passes while the match has its clock and a day
    // has a length.
    const bool passed = edit_.day_night && edit_.day_minutes > 0;
    float passes = passed ? 1.0f : 0.0f;
    // The dynamic storm (19 Nov 2026): a switch, as the first row.
    float storm = edit_.dynamic_storm ? 1.0f : 0.0f;
    float storm_type = float(edit_.storm_type);  // D224: a choice, last
    float free_cam = freecam::Wanted() ? 1.0f : 0.0f, hide_ui = freecam::HideUi() ? 1.0f : 0.0f;
    const Settings before = edit_;
    // Every slider moves in steps of one (18 Nov 2026: "stepped by 1pt each, with time stepped at 1pt for each
    // second"): a percent, a minute of a day's length, a metre of the cloud base, and a second of the time of day (the
    // hour is kept in hours, so a second is 1/3600 of one; the last second of a day is 23:59:59).
    static constexpr float kSecond = 1.0f / 3600.0f, kLastHour = 86399.0f / 3600.0f;
    Row rows[] = {
        {"Time passes", &passes, 0, 1, 1, "On, the match's clock runs. Off, the time stands still at the time of day."},
        {"Time of day", &edit_.hour, 0, kLastHour, kSecond,
         "12 is noon, 18 sunset, 0 midnight. Click the value to type it (18:30 or 18:30:15)."},
        {"A day lasts", &edit_.day_minutes, 0, 240, 1, "Minutes a whole day and night take (typed: up to 1440). 0: the time stands still."},
        {"Cloud cover", &edit_.cloud_cover, 0, 1, 0.01f, "How much of the sky the volumetric clouds take."},
        {"Cloud change rate", &edit_.cloud_rate, 0, 1, 0.01f, "How fast the clouds form and dissolve."},
        {"Weather", &edit_.weather, 0, 1, 0.01f, "From fair to a storm: overcast, mist, rain, lightning."},
        {"Distance fog", &edit_.fog, 0, 1, 0.01f, "A haze that hides what is far."},
        {"Ground fog", &edit_.ground_fog, 0, 1, 0.01f,
         "How thick the fog lying on the ground and the water is. What moves through it pushes it about."},
        {"Ground fog sparseness", &edit_.ground_fog_sparse, 0, 1, 0.01f,
         "From everywhere (0) to a few small drifting patches (100)."},
        {"Wind", &edit_.wind, 0, 1, 0.01f, "From calm to a gale: how far the rain leans. Follows the weather until set apart."},
        {"Cloud height", &edit_.cloud_height, 100, 1500, 1, "The cloud base over the sea. Low, the planes fly through the clouds."},
        {"Ground fog height", &edit_.ground_fog_height, 0.2f, 5, 0.1f,
         "How deep the ground fog is, metres: 0.5 is shin high."},
        {"Dynamic storm", &storm, 0, 1, 1,
         "On, the weather builds to a storm over the match and eases again (shown live above). Weather moved: off."},
        {"Storm type", &storm_type, 0, 1, 1,
         "What the weather builds: a thunderstorm, or a hurricane with its eye and spiral rain bands."},
        // 24 Nov 2026 (free_cam.cpp): this game's own, not the match's: only the admin sees them.
        {"Free camera", &free_cam, 0, 1, 1,
         "Sticks fly and look, A up, B down, LT faster, RT slower, L3 fast, LB/RB speed, Y HUD, Back off. Only yours."},
        {"Hide HUD and UI", &hide_ui, 0, 1, 1,
         "The HUD and every on-screen element hidden while this panel is closed (Y in the free camera). Only yours."},
    };
    constexpr int kStormRow = 12, kTypeRow = 13, kFreeCamRow = 14, kHideRow = 15;  // (10 and 11: the heights, in metres)
    static_assert(std::size(rows) == kPanelRows);
    auto is_switch = [&](int i) { return i == 0 || i == kStormRow || i == kTypeRow || i >= kFreeCamRow; };
    // A value put on its row's steps: whole seconds, percents, minutes or metres.
    auto snap = [](const Row& r, float v) {
      if (r.step == kSecond) return std::min(std::round(v * 3600.0f) / 3600.0f, kLastHour);
      return r.lo + std::round((v - r.lo) / r.step) * r.step;
    };
    // The mouse over a rectangle. Unclipped: the panel is drawn on the foreground list with no window of its own, so
    // ImGui's clip would be its hidden 400 by 400 default window's, and most of the panel would not see the mouse.
    auto over = [](ImVec2 a, ImVec2 b) { return ImGui::IsMouseHoveringRect(a, b, false); };
    const int count = int(std::size(rows));
    // Rows as high as fit the screen (24 Nov 2026: sixteen rows of 44 reached past its foot), 44 at most.
    const Layout lay = PanelLayout(u);
    const float w = 420 * u, x0 = 16 * u, x1 = x0 + w, y0 = lay.y0, rh = lay.rh;
    const float y1 = y0 + 48 * u + rh * count + 64 * u;
    d->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(18, 22, 14, 225));
    d->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(201, 183, 122, 102), 0, 0, u);
    d->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + 40 * u), IM_COL32(93, 138, 50, 90));
    d->AddText(f.ui_bold, 20 * u, ImVec2(x0 + 12 * u, y0 + 6 * u), kit::kCream, "MATCH ADMIN (F9)");
    // Night Ops (24 Nov 2026): what an admin changes here holds for the rest of the round (server/gmhost.py
    // NIGHT_OPS_HELD); this button, or N, hands it all back to the server's schedule (18:30, the storm by the tickets).
    // In the header: the rows already reach past the screen's foot.
    if (status.night_ops) {
      const ImVec2 b1(x1 - 12 * u, y0 + 33 * u), b0(b1.x - 180 * u, y0 + 7 * u);
      const bool hot = status.held && over(b0, b1);
      const char* label = status.daybreak ? (status.held ? "BACK TO DAYBREAK (N)" : "ON DAYBREAK'S SCHEDULE")
                                          : (status.held ? "BACK TO NIGHT OPS (N)" : "ON NIGHT OPS' SCHEDULE");
      d->AddRectFilled(b0, b1, status.held ? (hot ? IM_COL32(201, 183, 122, 90) : IM_COL32(18, 22, 14, 160))
                                           : IM_COL32(18, 22, 14, 80));
      d->AddRect(b0, b1, status.held ? kit::kGold : IM_COL32(232, 226, 204, 60), 0, 0, u);
      d->AddText(f.ui_bold, 12 * u, ImVec2(b0.x + (180 * u - kit::TextWidth(f.ui_bold, 12 * u, label)) * 0.5f, b0.y + 6 * u),
                 status.held ? kit::kCream : IM_COL32(207, 198, 168, 160), label);
      if (status.held && typing_ < 0 &&
          ((hot && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) || ImGui::IsKeyPressed(ImGuiKey_N, false))) {
        BackToNightOps();
        edited_at_ = -100;  // the host's values again as soon as it answers
        REXLOG_INFO("BF world: the admin asked for {}'s schedule back", status.EventName());
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) row_ = (row_ + count - 1) % count;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) row_ = (row_ + 1) % count;
    // Left and right: a step; with Shift, ten (sixty on the time: a minute).
    const int key_step = (ImGui::IsKeyPressed(ImGuiKey_RightArrow) ? 1 : 0) - (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ? 1 : 0);
    const bool key_shift = io.KeyShift;
    bool changed = false;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag_ = -1;
    // A value's box, typed in: a click on it, or a digit typed on its row, starts it; Enter takes it.
    if (typing_ >= 0 && typing_ != row_) typing_ = -1;
    {
      ImGuiContext& g = *ImGui::GetCurrentContext();
      g.PlatformImeData.WantTextInput = true;
      g.PlatformImeData.ViewportId = ImGui::GetMainViewport()->ID;
      for (int k = 0; k < io.InputQueueCharacters.Size && !is_switch(row_); ++k) {
        const ImWchar c = io.InputQueueCharacters[k];
        if (!((c >= '0' && c <= '9') || c == ':' || c == '.')) continue;
        if (typing_ != row_) typing_ = row_, typed_.clear();
        if (typed_.size() < 8) typed_ += char(c);  // 18:30:15
      }
      if (typing_ >= 0 && ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        if (typed_.empty()) typing_ = -1;
        else typed_.pop_back();
      }
      if (typing_ >= 0 && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
        if (!typed_.empty()) {
          // The time as hours (18.5), hours and minutes (18:30) or with seconds too (18:30:15).
          const size_t colon = typed_.find(':');
          float v = float(std::atof(typed_.c_str()));
          if (typing_ == 1 && colon != std::string::npos) {
            const size_t colon2 = typed_.find(':', colon + 1);
            v = float(std::atoi(typed_.c_str())) + float(std::atoi(typed_.c_str() + colon + 1)) / 60.0f +
                (colon2 != std::string::npos ? float(std::atoi(typed_.c_str() + colon2 + 1)) / 3600.0f : 0.0f);
          }
          Row& r = rows[typing_];
          if (typing_ == 1) v = std::clamp(v, 0.0f, kLastHour);
          else if (typing_ == 2) v = std::clamp(v, 0.0f, 1440.0f);
          else if (typing_ == 10 || typing_ == 11) v = std::clamp(v, r.lo, r.hi);
          else v = std::clamp(v / 100.0f, 0.0f, 1.0f);
          v = snap(r, v);
          if (v != *r.value) *r.value = v, changed = true;
        }
        typing_ = -1;
      }
    }
    for (int i = 0; i < count; ++i) {
      Row& r = rows[i];
      const float yy = y0 + 48 * u + rh * i;
      const ImVec2 a(x0 + 4 * u, yy), b(x1 - 4 * u, yy + rh - 2 * u);
      const bool hover = over(a, b);
      // The pointer picks the row, except while a box is typed in (its row stays until Enter, Esc or a click).
      if (hover && typing_ < 0 && drag_ < 0 && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) row_ = i;
      if (i == row_) d->AddRectFilled(a, b, IM_COL32(93, 138, 50, 80));
      d->AddText(f.ui, 15 * u, ImVec2(a.x + 10 * u, yy + 2 * u), kit::kCream, r.label);
      char value[32];
      if (i == kTypeRow) {
        std::snprintf(value, sizeof(value), "%s", *r.value > 0.5f ? "Hurricane" : "Storm");
      } else if (is_switch(i)) {
        std::snprintf(value, sizeof(value), "%s", *r.value > 0.5f ? "On" : "Off");
      } else if (i == 1) {
        // The time to the second, as the slider steps it (rounded: a running clock's hour has fractions of one).
        const long s = std::lround(double(*r.value) * 3600.0) % 86400;
        std::snprintf(value, sizeof(value), "%02ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
      } else if (i == 2) {
        std::snprintf(value, sizeof(value), *r.value <= 0 ? "Stands still" : "%.0f min", *r.value);
      } else if (i == 10 || i == 11) {
        std::snprintf(value, sizeof(value), i == 11 ? "%.1f m" : "%.0f m", *r.value);
      } else {
        std::snprintf(value, sizeof(value), "%.0f%%", *r.value * 100.0f);
      }
      if (!is_switch(i)) {
        // The value's box: clicked, it is typed in.
        const ImVec2 v0(b.x - 92 * u, yy + 1 * u), v1(b.x - 6 * u, yy + 21 * u);
        const bool typing = typing_ == i;
        if (over(v0, v1) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) row_ = i, typing_ = i, typed_.clear();
        d->AddRect(v0, v1, typing ? kit::kGold : IM_COL32(232, 226, 204, 70), 0, 0, u);
        if (typing) std::snprintf(value, sizeof(value), "%s%s", typed_.c_str(), std::fmod(ImGui::GetTime(), 1.0) < 0.5 ? "_" : " ");
      }
      d->AddText(f.ui, 15 * u, ImVec2(b.x - 10 * u - kit::TextWidth(f.ui, 15 * u, value), yy + 2 * u), kit::kGreenBright, value);
      // The track: dragged with the mouse, stepped with left and right.
      const float tx0 = a.x + 10 * u, tx1 = b.x - 10 * u, ty = yy + lay.track;
      d->AddRectFilled(ImVec2(tx0, ty - u), ImVec2(tx1, ty + u), IM_COL32(232, 226, 204, 60));
      // A typed day's length past the track's end (up to 1440 minutes) keeps its knob at the end.
      const float at = tx0 + (tx1 - tx0) * std::clamp((*r.value - r.lo) / (r.hi - r.lo), 0.0f, 1.0f);
      d->AddRectFilled(ImVec2(at - 3 * u, ty - 7 * u), ImVec2(at + 3 * u, ty + 7 * u), kit::kGold);
      float v = *r.value;
      // A click on the track (or its knob) sets the value there and holds it until the button is let go.
      if (over(ImVec2(tx0 - 8 * u, ty - 9 * u), ImVec2(tx1 + 8 * u, ty + 12 * u)) &&
          ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        drag_ = i, row_ = i, typing_ = -1;
      }
      if (drag_ == i) {
        v = snap(r, r.lo + (r.hi - r.lo) * std::clamp((io.MousePos.x - tx0) / (tx1 - tx0), 0.0f, 1.0f));
      } else if (i == row_ && key_step && typing_ < 0) {
        const float steps = is_switch(i) ? 1.0f : key_shift ? (i == 1 ? 60.0f : 10.0f) : 1.0f;
        v = snap(r, std::clamp(snap(r, v) + key_step * steps * r.step, r.lo, std::max(r.hi, v)));
      }
      if (v != *r.value) *r.value = v, changed = changed || i < kFreeCamRow;  // the last two are not the match's
    }
    if ((free_cam > 0.5f) != freecam::Wanted()) freecam::Set(free_cam > 0.5f);
    if ((hide_ui > 0.5f) != freecam::HideUi()) freecam::SetHideUi(hide_ui > 0.5f);
    // The first row: on, the clock runs at the length it had (24 minutes if none); off, it stands. A change of the
    // time or of a day's length gives the match its clock's light.
    if ((passes > 0.5f) != passed) {
      edit_.day_night = true;
      edit_.day_minutes = passes > 0.5f ? (kept_minutes_ > 0 ? kept_minutes_ : 24.0f) : 0.0f;
    } else if (edit_.hour != before.hour || edit_.day_minutes != before.day_minutes) {
      edit_.day_night = true;
    }
    // The storm's switch; the weather moved by hand while the storm runs stops it (the storm would take it back).
    edit_.dynamic_storm = storm > 0.5f;
    edit_.storm_type = storm_type > 0.5f ? 1 : 0;  // D224
    if (edit_.dynamic_storm && before.dynamic_storm && edit_.weather != before.weather) edit_.dynamic_storm = false;
    // The wind follows the weather while the two are the same (the user, 19 Nov 2026: "unless it's manually adjusted,
    // so 100% weather should result in 100% wind"): a wind set apart from the weather stays where it was put.
    if (edit_.weather != before.weather && edit_.wind == before.wind && std::abs(before.wind - before.weather) < 0.006f)
      edit_.wind = edit_.weather;
    if (changed) {
      edited_at_ = ImGui::GetTime();
      // What this change moved (24 Nov 2026): a Night Ops server takes only those and holds them for the round.
      const Settings& e = edit_;
      const unsigned touched =
          (e.day_night != before.day_night ? kTouchDayNight : 0) | (e.hour != before.hour ? kTouchHour : 0) |
          (e.day_minutes != before.day_minutes ? kTouchDayMinutes : 0) |
          (e.cloud_cover != before.cloud_cover ? kTouchCover : 0) | (e.cloud_rate != before.cloud_rate ? kTouchRate : 0) |
          (e.weather != before.weather ? kTouchWeather : 0) | (e.fog != before.fog ? kTouchFog : 0) |
          (e.ground_fog != before.ground_fog ? kTouchGroundFog : 0) | (e.wind != before.wind ? kTouchWind : 0) |
          (e.cloud_height != before.cloud_height ? kTouchCloudHeight : 0) |
          (e.dynamic_storm != before.dynamic_storm ? kTouchStorm : 0) |
          (e.storm_type != before.storm_type ? kTouchStormType : 0) |
          (e.ground_fog_height != before.ground_fog_height ? kTouchFogHeight : 0) |
          (e.ground_fog_sparse != before.ground_fog_sparse ? kTouchFogSparse : 0);
      Request(edit_, touched);
    }
    const float fy = y0 + 48 * u + rh * count + 6 * u;
    d->AddLine(ImVec2(x0 + 12 * u, fy), ImVec2(x1 - 12 * u, fy), IM_COL32(201, 183, 122, 64));
    d->AddText(f.ui, 12 * u, ImVec2(x0 + 12 * u, fy + 6 * u), IM_COL32(207, 198, 168, 255), rows[row_].help);
    d->AddText(f.ui, 12 * u, ImVec2(x0 + 12 * u, fy + 24 * u), IM_COL32(207, 198, 168, 255),
               status.daybreak  ? "Every player sees a change within two seconds. Daybreak: it holds to the round's end."
               : status.night_ops ? "Every player sees a change within two seconds. Night Ops: it holds to the round's end."
                                : "Every player of the match sees a change within two seconds.");
    d->AddText(f.ui, 12 * u, ImVec2(x0 + 12 * u, fy + 40 * u), IM_COL32(207, 198, 168, 255),
               "Drag a slider, click a value to type it, or use the arrows \xC2\xB7 F9 or Esc: close");
  }

 private:
  Settings edit_;
  double edited_at_ = -100, told_at_ = 0;
  int row_ = 0, drag_ = -1;
  int typing_ = -1;       // the row whose value is typed in
  std::string typed_;
  float kept_minutes_ = 24.0f;  // a day's length while the time stands still
};

AdminPanel* g_panel = nullptr;

}  // namespace

void NoteJoined(const std::string& address) {
  {
    std::lock_guard lock(g_mutex);
    g_joined = address;
  }
  if (!address.empty()) StartOnce();
}

void SetAdminKey(const std::string& key) {
  std::lock_guard lock(g_mutex);
  g_key = key;
}

Status Current() {
  std::lock_guard lock(g_mutex);
  return g_status;
}

void Request(const Settings& settings, unsigned touched) {
  std::lock_guard lock(g_mutex);
  // UDP may lose one; a change still going out keeps what it moved (its values are in this one too: the panel's).
  g_request_touched = (g_have_request ? g_request_touched : 0) | (touched & kTouchAll);
  g_request = settings, g_have_request = true, g_request_sends = 3;
  g_back_sends = 0;
}

void BackToNightOps() {
  std::lock_guard lock(g_mutex);
  g_back_sends = 3, g_have_request = false, g_request_touched = 0;
}

bool AdminPanelOpen() { return g_panel && g_panel->open.load(); }

int OwnPing() { return g_own_ping.load(); }

int PingOf(const std::string& name) {
  if (comms::IsMe(name)) return OwnPing();
  const std::string key = BoardKey(name);
  std::lock_guard lock(g_mutex);
  if (Now() - g_board_at > 10.0) return -1;
  for (const auto& [n, ms] : g_board)
    if (n == key) return ms;
  return -1;
}

bool IsBot(const std::string& name) {
  if (name.rfind("[BOT]", 0) == 0) return true;  // a server from before 20 Nov 2026's untagged bots
  if (name.empty() || comms::IsMe(name)) return false;
  const std::string key = BoardKey(name);
  std::lock_guard lock(g_mutex);
  if (!g_board_complete || Now() - g_board_at > 10.0) return false;
  for (const auto& entry : g_board)
    if (entry.first == key) return false;
  return true;
}

}  // namespace bf1943::world

namespace bf1943 {
void CreateWorldAdminPanel(rex::ui::ImGuiDrawer* drawer) {
  if (!world::g_panel) world::g_panel = new world::AdminPanel(drawer);  // lives for the session
}
}  // namespace bf1943
