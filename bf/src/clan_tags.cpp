// Clan tags (clan_tags.h): the player's own tag to the backend, everyone's tags from it, "[TAG]Name" in the game's
// player list, and the drawing of a tagged name on our pages (the Dev tag's colour cycle).

#include "clan_tags.h"

#include <cstring>

#include "player_name.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include "server_list.h"
#include "text_filter.h"
#include "player_limits.h"

REXCVAR_DECLARE(bool, server);
REXCVAR_DEFINE_BOOL(bf_clan_tags_in_game, true, "BF1943/Online",
                    "Clan tags in front of the players' names in a match (the kill feed, name tags and scoreboard)");

// For tests (and a player who wants one start under another tag), as bf_player_name is for the name: in place of the
// saved tag; "-" is no tag.
REXCVAR_DEFINE_STRING(bf_clan_tag, "", "BF1943/Online", "This start's clan tag in place of the saved one (- for none)");

REX_EXTERN(sub_821D51C8);  // the game's string assign (string, C text): auto_join.cpp's

namespace bf1943::clan {
namespace {

using Clock = std::chrono::steady_clock;

std::mutex g_mutex;
std::string g_saved;      // the settings file's tag
std::string g_own;        // the backend's answer for this player
bool g_confirmed = false;
std::string g_refusal;    // the backend's words for a refused tag, until the page takes them
struct Known {
  std::string name, tag;
  Clock::time_point at;
};
std::map<std::string, Known> g_tags;  // lower case name -> its tag as the backend last said ("" none)
Clock::time_point g_match_asked{};
bool g_match_busy = false;
std::vector<std::string> g_match_names;  // the plain names in this game's player list now (GameNamesTick)

std::string Lower(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// A tag as the backend sent it: kept only when it is one a game could show (2 to 4 letters or digits), whatever the
// backend says (a changed backend address cannot draw anything else into our pages or the game's).
std::string Clean(const std::string& tag) {
  if (int(tag.size()) < text_filter::kTagMin || int(tag.size()) > text_filter::kTagMax) return {};
  for (char c : tag)
    if (!std::isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80) return {};
  return tag;
}

std::string JsonText(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (static_cast<unsigned char>(c) >= 0x20) out += c;
  }
  return out;
}

// The "tags" object of a reply ({"Name": "TAG", ...}: names and tags are letters, digits, spaces and underscores).
std::vector<std::pair<std::string, std::string>> ParseTags(const std::string& reply) {
  std::vector<std::pair<std::string, std::string>> out;
  const size_t at = reply.find("\"tags\":");
  if (at == std::string::npos) return out;
  const size_t open = reply.find('{', at), close = reply.find('}', at);
  if (open == std::string::npos || close == std::string::npos || close < open) return out;
  size_t i = open + 1;
  while (i < close) {
    const size_t k0 = reply.find('"', i);
    if (k0 == std::string::npos || k0 >= close) break;
    const std::string key = JsonString(reply, k0);
    const size_t colon = reply.find(':', k0 + 1 + key.size());
    const size_t v0 = colon == std::string::npos ? std::string::npos : reply.find('"', colon);
    if (v0 == std::string::npos || v0 >= close) break;
    const std::string value = JsonString(reply, v0);
    out.emplace_back(key, value);
    const size_t v1 = reply.find('"', v0 + 1);
    if (v1 == std::string::npos) break;
    i = v1 + 1;
  }
  return out;
}

}  // namespace

bool IsDevTag(const std::string& tag) { return tag == "Dev"; }

void SetSavedTag(const std::string& saved) {
  const std::string forced = REXCVAR_GET(bf_clan_tag);
  const std::string tag = forced.empty() ? saved : forced == "-" ? std::string() : forced;
  std::lock_guard lock(g_mutex);
  if (tag != g_saved) g_confirmed = false;
  g_saved = tag;
}

std::string OwnTag() {
  std::lock_guard lock(g_mutex);
  return g_confirmed ? g_own : g_saved;
}
bool OwnTagConfirmed() {
  std::lock_guard lock(g_mutex);
  return g_confirmed;
}
std::string TakeRefusal() {
  std::lock_guard lock(g_mutex);
  std::string out;
  out.swap(g_refusal);
  return out;
}

void SendOwnTag(const std::string& name, uint32_t online_id) {
  if (REXCVAR_GET(server) || REXCVAR_GET(bf_online_host).empty() || name.empty() || !online_id) return;
  std::string tag;
  {
    std::lock_guard lock(g_mutex);
    tag = g_saved;
  }
  char id[9];
  std::snprintf(id, sizeof(id), "%08X", online_id);
  const std::string request = "{\"op\": \"tag\", \"name\": \"" + JsonText(name) + "\", \"id\": \"" + id +
                              "\", \"tag\": \"" + JsonText(tag) + "\"}";
  std::thread([request, name, tag] {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    std::string reply;
    if (!RegistryRequest(request, reply) || JsonField(reply, "op") != "tag") {
      REXLOG_INFO("BF clan tag: the backend took no clan tag (an older backend, or none): \"{}\" kept here only", tag);
      return;
    }
    const bool ok = JsonField(reply, "ok") == "true";
    const std::string worn = Clean(JsonField(reply, "tag"));
    std::lock_guard lock(g_mutex);
    if (tag != g_saved) return;  // saved again meanwhile: that one's answer counts
    g_own = worn, g_confirmed = true;
    g_tags[Lower(name)] = {name, worn, Clock::now()};
    if (ok) {
      REXLOG_INFO("BF clan tag: {} wears \"{}\"{}", name, worn, JsonField(reply, "dev") == "true" ? " (a developer)" : "");
    } else {
      g_refusal = JsonField(reply, "text");
      REXLOG_INFO("BF clan tag: \"{}\" refused for {} ({}); wearing \"{}\"", tag, name, JsonField(reply, "why"), worn);
    }
  }).detach();
}

void Note(const std::string& name, const std::string& tag) {
  if (name.empty()) return;
  std::lock_guard lock(g_mutex);
  if (g_tags.size() > 4096) g_tags.clear();
  g_tags[Lower(name)] = {name, Clean(tag), Clock::now()};
}

std::string TagOf(const std::string& name) {
  std::lock_guard lock(g_mutex);
  const auto it = g_tags.find(Lower(name));
  return it == g_tags.end() ? std::string() : it->second.tag;
}

std::string Tagged(const std::string& name) {
  const std::string tag = TagOf(name);
  return tag.empty() ? name : "[" + tag + "]" + name;
}

std::string Strip(const std::string& shown) {
  if (shown.size() < 4 || shown[0] != '[') return shown;
  const size_t close = shown.find(']');
  if (close == std::string::npos || close < 3 || close > size_t(text_filter::kTagMax) + 1) return shown;
  return shown.substr(close + 1);
}

void MatchTick(const std::string& gid, const std::string& own_name, uint32_t online_id) {
  if (REXCVAR_GET(server) || REXCVAR_GET(bf_online_host).empty() || gid.empty()) return;
  std::string tag;
  {
    std::lock_guard lock(g_mutex);
    const auto now = Clock::now();
    if (g_match_busy || now - g_match_asked < std::chrono::seconds(10)) return;
    g_match_asked = now, g_match_busy = true;
    tag = g_saved;
  }
  char id[9];
  std::snprintf(id, sizeof(id), "%08X", online_id);
  std::vector<std::string> asked;
  {
    std::lock_guard lock(g_mutex);
    asked = g_match_names;
  }
  std::string request = "{\"op\": \"tags\", \"gid\": \"" + JsonText(gid) + "\", \"names\": [";
  for (size_t i = 0; i < asked.size() && i < 64; ++i) request += (i ? ", \"" : "\"") + JsonText(asked[i]) + "\"";
  request += "]";
  if (!own_name.empty() && online_id)
    request += ", \"me\": {\"name\": \"" + JsonText(own_name) + "\", \"id\": \"" + id + "\", \"tag\": \"" +
               JsonText(tag) + "\"}";
  request += "}";
  std::thread([request, asked] {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    std::string reply;
    const bool ok = RegistryRequest(request, reply) && JsonField(reply, "op") == "tags";
    std::lock_guard lock(g_mutex);
    g_match_busy = false;
    if (!ok) return;
    // A player with no tag is not in the reply: each one asked about and not said wears none (a tag taken off in the
    // match goes at the next ask).
    const auto now = Clock::now();
    std::map<std::string, std::string> said;
    for (const auto& [name, tag] : ParseTags(reply)) said[Lower(name)] = Clean(tag);
    for (const std::string& name : asked)
      if (!said.count(Lower(name))) said[Lower(name)] = "";
    for (const auto& [key, tag] : said) {
      Known& k = g_tags[key];
      if (k.name.empty()) k.name = key;
      k.tag = tag, k.at = now;
    }
  }).detach();
}

// ---- the game's player list (map_players.cpp: the player manager at 0x82AF3AE4, every player of the match from
// the pointer at +0x98 to the one at +0x9C, a player's name at +8 as begin, end and capacity words)
namespace {
uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
bool Plausible(uint32_t a) { return a >= 0x00100000u && a < 0xFFF00000u && !(a & 3); }
bool IsPlayer(const uint8_t* base, uint32_t p) {
  return Plausible(p) && Load32(base, p) == 0x8200C960u && Load32(base, p + 4) == 0x8200C94Cu;
}
constexpr uint32_t kManager = 0x82AF3AE4;
constexpr uint32_t kName = 8;

// A player's name as the game holds it, or false when the string does not look like one (32 bytes at most, printable).
bool ReadName(const uint8_t* base, uint32_t player, std::string& out) {
  const uint32_t b = Load32(base, player + kName), e = Load32(base, player + kName + 4),
                 cap = Load32(base, player + kName + 8);
  if (b < 0x00100000u || b >= 0xFFF00000u || e < b || e - b > 32 || cap < e || cap - b > 4096) return false;
  out.assign(reinterpret_cast<const char*>(base + b), e - b);
  for (size_t i = 0; i < out.size(); ++i) {
    if (out.compare(i, player_name::kTm.size(), player_name::kTm) == 0) {  // a soldier name's trademark sign
      i += player_name::kTm.size() - 1;
      continue;
    }
    if (static_cast<unsigned char>(out[i]) < 0x20 || static_cast<unsigned char>(out[i]) >= 0x7F) return false;
  }
  return true;
}

struct Written {
  std::string plain, shown;
};
std::map<uint32_t, Written> g_written;  // player object -> its plain name and what we wrote (the game's thread only)
Clock::time_point g_names_at{};
uint32_t g_text = 0;  // 64 bytes of guest memory for the text handed to the game's string assign

void Assign(void* ctx_raw, uint8_t* base, uint32_t player, const std::string& text) {
  if (!g_text) g_text = REX_KERNEL_MEMORY()->SystemHeapAlloc(64);
  if (!g_text || text.size() > 40) return;
  std::memset(base + g_text, 0, 64);
  std::memcpy(base + g_text, text.data(), text.size());
  PPCContext call = *static_cast<PPCContext*>(ctx_raw);
  call.r3.u64 = player + kName;
  call.r4.u64 = g_text;
  sub_821D51C8(call, base);
}
}  // namespace

void GameNamesTick(void* ctx, uint8_t* base, bool level_loaded) {
  if (REXCVAR_GET(server)) return;
  const bool on = REXCVAR_GET(bf_clan_tags_in_game) && !REXCVAR_GET(bf_online_host).empty();
  const uint32_t manager = Load32(base, kManager);
  const uint32_t begin = Plausible(manager) ? Load32(base, manager + 0x98) : 0,
                 end = Plausible(manager) ? Load32(base, manager + 0x9C) : 0;
  const bool list_ok = Plausible(begin) && Plausible(end) && end >= begin && (end - begin) / 4 <= uint32_t(bf1943::kPlayerListLimit);
  if (!level_loaded || !on || !list_ok) {
    // The level is going (or the tags were turned off): the plain names back in every player still listed, so the
    // game's own bookkeeping at the end of a level sees the names it knows.
    if (!g_written.empty() && list_ok) {
      for (uint32_t e = begin; e < end; e += 4) {
        const uint32_t p = Load32(base, e);
        const auto it = g_written.find(p);
        std::string now;
        if (it != g_written.end() && IsPlayer(base, p) && ReadName(base, p, now) && now == it->second.shown &&
            now != it->second.plain)
          Assign(ctx, base, p, it->second.plain);
      }
    }
    g_written.clear();
    return;
  }
  const auto t = Clock::now();
  if (t - g_names_at < std::chrono::milliseconds(250)) return;
  g_names_at = t;
  std::map<uint32_t, Written> seen;
  for (uint32_t e = begin; e < end; e += 4) {
    const uint32_t p = Load32(base, e);
    std::string now;
    if (!IsPlayer(base, p) || !ReadName(base, p, now) || now.empty()) continue;
    Written w;
    const auto it = g_written.find(p);
    // Ours from before (the game has not set it since), or the game's own (a new player, or the game set it again).
    w.plain = it != g_written.end() && now == it->second.shown ? it->second.plain : now;
    const std::string tag = TagOf(w.plain);
    w.shown = tag.empty() ? w.plain : "[" + tag + "]" + w.plain;
    if (w.shown != now) {
      Assign(ctx, base, p, w.shown);
      std::string check;
      if (!ReadName(base, p, check) || check != w.shown) {
        REXLOG_WARN("BF clan tag: the name of player {:08X} did not take \"{}\"", p, w.shown);
        continue;
      }
      REXLOG_INFO("BF clan tag: player {} shown as {} in the match", w.plain, w.shown);
    }
    seen[p] = w;
  }
  g_written.swap(seen);
  std::lock_guard lock(g_mutex);
  g_match_names.clear();
  for (const auto& [p, w] : g_written) g_match_names.push_back(w.plain);
}

// ---- drawing
unsigned DevColour(int i, double seconds, float alpha) {
  // World at War's "cycl" (TekRantGaming, 16 Nov 2026: "the colors in your clan tag scroll and cycle continuously from
  // left to right", not a rainbow; his four frames of a World at War scoreboard): the whole "[Dev]Name" is one colour,
  // and the next one comes in at the left and travels to the right over it, the old colour left on the letters it has
  // not reached yet (green over orange, yellow over green, lilac over yellow), its front a soft edge two letters wide.
  // A new colour every second at 20 letters a second, so a full "[Dev]Name" (20 letters at most) is nearly always
  // in its sweep, as in his frames. The colours are the frames' soft ones, in their order.
  static constexpr unsigned char kColours[][3] = {{255, 155, 70},  {120, 230, 100}, {250, 230, 100},
                                                   {175, 145, 255}, {255, 150, 205}, {120, 220, 245}};
  constexpr long long kCount = sizeof(kColours) / sizeof(kColours[0]);
  constexpr double kPeriod = 1.0, kLettersPerSecond = 20.0, kEdge = 2.0;
  const double t = std::max(seconds, 0.0);
  const long long cycle = static_cast<long long>(t / kPeriod);
  const double front = (t - double(cycle) * kPeriod) * kLettersPerSecond;  // letters the new colour has reached
  const float mix = float(std::clamp((front - double(i)) / kEdge, 0.0, 1.0));  // 1 the new colour, 0 the old
  const unsigned char* now = kColours[cycle % kCount];
  const unsigned char* before = kColours[(cycle + kCount - 1) % kCount];
  auto blend = [mix](unsigned char a, unsigned char b) { return int(float(a) + (float(b) - float(a)) * mix + 0.5f); };
  return IM_COL32(blend(before[0], now[0]), blend(before[1], now[1]), blend(before[2], now[2]),
                  int(std::clamp(alpha, 0.0f, 1.0f) * 255));
}

float TaggedWidth(ImFont* font, float size, const std::string& name, const std::string& tag) {
  const std::string text = tag.empty() ? name : "[" + tag + "]" + name;
  return font->CalcTextSizeA(size, 1e9f, 0, text.c_str()).x;
}

float DrawTagged(ImDrawList* d, ImFont* font, float size, const ImVec2& at, unsigned ink, const std::string& name,
                 const std::string& tag, unsigned tag_ink, float alpha) {
  auto fade = [alpha](unsigned c) {
    const unsigned a = (c >> IM_COL32_A_SHIFT) & 0xFF;
    return (c & ~IM_COL32_A_MASK) | (unsigned(float(a) * std::clamp(alpha, 0.0f, 1.0f)) << IM_COL32_A_SHIFT);
  };
  float x = at.x;
  auto put = [&](const std::string& s, unsigned c) {
    d->AddText(font, size, ImVec2(x, at.y), c, s.c_str());
    x += font->CalcTextSizeA(size, 1e9f, 0, s.c_str()).x;
  };
  // A developer's whole tagged name cycles (TekRantGaming, 16 Nov 2026: "the full clan tag and user name").
  if (IsDevTag(tag)) return DrawDevText(d, font, size, at, "[" + tag + "]" + name, alpha, false);
  if (!tag.empty()) {
    put("[", fade(tag_ink));
    put(tag, fade(tag_ink));
    put("]", fade(tag_ink));
  }
  put(name, fade(ink));
  return x - at.x;
}

float DrawDevText(ImDrawList* d, ImFont* font, float size, const ImVec2& at, const std::string& text, float alpha,
                  bool shadow) {
  const double now = ImGui::GetTime();
  float x = at.x;
  const float off = size * 0.07f;
  int letter = 0;
  for (size_t i = 0; i < text.size(); ++i, ++letter) {
    char ch[4] = {text[i], 0, 0, 0};
    if (text.compare(i, player_name::kTm.size(), player_name::kTm) == 0) {  // the trademark sign, whole
      std::memcpy(ch, player_name::kTm.data(), player_name::kTm.size());
      i += player_name::kTm.size() - 1;
    }
    if (shadow) d->AddText(font, size, ImVec2(x + off, at.y + off), IM_COL32(0, 0, 0, int(180 * alpha)), ch);
    d->AddText(font, size, ImVec2(x, at.y), DevColour(letter, now, alpha), ch);
    x += font->CalcTextSizeA(size, 1e9f, 0, ch).x;
  }
  return x - at.x;
}

}  // namespace bf1943::clan
