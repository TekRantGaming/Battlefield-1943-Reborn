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
#include <cstdio>
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
  float r = 1.0f, g = 0.76f, b = 0.46f;  // its colour (a warm lamp, about 2700 K, unless given)
  float flicker = 0.0f, spin = 0.0f;     // D170: firelight's waver; a turning beam (radians a second)
};
// D177: a street lamp's sodium light (the user, 16 Nov 2026: "like sodium ... lightbulbs would from street lights"):
// a deep amber, about 2100 K.
#define BF_SODIUM 1.0f, 0.56f, 0.17f
// D170 colours: firelight, a lantern's flame, a red aviation light.
constexpr float kFire[3] = {1.0f, 0.52f, 0.20f};
constexpr float kLantern[3] = {1.0f, 0.68f, 0.36f};
constexpr float kRed[3] = {1.0f, 0.06f, 0.04f};
LampPart Lamp(float x, float y, float z, float range, float strength, float glow, const float* c, float flicker = 0.0f) {
  LampPart l{x, y, z, 0.0f, 0.0f, 0.0f, range, 0.0f, 0.0f, strength, glow};
  l.r = c[0], l.g = c[1], l.b = c[2], l.flicker = flicker;
  return l;
}
LampPart Beam(float x, float y, float z, float dx, float dy, float dz, float range, float inner, float outer,
              float strength, float glow, float spin, const float* c) {
  LampPart l{x, y, z, dx, dy, dz, range, inner, outer, strength, glow};
  l.r = c[0], l.g = c[1], l.b = c[2], l.spin = spin;
  return l;
}
// A work lamp hanging under a roof, shining down (a sodium lamp, as the street lamps: D177).
LampPart Down(float x, float y, float z, float range, float inner, float outer, float strength, float glow) {
  return LampPart{x, y, z, 0.0f, -1.0f, 0.0f, range, inner, outer, strength, glow, BF_SODIUM};
}

struct LampModel {
  const char* blueprint;  // the start of the blueprint's reference (lower case)
  std::vector<LampPart> parts;
  float share = 1.0f;  // D170: the share of these objects lit (some houses lived in, the rest dark), by where they stand
};

