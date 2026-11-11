// The launcher's Version 1 look and its button glyphs (launcher_kit.h).

#include "launcher_kit.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

#include <imgui_internal.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/image_decode.h>

#include "picture_decode.h"

// The stamp on every launcher page, BF's main menu and its pause (TekRantGaming, 8 Nov 2026); a release build can leave it
// off by default (BF_NO_PLAYTEST_WATERMARK).
#if defined(BF_NO_PLAYTEST_WATERMARK)
REXCVAR_DEFINE_BOOL(bf_playtest_watermark, false, "BF1943", "The EARLY PLAYTEST stamp on our menus");
#else
REXCVAR_DEFINE_BOOL(bf_playtest_watermark, true, "BF1943", "The EARLY PLAYTEST stamp on our menus");
#endif

namespace bf1943 {
std::string PadFamily();  // game_menu.cpp: the controller in use
}  // namespace bf1943

namespace bf1943::kit {

namespace fs = std::filesystem;

namespace {
Fonts g_fonts;
fs::path g_glyph_dir;
}  // namespace

const Fonts& GetFonts() { return g_fonts; }

void LoadFonts(ImFontAtlas* atlas, const fs::path& exe_dir, ImFont* ui, ImFont* ui_bold) {
  auto add = [&](const fs::path& p, float size) -> ImFont* {
    std::error_code ec;
    if (!fs::exists(p, ec)) return nullptr;
    return atlas->AddFontFromFileTTF(p.string().c_str(), size);
  };
  const fs::path ours = exe_dir / "fonts";
#if defined(_WIN32)
  const fs::path windows = "C:\\Windows\\Fonts";
#else
  const fs::path windows = "/usr/share/fonts";
#endif
  for (const char* f : {"BigShouldersStencilDisplay-Black.ttf", "BigShouldersStencilDisplay-ExtraBold.ttf",
                        "BigShouldersStencilDisplay-Bold.ttf"})
    if (!g_fonts.stencil) g_fonts.stencil = add(ours / f, 64);
  const char* stencil_from = "Big Shoulders Stencil Display";
  if (!g_fonts.stencil && (g_fonts.stencil = add(windows / "STENCIL.TTF", 64))) {
    g_fonts.stencil_squeeze = 0.74f, stencil_from = "Windows' Stencil, narrowed";
  }
  if (!g_fonts.stencil) g_fonts.stencil = ui_bold, stencil_from = "Oswald Bold";
  g_fonts.typed = add(ours / "SpecialElite-Regular.ttf", 26);
  const char* typed_from = "Special Elite";
  if (!g_fonts.typed && (g_fonts.typed = add(windows / "courbd.ttf", 26))) typed_from = "Courier New Bold";
  if (!g_fonts.typed) g_fonts.typed = ui, typed_from = "Oswald";
  g_fonts.ui = ui, g_fonts.ui_bold = ui_bold;
  REXLOG_INFO("BF launcher fonts: headings {}, captions {}", stencil_from, typed_from);
  g_glyph_dir = exe_dir / "glyphs";
}

void SetGlyphDir(const fs::path& dir) { g_glyph_dir = dir; }

bool WatermarkOn() { return REXCVAR_GET(bf_playtest_watermark); }

// ---- text

namespace {
// Vertices added since `start` scaled along x round x0 (the stencil stand in, narrowed).
void ScaleX(ImDrawList* d, int start, float x0, float f) {
  if (f == 1) return;
  for (int i = start; i < d->VtxBuffer.Size; ++i) d->VtxBuffer[i].pos.x = x0 + (d->VtxBuffer[i].pos.x - x0) * f;
}
size_t CharLen(const std::string& t, size_t i) {
  size_t n = 1;
  while (i + n < t.size() && (static_cast<unsigned char>(t[i + n]) & 0xC0) == 0x80) ++n;
  return n;
}
}  // namespace

float TextWidth(ImFont* f, float size, const std::string& t) {
  if (!f) return 0;
  return f->CalcTextSizeA(size, FLT_MAX, 0, t.c_str()).x;
}

float SpacedWidth(ImFont* f, float size, const std::string& t, float track) {
  if (track == 0) return TextWidth(f, size, t) * (f == g_fonts.stencil ? g_fonts.stencil_squeeze : 1.0f);
  float w = 0;
  for (size_t i = 0; i < t.size();) {
    const size_t n = CharLen(t, i);
    w += TextWidth(f, size, t.substr(i, n)) + track;
    i += n;
  }
  if (!t.empty()) w -= track;
  return w * (f == g_fonts.stencil ? g_fonts.stencil_squeeze : 1.0f);
}

float Spaced(ImDrawList* d, ImFont* f, float size, ImVec2 at, ImU32 col, const std::string& t, float track) {
  if (!f || t.empty()) return 0;
  const int start = d->VtxBuffer.Size;
  float x = at.x;
  if (track == 0) {
    d->AddText(f, size, at, col, t.c_str());
    x += TextWidth(f, size, t);
  } else {
    for (size_t i = 0; i < t.size();) {
      const size_t n = CharLen(t, i);
      const std::string ch = t.substr(i, n);
      d->AddText(f, size, ImVec2(x, at.y), col, ch.c_str());
      x += TextWidth(f, size, ch) + track;
      i += n;
    }
    x -= track;
  }
  const float squeeze = f == g_fonts.stencil ? g_fonts.stencil_squeeze : 1.0f;
  ScaleX(d, start, at.x, squeeze);
  return (x - at.x) * squeeze;
}

float Heading(ImDrawList* d, float size, ImVec2 at, ImU32 col, const std::string& t, float track, float max_w) {
  ImFont* f = g_fonts.stencil;
  float w = SpacedWidth(f, size, t, track);
  if (max_w > 0 && w > max_w) {
    const float k = max_w / w;
    size *= k, track *= k;
    at.y += size * (1 / k - 1) * 0.5f;  // shrunk text stays centred on its line
  }
  return Spaced(d, f, size, at, col, t, track);
}

float WrappedHeight(ImFont* f, float size, const std::string& t, float wrap) {
  if (!f) return 0;
  return f->CalcTextSizeA(size, FLT_MAX, wrap, t.c_str()).y;
}

float Wrapped(ImDrawList* d, ImFont* f, float size, ImVec2 at, ImU32 col, const std::string& t, float wrap) {
  if (!f) return 0;
  d->AddText(f, size, at, col, t.c_str(), nullptr, wrap);
  return WrappedHeight(f, size, t, wrap);
}

// ---- turning

Turn::Turn(ImDrawList* d, ImVec2 centre, float degrees)
    : d_(d), start_(d->VtxBuffer.Size), centre_(centre), degrees_(degrees) {}

Turn::~Turn() {
  if (degrees_ == 0) return;
  const float r = degrees_ * 3.14159265f / 180.0f, c = std::cos(r), s = std::sin(r);
  for (int i = start_; i < d_->VtxBuffer.Size; ++i) {
    ImVec2& p = d_->VtxBuffer[i].pos;
    const float x = p.x - centre_.x, y = p.y - centre_.y;
    p = ImVec2(centre_.x + x * c - y * s, centre_.y + x * s + y * c);
  }
}

// ---- the paper's grain: a 48 px tile of soft dots on a 6 px grid (the mockups' radial gradient dots), once in
// ImGui's font atlas.

namespace {
bool AtlasRect(ImFontAtlasRectId& id, int w, int h, const std::function<void(uint8_t* rgba, int stride)>& fill,
               ImFontAtlasRect& out) {
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  if (id != ImFontAtlasRectId_Invalid && atlas->GetCustomRect(id, &out)) return true;
  id = atlas->AddCustomRect(w, h, &out);
  ImTextureData* tex = atlas->TexData;
  if (id == ImFontAtlasRectId_Invalid || !tex || !tex->Pixels || tex->Format != ImTextureFormat_RGBA32) {
    id = ImFontAtlasRectId_Invalid;
    return false;
  }
  fill(static_cast<uint8_t*>(tex->GetPixelsAt(out.x, out.y)), tex->GetPitch());
  ImFontAtlasTextureBlockQueueUpload(atlas, tex, out.x, out.y, w, h);
  return atlas->GetCustomRect(id, &out);
}
}  // namespace

namespace {
void Grain(ImDrawList* d, ImVec2 a, ImVec2 b, float u) {
  static ImFontAtlasRectId id = ImFontAtlasRectId_Invalid;
  ImFontAtlasRect r;
  constexpr int kTile = 48;
  if (!AtlasRect(id, kTile, kTile,
                 [](uint8_t* p, int stride) {
                   for (int y = 0; y < kTile; ++y)
                     for (int x = 0; x < kTile; ++x) {
                       const float dx = float(x % 6) - 2.5f, dy = float(y % 6) - 2.5f;
                       const float dist = std::sqrt(dx * dx + dy * dy);
                       uint8_t* px = p + y * stride + x * 4;
                       px[0] = px[1] = px[2] = 255;
                       px[3] = uint8_t(std::clamp(1.0f - dist / 1.4f, 0.0f, 1.0f) * 255);
                     }
                 },
                 r))
    return;
  const float tile = kTile * 0.5f * u;  // the dots 6 px apart at 720 lines (3 texels a dot step at 2x)
  const ImU32 tint = IM_COL32(120, 100, 60, 26);
  for (float y = a.y; y < b.y; y += tile)
    for (float x = a.x; x < b.x; x += tile) {
      const float x1 = std::min(b.x, x + tile), y1 = std::min(b.y, y + tile);
      const ImVec2 uv1(r.uv0.x + (r.uv1.x - r.uv0.x) * (x1 - x) / tile, r.uv0.y + (r.uv1.y - r.uv0.y) * (y1 - y) / tile);
      d->AddImage(ImGui::GetIO().Fonts->TexRef, ImVec2(x, y), ImVec2(x1, y1), r.uv0, uv1, tint);
    }
}
}  // namespace

void Backdrop(ImDrawList* d, ImVec2 size, float u, float over_game) {
  if (over_game > 0) {
    d->AddRectFilled(ImVec2(0, 0), size, IM_COL32(16, 18, 12, int(255 * std::clamp(over_game, 0.0f, 1.0f))));
  } else {
    // radial-gradient(ellipse at 50% 30%, #343a29, #1f2318 60%, #15180f): top lighter, edges darker.
    d->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x, size.y * 0.3f), IM_COL32(40, 45, 32, 255),
                               IM_COL32(40, 45, 32, 255), IM_COL32(48, 54, 38, 255), IM_COL32(48, 54, 38, 255));
    d->AddRectFilledMultiColor(ImVec2(0, size.y * 0.3f), size, IM_COL32(48, 54, 38, 255), IM_COL32(48, 54, 38, 255),
                               IM_COL32(21, 24, 15, 255), IM_COL32(21, 24, 15, 255));
    // the ellipse's darker sides
    d->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x * 0.3f, size.y), IM_COL32(15, 18, 10, 150),
                               IM_COL32(15, 18, 10, 0), IM_COL32(15, 18, 10, 0), IM_COL32(15, 18, 10, 150));
    d->AddRectFilledMultiColor(ImVec2(size.x * 0.7f, 0), size, IM_COL32(15, 18, 10, 0), IM_COL32(15, 18, 10, 150),
                               IM_COL32(15, 18, 10, 150), IM_COL32(15, 18, 10, 0));
  }
  const float step = 40 * u;
  const ImU32 line = IM_COL32(214, 203, 170, 13);
  for (float x = 0; x < size.x; x += step) d->AddLine(ImVec2(x, 0), ImVec2(x, size.y), line, std::max(1.0f, u));
  for (float y = 0; y < size.y; y += step) d->AddLine(ImVec2(0, y), ImVec2(size.x, y), line, std::max(1.0f, u));
}

