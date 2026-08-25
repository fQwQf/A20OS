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
- LS2K1000 v3 候选镜像大小为 2,928,792 字节，SHA-256 为 `f729b38cd48167105abbbe960aa9e20a2d519b29a2af67c9c2dcb04bdf03165a`。真机识别 `SSTS=0x123`、`TFD=0x50`、`62,533,296` sectors 和 MBR `0x83` 分区，并将 ext4 只读挂载到 `/extra`；该轮询读取路径已 physical-board-validated，写入仍明确不受支持。

### 驱动状态与边界

- `ls2k_gmac.c`：使用 DWMAC1000 basic 16-byte ring descriptor，每个实例私有 spinlock 串行化 send/recv/poll；读取 ownership 前及交还 ownership 前执行 `dma_sync_for_device/cpu`（LoongArch 上 `cacop` 缓存维护）。
- MMIO 基址由 board 通过 VSEG 8 uncached DMW 提供；RAM 使用 VSEG 9 cached DMW（`PAGE_OFFSET=0x9000000000000000`）。
- vendor Linux 确认硬件也支持 enhanced/alternate descriptor，但当前候选未开启该模式。TX 使用 des0 的 OWN/IC/LS/FS/TER，RX ring end 使用 des1 的 RER；该布局仍需有网线时用实际收发验证。
- 无网线时 vendor Linux 对 eth0/eth1 均报告 `NO-CARRIER`。当前候选已把 PHY 存在与 carrier 状态分开，无 carrier 仍注册设备并向 lwIP 报告 link down；DMA IRQ 和 GMAC LIOINTC 源保持关闭。该无网线探测路径已通过 RAM-only 真机验证，但 descriptor 数据收发仍未验证。
- GMAC 无 generic `.a20drv` 包，只能 embedded 静态部署。
- UART 仍为轮询模式。手册第 15 章确认它兼容 NS16550A，接收数据进入 FIFO，`LSR.OE` 表示未及时读取造成溢出。无设备 IRQ 时原 50 ms 轮询休眠会在 115200 波特率下丢失批量输入；当前候选通过 board 参数把唤醒周期缩短为 1 ms，QEMU 路径仍保留 50 ms 默认值。真机低速分段输入可用，但突发输入仍会丢字符。

### 恢复与 RAM-only 启动

真机恢复包位于板载 Linux 的 `/root/a20-recovery-20260817/`，包含六个 MTD 分区、运行时 DTB、布局与系统信息及 SHA-256 校验文件。开始试启动前先在原 Linux 中执行 `sha256sum -c /root/a20-recovery-20260817/SHA256SUMS`。板外副本为 WSL 中的 `/home/gyy/a20-board-recovery/a20-recovery-20260817.tar.gz`，SHA-256 为 `2c4aa17a39c550b8edf1acca85b3a198da5f10cbec9f4d8cc1c100db11159088`；仅放在同一块系统盘上不能覆盖磁盘故障场景。

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

2026-08-17 RAM-only 真机启动已到达 `[INIT] System ready (bringup, no userspace)`，并完成 bring-up smoke test 的 `part ok` 关机路径。期间确认 LA264 会对编译器生成的未对齐宽访存触发异常，因此 LoongArch64 内核统一使用 `-mstrict-align`，且 PFA 的 `frame_meta_t` 数组元素显式保持 8 字节对齐。首次 timer IRQ 前保持 `CRMD.IE=0`，在 `proc_init()` 与 `net_init()` 完成后重装 one-shot timer 再开放中断；公共异常入口使用非向量模式，并只分派 `ECFG.LIE` 实际启用的 pending 位。该验收仅覆盖 `NR_CPUS=1`、轮询控制台、内存、VFS、驱动模块装载、网络核心初始化和本地 timer，不能替代设备 IRQ、GMAC 数据面、板载存储或双核 SMP 验收。

