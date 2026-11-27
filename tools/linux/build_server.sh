#!/usr/bin/env bash
# The Battlefield 1943 Reborn SERVER for Linux from the WSL Linux build, after build_linux.sh and build_appimage.sh
# for the same version (the server's program files come from the game's AppDir):
#
#   BF1943-Reborn-Server-<version>-linux.tar.gz     the plain archive: unpack, run ./bf1943-server (needs python3);
#                                                   also what a running manager downloads to update itself
#   BF1943-Reborn-Server-<version>-x86_64.AppImage  one file with its own Python (TekRantGaming, 13 Nov 2026)
#
#   bash build_server.sh <version> [tar|appimage|all|stage] [output folder]
#
# Names: no file may END with "linux-x86_64.AppImage" but the game's (its updater takes the release file that ends
# so: bf/src/update.cpp; tools/check_package.ps1 refuses a second one).
set -euo pipefail
VERSION="${1:?usage: build_server.sh <version> [tar|appimage|all|stage] [output folder]}"
WHAT="${2:-all}"
BASE="${BF_LINUX_ROOT:-$HOME/bf-linux}"
OUT_DIR="${3:-$BASE/dist}"
WT="${BF_LINUX_SOURCE:-${BF_LINUX_MAIN:-/mnt/f/BF-1942-MP-Decomp}}"
APPDIR="${BF_APPDIR:-$BASE/AppDir-linux-amd64-release}"  # build_appimage.sh made it
TOOL="${BF_APPIMAGETOOL:-/root/kk-linux-beta/work/appimagetool-1.9.1-x86_64.AppImage}"
RT="${BF_APPIMAGE_RUNTIME:-$BASE/runtime-x86_64}"
ONLINE_HOST="${BF_ONLINE_HOST:?set BF_ONLINE_HOST to the main backend address (the official one, or 127.0.0.1 for a local test)}"
NAME="BF1943-Reborn-Server-$VERSION-linux"
STAGE="${BF_SERVER_STAGE:-$BASE/server-stage}"
T="$STAGE/$NAME"

