#!/bin/bash
# Build the kernel modules of this project.
#
# usage: scripts/build-modules.sh [all | <module> ...]
#   modules: gkiunlock intpol blklog harvest ovl con fb shot peek ppmtemp spmstub simpledrm panfrost   (sources in kernel-modules/<module>)
#
# Method (same as the vendor build of the kernel): every module is compiled with Android clang r510928 against a COPY of the prepared g94 kernel output
# directory (TB_KDIR; the original is never written to), debug info is stripped, and the vermagic string is changed to the debug kernel's (TB_KREL_TARGET).
# simpledrm and panfrost are generated from the kernel's own sources with exact-match patches first (kernel-modules/simpledrm/patch_simpledrm.py,
# kernel-modules/panfrost/patch_panfrost.py); the build aborts if a patch no longer matches.
# Output: $TB_MODOUT/<name>.ko  (tb330fu_<module>.ko, simpledrm.ko, panfrost.ko)  and a SHA256SUMS file.
set -uo pipefail
. "$(dirname "$0")/lib/common.sh"

ALL="gkiunlock intpol blklog harvest ovl con fb shot peek ppmtemp spmstub simpledrm panfrost"
need make python3 pahole cp find sha256sum
[ -x "$TB_CLANG/clang" ] || die "clang not found in $TB_CLANG (run scripts/kernel/fetch-toolchain.sh)"
[ -f "$TB_KDIR/Module.symvers" ] || die "no prepared kernel tree in $TB_KDIR (run scripts/kernel/prepare-kernel-tree.sh)"
[ $# -gt 0 ] || set -- all
[ "$1" = all ] && set -- $ALL
kbuild_env
BUILD=${TB_BUILD_TMP:-/tmp/tb330fu-modbuild}
mkdir -p "$TB_MODOUT"
status=0

# Symbols that the debug kernel exports but the production tree (whose headers and Module.symvers the build uses) does not: their CRCs come from the debug
# kernel's own export table. usage: extra_symvers <module build dir> <symbol>...   (sets EXTRA)
extra_symvers() {
    local m=$1 ex=$TB_KDATA/boot-debug/exports.json
    shift
    [ -f "$ex" ] || die "$ex missing (run scripts/kernel/extract-kernel-data.sh)"
    python3 -I "$TB_ROOT/scripts/kernel/make-extra-symvers.py" "$ex" "$(dirname "$m")/extra.symvers" "$@" || return 1
    EXTRA="KBUILD_EXTRA_SYMBOLS=$(dirname "$m")/extra.symvers"
}

# ppmtemp calls into the vendor module mtk_ppm_v3 (TB_VENDOR_MODULES = directory with the modules of your ROW074 vendor_dlkm)
prep_ppmtemp() {     # $1 = module build dir; sets EXTRA
    local v=${TB_VENDOR_MODULES:-$TB_FIRMWARE/vendor_dlkm/lib/modules}
    [ -f "$v/mtk_ppm_v3.ko" ] || die "$v/mtk_ppm_v3.ko not found (set TB_VENDOR_MODULES to the vendor_dlkm module directory)"
    find "$TB_ROOT/kernel-modules/ppmtemp" -maxdepth 1 -type f \( -name '*.c' -o -name Makefile \) -exec cp {} "$1/" \;
    python3 -I "$TB_ROOT/scripts/kernel/module-symvers.py" "$v/mtk_ppm_v3.ko" "$(dirname "$1")/extra.symvers" mt_ppm_thermal_get_cpu_cluster_temp_cb || return 1
    EXTRA="KBUILD_EXTRA_SYMBOLS=$(dirname "$1")/extra.symvers"
}

prep_simpledrm() {   # $1 = module build dir; sets EXTRA
    local s=$TB_ROOT/kernel-modules/simpledrm d=$TB_KSRC/drivers/gpu/drm
    [ -f "$d/tiny/simpledrm.c" ] || die "kernel source not found in $TB_KSRC (run scripts/kernel/fetch-kernel-source.sh)"
    cp "$d/drm_format_helper.c" "$d/drm_gem_atomic_helper.c" "$d/drm_gem_framebuffer_helper.c" "$1/"
    cp "$s/tb330fu_shim.c" "$s/noexport.h" "$s/tb330fu_flip.inc" "$s/Makefile" "$1/"
    python3 -I "$s/patch_simpledrm.py" "$d/tiny/simpledrm.c" "$s/tb330fu_flip.inc" "$1/sd_main.c" || return 1
    # screen on/off: the backlight class device is looked up by name
    extra_symvers "$1" backlight_device_get_by_name || return 1
}

prep_panfrost() {    # $1 = module build dir; sets EXTRA to the KBUILD_EXTRA_SYMBOLS argument
    local s=$TB_ROOT/kernel-modules/panfrost d=$TB_KSRC/drivers/gpu/drm
    [ -f "$d/panfrost/panfrost_drv.c" ] || die "kernel source not found in $TB_KSRC (run scripts/kernel/fetch-kernel-source.sh)"
    cp "$d"/panfrost/*.c "$d"/panfrost/*.h "$d"/scheduler/*.c "$d"/scheduler/*.h "$1/"
    cp "$TB_ROOT/kernel-modules/simpledrm/noexport.h" "$s/tb330fu_gpu.c" "$s/tb330fu_gpu.h" "$s/tb330fu_drm_compat.c" "$1/"
    python3 -I "$s/patch_panfrost.py" "$1" || return 1
    cat > "$1/Makefile" <<'MK'
obj-m += panfrost.o
panfrost-y := panfrost_drv.o panfrost_device.o panfrost_devfreq.o panfrost_gem.o panfrost_gem_shrinker.o panfrost_gpu.o panfrost_job.o panfrost_mmu.o panfrost_perfcnt.o panfrost_dump.o sched_main.o sched_entity.o sched_fence.o tb330fu_gpu.o tb330fu_drm_compat.o
ccflags-y += -include $(src)/noexport.h -I$(src) -I$(srctree)/drivers/gpu/drm -Wno-unused-function
MK
    extra_symvers "$1" drm_gem_vmap || return 1
}

build_one() {
    local n=$1 ko src B
    case "$n" in simpledrm|panfrost) ko=$n ;; *) ko=tb330fu_$n ;; esac
    case "$n" in con) src=$TB_ROOT/kernel-modules/console ;; *) src=$TB_ROOT/kernel-modules/$n ;; esac   # (a directory named con cannot exist on Windows)
    [ -d "$src" ] || { echo "unknown module: $n"; return 1; }
    B=$BUILD/$n
    rm -rf "$B"; mkdir -p "$B/m"
    cp -a "$TB_KDIR" "$B/kdir"                      # the module directory is a sibling of kdir, so __FILE__ strings are the same on every machine
    EXTRA=""
    case "$n" in
        simpledrm) prep_simpledrm "$B/m" || return 1 ;;
        panfrost)  prep_panfrost "$B/m" || return 1 ;;
        ppmtemp)   prep_ppmtemp "$B/m" || return 1 ;;
        *) find "$src" -maxdepth 1 -type f \( -name '*.c' -o -name '*.h' -o -name '*.inc' -o -name Makefile \) -exec cp {} "$B/m/" \; ;;
    esac
    echo "=============== $n"
    ( cd /tmp && make -C "$B/kdir" M="$B/m" $EXTRA modules LOCALVERSION="" 2>&1 | grep -v -E '^  (CC|LD|BTF)|Skipping BTF' | tail -30
      exit "${PIPESTATUS[0]}" ) || { echo "make failed for $n"; return 1; }
    [ -f "$B/m/$ko.ko" ] || { echo "no $ko.ko produced"; return 1; }
    "$TB_CLANG/llvm-objcopy" --strip-debug "$B/m/$ko.ko" "$B/$ko.stripped.ko" || return 1
    if [ "$n" = spmstub ]; then    # the stand-in exports under the CRCs that the audio driver (the importer) expects
        local v=${TB_VENDOR_MODULES:-$TB_FIRMWARE/vendor_dlkm/lib/modules}
        [ -f "$v/snd-soc-mt6768-afe.ko" ] || die "$v/snd-soc-mt6768-afe.ko not found (set TB_VENDOR_MODULES to the vendor_dlkm module directory)"
        python3 -I "$TB_ROOT/scripts/patch-crc.py" "$B/$ko.stripped.ko" "$B/$ko.crc.ko" "$v/snd-soc-mt6768-afe.ko" spm_resource_req mtk_idle_notifier_register RegisterConditionEnterSuspend || return 1
        mv "$B/$ko.crc.ko" "$B/$ko.stripped.ko"
    fi
    python3 -I "$TB_ROOT/scripts/patch-vermagic.py" "$B/$ko.stripped.ko" "$TB_MODOUT/$ko.ko" "$TB_KREL_BUILD" "$TB_KREL_TARGET" "$TB_VERMAGIC_FLAGS" || return 1
    echo "$(sha "$TB_MODOUT/$ko.ko")  $ko.ko"
}

for n in "$@"; do build_one "$n" || status=1; done
( cd "$TB_MODOUT" && sha256sum *.ko > SHA256SUMS )
echo; echo "modules in $TB_MODOUT:"; cat "$TB_MODOUT/SHA256SUMS"
exit $status
