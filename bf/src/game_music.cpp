// The launcher's menu music from the player's game files (docs/SERVERS-PLAN.md section 2, TekRantGaming 8 Nov evening): the
// game's own main menu track, BMUS_Menu_001 (its EA header in menu\main\mainmenu-00.fbrb, its stream in
// streaming_sounds-00.fbrb), made once into a WAV in the user's cache (never shipped) and played on loop while the
// launcher shows before the game runs.
//
// The sound is EA's EAAC format with EA-XMA inside (codec 3): an 8 byte header (version and codec, channels, sample
// rate; type, loop flag, sample count), then blocks (a flag byte and a 24 bit size; 0x80 ends), each holding the
// sample count and, per 1 or 2 channel stream, its size times 4 and its XMA2 data, one packet short of 2048 bytes and
// padded with 0xFF. This layout is the one vgmstream reads (src/meta/ea_eaac.c, ea_eaac_streamfile.h; ISC licence,
// licences\vgmstream.txt); the packets go to FFmpeg's XMA2 decoder (built into the SDK's FFmpeg, not registered).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif
#include <SDL3/SDL.h>  // the menu sounds' mixer and the WAV player

#include "audio_devices.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/mem.h"
extern AVCodec ff_xma2_decoder;  // wmaprodec.c: compiled in, not in the SDK's codec list
}

namespace bf1943 {
namespace fs = std::filesystem;
namespace {

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

void Le16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(uint8_t(v)), o.push_back(uint8_t(v >> 8)); }
void Le32(std::vector<uint8_t>& o, uint32_t v) { Le16(o, uint16_t(v)), Le16(o, uint16_t(v >> 16)); }

}  // namespace

