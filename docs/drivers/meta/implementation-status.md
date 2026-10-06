# 驱动实现符合性与限制

> 不要恢复已清理的双初始化入口；不要扩大已知边界却不同时更新说明和文档；不要把“符合”当成“所有平台所有配置都成立”。

审查覆盖 driver core、公共总线、STM32F103、VirtualBox ARM64/x86_64 及其设备驱动。

## 如何阅读矩阵

每行矩阵的格式是：范围、状态、说明。

- **符合**：源码和文档一致，当前可以依赖。
- **已扩展/基础可用**：已实现，但有明确限制（限制写在说明栏）。
- **有条件**：只在特定平台或特定序列化假设下工作，SMP/IRQ 化前要先加锁。
- **动态类视图**：char/block/audio 自动发布 devfs 节点，所有 class 发布 sysfs 目录；display/input 的旧聚合节点仍保留。

## 如何更新矩阵

1. 先在目标平台或 QEMU 或 VirtualBox 上复现实验，记录 commit、构建命令和日志。
2. 如果是新增行，说明里写清平台、版本、观察到的运行边界和失败行为。
3. 如果改动了现有项的状态或限制，必须同步更新相关平台文档和 [构建、测试与提交](testing-and-submission.md) 中的测试矩阵。
4. 不要只改状态不改说明。新改动不得扩大已知边界，除非文档里已经解释了原因。

from-source GUI 栈与 LVGL 原生桌面已退役（归档在分支 `archive/legacy-desktop`），其 `smoke-qemu-gui-*` 冒烟目标与 `tools/smoke_qemu_gui.py` 一并移除。当前 GUI 桌面由 Alpine `xfce` world 提供，验证走 `make run-world-gui PKG_WORLD=xfce`。门禁入口见 [testing-gates.md](../../testing-gates.md)。

## 核心与公共基础设施

