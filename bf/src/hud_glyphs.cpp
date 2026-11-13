// The game's own button prompts in the chosen style (TekRantGaming, 8 Nov 2026: "button glyphs follow the setting in the game
// and the launcher, using Kong's lag-free approach"). Battlefield draws every controller prompt of its menus, pause
// and HUD from four uncompressed UI atlases (research in the session scratchpad glyph_research/notes.md: each picture's
// rectangle from the atlas's own metadata, atlas_meta.dbx, checked by eye):
//   Menu/menu_common_textures_Xenon   256x128  the menus' bottom bar ("A SELECT", "B BACK")
//   Menu/main_popups (= hud_popups)   128x128  dialogs
//   Menu/hud_root_cq_textures_Xenon   512x512  the HUD's hints (d-pad, sticks, triggers)
//   Menu/igm_mp_cq_textures_Xenon     512x256  the pause and deploy screens
// A fifth atlas has no prompts: menu/minimap/icons0 (512x256), the HUD's and the maps' icons. Its capture point flags
// get their letters here (capture_points.cpp: lettered copies drawn into its empty side), whatever the prompt style.
// King Kong found its sheet by scanning guest memory (its freezes, issue 11). Here nothing is scanned: NativeXRenderer
// hands each k_8_8_8_8 texture it loads (D142); one of those sizes is hashed, and a known atlas gets the chosen
// style's pictures (bf/glyphs, Xelu's and Kenney's CC0 prompts) drawn over its Xbox 360 ones. The Xbox 360 style
// leaves them as they are.
// The keyboard style names the key that presses each button where the player is (GitHub issue 1: in a landing boat
// "THROTTLE TO RELEASE BOAT" showed the mouse, the zoom key on foot, while W throttles there): of the controller
// buttons the remapping turns into that button, the keys bound to them, on foot or in a vehicle (input_settings.cpp's
// vehicle keys), the first one with a picture. The pad hook tells us the seat (HudGlyphsTick); when what the prompts
// show changes (the seat, the style, a key or a remap), NativeXRenderer offers the atlases again (D160) and they are
// drawn again before the next draw.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <native_x_renderer/native_x_renderer.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/image_decode.h>

#include "capture_points.h"