// The EA sound in `wave_res` (a Frostbite Wave resource: an optional 0x40 byte "RES" wrapper, then the EA header at
// +0x24) with its data inline (RAM type) or in `stream` (stream type), as a 16 bit WAV file at `scale` volume.
bool EaAudioToWav(const std::vector<uint8_t>& wave_res, const std::vector<uint8_t>* stream, const fs::path& wav, float scale) {
  const size_t base = wave_res.size() >= 3 && std::memcmp(wave_res.data(), "RES", 3) == 0 ? 0x40 : 0;
  if (wave_res.size() < base + 0x2C) return false;
  const uint8_t* snr = &wave_res[base + 0x24];
  const uint32_t h1 = Be32(snr), h2 = Be32(snr + 4);
  const int codec = int(h1 >> 24) & 15, channels = int((h1 >> 18) & 63) + 1, rate = int(h1 & 0x3FFFF);
  const int type = int(h2 >> 30), loop = int(h2 >> 29) & 1;
  const uint32_t samples = h2 & 0x1FFFFFFF;
  if (codec != 3 || channels > 2 || rate <= 0) {
    REXLOG_WARN("BF music: not EA-XMA in one stream (codec {}, {} channels)", codec, channels);
    return false;
  }
  const uint8_t* data;
  size_t size;
  if (type == 0) {  // in memory: the blocks follow the header
    const size_t at = base + 0x24 + 8 + (loop ? 4 : 0);
    if (at >= wave_res.size()) return false;
    data = &wave_res[at], size = wave_res.size() - at;
  } else {
    if (!stream || stream->empty()) return false;
    data = stream->data(), size = stream->size();
  }

  AVCodecContext* ctx = avcodec_alloc_context3(&ff_xma2_decoder);
  if (!ctx) return false;
  ctx->channels = channels;
  ctx->sample_rate = rate;
  ctx->block_align = 2048;
  ctx->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));  // XMA2WAVEFORMATEX
  ctx->extradata_size = 34;
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  std::vector<int16_t> pcm;
  pcm.reserve(size_t(samples) * channels);
  bool ok = ctx->extradata && packet && frame && avcodec_open2(ctx, &ff_xma2_decoder, nullptr) == 0;
  std::vector<uint8_t> buffer(2048 + AV_INPUT_BUFFER_PADDING_SIZE);
  auto drain = [&] {
    while (avcodec_receive_frame(ctx, frame) == 0) {
      const int n = frame->nb_samples;
      for (int i = 0; i < n && pcm.size() < size_t(samples) * channels; ++i)
        for (int c = 0; c < channels; ++c) {
          const float v = reinterpret_cast<const float*>(frame->extended_data[c])[i] * scale;
          pcm.push_back(int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)));
        }
    }
  };
  size_t at = 0, blocks = 0;
  while (ok && at + 12 <= size) {
    const uint8_t flag = data[at];
    const uint32_t block = Be32(&data[at]) & 0xFFFFFF;
    if (block < 12 || at + block > size) break;
    // One stream (1 or 2 channels): its size times 4 (with itself), then its XMA2 data.
    const uint32_t stream_size = Be32(&data[at + 8]) / 4;
    if (stream_size >= 4 && stream_size + 8 <= block) {
      const uint8_t* xma = &data[at + 12];
      for (size_t done = 0; done < stream_size - 4; done += 2048) {
        const size_t n = std::min<size_t>(2048, stream_size - 4 - done);
        std::memset(buffer.data(), 0xFF, 2048);
        std::memcpy(buffer.data(), xma + done, n);
        packet->data = buffer.data();
        packet->size = 2048;
        if (avcodec_send_packet(ctx, packet) == AVERROR(EAGAIN)) {
          drain();
          avcodec_send_packet(ctx, packet);
        }
        drain();
      }
    }
    ++blocks;
    at += block;
    if (flag == 0x80) break;
  }
  if (ok) {
    avcodec_send_packet(ctx, nullptr);
    drain();
  }
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&ctx);
  if (!ok || pcm.empty()) {
    REXLOG_WARN("BF music: the XMA2 decoder gave nothing ({} blocks)", blocks);
    return false;
  }
  // A 16 bit WAV.
  std::vector<uint8_t> out;
  const uint32_t bytes = uint32_t(pcm.size() * 2);
  out.insert(out.end(), {'R', 'I', 'F', 'F'});
  Le32(out, 36 + bytes);
  out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  Le32(out, 16), Le16(out, 1), Le16(out, uint16_t(channels)), Le32(out, uint32_t(rate));
  Le32(out, uint32_t(rate * channels * 2)), Le16(out, uint16_t(channels * 2)), Le16(out, 16);
  out.insert(out.end(), {'d', 'a', 't', 'a'});
  Le32(out, bytes);
  const size_t head = out.size();
  out.resize(head + bytes);
  std::memcpy(&out[head], pcm.data(), bytes);
  std::error_code ec;
  fs::create_directories(wav.parent_path(), ec);
  const fs::path temp = wav.string() + ".tmp";
  {
    std::ofstream f(temp, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    if (!f) return false;
  }
  fs::rename(temp, wav, ec);
  REXLOG_INFO("BF music: {} ({} channels, {} Hz, {:.1f} s from {} blocks)", wav.filename().string(), channels, rate,
              double(pcm.size() / channels) / rate, blocks);
  return !ec;
}

namespace {
// SDL's audio (the runtime's, as the voice chat's, comms.cpp) plays these WAVs, one sound at a time: starting one stops
// the other. Linux first (GitHub issue 21, 14 Nov 2026: "users with the appimage having no menu music"); Windows too
// since 23 Nov 2026 (it had its own PlaySound, which plays on the system's default device only): so the menu music is
// on the device Settings > Audio's Game sound output names (audio_devices.h).
struct WavPlayer {
  SDL_AudioStream* stream = nullptr;
  Uint8* data = nullptr;
  Uint32 length = 0;
  size_t at = 0;
  bool loop = false;
};
WavPlayer g_player;

// SDL's audio thread asks for more: the WAV from where it was, from its start again when it loops.
void SDLCALL FeedWav(void* user, SDL_AudioStream* stream, int additional, int) {
  auto* p = static_cast<WavPlayer*>(user);
  while (additional > 0 && p->length > 0) {
    if (p->at >= p->length) {
      if (!p->loop) return;
      p->at = 0;
    }
    const int n = int(std::min<size_t>(size_t(additional), p->length - p->at));
    SDL_PutAudioStreamData(stream, p->data + p->at, n);
    p->at += size_t(n), additional -= n;
  }
}
void StopWav() {
  if (g_player.stream) audio_devices::CloseStream(g_player.stream);  // its callback has stopped once this returns
  if (g_player.data) SDL_free(g_player.data);
  g_player = WavPlayer{};
}
bool PlayWav(const fs::path& wav, bool loop) {
  StopWav();
  if (!fs::exists(wav)) return false;
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXLOG_WARN("BF music: SDL's audio did not start ({})", SDL_GetError());
    return false;
  }
  SDL_AudioSpec spec{};
  const std::u8string path = wav.u8string();  // SDL's paths are UTF-8 (a user folder with accents, on Windows)
  if (!SDL_LoadWAV(reinterpret_cast<const char*>(path.c_str()), &spec, &g_player.data, &g_player.length)) {
    REXLOG_WARN("BF music: {} could not be read ({})", wav.string(), SDL_GetError());
    return false;
  }
  g_player.loop = loop;
  g_player.stream = audio_devices::OpenStream(audio_devices::Role::kGame, spec, FeedWav, &g_player, "the menu music");
  if (!g_player.stream) {
    REXLOG_WARN("BF music: no sound device ({})", SDL_GetError());
    StopWav();
    return false;
  }
  return true;
}
}  // namespace

