# Hosting a server

The server download runs a full Battlefield 1943 online setup on one Windows PC:

- **The backend** (`server\plasma.py`): stands in for EA's retired online service. Players log in to it, it lists the
  servers, and it sends them to a game.
- **One dedicated server per map** (`bin\bf1943.exe --server`): the game itself running as a server for Wake Island,
  Guadalcanal, Iwo Jima and Coral Sea. Each has its host (`server\gmhost.py`), which links the players to it.

## What you need

- Windows 10 or 11, 64 bit.
- [Python 3.12](https://www.python.org/downloads/), with "Add python.exe to PATH" ticked when you install it.
- Your own copy of Battlefield 1943 for Xbox 360: the server runs the game's own maps and rules.

## Start it

1. Unzip `BF1943-Reborn-Server-<version>-windows.zip` anywhere.
2. Put your game files in the `game` folder (see [the player guide](../README.md) for how to get them out of the
   game's package).
3. Run `Start servers.bat`. It starts the backend and the four map servers in the background and writes their logs
   to `logs`.
4. Run `Stop servers.bat` to stop them all.

## Today's limits

- **Same PC only for now.** The servers listen on this PC's own addresses (127.0.0.x), so players on this PC can join.
  Hosting for other PCs on your network or the internet (public addresses, ports to open, the backend's address in
  the game's settings) is the next step and is not in this package yet.
- One match per map at a time, up to 24 players each.
