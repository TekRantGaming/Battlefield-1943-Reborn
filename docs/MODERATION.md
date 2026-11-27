# Server moderation

Ban lists for community servers and for the Reborn network, with kicks, mutes, a player lookup and a log, in the
server manager. Asked for by TekRantGaming on 20 Nov 2026:

> Server moderation tools. We're going to need the ability to allow community servers to have their own independent
> ban lists, their own moderation tools, and of course, for our official servers, we're going to need our own separate
> ban list. [...] anyone that has been banned from a community server, is only banned from that community server. But
> if they're banned on the official networks, that means they're banned from joining any official server and community
> server, but can still join via direct IP as long as the direct IP doesn't belong to one of the community servers or
> an official server that's hooked up to our backend.

## The rules

| Who is banned | Official servers | Community servers on our backend | That one community server | Servers on their own backend (LAN only, Direct connect) |
|---|---|---|---|---|
| By a community server | can join | can join | **refused** | can join |
| On the Reborn network | **refused** | **refused** | **refused** | can join |

Direct connect to the address of a server registered with our backend is refused like any other join to it, because
that join goes through our backend too. A server on its own backend never asks ours, so a network ban does not reach
it.

## TekRantGaming's answers (20 Nov 2026)

1. **What a ban holds:** the player id, the account (Protect your name) and the IP address. A join that matches any of
   the three is refused.
2. **Length:** 1 hour, 1 day, 7 days, 30 days or permanent. The reason is typed by the admin and shown to the player,
   with when the ban ends.
3. **Where admins moderate:** the server manager's web page and its command line. The Reborn network's list is kept by
   the official manager on the machine that runs the main backend.
4. **The official list and community admins:** it is enforced on their servers, but they never see it. Their log only
   says "refused: banned on the Reborn network".
5. **What a refused player sees:** a pop-up, "BANNED FROM THIS SERVER" or "BANNED FROM THE REBORN NETWORK", with the
   reason and the end. A network ban says "Appeal on the Reborn Discord". A server ban says to contact that server's
   admin.
6. **Chat:** a separate Mute (timed or permanent, per server). The player keeps playing, but their voice and text
   chat reach nobody.
7. **Kick:** the player is removed with "You were kicked: <reason>" and that server refuses them for 5 minutes.
8. **Logging:** a moderation log in each manager, kept 90 days, with no addresses in it. A ban keeps an address only
   while it lasts. A line for the privacy page is drafted below and not published.

## Who a player is

The game sends three things that could name a player, and none of them is proof:

- **Player id:** 8 hex digits, made at the game's first start and saved in `bf1943-settings.txt` (the low word of the
  XUID the game logs in with). It survives restarts and updates. Deleting the settings file makes a new one.
