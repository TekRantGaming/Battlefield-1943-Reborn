// NativeXRenderer - a developer's export of the first person weapons' models (weapon_export.h).

#include "weapon_export.h"

#include <native_x_renderer/native_x_renderer.h>

#if !defined(NXR_INTERNAL_TOOLS)

namespace native_x_renderer {
bool WeaponExportOn() { return false; }
void WeaponExportName(const char*) {}
void WeaponExportDrawNow(const WeaponExportDraw&) {}
void WeaponExportFlush(Diligent::IRenderDevice*, Diligent::IDeviceContext*) {}
void NoteHeldWeapon(const char*) {}
}  // namespace native_x_renderer

#else

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "DeviceContext.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Texture.h"
#include "TextureView.h"

#include <rex/logging.h>

using namespace Diligent;

namespace native_x_renderer {
namespace {

std::filesystem::path Root() {
  static const std::filesystem::path root = [] {
    const char* v = std::getenv("NXR_WEAPON_EXPORT");
    return std::filesystem::path(v && *v ? v : "");
  }();
  return root;
}

struct Vec3 {
  float x = 0, y = 0, z = 0;
};
// A bone's matrix: three rows of four (the fourth column its place), as the game's vertex shaders take them.
struct Bone {
  float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  Vec3 Point(const Vec3& p) const {
    return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
            m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
  }
  Vec3 Way(const Vec3& p) const {
    return {m[0] * p.x + m[1] * p.y + m[2] * p.z, m[4] * p.x + m[5] * p.y + m[6] * p.z, m[8] * p.x + m[9] * p.y + m[10] * p.z};
  }
  // Its inverse (the three by three part's, and the place carried back through it).
  Bone Inverse() const {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    const float s = std::abs(det) > 1e-12f ? 1.0f / det : 0.0f;
    Bone r;
    r.m[0] = (e * i - f * h) * s, r.m[1] = (c * h - b * i) * s, r.m[2] = (b * f - c * e) * s;
    r.m[4] = (f * g - d * i) * s, r.m[5] = (a * i - c * g) * s, r.m[6] = (c * d - a * f) * s;
    r.m[8] = (d * h - e * g) * s, r.m[9] = (b * g - a * h) * s, r.m[10] = (a * e - b * d) * s;
    const Vec3 t = r.Way({m[3], m[7], m[11]});
    r.m[3] = -t.x, r.m[7] = -t.y, r.m[11] = -t.z;
    return r;
  }
  Bone Then(const Bone& first) const {  // this after `first`
    Bone r;
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 4; ++col)
        r.m[row * 4 + col] = m[row * 4] * first.m[col] + m[row * 4 + 1] * first.m[4 + col] + m[row * 4 + 2] * first.m[8 + col] +
                             (col == 3 ? m[row * 4 + 3] : 0.0f);
    return r;
  }
};

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
float BeFloat(const uint8_t* p) {
  const uint32_t v = Be32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
float Half(uint16_t h) {
  const int exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
  float v;
  if (exponent == 0) v = std::ldexp(float(mantissa), -24);
  else if (exponent == 31) v = mantissa ? 0.0f : 65504.0f;
  else v = std::ldexp(float(mantissa + 1024), exponent - 25);
  return (h & 0x8000) ? -v : v;
}
float Signed(uint32_t bits, int count) {
  const int32_t sign = 1 << (count - 1);
  const int32_t v = (int32_t(bits) ^ sign) - sign;
  return std::max(float(v) / float(sign - 1), -1.0f);
}

struct Box {
  Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
  uint32_t count = 0;
  void Add(const Vec3& p) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    ++count;
  }
};

struct PendingTexture {
  RefCntAutoPtr<ITextureView> view;
  std::filesystem::path file;
};

struct Weapon {
  std::set<std::array<uint32_t, 3>> draws;  // vertex address, index address, index count
  std::vector<std::string> obj;             // the mesh's lines so far
  std::vector<std::string> info;
  uint32_t vertices = 0;                    // written so far (the next draw's first index)
  int main_bone = -1;
  float bone_step = 3.0f;
  std::map<int, Box> parts;                 // each bone's vertices, in the main part's space
  std::map<int, Bone> bones;                // each bone against the main part, as held
  Box whole;
  std::set<ITexture*> textures;
  // The names the game gave the held weapon while this mesh was drawn, and for how many draws: the most seen is its
  // own (in the moment of a change of weapon the name is already the next one's while the old mesh is still drawn:
  // by the name alone the Thompson's mesh was written as the wrench's too, runs export_us1 to jp3).
  std::map<std::string, uint32_t> names;
  uint32_t seen = 0;
};

struct State {
  std::mutex mutex;
  std::string name;
  std::map<std::string, Weapon> weapons;
  std::vector<PendingTexture> pending;
};
State& S() {
  static State s;
  return s;
}

std::string Line(const char* format, ...) {
  char text[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  return text;
}

void WriteWeapon(const std::string& name, const Weapon& w) {
  const std::filesystem::path dir = Root() / name;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  {
    std::ofstream out(dir / "weapon.obj", std::ios::trunc);
    out << "# " << name << ": the first person weapon as held, in its main part's space (metres). NativeXRenderer's export;\n"
        << "# the game's own model: a reference for skins of our own, not to be passed on.\n";
    for (const std::string& line : w.obj) out << line << '\n';
  }
  {
    std::ofstream out(dir / "info.txt", std::ios::trunc);
    out << name << "\n\nThe whole gun, in its main part's space (bone " << w.main_bone << "), metres:\n";
    out << Line("  box %.4f %.4f %.4f to %.4f %.4f %.4f (%u vertices)\n", w.whole.lo.x, w.whole.lo.y, w.whole.lo.z, w.whole.hi.x,
                w.whole.hi.y, w.whole.hi.z, w.whole.count);
    const float size[3] = {w.whole.hi.x - w.whole.lo.x, w.whole.hi.y - w.whole.lo.y, w.whole.hi.z - w.whole.lo.z};
    const int longest = size[0] >= size[1] && size[0] >= size[2] ? 0 : size[1] >= size[2] ? 1 : 2;
    out << Line("  size %.4f %.4f %.4f; its long axis is %c (%.3f m)\n\nIts parts, by bone:\n", size[0], size[1], size[2], "xyz"[longest],
                size[longest]);
    for (const auto& [bone, box] : w.parts)
      out << Line("  bone %3d: %5u vertices, box %.4f %.4f %.4f to %.4f %.4f %.4f\n", bone, box.count, box.lo.x, box.lo.y, box.lo.z,
                  box.hi.x, box.hi.y, box.hi.z);
    out << "\nIts draws:\n";
    for (const std::string& line : w.info) out << line << '\n';
  }
  {
    std::ofstream out(dir / "bones.txt", std::ios::trunc);
    out << "# Each bone against the main part (bone " << w.main_bone << "), as held: three rows of four, the fourth column its place (m).\n"
        << "# In the game's vertex shaders a bone's rows are the constants c[17 + " << w.bone_step << " * bone] and the two after.\n";
    for (const auto& [bone, m] : w.bones)
      out << Line("bone %d\n  %.5f %.5f %.5f %.5f\n  %.5f %.5f %.5f %.5f\n  %.5f %.5f %.5f %.5f\n", bone, m.m[0], m.m[1], m.m[2], m.m[3],
                  m.m[4], m.m[5], m.m[6], m.m[7], m.m[8], m.m[9], m.m[10], m.m[11]);
  }
}

// A DDS file's header for a host texture's first level; false for a format it has none for.
bool DdsHeader(TEXTURE_FORMAT format, uint32_t width, uint32_t height, std::vector<uint8_t>& out, uint32_t& block, uint32_t& bytes_per) {
  const char* four = nullptr;
  block = 4, bytes_per = 0;
  switch (format) {
    case TEX_FORMAT_BC1_UNORM:
    case TEX_FORMAT_BC1_UNORM_SRGB: four = "DXT1", bytes_per = 8; break;
    case TEX_FORMAT_BC2_UNORM:
    case TEX_FORMAT_BC2_UNORM_SRGB: four = "DXT3", bytes_per = 16; break;
    case TEX_FORMAT_BC3_UNORM:
    case TEX_FORMAT_BC3_UNORM_SRGB: four = "DXT5", bytes_per = 16; break;
    case TEX_FORMAT_BC4_UNORM: four = "ATI1", bytes_per = 8; break;
    case TEX_FORMAT_BC5_UNORM: four = "ATI2", bytes_per = 16; break;
    case TEX_FORMAT_RGBA8_UNORM:
    case TEX_FORMAT_RGBA8_UNORM_SRGB:
    case TEX_FORMAT_BGRA8_UNORM: block = 1, bytes_per = 4; break;
    default: return false;
  }
  out.assign(128, 0);
  auto put = [&](size_t at, uint32_t v) { std::memcpy(out.data() + at, &v, 4); };
  std::memcpy(out.data(), "DDS ", 4);
  put(4, 124), put(8, 0x1007 | (four ? 0x80000 : 0x8)), put(12, height), put(16, width);
  put(20, four ? std::max(1u, (width + 3) / 4) * std::max(1u, (height + 3) / 4) * bytes_per : width * 4);
  put(28, 1), put(76, 32);
  if (four) {
    put(80, 0x4);
    std::memcpy(out.data() + 84, four, 4);
  } else {
    const bool bgra = format == TEX_FORMAT_BGRA8_UNORM;
    put(80, 0x41), put(88, 32), put(92, bgra ? 0x00FF0000u : 0x000000FFu), put(96, 0x0000FF00u), put(100, bgra ? 0x000000FFu : 0x00FF0000u),
        put(104, 0xFF000000u);
  }
  put(108, 0x1000);
  return true;
}

}  // namespace

bool WeaponExportOn() { return !Root().empty(); }

void WeaponExportName(const char* name) {
  if (!WeaponExportOn()) return;
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.name = name ? name : "";
  for (char& c : s.name)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) c = '_';
}
void NoteHeldWeapon(const char* name) { WeaponExportName(name); }

