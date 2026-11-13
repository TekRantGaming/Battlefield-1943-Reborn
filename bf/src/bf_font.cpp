// BF's own menu lettering for our rows (TekRantGaming, 6 Nov 2026: our rows must look as if they were always there), and
// since 12 Nov 2026 every font its menus have, for the launcher's new look (TekRantGaming: "extract anything from the game
// files and use it within these menus ... fonts").
//
// BF's menus draw their text from glyph atlases the game builds into build_overlay-00.fbrb: Fonts/us/<name>_texture.res
// (an Xbox 360 texture, 8 bit, tiled) and Fonts/us/<name>_meta.dbx (each glyph's place in it, its offset and advance,
// and kerning pairs). Its fonts (the source names are in each font's dbx; only the rendered atlases are in the
// package, at 720p sizes): headline_large, headline_medium, headline_small and headnumbers from Artbrush (brush
// capitals), normal_small, normal_medium, normal_large and custom1 from Eurostile LT Std Bold Condensed, instruction
// from its Bold Cn Italic, list_header from Century Schoolbook Bold Italic and list_item from Century Schoolbook.
// They are read from the player's own installed game, never shipped: the first start unpacks each into the user
// folder (cache\bf_font_<name>.bin; normal_large also as cache\bf_menu_font.bin, as before), later starts read that.
// Each atlas goes into ImGui's font texture as one custom rectangle, and BfText draws each glyph from it as an image.
//
// normal_large, measured against BF's own rows (runs origmenu3, 3200 x 1800; menu2c): BF draws it with its advances
// at 0.88, at 1.55 atlas pixels per screen pixel at 1800 lines (BF's 720p menu, 2.5 times up). Three glyphs (W, % and
// @) have no entry in its metrics file although they are in the atlas, at its left edge and a little larger; W, the
// one our labels need, is placed from the atlas (scaled to the other capitals). The other fonts are drawn plain: the
// capitals `cap` pixels high (the H's rows), their advances and kerning as the game's tables have them.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>  // ImFontAtlasTextureBlockQueueUpload
#include <rex/logging.h>

#include "dbx_tree.h"

