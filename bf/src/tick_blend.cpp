// Moving objects drawn on every frame (bf_smooth_motion; TekRantGaming's list of 9 Nov 2026, item 1: vehicles above 30 FPS).
//
// Measured (run bones1, a Sherman driving on Guadalcanal, BF_DEV_BONES): every skinned object (vehicles, soldiers) is
// placed each frame by its skeleton update sub_826D41B8(r3..r8, r9 = the root transform, r10) on the game's worker
// threads: each bone is its local position and rotation times its parent's transform (sub_826E2220), the first bones
// times the root. The root is the object's own transform, which the game writes only in its 30 Hz tick: in the
// Sherman, its root changed 3 to 4 times per 100 ms while its 120 bones were composed 6 to 7 times. So above 30 FPS the
// tank, its turret and its crew stepped 30 times a second.
//
// Here the skeleton update is handed, in place of the root, a copy moved to this frame: the root's last two tick
// transforms blended by the game's time since its last tick (look.cpp's rule: one tick behind, never ahead), rotation
// by quaternion, position and scale linearly. The game's own transforms are never written: the copy lives in our own
// guest memory, so the simulation, the server and the rest of the game see the game's values.
//
// Not blended: roots that change between ticks (written every frame by someone else, such as look.cpp's walking carry),
// a step longer than kMaxStep, a turn over kMaxTurn or a step far beyond the one before (a respawn, a teleport, a
// camera switch), and roots not seen on the tick before (just appeared).
//
// A skeleton is known by its bone array, not its root's address: the root comes in a copy whose place changes.
//
// The vehicle camera. The camera manager (sub_82294A58) built its camera from the vehicle's tick transform and handed it
// to the renderer; above 30 FPS its smoothing chased a target that moved 30 times a second, so it shook
// (vehicle_camera.cpp held it to the tick instead: no shake, but 30 Hz). Now, in a vehicle, it runs on every frame with
// all the time since the last tick frame on tick frames and none between (the camera the game makes at 30 FPS), and the
// transform it gives the main view (sub_82583348 from 0x82294C28) is blended between its last two tick values like the
// vehicle, so the two move together; the manager's own transforms hold the same camera for the rest of the frame.
// Runs mot1 to mot10 in a Sherman: the camera's steps from 0.97 spread and 41% stalled frames to 0.08 and 2%, its turn
// from 0.85 and 13% to 0.30 and 4%. Measured against a manager at frame rate (10 Nov, runs v*hyb and v*bi): outside views
// the same, the jeep's first person worse (its body against the view 5.2 degrees on tick frames, 0.37 so), the tank's
// vision slit the same (it is placed at the tick either way: NativeXRenderer D154 moves it onto the frame's camera).
//
// Behind, not ahead (bf_smooth_motion_predict, off since 0.1.28): the player's own vehicle, its camera and whoever
// rides it (their tick steps were the seat's, Score) can be carried on past the last tick by the time since it, so
// driving adds no delay (0.1.25 to 0.1.27, TekRantGaming's first choice); everything else stays one tick behind. Carried ahead,
// every uneven tick step overshoots and is taken back at the next tick: from a chase camera the jeep jumped forward and
// back by up to 0.56 m (215 mm at the 95th percentile on tick frames, run vjeepdust; TekRantGaming's "teleporting forward and
// back"); behind, 18 mm (vjeepextbi), and the tank 11.6 mm against 54.7 (vtankextbi, vtankext).

#if defined(BF_DEV_TOOLS) && defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // BF_DEV_ROOTFIND's memory search

#include <string>

#include <fmt/format.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include <native_x_renderer/native_x_renderer.h>

REXCVAR_DEFINE_BOOL(bf_smooth_motion, true, "BF1943",
                    "Vehicles, soldiers and everything animated move on every frame above 30 FPS (the game moves them "
                    "30 times a second)");
REXCVAR_DEFINE_BOOL(bf_smooth_motion_predict, false, "BF1943",
                    "With bf_smooth_motion, in a vehicle: the player's own vehicle and its camera carried ahead by the "
                    "time since the game's tick (no added delay) instead of blended one tick behind; whenever the "
                    "vehicle's steps change it overshoots and comes back (it jumps forward and back seen from outside)");
REXCVAR_DEFINE_BOOL(bf_smooth_bones, false, "BF1943",
                    "Experimental, with bf_smooth_motion: each bone's own movement blended between the game's ticks "
                    "too (does not smooth the first person weapon yet)");

namespace bf1943 {
bool InMatch();             // auto_join.cpp
uint64_t ClientTicks();     // vehicle_camera.cpp
bool VehicleCameraFrame();  // vehicle_camera.cpp: the camera manager is running for a vehicle
uint32_t LocalSoldierEntity();  // look.cpp
bool LocalOnFoot();             // look.cpp
double TickBlendPhase();        // below
double FramePhase();            // below
bool DevClientUpdate(PPCContext& ctx, uint8_t* base);  // dev_call_rate.cpp: true when it ran the update itself
extern std::atomic<bool> g_in_client_tick;  // vehicle_camera.cpp
}  // namespace bf1943

namespace {
using Clock = std::chrono::steady_clock;
constexpr double kTick = 1.0 / 30.0;
constexpr double kMaxStep = 25.0;  // metres in one tick (a plane at 150 m/s makes 5)
constexpr double kMaxTurn = 0.6;   // radians in one tick
constexpr uint32_t kSlots = 4096;  // root copies in our guest memory, used in turn (one a skeleton update)

// Frostbite's LinearTransform: rows right, up, forward, position; three big endian floats each and a fourth word that
// is kept as the game had it.
struct Xf {
  float m[4][3];
  uint32_t pad[4];
};

uint32_t Load32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Store32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
float BeF(const uint8_t* p) {
  const uint32_t u = Load32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
Xf LoadXf(const uint8_t* p) {
  Xf x;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 3; ++c) {
      const uint32_t u = Load32(p + r * 16 + c * 4);
      std::memcpy(&x.m[r][c], &u, 4);
    }
    x.pad[r] = Load32(p + r * 16 + 12);
  }
  return x;
}
void StoreXf(uint8_t* p, const Xf& x) {
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 3; ++c) {
      uint32_t u;
      std::memcpy(&u, &x.m[r][c], 4);
      Store32(p + r * 16 + c * 4, u);
    }
    Store32(p + r * 16 + 12, x.pad[r]);
  }
}
bool Same(const Xf& a, const Xf& b) { return !std::memcmp(a.m, b.m, sizeof(a.m)); }

// a then b (row vectors: a point through a, then through b; a child's world is its local then its parent's world).
// The fourth words are a's.
Xf Mul(const Xf& a, const Xf& b) {
  Xf o = a;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 3; ++c) {
      double v = r == 3 ? double(b.m[3][c]) : 0.0;
      for (int k = 0; k < 3; ++k) v += double(a.m[r][k]) * b.m[k][c];
      o.m[r][c] = float(v);
    }
  return o;
}
// a's inverse (its 3x3 part may scale); false when it has none.
bool Inverse(const Xf& a, Xf& o) {
  double m[3][3], inv[3][3];
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) m[r][c] = a.m[r][c];
  inv[0][0] = m[1][1] * m[2][2] - m[1][2] * m[2][1];
  inv[0][1] = m[0][2] * m[2][1] - m[0][1] * m[2][2];
  inv[0][2] = m[0][1] * m[1][2] - m[0][2] * m[1][1];
  inv[1][0] = m[1][2] * m[2][0] - m[1][0] * m[2][2];
  inv[1][1] = m[0][0] * m[2][2] - m[0][2] * m[2][0];
  inv[1][2] = m[0][2] * m[1][0] - m[0][0] * m[1][2];
  inv[2][0] = m[1][0] * m[2][1] - m[1][1] * m[2][0];
  inv[2][1] = m[0][1] * m[2][0] - m[0][0] * m[2][1];
  inv[2][2] = m[0][0] * m[1][1] - m[0][1] * m[1][0];
  const double det = m[0][0] * inv[0][0] + m[0][1] * inv[1][0] + m[0][2] * inv[2][0];
  if (!(std::abs(det) > 1e-12)) return false;
  o = a;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) o.m[r][c] = float(inv[r][c] / det);
  for (int c = 0; c < 3; ++c)
    o.m[3][c] = float(-(double(a.m[3][0]) * inv[0][c] + double(a.m[3][1]) * inv[1][c] + double(a.m[3][2]) * inv[2][c]) / det);
  return true;
}

struct Quat {
  double w, x, y, z;
};
// A rotation's quaternion from its three unit rows, and back (the same convention both ways).
Quat ToQuat(const double r[3][3]) {
  Quat q;
  const double tr = r[0][0] + r[1][1] + r[2][2];
  if (tr > 0) {
    const double s = std::sqrt(tr + 1.0) * 2;
    q = {0.25 * s, (r[1][2] - r[2][1]) / s, (r[2][0] - r[0][2]) / s, (r[0][1] - r[1][0]) / s};
  } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
    const double s = std::sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2;
    q = {(r[1][2] - r[2][1]) / s, 0.25 * s, (r[1][0] + r[0][1]) / s, (r[2][0] + r[0][2]) / s};
  } else if (r[1][1] > r[2][2]) {
    const double s = std::sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2;
    q = {(r[2][0] - r[0][2]) / s, (r[1][0] + r[0][1]) / s, 0.25 * s, (r[2][1] + r[1][2]) / s};
  } else {
    const double s = std::sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2;
    q = {(r[0][1] - r[1][0]) / s, (r[2][0] + r[0][2]) / s, (r[2][1] + r[1][2]) / s, 0.25 * s};
  }
  return q;
}
void FromQuat(const Quat& q, double r[3][3]) {
  const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  r[0][0] = 1 - 2 * (yy + zz), r[0][1] = 2 * (xy + wz), r[0][2] = 2 * (xz - wy);
  r[1][0] = 2 * (xy - wz), r[1][1] = 1 - 2 * (xx + zz), r[1][2] = 2 * (yz + wx);
  r[2][0] = 2 * (xz + wy), r[2][1] = 2 * (yz - wx), r[2][2] = 1 - 2 * (xx + yy);
}

// a's and b's rows split into lengths (scale) and a rotation.
struct Parts {
  double scale[3];
  bool mirrored;  // a left handed basis (a camera's forward row points back): the third row taken negated
  Quat q;
};
bool Split(const Xf& a, Parts& p) {
  double r[3][3];
  for (int i = 0; i < 3; ++i) {
    const double len = std::sqrt(double(a.m[i][0]) * a.m[i][0] + double(a.m[i][1]) * a.m[i][1] +
                                 double(a.m[i][2]) * a.m[i][2]);
    if (!(len > 1e-6 && len < 1e6)) return false;
    p.scale[i] = len;
    for (int c = 0; c < 3; ++c) r[i][c] = a.m[i][c] / len;
  }
  const double det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                     r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
  p.mirrored = det < 0;
  if (p.mirrored)
    for (int c = 0; c < 3; ++c) r[2][c] = -r[2][c];
  p.q = ToQuat(r);
  const double n = std::sqrt(p.q.w * p.q.w + p.q.x * p.q.x + p.q.y * p.q.y + p.q.z * p.q.z);
  if (!(n > 0.5 && n < 1.5)) return false;
  p.q = {p.q.w / n, p.q.x / n, p.q.y / n, p.q.z / n};
  double back[3][3];
  FromQuat(p.q, back);
  for (int i = 0; i < 3; ++i)
    for (int c = 0; c < 3; ++c)
      if (std::abs(back[i][c] - r[i][c]) > 2e-3) return false;  // not a rotation (a mirror or a skew)
  return true;
}

// a to b at t (0 a, 1 b, above 1 on past b): the position when `move`, the rotation and scale when `turn`, b's own
// otherwise. False when it is no blend: not rotations, or too big a step for one tick.
bool Blend(const Xf& a, const Xf& b, double t, Xf& out, bool move = true, bool turn = true) {
  out = b;
  if (turn) {
    Parts pa, pb;
    if (!Split(a, pa) || !Split(b, pb) || pa.mirrored != pb.mirrored) return false;
    double dot = pa.q.w * pb.q.w + pa.q.x * pb.q.x + pa.q.y * pb.q.y + pa.q.z * pb.q.z;
    if (dot < 0) pb.q = {-pb.q.w, -pb.q.x, -pb.q.y, -pb.q.z}, dot = -dot;
    if (2 * std::acos(std::min(1.0, dot)) > kMaxTurn) return false;
    Quat q{pa.q.w + (pb.q.w - pa.q.w) * t, pa.q.x + (pb.q.x - pa.q.x) * t, pa.q.y + (pb.q.y - pa.q.y) * t,
           pa.q.z + (pb.q.z - pa.q.z) * t};
    const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (!(n > 1e-6)) return false;
    q = {q.w / n, q.x / n, q.y / n, q.z / n};
    double r[3][3];
    FromQuat(q, r);
    for (int i = 0; i < 3; ++i) {
      const double s = (pa.scale[i] + (pb.scale[i] - pa.scale[i]) * t) * (i == 2 && pa.mirrored ? -1 : 1);
      for (int c = 0; c < 3; ++c) out.m[i][c] = float(r[i][c] * s);
    }
  }
  if (move) {
    const double dx = double(b.m[3][0]) - a.m[3][0], dy = double(b.m[3][1]) - a.m[3][1],
                 dz = double(b.m[3][2]) - a.m[3][2];
    if (dx * dx + dy * dy + dz * dz > kMaxStep * kMaxStep) return false;
    for (int c = 0; c < 3; ++c) out.m[3][c] = float(a.m[3][c] + (double(b.m[3][c]) - a.m[3][c]) * t);
  }
  return true;
}

// A root as the skeleton updates saw it (or the camera as the camera manager hands it over).
struct Track {
  Xf cur{}, prev{};
  uint64_t seen_tick = 0;   // the client tick count when cur was taken
  bool have = false;
  bool step = false;        // prev to cur is exactly one tick's change
  // Changed between ticks lately: that part is not the tick's (someone moves it every frame) and is left as it is.
  Clock::time_point moves_until{}, turns_until{};
  uint64_t frame = 0;       // the last frame it was used in
  double drift_move = 0, drift_turn = 0, tick_move = 0, tick_turn = 0;  // the last change between ticks, and the step
  bool drifted = false;  // the last Follow saw a change between ticks
  double step_move = 0, step_turn = 0;  // the last tick's step: its largest position and row change
  bool jump = false;     // the last tick's step was far beyond the one before (a teleport, a camera switch)
  // Ours (the local player's vehicle and whoever rides it): its last ticks' steps were the camera's. Scored once a tick.
  int own_score = 0;
  uint64_t scored_tick = 0;
};

// What of a track to blend this frame.
struct Plan {
  bool move = false, turn = false;
  bool any() const { return move || turn; }
};

struct State {
  std::mutex mutex;
  std::unordered_map<uint32_t, Track> tracks;
  Track camera;  // the vehicle camera's transform as the camera manager hands it to the view
  // The local soldier's own transform (its entity +368, which the vehicle camera follows: run vf2): in a vehicle, its
  // seat, fixed to the vehicle in every camera. What moves with it is ours.
  Track seat;
  uint32_t seat_entity = 0;
  uint64_t camera_frame = ~0ull;  // the last frame the vehicle camera was handed over (in a vehicle)
  Xf camera_shown{};               // the camera handed to the view this frame
  bool camera_shown_valid = false;
  // The camera manager's own transforms (+0x50, +0x90) while they hold ours, and the game's values to put back.
  uint32_t published = 0;
  uint8_t saved[2][64] = {}, wrote[2][64] = {};
  uint32_t reapply = 0;  // the manager whose slots were put back for the tick running now: ours again after it
  uint32_t manager = 0;  // the camera manager our camera was last written into
  uint32_t view = 0;     // the main view the camera manager hands its camera to (sub_82583348's first argument)
  Xf view_game{};        // the camera the manager last handed it (its own, before ours went in instead)
  bool view_game_valid = false;
  uint32_t scratch = 0;
} g;
thread_local bool t_own = false;  // the skeleton being placed on this thread is ours (its bones carried ahead too)

// Whether the camera is a vehicle's this frame (its view handed over within the last few frames).
bool InVehicleView(uint64_t frame) { return g.camera_frame != ~0ull && frame - g.camera_frame <= 4; }

