# 10 已知问题、没做的事

| 项目 | 状态与原因 |
|---|---|
| **Waydroid** | 用自编内核（`kernel-config/tb330fu.config` + 命令行 `lsm=bpf`）可用，见 [12-waydroid.md](12-waydroid.md)；官方内核上不行（缺 `PID_NS`、`IPC_NS`、`USER_NS`）。没有声音。在 Magisk Delta 里设置 Zygisk 并重启之后，容器启动约 2.5 分钟就停了，原因没查出来，详见 12-waydroid.md 的“已知问题”。 |
| **音频** | 没有声卡。已做：`tb330fu-audio.sh` 按顺序加载厂商的 AFE、MT6358 编解码器和 4 个 AW883xx 功放模块，`tb330fu_spmstub.ko` 代替 `MTK_INTERNAL_SPM`（音频驱动只用到它的 3 个函数；SPM 本身要拖进 17 个厂商模块和 SPM 固件），服务默认**不启用**。卡在：I2C3（`1100f000`）上的 AW883xx 探测超时（-110）。猜测是 I2C3 没有 pinmux（设备树里没有任何 I2C 节点带 pinctrl，LK 只给自己用的总线做了复用）——**还没验证**；也可能国行硬件用的是别的功放（fs1801 之类，没查）。音量键事件正常，但没有可调的输出。 |
| **关机充电（off-mode charging）** | 插着充电器关机，LK 会以充电模式重新启动内核；`tb330fu-charger`（stage1 把它当 PID 1 启动：熄屏，长按电源键 1.5 秒重启进正常系统）已写好，**没有在真实的充电开机上测过**，见 [04-input-keys.md](04-input-keys.md)。 |
| **SukiSU‑Ultra（内核 root）** | 搁置。`kernel-config/tb330fu-sukisu.config` 能编译、`crc-compare.py` 显示 CRC 无变化，刷进 `boot_a` 后平板却连不上，用 fastboot 刷回上一个镜像才恢复；原因没查。CRC 一致只说明厂商模块能加载，不说明内核能启动。要再试，先想好怎么抓早期启动日志。 |
| **摄像头** | 不可行（ISP/CCU/CAM 一整套 + 厂商用户态 HAL）。 |
| **休眠/唤醒** | 不支持；GNOME 的休眠选项已关。`cpuidle.off=1`，空闲功耗偏高。 |
| **SoC 温度传感器** | 没有（`thermal_monitor.ko` 依赖厂商 GPU DVFS 模块）。目前只有电池温度的粗保护，见 [06-cpu-power.md](06-cpu-power.md)。 |
| **CPU 调频的冷启动验证** | 手工加载整条链已验证；`tb330fu-cpufreq.service` 的开机自动加载已部署但在写文档时还没有冷启动验证过。 |
| **电源键短按/长按的实机验证** | 内核与桌面链路用合成事件验证过（`PowerSaveMode` 3/0 ↔ `bl_power` 4/0 ↔ `actual_brightness` 0/满）；按键事件本身用实机按键验证过；`tb330fu-keysd` 的“按下计时”逻辑还没有用实机按键完整走过一遍。 |
| **触摸** | 坐标与方向用合成触摸验证；真手指的最终确认缺失。熄屏时丢弃触摸的逻辑是新写的。 |
| **亮度初值** | 背光驱动注册时读到 LK 留下的 494/1023，GNOME 启动后会把它改成自己的值（实测变成 1023）。 |
| **传感器**（加速度计/光感/陀螺） | 没做；自动旋转没有。 |
| **蓝牙** | 能扫描。配对原来必然超时，原因是固件的支持命令位图缺了所有 LE 命令位，`tb330fu-btd` 现在转发时补上（见 [05-connectivity-usb.md](05-connectivity-usb.md)）；补丁已在平板上生效，还没有用真实的蓝牙键盘复测。蓝牙音频没测。 |
| **Wi‑Fi 日志** | 厂商驱动在 INFO 级别狂刷内核日志，无运行时开关；`ignore_loglevel` 让它们也上开机控制台。 |
| **SD 卡** | 在设备树里被禁用（降低风险），没有测试。 |
| **`build-rootfs.sh`** | 没有端到端跑通；按平板上实际安装的包整理（`rootfs/packages.txt`）。 |
| **`firefox`/`snap`** | Ubuntu 的 `firefox` 是 snap 过渡包，snap 需要 squashfs/命名空间；用 `epiphany-browser`（GNOME Web）。 |
| **GKI 受保护导出** | 某个新模块引用了受保护符号会被拒绝（`Protected symbol: … (err -13)`）。别让显示/GPU 路径依赖它们；Wi‑Fi/BT 链靠 `gkiunlock`。 |

## 经验清单（每一条都是真实踩过的坑）

* 在 initramfs shell 里，`2>/dev/null` 这类重定向会在 `/dev/null` 还不存在时把它创建成**普通文件**——先 `rm -f` + `mknod`，不带任何重定向，之后检查 `[ -c /dev/null ]`。
* 厂商钩子（OVL 图层寄存器）必须**读寄存器**确认，不能只看一行日志推断；LK 的显示有两个图层。
* 新加载的外设驱动先问：它的中断极性靠 INT_POL 吗？I2C 低有效线会中断风暴，加 `irqaffinity`。
* 换内核模块前先想**失败后还有没有显示驱动**；崩溃保护标记要由 `systemd-run` 定时器清，不要用服务里的后台子进程。
* 厂商 Wi‑Fi 上电必须先写 NVRAM 再开一次；失败后不要再开。
* 不要写 GKI 受保护导出表（初始化后只读，会 panic），用 kretprobe。
* 工具层面：在 Windows 上用 Git‑Bash 驱动 WSL 时，heredoc 里的反斜杠可能被吞——生成脚本/补丁用文件编辑，不要用 `python - <<EOF` 写含 `\n` 的字符串。
* 同样的环境里，`wsl.exe` 的 `/mnt/...` 参数要加 `MSYS_NO_PATHCONV=1`，否则会被改写成 Windows 路径；在 Windows 上写的文件带 CRLF 会让 Kconfig 和 shell 解析出错。多行的 `wsl.exe -- bash -lc '...'` 很脆，写成脚本文件再按路径调用。
* `tar xf … -C /` 解一个顶层是 `lib/` 的包，会把 `/lib`（指向 `usr/lib` 的符号链接）换成真目录，之后没有任何动态链接的程序能启动。内核模块包放在 `usr/lib/modules/` 下（`scripts/kernel/stage-modules.sh`）；`stage1.sh` 里有一个一次性的 RESCUE 块，发现 `/lib` 是目录就把内容搬回 `usr/lib` 并恢复链接。
* 导出符号 CRC 全部一致只能证明厂商模块能加载，不能证明内核能启动。刷 `boot` 之前要留好 fastboot 回退路径和原镜像。
* 新的 Kconfig 符号如果 `default y`，`olddefconfig` 会让它在之后所有构建（包括不带片段的基线）里都被打开。
