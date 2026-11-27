// Report an issue: what the in game form sends (report.h; the design in docs/REPORTS.md). The page is
// game_menu_report.inc.

#include "report.h"

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
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <ctime>
#include <fstream>
#include <map>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "account_key.h"
#include "diagnostics.h"
#include "http.h"
#include "../third_party/miniz/miniz.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include "stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

// The relay on the King Kong coordinator's Worker (server/report_relay; live since 16 Nov 2026, as bobandersonplexv2-ui).
// Test runs pass a closed local port instead (tools/run_game.ps1), so a test never files a real issue.
REXCVAR_DEFINE_STRING(bf_report_endpoint, "https://kk-launcher-telemetry.tekrantgaming.workers.dev", "BF1943",
                      "Report an issue: the relay that files reports on GitHub (empty: GitHub's page in the browser only)");
REXCVAR_DEFINE_BOOL(bf_report_dry_run, false, "BF1943",
                    "Report an issue: nothing is sent; the request and the GitHub link are written to user\\reports");

namespace bf1943 {
const char* BfVersion();  // pipeline_upload.cpp
}  // namespace bf1943

namespace bf1943::report {
namespace {

constexpr const char* kTypeNames[] = {"Crash", "Bug", "Performance", "Server or connection", "Suggestion"};
constexpr const char* kTypeLabels[] = {"crash", "bug", "performance", "server", "suggestion"};
constexpr const char* kTypeFiles[] = {"crash.yml", "bug.yml", "performance.yml", "server.yml", "suggestion.yml"};
constexpr int kLogLines = 400;
constexpr size_t kLogBytes = 40 * 1024;
constexpr size_t kCrashBytes = 8 * 1024;
// The whole log (sent beside the issue, stored by the relay: server/report_relay MAX_LOG): at most this compressed; of a
// larger log its start (the system and the settings) and its newest part.
constexpr size_t kWholeLogGzMax = 1400 * 1024;
constexpr size_t kWholeLogRawMax = size_t(48) << 20;
constexpr size_t kWholeLogHead = size_t(256) << 10;
constexpr int kMaxTries = 5;

std::string Stamp() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char b[32];
  std::strftime(b, sizeof(b), "%Y%m%d-%H%M%S", &tm);
  return b;
}

fs::path ReportsDir(const fs::path& user_dir) {
  std::error_code ec;
  fs::create_directories(user_dir / "reports", ec);
  return user_dir / "reports";
}

std::string ReadFile(const fs::path& p, size_t max = 1 << 24) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::string s;
  s.resize(max);
  f.read(s.data(), std::streamsize(max));
  s.resize(size_t(f.gcount()));
  return s;
}
void WriteFile(const fs::path& p, const std::string& s) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(s.data(), std::streamsize(s.size()));
}

// ---- a little JSON: strings escaped, and flat objects of strings and numbers read back (our own files and the
// relay's answer)

std::string Json(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof(b), "\\u%04x", c);
          out += b;
        } else {
          out += char(c);
        }
    }
  }
  return out + "\"";
}

void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6)), out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12)), out += char(0x80 | ((cp >> 6) & 0x3F)), out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18)), out += char(0x80 | ((cp >> 12) & 0x3F)), out += char(0x80 | ((cp >> 6) & 0x3F)),
        out += char(0x80 | (cp & 0x3F));
  }
}

std::map<std::string, std::string> ParseFlat(const std::string& s) {
  std::map<std::string, std::string> out;
  size_t i = 0;
  auto ws = [&] {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  };
  auto str = [&](std::string& v) {
    if (i >= s.size() || s[i] != '"') return false;
    for (++i; i < s.size() && s[i] != '"'; ++i) {
      if (s[i] != '\\' || i + 1 >= s.size()) {
        v += s[i];
        continue;
      }
      const char e = s[++i];
      if (e == 'n') v += '\n';
      else if (e == 'r') v += '\r';
      else if (e == 't') v += '\t';
      else if (e == 'u' && i + 4 < s.size()) {
        AppendUtf8(v, uint32_t(std::strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16)));
        i += 4;
      } else v += e;
    }
    ++i;
    return true;
  };
  ws();
  if (i >= s.size() || s[i] != '{') return out;
  ++i;
  for (;;) {
    ws();
    std::string k, v;
    if (!str(k)) break;
    ws();
    if (i >= s.size() || s[i] != ':') break;
    ++i;
    ws();
    if (i < s.size() && s[i] == '"') {
      str(v);
    } else {  // a number, true, false, null; an array or object is skipped
      int depth = 0;
      for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '[' || c == '{') ++depth;
        else if (c == ']' || c == '}') {
          if (depth == 0) break;
          --depth;
        } else if (c == ',' && depth == 0) break;
        v += c;
      }
      while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
    }
    out[k] = v;
    ws();
    if (i < s.size() && s[i] == ',') {
      ++i;
      continue;
    }
    break;
  }
  return out;
}

std::string Base64(const std::string& in) {
  static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  for (size_t i = 0; i < in.size(); i += 3) {
    const uint32_t n = (uint32_t(uint8_t(in[i])) << 16) | (i + 1 < in.size() ? uint32_t(uint8_t(in[i + 1])) << 8 : 0) |
                       (i + 2 < in.size() ? uint32_t(uint8_t(in[i + 2])) : 0);
    out += k[(n >> 18) & 63], out += k[(n >> 12) & 63];
    out += i + 1 < in.size() ? k[(n >> 6) & 63] : '=';
    out += i + 2 < in.size() ? k[n & 63] : '=';
  }
  return out;
}

std::string UrlEncode(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += char(c);
    } else {
      char b[4];
      std::snprintf(b, sizeof(b), "%%%02X", c);
      out += b;
    }
  }
  return out;
}

// ---- the PC, read once a run

struct System {
  std::string os, cpu, gpu, driver, api;
  int cores = 0, threads = 0;
  double ram_gb = 0;
};

#if defined(_WIN32)
std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}
std::string RegString(HKEY root, const wchar_t* key, const wchar_t* value) {
  wchar_t buf[512] = {};
  DWORD size = sizeof(buf);
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
  return Narrow(buf);
}
#endif

std::string Trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  s = s.substr(a);
  // runs of spaces (CPU names pad with them) to one
  std::string out;
  for (char c : s)
    if (!(c == ' ' && !out.empty() && out.back() == ' ')) out += c;
  return out;
}

fs::path LogPath(const fs::path& user_dir) {
  const std::string flag = rex::cvar::GetFlagByName("log_file");
  return flag.empty() ? user_dir / "bf1943.log" : fs::path(flag);
}

// The GPU the game draws with and its driver, from this run's log (the SDK and gpu_preference.cpp write them early).
void GpuFromLog(const fs::path& log, System& s) {
  std::ifstream f(log);
  std::string line;
  for (int n = 0; n < 2000 && std::getline(f, line); ++n) {
    size_t at;
    if (s.gpu.empty() && (at = line.find("Vulkan device '")) != std::string::npos) {
      const size_t end = line.find('\'', at + 15);
      if (end != std::string::npos) s.gpu = line.substr(at + 15, end - at - 15), s.api = "Vulkan";
    }
    if (s.driver.empty() && (at = line.find("* driverInfo: ")) != std::string::npos) s.driver = Trim(line.substr(at + 14));
    if (s.api.empty() && (at = line.find("rexgpu-bfnative: ")) != std::string::npos) {
      const std::string rest = line.substr(at + 17);
      s.api = rest.substr(0, rest.find(' '));
    }
    if (s.gpu.empty() && (at = line.find("BF GPU: ")) != std::string::npos) {
      const std::string rest = line.substr(at + 8);
      const size_t colon = rest.find(": ");
      const size_t paren = rest.find(" (index");
      if (colon != std::string::npos && paren != std::string::npos && paren > colon) s.gpu = rest.substr(colon + 2, paren - colon - 2);
    }
  }
}