// After a root's tick step: is it moving with the local soldier's seat (ours)? Its step against the seat's, within a
// third of the seat's step and 5 cm, within 20 m; two such ticks make it ours, two or three that are not undo it.
// Measured against the camera before, an outside camera's lag behind the vehicle made a jeep "not ours" (run jeeptree1:
// drawn a tick behind a camera carried ahead) and the driver change between the two (run mot13). Under g.mutex.
void Score(Track& t, uint64_t frame) {
  if (!t.step || t.scored_tick == t.seen_tick) return;
  t.scored_tick = t.seen_tick;
  const Track& ref = g.seat_entity && g.seat.step ? g.seat : g.camera;
  if (!InVehicleView(frame) || !ref.step) {
    t.own_score = 0;
    return;
  }
  double moved = 0, err = 0, dist = 0;
  for (int c = 0; c < 3; ++c) {
    const double dc = double(ref.cur.m[3][c]) - ref.prev.m[3][c];
    const double dr = double(t.cur.m[3][c]) - t.prev.m[3][c];
    moved += dc * dc, err += (dr - dc) * (dr - dc);
    dist += (double(t.cur.m[3][c]) - ref.cur.m[3][c]) * (double(t.cur.m[3][c]) - ref.cur.m[3][c]);
  }
  moved = std::sqrt(moved), err = std::sqrt(err), dist = std::sqrt(dist);
  if (moved < 0.02 || dist > 20) return;  // the seat standing: nothing to tell by
  t.own_score = err < moved / 3 + 0.05 ? std::min(t.own_score + 1, 6) : std::max(t.own_score - 2, -6);
}
std::atomic<uint32_t> g_slot{0};
std::atomic<uint64_t> g_followed{0};  // the vehicle camera's followed entity << 32 | its transform's offset (368, 496)
// The game's time since its last tick, as its own accumulator holds it (look.cpp's CarryMotion): each frame adds the
// frame's time, each tick takes one tick off.
std::atomic<double> g_phase{0.0};
std::atomic<bool> g_ticked{false};  // a tick ran since the last frame
// The phase this frame is placed with, fixed right after the client's update (FramePhase), and the updates run.
std::atomic<double> g_frame_phase{-1.0};
std::atomic<uint64_t> g_updates{0};
uint64_t g_published_update = ~0ull;  // the update the trees were last written after (under g.mutex)
std::atomic<uint64_t> g_frame{0};
Clock::time_point g_frame_at{};

// How a track takes changes between ticks.
struct Tolerance {
  bool ticks_only;  // keep the tick's value and pass over changes between ticks (the vehicle camera)
  double floor;     // otherwise a change between ticks over this, and over `share` of the change at the last tick,
  double share;     // means someone moves it every frame
};
// A root: any change between ticks means someone moves it every frame (look.cpp's walking carry moves the first person
// root by half a tick's step a frame at 60 FPS, a fifth at 144: no share of the step can tell that from a tick's).
constexpr Tolerance kRootTolerance{false, 1e-4, 0};
// The vehicle camera: given no time between ticks, its own smoothing still moved it a little (run mot7: 3 mm and a
// fifth of the tick's turn as a rule, at times as much as the tick's). Blending toward those changes stayed uneven, so
// the camera is blended between its tick values only: the camera the game makes at 30 FPS.
constexpr Tolerance kCameraTolerance{true, 0, 0};
// A bone with a whole local matrix: the first person weapon's bones change a little between ticks (float noise, 1e-4:
// runs foot2, foot9) and by their animation's step at ticks. Something that moves a bone every frame moves it about as
// much between ticks as at them, at any frame rate; under a third of the change at the tick is noise.
constexpr Tolerance kBoneTolerance{false, 2e-4, 0.33};

// The track's next state from the transform now there (a skeleton's root, the camera), and what to blend.
Plan Follow(Track& t, const Xf& now_xf, uint64_t tick, uint64_t frame, const Tolerance& tol) {
  const auto now = Clock::now();
  t.drifted = false;
  if (!t.have) {
    t.cur = now_xf, t.have = true, t.seen_tick = tick;
  } else if (t.seen_tick != tick) {  // the first time since a tick
    const double last_move = t.step_move, last_turn = t.step_turn;
    const uint64_t n = tick - t.seen_tick;
    // Below 30 FPS a frame holds two ticks at times: the tick before the last is taken on the way from the last frame's
    // value (a step of two ticks was left unblended, the game's value shown a tick ahead, then blended back a tick
    // behind on the next frame: run slowfps2 at 24 FPS, the camera stalled on one frame in eleven).
    Xf before_last = t.cur;
    if (n >= 2 && n <= 4 && !Blend(t.cur, now_xf, 1.0 - 1.0 / double(n), before_last)) before_last = t.cur;
    t.prev = before_last, t.cur = now_xf;
    t.step = n >= 1 && n <= 4;
    t.seen_tick = tick;
    t.step_move = t.step_turn = 0;
    for (int c = 0; c < 3; ++c) {
      t.step_move = std::max(t.step_move, double(std::abs(t.cur.m[3][c] - t.prev.m[3][c])));
      for (int r = 0; r < 3; ++r) t.step_turn = std::max(t.step_turn, double(std::abs(t.cur.m[r][c] - t.prev.m[r][c])));
    }
    // Far beyond the last step: not a movement to blend or carry on (a respawn, a teleport, a camera switch).
    t.jump = t.step_move > 3 * last_move + 1.0 || t.step_turn > 3 * last_turn + 0.2;
  } else if (tol.ticks_only) {
    t.drifted = !Same(now_xf, t.cur);  // passed over
  } else if (!Same(now_xf, t.cur)) {
    // Changed without a tick. Within the tolerance it is taken in quietly; more is a part that someone moves every
    // frame, left as it is for a second.
    double moved = 0, step = 0, turned = 0, turn = 0;
    for (int c = 0; c < 3; ++c) {
      moved = std::max(moved, double(std::abs(now_xf.m[3][c] - t.cur.m[3][c])));
      step = std::max(step, double(std::abs(t.cur.m[3][c] - t.prev.m[3][c])));
      for (int r = 0; r < 3; ++r) {
        turned = std::max(turned, double(std::abs(now_xf.m[r][c] - t.cur.m[r][c])));
        turn = std::max(turn, double(std::abs(t.cur.m[r][c] - t.prev.m[r][c])));
      }
    }
    t.drift_move = moved, t.drift_turn = turned, t.tick_move = step, t.tick_turn = turn, t.drifted = true;
    if (moved > std::max(tol.floor, tol.share * step)) t.moves_until = now + std::chrono::seconds(1);
    if (turned > std::max(tol.floor, tol.share * turn)) t.turns_until = now + std::chrono::seconds(1);
    t.cur = now_xf;
  }
  t.frame = frame;
  Plan p;
  if (!t.step || t.jump) return p;
  p.move = now >= t.moves_until && std::memcmp(t.prev.m[3], t.cur.m[3], sizeof(t.cur.m[3]));
  p.turn = now >= t.turns_until && std::memcmp(t.prev.m, t.cur.m, sizeof(float) * 9);
  return p;
}

// First person models the game places at each tick from the camera the tick sees (a tank's vision slit, a bomber's
// sight: run vtanknear5, NXR_NEAR_DUMP, the slit's world matrix in its draw constants held for the whole tick while the
// view moved 8 cm a frame), drawn by the renderer moved by the step from that camera to the one shown this frame
// (NativeXRenderer D154). They sit where the camera track's newest tick value (cur) puts them: moved from it, the slit's
// edge at 94 to 96% of the screen on every frame of a drive with the turret turning, the game's own view 93% (run
// vtankd154b, every frame dumped; 54 to 86% by the frame's place in the tick before, vtankparts3; from prev, steady but
// a tick off, the slit's right half black, vtankd154a). The renderer knows them by holding the camera they were placed
// from (D156: the air raid's bomb sight, a tick of a bomber's flight from the frame's eye, steady on every frame, run
// raiddump4; flipping every other frame before, raiddump2). BF_DEV_ATTACHED=prev or off: to compare. Under g.mutex.
void RecordAttachedMove(const Xf& shown) {
#if defined(BF_DEV_TOOLS)
  static const int which = [] {
    const char* v = std::getenv("BF_DEV_ATTACHED");
    return !v ? 1 : !std::strcmp(v, "prev") ? 0 : !std::strcmp(v, "off") ? 2 : 1;
  }();
  if (which == 2) return;
  const Xf& placed = which == 1 ? g.camera.cur : g.camera.prev;
#else
  const Xf& placed = g.camera.cur;
#endif
  Xf inverse;
  if (!g.camera.step || g.camera.jump || !Inverse(placed, inverse)) return;
  const Xf move = Mul(inverse, shown);  // rows: a point placed from `placed`, then to `shown`
  float position[3] = {shown.m[3][0], shown.m[3][1], shown.m[3][2]};
  float column[16];  // column form: column[r * 4 + c] = the row form's [c][r], the fourth row 0 0 0 1
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) column[r * 4 + c] = r < 3 ? move.m[c][r] : (c == 3 ? 1.0f : 0.0f);
  const float placed_at[3] = {placed.m[3][0], placed.m[3][1], placed.m[3][2]};
  native_x_renderer::SetAttachedCameraMove(position, column, placed_at);
}

// A copy of x in our guest memory (one of kSlots used in turn); 0 when there is no memory. Under g.mutex.
uint32_t Copy(uint8_t* base, const Xf& x) {
  if (!g.scratch) {
    const uint32_t at = REX_KERNEL_MEMORY()->SystemHeapAlloc(kSlots * 64 + 64);
    g.scratch = at ? (at + 63) & ~63u : 0;
  }
  if (!g.scratch) return 0;
  const uint32_t slot = g.scratch + (g_slot.fetch_add(1, std::memory_order_relaxed) % kSlots) * 64;
  StoreXf(base + slot, x);
  return slot;
}

// Bones (bf_smooth_bones): each bone's local position and rotation as the skeleton update hands them to
// sub_826E2220(r3 = position, r4 = rotation, r5 = the parent's transform, r6, r7, r8 = the bone's transform). The game
// samples its animations in the tick, so a bone's own movement (a turret's turn, a weapon raised to the sights) stepped
// 30 times a second even with its root blended. Kept by the bone's transform address, in stripes (the four worker
// threads place bones at once); blended like the roots, one tick behind.
//
// Not finished (off): the first person weapon and hands still step at 30 Hz against the view with it on (runs foot11,
// foot12: bones within 0.6 m of the eye, 54% of the frames still between moving ones while the camera stands, either
// way). They are bones of the soldier's own 147 bone skeleton (run foot8), placed by all three composers (the other two
// take a local matrix, below), and the game changes them every frame as well, so each bone's own pose is not where
// the 30 Hz part is: it comes from higher in the chain, still to be found.
struct Pose {
  float v[8];  // position x y z w, rotation x y z w
  bool operator==(const Pose& o) const { return !std::memcmp(v, o.v, sizeof(v)); }
};
struct BoneTrack {
  Pose cur{}, prev{};
  uint64_t seen_tick = 0, every_frame_until = 0, frame = 0;  // every_frame_until: a tick count
  bool have = false, step = false;
};
constexpr int kStripes = 64;
struct Stripe {
  std::mutex mutex;
  std::unordered_map<uint32_t, BoneTrack> bones;
};
Stripe g_stripes[kStripes];
constexpr uint32_t kPoseSlots = 1u << 16;  // 32 bytes each
std::atomic<uint32_t> g_pose_scratch{0}, g_pose_slot{0};
std::mutex g_pose_alloc;
thread_local bool t_in_skeleton = false;  // inside a skeleton update we look after
thread_local uint32_t t_root = 0;         // that update's root as the game gave it (BF_DEV_BONELOG=identity)
thread_local const char* t_bone_why = "";  // why the last bone was or was not blended (BF_DEV_BONELOG)

// Bones with a whole local matrix, placed by the skeleton's other two composers: sub_826E24E8(r3 = the parent, r4 = the
// local 4x4, unaligned, r5 = the bone's transform, r6 = where the parent is copied) and sub_826E2378(r3 = an offset in
// the parent, r4 = the parent, r5 = the local 4x4, unaligned, r6 = the bone's transform, r7 = where the moved parent is
// copied). The first person weapon's bones are of these. Kept like the roots, by the bone's transform address.
struct MatrixStripe {
  std::mutex mutex;
  std::unordered_map<uint32_t, Track> bones;
};
MatrixStripe g_matrix_stripes[kStripes];
constexpr uint32_t kMatrixSlots = 1u << 15;  // 64 bytes each
std::atomic<uint32_t> g_matrix_scratch{0}, g_matrix_slot{0};

Pose LoadPose(const uint8_t* base, uint32_t position, uint32_t rotation) {
  Pose p;
  for (int i = 0; i < 4; ++i) {
    uint32_t u = Load32(base + position + i * 4);
    std::memcpy(&p.v[i], &u, 4);
    u = Load32(base + rotation + i * 4);
    std::memcpy(&p.v[4 + i], &u, 4);
  }
  return p;
}

// a to b at t; false when it is no blend (not a rotation, or too big a change for one tick).
bool BlendPose(const Pose& a, const Pose& b, double t, Pose& out) {
  double qa[4], qb[4], dot = 0, na = 0, nb = 0;
  for (int i = 0; i < 4; ++i) qa[i] = a.v[4 + i], qb[i] = b.v[4 + i], dot += qa[i] * qb[i], na += qa[i] * qa[i],
                              nb += qb[i] * qb[i];
  if (std::abs(na - 1) > 0.05 || std::abs(nb - 1) > 0.05) return false;
  if (dot < 0) {
    for (double& x : qb) x = -x;
    dot = -dot;
  }
  if (dot < 0.8) return false;  // over 73 degrees in a tick: a new animation, not a movement
  double dp = 0;
  for (int i = 0; i < 3; ++i) dp += (double(b.v[i]) - a.v[i]) * (double(b.v[i]) - a.v[i]);
  if (dp > 4.0) return false;  // over 2 m in a tick
  double q[4], n = 0;
  for (int i = 0; i < 4; ++i) q[i] = qa[i] + (qb[i] - qa[i]) * t, n += q[i] * q[i];
  n = std::sqrt(n);
  if (!(n > 1e-6)) return false;
  for (int i = 0; i < 3; ++i) out.v[i] = float(a.v[i] + (double(b.v[i]) - a.v[i]) * t);
  out.v[3] = b.v[3];
  for (int i = 0; i < 4; ++i) out.v[4 + i] = float(q[i] / n);
  return true;
}
#if defined(BF_DEV_TOOLS)
// BF_DEV_MOTION=1: one moving root within 25 m of the rendered camera followed (another taken when it goes): each of its
// skeleton updates logged with the frame, the phase, the game's last two positions, ours and the camera's; 600 lines.
}  // namespace
namespace bf1943 {
extern std::atomic<float> g_dev_view_pos[3];  // dev_matscan.cpp: the rendered camera's position
}  // namespace bf1943
namespace {
void DevMotion(uint32_t root, const Xf& prev, const Xf& cur, const Xf& shown, double f, uint64_t frame) {
  static const bool on = std::getenv("BF_DEV_MOTION") != nullptr;
  static uint32_t followed = 0;
  static uint64_t followed_frame = 0;
  static int lines = 0;
  if (!on || lines >= 600) return;
  const float cam[3] = {bf1943::g_dev_view_pos[0].load(), bf1943::g_dev_view_pos[1].load(),
                        bf1943::g_dev_view_pos[2].load()};
  {
    // Which roots are the local soldier's own (inside its entity): once each.
    static std::unordered_map<uint32_t, bool> told;
    const uint32_t soldier = bf1943::LocalSoldierEntity();
    if (soldier && root >= soldier && root < soldier + 0x4000 && !told[root]) {
      told[root] = true;
      REXLOG_INFO("BF motion: root {:08X} is in the local soldier entity {:08X} at +{:X}", root, soldier, root - soldier);
    }
  }
  if (followed != root) {
    if (followed && frame - followed_frame < 30) return;
    const double dx = cur.m[3][0] - cam[0], dy = cur.m[3][1] - cam[1], dz = cur.m[3][2] - cam[2];
    const double sx = cur.m[3][0] - prev.m[3][0], sy = cur.m[3][1] - prev.m[3][1], sz = cur.m[3][2] - prev.m[3][2];
    if (dx * dx + dy * dy + dz * dz > 25.0 * 25.0 || sx * sx + sy * sy + sz * sz < 0.02 * 0.02) return;
    followed = root;
    REXLOG_INFO("BF motion: following root {:08X} (the local soldier entity {:08X})", root, bf1943::LocalSoldierEntity());
  }
  followed_frame = frame;
  ++lines;
  REXLOG_INFO("BF motion: frame {} f {:.3f} prev {:.3f} {:.3f} {:.3f} cur {:.3f} {:.3f} {:.3f} shown {:.3f} {:.3f} {:.3f} "
              "camera {:.3f} {:.3f} {:.3f}",
              frame, f, prev.m[3][0], prev.m[3][1], prev.m[3][2], cur.m[3][0], cur.m[3][1], cur.m[3][2], shown.m[3][0],
              shown.m[3][1], shown.m[3][2], cam[0], cam[1], cam[2]);
}
#if defined(_WIN32)
}  // namespace
namespace bf1943::dev {
void WatchHostAccess(std::vector<const void*> addresses, double seconds, const char* label);  // dev_watch.cpp
void WatchHostWrites(std::vector<const void*> addresses, double seconds, const char* label);  // dev_watch.cpp
}  // namespace bf1943::dev
namespace {
// BF_DEV_ROOTFIND=<n>: on the n-th skeleton update of a root carried ahead (ours), once: every place in guest memory
// that holds its transform (the four rows' first three floats), then a hardware watch of reads and writes of up to four
// of them (their position row) for 3 s. Who keeps and reads the vehicle's transform besides its skeleton (the rigid
// meshes, such as a tank's first person vision slit, stayed on the tick: run mot9).
void DevFind(uint8_t* base, const Xf& x, uint32_t root, uint32_t scratch, const char* env, std::atomic<long>& n,
             const char* what) {
  const char* v = std::getenv(env);
  const long at = v && *v ? std::atol(v) : 0L;
  if (at <= 0 || n.fetch_add(1) + 1 != at) return;
  uint8_t want[4][12];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 3; ++c) {
      uint32_t u;
      std::memcpy(&u, &x.m[r][c], 4);
      Store32(want[r] + c * 4, u);
    }
  std::vector<uint32_t> hits;
  for (uint64_t a = 0; a < 0x100000000ull && hits.size() < 96;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) break;
    const uint64_t start = uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base);
    const uint64_t end = start + mbi.RegionSize;
    const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                          (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                          PAGE_EXECUTE_READWRITE));
    if (readable)
      for (uint64_t p = (start + 15) & ~15ull; p + 64 <= end && hits.size() < 96; p += 16) {
        const uint8_t* m = base + p;
        if (std::memcmp(m + 48, want[3], 12) || std::memcmp(m, want[0], 12) || std::memcmp(m + 16, want[1], 12) ||
            std::memcmp(m + 32, want[2], 12))
          continue;
        if (scratch && p >= scratch && p < scratch + uint64_t(kSlots) * 64) continue;  // our own copies
        hits.push_back(uint32_t(p));
      }
    a = std::max(end, a + 4096);
  }
  std::string list;
  for (uint32_t h : hits) list += fmt::format(" {:08X}", h);
  REXLOG_INFO("BF motion: the {} {:08X}'s transform is also at{}", what, root, list.empty() ? " none" : list);
  // Watched: the 16 MB page view (0xC0000000 up, the one the game's own pointers use: the root copies are 0xDD...), not
  // the root's own copy; BF_DEV_ROOTFIND_SKIP=<n> passes over the first n of those (four debug registers a run).
  static const long skip = [] {
    const char* v = std::getenv("BF_DEV_ROOTFIND_SKIP");
    return v && *v ? std::atol(v) : 0L;
  }();
  std::vector<const void*> watch;
  std::string watched;
  long passed = 0;
  for (uint32_t h : hits)
    if (h != root && !(h >= 0xA0000000u && h < 0xC0000000u) && h < 0xE0000000u && watch.size() < 4 &&
        passed++ >= skip) {
      watch.push_back(base + h + 48);
      watched += fmt::format(" {:08X}", h);
    }
  REXLOG_INFO("BF motion: watching{}", watched);
  // BF_DEV_FIND_WRITES=1: writes only (who keeps the copies)
  if (!watch.empty()) {
    if (std::getenv("BF_DEV_FIND_WRITES")) bf1943::dev::WatchHostWrites(watch, 3.0, what);
    else bf1943::dev::WatchHostAccess(watch, 3.0, what);
  }
}
void DevRootFind(uint8_t* base, const Xf& x, uint32_t root, uint32_t scratch) {
  static std::atomic<long> n{0};
  DevFind(base, x, root, scratch, "BF_DEV_ROOTFIND", n, "root");
}
// BF_DEV_CAMFIND=<n>: the same for the vehicle camera's transform as the camera manager hands it to the view (r4 of
// sub_82583348 from 0x82294C28) on its n-th time in a vehicle.
void DevCamFind(uint8_t* base, uint32_t at, uint32_t scratch) {
  static std::atomic<long> n{0};
  DevFind(base, LoadXf(base + at), at, scratch, "BF_DEV_CAMFIND", n, "camera");
}
#endif
}  // namespace

