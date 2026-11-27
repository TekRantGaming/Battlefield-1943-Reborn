// The weather's sounds (the user, 17 Nov 2026: "I have my own audio files I want to use for the weather system"; the
// files ship in data\sounds\weather next to bf1943.exe): rain in three strengths cross-faded with the world's weather,
// the wind with its wind, a low rumble under a storm, and each lightning strike's thunder after the time its sound
// takes to come (343 m/s): a close strike's crack, a distant strike or a rumble in the clouds by how far it is.
//
// The world is NativeXRenderer's (CurrentWorldState: a match's server's, or the player's own settings) and the strikes
// are the sky's own (LightningStrikes: the same flashes the sky lights). The MP3s are decoded by the SDK's FFmpeg (its
// MP3 decoder and parser) into 48 kHz stereo, each loop with its end faded into its start so it repeats without a seam,
// and mixed into the game's own 5.1 as it goes to the system (audio_settings.cpp's XAudioSubmitRenderDriverFrame) at
// Master x Effects x Weather volume: the thunder placed round the speakers where its strike was (D185), the rain and
// the wind muffled under cover, the whole turned down for a moment while the game is loud (D187). Silent outside a
// match, and faded out while our menus mute the game. A loop is decoded the first time it is wanted and let go a
// minute after it is silent.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>  // the news window's storm: a sound stream of its own
#include <native_x_renderer/native_x_renderer.h>
#include <rex/cvar.h>

#include "capture_points.h"
#include "news_video.h"

namespace bf1943 {
extern std::atomic<float> g_sea_level;  // shader_prep.cpp: the level's water height (NaN: not known yet)
}  // namespace bf1943
#include <rex/logging.h>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/frame.h"
#include "libavutil/mem.h"
#include "libavutil/samplefmt.h"
}

REXCVAR_DEFINE_DOUBLE(bf_weather_volume, 100.0, "BF1943/Audio",
                      "How loud the weather's sounds are (rain, wind, thunder), 0 to 100 (%)");
REXCVAR_DECLARE(double, bf_master_volume);   // audio_settings.cpp
REXCVAR_DECLARE(double, bf_effects_volume);  // game_options.cpp

