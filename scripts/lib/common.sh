#!/bin/bash
# Common settings and helpers, sourced by every script in this repository.
#
# All paths are configurable. Put overrides in <repo>/config.sh (copy config.example.sh) or export them in the environment.
# Run everything on Linux (native or WSL2); the repository itself never writes outside TB_WORK, TB_DIST and mktemp directories.

TB_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
# shellcheck disable=SC1091
[ -f "$TB_ROOT/config.sh" ] && . "$TB_ROOT/config.sh"

# everything that is downloaded or built
: "${TB_WORK:=$HOME/tb330fu-work}"
# the owner's own copy of the ROW074 firmware images (boot.img, boot-debug.img, vendor_boot-debug.img, dtbo.img, ...). Never published, never modified.
: "${TB_FIRMWARE:=$TB_WORK/firmware}"
# final images and their checksums
: "${TB_DIST:=$TB_ROOT/dist}"

# toolchain and kernel
: "${TB_CLANG:=$TB_WORK/toolchains/clang-r510928/bin}"        # Android clang r510928 (the compiler the vendor kernel was built with)
: "${TB_KSRC:=$TB_WORK/kernel/common}"                         # kernel/common at the pinned commit (android15-6.6, 6.6.30)
: "${TB_KDIR:=$TB_WORK/kernel/prepared}"                       # prepared Kbuild output directory of the g94 build (headers + Module.symvers)
: "${TB_KDATA:=$TB_WORK/kernel/data}"                          # per-kernel data extracted from the firmware: <tag>/{Image,config,System.map,exports.json,Module.symvers}
: "${TB_KSRC_COMMIT:=94c1a24cabd5dc987132f98c4d7a7a32f2a38fef}"
# the vendor kernels: production g94 (boot.img) and the debug build gda (boot-debug.img) that this project boots
: "${TB_KREL_BUILD:=6.6.30-android15-8-g94c1a24cabd5-ab12293704-4k}"
: "${TB_KREL_TARGET:=6.6.30-android15-8-gda510ce0fdcf-4k}"
: "${TB_VERMAGIC_FLAGS:=SMP preempt mod_unload modversions aarch64}"

# build outputs
: "${TB_MODOUT:=$TB_WORK/modules}"                              # built kernel modules (gda vermagic), one .ko each
: "${TB_STAGE:=$TB_WORK/vendor-boot}"                           # vendor_boot working directory
: "${TB_ROOTFS_WORK:=$TB_WORK/rootfs}"

# upstream project this one is based on (stage1/init/devd/usb gadget); fetched on demand at a pinned commit
: "${TB_UPSTREAM:=$TB_WORK/upstream/nevedoma-zverushka}"
: "${TB_UPSTREAM_URL:=https://github.com/feliksyasnopolski/nevedoma-zverushka.git}"
: "${TB_UPSTREAM_COMMIT:=118bf0b66dd99b2456676473f566b1e9aa283f7f}"

# the root filesystem is found by stage1 through its ext4 label / UUID (historic name: the first image was a KDE one)
: "${TB_ROOT_LABEL:=TB330FU_KDE}"
: "${TB_ROOT_UUID:=7292ba20-d80a-45be-8002-000000000024}"

# AOSP boot image tools (vendored in third_party/)
: "${TB_MKBOOTIMG:=$TB_ROOT/third_party/mkbootimg/mkbootimg.py}"
: "${TB_UNPACK_BOOTIMG:=$TB_ROOT/third_party/mkbootimg/unpack_bootimg.py}"

export PYTHONDONTWRITEBYTECODE=1
# the helper scripts (python) read these from the environment
export TB_ROOT TB_WORK TB_FIRMWARE TB_DIST TB_CLANG TB_KSRC TB_KDIR TB_KDATA TB_MODOUT TB_STAGE TB_ROOTFS_WORK TB_UPSTREAM TB_KREL_BUILD TB_KREL_TARGET

die() { echo "ERROR: $*" >&2; exit 1; }
note() { echo "== $*"; }
sha() { sha256sum "$1" | cut -d' ' -f1; }
fail_count=0
# chk <what> <expected> <actual>: prints PASS/FAIL and remembers failures; call "chk_done" at the end of a script
chk() {
    if [ "$2" = "$3" ]; then echo "PASS  $1"; else echo "FAIL  $1"; echo "        expected $2"; echo "        actual   $3"; fail_count=$((fail_count + 1)); fi
}
chk_done() { if [ "$fail_count" -eq 0 ]; then echo "ALL CHECKS PASSED"; else echo "$fail_count CHECK(S) FAILED"; exit 5; fi; }
need() { local c; for c in "$@"; do command -v "$c" >/dev/null 2>&1 || die "missing tool: $c"; done; }

# kernel build environment shared by every module build (matches the vendor build: same clang, same fixed timestamp/user/host)
kbuild_env() {
    export ARCH=arm64 LLVM="$TB_CLANG/" LLVM_IAS=1 PAHOLE="$(command -v pahole)"
    export KBUILD_BUILD_HOST=build-host KBUILD_BUILD_USER=kleaf KBUILD_BUILD_VERSION=1
    export KBUILD_BUILD_TIMESTAMP='Thu Aug 29 05:01:15 UTC 2024' LOCALVERSION="" LLVM_OBJCOPY="$TB_CLANG/llvm-objcopy"
}
