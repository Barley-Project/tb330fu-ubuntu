# 08 刷机与恢复

> **风险**：把根文件系统写进 `userdata` 会**清空安卓用户数据**；`vendor_boot` 写错设备会进不了系统（还能进 fastboot/BROM 救）。
> 这套流程只在作者的一台 TB330FU 上做过。没有备份就不要动手。

## 前提（本仓库不做这些）

* Bootloader 已解锁，引导链是 **CN `lk` + CN `dtbo` + ROW `tee` + CN `gz`，其余为 ROW074**（作者在 [Barley-Project/lk_patch](https://github.com/Barley-Project/lk_patch)、`mtkclient-tb330fu-carbonara` 里做的研究与工具）；
  `vbmeta` 已被绕过（本项目不刷 `vbmeta`，也不刷 `lk/tee/gz/dtbo`）。
* `boot` 分区里是官方 ROW074 的 **`boot-debug.img`**（gda 内核），或本仓库自编的内核（见下面“自编内核”，版本字符串与导出 CRC 和 gda 相同）。`vendor_boot` 必须与它配对：**gda 的 vendor_boot 绝不能和 g94 的标准 `boot.img` 混用**。
* 分区布局（作者设备的 GPT 备份）：`vendor_boot_a/b` 各 64 MiB（镜像已补零到 64 MiB）、`userdata` = p52（约 110 GB）、`expdb` = p6、`nvdata`、`spmfw`、`sspm` 等保持原样。

## 刷机

先备份：当前槽的 `boot`、`vendor_boot`、`dtbo`（原厂回退镜像），并记录 SHA‑256；没有 `userdata` 备份就无法找回安卓数据。

```bash
fastboot getvar current-slot                 # 记为 a 或 b；也核对 product、unlocked、partition-size:vendor_boot_a(0x4000000)
fastboot flash vendor_boot_a dist/vendor_boot.img      # 当前槽
fastboot flash userdata dist/rootfs.ext4               # 清空 userdata！镜像约 6 GiB 稀疏文件，fastboot 会自动分片
fastboot reboot
```

`tools/tb.sh flash <分区> <镜像>` 只允许白名单分区（`vendor_boot_a/b`、`userdata`、`boot_a/b`），`tb.sh cycle` = 进 fastboot → 刷 → 重启 → 等 ssh。
首次启动时 `tb330fu-growfs` 会把 ext4 在线扩到整个 userdata。root 的选择靠 **ext4 标签 + UUID + 容量**（`stage1.conf`：`TB_ROOT_LABEL/TB_ROOT_UUID`，两个构建器必须用同一组值；
`make-image.sh` 会把 UUID 写进文件系统，不是随机的）。

## 自编内核（`boot`，Waydroid 需要）

`scripts/kernel/build-boot.sh` 把固件里 `boot-debug.img` 的内核换成自编的 `Image`（头版本 4、gzip 内核、原 ramdisk、补零到 32 MiB）；`--self-test` 先证明“把原内核原样重打包 = 原镜像”（AVB 页脚除外，设备跑在绕过校验的状态下）。只刷当前槽的 `boot`：

```bash
fastboot flash boot_a dist/boot.img          # 回退：fastboot flash boot_a <你自己固件里的 boot-debug.img>
```

刷之前先确认：能进 fastboot、手上有原来的 `boot-debug.img`、`crc-compare.py` 显示没有改变的导出 CRC。**这些都满足也不保证能启动**：加了 SukiSU‑Ultra 的那个内核编译通过、CRC 也一致，刷进去却连不上平板（原因没查），靠 fastboot 刷回原镜像才恢复。
实测启动失败后 `current-slot` 仍然是 a，不会自动切到 b 槽。

## 启动后

* 约 40–60 秒：桌面起来；USB 线接 PC 后得到 `192.168.7.2`（见 [11-dev-loop.md](11-dev-loop.md)）。
* 没有可用的账号密码时：`rootfs/build-rootfs.sh` 默认**锁定账号**，需要 `TB_SSH_PUBKEY`（ssh 密钥）或 `TB_PASSWORD`。
* 屏幕上看到开机控制台一直滚日志（没有桌面）：GNOME/GPU 没起来，用 ssh 看 `journalctl -b`、`dmesg | grep TB330FU`。

## 恢复

| 情况 | 办法 |
|---|---|
| 系统起不来，但还能进 fastboot（音量/按键组合，或 `tb330fu-fastboot`） | 刷回备份的原厂 `vendor_boot_a`（或上一个能用的镜像） |
| 刷了新的 `boot` 之后起不来 | fastboot 把原来的 `boot-debug.img` 刷回同一个槽（`fastboot flash boot_a …`）；别指望自动回退到另一个槽 |
| 某个驱动让内核 panic | 每个高风险步骤都有崩溃保护：下次启动自动跳过一次。也可以在平板上 `touch /var/lib/tb330fu/no-gpu`（`no-conn`、`no-cpufreq`、`no-keys`、`no-otg`）永久跳过 |
| `tb330fu-gpu` 失败后没有显示驱动 | 脚本会回退到 `simpledrm.ko.good`；手工：`insmod /usr/lib/tb330fu/modules/simpledrm.ko.good`，再启动会话 |
| GNOME 反复起不来 | 三次失败后自动切到 Plasma（若已安装）；或 `touch /var/lib/tb330fu/use-plasma` |
| fastboot 也进不去 | BROM + `mtkclient`（作者的 `mtkclient-tb330fu-carbonara`），读/写 `expdb`、刷 `vendor_boot` |
| 要看崩溃日志 | 平板活着时读 `/dev/mmcblk0p6`（expdb）用 `kernel-modules/blklog/parse_blklog.py` 解析；`journalctl -b -1` 只有 panic 之前的用户态日志 |

**日志的陷阱**：LK 的 `kedump` 只在**下一次** LK 启动时才把 panic 控制台抄进 `expdb`；你在重启前就断电或进 BROM，读到的就是旧日志。判断一份 dump 属于哪个镜像：LK 行
`booting linux … ramdisk @ … (N)` 里 N = vendor ramdisk 总大小 + 3022350。

## 刷机前的自检清单

1. `scripts/verify/loadsim.py` 对整条加载序列（构建器已自动跑）；2. vermagic 是 gda；3. 所有 `modules.load.*` 里的模块在解包后都存在（构建器已检查）；
4. 没有 Himax 固件/触摸 IC 刷写材料进 ramdisk；5. 新驱动加载前问自己：它的中断极性依赖 INT_POL 吗？I2C 一类低有效线在 INT_POL 没写之前会风暴；6. 要有一个能回退的 vendor_boot。
