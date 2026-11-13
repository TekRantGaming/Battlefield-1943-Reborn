# NativeXRenderer - native renderer for Xbox 360 games recompiled with the ReXGlue SDK (see README.md).
#
# Phase 1: the trace (src/trace.cpp) and the observer (src/observer.cpp, with the SDK's draw observer
# of its recomp branch, D89). Phase 2: DiligentCore (Vulkan) and the renderer (docs/plan.md). This repository
# contains no game-specific code or addresses (docs/decisions.md, D3); a game adds it as a submodule
# (docs/integrating-a-game.md).
#
# A game's hook definitions (NATIVE_X_RENDERER_HOOK, include/native_x_renderer/hooks.h) are compiled into the
# game's executable, never into this static library: the generated functions are weak, and a strong
# definition inside an archive member is not guaranteed to be pulled in when the weak one is already
# defined.
#
# Add this repository after the ReXGlue SDK (the game's generated/rexglue.cmake): DiligentCore reuses the
# SDK's Vulkan-Headers, SPIRV-Headers and xxHash targets instead of defining its own.

# ---------------------------------------------------------------------------------------------------
# DiligentCore: the Vulkan backend only.
#
# The integration problems Hell's Gate documented (docs/credits.md), and what is done about them:
# - glslang and SPIRV-Tools: the SDK builds glslang (its targets would collide), and NativeXRenderer gives
#   Diligent SPIR-V from the SDK's translator, never GLSL or HLSL source: DILIGENT_NO_GLSLANG and
#   DILIGENT_NO_HLSL, which also skip SPIRV-Tools.
# - AVX2: Diligent adds -mavx2 to its release builds through a normal variable that a cache value
#   cannot override; the project builds for x86-64-v2, so the flag is removed from
#   Diligent-BuildSettings after the subdirectory is added.
# - VSync and BC1: handled where the swap chain and the texture formats are created (src/).
set(NATIVE_X_RENDERER_DILIGENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/diligent-core" CACHE PATH
    "DiligentCore source tree (a submodule of this repository)")
foreach(_option
    DILIGENT_NO_DIRECT3D11 DILIGENT_NO_DIRECT3D12 DILIGENT_NO_OPENGL DILIGENT_NO_METAL DILIGENT_NO_WEBGPU
    DILIGENT_NO_ARCHIVER DILIGENT_NO_GLSLANG DILIGENT_NO_HLSL DILIGENT_NO_DSR DILIGENT_NO_FSR)
  set(${_option} ON CACHE BOOL "Set by native_x_renderer" FORCE)
endforeach()
# DLSS (Phase 5, D46): DiligentCore's SuperResolution module, with NVIDIA's NGX SDK and DLSS runtime fetched
# when configuring (DiligentGraphics/DLSS-Headers). DirectSR (D3D12) and FSR 1 (its shaders need the HLSL
# compiler) are left out. The module enables DLSS only with a Windows SDK of 10.0.26100 or later, which it
# reads from the WindowsSDKVersion variable that Visual Studio's prompts set; these Ninja builds have none,
# so the newest installed SDK is passed that way.
if(WIN32 AND NOT DEFINED ENV{WindowsSDKVersion})
  cmake_host_system_information(RESULT _nxr_kits_root QUERY WINDOWS_REGISTRY
      "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots" VALUE KitsRoot10)
  if(NOT _nxr_kits_root)
    set(_nxr_kits_root "C:/Program Files (x86)/Windows Kits/10")
  endif()
  file(GLOB _nxr_sdk_versions RELATIVE "${_nxr_kits_root}/Include" "${_nxr_kits_root}/Include/10.*")
  if(_nxr_sdk_versions)
    list(SORT _nxr_sdk_versions COMPARE NATURAL ORDER DESCENDING)
    list(GET _nxr_sdk_versions 0 _nxr_sdk_version)
    set(ENV{WindowsSDKVersion} "${_nxr_sdk_version}")
  endif()
