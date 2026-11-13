// The game's changelog (bf/CHANGELOG.md, built into the program by CMake as King Kong builds its own:
// kk/src/changelog.h), for the launcher's What's new pop-up after an update (TekRantGaming, 13 Nov 2026: "patch notes
// popup like kong has when game updates make sure it only shows after the updates shaders have finished exact same as
// kong") and the list of every version behind it.

#pragma once

#include <string>
#include <vector>

namespace bf1943 {

struct ChangelogEntry {
  std::string version;  // "0.3.0", "0.1.33 preview"
  std::string date;     // "12 November 2026"
  std::string title;    // "mouse aiming in vehicles, graphics presets, ..." (may be empty)
  std::string body;     // the version's notes as written (markdown: **New**, - bullets, wrapped lines)
};

// Every version, newest first.
const std::vector<ChangelogEntry>& Changelog();

// A version's notes as a reader's lines, the markdown taken out: a section's heading ("New", "Fixed"), a bullet
// (`lead` its bold opening words, if it has any), or a paragraph.
struct ChangelogLine {
  enum Kind { kHead, kBullet, kText } kind = kText;
  std::string lead;
  std::string text;
};
std::vector<ChangelogLine> ChangelogLines(const std::string& body);

// The entries to show after an update: every version newer than `from` (the version run before; empty when not
// known: then only the newest one that is not newer than `current`) and not newer than `current`, newest first.
std::vector<const ChangelogEntry*> ChangelogSince(const std::string& from, const std::string& current);

}  // namespace bf1943