#if defined(BF_DEV_TOOLS)
// BF_DEV_TREE=1: the part trees' placement (sub_822A9978(node r3, locals r4, worlds r5), recursive): once a second, its
// outer calls (not from itself) inside the client's tick and outside it, by caller, with the first world arrays seen.
REX_EXTERN(__imp__sub_822A9978);
REX_HOOK_RAW(sub_822A9978) {
  static const bool on = std::getenv("BF_DEV_TREE") != nullptr;
  const uint32_t lr = uint32_t(ctx.lr);
  if (on && !(lr >= 0x822A9978 && lr < 0x822A9AC0)) {
    static std::mutex m;
    static std::map<uint32_t, std::array<uint64_t, 2>> by_lr;  // lr -> calls outside, inside the tick
    static std::vector<uint32_t> worlds;
    static auto at = Clock::now();
    std::lock_guard lock(m);
    ++by_lr[lr][bf1943::g_in_client_tick.load(std::memory_order_relaxed) ? 1 : 0];
    if (worlds.size() < 8 && std::find(worlds.begin(), worlds.end(), ctx.r5.u32) == worlds.end())
      worlds.push_back(ctx.r5.u32);
    if (Clock::now() - at > std::chrono::seconds(1)) {
      at = Clock::now();
      std::string line;
      for (auto& [l, n] : by_lr) line += fmt::format(" {:08X}: {} out, {} in;", l, n[0], n[1]);
      std::string w;
      for (uint32_t x : worlds) w += fmt::format(" {:08X}", x);
      REXLOG_INFO("BF tree: outer calls by caller (outside the tick, inside it):{} worlds{}", line, w);
      by_lr.clear();
    }
  }
  __imp__sub_822A9978(ctx, base);
}
#endif

// The camera's base provider (sub_82286620(out, entity)): the entity the camera follows and its transform, copied to
// out from entity+368, +496 or the entity's physics body. In a vehicle that is the local soldier, in its seat (run
// vf2): noted with where its transform came from, for the seat (Score; look.cpp's soldier entity is another object).
REX_EXTERN(__imp__sub_82286620);
REX_HOOK_RAW(sub_82286620) {
  const uint32_t out = ctx.r3.u32, entity = ctx.r4.u32;
  __imp__sub_82286620(ctx, base);
  if (out && entity && !bf1943::LocalOnFoot() && bf1943::InMatch()) {
    const uint32_t from = !std::memcmp(base + out, base + entity + 368, 64)   ? 368
                          : !std::memcmp(base + out, base + entity + 496, 64) ? 496
                                                                              : 0;
    g_followed.store(uint64_t(entity) << 32 | from, std::memory_order_relaxed);
  }
#if defined(BF_DEV_TOOLS) && defined(_WIN32)
  static const long at = [] {
    const char* v = std::getenv("BF_DEV_VEHFIND");
    return v && *v ? std::atol(v) : 0L;
  }();
  if (at <= 0 || !out || !entity || !bf1943::InMatch()) return;
  {
    // each entity it is asked about, once, with where its transform came from and whether we are on foot
    static std::mutex m;
    static std::vector<uint32_t> told;
    std::lock_guard lock(m);
    if (std::find(told.begin(), told.end(), entity) == told.end() && told.size() < 40) {
      told.push_back(entity);
      const Xf x = LoadXf(base + out);
      REXLOG_INFO("BF motion: provider for entity {:08X} (vtable {:08X}, on foot {}): from {} at {:.3f} {:.3f} {:.3f}, "
                  "lr {:08X}", entity, Load32(base + entity), bf1943::LocalOnFoot(),
                  !std::memcmp(base + out, base + entity + 368, 64) ? "+368"
                  : !std::memcmp(base + out, base + entity + 496, 64) ? "+496" : "elsewhere",
                  x.m[3][0], x.m[3][1], x.m[3][2], uint32_t(ctx.lr));
    }
  }
  if (bf1943::LocalOnFoot()) return;
  static std::atomic<long> n{0};
  const long k = n.fetch_add(1) + 1;
  if (k == at) {
    const char* from = !std::memcmp(base + out, base + entity + 368, 64)   ? "+368"
                       : !std::memcmp(base + out, base + entity + 496, 64) ? "+496"
                                                                           : "elsewhere (its physics body?)";
    const Xf x = LoadXf(base + out);
    REXLOG_INFO("BF motion: the camera follows entity {:08X} (vtable {:08X}); its transform from {}, at {:.3f} {:.3f} "
                "{:.3f}", entity, Load32(base + entity), from, x.m[3][0], x.m[3][1], x.m[3][2]);
    static std::atomic<long> n_find{0};
    n_find = at - 1;  // DevFind fires on its at-th call: this one
    DevFind(base, x, out, g.scratch, "BF_DEV_VEHFIND", n_find, "vehicle");
  }
#endif
}

namespace {

// BF_DEV_MOTION: every 2 s, each root within 4 m of the rendered camera that frame: its distance, its tick step, whether
// it was blended, carried ahead (ours) or left as the game's, and why. Under g.mutex.
void DevNear(uint32_t root, const Track& t, const Plan& plan, bool blended, uint64_t frame) {
  static const bool on = std::getenv("BF_DEV_MOTION") != nullptr;
  static uint64_t at = 0;
  static int lines = 0;
  if (!on || lines > 400) return;
  if (frame >= at + 120) at = frame;  // a new listing every 120 frames
  if (frame != at) return;
  const float cam[3] = {bf1943::g_dev_view_pos[0].load(), bf1943::g_dev_view_pos[1].load(),
                        bf1943::g_dev_view_pos[2].load()};
  double d = 0;
  for (int c = 0; c < 3; ++c) d += (t.cur.m[3][c] - cam[c]) * (t.cur.m[3][c] - cam[c]);
  d = std::sqrt(d);
  if (d > 4) return;
  ++lines;
  const auto now = Clock::now();
  REXLOG_INFO("BF motion: near root {:08X} at {:.2f} m: step {:.3f} m {:.4f} turn, score {}, {}{}{}{}", root, d,
              t.step_move, t.step_turn, t.own_score,
              blended ? (t_own ? "carried ahead" : "blended") : "the game's",
              t.step ? "" : ", no tick step", t.jump ? ", a jump" : "",
              now < t.moves_until || now < t.turns_until ? ", moves every frame" : (plan.any() ? "" : ", still"));
}
#endif
}  // namespace

namespace {
// Rigid parts (bf_smooth_motion). A vehicle's body has no skeleton (run mot16: while driving, the only skeleton near
// the camera is the soldier's): its parts are a tree of components whose world transforms the game works out only in
// its tick (sub_822AE5D0 -> sub_822AE4C8 -> sub_822A9978, run tree1: 1,110 calls a second, all inside the tick) and
// the renderer reads every frame (runs vscan3, vscan4: sub_825C1508 and sub_823C5510 on its own thread). So with the
// camera blended the bodies stepped against it (TekRantGaming, 0.1.25: tank, jeep and plane "stutter incredibly bad").
//
// A tree (sub_822AE4C8's layout): +14 its part count n (byte), the n nodes of 16 bytes from +16, then the n local
// transforms, then the tree's base world transform (a part's parent when its node says 255) and the n parts' world
// transforms, 64 bytes each. Each tick the trees the game updated are noted; each frame (before the camera manager)
// their world transforms are written blended between their last two tick values (the player's own vehicle carried
// ahead, as its camera), and before the next tick the game's own values are put back, so the tick never sees ours.
struct TreeTrack {
  uint32_t worlds = 0;  // the base world's address; the parts' follow, 64 bytes each
  uint32_t count = 0;   // the base and the parts
  std::vector<Xf> prev, cur, shown;
  uint64_t seen_tick = 0;  // the tick cur was taken after
  bool have = false, step = false, published = false, moved = false;
  Track base;              // the base world's own track, for Score
  uint64_t frame = 0;
  uint32_t spun = 0;  // parts that turned too far in the last tick to blend (a propeller, a fast wheel): carried
  bool was_own = false;          // carried ahead at the last publish
  uint64_t published_frame = 0;  // the frame of the last publish
};
std::unordered_map<uint32_t, TreeTrack> g_trees;  // by the tree's address
std::atomic<int> g_trees_window{0};  // 1 between a publish and the next restore (BF_DEV_TREEWINDOW)
#if defined(BF_DEV_TOOLS)
// BF_DEV_TREEWINDOW: what the renderer and the rest of the game see of the tree BF_DEV_TREELOG follows.
std::atomic<uint32_t> g_dev_followed_tree{0};
std::atomic<uint64_t> g_dev_kept{0};         // its parts left as the game wrote them since the tick (publishes)
std::atomic<uint64_t> g_dev_flips{0};        // publishes where it changed between carried ahead and blended
std::atomic<uint64_t> g_dev_unpublished{0};  // frames it was not written after one it was
std::atomic<uint64_t> g_dev_outside{0}, g_dev_outside_followed{0};  // world updates outside the client tick
std::thread::id g_dev_main_thread{};
// BF_DEV_TIMELINE=<seconds>: from that long into a vehicle view, 120 frames of events with their time and thread:
// the client's update, its ticks, the trees written, the camera manager, VdSwap, skeleton updates near the seat and the
// renderer's tree reads; then written to the log in order (who runs before whom in a frame).
struct DevEvent {
  int64_t us;
  uint32_t thread;
  char what[12];
  uint64_t frame;
};
std::mutex g_dev_timeline_mutex;
std::vector<DevEvent> g_dev_timeline;
std::atomic<int> g_dev_timeline_state{0};  // 0 waiting, 1 recording, 2 written
uint64_t g_dev_timeline_end = 0;
void DevMark(const char* what) {
  static const double at = [] {
    const char* v = std::getenv("BF_DEV_TIMELINE");
    return v && *v ? std::atof(v) : -1.0;
  }();
  if (at < 0 || g_dev_timeline_state.load(std::memory_order_relaxed) != 1) return;
  DevEvent e{};
  e.us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
  e.thread = uint32_t(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFF);
  std::snprintf(e.what, sizeof(e.what), "%s", what);
  e.frame = g_frame.load(std::memory_order_relaxed);
  std::lock_guard lock(g_dev_timeline_mutex);
  if (g_dev_timeline.size() < 20000) g_dev_timeline.push_back(e);
}
// Skeleton updates against the frame's publish: run after a later tick than it, with a different phase; roots near the
// camera that changed between ticks (left unblended for a second) and that were ours.
std::atomic<uint64_t> g_dev_publish_tick{0}, g_dev_skel{0}, g_dev_skel_late{0}, g_dev_skel_phase{0}, g_dev_skel_drift{0},
    g_dev_skel_near{0}, g_dev_skel_near_own{0}, g_dev_skel_near_held{0};
std::atomic<double> g_dev_publish_phase{0};
#endif
std::vector<uint32_t> g_trees_updated;            // updated in the tick now running (main thread)

// The tree a component's world update belongs to (sub_822AE5D0(component), in the tick), noted for the tick's end.
void NoteTreeUpdated(uint32_t tree) {
  if (std::find(g_trees_updated.begin(), g_trees_updated.end(), tree) == g_trees_updated.end())
    g_trees_updated.push_back(tree);
}

uint32_t TreeCount(const uint8_t* base, uint32_t tree) { return base[tree + 14]; }
void PublishTreesLocked(uint8_t* base, bool final);  // below

// The game's own world transforms back in every tree we wrote (if it did not write them since). Under g.mutex.
void RestoreTreesLocked(uint8_t* base) {
  for (auto& [tree, t] : g_trees) {
    if (!t.published) continue;
    for (uint32_t i = 0; i < t.count; ++i) {
      uint8_t* at = base + t.worlds + i * 64;
      Xf now_xf = LoadXf(at);
      if (Same(now_xf, t.shown[i])) StoreXf(at, t.cur[i]);
    }
    t.published = false;
  }
  g_trees_window.store(0, std::memory_order_relaxed);
}

// The camera manager's own transforms back to the game's, if they still hold ours. Under g.mutex.
void RestoreCamera(uint8_t* base) {
  if (!g.published) return;
  for (int i = 0; i < 2; ++i) {
    uint8_t* at = base + g.published + (i ? 0x90 : 0x50);
    if (!std::memcmp(at, g.wrote[i], 64)) std::memcpy(at, g.saved[i], 64);
  }
  g.published = 0;
}

// For a tick only: the game's camera in the manager while the tick runs (it places a vehicle's first person parts
// from it, the tank's vision slit), and ours back right after, so that whatever else reads the manager's camera
// between the tick and the camera manager's frame sees ours on tick frames as on the others. Put back from the tick
// to the camera manager's frame (0.1.26), the plane's cockpit canopy, read there, got the game's camera one frame in
// three and ours on the others, and was drawn out of place or not at all (runs canopy*: TekRantGaming's "the cockpit view is a
// mess, flickering in and out of existence"). Under g.mutex.
void RestoreCameraForTick(uint8_t* base) {
  const uint32_t manager = g.published;
  RestoreCamera(base);
  g.reapply = manager;
}
// The camera manager's camera, between its frames, set to our best estimate of this frame's camera: what the camera
// manager's frame will hand the view (its last two tick cameras carried on by the frame's phase), and after a tick
// that frame has not seen yet, carried on one tick further. The game places a vehicle's first person parts from the
// manager's camera before that frame (the tick: a tank's vision slit; the skeleton updates: the pilot's view model, the
// plane's canopy): written only after the manager's frame, they took the frame before's camera, a frame or a tick
// behind the view, and at a plane's speed out of sight (runs vplanerider: the pilot 0.7 m behind the camera at
// take-off; TekRantGaming's "the cockpit view is a mess"). Under g.mutex.
void PublishCameraEstimateLocked(uint8_t* base) {
  if (!REXCVAR_GET(bf_smooth_motion) || !g.manager || !g.camera.have || !g.camera.step || g.camera.jump) return;
#if defined(BF_DEV_TOOLS)
  static const bool off = std::getenv("BF_DEV_NO_CAMEST") != nullptr;  // to compare
  if (off) return;
#endif
  if (!InVehicleView(g_frame.load(std::memory_order_relaxed))) return;
  const uint64_t since = bf1943::ClientTicks() - g.camera.seen_tick;
  if (since > 2) return;
  const double t = (REXCVAR_GET(bf_smooth_motion_predict) ? 1.0 : 0.0) + bf1943::FramePhase() + double(since);
  Xf est;
  if (!Blend(g.camera.prev, g.camera.cur, t, est, std::memcmp(g.camera.prev.m[3], g.camera.cur.m[3], 12) != 0,
             std::memcmp(g.camera.prev.m, g.camera.cur.m, 36) != 0))
    return;
  for (int i = 0; i < 2; ++i) {
    uint8_t* at = base + g.manager + (i ? 0x90 : 0x50);
    if (g.published != g.manager || std::memcmp(at, g.wrote[i], 64)) std::memcpy(g.saved[i], at, 64);  // the game's
    Xf x = est;
    for (int r = 0; r < 4; ++r) x.pad[r] = Load32(at + r * 16 + 12);
    StoreXf(at, x);
    std::memcpy(g.wrote[i], at, 64);
  }
  g.published = g.manager;
}

void ReapplyCameraAfterTick(uint8_t* base) {
  const uint32_t manager = g.reapply;
  g.reapply = 0;
  if (!manager) return;
  for (int i = 0; i < 2; ++i)  // the tick left the game's values there (it did not write its own)
    if (std::memcmp(base + manager + (i ? 0x90 : 0x50), g.saved[i], 64)) return;
  for (int i = 0; i < 2; ++i) std::memcpy(base + manager + (i ? 0x90 : 0x50), g.wrote[i], 64);
  g.published = manager;
}

// The camera manager (its frame's r3), and the active camera object's transform as the camera getter sub_82294708
// finds it (else the manager's own +0x50): for the dev searches below.
std::atomic<uint32_t> g_camera_manager{0};
#if defined(BF_DEV_TOOLS)
uint32_t ActiveCameraTransform(const uint8_t* base, uint32_t manager) {
  auto ok = [](uint32_t a) { return a >= 0x00100000u && a < 0xF0000000u; };
  const uint32_t head = manager + 208, first = Load32(base + head);
  if (!ok(first) || first == head) return 0;
  const uint32_t x = Load32(base + first + 8);
  if (!ok(x)) return 0;
  const uint32_t list = Load32(base + x + 32);
  if (!ok(list) || list == x + 8) return 0;
  const uint32_t camera = Load32(base + list + 20);
  return ok(camera) ? camera + 16 : 0;
}
#endif
}  // namespace

