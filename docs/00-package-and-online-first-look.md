# Package contents and a first look at the online code

4 Nov 2026. Evidence from the extracted package (`input\pkg`, ignored), the decrypted image (`input\image.bin`,
offset = VA minus 0x82000000) and its strings (`build\strings.txt`). No code analysis yet: everything below is
from headers, config files and strings, so code level claims are marked UNVERIFIED until Ghidra confirms them.

## Package (VERIFIED)
- STFS LIVE package, content type 000D0000 (XBLA), title ID 5841097E, display name "Battlefield 1943".
- One hash table per level, 142,321 blocks. 90 files, 582,754,796 bytes. Every data block matched its SHA1
  hash entry, so the extraction is complete and undamaged.
- `ArcadeInfo.xml`: XDK 2.0.8276.0, project version 1.0.88.0, 12 achievements, one rating leaderboard.
- Data is Frostbite 1.x `.fbrb` archives under `dist\xenon\`: four maps (Wake Island, Guadalcanal, Iwo Jima,
  Coral Sea) plus `wake_island_tutorial`, menus, HUD (including `hud_singleplayer`), sounds, weapons.
- `dist\xenon\config.zz` is a plain zip of `.cfg` and Lua files (see below).

## default.xex (VERIFIED from the header)
- XEX2, retail encrypted, compressed. Load address 82000000, entry point 827D78B0.
- Media ID 588DC151, version 0.0.1.6 (base 0.0.1.6), title ID 5841097E.
- Original PE name `Frost.Game_Xenon_Retail.exe`, PE timestamp 4A41FFCA (24 June 2009). Build path
  `C:\monkey\Next.Beach.Code.Xenon\TnT\...`, Perforce branch `//Frostbite/Sandboxes/BF1943/`.
- Imports: xam.xex (226 functions) and xboxkrnl.exe (343), both 2.0.7978.0.
- Static libraries (XDK 2.0.7978.3): XNET, XONLINE, XGRAPHC, XMP, XPARTY, LIBCPMT, XAPILIB, XBOXKRNL, XAUD, D3DX9,
  XHV, D3D9LTCG. Also RAD Bink and Granny, Havok, EASTL, EA DirtySock.
- LAN key is the placeholder `TESTTESTTESTTEST`.
- Sections: `.text` 82160000 (8.6 MB) plus `ctr` and `BINK` code, `.data`, Frostbite `fieldinf` and `typeinfo`
  (reflection data, useful for naming), `.pdata` (function table).

## Online stack
- **EA Plasma is used on the 360 (VERIFIED, strings).** FESL and Theater client code is present: `fesl.ea.com`,
  `.fesl` and `.ea.com` (host name built at run time), `feslPort`, `theaterIp`, `theaterPort`,
  `FeslHostOverride`, `TheaterHostOverride`, `360FeslServiceId`, `FeslConn broken`, `TheaterConn broken`.
  Editorial news from `http://easo.ea.com/easo/editorial/BF/2009/BF1943/`.
- **360 login transactions:** `NuXBL360Login` and `NuXBL360AddAccount` next to the PS3 ones (`NuPS3Login`,
  `NuGetAccountByPS3Ticket`) and the generic `NuLogin`, `NuLoginPersona`, `NuGetPersonas`, `NuEntitleGame`,
  `GetTelemetryToken`, stats (`GetStats`, `GetRankedStats`). The exact host prefix, port and which
  transactions the 360 sends are UNVERIFIED (need code).
- **SSL:** a VeriSign certificate string is present, so FESL is over EA ProtoSSL as on PS3 (UNVERIFIED for
  the check logic).
- **EA's 360 servers:** enum strings `XBOX_360_DEDICATED_SERVER_TYPE_NONE`, `_SG`, `_UNSECURE`, `_XLSP` and
  `360DedicatedServerType`. EA's dedicated servers were reached through Xbox LIVE Server Platform (XLSP) and the
  security gateway (GUESS from the names).
- **Server code:** `config.zz` has `ServerGame.cfg` (`-server -platform xenon` plus mounts of Win32 data) and
  `ServerGameLinux.cfg` (`-server -platform xenon -dedicated`), and `Scripts\Server\*.lua` (ban list, level
  list, player join and leave hooks, skipped when the platform is xenon or ps3). So EA ran separate Linux
  dedicated server builds for the 360 game, which we do not have (VERIFIED that the configs exist).
  The client also contains `ServerMain`, `Xenon server`, `IsServer`, `Server.Level is not set in Game.cfg`
  and many `Server.*` settings, which points to the Frostbite server running inside the client process, at
  least for the tutorial (UNVERIFIED, strong hint). Beachmod making the PS3 client host and run dedicated
  servers supports this.
- **Offline content:** `wake_island_tutorial`, `IsTutorial`, `startTutorial`, `TutorialLevelName`, and bot
  strings (`OSEnemyBotsOnly`, `OSPlayerAndEnemyBots`, `TargetBotSoldier`). A tutorial or bot mode may give a
  first in game milestone without any backend (UNVERIFIED).
- `Game.cfg` has `Game.RetailServers false` (the shipped config; meaning unknown).

## Prior art
- **Arcadia** (github.com/valters-tomsons/arcadia, C#, GPL 2.0, last push 24 Oct 2026): EA Plasma emulator
  for PS3 clients, live public server. Has real Battlefield 1943 support for the PS3 build (partition `/BEACH`,
  FESL prefix `beach-ps3`, ports 18231 FESL and 18236 Theater, NuPS3Login, stats including the derived score
  and the Coral Sea unlock at 1,000,000 global kills, squads as Theater playgroups, server browser through
  Theater). It does not handle `NuXBL360Login` or any Xbox 360 client, and it relies on PS3 specifics (PSN
  or RPCN tickets, the old ProtoSSL bug for certificates, DNS redirection).
- **Beachmod** (beachmod.net, closed source, no licence stated): a PS3 patch for 1943 that adds player hosting,
  dedicated servers with a console, a server browser and a create game menu, and connects to Arcadia. PS3 and
  RPCS3 only. Arcadia notes that 1943 needs Beachmod to host, so on PS3 the game servers are patched clients.
- **Other revivals:** none found for the 360 or a native PC port. A Battlefield 2 mod recreates 1943 on PC
  (different engine, not relevant to the code).
