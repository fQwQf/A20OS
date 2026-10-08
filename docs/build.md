# 构建与运行指南

A20OS 最常用的构建和运行命令都在项目根目录执行。更详细的架构设计说明见 [OS-Design.md](OS-Design.md)。

> 构建/运行/冒烟配置也可以用 `instances/` 下的 TOML 实例文件声明，
> 通过 `tools/a20 run|build|debug|test <实例>` 驱动；逐架构的 `run-*`/`debug-*`/`smoke-*`
> make 目标已是它的薄包装。字段参考、组件注册表与门禁说明见 [instances.md](instances.md)。

> 把构建产物打成 apk 包、按清单组合镜像、发布到仓库的体系见
> [packaging/overview.md](packaging/overview.md)；CI 容器（可复现构建环境）见
> [packaging/ci.md](packaging/ci.md)。

## 环境准备

需要本机安装各架构交叉工具链与镜像工具。Debian/Ubuntu 上一次装齐：

```bash
sudo apt-get install -y \
    build-essential curl git vim wget xz-utils file ripgrep \
    dosfstools e2fsprogs mtools \
    grub-pc-bin grub-efi-amd64-bin xorriso \
    qemu-system-misc qemu-system-x86 qemu-system-arm openocd \
    gcc-riscv64-unknown-elf gcc-riscv64-linux-gnu g++-riscv64-linux-gnu \
    gcc-aarch64-linux-gnu gcc-x86-64-linux-gnu gcc-powerpc64le-linux-gnu \
    gcc-arm-linux-gnueabihf gcc-arm-none-eabi binutils-arm-none-eabi \
    python3 python3-pip rustc cargo
```

注意两点：

- LoongArch64：Ubuntu 24.04 的 apt 源没有 `gcc-loongarch64-linux-gnu`，需要单独安装 Loongson 官方交叉工具链。
- LoongArch32（LA32R）：**发行版完全没有**这个包，需要按 [platforms/loongarch32.md](platforms/loongarch32.md) 从源码构建 cloudspurs 的 binutils/gcc la32 分支。没有它就跑不了 `make check-loongarch32-bringup`，该门禁也因此只在装了自建工具链的本机跑，不进 CI。
- arm32：需要 `gcc-arm-linux-gnueabihf` 与 `linux-libc-dev-armhf-cross`（Debian/Ubuntu 有）；`tools/ci/Dockerfile` 会装交叉编译器。后者为 fastfetch 提供 ARM Linux UAPI 头文件。若只把交叉工具链解包到临时目录，可将 `FF_LINUX_UAPI_ROOT` 指向包含 `<triple>/include` 的根目录，例如 `make FF_LINUX_UAPI_ROOT=/tmp/arm-root/usr check-build-matrix`。ARMv7-M 另走 STM32 目标，工具链不同。
- ARM32 musl 的 `time_t` 是 64 位，直接 syscall 回归 `timer_edge` 因此使用 `timer_settime64` / `timer_gettime64` 编号和同宽的 `itimerspec` 布局；不要把 32 位 `timer_*32` 编号与 64 位结构体混用。
- Python：Makefile 默认调用 `python3`（Python 3.11 或更新版本，仅需标准库）。不需要 Conda，也不会因机器安装了 Conda 而自动切换解释器。可通过 `make PYTHON=/path/to/python3 ...` 显式指定其他解释器。
- `grub-pc-bin` / `grub-efi-amd64-bin` / `xorriso`：只有 x86_64 瘦客户机部署用得到。`tools/a20 package x86_64-pc` 生成 GRUB rescue ISO 时，`grub-mkrescue` 会调用 `xorriso`；缺了它会在打包这一步报 `grub-mkrescue: 未找到 xorriso`。riscv64/aarch64 板和 QEMU 虚拟机都不需要这三个包。

## 最常用的构建与运行命令

