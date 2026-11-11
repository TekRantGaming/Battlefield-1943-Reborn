// Installing the player's own Battlefield 1943 (Xbox 360) into the game folder, once (game_install.cpp): the first
// start's setup page (game_menu.cpp) and the server package's install (--install_game, server_setup.ps1).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace bf1943::install {

constexpr uint32_t kTitleId = 0x5841097E;

// What the player chose: an STFS package (CON, LIVE or PIRS), or a folder that already holds the game (default.xex).
struct Source {
  bool ok = false;
  bool extracted = false;  // a folder with default.xex: used as it is, nothing copied
  std::filesystem::path path;  // the package file, or the game folder
  std::string error;           // why it cannot be used (for the player)
  std::string name;            // the package's display name
};
// A file or a folder the player picked: the package itself, a folder holding one (searched for 5841097E\000D0000),
// default.xex or a folder with it.
Source Inspect(const std::filesystem::path& picked);

struct Progress {
  std::atomic<uint64_t> done{0}, total{0};  // bytes
  std::atomic<bool> finished{false}, failed{false};
  std::mutex mutex;
  std::string error;  // with failed
};
// Writes the package's files into `out` (made if needed; a failed install leaves no default.xex), checking every
// block's SHA1. Runs on the calling thread.
bool Extract(const std::filesystem::path& package, const std::filesystem::path& out, Progress& progress);

// Whether `dir` holds an installed game.
inline bool Installed(const std::filesystem::path& dir) {
  std::error_code ec;
  return !dir.empty() && std::filesystem::exists(dir / "default.xex", ec);
}

// The title ID in an Xbox 360 executable's header (XEX2: its execution info, optional header 0x00040006: media ID,
// version, base version, then the title ID), or 0 when it cannot be read.
inline uint32_t XexTitleId(const std::filesystem::path& xex) {
  std::ifstream f(xex, std::ios::binary);
  auto be32 = [](const unsigned char* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
  };
  unsigned char h[0x18];
  if (!f.read(reinterpret_cast<char*>(h), sizeof(h)) || be32(h) != 0x58455832) return 0;  // "XEX2"
  const uint32_t count = be32(h + 0x14);
  for (uint32_t i = 0; i < count && i < 256; ++i) {
    unsigned char e[8];
    if (!f.read(reinterpret_cast<char*>(e), sizeof(e))) return 0;
    if (be32(e) != 0x00040006) continue;
    unsigned char info[0x10];
    if (!f.seekg(std::streamoff(be32(e + 4))) || !f.read(reinterpret_cast<char*>(info), sizeof(info))) return 0;
    return be32(info + 0x0C);
  }
  return 0;
}

// Whether `dir` holds another game: a default.xex whose title is not Battlefield 1943's. King Kong Recompiled's
// AppImage keeps its own "game" folder beside itself, as ours does: with both AppImages in Downloads, ours took King
// Kong's files for an installed game and skipped the setup (TekRantGaming's ROG Ally, 11 Nov 2026). A header that cannot be
// read counts as ours, so a passing read error never sends a player back to the setup.
inline bool OtherGame(const std::filesystem::path& dir) {
  if (!Installed(dir)) return false;
  const uint32_t title = XexTitleId(dir / "default.xex");
  return title != 0 && title != kTitleId;
}

// The game folder in `root` (beside the program, or on Linux beside the AppImage): "game", or "bf1943-game" while
// "game" holds another game (or holds nothing, and ours was installed there before).
inline std::filesystem::path GameFolderIn(const std::filesystem::path& root) {
  const std::filesystem::path game = root / "game", ours = root / "bf1943-game";
  return OtherGame(game) || (!Installed(game) && Installed(ours)) ? ours : game;
}

}  // namespace bf1943::install

namespace bf1943::install {
// --install_game: installs into game_dir and returns the process's exit code.
int InstallFromCommandLine(const std::filesystem::path& game_dir);
}  // namespace bf1943::install
