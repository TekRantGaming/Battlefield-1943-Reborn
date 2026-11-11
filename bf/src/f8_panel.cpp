// The graphics panel (F8), King Kong's (kk/src/debug_overlay.cpp) with Battlefield's settings only (docs/LAUNCHER-PLAN.md
// section 6.2; TekRantGaming, 8 Nov 2026): a translucent panel over the running game for trying settings while playing.
//   F8          the panel on and off (L3 and R3 together on a controller)
//   F7 (held)   the saved settings while held, the trial ones again when let go: an A/B comparison (LB on a pad)
//   F6          a capture: a PNG of the game's picture without the panel and a text file of the settings, in
//               <user>\captures (RB on a pad)
// Changes are a trial until Save as my settings; Reset to my settings puts the saved ones back. Rows that only apply
// at the next start are shown, locked. While it is open the controller and the mouse are the panel's (the game gets
// an empty pad, as with our menus); up and down pick a row, left and right or A change it, B closes.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window.h>

#include "launcher_kit.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "../third_party/miniz/miniz.h"

REXCVAR_DEFINE_DOUBLE(bf_f8_opacity, 0.88, "BF1943", "The graphics panel's (F8) background opacity, 0.3 to 1");
REXCVAR_DEFINE_BOOL(bf_f8_compact, false, "BF1943", "The graphics panel (F8) shows only the frame rate and its keys");

void SetFrameRate(double fps);  // frame_rate.cpp