// A sound played once (the startup cards' logo music: game_menu.cpp IntroPage), and every sound stopped.
void PlaySoundOnce(const fs::path& wav) { PlayWav(wav, false); }
void StopSounds() { StopWav(); }

// The menu music on loop (SDL's audio thread feeds it), or stopped.
void PlayMenuMusic(const fs::path& wav, bool on) {
  static bool playing = false;
  if (on == playing) return;
  if (on) {
    playing = PlayWav(wav, true);
    if (playing) REXLOG_INFO("BF music: the menu music plays on loop (SDL)");
  } else {
    StopWav();
    playing = false;
    REXLOG_INFO("BF music: the menu music stopped");
  }
}

// ---- The original menus' sounds in ours (TekRantGaming, 16 Nov 2026: "the original menus sounds for moving and
// button pressing is extracted and used in our menus now and pause menus").
//
// The game's menus (dice.UISound in main_nav, main_popups, common_screens and main_root .apt) play four waves:
//   SND_NAV_UP, SND_NAV_DOWN          BSFX_UI_FEScroll_01    up and down a list, a page's tabs   -> kScroll
//   SND_NAV_GRID                      BSFX_UI_FETick_01      left and right: an option's value   -> kTick
//   SND_MENU_SELECT, _TRANSITION      BSFX_UI_FEForward_01   a press, a page opened              -> kForward
//   SND_MENU_BACK                     BSFX_UI_FEBackwards_01 back out                            -> kBack
// and nothing for SND_NAV_NOGO (a move at a list's end) and the pop-ups (SND_POPUP_ERROR, _INFO). shader_prep.cpp
// makes the four into 16 bit WAVs in the user's cache (from mainmenu-00.fbrb, never shipped); here they are read at
// the first sound, kept in memory at 48 kHz stereo and mixed on SDL's audio (one stream, the runtime's SDL, as the
// voice chat's), apart from the menu music's own stream, so a press never stops the music. A sound whose
// file is not there stays silent (looked for again every 5 s: the cache is made while the first page shows).
//
// While our menus show, the game's own copies of these four are kept quiet (the sub_826A8330 hook below): our pause
// drives BF's pause list under ours with presses of its own (game_menu.cpp PressBf), and BF's menus would play
// their sounds for those too, in a match where the game's sound is on.

REXCVAR_DEFINE_BOOL(bf_menu_sounds, true, "BF1943/Audio", "The original menus' sounds in our menus");
REXCVAR_DEFINE_DOUBLE(bf_menu_sounds_volume, 100.0, "BF1943/Audio", "How loud the menu sounds are, 0 to 100 (%)");

