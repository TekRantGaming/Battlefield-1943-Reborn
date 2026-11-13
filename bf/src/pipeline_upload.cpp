// Every level's pipelines for the next update's shader list (TekRantGaming, 7 Nov 2026: "make sure we are capturing pipelines
// and shaders to pre compile from all levels ... log them to the cloudflair server"). A player's game keeps the
// pipelines it builds in NativeXRenderer's storage (<cache>\native_x_renderer\5841097E.pipelines); this sends that
// storage, without the game's shader code (its 'S' records: the same file as our shipped list, 5841097E.nxr), to our
// Cloudflare Worker (King Kong's, its own BF route and tables: server-api e1a440b), which keeps the two newest
// versions' files. At a release they are fetched (fetch-bf-pipelines.mjs) and merged into the shipped list; a new
// pipeline's shaders are found by their hashes in our own copy of the game files (tools/shader_list.py).
//
// What is sent: the pipelines and translations (render state and shader hashes), a random install id made once (only
// the server's rate limit uses it), the version, the API (vulkan) and the levels played this session. No names, no
// paths, no address is kept by the server. Settings > Gameplay > Share graphics data turns it off.
//
// When: two minutes after the start, then every 15 minutes, only when the storage changed since the last upload
// (pipelines-sent.txt beside the settings). The server takes one upload per install every 10 minutes; a refusal or a
// failure waits for the next turn, never a loop.

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../third_party/miniz/miniz.h"
#include "http.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

REXCVAR_DEFINE_BOOL(bf_share_pipelines, true, "BF1943",
                    "Send the graphics pipelines this game builds (no game files, no personal data) to our server, "
                    "for the next update's shader list");
REXCVAR_DEFINE_STRING(bf_pipelines_url, "https://kk-launcher-telemetry.tekrantgaming.workers.dev/v1/bf/pipelines",
                      "BF1943", "Where the graphics pipelines go (bf_share_pipelines)");
REXCVAR_DECLARE(bool, server);  // server_mode.cpp

#ifndef BF_VERSION_STRING
#define BF_VERSION_STRING "0.0.0"
#endif

namespace bf1943 {
// The release version (CMake BF_VERSION), for the launcher's Home.
const char* BfVersion() { return BF_VERSION_STRING; }
}  // namespace bf1943

namespace bf1943 {

namespace {
namespace fs = std::filesystem;

std::mutex g_levels_mutex;
std::set<std::string> g_levels;  // the levels played this session ("WakeIsland", "Guadalcanal")

// The storage without its shader code: the header, then the 'T' and 'P' records as they are.
std::vector<uint8_t> Stripped(const std::vector<uint8_t>& d, uint32_t& pipelines) {
  pipelines = 0;
  std::vector<uint8_t> out;
  if (d.size() < 16 || std::memcmp(d.data(), "XNPS", 4)) return out;
  uint32_t state_size;
  std::memcpy(&state_size, d.data() + 8, 4);
  if (state_size == 0 || state_size > 4096) return out;
  out.assign(d.begin(), d.begin() + 16);
  size_t at = 16;
  while (at < d.size()) {
    const uint8_t tag = d[at];
    if (tag == 'S') {
      if (at + 14 > d.size()) break;
      uint32_t dwords;
      std::memcpy(&dwords, d.data() + at + 2, 4);
      at += 14 + size_t(dwords) * 4;
    } else if (tag == 'T') {
      if (at + 18 > d.size()) break;
      out.insert(out.end(), d.begin() + at, d.begin() + at + 18);
      at += 18;
    } else if (tag == 'P') {
      const size_t n = 1 + 32 + size_t(state_size);
      if (at + n > d.size()) break;
      out.insert(out.end(), d.begin() + at, d.begin() + at + n);
      ++pipelines;
      at += n;
    } else {
      break;  // a record cut short at the end (the game closed mid write): the rest is left out
    }
  }
  return out;
}

uint64_t Fnv1a(const std::vector<uint8_t>& d) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (uint8_t b : d) h = (h ^ b) * 0x100000001b3ull;
  return h;
}

// A gzip member of the data (miniz's raw deflate in the gzip wrapper).
std::vector<uint8_t> Gzip(const std::vector<uint8_t>& d) {
  std::vector<uint8_t> out = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
  size_t deflated_size = 0;
  void* deflated = tdefl_compress_mem_to_heap(d.data(), d.size(), &deflated_size, TDEFL_DEFAULT_MAX_PROBES);
  if (!deflated) return {};
  out.insert(out.end(), static_cast<uint8_t*>(deflated), static_cast<uint8_t*>(deflated) + deflated_size);
  mz_free(deflated);
  const uint32_t crc = uint32_t(mz_crc32(MZ_CRC32_INIT, d.data(), d.size())), size = uint32_t(d.size());
  for (int i = 0; i < 4; ++i) out.push_back(uint8_t(crc >> (8 * i)));
  for (int i = 0; i < 4; ++i) out.push_back(uint8_t(size >> (8 * i)));
  return out;
}

std::string InstallId(const fs::path& user_dir) {
  const fs::path file = user_dir / "install-id.txt";
  std::string id;
  std::ifstream(file) >> id;
  if (id.size() == 36) return id;
  std::random_device r;
  char buf[40];
  const uint32_t a = r(), b = r(), c = r(), e = r();
  std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%04x%08x", a, b >> 16, (b & 0x0FFF) | 0x4000,
                (c >> 16 & 0x3FFF) | 0x8000, c & 0xFFFF, e);
  id = buf;
  std::ofstream(file, std::ios::trunc) << id << "\n";
  return id;
}

