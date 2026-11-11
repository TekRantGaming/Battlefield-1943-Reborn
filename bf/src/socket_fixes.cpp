// Socket error codes for the guest. The runtime's recv, send, recvfrom and sendto return -1 on failure but never set
// the guest thread's last error, so WSAGetLastError reports whatever an earlier call left there (31 in our runs).
// EA's DirtySock reads it after every -1: a non blocking recv with no data yet must give WSAEWOULDBLOCK (10035), or
// the FESL connection is closed right after the login reply ("Failed to connect to EA Online", runs\fesl3).
//
// recv and send call the host socket here and read the host error right after the call. Reading it after the
// runtime's own recv returned was not enough: on Windows WSAGetLastError is the thread's GetLastError, and any
// successful Win32 call in between (often under load) leaves 0 there, so a recv that would block came back as -1
// with error 0 and DirtySock closed the FESL connection while a level loaded (run ded2). On Linux errno is mapped to
// the Winsock codes, which are the same on the console.

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>

#include "dev_guest_memory.h"
#include "private_match.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace {

PPCFunc* RuntimeExport(const char* name) {
#if defined(_WIN32)
  static HMODULE runtime = GetModuleHandleA("rexruntime.dll");
  return reinterpret_cast<PPCFunc*>(GetProcAddress(runtime, name));
#else
  return reinterpret_cast<PPCFunc*>(dlsym(RTLD_NEXT, name));
#endif
}

// The last host socket error as a Winsock code.
uint32_t HostSocketError() {
#if defined(_WIN32)
  return static_cast<uint32_t>(WSAGetLastError());
#else
  switch (errno) {
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
      return 10035;  // WSAEWOULDBLOCK
    case EINPROGRESS: return 10036;
    case EALREADY: return 10037;
    case ENOTSOCK: return 10038;
    case EMSGSIZE: return 10040;
    case EADDRINUSE: return 10048;
    case ENETUNREACH: return 10051;
    case ECONNABORTED: return 10053;
    case ECONNRESET: return 10054;
    case ENOBUFS: return 10055;
    case EISCONN: return 10056;
    case ENOTCONN: return 10057;
    case ETIMEDOUT: return 10060;
    case ECONNREFUSED: return 10061;
    case EHOSTUNREACH: return 10065;
    default: return 10022;  // WSAEINVAL
  }
#endif
}

bool TraceOn() {
  static const bool on = [] {
    const char* v = std::getenv("BF_NET_TRACE");
    return v && *v && *v != '0';
  }();
  return on;
}

// Developer trace: every call that moved data or failed with something other than "would block"; would block
// results the first time per socket, then every 500th, so a socket polled forever shows up.
void TraceResult(const char* name, uint32_t socket, uint32_t length, const std::string& extra, uint32_t result,
                 uint32_t error) {
  if (!TraceOn()) return;
  static std::atomic<int> logged{0};
  if (error != 10035) {
    if (logged.fetch_add(1) < 200)
      REXLOG_INFO("BF NET {} socket {:08X} length {}{} -> {:08X} error {}", name, socket, length, extra, result, error);
    return;
  }
  static std::mutex mutex;
  static std::map<uint32_t, uint32_t> counts;
  std::lock_guard<std::mutex> lock(mutex);
  const uint32_t n = ++counts[socket];
  if (n == 1 || n % 500 == 0) REXLOG_INFO("BF NET {} socket {:08X} would block (x{})", name, socket, n);
}

std::string FormatAddress(const uint8_t* a) {
  return fmt::format("{}.{}.{}.{}:{}", a[4], a[5], a[6], a[7], a[2] << 8 | a[3]);
}

// The guest call stack (saved LRs up the back chain, stack frames only), for the first send from each caller.
std::string GuestStack(const PPCContext& ctx, const uint8_t* base) {
  auto load = [base](uint32_t a) { return bf1943::dev::SafeLoad32(base, a); };
  std::string text = " stack";
  uint32_t sp = ctx.r1.u32;
  for (int i = 0; i < 14 && sp >= 0x70000000 && sp < 0x7FFFFFF0; ++i) {
    const uint32_t back = load(sp);
    if (back <= sp || back >= 0x80000000) break;
    text += fmt::format(" {:08X}", load(back - 8));
    sp = back;
  }
  return text;
}

