#!/bin/bash
# Build the vendor_boot image: the owner's ROW074 debug vendor_boot with
#   - a new third ramdisk fragment (init launcher, BusyBox, stage1.sh, the early vendor modules and this project's modules),
#   - a patched device tree table (make-dtb.sh),
#   - the kernel command line below.
# The platform fragment (vendor_ramdisk00) and the recovery fragment (vendor_ramdisk01) of the stock image are carried over byte for byte; the vendor
# modules in our fragment are copies of the ones in the platform fragment (the build checks it).
#
# Inputs (see docs/09-build.md):
#   $TB_FIRMWARE/vendor_boot-debug.img   ROW074 debug vendor_boot (from your own firmware package)
#   $TB_FIRMWARE/boot-debug.img          optional: only for the load-order simulation at the end
#   $TB_MODOUT/*.ko                      scripts/build-modules.sh (tb330fu_blklog con fb harvest intpol ovl, simpledrm)
#   $TB_BUSYBOX                          static aarch64 BusyBox (scripts/build-busybox.sh)
#   $TB_UPSTREAM/src/boot/init.c         scripts/kernel/fetch-upstream.sh
# Output: $TB_DIST/vendor_boot.img (+ .sha256), padded to the 64 MiB partition size.
# Never flashes anything.
set -uo pipefail
. "$(dirname "$0")/../scripts/lib/common.sh"
HERE=$TB_ROOT/vendor-boot
STOCK=${TB_STOCK_VENDOR_BOOT:-$TB_FIRMWARE/vendor_boot-debug.img}
BUSYBOX=${TB_BUSYBOX:-$TB_WORK/busybox/busybox}
STAGE1=${TB_STAGE1:-$HERE/stage1.sh}
CC=${CC:-aarch64-linux-gnu-gcc}
OUT=$TB_DIST/vendor_boot.img
W=$TB_STAGE
OWN_MODULES="tb330fu_ovl tb330fu_con tb330fu_fb simpledrm tb330fu_blklog tb330fu_harvest tb330fu_intpol"
CMDLINE='bootopt=64S3,32N2,64N2 rdinit=/tb330fu-init printk.devkmsg=on log_buf_len=2m cpuidle.off=1 initcall_debug=0 systemd.log_target=kmsg systemd.log_level=info irqaffinity=7 systemd.journald.forward_to_kmsg=1 systemd.journald.max_level_kmsg=info ignore_loglevel systemd.mask=systemd-pstore.service'

need python3 lz4 cpio sha256sum "$CC" truncate fdtput file strings xxd gzip
note "1. pre-flight"
chk "mkbootimg.py (AOSP android15-release)" 37d84b3d162e0bc62e36c1f4e1c63c85ea0caa9f29be023eb2f8efe006ad948c "$(sha "$TB_MKBOOTIMG")"
[ -f "$STOCK" ] || die "stock vendor_boot not found: $STOCK"
[ -f "$BUSYBOX" ] || die "BusyBox not found: $BUSYBOX (scripts/build-busybox.sh)"
[ -f "$TB_UPSTREAM/src/boot/init.c" ] || die "upstream sources missing (scripts/kernel/fetch-upstream.sh)"
for m in $OWN_MODULES; do [ -f "$TB_MODOUT/$m.ko" ] || die "$TB_MODOUT/$m.ko missing (scripts/build-modules.sh)"; done
file "$BUSYBOX" 2>/dev/null | grep -q -E 'ARM aarch64.*(statically linked|static-pie linked)' || die "BusyBox is not a static aarch64 executable"
for m in $OWN_MODULES; do
    vm=$(strings -a "$TB_MODOUT/$m.ko" | grep '^vermagic=' | head -1)
    chk "$m.ko vermagic is the target kernel's" "vermagic=$TB_KREL_TARGET $TB_VERMAGIC_FLAGS" "$vm"
done
[ $fail_count -eq 0 ] || { echo "PRE-FLIGHT FAILED - nothing built."; exit 2; }
rm -rf "$W"; mkdir -p "$W/unpack" "$W/vr00" "$W/stage" "$TB_DIST"

note "2. unpack the stock image"
python3 -I "$TB_UNPACK_BOOTIMG" --boot_img "$STOCK" --out "$W/unpack" > "$W/unpack.info.txt" || die "unpack failed"
grep -E 'header version|page size|vendor ramdisk total size|dtb size' "$W/unpack.info.txt"
RD0=$W/unpack/vendor_ramdisk00; RD1=$W/unpack/vendor_ramdisk01
chk "stock platform fragment (ROW074 debug vendor_boot)" 87bd87e029c364c01b978e6ea0e63a50ccc4f3e0c12b61f128b6f2ea87b2c596 "$(sha "$RD0")"
chk "stock recovery fragment" aba31bdb9c40062df9837237b89b7d4eaf853b7b427b992e83867a8046703b93 "$(sha "$RD1")"
chk "stock dtb table" e52da954bc80b35f71cfeb12df3213410a08df2963de45f8bfa3f5992b271636 "$(sha "$W/unpack/dtb")"
if [ $fail_count -ne 0 ]; then
    [ -n "${TB_ALLOW_OTHER_FIRMWARE:-}" ] || die "this is not the ROW074 debug vendor_boot the project was developed against (set TB_ALLOW_OTHER_FIRMWARE=1 to continue at your own risk)"
    echo "WARNING: continuing with a different stock image"; fail_count=0
