# 04 触摸、屏幕键盘、硬件按键

## 触摸（`tb330fu-touchd`）

Himax 驱动除了多点触摸轴还注册了手势用的 `KEY_*` 和 `BTN_LEFT/RIGHT/MIDDLE`，libinput 因此把它报成“keyboard pointer touch”，mutter（设备类型判断是 pointer 先于 touch）把它当成**绝对指针**：不映射到内置显示器、不随面板方向旋转，
横屏桌面上就出现“触摸左边右边有反应”的镜像现象。udev/libinput 的 quirk 去不掉指针能力，所以：

* `tb330fu-touchd`（Python，root）等 `gnome-shell` 起来后，`EVIOCGRAB` 抓住 Himax 的 event 设备，用 uinput 重新发布一个只含槽位/tracking id/位置/`BTN_TOUCH` 的“干净”触摸屏（`1d6b:0330`）。
  **mutter 之后才出现的触摸屏**会被映射到内置显示器并带上面板方向（用合成触摸验证过）。
* `etc/udev/rules.d/90-tb330fu-touch.rules` 把原始 Himax 设备放到一个没人用的 seat（`seat-tb330fu-raw`）并清掉 `ID_INPUT_MOUSE/KEY/KEYBOARD`，防止桌面直接用它。
* **不要对 himax 做 rebind/rmmod**（它的 proc 条目不会清理，之后 probe 失败直到重启）。
* 熄屏时丢弃触摸，见 [03-display-gpu.md](03-display-gpu.md)。

## 屏幕键盘

GNOME 的屏幕键盘需要“触摸模式”（没有指针设备）或 `org.gnome.desktop.a11y.applications screen-keyboard-enabled=true`（本项目的 dconf 默认已开）。
插着外接键鼠时触摸模式会关闭，仍可从辅助功能菜单手动打开。

## 硬件按键

设备树里的分工：

| 键 | 硬件 | 设备树 | 驱动 |
|---|---|---|---|
| 电源键 | PMIC `pwrkey` | `mt6358keys/power`，`linux,keycodes = <116>` | `mtk-pmic-keys.ko`（输入设备 `mtk-pmic-keys`） |
| 音量减 | PMIC `homekey` | `mt6358keys/home`，`linux,keycodes = <114>` | 同上 |
| 音量加 | SoC 键盘矩阵第 0 个键 | `kp@10010000`，`mediatek,kpd-hw-init-map[0] = 115` | `mtk-kpd.ko`（输入设备 `mtk-kpd`） |

两个驱动都是厂商模块（vermagic 与 gda 一致、无模块依赖）。它们的 platform 设备本来就存在（设备树 + PMIC MFD），**只是没人加载驱动**；`tb330fu-keys.service` 按官方顺序加载（`mtk-pmic-keys` → `mtk-kpd`）。
用 `tools/keytest.py`（抓取设备后打印按键）可以验证；`tools/injectkey.py` 可以用 uinput 注入按键测试桌面反应。

### 为什么系统被标成“虚拟机”，以及电源键为什么会直接关机

设备树里有 `/hypervisor`（`mediatek,geniezone-hyp`，GenieZone）。systemd 的 `detect_vm_device_tree()` 只要看到这个节点就报 `vm-other`；hostnamed 在没有 `/etc/machine-info` 的 `CHASSIS=`、
设备树也没有 `chassis-type`、没有 DMI 的情况下，把机箱类型回退成 `vm`（图标 `computer-vm`）。而 GNOME 46 的 `gsd-media-keys` 对 `vm` 机箱**无视 `power-button-action`，短按电源键直接 PowerOff**。
修法：`/etc/machine-info` 写 `CHASSIS=tablet`（overlay 里有），这样不用改设备树里的 GZ 节点，也不用刷 vendor_boot。（`systemd-detect-virt` 仍会报 vm，只影响少数 `ConditionVirtualization=` 的单元。）

### 电源键策略（`tb330fu-keysd`）

dconf 里 `power-button-action='nothing'`，logind 被 `gsd-media-keys` 的 `handle-power-key` 阻塞型 inhibitor 挡开，因此只有 `tb330fu-keysd` 处理电源键：

* **短按**：熄屏 ↔ 亮屏（`org.gnome.Mutter.DisplayConfig` 的 `PowerSaveMode` 设 3/0；背光由显示驱动跟随，若没跟上就直接写 `bl_power`）。
* **长按 0.8 秒**（按住时就触发）：先唤醒屏幕，再弹 `tb330fu-powermenu`（GTK4，大按钮：关机 / 重启 / 取消，30 秒自动关闭）。菜单打开时再短按 = 关闭菜单。
* 没有 GNOME 会话时守护进程什么都不做（logind 默认：短按关机）。按住 ~10 秒由 PMIC 硬件强制关机，与软件无关。
* 音量键：事件正常到达桌面，`gsd-media-keys` 绑定的是 `XF86AudioRaiseVolume/LowerVolume`；目前没有声卡，所以没有可调的音量。
* 自动熄屏：GNOME 的 `idle-delay` 现在设成 0（从不），要打开可以在 dconf 里改成秒数——熄屏路径（DPMS → 背光）已经通；唤醒靠电源键（触摸在熄屏时被丢弃）。
