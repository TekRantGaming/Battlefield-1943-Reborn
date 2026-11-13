// Our Create Private Match (docs/MENUS-PLAN.md section 7): a dedicated server of our own started by the player's game
// on this PC, with our GameManager host inside it (gm_host.cpp). private_match.cpp has the details.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bf1943 {

struct PrivateMatchOptions {
  std::vector<std::string> maps;  // in turn, by key: wake, guadalcanal, iwo, coral
  bool upnp = true;               // ask the router to forward the match's port (UPnP)
  std::string name;               // its name in the backend's list
  // 13 Nov 2026, Network type LAN: the match's server answers the sign-in and matchmaking itself (lan_backend.cpp),
  // for this network's players, with no internet: nothing is registered with the main backend, no router is asked.
  bool lan = false;
};

struct PrivateMatchStatus {
  bool running = false;  // our server process is alive
  std::string state;     // the host's: starting, registered, no backend, port in use, refused, reconnecting; "" none yet
  std::string gid, address, map, why;
  std::string reachable;  // "true", "false" or "null" (not known yet)
  int players = 0;
  uint16_t port = 0;  // the UDP port players reach it on
};

// The game's folders as the app resolved them (bf1943_app.h): the server reads the same game, keeps its own user folder.
void NotePrivateMatchPaths(const std::filesystem::path& game, const std::filesystem::path& user);
// Starts the server (an earlier one is stopped first); false with the reason when it cannot start.
bool StartPrivateMatch(const PrivateMatchOptions& options, std::string* why);
void StopPrivateMatch();
PrivateMatchStatus ReadPrivateMatchStatus();

// This PC's address on its network (the one other PCs of the network reach it at), or empty when it has none.
std::string LanAddress();

// A match this PC hosts: the backend gives joiners the address it saw the host register from, this PC's own public
// one, and many home routers do not loop a PC's packets to its own public address back in. Our socket calls
// (socket_fixes.cpp, socket_overlapped.cpp) send this game's packets for that address and port to 127.0.0.1 instead,
// and what comes back from 127.0.0.1 on that port reads as from that address. Addresses and ports in network order,
// as in a sockaddr_in.
void SetSelfHostedAddress(uint32_t ip, uint16_t port);
bool SelfHostedSend(uint32_t& ip, uint16_t port);     // ip changed to loopback when it is our own match
bool SelfHostedReceive(uint32_t& ip, uint16_t port);  // ip changed back to the public one

// Linux: ports below 1024 need root there, and the console's game sockets use them (a player's 1000, a server's 1003),
// so a guest bind to such a port gets one kHostPortShift higher on the host (socket_fixes.cpp). Traffic on this
// machine follows: a send to such a port on 127.x goes to the moved one, and a packet from a moved port on 127.x reads
// as from the guest's own. Other machines are not affected (they answer the port a packet came from). Identity on
// Windows. Ports and addresses in network order, as in a sockaddr_in.
constexpr uint16_t kHostPortShift = 30000;
uint16_t HostBindPort(uint16_t port);                  // the host port a guest bind to `port` gets
uint16_t HostSendPort(uint32_t ip, uint16_t port);     // a send's destination port
uint16_t GuestSourcePort(uint32_t ip, uint16_t port);  // a received packet's source port as the guest sees it
uint16_t GuestOwnPort(uint16_t port);                  // the socket's own port (getsockname) as the guest sees it

}  // namespace bf1943