2026-08-18 的非协作 timer 实验镜像（2,613,016 字节，SHA-256 `e5840999ba35cfcf6fa68215c02df4b2cd8a7e653b543a9a27a56fa7b994bc71`）在真机打印 `LS2K1000 timer interrupts enabled`，进入 `mksh`，并完成 `help`、`cat /etc/os-release`、`ps`。这证明本地 timer 的重复触发和异常返回已覆盖到 PLV3 shell 路径；它尚未验证 `sleep`/timeout/idle wakeup。测试还复现了高速粘贴时的 UART FIFO 溢出风险，日志为 `/tmp/a20-ls2k-timer-phase1-board-run-20260818.log`。该次 A20OS 退出后停机，物理复位沿未修改的 U-Boot 默认路径加载 `/boot/uImage`，并成功返回 vendor Linux 5.10 的 root 提示符。

后续候选镜像 `/home/gyy/a20-ls2k-timer-sleep-poll-20260818.bin`（2,686,776 字节，SHA-256 `43bdcacb6cfb837a689d749b1f563b018cf0524fc11e8c17289dda4ddc2f7eb8`）加入 `/bin/sleep` 和 LS2K1000 专用 1 ms UART 轮询唤醒周期。QEMU 已完成三个 shell smoke 命令及 `sleep 1; echo SLEEP_OK`，日志为 `/tmp/a20-qemu-la64-timer-sleep-20260818.log`。真机从 U-Boot 只读加载相同字节数后进入开启 timer interrupt 的 `mksh`；低速分段输入下完成 `help`、`cat /etc/os-release`、`ps`、`sleep 1; echo SLEEP_OK` 和 `sleep 2; echo SLEEP2_OK`，证明本地 timer 重复异常返回和定时 sleep/wakeup 已 physical-board-validated。完整串口日志为 `/tmp/a20-ls2k-timer-sleep-poll-board-run-20260818.log`。最终物理复位沿未修改的 U-Boot 默认路径加载 vendor Linux 5.10 镜像并返回 root 提示符，恢复路径验证通过。

1 ms 轮询没有解决突发串口输入：宿主一次性发送 `cat /etc/os-release` 时，真机收到的是 `sat /etc/os-relee`。因此该改动只能作为低速诊断回退，不能标记为可靠 UART 接收或替代设备 IRQ；在 LioIntc/PCH-PIC 路径可用前，测试命令需限速发送。

RAM-only 候选 `/home/gyy/a20-ls2k-timer-preempt-20260818.bin` 为 2,764,632 字节，SHA-256 为 `0201dcdfb501c174e6139d4b2730dd6c27323113afa13a46d2fd3005639b5fe6`，板端文件为 `/boot/a20-ls2k-timer-preempt-20260818.bin`。它新增有界的 `/bin/timer_preempt`：一个子进程持续占用 CPU 约 750 ms，另一个子进程 sleep 100 ms 后写入管道；只有后者先完成才输出 PASS。QEMU 结果为 `order=SH first_ms=148 total_ms=755` 和 `TIMER_PREEMPT: PASS`，日志为 `/tmp/a20-qemu-la64-timer-preempt-20260818.log`。真机只读加载相同的 2,764,632 字节后得到 `order=SH first_ms=156 total_ms=750` 和 `TIMER_PREEMPT: PASS`，随后完成 `help`、`cat /etc/os-release`、`ps` 与 `sleep 1; echo SLEEP_OK`。因此持续 timer 抢占以及测试覆盖的子进程 exit/wait 已 physical-board-validated；完整日志为 `/tmp/a20-ls2k-timer-preempt-board-run-20260818.log`。cooperative LS2K、非 cooperative LS2K、QEMU 和 `check-ls2k1000-build` 均构建通过。最终物理复位沿未修改的 U-Boot 默认路径校验并启动 `Linux-5.10.0.lsgd+`，成功返回 vendor root 提示符，本次恢复检查通过。

