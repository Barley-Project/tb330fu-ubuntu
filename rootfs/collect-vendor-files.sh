#!/bin/bash
# Collect the vendor files this port needs from YOUR OWN copy of the ROW074 firmware. They are proprietary and are not part of this repository.
#
# usage: collect-vendor-files.sh <vendor firmware dir> <vendor_dlkm module dir> [<out dir>]
#   <vendor firmware dir>   contents of the "vendor" partition's firmware directory (vendor/firmware)
#   <vendor_dlkm module dir> lib/modules of the vendor_dlkm partition (the 172 vendor modules of the gda kernel)
#   <out dir>               default $TB_ROOTFS_WORK/vendor-files; build-rootfs.sh picks it up from there
#
# Result:  <out>/firmware/*        -> /vendor/firmware       (Wi-Fi/Bluetooth/CONNSYS firmware and configs, touch firmware)
#          <out>/conn/*.ko         -> /usr/lib/tb330fu/conn   (CONNSYS / Wi-Fi / Bluetooth modules, load order in order.txt / order_bt.txt)
#          <out>/keys/*.ko         -> /usr/lib/tb330fu/keys   (power / volume key drivers)
#          <out>/cpufreq/*.ko      -> /usr/lib/tb330fu/cpufreq (CPU DVFS stack: SSPM + cpufreq_v2)
#          <out>/audio/*.ko        -> /usr/lib/tb330fu/audio   (AFE, MT6358 codec, AW883xx smart PAs; load order in order.txt)
set -euo pipefail
. "$(dirname "$0")/../scripts/lib/common.sh"
FW=${1:?usage: $0 <vendor firmware dir> <vendor_dlkm module dir> [out dir]}
DLKM=${2:?usage: $0 <vendor firmware dir> <vendor_dlkm module dir> [out dir]}
OUT=${3:-$TB_ROOTFS_WORK/vendor-files}
HERE=$TB_ROOT/rootfs/vendor
mkdir -p "$OUT"/{firmware,conn,keys,cpufreq,audio}
copy_list() {   # <list file> <source dir> <dest dir> [suffix]
    local n=0 miss=0 f
    while IFS= read -r f; do
        case "$f" in ''|'#'*) continue ;; esac
        if [ -f "$2/$f$4" ]; then cp "$2/$f$4" "$3/"; n=$((n + 1)); else echo "missing: $2/$f$4"; miss=$((miss + 1)); fi
    done < "$1"
    echo "$(basename "$1"): $n copied, $miss missing"
}
copy_list "$HERE/firmware.list" "$FW" "$OUT/firmware"
copy_list "$HERE/conn-modules.list" "$DLKM" "$OUT/conn" .ko
copy_list "$HERE/keys-modules.list" "$DLKM" "$OUT/keys" .ko
copy_list "$HERE/cpufreq-modules.list" "$DLKM" "$OUT/cpufreq" .ko
copy_list "$HERE/audio-modules.list" "$DLKM" "$OUT/audio" .ko
cp "$HERE/audio-order.txt" "$OUT/audio/order.txt"
cp "$HERE/conn-order.txt" "$OUT/conn/order.txt"
cp "$HERE/conn-order-bt.txt" "$OUT/conn/order_bt.txt"
echo "vendor files collected in $OUT"
