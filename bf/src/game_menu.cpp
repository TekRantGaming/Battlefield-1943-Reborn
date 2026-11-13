// Our main menu inside the running game, King Kong style (TekRantGaming, 4 Nov 2026; mockups in runs\mockups). Since 5 Nov
// (TekRantGaming: "Launcher is the main menu > click join server > game loads into the server instantly skipping its main
// menu and everything else") it is the main menu, over BF's own menu scene. Since 7 Nov (TekRantGaming: no bare menu over
// the films, and BF's own loading screens) it opens only once BF's online menu is up: the game's films and title are
// its own, the player presses Start, and the driver (auto_join.cpp) signs in under BF's own dialogs. A join covers BF's
// menu while the driver picks the server there, then BF's own loading screen, Loading complete and spawn screen
// (the class pick) are the player's. In a match the pause menu is drawn over the game. Pages:
// main (Play online, Server browser, Settings, Quit), the server browser (the backend's live list, Join), settings
// (frame rate, fullscreen; saved in <user>\bf1943-settings.txt) and the joining screen while the driver walks BF's
// menus into the picked server. Its own input: player 1's pad (the game gets an empty pad meanwhile), the
// keyboard (arrows, Enter, Esc) and the mouse.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cmath>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>  // the achievements (AchievementsPage)
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window.h>
#if defined(BF_DEV_TOOLS)
#include <rex/ui/window_sdl.h>  // the tests' mouse side buttons, pushed into the window's own SDL queue
#endif

#include <native_x_renderer/native_x_renderer.h>

#include "capture_points.h"
#include "display_modes.h"
#include "game_install.h"
#include "launcher_kit.h"
#include "private_match.h"
#include "regions.h"
#include "scope_pip.h"
#include "server_list.h"
#include "projects_art.h"
#include "steam_shortcut.h"
#include "comms.h"
#include "text_filter.h"
#include "changelog.h"
#include "update.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#else
#include <dlfcn.h>
#include <unistd.h>  // fork and exec for the desktop's link opener (OpenUrl)
#include <SDL3/SDL.h>  // the launcher's controller read on Linux (HostPad): SDL, the runtime's own input backend
#endif

REXCVAR_DEFINE_BOOL(bf_launcher, true, "BF1943", "Our menu in place of BF's main menu (false: the original menus)");
REXCVAR_DEFINE_STRING(bf_button_prompts, "auto", "BF1943/Controls",
                      "Our menus' button hints: auto, xbox360, xbox_series, ps3, ps5, steam_deck, keyboard (ps2 reads as ps3)");
REXCVAR_DEFINE_STRING(bf_backend_registry, "", "BF1943",
                      "The backend's server list (host:port); empty: bf_online_host's, port 18250");
REXCVAR_DEFINE_STRING(bf_online_host, "127.0.0.1", "BF1943",
                      "The main backend players log in to and get the server list from (an address or a name)");
REXCVAR_DEFINE_STRING(bf_player_name, "", "BF1943",
                      "The name other players see (empty: the one saved in bf1943-settings.txt, made at the first start)");
// King Kong's Upscaler mode (TekRantGaming, 11 Nov 2026, v0.2.1: AMD FSR 1 and NVIDIA NIS "exactly" as King Kong has them): how
// many pixels the game draws before the upscaler (native_x_renderer_scaling fsr or nis) scales them to the screen.
REXCVAR_DEFINE_DOUBLE(bf_render_scale, 100.0, "BF1943",
                      "Render resolution in percent of the display's (50 to 150), with the Upscaler off: under 100 the "
                      "picture is drawn smaller and scaled up, over 100 larger and scaled down (supersampling). Never "
                      "below 720 lines");
// The Upscaling section (13 Nov 2026: "FSR, DLSS, XeSS, TAA should have their own section for Upscaling, and if
// enabled, they follow their own sharpness slider and disable other anti-aliasing options"): the temporal upscaler
// and its mode, and the antialiasing of the finished picture for when there is none. ApplyUpscaler makes the
// renderer's one setting (native_x_renderer_antialiasing) of them.
REXCVAR_DEFINE_BOOL(bf_wheel_bindable, false, "BF1943/Controls",
                    "The mouse wheel presses what the bindings name it for (WheelUp, WheelDown among a binding's keys); "
                    "false, as before the bindings had mouse columns: Toggle Weapon on foot");
REXCVAR_DEFINE_STRING(bf_temporal, "off", "BF1943",
                      "The temporal upscaler: off, fsr4 (AMD FSR 4.1), fsr (AMD FSR 3.1), xess (Intel XeSS), dlss "
                      "(NVIDIA DLSS) or taa (the renderer's own)");
REXCVAR_DEFINE_STRING(bf_temporal_mode, "native", "BF1943",
                      "The temporal upscaler's mode: native (antialiasing at the display's resolution), quality, "
                      "balanced, performance or ultra_performance");
REXCVAR_DEFINE_STRING(bf_post_aa, "", "BF1943",
                      "Antialiasing without a temporal upscaler: off, fxaa, smaa_low to smaa_ultra, cmaa or cmaa_ultra "
                      "(empty: the renderer's own setting as the start found it)");
REXCVAR_DEFINE_STRING(bf_upscaler_mode, "quality", "BF1943",
                      "With AMD FSR 1 or NVIDIA NIS (native_x_renderer_scaling): native, quality, balanced or "
                      "performance, the game drawn at the screen's size divided by 1, 1.5, 1.7 or 2 (never below 720p); "
                      "read at the start")
    .allowed({"native", "quality", "balanced", "performance"});
REXCVAR_DECLARE(bool, bf_unlock_fps);  // frame_rate.cpp
REXCVAR_DECLARE(bool, bf_flag_letters);  // capture_points.cpp: letters in the capture point flags (Enhancements)
REXCVAR_DECLARE(bool, server);         // server_mode.cpp: a dedicated server has no menu
REXCVAR_DECLARE(double, bf_max_fps);
REXCVAR_DECLARE(bool, native_x_renderer);  // NativeXRenderer: its own shader storage is made before the first draw
REXCVAR_DECLARE(bool, native_x_renderer_fill_screen);  // NativeXRenderer: ultrawide fill (Aspect ratio row)

void SetFrameRate(double fps);  // frame_rate.cpp

namespace bf1943 {
int SystemFrameCap();      // frame_cap.cpp: a frame limit set outside the game (0: none known)
void StartFrameCapRead();  // frame_cap.cpp

// The real Steam and Discord marks on the launcher's cards (TekRantGaming, 11 Nov 2026; King Kong's, Simple Icons, CC0).
namespace {
#include "discord_icon.inc"
#include "steam_icon.inc"
}  // namespace

// The upscaler modes (King Kong's launcher.cpp kUpscalerModes and settings.cpp RenderPresets): the screen's size over
// the drawn size. King Kong rounds the drawn size to its renderer's 720p steps; NativeXRenderer draws any size (D157).
struct UpscalerModeInfo {
  const char* value;
  const char* label;
  double ratio;
};
constexpr UpscalerModeInfo kUpscalerModes[] = {
    {"native", "Native", 1.0}, {"quality", "Quality", 1.5}, {"balanced", "Balanced", 1.7}, {"performance", "Performance", 2.0}};
double UpscalerRatio(const std::string& mode) {
  for (const auto& m : kUpscalerModes)
    if (mode == m.value) return m.ratio;
  return 1.5;  // the setting's default, Quality
}
// What NativeXRenderer draws for an output (renderer.cpp, D37 and D157): the game's 1280x720 at the largest scale that
// fits the output, divided by the ratio, at least 1.
// With Fill screen (native_x_renderer_fill_screen, 0.4.0) a screen of another shape gets the game's width times the
// screen's aspect over 16:9 (per axis scales, renderer.cpp's fill).
std::pair<int, int> DrawnSizeFor(int out_w, int out_h, double ratio) {
  const double w = double(std::max(out_w, 1)), h = double(std::max(out_h, 1));
  const double factor = REXCVAR_GET(native_x_renderer_fill_screen) && std::fabs((w / h) / (16.0 / 9.0) - 1.0) > 0.01
                            ? (w / h) / (16.0 / 9.0)
                            : 1.0;
  const double fit = std::min(w / (1280.0 * factor), h / 720.0);
  const double scale = std::max(1.0, fit / std::max(ratio, 1.0));
  return {int(std::lround(1280.0 * factor * scale)), int(std::lround(720.0 * scale))};
}
// Graphics presets (TekRantGaming, 12 Nov 2026: "graphic presets Potato, low, medium, high, ultra all with appropriate
// presets for players"): the Graphics page's settings that cost frame time. Low, Medium and High are the auto detect's
// slow, middle and fast tiers (TierSettings): Medium is the GTX 1060 or RX 580 at 1080p and 60 FPS, High an RTX 2060 or
// 3060 and up. Potato is below Low for integrated graphics (no antialiasing, no texture filtering), Ultra every setting
// at its most with the game drawn at the display's resolution (the upscaler Off). A setting a preset leaves out stays
// the player's: the look's own (AO strength, sharpness), AO's and GI's quality where they are off, HD textures above
// Low (they need a pack imported; Potato and Low turn them off for the graphics memory). Ultra's GI distance is Far:
// the most the menus offer (TekRantGaming, 13 Nov 2026: Full is the F8 panel's only).
struct GraphicsPreset {
  const char* name;
  std::vector<std::pair<const char*, const char*>> settings;
};
const std::vector<GraphicsPreset>& GraphicsPresets() {
  static const std::vector<GraphicsPreset> kPresets = {
      {"Potato",
       {{"bf_post_aa", "off"}, {"native_x_renderer_scaling", "fsr"}, {"bf_upscaler_mode", "performance"},
        {"native_x_renderer_shadow_scale", "0.000000"}, {"anisotropic_override", "0"}, {"native_ao_mode", "0"},
        {"native_gi_mode", "0"}, {"native_grass_radius", "45.000000"}, {"native_x_renderer_hd_textures", "false"}}},
      {"Low",
       {{"bf_post_aa", "fxaa"}, {"native_x_renderer_scaling", "fsr"}, {"bf_upscaler_mode", "performance"},
        {"native_x_renderer_shadow_scale", "0.000000"}, {"anisotropic_override", "2"}, {"native_ao_mode", "0"},
        {"native_gi_mode", "0"}, {"native_grass_radius", "45.000000"}, {"native_x_renderer_hd_textures", "false"}}},
      {"Medium",
       {{"bf_post_aa", "fxaa"}, {"native_x_renderer_scaling", "fsr"}, {"bf_upscaler_mode", "quality"},
        {"native_x_renderer_shadow_scale", "0.000000"}, {"anisotropic_override", "4"}, {"native_ao_mode", "1"},
        {"native_ao_samples", "1"}, {"native_gi_mode", "1"}, {"native_gi_quality", "1"}, {"native_gi_distance", "1"},
        {"native_grass_radius", "180.000000"}}},
      {"High",
       {{"bf_post_aa", "fxaa"}, {"native_x_renderer_scaling", "fsr"}, {"bf_upscaler_mode", "quality"},
        {"native_x_renderer_shadow_scale", "2.000000"}, {"anisotropic_override", "5"}, {"native_ao_mode", "1"},
        {"native_ao_samples", "2"}, {"native_gi_mode", "1"}, {"native_gi_quality", "2"}, {"native_gi_distance", "2"},
        {"native_grass_radius", "1440.000000"}}},
      {"Ultra",
       {{"native_x_renderer_antialiasing", "fxaa"}, {"native_x_renderer_scaling", "bilinear"},
        {"native_x_renderer_shadow_scale", "4.000000"}, {"anisotropic_override", "5"}, {"native_ao_mode", "1"},
        {"native_ao_samples", "3"}, {"native_gi_mode", "1"}, {"native_gi_quality", "3"}, {"native_gi_distance", "3"},
        {"native_grass_radius", "1440.000000"}}},
  };
  return kPresets;
}
// A setting's value as a preset has it: "2" and "2.000000" are the same, and NVIDIA NIS stands in for AMD FSR 1 (the
// upscaler the player picked stays: ApplyGraphicsPreset).
bool PresetValueMatches(const std::string& name, const std::string& now, const std::string& want) {
  if (now == want) return true;
  if (name == "native_x_renderer_scaling" && want == "fsr" && now == "nis") return true;
  char* e1 = nullptr;
  char* e2 = nullptr;
  const double a = std::strtod(now.c_str(), &e1), b = std::strtod(want.c_str(), &e2);
  return !now.empty() && !want.empty() && !*e1 && !*e2 && a == b;
}
// The sharpness for the upscaler Off (the settings file's, else the command line's or the renderer's default), as the
// start found it and the menu's Sharpness row changes it.
std::string g_plain_sharpness = "-1";
// The Upscaler (King Kong's, TekRantGaming 11 Nov 2026) into the renderer's settings, which it reads when it starts. The game
// draws at the display's resolution (TekRantGaming, 11 Nov 2026: no Render resolution row; "I want that resolution to match the
// render resolution", King Kong's Native): the largest 16:9 picture the screen or window holds, never above it. With
// AMD FSR 1 or NVIDIA NIS, that divided by the Upscaler mode's ratio (never below 720p, NativeXRenderer D157), and the
// upscaler sharpens as AMD and NVIDIA made it (RCAS at 0.2 stops, NIS at 0.5: King Kong offers no sharpness for them).
// NIS where this GPU was found not to run it (`nis_support` "0", the settings' last answer, D158) is FSR 1. Called at
// the start (ApplyStartupDisplaySettings) and when the menu's rows change before the game runs (Play takes them).
// 13 Nov 2026: with a temporal upscaler (bf_temporal) the renderer's antialiasing is that upscaler in its mode, the
// spatial upscaler is off and so is the sharpening at presenting (the temporal one has its own:
// native_x_renderer_upscaler_sharpness); without one, the antialiasing is the finished picture's (bf_post_aa).
bool TemporalUpscalerOn() {
  const std::string t = rex::cvar::GetFlagByName("bf_temporal");
  return t == "fsr4" || t == "fsr" || t == "dlss" || t == "xess" || t == "taa";
}
void ApplyUpscaler(bool log, const std::string& nis_support) {
  const bool temporal = TemporalUpscalerOn();
  if (temporal) {
    const std::string t = rex::cvar::GetFlagByName("bf_temporal");
    std::string mode = rex::cvar::GetFlagByName("bf_temporal_mode");
    if (mode != "quality" && mode != "balanced" && mode != "performance" && mode != "ultra_performance") mode = "native";
    const std::string name = t == "taa"    ? std::string("taa")  // at the display's resolution only
                             : t == "dlss" ? (mode == "native" ? std::string("dlaa") : "dlss_" + mode)
                                           : t + "_" + mode;
    rex::cvar::SetFlagByName("native_x_renderer_antialiasing", name);
    rex::cvar::SetFlagByName("native_x_renderer_scaling", "bilinear");
    if (log) REXLOG_INFO("BF upscaler: temporal, {} (its own sharpening; no other antialiasing)", name);
  } else {
    std::string aa = rex::cvar::GetFlagByName("bf_post_aa");
    if (aa != "off" && aa != "fxaa" && aa.rfind("smaa", 0) != 0 && aa.rfind("cmaa", 0) != 0) aa = "fxaa";
    rex::cvar::SetFlagByName("native_x_renderer_antialiasing", aa);
  }
  struct AfterAll {  // the temporal upscaler sharpens: none at presenting, whatever the rows below set
    bool temporal;
    ~AfterAll() {
      // (not our TAA: it has none of its own, the Sharpness row's stays)
      if (temporal && rex::cvar::GetFlagByName("bf_temporal") != "taa")
        rex::cvar::SetFlagByName("native_x_renderer_sharpness", "0");
    }
  } after{temporal};
  std::string scaling = rex::cvar::GetFlagByName("native_x_renderer_scaling");
  if (!REXCVAR_GET(server)) rex::cvar::SetFlagByName("native_x_renderer_check_nis", "true");  // D158, for the menu
  rex::cvar::SetFlagByName("native_x_renderer_resolution_scale", "0");  // the display's resolution
  if (scaling == "nis" && nis_support == "0") {
    rex::cvar::SetFlagByName("native_x_renderer_scaling", "fsr");
    if (log) REXLOG_INFO("BF upscaler: NVIDIA NIS does not run on this GPU (its last start found): AMD FSR 1 instead");
    scaling = "fsr";
  }
  if (scaling == "fsr" || scaling == "nis") {
    const std::string mode = rex::cvar::GetFlagByName("bf_upscaler_mode");
    const double ratio = UpscalerRatio(mode);
    rex::cvar::SetFlagByName("native_x_renderer_upscale_ratio", fmt::format("{:.6f}", ratio));
    rex::cvar::SetFlagByName("native_x_renderer_sharpness", "-1");
    if (log)
      REXLOG_INFO("BF upscaler: {} at {} (the display's size over {:.1f}, never below 720p), its own sharpening",
                  scaling == "fsr" ? "AMD FSR 1" : "NVIDIA NIS", mode, ratio);
    return;
  }
  if (scaling != "bilinear") rex::cvar::SetFlagByName("native_x_renderer_scaling", "bilinear");
  // Render resolution (13 Nov 2026: "set at 100% on a slider that can be increased to 150%, and decreased to 50%"):
  // the display's size times the percentage, as the renderer's ratio (the size divided by it: 2 for 50%, 0.667 for
  // 150%). 100%: none.
  const double percent = std::clamp(REXCVAR_GET(bf_render_scale), 50.0, 150.0);
  const bool scaled = std::fabs(percent - 100.0) > 0.5;
  rex::cvar::SetFlagByName("native_x_renderer_upscale_ratio", scaled ? fmt::format("{:.6f}", 100.0 / percent) : "0");
  rex::cvar::SetFlagByName("native_x_renderer_sharpness", g_plain_sharpness);
  if (log)
    REXLOG_INFO("BF upscaler: Off, the game drawn at {:.0f}% of the display's resolution{}", percent,
                scaled ? " (never below 720 lines)" : "");
}
#if defined(BF_DEV_TOOLS)
const auto g_dev_began = std::chrono::steady_clock::now();  // the process start, for test scripts
#endif

// ---- which controller is in use, for the automatic button prompts (TekRantGaming, 8 Nov evening: like King Kong's launcher,
// the pictures of the controller in the player's hands, the manual choice kept). A press the game saw that no Xbox
// (XInput) controller made came from another kind: which kind, from the controllers Windows lists (Sony's maker ID
// 054C: PlayStation; Valve's 28DE: Steam Deck). Read here, not from the runtime (its device names are private).
namespace {
std::atomic<int> g_pad_family{0};    // 0 Xbox, 1 PlayStation, 2 Steam Deck
std::atomic<int> g_hid_family{0};    // the non Xbox kind connected, from the last scan
std::atomic<bool> g_hid_scanning{false};
std::atomic<double> g_hid_scanned_at{-100.0};

double FamilyClock() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#if defined(_WIN32)
// The kinds of controller Windows lists now (setupapi, loaded when first needed): 1 a Sony one, 2 a Valve one.
int ScanHidFamily() {
  using GetDevs = void*(WINAPI*)(const GUID*, const wchar_t*, HWND, DWORD);
  using EnumIf = BOOL(WINAPI*)(void*, void*, const GUID*, DWORD, void*);
  using GetPath = BOOL(WINAPI*)(void*, void*, void*, DWORD, DWORD*, void*);
  using Destroy = BOOL(WINAPI*)(void*);
  static HMODULE m = LoadLibraryA("setupapi.dll");
  if (!m) return 0;
  static auto get = reinterpret_cast<GetDevs>(GetProcAddress(m, "SetupDiGetClassDevsW"));
  static auto next = reinterpret_cast<EnumIf>(GetProcAddress(m, "SetupDiEnumDeviceInterfaces"));
  static auto path = reinterpret_cast<GetPath>(GetProcAddress(m, "SetupDiGetDeviceInterfaceDetailW"));
  static auto done = reinterpret_cast<Destroy>(GetProcAddress(m, "SetupDiDestroyDeviceInfoList"));
  if (!get || !next || !path || !done) return 0;
  static const GUID kHid = {0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
  void* set = get(&kHid, nullptr, nullptr, 0x2 | 0x10);  // DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
  if (!set || set == INVALID_HANDLE_VALUE) return 0;
  int found = 0;
  struct {
    DWORD size;
    GUID cls;
    DWORD flags;
    ULONG_PTR reserved;
  } iface{};
  for (DWORD i = 0; i < 256; ++i) {
    iface.size = sizeof(iface);
    if (!next(set, nullptr, &kHid, i, &iface)) break;
    alignas(8) unsigned char buffer[1024];
    *reinterpret_cast<DWORD*>(buffer) = sizeof(void*) == 8 ? 8 : 6;  // cbSize of SP_DEVICE_INTERFACE_DETAIL_DATA_W
    if (!path(set, &iface, buffer, sizeof(buffer), nullptr, nullptr)) continue;
    std::wstring p(reinterpret_cast<const wchar_t*>(buffer + 4));
    for (auto& c : p) c = wchar_t(towlower(c));
    if (p.find(L"vid_054c") != std::wstring::npos) found = 1;
    else if (p.find(L"vid_28de") != std::wstring::npos && found == 0) found = 2;
  }
  done(set);
  return found;
}

// Whether an Xbox (XInput) controller holds a button or trigger now.
bool XInputPressed() {
  using GetStateFn = DWORD(WINAPI*)(DWORD, void*);
  static const GetStateFn get_state = [] {
    HMODULE m = LoadLibraryA("xinput1_4.dll");
    if (!m) m = LoadLibraryA("xinput9_1_0.dll");
    return m ? reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState")) : nullptr;
  }();
  if (!get_state) return false;
  for (DWORD i = 0; i < 4; ++i) {
    struct {
      DWORD packet;
      WORD buttons;
      BYTE lt, rt;
      SHORT lx, ly, rx, ry;
    } st{};
    if (get_state(i, &st) != 0) continue;
    if (st.buttons || st.lt > 30 || st.rt > 30 || std::abs(st.lx) > 16000 || std::abs(st.ly) > 16000) return true;
  }
  return false;
}
#endif

// A controller press was seen (by the game's read, or the launcher's own XInput read): which kind made it.
void ClassifyPadPress(bool from_xinput) {
#if defined(_WIN32)
  const double now = FamilyClock();
  if (now - g_hid_scanned_at.load() > 3.0 && !g_hid_scanning.exchange(true)) {
    g_hid_scanned_at = now;
    std::thread([] {
      g_hid_family = ScanHidFamily();
      g_hid_scanning = false;
    }).detach();
  }
  const int family = from_xinput || XInputPressed() ? 0 : g_hid_family.load();
  if (family != g_pad_family.exchange(family))
    REXLOG_INFO("BF prompts: the controller in use is {}", family == 1 ? "a PlayStation one" : family == 2 ? "a Steam Deck" : "an Xbox one");
#endif
}
}  // namespace

// For the automatic prompts (launcher_kit.cpp, hud_glyphs.cpp): the set of the controller in use.
std::string PadFamily() {
  const int f = g_pad_family.load();
  return f == 1 ? "ps5" : f == 2 ? "steam_deck" : "xbox_series";
}


std::filesystem::path ExecutableDir();  // shader_prep.cpp: the folder of bf1943.exe
std::string LoadedLevelFolder();        // shader_prep.cpp: the level the game has loaded (wake_island_s...)
const char* MapIconsFileName();         // shader_prep.cpp: the pause menu map's icons in cache\extracted\minimap
void PlayMenuMusic(const std::filesystem::path& wav, bool on);  // game_music.cpp
void SetGameAudioMuted(bool muted);  // audio_settings.cpp: the game's own sound silent while our menus show
void PlaySoundOnce(const std::filesystem::path& wav);
}  // namespace bf1943
#if defined(BF_DEV_TOOLS)
REXCVAR_DEFINE_BOOL(bf_dev_auto_host, false, "BF1943",
                    "Tests (BF_AUTO_JOIN=1): host our Custom Match at the main menu");
REXCVAR_DEFINE_STRING(bf_dev_auto_direct, "", "BF1943",
                      "Tests (BF_AUTO_JOIN=1): join this address by Direct connect at the main menu");
#endif
REXCVAR_DEFINE_BOOL(bf_dev_no_auto_join, false, "BF1943",
                    "Tests: no automatic Quick Match on our main menu (one client of two stays there)");
namespace bf1943 {
void StopSounds();
void StartAutoJoin(const std::string& server);  // auto_join.cpp
void StartTutorial();  // auto_join.cpp: the game's own tutorial
void NotePlayedLevel(const std::string& title);  // pipeline_upload.cpp: the levels sent with the pipelines
bool BfFontReady(const std::filesystem::path& game_dir, const std::filesystem::path& user_dir);  // bf_font.cpp
float BfTextWidth(const std::string& text, float cap);
void BfText(ImDrawList* d, ImVec2 top_left, float cap, ImU32 color, const std::string& text);
void BfText(ImDrawList* d, ImVec2 top_left, float cap, ImU32 color, const std::string& text, ImU32 edge);
void BfFontDebug(ImDrawList* d);
// bf_list_shot.cpp: BF's own lists (0 its main menu's, 1 its pause's), copied from the window once.
bool BfListShotTake(int list);
bool BfListShotReady(int list);
bool BfListShotLoad(int list);                      // a copy kept from an earlier start, if there is one for this size
bool BfListShotShowing(int list, int row);          // BF's own list on screen now, `row` selected
int BfListShotSelectedRow(int list);                // BF's own list on screen now: its selected row, or -1
void BfListShotSetDir(const std::filesystem::path& dir);
void BfListShotBar(int list, ImDrawList* d, ImVec2 a, ImVec2 b, bool selected);
void BfListShotLetters(int list, ImDrawList* d, ImVec2 a, ImVec2 b, int bf_row, ImU32 color, ImU32 edge);
bool BfListShotLabel(int list, ImDrawList* d, ImVec2 a, ImVec2 b, const std::string& text, ImU32 color, ImU32 edge);
void BfPaper(ImDrawList* d, ImVec2 a, ImVec2 b, float strength);
void RunPauseAction(int action);
void PressBf(const std::vector<uint16_t>& presses);  // auto_join.cpp: BF's own buttons, one after another
void PressBfTimed(const std::vector<std::pair<double, uint16_t>>& presses);  // each after its own wait (s)
bool BfPressesDone();
constexpr uint16_t kPadA = 0x1000, kPadB = 0x2000, kPadDown = 0x0002, kPadUp = 0x0001, kPadStart = 0x0010;
int MenuDriverStage();
bool InMatch();  // auto_join.cpp: the driver's in game stage
bool LevelReady();
void PressStartForPlayer();  // auto_join.cpp
void MarkExitChosen();
void LeaveMatch();  // auto_join.cpp: Exit Game, the game's own command (doExitLevel)
void SuicideNow();  // the game's own suicide command
void GameCommand(const std::string& name, const std::string& args);  // auto_join.cpp: a command as the game's menu sends it
std::map<std::string, std::pair<int, double>> TakeScoreEvents();  // auto_join.cpp: the HUD's score events
bool LevelLoaded();
bool LoadingScreenShown();  // auto_join.cpp: the level's loading screen is up after a join's load began

void NameChosen(const std::string& name);  // below: the name the player chose on the first start, in use at once
bool PollNameCheck(bool abandon);           // below: the backend's answer to a chosen name, applied once it is in
bool ShadersAfterUpdate();                  // below: the shaders are prepared again after an update
bool ShadersPreparedAtStart();              // below: this version's shaders were prepared before: no shader page
const char* BfVersion();                    // pipeline_upload.cpp

// Whether BF may sign in behind the launcher's pages (auto_join.cpp): a name was chosen and the backend answered.
std::atomic<bool>& LauncherWalks() {
  static std::atomic<bool> walks{false};
  return walks;
}

namespace {

namespace fs = std::filesystem;

std::atomic<bool> g_escape_press{false};  // the keyboard's Esc with our menu closed (TakeEscapePress)
// BF's own menu screen's last "couch" call (auto_join.cpp; steady clock ticks, 0: never): it comes about once a second
// while BF's pause or a screen under it is up, never in play.
std::atomic<int64_t> g_game_menu_signal_at{0};
double SecondsSinceGameMenuSignal() {
  const int64_t at = g_game_menu_signal_at.load(std::memory_order_relaxed);
  if (!at) return 1e9;
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch() -
                                       std::chrono::steady_clock::duration(at))
      .count();
}

constexpr ImU32 kGold = IM_COL32(214, 170, 92, 255), kCream = IM_COL32(234, 226, 208, 255);
constexpr ImU32 kText = IM_COL32(232, 232, 228, 255), kDim = IM_COL32(150, 152, 156, 255);
constexpr ImU32 kDark = IM_COL32(20, 22, 26, 255), kLine = IM_COL32(46, 50, 56, 255);
constexpr ImU32 kGreen = IM_COL32(120, 200, 120, 255);
// BF's own menus (TekRantGaming's pick, 6 Nov 2026: "Original, extended"), measured from screenshots of the original main
// menu and pause menu (runs origmenu1, origpause3): grey bars on the main menu, white ones in the pause, the selected
// one olive green; dialogs are a black title bar over paper; button hints are black tags.
constexpr ImU32 kBfPaper = IM_COL32(236, 235, 230, 255), kBfInk = IM_COL32(13, 13, 13, 255);
constexpr ImU32 kBfGreen = IM_COL32(85, 112, 59, 255), kBfGreenInk = IM_COL32(181, 181, 180, 255);
// BF's rows (runs origmenu3 and origpause2, sampled): the main menu's grey white paper and the pause's lighter one,
// opaque (BF's own list is under ours); the lines between rows near black, two pixels at 1800 lines.
constexpr ImU32 kBfMainBar = IM_COL32(216, 219, 220, 255), kBfPauseBar = IM_COL32(226, 232, 232, 255);
constexpr ImU32 kBfLine = IM_COL32(22, 22, 22, 255);
constexpr ImU32 kBfWhiteBar = IM_COL32(238, 238, 234, 255), kBfGreyBar = IM_COL32(140, 143, 147, 240);
constexpr ImU32 kBfEdge = IM_COL32(112, 112, 110, 255), kBfBlack = IM_COL32(12, 12, 12, 255);
constexpr ImU32 kBfFaint = IM_COL32(118, 116, 110, 255), kBfRed = IM_COL32(150, 36, 28, 255);

struct Fonts {
  ImFont* title = nullptr;   // Bahnschrift, the big BATTLEFIELD
  ImFont* header = nullptr;  // Bahnschrift, spaced capitals
  ImFont* item = nullptr;    // Segoe UI, menu rows
  ImFont* body = nullptr;    // Segoe UI, help and hints
  ImFont* bold = nullptr;    // Segoe UI Semibold
  ImFont* bf = nullptr;      // Oswald Bold (bin\fonts): BF's own menu lettering's look, its lists and titles
  ImFont* bf_med = nullptr;  // Oswald Medium: the settings' rows and help
} g_fonts;

// ---- settings: key = value lines

struct Settings {
  fs::path path;
  std::map<std::string, std::string> values;
  void Load() {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
      const size_t eq = line.find('=');
      if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
      auto trim = [](std::string s) {
        s.erase(0, s.find_first_not_of(" \t"));
        s.erase(s.find_last_not_of(" \t\r") + 1);
        return s;
      };
      values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
  }
  void Save() const {
    std::ofstream f(path, std::ios::trunc);
    f << "# Battlefield 1943 PC settings (our menu)\n";
    for (const auto& [k, v] : values) f << k << " = " << v << "\n";
  }
  std::string Get(const std::string& k, const std::string& def) const {
    auto it = values.find(k);
    return it == values.end() ? def : it->second;
  }
};

// ---- the menu

// kSetup to kAllSet: the first time setup's pages 1 to 9 (game_menu_launcher.inc); kHome on every later start; kMain
// BF's own online menu with our list (Play); kPause in a match.
enum class Page {
  kIntro,  // the startup cards with the logo music (IntroPage), at every launch
  kStats,  // our Stats & Awards (StatsPage), from the main menu
  kAchievements,  // our Achievements (AchievementsPage), from the main menu
  kLeaderboards,  // our Leaderboards (LeaderboardsPage), from the main menu and the pause
  kFriends,  // our Join Match With Friends (FriendsPage), from the main menu
  kScoreboard,  // our Scoreboard (ScoreboardPage), from the pause
  kPrivateMatch,  // our Create Private Match (PrivateMatchPage), from the main menu
  kHelp,          // our Help (HelpPage): the game's tutorial, its controls, the credits
  kControls,      // the game's controller layouts (ControlsPage), from Help and the pause
  kCredits,       // the credits (CreditsPage), from Help
  kSetup, kName, kShaders, kMain, kServers, kSettings, kJoining, kPause, kLeaving,
  kSetupSettings, kPrompts, kVoice, kAllSet, kHome, kProjects, kReport, kStarting, kUpdate
};

#if defined(_WIN32)
// The first start's pickers: the player's package file, or a folder (a console drive's, or an unpacked game). Each
// picker is owned by our window (`owner`, its HWND): in front of it, and the focus back to it when the picker closes
// (GameMenu::Pick).
fs::path BrowseForPackage(void* owner) {
  wchar_t file[MAX_PATH] = L"";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = static_cast<HWND>(owner);
  ofn.lpstrFilter = L"Battlefield 1943 package (any file)\0*.*\0Unpacked game (default.xex)\0default.xex\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrTitle = L"Select your Battlefield 1943 package (in 5841097E\\000D0000)";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  return GetOpenFileNameW(&ofn) ? fs::path(file) : fs::path();
}
fs::path BrowseForFolder(void* owner) {
  fs::path out;
  IFileOpenDialog* dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Select the folder that holds your Battlefield 1943");
    if (SUCCEEDED(dialog->Show(static_cast<HWND>(owner)))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) out = path, CoTaskMemFree(path);
        item->Release();
      }
    }
    dialog->Release();
  }
  return out;
}
// The HD texture pack's file (Settings > Graphics > Import HD texture pack).
fs::path BrowseForHdPack(void* owner) {
  wchar_t file[MAX_PATH] = L"";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = static_cast<HWND>(owner);
  ofn.lpstrFilter = L"Battlefield 1943 Reborn HD texture pack (*.bfhd)\0*.bfhd\0Any file\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrTitle = L"Select the HD texture pack";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  return GetOpenFileNameW(&ofn) ? fs::path(file) : fs::path();
}
#else
fs::path BrowseForPackage(void*) { return {}; }
fs::path BrowseForFolder(void*) { return {}; }
fs::path BrowseForHdPack(void*) { return {}; }
#endif

// Where an imported HD texture pack is kept (bf1943_app.h gives it to the renderer at the start).
fs::path HdPackPath(const fs::path& user_dir) { return user_dir / "hd" / "bf1943-hd.bfhd"; }

// The GPU plugin's shader storage progress (rex_gpu_shader_storage_progress, our SDK copy's rexgpu-xenos and
// rexgpu-bfnative): the pipelines made from the storage at start, which the shader screen shows. Null with a
// plugin that has no such export.
struct ShaderProgress {
  uint32_t queued = 0, done = 0, loading = 0, finished = 0;
};
using ShaderProgressFn = void (*)(uint32_t*, uint32_t*, uint32_t*, uint32_t*);
ShaderProgressFn FindShaderProgress() {
  for (const char* name : {"xenos", "bfnative"}) {
#if defined(_WIN32)
    if (HMODULE m = GetModuleHandleA((std::string("rexgpu-") + name + ".dll").c_str()))
      if (auto f = reinterpret_cast<ShaderProgressFn>(GetProcAddress(m, "rex_gpu_shader_storage_progress"))) return f;
#else
    if (void* m = dlopen((std::string("librexgpu-") + name + ".so").c_str(), RTLD_NOW | RTLD_NOLOAD)) {
      auto f = reinterpret_cast<ShaderProgressFn>(dlsym(m, "rex_gpu_shader_storage_progress"));
      dlclose(m);
      if (f) return f;
    }
#endif
  }
  return nullptr;
}

struct Input {
  bool up = false, down = false, left = false, right = false, accept = false, back = false;
  bool prev_tab = false, next_tab = false;  // LB and RB (Q and E): the settings' tabs
  bool x = false, y = false, start = false, view = false;  // the launcher's pages (F, R, P and Tab on the keyboard)
};

class GameMenu final : public rex::ui::ImGuiDialog {
 public:
  GameMenu(rex::ui::ImGuiDrawer* drawer, fs::path settings_path, std::function<void(bool)> fullscreen,
           std::function<void()> quit, std::function<rex::ui::Window*()> window, std::function<fs::path()> game_dir)
      : ImGuiDialog(drawer), fullscreen_(std::move(fullscreen)), quit_(std::move(quit)), window_(std::move(window)),
        game_dir_(std::move(game_dir)) {
    settings_.path = std::move(settings_path);
    settings_.Load();
    MigrateInvert041();
    RefreshGameFlipY();
    BfListShotSetDir(settings_.path.parent_path() / "cache" / "extracted");
    kit::SetMenuArtDir(settings_.path.parent_path() / "cache" / "extracted" / "menu");  // the game's own menu art
    // A saved antialiasing mode no longer offered shows as FXAA, which applies (ApplyStartupDisplaySettings).
    if (auto it = settings_.values.find("cvar.native_x_renderer_antialiasing");
        it == settings_.values.end() || (it->second != "off" && it->second != "fxaa"))
      settings_.values["cvar.native_x_renderer_antialiasing"] = "fxaa";
    settings_.values.erase("cvar.native_x_renderer_frame_generation");
    NoteStartValues();
    StartFrameCapRead();
    ApplyFrameRate();
    open_ = true;  // the launcher is the main menu: shown from the first frame
    // The SDK plugin's storage only runs on the Xenos path (under NativeXRenderer it never finishes); NativeXRenderer
    // reports its own: the stored pipelines (shader_prep.cpp writes them from the player's game files on a first
    // start) made at its first draw (D134).
    shader_progress_ = REXCVAR_GET(native_x_renderer) ? &native_x_renderer::StorageProgress : FindShaderProgress();
    page_ = Page::kShaders;  // until the storage's pipelines are made; then the setup or Home (AfterShaders)
    // A setup closed half way counts as done once a name was chosen (TekRantGaming's rule for King Kong). Not saved here:
    // the settings file is never written from the constructor (King Kong's pitfall 3).
    if (settings_.Get("launcher_setup", "") == "started" && settings_.Get("player_name_chosen", "") == "1")
      settings_.values["launcher_setup"] = "done";
    // Started again to join a server of the other tick rate (TickPopup, OwnBackendPopup): this run is at it.
    if (const char* tick = std::getenv("BF_JOIN_TICK"); tick && *tick) SetTickRate(std::atoi(tick));
    if (const char* address = std::getenv("BF_JOIN_ADDRESS"); address && *address) tick_join_address_ = address;
    // Started again to join a server on its own backend (OwnBackendPopup): this run is on that backend.
    if (const char* own = std::getenv("BF_OWN_BACKEND"); own && *own) {
      UseOwnBackend(own);
      if (const char* port = std::getenv("BF_OWN_BACKEND_JOIN"); port && *port) own_join_port_ = port;
    }
    // Read once: a later restart of this run (Restart to apply, an update) must not join that server again (the
    // review of pull request 16: the variables were passed on to every restart after).
    for (const char* name : {"BF_JOIN_TICK", "BF_JOIN_ADDRESS", "BF_OWN_BACKEND", "BF_OWN_BACKEND_JOIN"}) {
#if defined(_WIN32)
      _putenv_s(name, "");
#else
      unsetenv(name);
#endif
    }
    // What's new once after an update (King Kong's rule, launcher.cpp: the settings say the launcher has run before
    // and an older version ran last; never on a first install, whose setup's end notes this version). It opens on
    // the main menu once this version's shaders are prepared (WhatsNewTick).
    if (settings_.Get("launcher_setup", "") == "done" && settings_.Get("last_version", "") != BfVersion())
      whats_new_from_ = settings_.Get("last_version", ""), whats_new_pending_ = true;
#if defined(BF_DEV_TOOLS)
    if (!shader_progress_) {  // BF_MENU_PAGE / BF_MENU_TAB at once when there is no shader screen (screenshots)
      if (const char* v = std::getenv("BF_MENU_PAGE"); v && std::string(v) == "settings") page_ = Page::kSettings;
      if (const char* v = std::getenv("BF_MENU_TAB"); v && *v) tab_ = std::clamp(std::atoi(v), 0, int(kTabCount) - 1);
    }
#endif
    // The fullscreen mode (Borderless or a Fullscreen resolution) before the window first goes fullscreen; the
    // window mode itself was set before the window opened (ApplyStartupDisplaySettings).
    if (window_ && window_())
      ApplyFullscreenMode(window_(), std::atoi(settings_.Get("monitor", "0").c_str()),
                          settings_.Get("fullscreen_mode", "desktop"));
  }

  // A file picker over our window (TekRantGaming, 10 Nov 2026: picking the game files in the setup, with 0.1.30's exclusive
  // Fullscreen, left the game minimised behind the closed picker, looking crashed; the install itself had finished).
  // An exclusive Fullscreen steps down to Borderless while the picker is open (Windows minimises an exclusive window
  // that loses the focus) and back after; the picker is owned by our window, so the focus comes back to it.
  template <typename Browse>
  fs::path Pick(Browse browse) {
    rex::ui::Window* window = window_ ? window_() : nullptr;
    const std::string mode = settings_.Get("fullscreen_mode", "desktop");
    const int monitor = std::atoi(settings_.Get("monitor", "0").c_str());
    const bool exclusive = window && window->IsFullscreen() && ParseDisplayMode(mode).has_value();
    if (exclusive) ApplyFullscreenMode(window, monitor, "desktop");
    const fs::path picked = browse(window ? window->GetNativeWindowHandle() : nullptr);
    if (exclusive) ApplyFullscreenMode(window, monitor, mode);
    REXLOG_INFO("BF launcher: a file picker closed ({}{})", picked.empty() ? "nothing picked" : "picked",
                exclusive ? "; Fullscreen stepped down to Borderless while it was open" : "");
    return picked;
  }

  // The driver reached BF's online menu (or came back to it after a match). A join picked while the game was still
  // starting goes on from here (OnDraw).
  void Show() {
    open_ = true;
    // The game's own options as saved (it starts each run from its defaults): sent from our next draw, as this runs
    // in the menu driver's step, under its lock, which GameCommand takes too.
    if (!signed_in_once_) send_game_settings_ = true;
    signed_in_once_ = true;
    if (page_ == Page::kJoining && !failed_) return;
    // Signed in under the launcher's pages (BF's menu is up behind them): they stay; Play goes on from here.
    if (LauncherPage(page_) && page_ != Page::kStarting) {
      main_cursor_ = 0;
      return;
    }
    if (page_ == Page::kStarting) {
      page_ = Page::kMain, row_ = 0, main_cursor_ = 0;
      return;
    }
    // BF's menu reached while the shaders are still being made: the shader screen stays until they are done (its page
    // then goes on to ours).
    if (page_ == Page::kShaders) {
      main_cursor_ = 0;
      return;
    }
    page_ = Page::kMain, row_ = 0, from_pause_ = false, lost_ = false;
    main_cursor_ = 0;  // BF's own list, (re)entered, is on its first row
  }
  void ShowLost() { open_ = true, page_ = Page::kLeaving, lost_ = true; }  // the server went away during the match
  void ShowPause() {
    open_ = true, page_ = Page::kPause, pause_row_ = 0, pause_focus_panel_ = false, handing_over_ = -1;
    pause_confirm_ = -1;
    pause_opened_at_ = ImGui::GetTime();
  }
  bool open() const { return open_; }
  // For the graphics panel (f8_panel.cpp): the launcher's own pages are up; the settings file.
  bool ShowingLauncher() const { return open_ && LauncherPage(page_); }
  bool KeyboardLast() const { return device_.load(std::memory_order_relaxed) == Device::kKeyboard; }
  // The game's own Vertical look is Inverted (GstInput Scheme2FlipY; RefreshGameFlipY keeps it), for look.cpp.
  bool GameFlipY() const { return game_flip_y_.load(std::memory_order_relaxed); }
  // In a match with our menu closed (open, TrackDevice decides): the device the player uses, from the game's own pad
  // reads (auto_join.cpp: a press or a push on the controller itself, or a key or mouse button through the keyboard
  // and mouse driver) and the mouse's movement (look.cpp). The prompts in the game's HUD follow it (hud_glyphs.cpp
  // StyleSet, checked twice a second), so a player who picks up the mouse mid match sees keys, and the chosen
  // controller's buttons again as soon as the controller is used (TekRantGaming, 12 Nov 2026).
  void NoteMatchDevice(bool keyboard) {
    const Device to = keyboard ? Device::kKeyboard : Device::kPad;
    if (device_.load(std::memory_order_relaxed) == to) return;
    device_.store(to, std::memory_order_relaxed);
    REXLOG_INFO("BF prompts: now the {}'s (in the match)", keyboard ? "keyboard and mouse" : "controller");
  }
  std::string Setting(const std::string& key, const std::string& def) const { return settings_.Get(key, def); }
  void SetSetting(const std::string& key, const std::string& value) {
    settings_.values[key] = value;
    settings_.Save();
  }
  fs::path UserDir() const { return settings_.path.parent_path(); }
  bool passing() {
    std::lock_guard<std::mutex> lock(pad_mutex_);
    return passing_ >= 0;
  }

  // Player 1's pad, from the game's own pad reads: edges become our input.
  void Pad(uint16_t buttons, int16_t lx, int16_t ly, bool from_game = false) {
    std::lock_guard<std::mutex> lock(pad_mutex_);
    if (from_game) game_pad_at_ = std::chrono::steady_clock::now();
    uint16_t dirs = buttons;
    if (ly > 16000) dirs |= 0x0001;
    if (ly < -16000) dirs |= 0x0002;
    if (lx < -16000) dirs |= 0x0004;
    if (lx > 16000) dirs |= 0x0008;
    const uint16_t pressed = dirs & ~last_pad_;
    last_pad_ = dirs;
    if (!open_) {
      Passing(pressed);
      return;
    }
    pad_.up |= (pressed & 0x0001) != 0;
    pad_.down |= (pressed & 0x0002) != 0;
    pad_.left |= (pressed & 0x0004) != 0;
    pad_.right |= (pressed & 0x0008) != 0;
    pad_.accept |= (pressed & 0x1000) != 0;     // A
    pad_.start |= (pressed & 0x0010) != 0;      // Start (also A on BF's own pages, OnDraw)
    pad_.view |= (pressed & 0x0020) != 0;       // Back (View)
    pad_.back |= (pressed & 0x2000) != 0;       // B
    pad_.x |= (pressed & 0x4000) != 0;          // X
    pad_.y |= (pressed & 0x8000) != 0;          // Y
    pad_.prev_tab |= (pressed & 0x0100) != 0;   // LB
    pad_.next_tab |= (pressed & 0x0200) != 0;   // RB
    if (pressed) pad_pressed_ = true, classify_press_ = from_game ? 1 : 2;
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    // The keyboard's Esc in a match with our menu closed opens the pause, as the controller's Start does (auto_join.cpp
    // takes it at the next pad read; only with the soldier in the world, as Start).
    if (!open_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !comms::TypingOrJustClosed())
      g_escape_press.store(true, std::memory_order_relaxed);  // not the chat box's Esc (comms_ui.cpp)
    UpdateWalk(io);
    if (hd_import_->turn_on.exchange(false)) SetCvar("native_x_renderer_hd_textures", "true");
    // D150: the GPU's speed, timed once (the log keeps it) after the stored pipelines are made and 3 s of steady
    // frames (none over 50 ms). Timed while they compiled (a first start, or the first after an update), an RTX 4070
    // Ti took 13.3 ms instead of 5.8 (run smoke122); 2 s after them, with the game still loading, 8.7 (smoke122b): the
    // GPU idles between uneven frames and its clock falls. Not before the game runs: the launcher shows before the
    // renderer exists (the request was dropped, so the first start's auto detect, in the launcher, has no time yet and
    // keeps the fast tier).
    // Timed only while an auto detect waits for it (a short heavy load: a second or so on a GTX 1060), and in test
    // builds for the log.
#if defined(BF_DEV_TOOLS)
    const bool want_time = true;
#else
    const bool want_time = settings_.Get("auto_detect_gpu", "") == "pending";
#endif
    // A prepared start (no shader page): when the renderer has made its stored pipelines behind the menus.
    if (shaders_behind_ && shader_progress_) {
      ShaderProgress sp;
      shader_progress_(&sp.queued, &sp.done, &sp.loading, &sp.finished);
      if (sp.finished) {
        shaders_behind_ = false;
        REXLOG_INFO("BF shaders: {} stored pipelines made behind the menus, {:.1f} s after the menu came up", sp.done,
                    ImGui::GetTime() - shaders_behind_from_);
      }
    }
    if (!gpu_benchmark_started_ && want_time && !REXCVAR_GET(server)) {
      if (page_ == Page::kShaders || shaders_behind_ || pre_game_ || io.DeltaTime > 0.05f)
        gpu_benchmark_at_ = ImGui::GetTime() + 3.0;
      else if (ImGui::GetTime() >= gpu_benchmark_at_) gpu_benchmark_started_ = true, native_x_renderer::GpuBenchmarkStart();
    }
    if (!REXCVAR_GET(server)) AutoDetectWhenTimed(), NoteNisSupport();
#if defined(BF_DEV_TOOLS)
    // BF_DEV_HD_IMPORT=<pack> (tests): Settings > Graphics > Import HD texture pack, as if chosen, once.
    {
      static bool done = false;
      if (!done && std::getenv("BF_DEV_HD_IMPORT") && ImGui::GetFrameCount() > 30) done = true, ImportHdPack();
    }
    // BF_DEV_HD_FLIP=<seconds> (tests): HD textures off and on by turns (not saved), for screenshots of the same view.
    if (const char* v = std::getenv("BF_DEV_HD_FLIP"); v && *v) {
      static double next = 0;
      static bool on = true;
      const double now = ImGui::GetTime();
      if (next == 0) next = now + std::atof(v);
      if (now >= next) {
        on = !on, next = now + std::atof(v);
        rex::cvar::SetFlagByName("native_x_renderer_hd_textures", on ? "true" : "false");
        REXLOG_INFO("BF dev HD flip: HD textures {}", on ? "on" : "off");
      }
    }
    // BF_DEV_AUTODETECT=1 (tests): the auto detect once the GPU's time is known; =pre: at the launcher's first frame,
    // before the game runs, as the first start's setup makes it (Start on its settings pages).
    {
      static bool done = false;
      double ms = 0;
      const char* v = std::getenv("BF_DEV_AUTODETECT");
      if (!done && v && std::string(v) == "pre" && pre_game_) done = true, AutoDetect();
      if (!done && v && std::string(v) != "pre" && native_x_renderer::GpuBenchmarkResult(&ms)) done = true, AutoDetect();
    }
#endif
    if (!open_) PauseTest();
    // A frame limit outside the game found since (the driver's read, or measured): the rate offered at most.
    if (SystemFrameCap() != applied_cap_) ApplyFrameRate();
    if (!open_) MenuSteps();  // tests: their bfb and bfa steps act while BF's own screen has the pad
    if (!open_) ReopenPause();
    // A new match: BF's pause list starts on its first row again.
    if (const int stage = MenuDriverStage(); stage != last_stage_) {
      if (stage == 5) bf_cursor_ = 0;
      // Back at BF's online menu (the match over, or its join failed): no Night Ops server's night here. A map change
      // on the server never comes back here, so its next map stays at night.
      if (stage == 2 && last_stage_ > 2) SetJoinedServerNight(false);
      last_stage_ = stage;
    }
    if (page_ == Page::kJoining) {
      const int stage = MenuDriverStage();
      if (!pending_.empty() && stage == 2) {  // the game is at its menu now
        // The backend keeps a reservation for five minutes, and with its version gate gives a server only to a game
        // that has one: a start that took long (the shaders of a first start) asks again.
        if (ImGui::GetTime() - reserved_at_ > 120 && !joined_gid_.empty()) {
          const bool again = list_.Reserve(joined_gid_) == ServerList::Reserved::kYes;
          REXLOG_INFO("BF join: the reservation made again after {:.0f} s: {}", ImGui::GetTime() - reserved_at_, again ? "kept" : "refused");
          reserved_at_ = ImGui::GetTime();
        }
        StartAutoJoin(pending_), pending_.clear();
      }
      if (tutorial_pending_ && stage == 2) {  // Help's tutorial picked before the game ran: it is at its menu now
        tutorial_pending_ = false;
        REXLOG_INFO("BF help: the tutorial (the game's startTutorial) after the game's start");
        bf1943::StartTutorial();
      }
      // The level is loading: BF's own loading screen, then its Loading complete and spawn screens are the player's
      // (TekRantGaming, 7 Nov 2026: ours covered them and took the class pick away). Tests spawn by themselves (LevelReady).
      // Ours gives way once that loading screen is up: for a second or two after the load begins the game's own menu
      // is still on screen with its "Joining game" box (TekRantGaming, 10 Nov: the old menu showed; run launchaudio1), so our
      // page stays over it until the load's first progress, 10 s at most.
      if (stage == 5) {
        if (joining_load_at_ < 0) joining_load_at_ = ImGui::GetTime();
        if (LoadingScreenShown() || ImGui::GetTime() - joining_load_at_ > 10)
          open_ = false, page_ = Page::kMain, joining_load_at_ = -1;
      } else {
        joining_load_at_ = -1;
      }
      if (stage == 6) failed_ = true;  // kFailed
    }
    // Until BF's online menu is up our menu stays closed: the game's own films, title and sign in show as they are,
    // and the player presses Start (TekRantGaming, 7 Nov 2026: no bare menu over them). Its first start's setup and shader
    // screens come first.
    if (open_ && page_ == Page::kMain && MenuDriverStage() < 2) open_ = false;
    // A click is Start there too (the keyboard's Return already is: the SDK's keybind_start), and the autosave
    // notice's OK after it.
    if (!open_ && MenuDriverStage() <= 1 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) PressStartForPlayer();
    if (send_game_settings_.exchange(false)) SendGameSettings();
    // The menu music from the player's game files (game_music.cpp) while our menus show outside a match: before the
    // game runs, behind our Getting ready page while it starts and signs in, in our main menu and its pages, and while
    // a join connects; the game's own sound is silent meanwhile (audio_settings.cpp), so its start's logo sound and
    // title music and its hidden menu's music never play over ours (TekRantGaming, 10 Nov). From a level's load on (stage 5:
    // the match, its pause) the game's sound is back and ours stops. BF_DEV_MENU_MUSIC=1 (tests): music on as well.
    {
      static const bool dev_music = [] {
        const char* v = std::getenv("BF_DEV_MENU_MUSIC");
        return v && *v == '1';
      }();
      const bool in_match = MenuDriverStage() == 5 && page_ != Page::kJoining;  // the joining page is ours to its end
      const bool our_menus = page_ != Page::kIntro && !in_match && (pre_game_ || open_);
      bf1943::SetGameAudioMuted(our_menus && !pre_game_);
      const bool want = our_menus && (Cvar("bf_menu_music") != "false" || dev_music);
      PlayMenuMusic(UserDir() / "cache" / "extracted" / "music" / "menu.wav", want);
    }
    // Every frame, the menu open or not (Yes closes the pause: the check never ran, run v41team1).
    if (team_suicide_at_ > 0 && ImGui::GetTime() >= team_suicide_at_) {  // after Change Team (ChangeTeamNow)
      team_suicide_at_ = -1;
      // Only when the team did change and the player is still in the match (the review of pull request 16: a refused
      // switch, or leaving within the two seconds, still killed the soldier).
      if (InMatch() && comms::Team() == team_wanted_) {
        REXLOG_INFO("BF pause: the game's suicide after the change of team, to spawn at the new base");
        SuicideNow();
      } else {
        REXLOG_INFO("BF pause: no change of team to team {} (now {}): no suicide", team_wanted_, comms::Team());
      }
    }
    if (!open_) {
      was_open_ = false;
      return;
    }
    if (!was_open_) {
      // Input left over from before the menu opened is not this page's. A test's step that fell while the menu was
      // closed (MenuSteps runs then too, for its bfa and bfb steps) stayed in pad_ and closed the pause in the frame it
      // opened, as RESUME (runs zipjoin1, pauserepl1 and 2, 12 and 13 Nov 2026: the zip's pause check failed on it).
      // A controller's own presses are already dropped while the menu is closed (Pad), so only tests met this.
      std::lock_guard<std::mutex> lock(pad_mutex_);
      pad_ = {}, test_key_ = {}, pad_pressed_ = false;
      was_open_ = true;
    }
    // BF_AUTO_JOIN=1 (test runs): Play online as soon as the server list has a server.
    static const bool auto_join = [] {
      const char* v = std::getenv("BF_AUTO_JOIN");
      return v && *v == '1';
    }();
#if defined(BF_DEV_TOOLS)
    if (auto_join && !auto_joined_ && page_ == Page::kMain && DevAutoPrivate()) auto_joined_ = true;
#endif
    if (auto_join && !auto_joined_ && page_ == Page::kMain && !list_.servers().empty() && !REXCVAR_GET(bf_dev_no_auto_join))
      auto_joined_ = true, QuickJoin();
    PrivateMatchTick();  // our Create Private Match: its server's state, the join, its end
    TickRejoinTick();    // the join asked for before a restart at the server's tick rate
    // The server list is wanted (server_list.h Want) by a match being played and the friends page here; the browser
    // and Quick Play ask for it themselves.
    if (!joined_gid_.empty() || page_ == Page::kFriends) list_.Want();
    // Quick Play before the list was ever asked for: the join once its first answer is in (QuickPlay).
    if (quick_waiting_ > 0) {
      if (list_.answers() > quick_asked_at_) {
        quick_waiting_ = 0;
        if (!QuickJoin()) {
          menu_error_ = list_.reachable() ? "No server has room right now. Try again in a moment, or open the server browser."
                                         : "The servers do not answer. Check your internet connection.";
          menu_error_at_ = ImGui::GetTime();
        }
      } else if (ImGui::GetTime() > quick_waiting_) {
        quick_waiting_ = 0;
        menu_error_ = "The servers do not answer. Check your internet connection.", menu_error_at_ = ImGui::GetTime();
      }
    }

    PauseTest();
    MenuSteps();
    // Before the game runs (the first start's pages) nothing reads the pad for us: read it here. On the launcher's
    // pages the game behind may not read it either (its intro films: the name page took no controller, TekRantGaming, 8 Nov),
    // so with no read from the game for 0.3 s the launcher reads it itself.
    bool game_reads;
    {
      std::lock_guard<std::mutex> lock(pad_mutex_);
      game_reads = std::chrono::steady_clock::now() - game_pad_at_ < std::chrono::milliseconds(300);
    }
    const bool host_reads = pre_game_ || (LauncherPage(page_) && !game_reads);
    if (host_reads != host_reads_ && LauncherPage(page_)) {
      host_reads_ = host_reads;
      REXLOG_INFO("BF launcher: the controller read by {}", host_reads ? "the launcher (the game is not reading it)" : "the game");
    }
#if defined(BF_DEV_TOOLS)
    // BF_DEV_SCRIPTED=1 (tools\run_game.ps1's test runs): only the scripted steps drive the launcher. The PC's own
    // controller and mouse are not read: a test window at the back took the person's mouse over it and their
    // controller (11 Nov 2026: Home's cursor moved to Report an issue, so a scripted A opened its links).
    static const bool scripted = [] {
      const char* v = std::getenv("BF_DEV_SCRIPTED");
      return v && *v == '1';
    }();
    if (scripted) {
      // The mouse is the scripted steps' only (move/, click/, mdown/, mup/: MenuSteps), where they put it, moved on
      // two frames so the hover follows it. (13 Nov 2026: this guard blanked the steps' own mouse too, so a scripted
      // click landed nowhere.)
      const bool both = script_mouse_.x > -FLT_MAX && script_mouse_was_.x > -FLT_MAX;
      io.MousePos = script_mouse_;
      io.MouseDelta = both ? ImVec2(script_mouse_.x - script_mouse_was_.x, script_mouse_.y - script_mouse_was_.y) : ImVec2(0, 0);
      script_mouse_was_ = script_mouse_;
      script_mouse_ = script_mouse_to_;
    }
#else
    constexpr bool scripted = false;
#endif
    if (host_reads && !scripted) HostPad();
    Input pad, key;
    bool pad_pressed = false;
    {
      std::lock_guard<std::mutex> lock(pad_mutex_);
      pad = pad_, key = test_key_;
      pad_ = {}, test_key_ = {};
      pad_pressed = pad_pressed_;
      pad_pressed_ = false;
    }
    // The keyboard: arrows, Enter and Esc everywhere; the launcher's letters (F, R, P, Tab, Q and E) where nothing is
    // typed. Not in a scripted test (BF_DEV_SCRIPTED above).
    const bool typing = page_ == Page::kName || !capture_.empty() ||
                        (page_ == Page::kServers && (browser_typing_ || browser_tab_ == kTabDirect)) ||  // its search, an address
                        (page_ == Page::kPrivateMatch && PmTyping());  // the match's name, its port
    if (!scripted) {
      key.up |= ImGui::IsKeyPressed(ImGuiKey_UpArrow);
      key.down |= ImGui::IsKeyPressed(ImGuiKey_DownArrow);
      key.left |= ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
      key.right |= ImGui::IsKeyPressed(ImGuiKey_RightArrow);
      key.accept |= ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
      // Not while the Esc that cleared a binding's slot is still held (its key repeat backed out of the page: the
      // review of pull request 16).
      if (!ImGui::IsKeyDown(ImGuiKey_Escape)) esc_swallow_ = false;
      key.back |= ImGui::IsKeyPressed(ImGuiKey_Escape) && !esc_swallow_;
      if (!typing) {
        key.prev_tab |= ImGui::IsKeyPressed(ImGuiKey_Q);
        key.next_tab |= ImGui::IsKeyPressed(ImGuiKey_E);
        if (LauncherPage(page_) || page_ == Page::kMain || page_ == Page::kServers || page_ == Page::kSettings) {  // the new look's pages too
          key.x |= ImGui::IsKeyPressed(ImGuiKey_F, false);
          key.y |= ImGui::IsKeyPressed(ImGuiKey_R, false);
          key.start |= ImGui::IsKeyPressed(ImGuiKey_P, false);
          key.view |= ImGui::IsKeyPressed(ImGuiKey_Tab, false);
        }
      }
    }
    // The kind of controller that pressed (the automatic prompts), on this thread: XInput reads of empty slots are slow.
    if (const int c = classify_press_.exchange(0)) ClassifyPadPress(c == 2);
    TrackDevice(io, pad_pressed, key);
    Input in = MergeInput(pad, key);
    // The pause button closes our pause from any of its pages and cards, as it opened it (TekRantGaming, 10 Nov 2026: a second
    // Start showed the game's own pause). Never A there: on the Exit Game question it was YES. auto_join.cpp keeps the
    // press from the game until it is let go.
    if (in.start && InPause()) {
      ClosePause("Start");
      return;
    }
    if (!LauncherPage(page_)) in.accept |= in.start;  // BF's own pages: Start is A, as before

    const float s = io.DisplaySize.y / 1080.0f;
    io_scale_ = s;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    // NoInputs: the window never takes ImGui's mouse capture. The runtime turns the controller off while ImGui wants
    // the mouse (rex_app.cpp, SetActiveCallback), and this window covers the screen, so with the pointer over the
    // game the pad was ignored (TekRantGaming, 5 Nov). Our rows read the mouse themselves (IsMouseHoveringRect, clicks).
    ImGui::Begin("##bf_menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs);
    ImDrawList* d = ImGui::GetWindowDrawList();
#if defined(BF_DEV_TOOLS)
    if (LauncherPage(page_)) DevTextInputCheck();
#endif
    // A pop-up over whatever page is up (the version gate's UPDATE NEEDED, AskToUpdate; What's new after an update,
    // WhatsNewTick): that page is drawn without the buttons, the mouse's button, its movement and its wheel, which
    // are the pop-up's (ImGui's IsMouseClicked asks MouseDown first).
    const bool over_popup = update_popup_ || whats_new_open_ || own_popup_ || tick_popup_ || exp_popup_;
    const Input popup_in = in;
    const bool popup_down = io.MouseDown[0];
    const ImVec2 popup_delta = io.MouseDelta;
    const float popup_wheel = io.MouseWheel;
    if (over_popup) in = {}, io.MouseDown[0] = false, io.MouseDelta = ImVec2(0, 0), io.MouseWheel = 0;
    switch (page_) {
      case Page::kIntro: IntroPage(d, io, in); break;
      case Page::kStats: StatsPage(d, io, in); break;
      case Page::kAchievements: AchievementsPage(d, io, in); break;
      case Page::kLeaderboards: LeaderboardsPage(d, io, in); break;
      case Page::kFriends: FriendsPage(d, io, in); break;
      case Page::kScoreboard: ScoreboardPage(d, io, in); break;
      case Page::kPrivateMatch: PrivateMatchPage(d, io, in); break;
      case Page::kHelp: HelpPage(d, io, in); break;
      case Page::kControls: ControlsPage(d, io, in); break;
      case Page::kCredits: CreditsPage(d, io, in); break;
      case Page::kSetup: FilesPage(d, io, in); break;
      case Page::kName: AccountPage(d, io, in); break;
      case Page::kShaders: ShadersPage(d, io, in); break;
      case Page::kSetupSettings: SetupSettingsA1Page(d, io, in); break;
      case Page::kPrompts: PromptsA1Page(d, io, in); break;
      case Page::kVoice: VoicePage(d, io, in); break;
      case Page::kAllSet: ReadyPage(d, io, in); break;
      case Page::kHome: MenuPage(d, io, in); break;
      case Page::kProjects: ProjectsPage(d, io, in); break;
      case Page::kReport: ReportPage(d, io, in); break;
      case Page::kStarting: StartingPage(d, io, in); break;
      case Page::kUpdate: UpdatePage(d, io, in); break;
      case Page::kMain: MenuPage(d, io, in); break;
      case Page::kServers: BrowserPage(d, io, in); break;
      case Page::kSettings: SettingsA1Page(d, io, in); break;
      case Page::kJoining: JoiningPage(d, s, io, in); break;
      case Page::kPause: PauseA1Page(d, io, in); break;
      case Page::kLeaving: LeavingPage(d, s, io); break;
    }
    if (over_popup) {
      io.MouseDown[0] = popup_down, io.MouseDelta = popup_delta, io.MouseWheel = popup_wheel;
      if (update_popup_) UpdatePopup(d, io, popup_in);
      else if (own_popup_) OwnBackendPopup(d, io, popup_in);
      else if (exp_popup_) ExperimentalPopup(d, io, popup_in);
      else if (tick_popup_) TickPopup(d, io, popup_in);
      else if (whats_new_open_) WhatsNewPopup(d, io, popup_in);
    }
    // The stamp on BF's own menus too: its main menu (and our pages over it) and the pause; never over a match.
    if (kit::WatermarkOn() && open_ &&
        (page_ == Page::kStats ||
         page_ == Page::kAchievements || page_ == Page::kLeaderboards || page_ == Page::kFriends ||
         page_ == Page::kScoreboard || page_ == Page::kPrivateMatch || page_ == Page::kJoining || page_ == Page::kLeaving ||
         page_ == Page::kControls || page_ == Page::kCredits)) {  // the new look's pages stamp their own foot
      const Stage st = StageOf(io);
      kit::Stamp(d, ImVec2(io.DisplaySize.x - st(36) - st(130), st(34) + st(32)), st.u, 5, 1.0f, true);
    }
    BfFontDebug(d);
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
  }

 private:
  // ---- drawing helpers

  // The stencil (our headings) is the game's own headline lettering once its font is there (the new look,
  // bf_font.cpp): capitals about 0.72 of the stencil's size, on the same line.
  static bool StencilAsGame(ImFont* f) { return f && f == kit::GetFonts().stencil && kit::GameFontReady(kit::GameFont::kHeadline); }
  static void Text(ImDrawList* d, ImFont* f, float size, ImVec2 at, ImU32 col, const std::string& t) {
    if (StencilAsGame(f)) {
      kit::GameText(d, kit::GameFont::kHeadline, size * 0.72f, ImVec2(at.x, at.y + size * 0.14f), col, t);
      return;
    }
    d->AddText(f, size, at, col, t.c_str());
  }
  static float Width(ImFont* f, float size, const std::string& t) {
    if (StencilAsGame(f)) return kit::GameTextWidth(kit::GameFont::kHeadline, size * 0.72f, t);
    return f->CalcTextSizeA(size, FLT_MAX, 0, t.c_str()).x;
  }
  // Spaced capitals, like King Kong's section titles.
  static float Spaced(ImDrawList* d, ImFont* f, float size, ImVec2 at, ImU32 col, const std::string& t, float gap) {
    float x = at.x;
    for (size_t i = 0; i < t.size();) {
      size_t n = 1;  // one UTF-8 character
      while (i + n < t.size() && (static_cast<unsigned char>(t[i + n]) & 0xC0) == 0x80) ++n;
      const std::string ch = t.substr(i, n);
      d->AddText(f, size, ImVec2(x, at.y), col, ch.c_str());
      x += Width(f, size, ch) + gap;
      i += n;
    }
    return x;
  }

  void Shade(ImDrawList* d, ImGuiIO& io, float left, float right) {
    const ImVec2 sz = io.DisplaySize;
    d->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(sz.x * 0.75f, sz.y), IM_COL32(8, 10, 14, int(255 * left)),
                               IM_COL32(8, 10, 14, int(255 * right)), IM_COL32(8, 10, 14, int(255 * right)),
                               IM_COL32(8, 10, 14, int(255 * left)));
    d->AddRectFilled(ImVec2(sz.x * 0.75f, 0), sz, IM_COL32(8, 10, 14, int(255 * right)));
  }

  // Outside a match our pages have their own opaque background (BF's menu and title are never shown): dark, with a
  // soft warm light from the top right.
  void Backdrop(ImDrawList* d, ImGuiIO& io) {
    const ImVec2 sz = io.DisplaySize;
    d->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(8, 10, 14, 255));
    d->AddRectFilledMultiColor(ImVec2(sz.x * 0.35f, 0), sz, IM_COL32(8, 10, 14, 0), IM_COL32(58, 46, 30, 150),
                               IM_COL32(16, 18, 22, 60), IM_COL32(8, 10, 14, 0));
  }
  bool InMatch() const { return MenuDriverStage() == 5 && !(page_ == Page::kJoining); }
  void Background(ImDrawList* d, ImGuiIO& io, float left, float right) {
    if (InMatch()) Shade(d, io, left, right);
    else Backdrop(d, io);
  }

  // A hint's button in the Button prompts style (Settings > Controls): Xbox letters, PlayStation names, or keys.
  std::string Prompt(const std::string& k) const {
    std::string style = rex::cvar::GetFlagByName("bf_button_prompts");
    if (KeyboardLast()) style = "keyboard";  // the keyboard's keys whenever it was used last (launcher_kit.cpp StyleFor)
    static const std::pair<const char*, const char*> kPlayStation[] = {
        {"A", "Cross"}, {"B", "Circle"}, {"X", "Square"}, {"Y", "Triangle"}, {"LB", "L1"}, {"RB", "R1"}};
    static const std::pair<const char*, const char*> kKeyboard[] = {
        {"A", "Enter"}, {"B", "Esc"}, {"Y", "Y"}, {"LB", "Q"}, {"RB", "E"}};
    std::string out, word;
    auto flush = [&] {
      if (word.empty()) return;
      std::string t = word;
      if (style == "ps5" || style == "ps3" || style == "ps2") {
        for (const auto& [from, to] : kPlayStation) if (word == from) t = to;
      } else if (style == "keyboard") {
        for (const auto& [from, to] : kKeyboard) if (word == from) t = to;
      }
      out += t;
      word.clear();
    };
    for (char c : k) {
      if (c == ' ') flush(), out += c;
      else word += c;
    }
    flush();
    return out;
  }

  void Hints(ImDrawList* d, float s, ImGuiIO& io, const std::vector<std::pair<std::string, std::string>>& keys_in) {
    std::vector<std::pair<std::string, std::string>> keys;
    for (const auto& [k, t] : keys_in) keys.push_back({Prompt(k), t});
    float total = 0;
    for (const auto& [k, t] : keys) total += std::max(34.0f, Width(g_fonts.bold, 17 * s, k) / s + 18) * s + 12 * s +
                                              Width(g_fonts.body, 22 * s, t) + 40 * s;
    float x = io.DisplaySize.x - 80 * s - total + 40 * s, y = io.DisplaySize.y - 70 * s;
    for (const auto& [k, t] : keys) {
      const float w = std::max(34.0f * s, Width(g_fonts.bold, 17 * s, k) + 18 * s);
      d->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + 34 * s), IM_COL32(70, 72, 76, 255), 4 * s);
      Text(d, g_fonts.bold, 17 * s, ImVec2(x + (w - Width(g_fonts.bold, 17 * s, k)) / 2, y + 7 * s), kText, k);
      Text(d, g_fonts.body, 22 * s, ImVec2(x + w + 12 * s, y + 3 * s), kText, t);
      x += w + 12 * s + Width(g_fonts.body, 22 * s, t) + 40 * s;
    }
  }

  void Header(ImDrawList* d, float s, ImGuiIO& io, const std::vector<std::string>& crumbs) {
    float x = 100 * s;
    for (size_t i = 0; i < crumbs.size(); ++i) {
      const bool last = i + 1 == crumbs.size();
      const float end = Spaced(d, g_fonts.header, 34 * s, ImVec2(x, 52 * s), last ? kText : kDim, crumbs[i], 5 * s);
      if (last) d->AddLine(ImVec2(x, 104 * s), ImVec2(end - 5 * s, 104 * s), kGold, 3 * s);
      else Text(d, g_fonts.header, 34 * s, ImVec2(end + 18 * s, 52 * s), kDim, "/"), x = end + 60 * s;
    }
    d->AddLine(ImVec2(100 * s, 112 * s), ImVec2(io.DisplaySize.x - 100 * s, 112 * s), kLine);
  }

  // A menu row; true when it was clicked. The selected row is the cream bar.
  bool Row(ImDrawList* d, float s, float x, float y, float w, const std::string& name, const std::string& sub,
           bool selected, ImU32 color = kText) {
    const float h = selected && !sub.empty() ? 86 * s : 64 * s;
    const ImVec2 a(x, y), b(x + w, y + h);
    const bool hover = ImGui::IsMouseHoveringRect(a, b);
    if (selected) {
      d->AddRectFilled(a, b, kCream);
      Text(d, g_fonts.bold, 34 * s, ImVec2(x + 36 * s, y + 12 * s), kDark, name);
      if (!sub.empty()) Text(d, g_fonts.body, 19 * s, ImVec2(x + 36 * s, y + 54 * s), IM_COL32(80, 76, 68, 255), sub);
    } else {
      Text(d, g_fonts.item, 32 * s, ImVec2(x + 36 * s, y + 14 * s), hover ? kCream : color, name);
    }
    return hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  }

  // ---- BF's own look

  // A text's top for its capitals to sit in the middle of a band of height h (Oswald: ascent 1.193 em, capitals
  // 0.81 em high).
  static float CapTop(float top, float h, float size) { return top + (h - 0.81f * size) * 0.5f - 0.383f * size; }

  // One of BF's list bars: grey (main menu) or white (pause), olive green when selected.
  // BF's own lettering (bf_font.cpp), once the game is installed; Oswald before that.
  bool BfLetters() {
    if (bf_letters_ < 0) {
      const fs::path dir = game_dir_ ? game_dir_() : fs::path();
      if (dir.empty() || !install::Installed(dir)) return false;
      bf_letters_ = BfFontReady(dir, settings_.path.parent_path()) ? 1 : 0;
    }
    return bf_letters_ == 1;
  }

  // One of BF's list rows: its paper (grey white on the main menu, lighter in the pause), the near black line under
  // it, its capitals in BF's lettering 42 pixels high at 1800 lines (BF's size), 22 in from its left; olive green
  // with grey letters when selected.
  bool BfBar(ImDrawList* d, ImVec2 a, ImVec2 b, const std::string& t, bool sel, bool white, bool ours_disabled = false) {
    const bool hover = ImGui::IsMouseHoveringRect(a, b);
    const float px = io_scale_ * 1080.0f / 1800.0f;  // one pixel at 1800 lines
    d->AddRectFilled(a, b, sel ? kBfGreen : white ? kBfPauseBar : kBfMainBar);
    BfPaper(d, a, b, sel ? 0.1f : 0.16f);
    d->AddRectFilled(ImVec2(a.x, b.y - 2 * px), ImVec2(b.x, b.y), kBfLine);
    const float h = b.y - a.y;
    const ImU32 ink = sel ? kBfGreenInk : ours_disabled ? kBfFaint : kBfInk;
    if (BfLetters()) {
      const float cap = 42 * px;
      BfText(d, ImVec2(a.x + 22 * px, a.y + (h - cap) * 0.5f), cap, ink, t);
    } else {
      const float size = std::min(h * 0.62f, 34.0f * io_scale_);
      Text(d, g_fonts.bf, size, ImVec2(a.x + 15 * io_scale_, CapTop(a.y, h, size)), ink, t);
    }
    return hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  }

  // A list's frame: BF's near black edge round its rows.
  void BfListFrame(ImDrawList* d, ImVec2 a, ImVec2 b) {
    const float px = io_scale_ * 1080.0f / 1800.0f;
    d->AddRect(ImVec2(a.x - px, a.y - px), ImVec2(b.x + px, b.y + px), kBfLine, 0, 0, 2 * px);
  }

  // A button as BF draws it: a coloured disc with its letter (A green, B red, X blue, Y yellow); other keys (LB, a
  // key name) as a small grey plate. Returns its width.
  // The launcher's picture for one of BF's buttons when the prompts are not the Xbox 360's (0 when they are, or
  // for a combined hint such as "LB RB").
  float BfPicture(ImDrawList* d, ImVec2 centre_left, float h, const std::string& key_in, bool draw) {
    const kit::Style ks = PromptStyle();
    if (ks == kit::Style::kXbox360) return 0;
    static const std::pair<const char*, kit::Button> kMap[] = {
        {"A", kit::Button::kA},   {"B", kit::Button::kB},   {"X", kit::Button::kX},
        {"Y", kit::Button::kY},   {"LB", kit::Button::kLB}, {"RB", kit::Button::kRB},
        {"START", kit::Button::kStart}};
    for (const auto& [name, b] : kMap)
      if (key_in == name) return kit::Glyph(d, ks, b, KeyFor(b), centre_left, h * 1.15f, draw);
    return 0;
  }
  float BfButton(ImDrawList* d, ImVec2 centre_left, float h, const std::string& key_in) {
    if (const float w = BfPicture(d, centre_left, h, key_in, true)) return w;
    const std::string k = Prompt(key_in);
    const float r = h * 0.5f;
    if (k.size() == 1 && std::string("ABXY").find(k) != std::string::npos) {
      const ImU32 col = k == "A" ? IM_COL32(96, 162, 60, 255) : k == "B" ? IM_COL32(196, 46, 38, 255)
                        : k == "X" ? IM_COL32(44, 104, 204, 255) : IM_COL32(222, 182, 34, 255);
      const ImVec2 c(centre_left.x + r, centre_left.y);
      d->AddCircleFilled(c, r, col);
      d->AddCircle(c, r, IM_COL32(226, 226, 222, 255), 0, std::max(1.0f, r * 0.12f));
      const float size = h * 0.9f;
      Text(d, g_fonts.bf, size, ImVec2(c.x - Width(g_fonts.bf, size, k) * 0.5f, CapTop(c.y - r, h, size)),
           IM_COL32(240, 240, 236, 255), k);
      return h;
    }
    const float size = h * 0.62f, w = Width(g_fonts.bf, size, k) + h * 0.5f;
    d->AddRectFilled(ImVec2(centre_left.x, centre_left.y - r), ImVec2(centre_left.x + w, centre_left.y + r),
                     IM_COL32(96, 98, 102, 255), r * 0.3f);
    Text(d, g_fonts.bf, size, ImVec2(centre_left.x + h * 0.25f, CapTop(centre_left.y - r, h, size)),
         IM_COL32(240, 240, 236, 255), k);
    return w;
  }

  // BF's button hint: a black tag, a little tilted, with the button and what it does (its "A SELECT").
  float BfTag(ImDrawList* d, ImVec2 at, const std::string& key, const std::string& label, float min_w = 0) {
    const float sy = io_scale_, h = 52 * sy, size = 29 * sy, bh = 31 * sy;
    const std::string upper = Upper(label);
    // Measured first, drawn after the tag.
    const std::string k = Prompt(key);
    float kw = BfPicture(d, ImVec2(0, 0), bh, key, false);
    if (kw == 0)
      kw = k.size() == 1 && std::string("ABXY").find(k) != std::string::npos ? bh : Width(g_fonts.bf, bh * 0.62f, k) + bh * 0.5f;
    const float w = std::max(min_w, 12 * sy + kw + 9 * sy + Width(g_fonts.bf, size, upper) + 16 * sy), rise = w * 0.045f;
    d->AddQuadFilled(ImVec2(at.x, at.y + rise), ImVec2(at.x + w, at.y), ImVec2(at.x + w, at.y + h),
                     ImVec2(at.x, at.y + h + rise * 0.6f), kBfBlack);
    const float mid = at.y + h * 0.5f + rise * 0.5f;
    BfButton(d, ImVec2(at.x + 12 * sy, mid), bh, key);
    Text(d, g_fonts.bf, size, ImVec2(at.x + 12 * sy + kw + 9 * sy, CapTop(mid - h * 0.5f, h, size)),
         IM_COL32(238, 238, 234, 255), upper);
    return w;
  }

  // The hints, stacked at the bottom left where BF puts its own.
  // The last one sits exactly over BF's own A SELECT tag (x 7.2% to 15.8%, y 84% to 89% of the screen), so BF's
  // never shows through; the others stack above it.
  void BfTags(ImDrawList* d, ImGuiIO& io, const std::vector<std::pair<std::string, std::string>>& keys) {
    const float step = 56 * io_scale_, min_w = io.DisplaySize.x * 0.168f;
    float y = io.DisplaySize.y * 0.836f - (float(keys.size()) - 1) * step;
    for (const auto& [k, t] : keys) BfTag(d, ImVec2(io.DisplaySize.x * 0.068f, y), k, t, min_w), y += step;
  }

  // BF's dialog (its Connection error box): a black title bar over paper.
  static void BfDialog(ImDrawList* d, ImVec2 a, ImVec2 b, float bar) {
    d->AddRectFilled(ImVec2(a.x + 5, a.y + 7), ImVec2(b.x + 5, b.y + 7), IM_COL32(0, 0, 0, 70));
    d->AddRectFilled(a, ImVec2(b.x, a.y + bar), kBfBlack);
    d->AddRectFilled(ImVec2(a.x, a.y + bar), b, kBfPaper);
    d->AddRect(a, b, IM_COL32(30, 30, 30, 255), 0, 0, 1.5f);
  }

  static std::string Upper(std::string t) {
    for (char& c : t) c = char(std::toupper(static_cast<unsigned char>(c)));
    return t;
  }

  // Before BF's menu scene is up (its films and title): a plain backdrop in its colours, with the name.
  void BfBootBackdrop(ImDrawList* d, ImGuiIO& io) {
    const ImVec2 sz = io.DisplaySize;
    d->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(44, 54, 64, 255));
    const float sy = io_scale_, size = 150 * sy;
    const std::string name = "BATTLEFIELD";
    const float w = Width(g_fonts.bf, size, name), x = (sz.x - w) * 0.5f, y = sz.y * 0.16f;
    for (int dx = -2; dx <= 2; ++dx)
      for (int dy = -2; dy <= 2; ++dy) Text(d, g_fonts.bf, size, ImVec2(x + dx * sy, y + dy * sy), kBfBlack, name);
    Text(d, g_fonts.bf, size, ImVec2(x, y), IM_COL32(160, 162, 164, 255), name);
    const std::string year = "1943";
    Text(d, g_fonts.bf, 64 * sy, ImVec2((sz.x - Width(g_fonts.bf, 64 * sy, year)) * 0.5f, y + 196 * sy), kBfBlack, year);
  }

  // ---- pages

  // The first start (King Kong's setup idea): the player's game is installed once into the game folder, then the
  // game starts. A package (CON or LIVE) is unpacked with its checksums checked; a folder with an unpacked game is
  // used where it is (remembered as game_dir).
 public:
  void BeginSetup(fs::path game_dir, std::function<void(fs::path)> done) {
    setup_dir_ = std::move(game_dir);
    setup_done_ = std::move(done);
    page_ = Page::kSetup, row_ = 0, open_ = true, pre_game_ = true;
  }
  // The launcher before the game (TekRantGaming, 8 Nov evening: "When the launcher is opened the game is not to be loaded in
  // the background"): bf1943_app.h holds the runtime's start (OnFinalizePaths) and hands the start here; the setup or
  // Home show with nothing of the game running (the menu music plays, the controller is read here), and Play starts
  // the game: the shader page while its pipelines build, then the game's own startup cards, title and menus.
  void BeginLauncher(std::function<void()> start) {
    start_game_ = std::move(start);
    pre_game_ = true, launcher_first_ = true, open_ = true;
    REXLOG_INFO("BF launcher: before the game (it starts at Play)");
    if (!kNightOps) SetJoinedServerNight(false);  // night_ops.h: no night in this build, whatever a setting says
    // Run the game, the startup videos, our main menu (TekRantGaming, 10 Nov 2026): the game's own startup cards and logo
    // music, extracted from the player's files (shader_prep.cpp ExtractGameArt), played by us before anything else;
    // the setting Startup cards off (bf_startup_cards = false) skips them.
    if (fs::exists(UserDir() / "cache" / "extracted" / "startup" / "card5.png") &&
        settings_.Get("bf_startup_cards", "true") != "false") {
      page_ = Page::kIntro, intro_at_ = -1;
      return;
    }
    AfterIntro();
  }
  // The intro's end (or none): the setup, or Home.
  void AfterIntro() {
    if (settings_.Get("launcher_setup", "") == "done" && settings_.Get("player_name_chosen", "") == "1") {
      // The shaders first when this version's are not prepared (the first start after an update): then the menu.
      if (PrepareShadersAtStart("at the start, before the main menu")) return;
      page_ = Page::kHome, home_row_ = 0;
      return;
    }
    if (RenameRequired()) return;  // the saved name was refused (profanity): only a new name, not the whole setup
    setup_active_ = true;
    settings_.values["launcher_setup"] = "started";
    settings_.Save();
    BeginAccount(true);
  }
  // The startup cards (TekRantGaming, 10 Nov 2026): the game's rating notice, then the logo cards with the logo music, each
  // logo arriving on a bar's first beat (the music is in 4/4 at about 125 beats a minute, a bar every 1.9 s): EA,
  // DICE, Frostbite, Dolby, our TRG Studios card (shader_prep.cpp MakeTrgCard: TekRantGaming's logo on the player's own card
  // paper), and the game's title on the music's big hit, the climax's first beat. The music is the game's own with its
  // third bar played twice (MakeIntroMusic), so the title still lands on that hit after the extra card; a cache
  // without those two files plays the game's five cards on its music as it is. The square cards (the notice and the
  // title) are drawn 16:9 as the console showed them (TekRantGaming's cards: the game's squares stretched). Each card fades in
  // to its beat and out just before the next; A, B, Start, Esc or a click skips them.
  double intro_at_ = -1;
  bool intro_music_ = false;
  bool intro_trg_ = false;  // the cards with ours: chosen as the music starts (a first start after an update makes
                            // the two files while the notice shows)
  void IntroPage(ImDrawList* d, ImGuiIO& io, const Input& in) {
    const ImVec2 sz = io.DisplaySize;
    const double now = ImGui::GetTime();
    const fs::path dir = UserDir() / "cache" / "extracted" / "startup";
    struct Card {
      const char* file;
      double beat;  // seconds into the music: fully shown
      bool stretch;
    };
    // The drums' hits (the bars' first beats) in the music with the extra bar, and in the game's own.
    static constexpr Card kWithTrg[] = {{"card1", 0.52, false}, {"card2", 2.44, false}, {"card3", 4.42, false},
                                        {"card4", 6.25, false}, {"trg", 8.10, false},   {"card5", 9.91, true}};
    static constexpr Card kGame[] = {{"card1", 0.52, false}, {"card2", 2.44, false}, {"card3", 4.42, false},
                                     {"card4", 6.27, false}, {"card5", 8.08, true}};
    // The notice first, without music; then the music, its first sound a quarter second before EA's hit.
    constexpr double kNotice = 3.0, kIn = 0.25, kOut = 0.30, kNoticeFade = 0.4;
    if (intro_at_ < 0) {
      intro_at_ = now, intro_music_ = false;
      REXLOG_INFO("BF intro: the startup cards");
    }
    const double t = now - intro_at_;
    if (t >= kNotice && !intro_music_) {
      std::error_code ec;
      intro_music_ = true;
      intro_trg_ = fs::exists(dir / "intro.wav", ec) && fs::exists(dir / "trg.png", ec);
      PlaySoundOnce(dir / (intro_trg_ ? "intro.wav" : "logos.wav"));
      REXLOG_INFO("BF intro: the music, {}", intro_trg_ ? "with the TRG Studios card" : "the game's own five cards");
    }
    const bool trg = intro_trg_;
    const Card* cards = trg ? kWithTrg : kGame;
    const int count = trg ? int(std::size(kWithTrg)) : int(std::size(kGame));
    const double music_end = trg ? 14.15 : 12.33;  // the music's last sound (its tail has faded by then)
    d->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(0, 0, 0, 255));
    const bool skip = in.accept || in.back || in.start || ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (t > kNotice + music_end + 0.1 || (skip && t > 0.3)) {
      REXLOG_INFO("BF intro: {} after {:.1f} s", skip ? "skipped" : "done", t);
      StopSounds();
      intro_at_ = -1;
      AfterIntro();
      return;
    }
    const char* file = nullptr;
    bool stretch = false;
    double shown = 0;
    if (t < kNotice) {
      file = "esrb", stretch = true;
      shown = std::min({1.0, t / kNoticeFade, (kNotice - t) / kNoticeFade});
      // The logo cards read while the notice shows, one a frame from 1 s on (nothing drawn: no area), so none is
      // decoded on its own beat.
      static constexpr const char* kWarm[] = {"card1", "card2", "card3", "card4", "trg", "card5"};
      if (t > 1.0) {
        const ImVec2 none(-8, -8);
        kit::DrawImageFile(d, dir / (std::string(kWarm[ImGui::GetFrameCount() % 6]) + ".png"), none, none);
      }
    } else {
      const double m = t - kNotice;
      for (int i = 0; i < count; ++i) {
        const double from = cards[i].beat - kIn, to = i + 1 < count ? cards[i + 1].beat - kIn : music_end;
        if (m < from || m >= to) continue;
        const double out = i + 1 < count ? kOut : 0.9;  // the title fades out with the music
        file = cards[i].file, stretch = cards[i].stretch;
        shown = std::min({1.0, (m - from) / kIn, (to - m) / out});
        break;
      }
    }
    if (!file) return;
    // Each card filling the window's height at 16:9, centred: the 2:1 cards cropped at their sides, the squares
    // stretched.
    const float h = sz.y, w = h * 16.0f / 9.0f;
    const ImVec2 a((sz.x - w) * 0.5f, 0), b((sz.x + w) * 0.5f, h);
    kit::DrawImageFile(d, dir / (std::string(file) + ".png"), a, b, stretch);
    if (shown < 1) d->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(0, 0, 0, int(255 * std::clamp(1 - shown, 0.0, 1.0))));
  }

  // The player's name from Settings > Gameplay or Home's Edit (the setup's own account page is page 3).
  void BeginName(std::function<void()> done) {
    (void)done;
    BeginAccount(false);
  }

 private:
  void FinishSetup(const fs::path& game_dir) {
    REXLOG_INFO("BF setup: the game is in {}", game_dir.string());
    page_ = Page::kShaders, row_ = 0;  // the game starts; its shaders next (AfterShaders without a shader screen)
    pre_game_ = false;
    if (auto done = std::move(setup_done_)) done(game_dir);
  }

  bool pre_game_ = false;  // the first start's pages, before the game runs: our own pad reads (HostPad)
  bool launcher_first_ = false;  // the launcher showed before the game (BeginLauncher); BF signs in only after Play
  bool played_ = false;          // Play started the game: after its shaders the launcher closes
  std::function<void()> start_game_;
  uint16_t host_last_ = 0;
  bool host_reads_ = false;
  std::atomic<int> classify_press_{0};  // a press to classify: 1 from the game's read, 2 from XInput (HostPad)
  std::chrono::steady_clock::time_point game_pad_at_{};  // the game's last pad read (pad_mutex_)

  // The controller, read here while nothing else does (the first start's pages). XInput's buttons are the console's,
  // so Pad() takes them as the game's reads.
  void HostPad() {
#if defined(_WIN32)
    using GetStateFn = DWORD(WINAPI*)(DWORD, void*);
    static const GetStateFn get_state = [] {
      HMODULE m = LoadLibraryA("xinput1_4.dll");
      if (!m) m = LoadLibraryA("xinput9_1_0.dll");
      return m ? reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState")) : nullptr;
    }();
    if (!get_state) return;
    struct {
      DWORD packet;
      WORD buttons;
      BYTE lt, rt;
      SHORT lx, ly, rx, ry;
    } state{};
    // The first controller connected (the player's may not be the first slot).
    // With none, the slots are tried again once a second (a read of an empty slot is slow).
    static DWORD slot = 0;
    static double none_at = -10;
    if (slot == 4 && ImGui::GetTime() - none_at < 1.0) return;
    if (slot == 4 || get_state(slot, &state) != 0) {
      DWORD i = 0;
      while (i < 4 && get_state(i, &state) != 0) ++i;
      slot = i;
      if (i == 4) {
        none_at = ImGui::GetTime();
        return;
      }
    }
#if defined(BF_DEV_TOOLS)
    // BF_DEV_HOST_PAD=<s>:<button>,... (tests): presses on the controller this reads, from the process start (a, b,
    // x, y, start, back, up, down, left, right; 0.12 s each).
    static const std::vector<std::pair<double, uint16_t>> script = [] {
      std::vector<std::pair<double, uint16_t>> out;
      const char* v = std::getenv("BF_DEV_HOST_PAD");
      static const std::pair<const char*, uint16_t> kNames[] = {
          {"start", 0x0010}, {"back", 0x0020}, {"up", 0x0001}, {"down", 0x0002}, {"left", 0x0004},
          {"right", 0x0008}, {"a", 0x1000},    {"b", 0x2000},  {"x", 0x4000},    {"y", 0x8000}};
      for (const char* c = v ? v : ""; *c;) {
        const double at = std::atof(c);
        while (*c && *c != ':') ++c;
        if (*c) ++c;
        uint16_t b = 0;
        for (const auto& [name, bit] : kNames)
          if (!std::strncmp(c, name, std::strlen(name))) {
            b = bit;
            break;
          }
        out.push_back({at, b});
        while (*c && *c != ',') ++c;
        if (*c) ++c;
      }
      return out;
    }();
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_dev_began).count();
    for (const auto& [at, b] : script)
      if (t >= at && t < at + 0.12) state.buttons |= b;
#endif
    host_last_ = state.buttons;
    Pad(state.buttons, state.lx, state.ly);
#else
    // Linux: SDL's gamepads (the runtime's own input backend, input_backend "sdl"; XInput is Windows only, so before
    // this the launcher read no controller there: TekRantGaming, 11 Nov 2026, the ROG Ally's built in controller did nothing on
    // the menus). Every gamepad connected, merged as the runtime merges the one player's (bf1943_app.h): a handheld
    // shows several (the Ally's built in one through InputPlumber, Steam's virtual ones), and any of them may be the
    // one in the player's hands. Buttons by position as XInput's (the bottom face button is A on every layout), the
    // left stick pushed furthest, up positive. The runtime's driver opens the same pads; SDL counts the opens.
    static const bool sdl_ok = [] {
      const bool ok = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
      if (!ok) REXLOG_WARN("BF launcher: SDL's gamepads could not start: {}", SDL_GetError());
      return ok;
    }();
    if (!sdl_ok) return;
    static std::map<SDL_JoystickID, SDL_Gamepad*> pads;
    static double scanned_at = -10;
    SDL_UpdateGamepads();
    if (ImGui::GetTime() - scanned_at >= 1.0) {  // the pads connected, looked over once a second
      scanned_at = ImGui::GetTime();
      int count = 0;
      SDL_JoystickID* ids = SDL_GetGamepads(&count);
      std::map<SDL_JoystickID, SDL_Gamepad*> now;
      for (int i = 0; i < count; ++i) {
        if (auto it = pads.find(ids[i]); it != pads.end()) now[ids[i]] = it->second, pads.erase(it);
        else if (SDL_Gamepad* g = SDL_OpenGamepad(ids[i])) {
          const char* name = SDL_GetGamepadName(g);
          REXLOG_INFO("BF launcher: reads the controller \"{}\" (SDL)", name ? name : "?");
          now[ids[i]] = g;
        }
      }
      SDL_free(ids);
      for (auto& [id, g] : pads) {  // gone
        REXLOG_INFO("BF launcher: a controller was disconnected");
        SDL_CloseGamepad(g);
      }
      pads = std::move(now);
    }
    if (pads.empty()) return;
    static constexpr std::pair<SDL_GamepadButton, uint16_t> kButtons[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001},        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004},      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008},
        {SDL_GAMEPAD_BUTTON_START, 0x0010},          {SDL_GAMEPAD_BUTTON_BACK, 0x0020},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040},     {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100},  {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200},
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000},          {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
        {SDL_GAMEPAD_BUTTON_WEST, 0x4000},           {SDL_GAMEPAD_BUTTON_NORTH, 0x8000}};
    uint16_t buttons = 0;
    int16_t lx = 0, ly = 0;
    for (const auto& [id, pad] : pads) {
      for (const auto& [button, bit] : kButtons)
        if (SDL_GetGamepadButton(pad, button)) buttons |= bit;
      const int16_t x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
      const int16_t y = int16_t(std::clamp(-int(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY)), -32768, 32767));
      if (std::abs(x) > std::abs(lx)) lx = x;
      if (std::abs(y) > std::abs(ly)) ly = y;
    }
    host_last_ = buttons;
    Pad(buttons, lx, ly);
#endif
  }
  fs::path setup_dir_;
  std::function<void(fs::path)> setup_done_;
  std::shared_ptr<install::Progress> setup_progress_;  // shared with the install's thread (detached: quitting mid way)
  // An HD texture pack being imported (its copy's thread, detached): percent copied, -1 idle; the last result.
  struct HdImport {
    std::atomic<int> percent{-1};
    std::atomic<bool> turn_on{false};  // imported: HD textures on (by the menu's own thread)
    std::mutex mutex;
    std::string result;
  };
  std::shared_ptr<HdImport> hd_import_ = std::make_shared<HdImport>();
  std::string setup_error_;

  // What a row does. BF's own rows are pressed for the player (its selection moved there, then A); what happens next
  // is BF's, measured in runs origpause3 to pause1g and menu2a: a screen of its own (back with B, and A going one
  // screen deeper), the scoreboard (its B goes back to the match), a question (B back, A ends the pause), nothing on
  // PC (Help & Options and Achievements in the pause: Xbox screens), or leaving the match. Join match with friends
  // opens BF's Squad match and Create private match its Private match: their B asks "This will cancel your squad
  // (private) match" and only A (OK) goes back (runs menu3b, menu4c).
  enum class Does { kOurs, kScreen, kScreenThenGame, kScreenThenOk, kQuestion, kNothing, kLeave, kHelp };
  struct ListItem {
    const char* name;
    int bf_row;  // BF's own row (from its first), -1: ours
    Does does;
  };
  // The main menu (TekRantGaming's pick, 6 Nov 2026: BF's own list, ours added inside it, a little tighter so it keeps BF's
  // height): BF's eight rows in its order, with Server browser under Quick match and PC settings under Help &
  // options. Quick match is ours (the busiest server with room, as BF's would).
  static constexpr ListItem kMainItems[] = {
      {"QUICK MATCH", 0, Does::kOurs},          {"SERVER BROWSER", -1, Does::kOurs},
      {"JOIN MATCH WITH FRIENDS", 1, Does::kScreenThenOk},
      {"CUSTOM MATCH", -1, Does::kOurs},
      {"STATS & AWARDS", 3, Does::kScreen},
      {"PC SETTINGS", -1, Does::kOurs},         {"ACHIEVEMENTS", 5, Does::kNothing},
      {"LEADERBOARDS", 6, Does::kScreen},       {"HELP", -1, Does::kOurs},
      {"RETURN TO GAME LIBRARY", 7, Does::kScreen}};
  // A main menu row by its name (the pages that go back to it).
  static int MainRowOf(const char* name) {
    for (int i = 0; i < int(std::size(kMainItems)); ++i)
      if (std::string(kMainItems[i].name) == name) return i;
    return 0;
  }
  // The pause (TekRantGaming's pick: BF's own list as it is, PC settings added under its last row).
  static constexpr ListItem kPauseItems[] = {
      {"MY SQUAD", 0, Does::kScreen},       {"SCOREBOARD", 1, Does::kScreenThenGame},
      {"HELP & OPTIONS", 2, Does::kHelp},    {"LEADERBOARDS", 3, Does::kScreen},
      {"ACHIEVEMENTS", 4, Does::kNothing},  {"EXIT GAME", 5, Does::kLeave},
      {"SUICIDE", 6, Does::kQuestion},      {"PC SETTINGS", -1, Does::kOurs}};
  static constexpr int kPauseSettings = 7;

  // BF's row `it.bf_row`: its selection moved there from where it is (BF keeps it), then A. Returns the presses.
  std::vector<std::pair<double, uint16_t>> BfRowPresses(int& cursor, int row) {
    std::vector<std::pair<double, uint16_t>> presses;
    // 0.5 s apart: at 0.3 BF's pause list dropped one of five Downs and Exit Game landed on Achievements (run exit6).
    for (int r = cursor; r != row; r += row > r ? 1 : -1)
      presses.push_back({presses.empty() ? 0.15 : 0.5, row > r ? kPadDown : kPadUp});
    presses.push_back({presses.empty() ? 0.15 : 0.5, kPadA});
    cursor = row;
    return presses;
  }

  // Our main menu (TekRantGaming, 10 Nov 2026: ours in place of the game's, docs/MENUS-PLAN.md): the game's own menu is never
  // shown; the maps from the air (their pictures from the player's files, one after another) under the same column as
  // the pause. Quick Match, the server browser and Settings are ours (the join by the game's own startQuickMatch);
  // the game's other options still open the game's own screens for now (their presses, until our pages for them are
  // built: the plan's section 3).
  double main_map_at_ = -1;
  int main_map_ = 0;

  // fixed (0 to 3, the order of kMaps): that map only (the map being joined, or the one a private match plays), faded
  // in when it changes; -1: the four in turn.
  void MainBackdrop(ImDrawList* d, ImVec2 sz, int fixed = -1) {
    static constexpr const char* kMaps[] = {"wake", "guadalcanal", "iwo", "coral"};
    const double now = ImGui::GetTime();
    if (main_map_at_ < 0) main_map_at_ = now;
    if (fixed >= 0 && fixed < 4 && fixed != main_map_) main_map_ = fixed, main_map_at_ = now;
    else if (fixed < 0 && now - main_map_at_ > 16) main_map_ = (main_map_ + 1) % 4, main_map_at_ = now;
    const float t = float(now - main_map_at_);
    // A slow drift over the picture, a little larger than the window (with one map held, back and forth).
    const float td = fixed < 0 ? t : (std::fmod(t, 32.0f) < 16.0f ? std::fmod(t, 32.0f) : 32.0f - std::fmod(t, 32.0f));
    const float z = 1.08f + 0.004f * td, ox = -sz.x * (z - 1) * (0.2f + 0.02f * td), oy = -sz.y * (z - 1) * 0.5f;
    d->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(12, 14, 11, 255));
    kit::DrawImageFile(d, UserDir() / "cache" / "extracted" / "maps" / (std::string(kMaps[main_map_]) + ".png"),
                       ImVec2(ox, oy), ImVec2(ox + sz.x * z, oy + sz.y * z));
    d->AddRectFilledMultiColor(ImVec2(0, 0), sz, IM_COL32(6, 7, 5, 200), IM_COL32(6, 7, 5, 70),
                               IM_COL32(6, 7, 5, 110), IM_COL32(6, 7, 5, 230));
    const float fade = t < 0.8f ? 1.0f - t / 0.8f : 0.0f;  // each new map comes in from black
    if (fade > 0) d->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(6, 7, 5, int(255 * fade)));
  }

  // The flying notice (TekRantGaming, 11 Nov 2026): Home and the main menu told players to avoid flying a plane on
  // mouse and keyboard until a later update fixed it. That update is 0.4.0 (pull request 11's mouse flying, in on
  // his word, 13 Nov 2026), so the notice says what is new. A box `w` wide whose top right corner is `tr`; `u` is
  // the menus' unit (one pixel of a 1080 high window).
  static void FlyingNotice(ImDrawList* d, ImVec2 tr, float w, float u) {
    const kit::Fonts& f = kit::GetFonts();
    const std::string label = "PLAYTEST NOTICE";
    const std::string heading = "NEW: PLANES FLY WITH THE MOUSE";
    const std::string body = "The mouse moves the stick: left and right roll, back and forward pitch. A and D are the "
                             "rudder. Tell us how it feels.";
    const float pad = 24 * u, x = tr.x - w + 6 * u + pad, tw = w - 6 * u - 2 * pad;
    // The heading on one line: smaller when the box is narrow.
    const float k = std::min(1.0f, tw / std::max(1.0f, kit::SpacedWidth(f.ui_bold, 27 * u, heading, 2 * u)));
    const float hs = 27 * u * k, bs = 20 * u;
    const float h = pad + 15 * u + 14 * u + hs * 1.2f + 10 * u + kit::WrappedHeight(f.typed, bs, body, tw) + pad;
    const ImVec2 a(tr.x - w, tr.y), b(tr.x, tr.y + h);
    d->AddRectFilled(a, b, IM_COL32(15, 17, 13, 226));
    d->AddRectFilled(a, ImVec2(a.x + 6 * u, b.y), kit::kStamp);
    float y = a.y + pad;
    kit::Spaced(d, f.typed, 15 * u, ImVec2(x, y), kit::kStamp, label, 3 * u);
    y += 15 * u + 14 * u;
    kit::Spaced(d, f.ui_bold, hs, ImVec2(x, y), kit::kCream, heading, 2 * u * k);
    y += hs * 1.2f + 10 * u;
    kit::Wrapped(d, f.typed, bs, ImVec2(x, y), kit::kKhakiText, body, tw);
  }

  void MainPage(ImDrawList* d, float s, ImGuiIO& io, const Input& in_raw) {
    (void)s;
    const ImVec2 sz = io.DisplaySize;
    const kit::Fonts& f = kit::GetFonts();
    const float u = sz.y / 1080.0f;
    const bool at_menu = MenuDriverStage() == 2;
    Input in = in_raw;
    if (handing_over_ >= 0) {  // the game's presses for one of its rows still running
      in = {};
      if (BfPressesDone()) {
        std::lock_guard<std::mutex> lock(pad_mutex_);
        passing_ = handing_over_, passing_main_ = true, passing_depth_ = 0, passing_asked_ = false;
        handing_over_ = -1;
        open_ = false;
        return;
      }
    }
    MainBackdrop(d, sz);
    const int count = int(std::size(kMainItems));
    if (in.up) row_ = (row_ + count - 1) % count;
    if (in.down) row_ = (row_ + 1) % count;
    if (in.back && handing_over_ < 0 && !SkipLauncher()) {  // Home, the launcher (docs/LAUNCHER-PLAN.md, 2.3)
      page_ = Page::kHome;
      return;
    }
    const float cx0 = sz.x * 0.05f, cx1 = cx0 + 600 * u;
    d->AddRectFilled(ImVec2(0, 0), ImVec2(cx1, sz.y), IM_COL32(15, 17, 13, 226));
    d->AddRectFilled(ImVec2(cx1, 0), ImVec2(cx1 + 2 * u, sz.y), IM_COL32(201, 183, 122, 110));
    kit::Brand(d, ImVec2(cx0, 58 * u), u * 0.9f);
    const float hy = 150 * u;
    kit::Spaced(d, f.typed, 20 * u, ImVec2(cx0, hy), kit::kKhakiDim, "PACIFIC THEATRE, 1943", 4 * u);
    Text(d, f.stencil, 88 * u, ImVec2(cx0 - 4 * u, hy + 24 * u), kit::kCream, "MULTIPLAYER");
    d->AddRectFilled(ImVec2(cx0, hy + 126 * u), ImVec2(cx0 + 64 * u, hy + 132 * u), kit::kStamp);
    const float ry = hy + 168 * u, rh = 62 * u;
    int clicked = -1;
    for (int i = 0; i < count; ++i) {
      const ListItem& it = kMainItems[i];
      const ImVec2 a(cx0 - 28 * u, ry + rh * i), b(cx1, ry + rh * (i + 1));
      const bool sel = i == row_;
      const bool waits = it.bf_row >= 0 && it.does != Does::kOurs && !at_menu;  // the game's, once it is signed in
      const bool hover = ImGui::IsMouseHoveringRect(a, b);
      if (hover && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) row_ = i;
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = i;
      if (sel) {
        d->AddRectFilled(a, b, IM_COL32(201, 183, 122, 26));
        d->AddRectFilled(a, ImVec2(a.x + 8 * u, b.y), kit::kStamp);
      }
      d->AddRectFilled(ImVec2(cx0, b.y - 1 * u), ImVec2(cx1 - 40 * u, b.y), IM_COL32(201, 183, 122, 30));
      kit::Spaced(d, f.typed, 15 * u, ImVec2(cx0, a.y + 23 * u), sel ? kit::kStamp : kit::kKhakiDim,
                  fmt::format("{:02d}", i + 1), 2 * u);
      const std::string name = std::string(it.name) == "PC SETTINGS" ? "SETTINGS"
                               : std::string(it.name) == "RETURN TO GAME LIBRARY" ? "QUIT GAME" : it.name;
      kit::Spaced(d, f.ui_bold, 33 * u, ImVec2(cx0 + 52 * u, a.y + 13 * u),
                  waits ? kit::kKhakiDim : sel ? kit::kCream : kit::kKhakiText, name, 3 * u);
    }
    // The selected row's card, at the right, and the backend's numbers.
    static const std::map<std::string, const char*> kAbout = {
        {"QUICK MATCH", "Straight into the busiest server with room."},
        {"SERVER BROWSER", "Every official and community server: maps, players and ping."},
        {"JOIN MATCH WITH FRIENDS", "The game's own friends list and invitations."},
        {"CUSTOM MATCH", "Host a match on this PC for you and your friends: pick the map, then play. Friends "
                                 "join it by Direct connect."},
        {"STATS & AWARDS", "Your service record: rank, awards and best rounds."},
        {"HELP & OPTIONS", "The game's controls, its own settings and how to play."},
        {"PC SETTINGS", "Display, graphics, audio, controls (the game's own options with them) and gameplay."},
        {"ACHIEVEMENTS", "The game's achievements."},
        {"LEADERBOARDS", "Where you stand among every player."},
        {"HELP", "The game's tutorial on Wake Island, its controls on foot, in vehicles and in planes, and the credits."},
        {"RETURN TO GAME LIBRARY", "Close Battlefield 1943."}};
    {
      const ListItem& it = kMainItems[row_];
      const float w = 620 * u;
      const ImVec2 ca(sz.x * 0.95f - w, sz.y * 0.62f), cb(sz.x * 0.95f, sz.y * 0.62f + 170 * u);
      d->AddRectFilled(ca, cb, IM_COL32(15, 17, 13, 210));
      d->AddRectFilled(ca, ImVec2(ca.x + 6 * u, cb.y), kit::kStamp);
      const std::string name = std::string(it.name) == "PC SETTINGS" ? "SETTINGS"
                               : std::string(it.name) == "RETURN TO GAME LIBRARY" ? "QUIT GAME" : it.name;
      kit::Spaced(d, f.ui_bold, 30 * u, ImVec2(ca.x + 30 * u, ca.y + 22 * u), kit::kCream, name, 3 * u);
      const auto about = kAbout.find(it.name);
      kit::Wrapped(d, f.typed, 20 * u, ImVec2(ca.x + 30 * u, ca.y + 74 * u), kit::kKhakiText,
                   about == kAbout.end() ? "" : about->second, w - 60 * u);
    }
    FlyingNotice(d, ImVec2(sz.x * 0.95f, 200 * u), 620 * u, u);  // under the stamp, above the card
    const auto servers = list_.servers();
    int players = 0;
    for (const auto& sv : servers) players += sv.players;
    const std::string online = !list_.asked()       ? "CONNECTING"
                               : !list_.reachable() ? "SERVERS NOT REACHABLE"
                               : fmt::format("{} {}  /  {} {}", servers.size(), servers.size() == 1 ? "SERVER" : "SERVERS",
                                             players, players == 1 ? "PLAYER" : "PLAYERS");
    const float ow = kit::SpacedWidth(f.typed, 18 * u, online, 3 * u);
    d->AddCircleFilled(ImVec2(sz.x * 0.95f - ow - 22 * u, sz.y - 64 * u), 6 * u,
                       list_.reachable() ? kit::kGreenBright : kit::kStamp);
    kit::Spaced(d, f.typed, 18 * u, ImVec2(sz.x * 0.95f - ow, sz.y - 74 * u), kit::kKhakiText, online, 3 * u);
    // The prompts.
    const float py = sz.y - 64 * u;
    float x = cx0;
    x += kit::Glyph(d, PromptStyle(), kit::Button::kA, "Enter", ImVec2(x, py), 34 * u) + 10 * u;
    x += kit::Spaced(d, f.ui_bold, 24 * u, ImVec2(x, py - 14 * u), kit::kKhakiText, "SELECT", 3 * u) + 34 * u;
    if (!SkipLauncher()) {
      x += kit::Glyph(d, PromptStyle(), kit::Button::kB, "Esc", ImVec2(x, py), 34 * u) + 10 * u;
      kit::Spaced(d, f.ui_bold, 24 * u, ImVec2(x, py - 14 * u), kit::kKhakiText, "HOME", 3 * u);
    }
    if (in.accept) clicked = row_;
    if (clicked < 0) return;
    row_ = clicked;
    const ListItem& it = kMainItems[clicked];
    const std::string name = it.name;
    REXLOG_INFO("BF main menu: {}", name);
    if (name == "QUICK MATCH") return QuickPlay();
    if (name == "SERVER BROWSER") return OpenBrowser();
    if (name == "PC SETTINGS") {
      page_ = Page::kSettings, row_ = 0, from_pause_ = false, from_home_ = false;
      return;
    }
    if (name == "STATS & AWARDS") {  // ours (StatsPage), in place of the game's screen
      LoadTestProfile();
      page_ = Page::kStats;
      return;
    }
    if (name == "HELP") {  // ours (HelpPage): the rest of the game's Help & Options
      page_ = Page::kHelp, help_row_ = 0;
      return;
    }
    if (name == "CUSTOM MATCH") {  // ours (PrivateMatchPage): our own server on this PC, then joined
      page_ = Page::kPrivateMatch, pm_row_ = 0, pm_error_.clear();
      LoadPrivateMatchChoices();
      return;
    }
    if (name == "JOIN MATCH WITH FRIENDS") {  // ours (FriendsPage): the players online and their servers
      page_ = Page::kFriends, fr_row_ = 0, fr_asked_at_ = -100;
      return;
    }
    if (name == "LEADERBOARDS") {  // ours (LeaderboardsPage): every player's totals from our backend
      page_ = Page::kLeaderboards, lb_from_pause_ = false, lb_asked_at_ = -100;
      return;
    }
    if (name == "ACHIEVEMENTS") {  // ours (AchievementsPage): the game's list and this player's unlocks
      LoadTestProfile();
      page_ = Page::kAchievements, ach_scroll_ = 0, ach_from_pause_ = false;
      return;
    }
    if (name == "RETURN TO GAME LIBRARY") {  // quits as the window's close button does
      if (quit_) quit_();
      return;
    }
    if (!at_menu) return;  // the game's own rows once it is signed in
    PressBfTimed(BfRowPresses(main_cursor_, it.bf_row));
    if (it.does != Does::kNothing) handing_over_ = clicked;
  }

  void ImportHdPack() {
    if (hd_import_->percent.load() >= 0) return;
    fs::path from;
#if defined(BF_DEV_TOOLS)
    if (const char* v = std::getenv("BF_DEV_HD_IMPORT"); v && *v) from = v;  // tests: no picker
#endif
    if (from.empty()) from = Pick(BrowseForHdPack);
    if (from.empty()) return;
    const fs::path to = HdPackPath(UserDir());
    std::shared_ptr<HdImport> state = hd_import_;
    {
      std::lock_guard lock(state->mutex);
      state->result.clear();
    }
    state->percent.store(0);
    std::thread([this, state, from, to] {
      std::string result = "Imported";
      std::error_code ec;
      std::ifstream in(from, std::ios::binary);
      char magic[4] = {};
      if (!in || !in.read(magic, 4) || std::memcmp(magic, "BFHD", 4) != 0) {
        result = "Not an HD texture pack";
      } else {
        fs::create_directories(to.parent_path(), ec);
        const fs::path tmp = to.string() + ".part";
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        const uint64_t total = std::max<uint64_t>(1, fs::file_size(from, ec));
        in.seekg(0);
        std::vector<char> buffer(8 << 20);
        uint64_t done = 0;
        while (in && out) {
          in.read(buffer.data(), std::streamsize(buffer.size()));
          const std::streamsize n = in.gcount();
          if (n <= 0) break;
          out.write(buffer.data(), n);
          done += uint64_t(n);
          state->percent.store(int(std::min<uint64_t>(99, done * 100 / total)));
        }
        out.close();
        if (done != total || !out) {
          result = "Could not copy it";
          fs::remove(tmp, ec);
        } else {
          fs::rename(tmp, to, ec);
          if (ec) result = "Could not copy it";
        }
      }
      if (result == "Imported") {
        REXLOG_INFO("BF settings: HD texture pack imported from {} ({} MB)", from.string(),
                    fs::file_size(to, ec) >> 20);
        state->turn_on.store(true);
      } else {
        REXLOG_WARN("BF settings: HD texture pack from {}: {}", from.string(), result);
      }
      {
        std::lock_guard lock(state->mutex);
        state->result = result == "Imported" ? "Imported: on at the next start" : result;
      }
      state->percent.store(-1);
    }).detach();
  }

  // The busiest server with room; false when there is none. (Until v0.4.0 the busiest that played the player's
  // movement. TekRantGaming, 13 Nov 2026: "we are dropping support for modern and going full in on classic being the
  // default": every server plays the game's own movement and aiming now, and there is nothing to choose.)
  double quick_waiting_ = 0;      // Quick Play waits for the list's first answer until this time (ImGui's)
  uint64_t quick_asked_at_ = 0;   // the list's answers when it began to wait
  bool QuickJoin() {
    const auto servers = list_.servers();
    const ServerInfo* best = nullptr;
    // Never an experimental tick rate (TekRantGaming, 0.4.1, 13 Nov 2026: "quick play will never put you into one of
    // these experimental 60hz" servers): 30 Hz only.
    for (const auto& sv : servers)
      if (sv.Tick() == 30 && sv.players < sv.max_players && (!best || sv.players > best->players)) best = &sv;
    if (best) Join(*best);
    return best != nullptr;
  }

  // An experimental server (any tick rate but the game's own 30, 0.4.1): before it is joined from the server browser
  // (its list, Favourites, Direct connect) the player is warned in TekRantGaming's words (13 Nov 2026), and can only
  // connect anyway after 5 s. Not for Quick Play (it never picks one), the host's own Custom Match (chosen with its
  // warning on that page) or the join that follows a restart at the server's rate (the player was warned before it).
  bool exp_popup_ = false;
  ServerInfo exp_popup_server_;
  double exp_popup_at_ = 0;           // ImGui's time when it opened
  std::string exp_accepted_address_;  // the server the player chose to connect to anyway
  bool ExperimentalWarned(const ServerInfo& sv) {
    if (sv.Tick() == 30 || exp_accepted_address_ == sv.address) return true;
#if defined(BF_DEV_TOOLS)
    if (std::getenv("BF_AUTO_JOIN")) return true;  // test joins: nobody to read it
#endif
    exp_popup_ = true, exp_popup_server_ = sv, exp_popup_at_ = ImGui::GetTime(), popup_row_ = 0;
    REXLOG_INFO("BF join: {} runs at an experimental {} Hz: warned before joining", sv.name, sv.Tick());
    return false;
  }
  void ExperimentalPopup(ImDrawList* d, ImGuiIO& io, Input in) {
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    d->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(10, 12, 8, 190));
    const ImVec2 a = st.P(300, 150), b = st.P(980, 500);
    static constexpr const char* kParagraphs[] = {
        "You are connecting to an experimental server currently testing a 60Hz server tick rate.",
        "This server is known to have potential game breaking issues. Increasing the server tick rate introduces "
        "changes to the physics engine, which is currently being decoupled from its original timing. This may cause "
        "unexpected behaviour, physics inconsistencies, broken mechanics or other gameplay issues.",
        "This server is available for testing and feedback purposes only. It is not representative of the stable "
        "gameplay experience and is not recommended for normal gameplay."};
    {
      kit::Turn turn(d, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), -0.3f);
      kit::Paper(d, a, b, st.u);
      kit::Tape(d, ImVec2((a.x + b.x) * 0.5f, a.y), st(110), st(26), -3);
      kit::Heading(d, st(36), ImVec2(a.x + st(30), a.y + st(26)), kit::kInk, "EXPERIMENTAL SERVER", st(1.5f), st(620));
      kit::Spaced(d, f.typed, st(11), ImVec2(a.x + st(32), a.y + st(72)), kit::kStamp,
                  fmt::format("{} \xC2\xB7 {} HZ", exp_popup_server_.name, exp_popup_server_.Tick()), st(1.5f));
      float y = a.y + st(98);
      for (const char* para : kParagraphs) {
        kit::Wrapped(d, f.ui, st(16), ImVec2(a.x + st(30), y), kit::kInkSoft, para, st(620));
        y += kit::WrappedHeight(f.ui, st(16), para, st(620)) + st(12);
      }
    }
    // Close, and Connect anyway: locked for its first 5 s (it counts down), then like any button.
    const double left = 5.0 - (ImGui::GetTime() - exp_popup_at_);
    const bool locked = left > 0;
    if (locked) popup_row_ = 0;
    else if (in.left || in.right || in.up || in.down) popup_row_ = 1 - popup_row_;
    int clicked = -1;
    float x = a.x + st(30);
    for (int i = 0; i < 2; ++i) {
      const std::string label = i == 0 ? std::string("Close")
                                       : locked ? fmt::format("Connect anyway ({})", int(std::ceil(left)))
                                                : std::string("Connect anyway");
      const ImVec2 c0(x, b.y - st(70)), c1(x + st(250), b.y - st(26));
      const bool off = i == 1 && locked;
      const bool hover = ImGui::IsMouseHoveringRect(c0, c1);
      if (hover && !off && io.MouseDelta.x + io.MouseDelta.y != 0) popup_row_ = i;
      const bool sel = popup_row_ == i && !off;
      if (sel) d->AddRectFilled(ImVec2(c0.x, c0.y + st(4)), ImVec2(c1.x, c1.y + st(4)), kit::kGreenShadow);
      d->AddRectFilled(c0, c1, sel ? kit::kGreen : off ? kit::kPaperDim : kit::kPaperCard);
      if (!sel) d->AddRect(c0, c1, kit::kBorder, 0, 0, st(2));
      const float tw = kit::TextWidth(f.ui_bold, st(20), label);
      d->AddText(f.ui_bold, st(20), ImVec2((c0.x + c1.x - tw) * 0.5f, c0.y + st(8)),
                 sel ? IM_COL32(255, 255, 255, 255) : off ? kit::kInkFaint : kit::kInk, label.c_str());
      if (hover && !off && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = i;
      x += st(270);
    }
    if (in.accept) clicked = popup_row_;
    if (in.back) clicked = 0;
    if (clicked < 0) return;
    exp_popup_ = false;
    if (clicked == 0) {
      REXLOG_INFO("BF join: the experimental server {} not joined (closed)", exp_popup_server_.name);
      return;
    }
    REXLOG_INFO("BF join: {} joined anyway ({} Hz, experimental)", exp_popup_server_.name, exp_popup_server_.Tick());
    exp_accepted_address_ = exp_popup_server_.address;
    Join(exp_popup_server_);
  }

  // A server of the other tick rate while the game already runs (its rate is fixed at its start): asked, then the
  // game starts again at that rate and joins (TickRejoinTick below finds it on Home).
  bool tick_popup_ = false;
  ServerInfo tick_popup_server_;
  std::string tick_join_address_;  // after such a restart: the server to join, by its address
  double tick_join_until_ = 0;
  void Join(const ServerInfo& sv) {
    if (!ExperimentalWarned(sv)) return;
    if (sv.Tick() != TickRate() && !SetTickRate(sv.Tick())) {
      tick_popup_ = true, tick_popup_server_ = sv, popup_row_ = 1;
      REXLOG_INFO("BF join: {} runs at {} Hz and this run at {}: the game must start again to join it (asked)", sv.name,
                  sv.Tick(), TickRate());
      return;
    }
    // The version gate (server_list.h): a game older than the version players need is not let in, and says so with
    // the update pop-up. Asked of the list's last answer first (no round trip), then of the reservation itself.
    std::string needs = list_.required_version();
#if defined(BF_DEV_TOOLS)
    // BF_DEV_NO_GATE_PRECHECK=1 (tests): the list's answer is not asked, so the reservation's refusal is what shows.
    if (const char* v = std::getenv("BF_DEV_NO_GATE_PRECHECK"); v && *v == '1') needs.clear();
#endif
    if (!needs.empty() && update::IsNewerVersion(needs, BfVersion())) return AskToUpdate(needs, sv.name);
    switch (list_.Reserve(sv.gid, &needs)) {
      case ServerList::Reserved::kYes: break;
      case ServerList::Reserved::kOutdated: return AskToUpdate(needs, sv.name);
      case ServerList::Reserved::kNo:
        REXLOG_WARN("BF join: {} could not be reserved (no answer from the server list, or the server is gone)", sv.name);
        menu_error_ = "Could not join that server: the server list did not answer. Try again in a moment.";
        menu_error_at_ = ImGui::GetTime();
        BrowserNote("COULD NOT JOIN: THE SERVER LIST DID NOT ANSWER");
        return;
    }
    reserved_at_ = ImGui::GetTime();
    SetJoinedServerNight(sv.night);
    joining_ = sv.name;
    joining_map_ = sv.map;
    joined_gid_ = sv.gid;  // the pause's Scoreboard and the match score sent to the backend
    comms::NoteJoined(sv.address);  // voice and text chat go to this server's host (comms.cpp)
    joined_from_start_ = MenuDriverStage() < 2;  // its joining page shows the game's start first
    tutorial_ = false;
    NotePlayedLevel(sv.map.empty() ? "Wake Island" : sv.map);
    failed_ = false;
    page_ = Page::kJoining;
    main_cursor_ = 0;  // the join reloads BF's menu (StartAutoJoin)
    if (MenuDriverStage() == 2) StartAutoJoin(sv.name);
    else pending_ = sv.name;  // still starting: OnDraw joins once BF's online menu is up
    // Picked on Home before the game runs (TekRantGaming, 10 Nov 2026, as King Kong's chapter select): the game starts now,
    // its films, title and sign in hidden behind this Joining page, and the join follows the sign in.
    if (pre_game_ && start_game_) {
      REXLOG_INFO("BF launcher: a server picked on Home: the game starts, the join follows its sign in");
      StartGameFromLauncher();
    }
  }
  // The game started from Home's Joining page (a server picked, or Help's tutorial): its films, title and sign in
  // hidden behind that page. King Kong's rule: shaders not prepared for this version (just updated, or never) get the
  // shader page first, then the Joining page again (AfterShaders); prepared ones are made behind it.
  void StartGameFromLauncher() {
    pre_game_ = false, played_ = true, started_from_launcher_ = true;
    NoteStartValues();  // the renderer takes its settings now
    // Never the shader page at a join (TekRantGaming, 13 Nov 2026: "when I joined a server it prepared shaders this is
    // only ever to run at first start up or after an update to the game"). The shaders are prepared at the start
    // (PrepareShadersAtStart), so here the renderer makes its stored pipelines behind the Joining page, from the
    // driver's cache. Not prepared after all (a cache deleted while the launcher was open): they are made behind the
    // page all the same, and the next start prepares them on its shader page.
    if (!ShadersPreparedForPlay())
      REXLOG_WARN("BF shaders: not prepared for v{} at a join: made behind the Joining page, prepared at the next start", BfVersion());
    shaders_prepared_hint_ = true;
    auto start = std::move(start_game_);
    start();
  }
  // The shaders prepared at the start, and only there (TekRantGaming, 13 Nov 2026, as above: "remember the same way
  // kong handles its shaders with them pre compiled we can use them whenever we don't need to recompile any other
  // time"). The renderer makes its pipelines only once the game runs, so when they are not prepared for this version
  // (a first start: the setup's step 8, with the settings just chosen; the first start after an update, or a setup
  // closed before that step: before the main menu) the game starts now, behind the shader page, which waits for every
  // stored pipeline and then writes the marker (shaders_prepared_for). The game goes on starting behind our pages,
  // silent (SetGameAudioMuted), and is at its menu when the player picks a server. Every other start shows no shader
  // page: the game starts at the first join, its pipelines made from the driver's cache behind the Joining page.
  // False when there is nothing to prepare (or the game runs already).
  bool PrepareShadersAtStart(const char* why) {
    if (!pre_game_ || !start_game_ || ShadersPreparedForPlay()) return false;
    REXLOG_INFO("BF shaders: not prepared for v{} (prepared for \"{}\"): prepared now, {}; the game starts behind the "
                "shader page",
                BfVersion(), settings_.Get("shaders_prepared_for", ""), why);
    pre_game_ = false, started_from_launcher_ = true;
    NoteStartValues();  // the renderer takes its settings now
    shaders_prepared_hint_ = false;
    shaders_wait_from_ = ImGui::GetTime();
    page_ = Page::kShaders, shaders_shown_ = false;
    auto start = std::move(start_game_);
    start();
    return true;
  }

  // The server browser, version 3 of TekRantGaming's 8 Nov mockups (SERVERS-PLAN.md section 5): over the game's own menu
  // scene, PLAY ONLINE, three tabs (Official, Community, Direct connect), one table (name, map, movement, players,
  // ping) whose selected row opens with the map's picture, the description, the next map and Join. No address shown.
  static constexpr ImU32 kTableInk = IM_COL32(30, 32, 27, 255), kTableFaint = IM_COL32(106, 106, 94, 255);
  static constexpr ImU32 kTablePaper = IM_COL32(233, 230, 220, 255), kTableGreen = IM_COL32(111, 143, 58, 255);
  static constexpr ImU32 kTableLine = IM_COL32(201, 196, 178, 255), kCreamText = IM_COL32(233, 230, 220, 255);
  int browser_tab_ = 0;  // 0 official, 1 community, 2 direct connect
  bool sort_by_ping_ = false;

  // Night Ops (TekRantGaming, 11 Nov 2026): a night server's maps drawn at night (NativeXRenderer D162), only while
  // playing there: on at the join, off at another server's, at the match's end and in the tutorial.
  static void SetJoinedServerNight(bool night) {
    if (!kNightOps) night = false;  // night_ops.h: not in this build
    const std::string was = rex::cvar::GetFlagByName("native_x_renderer_night");
    if (was == (night ? "true" : "false")) return;
    const bool set = rex::cvar::SetFlagByName("native_x_renderer_night", night ? "true" : "false");
    REXLOG_INFO("BF night: {} (the renderer's setting was \"{}\", set: {}, now \"{}\")",
                night ? "on, a Night Ops server" : "off", was, set, rex::cvar::GetFlagByName("native_x_renderer_night"));
  }
  // The prompts in a row along the bottom (mockup 3), BF's discs or the chosen style's pictures.
  void BrowserHints(ImDrawList* d, float s, float x, float y, const std::vector<std::pair<std::string, std::string>>& keys) {
    const float bh = 34 * s, size = 25 * s;
    for (const auto& [k, t] : keys) {
      const float kw = BfButton(d, ImVec2(x, y), bh, k);
      Text(d, g_fonts.bf, size, ImVec2(x + kw + 10 * s, CapTop(y - bh * 0.5f, bh, size)), kCreamText, Upper(t));
      x += kw + 10 * s + Width(g_fonts.bf, size, Upper(t)) + 34 * s;
    }
  }

  // A map's picture: the one extracted from the player's files (SERVERS-PLAN.md section 2) once that is built; until
  // then a hatched plate with the map's name.
  void MapPicture(ImDrawList* d, float s, ImVec2 a, ImVec2 b, const std::string& map) {
    // The map's loading screen picture, made from the player's game files at the start (shader_prep.cpp
    // ExtractGameArt) into the user's cache.
    std::string key = map.empty() ? "Wake Island" : map;
    for (char& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
    const char* file = key.find("iwo") != std::string::npos ? "iwo" : key.find("guadal") != std::string::npos ? "guadalcanal"
                       : key.find("coral") != std::string::npos ? "coral" : "wake";
    if (kit::DrawImageFile(d, UserDir() / "cache" / "extracted" / "maps" / (std::string(file) + ".png"), a, b)) {
      d->AddRect(a, b, IM_COL32(30, 32, 27, 255), 0, 0, 2 * s);
      return;
    }
    d->AddRectFilled(a, b, IM_COL32(157, 167, 160, 255));
    d->PushClipRect(a, b, true);
    for (float x = a.x - (b.y - a.y); x < b.x; x += 20 * s)
      d->AddLine(ImVec2(x, b.y), ImVec2(x + (b.y - a.y), a.y), IM_COL32(179, 188, 180, 255), 10 * s);
    d->PopClipRect();
    const std::string t = Upper(map.empty() ? "Wake Island" : map);
    const float size = 26 * s, tw = Width(g_fonts.bf, size, t);
    const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    d->AddRectFilled(ImVec2(c.x - tw * 0.5f - 12 * s, c.y - 22 * s), ImVec2(c.x + tw * 0.5f + 12 * s, c.y + 22 * s),
                     IM_COL32(30, 32, 27, 200));
    Text(d, g_fonts.bf, size, ImVec2(c.x - tw * 0.5f, CapTop(c.y - 22 * s, 44 * s, size)), kCreamText, t);
  }

  // ---- Direct connect: an address typed (keyboard, or the on screen keys with a controller), found through the main
  // backend (registered servers, listed or not), then the normal join.
  std::string direct_text_, direct_error_;
  bool direct_keys_ = false, direct_letters_ = false;
  int direct_key_ = 0;
  std::shared_ptr<std::atomic<int>> direct_job_;  // 0 looking, 1 found, 2 not found
  std::shared_ptr<ServerInfo> direct_found_;
  void DirectConnect() {
    std::string text = direct_text_;
    int port = 18300;
    if (const size_t colon = text.rfind(':'); colon != std::string::npos) {
      port = std::atoi(text.c_str() + colon + 1);
      text = text.substr(0, colon);
    }
    if (text.empty() || port <= 0 || port > 65535) {
      direct_error_ = "Type an address, like 203.0.113.24:18300, or a name.";
      return;
    }
    direct_error_.clear();
    auto job = std::make_shared<std::atomic<int>>(0);
    auto found = std::make_shared<ServerInfo>();
    direct_job_ = job, direct_found_ = found;
    REXLOG_INFO("BF browser: Direct connect to {}:{}", text, port);
    direct_host_ = text;
    const bool port_typed = direct_text_.find(':') != std::string::npos;
    std::thread([this, job, found, text, port, port_typed] {
      if (list_.Find(text, port, *found)) return job->store(1);
      // Not known to the backend this game is on: the address may be a server on its own backend (the server
      // package's LAN only mode, TekRantGaming, 13 Nov 2026: "lan only so that they don't need to connect to the
      // backend ... get external users to connect via direct ip"). That backend is asked: for the server on the
      // port typed (listed there or not); with no port typed, for the busiest it lists with room, else the one on
      // the usual port.
      const std::string own_registry = text + ":18250";
      if (port_typed) return job->store(list_.Find(text, port, *found, own_registry) ? 3 : 2);
      std::vector<ServerInfo> own;
      if (ServerList::ListAt(text, own) && !own.empty()) {
        const ServerInfo* pick = nullptr;
        for (const ServerInfo& sv : own) {
          if (sv.players < sv.max_players && (!pick || sv.players > pick->players)) pick = &sv;
        }
        if (!pick) pick = &own.front();
        *found = *pick;
        return job->store(3);
      }
      job->store(list_.Find(text, port, *found, own_registry) ? 3 : 2);
    }).detach();
  }
  // A server on its own backend (Direct connect found it there): this game goes over to that backend for the join.
  // Before the game runs that is two settings (the game signs in there when it starts); once it runs it is signed in
  // to the main backend, so the program starts again on the other one (asked first) and joins from its Home.
  std::string direct_host_;          // the address typed, without its port
  std::string own_backend_;          // this run is on a server's own backend (its address), not the main one
  std::string own_join_port_;        // after the restart: the server to join there, by its port
  double own_join_until_ = 0;
  bool own_popup_ = false;
  ServerInfo own_popup_server_;
  void UseOwnBackend(const std::string& host) {
    own_backend_ = host;
    rex::cvar::SetFlagByName("bf_online_host", host);
    rex::cvar::SetFlagByName("bf_backend_registry", host + ":18250");
#if defined(_WIN32)
    _putenv_s("BF_ONLINE_HOST", host.c_str());  // online_services.cpp reads it when the game first asks
#else
    setenv("BF_ONLINE_HOST", host.c_str(), 1);
#endif
    list_.RefreshNow();
    REXLOG_INFO("BF browser: this run is on the backend at {} (a server's own, not the main one)", host);
  }
  void JoinOnOwnBackend(const std::string& host, const ServerInfo& sv) {
    if (pre_game_) {
      UseOwnBackend(host);
      // Its reservation is asked of that backend (the list's own thread may not have turned yet).
      Join(sv);
      return;
    }
    own_popup_ = true, own_popup_server_ = sv, popup_row_ = 1;
    REXLOG_INFO("BF browser: {} is on its own backend at {}: the game must start again to join it (asked)", sv.name, host);
  }
  void TickPopup(ImDrawList* d, ImGuiIO& io, Input in) {
    const Stage st = StageOf(io);
    bool yes = false;
    if (!Question(d, io, st, "RESTART TO JOIN",
                  fmt::format("This server runs the game at {} Hz and your game is running at {} Hz. The game starts again "
                              "at the server's rate and joins it.",
                              tick_popup_server_.Tick(), TickRate()),
                  "Cancel", "Restart and join", in, yes))
      return;
    tick_popup_ = false;
    if (!yes) return;
    const std::string tick = std::to_string(tick_popup_server_.Tick());
#if defined(_WIN32)
    _putenv_s("BF_JOIN_TICK", tick.c_str()), _putenv_s("BF_JOIN_ADDRESS", tick_popup_server_.address.c_str());
    if (!own_backend_.empty()) _putenv_s("BF_OWN_BACKEND", own_backend_.c_str()), _putenv_s("BF_ONLINE_HOST", own_backend_.c_str());
#else
    setenv("BF_JOIN_TICK", tick.c_str(), 1), setenv("BF_JOIN_ADDRESS", tick_popup_server_.address.c_str(), 1);
    if (!own_backend_.empty()) setenv("BF_OWN_BACKEND", own_backend_.c_str(), 1), setenv("BF_ONLINE_HOST", own_backend_.c_str(), 1);
#endif
    REXLOG_INFO("BF join: starting again at {} Hz to join {}", tick, tick_popup_server_.name);
    settings_.Save();
    if (update::RestartSelf() && quit_) quit_();
  }
  // Each frame on Home after such a restart: the join, once the list has the server.
  void TickRejoinTick() {
    if (tick_join_address_.empty() || !pre_game_) return;
    if (tick_join_until_ == 0) tick_join_until_ = ImGui::GetTime() + 25;
    for (const ServerInfo& sv : list_.servers()) {
      if (sv.address != tick_join_address_) continue;
      REXLOG_INFO("BF join: joining {} as asked before the restart ({} Hz)", sv.name, sv.Tick());
      tick_join_address_.clear();
      exp_accepted_address_ = sv.address;  // warned before the restart
      Join(sv);
      return;
    }
    if (ImGui::GetTime() > tick_join_until_) {
      tick_join_address_.clear();
      menu_error_ = "That server is not on the list any more. Open the server browser to see what is.";
      menu_error_at_ = ImGui::GetTime();
    }
  }
  void OwnBackendPopup(ImDrawList* d, ImGuiIO& io, Input in) {
    const Stage st = StageOf(io);
    bool yes = false;
    if (!Question(d, io, st, "RESTART TO JOIN",
                  "This server is not on the main server list: it runs on its own backend, as LAN servers do. The game starts again to join "
                  "it. The start after that is back on the main servers.",
                  "Cancel", "Restart and join", in, yes))
      return;
    own_popup_ = false;
    if (!yes) return;
    const std::string& a = own_popup_server_.address;
    const std::string port = a.find(':') == std::string::npos ? std::string() : a.substr(a.rfind(':') + 1);
#if defined(_WIN32)
    _putenv_s("BF_OWN_BACKEND", direct_host_.c_str()), _putenv_s("BF_OWN_BACKEND_JOIN", port.c_str());
    _putenv_s("BF_ONLINE_HOST", direct_host_.c_str());
    _putenv_s("BF_JOIN_TICK", std::to_string(own_popup_server_.Tick()).c_str());  // at that server's rate
#else
    setenv("BF_OWN_BACKEND", direct_host_.c_str(), 1), setenv("BF_OWN_BACKEND_JOIN", port.c_str(), 1);
    setenv("BF_ONLINE_HOST", direct_host_.c_str(), 1);
    setenv("BF_JOIN_TICK", std::to_string(own_popup_server_.Tick()).c_str(), 1);
#endif
    REXLOG_INFO("BF browser: starting again on the backend at {} to join the server on port {}", direct_host_, port);
    settings_.Save();
    if (update::RestartSelf() && quit_) quit_();
  }
  // Each frame on Home after such a restart: the join, once that backend's list has the server.
  void OwnBackendTick() {
    if (own_join_port_.empty() || !pre_game_) return;
    if (own_join_until_ == 0) own_join_until_ = ImGui::GetTime() + 25;
    const std::string tail = ":" + own_join_port_;
    for (const ServerInfo& sv : list_.servers()) {
      if (sv.address.size() > tail.size() && sv.address.compare(sv.address.size() - tail.size(), tail.size(), tail) == 0) {
        REXLOG_INFO("BF browser: on the backend at {}: joining {} as asked before the restart", own_backend_, sv.name);
        own_join_port_.clear();
        Join(sv);
        return;
      }
    }
    if (ImGui::GetTime() > own_join_until_) {
      own_join_port_.clear();
      menu_error_ = "That server is not on its backend's list any more. Open the server browser to see what is.";
      menu_error_at_ = ImGui::GetTime();
    }
  }
  void DirectConnectPage(ImDrawList* d, float s, ImGuiIO& io, const Input& in, float x0, float x1, float top) {
    // The keyboard types straight in (as on the launcher's name page, the window is asked for text input).
    {
      ImGuiContext& g = *ImGui::GetCurrentContext();
      g.PlatformImeData.WantTextInput = true;
      g.PlatformImeData.ViewportId = ImGui::GetMainViewport()->ID;
    }
    auto allowed = [](unsigned c) { return std::isalnum(int(c)) || c == '.' || c == ':' || c == '-'; };
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
      const ImWchar c = io.InputQueueCharacters[i];
      if (c < 128 && allowed(c) && direct_text_.size() < 64) direct_text_ += char(c), direct_keys_ = false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !direct_text_.empty()) direct_text_.pop_back();
    if (ImGui::IsKeyPressed(ImGuiKey_V) && io.KeyCtrl)
      if (const char* clip = ImGui::GetClipboardText())
        for (const char* c = clip; *c && direct_text_.size() < 64; ++c)
          if (allowed(static_cast<unsigned char>(*c))) direct_text_ += *c;
    // The finished lookup.
    if (direct_job_ && direct_job_->load() != 0) {
      if (direct_job_->load() == 1) {
        const ServerInfo sv = *direct_found_;
        std::string recent = sv.name + "|" + direct_text_;
        for (const std::string& r : RecentServers())
          if (r.substr(r.find('|') + 1) != direct_text_) recent += ";" + r;
        settings_.values["recent_servers"] = recent.substr(0, 400);
        settings_.Save();
        direct_job_.reset();
        Join(sv);
        return;
      }
      direct_error_ = "No server answers at that address through the main backend. Check the address and its port.";
      direct_job_.reset();
    }
    const float w = x1 - x0, pw = w * 0.52f;
    const ImVec2 a(x0, top), b(x0 + pw, top + 520 * s);
    d->AddRectFilled(a, b, kTablePaper);
    float y = top + 26 * s;
    const float px = x0 + 30 * s;
    Text(d, g_fonts.bf_med, 21 * s, ImVec2(px, y), kTableFaint, "JOIN A SERVER BY ITS ADDRESS");
    y += 34 * s;
    Text(d, g_fonts.bf, 48 * s, ImVec2(px, y), kTableInk, "DIRECT CONNECT");
    y += 66 * s;
    d->AddText(g_fonts.bf_med, 24 * s, ImVec2(px, y), IM_COL32(60, 59, 50, 255),
               "For a server that is not listed, such as a friend's private one. Ask its host for the address.", nullptr,
               pw - 60 * s);
    y += 74 * s;
    const ImVec2 f0(px, y), f1(x0 + pw - 30 * s, y + 64 * s);
    d->AddRectFilled(f0, f1, IM_COL32(244, 239, 223, 255));
    d->AddRect(f0, f1, kTableGreen, 0, 0, 4 * s);
    const std::string shown = direct_text_.empty() && !direct_keys_ ? "" : direct_text_;
    Text(d, g_fonts.bf_med, 34 * s, ImVec2(f0.x + 16 * s, CapTop(f0.y, 64 * s, 34 * s)), kTableInk, shown);
    if (std::fmod(ImGui::GetTime(), 1.0) < 0.6) {
      const float cx = f0.x + 18 * s + Width(g_fonts.bf_med, 34 * s, shown);
      d->AddRectFilled(ImVec2(cx, f0.y + 14 * s), ImVec2(cx + 3 * s, f1.y - 14 * s), kTableInk);
    }
    y += 76 * s;
    Text(d, g_fonts.bf_med, 20 * s, ImVec2(px, y), direct_error_.empty() ? kTableFaint : kBfRed,
         direct_job_ ? "LOOKING FOR THE SERVER..." : direct_error_.empty() ? "AN ADDRESS AND PORT, OR A NAME. THE PORT IS 18300 IF YOU LEAVE IT OUT."
                                                                          : Upper(direct_error_));
    y += 44 * s;
    // Recent servers: a click (or Y with a controller) fills the address.
    const auto recent = RecentServers();
    if (!recent.empty()) {
      Text(d, g_fonts.bf_med, 20 * s, ImVec2(px, y), kTableFaint, "RECENT");
      y += 30 * s;
      for (size_t i = 0; i < recent.size() && i < 3; ++i) {
        const std::string& r = recent[i];
        const ImVec2 r0(px, y), r1(x0 + pw - 30 * s, y + 38 * s);
        Text(d, g_fonts.bf_med, 25 * s, ImVec2(px, CapTop(y, 38 * s, 25 * s)), kTableInk, Upper(r.substr(0, r.find('|'))));
        if (ImGui::IsMouseHoveringRect(r0, r1) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
          direct_text_ = r.substr(r.find('|') + 1);
        y += 40 * s;
      }
      if (in.y) direct_text_ = recent[0].substr(recent[0].find('|') + 1);
    }
    // The on screen keys: numbers, the dot and the colon; ABC for the letters of a name.
    static const char* kDigits[] = {"1", "2", "3", "DEL", "4", "5", "6", ".", "7", "8", "9", ":", "ABC", "0", "-", "CONNECT"};
    static const char* kLetters[] = {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "A", "S", "D", "F", "G", "H",
                                     "J", "K", "L", ".", "Z", "X", "C", "V", "B", "N", "M", "123", "DEL", "CONNECT"};
    const int cols = direct_letters_ ? 10 : 4, count = direct_letters_ ? 30 : 16;
    const char* const* keys = direct_letters_ ? kLetters : kDigits;
    const float kx = x0 + pw + 40 * s, kw = x1 - kx, gap = 10 * s;
    const float cw = (kw - gap * float(cols - 1)) / float(cols), ch = direct_letters_ ? 70 * s : 86 * s;
    Text(d, g_fonts.bf_med, 21 * s, ImVec2(kx, top - 2 * s), IM_COL32(201, 196, 178, 255), "ON SCREEN KEYS (A OPENS THEM ON A CONTROLLER)");
    direct_key_ = std::clamp(direct_key_, 0, count - 1);
    int pressed = -1;
    for (int k = 0; k < count; ++k) {
      const ImVec2 k0(kx + float(k % cols) * (cw + gap), top + 34 * s + float(k / cols) * (ch + gap));
      const ImVec2 k1(k0.x + cw, k0.y + ch);
      const bool sel = direct_keys_ && k == direct_key_;
      d->AddRectFilled(k0, k1, sel ? kTableGreen : IM_COL32(233, 230, 220, 255));
      const float size = std::string(keys[k]).size() > 1 ? 22 * s : 30 * s;
      Text(d, g_fonts.bf, size, ImVec2((k0.x + k1.x - Width(g_fonts.bf, size, keys[k])) * 0.5f, CapTop(k0.y, ch, size)),
           sel ? IM_COL32(255, 255, 255, 255) : kTableInk, keys[k]);
      if (ImGui::IsMouseHoveringRect(k0, k1) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) pressed = k, direct_key_ = k;
    }
    if (direct_keys_) {
      if (in.left) direct_key_ = (direct_key_ / cols) * cols + (direct_key_ % cols + cols - 1) % cols;
      if (in.right) direct_key_ = (direct_key_ / cols) * cols + (direct_key_ % cols + 1) % cols;
      if (in.up) direct_key_ = (direct_key_ + count - cols) % count;
      if (in.down) direct_key_ = (direct_key_ + cols) % count;
      if (in.accept) pressed = direct_key_;
      if (in.x && !direct_text_.empty()) direct_text_.pop_back();
    } else if (in.accept && !ImGui::IsKeyPressed(ImGuiKey_Enter) && !ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
      direct_keys_ = true;
    }
    if (pressed >= 0) {
      const std::string k = keys[pressed];
      if (k == "DEL") {
        if (!direct_text_.empty()) direct_text_.pop_back();
      } else if (k == "ABC" || k == "123") {
        direct_letters_ = !direct_letters_, direct_key_ = 0;
      } else if (k == "CONNECT") {
        if (!direct_job_) DirectConnect();
      } else if (direct_text_.size() < 64) {
        direct_text_ += direct_letters_ ? char(std::tolower(static_cast<unsigned char>(k[0]))) : k[0];
      }
    }
    if ((in.start || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) && !direct_job_)
      DirectConnect();
    BrowserHints(d, s, x0, io.DisplaySize.y - 62 * s,
                 direct_keys_ ? std::vector<std::pair<std::string, std::string>>{{"A", "Type"}, {"X", "Delete"}, {"START", "Connect"}, {"B", "Done typing"}}
                              : std::vector<std::pair<std::string, std::string>>{{"A", "Type"}, {"START", "Connect"}, {"LB", "Tabs"}, {"B", "Back"}});
    if (in.back) {
      if (direct_keys_) direct_keys_ = false;
      else page_ = Page::kMain, row_ = 1;
    }
  }
  std::vector<std::string> RecentServers() {
    std::vector<std::string> out;
    const std::string all = settings_.Get("recent_servers", "");
    for (size_t at = 0; at < all.size();) {
      const size_t end = std::min(all.find(';', at), all.size());
      if (const std::string r = all.substr(at, end - at); r.find('|') != std::string::npos) out.push_back(r);
      at = end + 1;
    }
    return out;
  }

  // Without a saved choice the frame rate follows bf_unlock_fps and bf_max_fps (so a command line sets it for tests).
  static std::string DefaultFrameRate() {
    if (!REXCVAR_GET(bf_unlock_fps)) return "30";
    const double max_fps = REXCVAR_GET(bf_max_fps);
    return max_fps <= 0 ? "unlimited" : std::to_string(int(max_fps));
  }

  // The frame rates offered (TekRantGaming, 9 Nov 2026): the game updates its world 30 times a second, so rates that divide
  // into 30 keep motion even; up to the screen's refresh rate and a frame limit set outside the game
  // (frame_cap.cpp: NVIDIA's Max Frame Rate, or one measured). Unlimited only without such a limit.
  int ScreenRefresh() {
    rex::ui::Window* window = window_ ? window_() : nullptr;
    if (settings_.Get("fullscreen", "off") == "on")
      if (auto mode = ParseDisplayMode(settings_.Get("fullscreen_mode", "desktop")); mode && mode->refresh > 0)
        return int(std::lround(mode->refresh));
    const DisplayMode desktop = DesktopDisplayMode(std::atoi(settings_.Get("monitor", "0").c_str()), window);
    return desktop.refresh > 0 ? int(std::lround(desktop.refresh)) : 0;
  }
  std::vector<std::string> OfferedRates() {
    const int refresh = ScreenRefresh(), cap = SystemFrameCap();
    int most = refresh > 0 ? refresh + 1 : 1000;  // 59.94 Hz offers 60
    if (cap > 0) most = std::min(most, cap);
    std::vector<std::string> rates;
    for (int r = 30; r <= 240; r += 30)
      if (r <= 60 || r <= most) rates.push_back(std::to_string(r));
    if (cap <= 0) rates.push_back("unlimited");
    return rates;
  }
  // The saved choice as it applies now: the highest offered rate at or below it (110 or 144 saved: 90 or 120).
  std::string EffectiveFrameRate() {
    const std::string saved = settings_.Get("frame_rate", DefaultFrameRate());
    const std::vector<std::string> rates = OfferedRates();
    if (std::find(rates.begin(), rates.end(), saved) != rates.end()) return saved;
    const int want = saved == "unlimited" ? 100000 : std::atoi(saved.c_str());
    std::string best = "30";
    for (const std::string& r : rates)
      if (r != "unlimited" && std::atoi(r.c_str()) <= want) best = r;
    return best;
  }
  void ApplyFrameRate() {
    const std::string fps = EffectiveFrameRate();
    applied_cap_ = SystemFrameCap();
    SetFrameRate(fps == "unlimited" ? 0.0 : std::atof(fps.c_str()));
    if (fps != applied_rate_) {
      REXLOG_INFO("BF frame rate: {} (chosen {}; screen {} Hz; a limit outside the game: {})", fps,
                  settings_.Get("frame_rate", DefaultFrameRate()), ScreenRefresh(),
                  applied_cap_ ? std::to_string(applied_cap_) : std::string("none"));
      applied_rate_ = fps;
    }
  }

  // ---- settings: King Kong's pages (kk/src/launcher.cpp, settings_layout.h): Display, Graphics, Audio, Controls,
  // Gameplay, in King Kong's order, rows and wording. Every value is saved in bf1943-settings.txt: our own keys,
  // and runtime settings as "cvar.<name>" (set again at every start, ApplyStartupDisplaySettings). Rows that need
  // more of the game found first are shown, greyed, with a note.
  enum Tab { kTabDisplay, kTabGraphics, kTabAudio, kTabControls, kTabGameplay, kTabEnhancements, kTabCount };
  static constexpr const char* kTabNames[kTabCount] = {"DISPLAY", "GRAPHICS", "AUDIO", "CONTROLS", "GAMEPLAY",
                                                       "ENHANCEMENTS"};
  static constexpr const char* kNotYet = "Coming in a later version: this needs more of the game's own code found first.";

  struct SettingRow {
    std::string section, name, help;
    std::vector<std::string> options;
    int index = 0;
    std::function<void(int)> choose;  // null: a row that only informs (or a slider)
    std::string value;                // shown for a row without options
    bool slider = false;
    float lo = 0, hi = 1, step = 1, val = 0;
    std::function<void(float)> set;
    std::function<std::string(float)> format;
    std::function<void()> activate;  // a row that opens a page or does something
    bool disabled = false;
    std::string note;  // why it is greyed
    // A binding's row (13 Nov 2026): its four slots (two keys, two mouse buttons; empty: none), the slot the
    // controller or the arrow keys are on, the one waiting for a press (-1: none), and a click on one. `nudge`: left
    // and right on any row that wants them (-1, 1).
    std::vector<std::string> slots;
    int slot_cursor = 0, slot_capture = -1;
    std::function<void(int)> slot_click;
    std::function<void(int)> nudge;
    std::string start_key;  // a setting taken only at the start ("cvar.<name>" or a settings key): NEXT START if changed
  };

  // Settings the renderer or the window take only at the start (their rows say "Applies at the next start"). Their
  // values as this run started: a changed one shows NEXT START on its row, and the setup restarts the game at its end
  // (TekRantGaming, 8 Nov: the setup's DLAA and Ultra shadows only arrived at the second start, which then ran much slower
  // than the first match).
  static constexpr const char* kStartCvars[] = {"native_x_renderer_alpha_to_coverage", "bf_temporal", "bf_temporal_mode", "native_x_renderer_frame_generation",
                                                "native_x_renderer_shadow_scale", "native_x_renderer_sharpness",
                                                "native_x_renderer_scaling", "bf_upscaler_mode", "user_language",
                                                "native_x_renderer_fill_screen", "bf_render_scale"};
  static constexpr const char* kStartKeys[] = {"vsync", "monitor", "window_width", "window_height"};
  std::map<std::string, std::string> started_with_;
  std::string StartValue(const std::string& key) {
    return key.rfind("cvar.", 0) == 0 ? Cvar(key.substr(5)) : settings_.Get(key, "");
  }
  void NoteStartValues() {
    for (const char* c : kStartCvars) started_with_[std::string("cvar.") + c] = Cvar(c);
    for (const char* k : kStartKeys) started_with_[k] = settings_.Get(k, "");
  }
  bool StartChanged(const std::string& key) {
    // Before the game runs the renderer's settings are not taken yet: they apply at Play.
    if (pre_game_ && key.rfind("cvar.", 0) == 0) return false;
    const auto it = started_with_.find(key);
    if (it == started_with_.end()) return false;
    const std::string now = StartValue(key);
    if (now == it->second) return false;
    // "2" and "2.000000" are the same setting.
    char* e1 = nullptr;
    char* e2 = nullptr;
    const double a = std::strtod(now.c_str(), &e1), b = std::strtod(it->second.c_str(), &e2);
    if (!now.empty() && !it->second.empty() && !*e1 && !*e2) return a != b;
    return true;
  }
  bool RestartPending() {
    for (const auto& [key, value] : started_with_)
      if (StartChanged(key)) return true;
    return false;
  }
  bool Pending(const SettingRow& r) { return !r.start_key.empty() && StartChanged(r.start_key); }
  // Starts the game again for the settings taken at the start (the setup's end, Settings from Home).
  void RestartForSettings(const char* why) {
    std::string changed;
    for (const auto& [key, value] : started_with_)
      if (StartChanged(key)) changed += (changed.empty() ? "" : ", ") + key;
    REXLOG_INFO("BF launcher: restarting for the settings taken at the start ({}; {})", changed, why);
    settings_.Save();
    if (update::RestartSelf() && quit_) quit_();
  }

  // A runtime setting as the menu sees it: the saved value, else the runtime's own.
  // 13 Nov 2026 ("create buttons in the Graphics submenu on the bottom for Apply settings, and Restore defaults, that
  // way changes in Graphics are only made if applied"): while the Settings page shows Graphics (defer_graphics_), a
  // row's change is kept aside (gfx_pending_) and the rows read it back, so the page shows what Apply would set;
  // Apply makes the changes, leaving the page without it drops them. gfx_cvars_: every setting the Graphics rows
  // read, for Restore defaults.
  bool defer_graphics_ = false;
  std::map<std::string, std::string> gfx_pending_;
  std::set<std::string> gfx_cvars_;
  // The Settings page draws Keybinds and Remap gamepad as buttons over the Controls rows (the same day): the two
  // rows that opened them are left out of the list there.
  bool controls_buttons_ = false;
  std::string Cvar(const std::string& name) {
    if (defer_graphics_) {
      gfx_cvars_.insert(name);
      if (auto it = gfx_pending_.find(name); it != gfx_pending_.end()) return it->second;
    }
    const std::string saved = settings_.Get("cvar." + name, "");
    return saved.empty() ? rex::cvar::GetFlagByName(name) : saved;
  }
  void ApplyGraphicsPending() {
    const std::map<std::string, std::string> changes = std::move(gfx_pending_);
    gfx_pending_.clear();
    const bool was = defer_graphics_;
    defer_graphics_ = false;
    for (const auto& [name, value] : changes) {
      REXLOG_INFO("BF settings: GRAPHICS applied {} = {}", name, value);
      SetCvar(name, value);
    }
    defer_graphics_ = was;
  }
  // Every Graphics setting back to the game's own default, kept aside like any change: Apply makes it so.
  void RestoreGraphicsDefaults() {
    for (const std::string& name : gfx_cvars_) {
      const auto* info = rex::cvar::GetFlagInfo(name);
      if (!info) continue;
      const std::string saved = settings_.Get("cvar." + name, "");
      const std::string now = saved.empty() ? rex::cvar::GetFlagByName(name) : saved;
      if (now != info->default_value) gfx_pending_[name] = info->default_value;
      else gfx_pending_.erase(name);
    }
    REXLOG_INFO("BF settings: GRAPHICS defaults shown ({} settings differ), waiting for Apply", gfx_pending_.size());
  }
  void SetCvar(const std::string& name, const std::string& value) {
    if (defer_graphics_) {
      const std::string saved = settings_.Get("cvar." + name, "");
      if (value == (saved.empty() ? rex::cvar::GetFlagByName(name) : saved)) gfx_pending_.erase(name);
      else gfx_pending_[name] = value;
      return;
    }
    settings_.values["cvar." + name] = value;
    settings_.Save();
    rex::cvar::SetFlagByName(name, value);
    // The Upscaler's rows and the Sharpness it stands in for: the renderer's settings follow before the game runs (Play
    // takes them; once it runs, they wait for the next start, as the rows say).
    if (name == "native_x_renderer_sharpness") g_plain_sharpness = value;
    if (name == "native_x_renderer_scaling" || name == "bf_upscaler_mode" || name == "native_x_renderer_sharpness" ||
        name == "bf_render_scale" || name == "bf_temporal" || name == "bf_temporal_mode" || name == "bf_post_aa")
      ApplyUpscaler(false, settings_.Get("nis_support", ""));
  }
  static bool Truthy(const std::string& v) { return v == "true" || v == "1" || v == "on"; }

  SettingRow Choice(const std::string& section, const std::string& name, const std::string& help,
                    const std::string& cvar, const std::vector<std::pair<std::string, std::string>>& options) {
    SettingRow r{section, name, help};
    const std::string cur = Cvar(cvar);
    std::vector<std::string> values;
    for (size_t i = 0; i < options.size(); ++i) {
      r.options.push_back(options[i].first);
      values.push_back(options[i].second);
      if (options[i].second == cur) r.index = int(i);
    }
    r.choose = [this, cvar, values](int i) { SetCvar(cvar, values[size_t(i)]); };
    for (const char* c : kStartCvars)
      if (cvar == c) r.start_key = "cvar." + cvar;
    return r;
  }
  SettingRow Toggle(const std::string& section, const std::string& name, const std::string& help,
                    const std::string& cvar, const std::string& off = "Off", const std::string& on = "On") {
    SettingRow r = Choice(section, name, help, cvar, {{off, "false"}, {on, "true"}});
    r.index = Truthy(Cvar(cvar)) ? 1 : 0;
    return r;
  }
  SettingRow Slider(const std::string& section, const std::string& name, const std::string& help,
                    const std::string& cvar, float lo, float hi, float step, std::function<std::string(float)> format,
                    float fallback = 0) {
    SettingRow r{section, name, help};
    r.slider = true, r.lo = lo, r.hi = hi, r.step = step;
    const std::string cur = Cvar(cvar);  // empty for a setting not in this version yet: its default
    float v = cur.empty() ? fallback : float(std::atof(cur.c_str()));
    // Below the range: the setting's "the game's own" (bf_fov 0 is the game's 85.6, shown as 60 before).
    if (v < lo && fallback >= lo) v = fallback;
    r.val = std::clamp(v, lo, hi);
    r.format = std::move(format);
    r.set = [this, cvar](float v) {
      char b[32];
      std::snprintf(b, sizeof(b), "%g", v);
      SetCvar(cvar, b);
    };
    return r;
  }
  static SettingRow NotYet(SettingRow r) {
    r.disabled = true, r.note = kNotYet;
    return r;
  }
  static std::string Percent(float v) { return std::to_string(int(std::lround(v))) + "%"; }

  // ---- the game's own options: every row of its Controls and Sound / Image pages (common_screens.apt), sent as its
  // pages send them, FSCommand setSetting "<category>,<key>,<value>" and then applySettings "<category>"
  // (control_to_settings), so a change takes effect at once, in a match too (TekRantGaming, 10 Nov: inverted flight and the
  // rest of the original pause menu's pages were missing). The values are its widgets' own: a toggle "0" or "1"
  // (its flight toggle the other way round: inverted is "0"), a list its index, a slider a fraction. Saved here as
  // "game.<category>.<key>" and sent again after each start's sign in (SendGameSettings): the game starts every run
  // from its defaults (settings/gamesettings.dbx in startup-00.fbrb), which are each row's fallback.
  std::string GameValue(const std::string& category, const std::string& key, const std::string& fallback) const {
    return settings_.Get("game." + category + "." + key, fallback);
  }
  // The game's own Vertical look (Game controls, GstInput Scheme2FlipY, sent to the game), for the pad hook's thread
  // (look.cpp: the game negates the right stick's y in land vehicles and boats with it, so the stick value the
  // vehicle mouse gives must be negated too, or a tank's gun goes the wrong way for a player who set it:
  // TekRantGaming, 12 Nov 2026, 0.3.0 players with the mouse). Kept in an atomic: the settings map is the UI thread's.
  // 0.4.1's invert settings from 0.4.0's, once (the review of pull request 16, as of 12 before it: the pull request
  // stopped reading the mouse's "Mouse vertical in vehicles" and cleared the controller's Camera vertical without
  // carrying either over, so inverted players would have been turned round by the update):
  //   - the mouse in vehicles (bf_mouse_invert_y_vehicle) gets what 0.4.0 gave: its Inverted or Normal, or with "Same
  //     as on foot" (the default) the on foot invert;
  //   - the controller's Camera vertical (bf_invert_rs_y, the stick's y turned round before the game) is folded into
  //     the game's own Vertical look settings for each seat (FlipY turned round), and cleared.
  // The mouse's new Invert flight is not carried over, on purpose: mouse flying is a flight stick now (the notes say so).
  void MigrateInvert041() {
    if (settings_.Get("settings_041_invert", "") == "1") return;
    auto saved = [this](const char* name, const std::string& fallback) {
      const std::string v = settings_.Get(std::string("cvar.") + name, "");
      return v.empty() ? fallback : v;
    };
    auto put = [this](const char* name, bool on) {
      settings_.values[std::string("cvar.") + name] = on ? "true" : "false";
      rex::cvar::SetFlagByName(name, on ? "true" : "false");
    };
    // (0.3.0's own bf_mouse_invert_y_vehicle is not read: 0.4.0 went by this setting, and a "Normal" player who still
    // had it on would have been turned round: the review of pull request 16.)
    const std::string vertical = saved("bf_mouse_vehicle_vertical", "same");
    const bool foot = Truthy(saved("bf_mouse_invert_y", "false"));
    const bool vehicles = vertical == "inverted" ? true : vertical == "normal" ? false : foot;
    put("bf_mouse_invert_y_vehicle", vehicles);
    const bool fold = Truthy(saved("bf_invert_rs_y", "false"));
    if (fold) {
      for (const char* k : {"Scheme1FlipY", "Scheme2FlipY", "Scheme4FlipY"}) {
        const std::string key = std::string("game.GstInput.") + k;
        settings_.values[key] = settings_.Get(key, "0") == "1" ? "0" : "1";
      }
      const std::string fly = "game.GstInput.Scheme3FlipY";
      settings_.values[fly] = settings_.Get(fly, "1") == "1" ? "0" : "1";
      put("bf_invert_rs_y", false);
    }
    settings_.values["settings_041_invert"] = "1";
    settings_.Save();
    REXLOG_INFO("BF settings: 0.4.1's invert settings from 0.4.0's: the mouse in vehicles {}{}",
                vehicles ? "inverted" : "normal", fold ? "; the controller's Camera vertical folded into the game's own" : "");
  }
  void RefreshGameFlipY() {
    game_flip_y_.store(GameValue("GstInput", "Scheme2FlipY", "0") == "1", std::memory_order_relaxed);
  }
  void SetGameValue(const std::string& category, const std::vector<std::string>& keys, const std::string& value) {
    for (const auto& k : keys) settings_.values["game." + category + "." + k] = value;
    settings_.Save();
    RefreshGameFlipY();
    if (!signed_in_once_) return;  // sent after the sign in (Show)
    for (const auto& k : keys) GameCommand("setSetting", category + "," + k + "," + value);
    GameCommand("applySettings", category);
  }
  // At the first sign in of a run: every saved one to the game, a category at a time as its pages do.
  void SendGameSettings() {
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> by_category;
    for (const auto& [key, value] : settings_.values) {
      if (key.rfind("game.", 0) != 0) continue;
      const size_t dot = key.find('.', 5);
      if (dot == std::string::npos || value.empty()) continue;
      by_category[key.substr(5, dot - 5)].push_back({key.substr(dot + 1), value});
    }
    for (const auto& [category, values] : by_category) {
      for (const auto& [key, value] : values) GameCommand("setSetting", category + "," + key + "," + value);
      GameCommand("applySettings", category);
      REXLOG_INFO("BF launcher: sent the game's {} options ({} saved)", category, values.size());
    }
  }
  SettingRow GameChoice(const std::string& section, const std::string& name, const std::string& help,
                        const std::string& category, const std::vector<std::string>& keys, const std::string& fallback,
                        const std::vector<std::pair<std::string, std::string>>& options) {
    SettingRow r{section, name, help};
    const std::string cur = GameValue(category, keys.front(), fallback);
    std::vector<std::string> values;
    for (size_t i = 0; i < options.size(); ++i) {
      r.options.push_back(options[i].first);
      values.push_back(options[i].second);
      if (options[i].second == cur) r.index = int(i);
    }
    r.choose = [this, category, keys, values](int i) { SetGameValue(category, keys, values[size_t(i)]); };
    return r;
  }
  // A slider of the game's (0 to 1), shown in percent.
  SettingRow GameSlider(const std::string& section, const std::string& name, const std::string& help,
                        const std::string& category, const std::string& key, float fallback) {
    SettingRow r{section, name, help};
    r.slider = true, r.lo = 0, r.hi = 100, r.step = 5;
    const std::string cur = GameValue(category, key, "");
    r.val = std::clamp(cur.empty() ? fallback * 100 : float(std::atof(cur.c_str())) * 100, 0.0f, 100.0f);
    r.format = Percent;
    r.set = [this, category, key](float v) {
      char b[32];
      std::snprintf(b, sizeof(b), "%g", std::clamp(v, 0.0f, 100.0f) / 100.0f);
      SetGameValue(category, {key}, b);
    };
    return r;
  }

  std::vector<SettingRow> RowsOf(int tab) {
    switch (tab) {
      case kTabDisplay: return DisplayRows();
      case kTabGraphics: return GraphicsRows();
      case kTabAudio: return AudioRows();
      case kTabControls: return sub_ == kSubRemap ? RemapRows() : sub_ == kSubKeys ? KeyRows() : ControlsRows();
      case kTabEnhancements: return EnhancementsRows();
      default: return GameplayRows();
    }
  }

  std::vector<SettingRow> DisplayRows() {
    std::vector<SettingRow> rows;
    rex::ui::Window* window = window_ ? window_() : nullptr;
    const int monitor = std::atoi(settings_.Get("monitor", "0").c_str());
    if (modes_monitor_ != monitor || !modes_listed_) {
      modes_ = ListDisplayModes(monitor, window);
      displays_ = ListDisplays();
      modes_monitor_ = monitor, modes_listed_ = true;
    }
    const bool fullscreen = settings_.Get("fullscreen", "off") == "on";
    const std::string mode_value = settings_.Get("fullscreen_mode", "desktop");
    const bool exclusive = fullscreen && ParseDisplayMode(mode_value).has_value();
    const DisplayMode desktop = DesktopDisplayMode(monitor, window);
    auto size = [](int w, int h) { return std::to_string(w) + " \xC3\x97 " + std::to_string(h); };

    SettingRow mode{"Screen", "Window mode", "How the game fills your screen: in a window, borderless or fullscreen.",
                    {"Windowed", "Borderless", "Fullscreen"}, !fullscreen ? 0 : exclusive ? 2 : 1};
    mode.choose = [this, monitor, desktop](int i) {
      settings_.values["fullscreen"] = i ? "on" : "off";
      if (i == 1) settings_.values["fullscreen_mode"] = "desktop";
      if (i == 2 && !ParseDisplayMode(settings_.Get("fullscreen_mode", "desktop"))) {
        const float rate = HighestRefreshAt(modes_, desktop.width, desktop.height);
        settings_.values["fullscreen_mode"] = DisplayMode{desktop.width, desktop.height, rate}.Value();
      }
      settings_.Save();
      if (window_) ApplyFullscreenMode(window_(), monitor, settings_.Get("fullscreen_mode", "desktop"));
      if (fullscreen_) fullscreen_(i != 0);
    };
    rows.push_back(mode);

    SettingRow mon{"Screen", "Monitor", "Which display the game opens on. Applies at the next start.", {"Default"}, 0};
    for (size_t i = 0; i < displays_.size(); ++i)
      mon.options.push_back("Display " + std::to_string(i + 1) + (displays_[i].primary ? " (main)" : "") +
                            "  \xC2\xB7  " + size(displays_[i].width, displays_[i].height));
    mon.index = std::clamp(monitor, 0, int(mon.options.size()) - 1);
    mon.start_key = "monitor";
    mon.choose = [this](int i) {
      settings_.values["monitor"] = std::to_string(i);
      settings_.Save();
    };
    rows.push_back(mon);

    if (!fullscreen) {
      static const std::pair<int, int> kSizes[] = {{0, 0},       {1280, 720},  {1600, 900},
                                                   {1920, 1080}, {2560, 1440}, {3200, 1800}};
      SettingRow win{"Screen", "Window size", "The size of the game's window. Applies at the next start.", {}, 0};
      win.start_key = "window_width";
      std::vector<std::pair<int, int>> sizes;
      const int w = std::atoi(settings_.Get("window_width", "0").c_str());
      const int h = std::atoi(settings_.Get("window_height", "0").c_str());
      for (auto [sw, sh] : kSizes) {
        if (desktop.width && (sw >= desktop.width || sh >= desktop.height)) continue;  // must fit with its frame
        if (sw == w && sh == h) win.index = int(sizes.size());
        win.options.push_back(sw ? size(sw, sh) : "Default");
        sizes.emplace_back(sw, sh);
      }
      win.choose = [this, sizes](int i) {
        settings_.values["window_width"] = std::to_string(sizes[size_t(i)].first);
        settings_.values["window_height"] = std::to_string(sizes[size_t(i)].second);
        settings_.Save();
      };
      rows.push_back(win);
    } else if (exclusive) {
      const auto cur = ParseDisplayMode(mode_value);
      SettingRow res{"Screen", "Resolution", "The size your screen switches to while the game runs.", {}, -1};
      std::vector<std::pair<int, int>> sizes;
      for (const DisplayMode& m : modes_) {
        if (!sizes.empty() && sizes.back() == std::pair<int, int>{m.width, m.height}) continue;
        if (cur && m.width == cur->width && m.height == cur->height) res.index = int(sizes.size());
        sizes.emplace_back(m.width, m.height);
        res.options.push_back(size(m.width, m.height));
      }
      if (res.index < 0 && cur) {  // a size this display doesn't have
        res.index = int(sizes.size());
        sizes.emplace_back(cur->width, cur->height);
        res.options.push_back(size(cur->width, cur->height) + " (not on this display)");
      }
      res.index = std::max(res.index, 0);
      res.choose = [this, sizes, monitor](int i) {
        const auto [w, h] = sizes[size_t(i)];
        settings_.values["fullscreen_mode"] = DisplayMode{w, h, HighestRefreshAt(modes_, w, h)}.Value();
        settings_.Save();
        if (window_) ApplyFullscreenMode(window_(), monitor, settings_.Get("fullscreen_mode", "desktop"));
      };
      rows.push_back(res);
    } else {
      SettingRow res{"Screen", "Resolution", "Borderless shows the game at your desktop's size.", {}, 0};
      res.value = size(desktop.width, desktop.height);
      rows.push_back(res);
    }

    {
      const auto cur = ParseDisplayMode(mode_value);
      const DisplayMode shown = exclusive && cur ? DisplayMode{cur->width, cur->height,
                                                               HighestRefreshAt(modes_, cur->width, cur->height)}
                                                 : desktop;
      SettingRow rate{"Screen", "Refresh rate", "", {}, 0};
      rate.value = shown.refresh > 0 ? shown.RateLabel() : "";
      if (exclusive) {
        rate.help = "Fullscreen switches your screen to its highest refresh rate.";
      } else {
        const float highest = HighestRefreshAt(modes_, desktop.width, desktop.height);
        rate.help = DesktopBelowHighest(desktop, highest)
                        ? "Windows runs your screen at " + desktop.RateLabel() + ", but it can run at " +
                              DisplayMode{0, 0, highest}.RateLabel() + ". Your screen shows at most " +
                              std::to_string(int(std::lround(desktop.refresh))) + " pictures a second until then."
                        : "The game uses the refresh rate Windows has set for your screen.";
      }
      rows.push_back(rate);
    }

    // Ultrawide (0.4.0): NativeXRenderer fills a screen of any shape with a wider view (Hor+, fov.cpp), the HUD kept at
    // 16:9 in the middle; or the 16:9 picture with bars. "Stretch" did nothing under NativeXRenderer (it always
    // letterboxed). Read at the start (the render scale is fixed then).
    SettingRow aspect{"Screen", "Aspect ratio",
                      "Fill screen: on an ultrawide or other non 16:9 screen you see more to the sides, nothing is "
                      "stretched and the HUD stays 16:9 in the middle. Letterbox: the 16:9 picture with bars.",
                      {"Fill screen", "Letterbox 16:9"}, Cvar("native_x_renderer_fill_screen") == "false" ? 1 : 0};
    aspect.start_key = "cvar.native_x_renderer_fill_screen";
    aspect.choose = [this](int i) {
      const char* v = i ? "false" : "true";
      settings_.values["cvar.native_x_renderer_fill_screen"] = v;
      settings_.Save();
      rex::cvar::SetFlagByName("native_x_renderer_fill_screen", v);
    };
    rows.push_back(aspect);
    // With Fill screen on a wider screen: the HUD at the screen's edges (the minimap left, the ammo right) or all of it
    // in 16:9 in the middle. At once.
    SettingRow hud{"Screen", "HUD layout",
                   "With Fill screen on an ultrawide screen: Edges puts the minimap and ammo at the screen's sides; "
                   "Centered keeps the whole HUD in 16:9 in the middle.",
                   {"Edges", "Centered"}, Cvar("native_x_renderer_hud_layout") == "centered" ? 1 : 0};
    hud.choose = [this](int i) {
      const char* v = i ? "centered" : "edges";
      settings_.values["cvar.native_x_renderer_hud_layout"] = v;
      settings_.Save();
      rex::cvar::SetFlagByName("native_x_renderer_hud_layout", v);
    };
    rows.push_back(hud);

    // Picture: brightness and gamma of the game's picture, in the GPU plugin's gamma ramp (King Kong's curve), at
    // once; our menus and the overlays stay as they are.
    rows.push_back(Slider("Picture", "Brightness",
                          "Raises or lowers the darkest parts of the game's picture. Higher shows more in dark places, "
                          "lower gives deeper blacks. White stays the same. The menus do not change. 0 is the game's "
                          "own picture.",
                          "bf_brightness", -50, 50, 1, [](float v) {
                            const int n = int(std::lround(v));
                            return n > 0 ? "+" + std::to_string(n) : std::to_string(n);
                          }));
    rows.push_back(Slider("Picture", "Gamma",
                          "Brightens or darkens the middle tones of the game's picture while black and white stay "
                          "the same. Above 1.00 is brighter, below is darker. 1.00 is the game's own picture.",
                          "bf_gamma", 0.5f, 1.5f, 0.05f, [](float v) {
                            char b[16];
                            std::snprintf(b, sizeof(b), "%.2f", std::round(v * 20.0f) / 20.0f);
                            return std::string(b);
                          }));
    rows.push_back(GameSlider("Picture", "Game brightness",
                              "The game's own brightness from its Image settings, 50% as it starts. Brightness and "
                              "Gamma above change the picture on top of it.",
                              "GstRender", "Brightness", 0.5f));

    SettingRow vsync{"Frame rate", "VSync", "Makes each frame wait for your screen's next refresh. Applies at the next start.",
                     {"Off", "On"}, settings_.Get("vsync", "off") == "on" ? 1 : 0};
    vsync.start_key = "vsync";
    vsync.choose = [this](int i) {
      settings_.values["vsync"] = i ? "on" : "off";
      settings_.Save();
    };
    rows.push_back(vsync);

    const std::vector<std::string> rates = OfferedRates();
    std::vector<std::string> rate_names;
    for (const std::string& r : rates)
      rate_names.push_back(r == "30" ? "30 (Xbox 360)" : r == "unlimited" ? "Unlimited" : r);
    const std::string fps = EffectiveFrameRate();
    std::string help =
        "The most frames the game draws a second. The game updates its world 30 times a second, so these rates keep "
        "motion even. The Xbox 360 ran it at 30. Applies at once.";
    if (const int cap = SystemFrameCap(); cap > 0)
      help += " A frame limit of " + std::to_string(cap) + " is set outside the game (your graphics driver or a "
              "tool), so the most offered is " + rates.back() + ".";
    SettingRow frame{"Frame rate", "Frame rate", help, rate_names,
                     int(std::find(rates.begin(), rates.end(), fps) - rates.begin())};
    if (frame.index >= int(rates.size())) frame.index = std::min(1, int(rates.size()) - 1);
    frame.choose = [this, rates](int i) {
      settings_.values["frame_rate"] = rates[size_t(i)];
      settings_.Save();
      ApplyFrameRate();
    };
    rows.push_back(frame);

    // Camera: King Kong's rows; BF's camera and effects first need finding in the game's code.
    // Field of view (fov.cpp): horizontal at 16:9; the game's own is 86. Aiming down sights still narrows it.
    rows.push_back(Slider("Camera", "Field of view",
                          "How wide the camera sees, across the screen at 16:9. The game's own is 86\xC2\xB0. "
                          "Aiming down sights still zooms in. Applies at once.",
                          "bf_fov", 60, 120, 1,
                          [](float v) { return std::to_string(int(std::lround(v))) + "\xC2\xB0"; }, 86));
    // Vehicles' own (TekRantGaming, 12 Nov 2026: "the current FOV implementation also applies to the vehicle third
    // person camera, let's make that a separate slider").
    rows.push_back(Slider("Camera", "Vehicle field of view",
                          "The camera's width while you sit in a vehicle: the tank, jeep, boat and plane views, their "
                          "guns and turrets included. The game's own is 86\xC2\xB0. Applies at once.",
                          "bf_fov_vehicle", 60, 120, 1,
                          [](float v) { return std::to_string(int(std::lround(v))) + "\xC2\xB0"; }, 86));
    rows.push_back(Toggle("Camera", "Motion blur",
                          "The game's screen blur: from nearby explosions, bullets and low health, and its camera "
                          "blur. Off turns all of it off. Applies at once.",
                          "bf_motion_blur"));
    rows.push_back(Toggle("Camera", "Camera shake", "The camera shaking from explosions and gunfire. Applies at once.",
                                 "bf_camera_shake"));
    rows.push_back(Slider("Camera", "Camera shake strength",
                                 "How hard the camera shakes. 100% is the Xbox 360's; lower it if the shaking bothers "
                                 "you.",
                                 "bf_camera_shake_strength", 10, 200, 5, Percent, 100));
    return rows;
  }

  // The preset the Graphics page's settings are now (GraphicsPresets), or -1: Custom.
  int CurrentGraphicsPreset() {
    const auto& presets = GraphicsPresets();
    for (size_t i = 0; i < presets.size(); ++i) {
      bool all = true;
      for (const auto& [name, value] : presets[i].settings) all = all && PresetValueMatches(name, Cvar(name), value);
      if (all) return int(i);
    }
    return -1;
  }
  void ApplyGraphicsPreset(size_t i, const char* why) {
    const auto& presets = GraphicsPresets();
    if (i >= presets.size()) return;
    for (const auto& [name, value] : presets[i].settings) {
      // NVIDIA NIS where this GPU runs it stays for a preset's AMD FSR 1: both are the upscaler.
      if (std::string(name) == "native_x_renderer_scaling" && std::string(value) == "fsr" &&
          Cvar(name) == "nis" && settings_.Get("nis_support", "") == "1")
        continue;
      SetCvar(name, value);
    }
    settings_.values["graphics_preset"] = presets[i].name;  // the one X goes back to once a row made it Custom
    settings_.Save();
    REXLOG_INFO("BF settings: graphics preset {}{}", presets[i].name, why);
  }

  std::vector<SettingRow> GraphicsRows() {
    std::vector<SettingRow> rows;
    // The preset first: it sets the rows below it (GraphicsPresets); Custom once one of them is changed.
    {
      const auto& presets = GraphicsPresets();
      SettingRow r{"Quality", "Graphics preset",
                   "Sets every setting on this page at once. Potato is for integrated graphics and the oldest cards, "
                   "Medium for a GTX 1060 or RX 580 at 1080p, Ultra for the fastest cards. Custom means a setting below "
                   "was changed since. Some of them apply at the next start."};
      for (const auto& p : presets) r.options.push_back(p.name);
      r.index = CurrentGraphicsPreset();
      if (r.index < 0) {
        r.options.push_back("Custom");
        r.index = int(presets.size());
      }
      r.choose = [this](int i) { ApplyGraphicsPreset(size_t(i), ""); };
      rows.push_back(r);
    }
    // No Graphics API row: Vulkan, our renderer, is the only one (DECISIONS.md, 5 Nov 2026 evening). The rows set
    // NativeXRenderer's own settings: the presenter's swap_post_effect and present_effect, which these rows set before,
    // are not used when NativeXRenderer draws (they changed nothing).
    // Antialiasing (TekRantGaming, 9 Nov 2026, after testing them all: "FXAA works the best, so we're only going to
    // offer that, and it should be on by default"): Off or FXAA until 0.4.1, where pull request 16 (ThatZaliaSGuy,
    // merged at TekRantGaming's word "review and merge", 13 Nov) offers SMAA and Intel CMAA 2 again beside it, and the temporal
    // upscalers in a section of their own. Frame generation stays out (it needs DLSS or FSR 3.1 of SDK 1.1.4).
    // 13 Nov 2026 (this fork): SMAA is offered again beside FXAA, at the reference implementation's four presets
    // (NativeXRenderer has had SMAA 1x since D41; the row offered only FXAA since 9 Nov).
    const std::string temporal_now = Cvar("bf_temporal");  // (through Cvar: a change waiting for Apply shows)
    const bool temporal = temporal_now == "fsr4" || temporal_now == "fsr" || temporal_now == "dlss" ||
                          temporal_now == "xess" || temporal_now == "taa";
    SettingRow aa_row = Choice("Image", "Antialiasing",
                          "Smooths jagged edges. FXAA is the lightest and softens the whole picture a little. SMAA "
                          "finds the edges and smooths only those, so textures and text stay sharp: Low and Medium "
                          "are faster, High and Ultra search further along each edge. CMAA 2 (Intel's) smooths "
                          "edges and long slanted lines while leaving everything else untouched, the sharpest of the "
                          "three; Ultra catches fainter edges.",
                          "bf_post_aa",
                          {{"Off", "off"}, {"FXAA", "fxaa"}, {"SMAA Low", "smaa_low"}, {"SMAA Medium", "smaa_medium"},
                           {"SMAA High", "smaa_high"}, {"SMAA Ultra", "smaa_ultra"}, {"CMAA 2", "cmaa"},
                           {"CMAA 2 Ultra", "cmaa_ultra"}});
    if (temporal) aa_row.disabled = true, aa_row.note = "The upscaler below antialiases the picture itself.";
    rows.push_back(aa_row);
    // The Upscaling section (13 Nov 2026): the temporal upscalers, their mode and their own sharpness. They work on
    // the 3D view before the game's post-processing, with the picture's motion, so they also steady shimmer that the
    // antialiasing above cannot.
    rows.push_back(Choice("Upscaling", "Upscaler",
                          "Antialiases, and in the modes below Native upscales, with what the last frames showed: the "
                          "steadiest picture. AMD FSR 4 is AMD's newest (Radeon RX 6000 and later with a current "
                          "driver; elsewhere AMD's library picks what the card runs). AMD FSR 3.1 runs on any card. "
                          "Intel XeSS runs on any recent card. NVIDIA DLSS needs a GeForce RTX. TAA is the game's own plain "
                          "temporal antialiasing, at your display's resolution only. Replaces Antialiasing and the spatial upscaler. Applies at "
                          "the next start.",
                          "bf_temporal",
                          {{"Off", "off"}, {"AMD FSR 4", "fsr4"}, {"AMD FSR 3.1", "fsr"}, {"Intel XeSS", "xess"},
                           {"NVIDIA DLSS", "dlss"}, {"TAA", "taa"}}));
    {
      SettingRow mode = Choice("Upscaling", "Upscaler mode",
                               "Native AA draws the game at your display's resolution and only antialiases. The others "
                               "draw fewer pixels and upscale: faster, a little softer. Applies at the next start.",
                               "bf_temporal_mode",
                               {{"Native AA", "native"}, {"Quality", "quality"}, {"Balanced", "balanced"},
                                {"Performance", "performance"}, {"Ultra Performance", "ultra_performance"}});
      SettingRow sharp = Choice("Upscaling", "Upscaler sharpness",
                                "The upscaler's own sharpening of its result (AMD FSR). Applies at once.",
                                "native_x_renderer_upscaler_sharpness",
                                {{"Off", "0.000000"}, {"Low", "0.250000"}, {"Medium", "0.500000"}, {"High", "0.800000"}});
      if (!temporal) {
        mode.disabled = sharp.disabled = true;
        mode.note = sharp.note = "Choose an upscaler first.";
      } else if (Cvar("bf_temporal") == "taa") {
        mode.disabled = sharp.disabled = true;
        mode.note = "TAA works at your display's resolution.";
        sharp.note = "TAA uses the Sharpness row below.";
      } else if (Cvar("bf_temporal") == "dlss" || Cvar("bf_temporal") == "xess") {
        sharp.disabled = true, sharp.note = "Only AMD FSR has a sharpening of its own here.";
      }
      rows.push_back(mode);
      rows.push_back(sharp);
    }
    // Upscaling: King Kong's Upscaler and Upscaler mode rows and their words (TekRantGaming, 11 Nov 2026, v0.2.1: AMD FSR 1 and
    // NVIDIA NIS, "copy exactly that"). NativeXRenderer scales the drawn picture up to the screen with them (D115, on
    // Vulkan, so NIS can run on Linux too: King Kong's NIS is in its Direct3D 12 presenter, Windows only). Off draws the
    // display's resolution, with the Sharpness row; with an upscaler the Upscaler mode says how much of it is drawn and
    // the upscaler sharpens as AMD and NVIDIA made it (ApplyUpscaler).
    // NVIDIA NIS only where this GPU runs it (TekRantGaming, 11 Nov 2026: "ensure that NVIDIA NIS isn't shown to unsupported
    // devices"): the renderer's answer at the last start (D158, nis_support, NoteNisSupport), so not before the first;
    // a NIS chosen before on a GPU that does not run it shows and runs as FSR 1 (ApplyUpscaler). King Kong's sentences
    // for both lists. No Render resolution row (TekRantGaming, 11 Nov 2026): Off draws the display's resolution.
    std::string scaling = Cvar("native_x_renderer_scaling");
    const bool nis_offered = settings_.Get("nis_support", "") == "1";
    if (scaling == "nis" && !nis_offered) scaling = "fsr";
    const bool upscaler = scaling == "fsr" || scaling == "nis";
    {
      std::vector<std::pair<std::string, std::string>> choices = {{"Off", "bilinear"}, {"AMD FSR 1", "fsr"}};
      if (nis_offered) choices.push_back({"NVIDIA NIS", "nis"});
      SettingRow r = Choice("Upscaling", "Spatial upscaler",
                            std::string("Scales the picture up to your screen and sharpens it, for a clearer image when "
                                        "the game draws fewer pixels than your screen has. ") +
                                (nis_offered ? "AMD FSR 1 and NVIDIA Image Scaling (NIS) work on any graphics card. "
                                             : "AMD FSR 1 works on any graphics card. ") +
                                "Off draws the game at your display's resolution. Applies at the next start.",
                            "native_x_renderer_scaling", choices);
      if (Cvar("native_x_renderer_scaling") == "nis" && !nis_offered) r.index = 1;  // runs as FSR 1 here
      if (temporal) r.disabled = true, r.note = "The upscaler above does this.";
      rows.push_back(r);
    }
    if (upscaler) {
      // King Kong's facts under the row: what the game draws for this screen, and the modes that draw the same here.
      const ImGuiIO& io = ImGui::GetIO();
      const int out_w = int(std::lround(io.DisplaySize.x * io.DisplayFramebufferScale.x));
      const int out_h = int(std::lround(io.DisplaySize.y * io.DisplayFramebufferScale.y));
      const std::string mode = Cvar("bf_upscaler_mode");
      std::string help = "How many pixels the game draws before the upscaler scales them to your screen. Applies at the "
                         "next start.";
      std::vector<std::pair<std::string, std::string>> options;
      for (const auto& m : kUpscalerModes) options.push_back({m.label, m.value});
      for (const auto& m : kUpscalerModes) {
        if (mode != m.value) continue;
        const auto drawn = DrawnSizeFor(out_w, out_h, m.ratio);
        auto size = [](std::pair<int, int> s) { return std::to_string(s.first) + "x" + std::to_string(s.second); };
        help += " Draws " + size(drawn) + " for your " + size({out_w, out_h}) +
                (settings_.Get("fullscreen", "") == "on" ? " screen." : " window.");
        std::string same;
        for (const auto& o : kUpscalerModes)
          if (o.value != m.value && DrawnSizeFor(out_w, out_h, o.ratio) == drawn)
            same += std::string(same.empty() ? "" : " and ") + o.label;
        if (!same.empty()) help += " Same as " + same + " here.";
      }
      rows.push_back(Choice("Upscaling", "Spatial upscaler mode", help, "bf_upscaler_mode", options));
    }
    rows.push_back(Choice("Image", "Shadow quality",
                          "The shadow maps' detail. Original draws them at the render resolution. Applies at the next "
                          "start.",
                          "native_x_renderer_shadow_scale",
                          {{"Original", "0.000000"}, {"High (2x)", "2.000000"}, {"Ultra (4x)", "4.000000"}}));
    // Sharpness: CAS when presenting (native_x_renderer_sharpness, 0 to 1), at the next start; with FSR 1 or NIS their
    // own sharpening instead (King Kong offers no sharpness for them).
    {
      SettingRow sharp = Choice("Image", "Sharpness", "Makes fine detail crisper. Applies at the next start.",
                                "native_x_renderer_sharpness",
                                {{"Off", "0.000000"}, {"Low", "0.250000"}, {"Medium", "0.500000"}, {"High", "0.800000"}});
      if (upscaler || (temporal && Cvar("bf_temporal") != "taa"))
        sharp.disabled = true, sharp.note = "The upscaler sharpens the picture itself.";
      rows.push_back(sharp);
    }
    // 13 Nov 2026 (the rendering quirks: "shimmering smoke effects ... texture shimmer"): what the game draws with
    // alpha to coverage (leaves, fences, some effects) is dithered at the console's 1x, in a 2x2 pattern that
    // sparkles as the camera moves at a PC's resolution. Smooth blends it by its alpha instead (NativeXRenderer D69).
    rows.push_back(Choice("Image", "Transparent edges",
                          "How leaves, fences and other see-through edges are drawn. Original dithers them in a fine "
                          "dot pattern, as on the console: solid from afar, but it can sparkle as you move. Smooth "
                          "blends them instead: no sparkle, a little softer, and thin leaves can look more see-through. "
                          "Applies at the next start.",
                          "native_x_renderer_alpha_to_coverage", {{"Original", "auto"}, {"Smooth", "blend"}}));
    rows.push_back(Choice("Image", "Texture filtering", "Keeps the ground and distant textures sharp at steep angles.",
                          "anisotropic_override",
                          {{"Game", "-1"}, {"Off", "0"}, {"2\xC3\x97", "2"}, {"4\xC3\x97", "3"}, {"8\xC3\x97", "4"},
                           {"16\xC3\x97", "5"}}));
    // Ambient occlusion: King Kong's three rows (GTAO, NativeXRenderer D132), all applied at once.
    rows.push_back(Choice("Lighting", "Ambient occlusion",
                          "Soft shadows where objects meet the ground and each other, under vehicles and in corners. "
                          "Applies at once.",
                          "native_ao_mode", {{"Off", "0"}, {"On", "1"}}));
    rows.push_back(Choice("Lighting", "AO strength", "How dark the ambient occlusion is. Applies at once.",
                          "native_ao_strength", {{"Light", "1"}, {"Medium", "2"}, {"Strong", "3"}}));
    rows.push_back(Choice("Lighting", "AO quality",
                          "How many samples the ambient occlusion takes. Higher is smoother and costs more. Applies at "
                          "once.",
                          "native_ao_samples", {{"Low", "1"}, {"Medium", "2"}, {"High", "3"}}));
    // Global illumination: King Kong's SSGI (NativeXRenderer D133), one bounce of light from the lit scene, live.
    rows.push_back(Choice("Lighting", "Global illumination",
                          "Sunlight bouncing off the ground and walls onto what is next to them. Applies at once.",
                          "native_gi_mode", {{"Off", "0"}, {"On", "1"}}));
    rows.push_back(Choice("Lighting", "GI quality", "How many directions the bounce light is gathered from. Applies at once.",
                          "native_gi_quality", {{"Low", "1"}, {"Medium", "2"}, {"High", "3"}}));
    // Far is the most this page offers (TekRantGaming, 13 Nov 2026: "the GI option should never expose full on the
    // settings only the f8 menu the max setting I weant them to be able to ask for in menu is far"). Full stays in the
    // F8 panel (f8_panel.cpp): one saved there shows here as what is in use, and changing the row leaves it for Near,
    // Medium or Far.
    {
      const bool full = std::atof(Cvar("native_gi_distance").c_str()) >= 4.0;
      std::vector<std::pair<std::string, std::string>> distances = {{"Near", "1"}, {"Medium", "2"}, {"Far", "3"}};
      if (full) distances.push_back({"Full (F8 panel)", "4"});
      SettingRow r = Choice("Lighting", "GI distance", "How far away the bounce light is drawn. Applies at once.",
                            "native_gi_distance", distances);
      if (full) {
        r.index = 3;
        r.note = "Full was chosen in the F8 panel. This page offers Near, Medium and Far.";
        r.choose = [this](int i) { SetCvar("native_gi_distance", i <= 0 ? "1" : i == 1 ? "2" : "3"); };
      }
      rows.push_back(r);
    }
    // Grass and shrubs (NativeXRenderer grass.cpp) are always on (TekRantGaming, 10 Nov 2026): no row turns them off, only
    // their distance below is the player's (the F8 panel still switches them, for testing).
    // HD textures (TekRantGaming, 9 Nov 2026: an HD texture pack, imported in the launcher, toggled here; NativeXRenderer D149).
    {
      const fs::path pack = HdPackPath(UserDir());
      std::error_code ec;
      const bool have = fs::exists(pack, ec);
      SettingRow hd = Toggle("Textures", "HD textures",
                             "The game's textures at twice their size, from an HD texture pack you have imported. "
                             "Uses more graphics memory. Applies at the next start.",
                             "native_x_renderer_hd_textures");
      hd.start_key = "cvar.native_x_renderer_hd_textures";
      if (!have) hd.disabled = true, hd.note = "Import an HD texture pack first";
      rows.push_back(hd);
      SettingRow import{"Textures", "Import HD texture pack",
                        "Choose an HD texture pack file (.bfhd) to copy into the game. It replaces one imported "
                        "before."};
      const int percent = hd_import_->percent.load();
      std::string result;
      {
        std::lock_guard lock(hd_import_->mutex);
        result = hd_import_->result;
      }
      import.value = percent >= 0 ? "Copying " + std::to_string(percent) + "%"
                     : !result.empty() ? result
                     : have ? std::to_string(fs::file_size(pack, ec) >> 20) + " MB imported" : "None";
      import.activate = [this] { ImportHdPack(); };
      rows.push_back(import);
    }
    // How far the grass reaches (TekRantGaming, 9 Nov: "Right now it's too small"; later the same day: more grass appeared as
    // he walked towards it, so on the maps every plant within the distance is drawn at its full density, NXR D152;
    // Full map is the default). On a level without a grass map the far rings still have larger, sparser tufts.
    rows.push_back(Choice("Grass", "Grass distance",
                          "How far from you the grass and shrubs are drawn. Every plant within it is drawn, as dense far "
                          "away as near; the smallest far ones fade out. Applies at once.",
                          "native_grass_radius",
                          {{"Low", "45.000000"}, {"Medium", "90.000000"}, {"High", "180.000000"},
                           {"Extreme", "360.000000"}, {"Full map", "1440.000000"}}));
    return rows;
  }

  std::vector<SettingRow> AudioRows() {
    std::vector<SettingRow> rows;
    rows.push_back(Slider("Game audio", "Master volume", "How loud all game audio is. Applies during play.",
                          "bf_master_volume", 0, 100, 1, Percent));
    rows.push_back(Slider("Game audio", "Music volume", "How loud the game's music is, in menus, loading and matches. Applies at once.", "bf_music_volume", 0,
                                 100, 1, Percent, 100));
    rows.push_back(NotYet(Slider("Game audio", "Effects volume", "How loud the game's sound effects are.",
                                 "bf_effects_volume", 0, 100, 1, Percent, 100)));
    rows.push_back(Slider("Game audio", "Voices volume", "How loud the soldiers' voices and radio calls are. Applies at once.",
                                 "bf_voices_volume", 0, 100, 1, Percent, 100));
    rows.push_back(Toggle("Menu", "Menu music",
                          "The game's main menu music in the launcher, made from your game files. Applies at once.",
                          "bf_menu_music"));
    rows.push_back(NotYet(Slider("Menu", "Menu music volume", "How loud the menu's music is.", "bf_menu_music_volume", 0,
                                 100, 5, Percent, 100)));
    rows.push_back(NotYet(Toggle("Menu", "Menu sounds", "The game's own menu sounds as you move around our menu.",
                                 "bf_menu_sounds")));
    rows.push_back(NotYet(Slider("Menu", "Menu sound volume", "How loud the menu sounds are.", "bf_menu_sounds_volume",
                                 0, 100, 5, Percent, 100)));
    // The rest of the game's own Sound page (its master, music and dialogue volumes are the rows above).
    rows.push_back(GameChoice("Game sound", "Sound system",
                              "What you listen on, as the game's own setting: it mixes its sound for that.",
                              "GstAudio", {"SoundSystemSize"}, "1",
                              {{"TV", "0"}, {"Hi-fi", "1"}, {"Home cinema", "2"}}));
    rows.push_back(GameChoice("Game sound", "Voiceover language",
                              "The soldiers' voices: Localized in your game's language, Original as they were "
                              "recorded.",
                              "GstAudio", {"VOLanguage"}, "0", {{"Localized", "0"}, {"Original", "1"}}));
    // Voice chat in matches (comms.cpp): players near you hear you, both teams.
    rows.push_back(Choice("Voice chat", "Voice chat",
                          "Talk to the players near you through your microphone: both teams hear a voice close by, "
                          "fading with distance. Push to talk sends while its key is held (Keyboard bindings), Open "
                          "mic whenever you speak.",
                          "bf_voice_mode", {{"Push to talk", "push"}, {"Open mic", "open"}, {"Off", "off"}}));
    // To 500% (TekRantGaming, 13 Nov 2026: voices were too quiet at 200%). Every voice is also brought to the same
    // speaking level now (comms.cpp VoiceLevel), so 100% is already well above what a quiet microphone gave.
    rows.push_back(Slider("Voice chat", "Voice volume",
                          "How loud other players' voices are. Every voice plays at the same level, whatever its "
                          "microphone; raise this to hear them over the battle.",
                          "bf_voice_volume", 0, 500, 10, Percent, 100));
    rows.push_back(Slider("Voice chat", "Microphone sensitivity",
                          "Open mic: higher picks up quieter speech, and more of the room around you.",
                          "bf_voice_mic_sensitivity", 0, 100, 5, Percent, 50));
    SettingRow reset{"Reset", "Reset page", "Put these settings back to their defaults."};
    reset.activate = [this] {
      SetCvar("bf_master_volume", "100");
      SetGameValue("GstAudio", {"SoundSystemSize"}, "1");
      SetGameValue("GstAudio", {"VOLanguage"}, "0");
      SetCvar("bf_voice_mode", "push");
      SetCvar("bf_voice_volume", "100");
      SetCvar("bf_voice_mic_sensitivity", "50");
    };
    rows.push_back(reset);
    return rows;
  }

  std::vector<SettingRow> ControlsRows() {
    std::vector<SettingRow> rows;
    rows.push_back(Toggle("Input", "Keyboard & mouse",
                          "Controllers always work. With this on, a keyboard and mouse work too, at the same time: the "
                          "mouse turns the camera and stays in the game window while you play (our menu frees it). The "
                          "keys are under Keyboard bindings.",
                          "mnk_mode", "Off", "On"));
    rows.push_back(Choice("Input", "Button prompts",
                          "Which buttons our menus show in their hints. Automatic follows what you used last: the Xbox "
                          "360's buttons for a controller, the keys for the keyboard and mouse.",
                          "bf_button_prompts",
                          {{"Automatic", "auto"}, {"Xbox 360", "xbox360"}, {"Xbox Series", "xbox_series"},
                           {"PlayStation 3", "ps3"}, {"PlayStation 5", "ps5"}, {"Steam Deck", "steam_deck"},
                           {"Keyboard and mouse", "keyboard"}}));
    rows.push_back(Toggle("Input", "Camera turning",
                          "Every frame turns the camera on each frame you see, with the stick and the mouse, so turning "
                          "is as smooth as your frame rate. Original turns it 30 times a second, as on the Xbox 360.",
                          "bf_look", "Original", "Every frame"));
    const bool mnk = Truthy(Cvar("mnk_mode"));
    const std::string mnk_note = "Used when Keyboard & mouse is on.";
    {
      SettingRow r = Slider("Keyboard & mouse", "Mouse sensitivity",
                            "How far the camera turns as you move the mouse. It turns the same however fast you move "
                            "it, as in other first person games.", "bf_mouse_sensitivity", 0.1f, 5.0f, 0.05f,
                            [](float v) {
                              char b[32];
                              std::snprintf(b, sizeof(b), "%.2f\xC3\x97", v);
                              return std::string(b);
                            });
      if (!mnk) r.disabled = true, r.note = mnk_note;
      rows.push_back(r);
      SettingRow zoom = Slider("Keyboard & mouse", "Aiming sensitivity",
                               "How fast the mouse turns while you aim down sights or through a scope. At 100% a mouse "
                               "movement moves the view the same distance on screen as without zoom, for every sight "
                               "and scope.",
                               "bf_mouse_zoom_sensitivity", 25, 200, 5, Percent);
      if (!mnk) zoom.disabled = true, zoom.note = mnk_note;
      rows.push_back(zoom);
      SettingRow veh = Slider("Keyboard & mouse", "Vehicle sensitivity",
                              "How fast the mouse turns vehicles, their guns and planes. The mouse's own: the "
                              "controller's camera sensitivity does not change it.",
                              "bf_mouse_vehicle_sensitivity", 0.1f, 5.0f, 0.05f, [](float v) {
                                char b[32];
                                std::snprintf(b, sizeof(b), "%.2f×", v);
                                return std::string(b);
                              });
      if (!mnk) veh.disabled = true, veh.note = mnk_note;
      rows.push_back(veh);
      SettingRow aim = Toggle("Keyboard & mouse", "Aim",
                              "Hold: keep the right mouse button held to aim. Toggle: click once to aim and again to "
                              "stop. The controller has its own choice.",
                              "bf_mouse_toggle_aim", "Hold", "Toggle");
      if (!mnk) aim.disabled = true, aim.note = mnk_note;
      rows.push_back(aim);
// (No Aim assist row here since 13 Nov 2026: the mouse never has it.)
      SettingRow cam = Toggle("Keyboard & mouse", "Mouse camera",
                              "Turn the camera with the mouse. Off, it turns with the keys bound to the right stick.",
                              "mnk_mouse");
      if (!mnk) cam.disabled = true, cam.note = mnk_note;
      rows.push_back(cam);
      SettingRow inv = Toggle("Keyboard & mouse", "Invert camera",
                              "Inverts the mouse's up and down on foot.", "bf_mouse_invert_y", "Normal", "Inverted");
      if (!mnk || !Truthy(Cvar("mnk_mouse"))) inv.disabled = true, inv.note = mnk_note;
      rows.push_back(inv);
      // Issue #3: vehicles' own, so the soldier and a tank can each be normal or inverted.
      SettingRow vinv = Toggle("Keyboard & mouse", "Invert vehicle",
                               "Inverts the mouse's up and down for a tank's gun, mounted machine guns, AA guns and "
                               "looking around in a vehicle. Normal: up aims up, as on foot.",
                               "bf_mouse_invert_y_vehicle", "Normal", "Inverted");
      if (!mnk || !Truthy(Cvar("mnk_mouse"))) vinv.disabled = true, vinv.note = mnk_note;
      rows.push_back(vinv);
      SettingRow finv = Toggle("Keyboard & mouse", "Invert flight",
                               "Inverts the mouse's up and down in planes. Normal: pulling the mouse back climbs, as a "
                               "flight stick. Inverted: pushing it forward climbs.",
                               "bf_mouse_invert_y_flight", "Normal", "Inverted");
      if (!mnk || !Truthy(Cvar("mnk_mouse"))) finv.disabled = true, finv.note = mnk_note;
      rows.push_back(finv);
    }
    rows.push_back(Slider("Controller", "Camera sensitivity",
                          "How fast the right stick turns the camera and moves your aim.", "bf_camera_sensitivity", 25,
                          300, 5, Percent));
    rows.push_back(Toggle("Controller", "Camera response",
                          "Modern turns the camera the same way in every direction, so circles and diagonals feel even. "
                          "Original passes the stick to the game as the Xbox 360 did.",
                          "bf_camera_modern", "Original", "Modern"));
    rows.push_back(Slider("Controller", "Stick deadzone",
                          "Ignores small stick movements. Raise it if the camera or cursor drifts.", "bf_deadzone", 0,
                          40, 1, Percent));
    rows.push_back(Toggle("Controller", "Camera horizontal",
                          "Which way the camera turns when you push the right stick left or right.", "bf_invert_rs_x",
                          "Normal", "Inverted"));
    // One invert for each of on foot, vehicles and planes (13 Nov: several "Invert look" rows in different sections).
    // They are the game's own FlipY settings; the older stick-level Camera vertical is folded into them once, at the
    // start (MigrateInvert041), so it is off here.
    rows.push_back(GameChoice("Controller", "Invert camera",
                              "Inverts the right stick's up and down on foot, and in gunner and passenger seats.",
                              "GstInput", {"Scheme1FlipY"}, "0", {{"Normal", "0"}, {"Inverted", "1"}}));
    rows.push_back(GameChoice("Controller", "Invert vehicle",
                              "Inverts the right stick's up and down when you drive a tank, a jeep or a boat.",
                              "GstInput", {"Scheme2FlipY", "Scheme4FlipY"}, "0",
                              {{"Normal", "0"}, {"Inverted", "1"}}));
    rows.push_back(GameChoice("Controller", "Invert flight",
                              "Which way a plane pitches. Normal: pushing the stick up climbs. Inverted: pushing it up "
                              "dives, as in a real plane.",
                              "GstInput", {"Scheme3FlipY"}, "1", {{"Normal", "1"}, {"Inverted", "0"}}));
    {
      SettingRow ls{"Controller", "Left stick", "Invert the left stick's axes.", {"Normal", "Invert X", "Invert Y", "Both"}};
      const bool x = Truthy(Cvar("bf_invert_ls_x")), y = Truthy(Cvar("bf_invert_ls_y"));
      ls.index = x && y ? 3 : x ? 1 : y ? 2 : 0;
      ls.choose = [this](int i) {
        SetCvar("bf_invert_ls_x", i == 1 || i == 3 ? "true" : "false");
        SetCvar("bf_invert_ls_y", i == 2 || i == 3 ? "true" : "false");
      };
      rows.push_back(ls);
    }
    rows.push_back(Toggle("Controller", "Aim",
                          "Hold: keep the left trigger held to aim, as on the console. Toggle: press it once to aim and "
                          "again to stop. Opening the menu stops aiming.",
                          "bf_toggle_aim", "Hold", "Toggle"));
    // Aim assist (GitHub issue 4): the game's own, one setting for each device (look.cpp, sub_822B0F40).
    rows.push_back(Toggle("Controller", "Aim assist",
                          "The game's own help with aiming at enemies while you aim with a controller. Off, the aim "
                          "moves only as you move it.",
                          "bf_aim_assist"));
    rows.push_back(Slider("Controller", "Aim assist strength",
                          "How strongly the aim assist pulls your aim. 100% is the console's.", "bf_aim_assist_strength",
                          0, 100, 5, Percent));
    rows.push_back(Toggle("Controller", "Vibration", "Controller rumble.", "bf_vibration"));
    {
      SettingRow strength = Slider("Controller", "Vibration strength",
                                   "How strong the rumble is. Above 100% it is stronger than on the Xbox 360, up to the "
                                   "controller's full strength.",
                                   "bf_vibration_strength", 10, 200, 5, Percent);
      if (!Truthy(Cvar("bf_vibration"))) strength.disabled = true, strength.note = "Switch Vibration on to set its strength.";
      rows.push_back(strength);
    }
    // The game's own Controls page, row for row, with its names and choices.
    rows.push_back(GameChoice("Game controls", "Soldier controls",
                              "The game's own button layouts for playing on foot, from its Controls page.",
                              "GstInput", {"Scheme1InputType"}, "0",
                              {{"Normal", "0"}, {"Southpaw", "1"}, {"Lefty", "2"}, {"Lefty southpaw", "3"}}));
    rows.push_back(GameChoice("Game controls", "Land and boat controls",
                              "The game's own button layouts for driving tanks, jeeps and boats.", "GstInput",
                              {"Scheme2InputType"}, "0",
                              {{"Normal", "0"}, {"Stickdrive", "1"}, {"Southpaw", "2"}, {"Southpaw stickdrive", "3"}}));
    rows.push_back(GameChoice("Game controls", "Plane controls", "The game's own button layouts for flying planes.",
                              "GstInput", {"Scheme3InputType"}, "0",
                              {{"Normal", "0"}, {"Southpaw", "1"}, {"Lefty", "2"}, {"Lefty southpaw", "3"}}));
    rows.push_back(GameSlider("Game controls", "Game sensitivity",
                              "The game's own controller sensitivity, 50% as it starts. Camera sensitivity above works "
                              "on top of it.",
                              "GstInput", "Scheme1Sensitivity", 0.5f));
    rows.push_back(GameChoice("Game controls", "Game vibration",
                              "The game's own vibration strength. Vibration and Vibration strength above work on top "
                              "of it.",
                              "GstInput", {"Vibration"}, "2",
                              {{"Off", "0"}, {"Weak", "1"}, {"Fairly strong", "2"}, {"Strong", "3"}}));
    SettingRow remap{"Mapping", "Button remapping", "Choose what each controller button does in the game."};
    remap.value = ">";
    remap.activate = [this] { sub_ = kSubRemap, row_ = 0; };
    if (!controls_buttons_) rows.push_back(remap);
    SettingRow keys{"Mapping", "Keyboard bindings",
                    "The keys and mouse buttons for each of the game's actions, when Keyboard & mouse is on."};
    keys.value = ">";
    keys.activate = [this] { sub_ = kSubKeys, row_ = 0; };
    if (!controls_buttons_) rows.push_back(keys);
    SettingRow metrics_key{"Keys while playing", "Performance metrics", "F2 switches the performance metrics while you play."};
    metrics_key.value = "F2";
    rows.push_back(metrics_key);
    return rows;
  }

  // Controls > Button remapping: what each controller button presses in the game.
  std::vector<SettingRow> RemapRows() {
    static const std::pair<const char*, const char*> kButtonsShown[] = {
        {"a", "A"},           {"b", "B"},           {"x", "X"},           {"y", "Y"},
        {"lb", "Left bumper"}, {"rb", "Right bumper"}, {"lt", "Left trigger"}, {"rt", "Right trigger"},
        {"ls", "Left stick press"}, {"rs", "Right stick press"}, {"back", "Back"}, {"start", "Start"},
        {"up", "D-pad up"},   {"down", "D-pad down"}, {"left", "D-pad left"}, {"right", "D-pad right"}};
    std::vector<std::pair<std::string, std::string>> targets;
    for (const auto& [id, label] : kButtonsShown) targets.push_back({label, id});
    targets.push_back({"Nothing", "none"});
    std::vector<SettingRow> rows;
    for (const auto& [id, label] : kButtonsShown)
      rows.push_back(Choice("Button remapping", label, std::string("What ") + label + " does in the game.",
                            std::string("bf_remap_") + id, targets));
    SettingRow reset{"Reset", "Reset remapping", "Every button back to its own action."};
    reset.activate = [this] {
      for (const auto& [id, label] : kButtonsShown) SetCvar(std::string("bf_remap_") + id, id);
    };
    rows.push_back(reset);
    return rows;
  }

  // Controls > Keyboard bindings: the SDK's keybind_* settings, each the controller button BF reads, named by what BF
  // does with it (BF's own Help & Options > Controls for the soldier, vehicles and planes, runs help3 and helpveh1; in a
  // vehicle the walk keys are the throttle and the brake and the zoom key the second weapon, input_settings.cpp).
  // Select a row, then press the key or mouse button for it.
  std::vector<SettingRow> KeyRows() {
    static const std::pair<const char*, const char*> kBinds[] = {
        {"keybind_lstick_up", "Move forward / throttle"},
        {"keybind_lstick_down", "Move back / brake"},
        {"keybind_lstick_left", "Move left / turn left"},
        {"keybind_lstick_right", "Move right / turn right"},
        {"keybind_right_trigger", "Fire"},
        {"keybind_left_trigger", "Zoom / second weapon"},
        {"keybind_a", "Jump / change seat"},
        {"keybind_b", "Enter, use, pick up / exit"},
        {"keybind_x", "Reload"},
        {"keybind_y", "Draw knife"},
        {"keybind_left_shoulder", "Grenade, explosives"},
        {"keybind_right_shoulder", "Toggle weapon / second weapon"},
        {"keybind_lstick_press", "Sprint"},
        {"keybind_rstick_press", "Crouch"},
        {"keybind_back", "Command (tap), score (hold)"},
        {"keybind_start", "Game menu"},
        {"keybind_dpad_up", "Vehicle camera (D-pad up)"},
        {"keybind_dpad_down", "D-pad down"},
        {"keybind_dpad_left", "D-pad left"},
        {"keybind_dpad_right", "D-pad right"},
        {"keybind_rstick_up", "Look up (mouse camera off)"},
        {"keybind_rstick_down", "Look down (mouse camera off)"},
        {"keybind_rstick_left", "Look left (mouse camera off)"},
        {"keybind_rstick_right", "Look right (mouse camera off)"},
        {"keybind_push_to_talk", "Push to talk (voice chat)"},
        {"keybind_chat_all", "Chat to everyone"},
        {"keybind_chat_team", "Chat to your team"},
        {"keybind_chat_squad", "Chat to your squad"}};
    // 13 Nov 2026 ("Mouse and Keyboard keybinds should be available for remapping any key (except mouse look) with a
    // primary and second key for both a keyboard column and a mouse column. Click on a bind and holding Escape
    // should clear that bind slot"): a binding is the SDK's list of keys, any of which presses it; here its first two
    // keyboard keys and its first two mouse buttons, each a slot of its own. The wheel's notches are mouse buttons
    // for the bindings that are a button (look.cpp WheelNotch); the first visit puts the wheel where it was, on Toggle
    // Weapon.
    if (!REXCVAR_GET(bf_wheel_bindable)) {
      bool named = false;
      for (const auto& [cvar, label] : kBinds) named = named || Cvar(cvar).find("Wheel") != std::string::npos;
      if (!named) {
        const std::string now = Cvar("keybind_right_shoulder");
        SetCvar("keybind_right_shoulder", now.empty() ? "WheelUp,WheelDown" : now + ",WheelUp,WheelDown");
      }
      SetCvar("bf_wheel_bindable", "true");
    }
    std::vector<SettingRow> rows;
    int index = 0;
    for (const auto& [cvar, label] : kBinds) {
      SettingRow r{"Keyboard and mouse bindings", label,
                   "Two keys and two mouse buttons for each action: any of them does it. Click a slot (or A on it), "
                   "then press the key or the mouse button; the mouse columns also take the side buttons and, for the "
                   "button actions, a wheel notch. Esc keeps what it has; hold Esc to clear the slot."};
      const std::string name = cvar;
      const bool chosen = index++ == row_;
      const std::array<std::string, 4> slots = BindSlots(Cvar(cvar));
      r.slots.assign(slots.begin(), slots.end());
      r.slot_cursor = chosen ? std::clamp(slot_cursor_, 0, 3) : -1;
      r.slot_capture = capture_ == cvar ? capture_slot_ : -1;
      r.slot_click = [this, name](int slot) { capture_ = name, capture_slot_ = slot_cursor_ = std::clamp(slot, 0, 3); };
      r.activate = [this, name] { capture_ = name, capture_slot_ = std::clamp(slot_cursor_, 0, 3); };
      r.nudge = [this](int by) { slot_cursor_ = std::clamp(slot_cursor_ + by, 0, 3); };
      // The older panel (the pause menu's) shows the slots as text, the chosen one in brackets.
      for (int s = 0; s < 4; ++s) {
        std::string shown = r.slot_capture == s ? "PRESS" : slots[size_t(s)].empty() ? "-" : slots[size_t(s)];
        if (chosen && r.slot_cursor == s) shown = "[" + shown + "]";
        r.value += (s == 0 ? "" : s == 2 ? "   |   " : "  ") + shown;
      }
      rows.push_back(r);
    }
    SettingRow reset{"Reset", "Reset bindings", "Every key back to its default."};
    reset.activate = [this] {
      for (const auto& [cvar, label] : kBinds)
        if (const auto* info = rex::cvar::GetFlagInfo(cvar)) SetCvar(cvar, info->default_value);
    };
    rows.push_back(reset);
    return rows;
  }

  // Enhancements (TekRantGaming, 13 Nov 2026: "I think we will have a lot of upcoming features like this so should
  // probably have them in a new settings category"): features of ours that change how the game looks or plays, each
  // with its own switch. Not a step of the first start's setup (game_menu_setup.inc has its own list).
  std::vector<SettingRow> EnhancementsRows() {
    std::vector<SettingRow> rows;
    rows.push_back(Toggle("Enhancements", "Lettered flags",
                          "A letter inside every capture point's flag: in the row under the ticket bars, on the markers in "
                          "the world, on the minimap, the deploy screen's map and the pause menu's map. The same point has "
                          "the same letter in every match. Off: the game's own plain flags.",
                          "bf_flag_letters"));
    // TekRantGaming, 13 Nov 2026: "add a modern sniper scope implementation ... off by default ... marked as
    // (experimental)", picture in picture (NativeXRenderer D168, fov.cpp); not in 0.4.1 (scope_pip.h).
    if (kScopePip)
      rows.push_back(Toggle("Enhancements", "Sniper scope (experimental)",
                            "Picture in picture: through a sniper scope only its lens zooms in, and you still see the "
                            "world around it, darkened. The world around the lens is the one you saw as the scope came "
                            "up, turned with your aim. Off: the game's own scope, which fills the screen.",
                            "bf_scope_pip"));
    return rows;
  }

  std::vector<SettingRow> GameplayRows() {
    std::vector<SettingRow> rows;
    SettingRow name{"Player", "Name", name_saved_.empty() ? "The name other players see in the game."
                                                          : "Saved: other players see it from your next start."};
    name.value = settings_.Get("player_name", "");
    name.activate = [this] { BeginName(nullptr); };
    rows.push_back(name);
    // The game's language (GitHub issue 2: "the game supports other localizations and not only English"): the
    // console's language setting as the runtime gives it (user_language, read by the game through
    // ExGetXConfigSetting's XCONFIG_USER_LANGUAGE at its start), the six the game has text for (build_overlay-00.fbrb
    // language_<name>_normal.bin). Run lang1: French, "APPUYER SUR B POUR ENTRER" on the HUD.
    rows.push_back(Choice("Player", "Game language",
                          "The language of the game's own text: its menus, HUD and loading screens. The menus of this "
                          "launcher stay in English. Applies at the next start.",
                          "user_language",
                          {{"English", "1"}, {"French", "4"}, {"German", "3"}, {"Italian", "6"}, {"Spanish", "5"},
                           {"Japanese", "2"}}));
    rows.push_back(Toggle("Player", "Text chat",
                          "Messages top left in a match: T to everyone, Y to your team, U to your squad (Keyboard "
                          "bindings). Off hides them and the chat box.",
                          "bf_text_chat"));
    // 13 Nov 2026: the HUD's size (the soldier's and the vehicles' HUD and the crosshair; NativeXRenderer scales each
    // part about the screen's corner, edge or centre it hangs on) and, apart from it, the minimap's.
    rows.push_back(Slider("HUD", "HUD scale",
                          "The size of the HUD and the crosshair on foot and in vehicles. 100% is the game's own. The "
                          "minimap has its own size below. Applies at once.",
                          "native_x_renderer_hud_scale", 50, 100, 5, Percent));
    rows.push_back(Choice("HUD", "Minimap size", "The minimap's size, apart from the HUD scale. Applies at once.",
                          "native_x_renderer_minimap_scale",
                          {{"Small", "75.000000"}, {"Medium", "100.000000"}, {"Large", "125.000000"}}));
    rows.push_back(Choice("Overlay", "Performance metrics",
                          "Readings in the top left corner while you play. F2 switches between them.",
                          "bf_performance_metrics", {{"Off", "off"}, {"FPS only", "fps"}, {"Full", "full"}}));
    rows.push_back(Toggle("Sharing", "Share graphics data",
                          "Uploads graphics pipeline state, shader hashes, a random install ID, the game version, "
                          "the renderer API and levels played. Requires a configured upload endpoint.",
                          "bf_share_pipelines"));
    rows.push_back(GameChoice("Sharing", "Telemetry",
                              "The game's own play reports, from its Sound / Image page. They went to EA's servers, "
                              "which are gone, so this changes nothing you can see.",
                              "GstGameplay", {"Telemetry"}, "1", {{"Off", "0"}, {"On", "1"}}));
    return rows;
  }

  // A key or mouse button for the Keyboard bindings, in the SDK's names (keybind_* values, sdk ui/keybinds.cpp), from
  // ImGui's presses. Esc cancels (SettingsPanel); F2 is the metrics key.
  // 13 Nov 2026: `mouse` true gives only a mouse button (the side buttons too; a wheel notch where `wheel`), false
  // only a key: the bindings' slots are of one kind each.
  static std::string CapturedKey(bool mouse, bool wheel) {
    if (mouse) {
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return "LMB";
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) return "RMB";
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) return "MMB";
      // The side buttons by the names issue 14 gave them (the prompts' pictures, glyphs/keyboard/MB4.png and MB5.png).
      if (ImGui::IsMouseClicked(3)) return "MB4";
      if (ImGui::IsMouseClicked(4)) return "MB5";
      if (wheel && ImGui::GetIO().MouseWheel > 0) return "WheelUp";
      if (wheel && ImGui::GetIO().MouseWheel < 0) return "WheelDown";
      return {};
    }
    for (int k = ImGuiKey_A; k <= ImGuiKey_Z; ++k)
      if (ImGui::IsKeyPressed(ImGuiKey(k))) return std::string(1, char('A' + (k - ImGuiKey_A)));
    for (int k = ImGuiKey_0; k <= ImGuiKey_9; ++k)
      if (ImGui::IsKeyPressed(ImGuiKey(k))) return std::string(1, char('0' + (k - ImGuiKey_0)));
    for (int k = ImGuiKey_Keypad0; k <= ImGuiKey_Keypad9; ++k)
      if (ImGui::IsKeyPressed(ImGuiKey(k))) return "Numpad" + std::string(1, char('0' + (k - ImGuiKey_Keypad0)));
    static const std::pair<ImGuiKey, const char*> kNamed[] = {
        {ImGuiKey_Space, "Space"},        {ImGuiKey_Enter, "Return"},       {ImGuiKey_Backspace, "Backspace"},
        {ImGuiKey_Tab, "Tab"},            {ImGuiKey_UpArrow, "Up"},         {ImGuiKey_DownArrow, "Down"},
        {ImGuiKey_LeftArrow, "Left"},     {ImGuiKey_RightArrow, "Right"},   {ImGuiKey_Semicolon, "Semicolon"},
        {ImGuiKey_Apostrophe, "Quote"},   {ImGuiKey_LeftShift, "Shift"},    {ImGuiKey_RightShift, "Shift"},
        {ImGuiKey_LeftCtrl, "Control"},   {ImGuiKey_RightCtrl, "Control"},  {ImGuiKey_LeftAlt, "Alt"},
        {ImGuiKey_RightAlt, "Alt"},       {ImGuiKey_CapsLock, "CapsLock"},  {ImGuiKey_GraveAccent, "Backtick"},
        {ImGuiKey_Minus, "Minus"},        {ImGuiKey_Equal, "Plus"},         {ImGuiKey_Comma, "Comma"},
        {ImGuiKey_Period, "Period"},      {ImGuiKey_Slash, "Slash"},        {ImGuiKey_Backslash, "Backslash"},
        {ImGuiKey_LeftBracket, "LBracket"}, {ImGuiKey_RightBracket, "RBracket"}, {ImGuiKey_Delete, "Delete"},
        {ImGuiKey_Insert, "Insert"},      {ImGuiKey_Home, "Home"},          {ImGuiKey_End, "End"},
        {ImGuiKey_PageUp, "PageUp"},      {ImGuiKey_PageDown, "PageDown"},  {ImGuiKey_KeypadEnter, "NumpadEnter"},
        {ImGuiKey_KeypadAdd, "NumpadPlus"}, {ImGuiKey_KeypadSubtract, "NumpadMinus"},
        {ImGuiKey_KeypadMultiply, "NumpadStar"}, {ImGuiKey_KeypadDivide, "NumpadSlash"},
        {ImGuiKey_F1, "F1"},  {ImGuiKey_F3, "F3"},  {ImGuiKey_F4, "F4"},  {ImGuiKey_F5, "F5"},  {ImGuiKey_F6, "F6"},
        {ImGuiKey_F7, "F7"},  {ImGuiKey_F8, "F8"},  {ImGuiKey_F9, "F9"},  {ImGuiKey_F10, "F10"}, {ImGuiKey_F11, "F11"},
        {ImGuiKey_F12, "F12"}};
    for (const auto& [k, name] : kNamed)
      if (ImGui::IsKeyPressed(k)) return name;
    return {};
  }

  // A binding's keys as its four slots: its first two keyboard keys, then its first two mouse buttons.
  static bool MouseKeyName(const std::string& k) {
    return k == "LMB" || k == "RMB" || k == "MMB" || k == "MB4" || k == "MB5" || k == "Mouse4" || k == "Mouse5" ||
           k == "WheelUp" || k == "WheelDown";
  }
  static std::array<std::string, 4> BindSlots(const std::string& value) {
    std::array<std::string, 4> slots;
    int keys = 0, buttons = 0;
    for (size_t at = 0; at <= value.size();) {
      size_t comma = value.find(',', at);
      if (comma == std::string::npos) comma = value.size();
      std::string k = value.substr(at, comma - at);
      at = comma + 1;
      while (!k.empty() && k.front() == ' ') k.erase(k.begin());
      while (!k.empty() && k.back() == ' ') k.pop_back();
      if (k.empty()) continue;
      if (MouseKeyName(k)) {
        if (buttons < 2) slots[size_t(2 + buttons++)] = k;
      } else if (keys < 2) {
        slots[size_t(keys++)] = k;
      }
    }
    return slots;
  }
  // A slot's key set (empty: cleared); the same key in the binding's other slot of its kind goes.
  void StoreBindSlot(const std::string& cvar, int slot, const std::string& key) {
    std::array<std::string, 4> slots = BindSlots(Cvar(cvar));
    slot = std::clamp(slot, 0, 3);
    const int other = slot ^ 1;
    if (!key.empty() && slots[size_t(other)] == key) slots[size_t(other)].clear();
    slots[size_t(slot)] = key;
    std::string joined;
    for (const std::string& s : slots)
      if (!s.empty()) joined += (joined.empty() ? "" : ",") + s;
    REXLOG_INFO("BF bindings: {} slot {} -> {} (now \"{}\")", cvar, slot + 1, key.empty() ? "cleared" : key, joined);
    SetCvar(cvar, joined);
  }
  // The binding waiting for a press (capture_, capture_slot_): the next key or mouse button of its slot's kind goes
  // in. Esc let go at once keeps what the slot has; Esc held for 0.6 s clears it. True while it waits (the page gets
  // no input meanwhile).
  bool CaptureBind() {
    if (capture_.empty()) return false;
    const bool mouse = capture_slot_ >= 2;
    // A wheel notch only for the bindings that are a button (the wheel cannot hold a trigger or a direction).
    const bool wheel = capture_.find("trigger") == std::string::npos && capture_.find("stick_up") == std::string::npos &&
                       capture_.find("stick_down") == std::string::npos && capture_.find("stick_left") == std::string::npos &&
                       capture_.find("stick_right") == std::string::npos && capture_.find("chat") == std::string::npos &&
                       capture_.find("talk") == std::string::npos;
    if (ImGui::IsKeyDown(ImGuiKey_Escape)) {
      if (esc_held_since_ < 0) esc_held_since_ = ImGui::GetTime();
      if (ImGui::GetTime() - esc_held_since_ >= 0.6) {
        StoreBindSlot(capture_, capture_slot_, "");
        capture_.clear();
        esc_held_since_ = -1;
        esc_swallow_ = true;  // the rest of this hold is not a Back
      }
    } else if (esc_held_since_ >= 0) {  // let go before that: the slot keeps what it has
      esc_held_since_ = -1;
      capture_.clear();
    } else if (const std::string key = CapturedKey(mouse, wheel); !key.empty()) {
      StoreBindSlot(capture_, capture_slot_, key);
      capture_.clear();
    }
    return true;
  }

  // The settings from the main menu: BF's dialog over its scene.
  void SettingsPage(ImDrawList* d, float s, ImGuiIO& io, const Input& in) {
    const ImVec2 sz = io.DisplaySize;
    if (from_pause_) {  // the pause shows the panel in its own place
      page_ = Page::kPause, pause_focus_panel_ = true;
      return;
    }
    // The new look (docs/MENUS-PLAN.md section 6): the maps from the air, darkened, never the game's own menu.
    MainBackdrop(d, sz);
    d->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(12, 14, 11, 186));
    {
      const kit::Fonts& kf = kit::GetFonts();
      kit::Spaced(d, kf.typed, 16 * s, ImVec2(sz.x * 0.14f, sz.y * 0.035f), kit::kKhakiDim, "PACIFIC THEATRE, 1943", 4 * s);
      Text(d, kf.stencil, 54 * s, ImVec2(sz.x * 0.14f - 3 * s, sz.y * 0.052f), kit::kCream, "SETTINGS");
    }
    if (SettingsPanel(d, ImVec2(sz.x * 0.14f, sz.y * 0.13f), ImVec2(sz.x * 0.86f, sz.y * 0.80f), in, true))
      page_ = Page::kMain, row_ = MainRowOf("PC SETTINGS");
    BfTags(d, io, {{"LB RB", "Section"}, {"B", "Back"}});
    if (RestartPending()) {  // the rows marked NEXT START (SERVERS-PLAN.md section 3)
      const std::string t = "SETTINGS MARKED NEXT START TAKE EFFECT WHEN THE GAME STARTS AGAIN";
      Text(d, g_fonts.bf_med, 22 * s, ImVec2(sz.x * 0.86f - Width(g_fonts.bf_med, 22 * s, t), sz.y * 0.815f),
           IM_COL32(236, 235, 230, 255), t);
    }
  }

  // The settings in BF's dialog: the tabs in the black title bar (LB and RB), the rows on the paper, the selected one
  // green, and the selected row's help at the foot. With focused false it is drawn without a selection (the pause's
  // preview while its list has the focus). True when B leaves it.
  bool SettingsPanel(ImDrawList* d, ImVec2 a, ImVec2 b, const Input& in_raw, bool focused) {
    const float s = io_scale_;
    Input in = focused ? in_raw : Input{};
    const float bar = 52 * s;
    BfDialog(d, a, b, bar);
    // Keyboard bindings: the next key goes to the row being bound.
    if (focused && CaptureBind()) in = {};
    if (sub_ == kSubNone) {
      if (in.prev_tab) tab_ = (tab_ + kTabCount - 1) % kTabCount, row_ = 0, scroll_ = 0;
      if (in.next_tab) tab_ = (tab_ + 1) % kTabCount, row_ = 0, scroll_ = 0;
      // The tabs, spread over the bar; a click switches.
      const float tab_size = std::min(28 * s, (b.x - a.x) / 26.0f);
      float total = 0;
      for (int t = 0; t < kTabCount; ++t) total += Width(g_fonts.bf, tab_size, kTabNames[t]);
      const float gap = std::max(18 * s, ((b.x - a.x) - 40 * s - total) / float(kTabCount));
      float tx = a.x + 20 * s + gap * 0.5f;
      for (int t = 0; t < kTabCount; ++t) {
        const bool cur = t == tab_;
        const float tw = Width(g_fonts.bf, tab_size, kTabNames[t]);
        Text(d, g_fonts.bf, tab_size, ImVec2(tx, CapTop(a.y, bar, tab_size)),
             cur ? IM_COL32(242, 242, 238, 255) : IM_COL32(118, 118, 116, 255), kTabNames[t]);
        if (cur) d->AddRectFilled(ImVec2(tx, a.y + bar - 7 * s), ImVec2(tx + tw, a.y + bar - 3 * s), kBfGreen);
        if (ImGui::IsMouseHoveringRect(ImVec2(tx, a.y), ImVec2(tx + tw, a.y + bar)) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left))
          tab_ = t, row_ = 0, scroll_ = 0, sub_ = kSubNone;
        tx += tw + gap;
      }
    } else {
      const std::string t = sub_ == kSubRemap ? "CONTROLS  /  BUTTON REMAPPING" : "CONTROLS  /  KEYBOARD BINDINGS";
      Text(d, g_fonts.bf, 28 * s, ImVec2((a.x + b.x - Width(g_fonts.bf, 28 * s, t)) * 0.5f, CapTop(a.y, bar, 28 * s)),
           IM_COL32(242, 242, 238, 255), t);
    }
    const std::vector<SettingRow> rows = RowsOf(tab_);
    const int count = int(rows.size());
    if (in.up) row_ = (row_ + count - 1) % count;
    if (in.down) row_ = (row_ + 1) % count;
    row_ = std::clamp(row_, 0, count - 1);
    // The help at the foot of the paper; the rows scroll above it so the selected one stays in view.
    const float pad = 18 * s, help_h = std::min(150 * s, (b.y - a.y) * 0.26f);
    const float top = a.y + bar + 10 * s, bottom = b.y - help_h, rh = 38 * s, sh = 30 * s;
    std::vector<float> ys(static_cast<size_t>(count));
    {
      float y = 0;
      std::string section;
      for (int i = 0; i < count; ++i) {
        if (rows[i].section != section) section = rows[i].section, y += sh;
        ys[size_t(i)] = y;
        y += rh;
      }
    }
    if (ys[size_t(row_)] - scroll_ < sh) scroll_ = std::max(0.0f, ys[size_t(row_)] - sh);
    if (ys[size_t(row_)] + rh - scroll_ > bottom - top) scroll_ = ys[size_t(row_)] + rh - (bottom - top);
    d->PushClipRect(ImVec2(a.x, top), ImVec2(b.x, bottom), true);
    std::string section;
    const float x0 = a.x + pad, x1 = b.x - pad, name_size = 24 * s;
    for (int i = 0; i < count; ++i) {
      const SettingRow& it = rows[i];
      const float y = top + ys[size_t(i)] - scroll_;
      if (it.section != section) {
        section = it.section;
        const std::string head = Upper(section);
        Text(d, g_fonts.bf, 19 * s, ImVec2(x0, CapTop(y - sh, sh, 19 * s)), kBfFaint, head);
        const float hx = x0 + Width(g_fonts.bf, 19 * s, head) + 10 * s;
        d->AddLine(ImVec2(hx, y - sh * 0.5f), ImVec2(x1, y - sh * 0.5f), IM_COL32(176, 174, 168, 255), 1.0f);
      }
      if (y + rh < top || y > bottom) continue;
      const ImVec2 r0(x0, y), r1(x1, y + rh);
      const ImGuiIO& io = ImGui::GetIO();
      const bool hovered = focused && ImGui::IsMouseHoveringRect(
          ImVec2(x0, std::max(y, top)), ImVec2(x1, std::min(y + rh, bottom)));
      const bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
      if (hovered && (clicked || io.MouseDelta.y)) row_ = i;
      const bool sel = focused && i == row_;
      if (sel) d->AddRectFilled(r0, r1, kBfGreen);
      const ImU32 ink = it.disabled ? (sel ? IM_COL32(170, 176, 150, 255) : kBfFaint) : sel ? kBfGreenInk : kBfInk;
      Text(d, g_fonts.bf_med, name_size, ImVec2(x0 + 12 * s, CapTop(y, rh, name_size)), ink, Upper(it.name));
      std::string shown = it.slider ? (it.format ? it.format(it.val) : std::to_string(it.val))
                          : it.options.empty() ? it.value
                                               : it.options[size_t(std::clamp(it.index, 0, int(it.options.size()) - 1))];
      shown = Upper(shown);
      const float vs = 23 * s, w = Width(g_fonts.bf_med, vs, shown);
      const bool changeable = !it.disabled && (it.choose || it.slider);
      const float vx = x1 - 34 * s - w;
      Text(d, g_fonts.bf_med, vs, ImVec2(vx, CapTop(y, rh, vs)), sel ? kBfGreenInk : changeable ? kBfInk : kBfFaint, shown);
      if (Pending(it)) {  // changed, taken at the next start
        const float ts = 16 * s, tw = Width(g_fonts.bf_med, ts, "NEXT START");
        Text(d, g_fonts.bf_med, ts, ImVec2(vx - (sel ? 44 : 22) * s - tw, CapTop(y, rh, ts)),
             sel ? kBfGreenInk : IM_COL32(176, 52, 40, 255), "NEXT START");
      }
      const float t0 = x0 + (x1 - x0) * 0.52f, t1 = vx - 26 * s;
      if (it.slider) {  // the slider's track left of its value
        const float ty = y + rh * 0.5f;
        if (t1 > t0) {
          const float f = (it.val - it.lo) / std::max(1e-6f, it.hi - it.lo), fx = t0 + (t1 - t0) * f;
          d->AddRectFilled(ImVec2(t0, ty - 1.5f * s), ImVec2(t1, ty + 1.5f * s),
                           sel ? IM_COL32(150, 170, 120, 255) : IM_COL32(190, 188, 182, 255));
          d->AddRectFilled(ImVec2(t0, ty - 1.5f * s), ImVec2(fx, ty + 1.5f * s), sel ? kBfGreenInk : it.disabled ? kBfFaint : kBfInk);
          d->AddRectFilled(ImVec2(fx - 3 * s, ty - 9 * s), ImVec2(fx + 3 * s, ty + 9 * s), sel ? kBfGreenInk : kBfInk);
        }
      }
      if (sel && changeable) {
        Text(d, g_fonts.bf_med, vs, ImVec2(vx - 20 * s, CapTop(y, rh, vs)), kBfGreenInk, "<");
        Text(d, g_fonts.bf_med, vs, ImVec2(x1 - 22 * s, CapTop(y, rh, vs)), kBfGreenInk, ">");
      }
      if (!sel || it.disabled) continue;
      // Each change logged, for the player's log and the tests.
      if (it.nudge && (in.left || in.right)) it.nudge(in.left ? -1 : 1);
      if (it.activate && (in.accept || clicked)) {
        REXLOG_INFO("BF settings: {} / {} opened", kTabNames[tab_], it.name);
        it.activate();
      } else if (it.slider) {
        const float mouse_x = io.MousePos.x;
        const bool track_click = clicked && t1 > t0 && mouse_x >= t0 && mouse_x <= t1;
        const bool left = in.left || (clicked && mouse_x >= vx - 24 * s && mouse_x < vx);
        const bool right = in.right || (clicked && mouse_x >= x1 - 26 * s);
        if (!track_click && !left && !right) continue;
        const float target = track_click
            ? std::lerp(it.lo, it.hi, (mouse_x - t0) / (t1 - t0))
            : it.val + (left ? -it.step : it.step);
        const float v = std::clamp(
            track_click ? it.lo + std::round((target - it.lo) / it.step) * it.step : target, it.lo, it.hi);
        REXLOG_INFO("BF settings: {} / {} -> {}", kTabNames[tab_], it.name, it.format ? it.format(v) : fmt::format("{}", v));
        it.set(v);
      } else if (it.choose && !it.options.empty() && (in.left || in.right || in.accept || clicked)) {
        const int n = int(it.options.size()), next = (std::clamp(it.index, 0, n - 1) + (in.left ? n - 1 : 1)) % n;
        REXLOG_INFO("BF settings: {} / {} -> {}", kTabNames[tab_], it.name, it.options[size_t(next)]);
        it.choose(next);
      }
    }
    d->PopClipRect();
    // The selected row's help.
    d->AddLine(ImVec2(x0, bottom + 2 * s), ImVec2(x1, bottom + 2 * s), IM_COL32(60, 60, 58, 255), 1.5f * s);
    const SettingRow& it = rows[size_t(row_)];
    if (focused) {
      Text(d, g_fonts.bf, 24 * s, ImVec2(x0, bottom + 10 * s), it.disabled ? kBfFaint : kBfInk, Upper(it.name));
      d->AddText(g_fonts.body, 19 * s, ImVec2(x0, bottom + 46 * s), IM_COL32(40, 40, 38, 255), it.help.c_str(), nullptr,
                 x1 - x0);
      if (!it.note.empty()) {
        const float hh = g_fonts.body->CalcTextSizeA(19 * s, FLT_MAX, x1 - x0, it.help.c_str()).y;
        d->AddText(g_fonts.body, 17 * s, ImVec2(x0, bottom + 50 * s + hh), kBfRed, it.note.c_str(), nullptr, x1 - x0);
      }
    } else {
      Text(d, g_fonts.bf_med, 21 * s, ImVec2(x0, bottom + 14 * s), kBfFaint, "PRESS A TO CHANGE THE SETTINGS");
    }
    if (!focused || !in.back) return false;
    if (sub_ != kSubNone) {
      const char* back_to = sub_ == kSubRemap ? "Button remapping" : "Keyboard bindings";
      sub_ = kSubNone;
      const std::vector<SettingRow> page = ControlsRows();
      row_ = 0;
      for (size_t i = 0; i < page.size(); ++i)
        if (page[i].name == back_to) row_ = int(i);
      return false;
    }
    return true;
  }

  // In a match: our menu in place of BF's "Frontline Report". The match keeps running; the soldier stands still
  // while it is open (the game gets an empty pad).
  // Dev test (BF_PAUSE_TEST=<spawn s>,<pause s>): in a match, A three times at <spawn s> (Loading complete, spawn),
  // then at <pause s> our pause menu and Leave match.
  // BF_MENU_STEPS (tests): our menu's input at set times, "<s from start>:<what>" separated by commas: up, down,
  // left, right, a, b, lb, rb, and bfb, bfa, bfup, bfdown, bfleft, bfright, bflb, bfrb (the player's press on BF's own
  // screen); in dev builds set/<setting>/<value> (below). The dev pad's presses go to
  // the game, not to our menu.
  void MenuSteps() {
    static const std::vector<std::pair<double, std::string>> steps = [] {
      std::vector<std::pair<double, std::string>> out;
      const char* v = std::getenv("BF_MENU_STEPS");
      const std::string list = v ? v : "";
      for (size_t at = 0; at < list.size();) {
        const size_t end = std::min(list.find(',', at), list.size());
        const std::string item = list.substr(at, end - at);
        if (const size_t colon = item.find(':'); colon != std::string::npos)
          out.push_back({std::atof(item.substr(0, colon).c_str()), item.substr(colon + 1)});
        at = end + 1;
      }
      return out;
    }();
    static size_t next = 0;
    if (release_click_) ImGui::GetIO().AddMouseButtonEvent(0, false), release_click_ = false;
    while (next < steps.size() && ImGui::GetTime() > steps[next].first) {
      const std::string& w = steps[next++].second;
      REXLOG_INFO("BF menu steps: {}", w);
      std::lock_guard<std::mutex> lock(pad_mutex_);
      // A controller's press for the prompts (the keyboard's, typing and the mouse's steps are theirs).
      if (w.find('/') == std::string::npos && w.rfind("bf", 0) != 0) pad_pressed_ = true;
      if (w == "up") pad_.up = true;
      else if (w == "down") pad_.down = true;
      else if (w == "left") pad_.left = true;
      else if (w == "right") pad_.right = true;
      else if (w == "a") pad_.accept = true;
      else if (w == "b") pad_.back = true;
      else if (w == "lb") pad_.prev_tab = true;
      else if (w == "rb") pad_.next_tab = true;
      else if (w == "x") pad_.x = true;
      else if (w == "y") pad_.y = true;
      else if (w == "start") pad_.start = true;
      else if (w == "view") pad_.view = true;
      // key/<name>: the keyboard's press (enter, esc, up, down, left, right, tab, f, r, p, q, e); type/<text>: typed;
      // move/<x>/<y> and click/<x>/<y>: the mouse at the 1280 x 720 stage's point; mdown/<x>/<y> presses the left
      // button there and keeps it down, mup/<x>/<y> lets it go there (a drag, with move steps between).
      else if (w.rfind("key/", 0) == 0) {
        const std::string k = w.substr(4);
        Input& t = test_key_;
        if (k == "enter") t.accept = true;
        else if (k == "esc") t.back = true;
        else if (k == "up") t.up = true;
        else if (k == "down") t.down = true;
        else if (k == "left") t.left = true;
        else if (k == "right") t.right = true;
        else if (k == "tab") t.view = true;
        else if (k == "f") t.x = true;
        else if (k == "r") t.y = true;
        else if (k == "p") t.start = true;
        else if (k == "q") t.prev_tab = true;
        else if (k == "e") t.next_tab = true;
      } else if (w.rfind("press/", 0) == 0 || w.rfind("hold/", 0) == 0 || w.rfind("release/", 0) == 0) {
        // press/<key>, hold/<key>, release/<key>: f6, f7, f8 and the arrows as ImGui sees them (the graphics panel)
        const std::string k = w.substr(w.find('/') + 1);
        // A single letter (the chat's T, Y, U and push to talk's B), enter, esc, tab, backspace: the chat box tests.
        const ImGuiKey key = k == "f8" ? ImGuiKey_F8 : k == "f7" ? ImGuiKey_F7 : k == "f6" ? ImGuiKey_F6
                             : k == "right" ? ImGuiKey_RightArrow : k == "left" ? ImGuiKey_LeftArrow
                             : k == "down" ? ImGuiKey_DownArrow : k == "up" ? ImGuiKey_UpArrow
                             : k == "enter" ? ImGuiKey_Enter : k == "esc" ? ImGuiKey_Escape : k == "tab" ? ImGuiKey_Tab
                             : k == "backspace" ? ImGuiKey_Backspace
                             : k.size() == 1 && k[0] >= 'a' && k[0] <= 'z' ? ImGuiKey(ImGuiKey_A + (k[0] - 'a'))
                                                                           : ImGuiKey_None;
        if (key != ImGuiKey_None) {
          if (w[0] != 'r') ImGui::GetIO().AddKeyEvent(key, true);
          if (w[0] != 'h') ImGui::GetIO().AddKeyEvent(key, false);
        }
      } else if (w.rfind("type/", 0) == 0) {
        ImGui::GetIO().AddInputCharactersUTF8(w.substr(5).c_str());
      } else if (w.rfind("move/", 0) == 0 || w.rfind("click/", 0) == 0 || w.rfind("mdown/", 0) == 0 || w.rfind("mup/", 0) == 0) {
        const bool click = w[0] == 'c', down = w.rfind("mdown/", 0) == 0, up = w.rfind("mup/", 0) == 0;
        const size_t a = w.find('/'), b = w.find('/', a + 1);
        const float x = float(std::atof(w.substr(a + 1, b - a - 1).c_str())), y = float(std::atof(w.substr(b + 1).c_str()));
        const Stage st = StageOf(ImGui::GetIO());
        const ImVec2 at = st.P(x, y);
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(at.x - 2, at.y);  // two moves: the hover follows the mouse (it moved on two frames)
        io.AddMousePosEvent(at.x, at.y);
        script_mouse_ = ImVec2(at.x - 2, at.y), script_mouse_to_ = at;  // a scripted test run's mouse (OnDraw)
        if (click) io.AddMouseButtonEvent(0, true), release_click_ = true;
        if (down) io.AddMouseButtonEvent(0, true);
        if (up) io.AddMouseButtonEvent(0, false);
      }
#if defined(BF_DEV_TOOLS)
      // set/<setting>/<value>: a runtime setting changed as our menus change it, not saved (a key or the prompts'
      // style changed in a match: hud_glyphs.cpp draws the prompts again).
      else if (w.rfind("set/", 0) == 0 && w.find('/', 4) != std::string::npos) {
        const size_t slash = w.find('/', 4);
        rex::cvar::SetFlagByName(w.substr(4, slash - 4), w.substr(slash + 1));
      }
      // cmd/<name>/<args>: one of the game's own menu commands (selectTeam, leaveSquad...), as its menu sends them.
      else if (w.rfind("cmd/", 0) == 0) {
        const size_t slash = w.find('/', 4);
        GameCommand(w.substr(4, slash == std::string::npos ? std::string::npos : slash - 4),
                    slash == std::string::npos ? "" : w.substr(slash + 1));
      }
#endif
      else if (w == "bfb") PressBf({kPadB}), Passing(kPadB);  // the player's B on BF's own screen
      else if (w == "bfa") PressBf({kPadA}), Passing(kPadA);
      else if (w == "bfdown") PressBf({kPadDown}), Passing(kPadDown);  // BF's own lists (its Help & Options)
      else if (w == "bfup") PressBf({kPadUp}), Passing(kPadUp);
      else if (w == "bfright") PressBf({0x0008}), Passing(0x0008);
      else if (w == "bfleft") PressBf({0x0004}), Passing(0x0004);
      else if (w == "bfrb") PressBf({0x0200}), Passing(0x0200);
      else if (w == "bflb") PressBf({0x0100}), Passing(0x0100);
    }
  }

  void PauseTest() {
#if defined(BF_DEV_TOOLS)
    SideButtonTick();
#endif
    static const std::pair<double, double> at = [] {
      const char* v = std::getenv("BF_PAUSE_TEST");
      if (!v || !*v) return std::pair<double, double>{-1, -1};
      return std::pair<double, double>{std::atof(v), std::atof(std::strchr(v, ',') ? std::strchr(v, ',') + 1 : v)};
    }();
    if (at.first < 0) return;
    static double in_match = -1;
    static int step = 0;
    const double now = ImGui::GetTime();
    if (MenuDriverStage() != 5 || (in_match < 0 && !LevelLoaded())) return;  // from Loading complete
    if (in_match < 0) in_match = now;
    // BF_PAUSE_BARE=1: the leave walk with our menu closed, so screenshots show BF's own menu.
    static const bool bare = [] {
      const char* v = std::getenv("BF_PAUSE_BARE");
      return v && *v == '1';
    }();
    // BF_PAUSE_STEPS (with BF_PAUSE_TEST): what happens after the pause opens, "<s after it>:<what>" separated by
    // commas: row<n> (the list's row), panel (PC settings gets the focus), down, tab, back, select (A), leave,
    // bfb and bfa (the player's B or A on BF's own screen: pressed to BF, and seen as the real pad's).
    // Without it: Exit game 2 s after the pause opens, as before.
    static const std::vector<std::pair<double, std::string>> steps = [] {
      std::vector<std::pair<double, std::string>> out;
      const char* v = std::getenv("BF_PAUSE_STEPS");
      std::string list = v ? v : "2:leave,3:yes";
      for (size_t at = 0; at < list.size();) {
        const size_t end = std::min(list.find(',', at), list.size());
        const std::string item = list.substr(at, end - at);
        if (const size_t colon = item.find(':'); colon != std::string::npos)
          out.push_back({std::atof(item.substr(0, colon).c_str()), item.substr(colon + 1)});
        at = end + 1;
      }
      return out;
    }();
    static size_t next = 0;
    if (bare && step == 1 && now - in_match > at.second) LeaveMatch(), step = 3;
    if (step == 0 && now - in_match > at.first) RunPauseAction(3), step = 1;
    else if (step == 1 && now - in_match > at.second) ShowPause(), step = 2;  // ours only: the game's never opens
    else if (step == 2 && next < steps.size() && now - in_match > at.second + steps[next].first) {
      const std::string& what = steps[next++].second;
      REXLOG_INFO("BF pause test: {}", what);
      std::lock_guard<std::mutex> lock(pad_mutex_);
      if (what.rfind("row", 0) == 0) pause_row_ = std::atoi(what.c_str() + 3);
      else if (what == "panel") pause_focus_panel_ = true, row_ = 0, scroll_ = 0;
      // tabto<n>: the settings panel on that tab; prow=<name>: on that row of it.
      else if (what.rfind("tabto", 0) == 0) tab_ = std::clamp(std::atoi(what.c_str() + 5), 0, int(kTabCount) - 1), row_ = 0, scroll_ = 0;
      else if (what.rfind("prow=", 0) == 0) {
        const std::vector<SettingRow> rows = RowsOf(tab_);
        for (size_t i = 0; i < rows.size(); ++i)
          if (rows[i].name == what.substr(5)) row_ = int(i);
      }
      else if (what == "down") pad_.down = true;
      else if (what == "tab") pad_.next_tab = true;
      else if (what == "back") pad_.back = true;
      else if (what == "select") pad_.accept = true;
      else if (what == "right") pad_.right = true;
      else if (what == "left") pad_.left = true;
      else if (what == "leave") pause_row_ = kPauseExit, pad_.accept = true;  // its question next: "yes"
      else if (what == "yes") pad_.accept = true;
      else if (what == "y") pad_.y = true;  // Y: our private match card copies its address
      else if (what == "x") pad_.x = true;  // X: the map's zoom
      else if (what == "suicide") pause_row_ = kPauseSuicide, pad_.accept = true;
      else if (what == "bfb") PressBf({kPadB}), Passing(kPadB);  // the player's B on BF's own screen
      else if (what == "bfa") PressBf({kPadA}), Passing(kPadA);
      else if (what == "bfdown") PressBf({kPadDown}), Passing(kPadDown);
      else if (what == "bfup") PressBf({kPadUp}), Passing(kPadUp);
      else if (what == "bfright") PressBf({0x0008}), Passing(0x0008);
      else if (what == "bfy") PressBf({0x8000}), Passing(0x8000);
      else if (what == "bfstart") open_ = false, PressBf({kPadStart});  // ours closed, the game's own pause (traces)
#if defined(BF_DEV_TOOLS)
      // sdlmb4, sdlmb5: a mouse side button pressed and let go (issue 14).
      else if (what == "sdlmb4" || what == "sdlmb5") PushSideButton(what == "sdlmb4" ? SDL_BUTTON_X1 : SDL_BUTTON_X2, true);
#endif
    }
  }

#if defined(BF_DEV_TOOLS)
  // Tests: a mouse side button through the window's own SDL event queue, as a real one arrives (SDL, the window, the
  // menu layer and the keyboard and mouse driver), never through the PC's own input (issue 14, 13 Nov 2026). Let go
  // 0.15 s later (SideButtonTick).
  uint8_t side_button_up_ = 0;
  double side_button_up_at_ = 0;
  void PushSideButton(uint8_t button, bool down) {
    auto* w = window_ ? static_cast<rex::ui::WindowSDL*>(window_()) : nullptr;
    SDL_Window* sdl = w ? w->sdl_window() : nullptr;
    if (!sdl) {
      REXLOG_WARN("BF test: no window for mouse button {}", int(button));
      return;
    }
    SDL_Event e{};
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = SDL_GetWindowID(sdl);
    e.button.button = button;
    e.button.down = down;
    e.button.clicks = 1;
    const bool ok = SDL_PushEvent(&e);
    REXLOG_INFO("BF test: mouse button {} {} through the window's queue{}", int(button), down ? "down" : "up",
                ok ? "" : " (refused)");
    if (down) side_button_up_ = button, side_button_up_at_ = ImGui::GetTime() + 0.15;
  }
  void SideButtonTick() {
    if (side_button_up_ && ImGui::GetTime() >= side_button_up_at_) {
      const uint8_t b = side_button_up_;
      side_button_up_ = 0;
      PushSideButton(b, false);
    }
  }
#endif

  // Our pause, in place of the game's (TekRantGaming, 10 Nov 2026: a 100% replacement whose rows do what they say, by the
  // game's own commands; docs/MENUS-PLAN.md). The game's own pause is never opened: Start reaches ours only
  // (auto_join.cpp), the match goes on behind it darkened (multiplayer), and the game reads an empty pad meanwhile.
  // Exit Game and Suicide ask first, as the game's did, then send doExitLevel and suicide.
  // 13 Nov 2026 ("I don't see a way to change teams in-game"): Change Team, by the game's own selectTeam (the
  // console's game had no such row; a developer's test here has used the command since the comms work). The switch
  // leaves the soldier where it stands, so the game's suicide follows two seconds later and the player spawns at the
  // new team's base.
  static constexpr const char* kPauseRows[] = {"RESUME",   "MY SQUAD",     "CHANGE TEAM",  "SCOREBOARD", "SETTINGS",
                                               "CONTROLS", "LEADERBOARDS", "ACHIEVEMENTS", "EXIT GAME",  "SUICIDE"};
  static constexpr int kPauseResume = 0, kPauseSquad = 1, kPauseTeam = 2, kPauseScoreboard = 3, kPauseSettingsRow = 4,
                       kPauseControls = 5, kPauseLeaderboards = 6, kPauseAchievements = 7, kPauseExit = 8,
                       kPauseSuicide = 9;
  double team_suicide_at_ = -1;  // after a change of team: the suicide that sends the soldier to its new base
  void ChangeTeamNow() {
    const int mine = comms::Team();
    if (mine != 1 && mine != 2) {
      REXLOG_WARN("BF pause: Change Team, but the player's team is not known yet");
      menu_error_ = "Your team is not known yet. Try again once you have spawned.", menu_error_at_ = ImGui::GetTime();
      return;
    }
    const int other = 3 - mine;
    REXLOG_INFO("BF pause: Change Team, from team {} to {} (the game's own selectTeam)", mine, other);
    GameCommand("selectTeam", std::to_string(other));
    team_suicide_at_ = ImGui::GetTime() + 2.0, team_wanted_ = other;
  }
  int team_wanted_ = 0;  // the team Change Team asked for
  bool ach_from_pause_ = false;  // the achievements opened from the pause: B goes back there
  int pause_confirm_ = -1;  // the row whose question is up (Exit Game, Suicide), else -1

  // Our pause, or a page opened from it (its Scoreboard, Controls, Leaderboards and Achievements).
  bool InPause() const {
    return page_ == Page::kPause || page_ == Page::kScoreboard || (page_ == Page::kControls && ctl_from_pause_) ||
           (page_ == Page::kLeaderboards && lb_from_pause_) || (page_ == Page::kAchievements && ach_from_pause_) ||
           (page_ == Page::kSettings && from_pause_) || (page_ == Page::kServers && servers_from_pause_);
  }
  // Back to the match from wherever in the pause (its next opening starts on its first row).
  void ClosePause(const char* by) {
    REXLOG_INFO("BF pause: closed by {}", by);
    open_ = false, page_ = Page::kPause, pause_confirm_ = -1, pause_focus_panel_ = false;
    ctl_from_pause_ = lb_from_pause_ = ach_from_pause_ = from_pause_ = servers_from_pause_ = false;
  }

  void PauseAct(int row) {
    if (row == kPauseResume) {
      ClosePause("RESUME");
    } else if (row == kPauseSettingsRow) {  // the settings page (the new look), back here from it
      page_ = Page::kSettings, from_pause_ = true, from_home_ = false, tab_ = 0, row_ = 0, scroll_ = 0, sub_ = kSubNone;
    } else if (row == kPauseSquad) {
      pause_confirm_ = row;  // its card: find an open squad, or leave this one
    } else if (row == kPauseScoreboard) {
      page_ = Page::kScoreboard, sb_asked_at_ = -100;
    } else if (row == kPauseControls) {  // the game's in game Help & Options > Controls
      page_ = Page::kControls, ctl_tab_ = 0, ctl_from_pause_ = true;
    } else if (row == kPauseLeaderboards) {
      page_ = Page::kLeaderboards, lb_from_pause_ = true, lb_asked_at_ = -100;
    } else if (row == kPauseAchievements) {
      LoadTestProfile();
      page_ = Page::kAchievements, ach_scroll_ = 0, ach_from_pause_ = true;
    } else if (row == kPauseExit || row == kPauseSuicide || row == kPauseTeam) {
      pause_confirm_ = row;
    }
  }

  void PausePage(ImDrawList* d, float s, ImGuiIO& io, const Input& in) {
    (void)s;
    const ImVec2 sz = io.DisplaySize;
    const kit::Fonts& f = kit::GetFonts();
    const float u = sz.y / 1080.0f;
    const int count = int(std::size(kPauseRows));
    // The match behind, darker toward the column.
    d->AddRectFilledMultiColor(ImVec2(0, 0), sz, IM_COL32(6, 7, 5, 222), IM_COL32(6, 7, 5, 120),
                               IM_COL32(6, 7, 5, 120), IM_COL32(6, 7, 5, 222));
    const float cx0 = sz.x * 0.05f, cx1 = cx0 + 560 * u;
    d->AddRectFilled(ImVec2(0, 0), ImVec2(cx1, sz.y), IM_COL32(15, 17, 13, 238));
    d->AddRectFilled(ImVec2(cx1, 0), ImVec2(cx1 + 2 * u, sz.y), IM_COL32(201, 183, 122, 110));
    // The heading: the brand, a typed caption, a stencilled PAUSED and a red rule.
    kit::Brand(d, ImVec2(cx0, 58 * u), u * 0.9f);
    const float hy = 170 * u;
    kit::Spaced(d, f.typed, 20 * u, ImVec2(cx0, hy), kit::kKhakiDim, "IN THE FIELD", 4 * u);
    Text(d, f.stencil, 96 * u, ImVec2(cx0 - 4 * u, hy + 26 * u), kit::kCream, "PAUSED");
    d->AddRectFilled(ImVec2(cx0, hy + 138 * u), ImVec2(cx0 + 64 * u, hy + 144 * u), kit::kStamp);
    // The settings panel takes input only when it already had the focus as this frame began: the A that opens it
    // from the SETTINGS row is not also its first row's press (TekRantGaming, 10 Nov: it switched the game to a window).
    const bool panel_focused_before = pause_focus_panel_;
    // The rows (nine since CONTROLS: a little tighter, clear of the prompts).
    const float ry = hy + 186 * u, rh = 66 * u;
    const bool rows_have_input = pause_confirm_ < 0 && !pause_focus_panel_;
    if (rows_have_input) {
      if (in.up) pause_row_ = (pause_row_ + count - 1) % count;
      if (in.down) pause_row_ = (pause_row_ + 1) % count;
    }
    int clicked = -1;
    for (int i = 0; i < count; ++i) {
      const ImVec2 a(cx0 - 28 * u, ry + rh * i), b(cx1, ry + rh * (i + 1));
      const bool sel = i == pause_row_;
      const bool hover = ImGui::IsMouseHoveringRect(a, b);
      if (rows_have_input && hover && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) pause_row_ = i;
      if (rows_have_input && hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = i;
      if (sel) {
        d->AddRectFilled(a, b, IM_COL32(201, 183, 122, 26));
        d->AddRectFilled(a, ImVec2(a.x + 8 * u, b.y), kit::kStamp);
      }
      d->AddRectFilled(ImVec2(cx0, b.y - 1 * u), ImVec2(cx1 - 40 * u, b.y), IM_COL32(201, 183, 122, 34));
      kit::Spaced(d, f.typed, 16 * u, ImVec2(cx0, a.y + 24 * u), sel ? kit::kStamp : kit::kKhakiDim,
                  fmt::format("{:02d}", i + 1), 2 * u);
      kit::Spaced(d, f.ui_bold, 38 * u, ImVec2(cx0 + 52 * u, a.y + 12 * u), sel ? kit::kCream : kit::kKhakiText,
                  kPauseRows[i], 3 * u);
    }
    if (rows_have_input && in.accept) clicked = pause_row_;
    if (rows_have_input && in.back) {  // Start closed it already (OnDraw)
      ClosePause("B or Esc");
      return;
    }
    if (clicked >= 0) pause_row_ = clicked, PauseAct(clicked);
    // The right side: the settings while their row is chosen, or a row's question.
    const ImVec2 pa(cx1 + sz.x * 0.05f, sz.y * 0.14f), pb(sz.x * 0.95f, sz.y * 0.86f);
    if ((pause_row_ == kPauseSettingsRow || pause_focus_panel_) && pause_confirm_ < 0) {
      if (SettingsPanel(d, pa, pb, panel_focused_before ? in : Input{}, pause_focus_panel_)) {
        pause_focus_panel_ = false;
      } else if (!pause_focus_panel_ && ImGui::IsMouseHoveringRect(pa, pb) &&
                 ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        pause_focus_panel_ = true;
      }
    }
    if (pm_hosting_ && pm_joining_ && pause_confirm_ < 0 && pause_row_ != kPauseSettingsRow && !pause_focus_panel_)
      PrivateMatchPauseCard(d, io, in);  // our private match: its players, its reach, the address for friends
    if (pause_confirm_ == kPauseSquad) {  // My Squad: the game's own squad commands (traced, run squadtrace1)
      const float w = 760 * u, h = 280 * u;
      const ImVec2 ca((pa.x + pb.x - w) * 0.5f, sz.y * 0.5f - h * 0.5f), cb(ca.x + w, ca.y + h);
      kit::Paper(d, ca, cb, u, false);
      kit::Spaced(d, f.typed, 18 * u, ImVec2(ca.x + 40 * u, ca.y + 34 * u), kit::kInkFaint, "MY SQUAD", 4 * u);
      Text(d, f.stencil, 50 * u, ImVec2(ca.x + 38 * u, ca.y + 64 * u), kit::kInk, "SQUAD UP");
      kit::Wrapped(d, f.typed, 20 * u, ImVec2(ca.x + 40 * u, ca.y + 132 * u), kit::kInkSoft,
                   "Join an open squad to spawn on your squad mates, or leave the one you are in.", w - 80 * u);
      const ImVec2 ya(ca.x + 40 * u, cb.y - 80 * u), yb(ca.x + 380 * u, cb.y - 28 * u);
      const ImVec2 na(ca.x + 400 * u, cb.y - 80 * u), nb(ca.x + 720 * u, cb.y - 28 * u);
      d->AddRectFilled(ya, yb, kit::kInk);
      d->AddRectFilled(na, nb, kit::kPaperDim);
      const float gy = kit::Glyph(d, PromptStyle(), kit::Button::kA, "Enter", ImVec2(ya.x + 14 * u, (ya.y + yb.y) * 0.5f), 28 * u);
      kit::Spaced(d, f.ui_bold, 22 * u, ImVec2(ya.x + 24 * u + gy, ya.y + 13 * u), kit::kStripText, "FIND AN OPEN SQUAD", 2 * u);
      const float gn = kit::Glyph(d, PromptStyle(), kit::Button::kX, "F", ImVec2(na.x + 14 * u, (na.y + nb.y) * 0.5f), 28 * u);
      kit::Spaced(d, f.ui_bold, 22 * u, ImVec2(na.x + 24 * u + gn, na.y + 13 * u), kit::kInk, "LEAVE SQUAD", 2 * u);
      const bool find = (in.accept && clicked < 0) ||
                        (ImGui::IsMouseHoveringRect(ya, yb) && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
      const bool leave = in.x || (ImGui::IsMouseHoveringRect(na, nb) && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
      if (find || leave) {
        REXLOG_INFO("BF pause: {} (the game's own command)", find ? "find an open squad" : "leave the squad");
        GameCommand(find ? "triggerLookingForSquad" : "leaveSquad", "");
        pause_confirm_ = -1;
        open_ = false;
        return;
      }
      if (in.back) pause_confirm_ = -1;
    } else if (pause_confirm_ == kPauseTeam) {
      pause_confirm_ = -1;
      ChangeTeamNow();
      open_ = false;
      return;
    } else if (pause_confirm_ >= 0) {
      const bool exit = pause_confirm_ == kPauseExit;
      const float w = 760 * u, h = 300 * u;
      const ImVec2 ca((pa.x + pb.x - w) * 0.5f, sz.y * 0.5f - h * 0.5f), cb(ca.x + w, ca.y + h);
      kit::Paper(d, ca, cb, u, false);
      kit::Spaced(d, f.typed, 18 * u, ImVec2(ca.x + 40 * u, ca.y + 34 * u), kit::kInkFaint,
                  exit ? "EXIT GAME" : "SUICIDE", 4 * u);
      Text(d, f.stencil, 54 * u, ImVec2(ca.x + 38 * u, ca.y + 64 * u), kit::kInk,
           exit ? "LEAVE THIS MATCH?" : "END THIS LIFE?");
      kit::Wrapped(d, f.typed, 20 * u, ImVec2(ca.x + 40 * u, ca.y + 140 * u), kit::kInkSoft,
                   exit ? (pm_hosting_ ? "You go back to the main menu, and your custom match ends for everyone in it."
                                       : "You go back to the main menu. Your score in this round stays with the server.")
                        : "Your soldier dies and you pick a new spawn point. It counts as a death.",
                   w - 80 * u);
      const ImVec2 ya(ca.x + 40 * u, cb.y - 84 * u), yb(ca.x + 300 * u, cb.y - 30 * u);
      const ImVec2 na(ca.x + 330 * u, cb.y - 84 * u), nb(ca.x + 590 * u, cb.y - 30 * u);
      d->AddRectFilled(ya, yb, kit::kInk);
      d->AddRectFilled(na, nb, kit::kPaperDim);
      const float gy = kit::Glyph(d, PromptStyle(), kit::Button::kA, "Enter",
                                  ImVec2(ya.x + 14 * u, (ya.y + yb.y) * 0.5f), 30 * u);
      kit::Spaced(d, f.ui_bold, 26 * u, ImVec2(ya.x + 26 * u + gy, ya.y + 12 * u), kit::kStripText, "YES", 3 * u);
      const float gn = kit::Glyph(d, PromptStyle(), kit::Button::kB, "Esc",
                                  ImVec2(na.x + 14 * u, (na.y + nb.y) * 0.5f), 30 * u);
      kit::Spaced(d, f.ui_bold, 26 * u, ImVec2(na.x + 26 * u + gn, na.y + 12 * u), kit::kInk, "NO", 3 * u);
      const bool yes = in.accept || (ImGui::IsMouseHoveringRect(ya, yb) && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
      const bool no = in.back || (ImGui::IsMouseHoveringRect(na, nb) && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
      if (yes && clicked < 0) {
        REXLOG_INFO("BF pause: {} chosen (the game's own command)", exit ? "Exit Game" : "Suicide");
        pause_confirm_ = -1;
        if (exit) {
          Leave();
          return;
        }
        SuicideNow();
        open_ = false;
        return;
      }
      if (no) pause_confirm_ = -1;
    }
    // The prompts (the playtest stamp is the menu's own, top right).
    const float py = sz.y - 64 * u;
    float x = cx0;
    x += kit::Glyph(d, PromptStyle(), kit::Button::kA, "Enter", ImVec2(x, py), 34 * u) + 10 * u;
    x += kit::Spaced(d, f.ui_bold, 24 * u, ImVec2(x, py - 14 * u), kit::kKhakiText, "SELECT", 3 * u) + 34 * u;
    x += kit::Glyph(d, PromptStyle(), kit::Button::kB, "Esc", ImVec2(x, py), 34 * u) + 10 * u;
    kit::Spaced(d, f.ui_bold, 24 * u, ImVec2(x, py - 14 * u), kit::kKhakiText,
                pause_confirm_ >= 0 || pause_focus_panel_ ? "BACK" : "RESUME", 3 * u);
  }

  void RunPauseRow(int clicked) {
    const ListItem& it = kPauseItems[clicked];
    // BF's selection moves from where it is (BF keeps it between pauses, run origpause3) to the row.
    std::vector<std::pair<double, uint16_t>> presses = BfRowPresses(bf_cursor_, it.bf_row);
    if (it.does == Does::kNothing) {  // BF does nothing with it on PC, as on the original
      PressBfTimed(presses);
      return;
    }
    if (it.does == Does::kLeave) {  // the game's own Exit Game command (doExitLevel), not its rows pressed
      LeaveMatch();
      page_ = Page::kLeaving;
      leaving_at_ = ImGui::GetTime();
      return;
    }
    if (it.does == Does::kQuestion) {  // Suicide: the game's own command; the pause closes as the game's does
      SuicideNow();
      open_ = false;
      return;
    }
    // BF's own screen takes over with the real pad once the presses are done (ours stays over BF's list until
    // then, so its selection moving never shows); ours comes back when the player backs out to BF's list.
    PressBfTimed(presses);
    handing_over_ = clicked;
  }

  // While BF's own screen (from the main menu or the pause) has the real pad: B back to BF's list brings our list
  // back over it. Each A on a screen of BF's goes one screen deeper, each B one back. The scoreboard's B and the
  // Suicide question's A end the pause (BF goes back to the match), as does Start in the pause. Under pad_mutex_.
  void Passing(uint16_t pressed) {
    if (passing_ < 0) return;
    const Does does = (passing_main_ ? kMainItems : kPauseItems)[passing_].does;
    if ((pressed & kPadStart) && !passing_main_) {
      passing_ = -1;
    } else if (does == Does::kHelp) {
      // BF's Help & Options in the pause (runs pausehelp3 to pausehelp9; TekRantGaming, 9 Nov: going in and out was janky): its
      // list (Controls, Settings, How to Play) and the screens under it, where B goes back a screen, or in How to Play
      // a page (and Y leaves it). Where a B leads cannot be counted, so after each B or Y our list comes back only if
      // BF's pause list is then on screen (ReopenPause looks).
      if (pressed & (kPadB | 0x8000)) {
        reopen_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(450);
        reopen_check_ = true, reopen_tries_ = 0;
      }
    } else if (pressed & kPadB) {
      if (passing_depth_ > 0) --passing_depth_;
      else if (does == Does::kScreenThenGame) passing_ = -1;
      else if (does == Does::kScreenThenOk) passing_asked_ = true;  // BF's question; its OK goes back
      else reopen_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
    } else if (pressed & kPadA) {
      if (does == Does::kQuestion) passing_ = -1;
      else if (does == Does::kScreenThenOk && passing_asked_) {
        reopen_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
      } else if (does == Does::kScreen || does == Does::kScreenThenOk) ++passing_depth_;
    }
  }
  void ReopenPause() {
    std::lock_guard<std::mutex> lock(pad_mutex_);
    if (passing_ < 0 || reopen_at_ == std::chrono::steady_clock::time_point{} ||
        std::chrono::steady_clock::now() < reopen_at_)
      return;
    // In a match, only while BF's own pause is still up (its menu signal): a B that resumed the game instead leaves
    // ours closed (it sat over the match, TekRantGaming, 9 Nov).
    if (!passing_main_ && SecondsSinceGameMenuSignal() > 2.5) {
      REXLOG_INFO("BF pause: BF's own pause is closed: ours stays closed");
      passing_ = -1, reopen_at_ = {};
      return;
    }
    // Where it is not known that BF's pause list is back (Help & Options), a look at the screen: not yet, wait for the
    // player's next B.
    if (reopen_check_) {
      if (!BfListShotShowing(1, kPauseItems[passing_].bf_row)) {
        // BF's list fades in: looked at again three times, 0.4 s apart.
        if (++reopen_tries_ < 4) reopen_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
        else reopen_check_ = false, reopen_at_ = {};
        return;
      }
      reopen_check_ = false;
    }
    if (passing_main_) page_ = Page::kMain, row_ = passing_;
    else page_ = Page::kPause, pause_row_ = passing_, pause_focus_panel_ = false, pause_opened_at_ = ImGui::GetTime();
    passing_ = -1, reopen_at_ = {};
    pad_ = {};
    open_ = true;
  }

  void Leave() {
    page_ = Page::kLeaving;
    leaving_at_ = ImGui::GetTime();
    LeaveMatch();
  }

  // BF's "Connecting" box: paper with a black heading and its turning arrow, on a plain backdrop (BF's own screens
  // change underneath while the driver walks them).
  void BfNotice(ImDrawList* d, ImGuiIO& io, const std::string& title, const std::string& line, bool spinner) {
    const ImVec2 sz = io.DisplaySize;
    const float s = io_scale_;
    d->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(44, 54, 64, 255));
    const ImVec2 a(sz.x * 0.306f, sz.y * 0.36f), b(sz.x * 0.697f, sz.y * 0.62f);
    d->AddRectFilled(ImVec2(a.x + 5, a.y + 7), ImVec2(b.x + 5, b.y + 7), IM_COL32(0, 0, 0, 70));
    d->AddRectFilled(a, b, kBfPaper);
    Text(d, g_fonts.bf, 34 * s, ImVec2(a.x + 18 * s, a.y + 6 * s), kBfInk, Upper(title));
    Text(d, g_fonts.bf_med, 26 * s, ImVec2(a.x + 18 * s, a.y + 64 * s), IM_COL32(50, 50, 48, 255), Upper(line));
    if (spinner) {  // BF's turning arrow, bottom right
      const ImVec2 c(b.x - 52 * s, b.y - 52 * s);
      const float r = 26 * s, t = float(ImGui::GetTime()) * 4.0f;
      d->PathArcTo(c, r, t, t + 5.0f, 32);
      d->PathStroke(IM_COL32(70, 70, 68, 255), 0, 6 * s);
      const ImVec2 tip(c.x + std::cos(t + 5.0f) * r, c.y + std::sin(t + 5.0f) * r);
      d->AddCircleFilled(tip, 7 * s, IM_COL32(70, 70, 68, 255));
    }
  }

  bool joined_from_start_ = false;  // the join began before the game had signed in (a server picked on Home)
  double joining_load_at_ = -1;     // when the joined level's load began (our page waits for its loading screen)

  // The map a join or a match is on, by its title, as MainBackdrop's index (-1: not one of the four).
  static int MapIndexOf(const std::string& title) {
    static constexpr const char* kTitles[] = {"Wake Island", "Guadalcanal", "Iwo Jima", "Coral Sea"};
    for (int i = 0; i < 4; ++i)
      if (title == kTitles[i]) return i;
    return -1;
  }

  // Joining and leaving a match as the main menu is drawn (13 Nov 2026: "this screen also needs to follow the same
  // theming"): the menu's backdrop and chrome, the dark rail at the left with the caption, the heading in the game's
  // own lettering and the steps as its rows (the one under way on paper with the red tab and a turning marker), and
  // at the right the map's picture over a paper band. Opaque, so the game's own screens changing underneath never
  // show. Sizes are the Stage's (720 lines).
  void TransitPage(ImDrawList* d, ImGuiIO& io, int map, const std::string& caption, const std::string& heading,
                   const std::string& sub, const std::vector<std::string>& steps, int at, const std::string& error) {
    const Stage st = StageOf(io);
    const kit::Fonts& f = kit::GetFonts();
    MenuBackdrop(d, io.DisplaySize, st);
    bool online_ok = false;
    const std::string status = OnlineStatus(online_ok);
    MenuChrome(d, io, st, status, online_ok);
    MenuFoot(d, st);
    // ---- the rail: the caption, the heading, the steps
    const ImVec2 ra = st.P(48, 93), rb = st.P(400, 620);
    d->AddRectFilled(ra, rb, IM_COL32(13, 15, 11, 204));
    d->AddRect(ra, rb, IM_COL32(241, 234, 211, 30), 0, 0, 1.0f);
    kit::Spaced(d, f.typed, st(8), ImVec2(ra.x + st(24), ra.y + st(18)), kit::kKhakiDim, Upper(caption), st(2));
    // The heading at its size, smaller when it would not fit the rail (a long server name as the heading).
    float hs = st(26);
    const float room = rb.x - ra.x - st(48);
    while (hs > st(13) && kit::GameTextWidth(kit::GameFont::kHeadline, hs, Upper(heading)) > room) hs -= st(1);
    kit::GameText(d, kit::GameFont::kHeadline, hs, ImVec2(ra.x + st(24), ra.y + st(32) + (st(26) - hs) * 0.5f), kit::kCream,
                  Upper(heading));
    d->AddRectFilled(ImVec2(ra.x, ra.y + st(76)), ImVec2(rb.x, ra.y + st(77)), IM_COL32(241, 234, 211, 30));
    const float h = st(41);
    for (size_t i = 0; i < steps.size(); ++i) {
      const ImVec2 r0(ra.x, ra.y + st(77) + h * float(i)), r1(rb.x, r0.y + h);
      const bool done = int(i) < at, now = int(i) == at && error.empty();
      if (now) {
        kit::PaperArt(d, r0, r1);
        d->AddRectFilled(r0, ImVec2(r0.x + st(5), r1.y), kit::kStamp);
      }
      d->AddRectFilled(ImVec2(r0.x, r1.y - 1), ImVec2(r1.x, r1.y), IM_COL32(241, 234, 211, 20));
      const ImVec2 c(r0.x + st(30), r0.y + h * 0.5f);
      if (done) d->AddCircleFilled(c, st(5), kit::kGreenBright);
      else d->AddCircle(c, st(5), now ? kit::kInk : kit::kKhakiDim, 0, st(1.5f));
      if (now) {  // a turning arc over the step under way
        const float a0 = float(ImGui::GetTime()) * 5.0f;
        d->PathArcTo(c, st(9), a0, a0 + 2.2f, 16);
        d->PathStroke(kit::kStamp, 0, st(2));
      }
      float cap = st(16);
      const std::string label = Upper(steps[i]);
      while (cap > st(10) && kit::GameTextWidth(kit::GameFont::kHeadlineMedium, cap, label) > rb.x - r0.x - st(70)) cap -= st(1);
      kit::GameText(d, kit::GameFont::kHeadlineMedium, cap, ImVec2(r0.x + st(52), r0.y + (h - cap) * 0.5f),
                    now ? kit::kInk : done ? kit::kKhaki : kit::kKhakiDim, label);
    }
    // ---- the picture card at the right: the map from the air, the paper band under it
    static constexpr const char* kMapTitles[] = {"Wake Island", "Guadalcanal", "Iwo Jima", "Coral Sea"};
    const ImVec2 ha = st.P(427, 93), hb = st.P(1232, 380);
    const ImVec2 band(ha.x, hb.y - st(110));
    d->PushClipRect(ha, hb, true);
    MapPicture(d, st.u * 1.5f, ha, ImVec2(hb.x, band.y), map >= 0 && map < 4 ? kMapTitles[map] : "Wake Island");
    d->PopClipRect();
    kit::PaperArt(d, band, hb);
    kit::Spaced(d, f.typed, st(9), ImVec2(band.x + st(20), band.y + st(14)), kit::kInkSection, Upper(caption), st(2));
    float bs = st(26);
    while (bs > st(13) && kit::GameTextWidth(kit::GameFont::kHeadline, bs, Upper(heading)) > hb.x - band.x - st(40)) bs -= st(1);
    kit::GameText(d, kit::GameFont::kHeadline, bs, ImVec2(band.x + st(20), band.y + st(29)), kit::kInk, Upper(heading));
    kit::Wrapped(d, f.ui, st(11.5f), ImVec2(band.x + st(20), band.y + st(66)), error.empty() ? kit::kInkFaint : kit::kRed,
                 error.empty() ? sub : error, hb.x - band.x - st(40));
    if (!error.empty()) {  // the callers act on B; the strip shows it (a click on it is not taken here)
      Input none;
      std::vector<Cue> cues = {{kit::Button::kB, "BACK"}};
      Prompts(d, st, st.P(48, 662), cues, none, 15);
    }
  }

  void LeavingPage(ImDrawList* d, float s, ImGuiIO& io) {
    (void)s;
    // Still in the match after 30 s (the walk takes about 17): BF's Exit Game did not go through; back to the match.
    if (MenuDriverStage() == 5 && ImGui::GetTime() - leaving_at_ > 30) open_ = false, page_ = Page::kMain;
    const int map = MapIndexOf(joining_map_);
    if (lost_) {
      TransitPage(d, io, map, "The connection to the server was lost", "Disconnected", joining_,
                  {"Back to the main menu"}, 0, "");
      return;
    }
    if (tutorial_) {  // the game's tutorial: its training ground, no server
      TransitPage(d, io, map, "Exit game", "Leaving the tutorial", "", {"Leaving the training ground", "Back to the main menu"},
                  MenuDriverStage() == 5 ? 0 : 1, "");
      return;
    }
    TransitPage(d, io, map, "Exit game", "Leaving the match", joining_, {"Leaving the server", "Back to the main menu"},
                MenuDriverStage() == 5 ? 0 : 1, "");
  }

  void JoiningPage(ImDrawList* d, float s, ImGuiIO& io, const Input& in) {
    (void)s;
    const int stage = MenuDriverStage();
    // The steps of a join: the game's hidden start and sign in first when it was not running yet (a server picked on
    // Home), then connecting, the server's own join, and the map's load.
    std::vector<std::string> steps;
    const bool starting = joined_from_start_;
    if (starting) steps.push_back("Starting the game");
    steps.push_back("Connecting");
    steps.push_back("Joining the server");
    steps.push_back(joining_map_.empty() ? "Loading the map" : "Loading " + joining_map_);
    const int base = starting ? 1 : 0;
    const int at = stage < 2 ? 0 : stage == 5 ? base + 2 : stage == 4 ? base + 1 : base;
    if (tutorial_) {  // the game's tutorial: no server, its training level only
      TransitPage(d, io, 0, failed_ ? "The tutorial did not start" : "Training", "Wake Island", "The game's tutorial",
                  {"Loading the training ground"}, 0, failed_ ? "The training level did not load." : "");
      if (failed_ && in.back) page_ = Page::kHelp, help_row_ = 0, tutorial_ = false;
      return;
    }
    TransitPage(d, io, MapIndexOf(joining_map_), failed_ ? "The join did not go through" : "Joining",
                joining_map_.empty() ? "Battlefield 1943" : joining_map_, joining_, steps, at,
                failed_ ? "Could not join the server. It may be full, or it stopped." : "");
    if (failed_ && in.back) page_ = Page::kMain, row_ = 1;
  }

#include "game_menu_launcher.inc"
#include "game_menu_home.inc"
#include "game_menu_browser.inc"
#include "game_menu_settings.inc"
#include "game_menu_setup.inc"
#include "game_menu_map.inc"
#include "game_menu_pause.inc"

  // Which device the prompts show (King Kong's rules): a controller's press, or a key, a click, the wheel or the
  // mouse moving on two frames in a row.
  void TrackDevice(ImGuiIO& io, bool pad_pressed, const Input& key) {
    const bool key_used = key.up || key.down || key.left || key.right || key.accept || key.back || key.prev_tab ||
                          key.next_tab || key.x || key.y || key.start || key.view || io.InputQueueCharacters.Size > 0;
    mouse_frames_ = io.MouseDelta.x != 0 || io.MouseDelta.y != 0 ? mouse_frames_ + 1 : 0;
    const bool mouse_used = mouse_frames_ >= 2 || ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                            ImGui::IsMouseClicked(ImGuiMouseButton_Right) || io.MouseWheel != 0;
    const Device was = device_.load(std::memory_order_relaxed);
    Device to = was;
    if (key_used || mouse_used) to = Device::kKeyboard;
    if (pad_pressed) to = Device::kPad;
    if (to == was) return;
    device_.store(to, std::memory_order_relaxed);
    REXLOG_INFO("BF launcher: prompts now the {}'s", to == Device::kPad ? "controller" : "keyboard and mouse");
  }
  // One press per action across the controller and the keyboard: Steam's desktop layout types keys for a controller,
  // so the same action from the other device within 0.25 s is the same press (King Kong's rule).
  Input MergeInput(const Input& pad, const Input& key) {
    const double now = ImGui::GetTime();
    Input out;
    int i = 0;
    for (bool Input::*m : {&Input::up, &Input::down, &Input::left, &Input::right, &Input::accept, &Input::back,
                           &Input::prev_tab, &Input::next_tab, &Input::x, &Input::y, &Input::start, &Input::view}) {
      bool p = pad.*m, k = key.*m;
      if (p) pad_at_[i] = now;
      if (k && now - pad_at_[i] < 0.25) k = false;
      if (p && !k && key_at_[i] > 0 && now - key_at_[i] < 0.25) p = false;
      if (k) key_at_[i] = now;
      out.*m = p || k;
      if (m == &Input::accept) accept_from_key_ = k && !p;  // the name page: Enter saves, the pad's A edits
      ++i;
    }
    return out;
  }
  bool LauncherPage(Page p) const {
    switch (p) {
      case Page::kIntro: case Page::kSetup: case Page::kName: case Page::kShaders: case Page::kSetupSettings: case Page::kPrompts:
      case Page::kVoice: case Page::kAllSet: case Page::kHome: case Page::kProjects: case Page::kReport:
      case Page::kStarting: case Page::kUpdate: return true;
      case Page::kSettings: return from_home_;
      default: return false;
    }
  }
  // Every frame: whether BF may sign in behind the launcher (a name chosen, and the backend's answer to it in), and the
  // time in matches for the rank.
  void UpdateWalk(ImGuiIO& io) {
    if (name_ready_at_ > 0) {
      const bool late = ImGui::GetTime() - name_ready_at_ > 3.0;
      if (PollNameCheck(late) || late) name_ready_at_ = 0;
    }
    const bool named = settings_.Get("player_name_chosen", "") == "1";
    // With the launcher before the game, the game started from it (Play, or a server picked on Home) boots hidden
    // behind our pages and signs in by itself (TekRantGaming, 10 Nov 2026: our startup videos in place of the game's own
    // startup, a join straight into the server; it replaces the 7 Nov rule of the player pressing Start on its title).
    LauncherWalks().store((!launcher_first_ || started_from_launcher_) && !SkipLauncher() && named && name_ready_at_ == 0,
                          std::memory_order_relaxed);
    // The player's score events (auto_join.cpp TakeScoreEvents), kept as stat.<label> = events,points for Stats &
    // Awards; saved when there are new ones, at most every 10 s.
    // The totals already in the profile, once a start, as soon as the player has a name.
    if (!stats_sent_at_start_ && named && !settings_.Get("player_name", "").empty()) {
      stats_sent_at_start_ = true;
      bool any = std::atof(settings_.Get("match_seconds", "0").c_str()) > 0;
      for (const auto& [key, value] : settings_.values) any = any || key.rfind("stat.", 0) == 0;
      if (any) SendLeaderboard("saved totals at the start");
    }
    if (ImGui::GetTime() - stats_taken_at_ > 10) {
      stats_taken_at_ = ImGui::GetTime();
      const auto events = TakeScoreEvents();
      for (const auto& [label, v] : events) match_score_ += v.second;
      for (const auto& [label, v] : events) {
        const std::string key = "stat." + label, old = settings_.Get(key, "0,0");
        const size_t comma = old.find(',');
        const int n = std::atoi(old.c_str()) + v.first;
        const double pts = (comma == std::string::npos ? 0.0 : std::atof(old.c_str() + comma + 1)) + v.second;
        settings_.values[key] = fmt::format("{},{:.0f}", n, pts);
      }
      if (!events.empty()) settings_.Save(), SendLeaderboard();  // our backend's Leaderboards
    }
    // The match score for the pause's Scoreboard: reset as each match begins, sent every 10 s while in it.
    {
      static int last_stage = -1;
      const int stage = MenuDriverStage();
      if (stage == 5 && last_stage != 5) match_score_ = 0, match_began_ = ImGui::GetTime();
      last_stage = stage;
      if (stage == 5 && !joined_gid_.empty() && ImGui::GetTime() - match_score_sent_at_ > 10) {
        match_score_sent_at_ = ImGui::GetTime();
        std::string name;
        for (char c : settings_.Get("player_name", ""))
          if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_') name += c;
        if (!name.empty()) {
          const std::string req = fmt::format("{{\"op\": \"match_score\", \"gid\": \"{}\", \"name\": \"{}\", \"score\": {:.0f}}}",
                                              joined_gid_, name, match_score_);
          std::thread([req] {
            std::string reply;
            RegistryRequest(req, reply);
          }).detach();
        }
      }
    }
    if (MenuDriverStage() == 5 && LevelLoaded()) {
      match_accum_ += io.DeltaTime;
      if (match_accum_ >= 60) {
        const double total = std::atof(settings_.Get("match_seconds", "0").c_str()) + match_accum_;
        settings_.values["match_seconds"] = std::to_string(int64_t(total));
        settings_.Save();
        match_accum_ = 0;
      }
    }
  }

  // ---- the launcher's state (game_menu_launcher.inc)
  enum class Device { kPad, kKeyboard };
  std::atomic<Device> device_{Device::kPad};  // the UI thread (TrackDevice) and the game's pad reads (NoteMatchDevice)
  std::atomic<bool> game_flip_y_{false};      // GameFlipY: the game's own Vertical look, for the pad hook's thread
  int mouse_frames_ = 0;
  double pad_at_[12] = {}, key_at_[12] = {};
  bool pad_pressed_ = false;  // pad_mutex_
  bool release_click_ = false;  // a test's click: the button goes up a frame later
  // A scripted test run's mouse (BF_DEV_SCRIPTED): this frame's place, the next frame's, the last frame's.
  ImVec2 script_mouse_{-FLT_MAX, -FLT_MAX}, script_mouse_to_{-FLT_MAX, -FLT_MAX}, script_mouse_was_{-FLT_MAX, -FLT_MAX};
  Input test_key_;            // pad_mutex_: the tests' key presses (BF_MENU_STEPS key/...)
  std::string logged_page_;
  bool setup_active_ = false, from_home_ = false, skip_popup_ = false, quit_popup_ = false, files_help_ = false;
  bool steam_added_ = false, signed_in_once_ = false, name_setup_ = false, name_keys_ = false;
  bool name_required_ = false;  // the saved name was refused (profanity): the name page has no Back until a new one
  std::atomic<bool> send_game_settings_{false};  // Show: the game's own options to send (SendGameSettings)
  int setup_tab_ = 0, prompts_row_ = 0, popup_row_ = 0, home_row_ = 0, project_ = 0, files_card_ = 0, allset_row_ = 0;
  double settle_until_ = 0, next_page_at_ = 0, starting_at_ = 0, shader_started_ = -1, name_ready_at_ = 0;
  uint32_t shader_started_done_ = 0;
  double update_checked_at_ = 0;  // the start's check is CreateGameMenu's
  double menu_shown_at_ = -100;   // the main menu's last frame: a gap is an arrival (UpdateOnMenu)
  int update_retries_ = 0;        // checks made again by themselves after one without an answer (this run)
  double update_tried_at_ = -1e9; // the last of them
  // The version gate's pop-up (AskToUpdate): the version the server needs, the server that refused, and where Update
  // now goes back to when nothing could be installed.
  bool update_popup_ = false;
  std::string update_needs_, update_server_;
  // What's new after an update (changelog.h): waiting for the main menu and this version's shaders; open; listing
  // every version; the version run before (empty: not known); how far the notes are scrolled.
  bool whats_new_pending_ = false, whats_new_open_ = false, whats_new_all_ = false;
  std::string whats_new_from_;
  float whats_new_scroll_ = 0;
  bool update_asked_ = false;     // Update now was chosen: the update page shows the check and the download too
  bool relaunched_ = false;
  float match_accum_ = 0;
  static constexpr int kNameMax = 15;
  std::string name_text_, name_error_, name_saved_;
  int name_key_ = 10;
  bool typed_logged_ = false;
  bool accept_from_key_ = false;
  int applied_cap_ = -1;            // the outside frame limit ApplyFrameRate last saw
  std::string applied_rate_;        // the frame rate it last applied  // this frame's accept was the keyboard's Enter (MergeInput)
  bool name_caps_ = true;
  Page name_from_ = Page::kHome;

  Settings settings_;
  std::function<void(bool)> fullscreen_;
  std::function<void()> quit_;
  ServerList list_;
  std::atomic<bool> open_{false};
  Page page_ = Page::kMain;
  int row_ = 0;
  bool failed_ = false, auto_joined_ = false, from_pause_ = false, lost_ = false;
  double leaving_at_ = 0;
  std::string joining_, joining_map_;
  std::string pending_;  // a join picked while the game was still starting
  bool servers_from_home_ = false;  // the server browser opened from Home (before the game): B goes back there
  std::string joined_gid_;          // the server joined last (Join)
  double reserved_at_ = -1000;      // when the backend last kept a server for this game (Join)
  double match_score_ = 0;          // this match's score so far (the HUD's score events since it began)
  double match_score_sent_at_ = 0;
  double stats_taken_at_ = 0;       // the score events last taken (UpdateWalk)
  bool started_from_launcher_ = false;  // the game started from our launcher (Play, a server on Home): it signs in hidden
  ShaderProgressFn shader_progress_ = nullptr;
  enum Sub { kSubNone, kSubRemap, kSubKeys };
  int tab_ = 0;          // the settings' tab (kTabDisplay...)
  Sub sub_ = kSubNone;   // a page under Controls
  float scroll_ = 0;     // the settings' rows scrolled up by
  std::string capture_;  // the keybind_* setting waiting for a key
  int capture_slot_ = 0;  // its slot (0, 1 keys; 2, 3 mouse buttons)
  double esc_held_since_ = -1;  // Esc down while a slot waits (ImGui's time; -1: it is up)
  bool esc_swallow_ = false;    // the Esc that cleared a slot is still held
  int slot_cursor_ = 0;   // the slot left and right are on, in the bindings' chosen row
  std::function<rex::ui::Window*()> window_;
  std::vector<DisplayMode> modes_;  // the monitor's fullscreen modes, listed once per monitor
  std::vector<MonitorInfo> displays_;
  int modes_monitor_ = -1;
  bool modes_listed_ = false;
  int pause_row_ = 0;             // the pause list's row (kPauseItems)
  int bf_cursor_ = 0;             // BF's own pause list's selected row, under ours
  int pause_pending_row_ = -1, pause_probe_frames_ = 0;  // a row of BF's chosen: BF's selection read first
  bool gpu_benchmark_started_ = false;  // D150: the GPU's speed timed, from gpu_benchmark_at_ (2 s after the shaders)
  double gpu_benchmark_at_ = 0;
  int main_cursor_ = 0;           // BF's own main menu list's selected row, under ours
  bool passing_main_ = false;     // passing_ is a kMainItems row (else kPauseItems)
  bool passing_asked_ = false;    // BF asked its question on the way back (kScreenThenOk)
  int bf_letters_ = -1;           // BF's lettering: -1 not tried yet, 0 not found, 1 loaded
  int shot_tries_ = 0;            // copies of BF's list tried (bf_list_shot.cpp)
  int pause_shot_tries_ = 0;      // the same for BF's pause list
  double pause_shot_wait_ = -1;
  double shot_wait_ = -1;         // when our list was hidden for the copy
  std::function<fs::path()> game_dir_;
  int last_stage_ = -1;
  int passing_ = -1;              // the kPauseItems row whose BF screen has the real pad, or -1
  int passing_depth_ = 0;         // My squad's screens opened below it
  bool reopen_check_ = false;     // ReopenPause looks for BF's pause list first (Does::kHelp)
  int reopen_tries_ = 0;          // its looks so far
  double pause_opened_at_ = 0;    // when our pause list last opened over BF's
  int handing_over_ = -1;         // the row whose BF presses are running (our list stays up until they are done)
  std::chrono::steady_clock::time_point reopen_at_{};
  bool pause_focus_panel_ = false;  // the pause's PC settings panel has the focus
  float io_scale_ = 1;    // the display's height / 1080, for this frame
  float shader_bar_ = 0;  // the bar as drawn: eases towards the count, never back
  bool shaders_shown_ = false;
  bool shaders_behind_ = false;      // a prepared start: the stored pipelines still being made behind the menus
  double shaders_behind_from_ = 0;
  bool shaders_prepared_hint_ = false;  // decided at Play (ShadersPreparedForPlay), before the game has loaded
  bool join_after_shaders_ = false;     // a server picked on Home: its join once the shader page is done
  bool setup_shader_step_ = false;      // the first start's step 8 is up: Ready follows it (AfterShaders)
  double shaders_wait_from_ = -1;       // the game was started for the shader page: until its renderer is up (or 60 s)

  // King Kong's rule decided at Play: the start's own check (CheckShadersAfterUpdate) runs once the game has loaded,
  // a moment after Play, from the same marker and storage; deciding here too means a version prepared before never
  // shows the shader page, not even for that moment.
  bool ShadersPreparedForPlay() const {
    std::error_code ec;
    return settings_.Get("shaders_prepared_for", "") == BfVersion() &&
           fs::is_regular_file(UserDir() / "cache" / "native_x_renderer" / "5841097E.pipelines", ec);
  }
  std::mutex pad_mutex_;
  Input pad_;
  uint16_t last_pad_ = 0;
  bool was_open_ = false;  // OnDraw: the menu was open at the last frame (input left from before it opened is dropped)
};

GameMenu* g_menu = nullptr;

}  // namespace

void LoadMenuFonts(ImFontAtlas* atlas) {
#if defined(_WIN32)
  const fs::path dir = "C:\\Windows\\Fonts";
  auto add = [&](const char* file, float size) -> ImFont* {
    std::error_code ec;
    const fs::path p = dir / file;
    if (!fs::exists(p, ec)) return nullptr;
    return atlas->AddFontFromFileTTF(p.string().c_str(), size);
  };
  g_fonts.title = add("bahnschrift.ttf", 92);
  g_fonts.header = add("bahnschrift.ttf", 40);
  g_fonts.item = add("segoeui.ttf", 32);
  g_fonts.body = add("segoeui.ttf", 22);
  g_fonts.bold = add("seguisb.ttf", 32);
#else
  // Linux has none of Windows' fonts, and every distribution keeps its own in its own folders: the plain lettering
  // (the server message and chat boxes, the performance overlay) is the first of these families found under
  // /usr/share/fonts, by file name. The Flatpak's runtime has Liberation Sans, so the Flatpak looks the same on
  // every system; an AppImage reads the system's. Until 13 Nov 2026 nothing was found here (Windows' file names
  // were looked for) and those boxes were drawn in ImGui's own 13 pixel font, scaled up.
  struct Family {
    const char* regular;
    const char* bold;
  };
  static const Family kFamilies[] = {{"LiberationSans-Regular.ttf", "LiberationSans-Bold.ttf"},
                                     {"NotoSans-Regular.ttf", "NotoSans-Bold.ttf"},
                                     {"OpenSans-Regular.ttf", "OpenSans-Bold.ttf"},
                                     {"DejaVuSans.ttf", "DejaVuSans-Bold.ttf"}};
  std::map<std::string, fs::path> found;
  std::error_code ec;
  for (fs::recursive_directory_iterator it("/usr/share/fonts", fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    for (const Family& f : kFamilies)
      if (name == f.regular || name == f.bold) found.emplace(name, it->path());
  }
  for (const Family& f : kFamilies) {
    const auto regular = found.find(f.regular);
    if (regular == found.end()) continue;
    const auto bold = found.find(f.bold);
    const std::string r = regular->second.string();
    const std::string b = bold != found.end() ? bold->second.string() : r;
    g_fonts.item = atlas->AddFontFromFileTTF(r.c_str(), 32);
    g_fonts.body = atlas->AddFontFromFileTTF(r.c_str(), 22);
    g_fonts.bold = atlas->AddFontFromFileTTF(b.c_str(), 32);
    REXLOG_INFO("BF menu fonts: plain lettering from {} and {}", r, b);
    break;
  }
  if (!g_fonts.item) REXLOG_INFO("BF menu fonts: no plain font under /usr/share/fonts: Oswald instead");
#endif
  // Oswald (SIL Open Font License 1.1), shipped beside the exe: the condensed capitals of BF's menus.
  auto add_ours = [&](const char* file, float size) -> ImFont* {
    std::error_code ec;
    const fs::path p = ExecutableDir() / "fonts" / file;
    if (!fs::exists(p, ec)) return nullptr;
    return atlas->AddFontFromFileTTF(p.string().c_str(), size);
  };
  g_fonts.bf = add_ours("Oswald-Bold.ttf", 44);
  g_fonts.bf_med = add_ours("Oswald-Medium.ttf", 30);
  REXLOG_INFO("BF menu fonts: Oswald from {} ({})", (ExecutableDir() / "fonts").string(),
              g_fonts.bf ? "loaded" : "missing, Bahnschrift instead");
  // A missing plain font is Oswald, never ImGui's own 13 pixel one while a real font is at hand (and the other way
  // round, as before).
  ImFont* const oswald = g_fonts.bf;
  ImFont* const oswald_med = g_fonts.bf_med;
  if (!g_fonts.bf) g_fonts.bf = g_fonts.header;
  if (!g_fonts.bf_med) g_fonts.bf_med = g_fonts.body;
  if (!g_fonts.title) g_fonts.title = oswald;
  if (!g_fonts.header) g_fonts.header = oswald;
  if (!g_fonts.bold) g_fonts.bold = oswald_med ? oswald_med : oswald;
  if (!g_fonts.item) g_fonts.item = oswald_med ? oswald_med : oswald;
  if (!g_fonts.body) g_fonts.body = oswald_med ? oswald_med : oswald;
  // The launcher's stencil and typed lettering, and its Oswald (launcher_kit.cpp).
  kit::LoadFonts(atlas, ExecutableDir(), g_fonts.bf_med, g_fonts.bf);
  ImFont* fallback = atlas->Fonts.empty() ? nullptr : atlas->Fonts[0];
  for (ImFont** f : {&g_fonts.title, &g_fonts.header, &g_fonts.item, &g_fonts.body, &g_fonts.bold, &g_fonts.bf,
                     &g_fonts.bf_med})
    if (!*f) *f = fallback;
}

void CreateGameMenu(rex::ui::ImGuiDrawer* drawer, const fs::path& user_dir, std::function<void(bool)> fullscreen,
                    std::function<void()> quit, std::function<rex::ui::Window*()> window,
                    std::function<fs::path()> game_dir) {
  // A dedicated server shows no menu: since our menu opens on the first frame, a server's window looked like the game's
  // (TekRantGaming, 5 Nov).
  if (!REXCVAR_GET(bf_launcher) || REXCVAR_GET(server) || g_menu) return;
  const fs::path game = game_dir ? game_dir() : fs::path();
  g_menu = new GameMenu(drawer, user_dir / "bf1943-settings.txt", std::move(fullscreen), std::move(quit),
                        std::move(window), std::move(game_dir));
  // Updates (update.h; always on, TekRantGaming, 8 Nov 2026): the player's folders are never written, the .old files the
  // last update left are removed, then on a worker this version's first start check and the check for a newer one.
  update::SetProtectedFolders({user_dir, game});
  update::CleanUpAfterUpdate();
  update::StartInBackground(user_dir / "update-check.txt");
}

// The renderer chosen in Settings > Graphics > Graphics API: Vulkan (our rexgpu-bfnative) unless the player chose
// the SDK's Direct3D 12 one (TekRantGaming, 5 Nov: Vulkan is the renderer we work with). A command line or bf1943.toml
// gpu_plugin outranks it (OnPreSetup only asks when neither set one).
std::string StartupGpuPlugin(const fs::path&) {
  // Vulkan only (DECISIONS.md, 5 Nov 2026 evening): a saved Direct3D 12 choice from an older version is ignored. A
  // dedicated server gets the plugin's null backend (bf1943_app.h), which draws nothing.
  return "bfnative";
}

// The Display settings that take effect when the window is made (before OnCreateDialogs): the window mode, the
// monitor, the window size, the aspect ratio and VSync, from the settings file, as runtime settings. A setting the
// file doesn't have keeps the runtime's own value (so a test's --fullscreen=false stays).
// A new install's settings (TekRantGaming, 10 Nov 2026: on by default for every new install, the player changes them in the
// first start's setup or Settings): written once into a settings file whose setup never started, before anything
// applies them. The window is Borderless (fullscreen at the desktop's own resolution; TekRantGaming, the same day: 0.1.30's
// exclusive Fullscreen left the game minimised behind the setup's file picker). Not in test runs (a
// window kept in the background or hidden, as tools\run_game.ps1 starts it) unless BF_DEV_NEW_INSTALL=1, and their
// window is never made fullscreen.
void NewInstallDefaults(Settings& settings) {
  // 0.4.0 (TekRantGaming, 13 Nov 2026): the GI distance Full is the F8 panel's only; Far is the most the Settings
  // page and the Ultra preset offer. Both offered Full before, so a saved Full becomes Far once (gi_full_capped);
  // one saved from the F8 panel after that stays. A run without a settings file writes none here.
  if (settings.Get("gi_full_capped", "").empty() && !settings.values.empty()) {
    const auto it = settings.values.find("cvar.native_gi_distance");
    const bool full = it != settings.values.end() && std::atof(it->second.c_str()) >= 4.0;
    if (full) it->second = "3";
    settings.values["gi_full_capped"] = "1";
    settings.Save();
    if (full) REXLOG_INFO("BF settings: the GI distance Full is Far now (Full is in the F8 panel only since v0.4.0)");
  }
  // 0.1.30's new install Fullscreen (exclusive, "install_defaults = 1") becomes Borderless once.
  if (settings.Get("install_defaults", "") == "1" && settings.Get("fullscreen", "") == "on" &&
      ParseDisplayMode(settings.Get("fullscreen_mode", ""))) {
    settings.values["fullscreen_mode"] = "desktop", settings.values["install_defaults"] = "2";
    settings.Save();
    REXLOG_INFO("BF settings: 0.1.30's new install Fullscreen is Borderless now");
  }
  // 0.1.30 to 0.1.33 turned the performance metrics on for new installs (FPS only, then the full overlay;
  // "install_defaults" 1 to 3): they are off once, as for new installs now (TekRantGaming, 11 Nov 2026: "the performance
  // metrics is now defaulted to off"). F2 or Settings turns them on again.
  if (const std::string d = settings.Get("install_defaults", ""); d == "1" || d == "2" || d == "3") {
    const std::string was = settings.Get("cvar.bf_performance_metrics", "");
    const bool moved = was == "fps" || was == "full";
    if (moved) settings.values["cvar.bf_performance_metrics"] = "off";
    settings.values["install_defaults"] = "4";
    settings.Save();
    if (moved) REXLOG_INFO("BF settings: the performance metrics ({}) are off now, the default since v0.2.0", was);
  }
  // 0.2.1 (TekRantGaming, 11 Nov 2026): no Render resolution any more, the game draws the display's. A render resolution of
  // 720p chosen before (the auto detect's slow tier set it) with the upscaler Off becomes AMD FSR 1 at Performance,
  // which draws 720p for a 1080p display, so that player's frame rate stays (King Kong's rule: an update does not
  // change a player's picture); any other render resolution just goes.
  if (settings.Get("install_defaults", "") == "4") {
    const std::string scale = settings.Get("cvar.native_x_renderer_resolution_scale", "");
    const std::string scaling = settings.Get("cvar.native_x_renderer_scaling", "bilinear");
    const double s = scale.empty() ? 0.0 : std::atof(scale.c_str());
    const bool to_fsr = scaling == "bilinear" && s > 0.0 && s <= 1.0;
    if (to_fsr) {
      settings.values["cvar.native_x_renderer_scaling"] = "fsr";
      settings.values["cvar.bf_upscaler_mode"] = "performance";
    }
    settings.values.erase("cvar.native_x_renderer_resolution_scale");
    settings.values["install_defaults"] = "5";
    settings.Save();
    REXLOG_INFO("BF settings: no render resolution since v0.2.1 (it was {}){}", scale.empty() ? "Auto" : scale,
                to_fsr ? ": 720p with the upscaler Off is AMD FSR 1 at Performance now" : "");
  }
  if (REXCVAR_GET(server) || !settings.Get("launcher_setup", "").empty() || !settings.Get("install_defaults", "").empty())
    return;
  const std::string start = rex::cvar::GetFlagByName("window_start");
  const bool test_window = start == "background" || start == "hidden";
  bool asked = false;
#if defined(BF_DEV_TOOLS)
  if (const char* v = std::getenv("BF_DEV_NEW_INSTALL"); v && *v == '1') asked = true;
#endif
  if (test_window && !asked) return;
  static const std::pair<const char*, const char*> kDefaults[] = {
      // Display
      {"frame_rate", "60"}, {"vsync", "on"}, {"cvar.bf_fov", "86"}, {"cvar.bf_motion_blur", "false"},
      // Graphics: FXAA, shadows High (2x), sharpness High, 16x texture filtering, ambient occlusion Strong at High
      // quality, global illumination High to Medium distance, grass to the full map (always on: no row turns it off)
      {"cvar.native_x_renderer_antialiasing", "fxaa"}, {"cvar.native_x_renderer_shadow_scale", "2.000000"},
      {"cvar.native_x_renderer_sharpness", "0.800000"}, {"cvar.anisotropic_override", "5"},
      {"cvar.native_ao_mode", "1"}, {"cvar.native_ao_strength", "3"}, {"cvar.native_ao_samples", "3"},
      {"cvar.native_gi_mode", "1"}, {"cvar.native_gi_quality", "3"}, {"cvar.native_gi_distance", "2"},
      {"cvar.native_grass", "true"}, {"cvar.native_grass_radius", "1440.000000"},
      // Upscaler AMD FSR 1 at Quality, King Kong's default (TekRantGaming, 11 Nov 2026, v0.2.1). Only new installs: a settings
      // file from before has no upscaler, so its picture stays as it was (King Kong's settings version 2 did the same).
      {"cvar.native_x_renderer_scaling", "fsr"}, {"cvar.bf_upscaler_mode", "quality"},
      // Controls: camera response Modern. Gameplay: performance metrics off (the setting's own default).
      {"cvar.bf_camera_modern", "true"}, {"cvar.bf_performance_metrics", "off"}};
  for (const auto& [key, value] : kDefaults) settings.values[key] = value;
  settings.values["fullscreen_mode"] = "desktop";  // Borderless (a test window stays a window)
  if (!test_window) settings.values["fullscreen"] = "on";
  settings.values["install_defaults"] = "5";  // 0.2.1's: no render resolution to move (above)
  settings.values["gi_full_capped"] = "1";    // a new install never had the menus' Full (above)
  std::error_code ec;
  fs::create_directories(settings.path.parent_path(), ec);
  settings.Save();
  REXLOG_INFO("BF settings: a new install: its default settings written{}", test_window ? " (a test window: not fullscreen)" : "");
}

void ApplyStartupDisplaySettings(const fs::path& user_dir) {
  Settings settings;
  settings.path = user_dir / "bf1943-settings.txt";
  settings.Load();
  NewInstallDefaults(settings);
  auto set = [&](const char* key, const char* cvar) {
    if (auto it = settings.values.find(key); it != settings.values.end()) rex::cvar::SetFlagByName(cvar, it->second);
  };
  // Every runtime setting the menu saved ("cvar.<name>"), except the renderer, which OnPreSetup chooses itself
  // (StartupGpuPlugin).
  for (const auto& [key, value] : settings.values)
    if (key.rfind("cvar.", 0) == 0 && key != "cvar.gpu_plugin") rex::cvar::SetFlagByName(key.substr(5), value);
  // Grass and shrubs are always on (TekRantGaming, 10 Nov 2026: no longer a setting; only their distance is): a saved Off
  // from an older version is not taken. The F8 panel still switches them for the session.
  if (!REXCVAR_GET(server)) rex::cvar::SetFlagByName("native_grass", "true");
  // Antialiasing is Off or FXAA (TekRantGaming, 9 Nov 2026): FXAA by default and in place of a mode no longer offered
  // (SMAA, DLAA, DLSS, FSR 3.1); no frame generation (it needed those).
  if (!REXCVAR_GET(server)) {
    const std::string aa = rex::cvar::GetFlagByName("native_x_renderer_antialiasing");
    // 13 Nov 2026: the finished picture's antialiasing is bf_post_aa (the renderer's setting when that was never
    // chosen); ApplyUpscaler below writes the renderer's setting from it or from the temporal upscaler.
    if (rex::cvar::GetFlagByName("bf_post_aa").empty())
      rex::cvar::SetFlagByName("bf_post_aa", aa == "off" || aa == "fxaa" || aa.rfind("smaa", 0) == 0 ||
                                                     aa.rfind("cmaa", 0) == 0
                                                 ? aa
                                                 : "fxaa");
    rex::cvar::SetFlagByName("native_x_renderer_frame_generation", "false");
  }
  // The Upscaler (ApplyUpscaler): the sharpness as the settings and the command line give it is the one for Off.
  if (!REXCVAR_GET(server)) {
    g_plain_sharpness = rex::cvar::GetFlagByName("native_x_renderer_sharpness");
    ApplyUpscaler(true, settings.Get("nis_support", ""));
  }
  if (auto it = settings.values.find("fullscreen"); it != settings.values.end())
    rex::cvar::SetFlagByName("fullscreen", it->second == "on" ? "true" : "false");
  set("monitor", "monitor");
  set("window_width", "window_width");
  set("window_height", "window_height");
  if (auto it = settings.values.find("letterbox"); it != settings.values.end())
    rex::cvar::SetFlagByName("present_letterbox", it->second == "on" ? "true" : "false");
  // VSync, as King Kong's row: off lets frames show at once (tearing allowed, Vulkan's immediate or mailbox
  // present), on waits for the screen's refresh.
  if (auto it = settings.values.find("vsync"); it != settings.values.end()) {
    const char* tear = it->second == "on" ? "false" : "true";
    rex::cvar::SetFlagByName("d3d12_allow_variable_refresh_rate_and_tearing", tear);
    rex::cvar::SetFlagByName("vulkan_allow_present_mode_immediate", tear);
    rex::cvar::SetFlagByName("vulkan_allow_present_mode_mailbox", tear);
  }
}

// The player's name and id (live_layers.cpp): each game its own, so no two players log in as the runtime's "User"
// (the backend renames the second, and a renamed player's game crashes joining). Made at the first start and saved:
// player_name (letters, digits and spaces, at most 15) and player_id (the XUID's low word). bf_player_name wins.
void SetPlayerIdentity(const std::string& name, uint32_t id);  // live_layers.cpp
void StartNameCheck(const std::string& name, uint32_t player_id);  // below
void ApplyPlayerIdentity(const fs::path& user_dir) {
  if (REXCVAR_GET(server)) return;  // a dedicated server keeps the runtime's profile
  Settings settings;
  settings.path = user_dir / "bf1943-settings.txt";
  settings.Load();
  auto clean = [](std::string name) {
    std::string out;
    for (char c : name)
      if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_') out += c;
    out.erase(0, out.find_first_not_of(' '));
    if (out.size() > 15) out.resize(15);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
  };
  std::random_device random;
  bool changed = false;
  std::string name = clean(settings.Get("player_name", ""));
  // A saved name with profanity (our filter, text_filter.h; TekRantGaming, 11 Nov 2026: "we can't allow them to have
  // the name at all. They'll have to select a new name"): never used. A made up one stands in, the name counts as not
  // chosen, so BF does not sign in before the launcher's name page has a new one (player_name_refused: that page shows
  // on its own, with why).
  if (!name.empty() && !text_filter::NameAllowed(name)) {
    REXLOG_INFO("BF name: the saved name is not allowed (profanity): the player chooses a new one");
    name.clear();
    settings.values["player_name_chosen"] = "";
    settings.values["player_name_refused"] = "1";
  }
  if (name.empty()) {
    name = "Soldier" + std::to_string(1000 + random() % 9000);
    settings.values["player_name"] = name;
    changed = true;
  }
  uint32_t id = 0;
  try {
    id = static_cast<uint32_t>(std::stoul(settings.Get("player_id", "0"), nullptr, 16));
  } catch (...) {
  }
  if (!id) {
    while (!id) id = random();
    char text[9];
    std::snprintf(text, sizeof(text), "%08X", id);
    settings.values["player_id"] = text;
    changed = true;
  }
  if (changed) settings.Save();
  if (const std::string forced = clean(REXCVAR_GET(bf_player_name)); !forced.empty()) {
    if (text_filter::NameAllowed(forced)) name = forced;
    else REXLOG_INFO("BF name: bf_player_name is not allowed (profanity): not used");
  }
  SetPlayerIdentity(name, id);
  StartNameCheck(name, id);
}

// Two players with the same name (TekRantGaming, 7 Nov 2026: "give them a number at the end"): before the game logs in, the
// backend's registry gives this player's name, or it with the smallest free number (TekRantGaming2), and holds it for this
// player's id (server/plasma.py free_name). Asked on a thread from the start; the answer is waited for (3 s at most)
// just before the game starts (bf1943_app.h OnPreLaunchModule), so the game only ever sees the one name. The
// backend's own rename at login is the last resort: a game that sees another name than the backend's crashes
// joining (live_layers.cpp). The saved name stays the player's; the number is for this session.
namespace {
struct NameCheck {
  std::mutex mutex;
  std::condition_variable done_cv;
  bool done = false;
  std::string answer;
  uint32_t id = 0;
};
std::shared_ptr<NameCheck> g_name_check;
}  // namespace
uint32_t OnlinePlayerId();          // live_layers.cpp
const std::string& PlayerName();   // live_layers.cpp
void StartNameCheck(const std::string& name, uint32_t player_id) {
  if (REXCVAR_GET(server) || REXCVAR_GET(bf_online_host).empty() || name.empty()) return;
  auto check = std::make_shared<NameCheck>();
  check->id = player_id;
  g_name_check = check;
  char id[9];
  std::snprintf(id, sizeof(id), "%08X", OnlinePlayerId());
  const std::string request = "{\"op\": \"name\", \"name\": \"" + name + "\", \"id\": \"" + id + "\"}";
  std::thread([check, request] {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    std::string reply, answer;
    if (RegistryRequest(request, reply)) answer = JsonField(reply, "name");
    std::lock_guard lock(check->mutex);
    check->answer = answer;
    check->done = true;
    check->done_cv.notify_all();
  }).detach();
}
uint32_t PlayerId();  // live_layers.cpp
void NameChosen(const std::string& name) {
  SetPlayerIdentity(name, PlayerId());
  StartNameCheck(name, PlayerId());
}
// The answer, if it is in (true then, or with abandon: the check is dropped, the name stays as chosen).
bool PollNameCheck(bool abandon) {
  const auto check = g_name_check;
  if (!check) return true;
  std::unique_lock lock(check->mutex);
  if (!check->done && !abandon) return false;
  const std::string answer = check->done ? check->answer : std::string();
  const bool done = check->done;
  lock.unlock();
  g_name_check.reset();
  if (!done) REXLOG_INFO("BF name: the backend did not answer in 3 s; playing as {}", PlayerName());
  else if (!answer.empty() && answer != PlayerName()) {
    REXLOG_INFO("BF name: {} is taken online; playing as {} this time", PlayerName(), answer);
    SetPlayerIdentity(answer, check->id);
  } else if (!answer.empty()) {
    REXLOG_INFO("BF name: {} is free online", answer);
  }
  return true;
}
void FinishNameCheck() {
  const auto check = g_name_check;
  if (!check) return;
  std::unique_lock lock(check->mutex);
  check->done_cv.wait_for(lock, std::chrono::seconds(3), [&] { return check->done; });
  const std::string answer = check->done ? check->answer : std::string();
  lock.unlock();
  g_name_check.reset();
  if (!check->done) {
    REXLOG_INFO("BF name: the backend did not answer in 3 s; playing as {}", PlayerName());
  } else if (answer.empty()) {
    REXLOG_INFO("BF name: no answer from the backend; playing as {}", PlayerName());
  } else if (answer != PlayerName()) {
    REXLOG_INFO("BF name: {} is taken online; playing as {} this time", PlayerName(), answer);
    SetPlayerIdentity(answer, check->id);
  } else {
    REXLOG_INFO("BF name: {} is free online", answer);
  }
}

bool GraphicsPanelOpen();               // f8_panel.cpp
void GraphicsPanelPad(uint16_t buttons);  // f8_panel.cpp
// Our menus or the graphics panel (F8) have the controller and the mouse.
bool GameMenuOpen() { return (g_menu && g_menu->open()) || GraphicsPanelOpen(); }
bool LauncherShowing() { return g_menu && g_menu->ShowingLauncher(); }
bool LauncherKeyboardLast() { return g_menu && g_menu->KeyboardLast(); }
// look.cpp (the vehicle mouse's stick): the game's own Vertical look is Inverted (GstInput Scheme2FlipY).
bool GameVerticalLookInverted() { return g_menu && g_menu->GameFlipY(); }
// auto_join.cpp (the game's pad reads) and look.cpp (the mouse): the device used in the match, for the HUD's prompts.
void NoteMatchDevice(bool keyboard) {
  if (g_menu && !g_menu->open()) g_menu->NoteMatchDevice(keyboard);
}
std::string MenuSetting(const std::string& key, const std::string& def) { return g_menu ? g_menu->Setting(key, def) : def; }
void SetMenuSetting(const std::string& key, const std::string& value) {
  if (g_menu) g_menu->SetSetting(key, value);
}
fs::path MenuUserDir() { return g_menu ? g_menu->UserDir() : fs::path(); }
// King Kong's rule exactly (its shader_prepared.h; TekRantGaming, 11 Nov 2026: shaders compile when the game files
// are first given, then never again until the game updates, when they are all compiled again; "Copy Kong exactly").
// At each start (bf1943_app.h, before the shaders are prepared and before the renderer opens its storage):
// - shaders prepared for another version: an update; the shader page makes every pipeline again, over the cache
//   (nothing deleted: update.cpp), "UPDATED TO VERSION";
// - prepared for this version (shaders_prepared_for, written once the page has compiled them all) and the renderer's
//   storage still there: no shader page; the renderer makes its stored pipelines behind the menus, from the driver's
//   cache (pipelines first met in play are kept in the storage and made the same way at the next start);
// - otherwise (a first start, a setup closed half way, a cache someone deleted): the shader page.
namespace {
std::atomic<bool> g_shaders_after_update{false};  // set at the start (the runtime's thread), read by the menu
std::atomic<bool> g_shaders_prepared{false};
}
void CheckShadersAfterUpdate(const fs::path& user_dir, const fs::path& cache) {
  Settings settings;
  settings.path = user_dir / "bf1943-settings.txt";
  settings.Load();
  const std::string prepared_for = settings.Get("shaders_prepared_for", "");
  g_shaders_after_update = update::PrepareShadersAfterUpdate(cache, prepared_for);
  std::error_code ec;
  const bool storage = fs::is_regular_file(cache / "native_x_renderer" / "5841097E.pipelines", ec);
  g_shaders_prepared = !g_shaders_after_update && prepared_for == BfVersion() && storage;
  if (g_shaders_prepared)
    REXLOG_INFO("BF shaders: prepared for v{} already: no shader page, the stored pipelines are made behind the menus",
                BfVersion());
  else if (!g_shaders_after_update)
    REXLOG_INFO("BF shaders: not prepared for v{} yet ({}): the shader page compiles them", BfVersion(),
                prepared_for.empty() ? "never prepared: a first start, or a setup closed before they were done"
                                     : "the renderer's storage is gone");
}
bool ShadersAfterUpdate() { return g_shaders_after_update; }
bool ShadersPreparedAtStart() { return g_shaders_prepared; }
// For auto_join.cpp: BF signs in by itself behind the launcher (Start through its films and title) once allowed.
bool LauncherLetsGameSignIn() { return g_menu && LauncherWalks().load(std::memory_order_relaxed); }
// The first start's setup (bf1943_app.h, OnFinalizePaths): shown until the game is installed.
void BeginGameSetup(const fs::path& game_dir, std::function<void(fs::path)> done) {
  if (g_menu) g_menu->BeginSetup(game_dir, std::move(done));
}
// Whether the first start asks for the player's name (bf1943_app.h OnFinalizePaths): they never chose one, our menu
// is on, and no name is forced (bf_player_name; test runs).
bool NameNeeded(const fs::path& user_dir) {
  // Since 8 Nov 2026 the name is the launcher's page 3, shown while the game boots (game_menu_launcher.inc): BF signs
  // in only once it is chosen (LauncherWalks).
  return false;
  if (!g_menu || REXCVAR_GET(server) || !REXCVAR_GET(bf_launcher) || !REXCVAR_GET(bf_player_name).empty()) return false;
#if defined(BF_DEV_TOOLS)
  const char* dev_name = std::getenv("BF_DEV_NAME");
  if (const char* v = std::getenv("BF_AUTO_JOIN"); v && *v == '1' && !(dev_name && *dev_name)) return false;
#endif
  Settings settings;
  settings.path = user_dir / "bf1943-settings.txt";
  settings.Load();
  return settings.Get("player_name_chosen", "") != "1";
}
void BeginNameSetup(std::function<void()> done) {
  if (g_menu) g_menu->BeginName(std::move(done));
}
// Whether a player's start shows the launcher before the game (bf1943_app.h): our menu on, not a server, and not a
// test that joins by itself (BF_AUTO_JOIN=1 without BF_LAUNCHER_TEST=1).
bool GameMenuEnabled();
bool LauncherFirst() {
  const char* a = std::getenv("BF_AUTO_JOIN");
  const char* t = std::getenv("BF_LAUNCHER_TEST");
  return g_menu && GameMenuEnabled() && !(a && *a == '1' && !(t && *t == '1'));
}
void BeginLauncher(std::function<void()> start) {
  if (g_menu) g_menu->BeginLauncher(std::move(start));
}
// The game folder the player chose at setup (an unpacked game used where it is), or empty.
fs::path SavedGameDir(const fs::path& user_dir) {
  Settings settings;
  settings.path = user_dir / "bf1943-settings.txt";
  settings.Load();
  return settings.Get("game_dir", "");
}
bool GameMenuEnabled() { return REXCVAR_GET(bf_launcher) && !REXCVAR_GET(server); }
// For perf_overlay.cpp: 0 bold, 1 semibold.
ImFont* MenuFont(int which) { return which == 0 ? g_fonts.bold : g_fonts.item; }
void GameMenuPad(uint16_t buttons, int16_t lx, int16_t ly) {
  GraphicsPanelPad(buttons);  // L3 with R3 toggles it; open, the controller is its own
  if (GraphicsPanelOpen()) return;
  if (g_menu) g_menu->Pad(buttons, lx, ly, true);  // closed: it only watches for the way back from BF's pause screens
}
bool GameMenuPassing() { return g_menu && g_menu->passing(); }
void NoteGameMenuSignal() {
  const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
  const int64_t last = g_game_menu_signal_at.exchange(now, std::memory_order_relaxed);
  native_x_renderer::NoteGameMenu();  // ultrawide: the 2D layer is a menu, kept together in the middle
  // Once each time one comes up (its calls come about once a second): in a match it should never be.
  if (!last || std::chrono::steady_clock::duration(now - last) > std::chrono::seconds(3))
    REXLOG_INFO("BF menu: the game's own menu screen is up (its couch call){}", InMatch() ? ", in a match" : "");
}
bool TakeEscapePress() { return g_escape_press.exchange(false, std::memory_order_relaxed); }
// Tests (the match script's "esc"): the keyboard's Esc as our menu sees it with the menu closed.
void PressEscapeForTest() { g_escape_press.store(true, std::memory_order_relaxed); }
void ShowGameMenu() {
  if (g_menu) g_menu->Show();
}
void ShowPauseMenu() {
  if (g_menu) g_menu->ShowPause();
}
void ShowLostNotice() {
  if (g_menu) g_menu->ShowLost();
}

}  // namespace bf1943
