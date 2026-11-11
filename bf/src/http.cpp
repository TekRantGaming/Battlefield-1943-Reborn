// HTTP for the updater and pipeline sharing (http.h).

#include "http.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
extern char** environ;
#endif

#include <rex/logging.h>

namespace bf1943 {
const char* BfVersion();  // pipeline_upload.cpp

#if defined(_WIN32)
bool HttpGet(const std::string& url, std::vector<uint8_t>& out, std::atomic<uint64_t>* bytes,
             std::atomic<uint64_t>* total, int timeout_ms) {
  URL_COMPONENTSW parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256], path[2048];
  parts.lpszHostName = host;
  parts.dwHostNameLength = 256;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 2048;
  const std::wstring wurl(url.begin(), url.end());
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) return false;
  // GitHub's API needs a User-Agent; this is it.
  const std::string agent = std::string("BF1943Reborn/") + BfVersion();
  HINTERNET session = WinHttpOpen(std::wstring(agent.begin(), agent.end()).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;
  if (timeout_ms > 0) WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
  else WinHttpSetTimeouts(session, 15000, 15000, 30000, 30000);
  bool ok = false;
  if (HINTERNET conn = WinHttpConnect(session, host, parts.nPort, 0)) {
    const DWORD secure = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    if (HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, secure)) {
      DWORD code = 0, len = sizeof(code);
      if (WinHttpSendRequest(req, L"Accept: application/vnd.github+json, */*\r\n", DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0,
                             0, 0) &&
          WinHttpReceiveResponse(req, nullptr) &&
          WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &len,
                              nullptr) &&
          code == 200) {
        DWORD content = 0;
        len = sizeof(content);
        if (total && WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
                                         &content, &len, nullptr) &&
            content)
          *total = content;
        ok = true;
        for (DWORD avail = 0; WinHttpQueryDataAvailable(req, &avail) && avail;) {
          const size_t at = out.size();
          out.resize(at + avail);
          DWORD read = 0;
          if (!WinHttpReadData(req, out.data() + at, avail, &read)) {
            ok = false;
            break;
          }
          out.resize(at + read);
          if (bytes) *bytes += read;
        }
      } else if (code) {
        REXLOG_INFO("BF http: {} answered {}", url, code);
      }
      WinHttpCloseHandle(req);
    }
    WinHttpCloseHandle(conn);
  }
  WinHttpCloseHandle(session);
  return ok;
}
#else
int RunCapture(const std::vector<std::string>& argv, std::vector<uint8_t>& out, std::atomic<uint64_t>* bytes) {
  if (argv.empty()) return -1;
  int fds[2];
  if (pipe(fds) != 0) return -1;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, fds[0]);
  posix_spawn_file_actions_addclose(&actions, fds[1]);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  std::vector<char*> args;
  for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  std::vector<char*> env;  // without the AppImage's LD_LIBRARY_PATH (King Kong's lesson)
  for (char** e = environ; *e; ++e)
    if (std::strncmp(*e, "LD_LIBRARY_PATH=", 16) != 0) env.push_back(*e);
  env.push_back(nullptr);
  pid_t pid = 0;
  const int rc = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), env.data());
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);
  if (rc != 0) {
    close(fds[0]);
    return -1;
  }
  uint8_t buf[1 << 16];
  for (;;) {
    const ssize_t n = read(fds[0], buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    out.insert(out.end(), buf, buf + n);
    if (bytes) *bytes += uint64_t(n);
  }
  close(fds[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool HttpGet(const std::string& url, std::vector<uint8_t>& out, std::atomic<uint64_t>* bytes,
             std::atomic<uint64_t>* total, int timeout_ms) {
  (void)total;  // the caller knows the size from the release (curl gives no size before the end)
  const std::string agent = std::string("BF1943Reborn/") + BfVersion();
  std::vector<std::string> argv = {"curl", "-sSfL", "--connect-timeout", "15", "-A", agent, "-H",
                                   "Accept: application/vnd.github+json, */*"};
  if (timeout_ms > 0) {
    argv.push_back("-m");
    argv.push_back(std::to_string((timeout_ms + 999) / 1000));
  } else {  // a download: given up only when it stalls (under 1 KB a second for a minute)
    for (const char* a : {"--speed-limit", "1024", "--speed-time", "60"}) argv.push_back(a);
  }
  argv.push_back(url);
  const int code = RunCapture(argv, out, bytes);
  if (code == -1) REXLOG_WARN("BF http: could not start curl (is it installed?)");
  else if (code != 0) REXLOG_INFO("BF http: curl gave {} for {}", code, url);  // 22: an HTTP error, 6/7: no connection
  return code == 0;
}

int HttpPost(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
             const std::vector<uint8_t>& body, std::string& reply, int timeout_ms) {
  // curl reads the body from a file (it may be binary) and prints the reply, then the status on a line of its own.
  std::error_code ec;
  const std::filesystem::path scratch =
      std::filesystem::temp_directory_path(ec) / ("bf1943-post-" + std::to_string(getpid()) + ".bin");
  {
    std::ofstream f(scratch, std::ios::binary | std::ios::trunc);
    if (!f.write(reinterpret_cast<const char*>(body.data()), std::streamsize(body.size()))) return 0;
  }
  char seconds[32];
  std::snprintf(seconds, sizeof(seconds), "%.1f", timeout_ms > 500 ? timeout_ms / 1000.0 : 0.5);
  std::vector<std::string> argv = {"curl", "-sS", "-m", seconds, "-A", std::string("BF1943Reborn/") + BfVersion(),
                                   "-w", "\n%{http_code}"};
  for (const auto& [k, v] : headers) {
    argv.push_back("-H");
    argv.push_back(k + ": " + v);
  }
  argv.push_back("--data-binary");
  argv.push_back("@" + scratch.string());
  argv.push_back(url);
  std::vector<uint8_t> printed;
  const int code = RunCapture(argv, printed);
  std::filesystem::remove(scratch, ec);
  if (code != 0) return 0;
  std::string text(printed.begin(), printed.end());
  const size_t nl = text.rfind('\n');
  if (nl == std::string::npos) return 0;
  reply = text.substr(0, std::min<size_t>(nl, 65536));
  return std::atoi(text.c_str() + nl + 1);
}
#endif

}  // namespace bf1943
