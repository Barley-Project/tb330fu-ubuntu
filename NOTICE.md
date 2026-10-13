# Notice

## Third-party material

| component | where | license |
|---|---|---|
| NevedomaZverushka (`init.c`, `tb330fu-devd`, stage1 and USB gadget concept) | fetched at build time (`scripts/kernel/fetch-upstream.sh`, pinned commit); `vendor-boot/stage1.sh` and `rootfs/overlay/usr/libexec/tb330fu-usb-gadget` are derived from it | GPL-3.0-or-later |
| Linux kernel (`simpledrm`, DRM helpers, Panfrost, DRM scheduler, fonts, logo) | the module build copies the sources from your kernel tree and patches them (`kernel-modules/simpledrm/patch_simpledrm.py`, `kernel-modules/panfrost/patch_panfrost.py`); `kernel-modules/con/fonts.h` and `tux80.h` are generated from the kernel's font and logo sources | GPL-2.0 |
| AOSP `mkbootimg.py`, `unpack_bootimg.py`, `gki/` | `third_party/mkbootimg/`, unmodified | Apache-2.0 |
| Android clang r510928, kernel/common | downloaded by the scripts | their own licenses |
| Ubuntu 24.04 packages, BusyBox | downloaded / built by the scripts | their own licenses |

## Not included

The repository contains **no** firmware, vendor kernel modules, device images, factory calibration data (Wi-Fi/Bluetooth NVRAM, addresses), keys or
passwords. Everything proprietary is taken from the owner's own copy of the firmware at build time (`rootfs/vendor/*.list` names the files).

"Lenovo", "Tab M11", "MediaTek", "Helio" and "Android" are trademarks of their owners; this project is not affiliated with them.
