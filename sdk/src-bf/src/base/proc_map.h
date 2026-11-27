/**
 * @file        base/proc_map.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <string_view>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <rex/types.h>

namespace rex::memory {

struct ProcMapping {
  u64 start = 0;
  u64 end = 0;
  u64 offset = 0;
  std::array<char, 4> perms{};
  std::string_view path;
};

namespace detail {

inline std::string_view NextMapsField(std::string_view& line) {
  const size_t start = line.find_first_not_of(' ');
  if (start == std::string_view::npos) {
    line = {};
    return {};
  }
  line.remove_prefix(start);
  const size_t end = std::min(line.find(' '), line.size());
  const std::string_view field = line.substr(0, end);
  line.remove_prefix(end);
  return field;
}

inline u64 ParseMapsHex(std::string_view text) {
  u64 value = 0;
  std::from_chars(text.data(), text.data() + text.size(), value, 16);
  return value;
}

inline bool ParseMapsLine(std::string_view line, ProcMapping& out) {
  const std::string_view range = NextMapsField(line);
  const std::string_view perms = NextMapsField(line);
  const std::string_view offset = NextMapsField(line);
  NextMapsField(line);
  NextMapsField(line);
  const size_t dash = range.find('-');
  if (dash == std::string_view::npos || perms.size() < out.perms.size()) {
    return false;
  }
  out.start = ParseMapsHex(range.substr(0, dash));
  out.end = ParseMapsHex(range.substr(dash + 1));
  out.offset = ParseMapsHex(offset);
  std::copy_n(perms.begin(), out.perms.size(), out.perms.begin());
  const size_t path_start = line.find_first_not_of(' ');
  out.path = path_start == std::string_view::npos ? std::string_view{} : line.substr(path_start);
  return out.start < out.end;
}

}  // namespace detail

/**
 * Calls fn for each mapping of the process without allocating or locking. Paths longer than the
 * line buffer arrive cut short.
 *
 * @tparam F Callable taking a const ProcMapping& and returning true to continue.
 * @param fn Called per mapping, the path view is valid only during the call.
 * @return True when the map could be read.
 */
template <typename F>
bool ForEachMapping(F&& fn) {
  const int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  std::array<char, 1024> line{};
  size_t length = 0;
  std::array<char, 4096> chunk{};
  bool more = true;
  const auto deliver = [&]() {
    ProcMapping mapping;
    if (detail::ParseMapsLine({line.data(), length}, mapping)) {
      more = fn(static_cast<const ProcMapping&>(mapping));
    }
    length = 0;
  };
  for (ssize_t count; more && (count = read(fd, chunk.data(), chunk.size())) > 0;) {
    for (ssize_t i = 0; more && i < count; ++i) {
      if (chunk[i] == '\n') {
        deliver();
      } else if (length < line.size()) {
        line[length++] = chunk[i];
      }
    }
  }
  if (more && length) {
    deliver();
  }
  close(fd);
  return true;
}

// Battlefield 1943 Reborn patch (v0.10's pull request 28, carried to v0.11 on 27 Nov 2026): PROCMAP_QUERY (Linux
// 6.11+), one ioctl on /proc/self/maps that answers "which mapping holds this address" instead of reading and parsing
// the whole file, which for the guest's arena is thousands of lines at every look up (FindMapping: Protect's old
// access, QueryProtect, the write watches). The ioctl's number and struct are the kernel's fixed UAPI, written here:
// with <linux/fs.h>'s `#ifdef PROCMAP_QUERY` the fast path was compiled out of every release (built in an Ubuntu 22.04
// root, kernel headers 5.15). An older kernel answers ENOTTY: the first such answer switches to reading the file for
// the rest of the run. REX_PROCMAP_QUERY=0 forces the file (tests). Async signal safe (it runs in the write watch
// fault handler): a plain int descriptor per thread, no allocation, no lock.
namespace detail {
struct ProcmapQuery {
  u64 size, query_flags, query_addr, vma_start, vma_end, vma_flags, vma_page_size, vma_offset, inode;
  u32 dev_major, dev_minor, vma_name_size, build_id_size;
  u64 vma_name_addr, build_id_addr;
};
static_assert(sizeof(ProcmapQuery) == 104, "struct procmap_query is 104 bytes in Linux 6.11");
// _IOWR('f', 17, struct procmap_query)
inline constexpr unsigned long kProcmapQuery =
    (3ul << 30) | (sizeof(ProcmapQuery) << 16) | (static_cast<unsigned long>('f') << 8) | 17ul;
static_assert(kProcmapQuery == 0xC0686611ul, "PROCMAP_QUERY's ioctl number");
// 1: the ioctl is used; 0: not tried yet; -1: off (a kernel older than 6.11, or REX_PROCMAP_QUERY=0).
inline std::atomic<int> procmap_query_state{0};
inline thread_local int procmap_fd = -2;  // -2: not opened yet; -1: open failed

// 1: answered (out filled, its path empty); 0: no mapping holds the address; -1: not answered, read the file.
inline int ProcmapQueryLookup(u64 address, ProcMapping& out) {
  int state = procmap_query_state.load(std::memory_order_relaxed);
  if (state < 0) return -1;
  if (state == 0) {
    const char* off = std::getenv("REX_PROCMAP_QUERY");
    if (off && off[0] == '0') {
      procmap_query_state.store(-1, std::memory_order_relaxed);
      return -1;
    }
  }
  if (procmap_fd == -2) procmap_fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  if (procmap_fd < 0) return -1;
  ProcmapQuery query{};
  query.size = sizeof(query);
  query.query_addr = address;
  if (ioctl(procmap_fd, kProcmapQuery, &query) == 0) {
    if (state == 0) procmap_query_state.store(1, std::memory_order_relaxed);
    out = ProcMapping{};
    out.start = query.vma_start, out.end = query.vma_end, out.offset = query.vma_offset;
    out.perms = {char(query.vma_flags & 0x01 ? 'r' : '-'), char(query.vma_flags & 0x02 ? 'w' : '-'),
                 char(query.vma_flags & 0x04 ? 'x' : '-'), char(query.vma_flags & 0x08 ? 's' : 'p')};
    return 1;
  }
  const int error = errno;
  if (error == ENOENT) return 0;
  if (error == ENOTTY || error == EINVAL || error == EOPNOTSUPP) procmap_query_state.store(-1, std::memory_order_relaxed);
  return -1;
}
}  // namespace detail

/**
 * Finds the mapping that holds an address without allocating or locking.
 *
 * @param out The mapping, with an empty path.
 * @return True when a mapping holds the address.
 */
inline bool FindMapping(u64 address, ProcMapping& out) {
  if (const int answered = detail::ProcmapQueryLookup(address, out); answered >= 0) return answered == 1;
  bool found = false;
  ForEachMapping([&](const ProcMapping& mapping) {
    if (address >= mapping.start && address < mapping.end) {
      out = mapping;
      out.path = {};
      found = true;
    }
    return !found;
  });
  return found;
}

}  // namespace rex::memory
