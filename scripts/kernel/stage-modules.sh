#!/bin/bash
# Pack the in-tree modules of a kernel build (scripts/kernel/build-kernel.sh <name>) for the root filesystem: usr/lib/modules/<release>/ with depmod files.
# The paths are under usr/ on purpose: /lib is a symlink to usr/lib (merged /usr) and "tar xf -C /" of an archive with a top-level lib/ replaces that symlink
# with a directory, after which no dynamic program can start (this happened once; the RESCUE block of vendor-boot/stage1.sh repairs it).
#   scripts/kernel/stage-modules.sh <name> [output.tar]       output defaults to $TB_DIST/kernel-modules.tar (what rootfs/build-rootfs.sh unpacks)
# Install on a running tablet:  tar xf kernel-modules.tar -C / --no-same-owner   (root), then  depmod <release>  and modprobe works for zram, squashfs, rfcomm, ...
# Only modules of the build that is also in boot.img may be used: they are signed with that build's ephemeral key (needed for GKI protected exports).
set -euo pipefail
. "$(dirname "$0")/../lib/common.sh"
[ $# -ge 1 ] || die "usage: stage-modules.sh <name> [output.tar]"
name=$1
B=$TB_WORK/kernel/build-$name
S=$B/modules-staged/lib/modules/$TB_KREL_TARGET
out=${2:-$TB_DIST/kernel-modules.tar}
[ -d "$S/kernel" ] || die "$S/kernel not found (build-kernel.sh $name first)"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/usr/lib/modules" "$(dirname "$out")"
cp -a "$S" "$T/usr/lib/modules/$TB_KREL_TARGET"
rm -f "$T/usr/lib/modules/$TB_KREL_TARGET/build" "$T/usr/lib/modules/$TB_KREL_TARGET/source"
n=$(find "$T/usr/lib/modules" -name '*.ko' | wc -l)
tar -C "$T" --owner=0 --group=0 --numeric-owner --sort=name -cf "$out" usr
echo "$n modules for $TB_KREL_TARGET -> $out ($(stat -c %s "$out") bytes)"
sha256sum "$out" | tee "$out.sha256"
