# Benchmark runs

`bench.py` runs the game the same way every time and measures it, to compare builds and settings and to find stutters.
It needs a developer build (`BF_DEV_TOOLS`, the `bf-dev` preset or a release preset with `-DBF_DEV_TOOLS=ON`) and
Python 3; it works on Windows and Linux. For numbers close to a player's, use a release-configuration build: a
RelWithDebInfo build counts every guest call and carries Tracy, which costs frames.

A run starts a LAN dedicated server of its own on this PC (loopback only, not listed), then the game, which joins it
by itself (`BF_AUTO_JOIN`) with a copy of your settings (never written back). After the spawn it waits `--settle`
seconds (shaders, textures and the level settle), measures `--duration` seconds of frames (with `--freecam`, by
default one lap of its path from its take off: the `--freecam` delay is then the settling time), closes both and writes
`summary.txt` in the run folder (`runs/bench-<time>-<map>-<mode>`, git-ignored).

```
python tools/bench/bench.py run                                        # stand on the carrier and turn on the spot
python tools/bench/bench.py run --freecam 10,flags                     # one lap of the flags loop
python tools/bench/bench.py run --freecam 10,flags --explode 80        # every prop going off in view
python tools/bench/bench.py run --storm 1 --freecam 60,flags           # through a full storm
python tools/bench/bench.py run --preset all --freecam 10,flags        # every graphics preset
python tools/bench/bench.py compare runs/bench-*                       # one line per run
```

## What the view does

- Default: the soldier turns on the spot at a steady rate from 30 s after joining (`--turn`, `--turn-at`; `--turn 0`
  stands still), so the view sweeps the whole spawn point.
- `--freecam <s>[,<path>[,<value>]]`: from that many seconds after joining, the game's own free camera on a timed path
  (`bf/src/bench_freecam.cpp`), the same every run:
  - `orbit[,<s a lap>]`: a circle around the capture points, 60 m above the highest, looking down at the middle;
    the most of the level at once.
  - `flags[,<m/s>]`: a smooth loop 25 m above the capture points at a steady 30 m/s, looking ahead; close up, the
    view changing fast.
  - `props[,<m/s>]`: the same through the groups of explosive props (with `--explode`).

## Explosions

`--explode <metres>[,<half angle>]` makes the level's explosive props (oil drums, propane tanks, gas cylinders) blow
up as the free camera comes to them, in view: each one when it is in the camera's view cone (on the ground plane, 25
degrees to each side by default, of any height) and within that distance. A prop the path never brings that near while
in view goes off when it is as near as it will be in view: before it flies, the game goes through one lap of the path
and works out each prop's distance, so groups off the path are covered on any level and path without settings
(`bf/src/bench_freecam.cpp`). The server destroys them as a hit would, with the game's own explosion, fire and debris
(no attacker: like a prop a vehicle crashes into, nobody is credited; `bf/src/bench_explode.cpp`).

The server finds the props at the start of each level by a scan of its memory, on a thread of its own (3 to 11 s;
the server goes on meanwhile): 2 s into the first level, 10 s into each one after a level change. The runner starts
the game after the first (`BF explode: scan done` in `server.log`). Their places go to `props.txt` in the run folder,
and the props the camera asks for to `trigger.txt` (their lines in `props.txt`).

## Graphics presets

`--preset <name>` runs with one of the Graphics page's presets (Potato, Low, Medium, High, Ultra), applied to the
run's copy of the settings at the game's start (`BF_BENCH_PRESET`, `game_menu.cpp`) as choosing it on that page does.
A list (`--preset Low,High`) or `--preset all` makes a run each, one after the other, then prints their comparison:

```
python tools/bench/bench.py run --preset all --freecam 10,flags
```

## Other load

- `--bots <n>`: bots on the server (they walk to the flags and fight).
- `--storm <0..1>`: the match's weather; 1 is a full storm (rain, lightning, wind).
- `--arg` / `--server-arg`: any other option for the game or the server (repeatable).
- `--mode capped`: your frame limit and vsync instead of none (for stutters at a steady frame rate).

## The summary

- `fps`, `low1_fps`, `low01_fps`: frames a second, and the average of the slowest 1% and 0.1% of frames.
- `p50_ms`, `p99_ms`, `p999_ms`, `max_ms`, `over25` ... `over100`: frame times and how many frames were longer.
- `renderer_hitches`: the renderer's own lines for frames over `--hitch-ms` (default 40), with what each waited on,
  in `bf1943.log`.
- `gpu_page_events`: GPU memory pages DiligentCore made or freed in the window.
- `explosions`: props blown up in the window.
- Processor time: per thread on Linux; the whole process on Windows when `psutil` is installed.

The numbers repeat to within about 1% between runs of the same build (two runs each of the turning and the orbit
views on Wake Island, uncapped). Compare runs made the same way on the same PC.
