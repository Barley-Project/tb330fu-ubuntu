#!/bin/sh
# TB330FU connectivity bring-up: the MT6768 SoC integrated CONNSYS (Wi-Fi 802.11ac + Bluetooth 5.4).
#   1. unlock: unsigned modules may export GKI "protected" symbols (rfkill, cfg80211, bluetooth are signed with another build key)
#   2. factory NVRAM (Wi-Fi) and BD address (Bluetooth) copied from the nvdata partition (mounted read-only for a moment)
#   3. WMT stack (vendor modules) + tb330fu-wmt-loader + tb330fu-wmt-launcher (service): the CONNSYS firmware is downloaded and started
#   4. Wi-Fi: wmt_chrdev_wifi + wlan_drv_gen4m_6768, NVRAM handed over, function switched on -> wlan0 (NetworkManager / wpa_supplicant)
#   5. Bluetooth: bluetooth + hci_uart + bt_drv_connac1x, tb330fu-btd bridges /dev/stpbt to hci0
# Crash-loop guard: /var/lib/tb330fu/no-conn exists -> do nothing; /var/lib/tb330fu/conn-started exists -> the previous boot did not get through the
# bring-up (the dangerous part: CONNSYS power-on, firmware download) and died: skip it once. The marker is written before the bring-up and removed
# 20 s after it succeeded (by a systemd timer, not by a child of this script), so a normal reboot never skips Wi-Fi.
M=/usr/lib/tb330fu/modules
C=/usr/lib/tb330fu/conn
D=/var/lib/tb330fu
N=$D/nvram
P=/run/tb330fu/prop
CC=CN
[ -r /etc/tb330fu/country ] && CC=$(cat /etc/tb330fu/country)
K() { echo "TB330FU CONN: $*" > /dev/kmsg; echo "$*"; }
ins() { f=$1; shift; out=$(insmod "$f" "$@" 2>&1) || case "$out" in *"File exists"*) ;; *) K "insmod $f failed: $out"; return 1;; esac; }
mkdir -p "$D" "$N"
if [ -e "$D/no-conn" ]; then K "skipped: $D/no-conn exists"; exit 0; fi
if [ -e "$D/conn-started" ]; then
    K "the previous boot did not finish the connectivity bring-up (marker $D/conn-started): skipping it this time"
    rm -f "$D/conn-started"; sync; exit 0
fi
: > "$D/conn-started"; sync

# 2. factory data
dev=""
for p in /sys/block/mmcblk0/mmcblk0p*; do
    if grep -q "^PARTNAME=nvdata$" "$p/uevent" 2>/dev/null; then dev=/dev/$(basename "$p"); fi
done
if [ -n "$dev" ] && [ -b "$dev" ]; then
    mkdir -p /mnt/nvdata
    if mount -o ro,noload -t ext4 "$dev" /mnt/nvdata 2>/dev/null; then
        for f in WIFI BT_Addr; do [ -s /mnt/nvdata/APCFG/APRDEB/$f ] && cp -f /mnt/nvdata/APCFG/APRDEB/$f "$N/$f"; done
        umount /mnt/nvdata
    else
        K "cannot mount $dev (nvdata): using the copies in $N"
    fi
fi

# 1. + 3. modules
ins "$M/tb330fu_gkiunlock.ko" || exit 1
ins "$C/rfkill.ko" || exit 1
ins "$C/cfg80211.ko" ieee80211_regdom=$CC || exit 1
for m in pmic_lbat_service pmic_lvsys_notify mtk_low_battery_throttling connadp btif_drv conn_md_drv emimpu wmt_drv; do ins "$C/$m.ko" || exit 1; done
sleep 1
rm -rf "$P"; mkdir -p "$P"
/usr/bin/python3 /usr/local/sbin/tb330fu-wmt-loader || { K "wmt_loader failed"; exit 1; }
systemctl start --no-block tb330fu-wmt-launcher.service
i=0
while [ ! -e "$P/vendor.connsys.launcher.ready" ] && [ "$i" -lt 90 ]; do i=$((i + 1)); sleep 1; done
if [ "$(cat "$P/vendor.connsys.launcher.ready" 2>/dev/null)" != "yes" ]; then K "CONNSYS power on failed or timed out"; exit 1; fi
K "CONNSYS firmware is running"

# 4. Wi-Fi
ins "$C/wmt_chrdev_wifi.ko" || exit 1
ins "$C/wlan_drv_gen4m_6768.ko" || exit 1
if [ -s "$N/WIFI" ]; then
    /usr/bin/python3 /usr/local/sbin/tb330fu-wlan-nvram "$N/WIFI" || K "writing the Wi-Fi NVRAM failed"
    echo 1 > /dev/wmtWifi || K "switching Wi-Fi on failed"
    i=0
    while [ ! -e /sys/class/net/wlan0 ] && [ "$i" -lt 30 ]; do i=$((i + 1)); sleep 1; done
    [ -e /sys/class/net/wlan0 ] && K "wlan0 is up" || K "no wlan0"
    command -v iw >/dev/null && iw reg set "$CC" 2>/dev/null
else
    K "no Wi-Fi NVRAM ($N/WIFI): Wi-Fi stays off"
fi

# 5. Bluetooth
for m in bluetooth btbcm btqca hci_uart; do ins "$C/$m.ko" || exit 1; done
ins "$C/bt_drv_connac1x.ko" || exit 1
systemctl start --no-block tb330fu-btd.service
[ -c /dev/rfkill ] && chmod 0666 /dev/rfkill
K "connectivity bring-up done"
# survived the bring-up: drop the crash marker shortly after (a timer unit, independent of this script and of the service cgroup)
systemd-run --quiet --no-block --unit=tb330fu-conn-guard-clear --on-active=20 /bin/sh -c "rm -f $D/conn-started; sync" 2>/dev/null || rm -f "$D/conn-started"
exit 0