| 范围 | 状态 | 说明 |
|---|---|---|
| driver core | 符合 | 动态 registry；bus ID 后可执行无副作用的 driver protocol match；操作 mutex 串行化注册、probe/remove 与遍历；数组 spinlock 不跨回调；probe 失败完整解绑 |
| PCI bus | 已扩展 | ECAM、ID/subsystem match、零值未分配 BAR sizing/assignment、BAR 编号查询、modern VirtIO capabilities；已设置 `matched_id` |
| MSI-X (消息中断) | x86_64 已实现亲和性；非 x86 未实现 | 公共层 `kernel/drivers/bus/pci_msix.c`：capability 两种编码解析、条目编程与回读校验、PBA、Function Mask，以及每条目的 `target_cpu`/`vector` 记录。驱动侧 API `pci_msix_set_affinity(dev, index, cpu)` / `pci_msix_get_affinity()` / `pci_msix_set_all_affinity(cpu)`（后两个已进 drvmod 符号白名单）。**x86_64**：消息地址按目标 CPU 的 APIC ID 生成（`LAPIC_PHYS_BASE + (apic_id << 12)`，xAPIC 物理目的模式），目标 CPU 不是当前 CPU 时经 IPI（向量 `IRQ_VECTOR_MSIX_VECTOR`）在远端写 LVT，流程为 mask → 改写地址 → 远端 arm → unmask；地址回读不符返回 `-EIO` 且状态等同迁移前，远端 arm 失败则条目保持 masked 并停在新地址（不丢中断，但该 vector 不通），已成功的条目不回滚。默认目标仍是 boot CPU（`PCI_MSIX_CPU_BOOT`），与改动前逐位一致。运行时入口 `/proc/a20/irq_affinity`（读＝全部已编程条目，写＝单个十进制 CPU id 迁移全部条目）。**残留 1：非 x86 平台实现** —— riscv64/aarch64/loongarch64/ppc64le 的 GIC/EIOINTC/MPIC 尚未接 `arch_msix_message_address()`/`arch_msix_vector_setup()`，弱符号默认返回 `-EOPNOTSUPP`，`arch_irq_msix_cpu_count()` 返回 1，只有 boot CPU 合法。**残留 2：IRQ 亲和性 API 采用情况** —— 目前仅 `pci_msix_set_affinity()` 与 `/proc/a20/irq_affinity` 两个入口，无 per-function/per-queue 的细粒度接口、无 cmdline 亲和性策略、无驱动在 probe 期自行选 CPU；e1000e 的 MSI-X 仍只验证到表被解析并 arm（网卡无流量故未实测投递），本次改动未扩展它 |
| VirtIO-MMIO bus | 符合 | MMIO/IRQ 资源、type match、`matched_id`；静态最多 8 个设备 |
| platform bus | 已扩展 | `resource_t` 一直有 `RES_IRQ`，riscv64 FDT 枚举路径也已从 `interrupts` 生成 `RES_IRQ`；本次补上驱动侧唯一入口 `platform_device_irq(dev)`，语义为 `>= 0` 可用线号 / `-ENODEV` 板级未发布 IRQ 资源（调用方轮询，**不是错误**）/ `-EINVAL` 非 platform 设备或 `end != start`（只接受单点线号）/ `-ERANGE` 超出固定 256 线表。向后兼容是完整的：不带 `RES_IRQ` 的设备读出 `-ENODEV`，未使用该函数的驱动行为不变 |
| hwapi MMIO | 符合 | 宽度访问和 barrier；relaxed 版本需谨慎 |
| hwapi IRQ | 已扩展 | 固定 256 线；`IRQF_SHARED` 提供单链表 handler chain（首个注册必须是共享标志，后续共享 handler 追加）；非共享重复注册返回 `-EBUSY`；`free_irq` 校验 owner token 并等待在途 handler |
| hwapi DMA | 基础可用 | coherent helper 返回物理 handle，sync 委托 arch cache hook；`device_t.dma_mask`（默认 64 位全通）+ `dma_set_mask()`/`dma_addr_ok()`/`dma_range_ok()`，分配器按 mask 校验：kmalloc 块落在窗口外时改从帧分配器取物理连续页，aligned 版本有界重试，不满足返回 `NULL` 并让 probe 失败，无 bounce 路径也不截断 handle；HDA 按 GCAP[0] 声明窗口，xHCI/E1000 声明 64-bit；限制：仍无 IOMMU/IOVA 与 bounce，帧分配器无分区接口（32 位 mask 在 RAM 全高于 4 GiB 的机器上会 probe 失败），`dma_alloc_coherent_aligned` 的对齐上限仍是 `PAGE_SIZE` |
| class publication | 已扩展 | probe 成功自动建立带引用的 class device；remove 前先下线并排空在途调用 |
| devfs/sysfs | 动态类视图 | char/block/audio 自动生成 `/dev/charN`、`diskN`、`audioN`；所有 class 动态暴露于 `/sys/class`；旧 display/input 聚合节点暂时保留 |
| drvmod loader | 基础可用 | 四架构（riscv64/x86_64/aarch64/loongarch64）ELF `ET_REL`、`.a20drv` 描述符校验（唯一元数据，覆盖 RTC..USB 全部类型）、框架符号白名单、全量 ELF 边界校验（section/symbol/strtab/relocation）、按 `e_machine` 选择重定位解码器（RISC-V CALL、x86-64 large 模型 ABS64/`.lbss`、AArch64 CALL26+veneer、LoongArch CALL36/pcalau12i）；模块与内建共用同一 `driver_t`（经 `drv_driver_register` 注册，核心 `device_t` 匹配/probe），不再有第二套 `drv_driver_t`/`drv_device_t`；统一驱动管理器（`driver_manager.c`）扫描 DriverStore、按描述符路由内核模块与用户服务，已 pinned 模块不会被重复执行 DriverEntry；drv_env 双驻留共享代码的模块后端（DRV_ENV_DRVMOD）；goldfish RTC、CMOS RTC（x86_64 Early DriverStore）、PC speaker、virtio-input 内核探针、virtio-input 完整驱动、PS/2 键鼠控制器、NVMe、TPM 2.0、Intel HDA、virtio-blk、virtio-scsi、AHCI、DW SDIO、E1000、RTL8139、VMSVGA、virtio-net、virtio-gpu、virtio-snd、virtio-console、virtio-rng、xHCI、USB HID、USB storage 均已迁移为模块（generic 无内建设备驱动）；PCI 类驱动访问器、DMA 增强（aligned/sync）与地址 mask（`dma_set_mask`/`dma_addr_ok`/`dma_range_ok`）、调度/等待原语、`clock_ticks_per_sec`、内核熵池 `random_reseed`（供硬件 RNG 模块把读到的字节折进 `kernel/core/random.c`，见下方 virtio-rng 行）与 ACPI TPM2 发现均已导出；签名、运行时 unload 尚未完成 |

## drvmod 迁移矩阵

