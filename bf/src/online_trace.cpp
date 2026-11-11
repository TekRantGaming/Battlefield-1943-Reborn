// Developer aid (BF_DEV_TOOLS builds with BF_NET_TRACE=1): log the online flow's state changes and the connection
// dialogs, to find what makes Multiplayer report failure reason 14 ("Failed to connect to Xbox LIVE").
//
//   sub_8250F940(state machine, handler)  switches a state machine to a new handler function (seen in
//                                         Function_824EBA48: handlers 0x824EA3F0 and 0x824EA3B0)
//   sub_82455BB8(dialogs, event)          chooses the connection dialog; event type at +40, reason at +44 (14 platform
//                                         unavailable, 15 invalid profile, 16 busy, 17 update required, 18 no
//                                         network, 19 invite not found, 20 restricted)
//   sub_824EBA48(connect state)           the connect flow's failure path (switches to handler 0x824EA3F0, resets
//                                         the online manager, posts reason 14); logged with the guest call stack
// Each line has the guest return address, so the caller can be read in the generated code.

#if defined(BF_DEV_TOOLS)

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

#include "dev_guest_memory.h"

namespace {

bool TraceOn() {
  static const bool on = [] {
    const char* v = std::getenv("BF_NET_TRACE");
    return v && *v && *v != '0';
  }();
  return on;
}

uint32_t Load32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// The guest call stack: [r1] is the caller's r1 (back chain), the caller's frame keeps the saved LR at back chain - 8.
void LogGuestStack(const char* what, const PPCContext& ctx, const uint8_t* base) {
  std::string text;
  uint32_t sp = ctx.r1.u32;
  for (int i = 0; i < 16 && sp >= 0x70000000 && sp < 0x80000000; ++i) {
    const uint32_t back = bf1943::dev::SafeLoad32(base, sp);
    if (back <= sp || back >= 0x80000000) break;
    text += fmt::format(" {:08X}", bf1943::dev::SafeLoad32(base, back - 8));
    sp = back;
  }
  REXLOG_INFO("BF ONLINE {} (lr {:08X}) stack:{}", what, static_cast<uint32_t>(ctx.lr), text);
}

}  // namespace

REX_EXTERN(__imp__sub_824EBA48);
REX_HOOK_RAW(sub_824EBA48) {
  // Runs every frame; acts when the byte at +0x24 is set and the count at [+4]+0x124 is below 1. Log both when they
  // change, and the stack when the failure branch is taken.
  if (TraceOn()) {
    const uint32_t self = ctx.r3.u32;
    const uint32_t flag = base[self + 0x24];
    const uint32_t inner = bf1943::dev::SafeLoad32(base, self + 4);
    const int32_t count = inner ? static_cast<int32_t>(bf1943::dev::SafeLoad32(base, inner + 0x124)) : -999;
    static uint32_t last_flag = 0xFFFFFFFF;
    static int32_t last_count = -12345;
    if (flag != last_flag || count != last_count) {
      REXLOG_INFO("BF ONLINE connect update self={:08X} inner={:08X} flag(+0x24)={} count([+4]+0x124)={}", self, inner,
                  flag, count);
      last_flag = flag;
      last_count = count;
    }
    if (flag && count < 1) LogGuestStack("connect failure branch", ctx, base);
  }
  __imp__sub_824EBA48(ctx, base);
}

REX_EXTERN(__imp__sub_8250F940);
REX_HOOK_RAW(sub_8250F940) {
  static std::atomic<int> calls{0};
  if (TraceOn() && calls.fetch_add(1) < 400)
    REXLOG_INFO("BF ONLINE state {:08X} -> handler {:08X} (lr {:08X})", ctx.r3.u32, ctx.r4.u32,
                static_cast<uint32_t>(ctx.lr));
  __imp__sub_8250F940(ctx, base);
}