const System& GetSystem(const fs::path& user_dir) {
  static std::mutex m;
  static System s;
  static bool done = false;
  std::lock_guard lock(m);
  if (done && !s.gpu.empty()) return s;
  if (!done) {
    s.threads = int(std::thread::hardware_concurrency());
#if defined(_WIN32)
    {
      using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
      OSVERSIONINFOW v{};
      v.dwOSVersionInfoSize = sizeof(v);
      if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) fn(&v);
      const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
      const std::string edition = RegString(HKEY_LOCAL_MACHINE, cv, L"EditionID");
      const std::string display = RegString(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion");
      s.os = std::string(v.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10") + (edition.empty() ? "" : " " + edition) +
             (display.empty() ? "" : " " + display) + " (build " + std::to_string(v.dwBuildNumber) + ")";
      if (v.dwMajorVersion < 10) s.os = "Windows " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion);
      // Wine and Proton say so (a player on Linux running the Windows build).
      if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) s.os += ", under Wine or Proton";
      s.cpu = Trim(RegString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString"));
      DWORD len = 0;
      GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
      std::vector<uint8_t> buf(len);
      if (len && GetLogicalProcessorInformationEx(RelationProcessorCore,
                                                  reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data()), &len)) {
        for (DWORD at = 0; at < len;) {
          auto* p = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + at);
          ++s.cores;
          at += p->Size;
        }
      }
      MEMORYSTATUSEX ms{};
      ms.dwLength = sizeof(ms);
      if (GlobalMemoryStatusEx(&ms)) s.ram_gb = double(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
    }
#else
    {
      std::ifstream osr("/etc/os-release");
      std::string line;
      while (std::getline(osr, line))
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
          s.os = line.substr(12);
          s.os.erase(std::remove(s.os.begin(), s.os.end(), '"'), s.os.end());
        }
      utsname u{};
      if (uname(&u) == 0) s.os += std::string(s.os.empty() ? "" : ", ") + "Linux " + u.release;
      std::ifstream cpu("/proc/cpuinfo");
      std::set<std::string> cores;
      std::string phys;
      while (std::getline(cpu, line)) {
        if (s.cpu.empty() && line.rfind("model name", 0) == 0) s.cpu = Trim(line.substr(line.find(':') + 1));
        if (line.rfind("physical id", 0) == 0) phys = Trim(line.substr(line.find(':') + 1));
        if (line.rfind("core id", 0) == 0) cores.insert(phys + "/" + Trim(line.substr(line.find(':') + 1)));
      }
      s.cores = int(cores.size());
      struct sysinfo si{};
      if (sysinfo(&si) == 0) s.ram_gb = double(si.totalram) * si.mem_unit / (1024.0 * 1024.0 * 1024.0);
    }
#endif
    done = true;
  }
  GpuFromLog(LogPath(user_dir), s);
#if defined(_WIN32)
  // The driver from the display adapter's own entry when the log had none (Direct3D 12): NVIDIA's 32.0.15.6201 is
  // its 562.01.
  if (s.driver.empty() && !s.gpu.empty()) {
    const std::wstring cls = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\";
    for (int i = 0; i < 16 && s.driver.empty(); ++i) {
      wchar_t sub[8];
      std::swprintf(sub, 8, L"%04d", i);
      const std::wstring key = cls + sub;
      if (RegString(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverDesc") != s.gpu) continue;
      std::string v = RegString(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverVersion");
      if (s.gpu.find("NVIDIA") != std::string::npos) {
        std::string digits;
        for (char c : v)
          if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
        if (digits.size() >= 5) {
          const std::string last = digits.substr(digits.size() - 5);
          v = std::to_string(std::atoi(last.substr(0, 3).c_str())) + "." + last.substr(3) + " (" + v + ")";
        }
      }
      s.driver = v;
    }
  }
#endif
  return s;
}

// ---- the parts

std::string Fence(std::string text) {
  // A fence inside the text would end ours early.
  for (size_t at = 0; (at = text.find("```", at)) != std::string::npos;) text.replace(at, 3, "'''");
  while (!text.empty() && text.back() == '\n') text.pop_back();
  return "```\n" + text + "\n```";
}

std::string Details(const std::string& summary, const std::string& text) {
  return "<details><summary>" + summary + "</summary>\n\n" + Fence(text) + "\n\n</details>";
}

// The last `lines` lines of a file, at most `bytes`.
std::string Tail(const fs::path& p, int lines, size_t bytes) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f) return {};
  const std::streamoff size = f.tellg();
  const std::streamoff from = std::max<std::streamoff>(0, size - std::streamoff(bytes) * 2);
  f.seekg(from);
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (from > 0) s = s.substr(std::min(s.size(), s.find('\n') + 1));  // a whole first line
  int n = 0;
  size_t at = s.size();
  while (at > 0 && n <= lines) {
    const size_t nl = s.rfind('\n', at - 1);
    if (nl == std::string::npos) {
      at = 0;
      break;
    }
    if (nl + 1 < s.size()) ++n;
    if (n > lines) break;
    at = nl;
  }
  std::string out = s.substr(at == 0 ? 0 : at + 1);
  while (out.size() > bytes) out = out.substr(out.find('\n') + 1);
  // Long lines (the Vulkan extension lists) cut to 240 characters.
  std::string cut;
  std::istringstream in(out);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    cut += (line.size() > 240 ? line.substr(0, 240) + " ..." : line) + "\n";
  }
  return cut;
}

std::string Age(fs::file_time_type t) {
  const auto now = fs::file_time_type::clock::now();
  const auto min = std::chrono::duration_cast<std::chrono::minutes>(now - t).count();
  if (min < 2) return "just now";
  if (min < 120) return std::to_string(min) + " min ago";
  if (min < 48 * 60) return std::to_string(min / 60) + " h ago";
  return std::to_string(min / (60 * 24)) + " days ago";
}

double AgeHours(fs::file_time_type t) {
  return std::chrono::duration<double>(fs::file_time_type::clock::now() - t).count() / 3600.0;
}

// When this run started (the log's rotated parts written since are this run's).
const fs::file_time_type g_run_started = fs::file_time_type::clock::now();

// The whole log's files, oldest first: the run's rotated parts (bf1943.2.log, bf1943.1.log, written since this run
// started: the SDK's log_max_files), then the file itself. The run that crashed (bf1943.previous.log) is the file alone.
std::vector<fs::path> WholeLogFiles(const fs::path& log, bool this_run) {
  std::vector<fs::path> files;
  std::error_code ec;
  if (this_run)
    for (int n = 9; n >= 1; --n) {
      const fs::path part = log.parent_path() / (log.stem().string() + "." + std::to_string(n) + log.extension().string());
      if (fs::exists(part, ec) && fs::last_write_time(part, ec) >= g_run_started && !ec) files.push_back(part);
    }
  if (fs::exists(log, ec)) files.push_back(log);
  return files;
}

// A gzip member of the text (miniz's raw deflate in the gzip wrapper, as pipeline_upload.cpp sends pipelines).
std::string GzipText(const std::string& d) {
  std::string out("\x1f\x8b\x08\0\0\0\0\0\0\xff", 10);
  size_t deflated_size = 0;
  void* deflated = tdefl_compress_mem_to_heap(d.data(), d.size(), &deflated_size, TDEFL_DEFAULT_MAX_PROBES);
  if (!deflated) return {};
  out.append(static_cast<const char*>(deflated), deflated_size);
  mz_free(deflated);
  const uint32_t crc = uint32_t(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(d.data()), d.size()));
  const uint32_t size = uint32_t(d.size());
  for (int i = 0; i < 4; ++i) out.push_back(char(crc >> (8 * i)));
  for (int i = 0; i < 4; ++i) out.push_back(char(size >> (8 * i)));
  return out;
}

fs::path Newest(const fs::path& dir, const std::vector<std::string>& prefixes, const std::string& ext) {
  fs::path best;
  fs::file_time_type best_t{};
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    const std::string name = e.path().filename().string();
    bool want = false;
    for (const std::string& p : prefixes) want = want || name.rfind(p, 0) == 0;
    if (!want || e.path().extension() != ext) continue;
    const auto t = e.last_write_time(ec);
    if (best.empty() || t > best_t) best = e.path(), best_t = t;
  }
  return best;
}

std::string Line(const std::map<std::string, std::string>& kv, const char* k) {
  const auto it = kv.find(k);
  return it == kv.end() ? std::string() : it->second;
}

}  // namespace

const char* TypeName(int t) { return kTypeNames[std::clamp(t, 0, kTypeCount - 1)]; }
const char* TypeLabel(int t) { return kTypeLabels[std::clamp(t, 0, kTypeCount - 1)]; }
const char* TypeFile(int t) { return kTypeFiles[std::clamp(t, 0, kTypeCount - 1)]; }

// ---- the draft

