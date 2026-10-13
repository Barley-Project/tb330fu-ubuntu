# 内核模块一览（`kernel-modules/`）

| 目录 | 模块 | 作用 | 何时加载 |
|---|---|---|---|
| `intpol` | `tb330fu_intpol` | 按映射表写 MCUSYS `INT_POL`（94 个反相、30 个保持），读回校验；`apply=1 dump=1` | stage1 最先 |
| `ovl` | `tb330fu_ovl` | 读/解码 OVL0/OVL0_2L 图层，按 LK 的“末尾地址”公式把保持图层指到 framebuffer 缓冲 0（`seq=2` 默认） | stage1 显示组 |
| `console` | `tb330fu_con` | 开机控制台（`struct console`，画到 LK framebuffer；`rotate` `font` `quiet` `fps` `tux_scale` 参数；`fonts.h`/`tux80.h` 由 `gen_console_assets.py` 从内核字体/logo 生成） | stage1 显示组 |
| `fb` | `tb330fu_fb` | 注册 `simple-framebuffer`；`/sys/kernel/tb330fu/fb0` 回读；`pattern` 参数可画测试图 | stage1 显示组 |
| `simpledrm` | `simpledrm` | **显示驱动**：LK framebuffer + 90 Hz 模式 + DSI 连接器 + 面板方向 + 三缓冲扫描环 + 背光类设备 + 息屏钩子 + `drm_name`/`gpu_sync` | stage1 显示组；`tb330fu-gpu` 在需要时替换成新版 |
| `panfrost` | `panfrost` | Mali‑G52：Panfrost + DRM 调度器 + MT6768 电源时序 + `mediatek,ged` 桩（一个自包含模块） | `tb330fu-gpu` |
| `blklog` | `tb330fu_blklog` | 持久日志：kmsg 快照写进 expdb 空隙（`parse_blklog.py` 解析） | stage1，选根之后 |
| `harvest` | `tb330fu_harvest` | 计划内 panic 计时器（默认关） | 仅调试 |
| `gkiunlock` | `tb330fu_gkiunlock` | kretprobe：`gki_is_module_protected_export()` 返回 false | `tb330fu-conn` 最先 |
| `ppmtemp` | `tb330fu_ppmtemp` | 给 `mtk_ppm_v3` 注册 CPU 温度回调（参数 `cpu_temp_mc`） | `tb330fu-cpufreq` 末尾 |
| `shot` | `tb330fu_shot` | `/sys/kernel/tb330fu_shot/{cur,buf0,buf1,buf2,front}`：扫描缓冲的截屏接口 | 按需（`tb.sh shot`） |
| `peek` | `tb330fu_peek` | 物理寄存器读写（sysfs） | 按需 |

`kernel-modules/*/test/`：几个模块的**主机侧单元测试**（`hosttest_*.c` 把模块 .c 直接 `#include` 进来、用空的假头文件在 PC 上编译运行：控制台渲染矩阵、OVL 接管逻辑、blklog 窗口逻辑、fb 渲染），
`raw2png.py` 把帧转成 PNG。运行方式：把 `-I` 指到一个放了空 `linux/*.h` 的目录，`gcc -O1 hosttest_xxx.c`。
