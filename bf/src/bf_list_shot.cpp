// BF's own lists (its main menu's and its pause's), as BF draws them, for ours (TekRantGaming, 6 Nov 2026: our two rows inserted among BF's
// real rows, BF's rows a little tighter, as in the example picture he picked; redrawn letters looked wrong).
//
// When BF's main menu first shows, our list stays hidden for a moment and the game's window is copied once (the
// same PrintWindow the test harness uses, which sees this renderer's swap chain). From BF's list in it (x 37.8% to
// 62.1%, y 43.3% to 77.8%, eight rows) come: each row's letters as a mask (dark letters on paper; the selected first
// row's light letters on green), BF's plain paper (the right end of a short row) and its plain green (the right end
// of its selected first row). They go into ImGui's texture; our list draws BF's rows from them at our row height,
// and our two rows with BF's lettering (bf_font.cpp) on the same paper.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/logging.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace bf1943 {
namespace {

// The lists: BF's main menu list (eight rows) and its pause list ("Frontline Report", seven rows), where BF draws
// them (fractions of the screen; runs origmenu3 and origpause2).
struct ListPlace {
  float x0, x1, y0, row;  // left, right, top, one row's height
  int rows;
};
// BF's rows' words, in order (their letters are cut from the copy to spell our rows' words in BF's own letters).
const char* const kLabels[2][8] = {
    {"QUICK MATCH", "JOIN MATCH WITH FRIENDS", "CREATE PRIVATE MATCH", "STATS & AWARDS", "HELP & OPTIONS",
     "ACHIEVEMENTS", "LEADERBOARDS", "RETURN TO GAME LIBRARY"},
    {"MY SQUAD", "SCOREBOARD", "HELP & OPTIONS", "LEADERBOARDS", "ACHIEVEMENTS", "EXIT GAME", "SUICIDE", ""}};
constexpr ListPlace kPlaces[2] = {{0.3781f, 0.6213f, 0.4333f, (0.7778f - 0.4333f) / 8, 8},
                                  {0.0860f, 0.3350f, 0.3195f, 0.0420f, 7}};

struct Shot {
  bool ok = false;
  int w = 0, row_h = 0, plain_w = 0;   // the list's width, one row's height, the plain strips' width (pixels)
  std::vector<uint8_t> masks;          // kRows rows of w x row_h, alpha
  std::vector<uint8_t> paper, green;   // plain_w x row_h RGBA each
  ImFontAtlasRectId mask_rect = ImFontAtlasRectId_Invalid, paper_rect = ImFontAtlasRectId_Invalid,
                    green_rect = ImFontAtlasRectId_Invalid;
  float display_w = 0, display_h = 0;  // the display size it was taken at
  int selected = 0;                    // BF's green row when it was taken
  struct Cut {
    int row, x, w;  // a letter in the masks: its row, left and width (pixels)
    int dy = 0;     // its row's letters' top minus the usual top (pixels): moved up by that when drawn
  };
  std::map<char, Cut> letters;         // BF's letters, from its rows
  int left = 0, gap = 0, space = 0;    // the rows' left margin, the gap between letters, a word space (pixels)
  float text_mid = 0.5f;               // the letters' middle in a row, as a fraction of its height
} g_shots[2];

#if defined(_WIN32)
struct FindWindowData {
  DWORD pid;
  HWND best;
  LONG best_area;
};
BOOL CALLBACK FindOwnWindow(HWND h, LPARAM p) {
  auto* d = reinterpret_cast<FindWindowData*>(p);
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (pid != d->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
  RECT r;
  if (!GetClientRect(h, &r)) return TRUE;
  const LONG area = (r.right - r.left) * (r.bottom - r.top);
  if (area > d->best_area) d->best = h, d->best_area = area;
  return TRUE;
}

// The game window's client area as RGBA (top row first), or empty.
std::vector<uint8_t> CaptureWindow(int& w, int& h) {
  FindWindowData d{GetCurrentProcessId(), nullptr, 0};
  EnumWindows(FindOwnWindow, reinterpret_cast<LPARAM>(&d));
  if (!d.best) return {};
  RECT r;
  GetClientRect(d.best, &r);
  w = r.right - r.left, h = r.bottom - r.top;
  if (w <= 0 || h <= 0) return {};
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;  // top down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  std::vector<uint8_t> out;
  if (bmp && bits) {
    HGDIOBJ old = SelectObject(mem, bmp);
    if (PrintWindow(d.best, mem, 3)) {  // PW_CLIENTONLY | PW_RENDERFULLCONTENT
      out.resize(size_t(w) * h * 4);
      const auto* src = static_cast<const uint8_t*>(bits);
      for (size_t i = 0; i < size_t(w) * h; ++i)  // BGRA to RGBA
        out[i * 4] = src[i * 4 + 2], out[i * 4 + 1] = src[i * 4 + 1], out[i * 4 + 2] = src[i * 4], out[i * 4 + 3] = 255;
    }
    SelectObject(mem, old);
  }
  if (bmp) DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return out;
}
#else
std::vector<uint8_t> CaptureWindow(int&, int&) { return {}; }
#endif

int Lum(const uint8_t* p) { return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000; }

ImFontAtlasRectId Upload(int w, int h, const std::vector<uint8_t>& rgba_or_alpha, bool alpha_only) {
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  ImFontAtlasRect r;
  const ImFontAtlasRectId id = atlas->AddCustomRect(w, h, &r);
  if (id == ImFontAtlasRectId_Invalid || !atlas->TexData || !atlas->TexData->Pixels) return ImFontAtlasRectId_Invalid;
  ImTextureData* tex = atlas->TexData;
  for (int y = 0; y < h; ++y) {
    auto* row = static_cast<uint8_t*>(tex->GetPixelsAt(r.x, r.y + y));
    for (int x = 0; x < w; ++x) {
      const size_t i = size_t(y) * w + x;
      if (tex->Format == ImTextureFormat_Alpha8) {
        row[x] = alpha_only ? rgba_or_alpha[i] : 255;
      } else if (alpha_only) {
        row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 255, row[x * 4 + 3] = rgba_or_alpha[i];
      } else {
        std::memcpy(row + x * 4, &rgba_or_alpha[i * 4], 4);
      }
    }
  }
  return id;
}

bool Rect(ImFontAtlasRectId id, ImFontAtlasRect& r) {
  return id != ImFontAtlasRectId_Invalid && ImGui::GetIO().Fonts->GetCustomRect(id, &r);
}

}  // namespace

namespace {
std::filesystem::path g_dir;  // the user's cache\extracted: the copies kept for the next start

std::filesystem::path KeptPath(int list) { return g_dir / ("bf_list_" + std::to_string(list) + ".bin"); }

// The list's part of the window (its rectangle, RGBA) kept, with the window's and the display's sizes: the next start
// uses it at once instead of waiting for BF's list and copying the window again (TekRantGaming, 9 Nov: PC settings took a
// second or two to show in the pause). Kept in the user's cache only; it is BF's own lettering.
void Keep(int list, const std::vector<uint8_t>& img, int W, int H, ImVec2 ds) {
  if (g_dir.empty()) return;
  const ListPlace& L = kPlaces[list];
  const int x0 = int(W * L.x0), x1 = int(W * L.x1), y0 = int(H * L.y0), y1 = int(H * (L.y0 + L.row * L.rows));
  std::error_code ec;
  std::filesystem::create_directories(g_dir, ec);
  std::ofstream f(KeptPath(list), std::ios::binary);
  const int32_t head[6] = {W, H, int32_t(ds.x), int32_t(ds.y), x1 - x0, y1 - y0};
  f.write(reinterpret_cast<const char*>(head), sizeof(head));
  for (int y = y0; y < y1; ++y) f.write(reinterpret_cast<const char*>(&img[(size_t(y) * W + x0) * 4]), (x1 - x0) * 4);
}
}  // namespace

void BfListShotSetDir(const std::filesystem::path& dir) { g_dir = dir; }

bool BfListShotProcess(int list, const std::vector<uint8_t>& img, int W, int H);

// Takes the copy now (BF's own list must be on screen, ours not drawn this frame or the last few). True when done.
bool BfListShotTake(int list) {
  int W = 0, H = 0;
  const std::vector<uint8_t> img = CaptureWindow(W, H);
  if (img.empty()) {
    REXLOG_WARN("BF list copy: the window could not be copied");
    return false;
  }
  if (!BfListShotProcess(list, img, W, H)) return false;
  Keep(list, img, W, H, ImGui::GetIO().DisplaySize);
  return true;
}

// Whether BF's own list is on screen now with its green (selected) bar on `row` (one copy of the window: while a
// menu is up, not in play). The pause list coming back after BF's own screens (TekRantGaming, 9 Nov: Help & Options was
// janky; its How to Play pages back with B, so where B leads cannot be counted). Its green bar on `row`, and every
// other row's letters where the copy of BF's list has them (another of BF's lists, Help & Options' three rows, has
// its own words and fewer rows).
bool BfListShotShowing(int list, int row) {
  const Shot& g = g_shots[list];
  if (!g.ok) return false;
  int W = 0, H = 0;
  const std::vector<uint8_t> img = CaptureWindow(W, H);
  if (img.empty()) return false;
  const ListPlace& L = kPlaces[list];
  const int x0 = int(W * L.x0), x1 = int(W * L.x1), y0 = int(H * L.y0), y1 = int(H * (L.y0 + L.row * L.rows));
  const int w = x1 - x0, row_h = (y1 - y0) / L.rows;
  if (w != g.w || row_h != g.row_h) return false;  // another window size than the copy's
  const int plain_x = x1 - g.plain_w - std::max(16, w / 30);
  auto px = [&](int x, int y) { return &img[(size_t(y) * W + x) * 4]; };
  const uint8_t* green = px(plain_x + g.plain_w / 2, y0 + row * row_h + row_h / 2);
  bool ok = green[1] > green[0] + 15 && green[1] > green[2] + 25;
  int agree = 0, total = 0;
  for (int r = 0; r < L.rows && ok; ++r) {
    if (r == row || r == g.selected) continue;  // the copy's letters there were light on green
    for (int y = row_h * 3 / 10; y < row_h * 7 / 10; y += 2)
      for (int x = g.left; x < w * 3 / 5; x += 2) {
        const bool ink_now = Lum(px(x0 + x, y0 + r * row_h + y)) < 110;
        const bool ink_copy = g.masks[(size_t(r) * row_h + y) * w + x] > 160;
        agree += ink_now == ink_copy, ++total;
      }
  }
  const double share = total ? double(agree) / total : 0.0;
  ok = ok && share > 0.93;
  REXLOG_INFO("BF list copy: BF's list {} {} with row {} selected ({:.0f}% of its letters as copied)", list,
              ok ? "is on screen" : "is not on screen", row, share * 100);
  return ok;
}

// BF's own list on screen now: the row its green selection bar is on (the green of the plain strip at each row's
// right end), or -1 (not on screen, another window size than the copy's, or no row green). Ours must not be drawn
// over it then.
int BfListShotSelectedRow(int list) {
  const Shot& g = g_shots[list];
  if (!g.ok) return -1;
  int W = 0, H = 0;
  const std::vector<uint8_t> img = CaptureWindow(W, H);
  if (img.empty()) return -1;
  const ListPlace& L = kPlaces[list];
  const int x0 = int(W * L.x0), x1 = int(W * L.x1), y0 = int(H * L.y0), y1 = int(H * (L.y0 + L.row * L.rows));
  const int w = x1 - x0, row_h = (y1 - y0) / L.rows;
  if (w != g.w || row_h != g.row_h) return -1;
  const int plain_x = x1 - g.plain_w - std::max(16, w / 30);
  int found = -1, greens = 0;
  for (int r = 0; r < L.rows; ++r) {
    const uint8_t* p = &img[(size_t(y0 + r * row_h + row_h / 2) * W + plain_x + g.plain_w / 2) * 4];
    if (p[1] > p[0] + 15 && p[1] > p[2] + 25) found = r, ++greens;
  }
  REXLOG_INFO("BF list copy: BF's list {}: its selection on row {} ({} green rows)", list, found, greens);
  return greens == 1 ? found : -1;
}

// A copy kept from an earlier start, taken at this display size: used now. True when it was.
bool BfListShotLoad(int list) {
  // The file read once; its processing tried on up to 60 frames (ImGui's atlas can refuse a new rectangle on a frame).
  static bool read[2] = {};
  static int tries[2] = {};
  static std::vector<uint8_t> kept[2];
  static int kept_w[2] = {}, kept_h[2] = {};
  if (g_dir.empty() || tries[list] >= 60) return false;
  if (read[list]) {
    if (kept[list].empty()) return false;
    ++tries[list];
    if (!BfListShotProcess(list, kept[list], kept_w[list], kept_h[list])) {
      if (tries[list] == 60) REXLOG_INFO("BF list copy: the kept copy of list {} could not be used", list);
      return false;
    }
    kept[list].clear(), kept[list].shrink_to_fit();
    REXLOG_INFO("BF list copy: BF's list {} from the copy kept at an earlier start", list);
    return true;
  }
  read[list] = true;
  std::ifstream f(KeptPath(list), std::ios::binary);
  int32_t head[6] = {};
  if (!f.read(reinterpret_cast<char*>(head), sizeof(head))) return false;
  const ImVec2 ds = ImGui::GetIO().DisplaySize;
  const int W = head[0], H = head[1];
  if (W <= 0 || H <= 0 || W > 16384 || H > 16384 || head[2] != int32_t(ds.x) || head[3] != int32_t(ds.y)) {
    REXLOG_INFO("BF list copy: the kept copy of list {} was taken at {}x{}, the display is {}x{}: not used", list,
                head[2], head[3], int(ds.x), int(ds.y));
    return false;
  }
  const ListPlace& L = kPlaces[list];
  const int x0 = int(W * L.x0), x1 = int(W * L.x1), y0 = int(H * L.y0), y1 = int(H * (L.y0 + L.row * L.rows));
  if (head[4] != x1 - x0 || head[5] != y1 - y0) return false;
  std::vector<uint8_t> img(size_t(W) * H * 4, 0);
  for (int y = y0; y < y1; ++y)
    if (!f.read(reinterpret_cast<char*>(&img[(size_t(y) * W + x0) * 4]), (x1 - x0) * 4)) return false;
  kept[list] = std::move(img), kept_w[list] = W, kept_h[list] = H;
  return BfListShotLoad(list);
}

// The list's rows from a copy of the window (W x H, RGBA).
bool BfListShotProcess(int list, const std::vector<uint8_t>& img, int W, int H) {
  const ListPlace& L = kPlaces[list];
  const int kRows = L.rows;
  const ImVec2 ds = ImGui::GetIO().DisplaySize;
  const int x0 = int(W * L.x0), x1 = int(W * L.x1), y0 = int(H * L.y0), y1 = int(H * (L.y0 + L.row * kRows));
  Shot s;
  s.w = x1 - x0;
  s.row_h = (y1 - y0) / kRows;
  if (s.w < 64 || s.row_h < 8) return false;
  auto px = [&](int x, int y) { return &img[(size_t(y) * W + x) * 4]; };
  // BF's paper and green: the plain right end of a row (no letters there): its green row (which is BF's selected
  // one) and a paper row next to it.
  // Wide, and clear of the list's frame on the right (its ends showed as seams when tiled: runs menu4b, pause2c).
  s.plain_w = s.w * 2 / 5;
  const int plain_x = x1 - s.plain_w - std::max(16, s.w / 30);
  auto green_at = [&](int r) {
    const uint8_t* g = px(plain_x + s.plain_w / 2, y0 + r * s.row_h + s.row_h / 2);
    return g[1] > g[0] + 15 && g[1] > g[2] + 25;
  };
  s.selected = -1;
  for (int r = 0; r < kRows && s.selected < 0; ++r)
    if (green_at(r)) s.selected = r;
  if (s.selected < 0) {
    REXLOG_WARN("BF list copy: BF's list {} is not on screen", list);
    return false;
  }
  const int paper_row = s.selected == 3 ? 4 : 3;  // a short row (main: Stats & Awards; pause: Leaderboards)
  s.paper.resize(size_t(s.plain_w) * s.row_h * 4), s.green.resize(s.paper.size());
  for (int y = 0; y < s.row_h; ++y)
    for (int x = 0; x < s.plain_w; ++x) {
      std::memcpy(&s.paper[(size_t(y) * s.plain_w + x) * 4], px(plain_x + x, y0 + paper_row * s.row_h + y), 4);
      std::memcpy(&s.green[(size_t(y) * s.plain_w + x) * 4], px(plain_x + x, y0 + s.selected * s.row_h + y), 4);
    }
  // Each row's letters: dark on paper, light on green (the selected row). The rows' edges (their lines) are left
  // out: a few pixels top and bottom, and the list's frame at the sides.
  s.masks.assign(size_t(kRows) * s.w * s.row_h, 0);
  const int edge = std::max(2, s.row_h / 10), side = std::max(3, s.w / 120);
  for (int r = 0; r < kRows; ++r)
    for (int y = edge; y < s.row_h - edge; ++y)
      for (int x = side; x < s.w - side; ++x) {
        const int l = Lum(px(x0 + x, y0 + r * s.row_h + y));
        const int a = r == s.selected ? (l - 105) * 255 / 70 : (175 - l) * 255 / 150;
        s.masks[(size_t(r) * s.row_h + y) * s.w + x] = uint8_t(std::clamp(a, 0, 255));
      }
  // The selected row's letters were light on green with BF's dark edge round them: one pixel more each way, so they
  // are as heavy as the other rows' when drawn dark.
  {
    const size_t at = size_t(s.selected) * s.w * s.row_h;
    std::vector<uint8_t> row0(s.masks.begin() + at, s.masks.begin() + at + size_t(s.w) * s.row_h);
    for (int y = 1; y < s.row_h - 1; ++y)
      for (int x = 1; x < s.w - 1; ++x) {
        uint8_t m = 0;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) m = std::max(m, row0[size_t(y + dy) * s.w + x + dx]);
        s.masks[at + size_t(y) * s.w + x] = m;
      }
  }
  // BF's letters: each row's ink split into runs of columns, matched to the row's known words. A row whose runs
  // don't match its letters one for one (two letters touching) gives none. The selected row's are left for last
  // (its letters were thickened above).
  {
    std::vector<int> gaps, spaces, lefts, tops(kRows, -1);
    for (int r = 0; r < kRows; ++r)  // each row's capitals' top
      for (int y = 0; y < s.row_h && tops[r] < 0; ++y)
        for (int x = 0; x < s.w; ++x)
          if (s.masks[(size_t(r) * s.row_h + y) * s.w + x] > 160) {
            tops[r] = y;
            break;
          }
    for (int pass = 0; pass < 2; ++pass)
      for (int r = 0; r < kRows; ++r) {
        if ((r == s.selected) != (pass == 1)) continue;
        const std::string label = kLabels[list][r];
        std::string chars;
        for (char c : label)
          if (c != ' ') chars += c;
        std::vector<std::pair<int, int>> runs;  // x, width
        int start = -1;
        for (int x = 0; x <= s.w; ++x) {
          bool ink = false;
          if (x < s.w)
            for (int y = 0; y < s.row_h && !ink; ++y) ink = s.masks[(size_t(r) * s.row_h + y) * s.w + x] > 160;
          if (ink && start < 0) start = x;
          if (!ink && start >= 0) runs.push_back({start, x - start}), start = -1;
        }
        if (runs.size() != chars.size() || chars.empty()) {
          REXLOG_INFO("BF list copy: row {} ({}): {} ink runs for {} letters", r, label, runs.size(), chars.size());
          continue;
        }
        if (pass == 0) lefts.push_back(runs[0].first);
        size_t k = 0;
        for (size_t i = 0; i < label.size(); ++i) {
          if (label[i] == ' ') continue;
          if (!s.letters.count(label[i])) s.letters[label[i]] = {r, runs[k].first, runs[k].second, tops[r]};
          if (k + 1 < runs.size() && pass == 0) {
            const int g = runs[k + 1].first - runs[k].first - runs[k].second;
            (i + 1 < label.size() && label[i + 1] == ' ' ? spaces : gaps).push_back(g);
          }
          ++k;
        }
      }
    auto median = [](std::vector<int> v, int fallback) {
      if (v.empty()) return fallback;
      std::sort(v.begin(), v.end());
      return v[v.size() / 2];
    };
    s.left = median(lefts, s.w / 35);
    std::vector<int> known;
    for (int t : tops)
      if (t >= 0) known.push_back(t);
    const int usual = median(known, 0);
    for (auto& [c, cut] : s.letters) cut.dy -= usual;
    // The capitals are as high as BF's: from the usual top, the height of an I (a plain capital).
    int cap = s.row_h / 2;
    if (auto it = s.letters.find('I'); it != s.letters.end()) {
      const auto& c = it->second;
      for (int y = s.row_h - 1; y >= 0; --y)
        if (s.masks[(size_t(c.row) * s.row_h + y) * s.w + c.x + c.w / 2] > 160) {
          cap = y - tops[c.row] + 1;
          break;
        }
    }
    s.text_mid = (usual + cap * 0.5f) / s.row_h;
    s.gap = median(gaps, s.row_h / 18);
    s.space = median(spaces, s.row_h / 4);
  }
  s.mask_rect = Upload(s.w, s.row_h * kRows, s.masks, true);
  s.paper_rect = Upload(s.plain_w, s.row_h, s.paper, false);
  s.green_rect = Upload(s.plain_w, s.row_h, s.green, false);
  if (s.mask_rect == ImFontAtlasRectId_Invalid || s.paper_rect == ImFontAtlasRectId_Invalid ||
      s.green_rect == ImFontAtlasRectId_Invalid) {
    static int logged = 0;
    if (logged++ < 3) REXLOG_INFO("BF list copy: ImGui's atlas took no room for list {} on this frame", list);
    return false;
  }
  s.ok = true;
  s.display_w = ds.x, s.display_h = ds.y;
  g_shots[list] = std::move(s);
  std::string have;
  for (const auto& [c, cut] : g_shots[list].letters) have += c;
  REXLOG_INFO("BF list copy: BF's list {} from the {}x{} window (rows {}x{} px, row {} selected, letters {})", list,
              W, H, g_shots[list].w, g_shots[list].row_h, g_shots[list].selected, have);
  return true;
}

