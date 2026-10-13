#!/bin/bash
# Build the static aarch64 BusyBox that stage1 runs in the initramfs (BusyBox 1_36_stable, defconfig + CONFIG_STATIC, without tc).
# Output: $TB_BUSYBOX (default $TB_WORK/busybox/busybox)
set -euo pipefail
. "$(dirname "$0")/lib/common.sh"
need git make aarch64-linux-gnu-gcc
src=$TB_WORK/busybox/src; build=$TB_WORK/busybox/build
mkdir -p "$TB_WORK/busybox" "$build"
[ -d "$src/.git" ] || git ${TB_PROXY:+-c http.proxy="$TB_PROXY"} clone --depth=1 --branch 1_36_stable https://github.com/mirror/busybox.git "$src"
git -C "$src" rev-parse HEAD
cd "$src"
make O="$build" defconfig
sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' "$build/.config"
# newer host kernels dropped the CBQ UAPI that the optional BusyBox tc uses; tc is not a stage1 prerequisite (Ubuntu brings iproute2)
sed -i 's/CONFIG_TC=y/# CONFIG_TC is not set/' "$build/.config"
make -j"$(nproc)" O="$build" CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 busybox
file "$build/busybox"
for app in sh insmod mount mknod blkid readlink sed switch_root mdev; do qemu-aarch64 "$build/busybox" --list | grep -qx "$app" || die "applet missing: $app"; done
cp "$build/busybox" "${TB_BUSYBOX:-$TB_WORK/busybox/busybox}"
sha256sum "${TB_BUSYBOX:-$TB_WORK/busybox/busybox}"
