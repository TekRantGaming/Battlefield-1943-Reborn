// Server regions (TekRantGaming, 13 Nov 2026: "server browser now shows region including a nice little country flag
// to show users"; the server package's region drop down). A server says where it is with one of these codes when it
// registers (gm_host.cpp bf_gm_region, server/gmhost.py --region); the backend passes it on with the list
// (server/plasma.py listing); the browser shows the place's name and its country's flag. The flags are drawn here
// with a few shapes each, made to be read at a small size: nothing is shipped or downloaded for them.
// The same list is in the server manager (server/manager.py REGIONS): keep the two the same.

#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include <imgui.h>

namespace bf1943 {

struct Region {
  const char* code;     // what a server registers with
  const char* name;     // shown in the browser
  const char* country;  // the flag (two letters)
};

inline constexpr Region kRegions[] = {
    {"us-east", "US East", "us"},      {"us-central", "US Central", "us"}, {"us-west", "US West", "us"},
    {"ca", "Canada", "ca"},            {"mx", "Mexico", "mx"},             {"br", "Brazil", "br"},
    {"ar", "Argentina", "ar"},         {"cl", "Chile", "cl"},              {"gb", "United Kingdom", "gb"},
    {"ie", "Ireland", "ie"},           {"fr", "France", "fr"},             {"de", "Germany", "de"},
    {"nl", "Netherlands", "nl"},       {"be", "Belgium", "be"},            {"es", "Spain", "es"},
    {"pt", "Portugal", "pt"},          {"it", "Italy", "it"},              {"ch", "Switzerland", "ch"},
    {"at", "Austria", "at"},           {"pl", "Poland", "pl"},             {"cz", "Czechia", "cz"},
    {"se", "Sweden", "se"},            {"no", "Norway", "no"},             {"fi", "Finland", "fi"},
    {"dk", "Denmark", "dk"},           {"ua", "Ukraine", "ua"},            {"tr", "Turkey", "tr"},
    {"il", "Israel", "il"},            {"ae", "UAE", "ae"},                {"za", "South Africa", "za"},
    {"in", "India", "in"},             {"sg", "Singapore", "sg"},          {"jp", "Japan", "jp"},
    {"kr", "South Korea", "kr"},       {"hk", "Hong Kong", "hk"},          {"tw", "Taiwan", "tw"},
    {"au", "Australia", "au"},         {"nz", "New Zealand", "nz"},
};

// The region a code names, or null (no region said, or one this version does not know).
inline const Region* FindRegion(const std::string& code) {
  for (const Region& r : kRegions)
    if (code == r.code) return &r;
  return nullptr;
}

namespace flag_detail {

inline constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255), kBlack = IM_COL32(20, 20, 20, 255);
inline constexpr ImU32 kRed = IM_COL32(200, 16, 46, 255), kBlue = IM_COL32(0, 40, 120, 255);
inline constexpr ImU32 kGreen = IM_COL32(0, 132, 61, 255), kYellow = IM_COL32(254, 209, 0, 255);
inline constexpr ImU32 kSky = IM_COL32(108, 172, 228, 255), kOrange = IM_COL32(255, 130, 40, 255);

struct Box {
  ImDrawList* d;
  ImVec2 a, b;
  float W() const { return b.x - a.x; }
  float H() const { return b.y - a.y; }
  ImVec2 P(float u, float v) const { return ImVec2(a.x + W() * u, a.y + H() * v); }
  void Rect(float u0, float v0, float u1, float v1, ImU32 c) const { d->AddRectFilled(P(u0, v0), P(u1, v1), c); }
  void Disc(float u, float v, float r, ImU32 c) const { d->AddCircleFilled(P(u, v), H() * r, c, 20); }
  void Line(float u0, float v0, float u1, float v1, float w, ImU32 c) const { d->AddLine(P(u0, v0), P(u1, v1), c, H() * w); }
  void Tri(float u0, float v0, float u1, float v1, float u2, float v2, ImU32 c) const {
    d->AddTriangleFilled(P(u0, v0), P(u1, v1), P(u2, v2), c);
  }
  void H3(ImU32 c0, ImU32 c1, ImU32 c2) const {
    Rect(0, 0, 1, 1.0f / 3, c0), Rect(0, 1.0f / 3, 1, 2.0f / 3, c1), Rect(0, 2.0f / 3, 1, 1, c2);
  }
  void V3(ImU32 c0, ImU32 c1, ImU32 c2) const {
    Rect(0, 0, 1.0f / 3, 1, c0), Rect(1.0f / 3, 0, 2.0f / 3, 1, c1), Rect(2.0f / 3, 0, 1, 1, c2);
  }
  // A Nordic cross: the field, the cross (and a wider one under it for a border).
  void Cross(ImU32 field, ImU32 cross, ImU32 border = 0) const {
    Rect(0, 0, 1, 1, field);
    if (border) Rect(0.26f, 0, 0.48f, 1, border), Rect(0, 0.36f, 1, 0.64f, border);
    Rect(0.31f, 0, 0.43f, 1, cross), Rect(0, 0.42f, 1, 0.58f, cross);
  }
  // The Union Flag in the box u0..u1, v0..v1 (the whole flag, or a canton).
  void Union(float u0, float v0, float u1, float v1) const {
    const Box c{d, P(u0, v0), P(u1, v1)};
    d->PushClipRect(c.a, c.b, true);
    c.Rect(0, 0, 1, 1, kBlue);
    c.Line(0, 0, 1, 1, 0.22f, kWhite), c.Line(0, 1, 1, 0, 0.22f, kWhite);
    c.Line(0, 0, 1, 1, 0.08f, kRed), c.Line(0, 1, 1, 0, 0.08f, kRed);
    c.Rect(0.40f, 0, 0.60f, 1, kWhite), c.Rect(0, 0.33f, 1, 0.67f, kWhite);
    c.Rect(0.44f, 0, 0.56f, 1, kRed), c.Rect(0, 0.40f, 1, 0.60f, kRed);
    d->PopClipRect();
  }
};

}  // namespace flag_detail

