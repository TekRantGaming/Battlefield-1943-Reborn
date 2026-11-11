# Online flow of the Xbox 360 build (what the client needs, layer by layer)

4 Nov 2026. Found with the scripted pad, the network and online traces (`bf\src\net_trace.cpp`,
`bf\src\online_trace.cpp`, `tools\run_game.ps1 -NetTrace`) and the Ghidra project. Status of each claim:
VERIFIED (seen in runs), UNVERIFIED (read in code, not tested), GUESS.

## 1. Xbox LIVE sign in (bf\src\live_layers.cpp, all builds)
- VERIFIED: without these, Multiplayer says "You need to be signed into Xbox LIVE to access Xbox LIVE game play
  features". Answered at the imports (the exe overrides the runtime's exports): sign in state 2, sign in info with
  XUSER_INFO_FLAG_LIVE_ENABLED and an online XUID (0x0009 in the top 16 bits), XUID, privileges granted (first
  asked: 254), Ethernet link up, title address online, one XN_LIVE_CONNECTIONCHANGED with
  XONLINE_S_LOGON_CONNECTION_ESTABLISHED per listener that polls the LIVE area with no filter.

## 2. EA's Xbox LIVE Server Platform services (bf\src\online_services.cpp, all builds)
- VERIFIED: at start the game asks XOnlineGetServiceInfo (XMsgInProcessCall to the LIVE base app 0xFC, message
  0x58007) for services 0x45410004 and 0x45410805. The runtime says "not logged on" (0x80151802) and the game then
  fails Multiplayer after 6 s with reason 14, "Failed to connect to Xbox LIVE" (dialog chooser `sub_82455BB8`,
  connect state update `sub_824EBA48` counting services at `[+4]+0x124`).
- Answered with BF_ONLINE_HOST (default 127.0.0.1), port BF_ONLINE_PORT or BF_ONLINE_PORT_<service> (default 0).
  The game then calls XNetConnect and XNetGetConnectStatus on that address (runtime stubs), answered as connected.
- 0x45410004 is the FESL and Theater host. 0x45410805 GUESS: the HTTP host (the news request went to the same
  address).

## 3. Sockets (bf\src\socket_fixes.cpp, all builds)
- VERIFIED: the runtime's recv, send, recvfrom and sendto never set the guest last error, so WSAGetLastError said
  31 after a non blocking recv and DirtySock closed the connection ("Failed to connect to EA Online"). Fixed by
  copying the host socket error (Winsock codes; errno mapped on Linux).
- Test runs bind to 127.0.0.1 (BF_LOOPBACK_ONLY=1) so Windows Firewall does not prompt.

## 4. EA Plasma: FESL (TCP 18241) and Theater (TCP 18246), plain text, no SSL
- VERIFIED: on the 360 the Xbox security gateway encrypted the link, so the client speaks plain FESL with no
  ProtoSSL. Packet: 4 letter type, big endian word (transmission type in the top byte: 0xC0 request, 0x80
  response, 0xB0 and 0xF0 multi part, 0x00 ping; id in the low 24 bits), big endian length, then `key=value` lines
  and a zero byte. Payloads over about 8 KB go as multi part (`data` base64 chunks with `=` as `%3d`, `size`,
  `decodedSize`); the client announces `fragmentSize=8096`.
- FESL port 18241 for the 360 (Arcadia lists 18231 for the PS3 build). Theater port is what our Hello says (18246).
- Client sequence (VERIFIED, `server\plasma.py` answers all of it):
  1. `fsys Hello` (clientString beach-360, sku xenon, clientPlatform XBOX360, SDKVersion 5.1.0.0.0,
     protocolVersion 2.0) and our `MemCheck`.
  2. `acct NuXBL360Login` (xuid in decimal, gamertag, macAddr, consoleId); reply lkey, userId, personaName.
  3. `fsys GetPingSites`, `asso GetAssociations` (type PlasmaBlock), Theater `CONN` (PROD beach-360, PLAT XBOX360)
     and `USER` (LKEY), `acct GetTelemetryToken`, `rank GetStats` for the player (201 keys: score, rank, kills,
     deaths, awards, ...) and for the game (owner 100, ownerType 11: global_kills, pm_minplayers).
  4. HTTP GET `/easo/editorial/BF/2009/BF1943/ticker-xenon.xml` with Host `easo.stest.ea.com` (the shipped
     `Game.cfg` sets `Game.RetailServers false`, hence the test host), User-Agent ProtoHttp 1.3/DS 7.3.1.0.
- The client also opens a VDP socket (UDP, protocol 0xFE) on port 1000: the game traffic socket.

## 5. News ticker (HTTP)
- VERIFIED: `sub_823BB598` handles `ticker-xenon.xml`: the first 8 bytes must be a UTF-8 byte order mark and
  `<?xml`, else it retries in 60 s; then `<ticker>`, `<lang id="en-US">` (the console language), `<item>` texts;
  then game message 0x25. With our server answering, the client reads the whole response but the handler is never
  called (runs http3 to http5); with port 80 closed the connect fails quickly and the flow continues.

## 6. Menu side
- The menus are EA APT movies (BIGF archives `main_root.res`, `main_popups.res`, ... in `mainmenu-00.fbrb`).
- Game to menu calls by name go through `Function_8243BB70` (traced as `BF UI call`). Sequence on Multiplayer:
  `couch`, dialog event 1 (Connecting), event 2, event 4 `presenceConnected`, `couch`, then nothing.
