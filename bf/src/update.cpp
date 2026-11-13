// Updates, King Kong's way (update.h): kk/src/update.cpp (task 370) and its lessons (codex/state/updater-audit.md).

#include "update.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <unistd.h>
extern char** environ;
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include "http.h"
#include "update_files.h"

// The releases' feed: the public release repository's releases/latest (0.1.x asked for TekRantGaming/BF1943-Reborn,
// which never existed, so they never found an update).
REXCVAR_DEFINE_STRING(bf_update_feed, "", "BF1943",
                      "GitHub releases/latest API address for updates and repair (empty disables both)");

namespace bf1943 {
const char* BfVersion();                // pipeline_upload.cpp
std::filesystem::path ExecutableDir();  // shader_prep.cpp
}  // namespace bf1943

namespace bf1943::update {
namespace {

// Player facing messages (the launcher's update page and its Home row).
constexpr const char* kCouldNotCheck = "Could not check for updates.";
constexpr const char* kUpToDate = "Up to date";
constexpr const char* kDownloadFailed = "The download failed. Check your internet connection; it tries again later.";
constexpr const char* kDamaged = "The download did not pass its check, so nothing was changed.";
#if defined(_WIN32)
constexpr const char* kUnpackFailed = "Could not unpack the update.";
constexpr const char* kNoProgram = "The update did not contain the game's program.";
constexpr const char* kListDamaged = "The update's file list is damaged, so nothing was changed.";
#endif

Status g_status;
std::atomic<bool> g_working{false};
std::mutex g_protect_mutex;
std::vector<fs::path> g_protect;
// The checked release, ready to install (kChecked): Windows, the unpacked folder holding bf1943.exe; Linux, the
// checked AppImage in the work folder. Set and read under g_status.mutex.
fs::path g_ready;

void SetState(State s, const std::string& message = {}) {
  if (!message.empty()) {
    std::lock_guard lock(g_status.mutex);
    g_status.message = message;
  }
  g_status.state = int(s);
}

#if defined(BF_DEV_TOOLS)
// Developer aid (King Kong's KK_DEV_UPDATE_ZIP): a release zip (Linux: an AppImage) on this PC offered as the newest
// version and installed from that file instead of a download.
const char* DevUpdateZip() {
  const char* v = std::getenv("BF_DEV_UPDATE_ZIP");
  return v && *v ? v : nullptr;
}
#endif

bool UpdatesEnabled() {
#if defined(BF_DEV_TOOLS)
  if (DevUpdateZip()) return true;
#endif
  return !REXCVAR_GET(bf_update_feed).empty();
}

// This platform's download: only a name that ENDS with it (King Kong's lesson: old updaters took any name containing
// it, so no other asset may contain it either; tools/make_packages.ps1 checks).
#if defined(_WIN32)
constexpr const char* kAssetTail = "windows-x64.zip";
#else
constexpr const char* kAssetTail = "linux-x86_64.AppImage";
#endif
// The release for this platform.
bool ParseOurs(const std::string& json, Release& r) { return ParseRelease(json, kAssetTail, r); }

std::string ReadText(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

fs::path WorkDir(const char* name) {
  std::error_code ec;
#if defined(_WIN32)
  return fs::temp_directory_path(ec) / name;
#else
  // Not /tmp (often memory): the cache, on the same disk as the player's games.
  const char* xdg = std::getenv("XDG_CACHE_HOME");
  const char* home = std::getenv("HOME");
  const fs::path cache = xdg && *xdg ? fs::path(xdg) : home && *home ? fs::path(home) / ".cache" : fs::temp_directory_path(ec);
  return cache / "bf1943-reborn" / name;
#endif
}

// The hash the release's SHA256SUMS.txt gives the download (the developer zip: a SHA256SUMS.txt beside it, if any).
// False with `why` set when the release cannot be checked: then nothing is installed (no size only fallback: every
// release of ours has SHA256SUMS.txt, so one without it is a mistake and is refused).
bool WantedHash(const Release& r, bool dev, std::string& want, std::string& why) {
  want.clear();
  if (dev) {
    const fs::path sums = fs::path(r.url).parent_path() / "SHA256SUMS.txt";
    std::error_code ec;
    if (!fs::exists(sums, ec)) return true;  // developer files: their size and type only
    want = SumFor(ReadText(sums), UrlFileName(r.url));
    if (want.empty()) {
      REXLOG_WARN("BF update: {} has no line for {}", Utf8(sums), UrlFileName(r.url));
      return why = kDamaged, false;
    }
    return true;
  }
  if (r.sums_url.empty()) {
    REXLOG_WARN("BF update: v{} has no SHA256SUMS.txt: not installed", r.version);
    return why = kDamaged, false;
  }
  std::vector<uint8_t> sums;
  if (!HttpGet(r.sums_url, sums, nullptr, nullptr, 20000)) return why = kDownloadFailed, false;
  want = SumFor(std::string(sums.begin(), sums.end()), UrlFileName(r.url));
  if (want.empty()) {
    REXLOG_WARN("BF update: SHA256SUMS.txt has no line for {}", UrlFileName(r.url));
    return why = kDamaged, false;
  }
  return true;
}

// Downloads (or reuses a checked copy from an earlier try) the release's file into `file` and checks it: its size,
// SHA256SUMS.txt's hash, and on Linux that it is a program. On failure `why` is a player facing message.
bool FetchFile(const Release& r, bool dev, const fs::path& file, std::string& why) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  std::string want;
  if (!WantedHash(r, dev, want, why)) return false;
  g_status.bytes = 0;
  g_status.total = r.size;
  if (!want.empty() && fs::is_regular_file(file, ec) && (r.size == 0 || fs::file_size(file, ec) == r.size) &&
      FileSha256(file) == want) {
    REXLOG_INFO("BF update: using the checked download from an earlier try ({})", Utf8(file));
    return true;
  }
  fs::remove(file, ec);
  std::vector<uint8_t> data;
  if (dev) {
#if defined(BF_DEV_TOOLS)
    // A local copy, in pieces so the screen shows progress.
    std::ifstream in(fs::path(r.url), std::ios::binary);
    if (!in) {
      REXLOG_WARN("BF_DEV_UPDATE_ZIP: could not read {}", r.url);
      return why = kDownloadFailed, false;
    }
    g_status.total = fs::file_size(fs::path(r.url), ec);
    std::vector<char> buf(4 << 20);
    while (in) {
      in.read(buf.data(), std::streamsize(buf.size()));
      if (in.gcount() > 0) data.insert(data.end(), buf.data(), buf.data() + in.gcount()), g_status.bytes += uint64_t(in.gcount());
      if (std::getenv("BF_DEV_UPDATE_SLOW")) std::this_thread::sleep_for(std::chrono::milliseconds(60));  // to see it
    }
#endif
  } else if (!HttpGet(r.url, data, &g_status.bytes, &g_status.total)) {
    return why = kDownloadFailed, false;
  }
  if (r.size && data.size() != r.size) {
    REXLOG_WARN("BF update: the download is {} bytes, the release lists {}", data.size(), r.size);
    return why = kDamaged, false;
  }
#if !defined(_WIN32)
  if (data.size() < (1u << 20) || std::memcmp(data.data(), "\x7f" "ELF", 4) != 0) {
    REXLOG_WARN("BF update: the download ({} bytes) is not a program", data.size());
    return why = kDamaged, false;
  }
#endif
  if (!want.empty()) {
    const std::string got = Sha256Of(data.data(), data.size());
    if (got != want) {
      REXLOG_WARN("BF update: sha256 {} is not SHA256SUMS.txt's {}", got, want);
      return why = kDamaged, false;
    }
  }
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()))) {
      REXLOG_WARN("BF update: could not save {}", Utf8(file));
      out.close();
      fs::remove(file, ec);
      return why = kDownloadFailed, false;
    }
  }
  REXLOG_INFO("BF update: v{} downloaded and checked ({} bytes{})", r.version, data.size(),
              want.empty() ? ", developer file: size only" : ", SHA256 matches SHA256SUMS.txt");
  return true;
}

