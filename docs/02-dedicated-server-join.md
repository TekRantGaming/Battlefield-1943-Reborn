# Joining a dedicated server (4 Nov 2026)

How a BF1943 client joins a game on a dedicated server, and what our side (`server/gmhost.py`, `server/plasma.py`)
answers. Verified end to end in run ded10: one client, Quick Match, Wake Island, spawned on the USMC carrier.

## The pieces
- Backend (`server/plasma.py`): FESL login, Theater. `--dedicated GWHOST:GWPORT=SERVERHOST:SERVERPORT` lists a game
  whose host address (EGEG `I`, `P`, `XNADDR`) is our GameManager host, and tells that host about each joiner
  (ticket, PID, name, XUID, XNADDR) before EGEG goes out.
- GameManager host (`server/gmhost.py`): plays the console host's part of EA's online layer for one game. Console
  hosts and EA's dedicated servers ran this inside the game; the dedicated server mode of this exe does not. Run on
  its own (`python server/gmhost.py --backend HOST:18250 --listen 0.0.0.0:1000 --public IP:1000 --game-server
  127.0.0.1:1003 --name ...`) it registers with the backend's server registry and stays listed while connected.
- Server registry (`plasma.py` port 18250, class `Registry`): one JSON object per line. A server sends `register`
  (name, address, map, max_players) and gets `registered` (gid and the host's uid and pid); it sends `status`
  (players) every 5 s; the backend sends `join` (ticket, pid, name, xuid, xnaddr) before a joiner's EGEG. `list`
  returns every server with name, address, map and players: the data our menu's server browser will show.
- Dedicated server: `bf1943.exe --server --cl=-dedicated`, the game's own Frostbite server on UDP 1003 (socket
  made VDP by `bf/src/server_mode.cpp`).

## Client flow
1. Quick Match: FESL `pnow` gives JOIN with the game; Theater GDAT, EGAM, EGEG (`PL`, `TICKET`, `PID`, `HUID`, `I`,
   `P`, `XNADDR`, `XNKID`, `XNKEY`, `EKEY`).
2. GameManager makes the host connection: an EA tunnel to `I:P`, its ident = hash of the decimal `HUID`
   (`h = (h >> 27) ^ (h << 5) ^ c`).
3. Link: the client sends INIT; we answer CONN; both sides then send reliable data from seq 0x100.
4. GameManager messages (all reliable, SendData flag 1): client hello (ticket text) -> host roster header (type 2,
   with the host player) and roster element (type 3, the joiner) -> client roster ack (type 4) -> host join
   (type 5), full mesh (8), join complete (9), voice receivers (0x15).
5. The client's Frostbite client starts (`sub_82385868`) and sends its packets as link datagrams (game link kind 7,
   SendData flag 0, unreliable seq 0x80 to 0xFF). The host passes them to the dedicated server and wraps the
   replies the same way. First exchange: client `80 00 00 D0`, server `A0 00 00 F0`; the client's join packet
   carries protocol version 54 (0x36).
6. Level load, "loading complete", A to start, spawn. A client that does not start in 20 s is kicked ("you have been
   kicked from the game", the game's own idle rule).

## Wire formats
- Tunnel datagram: u16 length (also the VDP header), one u16 per sub packet (length << 4 | port index), then the
  sub packets. Port 7 control: u8 0, u32 sender id, u8 bound (set once the sender has the other side's id); a side
  sends control until the other says bound. Port 1 GameManager link. Port 0 voice chat (DirtySock VoipConnection:
  `COd` connect, `PNG` ping, `DSC` disconnect, `MIC` data; tag table at 0x82ADC3E0): not served.
- Link (DirtySock CommUDP): u32 seq, u32 ack, data. seq 1 INIT (ack field: connection id, body 4 zero bytes), 2 CONN,
  0x80 to 0xFF unreliable, 0x100 and up reliable; an empty body is an ack only. The top nibble of seq counts older
  payloads appended for redundancy, each followed by its size byte (payload for seq - k at index k).
- Game link (DirtySock NetGameLink): data, 10 sync bytes when the kind byte has 0x40 (u32 time, u32 time, u16), kind
  byte. Kind 6 stream, 7 datagram. SendData (`sub_824A78A0`) writes [flag][data]: 1 for GameManager's own messages.
- GameManager message: type byte (0x80 + type), then fields. Integers are big endian with the sign bit flipped
  (+0x80, +0x8000, +0x80000000); strings are an i32 length and the bytes. Writers: roster `sub_824AA550`, join
  `sub_824BC798`, mesh `sub_824B95F0`, hello `sub_824AA078`; readers: host hello `sub_824B87F8`.
- Player record (88 bytes): i32 PID, i32 slot, i32 slot, string name, i8 0, u64 (a time like value), u64 XUID,
  i8 0, i8 2, XNADDR (36 bytes), u16 port (host 6001, joiner 1000), 00 80 and 7 zero bytes.
- `tools/tunnel_dump.py <log>` decodes a run's tunnel traffic (`-NetTrace`, `BF_NET_DUMP=1`).

## Runtime fixes this needed
- recv and send read the host socket error right after the call (`bf/src/socket_fixes.cpp`): a would block came
  back as error 0 under load and DirtySock closed the FESL connection.
- ObDereferenceObject skips an object found through a dead thread's stale handle (`bf/src/kernel_fixes.cpp`): the
  game dereferences exited threads while a level loads, and the release freed the FESL socket.

## Not done yet
- More than one client per server (needs three game processes; see STATE.md).
- The server list in our own menu (the registry's `list` is there); a ping for each server.
- Voice chat, the "Team" lobby messages a console host sends, game end and map rotation, players leaving.
- A server without a window or GPU; Linux.

## Test
`powershell -File tools\run_dedicated_test.ps1 -Name x [-DevPad ...] [-Dump]`: backend with the GameManager host on
127.0.0.2:1000, the dedicated server, one client that picks Quick Match. Spawn script used in ded10:
`20:start/0.5,24:start/0.5,28:start/0.5,36:a/0.5,48:a/0.5,86:a/0.3,92:a/0.3,98:a/0.3,104:a/0.3`.
