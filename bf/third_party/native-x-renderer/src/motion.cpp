// NativeXRenderer - see motion.h.

#include "motion.h"

#include <rex/logging.h>
#include <xxhash.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace native_x_renderer {

namespace {
// NXR_MOTION_DUMP=<file>[,<first frame>,<frames>] (investigation): Match's inputs and the frames' ends, for replaying
// them in a benchmark (scratchpad motion_bench).
// NXR_MOTION_DUMP=<file>,auto,<frames>: from the frame after 30 running whose matching took over 5 ms each.
struct MotionDump {
  std::FILE* file = nullptr;
  uint64_t frame = 0, first = 0, last = 0, frames = 0;
  bool automatic = false;
  uint64_t frame_ns = 0;  // this frame's matching
  uint32_t slow = 0;      // frames running over 5 ms
  MotionDump() {
    const char* v = std::getenv("NXR_MOTION_DUMP");
    if (!v || !*v) return;
    std::string spec(v);
    const size_t comma = spec.find(',');
    const std::string path = spec.substr(0, comma);
    if (comma != std::string::npos && spec.compare(comma + 1, 4, "auto") == 0) {
      automatic = true;
      const size_t comma2 = spec.find(',', comma + 1);
      frames = comma2 != std::string::npos ? std::strtoull(spec.c_str() + comma2 + 1, nullptr, 10) : 900;
      first = last = UINT64_MAX;
    } else if (comma != std::string::npos) {
      first = std::strtoull(spec.c_str() + comma + 1, nullptr, 10);
      const size_t comma2 = spec.find(',', comma + 1);
      last = first + (comma2 != std::string::npos ? std::strtoull(spec.c_str() + comma2 + 1, nullptr, 10) : 300);
    } else {
      last = 300;
    }
    file = std::fopen(path.c_str(), "wb");
  }
  bool on() const { return file && frame >= first && frame < last; }
};
MotionDump& Dump() {
  static MotionDump dump;
  return dump;
}
}  // namespace

