// The game folder's choice (bf/src/game_install.h): the title ID read from a default.xex header (XexTitleId), another
// game's folder told apart from ours (OtherGame), and "game" or "bf1943-game" beside the program (GameFolderIn). King
// Kong Recompiled's AppImage keeps its own "game" folder beside itself as ours does, so with both in Downloads ours took
// King Kong's files for an installed game (TekRantGaming's ROG Ally, 11 Nov 2026). Synthetic XEX2 headers in a temp folder;
// any further arguments are real default.xex files, whose titles are printed.
//
// Build (no game, no SDK): clang++ -std=c++20 -I bf/src bf/tests/game_folder_tests.cpp
// Usage: game_folder_tests <scratch folder> [default.xex ...]

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "game_install.h"

namespace fs = std::filesystem;
using namespace bf1943::install;

static int failures = 0;
#define CHECK(x)                                                    \
  do {                                                              \
    if (!(x)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);      \
      ++failures;                                                   \
    }                                                               \
  } while (0)

static void Put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  if (b.size() < at + 4) b.resize(at + 4);
  b[at] = uint8_t(v >> 24), b[at + 1] = uint8_t(v >> 16), b[at + 2] = uint8_t(v >> 8), b[at + 3] = uint8_t(v);
}

// An XEX2 header with two optional headers (another one first, then the execution info at 0x80), as the console's
// tools write them.
static std::vector<uint8_t> Xex(uint32_t title, bool with_info = true, size_t size = 0x100) {
  std::vector<uint8_t> b(size, 0);
  Put32(b, 0x00, 0x58455832);  // "XEX2"
  Put32(b, 0x08, 0x2000);      // the PE data's offset
  Put32(b, 0x10, 0x100);       // the security info's offset
  Put32(b, 0x14, with_info ? 2 : 1);
  Put32(b, 0x18, 0x000002FF), Put32(b, 0x1C, 0x60);  // the resource info, say
  if (with_info) Put32(b, 0x20, 0x00040006), Put32(b, 0x24, 0x80);
  if (b.size() >= 0x98) {
    Put32(b, 0x80, 0x12345678);  // media ID
    Put32(b, 0x84, 0x00000001);  // version
    Put32(b, 0x88, 0x00000000);  // base version
    Put32(b, 0x8C, title);
  }
  b.resize(size);
  return b;
}

static void Write(const fs::path& p, const std::vector<uint8_t>& b) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: game_folder_tests <scratch folder> [default.xex ...]\n");
    return 2;
  }
  const fs::path scratch = fs::path(argv[1]) / "game_folder_tests";
  std::error_code ec;
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch);
  constexpr uint32_t kOther = 0x4D5307E6;  // any other title

  // The title read.
  Write(scratch / "x" / "bf.xex", Xex(kTitleId));
  Write(scratch / "x" / "other.xex", Xex(kOther));
  Write(scratch / "x" / "noinfo.xex", Xex(kTitleId, false));
  Write(scratch / "x" / "cut.xex", Xex(kTitleId, true, 0x88));  // the execution info past the file's end
  Write(scratch / "x" / "junk.xex", std::vector<uint8_t>(0x100, 0x41));
  Write(scratch / "x" / "empty.xex", {});
  CHECK(XexTitleId(scratch / "x" / "bf.xex") == kTitleId);
  CHECK(XexTitleId(scratch / "x" / "other.xex") == kOther);
  CHECK(XexTitleId(scratch / "x" / "noinfo.xex") == 0);
  CHECK(XexTitleId(scratch / "x" / "cut.xex") == 0);
  CHECK(XexTitleId(scratch / "x" / "junk.xex") == 0);
  CHECK(XexTitleId(scratch / "x" / "empty.xex") == 0);
  CHECK(XexTitleId(scratch / "x" / "missing.xex") == 0);

  // Another game's folder: only a default.xex that names another title.
  Write(scratch / "o" / "ours" / "default.xex", Xex(kTitleId));
  Write(scratch / "o" / "theirs" / "default.xex", Xex(kOther));
  Write(scratch / "o" / "unreadable" / "default.xex", std::vector<uint8_t>(0x100, 0x41));
  fs::create_directories(scratch / "o" / "partial" / "maps");
  CHECK(!OtherGame(scratch / "o" / "ours"));
  CHECK(OtherGame(scratch / "o" / "theirs"));
  CHECK(!OtherGame(scratch / "o" / "unreadable"));  // counts as ours: a read error never sends a player to the setup
  CHECK(!OtherGame(scratch / "o" / "partial"));      // an install cut short (default.xex is written last)
  CHECK(!OtherGame(scratch / "o" / "missing"));
  CHECK(!OtherGame(fs::path()));

  // The folder beside the program.
  const fs::path r1 = scratch / "r1";  // nothing yet: "game"
  fs::create_directories(r1);
  CHECK(GameFolderIn(r1) == r1 / "game");
  const fs::path r2 = scratch / "r2";  // ours in "game"
  Write(r2 / "game" / "default.xex", Xex(kTitleId));
  CHECK(GameFolderIn(r2) == r2 / "game");
  const fs::path r3 = scratch / "r3";  // King Kong's "game" (Downloads): ours goes in "bf1943-game"
  Write(r3 / "game" / "default.xex", Xex(kOther));
  CHECK(GameFolderIn(r3) == r3 / "bf1943-game");
  CHECK(!Installed(GameFolderIn(r3)));  // so the setup asks for the player's files
  Write(r3 / "bf1943-game" / "default.xex", Xex(kTitleId));  // installed there
  CHECK(GameFolderIn(r3) == r3 / "bf1943-game");
  CHECK(Installed(GameFolderIn(r3)));
  const fs::path r4 = scratch / "r4";  // ours in "bf1943-game", the other game since removed
  Write(r4 / "bf1943-game" / "default.xex", Xex(kTitleId));
  CHECK(GameFolderIn(r4) == r4 / "bf1943-game");
  const fs::path r5 = scratch / "r5";  // ours in both: "game" first
  Write(r5 / "game" / "default.xex", Xex(kTitleId));
  Write(r5 / "bf1943-game" / "default.xex", Xex(kTitleId));
  CHECK(GameFolderIn(r5) == r5 / "game");
  const fs::path r6 = scratch / "r6";  // an install cut short in "game": it carries on there
  fs::create_directories(r6 / "game" / "maps");
  CHECK(GameFolderIn(r6) == r6 / "game");
  const fs::path r7 = scratch / "r7";  // a "game" whose header cannot be read: ours, as before
  Write(r7 / "game" / "default.xex", std::vector<uint8_t>(0x100, 0x41));
  CHECK(GameFolderIn(r7) == r7 / "game");

  // Real files, when given.
  for (int i = 2; i < argc; ++i) {
    const fs::path xex = argv[i];
    std::printf("%s: title %08X, %s\n", xex.string().c_str(), XexTitleId(xex),
                OtherGame(xex.parent_path()) ? "another game" : "ours (or unreadable)");
  }

  fs::remove_all(scratch, ec);
  if (failures) std::printf("game_folder_tests: %d FAILED\n", failures);
  else std::printf("game_folder_tests: all OK\n");
  return failures ? 1 : 0;
}
