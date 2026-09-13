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

## 阅读顺序

- [`build.md`](build.md)：rootfs 是怎么构建出来的，以及构建环境里踩过的坑。
- [`boot.md`](boot.md)：从 A20OS init 到 XFCE 桌面的一整条启动链路。
- [`kernel-requirements.md`](kernel-requirements.md)：发行版对内核提出了哪些 要求、内核分别在哪里满足。
- [`known-issues.md`](known-issues.md)：已解决的问题（输入、读路径自死锁）的 根因与修法，以及遗留的 dbus 偶发超时怎么排查。