REX_EXTERN(__imp__sub_82455BB8);
REX_HOOK_RAW(sub_82455BB8) {
  if (TraceOn()) {
    const uint32_t event = ctx.r4.u32;
    REXLOG_INFO("BF ONLINE dialog r3={:08X} event={:08X} type {} reason {} (lr {:08X})", ctx.r3.u32, event,
                bf1943::dev::SafeLoad32(base, event + 40), bf1943::dev::SafeLoad32(base, event + 44), static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_82455BB8(ctx, base);
}

// The news ticker handler (sub_823BB598(ticker, ?, http request)): checks the first 8 bytes for a UTF-8 byte order
// mark and "<?xml", reads <ticker><lang id><item>, posts game message 0x25, or retries in 60 s.
REX_EXTERN(__imp__sub_823BB598);
REX_HOOK_RAW(sub_823BB598) {
  static std::atomic<int> calls{0};
  const bool log = TraceOn() && calls.fetch_add(1) < 20;
  if (log) REXLOG_INFO("BF ONLINE ticker handler r3={:08X} r4={:08X} r5={:08X} (lr {:08X})", ctx.r3.u32, ctx.r4.u32,
                       ctx.r5.u32, static_cast<uint32_t>(ctx.lr));
  const uint32_t self = ctx.r3.u32;
  __imp__sub_823BB598(ctx, base);
  if (log) REXLOG_INFO("BF ONLINE ticker handler done: [+0xC]={:08X} [+0x1C]={} [+0x10] byte={}",
                       bf1943::dev::SafeLoad32(base, self + 0xC), bf1943::dev::SafeLoad32(base, self + 0x1C),
                       base[self + 0x10]);
}

// The Flash menu calls (sub_8243BB70) are logged by auto_join.cpp, which also needs them in release builds.

// Playgroup ("couch") events to the menu (sub_82473100(ui, event), type at +0x28, reason at +0x2C): 0 join popup,
// 1 leaving, 2 creating, 3 to 5 join failed (4 full, 5 trial), 6 create failed, 7 to 9 member updates, 10 self
// joined, 11 to 13 exit or lost, ...
REX_EXTERN(__imp__sub_82473100);
REX_HOOK_RAW(sub_82473100) {
  if (TraceOn()) {
    const uint32_t event = ctx.r4.u32;
    REXLOG_INFO("BF ONLINE couch event type {} reason {} (lr {:08X})", bf1943::dev::SafeLoad32(base, event + 0x28),
                bf1943::dev::SafeLoad32(base, event + 0x2C), static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_82473100(ctx, base);
}

// DirtySock's "data arrived" step for a socket record (sub_82495F50(record)): socket at +0x18, receive callback at
// +0x50 with its user data at +0x4C, callback flags at +0x40. Logged once per record, to find who reads a socket.
REX_EXTERN(__imp__sub_82495F50);
REX_HOOK_RAW(sub_82495F50) {
  if (TraceOn()) {
    static std::mutex mutex;
    static std::set<uint32_t> seen;
    const uint32_t record = ctx.r3.u32;
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.insert(record).second)
      REXLOG_INFO("BF ONLINE socket record {:08X} socket {:08X} callback {:08X} data {:08X} flags {:08X} (lr {:08X})",
                  record, bf1943::dev::SafeLoad32(base, record + 0x18), bf1943::dev::SafeLoad32(base, record + 0x50),
                  bf1943::dev::SafeLoad32(base, record + 0x4C), bf1943::dev::SafeLoad32(base, record + 0x40),
                  static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_82495F50(ctx, base);
}

// The tunnel module's receive match, sub_82939458(module, ..., data, length, from): finds the tunnel for a datagram
// by sender address, else an unbound tunnel whose key and ident check (sub_829392F8) accepts it. Module: tunnel
// count at +0x14, tunnels from +0x88, 0xB8C each: ident +0, address +4, port +8, bound flag +0x67C, 8 keys of 0x80
// bytes from +0x27C. Logged for the first datagrams, with the first bytes of each.
REX_EXTERN(__imp__sub_82939458);
REX_HOOK_RAW(sub_82939458) {
  static std::atomic<int> calls{0};
  const uint32_t module = ctx.r3.u32, data = ctx.r6.u32, length = ctx.r7.u32, from = ctx.r8.u32;
  const bool log = TraceOn() && calls.fetch_add(1) < 6;
  if (log) {
    const uint32_t count = bf1943::dev::SafeLoad32(base, module + 0x14);
    std::string bytes;
    for (uint32_t i = 0; i < length && i < 32; ++i) bytes += fmt::format("{:02X}", base[data + i]);
    REXLOG_INFO("BF TUNNEL recv module {:08X} tunnels {} crypt off {} from {}.{}.{}.{}:{} data {}", module, count,
                bf1943::dev::SafeLoad32(base, module + 0x20), base[from + 4], base[from + 5], base[from + 6],
                base[from + 7], base[from + 2] << 8 | base[from + 3], bytes);
    for (uint32_t t = 0; t < count && t < 4; ++t) {
      const uint32_t tunnel = module + 0x88 + t * 0xB8C;
      std::string keys;
      for (uint32_t k = 0; k < 8; ++k) {
        const uint32_t key = tunnel + 0x27C + k * 0x80;
        if (!base[key]) continue;
        std::string text;
        for (uint32_t i = 0; i < 0x80 && base[key + i]; ++i) text += char(base[key + i]);
        keys += fmt::format(" key{}={}", k, text);
      }
      REXLOG_INFO("BF TUNNEL   tunnel {} ident {:08X} address {:08X}:{} bound {}{}", t,
                  bf1943::dev::SafeLoad32(base, tunnel), bf1943::dev::SafeLoad32(base, tunnel + 4),
                  base[tunnel + 8] << 8 | base[tunnel + 9], base[tunnel + 0x67C], keys);
    }
  }
  __imp__sub_82939458(ctx, base);
  if (log) REXLOG_INFO("BF TUNNEL recv result {}", static_cast<int32_t>(ctx.r3.u32));
}

// The tunnel module's per tunnel send, sub_829381E8(module, tunnel): logged once per tunnel (layout as above).
REX_EXTERN(__imp__sub_829381E8);
REX_HOOK_RAW(sub_829381E8) {
  if (TraceOn()) {
    static std::mutex mutex;
    static std::set<uint32_t> seen;
    const uint32_t tunnel = ctx.r4.u32;
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.insert(tunnel).second) {
      std::string keys;
      for (uint32_t k = 0; k < 8; ++k) {
        const uint32_t key = tunnel + 0x27C + k * 0x80;
        if (!base[key]) continue;
        std::string text;
        for (uint32_t i = 0; i < 0x80 && base[key + i]; ++i) text += char(base[key + i]);
        keys += fmt::format(" key{}={}", k, text);
      }
      REXLOG_INFO("BF TUNNEL send tunnel {:08X} ident {:08X} address {:08X}:{} bound {} crypt off {}{}", tunnel,
                  bf1943::dev::SafeLoad32(base, tunnel), bf1943::dev::SafeLoad32(base, tunnel + 4),
                  base[tunnel + 8] << 8 | base[tunnel + 9], base[tunnel + 0x67C],
                  bf1943::dev::SafeLoad32(base, ctx.r3.u32 + 0x20), keys);
    }
  }
  __imp__sub_829381E8(ctx, base);
}

// Tunnel alloc, sub_829389E8(module, info, key): info is ident, address, port (then virtual ports 6000 to 6003).
// Its caller sub_824A7420(link, address info) is the GameManager link setup: ident = hash of the decimal text of
// the 64 bit id at link+0x110, key at link+0x121.
REX_EXTERN(__imp__sub_829389E8);
REX_HOOK_RAW(sub_829389E8) {
  if (TraceOn()) {
    const uint32_t info = ctx.r4.u32, key = ctx.r5.u32;
    std::string text;
    for (uint32_t i = 0; i < 0x80 && base[key + i]; ++i) text += char(base[key + i]);
    REXLOG_INFO("BF TUNNEL alloc module {:08X} ident {:08X} address {:08X}:{} key {}", ctx.r3.u32,
                bf1943::dev::SafeLoad32(base, info), bf1943::dev::SafeLoad32(base, info + 4),
                base[info + 8] << 8 | base[info + 9], text);
    LogGuestStack("tunnel alloc", ctx, base);
  }
  __imp__sub_829389E8(ctx, base);
}

REX_EXTERN(__imp__sub_824A7420);
REX_HOOK_RAW(sub_824A7420) {
  if (TraceOn()) {
    const uint32_t link = ctx.r3.u32, address = ctx.r4.u32;
    const uint32_t module_owner = bf1943::dev::SafeLoad32(base, link + 0x14);
    REXLOG_INFO("BF TUNNEL link setup {:08X} id {:08X}{:08X} address {:08X} tunnel module {:08X}", link,
                bf1943::dev::SafeLoad32(base, link + 0x110), bf1943::dev::SafeLoad32(base, link + 0x114),
                address ? bf1943::dev::SafeLoad32(base, address + 0xC) : 0,
                module_owner ? bf1943::dev::SafeLoad32(base, module_owner + 0x1C) : 0);
    LogGuestStack("link setup", ctx, base);
  }
  __imp__sub_824A7420(ctx, base);
}

// Theater packet builders (vtable at 0x8202B4BC): EGRS sub_824D7800(builder, packet, lid, gid, allowed, pid, reason)
// is built by the host's EGRQ handler; its stack shows that handler.
REX_EXTERN(__imp__sub_824D7800);
REX_HOOK_RAW(sub_824D7800) {
  if (TraceOn()) {
    REXLOG_INFO("BF ONLINE EGRS lid {} gid {} allowed {} pid {}", ctx.r5.u32, ctx.r6.u32, ctx.r7.u32 & 0xFF,
                ctx.r8.u32);
    LogGuestStack("EGRS built", ctx, base);
  }
  __imp__sub_824D7800(ctx, base);
}

// Theater request send, sub_824EBD50(connection, packet, owner, callback, user, timeout): the packet's command is at
// +0x18. PLVT (player left) is logged with its stack, to find why a host drops a joiner.
REX_EXTERN(__imp__sub_824EBD50);
REX_HOOK_RAW(sub_824EBD50) {
  if (TraceOn()) {
    const uint32_t command = bf1943::dev::SafeLoad32(base, ctx.r4.u32 + 0x18);
    if (command == 0x504C5654) LogGuestStack("Theater PLVT sent", ctx, base);
    if (command == 0x45434E4C) LogGuestStack("Theater ECNL sent", ctx, base);
  }
  __imp__sub_824EBD50(ctx, base);
}

// The PLVT builder (vtable at 0x8202B4BC, slot 0x78): its stack shows why a host drops a player.
REX_EXTERN(__imp__sub_824D79A8);
REX_HOOK_RAW(sub_824D79A8) {
  if (TraceOn()) LogGuestStack("PLVT built", ctx, base);
  __imp__sub_824D79A8(ctx, base);
}

// Game create or join start, sub_824B61C0(game, a, packet, kind, b, c): stores the kind at game+0x9C (0 public
// game, 1 private match) and sends the packet (CGAM or EGAM, its command at +0x18). The kind also picks the
// operation's completion event: a Quick Match join must stay kind 0, or BF never takes the joined game and the
// joiner leaves at once (ECNL, run duo15).
REX_EXTERN(__imp__sub_824B61C0);
REX_HOOK_RAW(sub_824B61C0) {
  if (TraceOn())
    REXLOG_INFO("BF ONLINE game start {:08X} command {:08X} kind {}", ctx.r3.u32,
                bf1943::dev::SafeLoad32(base, ctx.r5.u32 + 0x18), ctx.r6.u32);
  __imp__sub_824B61C0(ctx, base);
}

// A host's link to a joining player, sub_824BCDF8(player, ...): the game (player[2]) kind at +0x9C picks the link's
// virtual port, 6001 for a public game, 6002 for a private match. A Quick Match joiner of a hosted private match
// links on 6001 and the host on 6002, so they never meet (run duo11). Experiment: BF_HOST_LINK_PUBLIC=1 makes a
// private match host link on 6001.
REX_EXTERN(__imp__sub_824BCDF8);
REX_HOOK_RAW(sub_824BCDF8) {
  static const bool public_link = [] {
    const char* v = std::getenv("BF_HOST_LINK_PUBLIC");
    return v && *v == '1';
  }();
  const uint32_t game = bf1943::dev::SafeLoad32(base, ctx.r3.u32 + 8);
  const uint32_t kind = game ? bf1943::dev::SafeLoad32(base, game + 0x9C) : 0xFFFFFFFF;
  if (TraceOn()) REXLOG_INFO("BF ONLINE player link start player {:08X} game {:08X} kind {}", ctx.r3.u32, game, kind);
  const bool patch = public_link && kind == 1;
  if (patch) {
    base[game + 0x9F] = 0;
    REXLOG_INFO("BF ONLINE player link: private match linked on the public port (BF_HOST_LINK_PUBLIC)");
  }
  __imp__sub_824BCDF8(ctx, base);
  if (patch) base[game + 0x9F] = 1;
}

// EA GameManager's debug log (the object at 0x82AF696C, vtable 0x8202AD08): +0x10 sub_824C6250(log, text) and
// +0x14 sub_824C6378(log, level, format, ...). Both are kept, and written to our log as "BF GM" lines.
// Guest variadic arguments: r6 to r10, then the stack parameter area from r1+0x54 in 8 byte slots.
namespace {

std::string GuestString(const uint8_t* base, uint32_t address) {
  std::string text;
  for (uint32_t i = 0; address && i < 400 && bf1943::dev::GuestReadable(base, address + i, 1) && base[address + i]; ++i)
    text += char(base[address + i]);
  return text;
}

std::string GuestFormat(const PPCContext& ctx, const uint8_t* base, uint32_t format) {
  const std::string f = GuestString(base, format);
  int next = 0;
  auto arg = [&]() -> uint64_t {
    const int k = next++;
    switch (k) {
      case 0: return ctx.r6.u64;
      case 1: return ctx.r7.u64;
      case 2: return ctx.r8.u64;
      case 3: return ctx.r9.u64;
      case 4: return ctx.r10.u64;
      default: return bf1943::dev::SafeLoad32(base, ctx.r1.u32 + 0x54 + (k - 5) * 8);
    }
  };
  std::string out;
  for (size_t i = 0; i < f.size(); ++i) {
    if (f[i] != '%') { out += f[i]; continue; }
    size_t j = i + 1;
    while (j < f.size() && std::strchr("-+ #0123456789.lhI64", f[j])) ++j;
    if (j >= f.size()) break;
    const char c = f[j];
    if (c == '%') out += '%';
    else if (c == 's') out += GuestString(base, static_cast<uint32_t>(arg()));
    else if (c == 'd' || c == 'i') out += std::to_string(static_cast<int32_t>(arg()));
    else if (c == 'u') out += std::to_string(static_cast<uint32_t>(arg()));
    else if (c == 'x' || c == 'X' || c == 'p') out += fmt::format("{:x}", static_cast<uint32_t>(arg()));
    else if (c == 'c') out += char(arg());
    else out += fmt::format("<%{}>", c), arg();
    i = j;
  }
  return out;
}

}  // namespace

REX_EXTERN(__imp__sub_824C6250);
REX_HOOK_RAW(sub_824C6250) {
  if (TraceOn()) REXLOG_INFO("BF GM {}", GuestString(base, ctx.r4.u32));
  __imp__sub_824C6250(ctx, base);
}

REX_EXTERN(__imp__sub_824C6378);
REX_HOOK_RAW(sub_824C6378) {
  if (TraceOn()) REXLOG_INFO("BF GM [{:x}] {}", ctx.r4.u32, GuestFormat(ctx, base, ctx.r5.u32));
  __imp__sub_824C6378(ctx, base);
}

// The ECNL builder (cancel entering a game, vtable 0x8202B4BC slot 0x6C): its stack shows why a joiner gives up.
REX_EXTERN(__imp__sub_824D7798);
REX_HOOK_RAW(sub_824D7798) {
  if (TraceOn()) LogGuestStack("ECNL built", ctx, base);
  __imp__sub_824D7798(ctx, base);
}

// GameManager's leave game (sub_824BDC50) and the client connection's disconnect (sub_8237CE40) are hooked in
// auto_join.cpp (all builds), which logs them with BF_NET_TRACE.

// The game client's connection object (vtable 0x8201AF80 area: Frostbite client connection methods at 0x8274xxxx,
// BF overrides at 0x8237xxxx and 0x8238xxxx). Logs the first calls of each slot, to see how a joiner connects to
// the game server and why it disconnects (sub_8237CE40(connection, reason) posts the leave event).
#define BF_CONN_TRACE(addr)                                                                                       REX_EXTERN(__imp__sub_##addr);                                                                                  REX_HOOK_RAW(sub_##addr) {                                                                                        static std::atomic<int> calls{0};                                                                               if (TraceOn() && calls++ < 20)                                                                                    REXLOG_INFO("BF CONN " #addr " r3={:08X} r4={:08X} r5={:08X} r6={:08X} (lr {:08X})", ctx.r3.u32, ctx.r4.u32,                   ctx.r5.u32, ctx.r6.u32, static_cast<uint32_t>(ctx.lr));                                           __imp__sub_##addr(ctx, base);                                                                                 }
BF_CONN_TRACE(82385E10)
BF_CONN_TRACE(8238D1A8)
BF_CONN_TRACE(82385868)
BF_CONN_TRACE(82371588)
BF_CONN_TRACE(822F32D8)
BF_CONN_TRACE(82743A90)

#endif  // BF_DEV_TOOLS
