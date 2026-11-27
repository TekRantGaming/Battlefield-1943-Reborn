// Report an issue (TekRantGaming, 16 Nov 2026: "it needs to have git hub integration for feedback with template to
// automatically submit data give user text boxes n stuff"): what the in game form sends, without its page (that is
// game_menu_report.inc). The details attached (each one the player can untick), the scrubbing of private data, the
// issue's title and Markdown body, the two routes to GitHub (the relay that files it for the player, and GitHub's own
// new issue page prefilled from our issue forms), the dry run, the reports kept to send later, the F12 screenshots.
// docs/REPORTS.md has the design. A GitHub token never ships in the game: the relay holds it.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace bf1943::report {

namespace fs = std::filesystem;

enum Type { kCrash, kBug, kPerformance, kServer, kSuggestion, kTypeCount };
const char* TypeName(int type);   // "Crash", "Bug", "Performance", "Server or connection", "Suggestion"
const char* TypeLabel(int type);  // the GitHub label: crash, bug, performance, server, suggestion
const char* TypeFile(int type);   // the issue form: crash.yml ...
inline constexpr const char* kOften[] = {"Every time", "Often", "Sometimes", "Once"};
inline constexpr int kOftenCount = 4;

// The limits of the fields (characters, as bytes of UTF-8).
inline constexpr size_t kTitleMax = 100, kWhatMax = 2000, kLongMax = 1000, kShortMax = 300, kContactMax = 60;
inline constexpr size_t kTitleMin = 8, kWhatMin = 20;

// What the player typed. Kept in user\reports\draft.json until sent or cleared.
struct Draft {
  int type = kBug;
  std::string title, what, steps, expected, actual, contact;
  int often = -1;  // kOften, -1 not said
};
bool LoadDraft(const fs::path& user_dir, Draft& d);
void SaveDraft(const fs::path& user_dir, const Draft& d);
void ClearDraft(const fs::path& user_dir);

// What the menu knows that the files do not: filled by the page when it gathers.
struct Context {
  fs::path user_dir;          // the user folder (bf1943.log, crashes, settings beside it)
  std::string display;        // "2560 x 1440 borderless, 165 Hz"
  std::vector<std::pair<std::string, std::string>> settings;  // the graphics settings, by the settings file's keys
  std::string player_name;    // the soldier name (the contact part may offer it)
  std::string account_code;   // the name's recovery code: never sent (scrubbed)
  int tick_rate = 0;
};

// One attached part: shown on the review page with its tick, its size and its text exactly as sent.
struct Part {
  std::string id;       // game, pc, display, match, crash, log, screenshot
  std::string name;     // "Crash report"
  std::string summary;  // one line for the review page
  std::string text;     // the Markdown it adds (already scrubbed)
  fs::path file;        // the screenshot's file; the whole log's file
  std::string data;     // the whole log: its gzip (sent as log_gz; the relay keeps it and links it from the issue)
  bool on = true;
  bool available = true;  // false: nothing to attach (no crash report)
};

// The parts for this type (system details are read once a run and kept). The screenshot's text is empty: it goes as a
// file (the relay) or is dragged in by hand (the browser).
std::vector<Part> Gather(const Context& c, int type);
// The system in one line for the log (diagnostics_health.cpp): "Windows 11 ... · CPU, cores, threads · RAM · GPU,
// driver (API)".
std::string SystemLine(const fs::path& user_dir);

// Private data out of a text: the Windows user and PC names, the user folder's paths, IP addresses, the recovery code,
// the install id, keys and tokens, e-mail addresses. ScrubSelfTest checks it (dev builds: BF_DEV_REPORT_SELFTEST=1).
struct Scrubber {
  std::vector<std::string> names;  // the user name, the PC name
  std::string code;                // the recovery code (20 characters, no separators)
  std::string install;             // the install id
};
Scrubber MakeScrubber(const Context& c);
std::string Scrub(const std::string& text, const Scrubber& s);
bool ScrubSelfTest(std::string& failures);
// The same rules for the log file as it is written (diagnostics.cpp AnonymizeLine): compiled once (MakeLogScrub
// throws when a rule cannot be built), each line's addresses handed to `ip_label` for their replacement ("[ip 3]";
// null: "[ip]").
struct LogScrubRules;
std::shared_ptr<const LogScrubRules> MakeLogScrub(const Scrubber& s);
std::string ScrubLogLine(const std::string& line, const LogScrubRules& rules,
                         const std::function<std::string(const std::string&)>& ip_label);

// The issue.
std::string Title(const Draft& d);                                   // "[Crash] Game closes when..."
std::string Body(const Draft& d, const std::vector<Part>& parts);    // the Markdown, as the review page shows it
std::string Signature(const std::vector<Part>& parts);               // a crash's signature (hex), "" without one
std::string Validate(const Draft& d);                                // "" when it can be sent, else what is missing

// The browser route: GitHub's new issue page for our issue form of this type, its fields prefilled (by their ids).
// Kept under kUrlMax bytes: what does not fit (the log, then the crash report) is left out and returned in `rest`
// (the page copies it to the clipboard for the player to paste into the Log field).
inline constexpr size_t kUrlMax = 8000;
inline constexpr const char* kRepo = "TekRantGaming/Battlefield-1943-Reborn";
std::string BrowserUrl(const Draft& d, const std::vector<Part>& parts, std::string& rest);
// The whole report as text, for Copy (Discord or anywhere).
std::string CopyText(const Draft& d, const std::vector<Part>& parts);

// The relay route: POST <bf_report_endpoint>/v1/bf/reports with this JSON (the screenshot's JPEG in base64).
std::string RequestJson(const Draft& d, const std::vector<Part>& parts);

// A send, on a thread of its own.
struct Send {
  enum State { kSending, kSent, kDuplicate, kKept, kRefused, kDryRun };
  std::atomic<int> state{kSending};
  std::mutex mutex;
  int status = 0;        // the HTTP status (0 no answer)
  int issue = 0;         // the issue's number (sent or duplicate)
  std::string url;       // the issue's address
  std::string message;   // why it was refused, or where the dry run wrote
  fs::path kept;         // the file it was kept in (kKept)
};
// The relay's address (bf_report_endpoint), "" when there is none (the browser route only).
std::string Endpoint();
bool DryRun();  // BF_REPORT_DRY_RUN=1: nothing leaves the PC (also bf_report_dry_run)
std::shared_ptr<Send> StartSend(const fs::path& user_dir, const Draft& d, const std::vector<Part>& parts);
// The browser route in a dry run: the URL written beside the request (returns the file).
fs::path DryRunUrl(const fs::path& user_dir, const std::string& url, const std::string& rest);

// Reports kept after a failed send (user\reports\pending-*.json): how many, and one more try of each (a thread; at
// most 5 tries a report, then it stays for Copy). Called at the start and every 10 minutes on the menu.
int PendingCount(const fs::path& user_dir);
void RetryPending(const fs::path& user_dir);

// F12 in a match: the game's window saved to user\screenshots\shot-<time>.jpg (1280 wide at most). Windows only.
bool TakeScreenshot(const fs::path& user_dir);
fs::path NewestScreenshot(const fs::path& user_dir, int max_age_hours);

// The last match joined (the server's name, official or not, the map, players then): written at each join, read by
// the Last match part. Never the address.
void NoteLastMatch(const fs::path& user_dir, const std::string& server, bool official, const std::string& map, int players,
                   int max_players, int ping_ms);

// At the start, before the log opens: bf1943.log kept as bf1943.previous.log (the log part sends the run that crashed).
void KeepPreviousLog(const fs::path& log);

}  // namespace bf1943::report
