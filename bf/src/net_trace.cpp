// Developer aid (BF_DEV_TOOLS builds with BF_NET_TRACE=1): log the game's network and online imports, arguments in
// and result out, the first 30 calls of each, then call the runtime's own implementation unchanged. Used to see which
// call makes Multiplayer say "Failed to connect to Xbox LIVE". Imports answered in live_layers.cpp are left out
// (two definitions of one import would not link).

#if defined(BF_DEV_TOOLS)

#include <atomic>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xsocket.h>

#include "dev_guest_memory.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
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

bool TraceOn() {
  static const bool on = [] {
    const char* v = std::getenv("BF_NET_TRACE");
    return v && *v && *v != '0';
  }();
  return on;
}

// A guest C string for the log, or "" when the argument does not look like one.
std::string GuestString(const uint8_t* base, uint32_t address) {
  if (address < 0x40000000 || address >= 0xC0000000 || !bf1943::dev::GuestReadable(base, address, 64)) return {};
  std::string s;
  for (uint32_t i = 0; i < 64; ++i) {
    const char c = static_cast<char>(base[address + i]);
    if (!c) return s;
    if (c < 0x20 || c > 0x7E) return {};
    s += c;
  }
  return s;
}

void Trace(PPCFunc* original, const char* name, std::atomic<int>& calls, PPCContext& ctx, uint8_t* base) {
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32, r7 = ctx.r7.u32;
  const uint64_t lr = ctx.lr;
  if (original) original(ctx, base);
  if (!TraceOn()) return;
  const int n = calls.fetch_add(1);
  if (n >= 30) return;
  std::string text;
  for (uint32_t a : {r3, r4, r5}) {
    if (std::string s = GuestString(base, a); !s.empty()) text += " \"" + s + "\"";
  }
  // sockaddr_in (big endian): family, port, address. connect and bind take it in r5, sendto in r8 (not logged).
  const std::string_view fn = name;
  if ((fn == "NetDll_connect" || fn == "NetDll_bind") && bf1943::dev::GuestReadable(base, r5, 8)) {
    const uint8_t* a = base + r5;
    text += fmt::format(" addr {}.{}.{}.{}:{}", a[4], a[5], a[6], a[7], (a[2] << 8) | a[3]);
  }
  REXLOG_INFO("BF NET {} #{} (lr {:08X}) r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X} -> {:08X}{}", name, n + 1,
              static_cast<uint32_t>(lr), r3, r4, r5, r6, r7, ctx.r3.u32, text);
}

}  // namespace

