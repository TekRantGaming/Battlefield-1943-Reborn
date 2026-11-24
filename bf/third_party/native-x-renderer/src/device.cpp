// NativeXRenderer - see device.h.

#include "device.h"
#include "xess.h"

#include "queries.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <chrono>
#include <mutex>
#include <vector>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include "EngineFactoryVk.h"
#include "CommandQueue.h"
#include "Fence.h"
#include "NativeWindow.h"
// DiligentCore's Vulkan interfaces take Vulkan's types from the includer (its functions are loaded by volk).
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>
#include "../thirdparty/diligent-core/ThirdParty/volk/volk.h"  // vkCreateSwapchainKHR's pointer (NXR_TEST_SWAPCHAIN_FAIL)
#include "SwapChainVk.h"
#include "RenderDeviceVk.h"
#include "DeviceContextVk.h"
#include "ThreadPool.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

// D172: DiligentCore's dynamic heap, host-visible system memory held for the whole run (resident: it is mapped and
// written every frame). Battlefield 1943 Reborn in play needs under 16 MB of it; a level's load between 32 and 64 MB.
REXCVAR_DEFINE_INT32(native_x_renderer_dynamic_heap_mb, 256, "NativeXRenderer",
                     "Size of DiligentCore's dynamic heap in MB (D172): system memory for the draws' constants and "
                     "uploads, held for the whole run; when it is short, DiligentCore waits for the GPU");
REXCVAR_DEFINE_INT32(native_x_renderer_compile_threads, 0, "NativeXRenderer",
                     "Threads compiling pipelines (D135): 0 = the hardware's threads less two, from 4 to 16, below "
                     "normal priority so the game's own threads come first");

namespace native_x_renderer {
namespace {
// D135: the compilation threads, at normal priority while the stored pipelines are made at start (the game waits on
// them behind its setup screen: below normal there, a warm start took 3 to 5 s against 1.7 s), below normal after.
std::mutex g_compile_threads_mutex;
std::vector<uint64_t> g_compile_threads;  // HANDLE on Windows, the thread id on Linux
bool g_compile_threads_low = false;

void ApplyCompilePriority(uint64_t thread, bool low) {
#ifdef _WIN32
  SetThreadPriority(reinterpret_cast<HANDLE>(thread), low ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
#elif defined(__linux__)
  setpriority(PRIO_PROCESS, id_t(thread), low ? 5 : 0);  // a thread's own nice value on Linux
#else
  (void)thread, (void)low;
#endif
}
}  // namespace

void RegisterCompileThread() {
  uint64_t thread = 0;
#ifdef _WIN32
  HANDLE handle = nullptr;
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle, 0, FALSE,
                  DUPLICATE_SAME_ACCESS);
  thread = reinterpret_cast<uint64_t>(handle);
#elif defined(__linux__)
  thread = uint64_t(syscall(SYS_gettid));
#endif
  std::lock_guard<std::mutex> lock(g_compile_threads_mutex);
  g_compile_threads.push_back(thread);
  ApplyCompilePriority(thread, g_compile_threads_low);
}

void SetCompileThreadsLow(bool low) {
  std::lock_guard<std::mutex> lock(g_compile_threads_mutex);
  if (g_compile_threads_low == low) return;
  g_compile_threads_low = low;
  for (uint64_t thread : g_compile_threads) ApplyCompilePriority(thread, low);
}
}  // namespace native_x_renderer

REXCVAR_DEFINE_STRING(native_x_renderer_present_mode, "", "NativeXRenderer",
                      "How frames reach the screen (D106, D107): empty = MAILBOX (no tearing, no wait for the "
                      "display), else IMMEDIATE (tearing); \"vsync\" = FIFO, each frame shown once, never torn; "
                      "\"mailbox\" = MAILBOX, else FIFO; \"immediate\" = IMMEDIATE (tearing, or a variable refresh "
                      "rate display following the game), else MAILBOX. FIFO where the driver offers none of them");

// DiligentCore's (diligent_patches/0011, GraphicsEngineVulkan/include/SwapChainVkImpl.hpp, not on the include path):
// the V-Sync state and present modes the next swap chains start with.
namespace Diligent {
void SetSwapChainVkInitialPresentation(bool VSync, const VkPresentModeKHR* pModes, Uint32 NumModes);
}  // namespace Diligent

// DiligentCore's (GraphicsEngineVulkan/include/VulkanUtilities/Debug.hpp, not on the include path): loads the
// VK_EXT_debug_utils functions it names objects with.
namespace VulkanUtilities {
bool SetupDebugUtils(VkInstance instance, VkDebugUtilsMessageSeverityFlagsEXT message_severity,
                     VkDebugUtilsMessageTypeFlagsEXT message_type, uint32_t ignore_message_count,
                     const char* const* ignore_message_names, void* user_data);
}  // namespace VulkanUtilities

