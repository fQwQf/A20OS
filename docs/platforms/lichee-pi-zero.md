# Lichee Pi Zero（Allwinner V3s）

A20OS 的 `lichee-pi-zero` 实例面向搭载 Allwinner V3s、64 MiB DDR2 的原版 Lichee Pi Zero，使用 ARMv7-A MMU 和单个 Cortex-A7。它提供独立的内核 bring-up 和 RAMFS 串口 shell 实例；固件、内核、uImage 和完整 SD 镜像可在主机交叉构建，物理硬件启动尚未验证。

## 系统支持

| 项目 | A20OS 实现与边界 |
| --- | --- |
| CPU / MMU | 单核 ARMv7-A，短描述符页表；映射真实 64 MiB RAM，并将 V3s 外设页映射为 Device 内存。入口接受 U-Boot 的非安全 SVC/HYP 状态，HYP 时先切换到 SVC。 |
| UART0 | DW APB UART，物理地址 `0x01c28000`、32 位访问、寄存器间隔 4 字节。保留 U-Boot 已配置的引脚、时钟与 115200 分频，支持收发及 RX IRQ。 |
| 中断 | GIC-400，Distributor `0x01c81000`、CPU interface `0x01c82000`；UART IRQ 32、非安全物理通用定时器 IRQ 30。 |
| 时间 | 24 MHz ARM 通用计数器；64 位 `CNTPCT` 读取与物理定时器。 |
| 启动内存 | 从匹配 DTB 读取 RAM、bootargs、`reserved-memory` 和 memreserve；保护 DTB 自身及固件保留区，分配器使用扣除保留区后的多个 RAM 区间。 |
| 关机 / 重启 | 关机输出标记后停机，需要外部断电；重启通过 V3s watchdog 请求复位。两者均待实机验证。 |
| SD / MMC0 | DT 的 `allwinner,sun7i-a20-mmc` 节点接入通用 `sunxi-mmc` PIO 块驱动；SDSC/SDHC/SDXC、512 字节扇区、单块读写和 flush；1-bit / 400 kHz。实机未验证；SD 不作为默认根文件系统，串口 shell 使用独立 RAMFS 实例。 |
| 网络 / USB / 显示 | 尚无 EMAC、USB、显示及摄像头驱动；不提供网络登录。 |
| 模拟音频 | 内部 DAC/FIFO/模拟 codec 的 `sunxi-codec` DMA 播放实现，48 kHz、双声道 S16_LE；裸板默认关闭，仅明确连接的 Dock/外接耳机拓扑可启用；未实机验证。 |

默认 `[kernel].bringup=true`、关闭 swap、不创建用户态。验收串口必须包含内存范围、`System ready (bringup, no userspace)`、`part ok` 和停机标记；这些字符串同时出现也只证明对应一次运行达到验收点。

## RAMFS 串口 shell

`lichee-pi-zero-shell` 复用同一板型固件和 SD 布局，将 init、mksh 及基本命令静态
ELF 嵌入内核，关闭 bring-up 自动停机和 swap。启动后直接通过 UART0 进入 shell，
无需内核 SD 驱动挂载根分区。先准备板级固件，再打包 shell 实例：

```sh
tools/a20 prepare lichee-pi-zero
tools/a20 package lichee-pi-zero-shell
sudo tools/a20 deploy lichee-pi-zero-shell --no-build --no-console \
  --media-device /dev/disk/by-id/替换为实际SD整卡设备
tools/a20 console lichee-pi-zero-shell --serial /dev/ttyUSB0 --interactive
```

该实例生成 `build/lichee-pi-zero/a20os-lichee-pi-zero-shell-sdcard.img`；每次 package
都会按所选实例刷新共享 handoff 目录，不可复用另一个实例的旧 bundle。启动验收先等待
`[init] fork=0, calling execve mksh`，再实际输入以下命令确认用户程序工作：

```sh
/bin/echo FIRST
/bin/echo SECOND
/bin/sleep 1; echo TIMER_OK
echo hello > /tmp/hello
/bin/cat /tmp/hello
/bin/ps
/bin/poweroff
```

