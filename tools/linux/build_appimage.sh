#!/usr/bin/env bash
# The Battlefield 1943 Reborn AppImage from the WSL Linux build (bf_linux_build.sh), King Kong's way
# (F:\KK-native-renderer\wt\v2.1.0\tools\build_appimage.sh): the AppDir, its AppRun (runs from a copy in the cache),
# the C++ runtime for older systems, the missing library check, appimagetool.
#
#   bash bf_appimage.sh <version> [Release|RelWithDebInfo] [output folder]
#
# The file name has no version (BF1943-Reborn-linux-x86_64.AppImage): Steam shortcuts keep working after an update.
set -euo pipefail
VERSION="${1:?usage: bf_appimage.sh <version, e.g. 0.1.33-preview> [config] [output folder]}"
CONFIG="${2:-Release}"
BASE="${BF_LINUX_ROOT:-$HOME/bf-linux}"
OUT_DIR="${3:-$BASE/dist}"
WT="${BF_LINUX_SOURCE:-${BF_LINUX_MAIN:-/mnt/f/BF-1942-MP-Decomp}}"  # the main tree (from v0.2.0 the Linux work is merged)
LICENCES="${BF_LICENCES:-/mnt/f/BF-1942-MP-Decomp/runs/release/0.1.33-preview/BF1943-Reborn-0.1.33-preview-windows-x64/licences}"
ONLINE_HOST="${BF_ONLINE_HOST:?set BF_ONLINE_HOST to the main backend address (the official one, or 127.0.0.1 for a local test)}"
P=linux-amd64-release
[ "$CONFIG" = RelWithDebInfo ] && P=linux-amd64-relwithdebinfo
B="$BASE/src/bf/out/build/$P"
SDK="$BASE/sdk"
A="$BASE/AppDir-$P"
TOOL="${BF_APPIMAGETOOL:-/root/kk-linux-beta/work/appimagetool-1.9.1-x86_64.AppImage}"  # King Kong's pinned copy

echo "== AppDir ($B)"
rm -rf "$A"
mkdir -p "$A/usr/bin" "$A/usr/lib"
cp "$B/bf1943" "$A/usr/bin/"
# Anything the build put beside the program (the runtime, the GPU plugin), then the SDK install's own copies,
# which win (the build refreshes the ones beside the program only when it relinks).
find "$B" -maxdepth 1 -name '*.so*' -exec cp -a {} "$A/usr/bin/" \;
for lib in librexruntime.so librexgpu-bfnative.so libTracyClient.so; do
  f="$(find "$SDK" -name "$lib" -print -quit)"
  [ -z "$f" ] || cp -aL "$f" "$A/usr/bin/"
done
for d in data fonts glyphs; do cp -a "$B/$d" "$A/usr/bin/"; done
[ -d "$LICENCES" ] && cp -a "$LICENCES" "$A/usr/bin/licences" || echo "warning: no licences folder at $LICENCES"
# Runtime settings, as the Windows download's bf1943.toml: the official main backend.
printf '# Battlefield 1943 Reborn: runtime settings. Your own settings are in user/bf1943-settings.txt beside the AppImage.\nbf_online_host = "%s"\n' \
  "$ONLINE_HOST" > "$A/usr/bin/bf1943.toml"