namespace bf1943 {

std::filesystem::path ExecutableDir();  // shader_prep.cpp
bool LauncherKeyboardLast();            // game_menu.cpp: the keyboard or mouse was used last in our menus
std::string PadFamily();                // game_menu.cpp: the controller in use (xbox_series, ps5, steam_deck)
// input_settings.cpp: the controller buttons the remapping turns into a game button, and the keys that press one.
std::vector<std::string> ButtonsPressing(const std::string& game_button);
std::vector<const char*> KeybindsPressing(const std::string& button, bool seated);

namespace {

std::atomic<bool> g_seated{false};  // the local soldier sits in a vehicle (HudGlyphsTick, from the pad hook)

struct Rect {
  const char* button;  // a, b, x, y, lb, rb, lt, rt, start, back, dpad_up/down/left/right, ls, rs, ls_click, ...
  int x, y, w, h;
};
struct Atlas {
  const char* name;
  uint32_t width, height;
  uint64_t hash;  // FNV-1a 64 of its guest data (width * height * 4 bytes)
  std::vector<Rect> rects;
};

constexpr const char* kFlagAtlas = "minimap_icons";

const std::vector<Atlas>& Atlases() {
  static const std::vector<Atlas> atlases = {
      {"menu_common", 256, 128, 0x1099BEF8DCBBF9FBull,
       {{"a", 86, 54, 26, 26}, {"b", 86, 28, 26, 26}, {"x", 90, 0, 26, 26}, {"y", 0, 88, 26, 26},
        {"start", 112, 46, 24, 22}, {"back", 86, 80, 24, 22}, {"lt", 0, 30, 28, 30}, {"rt", 28, 30, 28, 30},
        {"rs", 30, 0, 30, 30}, {"lb", 30, 60, 30, 24}, {"rb", 56, 30, 30, 24}}},
      {"popups", 128, 128, 0x25ACE164CCA0161Bull, {{"a", 0, 90, 26, 26}, {"b", 0, 64, 26, 26}, {"y", 26, 64, 26, 26}}},
      {"igm_mp_cq", 512, 256, 0x733675E0481EB2C5ull,
       {{"a", 106, 0, 26, 26}, {"b", 106, 26, 26, 26}, {"y", 88, 60, 26, 26}, {"rs", 76, 0, 30, 30},
        {"ls", 0, 88, 30, 30}}},
      {"hud_root_cq", 512, 512, 0x2E9696CC53BF9110ull,
       {{"start", 344, 178, 24, 22}, {"back", 292, 261, 24, 22}, {"y", 371, 28, 26, 26}, {"a", 374, 0, 26, 26},
        {"x", 318, 180, 26, 26}, {"b", 120, 340, 26, 26}, {"dpad_up", 212, 182, 30, 30},
        {"dpad_down", 264, 118, 30, 30}, {"dpad_left", 64, 294, 30, 30}, {"dpad_right", 203, 214, 30, 30},
        {"lt", 324, 102, 28, 30}, {"lb", 90, 340, 30, 24}, {"rt", 343, 28, 28, 30}, {"rb", 201, 275, 30, 24},
        {"ls_click", 141, 264, 30, 30}, {"rs_click", 262, 150, 30, 30}, {"ls_lr", 0, 312, 30, 30},
        {"ls_ud", 242, 180, 30, 30}, {"rs_lr", 313, 0, 30, 30}, {"rs_ud", 301, 72, 30, 30}, {"rs", 94, 294, 30, 30},
        {"ls", 30, 312, 30, 30}}},
      // The HUD's and the maps' icons: no prompts, the capture point flags' letters (kFlagAtlas).
      {kFlagAtlas, 512, 256, 0x189F8B5D47F80C8Full, {}},
  };
  return atlases;
}

uint64_t Fnv(const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

// Xenos 2D tiling (as bf_font.cpp's, for 4 byte texels): the texel (x, y)'s index in the tiled data.
uint32_t TiledIndex(uint32_t x, uint32_t y, uint32_t width) {
  const uint32_t tp = 4, aligned = (width + 31) & ~31u;
  const uint32_t lb = (tp >> 2) + ((tp >> 1) >> (tp >> 2));
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (lb + 7);
  const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << lb;
  const uint32_t off = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4);
  return (((off & ~511u) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >>
         lb;
}

struct Picture {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
};
std::mutex g_mutex;
std::map<std::string, Picture> g_pictures;
const Picture* Load(const std::string& set, const std::string& name) {
  const std::string key = set + "/" + name;
  auto it = g_pictures.find(key);
  if (it != g_pictures.end()) return it->second.rgba.empty() ? nullptr : &it->second;
  Picture p;
  std::ifstream f(ExecutableDir() / "glyphs" / set / (name + ".png"), std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (!bytes.empty()) p.rgba = rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), p.w, p.h);
  auto& slot = g_pictures[key] = std::move(p);
  return slot.rgba.empty() ? nullptr : &slot;
}

// A key's picture name: the SDK's key names (rex/ui keybinds.cpp), a few as the set calls them.
std::string PictureName(std::string_view key) {
  if (key == "Return") return "Enter";
  if (key == "Escape") return "Esc";
  if (key == "Control") return "Ctrl";
  return std::string(key);
}

// The key that presses a game button (an atlas rectangle's name) now, as a picture name, empty for none: of the
// controller buttons the remapping turns into it (the button itself first), the keys bound to them where the player
// is (on foot or in a vehicle), the first with a picture. A bind lists keys separated by commas ("Up,V": the first
// with a picture); a key that needs a modifier ("Shift+W") has no picture of its own and is passed over.
std::string KeyFor(const std::string& button, bool seated) {
  if (button == "ls" || button == "ls_lr" || button == "ls_ud") return "WASD";
  if (button == "rs" || button == "rs_lr" || button == "rs_ud") return "Mouse_Simple";
  std::string game = button;  // the atlas's names as input_settings.cpp's
  if (button.rfind("dpad_", 0) == 0) game = button.substr(5);
  if (button == "ls_click") game = "ls";
  if (button == "rs_click") game = "rs";
  for (const std::string& pad : ButtonsPressing(game))
    for (const char* bind : KeybindsPressing(pad, seated)) {
      const std::string keys = rex::cvar::GetFlagByName(bind);
      for (size_t at = 0; at <= keys.size();) {
        size_t comma = keys.find(',', at);
        if (comma == std::string::npos) comma = keys.size();
        std::string_view key(keys.data() + at, comma - at);
        at = comma + 1;
        while (!key.empty() && key.front() == ' ') key.remove_prefix(1);
        while (!key.empty() && key.back() == ' ') key.remove_suffix(1);
        if (key.empty() || key.find_first_of("+./\\") != std::string_view::npos) continue;  // a combination, no name
        const std::string name = PictureName(key);
        if (Load("keyboard", name)) return name;
      }
    }
  return {};
}

// The picture a button gets in a style: the set's own, else Xbox Series'. The d-pad's directions are its up turned.
struct Choice {
  std::string set, name;
  int turns = 0;
};
bool Choose(const std::string& set, const std::string& button, bool seated, Choice& out) {
  if (set == "keyboard") {
    if (std::string key = KeyFor(button, seated); !key.empty()) {
      out = {"keyboard", key, 0};
      return true;
    }
    return Choose("xbox_series", button, seated, out);
  }
  std::string name = button;
  int turns = 0;
  if (button.rfind("dpad_", 0) == 0) {
    name = "dpad_up";
    turns = button == "dpad_right" ? 1 : button == "dpad_down" ? 2 : button == "dpad_left" ? 3 : 0;
  } else if (button == "ls" || button == "rs") {
    name = "stick";
  } else if (button == "ls_click" || button == "rs_click") {
    name = "stick_click";
  } else if (button == "ls_lr" || button == "ls_ud" || button == "rs_lr" || button == "rs_ud") {
    name = "stick_move";
  }
  if (Load(set, name)) {
    out = {set, name, turns};
    return true;
  }
  return set != "xbox_series" && Choose("xbox_series", button, seated, out);
}

// `p` drawn into the rectangle, fitted and centred, bilinear, over a cleared rectangle.
void Draw(std::vector<uint8_t>& rgba, uint32_t width, const Rect& r, const Picture& p, int turns) {
  for (int y = r.y; y < r.y + r.h; ++y)
    for (int x = r.x; x < r.x + r.w; ++x) std::fill_n(&rgba[(size_t(y) * width + x) * 4], 4, uint8_t(0));
  const int pw = turns & 1 ? p.h : p.w, ph = turns & 1 ? p.w : p.h;
  const float k = std::min(float(r.w) / pw, float(r.h) / ph);
  const float dw = pw * k, dh = ph * k, ox = r.x + (r.w - dw) * 0.5f, oy = r.y + (r.h - dh) * 0.5f;
  auto texel = [&](int sx, int sy, int c) -> float {  // the picture turned by `turns` quarter turns clockwise
    sx = std::clamp(sx, 0, pw - 1), sy = std::clamp(sy, 0, ph - 1);
    int u = sx, v = sy;
    if (turns == 1) u = sy, v = p.h - 1 - sx;
    else if (turns == 2) u = p.w - 1 - sx, v = p.h - 1 - sy;
    else if (turns == 3) u = p.w - 1 - sy, v = sx;
    return p.rgba[(size_t(v) * p.w + u) * 4 + c];
  };
  for (int y = r.y; y < r.y + r.h; ++y)
    for (int x = r.x; x < r.x + r.w; ++x) {
      const float fx = (x + 0.5f - ox) / k - 0.5f, fy = (y + 0.5f - oy) / k - 0.5f;
      if (fx < -0.5f || fy < -0.5f || fx > pw - 0.5f || fy > ph - 0.5f) continue;
      const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
      const float ax = fx - x0, ay = fy - y0;
      uint8_t* out = &rgba[(size_t(y) * width + x) * 4];
      float px[4];
      for (int c = 0; c < 4; ++c) {
        const float top = texel(x0, y0, c) * (1 - ax) + texel(x0 + 1, y0, c) * ax;
        const float bottom = texel(x0, y0 + 1, c) * (1 - ax) + texel(x0 + 1, y0 + 1, c) * ax;
        px[c] = top * (1 - ay) + bottom * ay;
      }
      // premultiplied while filtering would be better; the glyphs' edges are soft enough
      for (int c = 0; c < 4; ++c) out[c] = uint8_t(std::clamp(px[c] + 0.5f, 0.0f, 255.0f));
    }
}

// The style for the game's prompts (bf_button_prompts; automatic follows the device used last in our menus).
std::string StyleSet() {
  std::string v = rex::cvar::GetFlagByName("bf_button_prompts");
  // The keyboard's keys whenever the keyboard or mouse was used last, whatever controller style was chosen; the
  // chosen style (or, automatic, the controller in use) as soon as the controller is used again (TekRantGaming, 12
  // Nov 2026; launcher_kit.cpp StyleFor is the same rule for our menus).
  if (LauncherKeyboardLast()) v = "keyboard";
  else if (v == "auto") v = PadFamily();  // the controller in use
  if (v == "ps3" || v == "ps2") return "ps2";  // King Kong's DualShock set
  if (v == "ps5" || v == "xbox_series" || v == "steam_deck" || v == "keyboard") return v;
  return {};  // Xbox 360: the game's own pictures
}

// What the prompts show now, every atlas's buttons in the style for the seat: drawn again when it changes (a seat
// changes only the keyboard's).
std::string Shown(const std::string& set, bool seated) {
  std::string shown = set.empty() ? "xbox360" : set;
  if (set.empty()) return shown;
  for (const Atlas& atlas : Atlases())
    for (const Rect& r : atlas.rects) {
      Choice c;
      shown += Choose(set, r.button, seated, c) ? " " + c.set + "/" + c.name + "/" + std::to_string(c.turns) : " -";
    }
  return shown;
}

// The game's texels of each atlas (A, R, G, B bytes, tiled) as RGBA, made once: an atlas is known by its data's hash.
std::map<const Atlas*, std::vector<uint8_t>> g_untouched;
const std::vector<uint8_t>& Untouched(const Atlas& atlas, const uint8_t* guest) {
  std::vector<uint8_t>& rgba = g_untouched[&atlas];
  if (!rgba.empty()) return rgba;
  rgba.resize(size_t(atlas.width) * atlas.height * 4);
  for (uint32_t y = 0; y < atlas.height; ++y)
    for (uint32_t x = 0; x < atlas.width; ++x) {
      const uint8_t* s = guest + size_t(TiledIndex(x, y, atlas.width)) * 4;
      uint8_t* d = &rgba[(size_t(y) * atlas.width + x) * 4];
      d[0] = s[1], d[1] = s[2], d[2] = s[3], d[3] = s[0];
    }
  return rgba;
}

// NativeXRenderer's patcher (D142): a known atlas gets the chosen style's pictures. Each one is taken in every style
// (D160), so a later style or seat draws it again (`again`: offered again, its data unchanged since, so it is not
// hashed again); in the Xbox 360 style a first load is left as it is and a texture offered again gets its own
// pictures back.
bool Patch(uint32_t width, uint32_t height, const uint8_t* guest, std::vector<uint8_t>& rgba, bool again) {
  const Atlas* atlas = nullptr;
  int sized = 0;
  for (const Atlas& a : Atlases())
    if (a.width == width && a.height == height) atlas = &a, ++sized;
  if (!atlas) return false;
  // Known by its data's hash: on a first load (another texture can have that size), and whenever two atlases share
  // the size (the pause screens' and the icons' are both 512 x 256).
  if (!again || sized > 1) {
    const uint64_t hash = Fnv(guest, size_t(width) * height * 4);
    atlas = nullptr;
    for (const Atlas& a : Atlases())
      if (a.width == width && a.height == height && a.hash == hash) atlas = &a;
    if (!atlas) {
#if defined(BF_DEV_TOOLS)
      if (!again && width == 512 && height == 256) REXLOG_INFO("BF prompts: a 512 x 256 texture that is no atlas of ours (hash {:016X})", hash);
#endif
      return false;
    }
  }
  if (atlas->name == kFlagAtlas) {  // the icons: the capture point flags' letters, in every prompt style
    std::lock_guard lock(g_mutex);
    rgba = Untouched(*atlas, guest);
    const int drawn = DrawFlagLetters(rgba, width, height);
    if (!drawn) return false;
    FlagLettersReady();
    for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);  // as below
    if (!again) REXLOG_INFO("BF flags: {} lettered flags drawn into the game's icon atlas", drawn);
    return true;
  }
  const std::string set = StyleSet();
  const bool seated = g_seated.load(std::memory_order_relaxed);
  std::lock_guard lock(g_mutex);
  if (set.empty() && !again) return true;
  rgba = Untouched(*atlas, guest);
  int drawn = 0;
  if (!set.empty())
    for (const Rect& r : atlas->rects) {
      Choice c;
      if (!Choose(set, r.button, seated, c)) continue;
      if (const Picture* p = Load(c.set, c.name)) Draw(rgba, width, r, *p, c.turns), ++drawn;
    }
  // The host texture holds each guest word byte swapped (B, G, R, A, as the loader writes it; run glyph1 showed the
  // flags red for blue with R, G, B, A).
  for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
  if (set.empty()) {
    REXLOG_INFO("BF prompts: the game's {} atlas has its own pictures again", atlas->name);
  } else {
    std::string keys;  // the keyboard's choices for the log: the HUD's hints are what the issue was about
    if (set == "keyboard")
      for (const char* b : {"a", "b", "x", "y", "lb", "rb", "lt", "rt", "dpad_up"}) {
        Choice c;
        keys += std::string(" ") + b + "=" + (Choose(set, b, seated, c) ? c.name : "-");
      }
    REXLOG_INFO("BF prompts: {} of {} buttons in the game's {} atlas in the {} style{}{}{}", drawn,
                atlas->rects.size(), atlas->name, set, seated ? ", seated" : "", keys.empty() ? "" : ":", keys);
  }
  return true;
}

}  // namespace

bool LocalSeatedNow() { return g_seated.load(std::memory_order_relaxed); }  // fov.cpp: the vehicle's own field of view

void InstallHudGlyphs() { native_x_renderer::SetTexturePatcher(&Patch); }

// From the pad hook (auto_join.cpp), at each of the game's pad reads: whether the local soldier sits in a vehicle. What
// the prompts show is checked at once when the seat changes and twice a second otherwise (a key, a remap or the style
// changed in our menus); a change has NativeXRenderer offer the atlases again (D160).
void HudGlyphsTick(bool seated) {
  using Clock = std::chrono::steady_clock;
  static std::mutex mutex;
  std::unique_lock lock(mutex, std::try_to_lock);  // the game reads the pad from more than one thread
  if (!lock.owns_lock()) return;
  static bool was_seated = false;
  static Clock::time_point checked{};
  static std::string shown;
  const auto now = Clock::now();
  if (seated == was_seated && checked.time_since_epoch().count() && now - checked < std::chrono::milliseconds(500))
    return;
  was_seated = seated, checked = now;
  g_seated.store(seated, std::memory_order_relaxed);
  std::string next;
  {
    std::lock_guard pictures(g_mutex);
    next = Shown(StyleSet(), seated);
  }
  if (next == shown) return;
  shown = std::move(next);
  REXLOG_INFO("BF prompts: the game's atlases drawn again in the {} style{}", StyleSet().empty() ? "xbox360" : StyleSet(),
              seated ? ", seated" : "");
  native_x_renderer::RepatchTextures();
}

}  // namespace bf1943
