# Anti-cheat: what the server trusts

Players have asked for anti-cheat. Battlefield 1943 Reborn is open source and has no budget, and an anti-cheat in the
player's own game is easy to bypass when everyone can read and rebuild it. So the first step is the server: what does
it take from a player's game as it is, and what can it check on its own?

This is a study of 20 Nov 2026 (branch `night-ops-048`, GitHub's v0.4.8-testing). It is a static reading of the
recompiled code (`bf/generated/default`), our own code (`bf/src`, `server/`) and what earlier work found in matches.
Nothing here was built or run for it. Each finding says how sure it is:

- **Read:** seen in the code.
- **Seen:** seen in a match or a test run before.
- **Likely:** what the evidence points to, not proven.
- **Not traced:** not looked at far enough to say. Each comes with the probe that would settle it.

No check described here is built. TekRantGaming chooses which ones are built first, because a check that is wrong
removes real players.

## The short answer

**The game's server decides what happens.** A player's game sends the server its **input**: the stick and trigger
values, the buttons and its aim angles, once a tick. It does not send where its soldier is, what it hit, how much
damage it did or what its score is. The server moves each soldier from that input, fires its weapon, works out the
hits and counts the score itself. Later Frostbite games (Battlefield 3 on) moved hit detection to the player's game.
This one did not, by every sign below.

So the cheats that write "I am here" or "I hit him" into the game do nothing here. What is left are the cheats that
give **perfect input** (aimbots, triggerbots), that show what the game already knows but hides (wallhacks, radar,
Night Ops without its dark or fog), and that abuse **our own** additions: the chat and voice relay, the scoreboard's
ping, the world clock, and the dedicated server's own socket. Our own additions are the cheapest to fix.

### Why hits are the server's

1. **The soldier's message is input only** (read, `docs/input-concepts.md`). `sub_82378138` sends `sub_822BADF8`'s
   message: the tick (32 bits), the entry, digital buttons 8 to 33 and 40 to 43, eight analog values and the aim's yaw
   and pitch. The server reads it (`sub_822BA928`) into the player's input (`sub_823AF4A0`).
2. **Bots kill players with no game behind them** (seen, `bf/src/bots.cpp`). A bot is a server player with an input
   the server fills itself: the trigger is concept 7. Its shots kill human players. So the server fires weapons and
   works out hits from input alone, through the same path a human's input takes.
3. **The bullets are the server's** (read, the game's type names in `build/strings.txt`): `ServerBulletEntity`,
   `ServerProjectileEntity`, `ServerVehicleProjectileEntity` and the setting `Server.IsSoldierDetailedCollisionEnabled`
   (the soldiers' hit boxes on the server). A player's game has `AsyncClientBulletEntity` and
   `ClientGhostProjectileEntity`: its own bullets for the picture, and copies of the server's rockets and shells.
4. **The server's message handlers are requests** (read). The handlers for a player's messages (a run of small
   functions at 0x823EAF40 to 0x823EBF68, each passing the message's fields to the server's player) join a player
   (`sub_823AEF90`), choose a team (`sub_82374F90`), a squad and its leader (`sub_8237B1E8`, `sub_82375280`) and
   similar. None found takes a position, a hit, damage or a score. This list is not proven complete (see the
   appendix).
5. **The server's soldier is the authority** (seen, `bf/src/tick_rate.cpp`, `server_list.h`): a game simulating at a
   different rate from its server is "pulled about by every correction". The game predicts its own soldier and the
   server corrects it.
6. **The recoil kick is the camera's only** (read, side-session study of 6 Nov): shots leave along the aim, never the
   kick. A no-recoil cheat changes the picture, not where the bullets go.

### Why a speed hack through input does not work

- **The analog values cannot be large** (read). Each is read as a unit value (`sub_826533C8` and `sub_82652B50`: a
  sign, then a fraction), so a stick cannot be pushed past full on the wire. The aim angles are scaled the same way
  (`sub_82653448`).
- **One input a tick** (read, `sub_823AF4A0`). The server keeps each player's inputs in a queue and takes one per
  server tick; with none waiting it uses an empty input. A game that sends more inputs than there are ticks does not
  move faster. How the queue is trimmed when it grows was not traced.

## The trust table

Costs are per server tick at 60 ticks a second (16.7 ms each). They are estimates from the bots' measured costs
(`bots.cpp`'s BotCost: reading a soldier's place through the game is a few microseconds) and are for the **US Central
VPS**, which is small. "Free" means under 0.01 ms a tick.

| What | Does the server take the player's word? | How a cheat would abuse it | A server-side check | How hard | Cost a tick |
|---|---|---|---|---|---|
| **Hits and damage** | No: the server's bullets (above) | Nothing to send. Aimbots aim for the cheat instead | Statistics (below) | — | — |
| **Aim** | Yes, any angle inside the range | Aimbot: snaps to heads. Triggerbot: fires when on target | Snap speed before a hit; headshot share; accuracy | Medium | Free to 0.02 ms (a few sums per input) |
| **Position and movement** | No: the server moves the soldier from input | Speed and teleport by writing the position: nothing. A game bug (fall through, glitches) still works | Speed and teleport limits on the server's own soldier positions, as a tripwire | Easy | Free at 2 checks a second (32 places) |
| **Input rate and timing** | One input a tick is used | Sending extra: nothing. Holding inputs back and sending them in a burst (a lag switch): the player is hard to hit, then moves | Inputs a second per player; gaps over 1 s while alive | Easy | Free |
| **Fire rate and ammo** | No: the server fires the weapon and keeps its cooldown and ammo (likely) | A macro fires as fast as the weapon allows: allowed anyway | Shots a second per weapon against its RateOfFire, as a tripwire | Easy (hook on the shot, `sub_822DC330`) | Free |
| **Vehicles** | Likely no: driven by the same input. The game has `Client.LocalVehicleSimulationEnabled` | If the server ever takes a driver's vehicle state: speed, flying tanks | The same speed limits, per kind of vehicle | Easy | Free |
| **Flag captures** | No: the server's capture points count the server's soldiers | Nothing to send | — | — | — |
| **Score and stats** | No: counted on the server (`server_stats.cpp`); the backend keeps them from official servers only (`player_stats.py`) | Kill trading with a friend; playing under another player's name that is not protected | Kills of the same victim again and again; stats only for protected names (later) | Easy | Free (at each kill) |
| **Spawn choice** | The server spawns at the point asked for (`sub_8236D558`); whether it checks the point is the team's: not traced | Spawning at an enemy flag, if it is not checked | Refuse a spawn point the team does not hold | Medium | Free (at each spawn) |
| **Team, squad, kit** | Requests the server carries out with its own rules; Change Team goes through our balancer (`team_balance.cpp`) | Little | — | — | — |
| **The camera's place** | A message stores a 4 x 4 transform on the server's player (`sub_8236F468`, player +416); what reads it was not traced | Likely only what the server sends the player first | Not traced | — | — |
| **What the player's game is told** | Everyone's soldier, near or far | **Wallhack, radar, ESP**: draws enemies through walls. Night Ops with its dark and fog turned off | Sending only what a player could see: a ray per pair of players | Hard | High: 32 x 32 rays a tick; not for the VPS |
| **Text chat** (`gmhost.py` Comms) | The **team and squad** a player's game says it is in | Read the other team's team chat by saying you are on it | Take each player's team and squad from the game server | Easy | Free (a small file once a second) |
| **Text chat: who is talking** | The name, if a joined player with that name is at the same IP address | Behind one home router: speak as the other player, and push their chat link off | Tie the comms link to the game link it came with (a code given at the join) | Medium (game and host) | Free |
| **Chat flooding** | 5 lines in 5 s, 200 bytes | — (already limited) | — | — | — |
| **Voice** | The sender's place, and the listener's place (a listener with no place hears everyone) | Hear the whole server, the enemy's callouts too; be heard everywhere | Places from the game server, as for chat | Easy | Free |
| **Voice flooding** | 60 packets a second | — (already limited) | — | — | — |
| **Ping on the scoreboard** (`World`, flag 4) | The ping the game says it has | Hide a high ping, or show one; dodge the High Ping pop-up | The host measures it from the link's own replies | Easy | Free |
| **World clock and weather** (`World.admin`) | An admin by player id and the same IP address, or the admin key | The key goes as plain text; anyone who sees it can change the sky | Keep keys off the network: player ids only, or a signed request | Easy | Free |
| **The dedicated server's own socket** (UDP 1003, 31003 on Linux) | Anything that reaches it | If it can be reached from outside, a game can skip the host: no ticket, no bans, no mutes | The server manager binds it to 127.0.0.x (`BF_LOOPBACK_ONLY`). A server started by hand binds every address | Easy | — |
| **Packets to a player's game** (`SenderAllowed`, `socket_fixes.cpp`) | A player's game takes packets only from where it sent | Protects players from strangers. A dedicated server answers anyone, by design: the host is its gate | — (already done) | — | — |
| **Joining** (`gmhost.py` hello, `plasma.py` reserve and EGAM) | A ticket from the backend; bans and kicks checked (`docs/MODERATION.md`) | Ban evasion with a new player id, account and address | A new player id and address are cheap to get; flags (below) catch the play, not the person | — | — |
| **Modified game files** | The server cannot see the player's files | Removing smoke, fog, darkness, foliage | None on the server | — | — |

## What `bots.cpp` already gives a check

The bots work found everything a server-side check needs, all on the server thread:

- The players: the game context 0x82AF56F0, its player manager at +40, the players from +84 to +88. A player's team at
  +48, squad at +52, soldier at +288 (0: dead), kit's soldier data at +500, the game's own AI flag at +532.
- A soldier's or vehicle's place: `sub_822A7BB0` (the transform, position at +48). `CachedPosition` keeps one read per
  entity per tick, because reading it through the game thousands of times a second cost too much.
- A player's input as the server got it: the EntryInput at player +280 (`sub_823AF4A0` fills it), with the aim's yaw
  at +136 and pitch at +140 of its state.
- Ray tests on the server (`Ray`, `bots.cpp`): what a bot can see. The same test could ask whether a killer could see
  the victim, after a kill only, which is cheap.
- Kills with the killer, victim, weapon and headshot (`sub_82409708`, hooked in `server_stats.cpp`; the bots' own
  `BotsNoteKill` takes where both stood).
- Bots are told apart (`BotIsPlayer`), so no check ever counts a bot, and a bot's death never counts as a kill for
  statistics.

## Order of work

Cheapest first, biggest effect first, and nothing removes a player on its own until the numbers have been checked
against real matches. Every check in steps 2 and 3 only **logs and flags** at first.

### 1. Close our own holes (no false positives possible)

These are fixes, not judgements about players:

1. **The dedicated server's socket.** In server mode the game binds its socket to a loopback address unless told
   otherwise, and the server's log says so. Check the firewalls of the Hetzner servers and US Central for UDP 1003 and
   31003 (`us_night.sh` starts its server by hand).
2. **Chat and voice from the game server's facts.** The game server writes each player's name, team, squad and place
   once a second to a small file (as it writes `stats.txt` and the balance file); the host routes team chat, squad
   chat and voice by those, not by what the player's game says.
3. **Ping measured by the host** from the link's own sequence and acknowledgement times, kept beside the game's figure.
4. **The world's admin key** kept off the network (admins by player id only, or a signed request as the network
   broadcasts are).

