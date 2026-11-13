# bf1943 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(bf1943 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(BF1943_SOURCES
    src/main.cpp
    src/fpe_guard.cpp
    src/live_layers.cpp
    src/online_services.cpp
    src/socket_fixes.cpp
    src/socket_overlapped.cpp
    src/kernel_fixes.cpp
    src/game_fixes.cpp
    src/shader_prep.cpp
    src/frame_stats.cpp
    src/frame_rate.cpp
    src/frame_cap.cpp
    src/native_x_renderer_bf1943.cpp
    src/native_x_renderer_engine.cpp
    src/game_menu.cpp
    src/launcher_kit.cpp
    src/steam_shortcut.cpp
    src/projects_art.cpp
    src/picture_decode.cpp
    src/update.cpp
    src/update_files.cpp
    src/http.cpp
    src/f8_panel.cpp
    src/hud_glyphs.cpp
    src/capture_points.cpp
    src/game_music.cpp
    src/plane_mouse.cpp
    src/vehicle_mouse.cpp
    src/native_input.cpp
    src/tick_rate.cpp
    src/flash_mouse.cpp
    src/lan_backend.cpp
    src/bf_font.cpp
    src/bf_list_shot.cpp
    src/display_modes.cpp
    src/input_settings.cpp
    src/perf_overlay.cpp
    src/audio_settings.cpp
    src/auto_join.cpp
    src/comms.cpp
    src/comms_ui.cpp
    src/gpu_preference.cpp
    third_party/miniz/miniz.c
    src/server_mode.cpp
    src/break_trace.cpp
    src/dev_pad.cpp
    src/net_trace.cpp
    src/online_trace.cpp
    src/dev_stacks.cpp
    src/dev_ui_probe.cpp
    src/dev_mem_dump.cpp
    src/dev_matscan.cpp
    src/dev_call_rate.cpp
    src/dev_config_probe.cpp
    src/look.cpp
    src/playtest_mark.cpp
    src/crash_report.cpp
    src/start_failure.cpp
    src/changelog.cpp
    src/fov.cpp
    src/vehicle_camera.cpp
    src/tick_blend.cpp
    src/effects.cpp
    src/night_lamps.cpp
    src/dev_types.cpp
    src/dev_team_scan.cpp
    src/modern_mode.cpp
    src/pipeline_upload.cpp
    src/game_options.cpp
    src/recoil.cpp
    src/game_install.cpp
    src/server_rotation.cpp
    src/gm_host.cpp
    src/private_match.cpp
    src/dev_watch.cpp
    src/dev_renderdoc.cpp
    src/dev_profiler.cpp
    src/dev_io_stats.cpp
)

if(WIN32)
    add_executable(bf1943 WIN32 ${BF1943_SOURCES})
else()
    add_executable(bf1943 ${BF1943_SOURCES})
endif()

# Players get our Vulkan renderer only (DECISIONS.md, 5 Nov 2026 evening). The SDK's Xenos plugin (D3D12) is in the
# developer build only, to capture reference frames.
option(BF_DEV_TOOLS "Build the developer aids" OFF)
if(BF_DEV_TOOLS)
    rexglue_setup_target(bf1943 GPU_PLUGINS xenos)
else()
    rexglue_setup_target(bf1943)
