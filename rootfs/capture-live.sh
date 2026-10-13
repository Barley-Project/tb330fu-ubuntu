#!/bin/bash
# Capture the running tablet's root filesystem as a tar (over ssh, using tools/tb.sh settings), leaving out everything that is device-private or regenerated:
# factory NVRAM copies, ssh host keys, machine id, caches, journals, crash markers. The tar can be turned into an image with rootfs/make-image.sh.
#
# usage: capture-live.sh [<out.tar>]        default $TB_ROOTFS_WORK/live.tar
# The captured tar still contains the account's password hash and authorized_keys: do not publish it.
set -euo pipefail
. "$(dirname "$0")/../scripts/lib/common.sh"
OUT=${1:-$TB_ROOTFS_WORK/live.tar}
mkdir -p "$(dirname "$OUT")"
TB=$TB_ROOT/tools/tb.sh
EX=(--exclude=./proc --exclude=./sys --exclude=./dev --exclude=./run --exclude=./tmp --exclude=./mnt --exclude=./media --exclude=./lost+found
    --exclude=./var/cache/apt/archives --exclude=./var/lib/apt/lists --exclude=./var/log/journal --exclude=./var/lib/tb330fu --exclude=./etc/machine-id
    --exclude='./etc/ssh/ssh_host_*' --exclude=./etc/tb330fu-dev-build --exclude='./home/*/.cache' --exclude='./home/*/*.ko' --exclude='./root/.*')
echo "capturing the tablet's root filesystem ..."
bash "$TB" sudo "tar --numeric-owner --xattrs --acls -C / -cf - ${EX[*]} ." < /dev/null > "$OUT"
ls -la "$OUT"