stage() {
  echo "== the server tree ($T)"
  local S="$APPDIR/usr/bin"
  [ -x "$S/bf1943" ] || { echo "no built program at $S (build_linux.sh and build_appimage.sh first)" >&2; exit 1; }
  rm -rf "$STAGE"; mkdir -p "$T/bin" "$T/server/manager_ui" "$T/lib/desktop"
  # The program, its runtime and the plugin (its null GPU on a server), the fonts and the licences. No shader list
  # and no button pictures: a server draws nothing.
  cp -a "$S/bf1943" "$S"/*.so* "$T/bin/"
  if [ -d "$S/fonts" ]; then cp -a "$S/fonts" "$T/bin/"; fi
  # The shared bot data (bf/data/bots, server/botdata.py install): what every server's bots start from.
  if [ -f "$S/data/bots/version.txt" ]; then mkdir -p "$T/bin/data/bots" && cp -a "$S/data/bots/." "$T/bin/data/bots/"; rm -f "$T/bin/data/bots/README.md"
  else echo "warning: no shared bot data in $S/data/bots: the package carries none"; fi
  if [ -d "$S/licences" ]; then cp -a "$S/licences" "$T/licences"; else echo "warning: no licences folder in $S"; fi
  # Every module plasma.py imports (16 Nov 2026: player_stats.py was missing, so the package's own backend, LAN only
  # mode and the manager's commands that load plasma.py, stopped at the start); as tools/make_packages.ps1 copies.
  cp "$WT/server/manager.py" "$WT/server/gmhost.py" "$WT/server/plasma.py" "$WT/server/ed25519.py" "$WT/server/stats.py" \
    "$WT/server/player_stats.py" "$WT/server/text_filter.py" "$WT/server/bans.py" "$WT/server/dashboard.py" \
    "$WT/server/botdata.py" "$T/server/"
  cp "$WT/server/manager_ui/index.html" "$WT/server/manager_ui/dash.js" "$WT/server/manager_ui/public.html" "$T/server/manager_ui/"
  cp "$WT/tools/linux/server/bf1943-server" "$T/bf1943-server"
  chmod +x "$T/bf1943-server" "$T/bin/bf1943"
  echo "$VERSION" > "$T/version.txt"
  # package.json: the game has Night Ops and Daybreak (server/manager.py package_flags: the hosts' --night and
  # --daybreak, and the Night Ops box), as tools/make_packages.ps1 writes for Windows. 0.4.8's Linux package had none,
  # so its servers ran without them (TekRantGaming, 27 Nov 2026, from community server owners). A build without Night
  # Ops is refused, as the Windows packager does.
  local night
  night="$(grep -h '^BF_NIGHT_OPS:BOOL=' "$BASE"/src/bf/out/build/linux-amd64-*/CMakeCache.txt 2>/dev/null | head -1 | cut -d= -f2)"
  case "$night" in
    OFF|FALSE|0) echo "the Linux build has no Night Ops (BF_NIGHT_OPS $night): players' games would draw Night Ops and Daybreak servers by day; build it with BF_NIGHT_OPS=ON" >&2; exit 1 ;;
    "") echo "warning: no CMake cache under $BASE/src/bf/out/build to check BF_NIGHT_OPS (build_linux.sh's default is ON)" ;;
  esac
  printf '{"night_ops":true}' > "$T/package.json"
  printf '{"version": 3, "upnp": true, "backend": "%s", "servers": [{"name": "Battlefield 1943 Reborn", "description": "", "maps": ["wake", "guadalcanal", "iwo", "coral"], "round_end_delay": 45, "listed": true, "port": 18300}]}\n' \
    "$ONLINE_HOST" > "$T/server.default.json"
  # The C++ runtime from GCC 13, for systems with an older one, and the X11 and Wayland client libraries the program
  # is linked with, for a host without a desktop (bf1943-server uses each only when the system has none).
  # BF_SERVER_LIB_DIR: take them from the system the game was built on (the Ubuntu 22.04 root's
  # /srv/jammy/usr/lib/x86_64-linux-gnu, build_in_ubuntu2204.sh), not this one: this system's need its newer glibc
  # (16 Nov 2026: copies from Ubuntu 24.04 needed glibc 2.38, so the archive failed on 22.04 and Debian 12 servers).
  LIBDIR="${BF_SERVER_LIB_DIR:-}"
  for lib in libstdc++.so.6 libgcc_s.so.1; do
    if [ -n "$LIBDIR" ]; then cp -L "$LIBDIR/$lib" "$T/lib/"; else cp -L "$(g++ -print-file-name=$lib)" "$T/lib/"; fi
  done
  for lib in libX11-xcb.so.1 libX11.so.6 libxcb.so.1 libwayland-client.so.0 libXau.so.6 libXdmcp.so.6 libffi.so.8 libbsd.so.0 libmd.so.0; do
    if [ -n "$LIBDIR" ]; then f="$LIBDIR/$lib"; [ -e "$f" ] || f=""
    else f="$(ldconfig -p | awk -v l="$lib" '$1==l && /x86-64/ && !seen {print $NF; seen=1}')"; fi
    [ -n "$f" ] || { echo "error: $lib is not on this system" >&2; exit 1; }
    cp -L "$f" "$T/lib/desktop/"
  done
  sed "s/@VERSION@/$VERSION/" "$WT/tools/linux/server/README.txt" > "$T/README.txt"
  sed "s/@VERSION@/$VERSION/" "$WT/tools/linux/server/bf1943-reborn-server.service" > "$T/bf1943-reborn-server.service"
  echo "== Missing libraries (with the bundled ones first)"
  for f in "$T/bin/bf1943" "$T"/bin/*.so*; do
    LD_LIBRARY_PATH="$T/bin:$T/lib/desktop:$T/lib" ldd "$f" 2>/dev/null | grep "not found" | sed "s|^|$(basename "$f"): |"
  done || true
  echo "== The newest glibc each file needs (the archive's README says 2.35 when built from the Ubuntu 22.04 root)"
  for f in "$T/bin/bf1943" "$T"/bin/*.so* "$T"/lib/*.so* "$T"/lib/desktop/*.so*; do
    echo "  $(basename "$f"): $(objdump -T "$f" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -t. -k2,2n -k3,3n -u | tail -1)"
  done
  echo "== No Windows or game files, no key"
  if find "$T" \( -iname "*.dll" -o -iname "*.exe" -o -iname "default.xex" -o -iname "*.key" -o -iname "*.fbrb" \) | grep .; then
    echo "error: the files above must not ship" >&2; exit 1
  fi
}

tarball() {
  mkdir -p "$OUT_DIR"
  local OUT="$OUT_DIR/$NAME.tar.gz"
  tar -C "$STAGE" --owner=0 --group=0 -czf "$OUT" "$NAME"
  echo "$OUT: $(stat -c %s "$OUT") bytes, SHA-256 $(sha256sum "$OUT" | cut -d" " -f1)"
}

# The Python the AppImage carries: this system interpreter, its library (no tests, no Tk, no installers) and the
# shared libraries its modules load that are not part of the C library. BF_SERVER_PYTHON_ROOT: the one of the system
# the game was built on instead (the Ubuntu 22.04 root, /srv/jammy: Python 3.10 for glibc 2.35; this system's Python
# needs its newer glibc), its libraries found inside that root (chroot, as root).
python_into() {
  local P="$1" R="${BF_SERVER_PYTHON_ROOT:-}" PY LIBDIR V
  in_root() { if [ -n "$R" ]; then chroot "$R" "$@"; else "$@"; fi; }
  PY="$(in_root readlink -f /usr/bin/python3)"
  [ -n "$R" ] || PY="$(readlink -f "$(command -v python3)")"
  LIBDIR="$(in_root "$PY" -c "import sysconfig; print(sysconfig.get_path('stdlib'))")"
  V="$(basename "$LIBDIR")"  # python3.12, or python3.10 from the 22.04 root
  mkdir -p "$P/bin" "$P/lib/$V"
  cp -L "$R$PY" "$P/bin/python3"
  rsync -a --exclude test --exclude tests --exclude idle_test --exclude tkinter --exclude idlelib --exclude turtledemo \
    --exclude ensurepip --exclude lib2to3 --exclude pydoc_data --exclude venv --exclude sqlite3 --exclude unittest \
    --exclude __pycache__ --exclude "config-*" --exclude sitecustomize.py --exclude dist-packages --exclude site-packages \
    "$R$LIBDIR/" "$P/lib/$V/"
  { in_root ldd "$PY"; for m in "$R$LIBDIR/lib-dynload"/*.so; do in_root ldd "${m#"$R"}"; done; } 2>/dev/null |
    awk '/=> \// {print $3}' | sort -u |
    { grep -Ev "/(libc|libm|libdl|libpthread|librt|libutil|libresolv|libnsl|ld-linux[^/]*)\.so" || true; } |
    while read -r f; do cp -L "$R$f" "$P/lib/"; done
  LD_LIBRARY_PATH="$P/lib" "$P/bin/python3" -c "import ssl, json, asyncio, zipfile, tarfile, http.server, ctypes, hashlib, lzma, bz2, sys; print('bundled Python', sys.version.split()[0], ssl.OPENSSL_VERSION, sys.prefix)"
}

appimage() {
  local A="$BASE/AppDir-server"
  echo "== AppDir ($A)"
  rm -rf "$A"; mkdir -p "$A"
  cp -a "$T" "$A/tree"
  python_into "$A/tree/python"
  cp "$WT/bf/data/linux/bf1943-reborn.png" "$A/bf1943-reborn-server.png"
  cp "$A/bf1943-reborn-server.png" "$A/.DirIcon"
  cp "$WT/tools/linux/server/bf1943-reborn-server.desktop" "$A/bf1943-reborn-server.desktop"
  cp "$WT/tools/linux/server/AppRun" "$A/AppRun"
  chmod +x "$A/AppRun"
  mkdir -p "$OUT_DIR"
  local OUT="$OUT_DIR/BF1943-Reborn-Server-$VERSION-x86_64.AppImage"
  if [ ! -s "$RT" ] && [ -x "$TOOL" ]; then off="$("$TOOL" --appimage-offset)" && head -c "$off" "$TOOL" > "$RT"; fi
  ARCH=x86_64 "$TOOL" --appimage-extract-and-run ${RT:+--runtime-file "$RT"} "$A" "$OUT" > "$BASE/appimagetool-server.log" 2>&1 ||
    { tail -20 "$BASE/appimagetool-server.log"; exit 1; }
  echo "$OUT: $(stat -c %s "$OUT") bytes, SHA-256 $(sha256sum "$OUT" | cut -d" " -f1)"
}

stage
case "$WHAT" in
  tar) tarball ;;
  appimage) appimage ;;
  all) tarball; appimage ;;
  stage) ;;
  *) echo "usage: build_server.sh <version> [tar|appimage|all|stage] [output folder]" >&2; exit 2 ;;
esac
