// Shader preparation from the player's own game files (TekRantGaming, 4 Nov 2026: no shipped shader code).
//
// The runtime keeps the shaders and pipelines it has made in storage files (<cache>\shaders\shareable\5841097E.xsh
// and 5841097E.*.xpso) and makes all of them when the game starts, so a stored shader or pipeline never makes a
// frame skip draws later (runs sh1 and sh2: 207 pipelines made during play and 2,191 draws skipped without them,
// none with them). We ship data\shaders\bf1943-shaders.list (tools\shader_list.py): for each shader the game
// uses, its hash and where its microcode is in the player's files, plus the merged pipeline descriptions (shader
// hashes and render state). Before the runtime opens its storage, this writes the shaders from the player's
// archives into the storage and merges the pipeline descriptions, once per list version.
//
// The players' renderer is NativeXRenderer (Vulkan), which keeps its own storage,
// <cache>\native_x_renderer\5841097E.pipelines: shaders (their microcode), translations and pipelines, all made at its first draw. We ship
// data\shaders\5841097E.nxr, that file's translations and pipelines (shader hashes, modifications and render state)
// without its shader records; here the shader records are written from the player's archives, so a first start makes
// every recorded pipeline before play instead of during it (6 Nov 2026: 1,688 pipelines, about 31,000 draws skipped
// in a first session without them).
//
// List lines: <hash> <p|v> <dwords> fbrb <archive> <entry> <offset> [<dword>:<xor> ...]
//             <hash> <p|v> <dwords> xex <guest address>
// Vertex shaders carry the few dwords the game's D3D patches at bind time as XOR masks. Every shader written is
// checked against its hash first.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/image_decode.h>  // DecodeImageRGBA: our TRG Studios logo (MakeTrgCard)

#include <native_x_renderer/native_x_renderer.h>
#define XXH_INLINE_ALL  // header only, no link to the SDK's xxhash.lib
#include <xxhash.h>

#include "../third_party/miniz/miniz.h"
#include "capture_points.h"
#include "dbx_tree.h"
#include "night_ops.h"

#include <map>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

REXCVAR_DECLARE(bool, server);  // server_mode.cpp: a dedicated server draws nothing (no grass map)

namespace bf1943 {
namespace {

namespace fs = std::filesystem;

constexpr char kTitle[] = "5841097E";
constexpr size_t kPipelineRecord = 72;

std::vector<uint8_t> ReadFile(const fs::path& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);  // one read (a byte at a time was most of the 3.4 s)
  if (!f) return {};
  const std::streamoff size = f.tellg();
  if (size <= 0) return {};
  std::vector<uint8_t> out(static_cast<size_t>(size));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(out.data()), size)) return {};
  return out;
}

bool WriteFileAtomic(const fs::path& path, const std::vector<uint8_t>& data) {
  fs::path temp = path;
  temp += ".tmp";
  {
    std::ofstream f(temp, std::ios::binary | std::ios::trunc);
    if (!f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()))) return false;
  }
  std::error_code ec;
  fs::rename(temp, path, ec);
  return !ec;
}

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t Be16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }
uint64_t Le64(const uint8_t* p) { return uint64_t(Le32(p + 4)) << 32 | Le32(p); }

// A gzip member's deflate data, inflated.
bool Gunzip(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
  if (size < 18 || data[0] != 0x1F || data[1] != 0x8B || data[2] != 8) return false;
  const uint8_t flags = data[3];
  size_t at = 10;
  if (flags & 4) at += 2 + (data[at] | data[at + 1] << 8);  // FEXTRA
  if (flags & 8) while (at < size && data[at++]) {}           // FNAME
  if (flags & 16) while (at < size && data[at++]) {}          // FCOMMENT
  if (flags & 2) at += 2;                                     // FHCRC
  if (at >= size) return false;
  size_t length = 0;
  void* p = tinfl_decompress_mem_to_heap(data + at, size - at, &length, 0);
  if (!p) return false;
  out.assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + length);
  mz_free(p);
  return true;
}

// A Frostbite 1 archive (.fbrb): "FbRB", u32 header size, the header (gzip), the data (gzip or stored). Header:
// version, string table size, strings, entry count, then per entry six words: name offset, flags, data offset,
// size, size, type offset. Same layout as tools\fbrb_extract.py.
struct Archive {
  std::vector<uint8_t> data;
  std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> entries;  // name -> offset, size
};

bool LoadArchive(const fs::path& path, Archive& out) {
  const std::vector<uint8_t> d = ReadFile(path);
  if (d.size() < 8 || std::memcmp(d.data(), "FbRB", 4) != 0) return false;
  const uint32_t header_size = Be32(&d[4]);
  if (8 + size_t(header_size) > d.size()) return false;
  std::vector<uint8_t> h;
  if (!Gunzip(&d[8], header_size, h) || h.size() < 12) return false;
  const uint32_t strings = Be32(&h[4]);
  if (8 + size_t(strings) + 4 > h.size()) return false;
  const uint8_t* table = &h[8];
  const uint8_t* rest = table + strings;
  const uint32_t count = Be32(rest);
  if (size_t(rest - h.data()) + 4 + size_t(count) * 24 > h.size()) return false;
  const uint8_t* raw = &d[8 + header_size];
  const size_t raw_size = d.size() - 8 - header_size;
  if (raw_size >= 2 && raw[0] == 0x1F && raw[1] == 0x8B) {
    if (!Gunzip(raw, raw_size, out.data)) return false;
  } else {
    out.data.assign(raw, raw + raw_size);
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = rest + 4 + i * 24;
    const uint32_t name = Be32(e);
    if (name >= strings) continue;
    out.entries[reinterpret_cast<const char*>(table + name)] = {Be32(e + 8), Be32(e + 12)};
  }
  return true;
}

struct Line {
  uint64_t hash = 0;
  bool pixel = false;
  uint32_t dwords = 0;
  std::string kind, archive, entry;
  uint32_t offset = 0;  // fbrb: byte offset into the entry; xex: guest address
  std::vector<std::pair<uint32_t, uint32_t>> patches;
};

std::vector<Line> ParseList(const std::string& text, std::vector<uint8_t>& xsh_header) {
  std::vector<Line> lines;
  std::istringstream in(text);
  std::string row;
  while (std::getline(in, row)) {
    if (row.empty()) continue;
    if (row[0] == '#') {
      const size_t at = row.find("header ");
      if (at != std::string::npos) {
        const std::string hex = row.substr(at + 7, 16);
        for (size_t i = 0; i + 1 < hex.size(); i += 2) xsh_header.push_back(uint8_t(std::stoul(hex.substr(i, 2), nullptr, 16)));
      }
      continue;
    }
    std::istringstream f(row);
    Line l;
    std::string hash, type;
    f >> hash >> type >> l.dwords >> l.kind;
    l.hash = std::stoull(hash, nullptr, 16);
    l.pixel = type == "p";
    if (l.kind == "fbrb") {
      f >> l.archive >> l.entry >> l.offset;
      for (size_t p; (p = l.entry.find("%20")) != std::string::npos;) l.entry.replace(p, 3, " ");
      std::string patch;
      while (f >> patch) {
        const size_t colon = patch.find(':');
        l.patches.push_back({uint32_t(std::stoul(patch.substr(0, colon))),
                             uint32_t(std::stoul(patch.substr(colon + 1), nullptr, 16))});
      }
    } else if (l.kind == "xex") {
      std::string address;
      f >> address;
      l.offset = uint32_t(std::stoul(address, nullptr, 16));
    }
    if (l.dwords) lines.push_back(std::move(l));
  }
  return lines;
}

// The whole records of a storage file (a half written record ends the runtime's reading of it, so only records
// whose hash matches are kept; King Kong's RepairShaderStorage does the same).
void ReadShaderRecords(const std::vector<uint8_t>& d, std::vector<uint8_t>& out, std::unordered_set<uint64_t>& have) {
  for (size_t at = 8; at + 12 <= d.size();) {
    const size_t size = 12 + size_t(Le32(&d[at + 8]) & 0x7FFFFFFF) * 4;
    if (at + size > d.size()) break;
    const uint64_t hash = Le64(&d[at]);
    if (XXH3_64bits(&d[at + 12], size - 12) != hash) break;
    if (have.insert(hash).second) out.insert(out.end(), d.begin() + at, d.begin() + at + size);
    at += size;
  }
}

void MergePipelines(const std::vector<uint8_t>& from, std::vector<uint8_t>& into_records,
                    std::unordered_set<uint64_t>& have, size_t& added) {
  for (size_t at = 12; at + kPipelineRecord <= from.size(); at += kPipelineRecord) {
    const uint64_t hash = Le64(&from[at]);
    if (XXH3_64bits(&from[at + 8], kPipelineRecord - 8) != hash) continue;
    if (have.insert(hash).second) {
      into_records.insert(into_records.end(), from.begin() + at, from.begin() + at + kPipelineRecord);
      ++added;
    }
  }
}

}  // namespace

std::filesystem::path ExecutableDir() {
#if defined(_WIN32)
  wchar_t buffer[32768];
  const DWORD n = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
  return fs::path(std::wstring(buffer, n)).parent_path();
#else
  std::error_code ec;
  return fs::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

// Where the player's own folders go (user, game): beside bf1943.exe, or on Linux beside the AppImage file. The
// AppImage's program runs from a read only mount (or AppRun's copy of it), so its own folder cannot hold them; the
// AppImage runtime gives the file's own path in $APPIMAGE (King Kong keeps its files beside the AppImage the same way).
std::filesystem::path PlayerRoot() {
#if !defined(_WIN32)
  if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage) return fs::path(appimage).parent_path();
#endif
  return ExecutableDir();
}

