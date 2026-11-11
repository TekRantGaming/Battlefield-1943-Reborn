// Developer aid (BF_DEV_TOOLS builds only): the game's own type information (Frostbite's reflection) for some
// classes, read from the running game, where its field tables are filled in by the static initializers.
//
//   BF_DEV_TYPES=<class>[,<class>...]   (or "all": every class entry in the table)
//   BF_DEV_TYPES_AT=<seconds>           (default 25: after the boot)
//   BF_DEV_TYPES_OUT=<file>             (the text file written)
//
// A class entry in the image's type table is 24 bytes: name, flags (high half) and size (low half), the module, then
// alignment and field count, and two words the initializers fill (the field table among them). Written to
// <user data>\types.txt: each class's entry words, and each field's 24 bytes with any pointer to a name shown.
// For the modern movement and aiming mode (game_options.cpp): where the tuning values sit in each data object.

#if defined(BF_DEV_TOOLS)

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <rex/logging.h>

namespace bf1943 {

namespace {
uint32_t Load32(const uint8_t* base, uint32_t a) {
  return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
}
// A NUL ended name of letters, digits, '_' and '.' at a guest address in the image, or empty.
std::string Name(const uint8_t* base, uint32_t a) {
  if (a < 0x82000000 || a >= 0x82F00000) return {};
  std::string s;
  for (uint32_t i = 0; i < 80; ++i) {
    const char c = char(base[a + i]);
    if (!c) return s.size() >= 2 ? s : std::string();
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == ':')) return {};
    s += c;
  }
  return {};
}
}  // namespace

