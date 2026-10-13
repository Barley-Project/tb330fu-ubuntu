#!/bin/bash
# Extract kernel configuration, symbol table, export table and Module.symvers from the two vendor kernels in your own ROW074 firmware.
#   $TB_FIRMWARE/boot.img        production kernel (g94)  -> $TB_KDATA/boot
#   $TB_FIRMWARE/boot-debug.img  debug kernel (gda)       -> $TB_KDATA/boot-debug   (the kernel this project boots)
set -euo pipefail
. "$(dirname "$0")/../lib/common.sh"
need python3
for tag in boot boot-debug; do
    [ -f "$TB_FIRMWARE/$tag.img" ] || die "$TB_FIRMWARE/$tag.img not found (put your ROW074 images there, see docs/09-build.md)"
    python3 -I "$TB_ROOT/scripts/kernel/extract_kernel_data.py" "$TB_FIRMWARE/$tag.img" "$TB_KDATA/$tag"
done
