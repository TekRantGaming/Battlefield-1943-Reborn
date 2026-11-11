// Updates, King Kong's way (docs/LAUNCHER-PLAN.md section 5; kk/src/update.cpp, task 370, and its audit). The latest
// release from the GitHub releases API (bf_update_feed: github.com/TekRantGaming/Battlefield-1943-Reborn; releases/latest
// never returns a prerelease, so releases are published as normal ones). TekRantGaming's rule (8 Nov 2026): always automatic,
// no way to turn it off.
//
// Windows: the release's asset whose name ENDS with windows-x64.zip, checked against the release's SHA256SUMS.txt line
// and its listed size (a release without SHA256SUMS.txt is refused), unpacked with Windows' own tar.exe. Its
// bf-files.txt (update_files.h) lists every file of the release with its hash: each listed file whose copy here
// differs is installed (no list of allowed folders), staged beside the old one (.new), the old one kept (.old) and every
// one put back if any swap fails; then the files bf-retired.txt lists are removed, each only while unchanged. Any other
// file is never touched, nor the player's folders (user, game, and the fixed protected list).
// Linux: the asset whose name ENDS with linux-x86_64.AppImage, checked the same way (and that it is a program), saved
// beside the running AppImage ($APPIMAGE) and renamed over it. A release without this platform's file is no update.
// Then the program starts again with the same command line.
//
// The first start of a version (Windows) compares the install with bf-files.txt: files missing or wrong (an unpacked
// download over an older folder, an antivirus) are fetched by downloading this version's release again, and the
// retired files are removed. A passed check is remembered in the user folder (update-check.txt).
//
// After an update (the version differs from the one the shaders were prepared for) the renderer's driver pipeline
// cache is deleted, so every stored pipeline is compiled again on the shader page (PrepareShadersAfterUpdate).
//
// Developer builds: BF_DEV_UPDATE_ZIP=<a release zip, or on Linux an AppImage, on this PC> is offered as version
// BF_DEV_UPDATE_VERSION (default 9.9.9) and installed from that file (checked against a SHA256SUMS.txt beside it when
// there is one); BF_DEV_UPDATE_SLOW=1 copies it slowly to see the progress. The program it starts is not offered it
// again. Any build: bf_update_feed can name a test feed on this PC (http://127.0.0.1:<port>/releases/latest).

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace bf1943::update {

enum class State : int {
  kIdle,         // not checked yet
  kChecking,
  kNone,         // no newer release (or no answer: `message` says which)
  kDownloading,  // bytes / total
  kChecked,      // downloaded and checked: ready to install
  kInstalling,
  kInstalled,    // swapped in: restart now
  kFailed,       // `message` says why (player facing)
};

struct Status {
  std::atomic<int> state{int(State::kIdle)};
  std::atomic<uint64_t> bytes{0}, total{0};
  // The first start check is downloading this version's files again (bytes / total).
  std::atomic<bool> finishing{false};
  std::mutex mutex;
  std::string version;  // the newer release's, once found
  std::string message;
};

Status& Get();
// Folders an update never writes or deletes in, besides the fixed list (update_files.h Protected): the player's user
// and game folders, wherever they are.
void SetProtectedFolders(std::vector<std::filesystem::path> folders);
// At the launcher's start, on a worker: the first start check of this version (Windows; remembered in `state_file`),
// then the check for a newer release (CheckInBackground's work).
void StartInBackground(const std::filesystem::path& state_file);
// Checks the feed on a worker (no newer: kNone); a newer release downloads and is checked by itself (kChecked).
void CheckInBackground();
// Installs the checked release on a worker (kInstalled, or kFailed with everything put back).
void InstallInBackground();
// Starts this program again with its own command line (after kInstalled); the caller quits.
bool RelaunchSelf();
// Starts this program again with its own command line (settings that apply at the start); the caller quits. Linux:
// the AppImage itself ($APPIMAGE), not the copy it runs from.
bool RestartSelf();
// At start: the .old files an update left (in use then) removed; on Linux a half written AppImage.
void CleanUpAfterUpdate();
// At start, before the renderer opens its storage: when the shaders were last prepared for another version, the
// driver's pipeline cache is deleted so they are all compiled again. True when that happened (the shader page then
// says the game was updated).
bool PrepareShadersAfterUpdate(const std::filesystem::path& cache_root, const std::string& prepared_for);
// True when version a ("0.2.1") is newer than b. A version with a suffix comes before the same numbers without one
// (0.2.0-preview < 0.2.0); suffixes with numbers count up (0.2.0-beta.2 < 0.2.0-beta.3).
bool IsNewerVersion(const std::string& a, const std::string& b);

}  // namespace bf1943::update