| 驱动 | 迁移状态 | 说明 |
|---|---|---|
| goldfish RTC | 已迁移 | 内建 `goldfish_rtc_kdrv.c` 已删除；`rtc.a20drv` 在 riscv64/aarch64/loongarch64 QEMU virt 上由 `smoke-drvmod-*` 验证绑定 |
| PC speaker | 已迁移 | 内建 `pc_speaker.c` 已删除；`pc-spkr.a20drv`（x86_64）经统一驱动核心桥接注册 AUDIO 类并绑定 platform 设备 |
| CMOS RTC (MC146818) | 新增 `.a20drv` | `cmos-rtc.a20drv`（x86_64 Early）：x86_64 墙钟来源，只读、无 IRQ、无时区换算、century 寄存器不可信时按 20xx 处理；门禁 `smoke-rtc-cmos` **已实跑 PASS**（`-rtc base=utc`，宿主侧复核来客墙钟；日志 `.kernel-build/smoke/rtc-cmos-x86_64.log`，来客采用 epoch 1791231484，与编译期种子 1791230264 不同，说明读到的是硬件），`smoke-rtc-cmos-fallback`（CMOS 落在可接受窗口外时回退到编译期种子）**尚未执行过**。两块 x86_64 板级（qemu-virt-x86_64、x86_64-pc）都声明了该 platform 设备 |
| UART | 内核服务 | 启动早期串口服务，模块加载前必须可用，不是可迁移设备包 |
| virtio-blk | 已迁移 | 根文件系统挂载依赖它；`virtio-blk.a20drv` 已嵌入 Early DriverStore（root ramfs overlay），在挂载真实根盘前加载，解除 bootstrap 循环 |
| PCI bus、virtio-mmio bus、USB core、framebuffer/gpu_core、audio_core、input_mux | 内核服务 | transport、class 聚合和设备节点服务，保持静态链接，不计入迁移账本 |
| DMA/PCI BAR/class ops 框架 API | 已导出 | `drv_dma_alloc_coherent`/`dma_alloc_coherent_aligned`/`dma_sync_*`、`pci_get_bar_resource`/`pci_enable_and_assign_bars`/`pci_intx_irq`/`pci_class_code`/`pci_bus`、block/net/input/audio/display class 操作（统一核心桥接）|
| virtio-input 内核探针 | 已迁移 | 内建 `virtio_input_kprobe.c` 已删除；`vinput-probe.a20drv` 用 drv_env 模块后端复用双驻留共享协议，`smoke-dual-input` 验证与用户态 uinputd 读到同一设备身份 |
| virtio-input 完整事件投递 | 已迁移 | `vinput.a20drv` 模块（virtq + IRQ + input class），`/dev/event0` mux 拆分至 `kernel/drivers/input/input_mux.c`，`input_mux_wake` 导出；QEMU 实测事件流 |
| AHCI、DW SDIO、virtio-scsi | 已迁移 | `ahci.a20drv`（x86_64 Early）、`dw-sdio.a20drv`（riscv64 Early）、`virtio-scsi.a20drv`（四架构 Early）经 Early DriverStore 在根盘挂载前加载，与 virtio-blk 一起解决根设备 bootstrap |
| E1000、virtio-net、virtio-gpu、vmsvga、virtio-snd、xHCI、USB HID、USB storage | `.a20drv` | 设备实现仅在 `tools/driver-modules.mk` 为目标架构列出时由 generic DriverStore 提供；其 transport、framebuffer 和 class 服务仍由内核提供 |
| virtio-console | 新增 `.a20drv` | `virtio-console.a20drv`（riscv64/x86_64/aarch64/loongarch64）：virtio-serial port 0 → CHAR 类 `/dev/vport0`，rx 中断 / tx 同步，收到数据默认镜像进控制台输入。**single-port、不协商 MULTIPORT、单静态实例、无 MSI-X**；门禁 `smoke-virtio-console`（x86_64 QEMU）**已实跑 PASS**（日志 `.kernel-build/smoke/virtio-console-x86_64.log`） |
| virtio-rng | 新增 `.a20drv` | `virtio-rng.a20drv`（riscv64/x86_64）：`VIRTIO_ID_RNG` 熵源 → CHAR 类 `/dev/hwrng`，读到的字节同时经 `random_reseed()` 折进 `kernel/core/random.c`（因此需要新增导出该符号）。双 transport：PCI（x86_64，`1af4:1044` modern 与 `1af4:1005` transitional）与 virtio-mmio（riscv64，device-id 4）。**单静态实例、队列 0 的 8×256 B 设备缓冲只在 read 需要熵时才挂上（QEMU 每次按 avail ring 字节数向宿主索取，挂满就等于持续抽宿主熵）、读路径 500 ms 有界等待、熵池只在 read 路径喂（ISR 不喂，`random_reseed` 经 `arch_entropy_sample()` 触及 `proc_current()`）**；门禁 `smoke-virtio-rng`（x86_64 QEMU + `virtio-rng-pci`，`hwrng_test` 读 ≥256 B 并断言非全 0x00/全 0xff/单字节重复）**已实跑 PASS**（日志 `.kernel-build/smoke/virtio-rng-x86_64.log`，`HWRNG_TEST: PASS bytes=256`） |
| RTL8139 | 新增 `.a20drv` | `rtl8139.a20drv`（x86_64）：PCI 10ec:8139 + class `0x020000`，TSAD/TSD 四描述符 TX、64 KiB RX ring、IMR/ISR 中断收发。此前该型号无任何驱动认领，`-nic user,model=rtl8139` 起不来任何 NET 设备。门禁 `smoke-net-rtl8139`（hostfwd 端到端回环 + 中断计数）**已实跑 PASS**；寄存器图取自 QEMU 设备模型，未在真实硅片上验证 |
| TPM 2.0 | 已迁移 | `tpm.a20drv`（x86_64）：`firmware_acpi_tpm2` 导出 + TIS FIFO，无设备优雅失败 |
| GMAC、SDIO | 板级 platform | StarFive/LS2K GMAC 与 DW SDIO 通过 `platform_bus` + `hardware_id` 注册绑定。板级 IRQ 资源经 `platform_device_irq()` 读取：发布了就用中断，没发布就轮询。DW SDIO 有 riscv64 generic Early 模块；两个 GMAC 当前只在 embedded 的显式静态账本中，没有 generic 包。板级 bring-up 细节（时钟、地址、IRQ、缺项）见 [物理开发板移植](../../platforms/physical-boards.md) |

