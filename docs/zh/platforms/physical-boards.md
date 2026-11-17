# VisionFive 2 与 LS2K1000 的板级接口参考

两块板的目标、设备支持范围、宿主依赖、构建产物、串口接线、部署和恢复步骤分别在：

- [StarFive VisionFive 2](visionfive2-boot.md)：SPL/OpenSBI 直接引导 FIT、SD 镜像和可选 QSPI。
- [LS2K1000-DP-V10](2k1000.md)：保留厂商启动环境、RAM-only `go` 交接和只读 SATA。

这两篇是完整的单板操作入口。本页只说明共享代码与板级约定，不作为某一块板可执行的启动配方。

## 机制与平台配置

`kernel/arch/<arch>/boards/<board>/board.c` 提供 `current_board`，描述 RAM、定时器、中断控制器、设备枚举、SMP 和电源操作。体系结构代码负责异常入口、页表及固件调用；板级负责具体 MMIO、时钟、复位和交接约束。

| 接口 | VisionFive 2 | LS2K1000-DP-V10 |
| --- | --- | --- |
| 内核选择 | `ARCH=riscv64 BOARD=visionfive2` | `ARCH=loongarch64 BOARD=ls2k1000` |
| 入口协议 | S-mode，SBI，hart ID + DTB | 厂商 U-Boot `go`，板级入口读取约定 DTB 缓冲 |
| 内存布局 | 物理 `0x40000000` 起，内核 `0x40200000` | 保守物理 RAM 窗口；cached DMW 内核 `0x9000000002000000` |
| 中断 | 标准 PLIC；定时器经 SBI | 片内 LIOINTC；本地 timer；不套用 QEMU PCH-PIC |
| 用户定时器访问 | 板级标记 vDSO 用户时间 CSR 不可靠，走兼容路径 | 由 LoongArch 体系结构机制提供 |
| SMP | 有 SBI HSM/IPI 操作，需单独硬件验收 | 无板级 SMP 操作，默认单核 |
| 轮询配置 | 控制台 RX、GMAC 数据面；SDIO 可按资源选择 IRQ | cooperative UART、GMAC、只读 AHCI；正常 UART0 有 IRQ |

硬件地址、固件保留区和接口语义不同；不能将两板 Make 参数、DTB、`booti`/`go` 命令或 QEMU 寄存器窗口互换。

## 设备枚举与驱动部署

VisionFive 2 优先从固件 DTB 枚举平台设备，缺失可用描述时使用板级回退资源表。 SDIO 的 IRQ 资源来自 DTB；回退表不猜测线号。`dw_sdio.c` 没有 IRQ 资源时使用轮询，IRQ 首次超时后屏蔽并降级；可用 `a20.dw-sdio.poll=1` 做对照。中断路径是否在真板可用，以对应设备验收为准。

LS2K1000 的 GMAC/AHCI 由板级直接提供 MMIO 资源；AHCI 仅在`STORAGE_READ_ONLY=1` 时注册，不依赖尚未提供的 PCI 配置窗口前端。它保留厂商 U-Boot 建立的 SATA PHY 链路，拒绝存储写入。

`starfive_gmac.c` 与 `ls2k_gmac.c` 都需要 `DRIVER_DEPLOYMENT=embedded`，尚无可替代的通用 `.a20drv` 包。数据面仍使用私有锁/轮询及`dma_sync_for_device/cpu`；PHY 已发现、carrier down 或队列已初始化都不等于完成了实际数据收发验收。

## 内存与固件所有权

VisionFive 2 根据 DTB 内存节点和保留区收窄可分配范围； RAM 基址与启动映射窗口由板级链接脚本确定。 LS2K1000 保持已验收的安全 RAM 窗口，不能因为固件 DTB 宣称更多内存就扩大分配。 DTB 自身、固件、显示缓冲及内核 BSS 都必须在交接时保留，不能只按 `kernel.bin` 的磁盘长度判断内存是否重叠。

固件具备 SD、TFTP、USB 或网络驱动，不表示 A20OS 接管后仍可调用它们。 A20OS 只能使用自己的驱动，硬件验收须覆盖实际读写、中断、缓存一致性和恢复。

## 构建和验收约定

```sh
make check-visionfive2-build
make check-ls2k1000-build
```

这两个目标分别检查普通和 embedded 构建；它们不连接硬件，不证明已完成启动、网络、存储或多核验收。单板页说明已有真机基线与尚未验证的路径。

构建目录随选项变化，用相同参数执行 `make -s ... print-build-dir` 查询；镜像大小与 SHA-256 在打包后计算，日志须与实际 ELF/镜像对应。不要把旧候选的字节数、磁盘扇区数或个人目录当成平台常量。

公共移植接口见[移植指南](porting-guide.md)，平台文档约定见[平台约定](README.md)。
