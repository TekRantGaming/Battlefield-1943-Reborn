// The game's own code where it crashes on our servers. The Xbox 360 game's dedicated server code never ran in
// production (DICE's servers were PC builds), so rare paths can still fault here.
//
// sub_8275B690(owner, filter, out): a query over the owner's list of object pointers (owner +32 to +36): for each
// object whose type (the word at +8) is in the filter's bit set, its method (vtable slot 2) adds it to `out`. Both
// callers pass an empty list of their own to fill. Official server 1 (Classic: All Maps 1, 7 players, Guadalcanal)
// crashed there on 11 Nov 2026 at 07:21 server time: an empty slot (0) in the owner's list, and the type read went to
// guest 0x8 ("Unhandled guest access violation: read of guest 0x00000008"; the crash report's frames are this function,
// then sub_82762EC8, sub_82763CB8, sub_82764298, ... in the server's update). The game's code below is the generated
// code exactly (bf/generated/default, bf1943_recomp.144.cpp), with one change: an empty slot adds nothing and is
// passed over, as a removed object would be.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "bf1943_pch.h"

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // ReadProcessMemory (GuestLive)
#else
#include <sys/uio.h>  // process_vm_readv (GuestLive)
#include <unistd.h>
#endif

REX_EXTERN(__savegprlr_27);
REX_EXTERN(__restgprlr_27);

namespace bf1943 {
bool StrayReadPage(uint32_t guest_page);  // crash_report.cpp: a page the stray read guard gave
}

namespace {
std::atomic<uint32_t> g_empty_slots{0};
}  // namespace

extern "C" REX_FUNC(sub_8275B690) {
  REX_FUNC_PROLOGUE();
  uint32_t ea{};
  // mflr r12
  ctx.r12.u64 = ctx.lr;
  // bl 0x827be834
  ctx.lr = 0x8275B698;
  __savegprlr_27(ctx, base);
  // stwu r1,-128(r1)
  ea = -128 + ctx.r1.u32;
  REX_STORE_U32(ea, ctx.r1.u32);
  ctx.r1.u32 = ea;
  // lwz r11,0(r4)
  ctx.r11.u64 = REX_LOAD_U32(ctx.r4.u32 + 0);
  // mr r30,r4
  ctx.r30.u64 = ctx.r4.u64;
  // lwz r27,36(r3)
  ctx.r27.u64 = REX_LOAD_U32(ctx.r3.u32 + 36);
  // mr r29,r5
  ctx.r29.u64 = ctx.r5.u64;
  // lwz r31,32(r3)
  ctx.r31.u64 = REX_LOAD_U32(ctx.r3.u32 + 32);
  // cmplw cr6,r31,r27
  ctx.cr6.compare<uint32_t>(ctx.r31.u32, ctx.r27.u32, ctx.xer);
  // lwz r10,0(r11)
  ctx.r10.u64 = REX_LOAD_U32(ctx.r11.u32 + 0);
  // clrlwi r28,r10,31
  ctx.r28.u64 = ctx.r10.u32 & 0x1;
  // beq cr6,0x8275b724
  if (ctx.cr6.eq) goto loc_8275B724;
loc_8275B6C0:
  // lwz r3,0(r31)
  ctx.r3.u64 = REX_LOAD_U32(ctx.r31.u32 + 0);
  // Ours: an empty slot is passed over (the game read its type at guest 0x8 and the server crashed).
  if (ctx.r3.u32 == 0) [[unlikely]] {
    const uint32_t n = g_empty_slots.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 5 || n % 1000 == 0)
      REXLOG_WARN("BF game fix: an empty slot in an object list (at {:08X}) passed over ({} so far); the game "
                  "would have crashed there",
                  ctx.r31.u32, n);
    goto loc_8275B718;
  }
  // lwz r11,0(r30)
  ctx.r11.u64 = REX_LOAD_U32(ctx.r30.u32 + 0);
  // lwz r10,8(r3)
  ctx.r10.u64 = REX_LOAD_U32(ctx.r3.u32 + 8);
  // rlwinm r9,r10,16,16,31
  ctx.r9.u64 = __builtin_rotateleft64(ctx.r10.u32 | (ctx.r10.u64 << 32), 16) & 0xFFFF;
  // rlwinm r8,r10,29,19,29
  ctx.r8.u64 = __builtin_rotateleft64(ctx.r10.u32 | (ctx.r10.u64 << 32), 29) & 0x1FFC;
  // neg r7,r9
  ctx.r7.s64 = static_cast<int64_t>(-ctx.r9.u64);
  // neg r6,r9
  ctx.r6.s64 = static_cast<int64_t>(-ctx.r9.u64);
  // clrlwi r5,r10,27
  ctx.r5.u64 = ctx.r10.u32 & 0x1F;
  // srawi r4,r7,31
  ctx.xer.ca = (ctx.r7.s32 < 0) & ((ctx.r7.u32 & 0x7FFFFFFF) != 0);
  ctx.r4.s64 = ctx.r7.s32 >> 31;
  // lwzx r11,r8,r11
  ctx.r11.u64 = REX_LOAD_U32(ctx.r8.u32 + ctx.r11.u32);
  // clrlwi r10,r6,27
  ctx.r10.u64 = ctx.r6.u32 & 0x1F;
  // srw r9,r11,r5
  ctx.r9.u64 = ctx.r5.u8 & 0x20 ? 0 : (ctx.r11.u32 >> (ctx.r5.u8 & 0x3F));
  // srw r8,r4,r10
  ctx.r8.u64 = ctx.r10.u8 & 0x20 ? 0 : (ctx.r4.u32 >> (ctx.r10.u8 & 0x3F));
  // and r4,r9,r8
  ctx.r4.u64 = ctx.r9.u64 & ctx.r8.u64;
  // cmplwi cr6,r4,0
  ctx.cr6.compare<uint32_t>(ctx.r4.u32, 0, ctx.xer);
  // beq cr6,0x8275b718
  if (ctx.cr6.eq) goto loc_8275B718;
  // lwz r11,0(r3)
  ctx.r11.u64 = REX_LOAD_U32(ctx.r3.u32 + 0);
  // mr r6,r28
  ctx.r6.u64 = ctx.r28.u64;
  // mr r5,r29
  ctx.r5.u64 = ctx.r29.u64;
  // lwz r10,8(r11)
  ctx.r10.u64 = REX_LOAD_U32(ctx.r11.u32 + 8);
  // mtctr r10
  ctx.ctr.u64 = ctx.r10.u64;
  // bctrl
  ctx.lr = 0x8275B718;
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
loc_8275B718:
  // addi r31,r31,4
  ctx.r31.s64 = ctx.r31.s64 + 4;
  // cmplw cr6,r31,r27
  ctx.cr6.compare<uint32_t>(ctx.r31.u32, ctx.r27.u32, ctx.xer);
  // bne cr6,0x8275b6c0
  if (!ctx.cr6.eq) goto loc_8275B6C0;
