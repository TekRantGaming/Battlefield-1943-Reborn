// The player's Xbox 360 package into the game folder (game_install.h). The STFS reader of tools\stfs_extract.py in
// C++, written for this project from the public format description (Free60 wiki): the file table, each file's
// block chain through the level 0 hash tables, every data block checked against its SHA1 entry.

#include "game_install.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

#include <rex/logging.h>

namespace bf1943::install {
namespace {
namespace fs = std::filesystem;

constexpr uint32_t kBlock = 0x1000;
constexpr uint32_t kPerTable = 0xAA;
constexpr uint32_t kL1Span = 0xAA * 0xAA;

uint32_t U24Le(const uint8_t* b) { return b[0] | b[1] << 8 | b[2] << 16; }
uint32_t U24Be(const uint8_t* b) { return b[0] << 16 | b[1] << 8 | b[2]; }
uint32_t U32Be(const uint8_t* b) { return uint32_t(b[0]) << 24 | b[1] << 16 | b[2] << 8 | b[3]; }

bool Sha1(const uint8_t* data, size_t size, uint8_t out[20]) {
#if defined(_WIN32)
  return BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, const_cast<uint8_t*>(data), ULONG(size), out, 20));
#else
  (void)data, (void)size, (void)out;
  return false;
#endif
}

class Stfs {
 public:
  bool Open(const fs::path& path, std::string& error) {
    f_.open(path, std::ios::binary);
    if (!f_) return error = "The file cannot be opened.", false;
    std::vector<uint8_t> h(0xB000);
    f_.read(reinterpret_cast<char*>(h.data()), std::streamsize(h.size()));
    if (f_.gcount() < 0x1000) return error = "This is not an Xbox 360 package.", false;
    if (std::memcmp(h.data(), "CON ", 4) && std::memcmp(h.data(), "LIVE", 4) && std::memcmp(h.data(), "PIRS", 4))
      return error = "This is not an Xbox 360 package.", false;
    header_size_ = U32Be(&h[0x340]);
    title_id_ = U32Be(&h[0x360]);
    const uint8_t* vd = &h[0x379];
    const uint8_t sep = vd[2];
    ft_blocks_ = vd[3] | vd[4] << 8;
    ft_start_ = U24Le(vd + 5);
    shift_ = (sep & 1) ? 0 : 1;
    first_table_ = (uint64_t(header_size_) + 0xFFF) & ~uint64_t(0xFFF);
    step0_ = shift_ == 0 ? 0xAB : 0xAC;
    for (int i = 0; i < 0x40; ++i) {
      const uint16_t c = uint16_t(h[0x411 + 2 * i] << 8 | h[0x412 + 2 * i]);
      if (!c) break;
      name_ += c < 0x80 ? char(c) : '?';
    }
    f_.clear();
    return true;
  }
  uint32_t title_id() const { return title_id_; }
  const std::string& name() const { return name_; }

  struct Entry {
    std::string name;
    bool dir = false, consecutive = false;
    uint32_t blocks = 0, start = 0, size = 0;
    int16_t parent = -1;
  };
  bool Files(std::vector<Entry>& out) {
    std::vector<uint8_t> raw;
    uint32_t n = ft_start_;
    for (uint32_t i = 0; i < ft_blocks_; ++i) {
      std::array<uint8_t, kBlock> b;
      if (!Read(n, b.data())) return false;
      raw.insert(raw.end(), b.begin(), b.end());
      n = HashNext(n);
    }
    for (size_t i = 0; i + 0x40 <= raw.size(); i += 0x40) {
      const uint8_t* e = &raw[i];
      if (!e[0]) break;
      Entry x;
      const uint8_t flags = e[0x28];
      x.name.assign(reinterpret_cast<const char*>(e), flags & 0x3F);
      x.dir = flags & 0x80, x.consecutive = flags & 0x40;
      x.blocks = U24Le(e + 0x29), x.start = U24Le(e + 0x2F);
      x.parent = int16_t(e[0x32] << 8 | e[0x33]);
      x.size = U32Be(e + 0x34);
      out.push_back(x);
    }
    return true;
  }
  bool Read(uint32_t n, uint8_t* out) {
    f_.seekg(std::streamoff(first_table_ + uint64_t(Backing(n)) * kBlock));
    f_.read(reinterpret_cast<char*>(out), kBlock);
    const auto got = f_.gcount();
    if (got < std::streamsize(kBlock)) std::memset(out + got, 0, size_t(kBlock - got));
    f_.clear();
    return got > 0;
  }
  // The block's SHA1 and the next block of its chain, from its level 0 hash entry.
  bool Hash(uint32_t n, uint8_t sha[20], uint32_t& next) {
    uint8_t e[0x18];
    f_.seekg(std::streamoff(first_table_ + uint64_t(L0(n)) * kBlock + (n % kPerTable) * 0x18));
    f_.read(reinterpret_cast<char*>(e), sizeof(e));
    f_.clear();
    std::memcpy(sha, e, 20);
    next = U24Be(e + 21);
    return true;
  }
  uint32_t HashNext(uint32_t n) {
    uint8_t sha[20];
    uint32_t next = 0;
    Hash(n, sha, next);
    return next;
  }

