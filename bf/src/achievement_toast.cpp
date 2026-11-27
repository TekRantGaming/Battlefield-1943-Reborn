// The achievement popup and its chime (achievement_toast.h).

#include "achievement_toast.h"
#include "audio_devices.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <vector>

#include <imgui.h>
#include <SDL3/SDL.h>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>

#include "launcher_kit.h"
#include "reborn_achievements.h"
#include "theme_chime.h"

// Settings > Audio: the popups and the chime's volume.
REXCVAR_DEFINE_BOOL(bf_achievement_popups, true, "BF1943/Audio", "A popup and a chime when the game unlocks an achievement");
REXCVAR_DEFINE_BOOL(bf_achievement_sound, true, "BF1943/Audio",
                    "The chime with an achievement's popup (off: the popup alone, silent)");
REXCVAR_DEFINE_DOUBLE(bf_achievement_volume, 80.0, "BF1943/Audio", "How loud the achievement chime is, 0 to 100 (%)");

namespace bf1943 {
namespace fs = std::filesystem;
bool ShaderScreenShowing();  // game_menu.cpp: the shader page is up (nothing is drawn over it)
namespace {

// One popup's timeline, in seconds: in from the right, held, out to the right.
constexpr double kIn = 0.35, kHold = 5.0, kOut = 0.35, kTotal = kIn + kHold + kOut;

std::atomic<bool> g_test{false};  // TestAchievementPopup: shown even with the popups off
std::mutex g_dir_mutex;
fs::path g_user_dir;
AchievementToast* g_toast = nullptr;  // the one the app made (CreateAchievementNotificationDialog)

float EaseOut(float t) { return 1 - std::pow(1 - std::clamp(t, 0.0f, 1.0f), 3.0f); }
float EaseIn(float t) { return std::pow(std::clamp(t, 0.0f, 1.0f), 3.0f); }

// ---- the plain chime, when the game's files give none: two rising bell notes over a soft low one, with a short echo
// (King Kong's, kk/src/toast.cpp)

void PutWavHeader(std::vector<uint8_t>& wav, uint32_t rate, uint16_t channels, uint32_t data_bytes) {
  auto put32 = [&](size_t o, uint32_t v) { std::memcpy(&wav[o], &v, 4); };
  auto put16 = [&](size_t o, uint16_t v) { std::memcpy(&wav[o], &v, 2); };
  std::memcpy(&wav[0], "RIFF", 4);
  put32(4, 36 + data_bytes);
  std::memcpy(&wav[8], "WAVEfmt ", 8);
  put32(16, 16);
  put16(20, 1);
  put16(22, channels);
  put32(24, rate);
  put32(28, rate * channels * 2);
  put16(32, uint16_t(channels * 2));
  put16(34, 16);
  std::memcpy(&wav[36], "data", 4);
  put32(40, data_bytes);
}

std::vector<uint8_t> MakeChime(float volume) {
  constexpr int kRate = 44100;
  constexpr double kLength = 1.6, kPi = 3.14159265358979323846;
  std::vector<float> mix(size_t(kRate * kLength), 0.0f);
  auto bell = [&](double start, double freq, double gain, double decay) {
    const size_t s0 = size_t(start * kRate);
    for (size_t i = s0; i < mix.size(); ++i) {
      const double t = double(i - s0) / kRate;
      const double env = std::min(1.0, t / 0.006) * std::exp(-t / decay);
      const double v = std::sin(2 * kPi * freq * t) + 0.45 * std::sin(2 * kPi * freq * 2.0 * t) * std::exp(-t / 0.15) +
                       0.18 * std::sin(2 * kPi * freq * 3.01 * t) * std::exp(-t / 0.08);
      mix[i] += float(gain * env * v);
    }
  };
  bell(0.00, 392.00, 0.22, 0.30);
  bell(0.02, 783.99, 0.55, 0.40);
  bell(0.13, 1174.66, 0.60, 0.55);
  bell(0.13, 1567.98, 0.18, 0.30);
  const size_t delay = size_t(0.17 * kRate);
  for (size_t i = mix.size(); i-- > delay;) mix[i] += 0.28f * mix[i - delay];
  float peak = 1e-6f;
  for (float v : mix) peak = std::max(peak, std::abs(v));
  const float scale = 0.70f * std::clamp(volume, 0.0f, 1.0f) / peak;
  const uint32_t data_bytes = uint32_t(mix.size() * 2);
  std::vector<uint8_t> wav(44 + data_bytes);
  PutWavHeader(wav, kRate, 1, data_bytes);
  for (size_t i = 0; i < mix.size(); ++i) {
    const int16_t s = int16_t(std::clamp(mix[i] * scale, -1.0f, 1.0f) * 32767.0f);
    std::memcpy(&wav[44 + i * 2], &s, 2);
  }
  return wav;
}

// The player's own sounds\achievement.wav (16 bit PCM), at the volume; empty when there is none or it is not that.
std::vector<uint8_t> LoadOwnWav(const fs::path& file, float volume) {
  std::ifstream f(file, std::ios::binary | std::ios::ate);
  if (!f) return {};
  const auto size = size_t(f.tellg());
  if (size < 44 || size > 16u << 20) return {};
  std::vector<uint8_t> in(size);
  f.seekg(0);
  f.read(reinterpret_cast<char*>(in.data()), std::streamsize(in.size()));
  if (std::memcmp(in.data(), "RIFF", 4) || std::memcmp(in.data() + 8, "WAVE", 4)) return {};
  const uint8_t* fmt = nullptr;
  size_t data_off = 0, data_len = 0;
  for (size_t o = 12; o + 8 <= in.size();) {
    uint32_t len;
    std::memcpy(&len, &in[o + 4], 4);
    if (!std::memcmp(&in[o], "fmt ", 4) && len >= 16 && o + 8 + 16 <= in.size()) fmt = &in[o + 8];
    if (!std::memcmp(&in[o], "data", 4)) {
      data_off = o + 8;
      data_len = std::min<size_t>(len, in.size() - data_off);
      break;
    }
    o += 8 + size_t(len) + (len & 1);
  }
  if (!fmt || !data_off) return {};
  uint16_t format, channels, bits;
  uint32_t rate;
  std::memcpy(&format, fmt, 2), std::memcpy(&channels, fmt + 2, 2), std::memcpy(&rate, fmt + 4, 4);
  std::memcpy(&bits, fmt + 14, 2);
  if (format != 1 || bits != 16 || channels == 0 || channels > 2 || rate < 8000 || rate > 192000) return {};
  data_len &= ~size_t(channels * 2 - 1);
  std::vector<uint8_t> wav(44 + data_len);
  PutWavHeader(wav, rate, channels, uint32_t(data_len));
  for (size_t i = 0; i + 1 < data_len; i += 2) {
    int16_t s;
    std::memcpy(&s, &in[data_off + i], 2);
    const int16_t scaled = int16_t(std::clamp(float(s) * volume, -32768.0f, 32767.0f));
    std::memcpy(&wav[44 + i], &scaled, 2);
  }
  return wav;
}

// ---- the Battlefield theme's chime (theme_chime.h; TekRantGaming, 18 Nov 2026: "the iconic Battlefield tone play
// as like a kind of esque achievement kind of sound"): made from the menu theme the first start extracted from the
// player's own game files, and kept beside the menus' other sounds (user\cache\extracted\sounds). Only its recipe
// ships.

// The first `frames` samples (16 bit stereo at 48 kHz) of `file`; empty with `why` when it is not there or not that.
std::vector<int16_t> ReadThemeFrames(const fs::path& file, uint32_t frames, const char** why) {
  auto fail = [&](const char* reason) {
    *why = reason;
    return std::vector<int16_t>{};
  };
  std::ifstream f(file, std::ios::binary);
  if (!f) return fail("the menu music is not extracted");
  uint8_t head[12];
  if (!f.read(reinterpret_cast<char*>(head), 12) || std::memcmp(head, "RIFF", 4) || std::memcmp(head + 8, "WAVE", 4))
    return fail("the menu music is not a WAV");
  bool fmt_ok = false;
  for (;;) {
    uint8_t chunk[8];
    if (!f.read(reinterpret_cast<char*>(chunk), 8)) return fail("the menu music has no sound data");
    uint32_t len;
    std::memcpy(&len, chunk + 4, 4);
    if (!std::memcmp(chunk, "fmt ", 4) && len >= 16) {
      uint8_t fmt[16];
      if (!f.read(reinterpret_cast<char*>(fmt), 16)) return fail("the menu music is cut short");
      uint16_t format, channels, bits;
      uint32_t rate;
      std::memcpy(&format, fmt, 2), std::memcpy(&channels, fmt + 2, 2), std::memcpy(&rate, fmt + 4, 4);
      std::memcpy(&bits, fmt + 14, 2);
      fmt_ok = format == 1 && channels == 2 && rate == theme_chime::kRate && bits == 16;
      if (!fmt_ok) return fail("the menu music is not 16 bit stereo at 48 kHz");
      f.seekg(std::streamoff(len - 16 + (len & 1)), std::ios::cur);
    } else if (!std::memcmp(chunk, "data", 4)) {
      if (!fmt_ok) return fail("the menu music has no format");
      if (uint64_t(frames) * 4 > len) return fail("the menu music is shorter than the cut");
      std::vector<int16_t> out(size_t(frames) * 2);
      if (!f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size() * 2))) return fail("the menu music is cut short");
      return out;
    } else {
      f.seekg(std::streamoff(len + (len & 1)), std::ios::cur);
    }
  }
}

}  // namespace