endif()
# Linux: the SDK compiles every target with -mcmodel=large ("large executable support"), which makes each call and
# global access an absolute 64 bit load. The program is far under the 2 GB the default model allows, so the game's
# own code goes back to it (the flag given last wins). King Kong found this on the ROG Ally (its commit 39064772):
# its recompiled movie decoder went from 13 to 30 FPS.
if(UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    foreach(_bf_target bf1943 bf1943_recomp)
        if(TARGET ${_bf_target})
            target_compile_options(${_bf_target} PRIVATE -mcmodel=small)
        endif()
    endforeach()
endif()


# NativeXRenderer (third_party/native-x-renderer, a submodule: github.com/ElGuslml/NativeXRenderer, MIT, Gustavo
# Garcia; DECISIONS.md, 5 Nov 2026): our native Vulkan renderer's base. It hooks the XDK Direct3D library, rebuilds
# each draw from the device and draws through DiligentCore (Apache 2.0). It compiles the SDK's shader translator and
# texture code from our SDK copy's source (REXSDK_DIR here only, after generated/rexglue.cmake, so the game still
# builds against the installed SDK), and wants the SDK's third party targets by their in tree names: given here
# from our SDK copy's own folders, so DiligentCore uses the same Vulkan and SPIR-V headers as the SDK.
set(BF_SDK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/src-bf")
set(REXSDK_DIR "${BF_SDK_SOURCE_DIR}")
if(NOT TARGET Vulkan::Headers)
    add_library(bf_vulkan_headers INTERFACE)
    target_include_directories(bf_vulkan_headers INTERFACE "${BF_SDK_SOURCE_DIR}/thirdparty/vulkan-headers/include")
    add_library(Vulkan::Headers ALIAS bf_vulkan_headers)
endif()
if(NOT TARGET SPIRV-Headers)
    add_library(SPIRV-Headers INTERFACE)
    target_include_directories(SPIRV-Headers INTERFACE "${BF_SDK_SOURCE_DIR}/thirdparty/spirv-headers/include")
endif()
if(NOT TARGET xxHash::xxhash)
    add_library(bf_xxhash INTERFACE)
    target_link_libraries(bf_xxhash INTERFACE rex::xxhash)
    target_include_directories(bf_xxhash INTERFACE "${BF_SDK_SOURCE_DIR}/thirdparty/xxHash")
    add_library(xxHash::xxhash ALIAS bf_xxhash)
endif()
if(NOT TARGET glslang)
    add_library(glslang INTERFACE)
    target_link_libraries(glslang INTERFACE rex::glslang rex::MachineIndependent rex::GenericCodeGen rex::OSDependent)
    target_include_directories(glslang INTERFACE "${BF_SDK_SOURCE_DIR}/thirdparty/glslang")
endif()
if(NOT TARGET glslang::SPIRV)
    add_library(bf_glslang_spirv INTERFACE)
    target_link_libraries(bf_glslang_spirv INTERFACE rex::SPIRV glslang)
    add_library(glslang::SPIRV ALIAS bf_glslang_spirv)
endif()
add_subdirectory(third_party/native-x-renderer)
# DiligentCore's Win32 file system includes ATL for one folder picker dialog; these Build Tools have no ATL, so that
# its libraries that include it get our CComPtr (third_party/atl_shim/atlbase.h).
foreach(_bf_atl_target Diligent-Win32Platform Diligent-ShaderTools)
  if(TARGET ${_bf_atl_target})
    target_include_directories(${_bf_atl_target} BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/atl_shim")
  endif()
endforeach()
target_link_libraries(bf1943 PRIVATE native_x_renderer)
# The launcher's menu music from the player's game files (src/game_music.cpp): FFmpeg's XMA2 decoder from the SDK's
# own FFmpeg (LGPL 2.1, decoders only; its headers from the SDK's source).
target_link_libraries(bf1943 PRIVATE rex::libavcodec rex::libavutil)
set_source_files_properties(src/game_music.cpp PROPERTIES INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/src-bf/thirdparty/FFmpeg")
# The launcher's PNG and JPEG pictures (src/picture_decode.cpp): stb_image from the SDK copy's thirdparty/stb, static.
set_source_files_properties(src/picture_decode.cpp PROPERTIES INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/src-bf/thirdparty/stb")
# src/gpu_preference.cpp: the SDK's Vulkan instance header includes RenderDoc's API header (in every build, not only
# the developer one's src/dev_renderdoc.cpp).
set_source_files_properties(src/gpu_preference.cpp PROPERTIES INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/src-bf/thirdparty/renderdoc")
native_x_renderer_copy_runtime(bf1943)

# Our data next to the exe: data/shaders/bf1943-shaders.list and the pipeline descriptions (src/shader_prep.cpp),
# our menus' font (Oswald, SIL Open Font License 1.1: fonts/OFL.txt), BF's menu lettering's look, and the launcher's
# button glyphs (Xelu's and Kenney's CC0 prompts, glyphs/LICENSE.txt, as King Kong ships them). Copied on every
# build (a target of its own): as a step after linking they were left stale when only the list changed (7 Nov 2026).
add_custom_target(bf1943_data ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/data $<TARGET_FILE_DIR:bf1943>/data
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/fonts $<TARGET_FILE_DIR:bf1943>/fonts
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/glyphs $<TARGET_FILE_DIR:bf1943>/glyphs
    COMMENT "Copying our shader list, fonts and button glyphs next to bf1943")
add_dependencies(bf1943 bf1943_data)

# The GPU plugins from our SDK copy (sdk/src-bf, build_bf.bat) over the pinned SDK's: rexgpu-xenos with the
# shader storage progress the shader screen shows (game_menu.cpp), and rexgpu-bfnative, the Vulkan only native
# renderer (docs/native-renderer.md). Skipped when the SDK copy has not been built.
set(BF_SDK_PLUGIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/src-bf/out/install/win-amd64/bin" CACHE PATH
    "Where our SDK copy installs its GPU plugins")
set(_bf_plugins rexgpu-bfnative.dll)
if(BF_DEV_TOOLS)
    list(APPEND _bf_plugins rexgpu-xenos.dll)
endif()
foreach(_plugin ${_bf_plugins})
    if(EXISTS "${BF_SDK_PLUGIN_DIR}/${_plugin}")
        add_custom_command(TARGET bf1943 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BF_SDK_PLUGIN_DIR}/${_plugin}" $<TARGET_FILE_DIR:bf1943>)
    endif()
endforeach()

# Developer aids (src/break_trace.cpp, src/dev_pad.cpp, src/net_trace.cpp, src/online_trace.cpp,
# src/dev_stacks.cpp). Never in a release build.
# Night Ops (src/night_ops.h): not in releases until TekRantGaming says it is ready (12 Nov 2026, for 0.3.0); ON for
# the private test builds and development.
option(BF_NIGHT_OPS "Night Ops: a night server's maps drawn at night in players' games" OFF)
if(BF_NIGHT_OPS)
    target_compile_definitions(bf1943 PRIVATE BF_NIGHT_OPS=1)
endif()

# Our own builds only (not a release's, not a pull request's): the SMG sight alignment's sliders and keys.
option(BF_INTERNAL_TOOLS "Build the internal tuning aids (the SMG sight alignment)" OFF)
if(BF_INTERNAL_TOOLS)
    target_compile_definitions(bf1943 PRIVATE BF_INTERNAL_TOOLS=1)
endif()
if(BF_DEV_TOOLS)
    target_compile_definitions(bf1943 PRIVATE BF_DEV_TOOLS=1)
    # src/dev_renderdoc.cpp: RenderDoc's in-application API header, from our SDK copy.
    target_include_directories(bf1943 PRIVATE "${BF_SDK_SOURCE_DIR}/thirdparty/renderdoc")
endif()

# bf1943.exe's file details (src/bf1943_version.rc.in): the release's version, set for a package build
# (tools/make_packages.ps1 checks it).
set(BF_VERSION "0.4.1" CACHE STRING "The release version in bf1943.exe's file details (major.minor.patch)")
if(WIN32)
    enable_language(RC)
    string(REGEX MATCH "^([0-9]+)[.]([0-9]+)[.]([0-9]+)" _bf_version_match "${BF_VERSION}")
    set(BF_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(BF_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(BF_VERSION_PATCH "${CMAKE_MATCH_3}")
    configure_file(src/bf1943_version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/bf1943_version.rc" @ONLY)
    target_sources(bf1943 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/bf1943_version.rc")
endif()
# CHANGELOG.md, built into the program for the launcher's What's new pop-up (src/changelog.cpp), as King Kong does.
# The public changelog (the players' repository) is this file.
set(BF_CHANGELOG_MD "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BF_CHANGELOG_MD}")
file(READ "${BF_CHANGELOG_MD}" BF_CHANGELOG_TEXT)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/changelog_text.cpp"
     CONTENT "namespace bf1943 {
extern const char* const kChangelogText;
const char* const kChangelogText = R\"BFCHANGELOG(@BF_CHANGELOG_TEXT@)BFCHANGELOG\";
}
"
     @ONLY)
target_sources(bf1943 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/changelog_text.cpp")
# The version the graphics pipelines are sent with (src/pipeline_upload.cpp), on that file alone.
set_source_files_properties(src/pipeline_upload.cpp PROPERTIES COMPILE_DEFINITIONS "BF_VERSION_STRING=\"${BF_VERSION}\"")

if(WIN32)
    target_link_libraries(bf1943 PRIVATE ws2_32)  # src/socket_fixes.cpp: WSAGetLastError
    target_link_libraries(bf1943 PRIVATE pdh)  # src/perf_overlay.cpp: the GPU counters
    target_link_libraries(bf1943 PRIVATE dbghelp winmm bcrypt ole32 oleaut32 comdlg32)  # dev_profiler.cpp; game_install.cpp (SHA1), the setup pickers; gm_host.cpp (UPnP)
    target_link_libraries(bf1943 PRIVATE winhttp)  # src/pipeline_upload.cpp
    target_link_libraries(bf1943 PRIVATE d3d12 dxgi)  # src/gpu_preference.cpp: the high-performance GPU
    # UTF-8 as the program's code page, and a linker map for decoding crash reports.
    target_link_options(bf1943 PRIVATE "LINKER:/MANIFEST:EMBED" "LINKER:/MANIFESTUAC:NO"
                        "LINKER:/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/bf1943.manifest"
                        "LINKER:/MAP")
endif()