namespace bf1943 {
// Right before the camera manager's frame (vehicle_camera.cpp): which manager it is (for the dev searches).
void TickBlendCameraForManager(uint8_t* base, uint32_t manager) {
  (void)base;
  g_camera_manager.store(manager, std::memory_order_relaxed);
}

// Before each client tick (vehicle_camera.cpp): the game's own world transforms back in every tree we wrote, and its
// camera in the camera manager.
void TickBlendBeforeTick(uint8_t* base) {
#if defined(BF_DEV_TOOLS)
  DevMark("tick+");
#endif
  std::lock_guard lock(g.mutex);
#if defined(BF_DEV_TOOLS)
  g_dev_main_thread = std::this_thread::get_id();
#endif
  // The tick sees the game's own camera and trees: it places a vehicle's first person parts from them (the tank's
  // vision slit: carried ahead twice with ours in it, run mot18). Ours are back right after it (ReapplyCameraAfterTick,
  // PublishCameraEstimateLocked, PublishTreesLocked).
#if defined(BF_DEV_TOOLS)
  static const bool keep_camera = std::getenv("BF_DEV_NO_TICKRESTORE") != nullptr;  // to compare
  if (!keep_camera)
#endif
    RestoreCameraForTick(base);
  RestoreTreesLocked(base);
  g_trees_updated.clear();
}

// After each client tick: the trees it updated take its values as their newest; the others stood still.
void TickBlendAfterTick(uint8_t* base) {
#if defined(BF_DEV_TOOLS)
  DevMark("tick-");
#endif
  const uint64_t tick = ClientTicks();
  std::lock_guard lock(g.mutex);
  for (uint32_t tree : g_trees_updated) {
    const uint32_t n = TreeCount(base, tree);
    if (!n || n > 200) continue;
    TreeTrack& t = g_trees[tree];
    const uint32_t worlds = tree + 16 + 80 * n;
    if (t.worlds != worlds || t.count != n + 1) {  // new, or a different tree at the address
      t = TreeTrack{};
      t.worlds = worlds, t.count = n + 1;
      t.prev.resize(t.count), t.cur.resize(t.count), t.shown.resize(t.count);
    }
    if (t.have) t.prev = t.cur;
    t.moved = false;
    for (uint32_t i = 0; i < t.count; ++i) {
      t.cur[i] = LoadXf(base + worlds + i * 64);
      t.moved |= t.have && !Same(t.prev[i], t.cur[i]);
    }
    t.step = t.have && tick - t.seen_tick == 1;
    t.seen_tick = tick, t.have = true;
    t.frame = g_frame.load(std::memory_order_relaxed);
    Follow(t.base, t.cur[0], tick, t.frame, kRootTolerance);
  }
  {  // the seat, once a tick: the vehicle camera's followed entity (the local soldier) and its transform
    const uint64_t followed = g_followed.load(std::memory_order_relaxed);
    const uint32_t entity = uint32_t(followed >> 32), from = uint32_t(followed);
    if (entity && from) {
      if (g.seat_entity != entity) g.seat = Track{}, g.seat_entity = entity;
      Follow(g.seat, LoadXf(base + entity + from), tick, g_frame.load(std::memory_order_relaxed), kRootTolerance);
    }
  }
  for (auto& [tree, t] : g_trees)  // not updated in this tick: standing still
    if (t.have && t.seen_tick != tick) {
      if (t.seen_tick == tick - 1) t.prev = t.cur, t.step = true, t.seen_tick = tick, t.moved = false;
      Follow(t.base, t.cur[0], tick, t.frame, kRootTolerance);
    }
  g_trees_updated.clear();
#if defined(BF_DEV_TOOLS)
  {  // BF_DEV_VIEWREL=<parts>: every 15 ticks, the view against parts 0 and 21 of the nearest tree of that many parts
    static const int want = [] {
      const char* v = std::getenv("BF_DEV_VIEWREL");
      return v && *v ? std::atoi(v) : 0;
    }();
    static int n = 0, lines = 0;
    if (want > 0 && ++n % 15 == 0 && lines < 200) {
      const float cam[3] = {g_dev_view_pos[0].load(), g_dev_view_pos[1].load(), g_dev_view_pos[2].load()};
      const TreeTrack* best = nullptr;
      double bd = 1e9;
      for (auto& [tree, t] : g_trees) {
        if (!t.have || int(t.count) - 1 != want) continue;
        double d2 = 0;
        for (int c = 0; c < 3; ++c) d2 += (t.cur[0].m[3][c] - cam[c]) * (t.cur[0].m[3][c] - cam[c]);
        if (d2 < bd) bd = d2, best = &t;
      }
      if (best && bd < 100 && best->count > 22) {
        ++lines;
        const Xf& p0 = best->cur[0];
        const Xf& p21 = best->cur[22];  // part 21 (index 0 is the base)
        // the view in part 0's frame (rows right, up, forward)
        double rel[3], rel21[3];
        for (int r = 0; r < 3; ++r) {
          rel[r] = 0, rel21[r] = 0;
          for (int c = 0; c < 3; ++c) {
            rel[r] += (cam[c] - p0.m[3][c]) * p0.m[r][c];
            rel21[r] += (cam[c] - p21.m[3][c]) * p0.m[r][c];
          }
        }
        REXLOG_INFO("BF viewrel: the view from the base {:.3f} {:.3f} {:.3f}, from part 21 {:.3f} {:.3f} {:.3f} (base's rows)",
                    rel[0], rel[1], rel[2], rel21[0], rel21[1], rel21[2]);
      }
    }
  }
#endif
  // The frame's phase after this tick, and the trees written for it before the frame's skeletons are placed; the
  // camera manager's camera ours again.
  g_frame_phase.store(TickBlendPhase(), std::memory_order_relaxed);
  PublishTreesLocked(base, false);
#if defined(BF_DEV_TOOLS)
  static const bool no_reapply = std::getenv("BF_DEV_NO_REAPPLY") != nullptr;  // to compare
  if (no_reapply) g.reapply = 0;
#endif
  ReapplyCameraAfterTick(base);
  PublishCameraEstimateLocked(base);
}

}  // namespace bf1943

namespace {
#if defined(BF_DEV_TOOLS) && defined(_WIN32)
// BF_DEV_NEARFIND=<seconds>: that long into a vehicle view, after the frame's last publish, once: every rigid transform
// in guest memory within 0.6 m of the view (orthogonal rows, not one of our trees or copies), then for 120 frames each
// of them against the view (position in the view's frame), to find what is drawn at the tick (a tank's first person
// slit and barrel: runs vtankparts3, vtankdump*). BF_DEV_NEARFIND_WATCH=<address>: also watch that one's position row
// for 3 s (who writes it). Under g.mutex.
void DevNearFind(uint8_t* base, uint64_t frame) {
  static const double at_s = [] {
    const char* v = std::getenv("BF_DEV_NEARFIND");
    return v && *v ? std::atof(v) : -1.0;
  }();
  if (at_s < 0 || !g.view_game_valid) return;
  static Clock::time_point since{};
  static uint64_t found_frame = 0;
  static std::vector<uint32_t> hits;
  if (!InVehicleView(frame)) {
    if (!found_frame) since = {};
    return;
  }
  if (since == Clock::time_point{}) since = Clock::now();
  if (!found_frame) {
    if (std::chrono::duration<double>(Clock::now() - since).count() < at_s) return;
    found_frame = frame;
    const float cam[3] = {g.view_game.m[3][0], g.view_game.m[3][1], g.view_game.m[3][2]};
    for (uint64_t a = 0; a < 0x100000000ull && hits.size() < 400;) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) break;
      const uint64_t start = uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base);
      const uint64_t end = start + mbi.RegionSize;
      const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                            PAGE_EXECUTE_READWRITE));
      if (readable && start >= 0x00100000ull)
        for (uint64_t p = (start + 15) & ~15ull; p + 64 <= end && hits.size() < 400; p += 16) {
          const uint8_t* m = base + p;
          // a 4x4 (rows, the position in the fourth row) or a 3x4 as shaders take it (the position in each row's
          // fourth float): marked by the low bit of the address noted
          for (int layout = 0; layout < 2; ++layout) {
          float pos[3];
          bool within = true;
          for (int c = 0; c < 3 && within; ++c) {
            pos[c] = layout ? BeF(m + c * 16 + 12) : BeF(m + 48 + c * 4);
            within = std::abs(pos[c] - cam[c]) < 0.6f;
          }
          if (!within) continue;
          float r[3][3];
          double len[3] = {};
          bool unit = true;
          for (int i = 0; i < 3 && unit; ++i) {
            for (int c = 0; c < 3; ++c) r[i][c] = BeF(m + i * 16 + c * 4), len[i] += double(r[i][c]) * r[i][c];
            len[i] = std::sqrt(len[i]);
            unit = len[i] > 0.01 && len[i] < 100.0;  // any scale (a first person model may be drawn shrunk)
          }
          if (!unit) continue;
          auto dot = [&](int i, int j) {
            return (r[i][0] * r[j][0] + r[i][1] * r[j][1] + r[i][2] * r[j][2]) / (len[i] * len[j]);
          };
          if (std::abs(dot(0, 1)) > 0.05 || std::abs(dot(0, 2)) > 0.05 || std::abs(dot(1, 2)) > 0.05) continue;
          bool copy = true;  // the camera itself, copied (views, the manager's slots): not what is sought
          for (int i = 0; i < 3 && copy; ++i)
            for (int c = 0; c < 3 && copy; ++c) copy = std::abs(r[i][c] - g.view_game.m[i][c]) < 1e-5f;
          for (int c = 0; c < 3 && copy; ++c) copy = std::abs(pos[c] - cam[c]) < 1e-4f;
          if (copy) continue;
          bool ours = g.scratch && p >= g.scratch && p < g.scratch + uint64_t(kSlots) * 64;
          for (auto& [tree, t] : g_trees)
            if (p >= t.worlds && p < t.worlds + uint64_t(t.count) * 64) ours = true;
          if (!ours) hits.push_back(uint32_t(p) | uint32_t(layout));
          }
        }
      a = std::max(end, a + 4096);
    }
    std::string list;
    for (uint32_t h : hits) {
      // which tracked tree holds it, by physical address (the 0xA, 0xC and 0xE page views alias the same memory)
      std::string owner;
      const uint32_t ph = (h & ~15u) & 0x1FFFFFFFu;
      for (auto& [tree, t] : g_trees) {
        const uint32_t pw = t.worlds & 0x1FFFFFFFu;
        if (ph >= pw && ph < pw + t.count * 64) {
          owner = fmt::format("=tree {:08X} ({} parts) part {} +{}", tree, t.count - 1, (ph - pw) / 64, (ph - pw) % 64);
          break;
        }
      }
      list += fmt::format(" {:08X}{}", h, owner);
    }
    REXLOG_INFO("BF nearfind: {} rigid transforms within 0.6 m of the view:{}", hits.size(), list);
    if (const char* w = std::getenv("BF_DEV_NEARFIND_WATCH"); w && *w) {
      // an address, or "active": the active camera object's transform (what the camera getter hands out)
      // "first34": the first 3x4 hit's position (its first row's fourth float: a draw's constants)
      uint32_t first34 = 0;
      for (uint32_t h : hits)
        if (h & 1) {
          first34 = (h & ~15u) + 12 - 48;  // so that +48 below lands on the first row's fourth float
          break;
        }
      const uint32_t watch = !std::strcmp(w, "active")
                                 ? ActiveCameraTransform(base, g_camera_manager.load(std::memory_order_relaxed))
                             : !std::strcmp(w, "first34") ? first34
                                                          : uint32_t(std::strtoul(w, nullptr, 16));
      {
        const uint32_t m = g_camera_manager.load(std::memory_order_relaxed);
        const uint32_t first = m ? Load32(base + m + 208) : 0;
        const uint32_t x = first ? Load32(base + first + 8) : 0;
        const uint32_t list = x ? Load32(base + x + 32) : 0;
        const uint32_t cam = list ? Load32(base + list + 20) : 0;
        REXLOG_INFO("BF nearfind: manager {:08X} first {:08X} (head {:08X}) x {:08X} list {:08X} (x+8 {:08X}) camera {:08X}",
                    m, first, m + 208, x, list, x + 8, cam);
      }
      REXLOG_INFO("BF nearfind: watching {:08X}", watch);
      if (watch) bf1943::dev::WatchHostAccess({base + watch + 48}, 3.0, "nearfind");
    }
  }
  if (frame >= found_frame + 120) return;
  Xf inverse;
  if (!Inverse(g.view_game, inverse)) return;
  std::string list;
  for (uint32_t h : hits) {
    const uint32_t at = h & ~15u;
    Xf x = LoadXf(base + at);
    if (h & 1) {  // a 3x4: the position from each row's fourth float, the rows as they are (transposed or not)
      for (int c = 0; c < 3; ++c) x.m[3][c] = BeF(base + at + c * 16 + 12);
    }
    const Xf rel = Mul(x, inverse);
    list += fmt::format(" [{:08X} {:.4f} {:.4f} {:.4f}]", h, rel.m[3][0], rel.m[3][1], rel.m[3][2]);
  }
  REXLOG_INFO("BF nearfind: frame {} f {:.3f} tick {}{}", frame, bf1943::FramePhase(), bf1943::ClientTicks(), list);
}
#endif

