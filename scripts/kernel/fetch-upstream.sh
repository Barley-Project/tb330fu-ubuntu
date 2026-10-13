#!/bin/bash
# Fetch the upstream project (NevedomaZverushka, GPL-3.0-or-later) at the commit this repository was developed against: its init.c, tb330fu-devd
# (device node manager: the vendor kernel has no devtmpfs) and the original USB gadget helper are used unmodified.
set -euo pipefail
. "$(dirname "$0")/../lib/common.sh"
need git
if [ ! -d "$TB_UPSTREAM/.git" ]; then
    mkdir -p "$TB_UPSTREAM"
    git init -q "$TB_UPSTREAM"
fi
if [ "$(git -C "$TB_UPSTREAM" rev-parse HEAD 2>/dev/null || true)" != "$TB_UPSTREAM_COMMIT" ]; then
    git -C "$TB_UPSTREAM" ${TB_PROXY:+-c http.proxy="$TB_PROXY"} fetch --depth=1 "$TB_UPSTREAM_URL" "$TB_UPSTREAM_COMMIT"
    git -C "$TB_UPSTREAM" checkout -q --detach FETCH_HEAD
fi
echo "upstream ready: $TB_UPSTREAM @ $(git -C "$TB_UPSTREAM" rev-parse --short HEAD)"
