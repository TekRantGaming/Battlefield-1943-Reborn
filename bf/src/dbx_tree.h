// Frostbite 1's binary dbx ("{binary}"), read into a tree (the menu atlases' sprite tables and the fonts' glyph
// tables; night_lamps.cpp reads the levels with its own stream reader). The layout, as night_lamps.cpp has it: the
// node data's file offset, 0, that offset minus 24, the string count, 0, the string offsets (one fewer than the count,
// from byte 28), the strings; each node a tag (a varint string index, 0 ends a list of children), a flags byte (the
// low nibble the attribute count; bits 4 to 6 the value: 2 a text, 6 bytes, 7 an array of a byte size; 0x80 children
// follow), the attributes (name and value string indices), the value, the children.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace bf1943::dbx {

struct Node {
  std::string tag;
  std::vector<std::pair<std::string, std::string>> attrs;
  std::string text;             // a text value
  std::vector<uint8_t> bytes;   // a bytes value, or an array's data
  int array_size = 0;           // an array's element size (0: not an array)
  std::vector<Node> children;

  const std::string* Attr(const char* name) const {
    for (const auto& [k, v] : attrs)
      if (k == name) return &v;
    return nullptr;
  }
  // The child field (or complex) called `name`.
  const Node* Field(const char* name) const {
    for (const Node& c : children)
      if (const std::string* n = c.Attr("name"); n && *n == name) return &c;
    return nullptr;
  }
  // A one element array's number, big endian.
  bool Int(int32_t& out) const {
    if (array_size != 4 || bytes.size() < 4) return false;
    out = int32_t(uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3]);
    return true;
  }
  bool Float(float& out) const {
    int32_t i;
    if (!Int(i)) return false;
    std::memcpy(&out, &i, 4);
    return true;
  }
};

class Parser {
 public:
  Parser(const uint8_t* d, size_t n) : d_(d), n_(n) {}

  bool Parse(std::vector<Node>& roots) {
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
    while (i_ < n_ && ok_) {
      Node n;
      if (!ReadNode(n, 0)) break;
      roots.push_back(std::move(n));
    }
    return ok_ && !roots.empty();
  }

 private:
  static uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

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
  bool ReadNode(Node& out, int depth) {
    if (depth > 64 || i_ >= n_) return ok_ = false;
    const uint32_t tag = Varint();
    if (tag == 0 || !ok_) return false;
    if (i_ >= n_) return ok_ = false;
    const uint8_t flags = d_[i_++];
    out.tag = String(tag);
    for (int a = 0; a < (flags & 0x0F); ++a) {
      const std::string& k = String(Varint());
      const std::string& v = String(Varint());
      out.attrs.emplace_back(k, v);
    }
    switch ((flags >> 4) & 7) {
      case 0: break;
      case 2: out.text = String(Varint()); break;
      case 6: {
        const uint32_t c = Varint();
        if (i_ + c > n_) return ok_ = false;
        out.bytes.assign(d_ + i_, d_ + i_ + c);
        i_ += c;
        break;
      }
      case 7: {
        const uint32_t c = Varint();
        if (i_ >= n_) return ok_ = false;
        const uint8_t size = d_[i_++];
        if (i_ + size_t(c) * size > n_) return ok_ = false;
        out.array_size = size;
        out.bytes.assign(d_ + i_, d_ + i_ + size_t(c) * size);
        i_ += size_t(c) * size;
        break;
      }
      default: return ok_ = false;
    }
    if (flags & 0x80) {
      while (ok_) {
        Node child;
        if (!ReadNode(child, depth + 1)) break;
        out.children.push_back(std::move(child));
      }
      if (!ok_) return false;
    }
    return ok_;
  }

  const uint8_t* d_;
  size_t n_, i_ = 0;
  bool ok_ = true;
  std::vector<std::string> strings_;
};

inline bool Parse(const std::vector<uint8_t>& data, std::vector<Node>& roots) {
  return Parser(data.data(), data.size()).Parse(roots);
}

// Every instance (a partition's children) whose type is `type`.
inline std::vector<const Node*> Instances(const std::vector<Node>& roots, const char* type) {
  std::vector<const Node*> out;
  for (const Node& r : roots)
    for (const Node& c : r.children)
      if (c.tag == "instance")
        if (const std::string* t = c.Attr("type"); t && *t == type) out.push_back(&c);
  return out;
}

}  // namespace bf1943::dbx