endif()
set(DILIGENT_NO_SUPER_RESOLUTION OFF CACHE BOOL "Set by native_x_renderer" FORCE)  # DLSS, below
set(DILIGENT_BUILD_TESTS OFF CACHE BOOL "Set by native_x_renderer" FORCE)
set(DILIGENT_INSTALL_CORE OFF CACHE BOOL "Set by native_x_renderer" FORCE)
set(DILIGENT_NO_FORMAT_VALIDATION ON CACHE BOOL "Set by native_x_renderer" FORCE)
# NativeXRenderer's changes to DiligentCore (diligent_patches/, decision D22), applied before it is added.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply_diligent_patches.cmake")
add_subdirectory("${NATIVE_X_RENDERER_DILIGENT_DIR}" "${CMAKE_BINARY_DIR}/diligent-core" EXCLUDE_FROM_ALL)

get_target_property(_diligent_options Diligent-BuildSettings INTERFACE_COMPILE_OPTIONS)
if(_diligent_options)
  list(TRANSFORM _diligent_options REPLACE "-mavx2" "")
  list(TRANSFORM _diligent_options REPLACE "/arch:AVX2" "")
  set_property(TARGET Diligent-BuildSettings PROPERTY INTERFACE_COMPILE_OPTIONS "${_diligent_options}")
endif()

# ---------------------------------------------------------------------------------------------------
# The SDK's graphics code NativeXRenderer uses in process (decision D15): the shader translator to SPIR-V,
# the register, texture and draw utilities. The SDK compiles it only into its GPU plugin, with hidden
# symbols. It is compiled again here, except the cvar definitions, which stay the plugin's
# (src/sdk_cvars.cpp explains why and how).
set(_nxr_sdk_graphics "${REXSDK_DIR}/src/graphics")
add_library(native_x_renderer_sdk STATIC
    ${_nxr_sdk_graphics}/register_file.cpp
    ${_nxr_sdk_graphics}/registers.cpp
    ${_nxr_sdk_graphics}/xenos.cpp
    ${_nxr_sdk_graphics}/sampler_info.cpp
    ${_nxr_sdk_graphics}/format/ucode.cpp
    ${_nxr_sdk_graphics}/pipeline/shader/shader.cpp
    ${_nxr_sdk_graphics}/pipeline/shader/translator.cpp
    ${_nxr_sdk_graphics}/pipeline/shader/translator_disasm.cpp
    ${_nxr_sdk_graphics}/pipeline/shader/spirv.cpp
    ${_nxr_sdk_graphics}/pipeline/shader/spirv_builder.cpp
    ${_nxr_sdk_graphics}/pipeline/shader/spirv_translator.cpp
    ${_nxr_sdk_graphics}/pipeline/shader/spirv_translator_alu.cpp
    ${_nxr_sdk_graphics}/pipeline/shader/spirv_translator_fetch.cpp
    ${_nxr_sdk_graphics}/pipeline/shader/spirv_translator_memexport.cpp
    ${_nxr_sdk_graphics}/pipeline/shader/spirv_translator_rb.cpp
    ${_nxr_sdk_graphics}/pipeline/texture/info.cpp
    ${_nxr_sdk_graphics}/pipeline/texture/info_formats.cpp
    ${_nxr_sdk_graphics}/pipeline/texture/util.cpp
    ${_nxr_sdk_graphics}/pipeline/texture/conversion.cpp
    ${_nxr_sdk_graphics}/pipeline/texture/extent.cpp
    ${_nxr_sdk_graphics}/shared_memory.cpp
    src/sdk_cvars.cpp
    src/sdk_draw.cpp
    src/sdk_primitive_processor.cpp
    src/sdk_texture_cache.cpp
    # glslang's default resource limits, for NativeXRenderer's utility shaders (src/glsl.cpp, decision D16).
    ${REXSDK_DIR}/thirdparty/glslang/StandAlone/ResourceLimits.cpp
)
target_compile_definitions(native_x_renderer_sdk PRIVATE
    NATIVE_X_RENDERER_SDK_DRAW_CPP="${_nxr_sdk_graphics}/util/draw.cpp"
    NATIVE_X_RENDERER_SDK_PRIMITIVE_PROCESSOR_CPP="${_nxr_sdk_graphics}/primitive_processor.cpp"
    NATIVE_X_RENDERER_SDK_TEXTURE_CACHE_CPP="${_nxr_sdk_graphics}/pipeline/texture/cache.cpp")