namespace bf1943 {

bool ReadFbrbEntry(const std::filesystem::path& archive, const std::string& name, std::vector<uint8_t>& out);  // shader_prep.cpp
bool ReadFbrbEntries(const std::filesystem::path& archive, const std::vector<std::string>& names,
                     std::vector<std::vector<uint8_t>>& out);  // shader_prep.cpp: several at one read of the archive

namespace {

namespace fs = std::filesystem;

struct Glyph {
  int x = 0, y = 0, w = 0, h = 0, min_x = 0, y_offset = 0, advance = 0;
  float scale = 1;  // drawn this much larger than its atlas size (W, from a larger set)
};
// The glyph tables' X and Y are 3 texels up and left of each glyph's ink in every font (the font tool's padding):
// the atlas rect is (X + 3, Y + 3, Width, Rows). Without it the glyphs lost their last 3 rows and columns
// ("SERVER" without its feet, 12 Nov 2026).
constexpr int kInkShift = 3;

struct Font {
  bool tried = false, ok = false;
  int w = 0, h = 0;              // the atlas
  int cap_rows = 0, cap_top = 0;  // a capital's rows and its offset from the line's top (H, else A)
  std::map<int, Glyph> glyphs;
  std::map<std::pair<int, int>, int> kerning;
  std::vector<uint8_t> alpha;  // w x h, untiled
  ImFontAtlasRectId rect = ImFontAtlasRectId_Invalid;
};

constexpr const char* kLegacy = "normal_large";
std::map<std::string, Font> g_fonts;  // by the game's font name; the UI thread's (the worker writes only files)
std::mutex g_prepare_mutex;

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

// The metrics DBX ("{binary}", dbx_tree.h): one UI.UIFontGlyph instance a glyph (Glyph, X, Y, Width, Rows, MinX,
// YOffset, Advance) and one UI.UIFontGlyphKerning a kerning pair (First, Second, Kerning). (A pattern scan of the
// bytes, used before 12 Nov 2026, missed W and M in the Artbrush fonts.)
bool ParseMeta(const std::vector<uint8_t>& b, Font& f) {
  std::vector<dbx::Node> roots;
  if (!dbx::Parse(b, roots)) return false;
  auto iv = [](const dbx::Node* n, const char* k, int& out) {
    const dbx::Node* c = n->Field(k);
    int32_t v = 0;
    if (!c || !c->Int(v)) return false;
    out = v;
    return true;
  };
  for (const dbx::Node* g : dbx::Instances(roots, "UI.UIFontGlyph")) {
    int code = 0;
    if (!iv(g, "Glyph", code)) continue;
    Glyph gl;
    iv(g, "X", gl.x), iv(g, "Y", gl.y), iv(g, "Width", gl.w), iv(g, "Rows", gl.h);
    iv(g, "MinX", gl.min_x), iv(g, "YOffset", gl.y_offset), iv(g, "Advance", gl.advance);
    gl.x += kInkShift, gl.y += kInkShift;
    f.glyphs[code] = gl;
  }
  for (const dbx::Node* k : dbx::Instances(roots, "UI.UIFontGlyphKerning")) {
    int first = 0, second = 0, v = 0;
    if (iv(k, "First", first) && iv(k, "Second", second) && iv(k, "Kerning", v)) f.kerning[{first, second}] = v;
  }
  return f.glyphs.size() > 60;
}

// W: in normal_large's atlas at its left edge (x 0 to 38, y 37 to 73), drawn larger than the rest; scaled to the
// capitals (A: 31 rows, offset 10).
void AddW(Font& f) {
  if (f.glyphs.count('W')) return;
  Glyph w;
  w.x = 0, w.y = 37, w.w = 38, w.h = 36;
  w.scale = 31.0f / 34.0f;
  w.min_x = 0, w.y_offset = 10, w.advance = 31;
  f.glyphs['W'] = w;
}

void MeasureCaps(Font& f) {
  auto it = f.glyphs.find('H');
  if (it == f.glyphs.end()) it = f.glyphs.find('A');
  if (it == f.glyphs.end()) it = f.glyphs.begin();
  f.cap_rows = std::max(1, it->second.h), f.cap_top = it->second.y_offset;
}


// The cache: "BFMF", version 4 (2 before 12 Nov 2026: tables without W and M; 3: rects without kInkShift), the
// atlas's width and height, glyph count, glyphs (8 ints and a float), kerning count, pairs (3 ints), the atlas.
bool ReadCache(const fs::path& p, Font& f) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  char magic[4];
  uint32_t version = 0, w = 0, h = 0, n = 0;
  in.read(magic, 4).read(reinterpret_cast<char*>(&version), 4);
  if (!in || std::memcmp(magic, "BFMF", 4) != 0 || version != 4) return false;
  in.read(reinterpret_cast<char*>(&w), 4).read(reinterpret_cast<char*>(&h), 4).read(reinterpret_cast<char*>(&n), 4);
  if (!in || w == 0 || h == 0 || w > 4096 || h > 4096 || n > 4096) return false;
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
  f.w = int(w), f.h = int(h);
  f.alpha.resize(size_t(w) * h);
  in.read(reinterpret_cast<char*>(f.alpha.data()), std::streamsize(f.alpha.size()));
  if (!in || f.glyphs.size() < 60) return false;
  MeasureCaps(f);
  return true;
}

void WriteCache(const fs::path& p, const Font& f) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  const fs::path temp = p.string() + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    const uint32_t version = 4, w = uint32_t(f.w), h = uint32_t(f.h), n = uint32_t(f.glyphs.size()),
                   k = uint32_t(f.kerning.size());
    out.write("BFMF", 4).write(reinterpret_cast<const char*>(&version), 4);
    out.write(reinterpret_cast<const char*>(&w), 4).write(reinterpret_cast<const char*>(&h), 4);
    out.write(reinterpret_cast<const char*>(&n), 4);
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
  fs::rename(temp, p, ec);
}

fs::path CachePath(const fs::path& user_dir, const std::string& name) {
  return user_dir / "cache" / ("bf_font_" + name + ".bin");
}