namespace {
constexpr int kMixRate = 48000;
constexpr const char* kMenuSoundFiles[4] = {"scroll", "tick", "forward", "back"};

struct MenuVoice {
  int wave = 0;
  size_t at = 0;  // frames played
  float gain = 1.0f;
};
struct MenuMixer {
  std::mutex mutex;
  std::vector<float> waves[4];  // 48 kHz stereo, interleaved
  double look_at[4] = {};       // when to look for a missing file again (steady seconds)
  std::vector<MenuVoice> voices;
  SDL_AudioStream* stream = nullptr;
  bool no_device = false;
  double last_at[4] = {-1, -1, -1, -1};
#if defined(BF_DEV_TOOLS)
  // BF_DEV_MENU_SOUND_OUT=<file.wav> (tests): no sound device; every sound mixed into that file at the moment it
  // played, 48 kHz stereo 16 bit, from the first sound on (the evidence that each press made its sound).
  std::string out_path;
  std::vector<float> out;
  double out_from = -1;
#endif
};
MenuMixer g_mixer;

double SteadySeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A 16 bit PCM WAV (ours, EaAudioToWav) as 48 kHz stereo floats.
bool ReadWav16(const fs::path& path, std::vector<float>& lr) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(&d[8], "WAVE", 4) != 0) return false;
  auto u16 = [&](size_t at) { return uint32_t(d[at]) | uint32_t(d[at + 1]) << 8; };
  auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
  int channels = 0, rate = 0, bits = 0;
  const uint8_t* pcm = nullptr;
  size_t bytes = 0;
  for (size_t at = 12; at + 8 <= d.size();) {
    const uint32_t size = u32(at + 4);
    if (at + 8 + size > d.size()) break;
    if (std::memcmp(&d[at], "fmt ", 4) == 0 && size >= 16) {
      if (u16(at + 8) != 1) return false;
      channels = int(u16(at + 10)), rate = int(u32(at + 12)), bits = int(u16(at + 22));
    } else if (std::memcmp(&d[at], "data", 4) == 0) {
      pcm = &d[at + 8], bytes = size;
    }
    at += 8 + size + (size & 1);
  }
  if (!pcm || bits != 16 || channels < 1 || channels > 2 || rate <= 0) return false;
  const size_t frames = bytes / size_t(2 * channels);
  if (frames == 0) return false;
  auto sample = [&](size_t frame, int c) {
    const uint8_t* p = pcm + (frame * channels + size_t(std::min(c, channels - 1))) * 2;
    return float(int16_t(uint16_t(p[0] | p[1] << 8))) / 32768.0f;
  };
  const size_t out_frames = size_t(double(frames) * kMixRate / rate);
  lr.resize(out_frames * 2);
  for (size_t i = 0; i < out_frames; ++i) {  // linear: these are short clicks, made at 32 or 48 kHz
    const double src = double(i) * rate / kMixRate;
    const size_t i0 = std::min(size_t(src), frames - 1), i1 = std::min(i0 + 1, frames - 1);
    const float t = float(src - double(i0));
    for (int c = 0; c < 2; ++c) lr[i * 2 + c] = sample(i0, c) * (1 - t) + sample(i1, c) * t;
  }
  return true;
}

// SDL's audio thread asks for more: every sound playing, added up.
void SDLCALL MixMenuSounds(void*, SDL_AudioStream* stream, int additional, int) {
  float buffer[512 * 2];
  while (additional > 0) {
    const int frames = std::min(512, additional / 8);
    if (frames <= 0) break;
    std::fill(buffer, buffer + frames * 2, 0.0f);
    {
      std::lock_guard<std::mutex> lock(g_mixer.mutex);
      for (MenuVoice& v : g_mixer.voices) {
        const std::vector<float>& w = g_mixer.waves[v.wave];
        const size_t total = w.size() / 2;
        for (int i = 0; i < frames && v.at < total; ++i, ++v.at) {
          buffer[i * 2] += w[v.at * 2] * v.gain;
          buffer[i * 2 + 1] += w[v.at * 2 + 1] * v.gain;
        }
      }
      g_mixer.voices.erase(std::remove_if(g_mixer.voices.begin(), g_mixer.voices.end(),
                                          [](const MenuVoice& v) { return v.at >= g_mixer.waves[v.wave].size() / 2; }),
                           g_mixer.voices.end());
    }
    for (int i = 0; i < frames * 2; ++i) buffer[i] = std::clamp(buffer[i], -1.0f, 1.0f);
    SDL_PutAudioStreamData(stream, buffer, frames * 8);
    additional -= frames * 8;
  }
}