target_compile_features(native_x_renderer_sdk PUBLIC cxx_std_23)
target_compile_options(native_x_renderer_sdk PRIVATE -Wno-unused-variable)
# The same third-party pieces the plugin compiles these files with.
target_include_directories(native_x_renderer_sdk PRIVATE "${REXSDK_DIR}")
# The SDK's graphics headers include renderdoc_app.h (through rex/ui) and glslang's.
target_include_directories(native_x_renderer_sdk PUBLIC "${REXSDK_DIR}/thirdparty/renderdoc" "${REXSDK_DIR}/thirdparty/glslang")
target_link_libraries(native_x_renderer_sdk PUBLIC rex::runtime Vulkan::Headers glslang glslang::SPIRV
    PRIVATE xxHash::xxhash)
rexglue_apply_target_settings(native_x_renderer_sdk)

# ---------------------------------------------------------------------------------------------------
# The library.

add_library(native_x_renderer STATIC
    src/native_x_renderer.cpp
    src/trace.cpp
    src/observer.cpp
    src/state.cpp
    src/renderer.cpp
    src/commands.cpp
    src/device.cpp
    src/targets.cpp
    src/blit.cpp
    src/overlay.cpp
    src/aa.cpp
    src/grass.cpp
    src/gtao.cpp
    src/ssgi.cpp
    src/scaling.cpp
    src/upscaler.cpp
    src/framegen.cpp
    src/overlay_motion.cpp
    src/fsr.cpp
    src/taa.cpp
    src/xess.cpp
    src/motion.cpp
    src/camera_vote.cpp
    src/glsl.cpp
    src/guest_memory.cpp
    src/primitives.cpp
    src/shaders.cpp
    src/spirv_patch.cpp
    src/pipelines.cpp
    src/textures.cpp
    src/gamma.cpp
    src/guest_textures.cpp
    src/queries.cpp
    src/storage.cpp
    src/draw.cpp
)
add_library(native_x_renderer::native_x_renderer ALIAS native_x_renderer)

target_include_directories(native_x_renderer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# The SDK's precompiled SPIR-V shaders (texture loading), included by guest_textures.cpp.
target_include_directories(native_x_renderer PRIVATE "${REXSDK_DIR}/src/graphics/shaders")
# The SDK's ImGui headers (overlay.cpp draws its dialogs, D75): rexruntime does not pass them on when the SDK is
# built from source; the symbols come from its DLL.
target_include_directories(native_x_renderer PRIVATE "${REXSDK_DIR}/thirdparty/imgui")
# SMAA (thirdparty/smaa, MIT): its lookup textures are headers, and its shader source is compiled at startup
# (src/aa.cpp), so it is embedded as a string, regenerated when the file changes.
set(_nxr_smaa_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/smaa/SMAA.hlsl")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_nxr_smaa_hlsl}")
file(READ "${_nxr_smaa_hlsl}" _nxr_smaa_source)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/smaa_source.h"
     "// Generated by native_x_renderer/CMakeLists.txt from thirdparty/smaa/SMAA.hlsl.\n#pragma once\n"
     "constexpr const char kSmaaSource[] = R\"SMAA(${_nxr_smaa_source})SMAA\";\n")
# The spatial upscalers and sharpening of presenting (src/scaling.cpp, D115): AMD FSR 1 and CAS (thirdparty/fsr1,
# thirdparty/cas) and NVIDIA Image Scaling (thirdparty/nis), all MIT. Their shader headers are compiled at startup
# too, embedded the same way; NIS_Config.h is included as C++.
set(_nxr_scaling_header
    "// Generated by native_x_renderer/CMakeLists.txt from thirdparty/fsr1, cas and nis.\n#pragma once\n")