// Writes the listed shaders and pipelines into the runtime's storage. data_dir holds the list (data\shaders next
// to the exe), game is the game folder. Called once the xex image is mapped (shaders built into the xex are read
// from guest memory) and before the runtime opens its shader storage.
void PrepareShaders(const fs::path& data_dir, const fs::path& game, const fs::path& cache) {
  const auto start = std::chrono::steady_clock::now();
  const fs::path list_path = data_dir / "bf1943-shaders.list";
  const std::vector<uint8_t> list_bytes = ReadFile(list_path);
  if (list_bytes.empty()) {
    REXLOG_INFO("BF shaders: no shader list at {}", list_path.string());
    return;
  }
  std::vector<fs::path> pipeline_files;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(data_dir, ec))
    if (e.path().extension() == ".xpso" || e.path().extension() == ".nxr") pipeline_files.push_back(e.path());

  // One preparation per list version: the marker holds the list's and pipeline files' hash.
  uint64_t version = XXH3_64bits(list_bytes.data(), list_bytes.size());
  for (const auto& p : pipeline_files) {
    const auto d = ReadFile(p);
    version ^= XXH3_64bits(d.data(), d.size()) + p.filename().string().size();
  }
  const fs::path shareable = cache / "shaders" / "shareable";
  fs::create_directories(shareable, ec);
  const fs::path marker = shareable / "bf1943-prepared.txt";
  const fs::path xsh_path = shareable / (std::string(kTitle) + ".xsh");
  const fs::path nxr_path = cache / "native_x_renderer" / (std::string(kTitle) + ".pipelines");
  char version_text[32];
  std::snprintf(version_text, sizeof(version_text), "%016llx", static_cast<unsigned long long>(version));
  {
    std::ifstream f(marker);
    std::string have;
    if (std::getline(f, have) && have == version_text && fs::exists(xsh_path) && fs::exists(nxr_path)) {
      REXLOG_INFO("BF shaders: prepared already (list {})", version_text);
      return;
    }
  }

  std::vector<uint8_t> header;
  const std::string text(list_bytes.begin(), list_bytes.end());
  std::vector<Line> lines = ParseList(text, header);
  if (header.size() != 8) header = {'X', 'E', 'S', 'H', 0x19, 0x12, 0x20, 0x20};

  // Shaders already in the storage stay; ours are added.
  std::vector<uint8_t> records;
  std::unordered_set<uint64_t> have;
  const std::vector<uint8_t> old = ReadFile(xsh_path);
  if (old.size() >= 8) {
    header.assign(old.begin(), old.begin() + 8);
    ReadShaderRecords(old, records, have);
  }

  std::map<std::string, std::vector<const Line*>> by_archive;
  size_t added = 0, failed = 0;
  std::vector<uint8_t> ucode;
  std::vector<std::pair<const Line*, std::vector<uint8_t>>> made;  // every listed shader, checked (NativeXRenderer's)
  auto add = [&](const Line& l, const uint8_t* source) {
    ucode.assign(source, source + size_t(l.dwords) * 4);
    for (const auto& [index, mask] : l.patches) {
      if (index >= l.dwords) continue;
      const uint32_t v = Le32(&ucode[index * 4]) ^ mask;
      std::memcpy(&ucode[index * 4], &v, 4);  // host little endian, as the storage holds it
    }
    if (XXH3_64bits(ucode.data(), ucode.size()) != l.hash) {
      ++failed;
      return;
    }
    made.push_back({&l, ucode});
    if (have.count(l.hash)) return;  // in the Xenos path's storage already
    uint8_t head[12];
    std::memcpy(head, &l.hash, 8);
    const uint32_t word = l.dwords | (l.pixel ? 0x80000000u : 0);
    std::memcpy(head + 8, &word, 4);
    records.insert(records.end(), head, head + 12);
    records.insert(records.end(), ucode.begin(), ucode.end());
    have.insert(l.hash);
    ++added;
  };
  const size_t had = have.size();
  for (const Line& l : lines) {
    if (l.kind == "xex") {
      if (l.offset >= 0x82000000u) add(l, REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(l.offset));
      else ++failed;
    } else if (l.kind == "fbrb") {
      by_archive[l.archive].push_back(&l);
    }
  }
  // The archives read on threads of their own (each inflates tens of MB: 3.4 s one after another, before the window
  // shows anything); each keeps only its shaders' bytes, added here in order.
  struct Read {
    std::vector<std::pair<const Line*, std::vector<uint8_t>>> found;
    size_t missing = 0;
    bool loaded = false;
  };
  std::vector<std::pair<const std::string*, const std::vector<const Line*>*>> jobs;
  for (const auto& [name, wanted] : by_archive) jobs.push_back({&name, &wanted});
  std::vector<Read> reads(jobs.size());
  {
    std::atomic<size_t> next{0};
    auto work = [&] {
      for (size_t j; (j = next.fetch_add(1)) < jobs.size();) {
        Archive archive;
        Read& r = reads[j];
        r.loaded = LoadArchive(game / fs::path(*jobs[j].first), archive);
        if (!r.loaded) continue;
        for (const Line* l : *jobs[j].second) {
          auto e = archive.entries.find(l->entry);
          if (e == archive.entries.end() || size_t(l->offset) + size_t(l->dwords) * 4 > e->second.second ||
              size_t(e->second.first) + e->second.second > archive.data.size()) {
            ++r.missing;
            continue;
          }
          const uint8_t* at = archive.data.data() + e->second.first + l->offset;
          r.found.push_back({l, std::vector<uint8_t>(at, at + size_t(l->dwords) * 4)});
        }
      }
    };
    // At most three at once: each holds its inflated archive (up to about 150 MB) while it is read.
    std::vector<std::thread> threads;
    for (size_t i = 0; i < std::min<size_t>(3, jobs.size()); ++i) threads.emplace_back(work);
    for (auto& t : threads) t.join();
  }
  for (size_t j = 0; j < jobs.size(); ++j) {
    const Read& r = reads[j];
    if (!r.loaded) {
      REXLOG_WARN("BF shaders: could not read {}", (game / *jobs[j].first).string());
      failed += jobs[j].second->size();
      continue;
    }
    failed += r.missing;
    for (const auto& [l, bytes] : r.found) add(*l, bytes.data());
  }
  std::vector<uint8_t> xsh = header;
  xsh.insert(xsh.end(), records.begin(), records.end());
  if (!WriteFileAtomic(xsh_path, xsh)) {
    REXLOG_WARN("BF shaders: could not write {}", xsh_path.string());
    return;
  }

  // Pipeline descriptions: merged by record hash into the file of the same name.
  size_t pipelines = 0;
  for (const auto& p : pipeline_files) {
    if (p.extension() != ".xpso") continue;
    std::vector<uint8_t> ours = ReadFile(p);
    if (ours.size() < 12) continue;
    // The list of 4 Nov shipped its records without the file's 12 byte header ('XEPS', 'DXRT' or 'DXRO', the
    // version), so every record was read 12 bytes off and none was merged (0 pipelines, all built during play).
    // Such a file gets the header the runtime writes itself (runs vs30, sh1).
    if (std::memcmp(ours.data(), "XEPS", 4) != 0 && ours.size() % kPipelineRecord == 0) {
      const bool rov = p.filename().string().find(".rov.") != std::string::npos;
      const uint8_t head[12] = {'X', 'E', 'P', 'S', 'D', 'X', 'R', uint8_t(rov ? 'O' : 'T'), 0x20, 0x26, 0x02, 0x26};
      ours.insert(ours.begin(), head, head + 12);
    }
    const fs::path target = shareable / p.filename();
    std::vector<uint8_t> current = ReadFile(target);
    std::vector<uint8_t> out_records;
    std::unordered_set<uint64_t> seen;
    size_t ignored = 0;
    if (current.size() >= 12 && std::memcmp(current.data(), ours.data(), 12) == 0) MergePipelines(current, out_records, seen, ignored);
    MergePipelines(ours, out_records, seen, pipelines);
    std::vector<uint8_t> out(ours.begin(), ours.begin() + 12);
    out.insert(out.end(), out_records.begin(), out_records.end());
    WriteFileAtomic(target, out);
  }
  // NativeXRenderer's storage: what it holds stays, our shaders (from the game files), translations and pipelines are
  // added.
  size_t nxr_shaders = 0, nxr_translations = 0, nxr_pipelines = 0;
  bool nxr_ok = true;
  for (const auto& p : pipeline_files) {
    if (p.extension() != ".nxr") continue;
    const std::vector<uint8_t> seed = ReadFile(p);
    if (seed.size() < 16 || Le32(seed.data()) != 0x53504E58) continue;
    const uint32_t state_size = Le32(&seed[8]);
    const size_t pipeline_record = 1 + 32 + size_t(state_size);
    std::vector<uint8_t> out(seed.begin(), seed.begin() + 16);
    std::unordered_set<uint64_t> keys;  // shaders by type and hash; translations and pipelines by their bytes
    auto shader_key = [](uint8_t type, uint64_t hash) { return hash ^ (0x9E3779B97F4A7C15ull * (uint64_t(type) + 1)); };
    // A record of the file (tagged as NativeXRenderer's storage.cpp writes them): its size, 0 if cut short or unknown.
    auto record_size = [&](const std::vector<uint8_t>& d, size_t at) -> size_t {
      const uint8_t tag = d[at];
      size_t n = 0;
      if (tag == 'S' && at + 14 <= d.size()) n = 14 + size_t(Le32(&d[at + 2])) * 4;
      else if (tag == 'T') n = 18;
      else if (tag == 'P') n = pipeline_record;
      return n && at + n <= d.size() ? n : 0;
    };
    auto record_key = [&](const std::vector<uint8_t>& d, size_t at, size_t n) {
      return d[at] == 'S' ? shader_key(d[at + 1], Le64(&d[at + 6])) : XXH3_64bits(&d[at], n);
    };
    const std::vector<uint8_t> current = ReadFile(nxr_path);
    if (current.size() >= 16 && std::memcmp(current.data(), seed.data(), 16) == 0) {
      for (size_t at = 16, n; at < current.size() && (n = record_size(current, at)); at += n) {
        if (keys.insert(record_key(current, at, n)).second) out.insert(out.end(), current.begin() + at, current.begin() + at + n);
      }
    }
    // Shaders first: NativeXRenderer finds a record's shaders among those read before it.
    for (const auto& [l, code] : made) {
      if (!keys.insert(shader_key(l->pixel ? 1 : 0, l->hash)).second) continue;
      out.push_back('S');
      out.push_back(l->pixel ? 1 : 0);
      const uint32_t dwords = l->dwords;
      out.insert(out.end(), reinterpret_cast<const uint8_t*>(&dwords), reinterpret_cast<const uint8_t*>(&dwords) + 4);
      out.insert(out.end(), reinterpret_cast<const uint8_t*>(&l->hash), reinterpret_cast<const uint8_t*>(&l->hash) + 8);
      out.insert(out.end(), code.begin(), code.end());
      ++nxr_shaders;
    }
    for (size_t at = 16, n; at < seed.size() && (n = record_size(seed, at)); at += n) {
      if (seed[at] == 'S' || !keys.insert(record_key(seed, at, n)).second) continue;
      out.insert(out.end(), seed.begin() + at, seed.begin() + at + n);
      ++(seed[at] == 'T' ? nxr_translations : nxr_pipelines);
    }
    fs::create_directories(nxr_path.parent_path(), ec);
    if (!WriteFileAtomic(nxr_path, out)) {
      REXLOG_WARN("BF shaders: could not write {}", nxr_path.string());
      nxr_ok = false;
    }
  }
  if (!failed && nxr_ok) {
    std::ofstream f(marker, std::ios::trunc);
    f << version_text << "\n";
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  REXLOG_INFO("BF shaders: prepared {} shaders from the game files ({} already there, {} failed) and {} pipeline "
              "descriptions in {} ms; NativeXRenderer: {} shaders, {} translations and {} pipelines added",
              added, had, failed, pipelines, ms.count(), nxr_shaders, nxr_translations, nxr_pipelines);
}

// One entry of a Frostbite archive, for bf_font.cpp (BF's menu lettering from the player's game).
bool ReadFbrbEntry(const fs::path& archive, const std::string& name, std::vector<uint8_t>& out) {
  Archive a;
  if (!LoadArchive(archive, a)) return false;
  auto it = a.entries.find(name);
  if (it == a.entries.end() || size_t(it->second.first) + it->second.second > a.data.size()) return false;
  out.assign(a.data.begin() + it->second.first, a.data.begin() + it->second.first + it->second.second);
  return true;
}

// Several entries at one read of the archive (the fonts: build_overlay is 32 MB to inflate). False when one is
// missing; `out` then holds nothing.
bool ReadFbrbEntries(const fs::path& archive, const std::vector<std::string>& names, std::vector<std::vector<uint8_t>>& out) {
  out.clear();
  Archive a;
  if (!LoadArchive(archive, a)) return false;
  for (const std::string& name : names) {
    auto it = a.entries.find(name);
    if (it == a.entries.end() || size_t(it->second.first) + it->second.second > a.data.size()) {
      out.clear();
      return false;
    }
    out.emplace_back(a.data.begin() + it->second.first, a.data.begin() + it->second.first + it->second.second);
  }
  return true;
}

// ---- pictures from the player's game files for our menus (SERVERS-PLAN.md section 2): never shipped, made once
// into the user's cache after the game is installed. The server browser's map pictures are each level's own loading
// screen picture (levels\<map>\ui\multiplayer-00.fbrb, Levels/<Map>/UI/Load_Image_<Name>.res), 512 pixels wide.
namespace {

// The Xbox 360's 2D tiled layout (XGAddress2DTiledOffset), in elements of `bytes` bytes (DXT blocks or texels).
uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t width, uint32_t bytes) {
  const uint32_t aligned = (width + 31) & ~31u;
  const uint32_t lb = (bytes >> 2) + ((bytes >> 1) >> (bytes >> 2));
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (lb + 7);
  const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << lb;
  const uint32_t off = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4);
  return (((off & ~511u) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >>
         lb;
}

void Rgb565(uint16_t c, int out[3]) {
  out[0] = ((c >> 11) & 31) * 255 / 31, out[1] = ((c >> 5) & 63) * 255 / 63, out[2] = (c & 31) * 255 / 31;
}

// A XenonTexture entry (an optional 0x40 byte "RES" wrapper, then a 0x5C byte header: format at +0x08, width +0x10,
// height +0x14) as RGBA: DXT1 (0), DXT3 (1), DXT5 (2) and A8R8G8B8 (9), tiled, 16 bit words swapped for DXT.
bool DecodeXenonTexture(const std::vector<uint8_t>& res, std::vector<uint8_t>& rgba, int& w, int& h) {
  size_t at = res.size() >= 3 && std::memcmp(res.data(), "RES", 3) == 0 ? 0x40 : 0;
  if (res.size() < at + 0x5C) return false;
  const uint8_t* hd = &res[at];
  const uint32_t fmt = Be32(hd + 8);
  w = int(Be32(hd + 16)), h = int(Be32(hd + 20));
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
  const uint8_t* data = hd + 0x5C;
  const size_t size = res.size() - at - 0x5C;
  rgba.assign(size_t(w) * h * 4, 255);
  if (fmt == 9) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const size_t o = size_t(TiledOffset(uint32_t(x), uint32_t(y), uint32_t(w), 4)) * 4;
        if (o + 4 > size) continue;
        uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
        p[0] = data[o + 1], p[1] = data[o + 2], p[2] = data[o + 3], p[3] = data[o];
      }
    return true;
  }
  if (fmt == 10) {  // one byte a texel (the fonts' glyph coverage): white, the byte its alpha
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const size_t o = size_t(TiledOffset(uint32_t(x), uint32_t(y), uint32_t(w), 1));
        if (o >= size) continue;
        uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
        p[0] = p[1] = p[2] = 255, p[3] = data[o];
      }
    return true;
  }
  if (fmt > 2) return false;
  const uint32_t bs = fmt == 0 ? 8 : 16, bw = uint32_t(std::max(1, w / 4)), bh = uint32_t(std::max(1, h / 4));
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const size_t o = size_t(TiledOffset(bx, by, bw, bs)) * bs;
      if (o + bs > size) continue;
      uint8_t b[16];
      for (uint32_t i = 0; i < bs; i += 2) b[i] = data[o + i + 1], b[i + 1] = data[o + i];  // 16 bit words swapped
      const uint8_t* cb = b + bs - 8;
      const uint16_t c0 = uint16_t(cb[0] | cb[1] << 8), c1 = uint16_t(cb[2] | cb[3] << 8);
      const uint32_t idx = uint32_t(cb[4]) | uint32_t(cb[5]) << 8 | uint32_t(cb[6]) << 16 | uint32_t(cb[7]) << 24;
      int pal[4][3];
      Rgb565(c0, pal[0]), Rgb565(c1, pal[1]);
      const bool four = c0 > c1 || fmt != 0;
      for (int k = 0; k < 3; ++k) {
        pal[2][k] = four ? (2 * pal[0][k] + pal[1][k]) / 3 : (pal[0][k] + pal[1][k]) / 2;
        pal[3][k] = four ? (pal[0][k] + 2 * pal[1][k]) / 3 : 0;
      }
      int alpha[16];
      for (int i = 0; i < 16; ++i) alpha[i] = 255;
      if (fmt == 1) {
        for (int i = 0; i < 16; ++i) alpha[i] = ((b[i / 2] >> (4 * (i & 1))) & 15) * 17;
      } else if (fmt == 2) {
        const int a0 = b[0], a1 = b[1];
        uint64_t bits = 0;
        for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
        int ap[8] = {a0, a1};
        for (int i = 1; i < 7; ++i) ap[i + 1] = a0 > a1 ? ((7 - i) * a0 + i * a1) / 7 : 0;
        if (a0 <= a1) {
          for (int i = 1; i < 5; ++i) ap[i + 1] = ((5 - i) * a0 + i * a1) / 5;
          ap[6] = 0, ap[7] = 255;
        }
        for (int i = 0; i < 16; ++i) alpha[i] = ap[(bits >> (3 * i)) & 7];
      }
      for (int i = 0; i < 16; ++i) {
        const uint32_t s = (idx >> (2 * i)) & 3;
        const int x = int(bx) * 4 + i % 4, y = int(by) * 4 + i / 4;
        if (x >= w || y >= h) continue;
        uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
        p[0] = uint8_t(pal[s][0]), p[1] = uint8_t(pal[s][1]), p[2] = uint8_t(pal[s][2]);
        p[3] = uint8_t(fmt == 0 && !four && s == 3 ? 0 : alpha[i]);
      }
    }
  return true;
}