namespace bf1943 {

size_t RecentFrameTimes(float* out, size_t max);       // frame_stats.cpp: oldest first, ms
bool FrameRateNow(double* fps, double* frame_time_ms);  // frame_stats.cpp
int MenuDriverStage();                                  // auto_join.cpp
bool LauncherShowing();                                 // game_menu.cpp: the launcher's own pages are up
std::string MenuSetting(const std::string& key, const std::string& def);  // game_menu.cpp: bf1943-settings.txt
void SetMenuSetting(const std::string& key, const std::string& value);    // game_menu.cpp: saved at once
std::filesystem::path MenuUserDir();                                       // game_menu.cpp

namespace {

namespace fs = std::filesystem;

struct Row {
  const char* section;
  const char* label;
  const char* key;  // a runtime setting, or "frame_rate" (the menu's own)
  std::vector<std::pair<const char*, const char*>> options;  // shown, value
  bool restart = false;                                      // applies at the next start: shown, locked
  float lo = 0, hi = 0, step = 0;                            // a number row when step > 0
  const char* format = "%g";
};

const std::vector<Row>& Rows() {
  static const std::vector<Row> rows = {
      {"ANTIALIASING", "Mode", "native_x_renderer_antialiasing",
       {{"Off", "off"}, {"FXAA", "fxaa"}}, true},
      {"RESOLUTION", "Sharpness", "native_x_renderer_sharpness",
       {{"Off", "0.000000"}, {"Low", "0.250000"}, {"Medium", "0.500000"}, {"High", "0.800000"}, {"Upscaler's", "-1.000000"}},
       true},
      // King Kong's F8 row "Spatial upscaler" (debug_overlay.cpp), with the mode: taken at the start (game_menu.cpp).
      {"RESOLUTION", "Upscaler", "native_x_renderer_scaling",
       {{"Off", "bilinear"}, {"AMD FSR 1", "fsr"}, {"NVIDIA NIS", "nis"}}, true},
      {"RESOLUTION", "Upscaler mode", "bf_upscaler_mode",
       {{"Native", "native"}, {"Quality", "quality"}, {"Balanced", "balanced"}, {"Performance", "performance"}}, true},
      {"LIGHTING", "Ambient occlusion", "native_ao_mode", {{"Off", "0"}, {"On", "1"}}},
      {"LIGHTING", "AO strength", "native_ao_strength", {{"Light", "1"}, {"Medium", "2"}, {"Strong", "3"}}},
      {"LIGHTING", "AO quality", "native_ao_samples", {{"Low", "1"}, {"Medium", "2"}, {"High", "3"}}},
      {"LIGHTING", "Global illumination", "native_gi_mode", {{"Off", "0"}, {"On", "1"}}},
      {"LIGHTING", "GI quality", "native_gi_quality", {{"Low", "1"}, {"Medium", "2"}, {"High", "3"}}},
      {"LIGHTING", "GI distance", "native_gi_distance", {{"Near", "1"}, {"Medium", "2"}, {"Far", "3"}, {"Full", "4"}}},
      {"SHADOWS AND EFFECTS", "Shadow quality", "native_x_renderer_shadow_scale",
       {{"Original", "0.000000"}, {"High", "2.000000"}, {"Ultra", "4.000000"}}, true},
      {"SHADOWS AND EFFECTS", "Motion blur", "bf_motion_blur", {{"Off", "false"}, {"On", "true"}}},
      {"SHADOWS AND EFFECTS", "Camera shake", "bf_camera_shake", {{"Off", "false"}, {"On", "true"}}},
      {"PICTURE", "Brightness", "bf_brightness", {}, false, -50, 50, 5, "%+.0f"},
      {"PICTURE", "Gamma", "bf_gamma", {}, false, 0.5f, 1.5f, 0.05f, "%.2f"},
      {"PICTURE", "Field of view", "bf_fov", {}, false, 60, 120, 2, "%.0f\xC2\xB0"},
      {"FRAME RATE", "Frame rate cap", "frame_rate",
       {{"30", "30"}, {"60", "60"}, {"90", "90"}, {"120", "120"}, {"150", "150"}, {"180", "180"}, {"240", "240"},
        {"Unlimited", "unlimited"}}},
      {"SHOW", "Performance metrics", "bf_performance_metrics", {{"Off", "off"}, {"FPS only", "fps"}, {"Full", "full"}}},
      {"EXPERIMENTAL", "Grass and shrubs", "native_grass", {{"Off", "false"}, {"On", "true"}}},
      {"EXPERIMENTAL", "Grass distance", "native_grass_radius",
       {{"Low", "45.000000"}, {"Medium", "90.000000"}, {"High", "180.000000"}, {"Extreme", "360.000000"},
        {"Full map", "1440.000000"}}},
  };
  return rows;
}

// The frame rate tried in the panel (the menu keeps its own setting, not a runtime one); empty: the saved one.
std::string g_frame_rate_trial;
// The value in use: the runtime's (a trial), or the menu's frame rate.
std::string Current(const Row& r) {
  if (std::string(r.key) == "frame_rate")
    return g_frame_rate_trial.empty() ? MenuSetting("frame_rate", "60") : g_frame_rate_trial;
  return rex::cvar::GetFlagByName(r.key);
}
std::string Saved(const Row& r) {
  if (std::string(r.key) == "frame_rate") return MenuSetting("frame_rate", "60");
  return MenuSetting(std::string("cvar.") + r.key, rex::cvar::GetFlagByName(r.key));
}
void Apply(const Row& r, const std::string& v) {
  if (std::string(r.key) == "frame_rate") {
    g_frame_rate_trial = v;
    SetFrameRate(v == "unlimited" ? 0.0 : std::atof(v.c_str()));
    return;
  }
  rex::cvar::SetFlagByName(r.key, v);
}

class Panel final : public rex::ui::ImGuiDialog {
 public:
  explicit Panel(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window) : ImGuiDialog(drawer), window_(window) {}

  std::atomic<bool> open{false};
  std::atomic<uint16_t> pad_presses{0}, pad_held{0};

 protected:
  void OnDraw(ImGuiIO& io) override {
    // F8 (and L3 with R3) toggles while the game's own picture is up (not on the launcher's pages).
    const uint16_t presses = pad_presses.exchange(0);
    const bool toggle = (ImGui::IsKeyPressed(ImGuiKey_F8, false) || (presses & 0x00C0) == 0x00C0) && !LauncherShowing();
    if (toggle) {
      open = !open;
      REXLOG_INFO("BF graphics panel: {}", open ? "open" : "closed");
      if (!open) EndCompare();
    }
    if (capture_frames_ > 0 && --capture_frames_ == 0) CaptureNow();
    if (!open || LauncherShowing() || capture_frames_ > 0) return;
    // F7 or LB held: the saved settings, for comparing.
    const bool compare = ImGui::IsKeyDown(ImGuiKey_F7) || (pad_held.load() & 0x0100);
    if (compare != comparing_) compare ? StartCompare() : EndCompare();
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false) || (presses & 0x0200)) Capture();