#if defined(_WIN32)
bool RunAndWait(std::wstring command) {
  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    return false;
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return code == 0;
}

bool Untar(const fs::path& zip, const fs::path& into) {
  wchar_t system_dir[MAX_PATH];
  GetSystemDirectoryW(system_dir, MAX_PATH);  // Windows 10 and 11 include tar.exe
  return RunAndWait(L"\"" + std::wstring(system_dir) + L"\\tar.exe\" -xf \"" + zip.wstring() + L"\" -C \"" +
                    into.wstring() + L"\"");
}

// The release zip unpacked into work/files: `root` is the folder holding bf1943.exe nearest the top.
bool Unpack(const fs::path& work, fs::path& root, std::string& why) {
  std::error_code ec;
  fs::remove_all(work / "files", ec);
  fs::create_directories(work / "files", ec);
  if (!Untar(work / "update.zip", work / "files")) return why = kUnpackFailed, false;
  root.clear();
  size_t depth = SIZE_MAX;
  for (auto& e : fs::recursive_directory_iterator(work / "files", ec)) {
    if (e.path().filename() != "bf1943.exe") continue;
    const size_t d = size_t(std::distance(e.path().begin(), e.path().end()));
    if (d < depth) depth = d, root = e.path().parent_path();
  }
  if (root.empty()) return why = kNoProgram, false;
  return true;
}

