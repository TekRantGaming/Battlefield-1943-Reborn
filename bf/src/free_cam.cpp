// The admin's free camera and the hidden HUD (TekRantGaming, 24 Nov 2026: "a free cam mode, akin to something like
// NoClip, especially with an option to completely disable the HUD and on-screen UI elements ... for getting clips for
// the trailers ... The controller must be workable in the free cam mode ... analog sticks. A to ascend, B to descend,
// and the sprint toggle button to speed up the camera should be something like left trigger").
//
// For a match's admin only (the F9 panel, world_sync.cpp: the server's admin list or the host's key). The camera flies
// through everything (no collision), the soldier stays where he was (he can still be shot), and the game gets an empty
// pad while it flies, so he does not move. Controller (and the keyboard through Keyboard & mouse's bindings):
//   left stick       fly along the view and sideways        right stick   look
//   A (Space)        up                                     B (E)         down
//   LT               faster, the further it is pulled        RT            slower, for slow pans (look too)
//   L3 (Sprint)      fast on or off                          LB / RB       the base speed down or up a step
//   Y                the HUD and on-screen UI hidden or shown
//   Back (View)      the free camera off
// The mouse looks (while it is locked to the window). Moving and turning ease in and out, so a clip has no jerks.
//
// The camera (as tools/bench's, bench_freecam.cpp): the client builds a FreeCamera at its start (sub_8224FB50: client
// +0xA4 the camera); the client's camera mode switch sub_82254B28(client, mode) makes it the view with mode 1 (mode 0
// takes the same path). Back to the player's own view is mode 4 (run naop_cam1: 0 left the free camera on): its path
// takes the camera of the object the player controls (client +0xC4) and turns off what mode 1 turned on
// (sub_82373020(..., 0) where mode 1 passes 1). Its matrix at camera +0x10 (rows right, up, forward, place; it looks along minus forward) is written at
// every pad read (auto_join.cpp's XamInputGetState), on the game's thread; the camera's own update does not run.
//
// Hidden HUD (Y, or F9's "Hide HUD and UI"): the game's HUD (native_x_renderer_hide_hud) and every overlay of ours
// (chat, scoreboard extras, the round clock, the performance figures...) are not drawn while our menus and the F9 panel
// are closed. It works in first person too. The game's HUD is always hidden while the free camera flies (the soldier's
// crosshair and map would be wrong for its view).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ui/imgui_dialog.h>

#include "comms.h"
#include "free_cam.h"
#include "world_sync.h"

REXCVAR_DEFINE_DOUBLE(bf_free_cam_speed, 10.0, "BF1943/Admin", "The admin's free camera: its base speed, metres a second (1 to 80)");

namespace bf1943 {
bool InMatch();       // auto_join.cpp
bool GameMenuOpen();  // game_menu.cpp
}  // namespace bf1943

