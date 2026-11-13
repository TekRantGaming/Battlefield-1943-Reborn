// Field of view (Settings > Display > Camera > Field of view, bf_fov): the camera's horizontal field of view at 16:9.
//
// The game's camera manager (the singleton at 0x82AF3BA0) keeps the current vertical field of view in degrees and
// blends it toward each request every frame (a camera change, or a weapon's zoom: aim down sights narrows it); then
// sub_82294A58 turns it into radians and hands it to the view's setFovY, sub_82583248(view, f1 = radians), called from
// 0x82294CAC, which writes the view's +0x0C and +0x10 for the projection. A second view (sub_82623DB8, called from
// 0x822948E4 in sub_82294818) takes the same value. The game's default is 55 degrees vertical, 85.6 horizontal at
// 16:9 (the voted projection, NXR D132: 1/tan(v/2) = 1.9210). Static study: side-session note fov_study.md.
//
// The value handed over is mapped in tangent space, out = 2 atan(k tan(in / 2)) with k = tan(chosen / 2) /
// tan(27.5 degrees): unzoomed it becomes exactly the chosen field of view, and a zoom keeps its own ratio, so aiming
// down sights still narrows as much. The game's own state is never written, so nothing compounds, and the server and
// other players are not affected (the camera manager is the client's). The first person weapon has a projection of its
// own (RenderFov, 30 degrees), which this leaves alone.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <string>
#include <fmt/format.h>
#include <rex/logging.h>
#include <set>
#include <vector>
#include <mutex>
#include <map>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <native_x_renderer/native_x_renderer.h>

#include "comms.h"
#include "scope_pip.h"
#include "seat_info.h"

REXCVAR_DEFINE_BOOL(bf_scope_pip, false, "BF1943",
                    "Sniper scope (Settings > Enhancements, experimental): picture in picture, only the scope's lens "
                    "zooms in and the world around it stays in view (NativeXRenderer D168)");
REXCVAR_DEFINE_DOUBLE(bf_fov, 0.0, "BF1943",
                      "The camera's horizontal field of view at 16:9 in degrees (60 to 120); 0: the game's own, 85.6");
// Vehicles' own slider (TekRantGaming, 12 Nov 2026): the camera while the local soldier sits in a vehicle.
REXCVAR_DEFINE_DOUBLE(bf_fov_vehicle, 0.0, "BF1943",
                      "The camera's horizontal field of view in vehicles, at 16:9 in degrees (60 to 120); 0: the game's own");

namespace bf1943 {
bool LocalSeatedNow();  // hud_glyphs.cpp: the local soldier sits in a vehicle (from the pad hook)
// The game's own vertical field of view this frame (radians, before our mapping), zoom included: look.cpp scales the
// mouse by how far a zoom narrows it.
std::atomic<float> g_game_fov{0.0f};
// The camera manager's field of view controller this frame (degrees): +0x30 the view's now (a sight's zoom and the
// sprint's widening included), +0x28 the camera's own (its request C). Read in the frame update that sets the view
// (sub_82294A58(mgr), whose r31 holds the manager when it calls setFovY; the controller is *(mgr + 0xE0)): look.cpp's
// zoom is the view narrower than the camera's own.
std::atomic<float> g_fov_now_deg{0.0f}, g_fov_own_deg{0.0f};
// The game's last answer to "the first person weapon's field of view" (sub_821FAAF0: -1 when the view is not a first
// person one) and when: 1 first person, 0 not, -1 not asked within a quarter second.
std::atomic<int> g_first_person{0};
std::atomic<int64_t> g_first_person_at{0};
int FirstPersonView() {
  const int64_t at = g_first_person_at.load(std::memory_order_relaxed);
  const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
  if (!at || now - at > std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(250)).count())
    return -1;
  return g_first_person.load(std::memory_order_relaxed);
}
}  // namespace bf1943

