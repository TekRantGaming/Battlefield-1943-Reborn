// Battlefield 1943 Reborn's Night Ops (NativeXRenderer D163; TekRantGaming, 12 Nov 2026: "Around the map especially
// on Wake Island ... There is light sources such as the telegraph poles. They have what look like spotlights on them.
// If we could get those lights working, it would add quite a bit of atmosphere"): the level's lamps, read from the
// player's own game files when the level loads and given to the renderer, which lights them only at night.
//
// The level's main partition (levels/<map>.dbx in its reduced-level-00.fbrb) places each object as an
// Entity.ReferenceObjectData: a Transform (a LinearTransform: the right, up and forward axes and the position, each
// row three big endian floats and a pad) and its ReferencedObject (the blueprint). Wake Island has 92 lampposts
// (objects/beach_objects/lamppost_01) and one bomb lamp. The lamppost's model has a part of its own for its lamps,
// Level04_light: two small glasses facing down and a little forward, either side of the pole at 6.35 m (its
// vertices, half floats, in Lamppost_01_Mesh_lod0_data.res); the bomb lamp's lamp_Bulb, 0.16 to 0.37 m up.
//
// The binary dbx (Frostbite 1's binary XML, worked out from the level's files): "{binary}", then big endian words: the
// node data's file offset, 0, that offset minus 24, the string count, 0, then the string offsets (one fewer than the
// count, from byte 28) and the strings. Each node: its tag (a varint, a 1-based string index; 0 ends a list of
// children), a flags byte (the low nibble the attribute count; bits 4 to 6 the value after the attributes: 2 a text, a
// varint string index; 6 bytes, a varint count then the bytes; 7 an array, a varint count, a byte size, the data; 0x80:
// children follow, ended by a 0 tag), then the attributes (name and value, varint string indices).

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <native_x_renderer/native_x_renderer.h>
#include <rex/logging.h>

