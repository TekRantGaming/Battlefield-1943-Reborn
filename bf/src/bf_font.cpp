// BF's own menu lettering for our rows (TekRantGaming, 6 Nov 2026: our rows must look as if they were always there).
//
// BF's menus draw their text in "Eurostile LT Std Condensed" (its font map, Fonts/us_fonts), from glyph atlases the
// game builds into build_overlay: Fonts/us/normal_large_texture.res (an Xbox 360 texture, 1024 x 512, 8 bit, tiled)
// and Fonts/us/normal_large_meta.dbx (each glyph's place in it, its offset and advance, and kerning pairs). They are
// read from the player's own installed game, never shipped: the first start unpacks them into the user folder
// (cache\bf_menu_font.bin), later starts read that. The atlas goes into ImGui's font texture as one custom
// rectangle, and BfText draws each glyph from it as an image.
//
// Measured against BF's own rows (runs origmenu3, 3200 x 1800; menu2c): BF draws this font with its advances at
// 0.88, at 1.55 atlas pixels per screen pixel at 1800 lines (BF's 720p menu, 2.5 times
// up). Three glyphs (W, % and @) have no entry in BF's metrics file although they are in the atlas, at its left edge
// and a little larger; W, the one our labels need, is placed from the atlas (scaled to the other capitals).

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/logging.h>

namespace bf1943 {

bool ReadFbrbEntry(const std::filesystem::path& archive, const std::string& name, std::vector<uint8_t>& out);  // shader_prep.cpp

namespace {

namespace fs = std::filesystem;

constexpr int kAtlasW = 1024, kAtlasH = 512;

struct Glyph {
  int x = 0, y = 0, w = 0, h = 0, min_x = 0, y_offset = 0, advance = 0;
  float scale = 1;  // drawn this much larger than its atlas size (W, from a larger set)
};

struct Font {
  bool tried = false, ok = false;
  std::map<int, Glyph> glyphs;
  std::map<std::pair<int, int>, int> kerning;
  std::vector<uint8_t> alpha;  // kAtlasW x kAtlasH, untiled
  ImFontAtlasRectId rect = ImFontAtlasRectId_Invalid;
} g_font;

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// XGAddress2DTiledOffset: the element index of (x, y) in a tiled Xenos texture, texel pitch tp bytes.
uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t width, uint32_t tp) {
  const uint32_t aligned = (width + 31) & ~31u;
  const uint32_t lb = (tp >> 2) + ((tp >> 1) >> (tp >> 2));
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (lb + 7);
  const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << lb;
  const uint32_t off = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4);
  return (((off & ~511u) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> lb;
}

// The metrics DBX ("{binary}"): a string table, then fields as 02 71 01 <name index> 01 04 <int32, big endian>.
// Glyph starts each glyph; First, Second and Kerning make a kerning pair.
bool ParseMeta(const std::vector<uint8_t>& b, Font& f) {
  if (b.size() < 24 || std::memcmp(b.data(), "{binary}", 8) != 0) return false;
  const uint32_t strings = Be32(&b[16]), count = Be32(&b[20]);
  const size_t base = 24 + size_t(count) * 4;
  if (base + strings > b.size()) return false;
  std::vector<std::string> names(count);
  for (uint32_t i = 0; i < count; ++i) {
    const size_t at = base + Be32(&b[24 + i * 4]);
    if (at >= b.size()) return false;
    names[i] = reinterpret_cast<const char*>(&b[at]);
  }
  Glyph* cur = nullptr;
  int first = -1, second = -1;
  for (size_t i = base + strings; i + 10 <= b.size(); ++i) {
    if (b[i] != 0x02 || b[i + 1] != 0x71 || b[i + 2] != 0x01 || b[i + 4] != 0x01 || b[i + 5] != 0x04) continue;
    if (b[i + 3] >= names.size()) continue;
    const std::string& n = names[b[i + 3]];
    const int v = int32_t(Be32(&b[i + 6]));
    i += 9;
    if (n == "Glyph") cur = &f.glyphs[v], first = second = -1;
    else if (n == "First") first = v, cur = nullptr;
    else if (n == "Second") second = v;
    else if (n == "Kerning" && first >= 0 && second >= 0) f.kerning[{first, second}] = v;
    else if (!cur) continue;
    else if (n == "X") cur->x = v;
    else if (n == "Y") cur->y = v;
    else if (n == "Width") cur->w = v;
    else if (n == "Rows") cur->h = v;
    else if (n == "MinX") cur->min_x = v;
    else if (n == "YOffset") cur->y_offset = v;
    else if (n == "Advance") cur->advance = v;
  }
  return f.glyphs.size() > 90;
}

// W: in the atlas at its left edge (x 0 to 38, y 37 to 73), drawn larger than the rest; scaled to the capitals
// (A: 31 rows, offset 10).
void AddW(Font& f) {
  if (f.glyphs.count('W')) return;
  Glyph w;
  w.x = 0, w.y = 37, w.w = 38, w.h = 36;
  w.scale = 31.0f / 34.0f;
  w.min_x = 0, w.y_offset = 10, w.advance = 31;
  f.glyphs['W'] = w;
}

// The cache: "BFMF", version 1, glyph count, glyphs (8 ints and a float), kerning count, pairs (3 ints), the atlas.
bool ReadCache(const fs::path& p, Font& f) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  char magic[4];
  uint32_t version = 0, n = 0;
  in.read(magic, 4).read(reinterpret_cast<char*>(&version), 4).read(reinterpret_cast<char*>(&n), 4);
  if (!in || std::memcmp(magic, "BFMF", 4) != 0 || version != 1 || n > 4096) return false;
  for (uint32_t i = 0; i < n; ++i) {
    int32_t v[8];
    float s;
    in.read(reinterpret_cast<char*>(v), sizeof(v)).read(reinterpret_cast<char*>(&s), 4);
    Glyph g;
    g.x = v[1], g.y = v[2], g.w = v[3], g.h = v[4], g.min_x = v[5], g.y_offset = v[6], g.advance = v[7], g.scale = s;
    f.glyphs[v[0]] = g;
  }
  uint32_t k = 0;
  in.read(reinterpret_cast<char*>(&k), 4);
  if (!in || k > 65536) return false;
  for (uint32_t i = 0; i < k; ++i) {
    int32_t v[3];
    in.read(reinterpret_cast<char*>(v), sizeof(v));
    f.kerning[{v[0], v[1]}] = v[2];
  }
  f.alpha.resize(size_t(kAtlasW) * kAtlasH);
  in.read(reinterpret_cast<char*>(f.alpha.data()), std::streamsize(f.alpha.size()));
  return bool(in) && f.glyphs.size() > 90;
}

void WriteCache(const fs::path& p, const Font& f) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  const uint32_t version = 1, n = uint32_t(f.glyphs.size()), k = uint32_t(f.kerning.size());
  out.write("BFMF", 4).write(reinterpret_cast<const char*>(&version), 4).write(reinterpret_cast<const char*>(&n), 4);
  for (const auto& [c, g] : f.glyphs) {
    const int32_t v[8] = {c, g.x, g.y, g.w, g.h, g.min_x, g.y_offset, g.advance};
    out.write(reinterpret_cast<const char*>(v), sizeof(v)).write(reinterpret_cast<const char*>(&g.scale), 4);
  }
  out.write(reinterpret_cast<const char*>(&k), 4);
  for (const auto& [pair, v] : f.kerning) {
    const int32_t e[3] = {pair.first, pair.second, v};
    out.write(reinterpret_cast<const char*>(e), sizeof(e));
  }
  out.write(reinterpret_cast<const char*>(f.alpha.data()), std::streamsize(f.alpha.size()));
}