fi
( cd "$W/vr00" && lz4 -dc "$RD0" 2>/dev/null | cpio -idm --quiet 2>/dev/null )

note "3. stage tree (ramdisk fragment 2)"
S=$W/stage
mkdir -p "$S/bin" "$S/etc" "$S/lib/modules"
"$CC" -static -O2 -s -Wall -Wextra -Werror -o "$S/init" "$TB_UPSTREAM/src/boot/init.c" || die "init.c did not build"
cp "$S/init" "$S/tb330fu-init"
cp "$BUSYBOX" "$S/bin/busybox"
sid=$(sha "$STAGE1" | cut -c1-8)
sed "s/@STAGE1_ID@/$sid/g" "$STAGE1" > "$S/etc/stage1.sh"
sed -e "s/@ROOT_LABEL@/$TB_ROOT_LABEL/" -e "s/@ROOT_UUID@/$TB_ROOT_UUID/" "$HERE/stage1.conf.in" > "$S/etc/tb330fu-stage1.conf"
while IFS= read -r m; do
    [ -n "$m" ] || continue
    [ -f "$W/vr00/lib/modules/$m" ] || die "vendor module $m not in the stock platform fragment"
    cp "$W/vr00/lib/modules/$m" "$S/lib/modules/$m"
done < "$HERE/modules.vendor"
for m in $OWN_MODULES; do cp "$TB_MODOUT/$m.ko" "$S/lib/modules/$m.ko"; done
cp "$HERE/modules.load.native" "$HERE/modules.load.display" "$HERE/modules.load.extra" "$S/lib/modules/"
# modes inside the ramdisk (TB_STAGE_*MODE only exist to reproduce an older image bit for bit: it had been built on a Windows drive, where everything is 0777)
find "$S" -type d -exec chmod "${TB_STAGE_DIRMODE:-0755}" {} +
find "$S" -type f -exec chmod "${TB_STAGE_FILEMODE:-0644}" {} +
chmod "${TB_STAGE_EXECMODE:-0755}" "$S/init" "$S/tb330fu-init" "$S/bin/busybox" "$S/etc/stage1.sh"
echo "stage1 id $sid; $(find "$S" -type f | wc -l) files, $(ls "$S/lib/modules" | grep -c '\.ko$') modules"

note "4. cpio + lz4 (legacy format, as the kernel expects)"
find "$S" -exec touch -h -d '@0' {} +
( cd "$S" && find . -print0 | LC_ALL=C sort -z | cpio --null -o -H newc --owner=0:0 --reproducible > "$W/fragment02.cpio" ) 2>&1 | tail -1
lz4 -l -12 -f "$W/fragment02.cpio" "$W/fragment02.lz4" 2>&1 | tail -1
lz4 -d -f "$W/fragment02.lz4" "$W/roundtrip.cpio" > /dev/null 2>&1
chk "lz4 round trip is byte-exact" "$(sha "$W/fragment02.cpio")" "$(sha "$W/roundtrip.cpio")"

note "5. device tree table"
bash "$HERE/make-dtb.sh" "$W/unpack/dtb" "$W/dtb_table.bin" || die "make-dtb failed"

note "6. mkbootimg"
python3 "$TB_MKBOOTIMG" --vendor_boot "$W/vendor_boot.raw" \
    --header_version 4 --pagesize 0x00001000 --base 0x00000000 \
    --kernel_offset 0x40080000 --ramdisk_offset 0x47c80000 --tags_offset 0x4bc80000 \
    --dtb_offset 0x000000004bc80000 --vendor_cmdline "$CMDLINE" --board '' \
    --dtb "$W/dtb_table.bin" \
    --vendor_bootconfig "$W/unpack/bootconfig" \
    --ramdisk_type 1 --ramdisk_name '' --vendor_ramdisk_fragment "$RD0" \
    --ramdisk_type 2 --ramdisk_name recovery --vendor_ramdisk_fragment "$RD1" \
    --ramdisk_type 1 --ramdisk_name init_boot --vendor_ramdisk_fragment "$W/fragment02.lz4" || die "mkbootimg failed"
truncate -s 67108864 "$W/vendor_boot.raw"        # the vendor_boot partition is 64 MiB

