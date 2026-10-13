# 10 已知问题、没做的事

| 项目 | 状态与原因 |
|---|---|
| **Waydroid** | 不可能：内核缺 `PID_NS`、`IPC_NS`、`USER_NS`、`SYSVIPC`、`CGROUP_DEVICE`、`PIDS`（有 binderfs、ashmem、bridge/NAT、cgroup v1）。要跑需要换成自己编译、带这些选项的内核——而这需要换整套 boot 链。`tb330fu-waydroid-preflight` 是只读预检脚本。 |
| **音频** | 没有声卡。`mt6358` 编解码器和 AW883xx 功放的依赖是 29 个厂商电源模块（SPM、SSPM、DRAMC…）；SSPM 现在能用，但 DRAM/EMI 那一族是有意不加载的。音量键事件正常，但没有可调的输出。 |
| **摄像头** | 不可行（ISP/CCU/CAM 一整套 + 厂商用户态 HAL）。 |
| **休眠/唤醒** | 不支持；GNOME 的休眠选项已关。`cpuidle.off=1`，空闲功耗偏高。 |
| **SoC 温度传感器** | 没有（`thermal_monitor.ko` 依赖厂商 GPU DVFS 模块）。目前只有电池温度的粗保护，见 [06-cpu-power.md](06-cpu-power.md)。 |
| **CPU 调频的冷启动验证** | 手工加载整条链已验证；`tb330fu-cpufreq.service` 的开机自动加载已部署但在写文档时还没有冷启动验证过。 |
| **电源键短按/长按的实机验证** | 内核与桌面链路用合成事件验证过（`PowerSaveMode` 3/0 ↔ `bl_power` 4/0 ↔ `actual_brightness` 0/满）；按键事件本身用实机按键验证过；`tb330fu-keysd` 的“按下计时”逻辑还没有用实机按键完整走过一遍。 |
| **触摸** | 坐标与方向用合成触摸验证；真手指的最终确认缺失。熄屏时丢弃触摸的逻辑是新写的。 |
| **亮度初值** | 背光驱动注册时读到 LK 留下的 494/1023，GNOME 启动后会把它改成自己的值（实测变成 1023）。 |
| **传感器**（加速度计/光感/陀螺） | 没做；自动旋转没有。 |
| **蓝牙** | 能扫描；配对、音频没测。 |
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