const std::vector<LampModel>& Models() {
  // The lamppost's glasses (Level04_light): centres -0.48 / 0.48, 6.35, 0.42; each faces (-/+0.15, -0.93, 0.35).
  static const std::vector<LampModel> models = {
      {"objects/beach_objects/lamppost_01/lamppost_01/",
       {{-0.48f, 6.33f, 0.42f, -0.15f, -0.93f, 0.35f, 22.0f, 40.0f, 75.0f, 8.0f, 0.09f, BF_SODIUM},
        {0.48f, 6.33f, 0.43f, 0.15f, -0.93f, 0.35f, 22.0f, 40.0f, 75.0f, 8.0f, 0.09f, BF_SODIUM}}},
      {"objects/beach_objects/bomb_lamp/bomb_lamp/", {{0.0f, 0.27f, 0.0f, 0.0f, 0.0f, 0.0f, 9.0f, 0.0f, 0.0f, 2.0f, 0.05f}}},
      // D170 (TekRantGaming, 14 Nov 2026, the night maps' lighting): each from the model's own parts
      // (tools/level_objects.py for where they stand, the meshes' part bounds for where on them).
      // The houses' fire emitters (Houseeffects_Fire): inside the houses, no flames drawn while the house stands (runs
      // nfire1 and nfire2: its light seen only through the door). A stove's or a lantern's glow in the room, wavering a
      // little, its reach kept inside the walls.
      {"objects/architecture/houseeffects/houseeffects_fire_t01/houseeffects_fire_t01", {Lamp(0.0f, 1.2f, 0.0f, 5.5f, 2.4f, 0.0f, kLantern, 0.4f)}},
      {"objects/architecture/houseeffects/houseeffects_fire_t01/houseeffect_fire_t02_small", {Lamp(0.0f, 0.8f, 0.0f, 4.5f, 1.6f, 0.0f, kLantern, 0.4f)}},
      // Iwo Jima's lighthouse: its lens (part_2_LighthouseLens, 17.58 to 19.16 m) turns a beam across the sea and the
      // shore, and lights its own lantern room.
      // The beam dips a quarter (it reaches the sea and the shore 70 to 160 m out, run nlight2: level ones lit nothing);
      // the lantern's own glow, not the beam's (its was a white disc).
      // TekRantGaming, 20 Nov 2026: "far too bright ... casting two simultaneous lights which shoot off in either
      // direction ... could become annoying for players ... reduce this to just one beam ... reduce the rotation speed
      // significantly ... the colour ... a modern day white LED, whereas the rest of the light sources in the game offer
      // a much more amber tone that fits the time period": one beam (was two, opposite), strength 8 (was 30), a turn
      // in 42 s (0.15 radians a second, was 0.55: 11.4 s), the lanterns' amber (was 1, 0.92, 0.78). Its shaft in the
      // air (NativeXRenderer D195) takes the same colour and strength.
      {"objects/architecture/beach/lighthouse_t01/lighthouse_t01",
       {Beam(0.0f, 18.37f, 0.0f, 1.0f, -0.25f, 0.0f, 260.0f, 3.0f, 8.0f, 8.0f, 0.0f, 0.15f, kLantern),
        Lamp(0.0f, 18.4f, 0.0f, 7.0f, 2.5f, 0.45f, kLantern)}},
      // The airfield's control tower: a dim lamp in its glass cab (14.25 to 15.69 m) and a red light on its roof (16.05 m).
      {"objects/architecture/beach/airtower_t01/airtower_t01",
       {Lamp(0.0f, 14.9f, 0.0f, 9.0f, 1.6f, 0.0f, kLantern), Lamp(0.0f, 16.3f, 0.0f, 3.0f, 1.2f, 0.12f, kRed)}},
      // The guard towers: a lantern under the roof (the platform 9.2 m, the roof 11.6 to 12.05 m).
      {"objects/architecture/beach/guardtower_t01/guardtower_t01", {Lamp(0.0f, 11.2f, 0.0f, 12.0f, 2.2f, 0.10f, kLantern, 0.25f)}},
      // Guadalcanal's wooden watchtowers: a lantern on the platform (4.1 to 6.75 m, the roof 6.95 m).
      {"objects/architecture/military/mil_watchtower_t01v01/mil_watchtower_t01v01", {Lamp(0.0f, 6.1f, 0.0f, 10.0f, 1.8f, 0.09f, kLantern, 0.25f)}},
      // The radio masts: a red light at the top (6.75 m and 4.8 m).
      {"objects/beach_objects/antenna_01/antenna_01", {Lamp(0.0f, 6.85f, 0.0f, 2.5f, 0.9f, 0.10f, kRed)}},
      {"objects/beach_objects/antenna_02/antenna_02", {Lamp(0.0f, 4.9f, 0.0f, 2.5f, 0.9f, 0.09f, kRed)}},
      // D195: the large buoys that mark the edge of the play area (Wake 61, Iwo Jima 35, Guadalcanal 32; the user, 18 Nov
      // 2026: "keep the buoys that mark the edges of the maps lit"): a red light on the top of each (its mesh: 1.6 m
      // across, 6.13 m over its origin at the sea), seen from far off.
      {"objects/street_objects/buoylarge_t01v01_br/buoylarge_t01v01_static", {Lamp(0.0f, 6.0f, 0.0f, 4.0f, 1.4f, 0.22f, kRed)}},
      // Wake's water towers: a red light on the tank (its top 7.28 m).
      {"objects/street_objects/watertower_t01v01_br/watertower_t01v01_br", {Lamp(0.0f, 7.45f, 0.0f, 2.5f, 0.9f, 0.09f, kRed)}},
      // The airfields' hangars: two work lamps under the roof (8.1 m), their pools inside the walls (+-13 m by +-10 m).
      {"objects/architecture/beach/hangar_t01/hangar_t01",
       {Down(-5.0f, 7.4f, 0.0f, 13.0f, 25.0f, 50.0f, 4.5f, 0.12f), Down(5.0f, 7.4f, 0.0f, 13.0f, 25.0f, 50.0f, 4.5f, 0.12f)}},
      // The open vehicle sheds and the repair platforms: a lamp under the roof (4.5 m, 5.5 m).
      {"objects/architecture/industry/ind_vehicle_shed_t01v01/ind_vehicle_shed_t01v01",
       {Down(0.0f, 3.45f, 0.4f, 6.5f, 30.0f, 62.0f, 2.2f, 0.08f)}},
      {"objects/architecture/beach/repairplatform_t01/repairplatform_t01", {Down(0.0f, 4.95f, -0.4f, 9.0f, 30.0f, 60.0f, 3.0f, 0.10f)}},
      // The jetties: a lantern on the far post (the small jetty's planks 3.7 to 4 m, its posts to 5.08 m; the big
      // jetty's planks 9 m, its rail 10.2 m, 40 m long: three, side to side).
      {"objects/objects/jettysmall_1/jettysmall_1", {Lamp(0.94f, 5.2f, 14.6f, 7.0f, 1.5f, 0.08f, kLantern, 0.15f)}},
      {"objects/objects/jettysmall_3/jettysmall_3", {Lamp(0.93f, 5.05f, -0.4f, 7.0f, 1.5f, 0.08f, kLantern, 0.15f)}},
      {"objects/objects/jettybig/jettybig",
       {Lamp(1.95f, 10.3f, -15.0f, 9.0f, 1.8f, 0.08f, kLantern, 0.15f), Lamp(-1.95f, 10.3f, 0.0f, 9.0f, 1.8f, 0.08f, kLantern, 0.15f),
        Lamp(1.95f, 10.3f, 15.0f, 9.0f, 1.8f, 0.08f, kLantern, 0.15f)}},
      // Guadalcanal's stone harbour (its deck 19.08 m, 10 m by 40 m): lanterns at its edges.
      {"objects/objects/harbour_04/harbour_04",
       {Lamp(4.0f, 19.6f, -12.0f, 9.0f, 1.7f, 0.03f, kLantern, 0.12f), Lamp(-4.0f, 19.6f, 12.0f, 9.0f, 1.7f, 0.03f, kLantern, 0.12f)}},
      // The bunkers and pillboxes: a dim lantern inside (its light within their walls, seen through the slits).
      {"objects/architecture/military/mil_bunker_t01v01/mil_bunker_t01v01", {Lamp(0.4f, 1.5f, 0.4f, 3.2f, 1.1f, 0.05f, kLantern, 0.3f)}},
      {"objects/objects/pillboxbunker/pillboxbunker", {Lamp(0.0f, 1.3f, 0.0f, 2.6f, 0.9f, 0.04f, kLantern, 0.3f)}},
      {"objects/architecture/beach/bunker_1/bunker_1", {Lamp(0.0f, 1.5f, 0.0f, 3.4f, 1.0f, 0.0f, kLantern, 0.3f)}},
      {"objects/architecture/beach/radarbunker_t01/radarbunker_t01", {Lamp(0.0f, 1.2f, 0.0f, 3.4f, 1.0f, 0.0f, kLantern, 0.3f)}},
      // Lit rooms (their light inside the walls: 14 m by 8 m houses, the storeroom, the factory's upper floor seen
      // through its windows): some of the houses, all of the rest.
      {"objects/architecture/beach/beachhouse_t01/beachhouse_t01", {Lamp(2.5f, 2.6f, 0.0f, 4.6f, 1.6f, 0.0f, kLantern, 0.12f)}, 0.45f},
      {"objects/architecture/beach/beachhouse_t02/beachhouse_t02", {Lamp(0.0f, 2.6f, -8.0f, 4.6f, 1.6f, 0.0f, kLantern, 0.12f)}, 0.45f},
      {"objects/architecture/beach/storeroom_t01/storeroom_t01", {Lamp(0.0f, 3.2f, 0.0f, 5.0f, 1.5f, 0.0f, kLantern, 0.1f)}},
      {"objects/architecture/beach/factory_t01/factory_t01", {Lamp(4.2f, 3.8f, 5.6f, 8.0f, 2.2f, 0.0f, kLantern, 0.08f)}},
  };
  return models;
}

