// Voice and text chat in a match (TekRantGaming, 11 Nov 2026: proximity voice through the microphone that both teams
// hear, and a text chat top left for the squad, the team or the whole server, as Battlefield games have). The design
// is docs/COMMS-PLAN.md; the server side is server/gmhost.py's Comms (the relay in each server's host).
//
// The client opens its own UDP socket to the joined server's host port (the port players already reach), so nothing
// new needs opening on a router. Packets start with BF43COMM and a type byte, then big endian fields:
//   HELLO name, team, squad   -> WELCOME id (or REFUSED: not in the game on that server yet, another version)
//   STATE every 0.5 s: team, squad, in the world, the camera's position (the host forwards voice only near it)
//   CHAT channel, text (twice: UDP may lose one; receivers keep the first)   -> CHAT_RELAY to those it is for
//   VOICE seq, position, 20 ms of 16 kHz IMA ADPCM                           -> VOICE_RELAY to listeners in range
// Voice: SDL3's recording and playback streams (WASAPI on Windows, PipeWire or Pulse on Linux), mono 16 kHz. Push to
// talk (keybind_push_to_talk) or open mic (a level gate with a short hold); only while the soldier is in the world.
// Each speaker heard is buffered 40 ms, then faded with distance (full within 4 m, silent at bf_voice_range) and panned
// left or right of the listener's camera.

#include "comms.h"
#include "text_filter.h"

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
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(bf_voice_mode, "push", "BF1943/Voice", "Voice chat: push (push to talk), open (open mic) or off");
REXCVAR_DEFINE_STRING(keybind_push_to_talk, "B", "Input/Keybinds/Voice", "Push to talk (voice chat)");
REXCVAR_DEFINE_DOUBLE(bf_voice_volume, 100.0, "BF1943/Voice", "Other players' voices, 0 to 200 (%)");
REXCVAR_DEFINE_DOUBLE(bf_voice_mic_sensitivity, 50.0, "BF1943/Voice",
                      "Open mic: how quiet a voice still opens it, 0 to 100 (%)");
REXCVAR_DEFINE_DOUBLE(bf_voice_range, 50.0, "BF1943/Voice", "Metres at which a voice fades to nothing");
REXCVAR_DEFINE_BOOL(bf_text_chat, true, "BF1943/Chat", "Text chat shown in matches");
REXCVAR_DECLARE(bool, server);

namespace bf1943 {
const std::string& PlayerName();  // live_layers.cpp: the name played under online
bool InMatch();                   // auto_join.cpp
void GameCommand(const std::string& name, const std::string& args);  // auto_join.cpp: the game's own menu command
}  // namespace bf1943