void Paper(ImDrawList* d, ImVec2 a, ImVec2 b, float u, bool torn) {
  // box-shadow: 0 18px 40px rgba(0,0,0,0.5), as layers growing out and down
  for (int i = 6; i >= 1; --i) {
    const float g = float(i) * 4 * u;
    d->AddRectFilled(ImVec2(a.x - g * 0.6f, a.y - g * 0.2f + 10 * u), ImVec2(b.x + g * 0.6f, b.y + g + 10 * u),
                     IM_COL32(0, 0, 0, 20), g);
  }
  if (torn) {  // clip-path: polygon(0 1%, 2% 0, 98% 1%, 100% 0, 99% 99%, 97% 100%, 1% 99%, 0 100%)
    const float w = b.x - a.x, h = b.y - a.y;
    const ImVec2 pts[] = {{a.x, a.y + h * 0.01f},          {a.x + w * 0.02f, a.y},
                          {a.x + w * 0.98f, a.y + h * 0.01f}, {b.x, a.y},
                          {b.x - w * 0.01f, a.y + h * 0.99f}, {a.x + w * 0.97f, b.y},
                          {a.x + w * 0.01f, b.y - h * 0.01f}, {a.x, b.y}};
    // concave in places: one concave fill (a fan of triangles showed their antialiased seams)
    d->AddConcavePolyFilled(pts, 8, kPaper);
  } else {
    d->AddRectFilled(a, b, kPaper);
  }
  Grain(d, ImVec2(a.x + 2 * u, a.y + 2 * u), ImVec2(b.x - 2 * u, b.y - 2 * u), u);
}