bool LoadFromGame(const fs::path& game_dir, Font& f) {
  const fs::path archive = game_dir / "dist" / "xenon" / "build_overlay-00.fbrb";
  std::vector<uint8_t> meta, tex;
  if (!ReadFbrbEntry(archive, "Output/xenon/Fonts/us/normal_large_meta.dbx", meta) ||
      !ReadFbrbEntry(archive, "Output/xenon/Fonts/us/normal_large_texture.res", tex))
    return false;
  if (!ParseMeta(meta, f) || tex.size() < size_t(kAtlasW) * kAtlasH) return false;
  const uint8_t* data = tex.data() + tex.size() - size_t(kAtlasW) * kAtlasH;  // the texture after its header
  f.alpha.resize(size_t(kAtlasW) * kAtlasH);
  for (uint32_t y = 0; y < uint32_t(kAtlasH); ++y)
    for (uint32_t x = 0; x < uint32_t(kAtlasW); ++x) f.alpha[y * kAtlasW + x] = data[TiledOffset(x, y, kAtlasW, 1)];
  return true;
}

// The atlas into ImGui's font texture, once (it may move when ImGui grows its texture; GetCustomRect follows it).
bool EnsureRect() {
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  if (g_font.rect != ImFontAtlasRectId_Invalid) {
    ImFontAtlasRect r;
    if (atlas->GetCustomRect(g_font.rect, &r)) return true;
  }
  ImFontAtlasRect r;
  g_font.rect = atlas->AddCustomRect(kAtlasW, kAtlasH, &r);
  if (g_font.rect == ImFontAtlasRectId_Invalid || !atlas->TexData || !atlas->TexData->Pixels) return false;
  ImTextureData* tex = atlas->TexData;
  for (int y = 0; y < kAtlasH; ++y) {
    auto* row = static_cast<uint8_t*>(tex->GetPixelsAt(r.x, r.y + y));
    const uint8_t* src = &g_font.alpha[size_t(y) * kAtlasW];
    if (tex->Format == ImTextureFormat_Alpha8) {
      std::memcpy(row, src, kAtlasW);
    } else {
      for (int x = 0; x < kAtlasW; ++x) row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 255, row[x * 4 + 3] = src[x];
    }
  }
  return true;
}

