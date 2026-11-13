// The mouse on the game's deploy screen (0.4.1): a free pointer that picks a kit, a spawn point and the screen's
// buttons.
//
// The game's menus are Flash movies with no pointer input at all (the research of 13 Nov: the runtime never raises a
// mouse event, and no movie has mouse code). So this is our own layer over the screen: the pointer is freed as for our
// own menu, and a click is turned into what the screen already understands, the controller's buttons and sticks, put
// into the pad the game reads (auto_join.cpp's pad hook):
//   - a kit card: d-pad left or right until that card is the chosen one (the screen's onKitLeft adds one to its kit
//     index, onKitRight takes one off, round the three; the cards stand Rifleman 1, Infantry 0, Scout 2);
//   - Set to flag spawn, Enter battle, Back: Y, A, B;
//   - a spawn point on the map: chosen directly, with the screen's own script and no stick. The screen's script
//     (igm_mp_cq, read with tools/apt_disasm.py, 13 Nov) keeps an index into the list of the points the player may
//     spawn at, and only its flag mode lets the player change it (in its frontline mode, "auto-spawn", setSpawnInfo
//     puts the index back on the front line each time: why steps and stick pushes did nothing in the runs before).
//     So a click on a point: toggleSpawnMode() when the index does not move (frontline to flag: m_spawnMode = 1 -
//     m_spawnMode), then onSpawnDown() or onSpawnUp() (the index plus or less one, round the list) until the point
//     the screen tells the game (selectSpawnPoint, the point's own number) is the clicked one.
// Where things are on the screen was measured from a screenshot of it (13 Nov, 1280 x 720 stage units). The map: a
// least squares fit of Wake Island's five flags in that screenshot to their places in the world put every one within
// a unit, and the carrier's marker too: 0.332 units a metre, turned 20 degrees, the world's origin at 920, 358. Other
// levels are taken to be drawn the same way (not yet checked: bf_debug_spawn_mouse logs the clicks).
// The screen's script is reached at _root.contentMC.loader1 (probed 13 Nov: onKitLeft, onSpawnUp, setKitInfo,
// gotoNextSpawnPoint and spawnmap.sp0 all resolve there).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "capture_points.h"

REXCVAR_DEFINE_BOOL(bf_spawn_mouse, true, "BF1943/Controls",
                    "On the deploy screen the mouse is a free pointer: click a kit, the map and the buttons (false: the "
                    "mouse pushes the right stick there, as before)");
REXCVAR_DEFINE_BOOL(bf_debug_spawn_mouse, false, "BF1943/Controls",
                    "Log the deploy screen's commands, the capture points and the map's icons (bf1943.log)");