// The picture scaled down to `to_w` wide (a box filter), as RGB.
std::vector<uint8_t> ShrinkRgb(const std::vector<uint8_t>& rgba, int w, int h, int to_w, int& to_h) {
  const int f = std::max(1, w / to_w);
  to_w = w / f, to_h = h / f;
  std::vector<uint8_t> out(size_t(to_w) * to_h * 3);
  for (int y = 0; y < to_h; ++y)
    for (int x = 0; x < to_w; ++x)
      for (int c = 0; c < 3; ++c) {
        int sum = 0;
        for (int j = 0; j < f; ++j)
          for (int i = 0; i < f; ++i) sum += rgba[((size_t(y) * f + j) * w + size_t(x) * f + i) * 4 + c];
        out[(size_t(y) * to_w + x) * 3 + c] = uint8_t(sum / (f * f));
      }
  return out;
}

}  // namespace

bool EaAudioToWav(const std::vector<uint8_t>& wave_res, const std::vector<uint8_t>* stream, const fs::path& wav,
                  float scale);  // game_music.cpp

// The grass's plants (TekRantGaming, 9 Nov: textured, and more than one kind; NativeXRenderer grass.cpp reads the file): the
// game's own vegetation textures, from the player's game files, each plant cropped to where its picture is (its alpha)
// and stored 512 x 512, with its average colour and its width over its height. Never shipped: made in the user's cache.
// File (vegetation-2.bin: 12 kinds; vegetation-1.bin had 7): "BFVG", version 1, kinds, size; per kind its average r, g, b and aspect (floats); then each kind's RGBA.
namespace {
constexpr int kVegSize = 512;
struct VegKind {
  std::vector<uint8_t> rgba;  // kVegSize x kVegSize
  float avg[3] = {}, aspect = 1;
};

// The part of `rgba` (w x h) in [x0, x1) and [y0, y1) (all its rows: y1 0) cropped to its alpha's bounds, scaled to
// kVegSize square (bilinear).
bool CropPlant(const std::vector<uint8_t>& rgba, int w, int h, int x0, int x1, VegKind& out, int y0 = 0, int y1 = 0) {
  if (y1 <= 0) y1 = h;
  int bx0 = x1, bx1 = x0, by0 = y1, by1 = y0;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x)
      if (rgba[(size_t(y) * w + x) * 4 + 3] > 64) bx0 = std::min(bx0, x), bx1 = std::max(bx1, x + 1), by0 = std::min(by0, y),
                                                 by1 = std::max(by1, y + 1);
  if (bx1 - bx0 < 8 || by1 - by0 < 8) return false;
  const int cw = bx1 - bx0, ch = by1 - by0;
  out.aspect = float(cw) / float(ch);
  out.rgba.assign(size_t(kVegSize) * kVegSize * 4, 0);
  double sum[3] = {}, weight = 0;
  for (int y = 0; y < kVegSize; ++y)
    for (int x = 0; x < kVegSize; ++x) {
      const float fx = bx0 + (x + 0.5f) * cw / kVegSize - 0.5f, fy = by0 + (y + 0.5f) * ch / kVegSize - 0.5f;
      const int ix = std::clamp(int(std::floor(fx)), bx0, bx1 - 1), iy = std::clamp(int(std::floor(fy)), by0, by1 - 1);
      const int jx = std::min(ix + 1, bx1 - 1), jy = std::min(iy + 1, by1 - 1);
      const float ax = std::clamp(fx - ix, 0.0f, 1.0f), ay = std::clamp(fy - iy, 0.0f, 1.0f);
      uint8_t* o = &out.rgba[(size_t(y) * kVegSize + x) * 4];
      for (int c = 0; c < 4; ++c) {
        auto at = [&](int xx, int yy) { return float(rgba[(size_t(yy) * w + xx) * 4 + c]); };
        const float v = (at(ix, iy) * (1 - ax) + at(jx, iy) * ax) * (1 - ay) + (at(ix, jy) * (1 - ax) + at(jx, jy) * ax) * ay;
        o[c] = uint8_t(std::clamp(v + 0.5f, 0.0f, 255.0f));
      }
      if (o[3] > 128) {
        for (int c = 0; c < 3; ++c) sum[c] += o[c];
        weight += 1;
      }
    }
  for (int c = 0; c < 3; ++c) out.avg[c] = weight > 0 ? float(sum[c] / weight / 255.0) : 0.4f;
  return true;
}

// `rgba` (w x h) turned a quarter to the left: its left edge at the bottom (a palm frond, its stem at its left, stood
// up on its stem).
std::vector<uint8_t> TurnLeft(const std::vector<uint8_t>& rgba, int& w, int& h) {
  std::vector<uint8_t> out(rgba.size());
  const int nw = h, nh = w;
  for (int y = 0; y < nh; ++y)
    for (int x = 0; x < nw; ++x) std::memcpy(&out[(size_t(y) * nw + x) * 4], &rgba[(size_t(x) * w + (w - 1 - y)) * 4], 4);
  w = nw, h = nh;
  return out;
}

// The columns of `rgba` holding a picture, as runs (separate plants side by side in one texture), widest first.
std::vector<std::pair<int, int>> PlantColumns(const std::vector<uint8_t>& rgba, int w, int h) {
  std::vector<std::pair<int, int>> runs;
  int start = -1;
  for (int x = 0; x <= w; ++x) {
    bool any = false;
    for (int y = 0; x < w && y < h && !any; ++y) any = rgba[(size_t(y) * w + x) * 4 + 3] > 64;
    if (any && start < 0) start = x;
    if (!any && start >= 0) {
      if (x - start >= 8) runs.push_back({start, x});
      start = -1;
    }
  }
  return runs;
}

bool MakeVegetation(const fs::path& game, const fs::path& file) {
  const fs::path levels = game / "dist" / "xenon" / "levels";
  std::map<std::string, std::vector<uint8_t>> res;
  auto take = [&](const fs::path& archive, std::initializer_list<const char*> names) {
    Archive a;
    if (!LoadArchive(archive, a)) {
      REXLOG_WARN("BF game art: {} could not be read", archive.string());
      return;
    }
    for (const char* n : names) {
      auto it = a.entries.find(n);
      if (it != a.entries.end() && size_t(it->second.first) + it->second.second <= a.data.size())
        res[n].assign(a.data.begin() + it->second.first, a.data.begin() + it->second.first + it->second.second);
    }
  };
  take(levels / "levels-common-00.fbrb", {"Objects/Beach_Objects/Vegetation/Textures/Beach_Grass_01_C.res",
                                          "Textures/Undergrowth/UG_Set_01.res", "Textures/Undergrowth/jungleleaf_2.res",
                                          "Textures/Trees/MEBushLeaves_01.res", "Textures/Trees/AcaciaLeaves_01.res",
                                          "Textures/Trees/BeechLeaves_02.res", "Textures/Trees/PalmTreeFrond_01.res"});
  take(levels / "guadal_canal" / "reduced-level-00.fbrb", {"Objects/Beach_Objects/Vegetation/Textures/leave_branch.res"});
  auto decode = [&](const char* n, std::vector<uint8_t>& rgba, int& w, int& h) {
    auto it = res.find(n);
    return it != res.end() && DecodeXenonTexture(it->second, rgba, w, h);
  };
  // The kinds, in the order grass.cpp places them: two halves of the grass strip, the undergrowth set's three plants
  // (a long leaf, a fern, a leafy weed: its pictures side by side, left to right), the jungle leaf, the leafy branch,
  // then (TekRantGaming, 9 Nov: more kinds) a tall leafy weed, two small leafy bushes (the acacia's and the beech's leaves),
  // and a young palm's frond, green and dried (the frond texture's two halves, each stood up on its stem).
  std::vector<VegKind> kinds;
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
  REXLOG_INFO("BF game art: {} vegetation textures found", res.size());
  if (!decode("Objects/Beach_Objects/Vegetation/Textures/Beach_Grass_01_C.res", rgba, w, h)) {
    REXLOG_WARN("BF game art: the grass texture could not be decoded");
    return false;
  }
  for (int half = 0; half < 2; ++half) {
    VegKind k;
    if (!CropPlant(rgba, w, h, half * w / 2, (half + 1) * w / 2, k)) {
      REXLOG_WARN("BF game art: the grass texture ({}x{}) has no picture in its half {}", w, h, half);
      return false;
    }
    kinds.push_back(std::move(k));
  }
  if (decode("Textures/Undergrowth/UG_Set_01.res", rgba, w, h)) {
    std::vector<std::pair<int, int>> runs = PlantColumns(rgba, w, h);
    for (const auto& [x0, x1] : runs) {
      VegKind k;
      if (CropPlant(rgba, w, h, x0, x1, k)) kinds.push_back(std::move(k));
      if (kinds.size() == 5) break;
    }
  }
  while (kinds.size() < 5) kinds.push_back(kinds[0]);  // no undergrowth set: grass in its places
  for (const char* n : {"Textures/Undergrowth/jungleleaf_2.res", "Objects/Beach_Objects/Vegetation/Textures/leave_branch.res"}) {
    VegKind k;
    if (decode(n, rgba, w, h) && CropPlant(rgba, w, h, 0, w, k)) kinds.push_back(std::move(k));
    else kinds.push_back(kinds[0]);
  }
  for (const char* n : {"Textures/Trees/MEBushLeaves_01.res", "Textures/Trees/AcaciaLeaves_01.res", "Textures/Trees/BeechLeaves_02.res"}) {
    VegKind k;
    if (decode(n, rgba, w, h) && CropPlant(rgba, w, h, 0, w, k)) kinds.push_back(std::move(k));
    else kinds.push_back(kinds[0]);
  }
  if (decode("Textures/Trees/PalmTreeFrond_01.res", rgba, w, h)) {
    const int half = h / 2;
    for (int part = 0; part < 2; ++part) {
      std::vector<uint8_t> strip(rgba.begin() + size_t(part) * half * w * 4, rgba.begin() + size_t(part + 1) * half * w * 4);
      int sw = w, sh = half;
      std::vector<uint8_t> up = TurnLeft(strip, sw, sh);
      VegKind k;
      if (CropPlant(up, sw, sh, 0, sw, k)) kinds.push_back(std::move(k));
      else kinds.push_back(kinds[0]);
    }
  }
  while (kinds.size() < 12) kinds.push_back(kinds[0]);
  std::vector<uint8_t> out;
  auto put32 = [&](uint32_t v) { out.insert(out.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); };
  auto putf = [&](float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    put32(u);
  };
  out.insert(out.end(), {'B', 'F', 'V', 'G'});
  put32(1), put32(uint32_t(kinds.size())), put32(kVegSize);
  for (const VegKind& k : kinds) putf(k.avg[0]), putf(k.avg[1]), putf(k.avg[2]), putf(k.aspect);
  for (const VegKind& k : kinds) out.insert(out.end(), k.rgba.begin(), k.rgba.end());
  return WriteFileAtomic(file, out);
}
}  // namespace