bool MakeAchievementChime(const fs::path& extracted) {
  static std::mutex mutex;  // the first start's worker and an unlock may both ask
  std::lock_guard lock(mutex);
  const fs::path made = (extracted / theme_chime::kMade).make_preferred();
  std::error_code ec;
  if (fs::exists(made, ec)) return true;
  if (kit::NoGameArt()) return false;
  const auto t0 = std::chrono::steady_clock::now();
  const char* why = nullptr;
  const std::vector<int16_t> src = ReadThemeFrames((extracted / theme_chime::kSource).make_preferred(), theme_chime::kRecipe.end, &why);
  if (!src.empty()) {
    const std::vector<int16_t> pcm = theme_chime::Make(theme_chime::kRecipe, src.data(), src.size() / 2);
    if (pcm.empty()) {
      why = "the cut could not be made";
    } else {
      std::vector<uint8_t> wav(44 + pcm.size() * 2);
      PutWavHeader(wav, theme_chime::kRate, 2, uint32_t(pcm.size() * 2));
      std::memcpy(&wav[44], pcm.data(), pcm.size() * 2);
      fs::create_directories(made.parent_path(), ec);
      const fs::path temp = made.string() + ".tmp";
      {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(wav.data()), std::streamsize(wav.size()));
        if (!f) why = "the file could not be written";
      }
      if (!why) {
        fs::rename(temp, made, ec);
        if (ec) why = "the file could not be written";
      }
      if (!why) {
        REXLOG_INFO("BF achievements: the chime made from the player's game files, {} ({} ms of sound, in {} ms): {}",
                    theme_chime::kRecipe.what, 1000ull * (pcm.size() / 2) / theme_chime::kRate,
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(),
                    made.string());
        return true;
      }
      fs::remove(temp, ec);
    }
  }
  static std::set<std::string> said;  // each reason once a run
  if (said.insert(why).second) REXLOG_WARN("BF achievements: the theme's chime was not made ({}): the plain chime plays", why);
  return false;
}