namespace native_x_renderer {

using namespace Diligent;

namespace {

// Issue 26: DiligentCore reported that the swap chain could not be made again (OnDiligentMessage).
std::atomic<bool> g_swap_chain_failed{false};

// NXR_TEST_SWAPCHAIN_FAIL=<n> (tests of issue 26): the next n remakes of the swap chain (vkCreateSwapchainKHR with
// oldSwapchain) fail as on the player's PC: the new swap chain is made, which retires the old one, then destroyed, and
// VK_ERROR_INITIALIZATION_FAILED returned. Through volk's pointer, which DiligentCore calls.
// NXR_TEST_SWAPCHAIN_FAIL=after-first: every swap chain after the first fails, remade or new (the player's PC).
PFN_vkCreateSwapchainKHR g_real_create_swapchain = nullptr;
std::atomic<int> g_swapchain_failures_left{0};
std::atomic<int> g_swapchains_made{0};
bool g_fail_after_first = false;
VKAPI_ATTR VkResult VKAPI_CALL FailingCreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info,
                                                      const VkAllocationCallbacks* allocator, VkSwapchainKHR* out) {
  if (g_fail_after_first && g_swapchains_made.fetch_add(1) > 0) {
    if (info && info->oldSwapchain != VK_NULL_HANDLE) {
      VkSwapchainKHR made = VK_NULL_HANDLE;
      if (g_real_create_swapchain(device, info, allocator, &made) == VK_SUCCESS) vkDestroySwapchainKHR(device, made, allocator);
    }
    REXLOG_WARN("[native_x_renderer] NXR_TEST_SWAPCHAIN_FAIL=after-first: this swap chain fails");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (!g_fail_after_first && info && info->oldSwapchain != VK_NULL_HANDLE && g_swapchain_failures_left.fetch_sub(1) > 0) {
    VkSwapchainKHR made = VK_NULL_HANDLE;
    if (g_real_create_swapchain(device, info, allocator, &made) == VK_SUCCESS) vkDestroySwapchainKHR(device, made, allocator);
    REXLOG_WARN("[native_x_renderer] NXR_TEST_SWAPCHAIN_FAIL: this remake of the swap chain fails");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  return g_real_create_swapchain(device, info, allocator, out);
}
void InstallSwapChainFailureTest() {
  const char* v = std::getenv("NXR_TEST_SWAPCHAIN_FAIL");
  g_fail_after_first = v && std::strcmp(v, "after-first") == 0;
  const int n = g_fail_after_first ? 1 : v ? std::atoi(v) : 0;
  if (n <= 0 || g_real_create_swapchain || !vkCreateSwapchainKHR) return;
  g_swapchain_failures_left = n;
  g_real_create_swapchain = vkCreateSwapchainKHR;
  vkCreateSwapchainKHR = FailingCreateSwapchain;
  REXLOG_WARN("[native_x_renderer] NXR_TEST_SWAPCHAIN_FAIL: the next {} remakes of the swap chain fail", n);
}

// DiligentCore's messages, in the game's log.
void DILIGENT_CALL_TYPE OnDiligentMessage(DEBUG_MESSAGE_SEVERITY severity, const Char* message, const Char* function,
                                          const Char* file, int line) {
  (void)file, (void)line;
  const char* text = message ? message : "";
  const char* where = function ? function : "";
  switch (severity) {
    case DEBUG_MESSAGE_SEVERITY_INFO:
      REXLOG_INFO("[native_x_renderer] DiligentCore: {}", text);
      break;
    case DEBUG_MESSAGE_SEVERITY_WARNING:
      REXLOG_WARN("[native_x_renderer] DiligentCore: {} ({})", text, where);
      break;
    default:
      REXLOG_ERROR("[native_x_renderer] DiligentCore: {} ({})", text, where);
      // Issue 26: the swap chain could not be made again. DiligentCore throws then, or, in Resize, catches it itself
      // and leaves the swap chain without images: the next use makes a new one first (Device::RebuildSwapChain).
      if (std::strstr(where, "SwapChainVkImpl")) g_swap_chain_failed.store(true);
      break;
  }
}

}  // namespace

namespace {
void InstallRaceCheck(Recorder* recorder, VkDevice device);  // D229 (below)
}  // namespace

bool Device::Create(void* native_window, bool half_precision) {
  window_ = native_window;
  IEngineFactoryVk* factory = GetEngineFactoryVk();
  if (!factory) {
    REXLOG_ERROR("[native_x_renderer] no DiligentCore Vulkan factory");
    return false;
  }
  factory->SetMessageCallback(OnDiligentMessage);

  EngineVkCreateInfo create_info;
  create_info.EnableValidation = false;
  // D161: the GPU the SDK's provider uses, its vulkan_device: the player's, or the high-performance GPU the game
  // points it at before the provider is made (Battlefield 1943's gpu_preference.cpp, King Kong's choice). An index
  // in vkEnumeratePhysicalDevices' order, which is DiligentCore's too. -1 (automatic, a single GPU or the first one
  // already the best): DiligentCore's own choice, discrete before integrated.
  if (const int32_t vulkan_device = rex::cvar::Query<int32_t>("vulkan_device"); vulkan_device >= 0) {
    create_info.AdapterId = uint32_t(vulkan_device);
    REXLOG_INFO("[native_x_renderer] Vulkan device {} (vulkan_device, D161)", vulkan_device);
  }
  // NXR_DEBUG_NAMES=1 (a bring-up aid): VK_EXT_debug_utils without DiligentCore's validation (which keeps
  // Battlefield 1943 on its start screen), so the Vulkan objects carry their names (shaders: their guest hash) in a
  // RenderDoc capture.
  static const char* const debug_names_extension[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
  const char* debug_names = std::getenv("NXR_DEBUG_NAMES");
  const bool name_objects = debug_names && *debug_names == '1';
  if (name_objects) {
    create_info.InstanceExtensionCount = 1;
    create_info.ppInstanceExtensionNames = debug_names_extension;
  }
  // NXR_DEVICE_FAULT=1 (a diagnosis aid, D229: the AMD driver lost the device as the storm's clouds first drew, runs
  // fstorm3 to fstorm6): VK_EXT_device_fault, so a lost device's report says why (a page fault and where, or a hang).
  static const char* const fault_extension[] = {VK_EXT_DEVICE_FAULT_EXTENSION_NAME};
  static VkPhysicalDeviceFaultFeaturesEXT fault_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
  {
    const char* fault = std::getenv("NXR_DEVICE_FAULT");
    fault_report_ = fault && *fault == '1';
  }
  // With it, VK_EXT_device_address_binding_report: every memory bind and unbind with its object's name, kept, so the
  // fault's address can be named (a resource there, or one freed from there).
  static const char* const fault_extensions[] = {fault_extension[0], VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME,
                                                 VK_AMD_BUFFER_MARKER_EXTENSION_NAME};
  static VkPhysicalDeviceAddressBindingReportFeaturesEXT binding_features{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT};
  if (fault_report_) {
    fault_features.deviceFault = VK_TRUE;
    binding_features.reportAddressBinding = VK_TRUE;
    fault_features.pNext = &binding_features;
    create_info.DeviceExtensionCount = 3;
    create_info.ppDeviceExtensionNames = fault_extensions;
    create_info.pDeviceExtensionFeatures = &fault_features;
    create_info.InstanceExtensionCount = 1;  // VK_EXT_debug_utils, for the reports and the objects' names
    create_info.ppInstanceExtensionNames = debug_names_extension;
    REXLOG_INFO("[native_x_renderer] VK_EXT_device_fault and the address binding report asked for (NXR_DEVICE_FAULT)");
  }
  // What guest draws need: independent blending per render target, depth clamp (PA_CL_CLIP_CNTL
  // clip_disable), wireframe (polygon modes), BC textures. Optional ones are used when present.
  create_info.Features.IndependentBlend = DEVICE_FEATURE_STATE_ENABLED;
  create_info.Features.DepthClamp = DEVICE_FEATURE_STATE_OPTIONAL;
  create_info.Features.DepthBiasClamp = DEVICE_FEATURE_STATE_OPTIONAL;
  create_info.Features.WireframeFill = DEVICE_FEATURE_STATE_OPTIONAL;
  create_info.Features.TextureCompressionBC = DEVICE_FEATURE_STATE_OPTIONAL;
  create_info.Features.OcclusionQueries = DEVICE_FEATURE_STATE_OPTIONAL;
  create_info.QueryPoolSizes[QUERY_TYPE_OCCLUSION] = OcclusionQueries::kPoolSize;  // the guest's (D62)
  create_info.Features.DurationQueries = DEVICE_FEATURE_STATE_OPTIONAL;  // GPU time per frame, for the log
  // The duration queries' pool: the frame timer's 32, and with native_x_renderer_gpu_sections the passes' timers
  // (4 a section) and the night pass's; DiligentCore's 256 left too little room (a failed allocation crashed).
  create_info.QueryPoolSizes[QUERY_TYPE_DURATION] = 512;
  // D174: the guest memory mirror as a sparse buffer, its binds signalling a timeline semaphore.
  create_info.Features.SparseResources = DEVICE_FEATURE_STATE_OPTIONAL;
  create_info.Features.NativeFence = DEVICE_FEATURE_STATE_OPTIONAL;
  // Pipelines compile in DiligentCore's thread pool (D28). A few threads: new pipelines come in bursts
  // (a level, a new effect), and the game's own threads need the CPU.
  create_info.Features.AsyncShaderCompilation = DEVICE_FEATURE_STATE_OPTIONAL;
  if (half_precision) create_info.Features.ShaderFloat16 = DEVICE_FEATURE_STATE_OPTIONAL;
  // D135 (was D128's six on 16 threads): all but two hardware threads, below normal priority. A first start compiles
  // every stored pipeline behind the game's setup screen (Battlefield 1943: 1,688, 49 s on six threads with the
  // driver's cache cold); in play the game's threads come first, and a burst of new pipelines still has the idle cores.
  const int32_t wanted_threads = REXCVAR_GET(native_x_renderer_compile_threads);
  const uint32_t compile_threads =
      wanted_threads > 0 ? uint32_t(wanted_threads)
                         : std::clamp(std::thread::hardware_concurrency() > 2 ? std::thread::hardware_concurrency() - 2
                                                                              : 4u,
                                      4u, 16u);
  ThreadPoolCreateInfo pool_info;
  pool_info.NumThreads = compile_threads;
  pool_info.OnThreadStarted = [](Uint32) { RegisterCompileThread(); };
  RefCntAutoPtr<IThreadPool> compile_pool = CreateThreadPool(pool_info);
  create_info.pAsyncShaderCompilationThreadPool = compile_pool;
  create_info.NumAsyncShaderCompilationThreads = compile_threads;
  REXLOG_INFO("[native_x_renderer] pipelines compile on {} threads (D135)", compile_threads);
  // Every draw maps its constant buffers from the dynamic heap (system, bool/loop and fetch constants:
  // 1.4 KB, plus the float constants its shaders use), with several frames in flight.
  create_info.DynamicHeapSize = uint32_t(std::clamp(REXCVAR_GET(native_x_renderer_dynamic_heap_mb), 8, 1024)) << 20;
  create_info.DynamicHeapPageSize = 1 << 20;
  IDeviceContext* contexts[1] = {};
  // Intel XeSS as the antialiasing (xess_*): the device made with the extensions it needs (xess.h); made without
  // them when that fails.
  if (rex::cvar::GetFlagByName("native_x_renderer_antialiasing").rfind("xess", 0) == 0) {
    const EngineVkCreateInfo plain = create_info;
    if (Xess::PrepareDevice(create_info)) {
      factory->CreateDeviceAndContextsVk(create_info, &device_, contexts);
      if (!device_ || !contexts[0]) {
        REXLOG_WARN("[native_x_renderer] XeSS: no Vulkan device with its extensions; made without them (no XeSS)");
        Xess::DeviceFailed();
        device_.Release();
        contexts[0] = nullptr;
        create_info = plain;
      }
    }
  }
  if (!device_) factory->CreateDeviceAndContextsVk(create_info, &device_, contexts);
  context_.Attach(contexts[0]);
  recorder_.Initialize(context_);
  if (!device_ || !context_) {
    REXLOG_ERROR("[native_x_renderer] could not create the Vulkan device");
    return false;
  }
  if (fault_report_) StartBindingReport();
  if (fault_report_) StartMarkers();
  if (const char* race = std::getenv("NXR_RACE_CHECK"); race && *race == '1') {
    RefCntAutoPtr<IRenderDeviceVk> device_vk(device_, IID_RenderDeviceVk);
    InstallRaceCheck(&recorder_, device_vk ? device_vk->GetVkDevice() : VK_NULL_HANDLE);
  }
  if (name_objects && !fault_report_) {
    RefCntAutoPtr<IRenderDeviceVk> device_vk(device_, IID_RenderDeviceVk);
    if (device_vk && VulkanUtilities::SetupDebugUtils(device_vk->GetVkInstance(),
                                                      VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                                                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, 0, nullptr, nullptr))
      REXLOG_INFO("[native_x_renderer] Vulkan objects are named (NXR_DEBUG_NAMES)");
  }

  FenceDesc fence_desc;
  fence_desc.Name = "NativeXRenderer frames";
  fence_desc.Type = FENCE_TYPE_CPU_WAIT_ONLY;
  device_->CreateFence(fence_desc, &frame_fence_);
  if (!frame_fence_) {
    REXLOG_ERROR("[native_x_renderer] could not create the frame fence");
    return false;
  }

  InstallSwapChainFailureTest();
  if (!CreateSwapChain()) {
    REXLOG_ERROR("[native_x_renderer] could not create the swap chain on the game window");
    return false;
  }

  const auto& adapter = device_->GetAdapterInfo();
  const auto& desc = swap_chain_->GetDesc();
  REXLOG_INFO("[native_x_renderer] Vulkan device on {}; swap chain {}x{}", adapter.Description, desc.Width,
              desc.Height);
  return true;
}

static std::vector<VkPresentModeKHR> PresentModesFor(const std::string& mode, bool vsync_mailbox);
static const char* PresentModeText(const std::string& mode, bool vsync_mailbox);

bool Device::VsyncMailbox() const {
#ifdef _WIN32
  return false;
#else
  return !force_vsync_;
#endif
}

bool Device::CreateSwapChain() {
  IEngineFactoryVk* factory = GetEngineFactoryVk();
  if (!factory) return false;
  SwapChainDesc swap_chain_desc;
  // The guest's front buffer is 8-bit RGBA, already gamma-corrected by the game: no sRGB conversion
  // on the way out.
  swap_chain_desc.ColorBufferFormat = TEX_FORMAT_RGBA8_UNORM;
  swap_chain_desc.DepthBufferFormat = TEX_FORMAT_UNKNOWN;
  swap_chain_desc.BufferCount = 3;
  if (std::getenv("NXR_FRAME_DUMP")) swap_chain_desc.Usage |= SWAP_CHAIN_USAGE_COPY_SOURCE;  // DumpFrame
#ifdef _WIN32
  Win32NativeWindow window{static_cast<HWND>(window_)};
#else
  // Linux: Wayland when the window has a Wayland surface, else X11 (Xlib: display and window id).
  const auto* host = static_cast<const HostWindow*>(window_);
  NativeWindow window{};
  window.pDisplay = host->display;
  window.pWaylandSurface = host->wayland_surface;
  window.WindowId = host->x11_window;
  if (host->pixel_size) swap_chain_desc.Width = host->pixel_size(false), swap_chain_desc.Height = host->pixel_size(true);
#endif
  // Issue 26: made with the present mode and the V-Sync it is presented with (Present: V-Sync off, or on after a
  // failure), so the first Present needs no second swap chain. Before, the first one was made with V-Sync and made
  // again at the first Present; a player's PC refused every swap chain after the first on the game window.
  const std::string mode = vsync_fallback_ ? std::string() : WantedPresentMode();
  const std::vector<VkPresentModeKHR> modes = vsync_fallback_ ? std::vector<VkPresentModeKHR>() : PresentModesFor(mode, VsyncMailbox());
  SetSwapChainVkInitialPresentation(vsync_fallback_, modes.empty() ? nullptr : modes.data(), Uint32(modes.size()));
  factory->CreateSwapChainVk(device_, context_, swap_chain_desc, window, &swap_chain_);
  SetSwapChainVkInitialPresentation(true, nullptr, 0);  // DiligentCore's default again
  if (!swap_chain_) return false;
  if (!vsync_fallback_) {
    present_mode_applied_ = true;
    present_mode_ = mode;
    REXLOG_INFO("[native_x_renderer] present mode: {} (the swap chain made with it)", PresentModeText(mode, VsyncMailbox()));
  }
  return true;
}

// Issue 26 (14 Nov 2026, a player's RTX 4090 with six Vulkan overlay layers: OBS twice, Overwolf twice, RivaTuner,
// HudSight): the swap chain made at the start worked, but making it again for the first frame's present mode (the
// V-Sync of its creation off, MAILBOX: oldSwapchain set) failed with VK_ERROR_INITIALIZATION_FAILED. DiligentCore threw
// out of Present on the renderer's thread and the game closed, at the shaders page of a first start, every start. A
// failure now costs a frame: the swap chain is made again from nothing, as the one at the start was, and V-Sync stays
// on (Present(1): no present mode change, which would make it again with oldSwapchain).
bool Device::RebuildSwapChain(const char* where, const char* what) {
  if (what && std::strstr(what, "DEVICE_LOST")) ReportFault();
  ++rebuilds_;
  g_swap_chain_failed.store(false);
  if (!vsync_fallback_ || rebuilds_ <= 5) {
    REXLOG_ERROR("[native_x_renderer] the swap chain failed while {} ({}): made again from nothing, with V-Sync from "
                 "now on (time {})",
                 where, what ? what : "", rebuilds_);
  }
  vsync_fallback_ = true;
  if (RemakeSwapChain()) {
    const auto& desc = swap_chain_->GetDesc();
    REXLOG_INFO("[native_x_renderer] a new swap chain {}x{} (V-Sync)", desc.Width, desc.Height);
    return true;
  }
  REXLOG_ERROR("[native_x_renderer] no new swap chain could be made on the game window");
  return false;
}

// D229: the race check (NXR_RACE_CHECK; device.h).
namespace {
Recorder* g_race_recorder = nullptr;
std::atomic<VkCommandBuffer> g_recorder_cmd{VK_NULL_HANDLE};
std::atomic<bool> g_race_probe{false};
std::atomic<uint64_t> g_race_count{0};
std::mutex g_race_mutex;
std::vector<uint64_t> g_race_seen;

std::string RaceStack() {
  std::string out;
#if defined(_WIN32)
  void* frames[28] = {};
  const USHORT n = CaptureStackBackTrace(2, 28, frames, nullptr);
  for (USHORT i = 0; i < n; ++i) {
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(frames[i]), &module) &&
        module) {
      GetModuleFileNameA(module, name, MAX_PATH);
    }
    const char* base = std::strrchr(name, '\\');
    out += fmt::format("{}{}+0x{:X}", out.empty() ? "" : " ", base ? base + 1 : name,
                       uint64_t(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(module)));
  }
#endif
  return out;
}

// True to pass the command on.
bool RaceCheck(VkCommandBuffer cb, const char* what) {
  Recorder* r = g_race_recorder;
  if (!r || !r->threaded()) return true;
  if (r->OnThread()) {
    g_recorder_cmd.store(cb, std::memory_order_relaxed);
    return true;
  }
  if (cb != g_recorder_cmd.load(std::memory_order_relaxed) || r->Idle()) return true;
  const uint64_t count = ++g_race_count;
  const bool probe = g_race_probe.load(std::memory_order_acquire);
  uint64_t hash = 1469598103934665603ull;
#if defined(_WIN32)
  void* frames[28] = {};
  const USHORT n = CaptureStackBackTrace(1, 28, frames, nullptr);
  for (USHORT i = 0; i < n; ++i) hash = (hash ^ uint64_t(reinterpret_cast<uintptr_t>(frames[i]))) * 1099511628211ull;
#endif
  bool fresh = false;
  {
    std::lock_guard<std::mutex> lock(g_race_mutex);
    if (g_race_seen.size() < 32 && std::find(g_race_seen.begin(), g_race_seen.end(), hash) == g_race_seen.end()) {
      g_race_seen.push_back(hash);
      fresh = true;
    }
  }
  if (fresh)
    REXLOG_ERROR("[native_x_renderer] RACE{} ({}, #{}): a thread recorded into the recorder's command buffer while the "
                 "recorder had work in hand; stack: {}",
                 probe ? " (the self-test's, made on purpose)" : "", what, count, RaceStack());
  return !probe;
}

// D229, widened: Vulkan's external synchronisation. A queue, a command pool (and the command buffers from it), a
// command buffer, a descriptor pool and a descriptor set may each be used by one thread at a time; two threads in calls
// on the same one at once is a race, logged with both threads' names and this one's stack. Every call is also counted
// by its thread and kind (the threads that touch Vulkan, logged every 30 s and at a fault).
enum GuardKind { kGuardCmdBuffer, kGuardCmdPool, kGuardQueue, kGuardDescPool, kGuardDescSet, kGuardKinds };
constexpr const char* kGuardNames[kGuardKinds] = {"command buffer", "command pool", "queue", "descriptor pool",
                                                  "descriptor set"};
struct GuardOwner {
  uint32_t thread = 0;
  int depth = 0;
  const char* what = nullptr;
};
struct ThreadCounts {
  std::string name;
  uint64_t calls[kGuardKinds] = {};
};
std::mutex g_guard_mutex;
std::unordered_map<uint64_t, GuardOwner> g_guard;
std::unordered_map<uint64_t, uint64_t> g_cmd_pool;  // command buffer -> its pool
std::unordered_map<uint32_t, ThreadCounts> g_thread_counts;
std::atomic<uint64_t> g_guard_races{0};
std::chrono::steady_clock::time_point g_inventory_at{};

uint32_t ThisThread() {
#if defined(_WIN32)
  return uint32_t(GetCurrentThreadId());
#else
  return uint32_t(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}
std::string ThreadName(uint32_t id) {
#if defined(_WIN32)
  if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, DWORD(id))) {
    PWSTR wide = nullptr;
    std::string out;
    if (SUCCEEDED(GetThreadDescription(h, &wide)) && wide) {
      for (const wchar_t* c = wide; *c; ++c) out += *c < 128 ? char(*c) : '?';
      LocalFree(wide);
    }
    CloseHandle(h);
    if (!out.empty()) return out;
  }
#endif
  return "unnamed";
}
template <typename H>
uint64_t GuardKey(GuardKind kind, H handle) {
  return (uint64_t(reinterpret_cast<uintptr_t>(handle)) << 3) | uint64_t(kind);
}
std::string InventoryLine() {  // (the guard's mutex held)
  std::string out;
  for (const auto& [id, t] : g_thread_counts)
    out += fmt::format("{}{} \"{}\": {} cmd, {} cmd pool, {} queue, {} desc pool, {} desc set", out.empty() ? "" : "; ",
                       id, t.name, t.calls[kGuardCmdBuffer], t.calls[kGuardCmdPool], t.calls[kGuardQueue],
                       t.calls[kGuardDescPool], t.calls[kGuardDescSet]);
  return out;
}
// True if this thread now holds it (false: another thread was in a call on it: logged).
bool GuardEnter(uint64_t key, GuardKind kind, const char* what) {
  const uint32_t me = ThisThread();
  uint32_t other = 0;
  const char* other_what = nullptr;
  std::string inventory;
  {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    ThreadCounts& counts = g_thread_counts[me];
    if (counts.name.empty()) counts.name = ThreadName(me);
    ++counts.calls[kind];
    const auto now = std::chrono::steady_clock::now();
    if (now - g_inventory_at > std::chrono::seconds(30)) {
      g_inventory_at = now;
      inventory = InventoryLine();
    }
    GuardOwner& o = g_guard[key];
    if (o.depth == 0 || o.thread == me) {
      o.thread = me, ++o.depth, o.what = what;
    } else {
      other = o.thread, other_what = o.what;
    }
  }
  if (!inventory.empty())
    REXLOG_INFO("[native_x_renderer] race check: the threads in Vulkan calls so far: {}", inventory);
  if (!other) return true;
  const uint64_t count = ++g_guard_races;
  uint64_t hash = 1469598103934665603ull ^ uint64_t(kind);
#if defined(_WIN32)
  void* frames[28] = {};
  const USHORT n = CaptureStackBackTrace(1, 28, frames, nullptr);
  for (USHORT i = 0; i < n; ++i) hash = (hash ^ uint64_t(reinterpret_cast<uintptr_t>(frames[i]))) * 1099511628211ull;
#endif
  bool fresh = false;
  {
    std::lock_guard<std::mutex> lock(g_race_mutex);
    if (g_race_seen.size() < 64 && std::find(g_race_seen.begin(), g_race_seen.end(), hash) == g_race_seen.end()) {
      g_race_seen.push_back(hash);
      fresh = true;
    }
  }
  if (fresh)
    REXLOG_ERROR("[native_x_renderer] RACE on a {} (#{}): {} on thread {} \"{}\" while thread {} \"{}\" was in {}; "
                 "stack: {}",
                 kGuardNames[kind], count, what, me, ThreadName(me), other, ThreadName(other),
                 other_what ? other_what : "?", RaceStack());
  return false;
}
void GuardExit(uint64_t key, bool held) {
  if (!held) return;
  std::lock_guard<std::mutex> lock(g_guard_mutex);
  auto it = g_guard.find(key);
  if (it != g_guard.end() && --it->second.depth <= 0) g_guard.erase(it);
}
uint64_t PoolOf(VkCommandBuffer cb) {
  std::lock_guard<std::mutex> lock(g_guard_mutex);
  auto it = g_cmd_pool.find(uint64_t(reinterpret_cast<uintptr_t>(cb)));
  return it == g_cmd_pool.end() ? 0 : it->second;
}

// Calls on a command buffer: the recorder's rule (RaceCheck), then the buffer and its pool held for the call.
template <int I, typename R, typename... A>
struct CmdHook {
  static inline R(VKAPI_PTR* real)(VkCommandBuffer, A...) = nullptr;
  static inline const char* name = nullptr;
  static R VKAPI_PTR Call(VkCommandBuffer cb, A... a) {
    if (!RaceCheck(cb, name)) {
      if constexpr (std::is_void_v<R>) return;
      else return R{};
    }
    const uint64_t buffer_key = GuardKey(kGuardCmdBuffer, cb);
    const uint64_t pool = PoolOf(cb);
    const uint64_t pool_key = (pool << 3) | uint64_t(kGuardCmdPool);
    const bool held_buffer = GuardEnter(buffer_key, kGuardCmdBuffer, name);
    const bool held_pool = pool ? GuardEnter(pool_key, kGuardCmdPool, name) : false;
    if constexpr (std::is_void_v<R>) {
      real(cb, a...);
      GuardExit(pool_key, held_pool), GuardExit(buffer_key, held_buffer);
    } else {
      R r = real(cb, a...);
      GuardExit(pool_key, held_pool), GuardExit(buffer_key, held_buffer);
      return r;
    }
  }
};
template <int I, typename R, typename... A>
int Hook(R(VKAPI_PTR*& fn)(VkCommandBuffer, A...), const char* name) {
  if (!fn) return 0;
  CmdHook<I, R, A...>::real = fn;
  CmdHook<I, R, A...>::name = name;
  fn = &CmdHook<I, R, A...>::Call;
  return 1;
}
#define NXR_HOOK(fn) Hook<__COUNTER__>(fn, #fn)

// Calls on a queue: the queue held for the call.
template <int I, typename R, typename... A>
struct QueueHook {
  static inline R(VKAPI_PTR* real)(VkQueue, A...) = nullptr;
  static inline const char* name = nullptr;
  static R VKAPI_PTR Call(VkQueue q, A... a) {
    const uint64_t key = GuardKey(kGuardQueue, q);
    const bool held = GuardEnter(key, kGuardQueue, name);
    R r = real(q, a...);
    GuardExit(key, held);
    return r;
  }
};
template <int I, typename R, typename... A>
int HookQueue(R(VKAPI_PTR*& fn)(VkQueue, A...), const char* name) {
  if (!fn) return 0;
  QueueHook<I, R, A...>::real = fn;
  QueueHook<I, R, A...>::name = name;
  fn = &QueueHook<I, R, A...>::Call;
  return 1;
}
#define NXR_HOOK_QUEUE(fn) HookQueue<__COUNTER__>(fn, #fn)

// D229, widened again: descriptor sets' lifetimes. Each set's pool; the sets each command buffer binds (from its
// begin); each submission's fence and the sets of its command buffers. A pool reset or destroyed, or sets freed, while
// a submission the GPU has not finished (its fence not signalled) or a command buffer not yet submitted uses them, is
// a race (the GPU would read descriptors that are gone), logged with this thread's stack.
struct Flight {
  VkFence fence = VK_NULL_HANDLE;
  std::vector<uint64_t> pools;
  std::vector<VkDescriptorSet> sets;
  std::unordered_set<uint64_t> objects;  // the views and buffers (low bit set) its command buffers use
};
std::unordered_map<uint64_t, std::unordered_set<uint64_t>> g_cmd_objects;  // command buffer -> what it uses
std::atomic<uint64_t> g_inflight_destroys{0};
VkDevice g_vk_device = VK_NULL_HANDLE;
std::unordered_map<uint64_t, uint64_t> g_set_pool;                       // set -> its pool
std::unordered_map<uint64_t, std::vector<VkDescriptorSet>> g_cmd_sets;  // command buffer -> the sets it binds
std::vector<Flight> g_flights;
std::atomic<uint64_t> g_lifetime_races{0}, g_lifetime_checks{0};
PFN_vkCmdBindDescriptorSets g_next_bind_sets = nullptr;
PFN_vkBeginCommandBuffer g_next_begin = nullptr;
PFN_vkResetCommandBuffer g_next_reset_cb = nullptr;
PFN_vkQueueSubmit g_next_submit = nullptr;
PFN_vkDestroyDescriptorPool g_real_destroy_desc_pool = nullptr;
PFN_vkResetFences g_real_reset_fences = nullptr;
PFN_vkDestroyFence g_real_destroy_fence = nullptr;
std::atomic<uint64_t> g_fences_reset{0};

uint64_t H(const void* h) { return uint64_t(reinterpret_cast<uintptr_t>(h)); }

// The sets freed or reset (not since allocated again), the image views and buffers destroyed (not since made again),
// and the views and buffers each set was written with (a buffer's handle with its low bit set): a set no longer
// allocated, or one written with a view or buffer since destroyed, bound in a command buffer; or a destroyed view or
// buffer written into a set; each is a race (the GPU would read what is gone).
std::unordered_set<uint64_t> g_dead_sets, g_dead_objects;
std::unordered_map<uint64_t, std::vector<uint64_t>> g_pool_sets;
std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint64_t>>> g_set_objects;  // set -> (binding << 16 | element, object)
std::atomic<uint64_t> g_stale_races{0};
PFN_vkCreateImageView g_real_create_view = nullptr;
PFN_vkDestroyImageView g_real_destroy_view = nullptr;
PFN_vkCreateBuffer g_real_create_buffer = nullptr;
PFN_vkDestroyBuffer g_real_destroy_buffer = nullptr;
// Attachments: each framebuffer's views, each view's image; the images destroyed (a view whose image is gone is dead).
std::unordered_map<uint64_t, std::vector<uint64_t>> g_framebuffer_views;
std::unordered_map<uint64_t, uint64_t> g_view_image;
std::unordered_set<uint64_t> g_dead_images;
PFN_vkCreateFramebuffer g_real_create_framebuffer = nullptr;
PFN_vkDestroyFramebuffer g_real_destroy_framebuffer = nullptr;
PFN_vkDestroyImage g_real_destroy_image = nullptr;
PFN_vkCmdBeginRenderPass g_next_begin_pass = nullptr;
PFN_vkCmdBeginRendering g_next_begin_rendering = nullptr;
PFN_vkCmdBeginRenderingKHR g_next_begin_rendering_khr = nullptr;
// (The guard's mutex held.) Why the view is dead, or empty.
std::string ViewDead(uint64_t view) {
  if (g_dead_objects.count(view)) return "destroyed";
  auto it = g_view_image.find(view);
  if (it != g_view_image.end() && g_dead_images.count(it->second)) return fmt::format("its image 0x{:X} destroyed", it->second);
  return {};
}

void ReportStale(const std::string& text) {
  const uint64_t count = ++g_stale_races;
  uint64_t hash = 0xC2B2AE3D27D4EB4Full;
#if defined(_WIN32)
  void* frames[28] = {};
  const USHORT k = CaptureStackBackTrace(1, 28, frames, nullptr);
  for (USHORT i = 0; i < k; ++i) hash = (hash ^ uint64_t(reinterpret_cast<uintptr_t>(frames[i]))) * 1099511628211ull;
#endif
  bool fresh = false;
  {
    std::lock_guard<std::mutex> lock(g_race_mutex);
    if (g_race_seen.size() < 96 && std::find(g_race_seen.begin(), g_race_seen.end(), hash) == g_race_seen.end()) {
      g_race_seen.push_back(hash);
      fresh = true;
    }
  }
  if (fresh)
    REXLOG_ERROR("[native_x_renderer] RACE, a dead object (#{}): {}; thread {} \"{}\"; stack: {}", count, text,
                 ThisThread(), ThreadName(ThisThread()), RaceStack());
}
// (The guard's mutex held.)
void SetDead(uint64_t set) {
  g_dead_sets.insert(set);
  g_set_objects.erase(set);
}

void VKAPI_PTR BindSets(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first,
                        uint32_t count, const VkDescriptorSet* sets, uint32_t dyn_count, const uint32_t* dyn) {
  std::string stale;
  {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    auto& list = g_cmd_sets[H(cb)];
    for (uint32_t i = 0; i < count; ++i) {
      if (!sets[i]) continue;
      if (list.empty() || list.back() != sets[i]) list.push_back(sets[i]);
      if (auto so = g_set_objects.find(H(sets[i])); so != g_set_objects.end()) {
        auto& used = g_cmd_objects[H(cb)];
        for (const auto& [slot, o] : so->second) used.insert(o);
      }
      if (stale.empty() && g_dead_sets.count(H(sets[i])))
        stale = fmt::format("vkCmdBindDescriptorSets bound set 0x{:X} (set {}), which was freed or its pool reset",
                            H(sets[i]), first + i);
      if (stale.empty())
        if (auto it = g_set_objects.find(H(sets[i])); it != g_set_objects.end())
          for (const auto& [slot, o] : it->second)
            if (g_dead_objects.count(o) || (!(o & 1) && !ViewDead(o).empty())) {
              stale = fmt::format("vkCmdBindDescriptorSets bound set 0x{:X} (set {}), written with {} 0x{:X} since "
                                  "destroyed",
                                  H(sets[i]), first + i, (o & 1) ? "buffer" : "image view", o & ~uint64_t(1));
              break;
            }
    }
  }
  if (!stale.empty()) ReportStale(stale);
  g_next_bind_sets(cb, point, layout, first, count, sets, dyn_count, dyn);
}
VkResult VKAPI_PTR BeginCb(VkCommandBuffer cb, const VkCommandBufferBeginInfo* info) {
  {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    g_cmd_sets[H(cb)].clear();
    g_cmd_objects[H(cb)].clear();
  }
  return g_next_begin(cb, info);
}
VkResult VKAPI_PTR ResetCb(VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
  {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    g_cmd_sets[H(cb)].clear();
    g_cmd_objects[H(cb)].clear();
  }
  return g_next_reset_cb(cb, flags);
}
VkResult VKAPI_PTR SubmitQueue(VkQueue q, uint32_t n, const VkSubmitInfo* submits, VkFence fence) {
  {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    // A fence is used again only once it was signalled: what it guarded before is done.
    if (fence)
      std::erase_if(g_flights, [fence](const Flight& f) { return f.fence == fence; });
    Flight flight;
    flight.fence = fence;
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t c = 0; c < submits[i].commandBufferCount; ++c) {
        auto it = g_cmd_sets.find(H(submits[i].pCommandBuffers[c]));
        if (it == g_cmd_sets.end()) continue;
        for (VkDescriptorSet set : it->second) {
          flight.sets.push_back(set);
          auto pool = g_set_pool.find(H(set));
          if (pool != g_set_pool.end() &&
              std::find(flight.pools.begin(), flight.pools.end(), pool->second) == flight.pools.end())
            flight.pools.push_back(pool->second);
        }
        it->second.clear();  // (submitted: followed by its fence from now on)
      }
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t c = 0; c < submits[i].commandBufferCount; ++c) {
        auto it = g_cmd_objects.find(H(submits[i].pCommandBuffers[c]));
        if (it == g_cmd_objects.end()) continue;
        flight.objects.insert(it->second.begin(), it->second.end());
        it->second.clear();
      }
    if (!flight.sets.empty() || !flight.objects.empty()) {
      if (fence) g_flights.push_back(std::move(flight));
      // (No fence: nothing to tell when it is done; such submissions are counted, not followed.)
    }
  }
  return g_next_submit(q, n, submits, fence);
}