// The first bytes of a datagram and the guest caller, for the trace. BF_NET_DUMP=1 logs whole datagrams.
std::string FormatData(const uint8_t* data, int length, uint32_t lr) {
  static const int limit = [] {
    const char* v = std::getenv("BF_NET_DUMP");
    return v && *v && *v != '0' ? 4096 : 48;
  }();
  std::string text = fmt::format(" (lr {:08X})", lr);
  for (int i = 0; i < length && i < limit; ++i) text += fmt::format("{}{:02X}", i ? "" : " data ", data[i]);
  return text;
}

}  // namespace


// sendto and recvfrom. The runtime converts the guest's sockaddr_in through its byte swapping fields: sendto turns
// 127.0.0.1:1003 into 1.0.0.127 on port 60163 (WSAENETUNREACH, run join2) and recvfrom reports the sender's port
// byte swapped. Here the guest's port and address bytes, already in network order, go to the host socket as they
// are and come back the same way. VDP sockets (protocol 254) are plain UDP sockets in the runtime.
//   sendto(socket, buf, len, flags, to, tolen)        recvfrom(socket, buf, len, flags, from, fromlen)
namespace {

using NativeSocket = decltype(::socket(0, 0, 0));

bool LookupNative(uint32_t handle, NativeSocket* out) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XSocket>(handle);
  if (!socket) {
    if (TraceOn()) {
      auto object = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XObject>(handle);
      REXLOG_INFO("BF NET handle {:08X} is not a socket: {}", handle,
                  object ? fmt::format("object type {}", static_cast<uint32_t>(object->type())) : "no object");
    }
    return false;
  }
  *out = static_cast<NativeSocket>(socket->native_handle());
  return true;
}

}  // namespace

#if defined(BF_DEV_TOOLS)
namespace {
// BF_DEV_SENDS=1 (tests): datagrams sent to each address, logged every 5 s (a dedicated server still sending to a
// player whose game is gone, or not).
void DevCountSend(const uint8_t* base, uint32_t to) {
  static const bool sends = std::getenv("BF_DEV_SENDS") != nullptr;
  if (!sends || !to) return;
  static std::mutex mutex;
  static std::map<std::string, uint32_t> counts;
  static auto since = std::chrono::steady_clock::now();
  std::lock_guard lock(mutex);
  ++counts[FormatAddress(base + to)];
  const auto now = std::chrono::steady_clock::now();
  if (now - since > std::chrono::seconds(5)) {
    std::string line;
    for (const auto& [address, n] : counts) line += " " + address + " " + std::to_string(n);
    REXLOG_INFO("BF dev sends in 5 s:{}", line);
    counts.clear();
    since = now;
  }
}
}  // namespace
#endif

// Ports below 1024 moved on Linux (private_match.h).
namespace bf1943 {
namespace {
[[maybe_unused]] uint16_t Swap16(uint16_t v) { return uint16_t(v >> 8 | v << 8); }
[[maybe_unused]] bool LoopbackIp(uint32_t ip) { return reinterpret_cast<const uint8_t*>(&ip)[0] == 127; }
}  // namespace
uint16_t HostBindPort(uint16_t port) {
#if defined(_WIN32)
  return port;
#else
  const uint16_t p = Swap16(port);
  return p > 0 && p < 1024 ? Swap16(uint16_t(p + kHostPortShift)) : port;
#endif
}
uint16_t HostSendPort(uint32_t ip, uint16_t port) {
#if defined(_WIN32)
  (void)ip;
  return port;
#else
  return LoopbackIp(ip) ? HostBindPort(port) : port;
#endif
}
uint16_t GuestOwnPort(uint16_t port) {
#if defined(_WIN32)
  return port;
#else
  const uint16_t p = Swap16(port);
  return p > kHostPortShift && p < kHostPortShift + 1024 ? Swap16(uint16_t(p - kHostPortShift)) : port;
#endif
}
uint16_t GuestSourcePort(uint32_t ip, uint16_t port) {
#if defined(_WIN32)
  (void)ip;
  return port;
#else
  return LoopbackIp(ip) ? GuestOwnPort(port) : port;
#endif
}
}  // namespace bf1943

