# Bots and Custom Match options

Added from 18 to 19 Nov 2026 on `local/pc-improvements`, and still being worked on. This page describes the code as it
was on 19 Nov 2026; upstream's bot changes of 20 Nov, merged on 20 Nov, are summed up just below and change some of
what follows. Where something has only been seen in developer test runs, or not at all, it says so.

### Upstream's changes of 20 Nov 2026

Bots now run on every server as back-fill (TekRantGaming's commits, merged into this branch on 20 Nov):

- **Back-fill** (0fb2d849): bots take the room humans leave but 2 slots, so a joining player always gets in. Past that,
  a bot leaves from the side with more players (a dead one at once, a living one at its next death), through the game's
  own removal of a player; when a player leaves, a bot joins again.
- **The [BOT] tag** (7d2ce31b): every bot's name starts with the clan tag `[BOT]`, in the scoreboard, kill feed and name
  tags. "bot" is a reserved tag that players cannot take (in the game's text filter and the backend's).
- **Waiting on an empty server** (7d2ce31b): with no human on the server the bots stand still and cost almost nothing,
  until a player joins.
- **The landing craft** (26b9992d): a bot whose flag is across the sea takes a boat from its carrier, as driver or
  passenger, waits a few seconds for others, drives the way players drove (or straight for its flag) and gets out on
  the beach with the game's own button. Bots riding with a player get out where the player stops.
- **End of round names** (4e189d87): each bot gets a user id of its own, so every bot shows under its own name at a
  round's end (the "same name in every slot" bug).
- Through round changes, Linux servers, and an optimization pass (2ef4482f, 2c956e16, 350542a6).

In the merge, bots in a tank's or a plane's driver's seat drive them with this branch's code (b28008c6), and every
other seat uses upstream's riding and boat code.

- **Bots**: computer players that join a match, take flags, fight, learn each map from the players and talk in the
  text chat. Decision D198, mostly `bf/src/bots.cpp` and `bf/src/bot_chat.cpp`.
- **Custom Match options**: game mode, a map rotation submenu, tickets, match time and a dynamic storm, beside the
  Bots row. Decision D209, mostly `bf/src/match_rules.cpp`, `bf/src/gm_host.cpp` and the Custom Match page in
  `bf/src/game_menu_launcher.inc`.

The decisions are in `bf/third_party/native-x-renderer/docs/decisions.md`. How dedicated servers are run in general is
in [SERVER.md](SERVER.md).

## Bots

### What a bot is

The game has no AI of its own: the engine's AI data types are still in the executable, but no code thinks, finds a
path or aims for a soldier. So a bot here is a player made by the server itself (D198):

- It is made by the game's own player manager, with no network connection behind it, and given a team, a kit and a
  spawn point the way a joining player is.
- Once every server tick it is fed input as a client's message would bring it (the sticks, the buttons and the aim),
  through the same input step the game uses for a human.
- Everything runs in the server process. Every player's game sees ordinary players: soldiers in the world, names on
  the scoreboard, kills in the kill feed. Nothing has to be installed on the players' side.

Because a Custom Match is a hidden dedicated server started by the player's own game, the same code gives bots for
offline or LAN play and for real servers.

### How bots join

- **How many:** the Bots row of Custom Match (0 to the match's size), or `--bf_bots=<n>` on a dedicated server. 0 (the default)
  means no bots.
- **When:** about 10 seconds after a level is up. A bot that dies spawns again about 5 seconds later.
- **Which side:** each bot joins the side with fewer players at that moment, human players counted, so they come out
  half on each side. A bot keeps its side through the round's start (the game's own round start would otherwise
  re-team every player).
- **Never more than the free slots:** the server adds bots only up to the room the human players leave, counted
  against `bf_gm_max_players` (24 by default). This came from a match on 19 Nov 2026 with 23 bots and the player: one
  side was full and the 23rd bot got no team and never spawned.
- **Late joiners:** since 20 Nov a bot leaves to make room for a joining player (see "Upstream's changes" above).
- **Where they spawn:** at their own side's soldier spawn points. Spawn points that put the soldier straight into a
  vehicle are skipped once seen.
- **Kit:** one of the side's three kits, picked by how often human players pick each one (see "What bots learn").
- **Names:** a first and a last name for their side, from our own lists of common 1940s names: American given name
  first for the Marines, Japanese family name first for Japan (for example "Eugene Ross", "Fujita Makoto"). At most 15
  characters, the limit of a player's name, and never the same as another name on the server. Since 20 Nov every bot
  also wears the clan tag `[BOT]` (see above).
- **Idle kick:** bots are never kicked for idling.

### Flags and objectives

- A bot goes for the nearest flag its side does not hold and captures it by standing in its capture area. It keeps
  that goal until its side holds the flag, for 60 seconds at most, so it does not turn back and forth.
- Part of the time it defends instead: it goes to its side's nearest held flag. How often depends on how much the
  human players attack or defend (see "What bots learn").
- At a flag it moves between spots, new ones every 6 to 12 seconds. Two times in three it picks a spot its side has
  fought or waited from before, and it faces where enemy kills came from.
- If it cannot reach the flag's pole (two plans in a row ended short of it), it goes to a spot 6 to 14 m round it.
- Only Conquest flags are known to the bots. They play no other objective yet (see "Known gaps").

### Fighting

- **Sight:** enemies within 90 m, inside a field of view of 135 degrees (or within 12 m, as if heard), with nothing
  of the level in the way of the head, chest or hips. An enemy lost from sight is still fought for 2.5 seconds, at
  the place it was last seen.
- **Aim:** a small error at first that shrinks while the enemy stays in sight. The bot turns at most 3.5 radians a
  second.
- **Fire:** bursts, then short pauses. Burst length, the use of the sights, fighting range, side steps and sprinting
  follow the human players' measured style, each bot with its own variety (0.75 to 1.25 of each trait).
- **Range:** a bot closes in on an enemy further than its preferred range and backs off from one much nearer.
- **Dynamic aggression (`bf_bots_dynamic_aggression`, on):** when the human players on the other side are well
  ahead, a side's bots press harder. Every 2 seconds the server compares the humans' kills of that side's bots with
  its bots' kills of humans (older kills fade, half gone in 2 minutes) and the flags each side holds. The side's
  aggression, from 0 to 1, rises toward that over 90 seconds at most and falls back over 3 minutes. Level ground, or
  humans only a little ahead, keeps it at 0.
  - From 0.25: a bot that sees an enemy tells the bots of its side, which face that way as they go (for 5 seconds),
    and bots attack more often than they defend.
  - From 0.5: bot-led squads join each other's attacks on one flag and defend less, a squad leader waits 50 to 100 m
    short of the flag it attacks for two of its squad (12 seconds at most), two bots in three come at a flag from the
    side (a point 30 to 60 m off the straight way), and a third bot may take a tank. A bot squad leader calls the push
    in Team chat.
  - From 0.6: a bot goes after an enemy another bot saw within 45 m.
  - All the way up: the bots fight closer (preferred range down to 70%), aim up to 30% better, settle their aim faster,
    sprint more and pause less between bursts.
  - Not yet seen in a match.
- **Weapons:** a bot reads its kit's weapons in slot order (the soldier data's weapon list at +0x1B8, its count at
  +0x1B0; until 26 Nov 2026 the list was read at +0x1A8, which held no weapons, so no bot ever used anything but its
  gun) and tells the anti-tank weapon (M18, M18_jap), the rifle grenade (M1Garand_rgl, Type5_rgl) and a thrown grenade
  (US_Pineapple_Gren, Jap_Type97_Gren) apart by their names; the server log lists each kit's weapons as the level
  starts. An enemy in a tank is fought only with the anti-tank weapon or a rifle grenade (15 to 140 m); a bot with
  neither left leaves the tank alone and goes on with its flag. It picks a slot with the game's SelectWeapon1 to 4 (run weap1: the
  server took them; the pad's Y swap is actions 9 and 19 together) and waits about a second before firing. The
  anti-tank weapon goes at an enemy in a vehicle within 150 m (a plane only within 120 m and 40 m over the bot), the
  rifle grenade at an enemy on foot 35 to 140 m off, a thrown explosive at one 8 to 25 m off just lost behind cover.
  One shot at a time, aimed up for the distance, 5, 6 and 3 a life at most; back to its gun when the fight ends. A
  bot's kills are logged with the weapon that made them. The names matched, the slot order and the arcs (shots at 70,
  40 and 18 m/s) are guesses until a match's log shows them.
