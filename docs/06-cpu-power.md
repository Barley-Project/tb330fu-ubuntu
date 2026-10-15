# 06 CPU 调频、热保护、性能

## 问题

官方固件里 CPU 的频率由 SSPM/CSPM 里的 DVFS 栈管；本项目的启动链没有加载它，**bootloader 把 CPU 留在一个固定的工作点**：
6 个 A55（cpu0‑5）约 **1175 MHz**、2 个 A75（cpu6‑7）约 **1087 MHz**，电压 0.8 V，内核里又没有 cpufreq 驱动。厂商表里的上限是 A55 1800 MHz、A75 2000 MHz，也就是只跑到 54–65%。
症状：apt 解包/建缓存、dpkg 触发器、浏览器都慢一倍左右。（空闲时系统其实是健康的：合成器 86–134 fps，eMMC 197/318 MB/s 写/读，Wi‑Fi 5 GHz 175/390 Mbit/s。）
另外 `uptime` 的 load average 常驻 ~9 是假象：8 个厂商看门狗线程 `wdtk-0..7` 和 `hang_detect` 永远处于 D 状态，不占 CPU。

## 解决：厂商 `cpufreq_v2`（混合 DVFS）

`CPU_DVFS.ko` 来自 `kernel_device_modules-6.6/drivers/misc/mediatek/cpufreq_v2/src/plat_k68/`：内核只通过 IPI 把 OPP 表交给 **SSPM 固件**（预加载器阶段就启动了，旧 LK 日志里有 `[BLDR] SSPM Start`、`tinysys-sspm` 镜像校验），
PLL 与电压的切换由 SSPM + 硬件 CSPM（`dvfsp@10227000`，OPP 表在 CSRAM `0x00110c00`）完成，`CPU_DVFS` 再注册 cpufreq 策略。设备树 `dvfsp@00110c00` 里有 `B-table`（A75：2000…850 MHz）、`L-table`（A55：1700…500 MHz）、`CCI-table`。

按依赖顺序加载（`rootfs/vendor/cpufreq-modules.list`，全部来自 `vendor_dlkm`，模块间符号都能解析，`scripts/verify/dep_closure.py` 算出的闭包）：

```
emimpu pmic_lbat_service pmic_lvsys_notify mtk_low_battery_throttling      (与连接栈共用)
sspm_v1  -> 日志 "SSPM is ready to service IPI"          (SSPM 固件确实在运行)
mtk_cpuhp mtk_mdpm_v1 mtk_dynamic_loading_throttling mtk_pbm mtk_battery_oc_throttling mediatek_static_power Upower mtk_ppm_v3
CPU_DVFS -> /sys/devices/system/cpu/cpufreq/policy0 (cpu0-5, 500..1800 MHz) 与 policy6 (cpu6-7, 850..2000 MHz)，schedutil
tb330fu_ppmtemp                                          (见下)
```

实测（8 个核同时满载）：A55 ≈ 1795 MHz、A75 ≈ 1997 MHz；`vproc11 = 1.0875 V`、`vproc12 = 1.006 V`（电压随频率联动）；空闲时小核降回 500 MHz。Python 5M 次循环在大核上从 0.42 s 变成 0.23 s。
`tb330fu-cpufreq.service` 在开机早期（`Before=tb330fu-gpu/conn/gnome`）加载整条链，带 `no-cpufreq`/`cpufreq-started`（2 分钟）崩溃保护。

### PPM 的温度回调与日志洪水

`mtk_ppm_v3` 每次重算限制（约 20 次/秒）都向“CPU 温度回调”要温度，回调本该由厂商热管理模块 `thermal_monitor.ko` 注册，
但它连带要求厂商 GPU DVFS 模块（`mtk_gpufreq_*`、`mtk_gpu_hal`），而 GPU 由 Panfrost 自己上电，不能加载那些。没有回调时 PPM 每次都在内核日志里打 `ppm_get_cpu_temp, … not init!`（20+ 行/秒，永远）。
`kernel-modules/ppmtemp`（`tb330fu_ppmtemp.ko`）调用 `mtk_ppm_v3` 导出的 `mt_ppm_thermal_get_cpu_cluster_temp_cb` 注册两个返回模块参数 `cpu_temp_mc`（毫摄氏度）的回调（kCFI 类型哈希已和厂商回调核对一致）。