namespace bf1943::comms {
namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point kStart = Clock::now();

constexpr char kMagic[8] = {'B', 'F', '4', '3', 'C', 'O', 'M', 'M'};
enum : uint8_t { kHello = 1, kWelcome, kState, kChatRelay, kChat, kRoster, kVoice, kVoiceRelay, kBye, kRefused };
constexpr uint8_t kProtocol = 1;
constexpr int kRate = 16000, kFrame = 320;  // 20 ms
constexpr uint8_t kCodecAdpcm = 1;

// ---- IMA ADPCM: 4 bits a sample; each frame carries its starting state, so a lost packet never breaks the next.
constexpr int kStep[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
                           25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
                           88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
                           307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
                           1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
                           3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
                           12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int kIndexStep[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
struct Adpcm {
  int pred = 0, index = 0;
  uint8_t Encode(int sample) {
    int step = kStep[index], diff = sample - pred, delta = step >> 3;
    uint8_t code = 0;
    if (diff < 0) code = 8, diff = -diff;
    if (diff >= step) code |= 4, diff -= step, delta += step;
    step >>= 1;
    if (diff >= step) code |= 2, diff -= step, delta += step;
    step >>= 1;
    if (diff >= step) code |= 1, delta += step;
    Apply(code, delta);
    return code;
  }
  int Decode(uint8_t code) {
    const int step = kStep[index];
    int delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    Apply(code, delta);
    return pred;
  }
  void Apply(uint8_t code, int delta) {
    pred = std::clamp(pred + ((code & 8) ? -delta : delta), -32768, 32767);
    index = std::clamp(index + kIndexStep[code & 15], 0, 88);
  }
};
std::vector<uint8_t> EncodeFrame(Adpcm& st, const int16_t* pcm) {
  std::vector<uint8_t> out(4 + kFrame / 2);
  out[0] = uint8_t(uint16_t(st.pred) >> 8), out[1] = uint8_t(st.pred), out[2] = uint8_t(st.index), out[3] = 0;
  for (int i = 0; i < kFrame; i += 2) {
    const uint8_t lo = st.Encode(pcm[i]), hi = st.Encode(pcm[i + 1]);
    out[4 + i / 2] = uint8_t(lo | hi << 4);
  }
  return out;
}
bool DecodeFrame(const uint8_t* p, size_t n, std::array<int16_t, kFrame>& pcm) {
  if (n != 4 + kFrame / 2) return false;
  Adpcm st;
  st.pred = int16_t(uint16_t(p[0]) << 8 | p[1]), st.index = std::min<int>(p[2], 88);
  for (int i = 0; i < kFrame; i += 2) {
    pcm[i] = int16_t(st.Decode(p[4 + i / 2] & 15));
    pcm[i + 1] = int16_t(st.Decode(p[4 + i / 2] >> 4));
  }
  return true;
}

// ---- packets
struct Writer {
  std::vector<uint8_t> b;
  explicit Writer(uint8_t type) : b(kMagic, kMagic + 8) { b.push_back(type); }
  void u8(uint8_t v) { b.push_back(v); }
  void u16(uint16_t v) { b.push_back(uint8_t(v >> 8)), b.push_back(uint8_t(v)); }
  void f32(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(u >> s));
  }
  void text8(const std::string& s, size_t max) {
    const size_t n = std::min(s.size(), max);
    u8(uint8_t(n));
    b.insert(b.end(), s.begin(), s.begin() + n);
  }
  void bytes(const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); }
};
struct Reader {
  const uint8_t* p;
  size_t n, at = 0;
  bool ok = true;
  bool Need(size_t k) { return ok = ok && at + k <= n; }
  uint8_t u8() { return Need(1) ? p[at++] : 0; }
  uint16_t u16() {
    if (!Need(2)) return 0;
    const uint16_t v = uint16_t(p[at] << 8 | p[at + 1]);
    at += 2;
    return v;
  }
  float f32() {
    if (!Need(4)) return 0;
    const uint32_t u = uint32_t(p[at]) << 24 | uint32_t(p[at + 1]) << 16 | uint32_t(p[at + 2]) << 8 | p[at + 3];
    at += 4;
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  }
  std::string text8() {
    const size_t k = u8();
    if (!Need(k)) return {};
    std::string s(reinterpret_cast<const char*>(p + at), k);
    at += k;
    return s;
  }
};

// ---- UDP
#if defined(_WIN32)
using socket_t = SOCKET;
constexpr socket_t kNoSocket = INVALID_SOCKET;
void CloseSocket(socket_t s) { closesocket(s); }
#else
using socket_t = int;
constexpr socket_t kNoSocket = -1;
void CloseSocket(socket_t s) { close(s); }
#endif
struct Udp {
  socket_t s = kNoSocket;
  sockaddr_in to{};
  std::string target;
  bool Open(const std::string& hostport) {
    Close();
#if defined(_WIN32)
    static const bool started = [] {
      WSADATA w;
      return WSAStartup(MAKEWORD(2, 2), &w) == 0;
    }();
    if (!started) return false;
#endif
    const size_t colon = hostport.rfind(':');
    if (colon == std::string::npos) return false;
    const std::string host = hostport.substr(0, colon), port = hostport.substr(colon + 1);
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET, hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return false;
    std::memcpy(&to, res->ai_addr, sizeof(to));
    freeaddrinfo(res);
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) return false;
    // Test runs on one PC (BF_LOOPBACK_ONLY, tools\run_game.ps1): from this client's own loopback address
    // (BF_LOOPBACK_ADDR: the second client's 127.0.0.2), as its game's sockets (socket_fixes.cpp), so the host sees
    // the same player in both.
    if (const char* lo = std::getenv("BF_LOOPBACK_ONLY"); lo && *lo == '1') {
      sockaddr_in me{};
      me.sin_family = AF_INET;
      const char* a = std::getenv("BF_LOOPBACK_ADDR");
      inet_pton(AF_INET, a && *a ? a : "127.0.0.1", &me.sin_addr);
      bind(s, reinterpret_cast<const sockaddr*>(&me), sizeof(me));
    }
#if defined(_WIN32)
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    target = hostport;
    return true;
  }
  void Close() {
    if (s != kNoSocket) CloseSocket(s);
    s = kNoSocket, target.clear();
  }
  void Send(const Writer& w) const {
    if (s != kNoSocket)
      sendto(s, reinterpret_cast<const char*>(w.b.data()), int(w.b.size()), 0, reinterpret_cast<const sockaddr*>(&to),
             sizeof(to));
  }
  int Recv(uint8_t* buf, int cap) const {  // only the host's own packets
    if (s == kNoSocket) return -1;
    sockaddr_in from{};
#if defined(_WIN32)
    int len = sizeof(from);
#else
    socklen_t len = sizeof(from);
#endif
    const int n = int(recvfrom(s, reinterpret_cast<char*>(buf), cap, 0, reinterpret_cast<sockaddr*>(&from), &len));
    if (n <= 0) return -1;
    if (from.sin_addr.s_addr != to.sin_addr.s_addr || from.sin_port != to.sin_port) return 0;
    return n;
  }
};

// ---- shared state
std::atomic<float> g_cam[9];  // position, right, back
std::atomic<bool> g_cam_valid{false}, g_in_world{false}, g_ptt{false}, g_typing{false}, g_transmitting{false},
    g_connected{false};
std::atomic<double> g_typing_closed_at{-10.0};
std::atomic<int> g_team{0}, g_squad{0};  // NoteTeam: the local player's, 0 while not known
std::mutex g_mutex;  // the members below
std::string g_joined;
std::deque<ChatLine> g_chat;
std::deque<std::pair<Channel, std::string>> g_outbox;
std::vector<std::string> g_speaking;
std::once_flag g_started;

double NowS() { return std::chrono::duration<double>(Clock::now() - kStart).count(); }

void AddLine(ChatLine line) {
  std::lock_guard lock(g_mutex);
  g_chat.push_back(std::move(line));
  while (g_chat.size() > 60) g_chat.pop_front();
}
void Notice(const std::string& text) { AddLine({NowS(), kAll, "", text, false, true}); }