// The protected paths (update_files.h) plus the player's folders when they are inside the program folder.
Protected MakeProtected(const fs::path& exe_dir) {
  std::vector<std::string> extra;
  std::lock_guard lock(g_protect_mutex);
  for (const auto& p : g_protect) {
    if (p.empty()) continue;
    std::error_code ec;
    const fs::path rel = fs::relative(fs::weakly_canonical(p, ec), fs::weakly_canonical(exe_dir, ec), ec);
    const std::string r = Utf8(rel);
    if (!ec && !rel.empty() && r != "." && r.rfind("..", 0) != 0) extra.push_back(r);
  }
  return Protected(std::move(extra));
}

Log LogTo(const char* what) {
  return [what](const std::string& line) { REXLOG_INFO("BF {}: {}", what, line); };
}

std::string InstallMessage(const InstallResult& r) {
  switch (r.outcome) {
    case Outcome::kOk: return {};
    case Outcome::kNoSource: return kDamaged;
    case Outcome::kWrite:
      return "Could not write " + r.path + (r.error.empty() ? "" : " (" + r.error + ")") +
             " into the game's folder, so nothing was changed. If your antivirus blocked it, allow the game's folder "
             "and try again.";
    case Outcome::kInUse:
      return r.path + " is in use by another program, so the update was undone and nothing was changed. Close it and "
                      "try again.";
  }
  return kDamaged;
}
#endif

void Check() {
  SetState(State::kChecking);
  Release r;
  bool dev = false;
#if defined(BF_DEV_TOOLS)
  if (const char* zip = DevUpdateZip()) {
    if (std::getenv("BF_DEV_UPDATE_RELAUNCHED")) {  // the program it just installed: no update (and no feed)
      REXLOG_INFO("BF dev: BF_DEV_UPDATE_ZIP installed; not offered again");
      return SetState(State::kNone, kUpToDate);
    }
    const char* v = std::getenv("BF_DEV_UPDATE_VERSION");
    r.version = v && *v ? v : "9.9.9";
    r.url = zip;
    dev = true;
    REXLOG_INFO("BF dev: BF_DEV_UPDATE_ZIP offers v{} from {}", r.version, zip);
  }
#endif
#if !defined(_WIN32)
  // The AppImage replaces itself; a program started some other way (a developer's build) has nothing to replace.
  const char* appimage = std::getenv("APPIMAGE");
  if (!appimage || !*appimage) {
    REXLOG_INFO("BF update: not started from an AppImage: updates are not looked for");
    return SetState(State::kNone, "Not an AppImage");
  }
#endif
  if (!dev) {
    const std::string feed = REXCVAR_GET(bf_update_feed);
    std::vector<uint8_t> data;
    if (feed.empty() || !HttpGet(feed, data, nullptr, nullptr, 20000)) {
      REXLOG_INFO("BF update: no answer from {}", feed);
      return SetState(State::kNone, kCouldNotCheck);
    }
    if (!ParseOurs(std::string(data.begin(), data.end()), r)) {
      if (r.version.empty()) {
        REXLOG_WARN("BF update: {} gave no release", feed);
        return SetState(State::kNone, kCouldNotCheck);
      }
      // King Kong's lesson: a release without this platform's file is no update, not a failed check.
      REXLOG_INFO("BF update: v{} has no {} download: no update", r.version, kAssetTail);
      return SetState(State::kNone, kUpToDate);
    }
    // The version this program was just updated to (RelaunchSelf) is never offered again: a test release can carry
    // a program of another number.
    const char* updated = std::getenv("BF_UPDATED_TO");
    if (!IsNewerVersion(r.version, BfVersion()) || (updated && r.version == updated)) {
      REXLOG_INFO("BF update: no update (latest v{}, this is v{})", r.version, BfVersion());
      return SetState(State::kNone, kUpToDate);
    }
  }
  {
    std::lock_guard lock(g_status.mutex);
    g_status.version = r.version;
  }
  REXLOG_INFO("BF update: v{} found (this is v{}): downloading {}", r.version, BfVersion(), r.url);
  SetState(State::kDownloading);
  std::string why;
#if defined(_WIN32)
  const fs::path work = WorkDir("bf1943_update");
  fs::path root;
  if (!FetchFile(r, dev, work / "update.zip", why) || !Unpack(work, root, why)) {
    std::error_code ec;
    fs::remove_all(work / "files", ec);  // a checked zip stays for the next try
    return SetState(State::kFailed, why);
  }
  {
    std::lock_guard lock(g_status.mutex);
    g_ready = root;
  }
#else
  const fs::path file = WorkDir("update") / UrlFileName(r.url);
  if (!FetchFile(r, dev, file, why)) return SetState(State::kFailed, why);
  {
    std::lock_guard lock(g_status.mutex);
    g_ready = file;
  }
#endif
  SetState(State::kChecked, "Update ready: version " + r.version);
}

