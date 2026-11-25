# Match recording, live feed and theater

24 Nov 2026 · ZALIAS (ThatZaliaSGuy) · design proposal, nothing built yet

> **For TekRantGaming, and for Claude when merging this PR:** please read this whole document before merging. It is a
> design proposal, not code: it describes a match recorder, a 30-second-delayed live feed (like ECHO TV for
> Battlefield 1942) and a Halo 3 / Reach style theater for Battlefield 1943 Reborn. Nothing in this PR implements it.
> Parts of it need TekRantGaming's decisions before anyone builds them: who hosts the live relay (it would sit on
> TekRantGaming's backend, next to the server list and stats), what a public web viewer may draw (the game's art
> cannot be published), and the open questions at the end. Claude: summarise those questions for TekRantGaming when
> you merge, and do not start building any of it without his say.

One recorder on the match server feeds both a 30-second-delayed live viewer and a replay theater: build the recorder
first, then a viewer, then the live relay, and research a true in-game theater alongside.

## At a glance

```
Match server --stream--> Live relay --30 s later--> Web viewer
(recorder,               (holds 30 s,               (stylised 3D,
 20 snapshots/s)          serves viewers)            free cameras)
     |
     | saved match
     v
Recording file --------------play back--------------> Theater
(full snapshot every 5 s)                             (browser or in-game, later)
                                                          ^
Player's client --> Network replay  - - - - - - - - - - - +
(records server     (research: feed it back;   replayed in the game
 traffic)            dashed = not proven)
```

The server's recorder is the one source: the relay plays it live 30 seconds late, and the saved file plays in the
theater. The dashed path is the research route to a theater with the game's own look.

## What ECHO TV does

[ECHO TV](https://stats.echo-gaming.com/tv/) is not a video stream: it replays a recording of the match in 3D in the
browser, about 30 seconds behind the server (46 s when looked at, 24 Nov 2026). The page says "Loading the replay..."
while it starts.

- **Assets:** the game's own models (`.glb`), animation poses, textures and map pictures, converted for the web and
  served from their site.
- **Cameras:** Auto, Orbit, Player and Free, plus full screen.
- **Privacy:** the public feed leaves out in-game chat and private server fields.

So their live TV and their replays are one system: the server records, and the viewer plays the recording back with a
safety delay. That is the model to copy.

## The recorder

The recorder runs inside the match server, and optionally in a client, and writes the match as snapshots about 20 times
a second. Every server is our own recompiled game, and Reborn already reads most of this from it: the pause-menu map,
the bots, the vehicle views, the capture points and the shared weather.

| What | Contents | How often |
| --- | --- | --- |
| Players | ID, name, team, squad, kit, position, facing, pose, health, weapon, seat | 20 per second |
| Vehicles | Type, position and rotation, turret and gun parts, health, who sits where | 20 per second |
| Match | Flags (owner, capture progress), tickets, score, time left | On change |
| World | Time of day, weather, clouds, storm | On change |
| Events | Kills (who, whom, weapon, distance), shots, explosions, captures, spawns, chat (kept out of public feeds) | As they happen |

**File format:**

- A header with the map, mode, server, start time and the Reborn version that recorded it.
- A full snapshot every 5 seconds, with only the changes in between, so a viewer can seek and join late.
- Positions quantised to centimetres, angles to small integers, and the stream compressed: a 30-minute match should be
  a few megabytes.

## The live feed

A server that opts in streams its recording to a relay, which holds it 30 seconds and serves it to any number of
viewers.

- **Server side:** a setting in the server package turns the feed on. The server sends the recording in chunks of a
  second or two over one outgoing connection, so watching costs the server nothing per viewer.
- **Relay:** keeps a short buffer per server, serves each viewer from 30 seconds back, and keeps the whole match as a
  file for the theater afterwards. It is a small web service on TekRantGaming's infrastructure, next to the server list
  and stats backend.
- **Delay:** 30 seconds by default, so nobody can watch the feed to spot enemies while playing on the same server.
- **Kept out of the public feed:** chat, player IPs, server passwords, private match fields. A private match is never
  streamed unless its host turns the feed on.

## The viewer

The viewer is a browser page that plays a live feed or a saved recording, with Follow, Orbit, Overhead and Free
cameras, a scoreboard and a kill feed. The open question is what it draws.

| Option | Looks like the game | Risk | Work |
| --- | --- | --- | --- |
| The game's models and textures, converted and hosted on a website (as ECHO does) | Yes | High: publishing EA's art, against our rule of no game art in the repository | Large |
| Our own simple models and an outline map | No, stylised | None | Medium |
| A viewer inside the Reborn client, drawing with the player's own game files | Yes | Low: nothing is published | Large |

The second option suits a public web page. The third is the theater below, and also lets a player watch a live feed in
the game.

## The theater

Halo 3 and Halo: Reach record only the players' inputs and re-run the match, because their engine is deterministic.
Battlefield 1943 is a client and server game with physics that is not, so its inputs alone will not reproduce a match.
Two routes fit it instead.

| Route | How | Looks | Risk |
| --- | --- | --- | --- |
| Network replay | A client records the server's traffic; later a local stand-in server feeds it back to the game, and the player watches with our free camera | Exactly the game: its renderer, effects and sound | Unknown until researched: the game's connection (handshake, sequence numbers, acknowledgements) may refuse a replayed session |
| Snapshot playback | The viewer above plays the recorder's file, in the browser or in the client | The viewer's own look | Low: the recorder exists anyway |

A network replay is the true Halo-style theater, so it gets a research spike first. If it works, clients record their
own matches with no server help, and servers can record every match too.

## Build order

1. **Recorder:** snapshots and events written to a file by hosted matches, dedicated servers and, optionally, clients.
   Done when a 30-minute match gives a file of a few megabytes holding every player, vehicle, flag and kill.
2. **First viewer:** an overhead map that plays a saved file, with seeking, a scoreboard and a kill feed. Proves the
   format end to end.
3. **3D viewer:** the stylised web viewer, with Follow, Orbit and Free cameras.
4. **Live relay:** server streaming, the 30-second buffer and a channel page, built with TekRantGaming on their backend.
5. **Network replay research (alongside 1 to 4):** record a client's incoming traffic and try feeding it back to a
   local game. Its result decides how the in-game theater is built.
6. **In-game theater:** watch recordings and live feeds from the main menu, with the game's own look.

## Open questions

- [ ] Who hosts the relay and the channel page: TekRantGaming's backend, or a community server?
- [ ] What may a public web viewer draw: stylised models only, or nothing beyond an overhead map?
- [ ] Which servers stream by default: official ones only, or any server that opts in?
- [ ] Should clients record every match automatically, or only when the player asks?
- [ ] How long are recordings kept, and can players share them by link?
- [ ] Is 30 seconds the right delay for competitive matches?
