#!/bin/sh
# Compressed RAM swap: with 8 GB of RAM and no swap, a browser plus an Android container (Waydroid) runs into the OOM killer; zram swap costs little (zstd
# squeezes ~2.5x) and keeps the desktop alive.  Size = half of the RAM (the pages are stored compressed, the RAM they really take is ~1/5 of that).
# zram.ko is an in-tree module of the kernel build (CONFIG_ZRAM=m); skipped when it is missing (old kernel).
K() { echo "TB330FU zram: $*" > /dev/kmsg; }
[ -e /sys/block/zram0 ] || modprobe zram num_devices=1 2>/dev/null || { K "zram module not available, no swap"; exit 0; }
[ -e /sys/block/zram0 ] || { K "no zram0 after loading the module"; exit 0; }
grep -q '^/dev/zram0 ' /proc/swaps && exit 0
mem_kb=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)
echo zstd > /sys/block/zram0/comp_algorithm 2>/dev/null || true
echo $((mem_kb * 1024 / 2)) > /sys/block/zram0/disksize
[ -b /dev/zram0 ] || { maj_min=$(cat /sys/block/zram0/dev); mknod /dev/zram0 b "${maj_min%:*}" "${maj_min#*:}"; }
mkswap /dev/zram0 > /dev/null
swapon -p 100 /dev/zram0 && K "swap on /dev/zram0, $((mem_kb / 2048)) MiB (zstd)"
