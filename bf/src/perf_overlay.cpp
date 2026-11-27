// Performance metrics (Settings > Gameplay > Overlay), King Kong's (kk/src/overlay.cpp, perf_overlay_logic.h and the
// system sampler of benchmark_monitor.cpp, tag v2.1.0): readings in the top left corner while playing.
//   Off       nothing
//   FPS only  one row: the frame rate and the frame time
//   Full      GPU load, temperature and clock; VRAM in use and the card's total; CPU load and the game's share;
//             RAM in use and the game's; the graphics API with the frame rate and frame time; a frame time graph
// F2 switches between them for the session; Settings saves the choice (bf_performance_metrics). Not over our menus.
// Full's readings come from one worker twice a second while Full is shown: Windows' PDH counters (GPU engines and
// GPU memory per process), the installed NVIDIA driver's NVML when there is one (clock, temperature, total memory),
// GetSystemTimes, the process's CPU times and memory. King Kong's GPU frame time came from its own renderer;
// ours has none yet, so that cell shows "-".

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <imgui.h>
#include <rex/logging.h>
#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#endif

REXCVAR_DEFINE_STRING(bf_performance_metrics, "off", "BF1943", "Performance metrics: off, fps, full");

namespace bf1943 {

bool GameMenuOpen();                                   // game_menu.cpp
ImFont* MenuFont(int which);                           // game_menu.cpp: 0 bold, 1 semibold
size_t RecentFrameTimes(float* out, size_t max);       // frame_stats.cpp: oldest first, ms
bool FrameRateNow(double* fps, double* frame_time_ms);  // frame_stats.cpp: the last second

// Where the metrics box ends as last drawn (pixels from the top), 0 when it is not shown: the chat goes below it.
std::atomic<float> g_perf_bottom{0.0f};
float PerfOverlayBottom() { return g_perf_bottom.load(std::memory_order_relaxed); }

namespace {

// ---------------------------------------------------------------- the level (perf_overlay_logic.h)
enum class Level { kOff, kFps, kFull };
Level ParseLevel(std::string_view v) { return v == "full" ? Level::kFull : v == "fps" ? Level::kFps : Level::kOff; }
const char* LevelValue(Level l) { return l == Level::kFull ? "full" : l == Level::kFps ? "fps" : "off"; }
Level NextLevel(Level l) { return l == Level::kOff ? Level::kFps : l == Level::kFps ? Level::kFull : Level::kOff; }
Level CurrentLevel() { return ParseLevel(REXCVAR_GET(bf_performance_metrics)); }

// The frame time graph's top (ms): the first of 35, 50, 100, 200 with the slowest frame in its lower 70%.
float GraphTop(float slowest_ms) {
  for (float top : {35.0f, 50.0f, 100.0f, 200.0f})
    if (slowest_ms <= top * 0.7f) return top;
  return 200.0f;
}
std::string GiB(double mib) {
  if (!(mib >= 0)) return "-";
  char b[16];
  std::snprintf(b, sizeof(b), "%.1f", mib / 1024.0);
  return b;
}
std::string Whole(double v) {
  if (!(v >= 0) || !std::isfinite(v)) return "-";
  char b[16];
  std::snprintf(b, sizeof(b), "%.0f", v);
  return b;
}
// The frame rate's colour band: 0 at or above 55 FPS (green), 1 from 28 (amber), 2 below (red).
int FpsBand(double fps) { return fps >= 55 ? 0 : fps >= 28 ? 1 : 2; }

// ---------------------------------------------------------------- the system readings
struct SystemSample {
  double gpu_load = -1;  // the graphics card's busiest engine, every program (Task Manager's GPU %)
  double cpu_load = -1;  // every logical processor
  double cpu_game = -1;  // the game's process, as a share of all processors
  double ram_game = -1, ram_used = -1;  // MiB
  double vram_game = -1, vram_total = -1;  // MiB
  double gpu_clock = -1, gpu_temperature = -1;
};

#if defined(_WIN32)
uint64_t Ticks(FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

uint32_t InstancePid(std::wstring_view name) {
  if (!name.starts_with(L"pid_")) return 0;
  uint32_t pid = 0;
  for (size_t i = 4; i < name.size() && name[i] >= L'0' && name[i] <= L'9'; ++i) pid = pid * 10 + uint32_t(name[i] - L'0');
  return pid;
}
std::wstring_view InstanceAdapter(std::wstring_view name) {
  const auto at = name.find(L"luid_"), phys = name.find(L"_phys_");
  return at == std::wstring_view::npos || phys == std::wstring_view::npos || phys < at ? std::wstring_view()
                                                                                       : name.substr(at, phys - at);
}
std::wstring_view InstanceEngine(std::wstring_view name) {
  const auto at = name.find(L"luid_");
  return at == std::wstring_view::npos ? std::wstring_view() : name.substr(at);
}

class SystemSampler {
 public:
  SystemSampler() {
    if (PdhOpenQueryW(nullptr, 0, &query_) == ERROR_SUCCESS) {
      PdhAddEnglishCounterW(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engines_);
      PdhAddEnglishCounterW(query_, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &memory_);
      PdhCollectQueryData(query_);
    }
    // The installed NVIDIA driver's library only; nothing is downloaded or required.
    nvml_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (nvml_) {
      auto init = reinterpret_cast<int (*)()>(GetProcAddress(nvml_, "nvmlInit_v2"));
      shutdown_ = reinterpret_cast<int (*)()>(GetProcAddress(nvml_, "nvmlShutdown"));
      auto count = reinterpret_cast<int (*)(unsigned*)>(GetProcAddress(nvml_, "nvmlDeviceGetCount_v2"));
      auto get = reinterpret_cast<int (*)(unsigned, void**)>(GetProcAddress(nvml_, "nvmlDeviceGetHandleByIndex_v2"));
      unsigned n = 0;
      if (!init || !shutdown_ || !count || !get || init() || count(&n) || n != 1 || get(0, &device_)) device_ = nullptr;
      clock_ = reinterpret_cast<int (*)(void*, unsigned, unsigned*)>(GetProcAddress(nvml_, "nvmlDeviceGetClockInfo"));
      temperature_ =
          reinterpret_cast<int (*)(void*, unsigned, unsigned*)>(GetProcAddress(nvml_, "nvmlDeviceGetTemperature"));
      memory_info_ = reinterpret_cast<int (*)(void*, unsigned long long*)>(
          GetProcAddress(nvml_, "nvmlDeviceGetMemoryInfo"));
    }
    process_cpu_ = ProcessCpu();
    last_ = std::chrono::steady_clock::now();
    SystemCpu();  // the first call only starts the window
  }
  ~SystemSampler() {
    if (query_) PdhCloseQuery(query_);
    if (nvml_) {
      if (shutdown_) shutdown_();
      FreeLibrary(nvml_);
    }
  }

  SystemSample Sample() {
    SystemSample out;
    const std::wstring prefix = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    if (query_ && PdhCollectQueryData(query_) == ERROR_SUCCESS) {
      std::vector<unsigned char> buffer;
      if (auto* items = Items(engines_, buffer)) {
        // The whole card's load (benchmark_other_gpu.h AdapterGpuLoad): every program's use of the adapter the
        // game draws on, summed per engine; the busiest engine.
        std::set<std::wstring_view> adapters;
        double busiest = 0;
        std::wstring_view busiest_adapter;
        for (DWORD i = 0; i < count_; ++i) {
          if (!std::wstring_view(items[i].szName).starts_with(prefix)) continue;
          const auto adapter = InstanceAdapter(items[i].szName);
          if (adapter.empty()) continue;
          adapters.insert(adapter);
          if (items[i].FmtValue.doubleValue > busiest) busiest = items[i].FmtValue.doubleValue, busiest_adapter = adapter;
        }
        if (busiest > 0) adapters = {busiest_adapter};
        std::map<std::wstring_view, double> engines;
        for (DWORD i = 0; i < count_; ++i) {
          const auto engine = InstanceEngine(items[i].szName);
          if (!InstancePid(items[i].szName) || engine.empty() || !adapters.count(InstanceAdapter(items[i].szName))) continue;
          engines[engine] += std::clamp(items[i].FmtValue.doubleValue, 0.0, 100.0);
        }
        if (!adapters.empty()) {
          out.gpu_load = 0;
          for (const auto& [k, v] : engines) out.gpu_load = std::max(out.gpu_load, std::min(100.0, v));
        }
      }
      std::vector<unsigned char> memory;
      if (auto* items = Items(memory_, memory)) {
        double bytes = 0;
        bool found = false;
        for (DWORD i = 0; i < count_; ++i)
          if (std::wstring_view(items[i].szName).starts_with(prefix)) bytes += items[i].FmtValue.doubleValue, found = true;
        if (found) out.vram_game = bytes / 1048576.0;
      }
    }
    out.cpu_load = SystemCpu();
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - last_).count();
    const uint64_t pc = ProcessCpu();
    if (seconds > 0 && pc >= process_cpu_) out.cpu_game = std::min(100.0, (pc - process_cpu_) / (seconds * 100000.0 * cpus_));
    process_cpu_ = pc;
    last_ = now;
    PROCESS_MEMORY_COUNTERS_EX m{};
    m.cb = sizeof(m);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof(m)))
      out.ram_game = double(m.WorkingSetSize) / 1048576;
    MEMORYSTATUSEX system{sizeof(system)};
    if (GlobalMemoryStatusEx(&system)) out.ram_used = double(system.ullTotalPhys - system.ullAvailPhys) / 1048576;
    if (device_) {
      unsigned value = 0;
      if (clock_ && !clock_(device_, 0, &value)) out.gpu_clock = value;
      if (temperature_ && !temperature_(device_, 0, &value)) out.gpu_temperature = value;
      unsigned long long info[3] = {};  // nvmlMemory_t: total, free, used
      if (memory_info_ && !memory_info_(device_, info)) out.vram_total = double(info[0]) / 1048576.0;
    }
    return out;
  }