#define TRACE_IMPORT(name)                                          \
  extern "C" REX_FUNC(__imp__##name) {                              \
    static PPCFunc* original = RuntimeExport("__imp__" #name);      \
    static std::atomic<int> calls{0};                               \
    Trace(original, #name, calls, ctx, base);                       \
  }

TRACE_IMPORT(NetDll_XNetStartup)
TRACE_IMPORT(NetDll_XNetCleanup)
TRACE_IMPORT(NetDll_XNetDnsLookup)
TRACE_IMPORT(NetDll_XNetDnsRelease)
TRACE_IMPORT(NetDll_XNetServerToInAddr)
TRACE_IMPORT(NetDll_XNetCreateKey)
TRACE_IMPORT(NetDll_XNetRegisterKey)
TRACE_IMPORT(NetDll_XNetUnregisterKey)
TRACE_IMPORT(NetDll_XNetQosLookup)
TRACE_IMPORT(NetDll_XNetQosServiceLookup)
TRACE_IMPORT(NetDll_XNetQosListen)
TRACE_IMPORT(NetDll_XNetQosRelease)
TRACE_IMPORT(NetDll_XNetRandom)
TRACE_IMPORT(NetDll_WSAStartup)
TRACE_IMPORT(NetDll_connect)
TRACE_IMPORT(NetDll_listen)
TRACE_IMPORT(NetDll_setsockopt)
TRACE_IMPORT(NetDll_ioctlsocket)
TRACE_IMPORT(NetDll_select)
TRACE_IMPORT(NetDll_WSAGetLastError)
TRACE_IMPORT(XNetLogonGetTitleID)
TRACE_IMPORT(XNetLogonGetMachineID)
TRACE_IMPORT(XamSessionCreateHandle)
TRACE_IMPORT(XamSessionRefObjByHandle)
TRACE_IMPORT(XamShowSigninUI)
TRACE_IMPORT(XMsgStartIORequest)
TRACE_IMPORT(XMsgStartIORequestEx)
TRACE_IMPORT(XMsgCancelIORequest)
TRACE_IMPORT(XamCreateEnumeratorHandle)
TRACE_IMPORT(XamEnumerate)
TRACE_IMPORT(XamGetPrivateEnumStructureFromHandle)
TRACE_IMPORT(XamTaskSchedule)
TRACE_IMPORT(NetDll_WSACreateEvent)
TRACE_IMPORT(NetDll_WSASetEvent)
TRACE_IMPORT(NetDll_WSAResetEvent)
TRACE_IMPORT(NetDll_WSARecv)

// Handles the game got from socket(), until closesocket. NtClose on one of them, whatever it holds by then, means the
// game closed a handle twice (run ded5: the FESL socket's handle came back as a thread while a level loaded).
std::mutex socket_handles_mutex;
std::set<uint32_t> socket_handles;

std::string GuestStackText(const PPCContext& ctx, const uint8_t* base) {
  std::string stack;
  uint32_t sp = ctx.r1.u32;
  for (int i = 0; i < 12 && sp >= 0x70000000 && sp < 0x7FFFFFF0; ++i) {
    const uint32_t back = bf1943::dev::SafeLoad32(base, sp);
    if (back <= sp || back >= 0x80000000) break;
    stack += fmt::format(" {:08X}", bf1943::dev::SafeLoad32(base, back - 8));
    sp = back;
  }
  return stack;
}

extern "C" REX_FUNC(__imp__NetDll_socket) {
  static PPCFunc* original = RuntimeExport("__imp__NetDll_socket");
  static std::atomic<int> calls{0};
  Trace(original, "NetDll_socket", calls, ctx, base);
  std::lock_guard<std::mutex> lock(socket_handles_mutex);
  if (ctx.r3.u32 != 0xFFFFFFFFu) socket_handles.insert(ctx.r3.u32);
}

extern "C" REX_FUNC(__imp__NetDll_closesocket) {
  static PPCFunc* original = RuntimeExport("__imp__NetDll_closesocket");
  static std::atomic<int> calls{0};
  const uint32_t handle = ctx.r4.u32;
  Trace(original, "NetDll_closesocket", calls, ctx, base);
  std::lock_guard<std::mutex> lock(socket_handles_mutex);
  socket_handles.erase(handle);
}

bool HandleTraceOn() {
  static const bool on = [] {
    const char* v = std::getenv("BF_HANDLE_TRACE");
    return v && *v && *v != '0';
  }();
  return on;
}

std::string ObjectText(uint32_t handle) {
  auto object = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XObject>(handle);
  return object ? fmt::format("type {}", static_cast<uint32_t>(object->type())) : "nothing";
}

// BF_HANDLE_TRACE=1: a host thread checks the live socket handles every 2 ms and logs the moment one stops naming
// a socket, next to every NtClose, ObDereferenceObject and ExCreateThread in the log.
void StartSocketWatch() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::set<uint32_t> handles;
        {
          std::lock_guard<std::mutex> lock(socket_handles_mutex);
          handles = socket_handles;
        }
        for (uint32_t h : handles) {
          if (REX_KERNEL_OBJECTS()->LookupObject<rex::system::XSocket>(h)) continue;
          REXLOG_INFO("BF HANDLE socket {:08X} gone, the handle holds {}", h, ObjectText(h));
          std::lock_guard<std::mutex> lock(socket_handles_mutex);
          socket_handles.erase(h);
        }
      }
    }).detach();
  });
}

extern "C" REX_FUNC(__imp__ExCreateThread) {
  static PPCFunc* original = RuntimeExport("__imp__ExCreateThread");
  const uint32_t handle_ptr = ctx.r3.u32, start = ctx.r7.u32;
  const uint64_t lr = ctx.lr;
  if (original) original(ctx, base);
  if (HandleTraceOn())
    REXLOG_INFO("BF HANDLE ExCreateThread start {:08X} -> handle {:08X} (lr {:08X})", start,
                handle_ptr ? bf1943::dev::SafeLoad32(base, handle_ptr) : 0, static_cast<uint32_t>(lr));
}

extern "C" REX_FUNC(__imp__NtClose) {
  static PPCFunc* original = RuntimeExport("__imp__NtClose");
  const uint32_t handle = ctx.r3.u32;
  if (HandleTraceOn()) {
    StartSocketWatch();
    REXLOG_INFO("BF HANDLE NtClose {:08X} holding {} (lr {:08X})", handle, ObjectText(handle),
                static_cast<uint32_t>(ctx.lr));
  }
  if (TraceOn()) {
    bool tracked;
    {
      std::lock_guard<std::mutex> lock(socket_handles_mutex);
      tracked = socket_handles.count(handle) != 0;
    }
    if (tracked) {
      auto object = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XObject>(handle);
      REXLOG_INFO("BF NET NtClose on socket handle {:08X} holding {} (lr {:08X}) stack:{}", handle,
                  object ? fmt::format("type {}", static_cast<uint32_t>(object->type())) : "nothing",
                  static_cast<uint32_t>(ctx.lr), GuestStackText(ctx, base));
    }
  }
  if (original) original(ctx, base);
}

// WSACloseEvent releases whatever handle it gets; on a socket handle that would kill the socket.
extern "C" REX_FUNC(__imp__NetDll_WSACloseEvent) {
  static PPCFunc* original = RuntimeExport("__imp__NetDll_WSACloseEvent");
  if (TraceOn() && REX_KERNEL_OBJECTS()->LookupObject<rex::system::XSocket>(ctx.r3.u32))
    REXLOG_INFO("BF NET WSACloseEvent on socket {:08X} (lr {:08X})", ctx.r3.u32, static_cast<uint32_t>(ctx.lr));
  if (original) original(ctx, base);
}

#endif  // BF_DEV_TOOLS
