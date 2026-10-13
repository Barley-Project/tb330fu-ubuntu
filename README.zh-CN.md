# tb330fu-ubuntu

在 **联想 Lenovo Tab M11（TB330FU，联发科 MT6768 / Helio G85）** 上，从内置 eMMC 启动 **Ubuntu 24.04 + GNOME（Wayland）**，内核直接使用官方 ROW074 固件里的
Android 15 内核（`6.6.30-android15-8`，GKI）。目前显示、GPU、触摸与屏幕键盘、Wi‑Fi、蓝牙、电池、USB、电源/音量键、背光、CPU 调频都能用；
音频、摄像头、休眠唤醒还不行。

> English summary: [README.md](README.md)。详细文档在 [`docs/`](docs/)（中文）。

**这是只在作者自己那台平板上跑过的实验性项目**：需要已解锁的 bootloader，会替换 `vendor_boot` 和 `userdata`（**安卓用户数据会被清空**）。
刷机前请先读 [docs/08-flash-and-recovery.md](docs/08-flash-and-recovery.md)。

## 现状

| 项目 | 状态 |
|---|---|
| 内置 eMMC 启动（CN lk + CN dtbo + ROW tee + CN gz 这套组合，官方 ROW074 debug 内核） | 可用：自制 `vendor_boot`（stage1、设备树表、INT_POL 修正、开机控制台、持久日志） |
| 显示 | 可用：`simpledrm` 接管 LK 的 framebuffer；横屏（面板方向）、90 Hz 模式、无撕裂的三缓冲扫描环、背光（backlight 设备）、息屏/亮屏 |
| GPU | 可用：Mali‑G52 走 Panfrost（自包含模块，通过 `simpledrm` 的 kmsro 路径），GNOME 用 GPU 合成 |
| 触摸 + 屏幕键盘 | 可用（用合成触摸验证过，GNOME 屏幕键盘能弹出）：uinput 代理把触摸屏重新发布成“干净”的触摸设备 |
| Wi‑Fi / 蓝牙 | 可用：MT6768 CONNSYS 由用户态拉起（用 Python 重写了安卓的 WMT loader/launcher），5 GHz `wlan0`、`hci0` |
| 电池、USB（NCM + 串口 gadget、OTG 主机） | 可用 |
| 电源键 / 音量键 | 可用：PMIC 键驱动 + 键盘矩阵驱动；短按息屏/亮屏，长按弹电源菜单 |
| CPU 调频 | 可用：厂商 `cpufreq_v2`（SSPM + `CPU_DVFS`），A55 500–1800 MHz、A75 850–2000 MHz（bootloader 默认把 CPU 锁在 ~1.1 GHz） |
| GNOME 会话 | 可用：不用显示管理器，`tb330fu-gnome.service`，可选 Plasma 回退 |
| 音频、摄像头、休眠、传感器、SD 卡、SoC 温度传感器 | 没做 / 不可用，见 [docs/10-known-issues.md](docs/10-known-issues.md) |
| Waydroid | **这个内核上不可能**（没有 PID/IPC/用户命名空间，没有 SYSV IPC） |

## 整体结构

```
preloader -> LK(CN) -> 内核(ROW074 debug, gda) + vendor_boot --stage1--> switch_root --> userdata(p52) 里的 Ubuntu
                              |                                    systemd: tb330fu-devd(设备节点) tb330fu-gpu tb330fu-conn(Wi-Fi/BT)
                              |                                    tb330fu-cpufreq tb330fu-keys/keysd tb330fu-gnome(GNOME Wayland 会话)
                              +-- vendor-boot/make-dtb.sh 改过的设备树表、INT_POL 模块、开机控制台、持久日志
```

内核是**未改动的厂商内核**；其余全部是针对它的头文件（Android clang r510928）编译的树外模块，并绕过 GKI 的模块保护加载（见 [docs/02-boot-and-kernel.md](docs/02-boot-and-kernel.md)）。

## 目录

