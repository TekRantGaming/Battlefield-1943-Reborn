// The voice and text chat overlay (comms.cpp is the engine; docs/COMMS-PLAN.md). In a match, with our menus closed:
// the chat top left (below the performance metrics when they show), each line for 12 s; the chat box opened by
// keybind_chat_all (T), keybind_chat_team (Y) or keybind_chat_squad (U): Tab changes the channel, Enter sends, Esc
// closes, and while it is open the game gets no input (auto_join.cpp's pad, look.cpp's mouse). Push to talk
// (keybind_push_to_talk, B) is read here. Who is speaking shows under the chat, and MIC while your voice goes out.

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>  // ImGuiContext: PlatformImeData, as the launcher's name page
#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>

#include "comms.h"

REXCVAR_DEFINE_STRING(keybind_chat_all, "T", "Input/Keybinds/Chat", "Text chat to everyone on the server");
REXCVAR_DEFINE_STRING(keybind_chat_team, "Y", "Input/Keybinds/Chat", "Text chat to your team");
REXCVAR_DEFINE_STRING(keybind_chat_squad, "U", "Input/Keybinds/Chat", "Text chat to your squad");
REXCVAR_DECLARE(std::string, keybind_push_to_talk);
REXCVAR_DECLARE(std::string, bf_voice_mode);
REXCVAR_DECLARE(bool, bf_text_chat);