 private:
  PDH_FMT_COUNTERVALUE_ITEM_W* Items(PDH_HCOUNTER counter, std::vector<unsigned char>& buffer) {
    count_ = 0;
    if (!counter) return nullptr;
    DWORD bytes = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != PDH_MORE_DATA) return nullptr;
    buffer.resize(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return nullptr;
    count_ = count;
    return items;
  }
  static uint64_t ProcessCpu() {
    FILETIME c{}, e{}, k{}, u{};
    return GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ? Ticks(k) + Ticks(u) : 0;
  }
  double SystemCpu() {
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) return -1;
    const uint64_t i = Ticks(idle), k = Ticks(kernel), u = Ticks(user);
    double out = -1;
    if (sys_total_ && k + u > sys_total_) {
      const double total = double(k + u - sys_total_), busy = total - double(i - sys_idle_);
      out = std::clamp(busy / total * 100, 0.0, 100.0);
    }
    sys_idle_ = i, sys_total_ = k + u;
    return out;
  }

  PDH_HQUERY query_ = nullptr;
  PDH_HCOUNTER engines_ = nullptr, memory_ = nullptr;
  DWORD count_ = 0;
  HMODULE nvml_ = nullptr;
  void* device_ = nullptr;
  int (*shutdown_)() = nullptr;
  int (*clock_)(void*, unsigned, unsigned*) = nullptr;
  int (*temperature_)(void*, unsigned, unsigned*) = nullptr;
  int (*memory_info_)(void*, unsigned long long*) = nullptr;
  uint64_t process_cpu_ = 0, sys_idle_ = 0, sys_total_ = 0;
  std::chrono::steady_clock::time_point last_;
  double cpus_ = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
};
#else
class SystemSampler {  // Linux: to follow (King Kong reads /proc and the amdgpu sysfs files)
 public:
  SystemSample Sample() { return {}; }
};
#endif