void Tape(ImDrawList* d, ImVec2 centre, float w, float h, float degrees) {
  Turn t(d, centre, degrees);
  const ImVec2 a(centre.x - w * 0.5f, centre.y - h * 0.5f), b(centre.x + w * 0.5f, centre.y + h * 0.5f);
  d->AddRectFilled(ImVec2(a.x, a.y + 1), ImVec2(b.x, b.y + 2), IM_COL32(0, 0, 0, 50));
  d->AddRectFilled(a, b, IM_COL32(222, 210, 172, 184));
}

void Brand(ImDrawList* d, ImVec2 at, float u) {
  const float size = 30 * u, track = 4 * u;
  const float w = Spaced(d, g_fonts.stencil, size, at, kKhaki, "BATTLEFIELD 1943 ", track);
  Spaced(d, g_fonts.stencil, size, ImVec2(at.x + w + track, at.y), kKhakiDim, "REBORN", track);
}

void StepBar(ImDrawList* d, ImVec2 right_top, float u, int step, int of) {
  const float bw = 180 * u, bh = 8 * u;
  const ImVec2 b1(right_top.x, right_top.y + 6 * u + bh), b0(right_top.x - bw, right_top.y + 6 * u);
  d->AddRectFilled(b0, b1, IM_COL32(241, 234, 211, 51));
  d->AddRectFilled(b0, ImVec2(b0.x + bw * float(std::clamp(step, 0, of)) / float(std::max(of, 1)), b1.y), kGreenBright);
  const std::string t = std::to_string(step) + " of " + std::to_string(of);
  const float size = 16 * u;
  d->AddText(g_fonts.ui, size, ImVec2(b0.x - 12 * u - TextWidth(g_fonts.ui, size, t), right_top.y), kBackText, t.c_str());
}

