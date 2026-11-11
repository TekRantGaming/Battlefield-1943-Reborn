// HTTP for the updater and pipeline sharing (King Kong's http.h): WinHTTP on Windows; on Linux the curl program
// (installed on SteamOS, Bazzite and the usual desktop distributions), started without the AppImage's library path.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace bf1943 {

// GET into memory over https (or http: a test feed on this PC), following redirects (GitHub's downloads). Adds to
// `bytes` as data arrives; sets `total` to the response's size when it gives one. `timeout_ms` > 0 limits each step
// (connecting, sending, waiting for data). True on a 200.
bool HttpGet(const std::string& url, std::vector<uint8_t>& out, std::atomic<uint64_t>* bytes = nullptr,
             std::atomic<uint64_t>* total = nullptr, int timeout_ms = 0);

#if !defined(_WIN32)
// POST `body` with the headers; the reply (up to 64 KB) into `reply`. The HTTP status, or 0 when there was no answer.
// Linux only: Windows keeps pipeline_upload.cpp's own WinHTTP call.
int HttpPost(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
             const std::vector<uint8_t>& body, std::string& reply, int timeout_ms);

// Runs argv[0] (found in PATH) with its standard output into `out` (counted into `bytes`), its errors thrown away, and
// the environment without LD_LIBRARY_PATH (the AppImage's own libraries are not the system programs'). Its exit code,
// or -1 when it could not be started.
int RunCapture(const std::vector<std::string>& argv, std::vector<uint8_t>& out, std::atomic<uint64_t>* bytes = nullptr);
#endif

}  // namespace bf1943