void Install() {
  SetState(State::kInstalling);
  fs::path ready;
  std::string version;
  {
    std::lock_guard lock(g_status.mutex);
    ready = g_ready;
    version = g_status.version;
  }
  std::error_code ec;
#if defined(_WIN32)
  const fs::path exe_dir = ExecutableDir(), work = WorkDir("bf1943_update");
  const Protected keep = MakeProtected(exe_dir);
  const auto log = LogTo("update");
  InstallResult result;
  if (fs::exists(ready / kFilesList, ec)) {
    // Every file the release lists whose copy here differs (any folder: no list of allowed ones), its file list last.
    std::string error;
    const auto list = ReadManifest(ready / kFilesList, &error);
    if (!list) {
      REXLOG_WARN("BF update: {}: {}", kFilesList, error);
      fs::remove_all(work, ec);
      return SetState(State::kFailed, kListDamaged);
    }
    std::vector<std::string> plan = MissingOrWrong(exe_dir, *list, keep, UINT64_MAX);
    plan.push_back(kFilesList);
    REXLOG_INFO("BF update: v{} lists {} files; {} to install", list->version, list->files.size(), plan.size());
    result = InstallFiles(ready, exe_dir, plan, log);
    if (result.outcome == Outcome::kOk) {
      if (const auto retired = ReadManifest(ready / kRetiredList, &error)) {
        const auto removed = RetireFiles(exe_dir, *retired, *list, keep, log);
        REXLOG_INFO("BF update: {} retired files removed", removed.size());
      } else if (fs::exists(ready / kRetiredList, ec)) {
        REXLOG_WARN("BF update: {}: {} (nothing removed)", kRetiredList, error);
      }
    }
  } else {
    // A release without the list (none of ours from 0.2.0 on): every file but the protected ones, nothing removed.
    std::vector<std::string> plan;
    for (auto& e : fs::recursive_directory_iterator(ready, ec)) {
      if (!e.is_regular_file(ec)) continue;
      const std::string rel = Utf8(fs::relative(e.path(), ready, ec));
      if (SafePath(rel) && !keep(rel)) plan.push_back(rel);
    }
    REXLOG_INFO("BF update: v{} has no {}: its {} files", version, kFilesList, plan.size());
    result = InstallFiles(ready, exe_dir, plan, log);
  }
  if (result.outcome != Outcome::kOk) {
    fs::remove_all(work / "files", ec);  // the checked zip stays for a retry
    const std::string message = InstallMessage(result);
    REXLOG_WARN("BF update: {}", message);
    return SetState(State::kFailed, message);
  }
  fs::remove_all(work, ec);
#else
  // The AppImage is one file: the checked download copied beside it (same disk, so the rename is one step), made
  // runnable, and renamed over it (allowed while it runs: this program keeps the old one open until it quits).
  const char* appimage = std::getenv("APPIMAGE");
  if (!appimage || !*appimage) return SetState(State::kFailed, "Updates install themselves in the AppImage only.");
  const fs::path target = appimage;
  fs::path tmp = target;
  tmp += ".new";
  if (!fs::copy_file(ready, tmp, fs::copy_options::overwrite_existing, ec)) {
    REXLOG_WARN("BF update: could not write {}: {}", Utf8(tmp), ec.message());
    fs::remove(tmp, ec);
    return SetState(State::kFailed, "Could not save the update beside the AppImage, so nothing was changed. Is its "
                                    "folder read only?");
  }
  fs::permissions(tmp, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                           fs::perms::others_exec, ec);
  fs::rename(tmp, target, ec);
  if (ec) {
    REXLOG_WARN("BF update: could not rename {} over {}: {}", Utf8(tmp), Utf8(target), ec.message());
    std::error_code e;
    fs::remove(tmp, e);
    return SetState(State::kFailed, "Could not replace the AppImage with the new version, so nothing was changed.");
  }
  fs::remove(ready, ec);
#endif
  REXLOG_INFO("BF update: v{} installed; restarting", version);
  SetState(State::kInstalled, "Updated to version " + version + ". Restarting...");
}

