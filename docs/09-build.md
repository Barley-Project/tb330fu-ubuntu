# 09 构建

构建在 **Linux 或 WSL2** 里做（需要 `bash python3 make git curl clang 依赖 pahole lz4 cpio qemu-user fdtput dtc e2fsprogs`，交叉编译 `aarch64-linux-gnu-gcc`，Python 包 `pyelftools`）。
所有路径与参数集中在 `scripts/lib/common.sh`，可用 `config.sh`（复制 `config.example.sh`）或环境变量覆盖：

| 变量 | 默认 | 含义 |
|---|---|---|
| `TB_WORK` | `~/tb330fu-work` | 所有下载与构建输出 |
| `TB_FIRMWARE` | `$TB_WORK/firmware` | **你自己的** ROW074 固件文件（`boot.img`、`boot-debug.img`、`vendor_boot-debug.img`、`dtbo.img`） |
| `TB_DIST` | `<repo>/dist` | 最终镜像 |
| `TB_ROOT_LABEL` / `TB_ROOT_UUID` | `TB330FU_KDE` / `7292ba20-…-8002-000000000024` | 根文件系统身份（vendor_boot 与镜像必须一致） |
| `TB_PROXY` | 空 | 构建时下载用的 HTTP 代理 |

## 1. 内核准备

```bash
scripts/kernel/fetch-toolchain.sh        # clang r510928（Android 预编译，“clang version 18.0.0”）
scripts/kernel/fetch-kernel-source.sh    # kernel/common 提交 94c1a24cabd5…（生产内核 g94 的源码），并打 libbpf 的一行 const 补丁
scripts/kernel/extract-kernel-data.sh    # boot.img / boot-debug.img -> 配置、System.map、导出表 JSON、Module.symvers
scripts/kernel/prepare-kernel-tree.sh    # 用 g94 的配置 + 恢复出的 Module.symvers 生成 Kbuild 输出目录（~30 MB），检查 kernelrelease
```

`extract_kernel_data.py` 已对 `boot.img`/`boot-debug.img` 验证：导出表、`System.map`、`Module.symvers` 与此前人工恢复的结果逐字节一致。

## 1b. 自编内核（可选，Waydroid 需要）

```bash
scripts/kernel/build-kernel.sh baseline                                 # 不带片段：应复现 gda 内核的导出表（复现性测试）
scripts/kernel/build-kernel.sh tb330fu kernel-config/tb330fu.config     # 带片段；输出在 $TB_WORK/kernel/build-tb330fu
python3 scripts/kernel/crc-compare.py "$TB_WORK/kernel/build-tb330fu" "$TB_KDATA/boot-debug/Module.symvers" --used "$TB_MODOUT"
scripts/kernel/build-boot.sh --self-test                                # 原内核原样重打包 = 原 boot-debug.img
scripts/kernel/build-boot.sh "$TB_WORK/kernel/build-tb330fu/arch/arm64/boot/Image"    # -> $TB_DIST/boot.img（32 MiB）
scripts/kernel/stage-modules.sh tb330fu                                 # -> $TB_DIST/kernel-modules.tar（树内模块，build-rootfs.sh 自动解开）
```

`build-kernel.sh` 用 g94 的源码和 gda 的配置（`CONFIG_LOCALVERSION` 改成让版本字符串与 gda 完全相同，已有模块和厂商模块的 vermagic 照常匹配），再用内核自带的 `merge_config.sh` 合并片段并 `olddefconfig`。
**片段的原则**：不能改变任何厂商模块用到的结构体布局。`crc-compare.py` 事后比较导出符号的 CRC，并列出会因此被内核拒绝加载的模块（`--used` 可重复，给出模块目录，例如 `collect-vendor-files.sh` 的输出）。
`kernel-config/tb330fu.config` 里写明了哪些选项因为会改 CRC 而不能开：`CGROUP_DEVICE`/`CGROUP_PIDS`（约 6000 个导出符号的 CRC 变化）、`SYSVIPC`（`task_struct`）、关闭 GKI 模块保护（`struct module`）。

坑：接入外部驱动时，新的 Kconfig 符号如果 `default y`，`build-kernel.sh` 的 `olddefconfig` 会在合并片段**之前**就把它打开——之后每次构建（包括不带片段的基线）都带上它。接入前先把 `default` 改成 `n`。

## 2. 内核模块

```bash
scripts/build-modules.sh all            # 或 gkiunlock intpol blklog harvest ovl con fb shot peek ppmtemp spmstub simpledrm panfrost
```