loc_8275B724:
  // addi r1,r1,128
  ctx.r1.s64 = ctx.r1.s64 + 128;
  // b 0x827be884
  __restgprlr_27(ctx, base);
  return;
}

// sub_821DBC68(r3, buckets, count): empties a hash table's buckets: for each bucket, every node of its chain (next at
// +36) freed (sub_82624D90), with the node's own buffer, then the bucket set to 0. sub_821E08D0(table) clears a table
// that way (buckets at +36, count at +40) and then frees the bucket array when there is more than one. Official servers
// crashed there four times, 13 to 15 Nov 2026 (0.4.0 server 6, 0.4.1 servers 10 and 13, then 0.4.6 All Maps 4 mid
// match on Iwo Jima, every player dropped): the table's bucket pointer was 1 with buckets counted, so the clear read
// guest 0x1 (the null page guard gave zeros) and then wrote there ("Unhandled guest access violation: write of guest
// 0x00000001", frames sub_821DBC68, sub_821E08D0, sub_823F0F30, sub_82410950, ... in the server's tick). What left the
// table that way is not known. A bucket pointer in the guest's first 64 KB (never a heap address) is a table with
// nothing to clear: sub_821DBC68 returns at once, and sub_821E08D0 leaves the table alone. Logged, the first 10 times.
//
// Until 0.4.8 sub_821E08D0's guard emptied such a table (+36 and +40 set to 0) and let the game's clear run. That clear
// also frees the table's own buffer at +8 when +16 minus +8 is more than 1, and every table it caught had been freed
// already: the caller (sub_823F0F30) then freed the table itself a second time (the small heap guard's line follows),
// or the memory belonged to another object by then (a count of C2C80000, the float -100.0). All Maps 3 at 14:38 UTC on
// 18 Nov 2026 and All Maps 1 at 00:20 UTC on 20 Nov (0.4.7.4) crashed that way: +8 held 00000400 and 00800000, the
// big heap's free (sub_82624D90, sub_82624850, sub_826230C8) wrote 4 bytes below it, guest 3FC and 7FFFFC. So a bad
// table is now passed over whole: none of the clear runs, nothing is freed, and nothing is written into memory that
// may be another object's.
namespace {
std::atomic<uint32_t> g_bad_tables{0};
bool BadBuckets(uint32_t buckets, uint32_t count) { return count != 0 && buckets < 0x10000; }
void NoteBadTable(const char* where, uint32_t table, uint32_t buckets, uint32_t count) {
  const uint32_t n = ++g_bad_tables;
  if (n <= 10)
    REXLOG_WARN("BF game fix: a hash table with its bucket pointer at {:08X} and {} buckets in {} (table {:08X}): "
                "its clear passed over instead of a crash (time {})",
                buckets, count, where, table, n);
}
}  // namespace

