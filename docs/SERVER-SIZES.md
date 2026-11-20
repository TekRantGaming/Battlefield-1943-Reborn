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
| Per-team cap | a TeamData word (+24 of a team's data) | sub_82375918, used only by the team pick sub_82399AE8 | It steers which side a player gets; the auto-team tries every side without it, so it never blocks a join. Not tied to `Game.MaxPlayerCount`. |
| Squads | 4 a squad (+28, MaxPlayersPerSquad); squads 0 to 23 for each team; 24 means none | sub_82392718, sub_82392828 | 24 x 4 = 96 a side: enough for 32 a side. |
| `Network.MaxGhostCount` | 8192 (13-bit object ids) | game settings, both sides | At 63 players the client's ghost table held about 100 objects when it joined. Not near the limit. |
| `Server.OutgoingFrequency` | 15 | game settings | How often the server sends updates; part of why traffic stays low (section 4). |
| The game's scoreboard (Flash) | 12 rows a side | the HUD's scoreboard movie, fed from `friendlyTeamScores` / `enemyTeamScores` | **Still a limit:** on a 32 or 64 server only the top 12 of each side show, and the cursor stops at row 12 (run `size64e`, shot 104). See section 6. |

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

| Run | Map | Players | Tick work, average (longest) | Server CPU | Client FPS | Traffic to a player |
|---|---|---|---|---|---|---|
| size24a | Wake | 1 + 22 bots | 0.47 to 1.29 ms (3.0) | about 12% of a core | 60 (the limit), 13.6 ms a frame to spare | 1.7 to 2.2 kB/s |
| size32a | Wake | 1 + 30 bots | 0.62 to 1.34 ms (2.8) | 13 to 17% | 60, 13.8 ms to spare | not measured (older host) |
| size64b | Wake | 1 + 61 bots | 0.56 to 1.24 ms (2.9) | 9 to 14% | 60, 13.8 ms to spare | 1.8 to 2.2 kB/s |
| size64g | Guadalcanal | 1 + 61 bots | 0.46 to 0.57 ms (1.7); the bots 2.3 to 3.0 ms on top (0.5 of it mapping the unmapped level) | 17 to 22% | 60, 12.5 ms to spare | 2.6 kB/s |

On Wake many bots wait on the carrier for a landing boat, so they do not all walk at once. The 64-player Wake figures are a light case, and Guadalcanal (both
sides start on land) is the fair one.

The tick work column is the game's own tick; the bots' cost ("BF bots: cost over 30 s") comes on top of it. On Wake that was 0.9 to 1.3 ms with 21 bots and 0.7 to 1.3 ms with 61. On Guadalcanal it was 2.3 to 3.0 ms with 61. Every run had 0 ticks of 25 ms or more.

**What a 64-player server costs to run:** the game's own tick stays small. Bots' soldiers cost the same as players'
soldiers (the soldier input step, about 0.05 ms each a tick). Traffic to each player stays near 2 kB/s because the
game sends at 15 Hz and only what is near. 64 players is therefore about 64 x 2 kB/s = 130 kB/s (about 1 Mbit/s) out
of the server, and about the same in. On this PC the busiest 64 server (Guadalcanal) took about a fifth of a core. US Central's 4-core VPS can
carry a 64 server; plan on a core per 64 server for real players (their connections and replication cost more than
bots'). The players' games ran at their 60 frame limit with most of each frame to spare, at 63 players on screen in the
same match.

## 3. 128 players: what it would take (not built)

- **Player ids:** `Game.MaxPlayerCount` 128 means 7-bit ids. Every game joining must switch to 128 before its player
  manager is made, and switch back for a 24 to 64 server. A game from before that change dies joining a 128 server
  (seen at 68). Big servers would need their own version gate.
- **The client's player table** is sized from its own count. Ids past 63 would write past a 64-slot table
  (sub_8226E1C0 does not check).
- **Squads:** 96 a side holds 64 a side, so squads are fine.
- **Ghosts:** 8192 objects; probably enough, but not measured at 128.
- **Scoreboard:** 12 rows a side (section 6), so 52 of each side would not show.
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
- **Game server** (`frame_rate.cpp`, the settings hook): on a 32 or 64 server, `Network.MaxClientCount` becomes the
  size + 4. `Game.MaxPlayerCount` is never raised. Bots fill to the size given (`bf_bots` is no longer clamped to 24).
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

- **The game's scoreboard shows 12 a side.** On a 32 or 64 server, a player ranked 13th or lower on their side does not
  see their own row. With 32 as the default, this hits every default server: 4 players a side are missing. Possible fixes:
  - page the Flash list: find where the game fills `friendlyTeamScores` / `enemyTeamScores` and show 12 at a time
    from the cursor;
  - draw our own full scoreboard on bigger servers.
- **Backfill at exactly 64:** the player manager stops at 64 players in all. Bots leave at their next death, so when
  the 63rd and 64th players arrive while a bot is still alive and leaving, the newest player may not get in until that
  bot dies. 32 has room to spare (the game's cap is 64).
- **Not tested:**
  - 64 real players, only bots;
  - the Linux server at 32 or 64;
  - Custom Match (`gm_host.cpp`), which keeps its own 24.
