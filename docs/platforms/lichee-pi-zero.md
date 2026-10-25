# Lichee Pi Zero（Allwinner V3s）

A20OS 的 `lichee-pi-zero` 实例面向搭载 Allwinner V3s、64 MiB DDR2 的原版 Lichee Pi Zero，使用 ARMv7-A MMU 和单个 Cortex-A7。它是串口内核 bring-up 平台；固件、内核、uImage 和完整 SD 镜像可在主机交叉构建，物理硬件启动尚未验证。

## 系统支持

| 项目 | A20OS 实现与边界 |
| --- | --- |
| CPU / MMU | 单核 ARMv7-A，短描述符页表；映射真实 64 MiB RAM，并将 V3s 外设页映射为 Device 内存。入口接受 U-Boot 的非安全 SVC/HYP 状态，HYP 时先切换到 SVC。 |
| UART0 | DW APB UART，物理地址 `0x01c28000`、32 位访问、寄存器间隔 4 字节。保留 U-Boot 已配置的引脚、时钟与 115200 分频，支持收发及 RX IRQ。 |
| 中断 | GIC-400，Distributor `0x01c81000`、CPU interface `0x01c82000`；UART IRQ 32、非安全物理通用定时器 IRQ 30。 |
| 时间 | 24 MHz ARM 通用计数器；64 位 `CNTPCT` 读取与物理定时器。 |
| 启动内存 | 从匹配 DTB 读取 RAM、bootargs、`reserved-memory` 和 memreserve；保护 DTB 自身及固件保留区。分配器只使用第一个保留区之前的连续 RAM。 |
| 关机 / 重启 | 关机输出标记后停机，需要外部断电；重启通过 V3s watchdog 请求复位。两者均待实机验证。 |
| SD / 网络 / USB / 显示 | SD 读卡由 SPL/U-Boot 完成。A20OS 尚无 V3s MMC、EMAC、USB、显示及摄像头驱动，不提供 SD 根文件系统、网络登录或用户态 shell。 |

默认 `[kernel].bringup=true`、关闭 swap、不创建用户态。验收串口必须包含内存范围、`System ready (bringup, no userspace)`、`part ok` 和停机标记；这些字符串同时出现也只证明对应一次运行达到验收点。

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

Linux 主机需要 `make`、`arm-linux-gnueabihf-gcc`/binutils、交叉 libc 开发头、Python 3.11+、Rust `armv7a-none-eabihf` 目标、`bison`、`flex`、`swig`、OpenSSL 开发包、`mkfs.fat` 和 `mtools`。Debian/Ubuntu 的交叉工具链包通常为 `gcc-arm-linux-gnueabihf`、`binutils-arm-linux-gnueabihf`、`libc6-dev-armhf-cross`。Rust 目标安装：

```sh
rustup target add armv7a-none-eabihf
```

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

## 硬件与协议依据

- [Sipeed Lichee Pi Zero 板卡介绍](https://wiki.sipeed.com/soft/Lichee/zh/Zero-Doc/Start/board_intro.html) 与 [UART0 / 上电使用说明](https://wiki.sipeed.com/soft/Lichee/zh/Zero-Doc/Start/use_board.html)：板型、64 MiB 内存及 `U0T/R` 调试口。
- [U-Boot LicheePi_Zero_defconfig](https://github.com/u-boot/u-boot/blob/v2025.10/configs/LicheePi_Zero_defconfig) 和 [Allwinner 启动布局](https://docs.u-boot.org/en/v2025.10/board/allwinner/sunxi.html)：匹配 V3s 板型、集成 SPL 镜像及 SD 原始偏移。
- [上游 V3s SoC 设备树](https://github.com/u-boot/u-boot/blob/v2025.10/dts/upstream/src/arm/allwinner/sun8i-v3s.dtsi) 与 [Lichee Pi Zero DTS](https://github.com/u-boot/u-boot/blob/v2025.10/dts/upstream/src/arm/allwinner/sun8i-v3s-licheepi-zero.dts)：UART、GIC、定时器中断、时钟和板级兼容标识。
- [U-Boot bootm 协议](https://docs.u-boot.org/en/v2025.10/usage/cmd/bootm.html)：legacy uImage 和外部 DTB 交接方式。