bool LoadDraft(const fs::path& user_dir, Draft& d) {
  const std::string s = ReadFile(user_dir / "reports" / "draft.json", 64 * 1024);
  if (s.empty()) return false;
  const auto kv = ParseFlat(s);
  d.type = std::clamp(std::atoi(Line(kv, "type").c_str()), 0, kTypeCount - 1);
  d.title = Line(kv, "title"), d.what = Line(kv, "what"), d.steps = Line(kv, "steps");
  d.expected = Line(kv, "expected"), d.actual = Line(kv, "actual"), d.contact = Line(kv, "contact");
  d.often = kv.count("often") ? std::clamp(std::atoi(Line(kv, "often").c_str()), -1, kOftenCount - 1) : -1;
  return true;
}
void SaveDraft(const fs::path& user_dir, const Draft& d) {
  const std::string s = "{\"type\": " + std::to_string(d.type) + ", \"often\": " + std::to_string(d.often) +
                        ", \"title\": " + Json(d.title) + ", \"what\": " + Json(d.what) + ", \"steps\": " + Json(d.steps) +
                        ", \"expected\": " + Json(d.expected) + ", \"actual\": " + Json(d.actual) +
                        ", \"contact\": " + Json(d.contact) + "}\n";
  WriteFile(ReportsDir(user_dir) / "draft.json", s);
}
void ClearDraft(const fs::path& user_dir) {
  std::error_code ec;
  fs::remove(user_dir / "reports" / "draft.json", ec);
}

// ---- scrubbing

Scrubber MakeScrubber(const Context& c) {
  Scrubber s;
#if defined(_WIN32)
  wchar_t buf[256];
  DWORD n = 256;
  if (GetUserNameW(buf, &n)) s.names.push_back(Narrow(buf));
  n = 256;
  if (GetComputerNameW(buf, &n)) s.names.push_back(Narrow(buf));
  if (const char* p = std::getenv("USERPROFILE"); p && *p) s.names.push_back(fs::path(p).filename().string());
#else
  if (const char* u = std::getenv("USER"); u && *u) s.names.push_back(u);
  char host[256] = {};
  if (gethostname(host, sizeof(host) - 1) == 0 && *host) s.names.push_back(host);
  if (const char* h = std::getenv("HOME"); h && *h) s.names.push_back(fs::path(h).filename().string());
#endif
  // Not names too short or too common to scrub without wrecking the text.
  std::vector<std::string> keep;
  for (const std::string& n2 : s.names) {
    std::string low = n2;
    for (char& ch : low) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (n2.size() >= 3 && low != "user" && low != "admin" && low != "root" && low != "steam" && low != "deck" &&
        std::find(keep.begin(), keep.end(), n2) == keep.end())
      keep.push_back(n2);
  }
  s.names = keep;
  s.code = account::NormalizeCode(c.account_code);
  if (!c.user_dir.empty()) {
    std::ifstream(c.user_dir / "install-id.txt") >> s.install;
  }
  return s;
}

namespace {
std::string RegexEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (std::strchr("\\^$.|?*+()[]{}-", c)) out += '\\';
    out += c;
  }
  return out;
}
bool IsLoopbackOrVersion(const std::string& text, size_t at, const std::string& ip) {
  if (ip.rfind("127.", 0) == 0 || ip.rfind("0.", 0) == 0 || ip == "255.255.255.255") return true;
  // A version (0.4.6.7, Vulkan's 1.4.3.3) right after "version" or a "v", or a graphics driver's (issue 41, 17 Nov
  // 2026: an AMD driver of four small numbers became "driver [ip]" in the report's system table).
  const size_t from = at >= 16 ? at - 16 : 0;
  std::string before = text.substr(from, at - from);
  for (char& c : before) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (before.find("version") != std::string::npos || before.find("driver") != std::string::npos) return true;
  return at > 0 && (text[at - 1] == 'v' || text[at - 1] == 'V');
}
}  // namespace