#if defined(BF_DEV_TOOLS)
// Tests: BF_DEV_MIC_FILE=<16 kHz mono 16 bit WAV> in place of the microphone (looped; no real one is opened),
// BF_DEV_VOICE_OUT=<folder> takes the voices heard (voice_<player name>.wav, 16 kHz stereo; no real playback), BF_DEV_VOICE_ALWAYS=1 sends without
// a key, BF_DEV_VOICE_HEAR_AT=<right|left|front|back>,<metres> hears every speaker there from the listener, and
// BF_DEV_CHAT=<s>:<all|team|squad>:<text>[;...] sends chat that many seconds after the relay answered.
const char* Env(const char* n) {
  const char* v = std::getenv(n);
  return v && *v ? v : nullptr;
}
std::vector<int16_t> ReadWav(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  for (size_t at = 12; at + 8 <= b.size();) {
    const uint32_t size = uint32_t(b[at + 4]) | uint32_t(b[at + 5]) << 8 | uint32_t(b[at + 6]) << 16 | uint32_t(b[at + 7]) << 24;
    if (std::memcmp(&b[at], "data", 4) == 0) {
      std::vector<int16_t> pcm(std::min<size_t>(size, b.size() - at - 8) / 2);
      std::memcpy(pcm.data(), &b[at + 8], pcm.size() * 2);
      return pcm;
    }
    at += 8 + size + (size & 1);
  }
  return {};
}
struct WavOut {
  std::FILE* f = nullptr;
  uint32_t bytes = 0;
  explicit WavOut(const char* path) {
    f = std::fopen(path, "wb");
    if (f) Header();
  }
  void Header() {
    const uint32_t rate = kRate, byte_rate = kRate * 4, riff = 36 + bytes;
    std::fseek(f, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, f), std::fwrite(&riff, 4, 1, f), std::fwrite("WAVEfmt ", 1, 8, f);
    const uint32_t fmt_size = 16;
    const uint16_t pcm = 1, channels = 2, align = 4, bits = 16;
    std::fwrite(&fmt_size, 4, 1, f), std::fwrite(&pcm, 2, 1, f), std::fwrite(&channels, 2, 1, f);
    std::fwrite(&rate, 4, 1, f), std::fwrite(&byte_rate, 4, 1, f), std::fwrite(&align, 2, 1, f), std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f), std::fwrite(&bytes, 4, 1, f);
    std::fseek(f, 0, SEEK_END);
  }
  void Write(const float* lr, int frames) {
    if (!f) return;
    for (int i = 0; i < frames * 2; ++i) {
      const int16_t v = int16_t(std::clamp(lr[i], -1.0f, 1.0f) * 32767.0f);
      std::fwrite(&v, 2, 1, f);
    }
    bytes += uint32_t(frames * 4);
    Header();
    std::fflush(f);
  }
};
#endif

// ---- one speaker heard
int16_t SeqDiff(uint16_t a, uint16_t b) { return int16_t(uint16_t(a - b)); }
// A player's name as shown in the chat and the talking list: all stars when it holds profanity (our games refuse such
// a name at the start and on the name page, so only a changed game can send one).
std::string ShownName(const std::string& name) {
  return text_filter::NameAllowed(name) ? name : std::string(name.size(), '*');
}

struct Talker {
  std::string name;
  std::vector<std::pair<uint16_t, std::array<int16_t, kFrame>>> frames;  // a few, in any order (seq wraps)
  uint16_t next = 0;
  bool playing = false;
  double last_packet = -10;
  double audible_at = -10;  // the last frame placed within hearing: only then is the name in the speaking list
  float pos[3] = {0, 0, 0};
  size_t Oldest() const {
    size_t o = 0;
    for (size_t i = 1; i < frames.size(); ++i)
      if (SeqDiff(frames[i].first, frames[o].first) < 0) o = i;
    return o;
  }
  void Add(uint16_t seq, const std::array<int16_t, kFrame>& pcm) {
    for (const auto& f : frames)
      if (f.first == seq) return;
    frames.emplace_back(seq, pcm);
    if (frames.size() > 15) frames.erase(frames.begin() + Oldest());  // over 300 ms behind: catch up
  }
  // The frame to play now, or none (silence): an underrun waits for its frame, a lost one is passed over.
  bool Take(std::array<int16_t, kFrame>& out) {
    for (size_t i = 0; i < frames.size(); ++i)
      if (frames[i].first == next) {
        out = frames[i].second;
        frames.erase(frames.begin() + i);
        ++next;
        return true;
      }
    bool later = false;
    for (const auto& f : frames) later = later || SeqDiff(f.first, next) > 0;
    if (later) ++next;
    return false;
  }
};

