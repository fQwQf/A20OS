# 构建与运行指南

本文档列出 A20OS 最常用的构建和运行命令。所有命令都在项目根目录执行。更详细的架构设计说明见 [OS-Design.md](OS-Design.md)。

## 环境准备



```bash


```

## 最常用的构建与运行命令

| 命令 | 作用 | 使用场景 |
|------|------|----------|
| `make ARCH=riscv64 BOARD=qemu-virt-riscv64 run` | 编译内核、用户态和文件系统镜像，并在 QEMU 中启动 | 默认开发流程 |
| `make ARCH=riscv64 run` | 同上，`BOARD` 默认等于 `qemu-virt-riscv64` | 快速启动 |
| `make run-riscv64` | 等价于 `make ARCH=riscv64 run` | 记住目标名即可 |
| `make run-loongarch64` / `make run-arm64` / `make run-x86_64` | 在对应架构的 QEMU 中启动 | 跨架构测试 |
| `make run-ppc64le` | 在 QEMU pSeries 中启动 PPC64LE | PPC64LE 单核 bring-up 和 shell 验证 |
| `make run-gui-riscv64` / `make run-gui-aarch64` / `make run-gui-x86_64` / `make run-gui-loongarch64` | 启动带 virtio-gpu 的图形 QEMU；RISC-V/x86_64/LoongArch64 默认挂载 HDA，也可切换 virtio-sound | 测试桌面、GUI 和音频应用 |
| `make ARCH=riscv64 BRINGUP=1 kernel-only` | 仅编译内核，不生成文件系统镜像 | 只改内核、不需要用户态 |
| `make ARCH=riscv64 BRINGUP=1 run` | 仅编译内核并在 QEMU 启动 | 内核 bring-up 测试 |
| `make ARCH=riscv64 NOMMU=1 run` | 以 NOMMU 模式运行 | 测试 NOMMU 路径 |
| `make debug-riscv64` | 用 `-O0 -g -DDEBUG` 编译并启动 QEMU 等待 GDB | 源码级调试 |
| `make all` | 构建 双架构发布产物：`kernel-rv`、`kernel-la`、`disk.img`、`disk-la.img` | 测试平台入口 |
| `make final-all` / `make all-architectures` | `make all` 的显式别名；名称 `all-architectures` 不表示七架构矩阵 | 提交前双架构编译 |
| `make release-all` | 构建保留的双架构入口与脚本 | 回归检查 |
| `make benchmark-rv` / `make benchmark-la` | 单独构建某架构的产物 | 定点复验 |
| `make dev-build` | 生成内核、FAT32 和 ext4 镜像 | 需要完整用户态时 |
| `make kernel-only` | 只生成内核 | 快速编译验证 |
| `make stm32f103-bringup` | 生成 STM32F103 64 KiB Flash / 20 KiB SRAM 固件 | MCU 起步 |
| `make stm32f103-xuanwu` | 生成玄武板 512 KiB Flash / 64 KiB SRAM 固件 | 普中玄武板 |
| `make run-stm32f103-qemu` | 构建 128 KiB Flash / 8 KiB SRAM 固件并启动 `stm32vldiscovery` | 需要 `qemu-system-arm` |
| `make flash-stm32f103-xuanwu` | 构建玄武板固件并通过 OpenOCD 烧录 | 需要 OpenOCD、CMSIS-DAP 和实板 |
| `make -C user ARCH=riscv64` | 单独编译 RISC-V 用户态 | 只改用户程序 |
| `make check-user-build` | 编译主机默认集合的 hosted 用户态（Linux 为七架构，macOS 为 RISC-V64） | 常规提交前检查 |
| `make check-build-matrix-all` | 显式编译七个 hosted 架构的内核 bring-up 和用户态 | 完整跨架构构建检查 |

STM32 固件、QEMU 和烧录目标使用同一套 `BUILD_DIR` 命名。QEMU 运行和实板烧录 仍分别依赖宿主机的 `qemu-system-arm`、OpenOCD 与实际调试硬件；缺少这些环境时可 只运行 `make check-stm32f103` 验证编译。准确产物路径见 [STM32F103 移植说明](platforms/stm32f103-port.md)。

## 发布与调试模式

- 默认 `OPT=-O3`，对应发布构建。
- 调试构建使用 `make debug-<arch>`，它会强制 `OPT="-O0 -g -DDEBUG"` 并启动 QEMU 的 GDB 服务器。
- `MODE` 目前不参与 CFLAGS 或用户态优化选择；不要用 `MODE=debug`/`MODE=release` 表达优化意图。

## 常用变量

