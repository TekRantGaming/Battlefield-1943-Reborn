// A frame limit outside the game (TekRantGaming, 9 Nov 2026: his NVIDIA "Max Frame Rate" of 110 on a 120 Hz VRR screen).
// The game updates its world 30 times a second, so frame rates that divide into 30 keep motion even (at 110 the
// updates fell on 3 or 4 frames in turn, a visible hitch in a vehicle); the frame rate setting offers only those, up
// to the screen's rate and up to a limit set outside the game, which is found two ways:
//   - NVIDIA's driver setting itself (NVAPI's driver settings, read only, never written): the game's own profile's
//     Max Frame Rate (bf1943.exe), else the global profile's. Read once on its own thread at the start.
//   - measured (frame_rate.cpp, NoteFrameInterval): frames our own limit did not hold, coming at one steady interval
//     that is a whole frame rate below ours, in two windows of 600 frames running; any limiter, the session only.
// NVAPI is used through nvapi_QueryInterface with the interface numbers of NVIDIA's public nvapi_interface.h (no SDK
// needed); a driver without it, or another vendor's, gives no limit.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <rex/logging.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace bf1943 {
namespace {

std::atomic<int> g_driver_cap{-1};   // -1: not read yet; 0: none
std::atomic<int> g_measured_cap{0};  // 0: none seen

#if defined(_WIN32)
int ReadNvidiaCap() {
  HMODULE nvapi = LoadLibraryA("nvapi64.dll");
  if (!nvapi) return 0;
  using QueryInterface = void*(__cdecl*)(uint32_t);
  auto query = reinterpret_cast<QueryInterface>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
  if (!query) return 0;
  using Initialize = int(__cdecl*)();
  using CreateSession = int(__cdecl*)(void**);
  using LoadSettings = int(__cdecl*)(void*);
  using GetProfile = int(__cdecl*)(void*, void**);
  using FindApplication = int(__cdecl*)(void*, const wchar_t*, void**, void*);
  using GetSetting = int(__cdecl*)(void*, void*, uint32_t, void*);
  using DestroySession = int(__cdecl*)(void*);
  auto initialize = reinterpret_cast<Initialize>(query(0x0150E828));
  auto create = reinterpret_cast<CreateSession>(query(0x0694D52E));
  auto load = reinterpret_cast<LoadSettings>(query(0x375DBD6B));
  auto global = reinterpret_cast<GetProfile>(query(0x617BFF9F));
  auto find = reinterpret_cast<FindApplication>(query(0xEEE566B2));
  auto get = reinterpret_cast<GetSetting>(query(0x73BF8338));
  auto destroy = reinterpret_cast<DestroySession>(query(0xDAD9CFF8));
  if (!initialize || !create || !load || !global || !get || !destroy || initialize() != 0) return 0;
  void* session = nullptr;
  if (create(&session) != 0 || !session) return 0;
  int cap = 0;
  if (load(session) == 0) {
    constexpr uint32_t kMaxFrameRate = 0x10835002;  // FRL_FPS
    // NVDRS_SETTING (version 1): version, name (2048 UTF-16), id, type, location, current predefined, predefined
    // valid, the predefined value (a 4100 byte union), the current value (the same).
    constexpr size_t kSettingSize = 4 + 4096 + 20 + 4100 + 4100;
    std::vector<uint8_t> setting(kSettingSize);
    auto read = [&](void* profile) -> int {
      std::fill(setting.begin(), setting.end(), uint8_t(0));
      const uint32_t version = uint32_t(kSettingSize) | (1u << 16);
      std::memcpy(setting.data(), &version, 4);
      if (get(session, profile, kMaxFrameRate, setting.data()) != 0) return -1;  // not set in this profile
      uint32_t value = 0;
      std::memcpy(&value, setting.data() + 4 + 4096 + 20 + 4100, 4);
      return int(value);
    };
    // The game's own profile first (NVDRS_APPLICATION version 1: version, predefined, app name, friendly name,
    // launcher, each name 2048 UTF-16).
    constexpr size_t kApplicationSize = 4 + 4 + 3 * 4096;
    std::vector<uint8_t> application(kApplicationSize);
    const uint32_t app_version = uint32_t(kApplicationSize) | (1u << 16);
    std::memcpy(application.data(), &app_version, 4);
    void* app_profile = nullptr;
    int app_cap = -1;
    if (find && find(session, L"bf1943.exe", &app_profile, application.data()) == 0 && app_profile)
      app_cap = read(app_profile);
    void* global_profile = nullptr;
    const int global_cap = global(session, &global_profile) == 0 && global_profile ? read(global_profile) : -1;
    cap = app_cap >= 0 ? app_cap : std::max(global_cap, 0);
    REXLOG_INFO("BF frame cap: NVIDIA Max Frame Rate {} (the game's profile {}, global {})",
                cap ? std::to_string(cap) : std::string("off"), app_cap >= 0 ? std::to_string(app_cap) : "not set",
                global_cap >= 0 ? std::to_string(global_cap) : "not set");
  }
  destroy(session);
  return cap;
}
#endif

}  // namespace

// Starts reading the driver's limit (once).
void StartFrameCapRead() {
  static std::atomic<bool> started{false};
  if (started.exchange(true)) return;
#if defined(_WIN32)
  std::thread([] { g_driver_cap = ReadNvidiaCap(); }).detach();
#else
  g_driver_cap = 0;
#endif
}

// The limit set outside the game (0: none known): the driver's, else the measured one.
int SystemFrameCap() {
  const int driver = g_driver_cap.load(std::memory_order_relaxed);
  if (driver > 0) return driver;
  return g_measured_cap.load(std::memory_order_relaxed);
}

// frame_rate.cpp, every game frame: its interval, whether our own limit held the frame before it, and our limit
// (0: none). A steady interval our limit did not make, a whole frame rate below ours, is a limit outside the game.
void NoteFrameInterval(double ms, bool held, double our_fps) {
  static std::vector<float> unheld;
  static uint32_t frames = 0;
  static int candidate = 0, streak = 0;
  ++frames;
  if (!held && ms > 0.5 && ms < 100.0) unheld.push_back(float(ms));
  if (frames < 600) return;
  bool steady = false;
  int fps = 0;
  if (unheld.size() >= 540) {
    std::sort(unheld.begin(), unheld.end());
    const float p10 = unheld[unheld.size() / 10], p50 = unheld[unheld.size() / 2], p90 = unheld[unheld.size() * 9 / 10];
    const double rate = 1000.0 / p50;
    fps = int(std::lround(rate));
    steady = p90 - p10 <= 0.03f * p50 && std::abs(rate - fps) < 0.3 && fps >= 30 && (our_fps <= 0 || fps < our_fps - 2);
  }
  if (steady && fps == candidate) {
    ++streak;
  } else {
    candidate = steady ? fps : 0;
    streak = steady ? 1 : 0;
  }
  if (streak >= 2 && g_measured_cap.load() != candidate) {
    g_measured_cap = candidate;
    REXLOG_INFO("BF frame cap: frames come at a steady {} a second that our limit ({}) did not make: a limit outside "
                "the game",
                candidate, our_fps > 0 ? std::to_string(int(our_fps)) : std::string("none"));
  }
  unheld.clear();
  frames = 0;
}

}  // namespace bf1943