namespace {

// user\cache\extracted\achievements: the game's pictures and the copy of its catalog.
fs::path AchievementCacheDir() {
  fs::path dir = kit::MenuArtDir().empty() ? fs::path() : kit::MenuArtDir().parent_path();
  if (dir.empty()) {
    std::lock_guard lock(g_dir_mutex);
    if (!g_user_dir.empty()) dir = g_user_dir / "cache" / "extracted";
  }
  return dir.empty() ? dir : dir / "achievements";
}

// The SDK's unlock file (KernelState: user\achievements\<title id>.toml; Battlefield 1943 is 5841097E).
fs::path UnlockFile() {
  std::lock_guard lock(g_dir_mutex);
  return g_user_dir.empty() ? fs::path() : g_user_dir / "achievements" / "5841097E.toml";
}

std::string OneLine(std::string s) {
  for (char& c : s)
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  return s;
}

// The copy of the game's own catalog (catalog.txt): one line per achievement, its fields split by tabs: id,
// gamerscore, image id, flags, name, description, locked description. Reborn's own are not in it (code has them).
std::vector<rex::system::AchievementInfo> ReadCatalogCopy() {
  std::vector<rex::system::AchievementInfo> out;
  const fs::path dir = AchievementCacheDir();
  if (dir.empty()) return out;
  std::ifstream in(dir / "catalog.txt");
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f;
    for (size_t at = 0;;) {
      const size_t tab = line.find('\t', at);
      f.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
      if (tab == std::string::npos) break;
      at = tab + 1;
    }
    if (f.size() < 7) continue;
    rex::system::AchievementInfo a;
    a.id = uint32_t(std::strtoul(f[0].c_str(), nullptr, 10));
    a.gamerscore = uint32_t(std::strtoul(f[1].c_str(), nullptr, 10));
    a.image_id = uint32_t(std::strtoul(f[2].c_str(), nullptr, 10));
    a.flags = uint32_t(std::strtoul(f[3].c_str(), nullptr, 10));
    a.label = f[4], a.description = f[5], a.unachieved_description = f[6];
    if (a.id && !reborn_achievements::IsOurs(a.id)) out.push_back(std::move(a));
  }
  return out;
}