namespace {
uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
float LoadF(const uint8_t* base, uint32_t a) {
  const uint32_t u = Load32(base, a);
  float f;
  std::memcpy(&f, &u, sizeof(f));
  return f;
}
constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kCameraFovCaller = 0x82294CB0;  // sub_82294A58's call of setFovY
constexpr uint32_t kSecondViewCaller = 0x822948E4;  // sub_82294818's call of sub_82623DB8 (at 0x822948E0)

// The slider that applies now: the vehicle's while the soldier sits in one, else the camera's.
double Chosen() { return bf1943::LocalSeatedNow() ? REXCVAR_GET(bf_fov_vehicle) : REXCVAR_GET(bf_fov); }

// `weight`: how much of the mapping applies (1 all of it, 0 none: the value as the game has it).
double Mapped(double radians, double weight = 1.0) {
  const double chosen = Chosen();
  if (chosen <= 0.0 || !(radians > 0.0 && radians < kPi)) return radians;
  const double h = std::clamp(chosen, 60.0, 120.0) * kPi / 180.0;
  const double tan_v = std::tan(h * 0.5) * 9.0 / 16.0;  // the vertical half angle's tangent at 16:9
  const double k = 1.0 + (tan_v / std::tan(27.5 * kPi / 180.0) - 1.0) * std::clamp(weight, 0.0, 1.0);
  return 2.0 * std::atan(k * std::tan(radians * 0.5));
}
// The same mapping for a value in degrees (the first person weapon's own projection).
double MappedDegrees(double degrees, double weight) { return Mapped(degrees * kPi / 180.0, weight) * 180.0 / kPi; }
// How much of the mapping the first person weapon's projection takes: all of it unzoomed, none through a scope. A
// scope's black mask is part of the weapon's picture, and widened with the camera it shrank on the screen (13 Nov
// 2026: "player FOV should not affect the sniper's scope mesh. Right now at high FOV, it shrinks the scope mesh").
// The zoom is the view's field of view now against the camera's own, in tangents (1 unzoomed): the mapping fades out
// from 0.9 (the start of aiming) to 0.5 (a scope is well under that), so iron sights keep part of it.
double WeaponMapWeight() {
  const double now = bf1943::g_fov_now_deg.load(std::memory_order_relaxed);
  const double own = bf1943::g_fov_own_deg.load(std::memory_order_relaxed);
  if (!(now > 1.0 && own > 1.0 && now < 179.0 && own < 179.0)) return 1.0;
  const double zoom = std::tan(now * kPi / 360.0) / std::tan(own * kPi / 360.0);
  return std::clamp((zoom - 0.5) / (0.9 - 0.5), 0.0, 1.0);
}
}  // namespace

REX_EXTERN(__imp__sub_82583248);
#if defined(BF_DEV_TOOLS)
namespace bf1943 {
extern std::atomic<uint32_t> g_dev_view;  // dev_matscan.cpp: the rendered view, for BF_DEV_VIEWLOG
extern std::atomic<uint32_t> g_dev_view_calls;
void DevViewSample(uint8_t* base, uint32_t view);
}
#endif
REX_HOOK_RAW(sub_82583248) {
#if defined(BF_DEV_TOOLS)
  if (uint32_t(ctx.lr) == kCameraFovCaller) {
    bf1943::g_dev_view.store(ctx.r3.u32, std::memory_order_relaxed);
    bf1943::g_dev_view_calls.fetch_add(1, std::memory_order_relaxed);
    bf1943::DevViewSample(base, ctx.r3.u32);
  }
#endif
  if (uint32_t(ctx.lr) == kCameraFovCaller) {
    // The rendered camera (its world matrix at +0x40: right, up, back, position rows): voice chat's listener and the
    // position a voice comes from (comms.cpp), on foot, in a vehicle or watching after a death alike.
    float right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, back[3] = {0, 0, 1};
    if (const uint32_t view = ctx.r3.u32) {
      for (int i = 0; i < 3; ++i)
        right[i] = LoadF(base, view + 0x40 + 4 * i), up[i] = LoadF(base, view + 0x50 + 4 * i),
        back[i] = LoadF(base, view + 0x60 + 4 * i);
      const float pos[3] = {LoadF(base, view + 0x70), LoadF(base, view + 0x74), LoadF(base, view + 0x78)};
      bf1943::comms::NoteCamera(pos, right, back);
    }
    bf1943::g_game_fov.store(float(ctx.f1.f64), std::memory_order_relaxed);
    const uint32_t manager = ctx.r31.u32;
    const uint32_t controller = manager ? Load32(base, manager + 0xE0) : 0;
    if (controller) {
      bf1943::g_fov_now_deg.store(LoadF(base, controller + 0x30), std::memory_order_relaxed);
      bf1943::g_fov_own_deg.store(LoadF(base, controller + 0x28), std::memory_order_relaxed);
    }
    // The picture in picture scope (Enhancements, NativeXRenderer D168): how far the view is zoomed (the half angles'
    // tangents, now over the camera's own: a sight's about 0.6 to 0.8, a sniper scope's much less; the renderer also
    // waits for the game's scope mask). The world around the lens is drawn with the camera's own field of view as
    // rendered (bf_fov's mapping included).
    {
      constexpr double kPi = 3.14159265358979323846;
      const double now_deg = bf1943::g_fov_now_deg.load(std::memory_order_relaxed);
      const double own_deg = bf1943::g_fov_own_deg.load(std::memory_order_relaxed);
      const bool valid = now_deg > 1.0 && now_deg < 179.0 && own_deg > 1.0 && own_deg < 179.0;
      const double zoom = valid ? std::tan(now_deg * kPi / 360.0) / std::tan(own_deg * kPi / 360.0) : 1.0;
      const double own_rendered = valid ? Mapped(own_deg * kPi / 180.0) : 0.9;
      // A step of the zoom logged (a tenth of the way), while the scope is on: the research of a sight's and a scope's.
      static int logged_step = 10;
      const int step = int(std::lround(std::clamp(zoom, 0.0, 1.0) * 10));
      // Only in a build with the scope (scope_pip.h: not in 0.4.1), with the switch on.
      const bool pip = kScopePip && REXCVAR_GET(bf_scope_pip);
      if (step != logged_step && pip) {
        logged_step = step;
        REXLOG_INFO("BF scope: zoom {:.2f} (field of view {:.1f} of the camera's {:.1f} degrees)", zoom, now_deg, own_deg);
      }
      // On foot only (the soldier's seat, from the game's own input mapping): the game's composite, which the renderer
      // follows, is the same with and without a scope (run v41scope26), so a vehicle's or the air raid bomber's zoomed
      // view must never count as a scope's.
      bf1943::SeatInfo seat;
      const bool on_foot = pip && bf1943::LocalSeat(seat) && seat.kind == bf1943::SeatKind::kSoldier;
      native_x_renderer::NoteScopeView(on_foot, right, up, back,
                                       float(std::tan(std::clamp(own_rendered, 0.1, 3.0) * 0.5)), float(zoom));
    }
    ctx.f1.f64 = Mapped(ctx.f1.f64);
  }
  __imp__sub_82583248(ctx, base);
}