    const float u = io.DisplaySize.y / 720.0f;
    const kit::Fonts& f = kit::GetFonts();
    ImDrawList* d = ImGui::GetForegroundDrawList();
    const float alpha = float(std::clamp(REXCVAR_GET(bf_f8_opacity), 0.3, 1.0));
    double fps = 0, ms = 0;
    FrameRateNow(&fps, &ms);
    float times[240];
    const size_t n = RecentFrameTimes(times, 240);
    float worst = 0;
    for (size_t i = 0; i < n; ++i) worst = std::max(worst, times[i]);
    char head[96];
    std::snprintf(head, sizeof(head), "%.0f FPS \xC2\xB7 %.1f ms \xC2\xB7 worst %.1f", fps, ms, worst);

    if (REXCVAR_GET(bf_f8_compact)) {
      const std::string hint = comparing_ ? "SAVED SETTINGS (F7 / LB)" : "F8 PANEL \xC2\xB7 HOLD F7 TO COMPARE";
      const float w = std::max(kit::TextWidth(f.ui, 14 * u, head), kit::TextWidth(f.ui, 12 * u, hint)) + 24 * u;
      const ImVec2 a(io.DisplaySize.x - 16 * u - w, 16 * u), b(io.DisplaySize.x - 16 * u, 16 * u + 50 * u);
      d->AddRectFilled(a, b, IM_COL32(18, 22, 14, int(225 * alpha)));
      d->AddText(f.ui, 14 * u, ImVec2(a.x + 12 * u, a.y + 6 * u), IM_COL32(232, 226, 204, 255), head);
      d->AddText(f.ui, 12 * u, ImVec2(a.x + 12 * u, a.y + 28 * u), comparing_ ? kit::kGreenBright : IM_COL32(207, 198, 168, 255),
                 hint.c_str());
      if (ImGui::IsMouseHoveringRect(a, b) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        rex::cvar::SetFlagByName("bf_f8_compact", "false");
      return;
    }

    const float w = 380 * u, x0 = io.DisplaySize.x - 16 * u - w, x1 = io.DisplaySize.x - 16 * u;
    const float y0 = 16 * u, y1 = io.DisplaySize.y - 16 * u;
    d->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(18, 22, 14, int(225 * alpha)));
    d->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(201, 183, 122, 102), 0, 0, u);
    // Header: the title, the frame rate, the changes not saved; the frame time graph.
    d->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + 40 * u), IM_COL32(93, 138, 50, 90));
    d->AddText(f.ui_bold, 20 * u, ImVec2(x0 + 12 * u, y0 + 6 * u), kit::kCream, "GRAPHICS (F8)");
    d->AddText(f.ui, 13 * u, ImVec2(x1 - 12 * u - kit::TextWidth(f.ui, 13 * u, head), y0 + 12 * u), IM_COL32(232, 226, 204, 255),
               head);
    float y = y0 + 48 * u;
    {
      const ImVec2 g0(x0 + 12 * u, y), g1(x1 - 12 * u, y + 40 * u);
      d->AddRectFilled(g0, g1, IM_COL32(232, 226, 204, 15));
      const float scale = std::max(33.4f, worst * 1.1f);
      d->AddLine(ImVec2(g0.x, g1.y - (g1.y - g0.y) * 16.7f / scale), ImVec2(g1.x, g1.y - (g1.y - g0.y) * 16.7f / scale),
                 IM_COL32(232, 226, 204, 50));
      for (size_t i = 1; i < n; ++i) {
        const float xa = g0.x + (g1.x - g0.x) * float(i - 1) / float(std::max<size_t>(n - 1, 1));
        const float xb = g0.x + (g1.x - g0.x) * float(i) / float(std::max<size_t>(n - 1, 1));
        d->AddLine(ImVec2(xa, g1.y - (g1.y - g0.y) * std::min(1.0f, times[i - 1] / scale)),
                   ImVec2(xb, g1.y - (g1.y - g0.y) * std::min(1.0f, times[i] / scale)), kit::kGreenBright, 1.5f * u);
      }
      y = g1.y + 6 * u;
    }
    const int changed = int(trial_.size());
    if (comparing_ || changed) {
      const std::string note = comparing_ ? "Showing your saved settings while F7 or LB is held."
                                          : std::to_string(changed) + (changed == 1 ? " change" : " changes") +
                                                " not saved: Save keeps them, Reset puts yours back.";
      d->AddText(f.ui, 12 * u, ImVec2(x0 + 12 * u, y), comparing_ ? kit::kGreenBright : IM_COL32(232, 196, 106, 255), note.c_str());
      y += 18 * u;
    }
    // The rows.
    const auto& rows = Rows();
    const int count = int(rows.size());
    const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow) || (presses & 0x0001);
    const bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow) || (presses & 0x0002);
    const bool left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || (presses & 0x0004);
    const bool right = ImGui::IsKeyPressed(ImGuiKey_RightArrow) || (presses & 0x0008) || (presses & 0x1000);
    if (up) row_ = (row_ + count - 1) % count;
    if (down) row_ = (row_ + 1) % count;
    if (presses & 0x2000) open = false;  // B
    const float footer = 96 * u, rh = 24 * u, top = y, bottom = y1 - footer;
    const char* section = "";
    float ry = top - scroll_;
    std::vector<float> row_y(static_cast<size_t>(count), 0.0f);
    for (int i = 0; i < count; ++i) {
      if (std::string(rows[size_t(i)].section) != section) section = rows[size_t(i)].section, ry += 22 * u;
      row_y[size_t(i)] = ry;
      ry += rh;
    }
    // The selected row kept in view (the list's positions moved with the scroll at once, this frame too).
    float shift = 0;
    if (row_ != shown_row_) {
      shown_row_ = row_;
      if (row_y[size_t(row_)] < top + 22 * u) shift = row_y[size_t(row_)] - (top + 22 * u);
      if (row_y[size_t(row_)] + rh > bottom) shift = row_y[size_t(row_)] + rh - bottom;
    }
    if (ImGui::IsMouseHoveringRect(ImVec2(x0, top), ImVec2(x1, bottom)) && io.MouseWheel != 0)
      shift -= io.MouseWheel * rh * 2;
    if (shift != 0) {
      const float before = scroll_;
      scroll_ = std::max(0.0f, scroll_ + shift);
      for (float& y : row_y) y -= scroll_ - before;
    }
    d->PushClipRect(ImVec2(x0, top), ImVec2(x1, bottom), true);
    section = "";
    for (int i = 0; i < count; ++i) {
      const Row& r = rows[size_t(i)];
      const float yy = row_y[size_t(i)];
      if (std::string(r.section) != section) {
        section = r.section;
        d->AddLine(ImVec2(x0 + 12 * u, yy - 21 * u), ImVec2(x1 - 12 * u, yy - 21 * u), IM_COL32(201, 183, 122, 64));
        d->AddText(f.ui, 13 * u, ImVec2(x0 + 12 * u, yy - 20 * u), kit::kGold, r.section);
      }
      const ImVec2 a(x0 + 4 * u, yy), b(x1 - 4 * u, yy + rh);
      const bool hover = ImGui::IsMouseHoveringRect(a, b);
      if (hover && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) row_ = i;
      const bool sel = i == row_;
      if (sel) d->AddRectFilled(a, b, IM_COL32(93, 138, 50, 115));
      const ImU32 ink = r.restart ? IM_COL32(150, 146, 128, 255) : IM_COL32(232, 226, 204, 255);
      d->AddText(f.ui, 15 * u, ImVec2(a.x + 10 * u, yy + 2 * u), ink, r.label);
      std::string value = ValueText(r, Current(r));
      if (r.restart) value += "  (next start)";
      if (sel && !r.restart) value = "<  " + value + "  >";
      const bool trial = trial_.count(r.key) > 0;
      d->AddText(f.ui, 15 * u, ImVec2(b.x - 10 * u - kit::TextWidth(f.ui, 15 * u, value), yy + 2 * u),
                 trial ? IM_COL32(232, 196, 106, 255) : r.restart ? IM_COL32(150, 146, 128, 255) : kit::kGreenBright,
                 value.c_str());
      if (r.restart || comparing_) continue;
      const bool click = hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left),
                 back = hover && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
      if ((sel && (left || right)) || click || back) Step(r, (sel && left) || back ? -1 : 1);
    }
    d->PopClipRect();
    // Footer: Reset, Save, Copy, Capture, Compact; Opacity; the keys.
    const float fy = bottom + 8 * u;
    d->AddLine(ImVec2(x0 + 12 * u, bottom + 2 * u), ImVec2(x1 - 12 * u, bottom + 2 * u), IM_COL32(201, 183, 122, 64));
    static const char* kButtons[] = {"Reset", "Save", "Copy", "Capture", "Compact"};
    float bx = x0 + 12 * u;
    for (int i = 0; i < 5; ++i) {
      const float bw = kit::TextWidth(f.ui, 14 * u, kButtons[i]) + 16 * u;
      const ImVec2 a(bx, fy), b(bx + bw, fy + 26 * u);
      const bool hover = ImGui::IsMouseHoveringRect(a, b);
      const bool lit = (i == 0 || i == 1) && !trial_.empty();
      d->AddRectFilled(a, b, hover ? IM_COL32(93, 138, 50, 200) : lit ? IM_COL32(93, 138, 50, 120) : IM_COL32(48, 46, 38, 230));
      d->AddText(f.ui, 14 * u, ImVec2(a.x + 8 * u, a.y + 3 * u), kit::kCream, kButtons[i]);
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (i == 0) Reset();
        else if (i == 1) Save();
        else if (i == 2) CopySettings();
        else if (i == 3) Capture();
        else rex::cvar::SetFlagByName("bf_f8_compact", "true");
      }
      bx = b.x + 6 * u;
    }
    {
      char op[32];
      std::snprintf(op, sizeof(op), "Opacity %.0f%%", alpha * 100);
      const ImVec2 a(x0 + 12 * u, fy + 32 * u);
      d->AddText(f.ui, 13 * u, a, IM_COL32(207, 198, 168, 255), op);
      const float tx0 = a.x + 96 * u, tx1 = x1 - 12 * u, ty = a.y + 9 * u;
      d->AddRectFilled(ImVec2(tx0, ty - u), ImVec2(tx1, ty + u), IM_COL32(232, 226, 204, 60));
      const float fx = tx0 + (tx1 - tx0) * (alpha - 0.3f) / 0.7f;
      d->AddRectFilled(ImVec2(fx - 3 * u, ty - 7 * u), ImVec2(fx + 3 * u, ty + 7 * u), kit::kGold);
      if (ImGui::IsMouseHoveringRect(ImVec2(tx0, ty - 9 * u), ImVec2(tx1, ty + 9 * u)) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const double v = 0.3 + 0.7 * std::clamp((io.MousePos.x - tx0) / (tx1 - tx0), 0.0f, 1.0f);
        char b[16];
        std::snprintf(b, sizeof(b), "%.2f", v);
        rex::cvar::SetFlagByName("bf_f8_opacity", b);
      }
    }
    d->AddText(f.ui, 12 * u, ImVec2(x0 + 12 * u, fy + 56 * u), IM_COL32(207, 198, 168, 255),
               "A or click: change \xC2\xB7 F7 / LB held: compare \xC2\xB7 F6 / RB: capture \xC2\xB7 F8 / B: close");
    if (!status_.empty() && ImGui::GetTime() - status_at_ < 4)
      d->AddText(f.ui, 12 * u, ImVec2(x0 + 12 * u, fy + 72 * u), kit::kGreenBright, status_.c_str());
  }

 private:
  static std::string ValueText(const Row& r, const std::string& v) {
    if (r.step > 0) {
      char b[32];
      double x = std::atof(v.c_str());
      if (std::string(r.key) == "bf_fov" && x <= 0) x = 86;  // 0: the game's own
      std::snprintf(b, sizeof(b), r.format, x);
      return b;
    }
    for (const auto& [shown, value] : r.options)
      if (v == value || (std::atof(v.c_str()) == std::atof(value) && v.find_first_not_of("0123456789.") == std::string::npos &&
                         std::string(value).find_first_not_of("0123456789.") == std::string::npos))
        return shown;
    if (!v.empty() && std::atof(v.c_str()) < 0) return "Default";  // a negative number: the setting's own default
    return v;
  }
  // One step of a row: the next option, or the number by its step. A trial until saved.
  void Step(const Row& r, int dir) {
    const std::string cur = Current(r);
    std::string next;
    if (r.step > 0) {
      double x = std::atof(cur.c_str());
      if (std::string(r.key) == "bf_fov" && x <= 0) x = 86;
      x = std::clamp(x + dir * r.step, double(r.lo), double(r.hi));
      char b[32];
      std::snprintf(b, sizeof(b), "%g", std::round(x / r.step) * r.step);
      next = b;
    } else {
      int i = 0;
      for (size_t k = 0; k < r.options.size(); ++k)
        if (ValueText(r, cur) == r.options[k].first) i = int(k);
      const int n = int(r.options.size());
      next = r.options[size_t((i + dir + n) % n)].second;
    }
    if (!trial_.count(r.key)) trial_[r.key] = Saved(r);
    Apply(r, next);
    if (next == trial_[r.key]) trial_.erase(r.key);
    REXLOG_INFO("BF graphics panel: {} {} (trial)", r.key, next);
  }
  void Save() {
    for (const auto& [key, saved] : trial_) {
      const Row* r = Find(key);
      if (!r) continue;
      const std::string v = Current(*r);
      if (key == "frame_rate") SetMenuSetting("frame_rate", v);
      else SetMenuSetting("cvar." + key, v);
    }
    g_frame_rate_trial.clear();
    REXLOG_INFO("BF graphics panel: {} changes saved as the player's settings", trial_.size());
    Status(std::to_string(trial_.size()) + " changes saved as your settings.");
    trial_.clear();
  }
  void Reset() {
    for (const auto& [key, saved] : trial_)
      if (const Row* r = Find(key)) Apply(*r, saved);
    g_frame_rate_trial.clear();
    REXLOG_INFO("BF graphics panel: {} changes put back", trial_.size());
    Status("Your saved settings are back.");
    trial_.clear();
  }
  // F7 held: the trial values put aside, the saved ones in use; let go: the trial back.
  void StartCompare() {
    comparing_ = true;
    for (const auto& [key, saved] : trial_)
      if (const Row* r = Find(key)) trial_values_[key] = Current(*r), Apply(*r, saved);
    REXLOG_INFO("BF graphics panel: comparing with the saved settings");
  }
  void EndCompare() {
    if (!comparing_) return;
    comparing_ = false;
    for (const auto& [key, value] : trial_values_)
      if (const Row* r = Find(key)) Apply(*r, value);
    trial_values_.clear();
    REXLOG_INFO("BF graphics panel: the trial settings back");
  }
  static const Row* Find(const std::string& key) {
    for (const Row& r : Rows())
      if (key == r.key) return &r;
    return nullptr;
  }
  std::string SettingsText() {
    std::string out = "Battlefield 1943 Reborn graphics settings (F8 panel)\n";
    for (const Row& r : Rows()) out += std::string(r.label) + ": " + ValueText(r, Current(r)) + "\n";
    return out;
  }
  void CopySettings() {
#if defined(_WIN32)
    const std::string text = SettingsText();
    if (OpenClipboard(nullptr)) {
      EmptyClipboard();
      if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1)) {
        std::memcpy(GlobalLock(h), text.c_str(), text.size() + 1);
        GlobalUnlock(h);
        SetClipboardData(CF_TEXT, h);
      }
      CloseClipboard();
    }