写入 `/tmp` 和根目录的文件只保存在 RAM，复位即消失。`poweroff` 最后输出 V3s
停机标记，需要移除外部电源。Ethernet/SSH、显示、音频均未因本 shell 实例启用；
ARM32 已保存 trap、调度切换和信号帧的 VFP 上下文，父子进程寄存器隔离探针已通过。
64 MiB QEMU virt 已连续 10 轮通过每轮 40 次顺序 fork/exec、浮点隔离、抢占、空闲唤醒、
文件操作和正常关机。该配置提供 RAM 串口 shell；V3s 外设仍待实机验收，原生显示控制器驱动尚未实现。

主机回归使用 **QEMU virt，而非模拟 V3s 外设**，检查 ARM32 Linux ABI 的连续 exec/
wait、休眠唤醒、subshell、文件重定向与正常关机：

```sh
make ARCH=arm32 BOARD=qemu-virt-arm32 ABI=both BRINGUP=0 RAMFS_USER=1 RAMFS_FPU_TESTS=1 \
  DRIVER_DEPLOYMENT=embedded kernel-only -j2
python3 tools/check_serial_shell.py --arch arm32 --kernel 构建输出目录/kernel.elf \
  --memory-mb 64 --fpu --timers --stress-forks 40
```

回归以 QEMU 生成的 DTB 经 ARM Linux `r2` 协议交接，分别在 64 MiB 与 1 GiB 完成上述命令；
64 MiB 时内核初始化后报告约 41 MiB 可用内存。它验证小内存预算，不模拟 V3s 的管脚与外设。
V3s shell 已交叉构建；串口接线、DRAM/SD 时序与真实硬件运行仍须逐板验收。

## 启动协议与地址

BootROM 从 SD 卡 8 KiB 位置读取 SPL；SPL 初始化 DDR，启动主线 U-Boot。FAT32 第一分区从 8 MiB 开始，根目录包含 `kernel.uimg`、匹配 DTB 和自动执行的 `boot.scr`。U-Boot 验证文件大小和 CRC，再执行 `bootm`。ARM32 内核不是 ARM64 Linux Image，也不是压缩 zImage。

| 对象 | 物理地址 / 范围 |
| --- | --- |
| RAM | `0x40000000..0x44000000` |
| 内核 load / entry | `0x40080000`；实际完整范围由 bundle 的 `image_size` 记录，包含 BSS |
| 外部 DTB | `0x41800000` |
| U-Boot 默认 scriptaddr | `0x41900000` |
| legacy ARM uImage 暂存 | `0x42000000`，由 `bootm` 解包到内核 load 地址 |

打包器拒绝内核完整范围到达 DTB、DTB 到达脚本缓冲区或 uImage 到达上方保守固件保留区。它验证 ELF32 机器类型、物理入口、load segment 与 `kernel.bin` 的一致性，以及 uImage header/data CRC。`bootm` 传递设备树地址至 ARM32 的 `r2`。

## 主机依赖与固件

Linux 主机需要 ARM32 GNU 硬浮点交叉工具链、固件构建工具与 FAT 组装工具。Debian/Ubuntu 安装：

```sh
sudo apt-get install build-essential git curl ca-certificates python3 \
  gcc-arm-linux-gnueabihf binutils-arm-linux-gnueabihf libc6-dev-armhf-cross \
  bison flex swig libssl-dev libgnutls28-dev python3-dev python3-pip \
  python3-setuptools python3-yaml dosfstools mtools
```

安装 rustup 后配置仓库指定的 Rust 1.96.0；若尚无 rustup，可先执行下面前两条命令。已有 rustup 时可跳过安装器：

```sh
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o /tmp/a20-rustup-init.sh
sh /tmp/a20-rustup-init.sh -y --profile minimal --default-toolchain 1.96.0
. "$HOME/.cargo/env"
rustup toolchain install 1.96.0 --profile minimal
rustup target add --toolchain 1.96.0 armv7a-none-eabihf
```