void WeaponExportDrawNow(const WeaponExportDraw& d) {
  if (!WeaponExportOn() || !d.vertices || !d.vs_constants || d.stride_bytes < 12 || d.position < 0) return;
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.name.empty()) return;
  // A mesh is told by its vertex buffer's size and its triangle count (the same in every run); its folder is named
  // by them, with names.txt beside it (tools rename it after its most seen name).
  const std::string mesh = Line("mesh_%uv_%ui", d.vertex_bytes / d.stride_bytes, d.index_count);
  Weapon& w = s.weapons[mesh];
  ++w.names[s.name];
  if (++w.seen % 240 == 1) {
    std::error_code ec;
    std::filesystem::create_directories(Root() / mesh, ec);
    std::ofstream out(Root() / mesh / "names.txt", std::ios::trunc);
    for (const auto& [name, count] : w.names) out << name << ' ' << count << '\n';
  }
  if (!w.draws.insert({d.vertex_address, d.index_address, d.index_count}).second) return;
  const uint32_t vertex_count = d.vertex_bytes / d.stride_bytes;
  // The indices (as a triangle list), and which vertices they use.
  std::vector<uint32_t> indices;
  if (d.indices) {
    indices.resize(d.index_count);
    for (uint32_t i = 0; i < d.index_count; ++i)
      indices[i] = d.index_32 ? Be32(d.indices + size_t(i) * 4) : uint32_t(d.indices[size_t(i) * 2]) << 8 | d.indices[size_t(i) * 2 + 1];
  } else {
    indices.resize(d.index_count);
    for (uint32_t i = 0; i < d.index_count; ++i) indices[i] = i;
  }
  std::vector<uint32_t> triangles;
  if (d.primitive == 4) {
    triangles = indices;
    triangles.resize(triangles.size() / 3 * 3);
  } else if (d.primitive == 6) {
    for (size_t i = 0; i + 2 < indices.size(); ++i) {
      if (indices[i] == indices[i + 1] || indices[i + 1] == indices[i + 2] || indices[i] == indices[i + 2]) continue;
      if (i & 1) triangles.insert(triangles.end(), {indices[i + 1], indices[i], indices[i + 2]});
      else triangles.insert(triangles.end(), {indices[i], indices[i + 1], indices[i + 2]});
    }
  }
  uint32_t lowest = UINT32_MAX, highest = 0;
  for (uint32_t i : triangles) lowest = std::min(lowest, i), highest = std::max(highest, i);
  if (triangles.empty() || highest >= vertex_count) {
    w.info.push_back(Line("  (a draw left out: VS %016llX, %u indices of primitive type %u, %u vertices in its buffer, highest index %u)",
                          static_cast<unsigned long long>(d.vs_hash), d.index_count, d.primitive, vertex_count, highest));
    WriteWeapon(mesh, w);
    return;
  }
  // The bones: c[17 + step * bone] and the two after (the step is c255's x).
  const float step = d.vs_constants[255 * 4];
  w.bone_step = step;
  auto bone_at = [&](int bone) {
    Bone b;
    const int first = 17 + int(std::lround(step * float(bone)));
    if (first >= 0 && first + 2 < 255) std::memcpy(b.m, d.vs_constants + size_t(first) * 4, sizeof(b.m));
    return b;
  };
  auto bone_of = [&](uint32_t v) { return d.bone >= 0 ? int(d.vertices[size_t(v) * d.stride_bytes + d.bone + 3]) : 0; };
  if (w.main_bone < 0) {  // the bone most of this first draw's vertices are on: the gun's body
    std::map<int, uint32_t> count;
    for (uint32_t v = lowest; v <= highest; ++v) ++count[bone_of(v)];
    uint32_t most = 0;
    for (const auto& [bone, n] : count)
      if (n > most) most = n, w.main_bone = bone;
  }
  const Bone to_main = bone_at(w.main_bone).Inverse();
  std::map<int, uint32_t> used;
  w.obj.push_back(Line("o draw_%zu_vs_%016llX", w.draws.size(), static_cast<unsigned long long>(d.vs_hash)));
  for (uint32_t v = lowest; v <= highest; ++v) {
    const uint8_t* p = d.vertices + size_t(v) * d.stride_bytes;
    const int bone = bone_of(v);
    ++used[bone];
    const Bone m = to_main.Then(bone_at(bone));
    w.bones[bone] = m;
    const Vec3 at = m.Point({BeFloat(p + d.position), BeFloat(p + d.position + 4), BeFloat(p + d.position + 8)});
    w.parts[bone].Add(at);
    w.whole.Add(at);
    w.obj.push_back(Line("v %.6f %.6f %.6f", at.x, at.y, at.z));
    if (d.uv >= 0) {
      // (Two half floats: the first the texture's u, the second its v; an OBJ's v runs up.)
      const float u = Half(uint16_t(p[d.uv] << 8 | p[d.uv + 1])), tv = Half(uint16_t(p[d.uv + 2] << 8 | p[d.uv + 3]));
      w.obj.push_back(Line("vt %.6f %.6f", u, 1.0f - tv));
    }
    if (d.normal >= 0) {
      const uint32_t packed = Be32(p + d.normal);
      Vec3 n = m.Way({Signed(packed & 0x7FF, 11), Signed((packed >> 11) & 0x7FF, 11), Signed(packed >> 22, 10)});
      const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
      if (len > 1e-6f) n = {n.x / len, n.y / len, n.z / len};
      w.obj.push_back(Line("vn %.5f %.5f %.5f", n.x, n.y, n.z));
    }
  }
  // Its faces, a group for each bone (a triangle goes with its first vertex's).
  int group = -1;
  std::vector<std::array<uint32_t, 3>> faces;
  for (size_t i = 0; i + 2 < triangles.size(); i += 3) faces.push_back({triangles[i], triangles[i + 1], triangles[i + 2]});
  std::stable_sort(faces.begin(), faces.end(), [&](const auto& a, const auto& b) { return bone_of(a[0]) < bone_of(b[0]); });
  for (const auto& f : faces) {
    const int bone = bone_of(f[0]);
    if (bone != group) {
      group = bone;
      w.obj.push_back(Line("g draw_%zu_bone_%d", w.draws.size(), bone));
    }
    const uint32_t a = f[0] - lowest + w.vertices + 1, b = f[1] - lowest + w.vertices + 1, c = f[2] - lowest + w.vertices + 1;
    if (d.uv >= 0 && d.normal >= 0) w.obj.push_back(Line("f %u/%u/%u %u/%u/%u %u/%u/%u", a, a, a, b, b, b, c, c, c));
    else if (d.uv >= 0) w.obj.push_back(Line("f %u/%u %u/%u %u/%u", a, a, b, b, c, c));
    else w.obj.push_back(Line("f %u %u %u", a, b, c));
  }
  w.vertices += highest - lowest + 1;
  std::string bones;
  for (const auto& [bone, n] : used) bones += Line(" %d (%u)", bone, n);
  w.info.push_back(Line("  draw %zu: VS %016llX PS %016llX, %u vertices of %u bytes, %zu triangles; bones:%s", w.draws.size(),
                        static_cast<unsigned long long>(d.vs_hash), static_cast<unsigned long long>(d.ps_hash), highest - lowest + 1,
                        d.stride_bytes, faces.size(), bones.c_str()));
  for (uint32_t t = 0; t < d.texture_count; ++t) {
    const WeaponExportDraw::Texture& tex = d.textures[t];
    w.info.push_back(Line("    texture: fetch %u, %ux%u, the guest's format %u", tex.fetch, tex.width, tex.height, tex.format));
    if (!tex.view || !w.textures.insert(tex.view->GetTexture()).second || s.pending.size() >= 64) continue;
    s.pending.push_back({RefCntAutoPtr<ITextureView>(tex.view),
                         Root() / mesh / Line("tex_fetch%u_%ux%u_format%u.dds", tex.fetch, tex.width, tex.height, tex.format)});
  }
  WriteWeapon(mesh, w);
  REXLOG_INFO("[native_x_renderer] weapon export: {} (held: {}) draw {} ({} vertices, {} triangles) into {}", mesh, s.name,
              w.draws.size(), highest - lowest + 1, faces.size(), (Root() / mesh).string());
}