// D222: the rooms of the buildings, brighter at night (TekRantGaming, 23 Nov 2026: "the insides of buildings are far
// too dark", and most of them have nowhere a lamp could hang; the review's option C). Each a box in the building's own
// space (x right, y up, z forward): the inside faces of its outer walls, its floor and its ceiling, measured from the
// lowest level of detail's triangles (each building cut at 1.4 m over its floor, the walls' inner faces found along
// lines across it; the beach houses' pitched roofs from cuts every half metre up). The rooms within a building's
// inner walls are left as one: both faces of an inner wall are inside. The beach houses' roof falls 0.495 m a metre
// from its ridge. Measured on 23 and 24 Nov 2026 from the meshes' triangles (positions as half floats; the warehouse's
// as 32 bit floats).
struct RoomBox {
  float x0, x1, y0, y1, z0, z1;
  float slope_x = 0.0f, slope_z = 0.0f;  // the top falls this much a metre from the box's middle along x and z
};
struct RoomModel {
  const char* blueprint;
  std::vector<RoomBox> boxes;
  float lift;  // how many times as bright at full night (run rooms_ab1: 3 left a beach house nearly black)
  // A part drawn only while the building stands, by its draws' index counts (its mesh's triangles times 3, levels of
  // detail 0 and 1; 0 none): the beach houses' roof (part_01_Roof_Exterior; the short house's whole baked mesh far
  // off). The storeroom's, the factory's and the hangar's are their largest breakable parts (not yet seen destroyed).
  uint32_t intact0 = 0, intact1 = 0;
};
const std::vector<RoomModel>& Rooms() {
  static const std::vector<RoomModel> rooms = {
      // The beach houses: walls 0.3 m thick, the floor at 0.2 m, open to the roof (its underside 5.54 m at the ridge).
      {"objects/architecture/beach/beachhouse_t01/beachhouse_t01", {{-6.70f, 6.70f, 0.15f, 5.54f, -3.70f, 3.70f, 0.0f, 0.495f}}, 5.0f, 603, 7998},
      // The long beach house: an L, its wing along z (ridge 5.39 m, along z) and its part across (as the short house).
      {"objects/architecture/beach/beachhouse_t02/beachhouse_t02",
       {{-6.70f, -0.21f, 0.15f, 5.39f, -12.40f, -3.70f, 0.495f, 0.0f}, {-6.70f, 6.70f, 0.15f, 5.54f, -3.70f, 3.70f, 0.0f, 0.495f}},
       5.0f, 1038},
      // The storeroom: its roof falls 0.31 m a metre from the ridge (5.96 m) over its width.
      {"objects/architecture/beach/storeroom_t01/storeroom_t01", {{-6.70f, 6.70f, 1.05f, 5.96f, -4.20f, 4.20f, 0.0f, 0.31f}}, 5.0f, 5541, 6366},
      // The factory: its hall, the strip at its right, its tower part at the back.
      {"objects/architecture/beach/factory_t01/factory_t01",
       {{-4.60f, 11.11f, 1.85f, 5.95f, 0.88f, 14.62f}, {11.11f, 13.00f, 1.85f, 5.95f, 2.73f, 14.62f},
        {4.70f, 11.08f, 1.85f, 5.45f, -3.35f, 0.88f}},
       5.0f, 11415, 14319},
      // The hangar (sheet metal 4 cm thick; its own work lamps light it too): its roof rises to 8.2 m over the middle.
      {"objects/architecture/beach/hangar_t01/hangar_t01", {{-13.07f, 13.07f, 0.15f, 8.20f, -10.03f, 10.03f, 0.0f, 0.21f}}, 4.0f, 1005, 6123},
      // Iwo Jima's warehouse (its mesh's positions are 32 bit floats, not halves): walls 0.3 m, the floor at 0.2 m, its roof's
      // underside 8.11 m at the ridge, falling 0.197 m a metre; its roof part1_Roof_Exterior (1152 indices).
      {"objects/architecture/beach/warehouse_t01/warehouse_t01", {{-12.70f, 12.70f, 0.15f, 8.11f, -9.70f, 9.70f, 0.0f, 0.197f}}, 5.0f, 1152, 12117},
      // The control tower's glass cab.
      {"objects/architecture/beach/airtower_t01/airtower_t01", {{-1.92f, 1.92f, 13.15f, 15.65f, -1.92f, 1.92f}}, 3.5f},
      // The lighthouse's two floors: round, its inside 3.37 m from the middle, its outside narrowing from 4.2 m at the
      // foot to 3.98 m at 7.5 m and 3.74 m at 15 m, so three boxes across each other per floor whose corners stay inside
      // the outer face (runs rooms_iwo1 to rooms_iwo4: larger corners lit strips of its outside; the upper floor stops 0.4 m under the
      // gallery's underside at 15.7 m, which a top at 15.65 m lit from outside).
      {"objects/architecture/beach/lighthouse_t01/lighthouse_t01",
       {{-3.37f, 3.37f, -0.45f, 7.35f, -2.00f, 2.00f}, {-2.00f, 2.00f, -0.45f, 7.35f, -3.37f, 3.37f},
        {-2.75f, 2.75f, -0.45f, 7.35f, -2.75f, 2.75f}, {-3.37f, 3.37f, 7.45f, 15.30f, -1.30f, 1.30f},
        {-1.30f, 1.30f, 7.45f, 15.30f, -3.37f, 3.37f}, {-2.50f, 2.50f, 7.45f, 15.30f, -2.50f, 2.50f}},
       4.0f},
      // The bunkers (their concrete 0.5 m or more; a lantern inside each too). Not the radar bunker or the pillbox: solid,
      // no floor inside to speak of (5 m2; run rooms_dbg1 lit the radar bunker's front).
      {"objects/architecture/military/mil_bunker_t01v01/mil_bunker_t01v01", {{-3.25f, 3.92f, 0.35f, 2.95f, -3.68f, 1.66f}}, 4.0f},
      {"objects/architecture/beach/bunker_1/bunker_1", {{-3.85f, 3.85f, 0.35f, 2.95f, -3.35f, 3.35f}}, 4.0f},
  };
  return rooms;
}

}  // namespace