| 命令 | 作用 | 使用场景 |
|------|------|----------|
| `make ARCH=riscv64 BOARD=qemu-virt-riscv64 run` | 编译内核、用户态和文件系统镜像，并在 QEMU 中启动 | 默认开发流程 |
| `make ARCH=riscv64 run` | 同上，`BOARD` 默认等于 `qemu-virt-riscv64` | 快速启动 |
| `make run-riscv64` | 等价于 `make ARCH=riscv64 run` | 记住目标名即可 |
| `make run-loongarch64` / `make run-arm64` / `make run-x86_64` | 在对应架构的 QEMU 中启动 | 跨架构测试 |
| `make run-ppc64le` | 在 QEMU pSeries 中启动 PPC64LE | PPC64LE 单核 bring-up 和 shell 验证 |
| `make ARCH=riscv64 BRINGUP=1 kernel-only` | 仅编译内核，不生成文件系统镜像 | 只改内核、不需要用户态 |
| `make ARCH=riscv64 BRINGUP=1 run` | 仅编译内核并在 QEMU 启动 | 内核 bring-up 测试 |
| `make ARCH=riscv64 NOMMU=1 run` | 以 NOMMU 模式运行 | 测试 NOMMU 路径 |
| `make debug-riscv64` | 用 `-O0 -g -DDEBUG` 编译并启动 QEMU 等待 GDB | 源码级调试 |
| `make all` | 构建双架构发布产物：`kernel-rv`、`kernel-la`、`disk.img`、`disk-la.img` | 发布构建入口 |
| `make dev-build` | 生成内核、FAT32 和 ext4 镜像 | 需要完整用户态时 |
| `make kernel-only` | 只生成内核 | 快速编译验证 |
| `make stm32f103-bringup` | 生成 STM32F103 64 KiB Flash / 20 KiB SRAM 固件 | MCU 起步 |
| `make stm32f103-xuanwu` | 生成玄武板 512 KiB Flash / 64 KiB SRAM 固件 | 普中玄武板 |
| `make run-stm32f103-qemu` | 构建 128 KiB Flash / 8 KiB SRAM 固件并启动 `stm32vldiscovery` | 需要 `qemu-system-arm` |
| `make flash-stm32f103-xuanwu` | 构建玄武板固件并通过 OpenOCD 烧录 | 需要 OpenOCD、CMSIS-DAP 和实板 |
| `make -C user ARCH=riscv64` | 单独编译 RISC-V 用户态 | 只改用户程序 |
| `make check-user-build` | 编译主机默认集合的 hosted 用户态（Linux 为七架构，macOS 为 RISC-V64） | 常规提交前检查 |
| `make check-build-matrix-all` | 显式编译七个 hosted 架构的内核 bring-up 和用户态 | 完整跨架构构建检查 |

STM32 固件、QEMU 和烧录目标使用同一套 `BUILD_DIR` 命名。QEMU 运行和实板烧录仍分别依赖宿主机的 `qemu-system-arm`、OpenOCD 与实际调试硬件；缺少这些环境时可只运行 `make check-stm32f103` 验证编译。准确产物路径见 [STM32F103 移植说明](platforms/stm32f103-port.md)。

## 声明式构建配置（instances/ 与 components/trim.toml）

Makefile 之外的构建配置有两个声明式入口，日常应优先使用它们而不是直接拼 make 变量：

- **实例清单** `instances/*.toml`：一个文件描述一个完整可构建/可运行/可测试的系统实例，`tools/a20 build|run|flash|... <实例>` 负责校验并推导 make 变量。字段参考见 [instances.md](instances.md)。
- **剪裁注册表** `components/trim.toml`：内核剪裁的**策略**数据——构建 profile 策展的源码清单（如 `[profile.mcu]`）、各可选能力的架构矩阵（nommu/swap/xlator/ramfs-user/SMP 已验证平台/PCIe MMIO）、每架构固定特性宏。Makefile 经生成的 `components/trim.mk` 读它（`NOMMU_SUPPORTED_ARCHES`、`SWAP_SUPPORTED_ARCHES`、MCU 源码列表都出自这里），`tools/a20` 校验实例时读同一份 TOML。改完 TOML 执行 `make regen-trim-fragment` 重新生成；`make check-trim-registry` 会拒绝过期的 fragment、缺失的条目和没有消费者的死配置。

想知道某个实例实际按什么剪裁构建，用 `tools/a20 trim <实例>` 查看完整计划（profile、各能力开关及理由、内存预算）。

## 发布与调试模式

- 默认 `OPT=-O3`，对应发布构建。
- 调试构建使用 `make debug-<arch>`，它会强制 `OPT="-O0 -g -DDEBUG"` 并启动 QEMU 的 GDB 服务器。

## 常用变量

- `ARCH`: 目标架构，如 `riscv64`、`aarch64`、`x86_64`、`loongarch64`、`loongarch32`、`ppc64le`、`arm32`、`riscv32`、`armv7m`。
- `BOARD`: 默认 `qemu-virt-<ARCH>`；STM32 时为 `stm32f103`。
- `BRINGUP`: `1` 只编译内核，`0` 编译完整用户态。
- `ABI`: `linux` / `native` / `both`，默认 `both`。
- `NR_CPUS`: 默认 `1`；只有 `riscv64`、`aarch64`、`loongarch64`、`x86_64` 的同名 QEMU virt 板列入已验证 SMP 白名单（出处：`components/trim.toml` 的 `smp-verified-qemu` 矩阵）。
- `CONFIG_KERNEL_PREEMPT`: 默认 `1`（hosted 架构内核态抢占，机制见 `docs/process-scheduler.md` §4.1）；MCU profile（armv7m）固定关闭。设为 `0` 回到协作式内核。该开关参与 `BUILD_VARIANT`（构建目录带 `-preempt` 后缀），因此翻转它不会复用旧构建目录里的陈旧目标文件。
- `NOMMU`: `1` 开启 NOMMU 模式；构建支持 `riscv64`、`riscv32`、`aarch64`、`arm32`、`armv7m`（出处：`components/trim.toml` 的 `nommu` 矩阵）。hosted MMU/NOMMU runtime matrix 只包含前四项，ARMv7-M 走独立 MCU 入口。
- `DRIVER_DEPLOYMENT`: hosted 开发构建通常默认 `generic`，将可发现设备驱动打包为 `.a20drv`；`embedded` 静态链接完整驱动集。ARMv7-M、PPC64LE 和发布构建使用 embedded。
- `QEMU_GUI_AUDIO_DRIVER`: RISC-V/x86_64/LoongArch64 图形 QEMU 的宿主音频 backend；Linux 默认 `pa`，macOS 默认 `coreaudio`，也可设置为 `pipewire`、`alsa`、`sdl` 或 `none`。
- `QEMU_GUI_AUDIO_DEVICE`: PCM controller，默认 `hda`；设为 `virtio` 时使用 QEMU virtio-sound。
- `QEMU_GUI_AUDIO_DEVICE`: PCM controller，默认 `hda`；设为 `virtio` 时使用 QEMU virtio-sound。

