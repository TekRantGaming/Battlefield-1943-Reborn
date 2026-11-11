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
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

// A sound played once (the startup cards' logo music: game_menu.cpp IntroPage), and every sound stopped.
void PlaySoundOnce(const fs::path& wav) {
#if defined(_WIN32)
  if (fs::exists(wav)) PlaySoundW(wav.wstring().c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
#endif
}
void StopSounds() {
#if defined(_WIN32)
  PlaySoundW(nullptr, nullptr, 0);
#endif
}

// The menu music on loop (Windows' own player, on its own thread), or stopped.
void PlayMenuMusic(const fs::path& wav, bool on) {
#if defined(_WIN32)
  static bool playing = false;
  if (on == playing) return;
  if (on) {
    if (!fs::exists(wav)) return;
    playing = PlaySoundW(wav.wstring().c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_LOOP | SND_NODEFAULT) != FALSE;
    if (playing) REXLOG_INFO("BF music: the menu music plays on loop");
  } else {
    PlaySoundW(nullptr, nullptr, 0);
    playing = false;
    REXLOG_INFO("BF music: the menu music stopped");
  }
#endif
}

}  // namespace bf1943
