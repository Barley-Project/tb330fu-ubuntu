#!/bin/sh
# TB330FU stage1: first userspace of the vendor_boot ramdisk. Picks the Ubuntu root on the internal eMMC, loads the early vendor module set, the INT_POL helper,
# the display/console modules, the persistent log module and (late, from RAM) the OTG chain, then switches to the real root.
# Derived from src/boot/stage1.sh of NevedomaZverushka (https://github.com/feliksyasnopolski/nevedoma-zverushka, GPL-3.0-or-later).
# @STAGE1_ID@ is replaced by vendor-boot/build-vendor-boot.sh with the hash prefix of this file.

PATH=/bin:/sbin
export PATH
. /etc/tb330fu-stage1.conf
PROFILE=${PROFILE:-debug}
ROOT_SELECTOR=${ROOT_SELECTOR:-LABEL=TB330FU_SERVER}
ROOT_SOURCE=${ROOT_SOURCE:-external}
ROOT_UUID=${ROOT_UUID:-}
ROOTFS_MIN_SECTORS=${ROOTFS_MIN_SECTORS:-0}

INTERNAL_MMC_CONTROLLER="11230000.mmc"
SD_MMC_CONTROLLER="11240000.mmc"
MODULE_DIR="/lib/modules"
SYSFS_ROOT="${SYSFS_ROOT:-/sys}"
DEV_ROOT="${DEV_ROOT:-/dev}"
MMC_DISCOVERY_TIMEOUT="${MMC_DISCOVERY_TIMEOUT:-30}"
HOLD_SECONDS="${HOLD_SECONDS:-30}"
DEADMAN_SECONDS="${DEADMAN_SECONDS:-150}"
INTPOL_APPLY="${INTPOL_APPLY:-1}"
HARVEST_SECONDS="${HARVEST_SECONDS:-0}"
HARVEST_FALLBACK_SECONDS="${HARVEST_FALLBACK_SECONDS:-240}"
EXTRA_MODULES="${EXTRA_MODULES:-1}"
DISPLAY_MODULES="${DISPLAY_MODULES:-1}"
DIRECT_SESSION="${DIRECT_SESSION:-1}"
OTG_SET="${OTG_SET:-1}"
OTG_MODULES="${OTG_MODULES:-mt635x-auxadc i2c-mt65xx tcpc_cps8851 aw35615_whole adapter_class charger_class sc8989x_charger rt_pd_manager tcpci_late_sync extcon-mtk-usb}"
BLKLOG="${BLKLOG:-1}"
BLKLOG_OK=0
CHARGER_FALLBACK="${CHARGER_FALLBACK:-normal}"
BIND_LISTS="${BIND_LISTS:-0}"
SYSRQ_TRIGGER="${SYSRQ_TRIGGER:-/proc/sysrq-trigger}"
bb() {
    /bin/busybox "$@"
}

kmsg() {
    echo "TB330FU INIT: $*" > /dev/kmsg 2>/dev/null || \
        echo "TB330FU INIT: $*" > /dev/console 2>/dev/null || true
}

make_static_nodes() {
    bb mkdir -p /dev /dev/pts
    # /dev/null FIRST and with NO redirection: on the fresh tmpfs a "2>/dev/null" makes the shell create /dev/null itself, as a REGULAR FILE
    # (earlier versions did that with the kmsg line below; the later mknod then failed with EEXIST).  systemd units with PrivateDevices=yes
    # (polkit, resolved, hostnamed, localed, ...) then died with 226/NAMESPACE ("/dev/null is not a device").
    bb rm -f /dev/null
    bb mknod /dev/null    c 1 3 || true
    bb chmod 666 /dev/null || true
    bb mknod /dev/kmsg    c 1 11 2>/dev/null || true
    bb mknod /dev/console c 5 1  2>/dev/null || true
    bb mknod /dev/tty     c 5 0  2>/dev/null || true
    bb mknod /dev/ptmx    c 5 2  2>/dev/null || true
    bb mknod /dev/zero    c 1 5  2>/dev/null || true
    bb mknod /dev/random  c 1 8  2>/dev/null || true
    bb mknod /dev/urandom c 1 9  2>/dev/null || true
    bb chmod 600 /dev/kmsg 2>/dev/null || true
    bb chmod 600 /dev/console 2>/dev/null || true
    bb chmod 666 /dev/tty /dev/ptmx /dev/null /dev/zero 2>/dev/null || true
    bb chmod 666 /dev/random /dev/urandom 2>/dev/null || true
    [ -c /dev/null ] || kmsg "TB330FU DIAG: /dev/null is NOT a character device"
}

mount_early() {
    bb mkdir -p /proc /sys /dev /dev/pts /newroot
    bb mount -t proc proc /proc || { kmsg "failed to mount proc"; return 1; }
    bb mount -t sysfs sysfs /sys || { kmsg "failed to mount sysfs"; return 1; }
    bb mount -t tmpfs -o mode=0755,nosuid tmpfs /dev || { kmsg "failed to mount tmpfs /dev"; return 1; }
    make_static_nodes || return 1
    bb mount -t devpts -o mode=0620,ptmxmode=0666 devpts /dev/pts || { kmsg "failed to mount devpts"; return 1; }
}

module_loaded() {
    modname="${1%.ko}"
    modname="$(echo "$modname" | bb tr '-' '_')"
    [ -d "/sys/module/$modname" ]
}

load_one() {
    module="$1"
    kmsg "insmod $module"
    if bb insmod "$MODULE_DIR/$module" 2>/dev/kmsg; then
        kmsg "loaded $module"
        return 0
    fi
    if module_loaded "$module"; then
        kmsg "$module already loaded"
        return 0
    fi
    kmsg "FAILED to load $module"
    return 1
}

log_watchdog_state() {
    for watchdog in "$SYSFS_ROOT"/class/watchdog/watchdog*; do
        [ -e "$watchdog" ] || continue
        kmsg "watchdog sysfs: $(bb basename "$watchdog")"
        for field in identity timeout state nowayout; do
            [ -r "$watchdog/$field" ] || continue
            value="$(bb cat "$watchdog/$field" 2>/dev/null)"
            kmsg "watchdog $field=${value:-empty}"
        done
    done
}