- Playgroup events: `sub_82473100` (couchJoinPopupShow, couchLeaving, couchCreating, couchJoinFailed,
  couchCreateFailed, ...), posted by the UI event pump `sub_82482380` like the dialog events.

## 7. The "Connecting" dialog (solved)
- VERIFIED: the menu's `presenceConnected` handler (main_root.apt, read with `toolspt_disasm.py`) starts with
  `connectionDialog.done()`. The game opens that dialog through a queued menu command (`presenceConnecting`,
  dialog event 1) but calls `presenceConnected` directly (event 4). With an instant local login reply,
  `presenceConnected` ran before the dialog existed, and the dialog then stayed up. `server\plasma.py` waits 2 s
  (`--login-delay`) before the NuXBL360Login reply; the online Multiplayer menu then appears (Quick Match, Join
  Match With Friends, Create Private Match, Stats and Awards), run `delay1`.
- `presenceConnected` also shows a one time PRC notice when `extern.chatRestricted` or `extern.isLive` is
  undefined, then calls `cbOnConnected` and `updateTrialTimeLeft`.
- The HTTP ticker is still open: with port 80 answered the "connected" event never comes; the server leaves port
  80 closed by default.

## 8. Games: Quick Match, hosting, joining (server\plasma.py Theater, 4 Nov 2026)
- Quick Match: FESL `pnow/Start` with sessionType findServer (filters version RETAIL429432, type RANKED), then
  resetServer (pool 24, matchunusedserver). Reply Start then Status: NOSERVER shows "No games available";
  JOIN with `props.{games}.N.gid/lid` makes the client ask GDAT and EGAM.
- EGAM from a client carries PORT 1000 and R-XNADDR (base64 of its XNADDR, IP 127.0.0.1 in our runs).
- Create Private Match: the client is the host. CGAM (RESERVE-HOST 1, TYPE P, HXFR 1, PORT 1000, INT-IP,
  MAX-PLAYERS 16); reply TID, LID, GID, MAX-PLAYERS, EKEY, UGID, JOIN, SECRET. Then the host enters its own game:
  EGAM, EGRQ to the host (it builds its player list from EGRQ), EGRS ALLOWED 1, EGEG (PID, TICKET, I, P,
  INT-IP, INT-PORT, HUID, EKEY, UGID, XNADDR, XNKID, XNKEY), PENT (reply must carry the PID, sub_824B6CD0),
  then UBRA START 1/0 around UGAM updates. The lobby shows "Players needed to start: 15".
- The client's field tables for these replies are at 0x8202543C to 0x8202589C. Its own local EGEG
  (sub_824B03F8) uses UGID NOGUID and EKEY NOENCYRPTIONKEY.
- Joining a server: after EGEG the client's GameManager sends 24 byte packets (VDP length 0x0016, then
  `0067 00C1 ...`) to the host every second (sub_829381E8, per peer, from sub_824DD530) and gives up after 30 s
  (ECNL). A Frostbite only dedicated server ignores them.

## 9. EA tunnel and GameManager (two clients, 4 Nov 2026)
- DirtySock tunnel (sub_829389E8 alloc, sub_829381E8 send, sub_82939458 receive match). Datagram on the VDP
  socket (port 1000): 2 byte length (also the VDP header), then 2 byte sub packet headers (12 bit length, 4 bit
  port index), then the data. Port index 7 is control: 0, the sender's 4 byte id, a bound flag. A tunnel's ident
  is hash(decimal text of the remote user id) (h = h>>27 ^ h<<5 ^ c); the module's own id ('clid', +0x10) is
  hash(own user id). The receiver matches the sender's control id against its tunnel ident. Port indexes 0 to 3
  are virtual ports 6000 to 6003: 6000 Frostbite game traffic, 6001 GameManager link for public games, 6002 for
  private matches.
- Host side: EGRQ creates the player (fields read by sub_824D8580), EGRS goes out as a request (its reply starts
  the link, sub_824BDD70), the link setup needs XNetXnAddrToInAddr on the joiner's XNADDR, the joiner's hello
  carries the EGEG ticket, which must equal the EGRQ ticket (sub_824BDDF0), then roster, join, mesh, PENT.
- The game kind (game+0x9C, 0 public, 1 private) is set by sub_824B61C0 and picks the link port and the
  completion event. A Quick Match joiner has kind 0; joining with kind 1 completes into an empty listener method
  and BF releases the game (ECNL).
- After a public join BF starts loading and its Frostbite client sends "COd" connect packets through virtual
  port 6000; without a running game server on the host it gives up after 7 s.

## Dialog events (sub_82455BB8, event type at +0x28, reason at +0x2C)
0 disconnected (refreshMainMenu), 1 connecting (DIALOG_CONNECTING_NOTICE, or UPDATE_REQUIRED for reason 17),
2 login state, 3 ?, 4 connected (presenceConnected), 5 create account (presenceCreateAccount), 6 create account
failed, 7 connection failed (reason 1 "Failed to connect to EA Online", 14 "Failed to connect to Xbox LIVE", 15
invalid profile, 16 busy, 17 update required, 18 no network, 19 invite not found, 20 restricted), 9 connection lost.
