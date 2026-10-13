#!/bin/sh
# TB330FU GPU bring-up (runs before the desktop session): Mali-G52 MC2 through Panfrost, GPU rendering for KWin via Mesa's kmsro.
#   1. simpledrm must be the module from /usr/lib/tb330fu/modules (scan-out ring, backlight, drm_name / gpu_sync parameters); the vendor_boot ramdisk may carry
#      an older one, which is then replaced here (nothing holds it yet: the session has not started)
#   2. panfrost.ko (Panfrost + DRM scheduler + MT6768 power sequencing in one module) powers the GPU and creates /dev/dri/renderD128
#   3. only when the render node exists: the display driver reports the name "mediatek" (Mesa then uses Panfrost for rendering into this device's dumb buffers)
#      and gpu_sync=1 (the CPU cache of every GPU-rendered source buffer is invalidated before the scan-out copy)
# Crash-loop guard: /var/lib/tb330fu/no-gpu exists -> do nothing; /var/lib/tb330fu/gpu-started exists -> the previous boot died within 2 minutes of the
# bring-up: skip it once (software rendering, as before).
M=/usr/lib/tb330fu/modules
D=/var/lib/tb330fu
K() { echo "TB330FU GPU: $*" > /dev/kmsg; }
mkdir -p "$D"
if [ -e "$D/no-gpu" ]; then K "skipped: $D/no-gpu exists"; exit 0; fi
if [ -e "$D/gpu-started" ]; then
    K "the previous boot did not stay up for 2 minutes after the GPU bring-up (marker $D/gpu-started): skipping it this time"
    rm -f "$D/gpu-started"; sync; exit 0
fi
: > "$D/gpu-started"; sync
if [ ! -e /sys/module/simpledrm/parameters/drm_name ]; then
    K "replacing simpledrm by $M/simpledrm.ko"
    rmmod simpledrm 2>/dev/null
    # without any display driver the desktop cannot start: if the new module is refused, fall back to the last version known to work on this device
    if ! insmod "$M/simpledrm.ko"; then
        K "simpledrm.ko FAILED to load, trying $M/simpledrm.ko.good"
        insmod "$M/simpledrm.ko.good" || { K "simpledrm.ko.good FAILED as well, no display driver"; rm -f "$D/gpu-started"; exit 1; }
    fi
    sleep 1
fi
if [ ! -d /sys/module/panfrost ]; then
    insmod "$M/panfrost.ko" || { K "panfrost.ko FAILED to load"; rm -f "$D/gpu-started"; exit 1; }
fi
i=0
while [ ! -c /dev/dri/renderD128 ] && [ "$i" -lt 400 ]; do i=$((i + 1)); sleep 0.1; done
if [ ! -c /dev/dri/renderD128 ]; then K "no render node after 40 s, staying with software rendering"; rm -f "$D/gpu-started"; exit 1; fi
echo mediatek > /sys/module/simpledrm/parameters/drm_name
echo 1 > /sys/module/simpledrm/parameters/gpu_sync
K "GPU ready: Panfrost render node present, display driver name '$(cat /sys/module/simpledrm/parameters/drm_name)', gpu_sync=$(cat /sys/module/simpledrm/parameters/gpu_sync)"
( sleep 120; rm -f "$D/gpu-started"; sync; K "GPU bring-up marker removed after 2 minutes" ) &
exit 0