namespace {

constexpr float kPi = 3.14159265f;
std::atomic<uint32_t> g_client{0};

uint32_t Load32(const uint8_t* base, uint32_t at) {
  uint32_t v;
  std::memcpy(&v, base + at, 4);
  return __builtin_bswap32(v);
}
void StoreF(uint8_t* base, uint32_t at, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  std::memcpy(base + at, &v, 4);
}

struct V3 {
  float x = 0, y = 0, z = 0;
};
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Norm(V3 a) {
  const float l = std::sqrt(Dot(a, a));
  return l > 1e-6f ? a * (1.f / l) : V3{0, 0, 1};
}
void StoreRow(uint8_t* base, uint32_t at, V3 v, float w) {
  StoreF(base, at, v.x), StoreF(base, at + 4, v.y), StoreF(base, at + 8, v.z), StoreF(base, at + 12, w);
}

// Asked from the panel (any thread), done at the next pad read (the game's thread).
std::atomic<bool> g_want{false}, g_on{false}, g_hide_ui{false};
// The mouse's motion while the camera flies (look.cpp), in counts.
std::mutex g_mouse_mutex;
float g_mouse_dx = 0, g_mouse_dy = 0;

// The camera's flight (the game's thread).
struct Flight {
  V3 place;
  float yaw = 0, pitch = 0;      // the look: x = cos(pitch) sin(yaw), y = sin(pitch), z = cos(pitch) cos(yaw)
  V3 velocity;                   // eased toward the sticks' wish
  float yaw_rate = 0, pitch_rate = 0;  // radians a second, eased
  float handed = 1;              // +1: right = up x forward, as the soldier's matrix has it
  bool fast = false;             // L3's toggle
  float base_speed = 10;         // m/s (LB, RB, bf_free_cam_speed)
  uint16_t last_buttons = 0;
  std::chrono::steady_clock::time_point at{};
} g_f;

V3 Look(float yaw, float pitch) {
  return {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
}

void ApplyHud() {
  rex::cvar::SetFlagByName("native_x_renderer_hide_hud", g_on.load() || g_hide_ui.load() ? "true" : "false");
}

constexpr int kFreeView = 1, kPlayerView = 4;  // sub_82254B28's modes
void SwitchView(PPCContext& ctx, uint8_t* base, uint32_t client, int mode);

}  // namespace

// The client's start (sub_8224FB50, r3 the client): its free camera made; the client kept for the mode switch.
REX_EXTERN(__imp__sub_8224FB50);
REX_HOOK_RAW(sub_8224FB50) {
  const uint32_t client = ctx.r3.u32;
  __imp__sub_8224FB50(ctx, base);
  g_client = client;
}

REX_EXTERN(sub_82254B28);  // the client's camera mode switch

namespace {
void SwitchView(PPCContext& ctx, uint8_t* base, uint32_t client, int mode) {
  PPCContext call = ctx;
  call.r3.u64 = client;
  call.r4.u64 = uint64_t(mode);
  sub_82254B28(call, base);
}
}  // namespace

namespace bf1943::freecam {

uint32_t Client() { return g_client.load(); }
bool Active() { return g_on.load(); }
bool Wanted() { return g_want.load(); }
void Set(bool on) {
  if (g_want.exchange(on) != on) REXLOG_INFO("BF free camera: {} asked", on ? "on" : "off");
}
bool HideUi() { return g_hide_ui.load(); }
void SetHideUi(bool hide) {
  if (g_hide_ui.exchange(hide) == hide) return;
  REXLOG_INFO("BF free camera: the HUD and on-screen UI {}", hide ? "hidden" : "shown");
  ApplyHud();
}

bool TakeMouse(float dx, float dy) {
  if (!g_on.load(std::memory_order_relaxed)) return false;
  std::lock_guard lock(g_mouse_mutex);
  g_mouse_dx += dx, g_mouse_dy += dy;
  return true;
}

void Pad(PPCContext& ctx, uint8_t* base, uint8_t* pad, bool input) {
  static uint8_t neutral[12];
  if (!input) std::memset(neutral, 0, sizeof(neutral)), pad = neutral;  // our menu or the chat has the pad
#if defined(BF_DEV_TOOLS)
  // BF_DEV_FREE_CAM=<s> (tests: no controller in a test run): that long after this game became a match's admin the
  // free camera is turned on, and BF_DEV_FREE_CAM_SCRIPT="<s>:<lx>,<ly>,<rx>,<ry>,<buttons hex>[,<lt>,<rt>];..." is
  // its pad from then (seconds since it came on; sticks -1 to 1, triggers 0 to 1; each item holds until the next),
  // in place of the real one: Y (8000) hides the HUD, Back (0020) ends it.
  {
    struct Item {
      double at;
      float lx, ly, rx, ry, lt, rt;
      unsigned buttons;
    };
    static const double start = [] {
      const char* v = std::getenv("BF_DEV_FREE_CAM");
      return v && *v ? std::atof(v) : -1.0;
    }();
    static const std::vector<Item> script = [] {
      std::vector<Item> out;
      const char* v = std::getenv("BF_DEV_FREE_CAM_SCRIPT");
      for (const char* c = v ? v : ""; *c;) {
        Item it{};
        if (std::sscanf(c, "%lf:%f,%f,%f,%f,%x,%f,%f", &it.at, &it.lx, &it.ly, &it.rx, &it.ry, &it.buttons, &it.lt, &it.rt) >= 6)
          out.push_back(it);
        c = std::strchr(c, ';');
        c = c ? c + 1 : "";
      }
      return out;
    }();
    static std::chrono::steady_clock::time_point admin_at{}, on_at{};
    static bool started = false;
    const auto now = std::chrono::steady_clock::now();
    if (start >= 0) {
      const bool admin = InMatch() && world::Current().admin;
      if (admin && admin_at == std::chrono::steady_clock::time_point{}) admin_at = now;
      if (admin && !started && std::chrono::duration<double>(now - admin_at).count() > start) {
        started = true, on_at = now;
        REXLOG_INFO("BF free camera: the test turns it on ({} pad items)", script.size());
        Set(true);
      }
      if (started && g_want.load() && input) {
        const double t = std::chrono::duration<double>(now - on_at).count();
        const Item* cur = nullptr;
        for (const Item& it : script)
          if (t >= it.at) cur = &it;
        if (cur) {
          static uint8_t fake[12];
          auto put = [&](int at, float v) {
            const int16_t s = int16_t(std::clamp(v, -1.f, 1.f) * 32767.f);
            fake[at] = uint8_t(uint16_t(s) >> 8), fake[at + 1] = uint8_t(s);
          };
          fake[0] = uint8_t(cur->buttons >> 8), fake[1] = uint8_t(cur->buttons);
          fake[2] = uint8_t(std::clamp(cur->lt, 0.f, 1.f) * 255), fake[3] = uint8_t(std::clamp(cur->rt, 0.f, 1.f) * 255);
          put(4, cur->lx), put(6, cur->ly), put(8, cur->rx), put(10, cur->ry);
          std::memset(pad, 0, 12);  // the real pad's (a test window's) never moves it
          pad = fake;
        }
      }
    }
  }
#endif
  const uint32_t client = g_client.load();
  const bool on = g_on.load();
  const bool allowed = InMatch() && world::Current().admin && client;
  if (on && (!g_want.load() || !allowed || !Load32(base, client + 0xA4))) {
    // Off: the soldier's view again (asked, or the match, the admin's right or the client gone).
    if (InMatch() && client && Load32(base, client + 0xA4)) SwitchView(ctx, base, client, kPlayerView);
    g_on = false, g_want = false;
    ApplyHud();
    REXLOG_INFO("BF free camera: off ({}) at {:.1f} {:.1f} {:.1f}", allowed ? "asked" : "the match or the admin's right ended",
                g_f.place.x, g_f.place.y, g_f.place.z);
    return;
  }
  if (!on && g_want.load()) {
    const uint32_t camera = allowed ? Load32(base, client + 0xA4) : 0;
    float pos[3], right[3], back[3];
    if (!camera || !comms::Camera(pos, right, back)) {
      g_want = false;
      REXLOG_INFO("BF free camera: not now ({})", !allowed ? "not an admin in a match" : "no view yet");
      return;
    }
    // From where the player's view is, looking the same way.
    const V3 b{back[0], back[1], back[2]}, r{right[0], right[1], right[2]};
    const V3 look = Norm(b * -1.f);
    g_f.place = {pos[0], pos[1], pos[2]};
    g_f.yaw = std::atan2(look.x, look.z);
    g_f.pitch = std::asin(std::clamp(look.y, -1.f, 1.f));
    g_f.handed = Dot(Cross(V3{0, 1, 0}, b), r) >= 0 ? 1.f : -1.f;
    g_f.velocity = {}, g_f.yaw_rate = g_f.pitch_rate = 0, g_f.fast = false;
    g_f.base_speed = float(std::clamp(REXCVAR_GET(bf_free_cam_speed), 1.0, 80.0));
    g_f.last_buttons = uint16_t(pad[0] << 8 | pad[1]);  // what is held as it starts acts only once let go
    g_f.at = std::chrono::steady_clock::now();
    {
      std::lock_guard lock(g_mouse_mutex);
      g_mouse_dx = g_mouse_dy = 0;
    }
    SwitchView(ctx, base, client, kFreeView);
    g_on = true;
    ApplyHud();
    REXLOG_INFO("BF free camera: on at {:.1f} {:.1f} {:.1f}, looking {:.2f} {:.2f} {:.2f}, {:.0f} m/s", g_f.place.x,
                g_f.place.y, g_f.place.z, look.x, look.y, look.z, g_f.base_speed);
  }
  if (!g_on.load()) return;
  const uint32_t camera = Load32(base, client + 0xA4);
  const auto now = std::chrono::steady_clock::now();
  const float dt = std::clamp(std::chrono::duration<float>(now - g_f.at).count(), 0.0f, 0.1f);
  g_f.at = now;
  // The pad as the game would have had it (the keyboard's bindings in it), then nothing of it for the game.
  const uint16_t buttons = uint16_t(pad[0] << 8 | pad[1]);
  const float lt = pad[2] / 255.f, rt = pad[3] / 255.f;
  auto stick = [&](int at) {
    const float v = float(int16_t(uint16_t(pad[at]) << 8 | pad[at + 1])) / 32767.f;
    constexpr float kDead = 0.24f;  // XInput's left stick dead zone (7849)
    const float a = std::fabs(v);
    return a <= kDead ? 0.f : std::copysign(std::min(1.f, (a - kDead) / (1 - kDead)), v);
  };
  const float lx = stick(4), ly = stick(6), rx = stick(8), ry = stick(10);
  std::memset(pad, 0, 12);
  // The server's idle kick (sub_8237F1F8: a player whose input stays still for the server's idle time is kicked; the
  // user, 26 Nov 2026: "i still get kicked from the game when using free cam as an admin i think its the idel kick
  // timer"): the game gets an empty pad while the camera flies, so every 600 pad reads (10 s at 60 frames) its look
  // stick goes three quarters right for four reads and as far left for four: the soldier turns there and back, and the
  // server sees input.
  {
    static uint32_t reads = 0;
    const uint32_t phase = reads++ % 600;
    if (phase < 8) {
      const uint16_t v = uint16_t(int16_t(phase < 4 ? 24576 : -24576));
      pad[8] = uint8_t(v >> 8), pad[9] = uint8_t(v & 0xFF);
    }
  }
  const uint16_t pressed = buttons & ~g_f.last_buttons;
  g_f.last_buttons = buttons;
  if (pressed & 0x0020) {  // Back: off
    g_want = false;
    return;  // the next read switches back
  }
  if (pressed & 0x8000) SetHideUi(!g_hide_ui.load());  // Y
  if (pressed & 0x0040) {                               // L3: fast on or off
    g_f.fast = !g_f.fast;
    REXLOG_INFO("BF free camera: fast {}", g_f.fast ? "on" : "off");
  }
  if (pressed & 0x0300) {  // LB down, RB up a step
    g_f.base_speed = std::clamp(g_f.base_speed * (pressed & 0x0200 ? 1.5f : 1 / 1.5f), 1.0f, 80.0f);
    REXLOG_INFO("BF free camera: base speed {:.1f} m/s", g_f.base_speed);
  }
  // Look: up to 100 degrees a second at full stick (a third with RT held), eased over about a fifth of a second, and
  // the mouse's counts at 0.066 degrees each (look.cpp's).
  const float slow = 1.0f - 0.8f * rt;
  const float look_rate = 100.f * kPi / 180.f * (1.0f - 0.66f * rt);
  const float ease = 1.f - std::exp(-dt * 6.f);
  g_f.yaw_rate += (rx * look_rate - g_f.yaw_rate) * ease;
  g_f.pitch_rate += (ry * look_rate - g_f.pitch_rate) * ease;
  float mdx, mdy;
  {
    std::lock_guard lock(g_mouse_mutex);
    mdx = g_mouse_dx, mdy = g_mouse_dy;
    g_mouse_dx = g_mouse_dy = 0;
  }
  const float per_count = 0.066f * kPi / 180.f;
  // Turning right: the way that takes the look toward the camera's right (the game's matrices' handedness).
  const V3 look0 = Look(g_f.yaw, g_f.pitch);
  const V3 right0 = Norm(Cross(V3{0, 1, 0}, look0 * -1.f) * g_f.handed);
  const V3 d_yaw{std::cos(g_f.yaw), 0, -std::sin(g_f.yaw)};
  const float turn_sign = Dot(d_yaw, right0) >= 0 ? 1.f : -1.f;
  g_f.yaw += turn_sign * (g_f.yaw_rate * dt + mdx * per_count);
  g_f.pitch = std::clamp(g_f.pitch + g_f.pitch_rate * dt - mdy * per_count, -1.5f, 1.5f);
  g_f.yaw = std::remainder(g_f.yaw, 2 * kPi);
  // Move: along the look, sideways, and A / B straight up and down; eased over about a third of a second.
  const V3 look = Look(g_f.yaw, g_f.pitch);
  const V3 back = look * -1.f;
  const V3 right = Norm(Cross(V3{0, 1, 0}, back) * g_f.handed);
  const V3 up = Norm(Cross(back, right) * g_f.handed);
  const float rise = (buttons & 0x1000 ? 1.f : 0.f) - (buttons & 0x2000 ? 1.f : 0.f);
  const float speed = g_f.base_speed * (g_f.fast ? 4.f : 1.f) * (1.f + 4.f * lt) * slow;
  const V3 wish = (look * ly + right * lx + V3{0, 1, 0} * rise) * speed;
  const float move_ease = 1.f - std::exp(-dt * 4.f);
  g_f.velocity = g_f.velocity + (wish - g_f.velocity) * move_ease;
  g_f.place = g_f.place + g_f.velocity * dt;
  if (!camera) return;
  StoreRow(base, camera + 0x10, right, 0);
  StoreRow(base, camera + 0x20, up, 0);
  StoreRow(base, camera + 0x30, back, 0);
  StoreRow(base, camera + 0x40, g_f.place, 1);
  static std::chrono::steady_clock::time_point logged{};
  if (now - logged > std::chrono::seconds(5)) {
    logged = now;
    REXLOG_INFO("BF free camera: at {:.1f} {:.1f} {:.1f} looking {:.2f} {:.2f} {:.2f}, {:.1f} m/s", g_f.place.x, g_f.place.y,
                g_f.place.z, look.x, look.y, look.z, std::sqrt(Dot(g_f.velocity, g_f.velocity)));
  }
}

}  // namespace bf1943::freecam

// ---------------------------------------------------------------------------------------------------------------
// The hidden UI: made after every other overlay of ours (bf1943_app.h), so it is drawn last in each ImGui frame, and
// there empties what they drew. Not while our menus, the chat box or the F9 panel are open: those are for using.

namespace {
class UiHider final : public rex::ui::ImGuiDialog {
 public:
  explicit UiHider(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO&) override {
    if (!g_hide_ui.load() || bf1943::GameMenuOpen() || bf1943::world::AdminPanelOpen() || bf1943::comms::Typing()) return;
    ImGui::GetForegroundDrawList()->_ResetForNewFrame();
    ImGui::GetBackgroundDrawList()->_ResetForNewFrame();
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) w->Hidden = true;
  }
};
UiHider* g_hider = nullptr;
}  // namespace

namespace bf1943 {
void CreateUiHider(rex::ui::ImGuiDrawer* drawer) {
  if (!g_hider) g_hider = new UiHider(drawer);  // lives for the session
}
}  // namespace bf1943
