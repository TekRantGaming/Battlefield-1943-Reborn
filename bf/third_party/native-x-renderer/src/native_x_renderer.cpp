// NativeXRenderer - see include/native_x_renderer/native_x_renderer.h.
//
// This file owns the hook dispatch: the game's hooks (hooks.h) call detail::Enter/Leave around the
// original function while a feature is on, and the features (trace.cpp, observer.cpp) see every call
// with whether it is nested in another hooked call, and the end of every frame.

#include <native_x_renderer/hooks.h>
#include <native_x_renderer/native_x_renderer.h>

#include <rex/logging.h>

#include <atomic>
#include <memory>
#include <string_view>

#include "internal.h"
#include "observer.h"
#include "renderer.h"
#include "state.h"
#include "targets.h"
#include "trace.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace native_x_renderer {
namespace internal {

namespace {
constexpr CallInfo kCalls[] = {
#define NATIVE_X_RENDERER_CALL(id, arguments, flags) \
  {#id, arguments, flags, std::string_view(arguments).starts_with("dev")},
#include <native_x_renderer/calls.inc>
#undef NATIVE_X_RENDERER_CALL
};
static_assert(std::size(kCalls) == kCallCount);
}  // namespace

const CallInfo& Info(Call call) { return kCalls[size_t(call)]; }

uint32_t CurrentThreadId() {
#ifdef _WIN32
  return GetCurrentThreadId();
#else
  return 0;
#endif
}

}  // namespace internal

namespace {

bool g_trace = false;
bool g_observer = false;
bool g_renderer = false;
// The guest GPU state, for the observer and the renderer (state.h).
std::unique_ptr<StateTracker> g_tracker;
std::atomic<uint32_t> g_frame{0};
std::atomic<uint32_t> g_render_thread{0};  // the thread that presents
thread_local int t_depth = 0;
// Whether this thread's outermost call, and so the calls nested in it, goes to the state tracker and the
// renderer: a device call, or a call that writes into the device's command buffer (kPackets). The others
// (Lock, Unlock, the cache flush) are only traced: games make them from other threads too, while the
// render thread is inside a device call, and the tracker's call stack is not shared between threads (D61).
thread_local bool t_tracked = false;
// DeviceLayout::recording_flags and recording_mask: a command buffer device's calls are only traced.
uint32_t g_recording_flags = kUnknown;
uint8_t g_recording_mask = 0;

// The command buffer write pointer of the device of this thread's outermost recording call, at its entry (the observer
// learns which memory the recorded command buffers are in).
thread_local uint32_t t_recording_device = 0, t_recording_start = 0;
uint32_t g_command_buffer_write = kUnknown;
const Profile* g_profile = nullptr;

uint32_t Load32(const uint8_t* base, uint32_t address) { return internal::Load32(base, address); }

bool Recording(const internal::CallInfo& info, const PPCContext& ctx, const uint8_t* base) {
  if (!info.device || g_recording_flags == kUnknown || !g_recording_mask) return false;
  const uint32_t device = ctx.r3.u32;
  return device && (base[device + g_recording_flags] & g_recording_mask);
}

}  // namespace

const char* Describe() {
  return "NativeXRenderer 0.6 (--native_x_renderer draws with NativeXRenderer, the Xenos path otherwise; trace and observer)";
}

const char* CallName(Call call) {
  return size_t(call) < kCallCount ? internal::Info(call).name : "?";
}

void Initialize(const Profile& profile, rex::system::IGraphicsSystem* graphics, rex::ui::Window* window,
                const std::filesystem::path& cache_root) {
  REXLOG_INFO("[native_x_renderer] profile: {} (XDK {})", profile.game, profile.xdk_version);
  g_recording_flags = profile.device.recording_flags;
  g_recording_mask = profile.device.recording_mask;
  g_command_buffer_write = profile.device.command_buffer_write;
  g_profile = &profile;
  g_trace = trace::Initialize(profile);
  g_observer = observer::Initialize(profile, graphics);
  g_renderer = renderer::Initialize(profile, graphics, window, cache_root);
  if (g_observer || g_renderer) g_tracker = std::make_unique<StateTracker>(profile);
  // The observer's verification wants every call; the renderer only those that changed what the tracker models.
  // The observer reads the tracker's register file; the renderer applies the tracker's records on its own thread.
  if (g_tracker) {
    g_tracker->set_skip_empty_calls(!g_observer);
    g_tracker->set_apply_locally(g_observer);
    g_tracker->set_keep_record(g_renderer);
  }
  detail::g_hooks_active = g_trace || g_observer || g_renderer;
}

bool Renders() { return g_renderer; }

void StorageProgress(uint32_t* queued, uint32_t* done, uint32_t* loading, uint32_t* finished) {
  if (!g_renderer) {
    if (queued) *queued = 0;
    if (done) *done = 0;
    if (loading) *loading = 0;
    if (finished) *finished = 1;
    return;
  }
  renderer::StorageProgress(queued, done, loading, finished);
}

double AspectFactor() { return g_renderer ? renderer::AspectFactor() : 1.0; }

void GpuBenchmarkStart() {
  if (g_renderer) renderer::GpuBenchmarkStart();
}
bool GpuBenchmarkResult(double* ms) { return g_renderer && renderer::GpuBenchmarkResult(ms); }
bool NisSupport(bool* runs, std::string* gpu) { return g_renderer && renderer::NisSupport(runs, gpu); }

// D163: kept whether the renderer runs yet or not (the level is read on a worker).
void NightLamps(const NightLamp* lamps, uint32_t count) { renderer::NightLamps(lamps, count); }
void NightModels(const NightModelLamps* models, uint32_t count) { renderer::NightModels(models, count); }

void SetGameAspectBox(bool on) {
  if (g_renderer) renderer::SetGameAspectBox(on);
}

void BeginView(const float* scene_view_projection) {
  if (g_renderer) renderer::BeginView(scene_view_projection);
}

void EndViewGeometry() {
  if (g_renderer) renderer::EndViewGeometry();
}

void RecordAttachedCameraMove(const float* position, const float* move, const float* placed);  // draw.cpp
void SetAttachedCameraMove(const float* position, const float* move, const float* placed) {
  RecordAttachedCameraMove(position, move, placed);
}

namespace detail {

bool g_hooks_active = false;

void Enter(Call call, PPCContext& ctx, uint8_t* base) {
  if (t_depth == 0 && g_tracker) {
    // A game that commits its inline draws itself (an inlined EndVertices, D77): the draw its last BeginVertices
    // tracked is handed on as an EndVertices before the next device call, if the game committed it.
    const internal::CallInfo& info = internal::Info(call);
    if (info.device || (info.flags & internal::kPackets)) {
      if (const TrackedCall* tracked = g_tracker->TakeCommittedInlineDraw(base)) {
        uint64_t r3 = ctx.r3.u64;
        ctx.r3.u64 = tracked->device;
        if (g_trace) {
          trace::OnEnter(Call::kEndVertices, ctx, base, false, false);
          trace::OnLeave(Call::kEndVertices, ctx, base);
        }
        if (g_observer) observer::OnLeave(*tracked, *g_tracker);
        if (g_renderer) {
          renderer::OnEnter(Call::kEndVertices, ctx, base, false);
          renderer::OnLeave(*tracked, *g_tracker, ctx, base);
        }
        ctx.r3.u64 = r3;
      }
    }
  }
  bool nested = t_depth++ > 0;
  if (!nested) {
    const internal::CallInfo& info = internal::Info(call);
    const bool recording = Recording(info, ctx, base);
    t_tracked = (info.device || (info.flags & internal::kPackets)) && !recording;
    t_recording_device = recording && g_command_buffer_write != kUnknown ? ctx.r3.u32 : 0;
    if (t_recording_device) t_recording_start = Load32(base, t_recording_device + g_command_buffer_write);
  }
  if (g_trace) {
    uint32_t render = g_render_thread.load(std::memory_order_relaxed);
    trace::OnEnter(call, ctx, base, nested, render && render != internal::CurrentThreadId());
  }
  if (!t_tracked) return;
  if (g_tracker) g_tracker->Enter(call, ctx.r3.u32, base, nested);
  if (g_renderer) renderer::OnEnter(call, ctx, base, nested);
}

void Leave(Call call, PPCContext& ctx, uint8_t* base) {
  bool nested = --t_depth > 0;
  if (g_trace) trace::OnLeave(call, ctx, base);
  if (!nested && t_recording_device) {
    // The write pointer holds the last dword written: the call's packets are (start, end].
    const uint32_t start = t_recording_start + 4, end = Load32(base, t_recording_device + g_command_buffer_write) + 4;
    if (g_observer) observer::OnRecorded(start, end);
    if ((internal::Info(call).flags & internal::kDraw) && end > start && end - start < 0x10000 && g_profile) {
      // The surfaces this draw renders into, for its replay at RunCommandBuffer.
      const DeviceLayout& d = g_profile->device;
      std::array<SurfaceSnapshot, 5> surfaces{};
      for (uint32_t i = 0; i < 5; ++i) {
        const uint32_t object =
            Load32(base, t_recording_device + (i < 4 ? d.render_targets + 4 * i : d.depth_stencil));
        const GuestSurface s = ReadSurface(base, object, i == 4, g_profile->objects);
        surfaces[i] = {s.object, s.surface_info, s.info, s.width, s.height};
      }
      NoteRecordedDraw(internal::GpuAddress(start), internal::GpuAddress(end - 4) + 4, surfaces);
    }
    t_recording_device = 0;
  }
  if (g_tracker && t_tracked && (internal::Info(call).flags & internal::kReplay) && !nested) {
    // A recorded command buffer run: each recorded draw goes on as a draw of its own, in order, then the call.
    const TrackedCall* tracked = g_tracker->LeaveReplay(call, base, nested, [&](const TrackedCall& draw) {
      if (g_observer) observer::OnLeave(draw, *g_tracker);
      if (g_renderer) renderer::OnLeave(draw, *g_tracker, ctx, base);
    });
    if (tracked) {
      if (g_observer) observer::OnLeave(*tracked, *g_tracker);
      if (g_renderer) renderer::OnLeave(*tracked, *g_tracker, ctx, base);
    }
  } else if (g_tracker && t_tracked) {
    if (const TrackedCall* tracked = g_tracker->Leave(call, base, nested)) {
      if (g_observer) observer::OnLeave(*tracked, *g_tracker);
      if (g_renderer) renderer::OnLeave(*tracked, *g_tracker, ctx, base);
    }
  }
  if ((internal::Info(call).flags & internal::kFrameEnd) && !nested) {
    g_render_thread.store(internal::CurrentThreadId(), std::memory_order_relaxed);
    uint32_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_trace) trace::OnFrameEnd(frame);
    if (g_observer) observer::OnFrameEnd(frame);
    if (g_renderer) renderer::OnFrameEnd(frame);
  }
}

}  // namespace detail

}  // namespace native_x_renderer
