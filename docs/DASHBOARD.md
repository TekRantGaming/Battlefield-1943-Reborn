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

## The community page (the public page)

TekRantGaming, 20 Nov 2026: "allowing community servers to have a external web page that they can theme with their
own branding if they choose. It shows all the statistics of the server that is customizable as well on what the host
wants to show ... It can link to our project so that they can download it. It can link to our Discord, and ... if the
server host has their own Discord or communities ... a customizable name and description, and it can show the server
IP to join externally or give instructions as well on how to connect to it from the server browser."

The owner sets it up on the manager's **Community page** tab, and **Preview** shows it as players will see it, even
while it is off:

- **The page:** on or off, and its port (TCP 18290 by default).
- **Name and words:** a name (default: the first server's), a description of up to 1,000 characters with its line
  breaks kept, and optional server rules of up to 1,500.
- **Look:** a theme (Field Report olive, Night navy, Desert sand, Steel grey, or Light), an optional accent colour
  for the header stripe, buttons and live badge, and a logo (PNG, JPEG, WebP or GIF, 1 MB at most). The logo is
  checked by its own bytes, kept as `public_logo.<type>` in the data folder, and served at `/logo`.
- **What it shows:** each part on or off: How to join, the headline numbers, each server live, players over time,
  maps, busiest hours, the veterans leaderboard, recent joins and leaves, the Reborn server list, the rules. Player
  names can also be turned off everywhere.
- **Links:** "Download Battlefield 1943 Reborn" (the project's latest release), the Battlefield 1943 Reborn Discord,
  and up to 6 of the owner's own (their Discord, website...). Only http and https links are taken, with no spaces and
  no user name in them.
- **How to join:** each server with its address and a Copy button for Direct connect. The address is the one the main
  backend saw the server register from, or the owner's own (`play.example.com`, with each server's port), or left off.
  Below it are instructions: the owner's own, or by default find the server in the Server Browser's Community tab, or
  Direct connect.

The page itself (`manager_ui/public.html`) shows the owner's header (logo, name, live count), About with the links,
How to join, the numbers they chose (drawn by `dash.js`, the same as the manager's tab), the rules, and a footer
linking the project.

### Their own code

TekRantGaming, 20 Nov 2026: "they don't have to use our kind of implementation, they can use their own custom ... they
can code it themselves and then it'll display". Two levels, both on the Community page tab under "Your own code":

- **Their CSS over our page:** up to 20,000 characters, put last on the page, so it wins. The theme's colours are CSS
  variables (`--bg`, `--bar`, `--card`, `--line`, `--cream`, `--accent`). The parts have classes: `header`, `.box`
  (About and How to join), `.dcard` (the numbers), `.dpanel` (the panels), `.dsv` (a server). It is put in as text
  in a style element, so CSS can change the look but never run code.
- **A website of their own:** "Use my own website instead of this page" serves the folder `community_site` in the
  manager's data folder, with its `index.html` as the front page. Pages, CSS, scripts, pictures, fonts and video can
  sit in subfolders. "Make a starter in the folder" puts our page there as `index.html`, to change, and a
  `README.txt` listing every field of `/stats.json`. Their code reads the live numbers from `/stats.json`. `/dash.js`
  (our charts: `Dash.render(element, stats, {public: true})`) and `/logo` stay at those addresses whatever the
  folder holds. Only files inside the folder are served, of known types, 20 MB at most, never a name starting with a
  dot. `..`, encoded slashes and drive letters get 404. Their pages run under a looser content policy (fonts,
  pictures and scripts from https sites allowed), because it is their site, and anything outside the folder is
  still out of reach.

### Safety of the page

- It has its own small server (`PublicHandler` in `manager.py`) on its own port. It answers only `/`, `/dash.js`,
  `/stats.json` and `/logo`, or with their own site the files of its folder. Everything else gets 404, so nothing of
  the manager can be reached through it.
- Everything the owner or a player wrote is put on the page as text (`textContent`), never as markup: a description
  holding `<img onerror=...>` shows those characters and runs nothing (checked in the browser). Links open with
  `rel="noopener nofollow"`.
- `/stats.json` is the dashboard's public view (`Dashboard.view(public=True)`): no processes, no alerts, no restart
  reasons, never player ids or players' addresses, and no names when the owner turns them off. It is made at most
  once every 5 seconds.
- At most 120 requests a minute from one address, a strict content security policy (scripts and pictures only from
  the page itself) and no caching. UPnP opens the port with the servers' ports, and `open_firewall.ps1` has a rule
  for TCP 18290.
- The preview on the manager (`/preview/`) needs the manager's sign-in, like the rest of its page.

## The charts

Blue lines on the cream panels (TekRantGaming, 20 Nov 2026: green was hard to see against the green page), with
whole numbers on their axes.

## Tests

`python -I tools/test_dashboard.py`: 65 checks. A week of three servers played by made-up players goes through the
real `Dashboard`: playtime, sessions, kills and deaths (never bots, never lines from before the start), favourite
maps, cards, series, the busiest hours, restarts, the tick rate held, one file a day, 90 days kept, the history read
back, no ids or addresses in the files, and the public and names-off views. Then the real `Manager`, with its
servers' states stood in for: alerts, the public page on 127.0.0.8 (its three paths, 404 for everything else, the
policy, the rate limit, names off). Then the community page: links refused unless http or https, colours,
themes, text, a logo checked by its bytes (not a picture, over 1 MB), the logo kept when the page is saved and served
as a PNG, the page's words, links, addresses and sections in its numbers, the address left off, the logo removed,
and their own code: their CSS in the numbers, the starter (made once, then kept), their own site served in place of
ours with /stats.json, /dash.js and /logo still there, and ten tries at files outside the folder (`..`, encoded,
hidden, `.py`, a drive) all 404.

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
