# 05 Wi‑Fi、蓝牙、USB、电池

## Wi‑Fi 与蓝牙（MT6768 CONNSYS，A‑die MT6631，`tb330fu-conn.service`）

安卓靠 `wmt_loader`、`wmt_launcher` 两个守护进程把 CONNSYS 固件拉起来。这里用 Python 重写了它们（逆向自安卓二进制，标注过的反汇编在 `scripts/research/annotate_*.py` 的输出里）：

1. **解锁**：`tb330fu_gkiunlock.ko`（`rfkill`、`cfg80211`、`bluetooth` 由另一把密钥签名，属于受保护导出，未签名模块要引用/导出就得先解锁）。
2. **出厂数据**：从 `nvdata` 分区（只读挂载一下）拷出 Wi‑Fi 的 `WIFI` NVRAM 与蓝牙地址 `BT_Addr` 到 `/var/lib/tb330fu/nvram`（**设备私有数据，不进镜像、不进仓库**）。
3. **WMT 栈**（`/usr/lib/tb330fu/conn`，顺序见 `order.txt`）：`pmic_lbat_service … wmt_drv`，然后 `tb330fu-wmt-loader`（SOC 硬件初始化、读芯片 id、设 patch 列表）与 `tb330fu-wmt-launcher`（服务）。
   launcher 必须对内核的 `srh_patch`/`srh_rom_patch` 读命令回答 `ok`，否则固件下载卡住。固件在 `/vendor/firmware`（内核参数 `firmware_class.path`）。
4. **Wi‑Fi**：`wmt_chrdev_wifi` + `wlan_drv_gen4m_6768`；**先**把 NVRAM 以 `WR-BUF:NVRAM` 写进 `/dev/wmtWifi`（`tb330fu-wlan-nvram`），**再**只 `echo 1 > /dev/wmtWifi` 一次。
   失败后重复 `echo 1`，或在失败状态下再上电，会让整机 panic。regdom 取 `/etc/tb330fu/country`（默认 CN）。需要 `wpasupplicant` 与 `wireless-regdb`。
5. **蓝牙**：内核没有 `HCI_VHCI`，`tb330fu-btd` 把 `/dev/stpbt` 桥接到一个 pty，再 `btattach -P h4`（得到 `hci0`）。控制器不支持 `Read Sync Train Parameters`（0x0c77），由 btd 本地应答；每次 Reset 后用厂商命令 `0xFC1A` 写回出厂 BD 地址。
   `bluetooth.service` 靠 `tb330fu-btd.service` 的 `ExecStartPost` 启动（它的 `ConditionPathIsDirectory` 否则不满足）。`bluetooth.service.d/tb330fu.conf` 在启动后执行 `bluetoothctl pairable on`（适配器开机后出现过“不可配对”）。
   **LE 命令位图补丁**：固件对 `Read Local Supported Commands` 的回复里，第 25–28 字节（所有传统 LE 命令位）全是 0——尽管这些命令它都实现了（用 `hcitool` 探过：accept list 大小 64、支持的状态、LE Rand 都有应答）。Linux 按这张位图生成 LE 事件掩码，
   于是 `LE Read Remote Features Complete`、`LE Connection Update Complete` 被屏蔽：新连接永远走不出配置阶段，没有任何 L2CAP/SMP 数据发出，配对必然超时。`tb330fu-btd` 转发这条回复时把这些位补上（脚本里的 `LE_COMMAND_BITS`）。
   **HCI 抓包（调试用）**：把 `tb330fu-btd.service.d/trace.conf.example` 复制成 `trace.conf`，桥接程序就把经过它的每个 HCI 包按 btsnoop 格式记到 `/var/log/tb330fu/btd.snoop`（上限 32 MiB，`btmon -r` 或 Wireshark 读；这个内核的 monitor 通道里看不到 HCI 帧，单用 `btmon` 没有用）。
   **记录里包含蓝牙键盘的按键内容**，平时别开。

崩溃保护：`/var/lib/tb330fu/no-conn`、`conn-started`（上次启动没完成 bring‑up 则跳过一次；成功后 20 秒由定时器清除标记）。

已验证：扫描、连接 5 GHz 网络（175/390 Mbit/s）、apt 下载正常；蓝牙能扫描到设备。配对：上面的位图补丁已在平板上生效，但还没有用真实的蓝牙键盘复测；蓝牙音频没测。
**日志**：厂商 Wi‑Fi 驱动在 INFO 级别狂刷内核日志（空闲 ~22 行/秒，传输时上百行/秒），没有运行时调试级别开关。桌面没起来时这些日志会刷在开机控制台上。

## USB（`tb330fu-usb-gadget`、`tb330fu-otg`）

* gadget：configfs 里的 NCM（`usb0`，平板 `192.168.7.2/24`，systemd‑networkd 给主机发 DHCP `192.168.7.1`）+ CDC‑ACM 串口（`tb330fu-ttyGS0.service` 在上面开登录），复合设备类 `EF/02/01`，Windows 用 usbccgp。
* **OTG/设备模式**靠厂商 Type‑C 模块链：`mt635x-auxadc i2c-mt65xx tcpc_cps8851 aw35615_whole adapter_class charger_class sc8989x_charger rt_pd_manager tcpci_late_sync extcon-mtk-usb`
  （来自平台 ramdisk，由 stage1 暂存到 `/dev/.tb330fu/mods`，`tb330fu-otg.sh` 加载）。**关键细节**：`tcpci_late_sync` 一使能 TCPC 中断，“线缆已接入”的事件立刻发出，`extcon-mtk-usb` 只在注册当时才会切角色；
  所以必须 `extcon-mtk-usb` 先于 `tcpci_late_sync` 加载，并且模块之间**不能 sleep**——早期版本之间睡了 1 秒，错过事件，要拔插一次线才有 USB 网络。脚本里还有兜底：`usb_role` 写 `none` 再写 `device`。
* 充电器 SC89890H（i2c@11004000 地址 0x6a），电量计 mm8013：`upower` 能读，但内核没有 USER_NS，需要 `PrivateUsers=no`、`RestrictNamespaces=no` 的 drop‑in（overlay 里有 `upower`、`colord`）。