// Whether `pool` (or, with `sets`, those of its sets) is in use by unfinished work; logs it if so.
void CheckSetsInUse(uint64_t pool, const VkDescriptorSet* sets, uint32_t n, const char* what) {
  ++g_lifetime_checks;
  std::string where;
  {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    // Done submissions dropped.
    std::erase_if(g_flights, [](const Flight& f) {
      return !f.fence || !vkGetFenceStatus || vkGetFenceStatus(g_vk_device, f.fence) == VK_SUCCESS;
    });
    auto uses = [&](const std::vector<VkDescriptorSet>& list) -> bool {
      for (VkDescriptorSet set : list) {
        if (sets) {
          for (uint32_t i = 0; i < n; ++i)
            if (sets[i] == set) return true;
        } else {
          auto it = g_set_pool.find(H(set));
          if (it != g_set_pool.end() && it->second == pool) return true;
        }
      }
      return false;
    };
    for (const Flight& f : g_flights) {
      if (std::find(f.pools.begin(), f.pools.end(), pool) == f.pools.end()) continue;
      if (uses(f.sets)) {
        where = fmt::format("a submission not yet finished by the GPU (fence 0x{:X}; {} in flight)", H(f.fence),
                            g_flights.size());
        break;
      }
    }
    if (where.empty())
      for (const auto& [cb, list] : g_cmd_sets)
        if (!list.empty() && uses(list)) {
          where = fmt::format("command buffer 0x{:X}, recorded and not yet submitted", cb);
          break;
        }
  }
  if (where.empty()) return;
  const uint64_t count = ++g_lifetime_races;
  uint64_t hash = 0x9E3779B97F4A7C15ull;
#if defined(_WIN32)
  void* frames[28] = {};
  const USHORT k = CaptureStackBackTrace(1, 28, frames, nullptr);
  for (USHORT i = 0; i < k; ++i) hash = (hash ^ uint64_t(reinterpret_cast<uintptr_t>(frames[i]))) * 1099511628211ull;
#endif
  bool fresh = false;
  {
    std::lock_guard<std::mutex> lock(g_race_mutex);
    if (g_race_seen.size() < 64 && std::find(g_race_seen.begin(), g_race_seen.end(), hash) == g_race_seen.end()) {
      g_race_seen.push_back(hash);
      fresh = true;
    }
  }
  if (fresh)
    REXLOG_ERROR("[native_x_renderer] RACE, a descriptor set's lifetime (#{}): {} on pool 0x{:X} while {} uses its "
                 "sets; thread {} \"{}\"; stack: {}",
                 count, what, pool, where, ThisThread(), ThreadName(ThisThread()), RaceStack());
}
// A fence reset or destroyed was signalled (Vulkan forbids it while its submission is pending): what it guarded is
// done. (DiligentCore resets its fences as it takes them back, long before it uses them again: without this, a done
// submission's fence read as unsignalled.)
void ForgetFences(uint32_t n, const VkFence* fences) {
  std::lock_guard<std::mutex> lock(g_guard_mutex);
  for (uint32_t i = 0; i < n; ++i) {
    const VkFence f = fences[i];
    std::erase_if(g_flights, [f](const Flight& x) { return x.fence == f; });
  }
}
VkResult VKAPI_PTR ResetFences(VkDevice d, uint32_t n, const VkFence* fences) {
  ForgetFences(n, fences);
  g_fences_reset += n;
  return g_real_reset_fences(d, n, fences);
}
void VKAPI_PTR DestroyFence(VkDevice d, VkFence fence, const VkAllocationCallbacks* alloc) {
  if (fence) ForgetFences(1, &fence);
  g_real_destroy_fence(d, fence, alloc);
}
void VKAPI_PTR DestroyDescPool(VkDevice d, VkDescriptorPool pool, const VkAllocationCallbacks* alloc) {
  CheckSetsInUse(H(pool), nullptr, 0, "vkDestroyDescriptorPool");
  g_real_destroy_desc_pool(d, pool, alloc);
}