迁移细节与顺序见 [kernel-modules.md](../guide/kernel-modules.md)。

## 设备驱动

| 驱动 | 类 | 状态与限制 |
|---|---|---|
| virtio-blk | BLOCK | 类接口；**完成路径默认中断驱动**，三级阶梯：每队列一个 MSI-X 向量 → 共享 INTx → 轮询（无 cmdline 开关，阶梯由 `request_irq()` 是否成功决定）。每级只在前一级失败时进入，MSI-X 部分注册成功时把已注册的那些 `free_irq()` 掉再退回 INTx。**`/proc/a20/perf` 尚未把中断与轮询两条路径分开计数**（现有 `virtio_blk_*` 计数器是 polls/completions/dma，不区分驱动方），因此从外部无法判断中断路径是否真的在工作 —— 这是与 AHCI、E1000 不同的一处缺口。remove 停止设备并释放已注册 IRQ；有限静态实例 |
| virtio-net | NET | 多实例、IRQ/轮询和类接口；remove 释放 IRQ 并复位 transport |
| virtio-input | INPUT | 类接口、PCI/MMIO、多实例槽和 remove；由 `/dev/event0` 聚合 |
| virtio-gpu | DISPLAY | class registry、framebuffer 页释放和 transport reset；单实例、同步 controlq；VIRGL 3D feature 协商、capset 查询与 `CTX_CREATE`/`RESOURCE_CREATE_3D`/`SUBMIT_3D` 透传（经 `DRM_IOCTL_VIRTGPU_*` UAPI，见 [3D 图形加速栈](../../graphics/3d-graphics.md)）；2D-only 设备自动回退 |
| VirtIO-SCSI | BLOCK | VirtualBox ARM 已验证，remove 复位/释放槽；只支持 target/LUN 0、READ/WRITE(10)、SYNCHRONIZE CACHE(10)、512B sector、每控制器单条 in-flight 命令。command queue 完成走 used-ring 中断：probe 按 MSI-X → 共享 INTx → 轮询顺序选路（`a20.virtio-scsi.poll=1` 强制轮询，注册失败也回退轮询），IRQ top-half 只 ack 并唤醒 park 中的提交者。混合完成窗口仍在（先自旋 `VIRTIO_SCSI_HYBRID_PRE_POLL_US`，再 park 50 ms 一段），因此短命令多数在自旋窗口内完成、被中断唤醒的比例未量化。x86_64 QEMU（virtio-scsi-pci + scsi-hd）由 `make smoke-virtio-scsi-irq` 验证读写/flush 回环与 `irq_count > 0`；其余架构的中断投递**未验证**，VirtualBox ARM 那条路径仍以轮询为主 |
| AHCI | BLOCK | VirtualBox x86_64；单 controller/单 port/单 slot、LBA48；probe 回滚和 remove 释放 DMA/IRQ。完成路径默认中断驱动（PCI INTx；`PxIE` 与 `GHC.IE` 只在 `request_irq()` 成功后才打开），`a20.ahci.poll=1` 强制纯轮询；`/proc/a20/perf` 的 `ahci_irq_completions`/`ahci_poll_completions` 把两条路径分开计数，否则它们从外部同样表现为"盘能用"。门禁：`make smoke-ahci-ich9`（x86_64 QEMU q35 + `-device ich9-ahci` + `ide-hd`，ext4 挂 /extra，断言 `block_flushes` 增长且中断完成计数 > 0）**已实跑 PASS**（日志 `.kernel-build/smoke/ahci-ich9-x86_64.log`，`FSYNC_TEST: PASS` 且中断完成计数 > 0），因此 QEMU 上中断路径已验证。仅 INTx，无 MSI-X 路径；真实 SATA PHY 与未中断硬件未验证 |
| NVMe | BLOCK | 架构无关 PCI class 驱动；x86_64 与 LoongArch64 构建，LoongArch QEMU 已验证 BAR、CAP、admin/I/O queue、Identify，以及跨 8 KiB bounce chunk 的写入/flush/读回比较；要求 NVM command set 和兼容 4 KiB memory page，首个活动 namespace、轮询、每 controller 只发布一个 namespace |
| E1000 | NET | VirtualBox/QEMU 82540EM，**完成路径默认中断驱动**（MSI-X，回退 INTx；`request_irq()` 成功后才打开 `IMS`，无向量时保持纯轮询），已加 stop/remove；静态单实例；probe 按 RDBAH/TDBAH 的 64 位拆分寄存器声明 64-bit DMA mask，ring 块越窗时 probe 失败（`-EOPNOTSUPP`）；`a20.e1000.poll=1` 强制纯轮询；RX 由 ISR 一次性排空（复用 `a20_lwip_process_netif_irq_locked()`，**无 NAPI budget**），TX 靠 `tx_done` 镜像 TDH 在发送路径与中断路径释放描述符（旧代码清 DD 后从不重置，ring 首绕之后即永久失败）；`/proc/a20/perf` 的 `e1000_irq_calls`/`_rx`/`_tx`/`_empty`/`e1000_tx_reclaimed`/`e1000_rx_drained` 分开计数（`_rx`/`_tx` 是 `_calls` 的子集，不相加）。门禁：`make smoke-net-e1000-irq`（x86_64 QEMU q35 + `-device e1000` + hostfwd 端到端回环，断言中断计数 > 0）**已实跑 PASS**（日志 `.kernel-build/smoke/net-e1000-irq-x86_64.log`），QEMU 上中断送达与端到端回环已验证；`a20.e1000.poll=1` 的强制轮询分支本身未被该门禁覆盖；单静态实例仍是已知限制 |
| RTL8139 | NET | Realtek 10ec:8139，PCI class `0x020000` 匹配；x86_64 QEMU（`-device rtl8139`），此前该型号无任何驱动认领、`DEV_CLASS_NET` 里根本没有网卡。**默认中断驱动**（PCI INTx；`request_irq()` 成功且 handler 就位后才写 `IMR`，失败或 `a20.rtl8139.poll=1` 则纯轮询）；静态单实例；RX 64 KiB ring（`RCR[12:11]=0b11`）+ 4×2048 TX 描述符，同一块 kmalloc、按 2048 对齐；`send()` 先线性拷贝进 bounce buffer 再写 TSAD/TSD/`TxPoll`（写 0x60：datasheet 的 bit5 与 QEMU 的 bit6 都置，bit7 保持 0 以保留正常优先级）；`recv()` 遇到坏帧/超长按 **丢弃而非截断** 处理并照样推进 `CAPR`；`caps()` 返回 0，**不声明任何 checksum offload / SG / MRG**（本树有意不开，见 `driver_class.h:145-146`）；`link_up` 留空即始终 up，不读 MediaStatus（8139/8139B/8139C 极性不一致，报错 down 会静默把 netif 下线）；`/proc/a20/perf` 的 `rtl8139_irq_calls`/`_rx`/`_tx`/`rtl8139_tx_reclaimed`/`rtl8139_rx_drained` 分开计数（`_rx`/`_tx` 是 `_calls` 的子集，不相加）。门禁：`make smoke-net-rtl8139`（x86_64 QEMU q35 + `-device rtl8139` + hostfwd 端到端回环，断言 `mode=irq` 且中断与两个收/发退休计数 > 0）**已实跑 PASS**（日志 `.kernel-build/smoke/net-rtl8139-x86_64.log`，`mode=irq` 且中断与两个收/发退休计数 > 0）。寄存器图与 ring 几何取自仓库内置的 QEMU 设备模型（`qemu-10.0.13+ds/hw/net/rtl8139.c`），**未在真实硅片上跑过**；同一寄存器在 RTL8139C datasheet 上记为「8K + 16K」，几何是否一致未验证。已知限制：单静态实例、无 WOL/省电/速率报告、无多队列与接收合并、无吞吐结论；`a20.rtl8139.poll=1` 的强制轮询分支本身未被该门禁覆盖 |
| VMSVGA/SVGAv3 | DISPLAY | VirtualBox x86_64/ARM，BAR offset/pitch 边界和 class registry，已加 remove；静态单实例 |
| xHCI HID | INPUT | PCI class `0x0c0330` 匹配，probe 声明 64-bit DMA mask 并校验 controller/endpoint 块（DCBAA、scratchpad、context、ring 全在其中）落在窗口内，否则 probe 失败；每 controller 动态分配实例，USB core 最多登记四个 HCD；QEMU/VirtualBox keyboard/mouse/tablet 走 class 路径。**完成路径已由全局 poll 改为 per-controller IRQ**：probe 用 `pci_intx_irq()` + `request_irq(IRQF_SHARED)` 申领 INTx，`xhci_op_start()` 在 `USBCMD.RUN` 置起且 `USBSTS` 非 halted 之后才写 `INTR0.IE\|IP`；handler 取事件环、写 `ERDP`（置 EHB）、按批跑 completion 回调，最后 `xhci_ack_event_irq()` 清 `INTR0.IP`。轮询回退保留：无 INTx 或 `request_irq()` 失败自动回退，`a20.xhci.poll=1` 强制回退，两侧计数由 process-context poll 以 `[XHCI] completions: irq=N poll=M` 打印（handler 内不打日志）。**`xhci->lock` 现已真正获取**：同步传输由 `xfer_busy` 所有权标志串行化（有界自旋，超时返回 `-EBUSY`），completion 回调在锁外成批执行，IRQ handler 在 `xfer_busy` 置位期间只 ack 不消费 ring（否则会抢走等待者要匹配的 TRB，并让电平触发 INTx 在有界忙等期间反复重入）；class 侧单向顺序 `h->lock -> xhci->lock`（HID）、`st->lock -> xhci->lock`（storage），HCD 不在持锁时回调 class。**剩余边界（因此本行仍是"有条件"而非"符合"）**：门禁 `smoke-usb-x86_64` 只跑 `-smp 1`，SMP/多核压力未验证；同 controller 上的并发同步传输靠 `xfer_busy` 拒绝而非排队；class remove 与已排队 URB 的 completion 之间仍有窗口（清 `urb.complete` 只缩小、不消除该窗口），HID 与 hub 的 status-change URB 同理 |
| USB Storage (BOT) | BLOCK | 共享实现位于 `kernel/drivers/usb/class/usb_storage.c`；generic 由 `usb-storage.a20drv` 包装，embedded 静态链接。xHCI bulk + BOT（CBW/CSW）+ SCSI READ(10)/WRITE(10)/READ_CAPACITY(10) 已在 QEMU x86_64 验证，动态发布 `/dev/diskN`。只支持单 LUN、512/2048/4096 扇区、每命令 4 KiB 数据块；`msc_command()` 当前跨同步 bulk wait 持有 spinlock，不符合新代码的锁规范 |
| TPM 2.0 (TIS) | x86 安全 | `tpm.a20drv` 模块：ACPI TPM2 表发现 + TIS FIFO 状态机 + Startup/GetRandom；无 TPM 时 probe 优雅返回 |
| PS/2 | x86 板级服务 | drvmod 模块（`ps2.a20drv`，x86_64），初始化 + 双向量 ISR；键盘字符经 `uart_receive_char` 进控制台 |
| virtio-console (virtio-serial) | CHAR | drvmod 模块（`virtio-console.a20drv`，riscv64/x86_64/aarch64/loongarch64 构建），PCI `1af4:1043`（modern）与 `1af4:1003`（transitional），只协商 `VIRTIO_F_VERSION_1`、**不协商 MULTIPORT**，因此设备侧只有 id 0 一个端口，用 q0(rx)/q1(tx) 两个 split virtqueue 承载，不设控制队列；probe 一次性分配两块 coherent 内存（ring + 数据区同块，描述符地址全为 `dma + offsetof`）。接收**中断驱动**（INTx，`request_irq` 失败时自动退回 read/poll 排空），IRQ top-half 只 ack `INTR_STATUS` 并把 used ring 排空进字节环；发送**同步**：单描述符在飞，有界自旋等待（2 s，超时计 `tx_timeouts` 并返回 `-ETIMEDOUT`），等待期间不放实例锁。节点名 `/dev/vport0` 由 `device_set_devfs_name()` 在 probe 里指定；默认把收到的字节镜像进控制台输入（`uart_receive_char`，可用 `VPORT_IOCTL_SET_CONSOLE` 断开）。限制：**single-port**（第二个 virtio-serial 函数 probe 返回 `-EBUSY`，无多端口控制协议）、**单静态实例**、无 MSI-X（只用 PCI INTx）、无流控/终端层（raw byte stream，无 termios 语义）、每队列 8 个 4 KiB rx 缓冲与 1 个 tx 缓冲、超长写被截断到 4 KiB。门禁 `make smoke-virtio-console`（x86_64 QEMU q35 + `-device virtio-serial-pci` + `virtconsole` + chardev socket，宿主 `tools/vport_host_probe.py` 写 64 字节、来客 `vport_test` 从 `/dev/vport0` 逐字节校验并回写、宿主比对回显）**已实跑 PASS**（日志 `.kernel-build/smoke/virtio-console-x86_64.log`），QEMU 上的双向回环已验证；riscv64/aarch64/loongarch64 只做过单文件 `-fsyntax-only` 自检，运行时**未验证** |
| virtio-rng (VIRTIO_ID_RNG) | CHAR | drvmod 模块（`virtio-rng.a20drv`，riscv64/x86_64 构建），实现位于 `kernel/drivers/char/virtio_rng.c`。**双 transport**：`dev->bus == &pci_bus` 走 `pci_virtio_transport_init(dev, 4, &vt)`（PCI `1af4:1044` modern / `1af4:1005` transitional），其余走原生 virtio-mmio v2 窗口（`RES_MMIO` + `RES_IRQ`，device-id 4），probe 里 magic/version/device-id 三项在两条路径上都读，只是 PCI 传输层自己应答这三个偏移。只协商 `VIRTIO_F_VERSION_1`，队列 0 是唯一的 request queue，8 个槽 × 256 B 与 ring 同在一块 page 对齐 coherent 分配里（描述符地址全为 `dma + offsetof`）。**按需投递**：probe 不挂缓冲；`read()` 发现字节环空才把空闲槽挂进 avail ring 并 notify，used ring 由 ISR（或 read/poll 回退路径）排空进 4 KiB 字节环。之所以不学 Linux 那样常驻挂满：QEMU 的 `virtio_rng_process()` 按 `virtqueue_get_avail_bytes()` 向宿主索取恰好 avail ring 宣告的字节数并填满后停手，常驻挂满等于每次 probe 都在无人读取时持续抽宿主熵。字节环空时 `read()` 发 notify 后**有界**自旋（500 ms，`proc_yield()` 逐轮排空），超时返回 `-ETIMEDOUT` 并计 `read_timeouts`，不 park，因此设备不应答也卡不死读者；等待期间不放实例锁。节点名 `/dev/hwrng` 由 `device_set_devfs_name()` 在 probe 里指定（模块侧 `drv_device_set_devfs_name`，已在符号白名单）。**熵池接线**：内核实有熵池（`kernel/core/random.c` 的 xoshiro `random_reseed()`，消费方是 `sys_getrandom()`、ASLR 与栈保护），本驱动把每次读到的字节按 64 位一组折进去（单次读上限 64 组），为此在 drvmod 白名单里新导出 `random_reseed`；**只走 read 路径，不在 ISR 里喂**（`arch_entropy_sample()` 会取 `proc_current()`）。限制：**单静态实例**（第二个 virtio-rng 函数 probe 返回 `-EBUSY`）、无 MSI-X（只用 PCI INTx / virtio-mmio 共享线）、**熵池只在有读者时才被喂**（不打开 `/dev/hwrng` 的系统完全不会去动这块设备）、字节环满时静默丢弃新到的熵、read 返回的字节数不保证是 256 的整数倍（随设备一次填多少而变）。门禁 `make smoke-virtio-rng`（x86_64 QEMU q35 + `virtio-rng-pci`，`hwrng_test` 读 ≥256 B，断言非全 `0x00`、非全 `0xff`、非单字节重复）**已实跑 PASS**（日志 `.kernel-build/smoke/virtio-rng-x86_64.log`，`HWRNG_TEST: PASS bytes=256`），QEMU 上的实际读出已验证；riscv64 virtio-mmio 路径只做过单文件 `gcc -fsyntax-only`（DRVMOD_CFLAGS）自检，运行时**未验证** |
| PC Speaker | AUDIO | drvmod 模块（`pc-spkr.a20drv`，x86_64），动态 `/dev/audioN`；支持 19 Hz–20 kHz 有界 tone/stop ABI，不冒充 PCM |
| CMOS RTC (MC146818) | 无类（x86 墙钟）| drvmod 模块（`cmos-rtc.a20drv`，x86_64 Early），绑定板级 `cmos-rtc` platform 设备（IOPORT 0x70/0x71）；BCD/二进制两种计数格式、等待 UIP 清除后再读、成功时经 `timekeeping_wallclock_set_hw()` 替换编译期墙钟种子。限制：**只读**（不写任何 CMOS 寄存器，因此没有 RTC_SET_TIME 回写路径）、**无 IRQ**（只在绑定时采样一次，不跟踪秒中断）、**不做时区换算**（CMOS 里是什么就是什么，`-rtc base=utc` 的门禁才使宿主与来客时钟可比）、**century 字节不可信**（读 CMOS 0x32（PC century 字节，非寄存器 C/D）；0x00/0xff 或落在 19xx/20xx 之外时按 20xx 处理并打日志，固件把 84 写成 1984 而 0x32 留 0x00 的机器会被算成 2084）。统一模型里目前没有 RTC class ops（`driver_class.h` 只有 char/block/net/input/display/audio），因此它以 `DEV_CLASS_NONE` 绑定，使用者通过 `timekeeping_get_realtime()`（`/dev/rtc`、`/proc/stat` 的 btime）取时间 |
| Intel HDA | AUDIO | 架构无关 PCI class 驱动；probe 按 GCAP[0] 声明 32/64-bit DMA mask，BDL/PCM 在窗口内分配，越窗仍报 `-EOPNOTSUPP`；x86_64 与 LoongArch64 QEMU 通过 BDL DMA smoke，x86_64 用户态 tone 到 QEMU WAV 验证，RISC-V64 曾在 from-source Wayland/FFmpeg/PulseAudio 路径完成播放（该路径已退役）；`make run-world-gui PKG_WORLD=xfce` 连接宿主音频；支持 48 kHz 双声道 S16_LE、环形 DMA、stop/drain、完整 remove 和用户态 WAV/raw/tone 播放器 |
| STM32 SDIO | BLOCK | 统一类 + MCU bridge；板级 bus 仍用名称匹配 |
| STM32 简单外设 | 允许例外 | 板级轮询轻量 API，不强制统一对象；扩展到多实例/用户 ABI 时必须迁移 |
| StarFive/LS2K GMAC、DW SDIO | 有条件 | GMAC 仍是 per-instance 私有锁 + 锁内轮询，多实例池按 MMIO 基址分配，数据面未接 IRQ（VF2 GMAC 线号未核实，LS2K1000 缺 PIC 路由），要求 embedded 部署，真机未复现。**DW SDIO 数据面已按板级 IRQ 资源接中断**：`platform_device_irq()` 返回线号则 `request_irq` + 有界 pre-poll + park on `wait_queue_t`，无 IRQ 资源或内核参数 `a20.dw-sdio.poll=1` 则纯轮询（向后兼容，旧路径未变）；IRQ 模式一旦超时则释放线号、屏蔽 INTMASK 并永久降级为轮询，不会因线号猜错而卡死卡。卡枚举（`dw_sdio_init_dev`）仍全程轮询，因为 probe 跑在 `kernel_main`/kthread 里不能 park。限制：QEMU 没有 dw-mshc 设备模型，**中断路径未在真机验证**；树内没有 DTB 携带的 DW SDIO 线号，fallback 平台表故意只发布 MMIO（真机正常启动时由 riscv64 FDT 枚举器从 `interrupts` 生成 `RES_IRQ`） |