| 路径 | 内容 |
|---|---|
| `kernel-modules/` | 为本项目写的内核模块源码：`simpledrm`（含背光与扫描环）、`panfrost` 电源时序、`gkiunlock`、INT_POL、开机控制台、持久日志、调试辅助模块 |
| `vendor-boot/` | `stage1.sh`、模块清单、设备树补丁、**无版本号的 `vendor_boot` 构建器**（能逐字节复现平板上刷着的那个镜像） |
| `rootfs/` | overlay（systemd 单元、脚本、dconf、udev）、包清单、从零构建脚本、镜像打包、在线抓取 |
| `scripts/` | 内核准备（`kernel/`）、模块构建、校验工具（`verify/`：导入符号/CRC/kCFI/加载模拟）、分析工具（`research/`） |
| `tools/` | PC 端：`tb.sh` 远程调试环（USB‑NCM 上 ssh、截屏、fastboot）、探测与测试小工具 |
| `docs/` | 文档 |
| `third_party/mkbootimg/` | AOSP 的 boot 镜像工具（Apache‑2.0），未修改 |

## 快速开始（构建）

需要**你自己的 ROW074 固件**（`boot.img`、`boot-debug.img`、`vendor_boot-debug.img`，以及 `vendor` / `vendor_dlkm` 里的文件），仓库里不含任何固件内容。在 Linux 或 WSL2 里：

```bash
cp config.example.sh config.sh                 # 设置 TB_WORK、TB_FIRMWARE 等
scripts/kernel/fetch-toolchain.sh              # Android clang r510928
scripts/kernel/fetch-kernel-source.sh          # 固定提交的 kernel/common
scripts/kernel/extract-kernel-data.sh          # 从你的 boot 镜像提取 config、导出表、Module.symvers
scripts/kernel/prepare-kernel-tree.sh          # 生成 Kbuild 输出目录
scripts/kernel/fetch-upstream.sh               # NevedomaZverushka（init.c、tb330fu-devd）
scripts/build-busybox.sh
scripts/build-modules.sh all                   # 内核模块
vendor-boot/build-vendor-boot.sh               # -> dist/vendor_boot.img
rootfs/collect-vendor-files.sh <vendor/firmware> <vendor_dlkm/lib/modules>
sudo -E rootfs/build-rootfs.sh && rootfs/make-image.sh    # -> dist/rootfs.ext4
```

详细步骤、刷机与恢复见 [docs/09-build.md](docs/09-build.md)、[docs/08-flash-and-recovery.md](docs/08-flash-and-recovery.md)。

## 验证情况（如实说明）

* 用当初刷入镜像的同一组输入（`TB_STAGE_*MODE=0777`），`build-vendor-boot.sh` **逐字节复现**了平板上的 `vendor_boot`（sha256 `dfc2847656d07d8d780ec745732bd286099375e1667167f58065cb66193fac6e`）。
* `build-modules.sh` 复现了此前验证过的模块构建（除 GNU build‑id——它取决于构建目录——以及两个后来改过源码文字的模块外完全一致）；每个模块都对照调试内核的导出表检查了导入符号、CRC、kCFI 类型哈希。
* `rootfs/build-rootfs.sh` **没有**完整跑通过；平板上的系统是在运行中的系统里装包后用 `capture-live.sh` 抓取出来的。
* 用当前源码构建的 `vendor_boot`（带背光的新 `simpledrm`）只通过了构建器自带的自动检查，**没有刷机验证**。

## 致谢与许可

`init`/stage1 思路、`tb330fu-devd` 和 USB gadget 脚本来自 [NevedomaZverushka](https://github.com/feliksyasnopolski/nevedoma-zverushka)（GPL‑3.0‑or‑later）。
内核模块源码为 GPL‑2.0（派生自 Linux 的 `simpledrm` 与 Panfrost），其余为 GPL‑3.0‑or‑later，见 [LICENSE](LICENSE)、[LICENSES/](LICENSES/)、[NOTICE.md](NOTICE.md)。
本仓库不分发任何专有固件、厂商模块或镜像。