// The trees that moved in the last tick written blended for this frame (FramePhase). Under g.mutex. Done three times a
// frame, as the game's own frame runs (run tljeep2's timeline: the client's update spans the frame; in it the ticks,
// then the riders' skeletons, then the camera manager; the renderer reads the trees after the camera manager): at the
// update's start, after each tick (the restore before it put the game's values back), and from the camera manager
// (`final`, the frame's last, the one the tree log follows). Written from the camera manager only, the skeletons placed
// before it took the frame before's trees, or the tick's own on a tick frame (run netjeep4: the driver placed from the
// jeep's tick values on 500 of 1,500 frames, then drawn against the jeep written later: TekRantGaming's "the driver phases in
// and out", the jeep "two models slightly offset").
void PublishTreesLocked(uint8_t* base, bool final) {
  if (!REXCVAR_GET(bf_smooth_motion) || !bf1943::InMatch()) return;
#if defined(BF_DEV_TOOLS)
  // BF_DEV_LATE_PUBLISH=1: written from the camera manager only, as 0.1.26 did (to compare).
  static const bool late = std::getenv("BF_DEV_LATE_PUBLISH") != nullptr;
  if (late && !final) return;
  // BF_DEV_NO_TREES=1: the trees never written (to compare the camera with and without).
  static const bool none = std::getenv("BF_DEV_NO_TREES") != nullptr;
  if (none) return;
#endif
  const uint64_t frame = g_frame.load(std::memory_order_relaxed);
  const double f = bf1943::FramePhase();
#if defined(BF_DEV_TOOLS)
  g_dev_publish_tick.store(bf1943::ClientTicks(), std::memory_order_relaxed);
  g_dev_publish_phase.store(f, std::memory_order_relaxed);
  DevMark(final ? "pub" : "pub-early");
#endif
#if defined(BF_DEV_TOOLS)
  // BF_DEV_SKIP_PARTS=<n>: trees of n parts never written (which tree is which: TekRantGaming's missing canopy).
  static const uint32_t skip_parts = [] {
    const char* v = std::getenv("BF_DEV_SKIP_PARTS");
    return v && *v ? uint32_t(std::atoi(v)) : 0u;
  }();
#endif
  for (auto& [tree, t] : g_trees) {
#if defined(BF_DEV_TOOLS)
    if (skip_parts && t.count - 1 == skip_parts) continue;
#endif
    if (!t.have || !t.step || !t.moved) {
#if defined(BF_DEV_TOOLS)
      if (final && tree == g_dev_followed_tree.load(std::memory_order_relaxed) && t.published_frame + 1 == frame)
        g_dev_unpublished.fetch_add(1, std::memory_order_relaxed);
#endif
      continue;
    }
    Score(t.base, frame);
    const bool own = REXCVAR_GET(bf_smooth_motion_predict) && InVehicleView(frame) && t.base.own_score >= 2;
    const double tt = f + (own ? 1.0 : 0.0);
#if defined(BF_DEV_TOOLS)
    if (final && tree == g_dev_followed_tree.load(std::memory_order_relaxed) && own != t.was_own)
      g_dev_flips.fetch_add(1, std::memory_order_relaxed);
#endif
    if (final) t.was_own = own, t.published_frame = frame;
#if defined(BF_DEV_TOOLS)
    // BF_DEV_TREELOG=1: a moving tree whose base is within 15 m of the rendered camera, each frame: its base as written,
    // as the game had it, and the camera; 2400 lines (in BF_DEV_MOTION's form, for analyze_motion.py). BF_DEV_TREELOG=<n>
    // (over 1): only a tree of n parts (a Corsair is 57, a Sherman 56, a jeep 37: runs planelist1, treelist1).
    {
      static const bool on = std::getenv("BF_DEV_TREELOG") != nullptr;
      static const uint32_t want_parts = [] {  // BF_DEV_TREELOG=-1: a one part tree (the plane's canopy?)
        const char* v = std::getenv("BF_DEV_TREELOG");
        const int n = v ? std::atoi(v) : 0;
        return n > 1 ? uint32_t(n) : n < 0 ? uint32_t(-n) : 0u;
      }();
      static uint32_t followed = 0;
      static uint64_t followed_frame = 0;
      static int lines = 0;
      if (final && on && lines < 2400 && (!want_parts || t.count - 1 == want_parts)) {
        const float cam[3] = {bf1943::g_dev_view_pos[0].load(), bf1943::g_dev_view_pos[1].load(),
                              bf1943::g_dev_view_pos[2].load()};
        double d2 = 0;
        for (int c = 0; c < 3; ++c) d2 += (t.cur[0].m[3][c] - cam[c]) * (t.cur[0].m[3][c] - cam[c]);
        if ((followed != tree && (!followed || frame - followed_frame > 30) && d2 < 225) || followed == tree) {
          if (followed != tree) REXLOG_INFO("BF motion: following tree {:08X} ({} parts)", tree, t.count - 1);
          followed = tree, followed_frame = frame;
          g_dev_followed_tree.store(tree, std::memory_order_relaxed);
          Xf shown_base;
          if (Same(t.prev[0], t.cur[0]) || !Blend(t.prev[0], t.cur[0], tt, shown_base)) shown_base = t.cur[0];
          ++lines;
          REXLOG_INFO("BF motion: frame {} f {:.3f} prev {:.3f} {:.3f} {:.3f} cur {:.3f} {:.3f} {:.3f} shown {:.3f} {:.3f} "
                      "{:.3f} camera {:.3f} {:.3f} {:.3f}",
                      frame, tt, t.prev[0].m[3][0], t.prev[0].m[3][1], t.prev[0].m[3][2], t.cur[0].m[3][0],
                      t.cur[0].m[3][1], t.cur[0].m[3][2], shown_base.m[3][0], shown_base.m[3][1], shown_base.m[3][2],
                      cam[0], cam[1], cam[2]);
        }
      }
    }
#endif
    // The base's move from its tick value to the one shown: a part that cannot be blended rides on it.
    Xf base_move{};
    bool have_move = false;
    {
      Xf shown_base, inverse;
      if (!Same(t.prev[0], t.cur[0]) && Blend(t.prev[0], t.cur[0], tt, shown_base) && Inverse(t.cur[0], inverse))
        base_move = Mul(inverse, shown_base), have_move = true;
    }
    t.spun = 0;
    for (uint32_t i = 0; i < t.count; ++i) {
      uint8_t* at = base + t.worlds + i * 64;
      Xf out;
      if (Same(t.prev[i], t.cur[i])) {
        out = t.cur[i];
      } else if (!Blend(t.prev[i], t.cur[i], tt, out)) {
        // Turned too far in one tick to blend (a propeller, a wheel at speed: runs planeseek1, jeepfinal1): its pose on
        // the vehicle stays the tick's, carried by the base's move; left at the tick, it sat a tick's travel away from
        // its blended body.
        if (have_move) out = Mul(t.cur[i], base_move);
        else if (!Blend(t.prev[i], t.cur[i], tt, out, true, false)) out = t.cur[i];
        ++t.spun;
      }
      const Xf now_xf = LoadXf(at);
      if (!Same(now_xf, t.cur[i]) && !(t.published && Same(now_xf, t.shown[i]))) {  // the game changed it
#if defined(BF_DEV_TOOLS)
        if (tree == g_dev_followed_tree.load(std::memory_order_relaxed))
          g_dev_kept.fetch_add(1, std::memory_order_relaxed);
#endif
        continue;
      }
      for (int r = 0; r < 4; ++r) out.pad[r] = now_xf.pad[r];
#if defined(BF_DEV_TOOLS)
      // BF_DEV_PARTDUMP=<seconds>: that long into a vehicle view, 24 frames of every part within 2.5 m of the camera:
      // the tree, the part, the game's last two tick positions, what is written and its rows' lengths and handedness.
      {
        static const double at_s = [] {
          const char* v = std::getenv("BF_DEV_PARTDUMP");
          return v && *v ? std::atof(v) : -1.0;
        }();
        static Clock::time_point since{};
        static uint64_t first = 0;
        if (final && at_s >= 0) {
          if (!InVehicleView(frame)) since = {}, first = 0;
          else if (since == Clock::time_point{}) since = Clock::now();
          else if (!first && std::chrono::duration<double>(Clock::now() - since).count() >= at_s) first = frame;
          if (first && frame < first + 24) {
            double d2 = 0;
            for (int c = 0; c < 3; ++c)
              d2 += (t.cur[i].m[3][c] - bf1943::g_dev_view_pos[c].load()) * (t.cur[i].m[3][c] - bf1943::g_dev_view_pos[c].load());
            if (d2 < 2.5 * 2.5) {
              Parts pc{};
              const bool ok = Split(t.cur[i], pc);
              REXLOG_INFO("BF part: frame {} f {:.3f} tree {:08X} ({} parts) part {} at {:.2f} m: prev {:.4f} {:.4f} {:.4f} "
                          "cur {:.4f} {:.4f} {:.4f} written {:.4f} {:.4f} {:.4f}; rows {:.3f} {:.3f} {:.3f}{}{}, {}",
                          frame, tt, tree, t.count - 1, i, std::sqrt(d2), t.prev[i].m[3][0], t.prev[i].m[3][1],
                          t.prev[i].m[3][2], t.cur[i].m[3][0], t.cur[i].m[3][1], t.cur[i].m[3][2], out.m[3][0],
                          out.m[3][1], out.m[3][2], ok ? pc.scale[0] : 0.0, ok ? pc.scale[1] : 0.0,
                          ok ? pc.scale[2] : 0.0, ok && pc.mirrored ? " mirrored" : "", ok ? "" : " (no rotation)",
                          Same(t.prev[i], t.cur[i]) ? "still" : "moved");
            }
          }
        }
      }
#endif
      StoreXf(at, out);
      t.shown[i] = LoadXf(at);
    }
    t.published = true;
  }
  g_trees_window.store(1, std::memory_order_relaxed);
#if defined(BF_DEV_TOOLS) && defined(_WIN32)
  if (final) DevNearFind(base, frame);
#endif
  if (!final) return;
#if defined(BF_DEV_TOOLS)
  {
    // BF_DEV_PARTLOG=<parts>:<a>,<b>: each frame in a vehicle view, parts a and b of the tree of that many parts nearest
    // the camera, as written and as the game had them (cur), and the frame's view (its camera position, the view's
    // rows); 900 lines (the tank's vision slit against the camera).
    static int want_parts = -1, part_a = 0, part_b = 0;
    if (want_parts < 0) {
      want_parts = 0;
      if (const char* v = std::getenv("BF_DEV_PARTLOG"); v && *v)
        std::sscanf(v, "%d:%d,%d", &want_parts, &part_a, &part_b);
    }
    static int lines = 0;
    if (want_parts > 0 && lines < 900 && InVehicleView(frame)) {
      const float cam[3] = {bf1943::g_dev_view_pos[0].load(), bf1943::g_dev_view_pos[1].load(),
                            bf1943::g_dev_view_pos[2].load()};
      const TreeTrack* best = nullptr;
      double best_d2 = 1e9;
      for (auto& [tree, t] : g_trees) {
        if (!t.have || int(t.count) - 1 != want_parts) continue;
        double d2 = 0;
        for (int c = 0; c < 3; ++c) d2 += (t.cur[0].m[3][c] - cam[c]) * (t.cur[0].m[3][c] - cam[c]);
        if (d2 < best_d2) best_d2 = d2, best = &t;
      }
      if (best && best_d2 < 100 && part_a < int(best->count) && part_b < int(best->count)) {
        ++lines;
        const Xf a = LoadXf(base + best->worlds + part_a * 64), b = LoadXf(base + best->worlds + part_b * 64);
        const Xf& ca = best->cur[part_a];
        const Xf& cb = best->cur[part_b];
        REXLOG_INFO("BF partlog: frame {} f {:.3f} tick {} view {:.4f} {:.4f} {:.4f} a {:.4f} {:.4f} {:.4f} (cur {:.4f} {:.4f} "
                    "{:.4f}) fwd {:.4f} {:.4f} {:.4f} b {:.4f} {:.4f} {:.4f} (cur {:.4f} {:.4f} {:.4f}) fwd {:.4f} {:.4f} {:.4f}",
                    frame, bf1943::FramePhase(), bf1943::ClientTicks(), cam[0], cam[1], cam[2], a.m[3][0], a.m[3][1],
                    a.m[3][2], ca.m[3][0], ca.m[3][1], ca.m[3][2], a.m[2][0], a.m[2][1], a.m[2][2], b.m[3][0], b.m[3][1],
                    b.m[3][2], cb.m[3][0], cb.m[3][1], cb.m[3][2], b.m[2][0], b.m[2][1], b.m[2][2]);
      }
    }
  }
  {
    static const double at_s = [] {
      const char* v = std::getenv("BF_DEV_NEARPARTS");
      return v && *v ? std::atof(v) : -1.0;
    }();
    static Clock::time_point since{};
    static int dumps = 0;
    if (at_s >= 0 && dumps < 3) {
      if (!InVehicleView(frame) || !g.seat_entity || !g.seat.step || bf1943::LocalOnFoot()) since = {};
      else if (since == Clock::time_point{}) {
        since = Clock::now();
        REXLOG_INFO("BF nearpart: in a vehicle view");
      }
      else if (std::chrono::duration<double>(Clock::now() - since).count() >= at_s + dumps * 0.5) {
        ++dumps;
        const float cam[3] = {bf1943::g_dev_view_pos[0].load(), bf1943::g_dev_view_pos[1].load(),
                              bf1943::g_dev_view_pos[2].load()};
        for (auto& [tree, t] : g_trees) {
          if (!t.have) continue;
          for (uint32_t i = 0; i < t.count; ++i) {
            const Xf now_xf = LoadXf(base + t.worlds + i * 64);
            double d2 = 0;
            for (int c = 0; c < 3; ++c) d2 += (now_xf.m[3][c] - cam[c]) * (now_xf.m[3][c] - cam[c]);
            if (d2 > 4.0 * 4.0 && t.count - 1 != 57) continue;  // every part of a Corsair
            REXLOG_INFO("BF nearpart {}: tree {:08X} ({} parts, {}, {}) part {} at {:.3f} m: [{:.3f} {:.3f} {:.3f}] "
                        "[{:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f}] cur {:.3f} {:.3f} {:.3f}",
                        dumps, tree, t.count - 1, t.moved ? "moved" : "still", t.published ? "written" : "the game's", i,
                        std::sqrt(d2), now_xf.m[0][0], now_xf.m[0][1], now_xf.m[0][2], now_xf.m[1][0], now_xf.m[1][1],
                        now_xf.m[1][2], now_xf.m[2][0], now_xf.m[2][1], now_xf.m[2][2], now_xf.m[3][0], now_xf.m[3][1],
                        now_xf.m[3][2], t.cur[i].m[3][0], t.cur[i].m[3][1], t.cur[i].m[3][2]);
          }
        }
      }
    }
  }
#endif
#if defined(BF_DEV_TOOLS)
  using bf1943::g_dev_view_pos;
  // BF_DEV_TREELOG: every 270 frames, the trees within 10 m of the camera: parts, distance, last step, how written.
  if (frame % 270 == 0 && std::getenv("BF_DEV_TREELOG")) {
    const float cam[3] = {g_dev_view_pos[0].load(), g_dev_view_pos[1].load(), g_dev_view_pos[2].load()};
    std::string list;
    int shown = 0;
    for (auto& [tree, t] : g_trees) {
      if (!t.have) continue;
      double d2 = 0, step = 0;
      for (int c = 0; c < 3; ++c) {
        d2 += (t.cur[0].m[3][c] - cam[c]) * (t.cur[0].m[3][c] - cam[c]);
        step = std::max(step, double(std::abs(t.cur[0].m[3][c] - t.prev[0].m[3][c])));
      }
      if (d2 > 100 || ++shown > 12) continue;
      const bool own = InVehicleView(frame) && t.base.own_score >= 2;
      list += fmt::format(" [{:08X} {} parts {:.1f} m step {:.3f} score {} {}{}]", tree, t.count - 1, std::sqrt(d2),
                          step, t.base.own_score, !t.published ? "the game's" : own ? "ahead" : "blended",
                          t.spun ? fmt::format(", {} parts carried", t.spun) : std::string());
    }
    double seat_step = 0;
    for (int c = 0; c < 3; ++c) seat_step = std::max(seat_step, double(std::abs(g.seat.cur.m[3][c] - g.seat.prev.m[3][c])));
    REXLOG_INFO("BF motion: trees near the camera (seat {:08X} at {:.2f} {:.2f} {:.2f}, step {:.3f}, tick step {}, in a "
                "vehicle view {}):{}", g.seat_entity, g.seat.cur.m[3][0], g.seat.cur.m[3][1], g.seat.cur.m[3][2], seat_step,
                g.seat.step, InVehicleView(frame), list);
  }
#endif
#if defined(BF_DEV_TOOLS)
  // BF_DEV_TREELIST=1: every 300 frames, every tree within 80 m of the camera, nearest first: its part count and
  // distance (which count is which vehicle: the seeker's BF_SEEK_PARTS). BF_DEV_TREELIST=all: every tree the game
  // updates, however far (where a map's vehicles stand), up to 64.
  if (frame % 300 == 0 && std::getenv("BF_DEV_TREELIST")) {
    static const bool all = !std::strcmp(std::getenv("BF_DEV_TREELIST"), "all");
    const float cam[3] = {g_dev_view_pos[0].load(), g_dev_view_pos[1].load(), g_dev_view_pos[2].load()};
    std::vector<std::pair<double, std::string>> listed;
    for (auto& [tree, t] : g_trees) {
      if (!t.have) continue;
      double d2 = 0;
      for (int c = 0; c < 3; ++c) d2 += (t.cur[0].m[3][c] - cam[c]) * (t.cur[0].m[3][c] - cam[c]);
      if (d2 > 80.0 * 80.0 && !all) continue;
      listed.emplace_back(std::sqrt(d2), fmt::format(" [{} parts {:.1f} m at {:.1f} {:.1f} {:.1f}{}]", t.count - 1,
                                                   std::sqrt(d2), t.cur[0].m[3][0], t.cur[0].m[3][1], t.cur[0].m[3][2],
                                                   t.moved ? " moving" : ""));
    }
    std::sort(listed.begin(), listed.end());
    std::string list;
    for (size_t i = 0; i < listed.size() && i < (all ? 64u : 24u); ++i) list += listed[i].second;
    REXLOG_INFO("BF motion: trees within {} ({} known): {}", all ? "the map" : "80 m", g_trees.size(), list);
  }
#endif
  if (frame % 600 == 0)  // forget trees not updated for 600 frames
    for (auto it = g_trees.begin(); it != g_trees.end();)
      it = frame - it->second.frame > 600 && !it->second.published ? g_trees.erase(it) : ++it;
}
}  // namespace