// The game's VDP sends (the dedicated server's and the client's game traffic): the runtime's own, with a match this
// PC hosts reached through 127.0.0.1 (private_match.h: the guest's address written for the call, then put back), and
// counted in tests (BF_DEV_SENDS).
extern "C" REX_FUNC(__imp__NetDll_WSASendTo) {
  static PPCFunc* original = RuntimeExport("__imp__NetDll_WSASendTo");
  const uint32_t to = ctx.r9.u32;
  uint32_t ip = 0, guest_ip = 0;
  uint16_t port = 0;
  bool changed = false;
  if (to) {
    std::memcpy(&ip, base + to + 4, 4);
    std::memcpy(&port, base + to + 2, 2);
    guest_ip = ip;
    if (bf1943::SelfHostedSend(ip, port)) std::memcpy(base + to + 4, &ip, 4), changed = true;
    const uint16_t host_port = bf1943::HostSendPort(ip, port);  // Linux: a moved port on this machine
    if (host_port != port) std::memcpy(base + to + 2, &host_port, 2), changed = true;
  }
  const uint32_t handle = ctx.r4.u32;
  original(ctx, base);
  if (TraceOn() && to) {  // where each send went (the first 200)
    static std::atomic<int> traced{0};
    const uint8_t* a = base + to + 4;
    if (traced.fetch_add(1) < 200)
      REXLOG_INFO("BF NET WSASendTo socket {:08X} to {}.{}.{}.{}:{}{} -> {}", handle, a[0], a[1], a[2], a[3],
                  base[to + 2] << 8 | base[to + 3], changed ? " (sent to this host's own address or moved port)" : "",
                  static_cast<int32_t>(ctx.r3.u32));
  }
  if (changed) std::memcpy(base + to + 4, &guest_ip, 4), std::memcpy(base + to + 2, &port, 2);
#if defined(BF_DEV_TOOLS)
  if (int32_t(ctx.r3.u32) >= 0) DevCountSend(base, to);
#endif
}

extern "C" REX_FUNC(__imp__NetDll_sendto) {
  const uint32_t handle = ctx.r4.u32, buf = ctx.r5.u32, length = ctx.r6.u32, to = ctx.r8.u32;
  NativeSocket native;
  if (!LookupNative(handle, &native)) {
    rex::system::XThread::SetLastError(10038);  // WSAENOTSOCK
    ctx.r3.u64 = 0xFFFFFFFFu;
    return;
  }
  sockaddr_in host{};
  if (to) {
    host.sin_family = AF_INET;
    std::memcpy(&host.sin_port, base + to + 2, 2);
    std::memcpy(&host.sin_addr, base + to + 4, 4);
    uint32_t ip = host.sin_addr.s_addr;  // a match this PC hosts: through 127.0.0.1 (private_match.h)
    if (bf1943::SelfHostedSend(ip, host.sin_port)) host.sin_addr.s_addr = ip;
    host.sin_port = bf1943::HostSendPort(host.sin_addr.s_addr, host.sin_port);  // Linux: a moved port here
  }
  const int result = ::sendto(native, reinterpret_cast<const char*>(base + buf), static_cast<int>(length),
                              static_cast<int>(ctx.r7.u32), to ? reinterpret_cast<sockaddr*>(&host) : nullptr,
                              to ? static_cast<socklen_t>(sizeof(host)) : 0);
  uint32_t error = 0;
  if (result < 0) {
    error = HostSocketError();
    rex::system::XThread::SetLastError(error);
  }
  ctx.r3.u64 = static_cast<uint32_t>(result);
#if defined(BF_DEV_TOOLS)
  if (result >= 0) DevCountSend(base, to);
#endif
  std::string extra;
  if (TraceOn()) {
    extra = (to ? " to " + FormatAddress(base + to) : std::string()) +
            FormatData(base + buf, static_cast<int>(length), static_cast<uint32_t>(ctx.lr));
    static std::atomic<int> stacks{0};
    if (stacks.fetch_add(1) < 3) extra += GuestStack(ctx, base);
  }
  TraceResult("sendto", handle, length, extra, ctx.r3.u32, error);
}

