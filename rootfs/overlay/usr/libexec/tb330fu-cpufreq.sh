#!/bin/sh
# TB330FU CPU frequency scaling. The bootloader leaves the CPUs at one fixed operating point (A55 cluster 1175 MHz, A75 cluster 1087 MHz, 0.8 V) and the
# kernel has no cpufreq driver of its own. The vendor "cpufreq_v2" stack (hybrid DVFS) does it: the PLL and voltage switching is done by the SSPM firmware
# (started by the preloader) and the CSPM hardware, CPU_DVFS.ko only talks to them over IPI and registers the cpufreq policies:
#   policy0 = cpu0-5 (A55) 500 .. 1800 MHz, policy6 = cpu6-7 (A75) 850 .. 2000 MHz, governor schedutil.
# The vendor modules are copied from the ROW074 vendor_dlkm into /usr/lib/tb330fu/cpufreq (stock dependency order below).
# Crash-loop guard: /var/lib/tb330fu/no-cpufreq exists -> do nothing; /var/lib/tb330fu/cpufreq-started exists -> the previous boot died within 2 minutes of the
# bring-up: skip it once.
M=/usr/lib/tb330fu/cpufreq
D=/var/lib/tb330fu
K() { echo "TB330FU cpufreq: $*" > /dev/kmsg; echo "$*"; }
mkdir -p "$D"
[ -e "$D/no-cpufreq" ] && { K "skipped: $D/no-cpufreq exists"; exit 0; }
if [ -e "$D/cpufreq-started" ]; then
    K "the previous boot did not stay up for 2 minutes after the cpufreq bring-up (marker $D/cpufreq-started): skipping it this time"
    rm -f "$D/cpufreq-started"; sync; exit 0
fi
: > "$D/cpufreq-started"; sync
rc=0
for m in emimpu pmic_lbat_service pmic_lvsys_notify mtk_low_battery_throttling sspm_v1 mtk_cpuhp mtk_mdpm_v1 mtk_dynamic_loading_throttling mtk_pbm \
         mtk_battery_oc_throttling mediatek_static_power Upower mtk_ppm_v3 CPU_DVFS tb330fu_ppmtemp; do
    [ -d "/sys/module/$m" ] && continue
    [ -f "$M/$m.ko" ] || { [ "$m" = tb330fu_ppmtemp ] && continue; K "$M/$m.ko is missing"; rc=1; break; }
    if ! out=$(timeout 60 insmod "$M/$m.ko" 2>&1); then K "insmod $m failed: $out"; rc=1; break; fi
done
if [ "$rc" -eq 0 ] && [ -d /sys/devices/system/cpu/cpufreq/policy0 ]; then
    K "ready: $(for p in /sys/devices/system/cpu/cpufreq/policy*; do printf '%s %s-%s kHz  ' "$(basename "$p")" "$(cat "$p/scaling_min_freq")" "$(cat "$p/scaling_max_freq")"; done)"
elif [ "$rc" -eq 0 ]; then
    K "modules loaded but no cpufreq policy appeared"
fi
# survived the bring-up: drop the crash marker after 2 minutes (a timer unit, independent of this script)
systemd-run --quiet --no-block --unit=tb330fu-cpufreq-guard-clear --on-active=120 /bin/sh -c "rm -f $D/cpufreq-started; sync" 2>/dev/null || ( sleep 120; rm -f "$D/cpufreq-started" ) &
exit "$rc"