void Stamp(ImDrawList* d, ImVec2 centre, float u, float degrees, float scale, bool backed) {
  const float k = u * scale;
  const std::string top = "EARLY PLAYTEST", bottom = "ALPHA \xC2\xB7 SUBJECT TO CHANGE";
  const float ts = 30 * k, tt = 3 * k, bs = 13 * k, bt = 3 * k;
  const float tw = SpacedWidth(g_fonts.stencil, ts, top, tt), bw = SpacedWidth(g_fonts.typed, bs, bottom, bt);
  const float w = std::max(tw, bw) + 28 * k, h = ts * 1.05f + bs * 1.4f + 10 * k;
  Turn t(d, centre, degrees);
  const ImVec2 a(centre.x - w * 0.5f, centre.y - h * 0.5f), b(centre.x + w * 0.5f, centre.y + h * 0.5f);
  if (backed) d->AddRectFilled(a, b, IM_COL32(236, 229, 209, 140));
  d->AddRect(a, b, kStampEdge, 0, 0, 4 * k);
  Spaced(d, g_fonts.stencil, ts, ImVec2(centre.x - tw * 0.5f, a.y + 5 * k), kStamp, top, tt);
  Spaced(d, g_fonts.typed, bs, ImVec2(centre.x - bw * 0.5f, a.y + 5 * k + ts * 1.02f), kStamp, bottom, bt);
}

// ---- glyphs

Style StyleFor(const std::string& v, bool keyboard_last) {
  if (v == "keyboard") return Style::kKeyboard;
  if (v == "xbox360") return Style::kXbox360;
  if (v == "xbox_series") return Style::kXboxSeries;
  if (v == "ps3" || v == "ps2") return Style::kPlayStation3;
  if (v == "ps5") return Style::kPlayStation5;
  if (v == "steam_deck") return Style::kSteamDeck;
  // Automatic: the keyboard's when it was used last, else the controller in use (game_menu.cpp PadFamily).
  if (keyboard_last) return Style::kKeyboard;
  const std::string family = PadFamily();
  return family == "ps5" ? Style::kPlayStation5 : family == "steam_deck" ? Style::kSteamDeck : Style::kXboxSeries;
}

