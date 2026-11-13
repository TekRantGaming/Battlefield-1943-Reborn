// NativeXRenderer - see device.h.

#include "device.h"
#include "xess.h"

#include "queries.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <string>

#include "EngineFactoryVk.h"
#include "CommandQueue.h"
#include "Fence.h"
#include "NativeWindow.h"
// DiligentCore's Vulkan interfaces take Vulkan's types from the includer (its functions are loaded by volk).
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>
#include "SwapChainVk.h"
#include "RenderDeviceVk.h"
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
      break;
  }
}

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
  create_info.DynamicHeapSize = 256 << 20;
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
  if (!device_ || !context_) {
    REXLOG_ERROR("[native_x_renderer] could not create the Vulkan device");
    return false;
  }
  if (name_objects) {
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

  SwapChainDesc swap_chain_desc;
  // The guest's front buffer is 8-bit RGBA, already gamma-corrected by the game: no sRGB conversion
  // on the way out.
  swap_chain_desc.ColorBufferFormat = TEX_FORMAT_RGBA8_UNORM;
  swap_chain_desc.DepthBufferFormat = TEX_FORMAT_UNKNOWN;
  swap_chain_desc.BufferCount = 3;
  if (std::getenv("NXR_FRAME_DUMP")) swap_chain_desc.Usage |= SWAP_CHAIN_USAGE_COPY_SOURCE;  // DumpFrame
#ifdef _WIN32
  Win32NativeWindow window{static_cast<HWND>(native_window)};
#else
  // Linux: Wayland when the window has a Wayland surface, else X11 (Xlib: display and window id).
  const auto* host = static_cast<const HostWindow*>(native_window);
  NativeWindow window{};
  window.pDisplay = host->display;
  window.pWaylandSurface = host->wayland_surface;
  window.WindowId = host->x11_window;
#endif
  factory->CreateSwapChainVk(device_, context_, swap_chain_desc, window, &swap_chain_);
  if (!swap_chain_) {
    REXLOG_ERROR("[native_x_renderer] could not create the swap chain on the game window");
    return false;
  }

  const auto& adapter = device_->GetAdapterInfo();
  const auto& desc = swap_chain_->GetDesc();
  REXLOG_INFO("[native_x_renderer] Vulkan device on {}; swap chain {}x{}", adapter.Description, desc.Width,
              desc.Height);
  return true;
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

void Device::ApplyPresentMode() {
  const std::string& mode = force_vsync_ ? std::string("vsync") : REXCVAR_GET(native_x_renderer_present_mode);
  if (present_mode_applied_ && mode == present_mode_) return;
  present_mode_applied_ = true;
  present_mode_ = mode;
  RefCntAutoPtr<ISwapChainVk> swap_chain_vk(swap_chain_, IID_SwapChainVk);
  if (!swap_chain_vk) return;
  // D107: V-Sync is FIFO, which every driver offers. NVIDIA's MAILBOX queued three frames whenever the game ran at the
  // display's rate (16-19 ms from present to display at 160 Hz, FIFO 5.8 ms).
  static constexpr VkPresentModeKHR kVsync[] = {VK_PRESENT_MODE_FIFO_KHR};
  static constexpr VkPresentModeKHR kMailbox[] = {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR};
  static constexpr VkPresentModeKHR kImmediate[] = {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
                                                    VK_PRESENT_MODE_FIFO_KHR};
  if (mode == "vsync") {
    swap_chain_vk->SetPresentModes(kVsync, Uint32(std::size(kVsync)));
  } else if (mode == "mailbox") {
    swap_chain_vk->SetPresentModes(kMailbox, Uint32(std::size(kMailbox)));
  } else if (mode == "immediate") {
    swap_chain_vk->SetPresentModes(kImmediate, Uint32(std::size(kImmediate)));
  } else {
    if (!mode.empty()) {
      REXLOG_WARN("[native_x_renderer] native_x_renderer_present_mode \"{}\" is not one: the default", mode);
    }
    swap_chain_vk->SetPresentModes(nullptr, 0);
  }
  REXLOG_INFO("[native_x_renderer] present mode: {}", mode == "vsync"       ? "vsync (FIFO)"
                                                      : mode == "mailbox"   ? "mailbox (MAILBOX, else FIFO)"
                                                      : mode == "immediate" ? "immediate (IMMEDIATE, else MAILBOX)"
                                                                            : "default (MAILBOX, else IMMEDIATE)");
}

double Device::Present(double* submit_ms) {
  ApplyPresentMode();
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
  DumpFrame();
  swap_chain_->Present(0);
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
