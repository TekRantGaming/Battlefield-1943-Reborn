// The storms' radar patterns (D228; the fluid weather plan's step 2): the user, 20 Nov 2026: "The hurricane shape
// should look and spin like a real hurricane. Use real recorded weather radar data to base storms from."
//
// data\weather\storms\*.bin next to bf1943.exe, baked by tools/storm_bake.py from NWS NEXRAD composite reflectivity
// (the Iowa Environmental Mesonet's archive; public domain): Hurricane Michael's landfall, 10 Oct 2018, 17:00 to 17:55
// UTC, twelve frames five minutes apart, eye-centred, 256 x 256 at 1.17 km a pixel. Read once at the start and handed
// to the renderer (native_x_renderer::SetStormPattern), which builds the storm's clouds on it.
//
//   "BFSTORM1", width, height, frames (u32), the eye's radius in pixels, km a pixel, minutes between frames (f32),
//   then frames x height x width bytes.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <rex/logging.h>

#include <native_x_renderer/native_x_renderer.h>

namespace bf1943 {
std::filesystem::path ExecutableDir();  // shader_prep.cpp

void LoadStormPatterns() {
  const std::filesystem::path path = ExecutableDir() / "data" / "weather" / "storms" / "michael_2018.bin";
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    REXLOG_INFO("BF storms: no radar pattern at {} (the storm keeps its own shape)", path.string());
    return;
  }
  char magic[8] = {};
  uint32_t dims[3] = {};
  float meta[3] = {};
  in.read(magic, 8);
  in.read(reinterpret_cast<char*>(dims), sizeof(dims));
  in.read(reinterpret_cast<char*>(meta), sizeof(meta));
  if (!in || std::memcmp(magic, "BFSTORM1", 8) != 0 || dims[0] == 0 || dims[1] == 0 || dims[2] == 0 || dims[0] > 4096 ||
      dims[1] > 4096 || dims[2] > 256) {
    REXLOG_WARN("BF storms: {} is not a storm pattern", path.string());
    return;
  }
  std::vector<uint8_t> bytes(size_t(dims[0]) * dims[1] * dims[2]);
  in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
  if (!in) {
    REXLOG_WARN("BF storms: {} is cut short", path.string());
    return;
  }
  native_x_renderer::StormPattern pattern;
  pattern.frames = bytes.data();
  pattern.width = dims[0], pattern.height = dims[1], pattern.count = dims[2];
  pattern.eye_px = meta[0], pattern.km_per_px = meta[1], pattern.minutes = meta[2];
  native_x_renderer::SetStormPattern(pattern);
  REXLOG_INFO("BF storms: radar pattern {} ({} frames of {}x{}, {:.2f} km a pixel)", path.filename().string(), dims[2],
              dims[0], dims[1], meta[1]);
}

}  // namespace bf1943
