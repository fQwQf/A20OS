# A20OS 发行版 rootfs 运行（`make distro-run`）

A20OS 当前唯一的 XFCE Wayland 桌面路径就是本目录描述的 **distro 路径**：直接用包管理器拉一个原生 Alpine Linux 发行版做用户态，A20OS 只当内核。

（历史上还有一条 from-source 路径：`user/wayland/` 下自研编译的 wlroots/labwc/xfce 组件，dbus/elogind/seatd/eudev 等服务层用 stub 替代。该路径与 LVGL 原生桌面已退役，归档在分支 `archive/legacy-desktop`，见 `docs/graphics/xfce-wayland-adaptation.md`。）

distro 路径的价值，在于它没有"改自研组件绕过去"的自由度——内核要么把行为做到位，要么桌面起不来。为此在 内核侧补齐了一批 Linux 行为（netlink uevent、`PR_SET_PDEATHSIG`、`/sys/dev/char`、DRM 能力位、唯一 KMS 对象 ID 等），这些内容单独写在 `kernel-requirements.md`。

## 快速开始

```sh
make distro-run              # 默认 riscv64：构建内核 + rootfs + 启动 QEMU(GUI)
make distro-run-riscv64
make distro-run-x86_64
make distro-run ARCH=x86_64 QEMU_GUI_DISPLAY=none   # 无显示器环境

# 只构建 rootfs，不起 QEMU
make rootfs-alpine ARCH=riscv64
```

`make distro-run` 做三件事：`dev-build` 编内核、`rootfs-alpine` 出发行版镜像、 最后用 `QEMU_GUI_DISPLAY` 指定的显示后端启动。磁盘布局是 `dev0=fat32.img` （A20OS 自研用户态）+ `dev1=rootfs.img`（发行版），内核从 fat32 引导， 发行版 rootfs 挂到 `/extra`。

### 用实例放视频（`run-gui-*` + `GUI_MEDIA`）

`xfce` world 的桌面实例可以直接带媒体文件启动：`GUI_MEDIA` 接受空格分隔的
多个文件或目录，全部注入镜像的 `/usr/share/a20-media/`，并在 `~/Desktop/`
放一个 `a20-media` 链接（双击即可在文件管理器里打开）。

```sh
make run-gui-x86_64                                    # 只启动桌面
make run-gui-x86_64 GUI_MEDIA=~/Videos/demo.mp4        # 注入一个视频
make run-gui-x86_64 GUI_MEDIA="a.mp4 b.mkv clips/"     # 多文件 + 目录
make run-gui-riscv64 GUI_MEDIA=~/Videos/demo.mp4       # 其他架构同理
```

`run-gui-<arch>` 等价于 `tools/a20 run xfce-<arch>`（先 `image-world` 再 GUI
启动），只构建镜像用 `make ARCH=<arch> image-world PKG_WORLD=xfce
GUI_MEDIA=...`。

镜像内自带的播放器：**ffplay**（推荐；ffmpeg 的软件解码，实测 guest 内 15/15 次播放/解码全过、0 崩溃）、
**parole**（GStreamer 后端）、**mpv**（命令行）。注意 **mpv 目前约 1/10 次崩溃**——它每个内建 Lua 脚本跑在自己的
线程里，而 A20OS 的 x86_64 每线程状态有 bug（详见 [known-issues.md](known-issues.md)）；用 ffplay 或重试即可。
命令行播放：`ffplay /usr/share/a20-media/demo.mp4`（`Super+Enter` 开终端），或在 Thunar 里双击。

### 桌面里的 JVM 与图形栈

- **JVM**：world 装了 `openjdk21-jre`。`/usr/bin/java` 是 overlay 提供的
  薄封装（直接 exec 真实路径 + 给一组默认堆参数），原因见
  [known-issues.md](known-issues.md) 的 JVM 条目。`java -version` 可用。
- **图形 API**：world 装了 Mesa（`mesa`/`mesa-dri-gallium`/`mesa-gl`/
  `mesa-gles`/`mesa-egl`/`mesa-gbm` + `mesa-utils`/`mesa-demos`）与
  `virglrenderer`，镜像里有 `swrast`/`kms_swrast`/`virtio_gpu`/`zink` DRI
  驱动和 `libGL/libEGL/libgbm`。但 **GL 客户端目前出不了图**：内核的 DRM
  是 dumb-buffer KMS，缺 GEM 对象分配与 render node，PRIME 也只是 memfd
  拷贝。细节与 Minecraft 的前置条件见
  [../graphics/3d-graphics.md](../graphics/3d-graphics.md) 第 8 节。

## 阅读顺序

- [`build.md`](build.md)：rootfs 是怎么构建出来的，以及构建环境里踩过的坑。
- [`boot.md`](boot.md)：从 A20OS init 到 XFCE 桌面的一整条启动链路。
- [`kernel-requirements.md`](kernel-requirements.md)：发行版对内核提出了哪些 要求、内核分别在哪里满足。
- [`known-issues.md`](known-issues.md)：已解决的问题（输入、读路径自死锁）的 根因与修法，以及遗留的 dbus 偶发超时怎么排查。
