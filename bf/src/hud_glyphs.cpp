// The game's own button prompts in the chosen style (TekRantGaming, 8 Nov 2026: "button glyphs follow the setting in the game
// and the launcher, using Kong's lag-free approach"). Battlefield draws every controller prompt of its menus, pause
// and HUD from four uncompressed UI atlases (research in the session scratchpad glyph_research/notes.md: each picture's
// rectangle from the atlas's own metadata, atlas_meta.dbx, checked by eye):
//   Menu/menu_common_textures_Xenon   256x128  the menus' bottom bar ("A SELECT", "B BACK")
//   Menu/main_popups (= hud_popups)   128x128  dialogs
//   Menu/hud_root_cq_textures_Xenon   512x512  the HUD's hints (d-pad, sticks, triggers)
//   Menu/igm_mp_cq_textures_Xenon     512x256  the pause and deploy screens
// King Kong found its sheet by scanning guest memory (its freezes, issue 11). Here nothing is scanned: NativeXRenderer
// hands each k_8_8_8_8 texture it loads (D142); one of those sizes is hashed, and a known atlas gets the chosen
// style's pictures (bf/glyphs, Xelu's and Kenney's CC0 prompts) drawn over its Xbox 360 ones. The Xbox 360 style
// leaves them as they are. A style chosen in a match shows from the next load of the atlas (the next menu or map).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <native_x_renderer/native_x_renderer.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/image_decode.h>

namespace bf1943 {

std::filesystem::path ExecutableDir();  // shader_prep.cpp
bool LauncherKeyboardLast();            // game_menu.cpp: the keyboard or mouse was used last in our menus
std::string PadFamily();                // game_menu.cpp: the controller in use (xbox_series, ps5, steam_deck)

namespace {

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

// The keyboard's key for a controller button (the SDK's keybind_* settings), as a picture name.
std::string KeyFor(const std::string& button) {
  static const std::map<std::string, const char*> kBinds = {
      {"a", "keybind_a"},           {"b", "keybind_b"},
      {"x", "keybind_x"},           {"y", "keybind_y"},
      {"lb", "keybind_left_shoulder"}, {"rb", "keybind_right_shoulder"},
      {"lt", "keybind_left_trigger"}, {"rt", "keybind_right_trigger"},
      {"start", "keybind_start"},   {"back", "keybind_back"},
      {"dpad_up", "keybind_dpad_up"}, {"dpad_down", "keybind_dpad_down"},
      {"dpad_left", "keybind_dpad_left"}, {"dpad_right", "keybind_dpad_right"},
      {"ls_click", "keybind_lstick_press"}, {"rs_click", "keybind_rstick_press"}};
  if (button == "ls" || button == "ls_lr" || button == "ls_ud") return "WASD";
  if (button == "rs" || button == "rs_lr" || button == "rs_ud") return "Mouse_Simple";
  auto it = kBinds.find(button);
  if (it == kBinds.end()) return {};
  std::string k = rex::cvar::GetFlagByName(it->second);
  if (k == "Return") k = "Enter";
  if (k == "Escape") k = "Esc";
  if (k == "Control") k = "Ctrl";
  return k;
}

// The picture for a button in a style: the set's own, else Xbox Series'. The d-pad's directions are its up turned.
const Picture* PictureFor(const std::string& set, const std::string& button, int& turns) {
  turns = 0;
  if (set == "keyboard") {
    if (const Picture* p = Load("keyboard", KeyFor(button))) return p;
    return PictureFor("xbox_series", button, turns);
  }
  std::string name = button;
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
  if (const Picture* p = Load(set, name)) return p;
  return set == "xbox_series" ? nullptr : PictureFor("xbox_series", button, turns);
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
  if (v == "auto") v = LauncherKeyboardLast() ? "keyboard" : PadFamily();  // the controller in use
  if (v == "ps3" || v == "ps2") return "ps2";  // King Kong's DualShock set
  if (v == "ps5" || v == "xbox_series" || v == "steam_deck" || v == "keyboard") return v;
  return {};  // Xbox 360: the game's own pictures
}

bool Patch(uint32_t width, uint32_t height, const uint8_t* guest, std::vector<uint8_t>& rgba) {
  const Atlas* atlas = nullptr;
  for (const Atlas& a : Atlases())
    if (a.width == width && a.height == height) atlas = &a;
  if (!atlas) return false;
  const std::string set = StyleSet();
  if (set.empty()) return false;
  if (Fnv(guest, size_t(width) * height * 4) != atlas->hash) return false;  // another texture of that size
  std::lock_guard lock(g_mutex);
  // The game's texels (A, R, G, B bytes, tiled) as RGBA, then the chosen style over its buttons.
  rgba.resize(size_t(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* s = guest + size_t(TiledIndex(x, y, width)) * 4;
      uint8_t* d = &rgba[(size_t(y) * width + x) * 4];
      d[0] = s[1], d[1] = s[2], d[2] = s[3], d[3] = s[0];
    }
  int drawn = 0;
  for (const Rect& r : atlas->rects) {
    int turns = 0;
    if (const Picture* p = PictureFor(set, r.button, turns)) Draw(rgba, width, r, *p, turns), ++drawn;
  }
  // The host texture holds each guest word byte swapped (B, G, R, A, as the loader writes it; run glyph1 showed the
  // flags red for blue with R, G, B, A).
  for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
  REXLOG_INFO("BF prompts: {} of {} buttons in the game's {} atlas in the {} style", drawn, atlas->rects.size(),
              atlas->name, set);
  return drawn > 0;
}

}  // namespace

void InstallHudGlyphs() { native_x_renderer::SetTexturePatcher(&Patch); }

}  // namespace bf1943
