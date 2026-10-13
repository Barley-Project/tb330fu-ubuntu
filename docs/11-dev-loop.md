# 11 远程调试环（`tools/tb.sh`）

目标：开发者不碰平板就能“改 → 装 → 看结果”。通道：**USB‑NCM 上的 ssh**（平板 `192.168.7.2`，主机侧由平板的 systemd‑networkd 通过 DHCP 给 `192.168.7.1`），备用 **CDC‑ACM 串口登录**，
进 fastboot 靠 **reboot‑mode**（`reboot-mode.ko` + `syscon-reboot-mode.ko` 往 toprgu 写 bootloader 魔数，`/usr/local/sbin/tb330fu-fastboot` 用原始 `reboot(RESTART2,"bootloader")`，
`systemctl reboot bootloader` 传不进参数）。

## 设置

```bash
export TB_SSH_KEY=~/.ssh/tb330fu_dev     # 平板里 ~/.ssh/authorized_keys 放对应公钥（build-rootfs.sh 的 TB_SSH_PUBKEY）
export TB_PASSWORD=...                   # 仅 `tb.sh sudo` 和串口登录需要
export FASTBOOT=fastboot                 # 或平台工具里 fastboot 的路径
```

## 命令

| 命令 | 作用 |
|---|---|
| `tb.sh addr / nic / wait [秒]` | 找到平板（IPv4，不行就用 `fe80::ff:fe00:1%<ifindex>`）；Windows 上 `nic` 用 `ncm-adapter.ps1` 查网卡 |
| `tb.sh ssh [命令…]` / `tb.sh sudo 命令…` / `tb.sh sh`（从 stdin 读脚本，以 root 运行） | 执行 |
| `tb.sh put 源… 目标` / `tb.sh get 源 目标` | scp |
| `tb.sh status` | `tb330fu-status`：内核、USB 角色、充电、单元状态、DRM、模块、dmesg 尾部 |
| `tb.sh dmesg|journal [n]` | 取日志存到 `$TB_DEV_OUT` |
| `tb.sh shot [名字]` | **截屏**：读 `/sys/kernel/tb330fu_shot/cur`（`tb330fu_shot.ko`，跟随扫描环当前正在扫描的缓冲），转成横屏 PNG（`tbshot.py`） |
| `tb.sh time` | 用 PC 的时间给平板对时 |
| `tb.sh fastboot / flash <分区> <镜像> / reboot / cycle <分区> <镜像>` | 进 fastboot、刷（白名单分区）、重启、等 ssh |
| `tb.sh serial run "<命令>"` | 串口登录执行（IP 链路不通时） |
| `tb-install.sh <相对路径…>` | 把 `rootfs/overlay` 里的文件装到运行中的平板（root 属主，脚本 0755） |

`tools/` 里还有：`keytest.py`（抓取按键设备打印事件）、`injectkey.py`（uinput 注入按键）、`uitap.py`（uinput 虚拟触摸屏点击）、`touchprobe.py`（GTK4 全屏触摸坐标记录）、
`fpsprobe.py`（GTK4 帧时钟 fps 探针：`small|full`）、`gltest.py`、`cpumhz.c`（用依赖链循环估算每个核的真实频率：`taskset -c N cpumhz`）。

## 在线迭代模块

* 临时模块：`tb.sh put` 过去，`sudo insmod`；**实验用模块不要自动加载**，panic 不能进入循环。
* 换显示驱动（不重启）：停会话 → `rmmod simpledrm` → `insmod` 新的（失败回退 `.good`）→ 写 `drm_name=mediatek`、`gpu_sync=1` → 启会话。`tb330fu-gpu.sh` 开机时做的是同一件事。
* 读/写物理寄存器：`tb330fu_peek.ko`（`/sys/kernel/tb330fu_peek/{addr,val,dump}`，**危险**：读被门控的块会挂总线，错误的写会复位 SoC；背光 PWM 的 `0x08` 就是例子）。
* 从 PC 开始写代码前，先读 [02-boot-and-kernel.md](02-boot-and-kernel.md) 的“加载模块”一节，并用 `scripts/verify/verify_ko.py` 检查。
