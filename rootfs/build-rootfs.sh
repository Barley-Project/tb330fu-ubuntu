#!/bin/bash
# Build the root filesystem tree from scratch: Ubuntu 24.04 (noble) arm64 base + packages + this project's overlay, kernel modules and the vendor files that
# you collected from your own firmware.  NOT end-to-end tested in this form (the image that runs on the tablet was made by installing the packages on the
# running system and capturing it with rootfs/capture-live.sh); the steps are those of the scripts that produced the first images.
#
# Run as root on Linux / WSL2 with qemu-user (qemu-aarch64) and ~8 GB of free space:
#     sudo -E rootfs/build-rootfs.sh
#
# Inputs / settings (environment, see config.example.sh):
#   TB_MODOUT                 built kernel modules (scripts/build-modules.sh)
#   TB_ROOTFS_WORK            work directory (tree, downloads)         default $TB_WORK/rootfs
#   $TB_ROOTFS_WORK/vendor-files   output of rootfs/collect-vendor-files.sh (optional: without it Wi-Fi, Bluetooth, keys and CPU frequency scaling are absent)
#   TB_UPSTREAM               upstream sources for tb330fu-devd         (scripts/kernel/fetch-upstream.sh)
#   TB_USER (ubuntu)          the desktop account
#   TB_PASSWORD               its password; empty = the account stays locked (log in over ssh with TB_SSH_PUBKEY)
#   TB_SSH_PUBKEY             public key file that is installed as ~/.ssh/authorized_keys
#   TB_WITH_PLASMA=1          also install the Plasma fallback session (rootfs/packages-plasma.txt)
#   TB_DEV=1                  dev extras (apt through the PC proxy over the ssh reverse tunnel)
#   TB_APT_MIRROR             default http://ports.ubuntu.com/ubuntu-ports
#   TB_TZ                     default Asia/Shanghai
set -euo pipefail
. "$(dirname "$0")/../scripts/lib/common.sh"
[ "$(id -u)" -eq 0 ] || die "run as root (sudo -E)"
need tar curl chroot sha256sum aarch64-linux-gnu-gcc
: "${TB_USER:=ubuntu}"; : "${TB_APT_MIRROR:=http://ports.ubuntu.com/ubuntu-ports}"; : "${TB_TZ:=Asia/Shanghai}"
R=$TB_ROOTFS_WORK/tree
VF=$TB_ROOTFS_WORK/vendor-files
BASE_URL=https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/ubuntu-base-24.04.5-base-arm64.tar.gz
BASE_SHA=a91d5a93010193712d346d761372b7c9db6dfcf093893161c64ca107f05914f2
mkdir -p "$TB_ROOTFS_WORK/downloads"
[ -e "$R/.bootstrap-complete" ] && die "$R already exists; remove it for a fresh build"

note "1. Ubuntu base"
BASE=$TB_ROOTFS_WORK/downloads/$(basename "$BASE_URL")
[ -f "$BASE" ] || curl ${TB_PROXY:+--proxy "$TB_PROXY"} --fail --location --retry 3 -o "$BASE.part" "$BASE_URL" && { [ ! -f "$BASE.part" ] || mv "$BASE.part" "$BASE"; }
chk "ubuntu-base 24.04.5 arm64 tarball" "$BASE_SHA" "$(sha "$BASE")"
[ "$fail_count" -eq 0 ] || exit 2
mkdir -p "$R"; tar --numeric-owner --same-owner -xpf "$BASE" -C "$R"
cp "$(command -v qemu-aarch64 || command -v qemu-aarch64-static)" "$R/usr/bin/qemu-aarch64-static"
chroot "$R" /usr/bin/dpkg --print-architecture | grep -qx arm64 || die "the arm64 chroot does not run (qemu-user binfmt?)"