// BF's drawing of its rows, per screen pixel at 1800 lines (see the top).
constexpr float kScaleAt1800 = 1.55f, kWide = 1.0f, kTrack = 0.88f;

}  // namespace

// Loads BF's lettering once: from the cache in the user folder, else from the player's game (then cached).
bool BfFontReady(const fs::path& game_dir, const fs::path& user_dir) {
  if (g_font.tried) return g_font.ok;
  g_font.tried = true;
  const fs::path cache = user_dir / "cache" / "bf_menu_font.bin";
  if (ReadCache(cache, g_font)) {
    g_font.ok = true;
  } else {
    g_font = Font{};
    g_font.tried = true;
    if (LoadFromGame(game_dir, g_font)) {
      AddW(g_font);
      WriteCache(cache, g_font);
      g_font.ok = true;
    }
  }
  REXLOG_INFO("BF menu font: {} ({} glyphs, {} kerning pairs)", g_font.ok ? "BF's own Eurostile" : "not found",
              g_font.glyphs.size(), g_font.kerning.size());
  return g_font.ok;
}

// The width of text in BF's lettering with capitals `cap` pixels high (BF's own rows: 42 at 1800 lines).
float BfTextWidth(const std::string& text, float cap) {
  if (!g_font.ok) return 0;
  const float s = cap / 42.0f * kScaleAt1800;
  float w = 0;
  int prev = -1;
  for (unsigned char c : text) {
    auto it = g_font.glyphs.find(c);
    if (prev >= 0)
      if (auto k = g_font.kerning.find({prev, c}); k != g_font.kerning.end()) w += k->second * s * kWide;
    w += (it == g_font.glyphs.end() ? 10 : it->second.advance) * s * kWide * kTrack;
    prev = c;
  }
  return w;
}

// BF's paper grain on a row (its bars are paper, speckled): a 256 x 64 tile of soft noise, drawn over the bar at
// a low strength, tiled along it.
void BfPaper(ImDrawList* d, ImVec2 a, ImVec2 b, float strength) {
  static ImFontAtlasRectId rect = ImFontAtlasRectId_Invalid;
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  ImFontAtlasRect r;
  if (rect == ImFontAtlasRectId_Invalid || !atlas->GetCustomRect(rect, &r)) {
    rect = atlas->AddCustomRect(256, 64, &r);
    if (rect == ImFontAtlasRectId_Invalid || !atlas->TexData || !atlas->TexData->Pixels) return;
    uint32_t seed = 0x9E3779B9u;
    ImTextureData* tex = atlas->TexData;
    for (int y = 0; y < 64; ++y) {
      auto* row = static_cast<uint8_t*>(tex->GetPixelsAt(r.x, r.y + y));
      for (int x = 0; x < 256; ++x) {
        // value noise on a 4 px grid: soft flecks, as BF's paper has (not single pixel speckles)
        auto cell = [](int cx, int cy) {
          uint32_t h = uint32_t(cx * 374761393 + cy * 668265263) ^ 0x9E3779B9u;
          h = (h ^ (h >> 13)) * 1274126177u;
          return int((h >> 24) & 0xFF);
        };
        const int cx = x / 4, cy = y / 4, fx = x % 4, fy = y % 4;
        const int top = (cell(cx, cy) * (4 - fx) + cell(cx + 1, cy) * fx) / 4;
        const int bottom = (cell(cx, cy + 1) * (4 - fx) + cell(cx + 1, cy + 1) * fx) / 4;
        const uint8_t v = uint8_t((top * (4 - fy) + bottom * fy) / 4);
        (void)seed;
        const uint8_t a8 = uint8_t(v > 180 ? (v - 180) * 2 : v < 70 ? (70 - v) * 2 : 0);
        if (tex->Format == ImTextureFormat_Alpha8) row[x] = a8;
        else row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = v > 200 ? 255 : 0, row[x * 4 + 3] = a8;
      }
    }
    atlas->GetCustomRect(rect, &r);
  }
  const float tile = (b.y - a.y) * 4.0f;
  for (float x = a.x; x < b.x; x += tile) {
    const float x1 = std::min(b.x, x + tile), f = (x1 - x) / tile;
    d->AddImage(atlas->TexRef, ImVec2(x, a.y), ImVec2(x1, b.y), r.uv0, ImVec2(r.uv0.x + (r.uv1.x - r.uv0.x) * f, r.uv1.y),
                IM_COL32(255, 255, 255, int(255 * strength)));
  }
}

