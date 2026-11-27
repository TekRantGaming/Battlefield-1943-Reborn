// One mouse click -> one selection in the deploy movie's authoritative list.
// The APT entry points/types below are specific to BF1943's supported Xbox XEX,
// like the existing method bridge in auto_join.cpp. No game bytecode is
// embedded.
#include "flash_spawn.h"
#include "capture_points.h"
#include "spawn_selection.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>  // (v0.10's generated precompiled header brought it)

REXCVAR_DEFINE_BOOL(bf_spawn_mouse_direct, true, "BF1943/Controls",
                    "Select a clicked spawn by its UI name once (false: "
                    "restore the legacy mouse navigation)");
REXCVAR_DECLARE(bool, bf_debug_spawn_mouse);

REX_EXTERN(sub_82958C20); // AptString(text)
REX_EXTERN(sub_8295C470); // AptStringData release
REX_EXTERN(sub_82986340); // root movie scope
REX_EXTERN(sub_8297DD10); // path lookup
REX_EXTERN(sub_82965310); // value -> AptString
REX_EXTERN(sub_8295B1A0); // integer value factory
REX_EXTERN(sub_82986808); // interpreter's typed SetVariable/SetMember
REX_EXTERN(sub_82986450); // existing method bridge
REX_EXTERN(sub_829548F0); // VM's numeric Array GetMember

