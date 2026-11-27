/**
 * @file        xam/net_hooks.h
 * @brief       A title's hooks on the addresses its sockets use
 *
 * @license     BSD 3-Clause License
 *
 * @remarks     Battlefield 1943 Reborn's patch of the SDK. A title can change where its sockets send, bind and appear
 *              to be, and drop datagrams before the guest reads them, in every path that reaches the host socket
 *              (send, sendto and WSASendTo; recvfrom and WSARecvFrom, overlapped or not; bind; getsockname and
 *              getpeername). Addresses and ports are the host's, in network byte order.
 */

#pragma once

#include <rex/platform.h>
#include <rex/types.h>

namespace rex::xam {

struct NetHooks {
  /** Before a datagram is sent: may change where it goes. */
  void (*send)(u32& address, u16& port) = nullptr;
  /** After a datagram arrived: false drops it before the guest sees it; may change the sender the guest sees. */
  bool (*receive)(u32& address, u16& port) = nullptr;
  /** Before a bind: may change the address and port bound. */
  void (*bind)(u32& address, u16& port) = nullptr;
  /** What getsockname and getpeername report: may change it. peer is false for getsockname. */
  void (*name)(u32& address, u16& port, bool peer) = nullptr;
};

/** Sets the hooks, replacing any before. Each may be null. Set before the title's first socket call. */
REX_XAM_API void SetNetHooks(const NetHooks& hooks);

}  // namespace rex::xam