foreach(_nxr_entry "kFfxASource=fsr1/ffx_a.h" "kFsr1Source=fsr1/ffx_fsr1.h" "kCasSource=cas/ffx_cas.h"
                   "kNisScalerSource=nis/NIS_Scaler.h")
  string(REPLACE "=" ";" _nxr_pair "${_nxr_entry}")
  list(GET _nxr_pair 0 _nxr_name)
  list(GET _nxr_pair 1 _nxr_path)
  set(_nxr_file "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/${_nxr_path}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_nxr_file}")
  file(READ "${_nxr_file}" _nxr_content)
  string(APPEND _nxr_scaling_header "constexpr const char ${_nxr_name}[] = R\"NXR(${_nxr_content})NXR\";\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/scaling_sources.h" "${_nxr_scaling_header}")
target_include_directories(native_x_renderer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/smaa"
                                                "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/nis"
                                                "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(native_x_renderer PUBLIC cxx_std_23)
# Cvars, logging, keybinds, and the PPC context the hooks receive.
target_link_libraries(native_x_renderer PUBLIC rex::runtime)
target_link_libraries(native_x_renderer PRIVATE
    native_x_renderer_sdk
    Diligent-GraphicsEngineVk-static
    Diligent-GraphicsEngineVkInterface
    Diligent-GraphicsEngineInterface
    Diligent-GraphicsAccessories
    Diligent-PlatformInterface
    Diligent-Common
    xxHash::xxhash
)
if(TARGET Diligent-SuperResolution-static AND DILIGENT_DLSS_SUPPORTED)
  target_link_libraries(native_x_renderer PRIVATE Diligent-SuperResolution-static)
  target_compile_definitions(native_x_renderer PRIVATE NATIVE_X_RENDERER_DLSS=1)
endif()
# The SuperResolution interfaces' headers in every build: src/upscaler.cpp keeps empty pointers to them without DLSS
# (Linux).
target_include_directories(native_x_renderer PRIVATE
                           "${NATIVE_X_RENDERER_DILIGENT_DIR}/Graphics/SuperResolution/interface")
# AMD FSR 4.1 and FSR 3.1 (src/fsr.cpp, D116; 13 Nov 2026: AMD FSR SDK 2.4.0, the first with FSR 4 on Vulkan): the
# SDK's headers (thirdparty/ffx_api2, MIT) and AMD's signed amd_fidelityfx_loader_vk.dll and
# amd_fidelityfx_upscaler_vk.dll, fetched when configuring and checked by their hashes, then copied next to the game's
# executable (native_x_renderer_copy_runtime); the loader is loaded at run time and finds the upscaler beside it.
# thirdparty/ffx_api (SDK 1.1.4's headers) stays for frame generation's code (src/framegen.cpp), whose library
# (amd_fidelityfx_vk.dll) is no longer fetched: frame generation reports itself unavailable.
target_include_directories(native_x_renderer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/ffx_api"
                           "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty")
set(_nxr_ffx_dir "${CMAKE_BINARY_DIR}/_deps/fidelityfx")
set(_nxr_ffx_base "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/raw/v2.4.0/Kits/FidelityFX/signedbin")
set(_nxr_ffx_ok TRUE)
foreach(_nxr_ffx "amd_fidelityfx_loader_vk.dll=38A56A01FB3565C0757D9F5C1E4E69DE4B65E5F2E5769B323B200DC1F142BAB9"
                 "amd_fidelityfx_upscaler_vk.dll=4A7E895AF6E0B615D40A84D59EB8C2B1054AD91CDD0C739567940270D51BDAB7")
  string(REPLACE "=" ";" _nxr_ffx_pair "${_nxr_ffx}")
  list(GET _nxr_ffx_pair 0 _nxr_ffx_name)
  list(GET _nxr_ffx_pair 1 _nxr_ffx_hash)
  if(WIN32 AND NOT EXISTS "${_nxr_ffx_dir}/${_nxr_ffx_name}")
    file(DOWNLOAD "${_nxr_ffx_base}/${_nxr_ffx_name}" "${_nxr_ffx_dir}/${_nxr_ffx_name}.part" STATUS _nxr_ffx_status
         EXPECTED_HASH SHA256=${_nxr_ffx_hash})
    list(GET _nxr_ffx_status 0 _nxr_ffx_code)
    if(_nxr_ffx_code EQUAL 0)
      file(RENAME "${_nxr_ffx_dir}/${_nxr_ffx_name}.part" "${_nxr_ffx_dir}/${_nxr_ffx_name}")
    else()
      file(REMOVE "${_nxr_ffx_dir}/${_nxr_ffx_name}.part")
      message(WARNING "NativeXRenderer: AMD's ${_nxr_ffx_name} could not be downloaded (${_nxr_ffx_status}): no FSR")
    endif()
  endif()
  if(NOT EXISTS "${_nxr_ffx_dir}/${_nxr_ffx_name}")
    set(_nxr_ffx_ok FALSE)
  endif()
endforeach()
if(_nxr_ffx_ok)
  target_compile_definitions(native_x_renderer PRIVATE NATIVE_X_RENDERER_FSR=1)
  set(NATIVE_X_RENDERER_FFX_DLL "${_nxr_ffx_dir}/amd_fidelityfx_loader_vk.dll;${_nxr_ffx_dir}/amd_fidelityfx_upscaler_vk.dll"
      CACHE INTERNAL "AMD's FSR libraries for Vulkan (FSR 4.1 and 3.1)")
else()
  unset(NATIVE_X_RENDERER_FFX_DLL CACHE)
endif()
# Intel XeSS (src/xess.cpp; 13 Nov 2026): the XeSS SDK 3.0.2's headers (thirdparty/xess, MIT) and Intel's libxess.dll
# (the Intel Simplified Software License, thirdparty/xess/LICENSE.txt), fetched when configuring and checked by its
# hash, copied next to the game's executable and loaded at run time.
set(_nxr_xess_dll "${CMAKE_BINARY_DIR}/_deps/xess/libxess.dll")
if(WIN32 AND NOT EXISTS "${_nxr_xess_dll}")
  file(DOWNLOAD "https://github.com/intel/xess/raw/v3.0.2/bin/libxess.dll" "${_nxr_xess_dll}.part" STATUS _nxr_xess_status
       EXPECTED_HASH SHA256=251659DD84A3E84DE67C886A4186E01F3ECA49B00641906FE38BB6B807E5D5B7)
  list(GET _nxr_xess_status 0 _nxr_xess_code)
  if(_nxr_xess_code EQUAL 0)
    file(RENAME "${_nxr_xess_dll}.part" "${_nxr_xess_dll}")
  else()
    file(REMOVE "${_nxr_xess_dll}.part")
    message(WARNING "NativeXRenderer: Intel's libxess.dll could not be downloaded (${_nxr_xess_status}): no XeSS")
  endif()
endif()
if(EXISTS "${_nxr_xess_dll}")
  target_compile_definitions(native_x_renderer PRIVATE NATIVE_X_RENDERER_XESS=1)
  set(NATIVE_X_RENDERER_XESS_DLL "${_nxr_xess_dll}" CACHE INTERNAL "Intel's XeSS library")
else()
  unset(NATIVE_X_RENDERER_XESS_DLL CACHE)
endif()
rexglue_apply_target_settings(native_x_renderer)

# Copies what NativeXRenderer needs at run time next to a game's executable: NVIDIA's DLSS runtime
# (nvngx_dlss.dll), which NGX loads from the application's folder, and AMD's FidelityFX library
# (amd_fidelityfx_vk.dll, FSR 3.1), which fsr.cpp loads from there.
function(native_x_renderer_copy_runtime target)
  if(COMMAND copy_dlss_dlls)
    copy_dlss_dlls(${target})
  endif()
  if(NATIVE_X_RENDERER_FFX_DLL)
    add_custom_command(TARGET ${target} POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy_if_different ${NATIVE_X_RENDERER_FFX_DLL}
                               "$<TARGET_FILE_DIR:${target}>"
                       COMMENT "Copying AMD's FSR libraries for ${target}")
  endif()
  if(NATIVE_X_RENDERER_XESS_DLL)
    add_custom_command(TARGET ${target} POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NATIVE_X_RENDERER_XESS_DLL}"
                               "$<TARGET_FILE_DIR:${target}>"
                       COMMENT "Copying Intel's XeSS library for ${target}")
  endif()
endfunction()