load_intpol_helper() {
    # The ROW kernel expects the MCUSYS interrupt-polarity (INT_POL) bits to be set before it runs; with the CN LK they are
    # not, and low-active peripheral SPIs (I2C, ...) storm.  tb330fu_intpol dumps the state and (apply=1) programs the bits.
    if [ ! -f "$MODULE_DIR/tb330fu_intpol.ko" ]; then
        kmsg "TB330FU DIAG: tb330fu_intpol.ko is not in the ramdisk"
        return 0
    fi
    kmsg "loading tb330fu_intpol (apply=$INTPOL_APPLY)"
    if bb insmod "$MODULE_DIR/tb330fu_intpol.ko" apply="$INTPOL_APPLY" dump=1 2>/dev/kmsg; then
        kmsg "loaded tb330fu_intpol.ko"
    else
        kmsg "TB330FU DIAG: tb330fu_intpol.ko FAILED to load; interrupt polarity stays as the firmware left it"
    fi
    return 0
}

load_stock_early_modules() {
    # Stock Android first-stage init uses libmodprobe, which consumes the
    # normal modules.load request list plus modules.dep/modules.softdep.  The
    # native initramfs intentionally keeps its static init and BusyBox shell;
    # modules.load.native is the equivalent dependency-resolved request order
    # generated from those preserved stock metadata files.  No module options
    # file exists in the oracle.
    failed=0
    kmsg "loading stock normal-boot module set"
    while IFS= read -r module || [ -n "$module" ]; do
        case "$module" in
            ''|'#'*) continue ;;
        esac
        load_one "$module" || failed=1
    done < "$MODULE_DIR/modules.load.native"
    log_watchdog_state
    [ "$failed" -eq 0 ]
}

driver_name() {
    device="$1"
    link="$SYSFS_ROOT/bus/platform/devices/$device/driver"
    [ -L "$link" ] || return 1
    bb basename "$(bb readlink "$link" 2>/dev/null)"
}

wait_driver() {
    device="$1"
    expected="$2"
    timeout="$3"
    count=0
    while [ "$count" -lt "$timeout" ]; do
        observed="$(driver_name "$device" 2>/dev/null || true)"
        if [ "$observed" = "$expected" ]; then
            kmsg "provider ready: $device -> $expected"
            return 0
        fi
        count=$((count + 1))
        bb sleep 1
    done
    observed="$(driver_name "$device" 2>/dev/null || true)"
    kmsg "provider timeout: $device expected $expected observed ${observed:-unbound}"
    return 1
}

regulator_present() {
    wanted="$1"
    for name_file in "$SYSFS_ROOT"/class/regulator/*/name; do
        [ -r "$name_file" ] || continue
        observed="$(bb cat "$name_file" 2>/dev/null)"
        [ "$observed" = "$wanted" ] && return 0
    done
    return 1
}

wait_regulator_name() {
    wanted="$1"
    timeout="$2"
    count=0
    while [ "$count" -lt "$timeout" ]; do
        if regulator_present "$wanted"; then
            kmsg "regulator ready: $wanted"
            return 0
        fi
        count=$((count + 1))
        bb sleep 1
    done
    kmsg "regulator timeout: $wanted"
    return 1
}

wait_for_providers() {
    # Diagnostic build.  Probe every provider, log each result, and let select_root decide whether
    # storage is actually usable (the binding report that follows shows what else is or is not bound).
    _wp_fail=0
    kmsg "TB330FU DIAG: DVFSRC/DRAM modules are intentionally not loaded (the CN LK does not write the DRAM DT properties the ROW mtk_dramc probe needs); dvfsrc providers are not waited for"
    wait_driver 10000000.topckgen clk-mt6768 5 || _wp_fail=1
    wait_driver 10001000.infracfg_ao clk-mt6768 5 || _wp_fail=1
    wait_driver 10005000.pinctrl mt6768-pinctrl 5 || _wp_fail=1
    wait_driver mt6358-regulator mt6358-regulator 5 || _wp_fail=1
    wait_regulator_name vmc 5 || _wp_fail=1
    wait_regulator_name mt6358_vmch_eint_low 5 || _wp_fail=1
    [ "$_wp_fail" -eq 0 ] || kmsg "TB330FU DIAG: at least one provider wait failed (see lines above); continuing to storage discovery"
    return 0
}

wait_for_controller() {
    _wfc_controller="$1"
    count=0
    while [ "$count" -lt 20 ]; do
        if [ -L "$SYSFS_ROOT/bus/platform/devices/$_wfc_controller/driver" ]; then
            kmsg "MMC controller $_wfc_controller bound"
            return 0
        fi
        count=$((count + 1))
        bb sleep 1
    done
    kmsg "MMC controller $_wfc_controller did not bind"
    return 1
}

read_charger_boot_mode() {
    _boot_mode_bytes="$(bb hexdump -v -s 8 -n 4 \
        -e '4/1 "%u "' "$SYSFS_ROOT/firmware/devicetree/base/chosen/atag,boot" \
        2>/dev/null)" || return 1
    set -- $_boot_mode_bytes
    [ "$#" -eq 4 ] || return 1
    case "$1$2$3$4" in
        *[!0-9]*) return 1 ;;
    esac
    # The native AArch64 DT value is little-endian: byte 8 is the low byte.
    BOOT_MODE=$(($1 | ($2 << 8) | ($3 << 16) | ($4 << 24)))
    return 0
}

platform_driver_state() {
    device="$1"
    observed="$(driver_name "$device" 2>/dev/null || true)"
    kmsg "MMC state: $device driver=${observed:-unbound}"
}

log_mmc_failure_state() {
    platform_driver_state 11230000.mmc
    platform_driver_state 11240000.mmc
    [ -d "$SYSFS_ROOT/class/mmc_host/mmc0" ] && mmc0=present || mmc0=absent
    [ -d "$SYSFS_ROOT/class/mmc_host/mmc1" ] && mmc1=present || mmc1=absent
    kmsg "MMC hosts: mmc0=$mmc0 mmc1=$mmc1"
    blocks="$(bb ls "$SYSFS_ROOT/class/block" 2>/dev/null | bb tr '\n' ' ')"
    kmsg "block devices: ${blocks:-none}"
}

create_block_node() {
    _cbn_sysnode="$1"
    _cbn_name="$(bb basename "$_cbn_sysnode")"
    _cbn_devno="$(bb cat "$_cbn_sysnode/dev" 2>/dev/null)"
    _cbn_major="${_cbn_devno%:*}"
    _cbn_minor="${_cbn_devno#*:}"
    case "$_cbn_major" in
        ''|*[!0-9]*) return 1 ;;
    esac
    case "$_cbn_minor" in
        ''|*[!0-9]*) return 1 ;;
    esac
    bb mknod "$DEV_ROOT/$_cbn_name" b "$_cbn_major" "$_cbn_minor" 2>/dev/null || \
        [ -b "$DEV_ROOT/$_cbn_name" ] || return 1
    bb chmod 600 "$DEV_ROOT/$_cbn_name" 2>/dev/null || true
    kmsg "created $DEV_ROOT/$_cbn_name ($_cbn_major:$_cbn_minor)"
    return 0
}

create_char_node() {
    _ccn_sysnode="$1"
    _ccn_node="$2"
    _ccn_parent="$3"
    _ccn_mode="$4"
    _ccn_devno="$(bb cat "$_ccn_sysnode/dev" 2>/dev/null)" || return 1
    _ccn_major="${_ccn_devno%:*}"
    _ccn_minor="${_ccn_devno#*:}"
    case "$_ccn_major" in
        ''|*[!0-9]*) return 1 ;;
    esac
    case "$_ccn_minor" in
        ''|*[!0-9]*) return 1 ;;
    esac
    bb mkdir -p "$_ccn_parent" || return 1
    bb mknod "$_ccn_node" c "$_ccn_major" "$_ccn_minor" 2>/dev/null || \
        [ -c "$_ccn_node" ] || return 1
    bb chmod "$_ccn_mode" "$_ccn_node" 2>/dev/null || return 1
    kmsg "character node $_ccn_node ($_ccn_major:$_ccn_minor) mode=$_ccn_mode"
    return 0
}

wait_for_host() {
    _wfh_controller="$1"
    _wfh_role="$2"
    _wfh_count=0
    while [ "$_wfh_count" -lt "$MMC_DISCOVERY_TIMEOUT" ]; do
        for _wfh_sysnode in \
            "$SYSFS_ROOT/bus/platform/devices/$_wfh_controller/mmc_host"/mmc*; do
            [ -d "$_wfh_sysnode" ] || continue
            DISCOVERED_MMC_HOST="$(bb basename "$_wfh_sysnode")"
            kmsg "$_wfh_role MMC host: $DISCOVERED_MMC_HOST"
            return 0
        done
        _wfh_count=$((_wfh_count + 1))
        bb sleep 1
    done
    kmsg "$_wfh_role host not found for $_wfh_controller"
    return 1
}

wait_for_disk() {
    _wfd_controller="$1"
    _wfd_host="$2"
    _wfd_role="$3"
    _wfd_count=0
    while [ "$_wfd_count" -lt "$MMC_DISCOVERY_TIMEOUT" ]; do
        for _wfd_sysnode in \
            "$SYSFS_ROOT/bus/platform/devices/$_wfd_controller/mmc_host/$_wfd_host"/"$_wfd_host":*/block/mmcblk*; do
            [ -r "$_wfd_sysnode/dev" ] || continue
            DISCOVERED_MMC_DISK="$(bb basename "$_wfd_sysnode")"
            kmsg "$_wfd_role MMC disk: $DISCOVERED_MMC_DISK"
            return 0
        done
        _wfd_count=$((_wfd_count + 1))
        bb sleep 1
    done
    kmsg "disk not found for $_wfd_role host $_wfd_host"
    return 1
}

