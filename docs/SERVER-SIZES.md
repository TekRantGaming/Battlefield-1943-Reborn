# Server sizes: 24, 32 and 64 players

TekRantGaming, 20 Nov 2026: "work on allowing increased player numbers in servers. This will be an option for the
community servers to use, and obviously for us too. We should support the game's default. We should then support 32,
64, and 128."

What was found and built is below. A server can now be 24 players (the game's own size, "original") or smaller, 32,
or 64. 128 is not offered: section 3 says what it would take. His answers to the questions this raised are in section 5.

**32 is the default** (TekRantGaming, 20 Nov 2026: "32 players is becoming the new default even on official servers but
we will still allow 24 as (original) to be an option"):
- A new server, and a `server.json` with no `max_players`, is 32; gmhost.py's `--max-players` defaults to 32.
- A `server.json` that says 24 keeps 24. Every existing `server.json` holds 24, because the manager writes every
  field. So the official servers move to 32 only when their `server.json` says `"max_players": 32` at the next deploy.

## 1. Where the game limits players

| Limit | Value | Where | What it means |
|---|---|---|---|
| `Network.MaxClientCount` | 24 | game settings, read by the server's network setup (sub_823BD870 keeps it at +828; sub_823B59E0 refuses a connection past it) | Human players, since each one is a connection. Bots take none, which is why 24 humans plus bots already reached 26. **Raised** on a 32 or 64 server to its size + 4. |
| `Game.MaxPlayerCount` | 64 | game settings, on every machine; the player manager is sized from it (server sub_8237B540, client sub_82272E18) | The player manager makes no player past it (sub_82387EF0). It also sets how many bits a player id takes on the wire (`bits(N - 1)`: 6 for 64). **Left at 64.** |
| Player id bits | 6 (from 64) | written by sub_8234DDF8, read by sub_82257BD0 and sub_823EC128, each machine using its own count | A server and its players must agree. A server raised to 68 sent 7-bit ids to games that read 6, and the first game to join a 63-player match died on an object id it never had (run `size64a`). |
| Per-team cap | 12 (a TeamData word, +24 of a team's data) | sub_82375918, read only by the team pick sub_82399AE8 | A player's own join tries every side without it, but a pick made with the check on (our bots, a Change Team) is refused at 12: on a 32 server 6 of 30 bots had **no team** (team 0, run `sbstudy8`). **Raised** on a 32 or 64 server to half the size + 2 (18, 34). |
| Squads | 4 a squad (+28, MaxPlayersPerSquad); squads 0 to 23 for each team; 24 means none | sub_82392718, sub_82392828 | 24 x 4 = 96 a side: enough for 32 a side. |
| `Network.MaxGhostCount` | 8192 (13-bit object ids) | game settings, both sides | At 63 players the client's ghost table held about 100 objects when it joined. Not near the limit. |
| `Server.OutgoingFrequency` | 15 | game settings | How often the server sends updates; part of why traffic stays low (section 4). |
| The game's scoreboard (Flash) | 12 rows shown, scrolls | the HUD's scoreboard movie, fed from `friendlyTeamScores` / `enemyTeamScores` (built by sub_824627C8 and sub_82462EE0 from the side's players) | **Not a limit.** The lists held 12 only because nobody past 12 had a team. With the team cap raised, the list holds the whole side and the movie scrolls under its cursor: rows 4 to 15 of 15 shown in run `size32b`, our BOT and ping cells following. |

Our own code had lists cut at 64 players. They now use `bf/src/player_limits.h`: `kPlayerListLimit` (128) for lists
read from the game, and `kMaxServerSize` (64). The files changed are bots, team balance, minimap players and clan
tags. In the host and backend, `MOST_PLAYERS` (128) applies to the player name, score, stats and id lists.
`BIGGEST_SERVER` (64) is the most a server may register as.

Maps: Iwo Jima has 7 soldier spawn points per side, and every map keeps its shipped vehicles and flags. At 64 they are
shared by more players. TekRantGaming chose to keep the maps as they are.

## 2. What 32 and 64 take, and what they cost

All runs were on this PC with bf-dev at 60 ticks a second. Each had a local backend (127.0.0.5), one test player, and
bots filling the rest (bots cost about what a player costs: memory `bots-performance`). The tick figures are the
server's own "BF server ticks" lines. The CPU column is the server process's share of one core, sampled every 10 s.
Traffic is the host's new line, "traffic over 60 s".

| Run | Map | Players | Tick work, average (longest) | Bots' cost on top | Server CPU | Client FPS | Traffic to a player |
|---|---|---|---|---|---|---|---|
| size24a | Wake | 1 + 22 bots, 12 a side | 0.47 to 1.29 ms (3.0) | 0.7 to 1.3 ms | about 12% of a core | 60 (the limit), 13.6 ms a frame to spare | 1.7 to 2.2 kB/s |
| size32c | Wake | 1 + 29 bots, 15 a side, none without a team | 0.61 to 1.12 ms (2.7) | 0.8 to 1.5 ms | 11 to 17% | 60, 13.8 ms to spare | 2.1 to 2.6 kB/s |
| size64j, size64k | Wake | 1 + 61 bots, 31 a side, none without a team | 0.93 to 1.69 ms (12.9) | 1.1 to 1.9 ms (many bots wait on the carrier for boats) | 21 to 33% | 60, 12.3 to 13.5 ms to spare | 3.7 to 5.2 kB/s |
| size64h | Wake | the same | the player's game hung 30 s after joining (below); the server played on | | | | |
| size64i | Guadalcanal | 1 + 61 bots, 31 a side, none without a team | 0.79 to 0.89 ms (2.6) | 6.2 to 6.9 ms (0.5 of it mapping the unmapped level; the soldiers' input step 5.4 to 6.2) | 34 to 48% | 60, 12.9 ms to spare | 5.4 to 6.0 kB/s |

The tick work column is the game's own tick ("BF server ticks"); the bots' cost ("BF bots: cost over 30 s") comes on
top of it. Every run had 0 ticks of 25 ms or more. Flags taken in 4 minutes: 6 at 24, 8 at 32.

**The hang at 64 (run `size64h`, fixed):** the player's game stopped drawing 30 s into the match and its watchdog
wrote a hang dump. The game thread was spinning in the game's job scheduler: the job graph walk (sub_82607CA8) takes
one of the scheduler's 256 slots for each job and frees none until it ends, and a walk that needs more waits forever
in sub_82608AF8. The map editor branch had met the same freeze on 18 Nov (bca78cb) and given the pool 512 slots
(`bf_job_slots`); that hook is now on this line too (`frame_rate.cpp`), with its developer counter
`BF_DEV_JOBGRAPH=1`. Two more Wake runs at 64 on the old pool did not hang: it is a race with a busy moment, not every
match. Section 2a gives the slot counts.

The 64 runs of 20 Nov (`size64b`, `size64c`, `size64g`) are not used: past 12 a side their bots had no team
(section 1), so they measured about 24 players. They did show that 63 players in one match do not break the
game: no crash, the joining player at 60 FPS, the ghost table far from full.

**What a server costs to run:** the game's own tick stays under 1 ms even at 64. The cost is the soldiers: on
Guadalcanal at 64, where both sides fight on land at once, the soldiers' input step took about 0.1 ms each a tick.
That puts a 64 server near half of one core on this PC (a tick of about 7.5 ms out of 16.7). A player's soldier costs
about what a bot's does (memory `bots-performance`), so 64 real players cost about the same, plus their connections.
Plan on one core per 64 server, and do not put more than two 64 servers on US Central's 4-core VPS.

Traffic to each player grows with what is near them: 2 to 3 kB/s on Wake at 24 and 32, and 5.4 to 6 kB/s on
Guadalcanal at 64. A full 64 server sends about 64 x 6 = 380 kB/s (3 Mbit/s), and takes in about 64 x 2.6 = 170 kB/s.

### 2a. The job scheduler at 64

With `BF_DEV_JOBGRAPH=1`, the largest job graph walk every 10 s (runs `jobs24w`, `jobs64w`, `jobs64g`, 200 s each,
the 512-slot pool):

| Run | Largest walk when the player joined | Largest walk afterwards |
|---|---|---|
| 24, Wake | 169 | 163 to 169 |
| 64, Wake | 251 (of the game's own 256) | 169 to 179 |
| 64, Guadalcanal | 228 | 169 |

So it is joining a big match that nearly fills the game's own pool; one more busy moment and the walk never ends,
as in `size64h`. With 512 both 64 runs played on at 60 FPS (12.4 to 12.9 ms of each frame to spare), no hang.
**A player's game needs this fix (`bf_job_slots`) before it joins a 64 server**, so 64 should not be offered before
the release that carries it.

## 3. 128 players: what it would take (not built)

- **Player ids:** `Game.MaxPlayerCount` 128 means 7-bit ids. Every game joining must switch to 128 before its player
  manager is made, and switch back for a 24 to 64 server. A game from before that change dies joining a 128 server
  (seen at 68). Big servers would need their own version gate.
- **The client's player table** is sized from its own count. Ids past 63 would write past a 64-slot table
  (sub_8226E1C0 does not check).
- **Squads:** 96 a side holds 64 a side, so squads are fine.
- **Ghosts:** 8192 objects; probably enough, but not measured at 128.
- **Scoreboard:** scrolls; 64 rows a side not tried.
- **Host lists:** the comms roster (2048-byte client buffer) and the ping list (64 names, 1400 bytes) would need
  splitting.
- **Cost:** about 2 cores, and 260 kB/s out, per 128 server.

TekRantGaming, 20 Nov 2026: "Drop 128 for now".

## 4. Built

- **server.json / manager:** `max_players` is 2 to 24, 32 (the default) or 64; a value in between goes up to the next
  size. The edit form names them "24 players (original)" and "32 players (default)". A new
  `bots` field (0 to the size) is how many bots play; they keep 2 places free and leave as players join. The edit form
  has Players (adding 32 and 64) and Bots (No bots, Up to 8 / 16 / 22 / 30 / 46, Fill the server). The manager passes
  `--bf_gm_max_players=<size>` to the game, and `--bf_bots=<n>` when there are bots.
- **Players' game:** the job scheduler's pool is 512 (`bf_job_slots`, `frame_rate.cpp`; section 2a).
- **Game server:** on a 32 or 64 server `Network.MaxClientCount` becomes the size + 4 (`frame_rate.cpp`, the
  settings hook) and a team's cap half the size + 2 (`bots.cpp`, a hook on sub_82375918). `Game.MaxPlayerCount` is
  never raised. Bots fill to the size given (`bf_bots` is no longer clamped to 24).
- **Host (`gmhost.py`):** lists of players run to 128. A new line each minute gives each player's traffic.
- **Backend (`plasma.py`):** a server registers as 2 to 64. Its name, score and id lists run to 128.
- **Players' game:** the server browser has a SIZE chip, shown once any listed server is bigger than 24. It goes
  through the sizes listed (24 OR LESS, 32, 64), and "x / 64" shows as before (runs `sizechip2` and `sizechip3`: the
  chip, then "SIZE: 64" lit after a click, with the 64 server's row "0 / 64"). There is no pop-up (his choice). Our
  pause Scoreboard now scrolls (the cursor or the wheel) and says which rows it shows. Before, it drew only the first
  10 rows, even on a full 24 server.

## 5. TekRantGaming's answers (20 Nov 2026)

- 128 players: **Drop 128 for now.**
- Bots past 24: **Host chooses** (the `bots` setting above).
- Maps at 64: **Keep the maps as they are.**
- A warning before joining a big server: **Size shown, no pop-up.**
- Later the same evening: **32 is the new default, official servers too; 24 stays as the "original" option.**

## 6. Open

- Fixed 21 Nov: what looked like a 12-row scoreboard was the per-team cap of 12 (section 1). Runs before the fix
  (`size32a`, `size64b` to `size64g`) had bots beyond 12 a side sitting with no team; their numbers were replaced.
- **Backfill at exactly 64:** the player manager stops at 64 players in all. Bots leave at their next death, so when
  the 63rd and 64th players arrive while a bot is still alive and leaving, the newest player may not get in until that
  bot dies. 32 has room to spare (the game's cap is 64).
- **Not tested:**
  - 64 real players, only bots;
  - the Linux server at 32 or 64;
  - Custom Match (`gm_host.cpp`), which keeps its own 24.