namespace bf1943 {
namespace {
std::mutex g_mutex;
spawn_selection::Requests g_requests;
std::atomic<bool> g_focused{false};
std::vector<SpawnMouseMarker> g_markers;

class AptUi {
public:
  AptUi(PPCContext &parent, uint8_t *base, uint32_t ui)
      : parent_(parent), base_(base) {
    if (!Readable(ui, 24))
      return;
    const uint32_t holder = Load(ui + 20);
    if (!Readable(holder, 8) || !Readable(Load(holder + 4), 32))
      return;
    old_player_ = Load(0x82D135E8);
    old_context_ = Load(0x82D135EC);
    Store(0x82D135E8, Load(holder + 4));
    Store(0x82D135EC, Load(holder + 4));
    installed_ = true;
    static thread_local uint32_t scratch = 0;
    if (!scratch)
      scratch = REX_KERNEL_MEMORY()->SystemHeapAlloc(1024);
    scratch_ = scratch;
    if (!scratch_)
      return;
    root_ = Call(sub_82986340, {0});
    if (!Value(root_))
      return;
    screen_ = Lookup(root_, "_root.contentMC.loader1");
    if (!Value(screen_) ||
        (Load(screen_) != Load(root_) && Load(screen_) != 0x820BEDB0))
      screen_ = 0;
  }
  ~AptUi() {
    // Native getters may return temporary arrays/values. Keep them rooted for
    // the entire read/commit transaction, then release before restoring context.
    for (auto it = pinned_.rbegin(); it != pinned_.rend(); ++it)
      Release(*it);
    if (installed_) {
      Store(0x82D135E8, old_player_);
      Store(0x82D135EC, old_context_);
    }
  }
  uint32_t screen() const { return screen_; }
  std::vector<SpawnMouseMarker> Markers(const std::vector<CapturePoint> &points) {
    std::vector<SpawnMouseMarker> markers;
    const uint32_t map = Lookup(screen_, "m_activeSpawnMap");
    if (!Value(map) || (Load(map + 4) & 0x7F) == 3)
      return markers;
    for (const auto &point : points) {
      const uint32_t clip = LookupPart(map, point.name, true);
      float x, y;
      if (GlobalOrigin(clip, x, y) && x >= 0 && x <= 1280 && y >= 0 && y <= 720)
        markers.push_back({point.name, point.entity, x, y});
    }
    return markers;
  }
  bool Ready() {
    int index, mode;
    return screen_ && Integer("m_currentSpawnPointIdx", index) &&
           Integer("m_spawnMode", mode) && index >= 0 && mode >= 0 &&
           mode <= 1 && Method("updateControlpoints") &&
           Method("onSpawnModeChanged");
  }
  bool Snapshot(std::vector<spawn_selection::Entry> &entries,
                std::vector<std::string> &modes) {
    int count, squad;
    if (!Integer("extern.spawnPoints.length", count) || count < 1 ||
        count > 64 || !Integer("extern.squad.length", squad) || squad < 0 ||
        squad > 24) {
      REXLOG_WARN("BF spawn direct: list header read failed at {} (APT type {})",
                  last_path_, last_type_);
      return false;
    }
    // igm_mp_cq.getSpawnModes: flag/squad with a squad, otherwise
    // frontline/flag.
    modes = squad ? std::vector<std::string>{"flag", "squad"}
                  : std::vector<std::string>{"frontline", "flag"};
    entries.reserve(count);
    for (int i = 0; i < count; ++i) {
      const std::string path = "extern.spawnPoints." + std::to_string(i) + ".";
      spawn_selection::Entry entry;
      if (!Text(path + "0", entry.name) || !Integer(path + "1", entry.id) ||
          !Boolean(path + "2", entry.eligible) ||
          !Boolean(path + "5", entry.excluded)) {
        REXLOG_WARN("BF spawn direct: list entry read failed at {} (APT type {})",
                    last_path_, last_type_);
        return false;
      }
      entries.push_back(std::move(entry));
    }
    return true;
  }
  bool Commit(const spawn_selection::Plan &plan) {
    // Pin both old values and prepare both new typed values before changing any
    // property. SetMember performs reference management; no object offsets are
    // written.
    const uint32_t old_mode = Lookup(screen_, "m_spawnMode"),
                   old_index = Lookup(screen_, "m_currentSpawnPointIdx");
    if (!Retain(old_mode))
      return false;
    if (!Retain(old_index)) {
      Release(old_mode);
      return false;
    }
    const uint32_t mode = Call(sub_8295B1A0, {uint32_t(plan.flag_mode)});
    const bool have_mode = Retain(mode);
    const uint32_t index =
        have_mode ? Call(sub_8295B1A0, {uint32_t(plan.filtered_index)}) : 0;
    const bool have_index = index && Retain(index);
    int before_mode = -1;
    const bool prepared =
        have_mode && have_index && IntegerValue(old_mode, before_mode);
    bool committed = prepared && Set("m_currentSpawnPointIdx", index) &&
                     Set("m_spawnMode", mode);
    int check_mode = -1, check_index = -1;
    committed = committed && Integer("m_spawnMode", check_mode) &&
                Integer("m_currentSpawnPointIdx", check_index) &&
                check_mode == plan.flag_mode &&
                check_index == plan.filtered_index;
    if (committed) {
      if (before_mode != plan.flag_mode)
        Invoke("onSpawnModeChanged");
      Invoke("updateControlpoints"); // setSpawnInfo -> updateSpawnpoints ->
                                     // sendSpawnSettings, once
      int actual = -1;
      committed =
          Integer("currentSpawnPoint", actual) && actual == plan.point_id;
      if (!committed)
        REXLOG_WARN("BF spawn direct: selection refresh disagreed with the "
                    "planned UI ID; restoring");
    }
    if (prepared && !committed) {
      Set("m_currentSpawnPointIdx", old_index);
      Set("m_spawnMode", old_mode);
      Invoke("onSpawnModeChanged");
      Invoke("updateControlpoints");
    }
    if (have_index)
      Release(index);
    if (have_mode)
      Release(mode);
    Release(old_index);
    Release(old_mode);
    return committed;
  }

private:
  // Same transform chain as this XEX's MovieClip.localToGlobal: clip+28 is
  // the parent, clip+32 -> +4 -> +8 is its optional six-float affine matrix.
  // Read only; preserve rotation, scale and skew rather than fitting world
  // coordinates to a screenshot. A missing matrix is identity in the VM.
  bool GlobalOrigin(uint32_t clip, float &x, float &y) const {
    double px = 0, py = 0;
    std::vector<uint32_t> visited;
    for (int depth = 0; clip && depth < 32; ++depth) {
      if (!Value(clip) || !Readable(clip, 36) ||
          (Load(clip + 4) & 0x7F) != 12 ||
          std::find(visited.begin(), visited.end(), clip) != visited.end())
        return false;
      visited.push_back(clip);
      const uint32_t character = Load(clip + 32);
      if (!Readable(character, 8) || !Readable(Load(character + 4), 12))
        return false;
      const uint32_t matrix = Load(Load(character + 4) + 8);
      if (matrix) {
        if (!Readable(matrix, 24))
          return false;
        float m[6];
        for (int i = 0; i < 6; ++i) {
          const uint32_t bits = Load(matrix + 4 * i);
          std::memcpy(&m[i], &bits, sizeof(float));
          if (!std::isfinite(m[i]))
            return false;
        }
        const double next_x = m[0] * px + m[2] * py + m[4];
        py = m[1] * px + m[3] * py + m[5];
        px = next_x;
      }
      clip = Load(clip + 28);
    }
    if (clip || !std::isfinite(px) || !std::isfinite(py))
      return false;
    x = float(px);
    y = float(py);
    return true;
  }
  bool Readable(uint32_t a, size_t n) const {
    if (a < 0x10000 || uint64_t(a) + n > 0xF0000000)
      return false;
    auto *heap = REX_KERNEL_MEMORY()->LookupHeap(a);
    rex::memory::HeapAllocationInfo info{};
    return heap && heap->QueryRegionInfo(a, &info) &&
           (info.state & rex::memory::kMemoryAllocationCommit) &&
           (info.protect & rex::memory::kMemoryProtectRead) &&
           uint64_t(a) + n <=
               uint64_t(a & ~(heap->GetPageSize() - 1)) + info.region_size;
  }
  uint32_t Load(uint32_t a) const {
    return uint32_t(base_[a]) << 24 | uint32_t(base_[a + 1]) << 16 |
           uint32_t(base_[a + 2]) << 8 | base_[a + 3];
  }
  void Store(uint32_t a, uint32_t v) {
    for (int i = 0; i < 4; ++i)
      base_[a + i] = uint8_t(v >> (24 - 8 * i));
  }
  bool Value(uint32_t a) const {
    return Readable(a, 8) && Load(a) >= 0x82000000 && Load(a) < 0x82160000 &&
           Readable(Load(a), 8);
  }
  uint32_t Call(PPCFunc &function, std::initializer_list<uint32_t> args) {
    PPCContext call = parent_;
    auto at = args.begin();
    for (auto *reg : {&call.r3, &call.r4, &call.r5, &call.r6, &call.r7,
                      &call.r8, &call.r9, &call.r10})
      reg->u64 = at == args.end() ? 0 : *at++;
    function(call, base_);
    return call.r3.u32;
  }
  bool Reference(uint32_t value, uint32_t slot) {
    if (!Value(value))
      return false;
    auto *function = REX_KERNEL_STATE()->function_dispatcher()->GetFunction(
        Load(Load(value) + slot));
    if (!function)
      return false;
    Call(*function, {value});
    return true;
  }
  bool Retain(uint32_t value) { return Reference(value, 0); }
  void Release(uint32_t value) { Reference(value, 4); }
  bool Pin(uint32_t value) {
    if (std::find(pinned_.begin(), pinned_.end(), value) != pinned_.end())
      return true;
    if (!Retain(value))
      return false;
    pinned_.push_back(value);
    return true;
  }
  uint32_t Lookup(uint32_t scope, const std::string &path) {
    if (!Value(scope) || path.empty() || path.size() >= 240)
      return 0;
    // The movie-path parser uses dictionary lookup for intermediate tokens.
    // That cannot read native extern properties or dense array elements. Walk
    // those paths using the interpreter's GetMember dispatch instead, so each
    // token reaches the object's virtual getter (including array indices).
    last_path_ = path;
    last_type_ = -1;
    if (path.starts_with("_root."))
      return LookupPart(scope, path, false);
    uint32_t value = scope;
    for (size_t start = 0; start < path.size();) {
      const size_t dot = path.find('.', start);
      const size_t end = dot == std::string::npos ? path.size() : dot;
      if (end == start)
        return 0;
      value = LookupPart(value, path.substr(start, end - start), true);
      last_path_ = path.substr(0, end);
      last_type_ = value ? int(Load(value + 4) & 0x7F) : -1;
      if (!value || last_type_ == 3)
        return 0;
      start = end + 1;
    }
    return value;
  }
  uint32_t LookupPart(uint32_t scope, const std::string &path, bool member) {
    if (!Value(scope) || path.empty() || path.size() >= 240)
      return 0;
    std::memcpy(base_ + scratch_ + 16, path.c_str(), path.size() + 1);
    // GetMember has an explicit extern (type 11) case before generic lookup:
    // the registered callback at AptCallbacks+64 takes the property's C string
    // and returns its typed APT value. A movie/path lookup cannot substitute
    // for it (the extern object's ordinary virtual getter returns null).
    // Match the VM's GetMember opcode, sub_8297EC50, rather than guessing IDs.
    if (member && (Load(scope + 4) & 0x7F) == 11) {
      auto *getter = REX_KERNEL_STATE()->function_dispatcher()->GetFunction(
          Load(0x82D13C88));
      if (!getter)
        return 0;
      const uint32_t result = Call(*getter, {scratch_ + 16});
      return Value(result) && Pin(result) ? result : 0;
    }
    if (member && (Load(scope + 4) & 0x7F) == 14) {
      uint32_t index = 0;
      const auto [end, error] =
          std::from_chars(path.data(), path.data() + path.size(), index);
      if (error == std::errc() && end == path.data() + path.size()) {
        if (index > 64)
          return 0;
        const uint32_t result = Call(sub_829548F0, {scope, index});
        return Value(result) && Pin(result) ? result : 0;
      }
    }
    Call(sub_82958C20, {scratch_, scratch_ + 16});
    const uint32_t result =
        Call(sub_8297DD10, {0x82D13BB8, scope, 0, scratch_, 1,
                           member ? 0u : 1u, member ? 1u : 0u});
    Call(sub_8295C470, {Load(scratch_)});
    return Value(result) && Pin(result) ? result : 0;
  }
  bool StringData(uint32_t data, std::string &text) const {
    if (!Readable(data, 8))
      return false;
    const size_t n = size_t(base_[data + 2]) << 8 | base_[data + 3];
    if (n > 255 || !Readable(data + 8, n + 1) || base_[data + 8 + n] != 0)
      return false;
    text.assign(reinterpret_cast<const char *>(base_ + data + 8), n);
    return text.find('\0') == std::string::npos;
  }
  bool Text(const std::string &path, std::string &text) {
    const uint32_t value = Lookup(screen_, path);
    return value && (Load(value + 4) & 0x7F) == 1 && Readable(value, 12) &&
           StringData(Load(value + 8), text);
  }
  bool IntegerValue(uint32_t value, int &result) {
    if (!Value(value) || !Readable(value, 12))
      return false;
    const uint32_t type = Load(value + 4) & 0x7F;
    if (type == 7) {
      result = int32_t(Load(value + 8));
      return true;
    }
    if (type != 6)
      return false;
    // A numeric scalar's conversion is bounded by the interpreter; reject
    // objects/strings.
    Call(sub_82965310, {value, scratch_ + 272});
    const uint32_t data = Load(scratch_ + 272);
    std::string text;
    const bool read = StringData(data, text);
    Call(sub_8295C470, {data});
    if (!read)
      return false;
    double number = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), number);
    if (error != std::errc() || end != text.data() + text.size() ||
        !std::isfinite(number) || std::trunc(number) != number ||
        number < std::numeric_limits<int>::min() ||
        number > std::numeric_limits<int>::max())
      return false;
    result = int(number);
    return true;
  }
  bool Integer(const std::string &path, int &result) {
    return IntegerValue(Lookup(screen_, path), result);
  }
  bool Boolean(const std::string &path, bool &result) {
    const uint32_t value = Lookup(screen_, path);
    if (!value || !Readable(value, 9))
      return false;
    if ((Load(value + 4) & 0x7F) == 5 && base_[value + 8] <= 1) {
      result = base_[value + 8] != 0;
      return true;
    }
    // The movie uses loose equality to true/false. The engine may supply
    // numeric 0/1 rather than Boolean values for an array column.
    int number;
    if (IntegerValue(value, number) && (number == 0 || number == 1)) {
      result = number != 0;
      return true;
    }
    return false;
  }
  bool Method(const std::string &name) {
    const uint32_t value = Lookup(screen_, name);
    if (!value)
      return false;
    const uint32_t type = Load(value + 4) & 0x7F;
    return type >= 34 && type <= 36;
  }
  void Invoke(const char *name) {
    std::memcpy(base_ + scratch_ + 16, name, std::strlen(name) + 1);
    Call(sub_82986450, {scratch_ + 16, 0, screen_, 0, 0});
  }
  bool Set(const char *name, uint32_t value) {
    std::memcpy(base_ + scratch_ + 16, name, std::strlen(name) + 1);
    Call(sub_82958C20, {scratch_, scratch_ + 16});
    // Resolve a simple member on this clip; no global-variable fallbacks or
    // path injection.
    const bool result = Call(sub_82986808, {0x82D13BB8, screen_, 0, scratch_,
                                            value, 1, 0, 1}) != 0;
    Call(sub_8295C470, {Load(scratch_)});
    return result;
  }
  PPCContext &parent_;
  uint8_t *base_;
  uint32_t root_ = 0, screen_ = 0, scratch_ = 0, old_player_ = 0,
           old_context_ = 0;
  bool installed_ = false;
  std::string last_path_;
  int last_type_ = -1;
  std::vector<uint32_t> pinned_;
};
} // namespace

