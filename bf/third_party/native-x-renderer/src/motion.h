// NativeXRenderer - the motion of objects that move on their own, for DLAA (internal, D48).
//
// DLAA's motion vectors come from the depth and the camera (upscaler.cpp), which is right for everything
// that stands still. Characters (their bones are vertex shader constants) and the props a game animates
// move on their own; for those, the main 3D view's draws that write depth are matched with the last
// frame's:
//   - a draw is known by a key: its vertex shader, its vertex and index data, its vertex count. Several
//     draws with one key (one mesh drawn in several places) are told apart by their constants: the
//     closest of the last frame's is the same object;
//   - it moved if its vertex shader's float constants changed, apart from the registers the whole view
//     shares: the camera's, which every draw that uses them sees with one value, and which half of the
//     view's draws or more use. They are found in each frame, for the next;
//   - a draw that moved is drawn again before DLSS with its motion vertex shader (spirv_patch.h), its
//     constants of this frame and of the last, and the same viewport, depth test equal: it writes its
//     motion where it is what the scene shows (draw.cpp, DrawObjectMotion).

#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <utility>
#include <vector>

#include "GraphicsTypes.h"

namespace native_x_renderer {

// The motion vectors DLSS takes: from this frame's position to the last frame's, in host pixels.
constexpr Diligent::TEXTURE_FORMAT kMotionVectorFormat = Diligent::TEX_FORMAT_RG16_FLOAT;

class ObjectMotion {
 public:
  // A draw of the main 3D view that writes depth. `constants`: its vertex shader's float constants, 16 bytes
  // each, from the registers listed in `registers` (`count` of them). Returns the last frame's constants of
  // the same object if it moved since then; nullptr if it did not, or if it is new. The pointer is valid
  // until EndFrame. `matched`, when given: the last frame's constants of the same object whether it moved or not
  // (the camera vote's, D118), or nullptr if it is new.
  const uint8_t* Match(uint64_t key, const uint8_t* constants, const uint8_t* registers, uint32_t count,
                       const uint8_t** matched = nullptr);
  // Right after Match returned `previous` for a draw: whether its `constants` differ from those only at the positions
  // [from, to) of its register list and in the registers the view shares. Its own animation (bones) is elsewhere, so
  // then it moved only as its matrix says (D120).
  bool ChangedOnlyIn(const uint8_t* constants, const uint8_t* previous, uint32_t from, uint32_t to) const;
  // After the frame: its draws become the last frame's.
  void EndFrame();

  // Draws matched and draws that moved, since the last call.
  struct Stats {
    uint32_t matched = 0, moved = 0;
    uint64_t candidates = 0;  // the last frame's draws compared with
    uint64_t summarized = 0;  // of them, ruled out by their bounds alone (D143)
    uint64_t by_hash = 0;     // draws that did not move, found by their hash (D143)
    uint64_t unbounded = 0, unhashed = 0;  // draws compared with every one: no valid bounds; the last frame not hashed
    uint64_t limited = 0;     // draws past the frame's budget, compared with their kLimited closest only (D143)
    uint64_t match_ns = 0;    // time in Match
    uint32_t longest_key = 0; // the most draws of one key in a frame
  };
  Stats TakeStats() {
    Stats stats = stats_;
    stats_ = {};
    return stats;
  }

 private:
  // D143: a draw's bounds, a summary of its constants the view does not share: for each of kGroups groups of them in
  // order, the sums of their x, y, z and w. Two draws' summaries differ by no more than the draws do (the difference of
  // two sums is at most the sum of the differences), so a draw whose summary is further than the closest so far is not
  // the closest; compared beside the key, they rule most of a key's draws out without reading their constants.
  static constexpr uint32_t kGroups = 4;
  // And a sample: kSamples of those constants spread over them, whole. The distance over them is part of the whole
  // distance too, and does not average out where one draw holds many objects (Guadalcanal's plants: a draw of 256
  // constants, a batch of them, whose sums differ little from another batch's).
  static constexpr uint32_t kSamples = 8;
  // Values are clamped to +-1e6 first (not a number: 0): the difference of two clamped values is no more than the
  // distance counts for them (at most 1e30, NaN as far), so a draw with garbage in an unused register keeps its
  // bounds. The sums stay in double: their rounding must not exceed the room left for it.
  struct Bounds {
    double sums[kGroups * 4];
    float samples[kSamples * 4];
    uint32_t kept;  // the constants summed and sampled (the same for a key's draws)
    bool valid;     // false: none
  };
  struct Entry {
    uint64_t key;
    uint32_t offset;        // in data
    uint32_t registers_at;  // in registers
    uint32_t count;
    uint64_t hash;  // of its constants the view does not share (D143)
    Bounds bounds;
  };
  // A key's draws, in the order drawn, beside their hashes and bounds.
  struct IndexItem {
    uint64_t key;
    uint32_t entry;
    uint64_t hash;
    Bounds bounds;
    bool operator<(const IndexItem& o) const { return key != o.key ? key < o.key : entry < o.entry; }
  };
  struct Frame {
    std::vector<Entry> entries;
    std::vector<uint8_t> data;
    std::vector<uint8_t> registers;  // each draw's register numbers (D143: its hash and bounds made again)
    // (key, entry) sorted, made at the frame's end: the entries of a key are one run, in the order drawn (D122: a hash
    // map allocated a node for every draw).
    std::vector<IndexItem> index;
    std::bitset<256> shared;  // the view's shared registers when it was drawn (its hashes leave them out)
    void Clear() {
      entries.clear();
      data.clear();
      registers.clear();
      index.clear();
    }
  };
  Frame frames_[2];
  uint32_t current_ = 0;
  // A draw's runs of constants the view does not share, as positions [first, second) in its list (D122).
  std::vector<std::pair<uint32_t, uint32_t>> runs_;
  std::vector<uint8_t> scratch_;  // a draw's constants the view does not share, hashed (D143)
  std::vector<float> parts_;      // a key's draws' distances in their bounds (D143)
  // D143: a frame's matching past kBudgetNs compares the rest of its draws whole with their kLimited closest only.
  static constexpr uint64_t kBudgetNs = 2000000;
  static constexpr uint32_t kLimited = 4;
  uint64_t frame_match_ns_ = 0;
  std::vector<std::pair<float, uint32_t>> order_;
  // A draw's hash and bounds, without the shared registers (D143).
  void Summarize(const uint8_t* constants, const uint8_t* registers, uint32_t count, uint64_t& hash, Bounds& bounds);

  // The registers the view shares (the last frame's), and this frame's count of them.
  std::bitset<256> shared_;
  std::array<std::array<uint32_t, 4>, 256> view_values_{};
  std::array<uint32_t, 256> view_users_{};
  std::bitset<256> view_mixed_;
  uint32_t view_draws_ = 0;
  Stats stats_;
};

}  // namespace native_x_renderer