namespace {
// The fixed rules, compiled once (std::regex is slow to build). Applied a line at a time (MSVC's matcher recurses per
// character of a match, and a throw then loses only that line).
struct Rules {
  std::regex users{R"(([A-Za-z]:[\\/]+(?:Users|Documents and Settings)[\\/]+)[^\\/\r\n"']+)", std::regex::icase};
  std::regex home{R"((/home/|/Users/|/var/home/)[^/\s"']+)"};
  std::regex auth{R"((authorization:\s*(?:bearer|token|basic)?\s*)\S+)", std::regex::icase};
  std::regex assign{R"(((?:token|key|secret|password|passwd|pwd|code)\s*[=:]\s*)[^\s,;&"']{6,})", std::regex::icase};
  std::regex github{R"(\b(?:gh[pousr]_|github_pat_)[A-Za-z0-9_]{20,}\b)"};
  std::regex uuid{R"(\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\b)"};
  std::regex longrun{R"([A-Za-z0-9+/_=-]{32,})"};
  std::regex email{R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})"};
  std::regex ipv4{R"(\b(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})\b(:\d{1,5})?)"};
  std::regex ipv6{R"((?:[0-9A-Fa-f]{1,4}:){4,7}[0-9A-Fa-f]{1,4}|(?:[0-9A-Fa-f]{1,4}:)+:(?:[0-9A-Fa-f]{1,4})?(?::[0-9A-Fa-f]{1,4})*|::(?:[fF]{4}:)?[0-9A-Fa-f]{1,4}(?::[0-9A-Fa-f]{1,4})+)"};
};
const Rules& FixedRules() {
  static const Rules r;
  return r;
}
std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
// This scrubber's own rules: its names, the recovery code in any spacing, the install id. Each with the text (lower
// case) a line must hold for the rule to be tried; `spaced`: looked for with the line's spaces and dashes left out.
struct OwnRule {
  std::regex re;
  std::string with, guard;
  bool spaced = false;
};
struct OwnRules {
  std::vector<OwnRule> list;
};
OwnRules MakeOwnRules(const Scrubber& s) {
  OwnRules o;
  for (size_t i = 0; i < s.names.size(); ++i)
    // As a word (not inside a longer one), any case.
    o.list.push_back({std::regex("(^|[^A-Za-z0-9])" + RegexEscape(s.names[i]) + "(?=$|[^A-Za-z0-9])", std::regex::icase),
                      i == 0 ? "$1[user]" : "$1[name]", Lower(s.names[i])});
  if (s.code.size() == 20) {
    std::string pat;
    for (size_t i = 0; i < s.code.size(); ++i) pat += (i ? "[\\s-]?" : "") + RegexEscape(std::string(1, s.code[i]));
    o.list.push_back({std::regex(pat, std::regex::icase), "[recovery code]", Lower(s.code), true});
  }
  if (!s.install.empty())
    o.list.push_back({std::regex(RegexEscape(s.install), std::regex::icase), "[install]", Lower(s.install)});
  return o;
}

// An address's replacement: "[ip]", or the log's numbered label (LogScrubLine).
using IpLabel = std::function<std::string(const std::string&)>;

// A long run that is a name, not a key (26 Nov 2026: the whole log's Vulkan feature names and folders became [key]):
// a path (two or more '/', no part of 32 or more), or letters and digits changing places at most 3 times
// (shaderDenormFlushToZeroFloat32, VK_KHR_get_physical_device_properties2, linux-amd64-release-dev); a key's
// letters and digits change about every 4 characters.
bool NotAKey(const std::string& m) {
  if (std::count(m.begin(), m.end(), '/') >= 2) {
    size_t longest = 0, part = 0;
    for (char c : m) part = c == '/' ? 0 : part + 1, longest = std::max(longest, part);
    if (longest < 32) return true;
  }
  int changes = 0;
  for (size_t i = 1; i < m.size(); ++i) {
    const unsigned char a = static_cast<unsigned char>(m[i - 1]), b = static_cast<unsigned char>(m[i]);
    if ((std::isdigit(a) && std::isalpha(b)) || (std::isalpha(a) && std::isdigit(b))) ++changes;
  }
  return changes <= 3;
}
// A value that is an address or a range of them (code=82000000-82DB0000), not a secret: hex groups of up to 16.
bool AddressValue(const std::string& v) {
  std::string s = v.rfind("0x", 0) == 0 || v.rfind("0X", 0) == 0 ? v.substr(2) : v;
  size_t group = 0;
  for (char c : s) {
    if (c == '-') {
      if (!group) return false;
      group = 0;
    } else if (std::isxdigit(static_cast<unsigned char>(c))) {
      if (++group > 16) return false;
    } else {
      return false;
    }
  }
  return group > 0;
}

// A run of 32 or more of base64's letters (the longrun rule's least).
bool HasLongRun(const std::string& t) {
  size_t run = 0;
  for (char c : t) {
    run = std::isalnum(static_cast<unsigned char>(c)) || std::strchr("+/_=-", c) ? run + 1 : 0;
    if (run >= 32) return true;
  }
  return false;
}

// Each rule is tried only where the text it needs is in the line (the log file passes every line through these as it
// is written, and most lines need none): the checks only skip what could not match.
std::string ScrubLine(const std::string& line, const Rules& r, const OwnRules& own, const IpLabel* ip_label = nullptr) {
  std::string t = line;
  const std::string low = Lower(line);
  auto has = [&](const char* what) { return low.find(what) != std::string::npos; };
  const auto dashes = std::count(line.begin(), line.end(), '-'), dots = std::count(line.begin(), line.end(), '.'),
             colons = std::count(line.begin(), line.end(), ':');
  // Other users' and the home folders' names first (this user's own paths are caught by the names too).
  if (has(":\\") || has(":/")) t = std::regex_replace(t, r.users, "$1[user]");
  if (has("/home/") || has("/users/")) t = std::regex_replace(t, r.home, "$1[user]");
  std::string spaced_low;
  for (const OwnRule& o : own.list) {
    if (o.spaced) {
      if (spaced_low.empty())
        for (char c : low)
          if (c != ' ' && c != '\t' && c != '-') spaced_low += c;
      if (spaced_low.find(o.guard) == std::string::npos) continue;
    } else if (low.find(o.guard) == std::string::npos) {
      continue;
    }
    t = std::regex_replace(t, o.re, o.with);
  }
  // Keys and tokens.
  if (has("authorization")) t = std::regex_replace(t, r.auth, "$1[key]");
  if (has("token") || has("key") || has("secret") || has("passw") || has("pwd") || has("code"))
  {
    std::string out;
    size_t last = 0;
    for (auto it = std::sregex_iterator(t.begin(), t.end(), r.assign); it != std::sregex_iterator(); ++it) {
      const auto& m = *it;
      if (AddressValue(m.str().substr(size_t(m[1].length())))) continue;
      out += t.substr(last, size_t(m.position()) - last) + m[1].str() + "[key]";
      last = size_t(m.position()) + size_t(m.length());
    }
    t = out + t.substr(last);
  }
  if (has("_")) t = std::regex_replace(t, r.github, "[key]");
  if (dashes >= 4) t = std::regex_replace(t, r.uuid, "[id]");
  if (HasLongRun(t)) {
    // Long runs that look like keys: 32 or more of base64's letters with both letters and digits.
    std::string out;
    size_t last = 0;
    for (auto it = std::sregex_iterator(t.begin(), t.end(), r.longrun); it != std::sregex_iterator(); ++it) {
      const std::string m = it->str();
      const bool letters = std::any_of(m.begin(), m.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)); });
      const bool digits = std::any_of(m.begin(), m.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
      if (!letters || !digits || m.find('/') == 0 || NotAKey(m)) continue;
      out += t.substr(last, size_t(it->position()) - last) + "[key]";
      last = size_t(it->position()) + m.size();
    }
    t = out + t.substr(last);
  }
  if (has("@")) t = std::regex_replace(t, r.email, "[email]");
  if (dots >= 3) {
    // IPv4 (loopback and versions kept), with a port if it has one.
    std::string out;
    size_t last = 0;
    for (auto it = std::sregex_iterator(t.begin(), t.end(), r.ipv4); it != std::sregex_iterator(); ++it) {
      const auto& m = *it;
      bool ip = true;
      for (int g = 1; g <= 4; ++g) ip = ip && std::atoi(m[g].str().c_str()) <= 255;
      const std::string addr = m[1].str() + "." + m[2].str() + "." + m[3].str() + "." + m[4].str();
      if (!ip || IsLoopbackOrVersion(t, size_t(m.position()), addr)) continue;
      // Part of a longer dotted run (a driver version, 32.0.15.6201.1): not an address.
      const size_t end = size_t(m.position()) + size_t(m.length());
      if ((m.position() > 0 && t[size_t(m.position()) - 1] == '.') ||
          (end < t.size() && t[end] == '.' && end + 1 < t.size() && std::isdigit(static_cast<unsigned char>(t[end + 1]))))
        continue;
      out += t.substr(last, size_t(m.position()) - last) + (ip_label ? (*ip_label)(addr) : "[ip]");
      last = end;
    }
    t = out + t.substr(last);
  }
  if (colons >= 4 || has("::")) {
    // IPv6: hex groups with :: or with four colons or more (never a time of day, which has two).
    std::string out;
    size_t last = 0;
    for (auto it = std::sregex_iterator(t.begin(), t.end(), r.ipv6); it != std::sregex_iterator(); ++it) {
      const std::string m = it->str();
      const size_t pos = size_t(it->position());
      // Inside a word (C++'s rex::cvar) or the loopback ::1: kept.
      if (m == "::1" || (pos > 0 && std::isalnum(static_cast<unsigned char>(t[pos - 1])))) continue;
      if (std::count(m.begin(), m.end(), ':') < 2) continue;
      out += t.substr(last, pos - last) + (ip_label ? (*ip_label)(m) : "[ip]");
      last = pos + m.size();
    }
    t = out + t.substr(last);
  }
  return t;
}
}  // namespace

std::string Scrub(const std::string& text, const Scrubber& s) {
  // Nothing is ever sent unchecked: a rule that cannot run removes the text (or the line) it could not check.
  const Rules* rules = nullptr;
  OwnRules own;
  try {
    rules = &FixedRules();
    own = MakeOwnRules(s);
  } catch (const std::exception& e) {
    REXLOG_ERROR("BF report: the privacy check could not start ({}): the text is left out", e.what());
    return "[left out: the privacy check could not run]";
  }
  std::string out;
  out.reserve(text.size());
  for (size_t at = 0; at <= text.size();) {
    const size_t nl = std::min(text.find('\n', at), text.size());
    const std::string line = text.substr(at, nl - at);
    try {
      out += ScrubLine(line, *rules, own);
    } catch (const std::exception&) {
      out += "[a line left out: the privacy check could not read it]";
    }
    if (nl < text.size()) out += '\n';
    at = nl + 1;
  }
  return out;
}

struct LogScrubRules {
  OwnRules own;
};

std::shared_ptr<const LogScrubRules> MakeLogScrub(const Scrubber& s) {
  FixedRules();  // compiled now: a rule that cannot be built throws here, not at a line
  auto r = std::make_shared<LogScrubRules>();
  r->own = MakeOwnRules(s);
  return r;
}

std::string ScrubLogLine(const std::string& line, const LogScrubRules& rules,
                         const std::function<std::string(const std::string&)>& ip_label) {
  return ScrubLine(line, FixedRules(), rules.own, ip_label ? &ip_label : nullptr);
}

