#!/bin/bash
# Install files from rootfs/overlay onto the running tablet (root-owned, executables under usr/local/sbin and usr/libexec).
#   tb-install.sh <path relative to rootfs/overlay> [...]      e.g.  tb-install.sh etc/systemd/system/tb330fu-gnome.service usr/local/sbin/tb330fu-wmt-loader
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/../rootfs/overlay"
[ $# -gt 0 ] || { echo "usage: tb-install.sh <relative path> ..."; exit 2; }
for p in "$@"; do [ -e "$SRC/$p" ] || { echo "tb-install.sh: $SRC/$p does not exist"; exit 3; }; done
T="$(mktemp -d)"
tar cf "$T/tbinst.tar" -C "$SRC" --owner=0 --group=0 --numeric-owner "$@" || exit 4
bash "$HERE/tb.sh" put "$T/tbinst.tar" /tmp/tbinst.tar >/dev/null || exit 5
{
    echo 'set -e'
    echo 'tar xf /tmp/tbinst.tar -C / --no-same-owner --no-same-permissions'
    for p in "$@"; do
        case "$p" in
            usr/local/sbin/*|usr/libexec/*|usr/local/bin/*) echo "chmod -R 755 '/$p'" ;;
            *) echo "if [ -d '/$p' ]; then find '/$p' -type d -exec chmod 755 {} +; find '/$p' -type f -exec chmod 644 {} +; else chmod 644 '/$p'; fi" ;;
        esac
    done
    echo 'rm -f /tmp/tbinst.tar'
    echo 'echo installed: '"$*"
} | bash "$HERE/tb.sh" sh
rc=$?
rm -rf "$T"
exit $rc
