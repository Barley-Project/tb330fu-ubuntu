#!/bin/sh
# TB330FU audio bring-up: MT6768 audio front end (AFE) + MT6358 codec + four AW883xx smart amplifiers (I2C 0x34..0x37 on i2c@1100f000) -> sound card.
#
# The stock modules are loaded in dependency order from /usr/lib/tb330fu/audio (order.txt).  snd-soc-mt6768-afe imports three functions of the vendor system
# power manager (MTK_INTERNAL_SPM.ko: SPM resource requests, idle notifier); that module drags in 17 more vendor modules and the SPM firmware, so
# tb330fu_spmstub.ko (no-op functions, exported under the CRCs the audio driver expects) stands in for it.
# Needs: i2c-mt65xx (loaded with the OTG chain), the PMIC (regulators, auxadc, efuse), /vendor/firmware/aw883xx_acf.bin.
# Crash-loop guard: /var/lib/tb330fu/no-audio exists -> do nothing; /var/lib/tb330fu/audio-started exists -> the previous boot died within 2 minutes of the
# bring-up: skip it this time.  (A panic here must not cost the desktop.)
A=/usr/lib/tb330fu/audio
M=/usr/lib/tb330fu/modules
D=/var/lib/tb330fu
K() { echo "TB330FU audio: $*" > /dev/kmsg; echo "$*"; }
[ -e "$D/no-audio" ] && { K "skipped: $D/no-audio exists"; exit 0; }
if [ -e "$D/audio-started" ]; then
    K "the previous boot did not survive the audio bring-up (marker $D/audio-started): skipping it this time"
    rm -f "$D/audio-started"; sync; exit 0
fi
[ -s "$A/order.txt" ] || { K "no module list $A/order.txt"; exit 0; }
# the PA bus: i2c-mt65xx comes with the OTG chain a few seconds after boot
i=0
while [ ! -d /sys/bus/platform/devices/1100f000.i2c ] && [ "$i" -lt 90 ]; do i=$((i + 1)); sleep 1; done
[ -d /sys/bus/platform/devices/1100f000.i2c ] || { K "i2c@1100f000 is not there (OTG chain not loaded?): no audio"; exit 0; }
: > "$D/audio-started"; sync
ins() {   # $1 = path of the .ko
    n=$(basename "$1" .ko | tr - _)
    [ -d "/sys/module/$n" ] && return 0
    out=$(insmod "$1" 2>&1) && { K "loaded $(basename "$1")"; return 0; }
    K "insmod $(basename "$1") FAILED: $out"; return 1
}
ins "$M/tb330fu_spmstub.ko" || exit 1
while read -r m; do
    case "$m" in ''|'#'*) continue ;; esac
    ins "$A/$m.ko" || exit 1
done < "$A/order.txt"
# the sound card appears when the machine driver finds all its components (AFE, codec, 4 amplifiers)
i=0
while ! grep -q '[0-9]' /proc/asound/cards 2>/dev/null && [ "$i" -lt 30 ]; do i=$((i + 1)); sleep 1; done
if grep -q '[0-9]' /proc/asound/cards 2>/dev/null; then
    K "sound card: $(grep -E '^ *[0-9]' /proc/asound/cards | head -3 | tr '\n' ' ')"
else
    K "no sound card after loading the modules (dmesg | grep -i -E 'asoc|snd|aw883|mt6358|afe')"
fi
systemd-run --quiet --no-block --unit=tb330fu-audio-guard-clear --on-active=120 /bin/sh -c "rm -f $D/audio-started; sync" 2>/dev/null || rm -f "$D/audio-started"
exit 0
