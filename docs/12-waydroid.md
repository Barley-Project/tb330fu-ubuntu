# 12 Waydroid（Android 13 / LineageOS 20，LXC 容器）

状态：用自编内核（[09-build.md](09-build.md) 的 1b 节）加命令行 `lsm=bpf`，Waydroid 能装、能起，安卓界面经 Panfrost 渲染出来。**没有声音**（没有声卡，见 [10-known-issues.md](10-known-issues.md)）。
官方的 `boot-debug.img` 不行：缺 `PID_NS`、`IPC_NS`、`USER_NS`。

## 内核与命令行的要求

* `kernel-config/tb330fu.config`：`PID_NS`、`USER_NS`、`IPC_NS`（靠 `POSIX_MQUEUE`，不是 `SYSVIPC`）、`FHANDLE`、`NETFILTER_XT_TARGET_CHECKSUM`、`DMABUF_HEAPS_SYSTEM` 等。
  `CGROUP_DEVICE`、`CGROUP_PIDS` **不能**开（会改约 6000 个导出符号的 CRC，厂商模块全部加载失败）；cgroup v2 上容器的设备访问控制走 BPF 的 cgroup device 程序（`CGROUP_BPF` 已开），LXC 在 v2 上就是这样用的。
* 内核命令行 `lsm=bpf`（`vendor-boot/build-vendor-boot.sh`）：让内核里的 SELinux 保持未初始化，否则容器里的安卓 init 会把自己的策略装进宿主内核，把整个系统切到 SELinux。
* 预检：`tb330fu-waydroid-preflight`（只读，不会挂载 binderfs 也不会启动 Waydroid）检查 `/proc/config.gz` 里的选项、SELinux 是否未激活、binder 文件系统、cgroup v2、GPU 渲染节点、命名空间和 `memfd_create`；有一项不通过就别装。

## 安装

在平板上，root：

```bash
sudo tb330fu-waydroid-setup [--images DIR] [VANILLA|GAPPS]
```

步骤：预检 → waydroid 的 apt 源 → 软件包 → `iptables-legacy`（内核没有 nf_tables）→ 镜像 + `waydroid init` → 开机启动容器服务。镜像放在 `/usr/share/waydroid-extra/images`（Waydroid 自己会去这里找），有 `system.img` 和 `vendor.img` 时不会再下载。

镜像约 1 GB，平板自己走 Wi‑Fi 下载很慢。可以在 PC 上用 `tools/fetch-waydroid-images.py`（多连接分块下载、可走代理、校验 Waydroid OTA 服务器公布的 sha256）下好，再用 `tools/tb.sh put` 拷过去：

```bash
python3 tools/fetch-waydroid-images.py --out waydroid-images [--proxy http://127.0.0.1:17897]
tools/tb.sh put waydroid-images/*.zip /var/tmp/
tools/tb.sh sudo tb330fu-waydroid-setup --images /var/tmp
```

装好之后，**在 GNOME 会话里**（不是 ssh）：

```bash
waydroid session start &
waydroid show-full-ui
```

`waydroid session start` 需要 Wayland 显示和会话总线，ssh 里会报 “Unable to autolaunch a dbus-daemon without a $DISPLAY”。`waydroid container {start,stop,restart,freeze,unfreeze}` 只管 LXC 容器，不需要会话。
`waydroid shell` 里要在命令前加 `--`（`waydroid shell -- su -c id`），否则 `-c` 之类的参数会被 argparse 当成它自己的。

## 可选：GApps、Magisk

[casualsnek/waydroid_script](https://github.com/casualsnek/waydroid_script)（第三方，不在本仓库里）把东西装进 Waydroid 的 overlay（`/var/lib/waydroid/overlay`），不碰 boot.img。参数 `-a/--android-version` 要放在子命令**前面**：

```bash
sudo python3 main.py -a 13 install gapps     # MindTheGapps 13
sudo python3 main.py -a 13 install magisk    # Magisk Delta
```

* GApps：装好后 `pm list packages` 里有 `com.google.android.gms`、`com.android.vending` 等。没有做 Play 认证。
* Magisk Delta：通过改写 Android 的 `bootanim.rc` 注册 init 触发器（不是 boot ramdisk）；作者在平板上确认它生效。`magisk -V` 报 26301。
* Zygisk 模块（ReZygisk、LSPosed、Shamiko）：试过手工安装，**没有跑通，已放弃**。遇到的坑：
  `magisk --install-module` 在这个构建上报 “Incomplete Magisk install”，只能把模块解压到 `/data/adb/modules/<id>/`（宿主上是 `~/.local/share/waydroid/data/adb/modules/`），这样模块的 `customize.sh` 不会执行，它做的事要手工补（ReZygisk、LSPosed 把各架构目录里的二进制摊平；LSPosed 往 `daemon.apk` 和 `dex2oat` 里替换一个随机生成的路径串）；
  Shamiko 的 `post-fs-data.sh` 里写死了 `MODULE_MIN_MAGISK_VERSION=27005`，Magisk 版本低于它就自己写 `disable` 标记；Shamiko 的 zip 用 XZ 压缩，Python 的 `zipfile` 和 Ubuntu 的 `unzip` 都解不开，要用 `7z x`。
  Magisk Delta 自带 Zygisk 开关（`magisk --sqlite "select * from settings"` 里的 `zygisk`，查到时已经是 1），不需要 ReZygisk。

## 已知问题

在 Magisk Delta 里设置 Zygisk 并重启之后，Waydroid 起不来：容器启动后约 2.5 分钟停掉，`waydroid status` 变成 `Session: RUNNING`、`Container: STOPPED`。原因**没查出来**：

* 宿主 `dmesg` 里没有 OOM、panic、segfault；内存充裕（7.7 GiB 里用了不到 1 GiB）。
* `waydroid.log` 里 `lxc.hook.post-stop` 报 “Script exited with status 126”，但有意停止容器时同样会报，不是原因。
* 更早一次启动留下的 tombstone 是 `android.hardware.graphics.composer@2.1-service`（运行 2 秒时）的 abort，信息 “Binder threadpool cannot be shrunk after starting”；它和这次停止有没有关系没确认。
* 怀疑：手工放进 `/data/adb/modules` 的 ReZygisk、LSPosed、Shamiko 与 Magisk 自带的 Zygisk 同时在往 zygote 里注入（**未验证**）。下次先把这三个模块目录移走再试。
