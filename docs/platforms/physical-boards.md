# 物理开发板移植：VisionFive 2 与 LS2K1000

本文记录两块物理开发板的板级事实、驱动 bring-up 细节和已知边界，作为 `kernel/platform/visionfive2/` 与 `kernel/platform/ls2k1000/` 的配套说明。 硬件细节参考了 [RocketOS (MIT)](../ACKNOWLEDGMENTS.md) 的 StarFive 与 loongson-2K bring-up 驱动；凡未在真机复现的结论都明确标注"未核实"。

与 QEMU virt 的区别：物理板和 QEMU virt 使用不同的地址与中断布局。VisionFive 2 的启动链、存储挂载和 GMAC1 网络驱动已经完成源码级构建验证；在接入网线前不要把 PHY link-up、DMA 收发或公网访问写成真机验收结论。LS2K1000 的存储与网络数据面 仍按各自章节的边界执行。

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

PLIC 与 QEMU virt 同布局，board 复用 `PLIC_SENABLE/SPRIORITY/SCLAIM` 宏按当前 hart 编程；`0x16040000` GMAC1 的 `macirq` 为 PLIC 78（由随镜像构建的 VF2 DTB 确认），`ack/eoi` 由通用异常路径完成。GMAC 数据面当前使用轮询，故不会依赖该线。 DW-SDIO 当前不提供 IRQ 资源，纯轮询。

### 驱动状态与边界

- `starfive_gmac.c`：EQOS ring descriptor，TX 长度写入 des2，des3 = OWN|FD|LD|len； RX 使用 OWN|BUF1V 并在收包后重新推进 tail；每个实例持私有 spinlock 串行化 send/recv/poll；buffer/descriptor 在所有权移交前后调 `dma_sync_for_device/cpu`。
- PHY：扫描 MDIO 0..31 定位（VF2 板载 Motorcomm YT8531），复位 + 自协商。
- DW-SDIO（`dw_sdio.c`）：`g_sdio` 单实例 + 私有锁，命令/数据路径同步轮询。
- 已知边界：数据面全部轮询，未接 IRQ；GMAC 无 generic `.a20drv` 包，只能 embedded 静态部署（见 `docs/drivers/meta/implementation-status.md`）。
- 架构级 `TICKS_PER_SEC` 已改为运行时值：riscv64 在首次使用时读取 DTB `timebase-frequency` 并缓存（QEMU virt 10 MHz、JH7110 24 MHz 均正确）， `timer_set_interval` 与全部 tick↔时间换算随之按板校准。
- 内核加载/链接地址与启动页表 RAM 窗口已由链接脚本符号 （`BOOT_MAP_PHYS`/`BOOT_MAP_MMIO_HI`）参数化，board 级 `ldscript.ld` 把 VF2 内核定位在 PA 0x40200000；上板启动链与 Flash 烧录流程见 [visionfive2-boot.md](visionfive2-boot.md)。

## Loongson LS2K1000（龙芯 2K1000）

### 硬件要点

| 项 | 值 | 说明 |
|---|---|---|
| 开发板 | LS2K1000-DP-V10 | 2026-08-17 真机读取 |
| CPU | 2× Loongson-2K1001 / LA264，800 MHz | 当前 BSP-only（`.smp = NULL`，构建时 `NR_CPUS=1`） |
| 内存 | 1 GiB | U-Boot DMW bank：256 MiB @ `0x9000000000000000`，768 MiB @ `0x9000000090000000` |
| A20OS 可用内存 | 物理 `0x00200000..0x0b000000` | 首次 bring-up 的保守窗口，DTB 只能收窄，不能扩大 |
| UART | 物理 `0x1FE20000`，IRQ 16 | 内核通过 uncached DMW 地址 `0x800000001fe20000` 轮询，115200 8N1 |
| GMAC0 | 0x40040000 | DWMAC1000（Synopsys ID `0x37`），RGMII，PHY 0 |
| GMAC1 | 0x40050000 | DWMAC1000（Synopsys ID `0x37`），RGMII，PHY 0 |
| GMAC0 PCI 配置窗口 | 0xFE00001800 | 读 BAR0 得 0x40040000 |
| 内核加载/入口 | `0x9000000002000000` | cached DMW 地址，对应物理 `0x02000000` |
| 定时器 | `rdtime.d`，100 MHz | 与 QEMU virt 同频 |
| 固件 | U-Boot `2022.04-v2.1.0-00583-g2ed41674` | 小写 `c` 中断自动启动；默认从 `/dev/sda1` 的 `/boot/uImage` 启动 Linux |

