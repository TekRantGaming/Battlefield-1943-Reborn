// WSARecvFrom and WSAGetOverlappedResult. Both are stubs in the runtime (WSARecvFrom always fails, the other is
// REX_EXPORT_STUB), and DirtySock reads its UDP sockets only through them: the client's game socket (VDP, bound to
// port 1000) never received anything (run join2).
//
// DirtySock's socket thread (sub_824960E0, sub_82495FF0) per socket record:
//   WSARecvFrom(socket, &wsabuf, 1, &bytes, &flags, &from, &fromlen, &overlapped, 0)
//     0                    data is in the buffer, bytes says how much
//     -1, error 997        WSA_IO_PENDING: it then waits for overlapped.hEvent with its other events
//   WSAGetOverlappedResult(socket, &overlapped, &bytes, wait 0, &flags)
//     1                    done, bytes filled
//     0, error 996         WSA_IO_INCOMPLETE: still waiting
// Here a receive that would block is handed to one host worker thread, which waits for the socket, receives into
// the guest's buffers, fills the address the guest gave, and sets the guest event. The sender's port and address
// are copied in network order as they are (the runtime's own recvfrom swaps the port, see socket_fixes.cpp).
//
// Guest arguments: r3 is the caller id (1), then the Winsock arguments from r4; WSARecvFrom's 9th and 10th
// arguments (overlapped, completion routine) are on the stack at r1+0x54 and r1+0x5C.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>

#include "private_match.h"  // a match this PC hosts, reached through 127.0.0.1

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <cerrno>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

namespace {

using NativeSocket = decltype(::socket(0, 0, 0));

constexpr uint32_t kWouldBlock = 10035, kIoPending = 997, kIoIncomplete = 996, kNotSocket = 10038;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  uint8_t* p = base + address;
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

bool TraceOn() {
  static const bool on = [] {
    const char* v = std::getenv("BF_NET_TRACE");
    return v && *v && *v != '0';
  }();
  return on;
}

uint32_t LastSocketError() {
#if defined(_WIN32)
  return static_cast<uint32_t>(WSAGetLastError());
#else
  switch (errno) {
    case EAGAIN: return kWouldBlock;
    case ECONNRESET: return 10054;
    case ECONNREFUSED: return 10061;
    default: return 10022;
  }
#endif
}

// One receive: the guest's buffers (WSABUF: length, then pointer), where to put the sender and the byte count.
struct Receive {
  NativeSocket native;
  uint32_t socket = 0;
  std::vector<std::pair<uint32_t, uint32_t>> buffers;  // guest pointer, length
  uint32_t from = 0, from_length = 0, overlapped = 0, event = 0;
  bool done = false;
  uint32_t bytes = 0;
};

// Receives once without blocking. Returns the byte count, or -1 with the Winsock error. On Windows a UDP socket
// reports WSAECONNRESET after an ICMP port unreachable for an earlier send; that is not a receive error here.
int ReceiveNow(Receive& r, uint8_t* base, uint32_t* error) {
  size_t total = 0;
  for (auto& b : r.buffers) total += b.second;
  std::vector<uint8_t> data(total ? total : 1);
  for (;;) {
    sockaddr_in host{};
    socklen_t size = sizeof(host);
    const int n = ::recvfrom(r.native, reinterpret_cast<char*>(data.data()), static_cast<int>(total), 0,
                             reinterpret_cast<sockaddr*>(&host), &size);
    if (n < 0) {
      *error = LastSocketError();
      if (*error == 10054) continue;
      return -1;
    }
    size_t at = 0;
    for (auto& b : r.buffers) {
      const size_t part = std::min<size_t>(b.second, static_cast<size_t>(n) - at);
      if (part) std::memcpy(base + b.first, data.data() + at, part);
      at += part;
    }
    host.sin_port = bf1943::GuestSourcePort(host.sin_addr.s_addr, host.sin_port);  // Linux: a moved port here
    uint32_t ip = host.sin_addr.s_addr;  // a match this PC hosts: its replies read as from its public address
    if (bf1943::SelfHostedReceive(ip, host.sin_port)) host.sin_addr.s_addr = ip;
    if (r.from) {
      uint8_t* out = base + r.from;
      std::memset(out, 0, 16);
      out[1] = 2;  // AF_INET, big endian
      std::memcpy(out + 2, &host.sin_port, 2);
      std::memcpy(out + 4, &host.sin_addr, 4);
    }
    if (r.from_length) Store32(base, r.from_length, 16);
    if (TraceOn()) {
      const uint8_t* a = reinterpret_cast<const uint8_t*>(&host.sin_addr);
      const uint8_t* p = reinterpret_cast<const uint8_t*>(&host.sin_port);
      static const bool dump = [] {
        const char* v = std::getenv("BF_NET_DUMP");
        return v && *v && *v != '0';
      }();
      std::string bytes;
      if (dump)
        for (int i = 0; i < n && i < 4096; ++i) bytes += fmt::format("{}{:02X}", i ? "" : " data ", data[i]);
      REXLOG_INFO("BF NET WSARecvFrom socket {:08X} {} bytes from {}.{}.{}.{}:{}{}", r.socket, n, a[0], a[1], a[2],
                  a[3], p[0] << 8 | p[1], bytes);
    }
    return n;
  }
}

void SetGuestEvent(uint32_t handle) {
  if (!handle) return;
  if (auto ev = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(handle)) ev->Set(0, false);
}

// Overlapped receives waiting for data, by guest OVERLAPPED address.
struct Pending {
  std::mutex mutex;
  std::condition_variable wake;
  std::map<uint32_t, Receive> receives;
  uint8_t* base = nullptr;
  bool started = false;
};

Pending& pending() {
  static Pending p;
  return p;
}

void Complete(Receive& r, uint8_t* base, uint32_t bytes) {
  r.done = true;
  r.bytes = bytes;
  Store32(base, r.overlapped, 0);           // Internal: status
  Store32(base, r.overlapped + 4, bytes);   // InternalHigh: bytes transferred
  SetGuestEvent(r.event);
}

void Worker() {
  auto& p = pending();
  std::unique_lock<std::mutex> lock(p.mutex);
  for (;;) {
    std::vector<NativeSocket> sockets;
    for (auto& [ovl, r] : p.receives)
      if (!r.done) sockets.push_back(r.native);
    if (sockets.empty()) {
      p.wake.wait(lock);
      continue;
    }
    fd_set readable;
    FD_ZERO(&readable);
    NativeSocket top = 0;
    for (auto s : sockets) {
      FD_SET(s, &readable);
      if (s > top) top = s;
    }
    lock.unlock();
    timeval timeout{0, 20000};  // 20 ms: also picks up receives added meanwhile
    const int ready = ::select(static_cast<int>(top + 1), &readable, nullptr, nullptr, &timeout);
    lock.lock();
    if (ready <= 0) continue;
    for (auto& [ovl, r] : p.receives) {
      if (r.done || !FD_ISSET(r.native, &readable)) continue;
      uint32_t error = 0;
      const int n = ReceiveNow(r, p.base, &error);
      if (n >= 0) Complete(r, p.base, static_cast<uint32_t>(n));
    }
  }
}

}  // namespace