bool ScrubSelfTest(std::string& failures) {
  Scrubber s;
  s.names = {"PlayerOne", "DESKTOP-K9X2"};
  s.code = "ABCDE6GHJK7MNPQRST89";
  s.install = "0f8e2a11-4b2c-4d3e-8f00-123456789abc";
  const std::pair<std::string, std::string> cases[] = {
      {R"(C:\Users\PlayerOne\AppData\Local\bf1943\user\bf1943.log)", R"(C:\Users\[user]\AppData\Local\bf1943\user\bf1943.log)"},
      {R"(D:\Users\someone else\x)", R"(D:\Users\[user]\x)"},
      {"/home/deckuser/.local/share/bf1943", "/home/[user]/.local/share/bf1943"},
      {"user playerone on DESKTOP-K9X2", "user [user] on [name]"},
      {"code ABCDE 6GHJK 7MNPQ RST89 kept", "code [recovery code] kept"},
      {"code abcde-6ghjk-7mnpq-rst89", "code [recovery code]"},
      {"install 0f8e2a11-4b2c-4d3e-8f00-123456789abc", "install [install]"},
      {"joined 203.0.113.24:18300 ok", "joined [ip] ok"},
      {"bind to 127.0.0.1:0", "bind to 127.0.0.1:0"},
      {"Battlefield 1943 Reborn version 0.4.6.7", "Battlefield 1943 Reborn version 0.4.6.7"},
      {"driver 32.0.15.6201 ok", "driver 32.0.15.6201 ok"},
      {"RX 6500 XT, driver 23.40.27.6 (AMD)", "RX 6500 XT, driver 23.40.27.6 (AMD)"},
      {"[2026-11-16 04:37:39.560] [info]", "[2026-11-16 04:37:39.560] [info]"},
      {"peer 2001:db8:85a3::8a2e:370:7334 left", "peer [ip] left"},
      {"rex::cvar::GetFlag", "rex::cvar::GetFlag"},
      {"Authorization: Bearer abc.def.ghi", "Authorization: Bearer [key]"},
      {"token=gh" "p_abcdefghijklmnopqrstuvwxyz0123456789", "token=[key]"},  // (split: no token shape in the source)
      {"mail me at someone@example.com", "mail me at [email]"},
      {"VS 1B4EBA8B2370E9A7 draws", "VS 1B4EBA8B2370E9A7 draws"},
      {"seed aGVsbG8gd29ybGQgdGhpcyBpcyBhIGtleQ1234567", "seed [key]"},
      // The rules' cheap checks (26 Nov 2026) never skip a match: other spellings of the same things.
      {"C:/Users/PlayerOne/Saved Games", "C:/Users/[user]/Saved Games"},
      {"/Users/macuser/Library/Logs", "/Users/[user]/Library/Logs"},
      {"user PLAYERONE here", "user [user] here"},
      {"addr 2001:0db8:0000:0000:0000:ff00:0042:8329 ok", "addr [ip] ok"},
      {"Authorization: token abcdef123456", "Authorization: token [key]"},
      {"PASSWORD = hunter2hunter2", "PASSWORD = [key]"},
      {"at 10:41:07 the round ended", "at 10:41:07 the round ended"},
      // Names and folders are no keys (the whole log, 26 Nov 2026); a key among them still is.
      {"* VK_KHR_get_physical_device_properties2", "* VK_KHR_get_physical_device_properties2"},
      {"* shaderDenormFlushToZeroFloat32: 0", "* shaderDenormFlushToZeroFloat32: 0"},
      {"fonts from /mnt/ms1/Games/BF1943/out/build/linux-amd64-release-dev/fonts", "fonts from /mnt/ms1/Games/BF1943/out/build/linux-amd64-release-dev/fonts"},
      {"module: code=82000000-82DB0000, image=82000000", "module: code=82000000-82DB0000, image=82000000"},
      {"api code=9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08", "api code=[key]"},
      {"blob 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08 end", "blob [key] end"},
  };
  failures.clear();
  for (const auto& [in, want] : cases) {
    const std::string got = Scrub(in, s);
    if (got != want) failures += "  \"" + in + "\" gave \"" + got + "\", wanted \"" + want + "\"\n";
  }
  return failures.empty();
}

// ---- gathering

std::string SystemLine(const fs::path& user_dir) {
  const System& sys = GetSystem(user_dir);
  char ram[32];
  std::snprintf(ram, sizeof(ram), "%.0f GB RAM", sys.ram_gb);
  return (sys.os.empty() ? std::string("unknown OS") : sys.os) + " \xC2\xB7 " +
         (sys.cpu.empty() ? std::string("unknown CPU") : sys.cpu) + ", " + std::to_string(sys.cores) + " cores, " +
         std::to_string(sys.threads) + " threads \xC2\xB7 " + ram + " \xC2\xB7 " +
         (sys.gpu.empty() ? std::string("unknown GPU") : sys.gpu) + (sys.driver.empty() ? "" : ", driver " + sys.driver) +
         (sys.api.empty() ? "" : " (" + sys.api + ")") + " \xC2\xB7 game " + BfVersion();
}