namespace bf1943 {

bool GameMenuOpen();          // game_menu.cpp
bool InMatch();               // auto_join.cpp
ImFont* MenuFont(int which);  // game_menu.cpp: 0 bold, 1 semibold
float PerfOverlayBottom();    // perf_overlay.cpp

namespace {

using comms::Channel;

// A key in the SDK's names (keybind_* values, as Settings > Controls writes them) as ImGui's.
ImGuiKey KeyFromName(const std::string& n) {
  if (n.size() == 1 && n[0] >= 'A' && n[0] <= 'Z') return ImGuiKey(ImGuiKey_A + (n[0] - 'A'));
  if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') return ImGuiKey(ImGuiKey_0 + (n[0] - '0'));
  if (n.size() == 7 && n.rfind("Numpad", 0) == 0 && n[6] >= '0' && n[6] <= '9')
    return ImGuiKey(ImGuiKey_Keypad0 + (n[6] - '0'));
  if (n.size() >= 2 && n[0] == 'F' && std::isdigit(static_cast<unsigned char>(n[1]))) {
    const int f = std::atoi(n.c_str() + 1);
    if (f >= 1 && f <= 12) return ImGuiKey(ImGuiKey_F1 + (f - 1));
  }
  static const std::pair<const char*, ImGuiKey> kNamed[] = {
      {"Space", ImGuiKey_Space},          {"Return", ImGuiKey_Enter},       {"Backspace", ImGuiKey_Backspace},
      {"Tab", ImGuiKey_Tab},              {"Up", ImGuiKey_UpArrow},         {"Down", ImGuiKey_DownArrow},
      {"Left", ImGuiKey_LeftArrow},       {"Right", ImGuiKey_RightArrow},   {"Semicolon", ImGuiKey_Semicolon},
      {"Quote", ImGuiKey_Apostrophe},     {"Shift", ImGuiKey_LeftShift},    {"Control", ImGuiKey_LeftCtrl},
      {"Alt", ImGuiKey_LeftAlt},          {"CapsLock", ImGuiKey_CapsLock},  {"Backtick", ImGuiKey_GraveAccent},
      {"Minus", ImGuiKey_Minus},          {"Plus", ImGuiKey_Equal},         {"Comma", ImGuiKey_Comma},
      {"Period", ImGuiKey_Period},        {"Slash", ImGuiKey_Slash},        {"Backslash", ImGuiKey_Backslash},
      {"LBracket", ImGuiKey_LeftBracket}, {"RBracket", ImGuiKey_RightBracket}, {"Delete", ImGuiKey_Delete},
      {"Insert", ImGuiKey_Insert},        {"Home", ImGuiKey_Home},          {"End", ImGuiKey_End},
      {"PageUp", ImGuiKey_PageUp},        {"PageDown", ImGuiKey_PageDown},  {"NumpadEnter", ImGuiKey_KeypadEnter},
      {"NumpadPlus", ImGuiKey_KeypadAdd}, {"NumpadMinus", ImGuiKey_KeypadSubtract},
      {"NumpadStar", ImGuiKey_KeypadMultiply}, {"NumpadSlash", ImGuiKey_KeypadDivide}};
  for (const auto& [name, key] : kNamed)
    if (n == name) return key;
  return ImGuiKey_None;
}

// A bind ("B", or keys separated by commas): held, or pressed this frame. Mouse buttons by their SDK names.
template <typename F>
bool AnyKey(const std::string& bind, F&& test) {
  for (size_t at = 0; at <= bind.size();) {
    size_t comma = bind.find(',', at);
    if (comma == std::string::npos) comma = bind.size();
    std::string k = bind.substr(at, comma - at);
    at = comma + 1;
    while (!k.empty() && k.front() == ' ') k.erase(k.begin());
    while (!k.empty() && k.back() == ' ') k.pop_back();
    if (!k.empty() && test(k)) return true;
  }
  return false;
}
bool BindDown(const std::string& bind) {
  return AnyKey(bind, [](const std::string& k) {
    if (k == "LMB") return ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (k == "RMB") return ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (k == "MMB") return ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    const ImGuiKey key = KeyFromName(k);
    return key != ImGuiKey_None && ImGui::IsKeyDown(key);
  });
}
bool BindPressed(const std::string& bind) {
  return AnyKey(bind, [](const std::string& k) {
    const ImGuiKey key = KeyFromName(k);
    return key != ImGuiKey_None && ImGui::IsKeyPressed(key, false);
  });
}

void AppendUtf8(std::string& s, unsigned c) {
  if (c < 0x80) s += char(c);
  else if (c < 0x800) s += char(0xC0 | (c >> 6)), s += char(0x80 | (c & 0x3F));
  else s += char(0xE0 | (c >> 12)), s += char(0x80 | ((c >> 6) & 0x3F)), s += char(0x80 | (c & 0x3F));
}
void PopUtf8(std::string& s) {
  while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
  if (!s.empty()) s.pop_back();
}
std::string Printable(const std::string& s) {
  std::string out;
  for (char c : s) out += (static_cast<unsigned char>(c) < 0x20) ? ' ' : c;
  return out;
}

const char* ChannelLabel(Channel c) { return c == comms::kTeam ? "TEAM" : c == comms::kSquad ? "SQUAD" : "ALL"; }
ImU32 ChannelColor(Channel c, float alpha) {
  const int a = int(std::clamp(alpha, 0.0f, 1.0f) * 255);
  if (c == comms::kTeam) return IM_COL32(120, 180, 255, a);
  if (c == comms::kSquad) return IM_COL32(130, 225, 120, a);
  return IM_COL32(235, 235, 228, a);
}

class CommsOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit CommsOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!InMatch() || GameMenuOpen()) {
      if (comms::Typing()) Close();
      comms::SetPushToTalk(false);
      return;
    }
    const bool typing = comms::Typing();
    comms::SetPushToTalk(!typing && REXCVAR_GET(bf_voice_mode) == "push" && BindDown(REXCVAR_GET(keybind_push_to_talk)));
    if (!typing && REXCVAR_GET(bf_text_chat)) {
      if (BindPressed(REXCVAR_GET(keybind_chat_all))) Open(comms::kAll);
      else if (BindPressed(REXCVAR_GET(keybind_chat_team))) Open(comms::kTeam);
      else if (BindPressed(REXCVAR_GET(keybind_chat_squad))) Open(comms::kSquad);
    } else if (typing) {
      Edit(io);
    }
    Draw(io);
  }

 private:
  Channel channel_ = comms::kAll;
  std::string text_;
  int opened_frame_ = -10;

  void Open(Channel c) {
    channel_ = c, text_.clear(), opened_frame_ = ImGui::GetFrameCount();
    comms::SetTyping(true);
  }
  void Close() {
    text_.clear();
    comms::SetTyping(false);
  }

  void Edit(ImGuiIO& io) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    g.PlatformImeData.WantTextInput = true;  // as the launcher's name page: the window takes typed text
    g.PlatformImeData.ViewportId = ImGui::GetMainViewport()->ID;
    if (ImGui::GetFrameCount() > opened_frame_ + 1) {  // not the key that opened it
      for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const unsigned c = io.InputQueueCharacters[i];
        if (c >= 0x20 && c != 0x7F && text_.size() < 120) AppendUtf8(text_, c);
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) PopUtf8(text_);
    if (ImGui::IsKeyPressed(ImGuiKey_V) && io.KeyCtrl) {
      if (const char* clip = ImGui::GetClipboardText())
        for (const char* p = clip; *p && text_.size() < 120; ++p)
          if (static_cast<unsigned char>(*p) >= 0x20) text_ += *p;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) channel_ = Channel((channel_ + 1) % 3);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) return Close();
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
      std::string t = text_;
      while (!t.empty() && t.back() == ' ') t.pop_back();
      while (!t.empty() && t.front() == ' ') t.erase(t.begin());
      if (!t.empty()) comms::SendChat(channel_, t);
      Close();
    }
  }

  void Draw(ImGuiIO& io) {
    const float s = io.DisplaySize.y / 1080.0f;
    ImFont* bold = MenuFont(0) ? MenuFont(0) : ImGui::GetFont();
    ImFont* font = MenuFont(1) ? MenuFont(1) : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float size = 20.0f * s, line = 26.0f * s, x0 = 18.0f * s;
    float y = std::max(16.0f * s, PerfOverlayBottom() + 10.0f * s);
    const float wrap = io.DisplaySize.x * 0.38f;
    const double now = comms::Now();
    const bool typing = comms::Typing();
    auto shadowed = [&](ImFont* f, ImVec2 at, ImU32 color, const std::string& t, float alpha) {
      const ImU32 shadow = IM_COL32(0, 0, 0, int(180 * alpha));
      dl->AddText(f, size, ImVec2(at.x + 1.5f * s, at.y + 1.5f * s), shadow, t.c_str(), nullptr, wrap);
      dl->AddText(f, size, at, color, t.c_str(), nullptr, wrap);
    };
    if (REXCVAR_GET(bf_text_chat)) {
      // Laid out first, so a faint dark panel can go behind the lines (the text stays readable over a bright sky).
      struct Laid {
        const comms::ChatLine* l;
        float alpha, height, body_x;
        std::string tag, who, body;
      };
      const std::vector<comms::ChatLine> recent = comms::RecentChat(typing ? 10 : 6);
      std::vector<Laid> laid;
      float total = 0, strongest = typing ? 1.0f : 0.0f;
      for (const comms::ChatLine& l : recent) {
        const double age = now - l.time;
        if (!typing && age > 12.0) continue;
        Laid d{&l, typing ? 1.0f : float(std::clamp(12.0 - age, 0.0, 2.0) / 2.0), line, x0, {}, {}, Printable(l.text)};
        if (!l.notice) {
          d.tag = std::string("[") + ChannelLabel(l.channel) + "] ";
          d.who = Printable(l.name) + ": ";
          d.body_x = x0 + bold->CalcTextSizeA(size, FLT_MAX, 0, d.tag.c_str()).x +
                     bold->CalcTextSizeA(size, FLT_MAX, 0, d.who.c_str()).x;
        }
        const float body_wrap = wrap - (d.body_x - x0);
        d.height = std::max(line, font->CalcTextSizeA(size, FLT_MAX, body_wrap, d.body.c_str()).y + 4 * s);
        total += d.height;
        strongest = std::max(strongest, d.alpha);
        laid.push_back(std::move(d));
      }
      const float typing_h = typing ? line + 26 * s : 0.0f;
      if (!laid.empty() || typing)
        dl->AddRectFilled(ImVec2(x0 - 8 * s, y - 5 * s), ImVec2(x0 + wrap + 12 * s, y + total + typing_h + 1 * s),
                          IM_COL32(0, 0, 0, int(80 * strongest)), 5 * s);
      for (const Laid& d : laid) {
        const float alpha = d.alpha;
        if (d.l->notice) {
          shadowed(font, ImVec2(x0, y), IM_COL32(240, 210, 120, int(255 * alpha)), d.body, alpha);
          y += d.height;
          continue;
        }
        float x = x0;
        shadowed(bold, ImVec2(x, y), ChannelColor(d.l->channel, alpha), d.tag, alpha);
        x += bold->CalcTextSizeA(size, FLT_MAX, 0, d.tag.c_str()).x;
        shadowed(bold, ImVec2(x, y),
                 d.l->own ? IM_COL32(255, 225, 140, int(255 * alpha)) : ChannelColor(d.l->channel, alpha), d.who, alpha);
        const float body_wrap = wrap - (d.body_x - x0);
        dl->AddText(font, size, ImVec2(d.body_x + 1.5f * s, y + 1.5f * s), IM_COL32(0, 0, 0, int(180 * alpha)),
                    d.body.c_str(), nullptr, body_wrap);
        dl->AddText(font, size, ImVec2(d.body_x, y), IM_COL32(255, 255, 255, int(255 * alpha)), d.body.c_str(),
                    nullptr, body_wrap);
        y += d.height;
      }
      if (typing) {
        const std::string tag = std::string("[") + ChannelLabel(channel_) + "] ";
        const float tw = bold->CalcTextSizeA(size, FLT_MAX, 0, tag.c_str()).x;
        dl->AddRectFilled(ImVec2(x0 - 4 * s, y - 2 * s), ImVec2(x0 + wrap + 8 * s, y + line), IM_COL32(10, 12, 10, 210),
                          4 * s);
        dl->AddText(bold, size, ImVec2(x0, y), ChannelColor(channel_, 1), tag.c_str());
        // A long message shows its end (where the typing is), cut at the front.
        std::string shown = text_ + (std::fmod(ImGui::GetTime(), 1.0) < 0.55 ? "_" : " ");
        const float room = wrap - tw - 4 * s;
        bool cut = false;
        while (!shown.empty() && font->CalcTextSizeA(size, FLT_MAX, 0, ((cut ? "..." : "") + shown).c_str()).x > room) {
          shown.erase(shown.begin());
          while (!shown.empty() && (static_cast<unsigned char>(shown.front()) & 0xC0) == 0x80) shown.erase(shown.begin());
          cut = true;
        }
        dl->AddText(font, size, ImVec2(x0 + tw, y), IM_COL32(255, 255, 255, 255), ((cut ? "..." : "") + shown).c_str());
        y += line + 4 * s;
        dl->AddText(font, 15.0f * s, ImVec2(x0, y), IM_COL32(215, 215, 205, 230),
                    "Enter send   Tab channel   Esc close");
        y += 22 * s;
      }
    }
    // Voice: who is speaking, and MIC while yours goes out.
    const std::vector<std::string> speaking = comms::Speaking();
    if (!speaking.empty() || comms::Transmitting()) {
      y += 4 * s;
      float x = x0;
      if (comms::Transmitting()) {
        dl->AddRectFilled(ImVec2(x, y + 2 * s), ImVec2(x + 44 * s, y + line - 2 * s), IM_COL32(60, 170, 70, 230), 3 * s);
        dl->AddText(bold, 16.0f * s, ImVec2(x + 7 * s, y + 4 * s), IM_COL32(255, 255, 255, 255), "MIC");
        x += 54 * s;
      }
      for (const std::string& name : speaking) {
        dl->AddCircleFilled(ImVec2(x + 6 * s, y + line * 0.5f), 5 * s, IM_COL32(130, 225, 120, 255));
        shadowed(bold, ImVec2(x + 16 * s, y), IM_COL32(235, 235, 228, 255), Printable(name), 1.0f);
        x += 26 * s + bold->CalcTextSizeA(size, FLT_MAX, 0, name.c_str()).x;
        if (x > io.DisplaySize.x * 0.45f) break;
      }
    }
  }
};

}  // namespace

void CreateCommsOverlay(rex::ui::ImGuiDrawer* drawer) {
  new CommsOverlay(drawer);  // lives for the rest of the session
}

}  // namespace bf1943
