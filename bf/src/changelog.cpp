// The changelog built into the program (changelog.h): bf/CHANGELOG.md read into kChangelogText by CMake, parsed here.
// Its form is the public changelog's: "## 0.3.0 (12 November 2026): what it is about", then paragraphs, a section's
// name on a line of its own in bold ("**New**"), and bullets ("- **Opening words.** The rest", continued on lines that
// begin with two spaces).

#include "changelog.h"

#include <sstream>

#include "update.h"

namespace bf1943 {

extern const char* const kChangelogText;  // made from bf/CHANGELOG.md by the build (CMakeLists.txt)

namespace {

// Markdown out of a run of text: [words](address) to the words, ** and ` dropped.
std::string Plain(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '[') {
      const size_t close = s.find(']', i);
      if (close != std::string::npos && close + 1 < s.size() && s[close + 1] == '(') {
        const size_t end = s.find(')', close);
        if (end != std::string::npos) {
          out += s.substr(i + 1, close - i - 1);
          i = end;
          continue;
        }
      }
    }
    if (c == '*' && i + 1 < s.size() && s[i + 1] == '*') {
      ++i;
      continue;
    }
    if (c == '`') continue;
    out += c;
  }
  return out;
}

std::string Trimmed(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
  size_t at = 0;
  while (at < s.size() && (s[at] == ' ' || s[at] == '\t')) ++at;
  return s.substr(at);
}

}  // namespace

const std::vector<ChangelogEntry>& Changelog() {
  static const std::vector<ChangelogEntry> entries = [] {
    std::vector<ChangelogEntry> out;
    std::stringstream in(kChangelogText);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind("## ", 0) == 0) {  // "## 0.3.0 (12 November 2026): mouse aiming in vehicles, ..."
        ChangelogEntry e;
        std::string rest = line.substr(3);
        if (!rest.empty() && (rest[0] == 'v' || rest[0] == 'V')) rest.erase(0, 1);
        const size_t open = rest.find(" (");
        const size_t close = open == std::string::npos ? std::string::npos : rest.find(')', open);
        e.version = Trimmed(rest.substr(0, open));
        if (close != std::string::npos) {
          e.date = rest.substr(open + 2, close - open - 2);
          std::string title = rest.substr(close + 1);
          if (!title.empty() && title[0] == ':') title.erase(0, 1);
          e.title = Trimmed(title);
        }
        out.push_back(std::move(e));
      } else if (!out.empty()) {
        out.back().body += line + "\n";
      }
    }
    for (auto& e : out) {  // no blank lines round a version's notes
      while (!e.body.empty() && e.body.front() == '\n') e.body.erase(0, 1);
      while (!e.body.empty() && e.body.back() == '\n') e.body.pop_back();
    }
    return out;
  }();
  return entries;
}

std::vector<ChangelogLine> ChangelogLines(const std::string& body) {
  std::vector<ChangelogLine> out;
  bool open = false;  // the last line of `out` still takes continuation lines
  std::stringstream in(body);
  for (std::string raw; std::getline(in, raw);) {
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    const std::string line = Trimmed(raw);
    if (line.empty()) {
      open = false;
      continue;
    }
    if (line.rfind("- ", 0) == 0) {
      ChangelogLine b;
      b.kind = ChangelogLine::kBullet;
      std::string rest = line.substr(2);
      if (rest.rfind("**", 0) == 0) {  // its bold opening words
        const size_t end = rest.find("**", 2);
        if (end != std::string::npos) {
          b.lead = Plain(rest.substr(2, end - 2));
          rest = Trimmed(rest.substr(end + 2));
        }
      }
      b.text = Plain(rest);
      out.push_back(std::move(b));
      open = true;
      continue;
    }
    // A line that is all bold, or a markdown heading, names a section.
    const bool bold_line = line.size() > 4 && line.rfind("**", 0) == 0 && line.compare(line.size() - 2, 2, "**") == 0 &&
                           line.find("**", 2) == line.size() - 2;
    if (bold_line || line.rfind("### ", 0) == 0) {
      ChangelogLine h;
      h.kind = ChangelogLine::kHead;
      h.text = Plain(bold_line ? line : line.substr(4));
      out.push_back(std::move(h));
      open = false;
      continue;
    }
    if (open && !out.empty()) {  // a bullet's or a paragraph's next line
      std::string& t = out.back().text;
      t += (t.empty() ? "" : " ") + Plain(line);
      continue;
    }
    ChangelogLine t;
    t.kind = ChangelogLine::kText;
    t.text = Plain(line);
    out.push_back(std::move(t));
    open = true;
  }
  return out;
}

std::vector<const ChangelogEntry*> ChangelogSince(const std::string& from, const std::string& current) {
  std::vector<const ChangelogEntry*> out;
  for (const ChangelogEntry& e : Changelog()) {
    if (update::IsNewerVersion(e.version, current)) continue;  // a later version's notes in an older program
    if (!from.empty() && !update::IsNewerVersion(e.version, from)) break;
    out.push_back(&e);
    if (from.empty()) break;  // the version before is not known: this version's notes only
  }
  return out;
}

}  // namespace bf1943