const char* StyleName(Style s) {
  switch (s) {
    case Style::kXbox360: return "Xbox 360";
    case Style::kXboxSeries: return "Xbox Series";
    case Style::kPlayStation3: return "PlayStation 3";
    case Style::kPlayStation5: return "PlayStation 5";
    case Style::kSteamDeck: return "Steam Deck";
    case Style::kKeyboard: return "Keyboard and mouse";
  }
  return "";
}

namespace {
struct GlyphImage {
  ImFontAtlasRectId id = ImFontAtlasRectId_Invalid;
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;  // kept: the atlas is rebuilt now and then
  bool missing = false;
};
std::map<std::string, GlyphImage> g_glyphs;

// The picture `set`/`name`.png from <exe>\glyphs, in the atlas; null if there is none.
const GlyphImage* Picture(const std::string& set, const std::string& name, ImFontAtlasRect& r) {
  GlyphImage& g = g_glyphs[set + "/" + name];
  if (g.missing) return nullptr;
  if (g.rgba.empty()) {
    std::ifstream f(g_glyph_dir / set / (name + ".png"), std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    int w = 0, h = 0;
    if (!bytes.empty()) g.rgba = rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), w, h);
    if (g.rgba.empty() || w <= 0 || h <= 0) {
      g.missing = true;
      return nullptr;
    }
    g.w = w, g.h = h;
  }
  const std::vector<uint8_t>& src = g.rgba;
  const int w = g.w;
  if (!AtlasRect(g.id, g.w, g.h,
                 [&src, w, h = g.h](uint8_t* p, int stride) {
                   for (int y = 0; y < h; ++y) std::memcpy(p + y * stride, &src[size_t(y) * w * 4], size_t(w) * 4);
                 },
                 r)) {
    g.missing = true;
    return nullptr;
  }
  return &g;
}

}  // namespace

bool DrawImageFile(ImDrawList* d, const std::filesystem::path& file, ImVec2 a, ImVec2 b, bool stretch, bool fit,
                   int max_width) {
  struct FileImage {
    ImFontAtlasRectId id = ImFontAtlasRectId_Invalid;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    double checked = -10;
  };
  static std::map<std::string, FileImage> images;
  FileImage& g = images[file.string()];
  if (g.rgba.empty()) {
    if (ImGui::GetTime() - g.checked < 2.0) return false;  // made on a worker: looked for again every 2 s
    g.checked = ImGui::GetTime();
    std::ifstream f(file, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    int w = 0, h = 0;
    if (!bytes.empty()) g.rgba = DecodePicture(bytes.data(), bytes.size(), w, h);  // PNG or JPEG (picture_decode.cpp)
    if (g.rgba.empty() || w <= 0 || h <= 0) {
      g.rgba.clear();
      return false;
    }
    // Kept no wider than `max_width` in the atlas (a box average by a whole factor), as King Kong decodes its tiles'
    // pictures at most 960 wide: a 1920 wide banner would take the atlas eight times the room.
    if (max_width > 0 && w > max_width) {
      const int k = (w + max_width - 1) / max_width, nw = std::max(1, w / k), nh = std::max(1, h / k);
      std::vector<uint8_t> small(size_t(nw) * nh * 4);
      for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x)
          for (int c = 0; c < 4; ++c) {
            unsigned sum = 0;
            for (int j = 0; j < k; ++j)
              for (int i = 0; i < k; ++i) sum += g.rgba[(size_t(y * k + j) * w + size_t(x * k + i)) * 4 + c];
            small[(size_t(y) * nw + x) * 4 + c] = uint8_t(sum / unsigned(k * k));
          }
      g.rgba = std::move(small), w = nw, h = nh;
    }
    g.w = w, g.h = h;
  }
  ImFontAtlasRect r;
  const std::vector<uint8_t>& src = g.rgba;
  const int w = g.w;
  if (!AtlasRect(g.id, g.w, g.h,
                 [&src, w, h = g.h](uint8_t* p, int stride) {
                   for (int y = 0; y < h; ++y) std::memcpy(p + y * stride, &src[size_t(y) * w * 4], size_t(w) * 4);
                 },
                 r))
    return false;
  // Cropped to the box's shape about the middle.
  const float box = (b.x - a.x) / std::max(1.0f, b.y - a.y), pic = float(g.w) / float(g.h);
  ImVec2 uv0 = r.uv0, uv1 = r.uv1;
  if (fit && (pic > box * 1.3f || pic < box * 0.75f)) {
    // King Kong's DrawCover (workshop.cpp): a shape far from the box's is shown whole, over itself cropped and dimmed.
    ImVec2 cu0 = uv0, cu1 = uv1;
    if (pic > box) {
      const float du = (cu1.x - cu0.x) * (1 - box / pic) * 0.5f;
      cu0.x += du, cu1.x -= du;
    } else {
      const float dv = (cu1.y - cu0.y) * (1 - pic / box) * 0.5f;
      cu0.y += dv, cu1.y -= dv;
    }
    d->AddImage(ImGui::GetIO().Fonts->TexRef, a, b, cu0, cu1, IM_COL32(70, 72, 78, 255));
    ImVec2 c0 = a, c1 = b;
    if (pic > box) {
      const float h = (b.x - a.x) / pic, y = std::round((a.y + b.y - h) * 0.5f);
      c0.y = y, c1.y = y + h;
    } else {
      const float wd = (b.y - a.y) * pic, x = std::round((a.x + b.x - wd) * 0.5f);
      c0.x = x, c1.x = x + wd;
    }
    d->AddImage(ImGui::GetIO().Fonts->TexRef, c0, c1, uv0, uv1);
    return true;
  }
  if (stretch) {
  } else if (pic > box) {
    const float keep = box / pic, du = (uv1.x - uv0.x) * (1 - keep) * 0.5f;
    uv0.x += du, uv1.x -= du;
  } else {
    const float keep = pic / box, dv = (uv1.y - uv0.y) * (1 - keep) * 0.5f;
    uv0.y += dv, uv1.y -= dv;
  }
  d->AddImage(ImGui::GetIO().Fonts->TexRef, a, b, uv0, uv1);
  return true;
}