extern "C" REX_FUNC(__imp__NetDll_recvfrom) {
  const uint32_t handle = ctx.r4.u32, buf = ctx.r5.u32, length = ctx.r6.u32, from = ctx.r8.u32,
                 from_length = ctx.r9.u32;
  NativeSocket native;
  if (!LookupNative(handle, &native)) {
    rex::system::XThread::SetLastError(10038);  // WSAENOTSOCK
    ctx.r3.u64 = 0xFFFFFFFFu;
    return;
  }
  sockaddr_in host{};
  socklen_t size = sizeof(host);
  const int result = ::recvfrom(native, reinterpret_cast<char*>(base + buf), static_cast<int>(length),
                                static_cast<int>(ctx.r7.u32), reinterpret_cast<sockaddr*>(&host), &size);
  uint32_t error = 0;
  if (result < 0) {
    error = HostSocketError();
    rex::system::XThread::SetLastError(error);
  } else {
    host.sin_port = bf1943::GuestSourcePort(host.sin_addr.s_addr, host.sin_port);  // Linux: a moved port here
    uint32_t ip = host.sin_addr.s_addr;  // a match this PC hosts: its replies read as from its public address
    if (bf1943::SelfHostedReceive(ip, host.sin_port)) host.sin_addr.s_addr = ip;
    if (from) {
      uint8_t* out = base + from;
      std::memset(out, 0, 16);
      out[1] = 2;  // AF_INET, big endian
      std::memcpy(out + 2, &host.sin_port, 2);
      std::memcpy(out + 4, &host.sin_addr, 4);
    }
    if (from_length) {
      uint8_t* len = base + from_length;
      len[0] = len[1] = len[2] = 0;
      len[3] = 16;
    }
  }
  ctx.r3.u64 = static_cast<uint32_t>(result);
  TraceResult("recvfrom", handle, length,
              result >= 0 && from ? " from " + FormatAddress(base + from) +
                                        (TraceOn() ? FormatData(base + buf, result, static_cast<uint32_t>(ctx.lr)) : "")
                                  : std::string(),
              ctx.r3.u32, error);
}

//   recv(socket, buf, len, flags)        send(socket, buf, len, flags)
extern "C" REX_FUNC(__imp__NetDll_recv) {
  const uint32_t handle = ctx.r4.u32, buf = ctx.r5.u32, length = ctx.r6.u32;
  NativeSocket native;
  if (!LookupNative(handle, &native)) {
    rex::system::XThread::SetLastError(10038);  // WSAENOTSOCK
    ctx.r3.u64 = 0xFFFFFFFFu;
    TraceResult("recv", handle, length, " (not a socket)", ctx.r3.u32, 10038);
    return;
  }
  const int result = ::recv(native, reinterpret_cast<char*>(base + buf), static_cast<int>(length),
                            static_cast<int>(ctx.r7.u32));
  const uint32_t error = result < 0 ? HostSocketError() : 0;
  if (result < 0) rex::system::XThread::SetLastError(error);
  ctx.r3.u64 = static_cast<uint32_t>(result);
  TraceResult("recv", handle, length, {}, ctx.r3.u32, error);
}

