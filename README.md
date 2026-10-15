# tb330fu-ubuntu

Ubuntu 24.04 with GNOME (Wayland) on the **Lenovo Tab M11 (TB330FU, MediaTek MT6768 / Helio G85)**, booted from the internal eMMC on top of the Android 15
kernel of the ROW074 firmware (`6.6.30-android15-8`, GKI), optionally rebuilt from source with a few extra options. Display, GPU, touch, on-screen keyboard,
Wi-Fi, Bluetooth, battery, USB, hardware keys, backlight, CPU frequency scaling and Waydroid work; audio, camera and suspend do not (yet).

> 中文说明: [README.zh-CN.md](README.zh-CN.md) — the detailed documentation in [`docs/`](docs/) is written in Chinese.

**This is experimental work on one device.** It needs an unlocked bootloader, replaces `vendor_boot` and `userdata` (all Android user data is erased) and was only
ever run on the author's tablet. Read [docs/08-flash-and-recovery.md](docs/08-flash-and-recovery.md) before flashing anything.

## What works

| area | state |
|---|---|
| Boot from internal eMMC (CN lk + CN dtbo + ROW tee + CN gz chain, stock ROW074 debug kernel) | works: own `vendor_boot` (stage1, device tree table, INT_POL fix, boot console, persistent log) |
| Display | works: `simpledrm` on LK's framebuffer, landscape (panel orientation), 90 Hz mode, tear-free 3-buffer scan-out ring, backlight class device, screen off/on |
| GPU | works: Mali-G52 via Panfrost (self-contained module, kmsro path through `simpledrm`), GNOME composites with the GPU |
| Touch + on-screen keyboard | works (verified with synthetic touch; GNOME OSK appears): uinput proxy gives mutter a clean touchscreen |
| Wi-Fi / Bluetooth | works: MT6768 CONNSYS brought up from user space (Python replacements of the Android WMT loader/launcher), `wlan0` 5 GHz, `hci0` |
| Battery, USB (NCM + serial gadget, OTG host) | works |
| Power / volume keys | works: PMIC + keypad drivers; short press = screen off/on, long press = shutdown dialog |
| CPU frequency scaling | works: vendor `cpufreq_v2` stack (SSPM + `CPU_DVFS`), A55 500–1800 MHz, A75 850–2000 MHz (the bootloader leaves ~1.1 GHz) |
| GNOME session | works: no display manager, `tb330fu-gnome.service`, optional Plasma fallback |
| Audio, camera, suspend/resume, sensors, SD card, SoC temperature sensor | not working / not done — see [docs/10-known-issues.md](docs/10-known-issues.md) |
| Own kernel (optional) | works: built from the pinned `kernel/common` commit with the debug kernel's configuration plus [kernel-config/tb330fu.config](kernel-config/tb330fu.config) (PID/IPC/user namespaces, Bluetooth BNEP, squashfs, zram, ...); every exported symbol keeps its CRC, so the vendor modules still load |
| Waydroid | works with the own kernel: Android 13 (LineageOS 20) in an LXC container, drawn by the Panfrost GPU; no sound (no audio yet). See [docs/12-waydroid.md](docs/12-waydroid.md) |

## How it fits together

```
preloader -> LK (CN) -> kernel (ROW074 debug, gda) + vendor_boot  --stage1-->  switch_root  -->  Ubuntu root on /dev/mmcblk0p52 (userdata)
                                |                                              systemd: tb330fu-devd (device nodes), tb330fu-gpu, tb330fu-conn (Wi-Fi/BT),
                                |                                              tb330fu-cpufreq, tb330fu-keys/keysd, tb330fu-gnome (GNOME Wayland session)
                                +-- device tree table patched by vendor-boot/make-dtb.sh, INT_POL helper module, boot console, persistent log
```

The kernel is either the **unmodified vendor kernel** (`boot-debug.img` of the firmware) or, for Waydroid, the same kernel rebuilt from source with a small
configuration fragment ([kernel-config/](kernel-config/)); `scripts/kernel/crc-compare.py` checks that no exported symbol changes its CRC, so the vendor modules
still load. Everything else is built as out-of-tree modules against its headers (compiler: Android clang r510928) and loaded over the GKI module protection
(see [docs/02-boot-and-kernel.md](docs/02-boot-and-kernel.md)).

## Repository layout