namespace bf1943 {
bool InMatch();                                // auto_join.cpp
extern std::atomic<bool> g_game_audio_muted;   // audio_settings.cpp: our menus mute the game
std::filesystem::path ExecutableDir();         // shader_prep.cpp

namespace {
namespace fs = std::filesystem;
constexpr int kRate = 48000;

// Between two rounds (TekRantGaming, 20 Nov 2026: after a full storm "the game would end and load the next match ... you
// could hear the storm. And then once you were in, you could actually see the storm, and then the clouds would begin to
// retreat"): from a round's end (WeatherRoundOver) or the next level's load (WeatherLevelLoading) until that level is
// in (WeatherLevelIn) the weather is silent; the steady clock's ticks since then, 0 while a round runs.
std::atomic<int64_t> g_between_since{0};
// D221's window: once the next level is in, the match's world is taken at once (not eased) this long, so a host that
// says the new round's weather a few seconds after the load (it reads the round from its server each second) still
// shows its first frame clear.
constexpr double kNewRoundSettle = 20.0;
// A round's end with no next level after this long (a server that stays on its end of round screen): the weather's
// sounds again.
constexpr double kBetweenAtMost = 180.0;

int64_t SteadyTicks() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

#if defined(BF_DEV_TOOLS)
// BF_DEV_WEATHER_SOUND_LOG=1 (tests, which run with the volume at 0): the weather mixed at full volume (the master,
// effects and weather volumes taken as 100) but never given to the speakers: logged each second, and recorded to
// BF_DEV_WEATHER_SOUND_OUT's file at its real level, while the PC stays silent.
const bool g_test_sound = [] {
  const char* v = std::getenv("BF_DEV_WEATHER_SOUND_LOG");
  return v && *v == '1';
}();
#else
constexpr bool g_test_sound = false;
#endif

// The sounds, by the user's file names.
enum Sound {
  kRainSoft, kRainMedium, kRainStorm, kWindHurricane, kWindTornado, kThunderDistant,  // loops
  kStrikeClose, kStrikeDistant, kStrikeClouds,                                       // one per strike
  kSoundCount
};
constexpr const char* kFiles[kSoundCount] = {"Rain_soft.mp3",      "Rain_medium.mp3",          "Rain_storm.mp3",
                                             "Wind_hurricane.mp3", "Wind_tornado.mp3",         "Thunder_distant.mp3",
                                             "Lightning_strike-close.mp3", "Lightning_strike-distant.mp3",
                                             "Lightning_clouds.mp3"};
constexpr int kLoops = kThunderDistant + 1;

// A decoded sound: 48 kHz stereo, 16 bit (a storm's loops together are about 80 MB this way).
struct Wave {
  std::vector<int16_t> lr;
  size_t frames() const { return lr.size() / 2; }
};

// The MP3 at `path` as 48 kHz stereo (linear resampling: rain and wind are noise, and the files are 44.1 or 48 kHz).
bool DecodeMp3(const fs::path& path, Wave& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (file.size() < 128) return false;
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MP3);
  AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_MP3);
  AVCodecContext* ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  std::vector<float> lr;  // at the file's own rate
  int rate = 0;
  bool ok = parser && ctx && packet && frame && avcodec_open2(ctx, codec, nullptr) == 0;
  auto take = [&] {
    while (avcodec_receive_frame(ctx, frame) == 0) {
      rate = frame->sample_rate;
      const int ch = std::max(1, frame->channels);
      const auto fmt = AVSampleFormat(frame->format);
      for (int i = 0; i < frame->nb_samples; ++i)
        for (int c = 0; c < 2; ++c) {
          const int k = std::min(c, ch - 1);
          float v = 0.0f;
          if (fmt == AV_SAMPLE_FMT_FLTP) v = reinterpret_cast<const float*>(frame->extended_data[k])[i];
          else if (fmt == AV_SAMPLE_FMT_FLT) v = reinterpret_cast<const float*>(frame->extended_data[0])[i * ch + k];
          else if (fmt == AV_SAMPLE_FMT_S16P) v = reinterpret_cast<const int16_t*>(frame->extended_data[k])[i] / 32768.0f;
          else if (fmt == AV_SAMPLE_FMT_S16) v = reinterpret_cast<const int16_t*>(frame->extended_data[0])[i * ch + k] / 32768.0f;
          lr.push_back(v);
        }
    }
  };
  if (ok) {
    // The padded input FFmpeg's parser reads past the end of.
    file.resize(file.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    const uint8_t* data = file.data();
    int left = int(file.size() - AV_INPUT_BUFFER_PADDING_SIZE);
    while (left > 0) {
      uint8_t* pkt = nullptr;
      int pkt_size = 0;
      const int used = av_parser_parse2(parser, ctx, &pkt, &pkt_size, data, left, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
      if (used < 0) break;
      data += used, left -= used;
      if (pkt_size > 0) {
        packet->data = pkt, packet->size = pkt_size;
        if (avcodec_send_packet(ctx, packet) == 0) take();
      }
      if (used == 0 && pkt_size == 0) break;
    }
    uint8_t* pkt = nullptr;
    int pkt_size = 0;
    av_parser_parse2(parser, ctx, &pkt, &pkt_size, nullptr, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
    if (pkt_size > 0) {
      packet->data = pkt, packet->size = pkt_size;
      if (avcodec_send_packet(ctx, packet) == 0) take();
    }
    avcodec_send_packet(ctx, nullptr);
    take();
  }
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&ctx);
  if (parser) av_parser_close(parser);
  if (!ok || rate <= 0 || lr.size() < 4) return false;
  const size_t frames = lr.size() / 2;
  const size_t out_frames = size_t(double(frames) * kRate / rate);
  out.lr.resize(out_frames * 2);
  for (size_t i = 0; i < out_frames; ++i) {
    const double src = double(i) * rate / kRate;
    const size_t i0 = std::min(size_t(src), frames - 1), i1 = std::min(i0 + 1, frames - 1);
    const float t = float(src - double(i0));
    for (int c = 0; c < 2; ++c) {
      const float v = lr[i0 * 2 + c] * (1.0f - t) + lr[i1 * 2 + c] * t;
      out.lr[i * 2 + c] = int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
    }
  }
  return true;
}

// A loop's end faded into its start (equal power, over two seconds or a tenth of it), the faded end cut: played round
// and round it has no seam.
void MakeLoop(Wave& w) {
  const size_t frames = w.frames();
  const size_t fade = std::min<size_t>(kRate * 2, frames / 10);
  if (fade < 64) return;
  const size_t keep = frames - fade;
  for (size_t i = 0; i < fade; ++i) {
    const float t = (float(i) + 0.5f) / float(fade);
    const float in = std::sin(t * 1.5707963f), out = std::cos(t * 1.5707963f);
    for (int c = 0; c < 2; ++c) {
      const float a = w.lr[i * 2 + c], b = w.lr[(keep + i) * 2 + c];
      w.lr[i * 2 + c] = int16_t(std::clamp(std::lround(a * in + b * out), -32768L, 32767L));
    }
  }
  w.lr.resize(keep * 2);
}

struct Loop {
  float gain = 0.0f;     // now (moved toward `target` a little each sample)
  float target = 0.0f;   // what the weather asks for
  size_t at = 0;         // its place, frames
  double silent_since = -1.0;
  float low[2] = {};     // its muffling's low pass (the camera under cover)
};
struct Shot {
  int sound = 0;
  int64_t wait = 0;      // frames before it starts (the thunder's way to the player)
  size_t at = 0;
  float gain = 1.0f;
  // Where the sky flashed (world metres, x and z), when the camera was known: the thunder comes from there, panned by
  // where the camera faces as it plays. Unplaced: from all round, as the file is.
  bool placed = false;
  float x = 0.0f, z = 0.0f;
  bool begun = false;
  float pan[4] = {};     // its gains now in the front left, front right, back left and back right speakers
  float cut = 1.0f;      // its low pass now (behind the camera, or under cover, duller)
  float low[2] = {};
};

struct Mixer {
  std::mutex mutex;
  std::array<std::shared_ptr<const Wave>, kSoundCount> waves{};
  std::array<bool, kSoundCount> loading{}, missing{};
  std::array<Loop, kLoops> loops{};
  std::vector<Shot> shots;
  float volume = 0.0f, volume_target = 0.0f;  // Master x Effects x Weather, and the match and the menus' fade
  float shelter = 0.0f, shelter_target = 0.0f;  // the camera under cover (0 to 1): the rain and the wind muffled
  // The game's loud moments: how far the weather is turned down now (dB), the game's usual loudness (dB, followed
  // over some seconds) and how long the turn down is held before it lets go.
  float duck_db = 0.0f, usual_db = -200.0f, game_power = 0.0f;  // (-200: none yet)
  float duck_hold = 0.0f;
  // The rain and the wind's own loudness as the game's is measured (dB, over about 60 ms, before the turn down), and
  // the master volume (the game's loudness is taken before it, the weather's after it).
  float bed_power = 0.0f, bed_db = -200.0f, master = 1.0f;
  fs::path dir;
#if defined(BF_DEV_TOOLS)
  // BF_DEV_WEATHER_SOUND_OUT=<file.wav> (tests): the weather's part of the game's sound also written to that file (as
  // a stereo device hears it, the 5.1 folded), 48 kHz stereo 16 bit, from the first sound on.
  std::string out_path;
  std::vector<int16_t> out;
  float duck_most = 0.0f, duck_game = -99.0f;  // the most it was turned down since the last log line, and the game then
  double duck_said = 0.0;
#endif
};
Mixer g_mix;
std::atomic<bool> g_started{false};

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// The game's 5.1 (its frames' order: front left, front right, centre, LFE, back left, back right).
constexpr int kFL = 0, kFR = 1, kBL = 4, kBR = 5;

// A bearing's gains in the four corner speakers (front left, front right, back left, back right): equal power between
// the two either side of it, the speakers at -30, 30, -110 and 110 degrees. `bearing` in radians from the camera's
// facing, to the right positive.
void QuadPan(float bearing, float g[4]) {
  float deg = bearing * 57.29578f + 30.0f;
  deg = std::fmod(deg, 360.0f);
  if (deg < 0.0f) deg += 360.0f;
  deg -= 30.0f;  // -30 to 330, round from the front left
  struct Pair { int a, b; float from, to; };
  // (Indices: 0 front left, 1 front right, 2 back left, 3 back right.)
  constexpr Pair pairs[4] = {{0, 1, -30.0f, 30.0f}, {1, 3, 30.0f, 110.0f}, {3, 2, 110.0f, 250.0f}, {2, 0, 250.0f, 330.0f}};
  g[0] = g[1] = g[2] = g[3] = 0.0f;
  for (const Pair& p : pairs)
    if (deg >= p.from && deg < p.to) {
      const float t = (deg - p.from) / (p.to - p.from) * 1.5707963f;
      g[p.a] = std::cos(t), g[p.b] = std::sin(t);
      return;
    }
  g[0] = g[1] = 0.7071068f;
}

// The game's sound frame (WeatherAudioFrame): the loops at their gains and the strikes due, added into its 5.1. The
// file's own stereo: left in the front left and, softer, the back left; so the right. A strike's thunder is panned
// round the four corners by its bearing, part of the file's stereo kept. Under cover the rain and the wind are
// quieter and duller; while the game is loud for a moment (an explosion, a shell, gunfire near) the whole is turned
// down and comes back over a second or so.
void MixInto(float* frame, int n, float game_db) {
  std::lock_guard lock(g_mix.mutex);
  if (g_mix.volume <= 0.0f && g_mix.volume_target <= 0.0f && !g_test_sound) return;  // (tests: the silence recorded too)
  float add[6][256] = {};   // the rain, the wind and the far rumble
  float shot[6][256] = {};  // the strikes' thunder (turned down less in the game's loud moments)
  n = std::min(n, 256);
  const float dt = float(n) / kRate;

  // The game's loud moments: its loudness over the last 60 ms or so (one frame's 5 ms alone rose and fell with every
  // sound's peaks, and the weather pumped by 4 dB all the time: run w51_a) over its usual (from the first frame, rising
  // over 10 s, falling over 1.5 s; never taken under -50 dB). D207 (19 Nov 2026, the user: "Rain and wind completely
  // drown out gunfire and explosions almost entirely"): 3 dB over the usual and more turns the rain and the wind down
  // by a dB a dB, and in a loud moment they are also turned down until the game stands 12 dB over them; 18 dB at the
  // most; at once (12 ms), held a quarter second, let go at 10 dB a second. The usual rose over 3 s before, so a
  // firefight soon became the usual and the weather came back over it. The thunder is turned down by 3 dB at most.
  {
    float& power = g_mix.game_power;
    power += (std::pow(10.0f, game_db / 10.0f) - power) * (1.0f - std::exp(-dt / 0.06f));
    const float now_db = 10.0f * std::log10(power + 1e-12f);
    float& usual = g_mix.usual_db;
    if (usual < -199.0f) usual = now_db;
    usual += (now_db - usual) * std::min(1.0f, dt / (now_db > usual ? 10.0f : 1.5f));
    game_db = now_db;
    const float over = now_db - std::max(usual, -50.0f);
    float want = std::clamp(over - 3.0f, 0.0f, 18.0f);
    // The weather's loudness as the game's is taken (before the master volume).
    const float bed_cmp = g_mix.bed_db - 20.0f * std::log10(std::max(g_mix.master, 1e-3f));
    if (over > 2.0f && g_mix.bed_db > -150.0f) want = std::max(want, std::clamp(bed_cmp + 12.0f - now_db, 0.0f, 18.0f));
    if (want >= g_mix.duck_db) {
      g_mix.duck_db += (want - g_mix.duck_db) * (1.0f - std::exp(-dt / 0.012f));
      g_mix.duck_hold = 0.25f;
    } else if (g_mix.duck_hold > 0.0f) {
      g_mix.duck_hold -= dt;
    } else {
      g_mix.duck_db = std::max(want, g_mix.duck_db - 10.0f * dt);
    }
#if defined(BF_DEV_TOOLS)
    if (g_mix.duck_db > g_mix.duck_most) g_mix.duck_most = g_mix.duck_db, g_mix.duck_game = game_db;
#endif
  }
  g_mix.shelter += std::clamp(g_mix.shelter_target - g_mix.shelter, -dt / 0.8f, dt / 0.8f);
  const float shelter = g_mix.shelter;

  // About 1.5 s from silent to full for a loop.
  const float loop_step = 1.0f / (1.5f * kRate);
  for (int l = 0; l < kLoops; ++l) {
    Loop& p = g_mix.loops[l];
    const Wave* w = g_mix.waves[l].get();
    if (!w || w->frames() == 0) continue;
    if (p.gain <= 0.0f && p.target <= 0.0f) continue;
    // Under cover: the rain most muffled, the wind less, the far rumble least (to about 1.2 kHz, 2 kHz and 5 kHz).
    const float m = shelter * (l <= kRainStorm ? 1.0f : l <= kWindTornado ? 0.75f : 0.45f);
    const float cut = 1.0f - 0.85f * m, keep = 1.0f - 0.45f * m;
    const size_t frames = w->frames();
    for (int i = 0; i < n; ++i) {
      p.gain += std::clamp(p.target - p.gain, -loop_step, loop_step);
      if (p.at >= frames) p.at = 0;
      const float g = p.gain * p.gain * keep / 32768.0f;  // the gain squared: a fade sounds even to the ear
      p.low[0] += (w->lr[p.at * 2] - p.low[0]) * cut;
      p.low[1] += (w->lr[p.at * 2 + 1] - p.low[1]) * cut;
      add[kFL][i] += p.low[0] * g, add[kFR][i] += p.low[1] * g;
      add[kBL][i] += p.low[0] * g * 0.7f, add[kBR][i] += p.low[1] * g * 0.7f;
      ++p.at;
    }
  }

  // The camera now, for the thunder's direction.
  float eye[3], view_right[3], view_forward[3];
  double age = 1e9;
  const bool view = native_x_renderer::ListenerView(eye, view_right, view_forward, &age) && age < 1.0;
  for (Shot& s : g_mix.shots) {
    const Wave* w = g_mix.waves[s.sound].get();
    if (!w) continue;
    int i = 0;
    if (s.wait > 0) {
      const int64_t skip = std::min<int64_t>(s.wait, n);
      s.wait -= skip, i = int(skip);
    }
    if (i >= n) continue;
    // Its direction: behind the camera a little quieter and duller (a low pass to about 3.5 kHz straight behind),
    // which tells front from back on a stereo device too. Unplaced: front and back alike.
    float want[4] = {0.5f, 0.5f, 0.5f, 0.5f}, want_cut = 1.0f;
    if (s.placed && view) {
      const float dx = s.x - eye[0], dz = s.z - eye[2];
      const float rl = std::hypot(view_right[0], view_right[2]), fl = std::hypot(view_forward[0], view_forward[2]);
      if (std::hypot(dx, dz) > 1.0f && rl > 1e-3f && fl > 1e-3f) {
        const float side = (dx * view_right[0] + dz * view_right[2]) / rl;
        const float front = (dx * view_forward[0] + dz * view_forward[2]) / fl;
        QuadPan(std::atan2(side, front), want);
        const float behind = std::clamp(-front / std::max(1.0f, std::hypot(side, front)), 0.0f, 1.0f);
        for (float& g : want) g *= 1.0f - 0.2f * behind;
        want_cut = 1.0f - 0.63f * behind;
      }
    }
    want_cut *= 1.0f - 0.4f * shelter;  // under cover, the thunder through the walls
    if (!s.begun) {
      s.begun = true;
      for (int k = 0; k < 4; ++k) s.pan[k] = want[k];
      s.cut = want_cut;
    }
    // How much of the file's own stereo is kept: a close crack's little (a point), the clouds' rumble more (it rolls
    // across the sky). The rest, one sound, panned (twice as loud: on a stereo device the corners fold to about 0.4).
    const float spread = s.sound == kStrikeClose ? 0.2f : s.sound == kStrikeDistant ? 0.35f : 0.55f;
    const float step = 1.0f / (0.05f * kRate);  // 50 ms to follow the camera's turn
    const float g = s.gain * (1.0f - 0.2f * shelter) / 32768.0f;
    for (; i < n && s.at < w->frames(); ++i, ++s.at) {
      for (int k = 0; k < 4; ++k) s.pan[k] += std::clamp(want[k] - s.pan[k], -step, step);
      s.cut += std::clamp(want_cut - s.cut, -step, step);
      s.low[0] += (w->lr[s.at * 2] * g - s.low[0]) * s.cut;
      s.low[1] += (w->lr[s.at * 2 + 1] * g - s.low[1]) * s.cut;
      const float one = 0.5f * (s.low[0] + s.low[1]) * (1.0f - spread) * 2.0f;
      shot[kFL][i] += spread * s.low[0] + one * s.pan[0];
      shot[kFR][i] += spread * s.low[1] + one * s.pan[1];
      shot[kBL][i] += spread * s.low[0] * 0.7f + one * s.pan[2];
      shot[kBR][i] += spread * s.low[1] * 0.7f + one * s.pan[3];
    }
  }
  std::erase_if(g_mix.shots, [](const Shot& s) {
    const Wave* w = g_mix.waves[s.sound].get();
    return !w || (s.wait <= 0 && s.at >= w->frames());
  });

  // The whole: its volume (0.5 s from silent to full), the game's loud moments, a soft limit (thunder over a storm's
  // rain never clips), into the game's frame.
  const float mix_step = 1.0f / (0.5f * kRate);
  // D207, the levels against the game's own (run wsnd1, Guadalcanal at 100%, everything at full volume): the game's
  // gunfire measured about -30 dB RMS and its quiet -46 to -58, while the storm's rain and wind stood at -20 and a
  // strike's thunder 781 m off at -13.5 over them (peaking at the limit), a distant one not over them at all. The rain,
  // the wind and the far rumble are taken 18 dB down (a storm about -38, under the gunfire), the strikes 6 dB (the
  // close crack well over the gunfire, every strike well over the rain).
  constexpr float kBedTrim = 0.1259f;    // -18 dB
  constexpr float kThunderTrim = 0.5012f;  // -6 dB
  const float duck = std::pow(10.0f, -g_mix.duck_db / 20.0f);
  const float duck_shot = std::pow(10.0f, -std::min(g_mix.duck_db, 3.0f) / 20.0f);
  static float last_duck = 1.0f, last_duck_shot = 1.0f;
  double bed_sum = 0.0;
  for (int i = 0; i < n; ++i) {
    g_mix.volume += std::clamp(g_mix.volume_target - g_mix.volume, -mix_step, mix_step);
    const float t = float(i + 1) / float(n), vv = g_mix.volume * g_mix.volume * kBedTrim;
    const float v = vv * (last_duck + (duck - last_duck) * t);
    const float vs = g_mix.volume * g_mix.volume * kThunderTrim * (last_duck_shot + (duck_shot - last_duck_shot) * t);
    for (int c : {kFL, kFR, kBL, kBR}) {
      bed_sum += double(add[c][i] * vv) * (add[c][i] * vv);
      const float x = add[c][i] * v + shot[c][i] * vs;
      add[c][i] = x / (1.0f + 0.15f * std::abs(x));
      if (!g_test_sound) frame[c * 256 + i] += add[c][i];
    }
  }
  last_duck = duck, last_duck_shot = duck_shot;
  {  // the rain and the wind's loudness, before the turn down (RMS of the four speakers', over about 60 ms)
    const float p = float(bed_sum / std::max(1, 4 * n));
    g_mix.bed_power += (p - g_mix.bed_power) * (1.0f - std::exp(-dt / 0.06f));
    g_mix.bed_db = g_mix.bed_power > 1e-14f ? 10.0f * std::log10(g_mix.bed_power) : -200.0f;
  }
#if defined(BF_DEV_TOOLS)
  if (!g_mix.out_path.empty())
    for (int i = 0; i < n; ++i) {
      // As SDL's stereo fold (sdk audio/downmix.h): the front, the back at 0.707, scaled by 0.586.
      const float l = 0.5857864f * (add[kFL][i] + 0.7071068f * add[kBL][i]);
      const float r = 0.5857864f * (add[kFR][i] + 0.7071068f * add[kBR][i]);
      g_mix.out.push_back(int16_t(std::lround(std::clamp(l, -1.0f, 1.0f) * 32767.0f)));
      g_mix.out.push_back(int16_t(std::lround(std::clamp(r, -1.0f, 1.0f) * 32767.0f)));
    }
#endif
}

// The sound's wave, decoded on a thread of its own the first time it is asked for (null until then, or without its
// file).
std::shared_ptr<const Wave> Want(int sound) {
  std::lock_guard lock(g_mix.mutex);
  if (g_mix.waves[sound] || g_mix.loading[sound] || g_mix.missing[sound]) return g_mix.waves[sound];
  g_mix.loading[sound] = true;
  const fs::path path = g_mix.dir / kFiles[sound];
  std::thread([sound, path] {
    auto w = std::make_shared<Wave>();
    const auto from = std::chrono::steady_clock::now();
    const bool ok = DecodeMp3(path, *w);
    if (ok && sound < kLoops) MakeLoop(*w);
    std::lock_guard lock(g_mix.mutex);
    g_mix.loading[sound] = false;
    if (!ok) {
      g_mix.missing[sound] = true;
      REXLOG_WARN("BF weather sounds: {} could not be read: that sound is silent", path.string());
      return;
    }
    g_mix.waves[sound] = std::move(w);
    REXLOG_INFO("BF weather sounds: {} ready ({:.1f} s, {} ms to decode)", kFiles[sound],
                double(g_mix.waves[sound]->frames()) / kRate,
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from).count());
  }).detach();
  return nullptr;
}


float Smooth(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

#if defined(BF_DEV_TOOLS)
void WriteOut() {
  std::vector<int16_t> copy;
  std::string path;
  {
    std::lock_guard lock(g_mix.mutex);
    copy = g_mix.out, path = g_mix.out_path;
  }
  if (path.empty() || copy.empty()) return;
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  const uint32_t bytes = uint32_t(copy.size() * 2);
  f.write("RIFF", 4), u32(36 + bytes), f.write("WAVEfmt ", 8), u32(16), u16(1), u16(2), u32(kRate), u32(kRate * 4),
      u16(4), u16(16), f.write("data", 4), u32(bytes);
  f.write(reinterpret_cast<const char*>(copy.data()), std::streamsize(bytes));
}
#endif

// D208: the hurricane's aim and the strikes' targets, twice a second: the level's middle (its capture points'), and the
// places two or more players stand within 120 m of each other (the camera's player among them), each group's middle
// rounded to 50 m and the groups in order, so every player's game takes the same ones (the strikes are worked out on
// each game from the world's clock).
void TellStormWhere() {
  const std::vector<CapturePoint> points = CapturePoints();
  if (!points.empty()) {
    double x = 0, z = 0;
    for (const CapturePoint& c : points) x += c.x, z += c.z;
    x /= points.size(), z /= points.size();
    // D213: the level's reach, the hurricane's size: its points' farthest from their middle, and 300 m.
    double reach = 0;
    for (const CapturePoint& c : points) reach = std::max(reach, std::hypot(c.x - x, c.z - z));
    native_x_renderer::SetMapArea(float(x), float(z), float(reach + 300.0));
  }
  std::vector<std::array<float, 2>> at;
  for (const MapPlayer& m : MapPlayers()) at.push_back({m.x, m.z});
  float eye[3], right[3], forward[3];
  double age = 1e9;
  if (native_x_renderer::ListenerView(eye, right, forward, &age) && age < 2.0) at.push_back({eye[0], eye[2]});
  std::vector<float> groups;
  std::vector<bool> taken(at.size(), false);
  for (size_t i = 0; i < at.size(); ++i) {
    if (taken[i]) continue;
    double sx = 0, sz = 0;
    int n = 0;
    for (size_t j = i; j < at.size(); ++j) {
      if (taken[j] || std::hypot(at[j][0] - at[i][0], at[j][1] - at[i][1]) > 120.0f) continue;
      taken[j] = true, sx += at[j][0], sz += at[j][1], ++n;
    }
    if (n >= 2) {
      groups.push_back(float(std::round(sx / n / 50.0) * 50.0));
      groups.push_back(float(std::round(sz / n / 50.0) * 50.0));
    }
  }
  std::vector<std::array<float, 2>> sorted;
  for (size_t i = 0; i + 1 < groups.size(); i += 2) sorted.push_back({groups[i], groups[i + 1]});
  std::sort(sorted.begin(), sorted.end());
  groups.clear();
  for (const auto& g : sorted) groups.push_back(g[0]), groups.push_back(g[1]);
  native_x_renderer::SetPlayerGroups(groups.data(), uint32_t(std::min<size_t>(sorted.size(), 8)));
}

// Ten times a second: what the weather asks of each loop, and the strikes whose thunder is on its way.
void Run() {
  g_mix.dir = ExecutableDir() / "data" / "sounds" / "weather";
  std::error_code ec;
  if (!fs::is_directory(g_mix.dir, ec)) {
    REXLOG_INFO("BF weather sounds: no {} : no weather sounds", g_mix.dir.string());
    return;
  }
  REXLOG_INFO("BF weather sounds: from {}, mixed into the game's own 5.1", g_mix.dir.string());
#if defined(BF_DEV_TOOLS)
  if (const char* v = std::getenv("BF_DEV_WEATHER_SOUND_OUT"); v && *v) {
    std::lock_guard lock(g_mix.mutex);
    g_mix.out_path = v;
  }
#endif
  double last_clock = -1.0;
  double said_at = 0.0;
#if defined(BF_DEV_TOOLS)
  double written_at = 0.0;
#endif
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const double now = Now();
    // Between two rounds (g_between_since): silent, and the thunder still on its way dropped.
    bool between = false;
    if (const int64_t since = g_between_since.load(std::memory_order_relaxed); since != 0) {
      const double for_s = std::chrono::duration<double>(std::chrono::steady_clock::duration(SteadyTicks() - since)).count();
      if (!InMatch() || for_s > kBetweenAtMost) {
        g_between_since.store(0, std::memory_order_relaxed);
        REXLOG_INFO("BF weather sounds: {} (between two rounds for {:.0f} s): the weather heard again",
                    InMatch() ? "no next level came" : "the match was left", for_s);
      } else {
        between = true;
      }
    }
    static bool was_between = false;
    if (between && !was_between) {
      std::lock_guard lock(g_mix.mutex);
      g_mix.shots.clear();
    }
    was_between = between;
    const bool playing = InMatch() && !between;
    static double told_at = 0.0;
    if (now - told_at > 0.5) told_at = now, TellStormWhere();  // D208
    const native_x_renderer::WorldState w = native_x_renderer::CurrentWorldState();
    const float volume = g_test_sound ? 1.0f
                                      : float(std::clamp(REXCVAR_GET(bf_master_volume), 0.0, 100.0) / 100.0 *
                                              std::clamp(REXCVAR_GET(bf_effects_volume), 0.0, 100.0) / 100.0 *
                                              std::clamp(REXCVAR_GET(bf_weather_volume), 0.0, 100.0) / 100.0);
    const bool muted = g_game_audio_muted.load(std::memory_order_relaxed);
    // What each loop is asked for (0 to 1, before the volume). Rain falls from 55% of the weather (sky.cpp) and is
    // full at 95%: soft, medium and storm rain cross-fade along it. The wind follows the world's wind: the hurricane
    // loop from a fresh breeze, the tornado over it in a gale. The distant rumble under a storm (the lightning's own
    // range, 60 to 95%).
    std::array<float, kLoops> want{};
    if (playing) {
      // D212 (the user, 19 Nov 2026: "the rain audio should only be full loud when on the ground, and quiet above the
      // clouds. distant thunder and lighting can still be heard though"): over the cloud base (its height over the
      // level's water) the rain falls away to 15% by 200 m over it; under the base, full. Thunder as before.
      float above = 0.0f;
      {
        float eye[3], right[3], forward[3];
        double age = 1e9;
        const float sea = g_sea_level.load(std::memory_order_relaxed);
        if (native_x_renderer::ListenerView(eye, right, forward, &age) && age < 2.0 && std::isfinite(sea))
          above = Smooth(w.cloud_height * 0.85f, w.cloud_height + 200.0f, eye[1] - sea);
      }
      const float rain = std::clamp((w.weather - 0.55f) / 0.40f, 0.0f, 1.0f);
      if (rain > 0.0f) {
        want[kRainSoft] = 0.85f * (1.0f - Smooth(0.30f, 0.60f, rain)) * Smooth(0.0f, 0.12f, rain);
        want[kRainMedium] = 0.85f * Smooth(0.20f, 0.50f, rain) * (1.0f - Smooth(0.65f, 0.95f, rain));
        want[kRainStorm] = 0.90f * Smooth(0.60f, 0.90f, rain);
        for (int l : {kRainSoft, kRainMedium, kRainStorm}) want[l] *= std::sqrt(1.0f - 0.85f * above);  // (squared after)
      }
      const float tornado = Smooth(0.80f, 1.0f, w.wind);
      want[kWindHurricane] = 0.70f * Smooth(0.15f, 0.85f, w.wind) * (1.0f - 0.5f * tornado);
      want[kWindTornado] = 0.65f * tornado;
      // D207: the far rumble 6 dB over the rain and the wind (0.55 before; the gain is squared).
      want[kThunderDistant] = 0.78f * Smooth(0.60f, 0.95f, std::max(w.weather, 0.85f * w.storm));  // D208: heard from outside it too
    }
    const bool any = std::any_of(want.begin(), want.end(), [](float v) { return v > 0.0f; });
    (void)any;
    // Under cover (the renderer's look about twice a second: most of five places round the camera under something),
    // while it rains.
    double cover_age = 1e9;
    const bool covered = native_x_renderer::EyeCovered(&cover_age) && cover_age < 2.0;
    static bool said_covered = false;
    if (covered != said_covered) {
      said_covered = covered;
      REXLOG_INFO("BF weather sounds: the camera is {} (the rain and the wind {})", covered ? "under cover" : "in the open",
                  covered ? "muffled" : "as they are");
    }
    for (int l = 0; l < kLoops; ++l)
      if (want[l] > 0.0f) Want(l);
    // The strikes since the last look on the world's clock (a jump of more than five seconds, as on joining or an
    // admin's change, starts afresh); each one's thunder comes from where the sky flashed (sky.cpp lights it at the
    // middle of the eye's 4 km square and the strike's offset from there) and waits as long as its sound takes to
    // come from there to the camera. Without a camera known: from the square's middle, both sides alike.
    std::vector<Shot> shots;
    // D208: the strikes are the match's storm's (they fall only under it, wherever the camera is).
    if (playing && std::max(w.storm, w.weather) >= 0.6f) {
      const double clock = w.cloud_seconds;
      if (last_clock < 0.0 || clock < last_clock || clock - last_clock > 5.0) last_clock = clock;
      native_x_renderer::LightningStrike strikes[8];
      const uint32_t n = native_x_renderer::LightningStrikes(last_clock, clock, std::max(w.storm, w.weather), strikes, 8);
      last_clock = clock;
      float eye[3], view_right[3], view_forward[3];
      double age = 1e9;
      const bool view = native_x_renderer::ListenerView(eye, view_right, view_forward, &age) && age < 1.0;
      for (uint32_t i = 0; i < n; ++i) {
        Shot s;
        float dx = strikes[i].x, dz = strikes[i].z;
        if (view) {
          s.placed = true;
          s.x = std::round(eye[0] / 4000.0f) * 4000.0f + strikes[i].x;
          s.z = std::round(eye[2] / 4000.0f) * 4000.0f + strikes[i].z;
          dx = s.x - eye[0], dz = s.z - eye[2];
        }
        const float distance = std::max(150.0f, std::hypot(dx, dz));
        // D225 (the user, 20 Nov 2026: "why is none of the other audio variants playing for distant thunder or
        // lightning too? I only hear the same lightning audio, and its the one meant for lightning strikes being
        // extremely close"): the storm the level's size (D213) puts every strike within about 1.5 km, where the
        // thresholds of 17 Nov (1.5 and 5 km) gave every one the close crack. Now by what it is and how far: a bolt
        // within 350 m the close crack, a bolt further off the distant strike, a flash inside the clouds their rumble.
        s.sound = !strikes[i].bolt ? kStrikeClouds : distance < 350.0f ? kStrikeClose : kStrikeDistant;
        s.wait = int64_t(double(distance) / 343.0 * kRate);
        // D207 (the user: "Lightning and thunder can barely be heard too, and lightning should be relatively loud,
        // especially if close"): a close strike's crack up to 2.2 at 150 m, falling to 1.6 at 1.5 km; a distant one's
        // from 1.9 to 1.0 at 5 km; the clouds' rumble 0.4 to 1.2 (run wsnd2: a distant one at 1.3 stood only 1 dB over a
        // storm's rain). (Before: 1.0 at the most, and turned down with the
        // rest in the game's loud moments.)
        const float st = strikes[i].strength;
        // D208: near the players a startling crack, 3.5 at 150 m to 2.8 at 350 m (D225). A distant strike 2.2 at 350 m
        // to 0.8 at 3 km; the clouds' rumble 1.5 overhead to 0.5 at 3 km.
        s.gain = s.sound == kStrikeClose     ? std::clamp(st * (3.5f - 0.7f * (distance - 150.0f) / 200.0f), 1.6f, 3.5f)
                 : s.sound == kStrikeDistant ? std::clamp(st * (2.2f - 1.4f * (distance - 350.0f) / 2650.0f), 0.8f, 2.2f)
                                             : std::clamp(st * (1.5f - distance / 3000.0f), 0.5f, 1.5f);
        if (Want(s.sound)) shots.push_back(s);
#if defined(BF_DEV_TOOLS)
        constexpr double kSayEvery = 0.0;  // tests: every strike
#else
        constexpr double kSayEvery = 30.0;
#endif
        if (now - said_at >= kSayEvery) {
          said_at = now;
          // Its bearing from the camera's facing (degrees, to the right positive), when placed.
          float bearing = 0.0f;
          if (view) {
            const float side = dx * view_right[0] + dz * view_right[2];
            const float front = dx * view_forward[0] + dz * view_forward[2];
            bearing = std::atan2(side / std::max(1e-6f, std::hypot(view_right[0], view_right[2])),
                                 front / std::max(1e-6f, std::hypot(view_forward[0], view_forward[2]))) *
                      57.29578f;
          }
          REXLOG_INFO("BF weather sounds: a strike {:.0f} m off{}: {} in {:.1f} s", distance,
                      view ? fmt::format(", {:.0f} degrees from the view", bearing) : std::string(" (no view)"),
                      kFiles[s.sound], double(distance) / 343.0);
        }
      }
      for (int k = kStrikeClose; k <= kStrikeClouds; ++k) Want(k);  // ready before the first strike's thunder
    } else {
      last_clock = -1.0;
    }
    {
      std::lock_guard lock(g_mix.mutex);
      g_mix.volume_target = playing && !muted ? volume : 0.0f;
      g_mix.master = g_test_sound ? 1.0f : float(std::clamp(REXCVAR_GET(bf_master_volume), 0.0, 100.0) / 100.0);
      g_mix.shelter_target = covered ? 1.0f : 0.0f;
      for (int l = 0; l < kLoops; ++l) {
        Loop& p = g_mix.loops[l];
        p.target = want[l];
        // A minute silent: its wave let go (decoded again when wanted).
        if (p.target <= 0.0f && p.gain <= 0.0f) {
          if (p.silent_since < 0.0) p.silent_since = now;
          else if (now - p.silent_since > 60.0 && g_mix.waves[l]) g_mix.waves[l].reset(), p.at = 0;
        } else {
          p.silent_since = -1.0;
        }
      }
      for (const Shot& s : shots) {
        if (g_mix.shots.size() >= 16) break;  // (far thunder waits up to half a minute: 6 dropped some)
        g_mix.shots.push_back(s);
#if defined(BF_DEV_TOOLS)
        // Tests: where its thunder starts in BF_DEV_WEATHER_SOUND_OUT's file (about: the device's buffer ahead).
        if (!g_mix.out_path.empty())
          REXLOG_INFO("BF weather sounds: that thunder from {:.1f} s in the recording", double(g_mix.out.size() / 2 + s.wait) / kRate);
#endif
      }
    }
#if defined(BF_DEV_TOOLS)
    if (now - written_at > 5.0) written_at = now, WriteOut();
    // BF_DEV_WEATHER_SOUND_LOG=1 (g_test_sound): each second what each loop is asked for and plays now (its gain),
    // the whole's volume and the thunder on its way: the rain heard or not, without a speaker.
    static double logged_at = 0.0;
    if (g_test_sound && now - logged_at >= 1.0) {
      logged_at = now;
      std::lock_guard lock(g_mix.mutex);
      std::string gains;
      for (int l = 0; l < kLoops; ++l) gains += fmt::format(" {:.2f}/{:.2f}", want[l], g_mix.loops[l].gain);
      REXLOG_INFO("BF weather sounds (test): {}; the match's weather {:.0f}%, here {:.0f}%; loops asked/playing (rain soft, "
                  "medium, storm, wind, tornado, rumble):{}; the whole's volume {:.2f}; thunder on its way {}",
                  between ? "between two rounds, silent" : playing ? "in a round" : "not in a match", w.storm * 100.0f,
                  w.weather * 100.0f, gains, g_mix.volume, g_mix.shots.size());
    }
    // Tests: each second the weather was turned down for the game's loudness, by how much at most, where in the
    // recording, and the game's loudness then against its usual.
    {
      std::lock_guard lock(g_mix.mutex);
      if (now - g_mix.duck_said > 1.0 && g_mix.duck_most > 1.0f) {
        REXLOG_INFO("BF weather sounds: turned down {:.1f} dB under the game's {:.1f} dB (usual {:.1f}; the rain and wind {:.1f}), {:.1f} s in the recording, {}",
                    g_mix.duck_most, g_mix.duck_game, g_mix.usual_db, g_mix.bed_db, double(g_mix.out.size() / 2) / kRate,
                    g_mix.shelter > 0.5f ? "under cover" : "in the open");
        g_mix.duck_most = 0.0f, g_mix.duck_said = now;
      }
    }
#endif
  }
}

}  // namespace