void NightJeeps();

// D222: the rooms of the placed buildings to the renderer (see Rooms()).
static void NightRoomsFromPlaced(const std::vector<Placed>& placed, const std::string& folder) {
  std::vector<native_x_renderer::NightRoom> out;
  uint32_t buildings = 0;
  for (const Placed& p : placed) {
    std::string lower = p.blueprint;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const RoomModel& model : Rooms()) {
      if (lower.rfind(model.blueprint, 0) != 0) continue;
      const float* r = p.m;  // right 0..2, up 4..6, forward 8..10, position 12..14
      float len[3], unit[3][3];
      for (int a = 0; a < 3; ++a) {
        const float* v = r + 4 * a;
        len[a] = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (int k = 0; k < 3; ++k) unit[a][k] = len[a] > 1e-4f ? v[k] / len[a] : 0.0f;
      }
      if (len[0] < 1e-4f || len[1] < 1e-4f || len[2] < 1e-4f) break;
      ++buildings;
      for (const RoomBox& b : model.boxes) {
        const float mid[3] = {0.5f * (b.x0 + b.x1), 0.5f * (b.y0 + b.y1), 0.5f * (b.z0 + b.z1)};
        native_x_renderer::NightRoom room;
        for (int k = 0; k < 3; ++k) room.centre[k] = r[12 + k] + mid[0] * r[k] + mid[1] * r[4 + k] + mid[2] * r[8 + k];
        for (int a = 0; a < 3; ++a) {
          for (int k = 0; k < 3; ++k) room.to_local[a][k] = unit[a][k];
          room.to_local[a][3] = -(unit[a][0] * room.centre[0] + unit[a][1] * room.centre[1] + unit[a][2] * room.centre[2]);
        }
        room.half[0] = 0.5f * (b.x1 - b.x0) * len[0];
        room.half[1] = 0.5f * (b.y1 - b.y0) * len[1];
        room.half[2] = 0.5f * (b.z1 - b.z0) * len[2];
        room.slope[0] = b.slope_x * len[1] / len[0];
        room.slope[1] = b.slope_z * len[1] / len[2];
        room.lift = model.lift;
        room.intact[0] = model.intact0, room.intact[1] = model.intact1;
        for (int k = 0; k < 3; ++k) room.building[k] = r[12 + k];
        out.push_back(room);
      }
      break;
    }
  }
  native_x_renderer::NightRooms(out.data(), uint32_t(out.size()));
  REXLOG_INFO("BF level {}: {} buildings with rooms ({} rooms) for the night", folder, buildings, out.size());
}

