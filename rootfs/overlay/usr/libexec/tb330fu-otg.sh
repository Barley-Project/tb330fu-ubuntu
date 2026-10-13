#!/bin/sh
# TB330FU late USB OTG / device-mode bring-up (rootfs copy; the stage1 RAM copy is replaced by /etc/systemd/system/tb330fu-otg.service).
#
# Loads the vendor Type-C / charger / role-switch modules that stage1 staged in /dev/.tb330fu/mods (list: /dev/.tb330fu/otg.list), in the stock order
# EXCEPT that extcon-mtk-usb comes BEFORE tcpci_late_sync, and WITHOUT pauses between rt_pd_manager, extcon-mtk-usb and tcpci_late_sync:
#   tcpci_late_sync enables the TCPC interrupt, the TCPC then immediately posts the "cable attached" event (old state UNATTACHED -> new state SNK for a PC,
#   SRC for a dock); extcon-mtk-usb only switches the USB role when it is registered at that moment (the first version slept 1 s between the insmods, loaded it
#   AFTER tcpci_late_sync and so missed the event whenever the cable was plugged in before the chain was loaded; replugging the cable fixed it).
# Fallback: if a host is attached but the UDC never gets "configured", the role is kicked none -> device.
#
# Crash-loop guard (a panic here must not cut the only remote channel for ever):
#   /var/lib/tb330fu/no-otg        exists -> do nothing
#   /var/lib/tb330fu/otg-started   exists -> the previous boot died before the chain had been alive for 2 minutes: skip the chain this time
M=/dev/.tb330fu/mods
LIST=/dev/.tb330fu/otg.list
D=/var/lib/tb330fu
K() { echo "TB330FU OTG: $*" > /dev/kmsg; }
INSMOD="$(command -v insmod || echo /usr/sbin/insmod)"
snap() {
    K "---- $1"
    K "tcpc class: $(ls /sys/class/tcpc 2>/dev/null | tr '\n' ' ')"
    for r in /sys/class/usb_role/*; do [ -e "$r" ] && K "usb_role $(basename "$r") role=$(cat "$r/role" 2>/dev/null)"; done
    for u in /sys/class/udc/*; do [ -e "$u" ] && K "udc $(basename "$u") state=$(cat "$u/state" 2>/dev/null)"; done
    K "power supplies: $(for p in /sys/class/power_supply/*; do [ -e "$p" ] && printf '%s(online=%s) ' "$(basename "$p")" "$(cat "$p/online" 2>/dev/null)"; done)"
    K "usb devices: $(ls /sys/bus/usb/devices 2>/dev/null | tr '\n' ' ')"
}
mkdir -p "$D" 2>/dev/null
if [ -e "$D/no-otg" ]; then
    K "skipped: $D/no-otg exists"
    exit 0
fi
if [ -e "$D/otg-started" ]; then
    K "the previous boot did not stay up for 2 minutes after loading the OTG chain (marker $D/otg-started): skipping the chain this time, the next boot tries again"
    rm -f "$D/otg-started"
    sync
    exit 0
fi
[ -s "$LIST" ] || { K "no module list $LIST (stage1 did not stage the OTG modules)"; exit 0; }
sleep "${OTG_DELAY:-12}"
snap "before loading"
: > "$D/otg-started"
sync
load() {
    [ -f "$M/$1.ko" ] || { K "module $1.ko is not staged"; return 1; }
    out="$(timeout 60 "$INSMOD" "$M/$1.ko" 2>&1)"
    rc=$?
    K "insmod $1.ko rc=$rc${out:+ ($out)}"
    return $rc
}
# 1. everything up to rt_pd_manager in the staged order (pauses only here: these modules probe I2C devices)
while read -r m; do
    case "$m" in
        ''|extcon-mtk-usb|tcpci_late_sync) continue ;;
    esac
    load "$m"
    case "$m" in
        rt_pd_manager) ;;
        *) sleep 1 ;;
    esac
done < "$LIST"
# 2. the role switch first, then the late sync that releases the TCPC state machine: back to back
load extcon-mtk-usb
load tcpci_late_sync
sleep 6
snap "after loading"
# 3. watch for 10 minutes; kick the role if a host is attached and the gadget never connects
n=0
kicked=0
while [ "$n" -lt 40 ]; do
    sleep 15
    n=$((n + 1))
    if [ "$n" -eq 8 ]; then
        rm -f "$D/otg-started"
        sync
        K "the OTG module chain has been alive for 2 minutes, crash marker removed"
    fi
    st="$(cat /sys/class/udc/*/state 2>/dev/null | head -1)"
    role="$(cat /sys/class/usb_role/*/role 2>/dev/null | head -1)"
    online="$(cat /sys/class/power_supply/charger/online 2>/dev/null)"
    K "watch $n: udc=$st role=$role charger_online=$online usb=$(ls /sys/bus/usb/devices 2>/dev/null | tr '\n' ' ')"
    if [ "$kicked" -lt 3 ] && [ "$online" = 1 ] && [ "$st" = "not attached" ] && [ "$role" != host ]; then
        kicked=$((kicked + 1))
        K "a host is attached (charger online) but the UDC is not attached: kicking the role none -> device ($kicked)"
        for r in /sys/class/usb_role/*/role; do
            echo none > "$r" 2>/dev/null
            sleep 1
            echo device > "$r" 2>/dev/null
        done
    fi
done
snap "after 10 minutes"
