# 03 显示、背光、GPU

## 显示路径：`simpledrm` 接管 LK 的 framebuffer

内核没有 DRM 显示驱动（`mediatek-drm` 那一族要 DRAM/EMI/DVFSRC 一串模块，这里不用）。LK 点亮面板后把画面留在 `0x7c8e0000..0x7f5fffff`
的保留区里：3 个 4864×1920 字节的缓冲（相隔 `0x8E8000`，**每行 1216 像素，只扫描 1200 个**，行尾 16 像素从不显示，被用作邮箱），格式 a8r8g8b8。
`tb330fu_fb` 注册 `simple-framebuffer`，`simpledrm`（`kernel-modules/simpledrm`）驱动它。

* **OVL 事实**（`tb330fu_ovl`，对 LK 汇编 `scripts/research/lk_disasm.py` 逆出来的）：LK 的每个图层 `L_CON` 总是带 `0x600`（垂直+水平翻转），`ADDR(+0xf40+0x20n)` 写的是图层**末尾**地址
  `addr + pitch*(y+h-1) + bpp*(x+w) + bpp-1`，不是起始地址——写起始地址会让 OVL 反向读到缓冲区下面的内存，屏幕变白。显示出来有“竖屏原生方向的列扫描”，所以横屏桌面上更新看起来像 CRT 逐列刷新。
* **扫描环**（`tb330fu_flip.inc`）：每次提交先 blit 到**当前没被扫描的缓冲**，再把 OVL0 图层 0 的地址寄存器（`0x1400bf40`）指过去；三个缓冲各自维护“还缺哪些脏矩形”。小面积更新实测 80–134 fps，全屏更新 ~86 fps。
* **面板方向**：原生竖屏装在横屏优先的平板里，摄像头一侧（“上”）是面板的**右边缘** → DRM 属性 `panel orientation = RIGHT_UP`（模块参数 `orientation=3`），mutter 据此旋转桌面和触摸。
* **90 Hz**：连接器上报的是面板真实的 90 Hz 模式（`refresh=90`）；之前的假 60 Hz 会让合成器按 60 fps 出帧。
* **连接器类型必须是 DSI**：mutter 只把触摸屏映射到“内置显示器”（`DSI/eDP/LVDS`），`Unknown` 类型下触摸既不旋转也不映射。
* `drm_name` / `gpu_sync` 参数见下面的 GPU 一节。

## 背光与息屏（`tb330fu_flip.inc` 里的 `tb330fu_bl_*`）

LK 把面板背光交给显示 PWM `DISP_PWM0`（`0x1100e000`）后就没人管了（周期 1023，占空比 494）。`simpledrm` 在 probe 时注册 `/sys/class/backlight/tb330fu_bl`（`raw` 类型，`max_brightness=1023`），GNOME 的亮度滑块直接可用：

| 偏移 | 含义 |
|---|---|
| `0x00` | `DISP_PWM_EN`，bit0 = 运行（LK 已置位，驱动只检查） |
| `0x1c` | `CON_1`：bit28:16 占空比，bit11:0 周期 |
| `0x2c` | 只读：PWM 当前真正输出的值 |
| `0x80` | `BLS_DEBUG`：bit1:0 = 3 关闭双缓冲，**必须先置位**，否则写 `0x1c` 不生效 |
| `0x08` | **复位——绝对不要写**（写 1 再写 0 会让整个块复位，PWM 停掉、屏幕全黑） |

息屏跟随显示管线：CRTC/平面被关掉（DPMS 关、空闲熄屏、电源键）时 `bl_power` 变成 `FB_BLANK_POWERDOWN`，背光灭；下一帧提交后恢复。因此 GNOME 的所有熄屏路径自动带背光。
`tb330fu-touchd` 在 `bl_power != 0` 时丢弃触摸（并在熄屏时给虚拟触摸屏补发“所有手指抬起”，亮屏时如果手指仍按着，会等它抬起再恢复）。
不能做成独立的背光模块：`backlight_device_get_by_name` 在 GKI 受保护导出表里，未签名模块引用会被拒绝（`Permission denied`），而 GPU 服务是“先 rmmod 旧的再 insmod 新的”，失败就会没有任何显示驱动——
所以 `tb330fu-gpu.sh` 现在在新模块被拒绝时回退到 `simpledrm.ko.good`。

## GPU：Panfrost（`kernel-modules/panfrost`）

* 内核配置里没有 Panfrost。`panfrost.ko` 是把 6.6.30 的 Panfrost + DRM 调度器**编进同一个模块**（不依赖 `gpu_sched.ko`，不导出符号），加上 `tb330fu_gpu.c`（MT6768 的电源时序：clk/稳压器/电源域由我们自己控制，设备树里的 `mali` 节点没有 clock/reset）。
  补丁是“每个模式必须恰好匹配一次”的精确替换（`patch_panfrost.py`）：`mediatek,mali`/`arm,mali-valhall` 兼容串、去掉 clk/reset/devfreq、IRQ 名 `JOB/MMU/GPU` 大写。
* **fw_devlink**：`mali` 节点有 `ged-supply = <&ged>`，而厂商 GED 驱动不加载，probe 会被推迟到 deferred‑probe 超时（开机后 ~16 s）。模块里放一个绑定 `mediatek,ged` 的桩 platform driver，立刻满足依赖。
* **Mesa 怎么选 Panfrost**：Mesa 按 DRM **驱动名**选渲染后端；`simpledrm` 的名字是模块参数 `drm_name`，GPU 服务在 `renderD128` 出现后才把它设成 `mediatek`（kmsro 路径：用 Panfrost 渲到本设备的 dumb buffer，再由 `simpledrm` blit 到扫描缓冲）。
  GPU 写缓冲绕过 CPU（非一致性 DMA），所以 `gpu_sync=1` 时 blit 前对源缓冲做 cache 失效。
* **会话必须在 GPU 之后启动**：render 节点晚于 mutter 出现会破坏 GL。`tb330fu-gnome.service` `Wants/After=tb330fu-gpu.service`。
* 崩溃保护：`/var/lib/tb330fu/no-gpu`、`gpu-started`（2 分钟）。
* 局限：GPU 渲染 + CPU memcpy blit，没有真正的 vsync；CPU 之前锁在 ~1.1 GHz 时全屏动画更吃力，现在有调频后好很多。