## VirtualBox 平台

VirtualBox ARM64 源码位于 `kernel/platform/virtualbox-aarch64/`，通过 UEFI ACPI RSDP/MCFG 枚举 PCI。VirtIO-SCSI、E1000、SVGAv3、xHCI HID 已进入统一类模型。平台 GIC disable 已实现；generic timer trap 和 PCI interrupt routing 仍使数据面主要轮询。运行配置见 [VirtualBox ARM64 运行手册](../../platforms/virtualbox-aarch64.md) 与 [VirtualBox x86_64 运行手册](../../platforms/virtualbox-x86_64.md)，驱动架构见 [VirtualBox 驱动栈](../../platforms/virtualbox.md)。

VirtualBox x86_64 复用 x86_64 平台 PCI、AHCI、VMSVGA、PS/2/E1000/VirtIO 驱动，以 GRUB ISO 启动。

## 跨架构 PCI 协议验证

NVMe/HDA 源码不含 CPU 架构门禁，它们的运行条件来自下层 PCI/MMIO/DMA 能力。`make smoke-pci-portability` 在 QEMU LoongArch64 virt 上联合挂载 `intel-hda`、`hda-duplex` 和 NVMe，验证零值 BAR sizing/assignment、HDA codec topology、PCM BDL DMA，以及 NVMe queue/Identify 和 17 个扇区的写入、flush、读回比较。该结果证明协议驱动并非 x86 专用，但不代表所有已构建架构都具备 PCI host。

