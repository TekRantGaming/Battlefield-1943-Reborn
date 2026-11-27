// The scoreboards' ping cell (scoreboard_ping.h): shapes and text, sharp at any size.

#include "scoreboard_ping.h"

#include <algorithm>
#include <string>

#include <imgui.h>

namespace bf1943::ping {

int Bars(int ms) { return ms < 0 ? 0 : ms < 60 ? 4 : ms < 120 ? 3 : ms < 200 ? 2 : 1; }

unsigned Colour(int ms) {
  return ms < 60 ? IM_COL32(104, 196, 74, 255) : ms < 120 ? IM_COL32(222, 172, 44, 255) : IM_COL32(226, 70, 52, 255);
}

float DrawBars(ImDrawList* d, float left, float base, float h, int ms, unsigned off) {
  const int lit = Bars(ms);
  const unsigned on = Colour(ms);
  const float step = h * 0.32f, w = h * 0.22f;
  for (int k = 0; k < 4; ++k) {
    const float x = left + float(k) * step, bh = h * (0.34f + 0.22f * float(k));
    d->AddRectFilled(ImVec2(x, base - bh), ImVec2(x + w, base), k < lit ? on : off);
  }
  return 3 * step + w;
}

float CellWidth(float h) { return h * 3.3f; }

float DrawCell(ImDrawList* d, ImFont* font, float right, float mid, float h, int ms, bool bot, bool tag, unsigned ink) {
  if (!bot && ms < 0) return 0;
  const float w = CellWidth(h), left = right - w;
  const float pad = h * 0.18f;
  if (tag) {
    d->AddRectFilled(ImVec2(left, mid - h * 0.5f), ImVec2(right, mid + h * 0.5f), IM_COL32(24, 22, 17, 236), h * 0.18f);
    ink = IM_COL32(240, 233, 212, 255);
  }
  const float size = h * (tag ? 0.9f : 0.78f);
  if (bot) {
    // No connection to show: the word, centred, dimmer than a number.
    const char* word = "BOT";
    const ImVec2 tw = font->CalcTextSizeA(size, 1e9f, 0, word);
    const unsigned dim = (ink & 0x00FFFFFFu) | (tag ? 0xC8000000u : 0xA0000000u);
    d->AddText(font, size, ImVec2(left + (w - tw.x) * 0.5f, mid - tw.y * 0.5f), dim, word);
    return w;
  }
  const unsigned off = tag ? IM_COL32(255, 255, 255, 60) : (ink & 0x00FFFFFFu) | 0x38000000u;
  DrawBars(d, left + pad, mid + h * 0.32f, h * 0.64f, ms, off);
  const std::string n = std::to_string(std::min(ms, 999));
  const ImVec2 tw = font->CalcTextSizeA(size, 1e9f, 0, n.c_str());
  d->AddText(font, size, ImVec2(right - pad - tw.x, mid - tw.y * 0.5f), ink, n.c_str());
  return w;
}

}  // namespace bf1943::ping