std::vector<SpawnMouseMarker> SpawnMouseMarkers() {
  std::lock_guard lock(g_mutex);
  return g_markers;
}
void CancelSpawnSelection() {
  std::lock_guard lock(g_mutex);
  g_requests.Cancel();
}
void SpawnSelectionFocus(bool focused) {
  g_focused.store(focused, std::memory_order_relaxed);
  if (!focused)
    CancelSpawnSelection();
}
bool QueueSpawnSelection(const std::string &name, uint32_t entity) {
  std::lock_guard lock(g_mutex);
  return g_focused.load(std::memory_order_relaxed) &&
         g_requests.Queue(name, entity);
}

void ProcessSpawnSelection(PPCContext &ctx, uint8_t *base, uint32_t ui,
                           bool allowed, bool navigation) {
  if (!allowed || !g_focused.load(std::memory_order_relaxed) ||
      !REXCVAR_GET(bf_spawn_mouse_direct)) {
    std::lock_guard lock(g_mutex);
    g_requests.Observe(0, false);
    g_markers.clear();
    return;
  }
  static thread_local bool processing = false;
  if (processing)
    return;
  struct Guard {
    bool &flag;
    Guard(bool &f) : flag(f) { flag = true; }
    ~Guard() { flag = false; }
  } guard(processing);
  AptUi apt(ctx, base, ui);
  const uint32_t screen = apt.Ready() ? apt.screen() : 0;
  const auto points = CapturePoints();
  static thread_local uint32_t marker_screen = 0;
  static thread_local auto marker_time = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  const bool refresh_markers = screen != marker_screen ||
      now - marker_time >= std::chrono::milliseconds(100);
  std::vector<SpawnMouseMarker> markers;
  if (refresh_markers) {
    if (screen)
      markers = apt.Markers(points);
    if (screen && screen != marker_screen)
      REXLOG_INFO("BF spawn direct: deployment UI marker positions: {} of {}",
                  markers.size(), points.size());
    marker_screen = screen;
    marker_time = now;
  }
  std::optional<spawn_selection::Request> request;
  {
    std::lock_guard lock(g_mutex);
    g_requests.Observe(screen, screen != 0);
    if (refresh_markers)
      g_markers = std::move(markers);
    if (navigation)
      g_requests.Cancel(); // a deliberate native navigation takes precedence;
                           // input is not suppressed
    request = g_requests.Take();
  }
  if (!request)
    return;
  if (std::none_of(points.begin(), points.end(),
                   [&](const CapturePoint &point) {
                     return point.entity == request->entity &&
                            point.name == request->name;
                   }))
    return;
  std::vector<spawn_selection::Entry> entries;
  std::vector<std::string> modes;
  if (!apt.Snapshot(entries, modes)) {
    REXLOG_WARN(
        "BF spawn direct: {} ignored; deployment UI schema could not be read",
        request->name);
    return;
  }
  if (REXCVAR_GET(bf_debug_spawn_mouse))
    for (const auto &entry : entries)
      REXLOG_INFO("BF spawn direct: UI entry {} id {} eligible {} excluded {}",
                  entry.name, entry.id, entry.eligible, entry.excluded);
  const auto plan =
      spawn_selection::PlanSelection(entries, request->name, modes);
  if (!plan) {
    REXLOG_INFO(
        "BF spawn direct: {} ignored; not uniquely eligible in the UI list",
        request->name);
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    if (!g_requests.Current(*request))
      return;
  }
  const bool committed =
      apt.Commit(*plan); // no request/state lock across reentrant UI calls
  REXLOG_INFO("BF spawn direct: request {} {} -> UI index {}, point {}: {}",
              request->generation, request->name, plan->filtered_index,
              plan->point_id, committed ? "selected once" : "not committed");
}
} // namespace bf1943
