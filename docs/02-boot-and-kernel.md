# 02 启动、设备树、内核模块

## 设备树（`vendor-boot/make-dtb.sh`）

`vendor_boot` 的 `dtb` 段是 Android DT table（魔数 `d7b7ab1e`，FDT 在偏移 64）。CN 的 LK 把 CN 的 `dtbo` 叠加在这棵基础树上，用的是 libufdt（**节点名精确匹配**，
不是 libfdt 的 `fdtoverlay`；判断 LK 会不会接受一棵树要用 AOSP 的 `ufdt_overlay_test_app`，`scripts/` 里没有预编译件，需要的话从 AOSP 取源码编译）。从 ROW074 的树出发做 4 处修改：

1. 加两个空桩节点 `/mt6370_pmu_dts`（phandle 0x171）与 `/panel@0`（0x172）：CN dtbo 引用了基础树里没有的 label `mt6370_pmu`、`panel`，没有桩 LK 会中止。面板节点由 dtbo 填内容（GPIO）。
2. CN LK 不写 ROW 的 DRAM 属性（`dram_type`、`ch_cnt`、`rk_cnt`…），所以 `mtk_dramc`/`dvfsrc` 这族模块不加载；两个 mmc 节点删掉 `dvfsrc-vcore-supply`/`req-vcore`（否则 fw_devlink 把 mmc 的 probe 拖到超时）。
3. DRAM 的 VDDQ 稳压器 `ldo_vdram2` 加 `regulator-always-on`（唯一的使用者本来是 dramc，不加的话内核 30 秒后的 `regulator_late_cleanup` 会把它关掉，DRAM 就死了）。
4. SD 卡主机 `mmc@11240000` 禁用（根在 eMMC；少一个卡检测中断源）。

## INT_POL（`kernel-modules/intpol`）

MCUSYS 的 `INT_POL`（物理地址 `0x0c53a650`，20 个字）决定每个 SPI 是否反相。CN 的 4.19 内核按设备树里的 `LEVEL_LOW` 写它；ROW 的 6.6 设备树全部写 `LEVEL_HIGH`，
并假定 INT_POL 已被固件预置——而 CN LK/GZ 链路里没人写，启动时除 5 个位外全是 0，于是低有效的线（I2C 等）看起来一直有效，`i2c-mt65xx` 一加载就中断风暴把 CPU0 淹没
（这是早期一个版本循环重启的根因，已被日志证实）。`tb330fu_intpol.ko` 在 stage1 最先加载：把 94 个 SPI 置反相、30 个保持，读回校验。映射表 `intpol_map.json` 由 CN 的 dtbo/设备树与 ROW 设备树的触发类型比对得到
（`crosscheck_intpol_map.py` 独立再推一遍）。stage1 还加了 `irqaffinity=7`，万一还有风暴，也落在 CPU7 而不是 CPU0。

## 内核模块：怎么在 GKI 内核里加载自己编译的模块

* **vermagic**：用 g94 的头文件编译，`scripts/patch-vermagic.py` 把字符串改成 gda 的（gda 与 g94 同源同配置，仅调试选项不同，`module_layout` 与共有符号的 CRC 完全一致）。
* **CRC / 导入检查**：`scripts/verify/verify_ko.py`（导入符号逐个在 gda 导出表里查并比 CRC）、`loadsim.py`（整条加载序列模拟，含保护导出表）、`cfi_refs.py`（kCFI：回调函数前 4 字节的类型哈希要和内核里同类型函数一致）。
* **GKI 受保护导出**（`CONFIG_MODULE_SIG_PROTECT`）：未签名模块不能**导出**也不能**引用**受保护符号（GKI 模块的导出集合，例如 `rfkill`、`cfg80211`、`bluetooth`，甚至 `backlight_device_get_by_name`）。
  对策：`tb330fu_gkiunlock.ko` 在 `gki_is_module_protected_export()` 上挂 kretprobe 让它返回 false（只有 Wi‑Fi/BT 那条链需要，在 `tb330fu-conn` 里最先加载）；
  **永远不要去改写那张表**（初始化后只读，改它会 panic）；显示/GPU 路径不依赖它——所以背光做在 `simpledrm` 里，而不是单独的模块。
* **gda 与 g94 的导出集合不同**：gda 导出 15505 个符号，g94 只有 7820 个。用 g94 的 `Module.symvers` 编译会缺 DRM 帮助函数等，因此 `simpledrm` 是把用到的内核源文件一并编进去的“自包含”模块（`noexport.h` 去掉导出），
  需要 gda 才有的个别符号用 `KBUILD_EXTRA_SYMBOLS`（CRC 取自 gda 导出表，`scripts/kernel/make-extra-symvers.py`，或取自厂商模块，`module-symvers.py`）。
* `scripts/kernel/extract_kernel_data.py` 只用 `boot.img`/`boot-debug.img` 就能恢复：内核配置（IKCONFIG）、kallsyms（`System.map`）、导出表与 CRC（`Module.symvers`）——与此前人工恢复的结果逐字节一致。

## 持久日志与排错信道

* 内核日志会在崩溃后丢失：`kedump` 只在**下一次** LK 启动时才把 panic 控制台抄进 `expdb`，而且 `systemd-pstore` 会在 t≈4 s 清空 pstore（所以命令行里有 `systemd.mask=systemd-pstore.service` 与 `ignore_loglevel`）。
* `tb330fu_blklog.ko` 每秒（前 20 秒）/每 5 秒（25 分钟内）把 kmsg 快照写进 `expdb` 分区里一段全零空隙（`0x1120000` 起，带 `TB330LG1` 头与 CRC，两个槽轮换）；模块初始化时会检查分区确实是 expdb 才写。
  读取：`kernel-modules/blklog/parse_blklog.py <expdb.img> <前缀>`；平板还活着时也可以直接读 `/dev/mmcblk0p6`。
* 开机控制台 `tb330fu_con.ko`：内核没有 fbcon/VT，这是个小 `struct console`，把日志画到 LK 的 framebuffer 上（Sun 12×22 字体、Tux 标志，约 8 fps，红/黄/绿着色）；`simpledrm` 在第一次提交前通过 framebuffer 行尾 16 像素里的邮箱字（`0x54583330/0x54583331`）通知它停止绘制。
* `tb330fu_harvest.ko`：计划内 panic 计时器，早期用来把整段启动日志逼进 expdb，现在默认关闭（`HARVEST_SECONDS=0`）。