// The SDK's unlock file as it saves it: "[unlocked.<id>]" then "filetime = <n>".
std::map<uint32_t, uint64_t> ReadUnlockFile() {
  std::map<uint32_t, uint64_t> out;
  const fs::path file = UnlockFile();
  if (file.empty()) return out;
  std::ifstream in(file);
  std::string line;
  uint32_t id = 0;
  while (std::getline(in, line)) {
    if (line.rfind("[unlocked.", 0) == 0) {
      id = uint32_t(std::strtoul(line.c_str() + 10, nullptr, 10));
      if (id) out[id] = 0;
    } else if (id && line.rfind("filetime", 0) == 0) {
      const size_t eq = line.find('=');
      if (eq != std::string::npos) out[id] = std::strtoull(line.c_str() + eq + 1, nullptr, 10);
    }
  }
  return out;
}

// One of the game's pictures (an image of its title data, a PNG) written to <file>; empty when it was, else why not.
const char* WriteTitlePicture(rex::system::KernelState* k, uint32_t image_id, const fs::path& file) {
  const auto db = k->title_xdbf();
  if (!db.is_valid()) return "the game's title data is not there";
  const auto block = db.GetEntry(rex::system::util::XdbfSection::kImage, image_id);
  if (!block.buffer || block.size < 8) return "not in the title data";
  if (std::memcmp(block.buffer, "\x89PNG", 4)) return "not a PNG";
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  {
    std::ofstream png(file.string() + ".tmp", std::ios::binary | std::ios::trunc);
    png.write(reinterpret_cast<const char*>(block.buffer), std::streamsize(block.size));
  }
  fs::rename(file.string() + ".tmp", file, ec);
  return ec ? "the file could not be written" : nullptr;
}