下一 RAM-only 候选 `/home/gyy/a20-ls2k-timer-idle-20260818.bin` 为 2,838,392 字节，SHA-256 为 `bb74dbc71705beb47febe293862b427d864631a24033c62d3e9414e5e9ab17e2`。它新增 `/bin/timer_idle`：先读取 `/proc/a20/perf` 开启并快照计数器，再执行 `poll(NULL, 0, 200)`，最后要求 `idle_wait_entries` 和 `idle_wait_wake_returns` 均增加。QEMU 得到 200 ms timeout、两个计数器 delta 均为 45，并输出 `TIMER_IDLE: PASS`；日志为 `/tmp/a20-qemu-la64-timer-idle-20260818.log`。真机的 `timer_idle` 本身也得到 202 ms timeout、两个 delta 均为 50 并输出 PASS，但紧接着运行 `timer_preempt` 时只打印 `start` 后挂起。原因范围收敛到读取 perf 文件后全局 syscall profiling 一直保持开启，污染了后续压力测试，因此该候选被拒绝，不能标记为完整 physical-board-validated。

修正版 `/home/gyy/a20-ls2k-timer-idle-v2-20260818.bin` 大小仍为 2,838,392 字节，SHA-256 为 `c4fd8c5b221da3b5bb75330a09e486872068efc4de9a65a0ac248518d4d2b6e0`，板端文件为 `/boot/a20-ls2k-timer-idle-v2-20260818.bin`。`/proc/a20/perf` 现在允许显式关闭计数，`timer_idle` 在返回前恢复默认关闭状态。QEMU 已按 `timer_idle`、`timer_preempt`、shell smoke 的精确顺序通过，日志为 `/tmp/a20-qemu-la64-timer-idle-disable-20260818.log`；cooperative LS2K、非 cooperative LS2K、QEMU 和 `check-ls2k1000-build` 均构建通过。真机按同一顺序得到 202 ms timeout、`idle_wait_entries` 与 `idle_wait_wake_returns` delta 均为 50、`TIMER_IDLE: PASS`，紧接着的抢占测试得到 `order=SH first_ms=156 total_ms=750` 和 `TIMER_PREEMPT: PASS`，随后 `help`、`cat /etc/os-release`、`ps` 均通过。这证明修正版不再污染后续压力测试，单核 timeout、idle wait 唤醒、持续抢占和该测试覆盖的 exit/wait 路径已 physical-board-validated。之后尝试的组合 `sleep` 命令被轮询 UART 丢字符破坏，不计为本轮结果；定时 sleep/wakeup 已由前一候选单独验证。完整日志为 `/tmp/a20-ls2k-timer-idle-v2-board-run-20260818.log`。最终物理复位沿未修改的 U-Boot 默认路径校验并启动 `Linux-5.10.0.lsgd+`，成功返回 vendor root 提示符，本次恢复检查通过。

UART0 LIOINTC 候选 `/home/gyy/a20-ls2k-uart-irq-v1-20260818.bin` 为 2,838,392 字节，SHA-256 为 `98533ae44132529f26f8b0ed046a63fb1eeb30db0448f95cc8b205aafe2a4773`，板端同名文件位于 `/boot`。该镜像仅将 source 0 路由到 CPU0 HWI1 (`0x21`)，其余 63 个 LIOINTC 源保持 masked，并保留轮询回退。真机首次读取 `/proc/interrupts` 得到 UART0/source 0 与 cascade 均为 21、spurious/storm 均为 0；一次主机写入四条 `echo` 和一条 `cat` 的突发测试全部完整执行，计数升至 138，错误计数仍为 0。随后 `help`、`cat /etc/os-release`、`ps` 和 `timer_preempt` 均通过，抢占结果为 `order=SH first_ms=160 total_ms=750`。这证明 UART0 中断投递、mask/ack/re-enable、有界分派及与本地 timer 共存已通过真机功能测试。最终物理复位沿未修改的默认 U-Boot 路径校验并启动 vendor `Linux-5.10.0.lsgd+`，返回 root 提示符，本次恢复检查通过。