- These numbers are fixed in the code for now. There is no difficulty setting yet.

### Getting around

- No navigation data ships with the game, so the bots build their own: a grid of 2 m cells learned with the game's
  own ray test (its collision), and an A* search over it.
- The first match on a map scans the whole reachable level once, from every spawn point and flag, a budget of rays
  each tick so the server keeps up (Guadalcanal took 86 seconds while the match ran). The grid is saved and loaded
  whole in later matches.
- Where human players walk becomes walkable ground for sure, whatever the rays found.
- Water is swimmable once its height is known, at ten times the cost of a step on land (three until 26 Nov 2026:
  on Wake the swim across the lagoon then beat the walk round it). A cell walked by a swimmer counts as water, not
  ground: before, swimmers' tracks were learned as ground, lines of "land" across the lagoon and out from the
  carriers that led bots into the water.
- **The way to each flag over the whole level** (26 Nov 2026): every three minutes the server works out, on a thread
  of its own, every cell's cost to reach each flag. A bot more than 70 m from a goal at its flag plans its path only
  to a point about 80 m along that way, so a long way round (Wake's lagoon) is always found; the path search alone
  looks at 4,000 cells, and Wake's walks round the lagoon need 14,000 to 31,000. The server log says "the ways to N
  flags worked out".
- A bot that gets stuck closes that step for a while and plans round it. Stuck in front of something low (blocked at
  the waist within 1.5 m, clear over the head), it knifes it for 2 seconds (B, as a player's B on foot), twice at most,
  before going round: a fence the game lets break falls. Not within 6 m of a vehicle, where B gets in it.
  Until 28 Nov 2026 ([issue 86](https://github.com/TekRantGaming/Battlefield-1943-Reborn/issues/86)) this never ran:
  the clear ray over the head failed the test, and the press added an input no button sets. In a 5 minute Wake match
  with 23 bots it now knifes about 70 times, and about half of those bots are moving again 10 seconds later; a fence
  seen falling to a bot's knife is still to be shown in a screenshot.
- A soldier standing in a boat is not walking: its track over the water is not learned as ground. Such tracks drew
  lines of "shore" out from both carriers, where bots then got off their boats.

### Squads

- Every 2 seconds a bot in no squad joins one of its side with room (4 a squad): one with a human player first, then
  one with bots, else a new one.
- A human player in no squad is put in a squad of its side with bots and room, and made its leader
  (`bf_bots_squad_humans`, on by default). Without such a squad the player chooses their own.
- Squad mates show in the brighter squad green, and the game's own name tag over a soldier shows a bot's name.
- Bots do not follow a squad leader's orders yet: each picks its own flag. They acknowledge an order in the chat
  ("Roger that") without acting on it.

### Vehicles

- **Riding along:** a bot within 6 m of a vehicle of its own side that a human drives (a swimmer next to a passing
  boat too) takes the first free seat other than the driver's, through the server's own seating. Checked twice a
  second.
- **Getting off** (27 Nov 2026): a passenger gets off when the vehicle has come 25 m or more and stopped (stood 2
  seconds, or moved under 4 m in 10 seconds: a boat grinding in the surf), when the driver's seat has been empty a
  second, or when the vehicle has not moved for 20 seconds, whoever drives. A bot boat driver gets off on the beach
  the same way.
- **Not at sea** (28 Nov 2026, [issue 82](https://github.com/TekRantGaming/Battlefield-1943-Reborn/issues/82)): the
  passengers of a boat a bot drives stay aboard unless a shore is within 40 m (120 or more cells of ground at most
  2.5 m over the water: Wake's beaches have 198 to 527, beside a carrier there are about 50), and a bot driver stuck at
  sea backs off up to 12 times (every other time the other way) before giving the boat up. Before, the landing boats
  that got stuck beside the Japanese carrier were left there within 30 seconds, and their crews swam 400 m to the
  island: 18% of the bots' positions in a 5 minute Wake match were swimming.
- **Waiting on the deck:** a bot on its carrier with no boat to take, whose flag is 250 m off and mostly over the sea,
  waits on the deck for the next boat, up to 90 seconds into its life, instead of walking off the deck and swimming.
- **A landing craft's places 3 to 5** keep the soldier as what its player controls (its own weapon in hand), and no
  button gets a soldier out of them. Bots now board boats only into seats 0 to 2 (the driver's and the two guns').
  A bot that is in such a place anyway is found by the boat's seats, stands still, and on the beach is moved by the
  server's seating to a gun seat the others have left, from which it gets off. Before this, a bot in such a place was
  taken for one on foot, rode to the beach and then walked against the boat for the rest of the match.
- **Tanks (new, commit b28008c6):** a bot within 20 m of a free Sherman or Japanese tank may take its driver's seat,
  two bots a side at most. It drives to the nearest flag its side does not hold and stops about 18 m short, follows
  the ground human players drove that tank over where that is shorter, steers round what is in front of it, and fires
  the gun at the nearest enemy soldier it can see within 150 m, a shot every 2.5 seconds. Something within 7 m ahead:
  it steers for the clearest of six ways up to a radian either side and slows down; not moving for 3 seconds, it backs
  off 1.5 seconds and turns hard 2 seconds (26 Nov 2026: a hard turn alone before; stuck tanks in a 6 minute
  Guadalcanal match with 16 bots went from 85 to 47).
- **Planes (new, commit b28008c6):** one bot pilot a side at most, in a Corsair or a Zero within 40 m. It takes off,
  climbs to about 100 m over the flags, banks toward the nearest flag its side does not hold and the enemy soldiers
  near it, dives on them, fires when lined up and pulls up about 75 m over the ground. On the Japanese airfield of
  Guadalcanal the ground ray found nothing, so a Zero on the runway read as 400 m up, banked at 19 m/s and crashed at
  every take-off (the player's match of 23 Nov 2026). Its height now counts from where it got in when the ray finds
  nothing, and it is flying only over 30 m up and 25 m/s. Not yet seen in a match.
- Tanks and planes were seen working only in developer runs (tank1, tank2, plane1 to plane3, night1). The player's
  report of 19 Nov 2026 that bots do not drive or fly was made before this, and the new code has not been confirmed
  in the player's own matches (see "Known issues").
- Tank drivers also fire at enemy vehicles in sight (aimed at the middle of the hull), whichever is nearest, a
  vehicle or a soldier.
- **Bailing out:** a bot in any seat watches its vehicle's health (the weakest of its health zones, read from the
  vehicle's own health object) and how fast it falls. Below 12%, or falling fast enough to be gone within 3 seconds
  (under 45%) or 6 seconds (under 30%), it gets out with the enter/exit button. It stays in if that does more for its
  side, while above 6%: its tank is finishing an enemy vehicle weaker than its own and under 25%, or it sits in the
  area of a flag its side has 75% or more taken. It also stays if getting out would not help: a plane 4 to 35 m up
  (too low for the parachute, too high to jump), or open sea with no flag within 300 m. Out of a plane higher up, it
  opens its parachute with A once it starts to fall (from 0.4 seconds out), and presses again 1.5 seconds later
  while still falling faster than 12 m/s, three tries at most. Bots do not board a vehicle under 40% health (a ride
  along: under 30%), nor one they bailed out of in the last minute.
- Tested with `BF_BOT_BAILTEST=1` (developer builds: every driver's vehicle counted as doomed 25 seconds after it got
  in), 23 Nov 2026. Run bail2: drivers of tanks and planes got out at once, and the bail lines came in Squad chat; the
  pilots died in the fall (one RB press, no parachute). Run bail3: A opened the parachute (40 m/s down to a steady
  8.7 m/s) and pilots who got out over 110 m up landed alive; RB did nothing, and a pilot out at 61 m died with the
  A press too late. The bots now press A first. A real bail (a vehicle shot to pieces) has not been seen yet.
- Jeeps, AA guns and other fixed guns are not driven or manned by bots.

### Chat: callouts and banter

Bots write in the text chat (`bot_chat.cpp`, D198's chat step, commit 34d0f2c1). Every line is our own writing.

- **All:** trash talk or a good sport's word after a kill or a death, a note on a long shot (over 120 m), jokes at a
  tank that killed them, and the other side joking about hacks during a human's infantry kill streak (at 5, 8, 12,
  then every third kill).
- **Team:** flags taken and lost ("We lost B (Village)! Need people there."), told by a squad leader of that side
  where there is one; praise for a teammate's long shot or streak; now and then a jab at a teammate who died again
  within 25 seconds.
- **Squad:** praise for a squad mate's kill, and a bot squad leader naming its next goal to its squad.
- **Replies:** a bot that can read the channel answers a player's line: greetings, thanks, "gg", praise, taunts,
  "hacks", "are you a bot", a call for help (it names the flag it is heading for), orders, and some questions. Lines it
  does not recognise get no answer.
- Each bot has its own temper by its name: a good sport, cheeky or salty.
- To keep the chat readable: a line waits 1 to 4 seconds before it is sent, a channel carries a bot line every 4 to 6
  seconds at most, a bot speaks at most once in 15 seconds, and only a share of events get a line.
- `bf_bot_chat` (on by default) turns all of it off.
- Bots have no voice. Their lines go out through the match's own host inside the game (`gm_host.cpp`), so they reach
  players in a Custom Match. A server run by the server package's `gmhost.py` does not carry them (see "Dedicated
  servers").

### On the scoreboard and in stats

- Each bot gets the score record a joining player's connection gets, so the in-match scoreboard lists bots with their
  kills and score (run score2: 12 a side).
- Each bot gets its own online id, so that what the game keeps by that id (the end of round scoreboard) tells them
  apart. This is the fix for every bot showing the same name at a round's end; it has not yet been seen at a round's
  end in a test (see "Known issues").
- Bots are kept out of the players' stats (the service record): a kill of a bot is counted as `botkills` and adds no
  score, a death to a bot is not counted, and bots get no stats or awards of their own. The `botkills` count is written
  to the server's stats file; a Bots counter in the backend and the game's menus is not built yet.

### What bots learn

Bots get better on a map with every match played on it, and they copy how the human players play (D198, commits
c18846b4, ed504263, 22a68ecd). It is kept in the match server's user folder, under `bots/`:

| File | What it holds |
|---|---|
| `bots/<map>.grid` | The level's navigation grid as scanned: cells, their ground height, open and closed steps. |
| `bots/<map>.txt` | Ground soldiers walked and how much each step is used (a human's walk counts 1, a bot's 0.5); kills made from and deaths at each cell, by side; where humans waited with an enemy near; the ways humans took into each flag in the minute before they captured it; human players' use of each vehicle and seat (time, where it went, where it killed from and how far). |
| `bots/style.txt` | The human players' style on all maps: sprinting and standing still, firing and with the sights, burst lengths, kill distances, kills from an enemy's side or back, attacking or defending near flags, and kits. |

- `<map>` is the map's title with anything but letters and digits made `_` (for example `Wake_Island`).
- For a Custom Match that folder is `private_match/user/` in the game's user folder; each server of the server package
  has its own.
- Files are saved every 20 seconds and at a level change. Older matches' style counts less at each load (times 0.7).
- In the bots' choices: often used steps and a flag's known ways in cost less on a route, cells where their side died
  cost more; at a flag they hold the spots their side fought and waited from, and watch where the enemy's kills came
  from.
- The match's own short memory: deaths and the enemy's firing spots of the last couple of minutes (fading over about
  120 seconds) make bots route away from them and watch that way.
- Kills from vehicles are kept apart from the soldiers' kill spots and the infantry style. What is learned about the
  players' vehicles is used by the bots' tanks (the ground players drove a tank over).
- Every value read back is checked: finite, not negative, cells within range (a weight of -1 or less made the route
  search's costs -inf or NaN). A step left of x = 0 is written as x + 2^29 (`CellX(key >> 3)` keeps 29 bits); the
  readers turn it back.

### The shared bot data

TekRantGaming, 26 Nov 2026: "we do this for the official servers only, and then we can, when we do like hot fixes and
releases, also include updated bot information for community servers". The plan and the reasons are in the shared
notes (`bots-shared-learning-plan.md`).

**The flow.** On an official server the game hands what its bots learned since the last hand-off to the main
backend; the backend merges every official server's hand-offs into one set and sends it back to them; the bots use
the newest set from their next level (the next map or round), so a server never restarts for it. Every release
carries the latest set beside the program, so community servers and offline Custom Matches start from it too.
Nothing is sent by, or to, a community server.

```
 official server: bots.cpp ──► bots/outbox/ ──► gmhost.py (BotShare) ──► main backend: plasma.py (BotData)
                                                                              │ botdata.py merge (own process)
 bots/network/ ◄── gmhost.py checks the signature ◄── signed set (out/<v>/) ◄─┘
 every package: bf/data/bots (botdata.py install) ──► <program>/data/bots/ ──► community servers, Custom Match
```

**The layers** (`bots.cpp` LoadLearned). `own` is the server's own learning (`bots/<map>.txt`, as before). The bots use
own + the shared set − `sent` (what of own an official server already handed in: `bots/sent/<map>.txt`), so nothing
counts twice. The shared set is the newer (its `version.txt`) of `bots/network/` (an official server's, written by its
host) and `<program>/data/bots/` (shipped). A server whose own grid is missing or only scanned in part takes the
shared set's finished grid: no scan of its own (it saves 0.5 to 2 ms a tick until a map is mapped).

**Hand-offs** (`bots.cpp` ShareLearned, only with `bf_bots_share`, which the server manager sets on an official
package): every `bf_bots_share_minutes` (15) and at each level change, `own − sent` goes into
`bots/outbox/<map>.<ms>.txt`, and the match's style since the last one into `bots/outbox/style.<ms>.txt`; a finished
level grid goes once into `bots/outbox/<map>.<ms>.grid`. A hand-off waits until `bf_bots_share_min_players` (3)
different human players have played the map since the last one, so none is one player's routes; the names are
counted in memory as hashes, never written. The host sends the outbox (zlib, base64, lines under 48,000 characters)
and deletes each file when the backend has it; a file the backend refuses goes to `bots/outbox/refused/`.

**The merge** (`server/botdata.py`, under the main backend's `--bot-data` folder; the manager gives
`<data>/botdata` on the official machine). The backend keeps each hand-off as it came (`in/<day>/<server>/`) and,
every `--bot-data-every` seconds (600) when something came in, runs `botdata.py merge` in its own process: every line
is checked again, the totals are faded (each count times 0.97 a day: about half after three weeks) and the new
hand-offs added; the most complete finished grid of each map is kept. The set goes to `out/<version>/` (each map's
`.txt` and `.grid`, `style.txt`, `version.txt`, `manifest.json` with each file's size and SHA-256, and
`manifest.sig`, signed with the network key made from `official.key`); the last three are kept. Hand-offs are deleted
after 90 days (the privacy page). `botdata.py rebuild --exclude <server>` makes the totals again from the hand-offs
kept, leaving one server out. Totals that start empty (a new machine, or the folder lost) start from the set shipped in
the backend's own package (`bin/data/bots`, `--seed`), so a new set never holds less than the last release's.
**A deploy to a new folder copies `<data>\botdata\` from the old one**, like the accounts and leaderboard files.

**Sending it back.** A new set goes to every official server over its registry link; a server that registers says
which set it has (`bot_data_have`) and gets a newer one at once. The host checks the manifest's signature against the
official machine's keys and every file's checksum, then writes `bots/network/`, `version.txt` last.

**A release.** Copy the newest set from the official machine (`<data>\botdata\out\<latest.txt's version>`), then
`python server/botdata.py install --from <that folder> --to bf/data/bots` (checks the signature; replaces the old set)
and commit `bf/data/bots`. The build copies it into `data/bots` beside the program; `tools/make_packages.ps1`, the
AppImage and `tools/linux/build_server.sh` carry it (both packages; a build without it warns). With no set there, the
bots learn as before.

**Size.** A set holds each map's learning (Wake from one busy server: about 280 KB of text), its finished grid (Wake:
5.3 MB) and the style: a few MB in all. A hand-off is a few KB to a few hundred KB.

**Privacy.** No names, ids, addresses or times are in any of it: counts per 2 m cell, sides 1 and 2, flag and vehicle
names. The backend keeps which official server handed each file in (its folder name) for 90 days, for `rebuild`.

**Switches.**

| Setting | Where | Default | What it does |
|---|---|---|---|
| `bf_bots_shared_data` | cvar; manager Settings "Bots start from the shared bot data" | on | Off: the bots use this server's own learning only. |
| `bf_bots_share` | cvar (the manager sets it on an official package) | off | Hand what is learned here to the shared set. |
| `bf_bots_share_minutes` | cvar | 15 | Minutes between two hand-offs. |
| `bf_bots_share_min_players` | cvar | 3 | Different human players on the map before a hand-off. |
| `--bot-data`, `--bot-data-every` | `plasma.py` | off, 600 s | The merge's folder (needs `--official-key-file`), and how often it runs. |
| `--bot-data-dir` | `gmhost.py` | beside `--map-file` | The game's `bots` folder. |

**Tests** (26 to 27 Nov 2026):
- `botdata.py` offline: bad lines left out, fading, the signature, a changed file caught, install, rebuild without a
  server, 90 days, an empty backend seeded once from the shipped set.
- A test backend and two hosts on 127.0.0.5 with a TEST key: the official host's outbox in, merged, the signed set back
  and installed in 33 s; the community host handed nothing in and got nothing.
- In game (run bs2: a dedicated server on Wake with 8 bots, one client, the test backend and an official test host;
  hand-offs every minute, a round ended by `BF_DEV_TICKETS`): round 1 took the shipped set (17,408 entries) and its
  finished grid (353,897 cells, no scan); five hand-offs went in, each merged and sent back within 10 s and checked;
  round 2 took "the official servers' latest" set (no restart), with its own learning counted once (own 3,541 entries,
  all handed). The bots' cost stayed at about 0.7 ms a tick; a level's load (the 5 MB grid read) took about 120 ms of
  one tick, as reading a server's own grid did before.

### Bot settings

| Setting | Where | Default | What it does |
|---|---|---|---|
| Bots | Custom Match row; `--bf_bots` | 0 | How many bots (0 to 24). |
| `bf_bots_squad_humans` | cvar | off | A human in no squad is put in a squad with its side's bots, as its leader. |
| `bf_bot_chat` | cvar | on | Bots write in All, Team and Squad, and answer players. |
| `bf_bots_dynamic_aggression` | cvar | on | Bots press harder when the human players are well ahead (see "Fighting"). |

There is no difficulty setting yet, no Co-op mode (bots on one team only) and no bot settings in the
server manager. They were asked for (D198) and are not built.

## Custom Match options

The **Custom Match** page hosts a match from the game: a hidden dedicated server on this PC that the player's game then
joins (`bf/src/private_match.cpp`). On 19 Nov 2026 it got a game mode, a map rotation submenu, tickets, match time and a
dynamic storm (D209), beside the Bots row of 18 Nov. The page's list scrolls when its rows are more than it holds.

The rows in order: Server name, Password, **Game mode**, Map (only while the rotation is off), **Map rotation**,
**Tickets**, **Match time**, Tick rate, **Bots**, Weather and time, Network type, then Show in browser and UPnP (not for
LAN), Port and Host the match. Since 27 Nov 2026 (hotfix 0.4.8.1) the Match tab's rows are Server name, Password,
**Server type**, **Server size**, Tickets, Match time, Bots, Tick rate, Show in browser, UPnP (Port) and Weather and
time; the game mode and the rotation are in the page's middle and right columns.

| Row | Choices | Default |
|---|---|---|
| Game mode | Conquest, Air Superiority, All modes | Conquest |
| Map | the maps of the game mode | as last chosen |
| Map rotation | opens its own list: Rotation, Rotate all maps, one row a map | off (all maps ticked) |
| Server type (27 Nov 2026) | Classic, Night Ops (18:30), Daybreak (06:10) | Classic |
| Server size (27 Nov 2026) | 24 (original), 32 (default), 64 | 32 |
| Tickets | Auto, the map's own (130, Coral Sea 100), 25, 50, 75, 100, 150, 200, 250, 300, 400, 500, 750, 1000 | Auto |
| Match time | off (tickets only), 5, 10, 15, 20, 25, 30, 40, 45, 60, 90, 120 minutes | off |
| Bots | 0 to the server size, in steps of 1, shown as "8 - 4 A SIDE" | none |
| Dynamic storm (in Weather and time) | on or off | off |
| Storm type (in Weather and time, D224) | Storm (a thunderstorm), Hurricane | Storm |

### Game mode

Each map has one mode in the game's data: its level holds that mode's objects and no other's, and there is no mode
switch in the game. So the mode chooses the maps:

- **Conquest:** Wake Island, Guadalcanal, Iwo Jima.
- **Air Superiority:** Coral Sea.
- **All modes:** any of the four.

Changing the mode moves the Map row to a map of that mode.

### Map rotation

Map rotation opens a list of its own:

- **Rotation:** on or off. Off, the match plays the one map from the Map row, round after round. Switched on with no
  maps ticked, it ticks every map of the mode.
- **Rotate all maps:** switches the rotation on with every map of the game mode.
- **Wake Island, Guadalcanal, Iwo Jima, Coral Sea:** one switch each. A map of another mode shows "CONQUEST ONLY" or
  "AIR SUPERIORITY ONLY" and cannot be ticked. Ticking a map while the rotation is off starts a rotation of that map
  alone; unticking the last one switches the rotation off.
- **Back to the match.**

With the rotation on, the Map row is hidden and the Map rotation row shows "ALL n MAPS" or "n MAPS". The maps play in
turn, the next one after each round's end screen (45 seconds for a Custom Match). Settings saved before 19 Nov with
"all four maps in turn" become a rotation of all four in All modes.

### Tickets

Each team's tickets at a round's start. "The map's own" keeps the level's count: 130 on the Conquest maps, 100 on
Coral Sea. The server writes the chosen count into the game's own ticket counters, so the ticket bleed, the "tickets
low" warnings and the end of round all use it. Players' HUD ticket bars are scaled to the match's count.
Since 27 Nov 2026 "Auto" is the default, as on the servers of the server manager: the map's own grown with the
server size (175 at 32, 345 at 64; `--bf_match_tickets=0`); "the map's own" passes `-1`. A setting saved as 0 before
(the map's own at 24 players, the only size then) now reads as Auto, the same count at 24.

### Server type and server size

(TekRantGaming, 27 Nov 2026: "custom match screen parity in settings and bot counts ... a toggle for daybreak and
nightops presets like the servers have".) **Server type** runs the server package's events in the match's own host
(`gm_host.cpp`, `--bf_world_event` 1 or 2; the schedules are `server/gmhost.py`'s `NIGHT_OPS_STAGES` and
`DAYBREAK_STAGES`): the clock held at 18:30 or 06:10, a clear sky that clouds over from 40% of the round (the share of
its tickets the team with fewer has lost, `match_rules.cpp`), the storm rolling in from 55% and full from 80%; each new
round clear again. The host answers every player's game with the event's flags (4, 8 while an admin holds a setting, 16
Daybreak), so their games draw it, the F9 panel shows "Back to Night Ops" and the browser's TYPE says it (`night` or
`daybreak` in the registration). With an event the Weather tab is closed (the schedule sets the world). Only in builds
with `BF_NIGHT_OPS` (all of them since 0.4.8). Tests: `BF_DEV_EVENT_RAMP=<s>` runs the whole build in that many
seconds (dev builds; passed on to the match's server).

**Server size** passes `--bf_gm_max_players` (24, 32, 64) as the server manager does: the players' connections and each
team's cap grow with it (docs/SERVER-SIZES.md), the bots fill up to it (two places always kept free) and Auto tickets
follow it. The rail's plain rows share its height (26 to 37 units) so both new rows fit.

### Match time

How long a round lasts. When the time is up, the team with fewer tickets loses all the rest, through the game's own
loss of tickets, so its own end of round, winner and next map follow. With both teams level, the next ticket lost
decides. Every player sees the time left above the ticket bars, red in the last minute and "OVERTIME" when level at 0.

Off ("OFF - TICKETS ONLY"), a round ends only when a team has no tickets left, as in the original game: no level has a
time limit of its own.

### Dynamic storm

Under Weather and time, after Weather. On, the weather builds from the Weather setting to a full storm over each round
and eases again: a smooth rise and fall over the match time, or over 30 minutes (repeating) without one, with a slow
swell on the way. Each new round starts it again. The wind follows it while the wind is set the same as the weather.
The F9 panel shows the weather moving and can switch the storm; moving the Weather slider by hand switches it off.

### Where the choices are kept

In the game's settings file, as `private_match_bots`, `private_match_size`, `private_match_event`, `private_match_mode`, `private_match_rotation`,
`private_match_rotation_maps` (one bit a map), `private_match_tickets`, `private_match_time_limit` and
`private_match_dynamic_storm`. Hosting passes them to the match's server as command-line flags.

## Dedicated servers

A dedicated server takes the same things as command-line flags of the game (`bin\bf1943.exe --server`):

| Flag | Default | Range | What it does |
|---|---|---|---|
| `--bf_bots=<n>` | 0 | 0 to 24 | Bots in the match. |
| `--bf_match_tickets=<n>` | 0 | -1 to 9999 | Each team's tickets. 0: automatic, the map's own scaled to `--bf_gm_max_players` past 24 (below). -1: the map's own at any size. |
| `--bf_match_time_limit=<minutes>` | 0 | 0 to 600 (0: none) | A round's time limit. |
| `--bf_world_dynamic_storm=true` | false | | The dynamic storm (run by the match's host, see below). |
| `--bf_bots_squad_humans=true` | false | | Players in no squad are put in squads with bots. |
| `--bf_bot_chat=false` | true | | Bots stay quiet. |
| `--bf_bots_dynamic_aggression=false` | true | | Bots do not press harder when the players are ahead. |
| `--bf_gm_max_players=<n>` | 24 | | The most players; bots fill only what humans leave of it. |

### Automatic tickets

TekRantGaming, 24 Nov 2026: on a bigger server more soldiers die, and a team loses a ticket for each one, so the
map's own count, made for 24 players, ran out much sooner. With `--bf_match_tickets` at 0 (the default), the server
scales the map's own count to its size (`--bf_gm_max_players`), to the nearest 5 (`match_rules.cpp` AutoTickets):

| Server size | Wake Island, Guadalcanal, Iwo Jima | Coral Sea |
|---|---|---|
| 24 and fewer | 130 (the map's own) | 100 |
| 32 | 175 | 135 |
| 64 | 345 | 265 |

The flags' bleed (about 4 tickets a minute while the enemy holds 3 of the 5 flags) is the level's and is not
scaled. A count chosen by the server's owner (above 0) is used on every map as it is, and -1 keeps the map's own at any
size. The server log says which it used ("BF match rules: the US's tickets 175 (automatic for this size; the level's
130, a server of 32)").

What works where:

- **Custom Match** (the game's own host, `gm_host.cpp`): everything. The game passes all of these itself.
- **Server package** (`server\manager.py` and `server\gmhost.py`): the manager passes the size, the bots and, since 24
  Nov 2026, the tickets (server.json `tickets`: `"auto"`, the default; `"map"`; or a number; the edit form's
  "Tickets, each team", whose Automatic choice shows the count for the Players chosen). gmhost.py reads each round's
  count from the server's `match.txt` and sends it to the players' games, so their HUD ticket bars fit it. The
  manager has no page settings for the other new options. The manager adds the game flags in the
  `BF_SERVER_GAME_ARGS` environment variable (separated by spaces) to every server it starts, which is meant for tests
  and tuning. From the code, not tested on a package server:
  - Bots, tickets and the time limit are run by the game server itself, so the flags take effect.
  - No clock shows for a time limit, because gmhost.py does not send the match's time (D209).
  - The dynamic storm does not run: the storm lives in the game's own host, and gmhost.py keeps its own world without
    one (it ignores the storm byte).
  - The bots' chat lines are not carried: they go out through the game's own host only.
  - The bots' room is counted against the game's `bf_gm_max_players`, which the manager does not set; its Max players
    setting goes to gmhost.py.

## The F9 admin panel

**F9** in a match opens the match admin's panel, for an admin only (`bf/src/world_sync.cpp`). In a Custom Match the
host's game is the admin (the server is given a key that only the host's game holds). On a server, the players whose
IDs are in its admin list are (the manager's Admin player IDs, gmhost.py `--admins`, or `bf_world_admins`). A player
without the right who presses F9 is shown their player ID to send to the server's owner.

Its rows: Time passes, Time of day, A day lasts, Cloud cover, Cloud change rate, Weather, Distance fog, Ground fog,
Wind, Cloud height and, last, **Dynamic storm** (a switch). The mouse works on every slider and box, a value can be
typed in, and the arrows step it. The panel has no bot, ticket or match time controls. The storm switch works only
where the match's host is the game's own (Custom Match). The switch and the host taking it were not tested (D209).

Two more rows (24 Nov 2026, `bf/src/free_cam.cpp`) are the admin's own, not the match's: **Free camera** flies the view
anywhere, through everything, while the soldier stands where he was (controller: left stick flies along the view, right
stick looks, A up, B down, LT faster, RT slower and finer, L3 fast on or off, LB and RB the base speed, Y hides the HUD,
Back ends it; the keyboard's bindings and the mouse work too), and **Hide HUD and UI** hides the game's HUD and every
overlay of ours while the panel and our menus are closed, in first person as well. On a Night Ops server the title bar
holds **Back to Night Ops (N)** (see WEATHER-AND-SKY.md).

## Known gaps

- No difficulty setting, no Co-op mode, no settings for bots in the server manager. Aggression changes by itself
  (`bf_bots_dynamic_aggression`); it has no level to pick.
- Bots play Conquest flags only. Air Superiority with bots, and bots on Coral Sea, have not been tested.
- Bots flank only under pressure (dynamic aggression at 0.5 or more), and do not take cover under fire (the fight has
  only side steps). They use no AA guns or jeeps.
- The level's own bail settings for a vehicle's spawner (`BotBailWhenHealthBelow`, `BotBailOutDelay`) are not read;
  the bots use the fixed thresholds above.
- Bots do not follow a squad leader's orders.
- The Bots row's help text still says that bots fight on foot only and cannot leave Wake Island's carrier or fly; it
  predates the tank and plane code.
- On Guadalcanal some US bots stayed at their base (the hangars) in the first runs (D198, step 1). Not re-checked
  since.
- The service record's Bots counter (backend and menu) is not built; only the server's stats file counts `botkills`.
- The F9 storm switch, a new round restarting the storm, OVERTIME ending on the next ticket, and the mouse on the
  Custom Match page were not tested (D209). The time-left clock also shows on the deploy and loading screens of a
  running round.

## Known issues

Reported by the player on 19 Nov 2026 and not yet fixed or not yet confirmed fixed:

- **Bots do not drive or fly; they only ride along.** Since the report, commit b28008c6 lets bots drive tanks and fly
  fighter planes (see "Vehicles"). It was tested only in developer runs and has not been confirmed in the player's
  matches. Jeeps, boats and guns are still passenger-only.
- **At a round's end every bot showed the same name on the scoreboard, and the voice icons were misplaced.** The
  names are fixed (each bot has a user id of its own; upstream's 4e189d87, tested on the end screen). Nothing has been
  done about the voice icons.
- **Some bots at night showed floating headlights.** Three night test runs did not show it (every lit lamp was on a real
  vehicle being driven); set aside until it is seen again, with a screenshot of the map and the spot.
- **A dedicated server's package (gmhost.py and the manager) does not pass the new Custom Match options**, nor the
  bot count (see "Dedicated servers").

## For developers

### How it fits together

- `bf/src/bots.cpp`, `BotsTick`, runs on the server thread after the game's own tick (called from
  `bf/src/server_rotation.cpp`). It adds bots up to the wanted count and the room left, spawns the dead ones, and for
  each living bot either drives its vehicle (`TankTick`, `PlaneTick`) or thinks four times a second (`Think`: the
  enemy to fight, else the flag and the path) and sends one tick of input (`Drive`).
- A bot is made by the player manager's factory (`sub_82387EF0`, `sub_8238EFD8`), teamed by the game's team pick
  (`sub_82399AE8`), given its kit at player+500 from the level's TeamData, and spawned by the spawn point's own routine
  (`sub_8236B688`). Its input is its own EntryInput at player+280, filled each tick, then the soldier's input step
  (vtable +260), as a connection does. The game's AI player flag (player+532) must stay off: with it a soldier turns
  but never moves (D198).
- Hooks in `bots.cpp`: the team pick (a bot keeps its side), the round's start (bots skipped, so they spawn at their
  own side's points), the death scoring (no record, no crash), the idle kick, the spawn points' and vehicles'
  constructors and destructors.
- Score and name: `sub_8240DFE0` gives a bot its score record (scoreboard); its name also goes to player+0x18 (the
  name tag); its own online id at player+40 (the end of round scoreboard).
- Vehicles: the level's `ServerVehicleEntity` objects are tracked; seats are taken through `sub_82398B98`, the
  server's own seating of a client's request. A seated driver's input is the vehicle's analog concepts (D198 and the
  commit messages list which concept steers what).
- Squads: `sub_8237B320` (squad) and `sub_82375280` (leader), every 2 seconds.
- Stats: `bf/src/server_stats.cpp` asks `BotIsPlayer` and keeps bots out.
- Chat: `bots.cpp` tells `bot_chat.cpp` the kills, the flags' owners each second and the bots each tick;
  `gm_host.cpp` hands it players' lines (`BotsHeardChat`) and sends the bots' lines (`GmHostChat`) through its chat
  relay ("BF43COMM"), which was ported from gmhost.py for the game's own host in the same commit.
- Match rules: `bf/src/match_rules.cpp` hooks the ticket counter's constructor, reset and destructor
  (`sub_8232C780`, `sub_8232C890`, `sub_82321E88`) to write the tickets into InitialTicketCount, keeps the round's
  clock (stopped by `server_rotation.cpp` at the round's end), and calls `sub_82335388(counter, 0)` for the losing
  team when time is up. In a player's game it answers the HUD bar's getter `sub_8222B120` with the match's tickets for
  that one call site.
- The match's world: `gm_host.cpp` runs the dynamic storm and sends the storm flag, tickets, time limit and seconds
  left at the end of its "BF43WRLR" answer; `bf/src/world_sync.cpp` reads them, draws the time left and the F9 panel.
  Older games and hosts read none of the new bytes.
- The Custom Match page (`bf/src/game_menu_launcher.inc`, rows `kPmBots`, `kPmMode`, `kPmRotation`, `kPmTickets`,
  `kPmTimeLimit`, `kPmStorm` and the rotation rows) fills `PrivateMatchOptions` (`bf/src/private_match.h`), and
  `private_match.cpp` turns it into the server's command line.

### Cvars

| Cvar | File | Default |
|---|---|---|
| `bf_bots` | `bots.cpp` | 0 (clamped to 0 to 24) |
| `bf_bots_squad_humans` | `bots.cpp` | true |
| `bf_bots_shared_data` | `bots.cpp` | true |
| `bf_bots_share` | `bots.cpp` | false (the server manager sets it on an official package) |
| `bf_bots_share_minutes` | `bots.cpp` | 15 |
| `bf_bots_share_min_players` | `bots.cpp` | 3 |
| `bf_bot_chat` | `bot_chat.cpp` | true |
| `bf_match_tickets` | `match_rules.cpp` | 0, automatic (clamped to -1 to 9999) |
| `bf_match_time_limit` | `match_rules.cpp` | 0 minutes (clamped to 0 to 600) |
| `bf_world_dynamic_storm` | `gm_host.cpp` | false |
| `bf_gm_max_players` | `gm_host.cpp` | 24 |

### Developer and test switches

| Switch | Build | What it does |
|---|---|---|
| `BF_BOTS=<n>` | any | Bots on a server; overrides `bf_bots` when set. |
| `BF_BOT_INPUTLOG=1` | any | Logs each human's input concepts as they change (how the buttons were mapped). |
| `BF_BOT_VEHLOG=1` | developer (`BF_DEV_TOOLS`) | Logs the level's vehicles, places and seats every 20 s. |
| `BF_BOT_VEHTEST=<vehicle>` | developer | Seats the first bot in that vehicle and drives it through set inputs, logged. |
| `BF_BOT_TANKTEST=1` | developer | The same with a Sherman. |
| `BF_DEV_CHAT=<s>:<all\|team\|squad>:<text>[;...]` | developer | The player's game sends chat lines after the relay answers. |
| `BF_DEV_TICKETS=<after s>[,<offset hex>,<value>]` | developer | Logs the ticket counters; can set them to end a round on demand. |
| `BF_DEV_WORLD_PANEL=<seconds>` | developer | Opens the F9 panel by itself in a test run. |
| `BF_DEV_WORLD_PANEL_TIME`, `BF_DEV_WORLD_PANEL_BACK=<s>` | developer | The time that test types; its click on Back to Night Ops. |
| `BF_DEV_FREE_CAM=<s>`, `BF_DEV_FREE_CAM_SCRIPT` | developer | The free camera on in a test run, flown by a scripted pad (free_cam.cpp). |

- `BF_HOST_LAN_AT_START=1` makes the game host a LAN Custom Match from its saved choices as it starts (the game also
  uses it to restart into a LAN host).
- `.handoff/scripts/run_bots.ps1` (local, not in git) runs a LAN Custom Match with bots from a copy under `runs\`: it
  sets `private_match_bots` in a copied settings file, and `-Learned <folder>` carries the bots' `bots\` files from run
  to run. The bots' log lines (`BF bots: ...`) are in `runs\<name>\user\private_match\server.log`.

### Commits

The main ones, all on 18 and 19 Nov 2026:

- 0088c405 bots step 0 (D198); 01cc63bc names and sides; 8e3013ce flags, navigation, sight and fire, stats;
  1bf498eb the Bots row; c18846b4 learning per map; ed504263 the players' style; 22a68ecd the players' vehicles;
  2b910301 squads; 7f08870b and e3430e42 names in the game's name tag; 32663e0a the scoreboard and the room limit;
  071f54e9 riding along; 34d0f2c1 chat; b28008c6 tanks, planes, fences and online ids.
- 8fdfd4bb Custom Match game mode, rotation, tickets, match time and dynamic storm (D209).