namespace {
// Three passes of a box blur of radius r (close to a Gaussian of that width) over one plane, in place, the edges
// repeated.
void BlurPlane(std::vector<float>& p, int w, int h, int r) {
  std::vector<float> tmp(p.size());
  const float scale = 1.0f / float(2 * r + 1);
  for (int pass = 0; pass < 3; ++pass) {
    for (int y = 0; y < h; ++y) {
      const float* row = &p[size_t(y) * w];
      float* out = &tmp[size_t(y) * w];
      double sum = 0;
      for (int i = -r; i <= r; ++i) sum += row[std::clamp(i, 0, w - 1)];
      for (int x = 0; x < w; ++x) {
        out[x] = float(sum) * scale;
        sum += row[std::clamp(x + r + 1, 0, w - 1)] - row[std::clamp(x - r, 0, w - 1)];
      }
    }
    for (int x = 0; x < w; ++x) {
      double sum = 0;
      for (int i = -r; i <= r; ++i) sum += tmp[size_t(std::clamp(i, 0, h - 1)) * w + x];
      for (int y = 0; y < h; ++y) {
        p[size_t(y) * w + x] = float(sum) * scale;
        sum += tmp[size_t(std::clamp(y + r + 1, 0, h - 1)) * w + x] - tmp[size_t(std::clamp(y - r, 0, h - 1)) * w + x];
      }
    }
  }
}

// The TRG Studios card (TekRantGaming, 10 Nov 2026: his studio's logo, a card of ours before the game's title). His card
// is his logo on the game's own card paper, so only the logo ships (data/startup/trg_studios.png: its ink's colour
// and how much of it, nothing of the game's) and the paper is made here from the player's own four logo cards (EA,
// DICE, Frostbite, Dolby, 1024 x 512): each pixel the brightest of the four, the specks left where their logos overlap
// filled from the paper around them, then warmed as his card's paper is (green 99.3%, blue 96.6%). The card is the
// logo's size (2:1, as the other cards). Never shipped: made in the user's cache with the cards.
// The card paper from the four logo cards (RGB): the brightest of the four at each pixel (the logos never overlap),
// the specks where they do filled from around them (`filled` says how many).
bool PaperFromCards(const std::vector<std::vector<uint8_t>>& cards, int w, int h, std::vector<float>& paper, int& filled) {
  const size_t n = size_t(w) * h;
  if (cards.size() != 4 || w <= 0 || h <= 0) return false;
  for (const auto& c : cards)
    if (c.size() != n * 3) return false;
  paper.assign(n * 3, 0.0f);
  for (size_t i = 0; i < n * 3; ++i) {
    uint8_t m = 0;
    for (const auto& c : cards) m = std::max(m, c[i]);
    paper[i] = m;
  }
  std::vector<float> lum(n);
  for (size_t i = 0; i < n; ++i) lum[i] = (paper[i * 3] + paper[i * 3 + 1] + paper[i * 3 + 2]) / 3.0f;
  std::vector<float> around = lum;
  BlurPlane(around, w, h, 6);
  std::vector<float> speck(n);
  for (size_t i = 0; i < n; ++i) speck[i] = lum[i] < around[i] - 10.0f ? 1.0f : 0.0f;
  BlurPlane(speck, w, h, 2);
  std::vector<float> keep(n);
  double avg[3] = {}, kept = 0;
  for (size_t i = 0; i < n; ++i) {
    keep[i] = speck[i] > 0.05f ? 0.0f : 1.0f;
    if (keep[i] > 0) avg[0] += paper[i * 3], avg[1] += paper[i * 3 + 1], avg[2] += paper[i * 3 + 2], kept += 1;
  }
  std::vector<float> weight = keep;
  BlurPlane(weight, w, h, 6);
  filled = 0;
  for (int c = 0; c < 3; ++c) {
    std::vector<float> plane(n);
    for (size_t i = 0; i < n; ++i) plane[i] = paper[i * 3 + c] * keep[i];
    BlurPlane(plane, w, h, 6);
    for (size_t i = 0; i < n; ++i) {
      if (keep[i] > 0) continue;
      paper[i * 3 + c] = weight[i] > 0.01f ? plane[i] / weight[i] : float(avg[c] / std::max(1.0, kept));
      if (c == 0) ++filled;
    }
  }
  return true;
}

bool MakeTrgCard(const std::vector<std::vector<uint8_t>>& cards, int w, int h, const fs::path& logo_file,
                 const fs::path& out) {
  std::vector<float> paper;
  int filled = 0;
  if (!PaperFromCards(cards, w, h, paper, filled)) return false;
  std::ifstream f(logo_file, std::ios::binary);
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  int lw = 0, lh = 0;
  const std::vector<uint8_t> logo =
      bytes.empty() ? std::vector<uint8_t>{} : rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), lw, lh);
  if (logo.empty() || lw <= 0 || lh <= 0) return false;
  static constexpr float kWarm[3] = {1.0f, 0.993f, 0.966f};
  std::vector<uint8_t> rgb(size_t(lw) * lh * 3);
  for (int y = 0; y < lh; ++y) {
    const float sy = std::clamp((y + 0.5f) * float(h) / float(lh) - 0.5f, 0.0f, float(h - 1));
    const int y0 = int(sy), y1 = std::min(y0 + 1, h - 1);
    const float fy = sy - float(y0);
    for (int x = 0; x < lw; ++x) {
      const float sx = std::clamp((x + 0.5f) * float(w) / float(lw) - 0.5f, 0.0f, float(w - 1));
      const int x0 = int(sx), x1 = std::min(x0 + 1, w - 1);
      const float fx = sx - float(x0);
      const uint8_t* ink = &logo[(size_t(y) * lw + x) * 4];
      const float alpha = ink[3] / 255.0f;
      for (int c = 0; c < 3; ++c) {
        auto at = [&](int xx, int yy) { return paper[(size_t(yy) * w + xx) * 3 + c]; };
        const float p = (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
        const float v = p * kWarm[c] * (1 - alpha) + float(ink[c]) * alpha;
        rgb[(size_t(y) * lw + x) * 3 + c] = uint8_t(std::clamp(v + 0.5f, 0.0f, 255.0f));
      }
    }
  }
  size_t length = 0;
  void* png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), lw, lh, 3, &length, 6, MZ_FALSE);
  if (!png) return false;
  const std::vector<uint8_t> out_bytes(static_cast<uint8_t*>(png), static_cast<uint8_t*>(png) + length);
  mz_free(png);
  REXLOG_INFO("BF game art: the TRG Studios card ({}x{}) on the player's own card paper ({} specks filled)", lw, lh,
              filled);
  return WriteFileAtomic(out, out_bytes);
}

// The logo music with one more bar (TekRantGaming, 10 Nov 2026: a beat for the TRG card, with the game's title still on the
// music's big hit). The music (14.5 s) is in 4/4 at about 125 beats a minute, a bar every 1.9 s, and the game's five
// logo cards were on the first beats of bars 1 to 5 (the drums' hits at 0.52, 2.44, 4.42, 6.27 s and the climax at
// 8.08 s). Bar 3 is played twice: cut where it is quiet before bars 3 and 4 (4.18 s and 6.02 s, both 0.24 s before
// the hit), a 12 ms crossfade at the join (its largest step is smaller than the music's own there), 1.828 s longer.
// Made from the player's own logos.wav only (the times were found in that decode; another length is left alone).
bool MakeIntroMusic(const fs::path& in, const fs::path& out) {
  std::ifstream f(in, std::ios::binary);
  const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0) return false;
  auto le16 = [&](size_t o) { return uint32_t(d[o]) | uint32_t(d[o + 1]) << 8; };
  auto le32 = [&](size_t o) { return le16(o) | le16(o + 2) << 16; };
  uint32_t rate = 0, channels = 0, bits = 0;
  size_t data_at = 0, data_size = 0;
  for (size_t o = 12; o + 8 <= d.size();) {
    const uint32_t size = le32(o + 4);
    if (std::memcmp(d.data() + o, "fmt ", 4) == 0 && o + 24 <= d.size()) {
      if (le16(o + 8) != 1) return false;  // PCM only
      channels = le16(o + 10), rate = le32(o + 12), bits = le16(o + 22);
    } else if (std::memcmp(d.data() + o, "data", 4) == 0) {
      data_at = o + 8, data_size = std::min<size_t>(size, d.size() - data_at);
      break;
    }
    o += 8 + size + (size & 1);
  }
  if (bits != 16 || channels == 0 || rate == 0 || data_at == 0) return false;
  const size_t frames = data_size / (size_t(channels) * 2);
  const double seconds = double(frames) / rate;
  if (std::fabs(seconds - 14.486) > 0.1) {
    REXLOG_WARN("BF game art: the logo music is {:.3f} s, not the 14.486 s its bars were timed in: no extra bar", seconds);
    return false;
  }
  const size_t a = size_t(4.18 * rate), j = size_t(6.02 * rate), x = size_t(0.012 * rate);
  std::vector<int16_t> s(frames * channels);
  std::memcpy(s.data(), d.data() + data_at, s.size() * 2);  // little endian, as this PC
  std::vector<int16_t> o;
  o.reserve((frames + (j - a)) * channels);
  o.insert(o.end(), s.begin(), s.begin() + (j - x) * channels);
  for (size_t i = 0; i < x; ++i) {
    const float t = float(i) / float(x);
    for (uint32_t c = 0; c < channels; ++c) {
      const float v = s[(j - x + i) * channels + c] * (1 - t) + s[(a + i) * channels + c] * t;
      o.push_back(int16_t(std::clamp(std::lround(v), -32768L, 32767L)));
    }
  }
  o.insert(o.end(), s.begin() + (a + x) * channels, s.begin() + j * channels);
  o.insert(o.end(), s.begin() + j * channels, s.end());
  std::vector<uint8_t> wav(44 + o.size() * 2);
  auto put16 = [&](size_t at, uint32_t v) { wav[at] = uint8_t(v), wav[at + 1] = uint8_t(v >> 8); };
  auto put32 = [&](size_t at, uint32_t v) { put16(at, v & 0xFFFF), put16(at + 2, v >> 16); };
  std::memcpy(wav.data(), "RIFF", 4), put32(4, uint32_t(wav.size() - 8)), std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
  put32(16, 16), put16(20, 1), put16(22, channels), put32(24, rate), put32(28, rate * channels * 2);
  put16(32, channels * 2), put16(34, 16), std::memcpy(wav.data() + 36, "data", 4), put32(40, uint32_t(o.size() * 2));
  std::memcpy(wav.data() + 44, o.data(), o.size() * 2);
  REXLOG_INFO("BF game art: the logo music with bar 3 played twice ({:.3f} s)", double(o.size() / channels) / rate);
  return WriteFileAtomic(out, wav);
}
}  // namespace

// ---- the menus' own art (TekRantGaming, 12 Nov 2026: "extract anything from the game files and use it within these
// menus ... Assets, texts, fonts, textures, anything"): the pieces the launcher's new look draws, cut from the
// atlases BF's menus draw from (menu\main\mainmenu-00.fbrb and menu\hud\hud_multiplayer_cq-00.fbrb), once into
// user\cache\extracted\menu\, never shipped. The sprites' numbers are the atlases' own (atlas_meta.dbx; the pictures
// were picked in runs\ui-designs, 12 Nov). The fonts go through bf_font.cpp (BfFontPrepare).
void BfFontPrepare(const fs::path& game_dir, const fs::path& user_dir);  // bf_font.cpp

