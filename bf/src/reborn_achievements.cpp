// Reborn's own achievements (reborn_achievements.h).

#include "reborn_achievements.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <initializer_list>
#include <iterator>

#include "seat_info.h"

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace bf1943 {
std::filesystem::path ExecutableDir();  // shader_prep.cpp: the folder of bf1943.exe
#if defined(BF_DEV_TOOLS)
void DevVehicleScan(uint32_t vehicle);  // dev_vehicle_scan.cpp (BF_DEV_VEHICLESCAN=1)
#endif
}

namespace bf1943::reborn_achievements {
namespace {

struct Ours {
  uint32_t id, gamerscore;
  const char* label;
  const char* description;
};
// Our own words. The game's locked text is its unlocked text, and so is ours.
constexpr Ours kOurs[] = {
    {kReborn, 10, "I am reborn", "Load into your first multiplayer match."},
    {kForever, 20, "I am forever", "Load into a match on a community server."},
    {kInevitable, 30, "I am inevitable", "Load into a custom match that you are hosting."},
    {kSteelBehemoth, 10, "Steel Behemoth", "Get into a tank for the first time."},
    {kRoadWarrior, 10, "Road Warrior", "Get into a jeep for the first time."},
    {kNavalWarfare, 10, "Naval Warfare", "Get into a boat for the first time."},
    {kAirSuperiority, 10, "Air Superiority", "Get into a fighter plane for the first time."},
    {kDeathFromAbove, 20, "Death From Above", "Call in the bombers from an airfield's air raid bunker."},
};

std::atomic<bool> g_ready{false};
std::atomic<int64_t> g_checked_ms{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

bool IsOurs(uint32_t id) {
  for (const Ours& o : kOurs)
    if (o.id == id) return true;
  return false;
}

size_t Count() { return std::size(kOurs); }

bool Ensure() {
  // Checked again every 2 s once there (the SDK's catalog is replaced only when it loads the game's title data).
  const int64_t now = NowMs();
  if (g_ready.load() && now - g_checked_ms.load() < 2000) return true;
  g_checked_ms = now;
  auto* k = REX_KERNEL_STATE();
  if (!k) return g_ready = false;
  auto& m = k->achievements();
  bool missing = false;
  for (const Ours& o : kOurs) missing |= !m.FindAchievement(o.id).has_value();
  if (!missing) return g_ready = true;
  // Only after the game's own are in: until then a register would be wiped by the SDK's load of them.
  if (m.ListAchievements().empty()) return g_ready = false;
  for (auto& info : Infos()) m.RegisterAchievement(std::move(info));
  std::string unlocked;
  for (const Ours& o : kOurs)
    if (m.IsUnlocked(o.id)) unlocked += (unlocked.empty() ? "" : ", ") + std::to_string(o.id);
  REXLOG_INFO("BF achievements: Reborn's own {} added beside the game's ({} in all; unlocked: {})", std::size(kOurs),
              m.ListAchievements().size(), unlocked.empty() ? "none" : unlocked);
  return g_ready = true;
}

std::vector<rex::system::AchievementInfo> Infos() {
  std::vector<rex::system::AchievementInfo> out;
  for (const Ours& o : kOurs) {
    rex::system::AchievementInfo info;
    info.id = o.id;
    info.label = o.label;
    info.description = o.description;
    info.unachieved_description = o.description;
    info.image_id = o.id;
    info.icon_path = IconFile(o.id).string();
    info.gamerscore = o.gamerscore;
    out.push_back(std::move(info));
  }
  return out;
}

std::filesystem::path IconFile(uint32_t image_id) {
  if (!IsOurs(image_id)) return {};
  return ExecutableDir() / "data" / "achievements" / (std::to_string(image_id) + ".png");
}

bool Unlock(uint32_t id, const char* why) {
  if (!IsOurs(id) || !Ensure()) return false;
  auto* k = REX_KERNEL_STATE();
  if (!k || k->achievements().IsUnlocked(id)) return false;
  const auto result = k->achievements().UnlockAchievement(id, rex::system::AchievementNotification::kShow);
  REXLOG_INFO("BF achievements: Reborn's {} {} ({})", id,
              result == rex::system::AchievementUnlockResult::kUnlocked ? "unlocked" : "not unlocked", why);
  return result == rex::system::AchievementUnlockResult::kUnlocked;
}

// ---- the vehicles

namespace {
uint32_t g_pending = 0;  // bit (id - 100): a vehicle achievement waiting for the catalog (VehicleTick's thread only)

std::string Squash(const std::string& s) {  // "Type 97 Chi-Ha" -> "type97chiha"
  std::string t;
  for (char c : s)
    if (std::isalnum(static_cast<unsigned char>(c))) t += char(std::tolower(static_cast<unsigned char>(c)));
  return t;
}
bool Has(const std::string& t, std::initializer_list<const char*> keys) {
  for (const char* k : keys)
    if (t.find(k) != std::string::npos) return true;
  return false;
}

// Guest memory, read only where it is committed and readable (as server_stats.cpp does).
bool Plausible(uint32_t a) { return a >= 0x00100000u && a < 0xFFF00000u && !(a & 3); }
bool Readable(rex::memory::Memory* memory, uint32_t address, uint32_t size) {
  if (!memory || address < 0x00100000u || address + size < address) return false;
  for (uint32_t page = address & ~0xFFFu; page < address + size;) {
    rex::memory::BaseHeap* heap = memory->LookupHeap(page);
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(page, &info)) return false;
    if (!(info.state & rex::memory::kMemoryAllocationCommit) || !(info.protect & rex::memory::kMemoryProtectRead)) return false;
    const uint32_t next = info.base_address + info.region_size;
    if (next <= page) return false;
    page = next;
  }
  return true;
}
uint32_t Load(rex::memory::Memory* memory, uint32_t a) {
  if (!Readable(memory, a, 4)) return 0;
  const uint8_t* p = memory->virtual_membase() + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
// Text ending in a zero at `a` (up to 63 characters), or "".
std::string Text(rex::memory::Memory* memory, uint32_t a) {
  if (!Plausible(a & ~3u) || !Readable(memory, a, 64)) return {};
  const char* p = reinterpret_cast<const char*>(memory->virtual_membase() + a);
  std::string s;
  for (int i = 0; i < 64 && p[i]; ++i) {
    if (p[i] < 32 || p[i] > 126) return {};
    s += p[i];
  }
  return s.size() < 64 ? s : std::string();
}

// The vehicle as the game's data has it (run vach_scan1 and vach_plane1, 18 Nov 2026): the vehicle object's data at
// +0x14; there its name at +0xC ("LCVP", "Corsair": the name the server's "into a vehicle" event gives too) and at
// +0x80 its class, an object whose name comes first ("Light_Boat", "Aircraft_Airframe_corsair").
struct VehicleNames {
  std::string name, cls;
};
VehicleNames ReadVehicle(uint32_t vehicle) {
  VehicleNames out;
  auto* k = REX_KERNEL_STATE();
  rex::memory::Memory* memory = k ? k->memory() : nullptr;
  if (!memory || !Plausible(vehicle)) return out;
  const uint32_t data = Load(memory, vehicle + 0x14);
  if (!Plausible(data)) return out;
  out.name = Text(memory, Load(memory, data + 0xC));
  out.cls = Text(memory, Load(memory, data + 0x80));
  return out;
}
}  // namespace

uint32_t ForVehicle(const std::string& name) {
  const std::string t = Squash(name);
  if (t.empty()) return 0;
  // The air raid first: its bombers are planes too, but they are flown from the bunker, not got into.
  if (Has(t, {"bombraid", "airraid", "bomber"})) return kDeathFromAbove;
  if (Has(t, {"corsair", "zero", "f4u", "a6m", "fighter", "plane", "aircraft", "airframe"})) return kAirSuperiority;
  if (Has(t, {"tank", "sherman", "chiha", "type97", "m4a"})) return kSteelBehemoth;
  if (Has(t, {"jeep", "willys", "kurogane", "truck"})) return kRoadWarrior;
  if (Has(t, {"boat", "lcvp", "landing", "higgins", "daihatsu", "ship"})) return kNavalWarfare;
  return 0;
}

bool VehicleTick(bool in_level) {
  if (!in_level) return false;
  SeatInfo seat;
  if (LocalSeat(seat)) {
    // Once a seat: which vehicle it is, by the game's own name for it, then by its class.
    static uint32_t key = 0;
    if (seat.key != key) {
      key = seat.key;
      uint32_t id = 0;
      if (seat.vehicle) {
#if defined(BF_DEV_TOOLS)
        DevVehicleScan(seat.vehicle);
#endif
        const VehicleNames v = ReadVehicle(seat.vehicle);
        id = ForVehicle(v.name);
        if (!id) id = ForVehicle(v.cls);
        // The seat's own scheme says the air raid's bombers and a plane's pilot (seat_info.h), whatever the names.
        if (seat.kind == SeatKind::kBomber) id = kDeathFromAbove;
        else if (seat.kind == SeatKind::kPlane) id = kAirSuperiority;
        else if (id == kDeathFromAbove) id = 0;  // a seat named for the raid, not flying it
        REXLOG_INFO("BF achievements: seat #{} in the vehicle \"{}\" (class \"{}\"), {}: {}", seat.key, v.name, v.cls,
                    SeatKindName(seat.kind), id ? "Reborn's " + std::to_string(id) : std::string("none of Reborn's"));
      } else if (seat.kind == SeatKind::kBomber) {
        id = kDeathFromAbove;
      }
      if (id) g_pending |= 1u << (id - 100);
    }
  }
  if (!g_pending) return false;
  auto* k = REX_KERNEL_STATE();
  if (!k || !Ensure()) return false;  // the catalog not there yet: they wait
  const uint32_t pending = g_pending;
  g_pending = 0;
  bool any = false;
  for (uint32_t id = kSteelBehemoth; id <= kDeathFromAbove; ++id) {
    if (!(pending & (1u << (id - 100)))) continue;
    if (k->achievements().IsUnlocked(id)) continue;  // the usual case: had it already
    const char* why = id == kSteelBehemoth ? "into a tank"
                      : id == kRoadWarrior ? "into a jeep"
                      : id == kNavalWarfare ? "into a boat"
                      : id == kAirSuperiority ? "into a fighter plane"
                                              : "the air raid's bombers";
    any |= Unlock(id, why);
  }
  return any;
}

}  // namespace bf1943::reborn_achievements