# The C++ runtime from GCC 13 (GLIBCXX_3.4.32), for systems with an older one; AppRun uses it only then.
for lib in libstdc++.so.6 libgcc_s.so.1; do cp -L "$(g++ -print-file-name=$lib)" "$A/usr/lib/"; done
cp "$WT/bf/data/linux/bf1943-reborn.png" "$A/bf1943-reborn.png"
cp "$A/bf1943-reborn.png" "$A/.DirIcon"
cat > "$A/bf1943-reborn.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Battlefield 1943 Reborn
Comment=Battlefield 1943 (Xbox 360) on PC, online play restored. Bring your own copy of the game.
Exec=bf1943
Icon=bf1943-reborn
Categories=Game;
Terminal=false
EOF
cat > "$A/AppRun" <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
# Run from a copy outside the AppImage's mount (King Kong's AppRun): the mount is served by a helper that ends at the
# first signal, and Steam signals every process of a game at once when it stops it, so the game, still quitting,
# lost its own code (SIGBUS). The copy is made once per AppImage file (its size and time) under
# ~/.cache/bf1943-reborn/appimage; older copies are removed. BF_APPIMAGE_COPY=0 runs from the mount.
if [ -n "$APPIMAGE" ] && [ "${BF_APPIMAGE_COPY:-1}" != 0 ]; then
  ID="$(stat -c '%s-%Y' "$APPIMAGE" 2>/dev/null)"
  BASE="${XDG_CACHE_HOME:-$HOME/.cache}/bf1943-reborn/appimage"
  if [ -n "$ID" ] && [ ! -f "$BASE/$ID/.complete" ]; then
    for old in "$BASE"/*; do [ "$old" = "$BASE/$ID" ] || rm -rf "$old"; done
    PART="$BASE/.part-$$"
    rm -rf "$BASE/$ID"
    if mkdir -p "$PART" && cp -a "$HERE/." "$PART/" && touch "$PART/.complete"; then
      mv -T "$PART" "$BASE/$ID" 2>/dev/null || rm -rf "$PART"
    else
      rm -rf "$PART"
    fi
  fi
  [ -n "$ID" ] && [ -f "$BASE/$ID/.complete" ] && HERE="$BASE/$ID"
fi
LIBS="$HERE/usr/bin"
# The bundled C++ runtime only when the system's is older than the game needs; a newer system copy stays for the
# graphics drivers.
SYS_CXX="$(ldconfig -p 2>/dev/null | awk '/libstdc\+\+\.so\.6 .*x86-64/ {print $NF; exit}')"
if [ -z "$SYS_CXX" ] || ! grep -q GLIBCXX_3.4.32 "$SYS_CXX" 2>/dev/null; then LIBS="$LIBS:$HERE/usr/lib"; fi
export LD_LIBRARY_PATH="$LIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# X11 (XWayland) where there is one, unless the player chose: our Vulkan renderer's swap chain fails on a Wayland surface
# the SDK's presenter has used (VK_ERROR_OUT_OF_HOST_MEMORY on the ROG Ally), and Steam's Game Mode runs games
# through XWayland anyway. Without X11 the game still runs on Wayland, drawn by the SDK's renderer.
[ -n "$DISPLAY" ] && [ -z "$SDL_VIDEO_DRIVER" ] && export SDL_VIDEO_DRIVER=x11
exec "$HERE/usr/bin/bf1943" "$@"
EOF
chmod +x "$A/AppRun"

echo "== Missing libraries"
for f in "$A/usr/bin/bf1943" "$A"/usr/bin/*.so*; do
  LD_LIBRARY_PATH="$A/usr/bin:$A/usr/lib" ldd "$f" 2>/dev/null | grep "not found" | sed "s|^|$(basename "$f"): |"
done || true
echo "== No Windows or game files"
if find "$A" \( -iname '*.dll' -o -iname '*.exe' -o -iname 'default.xex' -o -iname '*.bfhd' \) | grep .; then
  echo "error: the files above must not ship" >&2; exit 1
fi

echo "== AppImage"
mkdir -p "$OUT_DIR"
OUT="$OUT_DIR/BF1943-Reborn-linux-x86_64.AppImage"
RT="$BASE/runtime-x86_64"
if [ ! -s "$RT" ] && [ -x "$TOOL" ]; then off="$("$TOOL" --appimage-offset)" && head -c "$off" "$TOOL" > "$RT"; fi
ARCH=x86_64 "$TOOL" --appimage-extract-and-run ${RT:+--runtime-file "$RT"} "$A" "$OUT" > "$BASE/appimagetool.log" 2>&1 ||
  { tail -20 "$BASE/appimagetool.log"; exit 1; }
SIZE=$(stat -c %s "$OUT")
SUM=$(sha256sum "$OUT" | cut -d' ' -f1)
echo "$OUT: $SIZE bytes ($(( SIZE / 1048576 )) MiB), SHA-256 $SUM"
echo "$VERSION" > "$OUT_DIR/version.txt"