### 热保护（`tb330fu-thermalguard`）

**目前没有 SoC 温度传感器驱动**（thermal zone 只有 `battery`）。守护进程每 5 秒读电池热敏电阻，分级限制两个簇的 `scaling_max_freq`（带 1.5 °C 回差）：

| 电池温度 | A55 | A75 |
|---|---|---|
| < 42 °C | 1800 | 2000 |
| 42–45 | 1500 | 1710 |
| 45–48 | 1100 | 1176 |
| 48–52 | 774 | 850 |
| ≥ 52 | 500 | 850 |

电池温度滞后于机身发热几分钟，所以这只防“长时间持续高负载”；满载 20 秒电池温度升了约 2 °C（29.9 → 32.0 °C）。同时它把“电池温度 + 8 °C”写进 `cpu_temp_mc` 给 PPM 用。
真正的 SoC 温度要靠 `thermal_monitor.ko` 里的 `tscpu_thermal_*`（需要先解决它对 GPU DVFS 模块的依赖）——未做。

## 其它提速与 apt

* 厂商 `cpuidle` 被命令行 `cpuidle.off=1` 关掉（早期 CN idle‑states 会让 tick 冻结），所以空闲功耗偏高、机身偏热。
* `apt-get update` 在 Wi‑Fi 正常时约 5 秒，装小包约 6 秒；慢多半是 CPU 频率或没有网络（旧版 Wi‑Fi 崩溃保护标记有时清不掉，会在快速重启后悄悄跳过 Wi‑Fi，已修）。
* `etc/apt/apt.conf.d/99tb330fu` 默认已经打开 `Acquire::Languages "none"`（省掉翻译索引）和 `dpkg --force-unsafe-io`（见下表）。

## 内存、存储与其它系统默认值（`rootfs/overlay`）

| 文件 | 作用 |
|---|---|
| `tb330fu-zram.service`、`usr/libexec/tb330fu-zram.sh` | zram 交换区（zstd，容量 = 内存的一半，优先级 100）：8 GB 内存加浏览器和 Android 容器会撞上 OOM killer。`zram.ko` 是自编内核的树内模块（`CONFIG_ZRAM=m`），没有它就跳过 |
| `etc/sysctl.d/90-tb330fu.conf` | `vm.dirty_background_bytes` = 32 MiB、`vm.dirty_bytes` = 128 MiB：默认按百分比最多攒约 1.5 GB 脏页，apt 或下载时一次性写出，其它程序的 `fsync()` 全卡在后面，桌面冻结。`vm.swappiness=100`、`vm.page-cluster=0`（swap 在 zram 上，便宜） |
| `etc/udev/rules.d/60-tb330fu-iosched.rules` | eMMC 用 BFQ：交互式读取优先于批量写入 |
| `tb330fu-quiet.service` | 开机完成后关掉 `ignore_loglevel` 并把控制台日志级别设成 4：厂商 Wi‑Fi/BT/电量计驱动每秒打几十条 WARNING，`ignore_loglevel` 会把每一条都格式化后抄到 pstore 控制台。消息仍在 dmesg 和 journal 里 |
| `etc/apt/apt.conf.d/99tb330fu` | `--force-unsafe-io`（eMMC 上 fsync 很慢；代价：断电时正在解包的那个包可能损坏，`dpkg --configure -a` 或重装可修，其余不受影响）、不下载翻译索引、下载重试 |
| `etc/systemd/timesyncd.conf.d/tb330fu.conf` | NTP 服务器（阿里云、腾讯云、cn.pool.ntp.org），后备 `ntp.ubuntu.com`；平板没有 RTC 电池，开机时间靠网络校正（`systemd-timesyncd` 在 `packages.txt` 里） |
| `etc/polkit-1/rules.d/50-tb330fu-power.rules` | `sudo` 组可以在任何会话里关机/重启（ssh、root 服务经 `runuser` 起的进程不算活动的本地会话，否则 polkit 会要管理员密码）；块抑制器（apt/dpkg 在运行）仍然有效 |