// From the menu driver (auto_join.cpp), on the game's thread, as the game's own menu hears them: a round's end (its
// WinningTeamIsSet), the next level's load beginning in a match (playerRoleReady; also a server's change of map
// without a round's end) and that level in (the second gameIsReady). The weather is silent from the first of them
// until the last; and from the load on, D221: the match's world is taken at once, not eased from the last round's
// storm, and what its rain left is dried (the round's own weather comes from its host: Night Ops' clear night).
void WeatherRoundOver() {
  int64_t none = 0;
  if (g_between_since.compare_exchange_strong(none, SteadyTicks(), std::memory_order_relaxed))
    REXLOG_INFO("BF weather sounds: the round is over: silent until the next level is in");
}

void WeatherLevelLoading() {
  int64_t none = 0;
  const bool first = g_between_since.compare_exchange_strong(none, SteadyTicks(), std::memory_order_relaxed);
  native_x_renderer::SnapWorld(kBetweenAtMost);  // until the level is in (WeatherLevelIn)
  REXLOG_INFO("BF weather sounds: the next level is loading: {}the match's world taken at once (D221)",
              first ? "silent until it is in, " : "");
}

void WeatherLevelIn() {
  native_x_renderer::SnapWorld(kNewRoundSettle);
  g_between_since.store(0, std::memory_order_relaxed);
  REXLOG_INFO("BF weather sounds: the next level is in: the weather heard again; its world taken at once for {:.0f} s (D221)",
              kNewRoundSettle);
}

