# bf1943 - ReXGlue Recompiled Project
#
# SDK v0.11+ port (.handoff/SDK-0.11-PORT-PLAN.md): the SDK is our copy in sdk/src-bf, built here with
# add_subdirectory (REXSDK_DIR), as upstream's project template does. Never the installed SDK package: the shared
# builds find theirs through the CMake package registry.

cmake_minimum_required(VERSION 3.25)
project(bf1943 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/src-bf" CACHE PATH "Our ReXGlue SDK copy")
# Linux x86_64: the SDK's targets in the default (small) code model too, as the game's own (rexglue_target.cmake,
# REXGLUE_SMALL_CODE_MODEL): a ThinLTO link refuses to mix the large model's objects with the small ones.
if(UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(REXGLUE_SMALL_CODE_MODEL ON CACHE BOOL "The SDK in the default code model (Linux x86_64)" FORCE)
endif()
add_subdirectory("${REXSDK_DIR}" rexglue-sdk)
rexglue_project(bf1943 OUTPUT_DIR generated/default)

# SDL: v0.11 links SDL3 as a shared library beside the exe (SDL3.dll, a runtime file of rexglue_configure_target), so
# the game no longer carries a second, static SDL (the 0.2.2 antivirus false positive, 14 Nov 2026).

# v0.10's SDK interfaces the game still uses, kept in the game (src/compat, SDK v0.11+ port phase 2): v0.10's cvar
# registry as the game's own, bridged to v0.11's settings, and headers for the names v0.11 moved. Before the SDK's
# include folders, for the game and NativeXRenderer alike.
add_library(bf_compat_headers INTERFACE)
target_include_directories(bf_compat_headers BEFORE INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src/compat"
                           "${REXSDK_DIR}/thirdparty/cli11/include"
                           "${REXSDK_DIR}/thirdparty/xxHash")  # header only (XXH_INLINE_ALL), as before
target_link_libraries(bf_compat_headers INTERFACE rex::sdk tomlplusplus)
# The game calls SDL itself (voice chat, the launcher): the SDK's shared SDL3.dll.
target_link_libraries(bf_compat_headers INTERFACE SDL3::SDL3)
# src/gpu_preference.cpp: Vulkan's headers, the SDK's copy (thirdparty/Vulkan-Headers).
target_link_libraries(bf_compat_headers INTERFACE rex-vulkan-headers)

# Sources
set(BF1943_SOURCES
    src/compat/cvar.cpp
    src/compat/kernel_state.cpp
    src/compat/ui_compat.cpp
    src/compat/display_mode.cpp
    src/compat/graphics_hooks.cpp
    src/bf1943_app.cpp
    src/compat/cvar_bridge.cpp
    src/compat/sdk_cvars.cpp
    src/compat/input_compat.cpp
    src/main.cpp
    src/cpu_check.cpp
    src/gpu_lost.cpp
    src/fpe_guard.cpp
    src/live_layers.cpp
    src/online_services.cpp
    src/socket_fixes.cpp
    # src/socket_overlapped.cpp: v0.10 only (v0.11 has overlapped WSARecvFrom; its filtering is in socket_fixes.cpp).
    # src/kernel_fixes.cpp: v0.10 only. v0.11's ObDereferenceObject finds the object by its guest address and
    # dereferences that object, which is what this fix did (SDK v0.11+ port, phase 4).
    src/game_fixes.cpp
    src/shader_prep.cpp
    src/frame_stats.cpp
    src/frame_rate.cpp
    src/frame_throttle.cpp
    src/native_x_renderer_bf1943.cpp
    src/native_x_renderer_engine.cpp
    src/game_menu.cpp
    src/account_key.cpp
    src/launcher_kit.cpp
    src/achievement_toast.cpp
    src/theme_chime.cpp
    src/reborn_achievements.cpp
    src/server_stats.cpp
    src/steam_shortcut.cpp
    src/projects_art.cpp
    src/news.cpp
    src/news_video.cpp
    src/picture_decode.cpp
    src/update.cpp
    src/update_files.cpp
    src/http.cpp
    src/f8_panel.cpp
    src/hud_glyphs.cpp
    src/capture_points.cpp
    src/map_players.cpp
    src/clan_tags.cpp
    src/clan_tag_colours.cpp
    src/discord_presence.cpp
    src/game_music.cpp
    src/weather_audio.cpp
    src/plane_mouse.cpp
    src/vehicle_mouse.cpp
    src/native_input.cpp
    src/vehicle_binds.cpp
    src/tick_rate.cpp
    src/flash_mouse.cpp
    src/flash_spawn.cpp
    src/lan_backend.cpp
    src/bf_font.cpp
    src/bf_list_shot.cpp
    src/display_modes.cpp
    src/input_settings.cpp
    src/perf_overlay.cpp
    src/audio_settings.cpp
    src/ea_voice_cull.cpp
    src/ea_mixer_ready.cpp
    src/ea_xma_kicks.cpp
    src/auto_join.cpp
    src/comms.cpp
    src/audio_devices.cpp
    src/comms_ui.cpp
    src/storm_patterns.cpp
    src/world_sync.cpp
    src/free_cam.cpp
    src/voice_mute.cpp
    src/voice_mute_icon.cpp
    src/scoreboard_ping.cpp
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
    src/shadow_sun.cpp
    src/vehicle_camera.cpp
    src/vehicle_views.cpp
    src/tick_blend.cpp
    src/linux_profiler.cpp
    src/effects.cpp
    src/night_lamps.cpp
    src/dev_types.cpp
    src/dev_team_scan.cpp
    src/dev_player_scan.cpp
    src/dev_vehicle_scan.cpp
    src/modern_mode.cpp
    src/pipeline_upload.cpp
    src/report.cpp
    src/game_options.cpp
    src/recoil.cpp
    src/game_install.cpp
    src/server_rotation.cpp
    src/team_balance.cpp
    src/match_rules.cpp
    src/bots.cpp
    src/bot_chat.cpp
    src/squad_colors.cpp
    src/sharp_text.cpp
    src/server_tick_probe.cpp
    src/gm_host.cpp
    src/private_match.cpp
    src/dev_watch.cpp
    src/dev_renderdoc.cpp
    src/bench_freecam.cpp
    src/bench_explode.cpp
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
# v0.11: the graphics drivers are modules beside the exe, selected by gpu.driver (rexgpu-xenos-vulkan, -d3d12, -null;
# ours, rexgpu-xenos-bfnative, comes in phase 5).
rexglue_setup_target(bf1943)
target_link_libraries(bf1943 PRIVATE bf_compat_headers)
# The keyboard and mouse pad's additions (rex/hid/bf_mnk.h, our SDK patch; compat rex/input/mnk): exported by the
# input module, which the SDK loads as a plugin too (the same DLL).
target_link_libraries(bf1943 PRIVATE rexhid)
# The compatibility headers first, ahead of the SDK's: some keep a name v0.11 still uses (rex/ui/window.h).
target_include_directories(bf1943 BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/compat")
# Linux: the SDK compiles every target with -mcmodel=large ("large executable support"), which makes each call and
# global access an absolute 64 bit load. The program is far under the 2 GB the default model allows, so the game's
# own code goes back to it (the flag given last wins). King Kong found this on the ROG Ally (its commit 39064772):
# its recompiled movie decoder went from 13 to 30 FPS. (The renderer's targets: after their add_subdirectory below.)
if(UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    foreach(_bf_target bf1943 bf1943_recomp bf1943_recompiled)
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
set(BF_SDK_SOURCE_DIR "${REXSDK_DIR}")
# SDK v0.11+ port: the SDK's own targets are in this build (add_subdirectory), under v0.11's names. Vulkan's headers
# are its rex-vulkan-headers (thirdparty/Vulkan-Headers), xxHash's and glslang's targets keep their names. v0.11 has
# no SPIRV-Headers of its own (its translator uses glslang's spirv.hpp): DiligentCore then uses its own copy.
if(NOT TARGET Vulkan::Headers)
    add_library(Vulkan::Headers ALIAS rex-vulkan-headers)
endif()
add_subdirectory(third_party/native-x-renderer)
# Linux: the renderer back to the default code model as well (see the game's own targets above). Until 13 Nov 2026
# NativeXRenderer and its SDK glue were the only renderer of any platform built with -mcmodel=large, every call and
# global access an absolute 64 bit load (LINUX-PERFORMANCE.md).
if(UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    foreach(_bf_target native_x_renderer native_x_renderer_sdk)
        if(TARGET ${_bf_target})
            target_compile_options(${_bf_target} PRIVATE -mcmodel=small)
        else()
            message(WARNING "${_bf_target}: no such target, its code model is left as the SDK's")
        endif()
    endforeach()
endif()
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
target_link_libraries(bf1943 PRIVATE libavcodec libavutil)
set_source_files_properties(src/game_music.cpp PROPERTIES INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/src-bf/thirdparty/FFmpeg")
# The weather's sounds (src/weather_audio.cpp): the same FFmpeg's MP3 decoder and parser.
set_source_files_properties(src/weather_audio.cpp PROPERTIES INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/src-bf/thirdparty/FFmpeg")
# The launcher's PNG and JPEG pictures (src/picture_decode.cpp): stb_image from the SDK copy's thirdparty/stb, static.
set_source_files_properties(src/picture_decode.cpp PROPERTIES INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/src-bf/thirdparty/stb")
# src/gpu_preference.cpp: the SDK's Vulkan instance header includes RenderDoc's API header (in every build, not only
# the developer one's src/dev_renderdoc.cpp).
set_source_files_properties(src/gpu_preference.cpp PROPERTIES INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/src-bf/thirdparty/renderdoc")
native_x_renderer_copy_runtime(bf1943)
# The news window's embedded YouTube player (src/news_video.cpp, docs/NEWS.md): the WebView2 SDK's header, and
# Microsoft's signed loader beside the exe (loaded at run time; without it the news shows thumbnails). Windows only.
if(WIN32)
    set_source_files_properties(src/news_video.cpp PROPERTIES INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/include")
    add_custom_command(TARGET bf1943 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/x64/WebView2Loader.dll" $<TARGET_FILE_DIR:bf1943>)
endif()

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

# SDK v0.11+ port: v0.10's GPU plugins (rexgpu-xenos, rexgpu-bfnative) copied from the installed SDK are gone. The
# SDK's driver modules are built here and copied beside the exe by rexglue_configure_target; NativeXRenderer's own
# driver module is phase 5.

# Developer aids (src/break_trace.cpp, src/dev_pad.cpp, src/net_trace.cpp, src/online_trace.cpp,
# src/dev_stacks.cpp). Never in a release build.
# Night Ops (src/night_ops.h): not in releases until TekRantGaming says it is ready (12 Nov 2026, for 0.3.0); ON for
# the private test builds and development.
# On since 0.4.8, the release that brings Night Ops and Daybreak to players (TekRantGaming, 27 Nov 2026).
option(BF_NIGHT_OPS "Night Ops: a night server's maps drawn at night in players' games" ON)
if(BF_NIGHT_OPS)
    target_compile_definitions(bf1943 PRIVATE BF_NIGHT_OPS=1)
endif()
# A developer's own build switches, kept out of the repository (bf/local.cmake, gitignored): for one, the SMG sight
# alignment's tuning aids (BF_INTERNAL_TOOLS: its settings row, keys and pause menu sliders), never in a release's.
include(${CMAKE_CURRENT_SOURCE_DIR}/local.cmake OPTIONAL)
# The picture in picture sniper scope (src/scope_pip.h): taken out of 0.4.1 at TekRantGaming's word (13 Nov 2026); ON
# for the private test builds and development.
option(BF_SCOPE_PIP "The picture in picture sniper scope in Settings > Enhancements" OFF)
if(BF_SCOPE_PIP)
    target_compile_definitions(bf1943 PRIVATE BF_SCOPE_PIP=1)
endif()

if(BF_DEV_TOOLS)
    target_compile_definitions(bf1943 PRIVATE BF_DEV_TOOLS=1)
    # src/dev_renderdoc.cpp: RenderDoc's in-application API header, from our SDK copy.
    target_include_directories(bf1943 PRIVATE "${BF_SDK_SOURCE_DIR}/thirdparty/renderdoc")
endif()

# bf1943.exe's file details (src/bf1943_version.rc.in): the release's version, set for a package build
# (tools/make_packages.ps1 checks it).
set(BF_VERSION "0.1.11" CACHE STRING "The release version in bf1943.exe's file details (major.minor.patch)")
if(WIN32)
    enable_language(RC)
    string(REGEX MATCH "^([0-9]+)[.]([0-9]+)[.]([0-9]+)" _bf_version_match "${BF_VERSION}")
    set(BF_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(BF_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(BF_VERSION_PATCH "${CMAKE_MATCH_3}")
    # A hotfix's fourth number (0.4.5.10, 14 Nov 2026) as the file version's fourth number.
    set(BF_VERSION_HOTFIX 0)
    if(BF_VERSION MATCHES "^[0-9]+[.][0-9]+[.][0-9]+[.]([0-9]+)")
        set(BF_VERSION_HOTFIX "${CMAKE_MATCH_1}")
    endif()
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
# Report an issue (src/report.cpp): the screenshots' JPEG from stb_image_write (static), Diligent's copy of stb.
set_source_files_properties(src/report.cpp PROPERTIES INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/native-x-renderer/thirdparty/diligent-core/ThirdParty/stb")
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