namespace {

constexpr int kMenuArtVersion = 2;  // 2: the Xbox bumpers and triggers (12 Nov 2026)
constexpr const char* kMapIconsFile = "icons-1.png";  // the pause menu map's icons; its number follows the letters' look

struct Picture {
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
};

bool MenuArtReady(const fs::path& out_dir) {
  std::ifstream f(out_dir / "menu" / "ready.txt");
  int v = 0;
  return bool(f) && bool(f >> v) && v >= kMenuArtVersion;
}

// An archive entry by its name, case aside (the archives mix cases: Menu/... and menu/...).
const uint8_t* FindEntry(const Archive& a, std::string name, size_t& size) {
  for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
  for (const auto& [n, where] : a.entries) {
    if (n.size() != name.size()) continue;
    bool same = true;
    for (size_t i = 0; i < n.size() && same; ++i) same = char(std::tolower(static_cast<unsigned char>(n[i]))) == name[i];
    if (!same) continue;
    if (size_t(where.first) + where.second > a.data.size()) return nullptr;
    size = where.second;
    return a.data.data() + where.first;
  }
  return nullptr;
}

bool DecodeEntry(const Archive& a, const std::string& name, Picture& out) {
  size_t size = 0;
  const uint8_t* p = FindEntry(a, name, size);
  if (!p) return false;
  const std::vector<uint8_t> res(p, p + size);
  return DecodeXenonTexture(res, out.rgba, out.w, out.h) && out.w > 0 && out.h > 0;
}

// The sprites of an atlas by their numbers: each one's place in it (the UV rects of its atlas_meta.dbx).
struct SpriteRect {
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};
std::map<std::string, SpriteRect> AtlasRects(const Archive& a, const std::string& meta_name) {
  std::map<std::string, SpriteRect> out;
  size_t size = 0;
  const uint8_t* p = FindEntry(a, meta_name, size);
  if (!p) return out;
  std::vector<dbx::Node> roots;
  if (!dbx::Parse(std::vector<uint8_t>(p, p + size), roots)) return out;
  for (const dbx::Node* inst : dbx::Instances(roots, "UI.UITextureAtlasInfo")) {
    const dbx::Node* file = inst->Field("TextureFile");
    const dbx::Node *u0 = inst->Field("MinU"), *u1 = inst->Field("MaxU"), *v0 = inst->Field("MinV"), *v1 = inst->Field("MaxV");
    SpriteRect r;
    if (!file || !u0 || !u1 || !v0 || !v1 || !u0->Float(r.u0) || !u1->Float(r.u1) || !v0->Float(r.v0) || !v1->Float(r.v1))
      continue;
    out[file->text] = r;
  }
  return out;
}

// A sprite cut from its atlas, trimmed to where it has any alpha (drawn to its own shape).
bool CutSprite(const Picture& atlas, const SpriteRect& r, Picture& out, bool trim) {
  int x0 = std::clamp(int(std::lround(r.u0 * atlas.w)), 0, atlas.w), x1 = std::clamp(int(std::lround(r.u1 * atlas.w)), 0, atlas.w);
  int y0 = std::clamp(int(std::lround(r.v0 * atlas.h)), 0, atlas.h), y1 = std::clamp(int(std::lround(r.v1 * atlas.h)), 0, atlas.h);
  if (x1 <= x0 || y1 <= y0) return false;
  if (trim) {
    int tx0 = x1, ty0 = y1, tx1 = x0, ty1 = y0;
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x)
        if (atlas.rgba[(size_t(y) * atlas.w + x) * 4 + 3] > 8)
          tx0 = std::min(tx0, x), tx1 = std::max(tx1, x + 1), ty0 = std::min(ty0, y), ty1 = std::max(ty1, y + 1);
    if (tx1 <= tx0 || ty1 <= ty0) return false;
    x0 = tx0, x1 = tx1, y0 = ty0, y1 = ty1;
  }
  out.w = x1 - x0, out.h = y1 - y0;
  out.rgba.resize(size_t(out.w) * out.h * 4);
  for (int y = 0; y < out.h; ++y)
    std::memcpy(&out.rgba[size_t(y) * out.w * 4], &atlas.rgba[(size_t(y0 + y) * atlas.w + x0) * 4], size_t(out.w) * 4);
  return true;
}

bool WritePicture(const Picture& p, const fs::path& path, bool rgb) {
  std::vector<uint8_t> data;
  const uint8_t* src = p.rgba.data();
  int channels = 4;
  if (rgb) {
    data.resize(size_t(p.w) * p.h * 3);
    for (size_t i = 0; i < size_t(p.w) * p.h; ++i)
      data[i * 3] = p.rgba[i * 4], data[i * 3 + 1] = p.rgba[i * 4 + 1], data[i * 3 + 2] = p.rgba[i * 4 + 2];
    src = data.data(), channels = 3;
  }
  size_t length = 0;
  void* png = tdefl_write_image_to_png_file_in_memory_ex(src, p.w, p.h, channels, &length, 6, MZ_FALSE);
  if (!png) return false;
  const std::vector<uint8_t> bytes(static_cast<uint8_t*>(png), static_cast<uint8_t*>(png) + length);
  mz_free(png);
  return WriteFileAtomic(path, bytes);
}

struct Cut {
  const char* atlas;   // the atlas texture entry
  const char* meta;    // its sprite table
  const char* sprite;  // the sprite's number in it
  const char* out;     // our name for it
};

int CutAll(const Archive& a, const fs::path& dir, const std::vector<Cut>& cuts) {
  int made = 0;
  std::string loaded;
  Picture atlas;
  std::map<std::string, SpriteRect> rects;
  for (const Cut& c : cuts) {
    if (loaded != c.atlas) {
      loaded = c.atlas;
      atlas = Picture{};
      rects.clear();
      if (!DecodeEntry(a, c.atlas, atlas)) {
        REXLOG_WARN("BF menu art: {} could not be decoded", c.atlas);
        continue;
      }
      rects = AtlasRects(a, c.meta);
    }
    if (atlas.w == 0) continue;
    const std::string out = c.out;
    const bool whole = out == "backdrop";
    auto it = rects.find(c.sprite);
    Picture sprite;
    if (it == rects.end() || !CutSprite(atlas, it->second, sprite, !whole)) {
      REXLOG_WARN("BF menu art: sprite {} ({}) is not in {}", c.sprite, out, c.meta);
      continue;
    }
    if (WritePicture(sprite, dir / (out + ".png"), whole)) ++made;
  }
  return made;
}

// A whole texture entry as one picture.
bool CutWhole(const Archive& a, const fs::path& dir, const std::string& entry, const std::string& out, bool trim) {
  Picture p;
  if (!DecodeEntry(a, entry, p)) {
    REXLOG_WARN("BF menu art: {} ({}) could not be decoded", entry, out);
    return false;
  }
  if (trim) {
    Picture t;
    const SpriteRect all{0, 0, 1, 1};
    if (CutSprite(p, all, t, true)) p = std::move(t);
  }
  return WritePicture(p, dir / (out + ".png"), false);
}

// The card paper (the panels' paper), from the four logo cards already in the cache (startup/card1 to card4).
bool MakePaper(const fs::path& out_dir) {
  std::vector<std::vector<uint8_t>> cards;
  int w = 0, h = 0;
  for (int i = 1; i <= 4; ++i) {
    std::ifstream f(out_dir / "startup" / ("card" + std::to_string(i) + ".png"), std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    int cw = 0, ch = 0;
    const std::vector<uint8_t> rgba = bytes.empty() ? std::vector<uint8_t>{} : rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), cw, ch);
    if (rgba.empty() || cw <= 0 || ch <= 0 || (w && (cw != w || ch != h))) return false;
    w = cw, h = ch;
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    for (size_t p = 0; p < size_t(w) * h; ++p) rgb[p * 3] = rgba[p * 4], rgb[p * 3 + 1] = rgba[p * 4 + 1], rgb[p * 3 + 2] = rgba[p * 4 + 2];
    cards.push_back(std::move(rgb));
  }
  std::vector<float> paper;
  int filled = 0;
  if (!PaperFromCards(cards, w, h, paper, filled)) return false;
  static constexpr float kWarm[3] = {1.0f, 0.993f, 0.966f};  // as the TRG card's paper
  Picture p;
  p.w = w, p.h = h;
  p.rgba.resize(size_t(w) * h * 4, 255);
  for (size_t i = 0; i < size_t(w) * h; ++i)
    for (int c = 0; c < 3; ++c) p.rgba[i * 4 + c] = uint8_t(std::clamp(paper[i * 3 + c] * kWarm[c] + 0.5f, 0.0f, 255.0f));
  return WritePicture(p, out_dir / "menu" / "paper.png", true);
}

void ExtractMenuArt(const fs::path& game, const fs::path& out_dir) {
  const auto t0 = std::chrono::steady_clock::now();
  const fs::path dir = out_dir / "menu";
  std::error_code ec;
  fs::create_directories(dir, ec);
  int made = 0;
  const fs::path xenon = game / "dist" / "xenon";
  {
    Archive a;
    if (!LoadArchive(xenon / "menu" / "main" / "mainmenu-00.fbrb", a)) {
      REXLOG_WARN("BF menu art: the main menu archive could not be read");
    } else {
      static const char* kShared = "menu/menu_shared_images_textures_xenon/atlas0.res";
      static const char* kSharedMeta = "menu/menu_shared_images_textures_xenon/atlas_meta.dbx";
      static const char* kComp = "menu/igm_comp_images_textures_xenon/atlas0.res";
      static const char* kCompMeta = "menu/igm_comp_images_textures_xenon/atlas_meta.dbx";
      static const char* kPopups = "menu/main_popups_textures_xenon/atlas0.res";
      static const char* kPopupsMeta = "menu/main_popups_textures_xenon/atlas_meta.dbx";
      static const char* kCommon = "menu/menu_common_textures_xenon/atlas0.res";
      static const char* kCommonMeta = "menu/menu_common_textures_xenon/atlas_meta.dbx";
      made += CutAll(a, dir, {{kShared, kSharedMeta, "12", "logo"},        {kShared, kSharedMeta, "9", "backdrop"},
                              {kShared, kSharedMeta, "1", "corsair"},      {kShared, kSharedMeta, "5", "zero"},
                              {kComp, kCompMeta, "28", "brush_bar"},       {kComp, kCompMeta, "31", "brush_a"},
                              {kComp, kCompMeta, "34", "brush_b"},         {kComp, kCompMeta, "37", "brush_c"},
                              {kComp, kCompMeta, "63", "brush_line"},      {kComp, kCompMeta, "25", "brush_block_a"},
                              {kComp, kCompMeta, "45", "brush_block_b"},   {kComp, kCompMeta, "60", "brush_small"},
                              {kComp, kCompMeta, "10", "portrait_us_rifleman"}, {kComp, kCompMeta, "14", "portrait_us_scout"},
                              {kComp, kCompMeta, "2", "portrait_us_infantry"},  {kComp, kCompMeta, "18", "portrait_ijn_a"},
                              {kComp, kCompMeta, "22", "portrait_ijn_b"},  {kComp, kCompMeta, "6", "portrait_ijn_c"},
                              {kComp, kCompMeta, "48", "pad_xbox"},        {kComp, kCompMeta, "40", "pad_ps3"},
                              {kPopups, kPopupsMeta, "20", "glyph_a"},     {kPopups, kPopupsMeta, "30", "glyph_b"},
                              {kPopups, kPopupsMeta, "16", "glyph_x"},     {kPopups, kPopupsMeta, "26", "glyph_y"},
                              {kPopups, kPopupsMeta, "35", "glyph_refresh"},
                              // (55, 61, 48 and 47 beside them are the PlayStation set: L1, R1, L2, R2)
                              {kCommon, kCommonMeta, "57", "glyph_lb"},    {kCommon, kCommonMeta, "63", "glyph_rb"},
                              {kCommon, kCommonMeta, "51", "glyph_lt"},    {kCommon, kCommonMeta, "50", "glyph_rt"}});
      for (int i = 1; i <= 25; ++i) {
        char entry[64], out[16];
        std::snprintf(entry, sizeof(entry), "awards/images/ranks/r%03d.res", i);
        std::snprintf(out, sizeof(out), "rank_%02d", i);
        if (CutWhole(a, dir, entry, out, true)) ++made;
      }
      if (CutWhole(a, dir, "menu/images/mainnav/mp_anypic.res", "polaroid", false)) ++made;
    }
  }
  {
    Archive a;
    if (!LoadArchive(xenon / "menu" / "hud" / "hud_multiplayer_cq-00.fbrb", a)) {
      REXLOG_WARN("BF menu art: the HUD archive could not be read");
    } else {
      static const char* kCq = "menu/igm_mp_cq_textures_xenon/atlas0.res";
      static const char* kCqMeta = "menu/igm_mp_cq_textures_xenon/atlas_meta.dbx";
      made += CutAll(a, dir, {{kCq, kCqMeta, "223", "flag_us"},        {kCq, kCqMeta, "225", "flag_jp"},
                              {kCq, kCqMeta, "273", "class_rifleman"}, {kCq, kCqMeta, "278", "class_infantry"},
                              {kCq, kCqMeta, "283", "class_scout"},    {kCq, kCqMeta, "311", "icon_tank"},
                              {kCq, kCqMeta, "313", "icon_plane"},     {kCq, kCqMeta, "319", "icon_jeep"},
                              {kCq, kCqMeta, "315", "icon_boat"},      {kCq, kCqMeta, "339", "icon_tank_w"},
                              {kCq, kCqMeta, "329", "icon_plane_w"},   {kCq, kCqMeta, "337", "icon_jeep_w"},
                              {kCq, kCqMeta, "333", "icon_boat_w"}});
      if (CutWhole(a, dir, "textures/ui/minimap/minimapcompass.res", "compass", true)) ++made;
    }
  }
  if (MakePaper(out_dir)) ++made;
  else REXLOG_WARN("BF menu art: the card paper was not made (the startup cards are not in the cache yet)");
  {
    std::ofstream f(dir / "ready.txt", std::ios::trunc);
    f << kMenuArtVersion << "\n";
  }
  REXLOG_INFO("BF menu art: {} pictures from the game's menus in {} ms", made,
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}

}  // namespace