// Once a run, when the game has its catalog: the copy written (only when it changed), and every picture of the game's
// that is not in the cache yet, for the next start's Home. The game runs hidden behind the shader page on a first start
// and after an update, then Home shows without it: the pictures are written here, not only when a page asks for one
// while the game runs (TekRantGaming, 16 Nov 2026: "achievement icons missing on main menu").
void NoteAchievementCatalog() {
  static bool done = false;
  if (done) return;
  auto* k = REX_KERNEL_STATE();
  if (!k) return;
  std::string text = "# The game's achievements as its title data has them (achievement_toast.cpp), for our menus "
                     "before the game runs.\n";
  std::set<uint32_t> images;
  int n = 0;
  for (const auto& a : k->achievements().ListAchievements()) {
    if (reborn_achievements::IsOurs(a.id)) continue;
    text += fmt::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\n", a.id, a.gamerscore, a.image_id, a.flags, OneLine(a.label),
                        OneLine(a.description), OneLine(a.unachieved_description));
    images.insert(a.image_id);
    ++n;
  }
  if (!n) return;
  done = true;
  const fs::path dir = AchievementCacheDir();
  if (dir.empty()) return;
  std::error_code ec;
  int written = 0, there = 0;
  std::string failed;
  for (uint32_t image_id : images) {
    const fs::path file = dir / (std::to_string(image_id) + ".png");
    if (fs::exists(file, ec)) {
      ++there;
      continue;
    }
    if (const char* why = WriteTitlePicture(k, image_id, file)) failed += fmt::format("{}{}: {}", failed.empty() ? "" : ", ", image_id, why);
    else ++written;
  }
  if (written || !failed.empty())
    REXLOG_INFO("BF achievements: {} of the game's {} pictures written for the menus before the game runs ({} already "
                "there){}{} ({})", written, images.size(), there, failed.empty() ? "" : "; not written: ", failed, dir.string());
  {
    std::ifstream in(dir / "catalog.txt", std::ios::binary);
    const std::string old((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (old == text) return;
  }
  fs::create_directories(dir, ec);
  {
    std::ofstream out((dir / "catalog.txt").string() + ".tmp", std::ios::binary | std::ios::trunc);
    out << text;
  }
  fs::rename((dir / "catalog.txt").string() + ".tmp", dir / "catalog.txt", ec);
  REXLOG_INFO("BF achievements: the game's {} kept for the menus before the game runs ({}){}", n,
              (dir / "catalog.txt").string(), ec ? ", could not be written" : "");
}

}  // namespace

std::vector<AchievementEntry> AchievementList(bool* from_game, bool* has_games) {
  std::vector<AchievementEntry> out;
  bool game = false, games = false;
  if (auto* k = REX_KERNEL_STATE()) {
    reborn_achievements::Ensure();
    auto& m = k->achievements();
    for (const auto& a : m.ListAchievements()) {
      AchievementEntry e;
      e.info = a;
      e.unlocked = m.IsUnlocked(a.id);
      e.when = e.unlocked ? m.GetUnlockTime(a.id) : 0;
      games |= !reborn_achievements::IsOurs(a.id);
      out.push_back(std::move(e));
    }
    game = !out.empty();
  }
  if (!game) {
    // Before the game runs: the copy and the unlock file, read again at most once a second (the page asks each frame).
    static std::mutex mutex;
    static double read_at = -100;
    static std::vector<AchievementEntry> copy;
    static bool copy_games = false;
    std::lock_guard lock(mutex);
    const double now = ImGui::GetTime();
    if (now - read_at > 1.0 || now < read_at) {
      read_at = now;
      copy.clear();
      std::vector<rex::system::AchievementInfo> infos = ReadCatalogCopy();
      copy_games = !infos.empty();
      for (auto& a : reborn_achievements::Infos()) infos.push_back(std::move(a));
      const std::map<uint32_t, uint64_t> unlocks = ReadUnlockFile();
      for (auto& a : infos) {
        AchievementEntry e;
        e.info = std::move(a);
        if (auto it = unlocks.find(e.info.id); it != unlocks.end()) e.unlocked = true, e.when = it->second;
        copy.push_back(std::move(e));
      }
    }
    out = copy, games = copy_games;
  }
  if (from_game) *from_game = game;
  if (has_games) *has_games = games;
  return out;
}

void SetAchievementUserDir(const fs::path& user_dir) {
  std::lock_guard lock(g_dir_mutex);
  if (g_user_dir != user_dir) g_user_dir = user_dir;  // the Achievements page sets it each frame
}