void WeaponExportFlush(IRenderDevice* device, IDeviceContext* context) {
  if (!WeaponExportOn() || !device || !context) return;
  std::vector<PendingTexture> pending;
  {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    pending.swap(s.pending);
  }
  for (const PendingTexture& p : pending) {
    ITexture* texture = p.view->GetTexture();
    const TextureDesc& desc = texture->GetDesc();
    std::vector<uint8_t> file;
    uint32_t block = 4, bytes_per = 0;
    if (!DdsHeader(desc.Format, desc.Width, desc.Height, file, block, bytes_per)) {
      REXLOG_INFO("[native_x_renderer] weapon export: {} is of the host's format {}: not written", p.file.filename().string(),
                  int(desc.Format));
      continue;
    }
    TextureDesc staging = desc;
    staging.Name = "NativeXRenderer weapon export";
    staging.Usage = USAGE_STAGING;
    staging.BindFlags = BIND_NONE;
    staging.CPUAccessFlags = CPU_ACCESS_READ;
    staging.MiscFlags = MISC_TEXTURE_FLAG_NONE;
    staging.MipLevels = 1;
    staging.Type = RESOURCE_DIM_TEX_2D;
    staging.ArraySize = 1;
    RefCntAutoPtr<ITexture> copy_to;
    device->CreateTexture(staging, nullptr, &copy_to);
    if (!copy_to) {
      REXLOG_INFO("[native_x_renderer] weapon export: no staging texture for {}", p.file.filename().string());
      continue;
    }
    CopyTextureAttribs copy(texture, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, copy_to, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->CopyTexture(copy);
    context->WaitForIdle();
    MappedTextureSubresource mapped;
    context->MapTextureSubresource(copy_to, 0, 0, MAP_READ, MAP_FLAG_NONE, nullptr, mapped);
    if (mapped.pData) {
      const uint32_t rows = block == 4 ? std::max(1u, (desc.Height + 3) / 4) : desc.Height;
      const size_t row_bytes = block == 4 ? size_t(std::max(1u, (desc.Width + 3) / 4)) * bytes_per : size_t(desc.Width) * 4;
      for (uint32_t y = 0; y < rows; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.Stride;
        file.insert(file.end(), row, row + row_bytes);
      }
      std::ofstream out(p.file, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
      REXLOG_INFO("[native_x_renderer] weapon export: {} ({}x{}, the host's format {})", p.file.string(), desc.Width, desc.Height,
                  int(desc.Format));
    }
    context->UnmapTextureSubresource(copy_to, 0, 0);
  }
}

}  // namespace native_x_renderer

#endif