const char* MapIconsFileName() { return kMapIconsFile; }

void ExtractGameArt(const fs::path& game, const fs::path& out_dir) {
  static const std::pair<const char*, const char*> kMaps[] = {
      {"wake", "wake_island_s"}, {"iwo", "iwo_jima_s"}, {"guadalcanal", "guadal_canal"}, {"coral", "coral_sea"}};
  std::vector<std::pair<std::string, std::string>> todo;
  for (const auto& [key, folder] : kMaps)
    if (!fs::exists(out_dir / "maps" / (std::string(key) + ".png"))) todo.push_back({key, folder});
  // The launcher's menu music (game_music.cpp): the game's main menu track, at 60% (King Kong's launcher plays at half).
  const bool music = !fs::exists(out_dir / "music" / "menu.wav");
  const bool plants = !fs::exists(out_dir / "vegetation-2.bin");
  // The startup cards and their music (TekRantGaming, 10 Nov 2026: the startup videos played by us, the game not started for
  // them): the game's own six pictures and its logo music, from build_overlay-00.fbrb (game_menu.cpp IntroPage).
  // Since 0.1.30 also the TRG Studios card on the player's paper and the music with its extra bar (MakeTrgCard,
  // MakeIntroMusic), made once into a cache that has the game's own cards already.
  const fs::path trg_logo = ExecutableDir() / "data" / "startup" / "trg_studios.png";
  const bool startup = !fs::exists(out_dir / "startup" / "logos.wav") || !fs::exists(out_dir / "startup" / "card5.png") ||
                       !fs::exists(out_dir / "startup" / "intro.wav") ||
                       (fs::exists(trg_logo) && !fs::exists(out_dir / "startup" / "trg.png"));
  // The menus' own art and fonts (ExtractMenuArt, bf_font.cpp BfFontPrepare), after the cards (the paper needs them).
  const bool menu = !MenuArtReady(out_dir);
  const bool fonts = !fs::exists(out_dir.parent_path() / "bf_font_headline_large.bin");
  // The pause menu's map (TekRantGaming, 13 Nov 2026: "the current pause menu doesn't have a map which makes bombers
  // hard to use"): each level's own map picture (the one its minimap and deploy screen draw, 1024 x 1024 over 2048 m)
  // and the game's map icons with the capture point flags' letters drawn in (capture_points.cpp), in
  // cache\extracted\minimap.
  std::vector<std::string> minimaps;
  for (const auto& [key, folder] : kMaps)
    if (!fs::exists(out_dir / "minimap" / (std::string(folder) + ".png"))) minimaps.push_back(folder);
  const bool map_icons = !fs::exists(out_dir / "minimap" / kMapIconsFile);
  if (todo.empty() && !music && !plants && !startup && !menu && !fonts && minimaps.empty() && !map_icons) return;
  std::thread([game, out_dir, todo, music, plants, startup, trg_logo, menu, fonts, minimaps, map_icons] {
    if (plants) {
      std::error_code made;
      fs::create_directories(out_dir, made);
      const auto t0 = std::chrono::steady_clock::now();
      if (MakeVegetation(game, out_dir / "vegetation-2.bin"))
        REXLOG_INFO("BF game art: the grass's plants in {} ms",
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
      else
        REXLOG_WARN("BF game art: no grass plants made (the game's vegetation textures were not found)");
    }
    std::error_code ec;
    fs::create_directories(out_dir / "maps", ec);
    for (const auto& [key, folder] : todo) {
      const auto t0 = std::chrono::steady_clock::now();
      Archive a;
      if (!LoadArchive(game / "dist" / "xenon" / "levels" / folder / "ui" / "multiplayer-00.fbrb", a)) {
        REXLOG_WARN("BF game art: the {} archive could not be read", folder);
        continue;
      }
      bool done = false;
      for (const auto& [name, where] : a.entries) {
        std::string lower = name;
        for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (lower.find("/ui/load_image_") == std::string::npos || lower.size() < 4 || lower.substr(lower.size() - 4) != ".res")
          continue;
        if (size_t(where.first) + where.second > a.data.size()) break;
        const std::vector<uint8_t> res(a.data.begin() + where.first, a.data.begin() + where.first + where.second);
        std::vector<uint8_t> rgba;
        int w = 0, h = 0, sh = 0, sw = 512;
        if (!DecodeXenonTexture(res, rgba, w, h)) break;
        const std::vector<uint8_t> rgb = ShrinkRgb(rgba, w, h, sw, sh);
        size_t length = 0;
        void* png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), sw, sh, 3, &length, 6, MZ_FALSE);
        if (!png) break;
        const std::vector<uint8_t> bytes(static_cast<uint8_t*>(png), static_cast<uint8_t*>(png) + length);
        mz_free(png);
        done = WriteFileAtomic(out_dir / "maps" / (key + ".png"), bytes);
        REXLOG_INFO("BF game art: {} map picture from {} ({}x{} to {}x{}) in {} ms", key, name, w, h, sw, sh,
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        break;
      }
      if (!done) REXLOG_WARN("BF game art: no map picture made for {}", key);
    }
    if (!minimaps.empty() || map_icons) fs::create_directories(out_dir / "minimap", ec);
    for (const std::string& folder : minimaps) {
      const auto t0 = std::chrono::steady_clock::now();
      Archive a;
      if (!LoadArchive(game / "dist" / "xenon" / "levels" / folder / "reduced-level-00.fbrb", a)) {
        REXLOG_WARN("BF game art: the {} level archive could not be read: no map in the pause menu there", folder);
        continue;
      }
      bool done = false;
      for (const auto& [name, where] : a.entries) {
        std::string lower = name;
        for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (lower.find("/ui/minimap/") == std::string::npos || lower.size() < 4 || lower.substr(lower.size() - 4) != ".res")
          continue;
        if (size_t(where.first) + where.second > a.data.size()) continue;
        const std::vector<uint8_t> res(a.data.begin() + where.first, a.data.begin() + where.first + where.second);
        Picture p;
        if (!DecodeXenonTexture(res, p.rgba, p.w, p.h) || p.w != p.h || p.w < 256) continue;
        done = WritePicture(p, out_dir / "minimap" / (folder + ".png"), true);
        REXLOG_INFO("BF game art: {} minimap picture from {} ({}x{}) in {} ms", folder, name, p.w, p.h,
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        break;
      }
      if (!done) REXLOG_WARN("BF game art: no minimap picture made for {}", folder);
    }
    if (map_icons) {
      Archive a;
      Picture p;
      if (LoadArchive(game / "dist" / "xenon" / "menu" / "hud" / "hud_multiplayer_cq-00.fbrb", a) &&
          DecodeEntry(a, "menu/minimap/icons0.res", p) && DrawFlagLetters(p.rgba, uint32_t(p.w), uint32_t(p.h)) > 0 &&
          WritePicture(p, out_dir / "minimap" / kMapIconsFile, false))
        REXLOG_INFO("BF game art: the map's icons ({}x{}, the flags lettered)", p.w, p.h);
      else
        REXLOG_WARN("BF game art: the map's icons were not made: the pause menu's map draws its own marks only");
    }
    if (startup) {
      const auto t0 = std::chrono::steady_clock::now();
      Archive a;
      if (!LoadArchive(game / "dist" / "xenon" / "build_overlay-00.fbrb", a)) {
        REXLOG_WARN("BF game art: the startup cards' archive could not be read");
      } else {
        fs::create_directories(out_dir / "startup", ec);
        static const std::pair<const char*, const char*> kCards[] = {
            {"startupsplash/esrbnotice.res", "esrb"},       {"startupsplash/islands_splash1.res", "card1"},
            {"startupsplash/islands_splash2.res", "card2"}, {"startupsplash/islands_splash3.res", "card3"},
            {"startupsplash/islands_splash4.res", "card4"}, {"startupsplash/islands_splash5.res", "card5"}};
        int cards = 0;
        bool sound = false;
        std::vector<std::vector<uint8_t>> logo_cards(4);  // card1 to card4 (RGB, 1024 x 512): the TRG card's paper
        int logo_w = 0, logo_h = 0;
        for (const auto& [name, where] : a.entries) {
          std::string lower = name;
          for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
          if (size_t(where.first) + where.second > a.data.size()) continue;
          const std::vector<uint8_t> res(a.data.begin() + where.first, a.data.begin() + where.first + where.second);
          if (lower.find("bsfx_ui_logos_01_wave.res") != std::string::npos) {
            sound = EaAudioToWav(res, nullptr, out_dir / "startup" / "logos.wav", 1.0f);
            continue;
          }
          for (const auto& [src, dst] : kCards) {
            if (lower.find(src) == std::string::npos) continue;
            std::vector<uint8_t> rgba;
            int w = 0, h = 0, sh = 0;
            if (!DecodeXenonTexture(res, rgba, w, h) || w <= 0 || h <= 0) break;
            int sw = std::min(w, 1280);  // ShrinkRgb only shrinks
            const std::vector<uint8_t> rgb = ShrinkRgb(rgba, w, h, sw, sh);
            size_t length = 0;
            void* png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), sw, sh, 3, &length, 6, MZ_FALSE);
            if (!png) break;
            const std::vector<uint8_t> bytes(static_cast<uint8_t*>(png), static_cast<uint8_t*>(png) + length);
            mz_free(png);
            if (WriteFileAtomic(out_dir / "startup" / (std::string(dst) + ".png"), bytes)) ++cards;
            const std::string card = dst;
            if (card.size() == 5 && card.rfind("card", 0) == 0 && card[4] >= '1' && card[4] <= '4' &&
                (logo_w == 0 || (logo_w == sw && logo_h == sh)))
              logo_cards[size_t(card[4] - '1')] = rgb, logo_w = sw, logo_h = sh;
            break;
          }
        }
        REXLOG_INFO("BF game art: {} startup cards and {} their music in {} ms", cards, sound ? "with" : "without",
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        if (sound && !MakeIntroMusic(out_dir / "startup" / "logos.wav", out_dir / "startup" / "intro.wav"))
          REXLOG_WARN("BF game art: the logo music's extra bar was not made (the intro plays the game's five cards)");
        if (fs::exists(trg_logo) &&
            !MakeTrgCard(logo_cards, logo_w, logo_h, trg_logo, out_dir / "startup" / "trg.png"))
          REXLOG_WARN("BF game art: the TRG Studios card was not made");
      }
    }
    if (music) {
      const auto t0 = std::chrono::steady_clock::now();
      Archive menu, streams;
      const fs::path xenon = game / "dist" / "xenon";
      std::vector<uint8_t> header, stream;
      if (LoadArchive(xenon / "menu" / "main" / "mainmenu-00.fbrb", menu)) {
        auto it = menu.entries.find("Sound/Beach_Music/BMUS_Menu_001_Wave.res");
        if (it != menu.entries.end() && size_t(it->second.first) + it->second.second <= menu.data.size())
          header.assign(menu.data.begin() + it->second.first, menu.data.begin() + it->second.first + it->second.second);
      }
      menu = {};
      if (!header.empty() && LoadArchive(xenon / "streaming_sounds-00.fbrb", streams)) {
        auto it = streams.entries.find("Sound/Beach_Music/BMUS_Menu_001_WaveDataSns_1.res");
        if (it != streams.entries.end() && size_t(it->second.first) + it->second.second <= streams.data.size())
          stream.assign(streams.data.begin() + it->second.first, streams.data.begin() + it->second.first + it->second.second);
      }
      streams = {};
      if (header.empty() || stream.empty() || !EaAudioToWav(header, &stream, out_dir / "music" / "menu.wav", 0.6f))
        REXLOG_WARN("BF game art: no menu music made");
      else
        REXLOG_INFO("BF game art: the menu music in {} ms",
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    }
    if (menu) ExtractMenuArt(game, out_dir);
    if (fonts) BfFontPrepare(game, out_dir.parent_path().parent_path());  // user\cache\extracted -> user
  }).detach();
}


// The level's water height (TekRantGaming, 9 Nov: grass grew under the shallows and the deep sea). When the game opens a
// level's terrain archive (GAME:\dist\xenon\levels\<level>\terrain-00.fbrb), its water mesh's height is read from the
// player's files and given to the grass (native_grass_water_level), which grows nothing below it. Every level has one
// flat water mesh (its vertices' y: Guadalcanal 173, Wake Island 215, Iwo Jima 5.3, Coral Sea 61.5).
std::atomic<float> g_sea_level{std::numeric_limits<float>::quiet_NaN()};

void NightLampsFromLevel(const uint8_t* data, size_t size, const std::string& folder);  // night_lamps.cpp (D163)

namespace {
std::mutex g_level_mutex;
fs::path g_level_game_root;
std::string g_level_folder;

bool ReadWaterLevel(const Archive& a, float& out) {
  for (const auto& [name, at] : a.entries) {
    if (name.size() < 15 || name.compare(name.size() - 15, 15, ".Water.Mesh.res") != 0) continue;
    // Version, vertex count, then each vertex's x, y, z (big endian floats).
    if (size_t(at.first) + at.second > a.data.size() || at.second < 20) continue;
    const uint8_t* p = &a.data[at.first];
    if (Be32(p + 4) == 0) continue;
    const uint32_t bits = Be32(p + 12);
    std::memcpy(&out, &bits, 4);
    return std::isfinite(out);
  }
  return false;
}

// The level's grass map (TekRantGaming, 9 Nov 2026: the grass flickered at the edges of the ground's grass, bits of it moved
// about, and it faded in as he turned: it was decided from the screen; NativeXRenderer D151 places it from this). The
// level's ground material map (its TerrainMaterialMap in terrain-00.fbrb; our reading): u32 BE side (4096), u16 BE
// chunks (128), then each chunk: u32 BE byte count, those bytes, 256 u16 BE row byte counts; a row is 256 bytes, two
// cells a byte (the low four bits first), as a byte and, when the next byte repeats it, a third byte: the run's length
// less one. The chunks are tiles of 512 cells by 256 rows, side / 512 across; columns along world x, rows along z, from
// the corner of the level's heightfield (its .Heightfield-000.res: min x, y, z and max x, y, z, big endian floats).
// Then the material names (u32 BE size, zero ended): the cells named "Grass" are the grass (Guadalcanal, Wake Island,
// Iwo Jima; Coral Sea has none). Checked against camera tour spots on all four maps (coast spots on water, inland
// ones on land). Given to the renderer as each grass cell's distance to other ground (m x 16), over the box round the
// grass and a margin.
bool ReadGrassMap(const Archive& a, const std::string& folder) {
  const auto start = std::chrono::steady_clock::now();
  const uint8_t* p = nullptr;
  size_t size = 0;
  float box[6] = {};
  bool have_box = false;
  for (const auto& [name, at] : a.entries) {
    if (size_t(at.first) + at.second > a.data.size()) continue;
    const bool res = name.size() > 4 && name.compare(name.size() - 4, 4, ".res") == 0;
    if (res && name.find("MaterialMap") != std::string::npos) {
      p = &a.data[at.first], size = at.second;
    } else if (name.size() >= 20 && name.compare(name.size() - 20, 20, ".Heightfield-000.res") == 0 && at.second >= 24) {
      for (int i = 0; i < 6; ++i) {
        const uint32_t bits = Be32(&a.data[at.first + 4 * i]);
        std::memcpy(&box[i], &bits, 4);
      }
      have_box = true;
    }
  }
  if (!p || size < 6 || !have_box) return false;
  const uint32_t side = Be32(p), chunks = Be16(p + 4);
  if (side == 0 || side % 512 != 0 || side > 8192 || chunks == 0 || chunks % (side / 512) != 0 ||
      chunks / (side / 512) * 256 != side) {
    return false;
  }
  const uint32_t across = side / 512;
  std::vector<uint8_t> cells(size_t(side) * side);
  size_t at = 6;
  for (uint32_t k = 0; k < chunks; ++k) {
    if (at + 4 > size) return false;
    const uint32_t n = Be32(p + at);
    at += 4;
    if (at + size_t(n) + 512 > size) return false;
    const uint8_t* data = p + at;
    const uint8_t* lengths = p + at + n;
    at += size_t(n) + 512;
    size_t o = 0;
    for (uint32_t r = 0; r < 256; ++r) {
      const uint32_t length = Be16(lengths + 2 * r);
      if (o + length > n) return false;
      const uint8_t* b = data + o;
      uint8_t row[256];
      uint32_t w = 0;
      for (uint32_t i = 0; i < length && w < 256;) {
        const uint8_t v = b[i++];
        row[w++] = v;
        if (i < length && b[i] == v) {
          ++i;
          if (w < 256) row[w++] = v;
          if (i < length) {
            const uint32_t total = uint32_t(b[i++]) + 1;  // the run's length, its first two bytes included
            for (uint32_t e = 2; e < total && w < 256; ++e) row[w++] = v;
          }
        }
      }
      if (w != 256) return false;
      o += length;
      uint8_t* line = &cells[(size_t(k / across) * 256 + r) * side + size_t(k % across) * 512];
      for (uint32_t x = 0; x < 256; ++x) line[2 * x] = row[x] & 15, line[2 * x + 1] = row[x] >> 4;
    }
  }
  if (at + 4 > size) return false;
  const uint32_t names_size = Be32(p + at);
  at += 4;
  if (at + names_size > size) return false;
  int grass = -1;
  std::string names;
  {
    int index = 0;
    std::string name;
    for (uint32_t i = 0; i < names_size; ++i) {
      const char c = char(p[at + i]);
      if (c) {
        name += c;
        continue;
      }
      std::string lower = name;
      for (char& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
      if (lower == "grass") grass = index;
      names += (names.empty() ? "" : ", ") + name;
      name.clear();
      ++index;
    }
  }
  const float cell = (box[3] - box[0]) / float(side);
  if (!(cell > 0.0f) || std::abs((box[5] - box[2]) / float(side) - cell) > 1e-3f) return false;
  // The box round the grass, with a margin of other ground (so every row and column of it ends in other ground).
  uint32_t x0 = side, x1 = 0, z0 = side, z1 = 0;
  size_t count = 0;
  for (uint32_t z = 0; z < side && grass >= 0; ++z) {
    const uint8_t* line = &cells[size_t(z) * side];
    for (uint32_t x = 0; x < side; ++x) {
      if (line[x] != uint32_t(grass)) continue;
      ++count;
      x0 = std::min(x0, x), x1 = std::max(x1, x), z0 = std::min(z0, z), z1 = std::max(z1, z);
    }
  }
  if (count == 0) {
    const uint8_t none = 0;  // a map with no grass: nothing grows anywhere
    native_x_renderer::GrassMap(&none, 1, 1, box[0], box[2], cell * float(side));
    REXLOG_INFO("BF level {}: its ground map has no grass (materials: {}): no grass grows", folder, names);
    return true;
  }
  constexpr uint32_t kMargin = 4;
  const int64_t bx = int64_t(x0) - kMargin, bz = int64_t(z0) - kMargin;
  const uint32_t w = x1 - x0 + 1 + 2 * kMargin, h = z1 - z0 + 1 + 2 * kMargin;
  // Each grass cell's squared distance (cells) to the nearest cell of other ground, exactly (Felzenszwalb and
  // Huttenlocher's transform: columns, then rows).
  constexpr double kFar = 1e20;
  std::vector<double> d2(size_t(w) * h);
  for (uint32_t z = 0; z < h; ++z) {
    for (uint32_t x = 0; x < w; ++x) {
      const int64_t sx = bx + x, sz = bz + z;
      const bool g = sx >= 0 && sz >= 0 && sx < int64_t(side) && sz < int64_t(side) &&
                     cells[size_t(sz) * side + size_t(sx)] == uint32_t(grass);
      d2[size_t(z) * w + x] = g ? kFar : 0.0;
    }
  }
  const uint32_t longest = std::max(w, h);
  std::vector<double> f(longest), out(longest), zs(size_t(longest) + 1);
  std::vector<int> v(longest);
  auto transform = [&](uint32_t n) {
    int k = 0;
    v[0] = 0, zs[0] = -kFar, zs[1] = kFar;
    for (int q = 1; q < int(n); ++q) {
      double s = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
      while (s <= zs[k]) {
        --k;
        s = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
      }
      ++k;
      v[k] = q, zs[k] = s, zs[k + 1] = kFar;
    }
    k = 0;
    for (int q = 0; q < int(n); ++q) {
      while (zs[k + 1] < q) ++k;
      out[q] = double(q - v[k]) * (q - v[k]) + f[v[k]];
    }
  };
  for (uint32_t x = 0; x < w; ++x) {
    for (uint32_t z = 0; z < h; ++z) f[z] = d2[size_t(z) * w + x];
    transform(h);
    for (uint32_t z = 0; z < h; ++z) d2[size_t(z) * w + x] = out[z];
  }
  std::vector<uint8_t> distance(size_t(w) * h);
  for (uint32_t z = 0; z < h; ++z) {
    for (uint32_t x = 0; x < w; ++x) f[x] = d2[size_t(z) * w + x];
    transform(w);
    for (uint32_t x = 0; x < w; ++x) {
      if (out[x] <= 0.0) continue;  // other ground
      // From the cell's centre to the edge of the other ground (half a cell nearer than its centre), m x 16.
      const double m = std::sqrt(out[x]) * cell - 0.5 * cell;
      distance[size_t(z) * w + x] = uint8_t(std::clamp<long>(std::lround(m * 16.0), 1L, 255L));
    }
  }
  native_x_renderer::GrassMap(distance.data(), w, h, box[0] + float(bx) * cell, box[2] + float(bz) * cell, cell);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  REXLOG_INFO("BF level {}: its grass map, {} x {} cells of {:.2f} m ({:.0f} m2 of grass, material {} of: {}) in {} ms",
              folder, w, h, cell, double(count) * cell * cell, grass, names, ms.count());
  return true;
}

// A file the game opens (its path as the game names it).
void NoteGameFile(const char* path) {
  std::string s(path);
  for (char& c : s) c = c == '/' ? '\\' : char(std::tolower(static_cast<unsigned char>(c)));
  const size_t at = s.find("\\levels\\");
  if (at == std::string::npos || s.find("\\terrain-00.fbrb") == std::string::npos) return;
  const size_t start = at + 8, end = s.find('\\', start);
  if (end == std::string::npos) return;
  const std::string folder = s.substr(start, end - start);
  fs::path root;
  {
    std::lock_guard lock(g_level_mutex);
    if (folder == g_level_folder || g_level_game_root.empty()) return;
    g_level_folder = folder;
    root = g_level_game_root;
  }
  // D151: no grass until the level's grass map is read (not on a server: it draws nothing).
  const bool grass = !REXCVAR_GET(server);
  if (grass) native_x_renderer::GrassMap(nullptr, 0, 0, 0, 0, 0.0f);
  // D163: no lamps until the level's own are read (nor on a server).
  if (grass) native_x_renderer::NightLamps(nullptr, 0);
  std::thread([root, folder, grass] {
    // D163: the level's lamps for the night, from its main partition (levels/<map>.dbx in reduced-level-00.fbrb).
    if (grass && kNightOps) {  // night_ops.h
      const fs::path level_archive = root / "dist" / "xenon" / "levels" / folder / "reduced-level-00.fbrb";
      Archive l;
      bool found = false;
      if (LoadArchive(level_archive, l)) {
        const std::string want = "levels/" + folder + ".dbx";
        for (const auto& [name, where] : l.entries) {
          std::string lower = name;
          for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
          if (lower != want || size_t(where.first) + where.second > l.data.size()) continue;
          NightLampsFromLevel(l.data.data() + where.first, where.second, folder);
          found = true;
          break;
        }
      }
      if (!found) REXLOG_WARN("BF level {}: no {} in {}: no lamps at night", folder, "levels/" + folder + ".dbx",
                              level_archive.string());
    }
    float level = 0;
    const fs::path archive = root / "dist" / "xenon" / "levels" / folder / "terrain-00.fbrb";
    Archive a;
    const bool loaded = LoadArchive(archive, a);
    if (grass && !(loaded && ReadGrassMap(a, folder))) {
      native_x_renderer::GrassMap(nullptr, 0, 0, 0, 0, -1.0f);  // none known: the grass decides from the screen
      REXLOG_WARN("BF level {}: no grass map read from {}", folder, archive.string());
    }
    if (loaded && ReadWaterLevel(a, level)) {
      g_sea_level.store(level);
      rex::cvar::SetFlagByName("native_grass_water_level", std::to_string(level));
      REXLOG_INFO("BF level {}: its water at {:.2f} m (its water mesh): no grass below it", folder, level);
    } else {
      g_sea_level.store(std::numeric_limits<float>::quiet_NaN());
      rex::cvar::SetFlagByName("native_grass_water_level", "-100000");
      REXLOG_WARN("BF level {}: no water height read from {}", folder, archive.string());
    }
  }).detach();
}
}  // namespace

// The folder of the level the game has loaded (wake_island_s...), empty before the first.
std::string LoadedLevelFolder() {
  std::lock_guard lock(g_level_mutex);
  return g_level_folder;
}

// The HD texture pack's source (developer: BF_DEV_HD_EXPORT=<folder>, TekRantGaming, 9 Nov: an HD pack made here, imported
// in the launcher): every world texture in the game's archives (DXT1, DXT3 or DXT5, its first level at least 64 KB;
// not the menus, fonts, minimaps, loading pictures, intensity maps, particles or normal maps), each written as a PNG
// with a manifest line: its key (XXH3 of its first 64 KB, as NativeXRenderer hashes the texture in guest memory, with
// its guest format and size), its name, its .res format, its size. tools\hdpack\make_pack.py upscales them and writes
// the pack. Once a run, on a worker.
#if defined(BF_DEV_TOOLS)
namespace {
bool HdExportSkip(const std::string& lower) {
  static const char* kSkip[] = {"menu/", "/ui/", "output/", "font", "minimap", "load_image", "intensitymap",
                                "emitter", "effects/", "/particles", "_n.res", "_nm.res", "_nrm", "normal", "_bump",
                                "loading", "gametips", "atlas", "_s.res", "_spec", "shadow", "awards/",
                                "ambient_occlusion", "bakedtextures", "masks/", "materialmask", "meshparticles", "menu"};
  for (const char* s : kSkip)
    if (lower.find(s) != std::string::npos) return true;
  return false;
}
}  // namespace

void HdExport(const fs::path& game) {
  const char* v = std::getenv("BF_DEV_HD_EXPORT");
  if (!v || !*v) return;
  const fs::path out(v);
  std::thread([game, out] {
    std::error_code ec;
    fs::create_directories(out, ec);
    std::ofstream manifest(out / "manifest.tsv", std::ios::binary | std::ios::trunc);
    manifest << "key\tguest_format\twidth\theight\tres_format\tpng\tname\n";
    std::unordered_set<uint64_t> done;
    size_t written = 0, skipped = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (auto it = fs::recursive_directory_iterator(game / "dist" / "xenon", ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      if (ec) break;
      const fs::path p = it->path();
      if (p.extension() != ".fbrb") continue;
      std::string ps = p.generic_string();
      for (char& c : ps) c = char(std::tolower(static_cast<unsigned char>(c)));
      if (ps.find("/vo/") != std::string::npos) continue;
      Archive a;
      if (!LoadArchive(p, a)) continue;
      for (const auto& [name, at] : a.entries) {
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".res") != 0) continue;
        std::string lower = name;
        for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (HdExportSkip(lower)) continue;
        if (size_t(at.first) + at.second > a.data.size() || at.second < 0x5C + 65536) continue;
        const uint8_t* res = &a.data[at.first];
        const size_t hdr = std::memcmp(res, "RES", 3) == 0 ? 0x40 : 0;
        const uint8_t* hd = res + hdr;
        // A XenonTexture: its format (0 DXT1, 1 DXT3, 2 DXT5), width, height; the data after 0x5C bytes.
        const uint32_t fmt = Be32(hd + 8), w = Be32(hd + 16), h = Be32(hd + 20);
        if (fmt > 2 || w < 64 || h < 64 || w > 4096 || h > 4096) continue;
        const uint64_t base_size = uint64_t(w) * h / (fmt == 0 ? 2 : 1);
        if (base_size < 65536 || at.second < hdr + 0x5C + 65536) {
          ++skipped;
          continue;
        }
        const uint32_t guest_format = fmt == 0 ? 18 : fmt == 1 ? 19 : 20;  // Xenos k_DXT1, k_DXT2_3, k_DXT4_5
        const uint64_t key = XXH3_64bits(hd + 0x5C, 65536) ^ (uint64_t(guest_format) << 56) ^ (uint64_t(w) << 32) ^
                             (uint64_t(h) << 16);
        if (!done.insert(key).second) continue;
        std::vector<uint8_t> bytes(res, res + at.second), rgba;
        int dw = 0, dh = 0;
        if (!DecodeXenonTexture(bytes, rgba, dw, dh)) continue;
        size_t png_size = 0;
        void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba.data(), dw, dh, 4, &png_size, 6, false);
        if (!png) continue;
        char file[32];
        std::snprintf(file, sizeof(file), "%016llx.png", static_cast<unsigned long long>(key));
        {
          std::ofstream f(out / file, std::ios::binary | std::ios::trunc);
          f.write(static_cast<const char*>(png), std::streamsize(png_size));
        }
        mz_free(png);
        manifest << std::string(file, 16) << '\t' << guest_format << '\t' << w << '\t' << h << '\t' << fmt << '\t' << file << '\t' << name
                 << '\n';
        ++written;
      }
    }
    manifest.flush();
    REXLOG_INFO("BF dev HD export: {} textures written to {} ({} below 64 KB skipped) in {} s", written, out.string(),
                skipped,
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count());
  }).detach();
}
#else
void HdExport(const fs::path&) {}
#endif