// A font from its two archive entries: the texture (an optional 0x40 byte "RES" wrapper, then a 0x5C byte header:
// format at +0x08, width +0x10, height +0x14; format 10 is one byte a texel, tiled) and its metrics.
bool FontFromEntries(const std::string& name, const std::vector<uint8_t>& meta, const std::vector<uint8_t>& tex, Font& f) {
  if (!ParseMeta(meta, f)) return false;
  const size_t at = tex.size() >= 3 && std::memcmp(tex.data(), "RES", 3) == 0 ? 0x40 : 0;
  if (tex.size() < at + 0x5C) return false;
  const uint8_t* hd = &tex[at];
  const uint32_t fmt = Be32(hd + 8), w = Be32(hd + 16), h = Be32(hd + 20);
  if (fmt != 10 || w == 0 || h == 0 || w > 4096 || h > 4096 || tex.size() < at + 0x5C + size_t(w) * h) return false;
  const uint8_t* data = hd + 0x5C;
  f.w = int(w), f.h = int(h);
  f.alpha.resize(size_t(w) * h);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) f.alpha[size_t(y) * w + x] = data[TiledOffset(x, y, w, 1)];
  if (name == kLegacy) AddW(f);
  MeasureCaps(f);
  return true;
}

bool LoadFromGame(const std::string& name, const fs::path& game_dir, Font& f) {
  const fs::path archive = game_dir / "dist" / "xenon" / "build_overlay-00.fbrb";
  std::vector<std::vector<uint8_t>> parts;
  if (!ReadFbrbEntries(archive, {"Output/xenon/Fonts/us/" + name + "_meta.dbx", "Output/xenon/Fonts/us/" + name + "_texture.res"}, parts) ||
      parts.size() != 2)
    return false;
  return FontFromEntries(name, parts[0], parts[1], f);
}

// The atlas into ImGui's font texture, once (it may move when ImGui grows its texture; GetCustomRect follows it).
bool EnsureRect(Font& f) {
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  if (f.rect != ImFontAtlasRectId_Invalid) {
    ImFontAtlasRect r;
    if (atlas->GetCustomRect(f.rect, &r)) return true;
  }
  ImFontAtlasRect r;
  f.rect = atlas->AddCustomRect(f.w, f.h, &r);
  if (f.rect == ImFontAtlasRectId_Invalid || !atlas->TexData || !atlas->TexData->Pixels) return false;
  ImTextureData* tex = atlas->TexData;
  for (int y = 0; y < f.h; ++y) {
    auto* row = static_cast<uint8_t*>(tex->GetPixelsAt(r.x, r.y + y));
    const uint8_t* src = &f.alpha[size_t(y) * f.w];
    if (tex->Format == ImTextureFormat_Alpha8) {
      std::memcpy(row, src, size_t(f.w));
    } else {
      for (int x = 0; x < f.w; ++x) row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 255, row[x * 4 + 3] = src[x];
    }
  }
  ImFontAtlasTextureBlockQueueUpload(atlas, tex, r.x, r.y, f.w, f.h);
  return true;
}

// The font called `name`, loaded once from its cache in the user folder; normal_large also from the game itself
// (its old path; the others are prepared on the worker, BfFontPrepare).
Font* Get(const std::string& name, const fs::path& game_dir, const fs::path& user_dir) {
  Font& f = g_fonts[name];
  if (f.tried) return f.ok ? &f : nullptr;
  f.tried = true;
  const fs::path cache = CachePath(user_dir, name);
  if (ReadCache(cache, f)) {
    f.ok = true;
  } else if (name == kLegacy) {
    f = Font{};
    f.tried = true;
    if (LoadFromGame(name, game_dir, f)) {
      WriteCache(cache, f);
      f.ok = true;
    }
  }
  if (f.ok || name == kLegacy)
    REXLOG_INFO("BF menu font {}: {} ({} glyphs, {} kerning pairs, {}x{}, capitals {} rows)", name,
                f.ok ? "ready" : "not found", f.glyphs.size(), f.kerning.size(), f.w, f.h, f.cap_rows);
  return f.ok ? &f : nullptr;
}

// BF's drawing of its rows, per screen pixel at 1800 lines (see the top).
constexpr float kScaleAt1800 = 1.55f, kWide = 1.0f, kTrack = 0.88f;

fs::path g_game_dir, g_user_dir;

}  // namespace