远端 `main` 重写后，`dev/2k1000` 基于 `7523d79e` 重放并在提交 `78f5c2af` 生成候选 `/home/gyy/a20-ls2k-rebase-uartirq-v2-20260818.bin`；板端文件为 `/boot/a20-ls2k-rebase-uartirq-v2-20260818.bin`，大小 2,842,496 字节，WSL 与板端 SHA-256 均为 `7356c48836efeaf0a1343f953b73322bedb2b3cfad3330fab426c7918c68d148`。严格 cooperative、非 cooperative、QEMU 构建及 `make check-ls2k1000-build` 全部通过。QEMU 的 `timer_idle` 为 202 ms、idle entry/wake delta 均为 48，`timer_preempt` 为 `order=SH first_ms=157 total_ms=751`；日志为 `/tmp/a20-rebase-qemu-focus-20260818.log`。真机得到 202 ms、delta 均为 50，以及 `order=SH first_ms=160 total_ms=750`。一次主机写入四条 `echo` 和一条 `cat` 全部完整执行，UART0/cascade 从 21 增至 139，spurious/storm 保持 0，随后三个 shell smoke 命令通过。完整实验与最终复位日志为 `/tmp/a20-ls2k-rebase-uartirq-v2-board-run2-20260818.log`；复位后默认 U-Boot 校验并启动 `/boot/uImage` 中的 `Linux-5.10.0.lsgd+`，返回 vendor root 提示符。因此 timer/idle/preemption 与 UART0 LIOINTC 在重写后的主线基线上再次完成 physical-board validation 和恢复验收。

Phase 3 无网线候选 `/home/gyy/a20-ls2k-gmac-nolink-v1-20260818.bin` 的板端文件为 `/boot/a20-ls2k-gmac-nolink-v1-20260818.bin`，大小 2,842,520 字节，WSL、板端临时文件和 `/boot` 文件的 SHA-256 均为 `9c4606935b2d66d63df4c64d6147c8289c9ca9b0ace399c6a2981678e8314e63`。真机 RAM-only 启动只探测 GMAC0 一次，读取 MAC 版本 `0x0000d137` 和 PHY ID `0x0000010a`，打印 `PHY carrier down at addr 0; continuing without cable` 后成功绑定驱动并将 lwIP netif 挂接为未配置/无链路状态。DMA IRQ 与 GMAC LIOINTC 源全程关闭；`timer_idle` 得到 202 ms、idle entry/wake delta 均为 50 并 PASS，`timer_preempt` 得到 `order=SH first_ms=160 total_ms=750` 并 PASS，随后三个 shell smoke 命令通过。`/proc/interrupts` 仅显示 UART0/source 0，spurious/storm 均为 0，未发生 GMAC 中断风暴。上传日志为 `/tmp/a20-ls2k-gmac-nolink-v1-board-run-20260818.log`，完整 RAM 启动与最终恢复日志为 `/tmp/a20-ls2k-gmac-nolink-v1-ramboot-single-20260818.log`。物理复位后默认 U-Boot 路径重新校验并启动 vendor `Linux-5.10.0.lsgd+`，返回 root 提示符，恢复验收通过。该结果只证明无网线探测和 link-down 集成；PHY link-up、descriptor DMA 收发、ping 和 socket 流量仍未验证。