// BF_FONT_DEBUG=1 (tests): the atlas as it is in ImGui's texture, at the top left.
void BfFontDebug(ImDrawList* d) {
  static const bool on = [] {
    const char* v = std::getenv("BF_FONT_DEBUG");
    return v && *v == '1';
  }();
  if (!on || !g_font.ok || !EnsureRect()) return;
  ImFontAtlasRect r;
  if (!ImGui::GetIO().Fonts->GetCustomRect(g_font.rect, &r)) return;
  d->AddRectFilled(ImVec2(0, 0), ImVec2(1024, 512), IM_COL32(0, 0, 0, 255));
  d->AddImage(ImGui::GetIO().Fonts->TexRef, ImVec2(0, 0), ImVec2(1024, 512), r.uv0, r.uv1);
}

// Text in BF's lettering: `top` is where BF's capitals start, `cap` their height in pixels.
void BfText(ImDrawList* d, ImVec2 top_left, float cap, ImU32 color, const std::string& text, ImU32 edge);
void BfText(ImDrawList* d, ImVec2 top_left, float cap, ImU32 color, const std::string& text) {
  BfText(d, top_left, cap, color, text, 0);
}
// With `edge`, the letters get BF's selected row's dark edge round them first.
void BfText(ImDrawList* d, ImVec2 top_left, float cap, ImU32 color, const std::string& text, ImU32 edge) {
  if (edge) {
    const float e = cap / 42.0f * 1.6f;
    for (const ImVec2 o : {ImVec2(-e, 0), ImVec2(e, 0), ImVec2(0, -e), ImVec2(0, e), ImVec2(e, e)})
      BfText(d, ImVec2(top_left.x + o.x, top_left.y + o.y), cap, edge, text, 0);
  }
  if (!g_font.ok || !EnsureRect()) return;
  ImFontAtlasRect r;
  if (!ImGui::GetIO().Fonts->GetCustomRect(g_font.rect, &r)) return;
  const float s = cap / 42.0f * kScaleAt1800;
  const float du = (r.uv1.x - r.uv0.x) / kAtlasW, dv = (r.uv1.y - r.uv0.y) / kAtlasH;
  // The capitals' top in the atlas is about 3 rows into a capital's cell (A: offset 10 + 3).
  float x = top_left.x;
  const float y0 = top_left.y - 13.0f * s;
  const float bold = cap / 42.0f * 0.0f, soft = cap / 42.0f * 0.9f;
  int prev = -1;
  for (unsigned char c : text) {
    auto it = g_font.glyphs.find(c);
    if (prev >= 0)
      if (auto k = g_font.kerning.find({prev, c}); k != g_font.kerning.end()) x += k->second * s * kWide;
    if (it == g_font.glyphs.end()) {
      x += 10 * s * kWide * kTrack;
      prev = c;
      continue;
    }
    const Glyph& g = it->second;
    const float gs = s * g.scale;
    const ImVec2 p0(x + g.min_x * s * kWide, y0 + g.y_offset * s), p1(p0.x + g.w * gs * kWide, p0.y + g.h * gs);
    const ImVec2 uv0(r.uv0.x + g.x * du, r.uv0.y + g.y * dv), uv1(r.uv0.x + (g.x + g.w) * du, r.uv0.y + (g.y + g.h) * dv);
    // BF's bold (its "propBold"): the glyph drawn again a little to the right, about 1.2 pixels at 1800 lines in
    // all, as Flash thickens text (runs menu2a to menu2c: same size as BF's letters, thinner strokes without it).
    // BF's bold (its "propBold"): the glyph drawn again a little to the right, 1.2 pixels at 1800 lines in all; then
    // a faint copy a pixel out on each side, as BF's letters are soft (its menu is drawn at 720p and scaled up).
    // Letter widths and gaps then match BF's own to a pixel (run menu2c, LEADERBOARDS).
    const float a = float((color >> 24) & 0xFF) / 255.0f;
    const ImU32 faint = (color & 0x00FFFFFF) | (uint32_t(255.0f * a * 0.14f) << 24);
    for (const ImVec2 o : {ImVec2(-soft, 0), ImVec2(bold + soft, 0), ImVec2(0, -soft), ImVec2(0, soft)})
      d->AddImage(ImGui::GetIO().Fonts->TexRef, ImVec2(p0.x + o.x, p0.y + o.y), ImVec2(p1.x + o.x, p1.y + o.y), uv0, uv1,
                  faint);
    for (float k : {0.0f, 0.5f, 1.0f})
      d->AddImage(ImGui::GetIO().Fonts->TexRef, ImVec2(p0.x + k * bold, p0.y), ImVec2(p1.x + k * bold, p1.y), uv0, uv1,
                  color);
    x += g.advance * s * kWide * kTrack;
    prev = c;
  }
}

}  // namespace bf1943
