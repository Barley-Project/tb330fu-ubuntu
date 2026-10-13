#!/bin/bash
# Download the Android clang toolchain the vendor kernel was built with (clang r510928, "clang version 18.0.0") into $TB_CLANG/..
set -euo pipefail
. "$(dirname "$0")/../lib/common.sh"
need curl tar
dest=$(dirname "$TB_CLANG")
mkdir -p "$dest"
tgz=$dest/clang-r510928.tar.gz
url=https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/heads/android15-release/clang-r510928.tar.gz
if [ ! -x "$TB_CLANG/clang" ]; then
    [ -f "$tgz" ] || curl ${TB_PROXY:+--proxy "$TB_PROXY"} --fail --location --retry 2 --retry-all-errors --connect-timeout 10 --max-time 900 -o "$tgz.part" "$url"
    [ -f "$tgz.part" ] && mv "$tgz.part" "$tgz"
    mkdir -p "$dest/clang-r510928"
    tar -xf "$tgz" -C "$dest/clang-r510928"
fi
"$TB_CLANG/clang" --version | head -1