void PlayAchievementSound() {
  // (The user, 23 Nov 2026: "Add an option to toggle the Achievement sound": the popup without its chime.)
  if (!REXCVAR_GET(bf_achievement_sound)) return;
  const int volume = int(std::clamp(REXCVAR_GET(bf_achievement_volume), 0.0, 100.0) + 0.5);
  if (volume <= 0) return;
  // SDL's audio, as the menu sounds (game_music.cpp): a stream of its own, replaced by the next chime.
  static std::mutex mutex;
  static SDL_AudioStream* stream = nullptr;
  std::lock_guard lock(mutex);
  fs::path own;
  {
    std::lock_guard dir_lock(g_dir_mutex);
    if (!g_user_dir.empty()) own = g_user_dir / "sounds" / "achievement.wav";
  }
  std::error_code ec;
  std::vector<uint8_t> wav = !own.empty() && fs::exists(own, ec) ? LoadOwnWav(own, volume / 100.0f) : std::vector<uint8_t>{};
  // The player's own first; then the Battlefield theme's chime from their game files (kept in the cache by the
  // first start, made now if it is not there yet); then the plain chime.
  if (wav.empty()) {
    const fs::path achievements = AchievementCacheDir();
    if (!achievements.empty() && MakeAchievementChime(achievements.parent_path())) {
      const fs::path made = (achievements.parent_path() / theme_chime::kMade).make_preferred();
      wav = LoadOwnWav(made, volume / 100.0f);
      if (static bool said = false; !said && !wav.empty()) {
        said = true;
        REXLOG_INFO("BF achievements: the chime is {} ({})", theme_chime::kRecipe.what, made.string());
      }
    }
  }
  if (wav.empty()) wav = MakeChime(volume / 100.0f);
#if defined(BF_DEV_TOOLS)
  // BF_DEV_ACHIEVEMENT_WAV=<file> (tests, the PC kept silent): the chime written there in place of played.
  if (const char* out = std::getenv("BF_DEV_ACHIEVEMENT_WAV"); out && *out) {
    std::ofstream(out, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(wav.data()), std::streamsize(wav.size()));
    REXLOG_INFO("BF achievements: the chime ({} bytes, volume {}%) written to {} in place of played", wav.size(), volume, out);
    return;
  }
#endif
  if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXLOG_WARN("BF achievements: SDL's audio did not start ({})", SDL_GetError());
    return;
  }
  SDL_AudioSpec spec{};
  Uint8* pcm = nullptr;
  Uint32 pcm_len = 0;
  if (!SDL_LoadWAV_IO(SDL_IOFromConstMem(wav.data(), wav.size()), true, &spec, &pcm, &pcm_len)) return;
  if (stream) audio_devices::CloseStream(stream);
  // On the Game sound output device (Settings > Audio, audio_devices.h).
  stream = audio_devices::OpenStream(audio_devices::Role::kGame, spec, nullptr, nullptr, "the achievement chime");
  if (stream) {
    SDL_PutAudioStreamData(stream, pcm, int(pcm_len));
    SDL_FlushAudioStream(stream);
    REXLOG_INFO("BF achievements: the chime plays ({} ms, volume {}%)", 1000ull * pcm_len / std::max(1, spec.freq * spec.channels * 2), volume);
  } else {
    REXLOG_WARN("BF achievements: no sound device for the chime ({})", SDL_GetError());
  }
  SDL_free(pcm);
}

fs::path AchievementIconFile(uint32_t image_id) {
  // Reborn's own (reborn_achievements.h): our pictures, shipped next to the exe.
  if (reborn_achievements::IsOurs(image_id)) {
    std::error_code ec;
    const fs::path ours = reborn_achievements::IconFile(image_id);
    if (fs::exists(ours, ec)) return ours;
    if (static std::set<uint32_t> said; said.insert(image_id).second)
      REXLOG_WARN("BF achievements: no picture {} at {}", image_id, ours.string());
    return {};
  }
  // Beside the menu art (user\cache\extracted\achievements), made once from the game's title data.
  const fs::path dir = AchievementCacheDir();
  if (dir.empty()) return {};
  const fs::path file = dir / (std::to_string(image_id) + ".png");
  // Tried again every 2 s (the game's title data is there once the game runs), each reason logged once.
  static std::map<uint32_t, double> next_try;
  static std::set<std::string> said;
  std::error_code ec;
  if (fs::exists(file, ec)) return file;
  const double now = ImGui::GetTime();
  if (now < next_try[image_id]) return {};
  next_try[image_id] = now + 2.0;
  auto fail = [&](const char* why) {
    if (said.insert(std::to_string(image_id) + why).second) REXLOG_WARN("BF achievements: no picture {} yet: {}", image_id, why);
    return fs::path();
  };
  auto* k = REX_KERNEL_STATE();
  if (!k) return fail("the game is not running yet");
  if (const char* why = WriteTitlePicture(k, image_id, file)) return fail(why);
  REXLOG_INFO("BF achievements: picture {} from the game's title data", image_id);
  return file;
}