QEMU RISC-V64 board 已提供 ECAM、PCI MMIO BAR 窗口和实际 HDA PCM DMA；QEMU AArch64 board 当前仍只枚举 VirtIO-MMIO。VirtualBox AArch64 有 ACPI MCFG PCI 枚举，但 BAR 依赖固件预分配且目前没有 HDA/NVMe 运行日志。状态表必须继续按“构建”“绑定”“实际 I/O”三个等级记录证据。

## 强制边界

- 从 `kernel_main` 直接调用硬件 init，而不是 `DRIVER_REGISTER` 加枚举。
- 只暴露 `*_get_dev()` 全局 getter，不实现 class ops。
- 设备发现和功能发布使用 class；块层适配对象不能代替驱动注册。
- probe 获得 DMA/IRQ 后直接 `return -1` 而不回滚。
- 空 remove，即使驱动启用了 DMA、IRQ 或类级全局 registry。
- 在 spinlock 下执行秒级 busy poll。
- 在具体驱动中添加静态 devfs vnode，而不是通用类适配器。

新改动不得扩大表中边界。依赖轮询、固定实例数或固定节点时，提交必须写明适用平台、并发假设和失败行为。

已清理的兼容债务：`kernel_main` 不再直调 VirtIO GPU/input 初始化；block mount 和 network init 不再启动架构私有 VirtIO PCI 扫描。QEMU x86_64 现在只有统一 PCI bus 拥有 BAR 与 transport，VirtIO-MMIO 通过其 bus device ID 匹配 virtio-blk。不得恢复这些双初始化入口。