// Calls on pools and sets (the device first): each with its own key.
PFN_vkAllocateCommandBuffers g_real_alloc_cmd = nullptr;
PFN_vkFreeCommandBuffers g_real_free_cmd = nullptr;
PFN_vkResetCommandPool g_real_reset_cmd_pool = nullptr;
PFN_vkAllocateDescriptorSets g_real_alloc_sets = nullptr;
PFN_vkFreeDescriptorSets g_real_free_sets = nullptr;
PFN_vkResetDescriptorPool g_real_reset_desc_pool = nullptr;
PFN_vkUpdateDescriptorSets g_real_update_sets = nullptr;
VkResult VKAPI_PTR AllocCmd(VkDevice d, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* out) {
  const uint64_t key = GuardKey(kGuardCmdPool, info->commandPool);
  const bool held = GuardEnter(key, kGuardCmdPool, "vkAllocateCommandBuffers");
  const VkResult r = g_real_alloc_cmd(d, info, out);
  if (r == VK_SUCCESS) {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    for (uint32_t i = 0; i < info->commandBufferCount; ++i)
      g_cmd_pool[uint64_t(reinterpret_cast<uintptr_t>(out[i]))] = uint64_t(reinterpret_cast<uintptr_t>(info->commandPool));
  }
  GuardExit(key, held);
  return r;
}
void VKAPI_PTR FreeCmd(VkDevice d, VkCommandPool pool, uint32_t n, const VkCommandBuffer* cbs) {
  const uint64_t key = GuardKey(kGuardCmdPool, pool);
  const bool held = GuardEnter(key, kGuardCmdPool, "vkFreeCommandBuffers");
  {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    for (uint32_t i = 0; i < n; ++i) g_cmd_pool.erase(uint64_t(reinterpret_cast<uintptr_t>(cbs[i])));
  }
  g_real_free_cmd(d, pool, n, cbs);
  GuardExit(key, held);
}
VkResult VKAPI_PTR ResetCmdPool(VkDevice d, VkCommandPool pool, VkCommandPoolResetFlags flags) {
  const uint64_t key = GuardKey(kGuardCmdPool, pool);
  const bool held = GuardEnter(key, kGuardCmdPool, "vkResetCommandPool");
  const VkResult r = g_real_reset_cmd_pool(d, pool, flags);
  GuardExit(key, held);
  return r;
}
VkResult VKAPI_PTR AllocSets(VkDevice d, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* out) {
  const uint64_t key = GuardKey(kGuardDescPool, info->descriptorPool);
  const bool held = GuardEnter(key, kGuardDescPool, "vkAllocateDescriptorSets");
  const VkResult r = g_real_alloc_sets(d, info, out);
  if (r == VK_SUCCESS) {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    for (uint32_t i = 0; i < info->descriptorSetCount; ++i) {
      const uint64_t set = uint64_t(reinterpret_cast<uintptr_t>(out[i]));
      g_set_pool[set] = uint64_t(reinterpret_cast<uintptr_t>(info->descriptorPool));
      g_dead_sets.erase(set);
      g_set_objects.erase(set);
      g_pool_sets[uint64_t(reinterpret_cast<uintptr_t>(info->descriptorPool))].push_back(set);
    }
  }
  GuardExit(key, held);
  return r;
}
VkResult VKAPI_PTR FreeSets(VkDevice d, VkDescriptorPool pool, uint32_t n, const VkDescriptorSet* sets) {
  const uint64_t key = GuardKey(kGuardDescPool, pool);
  const bool held = GuardEnter(key, kGuardDescPool, "vkFreeDescriptorSets");
  CheckSetsInUse(uint64_t(reinterpret_cast<uintptr_t>(pool)), sets, n, "vkFreeDescriptorSets");
  {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    for (uint32_t i = 0; i < n; ++i)
      if (sets[i]) SetDead(H(sets[i]));
  }
  const VkResult r = g_real_free_sets(d, pool, n, sets);
  GuardExit(key, held);
  return r;
}
VkResult VKAPI_PTR ResetDescPool(VkDevice d, VkDescriptorPool pool, VkDescriptorPoolResetFlags flags) {
  const uint64_t key = GuardKey(kGuardDescPool, pool);
  const bool held = GuardEnter(key, kGuardDescPool, "vkResetDescriptorPool");
  CheckSetsInUse(uint64_t(reinterpret_cast<uintptr_t>(pool)), nullptr, 0, "vkResetDescriptorPool");
  {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    auto it = g_pool_sets.find(H(pool));
    if (it != g_pool_sets.end()) {
      for (uint64_t set : it->second) SetDead(set);
      it->second.clear();
    }
  }
  const VkResult r = g_real_reset_desc_pool(d, pool, flags);
  GuardExit(key, held);
  return r;
}
void VKAPI_PTR UpdateSets(VkDevice d, uint32_t nw, const VkWriteDescriptorSet* w, uint32_t nc, const VkCopyDescriptorSet* c) {
  const VkDescriptorSet set = nw ? w[0].dstSet : nc ? c[0].dstSet : VK_NULL_HANDLE;
  const uint64_t key = GuardKey(kGuardDescSet, set);
  const bool held = set ? GuardEnter(key, kGuardDescSet, "vkUpdateDescriptorSets") : false;
  std::string stale;
  {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    for (uint32_t i = 0; i < nw; ++i) {
      auto& objects = g_set_objects[H(w[i].dstSet)];
      if (stale.empty() && g_dead_sets.count(H(w[i].dstSet)))
        stale = fmt::format("vkUpdateDescriptorSets wrote set 0x{:X}, which was freed or its pool reset", H(w[i].dstSet));
      for (uint32_t j = 0; j < w[i].descriptorCount; ++j) {
        uint64_t o = 0;
        switch (w[i].descriptorType) {
          case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
          case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
          case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
          case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            if (w[i].pImageInfo) o = H(w[i].pImageInfo[j].imageView);
            break;
          case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
          case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
          case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
          case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            if (w[i].pBufferInfo && w[i].pBufferInfo[j].buffer) o = H(w[i].pBufferInfo[j].buffer) | 1;
            break;
          default:
            break;
        }
        if (!o) continue;
        if (stale.empty() && g_dead_objects.count(o))
          stale = fmt::format("vkUpdateDescriptorSets wrote {} 0x{:X}, already destroyed, into set 0x{:X}",
                              (o & 1) ? "buffer" : "image view", o & ~uint64_t(1), H(w[i].dstSet));
        const uint32_t slot = (w[i].dstBinding << 16) | (w[i].dstArrayElement + j);
        auto at = std::find_if(objects.begin(), objects.end(), [slot](const auto& x) { return x.first == slot; });
        if (at != objects.end()) at->second = o;
        else if (objects.size() < 256) objects.emplace_back(slot, o);
      }
    }
  }
  if (!stale.empty()) ReportStale(stale);
  g_real_update_sets(d, nw, w, nc, c);
  GuardExit(key, held);
}
// A view, buffer (low bit set) or image (`image`: its views) destroyed while a submission the GPU has not finished, or a
// command buffer not yet submitted, uses it: logged.
void CheckObjectInUse(uint64_t object, uint64_t image, const char* what) {
  std::string where;
  {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    std::erase_if(g_flights, [](const Flight& f) {
      return !f.fence || !vkGetFenceStatus || vkGetFenceStatus(g_vk_device, f.fence) == VK_SUCCESS;
    });
    auto uses = [&](const std::unordered_set<uint64_t>& objects) -> uint64_t {
      if (object) return objects.count(object) ? object : 0;
      for (uint64_t o : objects)
        if (!(o & 1))
          if (auto it = g_view_image.find(o); it != g_view_image.end() && it->second == image) return o;
      return 0;
    };
    for (const Flight& f : g_flights)
      if (const uint64_t o = uses(f.objects)) {
        where = fmt::format("a submission not yet finished by the GPU (fence 0x{:X}; {} in flight) uses {} 0x{:X}",
                            H(f.fence), g_flights.size(), (o & 1) ? "buffer" : "view", o & ~uint64_t(1));
        break;
      }
    if (where.empty())
      for (const auto& [cb, objects] : g_cmd_objects)
        if (const uint64_t o = uses(objects)) {
          where = fmt::format("command buffer 0x{:X}, recorded and not yet submitted, uses {} 0x{:X}", cb,
                              (o & 1) ? "buffer" : "view", o & ~uint64_t(1));
          break;
        }
  }
  if (where.empty()) return;
  ++g_inflight_destroys;
  ReportStale(fmt::format("{} of {} 0x{:X} while {}", what, image ? "image" : (object & 1) ? "buffer" : "view",
                          image ? image : object & ~uint64_t(1), where));
}
VkResult VKAPI_PTR CreateView(VkDevice d, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* a, VkImageView* out) {
  const VkResult r = g_real_create_view(d, info, a, out);
  if (r == VK_SUCCESS) {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    g_dead_objects.erase(H(*out));
    g_view_image[H(*out)] = H(info->image);
    g_dead_images.erase(H(info->image));  // (a live image's handle, made again)
  }
  return r;
}
void VKAPI_PTR DestroyImage(VkDevice d, VkImage image, const VkAllocationCallbacks* a) {
  if (image) CheckObjectInUse(0, H(image), "vkDestroyImage");
  if (image) {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    g_dead_images.insert(H(image));
  }
  g_real_destroy_image(d, image, a);
}
VkResult VKAPI_PTR CreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo* info, const VkAllocationCallbacks* a,
                                     VkFramebuffer* out) {
  std::string stale;
  {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    for (uint32_t i = 0; i < info->attachmentCount && stale.empty(); ++i)
      if (std::string why = ViewDead(H(info->pAttachments[i])); !why.empty())
        stale = fmt::format("vkCreateFramebuffer with attachment {} view 0x{:X}: {}", i, H(info->pAttachments[i]), why);
  }
  if (!stale.empty()) ReportStale(stale);
  const VkResult r = g_real_create_framebuffer(d, info, a, out);
  if (r == VK_SUCCESS) {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    auto& views = g_framebuffer_views[H(*out)];
    views.clear();
    for (uint32_t i = 0; i < info->attachmentCount; ++i) views.push_back(H(info->pAttachments[i]));
  }
  return r;
}
void VKAPI_PTR DestroyFramebuffer(VkDevice d, VkFramebuffer fb, const VkAllocationCallbacks* a) {
  if (fb) {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    g_framebuffer_views.erase(H(fb));
  }
  g_real_destroy_framebuffer(d, fb, a);
}
void VKAPI_PTR BeginPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
  std::string stale;
  {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    if (auto it = g_framebuffer_views.find(H(info->framebuffer)); it != g_framebuffer_views.end())
      g_cmd_objects[H(cb)].insert(it->second.begin(), it->second.end());
    if (auto it = g_framebuffer_views.find(H(info->framebuffer)); it != g_framebuffer_views.end())
      for (size_t i = 0; i < it->second.size() && stale.empty(); ++i)
        if (std::string why = ViewDead(it->second[i]); !why.empty())
          stale = fmt::format("vkCmdBeginRenderPass, framebuffer 0x{:X} attachment {} view 0x{:X}: {}",
                              H(info->framebuffer), i, it->second[i], why);
  }
  if (!stale.empty()) ReportStale(stale);
  g_next_begin_pass(cb, info, contents);
}
void RenderingUses(VkCommandBuffer cb, const VkRenderingInfo* info) {  // (the guard's mutex held)
  auto& used = g_cmd_objects[H(cb)];
  for (uint32_t i = 0; i < info->colorAttachmentCount; ++i)
    for (VkImageView v : {info->pColorAttachments[i].imageView, info->pColorAttachments[i].resolveImageView})
      if (v) used.insert(H(v));
  for (const VkRenderingAttachmentInfo* at : {info->pDepthAttachment, info->pStencilAttachment})
    if (at && at->imageView) used.insert(H(at->imageView));
}
std::string RenderingDead(const VkRenderingInfo* info) {  // (the guard's mutex held)
  for (uint32_t i = 0; i < info->colorAttachmentCount; ++i)
    for (VkImageView v : {info->pColorAttachments[i].imageView, info->pColorAttachments[i].resolveImageView})
      if (v)
        if (std::string why = ViewDead(H(v)); !why.empty())
          return fmt::format("vkCmdBeginRendering, colour attachment {} view 0x{:X}: {}", i, H(v), why);
  for (const VkRenderingAttachmentInfo* at : {info->pDepthAttachment, info->pStencilAttachment})
    if (at && at->imageView)
      if (std::string why = ViewDead(H(at->imageView)); !why.empty())
        return fmt::format("vkCmdBeginRendering, depth or stencil view 0x{:X}: {}", H(at->imageView), why);
  return {};
}
void VKAPI_PTR BeginRendering(VkCommandBuffer cb, const VkRenderingInfo* info) {
  std::string stale;
  {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    stale = RenderingDead(info);
    RenderingUses(cb, info);
  }
  if (!stale.empty()) ReportStale(stale);
  g_next_begin_rendering(cb, info);
}
void VKAPI_PTR BeginRenderingKhr(VkCommandBuffer cb, const VkRenderingInfo* info) {
  std::string stale;
  {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    stale = RenderingDead(info);
    RenderingUses(cb, info);
  }
  if (!stale.empty()) ReportStale(stale);
  g_next_begin_rendering_khr(cb, info);
}
void VKAPI_PTR DestroyView(VkDevice d, VkImageView view, const VkAllocationCallbacks* a) {
  if (view) CheckObjectInUse(H(view), 0, "vkDestroyImageView");
  if (view) {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    g_dead_objects.insert(H(view));
  }
  g_real_destroy_view(d, view, a);
}
VkResult VKAPI_PTR CreateBuf(VkDevice d, const VkBufferCreateInfo* info, const VkAllocationCallbacks* a, VkBuffer* out) {
  const VkResult r = g_real_create_buffer(d, info, a, out);
  if (r == VK_SUCCESS) {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    g_dead_objects.erase(H(*out) | 1);
  }
  return r;
}
void VKAPI_PTR DestroyBuf(VkDevice d, VkBuffer buffer, const VkAllocationCallbacks* a) {
  if (buffer) CheckObjectInUse(H(buffer) | 1, 0, "vkDestroyBuffer");
  if (buffer) {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    g_dead_objects.insert(H(buffer) | 1);
  }
  g_real_destroy_buffer(d, buffer, a);
}
template <typename F>
int HookDevice(F& fn, F& real, F replacement) {
  if (!fn) return 0;
  real = fn;
  fn = replacement;
  return 1;
}