note "7. verify the result"
V=$W/verify; mkdir -p "$V"
python3 -I "$TB_UNPACK_BOOTIMG" --boot_img "$W/vendor_boot.raw" --out "$V" > "$W/verify.info.txt"
chk "image size" 67108864 "$(stat -c %s "$W/vendor_boot.raw")"
chk "vendor cmdline" "$CMDLINE" "$(grep 'vendor command line args' "$W/verify.info.txt" | sed 's/^vendor command line args: //')"
chk "ramdisk00 is the stock platform fragment" "$(sha "$RD0")" "$(sha "$V/vendor_ramdisk00")"
chk "ramdisk01 is the stock recovery fragment" "$(sha "$RD1")" "$(sha "$V/vendor_ramdisk01")"
chk "ramdisk02 is the new fragment" "$(sha "$W/fragment02.lz4")" "$(sha "$V/vendor_ramdisk02")"
chk "dtb section is the derived table" "$(sha "$W/dtb_table.bin")" "$(sha "$V/dtb")"
chk "bootconfig is empty" 0 "$(stat -c %s "$V/bootconfig")"
python3 -I - "$W/vendor_boot.raw" <<'PY' || fail_count=$((fail_count + 1))
import struct, sys
d = open(sys.argv[1], "rb").read(); page = 4096
rs, = struct.unpack_from("<I", d, 24); ds, = struct.unpack_from("<I", d, 2100); ts, = struct.unpack_from("<I", d, 2112); bs, = struct.unpack_from("<I", d, 2124)
al = lambda x: (x + page - 1) // page * page
end = al(al(al(page + rs) + ds) + ts) + al(bs)
ok = not any(d[end:]) and b"AVBf" not in d[-64:]
print("%s  everything after the last section is zero padding, no AVB footer (%d bytes)" % ("PASS" if ok else "FAIL", len(d) - end))
sys.exit(0 if ok else 1)
PY

# simulate the kernel's unpack order (platform, recovery, ours, then the ramdisk of boot-debug.img) and check what stage1 will look for
SIM=$W/sim; mkdir -p "$SIM"
unz() { case "$(head -c 4 "$1" | xxd -p)" in 02214c18) lz4 -dc "$1" 2>/dev/null ;; 1f8b*) gzip -dc "$1" ;; *) cat "$1" ;; esac; }
srcs="$V/vendor_ramdisk00 $V/vendor_ramdisk01 $V/vendor_ramdisk02"
if [ -f "$TB_FIRMWARE/boot-debug.img" ]; then
    python3 -I "$TB_UNPACK_BOOTIMG" --boot_img "$TB_FIRMWARE/boot-debug.img" --out "$W/bootimg" > /dev/null && srcs="$srcs $W/bootimg/ramdisk"
fi
for f in $srcs; do [ -f "$f" ] && unz "$f" | ( cd "$SIM" && cpio -idmu --quiet 2>/dev/null ); done
chk "after the unpack /etc/stage1.sh is ours" "$(sha "$S/etc/stage1.sh")" "$(sha "$SIM/etc/stage1.sh")"
chk "after the unpack /tb330fu-init is the launcher (rdinit=)" "$(sha "$S/init")" "$(sha "$SIM/tb330fu-init")"
for lst in modules.load.native modules.load.display modules.load.extra; do
    miss=0; while IFS= read -r m; do [ -z "$m" ] && continue; [ -f "$SIM/lib/modules/$m" ] || { echo "FAIL  $lst names $m but it is not in the unpacked ramdisk"; miss=$((miss + 1)); }; done < "$SIM/lib/modules/$lst"
    chk "every module named in $lst exists after the unpack" 0 "$miss"
done
OTGL=$(grep '^OTG_MODULES=' "$S/etc/stage1.sh" | sed -E 's/.*:-([^}]*)\}.*/\1/')
miss=0; for m in $OTGL; do [ -f "$SIM/lib/modules/$m.ko" ] || { echo "FAIL  OTG module $m.ko missing"; miss=$((miss + 1)); }; done
chk "all OTG modules (stage1 OTG_MODULES) exist after the unpack" 0 "$miss"
if command -v qemu-aarch64 >/dev/null; then
    chk "stage1.sh passes busybox 'sh -n'" 0 "$(qemu-aarch64 "$BUSYBOX" sh -n "$S/etc/stage1.sh" > /dev/null 2>&1; echo $?)"
fi
if [ -f "$TB_KDATA/boot-debug/exports.json" ]; then
    # imports/CRCs of the whole load order against the debug kernel's export table (and the protected-export list)
    cat "$SIM/lib/modules/modules.load.native" "$SIM/lib/modules/modules.load.display" "$SIM/lib/modules/modules.load.extra" > "$W/load-sequence.txt"
    chk "loader simulation of native + display + extra (vermagic, imports, CRCs)" 0 "$(python3 -I "$TB_ROOT/scripts/verify/loadsim.py" "$W/load-sequence.txt" "$SIM/lib/modules" > "$W/loadsim.log" 2>&1; echo $?)"
fi
chk_done
cp "$W/vendor_boot.raw" "$OUT"
( cd "$TB_DIST" && sha256sum vendor_boot.img > vendor_boot.img.sha256 && cat vendor_boot.img.sha256 )