// The first start check (update.h; King Kong's FinishUpdate, task 370 E). Windows only: the AppImage is always whole.
void Finish(const fs::path& state_file) {
#if defined(_WIN32)
  const fs::path exe_dir = ExecutableDir();
  std::error_code ec;
  const std::string text = ReadText(exe_dir / kFilesList);
  if (text.empty()) {
    REXLOG_INFO("BF files check: no {} beside the program (not a packaged release): skipped", kFilesList);
    return;
  }
  // Remembered: this list, in this folder, passed before.
  const std::string passed = Sha256Of(text.data(), text.size()) + "\n" + Utf8(exe_dir) + "\n";
  if (ReadText(state_file) == passed) return;
  auto remember = [&] {
    fs::create_directories(state_file.parent_path(), ec);
    std::ofstream(state_file, std::ios::binary | std::ios::trunc) << passed;
  };
  std::string error;
  const auto list = ParseManifest(text, &error);
  if (!list) {
    REXLOG_WARN("BF files check: {}: {}", kFilesList, error);
    return;
  }
  if (list->version != BfVersion()) {
    REXLOG_WARN("BF files check: {} is for v{}, this program is v{}: skipped", kFilesList, list->version, BfVersion());
    return;
  }
  const Protected keep = MakeProtected(exe_dir);
  const auto log = LogTo("files check");
  if (const auto retired = ReadManifest(exe_dir / kRetiredList, &error)) {
    const auto removed = RetireFiles(exe_dir, *retired, *list, keep, log);
    if (!removed.empty()) REXLOG_INFO("BF files check: {} retired files removed", removed.size());
  }
  const auto needed = MissingOrWrong(exe_dir, *list, keep, kCheapHashLimit);
  if (needed.empty()) {
    REXLOG_INFO("BF files check: all {} files of v{} are in place", list->files.size(), BfVersion());
    remember();
    return;
  }
  std::string names;
  for (size_t i = 0; i < needed.size() && i < 8; ++i) names += (i ? ", " : "") + needed[i];
  REXLOG_INFO("BF files check: {} files missing or wrong ({}{}): downloading v{} again", needed.size(), names,
              needed.size() > 8 ? ", ..." : "", BfVersion());
  {  // A folder it cannot write to would mean downloading the release at every start for nothing.
    const fs::path probe = exe_dir / "bf-write-test.tmp";
    const bool writable = static_cast<bool>(std::ofstream(probe, std::ios::binary | std::ios::trunc));
    fs::remove(probe, ec);
    if (!writable) {
      REXLOG_WARN("BF files check: the program folder is read only: nothing downloaded");
      return;
    }
  }
  Release r;
  bool dev = false;
#if defined(BF_DEV_TOOLS)
  if (const char* zip = DevUpdateZip()) r.version = BfVersion(), r.url = zip, dev = true;
#endif
  if (!dev) {
    // This version's own release, by its tag (the feed's address with releases/tags/v<version> for releases/latest).
    std::string api = REXCVAR_GET(bf_update_feed);
    api = api.substr(0, api.rfind('/') + 1);
    if (const size_t latest = api.rfind("/latest/"); latest != std::string::npos) api.erase(latest + 1);
    api += std::string("tags/v") + BfVersion();
    std::vector<uint8_t> data;
    if (!HttpGet(api, data, nullptr, nullptr, 20000) || !ParseOurs(std::string(data.begin(), data.end()), r) ||
        r.version != BfVersion()) {
      REXLOG_WARN("BF files check: could not find the v{} release ({}); trying again next start", BfVersion(), api);
      return;
    }
  }
  g_status.finishing = true;
  const fs::path work = WorkDir("bf1943_finish");
  fs::path root;
  std::string why;
  if (!FetchFile(r, dev, work / "update.zip", why) || !Unpack(work, root, why)) {
    g_status.finishing = false;
    fs::remove_all(work / "files", ec);
    REXLOG_WARN("BF files check: {} Trying again next start.", why);
    return;
  }
  const auto result = InstallFromSource(root, exe_dir, *list, needed, log);
  g_status.finishing = false;
  if (result.install.outcome != Outcome::kOk) {
    fs::remove_all(work / "files", ec);
    REXLOG_WARN("BF files check: could not install the files ({}); trying again next start", result.install.path);
    return;
  }
  fs::remove_all(work, ec);
  REXLOG_INFO("BF files check: {} files installed{}", result.installed.size(),
              result.unavailable.empty() ? "" : ", " + std::to_string(result.unavailable.size()) + " not in the release");
  // Files the release itself lacks would only be downloaded again at every start: remembered as checked.
  remember();
#else
  (void)state_file;
#endif
}