下列仓库命令均在 A20OS 源码根目录执行；首次构建先运行 `git submodule update --init --recursive`。主机 Python 必须为 3.11 或更新版本，可用 `python3 --version` 确认。首次下载源码、Rust 组件和 Cargo 依赖需要联网。

在仓库根目录执行：

```sh
tools/a20 prepare lichee-pi-zero
tools/a20 package lichee-pi-zero
```

`prepare` 使用固定提交的 U-Boot v2025.10（`e50b1e8715011def8aff1588081a2649a2c6cd47`）、校验源码归档 SHA-256，选择大小写敏感的 `LicheePi_Zero_defconfig`。固件无需 TF-A/OpenSBI。配置启用 legacy uImage、脚本启动和 CRC 校验，并验证 64 MiB DRAM、非安全 ARMv7 与匹配 DTB。首次运行需要联网获取源码和固定版本 `pyelftools`；缓存放在 `build/lichee-pi-zero/`。

关键产物：

- `build/lichee-pi-zero/firmware/`：SPL/U-Boot、`sun8i-v3s-licheepi-zero.dtb`、`mkimage`、配置和固件来源清单。
- `build/lichee-pi-zero/handoff/`：ELF、原始内核、`kernel.uimg`、匹配 DTB、手工启动命令及覆盖这些文件和实例的校验清单。
- `build/lichee-pi-zero/a20os-lichee-pi-zero-sdcard.img`：128 MiB MBR 整卡镜像；相邻 `.img.json` 记录布局及固件、内核和 FAT 文件摘要。

`package` 不访问物理块设备。它在普通文件中创建 FAT 分区，逐个读回核对内容，再组装 MBR、8 KiB SPL/U-Boot 和 FAT。无需 loop mount。重复 prepare/package 是更新固件和内核的标准路径。

## 写卡与串口验收

准备可覆盖的 microSD 卡，将整卡设备路径核对为 `/dev/disk/by-id/...`。写入操作覆盖卡上的分区及文件；不能传某个分区如 `/dev/sdb1`。

```sh
sudo tools/a20 deploy lichee-pi-zero --no-build --no-console \
  --media-device /dev/disk/by-id/替换为实际SD整卡设备
```

板卡断电时插入已写好的卡。USB-UART 使用 3.3 V TTL；适配器 RX 接板上 `U0T`、TX 接 `U0R`，共地，供电使用板卡规定的独立电源路径。串口参数为 115200、8N1、无流控；官方板卡文档标明调试端口为 UART0。

先打开同一串口接收窗口，再给板卡上电：

```sh
tools/a20 console lichee-pi-zero --serial /dev/ttyUSB0 --interactive
```

SD 中的 `boot.scr` 自动加载并启动内核。工具保存完整串口日志，并按实例检查验收标记。默认等待 120 秒；需要更久时复制实例文件并修改 `target.boot_timeout`，重新 package 对应副本。`Ctrl-]` 可退出交互会话。内核停机后移除外部电源。

若自动脚本失败，按键停在 U-Boot 提示符，检查 `mmc list`、`mmc dev 0`、`mmc rescan`、`fatls mmc 0:1`。可在上述交互会话逐行输入 `build/lichee-pi-zero/handoff/uboot.cmd`，每次加载失败都应停止：

```text
setenv loadaddr 0x42000000
setenv fdt_addr_r 0x41800000
fatload mmc 0:1 ${loadaddr} kernel.uimg
fatload mmc 0:1 ${fdt_addr_r} sun8i-v3s-licheepi-zero.dtb
bootm ${loadaddr} - ${fdt_addr_r}
```

需要自动截停 U-Boot、核对加载文件大小/CRC/地址并重放同一实例命令时，在上电前运行：

```sh
tools/a20 console lichee-pi-zero --serial /dev/ttyUSB0 --auto-handoff
```

自动 handoff 要求本地 bundle 与当前实例和 SD 文件一致，且在串口开始等待之后重新上电。已经完成的历史启动不会重新出现在串口中。V3s 默认固件无网络驱动，实例只使用 SD/FAT，不能套用 Nano 的 TFTP 方案。

## 故障定位

