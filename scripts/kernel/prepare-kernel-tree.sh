#!/bin/bash
# Prepare the Kbuild output directory ($TB_KDIR) that external modules are compiled against: the production (g94) kernel's own configuration, generated
# headers, host tools and the export CRC table (Module.symvers) recovered from its image. Needs extract-kernel-data.sh, fetch-toolchain.sh and
# fetch-kernel-source.sh first. The result is a ~30 MB directory; modules are built against COPIES of it.
set -euo pipefail
. "$(dirname "$0")/../lib/common.sh"
need make python3 pahole
cfg=$TB_KDATA/boot/config
[ -f "$cfg" ] || die "$cfg missing (run scripts/kernel/extract-kernel-data.sh)"
[ -x "$TB_CLANG/clang" ] || die "clang missing (run scripts/kernel/fetch-toolchain.sh)"
[ "$(git -C "$TB_KSRC" rev-parse HEAD 2>/dev/null)" = "$TB_KSRC_COMMIT" ] || die "kernel source is not at the pinned commit (run scripts/kernel/fetch-kernel-source.sh)"
"$TB_CLANG/clang" --version | grep -q 'clang version 18.0.0' || die "unexpected clang version"
for opt in CONFIG_MODVERSIONS CONFIG_DEBUG_INFO_BTF CONFIG_DEBUG_INFO_BTF_MODULES CONFIG_MODULE_SIG_PROTECT; do
    grep -qx "$opt=y" "$cfg" || die "$opt is not enabled in the vendor config"
done
mkdir -p "$TB_KDIR"
cp "$cfg" "$TB_KDIR/.config"
# the release string of the vendor kernel is built into the config as CONFIG_LOCALVERSION (never edit vermagic in a .ko to get it)
python3 - "$TB_KDIR/.config" "$TB_KREL_BUILD" <<'PY'
import sys
p, rel = sys.argv[1], sys.argv[2]
s = open(p).read()
old = 'CONFIG_LOCALVERSION="-4k"'
new = 'CONFIG_LOCALVERSION="%s"' % rel.split("6.6.30", 1)[1]
assert s.count(old) == 1 and "CONFIG_LOCALVERSION_AUTO=y" in s
open(p, "w").write(s.replace(old, new).replace("CONFIG_LOCALVERSION_AUTO=y", "# CONFIG_LOCALVERSION_AUTO is not set"))
PY
kbuild_env
make -C "$TB_KSRC" O="$TB_KDIR" olddefconfig
make -C "$TB_KSRC" O="$TB_KDIR" prepare
actual=$(make -s -C "$TB_KSRC" O="$TB_KDIR" kernelrelease)
[ "$actual" = "$TB_KREL_BUILD" ] || die "Kbuild release '$actual' does not match '$TB_KREL_BUILD'"
make -C "$TB_KSRC" O="$TB_KDIR" -j"$(nproc)" modules_prepare
cp "$TB_KDATA/boot/Module.symvers" "$TB_KDIR/Module.symvers"
echo "prepared kernel tree: $TB_KDIR ($actual)"
