// The pictures on More from TRG Studios: see projects_art.h.

#include "projects_art.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

#include <rex/logging.h>

#include "http.h"
#include "picture_decode.h"

namespace bf1943::projects {
namespace fs = std::filesystem;

namespace {

long long Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string AmpDecoded(std::string s) {  // og:image's &amp; (King Kong's md::DecodeEntities, the one that matters here)
  for (size_t at = 0; (at = s.find("&amp;", at)) != std::string::npos; ++at) s.replace(at, 5, "&");
  return s;
}

// What the cache knows of a picture: its address and when it was fetched (<repo>.src: two lines).
struct Meta {
  std::string url;
  long long at = 0;
};
Meta ReadMeta(const fs::path& file) {
  Meta m;
  std::ifstream in(file);
  std::getline(in, m.url);
  std::string at;
  std::getline(in, at);
  m.at = std::atoll(at.c_str());
  return m;
}

bool Get(const std::string& url, std::vector<uint8_t>& out) {
  out.clear();
  return HttpGet(url, out, nullptr, nullptr, 15000) && !out.empty() && out.size() <= (12u << 20);  // King Kong's limit
}

// A picture the menu can draw (decoded once to check: an error page is never kept).
bool Decodes(const std::vector<uint8_t>& bytes, int& w, int& h) {
  w = h = 0;
  return !bytes.empty() && !DecodePicture(bytes.data(), bytes.size(), w, h).empty() && w > 0 && h > 0;
}

bool Save(const fs::path& file, const fs::path& meta, const std::vector<uint8_t>& bytes, const std::string& url) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  const fs::path tmp = fs::path(file).concat(".part");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!out) return false;
  }
  fs::rename(tmp, file, ec);  // whole, never half written under the menu's reader
  if (ec) return false;
  std::ofstream(meta, std::ios::trunc) << url << "\n" << Now() << "\n";
  return true;
}

// True when the project has a picture (fetched now, or still fresh in the cache).
bool Fetch(const fs::path& dir, const Repo& p) {
  const fs::path file = dir / (p.repo + ".img"), meta_file = dir / (p.repo + ".src");
  const std::string art = ArtworkUrl(p);
  std::error_code ec;
  if (fs::exists(file, ec)) {
    const Meta m = ReadMeta(meta_file);
    if (!art.empty() && m.url == art) return true;                          // the artwork: any age
    if (art.empty() && Now() - m.at >= 0 && Now() - m.at < kRefreshSeconds) return true;  // fresh
  }
  const auto began = std::chrono::steady_clock::now();
  std::vector<uint8_t> bytes;
  std::string url, source;
  int w = 0, h = 0;
  // King Kong's order (workshop.cpp LoadProject): the SteamGridDB artwork, else the README's first picture, else the
  // repository's social preview, else GitHub's card.
  if (!art.empty() && Get(art, bytes) && Decodes(bytes, w, h)) {
    url = art, source = "SteamGridDB";
  } else {
    if (Get(RawReadmeUrl(p, ""), bytes)) url = FirstReadmeImage(p, "", std::string(bytes.begin(), bytes.end())), source = "README";
    if (url.empty() && Get(RepoUrl(p), bytes)) {
      const std::string og = ResolveUrl(p, "", AmpDecoded(OgImage(std::string(bytes.begin(), bytes.end()))), true);
      if (IsAllowedImageHost(og) && !IsBadgeOrVector(og)) url = og, source = "social preview";
    }
    if (url.empty()) url = SocialPreviewUrl(p), source = "GitHub card";
    if (!(Get(url, bytes) && Decodes(bytes, w, h)) && source != "GitHub card") {
      url = SocialPreviewUrl(p), source = "GitHub card";
      if (!(Get(url, bytes) && Decodes(bytes, w, h))) bytes.clear();
    }
  }
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
  if (w <= 0 || !Save(file, meta_file, bytes, url)) {
    REXLOG_WARN("BF projects: {}/{}: no picture yet ({} ms)", p.owner, p.repo, ms);
    return false;
  }
  REXLOG_INFO("BF projects: {}/{}: picture ({}) {} {}x{}, {} ms", p.owner, p.repo, source, url, w, h, ms);
  return true;
}

std::atomic<bool> g_started{false};

}  // namespace

fs::path ArtFile(const fs::path& cache_dir, const std::string& repo) { return cache_dir / "projects" / (repo + ".img"); }

void StartArt(const fs::path& cache_dir, std::vector<Repo> repos) {
  if (g_started.exchange(true)) return;
  std::thread([dir = cache_dir / "projects", repos = std::move(repos)] {
    // A moment between projects, and those left without a picture once more after a few seconds: GitHub's card
    // service answers a quick run of requests with 429 (seen on 11 Nov 2026). Any still without one are asked for
    // again at the next start (nothing of theirs is in the cache).
    std::vector<Repo> missing;
    for (const Repo& p : repos) {
      if (!Fetch(dir, p)) missing.push_back(p);
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (missing.empty()) return;
    std::this_thread::sleep_for(std::chrono::seconds(5));
    for (const Repo& p : missing) {
      Fetch(dir, p);
      std::this_thread::sleep_for(std::chrono::milliseconds(750));
    }
  }).detach();
}

}  // namespace bf1943::projects
