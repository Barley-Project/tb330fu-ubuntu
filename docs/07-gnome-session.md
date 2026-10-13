# 07 GNOME 会话与桌面默认设置

## 没有显示管理器的 GNOME Wayland 会话

内核没有 VT，SDDM 的 greeter 会启动一个永远不放弃 DRM master 的 Xorg，KWin/mutter 拿不到 `/dev/dri/card0`。所以不用 DM，直接用一个系统单元（`tb330fu-gnome.service`）：

* `User=ubuntu`，`PAMName=tb330fu-session`（`etc/pam.d/tb330fu-session`：对被锁定的账户也能开 logind 会话），`gnome-session --session=gnome`，`LANG=zh_CN.UTF-8`，`XDG_SESSION_TYPE=wayland`，无 VT 的 logind 会话。
* `After/Wants`：`tb330fu-gpu.service`（GPU 必须先好）、`tb330fu-touchd.service`（`PartOf=` 本单元，会话重启时一起重启）。
* 失败三次（5 分钟内）→ `OnFailure=tb330fu-plasma-fallback.service` 创建 `/var/lib/tb330fu/use-plasma` 并启动 stage1 放在内存里的 Plasma 单元；`ConditionPathExists=!/var/lib/tb330fu/use-plasma` 阻止 GNOME 再起。
  **没装 Plasma 就不要启用这个回退**（`TB_WITH_PLASMA=1` 才装）。要回到 GNOME：删掉 `use-plasma`。
* `upower`、`colord` 需要 drop‑in（`PrivateUsers=no`、`RestrictNamespaces=no`：内核没有 USER_NS）；`WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1`（`environment.d`，没有命名空间时 WebKitGTK 沙箱起不来）。

## dconf 默认（`etc/dconf/db/local.d/00-tb330fu`）

不锁屏、不空闲、不休眠（内核没有休眠）、显示电量百分比、窗口按钮布局、`scale-monitor-framebuffer`（分数缩放）、输入法 `xkb:us` + `ibus libpinyin`、收藏应用、关欢迎对话框、
**`screen-keyboard-enabled=true`**、**`power-button-action='nothing'`**（电源键由 `tb330fu-keysd` 处理，见 [04-input-keys.md](04-input-keys.md)）。
`home/ubuntu/.config/monitors.xml` 固定 `DSI-1` 为 1920×1200@90.000、缩放 1.5（刷新率必须写成 `90.000`；用 `ApplyMonitorsConfig` 会弹 5 秒确认框然后回滚）。

## 其它

* 时间：平板没有 RTC 电池，`fake-hwclock` + `tb.sh time` 从 PC 对时（TLS 与 apt 需要一个合理的日期）。
* 只有 Wayland：没装 `kwin-x11`，GNOME 也没有 X11 会话；X 程序走 Xwayland。
* 默认语言中文；去掉 `rootfs/packages.txt` 里“中文界面与输入”那一段即可换成英文。
