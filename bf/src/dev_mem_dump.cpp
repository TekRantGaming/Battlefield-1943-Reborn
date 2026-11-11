// Developer aid (BF_DEV_TOOLS builds only): writes ranges of guest physical memory to files at a set time, for
// looking at what the GPU wrote there (resolve targets, with --readback_resolve=full).
//
//   BF_DUMP_PHYS=<seconds>:<hex address>:<hex size>[,<hex address>:<hex size>...]   BF_DUMP_OUT=<folder>
//
// Seconds count from the first frame of the game's UI update; each range goes to <folder>\phys_<address>.bin.

#if defined(BF_DEV_TOOLS)

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

namespace {

struct Range {
  uint32_t address, size;
};

struct Plan {
  double at = -1;
  std::vector<Range> ranges;
  std::string out;
  bool done = false;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

Plan& GetPlan() {
  static Plan plan = [] {
    Plan p;
    const char* v = std::getenv("BF_DUMP_PHYS");
    const char* o = std::getenv("BF_DUMP_OUT");
    if (!v || !*v || !o || !*o) return p;
    p.out = o;
    std::string s = v;
    size_t colon = s.find(':');
    if (colon == std::string::npos) return p;
    p.at = std::atof(s.substr(0, colon).c_str());
    s = s.substr(colon + 1);
    for (size_t at = 0; at < s.size();) {
      size_t comma = s.find(',', at);
      if (comma == std::string::npos) comma = s.size();
      const std::string item = s.substr(at, comma - at);
      const size_t c = item.find(':');
      if (c != std::string::npos)
        p.ranges.push_back({uint32_t(std::stoul(item.substr(0, c), nullptr, 16)),
                            uint32_t(std::stoul(item.substr(c + 1), nullptr, 16))});
      at = comma + 1;
    }
    return p;
  }();
  return plan;
}

}  // namespace

namespace bf1943 {
namespace dev {
void WatchHostWrites(std::vector<const void*> addresses, double seconds, const char* label);  // dev_watch.cpp
}

// Once a frame, from VdSwap (frame_stats.cpp): the UI's update does not run in every mode (NativeXRenderer).
void DevMemDumpFrame() {
  // BF_WATCH_PHYS=<hex physical address>:<start seconds>:<seconds>: a hardware watch on writes to that address through
  // its physical view and its 0xA0000000, 0xC0000000 and 0xE0000000 guest views (who writes texture data unseen).
  static const auto start = std::chrono::steady_clock::now();
  static bool watched = false;
  struct Watch {
    uint32_t address = 0;
    double at = -1, seconds = 0;
  };
  static const Watch watch = [] {
    Watch w;
    if (const char* v = std::getenv("BF_WATCH_PHYS"); v && *v) {
      unsigned address = 0;
      double at = 0, seconds = 0;
      if (std::sscanf(v, "%x:%lf:%lf", &address, &at, &seconds) == 3) w = {uint32_t(address), at, seconds};
    }
    return w;
  }();
  if (!watched && watch.at >= 0 &&
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= watch.at) {
    watched = true;
    auto* memory = REX_KERNEL_MEMORY();
    const uint32_t a = watch.address & 0x1FFFFFFF;
    std::vector<const void*> addresses = {memory->TranslatePhysical<const void*>(a),
                                          memory->TranslateVirtual<const void*>(0xA0000000u | a),
                                          memory->TranslateVirtual<const void*>(0xC0000000u | a),
                                          memory->TranslateVirtual<const void*>(0xE0000000u | a)};
    REXLOG_INFO("BF watch phys {:08X}: host {} {} {} {}", a, addresses[0], addresses[1], addresses[2], addresses[3]);
    dev::WatchHostWrites(addresses, watch.seconds, "phys");
  }
  Plan& plan = GetPlan();
  if (plan.at < 0 || plan.done) return;
  if (std::chrono::duration<double>(std::chrono::steady_clock::now() - plan.start).count() < plan.at) return;
  plan.done = true;
  for (const Range& r : plan.ranges) {
    const std::string path = plan.out + "\\phys_" + fmt::format("{:08X}", r.address) + ".bin";
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(REX_KERNEL_MEMORY()->TranslatePhysical<uint8_t*>(r.address), 1, r.size, f);
      std::fclose(f);
      REXLOG_INFO("BF dump: {:08X} size {:X} -> {}", r.address, r.size, path);
    }
  }
}
}  // namespace bf1943

#endif  // BF_DEV_TOOLS
