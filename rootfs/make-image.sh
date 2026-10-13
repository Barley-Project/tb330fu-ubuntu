#!/bin/bash
# Turn a root filesystem tree (rootfs/build-rootfs.sh) or a tar of a captured system (rootfs/capture-live.sh) into the flashable ext4 image.
#
# usage: make-image.sh [<tree dir | tar file>]      default: $TB_ROOTFS_WORK/tree
# env:   TB_ROOTFS_SIZE   image size (default 6G, sparse; the filesystem is grown to the whole userdata partition at the first boot by tb330fu-growfs)
#        TB_ROOT_LABEL / TB_ROOT_UUID  must be the values the vendor_boot image looks for (stage1 selects the root by label, UUID and size)
# Output: $TB_DIST/rootfs.ext4 (+ .sha256).  Needs mke2fs/e2fsck (e2fsprogs) and root only if the tar contains device nodes.
set -euo pipefail
. "$(dirname "$0")/../scripts/lib/common.sh"
need mke2fs e2fsck truncate tar
SRC=${1:-$TB_ROOTFS_WORK/tree}
SIZE=${TB_ROOTFS_SIZE:-6G}
OUT=$TB_DIST/rootfs.ext4
W=$TB_ROOTFS_WORK/image; rm -rf "$W"; mkdir -p "$W" "$TB_DIST"
if [ -d "$SRC" ]; then T=$SRC; else T=$W/tree; mkdir -p "$T"; tar xpf "$SRC" -C "$T" --numeric-owner --xattrs --acls; fi
# directories that are not part of a capture but must exist
mkdir -p "$T"/{proc,sys,dev,run,tmp,mnt,media} "$T/var/cache/apt/archives/partial" "$T/var/lib/apt/lists/partial" "$T/var/log/journal"
chmod 1777 "$T/tmp"
echo "tree: $(du -sh "$T" | cut -f1)"
truncate -s "$SIZE" "$W/rootfs.ext4"
mke2fs -q -t ext4 -L "$TB_ROOT_LABEL" -U "$TB_ROOT_UUID" -b 4096 -m 0 -N 400000 \
    -O has_journal,ext_attr,resize_inode,dir_index,filetype,extent,64bit,flex_bg,sparse_super,large_file,huge_file,dir_nlink,extra_isize,metadata_csum,^orphan_file,^metadata_csum_seed \
    -E lazy_itable_init=1 -d "$T" "$W/rootfs.ext4"
e2fsck -fn "$W/rootfs.ext4" | tail -2
cp --sparse=always "$W/rootfs.ext4" "$OUT"
( cd "$TB_DIST" && sha256sum rootfs.ext4 > rootfs.ext4.sha256 && cat rootfs.ext4.sha256 )
echo "label $TB_ROOT_LABEL, uuid $(blkid -s UUID -o value "$OUT" 2>/dev/null || echo "$TB_ROOT_UUID")"