#if defined(BF_DEV_TOOLS)
void WriteMenuSoundOut() {
  std::vector<uint8_t> o;
  const uint32_t bytes = uint32_t(g_mixer.out.size() * 2);
  o.insert(o.end(), {'R', 'I', 'F', 'F'});
  Le32(o, 36 + bytes);
  o.insert(o.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  Le32(o, 16), Le16(o, 1), Le16(o, 2), Le32(o, kMixRate), Le32(o, kMixRate * 4), Le16(o, 4), Le16(o, 16);
  o.insert(o.end(), {'d', 'a', 't', 'a'});
  Le32(o, bytes);
  for (float v : g_mixer.out) Le16(o, uint16_t(int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))));
  std::ofstream f(g_mixer.out_path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(o.data()), std::streamsize(o.size()));
}
#endif

std::atomic<bool> g_menu_owns_ui_sound{false};
}  // namespace

// Our menus show (game_menu.cpp, every frame): the game's own menu sounds are ours to play meanwhile.
void SetMenuOwnsUiSound(bool ours) { g_menu_owns_ui_sound.store(ours, std::memory_order_relaxed); }
bool MenuOwnsUiSound() { return g_menu_owns_ui_sound.load(std::memory_order_relaxed); }

// One of the four sounds (0 scroll, 1 tick, 2 forward, 3 back) from `dir` (the user's cache\extracted\sounds) at
// `gain` (the menu's volume rows), from our menus' thread; `why` is for the log. Silent when its file is missing or
// there is no sound device.
void PlayMenuSound(const fs::path& dir, int which, float gain, const char* why) {
  if (which < 0 || which > 3) return;
  const double now = SteadySeconds();
  std::unique_lock<std::mutex> lock(g_mixer.mutex);
  // The same sound twice within 40 ms (a key's repeat and the pad's in one frame) is once.
  if (g_mixer.last_at[which] >= 0 && now - g_mixer.last_at[which] < 0.04) return;
  if (g_mixer.waves[which].empty()) {
    if (now < g_mixer.look_at[which]) return;
    lock.unlock();
    std::vector<float> lr;
    const bool ok = ReadWav16(dir / (std::string(kMenuSoundFiles[which]) + ".wav"), lr);
    lock.lock();
    if (!ok) {
      if (g_mixer.look_at[which] == 0)
        REXLOG_WARN("BF menu sounds: no {} sound in the cache yet: silent until it is made", kMenuSoundFiles[which]);
      g_mixer.look_at[which] = now + 5;
      return;
    }
    if (g_mixer.waves[which].empty()) {
      REXLOG_INFO("BF menu sounds: the {} sound read ({:.0f} ms)", kMenuSoundFiles[which], 1000.0 * double(lr.size() / 2) / kMixRate);
      g_mixer.waves[which] = std::move(lr);  // the audio thread has no voice of it yet: nothing reads it
    }
  }
  g_mixer.last_at[which] = now;
  gain = std::clamp(gain, 0.0f, 1.0f);
#if defined(BF_DEV_TOOLS)
  REXLOG_INFO("BF menu sounds: {} ({}, gain {:.2f})", kMenuSoundFiles[which], why ? why : "", gain);
  static const char* out_env = std::getenv("BF_DEV_MENU_SOUND_OUT");
  if (out_env && *out_env) {
    if (g_mixer.out_from < 0) g_mixer.out_from = now, g_mixer.out_path = out_env;
    const std::vector<float>& w = g_mixer.waves[which];
    const size_t at = size_t((now - g_mixer.out_from) * kMixRate) * 2;
    if (g_mixer.out.size() < at + w.size()) g_mixer.out.resize(at + w.size(), 0.0f);
    // At full scale whatever the volume rows say (the log has the gain): a test keeps the PC silent with the master
    // volume at 0, and the file is there to tell the sounds apart.
    for (size_t i = 0; i < w.size(); ++i) g_mixer.out[at + i] += w[i];
    WriteMenuSoundOut();
    return;
  }
#else
  (void)why;
#endif
  if (gain <= 0.0f) return;
  if (!g_mixer.stream && !g_mixer.no_device) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      REXLOG_WARN("BF menu sounds: SDL's audio did not start ({}): the menus are silent", SDL_GetError());
      g_mixer.no_device = true;
      return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kMixRate};
    lock.unlock();  // the callback takes the lock: never open the device holding it
    SDL_AudioStream* stream =
        audio_devices::OpenStream(audio_devices::Role::kGame, spec, MixMenuSounds, nullptr, "the menu sounds");
    lock.lock();
    if (!stream) {
      REXLOG_WARN("BF menu sounds: no sound device ({}): the menus are silent", SDL_GetError());
      g_mixer.no_device = true;
      return;
    }
    g_mixer.stream = stream;
  }
  if (!g_mixer.stream) return;
  if (g_mixer.voices.size() >= 8) g_mixer.voices.erase(g_mixer.voices.begin());  // a held key: the oldest goes
  g_mixer.voices.push_back({which, 0, gain});
}

}  // namespace bf1943