Cost: none worth counting. Effort: about a day for 2, an hour each for 1, 3 and 4.

### 2. Tripwires on the server (log only)

They should never fire if the study above is right. If one does, it has found a hole:

1. **Speed and teleport:** each living soldier's and vehicle's place twice a second. Flag a soldier on foot faster
   than its sprint plus a margin, a vehicle faster than its kind allows (planes are fast: limits per kind), and a jump
   of more than 50 m that is not a spawn, a seat change or a parachute.
2. **Input rate:** inputs a second per player against the tick rate; gaps of more than a second while alive (lag
   switch).
3. **Fire rate:** shots a second per player and weapon (`sub_822DC330`) against the weapon's RateOfFire.

Cost: well under 0.05 ms a tick together. Effort: a day or two, in a new `bf/src/server_checks.cpp`.

### 3. Statistical flags for admins

Per player, per round and over the last 30 minutes:

- **Accuracy:** hits on players over shots fired.
- **Headshot share** of kills.
- **Kills per minute** alive.
- **Kill distance** against the weapon.
- **Aim snap:** how far the aim turned in the 100 ms before a hit, against how fast it turned the rest of the time.
- **Kills through cover:** after a kill, one ray from the killer's eyes to the victim (the bots' ray test). Many
  kills with no clear line is a wallhack sign.