namespace bf1943 {
void FlashCall(const std::string& path);  // auto_join.cpp ("probe:<path>" logs whether a script path resolves)

namespace {
using Clock = std::chrono::steady_clock;

// XInput's buttons, as the pad hook has them.
constexpr uint16_t kDpadLeft = 0x0004, kDpadRight = 0x0008, kA = 0x1000, kB = 0x2000, kY = 0x8000;

struct Action {
  uint16_t buttons = 0;
  float rx = 0, ry = 0;  // the right stick, -1 to 1 (up is +1)
  int hold_ms = 120, gap_ms = 140;
};

struct State {
  std::mutex mutex;
  bool active = false;        // the deploy screen is up and the pointer is ours
  Clock::time_point seen{};   // the pad hook last said the screen may be up
  Clock::time_point since{};  // and since when, without a break
  std::deque<Action> actions;
  Action now;
  Clock::time_point now_until{}, gap_until{};
  int kit = -1;    // the screen's kit index as it last told the game (0 Infantry, 1 Rifleman, 2 Scout); -1 not known
  int spawn = -1;  // its spawn point index, as last told
  int icons_logged = 0;
  Clock::time_point said_at{};  // the screen last told the game its kit or spawn point (it does while it is up)
  // A spawn point asked for by a click: its number, and the steps toward it (the pad hook makes them).
  int want = -1, want_steps = 0, want_toggles = 0, want_count = 1, want_from = -2;
  bool want_down = true;
  Clock::time_point want_at{};
} g;
std::atomic<bool> g_active{false};

uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
float LoadF(const uint8_t* base, uint32_t a) {
  const uint32_t v = Load32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
bool Guest(uint32_t a) { return a >= 0x10000 && a < 0xF0000000; }

// The screen in stage units (1280 x 720).
struct Box {
  float x0, y0, x1, y1;
  bool Has(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};
constexpr Box kKitCards[3] = {{102, 217, 251, 497}, {257, 217, 406, 497}, {412, 217, 561, 497}};
constexpr int kKitOfCard[3] = {1, 0, 2};  // Rifleman, Infantry, Scout
// The three buttons are black brush strokes, each a little off the level with ragged ends: its outline as a polygon,
// traced from a screenshot of the screen (13 Nov: the highlight "doesn't trace the outline of the black brush stroke").
struct Stroke {
  const ImVec2* points;
  int count;
  bool Has(float x, float y) const {  // inside the outline (crossings of a ray to the right)
    bool in = false;
    for (int i = 0, j = count - 1; i < count; j = i++) {
      const ImVec2 &a = points[i], &b = points[j];
      if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
  }
};
constexpr ImVec2 kSpawnModePoints[] = {{92.0f, 540.0f}, {129.0f, 534.5f}, {195.5f, 528.0f}, {254.0f, 523.5f}, {315.5f, 520.5f}, {319.0f, 529.5f}, {324.0f, 544.5f}, {320.5f, 553.5f}, {254.0f, 559.5f}, {179.0f, 564.5f}, {129.0f, 568.0f}, {95.0f, 569.0f}, {90.5f, 554.5f}};
constexpr ImVec2 kEnterPoints[] = {{90.5f, 576.0f}, {129.0f, 572.0f}, {195.5f, 568.0f}, {254.0f, 566.0f}, {258.0f, 567.0f}, {264.0f, 591.0f}, {261.5f, 596.0f}, {195.5f, 602.0f}, {129.0f, 607.0f}, {97.5f, 609.0f}, {90.5f, 603.0f}, {90.0f, 589.5f}};
constexpr ImVec2 kBackPoints[] = {{89.0f, 616.0f}, {129.0f, 612.0f}, {169.0f, 607.5f}, {185.5f, 606.0f}, {189.0f, 609.5f}, {190.5f, 636.0f}, {187.5f, 642.5f}, {129.0f, 645.5f}, {91.5f, 647.5f}, {88.0f, 632.0f}};
constexpr Stroke kSpawnMode{kSpawnModePoints, int(std::size(kSpawnModePoints))},
    kEnter{kEnterPoints, int(std::size(kEnterPoints))}, kBack{kBackPoints, int(std::size(kBackPoints))};
constexpr Box kMap{623, 119, 1180, 497};
// The map: a place in the world (x, z in metres) in stage units.
constexpr float kMapA = 0.311910f, kMapB = -0.113804f, kMapX = 920.03f, kMapY = 358.16f;
ImVec2 OnMap(float wx, float wz) { return ImVec2(kMapA * wx - kMapB * wz + kMapX, kMapB * wx + kMapA * wz + kMapY); }
constexpr float kPointReach = 30.0f;  // a click this near a point (stage units) is a click on it
// The number the screen sends the game for a spawn point (selectSpawnPoint: the entry's own number in the list the
// game gave the screen, not the screen's index into it). The game numbers the level's points from the end of its
// own list: on Wake Island (run of 13 Nov 04:16) the US carrier, sixth of seven, was 1 and the South Base, the
// first, was 6. Taken as that for every level (bf_debug_spawn_mouse logs each point's number to check it).
int SpawnNumber(const CapturePoint& p, size_t count) { return int(count) - 1 - p.order; }

void Queue(const Action& a) {
  if (g.actions.size() < 8) g.actions.push_back(a);
}

class SpawnMouseOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit SpawnMouseOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!g_active.load(std::memory_order_relaxed)) return;
    if (io.DisplaySize.x < 16 || io.DisplaySize.y < 16) return;
    // The stage: 16:9 in the middle of the picture (the renderer keeps a menu there on wider and taller screens).
    const float s = std::min(io.DisplaySize.x / 1280.0f, io.DisplaySize.y / 720.0f);
    const float ox = (io.DisplaySize.x - 1280.0f * s) * 0.5f, oy = (io.DisplaySize.y - 720.0f * s) * 0.5f;
    const float mx = (io.MousePos.x - ox) / s, my = (io.MousePos.y - oy) / s;
    const bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    auto outline = [&](const Box& b) {
      dl->AddRect(ImVec2(ox + b.x0 * s, oy + b.y0 * s), ImVec2(ox + b.x1 * s, oy + b.y1 * s),
                  IM_COL32(255, 236, 170, 220), 3.0f * s, 0, 2.5f * s);
    };
    std::lock_guard lock(g.mutex);
    for (int card = 0; card < 3; ++card) {
      if (!kKitCards[card].Has(mx, my)) continue;
      outline(kKitCards[card]);
      if (!click) break;
      const int want = kKitOfCard[card];
      if (g.kit < 0 || g.kit > 2) {
        // Not known yet (the screen has not told the game a kit): one step toward that side shows which it was.
        Queue({card == 0 ? kDpadLeft : kDpadRight});
        REXLOG_INFO("BF spawn mouse: kit card {} clicked, the chosen kit not known yet: one step", card);
      } else {
        const int left = ((want - g.kit) % 3 + 3) % 3;  // d-pad left adds one
        if (left == 1) Queue({kDpadLeft});
        if (left == 2) Queue({kDpadRight});
        REXLOG_INFO("BF spawn mouse: kit card {} clicked (kit {}, was {})", card, want, g.kit);
      }
      break;
    }
    const struct {
      const Stroke& stroke;
      uint16_t button;
      const char* name;
    } buttons[] = {{kSpawnMode, kY, "spawn mode"}, {kEnter, kA, "enter battle"}, {kBack, kB, "back"}};
    for (const auto& b : buttons) {
      const Stroke& k = b.stroke;
      if (!k.Has(mx, my)) continue;
      // The highlight is the stroke's own outline: a warm wash over it and an edge round it.
      ImVec2 outline_points[16];
      const int n = std::min(k.count, 16);
      for (int i = 0; i < n; ++i) outline_points[i] = ImVec2(ox + k.points[i].x * s, oy + k.points[i].y * s);
      dl->AddConvexPolyFilled(outline_points, n, IM_COL32(255, 236, 170, 46));
      dl->AddPolyline(outline_points, n, IM_COL32(255, 236, 170, 230), ImDrawFlags_Closed, 2.0f * s);
      if (click) {
        Queue({b.button});
        REXLOG_INFO("BF spawn mouse: {} clicked", b.name);
      }
    }
    if (kMap.Has(mx, my)) {
      // The capture point nearest the pointer, where the map shows it.
      const std::vector<CapturePoint> points = CapturePoints();
      const CapturePoint* near_point = nullptr;
      float best = kPointReach * kPointReach;
      ImVec2 at{};
      for (const CapturePoint& p : points) {
        const ImVec2 m = OnMap(p.x, p.z);
        const float d2 = (m.x - mx) * (m.x - mx) + (m.y - my) * (m.y - my);
        if (d2 < best && kMap.Has(m.x, m.y)) best = d2, near_point = &p, at = m;
      }
      if (near_point) {
        const ImVec2 c(ox + at.x * s, oy + at.y * s);
        dl->AddCircle(c, 17.0f * s, IM_COL32(0, 0, 0, 170), 32, 4.5f * s);
        dl->AddCircle(c, 17.0f * s, IM_COL32(255, 236, 170, 235), 32, 2.5f * s);
      }
      if (click) {
        if (near_point) {
          g.want = SpawnNumber(*near_point, points.size());
          g.want_steps = 0, g.want_toggles = 0, g.want_at = {}, g.want_from = -2;
          // The points the player's team holds: how many steps round the list can be needed, and which way is
          // shorter (the list is in the game's order, the numbers running down it).
          // The player's team: the owner of a point the game's maps show in the team's colour, else of the point the
          // screen is on now.
          int team = 0;
          for (const CapturePoint& p : points)
            if (SpawnNumber(p, points.size()) == g.spawn && p.owner) team = p.owner;
          for (const CapturePoint& p : points)
            if (p.shown == 1 && p.owner) team = p.owner;
          if (team && near_point->owner != team) {
            REXLOG_INFO("BF spawn mouse: {} is not the team's: no spawn there", near_point->name);
            g.want = -1;
          }
          std::vector<int> held;
          for (const CapturePoint& p : points)
            if (team && p.owner == team) held.push_back(SpawnNumber(p, points.size()));
          std::sort(held.begin(), held.end(), std::greater<int>());
          g.want_count = std::max<int>(1, int(held.size()));
          const auto cur = std::find(held.begin(), held.end(), g.spawn), to = std::find(held.begin(), held.end(), g.want);
          g.want_down = true;
          if (cur != held.end() && to != held.end()) {
            const int n = int(held.size()), forward = (int(to - held.begin()) - int(cur - held.begin()) + n) % n;
            g.want_down = forward <= n - forward;
          }
        }
        REXLOG_INFO("BF spawn mouse: map clicked at stage {:.0f}, {:.0f}: {} (number {}); the screen is on {}", mx, my,
                    near_point ? near_point->name : "no point there", near_point ? g.want : -1, g.spawn);
      }
    }
  }
};
}  // namespace

bool SpawnMouseActive() { return g_active.load(std::memory_order_relaxed); }

// From the pad hook (auto_join.cpp), at each of the game's pad reads: whether the deploy screen may be up (a level is
// loaded, the player is in no seat and not on foot, our own menu is closed), and the pad as the game will read it
// (X_INPUT_GAMEPAD, big endian: buttons at +0, the right stick at +8 and +10). The queued presses are put in.
void SpawnMousePad(uint8_t* gamepad, bool allowed, bool out_of_world) {
  const auto now = Clock::now();
  std::lock_guard lock(g.mutex);
  const bool on = allowed && out_of_world && REXCVAR_GET(bf_spawn_mouse);
  if (on) {
    if (now - g.seen > std::chrono::milliseconds(300)) g.since = now;
    g.seen = now;
  }
  // The screen is up while it talks to the game: it sends its kit and spawn point when it opens and again every
  // second and a half, until doSpawn (SpawnMouseCommand). After a death the soldier still counts as in the world, so
  // being out of the world alone left the mouse turning the camera behind the screen (13 Nov: "after player death,
  // the mouse is stuck with mouse look on the spawn screen"). Else: out of the world for 0.7 s without a break (a
  // moment between two seats is not the deploy screen).
  const bool talking = allowed && REXCVAR_GET(bf_spawn_mouse) && now - g.said_at < std::chrono::milliseconds(2500);
  const bool active = talking || (on && now - g.since > std::chrono::milliseconds(700));
  if (active != g.active) {
    g.active = active;
    g_active.store(active, std::memory_order_relaxed);
    g.actions.clear();
    g.now = {}, g.now_until = {}, g.gap_until = {};
    g.icons_logged = 0;
    g.want = -1;
    REXLOG_INFO("BF spawn mouse: the pointer is {}", active ? "free (deploy screen)" : "the game's again");
    if (active && REXCVAR_GET(bf_debug_spawn_mouse)) {
      for (const CapturePoint& p : CapturePoints())
        REXLOG_INFO("BF spawn mouse: capture point {} index {} order {} letter {} at x {:.1f} z {:.1f}, owner {}, shown {}",
                    p.name, p.index, p.order, p.letter < 0 ? '-' : char('A' + p.letter), p.x, p.z, p.owner, p.shown);
    }
  }
  if (!active) return;
  // A spawn point asked for by a click (see the top): one call of the screen's script at a time, each after the
  // screen has told the game where it is now.
  if (g.want >= 0) {
    static const char* const kScreen = "_root.contentMC.loader1";
    if (g.spawn == g.want) {
      g.want = -1;
    } else if (now >= g.want_at) {
      if (g.spawn < 0 && g.want_toggles == 0) {
        // The screen is on squad spawn (it sends no point): to flag spawn first.
        FlashCall(std::string(kScreen) + ".toggleSpawnMode");
        ++g.want_toggles, g.want_steps = 0, g.want_from = -2;
        g.want_at = now + std::chrono::milliseconds(300);
        REXLOG_INFO("BF spawn mouse: the screen set from squad spawn to flag spawn");
      } else if (g.spawn < 0) {
        // The change of mode led to squad spawn: it was on flag spawn already. Back, and no more.
        FlashCall(std::string(kScreen) + ".toggleSpawnMode");
        REXLOG_INFO("BF spawn mouse: spawn point {} not reached: the screen's list has no other point", g.want);
        g.want = -1;
      } else {
        const bool stuck = g.want_steps > 0 && g.spawn == g.want_from;  // the last step did not move the choice
        if ((stuck || g.want_steps >= g.want_count) && g.want_toggles < 1) {
          FlashCall(std::string(kScreen) + ".toggleSpawnMode");  // frontline (auto-spawn) to flag spawn
          ++g.want_toggles, g.want_steps = 0, g.want_from = -2;
          g.want_at = now + std::chrono::milliseconds(300);
          REXLOG_INFO("BF spawn mouse: the choice does not move: the screen's spawn mode changed");
        } else if (stuck || g.want_steps >= g.want_count) {
          REXLOG_INFO("BF spawn mouse: spawn point {} not reached (the screen stays on {})", g.want, g.spawn);
          g.want = -1;
        } else {
          // A method of the screen's, run on the screen itself (auto_join.cpp "method:": as a queued call its `this`
          // was the root, and the step changed nothing).
          g.want_from = g.spawn;
          FlashCall(std::string("method:") + kScreen + (g.want_down ? "|onSpawnDown" : "|onSpawnUp"));
          ++g.want_steps;
          g.want_at = now + std::chrono::milliseconds(220);
        }
      }
    }
  }
  if (now >= g.now_until && now >= g.gap_until && !g.actions.empty()) {
    g.now = g.actions.front();
    g.actions.pop_front();
    g.now_until = now + std::chrono::milliseconds(g.now.hold_ms);
    g.gap_until = g.now_until + std::chrono::milliseconds(g.now.gap_ms);
  }
  if (now < g.now_until) {
    const uint16_t buttons = uint16_t(gamepad[0] << 8 | gamepad[1]) | g.now.buttons;
    gamepad[0] = uint8_t(buttons >> 8), gamepad[1] = uint8_t(buttons);
    auto put = [](uint8_t* p, float v) {
      const int i = std::clamp(int(std::lround(v * 32767.0f)), -32768, 32767);
      p[0] = uint8_t(uint16_t(i) >> 8), p[1] = uint8_t(i);
    };
    if (g.now.rx != 0 || g.now.ry != 0) put(gamepad + 8, g.now.rx), put(gamepad + 10, g.now.ry);
  }
}

// From the game command hook (auto_join.cpp): what the deploy screen tells the game. True: not to be passed on
// (never, now).
bool SpawnMouseCommand(const char* name, const char* args) {
  if (!name) return false;
  const bool kit = std::strcmp(name, "selectKit") == 0, spawn = std::strcmp(name, "selectSpawnPoint") == 0;
  const bool ours = kit || spawn || std::strcmp(name, "selectSquadSpawn") == 0 || std::strcmp(name, "doSpawn") == 0 ||
                    std::strcmp(name, "selectWeapon") == 0;
  std::lock_guard lock(g.mutex);
  // The screen is up while it says these, and gone at doSpawn (or when the game's menus are left: enterMenu 0).
  if (std::strcmp(name, "doSpawn") == 0 || (std::strcmp(name, "enterMenu") == 0 && args && args[0] == '0'))
    g.said_at = {};
  else if (ours)
    g.said_at = Clock::now();
  if (!ours) return false;
  if (args && *args) {
    char* end = nullptr;
    const long v = std::strtol(args, &end, 10);
    if (end != args) {
      if (kit) g.kit = int(v);
      if (spawn) g.spawn = int(v);
    }
  }
  if (REXCVAR_GET(bf_debug_spawn_mouse)) REXLOG_INFO("BF spawn mouse: the screen sent {} ({})", name, args ? args : "");
  return false;
}

void CreateSpawnMouseOverlay(rex::ui::ImGuiDrawer* drawer) {
  new SpawnMouseOverlay(drawer);  // lives for the rest of the session
}
}  // namespace bf1943

