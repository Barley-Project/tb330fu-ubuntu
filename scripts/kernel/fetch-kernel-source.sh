#!/bin/bash
# Fetch kernel/common (android15-6.6) at the commit the production vendor kernel was built from (the "g94c1a24cabd5" in its release string) into $TB_KSRC
# and apply the one-line host-tool fix in patches/ (resolve_btfids does not build with clang r510928 -Werror otherwise).
set -euo pipefail
. "$(dirname "$0")/../lib/common.sh"
need git
mkdir -p "$TB_KSRC"
[ -d "$TB_KSRC/.git" ] || git init -q "$TB_KSRC"
if [ "$(git -C "$TB_KSRC" rev-parse HEAD 2>/dev/null || true)" != "$TB_KSRC_COMMIT" ]; then
    git -C "$TB_KSRC" ${TB_PROXY:+-c http.proxy="$TB_PROXY"} fetch --depth=1 https://android.googlesource.com/kernel/common "$TB_KSRC_COMMIT"
    git -C "$TB_KSRC" checkout -q --detach FETCH_HEAD
fi
[ "$(git -C "$TB_KSRC" rev-parse HEAD)" = "$TB_KSRC_COMMIT" ] || die "kernel source is not at $TB_KSRC_COMMIT"
if git -C "$TB_KSRC" apply --check "$TB_ROOT/patches/libbpf-const-correctness.patch" 2>/dev/null; then
    git -C "$TB_KSRC" apply "$TB_ROOT/patches/libbpf-const-correctness.patch"
fi
echo "kernel source ready: $TB_KSRC @ $TB_KSRC_COMMIT"
