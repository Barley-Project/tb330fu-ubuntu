# 01 概览：硬件、启动链、组件

## 设备与固件组合

* 联想 Tab M11（型号 TB330FU，代号 Barley），SoC MT6768（2×Cortex‑A75 + 6×Cortex‑A55，Mali‑G52 MC2），8 GB 内存，eMMC 128 GB（HS400）。
* 面板 BOE `hx83102j`，原生**竖屏** 1200×1920，DSI 4 通道视频模式 + MT6382 桥接 + DSC，实际刷新率 **90 Hz**（htotal 1405 × vtotal 2058，像素时钟 260.234 MHz）。
* 触摸 Himax HX83102J（SPI），充电 IC Southchip SC89890H，电量计 mm8013，Type‑C 控制器 CPS8851 或 AW35615，Wi‑Fi/BT 为 SoC 内置 CONNSYS（A‑die MT6631）。
* 引导组合（用户确认）：**CN `lk` + CN `dtbo` + ROW `tee` + CN `gz`，其余是 ROW074**；内核是 ROW074 的 debug 内核 `6.6.30-android15-8-gda510ce0fdcf-4k`
  （ABI 与 `boot.img` 里的生产内核 `…-g94c1a24cabd5-ab12293704-4k` 一致，所以可以用 g94 的头文件编译、再把 vermagic 改成 gda）。
* 内核由 GenieZone（GZ）hypervisor 在 EL1 下运行——设备树里有 `/hypervisor` 节点，systemd 因此把系统判成 `vm-other`，见 [04-input-keys.md](04-input-keys.md)。

## 启动链

```
preloader -> LK(CN) --读 vendor_boot--> 内核 + 设备树表(+CN dtbo 由 LK 叠加) + 初始 ramdisk(3 个片段)
   ramdisk 片段 0：官方平台片段（厂商模块，原样）    片段 1：recovery（原样）    片段 2：本项目的 stage1（init、BusyBox、stage1.sh、模块）
内核命令行里 rdinit=/tb330fu-init -> stage1：加载早期模块 -> INT_POL 修正 -> 显示/控制台模块 -> 找到 eMMC 上的根(标签+UUID+容量) -> switch_root
```

* **设备树表**：LK 只接受特定形式（见 [02-boot-and-kernel.md](02-boot-and-kernel.md)）；`vendor-boot/make-dtb.sh` 从官方表派生。
* **stage1**（`vendor-boot/stage1.sh`，约 40 KB 的 busybox sh）：挂载伪文件系统、`/dev/null` 先于任何重定向创建（否则会变成普通文件，所有 `PrivateDevices=yes` 的 unit 都会 226/NAMESPACE）、
  加载 `modules.load.native`（37 个厂商模块）、`tb330fu_intpol`、`modules.load.display`（ovl、con、fb、simpledrm）、`tb330fu_blklog`、触摸与 reboot‑mode 等 `modules.load.extra`，
  然后在 `/newroot/run` 的 tmpfs 里装一个“只存在于内存”的 Plasma 会话单元和 OTG 单元（根文件系统本身不被 stage1 修改）。
* 根文件系统：`userdata`（p52）里的 ext4，**标签 `TB330FU_KDE`**（历史名字，第一版是 KDE 镜像；现在是 GNOME，两个构建器用同一对变量 `TB_ROOT_LABEL/TB_ROOT_UUID`）。

## 用户态组件（`rootfs/overlay`）

| 单元 | 作用 |
|---|---|
| `tb330fu-devd` | 内核没有 devtmpfs，这个守护进程按 uevent 建设备节点（来自上游项目） |
| `tb330fu-gpu` | 必要时换上新版 `simpledrm`，加载 `panfrost`，等 `renderD128`，再把 DRM 驱动名改成 `mediatek`（让 Mesa 走 Panfrost） |
| `tb330fu-cpufreq` / `tb330fu-thermalguard` | CPU 调频栈 / 用电池温度做保守降频保护（[06-cpu-power.md](06-cpu-power.md)） |
| `tb330fu-keys` / `tb330fu-keysd` | 电源/音量键驱动 / 电源键策略（短按息屏、长按菜单） |
| `tb330fu-conn` + `tb330fu-wmt-launcher` + `tb330fu-btd` | Wi‑Fi 与蓝牙（[05-connectivity-usb.md](05-connectivity-usb.md)） |
| `tb330fu-otg` / `tb330fu-usb-gadget` | Type‑C/OTG 模块链 / NCM + 串口 gadget（192.168.7.2） |
| `tb330fu-gnome` / `tb330fu-touchd` | GNOME Wayland 会话（User=ubuntu，无 DM，PAM 会话）/ 触摸代理 |
| `tb330fu-growfs` | 根文件系统在线扩到整个分区（首次启动） |

## 崩溃保护约定

会让机器崩溃的步骤都有 `/var/lib/tb330fu/no-<名字>`（存在则跳过）和 `<名字>-started` 标记（上次启动没撑过去则这次跳过一次）。
标记由 `systemd-run` 定时器在成功后清除（不要用服务里的后台子进程清除）。这样“某个驱动让内核 panic”不会变成无限重启。
