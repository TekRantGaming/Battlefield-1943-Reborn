# The official servers

The official servers are TekRantGaming's: the main backend everyone logs in to, and the servers in the Official list
of the game's server browser. They run on a hosted Windows Server from a private build of the same
[server package](SERVER.md) that everyone can download, with the same manager; what makes a server official is a key
that only the official machine holds, which is never in a download.

## What runs

- **The main backend**: login, the list of every server (official and community), stats. Players' games find it
  through one setting, `bf_online_host`, which the release packages carry.
- **Thirteen servers** in EU Central, 24 players each: eight with Wake Island, Guadalcanal and Iwo Jima in turn, one
  each for Wake Island, Guadalcanal and Iwo Jima around the clock, Air Superiority on Coral Sea, and an experimental
  60 Hz test server with all four maps in turn.

## Versions

The main backend lets in only players on the newest release: it reads this repository's latest release every few
minutes. An older game gets a pop-up with **Update now** (0.4.0 and newer), or one line in its server list telling
the player to restart the game, which updates itself at the start (0.2.0 to 0.3.0).

The official servers are updated by hand, on the owner's word, when a release changes what a server runs.

## Ports

TCP 18241, 18246 and 18250 (the main backend), and one UDP port for each server, 18300 and on.