> 桌面环境已改为 Alpine xfce world（`make image-world PKG_WORLD=xfce` 组镜像，`make run-world-gui PKG_WORLD=xfce` 以 GUI 启动）。原先基于 `user/wayland` + `user/external/gui` 从源码构建的 Weston/XFCE 路径与 LVGL 原生桌面已退役，归档在 `archive/legacy-desktop`。

## QEMU 音频

`make run-world-gui PKG_WORLD=xfce` 默认挂载标准 Intel HDA controller 与 duplex codec。以下命令切换到第二个 PCM backend；RISC-V 使用 VirtIO-MMIO，x86_64 和 LoongArch64 使用 modern VirtIO PCI：

```bash
make run-world-gui PKG_WORLD=xfce QEMU_GUI_AUDIO_DEVICE=virtio
```

启动后可在终端直接验证 PCM 输出：

```bash
audioplay --tone 440
audioplay music.wav
```

WAV 输入必须是 48 kHz、双声道、S16_LE PCM；原始 PCM 使用 `audioplay --raw file.pcm`。播放器通过 `GET_CAPS` 自动寻找 PCM 设备，不假定具体驱动或动态编号。PCM 客户端可使用 `A20_AUDIO_IOCTL_DRAIN` 等待已提交音频播放完毕，关闭设备时也会自动 drain。宿主使用 PipeWire 而不提供 PulseAudio 兼容服务时，可执行 `make run-world-gui PKG_WORLD=xfce QEMU_GUI_AUDIO_DRIVER=pipewire`。

## 注意

- `BRINGUP=1` 不生成文件系统镜像；`BRINGUP=0` 才会触发用户态和磁盘构建。
- 顶层构建可能同时从磁盘镜像与 RAMFS guest kernel 进入用户构建。`tools/stamps.py` 会按共享的 `user/build/<arch>[-nommu]` 输出根使用跨进程锁，并在拿锁后重新检查 stamp；因此不同 `OPT`/profile 或 user/native stamp 的独立 make 进程不会并发清理或写入同一目录。锁文件位于该目录的父目录，不会随变体 clean 一起删除。
- `make clean` 会清除当前仓库配置的 `.kernel-build` 内容。若该路径是指向专用外部构建缓存的符号链接，命令会保留链接并清空其目标中的架构构建目录。首次使用外部缓存时，先确认目标目录专用于 A20OS，再创建标记文件：`printf 'a20-build-root-v1\n' > /path/to/cache/.a20-build-root`。清理会验证该标记、拒绝指向仓库本身/父目录的链接，并拒绝不符合架构构建目录命名约定的内容，以免误删无关文件。
- 默认 `NR_CPUS=1`。RISC-V 64、AArch64、LoongArch64 和 x86_64 的 QEMU virt 平台已验证 SMP，可直接设置 `NR_CPUS>1`；PPC64LE 当前仅验证 QEMU pSeries 单核路径。其他架构或板卡仍会被构建系统拒绝，除非显式设置 `ALLOW_UNVERIFIED_SMP=1`。
- 发布构建 `make all` 构建 RISC-V64 与 LoongArch64，使用 `PROFILE=benchmark NR_CPUS=8 DRIVER_DEPLOYMENT=embedded EXTERNAL_ROOT=1`，输出根目录的 `kernel-rv`、`kernel-la`、`disk.img`、`disk-la.img`。默认优化仍来自 `OPT=-O3`；若需要改变优化必须使用 `OPT`。
- `ARCH=armv7m` 需要 `arm-none-eabi-gcc` 或 `clang` + `llvm-objcopy`。
- 图形 QEMU 目标依赖宿主机显示能力；无图形环境请使用普通 `run-*` 目标。
- 不要直接仿照 Makefile 外的 QEMU 参数手写启动命令，容易遗漏 `-bios default` 等关键选项。
