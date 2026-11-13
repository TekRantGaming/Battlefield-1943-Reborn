// The overlays and hooks that put a DLL of theirs into the game's process and sit on its draws and presents: RivaTuner
// Statistics Server (the frame counter and overlay of MSI Afterburner and others), OBS Studio's game capture, and the
// Steam, Epic Games and Discord overlays. A player's v0.3.0 crashed at its very first frame inside RTSSHooks64.dll, 8
// starts of 8, and neither the log nor the crash report named it (12 Nov 2026). This is the table they are known by:
// crash_report.cpp logs them at the start, when one arrives later and at the first game frame, and names them in a
// crash report; the launcher's Report an issue page (ReportPage, game_menu_launcher.inc) says what to do about one.
// Included at namespace scope by crash_report.cpp and inside ReportPage's body (the menu's source was being rebuilt by
// another hand as this was written, so the page's edit stays within that one function): only types, constants and a
// lambda, which fit both places. Windows only; <windows.h>, <string> and <vector> come first.

struct OverlayHook {
  const wchar_t* file;  // the DLL as the process loads it (GetModuleHandleW finds it by this name, wherever it is from)
  const char* product;  // what the player knows it as
};
static constexpr OverlayHook kOverlayHooks[] = {  // static: usable by the lambda below in a function too
    {L"RTSSHooks64.dll", "RivaTuner Statistics Server"},    // its hook, in every game it watches; MSI Afterburner's too
    {L"RTSSVkLayer64.dll", "RivaTuner Statistics Server"},  // its Vulkan layer (VK_LAYER_RTSS)
    {L"graphics-hook64.dll", "OBS Studio"},                 // game capture (VK_LAYER_OBS_HOOK)
    {L"GameOverlayRenderer64.dll", "Steam"},                // the Steam overlay, in every game Steam starts
    {L"EOSOVH-Win64-Shipping.dll", "Epic Games"},           // the Epic Games overlay
    {L"DiscordHook64.dll", "Discord"},                      // Discord's in game overlay
};

// One of them in the process: its product, and the DLLs of its that are there ("RTSSHooks64.dll, RTSSVkLayer64.dll").
struct HookedOverlay {
  std::string product;
  std::string files;
};

// The overlays hooked into the process now, each product once, in the table's order. Developer builds:
// BF_DEV_OVERLAYS=<file,file> (tests) counts those DLLs as loaded.
const auto HookedOverlays = []() -> std::vector<HookedOverlay> {
  std::vector<HookedOverlay> out;
#if defined(BF_DEV_TOOLS)
  const char* pretend = std::getenv("BF_DEV_OVERLAYS");
  const std::string pretended = pretend && *pretend ? std::string(",") + pretend + "," : std::string();
#endif
  for (const OverlayHook& h : kOverlayHooks) {
    std::string file;  // the names are ASCII
    for (const wchar_t* c = h.file; *c; ++c) file += static_cast<char>(*c);
    bool loaded = GetModuleHandleW(h.file) != nullptr;
#if defined(BF_DEV_TOOLS)
    if (pretended.find("," + file + ",") != std::string::npos) loaded = true;
#endif
    if (!loaded) continue;
    if (!out.empty() && out.back().product == h.product) out.back().files += ", " + file;
    else out.push_back({h.product, file});
  }
  return out;
};