#endif
    Status("The settings are on the clipboard.");
  }
  // A capture waits two frames with the panel hidden, so the picture is the game's alone.
  void Capture() { capture_frames_ = 3; }
  void CaptureNow() {
    const fs::path dir = MenuUserDir() / "captures";
    std::error_code ec;
    fs::create_directories(dir, ec);
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    const fs::path png = dir / (std::string("graphics-") + stamp + ".png"), txt = dir / (std::string("graphics-") + stamp + ".txt");
    std::ofstream(txt) << SettingsText();
    bool saved = false;
#if defined(_WIN32)
    if (HWND hwnd = window_ ? static_cast<HWND>(window_->GetNativeWindowHandle()) : nullptr) {
      RECT rc{};
      GetClientRect(hwnd, &rc);
      const int w = rc.right - rc.left, h = rc.bottom - rc.top;
      HDC wdc = GetDC(hwnd), mdc = CreateCompatibleDC(wdc);
      BITMAPINFO bi{};
      bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
      bi.bmiHeader.biWidth = w, bi.bmiHeader.biHeight = -h, bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32;
      void* bits = nullptr;
      HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
      if (bmp && w > 0 && h > 0) {
        HGDIOBJ old = SelectObject(mdc, bmp);
        PrintWindow(hwnd, mdc, 3);  // PW_CLIENTONLY | PW_RENDERFULLCONTENT, as our test captures
        SelectObject(mdc, old);
        std::vector<uint8_t> rgb(size_t(w) * h * 3);
        const auto* p = static_cast<const uint8_t*>(bits);
        for (size_t i = 0; i < size_t(w) * h; ++i) rgb[i * 3] = p[i * 4 + 2], rgb[i * 3 + 1] = p[i * 4 + 1], rgb[i * 3 + 2] = p[i * 4];
        size_t len = 0;
        if (void* data = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), w, h, 3, &len, 6, MZ_FALSE)) {
          std::ofstream(png, std::ios::binary).write(static_cast<const char*>(data), std::streamsize(len));
          mz_free(data);
          saved = true;
        }
      }
      if (bmp) DeleteObject(bmp);
      DeleteDC(mdc);
      ReleaseDC(hwnd, wdc);
    }
