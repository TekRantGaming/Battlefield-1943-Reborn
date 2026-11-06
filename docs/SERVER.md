# Hosting a server

The server download runs a Battlefield 1943 dedicated server on a Windows PC, with no window and no graphics card
needed once it is set up. Its server shows in every player's in game server browser, with its name, its map and its
players.

- **The server** (`bin\bf1943.exe --server`): the game itself running as a dedicated server, playing your maps in
  turn. When a round ends, the next map loads after the end of round screen, and the players stay.
- **Its host** (`server\gmhost.py`): links the players to the server and keeps it listed.
- **The main backend** (`server\plasma.py`, the official servers only): where every player logs in and finds the
  servers. Everyone else's servers register with the official one.

## What you need

- Windows 10, 11 or Windows Server, 64 bit.
- [Python 3.12](https://www.python.org/downloads/), with "Add python.exe to PATH" ticked when you install it.
- Your own copy of Battlefield 1943 for Xbox 360: its package file (the single file in `5841097E\000D0000` on your
  console's storage). It is installed once.

## Set it up

1. Unzip `BF1943-Reborn-Server-<version>-windows.zip` anywhere.
2. Open **BF1943 Server.bat**. In its window:
   - **Install the game from your package**: select the package file. It is unpacked into `game`, once.
   - **Server name**: what players see in the server browser.
   - **Map rotation**: tick the maps, played in that order.
   - **Main backend**: leave it on the official servers' address. Only the official servers run it themselves.
   - **Public address**: press Detect (or type it). Leave **Open the ports on my router** on behind a home router.
3. Press **Start servers**. The server runs in the background; the window can be closed. **Stop servers** ends it.

Later, **Start servers (no window).bat** starts it with the saved settings, for a PC that starts it by itself.

## Ports

| Port | For | Who needs it open |
|---|---|---|
| UDP 18300 | the server (players join here) | every host |
| TCP 18241, 18246, 18250 | the main backend (login, games, server list) | the official servers only |

Behind a home router the server asks the router to open UDP 18300 itself (UPnP). If the router does not allow it,
the window says which port to open by hand. On a hosted server, run **Open firewall ports.bat** once, and open the
same ports in your host's own firewall.