### 内存

U-Boot 驻留区从物理 `0x0cbf4c30` 附近开始，显示缓冲等固件保留区也位于低端第一 bank 的高地址。A20OS 因此加载到物理 `0x02000000`，且分配器只接管 `0x00200000..0x0b000000`。板级链接脚本对 `_end <= 0x900000000b000000` 作硬性断言；固件 DTB 即使描述完整 1 GiB，也会与该窗口求交，不能把保留区重新交给分配器。

启动代码安装与 U-Boot 一致的 DMW：VSEG 8 为 uncached (`0x800000000000000f`)，VSEG 9 为 cached (`0x900000000000001f`)。PGDL/PGDH 写入页表的物理地址，RAM 指针则使用 VSEG 9 的 cached 别名。板载 DTB 从 SPI 的 `dtb` 分区临时读到 `0x900000000a000000`；实测 DTB 没有可用的 `memory` 节点时，内核回退到上述安全窗口。

### 中断控制器（未完成项）

设备 IRQ 使用 LS2K1000 内部 LIOINTC，与 QEMU 的 PCH-PIC/EIOINTC 路径完全分离。非 cooperative 真机镜像已实现并验证 UART0/source 0 到 CPU0 HWI1 的路由；初始化会清除 U-Boot 遗留的设备中断使能，只重新开放 source 0，其余设备源保持 masked。cooperative 恢复镜像仍使用 UART 轮询，GMAC 也仍为轮询/未完成状态；中断分派继续用 `ECFG.LIE` 过滤 `ESTAT.IS` 中 masked pending 位。

板载 vendor DTB 和匹配 Linux 驱动确认 LIOINTC 主寄存器位于物理 `0x1fe01400`，每核 pending 基址为 `0x1fe01040`（CPU1 加 `0x100`），级联到 LoongArch HWI1 / `ESTAT.IS[3]`。UART0 使用硬件源 0；运行中 Linux 的 route 字节为 `0x23`（HWI1、双核），A20OS 单核路由应为 `0x21`。手册中的 `0x1fe11400`/`0x1fe11040` 与本板实况冲突，不用于实现。寄存器布局、只读实测值和证据来源集中记录在 `docs/platforms/2k1000.md`。

### PCI（未完成项）

真机没有 QEMU virt 的 ECAM（0x20000000）。2K1000 的 PCI 配置空间通过 Loongson 配置窗口访问（例如 GMAC0 在 0x80000000fe00001800）。在加入一个 Loongson 配置访问 shim 之前，board 不调用 `pci_enumerate()`；也不要恢复 QEMU ECAM 调用。

### AHCI/SATA（只读路径已上板）

- vendor DTB/Linux 确认 AHCI 物理窗口为 `0x400e0000..0x400effff`，LIOINTC source 为 19。U-Boot 报告 AHCI 1.3、一个 SATA 端口和 `62,533,296` 个 512 字节扇区。
- A20OS 已增加独立 `STORAGE_READ_ONLY=1` 变体，通过 platform bus 直接提供 AHCI MMIO，不依赖尚未实现的 PCI host window。
- 当前路径只轮询，AHCI source 19 和控制器 IRQ 都不启用；AHCI 命令层、MBR 分区包装层和 VFS 只读挂载层均拒绝写入。
- LS2K1000 platform data 使用 `AHCI_PLATFORM_F_PRESERVE_FIRMWARE_LINK`：跳过通用 `GHC.HR` 和首次 COMRESET，保留 U-Boot `scsi reset` 建立的 SATA PHY 链路。QEMU/PCI AHCI 不启用该标志，仍执行标准 reset/COMRESET。
- DOS/MBR `0x55aa` 与 `0x83` 主分区解析已实现。干净 ext4 只读挂载到 `/extra`；`needs_recovery` 文件系统会被拒绝，不做日志回放，RAM shell 继续启动。
- QEMU `ich9-ahci` 已验证 IDENTIFY、MBR、ext4 只读挂载、`EROFS` 写栅栏、完整流式读取和 4 GiB 边界两侧读取。测试盘运行前后 SHA-256 一致。
- 候选镜像的字节数与 SHA-256 由 `tools/a20 ledger` 现算，不在此手抄。真机识别 `SSTS=0x123`、`TFD=0x50`、`62,533,296` sectors 和 MBR `0x83` 分区，并将 ext4 只读挂载到 `/extra`；该轮询读取路径已 physical-board-validated，写入仍明确不受支持。

### 驱动状态与边界