 private:
  uint32_t Backing(uint32_t n) const {
    const uint32_t r = (((n + kPerTable) / kPerTable) << shift_) + n;
    if (n < kPerTable) return r;
    if (n < kL1Span) return r + (((n + kL1Span) / kL1Span) << shift_);
    return (1u << shift_) + r + (((n + kL1Span) / kL1Span) << shift_);
  }
  uint32_t L0(uint32_t n) const {
    if (n < kPerTable) return 0;
    uint32_t r = (n / kPerTable) * step0_;
    r += ((n / kL1Span) + 1) << shift_;
    if (n / kL1Span == 0) return r;
    return r + (1u << shift_);
  }
  std::ifstream f_;
  uint32_t header_size_ = 0, title_id_ = 0, ft_blocks_ = 0, ft_start_ = 0, shift_ = 1, step0_ = 0xAC;
  uint64_t first_table_ = 0;
  std::string name_;
};

// The first file in `dir` (or under 5841097E\000D0000 below it) that is a package of the game.
fs::path FindPackage(const fs::path& dir) {
  std::error_code ec;
  for (const fs::path& d : {dir / "5841097E" / "000D0000", dir / "000D0000", dir}) {
    if (!fs::is_directory(d, ec)) continue;
    for (const auto& e : fs::directory_iterator(d, ec)) {
      if (!e.is_regular_file(ec) || e.file_size(ec) < 0x10000) continue;
      Stfs s;
      std::string error;
      if (s.Open(e.path(), error) && s.title_id() == kTitleId) return e.path();
    }
  }
  return {};
}
}  // namespace

Source Inspect(const fs::path& picked) {
  Source s;
  std::error_code ec;
  fs::path p = picked;
  if (p.filename() == "default.xex") p = p.parent_path();
  if (fs::is_directory(p, ec)) {
    if (OtherGame(p)) {  // King Kong Recompiled's game folder, say
      char b[160];
      std::snprintf(b, sizeof(b), "That folder holds another game (title %08X), not Battlefield 1943 (5841097E).",
                    XexTitleId(p / "default.xex"));
      s.error = b;
      return s;
    }
    if (Installed(p)) return s.ok = s.extracted = true, s.path = p, s.name = "Battlefield 1943", s;
    p = FindPackage(p);
    if (p.empty()) {
      s.error = "No Battlefield 1943 package in that folder. Select the package file itself (in 5841097E\\000D0000).";
      return s;
    }
  }
  Stfs stfs;
  if (!stfs.Open(p, s.error)) return s;
  if (stfs.title_id() != kTitleId) {
    char b[160];
    std::snprintf(b, sizeof(b), "This package is another game (title %08X), not Battlefield 1943 (5841097E).",
                  stfs.title_id());
    s.error = b;
    return s;
  }
  s.ok = true, s.path = p, s.name = stfs.name();
  return s;
}

