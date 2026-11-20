# The server owner's dashboard

A Dashboard tab in the server manager and an optional public stats page, so a server owner can watch their servers
live and over time. Asked for by TekRantGaming on 20 Nov 2026: "it would be nice for server owners to have a built in
dashboard to monitor things". He pointed to a community admin's live stats page and
asked for something like it, but expanded.

## His answers (20 Nov 2026)

1. **Where:** a Dashboard tab in the manager, plus a public stats page the owner can turn on.
2. **What:** live server status, activity and history, a player leaderboard, machine health, "and anything you think
   would be cool to also have".
3. **History:** 90 days, like the privacy page's logs.
4. **The public page:** player names shown, never player ids or addresses.

## What it shows

**Cards:** players now (out of the slots of the servers up), the 24-hour peak, player-hours and sessions in the last
24 hours, players tracked over 90 days, the MVP of the day (most playtime today), the top gun over 7 days (most
kills), this machine's CPU and memory, and how many play on the whole Reborn network now.

**Each server, live:**
- the map and the next one, players out of its size, the average ping, uptime;
- the tick rate it actually held (the dedicated server's own "BF server ticks" line), against the rate it should run;
- the round, with the share of tickets the losing side has left (`match.txt`);
- the players on it, each with their ping and how long they have been on;
- in the manager only: the game process's and the host's CPU and memory.

**History:**
- players over 6 hours, 24 hours and 7 days;
- maps played over 7 days, in player-minutes;
- the busiest hours over 30 days, a weekday by hour grid (UTC);
- in the manager only: this machine's CPU over 24 hours.

**Veterans:** the top 25 by playtime over 90 days, with sessions, kills, deaths, K/D, favourite map and when last
seen. The kills and deaths come from the game's own counts (`server_stats.cpp`'s lines in each server's `stats.txt`);
bots are left out.

**Recent activity:** joins (and how long each player stayed when they left), map changes, servers starting and
stopping, and a server started again after its game or host ended.

**The Reborn network now:** the main backend's list of servers, with names, players, maps, regions and tick rates
(never addresses), the trusted and Reborn servers marked.

**Alerts** (manager only): a server down, a server players cannot reach, a server holding under 90% of its tick rate,
restarts in the last 24 hours, CPU over 90%, memory over 92%, under 2 GB of disk left, and a new release out.

## How it works

`server/dashboard.py` keeps everything. The manager calls `Dashboard.tick()` every two seconds from its watch loop,
with each server's state from its host's status file (`gmhost.py`'s roster and pings). Each tick:

- Every player on a server gets the seconds since the last tick: on that server, on that map, and in total. A name not
  there on the last tick is a join; one gone is a leave.
- Map changes and server state changes are written down. The manager tells it of a restart after a crash.
- New lines in each server's `stats.txt` add to its players' counts. Lines already there when the manager starts are
  not counted again.
- Every 30 seconds it reads each server's last tick-rate line from its log.
- Every 60 seconds it takes a sample: the players on each server, the machine's CPU and memory, and each server
  process's CPU and memory. Process figures come from the standard library only: Windows `GetProcessTimes` and
  `K32GetProcessMemoryInfo`, Linux `/proc`.
- Every 60 seconds it saves.

**Files:** `dashboard/day-YYYY-MM-DD.json` in the manager's data folder, one per day (UTC). Each holds the players
(seconds, sessions, maps, servers, counts), the samples and up to 400 events. Files older than 90 days are deleted at
the start and when a new day begins. **Names only:** no player id or address is ever written there (moderation keeps
those, for its own purposes and times, docs/MODERATION.md).

The manager asks the main backend for its list once a minute (`{"op": "list"}`), in a thread of its own. In LAN only
mode it does not ask.

## The public stats page

Off until the owner turns it on, on the Dashboard tab: "Serve the public stats page", a port (TCP 18290 by default),
a title, and whether player names are shown.

- It has its own small server (`PublicHandler` in `manager.py`) on its own port. It answers only `/` (the page,
  `manager_ui/public.html`), `/dash.js` (its drawing) and `/stats.json` (the numbers). Everything else gets 404, so
  nothing of the manager, its settings or its other pages can be reached through it.
- `/stats.json` is the dashboard's public view (`Dashboard.view(public=True)`): no processes, no alerts, no restart
  reasons, and no names at all when the owner turns names off. It is made at most once every 5 seconds, whoever asks.
- At most 120 requests a minute from one address (429 after that), a strict content security policy, no caching.
- UPnP opens its port with the servers' ports, and `open_firewall.ps1` has a rule for TCP 18290.

The page draws with the same `manager_ui/dash.js` as the manager's tab, in the launcher's colours. It needs nothing
from the internet and refreshes every 15 seconds.

## Tests

`python -I tools/test_dashboard.py`: 32 checks. A week of three servers played by made-up players goes through the
real `Dashboard`: playtime, sessions, kills and deaths (never bots, never lines from before the start), favourite
maps, cards, series, the busiest hours, restarts, the tick rate held, one file a day, 90 days kept, the history read
back, no ids or addresses in the files, and the public and names-off views. Then the real `Manager`, with its
servers' states stood in for: alerts, the public page on 127.0.0.8 (its three paths, 404 for everything else, the
policy, the rate limit, names off).

`python -I tools/test_dashboard.py --serve` keeps both pages up with live made-up players (the manager's on
127.0.0.1:18296, the public one on 127.0.0.8:18297), for looking at them. Screenshots of 20 Nov 2026 are in the
shared notes folder's `dashboard/`.

## Not done

- No run against real servers under a real manager. The test runs the real recorder and views with the servers'
  states stood in for, and the host's roster and pings were already proven in the moderation runs. A server started
  by a manager on this PC would listen on every address, and the Windows Firewall prompt it may raise would land on
  TekRantGaming's screen.
- Per-player figures from the game beyond its counts (accuracy, vehicles) are not there. The game does not count them
  for servers yet.