#if !defined(_WIN32)
// This program again (the AppImage itself when it runs from one), with the same arguments.
bool SpawnSelf() {
  std::vector<std::string> args;
  {
    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    for (std::string arg; std::getline(cmdline, arg, '\0');) args.push_back(arg);
  }
  const char* appimage = std::getenv("APPIMAGE");
  std::error_code ec;
  const std::string exe = appimage && *appimage ? std::string(appimage) : fs::read_symlink("/proc/self/exe", ec).string();
  if (exe.empty()) return false;
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(exe.c_str()));
  for (size_t i = 1; i < args.size(); ++i) argv.push_back(args[i].data());
  argv.push_back(nullptr);
  // An AppImage sets its own library path again (AppRun); the old one names the copy this program runs from.
  std::vector<char*> env;
  for (char** e = environ; *e; ++e)
    if (!(appimage && std::strncmp(*e, "LD_LIBRARY_PATH=", 16) == 0)) env.push_back(*e);
  env.push_back(nullptr);
  pid_t pid = 0;
  if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), env.data()) != 0) {
    REXLOG_WARN("BF restart: could not start {} again", exe);
    return false;
  }
  REXLOG_INFO("BF restart: started again (PID {}, {})", int(pid), exe);
  return true;
}
#endif

}  // namespace

Status& Get() { return g_status; }

void SetProtectedFolders(std::vector<fs::path> folders) {
  std::lock_guard lock(g_protect_mutex);
  g_protect = std::move(folders);
}

void StartInBackground(const fs::path& state_file) {
  if (!UpdatesEnabled()) {
    REXLOG_INFO("BF update: no feed configured; automatic updates and file repair disabled");
    return SetState(State::kDisabled);
  }
  if (g_working.exchange(true)) return;
  std::thread([state_file] {
    Finish(state_file);
    Check();
    g_working = false;
  }).detach();
}

void CheckInBackground() {
  if (!UpdatesEnabled()) return SetState(State::kDisabled);
  if (g_working.exchange(true)) return;
  const int s = g_status.state;
  if (s == int(State::kChecked) || s == int(State::kInstalling) || s == int(State::kInstalled)) {
    g_working = false;
    return;
  }
  std::thread([] {
    Check();
    g_working = false;
  }).detach();
}

