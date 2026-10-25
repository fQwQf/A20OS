# 硬件平台

各平台页面说明 A20OS 的系统配置、设备支持范围、启动依赖和部署方法。
“内核初始化配置”指完成早期内存、异常和定时器初始化后执行自检的配置，
不提供交互 shell 或持久 rootfs；“探针”不是完整操作系统。
固件的存储、网络能力与 A20OS 接管后的设备驱动能力分别列明。

| 平台 | 架构与内存 | 默认实例 | 系统配置与启动方式 |
| --- | --- | --- | --- |
| [PINE A64 512 MiB](pine-a64.md) | AArch64，512 MiB | `pine-a64` | 单核内核初始化；SPL/TF-A/U-Boot，SD 镜像 |
| [LicheeRV Nano](licheerv-nano.md) | RV64 / Sv39，256 MiB | `licheerv-nano-e` | 单核内核初始化；FSBL/OpenSBI/U-Boot，SD 镜像 |
| [Intel J1900 工控机](j1900.md) | x86-64，按主板配置 | `j1900-pc` | 单核内核初始化；GRUB BIOS/64 位 UEFI ISO |
| [龙芯 2K0300 久久派](2k0300.md) | LoongArch64，保守低端 RAM 窗口 | `ls2k0300` | 单核内核初始化；原厂 U-Boot 从 USB/SD/TFTP 装载 RAM ELF |
| [Sipeed M0Sense](m0sense-bl702.md) | RV32 M-mode，无 MMU | `m0sense-bl702-probe` | SRAM 探针；原厂 UF2 应用上传或 JTAG |
| [Lichee Pi Zero](lichee-pi-zero.md) | ARMv7-A / MMU，64 MiB | `lichee-pi-zero` | 单核内核初始化；SPL/U-Boot，ARM32 uImage SD 镜像 |
| [PINE64 Ox64](ox64.md) | BL808 RV64 主核及 RV32 MCU 核，64 MiB PSRAM | `ox64` | D0 RV64 MMU/NoMMU 内核初始化；保留 SPI 固件、SD 载荷；RV32 M0 未移植 |
| [Quartz64-Zero](quartz64-zero.md) | AArch64，RK3566T，1 GiB | `quartz64-zero` | 单核内核初始化、GICv3；独立板型固件与 SD 镜像 |

以上平台的实机运行状态均为未验证。支持表表示源码提供的配置和接口，
不构成串口、存储、网络或电源管理已经在相应设备运行的证明。
每个平台页面单独说明未实现的驱动和固件限制。

## 实例工具

```sh
tools/a20 boards
tools/a20 show pine-a64
tools/a20 check instances/pine-a64.toml
```

构建和部署参数见 [实例参考](../dev/instances.md)，工具链依赖见 [构建指南](../dev/build.md)。
平台页的命令从仓库根目录执行；串口和目的盘占位符必须替换为实际设备。
原始块设备写入会覆盖已有内容，应先核对设备身份并卸载其分区。
`run`/`debug` 是 QEMU 入口；这些 SBC 的实例不会以不匹配的 QEMU virt 代替实际板卡。
`--dry-run` 只显示计划，`--no-console` 将介质准备与串口启动检查分开。

## 其他平台与移植参考

- [VisionFive 2 与 LS2K1000](physical-boards.md)：板级实现与设备支持范围。
- [VisionFive 2 启动链](visionfive2-boot.md)、[LS2K1000](2k1000.md)：独立部署文档。
- [x86-64 PC](x86_64-pc.md)、[VirtualBox](virtualbox.md)、[STM32F103](stm32f103-port.md)。
- [架构和板级移植接口](porting-guide.md)。