namespace bf1943 {
namespace {

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

float BeFloat(const uint8_t* p) {
  const uint32_t u = Be32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

struct Placed {
  std::string blueprint;
  float m[16];  // right, up, forward, position: each x y z and a pad
};

// The binary dbx's placed objects: each instance's Transform and ReferencedObject.
class Reader {
 public:
  Reader(const uint8_t* d, size_t n) : d_(d), n_(n) {}

  bool Read(std::vector<Placed>& out) {
    if (n_ < 32 || std::memcmp(d_, "{binary}", 8) != 0) return false;
    const uint32_t start = Be32(d_ + 8), count = Be32(d_ + 20);
    if (count < 2 || 28 + size_t(count - 1) * 4 > n_ || start > n_) return false;
    const size_t base = 28 + size_t(count - 1) * 4;
    strings_.reserve(count - 1);
    for (uint32_t i = 0; i + 1 < count; ++i) {
      const size_t at = base + Be32(d_ + 28 + i * 4);
      if (at >= n_) return false;
      const void* end = std::memchr(d_ + at, 0, n_ - at);
      if (!end) return false;
      strings_.emplace_back(reinterpret_cast<const char*>(d_ + at), static_cast<const uint8_t*>(end) - (d_ + at));
    }
    i_ = start;
    out_ = &out;
    while (i_ < n_ && ok_) {
      if (!Node(0)) break;
    }
    return ok_;
  }

 private:
  uint32_t Varint() {
    uint32_t v = 0;
    for (int shift = 0; i_ < n_ && shift < 35; shift += 7) {
      const uint8_t c = d_[i_++];
      v |= uint32_t(c & 0x7F) << shift;
      if (!(c & 0x80)) return v;
    }
    ok_ = false;
    return 0;
  }
  const std::string& String(uint32_t index) {
    static const std::string none;
    return index == 0 || index > strings_.size() ? none : strings_[index - 1];
  }

  // One node and its children; false at the end of a list of children (or on an error).
  bool Node(int depth) {
    if (depth > 64 || i_ >= n_) return ok_ = false;
    const uint32_t tag = Varint();
    if (tag == 0 || !ok_) return false;
    if (i_ >= n_) return ok_ = false;
    const uint8_t flags = d_[i_++];
    const std::string& name = String(tag);
    std::string attr_name, attr_ref;
    for (int a = 0; a < (flags & 0x0F); ++a) {
      const std::string& k = String(Varint());
      const std::string& v = String(Varint());
      if (k == "name") attr_name = v;
      else if (k == "ref") attr_ref = v;
    }
    const uint8_t* value = nullptr;
    uint32_t value_count = 0, value_size = 0;
    switch ((flags >> 4) & 7) {
      case 0: break;
      case 2: Varint(); break;
      case 6: {
        const uint32_t c = Varint();
        if (i_ + c > n_) return ok_ = false;
        i_ += c;
        break;
      }
      case 7: {
        value_count = Varint();
        if (i_ >= n_) return ok_ = false;
        value_size = d_[i_++];
        if (i_ + size_t(value_count) * value_size > n_) return ok_ = false;
        value = d_ + i_;
        i_ += size_t(value_count) * value_size;
        break;
      }
      default: return ok_ = false;
    }
    // A child of an instance (the partition the root, its instances below it): its transform or its blueprint.
    if (depth == 2 && in_instance_) {
      if (name == "complex" && attr_name == "Transform" && value && value_count == 16 && value_size == 4) {
        for (int k = 0; k < 16; ++k) placed_.m[k] = BeFloat(value + 4 * k);
        has_transform_ = true;
      } else if (name == "field" && attr_name == "ReferencedObject" && !attr_ref.empty() && attr_ref != "null") {
        placed_.blueprint = attr_ref;
      }
    }
    const bool instance = depth == 1 && name == "instance";
    if (instance) in_instance_ = true, has_transform_ = false, placed_.blueprint.clear();
    if (flags & 0x80) {
      while (ok_ && Node(depth + 1)) {
      }
      if (!ok_) return false;
    }
    if (instance) {
      if (has_transform_ && !placed_.blueprint.empty()) out_->push_back(placed_);
      in_instance_ = false;
    }
    return ok_;
  }

  const uint8_t* d_;
  size_t n_, i_ = 0;
  bool ok_ = true;
  std::vector<std::string> strings_;
  std::vector<Placed>* out_ = nullptr;
  bool in_instance_ = false, has_transform_ = false;
  Placed placed_{};
};

// A lamp of a model: where its light comes from in the model (x right, y up, z forward), where it points (a spot) or
// none (a point light), how far it reaches, its cone, how bright, its bulb's glow.
struct LampPart {
  float x, y, z;
  float dx, dy, dz;
  float range, inner, outer, strength, glow;
};

struct LampModel {
  const char* blueprint;  // the start of the blueprint's reference (lower case)
  std::vector<LampPart> parts;
};

const std::vector<LampModel>& Models() {
  // The lamppost's glasses (Level04_light): centres -0.48 / 0.48, 6.35, 0.42; each faces (-/+0.15, -0.93, 0.35).
  static const std::vector<LampModel> models = {
      {"objects/beach_objects/lamppost_01/lamppost_01/",
       {{-0.48f, 6.33f, 0.42f, -0.15f, -0.93f, 0.35f, 18.0f, 40.0f, 75.0f, 7.0f, 0.10f},
        {0.48f, 6.33f, 0.43f, 0.15f, -0.93f, 0.35f, 18.0f, 40.0f, 75.0f, 7.0f, 0.10f}}},
      {"objects/beach_objects/bomb_lamp/bomb_lamp/", {{0.0f, 0.27f, 0.0f, 0.0f, 0.0f, 0.0f, 9.0f, 0.0f, 0.0f, 2.0f, 0.05f}}},
  };
  return models;
}

}  // namespace

void NightJeeps();

// The level's main partition's bytes (levels/<map>.dbx): its lamps to the renderer (none if it has none).
void NightLampsFromLevel(const uint8_t* data, size_t size, const std::string& folder) {
  std::vector<Placed> placed;
  if (!Reader(data, size).Read(placed)) {
    native_x_renderer::NightLamps(nullptr, 0);
    REXLOG_WARN("BF level {}: its objects could not be read ({} placed before the error): no lamps at night", folder,
                placed.size());
    return;
  }
  std::vector<native_x_renderer::NightLamp> lamps;
  uint32_t objects = 0;
  for (const Placed& p : placed) {
    std::string lower = p.blueprint;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const LampModel& model : Models()) {
      if (lower.rfind(model.blueprint, 0) != 0) continue;
      ++objects;
      const float* r = p.m;  // right 0..2, up 4..6, forward 8..10, position 12..14
      for (const LampPart& part : model.parts) {
        native_x_renderer::NightLamp l;
        for (int k = 0; k < 3; ++k) {
          l.position[k] = r[12 + k] + part.x * r[k] + part.y * r[4 + k] + part.z * r[8 + k];
          l.direction[k] = part.dx * r[k] + part.dy * r[4 + k] + part.dz * r[8 + k];
        }
        const float n = std::sqrt(l.direction[0] * l.direction[0] + l.direction[1] * l.direction[1] +
                                  l.direction[2] * l.direction[2]);
        for (float& v : l.direction) v = n > 1e-4f ? v / n : 0.0f;
        // A warm lamp (about 2700 K).
        l.color[0] = 1.0f * part.strength, l.color[1] = 0.76f * part.strength, l.color[2] = 0.46f * part.strength;
        l.range = part.range;
        l.cone_inner = part.inner, l.cone_outer = part.outer;
        l.glow = part.glow;
        lamps.push_back(l);
      }
      break;
    }
  }
  native_x_renderer::NightLamps(lamps.data(), uint32_t(lamps.size()));
  REXLOG_INFO("BF level {}: {} placed objects, {} with lamps ({} lamps) for the night", folder, placed.size(), objects,
              lamps.size());
  NightJeeps();
}

// The jeeps' headlights (TekRantGaming: "I think the Jeep's headlights should work as well"): the Willys MB's and the
// Kurogane's bodies (their MainBody metal part: Willys-MB_Mesh and kurogane_mesh, levels of detail 0 to 2 by their
// index counts; the farthest left out, its counts small enough to be another model's), two lamps at the front of each
// (the bodies' boxes: the Willys from z -1.97 to 1.81, 0.3 to 1.54 m up, its lamps in the grille; the Kurogane's on its
// wings, z to 1.97), pointing ahead and a little down. Their own space: x right, y up, z forward.
void NightJeeps() {
  auto lamp = [](float x, float y, float z) {
    native_x_renderer::NightLamp l;
    l.position[0] = x, l.position[1] = y, l.position[2] = z;
    l.direction[0] = 0.0f, l.direction[1] = -0.08f, l.direction[2] = 1.0f;
    l.color[0] = 9.0f, l.color[1] = 8.1f, l.color[2] = 6.3f;  // a little warmer than white
    l.range = 30.0f;
    l.cone_inner = 13.0f, l.cone_outer = 30.0f;
    l.glow = 0.08f;
    return l;
  };
  std::vector<native_x_renderer::NightModelLamps> models;
  // Their main body's matrix: the third part's (the parts in alphabetical order, Glass01 and Glass02 first; the
  // landing boat's Left_MG and Left_MG_pivot), c23 to c25 (run nightboat4: c16 the camera, c17 on rigid rows).
  auto add = [&](uint32_t indices, float x, float y, float z) {
    native_x_renderer::NightModelLamps m;
    m.index_count = indices;
    m.matrix_register = 23;
    m.lamps[0] = lamp(-x, y, z), m.lamps[1] = lamp(x, y, z);
    m.lamp_count = 2;
    models.push_back(m);
  };
  for (uint32_t n : {18861u, 9975u, 4545u}) add(n, 0.30f, 0.95f, 1.70f);  // Willys MB
  for (uint32_t n : {17403u, 8985u, 4308u}) add(n, 0.62f, 1.03f, 1.85f);  // Kurogane
#if defined(BF_DEV_TOOLS)
  // BF_DEV_NIGHT_BOAT_LAMPS=1 (tests): the landing boat (gen_Landingcraft, Main_Body, 19242 and 9303 indices; its box z
  // -4.57 to 4.71, up to 2.39 m) gets two at its +z end, high: the model lamps' placement checked at the carrier's spawn.
  if (const char* v = std::getenv("BF_DEV_NIGHT_BOAT_LAMPS"); v && *v == '1') {
    for (uint32_t n : {19242u, 9303u}) add(n, 1.2f, 2.6f, 4.6f);
  }
#endif
  native_x_renderer::NightModels(models.data(), uint32_t(models.size()));
}

}  // namespace bf1943