extern "C" REX_FUNC(__imp__NetDll_send) {
  const uint32_t handle = ctx.r4.u32, buf = ctx.r5.u32, length = ctx.r6.u32;
  NativeSocket native;
  if (!LookupNative(handle, &native)) {
    rex::system::XThread::SetLastError(10038);  // WSAENOTSOCK
    ctx.r3.u64 = 0xFFFFFFFFu;
    TraceResult("send", handle, length, " (not a socket)", ctx.r3.u32, 10038);
    return;
  }
  const int result = ::send(native, reinterpret_cast<const char*>(base + buf), static_cast<int>(length),
                            static_cast<int>(ctx.r7.u32));
  const uint32_t error = result < 0 ? HostSocketError() : 0;
  if (result < 0) rex::system::XThread::SetLastError(error);
  ctx.r3.u64 = static_cast<uint32_t>(result);
  TraceResult("send", handle, length, {}, ctx.r3.u32, error);
}

// Test runs (BF_LOOPBACK_ONLY=1, set by tools\run_game.ps1): bind to 127.0.0.1 instead of every interface, so
// Windows Firewall never asks about the game on an unattended PC. All test traffic is local.
extern "C" REX_FUNC(__imp__NetDll_bind) {
  static PPCFunc* original = RuntimeExport("__imp__NetDll_bind");
  static const bool loopback_only = [] {
    const char* v = std::getenv("BF_LOOPBACK_ONLY");
    return v && *v == '1';
  }();
  const uint32_t addr = ctx.r5.u32;
  // sockaddr_in: family (2 bytes), port (2 bytes), IPv4 address (4 bytes, network order).
  // BF_LOOPBACK_ADDR (join tests with two clients): this client's own loopback address, 127.0.0.1 if unset.
  static const uint32_t loopback = [] {
    const char* v = std::getenv("BF_LOOPBACK_ADDR");
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!v || std::sscanf(v, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0x7F000001u;
    return a << 24 | b << 16 | c << 8 | d;
  }();
  if (loopback_only && addr && base[addr] == 0 && base[addr + 1] == 2 && !base[addr + 4] && !base[addr + 5] &&
      !base[addr + 6] && !base[addr + 7]) {
    base[addr + 4] = uint8_t(loopback >> 24);
    base[addr + 5] = uint8_t(loopback >> 16);
    base[addr + 6] = uint8_t(loopback >> 8);
    base[addr + 7] = uint8_t(loopback);
  }
  const uint32_t handle = ctx.r4.u32;
  // Linux: a port below 1024 moved up (private_match.h), the guest's own sockaddr put back after the call.
  uint16_t guest_port = 0, host_port = 0;
  if (addr) {
    std::memcpy(&guest_port, base + addr + 2, 2);
    host_port = bf1943::HostBindPort(guest_port);
    if (host_port != guest_port) std::memcpy(base + addr + 2, &host_port, 2);
  }
  if (original) original(ctx, base);
  if (addr && host_port != guest_port) {
    std::memcpy(base + addr + 2, &guest_port, 2);
    REXLOG_INFO("BF SOCKET bind {:08X}: port {} is {} on this host (ports below 1024 need root)", handle,
                base[addr + 2] << 8 | base[addr + 3], (host_port & 0xFF) << 8 | host_port >> 8);
  }
  if (addr)
    REXLOG_INFO("BF SOCKET bind {:08X} to {}.{}.{}.{}:{} -> {}", handle, base[addr + 4], base[addr + 5],
                base[addr + 6], base[addr + 7], base[addr + 2] << 8 | base[addr + 3], static_cast<int32_t>(ctx.r3.u32));
}

// getpeername and getsockname are stubs in the runtime (they log STUB and leave the address alone). DirtySock's HTTP
// client calls getpeername after connect to see whether the connection is up, and never reads its response
// without it (the news ticker request in runs\http1, which held the "connected" event back). Ask the host socket
// and copy the address into the guest's sockaddr_in: family (big endian), port and address (network order).
namespace {

void SocketName(PPCContext& ctx, uint8_t* base, bool peer) {
  const uint32_t handle = ctx.r4.u32, name = ctx.r5.u32, length_ptr = ctx.r6.u32;
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XSocket>(handle);
  if (!socket || !name || !length_ptr) {
    rex::system::XThread::SetLastError(socket ? 10014 : 10038);  // WSAEFAULT, WSAENOTSOCK
    ctx.r3.u64 = 0xFFFFFFFFu;
    return;
  }
  sockaddr_in host{};
  socklen_t size = sizeof(host);
  const auto native = static_cast<decltype(::socket(0, 0, 0))>(socket->native_handle());
  const int result = peer ? ::getpeername(native, reinterpret_cast<sockaddr*>(&host), &size)
                          : ::getsockname(native, reinterpret_cast<sockaddr*>(&host), &size);
  if (result != 0) {
    rex::system::XThread::SetLastError(HostSocketError());
    ctx.r3.u64 = 0xFFFFFFFFu;
    return;
  }
  host.sin_port = peer ? bf1943::GuestSourcePort(host.sin_addr.s_addr, host.sin_port) : bf1943::GuestOwnPort(host.sin_port);
  uint8_t* out = base + name;
  std::memset(out, 0, 16);
  out[1] = 2;  // AF_INET, big endian
  std::memcpy(out + 2, &host.sin_port, 2);
  std::memcpy(out + 4, &host.sin_addr, 4);
  uint8_t* len = base + length_ptr;
  len[0] = len[1] = len[2] = 0;
  len[3] = 16;
  ctx.r3.u64 = 0;
}

}  // namespace

extern "C" REX_FUNC(__imp__NetDll_getpeername) { SocketName(ctx, base, true); }
extern "C" REX_FUNC(__imp__NetDll_getsockname) { SocketName(ctx, base, false); }

// WSAWaitForMultipleEvents(count, events, wait_all, timeout ms, alertable). The runtime's version hands wait_all to
// the kernel wait as its wait type, where 0 means "all" (XObject::WaitMultiple: non zero is "any"), passes the
// millisecond timeout as kernel ticks, and always returns 0. DirtySock's socket thread waits for "any" of its wake
// event and the sockets' events with no timeout while holding a lock: with "all" it never wakes, and the main
// thread then blocks on that lock right after the online menu appears (runs qm3, stk1, ev1).
// Here: wait type 1 (any) unless wait_all, the timeout as relative 100 ns ticks, and the kernel's result as the
// WSA result (WSA_WAIT_EVENT_0 + index, WSA_WAIT_TIMEOUT 0x102, WSA_WAIT_IO_COMPLETION 0xC0).
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/kernel/xboxkrnl/threading.h>

extern "C" REX_FUNC(__imp__NetDll_WSAWaitForMultipleEvents) {
  const uint32_t count = ctx.r3.u32, events = ctx.r4.u32, wait_all = ctx.r5.u32, timeout_ms = ctx.r6.u32,
                 alertable = ctx.r7.u32;
  if (count == 0 || count > 64 || !events) {
    rex::system::XThread::SetLastError(87);  // ERROR_INVALID_PARAMETER
    ctx.r3.u64 = 0xFFFFFFFFu;
    return;
  }
  auto* handles = reinterpret_cast<rex::be<uint32_t>*>(base + events);
  uint64_t ticks = static_cast<uint64_t>(-static_cast<int64_t>(timeout_ms) * 10000);
  uint32_t result;
  do {
    result = rex::kernel::xboxkrnl::xeNtWaitForMultipleObjectsEx(count, handles, wait_all ? 0 : 1, 1, alertable,
                                                                 timeout_ms == 0xFFFFFFFFu ? nullptr : &ticks);
  } while (result == 0x101 && !alertable);  // X_STATUS_ALERTED
  if (result < count || result == 0x102 || result == 0xC0) {
    ctx.r3.u64 = result;
    return;
  }
  rex::system::XThread::SetLastError(rex::kernel::xboxkrnl::xeRtlNtStatusToDosError(result));
  ctx.r3.u64 = 0xFFFFFFFFu;
}