// POST the body; the HTTP status (0: no answer).
int Post(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
         const std::vector<uint8_t>& body, std::string& reply) {
#if defined(_WIN32)
  std::wstring wurl(url.begin(), url.end());
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256] = {}, path[1024] = {};
  uc.lpszHostName = host, uc.dwHostNameLength = 255;
  uc.lpszUrlPath = path, uc.dwUrlPathLength = 1023;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return 0;
  int status = 0;
  HINTERNET session = WinHttpOpen(L"Battlefield1943Reborn", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return 0;
  WinHttpSetTimeouts(session, 10000, 10000, 15000, 15000);
  if (HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0)) {
    const DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    if (HINTERNET request = WinHttpOpenRequest(connect, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, flags)) {
      std::wstring all;
      for (const auto& [k, v] : headers) all += std::wstring(k.begin(), k.end()) + L": " + std::wstring(v.begin(), v.end()) + L"\r\n";
      if (WinHttpSendRequest(request, all.c_str(), DWORD(-1), const_cast<uint8_t*>(body.data()), DWORD(body.size()),
                             DWORD(body.size()), 0) &&
          WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &size, WINHTTP_NO_HEADER_INDEX);
        status = int(code);
        char buf[512];
        DWORD got = 0;
        while (WinHttpReadData(request, buf, sizeof(buf), &got) && got && reply.size() < 4096) reply.append(buf, got);
      }
      WinHttpCloseHandle(request);
    }
    WinHttpCloseHandle(connect);
  }
  WinHttpCloseHandle(session);
  return status;
#else
  return HttpPost(url, headers, body, reply, 15000);  // Linux: through the curl program (http.h)
#endif
}

// Where this game sends its pipelines; empty for none. Developer builds send only where BF_DEV_PIPELINES_URL says (a
// local Worker: wrangler dev --local), never to the live server.
std::string UploadUrl() {
#if defined(BF_DEV_TOOLS)
  const char* v = std::getenv("BF_DEV_PIPELINES_URL");
  return v && *v ? v : "";
#else
  return REXCVAR_GET(bf_pipelines_url);
#endif
}

void UploadOnce(const fs::path& user_dir, const fs::path& cache_dir) {
  if (!REXCVAR_GET(bf_share_pipelines)) return;
  const std::string url = UploadUrl();
  if (url.empty()) return;
  const fs::path storage = cache_dir / "native_x_renderer" / "5841097E.pipelines";
  std::ifstream in(storage, std::ios::binary);
  if (!in) return;
  const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  uint32_t pipelines = 0;
  const std::vector<uint8_t> body = Stripped(raw, pipelines);
  if (body.size() <= 16 || !pipelines) return;
  char hash[20];
  std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(Fnv1a(body)));
  const fs::path sent = user_dir / "pipelines-sent.txt";
  std::string last;
  std::ifstream(sent) >> last;
  if (last == hash) return;
  const std::vector<uint8_t> gz = Gzip(body);
  if (gz.empty() || gz.size() > 1000000) {
    REXLOG_INFO("BF pipelines: {} pipelines, {} KB zipped: not sent (over the server's 1 MB)", pipelines, gz.size() >> 10);
    return;
  }
  std::string levels;
  {
    std::lock_guard lock(g_levels_mutex);
    for (const std::string& l : g_levels) levels += (levels.empty() ? "" : ",") + l;
  }
  std::vector<std::pair<std::string, std::string>> headers = {{"Content-Type", "application/gzip"},
                                                              {"X-BF-Install", InstallId(user_dir)},
                                                              {"X-BF-Version", BF_VERSION_STRING},
                                                              {"X-BF-Api", "vulkan"}};
  if (!levels.empty()) headers.push_back({"X-BF-Levels", levels});
  std::string reply;
  const int status = Post(url, headers, gz, reply);
  if (status == 200) std::ofstream(sent, std::ios::trunc) << hash << "\n";
  REXLOG_INFO("BF pipelines: {} pipelines ({} KB zipped, levels {}) sent: HTTP {} {}", pipelines, gz.size() >> 10,
              levels.empty() ? "-" : levels, status, reply.substr(0, 120));
}
}  // namespace

// A level played this session (game_menu.cpp's join: the server list's map title), for the upload's X-BF-Levels.
void NotePlayedLevel(const std::string& title) {
  std::string name;
  for (char c : title)
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.') name += c;
  if (name.empty() || name.size() > 40) return;
  std::lock_guard lock(g_levels_mutex);
  if (g_levels.size() < 32) g_levels.insert(name);
}

// The game's start (bf1943_app.h): the upload thread, in a player's game only.
void StartPipelineUploads(const std::filesystem::path& user_dir, const std::filesystem::path& cache_dir) {
  if (REXCVAR_GET(server)) return;
  std::thread([user_dir, cache_dir] {
#if defined(BF_DEV_TOOLS)
    // BF_DEV_PIPELINES_AFTER=<s> (tests): the first upload that long after the start (default 120).
    const char* v = std::getenv("BF_DEV_PIPELINES_AFTER");
    std::this_thread::sleep_for(std::chrono::seconds(v && *v ? std::atoi(v) : 120));
#else
    std::this_thread::sleep_for(std::chrono::seconds(120));
#endif
    for (;;) {
      UploadOnce(user_dir, cache_dir);
      std::this_thread::sleep_for(std::chrono::minutes(15));
    }
  }).detach();
}

}  // namespace bf1943