void DevTypesTick(const uint8_t* base) {
  static const std::string wanted = [] {
    const char* v = std::getenv("BF_DEV_TYPES");
    return std::string(v ? v : "");
  }();
  if (wanted.empty()) return;
  static const double at = [] {
    const char* v = std::getenv("BF_DEV_TYPES_AT");
    return v && *v ? std::atof(v) : 25.0;
  }();
  static const auto start = std::chrono::steady_clock::now();
  static std::atomic<bool> done{false};
  if (done || std::chrono::steady_clock::now() - start < std::chrono::duration<double>(at)) return;
  done = true;
  std::set<std::string> names;
  for (size_t pos = 0; pos < wanted.size();) {
    const size_t comma = std::min(wanted.find(',', pos), wanted.size());
    names.insert(wanted.substr(pos, comma - pos));
    pos = comma + 1;
  }
  const bool all = names.count("all") != 0;
  const char* out = std::getenv("BF_DEV_TYPES_OUT");
  if (!out || !*out) return;
  FILE* f = std::fopen(out, "w");
  if (!f) return;
  int classes = 0;
  // The type tables: the image's data from 0x82C00000 to 0x82E00000, entries on 4 byte boundaries.
  for (uint32_t e = 0x82C00000; e < 0x82E00000; e += 4) {
    const std::string name = Name(base, Load32(base, e));
    if (name.empty()) continue;
    const uint32_t w1 = Load32(base, e + 4), w2 = Load32(base, e + 8), w3 = Load32(base, e + 12);
    const uint32_t flags = w1 >> 16;
    // A class entry: flags 0x0035 or 0x0029 (classes and structs in what was seen), a module pointer in the image.
    if (!(flags == 0x0035 || flags == 0x0029 || flags == 0x0031 || flags == 0x0039) || w2 < 0x82000000 ||
        w2 >= 0x83000000)
      continue;
    if (!all && !names.count(name)) continue;
    const uint32_t w4 = Load32(base, e + 16), w5 = Load32(base, e + 20);
    const uint32_t count = (w3 >> 16) & 0xFF;
    std::fprintf(f, "class %s @%08X: flags %04X size %u module %08X align %u fields %u; +16 %08X +20 %08X\n",
                 name.c_str(), e, flags, w1 & 0xFFFF, w2, w3 >> 24, count, w4, w5);
    ++classes;
    // The field table: whichever of the two filled words points at entries with names.
    for (uint32_t table : {w4, w5}) {
      if (table < 0x40000000 || table >= 0xF0000000 || !count) continue;
      bool named = false;
      for (uint32_t i = 0; i < count && i < 128; ++i) {
        const uint32_t fe = table + i * 24;
        uint32_t w[6];
        for (int k = 0; k < 6; ++k) w[k] = Load32(base, fe + 4 * k);
        std::string fname = Name(base, w[0]);
        named = named || !fname.empty();
        std::fprintf(f, "  field %2u @%08X: %08X %08X %08X %08X %08X %08X  %s", i, fe, w[0], w[1], w[2], w[3], w[4],
                     w[5], fname.c_str());
        // A pointer to another type entry among the words: its name.
        for (int k = 1; k < 6; ++k) {
          const std::string tn = Name(base, w[k] >= 0x82000000 && w[k] < 0x83000000 ? Load32(base, w[k]) : 0);
          if (!tn.empty()) std::fprintf(f, "  [w%d type %s]", k, tn.c_str());
        }
        std::fprintf(f, "\n");
      }
      if (named) break;
    }
  }
  // BF_DEV_FIELDS=all: every field table the static initializers built (the data from 0x82D00000 to 0x82E00000):
  // runs of 24 byte field entries, each its name, flags, type, a word, the field's offset in its object, a word. A
  // run is one class's fields in order; the class is the one whose field count and names fit.
  if (const char* fv = std::getenv("BF_DEV_FIELDS"); fv && std::string(fv) == "all") {
    uint32_t run_start = 0, run_count = 0;
    for (uint32_t a = 0x82D00000; a < 0x82E00000; a += 4) {
      // A field entry: its name, flags in the high half's low byte (0 or 0x00480000), a type entry in the type
      // tables, a word, the field's offset in its object (small), a word.
      const std::string fname = Name(base, Load32(base, a));
      const uint32_t flags = Load32(base, a + 4), type = Load32(base, a + 8), offset = Load32(base, a + 16);
      if (fname.empty() || (flags & 0xFF00FFFF) || type < 0x82D00000 || type >= 0x82E00000 || offset >= 0x4000) continue;
      // The type's name: the first word of its entry that names something.
      std::string tname;
      for (int k = 0; k < 4 && tname.empty(); ++k) tname = Name(base, Load32(base, type + 4 * k));
      if (run_count && a == run_start + run_count * 24) {
        ++run_count;
      } else {
        run_start = a, run_count = 1;
        std::fprintf(f, "fields at %08X\n", a);
      }
      std::fprintf(f, "  +%04X %-40s %-24s type %08X flags %08X\n", offset, fname.c_str(), tname.c_str(), type, flags);
      a += 20;  // the next entry
    }
  } else if (const char* fv = std::getenv("BF_DEV_FIELDS"); fv && *fv) {
    const std::string fields = fv;
    for (size_t pos = 0; pos < fields.size();) {
      const size_t comma = std::min(fields.find(',', pos), fields.size());
      const std::string fname = fields.substr(pos, comma - pos);
      pos = comma + 1;
      // The name's string: NUL, the name, NUL.
      std::vector<uint32_t> strings;
      for (uint32_t a = 0x82000001; a < 0x82100000 && strings.size() < 4; ++a) {
        if (base[a - 1] == 0 && base[a + fname.size()] == 0 && !std::memcmp(base + a, fname.data(), fname.size()))
          strings.push_back(a);
      }
      for (uint32_t str : strings) {
        std::fprintf(f, "field name %s at %08X, pointed at from:\n", fname.c_str(), str);
        int hits = 0;
        for (uint32_t a = 0x82000000; a < 0x83000000 && hits < 8; a += 4) {
          if (Load32(base, a) != str) continue;
          ++hits;
          std::fprintf(f, "  %08X:", a);
          for (int k = -2; k < 8; ++k) std::fprintf(f, " %08X", Load32(base, a + 4 * k));
          std::fprintf(f, "\n");
        }
      }
    }
  }
  std::fclose(f);
  REXLOG_INFO("BF dev types: {} classes written to {}", classes, out);
}

}  // namespace bf1943

#endif