bool Extract(const fs::path& package, const fs::path& out, Progress& progress) {
  auto fail = [&](const std::string& why) {
    std::lock_guard lock(progress.mutex);
    progress.error = why;
    progress.failed = true;
    progress.finished = true;
    REXLOG_ERROR("BF install: {}", why);
    return false;
  };
  Stfs s;
  std::string error;
  if (!s.Open(package, error)) return fail(error);
  std::vector<Stfs::Entry> entries;
  if (!s.Files(entries) || entries.empty()) return fail("The package's file list cannot be read.");
  uint64_t total = 0;
  for (const auto& e : entries) total += e.dir ? 0 : e.size;
  progress.total = total;
  std::error_code ec;
  fs::create_directories(out, ec);
  std::vector<uint8_t> block(kBlock);
  uint64_t bad = 0;
  // Paths by the package's own entry order (a parent comes before its children); default.xex written last, so an
  // install cut short is never taken for a finished one.
  const std::vector<Stfs::Entry>& original = entries;
  std::vector<fs::path> paths(original.size());
  for (size_t i = 0; i < original.size(); ++i) {
    const auto& e = original[i];
    const fs::path parent = e.parent >= 0 && size_t(e.parent) < i ? paths[size_t(e.parent)] : fs::path();
    paths[i] = parent / e.name;
  }
  std::vector<size_t> order(original.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_partition(order.begin(), order.end(), [&](size_t i) { return original[i].name != "default.xex"; });
  for (size_t i : order) {
    const auto& e = original[i];
    const fs::path dest = out / paths[i];
    if (e.dir) {
      fs::create_directories(dest, ec);
      continue;
    }
    fs::create_directories(dest.parent_path(), ec);
    std::ofstream o(dest, std::ios::binary | std::ios::trunc);
    if (!o) return fail("Cannot write " + paths[i].string() + " (is the folder writable and the disk not full?).");
    uint32_t n = e.start, left = e.size;
    for (uint32_t b = 0; b < e.blocks && left; ++b) {
      if (!s.Read(n, block.data())) return fail("The package is cut short (" + paths[i].string() + ").");
      uint8_t want[20], got[20];
      uint32_t next = 0;
      s.Hash(n, want, next);
      if (Sha1(block.data(), kBlock, got) && std::memcmp(want, got, 20)) ++bad;
      const uint32_t chunk = std::min(left, kBlock);
      o.write(reinterpret_cast<const char*>(block.data()), chunk);
      left -= chunk;
      progress.done += chunk;
      n = e.consecutive ? n + 1 : next;
    }
    if (!o) return fail("Cannot write " + paths[i].string() + " (is the disk full?).");
  }
  if (bad) {
    fs::remove(out / "default.xex", ec);
    return fail("The package is damaged: " + std::to_string(bad) + " blocks do not match their checksums.");
  }
  REXLOG_INFO("BF install: {} files, {} MB from {} into {}", original.size(), total >> 20, package.filename().string(),
              out.string());
  progress.finished = true;
  return true;
}

}  // namespace bf1943::install

// ---- the server package's install, without a window: bf1943.exe --install_game=<package or folder>
// [--game_data_root=<folder>]. Progress and the result on stdout (server_setup.ps1 reads them); the exit code is 0
// when the game is installed.
#include <rex/cvar.h>
REXCVAR_DEFINE_STRING(install_game, "", "BF1943",
                      "Install the game from this package (or a folder holding it) into the game folder, then exit");

namespace bf1943::install {
int InstallFromCommandLine(const std::filesystem::path& game_dir) {
  const Source source = Inspect(REXCVAR_GET(install_game));
  if (!source.ok) {
    std::printf("error: %s\n", source.error.c_str());
    return 1;
  }
  if (source.extracted) {
    std::printf("installed: %s\n", source.path.string().c_str());
    return 0;
  }
  Progress progress;
  std::thread([&] { Extract(source.path, game_dir, progress); }).join();
  if (progress.failed) {
    std::printf("error: %s\n", progress.error.c_str());
    return 1;
  }
  std::printf("installed: %s\n", game_dir.string().c_str());
  return 0;
}
}  // namespace bf1943::install