class Engine {
 public:
  void Run() {
    REXLOG_INFO("BF comms: ready (voice {}, text chat {})", REXCVAR_GET(bf_voice_mode), REXCVAR_GET(bf_text_chat) ? "on" : "off");
    while (true) {
      Step();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

 private:
  Udp udp_;
  bool welcomed_ = false;
  uint16_t my_id_ = 0, chat_seq_ = 1, voice_seq_ = 0;
  int hellos_ = 0;
  double hello_at_ = -100, state_at_ = -100, welcomed_at_ = 0;
  bool told_unavailable_ = false;
  std::map<uint16_t, Talker> talkers_;
  std::map<uint16_t, std::string> roster_;
  std::deque<uint32_t> seen_order_;
  std::set<uint32_t> seen_;
  // audio
  bool audio_up_ = false, sdl_audio_ = false, real_mic_ = false;
  double mic_retry_at_ = 0;
  SDL_AudioStream* capture_ = nullptr;
  SDL_AudioStream* playback_ = nullptr;
  std::vector<int16_t> captured_;
  Adpcm encoder_;
  double open_until_ = 0;  // open mic: held this long after the last loud frame
  double next_mix_ = 0;
  // Voice traffic for the log (a line every 30 s while there is any: players' reports).
  int frames_sent_ = 0, frames_heard_ = 0;
  double follow_at_ = -100;  // BF_DEV_FOLLOW_TEAM
  double follow_suicide_at_ = -1;  // BF_DEV_FOLLOW_TEAM: the game's suicide after the switch, so it spawns at its base
  std::set<uint16_t> heard_from_;
  double stats_at_ = 0;
#if defined(BF_DEV_TOOLS)
  std::vector<int16_t> dev_mic_;
  size_t dev_mic_at_ = 0;
  double dev_mic_clock_ = 0;
  WavOut* dev_out_ = nullptr;
  std::vector<std::tuple<double, Channel, std::string>> dev_chat_;
#endif

  void Step() {
    const double now = NowS();
    std::string joined;
    {
      std::lock_guard lock(g_mutex);
      joined = g_joined;
    }
    const bool active = !joined.empty() && InMatch();
    if (!active) {
      if (!udp_.target.empty()) Leave("left the match");
      return;
    }
    if (udp_.target != joined) {
      if (!udp_.target.empty()) Leave("another server");
      if (!udp_.Open(joined)) {
        if (now - hello_at_ > 30) {
          REXLOG_WARN("BF comms: {} could not be resolved", joined);
          hello_at_ = now;
        }
        return;
      }
      welcomed_ = false, hellos_ = 0, hello_at_ = -100, told_unavailable_ = false;
      talkers_.clear(), roster_.clear();
      REXLOG_INFO("BF comms: to {}", joined);
    }
    if (!welcomed_) {
      // Every 2 s; after 3 unanswered, once a minute (a server package from before the relay drops them).
      const double every = hellos_ < 3 ? 2.0 : 60.0;
      if (now - hello_at_ >= every) {
        hello_at_ = now, ++hellos_;
        Writer w(kHello);
        w.u8(kProtocol), w.u8(uint8_t(g_team.load())), w.u8(uint8_t(g_squad.load()));
        w.text8(PlayerName(), 64);
        udp_.Send(w);
        if (hellos_ == 3) REXLOG_INFO("BF comms: no answer from {} (a server without the relay?)", joined);
      }
    } else if (now - state_at_ >= 0.5) {
      state_at_ = now;
      Writer w(kState);
      w.u16(my_id_), w.u8(uint8_t(g_team.load())), w.u8(uint8_t(g_squad.load()));
      const bool world = g_in_world.load() && g_cam_valid.load();
      w.u8(world ? 1 : 0);
      for (int i = 0; i < 3; ++i) w.f32(g_cam[i].load());
      udp_.Send(w);
    }
#if defined(BF_DEV_TOOLS)
    if (follow_suicide_at_ > 0 && now >= follow_suicide_at_) {
      follow_suicide_at_ = -1;
      GameCommand("suicide", "");
      REXLOG_INFO("BF comms: the game's suicide after following, to spawn at the new team's base (BF_DEV_FOLLOW_TEAM)");
    }
#endif
    Receive(now);
    Outbox(now);
    Audio(now);
  }

  void Leave(const char* why) {
    if (welcomed_) {
      Writer w(kBye);
      w.u16(my_id_);
      udp_.Send(w);
    }
    REXLOG_INFO("BF comms: off {} ({})", udp_.target, why);
    udp_.Close();
    welcomed_ = false, g_connected = false, g_transmitting = false;
    talkers_.clear(), roster_.clear();
    NoteTeam(0, 0);  // read again in the next match (ReadTeam)
    {
      std::lock_guard lock(g_mutex);
      g_speaking.clear(), g_outbox.clear();
    }
    CloseAudio();
  }

  void Receive(double now) {
    uint8_t buf[2048];
    for (int k = 0; k < 400; ++k) {
      const int n = udp_.Recv(buf, sizeof(buf));
      if (n < 0) break;
      if (n < 9 || std::memcmp(buf, kMagic, 8) != 0) continue;
      Reader r{buf + 9, size_t(n - 9)};
      switch (buf[8]) {
        case kWelcome: {
          const uint16_t id = r.u16();
          if (!r.ok) break;
          if (!welcomed_) {
            REXLOG_INFO("BF comms: in, id {}", id);
            welcomed_at_ = now;
          }
          welcomed_ = true, my_id_ = id, g_connected = true;
          break;
        }
        case kRefused: {
          const uint8_t reason = r.u8();
          const std::string why = r.text8();
          if (reason == 3) welcomed_ = false, g_connected = false, hellos_ = 0, hello_at_ = -100;  // the host restarted
          else if (hellos_ <= 1 || reason == 2) REXLOG_INFO("BF comms: refused ({})", why);
          break;
        }
        case kRoster: {
          const uint8_t count = r.u8();
          std::map<uint16_t, std::string> roster;
          std::map<uint16_t, int> teams;
          for (int i = 0; i < count && r.ok; ++i) {
            const uint16_t id = r.u16();
            const int team = r.u8();
            r.u8();
            const std::string name = r.text8();
            if (r.ok) roster[id] = ShownName(name), teams[id] = team;  // the talking list's names
          }
          if (!r.ok) break;
          roster_ = std::move(roster);
#if defined(BF_DEV_TOOLS)
          // BF_DEV_FOLLOW_TEAM=1 (a test player beside a person playing): when every other player is on the other
          // team, the game's own selectTeam to theirs (at most once in 30 s), so both spawn at the same base.
          if (Env("BF_DEV_FOLLOW_TEAM") && now - follow_at_ > 30) {
            const int mine = g_team.load();
            int theirs = 0, with_me = 0;
            for (const auto& [id, team] : teams) {
              if (id == my_id_ || !team) continue;
              if (team == mine) ++with_me;
              else theirs = team;
            }
            if (mine && theirs && !with_me) {
              follow_at_ = now;
              GameCommand("selectTeam", std::to_string(theirs));
              // The switch leaves the soldier where it stands, at the other team's base (run zipcheck4: the test
              // player went on talking from there, beyond the player's 80 m): the game's own suicide 2 s later, and
              // the test's A presses (BF_MENU_STEPS bfa) then spawn it at its new team's base.
              follow_suicide_at_ = now + 2;
              REXLOG_INFO("BF comms: following the other players to team {} (BF_DEV_FOLLOW_TEAM)", theirs);
            }
          }
#endif
          break;
        }
        case kChatRelay: {
          const uint16_t from = r.u16(), msg = r.u16();
          const uint8_t channel = r.u8();
          r.u8();
          const std::string name = r.text8(), text = r.text8();
          if (!r.ok || channel > kSquad) break;
          const uint32_t key = uint32_t(from) << 16 | msg;
          if (seen_.count(key)) break;
          seen_.insert(key), seen_order_.push_back(key);
          while (seen_order_.size() > 400) seen_.erase(seen_order_.front()), seen_order_.pop_front();
          // Our profanity filter (text_filter.h): each word holding profanity shown as stars, here on every receiving
          // game, the sender's own included, whatever game sent it; a name with profanity (only a changed game can
          // send one) all stars. The host's log keeps the text as sent (moderation).
          const std::string shown_name = ShownName(name), shown = text_filter::Censor(text);
          AddLine({now, Channel(channel), shown_name, shown, from == my_id_, false});
          REXLOG_INFO("BF comms: chat {} {}: {}", channel == kAll ? "all" : channel == kTeam ? "team" : "squad",
                      shown_name, shown);
          break;
        }
        case kVoiceRelay: {
          const uint16_t from = r.u16(), seq = r.u16();
          const float x = r.f32(), y = r.f32(), z = r.f32();
          const uint8_t codec = r.u8();
          const uint16_t len = r.u16();
          if (!r.ok || codec != kCodecAdpcm || !r.Need(len)) break;
          if (REXCVAR_GET(bf_voice_mode) == "off") break;
          Talker& t = talkers_[from];
          std::array<int16_t, kFrame> pcm;
          if (!DecodeFrame(buf + 9 + r.at, len, pcm)) break;
          if (t.playing && SeqDiff(seq, t.next) < 0) break;  // late
          t.Add(seq, pcm);
          ++frames_heard_, heard_from_.insert(from);
          t.last_packet = now, t.pos[0] = x, t.pos[1] = y, t.pos[2] = z;
          auto it = roster_.find(from);
          if (it != roster_.end()) t.name = it->second;
          break;
        }
        default:
          break;
      }
    }
  }

  void Outbox(double now) {
    std::deque<std::pair<Channel, std::string>> out;
    {
      std::lock_guard lock(g_mutex);
      out.swap(g_outbox);
    }
#if defined(BF_DEV_TOOLS)
    if (welcomed_ && dev_chat_.empty() && Env("BF_DEV_CHAT")) {
      static bool parsed = false;
      if (!parsed) {
        parsed = true;
        std::string list = Env("BF_DEV_CHAT");
        for (size_t at = 0; at < list.size();) {
          const size_t end = std::min(list.find(';', at), list.size());
          const std::string item = list.substr(at, end - at);
          const size_t c1 = item.find(':'), c2 = item.find(':', c1 + 1);
          if (c1 != std::string::npos && c2 != std::string::npos) {
            const std::string ch = item.substr(c1 + 1, c2 - c1 - 1);
            dev_chat_.emplace_back(welcomed_at_ + std::atof(item.substr(0, c1).c_str()),
                                   ch == "team" ? kTeam : ch == "squad" ? kSquad : kAll, item.substr(c2 + 1));
          }
          at = end + 1;
        }
      }
    }
    for (auto& [at, ch, text] : dev_chat_)
      if (!text.empty() && now >= at) out.emplace_back(ch, text), text.clear();
#endif
    for (auto& [channel, text] : out) {
      if (!welcomed_) {
        if (!told_unavailable_) Notice("Chat is not available on this server yet."), told_unavailable_ = true;
        continue;
      }
      // The host never sends team or squad chat of a player whose team or squad it was not told (gmhost.py).
      if (channel == kTeam && g_team.load() == 0) {
        Notice("Team chat: your team is not known yet. Try again in a moment.");
        continue;
      }
      if (channel == kSquad && g_squad.load() == 0) {
        Notice("You are not in a squad. Join one from the pause menu's My Squad.");
        continue;
      }
      Writer w(kChat);
      w.u16(my_id_), w.u16(chat_seq_++), w.u8(channel);
      w.text8(text, 200);
      udp_.Send(w), udp_.Send(w);  // twice: the relay keeps both, the receivers the first
    }
  }

  // ---- voice
  void OpenAudio() {
    if (audio_up_) return;
    audio_up_ = true;
    next_mix_ = NowS();
#if defined(BF_DEV_TOOLS)
    if (const char* mic = Env("BF_DEV_MIC_FILE")) {
      dev_mic_ = ReadWav(mic), dev_mic_at_ = 0, dev_mic_clock_ = NowS();
      REXLOG_INFO("BF comms: test microphone {} ({} samples)", mic, dev_mic_.size());
    }
    if (const char* out = Env("BF_DEV_VOICE_OUT")) {
      const std::string path = std::string(out) + "/voice_" + PlayerName() + ".wav";
      dev_out_ = new WavOut(path.c_str());
      REXLOG_INFO("BF comms: voices heard go to {}", path);
    }
    if (!dev_mic_.empty() && dev_out_) return;  // a test: no real device at all
#endif
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      REXLOG_WARN("BF comms: SDL audio did not start ({}): no voice", SDL_GetError());
      return;
    }
    const SDL_AudioSpec out{SDL_AUDIO_F32, 2, kRate};
#if defined(BF_DEV_TOOLS)
    const bool real_mic = dev_mic_.empty(), real_out = !dev_out_;
#else
    const bool real_mic = true, real_out = true;
#endif
    sdl_audio_ = true, real_mic_ = real_mic;  // the microphone opens when first wanted (OpenMic)
    if (real_out) {
      playback_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &out, nullptr, nullptr);
      if (playback_) SDL_ResumeAudioStreamDevice(playback_);
      else REXLOG_WARN("BF comms: voices cannot play ({})", SDL_GetError());
    }
    REXLOG_INFO("BF comms: voice on (playback {})", playback_ ? "open" : "none");
  }
  // The microphone is opened only when the player first wants to talk: Open mic at once, Push to talk at the first
  // press of its key (a player who never talks never has it opened). Then kept open for the match: opening takes a
  // moment, which would cut the start of each sentence.
  void OpenMic(bool wanted, double now) {
    if (capture_ || !sdl_audio_ || !real_mic_ || !wanted || now < mic_retry_at_) return;
    const SDL_AudioSpec in{SDL_AUDIO_S16, 1, kRate};
    capture_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &in, nullptr, nullptr);
    if (capture_) {
      SDL_ResumeAudioStreamDevice(capture_);
      REXLOG_INFO("BF comms: microphone open");
    } else {
      REXLOG_WARN("BF comms: no microphone ({}); trying again in 10 s", SDL_GetError());
      mic_retry_at_ = now + 10;
    }
  }
  void CloseAudio() {
    if (!audio_up_) return;
    audio_up_ = false;
    sdl_audio_ = false, mic_retry_at_ = 0;
    if (capture_) SDL_DestroyAudioStream(capture_), capture_ = nullptr;
    if (playback_) SDL_DestroyAudioStream(playback_), playback_ = nullptr;
    captured_.clear();
#if defined(BF_DEV_TOOLS)
    delete dev_out_, dev_out_ = nullptr;
    dev_mic_.clear();
#endif
  }

  void Audio(double now) {
    const std::string mode = REXCVAR_GET(bf_voice_mode);
    if (mode == "off" || !welcomed_) {
      CloseAudio();
      g_transmitting = false;
      std::lock_guard lock(g_mutex);
      g_speaking.clear();  // nothing is played: no one is shown talking either
      return;
    }
    OpenAudio();
#if defined(BF_DEV_TOOLS)
    static const bool always = Env("BF_DEV_VOICE_ALWAYS") != nullptr;
#else
    constexpr bool always = false;
#endif
    OpenMic(mode == "open" || g_ptt.load() || always, now);
    // Captured audio, 20 ms at a time.
    if (capture_) {
      int16_t tmp[1600];
      while (true) {
        const int got = SDL_GetAudioStreamData(capture_, tmp, sizeof(tmp));
        if (got <= 0) break;
        captured_.insert(captured_.end(), tmp, tmp + got / 2);
      }
    }
#if defined(BF_DEV_TOOLS)
    if (!dev_mic_.empty()) {
      while (dev_mic_clock_ + 0.02 <= now) {
        dev_mic_clock_ += 0.02;
        for (int i = 0; i < kFrame; ++i) captured_.push_back(dev_mic_[(dev_mic_at_ + i) % dev_mic_.size()]);
        dev_mic_at_ = (dev_mic_at_ + kFrame) % dev_mic_.size();
      }
    }
#endif
    while (captured_.size() >= size_t(kFrame)) {
      const int16_t* pcm = captured_.data();
      double sum = 0;
      for (int i = 0; i < kFrame; ++i) sum += double(pcm[i]) * pcm[i];
      const double rms_db = 10.0 * std::log10(sum / kFrame / (32768.0 * 32768.0) + 1e-12);
      // Open mic: from -30 dBFS at 0% sensitivity down to -60 dBFS at 100%, held 0.4 s after the last loud frame.
      const double gate = -30.0 - 0.3 * std::clamp(REXCVAR_GET(bf_voice_mic_sensitivity), 0.0, 100.0);
      if (rms_db > gate) open_until_ = now + 0.4;
      const bool keyed = mode == "open" ? now < open_until_ : g_ptt.load();
      const bool send = (keyed || always) && g_in_world.load() && g_cam_valid.load();
      g_transmitting = send;
      if (send) {
        Writer w(kVoice);
        w.u16(my_id_), w.u16(voice_seq_++);
        for (int i = 0; i < 3; ++i) w.f32(g_cam[i].load());
        const std::vector<uint8_t> frame = EncodeFrame(encoder_, pcm);
        w.u8(kCodecAdpcm), w.u16(uint16_t(frame.size()));
        w.bytes(frame);
        udp_.Send(w);
        ++frames_sent_;
      }
      captured_.erase(captured_.begin(), captured_.begin() + kFrame);
    }
    if (!g_transmitting.load() && captured_.size() > size_t(kFrame) * 10) captured_.clear();
    // Voices heard: mixed 20 ms at a time, about 60 ms ahead of the device.
    for (int k = 0; k < 6; ++k) {
      if (playback_) {
        const int queued = SDL_GetAudioStreamQueued(playback_);
        if (queued > kRate * 8 * 60 / 1000) break;  // 60 ms of stereo float queued
      } else {
        if (now < next_mix_) break;
        next_mix_ = std::max(next_mix_ + 0.02, now - 0.1);
      }
      MixFrame(now);
    }
    if (now - stats_at_ >= 30) {
      if (frames_sent_ || frames_heard_)
        REXLOG_INFO("BF comms: voice in the last {:.0f} s: {} frames sent, {} heard from {} players", now - stats_at_,
                    frames_sent_, frames_heard_, heard_from_.size());
      stats_at_ = now, frames_sent_ = frames_heard_ = 0, heard_from_.clear();
    }
    // Who is talking: only voices the player can hear (run zipcheck4: on the deploy screen, where the host passes on
    // every voice, the name of a player talking at the other base showed though it could not be heard; zipcheck6:
    // not any more).
    std::vector<std::string> speaking;
    for (auto& [id, t] : talkers_)
      if (now - t.last_packet < 0.3 && now - t.audible_at < 0.3 && !t.name.empty()) speaking.push_back(t.name);
    std::lock_guard lock(g_mutex);
    g_speaking = std::move(speaking);
  }

  void MixFrame(double now) {
    float mix[kFrame * 2] = {};
    const float cam[3] = {g_cam[0].load(), g_cam[1].load(), g_cam[2].load()};
    const float right[3] = {g_cam[3].load(), g_cam[4].load(), g_cam[5].load()};
    const float back[3] = {g_cam[6].load(), g_cam[7].load(), g_cam[8].load()};
    // Before the listener's own camera is known (loading in), no voice is placed anywhere: silence (comms7 played the
    // first 5 s centred at full volume, from a camera at 0, 0, 0).
    const bool cam_valid = g_cam_valid.load();
    const float volume = cam_valid ? float(std::clamp(REXCVAR_GET(bf_voice_volume), 0.0, 200.0) / 100.0) : 0.0f;
    const float range = float(std::clamp(REXCVAR_GET(bf_voice_range), 10.0, 200.0));
    for (auto it = talkers_.begin(); it != talkers_.end();) {
      Talker& t = it->second;
      if (!t.playing && t.frames.size() >= 2) t.playing = true, t.next = t.frames[t.Oldest()].first;  // 40 ms in hand
      if (t.playing && t.frames.empty() && now - t.last_packet > 0.2) t.playing = false;
      if (!t.playing) {
        if (now - t.last_packet > 30) it = talkers_.erase(it);
        else ++it;
        continue;
      }
      std::array<int16_t, kFrame> frame;
      if (!t.Take(frame)) {  // underrun or lost: silence for this frame
        ++it;
        continue;
      }
      float pos[3] = {t.pos[0], t.pos[1], t.pos[2]};
#if defined(BF_DEV_TOOLS)
      static const char* hear_at = Env("BF_DEV_VOICE_HEAR_AT");
      if (hear_at) {
        const std::string h = hear_at;
        const float metres = float(std::atof(h.substr(h.find(',') + 1).c_str()));
        const float sign = (h.rfind("left", 0) == 0 || h.rfind("front", 0) == 0) ? -1.0f : 1.0f;
        const float* axis = (h.rfind("right", 0) == 0 || h.rfind("left", 0) == 0) ? right : back;
        for (int i = 0; i < 3; ++i) pos[i] = cam[i] + sign * metres * axis[i];
      }
#endif
      const float d[3] = {pos[0] - cam[0], pos[1] - cam[1], pos[2] - cam[2]};
      const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      float gain = dist <= 4.0f ? 1.0f : std::max(0.0f, 1.0f - (dist - 4.0f) / (range - 4.0f));
      gain = gain * gain;
      if (cam_valid && gain > 0.0005f) t.audible_at = now;  // within hearing, whatever the voice volume
      gain *= volume;
      float pan = 0;  // -1 left, 1 right
      if (dist > 0.5f) pan = std::clamp((d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / dist, -1.0f, 1.0f);
      const float angle = (pan + 1.0f) * 0.25f * 3.14159265f;
      const float gl = gain * std::cos(angle) * 1.41421356f, gr = gain * std::sin(angle) * 1.41421356f;
      if (gain > 0.0005f) {
        for (int i = 0; i < kFrame; ++i) {
          const float s = frame[i] / 32768.0f;
          mix[i * 2] += s * gl * 0.7071f, mix[i * 2 + 1] += s * gr * 0.7071f;
        }
      }
      ++it;
    }
    for (float& v : mix) v = std::clamp(v, -1.0f, 1.0f);
    if (playback_) SDL_PutAudioStreamData(playback_, mix, sizeof(mix));
#if defined(BF_DEV_TOOLS)
    if (dev_out_) dev_out_->Write(mix, kFrame);
#endif
  }
};