// The copy is there and was taken at this display size.
// Our row's words in BF's own letters (cut from its rows), at the size BF draws them. True when every letter was
// there (else nothing is drawn, and the caller draws them itself).
bool BfListShotLabel(int list, ImDrawList* d, ImVec2 a, ImVec2 b, const std::string& text, ImU32 color, ImU32 edge) {
  const Shot& g = g_shots[list];
  for (char c : text)
    if (c != ' ' && !g.letters.count(c)) return false;
  ImFontAtlasRect r;
  if (!Rect(g.mask_rect, r)) return false;
  const int kRows = kPlaces[list].rows;
  const float sx = g.display_w / float(g.w) * (kPlaces[list].x1 - kPlaces[list].x0);  // display units per pixel
  const float h = b.y - a.y, full = kPlaces[list].row * g.display_h;
  const float keep = std::min(1.0f, h / full), v0 = std::clamp(g.text_mid - keep * 0.5f, 0.0f, 1.0f - keep);
  const float du = (r.uv1.x - r.uv0.x) / g.w, rv = (r.uv1.y - r.uv0.y) / kRows;
  auto draw = [&](ImVec2 o, ImU32 col) {
    float x = a.x + g.left * sx;
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == ' ') {
        x += (g.space - g.gap) * sx;
        continue;
      }
      const auto& c = g.letters.at(text[i]);
      const float sh = float(c.dy) / g.row_h;  // its row's letters sat lower: sampled lower, drawn level
      const ImVec2 uv0(r.uv0.x + c.x * du, r.uv0.y + rv * (c.row + v0 + sh)),
          uv1(r.uv0.x + (c.x + c.w) * du, r.uv0.y + rv * (c.row + v0 + keep + sh));
      d->AddImage(ImGui::GetIO().Fonts->TexRef, ImVec2(x + o.x, a.y + o.y), ImVec2(x + c.w * sx + o.x, b.y + o.y), uv0, uv1,
                  col);
      x += (c.w + g.gap) * sx;
    }
  };
  if (edge) {
    const float e = g.display_h / 1800.0f * 1.6f;
    for (const ImVec2 o : {ImVec2(-e, 0), ImVec2(e, 0), ImVec2(0, -e), ImVec2(0, e), ImVec2(e, e)}) draw(o, edge);
  }
  draw(ImVec2(0, 0), color);
  return true;
}