AchievementToast::AchievementToast(rex::ui::ImGuiDrawer* drawer) : AchievementNotificationDialog(drawer) {
  g_toast = this;
}

void AchievementToast::Push(const rex::system::AchievementEvent& event) {
  const auto& a = event.achievement;
  const bool show = REXCVAR_GET(bf_achievement_popups) || g_test.load();
  REXLOG_INFO("BF achievements: {} {} \"{}\" ({} G){}", g_test.load() ? "test popup for" : "unlocked", a.id, a.label,
              a.gamerscore, show ? "" : ", popups are off");
  if (!show) return;
  std::string title = a.label;
  while (!title.empty() && title.back() == ' ') title.pop_back();  // some of the game's names end in a space
  std::lock_guard lock(mutex_);
  if (queue_.size() < 16) queue_.push_back({title, a.description, a.gamerscore, a.image_id});
}

void TestAchievementPopup(uint32_t id) {
  if (!g_toast) return;
  // The game's catalog, or before the game runs its copy (AchievementList), Reborn's own too.
  for (const auto& entry : AchievementList()) {
    if (entry.info.id != id) continue;
    rex::system::AchievementEvent e;
    e.achievement = entry.info;
    g_test = true;
    g_toast->Push(e);
    g_test = false;
    return;
  }
}