namespace bf1943 {
// Around the camera manager's frame (vehicle_camera.cpp). It builds a vehicle's camera from the vehicle's own parts (the
// Corsair's first person camera is part 53 of its tree, at the camera: run canopyparts), so while it runs the trees
// hold the game's values; written blended before it, the camera was built from our values and then carried on again
// by our camera blend (twice: the first person camera off its mount by the frame's share of the vehicle's step, the
// Corsair's canopy out of sight at take-off, run vplanenopred: TekRantGaming's "shaking the camera around violently"). After
// it, the frame's last writing of the trees, for the renderer.
void TickBlendTreesForManager(uint8_t* base) {
  std::lock_guard lock(g.mutex);
#if defined(BF_DEV_TOOLS)
  static const bool off = std::getenv("BF_DEV_MGR_TREES") != nullptr;  // the trees left ours for the manager (to compare)
  if (off) return;
#endif
  RestoreTreesLocked(base);
}
void TickBlendPublishTrees(uint8_t* base) {
  std::lock_guard lock(g.mutex);
  PublishTreesLocked(base, true);
}
}  // namespace bf1943

#if defined(BF_DEV_TOOLS)
namespace bf1943 {
void DevMarkCamera() { DevMark("cam"); }
void DevMarkNamed(const char* what) { DevMark(what); }
}  // namespace bf1943
#endif

namespace bf1943 {
// The seeker (auto_join.cpp, BF_SEEK_PARTS): the nearest tree with that many parts to `from` (x, y, z), its base's
// position in `out`; false when the game updates none.
bool TickBlendNearestTree(uint32_t parts, const float from[3], float out[3], double* dist) {
  std::lock_guard lock(g.mutex);
  double best = -1;
  for (auto& [tree, t] : g_trees) {
    if (!t.have || t.count != parts + 1) continue;
    double d2 = 0;
    for (int c = 0; c < 3; ++c) d2 += (t.cur[0].m[3][c] - from[c]) * (t.cur[0].m[3][c] - from[c]);
    if (best >= 0 && d2 >= best) continue;
    best = d2;
    for (int c = 0; c < 3; ++c) out[c] = t.cur[0].m[3][c];
  }
  if (best < 0) return false;
  if (dist) *dist = std::sqrt(best);
  return true;
}
}  // namespace bf1943
#if defined(BF_DEV_TOOLS)
// BF_DEV_TREEWINDOW=1: the renderer's per frame reader of the trees (sub_825C1508, its own thread, runs vscan3 and
// vscan6): how many of its calls come while our values are written, and how many while the game's are back.
REX_EXTERN(__imp__sub_825C1508);
std::atomic<uint64_t> g_dev_getter_main{0}, g_dev_getter_main_ours{0}, g_dev_getter_other{0}, g_dev_getter_other_ours{0};
REX_HOOK_RAW(sub_825C1508) {
  DevMark("render");
  static const bool on = std::getenv("BF_DEV_TREEWINDOW") != nullptr;
  if (on) {
    static std::atomic<uint64_t> in{0}, out{0}, ours{0}, games{0}, other{0};
    (g_trees_window.load(std::memory_order_relaxed) ? in : out).fetch_add(1, std::memory_order_relaxed);
    if (const uint32_t tree = g_dev_followed_tree.load(std::memory_order_relaxed)) {
      std::lock_guard lock(g.mutex);
      auto it = g_trees.find(tree);
      if (it != g_trees.end() && it->second.have) {
        const Xf now_xf = LoadXf(base + it->second.worlds);
        (it->second.published && Same(now_xf, it->second.shown[0]) ? ours
         : Same(now_xf, it->second.cur[0])                          ? games
                                                                     : other)
            .fetch_add(1, std::memory_order_relaxed);
      }
    }
    static std::atomic<int64_t> at{0};
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    int64_t was = at.load();
    if (now - was > 2000 && at.compare_exchange_strong(was, now))
      REXLOG_INFO("BF motion: renderer reads {} in our window, {} out; the followed tree then ours {}, the game's {}, "
                  "other {}; publishes: {} parts left as the game's, {} ahead or behind changes, {} frames unwritten; "
                  "{} world updates outside the tick ({} of it); camera getter: main thread {} ({} ours), others {} "
                  "({} ours)",
                  in.exchange(0), out.exchange(0), ours.exchange(0), games.exchange(0), other.exchange(0),
                  g_dev_kept.exchange(0), g_dev_flips.exchange(0), g_dev_unpublished.exchange(0),
                  g_dev_outside.exchange(0), g_dev_outside_followed.exchange(0), g_dev_getter_main.exchange(0),
                  g_dev_getter_main_ours.exchange(0), g_dev_getter_other.exchange(0), g_dev_getter_other_ours.exchange(0));
  }
  __imp__sub_825C1508(ctx, base);
}

// The camera manager's getter (sub_82294708(manager, out): copies the active camera's transform, or +0x50, to out):
// who reads the camera, and what they got: ours (the blended camera written in the manager), the manager's own, or
// another (the active camera object's). Per caller (lr) and thread, every 2 s with the line above.
REX_EXTERN(__imp__sub_82294708);
REX_HOOK_RAW(sub_82294708) {
  static const bool on = std::getenv("BF_DEV_TREEWINDOW") != nullptr;
  const uint32_t manager = ctx.r3.u32, out = ctx.r4.u32, lr = uint32_t(ctx.lr);
  __imp__sub_82294708(ctx, base);
  if (!on || !out || !manager) return;
  const bool main = std::this_thread::get_id() == g_dev_main_thread;
  (main ? g_dev_getter_main : g_dev_getter_other).fetch_add(1, std::memory_order_relaxed);
  int got;  // 0 ours, 1 the manager's own +0x50, 2 another
  {
    std::lock_guard lock(g.mutex);
    if (g.published && !std::memcmp(base + out, g.wrote[0], 48)) got = 0;
    else if (!std::memcmp(base + out, base + manager + 0x50, 48)) got = 1;
    else got = 2;
    if (!got) (main ? g_dev_getter_main_ours : g_dev_getter_other_ours).fetch_add(1, std::memory_order_relaxed);
    static std::map<uint64_t, std::array<uint64_t, 3>> by;  // (lr, main) -> counts of each kind
    ++by[uint64_t(lr) << 1 | (main ? 1 : 0)][got];
    static auto at = Clock::now();
    if (Clock::now() - at > std::chrono::seconds(6) && InVehicleView(g_frame.load(std::memory_order_relaxed))) {
      at = Clock::now();
      std::string list;
      for (auto& [k, c] : by)
        list += fmt::format(" [{:08X} {} ours {} own {} other {}]", uint32_t(k >> 1), k & 1 ? "main" : "other", c[0],
                            c[1], c[2]);
      REXLOG_INFO("BF motion: camera getter callers:{}", list);
      by.clear();
    }
  }
}
#endif

// The client's update (sub_823961E8(client, frame time, ...)): the main thread's whole frame (run tljeep2), with the 30 Hz
// ticks (vehicle_camera.cpp's sub_82380600 hook), the skeletons and the camera manager inside it. At its start the
// frame's phase is fixed and the trees written for it.
REX_EXTERN(__imp__sub_823961E8);
REX_HOOK_RAW(sub_823961E8) {
#if defined(BF_DEV_TOOLS)
  DevMark("upd+");
#endif
  {
    std::lock_guard lock(g.mutex);
    g_frame_phase.store(bf1943::TickBlendPhase(), std::memory_order_relaxed);
    PublishTreesLocked(base, false);
    PublishCameraEstimateLocked(base);
  }
#if defined(BF_DEV_TOOLS)
  if (!bf1943::DevClientUpdate(ctx, base))
#endif
    __imp__sub_823961E8(ctx, base);
#if defined(BF_DEV_TOOLS)
  DevMark("upd-");
#endif
  g_updates.fetch_add(1, std::memory_order_relaxed);
}

REX_EXTERN(__imp__sub_822AE5D0);
REX_HOOK_RAW(sub_822AE5D0) {
  const uint32_t component = ctx.r3.u32;
  __imp__sub_822AE5D0(ctx, base);
  if (component && bf1943::g_in_client_tick.load(std::memory_order_relaxed)) {
    std::lock_guard lock(g.mutex);
    NoteTreeUpdated(Load32(base + component + 44));
  }
#if defined(BF_DEV_TOOLS)
  else if (component) {
    g_dev_outside.fetch_add(1, std::memory_order_relaxed);
    if (Load32(base + component + 44) == g_dev_followed_tree.load(std::memory_order_relaxed))
      g_dev_outside_followed.fetch_add(1, std::memory_order_relaxed);
  }
#endif
}

namespace bf1943 {
// Around the camera manager's frame in a vehicle (vehicle_camera.cpp). The camera handed to the view is blended, but
// the game reads the camera from the manager too (its getter sub_82294708 has 16 callers: effects, sound, the first
// person parts of a vehicle such as a tank's vision slit, which stayed on the tick camera and slid against the view:
// run mot9). So after the manager's frame its own two transforms (+0x50, +0x90) hold the blended camera for the rest
// of the frame, and before its next frame the game's values are put back (unless the game wrote them meanwhile), so
// nothing of ours stays in its state.
void TickBlendCameraRestore(uint8_t* base) {
  std::lock_guard lock(g.mutex);
  RestoreCamera(base);
}
void TickBlendCameraPublish(uint8_t* base, uint32_t manager) {
  std::lock_guard lock(g.mutex);
  if (!manager || !g.camera_shown_valid || !REXCVAR_GET(bf_smooth_motion)) return;
#if defined(BF_DEV_TOOLS)
  static const bool off = std::getenv("BF_DEV_NO_CAMPUBLISH") != nullptr;  // to compare
  if (off) return;
  {  // BF_DEV_CAMSLOTS=1: every 2 s, what the game holds at +0x50 and +0x90 and what is written
    static const bool slots = std::getenv("BF_DEV_CAMSLOTS") != nullptr;
    static auto at = Clock::now();
    // BF_DEV_CAMSLOTS=2: 60 frames in a row from 3 s into a seat: the frame, the tick, the fourth words of +0x50's rows.
    static int run = 0;
    static Clock::time_point seated{};
    if (slots && std::getenv("BF_DEV_CAMSLOTS")[0] == '2' && run < 60) {
      if (!g.seat_entity || bf1943::LocalOnFoot()) seated = {};
      else if (seated == Clock::time_point{}) seated = Clock::now();
      else if (Clock::now() - seated > std::chrono::seconds(3)) {
        ++run;
        const Xf a = LoadXf(base + manager + 0x50);
        REXLOG_INFO("BF camslot frame {} tick {}: +0x50 words {:08X} {:08X} {:08X} {:08X} pos {:.4f} {:.4f} {:.4f}",
                    g_frame.load(), ClientTicks(), a.pad[0], a.pad[1], a.pad[2], a.pad[3], a.m[3][0], a.m[3][1],
                    a.m[3][2]);
      }
    }
    if (slots && Clock::now() - at > std::chrono::seconds(2)) {
      at = Clock::now();
      const Xf a = LoadXf(base + manager + 0x50), b = LoadXf(base + manager + 0x90), w = g.camera_shown;
      auto rows = [](const Xf& x) {
        return fmt::format("[{:.4f} {:.4f} {:.4f} {:08X}] [{:.4f} {:.4f} {:.4f} {:08X}] [{:.4f} {:.4f} {:.4f} {:08X}] "
                           "[{:.3f} {:.3f} {:.3f} {:08X}]",
                           x.m[0][0], x.m[0][1], x.m[0][2], x.pad[0], x.m[1][0], x.m[1][1], x.m[1][2], x.pad[1],
                           x.m[2][0], x.m[2][1], x.m[2][2], x.pad[2], x.m[3][0], x.m[3][1], x.m[3][2], x.pad[3]);
      };
      REXLOG_INFO("BF camslots: +0x50 {}; +0x90 {}; written {}", rows(a), rows(b), rows(w));
    }
  }
#endif
  g.camera_shown_valid = false;
#if defined(BF_DEV_TOOLS)
  static const int only_slot = [] {  // BF_DEV_CAMSLOT=50 or 90: only that slot written (which one the parts read)
    const char* v = std::getenv("BF_DEV_CAMSLOT");
    return v && *v ? std::atoi(v) : 0;
  }();
#endif
  for (int i = 0; i < 2; ++i) {
    uint8_t* at = base + manager + (i ? 0x90 : 0x50);
    std::memcpy(g.saved[i], at, 64);
#if defined(BF_DEV_TOOLS)
    if (only_slot && only_slot != (i ? 90 : 50)) {
      std::memcpy(g.wrote[i], at, 64);
      continue;
    }
#endif
    Xf x = g.camera_shown;
    for (int r = 0; r < 4; ++r) x.pad[r] = Load32(at + r * 16 + 12);  // the game's fourth words kept
    StoreXf(at, x);
    std::memcpy(g.wrote[i], at, 64);
  }
  g.published = manager;
  g.manager = manager;
}

// Once a frame (VdSwap, frame_stats.cpp).
void TickBlendFrame() {
  const auto now = Clock::now();
  // A frame the game ran no tick in had its own time since the tick under one tick: ours is capped to that, so the
  // frames between ticks never all sit at the end (a start a frame late carried on: run mot1, 0.53 and 1.0 by turns).
  // After the frame's ticks the game's own remainder is under a tick (it runs ticks until it is), with or without a
  // tick this frame; ours never more. Below 30 FPS a frame holds two ticks at times, and a long frame's time can be
  // dropped by the game: capping here (no longer resetting in the tick) keeps the phase the game's (run walkslow2:
  // at 24 FPS the reset at two ticks put frames back a tick every fifth frame).
  double phase = g_phase.load(std::memory_order_relaxed);
  g_ticked.exchange(false, std::memory_order_relaxed);
  if (phase > kTick * 0.98) phase = kTick * 0.98;
  if (g_frame_at != Clock::time_point{})
    phase += std::min(0.25, std::chrono::duration<double>(now - g_frame_at).count());
  g_phase.store(phase, std::memory_order_relaxed);
  g_frame_at = now;
  const uint64_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(BF_DEV_TOOLS)
  DevMark("vdswap");
  {
    static const double at = [] {
      const char* v = std::getenv("BF_DEV_TIMELINE");
      return v && *v ? std::atof(v) : -1.0;
    }();
    static Clock::time_point since{};
    if (at >= 0 && g_dev_timeline_state.load() == 0) {
      if (!InVehicleView(frame)) since = {};
      else if (since == Clock::time_point{}) since = now;
      else if (std::chrono::duration<double>(now - since).count() >= at) {
        g_dev_timeline_end = frame + 120;
        g_dev_timeline_state.store(1);
      }
    } else if (g_dev_timeline_state.load() == 1 && frame >= g_dev_timeline_end) {
      g_dev_timeline_state.store(2);
      std::lock_guard lock(g_dev_timeline_mutex);
      const int64_t t0 = g_dev_timeline.empty() ? 0 : g_dev_timeline.front().us;
      std::string line;
      for (const DevEvent& e : g_dev_timeline) {
        line += fmt::format(" {}:{}@{:04X}/{}", (e.us - t0), e.what, e.thread, e.frame);
        if (line.size() > 1500) {
          REXLOG_INFO("BF timeline:{}", line);
          line.clear();
        }
      }
      if (!line.empty()) REXLOG_INFO("BF timeline:{}", line);
    }
  }
#endif
  if (frame % 600 == 0) {  // forget roots not drawn for 600 frames
    std::lock_guard lock(g.mutex);
    for (auto it = g.tracks.begin(); it != g.tracks.end();) it = frame - it->second.frame > 600 ? g.tracks.erase(it) : ++it;
  }
  if (frame % 600 == 300) {  // and bones
    for (Stripe& st : g_stripes) {
      std::lock_guard lock(st.mutex);
      for (auto it = st.bones.begin(); it != st.bones.end();) it = frame - it->second.frame > 600 ? st.bones.erase(it) : ++it;
    }
    for (MatrixStripe& st : g_matrix_stripes) {
      std::lock_guard lock(st.mutex);
      for (auto it = st.bones.begin(); it != st.bones.end();) it = frame - it->second.frame > 600 ? st.bones.erase(it) : ++it;
    }
  }
}

// After each client tick (vehicle_camera.cpp's sub_82380600 hook).
void TickBlendTick() {
  const double p = g_phase.load(std::memory_order_relaxed);
  g_phase.store(std::max(0.0, p - kTick), std::memory_order_relaxed);  // TickBlendFrame caps what is left over
  g_ticked.store(true, std::memory_order_relaxed);
}

// The game's time since its last tick as a part of a tick, 0 to 1.
double TickBlendPhase() { return std::clamp(g_phase.load(std::memory_order_relaxed) / kTick, 0.0, 1.0); }
// The phase everything in this frame is placed with: TickBlendPhase taken once, right after the client's update. The
// live one moves at VdSwap on the render thread, in the middle of the main thread's frame at times, and the trees, the
// skeletons on the worker threads and the camera must agree.
double FramePhase() {
  const double f = g_frame_phase.load(std::memory_order_relaxed);
  return f >= 0 ? f : TickBlendPhase();
}
}  // namespace bf1943

