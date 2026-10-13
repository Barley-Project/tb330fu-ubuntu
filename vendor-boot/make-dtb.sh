#!/bin/bash
# Derive the device tree table of the vendor_boot image from the ROW074 one.
#
# usage: make-dtb.sh <stock dtb table (the "dtb" section of vendor_boot-debug.img)> <out dtb table>
#
# Why the stock table has to be changed (this chain is CN lk + CN dtbo + ROW tee + CN gz + ROW kernel):
#   * The CN bootloader applies the CN dtbo on top of this base tree (libufdt, exact node names). The CN dtbo references two labels that the ROW base tree
#     does not have: mt6370_pmu and panel. Without them LK aborts ("Couldn't find symbol in main dtb"). Two empty stub nodes (phandles 0x171 / 0x172,
#     the base tree's highest phandle is 0x170) satisfy the references; the dtbo fills the panel node (GPIOs).
#   * The CN LK does not write the DRAM properties (dram_type, ch_cnt, rk_cnt, ...) that the ROW dramc/dvfsrc drivers require, so that module family is not
#     loaded. The two mmc nodes lose their dvfsrc vcore supply (otherwise fw_devlink holds the probe back), and the DRAM VDDQ regulator ldo_vdram2 - whose
#     only consumer would have been dramc - is marked always-on (the kernel's regulator_late_cleanup would switch it off 30 s after boot and kill DRAM).
#   * The SD card host (mmc@11240000) is disabled; the root is on the internal eMMC and one more card-detect interrupt is one more thing to storm.
# The result is wrapped into an Android DT table with the same header fields as the stock one.
set -euo pipefail
. "$(dirname "$0")/../scripts/lib/common.sh"
need fdtput fdtget dtc python3
[ $# -eq 2 ] || die "usage: $0 <stock dtb table> <out dtb table>"
in=$1; out=$2
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
fdt=$tmp/fdt.dtb

python3 -I - "$in" "$fdt" <<'PY'
import struct, sys
t = open(sys.argv[1], "rb").read()
magic, total, hs, es, cnt, eo, pg, ver = struct.unpack(">8I", t[:32])
assert magic == 0xd7b7ab1e and cnt == 1 and total == len(t), (hex(magic), cnt, total, len(t))
size, off = struct.unpack(">2I", t[eo:eo + 8])
open(sys.argv[2], "wb").write(t[off:off + size])
PY

# the stub phandles must be free
dtc -I dtb -O dts "$fdt" 2>/dev/null > "$tmp/base.dts"
if grep -q -E 'phandle = <0x(171|172)>' "$tmp/base.dts"; then die "phandle 0x171/0x172 already used in the base tree"; fi

fdtput -c "$fdt" /mt6370_pmu_dts
fdtput -t x "$fdt" /mt6370_pmu_dts phandle 171
fdtput -c "$fdt" /panel@0
fdtput -t s "$fdt" /panel@0 compatible "mediatek,panel"
fdtput -t x "$fdt" /panel@0 phandle 172
fdtput -t s "$fdt" /__symbols__ mt6370_pmu /mt6370_pmu_dts
fdtput -t s "$fdt" /__symbols__ panel /panel@0
for n in /mmc@11230000 /mmc@11240000; do fdtput -d "$fdt" "$n" dvfsrc-vcore-supply req-vcore; done
fdtput "$fdt" /pwrap@1000d000/mt6358-pmic/mt6358regulator/ldo_vdram2 regulator-always-on
fdtput -t s "$fdt" /mmc@11240000 status disabled

python3 -I - "$fdt" "$in" "$out" <<'PY'
import struct, sys
fdt = open(sys.argv[1], "rb").read()
ref = open(sys.argv[2], "rb").read()
magic, total, hs, es, cnt, eo, pg, ver = struct.unpack(">8I", ref[:32])
r_size, r_off, r_id, r_rev, c0, c1, c2, c3 = struct.unpack(">8I", ref[eo:eo + 32])
assert struct.unpack(">I", fdt[4:8])[0] == len(fdt)
open(sys.argv[3], "wb").write(struct.pack(">8I", magic, 64 + len(fdt), hs, es, 1, eo, pg, ver) + struct.pack(">8I", len(fdt), 64, r_id, r_rev, c0, c1, c2, c3) + fdt)
print("DT table: %d bytes (FDT %d)" % (64 + len(fdt), len(fdt)))
PY