// The game's own menu sounds (found 16 Nov 2026, runs sndmatch4 and 5). Its Flash player's callbacks (registered by
// sub_826B77E0 at 82D13C48) have, for sounds:
//   +0x54 sub_826A8330(system, r4 the movie, r5 1..4, r6 the name), through the thunk sub_826AAEF8: makes a
//         Sound/SimpleSound of "<name>_Wave" and returns its handle. dice.UISound makes its four once when a movie
//         loads, never per press (so it is never refused: the menus would keep a null handle for the whole run);
//   +0x5C sub_8269C8B0(handle): the handle's method 0, a press's play (lr 829733CC, Sound.start);
//   +0x58 sub_8269C8D8(handle): its method 2, the release when the movie goes.
// The four BSFX_UI_FE handles are noted as they are made; their plays are skipped while our menus show
// (SetMenuOwnsUiSound): our pause sends presses of its own to BF's hidden pause list, whose sounds would come with
// ours in a match, where the game's sound is on. BF's own screens that have the real pad (our menu closed) and the
// game's other sounds play as before.
namespace {
struct UiHandle {
  std::atomic<uint32_t> handle{0};
  std::atomic<int> which{-1};
};
UiHandle g_ui_handles[16];
std::atomic<uint32_t> g_ui_next{0};
UiHandle* UiHandleOf(uint32_t handle) {
  if (!handle) return nullptr;
  for (UiHandle& h : g_ui_handles)
    if (h.handle.load(std::memory_order_relaxed) == handle) return &h;
  return nullptr;
}
}  // namespace

REX_EXTERN(__imp__sub_826A8330);
REX_HOOK_RAW(sub_826A8330) {
  char name[96] = {};
  if (const uint32_t at = ctx.r6.u32; at >= 0x10000 && at < 0xF0000000)
    for (size_t i = 0; i + 1 < sizeof(name) && base[at + i]; ++i) name[i] = char(std::tolower(static_cast<unsigned char>(base[at + i])));
  __imp__sub_826A8330(ctx, base);
  const std::string lower = name;
  const int which = lower.find("bsfx_ui_fescroll") != std::string::npos      ? 0
                    : lower.find("bsfx_ui_fetick") != std::string::npos      ? 1
                    : lower.find("bsfx_ui_feforward") != std::string::npos   ? 2
                    : lower.find("bsfx_ui_febackwards") != std::string::npos ? 3
                                                                             : -1;
  if (which >= 0 && ctx.r3.u32) {
    UiHandle& h = g_ui_handles[g_ui_next.fetch_add(1, std::memory_order_relaxed) % 16];
    h.which.store(which), h.handle.store(ctx.r3.u32);
  }
}

REX_EXTERN(__imp__sub_8269C8B0);
REX_HOOK_RAW(sub_8269C8B0) {
  if (const UiHandle* h = UiHandleOf(ctx.r3.u32); h && bf1943::MenuOwnsUiSound()) {
#if defined(BF_DEV_TOOLS)
    REXLOG_INFO("BF menu sounds: the game's own {} kept quiet (our menus show)", bf1943::kMenuSoundFiles[h->which.load()]);
#endif
    return;
  }
#if defined(BF_DEV_TOOLS)
  if (const UiHandle* h = UiHandleOf(ctx.r3.u32))
    REXLOG_INFO("BF menu sounds: the game plays its own {}", bf1943::kMenuSoundFiles[h->which.load()]);
#endif
  __imp__sub_8269C8B0(ctx, base);
}

REX_EXTERN(__imp__sub_8269C8D8);
REX_HOOK_RAW(sub_8269C8D8) {
  if (UiHandle* h = UiHandleOf(ctx.r3.u32)) h->handle.store(0);  // released: its address is free again
  __imp__sub_8269C8D8(ctx, base);
}