void StartOnce() {
  std::call_once(g_started, [] {
    if (REXCVAR_GET(server)) return;  // a dedicated server has no voice or chat of its own
    std::thread([] { Engine().Run(); }).detach();
  });
}

}  // namespace

void NoteCamera(const float pos[3], const float right[3], const float back[3]) {
  for (int i = 0; i < 3; ++i) {
    g_cam[i].store(pos[i], std::memory_order_relaxed);
    g_cam[3 + i].store(right[i], std::memory_order_relaxed);
    g_cam[6 + i].store(back[i], std::memory_order_relaxed);
  }
  g_cam_valid.store(true, std::memory_order_release);
}
void NoteInWorld(bool in_world) { g_in_world.store(in_world, std::memory_order_relaxed); }
void NoteTeam(int team, int squad) {
  g_team.store(std::clamp(team, 0, 255), std::memory_order_relaxed);
  g_squad.store(std::clamp(squad, 0, 255), std::memory_order_relaxed);
}

// The local player's team and squad, from the game's own objects (found with BF_DEV_TEAMSCAN, dev_team_scan.cpp; runs
// comms2 and comms3 of 11 Nov 2026, two clients on opposite teams, before and after joining a squad): the player
// manager (the global at 0x82AF3AE4) holds the local player at +0xB8. That object starts with two vtables (0x8200C960,
// 0x8200C94C), then its name (begin, end, capacity words), its id at +0x2C, its team at +0x30 (1 or 2) and its squad at
// +0x34 (0 the first, 24 none). Read once this join's level has loaded (the deploy screen too: players chat there;
// run zipcheck3's team chat waited for a spawn) or while the soldier is in the world, so a new match never reports
// the last one's; kept while the player is dead.
void ReadTeam(const uint8_t* base, bool level_loaded) {
  if (!level_loaded && !g_in_world.load(std::memory_order_relaxed)) return;
  auto load = [base](uint32_t a) {
    const uint8_t* p = base + a;
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  };
  auto plausible = [](uint32_t a) { return a >= 0x00100000u && a < 0xFFF00000u && !(a & 3); };
  const uint32_t manager = load(0x82AF3AE4);
  if (!plausible(manager)) return;
  const uint32_t player = load(manager + 0xB8);
  if (!plausible(player) || load(player) != 0x8200C960u || load(player + 4) != 0x8200C94Cu) return;
  const uint32_t team = load(player + 0x30), squad = load(player + 0x34);
  if (team > 2 || squad > 24) return;
  const int t = int(team), q = squad == 24 ? 0 : int(squad) + 1;
  if (t != g_team.load(std::memory_order_relaxed) || q != g_squad.load(std::memory_order_relaxed)) {
    REXLOG_INFO("BF comms: team {}, squad {}", t, q ? std::to_string(q) : std::string("none"));
    NoteTeam(t, q);
  }
}
int Team() { return g_team.load(std::memory_order_relaxed); }
int Squad() { return g_squad.load(std::memory_order_relaxed); }
void NoteJoined(const std::string& address) {
  {
    std::lock_guard lock(g_mutex);
    g_joined = address;
  }
  NoteTeam(0, 0);  // the new match's are read once the soldier is in its world (ReadTeam)
  if (!address.empty()) StartOnce();
}

std::vector<ChatLine> RecentChat(size_t max) {
  std::lock_guard lock(g_mutex);
  std::vector<ChatLine> out;
  for (size_t i = g_chat.size() > max ? g_chat.size() - max : 0; i < g_chat.size(); ++i) out.push_back(g_chat[i]);
  return out;
}
void SendChat(Channel channel, const std::string& text) {
  std::lock_guard lock(g_mutex);
  if (g_outbox.size() < 8) g_outbox.emplace_back(channel, text);
}
std::vector<std::string> Speaking() {
  std::lock_guard lock(g_mutex);
  return g_speaking;
}
bool Transmitting() { return g_transmitting.load(); }
void SetPushToTalk(bool down) { g_ptt.store(down, std::memory_order_relaxed); }
void SetTyping(bool typing) {
  if (g_typing.exchange(typing) && !typing) g_typing_closed_at.store(NowS());
}
bool Typing() { return g_typing.load(); }
bool TypingOrJustClosed() { return g_typing.load() || NowS() - g_typing_closed_at.load() < 0.4; }
bool Connected() { return g_connected.load(); }
double Now() { return NowS(); }

}  // namespace bf1943::comms