// Every font the launcher draws with, from the player's game into the user folder's cache (one read of the
// archive), on a worker after the game is installed. Fonts already cached are left alone.
void BfFontPrepare(const fs::path& game_dir, const fs::path& user_dir) {
  static const char* kNames[] = {"normal_large", "headline_large", "headline_medium", "normal_medium", "list_header",
                                 "list_item", "normal_small", "headnumbers"};
  std::lock_guard lock(g_prepare_mutex);
  std::vector<std::string> todo, names;
  for (const char* n : kNames)
    if (!fs::exists(CachePath(user_dir, n))) todo.push_back(n);
  if (todo.empty()) return;
  for (const std::string& n : todo)
    names.push_back("Output/xenon/Fonts/us/" + n + "_meta.dbx"), names.push_back("Output/xenon/Fonts/us/" + n + "_texture.res");
  std::vector<std::vector<uint8_t>> parts;
  if (!ReadFbrbEntries(game_dir / "dist" / "xenon" / "build_overlay-00.fbrb", names, parts) || parts.size() != names.size()) {
    REXLOG_WARN("BF menu fonts: the game's font archive could not be read");
    return;
  }
  int made = 0;
  for (size_t i = 0; i < todo.size(); ++i) {
    Font f;
    if (!FontFromEntries(todo[i], parts[i * 2], parts[i * 2 + 1], f)) {
      REXLOG_WARN("BF menu fonts: {} could not be read from the game", todo[i]);
      continue;
    }
    WriteCache(CachePath(user_dir, todo[i]), f);
    ++made;
  }
  REXLOG_INFO("BF menu fonts: {} of {} prepared from the game's atlases ({} already there)", made, todo.size(),
              std::size(kNames) - todo.size());
}

// Loads BF's lettering once: from the cache in the user folder, else from the player's game (then cached).
bool BfFontReady(const fs::path& game_dir, const fs::path& user_dir) {
  g_game_dir = game_dir, g_user_dir = user_dir;
  return Get(kLegacy, game_dir, user_dir) != nullptr;
}

// One of the game's other fonts (headline_large, headline_medium, normal_medium, list_header...): true once its
// cache is there (BfFontPrepare makes it); false until then (callers draw with our own fonts meanwhile).
bool BfFontReadyNamed(const std::string& name, const fs::path& game_dir, const fs::path& user_dir) {
  g_game_dir = game_dir, g_user_dir = user_dir;
  Font* f = Get(name, game_dir, user_dir);
  if (!f) {  // not cached yet: looked for again, the worker may have made it meanwhile
    Font& again = g_fonts[name];
    static std::map<std::string, double> last;
    const double now = ImGui::GetTime();
    if (now - last[name] > 2.0) last[name] = now, again.tried = false;
  }
  return f != nullptr;
}

// The width of text in BF's lettering with capitals `cap` pixels high (BF's own rows: 42 at 1800 lines).
float BfTextWidth(const std::string& text, float cap) {
  const Font* f = Get(kLegacy, g_game_dir, g_user_dir);
  if (!f) return 0;
  const float s = cap / 42.0f * kScaleAt1800;
  float w = 0;
  int prev = -1;
  for (unsigned char c : text) {
    auto it = f->glyphs.find(c);
    if (prev >= 0)
      if (auto k = f->kerning.find({prev, c}); k != f->kerning.end()) w += k->second * s * kWide;
    w += (it == f->glyphs.end() ? 10 : it->second.advance) * s * kWide * kTrack;
    prev = c;
  }
  return w;
}