每个模块在“`$TB_BUILD_TMP/<名字>/{kdir,m}`”里对 **g94 Kbuild 目录的副本**编译（原目录不被写入；模块目录与 kdir 并列，`__FILE__` 字符串因此与机器无关），
`llvm-objcopy --strip-debug`，`patch-vermagic.py` 改成 gda，输出 `$TB_MODOUT/*.ko` 与 `SHA256SUMS`。`simpledrm`/`panfrost` 先由 `patch_*.py` 从内核源码生成（每个替换必须恰好匹配一次，否则中止）。
`ppmtemp` 需要 `TB_VENDOR_MODULES`（`vendor_dlkm` 的模块目录）来取 `mtk_ppm_v3.ko` 导出符号的 CRC；`spmstub` 同样需要其中的 `snd-soc-mt6768-afe.ko`：它导出的 3 个符号的 CRC 由 `scripts/patch-crc.py`（需要 `pyelftools`）改成这个音频驱动导入时期望的值。构建完用 `scripts/verify/verify_ko.py <ko> [已加载的模块…]` 检查。
同一台机器、同一路径下重编译得到逐字节相同的结果；换目录名只会改变 GNU build‑id 和个别含路径的字符串。

## 3. vendor_boot

```bash
scripts/kernel/fetch-upstream.sh        # NevedomaZverushka @ 固定提交：init.c、tb330fu-devd
scripts/build-busybox.sh                # 静态 aarch64 BusyBox 1_36_stable
vendor-boot/build-vendor-boot.sh        # -> $TB_DIST/vendor_boot.img（64 MiB）
```

构建器的步骤与检查：校验 `mkbootimg.py` 的哈希与官方片段 0/1/dtb 的哈希（不是 ROW074 的 debug vendor_boot 就拒绝，`TB_ALLOW_OTHER_FIRMWARE=1` 才继续）→ 解包 → 从平台片段里取 42 个厂商模块（`modules.vendor`）并确认与 ramdisk 里的字节一致 →
编译 `init.c`（静态，`-Werror`）→ 组 stage 树（`stage1.sh` 里的 `@STAGE1_ID@` 换成文件哈希前 8 位，开机日志里可识别是哪一版）→ cpio（排序、时间戳 0）→ `lz4 -l -12` →
`make-dtb.sh` → `mkbootimg`（命令行、偏移、3 个片段）→ 补零到 64 MiB → 重新解包逐段核对 → 模拟内核的解包顺序检查 `modules.load.*` 引用的模块都在 → `sh -n` → `loadsim.py` 跑整条加载序列。
`TB_STAGE_DIRMODE/FILEMODE/EXECMODE=0777` 仅用于逐位复现旧镜像（旧镜像是在 Windows 盘上打的包，权限全是 0777）。

内核命令行：

```
bootopt=64S3,32N2,64N2 rdinit=/tb330fu-init printk.devkmsg=on log_buf_len=2m cpuidle.off=1 initcall_debug=0 systemd.log_target=kmsg systemd.log_level=info
irqaffinity=7 systemd.journald.forward_to_kmsg=1 systemd.journald.max_level_kmsg=info ignore_loglevel systemd.mask=systemd-pstore.service lsm=bpf
```

`lsm=bpf`：GKI 内核把 SELinux 编进去了，没有策略时也处于“已启用”状态（会挂载 selinuxfs）；容器里的安卓 init 会把自己的策略加载进宿主内核，把整个系统切到 SELinux。显式列出 LSM 就让 SELinux 保持未初始化
（`selinux=0` 不可用：`CONFIG_SECURITY_SELINUX_BOOTPARAM` 没开）。Ubuntu 这边没有任何东西用 SELinux。`tb330fu-waydroid-preflight` 会检查这一点。

## 4. 根文件系统

```bash
rootfs/collect-vendor-files.sh <vendor/firmware 目录> <vendor_dlkm 的 lib/modules 目录>
sudo -E rootfs/build-rootfs.sh          # ubuntu-base 24.04.5 + 包 + overlay + 模块 + 厂商文件；账号默认锁定
rootfs/make-image.sh                    # -> $TB_DIST/rootfs.ext4（6 GiB 稀疏，标签/UUID 与 vendor_boot 一致）
```

变量：`TB_USER`、`TB_PASSWORD`（空 = 锁定，只能 ssh 密钥）、`TB_SSH_PUBKEY`、`TB_WITH_PLASMA=1`、`TB_DEV=1`（apt 走 PC 代理的开发辅助）、`TB_APT_MIRROR`、`TB_TZ`。
**`build-rootfs.sh` 没有端到端跑通过**（见 README 的“验证情况”）；已有设备上的系统可以直接 `rootfs/capture-live.sh`（过滤掉 NVRAM、ssh 主机密钥、machine‑id、缓存、崩溃标记）再 `make-image.sh`。

## 研究/分析工具

`scripts/research/`：`lk_disasm.py`/`lk_xref.py`/`lk_gbi_stores.py`（LK 的 Thumb‑2 反汇编与交叉引用，推出 OVL 公式用的）、`ko_disasm.py`（.ko 函数反汇编带重定位）、`annotate_*.py`、
`pstore_console.py`（按 ramoops `DBGC` 头取最新一次启动的控制台，不要用时间戳启发）、`module_symdeps.py`/`callsites.py`、`fdtlite.py`/`dtbo_dump.py`、`logo_extract.py`。
