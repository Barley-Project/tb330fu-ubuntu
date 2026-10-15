#!/bin/bash
# Build boot.img (the kernel partition) from a kernel Image: the stock debug boot image of your ROW074 firmware with its kernel replaced.
#   scripts/kernel/build-boot.sh <Image> [output]      output defaults to $TB_DIST/boot.img
#   scripts/kernel/build-boot.sh --self-test           repack the stock kernel + ramdisk and check that the result is bit-identical to the stock image
# The stock image is boot header v4 with a gzip-compressed kernel (gzip -9), the generic Android ramdisk (3022350 bytes) and an empty command line (the command
# line lives in vendor_boot); it is padded with zeros to the 32 MiB partition.  Only the kernel is replaced; ramdisk and header fields are the stock ones.
set -euo pipefail
. "$(dirname "$0")/../lib/common.sh"
need python3 gzip sha256sum
stock=$TB_FIRMWARE/boot-debug.img
[ -f "$stock" ] || die "$stock not found"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
python3 -I "$TB_UNPACK_BOOTIMG" --boot_img "$stock" --out "$T/stock" > "$T/stock.txt"
grep -q 'boot image header version: 4' "$T/stock.txt" || die "unexpected stock boot image header"
pack() {    # $1 kernel file, $2 output
    python3 "$TB_MKBOOTIMG" --header_version 4 --kernel "$1" --ramdisk "$T/stock/ramdisk" -o "$2.raw"
    truncate -s 33554432 "$2.raw"          # the partition is 32 MiB; the stock image is zero padded
    mv "$2.raw" "$2"
}
if [ "${1:-}" = --self-test ]; then
    python3 "$TB_MKBOOTIMG" --header_version 4 --kernel "$T/stock/kernel" --ramdisk "$T/stock/ramdisk" -o "$T/repack.raw"
    n=$(stat -c %s "$T/repack.raw")
    # the stock image continues with an AVB hash footer (vbmeta): not reproduced, the device runs with verification bypassed
    chk "stock boot image repacked bit-identically (first $n bytes)" "$(head -c "$n" "$stock" | sha256sum | cut -d' ' -f1)" "$(sha "$T/repack.raw")"
    chk_done
    exit 0
fi
[ $# -ge 1 ] || die "usage: build-boot.sh <Image> [output]"
img=$1; out=${2:-$TB_DIST/boot.img}
[ -f "$img" ] || die "$img not found"
mkdir -p "$(dirname "$out")"
gzip -9 -n -c "$img" > "$T/kernel.gz"
pack "$T/kernel.gz" "$out"
# verify: unpack again, the ramdisk is the stock one and the kernel decompresses to the Image we were given
python3 -I "$TB_UNPACK_BOOTIMG" --boot_img "$out" --out "$T/new" > /dev/null
chk "ramdisk is the stock one" "$(sha "$T/stock/ramdisk")" "$(sha "$T/new/ramdisk")"
chk "kernel decompresses to the Image" "$(sha "$img")" "$(gzip -dc "$T/new/kernel" | sha256sum | cut -d' ' -f1)"
chk "image size is 32 MiB" 33554432 "$(stat -c %s "$out")"
chk_done
sha256sum "$out" | tee "$out.sha256"
echo "kernel $(stat -c %s "$img") bytes -> boot.img (gzip $(stat -c %s "$T/kernel.gz") bytes)"