std::vector<Part> Gather(const Context& c, int type) {
  const Scrubber sc = MakeScrubber(c);
  const System& sys = GetSystem(c.user_dir);
  std::vector<Part> parts;
  const std::string build =
#if defined(BF_DEV_TOOLS)
      "dev";
#else
      "release";
#endif
  const std::string platform =
#if defined(_WIN32)
      "Windows";
#else
      "Linux";
#endif
  {
    Part p{"game", "Game"};
    p.summary = std::string("Battlefield 1943 Reborn ") + BfVersion() + " \xC2\xB7 " + platform + " " + build + " build" +
                (c.tick_rate > 0 ? " \xC2\xB7 " + std::to_string(c.tick_rate) + " Hz game" : "");
    p.text = "| Version | " + std::string(BfVersion()) + " (" + platform + ", " + build + ")" +
             (c.tick_rate > 0 ? ", " + std::to_string(c.tick_rate) + " Hz" : "") + " |\n";
    // Settings > Diagnostics (diagnostics.h): what this run's log holds.
    if (diagnostics::Anonymized()) p.text += "| Logging | " + diagnostics::Summary() + " (anonymized log) |\n";
    // The player's marks this run (the mark key: each one's line and black box are in the whole log).
    if (const int marks = diagnostics::Marks(); marks > 0) {
      const std::time_t last = std::time_t(diagnostics::LastMarkTime());
      char when[32] = {};
      if (const std::tm* tm = std::localtime(&last)) std::strftime(when, sizeof(when), "%H:%M:%S", tm);
      p.text += "| Marks | " + std::to_string(marks) + " this run (the last at " + when +
                "; \"BF mark\" and \"BF black box\" in the whole log) |\n";
    }
    parts.push_back(p);
  }
  {
    Part p{"pc", "PC"};
    char ram[32];
    std::snprintf(ram, sizeof(ram), "%.0f GB", sys.ram_gb);
    const std::string cpu = (sys.cpu.empty() ? std::string("unknown CPU") : sys.cpu) +
                            (sys.cores ? ", " + std::to_string(sys.cores) + " cores" : "") +
                            (sys.threads ? ", " + std::to_string(sys.threads) + " threads" : "");
    const std::string gpu = (sys.gpu.empty() ? std::string("unknown GPU") : sys.gpu) +
                            (sys.driver.empty() ? "" : ", driver " + sys.driver);
    p.summary = Scrub(sys.os + " \xC2\xB7 " + cpu + " \xC2\xB7 " + ram + " \xC2\xB7 " + gpu, sc);
    p.text = Scrub("| OS | " + sys.os + " |\n| CPU | " + cpu + " |\n| RAM | " + ram + " |\n| GPU | " + gpu + " |\n", sc);
    parts.push_back(p);
  }
  {
    Part p{"display", "Display and graphics"};
    std::string list;
    for (const auto& [k, v] : c.settings) list += k + " = " + v + "\n";
    p.summary = c.display + (sys.api.empty() ? "" : " \xC2\xB7 " + sys.api) + " \xC2\xB7 " + std::to_string(c.settings.size()) +
                " settings";
    p.text = Scrub("| Display | " + c.display + (sys.api.empty() ? "" : ", " + sys.api) + " |\n", sc);
    if (!list.empty()) p.text += "\n" + Details("Graphics settings", Scrub(list, sc));
    parts.push_back(p);
  }
  {
    Part p{"match", "Last match"};
    const fs::path f = c.user_dir / "reports" / "last_match.txt";
    std::map<std::string, std::string> kv;
    std::ifstream in(f);
    std::string line;
    while (std::getline(in, line))
      if (const size_t eq = line.find('='); eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
    std::error_code ec;
    const auto t = fs::last_write_time(f, ec);
    p.available = !ec && !Line(kv, "server").empty() && AgeHours(t) < 24;
    if (p.available) {
      const std::string players = Line(kv, "players") + " of " + Line(kv, "max") + " players";
      const std::string ping = Line(kv, "ping");
      const std::string m = Line(kv, "server") + " (" + (Line(kv, "official") == "1" ? "Reborn" : "community") + ") \xC2\xB7 " +
                            Line(kv, "map") + " \xC2\xB7 " + players +
                            (ping.empty() || ping[0] == '-' ? "" : " \xC2\xB7 ping " + ping + " ms") + " \xC2\xB7 joined " + Age(t);
      p.summary = Scrub(m, sc);
      p.text = "| Last match | " + p.summary + " |\n";
    } else {
      p.summary = "No match in the last day";
      p.on = false;
    }
    parts.push_back(p);
  }
  fs::path crash_file;
  {
    Part p{"crash", "Crash report"};
    crash_file = Newest(c.user_dir / "crashes", {"crash-", "hang-"}, ".txt");
    std::error_code ec;
    const auto t = crash_file.empty() ? fs::file_time_type{} : fs::last_write_time(crash_file, ec);
    p.available = !crash_file.empty() && !ec && AgeHours(t) < 24 * 7;
    if (p.available) {
      const std::string text = Scrub(ReadFile(crash_file, kCrashBytes), sc);
      std::string first;  // "Exception: 0xc0000005 at bf1943.exe + 0x1a2f30"
      std::istringstream in(text);
      std::string line;
      while (std::getline(in, line))
        if (line.rfind("Exception:", 0) == 0 || line.rfind("Reason:", 0) == 0) first = line;
      p.summary = crash_file.filename().string() + " \xC2\xB7 " + Age(t) + (first.empty() ? "" : " \xC2\xB7 " + first) +
                  " \xC2\xB7 the minidump stays on your PC";
      p.text = Details("Crash report (" + crash_file.filename().string() + ")", text);
      p.on = type == kCrash || (type != kSuggestion && AgeHours(t) < 2);
    } else {
      p.summary = "No crash or hang report in the last 7 days";
      p.on = false;
      crash_file.clear();
    }
    parts.push_back(p);
  }
  const fs::path cur = LogPath(c.user_dir), prev = cur.parent_path() / "bf1943.previous.log";
  fs::path use = cur;
  {
    Part p{"log", "Log"};
    // The run that crashed: its log was kept as bf1943.previous.log at this start (KeepPreviousLog); a crash report
    // from before this run started points there.
    std::error_code ec;
    if (!crash_file.empty() && fs::exists(prev, ec)) {
      const auto crash_t = fs::last_write_time(crash_file, ec);
      const auto prev_t = fs::last_write_time(prev, ec);
      if (!ec && crash_t <= prev_t + std::chrono::minutes(2)) use = prev;
    }
    const std::string text = Scrub(Tail(use, kLogLines, kLogBytes), sc);
    p.available = !text.empty();
    const int lines = int(std::count(text.begin(), text.end(), '\n'));
    p.summary = "The last " + std::to_string(lines) + " lines of " + use.filename().string() +
                (use == prev ? " (the run that crashed)" : " (this run)");
    p.text = Details("Log: the last " + std::to_string(lines) + " lines of " + use.filename().string(), text);
    p.on = p.available && type != kSuggestion;
    parts.push_back(p);
  }
  {
    // The whole log, compressed (TekRantGaming, 26 Nov 2026: logs for reports, Settings > Diagnostics): too large for
    // the issue (GitHub's 65,536 characters), so the relay keeps it and links it from the issue. Scrubbed again here,
    // though the log was anonymized as it was written: nothing is sent unchecked.
    Part p{"fulllog", "Whole log"};
    const std::vector<fs::path> files = WholeLogFiles(use, use == cur);
    std::string all;
    size_t raw = 0;
    std::error_code ec;
    for (const fs::path& f : files) raw += size_t(fs::file_size(f, ec));
    for (const fs::path& f : files) {
      if (files.size() > 1) all += "----- " + f.filename().string() + " -----\n";
      all += ReadFile(f, kWholeLogRawMax);
    }
    // Its start (the system, the settings) and its newest part, as large as fits once compressed.
    all = Scrub(all, sc);
    size_t keep = std::min(all.size(), kWholeLogRawMax);
    for (int tries = 0; tries < 6 && !all.empty(); ++tries) {
      std::string text = all;
      if (keep < all.size()) {
        const size_t head = std::min(kWholeLogHead, keep / 4);
        text = all.substr(0, head) + "\n[... " + std::to_string((all.size() - keep) >> 10) +
               " KB of the log left out here: the whole log is too large to send ...]\n" +
               all.substr(all.size() - (keep - head));
      }
      p.data = GzipText(text);
      if (p.data.size() <= kWholeLogGzMax) break;
      p.data.clear();
      keep /= 2;
    }
    p.file = use;
    p.available = !p.data.empty();
    if (p.available) {
      char sizes[96];
      std::snprintf(sizes, sizeof(sizes), "%.1f MB, %.0f KB compressed", raw / 1048576.0, p.data.size() / 1024.0);
      p.summary = use.filename().string() + (files.size() > 1 ? " with its " + std::to_string(files.size() - 1) + " older part(s)" : "") +
                  " \xC2\xB7 " + sizes + " \xC2\xB7 anonymized \xC2\xB7 kept 30 days by the report service, linked from the issue";
    } else {
      p.summary = all.empty() ? "No log" : "Too large to send";
    }
    p.on = p.available && type != kSuggestion;
    parts.push_back(p);
  }
  {
    Part p{"screenshot", "Screenshot"};
    p.file = NewestScreenshot(c.user_dir, 24);
    p.available = !p.file.empty();
    if (p.available) {
      std::error_code ec;
      const auto size = fs::file_size(p.file, ec);
      p.summary = p.file.filename().string() + " \xC2\xB7 taken " + Age(fs::last_write_time(p.file, ec)) + " \xC2\xB7 " +
                  std::to_string(size / 1024) + " KB";
    } else {
      p.summary = "None: F12 in a match saves one";
      p.on = false;
    }
    parts.push_back(p);
  }
  return parts;
}

// ---- the issue

std::string Title(const Draft& d) { return "[" + std::string(TypeName(d.type)) + "] " + Trim(d.title); }

namespace {
struct Section {
  std::string id, heading, text;
};
// The form's sections in order, by the issue form's field ids (.github/ISSUE_TEMPLATE).
std::vector<Section> Sections(const Draft& d) {
  const bool idea = d.type == kSuggestion;
  std::vector<Section> s;
  s.push_back({"what", idea ? "The idea" : "What happened", Trim(d.what)});
  s.push_back({"steps", idea ? "Why it helps" : "Steps to reproduce", Trim(d.steps)});
  if (!idea) {
    s.push_back({"expected", "What I expected", Trim(d.expected)});
    s.push_back({"actual", "What happened instead", Trim(d.actual)});
    s.push_back({"often", "How often", d.often >= 0 ? kOften[d.often] : ""});
  }
  s.push_back({"contact", "Contact", Trim(d.contact)});
  return s;
}
std::string SystemText(const std::vector<Part>& parts, bool with_details) {
  std::string table = "| | |\n|---|---|\n", details;
  for (const Part& p : parts) {
    if (!p.on || !p.available || p.id == "crash" || p.id == "log" || p.id == "screenshot") continue;
    const size_t cut = p.text.find("\n\n<details>");
    table += cut == std::string::npos ? p.text : p.text.substr(0, cut + 1);  // the row with its own line end
    if (cut != std::string::npos && with_details) details += p.text.substr(cut + 2) + "\n";
  }
  return table + (details.empty() ? "" : "\n" + details);
}
const Part* FindPart(const std::vector<Part>& parts, const char* id) {
  for (const Part& p : parts)
    if (p.id == id && p.on && p.available) return &p;
  return nullptr;
}
}  // namespace

std::string Body(const Draft& d, const std::vector<Part>& parts) {
  std::string b;
  for (const Section& s : Sections(d))
    if (!s.text.empty()) b += "### " + s.heading + "\n\n" + s.text + "\n\n";
  b += "### System\n\n" + SystemText(parts, true) + "\n";
  if (const Part* p = FindPart(parts, "crash")) b += p->text + "\n\n";
  if (const Part* p = FindPart(parts, "log")) b += p->text + "\n\n";
  if (FindPart(parts, "fulllog")) b += "<!-- full log -->\n\n";  // the relay's link to the whole log
  if (FindPart(parts, "screenshot")) b += "<!-- screenshot -->\n\n";
  const std::string sig = Signature(parts);
  b += "---\n_Filed from the game's Report an issue page" + (sig.empty() ? std::string() : " \xC2\xB7 crash signature `" + sig + "`") +
       "_\n";
  return b;
}

std::string Signature(const std::vector<Part>& parts) {
  const Part* p = FindPart(parts, "crash");
  if (!p) return {};
  // The exception, its place, and the first game function: the same crash on any PC (no addresses of the heap).
  std::string key;
  std::istringstream in(p->text);
  std::string line;
  bool frame = false;
  while (std::getline(in, line)) {
    if (line.rfind("Exception:", 0) == 0 || line.rfind("Reason:", 0) == 0) key += line + "\n";
    if (!frame && line.find("(game function") != std::string::npos) key += line.substr(line.find("(game function")) + "\n", frame = true;
  }
  if (key.empty()) return {};
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : key) h = (h ^ c) * 1099511628211ull;
  char b[20];
  std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(h));
  return b;
}