- **Kill trading:** the same killer and victim again and again.

These are what admins look at anyway. No threshold is set until two weeks of these numbers from the official servers
show what good players do: a 1943 veteran with a scout rifle has a high headshot share by right. Bots are left out of
every figure.

Cost: a few sums at each shot and kill, one ray at each kill. Effort: two or three days, plus the two weeks of numbers.

### 4. Later, if needed

- A short recording of the last minutes of a flagged player (their inputs and places), for an admin to look at.
- Sending each player only the enemies it could see. Too heavy for the VPS today.

### How it feeds the moderation tools

The moderation tools (`docs/MODERATION.md`, built in the session "Server moderation tools and ban lists") already have
the pieces:

- **The moderation log** (`moderation_log.jsonl`, 90 days, no addresses): each flag becomes a line, action `flag`, with
  the player id, the check and the numbers (for example "headshots 92% of 38 kills, aim snap 4.1x").
- **Players on now** on the manager's Moderation page: a flagged player gets a mark and the latest numbers beside
  Kick, Ban and Mute. The admin decides.
- **The Reborn network:** official servers send their flags to the main backend with the player id only, as the
  network's `remove` does. The official manager can see one player's flags across every official server before a
  network ban.
- **The path:** the game server writes flags as JSON lines into a file (as `stats.txt`), the host
  (`gmhost.py`) turns each into a moderation event, as it does for refusals.