没有任何 SPL 输出时先检查供电、UART 接线、115200 参数及写入的是整卡。只到 SPL 时重点检查 DDR 初始化、卡兼容性和是否使用 `LicheePi_Zero_defconfig`。停在 U-Boot 时核对 FAT 根目录和脚本加载结果；`Bad Magic Number`、CRC 或地址错误应重新打包并写卡。进入内核后的异常需要保留从上电开始的完整日志，特别是 DTB、RAM、IRQ 和最后一条标记。

64 MiB 不能按 QEMU virt 的 1 GiB 参数启动。不得使用其他 V3s 板、Lichee Pi Nano F1C100s 或 RISC-V LicheeRV Nano 的 DTB/固件。未知设备树、内核与固件保留区重叠时，内核会拒绝继续分配内存。

## 内核 SD 驱动与边界

`kernel/drivers/block/sunxi_mmc.c` 与 `sunxi_mmc_core.c` 是同一套架构无关控制器/
SD 协议实现。V3s 板级 `configure_devices` 为 DT 中已经启用、位于 MMC0 的节点
配置 PF0..PF5 的 function 2、30 mA 和上拉，打开 CCU AHB gate/reset，采用
OSC24M / 4 / 15 = 400 kHz。固件设备图保留节点身份；不会通过复制静态节点绕过 DT。
卡槽供电沿用 SPL/U-Boot；默认打包固件确实从该 MMC0 卡加载内核。

驱动以 1-bit 低速逐扇区 PIO 避免依赖 DMA、缓存一致性和高速调谐：
支持 SD 初始化、RCA、CSD、SDSC 字节寻址和 SDHC/SDXC 扇区寻址、CMD17/CMD24
读写及 CMD13 flush/就绪检查。CSD 永久/临时写保护及固件 `read-only` 属性均会拒绝写入。
所有硬件等待有上限；传输错误使当前绑定离线，后续 I/O 返回错误，需重新绑定初始化。
探测失败和移除接入公共 devres 与 class 生命周期。

尚不支持 4-bit/高速、DMA、MMC/eMMC、SDIO、GPIO 卡检测/机械写保护或热插拔重枚举。
不能热拔正在使用的卡。内核正常启动不等于卡片读写通过：实机应先核对
`[SUNXI-MMC]` 容量日志，再完成扇区内容与主机对比、受控写入和断电后读回。
固件仍负责 SPL/DDR 和加载内核，SD 驱动不替代启动固件。

生产代码宿主测试入口为：

```sh
python3 -m unittest tools.tests.test_sunxi_mmc
```

ASan/UBSan fixtures 检查 SDHC/SDSC 读写、容量/地址边界、写保护、无卡、CRC/FIFO/
clock/power/状态超时、分配失败及 probe/remove 清理。实际管脚时序、卡片兼容性、
供电和持久性仍待真机验证。

## 可选音频与显示交接

裸 Lichee Pi Zero 没有板载耳机插座；V3s 内部 codec 的存在不等于已连接输出。
板级代码只在固件根节点包含 `licheepi,licheepi-zero-dock` compatible，或显式
布尔属性 `a20,headphone-connected` 时准备音频；后者是 A20 板级接线声明，
只应在已确认 DAC/耳机线路及供电时添加。两种情况还必须提供 enabled 的
`allwinner,sun8i-v3s-codec`、模拟 codec、CCU 和 DMA 节点及真实 IRQ。
默认固件不增加该声明，也不启用虚假的耳机设备。

驱动由内部 DAC FIFO 和 sun6i 风格 DMA 组成，固定 48 kHz stereo S16_LE，
DMA channel 0 独占，使用 DRQ15、32 KiB 环形缓冲。写入有背压，支持尾部排空，
欠载或无法可靠判断已消费的周期返回错误；停止超时时保留仍可能被 DMA 使用的内存。
不支持录音、混音、HDMI 或自动切换采样率。板级时钟配置仅修改音频和 DMA 相关位，
拒绝接管不匹配的活跃音频 PLL。明确连接的设备可通过现有 `/dev/audioN` 接口播放。