const uint8_t* ObjectMotion::Match(uint64_t key, const uint8_t* constants, const uint8_t* registers, uint32_t count,
                                   const uint8_t** matched) {
  const auto started = std::chrono::steady_clock::now();
  struct Timed {
    std::chrono::steady_clock::time_point at;
    uint64_t& ns;
    uint64_t& frame_ns;
    ~Timed() {
      const uint64_t d =
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - at).count());
      ns += d, frame_ns += d;
    }
  } timed{started, stats_.match_ns, frame_match_ns_};
  struct TimedDump {
    std::chrono::steady_clock::time_point at;
    ~TimedDump() {
      if (MotionDump& dump = Dump(); dump.file)
        dump.frame_ns += uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - at).count());
    }
  } timed_dump{started};
  if (matched) *matched = nullptr;
  if (MotionDump& dump = Dump(); dump.on()) {
    const uint8_t tag = 1;
    std::fwrite(&tag, 1, 1, dump.file);
    std::fwrite(&key, 8, 1, dump.file);
    std::fwrite(&count, 4, 1, dump.file);
    std::fwrite(registers, 1, count, dump.file);
    std::fwrite(constants, 16, count, dump.file);
  }
  // The registers the view shares, for the next frame (a register two draws already disagreed on is not compared
  // again).
  ++view_draws_;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t r = registers[i];
    if (!view_users_[r]++) {
      std::memcpy(view_values_[r].data(), constants + 16 * i, 16);
    } else if (!view_mixed_.test(r) && std::memcmp(view_values_[r].data(), constants + 16 * i, 16)) {
      view_mixed_.set(r);
    }
  }

  // The constants the view does not share, in runs compared whole (D122: one comparison per register was most of
  // this function's time with GH Metallica's 256 registers a draw).
  runs_.clear();
  for (uint32_t i = 0; i < count; ++i) {
    if (shared_.test(registers[i])) continue;
    if (!runs_.empty() && runs_.back().second == i) {
      ++runs_.back().second;
    } else {
      runs_.push_back({i, i + 1});
    }
  }
  // D143: a hash of those constants (the same object, not moved, has the same) and their bounds.
  uint64_t hash;
  Bounds bounds;
  Summarize(constants, registers, count, hash, bounds);

  // This frame's, for the next.
  Frame& current = frames_[current_];
  uint32_t offset = uint32_t(current.data.size());
  current.data.insert(current.data.end(), constants, constants + 16 * size_t(count));
  const uint32_t registers_at = uint32_t(current.registers.size());
  current.registers.insert(current.registers.end(), registers, registers + count);
  current.entries.push_back({key, offset, registers_at, count, hash, bounds});

  // The last frame's.
  const Frame& previous = frames_[current_ ^ 1];
  auto it = std::lower_bound(previous.index.begin(), previous.index.end(), IndexItem{key, 0, 0, {}});
  if (it == previous.index.end() || it->key != key) return nullptr;
  auto end = it;
  while (end != previous.index.end() && end->key == key) ++end;
  ++stats_.matched;
  const uint32_t n = uint32_t(end - it);
  const bool only = n == 1;
  stats_.candidates += n;
  stats_.longest_key = std::max(stats_.longest_key, n);
  auto same_as = [&](const uint8_t* old) {
    for (const auto& [from, to] : runs_) {
      if (std::memcmp(old + 16 * size_t(from), constants + 16 * size_t(from), 16 * size_t(to - from))) return false;
    }
    return true;
  };
  // The last frame's hashes and bounds were made with the same shared registers (made again at the frame's end when
  // they changed): the one that did not move is found by its hash, beside the key, without reading the others'
  // constants.
  const bool hashed = previous.shared == shared_;
  if (hashed) {
    for (uint32_t k = 0; k < n; ++k) {
      if (it[k].hash != hash) continue;
      const Entry& entry = previous.entries[it[k].entry];
      if (entry.count != count) continue;
      const uint8_t* old = previous.data.data() + entry.offset;
      if (same_as(old)) {  // One of them did not move: it is this one.
        ++stats_.by_hash;
        if (matched) *matched = old;
        return nullptr;
      }
    }
  }
  // D143: the closest of the others. A mesh drawn in many places (Guadalcanal's palms, a hundred draws of one key, 256
  // constants each) was found by comparing each of them whole, up to 9 ms a frame there. Now their bounds are
  // compared first, beside the key: the one closest in its bounds is compared whole first, and its distance rules out
  // every other no closer in its bounds alone (the closest is the same, ties aside). A float sum's rounding is left
  // room: a bound counts only past the closest by a thousandth.
  const uint8_t* closest = nullptr;
  float closest_distance = INFINITY;
  auto consider = [&](const IndexItem& item) {
    const Entry& entry = previous.entries[item.entry];
    if (entry.count != count) return false;
    const uint8_t* old = previous.data.data() + entry.offset;
    // Without the hashes: one of them did not move, it is this one.
    if (!hashed && same_as(old)) {
      if (matched) *matched = old;
      return true;
    }
    // The only one of its key: the same object, however far (D122).
    if (only) {
      closest = old;
      return false;
    }
    float distance = 0.0f;
    for (const auto& [from, to] : runs_) {
      for (uint32_t i = from; i < to; ++i) {
        float a[4], b[4];
        std::memcpy(a, old + 16 * i, 16);
        std::memcpy(b, constants + 16 * i, 16);
        for (uint32_t c = 0; c < 4; ++c) {
          float d = std::abs(a[c] - b[c]);
          distance += d < 1e30f ? d : 1e30f;  // NaN and infinities count as far
        }
        if ((i & 7) == 7 && !(distance < closest_distance)) return false;
      }
    }
    if (distance < closest_distance) {
      closest_distance = distance;
      closest = old;
    }
    return false;
  };
  if (hashed && !only && bounds.valid) {
    parts_.resize(n);
    uint32_t first = 0;
    float first_part = INFINITY;
    for (uint32_t k = 0; k < n; ++k) {
      const Bounds& o = it[k].bounds;
      float part = -1.0f;  // compared whole
      if (o.valid && o.kept == bounds.kept) {
        double sums = 0.0;
        float samples = 0.0f;
        for (uint32_t i = 0; i < kGroups * 4; ++i) sums += std::abs(o.sums[i] - bounds.sums[i]);
        for (uint32_t i = 0; i < kSamples * 4; ++i) samples += std::abs(o.samples[i] - bounds.samples[i]);
        part = std::max(float(sums), samples);
        if (part < first_part) first_part = part, first = k;
      }
      parts_[k] = part;
    }
    // Past the frame's budget (a fast turn with a few hundred plant batches: every draw's constants change with the
    // camera, so their bounds rule out few), only the kLimited closest in their bounds are compared whole.
    if (frame_match_ns_ > kBudgetNs && n > kLimited) {
      order_.clear();
      for (uint32_t k = 0; k < n; ++k)
        if (parts_[k] >= 0.0f) order_.push_back({parts_[k], k});
      const size_t take = std::min<size_t>(kLimited, order_.size());
      std::partial_sort(order_.begin(), order_.begin() + take, order_.end());
      for (size_t j = 0; j < take; ++j) consider(it[order_[j].second]);
      ++stats_.limited;
      stats_.summarized += n - take;
    } else {
      consider(it[first]);
      for (uint32_t k = 0; k < n; ++k) {
        if (k == first) continue;
        if (parts_[k] >= 0.0f && parts_[k] > closest_distance * 1.001f + 1e-3f) {
          ++stats_.summarized;
          continue;
        }
        consider(it[k]);
      }
    }
  } else {
    if (!only) ++(hashed ? stats_.unbounded : stats_.unhashed);
    for (uint32_t k = 0; k < n; ++k) {
      if (consider(it[k])) return nullptr;
      if (only) break;
    }
  }
  if (closest) ++stats_.moved;
  if (matched) *matched = closest;
  return closest;
}