- Kicking or banning on a flag by itself is a later step, and only if TekRantGaming asks for it.

## Easy Anti-Cheat through Epic Online Services

Easy Anti-Cheat is free through Epic Online Services and covers Windows, macOS and Linux games with native builds, and
Windows games under Wine or Proton (so the Steam Deck), switched on in Epic's Developer Portal. What it would take
here:

- **An Epic product.** An organisation and a product in the Epic Developer Portal, with its client credentials, and
  Epic's developer agreement accepted for it. Epic's agreement and its Anti-Cheat terms are behind a sign-in in the
  portal and could not be read for this study. They must be read before any step is taken.
- **The rights question.** A developer registering a product normally states that it holds the rights to it. This
  project is a recompilation of EA's Battlefield 1943: the game code is EA's, the name is EA's trademark, and the
  players' game files are EA's. A product called Battlefield 1943 registered by this project may not meet that, and
  Epic could refuse or remove it at any time. Asking Epic first, in writing, is the only safe way.
- **A closed SDK.** The EOS SDK and the anti-cheat client and server modules are Epic's, not open source. They must
  stay out of the public repository. Official builds would be built privately with them; the public source would
  build a game without them.
- **Only official builds pass.** Easy Anti-Cheat checks the game's files against a list made for each release. Anyone
  who builds from source, or changes a file, fails it. Protected servers would only take official builds, so players
  of their own builds, and of pull requests under test, could only play on unprotected servers. That splits the
  player base.
