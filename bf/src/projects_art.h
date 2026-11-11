// The pictures on More from TRG Studios (TekRantGaming, 11 Nov 2026: got as King Kong gets them, kk/src/workshop.cpp
// LoadProject with workshop_logic.h's helpers, MIT, TekRantGaming): a project's SteamGridDB artwork where King Kong has
// one (direct links: the SteamGridDB API needs a key that cannot ship), else the README's first picture, else the
// repository's social preview, else GitHub's card. Fetched at run time on a worker into the user cache (projects/),
// asked for again at most once a day (the SteamGridDB artwork never: its link never changes); nothing of the projects
// is shipped or committed. The README comes from raw.githubusercontent.com (King Kong's fallback when the GitHub API is
// rate limited), so no GitHub API request is made at all.
#pragma once

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bf1943::projects {

struct Repo {
  std::string owner, repo;
};

// Starts the worker once per run (later calls do nothing).
void StartArt(const std::filesystem::path& cache_dir, std::vector<Repo> repos);
// The picture's file for a repository in the cache (drawn once it is there).
std::filesystem::path ArtFile(const std::filesystem::path& cache_dir, const std::string& repo);

// ---- King Kong's workshop_logic.h (the parts for the tile's picture), on a Repo.

// King Kong's kArtwork (task 334, the coordinator's picks) and King Kong itself (its own Steam icon), for the featured
// tile here.
struct Artwork {
  const char* repo;
  const char* url;
};
inline constexpr Artwork kArtwork[] = {
    {"king-kong-recompiled", "https://cdn2.steamgriddb.com/icon/9638d91ecb50d03683088611e5e65d9f.png"},
    {"point-blank-recompiled", "https://cdn2.steamgriddb.com/grid/24f5d2bc2e14744b5bfd41a47c9d25f3.png"},
    {"simpsons-wrestling-recompiled", "https://cdn2.steamgriddb.com/grid/63d36bef6780ab6f8195f0803b728ac4.jpg"},
    {"earthworm-jim-hd-recompiled", "https://cdn2.steamgriddb.com/grid/48019033f9941955c6348a063eeb09c9.png"},
    {"sms-launcher", "https://cdn2.steamgriddb.com/grid/e05d8eb9513ddafd8f410d874077d1b8.png"},
    {"digdug-native", "https://cdn2.steamgriddb.com/grid/455a8d4a5cbf27ca5c8e480ea66c6a8f.png"},
    {"south-park-tenormans-revenge-recompiled", "https://cdn2.steamgriddb.com/grid/a22997b93b15692ee45f7a9fb6778c8d.jpg"},
    {"south-park-tower-defense-recompiled", "https://cdn2.steamgriddb.com/grid/e65e26a9a28ef7478d953cbec82be1b0.png"},
};
inline std::string ArtworkUrl(const Repo& p) {
  for (const Artwork& a : kArtwork)
    if (std::string_view(a.repo) == p.repo) return a.url;
  return {};
}

// A day (King Kong's kRefreshSeconds).
inline constexpr long long kRefreshSeconds = 24 * 60 * 60;

inline std::string RepoUrl(const Repo& p) { return "https://github.com/" + p.owner + "/" + p.repo; }
inline std::string RawBase(const Repo& p, const std::string& branch) {
  return "https://raw.githubusercontent.com/" + p.owner + "/" + p.repo + "/" + (branch.empty() ? "HEAD" : branch) + "/";
}
inline std::string RawReadmeUrl(const Repo& p, const std::string& branch) { return RawBase(p, branch) + "README.md"; }
inline std::string SocialPreviewUrl(const Repo& p) { return "https://opengraph.githubassets.com/1/" + p.owner + "/" + p.repo; }

inline bool StartsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }
inline std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
inline std::string UrlHost(std::string_view url) {
  size_t at = 0;
  if (StartsWith(url, "https://")) at = 8;
  else if (StartsWith(url, "http://")) at = 7;
  else return {};
  const size_t end = url.find_first_of("/?#:", at);
  return Lower(std::string(url.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at)));
}
// Pictures are only fetched from GitHub's own hosts (a README could point anywhere).
inline bool IsAllowedImageHost(std::string_view url) {
  if (!StartsWith(url, "https://")) return false;
  const std::string host = UrlHost(url);
  static constexpr const char* kHosts[] = {
      "raw.githubusercontent.com",          "github.com",
      "user-images.githubusercontent.com",  "private-user-images.githubusercontent.com",
      "camo.githubusercontent.com",         "opengraph.githubassets.com",
      "repository-images.githubusercontent.com", "avatars.githubusercontent.com",
      "objects.githubusercontent.com"};
  for (const char* h : kHosts)
    if (host == h) return true;
  return false;
}
// Badges and SVG pictures: not photos, and not decodable.
inline bool IsBadgeOrVector(std::string_view url) {
  const std::string u = Lower(std::string(url.substr(0, url.find_first_of("?#"))));
  const std::string host = UrlHost(u);
  if (host == "img.shields.io" || host == "badgen.net" || host == "badge.fury.io" ||
      host == "invidget.switchblade.xyz" || host == "discordapp.com" || host == "discord.com")
    return true;
  if (u.size() >= 4 && u.compare(u.size() - 4, 4, ".svg") == 0) return true;
  return u.find("/badge.svg") != std::string::npos || u.find("/badges/") != std::string::npos;
}
// A README's address made absolute (relative paths are the repository's own files).
inline std::string ResolveUrl(const Repo& p, const std::string& branch, std::string url, bool image) {
  while (!url.empty() && std::isspace(static_cast<unsigned char>(url.front()))) url.erase(url.begin());
  while (!url.empty() && std::isspace(static_cast<unsigned char>(url.back()))) url.pop_back();
  if (url.size() >= 2 && url.front() == '<' && url.back() == '>') url = url.substr(1, url.size() - 2);
  if (url.empty() || url[0] == '#') return {};
  if (StartsWith(url, "//")) url = "https:" + url;
  if (StartsWith(url, "http://")) url = "https://" + url.substr(7);
  if (StartsWith(url, "https://")) {
    if (image && StartsWith(url, "https://github.com/") && url.find("/blob/") != std::string::npos) {
      const size_t at = url.find("/blob/");
      url = url.substr(0, at) + "/raw/" + url.substr(at + 6);
    }
    return url;
  }
  const size_t colon = url.find(':'), slash = url.find('/');
  if (colon != std::string::npos && (slash == std::string::npos || colon < slash)) return {};
  while (StartsWith(url, "./")) url = url.substr(2);
  while (StartsWith(url, "../")) url = url.substr(3);
  while (!url.empty() && url[0] == '/') url.erase(url.begin());
  if (url.empty()) return {};
  if (image) return RawBase(p, branch) + url;
  return RepoUrl(p) + "/blob/" + (branch.empty() ? std::string("HEAD") : branch) + "/" + url;
}
inline std::string HtmlAttr(std::string_view tag, std::string_view name) {
  const std::string lower = Lower(std::string(tag));
  for (size_t at = 0; (at = lower.find(name, at)) != std::string::npos; ++at) {
    if (at > 0 && !std::isspace(static_cast<unsigned char>(lower[at - 1]))) continue;
    size_t i = at + name.size();
    while (i < lower.size() && std::isspace(static_cast<unsigned char>(lower[i]))) ++i;
    if (i >= lower.size() || lower[i] != '=') continue;
    ++i;
    while (i < lower.size() && std::isspace(static_cast<unsigned char>(lower[i]))) ++i;
    if (i >= lower.size()) return {};
    if (lower[i] == '"' || lower[i] == '\'') {
      const char q = lower[i];
      const size_t end = lower.find(q, i + 1);
      return std::string(tag.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1));
    }
    size_t end = i;
    while (end < lower.size() && !std::isspace(static_cast<unsigned char>(lower[end])) && lower[end] != '>' &&
           lower[end] != '/')
      ++end;
    return std::string(tag.substr(i, end - i));
  }
  return {};
}
inline int HtmlPixels(const std::string& value) {
  if (value.empty() || value.find('%') != std::string::npos) return 0;
  return std::atoi(value.c_str());
}
// The og:image of a repository's page (its social preview: the owner's own picture or GitHub's card).
inline std::string OgImage(const std::string& html) {
  for (size_t at = 0; (at = html.find("<meta", at)) != std::string::npos; at += 5) {
    const size_t end = html.find('>', at);
    if (end == std::string::npos) break;
    const std::string_view tag(html.data() + at, end - at);
    if (HtmlAttr(tag, "property") == "og:image" || HtmlAttr(tag, "name") == "og:image") return HtmlAttr(tag, "content");
  }
  return {};
}
// The README's first picture that is a photo from GitHub (not a badge, an SVG, a tiny icon or a picture inside a code
// block), made absolute; "" when it has none.
inline std::string FirstReadmeImage(const Repo& p, const std::string& branch, const std::string& md) {
  bool fence = false;
  size_t line_start = 0;
  auto usable = [&](const std::string& url) { return !url.empty() && !IsBadgeOrVector(url) && IsAllowedImageHost(url); };
  while (line_start <= md.size()) {
    size_t line_end = md.find('\n', line_start);
    if (line_end == std::string::npos) line_end = md.size();
    const std::string line = md.substr(line_start, line_end - line_start);
    const size_t first = line.find_first_not_of(" \t");
    if (first != std::string::npos && (line.compare(first, 3, "```") == 0 || line.compare(first, 3, "~~~") == 0)) {
      fence = !fence;
    } else if (!fence) {
      for (size_t i = 0; i < line.size(); ++i) {
        if (line.compare(i, 2, "![") == 0) {
          const size_t close = line.find("](", i + 2);
          if (close == std::string::npos) continue;
          const size_t end = line.find(')', close + 2);
          if (end == std::string::npos) continue;
          std::string target = line.substr(close + 2, end - close - 2);
          if (const size_t sp = target.find(' '); sp != std::string::npos) target = target.substr(0, sp);
          const std::string url = ResolveUrl(p, branch, target, true);
          if (usable(url)) return url;
          i = end;
        } else if (line[i] == '<' && Lower(line.substr(i, 4)) == "<img") {
          const size_t end = line.find('>', i);
          const std::string_view tag(line.data() + i, (end == std::string::npos ? line.size() : end) - i);
          const int w = HtmlPixels(HtmlAttr(tag, "width")), h = HtmlPixels(HtmlAttr(tag, "height"));
          const std::string url = ResolveUrl(p, branch, HtmlAttr(tag, "src"), true);
          if ((w == 0 || w > 64) && (h == 0 || h > 64) && usable(url)) return url;
          if (end == std::string::npos) break;
          i = end;
        }
      }
    }
    line_start = line_end + 1;
  }
  return {};
}

}  // namespace bf1943::projects
