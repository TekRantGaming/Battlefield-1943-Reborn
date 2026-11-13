// NativeXRenderer - see xess.h.

#include "xess.h"

#include <rex/logging.h>

#include <algorithm>
#include <string>
#include <vector>

#if NATIVE_X_RENDERER_XESS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
// DiligentCore's Vulkan interfaces need the Vulkan types first.
#include <vulkan/vulkan.h>

#include "DeviceContextVk.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDeviceVk.h"
#include "TextureViewVk.h"
#include "TextureVk.h"

#if NATIVE_X_RENDERER_XESS
#include "xess/xess/xess_vk.h"
#endif

namespace native_x_renderer {

using namespace Diligent;

#if NATIVE_X_RENDERER_XESS
namespace {

HMODULE g_module = nullptr;  // libxess.dll (never unloaded)
template <typename T>
T Entry(const char* name) {
  return g_module ? reinterpret_cast<T>(GetProcAddress(g_module, name)) : nullptr;
}

bool LoadModule() {
  if (g_module) return true;
  wchar_t path[MAX_PATH] = {};
  DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring folder(path, length);
  folder.resize(folder.find_last_of(L"\\/") + 1);
  g_module = LoadLibraryW((folder + L"libxess.dll").c_str());
  if (!g_module) REXLOG_WARN("[native_x_renderer] XeSS: libxess.dll is not next to the game's executable");
  return g_module != nullptr;
}

// What the device was made with for XeSS (Xess::PrepareDevice), kept for the process.
std::vector<const char*> g_instance_extensions, g_device_extensions;
bool g_prepared = false;

VkFormat Format(TEXTURE_FORMAT format) {
  switch (format) {
    case TEX_FORMAT_RGBA16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TEX_FORMAT_RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case TEX_FORMAT_RGBA8_UNORM_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case TEX_FORMAT_BGRA8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    case TEX_FORMAT_RGB10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case TEX_FORMAT_R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case TEX_FORMAT_RG16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case TEX_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
  }
}

xess_vk_image_view_info Image(ITexture* texture, bool output) {
  xess_vk_image_view_info info{};
  const TextureDesc& desc = texture->GetDesc();
  RefCntAutoPtr<ITextureVk> texture_vk(texture, IID_TextureVk);
  ITextureView* view = texture->GetDefaultView(output ? TEXTURE_VIEW_UNORDERED_ACCESS : TEXTURE_VIEW_SHADER_RESOURCE);
  RefCntAutoPtr<ITextureViewVk> view_vk(view, IID_TextureViewVk);
  info.image = texture_vk ? texture_vk->GetVkImage() : VK_NULL_HANDLE;
  info.imageView = view_vk ? view_vk->GetVulkanImageView() : VK_NULL_HANDLE;
  info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  info.format = Format(desc.Format);
  info.width = desc.Width;
  info.height = desc.Height;
  return info;
}

bool IsFloatFormat(TEXTURE_FORMAT format) {
  return format == TEX_FORMAT_RGBA16_FLOAT || format == TEX_FORMAT_RGBA32_FLOAT || format == TEX_FORMAT_R11G11B10_FLOAT;
}

}  // namespace
#endif

Xess::~Xess() {
  // As FSR's: the context is not destroyed at the process's end, the device may be gone already.
}

bool Xess::PrepareDevice(EngineVkCreateInfo& create_info) {
#if NATIVE_X_RENDERER_XESS
  if (!LoadModule()) return false;
  auto instance_extensions = Entry<xess_result_t (*)(uint32_t*, const char* const**, uint32_t*)>(
      "xessVKGetRequiredInstanceExtensions");
  auto device_extensions =
      Entry<xess_result_t (*)(VkInstance, VkPhysicalDevice, uint32_t*, const char* const**)>(
          "xessVKGetRequiredDeviceExtensions");
  HMODULE vulkan = GetModuleHandleW(L"vulkan-1.dll");
  if (!vulkan) vulkan = LoadLibraryW(L"vulkan-1.dll");
  if (!instance_extensions || !device_extensions || !vulkan) return false;
  uint32_t count = 0, api = 0;
  const char* const* names = nullptr;
  if (instance_extensions(&count, &names, &api) != XESS_RESULT_SUCCESS) {
    REXLOG_WARN("[native_x_renderer] XeSS: it does not say which Vulkan instance extensions it needs");
    return false;
  }
  g_instance_extensions.assign(create_info.ppInstanceExtensionNames,
                               create_info.ppInstanceExtensionNames + create_info.InstanceExtensionCount);
  std::string said;
  for (uint32_t i = 0; i < count; ++i) g_instance_extensions.push_back(names[i]), said += std::string(" ") + names[i];
  // The device extensions are asked of a Vulkan instance and the GPU: an instance of our own for the question (the
  // device's is made inside DiligentCore, with the device).
  auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(GetProcAddress(vulkan, "vkCreateInstance"));
  auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(GetProcAddress(vulkan, "vkDestroyInstance"));
  auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(GetProcAddress(vulkan, "vkEnumeratePhysicalDevices"));
  auto properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(GetProcAddress(vulkan, "vkGetPhysicalDeviceProperties"));
  if (!create_instance || !destroy_instance || !enumerate || !properties) return false;
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = std::max<uint32_t>(api, VK_API_VERSION_1_1);
  VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pApplicationInfo = &app;
  info.enabledExtensionCount = count;
  info.ppEnabledExtensionNames = names;
  VkInstance instance = VK_NULL_HANDLE;
  if (create_instance(&info, nullptr, &instance) != VK_SUCCESS || !instance) {
    REXLOG_WARN("[native_x_renderer] XeSS: no Vulkan instance with its extensions ({})", said);
    return false;
  }
  uint32_t gpus = 0;
  enumerate(instance, &gpus, nullptr);
  std::vector<VkPhysicalDevice> devices(gpus);
  if (gpus) enumerate(instance, &gpus, devices.data());
  // DiligentCore's choice: the adapter asked for, else a discrete GPU before the others.
  uint32_t chosen = 0;
  if (create_info.AdapterId < gpus) {
    chosen = create_info.AdapterId;
  } else {
    for (uint32_t i = 0; i < gpus; ++i) {
      VkPhysicalDeviceProperties p{};
      properties(devices[i], &p);
      if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        chosen = i;
        break;
      }
    }
  }
  uint32_t device_count = 0;
  const char* const* device_names = nullptr;
  const xess_result_t result =
      gpus ? device_extensions(instance, devices[chosen], &device_count, &device_names) : XESS_RESULT_ERROR_DEVICE;
  // The device features it needs: a chain of Vulkan feature structures of the library's own, given to DiligentCore
  // after its own (only where the game gives none itself).
  void* features = nullptr;
  std::string said_features;
  if (result == XESS_RESULT_SUCCESS && !create_info.pDeviceExtensionFeatures) {
    if (auto get = Entry<xess_result_t (*)(VkInstance, VkPhysicalDevice, void**)>("xessVKGetRequiredDeviceFeatures");
        get && get(instance, devices[chosen], &features) == XESS_RESULT_SUCCESS) {
      for (auto* s = static_cast<const VkBaseInStructure*>(features); s; s = s->pNext)
        said_features += " " + std::to_string(int(s->sType));
    } else {
      features = nullptr;
    }
  }
  destroy_instance(instance, nullptr);
  if (result != XESS_RESULT_SUCCESS) {
    REXLOG_WARN("[native_x_renderer] XeSS: it does not run on this GPU (error {})", int(result));
    return false;
  }
  g_device_extensions.assign(create_info.ppDeviceExtensionNames,
                             create_info.ppDeviceExtensionNames + create_info.DeviceExtensionCount);
  std::string said_device;
  for (uint32_t i = 0; i < device_count; ++i)
    g_device_extensions.push_back(device_names[i]), said_device += std::string(" ") + device_names[i];
  create_info.ppInstanceExtensionNames = g_instance_extensions.data();
  create_info.InstanceExtensionCount = uint32_t(g_instance_extensions.size());
  create_info.ppDeviceExtensionNames = g_device_extensions.data();
  create_info.DeviceExtensionCount = uint32_t(g_device_extensions.size());
  if (features) create_info.pDeviceExtensionFeatures = features;
  g_prepared = true;
  REXLOG_INFO("[native_x_renderer] XeSS: the Vulkan device is made with its extensions: instance{}; device{}; feature "
              "structures (their Vulkan types){}",
              said.empty() ? " none" : said, said_device.empty() ? " none" : said_device,
              said_features.empty() ? " none" : said_features);
  return true;
#else
  (void)create_info;
  return false;
#endif
}