2026-08-19 的只读存储 v3 候选 `/home/gyy/a20-ls2k-storage-vim-v3-20260819.bin` 在 WSL 和板端均为 2,928,792 字节，SHA-256 均为 `f729b38cd48167105abbbe960aa9e20a2d519b29a2af67c9c2dcb04bdf03165a`。`make check-ls2k1000-build` 通过；QEMU `ich9-ahci` 完成 MBR/ext4 只读挂载、`EROFS` 写栅栏、`storage_read_test` 和三个 shell smoke，日志为 `/tmp/a20-qemu-storage-vim-v3-20260819.log`。真机在 U-Boot `scsi reset` 后 RAM-only 启动同一镜像，AHCI 报告 `SSTS=0x123`、`TFD=0x50` 和 `62,533,296` sectors，识别 MBR type `0x83` 并将 ext4 只读挂载到 `/extra`。`storage_read_test` 对 3,541,040 字节的 Vim 文件验证首尾采样和 `EROFS` 写栅栏并输出 `SAMPLE PASS`，随后 `help`、`cat /etc/os-release` 和 `ps` 均通过。完整板端日志为 `/tmp/a20-ls2k-storage-vim-v3-board-20260819.log`。最终物理复位沿未修改的默认 U-Boot 路径读取 SATA、校验 `/boot/uImage` 为 `OK`，启动 vendor `Linux-5.10.0.lsgd+` 并返回 root 提示符；`eth0` 恢复为 100 Mbps/full-duplex，恢复验收通过。

同轮测试通过 vendor Linux 的 `eth0` 100 Mbps/full-duplex 链路上传严格对齐版 Vim 到新文件 `/boot/a20-vim-static-strict-20260819`；WSL 与板端大小均为 3,541,040 字节，SHA-256 均为 `ae25dffdf70c7e9506d82df5a87f470946db4abe3d39c87320a93ca782b0a708`。本次测试二进制使用 `-mstrict-align`，修复旧版本在 LA264 上触发的 `ALE code=9`。该 Vim 已在 QEMU 和真机 A20OS 中从 `/extra` 执行：Ex 模式 `writefile()` 成功写入 RAMFS；交互模式成功插入 `A20_VIM_INTERACTIVE`、执行 `:wq` 并由 `cat` 读回。因此 ext4 可执行文件读取、PLV3 `execve`、Vim 用户态运行和 RAMFS 文件创建/写回已 physical-board-validated；这不改变 `/extra` 和底层 AHCI 的只读约束。

严格对齐版 Git 2.54.0 的真机候选为 `/boot/a20-git-static-strict-v2-20260819`，WSL 与板端大小均为 3,998,624 字节，SHA-256 均为 `914fa587d8bef97b699d1ec8d08a22ab2d2ffa6bc6c2d128768cf52bbe831008`。真机从只读 `/extra` 执行该文件，并在 RAMFS 中完成 `init`、`hash-object`、`add`、两次 `commit`、`log`、`status` 和 `fsck --full`；第二次提交成功派生 `maintenance run --auto`，`fsck` 成功派生 `refs verify`、`commit-graph verify` 和 `multi-pack-index verify`，未发生 ALE。该轮临时部署提供了测试所需的 Git helper；单独上传主二进制时缺少 templates 的 warning 不影响上述命令结果。本分支不包含完整 `extra.img` 的 Git 集成改动。完整串口日志为 `/tmp/a20-ls2k-git-strict-board-20260819.log`；最终物理复位沿未修改的默认 U-Boot 路径从 SATA 读取并校验原厂镜像为 `OK`，启动 `Linux 5.10.0.lsgd+` 并返回 `[root@LS-GD ~]#`，恢复验收通过。

不要把 vendor 根分区中的既有用户程序当作 A20OS 兼容程序：本轮从 `/extra/bin` 执行 vendor `mkdir` 和 `chmod` 均在 LA264 触发 `ALE code=9`，而本次使用 `-mstrict-align` 重建的 Vim 和 Git 没有该异常。这说明后续外部程序适配必须关注 LA264 的严格对齐要求；它不表示 vendor Linux 自身运行这些二进制存在问题，也不在本分支修改 `extra` 构建规则。

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
5. 记录固件版本、启动日志、压力测试结果并回填本文与 `docs/drivers/meta/implementation-status.md`。