默认 shell 镜像不含播放器；验证外接音频时，可在现有 shell 构建/打包命令末尾添加
`-- RAMFS_MEDIA=1`，将已有 `audioplay` 和 `videoplay` 一并嵌入。
这些命令仍须使用与实际 Dock/接线匹配的 DTB；不能只改布尔属性就代替供电、引脚和线路。
进入 shell 后可运行 `audioplay --tone 440 --duration 1000`。
`python3 -m unittest tools.tests.test_sunxi_audio` 检查生产驱动的 codec、DMA、背压、
欠载和错误清理；测试不代表 Dock 电气输出已经验收。

显示侧可消费固件提供的标准 DT simple-framebuffer，接入已有 GPU/fbdev；
其宽高、stride、格式、显存预留均来自真实交接。ARM32 对 RAM 帧缓冲还保留其
覆盖的完整 1 MiB section，并统一内核别名与用户映射属性；无法安全处理的映射拒绝接管。
目前本板固件配置没有初始化具体 LCD，A20 也没有 V3s TCON/DE2 原生驱动。
LCD 面板型号、RGB 时序、背光和复位接线确定前，默认配置仍为串口输出。

实现依据为 [Linux v6.17 V3s codec](https://github.com/torvalds/linux/blob/v6.17/sound/soc/sunxi/sun4i-codec.c)、
[模拟 codec](https://github.com/torvalds/linux/blob/v6.17/sound/soc/sunxi/sun8i-codec-analog.c)、
[V3s CCU](https://github.com/torvalds/linux/blob/v6.17/drivers/clk/sunxi-ng/ccu-sun8i-v3s.c)
和 [DMA](https://github.com/torvalds/linux/blob/v6.17/drivers/dma/sun6i-dma.c)。
公开实现用于核对寄存器和协议；A20 内核驱动为独立实现。

## 硬件与协议依据

- [Sipeed Lichee Pi Zero 板卡介绍](https://wiki.sipeed.com/soft/Lichee/zh/Zero-Doc/Start/board_intro.html) 与 [UART0 / 上电使用说明](https://wiki.sipeed.com/soft/Lichee/zh/Zero-Doc/Start/use_board.html)：板型、64 MiB 内存及 `U0T/R` 调试口。
- [U-Boot LicheePi_Zero_defconfig](https://github.com/u-boot/u-boot/blob/v2025.10/configs/LicheePi_Zero_defconfig) 和 [Allwinner 启动布局](https://docs.u-boot.org/en/v2025.10/board/allwinner/sunxi.html)：匹配 V3s 板型、集成 SPL 镜像及 SD 原始偏移。
- [上游 V3s SoC 设备树](https://github.com/u-boot/u-boot/blob/v2025.10/dts/upstream/src/arm/allwinner/sun8i-v3s.dtsi) 与 [Lichee Pi Zero DTS](https://github.com/u-boot/u-boot/blob/v2025.10/dts/upstream/src/arm/allwinner/sun8i-v3s-licheepi-zero.dts)：UART、GIC、定时器中断、时钟和板级兼容标识。
- [U-Boot bootm 协议](https://docs.u-boot.org/en/v2025.10/usage/cmd/bootm.html)：legacy uImage 和外部 DTB 交接方式。

- [Linux v6.17 V3s CCU](https://github.com/torvalds/linux/blob/v6.17/drivers/clk/sunxi-ng/ccu-sun8i-v3s.c)、[V3s pinctrl](https://github.com/torvalds/linux/blob/v6.17/drivers/pinctrl/sunxi/pinctrl-sun8i-v3s.c)：MMC0 gate/reset、分频和 PF0..PF5 功能。
- [U-Boot v2025.10 sunxi MMC](https://github.com/u-boot/u-boot/blob/v2025.10/drivers/mmc/sunxi_mmc.c) 与 [寄存器定义](https://github.com/u-boot/u-boot/blob/v2025.10/drivers/mmc/sunxi_mmc.h)：V3s PIO 控制器寄存器事实。
- [SD Association 简化规范](https://www.sdcard.org/downloads/pls/)：SD 初始化、CSD 与块传输协议。