// The flag of `country` (two letters, as kRegions has them) filling a..b, with a thin edge; an unknown one is a grey
// plate. Drawn clipped to its box.
inline void DrawFlag(ImDrawList* d, const std::string& country, ImVec2 a, ImVec2 b) {
  using namespace flag_detail;
  const Box f{d, a, b};
  d->PushClipRect(a, b, true);
  const std::string& c = country;
  if (c == "us") {
    for (int i = 0; i < 7; ++i) f.Rect(0, float(i) / 7, 1, float(i + 1) / 7, i % 2 ? kWhite : kRed);
    f.Rect(0, 0, 0.44f, 4.0f / 7, kBlue);
    for (int r = 0; r < 3; ++r)
      for (int s = 0; s < 4; ++s) f.Disc(0.07f + 0.10f * float(s), 0.11f + 0.18f * float(r), 0.035f, kWhite);
  } else if (c == "gb") {
    f.Union(0, 0, 1, 1);
  } else if (c == "ca") {
    f.Rect(0, 0, 1, 1, kWhite), f.Rect(0, 0, 0.25f, 1, kRed), f.Rect(0.75f, 0, 1, 1, kRed);
    f.Tri(0.5f, 0.16f, 0.36f, 0.62f, 0.64f, 0.62f, kRed), f.Tri(0.30f, 0.36f, 0.70f, 0.36f, 0.5f, 0.70f, kRed);  // the leaf
    f.Rect(0.485f, 0.62f, 0.515f, 0.84f, kRed);
  } else if (c == "mx") {
    f.V3(IM_COL32(0, 104, 71, 255), kWhite, kRed), f.Disc(0.5f, 0.5f, 0.13f, IM_COL32(140, 100, 50, 255));
  } else if (c == "br") {
    f.Rect(0, 0, 1, 1, IM_COL32(0, 156, 59, 255));
    d->AddQuadFilled(f.P(0.5f, 0.10f), f.P(0.92f, 0.5f), f.P(0.5f, 0.90f), f.P(0.08f, 0.5f), kYellow);
    f.Disc(0.5f, 0.5f, 0.24f, IM_COL32(0, 39, 118, 255));
  } else if (c == "ar") {
    f.H3(kSky, kWhite, kSky), f.Disc(0.5f, 0.5f, 0.10f, IM_COL32(246, 180, 14, 255));
  } else if (c == "cl") {
    f.Rect(0, 0, 1, 0.5f, kWhite), f.Rect(0, 0.5f, 1, 1, kRed), f.Rect(0, 0, 0.33f, 0.5f, kBlue), f.Disc(0.165f, 0.25f, 0.10f, kWhite);
  } else if (c == "ie") {
    f.V3(IM_COL32(22, 155, 98, 255), kWhite, kOrange);
  } else if (c == "fr") {
    f.V3(IM_COL32(0, 38, 153, 255), kWhite, IM_COL32(237, 41, 57, 255));
  } else if (c == "de") {
    f.H3(kBlack, IM_COL32(221, 0, 0, 255), IM_COL32(255, 206, 0, 255));
  } else if (c == "nl") {
    f.H3(IM_COL32(174, 28, 40, 255), kWhite, IM_COL32(33, 70, 139, 255));
  } else if (c == "be") {
    f.V3(kBlack, IM_COL32(253, 218, 36, 255), IM_COL32(239, 51, 64, 255));
  } else if (c == "es") {
    f.Rect(0, 0, 1, 1, IM_COL32(170, 21, 27, 255)), f.Rect(0, 0.25f, 1, 0.75f, IM_COL32(241, 191, 0, 255));
  } else if (c == "pt") {
    f.Rect(0, 0, 1, 1, IM_COL32(218, 41, 28, 255)), f.Rect(0, 0, 0.4f, 1, IM_COL32(4, 106, 56, 255)), f.Disc(0.4f, 0.5f, 0.17f, kYellow);
  } else if (c == "it") {
    f.V3(IM_COL32(0, 146, 70, 255), kWhite, IM_COL32(206, 43, 55, 255));
  } else if (c == "ch") {
    f.Rect(0, 0, 1, 1, IM_COL32(218, 41, 28, 255)), f.Rect(0.44f, 0.20f, 0.56f, 0.80f, kWhite), f.Rect(0.30f, 0.41f, 0.70f, 0.59f, kWhite);
  } else if (c == "at") {
    f.H3(IM_COL32(200, 16, 46, 255), kWhite, IM_COL32(200, 16, 46, 255));
  } else if (c == "pl") {
    f.Rect(0, 0, 1, 0.5f, kWhite), f.Rect(0, 0.5f, 1, 1, IM_COL32(220, 20, 60, 255));
  } else if (c == "cz") {
    f.Rect(0, 0, 1, 0.5f, kWhite), f.Rect(0, 0.5f, 1, 1, IM_COL32(215, 20, 26, 255)), f.Tri(0, 0, 0.5f, 0.5f, 0, 1, IM_COL32(17, 69, 126, 255));
  } else if (c == "se") {
    f.Cross(IM_COL32(0, 106, 167, 255), IM_COL32(254, 204, 2, 255));
  } else if (c == "no") {
    f.Cross(IM_COL32(186, 12, 47, 255), IM_COL32(0, 32, 91, 255), kWhite);
  } else if (c == "fi") {
    f.Cross(kWhite, IM_COL32(0, 47, 108, 255));
  } else if (c == "dk") {
    f.Cross(IM_COL32(200, 16, 46, 255), kWhite);
  } else if (c == "ua") {
    f.Rect(0, 0, 1, 0.5f, IM_COL32(0, 87, 183, 255)), f.Rect(0, 0.5f, 1, 1, IM_COL32(255, 215, 0, 255));
  } else if (c == "tr") {
    f.Rect(0, 0, 1, 1, IM_COL32(227, 10, 23, 255)), f.Disc(0.36f, 0.5f, 0.26f, kWhite), f.Disc(0.42f, 0.5f, 0.21f, IM_COL32(227, 10, 23, 255));
    f.Disc(0.60f, 0.5f, 0.07f, kWhite);
  } else if (c == "il") {
    f.Rect(0, 0, 1, 1, kWhite), f.Rect(0, 0.12f, 1, 0.26f, IM_COL32(0, 56, 184, 255)), f.Rect(0, 0.74f, 1, 0.88f, IM_COL32(0, 56, 184, 255));
    f.Tri(0.5f, 0.34f, 0.41f, 0.59f, 0.59f, 0.59f, IM_COL32(0, 56, 184, 255)), f.Tri(0.5f, 0.66f, 0.41f, 0.41f, 0.59f, 0.41f, IM_COL32(0, 56, 184, 255));
  } else if (c == "ae") {
    f.H3(IM_COL32(0, 115, 47, 255), kWhite, kBlack), f.Rect(0, 0, 0.26f, 1, IM_COL32(255, 0, 0, 255));
  } else if (c == "za") {
    f.Rect(0, 0, 1, 0.5f, IM_COL32(224, 60, 49, 255)), f.Rect(0, 0.5f, 1, 1, IM_COL32(0, 20, 137, 255));
    f.Rect(0.35f, 0.30f, 1, 0.70f, kWhite), f.Rect(0.35f, 0.38f, 1, 0.62f, IM_COL32(0, 119, 73, 255));
    f.Tri(0, 0, 0.52f, 0.5f, 0, 1, kWhite), f.Tri(0, 0.08f, 0.44f, 0.5f, 0, 0.92f, IM_COL32(0, 119, 73, 255));
    f.Tri(0, 0.24f, 0.28f, 0.5f, 0, 0.76f, IM_COL32(255, 184, 28, 255)), f.Tri(0, 0.32f, 0.20f, 0.5f, 0, 0.68f, kBlack);
  } else if (c == "in") {
    f.H3(IM_COL32(255, 153, 51, 255), kWhite, IM_COL32(19, 136, 8, 255));
    d->AddCircle(f.P(0.5f, 0.5f), f.H() * 0.13f, IM_COL32(0, 0, 128, 255), 16, std::max(1.0f, f.H() * 0.045f));
  } else if (c == "sg") {
    f.Rect(0, 0, 1, 0.5f, IM_COL32(238, 37, 54, 255)), f.Rect(0, 0.5f, 1, 1, kWhite);
    f.Disc(0.20f, 0.25f, 0.16f, kWhite), f.Disc(0.25f, 0.25f, 0.14f, IM_COL32(238, 37, 54, 255));
    for (int i = 0; i < 3; ++i) f.Disc(0.36f + 0.06f * float(i), 0.18f + (i == 1 ? -0.07f : 0.07f), 0.035f, kWhite);
  } else if (c == "jp") {
    f.Rect(0, 0, 1, 1, kWhite), f.Disc(0.5f, 0.5f, 0.30f, IM_COL32(188, 0, 45, 255));
  } else if (c == "kr") {
    f.Rect(0, 0, 1, 1, kWhite);
    d->PushClipRect(f.P(0, 0), f.P(1, 0.5f), true), f.Disc(0.5f, 0.5f, 0.25f, IM_COL32(205, 46, 58, 255)), d->PopClipRect();
    d->PushClipRect(f.P(0, 0.5f), f.P(1, 1), true), f.Disc(0.5f, 0.5f, 0.25f, IM_COL32(0, 71, 160, 255)), d->PopClipRect();
    f.Line(0.14f, 0.26f, 0.26f, 0.10f, 0.06f, kBlack), f.Line(0.74f, 0.10f, 0.86f, 0.26f, 0.06f, kBlack);
    f.Line(0.14f, 0.74f, 0.26f, 0.90f, 0.06f, kBlack), f.Line(0.74f, 0.90f, 0.86f, 0.74f, 0.06f, kBlack);
  } else if (c == "hk") {
    f.Rect(0, 0, 1, 1, IM_COL32(222, 41, 16, 255));
    for (int i = 0; i < 5; ++i) {
      const float ang = 6.2831853f * float(i) / 5 - 1.5708f;
      f.Disc(0.5f + 0.11f * std::cos(ang) * f.H() / f.W(), 0.5f + 0.17f * std::sin(ang), 0.10f, kWhite);
    }
  } else if (c == "tw") {
    f.Rect(0, 0, 1, 1, IM_COL32(254, 0, 0, 255)), f.Rect(0, 0, 0.5f, 0.5f, IM_COL32(0, 0, 149, 255)), f.Disc(0.25f, 0.25f, 0.12f, kWhite);
  } else if (c == "au" || c == "nz") {
    f.Rect(0, 0, 1, 1, IM_COL32(0, 0, 139, 255));
    f.Union(0, 0, 0.5f, 0.5f);
    const ImU32 star = c == "au" ? kWhite : IM_COL32(204, 20, 43, 255);
    if (c == "au") f.Disc(0.25f, 0.76f, 0.10f, kWhite);
    f.Disc(0.75f, 0.18f, 0.06f, star), f.Disc(0.62f, 0.44f, 0.06f, star), f.Disc(0.88f, 0.40f, 0.06f, star), f.Disc(0.75f, 0.80f, 0.07f, star);
  } else {
    f.Rect(0, 0, 1, 1, IM_COL32(120, 120, 112, 255));
  }
  d->PopClipRect();
  d->AddRect(a, b, IM_COL32(20, 20, 18, 160), 0, 0, 1.0f);
}

}  // namespace bf1943