| path | contents |
|---|---|
| `kernel-config/` | Kconfig fragments for the self-built kernel: `tb330fu.config` (containers, Bluetooth, squashfs, ...); `tb330fu-sukisu.config` is an experiment that does **not** boot |
| `kernel-modules/` | sources of the kernel modules written for this port (`simpledrm` + backlight + scan-out ring, `panfrost` power sequencing, `gkiunlock`, INT_POL helper, boot console, persistent log, `spmstub` stand-in for the audio driver, debug helpers) |
| `vendor-boot/` | `stage1.sh`, module lists, device tree patching and the **version-free `vendor_boot` builder** (reproduces the image that is flashed on the tablet byte for byte) |
| `rootfs/` | overlay (systemd units, scripts, dconf, udev), package list, from-scratch builder, image maker, live-system capture |
| `scripts/` | kernel preparation and the kernel / `boot.img` build (`kernel/`), module build, verification tools (`verify/`: imports, CRCs, kCFI, load simulation), analysis tools (`research/`) |
| `tools/` | PC side: `tb.sh` remote debug loop (ssh over USB-NCM, screenshot, fastboot), the Waydroid image downloader, probes and test helpers |
| `docs/` | documentation (Chinese) |
| `third_party/mkbootimg/` | AOSP boot image tools (Apache-2.0), unmodified |

## Quick start (build)

You need your **own copy of the ROW074 firmware** (`boot.img`, `boot-debug.img`, `vendor_boot-debug.img`, and the `vendor` / `vendor_dlkm` contents).
Nothing from it is in this repository. On Linux or WSL2:

```bash
cp config.example.sh config.sh                 # set TB_WORK, TB_FIRMWARE, ...
scripts/kernel/fetch-toolchain.sh              # Android clang r510928
scripts/kernel/fetch-kernel-source.sh          # kernel/common at the pinned commit
scripts/kernel/extract-kernel-data.sh          # config, exports, Module.symvers from your boot images
scripts/kernel/prepare-kernel-tree.sh          # Kbuild output directory
scripts/kernel/fetch-upstream.sh               # NevedomaZverushka (init.c, tb330fu-devd)
scripts/build-busybox.sh
scripts/build-modules.sh all                   # kernel modules
# optional, needed for Waydroid: the kernel rebuilt with kernel-config/tb330fu.config (details: docs/09-build.md, section 1b)
scripts/kernel/build-kernel.sh tb330fu kernel-config/tb330fu.config
scripts/kernel/build-boot.sh "$TB_WORK/kernel/build-tb330fu/arch/arm64/boot/Image"    # -> dist/boot.img
scripts/kernel/stage-modules.sh tb330fu        # -> dist/kernel-modules.tar, unpacked by build-rootfs.sh
vendor-boot/build-vendor-boot.sh               # -> dist/vendor_boot.img
rootfs/collect-vendor-files.sh <vendor/firmware> <vendor_dlkm/lib/modules>
sudo -E rootfs/build-rootfs.sh && rootfs/make-image.sh    # -> dist/rootfs.ext4
```

Details, flashing and recovery: [docs/09-build.md](docs/09-build.md), [docs/08-flash-and-recovery.md](docs/08-flash-and-recovery.md).

## Verification status

* `build-vendor-boot.sh` with the inputs of the originally flashed image (`TB_STAGE_*MODE=0777`) reproduces it **bit for bit** (sha256 `dfc2847656d07d8d780ec745732bd286099375e1667167f58065cb66193fac6e`).
* `build-modules.sh` reproduces the earlier verified module builds (identical apart from the GNU build-id, which depends on the build directory, and two
  modules whose source text changed since). Every module is checked against the debug kernel's export table (imports, CRCs, kCFI type hashes).
* `rootfs/build-rootfs.sh` has **not** been run end to end; the tablet's image was produced by installing the packages on the running system and capturing it.
* The self-built kernel (`kernel-config/tb330fu.config`) and a `vendor_boot` built from the current sources were flashed and run on the author's tablet;
  `crc-compare.py` found no changed export CRC against the debug kernel. The SukiSU-Ultra variant (`tb330fu-sukisu.config`) builds with the same CRC result but
  did **not** boot (the tablet was unreachable; restored with fastboot, cause not found).

## Credits and licenses

Based on [NevedomaZverushka](https://github.com/feliksyasnopolski/nevedoma-zverushka) (GPL-3.0-or-later) for the `init`/stage1 concept, `tb330fu-devd` and the
USB gadget helper. Kernel module sources are GPL-2.0 (derived from the Linux kernel's `simpledrm` and Panfrost drivers), the rest is GPL-3.0-or-later. See
[LICENSE](LICENSE), [LICENSES/](LICENSES/) and [NOTICE.md](NOTICE.md). No proprietary firmware, vendor modules or images are distributed.