bool DrawMask(ImDrawList* d, const char* key, const unsigned char* mask, int size, ImVec2 a, ImVec2 b, ImU32 tint) {
  static std::map<std::string, ImFontAtlasRectId> ids;
  ImFontAtlasRectId& id = ids.emplace(key, ImFontAtlasRectId_Invalid).first->second;
  ImFontAtlasRect r;
  // White, its alpha the mask's: ImGui's tint gives the colour (King Kong's BrandIcon does the same).
  if (!AtlasRect(id, size, size,
                 [mask, size](uint8_t* p, int stride) {
                   for (int y = 0; y < size; ++y)
                     for (int x = 0; x < size; ++x) {
                       uint8_t* px = p + y * stride + x * 4;
                       px[0] = px[1] = px[2] = 255;
                       px[3] = mask[y * size + x];
                     }
                 },
                 r))
    return false;
  d->AddImage(ImGui::GetIO().Fonts->TexRef, a, b, r.uv0, r.uv1, tint);
  return true;
}

namespace {
const char* PadName(Button b) {
  switch (b) {
    case Button::kA: return "a";
    case Button::kB: return "b";
    case Button::kX: return "x";
    case Button::kY: return "y";
    case Button::kLB: return "lb";
    case Button::kRB: return "rb";
    case Button::kLT: return "lt";
    case Button::kRT: return "rt";
    case Button::kStart: return "start";
    case Button::kBack: return "back";
    case Button::kDpad: return "dpad";
    case Button::kDpadUp: return "dpad_up";
    case Button::kLS:
    case Button::kRS: return "stick_click";
    default: return nullptr;
  }
}

const char* SetName(Style s) {
  switch (s) {
    case Style::kXboxSeries: return "xbox_series";
    case Style::kPlayStation3: return "ps2";  // the DualShock look, as King Kong's PlayStation 2 set
    case Style::kPlayStation5: return "ps5";
    case Style::kSteamDeck: return "steam_deck";
    case Style::kKeyboard: return "keyboard";
    default: return nullptr;
  }
}

// A light key cap with its name (the mockups' "START", "RB" tags), when there is no picture.
float Cap(ImDrawList* d, const std::string& t, ImVec2 at, float h, bool draw) {
  ImFont* f = g_fonts.ui_bold;
  const float size = h * 0.5f, w = TextWidth(f, size, t) + h * 0.5f;
  if (draw) {
    d->AddRectFilled(ImVec2(at.x, at.y - h * 0.36f), ImVec2(at.x + w, at.y + h * 0.36f), IM_COL32(233, 229, 218, 255),
                     h * 0.16f);
    d->AddText(f, size, ImVec2(at.x + h * 0.25f, at.y - size * 0.62f), kStrip, t.c_str());
  }
  return w;
}
}  // namespace

