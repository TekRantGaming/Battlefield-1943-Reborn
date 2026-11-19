// Sharper text in the game's own HUD and menus (the user, 19 Nov 2026: "Can we increase the resolution quality of the
// font?").
//
// The game draws its text from glyph atlases made for 720p (Fonts/<language>/<name>_texture.res: 8 bit, tiled;
// bf_font.cpp reads the same ones for our menus), so at 1440p every letter is a 720p glyph stretched twice as large:
// soft edges. NativeXRenderer's upscaler (D200) hands each 8 bit texture the game loads to Upscale below; a font atlas
// (its data the same as one of the font textures in the player's game, read once at the start on a worker: every
// Output/xenon/Fonts/.../*_texture.res in build_overlay-00.fbrb) gets a copy `scale` times as large: each glyph's
// coverage enlarged smoothly (Catmull-Rom, a row pass then a column pass) and its edge brought back to about one
// screen pixel (the coverage around one half stretched by the scale), so the letters keep their shapes and lose the
// blur. The scale follows the screen's height (720 lines a step: 2 at 1080 and 1440, 3 at 2160, 4 at most). Nothing
// of the game's is shipped or stored: the copy is made from the atlas in memory, each start.
//
// The icons the same way (the user, 19 Nov 2026: "Can the icons be done the same way too?"): the HUD's, the maps' and
// the menus' pictures are in the five colour atlases hud_glyphs.cpp patches (prompts, flag letters, kit badges), and
// the crosshairs, the minimap's compass and two icons in pictures of their own (kPictures, UpscaleImage). Where
// the HD pack has no picture for one, NativeXRenderer asks UpscaleRgba (D200) for a larger one, and the patcher then
// draws its own pictures into that, as into a pack's. Colour is enlarged premultiplied by its coverage (no dark rims);
// the coverage's edge is brought back to about a screen pixel only where the game's own edge is a sharp one (its
// coverage changing by a third or more from a texel to the next), and between the lowest and the highest coverage
// around it (3 x 3 texels): the soft glows and shadows stay soft, and a dark rim beside an icon's edge stays (run
// sharp3: tightened about one half, the flags' and the vehicles' rims were gone).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <native_x_renderer/native_x_renderer.h>
#include <rex/cvar.h>
#include <rex/logging.h>

// Enhancements > Textures > HUD scale (the user, 19 Nov 2026: "simply by toggling a 'HUD Scale' option to 'High-Res'
// from 'Original (Xbox 360)' ... we're not actually including the higher-res assets in our client, the clients build
// them from their own files"). The copies are always made (at a level's load, from what the game loaded); the option
// only says which is drawn, so it applies at once either way.
REXCVAR_DEFINE_STRING(bf_hud_scale, "original", "BF1943",
                      "The game's text and HUD icons: original (Xbox 360, made for 720p) or high (sharper copies built "
                      "from the game's own, at the screen's resolution)");