- `ls2k_gmac.c`：使用 DWMAC1000 basic 16-byte ring descriptor，每个实例私有 spinlock 串行化 send/recv/poll；读取 ownership 前及交还 ownership 前执行 `dma_sync_for_device/cpu`（LoongArch 上 `cacop` 缓存维护）。
- MMIO 基址由 board 通过 VSEG 8 uncached DMW 提供；RAM 使用 VSEG 9 cached DMW（`PAGE_OFFSET=0x9000000000000000`）。
- vendor Linux 确认硬件也支持 enhanced/alternate descriptor，但当前候选未开启该模式。TX 使用 des0 的 OWN/IC/LS/FS/TER，RX ring end 使用 des1 的 RER；该布局仍需有网线时用实际收发验证。
- 无网线时 vendor Linux 对 eth0/eth1 均报告 `NO-CARRIER`。当前候选已把 PHY 存在与 carrier 状态分开，无 carrier 仍注册设备并向 lwIP 报告 link down；DMA IRQ 和 GMAC LIOINTC 源保持关闭。该无网线探测路径已通过 RAM-only 真机验证，但 descriptor 数据收发仍未验证。
- GMAC 无 generic `.a20drv` 包，只能 embedded 静态部署。
- UART 仍为轮询模式。手册第 15 章确认它兼容 NS16550A，接收数据进入 FIFO，`LSR.OE` 表示未及时读取造成溢出。无设备 IRQ 时原 50 ms 轮询休眠会在 115200 波特率下丢失批量输入；当前候选通过 board 参数把唤醒周期缩短为 1 ms，QEMU 路径仍保留 50 ms 默认值。真机低速分段输入可用，但突发输入仍会丢字符。

### 恢复与 RAM-only 启动

真机恢复包位于板载 Linux 的 `/root/a20-recovery-<日期>/`，包含六个 MTD 分区、运行时 DTB、布局与系统信息及 `SHA256SUMS`。开始试启动前先在原 Linux 中执行 `sha256sum -c /root/a20-recovery-<日期>/SHA256SUMS`——校验文件随包分发，**不要**在这里记它的哈希值，记下来的数字一定会和下一次重新打包对不上。

恢复包必须另存一份到板子之外（另一块盘、另一台机器或版本控制附件）。只放在同一块系统盘上覆盖不了磁盘故障场景，而这个包存在的全部意义就是那种场景。

首次启动只使用 RAM，并以新文件名存放内核。禁止执行 `saveenv`、`sf write`、`sf erase`，禁止覆盖 `/boot/uImage`。在 U-Boot 中执行：

```text
sf probe
sf read ${fdt_addr} dtb
scsi reset
ext4load scsi 0:1 0x9000000002000000 /boot/a20os-ls2k1000-bringup.bin
go 0x9000000002000000
```

A20OS 挂起后用物理复位恢复，U-Boot 的默认 `bootcmd` 仍从 `/boot/uImage` 启动原 Linux。不要在 A20OS 仍运行时尝试跳回 U-Boot。

该固件的倒计时为零秒，人工在看到 `Autoboot` 后再输入已经太晚。`tools/ls2k1000-uboot-stop.runscript` 会在识别到 `Press c to enter u-boot console` 后覆盖 USB 扫描窗口发送固件菜单键；它只负责截停，不包含 Flash 写入或环境保存命令。
### 板级约束（真机踩出来的，不可由代码推断）

这几条是硬件/SoC 的事实，不是实现选择，改动相关代码时必须继续满足：

- **LA264 会对未对齐的宽访存触发异常。** 因此 LoongArch64 内核统一用
  `-mstrict-align`，且 PFA 的 `frame_meta_t` 数组元素显式保持 8 字节对齐。
  推论：vendor 分区里既有的用户程序**不能**当作 A20OS 兼容程序直接跑——
  从 `/extra/bin` 执行 vendor 的 `mkdir`、`chmod` 都会在 LA264 上触发
  `ALE code=9`；本轮验证过的外部程序（Vim、Git）都是用 `-mstrict-align`
  重新构建的。
- **中断开放顺序**：首次 timer IRQ 前保持 `CRMD.IE=0`，在 `proc_init()` 与
  `net_init()` 完成后重装 one-shot timer，再开放中断。公共异常入口使用
  非向量模式，并且只分派 `ECFG.LIE` 实际启用的 pending 位。