// The width of `text` in the font `name`, its capitals `cap` pixels high, `track` extra pixels after each glyph.
float BfTextWidthNamed(const std::string& name, const std::string& text, float cap, float track) {
  const Font* f = Get(name, g_game_dir, g_user_dir);
  if (!f) return 0;
  const float s = cap / float(f->cap_rows);
  float w = 0;
  int prev = -1;
  for (unsigned char c : text) {
    auto it = f->glyphs.find(c);
    if (prev >= 0)
      if (auto k = f->kerning.find({prev, c}); k != f->kerning.end()) w += k->second * s;
    w += (it == f->glyphs.end() ? f->cap_rows / 3 : it->second.advance) * s + track;
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
    ImFontAtlasTextureBlockQueueUpload(atlas, tex, r.x, r.y, 256, 64);
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
  Font* f = on ? Get(kLegacy, g_game_dir, g_user_dir) : nullptr;
  if (!f || !EnsureRect(*f)) return;
  ImFontAtlasRect r;
  if (!ImGui::GetIO().Fonts->GetCustomRect(f->rect, &r)) return;
  d->AddRectFilled(ImVec2(0, 0), ImVec2(float(f->w), float(f->h)), IM_COL32(0, 0, 0, 255));
  d->AddImage(ImGui::GetIO().Fonts->TexRef, ImVec2(0, 0), ImVec2(float(f->w), float(f->h)), r.uv0, r.uv1);
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
  Font* f = Get(kLegacy, g_game_dir, g_user_dir);
  if (!f || !EnsureRect(*f)) return;
  ImFontAtlasRect r;
  if (!ImGui::GetIO().Fonts->GetCustomRect(f->rect, &r)) return;
  const float s = cap / 42.0f * kScaleAt1800;
  const float du = (r.uv1.x - r.uv0.x) / float(f->w), dv = (r.uv1.y - r.uv0.y) / float(f->h);
  // The capitals' top is their cell's top (A: offset 10) now that the rects sit on the ink (kInkShift).
  float x = top_left.x;
  const float y0 = top_left.y - 10.0f * s;
  const float bold = cap / 42.0f * 0.0f, soft = cap / 42.0f * 0.9f;
  int prev = -1;
  for (unsigned char c : text) {
    auto it = f->glyphs.find(c);
    if (prev >= 0)
      if (auto k = f->kerning.find({prev, c}); k != f->kerning.end()) x += k->second * s * kWide;
    if (it == f->glyphs.end()) {
      x += 10 * s * kWide * kTrack;
      prev = c;
      continue;
    }
    const Glyph& g = it->second;
    const float gs = s * g.scale;
    const ImVec2 p0(x + g.min_x * s * kWide, y0 + g.y_offset * s), p1(p0.x + g.w * gs * kWide, p0.y + g.h * gs);
    const ImVec2 uv0(r.uv0.x + g.x * du, r.uv0.y + g.y * dv), uv1(r.uv0.x + (g.x + g.w) * du, r.uv0.y + (g.y + g.h) * dv);
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

// Text in the font `name`, plain: `top_left` is where its capitals start, `cap` their height in pixels, `track` extra
// pixels after each glyph. Nothing is drawn (and false returned) until the font is ready.
bool BfTextNamed(const std::string& name, ImDrawList* d, ImVec2 top_left, float cap, ImU32 color, const std::string& text,
                 float track) {
  Font* f = Get(name, g_game_dir, g_user_dir);
  if (!f || !EnsureRect(*f)) return false;
  ImFontAtlasRect r;
  if (!ImGui::GetIO().Fonts->GetCustomRect(f->rect, &r)) return false;
  const float s = cap / float(f->cap_rows);
  const float du = (r.uv1.x - r.uv0.x) / float(f->w), dv = (r.uv1.y - r.uv0.y) / float(f->h);
  float x = top_left.x;
  const float y0 = top_left.y - float(f->cap_top) * s;
  int prev = -1;
  for (unsigned char c : text) {
    auto it = f->glyphs.find(c);
    if (prev >= 0)
      if (auto k = f->kerning.find({prev, c}); k != f->kerning.end()) x += k->second * s;
    if (it == f->glyphs.end()) {
      x += f->cap_rows / 3 * s + track;
      prev = c;
      continue;
    }
    const Glyph& g = it->second;
    if (g.w > 0 && g.h > 0) {
      const ImVec2 p0(x + g.min_x * s, y0 + g.y_offset * s), p1(p0.x + g.w * s * g.scale, p0.y + g.h * s * g.scale);
      const ImVec2 uv0(r.uv0.x + g.x * du, r.uv0.y + g.y * dv), uv1(r.uv0.x + (g.x + g.w) * du, r.uv0.y + (g.y + g.h) * dv);
      d->AddImage(ImGui::GetIO().Fonts->TexRef, p0, p1, uv0, uv1, color);
    }
    x += g.advance * s + track;
    prev = c;
  }
  return true;
}

}  // namespace bf1943