// Started once with the world's sync (world_sync.cpp).
void StartWeatherAudio() {
  if (g_started.exchange(true)) return;
  std::thread(Run).detach();
}

// From the game's sound thread, each frame it hands the system (audio_settings.cpp): `frame` its 256 samples of each
// of the 6 channels one after the other (host floats, the master volume and the menus' mute applied), `game_db` the
// game's own loudness in it before those (dB, RMS of the five full channels).
void WeatherAudioFrame(float* frame, int samples, float game_db) {
  if (!g_started.load(std::memory_order_relaxed)) return;
  MixInto(frame, samples, game_db);
}


// ---- The news window's storm (news_video.h StormSound; TekRantGaming, 23 Nov 2026): the medium rain and the hurricane's
// wind on loop and the strikes' thunder, from the same recordings as a match's weather (Want: decoded once, shared
// with it), on an SDL stream of their own (our menus mute the game, and the window also shows before the game runs).
// Its level: full at the window's opening (the rain at its own level, the wind about 6 dB under it: the hurricane's
// recording is 14 dB louder than the rain's), held 3 s, then faded out with a cosine to silence 15 s after the opening,
// so the trailer's sound in the window soon has the room to itself.
namespace news {
namespace {
constexpr double kStormHold = 3.0, kStormSilent = 15.0, kStormCloseFade = 0.5;
constexpr float kStormRain = 0.85f, kStormWind = 0.12f;
// The thunders: their gains, and their recordings' quiet start cut (s) or a wait (s) before them, so each one's first
// sound comes as far after its flash as the strike is away: a close crack 0.12 s on, a distant strike 1.4 s on (480 m),
// lightning in the clouds 1 s on. The recordings' first sound: 0.57 s, 1.94 s and 0.73 s in.
constexpr float kThunderGain[3] = {0.55f, 0.45f, 0.47f};
constexpr int kThunderSound[3] = {kStrikeClose, kStrikeDistant, kStrikeClouds};
constexpr double kThunderSkip[3] = {0.45, 0.54, 0.0}, kThunderWait[3] = {0.0, 0.0, 0.27};

struct StormVoice {
  std::shared_ptr<const Wave> wave;
  size_t at = 0;
  int64_t wait = 0;
  float gain = 1.0f;
};
struct Storm {
  std::mutex mutex;
  SDL_AudioStream* stream = nullptr;
  bool no_device = false, open = false;
  int64_t frame = 0;   // frames mixed since the opening
  float close = 1.0f;  // the close's fade (1 while open)
  float volume = 0.0f;
  size_t rain_at = 0, wind_at = 0;
  std::vector<StormVoice> thunder;
  double last_log = -1.0;
  float log_power = 0.0f;
  int64_t log_frames = 0;
#if defined(BF_DEV_TOOLS)
  // BF_DEV_NEWS_STORM_OUT=<file.wav> (tests, run at volume 0): the storm as the speakers would have it at full volume.
  std::string out_path;
  std::vector<int16_t> out;
#endif
};
Storm g_storm;

float StormFade(double t) {
  if (t < kStormHold) return 1.0f;
  if (t >= kStormSilent) return 0.0f;
  return float(0.5 + 0.5 * std::cos((t - kStormHold) / (kStormSilent - kStormHold) * 3.14159265));
}

// SDL's audio thread asks for more.
void SDLCALL MixStorm(void*, SDL_AudioStream* stream, int additional, int) {
  float buffer[512 * 2];
  while (additional > 0) {
    const int frames = std::min(512, additional / 8);
    if (frames <= 0) break;
    std::shared_ptr<const Wave> rain, wind;
    {
      std::lock_guard lock(g_mix.mutex);
      rain = g_mix.waves[kRainMedium], wind = g_mix.waves[kWindHurricane];
    }
    {
      std::lock_guard lock(g_storm.mutex);
      for (int i = 0; i < frames; ++i) {
        const double t = double(g_storm.frame + i) / kRate;
        if (!g_storm.open) g_storm.close = std::max(0.0f, g_storm.close - float(1.0 / (kStormCloseFade * kRate)));
        const float level = StormFade(t) * g_storm.close;
        float l = 0.0f, r = 0.0f;
        if (level > 0.0f) {
          if (rain && rain->frames()) {
            const size_t k = (g_storm.rain_at++ % rain->frames()) * 2;
            l += rain->lr[k] * kStormRain, r += rain->lr[k + 1] * kStormRain;
          }
          if (wind && wind->frames()) {
            const size_t k = (g_storm.wind_at++ % wind->frames()) * 2;
            l += wind->lr[k] * kStormWind, r += wind->lr[k + 1] * kStormWind;
          }
          for (StormVoice& v : g_storm.thunder) {
            if (v.wait > 0) {
              --v.wait;
              continue;
            }
            if (v.at >= v.wave->frames()) continue;
            l += v.wave->lr[v.at * 2] * v.gain, r += v.wave->lr[v.at * 2 + 1] * v.gain;
            ++v.at;
          }
          l *= level / 32768.0f, r *= level / 32768.0f;
        }
        g_storm.log_power += 0.5f * (l * l + r * r), ++g_storm.log_frames;
#if defined(BF_DEV_TOOLS)
        if (!g_storm.out_path.empty()) {
          g_storm.out.push_back(int16_t(std::lround(std::clamp(l, -1.0f, 1.0f) * 32767.0f)));
          g_storm.out.push_back(int16_t(std::lround(std::clamp(r, -1.0f, 1.0f) * 32767.0f)));
        }
#endif
        buffer[i * 2] = std::clamp(l * g_storm.volume, -1.0f, 1.0f);
        buffer[i * 2 + 1] = std::clamp(r * g_storm.volume, -1.0f, 1.0f);
      }
      g_storm.frame += frames;
      g_storm.thunder.erase(std::remove_if(g_storm.thunder.begin(), g_storm.thunder.end(),
                                           [](const StormVoice& v) { return v.wait <= 0 && v.at >= v.wave->frames(); }),
                            g_storm.thunder.end());
    }
    SDL_PutAudioStreamData(stream, buffer, frames * 8);
    additional -= frames * 8;
  }
}

#if defined(BF_DEV_TOOLS)
void WriteStormOut() {
  if (g_storm.out_path.empty() || g_storm.out.empty()) return;
  std::ofstream f(g_storm.out_path, std::ios::binary | std::ios::trunc);
  auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  const uint32_t bytes = uint32_t(g_storm.out.size() * 2);
  f.write("RIFF", 4), u32(36 + bytes), f.write("WAVEfmt ", 8), u32(16), u16(1), u16(2), u32(kRate), u32(kRate * 4),
      u16(4), u16(16), f.write("data", 4), u32(bytes);
  f.write(reinterpret_cast<const char*>(g_storm.out.data()), std::streamsize(bytes));
}
#endif

void WantStormSounds() {
  {
    std::lock_guard lock(g_mix.mutex);
    if (g_mix.dir.empty()) g_mix.dir = ExecutableDir() / "data" / "sounds" / "weather";
  }
  for (int s : {int(kRainMedium), int(kWindHurricane), int(kStrikeClose), int(kStrikeDistant), int(kStrikeClouds)}) Want(s);
}
}  // namespace

void StormPrepare() { WantStormSounds(); }

void StormSound(bool open) {
  const double now = Now();
  std::unique_lock lock(g_storm.mutex);
  g_storm.volume = float(std::clamp(REXCVAR_GET(bf_master_volume), 0.0, 100.0) / 100.0 *
                         std::clamp(REXCVAR_GET(bf_effects_volume), 0.0, 100.0) / 100.0 *
                         std::clamp(REXCVAR_GET(bf_weather_volume), 0.0, 100.0) / 100.0);
  if (open && !g_storm.open) {  // the window opened: the storm from its start
    g_storm.open = true, g_storm.frame = 0, g_storm.close = 1.0f, g_storm.thunder.clear(), g_storm.last_log = now;
    g_storm.log_power = 0.0f, g_storm.log_frames = 0;
    lock.unlock();
    WantStormSounds();
    lock.lock();
#if defined(BF_DEV_TOOLS)
    if (const char* out = std::getenv("BF_DEV_NEWS_STORM_OUT"); out && *out) g_storm.out_path = out, g_storm.out.clear();
#endif
    REXLOG_INFO("BF news storm: the window opened: rain, wind and thunder, faded to silence over {:.0f} s (volume {:.2f})",
                kStormSilent, g_storm.volume);
    if (!g_storm.stream && !g_storm.no_device) {
      if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        REXLOG_WARN("BF news storm: SDL's audio did not start ({}): silent", SDL_GetError());
        g_storm.no_device = true;
        return;
      }
      const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kRate};
      lock.unlock();  // the callback takes the lock: never open the device holding it
      SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, MixStorm, nullptr);
      lock.lock();
      if (!stream) {
        REXLOG_WARN("BF news storm: no sound device ({}): silent", SDL_GetError());
        g_storm.no_device = true;
        return;
      }
      g_storm.stream = stream;
      SDL_ResumeAudioStreamDevice(stream);
    }
    return;
  }
  if (!open && g_storm.open) {
    g_storm.open = false;
    REXLOG_INFO("BF news storm: the window closed {:.1f} s after it opened: faded out", double(g_storm.frame) / kRate);
  }
  // Once a second while it sounds: its level (the tests' evidence of the fade).
  if (g_storm.stream && now - g_storm.last_log >= 1.0 && g_storm.log_frames > 0) {
    const float db = 10.0f * std::log10(std::max(1e-10f, g_storm.log_power / float(g_storm.log_frames)));
    REXLOG_INFO("BF news storm: {:.0f} s, {:.1f} dB (fade {:.0f}%)", double(g_storm.frame) / kRate, db,
                100.0f * StormFade(double(g_storm.frame) / kRate) * g_storm.close);
    g_storm.last_log = now, g_storm.log_power = 0.0f, g_storm.log_frames = 0;
  }
  // Silent (faded out, or closed and its fade done): the stream let go (outside the lock: it waits for its callback).
  const double t = double(g_storm.frame) / kRate;
  if (g_storm.stream && (t >= kStormSilent + 0.5 || (!g_storm.open && g_storm.close <= 0.0f))) {
    SDL_AudioStream* stream = g_storm.stream;
    g_storm.stream = nullptr;
#if defined(BF_DEV_TOOLS)
    WriteStormOut();
#endif
    REXLOG_INFO("BF news storm: silent after {:.1f} s: its sound stream closed", t);
    lock.unlock();
    SDL_DestroyAudioStream(stream);
  }
}

void StormThunder(int kind) {
  if (kind < 0 || kind > 2) return;
  std::shared_ptr<const Wave> wave;
  {
    std::lock_guard lock(g_mix.mutex);
    wave = g_mix.waves[kThunderSound[kind]];
  }
  std::lock_guard lock(g_storm.mutex);
  const double t = double(g_storm.frame) / kRate;
  if (!g_storm.stream || !g_storm.open || t >= kStormSilent) return;  // after the fade the flashes are silent
  if (!wave) {
    REXLOG_INFO("BF news storm: a strike's thunder not decoded yet: silent");
    return;
  }
  StormVoice v;
  v.wave = wave, v.gain = kThunderGain[kind];
  v.at = std::min(wave->frames(), size_t(kThunderSkip[kind] * kRate));
  v.wait = int64_t(kThunderWait[kind] * kRate);
  g_storm.thunder.push_back(std::move(v));
  REXLOG_INFO("BF news storm: thunder ({}) {:.1f} s after the opening", kind == 0 ? "close" : kind == 1 ? "distant" : "clouds", t);
}

}  // namespace news

}  // namespace bf1943
