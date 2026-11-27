#!/usr/bin/env bash
# The Linux game built for older distributions (15 Nov 2026): inside an Ubuntu 22.04 root (glibc 2.35) made with
# debootstrap in WSL's Ubuntu 24.04, so the AppImage starts on Ubuntu 22.04, Linux Mint 21, Pop!_OS 22.04, Zorin 17 and
# Debian 12 as well. A build on Ubuntu 24.04 itself needs glibc 2.38 (the C23 strtol and sscanf, fmod, strlcpy), and
# those distributions stop it at once ("GLIBC_2.38 not found").
#
#   sudo bash build_in_ubuntu2204.sh setup                  the root at /srv/jammy (debootstrap) and its tools: Clang 20
#                                                           (apt.llvm.org), GCC 13's C++ library (ubuntu-toolchain-r),
#                                                           CMake 3.28 (Kitware's release), the SDK's -dev packages
#   sudo bash build_in_ubuntu2204.sh build <version> [all|game|sdk]   build_linux.sh inside the root
#   sudo bash build_in_ubuntu2204.sh appimage <version> [out]         build_appimage.sh on the host from the root's
#                                                           build, with the root's GCC 13 C++ runtime (glibc 2.35)
#   sudo bash build_in_ubuntu2204.sh check                  the glibc versions the build needs, and a start inside the root
#   build, appimage and check stop when a Linux fast path is missing from librexbase.so (fastpaths below; it was in v0.10's librexruntime.so).
#
# BF_JAMMY_ROOT: the root (default /srv/jammy). The sources come from BF_LINUX_SOURCE / the Windows checkout through
# /mnt/f, bind mounted into the root, as build_linux.sh expects.
set -euo pipefail
J="${BF_JAMMY_ROOT:-/srv/jammy}"
HERE="$(dirname "$(readlink -f "$0")")"
WHAT="${1:-}"

mounts() {
  mountpoint -q "$J/proc" || mount -t proc proc "$J/proc"
  for d in /dev /dev/pts /sys /mnt/f; do
    mountpoint -q "$J$d" || { mkdir -p "$J$d"; mount --bind "$d" "$J$d"; }
  done
}
inside() {
  chroot "$J" /usr/bin/env -i HOME=/root PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 "$@"
}
needs() {
  for f in "$@"; do
    [ -e "$f" ] || continue
    echo "  $(basename "$f"): $(objdump -T "$(readlink -f "$f")" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' |
                               sort -t. -k2,2n -k3,3n -u | tail -1)"
  done
}
# The Linux fast paths that must be in the build whatever kernel headers the root has (17 Nov 2026: pull request 28's
# PROCMAP_QUERY look up, ioctl 0xc0686611, was compiled out of 0.4.7 to 0.4.7.2 because the root's headers are 5.15).
# Stops the build when one is missing.
fastpaths() {
  local f="$1" n
  [ -e "$f" ] || { echo "FAST PATH CHECK: $f is missing" >&2; exit 1; }
  n=$(objdump -d --no-show-raw-insn "$(readlink -f "$f")" | grep -c -i '0xc0686611' || true)
  if [ "${n:-0}" -lt 1 ]; then
    echo "FAST PATH CHECK FAILED: $(basename "$f") has no PROCMAP_QUERY ioctl (0xc0686611): pull request 28's" >&2
    echo "  Linux 6.11 page look up was compiled out (see sdk/src-bf/src/base/proc_map.h)" >&2
    exit 1
  fi
  echo "  fast path check: $(basename "$f") has the PROCMAP_QUERY look up ($n use(s) of 0xc0686611)"
}

