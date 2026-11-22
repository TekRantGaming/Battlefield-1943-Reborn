# Hosting a server

The server download runs Battlefield 1943 dedicated servers on a Windows PC, with no window and no graphics card
needed once they are set up. Your servers show in every player's in game server browser under Community, each with
its name, its map, its players, its region and its tick rate.

- **The server** (`bin\bf1943.exe --server`): the game itself running as a dedicated server, playing your maps in
  turn. When a round ends, the next map loads after the end of round screen, and the players stay.
- **Its host** (`server\gmhost.py`): links the players to the server, keeps it listed and passes on voice and text
  chat.
- **The server manager** (`server\manager.py`): a page in your browser. It installs the game, runs any number of
  servers and starts one again if it stops, sends messages to the players, and installs new releases.
- **The main backend** (`server\plasma.py`, the Project-Official Servers only): where every player logs in and finds the
  servers. Everyone else's servers register with the official one.

## What you need

- Windows 10, 11 or Windows Server, 64 bit. Nothing else to install: the download brings its own Python.
- Your own copy of Battlefield 1943 for Xbox 360: its package file (the single file in `5841097E\000D0000` on your
  console's storage). It is installed once.

## Set it up

1. Unzip `BF1943-Reborn-Server-<version>-windows.zip` anywhere you can write to.
2. Open **BF1943 Server.bat**: the server manager opens in your browser (`http://127.0.0.1:18280/`).
3. **Game files**: install the game from your package. It is unpacked into `game`, once.
4. **Servers**: each has its own name, description, maps (played in that order), players and port. **Start all**.
5. **Network**: pick your region; players see it with its flag. Leave **Open the ports** on behind a home router.
   **LAN only** keeps your servers off the main list: players then join with Direct connect, by this PC's address.
6. **Messages**: tell everyone on a server about an update, a restart or maintenance, with a countdown, or type your
   own. Your players also see the rare message the Reborn team sends to every server (from v0.4.7 on).

The Reborn team can also show a community server on the **Reborn** tab, marked TRUSTED COMMUNITY SERVER
([how it works](NETWORK-BROADCASTS-AND-PROMOTED-SERVERS.md)); there is nothing to do for it on your side.

The manager keeps running in the background, with no window, and starts a server again if it stops. **Stop
servers.bat** ends everything. **Start servers (no window).bat** starts them with the saved settings, for a PC that
starts its servers by itself. The page answers only on the PC itself, unless you set a password for it and allow
other machines. On the PC itself it opens as it is. Through an SSH tunnel with no password set, sign in with the code
that `manager.py code` (Linux: `bf1943-server code`) prints on the server.

## Time of day and weather

Each server keeps its own clock and weather, and every player on it sees the same. In a server's settings:

- **Day and night**: the light, the shadows and the sky follow the server's clock. Off, it is the game's own afternoon.
- **Time of day at the start**, and **A day lasts** (minutes for a whole day and night; 0 keeps the time still).
- **Cloud cover**, **Cloud change rate** (how fast clouds form and dissolve) and **Weather** (0 fair to 100 a storm:
  more and denser cloud; from about half, mist and far rain shafts; a little higher, rain; from about two thirds,
  lightning in the clouds).
- **Distance fog** (a haze that hides what is far; at 100 the sight is about a kilometre) and **Ground fog** (a low
  layer over the sea and the shore). Both show with either sky.
- **Wind** (0 calm to 100 a gale): how far the rain leans.
- **Cloud height** (100 to 1500 metres over the sea): where the clouds begin. Low, the planes fly through them and
  can hide in them.
- **Admin player IDs**: players who may change all of the above while the server runs, with **F9** in the game. A
  player who presses F9 in a match without the right is shown their ID to send you.

A match hosted from the game (Custom match) has the same settings under **Weather and time**, and its host is its admin.

## Custom match: game mode, maps, tickets, match time and the storm

A match hosted from the game (**Custom match**) also has:

- **Game mode**: Conquest (Wake Island, Guadalcanal, Iwo Jima), Air Superiority (Coral Sea) or All modes. Each map
  has one mode in the game, so the mode chooses the maps you can pick.
- **Map rotation**: a list of its own. Switch it on and pick the maps one by one, or **Rotate all maps** (every map of
  the mode). The match then plays them in turn, the next one after each round. Off, it plays the one map.
- **Tickets**: each team's tickets at a round's start (the map's own is 130, Coral Sea 100).
- **Match time**: how long a round lasts. When it is up, the team with fewer tickets loses; with both level, the next
  ticket lost decides. Players see the time left over the ticket bars. Off, a round ends only when a team has no
  tickets left.
- **Dynamic storm** (under Weather and time): the weather builds from its setting to a full storm over each round and
  eases again (over the match time, or half an hour without one); the wind goes with it unless set apart. The admin
  panel (**F9**) shows the weather as it moves and can switch the storm.

A dedicated server takes the same as `--bf_match_tickets=<n>`, `--bf_match_time_limit=<minutes>` and
`--bf_world_dynamic_storm=true` on its command line; the server package's own host (`gmhost.py`) does not pass them
yet.

## Dashboard

The manager's Dashboard tab shows your servers live and over the last 90 days: who is playing and their ping, the map,
the round's tickets, the tick rate each server really holds, players over 6 hours, a day and a week, the busiest
hours, the maps played, a playtime leaderboard with kills and deaths, recent joins and leaves, this machine's CPU and
memory, and alerts when something needs you.

The **Community page** tab makes a website of it for your players, on its own port (TCP 18290 by default): your
server's name, description, rules, logo, theme and accent colour, links to the game's download, the Reborn Discord and
your own Discord or website, how to join (each server's address with a Copy button, or your own instructions), and the
numbers you choose to show. It shows player names if you want, never player ids or addresses. Want it your own way?
Add your own CSS to restyle it, or build your own site in the `community_site` folder (the tab makes a starter for you),
reading the live numbers from `/stats.json`. More in [DASHBOARD.md](DASHBOARD.md).

## Moderation

The manager's Moderation page lists the players on your servers, with Kick, Ban and Mute:

- **Kick** removes a player now. They can join again in 5 minutes.
- **Ban** keeps a player out, for 1 hour, 1 day, 7 days, 30 days or for good. It holds their player id, their account
  if they have one and, for up to 30 days, their address.
- **Mute** keeps a player in the match, but their voice and text chat reach nobody.

Your reason is shown to the player. Your bans hold on your servers only, all of them or the one you pick. Find a
player lets you ban someone who has already left. The page keeps a moderation log for 90 days. From the command line:
`manager.py players`, `kick`, `ban`, `mute`, `unban`, `bans`, `modlog` and `lookup`. Players banned on the Reborn
network are refused by the main backend before they reach your servers; your log shows that, and nothing more. More in
[MODERATION.md](MODERATION.md).

## Updates

Players' games are always on the newest release (the main backend lets no older one in), so keep your servers on it
too. The manager looks for a new release by itself, tells the players on your servers with a countdown, installs it
and starts the servers again. Your `server.json`, your game files and your logs stay.

## Ports

| Port | For | Who needs it open |
|---|---|---|
| UDP 18300, 18301 and on (one for each server) | the servers (players join here) | every host |
| TCP 18241, 18246, 18250 | the main backend (login, games, server list) | the Project-Official Servers only |

Behind a home router the manager asks the router to open the servers' ports itself (UPnP). If the router does not
allow it, the page says which ports to open by hand. On a hosted server, run **Open firewall ports.bat** once, and
open the same ports in your host's own firewall.