// Ultrawide (0.4.0, NativeXRenderer D43: Profile::widens_cameras): a 16:9 view's aspect ratio widened by
// AspectFactor() (the screen's shape over 16:9), so the 3D view fills the screen with more on each side (Hor+): the
// vertical field of view, bf_fov's mapping and a zoom's ratio stay as they were. Only views of the game's 16:9 (its main
// and second camera, aspect 1280/720 from sub_823CD748); square views (cube and shadow maps, 1.0) and other shapes are
// left alone. 1 on a 16:9 screen or with native_x_renderer_fill_screen off.
namespace {
double Widened(double aspect) {
  const double factor = native_x_renderer::AspectFactor();
  if (factor == 1.0 || !(std::fabs(aspect - 16.0 / 9.0) < 0.02)) return aspect;
  return aspect * factor;
}
}  // namespace

// setAspect, sub_825832B8(view, f1 = aspect): writes the view's +0x20 and its horizontal estimate +0x10 (+0x0C times
// the aspect), so the game's culling sees the wider view too.
REX_EXTERN(__imp__sub_825832B8);
REX_HOOK_RAW(sub_825832B8) {
  ctx.f1.f64 = Widened(ctx.f1.f64);
  __imp__sub_825832B8(ctx, base);
}

// The perspective projection, sub_82623DB8(out, f1 = vertical field of view, f2 = aspect, f3 near, f4 far): a view whose
// aspect was set before the renderer knew the screen (still 16:9) is widened here as well.
REX_EXTERN(__imp__sub_82623DB8);
REX_HOOK_RAW(sub_82623DB8) {
  if (uint32_t(ctx.lr) == kSecondViewCaller) ctx.f1.f64 = Mapped(ctx.f1.f64);
  ctx.f2.f64 = Widened(ctx.f2.f64);
  __imp__sub_82623DB8(ctx, base);
}

// HUD world markers on an ultrawide screen (0.4.0): the game projects capture flags, name tags and objective icons
// to its 1280x720 HUD in a few functions (research 12 Nov 2026, side note): sub_8241B770(r4 position, r5 world
// matrix, r6 view-projection, r7 out float2 pixels; r3 = in front), sub_8241D038 (the same with a margin test, out in
// r7) and sub_8241B598(r3 out, r5 position, r6 view, r7 projection). Each point is noted for NativeXRenderer, which then
// places the HUD draw on it where the widened 3D view shows that point (HudMode::kPoint), not in the 16:9 box.
namespace {
void NoteMarker(const uint8_t* base, uint32_t out) {
  if (!out || native_x_renderer::AspectFactor() == 1.0) return;
  native_x_renderer::NoteHudMarker(LoadF(base, out), LoadF(base, out + 4));
}
}  // namespace

REX_EXTERN(__imp__sub_8241B770);
REX_HOOK_RAW(sub_8241B770) {
  const uint32_t out = ctx.r7.u32;
  __imp__sub_8241B770(ctx, base);
  NoteMarker(base, out);
}

REX_EXTERN(__imp__sub_8241D038);
REX_HOOK_RAW(sub_8241D038) {
  const uint32_t out = ctx.r7.u32;
  __imp__sub_8241D038(ctx, base);
  NoteMarker(base, out);
}