void InstallRaceCheck(Recorder* recorder, VkDevice device) {
  g_race_recorder = recorder;
  g_vk_device = device;
  const int commands =
      NXR_HOOK(vkCmdDraw) + NXR_HOOK(vkCmdDrawIndexed) + NXR_HOOK(vkCmdDrawIndirect) + NXR_HOOK(vkCmdDrawIndexedIndirect) +
      NXR_HOOK(vkCmdDispatch) + NXR_HOOK(vkCmdDispatchIndirect) + NXR_HOOK(vkCmdBindPipeline) +
      NXR_HOOK(vkCmdBindDescriptorSets) + NXR_HOOK(vkCmdBindVertexBuffers) + NXR_HOOK(vkCmdBindIndexBuffer) +
      NXR_HOOK(vkCmdPushConstants) + NXR_HOOK(vkCmdPipelineBarrier) + NXR_HOOK(vkCmdCopyBuffer) +
      NXR_HOOK(vkCmdCopyImage) + NXR_HOOK(vkCmdBlitImage) + NXR_HOOK(vkCmdCopyBufferToImage) +
      NXR_HOOK(vkCmdCopyImageToBuffer) + NXR_HOOK(vkCmdUpdateBuffer) + NXR_HOOK(vkCmdFillBuffer) +
      NXR_HOOK(vkCmdClearColorImage) + NXR_HOOK(vkCmdClearDepthStencilImage) + NXR_HOOK(vkCmdClearAttachments) +
      NXR_HOOK(vkCmdResolveImage) + NXR_HOOK(vkCmdBeginRenderPass) + NXR_HOOK(vkCmdEndRenderPass) +
      NXR_HOOK(vkCmdNextSubpass) + NXR_HOOK(vkCmdSetViewport) + NXR_HOOK(vkCmdSetScissor) +
      NXR_HOOK(vkCmdSetBlendConstants) + NXR_HOOK(vkCmdSetStencilReference) + NXR_HOOK(vkCmdBeginQuery) +
      NXR_HOOK(vkCmdEndQuery) + NXR_HOOK(vkCmdResetQueryPool) + NXR_HOOK(vkCmdWriteTimestamp) +
      NXR_HOOK(vkCmdCopyQueryPoolResults) + NXR_HOOK(vkCmdExecuteCommands) + NXR_HOOK(vkBeginCommandBuffer) +
      NXR_HOOK(vkEndCommandBuffer) + NXR_HOOK(vkResetCommandBuffer) + NXR_HOOK(vkCmdBeginRendering) +
      NXR_HOOK(vkCmdEndRendering) + NXR_HOOK(vkCmdBeginRenderingKHR) + NXR_HOOK(vkCmdEndRenderingKHR) +
      NXR_HOOK(vkCmdPipelineBarrier2) + NXR_HOOK(vkCmdPipelineBarrier2KHR) + NXR_HOOK(vkCmdBeginDebugUtilsLabelEXT) +
      NXR_HOOK(vkCmdEndDebugUtilsLabelEXT);
  const int queues = NXR_HOOK_QUEUE(vkQueueSubmit) + NXR_HOOK_QUEUE(vkQueueSubmit2) + NXR_HOOK_QUEUE(vkQueueSubmit2KHR) +
                     NXR_HOOK_QUEUE(vkQueueBindSparse) + NXR_HOOK_QUEUE(vkQueuePresentKHR) +
                     NXR_HOOK_QUEUE(vkQueueWaitIdle);
  const int others = HookDevice(vkAllocateCommandBuffers, g_real_alloc_cmd, &AllocCmd) +
                     HookDevice(vkFreeCommandBuffers, g_real_free_cmd, &FreeCmd) +
                     HookDevice(vkResetCommandPool, g_real_reset_cmd_pool, &ResetCmdPool) +
                     HookDevice(vkAllocateDescriptorSets, g_real_alloc_sets, &AllocSets) +
                     HookDevice(vkFreeDescriptorSets, g_real_free_sets, &FreeSets) +
                     HookDevice(vkResetDescriptorPool, g_real_reset_desc_pool, &ResetDescPool) +
                     HookDevice(vkUpdateDescriptorSets, g_real_update_sets, &UpdateSets);
  // The lifetime tracker's, over the guards' (each calls the next).
  int lifetimes = 0;
  if (vkCmdBindDescriptorSets) g_next_bind_sets = vkCmdBindDescriptorSets, vkCmdBindDescriptorSets = &BindSets, ++lifetimes;
  if (vkBeginCommandBuffer) g_next_begin = vkBeginCommandBuffer, vkBeginCommandBuffer = &BeginCb, ++lifetimes;
  if (vkResetCommandBuffer) g_next_reset_cb = vkResetCommandBuffer, vkResetCommandBuffer = &ResetCb, ++lifetimes;
  if (vkQueueSubmit) g_next_submit = vkQueueSubmit, vkQueueSubmit = &SubmitQueue, ++lifetimes;
  lifetimes += HookDevice(vkDestroyDescriptorPool, g_real_destroy_desc_pool, &DestroyDescPool);
  lifetimes += HookDevice(vkResetFences, g_real_reset_fences, &ResetFences);
  lifetimes += HookDevice(vkDestroyFence, g_real_destroy_fence, &DestroyFence);
  lifetimes += HookDevice(vkCreateImageView, g_real_create_view, &CreateView);
  lifetimes += HookDevice(vkDestroyImageView, g_real_destroy_view, &DestroyView);
  lifetimes += HookDevice(vkCreateBuffer, g_real_create_buffer, &CreateBuf);
  lifetimes += HookDevice(vkDestroyBuffer, g_real_destroy_buffer, &DestroyBuf);
  lifetimes += HookDevice(vkDestroyImage, g_real_destroy_image, &DestroyImage);
  lifetimes += HookDevice(vkCreateFramebuffer, g_real_create_framebuffer, &CreateFramebuffer);
  lifetimes += HookDevice(vkDestroyFramebuffer, g_real_destroy_framebuffer, &DestroyFramebuffer);
  // (Over the guards' command hooks: each calls the next.)
  if (vkCmdBeginRenderPass) g_next_begin_pass = vkCmdBeginRenderPass, vkCmdBeginRenderPass = &BeginPass, ++lifetimes;
  if (vkCmdBeginRendering) g_next_begin_rendering = vkCmdBeginRendering, vkCmdBeginRendering = &BeginRendering, ++lifetimes;
  if (vkCmdBeginRenderingKHR)
    g_next_begin_rendering_khr = vkCmdBeginRenderingKHR, vkCmdBeginRenderingKHR = &BeginRenderingKhr, ++lifetimes;
  REXLOG_INFO("[native_x_renderer] race check on (NXR_RACE_CHECK): {} command buffer calls, {} queue calls and {} pool "
              "and set calls checked; descriptor sets' lifetimes followed ({} calls; device {})",
              commands, queues, others, lifetimes, device ? "known" : "NOT KNOWN: lifetimes not followed");
}
}  // namespace