#if defined(BF_DEV_TOOLS)
// The skeletons' bone arrays lately placed (physical address, size, frame), for dev_matscan.cpp to leave out.
struct BoneArray {
  uint32_t start, size;
  uint64_t frame;
};
std::mutex g_bone_arrays_mutex;
std::vector<BoneArray> g_bone_arrays;
void DevNoteBoneArray(uint32_t start, uint32_t size) {
  const uint64_t frame = g_frame.load(std::memory_order_relaxed);
  std::lock_guard lock(g_bone_arrays_mutex);
  const uint32_t phys = start & 0x1FFFFFFF;
  for (BoneArray& b : g_bone_arrays)
    if (b.start == phys) {
      b.size = size, b.frame = frame;
      return;
    }
  g_bone_arrays.push_back({phys, size, frame});
}
namespace bf1943 {
// Whether a guest address is in a skeleton's bone array placed in the last 120 frames (any view of the page).
bool DevInBoneArray(uint32_t address) {
  const uint32_t phys = address & 0x1FFFFFFF;
  const uint64_t frame = g_frame.load(std::memory_order_relaxed);
  std::lock_guard lock(g_bone_arrays_mutex);
  for (const BoneArray& b : g_bone_arrays)
    if (frame - b.frame < 120 && phys >= b.start && phys < b.start + b.size) return true;
  return false;
}
}  // namespace bf1943

// BF_DEV_SKELDUMP=<seconds>: that long after the first skeleton update in a match, every skeleton update of the next
// two frames: its root and where that is (none: the update's identity), its distance from the rendered camera and from
// the origin, its first bone and count (r4, r5), and its bone array.
void DevSkeleton(PPCContext& ctx, const uint8_t* base, const Xf* root_xf) {
  static const double dump_at = [] {
    const char* v = std::getenv("BF_DEV_SKELDUMP");
    return v && *v ? std::atof(v) : -1.0;
  }();
  if (dump_at < 0) return;
  static Clock::time_point first{};
  static uint64_t dump_frame = 0;
  static std::mutex m;
  const auto now = Clock::now();
  std::lock_guard lock(m);
  if (first == Clock::time_point{}) first = now;
  const uint64_t frame_now = g_frame.load(std::memory_order_relaxed);
  if (!dump_frame && std::chrono::duration<double>(now - first).count() >= dump_at) dump_frame = frame_now;
  if (!dump_frame || frame_now < dump_frame || frame_now >= dump_frame + 2) return;
  const float cam[3] = {bf1943::g_dev_view_pos[0].load(), bf1943::g_dev_view_pos[1].load(),
                        bf1943::g_dev_view_pos[2].load()};
  const float zero[3] = {0, 0, 0};
  const float* pos = root_xf ? root_xf->m[3] : zero;
  const double dc = std::sqrt((pos[0] - cam[0]) * (pos[0] - cam[0]) + (pos[1] - cam[1]) * (pos[1] - cam[1]) +
                              (pos[2] - cam[2]) * (pos[2] - cam[2]));
  const double d0 = std::sqrt(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]);
  const uint32_t bones = ctx.r10.u32 ? Load32(base + ctx.r10.u32 + 4) : 0;
  REXLOG_INFO("BF skeleton: frame {} root {:08X} at {:.3f} {:.3f} {:.3f} ({:.2f} m from the camera, {:.2f} from the "
              "origin), bones {} to {}, array {:08X}, lr {:08X}",
              frame_now, ctx.r9.u32, pos[0], pos[1], pos[2], dc, d0, ctx.r4.u32, ctx.r4.u32 + ctx.r5.u32, bones,
              uint32_t(ctx.lr));
}
#endif

// A skeleton's update (sub_826D41B8; r9 the root transform, null for none).
REX_EXTERN(__imp__sub_826D41B8);
REX_HOOK_RAW(sub_826D41B8) {
  const uint32_t root = ctx.r9.u32;
  if (!REXCVAR_GET(bf_smooth_motion) || !bf1943::InMatch()) {
    __imp__sub_826D41B8(ctx, base);
    return;
  }
  if (!root) {  // no root (the update takes an identity): nothing to place, its bones are still looked after
#if defined(BF_DEV_TOOLS)
    DevSkeleton(ctx, base, nullptr);
#endif
    t_in_skeleton = true, t_root = 0, t_own = false;
    __imp__sub_826D41B8(ctx, base);
    t_in_skeleton = false;
    return;
  }
  const Xf now_xf = LoadXf(base + root);
  const uint64_t tick = bf1943::ClientTicks();
  const uint64_t frame = g_frame.load(std::memory_order_relaxed);
  // The skeleton is known by its first bone transform's address: its bone array (*(r10 + 4)) plus the first bone
  // (r4) times 64, as the update itself works it out. The root comes in a copy whose place changes (run mot9: the
  // same tank's root at five addresses 0xCD0 apart over a drive), so the root's own address could mix two objects.
  const uint32_t bones = ctx.r10.u32 ? Load32(base + ctx.r10.u32 + 4) : 0;
  const uint32_t key = bones ? bones + ctx.r4.u32 * 64 : root;
#if defined(BF_DEV_TOOLS)
  DevSkeleton(ctx, base, &now_xf);
  if (bones) DevNoteBoneArray(key, ctx.r5.u32 * 64);
#endif
  uint32_t copy = 0;
#if defined(BF_DEV_TOOLS)
  Xf find{};
  uint32_t find_scratch = 0;
#endif
  {
    std::lock_guard lock(g.mutex);
    Track& t = g.tracks[key];
    Xf out;
    const Plan plan = Follow(t, now_xf, tick, frame, kRootTolerance);
    Score(t, frame);
    t_own = REXCVAR_GET(bf_smooth_motion_predict) && InVehicleView(frame) && t.own_score >= 2;
#if defined(BF_DEV_TOOLS)
    {
      static const bool on = std::getenv("BF_DEV_TREEWINDOW") != nullptr;
      if (on) {
        g_dev_skel.fetch_add(1, std::memory_order_relaxed);
        if (tick != g_dev_publish_tick.load(std::memory_order_relaxed)) g_dev_skel_late.fetch_add(1, std::memory_order_relaxed);
        if (std::abs(bf1943::TickBlendPhase() - g_dev_publish_phase.load(std::memory_order_relaxed)) > 1e-6)
          g_dev_skel_phase.fetch_add(1, std::memory_order_relaxed);
        if (t.drifted) g_dev_skel_drift.fetch_add(1, std::memory_order_relaxed);
        double d2 = 0;
        for (int c = 0; c < 3; ++c)
          d2 += (now_xf.m[3][c] - bf1943::g_dev_view_pos[c].load()) * (now_xf.m[3][c] - bf1943::g_dev_view_pos[c].load());
        if (d2 < 100) {  // within 10 m of the camera (the driver, the riders)
          g_dev_skel_near.fetch_add(1, std::memory_order_relaxed);
          if (t_own) g_dev_skel_near_own.fetch_add(1, std::memory_order_relaxed);
          const auto n = Clock::now();
          if (n < t.moves_until || n < t.turns_until) g_dev_skel_near_held.fetch_add(1, std::memory_order_relaxed);
          // Which one it is: its distance from the seat and from the followed tree's base, its bones, how it changed.
          static std::atomic<int64_t> said{0};
          const int64_t ms2 = std::chrono::duration_cast<std::chrono::milliseconds>(n.time_since_epoch()).count();
          int64_t was2 = said.load();
          if (ms2 - was2 > 1000 && said.compare_exchange_strong(was2, ms2)) {
            double ds = 0, dt = -1;
            for (int c = 0; c < 3; ++c) ds += (now_xf.m[3][c] - g.seat.cur.m[3][c]) * (now_xf.m[3][c] - g.seat.cur.m[3][c]);
            if (auto it = g_trees.find(g_dev_followed_tree.load(std::memory_order_relaxed)); it != g_trees.end()) {
              dt = 0;
              for (int c = 0; c < 3; ++c)
                dt += (now_xf.m[3][c] - it->second.shown[0].m[3][c]) * (now_xf.m[3][c] - it->second.shown[0].m[3][c]);
              dt = std::sqrt(dt);
            }
            REXLOG_INFO("BF motion: near skeleton {:08X} ({} bones): {:.2f} m from the seat, {:.2f} from the followed "
                        "tree as shown; between ticks it moved {:.4f} and turned {:.4f} (tick step {:.4f}, {:.4f}); "
                        "score {}{}",
                        key, ctx.r5.u32, std::sqrt(ds), dt, t.drift_move, t.drift_turn, t.tick_move, t.tick_turn,
                        t.own_score, n < t.moves_until || n < t.turns_until ? ", held" : "");
          }
        }
        static std::atomic<int64_t> at{0};
        const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
        int64_t was = at.load();
        if (ms - was > 2000 && at.compare_exchange_strong(was, ms))
          REXLOG_INFO("BF motion: skeleton updates {}: after a later tick than the frame's publish {}, another phase {}, "
                      "changed between ticks {}; within 10 m {} (carried ahead {}, held unblended {})",
                      g_dev_skel.exchange(0), g_dev_skel_late.exchange(0), g_dev_skel_phase.exchange(0),
                      g_dev_skel_drift.exchange(0), g_dev_skel_near.exchange(0), g_dev_skel_near_own.exchange(0),
                      g_dev_skel_near_held.exchange(0));
      }
    }
#endif
    bool skip_rider = false;
#if defined(BF_DEV_TOOLS)
    {  // BF_DEV_NO_RIDERBLEND=1: skeletons within 3 m of the seat left as the game placed them (to compare)
      static const bool no_rider = std::getenv("BF_DEV_NO_RIDERBLEND") != nullptr;
      if (no_rider && g.seat_entity) {
        double ds = 0;
        for (int c = 0; c < 3; ++c) ds += (now_xf.m[3][c] - g.seat.cur.m[3][c]) * (now_xf.m[3][c] - g.seat.cur.m[3][c]);
        skip_rider = ds < 9;
      }
    }
#endif
    if (plan.any() && !skip_rider) {
      const double f = bf1943::FramePhase() + (t_own ? 1.0 : 0.0);
      if (Blend(t.prev, t.cur, f, out, plan.move, plan.turn)) {
        copy = Copy(base, out);
#if defined(BF_DEV_TOOLS)
        DevMotion(root, t.prev, t.cur, out, f, frame);
#endif
      }
    }
#if defined(BF_DEV_TOOLS)
    DevNear(root, t, plan, copy != 0, frame);
    if (t_own) find = t.cur, find_scratch = g.scratch;
#endif
  }
#if defined(BF_DEV_TOOLS) && defined(_WIN32)
  if (t_own) DevRootFind(base, find, root, find_scratch);
#endif
  if (copy) ctx.r9.u64 = copy;
#if defined(BF_DEV_TOOLS)
  if (g_dev_timeline_state.load(std::memory_order_relaxed) == 1) {
    double ds = 0;
    {
      std::lock_guard lock(g.mutex);
      for (int c = 0; c < 3; ++c) ds += (now_xf.m[3][c] - g.seat.cur.m[3][c]) * (now_xf.m[3][c] - g.seat.cur.m[3][c]);
    }
    if (ds < 9) DevMark(tick != g_dev_publish_tick.load(std::memory_order_relaxed) ? "skel-newtick" : "skel");
  }
  // BF_DEV_RIDERLOG=1: each skeleton update within 3 m of the seat (the driver, riders), in a vehicle view: the frame,
  // the tick, the phase, the root it is drawn with, and the followed tree's base as written then; 1500 lines (does the
  // driver move with the vehicle frame by frame?).
  {
    static const bool on = std::getenv("BF_DEV_RIDERLOG") != nullptr;
    static std::atomic<int> lines{0};
    if (on && lines.load(std::memory_order_relaxed) < 1500 && InVehicleView(frame)) {
      const Xf drawn = LoadXf(base + (copy ? copy : root));
      double ds = 0;
      std::lock_guard lock(g.mutex);
      for (int c = 0; c < 3; ++c) ds += (drawn.m[3][c] - g.seat.cur.m[3][c]) * (drawn.m[3][c] - g.seat.cur.m[3][c]);
      auto it = g_trees.find(g_dev_followed_tree.load(std::memory_order_relaxed));
      if (ds < 9 && it != g_trees.end() && it->second.have) {
        lines.fetch_add(1, std::memory_order_relaxed);
        DevMark(it->second.published && Same(LoadXf(base + it->second.worlds), it->second.shown[0]) ? "skel-ours"
                                                                                                    : "skel-game");
        const Xf tree_now = LoadXf(base + it->second.worlds);
        const Track& rt = g.tracks[key];
        REXLOG_INFO("BF rider: frame {} tick {} f {:.3f} skeleton {:08X} drawn {:.4f} {:.4f} {:.4f} {} tree {:.4f} {:.4f} "
                    "{:.4f} {} (own {}, score {}, step {}, jump {}, held {})",
                    frame, tick, bf1943::TickBlendPhase(), key, drawn.m[3][0], drawn.m[3][1], drawn.m[3][2],
                    copy ? "blended" : "the game's", tree_now.m[3][0], tree_now.m[3][1], tree_now.m[3][2],
                    it->second.published && Same(tree_now, it->second.shown[0]) ? "ours" : "the game's", t_own,
                    rt.own_score, rt.step, rt.jump, Clock::now() < rt.moves_until || Clock::now() < rt.turns_until);
      }
    }
  }
#endif
  t_in_skeleton = true;
  t_root = root;
  __imp__sub_826D41B8(ctx, base);
  t_in_skeleton = false;
  t_own = false;
}

#if defined(BF_DEV_TOOLS)
// BF_DEV_BONELOG=1 (with or without bf_smooth_bones): one bone within 1.2 m of the rendered camera followed (a first
// person arm or the gun), the one whose own pose moved most in a tick; each frame its world position and the camera's,
// whether it was blended, and the game's local position; 900 lines.
void DevBone(const uint8_t* base, uint32_t bone, const Pose& game, bool blended, double f) {
  static std::mutex m;
  static uint32_t followed = 0;
  static uint64_t followed_frame = 0, logged_frame = ~0ull;
  static int lines = 0;
  static std::unordered_map<uint32_t, Pose> last;  // each near bone's game pose at the last frame
  static uint32_t best = 0;
  static double best_move = 0;
  static uint64_t best_frame = 0;
  // BF_DEV_BONELOG=identity: only skeletons placed in their own space (the root the animation code's identity
  // transform at 0x82CA7410, a first person weapon's), measured there instead of against the camera.
  static const bool identity = [] {
    const char* v = std::getenv("BF_DEV_BONELOG");
    return v && !std::strcmp(v, "identity");
  }();
  if (identity && t_root != 0x82CA7410 && t_root != 0) return;
  std::lock_guard lock(m);
  if (lines >= 900) return;
  const uint64_t frame = g_frame.load(std::memory_order_relaxed);
  float cam[3] = {bf1943::g_dev_view_pos[0].load(), bf1943::g_dev_view_pos[1].load(),
                  bf1943::g_dev_view_pos[2].load()};
  if (identity) cam[0] = cam[1] = cam[2] = 0;
  const uint8_t* w = base + bone + 48;
  const float pos[3] = {BeF(w), BeF(w + 4), BeF(w + 8)};
  const double d2 = (pos[0] - cam[0]) * (pos[0] - cam[0]) + (pos[1] - cam[1]) * (pos[1] - cam[1]) +
                    (pos[2] - cam[2]) * (pos[2] - cam[2]);
  if (bone != followed) {
    static const double radius = [] {
      const char* v = std::getenv("BF_DEV_BONELOG_R");
      return v && *v ? std::atof(v) : 1.2;
    }();
    if (d2 > radius * radius) return;
    // pick the near bone whose own pose moves most (over a second of frames), when none is followed lately
    auto it = last.find(bone);
    if (it != last.end()) {
      double mv = 0;
      for (int i = 0; i < 3; ++i) mv += std::abs(double(game.v[i]) - it->second.v[i]);
      for (int i = 4; i < 8; ++i) mv += std::abs(double(game.v[i]) - it->second.v[i]);
      if (mv > best_move) best_move = mv, best = bone;
    }
    last[bone] = game;
    if (frame - best_frame > 60 && (!followed || frame - followed_frame > 60) && best) {
      followed = best, best_frame = frame, best_move = 0, last.clear();
      REXLOG_INFO("BF bones: following bone {:08X}", followed);
    }
    return;
  }
  followed_frame = frame;
  if (frame == logged_frame) return;
  logged_frame = frame;
  ++lines;
  REXLOG_INFO("BF bones: frame {} f {:.3f} blended {} world {:.4f} {:.4f} {:.4f} camera {:.4f} {:.4f} {:.4f} local {:.4f} "
              "{:.4f} {:.4f} ({})",
              frame, f, blended, pos[0], pos[1], pos[2], cam[0], cam[1], cam[2], game.v[0], game.v[1], game.v[2],
              t_bone_why);
}
#endif