std::string Validate(const Draft& d) {
  if (Trim(d.title).size() < kTitleMin) return "Give it a title of at least " + std::to_string(kTitleMin) + " characters.";
  if (Trim(d.what).size() < kWhatMin)
    return std::string(d.type == kSuggestion ? "Say what the idea is" : "Say what happened") + " in at least " +
           std::to_string(kWhatMin) + " characters.";
  return {};
}

std::string BrowserUrl(const Draft& d, const std::vector<Part>& parts, std::string& rest) {
  std::string url = std::string("https://github.com/") + kRepo + "/issues/new?template=" + TypeFile(d.type) +
                    "&title=" + UrlEncode(Title(d)) + "&labels=" + UrlEncode(std::string(TypeLabel(d.type)) + ",from-game");
  for (const Section& s : Sections(d))
    if (!s.text.empty()) url += "&" + s.id + "=" + UrlEncode(s.text);
  // The system table (the settings' details only if they fit), then the crash report and the log while they fit.
  std::string sys = SystemText(parts, true);
  if (url.size() + UrlEncode(sys).size() + 8 > kUrlMax) sys = SystemText(parts, false);
  url += "&system=" + UrlEncode(sys);
  rest.clear();
  std::string log;
  for (const char* id : {"crash", "log"}) {
    const Part* p = FindPart(parts, id);
    if (!p) continue;
    const std::string with = log + p->text + "\n\n";
    if (url.size() + UrlEncode("&log=" + with).size() <= kUrlMax) log = with;
    else rest += p->text + "\n\n";
  }
  if (!log.empty()) url += "&log=" + UrlEncode(log);
  return url;
}

std::string CopyText(const Draft& d, const std::vector<Part>& parts) {
  std::string b = Body(d, parts);
  for (const std::string mark : {"<!-- screenshot -->\n\n", "<!-- full log -->\n\n"})
    if (const size_t at = b.find(mark); at != std::string::npos) b.erase(at, mark.size());
  return "**" + Title(d) + "**\n\n" + b;
}

std::string RequestJson(const Draft& d, const std::vector<Part>& parts) {
  std::string j = "{\"type\": " + Json(TypeLabel(d.type)) + ", \"title\": " + Json(Title(d)) + ", \"body\": " + Json(Body(d, parts)) +
                  ", \"labels\": [" + Json(TypeLabel(d.type)) + ", \"from-game\", \"needs-triage\"]" +
                  ", \"signature\": " + Json(Signature(parts)) + ", \"version\": " + Json(BfVersion());
  if (const Part* p = FindPart(parts, "screenshot")) j += ", \"screenshot_jpeg\": " + Json(Base64(ReadFile(p->file, 1 << 20)));
  if (const Part* p = FindPart(parts, "fulllog")) j += ", \"log_gz\": " + Json(Base64(p->data));
  return j + "}";
}

// ---- sending

std::string Endpoint() {
  std::string e = REXCVAR_GET(bf_report_endpoint);
  if (const char* v = std::getenv("BF_REPORT_ENDPOINT"); v && *v) e = v;
  while (!e.empty() && e.back() == '/') e.pop_back();
  return e;
}
bool DryRun() {
  if (const char* v = std::getenv("BF_REPORT_DRY_RUN"); v && *v == '1') return true;
  return REXCVAR_GET(bf_report_dry_run);
}

namespace {
int Post(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body,
         std::string& reply) {
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
  WinHttpSetTimeouts(session, 10000, 10000, 20000, 20000);
  if (HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0)) {
    const DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    if (HINTERNET request = WinHttpOpenRequest(connect, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, flags)) {
      std::wstring all;
      for (const auto& [k, v] : headers) all += std::wstring(k.begin(), k.end()) + L": " + std::wstring(v.begin(), v.end()) + L"\r\n";
      if (WinHttpSendRequest(request, all.c_str(), DWORD(-1), const_cast<char*>(body.data()), DWORD(body.size()),
                             DWORD(body.size()), 0) &&
          WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &size, WINHTTP_NO_HEADER_INDEX);
        status = int(code);
        char buf[512];
        DWORD got = 0;
        while (WinHttpReadData(request, buf, sizeof(buf), &got) && got && reply.size() < 8192) reply.append(buf, got);
      }
      WinHttpCloseHandle(request);
    }
    WinHttpCloseHandle(connect);
  }
  WinHttpCloseHandle(session);
  return status;
#else
  return HttpPost(url, headers, std::vector<uint8_t>(body.begin(), body.end()), reply, 20000);
#endif
}

// The install's random id, as the pipeline uploads send it (pipeline_upload.cpp InstallId, the same file): the relay's
// rate limits count by it. Never shown in a report (scrubbed).
std::string InstallId(const fs::path& user_dir) {
  const fs::path file = user_dir / "install-id.txt";
  std::string id;
  std::ifstream(file) >> id;
  if (id.size() == 36) return id;
  std::random_device r;
  char buf[40];
  const uint32_t a = r(), b = r(), c = r(), e = r();
  std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%04x%08x", a, b >> 16, (b & 0x0FFF) | 0x4000, (c >> 16 & 0x3FFF) | 0x8000,
                c & 0xFFFF, e);
  id = buf;
  std::ofstream(file, std::ios::trunc) << id << "\n";
  return id;
}

std::vector<std::pair<std::string, std::string>> Headers(const fs::path& user_dir) {
  return {{"Content-Type", "application/json"}, {"X-BF-Install", InstallId(user_dir)}, {"X-BF-Version", BfVersion()}};
}

// One POST of a request; fills the job from the answer. True when it is done with (sent, a duplicate, or refused for
// good); false when it should be kept and tried later.
// The request without its whole log (log_gz) and the issue's mark for its link.
std::string WithoutWholeLog(std::string json) {
  const std::string key = ", \"log_gz\": \"";
  if (const size_t at = json.find(key); at != std::string::npos)
    if (const size_t end = json.find('"', at + key.size()); end != std::string::npos) json.erase(at, end + 1 - at);
  const std::string mark = "<!-- full log -->\\n\\n";
  if (const size_t at = json.find(mark); at != std::string::npos) json.erase(at, mark.size());
  return json;
}

bool PostOnce(const fs::path& user_dir, const std::string& json, Send* job) {
  std::string reply;
  const int status = Post(Endpoint() + "/v1/bf/reports", Headers(user_dir), json, reply);
  const auto kv = ParseFlat(reply);
  REXLOG_INFO("BF report: the relay answered {} ({} bytes)", status, reply.size());
  // Too large with the whole log (a relay from before it, 26 Nov 2026, takes 700 KB): the report again without it.
  if (status == 413)
    if (const std::string without = WithoutWholeLog(json); without.size() < json.size()) {
      REXLOG_INFO("BF report: sent again without the whole log ({} bytes less)", json.size() - without.size());
      return PostOnce(user_dir, without, job);
    }
  if (job) {
    std::lock_guard lock(job->mutex);
    job->status = status;
    job->url = Line(kv, "url");
    job->issue = std::atoi((kv.count("duplicate_of") ? Line(kv, "duplicate_of") : Line(kv, "issue")).c_str());
    job->message = Line(kv, "error");
  }
  if (status == 200 || status == 201) {
    if (job) job->state = kv.count("duplicate_of") ? Send::kDuplicate : Send::kSent;
    return true;
  }
  if (status == 400 || status == 409 || status == 413 || status == 422) {
    if (job) {
      std::lock_guard lock(job->mutex);
      if (job->message.empty())
        job->message = status == 409 ? "This report was already sent today." : status == 413 ? "The report is too large." : "The report service refused it.";
      job->state = Send::kRefused;
    }
    return true;
  }
  return false;  // no answer, 429 (too many for now), 5xx: kept
}
}  // namespace

