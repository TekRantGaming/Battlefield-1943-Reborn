#!/usr/bin/env bash
# On the Ally: start a test run, press Play, and once the local backend has sent the login reply, attach gdb to the
# game during its freeze and save every thread's stack (~/bf-linux-test/runs/<run>/freeze-bt.txt).
#   bf_freeze_bt.sh <run> <appimage in bin/>
set -u
RUN=$1 APP=$2
T=$HOME/bf-linux-test
$T/bfrun.sh stop >/dev/null 2>&1
LINES=$(wc -l < $T/plasma.log)
BFX_SDL_VIDEO_DRIVER=x11 USER_FROM=$T/runs/t2/user GAME=$T/game-files $T/bfrun.sh start "$RUN" "$APP" --bf_online_host=127.0.0.1
sleep 22
$T/bfpad.sh "key ENTER"
for i in $(seq 1 90); do
  tail -n +$((LINES + 1)) $T/plasma.log | grep -q "<- acct 80000000 id 2" && break
  sleep 1
done
echo "login reply seen after $i s"
sleep 6
PID=$(pgrep -x bf1943 | head -1)
echo "attaching to $PID"
timeout 120 gdb -p "$PID" -batch -ex "set pagination off" -ex "thread apply all bt 14" > "$T/runs/$RUN/freeze-bt.txt" 2>&1
echo "gdb done: $(wc -l < "$T/runs/$RUN/freeze-bt.txt") lines"