REX_EXTERN(__imp__sub_8241B598);
REX_HOOK_RAW(sub_8241B598) {
  const uint32_t out = ctx.r3.u32;
  __imp__sub_8241B598(ctx, base);
  NoteMarker(base, out);
}

// The first person weapon's own projection (TekRantGaming, 12 Nov 2026: "the FOV slider doesn't take into
// consideration the first-person weapon ... those stay the same"): sub_821FAAF0(soldier) returns the weapon's render
// field of view in degrees (SoldierWeaponData.RenderFov, 30, blended toward ZoomRenderFov while aiming; the study's
// fov_study.md), or -1 when not in first person; its consumers write it into the player's render record and the
// vehicle seat's view. Mapped the same way as the world's camera after the original, so the hands and the weapon
// widen with it and keep their place in the picture; not through a scope (WeaponMapWeight).
REX_EXTERN(__imp__sub_821FAAF0);
REX_EXTERN(sub_821EE7F0);  // (soldier, 0): the weapon in its hands
REX_EXTERN(sub_825835B0);  // (weapon): its aiming state
REX_EXTERN(sub_821F9318);  // (that): how far the zoom is in, 0 to 1
// 0.4.2 ("a toggle-able option ... for SMG ADS being either Zoom In or Iron Sight ... use the SMG model's own sights
// when zooming like the Rifles and Pistols do"): the game's zoom with an SMG only pulls the gun in (its own zoom
// animation). With bf_smg_sights the first person weapon's picture is moved in the view while the zoom is in, by the
// zoom's own blend, so the gun's sights come to the middle of the view: a move in the view's space the renderer
// applies to the weapon's draws (NativeXRenderer SetWeaponLayerMove). The view only: the aim, the spread and the
// server see nothing of it. The move is in metres and degrees, x right, y up, z forward.
REXCVAR_DEFINE_BOOL(bf_smg_sights, false, "BF1943", "SMG zoom: the gun's own sights brought to the eye (Iron Sight) instead of the game's zoom in");
// Each SMG's own move: "x,y,z,yaw,pitch,roll" (metres right, up, forward; degrees). The player lines them up with
// bf_smg_sight_tune ("If you have a way to give me either slider or transpose control, I could line it up manually
// faster"): while aiming with an SMG, the arrow keys move the gun left, right, up and down, Page Up and Page Down
// forward and back, the number pad's 4 6 turn it left and right, 8 2 up and down, 7 9 roll it, and 0 puts the built in
// values back; Shift makes the steps five times larger, Ctrl five times smaller. The values show on the screen
// (game_menu.cpp draws SmgSightTuneText) and are saved for that gun.
REXCVAR_DEFINE_STRING(bf_smg_sight_us, "-0.0725,0.0135,0.0,0,0,0", "BF1943", "Iron Sight: the Thompson's move, x,y,z,yaw,pitch,roll");
REXCVAR_DEFINE_STRING(bf_smg_sight_jp, "-0.0725,0.0135,0.0,0,0,0", "BF1943", "Iron Sight: the Type 100's move, x,y,z,yaw,pitch,roll");
REXCVAR_DEFINE_BOOL(bf_smg_sights_no_crosshair, true, "BF1943",
                    "Iron Sight: no crosshair while aiming down an SMG's sights, as with the rifles and pistols");
