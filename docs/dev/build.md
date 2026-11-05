# 构建与运行指南

本指南列出工具链前提、构建/运行命令和常用配置。当前实现与验证概况见 [模块状态](../status/current/module-status.md)，实现选择与发布条件见 [1.0 计划](../releases/v1.0.md)。[1.0 平台范围](../releases/v1.0.md#2-平台范围) 已排除 LoongArch32/PPC64LE。活跃 Make、实例、CI 和测试矩阵已停止列入这两个历史目标；默认构建会明确拒绝它们。历史源码与实例保留供追溯。需要复现时，在干净 checkout 中切到 `v0.18`，再按各平台文档操作；不要把历史构建记录解读为当前支持承诺。

A20OS 最常用的构建和运行命令都在项目根目录执行。更详细的架构设计说明见 [architecture.md](../architecture.md)。

> 构建/运行/冒烟配置也可以用 `instances/` 下的 TOML 实例文件声明，
> 通过 `tools/a20 run|build|debug|test <实例>` 驱动；逐架构的 `run-*`/`debug-*`/`smoke-*`
> make 目标已是它的薄包装。字段参考、组件注册表与门禁说明见 [instances.md](instances.md)。

> 把构建产物打成 apk 包、按清单组合镜像、发布到仓库的体系见[packaging/overview.md](../packaging/overview.md)；CI 容器（可复现构建环境）见[packaging/ci.md](../packaging/ci.md)。

## 首次运行：Linux + RISC-V 64 {#首次运行linux--risc-v-64}

推荐先完成这一条串口路径，再选择桌面或其他架构。无需开发板、宿主图形界面或硬件虚拟化；QEMU 使用软件模拟。本文的原生工具链基线是 x86_64 Debian 13、 GCC 14.2、QEMU 10.0.13、Python 3.13、Rust 1.96.0。Ubuntu 24.04 的安装入口也列在下方，但其发行版工具链组合应以实际构建和 smoke 结果为准。

推荐宿主 8 GiB 内存、20 GiB 可用磁盘，初次用 `-j4` 控制编译并发；容器镜像还会占用 Docker 存储空间。实际用量与配置有关，这些是准备预算。串口 guest 默认 1 GiB 内存；运行前会检查可用内存、磁盘及其他实例，资源不足时可能等待，见下方排查表。

### 1. 获取完整源码

先安装 Git（Debian/Ubuntu：`sudo apt-get update && sudo apt-get install -y git`），再执行：

```bash
git clone --recurse-submodules https://github.com/fQwQf/A20OS.git
cd A20OS
```

已有普通 clone 时执行 `git submodule update --init --recursive`。 fastfetch 是固定 commit 的子模块，普通 clone 或 GitHub 源码 ZIP 不包含它的内容。当前基础用户态构建会跳过缺席的 fastfetch，所以“编译完成”不保证拿到了完整程序集合；下方环境检查会明确报出这个缺项。更新代码后再次执行子模块初始化命令。

### 2. 安装单架构依赖

以下清单只为 RISC-V64 串口开发镜像准备，不要求安装 LoongArch、ARM 或桌面依赖：

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential git curl ca-certificates xz-utils file python3 \
    dosfstools e2fsprogs mtools util-linux \
    gcc-riscv64-unknown-elf binutils-riscv64-unknown-elf \
    gcc-riscv64-linux-gnu libc6-dev-riscv64-cross

# Debian 13：
sudo apt-get install -y qemu-system-riscv
# Ubuntu 24.04 用下面这一条替代上一条：
# sudo apt-get install -y qemu-system-misc
```

Python 必须为 3.11 或更新版本，仅需标准库；此路径不需要 pip、Conda、CMake 或 GRUB。`riscv64-unknown-elf-gcc` 编译内核和静态用户态；Linux 交叉工具链与交叉头文件供包括 fastfetch 在内的构建使用。显式安装交叉 libc 头，避免 `--no-install-recommends`下出现编译器在而头文件不在的情况。

Rust 使用 rustup 管理。尚未安装 rustup 时执行：

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o /tmp/a20-rustup-init.sh
sh /tmp/a20-rustup-init.sh -y --profile minimal --default-toolchain 1.96.0
. "$HOME/.cargo/env"
```

已有 rustup 的用户也执行以下步骤：

```bash
rustup toolchain install 1.96.0 --profile minimal
rustup target add --toolchain 1.96.0 \
    riscv64imac-unknown-none-elf riscv64gc-unknown-none-elf
rustc --version
python3 tools/check_quickstart.py
```

仓库的 `rust-toolchain.toml` 选择 1.96.0，不改变仓库外的默认版本。确保`$HOME/.cargo/bin` 在 PATH 前部；已有 `RUSTUP_TOOLCHAIN` 环境变量或 rustup 目录覆盖会优先于该文件，应清除不再需要的覆盖。发行版 `rustc` 不会读取 rustup 的版本选择。 RISC-V64 首次运行只需要上述两个预编译 core，不需要 `rust-src`。环境检查使用临时文件试编译，检查工具、目标 core、交叉头文件与子模块；不会安装依赖，也不证明整个内核可以编译。

### 3. 编译、启动与退出

在仓库根目录运行，编译和 QEMU 都使用普通用户权限：

```bash
tools/a20 run qemu-riscv64 -- -j4
```

这会构建内核、用户态及磁盘镜像，再启动串口 QEMU。首次构建比后续增量构建慢；不要同时在同一 checkout 启动多个构建或修改正在被 guest 使用的磁盘镜像。看到 shell 提示符后执行：

```sh
/bin/echo hello
poweroff
```

应看到单独一行 `hello`，随后看到 `System is going down for power-off NOW`， QEMU 退出并返回宿主 shell。`poweroff` 是正常退出方式；如 guest 已无响应，串口 QEMU 可以先按 `Ctrl-a`、再按 `x` 强制退出，但这不算正常关机验收。

也可以自动检查完整流程：

```bash
make -j4 smoke-quickstart
```

该门禁使用与首次运行相同的 both ABI 开发镜像，验证 shell 内置命令、外部程序`/bin/echo`、关机标记和 QEMU 的 0 退出码；超时或异常退出都失败。结果在 `.kernel-build/smoke/smoke-quickstart.log`。`make smoke-riscv64` 只验证 kernel-only bring-up，不能代替这项用户态验收。

裸 `make` / `make all` 是 RISC-V64 + LoongArch64 双架构发布构建，不是首次体验入口。只编译不启动可用 `tools/a20 build qemu-riscv64 -- -j4`。

### 可选：使用构建容器

有 Docker 的用户可在获取完整源码后，用仓库内 Dockerfile 构建单架构环境，省去宿主工具链安装。需要 Docker daemon 可用，且能访问 Debian 仓库和 Rust 下载服务。不依赖预先发布的 GHCR tag；本地旧镜像不会因为源码更新而自动更新。

```bash
tools/ci/build-image.sh riscv
docker run --rm -it -v "$PWD:/src" -w /src a20os-buildenv:riscv bash
# 以下在容器内执行：
python3 tools/check_quickstart.py
tools/a20 run qemu-riscv64 -- -j4
```

脚本先构建镜像，再自检其工具链；Dockerfile 固定 Rust 版本，但 Debian 包随仓库更新，这不是按包哈希冻结的环境。容器默认以 root 写挂载目录，产物可能归 root；需要改回宿主构建时，在退出容器后按需执行 `sudo chown -R "$(id -u):$(id -g)" .kernel-build user/build build`。更多镜像、缓存和 CI 说明见 [CI/CD](../packaging/ci.md)。

### 常见阻塞

| 现象 | 处理 |
| --- | --- |
| `rustup: command not found` | 安装 rustup，并执行 `. "$HOME/.cargo/env"`；仅 apt 安装 rustc/cargo 不足以完成本指南 |
| `can't find crate for core` | 按上述命令给 **1.96.0** 安装两个 target；用 `rustup show active-toolchain` 检查覆盖和 PATH |
| `tomllib` / `assert_never` 导入失败 | 当前 Python 太旧，换用 Python 3.11+ |
| `bits/libc-header-start.h` 或 Linux UAPI 头缺失 | 安装对应架构的 `libc6-dev-*-cross`，不要仅安装交叉 gcc |
| 环境检查报 fastfetch 缺失 | 执行 `git submodule update --init --recursive`；检查 GitHub 网络连通性 |
| 启动前显示资源不足并等待 | `tools/a20 resources` 查看预算，释放内存/磁盘或停止不用的实例；用 `tools/a20 run qemu-riscv64 --wait-timeout 60 -- -j4` 限制等待 |
| 镜像被占用或实例 busy | 退出该 checkout 的已有 QEMU；确认实例停止后重试 |
| 环境检查通过，但 C/Rust 编译失败 | 保存完整命令、`git rev-parse HEAD`、编译器版本和首个错误；这是构建问题，不应关闭 `-Werror` 假装成功 |
| GUI 启动失败 | 串口快速路径不需要显示服务；桌面额外需要显示、图形与音频后端，按 [发行版文档](../distro/README.md) 配置 |
| Alpine world 下载返回 403 或超时 | world 构建可加 `ALPINE_MIRROR_ROOT=https://dl-cdn.alpinelinux.org/alpine`；串口快速路径不下载 Alpine world |

## 其他平台与当前边界

Linux 原生快速路径是本页的验收重点。Windows 用户可在 Linux 虚拟机或 WSL2 中尝试， macOS 用户可尝试 Linux 容器；本页没有对这些宿主组合做端到端验收，不承诺原生命令通用。桌面能启动、内核 bring-up 通过、普通 shell 可用和真机可用是不同层级，请同时核对 [交付基线](../status/current/delivery-baseline.md) 与 [缺口台账](../status/gaps/README.md)。 STM32 当前有 `ARCH=armv7m` 的 Rust target 解析期阻塞，安装 ARM gcc 不会解决它；相关命令仅是维护入口，状态见 [驱动缺口](../status/gaps/drivers.md)。

需要扩展架构时，在通用工具与 rustup 已安装的基础上按目标补依赖：

| 架构 | Debian 13 工具链 / QEMU 包 | 1.96.0 Rust target |
| --- | --- | --- |
| x86_64 | `gcc-x86-64-linux-gnu qemu-system-x86` | `x86_64-unknown-none` |
| AArch64 | `gcc-aarch64-linux-gnu libc6-dev-arm64-cross qemu-system-arm` | `aarch64-unknown-none`，另装 `rust-src` |
| LoongArch64 | `gcc-loongarch64-linux-gnu libc6-dev-loong64-cross qemu-system-misc` | `loongarch64-unknown-none` |
| ARM32 | `gcc-arm-linux-gnueabihf libc6-dev-armhf-cross qemu-system-arm` | `armv7a-none-eabihf` |
| RISC-V32 | 与 RISC-V64 共用工具链和 QEMU | 自定义目标，另装 `rust-src` |
| ARMv7-M | `gcc-arm-none-eabi binutils-arm-none-eabi qemu-system-arm openocd` | `thumbv7m-none-eabi`，当前仍有上述构建阻塞 |

按所选架构执行 `rustup target add --toolchain 1.96.0 <表中的目标>`。 AArch64 内核使用 `aarch64-unknown-none-softfloat` 自建 core，用户态使用`aarch64-unknown-none`；RISC-V32 使用自定义 JSON 目标`tools/rust/targets/riscv32imafdc-a20.json`。这两种内核构建需要：

```bash
rustup component add --toolchain 1.96.0 rust-src
```

构建入口会设置 `RUSTC_BOOTSTRAP=1`，通过 `-Z build-std=core` 构建 core，首次运行 Cargo 可能需要联网下载依赖。不需要另装 nightly。技术依据见 [目标运行库](tooling-internals.md#target-runtime)。

注意：

- LoongArch64：Ubuntu 24.04 的 apt 源没有 `gcc-loongarch64-linux-gnu`，需要单独安装 Loongson 官方交叉工具链。
- LoongArch32（LA32R）和 PPC64LE 已从当前构建环境清单退出；历史资料不代表当前支持，如需复现，使用 `v0.18` 的干净 checkout。
- 交叉 gcc 只把 `libc6-dev-*-cross` 列为 **Recommends**：用`--no-install-recommends` 装出来的是一个头文件不全的编译器。内核里有几处会包含 libc 头（例如 `kernel/ipc/keyring.c` 的 `<limits.h>`），所以上面把对应的 `libc6-dev-*-cross` 显式写进来；它们同时带来`linux-libc-dev-*-cross`（UAPI 头）。
- arm32：`libc6-dev-armhf-cross` 提供内核与用户态需要的 libc/UAPI 头。若只把交叉工具链解包到临时目录，可把 `FF_LINUX_UAPI_ROOT` 指向包含`<triple>/include` 的根目录，例如`make FF_LINUX_UAPI_ROOT=/tmp/arm-root/usr check-build-matrix`；这个变量只影响**用户态**的 UAPI 头搜索（`user/Makefile`），不解决内核侧的 libc 头。 ARMv7-M 另走 STM32 目标，工具链不同。
- ARM32 定时器回归夹具的维护约束见 [时间 ABI](tooling-internals.md#arm32-time-abi)。
- Python：Makefile 默认调用 `python3`（Python 3.11 或更新版本，仅需标准库）。不需要 Conda，也不会因机器安装了 Conda 而自动切换解释器。可通过 `make PYTHON=/path/to/python3 ...` 显式指定其他解释器。
- `grub-pc-bin` / `grub-efi-amd64-bin` / `xorriso`：只有 x86_64 瘦客户机部署用得到。`tools/a20 package x86_64-pc` 生成 GRUB rescue ISO 时，`grub-mkrescue` 会调用 `xorriso`；缺了它会在打包这一步报 `grub-mkrescue: 未找到 xorriso`。riscv64/aarch64 板和 QEMU 虚拟机都不需要这三个包。

## 最常用的构建与运行命令

| 命令 | 作用 | 使用场景 |
|------|------|----------|
| `make ARCH=riscv64 BOARD=qemu-virt-riscv64 run` | 编译内核、用户态和文件系统镜像，并在 QEMU 中启动 | 默认开发流程 |
| `make ARCH=riscv64 run` | 同上，`BOARD` 默认等于 `qemu-virt-riscv64` | 快速启动 |
| `make run-riscv64` | 等价于 `make ARCH=riscv64 run` | 记住目标名即可 |
| `make run-loongarch64` / `make run-arm64` / `make run-x86_64` | 在对应架构的 QEMU 中启动 | 跨架构测试 |
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
| `tools/a20 test qemu-riscv32-mcu` | 构建并在 QEMU 中运行 RV32 M 态内核 + CMSIS 实验 4.1–4.5 | 需要 `qemu-system-riscv` 和 RV32IMC 工具链 |
| `tools/a20 flash hi3861` | 生成并写入 Hi3861 kernel A；`prepare` 取得固定版本 loaderboot | 需要 RV32IMC 工具链、CH340 串口和板卡复位操作 |
| `make -C user ARCH=riscv64` | 单独编译 RISC-V 用户态 | 只改用户程序 |
| `make check-user-build` | 编译主机默认集合的 hosted 用户态（Linux 为六架构，macOS 为 RISC-V64） | 常规提交前检查 |
| `make check-build-matrix-all` | 显式编译六个 hosted 架构的内核 bring-up 和用户态 | 完整活跃架构构建检查 |

STM32 固件、QEMU 和烧录目标使用同一套 `BUILD_DIR` 命名；目前仍受上述 Rust target 阻塞影响。 QEMU 运行和实板烧录还分别依赖 `qemu-system-arm`、OpenOCD 与实际调试硬件。准确产物路径见 [STM32F103 移植说明](../platforms/stm32f103-port.md)。

## 声明式构建配置（instances/ 与 components/trim.toml）

Makefile 之外的构建配置有两个声明式入口，日常应优先使用它们而不是直接拼 make 变量：

- **实例清单** `instances/*.toml`：一个文件描述一个完整可构建/可运行/可测试的系统实例，`tools/a20 build|run|flash|... <实例>` 负责校验并推导 make 变量。字段参考见 [instances.md](instances.md)。
- **剪裁注册表** `components/trim.toml`：内核剪裁的**策略**数据——构建 profile 策展的源码清单（如 `[profile.mcu]`）、各可选能力的架构矩阵（nommu/swap/xlator/ramfs-user/SMP 已验证平台/PCIe MMIO）、每架构固定特性宏。Makefile 经生成的 `components/trim.mk` 读它（`NOMMU_SUPPORTED_ARCHES`、`SWAP_SUPPORTED_ARCHES`、MCU 源码列表都出自这里），`tools/a20` 校验实例时读同一份 TOML。改完 TOML 执行 `make regen-trim-fragment` 重新生成；`make check-trim-registry` 会拒绝过期的 fragment、缺失的条目和没有消费者的死配置。

想知道某个实例实际按什么剪裁构建，用 `tools/a20 trim <实例>` 查看完整计划（profile、各能力开关及理由、内存预算）。

## 发布与调试模式

- 默认 `OPT=-O3`，对应发布构建。
- 调试构建使用 `make debug-<arch>`，它会强制 `OPT="-O0 -g -DDEBUG"` 并启动 QEMU 的 GDB 服务器。

## 常用变量

- `ARCH`: 当前目标架构，如 `riscv64`、`aarch64`、`x86_64`、`loongarch64`、`arm32`、`riscv32`、`armv7m`。LoongArch32/PPC64LE 是历史目标，Makefile 会拒绝活跃构建；历史复现需切到 `v0.18`。
- `BOARD`: 默认 `qemu-virt-<ARCH>`；STM32 时为 `stm32f103`。
- `BRINGUP`: `1` 只编译内核，`0` 编译完整用户态。
- `ABI`: `linux` / `native` / `both`，默认 `both`。
- `NR_CPUS`: 默认 `1`；只有 `riscv64`、`aarch64`、`loongarch64`、`x86_64` 的同名 QEMU virt 板列入已验证 SMP 白名单（出处：`components/trim.toml` 的 `smp-verified-qemu` 矩阵）。
- `CONFIG_KERNEL_PREEMPT`: 默认 `1`（hosted 架构内核态抢占，机制见 `docs/kernel/process-scheduler.md` §4.1）；MCU profile（armv7m）固定关闭。设为 `0` 回到协作式内核。该开关参与 `BUILD_VARIANT`（构建目录带 `-preempt` 后缀），因此翻转它不会复用旧构建目录里的陈旧目标文件。
- `NOMMU`: `1` 开启 NOMMU 模式；构建支持 `riscv64`、`riscv32`、`aarch64`、`arm32`、`armv7m`（出处：`components/trim.toml` 的 `nommu` 矩阵）。hosted MMU/NOMMU runtime matrix 只包含前四项，ARMv7-M 走独立 MCU 入口。
- `DRIVER_DEPLOYMENT`: hosted 开发构建通常默认 `generic`，将可发现设备驱动打包为 `.a20drv`；`embedded` 静态链接完整驱动集。ARMv7-M 和发布构建使用 embedded。
- `QEMU_GUI_AUDIO_DRIVER`: RISC-V/x86_64/LoongArch64 图形 QEMU 的宿主音频 backend；Linux 默认 `pa`，macOS 默认 `coreaudio`，也可设置为 `pipewire`、`alsa`、`sdl` 或 `none`。
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
- 共享用户态输出目录的构建会串行执行。不同优化选项或 profile 可同时发起构建，等待中的构建将在目录可用后继续；维护接线时见 [并发构建契约](tooling-internals.md#concurrent-builds)。
- `make clean` 会清除当前仓库配置的 `.kernel-build` 内容。若该路径是指向专用外部构建缓存的符号链接，命令会保留链接并清空其目标中的架构构建目录。首次使用外部缓存时，先确认目标目录专用于 A20OS，再创建标记文件：`printf 'a20-build-root-v1\n' > /path/to/cache/.a20-build-root`。清理会验证该标记、拒绝指向仓库本身/父目录的链接，并拒绝不符合架构构建目录命名约定的内容，以免误删无关文件。
- 默认 `NR_CPUS=1`。RISC-V 64、AArch64、LoongArch64 和 x86_64 的 QEMU virt 平台已验证 SMP，可直接设置 `NR_CPUS>1`。其他架构或板卡仍会被构建系统拒绝，除非显式设置 `ALLOW_UNVERIFIED_SMP=1`。
- 发布构建 `make all` 构建 RISC-V64 与 LoongArch64，使用 `PROFILE=benchmark NR_CPUS=8 DRIVER_DEPLOYMENT=embedded EXTERNAL_ROOT=1`，输出根目录的 `kernel-rv`、`kernel-la`、`disk.img`、`disk-la.img`。默认优化仍来自 `OPT=-O3`；若需要改变优化必须使用 `OPT`。
- `ARCH=armv7m` 需要 `arm-none-eabi-gcc` 或 `clang` + `llvm-objcopy`。
- 图形 QEMU 目标依赖宿主机显示能力；无图形环境请使用普通 `run-*` 目标。
- 不要直接仿照 Makefile 外的 QEMU 参数手写启动命令，容易遗漏 `-bios default` 等关键选项。