wait_for_partition() {
    _wfp_disk="$1"
    _wfp_number="$2"
    _wfp_role="$3"
    _wfp_partition="${_wfp_disk}p$_wfp_number"
    _wfp_count=0
    while [ "$_wfp_count" -lt "$MMC_DISCOVERY_TIMEOUT" ]; do
        if [ -r "$SYSFS_ROOT/class/block/$_wfp_partition/dev" ]; then
            DISCOVERED_MMC_PARTITION="$_wfp_partition"
            return 0
        fi
        _wfp_count=$((_wfp_count + 1))
        bb sleep 1
    done
    kmsg "partition $_wfp_number not found for $_wfp_role disk $_wfp_disk"
    return 1
}

discover_controller_root() {
    _dcr_controller="$1"
    _dcr_partition_number="$2"
    _dcr_role="$3"
    wait_for_controller "$_dcr_controller" || return 1
    wait_for_host "$_dcr_controller" "$_dcr_role" || return 1
    _dcr_host="$DISCOVERED_MMC_HOST"
    wait_for_disk "$_dcr_controller" "$_dcr_host" "$_dcr_role" || return 1
    _dcr_disk="$DISCOVERED_MMC_DISK"
    if ! create_block_node "$SYSFS_ROOT/class/block/$_dcr_disk"; then
        kmsg "mknod failed for $_dcr_role disk $_dcr_disk"
        return 1
    fi
    wait_for_partition "$_dcr_disk" "$_dcr_partition_number" "$_dcr_role" || return 1
    _dcr_partition="$DISCOVERED_MMC_PARTITION"
    if ! create_block_node "$SYSFS_ROOT/class/block/$_dcr_partition"; then
        kmsg "mknod failed for $_dcr_role partition $_dcr_partition"
        return 1
    fi
    ROOT_NODE="$DEV_ROOT/$_dcr_partition"
    return 0
}

