#!/bin/sh
# TB330FU hardware keys: power + volume down live in the PMIC (mt6358keys: pwrkey -> KEY_POWER, homekey -> KEY_VOLUMEDOWN), volume up is key 0 of the SoC
# keypad matrix (kp@10010000 -> KEY_VOLUMEUP). Both drivers are vendor modules (copied from the ROW074 vendor_dlkm into /usr/lib/tb330fu/keys, stock load
# order: mtk-pmic-keys, then mtk-kpd). Their platform devices already exist (DT + PMIC MFD), the drivers only have to be loaded.
# (The backlight is part of simpledrm: /sys/class/backlight/tb330fu_bl. tb330fu-keysd turns the screen off/on on a short press of the power key.)
# Crash guard: /var/lib/tb330fu/no-keys exists -> do nothing.
K_DIR=/usr/lib/tb330fu/keys
K() { echo "TB330FU keys: $*" > /dev/kmsg; }
[ -e /var/lib/tb330fu/no-keys ] && { K "skipped: /var/lib/tb330fu/no-keys exists"; exit 0; }
rc=0
for m in mtk-pmic-keys mtk-kpd; do
    if [ -d "/sys/module/$(echo "$m" | tr - _)" ]; then continue; fi
    if insmod "$K_DIR/$m.ko"; then K "loaded $m"; else K "$m FAILED to load"; rc=1; fi
done
exit $rc