void InstallInBackground() {
  if (!UpdatesEnabled()) return SetState(State::kDisabled);
  if (g_status.state != int(State::kChecked) || g_working.exchange(true)) return;
  std::thread([] {
    Install();
    g_working = false;
  }).detach();
}

bool RelaunchSelf() {
  std::string version;
  {
    std::lock_guard lock(g_status.mutex);
    version = g_status.version;
  }
  // The new program never offers this release again (nor, in developer builds, the developer zip).
#if defined(_WIN32)
  SetEnvironmentVariableA("BF_UPDATED_TO", version.c_str());
#if defined(BF_DEV_TOOLS)
  if (DevUpdateZip()) SetEnvironmentVariableA("BF_DEV_UPDATE_RELAUNCHED", "1");
#endif
#else
  setenv("BF_UPDATED_TO", version.c_str(), 1);
#if defined(BF_DEV_TOOLS)
  if (DevUpdateZip()) setenv("BF_DEV_UPDATE_RELAUNCHED", "1", 1);
#endif
#endif
  return RestartSelf();
}

bool RestartSelf() {
#if defined(_WIN32)
  // The program file by its path (the command line's first word can be relative), the same command line and folder.
  wchar_t exe[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring command = GetCommandLineW();
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(n && n < MAX_PATH ? exe : nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                      &si, &pi)) {
    REXLOG_WARN("BF restart: could not start the program again ({})", GetLastError());
    return false;
  }
  REXLOG_INFO("BF restart: started again (PID {})", pi.dwProcessId);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  return SpawnSelf();
#endif
}

void CleanUpAfterUpdate() {
  if (!UpdatesEnabled()) return;
#if defined(_WIN32)
  // On a worker, tried for 20 s: right after an update the program that installed it may still be closing, and its
  // old files are in use until it has. Only the .old copies beside the files the lists name (King Kong's way: a file
  // of the player's is never touched).
  std::thread([] {
    const fs::path exe_dir = ExecutableDir();
    std::vector<Manifest> lists;
    for (const char* name : {kFilesList, kRetiredList})
      if (auto m = ReadManifest(exe_dir / name)) lists.push_back(std::move(*m));
    Manifest self;
    self.files.push_back({"bf1943.exe", 0, ""});  // the program's own, list or not
    lists.push_back(self);
    for (int attempt = 0; attempt < 20; ++attempt) {
      int left = 0;
      for (const auto& m : lists) {
        RemoveOldCopies(exe_dir, m);
        std::error_code ec;
        for (const auto& f : m.files) left += fs::exists(fs::path(PathOf(exe_dir, f.path)) += ".old", ec);
      }
      if (!left) return;
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    REXLOG_WARN("BF update: old files from the last update are still in use; removed at a later start");
  }).detach();
#else
  // A copy an update could not rename over the AppImage.
  if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage) {
    std::error_code ec;
    fs::path tmp = appimage;
    tmp += ".new";
    if (fs::remove(tmp, ec)) REXLOG_INFO("BF update: a half installed update removed ({})", Utf8(tmp));
  }
#endif
}

bool PrepareShadersAfterUpdate(const fs::path& cache_root, const std::string& prepared_for) {
  (void)cache_root;
  if (prepared_for.empty() || prepared_for == BfVersion()) return false;
  // King Kong's rule exactly (TekRantGaming, 11 Nov 2026: "Copy Kong exactly"; its shader_prepared.h, launcher.cpp
  // JobAtLauncherStart and shader_pack_service.cpp): after an update everything is made again in full, once, behind
  // the shader page, and nothing is deleted. The preparation adds this version's list to the renderer's storage from
  // the player's game files; the pipelines this PC learned in play stay; the driver's pipeline cache stays too (Vulkan
  // ignores one made by another driver or device), so the page makes every stored pipeline again from it. A page left
  // before its end leaves the old marker, so the next start is an update again and finishes the job.
  REXLOG_INFO("BF update: shaders were prepared for v{}, this is v{}: the shader page makes every pipeline again, over "
              "the cache (nothing deleted, as King Kong does)",
              prepared_for, BfVersion());
  return true;
}

}  // namespace bf1943::update
