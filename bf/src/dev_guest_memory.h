// Developer aids only: read guest memory from hooks and exception handlers without faulting on addresses that are
// not mapped (a register that holds a number, not a pointer, or the page after the top of a guest stack).

#pragma once

#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace bf1943::dev {

// True when [address, address + size) is committed, readable guest memory.
inline bool GuestReadable(const uint8_t* base, uint32_t address, uint32_t size) {
#if defined(_WIN32)
  // One byte of each page in the range copied through the kernel (ReadProcessMemory fails on memory that is not there
  // and raises nothing): about 0.35 us. VirtualQuery measures the whole region the address lies in, up to 100 us in
  // the game's large committed regions, and this runs for every player and marker every frame (the user, 20 Nov 2026:
  // 12 FPS at the start of a round on US Central's bot server, the game's thread 95% in VirtualQuery).
  const uintptr_t first = reinterpret_cast<uintptr_t>(base + address);
  const uintptr_t last = first + (size ? size - 1 : 0);
  for (uintptr_t p = first & ~uintptr_t(0xFFF); p <= last; p += 0x1000) {
    uint8_t byte;
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(p < first ? first : p), &byte, 1, &got) ||
        got != 1)
      return false;
  }
  return true;
#else
  // Linux: a read through process_vm_readv of our own process fails (EFAULT) on a page that is not mapped or not
  // readable instead of faulting, so it is safe here and in signal handlers. One byte of each page in the range.
  static const uintptr_t page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
  const uintptr_t first = reinterpret_cast<uintptr_t>(base + address);
  const uintptr_t last = first + (size ? size - 1 : 0);
  for (uintptr_t p = first & ~(page - 1); p <= last; p += page) {
    uint8_t byte;
    iovec local{&byte, 1};
    iovec remote{reinterpret_cast<void*>(p < first ? first : p), 1};
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != 1) return false;
  }
  return true;
#endif
}

// A big endian word, or 0 when it is not readable.
inline uint32_t SafeLoad32(const uint8_t* base, uint32_t address) {
  if (!GuestReadable(base, address, 4)) return 0;
  const uint8_t* p = base + address;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

}  // namespace bf1943::dev