// The renderer drawing the game: our Vulkan plugin when it is loaded (the setting may be empty when Settings >
// Graphics chose it), else the SDK's Direct3D 12 one.
bool UsingVulkan() {
#if defined(_WIN32)
  static const bool vulkan = GetModuleHandleA("rexgpu-bfnative.dll") != nullptr;
  return vulkan;
#else
  return true;
#endif
}

// One worker reads the system twice a second while Full is shown, and stops when it is not.
class MetricsSampler {
 public:
  ~MetricsSampler() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
  }
  void Want(bool on) {
    if (!on || running_.load()) return;
    if (worker_.joinable()) worker_.join();
    running_ = true;
    worker_ = std::thread([this] { Run(); });
  }
  SystemSample Get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return view_;
  }

 private:
  void Run() {
    SystemSampler sampler;
    while (!stop_ && CurrentLevel() == Level::kFull) {
      const SystemSample v = sampler.Sample();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        view_ = v;
      }
#if defined(BF_DEV_TOOLS)
      // BF_DEV_GPU_LOG=1 (tests): the card's load and clock, averaged over 5 s, in the log (with the frame cap, the
      // GPU's busy time a frame: what a graphics setting costs as a whole).
      {
        static const bool log = std::getenv("BF_DEV_GPU_LOG") != nullptr;
        static double load = 0, clock = 0;
        static int n = 0;
        if (log && v.gpu_load >= 0) {
          load += v.gpu_load, clock += std::max(0.0, v.gpu_clock), ++n;
          if (n >= 10) {
            REXLOG_INFO("BF dev GPU: {:.1f}% busy, {:.0f} MHz over 5 s", load / n, clock / n);
            load = clock = 0, n = 0;
          }
        }
      }
#endif
      for (int i = 0; i < 10 && !stop_ && CurrentLevel() == Level::kFull; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    running_ = false;
  }
  std::atomic<bool> running_{false}, stop_{false};
  std::thread worker_;
  mutable std::mutex mutex_;
  SystemSample view_;
};

