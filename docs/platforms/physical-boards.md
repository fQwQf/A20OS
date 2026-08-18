# 物理开发板移植：VisionFive 2 与 LS2K1000

本文记录两块物理开发板的板级事实、驱动 bring-up 细节和已知边界，作为 `kernel/platform/visionfive2/` 与 `kernel/platform/ls2k1000/` 的配套说明。 硬件细节参考了 [RocketOS (MIT)](../ACKNOWLEDGMENTS.md) 的 StarFive 与 loongson-2K bring-up 驱动；凡未在真机复现的结论都明确标注"未核实"。

与 QEMU virt 的区别：这两块板当前只有**源码级移植 + 构建验证**，没有任何 QEMU 目标可以回归（QEMU virt 是另一套地址/中断布局）。因此在硬件日志补齐 之前，所有板级结论都不得声称"运行通过"。

## 共同原则

- `kernel/arch/<arch>/` 不含板级地址。物理内存与 MMIO 布局由 board 通过 `board_config_t` 声明，架构层的内存发现（`riscv64_memory_init` / `loongarch64_memory_init`）读取 `board_config.ram_base/ram_end` 作为 可用物理窗口，再用固件 DTB 的 `memory` 节点在该窗口内收窄。
- 无 DTB 时回退到 board 窗口：riscv64 见 `kernel/arch/riscv64/platform/fdt.c`， loongarch64 见 `kernel/arch/loongarch64/platform/fdt.c`。
- 架构的软中断（IPI）TLB shootdown 处理器提供**弱默认实现**，只有需要 generation 确认的板才提供强符号：riscv64 的 `rv64_ipi_tlb_flush_handler`、 loongarch64 的 `loongarch64_ipi_tlb_flush_handler`。这让不启用 SMP 的板 （如 LS2K1000）不与任何板符号硬链接。
- 驱动不得读取 `CONFIG_BOARD_*`，不得硬编码板级地址/IRQ（见 [移植指南](porting-guide.md)）。SoC 时钟门控这类板级事实放 board， 设备协议驱动只做寄存器级操作。

## StarFive VisionFive 2（JH7110）

### 硬件要点

| 项 | 值 | 说明 |
|---|---|---|
| CPU | 4× SiFive U74（rv64imafdc） | S-mode，OpenSBI 之上 |
| 内存 | 0x40000000 起，2/4/8 GiB | board 窗口上限 0x240000000 |
| UART0 | 0x10000000 | 内核映射需加 `PAGE_OFFSET` |
| PLIC | 0x0C000000 | 与 QEMU virt 相同基址 |
| SDIO0 (dw-mci) | 0x16020000 | 轮询驱动 |
| GMAC1 (EQOS) | 0x16040000 | 时钟由 SYS_CRG 开启 |
| SYS_CRG | 0x13020000 | GMAC1 时钟门控 + 复位 |
| 定时器 | RISC-V `time` CSR | DTB `timebase-frequency`（JH7110 24 MHz，回退值同） |
| 固件 | U-Boot SPL + OpenSBI + U-Boot | 以 SBI HSM 启动 secondary |

### SYS_CRG GMAC1 时钟/复位

GMAC 上电时被时钟门控并处于复位态，必须先使能再访问寄存器。board 的 `vf2_early_init()` 调 `vf2_gmac_clock_init()`：

| 寄存器（SYS_CRG 0x13020000） | 偏移 | 操作 |
|---|---|---|
| GMAC1_CLK_AHB | 0x184 | 置 bit31 使能 |
| GMAC1_CLK_AXI | 0x188 | 置 bit31 使能 |
| GMAC1_CLK_PTP | 0x198 | 置 bit31 使能 |
| GMAC1_CLK_TX | 0x1A4 | 置 bit31 使能 |
| GMAC1_CLK_GTXC | 0x1AC | 置 bit31 使能 |
| SYS_CRG_RESET2 | 0x300 | 清 bit2(AXI)/bit3(AHB) 去复位 |

偏移取自 RocketOS `eth_dev.rs` 与 StarFive JH7110 文档，未在真机复现时按 上表核对。

### 中断

PLIC 与 QEMU virt 同布局，board 复用 `PLIC_SENABLE/SPRIORITY/SCLAIM` 宏按 当前 hart 编程；`ack/eoi` 是 no-op，claim/complete 由 `arch_handle_irq` 完成。GMAC1 的 PLIC 线号按 RocketOS 记录为 78（**未核实**；上游 `jh7110.dtsi` 中 GMAC0/GMAC1 为 78/79，0x16040000 属 GMAC1，真机 bring-up 时必须用 DTB/实测确认）。DW-SDIO 当前不提供 IRQ 资源，纯轮询。

### 驱动状态与边界

- `starfive_gmac.c`：EQOS ring descriptor，TX 长度写入 des2（含 IOC）， des3 = OWN|FD|LD|len；每个实例持私有 spinlock 串行化 send/recv/poll； buffer/descriptor 在所有权移交前后调 `dma_sync_for_device/cpu`。
- PHY：扫描 MDIO 0..31 定位（VF2 板载 Motorcomm YT8531），复位 + 自协商。
- DW-SDIO（`dw_sdio.c`）：`g_sdio` 单实例 + 私有锁，命令/数据路径同步轮询。
- 已知边界：数据面全部轮询，未接 IRQ；GMAC 无 generic `.a20drv` 包，只能 embedded 静态部署（见 `docs/drivers/meta/implementation-status.md`）。
- 架构级 `TICKS_PER_SEC` 已改为运行时值：riscv64 在首次使用时读取 DTB
  `timebase-frequency` 并缓存（QEMU virt 10 MHz、JH7110 24 MHz 均正确），
  `timer_set_interval` 与全部 tick↔时间换算随之按板校准。