> 注意：状态矩阵描述的是当前代码事实，不是未来计划。任何“扩大限制”或“降低状态”的改动都要同步更新平台文档和提交清单。

## 历史基线观察（不代表当前回归状态）

以下两个问题曾在驱动部署重构之前的已提交基线（`3bfe64b`）上复现。它们未在 2026-08 核实时重新验证，因此不得据此声称当前源码仍失败或已经修复：

- x86_64 用户态 pid=3 崩溃：generic x86_64 启动可挂载 `/bin`、驱动全部绑定，但 mksh（pid 3）启动期在 `free_vma_pages → frame_put` 触发 `KERNEL PAGE FAULT`（`BADV=0x7fffff9xxxxx`，确定性复现），随后锁自旋。在 `3bfe64b` 干净基线（无任何驱动部署改动）上用同一 fat32 镜像复现相同故障类（`pid=3` + `0x7fffff9xxxxx`），证明为既有 mm/exec 问题；驱动重构只是让 x86 首次能到达用户态而暴露它。修复方向在 mm/vma 释放路径，不在驱动层。
- riscv64 `mm_stress` 在 evict 子测试挂起：`smoke-mm-stress` 停在 `MM_STRESS: evict start`（9 MiB 文件写回/读回压力，45 s watchdog 超时）。同一 fat32 镜像 + `3bfe64b` 干净基线内核复现相同挂起；`2026-08-06` 的 `mm_stress` 日志为 PASS，回归在 `3bfe64b` 及其之前的已提交改动之间，与驱动部署改动无关。

两者均为页缓存/写回或 exec 释放路径的既有缺陷，独立于 generic/embedded 驱动部署重构。修复时不得把这两个现象当作驱动部署的回归证据。
