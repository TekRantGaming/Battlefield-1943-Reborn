# Hosting a server

The server download runs Battlefield 1943 dedicated servers on a Windows PC, with no window and no graphics card
needed once they are set up. Your servers show in every player's in game server browser under Community, each with
its name, its map, its players, its region and its tick rate.

- **The server** (`bin\bf1943.exe --server`): the game itself running as a dedicated server, playing your maps in
  turn. When a round ends, the next map loads after the end of round screen, and the players stay.
- **Its host** (`server\gmhost.py`): links the players to the server, keeps it listed and passes on voice and text
  chat.
- **The server manager** (`server\manager.py`): a page in your browser. It installs the game, runs any number of
  servers and starts one again if it stops, sends messages to the players, and installs new releases.
- **The main backend** (`server\plasma.py`, the official servers only): where every player logs in and finds the
  servers. Everyone else's servers register with the official one.

## What you need

- Windows 10, 11 or Windows Server, 64 bit. Nothing else to install: the download brings its own Python.
- Your own copy of Battlefield 1943 for Xbox 360: its package file (the single file in `5841097E\000D0000` on your
  console's storage). It is installed once.

## Set it up

1. Unzip `BF1943-Reborn-Server-<version>-windows.zip` anywhere you can write to.
2. Open **BF1943 Server.bat**: the server manager opens in your browser (`http://127.0.0.1:18280/`).
3. **Game files**: install the game from your package. It is unpacked into `game`, once.
4. **Servers**: each has its own name, description, maps (played in that order), players and port. **Start all**.
5. **Network**: pick your region; players see it with its flag. Leave **Open the ports** on behind a home router.
   **LAN only** keeps your servers off the main list: players then join with Direct connect, by this PC's address.
6. **Messages**: tell everyone on a server about an update or a restart, with a countdown, or type your own.

The manager keeps running in the background, with no window, and starts a server again if it stops. **Stop
servers.bat** ends everything. **Start servers (no window).bat** starts them with the saved settings, for a PC that
starts its servers by itself. The page answers only on the PC itself, unless you set a password for it and allow
other machines.

## Updates

Players' games are always on the newest release (the main backend lets no older one in), so keep your servers on it
too. The manager looks for a new release by itself, tells the players on your servers with a countdown, installs it
and starts the servers again. Your `server.json`, your game files and your logs stay.

## Ports

| Port | For | Who needs it open |
|---|---|---|
| UDP 18300, 18301 and on (one for each server) | the servers (players join here) | every host |
| TCP 18241, 18246, 18250 | the main backend (login, games, server list) | the official servers only |

Behind a home router the manager asks the router to open the servers' ports itself (UPnP). If the router does not
allow it, the page says which ports to open by hand. On a hosted server, run **Open firewall ports.bat** once, and
open the same ports in your host's own firewall.
