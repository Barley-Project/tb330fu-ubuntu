#!/bin/bash
# Build the kernel image (and all in-tree modules) from the pinned kernel/common source (g94, the source of the production GKI kernel) with the
# configuration of the debug kernel that this project boots (gda: the g94 config with CONFIG_TRIM_UNUSED_KSYMS off and a few USB NIC modules), plus an
# optional config fragment.  The release string is set to the debug kernel's (CONFIG_LOCALVERSION), so every module already built for this project
# (vermagic gda) and the vendor modules (vermagic patched to gda) keep loading.
#
# usage: scripts/kernel/build-kernel.sh <name> [fragment ...]
#   <name>      build directory $TB_WORK/kernel/build-<name>; "baseline" with no fragment is the reproduction test (see crc-compare.py)
#   fragment    a Kconfig fragment (kernel-config/*.config); applied with the kernel's own merge_config.sh, then olddefconfig
# Output (in the build directory): arch/arm64/boot/Image, Module.symvers, .config, modules-staged/ (stripped modules + depmod files)
set -euo pipefail
. "$(dirname "$0")/../lib/common.sh"
need make python3 pahole flex bison bc openssl depmod
[ $# -ge 1 ] || die "usage: build-kernel.sh <name> [fragment ...]"
name=$1; shift
frags=(); for f in "$@"; do [ -f "$f" ] || die "fragment $f not found"; frags+=("$(readlink -f "$f")"); done
B=$TB_WORK/kernel/build-$name
cfg=$TB_KDATA/boot-debug/config
[ -f "$cfg" ] || die "$cfg missing (run scripts/kernel/extract-kernel-data.sh)"
[ -x "$TB_CLANG/clang" ] || die "clang missing (run scripts/kernel/fetch-toolchain.sh)"
[ "$(git -C "$TB_KSRC" rev-parse HEAD 2>/dev/null)" = "$TB_KSRC_COMMIT" ] || die "kernel source is not at the pinned commit (run scripts/kernel/fetch-kernel-source.sh)"
kbuild_env
export KBUILD_BUILD_TIMESTAMP='Thu Jan  1 00:00:00 UTC 1970'     # what uname -v of the debug kernel shows
mkdir -p "$B"
cp "$cfg" "$B/.config.base"
# the release string is CONFIG_LOCALVERSION (no scm suffix): 6.6.30 + -android15-8-gda510ce0fdcf-4k
python3 - "$B/.config.base" "$TB_KREL_TARGET" <<'PY'
import sys
p, rel = sys.argv[1], sys.argv[2]
s = open(p).read()
old = 'CONFIG_LOCALVERSION="-4k"'
new = 'CONFIG_LOCALVERSION="%s"' % rel.split("6.6.30", 1)[1]
assert s.count(old) == 1 and "CONFIG_LOCALVERSION_AUTO=y" in s
open(p, "w").write(s.replace(old, new).replace("CONFIG_LOCALVERSION_AUTO=y", "# CONFIG_LOCALVERSION_AUTO is not set"))
PY
cp "$B/.config.base" "$B/.config"
make -C "$TB_KSRC" O="$B" olddefconfig
if [ "${#frags[@]}" -gt 0 ]; then
    # merge_config.sh -m: merge only (no olddefconfig inside), then resolve dependencies once
    ( cd "$B" && "$TB_KSRC/scripts/kconfig/merge_config.sh" -m -O "$B" "$B/.config" "${frags[@]}" )
    make -C "$TB_KSRC" O="$B" olddefconfig
fi
# what did the fragment really change? (options that olddefconfig dropped or that depend on something missing show up here)
diff <(sort "$B/.config.base") <(sort "$B/.config") > "$B/config.diff" || true
echo "config changes vs. the debug kernel: $(grep -c '^[<>]' "$B/config.diff") lines (see $B/config.diff)"
actual=$(make -s -C "$TB_KSRC" O="$B" kernelrelease)
[ "$actual" = "$TB_KREL_TARGET" ] || die "kernel release '$actual' is not '$TB_KREL_TARGET'"
make -C "$TB_KSRC" O="$B" -j"$(nproc)" Image modules 2>&1 | grep -v -E '^  (CC|LD|AR|AS|HOSTCC|HOSTLD|GEN|UPD|CHK|BTF|CALL|SYMLINK|GZIP|OBJCOPY|STUBCP|STUBS)' | tail -40
ls -la "$B/arch/arm64/boot/Image" "$B/Module.symvers"
rm -rf "$B/modules-staged"
make -C "$TB_KSRC" O="$B" INSTALL_MOD_PATH="$B/modules-staged" INSTALL_MOD_STRIP=1 modules_install 2>&1 | tail -3
echo "built $actual in $B"