void Device::RaceSelfTest() {
  if (!g_race_recorder || !recorder_.threaded()) {
    REXLOG_WARN("[native_x_renderer] race check's self-test: no check or no recorder thread");
    return;
  }
  // A command on the recorder's thread, then it stays busy a while; meanwhile this thread records into the same
  // command buffer (the probe: checked, not passed on).
  std::atomic<VkCommandBuffer> cb{VK_NULL_HANDLE};
  recorder_.Do([&cb](IDeviceContext* context) {
    RefCntAutoPtr<IDeviceContextVk> vk(context, IID_DeviceContextVk);
    if (vk) {
      const VkCommandBuffer b = vk->GetVkCommandBuffer();
      g_recorder_cmd.store(b, std::memory_order_relaxed);
      cb.store(b, std::memory_order_release);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  });
  recorder_.Publish();
  for (int i = 0; i < 100 && !cb.load(std::memory_order_acquire); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const uint64_t before = g_race_count.load();
  if (const VkCommandBuffer b = cb.load(std::memory_order_acquire); b && vkCmdPipelineBarrier) {
    g_race_probe.store(true, std::memory_order_release);
    vkCmdPipelineBarrier(b, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 0, nullptr);
    g_race_probe.store(false, std::memory_order_release);
  }
  recorder_.Drain();
  REXLOG_INFO("[native_x_renderer] race check's self-test: {}",
              g_race_count.load() > before ? "the race made on purpose was caught" : "NOT CAUGHT: the check does not work");
  // The guard's: another thread holds a made-up queue a while; this one comes to it meanwhile.
  const uint64_t key = GuardKey(kGuardQueue, reinterpret_cast<void*>(uintptr_t(0x5E1F7E57)));
  std::atomic<bool> holding{false};
  std::thread other([&] {
    const bool held = GuardEnter(key, kGuardQueue, "the self-test's hold");
    holding.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    GuardExit(key, held);
  });
  while (!holding.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const uint64_t guard_before = g_guard_races.load();
  GuardExit(key, GuardEnter(key, kGuardQueue, "the self-test's own call (made on purpose)"));
  other.join();
  REXLOG_INFO("[native_x_renderer] race check's self-test (two threads on one queue): {}",
              g_guard_races.load() > guard_before ? "caught" : "NOT CAUGHT: the guard does not work");
  // The lifetime tracker's: a made-up set of a made-up pool in a submission whose fence (a real one, never signalled)
  // is not done; the pool reset meanwhile.
  if (g_vk_device && vkCreateFence && vkDestroyFence) {
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(g_vk_device, &fci, nullptr, &fence) == VK_SUCCESS) {
      const uint64_t fake_pool = 0x5E1F7E570000ull, fake_set = 0x5E1F7E571000ull;
      {
        std::lock_guard<std::mutex> lock(g_guard_mutex);
        g_set_pool[fake_set] = fake_pool;
        Flight f;
        f.fence = fence, f.pools = {fake_pool}, f.sets = {reinterpret_cast<VkDescriptorSet>(uintptr_t(fake_set))};
        g_flights.push_back(std::move(f));
      }
      const uint64_t lifetime_before = g_lifetime_races.load();
      CheckSetsInUse(fake_pool, nullptr, 0, "the self-test's reset (made on purpose)");
      {
        std::lock_guard<std::mutex> lock(g_guard_mutex);
        std::erase_if(g_flights, [fence](const Flight& f) { return f.fence == fence; });
        g_set_pool.erase(fake_set);
      }
      vkDestroyFence(g_vk_device, fence, nullptr);
      REXLOG_INFO("[native_x_renderer] race check's self-test (a pool reset while in use): {}",
                  g_lifetime_races.load() > lifetime_before ? "caught" : "NOT CAUGHT: the tracker does not work");
      // The dead objects': a made-up set written with a made-up view, the view destroyed (marked so), the set
      // checked as a bind would check it.
      const uint64_t stale_before = g_stale_races.load();
      {
        std::lock_guard<std::mutex> lock(g_guard_mutex);
        g_set_objects[fake_set] = {{0u, 0x5E1F7E572000ull}};
        g_dead_objects.insert(0x5E1F7E572000ull);
      }
      std::string text;
      {
        std::lock_guard<std::mutex> lock(g_guard_mutex);
        for (const auto& [slot, o] : g_set_objects[fake_set])
          if (g_dead_objects.count(o)) text = "the self-test's set bound, its view destroyed (made on purpose)";
        g_set_objects.erase(fake_set);
        g_dead_objects.erase(0x5E1F7E572000ull);
      }
      if (!text.empty()) ReportStale(text);
      REXLOG_INFO("[native_x_renderer] race check's self-test (a set holding a destroyed view): {}",
                  g_stale_races.load() > stale_before ? "caught" : "NOT CAUGHT: the dead objects' check does not work");
      VkFence fence2 = VK_NULL_HANDLE;
      if (vkCreateFence(g_vk_device, &fci, nullptr, &fence2) == VK_SUCCESS) {
        const uint64_t fake_view = 0x5E1F7E573000ull;
        {
          std::lock_guard<std::mutex> lock(g_guard_mutex);
          Flight f;
          f.fence = fence2, f.objects = {fake_view};
          g_flights.push_back(std::move(f));
        }
        const uint64_t inflight_before = g_inflight_destroys.load();
        CheckObjectInUse(fake_view, 0, "the self-test's vkDestroyImageView (made on purpose)");
        {
          std::lock_guard<std::mutex> lock(g_guard_mutex);
          std::erase_if(g_flights, [fence2](const Flight& f) { return f.fence == fence2; });
        }
        vkDestroyFence(g_vk_device, fence2, nullptr);
        REXLOG_INFO("[native_x_renderer] race check's self-test (a view destroyed while in flight): {}",
                    g_inflight_destroys.load() > inflight_before ? "caught" : "NOT CAUGHT: the in-flight check does not work");
      }
    }
  }
}

// D229: the memory binds the driver reports (NXR_DEVICE_FAULT), kept for naming a fault's address.
namespace {
struct BindRecord {
  uint64_t base = 0, size = 0;
  bool bound = true;
  uint64_t handle = 0;
  int type = 0;
  std::string name;
  uint64_t order = 0;
};
std::mutex g_binds_mutex;
std::vector<BindRecord> g_binds;
uint64_t g_bind_order = 0;
VkBool32 VKAPI_PTR OnBindingReport(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT type,
                                   const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  if (!(type & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) || !data) return VK_FALSE;
  for (auto* next = static_cast<const VkBaseInStructure*>(data->pNext); next; next = next->pNext) {
    if (next->sType != VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) continue;
    const auto* b = reinterpret_cast<const VkDeviceAddressBindingCallbackDataEXT*>(next);
    BindRecord r;
    r.base = b->baseAddress, r.size = b->size, r.bound = b->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT;
    if (data->objectCount > 0 && data->pObjects) {
      r.handle = data->pObjects[0].objectHandle, r.type = int(data->pObjects[0].objectType);
      if (data->pObjects[0].pObjectName) r.name = data->pObjects[0].pObjectName;
    }
    std::lock_guard<std::mutex> lock(g_binds_mutex);
    r.order = ++g_bind_order;
    if (g_binds.size() < 400000) g_binds.push_back(std::move(r));
  }
  return VK_FALSE;
}
}  // namespace

void Device::StartBindingReport() {
  RefCntAutoPtr<IRenderDeviceVk> device_vk(device_, IID_RenderDeviceVk);
  if (!device_vk || !vkGetInstanceProcAddr) return;
  const VkInstance instance = device_vk->GetVkInstance();
  // DiligentCore's own messenger (errors) also loads the names' function, so the objects made from now on are named.
  VulkanUtilities::SetupDebugUtils(instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                                   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, 0, nullptr, nullptr);
  const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
      vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
  if (!create) {
    REXLOG_ERROR("[native_x_renderer] device fault: no debug utils messenger");
    return;
  }
  VkDebugUtilsMessengerCreateInfoEXT ci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT;
  ci.pfnUserCallback = OnBindingReport;
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  const VkResult r = create(instance, &ci, nullptr, &messenger);
  REXLOG_INFO("[native_x_renderer] device fault: the address binding report {} (result {})",
              r == VK_SUCCESS ? "listened to" : "not available", int(r));
}

// D229: the breadcrumbs' memory (NXR_DEVICE_FAULT): 64 slots of two counts, host visible and coherent, mapped.
namespace {
VkBuffer g_marker_buffer = VK_NULL_HANDLE;
VkDeviceMemory g_marker_memory = VK_NULL_HANDLE;
volatile uint32_t* g_marker_data = nullptr;
std::atomic<uint32_t> g_marker_count{0};
const char* g_marker_names[64] = {};
}  // namespace

void Device::StartMarkers() {
  RefCntAutoPtr<IRenderDeviceVk> device_vk(device_, IID_RenderDeviceVk);
  if (!device_vk || !vkCmdWriteBufferMarkerAMD) {
    REXLOG_WARN("[native_x_renderer] device fault: no buffer markers (VK_AMD_buffer_marker)");
    return;
  }
  const VkDevice d = device_vk->GetVkDevice();
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = 64 * 2 * sizeof(uint32_t);
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (vkCreateBuffer(d, &bci, nullptr, &g_marker_buffer) != VK_SUCCESS) return;
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(d, g_marker_buffer, &req);
  VkPhysicalDeviceMemoryProperties props{};
  vkGetPhysicalDeviceMemoryProperties(device_vk->GetVkPhysicalDevice(), &props);
  uint32_t type = UINT32_MAX;
  const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for (uint32_t i = 0; i < props.memoryTypeCount && type == UINT32_MAX; ++i)
    if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) type = i;
  if (type == UINT32_MAX) return;
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type;
  if (vkAllocateMemory(d, &mai, nullptr, &g_marker_memory) != VK_SUCCESS) return;
  vkBindBufferMemory(d, g_marker_buffer, g_marker_memory, 0);
  void* mapped = nullptr;
  if (vkMapMemory(d, g_marker_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return;
  std::memset(mapped, 0, size_t(bci.size));
  g_marker_data = static_cast<volatile uint32_t*>(mapped);
  REXLOG_INFO("[native_x_renderer] device fault: breadcrumbs on (VK_AMD_buffer_marker, 64 passes)");
}

void Device::FaultMarker(IDeviceContext* context, int slot, bool begin, const char* name) {
  if (!g_marker_data || !context || slot < 0 || slot >= 64) return;
  RefCntAutoPtr<IDeviceContextVk> vk(context, IID_DeviceContextVk);
  if (!vk) return;
  const VkCommandBuffer cb = vk->GetVkCommandBuffer();
  if (!cb) return;
  g_marker_names[slot] = name;
  const uint32_t value = ++g_marker_count;
  vkCmdWriteBufferMarkerAMD(cb, begin ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            g_marker_buffer, VkDeviceSize(slot * 2 + (begin ? 0 : 1)) * sizeof(uint32_t), value);
}

// D229: the driver's report of why the device was lost (NXR_DEVICE_FAULT), once.
void Device::ReportFault() {
  if (!fault_report_ || fault_reported_ || !device_) return;
  fault_reported_ = true;
  RefCntAutoPtr<IRenderDeviceVk> device_vk(device_, IID_RenderDeviceVk);
  if (!device_vk || !vkGetDeviceProcAddr) return;
  const VkDevice vk = device_vk->GetVkDevice();
  const auto get = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(vkGetDeviceProcAddr(vk, "vkGetDeviceFaultInfoEXT"));
  if (!get) {
    REXLOG_ERROR("[native_x_renderer] device fault: vkGetDeviceFaultInfoEXT not there");
    return;
  }
  if (g_marker_data) {
    // Each pass's last start and end the GPU wrote; a start after its end: the GPU was in it.
    std::string in, last;
    uint32_t newest = 0;
    int newest_slot = -1;
    for (int s = 0; s < 64; ++s) {
      const uint32_t b = g_marker_data[s * 2], e = g_marker_data[s * 2 + 1];
      if (!b && !e) continue;
      if (b > e) in += fmt::format("{}\"{}\" (started #{}, last ended #{})", in.empty() ? "" : "; ",
                                   g_marker_names[s] ? g_marker_names[s] : "?", b, e);
      if (std::max(b, e) > newest) newest = std::max(b, e), newest_slot = s;
    }
    if (newest_slot >= 0)
      last = fmt::format("\"{}\" #{} (written {} of {})", g_marker_names[newest_slot] ? g_marker_names[newest_slot] : "?",
                         newest, g_marker_data[newest_slot * 2] == newest ? "start" : "end", g_marker_count.load());
    REXLOG_ERROR("[native_x_renderer] device fault: breadcrumbs: in progress on the GPU: {}; the newest written: {}",
                 in.empty() ? "none" : in, last.empty() ? "none" : last);
  }
  VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
  VkResult result = get(vk, &counts, nullptr);
  std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
  std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
  counts.vendorBinarySize = 0;
  VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
  info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
  info.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
  result = get(vk, &counts, &info);
  REXLOG_ERROR("[native_x_renderer] device fault (result {}): \"{}\"; {} addresses, {} vendor reports; races seen "
               "(NXR_RACE_CHECK): {} by the recorder's rule, {} by the guards, {} descriptor lifetimes (of {} checks; {} "
               "fences reset), {} dead objects bound or written ({} destroyed while in flight)",
               int(result), info.description, counts.addressInfoCount, counts.vendorInfoCount, g_race_count.load(),
               g_guard_races.load(), g_lifetime_races.load(), g_lifetime_checks.load(), g_fences_reset.load(),
               g_stale_races.load(), g_inflight_destroys.load());
  {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    if (!g_thread_counts.empty())
      REXLOG_ERROR("[native_x_renderer] device fault: the threads in Vulkan calls: {}", InventoryLine());
  }
  for (uint32_t i = 0; i < counts.addressInfoCount && i < addresses.size(); ++i) {
    const uint64_t at = addresses[i].reportedAddress, precision = std::max<uint64_t>(addresses[i].addressPrecision, 1);
    REXLOG_ERROR("[native_x_renderer] device fault: address {} type {} at 0x{:016X} (precision {})", i,
                 int(addresses[i].addressType), at, precision);
    // What was ever bound over that page (the latest first), and the nearest below it.
    const uint64_t lo = at & ~(precision - 1), hi = lo + precision;
    std::lock_guard<std::mutex> lock(g_binds_mutex);
    REXLOG_ERROR("[native_x_renderer] device fault: {} memory binds reported in all", g_binds.size());
    int shown = 0;
    for (auto it = g_binds.rbegin(); it != g_binds.rend() && shown < 12; ++it) {
      if (it->base < hi && it->base + it->size > lo) {
        ++shown;
        REXLOG_ERROR("[native_x_renderer] device fault:   #{} {} 0x{:016X}..0x{:016X} ({} bytes) object type {} 0x{:X} \"{}\"",
                     it->order, it->bound ? "bound" : "UNBOUND", it->base, it->base + it->size, it->size, it->type,
                     it->handle, it->name);
      }
    }
    const BindRecord* below = nullptr;
    for (const auto& b : g_binds)
      if (b.bound && b.base + b.size <= lo && (!below || b.base + b.size > below->base + below->size)) below = &b;
    if (below)
      REXLOG_ERROR("[native_x_renderer] device fault:   nearest below: #{} 0x{:016X}..0x{:016X} \"{}\" ({} bytes short)",
                   below->order, below->base, below->base + below->size, below->name, lo - (below->base + below->size));
    if (!shown) REXLOG_ERROR("[native_x_renderer] device fault:   nothing reported bound over that page");
  }
  for (uint32_t i = 0; i < counts.vendorInfoCount && i < vendors.size(); ++i)
    REXLOG_ERROR("[native_x_renderer] device fault: vendor {} \"{}\" code 0x{:X} data 0x{:X}", i, vendors[i].description,
                 uint64_t(vendors[i].vendorFaultCode), uint64_t(vendors[i].vendorFaultData));
}

bool Device::RemakeSwapChain() {
  // The frame's work submitted (its frame fence signal too, or WaitForFramesInFlight would wait for it forever) and the
  // GPU idle before the swap chain's images go.
  try {
    context_->Flush();
    device_->IdleGPU();
  } catch (const std::exception& e) {
    REXLOG_ERROR("[native_x_renderer] waiting for the GPU before the new swap chain: {}", e.what());
  }
  swap_chain_.Release();
  g_swap_chain_failed.store(false);
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (CreateSwapChain()) {
      g_swap_chain_failed.store(false);  // a failed attempt before this one reported itself
      return true;
    }
  }
  return false;
}

ITextureView* Device::BackBuffer() {
  if (g_swap_chain_failed.load() && !RebuildSwapChain("drawing", "it had failed before"))
    throw std::runtime_error("no swap chain on the game window");
  try {
    return swap_chain_->GetCurrentBackBufferRTV();
  } catch (const std::exception& e) {
    if (!RebuildSwapChain("acquiring an image", e.what())) throw;
  }
  return swap_chain_->GetCurrentBackBufferRTV();
}

bool Device::UpdateSize() {
#ifdef _WIN32
  RECT rect{};
  if (!GetClientRect(static_cast<HWND>(window_), &rect)) return false;
  uint32_t width = uint32_t(rect.right - rect.left), height = uint32_t(rect.bottom - rect.top);
#else
  // The window's size in pixels as the SDK tracks it (the surface has no size of its own on Wayland).
  const auto* host = static_cast<const HostWindow*>(window_);
  uint32_t width = swap_chain_->GetDesc().Width, height = swap_chain_->GetDesc().Height;
  if (host && host->pixel_size && host->pixel_size(false) && host->pixel_size(true)) {
    width = host->pixel_size(false), height = host->pixel_size(true);
  }
#endif
  if (!width || !height) return false;
  const auto& desc = swap_chain_->GetDesc();
  if (desc.Width == width && desc.Height == height) return true;
#ifndef _WIN32
  // D159: an X11 surface (XWayland too) has a size of its own, and DiligentCore makes the swap chain at that size
  // whatever it is asked. The SDK's size can be out of date: under XWayland on a scaled desktop, an SDL size event
  // lost in the exclusive Fullscreen's emulated mode switch left the SDK at the desktop's maximised 1038x506 while the
  // window was 1920x1080 (the ROG Ally at 185%, 3 game starts of 9 on 11 Nov 2026). Asking for it again every frame
  // rebuilt the swap chain 90 times a second, and the menus were laid out for the wrong size. A size the surface did
  // not take is not asked for again, and the SDK is told the surface's (an SDL size event of its own), so both agree
  // from then on.
  if (width == refused_width_ && height == refused_height_) return true;
  swap_chain_->Resize(width, height);
  if (g_swap_chain_failed.load()) {
    RebuildSwapChain("resizing", "see DiligentCore's line above");
    return true;
  }
  const auto& now = swap_chain_->GetDesc();
  if (now.Width != width || now.Height != height) {
    refused_width_ = width, refused_height_ = height;
    if (real_size_notes_ < 5) {
      ++real_size_notes_;
      REXLOG_WARN("[native_x_renderer] the window's size as the SDK has it, {}x{}, is not its surface's, {}x{}: the "
                  "surface's size is kept and given to the SDK (D159)",
                  width, height, now.Width, now.Height);
    }
    const auto* host = static_cast<const HostWindow*>(window_);
    if (host && host->real_pixel_size) host->real_pixel_size(now.Width, now.Height);
  } else {
    refused_width_ = refused_height_ = 0;
  }
#else
  swap_chain_->Resize(width, height);
  if (g_swap_chain_failed.load()) RebuildSwapChain("resizing", "see DiligentCore's line above");
#endif
  return true;
}

Device::FenceState Device::GetFenceState() {
  FenceState state{frame_, frame_fence_->GetCompletedValue(), 0, 0};
  if (ICommandQueue* queue = context_->LockCommandQueue()) {
    state.queue_next = queue->GetNextFenceValue();
    state.queue_completed = queue->GetCompletedFenceValue();
    context_->UnlockCommandQueue();
  }
  return state;
}

// The present modes for native_x_renderer_present_mode's value, in order; none: DiligentCore's for Present's sync
// interval (without V-Sync: MAILBOX, else IMMEDIATE, else FIFO).
// D107: V-Sync is FIFO, which every driver offers. NVIDIA's MAILBOX queued three frames whenever the game ran at the
// display's rate (16-19 ms from present to display at 160 Hz, FIFO 5.8 ms).
static std::vector<VkPresentModeKHR> PresentModesFor(const std::string& mode, bool vsync_mailbox) {
  if (mode == "vsync" && vsync_mailbox) return {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR};
  if (mode == "vsync") return {VK_PRESENT_MODE_FIFO_KHR};
  if (mode == "mailbox") return {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR};
  if (mode == "immediate") return {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR};
  if (!mode.empty())
    REXLOG_WARN("[native_x_renderer] native_x_renderer_present_mode \"{}\" is not one: the default", mode);
  return {};
}

static const char* PresentModeText(const std::string& mode, bool vsync_mailbox) {
  return mode == "vsync" && vsync_mailbox ? "vsync on Linux (MAILBOX at the frame limit, else FIFO)"
         : mode == "vsync"     ? "vsync (FIFO)"
         : mode == "mailbox"   ? "mailbox (MAILBOX, else FIFO)"
         : mode == "immediate" ? "immediate (IMMEDIATE, else MAILBOX)"
                               : "default (MAILBOX, else IMMEDIATE)";
}

static std::atomic<int> g_vsync{-1};
void SetVsync(bool on) { g_vsync.store(on ? 1 : 0); }

std::string Device::WantedPresentMode() const {
  const int vsync = g_vsync.load();
  if (force_vsync_ || vsync == 1) return "vsync";
  return vsync == 0 ? "immediate" : REXCVAR_GET(native_x_renderer_present_mode);
}

void Device::ApplyPresentMode() {
  const std::string mode = WantedPresentMode();
  if (present_mode_applied_ && mode == present_mode_) return;
  REXLOG_INFO("[native_x_renderer] present mode changed to {}: a new swap chain", PresentModeText(mode, VsyncMailbox()));
  present_mode_applied_ = true;
  present_mode_ = mode;
  vsync_fallback_ = false;
  if (!RemakeSwapChain() && !RebuildSwapChain("changing the present mode", nullptr))
    throw std::runtime_error("no swap chain");
}

double Device::Present(double* submit_ms) {
  context_->EnqueueSignal(frame_fence_, ++frame_);
  static const bool split = [] {
    const char* v = std::getenv("NXR_PRESENT_SPLIT");
    return v && *v == '1';
  }();
  if (split) {
    const auto start = std::chrono::steady_clock::now();
    context_->Flush();
    if (submit_ms)
      *submit_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  }
  if (g_swap_chain_failed.load()) {
    // The swap chain failed since the last present (a resize DiligentCore kept to itself): this frame is not shown.
    if (!RebuildSwapChain("presenting", "it had failed before")) throw std::runtime_error("no swap chain");
    return WaitForFramesInFlight();
  }
  try {
    DumpFrame();
    // V-Sync off: the game paces itself (above). After a swap chain failure, on (issue 26, RebuildSwapChain).
    swap_chain_->Present(vsync_fallback_ ? 1 : 0);
  } catch (const std::exception& e) {
    // The frame is lost; the next one goes to the new swap chain.
    if (!RebuildSwapChain("presenting", e.what())) throw;
  }
  ApplyPresentMode();
  return WaitForFramesInFlight();
}

// NXR_FRAME_DUMP=<dir>,<seconds>,<frames>[,<step>]: from that many seconds after the first present, that many presented
// frames in a row written to <dir>\<n>.bgra (n from 0): two 32-bit words, the width and height, then BGRA rows of
// every <step>th pixel of every <step>th row (2 by default). For checks frame by frame (a one frame flash); each frame
// waits for the GPU to finish, so the frame rate falls while it runs.
void Device::DumpFrame() {
  struct Config {
    std::string dir;
    double seconds = -1;
    int frames = 0, step = 2;
  };
  static const Config config = [] {
    Config c;
    const char* v = std::getenv("NXR_FRAME_DUMP");
    if (!v || !*v) return c;
    std::string text = v;
    std::vector<std::string> parts;
    for (size_t at = 0;;) {
      const size_t comma = text.find(',', at);
      parts.push_back(text.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
      if (comma == std::string::npos) break;
      at = comma + 1;
    }
    if (parts.size() < 3) return c;
    c.dir = parts[0];
    c.seconds = std::atof(parts[1].c_str());
    c.frames = std::atoi(parts[2].c_str());
    if (parts.size() > 3) c.step = std::max(1, std::atoi(parts[3].c_str()));
    return c;
  }();
  if (config.seconds < 0 || config.frames <= 0) return;
  static const auto start = std::chrono::steady_clock::now();
  static int dumped = 0;
  if (dumped >= config.frames ||
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < config.seconds)
    return;
  ITextureView* view = swap_chain_->GetCurrentBackBufferRTV();
  ITexture* back = view ? view->GetTexture() : nullptr;
  if (!back) return;
  const TextureDesc& desc = back->GetDesc();
  if (!dump_staging_ || dump_staging_->GetDesc().Width != desc.Width ||
      dump_staging_->GetDesc().Height != desc.Height) {
    TextureDesc staging = desc;
    staging.Name = "NativeXRenderer frame dump";
    staging.Usage = USAGE_STAGING;
    staging.BindFlags = BIND_NONE;
    staging.CPUAccessFlags = CPU_ACCESS_READ;
    staging.MiscFlags = MISC_TEXTURE_FLAG_NONE;
    dump_staging_.Release();
    device_->CreateTexture(staging, nullptr, &dump_staging_);
    if (!dump_staging_) return;
  }
  CopyTextureAttribs copy(back, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, dump_staging_,
                          RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context_->CopyTexture(copy);
  context_->WaitForIdle();
  MappedTextureSubresource mapped;
  context_->MapTextureSubresource(dump_staging_, 0, 0, MAP_READ, MAP_FLAG_NONE, nullptr, mapped);
  if (mapped.pData) {
    const uint32_t step = uint32_t(config.step);
    const uint32_t w = desc.Width / step, h = desc.Height / step;
    std::vector<uint8_t> out(8 + size_t(w) * h * 4);
    std::memcpy(out.data(), &w, 4);
    std::memcpy(out.data() + 4, &h, 4);
    const bool rgba = desc.Format == TEX_FORMAT_RGBA8_UNORM || desc.Format == TEX_FORMAT_RGBA8_UNORM_SRGB;
    for (uint32_t y = 0; y < h; ++y) {
      const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * step * mapped.Stride;
      uint8_t* dst = out.data() + 8 + size_t(y) * w * 4;
      for (uint32_t x = 0; x < w; ++x) {
        const uint8_t* px = row + size_t(x) * step * 4;
        dst[x * 4 + 0] = rgba ? px[2] : px[0];
        dst[x * 4 + 1] = px[1];
        dst[x * 4 + 2] = rgba ? px[0] : px[2];
        dst[x * 4 + 3] = px[3];
      }
    }
    REXLOG_INFO("[native_x_renderer] frame dump {} (present {})", dumped, frame_ + 1);
    const std::string path = config.dir + "/" + std::to_string(dumped) + ".bgra";
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(out.data(), 1, out.size(), f);
      std::fclose(f);
    }
  }
  context_->UnmapTextureSubresource(dump_staging_, 0, 0);
  if (++dumped == config.frames) REXLOG_INFO("[native_x_renderer] frame dump: {} frames in {}", dumped, config.dir);
}

double Device::FinishHiddenFrame() {
  context_->EnqueueSignal(frame_fence_, ++frame_);
  context_->Flush();
  context_->FinishFrame();
  device_->ReleaseStaleResources();
  return WaitForFramesInFlight();
}

double Device::WaitForFramesInFlight() {
  if (frame_ <= kMaxFramesInFlight) return 0.0;
  auto start = std::chrono::steady_clock::now();
  frame_fence_->Wait(frame_ - kMaxFramesInFlight);
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace native_x_renderer