- **UART 是轮询的，突发输入会丢字符。** 115200 波特率下原来的 50 ms 轮询
  休眠会丢批量输入；改成 1 ms 轮询周期后，实测一次性写入多条命令仍会丢
  （收到的 `cat /etc/os-release` 变成 `sat /etc/os-relee`）。因此 1 ms 只能
  作为低速诊断回退，**不能**当作可靠的 UART 接收方案，也不替代设备 IRQ。
  在 LioIntc/PCH-PIC 路径可用前，测试命令需限速发送。
- **恢复动作有禁区**：禁止 `saveenv`、`sf write`、`sf erase`，禁止覆盖
  `/boot/uImage`。A20OS 挂起后只能物理复位；U-Boot 默认 `bootcmd` 仍从
  `/boot/uImage` 启动原 Linux。A20OS 仍在运行时不要尝试跳回 U-Boot。
  该固件倒计时为零秒，人工在看到 `Autoboot` 后再输入已经太晚；
  `tools/ls2k1000-uboot-stop.runscript` 负责截停（覆盖 USB 扫描窗口发送
  固件菜单键），但它不包含 Flash 写入或环境保存命令。

### 真机验收状态

下面的表是**验收结论**，不是变更日志。逐个候选镜像的字节数与 SHA-256 由
`tools/a20 ledger` 现算，不在这里手抄——手抄的数字改一个字节也不会有人发现。

| 能力 | 状态 | 证据入口 |
| --- | --- | --- |
| RAM-only 启动到 `[INIT] System ready (bringup, no userspace)` | 已验证（`NR_CPUS=1`、轮询控制台） | `make check-ls2k1000-build` + 上板串口日志 |
| 本地 timer 重复触发、异常返回、持续抢占、单核 timeout 与 idle wait 唤醒 | 已验证 | `timer_idle`、`timer_preempt`（QEMU 与真机各一轮） |
| UART0 LIOINTC（source 0 → CPU0 HWI1）投递、mask/ack/re-enable、与本地 timer 共存 | 已验证 | `/proc/interrupts`：UART0/cascade 计数上升且 spurious/storm 保持 0 |
| GMAC 无网线探测与 link-down 集成 | 已验证 | MAC `0x0000d137`、PHY `0x000001a`，打印 `PHY carrier down` 后绑定成功 |
| PHY link-up、descriptor DMA 收发、ping / socket 流量 | **未验证** | 需要接网线后重跑 |
| 除 UART0 外的设备 IRQ | **未验证** | 设备源保持 masked |
| 双核 SMP（硬件为 2×LA264） | **未验证** | `.smp = NULL`，构建强制 `NR_CPUS=1` |
| PCI 配置空间访问 | **未完成** | 缺 Loongson 配置窗口 shim，board 不调用 `pci_enumerate()` |
| AHCI/SATA 只读路径（IDENTIFY、MBR、ext4 只读挂载、`EROFS` 写栅栏） | 已验证（`SSTS=0x123`、`TFD=0x50`、62,533,296 sectors） | `storage_read_test` 输出 `SAMPLE PASS` |
| SATA 写入 | **不支持** | `STORAGE_READ_ONLY=1`，AHCI/EXT4/MBR 层均拒绝写 |

恢复验收（物理复位后沿未修改的 U-Boot 默认路径校验 `/boot/uImage` 并返回
vendor Linux root 提示符）在上述每一轮真机测试后都做过，是这些结论可以信赖
的前提。

> 恢复包的字节级事实（大小、SHA-256）随发布物变化，用
> `sha256sum -c SHA256SUMS` 对你手上那份归档校验，不要引用本文档里的数字。
> 板外必须留一份副本：只放在同一块系统盘上覆盖不了磁盘故障场景。

只读存储实现使用以下独立构建命令，不得与 cooperative 回退配置合并：

```sh
make -j"$(getconf _NPROCESSORS_ONLN)" \
  ARCH=loongarch64 BOARD=ls2k1000 ABI=linux BRINGUP=1 \
  RAMFS_USER=1 DRIVER_DEPLOYMENT=embedded STORAGE_READ_ONLY=1 \
  kernel-only
```

上板第一阶段只验收 AHCI IDENTIFY、容量、MBR 和安全失败行为。若 vendor 根分区仍带 `needs_recovery`，必须看到拒绝挂载并进入 RAM shell；不得从 A20OS 修复、回放日志或写入该分区。

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
5. 记录结论而不是字节：把**能力级的已验证/未验证状态**回填到本文的验收表与
   `docs/drivers/meta/implementation-status.md`；镜像的字节数、SHA-256、build 目录
   由 `tools/a20 ledger <instance>` 现算，启动日志由 `tools/a20 console` 落盘。
   不要把哈希和日期抄进文档——抄进去的数字没人会再去核对。