extern "C" REX_FUNC(__imp__NetDll_WSARecvFrom) {
  const uint32_t handle = ctx.r4.u32, buffers = ctx.r5.u32, count = ctx.r6.u32, bytes_ptr = ctx.r7.u32,
                 flags_ptr = ctx.r8.u32;
  const uint32_t overlapped = Load32(base, ctx.r1.u32 + 0x54);
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XSocket>(handle);
  if (!socket) {
    rex::system::XThread::SetLastError(kNotSocket);
    ctx.r3.u64 = 0xFFFFFFFFu;
    return;
  }
  Receive r;
  r.native = static_cast<NativeSocket>(socket->native_handle());
  r.socket = handle;
  for (uint32_t i = 0; i < count && i < 16; ++i)
    r.buffers.emplace_back(Load32(base, buffers + i * 8 + 4), Load32(base, buffers + i * 8));
  r.from = ctx.r9.u32;
  r.from_length = ctx.r10.u32;
  r.overlapped = overlapped;
  r.event = overlapped ? Load32(base, overlapped + 0x10) : 0;

  uint32_t error = 0;
  const int n = ReceiveNow(r, base, &error);
  if (n >= 0) {
    if (bytes_ptr) Store32(base, bytes_ptr, static_cast<uint32_t>(n));
    if (flags_ptr) Store32(base, flags_ptr, 0);
    if (overlapped) {
      Store32(base, overlapped, 0);
      Store32(base, overlapped + 4, static_cast<uint32_t>(n));
      SetGuestEvent(r.event);
    }
    ctx.r3.u64 = 0;
    return;
  }
  if (error != kWouldBlock || !overlapped) {
    rex::system::XThread::SetLastError(error);
    ctx.r3.u64 = 0xFFFFFFFFu;
    return;
  }
  auto& p = pending();
  {
    std::lock_guard<std::mutex> lock(p.mutex);
    p.base = base;
    p.receives[overlapped] = std::move(r);
    if (!p.started) {
      p.started = true;
      std::thread(Worker).detach();
    }
  }
  p.wake.notify_one();
  rex::system::XThread::SetLastError(kIoPending);
  ctx.r3.u64 = 0xFFFFFFFFu;
}

extern "C" REX_FUNC(__imp__NetDll_WSAGetOverlappedResult) {
  const uint32_t overlapped = ctx.r5.u32, bytes_ptr = ctx.r6.u32, wait = ctx.r7.u32, flags_ptr = ctx.r8.u32;
  auto& p = pending();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(wait ? 3600 : 0);
  std::unique_lock<std::mutex> lock(p.mutex);
  for (;;) {
    auto it = p.receives.find(overlapped);
    if (it == p.receives.end()) {
      // Not one of ours (or already collected): report what the OVERLAPPED says.
      if (bytes_ptr) Store32(base, bytes_ptr, Load32(base, overlapped + 4));
      if (flags_ptr) Store32(base, flags_ptr, 0);
      ctx.r3.u64 = 1;
      return;
    }
    if (it->second.done) {
      if (bytes_ptr) Store32(base, bytes_ptr, it->second.bytes);
      if (flags_ptr) Store32(base, flags_ptr, 0);
      p.receives.erase(it);
      ctx.r3.u64 = 1;
      return;
    }
    if (std::chrono::steady_clock::now() >= deadline) break;
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    lock.lock();
  }
  rex::system::XThread::SetLastError(kIoIncomplete);
  ctx.r3.u64 = 0;
}