select_root() {
    case "$ROOT_SOURCE" in
        internal) _sr_controller="$INTERNAL_MMC_CONTROLLER"; _sr_role="internal eMMC" ;;
        external|sd) _sr_controller="$SD_MMC_CONTROLLER"; _sr_role="external SD" ;;
        *) kmsg "unsupported ROOT_SOURCE=$ROOT_SOURCE"; return 1 ;;
    esac
    case "$ROOT_SELECTOR" in
        LABEL=*) _sr_key=LABEL; _sr_wanted="${ROOT_SELECTOR#LABEL=}" ;;
        UUID=*) _sr_key=UUID; _sr_wanted="${ROOT_SELECTOR#UUID=}" ;;
        *) kmsg "unsupported root selector $ROOT_SELECTOR"; return 1 ;;
    esac
    if [ "$ROOT_SOURCE" = internal ] && [ -z "$ROOT_UUID" ]; then
        kmsg "internal eMMC root requires an explicit ROOT_UUID; refusing discovery"
        return 1
    fi
    if [ "$ROOT_SOURCE" = internal ]; then
        [ "$ROOT_SELECTOR" = LABEL=TB330FU_KDE ] || {
            kmsg "internal eMMC profile only accepts LABEL=TB330FU_KDE"
            return 1
        }
        case "$ROOTFS_MIN_SECTORS" in ''|*[!0-9]*)
            kmsg "internal eMMC root requires numeric ROOTFS_MIN_SECTORS"
            return 1 ;;
        esac
        [ "$ROOTFS_MIN_SECTORS" -gt 0 ] || {
            kmsg "internal eMMC root requires a nonzero capacity limit"
            return 1
        }
    fi
    _sr_controller_path="$(bb readlink -f "$SYSFS_ROOT/bus/platform/devices/$_sr_controller" 2>/dev/null || true)"
    if [ -z "$_sr_controller_path" ] || [ ! -d "$_sr_controller_path" ]; then
        kmsg "MMC controller path unavailable: $_sr_controller"
        return 1
    fi
    wait_for_controller "$_sr_controller" || return 1

    _sr_elapsed=0
    while [ "$_sr_elapsed" -lt "$MMC_DISCOVERY_TIMEOUT" ]; do
        _sr_valid_count=0
        _sr_valid_node=
        for _sr_node in "$SYSFS_ROOT"/class/block/*; do
            [ -r "$_sr_node/dev" ] || continue
            # Only partition nodes under the explicitly selected physical MMC
            # controller are eligible. Never infer or hard-code pN.
            [ -r "$_sr_node/partition" ] || continue
            _sr_path="$(bb readlink -f "$_sr_node" 2>/dev/null || true)"
            case "$_sr_path" in "$_sr_controller_path"/*) ;; *) continue ;; esac
            _sr_name="$(bb basename "$_sr_node")"
            if ! create_block_node "$_sr_node"; then
                kmsg "$_sr_role candidate $_sr_name could not be made in $DEV_ROOT"
                continue
            fi
            _sr_meta="$(bb blkid -p "$DEV_ROOT/$_sr_name" 2>/dev/null || true)"
            _sr_label="$(printf '%s\n' "$_sr_meta" | bb sed -n 's/.*LABEL="\([^"]*\)".*/\1/p')"
            [ "$_sr_label" = "$_sr_wanted" ] || continue
            _sr_uuid="$(printf '%s\n' "$_sr_meta" | bb sed -n 's/.*UUID="\([^"]*\)".*/\1/p')"
            _sr_type="$(printf '%s\n' "$_sr_meta" | bb sed -n 's/.*TYPE="\([^"]*\)".*/\1/p')"
            if [ -n "$ROOT_UUID" ] && [ "$_sr_uuid" != "$ROOT_UUID" ]; then
                kmsg "$_sr_role label match $_sr_name rejected: UUID=${_sr_uuid:-missing}, expected $ROOT_UUID"
                continue
            fi
            if [ "$_sr_type" != ext4 ]; then
                kmsg "$_sr_role label match $_sr_name rejected: filesystem=${_sr_type:-unknown}, expected ext4"
                continue
            fi
            _sr_sectors="$(bb cat "$_sr_node/size" 2>/dev/null || true)"
            case "$_sr_sectors" in ''|*[!0-9]*)
                kmsg "$_sr_role label match $_sr_name rejected: invalid sysfs capacity"
                continue ;;
            esac
            if [ "$_sr_sectors" -lt "$ROOTFS_MIN_SECTORS" ]; then
                kmsg "$_sr_role label match $_sr_name rejected: size=${_sr_sectors} sectors, need ${ROOTFS_MIN_SECTORS}"
                continue
            fi
            _sr_valid_count=$((_sr_valid_count + 1))
            _sr_valid_node="$DEV_ROOT/$_sr_name"
            kmsg "validated $_sr_role candidate $_sr_name label=$_sr_label uuid=$_sr_uuid type=$_sr_type size=${_sr_sectors} sectors"
        done
        if [ "$_sr_valid_count" -eq 1 ]; then
            ROOT_NODE="$_sr_valid_node"
            kmsg "selected $_sr_role root $ROOT_NODE via $ROOT_SELECTOR UUID=${ROOT_UUID:-any}"
            return 0
        fi
        if [ "$_sr_valid_count" -gt 1 ]; then
            kmsg "ambiguous $_sr_role root: $_sr_valid_count partitions match label, UUID, type, and capacity"
            return 1
        fi
        _sr_elapsed=$((_sr_elapsed + 1))
        bb sleep 1
    done
    kmsg "timed out after ${MMC_DISCOVERY_TIMEOUT}s finding $_sr_role root $ROOT_SELECTOR UUID=${ROOT_UUID:-any} type=ext4"
    return 1
}

mount_selected_root() {
    _msr_options="$1"
    kmsg "mounting $_msr_options ext4 root $ROOT_NODE source=${ROOT_SOURCE:-unknown} selector=$ROOT_SELECTOR uuid=${ROOT_UUID:-any}"
    if bb mount -t ext4 -o "$_msr_options" "$ROOT_NODE" /newroot 2>/dev/kmsg; then
        return 0
    fi
    kmsg "ext4 mount failed for $ROOT_NODE with options $_msr_options"
    return 1
}

kmsg_chunks() {
    # stdin: one item per line.  Writes "<prefix> item item ..." with <per> items per kernel-log record.
    _kc_prefix="$1"
    _kc_per="${2:-6}"
    bb awk -v p="$_kc_prefix" -v per="$_kc_per" '
        { line = line " " $0; if (++n % per == 0) { print p line; line = "" } }
        END { if (line != "") print p line }' |
    while IFS= read -r _kc_line; do
        kmsg "$_kc_line"
    done
}

report_bindings() {
    # Diagnostics only.  Never fails, never changes system state apart from mounting debugfs.
    _rb_tag="$1"
    _rb_tmp="$DEV_ROOT/.rb.$$"
    kmsg "BIND[$_rb_tag] begin, uptime $(bb cut -d' ' -f1 /proc/uptime 2>/dev/null)"
    [ -d "$SYSFS_ROOT/kernel/debug" ] && bb mount -t debugfs debugfs "$SYSFS_ROOT/kernel/debug" 2>/dev/null
    if [ -r "$SYSFS_ROOT/kernel/debug/devices_deferred" ]; then
        bb head -n 60 "$SYSFS_ROOT/kernel/debug/devices_deferred" 2>/dev/null | bb tr '\t' ' ' |
        while IFS= read -r _rb_line; do
            [ -n "$_rb_line" ] && kmsg "BIND[$_rb_tag] deferred: $_rb_line"
        done
        kmsg "BIND[$_rb_tag] deferred total: $(bb wc -l < "$SYSFS_ROOT/kernel/debug/devices_deferred" 2>/dev/null)"
    else
        kmsg "BIND[$_rb_tag] devices_deferred not readable"
    fi
    bb find "$SYSFS_ROOT/bus/platform/drivers" -maxdepth 2 -type l 2>/dev/null |
        bb awk -F/ '$NF != "module" { print $NF "=" $(NF-1) }' | bb sort > "$_rb_tmp.bound"
    bb ls "$SYSFS_ROOT/bus/platform/devices" 2>/dev/null | bb sort > "$_rb_tmp.all"
    kmsg "BIND[$_rb_tag] platform devices: $(bb wc -l < "$_rb_tmp.all") total, $(bb wc -l < "$_rb_tmp.bound") bound; modules: $(bb wc -l < /proc/modules 2>/dev/null)"
    if [ "$BIND_LISTS" = 1 ]; then
        kmsg_chunks "BIND[$_rb_tag] bound:" 5 < "$_rb_tmp.bound"
        bb awk -F= 'NR == FNR { b[$1] = 1; next } !($1 in b)' "$_rb_tmp.bound" "$_rb_tmp.all" |
            kmsg_chunks "BIND[$_rb_tag] unbound:" 8
    fi
    for _rb_d in "$SYSFS_ROOT"/class/regulator/regulator.*; do
        [ -r "$_rb_d/name" ] || continue
        printf '%s=%s/%s\n' "$(bb cat "$_rb_d/name" 2>/dev/null)" "$(bb cat "$_rb_d/state" 2>/dev/null)" "$(bb cat "$_rb_d/num_users" 2>/dev/null)"
    done 2>/dev/null | kmsg_chunks "BIND[$_rb_tag] regulators(name=state/users):" 5
    bb awk '{ s = 0; for (i = 2; i <= NF; i++) { if ($i ~ /^[0-9]+$/) s += $i; else break } if (s > 5000) print $1 s ":" $NF }' /proc/interrupts 2>/dev/null |
        kmsg_chunks "BIND[$_rb_tag] irq>5000:" 6
    kmsg "BIND[$_rb_tag] mmc_host: $(bb ls "$SYSFS_ROOT/class/mmc_host" 2>/dev/null | bb tr '\n' ' ')"
    kmsg "BIND[$_rb_tag] block: $(bb ls "$SYSFS_ROOT/class/block" 2>/dev/null | bb tr '\n' ' ')"
    kmsg "BIND[$_rb_tag] end"
    bb rm -f "$_rb_tmp.bound" "$_rb_tmp.all"
    return 0
}

start_deadman() {
    [ "$DEADMAN_SECONDS" -gt 0 ] 2>/dev/null || return 0
    (
        bb sleep "$DEADMAN_SECONDS"
        kmsg "TB330FU DIAG: deadman ${DEADMAN_SECONDS}s expired, stage1 is stuck; blocked-task backtraces follow, then sysrq-c"
        echo w > "$SYSRQ_TRIGGER" 2>/dev/null || true
        bb sleep 2
        echo c > "$SYSRQ_TRIGGER" 2>/dev/null || true
        bb sleep 3
        bb reboot -f 2>/dev/null || true
    ) &
    DEADMAN_PID=$!
    kmsg "TB330FU DIAG: deadman armed for ${DEADMAN_SECONDS}s (pid $DEADMAN_PID)"
}

stop_deadman() {
    [ -n "${DEADMAN_PID:-}" ] || return 0
    kill "$DEADMAN_PID" 2>/dev/null || true
    DEADMAN_PID=
    kmsg "TB330FU DIAG: deadman disarmed"
}

hold_pid1() {
    reason="$1"
    stop_deadman
    kmsg "$reason; PID 1 stays alive for ${HOLD_SECONDS}s, then forces a panic so this log survives in last_kmsg"
    report_bindings "final"
    bb sync 2>/dev/null || true
    _hp_left="$HOLD_SECONDS"
    while [ "$_hp_left" -gt 0 ]; do
        bb sleep 1
        _hp_left=$((_hp_left - 1))
    done
    kmsg "TB330FU DIAG: intentional sysrq-c now (diagnostic build)"
    echo c > "$SYSRQ_TRIGGER" 2>/dev/null || true
    bb sleep 3
    kmsg "TB330FU DIAG: sysrq-c did not take effect; trying reboot -f"
    bb reboot -f 2>/dev/null || true
    while true; do
        bb sleep 10
    done
}

charger_boot_usable() {
    # $1 = the mounted (ro) Ubuntu root.  Returns 0 when the charger program can run, 1 when this boot must continue as a
    # normal Ubuntu boot instead (CHARGER_FALLBACK=normal and the rootfs has no charger binary).  With CHARGER_FALLBACK=hold
    # a missing binary stops stage1 (hold, then planned sysrq-c).
    if [ -x "$1/usr/libexec/tb330fu-charger" ]; then
        return 0
    fi
    if [ "$CHARGER_FALLBACK" = normal ]; then
        kmsg "TB330FU DIAG: charger boot but the rootfs has no executable /usr/libexec/tb330fu-charger; continuing as a normal Ubuntu boot (CHARGER_FALLBACK=normal)"
        return 1
    fi
    hold_pid1 "charger binary missing or not executable"
}

load_harvest_timer() {
    # Diagnostic build: tb330fu_harvest.ko is a kernel timer, so it survives switch_root.  It logs a heartbeat and panics on
    # purpose HARVEST_SECONDS after loading so that the whole Ubuntu boot log reaches expdb.  HARVEST_SECONDS=0 disables it.
    if ! [ "$HARVEST_SECONDS" -gt 0 ] 2>/dev/null; then
        kmsg "TB330FU DIAG: harvest timer disabled (HARVEST_SECONDS=$HARVEST_SECONDS)"
        return 0
    fi
    if [ ! -f "$MODULE_DIR/tb330fu_harvest.ko" ]; then
        kmsg "TB330FU DIAG: tb330fu_harvest.ko is not in the ramdisk"
        return 0
    fi
    kmsg "loading tb330fu_harvest (planned panic ${HARVEST_SECONDS}s after load)"
    if bb insmod "$MODULE_DIR/tb330fu_harvest.ko" panic_after="$HARVEST_SECONDS" 2>/dev/kmsg; then
        kmsg "loaded tb330fu_harvest.ko"
    else
        kmsg "TB330FU DIAG: tb330fu_harvest.ko FAILED to load; the planned panic will not happen"
    fi
    return 0
}

install_runtime_overlay() {
    # RAM-only additions to the Ubuntu root; NOTHING is written to the ext4 root.  A tmpfs on /newroot/run (systemd keeps an already mounted
    # /run, like it does after a systemd initrd) carries two runtime units; the helper script and the OTG modules live in the /dev tmpfs
    # (/dev/.tb330fu) that is moved into the new root.
    #   DIRECT_SESSION=1  sddm.service is masked (runtime mask) and tb330fu-session.service starts the autologin Plasma Wayland session of the
    #                     user "ubuntu" itself.  Why: SDDM always starts an Xorg for its greeter; this kernel has no VTs, so that Xorg stays DRM
    #                     master for ever and KWin cannot get the display (observed: the Wayland autologin session died, SDDM fell back to the
    #                     greeter) - and the greeter needs a password the account does not have (key-only account, no keyboard anyway).
    #   OTG_SET=1         tb330fu-otg.service loads the vendor Type-C / charger / role-switch modules late (after the desktop is up) and logs what
    #                     they find; it never forces a role by hand.
    if [ "$DIRECT_SESSION" != 1 ] && [ "$OTG_SET" != 1 ]; then
        kmsg "TB330FU DIAG: runtime overlay disabled (DIRECT_SESSION=$DIRECT_SESSION OTG_SET=$OTG_SET)"
        return 0
    fi
    if [ ! -d /newroot/run ]; then
        kmsg "TB330FU DIAG: /newroot/run is missing, no runtime overlay"
        return 0
    fi
    if ! bb mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /newroot/run; then
        kmsg "TB330FU DIAG: could not mount a tmpfs on /newroot/run, no runtime overlay"
        return 0
    fi
    _iro_u=/newroot/run/systemd/system
    bb mkdir -p "$_iro_u/graphical.target.wants" "$_iro_u/multi-user.target.wants" /dev/.tb330fu/mods

    if [ "$DIRECT_SESSION" = 1 ]; then
        if [ -x /newroot/etc/sddm/wayland-session ] && [ -x /newroot/usr/bin/startplasma-wayland ] && \
           [ -x /newroot/usr/lib/aarch64-linux-gnu/libexec/plasma-dbus-run-session-if-needed ] && bb grep -q '^ubuntu:' /newroot/etc/passwd; then
            bb ln -s /dev/null "$_iro_u/sddm.service"
            bb cat > "$_iro_u/tb330fu-session.service" <<'TB330FU_SESSION_UNIT'
[Unit]
Description=TB330FU Plasma Wayland session (autologin ubuntu, no display manager; installed in RAM by stage1)
After=systemd-user-sessions.service systemd-logind.service systemd-udev-settle.service tb330fu-devd.service dbus.service
Wants=systemd-logind.service systemd-udev-settle.service
StartLimitIntervalSec=300
StartLimitBurst=3

[Service]
Type=simple
User=ubuntu
PAMName=sddm-autologin
WorkingDirectory=/home/ubuntu
StandardInput=null
Environment=LANG=C.UTF-8
Environment=XDG_SESSION_TYPE=wayland
Environment=XDG_SESSION_CLASS=user
Environment=XDG_SESSION_DESKTOP=KDE
Environment=XDG_CURRENT_DESKTOP=KDE
Environment=DESKTOP_SESSION=plasmawayland
Environment=XDG_SEAT=seat0
Environment=KWIN_DRM_DEVICES=/dev/dri/card0
ExecStart=/etc/sddm/wayland-session /usr/lib/aarch64-linux-gnu/libexec/plasma-dbus-run-session-if-needed /usr/bin/startplasma-wayland
Restart=on-failure
RestartSec=10
TimeoutStopSec=15

[Install]
WantedBy=graphical.target
TB330FU_SESSION_UNIT
            bb ln -s ../tb330fu-session.service "$_iro_u/graphical.target.wants/tb330fu-session.service"
            kmsg "TB330FU DIAG: direct Plasma Wayland session installed in RAM (sddm.service masked); no SDDM greeter, no password"
        else
            kmsg "TB330FU DIAG: direct session prerequisites missing (sddm wayland-session / startplasma-wayland / user ubuntu); SDDM stays as it is"
        fi
    fi

    if [ "$OTG_SET" = 1 ]; then
        _iro_n=0
        : > /dev/.tb330fu/otg.list
        for _iro_m in $OTG_MODULES; do
            if [ -f "$MODULE_DIR/$_iro_m.ko" ] && bb cp "$MODULE_DIR/$_iro_m.ko" "/dev/.tb330fu/mods/$_iro_m.ko"; then
                echo "$_iro_m" >> /dev/.tb330fu/otg.list
                _iro_n=$((_iro_n + 1))
            else
                kmsg "TB330FU DIAG: OTG module $_iro_m.ko is not in the ramdisk"
            fi
        done
        bb cat > /dev/.tb330fu/otg.sh <<'TB330FU_OTG_SCRIPT'
#!/bin/sh
# TB330FU late USB OTG bring-up (written by stage1, lives in RAM).  Loads the vendor Type-C / charger / role-switch modules in the stock
# first-stage order (minus the charging framework) and logs what they find.  It does not force any role: the TCPC state machine, rt_pd_manager
# and extcon-mtk-usb switch the role and VBUS when a cable is plugged in.
M=/dev/.tb330fu/mods
K() { echo "TB330FU OTG: $*" > /dev/kmsg; }
INSMOD="$(command -v insmod || echo /usr/sbin/insmod)"
snap() {
    K "---- $1"
    K "i2c adapters: $(for a in /sys/class/i2c-adapter/i2c-*; do [ -e "$a" ] || continue; printf '%s=%s ' "$(basename "$a")" "$(cat "$a/name" 2>/dev/null)"; done)"
    for d in /sys/bus/i2c/devices/*-*; do
        [ -e "$d" ] || continue
        drv="$(readlink "$d/driver" 2>/dev/null)"
        K "i2c device $(basename "$d") name=$(cat "$d/name" 2>/dev/null) driver=${drv##*/}"
    done
    K "tcpc class: $(ls /sys/class/tcpc 2>/dev/null | tr '\n' ' ')"
    for r in /sys/class/usb_role/*; do [ -e "$r" ] && K "usb_role $(basename "$r") role=$(cat "$r/role" 2>/dev/null)"; done
    K "otg/vbus regulators: $(for r in /sys/class/regulator/regulator.*; do n="$(cat "$r/name" 2>/dev/null)"; case "$n" in *otg*|*OTG*|*vbus*|*VBUS*) printf '%s=%s ' "$n" "$(cat "$r/state" 2>/dev/null)";; esac; done)"
    K "power supplies: $(ls /sys/class/power_supply 2>/dev/null | tr '\n' ' ')"
    K "usb devices: $(ls /sys/bus/usb/devices 2>/dev/null | tr '\n' ' ')"
}
D=/var/lib/tb330fu
mkdir -p "$D" 2>/dev/null
if [ -e "$D/no-otg" ]; then
    K "skipped: $D/no-otg exists (remove it to enable the OTG module chain again)"
    exit 0
fi
if [ -e "$D/otg-started" ]; then
    K "the previous boot did not stay up for 2 minutes after loading the OTG chain (marker $D/otg-started): skipping the chain this time, the next boot tries again"
    rm -f "$D/otg-started"
    sync
    exit 0
fi
sleep "${OTG_DELAY:-30}"
snap "before loading"
: > "$D/otg-started"
sync
while read -r m; do
    [ -n "$m" ] || continue
    out="$(timeout 60 "$INSMOD" "$M/$m.ko" 2>&1)"
    rc=$?
    K "insmod $m.ko rc=$rc${out:+ ($out)}"
    sleep 1
done < /dev/.tb330fu/otg.list
sleep 8
snap "after loading"
# keep watching for 10 minutes so that a dock plugged in later shows up in the persistent log with the role and the USB device list
n=0
while [ "$n" -lt 40 ]; do
    sleep 15
    n=$((n + 1))
    if [ "$n" -eq 8 ]; then
        rm -f "$D/otg-started"
        sync
        K "the OTG module chain has been alive for 2 minutes, crash marker removed"
    fi
    K "watch $n: roles=$(for r in /sys/class/usb_role/*; do printf '%s ' "$(cat "$r/role" 2>/dev/null)"; done) usb=$(ls /sys/bus/usb/devices 2>/dev/null | tr '\n' ' ')"
done
snap "after 10 minutes"
TB330FU_OTG_SCRIPT
        bb chmod 755 /dev/.tb330fu/otg.sh
        bb cat > "$_iro_u/tb330fu-otg.service" <<'TB330FU_OTG_UNIT'
[Unit]
Description=TB330FU USB OTG host bring-up (vendor Type-C / charger / role-switch modules, loaded late; installed in RAM by stage1)
After=tb330fu-devd.service systemd-udev-settle.service tb330fu-session.service

[Service]
Type=simple
ExecStart=/bin/sh /dev/.tb330fu/otg.sh
Nice=5

[Install]
WantedBy=multi-user.target
TB330FU_OTG_UNIT
        bb ln -s ../tb330fu-otg.service "$_iro_u/multi-user.target.wants/tb330fu-otg.service"
        kmsg "TB330FU DIAG: late OTG bring-up installed in RAM ($_iro_n modules staged in /dev/.tb330fu/mods)"
    fi
    return 0
}

load_display_modules() {
    # The boot display: tb330fu_ovl re-aims LK's overlay at the framebuffer region, tb330fu_con draws the kernel log and Tux there, tb330fu_fb
    # registers the region as simple-framebuffer and simpledrm turns it into /dev/dri/card0 (on the first plane update of a DRM client it
    # tells the console to stop).  Loaded at the very start of stage1 so that the whole boot is visible.  Every failure is only logged.
    if [ "$DISPLAY_MODULES" != 1 ]; then
        kmsg "TB330FU DIAG: boot display modules disabled (DISPLAY_MODULES=$DISPLAY_MODULES)"
        return 0
    fi
    if [ ! -f "$MODULE_DIR/modules.load.display" ]; then
        kmsg "TB330FU DIAG: modules.load.display is not in the ramdisk"
        return 0
    fi
    kmsg "loading boot display module set (modules.load.display)"
    _ldm_failed=0
    while IFS= read -r module || [ -n "$module" ]; do
        case "$module" in
            ''|'#'*) continue ;;
        esac
        load_one "$module" || _ldm_failed=1
    done < "$MODULE_DIR/modules.load.display"
    [ "$_ldm_failed" -eq 0 ] || kmsg "TB330FU DIAG: one or more boot display modules failed to load (see the FAILED lines above)"
    return 0
}

load_extra_modules() {
    # Display (simpledrm over the framebuffer LK already scans out) and touch (Himax over SPI).  The list is in dependency order
    # (checked offline with a loader simulation against the gda export table).  Every failure is logged and skipped; the only way
    # this can stop the boot is a kernel oops inside one of the modules, which mrdump turns into a log in expdb.
    if [ "$EXTRA_MODULES" != 1 ]; then
        kmsg "TB330FU DIAG: display/touch modules disabled (EXTRA_MODULES=$EXTRA_MODULES)"
        return 0
    fi
    if [ ! -f "$MODULE_DIR/modules.load.extra" ]; then
        kmsg "TB330FU DIAG: modules.load.extra is not in the ramdisk"
        return 0
    fi
    kmsg "loading display/touch module set (modules.load.extra)"
    _lem_failed=0
    while IFS= read -r module || [ -n "$module" ]; do
        case "$module" in
            ''|'#'*) continue ;;
        esac
        load_one "$module" || _lem_failed=1
    done < "$MODULE_DIR/modules.load.extra"
    [ "$_lem_failed" -eq 0 ] || kmsg "TB330FU DIAG: one or more display/touch modules failed to load (see the FAILED lines above)"
    return 0
}

find_expdb_devt() {
    # Prints MAJOR:MINOR of the expdb partition (GPT name "expdb"); nothing if it cannot be identified.  Fallback: mmcblk0p6 when it is
    # exactly 40960 sectors (20 MiB), which is the expdb entry of this tablet's GPT.  tb330fu_blklog additionally insists on the expdb
    # magic at the start of the partition before it writes anything.
    _fe_found=""
    for _fe_dir in "$SYSFS_ROOT"/class/block/mmcblk0p*; do
        [ -r "$_fe_dir/dev" ] || continue
        _fe_name=""
        if [ -r "$_fe_dir/uevent" ]; then
            while IFS= read -r _fe_line || [ -n "$_fe_line" ]; do
                case "$_fe_line" in
                    PARTNAME=*) _fe_name="${_fe_line#PARTNAME=}" ;;
                esac
            done < "$_fe_dir/uevent"
        fi
        if [ "$_fe_name" = "expdb" ]; then
            read -r _fe_found < "$_fe_dir/dev"
            break
        fi
    done
    if [ -z "$_fe_found" ] && [ -r "$SYSFS_ROOT/class/block/mmcblk0p6/size" ] && [ -r "$SYSFS_ROOT/class/block/mmcblk0p6/dev" ]; then
        read -r _fe_size < "$SYSFS_ROOT/class/block/mmcblk0p6/size"
        if [ "$_fe_size" = 40960 ]; then
            read -r _fe_found < "$SYSFS_ROOT/class/block/mmcblk0p6/dev"
        fi
    fi
    [ -n "$_fe_found" ] || return 1
    echo "$_fe_found"
}

load_blklog() {
    # Persistent kernel-log snapshots in an unused window of the expdb partition (tb330fu_blklog.ko), so that the log survives any power-off.
    # Non-fatal: every problem is only logged.
    if [ "$BLKLOG" != 1 ]; then
        kmsg "TB330FU DIAG: persistent log disabled (BLKLOG=$BLKLOG)"
        return 0
    fi
    if [ ! -f "$MODULE_DIR/tb330fu_blklog.ko" ]; then
        kmsg "TB330FU DIAG: tb330fu_blklog.ko is not in the ramdisk"
        return 0
    fi
    _bl_devt="$(find_expdb_devt)"
    if [ -z "$_bl_devt" ]; then
        kmsg "TB330FU DIAG: the expdb partition was not found (no PARTNAME=expdb, mmcblk0p6 is not 40960 sectors); no persistent log"
        return 0
    fi
    kmsg "loading tb330fu_blklog (expdb partition $_bl_devt)"
    if bb insmod "$MODULE_DIR/tb330fu_blklog.ko" devt="$_bl_devt" 2>/dev/kmsg; then
        BLKLOG_OK=1
        kmsg "loaded tb330fu_blklog.ko"
    else
        kmsg "TB330FU DIAG: tb330fu_blklog.ko FAILED to load (it refuses an unsuitable partition / window); no persistent log"
    fi
    return 0
}

harvest_fallback() {
    # Without the persistent log (module missing, partition not found, or tb330fu_blklog refused the window) the planned-panic harvest is the
    # only way to get the boot log out, so it is armed after HARVEST_FALLBACK_SECONDS unless a harvest time was asked for explicitly.
    if [ "$BLKLOG_OK" != 1 ] && ! [ "$HARVEST_SECONDS" -gt 0 ] 2>/dev/null; then
        HARVEST_SECONDS="$HARVEST_FALLBACK_SECONDS"
        kmsg "TB330FU DIAG: no persistent log, arming the planned-panic fallback (${HARVEST_SECONDS}s)"
    fi
    return 0
}

kmsg "TB330FU EARLY: init started"
mount_early || hold_pid1 "required pseudo-filesystem or bootstrap node setup failed"
kmsg "TB330FU EARLY: pseudo-filesystems ready"
kmsg "TB330FU DIAG: stage1 @STAGE1_ID@ (internal-eMMC selector, busybox switch_root, binding report, deadman ${DEADMAN_SECONDS}s, bounded hold then sysrq-c, no DVFSRC/DRAM modules, INT_POL helper, no i2c, charger fallback=${CHARGER_FALLBACK}, harvest ${HARVEST_SECONDS}s, boot display ${DISPLAY_MODULES}, touch modules ${EXTRA_MODULES}, direct session ${DIRECT_SESSION}, OTG set ${OTG_SET}, persistent log ${BLKLOG}, /dev/null created before any redirection)"
start_deadman
load_display_modules

CHARGER_BOOT=0
if read_charger_boot_mode; then
    kmsg "TB330FU EARLY: atag boot mode=0x$(printf '%x' "$BOOT_MODE")"
    [ "$BOOT_MODE" -eq 8 ] && CHARGER_BOOT=1
else
    kmsg "TB330FU EARLY: atag boot mode unavailable; using normal boot"
fi

load_intpol_helper
load_stock_early_modules || kmsg "TB330FU DIAG: one or more early modules failed to load; continuing so the log shows how far storage gets"
kmsg "TB330FU EARLY: local module closure loaded"
report_bindings "after-modules"
[ "$PROFILE" != debug ] || { log_mmc_failure_state; hold_pid1 "Minimal Debug reached storage diagnostics"; }
wait_for_providers
report_bindings "after-providers"
select_root || { log_mmc_failure_state; hold_pid1 "configured RootFS discovery failure"; }
load_blklog

if [ "$CHARGER_BOOT" -eq 1 ]; then
    bb mkdir -p /newroot
    kmsg "TB330FU CHARGER: mounting Ubuntu root $ROOT_NODE ro"
    if ! mount_selected_root ro; then
        hold_pid1 "charger ext4 root mount failed"
    fi
    if ! charger_boot_usable /newroot; then
        bb umount /newroot || hold_pid1 "could not unmount the read-only charger mount for the normal-boot fallback"
        CHARGER_BOOT=0
    fi
fi

if [ "$CHARGER_BOOT" -eq 1 ]; then
    [ -d /newroot/proc ] && [ -d /newroot/sys ] && [ -d /newroot/dev ] || \
        hold_pid1 "charger root mountpoints missing"
    if ! create_char_node "$SYSFS_ROOT/class/drm/card0" \
        "$DEV_ROOT/dri/card0" "$DEV_ROOT/dri" 0600; then
        hold_pid1 "charger DRM node creation failed"
    fi
    if ! create_char_node "$SYSFS_ROOT/class/misc/pmsg0" \
        "$DEV_ROOT/pmsg0" "$DEV_ROOT" 0222; then
        kmsg "charger pmsg node creation failed; continuing without pmsg"
    fi
    kmsg "TB330FU CHARGER: switching root and execing charger"
    bb mount --move /proc /newroot/proc || hold_pid1 "failed to move /proc for charger"
    bb mount --move /sys /newroot/sys || hold_pid1 "failed to move /sys for charger"
    bb mount --move /dev /newroot/dev || hold_pid1 "failed to move /dev for charger"
    stop_deadman
    exec /bin/busybox switch_root /newroot /usr/libexec/tb330fu-charger
    hold_pid1 "charger switch_root returned unexpectedly"
fi

bb mkdir -p /newroot
kmsg "TB330FU EARLY: mounting Ubuntu root $ROOT_NODE rw"
if ! mount_selected_root rw; then
    hold_pid1 "ext4 root mount failed"
fi
kmsg "TB330FU EARLY: Ubuntu root mounted"
# RESCUE (one-shot, does nothing when the layout is fine): a real directory /lib instead of the merged-/usr symlink to usr/lib means that no dynamic program
# (systemd, bash, ...) can start: the ELF interpreter /lib/ld-linux-aarch64.so.1 is gone.  Move whatever is in /lib into /usr/lib and restore the symlink.
if [ -d /newroot/lib ] && [ ! -L /newroot/lib ]; then
    kmsg "TB330FU RESCUE: /lib is a directory (the merged-/usr symlink was replaced), moving its content to /usr/lib and restoring the symlink"
    bb mkdir -p /newroot/usr/lib
    if bb cp -a /newroot/lib/. /newroot/usr/lib/ && bb rm -rf /newroot/lib && bb ln -s usr/lib /newroot/lib; then
        bb sync
        kmsg "TB330FU RESCUE: /lib -> usr/lib restored"
    else
        kmsg "TB330FU RESCUE: the repair FAILED"
    fi
fi

[ -e /newroot/sbin/init ] || hold_pid1 "required init path missing: /newroot/sbin/init"
[ -x /newroot/sbin/init ] || hold_pid1 "/newroot/sbin/init is not executable"
kmsg "TB330FU EARLY: /newroot/sbin/init ready"
bb mkdir -p /newroot/proc /newroot/sys /newroot/dev
load_extra_modules
install_runtime_overlay
report_bindings "pre-switch"
harvest_fallback
load_harvest_timer
kmsg "TB330FU EARLY: switching root to Ubuntu"
bb mount --move /proc /newroot/proc || hold_pid1 "failed to move /proc"
bb mount --move /sys /newroot/sys || hold_pid1 "failed to move /sys"
bb mount --move /dev /newroot/dev || hold_pid1 "failed to move /dev"
stop_deadman
exec /bin/busybox switch_root /newroot /sbin/init

hold_pid1 "switch_root returned unexpectedly"