note "2. chroot preparation (no service starts inside the chroot, device nodes for apt's verifier)"
printf '#!/bin/sh\nexit 101\n' > "$R/usr/sbin/policy-rc.d"; chmod 0755 "$R/usr/sbin/policy-rc.d"
mkdir -p "$R/dev/pts" "$R/run"
for spec in null:1:3 zero:1:5 full:1:7 random:1:8 urandom:1:9 tty:5:0 console:5:1 kmsg:1:11; do
    node=${spec%%:*}; pair=${spec#*:}; maj=${pair%%:*}; min=${pair#*:}
    [ -c "$R/dev/$node" ] || { rm -f "$R/dev/$node"; mknod -m 0666 "$R/dev/$node" c "$maj" "$min"; }
done
cp /etc/resolv.conf "$R/etc/resolv.conf" 2>/dev/null || echo 'nameserver 1.1.1.1' > "$R/etc/resolv.conf"
mount -t proc proc "$R/proc"; trap 'umount "$R/proc" 2>/dev/null || true' EXIT
rm -f "$R/etc/apt/sources.list.d/ubuntu.sources"
cat > "$R/etc/apt/sources.list" <<EOF
deb $TB_APT_MIRROR noble main restricted universe multiverse
deb $TB_APT_MIRROR noble-updates main restricted universe multiverse
deb $TB_APT_MIRROR noble-security main restricted universe multiverse
EOF
[ -n "${TB_PROXY:-}" ] && printf 'Acquire::http::Proxy "%s";\nAcquire::https::Proxy "%s";\n' "$TB_PROXY" "$TB_PROXY" > "$R/etc/apt/apt.conf.d/99proxy"
sh_chroot() { chroot "$R" /usr/bin/env DEBIAN_FRONTEND=noninteractive LC_ALL=C "$@"; }

note "3. packages"
pk=$(grep -v -E '^\s*(#|$)' "$TB_ROOT/rootfs/packages.txt" | tr '\n' ' ')
[ -n "${TB_WITH_PLASMA:-}" ] && pk="$pk $(grep -v -E '^\s*(#|$)' "$TB_ROOT/rootfs/packages-plasma.txt" | tr '\n' ' ')"
sh_chroot apt-get update
# shellcheck disable=SC2086
sh_chroot apt-get install -y --no-install-recommends $pk
sh_chroot dpkg-query -W > "$TB_ROOTFS_WORK/packages.tsv"

note "4. account, locale, time zone"
sh_chroot getent passwd "$TB_USER" > /dev/null || sh_chroot useradd --create-home --uid 1000 --user-group --shell /bin/bash --groups sudo,adm,dialout,audio,video,input,plugdev,netdev "$TB_USER"
if [ -n "${TB_PASSWORD:-}" ]; then echo "$TB_USER:$TB_PASSWORD" | sh_chroot chpasswd; else sh_chroot passwd --lock "$TB_USER"; fi
if [ -n "${TB_SSH_PUBKEY:-}" ]; then
    install -d -m 0700 -o 1000 -g 1000 "$R/home/$TB_USER/.ssh"
    install -m 0600 -o 1000 -g 1000 "$TB_SSH_PUBKEY" "$R/home/$TB_USER/.ssh/authorized_keys"
fi
printf 'en_US.UTF-8 UTF-8\nzh_CN.UTF-8 UTF-8\n' > "$R/etc/locale.gen"; sh_chroot locale-gen
echo 'LANG=zh_CN.UTF-8' > "$R/etc/default/locale"
ln -sf "/usr/share/zoneinfo/$TB_TZ" "$R/etc/localtime"; echo "$TB_TZ" > "$R/etc/timezone"
echo tb330fu > "$R/etc/hostname"

note "5. overlay (services, scripts, configuration)"
cp -a "$TB_ROOT/rootfs/overlay/." "$R/"
[ -n "${TB_DEV:-}" ] && cp -a "$TB_ROOT/rootfs/overlay-dev/." "$R/"
chmod 0755 "$R"/usr/local/sbin/tb330fu-* "$R"/usr/libexec/tb330fu-* "$R"/usr/libexec/tb330fu/* 2>/dev/null || true
chown -R 1000:1000 "$R/home/$TB_USER" 2>/dev/null || true
[ "$TB_USER" = ubuntu ] || echo "NOTE: the overlay assumes the account ubuntu (uid 1000): units, tb330fu-keysd and the dconf profile name it"
# tb330fu-devd: the vendor kernel has no devtmpfs, this daemon creates the device nodes from uevents (upstream sources, GPL-3.0-or-later)
[ -f "$TB_UPSTREAM/src/devd/tb330fu-devd.c" ] || die "upstream sources missing (scripts/kernel/fetch-upstream.sh)"
aarch64-linux-gnu-gcc -static -O2 -g0 -std=c11 -Wall -Wextra -Wpedantic "$TB_UPSTREAM/src/devd/tb330fu-devd.c" "$TB_UPSTREAM/src/devd/generated_ueventd_policy.c" -o "$R/usr/local/sbin/tb330fu-devd"
install -d "$R/usr/share/tb330fu"
[ -f "$TB_KDATA/boot-debug/config" ] && install -m 0644 "$TB_KDATA/boot-debug/config" "$R/usr/share/tb330fu/kernel-config.row074"

note "6. kernel modules built by this project"
M=$R/usr/lib/tb330fu/modules; install -d "$M" "$R/usr/lib/tb330fu/cpufreq"
for m in simpledrm panfrost tb330fu_gkiunlock tb330fu_shot tb330fu_peek; do install -m 0644 "$TB_MODOUT/$m.ko" "$M/$m.ko"; done
install -m 0644 "$TB_MODOUT/simpledrm.ko" "$M/simpledrm.ko.good"      # fallback of tb330fu-gpu.sh if a newer simpledrm.ko is ever refused
install -m 0644 "$TB_MODOUT/tb330fu_ppmtemp.ko" "$R/usr/lib/tb330fu/cpufreq/"

note "7. vendor files from your firmware"
if [ -d "$VF" ]; then
    install -d "$R/vendor/firmware" "$R/usr/lib/tb330fu/conn" "$R/usr/lib/tb330fu/keys"
    cp -a "$VF/firmware/." "$R/vendor/firmware/"; cp -a "$VF/conn/." "$R/usr/lib/tb330fu/conn/"
    cp -a "$VF/keys/." "$R/usr/lib/tb330fu/keys/"; cp -a "$VF/cpufreq/." "$R/usr/lib/tb330fu/cpufreq/"
    chmod 0644 "$R"/usr/lib/tb330fu/*/*.ko
else
    echo "WARNING: $VF not found: no Wi-Fi, Bluetooth, power/volume keys or CPU frequency scaling (see rootfs/collect-vendor-files.sh)"
fi

note "8. enable services, defaults"
for u in tb330fu-devd tb330fu-usb-gadget tb330fu-growfs tb330fu-ssh-hostkeys tb330fu-gpu tb330fu-conn tb330fu-otg tb330fu-keys tb330fu-keysd tb330fu-cpufreq \
         tb330fu-thermalguard tb330fu-gnome tb330fu-touchd systemd-networkd NetworkManager ssh; do
    systemctl --root="$R" enable "$u.service" 2>&1 | tail -1 || true
done
systemctl --root="$R" set-default graphical.target
sh_chroot dconf update
rm -f "$R/etc/machine-id" "$R/var/lib/dbus/machine-id"; : > "$R/etc/machine-id"
rm -f "$R"/etc/ssh/ssh_host_*
sh_chroot apt-get clean
rm -f "$R/usr/sbin/policy-rc.d" "$R/usr/bin/qemu-aarch64-static" "$R/etc/apt/apt.conf.d/99proxy"
umount "$R/proc"; trap - EXIT
touch "$R/.bootstrap-complete"
echo "tree ready: $R ($(du -sh "$R" | cut -f1)); next: rootfs/make-image.sh"