REX_EXTERN(__imp__sub_821DBC68);
REX_HOOK_RAW(sub_821DBC68) {
  if (BadBuckets(ctx.r4.u32, ctx.r5.u32)) {
    NoteBadTable("sub_821DBC68", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    return;
  }
  __imp__sub_821DBC68(ctx, base);
}

REX_EXTERN(__imp__sub_821E08D0);
REX_HOOK_RAW(sub_821E08D0) {
  const uint32_t table = ctx.r3.u32;
  if (table >= 0x10000) {
    const uint32_t buckets = REX_LOAD_U32(table + 36), count = REX_LOAD_U32(table + 40);
    if (BadBuckets(buckets, count)) {
      NoteBadTable("sub_821E08D0", table, buckets, count);
      return;  // the table is gone: none of the clear runs (see above)
    }
  }
  __imp__sub_821E08D0(ctx, base);
}

// sub_821DC048(object, key, f1 value) and sub_821DC178 (the same shape): set a value in a table inside an object (at
// +32), as the game does for its players and the like each tick. All Maps 3 (0.4.6.5) crashed at 19:12 UTC 15 Nov
// 2026 and All Maps 1 on 14 Nov reading such a table in sub_821D8160 (its lookup) from these two, in the server's tick
// (sub_824123F0): the object's memory had been released (a read of guest 1155ECE8, a page no longer committed), a
// minute after players left. An object whose memory is not live (not committed, readable and writable) is passed
// over: the setter returns 0 as it does for a locked object (+68), and nothing is written. Logged, the first 10 times.
namespace {
std::atomic<uint32_t> g_dead_objects{0};
// Whether the host memory under a guest address can be read: a one byte copy through the kernel, which fails on
// memory that is not there and raises nothing (ReadProcessMemory 0.35 us a call here, process_vm_readv 1.2 us in WSL;
// VirtualQuery was 100 us inside a large committed region, and a guarded read faults through the SDK's handler, which
// logs a line each time).
bool HostReads(const uint8_t* at) {
  char byte;
#if defined(_WIN32)
  SIZE_T got = 0;
  return ReadProcessMemory(GetCurrentProcess(), at, &byte, 1, &got) && got == 1;
#else
  iovec local{&byte, 1}, remote{const_cast<uint8_t*>(at), 1};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == 1;
#endif
}
bool GuestLive(uint32_t address, uint32_t size) {
  if (address < 0x10000) return false;
  auto* state = rex::system::kernel_state();
  rex::memory::Memory* memory = state ? state->memory() : nullptr;
  if (!memory) return true;
  for (uint32_t page = address & ~0xFFFu; page < address + size; page += 0x1000) {
    rex::memory::BaseHeap* heap = memory->LookupHeap(page);
    uint32_t protect = 0;
    if (!heap || !heap->QueryProtect(page, &protect)) return false;
    if (!(protect & rex::memory::kMemoryProtectRead) || !(protect & rex::memory::kMemoryProtectWrite)) return false;
  }
  // The guest's page table gives a page its protection when it is reserved, before it is committed (and keeps it
  // when it is decommitted), so only the host can tell whether the memory is there (16 Nov 2026 16:50 server time,
  // All Maps 3 on 0.4.7.1: a scene node's entries at guest 10000000, reserved and never committed, passed the
  // check above and the update read guest 10000018).
  uint8_t* host = memory->virtual_membase();
  if (!host) return true;
#if defined(BF_DEV_TOOLS)
  static const bool host_check_off = [] {  // BF_DEV_HOST_CHECK_OFF=1: the self test without this check
    const char* v = std::getenv("BF_DEV_HOST_CHECK_OFF");
    return v && *v == '1';
  }();
  if (host_check_off) return true;
#endif
  // Each page's answer is kept for 5 ms on this thread (a system call on every check would cost the tick), a window
  // far shorter than any release the guards are for.
  struct Seen {
    uint32_t page = 0;
    bool live = false;
    int64_t at = 0;
  };
  thread_local Seen seen[64];
  const int64_t now =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  for (uint32_t page = address & ~0xFFFu; page < address + size; page += 0x1000) {
    Seen& s = seen[(page >> 12) & 63];
    // A page the stray read guard gave (read only zeros, crash_report.cpp) reads, but nothing lives there.
    if (s.page != page || now - s.at > 5000)
      s.page = page, s.at = now, s.live = HostReads(host + page) && !bf1943::StrayReadPage(page);
    if (!s.live) return false;
  }
  return true;
}
}  // namespace

// Who freed it (17 Nov 2026): the crashes of 0.4.7.1 are objects freed while the game still lists them (vehicle parts
// in their trees, objects in the lists sub_8275B690 queries), and the guards only see them later. On a dedicated
// server every free through operator delete (sub_82624D90, both heaps) is noted in a fixed table (65536 slots by the
// block's address, 1.25 MB, the newest free of a slot wins): the block, when, and the guest's return addresses at the
// free. When a guard meets a dead object it looks the address up (the block it lies in, up to 512 bytes back) and its
// log line names the free. A few stores a free; nothing on clients.
REXCVAR_DECLARE(bool, server);
namespace {
struct FreeNote {
  std::atomic<uint32_t> block{0}, ms{0};
  std::atomic<uint32_t> callers[3]{};
};
FreeNote g_free_notes[1u << 16];
uint32_t NowMs32() {
  static const auto start = std::chrono::steady_clock::now();
  return uint32_t(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}
uint32_t FreeSlot(uint32_t block) { return (block >> 4) * 2654435761u >> 16; }

void NoteFree(const PPCContext& ctx, uint8_t* base, uint32_t block) {
  FreeNote& note = g_free_notes[FreeSlot(block)];
  note.block.store(block, std::memory_order_relaxed);
  note.ms.store(NowMs32(), std::memory_order_relaxed);
  note.callers[0].store(uint32_t(ctx.lr), std::memory_order_relaxed);
  uint32_t sp = ctx.r1.u32;
  for (int i = 1; i < 3; ++i) {
    uint32_t at = 0;
    if (sp >= 0x70000000u && sp < 0x80000000u) {
      const uint32_t back = REX_LOAD_U32(sp);
      if (back > sp && back < 0x80000000u) at = REX_LOAD_U32(back - 8), sp = back;
      else sp = 0;
    }
    note.callers[i].store(at, std::memory_order_relaxed);
  }
}

// ", freed 12.3 s ago (block 1F680740) from 822DE8A4 822DE7F0 822D8510" or "" when no free of it is noted.
std::string FreedBy(uint32_t address) {
  if (address < 0x10000) return "";
  for (uint32_t back = 0; back <= 512 && back <= address; back += 4) {
    const uint32_t block = address - back;
    const FreeNote& note = g_free_notes[FreeSlot(block)];
    if (note.block.load(std::memory_order_relaxed) != block) continue;
    const uint32_t ago = NowMs32() - note.ms.load(std::memory_order_relaxed);
    return fmt::format(", freed {:.1f} s ago (block {:08X}) from {:08X} {:08X} {:08X}", ago / 1000.0, block,
                       note.callers[0].load(), note.callers[1].load(), note.callers[2].load());
  }
  return "";
}

void NoteDeadObject(const char* where, uint32_t object, uint32_t caller) {
  const uint32_t n = ++g_dead_objects;
  if (n <= 10)
    REXLOG_WARN("BF game fix: {} on an object whose memory is released ({:08X}, called from {:08X}{}): passed over "
                "instead of a crash (time {})",
                where, object, caller, FreedBy(object), n);
}
}  // namespace

REX_EXTERN(__imp__sub_82624D90);
REX_HOOK_RAW(sub_82624D90) {
  static const bool server = REXCVAR_GET(server);
  if (server && ctx.r3.u32 >= 0x10000) NoteFree(ctx, base, ctx.r3.u32);
  __imp__sub_82624D90(ctx, base);
}

// A live object whose table has no bucket array (16 Nov 2026, the night of the 0.4.7 release: All Maps 1, 0.4.7, and
// earlier that day on 0.4.6.5, "read of guest 0x00010000" in sub_823C8BD8 from sub_821DC178, in the server's tick
// sub_824065D0, every player dropped): its bucket pointer (+36) was 0, so the lookup read the null page (the null page
// guard gave zeros), found nothing, and the insert's begin (sub_823C8BD8, which walks the buckets to the first one in
// use with no end of its own) walked the whole null page into 0x10000. A bucket pointer in the guest's first 64 KB is
// never an array: the setter passes the object over as for a released one.
namespace {
std::atomic<uint32_t> g_no_buckets{0};
bool NoBuckets(uint8_t* base, uint32_t object, const char* where, uint32_t caller) {
  const uint32_t buckets = REX_LOAD_U32(object + 36);
  if (buckets >= 0x10000) return false;
  const uint32_t n = ++g_no_buckets;
  if (n <= 10)
    REXLOG_WARN("BF game fix: {} on an object whose table has no buckets ({:08X}, bucket pointer {:08X}, {} counted, "
                "called from {:08X}): passed over instead of a crash (time {})",
                where, object, buckets, REX_LOAD_U32(object + 40), caller, n);
  return true;
}
}  // namespace

REX_EXTERN(__imp__sub_821DC048);
REX_HOOK_RAW(sub_821DC048) {
  if (!GuestLive(ctx.r3.u32, 72)) {
    NoteDeadObject("sub_821DC048", ctx.r3.u32, uint32_t(ctx.lr));
    ctx.r3.u64 = 0;
    return;
  }
  if (NoBuckets(base, ctx.r3.u32, "sub_821DC048", uint32_t(ctx.lr))) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_821DC048(ctx, base);
}

REX_EXTERN(__imp__sub_821DC178);
REX_HOOK_RAW(sub_821DC178) {
  if (!GuestLive(ctx.r3.u32, 72)) {
    NoteDeadObject("sub_821DC178", ctx.r3.u32, uint32_t(ctx.lr));
    ctx.r3.u64 = 0;
    return;
  }
  if (NoBuckets(base, ctx.r3.u32, "sub_821DC178", uint32_t(ctx.lr))) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_821DC178(ctx, base);
}

// sub_822AE650(node, flag): a scene node's update, recursing into its children. The node keeps an array of 16 byte
// entries (+44) and its index in it (+48 byte): the update reads and writes entry index+1 (+8, its flags), reads
// entry index (+30) and the children's pointers from entry index+2 on. Official servers crashed there 7 times, 13 to
// 16 Nov 2026 (0.4.0 to 0.4.7; All Maps 3 at 22:06 UTC on the 0.4.7 release night, every player dropped): the entry
// array was 0 (writes and reads of guest 0x18, 0x1B8, 0x218), 0x10000, or a released page, on a node still linked
// (its own type pointer was fine), often a child several levels down. A node whose entry array is not live memory is
// left as it is, and its children with it: the update returns at once. Logged, the first 10 times.
namespace {
std::atomic<uint32_t> g_bad_nodes{0};
uint32_t BadPartUnder(uint8_t* base, uint32_t node);  // below, with the part tree walk
}  // namespace

REX_EXTERN(__imp__sub_822AE650);
REX_HOOK_RAW(sub_822AE650) {
  const uint32_t node = ctx.r3.u32;
  if (node >= 0x10000) {
    const uint32_t entries = REX_LOAD_U32(node + 44);
    const uint32_t index = REX_LOAD_U8(node + 48);
    if (entries < 0x10000 || !GuestLive(entries + index * 16, 48)) {
      const uint32_t n = ++g_bad_nodes;
      if (n <= 10)
        REXLOG_WARN("BF game fix: a scene node whose entries are gone ({:08X}, entries {:08X}, index {}, called from "
                    "{:08X}{}): its update passed over instead of a crash (time {})",
                    node, entries, index, uint32_t(ctx.lr), FreedBy(entries), n);
      return;
    }
    // 17 Nov 2026: the children too (BadPartUnder, below): after each recursive call the loop reads the child's own
    // +44 and +48, and the guard above only looked at the node's entries.
    if (const uint32_t bad = BadPartUnder(base, node)) {
      const uint32_t n = ++g_bad_nodes;
      if (n <= 10)
        REXLOG_WARN("BF game fix: a scene node with a child that is gone ({:08X}, child {:08X}, called from {:08X}{}): "
                    "its update passed over instead of a crash (time {})",
                    node, bad, uint32_t(ctx.lr), FreedBy(bad), n);
      return;
    }
  }
  __imp__sub_822AE650(ctx, base);
}

// sub_822A8F80(node): a walk over a tree of vehicle parts kept the same way as sub_822AE650's nodes (an array of 16
// byte entries at +44 shared by the tree, the node's index in it at +48): it calls the node's method (vtable slot 42),
// then itself on each child (the first from entry index+2 when entry index's +30 is not 0, the next found from the
// child's own entries), and clears a flag (+49). All Maps 1 (0.4.6.5) crashed there at 10:07 server time on 16 Nov
// 2026, three levels down from sub_822F87A0 in the server's tick: a child pointer read 6C65745F ("let_", text), so the
// memory of a listed child had been released and used again; the US Central server's abort of 14 Nov was the same
// walk on a released part. Before a node is walked, the node and each child the walk would visit are checked with the
// walk's own steps (reads only): live memory (committed, readable and writable), a vtable in the game's read only data
// (0x82000000 up to its code, which starts at 0x82160000) and live entries. A node that fails is not walked (its
// method, its children and its flag are left as they are).
// Logged, the first 10 times.
namespace {
std::atomic<uint32_t> g_bad_parts{0};
bool PartNodeLive(uint8_t* base, uint32_t node) {
  if (node < 0x10000 || (node & 3) || !GuestLive(node, 52)) return false;
  const uint32_t vtable = REX_LOAD_U32(node);
  if (vtable < 0x82000000u || vtable >= 0x82160000u || (vtable & 3)) return false;
  const uint32_t entries = REX_LOAD_U32(node + 44), index = REX_LOAD_U8(node + 48);
  return entries >= 0x10000 && GuestLive(entries + index * 16, 48);
}
// 0: the node and its children are fine; otherwise the first bad one.
uint32_t BadPartUnder(uint8_t* base, uint32_t node) {
  if (!PartNodeLive(base, node)) return node ? node : 1;
  const uint32_t entries = REX_LOAD_U32(node + 44), index = REX_LOAD_U8(node + 48);
  if (REX_LOAD_U8(entries + index * 16 + 30) == 0) return 0;
  uint32_t child = REX_LOAD_U32(entries + (index + 2) * 16);
  for (int n = 0; child != 0 && n < 256; ++n) {
    if (!PartNodeLive(base, child)) return child;
    const uint32_t at = REX_LOAD_U8(child + 48), list = REX_LOAD_U32(child + 44);
    const uint32_t entry = list + (at + 1) * 16;
    const uint32_t step = REX_LOAD_U8(entry + 13);
    uint32_t last;
    if (step == 255) {
      if (!GuestLive(list, 16)) return child;
      last = (REX_LOAD_U8(list + 13) + 255) & 0xFF;
    } else {
      if (!GuestLive(list + step * 16 + 30, 1)) return child;
      last = (REX_LOAD_U8(list + step * 16 + 30) + step) & 0xFF;
    }
    const uint32_t next = (REX_LOAD_U8(entry + 14) + at + 1) & 0xFF;
    if (next > last) return 0;
    if (!GuestLive(list + (next + 1) * 16, 4)) return child;
    child = REX_LOAD_U32(list + (next + 1) * 16);
  }
  return 0;
}
}  // namespace

REX_EXTERN(__imp__sub_822A8F80);
REX_HOOK_RAW(sub_822A8F80) {
  if (const uint32_t bad = BadPartUnder(base, ctx.r3.u32)) [[unlikely]] {
    const uint32_t n = ++g_bad_parts;
    if (n <= 10)
      REXLOG_WARN("BF game fix: a vehicle part tree with a part that is gone (node {:08X}, bad part {:08X}, called from "
                  "{:08X}{}): its walk passed over instead of a crash (time {})",
                  ctx.r3.u32, bad, uint32_t(ctx.lr), FreedBy(bad), n);
    return;
  }
  __imp__sub_822A8F80(ctx, base);
}

// The other walks over the same trees (17 Nov 2026): All Maps 1 (0.4.7.1) crashed at 21:29 server time in
// sub_822AE918, from sub_822AEEA8 beside the guarded scene node update, writing guest F8: the node's entries were 0.
// sub_822AE918, sub_822AEBE0 (the same function for vtable slot 46), sub_822B0580 and sub_822DDBE8 read +44 and +48 and
// visit the children exactly as sub_822A8F80 does (found by their code: entry +13 and +14, the first child at index+2,
// the recursion), and none returns a value its callers read. Each is checked the same way before it runs.
namespace {
std::atomic<uint32_t> g_bad_trees{0};
bool BadTreeWalk(PPCContext& ctx, uint8_t* base, const char* walk) {
  const uint32_t bad = BadPartUnder(base, ctx.r3.u32);
  if (!bad) [[likely]] return false;
  const uint32_t n = ++g_bad_trees;
  if (n <= 10)
    REXLOG_WARN("BF game fix: {} on a tree with a node that is gone (node {:08X}, bad node {:08X}, called from "
                "{:08X}{}): its walk passed over instead of a crash (time {})",
                walk, ctx.r3.u32, bad, uint32_t(ctx.lr), FreedBy(bad), n);
  return true;
}
}  // namespace

#define BF_GUARD_TREE_WALK(fn)            REX_EXTERN(__imp__##fn);                REX_HOOK_RAW(fn) {                        if (BadTreeWalk(ctx, base, #fn)) return;     __imp__##fn(ctx, base);               }
BF_GUARD_TREE_WALK(sub_822AE918)
BF_GUARD_TREE_WALK(sub_822AEBE0)
BF_GUARD_TREE_WALK(sub_822B0580)
BF_GUARD_TREE_WALK(sub_822DDBE8)
#undef BF_GUARD_TREE_WALK

// The game's small block heap (blocks of 256 bytes or less; heap object 0x82CA7790): sub_826248C0(heap, size, align)
// hands out the head of the calling thread's free list for the size (the thread's cache, sub_82623970, from its TLS
// slot 28: the region the small blocks live in at +0 and its length at +4, the lists' heads at +8, their counts at
// +84) and sets the head to the block's first word; sub_826249F8(heap, block) pushes a block back (its first word the
// old head) and returns its size, or 0 for a block outside the region, which the big heap then frees (sub_82624D90).
//
// All Maps 4 and All Maps 3 (0.4.6.5) crashed at 14:50 and 18:00 server time on 15 Nov 2026 in sub_823C3858, from
// sub_8274B090 in the server's tick: operator new (sub_82627D10) had given it guest 2 and guest 8200F908, and its first
// store there faulted. 8200F908 is the vtable sub_822DD9B8's constructor stores at an object's +0: an object was built
// in a block that was still on a free list, so the block's link became that vtable and the list's next block was the
// vtable itself. A block that is freed twice does that (the same block twice on the list: handed out twice, and the
// second owner's constructor writes the link). Two guards, each logged:
//  - a free of a block that is free already (a bit per 4 bytes of guest memory, set by the free and cleared when the
//    block is handed out again) is passed over: the block stays on its list once, and the free reports it taken;
//  - a block handed out from outside the small block region (a list already broken some other way) is not given to
//    the game: that size's list is dropped (its blocks are lost, a few KB at most) and the big heap serves the request.
namespace {
constexpr uint32_t kFreeLeafShift = 20;                             // 1 MB of guest addresses per leaf
constexpr uint32_t kFreeLeafWords = (1u << kFreeLeafShift) / 4 / 64;  // a bit per 4 bytes: 4096 words, 32 KB
std::atomic<std::atomic<uint64_t>*> g_free_leaves[1u << (32 - kFreeLeafShift)];
std::atomic<uint32_t> g_double_frees{0}, g_bad_lists{0};

std::atomic<uint64_t>* FreeLeaf(uint32_t block, bool make) {
  std::atomic<std::atomic<uint64_t>*>& slot = g_free_leaves[block >> kFreeLeafShift];
  std::atomic<uint64_t>* leaf = slot.load(std::memory_order_acquire);
  if (leaf || !make) return leaf;
  auto* fresh = new std::atomic<uint64_t>[kFreeLeafWords]();
  if (slot.compare_exchange_strong(leaf, fresh, std::memory_order_acq_rel)) return fresh;
  delete[] fresh;
  return leaf;
}

// The calling thread's small block cache (0 when it has none yet) and the region of the small blocks.
uint32_t SmallCache(const PPCContext& ctx, uint8_t* base) {
  const uint32_t tls = REX_LOAD_U32(ctx.r13.u32 + 0);
  return tls >= 0x10000 ? REX_LOAD_U32(tls + 28) : 0;
}
bool InSmallRegion(uint8_t* base, uint32_t cache, uint32_t block) {
  const uint32_t from = REX_LOAD_U32(cache + 0), length = REX_LOAD_U32(cache + 4);
  return block >= from && block - from < length;
}

// The guest's return addresses above the hook's caller (each frame's back chain at +0, its saved lr 8 below the next).
std::string GuestCallers(const PPCContext& ctx, uint8_t* base) {
  std::string s = fmt::format("{:08X}", uint32_t(ctx.lr));
  uint32_t sp = ctx.r1.u32;
  for (int i = 0; i < 8 && sp >= 0x70000000u && sp < 0x80000000u; ++i) {
    const uint32_t back = REX_LOAD_U32(sp);
    if (back <= sp || back >= 0x80000000u) break;
    s += fmt::format(" {:08X}", REX_LOAD_U32(back - 8));
    sp = back;
  }
  return s;
}

// BF_DEV_HEAP_GUARD_OFF=1 (dev builds): the two small heap guards do nothing, so BF_DEV_FIX_TEST shows what the game
// does without them.
bool HeapGuardOff() {
#if defined(BF_DEV_TOOLS)
  static const bool off = [] {
    const char* v = std::getenv("BF_DEV_HEAP_GUARD_OFF");
    return v && *v == '1';
  }();
  return off;
#else
  return false;
#endif
}
}  // namespace

REX_EXTERN(sub_82627D10);  // operator new (BF_DEV_FIX_TEST)
REX_EXTERN(sub_82624D90);  // operator delete (BF_DEV_FIX_TEST)

REX_EXTERN(__imp__sub_826249F8);
REX_HOOK_RAW(sub_826249F8) {
  const uint32_t block = ctx.r4.u32;
  const uint32_t cache = SmallCache(ctx, base);
  // Not one of the small blocks: the game's own answer (0) sends it to the big heap.
  if (block < 0x10000 || (block & 3) || (cache && !InSmallRegion(base, cache, block)) || HeapGuardOff()) {
    __imp__sub_826249F8(ctx, base);
    return;
  }
  static std::atomic<bool> said{false};
  if (cache && !said.load(std::memory_order_relaxed) && !said.exchange(true, std::memory_order_relaxed))
    REXLOG_INFO("BF game fix: the small heap's blocks are at {:08X} to {:08X}; a block freed twice is passed over",
                REX_LOAD_U32(cache + 0), REX_LOAD_U32(cache + 0) + REX_LOAD_U32(cache + 4));
  std::atomic<uint64_t>* leaf = FreeLeaf(block, true);
  const uint32_t bit = (block & ((1u << kFreeLeafShift) - 1)) >> 2;
  const uint64_t mask = uint64_t(1) << (bit & 63);
  if (leaf[bit >> 6].fetch_or(mask, std::memory_order_acq_rel) & mask) {
    const uint32_t n = ++g_double_frees;
    if (n <= 20 || n % 1000 == 0)
      REXLOG_WARN("BF game fix: a small heap block freed twice ({:08X}, from {}): the second free passed over instead "
                  "of a broken free list (time {})",
                  block, GuestCallers(ctx, base), n);
    ctx.r3.u64 = 1;  // taken (the big heap's free must not see it either)
    return;
  }
  __imp__sub_826249F8(ctx, base);
  if (ctx.r3.u32 == 0) leaf[bit >> 6].fetch_and(~mask, std::memory_order_acq_rel);  // the game did not take it
}

REX_EXTERN(__imp__sub_826248C0);
REX_HOOK_RAW(sub_826248C0) {
  const uint32_t size = ctx.r4.u32, align = ctx.r5.u32;
  __imp__sub_826248C0(ctx, base);
  const uint32_t block = ctx.r3.u32;
  if (block == 0 || HeapGuardOff()) return;
  const uint32_t cache = SmallCache(ctx, base);
  if (cache && ((block & 3) || !InSmallRegion(base, cache, block))) {
    // The list's head was not a block. The game's own size class (sub_826248C0): the size rounded up to the alignment,
    // in steps of 4 bytes up to 12 and of 16 bytes above. The list (head +8, count +84) is emptied: the next blocks
    // of that size come from the global pool again.
    const uint32_t rounded = (size + align - 1) & ~(align - 1);
    const uint32_t index = rounded < 13 ? (rounded - 1) >> 2 : ((rounded - 1) >> 4) + 3;
    if (rounded >= 1 && index < 19) {
      REX_STORE_U32(cache + 8 + index * 4, 0);
      REX_STORE_U32(cache + 84 + index * 4, 0);
    }
    const uint32_t n = ++g_bad_lists;
    if (n <= 20 || n % 1000 == 0)
      REXLOG_WARN("BF game fix: the small heap's list for {} byte blocks handed out {:08X}, not a block (called from "
                  "{}): the list dropped and the big heap asked instead of a crash (time {})",
                  rounded, block, GuestCallers(ctx, base), n);
    ctx.r3.u64 = 0;  // sub_82627C40 then asks the big heap (sub_826274E8)
    return;
  }
  if (std::atomic<uint64_t>* leaf = FreeLeaf(block, false)) {
    const uint32_t bit = (block & ((1u << kFreeLeafShift) - 1)) >> 2;
    leaf[bit >> 6].fetch_and(~(uint64_t(1) << (bit & 63)), std::memory_order_acq_rel);
  }
}

namespace bf1943 {

#if defined(BF_DEV_TOOLS)
// BF_DEV_DEADOBJ_TEST=1 (dev builds): once, on the server's thread, a guest page allocated and released, then the
// setter called on an object there (the 15 Nov 19:12 All Maps 3 crash). With the guard it is passed over and logged;
// without, the read faults. The registers are put back after.
void RunDeadObjectTest(PPCContext& ctx, uint8_t* base) {
  static bool done = false;
  const char* v = std::getenv("BF_DEV_DEADOBJ_TEST");
  if (done || !v || *v != '1') return;
  done = true;
  rex::memory::Memory* memory = rex::system::kernel_state()->memory();
  rex::memory::BaseHeap* heap = memory->LookupHeap(0x20000000);
  uint32_t page = 0;
  if (!heap || !heap->Alloc(0x1000, 0x1000, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                            rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &page)) {
    REXLOG_WARN("BF game fix test: no guest page to release");
    return;
  }
  const bool live_before = GuestLive(page + 0x100, 72);
  heap->Release(page);
  PPCContext saved;
  std::memcpy(&saved, &ctx, sizeof(ctx));
  ctx.r3.u64 = page + 0x100;
  ctx.r4.u64 = 0x12345678;
  ctx.f1.f64 = 1.0;
  ctx.lr = 0x823F73E0;
  ctx.r1.u64 = ((ctx.r1.u32 - 0x1000) & ~0xFFu);
  sub_821DC048(ctx, base);
  REXLOG_INFO("BF game fix test: the setter on a released object at {:08X} came back (r3 {}; live before release: {})",
              page + 0x100, ctx.r3.u32, live_before);
  std::memcpy(&ctx, &saved, sizeof(ctx));
}
#endif

#if defined(BF_DEV_TOOLS)
// BF_DEV_FIX_TEST=1 (dev builds): once, from a hook on the game's thread, the guarded query over a list of three
// slots, the middle one empty, built on the free part of that thread's guest stack (below its frames). The first
// and last are objects whose type is not in the (empty) filter, so no method runs. Without the guard the empty slot
// faults at guest 0x8. The registers are put back after.
// BF_DEV_BADCALL_TEST=1: once, on the server's thread, a call through a broken function pointer (the stack address
// of the 15 Nov All Maps 3 stop). With guest_invalid_call_skip (on for dedicated servers, bf1943_app.h) it is skipped
// and logged; without, the program stops. The registers are put back after.
void RunBadCallTest(PPCContext& ctx, uint8_t* base) {
  static bool done = false;
  const char* v = std::getenv("BF_DEV_BADCALL_TEST");
  if (done || !v || *v != '1') return;
  done = true;
  PPCContext saved;
  std::memcpy(&saved, &ctx, sizeof(ctx));
  ctx.last_indirect_target = 0x7018E740;
  ctx.lr = 0x82000000;
  ctx.r3.u64 = 0x1234;
  rex::runtime::ResolveIndirectFunction(0x7018E740)(ctx, base);
  REXLOG_INFO("BF game fix test: the call through a broken pointer came back (r3 {:#x}); the server goes on", ctx.r3.u32);
  std::memcpy(&ctx, &saved, sizeof(ctx));
}

// BF_DEV_BAD_WRITE_TEST=1: once, on the server's thread, a write that no guard covers: guest 0x2, as the small heap
// crashes of 15 and 16 Nov wrote (the null page takes no writes). Windows: a crash report and the process ends. Linux:
// with the SDK's fix the process ends with SIGSEGV (a core dump) after a few tries; before it, the write faulted
// again forever, with no ticks and the process alive, so nothing restarted the server.
void RunBadWriteTest(uint8_t* base) {
  static bool done = false;
  const char* v = std::getenv("BF_DEV_BAD_WRITE_TEST");
  if (done || !v || *v != '1') return;
  done = true;
  REXLOG_INFO("BF game fix test: an unguarded write of guest 0x2 now");
  REX_STORE_U32(0x2, 0x12345678);
  REXLOG_INFO("BF game fix test: the unguarded write came back, which it never should");
}

void RunGameFixTest(PPCContext& ctx, uint8_t* base) {
  RunBadWriteTest(base);
  RunBadCallTest(ctx, base);
  RunDeadObjectTest(ctx, base);
  static bool done = false;
  const char* v = std::getenv("BF_DEV_FIX_TEST");
  if (done || !v || *v != '1') return;
  done = true;
  PPCContext saved;
  std::memcpy(&saved, &ctx, sizeof(ctx));
  const uint32_t area = (ctx.r1.u32 - 0x1000) & ~0xFFu;  // 4 KB below the hook's caller: nothing of the thread there
  for (uint32_t a = area; a < area + 0x200; a += 4) REX_STORE_U32(a, 0);
  const uint32_t owner = area, list = area + 0x40, filter = area + 0x80, bits = area + 0x90, words = area + 0xA0,
                 object = area + 0x100, out = area + 0x180;
  REX_STORE_U32(list + 0, object);  // an object of type 0 (not in the filter)
  REX_STORE_U32(list + 4, 0);       // the empty slot
  REX_STORE_U32(list + 8, object);
  REX_STORE_U32(owner + 32, list);
  REX_STORE_U32(owner + 36, list + 12);
  REX_STORE_U32(filter + 0, bits);  // filter -> bits -> words: the bit set, all clear
  REX_STORE_U32(bits + 0, words);
  ctx.r3.u64 = owner, ctx.r4.u64 = filter, ctx.r5.u64 = out;
  ctx.r1.u64 = area - 0x100;  // the query's own frame below the test's data
  const uint32_t before = g_empty_slots.load();
  sub_8275B690(ctx, base);
  REXLOG_INFO("BF game fix test: a list with an empty slot queried without a fault ({} empty slot passed over)",
              g_empty_slots.load() - before);
  // The hash table of the 15 Nov crash: its bucket pointer 1 with 3 buckets counted, and a buffer of its own (+8 to
  // +16) of one byte, which sub_821E08D0 does not free. Without the guard the clear writes guest 0x1.
  std::memcpy(&ctx, &saved, sizeof(ctx));
  for (uint32_t a = area; a < area + 0x200; a += 4) REX_STORE_U32(a, 0);
  const uint32_t table = area;
  REX_STORE_U32(table + 36, 1);
  REX_STORE_U32(table + 40, 3);
  ctx.r3.u64 = table;
  ctx.r1.u64 = area - 0x100;
  const uint32_t bad_before = g_bad_tables.load();
  sub_821E08D0(ctx, base);
  REXLOG_INFO("BF game fix test: a hash table with its bucket pointer at 1 cleared without a fault ({} caught; left "
              "as it was: bucket pointer {:08X}, {} buckets)",
              g_bad_tables.load() - bad_before, REX_LOAD_U32(table + 36), REX_LOAD_U32(table + 40));
  std::memcpy(&ctx, &saved, sizeof(ctx));
  // The table of the 18 and 20 Nov crashes: bucket pointer 0, a count of C2C80000 (the float -100.0 of the object now
  // in that memory) and a buffer from 00000400 to 00000800, which the clear frees. Without the guard the big heap's
  // free writes guest 0x3FC.
  for (uint32_t a = area; a < area + 0x200; a += 4) REX_STORE_U32(a, 0);
  REX_STORE_U32(table + 8, 0x400);
  REX_STORE_U32(table + 16, 0x800);
  REX_STORE_U32(table + 40, 0xC2C80000);
  REX_STORE_U32(table + 44, 0x5EED5EED);  // the clear sets +44 to 0: it stays if the clear did not run
  ctx.r3.u64 = table;
  ctx.r1.u64 = area - 0x100;
  const uint32_t gone_before = g_bad_tables.load();
  sub_821E08D0(ctx, base);
  REXLOG_INFO("BF game fix test: a hash table that is gone (count C2C80000, a buffer at 00000400) passed over without "
              "a fault ({} caught; +44 {:08X}, untouched)",
              g_bad_tables.load() - gone_before, REX_LOAD_U32(table + 44));
  std::memcpy(&ctx, &saved, sizeof(ctx));
  // 16 Nov 2026: a live object whose table has no bucket array (the All Maps 1 crash), and a scene node whose entry
  // array is 0 (the All Maps 3 crash). Without the guards: a read of guest 0x10000 and a write of guest 0x18.
  for (uint32_t a = area; a < area + 0x200; a += 4) REX_STORE_U32(a, 0);
  const uint32_t no_table = area;  // +36 bucket pointer 0, +40 three buckets counted
  REX_STORE_U32(no_table + 40, 3);
  ctx.r3.u64 = no_table, ctx.r4.u64 = 0x12345678, ctx.f1.f64 = 1.0, ctx.lr = 0x82406654;
  ctx.r1.u64 = area - 0x100;
  const uint32_t no_buckets_before = g_no_buckets.load();
  sub_821DC178(ctx, base);
  REXLOG_INFO("BF game fix test: the setter on a table with no buckets came back without a fault ({} caught, r3 {})",
              g_no_buckets.load() - no_buckets_before, ctx.r3.u32);
  std::memcpy(&ctx, &saved, sizeof(ctx));
  for (uint32_t a = area; a < area + 0x200; a += 4) REX_STORE_U32(a, 0);
  const uint32_t node = area;  // +44 entries 0, +48 index 32 (the 22:06 crash's write of 0x218)
  REX_STORE_U8(node + 48, 32);
  ctx.r3.u64 = node, ctx.r4.u64 = 1, ctx.lr = 0x822AEEC8;
  ctx.r1.u64 = area - 0x100;
  const uint32_t bad_nodes_before = g_bad_nodes.load();
  sub_822AE650(ctx, base);
  REXLOG_INFO("BF game fix test: a scene node with no entries updated without a fault ({} caught)",
              g_bad_nodes.load() - bad_nodes_before);
  std::memcpy(&ctx, &saved, sizeof(ctx));
  // 17 Nov 2026: the same node with its entries in guest memory that is reserved and not committed (All Maps 3 on
  // 0.4.7.1, 16 Nov 16:50 server time: entries 10000000, a read of guest 10000018). The page table gives the page its
  // protection already, so only the host check catches it.
  if (rex::memory::BaseHeap* heap = rex::system::kernel_state()->memory()->LookupHeap(0x20000000)) {
    uint32_t page = 0;
    if (heap->Alloc(0x10000, 0x10000, rex::memory::kMemoryAllocationReserve,
                    rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &page)) {
      uint32_t protect = 0;
      heap->QueryProtect(page, &protect);
      for (uint32_t a = area; a < area + 0x200; a += 4) REX_STORE_U32(a, 0);
      REX_STORE_U32(area + 44, page);
      ctx.r3.u64 = area, ctx.r4.u64 = 1, ctx.lr = 0x822AE700, ctx.r1.u64 = area - 0x100;
      const uint32_t reserved_before = g_bad_nodes.load();
      sub_822AE650(ctx, base);
      REXLOG_INFO("BF game fix test: a scene node whose entries are reserved, not committed ({:08X}, protection {:X} "
                  "in the page table) updated without a fault ({} caught)",
                  page, protect, g_bad_nodes.load() - reserved_before);
      heap->Release(page);
      std::memcpy(&ctx, &saved, sizeof(ctx));
    }
  }
  // 17 Nov 2026: the part tree walk on the child pointer of the 16 Nov 10:07 All Maps 1 crash (text, 6C65745F, not
  // committed memory). Without the guard: a read of guest 6C65745F, the node's vtable.
  ctx.r3.u64 = 0x6C65745F, ctx.lr = 0x822A8FE0, ctx.r1.u64 = area - 0x100;
  const uint32_t bad_parts_before = g_bad_parts.load();
  sub_822A8F80(ctx, base);
  REXLOG_INFO("BF game fix test: a part tree walk on a released part came back without a fault ({} caught)",
              g_bad_parts.load() - bad_parts_before);
  std::memcpy(&ctx, &saved, sizeof(ctx));
  // 17 Nov 2026: the other walks on a node whose entries are 0 (the 21:29 All Maps 1 crash: a write of guest F8), its
  // vtable a real one (sub_822D8F70's part, 8200F168). Without the guards each walk writes guest (index+1)*16+8.
  for (auto walk : {&sub_822AE918, &sub_822AEBE0, &sub_822B0580, &sub_822DDBE8}) {
    for (uint32_t a = area; a < area + 0x200; a += 4) REX_STORE_U32(a, 0);
    REX_STORE_U32(area, 0x8200F168);
    REX_STORE_U8(area + 48, 14);
    ctx.r3.u64 = area, ctx.r4.u64 = 1, ctx.lr = 0x822AEED4, ctx.r1.u64 = area - 0x100;
    const uint32_t before = g_bad_trees.load();
    walk(ctx, base);
    REXLOG_INFO("BF game fix test: a tree walk on a node with no entries came back without a fault ({} caught)",
                g_bad_trees.load() - before);
    std::memcpy(&ctx, &saved, sizeof(ctx));
  }
  // 17 Nov 2026: the small heap's guards, through the game's own operator new (sub_82627D10) and delete
  // (sub_82624D90). A 40 byte block freed twice, then two blocks of that size asked for: without the guard both are
  // the same block (the state behind the 15 Nov crashes). Then a freed block's link overwritten with the vtable of those
  // crashes: without the guard the second block asked for is guest 8200F908.
  const auto New = [&](uint32_t size) {
    ctx.r3.u64 = size, ctx.lr = 0x823C3918, ctx.r1.u64 = area - 0x100;
    sub_82627D10(ctx, base);
    return ctx.r3.u32;
  };
  const auto Delete = [&](uint32_t block) {
    ctx.r3.u64 = block, ctx.lr = 0x8274B2E0, ctx.r1.u64 = area - 0x100;
    sub_82624D90(ctx, base);
  };
  const uint32_t frees_before = g_double_frees.load(), lists_before = g_bad_lists.load();
  const uint32_t twice = New(40);
  Delete(twice);
  Delete(twice);
  const uint32_t first = New(40), second = New(40);
  REXLOG_INFO("BF game fix test: a small block freed twice ({:08X}), then two asked for: {:08X} and {:08X}, {} ({} "
              "caught)",
              twice, first, second, first != second ? "two blocks" : "THE SAME BLOCK TWICE",
              g_double_frees.load() - frees_before);
  Delete(first);
  if (second != first) Delete(second);
  const uint32_t broken = New(40);
  Delete(broken);
  REX_STORE_U32(broken, 0x8200F908);  // what sub_822DD9B8's constructor stores at an object's +0
  const uint32_t third = New(40), fourth = New(40);
  REXLOG_INFO("BF game fix test: a free list broken by a write into a freed block ({:08X}), then two asked for: "
              "{:08X} and {:08X}, {} ({} caught)",
              broken, third, fourth, fourth == 0x8200F908 ? "THE VTABLE HANDED OUT" : "both blocks",
              g_bad_lists.load() - lists_before);
  Delete(third);
  if (fourth != 0x8200F908) Delete(fourth);
  // 17 Nov 2026: the free history: a part freed through operator delete, then walked; the guard's line names the
  // free (from 8274B2E0, the return address Delete gives).
  const uint32_t part = New(64);
  Delete(part);
  ctx.r3.u64 = part, ctx.lr = 0x822A8FE0, ctx.r1.u64 = area - 0x100;
  sub_822A8F80(ctx, base);
  REXLOG_INFO("BF game fix test: a walk on a freed part ({:08X}) came back; the free noted: \"{}\"", part,
              FreedBy(part));
  std::memcpy(&ctx, &saved, sizeof(ctx));
  // 17 Nov 2026: a read of guest memory reserved and never committed (the 01:31 All Maps 3 crash read guest 7D8803C2).
  // Windows: the stray read guard gives the page as read only zeros, the live check still calls it dead, and the
  // guest's own commit later makes it live again. Without the guard (--bf_stray_read_guard=false) the read crashes.
  if (rex::memory::BaseHeap* heap = rex::system::kernel_state()->memory()->LookupHeap(0x20000000)) {
    uint32_t page = 0;
    if (heap->Alloc(0x10000, 0x10000, rex::memory::kMemoryAllocationReserve,
                    rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &page)) {
      REXLOG_INFO("BF game fix test: a read of guest {:08X}, reserved and never committed, now", page + 0x3C2);
      const uint32_t value = REX_LOAD_U32(page + 0x3C2);
      const bool live_after_read = GuestLive(page + 0x3C2, 4);
      const bool committed = heap->AllocFixed(page, 0x10000, 0x10000, rex::memory::kMemoryAllocationCommit,
                                              rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
      bool live_after_commit = false;
      uint32_t written = 0;
      if (committed) {
        std::this_thread::sleep_for(std::chrono::milliseconds(6));  // past GuestLive's 5 ms per page memory
        live_after_commit = GuestLive(page + 0x3C2, 4);
        REX_STORE_U32(page + 0x3C0, 0x12345678);
        written = REX_LOAD_U32(page + 0x3C0);
      }
      REXLOG_INFO("BF game fix test: the stray read gave {:08X} without a fault; live after the read: {} (want false on Windows); "
                  "the guest's commit: {}, live after it: {} (want true), a write there read back {:08X}",
                  value, live_after_read, committed, live_after_commit, written);
      heap->Release(page);
    }
  }
}
#endif

}  // namespace bf1943

// Server sizes study (docs/SERVER-SIZES.md, 20 Nov 2026): a client joining a 63 player match died on a network object
// id its ghost table did not hold (sub_8275D230). Its line is now Settings > Diagnostics' (diagnostics_extra.cpp, every
// build), with the id, the caller and which kind of miss.