void AchievementToast::OnDraw(ImGuiIO& io) {
  Toast toast;
  const double now = ImGui::GetTime();
  if (reborn_achievements::Ensure()) NoteAchievementCatalog();  // Reborn's own beside the game's; the copy kept
#if defined(BF_DEV_TOOLS)
  // BF_DEV_ACHIEVEMENT_TEST="<s>:<id>[,<s>:<id>...]" (tests): a test popup for achievement <id> <s> seconds after the
  // window opened, as the page's Y does.
  {
    static std::vector<std::pair<double, uint32_t>> due = [] {
      std::vector<std::pair<double, uint32_t>> out;
      if (const char* v = std::getenv("BF_DEV_ACHIEVEMENT_TEST")) {
        std::string s = v;
        size_t at = 0;
        while (at < s.size()) {
          const size_t comma = std::min(s.find(',', at), s.size()), colon = s.find(':', at);
          if (colon != std::string::npos && colon < comma)
            out.push_back({std::atof(s.substr(at, colon - at).c_str()), uint32_t(std::atoi(s.substr(colon + 1, comma - colon - 1).c_str()))});
          at = comma + 1;
        }
      }
      return out;
    }();
    for (auto it = due.begin(); it != due.end();) {
      if (now >= it->first) TestAchievementPopup(it->second), it = due.erase(it);
      else ++it;
    }
  }
#endif
  // The game's headline font, loaded before the first popup needs it (in a match our menus may not have asked yet).
  if (static bool font_ready = false; !font_ready) font_ready = kit::GameFontReady(kit::GameFont::kHeadline);
  {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) return;
    // Not over the shader page (TekRantGaming, 16 Nov 2026: the only screen visible until it is finished): one
    // unlocked meanwhile (the game signs in behind it after an update) waits, its chime too, and shows after it.
    if (queue_.front().started < 0 && ShaderScreenShowing()) return;
    if (queue_.front().started < 0) {
      queue_.front().started = now;
      PlayAchievementSound();
    }
    if (now - queue_.front().started > kTotal) {
      queue_.pop_front();
      return;
    }
    toast = queue_.front();
  }
  const float t = float(now - toast.started);
  // The menus' stage (1280 x 720, scaled to the screen), the slip at the screen's top right.
  const float u = std::max(0.1f, std::min(io.DisplaySize.y / 720.0f, io.DisplaySize.x / 1280.0f));
  auto S = [u](float v) { return v * u; };
  const kit::Fonts& f = kit::GetFonts();
  const float w = S(314), h = S(80), margin = S(28), top = S(64);
  float slide = 0;  // 0: in place; 1: just off the screen's right edge
  if (t < kIn) slide = 1 - EaseOut(t / float(kIn));
  else if (t > kTotal - kOut) slide = EaseIn((t - float(kTotal - kOut)) / float(kOut));
  const float x0 = io.DisplaySize.x - margin - w + slide * (w + margin + S(16));
  const ImVec2 a(x0, top), b(x0 + w, top + h);
  ImDrawList* d = ImGui::GetForegroundDrawList();
  d->AddRectFilled(ImVec2(a.x + S(3), a.y + S(6)), ImVec2(b.x + S(3), b.y + S(8)), IM_COL32(0, 0, 0, 80));
  d->AddRectFilled(ImVec2(a.x + S(1), a.y + S(2)), ImVec2(b.x + S(1), b.y + S(3)), IM_COL32(0, 0, 0, 60));
  kit::PaperArt(d, a, b);
  d->AddRectFilled(a, ImVec2(a.x + S(7), b.y), kit::kStamp);
  // The achievement's picture, with the green tick.
  const float is = S(54);
  const ImVec2 i0(a.x + S(19), a.y + (h - is) * 0.5f), i1(i0.x + is, i0.y + is);
  d->AddRectFilled(ImVec2(i0.x + S(1), i0.y + S(2)), ImVec2(i1.x + S(1), i1.y + S(2)), IM_COL32(0, 0, 0, 70));
  const fs::path icon = AchievementIconFile(toast.image_id);
  if (icon.empty() || !kit::DrawImageFile(d, icon, i0, i1, true)) d->AddRectFilled(i0, i1, kit::kPaperDim);
  {
    const ImVec2 c(i1.x - S(2), i1.y - S(2));
    d->AddCircleFilled(c, S(10.5f), kit::kGreen, 20);
    d->AddLine(ImVec2(c.x - S(4.5f), c.y + S(0.5f)), ImVec2(c.x - S(1.2f), c.y + S(3.8f)), IM_COL32(255, 255, 255, 255), S(2.2f));
    d->AddLine(ImVec2(c.x - S(1.2f), c.y + S(3.8f)), ImVec2(c.x + S(4.8f), c.y - S(3.5f)), IM_COL32(255, 255, 255, 255), S(2.2f));
  }
  // The words: ACHIEVEMENT UNLOCKED, its name in the game's headline font, what it is for and its gamerscore.
  const float tx = i1.x + S(14), tw = b.x - S(14) - tx;
  d->PushClipRect(ImVec2(tx, a.y), ImVec2(b.x - S(10), b.y), true);
  kit::Spaced(d, f.typed, S(8.5f), ImVec2(tx, a.y + S(10)), kit::kStamp, "ACHIEVEMENT UNLOCKED", S(2));
  std::string upper = toast.title;
  for (char& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
  float cap = S(17);
  const bool headline = kit::GameFontReady(kit::GameFont::kHeadline);
  if (static bool said = false; !said && t > 1.0f) {
    said = true;
    REXLOG_INFO("BF achievements: popup drawn, {} headline font, picture {}", headline ? "the game's" : "our stand in",
                icon.empty() ? "missing" : icon.string());
  }
  if (headline) {
    const float fw = kit::GameTextWidth(kit::GameFont::kHeadline, cap, upper);
    if (fw > tw) cap *= tw / fw;
    kit::GameText(d, kit::GameFont::kHeadline, cap, ImVec2(tx, a.y + S(26) + (S(17) - cap) * 0.5f), kit::kInk, upper);
  } else {
    d->AddText(f.ui_bold, S(19), ImVec2(tx, a.y + S(23)), kit::kInk, upper.c_str());
  }
  const std::string g = fmt::format("  \xC2\xB7  {} G", toast.gamerscore);
  const float gw = kit::TextWidth(f.ui_bold, S(11), g);
  std::string desc = toast.description;
  while (!desc.empty() && kit::TextWidth(f.ui, S(11), desc) > tw - gw) desc.pop_back();
  if (desc.size() < toast.description.size() && desc.size() > 3) desc = desc.substr(0, desc.size() - 3) + "...";
  d->AddText(f.ui, S(11), ImVec2(tx, a.y + S(53)), kit::kInkFaint, desc.c_str());
  d->AddText(f.ui_bold, S(11), ImVec2(tx + kit::TextWidth(f.ui, S(11), desc), a.y + S(53)), kit::kInk, g.c_str());
  d->PopClipRect();
}

}  // namespace bf1943
