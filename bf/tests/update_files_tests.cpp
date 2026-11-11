// The updater's file work (bf/src/update_files.h) on real files in a temp folder, King Kong's tests
// (kk/tests/update_files_tests.cpp) with our names: bf-files.txt and bf-retired.txt parsing and unsafe paths,
// protected paths, SHA256SUMS.txt lines, the install check, the staged install with its undo (a locked file on
// Windows), retired files (only unchanged ones, empty folders after), the first start check's install from a
// downloaded copy; and the release feed: a real GitHub answer (data/github-release-v0.1.33-preview.json) and the
// version order.
//
// Build (no game, no SDK): clang++ -std=c++20 -I bf/src bf/tests/update_files_tests.cpp bf/src/update_files.cpp
// Usage: update_files_tests <scratch folder> <bf/tests/data folder>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "update_files.h"

namespace {

namespace fs = std::filesystem;
using namespace bf1943::update;

int failures = 0;
void Check(bool ok, const std::string& what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}

void Put(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
}

std::string Get(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string Line(const std::string& path, const std::string& text) {
  return Sha256Of(text.data(), text.size()) + " " + std::to_string(text.size()) + " " + path + "\n";
}

bool Has(const std::vector<std::string>& v, const std::string& s) {
  for (const auto& x : v)
    if (x == s) return true;
  return false;
}

void ParseTests() {
  const std::string good = "\xEF\xBB\xBF# Battlefield 1943 Reborn file list\r\nversion 0.2.0\r\n" + Line("bf1943.exe", "exe") +
                           Line("data/shaders/a b.nxr", "x") + "\n";
  std::string error;
  const auto m = ParseManifest(good, &error);
  Check(m.has_value(), "good list parses: " + error);
  if (m) {
    Check(m->version == "0.2.0", "version line");
    Check(m->files.size() == 2, "two files");
    Check(m->files[1].path == "data/shaders/a b.nxr", "path with spaces kept");
    Check(m->files[0].size == 3, "size");
    Check(m->Find("BF1943.EXE") != nullptr, "Find ignores letter case");
    Check(m->Find("missing.dll") == nullptr, "Find: not listed");
  }
  const std::string sha = std::string(64, 'a');
  for (const char* bad : {"../x", "/x", "a\\b", "c:x", "a//b", "a/./b", "a/../b", "a/", ""}) {
    Check(!ParseManifest(sha + " 1 " + bad + "\n").has_value(), std::string("unsafe path refused: ") + bad);
    Check(!SafePath(bad), std::string("SafePath refuses ") + bad);
  }
  Check(!ParseManifest("abc 1 x\n").has_value(), "short hash refused");
  Check(!ParseManifest(sha + " 1x x\n").has_value(), "bad size refused");
  Check(!ParseManifest(sha + " 1\n").has_value(), "no path refused");
  Check(ParseManifest(std::string(64, 'A') + " 1 x\n")->files[0].sha256 == sha, "hash lowercased");
  Check(SafePath("data/shaders/bf1943-shaders.list") && SafePath("bf-files.txt"), "SafePath allows normal paths");
}

void ProtectedTests() {
  const Protected keep({"My Data", "games/bf/"});
  for (const char* p : {"game/default.xex", "GAME/x", "user/bf1943-settings.txt", "User/logs/a.log", "cache/a",
                        "logs/a.log", "crashes/c.dmp", "my data/x", "games/bf/default.xex"})
    Check(keep(p), std::string("protected: ") + p);
  for (const char* p : {"bf1943.exe", "bf1943.toml", "data/shaders/x.nxr", "fonts/Oswald.ttf", "game", "user",
                        "my datax/y", "glyphs/a.png", "gamex/a", "licences/x.txt"})
    Check(!keep(p), std::string("not protected: ") + p);
}

void SumsTests() {
  const std::string h1 = std::string(64, 'b'), h2 = std::string(63, 'c') + "D";
  const std::string sums = h1 + "  BF1943-Reborn-0.2.0-windows-x64.zip\r\n" + h2 + " *other.zip\n";
  Check(SumFor(sums, "BF1943-Reborn-0.2.0-windows-x64.zip") == h1, "SHA256SUMS two spaces");
  Check(SumFor(sums, "other.zip") == std::string(63, 'c') + "d", "SHA256SUMS star form, lowercased");
  Check(SumFor(sums, "missing.zip").empty(), "SHA256SUMS: no line");
  Check(Sha256Of("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");
  Check(Sha256Of("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 empty");
  const std::string long_text = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  Check(Sha256Of(long_text.data(), long_text.size()) ==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "sha256 two blocks");
  std::string million(1000000, 'a');
  Check(Sha256Of(million.data(), million.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        "sha256 a million a");
}

Manifest MakeList(const std::vector<std::pair<std::string, std::string>>& files, const std::string& version = "1") {
  std::string text = "version " + version + "\n";
  for (const auto& [p, t] : files) text += Line(p, t);
  return *ParseManifest(text);
}

void CheckTests(const fs::path& dir) {
  const fs::path root = dir / "check";
  Put(root / "a.dll", "AAAA");
  Put(root / "b.dll", "BBBB");        // same size as listed, other content
  Put(root / "big.bin", "XXXXXXXX");  // same size, other content, above the hash limit used below
  Put(root / "c.txt", "short");
  const Manifest m = MakeList({{"a.dll", "AAAA"}, {"b.dll", "bbbb"}, {"big.bin", "YYYYYYYY"}, {"c.txt", "longer"},
                               {"missing/d.txt", "d"}, {"game/default.xex", "g"}, {"user/x.txt", "u"}});
  const Protected keep;
  auto got = MissingOrWrong(root, m, keep, 4);
  Check(!Has(got, "a.dll"), "check: unchanged file passes");
  Check(Has(got, "b.dll"), "check: small file with other content found by its hash");
  Check(!Has(got, "big.bin"), "check: large file with the right size passes the cheap check");
  Check(Has(got, "c.txt") && Has(got, "missing/d.txt"), "check: wrong size and missing found");
  Check(!Has(got, "game/default.xex") && !Has(got, "user/x.txt"), "check: protected skipped");
  got = MissingOrWrong(root, m, keep, UINT64_MAX);
  Check(Has(got, "big.bin"), "check: full hash finds the large file");
}

void InstallTests(const fs::path& dir) {
  const fs::path src = dir / "src", dst = dir / "dst";
  Put(src / "bf1943.exe", "new exe");
  Put(src / "data/shaders/a.nxr", "new shaders");
  Put(src / "newtop/readme.txt", "new folder");
  Put(dst / "bf1943.exe", "old exe");
  Put(dst / "data/shaders/a.nxr", "old shaders");
  Put(dst / "player.txt", "mine");
  Put(dst / "game/default.xex", "game");
  const std::vector<std::string> plan = {"bf1943.exe", "data/shaders/a.nxr", "newtop/readme.txt"};
  auto r = InstallFiles(src, dst, plan);
  Check(r.outcome == Outcome::kOk, "install: ok");
  Check(Get(dst / "bf1943.exe") == "new exe" && Get(dst / "data/shaders/a.nxr") == "new shaders" &&
            Get(dst / "newtop/readme.txt") == "new folder",
        "install: every file is the new one, the new top folder too");
  Check(Get(dst / "player.txt") == "mine" && Get(dst / "game/default.xex") == "game", "install: others untouched");
  Check(!fs::exists(dst / "bf1943.exe.old") && !fs::exists(dst / "bf1943.exe.new"), "install: no leftovers");

  // A file that cannot be written: the failure says which file and the system's reason.
  fs::create_directories(dst / "bf1943.exe.new" / "in_the_way");
  r = InstallFiles(src, dst, {"bf1943.exe"});
  Check(r.outcome == Outcome::kWrite && r.path == "bf1943.exe", "install: a write failure is reported");
  Check(!r.error.empty(), "install: a write failure says why");
  fs::remove_all(dst / "bf1943.exe.new");

  // A file the update lacks: nothing changes, no .new files, no new folders.
  Put(dst / "bf1943.exe", "old exe");
  r = InstallFiles(src, dst, {"bf1943.exe", "brandnew/x.txt", "missing.dll"});
  Check(r.outcome == Outcome::kNoSource && r.path == "brandnew/x.txt", "install: missing source refused");
  Check(Get(dst / "bf1943.exe") == "old exe" && !fs::exists(dst / "bf1943.exe.new"),
        "install: nothing changed after a missing source");

#if defined(_WIN32)
  // A locked file in the middle: the files before it were swapped in and must be put back.
  fs::remove_all(dst / "newtop");
  Put(dst / "data/shaders/a.nxr", "old shaders");
  Put(src / "zzz/after.txt", "after");
  const HANDLE lock = CreateFileW((dst / "data/shaders/a.nxr").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(lock != INVALID_HANDLE_VALUE, "lock test: file opened");
  r = InstallFiles(src, dst, {"bf1943.exe", "newtop/readme.txt", "data/shaders/a.nxr", "zzz/after.txt"});
  CloseHandle(lock);
  Check(r.outcome == Outcome::kInUse && r.path == "data/shaders/a.nxr", "lock: in use reported with its path");
  Check(Get(dst / "bf1943.exe") == "old exe", "lock: the swapped exe is put back");
  Check(Get(dst / "data/shaders/a.nxr") == "old shaders", "lock: the locked file is unchanged");
  Check(!fs::exists(dst / "newtop") && !fs::exists(dst / "zzz"), "lock: new folders removed again");
  bool leftovers = false;
  for (auto& e : fs::recursive_directory_iterator(dst)) {
    const auto ext = e.path().extension();
    leftovers |= ext == ".new" || ext == ".old" || ext == ".undo";
  }
  Check(!leftovers, "lock: no .new, .old or .undo files left");
  // And the retry after it was closed works.
  r = InstallFiles(src, dst, {"bf1943.exe", "newtop/readme.txt", "data/shaders/a.nxr", "zzz/after.txt"});
  Check(r.outcome == Outcome::kOk && Get(dst / "data/shaders/a.nxr") == "new shaders", "lock: retry installs");
#endif
}

void RetireTests(const fs::path& dir) {
  const fs::path root = dir / "retire";
  Put(root / "rexgpu-xenos.dll", "xenos 0.1.5");
  Put(root / "data/shaders/old/a.nxr", "old shaders");
  Put(root / "data/shaders/old/b.nxr", "old shaders b");
  Put(root / "data/shaders/bf1943-shaders.list", "still shipped list");
  Put(root / "licences/Old.txt", "changed by the player");
  Put(root / "licences/Kept.txt", "still shipped");
  Put(root / "game/old.bin", "game");
  Put(root / "reshade.ini", "player");
  Put(root / "fonts/Oswald.ttf", "font");
  Put(root / "olddir/a.txt", "a");
  Put(root / "olddir/player.txt", "player");
  const Manifest retired = MakeList({{"rexgpu-xenos.dll", "xenos 0.1.4"},
                                     {"rexgpu-xenos.dll", "xenos 0.1.5"},
                                     {"data/shaders/old/a.nxr", "old shaders"},
#if defined(_WIN32)  // Windows paths ignore case (Linux updates replace the AppImage and never retire files)
                                     {"Data/Shaders/Old/b.nxr", "old shaders b"},
#else
                                     {"data/shaders/old/b.nxr", "old shaders b"},
#endif
                                     {"licences/Old.txt", "original"},
                                     {"licences/Kept.txt", "still shipped"},
                                     {"game/old.bin", "game"},
                                     {"olddir/a.txt", "a"}});
  const Manifest current = MakeList({{"licences/Kept.txt", "still shipped"}});
  const auto removed = RetireFiles(root, retired, current, Protected());
  Check(Has(removed, "rexgpu-xenos.dll") && !fs::exists(root / "rexgpu-xenos.dll"), "retire: unchanged DLL removed");
  Check(!fs::exists(root / "data/shaders/old") && fs::exists(root / "data/shaders/bf1943-shaders.list"),
        "retire: the emptied folder removed, its parent with a file kept");
  Check(fs::exists(root / "fonts/Oswald.ttf"), "retire: other files stay");
  Check(fs::exists(root / "licences/Old.txt"), "retire: a changed file is kept");
  Check(fs::exists(root / "licences/Kept.txt"), "retire: a file this release ships is kept");
  Check(fs::exists(root / "game/old.bin"), "retire: protected kept");
  Check(fs::exists(root / "reshade.ini"), "retire: unlisted file kept");
  Check(!fs::exists(root / "olddir/a.txt") && fs::exists(root / "olddir/player.txt"),
        "retire: a folder with a player's file stays");
  Check(removed.size() == 4, "retire: exactly four removed");
}

void SourceTests(const fs::path& dir) {
  const fs::path src = dir / "dl", dst = dir / "inst";
  Put(src / "glyphs/xelu/a.png", "glyph");
  Put(src / "data/shaders/v.nxr", "re released");
  Put(dst / "bf1943.exe", "exe");
  const Manifest m = MakeList({{"bf1943.exe", "exe"}, {"glyphs/xelu/a.png", "glyph"}, {"data/shaders/v.nxr", "shaders"}});
  const auto needed = MissingOrWrong(dst, m, Protected(), kCheapHashLimit);
  Check(needed.size() == 2, "finish: two files needed");
  const auto r = InstallFromSource(src, dst, m, needed);
  Check(r.install.outcome == Outcome::kOk && r.installed.size() == 1 && Get(dst / "glyphs/xelu/a.png") == "glyph",
        "finish: the matching file installed");
  Check(r.unavailable.size() == 1 && !fs::exists(dst / "data/shaders/v.nxr"),
        "finish: a copy that does not match the list is not installed");
  Put(dst / "data/shaders/v.nxr", "shaders");
  Put(dst / "data/shaders/v.nxr.old", "left");
  Put(dst / "stray.txt", "x");
  RemoveOldCopies(dst, m);
  Check(!fs::exists(dst / "data/shaders/v.nxr.old"), "RemoveOldCopies");
  const auto extra = Unlisted(dst, m, Protected());
  Check(extra.size() == 1 && extra[0] == "stray.txt", "Unlisted finds the stray file only");
}

void FeedTests(const fs::path& data) {
  // The real answer of GitHub's releases/latest for v0.1.33-preview (a Windows zip, the server package, the sums).
  const std::string json = Get(data / "github-release-v0.1.33-preview.json");
  Check(!json.empty(), "feed: the saved GitHub answer is there");
  Release r;
  Check(ParseRelease(json, "windows-x64.zip", r), "feed: the Windows download found");
  Check(r.version == "0.1.33-preview", "feed: version from the tag without its v");
  Check(UrlFileName(r.url) == "BF1943-Reborn-0.1.33-preview-windows-x64.zip", "feed: the player zip, not the server's");
  Check(r.size == 59466476, "feed: its listed size");
  Check(UrlFileName(r.sums_url) == "SHA256SUMS.txt", "feed: SHA256SUMS.txt found");
  // That release has no AppImage: no update for Linux (and still a release: the version is known).
  Release l;
  Check(!ParseRelease(json, "linux-x86_64.AppImage", l) && l.version == "0.1.33-preview",
        "feed: no AppImage is no Linux download");
  // Only a name that ENDS with the platform's tail (King Kong's lesson).
  const std::string tricky =
      R"({"tag_name":"v0.2.0","assets":[{"name":"a","size":11,"browser_download_url":"https://x/BF-0.2.0-windows-x64.zip.json"},)"
      R"({"name":"b","size":22,"browser_download_url":"https://x/BF1943-Reborn-0.2.0-windows-x64.zip"},)"
      R"({"name":"c","size":33,"browser_download_url":"https://x/BF1943-Reborn-0.2.0-linux-x86_64.AppImage"},)"
      R"({"name":"d","size":44,"browser_download_url":"https://x/SHA256SUMS.txt"}]})";
  Check(ParseRelease(tricky, "windows-x64.zip", r) && r.size == 22 && UrlFileName(r.url) == "BF1943-Reborn-0.2.0-windows-x64.zip",
        "feed: a name only containing windows-x64.zip is not taken");
  Check(ParseRelease(tricky, "linux-x86_64.AppImage", r) && r.size == 33, "feed: the AppImage and its size");
  Check(!ParseRelease(R"({"message":"Not Found"})", "windows-x64.zip", r) && r.version.empty(),
        "feed: an error answer is no release");
}

void VersionTests() {
  Check(IsNewerVersion("0.2.0", "0.1.33-preview"), "0.2.0 > 0.1.33-preview");
  Check(!IsNewerVersion("0.1.33-preview", "0.2.0"), "0.1.33-preview < 0.2.0");
  Check(IsNewerVersion("0.2.1", "0.2.0") && IsNewerVersion("0.10.0", "0.9.9"), "numbers compare as numbers");
  Check(IsNewerVersion("0.2.0", "0.2.0-preview"), "a release beats its preview");
  Check(IsNewerVersion("v0.2.0-beta.3", "0.2.0-beta.2"), "betas count up");
  Check(!IsNewerVersion("0.2.0", "0.2.0") && !IsNewerVersion("0.2", "0.2.0"), "equal is not newer");
  Check(IsNewerVersion("9.9.9", "0.2.0"), "the developer zip's 9.9.9");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: update_files_tests <scratch folder> <bf/tests/data folder>\n");
    return 2;
  }
  const fs::path dir = fs::path(argv[1]);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  ParseTests();
  ProtectedTests();
  SumsTests();
  CheckTests(dir);
  InstallTests(dir);
  RetireTests(dir);
  SourceTests(dir);
  FeedTests(fs::path(argv[2]));
  VersionTests();
  if (failures) {
    std::printf("%d update checks FAILED\n", failures);
    return 1;
  }
  std::printf("update checks: all ok\n");
  return 0;
}