- **Its launcher.** The game starts through Epic's launcher (`start_protected_game`) instead of `bf1943.exe`: a change
  to the updater, the Steam shortcut, the Linux AppImage, the Ally and Steam Deck paths, and every test script.
- **Server work.** The game server is our game run headless. It would need the anti-cheat server module in our own
  C++, its messages carried between each player's game and the server on a channel of their own (through the host's
  tunnel), and players with no valid session refused at the join. Linux servers are supported by Epic's SDK.
- **Privacy.** A kernel-level anti-cheat on players' PCs needs its own line on the privacy page and in the game's
  first start.
- **What it would not do.** It catches cheats that inject into the game. It does nothing about cheats that read the
  screen, or about players who never had to cheat because the server's own rules let them.

**Recommendation: not now.** The rights question alone could end it, and an official-builds-only rule cuts against an
open source project. Do the server-side work above first. Look at Easy Anti-Cheat again only if EA ever gives the
project permission, or if cheating grows past what admins and the server's flags can handle. If it is pursued then,
the first step is a written question to Epic about a recompilation of a game the project does not own.

Sources: Epic's announcement of Linux, Mac and Steam Deck support
(https://onlineservices.epicgames.com/en-US/news/epic-online-services-launches-anti-cheat-support-for-linux-mac-and-steam-deck),
Epic Online Services licensing (https://onlineservices.epicgames.com/licensing).

## Appendix: the server's message handlers

The run of handler thunks at 0x823EAF40 to 0x823EBF68 (each reads the message's fields and calls the server's player
or connection). Those named are from earlier work (`bots.cpp`, `team_balance.cpp`, `server_stats.cpp`); the rest were
not identified.

| Thunk | Calls | What it is |
|---|---|---|
| 0x823EB0C0 | `sub_823AF270` > `sub_823AEF90` | A player joins: the server player made (`sub_82392658`), scored (`sub_8240DFE0`) |
| 0x823EB888 | `sub_8239DE08` > `sub_82374F90` | Choose a team (as `team_balance.cpp` found) |
| 0x823EB918 | `sub_8239DE20` > `sub_82375280` | Squad leader |
| 0x823EB738 | `sub_8239E048` > `sub_8237B1E8` | A squad request |
| 0x823EBAF8 | `sub_8239E060` > `sub_8237B8A8` | A squad request |
| 0x823EBC00 | `sub_8239E080` > `sub_82375210` | Not identified |
| 0x823EB680 | `sub_8239DF78` > `sub_8236F468` | A 4 x 4 transform stored at player +416 (likely the camera) |
| 0x823EBA48 | `sub_8239DF48` > `sub_8236F800` | A value stored at player +528 |
| 0x823EBA70 | `sub_8239DF60` > `sub_8236F770` | A call on player +224's object |
| 0x823EB6A8 | `sub_8239DF90` | A flag at player +400, or `sub_823750E8` |
| 0x823EB360 | `sub_8239DE90` > `sub_8231E360` | Not identified |
| 0x823EB5F0 | `sub_823A5430` | Not identified |
| 0x823EBAD0 | `sub_823A54D8` | Not identified |
| 0x823EB748 | `sub_823AAF80` > `sub_823AADD0` | Not identified (several fields) |
| 0x823EBD28 | `sub_823AB2F0` | Not identified |
| 0x823EBF00, 0x823EBF68 | `sub_823A1A28`, `sub_823A1CF8` | Not identified |

**The probe that would settle the open rows:** a developer build that logs each handler's thunk, the player and the
first bytes of its message on the server for one match, with one player walking, driving each vehicle, flying,
spawning at each flag, changing kit, team and squad, and dying. Any handler that carries a place or a hit would show
there. A second probe asks the server to spawn a player at an enemy-held point.