- 内核加载/链接地址与启动页表 RAM 窗口已由链接脚本符号
  （`BOOT_MAP_PHYS`/`BOOT_MAP_MMIO_HI`）参数化，board 级
  `ldscript.ld` 把 VF2 内核定位在 PA 0x40200000；上板启动链与 Flash
  烧录流程见 [visionfive2-boot.md](visionfive2-boot.md)。

## Loongson LS2K1000（龙芯 2K1000）

### 硬件要点

| 项 | 值 | 说明 |
|---|---|---|
| CPU | 2× LA264（loongarch64） | 当前 BSP-only（`.smp = NULL`） |
| 内存 | 物理 0x0 起，通常 512 MiB..1 GiB | board 窗口 0x0..0x40000000 |
| UART | 0x1FE001E0 | 架构控制台同址 |
| GMAC0 | 0x40040000 | stmmac 类（"snps,dwmac-3.710"） |
| GMAC0 PCI 配置窗口 | 0xFE00001800 | 读 BAR0 得 0x40040000 |
| 内核链接 | 0x9000_0000_0000_0000 | 缓存窗口别名 = 物理 0x0 |
| 定时器 | `rdtime.d`，100 MHz | 与 QEMU virt 同频 |
| 固件 | PMON/UEFI | 一般无 FDT 传递 |

### 内存

LS2K1000 的 DDR 在物理 0x0，LoongArch 内核链接在缓存窗口 `0x9000_0000_0000_0000`（物理 0x0）。board 窗口 `ram_base=0x0, ram_end=0x40000000`；固件传递 FDT 时以其 `memory` 节点为准， 否则回退到该窗口。真机首次 bring-up 必须确认固件加载地址、PMON 传递的 内存范围与 `kernel/arch/loongarch64/include/platform.h` 的 `PHYS_MEMORY_BASE/PHYS_MEMORY_END/KERNEL_ENTRY` 一致（这些常量仍是 QEMU virt 取向，见 [移植指南](porting-guide.md) 的说明）。

### 中断控制器（未完成项）

板级 irqchip 目前只做 CPU 本地使能（CSR ECFG 的 HWI0），定时器/软件 IPI 可用；设备 IRQ 需要 LS2K1000 内部 PIC（LioIntc/PCH-PIC）路由。注意 `kernel/arch/loongarch64/trap/irqchip.c` 的 `trap_init` 无条件调用 `la64_eiointc_pic_init()`（QEMU virt 的 PCH-PIC/EIOINTC，地址 0x10000000）， 真机 bring-up 时必须替换为 2K1000 的中断控制器驱动，在此之前所有板级驱动 都只能轮询。

### PCI（未完成项）

真机没有 QEMU virt 的 ECAM（0x20000000）。2K1000 的 PCI 配置空间通过 Loongson 配置窗口访问（例如 GMAC0 在 0x80000000fe00001800）。在加入一个 Loongson 配置访问 shim 之前，board 不调用 `pci_enumerate()`；也不要恢复 QEMU ECAM 调用。

### 驱动状态与边界

- `ls2k_gmac.c`：stmmac 类 descriptor（status/length/buffer1/buffer2）， 每个实例私有 spinlock 串行化 send/recv/poll；ownership 位更新前后调 `dma_sync_for_device/cpu`（LoongArch 上 `cacop` 缓存维护）。
- MMIO 基址由 board 以恒等映射（`PAGE_OFFSET=0`）提供；若改用 uncached 窗口 0x8000000000000000 访问寄存器，必须先在 `entry.S` 里配置对应 DMW。
- descriptor 位布局沿用既有 A20OS 移植（`DESC_FD/LD` 29/28、`FS/LS` 9/8、 帧长 `[29:16]`），与 RocketOS la2000 的位定义存在出入，真机必须核对。
- GMAC 无 generic `.a20drv` 包，只能 embedded 静态部署。

## 构建与验证

```sh
make ARCH=riscv64 BOARD=visionfive2 ABI=linux BRINGUP=1 kernel-only
make ARCH=loongarch64 BOARD=ls2k1000 ABI=linux BRINGUP=1 kernel-only
# 驱动在 embedded 账本中，需加 DRIVER_DEPLOYMENT=embedded 验证 GMAC/SDIO
make ARCH=riscv64 BOARD=visionfive2 ABI=linux BRINGUP=1 DRIVER_DEPLOYMENT=embedded kernel-only
make ARCH=loongarch64 BOARD=ls2k1000 ABI=linux BRINGUP=1 DRIVER_DEPLOYMENT=embedded kernel-only
# SMP 链接验证（真机多核验收前需显式 ALLOW_UNVERIFIED_SMP）
make ARCH=riscv64 BOARD=visionfive2 ABI=linux BRINGUP=1 NR_CPUS=2 ALLOW_UNVERIFIED_SMP=1 kernel-only
# VF2 上板启动链：从源码构建 OpenSBI+U-Boot SPL，打包直接引导 FIT 与 SD 镜像
make vf2-firmware
make vf2-image
```

CI 目标：`check-visionfive2-build`、`check-ls2k1000-build` （见 `tools/targets-build.mk`），保证两块板随仓库始终可构建。

## 真机验收清单

1. 串口控制台（arch `UART0_BASE`）与异常/timer 正常。
2. 打印 `[FDT] RAM range ...` 或 board 窗口回退，核对实际内存。
3. `[StarFive-GMAC]/[LS2K-GMAC] PHY link up`；`ping`/`sockets` 数据面。
4. VF2：`NR_CPUS=2` secondary online、reschedule/TLB IPI。
5. 记录固件版本、启动日志、压力测试结果并回填本文与 `docs/drivers/meta/implementation-status.md`。
