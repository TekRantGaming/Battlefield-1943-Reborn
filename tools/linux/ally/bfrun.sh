#!/usr/bin/env bash
# Ally test runs of the Battlefield 1943 Reborn AppImage (Claude, Linux port; King Kong's kkrun.sh). Everything lives
# in ~/bf-linux-test; TekRantGaming's own folders are only read (GAME links a game folder in, never written).
#   bfrun.sh start <name> <appimage in bin/> [game args...]
#        env: GAME=<installed game folder> linked in as game/; USER_FROM=<folder> copied in as user/ (a finished
#        setup); BFX_<VAR>=<value> passes <VAR> to the game
#   bfrun.sh stop [name] | status | log <name> [lines] | shot <name> <label>
#   bfrun.sh clean   before ~/bf-linux-test is removed: Spectacle puts every shot in KDE's recent files and activity
#        history and remembers its last save path; this takes the test's entries out of all three
# Bazzite ends a user's processes when their login session closes (KillUserProcesses), so anything started over ssh
# outside a systemd unit dies with the connection: Steam as at login is
# `systemctl --user start app-steam@autostart.service` (hidden, -silent). No xdotool or ydotool: KWin asks the person at
# the Ally to allow input control (kwin_eis_prompter); end that process, never allow (no answer stores nothing).
# `steam steam://nav/games/details/<shortcut appid>` opens a shortcut's library page (xdg-open's did not reach it).
set -u
T=$HOME/bf-linux-test
cmd=${1:-status}; shift || true
# The desktop session's own environment (plasmashell's), so the game's window lands on the desktop.
desktop_env() {
  local pid; pid=$(pgrep -u "$(id -u)" -x plasmashell | head -1)
  [ -n "$pid" ] || { echo "no plasmashell: is the Ally in desktop mode?" >&2; return 1; }
  tr '\0' '\n' < /proc/$pid/environ | grep -E '^(WAYLAND_DISPLAY|DISPLAY|XDG_RUNTIME_DIR|XDG_SESSION_TYPE|XDG_CURRENT_DESKTOP|DBUS_SESSION_BUS_ADDRESS|XAUTHORITY|XDG_DATA_DIRS|PULSE_SERVER)='
}
case $cmd in
start)
  NAME=$1 APP=$2; shift 2
  RUN=$T/runs/$NAME
  mkdir -p "$RUN/shots"
  cp -f "$T/bin/$APP" "$RUN/BF1943-Reborn-linux-x86_64.AppImage" && chmod +x "$RUN/BF1943-Reborn-linux-x86_64.AppImage"
  [ -z "${GAME:-}" ] || [ -e "$RUN/game" ] || ln -s "$GAME" "$RUN/game"
  [ -z "${USER_FROM:-}" ] || [ -e "$RUN/user" ] || cp -a "$USER_FROM" "$RUN/user"
  ENVS=()
  while IFS= read -r kv; do ENVS+=(--setenv="$kv"); done < <(desktop_env) || exit 1
  for v in $(env | grep -o '^BFX_[A-Z0-9_]*'); do ENVS+=(--setenv="${v#BFX_}=${!v}"); done
  systemctl --user reset-failed "bftest-$NAME" 2>/dev/null
  systemd-run --user --quiet --unit="bftest-$NAME" --working-directory="$RUN" "${ENVS[@]}" \
    -p ExitType=cgroup -p StandardOutput=append:"$RUN/stdout.txt" -p StandardError=append:"$RUN/stdout.txt" \
    "$RUN/BF1943-Reborn-linux-x86_64.AppImage" "$@"
  echo "started bftest-$NAME in $RUN"
  ;;
stop)
  for u in $(systemctl --user list-units --plain --no-legend 'bftest-*' | awk '{print $1}'); do
    [ -z "${1:-}" ] || [ "$u" = "bftest-$1.service" ] || continue
    # The game first (it quits by itself), so the AppImage's mount outlives it.
    cg=$(systemctl --user show -p ControlGroup --value "$u")
    for pid in $(cat /sys/fs/cgroup$cg/cgroup.procs 2>/dev/null); do
      [ "$(cat /proc/$pid/comm 2>/dev/null)" = bf1943 ] && kill -TERM "$pid"
    done
    left=0
    for i in $(seq 1 24); do
      left=0; for pid in $(cat /sys/fs/cgroup$cg/cgroup.procs 2>/dev/null); do [ "$(cat /proc/$pid/comm 2>/dev/null)" = bf1943 ] && left=1; done
      [ $left = 0 ] && break; sleep 0.5
    done
    [ $left = 0 ] && echo "the game ended by itself" || echo "the game was still there after 12 s"
    systemctl --user stop "$u"; echo "stopped $u"
  done
  ;;
status)
  systemctl --user list-units --plain --no-legend 'bftest-*'
  pgrep -a -x bf1943 | cut -c1-160 || echo "no bf1943 process"
  ;;
log)
  tail -n "${2:-40}" "$T/runs/$1/user/bf1943.log"
  ;;
shot)
  # Desktop mode: the whole screen (spectacle), as kk_watch.sh.
  ENVS=$(desktop_env | sed 's/^/export /'); eval "$ENVS"
  spectacle -b -n -f -o "$T/runs/$1/shots/$2.png" >/dev/null 2>&1 && ls -la "$T/runs/$1/shots/$2.png"
  ;;
clean)
  # KDE keeps the path as given (/home/...) in recent files and the real one (/var/home/... on Bazzite) in the history.
  ENVS=$(desktop_env | sed 's/^/export /'); eval "$ENVS"
  R=$(realpath "$T"); Q=$(command -v qdbus6 || command -v qdbus)
  for d in "$T" "$R"; do
    "$Q" org.kde.ActivityManager /ActivityManager/Resources/Scoring \
      org.kde.ActivityManager.ResourcesScoring.DeleteStatsForResource ":any" "$(command -v spectacle)" "$d/*" >/dev/null
  done && echo "activity history: the test's shots forgotten"
  python3 -I - "$T" "$R" <<'EOF'
import os, re, sys, xml.etree.ElementTree as ET
p = os.path.expanduser("~/.local/share/recently-used.xbel")
if not os.path.exists(p):
    sys.exit(0)
s = open(p, encoding="utf-8").read()
dirs = ["file://" + d.rstrip("/") + "/" for d in sys.argv[1:]]
pat = re.compile(r'[ \t]*<bookmark\b[^>]*\bhref="(?:' + "|".join(map(re.escape, dirs)) + r')[^"]*"[^>]*>.*?</bookmark>\n?', re.S)
new, n = pat.subn("", s)
hrefs = lambda t: [b.get("href") for b in ET.fromstring(t.encode("utf-8")).findall("bookmark")]
old_h, new_h = hrefs(s), hrefs(new)
mine = [h for h in old_h if h.startswith(tuple(dirs))]
# Written only when exactly the test's entries went, every other one kept in order.
if n and n == len(mine) and new_h == [h for h in old_h if h not in mine]:
    tmp = p + ".bfclean"
    open(tmp, "w", encoding="utf-8").write(new)
    os.chmod(tmp, 0o600)
    os.replace(tmp, p)
    print("recent files: removed", n, "of", len(old_h))
else:
    print("recent files: none removed (" + str(len(mine)) + " of the test's found)")
EOF
  sed -i "\#^lastImageSaveLocation=file://\($T\|$R\)/#d" "$HOME/.config/spectaclerc" 2>/dev/null
  ;;
*) echo "usage: bfrun.sh start|stop|status|log|shot|clean ..." >&2; exit 2 ;;
esac