std::shared_ptr<Send> StartSend(const fs::path& user_dir, const Draft& d, const std::vector<Part>& parts) {
  auto job = std::make_shared<Send>();
  const std::string json = RequestJson(d, parts);
  const bool dry = DryRun();
  std::thread([job, json, dry, user_dir, title = Title(d), body = Body(d, parts)] {
   try {
    const fs::path dir = ReportsDir(user_dir);
    const std::string stamp = Stamp();
    if (dry) {
      // Exactly what would go: the request with its headers, and the issue as GitHub would show it.
      std::string head = "POST " + (Endpoint().empty() ? std::string("<bf_report_endpoint>") : Endpoint()) + "/v1/bf/reports\n";
      for (const auto& [k, v] : Headers(user_dir)) head += k + ": " + (k == "X-BF-Install" ? "<install id>" : v) + "\n";
      WriteFile(dir / ("dry-run-" + stamp + ".json"), json);
      WriteFile(dir / ("dry-run-" + stamp + ".http"), head + "\n" + json);
      WriteFile(dir / ("dry-run-" + stamp + ".md"), "# " + title + "\n\n" + body);
      REXLOG_INFO("BF report: DRY RUN, nothing sent: the request ({} bytes) is in reports\\dry-run-{}.json, the issue in "
                  "dry-run-{}.md",
                  json.size(), stamp, stamp);
      {
        std::lock_guard lock(job->mutex);
        job->message = "reports\\dry-run-" + stamp + ".md";
      }
      job->state = Send::kDryRun;
      return;
    }
    if (!Endpoint().empty() && PostOnce(user_dir, json, job.get())) return;
    // Kept for later.
    const fs::path keep = dir / ("pending-" + stamp + ".json");
    WriteFile(keep, json);
    {
      std::lock_guard lock(job->mutex);
      job->kept = keep;
    }
    REXLOG_INFO("BF report: not sent (status {}): kept as {}", job->status, keep.filename().string());
    job->state = Send::kKept;
   } catch (const std::exception& e) {
    REXLOG_ERROR("BF report: sending failed ({})", e.what());
    {
      std::lock_guard lock(job->mutex);
      job->message = "The report could not be sent or kept on this PC.";
    }
    job->state = Send::kRefused;
   }
  }).detach();
  return job;
}

fs::path DryRunUrl(const fs::path& user_dir, const std::string& url, const std::string& rest) {
  const fs::path f = ReportsDir(user_dir) / ("dry-run-" + Stamp() + ".url");
  WriteFile(f, url + "\n\n--- copied to the clipboard for the Log field (" + std::to_string(rest.size()) + " bytes) ---\n" + rest);
  REXLOG_INFO("BF report: DRY RUN, the browser was not opened: the GitHub link ({} bytes) is in reports\\{}", url.size(),
              f.filename().string());
  return f;
}

int PendingCount(const fs::path& user_dir) {
  int n = 0;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(user_dir / "reports", ec)) {
    const std::string name = e.path().filename().string();
    if (name.rfind("pending-", 0) == 0 && e.path().extension() == ".json") ++n;
  }
  return n;
}

void RetryPending(const fs::path& user_dir) {
  static std::atomic<bool> busy{false};
  if (Endpoint().empty() || DryRun() || PendingCount(user_dir) == 0 || busy.exchange(true)) return;
  std::thread([user_dir] {
   try {
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(user_dir / "reports", ec))
      if (e.path().filename().string().rfind("pending-", 0) == 0 && e.path().extension() == ".json") files.push_back(e.path());
    for (const fs::path& f : files) {
      fs::path tries_file = f;
      tries_file.replace_extension(".tries");
      int tries = std::atoi(ReadFile(tries_file, 16).c_str());
      if (tries >= kMaxTries) continue;
      Send job;
      if (PostOnce(user_dir, ReadFile(f), &job)) {
        REXLOG_INFO("BF report: a kept report was {} ({})", job.state.load() == Send::kRefused ? "refused" : "sent", job.url);
        std::error_code rc;
        fs::remove(f, rc), fs::remove(tries_file, rc);
      } else {
        WriteFile(tries_file, std::to_string(++tries));
      }
    }
   } catch (const std::exception& e) {
    REXLOG_ERROR("BF report: trying the kept reports failed ({})", e.what());
   }
    busy = false;
  }).detach();
}

// ---- screenshots

#if defined(_WIN32)
namespace {
struct FindWindowData {
  DWORD pid;
  HWND best;
  LONG best_area;
};
BOOL CALLBACK FindOwnWindow(HWND h, LPARAM p) {
  auto* d = reinterpret_cast<FindWindowData*>(p);
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (pid != d->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
  RECT r;
  if (!GetClientRect(h, &r)) return TRUE;
  const LONG area = (r.right - r.left) * (r.bottom - r.top);
  if (area > d->best_area) d->best = h, d->best_area = area;
  return TRUE;
}
}  // namespace
#endif

bool TakeScreenshot(const fs::path& user_dir) {
#if defined(_WIN32)
  // The game's window as the test harness and bf_list_shot.cpp copy it (PrintWindow sees this renderer's swap chain).
  FindWindowData fw{GetCurrentProcessId(), nullptr, 0};
  EnumWindows(FindOwnWindow, reinterpret_cast<LPARAM>(&fw));
  if (!fw.best) return false;
  RECT r;
  GetClientRect(fw.best, &r);
  const int w = r.right - r.left, h = r.bottom - r.top;
  if (w <= 0 || h <= 0) return false;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w, bi.bmiHeader.biHeight = -h, bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  std::vector<uint8_t> rgb;
  int ow = w, oh = h;
  if (bmp && bits) {
    HGDIOBJ old = SelectObject(mem, bmp);
    if (PrintWindow(fw.best, mem, 3)) {  // PW_CLIENTONLY | PW_RENDERFULLCONTENT
      // 1280 wide at most (a box filter), BGRA to RGB.
      const int step = std::max(1, (w + 1279) / 1280);
      ow = w / step, oh = h / step;
      rgb.resize(size_t(ow) * oh * 3);
      const auto* src = static_cast<const uint8_t*>(bits);
      for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x) {
          uint32_t sum[3] = {};
          for (int yy = 0; yy < step; ++yy)
            for (int xx = 0; xx < step; ++xx) {
              const uint8_t* p = src + (size_t(y * step + yy) * w + size_t(x * step + xx)) * 4;
              sum[0] += p[2], sum[1] += p[1], sum[2] += p[0];
            }
          uint8_t* o = &rgb[(size_t(y) * ow + x) * 3];
          for (int k = 0; k < 3; ++k) o[k] = uint8_t(sum[k] / uint32_t(step * step));
        }
    }
    SelectObject(mem, old);
  }
  if (bmp) DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  if (rgb.empty()) return false;
  std::string jpg;
  stbi_write_jpg_to_func([](void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<char*>(data), size_t(size)); },
                         &jpg, ow, oh, 3, rgb.data(), 85);
  if (jpg.empty()) return false;
  std::error_code ec;
  const fs::path dir = user_dir / "screenshots";
  fs::create_directories(dir, ec);
  const fs::path file = dir / ("shot-" + Stamp() + ".jpg");
  WriteFile(file, jpg);
  // The 20 newest kept.
  std::vector<std::pair<fs::file_time_type, fs::path>> all;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.path().filename().string().rfind("shot-", 0) == 0) all.push_back({e.last_write_time(ec), e.path()});
  std::sort(all.begin(), all.end());
  for (size_t i = 0; i + 20 < all.size(); ++i) fs::remove(all[i].second, ec);
  REXLOG_INFO("BF report: screenshot {} ({} x {}, {} KB)", file.filename().string(), ow, oh, jpg.size() / 1024);
  return true;
#else
  (void)user_dir;
  return false;
#endif
}

fs::path NewestScreenshot(const fs::path& user_dir, int max_age_hours) {
  const fs::path f = Newest(user_dir / "screenshots", {"shot-"}, ".jpg");
  std::error_code ec;
  if (f.empty() || AgeHours(fs::last_write_time(f, ec)) > max_age_hours) return {};
  return f;
}

void NoteLastMatch(const fs::path& user_dir, const std::string& server, bool official, const std::string& map, int players,
                   int max_players, int ping_ms) {
  std::string s = "server=" + server + "\nofficial=" + (official ? "1" : "0") + "\nmap=" + map + "\nplayers=" + std::to_string(players) +
                  "\nmax=" + std::to_string(max_players) + "\nping=" + std::to_string(ping_ms) + "\n";
  for (char& c : s)
    if (c == '\r') c = ' ';
  WriteFile(ReportsDir(user_dir) / "last_match.txt", s);
}

void KeepPreviousLog(const fs::path& log) {
  std::error_code ec;
  if (log.empty() || !fs::exists(log, ec) || fs::file_size(log, ec) == 0) return;
  fs::rename(log, log.parent_path() / "bf1943.previous.log", ec);
  if (ec) fs::copy_file(log, log.parent_path() / "bf1943.previous.log", fs::copy_options::overwrite_existing, ec);
}

}  // namespace bf1943::report
