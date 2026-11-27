// Whether guest memory is committed and readable in the game's heaps (the Readable checks of bots.cpp, team_balance.cpp,
// server_stats.cpp and reborn_achievements.cpp).
//
// 27 Nov 2026, the Linux server optimisation (US Central, 62 bots): those checks asked BaseHeap::QueryRegionInfo for
// each page, and QueryRegionInfo measures the whole region the page lies in by walking every page of it, thousands in
// the game's big heaps: 5% of the server's main thread. BaseHeap::QueryRangeCommittedReadable looks at the pages asked
// about only (SDK). The answer is the same: every page committed and readable.

#pragma once

#include <algorithm>
#include <cstdint>

#include <rex/system/xmemory.h>

namespace bf1943 {

inline bool GuestCommittedReadable(rex::memory::Memory* memory, uint32_t address, uint32_t size) {
  if (!memory || address + size < address) return false;
  if (!size) return true;
  const uint32_t last = address + (size - 1);
  for (uint32_t page = address;;) {
    rex::memory::BaseHeap* heap = memory->LookupHeap(page);
    if (!heap || !heap->heap_size()) return false;
    const uint32_t heap_last = heap->heap_base() + (heap->heap_size() - 1);
    if (heap_last < page) return false;
    const uint32_t end = std::min(last, heap_last);
    if (!heap->QueryRangeCommittedReadable(page, end)) return false;
    if (end >= last) return true;
    page = end + 1;
  }
}

}  // namespace bf1943
