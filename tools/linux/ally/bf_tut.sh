#!/usr/bin/env bash
# On the Ally: a developer AppImage run (private user and cache folders in the run) that signs in to the local
# backend, opens Help, starts the game's tutorial, and waits; extra environment through BFX_<VAR>.
#   bf_tut.sh <run> <appimage in bin/> [wait s after the tutorial starts]
set -u
RUN=$1 APP=$2 WAIT=${3:-25}
T=$HOME/bf-linux-test
R=$T/runs/$RUN
$T/bfrun.sh stop >/dev/null 2>&1
mkdir -p "$R"
[ -e "$R/user" ] || cp -a "$T/runs/t2/user" "$R/user"
GAME=$T/game-files $T/bfrun.sh start "$RUN" "$APP" --user_data_root="$R/user" --cache_root="$R/user/cache" \
  --log_file="$R/user/bf1943.log" --native_x_renderer=true --bf_engine_d3d_path=true --bf_online_host=127.0.0.1
sleep 22
$T/bfpad.sh "key ENTER"
for i in $(seq 1 60); do grep -q "BF menu driver: stage 2" "$R/user/bf1943.log" 2>/dev/null && break; sleep 1; done
sleep 3
$T/bfpad.sh "key DOWN; wait 0.3; key DOWN; wait 0.3; key DOWN; wait 0.3; key DOWN; wait 0.3; key DOWN; wait 0.3; key DOWN; wait 0.3; key DOWN; wait 0.3; key DOWN; wait 0.5; key ENTER"
sleep 4
$T/bfpad.sh "key ENTER"
sleep "$WAIT"
$T/bfrun.sh shot "$RUN" tut
grep -E "BF menu driver|connection ended|BF help" "$R/user/bf1943.log" | tail -8