bool BfListShotReady(int list) {
  const ImVec2 ds = ImGui::GetIO().DisplaySize;
  const Shot& g_shot = g_shots[list];
  return g_shot.ok && g_shot.display_w == ds.x && g_shot.display_h == ds.y;
}

// One row's paper (BF's own, or its green when selected), tiled along a..b, cut to the row's height from the middle.
void BfListShotBar(int list, ImDrawList* d, ImVec2 a, ImVec2 b, bool selected) {
  const Shot& g_shot = g_shots[list];
  ImFontAtlasRect r;
  if (!Rect(selected ? g_shot.green_rect : g_shot.paper_rect, r)) return;
  const float h = b.y - a.y, full = kPlaces[list].row * g_shot.display_h;  // BF's row height (display units)
  const float keep = std::min(1.0f, h / full), v0 = (1 - keep) * 0.5f;
  const float tile = full * float(g_shot.plain_w) / float(g_shot.row_h);
  // Every other tile mirrored, so the strip's ends meet themselves and no seam shows (run menu4a).
  int n = 0;
  for (float x = a.x; x < b.x; x += tile, ++n) {
    const float x1 = std::min(b.x, x + tile), f = (x1 - x) / tile;
    // A texel in from the strip's ends: filtering there read the next rectangle in the texture (a dark seam).
    const float tx = (r.uv1.x - r.uv0.x) / g_shot.plain_w * 1.5f;
    const float u0 = r.uv0.x + tx, ue = r.uv1.x - tx, u1 = u0 + (ue - u0) * f;
    const float va = r.uv0.y + (r.uv1.y - r.uv0.y) * v0, vb = r.uv0.y + (r.uv1.y - r.uv0.y) * (v0 + keep);
    if (n % 2 == 0)
      d->AddImage(ImGui::GetIO().Fonts->TexRef, ImVec2(x, a.y), ImVec2(x1, b.y), ImVec2(u0, va), ImVec2(u1, vb));
    else
      d->AddImage(ImGui::GetIO().Fonts->TexRef, ImVec2(x, a.y), ImVec2(x1, b.y), ImVec2(ue, va),
                  ImVec2(ue - (u1 - u0), vb));
  }
}