// King Kong's colours: each row's label has its own; values white with their units dimmed; a dark see-through box.
constexpr ImU32 kBox = IM_COL32(8, 10, 16, 150);
constexpr ImU32 kValue = IM_COL32(236, 238, 242, 255);
constexpr ImU32 kUnit = IM_COL32(160, 166, 176, 255);
constexpr ImU32 kGpu = IM_COL32(88, 200, 120, 255);
constexpr ImU32 kVram = IM_COL32(190, 130, 230, 255);
constexpr ImU32 kCpu = IM_COL32(80, 160, 235, 255);
constexpr ImU32 kRam = IM_COL32(235, 125, 170, 255);
constexpr ImU32 kApi = IM_COL32(240, 120, 85, 255);
constexpr ImU32 kFps[3] = {IM_COL32(139, 213, 80, 255), IM_COL32(240, 200, 90, 255), IM_COL32(240, 110, 90, 255)};
constexpr ImU32 kGraph = IM_COL32(139, 213, 80, 255);
constexpr ImU32 kGraphBack = IM_COL32(0, 0, 0, 70);
constexpr ImU32 kGuide = IM_COL32(255, 255, 255, 40);

class PerfOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit PerfOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const Level level = CurrentLevel();
    sampler_.Want(level == Level::kFull);
    g_perf_bottom.store(0.0f, std::memory_order_relaxed);
    if (level == Level::kOff || GameMenuOpen()) return;  // not over our menus
    double fps = 0, frame_ms = 0;
    if (!FrameRateNow(&fps, &frame_ms)) return;  // nothing to show yet
    const float s = io.DisplaySize.y / 1080.0f * 1.25f;
    ImFont* bold = MenuFont(0) ? MenuFont(0) : ImGui::GetFont();
    ImFont* font = MenuFont(1) ? MenuFont(1) : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float size = 15.0f * s, unit = 12.0f * s, line = 19.0f * s, pad = 8.0f * s, gap = 6.0f * s;
    const ImVec2 origin(12 * s, 12 * s);
    const ImU32 fps_color = kFps[FpsBand(fps)];
    char fps_text[16], ms_text[16];
    std::snprintf(fps_text, sizeof(fps_text), "%.0f", fps);
    std::snprintf(ms_text, sizeof(ms_text), "%.1f", frame_ms);
    auto text_w = [&](ImFont* f, float sz, const char* t) { return f->CalcTextSizeA(sz, FLT_MAX, 0, t).x; };
    struct Cell {
      std::string value;
      const char* unit;
    };
    const float col_w = 66 * s;
    auto row = [&](float x0, float y, float label_w, const char* label, ImU32 color, std::initializer_list<Cell> cells) {
      dl->AddText(bold, size, ImVec2(x0, y), color, label);
      float x = x0 + label_w;
      for (const Cell& c : cells) {
        const float uw = c.unit && *c.unit ? text_w(font, unit, c.unit) + 3 * s : 0;
        const float vw = text_w(font, size, c.value.c_str());
        const float right = x + col_w;
        dl->AddText(font, size, ImVec2(right - uw - vw, y), kValue, c.value.c_str());
        if (uw > 0) dl->AddText(font, unit, ImVec2(right - uw + 3 * s, y + (size - unit) * 0.8f), kUnit, c.unit);
        x = right + gap;
      }
      return x;
    };

    if (level == Level::kFps) {
      const float label_w = text_w(bold, size, "FPS") + 4 * s;
      const float w = label_w + col_w * 2 + gap;
      dl->AddRectFilled(origin, ImVec2(origin.x + w + pad * 2, origin.y + line + pad * 2 - 4 * s), kBox, 4 * s);
      row(origin.x + pad, origin.y + pad - 2 * s, label_w, "FPS", fps_color, {{fps_text, ""}, {ms_text, "ms"}});
      g_perf_bottom.store(origin.y + line + pad * 2 - 4 * s, std::memory_order_relaxed);
      return;
    }
    const SystemSample m = sampler_.Get();
    const float label_w = 58 * s, graph_h = 38 * s;
    const float w = label_w + col_w * 3 + gap * 2;
    const char* api = UsingVulkan() ? "VULKAN" : "D3D12";
    const float h = line * 5 + 4 * s + graph_h;
    dl->AddRectFilled(origin, ImVec2(origin.x + w + pad * 2, origin.y + h + pad * 2), kBox, 4 * s);
    g_perf_bottom.store(origin.y + h + pad * 2, std::memory_order_relaxed);
    const float x0 = origin.x + pad;
    float y = origin.y + pad - 2 * s;
    auto note = [&](float x, float y_row, const char* t) {
      dl->AddText(font, unit, ImVec2(x, y_row + (size - unit) * 0.8f), kUnit, t);
    };
    char clock[16] = "-";
    if (m.gpu_clock >= 0) std::snprintf(clock, sizeof(clock), "%.0f", m.gpu_clock);
    if (m.gpu_temperature >= 0 || m.gpu_clock >= 0)
      row(x0, y, label_w, "GPU", kGpu, {{Whole(m.gpu_load), "%"}, {Whole(m.gpu_temperature), "\xC2\xB0" "C"}, {clock, "MHz"}});
    else
      row(x0, y, label_w, "GPU", kGpu, {{Whole(m.gpu_load), "%"}});
    y += line;
    row(x0, y, label_w, "VRAM", kVram, {{GiB(m.vram_game), "GiB"}, {"/ " + GiB(m.vram_total), ""}, {"-", "ms"}});
    y += line;
    note(row(x0, y, label_w, "CPU", kCpu, {{Whole(m.cpu_load), "%"}, {Whole(m.cpu_game), "%"}}), y, "game");
    y += line;
    note(row(x0, y, label_w, "RAM", kRam, {{GiB(m.ram_used), "GiB"}, {GiB(m.ram_game), "GiB"}}), y, "game");
    y += line;
    row(x0, y, label_w, api, kApi, {{fps_text, "FPS"}, {ms_text, "ms"}});
    y += line + 4 * s;
    constexpr size_t kShown = 200;
    float times[kShown];
    const size_t n = RecentFrameTimes(times, kShown);
    const ImVec2 g0(x0, y), g1(x0 + w, y + graph_h);
    dl->AddRectFilled(g0, g1, kGraphBack, 2 * s);
    if (n >= 2) {
      float slowest = 0;
      for (size_t i = 0; i < n; ++i) slowest = std::max(slowest, times[i]);
      const float top = GraphTop(slowest);
      char label[16];
      std::snprintf(label, sizeof(label), "%.0f ms", top);
      dl->AddText(font, unit, ImVec2(g0.x + 3 * s, g0.y + 1 * s), kUnit, label);
      const float guide = g1.y - std::min(float(frame_ms) / top, 1.0f) * (graph_h - 2 * s);
      dl->AddLine(ImVec2(g0.x, guide), ImVec2(g1.x, guide), kGuide, 1.0f);
      ImVec2 points[kShown];
      const float step = w / float(kShown - 1);
      for (size_t i = 0; i < n; ++i)
        points[i] = ImVec2(g1.x - step * float(n - 1 - i), g1.y - std::min(times[i] / top, 1.0f) * (graph_h - 2 * s));
      dl->AddPolyline(points, int(n), kGraph, ImDrawFlags_None, 1.5f * s);
    }
  }

 private:
  MetricsSampler sampler_;
};

}  // namespace

void CreatePerfOverlay(rex::ui::ImGuiDrawer* drawer) {
  new PerfOverlay(drawer);  // lives for the rest of the session
  // F2: Performance metrics Off, FPS only, Full (for this session; Settings saves the choice).
  rex::ui::RegisterBind("bind_bf_metrics", "F2", "Switch performance metrics", [] {
    rex::cvar::SetFlagByName("bf_performance_metrics", LevelValue(NextLevel(CurrentLevel())));
  });
}

}  // namespace bf1943