// A matrix bone's local transform blended between the game's ticks: the address of a blended copy, or 0 to leave it.
uint32_t BlendMatrixBone(uint8_t* base, uint32_t local, uint32_t bone) {
  const Xf now_xf = LoadXf(base + local);
  const uint64_t tick = bf1943::ClientTicks();
  const uint64_t frame = g_frame.load(std::memory_order_relaxed);
  Xf out;
  bool use = false;
  {
    MatrixStripe& st = g_matrix_stripes[(bone >> 6) % kStripes];
    std::lock_guard lock(st.mutex);
    Track& t = st.bones[bone];
    const Plan plan = Follow(t, now_xf, tick, frame, kBoneTolerance);
    if (plan.any()) use = Blend(t.prev, t.cur, bf1943::FramePhase() + (t_own ? 1.0 : 0.0), out, plan.move, plan.turn);
    const auto now = Clock::now();
    t_bone_why = use ? "blended" : !t.step ? "no tick step" : t.jump ? "a jump" :
                 now < t.moves_until || now < t.turns_until ? "moves between ticks" : plan.any() ? "refused" : "still";
  }
  if (!use) return 0;
  uint32_t scratch = g_matrix_scratch.load(std::memory_order_acquire);
  if (!scratch) {
    std::lock_guard lock(g_pose_alloc);
    scratch = g_matrix_scratch.load(std::memory_order_relaxed);
    if (!scratch) {
      const uint32_t at = REX_KERNEL_MEMORY()->SystemHeapAlloc(kMatrixSlots * 64 + 64);
      scratch = at ? (at + 63) & ~63u : 0;
      g_matrix_scratch.store(scratch, std::memory_order_release);
    }
  }
  if (!scratch) return 0;
  const uint32_t slot = scratch + (g_matrix_slot.fetch_add(1, std::memory_order_relaxed) % kMatrixSlots) * 64;
  StoreXf(base + slot, out);
  return slot;
}

#if defined(BF_DEV_TOOLS)
// For BF_DEV_BONELOG: a matrix bone's local transform as a pose (its position, and its first row for the turn).
Pose MatrixPose(const uint8_t* base, uint32_t local) {
  const Xf x = LoadXf(base + local);
  return Pose{{x.m[3][0], x.m[3][1], x.m[3][2], 0, x.m[0][0], x.m[0][1], x.m[0][2], 0}};
}
#endif

REX_EXTERN(__imp__sub_826E24E8);
REX_HOOK_RAW(sub_826E24E8) {
  const uint32_t bone = ctx.r5.u32;
#if defined(BF_DEV_TOOLS)
  static const bool dev_log = std::getenv("BF_DEV_BONELOG") != nullptr;
  const Pose game = dev_log && t_in_skeleton && ctx.r4.u32 ? MatrixPose(base, ctx.r4.u32) : Pose{};
#endif
  bool blended = false;
  if (t_in_skeleton && REXCVAR_GET(bf_smooth_bones) && ctx.r4.u32 && bone)
    if (const uint32_t copy = BlendMatrixBone(base, ctx.r4.u32, bone)) ctx.r4.u64 = copy, blended = true;
  __imp__sub_826E24E8(ctx, base);
#if defined(BF_DEV_TOOLS)
  if (dev_log && t_in_skeleton && bone) DevBone(base, bone, game, blended, bf1943::TickBlendPhase());
#endif
}

REX_EXTERN(__imp__sub_826E2378);
REX_HOOK_RAW(sub_826E2378) {
  const uint32_t bone = ctx.r6.u32;
#if defined(BF_DEV_TOOLS)
  static const bool dev_log = std::getenv("BF_DEV_BONELOG") != nullptr;
  const Pose game = dev_log && t_in_skeleton && ctx.r5.u32 ? MatrixPose(base, ctx.r5.u32) : Pose{};
#endif
  bool blended = false;
  if (t_in_skeleton && REXCVAR_GET(bf_smooth_bones) && ctx.r5.u32 && bone)
    if (const uint32_t copy = BlendMatrixBone(base, ctx.r5.u32, bone)) ctx.r5.u64 = copy, blended = true;
  __imp__sub_826E2378(ctx, base);
#if defined(BF_DEV_TOOLS)
  if (dev_log && t_in_skeleton && bone) DevBone(base, bone, game, blended, bf1943::TickBlendPhase());
#endif
}

// A bone placed (sub_826E2220): its local position (r3) and rotation (r4) blended between the game's ticks.
REX_EXTERN(__imp__sub_826E2220);
REX_HOOK_RAW(sub_826E2220) {
#if defined(BF_DEV_TOOLS)
  static const bool dev_log = std::getenv("BF_DEV_BONELOG") != nullptr;
  if (dev_log && t_in_skeleton && ctx.r3.u32 && ctx.r4.u32 && !REXCVAR_GET(bf_smooth_bones)) {
    // the log without the blend: the game's own steps
    const uint32_t bone = ctx.r8.u32;
    const Pose p = LoadPose(base, ctx.r3.u32, ctx.r4.u32);
    __imp__sub_826E2220(ctx, base);
    DevBone(base, bone, p, false, 0);
    return;
  }
#endif
  if (!t_in_skeleton || !REXCVAR_GET(bf_smooth_bones) || !ctx.r3.u32 || !ctx.r4.u32) {
    __imp__sub_826E2220(ctx, base);
    return;
  }
  const uint32_t bone = ctx.r8.u32;
  const Pose now_pose = LoadPose(base, ctx.r3.u32, ctx.r4.u32);
  const uint64_t tick = bf1943::ClientTicks();
  Pose out;
  bool use = false;
  {
    Stripe& st = g_stripes[(bone >> 6) % kStripes];
    std::lock_guard lock(st.mutex);
    BoneTrack& t = st.bones[bone];
    if (!t.have) {
      t.cur = now_pose, t.have = true, t.seen_tick = tick;
    } else if (t.seen_tick != tick) {
      t.prev = t.cur, t.cur = now_pose;
      t.step = tick - t.seen_tick == 1;
      t.seen_tick = tick;
    } else if (!(now_pose == t.cur)) {
      // Changed between ticks: under a third of the change at the last tick (and 2e-4) it is noise, taken in quietly
      // (kBoneTolerance); more is a bone moved every frame, left as it is for 30 ticks.
      double drift = 0, step = 0;
      for (int i = 0; i < 8; ++i) {
        if (i == 3) continue;
        drift = std::max(drift, double(std::abs(now_pose.v[i] - t.cur.v[i])));
        step = std::max(step, double(std::abs(t.cur.v[i] - t.prev.v[i])));
      }
      if (drift > std::max(kBoneTolerance.floor, kBoneTolerance.share * step)) t.every_frame_until = tick + 30;
      t.cur = now_pose;
    }
    t.frame = g_frame.load(std::memory_order_relaxed);
    if (t.step && tick >= t.every_frame_until && !(t.prev == t.cur))
      use = BlendPose(t.prev, t.cur, bf1943::FramePhase() + (t_own ? 1.0 : 0.0), out);
    t_bone_why = use ? "blended" : !t.step ? "no tick step" : tick < t.every_frame_until ? "moves between ticks" :
                 t.prev == t.cur ? "still" : "refused";
  }
  if (use) {
    uint32_t scratch = g_pose_scratch.load(std::memory_order_acquire);
    if (!scratch) {
      std::lock_guard lock(g_pose_alloc);
      scratch = g_pose_scratch.load(std::memory_order_relaxed);
      if (!scratch) {
        const uint32_t at = REX_KERNEL_MEMORY()->SystemHeapAlloc(kPoseSlots * 32 + 64);
        scratch = at ? (at + 63) & ~63u : 0;
        g_pose_scratch.store(scratch, std::memory_order_release);
      }
    }
    if (scratch) {
      const uint32_t slot = scratch + (g_pose_slot.fetch_add(1, std::memory_order_relaxed) % kPoseSlots) * 32;
      for (int i = 0; i < 8; ++i) {
        uint32_t u;
        std::memcpy(&u, &out.v[i], 4);
        Store32(base + slot + i * 4, u);
      }
      ctx.r3.u64 = slot;
      ctx.r4.u64 = slot + 16;
    }
  }
  __imp__sub_826E2220(ctx, base);
#if defined(BF_DEV_TOOLS)
  if (dev_log) DevBone(base, bone, now_pose, use, bf1943::TickBlendPhase());
#endif
}

// The view's camera transform (sub_82583348(view, transform): copied in when it changed, the view marked for a new
// view matrix). (See also TickBlendViewWithoutManager below.) From the camera manager's frame (sub_82294A58, the call returning to 0x82294C28, the main view) while
// it runs for a vehicle: the camera blended between its last two ticks like the vehicle, so the two stay together.
REX_EXTERN(__imp__sub_82583348);
REX_HOOK_RAW(sub_82583348) {
  constexpr uint32_t kMainView = 0x82294C28;
#if defined(BF_DEV_TOOLS)
  {  // BF_DEV_VIEWCALLERS=1: every 3 s in a vehicle view, the callers (lr) of the view setter and their views
    static const bool on = std::getenv("BF_DEV_VIEWCALLERS") != nullptr;
    if (on) {
      static std::mutex m;
      static std::map<uint64_t, uint64_t> calls;  // lr << 32 | view -> calls
      static auto at = Clock::now();
      std::lock_guard lock(m);
      ++calls[uint64_t(uint32_t(ctx.lr)) << 32 | ctx.r3.u32];
      if (Clock::now() - at > std::chrono::seconds(3)) {
        at = Clock::now();
        std::string list;
        for (auto& [k, n] : calls) list += fmt::format(" [lr {:08X} view {:08X}: {}]", uint32_t(k >> 32), uint32_t(k), n);
        REXLOG_INFO("BF motion: view setter callers (vehicle view {}):{}", InVehicleView(g_frame.load()), list);
        calls.clear();
      }
    }
  }
#endif
#if defined(BF_DEV_TOOLS)
  if (uint32_t(ctx.lr) == kMainView && std::getenv("BF_DEV_MOTION")) {
    static uint64_t calls = 0, vehicle = 0;
    ++calls;
    vehicle += bf1943::VehicleCameraFrame();
    if (calls % 1200 == 0) REXLOG_INFO("BF motion: main view set {} times, {} for a vehicle", calls, vehicle);
  }
#endif
  if (uint32_t(ctx.lr) == kMainView && ctx.r4.u32 && bf1943::VehicleCameraFrame() && REXCVAR_GET(bf_smooth_motion)) {
    const Xf now_xf = LoadXf(base + ctx.r4.u32);
    const uint64_t tick = bf1943::ClientTicks();
    uint32_t copy = 0;
    {
      std::lock_guard lock(g.mutex);
      Xf out;
      const uint64_t frame = g_frame.load(std::memory_order_relaxed);
      g.camera_frame = frame;
      g.view = ctx.r3.u32;
      g.view_game = now_xf, g.view_game_valid = true;
      const Plan plan = Follow(g.camera, now_xf, tick, frame, kCameraTolerance);
      const bool follow = plan.any();
      const double f = bf1943::FramePhase() + (REXCVAR_GET(bf_smooth_motion_predict) ? 1.0 : 0.0);
      const bool blend = follow && Blend(g.camera.prev, g.camera.cur, f, out, plan.move, plan.turn);
      if (blend) copy = Copy(base, out);
      if (copy) RecordAttachedMove(out);
      g.camera_shown = out, g.camera_shown_valid = blend;
#if defined(BF_DEV_TOOLS)
      {  // BF_DEV_RIDERLOG: the camera handed to the view this frame, to pair with the riders' lines
        static const bool on = std::getenv("BF_DEV_RIDERLOG") != nullptr;
        static int lines = 0;
        if (on && lines < 1500) {
          ++lines;
          const Xf& shown = blend ? out : now_xf;
          REXLOG_INFO("BF rider cam: frame {} tick {} pos {:.4f} {:.4f} {:.4f} {}", frame, tick, shown.m[3][0],
                      shown.m[3][1], shown.m[3][2], blend ? "blended" : "the game's");
        }
      }
#endif
#if defined(BF_DEV_TOOLS)
      // BF_DEV_MOTION: how many camera frames were blended, and why the others were not.
      static uint64_t frames = 0, blended = 0, no_step = 0, moves = 0, turns = 0, both = 0, still = 0, refused = 0;
      static std::vector<double> drift_move, drift_turn, tick_move, tick_turn;  // this window's changes between ticks
      ++frames;
      if (g.camera.drifted) {
        drift_move.push_back(g.camera.drift_move), drift_turn.push_back(g.camera.drift_turn);
        tick_move.push_back(g.camera.tick_move), tick_turn.push_back(g.camera.tick_turn);
      }
      auto pct = [](std::vector<double> v, double q) {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, size_t(v.size() * q))];
      };
      blended += blend;
      no_step += !g.camera.step;
      const auto now = Clock::now();
      moves += g.camera.step && plan.move;
      turns += g.camera.step && plan.turn;
      both += g.camera.step && now < g.camera.moves_until && now < g.camera.turns_until;
      still += g.camera.step && Same(g.camera.prev, g.camera.cur);
      refused += follow && !blend;
      if (frames % 600 == 0 && std::getenv("BF_DEV_MOTION"))
        REXLOG_INFO("BF motion: camera {} frames, {} blended ({} with the position, {} with the turn); not: {} no tick "
                    "step, {} position and turn both changing between ticks, {} still, "
                    "{} refused; {} changes between ticks this window, half under {:.4f} m and {:.4f} turn (nine "
                    "in ten under {:.4f} m {:.4f} turn), against tick steps of {:.4f} m {:.4f} turn",
                    frames, blended, moves, turns, no_step, both, still, refused, drift_move.size(),
                    pct(drift_move, 0.5), pct(drift_turn, 0.5), pct(drift_move, 0.9), pct(drift_turn, 0.9),
                    pct(tick_move, 0.5), pct(tick_turn, 0.5));
      if (frames % 600 == 0) drift_move.clear(), drift_turn.clear(), tick_move.clear(), tick_turn.clear();
#endif
    }
#if defined(BF_DEV_TOOLS) && defined(_WIN32)
    DevCamFind(base, ctx.r4.u32, g.scratch);
#endif
    if (copy) ctx.r4.u64 = copy;
  }
  __imp__sub_82583348(ctx, base);
}

namespace bf1943 {
#if defined(BF_DEV_TOOLS)
void DevViewSample(uint8_t* base, uint32_t view);  // dev_matscan.cpp: BF_DEV_VIEWLOG
#endif
// In a vehicle, on a frame no tick ran since the camera manager's last frame (vehicle_camera.cpp): the manager does not
// run, as at 30 FPS, and the main view is handed this frame's camera (its last two tick cameras carried on by the
// frame's phase) through its own setter. Run on every frame with no time, the manager's own update still moved its
// camera off where the game puts it (the tank's first person: the vision slit's edge at 30 to 50% of the screen and
// jumping, against the game's 6%: runs vtankmgr, vtankgame; 16 to 17% and steady with the manager on tick frames only,
// vtanktickonly). False when there is no camera to hand (the manager then runs).
// Right before the camera manager's frame (vehicle_camera.cpp): the view holds the camera the manager itself handed it
// last, not ours, which the manager reads back (fed ours, its camera moved off where the game puts it, the more the
// further ours ran ahead: the tank's vision slit at 16% with ours from the tick frames in it, run vtanktickonly; a
// part of the turret across the view with ours from every frame, vtankview).
void TickBlendViewForManager(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(bf_smooth_motion)) return;
#if defined(BF_DEV_TOOLS)
  static const bool off = std::getenv("BF_DEV_NO_VIEWGAME") != nullptr;  // to compare
  if (off) return;
#endif
  uint32_t view = 0, copy = 0;
  {
    std::lock_guard lock(g.mutex);
    if (!g.view || !g.view_game_valid) return;
    copy = Copy(base, g.view_game);
    view = g.view;
  }
  if (!copy) return;
  PPCContext c = ctx;
  c.r3.u64 = view;
  c.r4.u64 = copy;
  __imp__sub_82583348(c, base);
}

bool TickBlendViewWithoutManager(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(bf_smooth_motion)) return false;
  uint32_t view = 0, copy = 0;
  {
    std::lock_guard lock(g.mutex);
    if (!g.view || !g.camera.have || !g.camera.step || g.camera.jump) return false;
    const double t = (REXCVAR_GET(bf_smooth_motion_predict) ? 1.0 : 0.0) + FramePhase() +
                     double(ClientTicks() - g.camera.seen_tick);
    Xf out;
    if (!Blend(g.camera.prev, g.camera.cur, t, out, std::memcmp(g.camera.prev.m[3], g.camera.cur.m[3], 12) != 0,
               std::memcmp(g.camera.prev.m, g.camera.cur.m, 36) != 0))
      out = g.camera.cur;
    copy = Copy(base, out);
    if (!copy) return false;
    g.camera_frame = g_frame.load(std::memory_order_relaxed);
    g.camera_shown = out;
    view = g.view;
  }
  PPCContext c = ctx;
  c.r3.u64 = view;
  c.r4.u64 = copy;
  __imp__sub_82583348(c, base);
#if defined(BF_DEV_TOOLS)
  DevViewSample(base, view);
#endif
  return true;
}
}  // namespace bf1943