namespace bf1943 {

bool ReadFbrbMatching(const std::filesystem::path& archive, const std::string& prefix, const std::string& suffix,
                      std::map<std::string, std::vector<uint8_t>>& out);  // shader_prep.cpp

namespace {

namespace fs = std::filesystem;

std::mutex g_mutex;
std::condition_variable g_ready_cv;
bool g_ready = false;
std::map<uint64_t, std::string> g_fonts;  // a font texture's data as the game holds it (FNV-1a 64, its size) -> its name

uint64_t Fnv(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}
uint64_t Key(const uint8_t* tiled, uint32_t w, uint32_t h) { return Fnv(tiled, size_t(w) * h) ^ (uint64_t(w) << 32) ^ h; }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// XGAddress2DTiledOffset for one byte a texel (bf_font.cpp's TiledOffset with tp 1).
uint32_t Tiled8(uint32_t x, uint32_t y, uint32_t width) {
  const uint32_t aligned = (width + 31) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << 7;
  const uint32_t micro = (x & 7) + ((y & 6) << 2);
  const uint32_t off = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << 3) + ((y & 1) << 4);
  return ((off & ~511u) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

// The same for four bytes a texel (an 8_8_8_8 texture's).
uint32_t Tiled32(uint32_t x, uint32_t y, uint32_t width) {
  const uint32_t aligned = (width + 31) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << 9;
  const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << 2;
  const uint32_t off = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << 5) + ((y & 1) << 4);
  return (((off & ~511u) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> 2;
}

// The game's font textures (bf_font.cpp's reading: an optional 0x40 byte "RES" wrapper, then a 0x5C byte header with
// the format, 10 for these 8 bit ones, at +8, the width at +16 and the height at +20; the texels after it, tiled).
void LoadFonts(const fs::path& game_dir) {
  std::map<std::string, std::vector<uint8_t>> entries;
  std::map<uint64_t, std::string> fonts;
  if (!ReadFbrbMatching(game_dir / "dist" / "xenon" / "build_overlay-00.fbrb", "Output/xenon/Fonts/", "_texture.res",
                        entries))
    REXLOG_WARN("BF sharp text: the game's font archive could not be read");
  for (const auto& [name, tex] : entries) {
    const size_t at = tex.size() >= 3 && std::memcmp(tex.data(), "RES", 3) == 0 ? 0x40 : 0;
    if (tex.size() < at + 0x5C) continue;
    const uint8_t* hd = &tex[at];
    const uint32_t fmt = Be32(hd + 8), w = Be32(hd + 16), h = Be32(hd + 20);
    if (fmt != 10 || !w || !h || w > 4096 || h > 4096 || tex.size() < at + 0x5C + size_t(w) * h) continue;
    const size_t slash = name.rfind('/');
    fonts[Key(hd + 0x5C, w, h)] = name.substr(slash + 1, name.size() - slash - 1 - 12);  // <name>_texture.res
  }
  REXLOG_INFO("BF sharp text: {} of the game's font textures known", fonts.size());
  std::lock_guard lock(g_mutex);
  g_fonts = std::move(fonts);
  g_ready = true;
  g_ready_cv.notify_all();
}

int ScreenScale() {
  int lines = 1440;
#if defined(_WIN32)
  if (const int h = GetSystemMetrics(SM_CYSCREEN); h > 0) lines = h;
#endif
  return std::clamp((lines + 719) / 720, 2, 4);
}

// Catmull-Rom weights for the fraction t.
void Weights(float t, float (&w)[4]) {
  const float t2 = t * t, t3 = t2 * t;
  w[0] = 0.5f * (-t3 + 2 * t2 - t);
  w[1] = 0.5f * (3 * t3 - 5 * t2 + 2);
  w[2] = 0.5f * (-3 * t3 + 4 * t2 + t);
  w[3] = 0.5f * (t3 - t2);
}

// `src` (w x h, 0..1) `s` times as large along x (rows) into `dst` (w*s x h).
void Rows(const std::vector<float>& src, uint32_t w, uint32_t h, int s, std::vector<float>& dst) {
  const uint32_t ow = w * s;
  dst.assign(size_t(ow) * h, 0.0f);
  for (uint32_t x = 0; x < ow; ++x) {
    const float at = (x + 0.5f) / s - 0.5f;
    const int i = int(std::floor(at));
    float wt[4];
    Weights(at - i, wt);
    int idx[4];
    for (int k = 0; k < 4; ++k) idx[k] = std::clamp(i - 1 + k, 0, int(w) - 1);
    for (uint32_t y = 0; y < h; ++y) {
      const float* row = &src[size_t(y) * w];
      dst[size_t(y) * ow + x] = row[idx[0]] * wt[0] + row[idx[1]] * wt[1] + row[idx[2]] * wt[2] + row[idx[3]] * wt[3];
    }
  }
}

// The same along y (columns): `src` (w x h) into `dst` (w x h*s).
void Columns(const std::vector<float>& src, uint32_t w, uint32_t h, int s, std::vector<float>& dst) {
  const uint32_t oh = h * s;
  dst.assign(size_t(w) * oh, 0.0f);
  for (uint32_t y = 0; y < oh; ++y) {
    const float at = (y + 0.5f) / s - 0.5f;
    const int i = int(std::floor(at));
    float wt[4];
    Weights(at - i, wt);
    const float* r[4];
    for (int k = 0; k < 4; ++k) r[k] = &src[size_t(std::clamp(i - 1 + k, 0, int(h) - 1)) * w];
    float* out = &dst[size_t(y) * w];
    for (uint32_t x = 0; x < w; ++x) out[x] = r[0][x] * wt[0] + r[1][x] * wt[1] + r[2][x] * wt[2] + r[3][x] * wt[3];
  }
}

bool Upscale(uint32_t width, uint32_t height, const uint8_t* guest, std::vector<uint8_t>& out, uint32_t& scale) {
  if (width > 4096 || height > 4096) return false;
  const uint64_t key = Key(guest, width, height);
  std::string name;
  {
    // The fonts are read on a worker from the start; the game's first fonts may come before that is done (once).
    std::unique_lock lock(g_mutex);
    g_ready_cv.wait_for(lock, std::chrono::seconds(5), [] { return g_ready; });
    const auto it = g_fonts.find(key);
    if (it == g_fonts.end()) {
#if defined(BF_DEV_TOOLS)
      static const bool dev = std::getenv("BF_DEV_SHARPLOG") != nullptr;
      if (dev) REXLOG_INFO("BF sharp text: an 8 bit {}x{} texture, no font of the game's ({:016X})", width, height, key);
#endif
      return false;
    }
    name = it->second;
  }
  std::vector<float> a(size_t(width) * height), wide, tall;
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x) a[size_t(y) * width + x] = guest[Tiled8(x, y, width)] / 255.0f;
  const int s = ScreenScale();
  Rows(a, width, height, s, wide);
  Columns(wide, width * s, height, s, tall);
  // The edge: the enlarged coverage around one half stretched by 0.85 times the scale (an edge about 1.2 pixels wide
  // at the screen's size), so a letter's outline is where the game's coverage is half.
  const float gain = 0.85f * float(s);
  out.resize(tall.size());
  for (size_t i = 0; i < tall.size(); ++i)
    out[i] = uint8_t(std::clamp((tall[i] - 0.5f) * gain + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
  scale = uint32_t(s);
  REXLOG_INFO("BF sharp text: the game's {} font ({}x{}) drawn from a copy {} times as large", name, width, height, s);
  return true;
}

// A picture (R, G, B, A rows, `width` x `height`) `scale` times as large into `rgba`, as the top says.
void Enlarge(const std::vector<uint8_t>& src, uint32_t width, uint32_t height, std::vector<uint8_t>& rgba,
             uint32_t& scale) {
  const size_t n = size_t(width) * height;
  // Colour premultiplied, a plane a channel; and where its edges are sharp.
  std::vector<float> plane[4], sharp(n), lo(n), hi(n);
  for (auto& p : plane) p.resize(n);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t* t = &src[i * 4];
    const float a = t[3] / 255.0f;
    plane[3][i] = a;
    for (int c = 0; c < 3; ++c) plane[c][i] = t[c] / 255.0f * a;
  }
  auto at = [&](int x, int y) {
    return plane[3][size_t(std::clamp(y, 0, int(height) - 1)) * width + std::clamp(x, 0, int(width) - 1)];
  };
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x) {
      const float a = at(int(x), int(y));
      const float step = std::max({std::fabs(at(int(x) + 1, int(y)) - a), std::fabs(at(int(x) - 1, int(y)) - a),
                                   std::fabs(at(int(x), int(y) + 1) - a), std::fabs(at(int(x), int(y) - 1) - a)});
      sharp[size_t(y) * width + x] = std::clamp((step - 0.15f) / 0.2f, 0.0f, 1.0f);
      float l = a, h = a;
      for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) l = std::min(l, at(int(x) + i, int(y) + j)), h = std::max(h, at(int(x) + i, int(y) + j));
      lo[size_t(y) * width + x] = l, hi[size_t(y) * width + x] = h;
    }
  const int s = ScreenScale();
  const uint32_t ow = width * s, oh = height * s;
  std::vector<float> big[7], wide;
  for (int c = 0; c < 4; ++c) {
    Rows(plane[c], width, height, s, wide);
    Columns(wide, ow, height, s, big[c]);
  }
  Rows(sharp, width, height, s, wide);
  Columns(wide, ow, height, s, big[4]);
  Rows(lo, width, height, s, wide);
  Columns(wide, ow, height, s, big[5]);
  Rows(hi, width, height, s, wide);
  Columns(wide, ow, height, s, big[6]);
  const float gain = 0.85f * float(s);
  rgba.resize(size_t(ow) * oh * 4);
  for (size_t i = 0; i < size_t(ow) * oh; ++i) {
    const float a = std::clamp(big[3][i], 0.0f, 1.0f);
    const float k = std::clamp(big[4][i], 0.0f, 1.0f);
    const float l = std::clamp(big[5][i], 0.0f, 1.0f), h = std::max(l + 0.001f, std::clamp(big[6][i], 0.0f, 1.0f));
    const float edge = l + (h - l) * std::clamp(((a - l) / (h - l) - 0.5f) * gain + 0.5f, 0.0f, 1.0f);
    const float out_a = a + (edge - a) * k;
    uint8_t* o = &rgba[i * 4];
    for (int c = 0; c < 3; ++c)
      o[c] = uint8_t(std::clamp(a > 0.004f ? big[c][i] / a : 0.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
    o[3] = uint8_t(out_a * 255.0f + 0.5f);
  }
  scale = uint32_t(s);
}

// A colour atlas the patcher took (D200): its texels (A, R, G, B bytes, tiled) as R, G, B, A rows `scale` times as
// large.
bool UpscaleRgba(uint32_t width, uint32_t height, const uint8_t* guest, std::vector<uint8_t>& rgba, uint32_t& scale) {
  if (width > 2048 || height > 2048) return false;
  std::vector<uint8_t> src(size_t(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* t = guest + size_t(Tiled32(x, y, width)) * 4;
      uint8_t* d = &src[(size_t(y) * width + x) * 4];
      d[0] = t[1], d[1] = t[2], d[2] = t[3], d[3] = t[0];
    }
  Enlarge(src, width, height, rgba, scale);
  REXLOG_INFO("BF sharp icons: a {}x{} atlas drawn from a copy {} times as large", width, height, scale);
  return true;
}

// The other pictures (the user, 19 Nov 2026: the crosshair and the minimap's compass too), known by their data as the
// game loads them (run texdump1, Guadalcanal): the crosshairs' sheet (8_8_8_8, 256 x 256: the rings' arcs, the
// sights' pieces, the hit marker), the minimap's compass (DXT5, 256 x 256: its ticks and N, E, S, W) and two 64 x 64
// icons (8_8_8_8: the reload arrow, a down arrow).
struct Known {
  uint32_t format, width, height;
  uint64_t fnv;  // FNV-1a 64 of its guest data
  const char* name;
};
constexpr Known kPictures[] = {
    {6, 256, 256, 0x37FA55FDAF87DC93ull, "crosshairs"},
    {20, 256, 256, 0xF6AA16E889E2CA3Cull, "compass"},
    {6, 64, 64, 0x401D5027E4B24A5Cull, "reload icon"},
    {6, 64, 64, 0x1785006DD08501FAull, "arrow icon"},
};

// The Xbox 360's tiled layout for `bytes` a texel or block (shader_prep.cpp's TiledOffset).
uint32_t Tiled(uint32_t x, uint32_t y, uint32_t width, uint32_t bytes) {
  const uint32_t aligned = (width + 31) & ~31u;
  const uint32_t lb = (bytes >> 2) + ((bytes >> 1) >> (bytes >> 2));
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (lb + 7);
  const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << lb;
  const uint32_t off = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4);
  return (((off & ~511u) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> lb;
}

// A DXT5 block (the guest's: 16 bit words big endian) into 4 x 4 R, G, B, A texels of `out` (w wide) at (x0, y0).
void Dxt5(const uint8_t* g, uint8_t* out, uint32_t w, uint32_t h, uint32_t x0, uint32_t y0) {
  uint8_t b[16];
  for (int i = 0; i < 16; i += 2) b[i] = g[i + 1], b[i + 1] = g[i];
  uint8_t al[8] = {b[0], b[1]};
  if (b[0] > b[1]) {
    for (int k = 2; k < 8; ++k) al[k] = uint8_t(((8 - k) * b[0] + (k - 1) * b[1]) / 7);
  } else {
    for (int k = 2; k < 6; ++k) al[k] = uint8_t(((6 - k) * b[0] + (k - 1) * b[1]) / 5);
    al[6] = 0, al[7] = 255;
  }
  uint64_t abits = 0;
  for (int i = 0; i < 6; ++i) abits |= uint64_t(b[2 + i]) << (8 * i);
  const uint32_t c0 = b[8] | b[9] << 8, c1 = b[10] | b[11] << 8;
  const uint32_t bits = b[12] | b[13] << 8 | b[14] << 16 | uint32_t(b[15]) << 24;
  int col[4][3];
  for (int k = 0; k < 2; ++k) {
    const uint32_t c = k ? c1 : c0;
    col[k][0] = int((c >> 11 & 31) * 255 / 31), col[k][1] = int((c >> 5 & 63) * 255 / 63);
    col[k][2] = int((c & 31) * 255 / 31);
  }
  for (int c = 0; c < 3; ++c) col[2][c] = (2 * col[0][c] + col[1][c]) / 3, col[3][c] = (col[0][c] + 2 * col[1][c]) / 3;
  for (int j = 0; j < 16; ++j) {
    const uint32_t x = x0 + j % 4, y = y0 + j / 4;
    if (x >= w || y >= h) continue;
    uint8_t* o = &out[(size_t(y) * w + x) * 4];
    const int* c = col[bits >> (2 * j) & 3];
    o[0] = uint8_t(c[0]), o[1] = uint8_t(c[1]), o[2] = uint8_t(c[2]), o[3] = al[abits >> (3 * j) & 7];
  }
}

// D200's image upscaler: one of the known pictures, as R, G, B, A rows `scale` times as large.
bool UpscaleImage(uint32_t width, uint32_t height, uint32_t format, bool tiled, const uint8_t* guest,
                  std::vector<uint8_t>& rgba, uint32_t& scale) {
  if (width > 1024 || height > 1024) return false;
  const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
  const size_t bytes = format == 6 ? size_t(width) * height * 4 : size_t(bw) * bh * 16;
  const uint64_t fnv = Fnv(guest, bytes);
  const Known* known = nullptr;
  for (const Known& k : kPictures)
    if (k.format == format && k.width == width && k.height == height && k.fnv == fnv) known = &k;
  if (!known) return false;
  std::vector<uint8_t> src(size_t(width) * height * 4);
  if (format == 6) {
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x) {
        const uint8_t* t = guest + size_t(tiled ? Tiled(x, y, width, 4) : y * width + x) * 4;
        uint8_t* d = &src[(size_t(y) * width + x) * 4];
        d[0] = t[1], d[1] = t[2], d[2] = t[3], d[3] = t[0];
      }
  } else {
    for (uint32_t by = 0; by < bh; ++by)
      for (uint32_t bx = 0; bx < bw; ++bx)
        Dxt5(guest + size_t(tiled ? Tiled(bx, by, bw, 16) : by * bw + bx) * 16, src.data(), width, height, bx * 4,
             by * 4);
  }
  Enlarge(src, width, height, rgba, scale);
  REXLOG_INFO("BF sharp icons: the game's {} ({}x{}) drawn from a copy {} times as large", known->name, width, height,
              scale);
  return true;
}

}  // namespace

// From the pad hook (auto_join.cpp): the option into NativeXRenderer (drawn at once).
void SharpTextTick() { native_x_renderer::SetUpscaledTextures(std::string(REXCVAR_GET(bf_hud_scale)) == "high"); }

void InstallSharpText(const std::filesystem::path& game_dir) {
  SharpTextTick();
  std::thread([game_dir] { LoadFonts(game_dir); }).detach();
  native_x_renderer::SetTextureUpscaler(&Upscale);
  native_x_renderer::SetTextureUpscalerRgba(&UpscaleRgba);
  native_x_renderer::SetTextureUpscalerImage(&UpscaleImage);
}

}  // namespace bf1943