// BF's row `bf_row`'s own letters in `color`, at BF's size, its middle on the middle of a..b (the row may be shorter
// than BF's: its empty top and bottom are cut).
void BfListShotLetters(int list, ImDrawList* d, ImVec2 a, ImVec2 b, int bf_row, ImU32 color, ImU32 edge) {
  const Shot& g_shot = g_shots[list];
  const int kRows = kPlaces[list].rows;
  ImFontAtlasRect r;
  if (bf_row < 0 || bf_row >= kRows || !Rect(g_shot.mask_rect, r)) return;
  const float h = b.y - a.y, full = kPlaces[list].row * g_shot.display_h;
  const float keep = std::min(1.0f, h / full), v0 = std::clamp(g_shot.text_mid - keep * 0.5f, 0.0f, 1.0f - keep);
  const float rv = (r.uv1.y - r.uv0.y) / kRows;
  const ImVec2 uv0(r.uv0.x, r.uv0.y + rv * (bf_row + v0)), uv1(r.uv1.x, r.uv0.y + rv * (bf_row + v0 + keep));
  const float w = (kPlaces[list].x1 - kPlaces[list].x0) * g_shot.display_w;
  // The selected row's letters have a dark edge round them (BF's own green row): the letters in it first.
  if (edge) {
    const float e = g_shot.display_h / 1800.0f * 1.6f;
    for (const ImVec2 o : {ImVec2(-e, 0), ImVec2(e, 0), ImVec2(0, -e), ImVec2(0, e), ImVec2(e, e)})
      d->AddImage(ImGui::GetIO().Fonts->TexRef, ImVec2(a.x + o.x, a.y + o.y), ImVec2(a.x + w + o.x, b.y + o.y), uv0, uv1,
                  edge);
  }
  d->AddImage(ImGui::GetIO().Fonts->TexRef, a, ImVec2(a.x + w, b.y), uv0, uv1, color);
}

}  // namespace bf1943
