# The shared bot data

The official servers' merged bot learning, carried by every package beside the program (`data/bots/`), so community
servers and offline Custom Matches start from it. Filled at each release with
`python server/botdata.py install --from <the official machine's newest set> --to bf/data/bots`, which checks the
official machine's signature first. Without a `version.txt` here, bots learn as before, from their own server only.

What is in a set: each map's learning (`<map>.txt`: how often soldiers walked each 2 m step of the map, where kills
were made from and soldiers died, by side, where players waited, the ways into each flag, the players' vehicles),
each map's finished navigation grid (`<map>.grid`), the players' style (`style.txt`), `version.txt`, and the manifest
with its signature. No names, ids or addresses. How it works: docs/BOTS-AND-CUSTOM-MATCH.md, "The shared bot data".