// sub_8241FA58(map, r4 a vector, r5 the item, r6 the picture's rectangle, ...): one icon drawn on a map of the game's
// (capture_points.cpp: the item's place in the world is at item - 0x30 with the item at r5 + 160). Logged while the
// deploy screen is up (bf_debug_spawn_mouse), to find how its map places the world on the screen.
REX_EXTERN(__imp__sub_8241FA58);
REX_HOOK_RAW(sub_8241FA58) {
  using namespace bf1943;
  if (g_active.load(std::memory_order_relaxed) && REXCVAR_GET(bf_debug_spawn_mouse)) {
    const uint32_t map = ctx.r3.u32, vec = ctx.r4.u32, item = ctx.r5.u32, rect = ctx.r6.u32;
    std::lock_guard lock(g.mutex);
    if (g.icons_logged < 60 && Guest(vec) && Guest(item) && Guest(rect)) {
      ++g.icons_logged;
      REXLOG_INFO("BF spawn mouse: map {:08X} icon: vector {:.2f} {:.2f} {:.2f} {:.2f}, world x {:.1f} z {:.1f}, picture "
                  "{:.3f} {:.3f} {:.3f} {:.3f}, r7 {:08X} r8 {}",
                  map, LoadF(base, vec), LoadF(base, vec + 4), LoadF(base, vec + 8), LoadF(base, vec + 12),
                  LoadF(base, item + 112), LoadF(base, item + 120), LoadF(base, rect), LoadF(base, rect + 4),
                  LoadF(base, rect + 8), LoadF(base, rect + 12), ctx.r7.u32, ctx.r8.u32);
    }
  }
  __imp__sub_8241FA58(ctx, base);
}