#endif
    REXLOG_INFO("BF graphics panel: capture {} ({})", png.string(), saved ? "picture and settings" : "settings only");
    Status(saved ? "Captured to " + png.filename().string() : "Settings saved to " + txt.filename().string());
  }
  void Status(std::string s) { status_ = std::move(s), status_at_ = ImGui::GetTime(); }

  rex::ui::Window* window_;
  int row_ = 3, shown_row_ = 3;
  float scroll_ = 0;
  bool comparing_ = false;
  int capture_frames_ = 0;
  std::map<std::string, std::string> trial_;         // key -> the saved value it replaced
  std::map<std::string, std::string> trial_values_;  // while comparing: the trial values put aside
  std::string status_;
  double status_at_ = 0;
};

Panel* g_panel = nullptr;

}  // namespace

void CreateGraphicsPanel(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window) {
  if (!g_panel) g_panel = new Panel(drawer, window);  // lives for the session
  // F7 is the panel's compare key: the SDK's achievements list (unused by Battlefield) loses it.
  rex::cvar::SetFlagByName("bind_achievements", "");
}
bool GraphicsPanelOpen() { return g_panel && g_panel->open.load(); }
// The controller's presses and held buttons (game_menu.cpp's pad reads).
void GraphicsPanelPad(uint16_t buttons) {
  if (!g_panel) return;
  static uint16_t last = 0;
  const uint16_t pressed = buttons & ~last;
  last = buttons;
  g_panel->pad_held = buttons;
  if (pressed) g_panel->pad_presses.fetch_or(pressed);
}

}  // namespace bf1943
