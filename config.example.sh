# Copy to config.sh (git-ignored) and edit. Every variable has a default in scripts/lib/common.sh; set only what differs on your machine.

# Working directory for downloads and build outputs (kernel source, toolchain, built modules, images in progress). Needs ~15 GB.
#TB_WORK=$HOME/tb330fu-work

# Your own ROW074 firmware images: boot.img, boot-debug.img, vendor_boot-debug.img, dtbo.img (and the vendor / vendor_dlkm
# partition contents for the Wi-Fi and Bluetooth modules and firmware). These are never part of this repository.
#TB_FIRMWARE=$HOME/firmware/row074

# Where the finished images are written (default: <repo>/dist, git-ignored).
#TB_DIST=$PWD/dist

# Root filesystem identity (stage1 looks for it by label, UUID and size). Change both builders together.
#TB_ROOT_LABEL=TB330FU_KDE
#TB_ROOT_UUID=7292ba20-d80a-45be-8002-000000000024

# Rootfs build: the account that is created and how it can be reached. Leave the password empty to keep the account locked (ssh key only).
#TB_USER=ubuntu
#TB_PASSWORD=
#TB_SSH_PUBKEY=$HOME/.ssh/id_ed25519.pub

# Optional HTTP(S) proxy for apt / git / curl during builds.
#TB_PROXY=http://127.0.0.1:7890