REXCVAR_DEFINE_BOOL(bf_smg_sight_tune, false, "BF1943", "Iron Sight: line the SMG's sights up with the keyboard while aiming (arrows, Page Up/Down, number pad)");
namespace {
std::mutex g_tune_mutex;
std::string g_tune_text;          // what the screen shows while tuning (empty: nothing)
std::string g_tune_save_name, g_tune_save_value;  // a changed setting to save
std::chrono::steady_clock::time_point g_tune_text_at{};
}  // namespace
namespace {
// The SMG in the hands now, for the pause menu's sliders: 0 none, 1 the Thompson, 2 the Type 100; and when it was seen.
std::atomic<int> g_tune_gun{0};
std::atomic<int64_t> g_tune_gun_at{0};
}  // namespace
namespace bf1943 {
// The pause menu's sight sliders ("If you can give me overlay sliders with pitch, yaw, roll and zoom, I can match it
// by hand"): while they are open the aim is held for the player (auto_join.cpp presses the left trigger).
std::atomic<bool> g_smg_tune_hold{false};
// The SMG held now ("Thompson", "Type 100"; empty: none in the last half second) and its six values.
std::string SmgSightTuneGun(double v[6]) {
  const int gun = g_tune_gun.load(std::memory_order_relaxed);
  const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
  if (!gun || now - g_tune_gun_at.load(std::memory_order_relaxed) > std::chrono::steady_clock::duration(std::chrono::milliseconds(500)).count())
    return "";
  const std::string text = gun == 2 ? REXCVAR_GET(bf_smg_sight_jp) : REXCVAR_GET(bf_smg_sight_us);
  for (int i = 0; i < 6; ++i) v[i] = 0;
  std::sscanf(text.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
  return gun == 2 ? "Type 100" : "Thompson";
}
// Its values set (the sliders); `reset`: the built in ones.
void SmgSightTuneSet(const double v[6], bool reset) {
  const int gun = g_tune_gun.load(std::memory_order_relaxed);
  if (!gun) return;
  const char* name = gun == 2 ? "bf_smg_sight_jp" : "bf_smg_sight_us";
  std::string value = fmt::format("{:.4f},{:.4f},{:.4f},{:.2f},{:.2f},{:.2f}", v[0], v[1], v[2], v[3], v[4], v[5]);
  if (reset)
    if (const auto* info = rex::cvar::GetFlagInfo(name)) value = info->default_value;
  rex::cvar::SetFlagByName(name, value);
  std::lock_guard lock(g_tune_mutex);
  g_tune_save_name = name, g_tune_save_value = value;
}
// game_menu.cpp, each frame: the tuning's line for the screen, and a changed setting to save (true once).
std::string SmgSightTuneText() {
  std::lock_guard lock(g_tune_mutex);
  if (std::chrono::steady_clock::now() - g_tune_text_at > std::chrono::milliseconds(300)) return "";
  return g_tune_text;
}
bool SmgSightTakeSave(std::string& name, std::string& value) {
  std::lock_guard lock(g_tune_mutex);
  if (g_tune_save_name.empty()) return false;
  name = std::move(g_tune_save_name), value = std::move(g_tune_save_value);
  g_tune_save_name.clear();
  return true;
}
}  // namespace bf1943
namespace {
// The held weapon's name (SoldierWeaponData: its name's first and last characters at +0x0C and +0x10).
std::string WeaponName(const uint8_t* base, uint32_t data) {
  auto word = [&](uint32_t at) { return uint32_t(base[at]) << 24 | base[at + 1] << 16 | base[at + 2] << 8 | base[at + 3]; };
  const uint32_t a = word(data + 0x0C), b = word(data + 0x10);
  if (!a || b <= a || b - a > 48) return "";
  return std::string(reinterpret_cast<const char*>(base + a), b - a);
}
// The zoom's blur ("Need to remove the zoom blur when using Iron sight"): a zoom level's ForegroundBlurFilter (its data
// + 0x30: 0 none, 2 a 5 pixel Gaussian over what is near, the gun) is none while the SMG's sights are in use, and the
// game's own again when the setting is off. The levels: the weapon data's aiming data (+0xE4), its array at +0x58,
// its count at +0x50.
std::mutex g_blur_mutex;
std::map<uint32_t, uint32_t> g_blur_own;  // a level's filter word's address -> the game's own value
void ZoomBlur(uint8_t* base, uint32_t data, bool off) {
  auto word = [&](uint32_t at) { return uint32_t(base[at]) << 24 | base[at + 1] << 16 | base[at + 2] << 8 | base[at + 3]; };
  auto store = [&](uint32_t at, uint32_t v) {
    base[at] = uint8_t(v >> 24), base[at + 1] = uint8_t(v >> 16), base[at + 2] = uint8_t(v >> 8), base[at + 3] = uint8_t(v);
  };
  std::lock_guard lock(g_blur_mutex);
  if (!off) {
    for (const auto& [at, own] : g_blur_own) store(at, own);
    g_blur_own.clear();
    return;
  }
  const uint32_t aiming = data ? word(data + 0xE4) : 0;
  if (aiming < 0x10000000 || aiming > 0xF0000000) return;
  const uint32_t count = word(aiming + 0x50), array = word(aiming + 0x58);
  if (count > 4 || array < 0x10000000 || array > 0xF0000000) return;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t level = word(array + i * 4);
    if (level < 0x10000000 || level > 0xF0000000) continue;
    const uint32_t filter = word(level + 0x30);
    if (filter == 0 || filter > 8) continue;  // none already, or not a filter
    g_blur_own.emplace(level + 0x30, filter);
    store(level + 0x30, 0);
    REXLOG_INFO("BF SMG sights: zoom level {}'s foreground blur (filter {}) off while the sights are used", i, filter);
  }
}
void SmgSights(PPCContext& ctx, uint8_t* base, uint32_t soldier, double fov_degrees) {
  if (!REXCVAR_GET(bf_smg_sights)) {
    if (!g_blur_own.empty()) ZoomBlur(base, 0, false);
    return;
  }
  if (!soldier) return;
  PPCContext c = ctx;
  c.r3.u64 = soldier, c.r4.s64 = 0;
  sub_821EE7F0(c, base);
  const uint32_t weapon = c.r3.u32;
  if (!weapon) return;
  const uint32_t data = uint32_t(base[weapon + 4]) << 24 | base[weapon + 5] << 16 | base[weapon + 6] << 8 | base[weapon + 7];
  if (!data) return;
  sub_825835B0(c, base);
  sub_821F9318(c, base);
  const double blend = std::clamp(c.f1.f64, 0.0, 1.0);
  static uint32_t last_data = 0;
  static bool smg = false;
  if (data != last_data) {
    last_data = data;
    std::string name = WeaponName(base, data);
    std::string lower = name;
    for (char& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    smg = lower.find("thompson") != std::string::npos || lower.find("100") != std::string::npos;
    REXLOG_INFO("BF SMG sights: holding {} ({})", name.empty() ? "a weapon without a name" : name, smg ? "an SMG" : "not an SMG");
  }
  if (smg && g_blur_own.empty()) ZoomBlur(base, data, true);
  {
    static double said = -1;
    if (std::abs(blend - said) > 0.4) {
      said = blend;
      REXLOG_INFO("BF SMG sights: zoom blend {:.2f} ({})", blend, smg ? "moving the gun" : "not an SMG");
    }
  }
  if (!smg) return;
  static bool japanese = false;
  static uint32_t pose_data = 0;
  if (pose_data != data) {
    pose_data = data;
    std::string lower = WeaponName(base, data);
    for (char& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    japanese = lower.find("thompson") == std::string::npos;
  }
  const char* cvar_name = japanese ? "bf_smg_sight_jp" : "bf_smg_sight_us";
  g_tune_gun.store(japanese ? 2 : 1, std::memory_order_relaxed);
  g_tune_gun_at.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
  double v[6] = {0, 0, 0, 0, 0, 0};
  {
    const std::string text = japanese ? REXCVAR_GET(bf_smg_sight_jp) : REXCVAR_GET(bf_smg_sight_us);
    std::sscanf(text.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
  }
#if defined(_WIN32)
  // Only while this game's window is the one in front (the keys are read from the keyboard itself).
  DWORD front_pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &front_pid);
#if defined(BF_INTERNAL_TOOLS)  // an internal build's only ("The sliders should be part of our internal build only")
  const bool tune = REXCVAR_GET(bf_smg_sight_tune);
#else
  const bool tune = false;
#endif
  if (tune && blend > 0.5 && front_pid == GetCurrentProcessId()) {
    // One step at a press, then repeated while the key is held (from 0.35 s, every 40 ms).
    static std::map<int, std::chrono::steady_clock::time_point> next;
    const auto now = std::chrono::steady_clock::now();
    auto pressed = [&](int vk) {
      if (!(GetAsyncKeyState(vk) & 0x8000)) {
        next.erase(vk);
        return false;
      }
      auto it = next.find(vk);
      if (it == next.end()) {
        next[vk] = now + std::chrono::milliseconds(350);
        return true;
      }
      if (now < it->second) return false;
      it->second = now + std::chrono::milliseconds(40);
      return true;
    };
    const double scale = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 5.0 : (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 0.2 : 1.0;
    const double step = 0.001 * scale, turn = 0.25 * scale;
    bool changed = false;
    struct Key {
      int vk, index;
      double by;
    };
    const Key keys[] = {{VK_LEFT, 0, -step},   {VK_RIGHT, 0, step},   {VK_UP, 1, step},      {VK_DOWN, 1, -step},
                        {VK_PRIOR, 2, step},   {VK_NEXT, 2, -step},   {VK_NUMPAD4, 3, -turn}, {VK_NUMPAD6, 3, turn},
                        {VK_NUMPAD8, 4, turn}, {VK_NUMPAD2, 4, -turn}, {VK_NUMPAD7, 5, -turn}, {VK_NUMPAD9, 5, turn}};
    for (const Key& key : keys)
      if (pressed(key.vk)) v[key.index] += key.by, changed = true;
    if (pressed(VK_NUMPAD0)) {
      if (const auto* info = rex::cvar::GetFlagInfo(cvar_name))
        std::sscanf(info->default_value.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
      changed = true;
    }
    const std::string value = fmt::format("{:.4f},{:.4f},{:.4f},{:.2f},{:.2f},{:.2f}", v[0], v[1], v[2], v[3], v[4], v[5]);
    if (changed) rex::cvar::SetFlagByName(cvar_name, value);
    std::lock_guard lock(g_tune_mutex);
    g_tune_text = fmt::format("SIGHT TUNING  {}   right {:+.4f}  up {:+.4f}  forward {:+.4f} m   turn {:+.2f}  tilt {:+.2f}  roll {:+.2f} deg\n"
                              "Arrows: left/right/up/down   PgUp/PgDn: forward/back   Numpad 4 6: turn   8 2: tilt   7 9: roll   0: reset   Shift x5, Ctrl /5",
                              japanese ? "Type 100" : "Thompson", v[0], v[1], v[2], v[3], v[4], v[5]);
    g_tune_text_at = now;
    if (changed) g_tune_save_name = cvar_name, g_tune_save_value = value;
  }
#endif
  if (blend <= 0.001) return;
  // "remove crosshair when iron sight is being used when zoomed-in, just like the pistols and rifles natively do"
  if (REXCVAR_GET(bf_smg_sights_no_crosshair) && blend > 0.3) native_x_renderer::HideCrosshair();
  const double k = blend * blend * (3.0 - 2.0 * blend);  // eased
  const double deg = 3.14159265358979 / 180.0;
  const double yaw = v[3] * deg * k, pitch = v[4] * deg * k, roll = v[5] * deg * k;
  const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch), cr = std::cos(roll), sr = std::sin(roll);
  // roll (about z) x pitch (about x) x yaw (about y), then the shift
  const double ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
  const double rx[9] = {1, 0, 0, 0, cp, -sp, 0, sp, cp};
  const double rz[9] = {cr, -sr, 0, sr, cr, 0, 0, 0, 1};
  auto mul3 = [](const double* l, const double* r, double* o) {
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) o[i * 3 + j] = l[i * 3] * r[j] + l[i * 3 + 1] * r[3 + j] + l[i * 3 + 2] * r[6 + j];
  };
  double t[9], rot[9];
  mul3(rx, ry, t), mul3(rz, t, rot);
  const float move[16] = {float(rot[0]), float(rot[1]), float(rot[2]), float(v[0] * k),
                          float(rot[3]), float(rot[4]), float(rot[5]), float(v[1] * k),
                          float(rot[6]), float(rot[7]), float(rot[8]), float(v[2] * k),
                          0, 0, 0, 1};
  native_x_renderer::SetWeaponLayerMove(move, float(fov_degrees));
}
}  // namespace
REX_HOOK_RAW(sub_821FAAF0) {
  const uint32_t soldier = ctx.r3.u32;
  __imp__sub_821FAAF0(ctx, base);
  const bool first_person_weapon = ctx.f1.f64 > 0.0 && ctx.f1.f64 < 180.0;
#if defined(BF_DEV_TOOLS)
  // BF_DEV_WEAPON=1 (the SMG's iron sights, 0.4.2): the held weapon's data (SoldierWeaponData, *(weapon + 4)) logged
  // once for each weapon, as words and floats; BF_DEV_WEAPON_POKE=<byte offset>:<add> adds to one float of it while
  // the zoom is in (times the zoom's blend), to find the fields that place the first person mesh.
  if (ctx.f1.f64 > 0.0 && soldier) {
    static const bool on = std::getenv("BF_DEV_WEAPON") != nullptr;
    if (on) {
      const double answer = ctx.f1.f64;
      PPCContext c = ctx;
      c.r3.u64 = soldier, c.r4.s64 = 0;
      sub_821EE7F0(c, base);
      const uint32_t weapon = c.r3.u32;
      const uint32_t data = weapon ? uint32_t(base[weapon + 4]) << 24 | base[weapon + 5] << 16 | base[weapon + 6] << 8 | base[weapon + 7] : 0;
      double blend = 0;
      if (weapon) {
        sub_825835B0(c, base);
        sub_821F9318(c, base);
        blend = c.f1.f64;
      }
      auto word = [&](uint32_t at) { return uint32_t(base[at]) << 24 | base[at + 1] << 16 | base[at + 2] << 8 | base[at + 3]; };
      static std::mutex m;
      static std::set<uint32_t> seen;
      static std::map<uint32_t, std::pair<uint32_t, uint32_t>> poked;  // address -> its own bits
      std::lock_guard lock(m);
      if (data && seen.insert(data).second) {
        std::string line;
        for (uint32_t o = 0; o < 0x180; o += 4) {
          const uint32_t w = word(data + o);
          float fl;
          std::memcpy(&fl, &w, 4);
          line += fmt::format(" +{:03X}:{:08X}", o, w);
          if (std::abs(fl) > 1e-4f && std::abs(fl) < 1e4f) line += fmt::format("({:.4g})", fl);
        }
        REXLOG_INFO("BF dev weapon: weapon {:08X} data {:08X}:{}", weapon, data, line);
        // What its pointers lead to (the zoom levels among them): each block of up to 0x120 bytes.
        for (const uint32_t o : {0x38u, 0x64u, 0x68u, 0xCCu, 0xDCu, 0xE0u, 0xE4u, 0xECu, 0xF8u, 0xFCu}) {
          const uint32_t at = word(data + o);
          if (at < 0x10000000 || at > 0xF0000000) continue;
          std::string block;
          for (uint32_t k = 0; k < 0x120; k += 4) {
            const uint32_t w = word(at + k);
            float fl;
            std::memcpy(&fl, &w, 4);
            block += fmt::format(" +{:03X}:{:08X}", k, w);
            if (std::abs(fl) > 1e-4f && std::abs(fl) < 1e4f) block += fmt::format("({:.4g})", fl);
          }
          REXLOG_INFO("BF dev weapon: data+{:03X} -> {:08X}:{}", o, at, block);
        }
        // The aiming data's zoom levels (data + 0xE4, its array at +0x58 with its count at +0x50): each level's data.
        if (const uint32_t aiming = word(data + 0xE4); aiming > 0x10000000 && aiming < 0xF0000000) {
          const uint32_t count = word(aiming + 0x50), array = word(aiming + 0x58);
          for (uint32_t i = 0; i < count && i < 4 && array > 0x10000000 && array < 0xF0000000; ++i) {
            const uint32_t level = word(array + i * 4);
            if (level < 0x10000000 || level > 0xF0000000) continue;
            std::string block;
            for (uint32_t k = 0; k < 0xA0; k += 4) {
              const uint32_t w = word(level + k);
              float fl;
              std::memcpy(&fl, &w, 4);
              block += fmt::format(" +{:03X}:{:08X}", k, w);
              if (std::abs(fl) > 1e-4f && std::abs(fl) < 1e4f) block += fmt::format("({:.4g})", fl);
            }
            REXLOG_INFO("BF dev weapon: zoom level {} at {:08X}:{}", i, level, block);
          }
        }
      }
      static double said = -1;
      if (std::abs(blend - said) > 0.25) { said = blend; REXLOG_INFO("BF dev weapon: data {:08X} zoom blend {:.2f}", data, blend); }
      // BF_DEV_WEAPON_POKE=<byte offset>:<add>,<byte offset>:<add>...: one after another, 8 s each from the first
      // zoom, each added to its float while the zoom is in; the others are as the game has them.
      if (const char* v = std::getenv("BF_DEV_WEAPON_POKE"); v && *v && data) {
        static std::vector<std::pair<uint32_t, float>> list;
        static std::chrono::steady_clock::time_point began{};
        static int active = -2;
        if (list.empty())
          for (const char* c = v; *c;) {
            const uint32_t off = uint32_t(std::strtoul(c, nullptr, 0));
            const char* colon = std::strchr(c, ':');
            list.push_back({off, colon ? float(std::atof(colon + 1)) : 0.0f});
            while (*c && *c != ',') ++c;
            if (*c) ++c;
          }
        if (blend > 0.5 && !began.time_since_epoch().count()) began = std::chrono::steady_clock::now();
        const int now_at = began.time_since_epoch().count()
                               ? int(std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count() / 8.0)
                               : -1;
        if (now_at != active) {
          active = now_at;
          if (active >= 0 && active < int(list.size()))
            REXLOG_INFO("BF dev weapon: poke {} of {}: +{:03X} by {}", active + 1, list.size(), list[size_t(active)].first, list[size_t(active)].second);
        }
        for (int i = 0; i < int(list.size()); ++i) {
          const uint32_t at = data + list[size_t(i)].first;
          if (!poked.count(at)) poked[at] = {word(at), 0};
          float own;
          std::memcpy(&own, &poked[at].first, 4);
          const float now = own + (i == active ? list[size_t(i)].second * float(blend) : 0.0f);
          uint32_t bits;
          std::memcpy(&bits, &now, 4);
          base[at] = uint8_t(bits >> 24), base[at + 1] = uint8_t(bits >> 16), base[at + 2] = uint8_t(bits >> 8), base[at + 3] = uint8_t(bits);
        }
      }
      ctx.f1.f64 = answer;
    }
  }
#endif
  // Its answer also says whether the view is a first person one (tick_blend.cpp's free look: a first person view
  // turns in place, an outside one goes round the vehicle).
  bf1943::g_first_person.store(ctx.f1.f64 > 0.0 ? 1 : 0, std::memory_order_relaxed);
  bf1943::g_first_person_at.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
  if (ctx.f1.f64 > 0.0 && ctx.f1.f64 < 180.0) ctx.f1.f64 = MappedDegrees(ctx.f1.f64, WeaponMapWeight());
  if (first_person_weapon) {  // with the field of view the weapon's view gets this frame
    const double answer = ctx.f1.f64;
    SmgSights(ctx, base, soldier, answer);
    ctx.f1.f64 = answer;
  }
}