// The level's main partition's bytes (levels/<map>.dbx): its lamps to the renderer (none if it has none).
void NightLampsFromLevel(const uint8_t* data, size_t size, const std::string& folder) {
  std::vector<Placed> placed;
  if (!Reader(data, size).Read(placed)) {
    native_x_renderer::NightLamps(nullptr, 0);
    native_x_renderer::NightRooms(nullptr, 0);
    REXLOG_WARN("BF level {}: its objects could not be read ({} placed before the error): no lamps at night", folder,
                placed.size());
    return;
  }
  std::vector<native_x_renderer::NightLamp> lamps;
  uint32_t objects = 0;
#if defined(BF_DEV_TOOLS)
  // BF_DEV_FIND_OBJECT=<text> (tests): where the level places each object whose blueprint has that text (lower case),
  // the first 40 (D191: the window glass's buildings, to put a camera by them).
  if (const char* find = std::getenv("BF_DEV_FIND_OBJECT"); find && *find) {
    int said = 0;
    for (const Placed& p : placed) {
      std::string lower = p.blueprint;
      for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
      if (lower.find(find) == std::string::npos || said++ >= 40) continue;
      REXLOG_INFO("BF level {}: {} at {:.1f} {:.1f} {:.1f} (forward {:.2f} {:.2f})", folder, p.blueprint, p.m[12], p.m[13],
                  p.m[14], p.m[8], p.m[10]);
    }
  }
#endif
  for (const Placed& p : placed) {
    std::string lower = p.blueprint;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const LampModel& model : Models()) {
      if (lower.rfind(model.blueprint, 0) != 0) continue;
      const float* r = p.m;  // right 0..2, up 4..6, forward 8..10, position 12..14
      if (model.share < 1.0f) {  // which ones by where they stand, the same on every player's game
        const float h = std::fmod(std::fabs(r[12] * 12.9898f + r[14] * 78.233f), 1.0f);
        if (h >= model.share) break;
      }
      ++objects;
      for (const LampPart& part : model.parts) {
        native_x_renderer::NightLamp l;
        for (int k = 0; k < 3; ++k) {
          l.position[k] = r[12 + k] + part.x * r[k] + part.y * r[4 + k] + part.z * r[8 + k];
          l.direction[k] = part.dx * r[k] + part.dy * r[4 + k] + part.dz * r[8 + k];
        }
        const float n = std::sqrt(l.direction[0] * l.direction[0] + l.direction[1] * l.direction[1] +
                                  l.direction[2] * l.direction[2]);
        for (float& v : l.direction) v = n > 1e-4f ? v / n : 0.0f;
        l.color[0] = part.r * part.strength, l.color[1] = part.g * part.strength, l.color[2] = part.b * part.strength;
        l.flicker = part.flicker, l.spin = part.spin;
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
  NightRoomsFromPlaced(placed, folder);
  NightJeeps();
}

// The jeeps' headlights (TekRantGaming: "I think the Jeep's headlights should work as well"): the Willys MB's and the
// Kurogane's bodies (their MainBody metal part: Willys-MB_Mesh and kurogane_mesh, levels of detail 0 to 2 by their
// index counts; the farthest left out, its counts small enough to be another model's), two lamps at the front of each
// (the bodies' boxes: the Willys from z -1.97 to 1.81, 0.3 to 1.54 m up, its lamps in the grille; the Kurogane's on its
// wings, z to 1.97), pointing ahead and a little down. Their own space: x right, y up, z forward.
// D177: where the tanks' headlights are in their hulls' space (x right, y up, z forward; BF_DEV_TANK_LAMPS in a dev
// build: "sx,sy,sz,cx,cy,cz", to place them by eye).
float kShermanLamp[3] = {0.85f, 1.45f, 2.55f};
float kChiHaLamp[3] = {0.0f, 1.15f, 2.45f};
void NightJeeps() {
#if defined(BF_DEV_TOOLS)
  if (const char* v = std::getenv("BF_DEV_TANK_LAMPS"); v && *v)
    std::sscanf(v, "%f,%f,%f,%f,%f,%f", &kShermanLamp[0], &kShermanLamp[1], &kShermanLamp[2], &kChiHaLamp[0], &kChiHaLamp[1],
                &kChiHaLamp[2]);
#endif
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
  auto add = [&](uint32_t indices, float x, float y, float z, uint32_t matrix_register = 23) {
    native_x_renderer::NightModelLamps m;
    m.index_count = indices;
    m.matrix_register = matrix_register;
    m.lamps[0] = lamp(-x, y, z), m.lamps[1] = lamp(x, y, z);
    m.lamp_count = x != 0.0f ? 2 : 1;
    models.push_back(m);
  };
  for (uint32_t n : {18861u, 9975u, 4545u}) add(n, 0.30f, 0.95f, 1.70f);  // Willys MB
  for (uint32_t n : {17403u, 8985u, 4308u}) add(n, 0.62f, 1.03f, 1.85f);  // Kurogane
  // D177, the tanks (the user, 16 Nov 2026: the lights "along with tanks and wheeled vehicles"): their one material's
  // draw (US_ShermanM4_tank_Mesh, ChiHa_mb_Mesh: the triangle counts of their levels of detail 0 to 2, from each
  // level's data file's first subset, times three), the Hull's matrix (the Sherman's parts Barrel, Hook1, Hook2, Hull:
  // the fourth, c26; the Chi-Ha's Hull the first, c17). The Sherman's two headlights on its glacis' corners; the
  // Chi-Ha's one, in the middle of its hull's front.
  for (uint32_t n : {29745u, 14685u, 7416u}) add(n, kShermanLamp[0], kShermanLamp[1], kShermanLamp[2], 26);
  for (uint32_t n : {29418u, 14535u, 7281u}) add(n, kChiHaLamp[0], kChiHaLamp[1], kChiHaLamp[2], 17);
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