- **Account:** the Ed25519 public key of a player who used Protect your name. It is only known when the player signed
  in for their name from that address (the backend's `VERIFIED` table). A new account is free to make.
- **Address:** the IP the backend and the host see. Homes share one, and it can change.

So a ban stores all three, and any one matching refuses the join (answer 1). An address is matched for **30 days at
most**, whatever the ban's length, then it is dropped from the ban: an address moves on to someone else in time. The
admin can leave the address out ("Also hold their address" off) for an address many players share, such as a LAN
party. Names are kept for the admins to read but never matched, since anyone can take a name.

## The lists and where they live

| List | File | Written by | Read by |
|---|---|---|---|
| A manager's own bans, kicks and mutes | `bans.json` in the manager's data folder | that manager (page, command line) | its hosts (`gmhost.py --bans-file`), and through them the backend |
| The Reborn network's bans | `network_bans.json` in the official manager's data folder | the official manager on the main backend's machine | the main backend (`plasma.py --network-bans`) |
| The moderation log | `moderation_log.jsonl` in the data folder | the manager, and its hosts' refusals and removals | the manager's page |
| Players seen this week | `players_seen.json` in the data folder | the manager | the page's lookup |

All four use one format, `server/bans.py`. A list has one writer and is written whole, to a `.tmp` file and renamed.
Every reader reads it again when it changes (at most once a second). An entry that has ended is dropped at the next
save.

A manager's list holds on all of its servers unless an entry names some of them ("On: #2"). Each community package
has its own manager, data folder and list, so one community admin's list never reaches another's servers.

## Where a join is checked

1. **`reserve`** (every join from our menus: the server browser, Quick Play, Favourites, Direct connect). The backend
   checks the network list and the server's own list (below) and answers
   `{"op": "banned", "gid", "network", "title", "message"}`. Any game since v0.4.0 shows a refusal with a message in
   its notice pop-up. v0.4.8 gives it its own heading, "MODERATION · COULD NOT JOIN", with no maintenance footnote.
   The text never holds the word "reserved", which a game from before the version gate reads as a yes
   (`bans.reason_problem`). `reserve` knows a player's id and account only once the game has signed in, so a join
   picked before that (from Home, the first after a start) asks again when it has (`game_menu.cpp ReserveSignedIn`).
   It checks the one player asking, never everyone who signed in from that address (a household, a LAN party): the
   game says its player id, matched with that id's own sign in from the same address (`plasma.py asker`). A game that
   says none (from before this) is checked by its address here, and as signed in at `EGAM`. A refusal
   gives up that player's own reservation and place in a queue, nobody else's: those go by address and player id
   (`plasma.py` RESERVATIONS), with a limit to what one address can hold (ADDRESS_PICKS, ADDRESS_QUEUED).
2. **The join queue:** a queued player whose turn comes is checked again.
3. **Theater's `EGAM`**, the step that gives a game the server's address. A game that skipped `reserve` (the game's
   own Quick Match, or a modified game) gets no address, so it cannot reach the server.
4. **The host**, at the player's hello (its ticket), against its own list: a banned player is not let in.
5. **While playing:** every 5 seconds, and at once when the list changes, the host checks its players. A banned,
   kicked or network-banned player is told why and taken off (below).

A refusal at the backend is also told to that server's host (`refused_join`), which writes it into its moderation
log. For a network ban, the line only says it was the network's.

### The server's own list at the backend

At registration, and whenever its list changes, a host sends the backend a copy of its bans and kicks:
`{"op": "server_bans", "salt", "bans": [{"ids", "accounts", "ips", "until", "ip_until", "kind", "title", "message"}]}`.
Every id, account and address is salted and hashed (`bans.hashed`), so the backend never holds a community server's
list of banned addresses in the clear. The copy is kept on that registration (its GID) only, at most 2,000 entries,
and the backend checks its text the way it checks a notice. It is dropped when the host's connection ends.

### A player removed during a match

The host sends the player's comms socket `C_KICKED` (12): the title and the message. A v0.4.8 game leaves the match at
once and shows the pop-up once its menus are back (`comms::TakeRemoved`, `game_menu.cpp`). The same words also go in
the broadcast box, for an older game. Two seconds later the host drops the player's link and ignores that address for
two minutes. A kick is a 5-minute entry in the server's list, so the same path refuses the player's rejoin.

When a network ban is made, the main backend looks at who joined each registered server (`JOINS`, from `EGAM`) and
sends that server's host `{"op": "remove", "player_id", "title", "message"}`. Only the player id leaves the backend,
never an address.

### Mutes

A mute keeps the player in the match. Their voice and chat packets are not passed on (`GameHost.muted`), and once a
minute they are told why in the broadcast box.

## The tools

### The manager's Moderation page

- **Players on now:** every player on this manager's servers, with player id and time on, and Kick, Ban and Mute.
- **Find a player:** the players its servers had in the last week, by any part of a name or by player id. On the
  official manager there is also "Search the Reborn network": everyone online, and names seen on the network this
  week. Below it, a ban by a player id or an address typed in.
- **Bans, kicks and mutes on your servers**, each with Unban, Unmute or Let back.
- **Reborn network bans** (official manager on the main backend's machine only).
- **Moderation log**, newest first.

The ban dialog shows exactly what the player will see.

### The command line

```
manager.py players
manager.py kick <name or player id> [--reason TEXT] [--server N] [--by NAME]
manager.py ban <name, player id, or --ip ADDRESS> [--length 7d] [--reason TEXT] [--no-ip] [--server N] [--network]
manager.py mute <name or player id> [--length 1h] [--reason TEXT] [--server N]
manager.py unban <ban id> [--network]
manager.py bans [--network]
manager.py modlog
manager.py lookup [TEXT] [--network]
```

`ban <name>` takes the player's id, account and address from the servers now, or from the week's list (with
`--network`, from the main backend's). A player id typed in full works for anyone.

## Security

- **Every admin action is the manager's**, behind its existing checks: this machine's tools by their token, a browser
  on this machine, or one signed in with the page's password (anything else gets 401). Every change is a POST with the
  page's own header and origin check. Nothing moderates through the game, so a game, modified or not, has no say.
- **The network list has no remote endpoint at all.** It is a file on the main backend's machine, and only that
  machine's official manager writes it (`network_admin()`: `own_backend() == 'official'`). The public package holds
  the code, but code without that machine and its login can do nothing. The network lookup is a signed admin question
  (`gmhost.sign_admin`, made from `official.key`), checked by the backend like promote's list of hosts, with a replay
  check and at most 30 a minute.
- **A community server affects only its own players.** Its `server_bans` is taken only from its own registry
  connection, and kept and checked for its own GID. Kicks and removals act on its own host. The backend's "remove" goes
  only to the host of the server the player joined.
- **The client is never trusted:** the backend checks at `EGAM` as well as `reserve`, and the host passes game packets
  only for a client that showed a ticket from the backend (`Peer.to_server`).
- **Limits:** a list holds at most 5,000 entries, a host's copy at the backend 2,000, and a reason 120 characters of a
  safe set.

## Privacy

- The moderation log keeps who did what, to which name and player id, why and when, for 90 days. **No addresses.**
- A ban keeps an address only while it is matched: 30 days at most, or less when the ban is shorter.
- The week's list of players (`players_seen.json`, the lookup) keeps name, player id, address and server for 7 days.
- The main backend's lookup table is in memory only, for a week at most.

Draft line for the privacy page (not published; TekRantGaming to approve):

> **Moderation.** When a server bans a player, it keeps their player id, their name and, for at most 30 days, the IP
> address they played from, so the ban holds. The moderation log keeps what was done, to whom and why for 90 days,
> without addresses. A server's bans stay on that server. Bans of the Reborn network are kept on our main server only.

## Limits

- **A player id is per install.** A new install, or a deleted settings file, makes a new one, so the address and the
  account carry a ban across it. A determined player on a new address with a new install gets in. A hardware id was
  offered and turned down (answer 1).
- A community admin who runs a modified host can ignore their own list. That is their server. The network ban still
  holds at our backend's `reserve` and `EGAM` for every server registered with it.
- The game-hosted matches (Custom Match, `bf/src/gm_host.cpp`) get network bans through the backend's `EGAM` check.
  The manager's bans, kicks and mutes are for the server package's servers.
- A v0.4.7 game that is kicked sees the message box, then a lost connection. A v0.4.8 game leaves cleanly and shows
  the pop-up.

## Deploying (later, on TekRantGaming's word)

- The official set's data folder must carry `network_bans.json` and `moderation_log.jsonl` from deploy to deploy,
  like `blocked_servers.json` and the accounts. Without them, the network bans end.
- The main backend, the official servers' hosts and the community package all need this version: hosts for
  `server_bans`, `remove` and `refused_join`, and games for `C_KICKED`. An older host still works with the new
  backend: its players are checked against the network list at `reserve` and `EGAM`, and it ignores the new messages.

## Tests

- `python -I tools/test_moderation.py`: the list itself; the host in-process (a removal with `C_KICKED`, the address
  kept out, a mute, no packets without a ticket, the backend's "remove"); and a backend of its own on 127.0.0.5 with
  real hosts. The backend part checks network and server bans at `reserve` and `EGAM` on official and community
  servers, a LAN-only backend letting the network-banned player in, a kick's minutes, unbans, a network ban made while
  a player plays, the signed lookup, and the manager's commands.
- In game (bf-dev, three runs on 127.0.0.5): see "Test record" below.

## Test record (20 Nov 2026)

- `tools/test_moderation.py`: 76 checks, all pass. Also passed after the change: test_join_queue, test_registry,
  test_maintenance, test_backend_restart, test_network, test_accounts backend.
- In game: bf-dev, a dedicated server with gmhost.py, its own backend on 127.0.0.5, player id 5EAB0001 set in the
  test user folder, our launcher with Quick Play:
  - **modnet3** (network ban): reserve answered "banned". The pop-up read "BANNED FROM THE REBORN NETWORK", with the
    reason, the end and the Discord appeal.
  - **modsrv3** (the server's own ban): the pop-up read "BANNED FROM THIS SERVER". The backend logged "refused game
    201: ... in the server's ban list", and the host's moderation log got the refusal.
  - **modkick3** (a kick 15 s after joining): the host sent C_KICKED and the game logged "removed from the server ...
    leaving the match". The host saw "left the game" in the same second. Back on Home 10 s later, the pop-up read
    "KICKED FROM THIS SERVER ... You can join again in 5 minutes".
  - **modnet1 and modsrv1** (the game's own menus, which skip `reserve`): no pop-up, and the backend refused at
    `EGAM`, so the game stayed on "Joining game". **modkick1**: a game whose chat had not linked was dropped, and its
    own "Connection to game has been lost" showed.
- Manager page: run in the built-in browser with test data, both as a community manager and as the official one
  (ban dialog, network list, unban, log).

Screenshots and logs: the shared notes folder of session local_516784ab, `moderation/`.