case "$WHAT" in
  setup)
    export DEBIAN_FRONTEND=noninteractive
    command -v debootstrap > /dev/null || { apt-get update -qq; apt-get install -y -qq debootstrap > /dev/null; }
    [ -x "$J/usr/bin/apt-get" ] ||
      debootstrap --variant=minbase --components=main,universe jammy "$J" http://archive.ubuntu.com/ubuntu
    cp /etc/resolv.conf "$J/etc/resolv.conf"
    printf '%s\n' "deb http://archive.ubuntu.com/ubuntu jammy main universe" \
      "deb http://archive.ubuntu.com/ubuntu jammy-updates main universe" \
      "deb http://security.ubuntu.com/ubuntu jammy-security main universe" > "$J/etc/apt/sources.list"
    mounts
    inside bash -c "apt-get update -qq && apt-get install -y -qq ca-certificates wget gnupg software-properties-common \
      lsb-release rsync git file xz-utils python3 ninja-build pkg-config make > /dev/null"
    inside bash -c "add-apt-repository -y ppa:ubuntu-toolchain-r/test > /dev/null && apt-get update -qq &&
      apt-get install -y -qq gcc-13 g++-13 libstdc++-13-dev > /dev/null"
    inside bash -c "[ -x /usr/bin/clang-20 ] || { wget -q https://apt.llvm.org/llvm.sh -O /tmp/llvm.sh && bash /tmp/llvm.sh 20 > /dev/null; }"
    inside bash -c "[ -x /usr/local/bin/cmake ] || { wget -q https://github.com/Kitware/CMake/releases/download/v3.28.3/cmake-3.28.3-linux-x86_64.tar.gz -O /tmp/cmake.tgz &&
      tar -xzf /tmp/cmake.tgz -C /opt && ln -sf /opt/cmake-3.28.3-linux-x86_64/bin/* /usr/local/bin/; }"
    inside bash -c "apt-get install -y -qq libasound2-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev libdecor-0-dev \
      libegl-dev libgl-dev libgles-dev libudev-dev libvulkan-dev libwayland-dev wayland-protocols libx11-dev libx11-xcb-dev \
      libxcb1-dev libxcb-randr0-dev libxcb-render0-dev libxcb-shape0-dev libxcb-shm0-dev libxcb-xfixes0-dev libxcb-xkb-dev \
      libxcursor-dev libxext-dev libxfixes-dev libxi-dev libxinerama-dev libxkbcommon-dev libxkbcommon-x11-dev \
      libxrandr-dev libxrender-dev libxss-dev libxtst-dev libgtk-3-dev zlib1g-dev libzstd-dev liblzma-dev libbz2-dev \
      uuid-dev libicu-dev libxml2-dev libzip-dev libsnappy-dev > /dev/null"
    inside bash -c 'echo "root: $(ldd --version | head -1); $(clang-20 --version | head -1); $(cmake --version | head -1)"'
    ;;
  build)
    V="${2:?version}"
    mounts
    cp "$HERE/build_linux.sh" "$J/root/build_linux_run.sh"
    inside env BF_VERSION="$V" nice -n 10 bash /root/build_linux_run.sh "${3:-all}" Release
    # SDK v0.11: the SDK's libraries are built beside the program (build_linux.sh).
    R="$J/root/bf-linux/src/bf/out/build/linux-amd64-release"
    needs "$R/bf1943" "$R"/librex*.so "$R"/libSDL3*.so*
    fastpaths "$R/librexbase.so"
    ;;
  appimage)
    V="${2:?version}"
    OUT="${3:-$HOME/bf-linux/dist-$V}"
    BF_LINUX_ROOT="$J/root/bf-linux" BF_APPIMAGE_RUNTIME="${BF_APPIMAGE_RUNTIME:-$HOME/bf-linux/runtime-x86_64}" \
      BF_CXX_LIB_DIR="$J/usr/lib/x86_64-linux-gnu" bash "$HERE/build_appimage.sh" "$V" Release "$OUT"
    needs "$J/root/bf-linux/AppDir-linux-amd64-release"/usr/bin/bf1943 "$J/root/bf-linux/AppDir-linux-amd64-release"/usr/bin/*.so \
      "$J/root/bf-linux/AppDir-linux-amd64-release"/usr/lib/*.so*
    fastpaths "$J/root/bf-linux/AppDir-linux-amd64-release/usr/bin/librexbase.so"
    ;;
  check)
    mounts
    needs "$J/root/bf-linux/AppDir-linux-amd64-release"/usr/bin/bf1943 "$J/root/bf-linux/AppDir-linux-amd64-release"/usr/bin/*.so \
      "$J/root/bf-linux/AppDir-linux-amd64-release"/usr/lib/*.so*
    fastpaths "$J/root/bf-linux/AppDir-linux-amd64-release/usr/bin/librexbase.so"
    inside bash -c 'cd /root/bf-linux/AppDir-linux-amd64-release/usr/bin && echo "inside: $(ldd --version | head -1)" &&
      echo "libraries not found: $(LD_LIBRARY_PATH=.:../lib ldd ./bf1943 | grep -c "not found")" &&
      { LD_LIBRARY_PATH=.:../lib timeout 20 ./bf1943 --help > /tmp/help.txt 2>&1 || true; } && head -3 /tmp/help.txt'
    ;;
  *)
    sed -n 2,16p "$0"
    exit 2
    ;;
esac