// The menu and HUD pictures for an HD pack (BF_HD_EXPORT_UI=<folder>; 13 Nov 2026: "upscale the lower quality imagery
// (such as character art and spawn menu)"): every XenonTexture under menu/, awards/ and the levels' ui/ in the game's
// archives, DXT1, DXT3, DXT5 or 8888, its first level at least 4 KB, each written as a PNG with a manifest line as
// HdExport's (tools\hdpack\make_pack.py --ui makes the pack). The key is NativeXRenderer's: XXH3 of the first level's
// first 64 KB (all of it when smaller), xor the guest format, width and height.
// Not the four atlases the game's prompts and flag letters are written into at their own size (hud_glyphs.cpp: an
// HD copy would show the pictures without them), not fonts, not the minimaps.
void HdExportUi(const fs::path& game) {
  const char* v = std::getenv("BF_HD_EXPORT_UI");
  if (!v || !*v) return;
  const fs::path out(v);
  std::thread([game, out] {
    std::error_code ec;
    fs::create_directories(out, ec);
    std::ofstream manifest(out / "manifest.tsv", std::ios::binary | std::ios::trunc);
    manifest << "key\tguest_format\twidth\theight\tres_format\tpng\tname\n";
    std::unordered_set<uint64_t> done;
    size_t written = 0;
    for (auto it = fs::recursive_directory_iterator(game / "dist" / "xenon", ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      if (ec) break;
      const fs::path p = it->path();
      if (p.extension() != ".fbrb") continue;
      std::string ps = p.generic_string();
      for (char& c : ps) c = char(std::tolower(static_cast<unsigned char>(c)));
      if (ps.find("/vo/") != std::string::npos) continue;
      Archive a;
      if (!LoadArchive(p, a)) continue;
      for (const auto& [name, at] : a.entries) {
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".res") != 0) continue;
        std::string lower = name;
        for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (lower.find("menu/") == std::string::npos && lower.find("awards/") == std::string::npos &&
            lower.find("/ui/") == std::string::npos && lower.find("hud") == std::string::npos)
          continue;
        bool skip = false;
        // (13 Nov, later: the atlases the prompts and flag letters are drawn into are in too; NativeXRenderer has the
        // game draw them into the pack's picture at its size.)
        for (const char* k : {"font", "dummy"}) skip = skip || lower.find(k) != std::string::npos;
        if (skip || size_t(at.first) + at.second > a.data.size() || at.second < 0x5C + 4096) continue;
        const uint8_t* res = &a.data[at.first];
        const size_t hdr = std::memcmp(res, "RES", 3) == 0 ? 0x40 : 0;
        const uint8_t* hd = res + hdr;
        const uint32_t fmt = Be32(hd + 8), w = Be32(hd + 16), h = Be32(hd + 20);
        if ((fmt > 2 && fmt != 9) || w < 16 || h < 16 || w > 4096 || h > 4096) continue;
        const uint64_t base_size = fmt == 9 ? uint64_t(w) * h * 4 : uint64_t(w) * h / (fmt == 0 ? 2 : 1);
        const size_t hashed = size_t(std::min<uint64_t>(base_size, 65536));
        if (base_size < 4096 || at.second < hdr + 0x5C + hashed) continue;
        const uint32_t guest_format = fmt == 9 ? 6 : fmt == 0 ? 18 : fmt == 1 ? 19 : 20;  // Xenos k_8_8_8_8, DXT1, 3, 5
        const uint64_t key = XXH3_64bits(hd + 0x5C, hashed) ^ (uint64_t(guest_format) << 56) ^ (uint64_t(w) << 32) ^
                             (uint64_t(h) << 16);
        if (!done.insert(key).second) continue;
        std::vector<uint8_t> bytes(res, res + at.second), rgba;
        int dw = 0, dh = 0;
        if (!DecodeXenonTexture(bytes, rgba, dw, dh)) continue;
        size_t png_size = 0;
        void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba.data(), dw, dh, 4, &png_size, 6, false);
        if (!png) continue;
        char file[32];
        std::snprintf(file, sizeof(file), "%016llx.png", static_cast<unsigned long long>(key));
        {
          std::ofstream f(out / file, std::ios::binary | std::ios::trunc);
          f.write(static_cast<const char*>(png), std::streamsize(png_size));
        }
        mz_free(png);
        manifest << std::string(file, 16) << '\t' << guest_format << '\t' << w << '\t' << h << '\t' << fmt << '\t' << file
                 << '\t' << name << '\n';
        ++written;
      }
    }
    manifest.flush();
    REXLOG_INFO("BF HD export: {} menu and HUD pictures written to {}", written, out.string());
  }).detach();
}