float Glyph(ImDrawList* d, Style style, Button b, const std::string& key, ImVec2 at, float h, bool draw) {
  if (style == Style::kKeyboard || b == Button::kKey) {
    const std::string name = key.empty() ? "?" : key;
    ImFontAtlasRect r;
    if (const GlyphImage* g = Picture("keyboard", name, r)) {
      const float w = h * float(g->w) / float(g->h);
      if (draw) d->AddImage(ImGui::GetIO().Fonts->TexRef, ImVec2(at.x, at.y - h * 0.5f), ImVec2(at.x + w, at.y + h * 0.5f),
                            r.uv0, r.uv1);
      return w;
    }
    return Cap(d, name, at, h, draw);
  }
  if (const char* set = SetName(style)) {
    if (const char* pad = PadName(b)) {
      ImFontAtlasRect r;
      if (const GlyphImage* g = Picture(set, pad, r)) {
        const float w = h * float(g->w) / float(g->h);
        if (draw)
          d->AddImage(ImGui::GetIO().Fonts->TexRef, ImVec2(at.x, at.y - h * 0.5f), ImVec2(at.x + w, at.y + h * 0.5f),
                      r.uv0, r.uv1);
        return w;
      }
    }
  }
  // The Xbox 360's, as the game draws its own: coloured discs with the letter; the rest as caps.
  if (b == Button::kA || b == Button::kB || b == Button::kX || b == Button::kY) {
    static const ImU32 kColors[] = {IM_COL32(87, 168, 59, 255), IM_COL32(217, 55, 47, 255), IM_COL32(47, 127, 209, 255),
                                    IM_COL32(232, 185, 35, 255)};
    static const char* kLetters[] = {"A", "B", "X", "Y"};
    const int i = int(b) - int(Button::kA);
    if (draw) {
      const float r = h * 0.5f;
      const ImVec2 c(at.x + r, at.y);
      d->AddCircleFilled(c, r, kColors[i], 24);
      ImFont* f = g_fonts.ui_bold;
      const float size = h * 0.62f;
      d->AddText(f, size, ImVec2(c.x - TextWidth(f, size, kLetters[i]) * 0.5f, c.y - size * 0.62f),
                 b == Button::kY ? IM_COL32(26, 26, 26, 255) : IM_COL32(255, 255, 255, 255), kLetters[i]);
    }
    return h;
  }
  static const char* kCaps[] = {"", "", "", "", "LB", "RB", "LT", "RT", "START", "BACK", "D-PAD", "UP", "LS", "RS", ""};
  std::string t = kCaps[int(b)];
  if (b == Button::kBack && style == Style::kXboxSeries) t = "VIEW";
  if (b == Button::kDpad) t = "\xE2\x9C\x9A";  // never shown: dpad.png is in every set
  return Cap(d, t, at, h, draw);
}

Strip PromptStrip(ImDrawList* d, ImVec2 at, float u, Style style, Button button, const std::string& key,
                  const std::string& label, float size, bool green, bool draw) {
  ImFont* f = g_fonts.ui;
  const float s = size * u, gh = s * 1.33f, pad_l = 10 * u, pad_r = 16 * u, gap = 9 * u, h = s * 1.2f + 18 * u;
  const float gw = Glyph(d, style, button, key, ImVec2(0, 0), gh, false);
  const float tw = SpacedWidth(f, s, label, 1 * u);
  Strip out;
  out.a = at, out.b = ImVec2(at.x + pad_l + gw + gap + tw + pad_r, at.y + h);
  out.hovered = ImGui::IsMouseHoveringRect(out.a, out.b);
  out.clicked = out.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  if (!draw) return out;
  d->AddRectFilled(out.a, out.b, green ? kGreen : out.hovered ? IM_COL32(46, 46, 40, 255) : kStrip);
  Glyph(d, style, button, key, ImVec2(at.x + pad_l, at.y + h * 0.5f), gh);
  Spaced(d, f, s, ImVec2(at.x + pad_l + gw + gap, at.y + (h - s * 1.45f) * 0.5f), green ? IM_COL32(255, 255, 255, 255) : kStripText,
         label, 1 * u);
  return out;
}

}  // namespace bf1943::kit