- `ARCH`: 目标架构，如 `riscv64`、`aarch64`、`x86_64`、`loongarch64`、`loongarch32`、`ppc64le`、`arm32`、`riscv32`、`armv7m`。
- `BOARD`: 默认 `qemu-virt-<ARCH>`；STM32 时为 `stm32f103`。
- `BRINGUP`: `1` 只编译内核，`0` 编译完整用户态。
- `ABI`: `linux` / `native` / `both`，默认 `both`。
- `NR_CPUS`: 默认 `1`；只有 `riscv64`、`aarch64`、`loongarch64`、`x86_64` 的同名 QEMU virt 板列入已验证 SMP 白名单。
- `NOMMU`: `1` 开启 NOMMU 模式；构建支持 `riscv64`、`riscv32`、`aarch64`、`arm32`、`armv7m`。hosted MMU/NOMMU runtime matrix 只包含前四项，ARMv7-M 走独立 MCU 入口。
- `DRIVER_DEPLOYMENT`: hosted 开发构建通常默认 `generic`，将可发现设备驱动打包为 `.a20drv`；`embedded` 静态链接完整驱动集。ARMv7-M、PPC64LE 和提交使用 embedded。
- `QEMU_GUI_AUDIO_DRIVER`: RISC-V/x86_64/LoongArch64 图形 QEMU 的宿主音频 backend；Linux 默认 `pa`，macOS 默认 `coreaudio`，也可设置为 `pipewire`、`alsa`、`sdl` 或 `none`。
- `QEMU_GUI_AUDIO_DEVICE`: PCM controller，默认 `hda`；设为 `virtio` 时使用 QEMU virtio-sound。
- `GUI_MEDIA`: 可选的 H.264/AAC MP4。仅在命令行显式设置时写入 GUI 镜像的 `/media/demo.mp4`；未设置时桌面和播放器仍会安装，但不会创建默认媒体或播放器 launcher。
- `GUI_DESKTOP`: GUI 默认桌面，默认为 `weston`；可设为 `xfce` 恢复 XFCE/labwc 实验路径。

## Wayland 媒体播放

`run-gui-riscv64`、`run-gui-aarch64`、`run-gui-x86_64` 和 `run-gui-loongarch64` 默认进入 Weston desktop shell，并安装 A20OS SHM desktop helper、精简 FFmpeg 共享库和 `a20-player`。需要测试 XFCE/labwc 时可设置 `GUI_DESKTOP=xfce`。播放器不由 session 自动启动，可通过镜像中的 `/bin/run-player.sh` 启动。

```bash
make run-gui-x86_64 GUI_MEDIA=/path/to/video.mp4
```

启动后，在 Weston 终端中执行：

```sh
/bin/run-player.sh
```

也可以播放镜像中的其他文件：

```sh
/bin/run-player.sh /path/to/video.mp4
```

未设置 `GUI_MEDIA` 时，`run-player.sh` 不带参数会显示用法；仍可执行 `run-player.sh /path/to/video.mp4` 播放镜像中其他位置的媒体。显式指定但文件不存在时，镜像构建会失败，而不是静默换用测试素材。

播放器支持本地 MP4 中的 H.264 视频和 AAC 音频，视频使用 Wayland SHM，音频自动寻找 `/dev/audioN` PCM 设备，并重采样为 48 kHz 双声道 S16_LE。没有 PCM 设备的架构会继续静音播放视频。

## QEMU 音频

`make run-gui-riscv64`、`make run-gui-x86_64` 和 `make run-gui-loongarch64` 默认挂载标准 Intel HDA controller 与 duplex codec。以下命令切换到第二个 PCM backend；RISC-V 使用 VirtIO-MMIO，x86_64 和 LoongArch64 使用 modern VirtIO PCI：

```bash
make run-gui-riscv64 QEMU_GUI_AUDIO_DEVICE=virtio
```

启动后可在终端直接验证 PCM 输出：

```bash
audioplay --tone 440
audioplay music.wav
```

WAV 输入必须是 48 kHz、双声道、S16_LE PCM；原始 PCM 使用 `audioplay --raw file.pcm`。播放器通过 `GET_CAPS` 自动寻找 PCM 设备，不假定具体驱动或动态编号。PCM 客户端可使用 `A20_AUDIO_IOCTL_DRAIN` 等待已提交音频播放完毕，关闭设备时也会自动 drain。宿主使用 PipeWire 而不提供 PulseAudio 兼容服务时，可执行 `make run-gui-x86_64 QEMU_GUI_AUDIO_DRIVER=pipewire`。

##  注意

- `BRINGUP=1` 不生成文件系统镜像；`BRINGUP=0` 才会触发用户态和磁盘构建。
- 默认 `NR_CPUS=1`。RISC-V 64、AArch64、LoongArch64 和 x86_64 的 QEMU virt 平台已验证 SMP，可直接设置 `NR_CPUS>1`；PPC64LE 当前仅验证 QEMU pSeries 单核路径。其他架构或板卡仍会被构建系统拒绝，除非显式设置 `ALLOW_UNVERIFIED_SMP=1`。
-  `make all` 构建 RISC-V64 与 LoongArch64，使用 `PROFILE=benchmark NR_CPUS=8 DRIVER_DEPLOYMENT=embedded EXTERNAL_ROOT=1`，输出根目录的 `kernel-rv`、`kernel-la`、`disk.img`、`disk-la.img`。默认优化仍来自 `OPT=-O3`；若需要改变优化必须使用 `OPT`。parallel-build 设计与复现说明见 [parallel-build-2026.md](parallel-build-2026.md)。
- `ARCH=armv7m` 需要 `arm-none-eabi-gcc` 或 `clang` + `llvm-objcopy`。
- 图形 QEMU 目标依赖宿主机显示能力；无图形环境请使用普通 `run-*` 目标。
- 不要直接仿照 Makefile 外的 QEMU 参数手写启动命令，容易遗漏 `-bios default` 等关键选项。