void SetLevelGameRoot(const fs::path& game) {
  std::lock_guard lock(g_level_mutex);
  g_level_game_root = game;
}

}  // namespace bf1943

#if defined(BF_DEV_TOOLS)
namespace bf1943 {
void DevNoteOpenedHandle(const uint8_t* base, uint32_t handle_out, uint32_t attributes, uint32_t status, uint32_t lr,
                         uint32_t sp);
}
#endif
// Every file the game opens: the kernel's NtCreateFile and NtOpenFile (r5 their object attributes: root directory,
// name (an ANSI string: length, size, buffer), attributes; big endian), then the runtime's own.
namespace {
PPCFunc* RuntimeKernelExport(const char* name) {
#if defined(_WIN32)
  static HMODULE runtime = GetModuleHandleA("rexruntime.dll");
  return reinterpret_cast<PPCFunc*>(GetProcAddress(runtime, name));
#else
  return reinterpret_cast<PPCFunc*>(dlsym(RTLD_NEXT, name));  // the runtime's own, after ours (input_settings.cpp)
#endif
}

void NoteOpenedFile(const uint8_t* base, uint32_t attributes) {
  if (attributes < 0x10000 || attributes >= 0xF0000000u) return;
  const uint32_t name = bf1943::Be32(base + attributes + 4);
  if (name < 0x10000 || name >= 0xF0000000u) return;
  const uint32_t length = uint32_t(base[name]) << 8 | base[name + 1], buffer = bf1943::Be32(base + name + 4);
  if (!length || length > 400 || buffer < 0x10000 || buffer >= 0xF0000000u) return;
  const std::string path(reinterpret_cast<const char*>(base + buffer), length);
#if defined(BF_DEV_TOOLS)
  static const bool files = std::getenv("BF_DEV_FILES") != nullptr;
  static std::atomic<int> logged{0};
  if (files && logged.fetch_add(1) < 80) REXLOG_INFO("BF dev file: {}", path);
#endif
  bf1943::NoteGameFile(path.c_str());
}
}  // namespace

extern "C" REX_FUNC(__imp__NtCreateFile) {
  static PPCFunc* original = RuntimeKernelExport("__imp__NtCreateFile");
  NoteOpenedFile(base, ctx.r5.u32);
#if defined(BF_DEV_TOOLS)
  const uint32_t handle_out = ctx.r3.u32, attributes = ctx.r5.u32, lr = uint32_t(ctx.lr), sp = ctx.r1.u32;
#endif
  original(ctx, base);
#if defined(BF_DEV_TOOLS)
  bf1943::DevNoteOpenedHandle(base, handle_out, attributes, ctx.r3.u32, lr, sp);  // dev_io_stats.cpp (BF_DEV_IOSTATS)
#endif
}
extern "C" REX_FUNC(__imp__NtOpenFile) {
  static PPCFunc* original = RuntimeKernelExport("__imp__NtOpenFile");
  NoteOpenedFile(base, ctx.r5.u32);
#if defined(BF_DEV_TOOLS)
  const uint32_t handle_out = ctx.r3.u32, attributes = ctx.r5.u32, lr = uint32_t(ctx.lr), sp = ctx.r1.u32;
#endif
  original(ctx, base);
#if defined(BF_DEV_TOOLS)
  bf1943::DevNoteOpenedHandle(base, handle_out, attributes, ctx.r3.u32, lr, sp);  // dev_io_stats.cpp (BF_DEV_IOSTATS)
#endif
}