void Xess::DeviceFailed() {
#if NATIVE_X_RENDERER_XESS
  g_prepared = false;
#endif
}

bool Xess::Load(IRenderDevice* device) {
  device_ = device;
#if NATIVE_X_RENDERER_XESS
  if (!g_prepared) {
    REXLOG_WARN("[native_x_renderer] XeSS: the Vulkan device was not made for it");
    return false;
  }
  return LoadModule() && Entry<void*>("xessVKCreateContext") && Entry<void*>("xessVKInit") &&
         Entry<void*>("xessVKExecute");
#else
  REXLOG_INFO("[native_x_renderer] XeSS: not in this build");
  return false;
#endif
}

bool Xess::Dispatch(IDeviceContext* context, const Inputs& in, int quality) {
#if NATIVE_X_RENDERER_XESS
  if (!g_module || !in.color || !in.depth || !in.motion || !in.output) return false;
  const TextureDesc& color = in.color->GetDesc();
  const TextureDesc& output = in.output->GetDesc();
  if (Format(color.Format) == VK_FORMAT_UNDEFINED || Format(output.Format) == VK_FORMAT_UNDEFINED) {
    REXLOG_ERROR("[native_x_renderer] XeSS: a format it is not given here (color {}, output {})", int(color.Format),
                 int(output.Format));
    return false;
  }
  RefCntAutoPtr<IRenderDeviceVk> device(device_, IID_RenderDeviceVk);
  RefCntAutoPtr<IDeviceContextVk> context_vk(context, IID_DeviceContextVk);
  if (!device || !context_vk) return false;
  const bool hdr = IsFloatFormat(color.Format);
  bool reset = in.reset;
  if (!context_ || size_[0] != output.Width || size_[1] != output.Height || hdr_ != hdr || quality_ != quality) {
    if (context_) {
      device_->IdleGPU();
      Entry<xess_result_t (*)(xess_context_handle_t)>("xessDestroyContext")(
          static_cast<xess_context_handle_t>(context_));
      context_ = nullptr;
    }
    xess_context_handle_t handle = nullptr;
    xess_result_t result =
        Entry<xess_result_t (*)(VkInstance, VkPhysicalDevice, VkDevice, xess_context_handle_t*)>("xessVKCreateContext")(
            device->GetVkInstance(), device->GetVkPhysicalDevice(), device->GetVkDevice(), &handle);
    if (result < 0 || !handle) {
      REXLOG_ERROR("[native_x_renderer] XeSS: no context (error {})", int(result));
      return false;
    }
    // Our modes are AMD's ratios (1, 1.5, 1.7, 2, 3): XeSS's settings with those ratios.
    static constexpr xess_quality_settings_t kQuality[] = {XESS_QUALITY_SETTING_AA, XESS_QUALITY_SETTING_ULTRA_QUALITY,
                                                           XESS_QUALITY_SETTING_QUALITY, XESS_QUALITY_SETTING_BALANCED,
                                                           XESS_QUALITY_SETTING_ULTRA_PERFORMANCE};
    xess_vk_init_params_t init{};
    init.outputResolution = {output.Width, output.Height};
    init.qualitySetting = kQuality[std::clamp(quality, 0, 4)];
    // A float scene (before the game's post-processing) is HDR, exposed by XeSS; an 8- or 10-bit one is LDR.
    init.initFlags = hdr ? XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE : XESS_INIT_FLAG_LDR_INPUT_COLOR;
    result = Entry<xess_result_t (*)(xess_context_handle_t, const xess_vk_init_params_t*)>("xessVKInit")(handle, &init);
    if (result < 0) {
      REXLOG_ERROR("[native_x_renderer] XeSS: it could not start for {}x{} (error {})", output.Width, output.Height,
                   int(result));
      Entry<xess_result_t (*)(xess_context_handle_t)>("xessDestroyContext")(handle);
      return false;
    }
    context_ = handle;
    size_[0] = output.Width, size_[1] = output.Height, hdr_ = hdr, quality_ = quality;
    reset = true;
    xess_version_t version{};
    if (auto get = Entry<xess_result_t (*)(xess_version_t*)>("xessGetVersion")) get(&version);
    REXLOG_INFO("[native_x_renderer] XeSS {}.{}.{} runs, {}x{} to {}x{}{}{}", version.major, version.minor,
                version.patch, color.Width, color.Height, output.Width, output.Height, hdr ? ", HDR" : ", LDR",
                result == XESS_RESULT_WARNING_OLD_DRIVER ? " (it asks for a newer graphics driver)" : "");
  }
  StateTransitionDesc barriers[] = {
      {in.color, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_SHADER_RESOURCE, STATE_TRANSITION_FLAG_UPDATE_STATE},
      {in.depth, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_SHADER_RESOURCE, STATE_TRANSITION_FLAG_UPDATE_STATE},
      {in.motion, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_SHADER_RESOURCE, STATE_TRANSITION_FLAG_UPDATE_STATE},
      {in.output, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_UNORDERED_ACCESS, STATE_TRANSITION_FLAG_UPDATE_STATE},
  };
  context->TransitionResourceStates(uint32_t(std::size(barriers)), barriers);
  xess_vk_execute_params_t exec{};
  exec.colorTexture = Image(in.color, false);
  exec.velocityTexture = Image(in.motion, false);
  exec.depthTexture = Image(in.depth, false);
  exec.outputTexture = Image(in.output, true);
  exec.jitterOffsetX = in.jitter_x;
  exec.jitterOffsetY = in.jitter_y;
  exec.exposureScale = 1.0f;
  exec.resetHistory = reset ? 1 : 0;
  exec.inputWidth = color.Width;
  exec.inputHeight = color.Height;
  const xess_result_t result =
      Entry<xess_result_t (*)(xess_context_handle_t, VkCommandBuffer, const xess_vk_execute_params_t*)>(
          "xessVKExecute")(static_cast<xess_context_handle_t>(context_), context_vk->GetVkCommandBuffer(), &exec);
  if (result < 0) {
    REXLOG_ERROR("[native_x_renderer] XeSS: the upscale failed (error {})", int(result));
    return false;
  }
  return true;
#else
  (void)context;
  (void)in;
  (void)quality;
  return false;
#endif
}

}  // namespace native_x_renderer
