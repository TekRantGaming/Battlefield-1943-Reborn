#!/usr/bin/env bash
# Battlefield 1943 Reborn: the Linux build, in WSL (wsl -d Ubuntu-24.04, never the default distro), King Kong's
# recipe (F:\KK-native-renderer\wt\v2.1.0\tools\build_linux_beta.sh and native-renderer\docs\linux.md).
#
#   bash bf_linux_build.sh sdk|game|all [Release|RelWithDebInfo]
#
# Sources: bf/ from the main tree (BF_LINUX_SOURCE: another checkout's), bf/generated, bf/third_party and the
# SDK copy (sdk/src-bf) from the main checkout (not in git). Everything is built on the Linux disk (~/bf-linux):
# much faster than /mnt/f, and file names keep their case. Logs: ~/bf-linux/*.log.
set -euo pipefail
WHAT="${1:-all}"
CONFIG="${2:-Release}"
MAIN="${BF_LINUX_MAIN:-/mnt/f/BF-1942-MP-Decomp}"  # the Windows checkout, seen from WSL
WT="${BF_LINUX_SOURCE:-$MAIN}"  # the main tree (from v0.2.0 the Linux work is merged)
BASE="${BF_LINUX_ROOT:-$HOME/bf-linux}"
JOBS="${BF_JOBS:-$(( $(nproc) > 2 ? $(nproc) - 2 : 1 ))}"
T0=$(date +%s)
step() { echo "== [$(( $(date +%s) - T0 )) s] $*"; }
mkdir -p "$BASE/src/bf" "$BASE/src/sdk"

sync_sources() {
  step "sources"
  # Times kept (-t): rsync then copies only files whose size or time changed, and an edited file arrives newer than
  # its object file, so ninja rebuilds it. The builds' out/ folders are kept.
  rsync -rlpDt --delete --exclude /out --exclude /generated --exclude /third_party --exclude /assets \
    --exclude /CMakeUserPresets.json "$WT/bf/" "$BASE/src/bf/"
  rsync -rlpDt --delete "$MAIN/bf/generated/" "$BASE/src/bf/generated/"
  # The game's own default.xex (bf/assets, not in git): the code generator checks the generated code against it. It
  # stays in the build folder; nothing of it is packaged.
  rsync -rlpDt --delete "$MAIN/bf/assets/" "$BASE/src/bf/assets/"
  rsync -rlpDt --delete --exclude .git "$MAIN/bf/third_party/" "$BASE/src/bf/third_party/"
  rsync -rlpDt --delete --exclude /out "$MAIN/sdk/src-bf/" "$BASE/src/sdk/src-bf/"
  # NativeXRenderer applies its DiligentCore patches at configure time (cmake/apply_diligent_patches.cmake) and knows
  # an already patched checkout by git: changed files plus its stamp in the checkout's git folder. The Windows
  # checkout is patched; this copy has no git, so it gets a git folder of its own whose status shows a change, and
  # the Windows stamp (the patch series' hash).
  local D="$BASE/src/bf/third_party/native-x-renderer/thirdparty/diligent-core"
  # The stamp is in DiligentCore's git folder: inside the renderer's when the renderer is a submodule, inside the main
  # repository's when the renderer is plain files (the public repository).
  local STAMP=""
  for s in "$MAIN/.git/modules/bf/third_party/native-x-renderer/modules/thirdparty/diligent-core" \
           "$MAIN/.git/modules/bf/third_party/native-x-renderer/thirdparty/diligent-core"; do
    [ -f "$s/native-x-renderer-patches.stamp" ] && { STAMP="$s/native-x-renderer-patches.stamp"; break; }
  done
  [ -n "$STAMP" ] || { echo "DiligentCore is not patched yet: configure the Windows build first" >&2; exit 1; }
  if [ ! -d "$D/.git" ]; then
    git -C "$D" init -q
    echo "Linux build copy" > "$D/.bf-linux-copy"
    git -C "$D" add .bf-linux-copy
    git -C "$D" -c user.name=bf -c user.email=bf@localhost commit -q -m "Linux build copy (patched on Windows)"
    echo "patched" >> "$D/.bf-linux-copy"
  fi
  cp "$STAMP" "$D/.git/native-x-renderer-patches.stamp"
}

build_sdk() {
  step "SDK configure (clang 20, Vulkan, the native renderer plugin)"
  local S="$BASE/src/sdk/src-bf"
  if [ ! -f "$S/out/build/linux-amd64/CMakeCache.txt" ]; then
    (cd "$S" && cmake --preset linux-amd64 -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 \
       -DCMAKE_INSTALL_PREFIX="$BASE/sdk" -DREXGLUE_USE_VULKAN=ON -DREXGLUE_BUILD_TESTS=OFF \
       -DBF_NATIVE_RENDERER_DIR="$BASE/src/bf/native-renderer") > "$BASE/sdk-configure.log" 2>&1 ||
      { tail -30 "$BASE/sdk-configure.log"; exit 1; }
  fi
  step "SDK build ($JOBS jobs)"
  cmake --build "$S/out/build/linux-amd64" --config Release -j "$JOBS" -- -k 0 > "$BASE/sdk-build.log" 2>&1 ||
    { grep -E "error:|FAILED" "$BASE/sdk-build.log" | head -40; exit 1; }
  cmake --install "$S/out/build/linux-amd64" --config Release > "$BASE/sdk-install.log" 2>&1 ||
    { tail -20 "$BASE/sdk-install.log"; exit 1; }
  ls -la "$BASE/sdk/lib" | grep -E "\.so" | head -20 || true
}

build_game() {
  local P=linux-amd64-release
  [ "$CONFIG" = RelWithDebInfo ] && P=linux-amd64-relwithdebinfo
  local B="$BASE/src/bf/out/build/$P"
  step "game configure ($P${BF_DEV_TOOLS:+, dev tools $BF_DEV_TOOLS})"
  if [ ! -f "$B/CMakeCache.txt" ]; then
    (cd "$BASE/src/bf" && cmake --preset "$P" -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
       -DCMAKE_PREFIX_PATH="$BASE/sdk" -Drexglue_DIR="$BASE/sdk/lib/cmake/rexglue" \
       -DBF_DEV_TOOLS="${BF_DEV_TOOLS:-OFF}") > "$BASE/game-configure.log" 2>&1 ||
      { tail -40 "$BASE/game-configure.log"; exit 1; }
  fi
  # The release's version (BfVersion(): the pipeline sharing, the updater's comparison), as the Windows package's.
  cmake "$B" -DBF_VERSION="${BF_VERSION:-0.1.33-preview}" -DBF_DEV_TOOLS="${BF_DEV_TOOLS:-OFF}" >> "$BASE/game-configure.log" 2>&1 ||
    { tail -20 "$BASE/game-configure.log"; exit 1; }
  step "game build ($JOBS jobs)"
  cmake --build "$B" -j "$JOBS" -- -k 0 > "$BASE/game-build.log" 2>&1 ||
    { grep -E "error:|FAILED|undefined reference" "$BASE/game-build.log" | head -60; exit 1; }
  ls -la "$B/bf1943" && file "$B/bf1943"
}

case "$WHAT" in
  sdk) sync_sources; build_sdk ;;
  game) sync_sources; build_game ;;
  all) sync_sources; build_sdk; build_game ;;
  sync) sync_sources ;;
  *) echo "usage: bf_linux_build.sh sdk|game|all|sync [Release|RelWithDebInfo]" >&2; exit 2 ;;
esac
step "done"