void ObjectMotion::Summarize(const uint8_t* constants, const uint8_t* registers, uint32_t count, uint64_t& hash,
                             Bounds& bounds) {
  scratch_.resize(16 * size_t(count));
  size_t kept = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (!shared_.test(registers[i])) std::memcpy(scratch_.data() + 16 * kept++, constants + 16 * size_t(i), 16);
  }
  scratch_.resize(16 * kept);
  hash = XXH3_64bits(scratch_.data(), scratch_.size());
  bounds = {};
  const uint32_t m = uint32_t(scratch_.size() / 16);
  auto clamped = [](float v) { return v == v ? std::clamp(v, -1e6f, 1e6f) : 0.0f; };
  for (uint32_t j = 0; j < m; ++j) {
    float v[4];
    std::memcpy(v, scratch_.data() + 16 * size_t(j), 16);
    double* g = bounds.sums + 4 * (uint64_t(j) * kGroups / m);
    for (uint32_t c = 0; c < 4; ++c) g[c] += clamped(v[c]);
  }
  bounds.valid = m > 0;
  bounds.kept = m;
  for (uint32_t j = 0; j < kSamples && m; ++j) {
    float v[4];
    std::memcpy(v, scratch_.data() + 16 * (uint64_t(j) * m / kSamples), 16);
    for (uint32_t c = 0; c < 4; ++c) bounds.samples[4 * j + c] = clamped(v[c]);
  }
}

bool ObjectMotion::ChangedOnlyIn(const uint8_t* constants, const uint8_t* previous, uint32_t from, uint32_t to) const {
  // The runs Match made for this draw, without [from, to).
  auto differ = [&](uint32_t a, uint32_t b) {
    return a < b && std::memcmp(constants + 16 * size_t(a), previous + 16 * size_t(a), 16 * size_t(b - a)) != 0;
  };
  for (const auto& [a, b] : runs_) {
    if (differ(a, std::min(b, from)) || differ(std::max(a, to), b)) return false;
  }
  return true;
}

void ObjectMotion::EndFrame() {
  frame_match_ns_ = 0;
  if (MotionDump& dump = Dump(); dump.file) {
    dump.slow = dump.frame_ns > 5000000 ? dump.slow + 1 : 0;
    if (dump.automatic && dump.first == UINT64_MAX && dump.slow >= 30) {
      dump.first = dump.frame + 1, dump.last = dump.first + dump.frames;
      REXLOG_INFO("[native_x_renderer] object motion: recording Match's inputs from frame {} (this one's matching took "
                  "{:.1f} ms)", dump.first, dump.frame_ns / 1e6);
    }
    dump.frame_ns = 0;
    if (dump.on()) {
      const uint8_t tag = 2;
      std::fwrite(&tag, 1, 1, dump.file);
    }
    if (++dump.frame == dump.last) std::fclose(dump.file), dump.file = nullptr;
  }
  frames_[current_].shared = shared_;  // the registers its hashes and bounds left out (D143)
  if (view_draws_) {
    std::bitset<256> shared;
    for (uint32_t r = 0; r < 256; ++r) {
      shared[r] = !view_mixed_[r] && view_users_[r] >= 2 && view_users_[r] * 2 >= view_draws_;
    }
    if (shared != shared_) {
      // Rarely: the camera's registers are the same from view to view.
      static uint32_t logged = 0;
      if (logged < 8) {
        ++logged;
        std::string list;
        for (uint32_t r = 0; r < 256; ++r) {
          if (shared[r]) list += " c" + std::to_string(r);
        }
        REXLOG_INFO("[native_x_renderer] object motion: the registers the main view shares:{}", list);
      }
      shared_ = shared;
    }
  }
  view_users_.fill(0);
  view_mixed_.reset();
  view_draws_ = 0;
  Frame& done = frames_[current_];
  // The shared registers changed: this frame's hashes and bounds made again without the new ones, so the next frame
  // finds its draws by them too (a frame compared whole took up to 15 ms on Guadalcanal).
  if (done.shared != shared_) {
    for (Entry& entry : done.entries) {
      Summarize(done.data.data() + entry.offset, done.registers.data() + entry.registers_at, entry.count, entry.hash,
                entry.bounds);
    }
    done.shared = shared_;
  }
  done.index.resize(done.entries.size());
  for (uint32_t e = 0; e < uint32_t(done.entries.size()); ++e) {
    const Entry& entry = done.entries[e];
    IndexItem& item = done.index[e];
    item.key = entry.key, item.entry = e, item.hash = entry.hash, item.bounds = entry.bounds;
  }
  std::sort(done.index.begin(), done.index.end());
  current_ ^= 1;
  frames_[current_].Clear();
}

}  // namespace native_x_renderer
