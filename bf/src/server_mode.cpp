// Dedicated server mode: --server starts this exe as a game server instead of a client.
//
// The game's main (sub_82397908) always builds its game object, sub_82396C48(game, is_server, callback), with
// is_server 0. With is_server set, the constructor skips the client setup and starts ServerMain at once
// (sub_823779B8, then sub_8240CBD8) on the level from Server.Level. The server code is all there in the client
// exe; only the flag is never passed.

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(server, false, "BF1943", "Run as a dedicated game server instead of a client");

REX_EXTERN(__imp__sub_82396C48);
REX_HOOK_RAW(sub_82396C48) {
  if (REXCVAR_GET(server)) {
    REXLOG_INFO("BF SERVER starting as a dedicated server");
    ctx.r4.u64 = 1;
  }
  __imp__sub_82396C48(ctx, base);
}

// Frostbite's game socket, opened by sub_82609EB8(socket, address, ...). Flags in the byte at +0xB0: 0x40 makes it
// an Xbox VDP socket (protocol 254), where every datagram starts with a 2 byte big endian length of the game data:
// sends prepend it (sub_82609B08) and receives strip it (sub_8260FD80). 0x20 keeps one socket (without it a VDP
// socket also opens a plain UDP one on the next port). Clients always send VDP through DirtySock (run join4:
// "0016..." for 22 bytes), but the dedicated server opens a plain UDP socket and reads the length as game data. A
// console hosting a game for consoles uses VDP, so the server does too.
REX_EXTERN(__imp__sub_82609EB8);
REX_HOOK_RAW(sub_82609EB8) {
  if (REXCVAR_GET(server)) {
    base[ctx.r3.u32 + 0xB0] |= 0x60;
    REXLOG_INFO("BF SERVER game socket {:08X}: VDP", ctx.r3.u32);
  }
  __imp__sub_82609EB8(ctx, base);
}

// The server's network host (sub_823BD870(host, server, 0), made once by sub_82413BC0 with the server object): its
// timeouts for a client that sends nothing, +556 while it loads a level and +560 in the match (seconds). The game
// reads them from Server.LoadingTimeout and Server.IngameTimeout, except when the server object's byte +30 is set:
// then both are 172800 (48 hours, image 0x8201C8F4). Our dedicated server got that, so a player whose game was closed
// or killed stayed in the match for two days (TekRantGaming, 9 Nov: "a copy of me still exists, even other account names I
// used for testing", the player cap filling with players who have quit). Capped here, the game's own timeout then
// takes them out of the match the way it does a player who leaves.
REXCVAR_DEFINE_DOUBLE(bf_server_ingame_timeout, 30.0, "BF1943",
                      "Dedicated server: seconds a player in the match may send nothing before the server drops them");
REXCVAR_DEFINE_DOUBLE(bf_server_loading_timeout, 180.0, "BF1943",
                      "Dedicated server: seconds a player loading the level may send nothing before the server drops "
                      "them");

namespace {
float LoadGuestFloat(const uint8_t* base, uint32_t a) {
  const uint32_t v = uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void StoreGuestFloat(uint8_t* base, uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  base[a] = uint8_t(v >> 24), base[a + 1] = uint8_t(v >> 16), base[a + 2] = uint8_t(v >> 8), base[a + 3] = uint8_t(v);
}
}  // namespace

REX_EXTERN(__imp__sub_823BD870);
REX_HOOK_RAW(sub_823BD870) {
  const uint32_t host = ctx.r3.u32, server = ctx.r4.u32;
  __imp__sub_823BD870(ctx, base);
  if (!REXCVAR_GET(server) || !host) return;
  const float loading = LoadGuestFloat(base, host + 556), ingame = LoadGuestFloat(base, host + 560);
  const float cap_loading = float(std::clamp(REXCVAR_GET(bf_server_loading_timeout), 10.0, 172800.0));
  const float cap_ingame = float(std::clamp(REXCVAR_GET(bf_server_ingame_timeout), 5.0, 172800.0));
  if (!(loading <= cap_loading)) StoreGuestFloat(base, host + 556, cap_loading);
  if (!(ingame <= cap_ingame)) StoreGuestFloat(base, host + 560, cap_ingame);
  REXLOG_INFO("BF SERVER network host {:08X} (the server's byte +30: {}): a silent player dropped after {:.0f} s in the "
              "match, {:.0f} s loading (the game's own {:.0f} and {:.0f})",
              host, server ? base[server + 30] : 0, LoadGuestFloat(base, host + 560), LoadGuestFloat(base, host + 556),
              ingame, loading);
}

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <thread>

namespace bf1943 {

// A dedicated server's window stays hidden: nobody plays in it, and shown it took the focus from the game on the same
// PC (TekRantGaming, 5 Nov). The runtime makes the window; this hides every window of the process, and again whenever one
// shows up (a hidden SDL window still runs the game; the server keeps its timing and network).
void HideServerWindows() {
  if (!REXCVAR_GET(server)) return;
  std::thread([] {
    for (;;) {
      EnumWindows(
          [](HWND hwnd, LPARAM) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
            return TRUE;
          },
          0);
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }).detach();
}

}  // namespace bf1943
#else
namespace bf1943 {
void HideServerWindows() {}
}  // namespace bf1943
#endif
