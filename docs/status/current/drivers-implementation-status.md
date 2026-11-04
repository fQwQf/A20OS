# 驱动实现符合性与限制

> 仍未完成的事项见 [1.0 缺口台账](../gaps/README.md)；本页只描述当前状态。

> 驱动实现遵循单一初始化入口规范；状态变更需同步更新相关文档与约束说明；“符合”仅指在声明支持的目标配置下验证通过。

审查覆盖 driver core、公共总线、STM32F103、VirtualBox ARM64/x86_64 及其设备驱动。

## 用户 MMIO 的失败契约

`udriver_map_mmio()` 对页取整后的完整范围授权，仅在全部页成功后发布 VMA。逐页映射失败会撤销已安装 PTE、回收失败遍历留下的空中间页表、完成 TLB 事务并释放临时 VMA。`tools/tests/test_udriver_mmio_rollback_host.py` 运行生产 mapper/unmap，在跨两级页表边界时注入分配失败，保留邻接映射，并以移除回滚/空表清理为负控。 RV64 guest 的 Native MMIO 用例另覆盖正常访问、release 和输出拷贝失败清理。这不是 guest 全局 OOM 或实板 DMA 故障注入结论。锁序见[锁顺序契约](../../drivers/guide/lock-order.md)。

## 如何阅读矩阵

每行矩阵的格式是：范围、状态、说明。

- **符合**：源码和文档一致，当前可以依赖。
- **已扩展/基础可用**：已实现，但有明确限制（限制写在说明栏）。
- **有条件**：只在特定平台或特定序列化假设下工作，SMP/IRQ 化前要先加锁。
- **动态类视图**：char/block/audio 自动发布 devfs 节点，所有 class 发布 sysfs 目录；display/input 的旧聚合节点仍保留。

## 如何更新矩阵

1. 先在目标平台或 QEMU 或 VirtualBox 上复现实验，记录 commit、构建命令和日志。
2. 如果是新增行，说明里写清平台、版本、观察到的运行边界和失败行为。
3. 如果改动了现有项的状态或限制，必须同步更新相关平台文档和 [构建、测试与提交](../../drivers/meta/testing-and-submission.md) 中的测试矩阵。
4. 状态变更必须同步补充说明，明确平台边界与行为约束。

from-source GUI 栈与 LVGL 原生桌面已退役（归档在分支 `archive/legacy-desktop`），其 `smoke-qemu-gui-*` 冒烟目标与 `tools/smoke_qemu_gui.py` 一并移除。当前 GUI 桌面由 Alpine `xfce` world 提供，验证走 `make run-world-gui PKG_WORLD=xfce`。门禁入口见 [testing-gates.md](../../testing-gates.md)。

## 核心与公共基础设施

| 范围 | 状态 | 说明 |
|---|---|---|
| driver core | 符合 | 动态 registry；bus ID 后可执行无副作用的 driver protocol match；操作 mutex 串行化注册、probe/remove 与遍历；数组 spinlock 不跨回调；probe 失败完整解绑；增长失败注入在 riscv64（单 hart 与 `-smp 2`）、x86_64、aarch64、loongarch64 上都实跑，driver/device/bus 三条路径均为 `-ENOMEM` → 旧条目保留 → 重试成功 → 计数与 capacity 恢复（`smoke-driver-lifecycle-{x86_64,aarch64,loongarch64,smp2}`） |
| 固件设备模型 | 已实现 | DT、ACPI/uACPI、静态 provider 共用节点/物理资源/IRQ domain/依赖接口；未知设备独立保留，失败事务回滚，probe 可延后。交接、保留区和架构边界见 [启动模型](../../dev/boot-model.md)；不代表任意板卡已完成实机验证。 |
| PCI bus | 已扩展 | ECAM、ID/subsystem match、零值未分配 BAR sizing/assignment、BAR 编号查询、modern VirtIO capabilities；已设置 `matched_id` |
| MSI-X (消息中断) | x86_64 已实现亲和性；非 x86 未实现 | 公共层 `kernel/drivers/bus/pci_msix.c`：capability 两种编码解析、条目编程与回读校验、PBA、Function Mask，以及每条目的 `target_cpu`/`vector` 记录。驱动侧 API `pci_msix_set_affinity(dev, index, cpu)` / `pci_msix_get_affinity()` / `pci_msix_set_all_affinity(cpu)`（后两个已进 drvmod 符号白名单）。**x86_64**：消息地址按目标 CPU 的 APIC ID 生成（`LAPIC_PHYS_BASE + (apic_id << 12)`，xAPIC 物理目的模式），目标 CPU 不是当前 CPU 时经 IPI（向量 `IRQ_VECTOR_MSIX_VECTOR`）在远端写 LVT，流程为 mask → 改写地址 → 远端 arm → unmask；地址回读不符返回 `-EIO` 且状态等同迁移前，远端 arm 失败则条目保持 masked 并停在新地址（不丢中断，但该 vector 不通），已成功的条目不回滚。默认目标仍是 boot CPU（`PCI_MSIX_CPU_BOOT`），与改动前逐位一致。运行时入口 `/proc/a20/irq_affinity`（读＝全部已编程条目，写＝单个十进制 CPU id 迁移全部条目）。**残留 1：非 x86 平台实现** —— riscv64/aarch64/loongarch64/ppc64le 的 GIC/EIOINTC/MPIC 尚未接 `arch_msix_message_address()`/`arch_msix_vector_setup()`，弱符号默认返回 `-EOPNOTSUPP`，`arch_irq_msix_cpu_count()` 返回 1，只有 boot CPU 合法。**残留 2：IRQ 亲和性 API 采用情况** —— 目前仅 `pci_msix_set_affinity()` 与 `/proc/a20/irq_affinity` 两个入口，无 per-function/per-queue 的细粒度接口、无 cmdline 亲和性策略、无驱动在 probe 期自行选 CPU；e1000e 的 MSI-X 现已由 `smoke-net-e1000e-msix` 证实两个条目分别承载 RX/TX 流量（`v89=1 v90=3`） |
| MSI (cap 0x05) | x86_64 可用；非 x86 显式回退 INTx | `kernel/drivers/bus/pci_msi.c`：解析 Message Control（MMC/64-bit/per-vector mask）、写 32/64 位 message address 与 data、回读校验，消息地址复用 x86_64 的 LAPIC MSI-X hook（`arch_msix_message_address`/`arch_msix_vector_setup`，弱符号在其它架构返回 `-EOPNOTSUPP`）。只编程单条消息（MME=0）。**采用者与降级链**：AHCI（无 MSI-X）MSI → INTx → 轮询，门禁 `smoke-ahci-ich9` 实跑 PASS（`[MSI] pci-8086:2922-4: enabled vector 89`、`completion=irq(msi)`、17 次中断完成、0 轮询）；e1000e MSI-X → MSI → INTx → 轮询（MSI 只在 MSI-X 表无法武装或注册失败时使用）；82540EM 无能力 → INTx；xHCI/NVMe/rtl8139 仍只用 INTx 或轮询。非 x86 没有消息中断实现，所有 PCI 驱动按上述每设备链回退到 INTx/轮询 |
| VirtIO-MMIO bus | 符合 | MMIO/IRQ 资源、type match、`matched_id`；静态最多 8 个设备 |
| platform bus | 已扩展 | `resource_t` 一直有 `RES_IRQ`，riscv64 FDT 枚举路径也已从 `interrupts` 生成 `RES_IRQ`；本次补上驱动侧唯一入口 `platform_device_irq(dev)`，语义为 `>= 0` 可用线号 / `-ENODEV` 板级未发布 IRQ 资源（调用方轮询，**不是错误**）/ `-EINVAL` 非 platform 设备或 `end != start`（只接受单点线号）/ `-ERANGE` 超出固定 256 线表。向后兼容是完整的：不带 `RES_IRQ` 的设备读出 `-ENODEV`，未使用该函数的驱动行为不变 |
| hwapi MMIO | 符合 | 宽度访问和 barrier；relaxed 版本需谨慎 |
| hwapi IRQ | 已扩展 | 固定 256 线；`IRQF_SHARED` 提供单链表 handler chain（首个注册必须是共享标志，后续共享 handler 追加）；非共享重复注册返回 `-EBUSY`；`free_irq` 校验 owner token 并等待在途 handler |
| hwapi DMA | 基础可用 | coherent helper 返回物理 handle，sync 委托 arch cache hook；`device_t.dma_mask`（默认 64 位全通）+ `dma_set_mask()`/`dma_addr_ok()`/`dma_range_ok()`，分配器按 mask 校验：kmalloc 块落在窗口外时改从帧分配器取物理连续页，aligned 版本有界重试，不满足返回 `NULL` 并让 probe 失败，无 bounce 路径也不截断 handle；HDA 按 GCAP[0] 声明窗口，xHCI/E1000 声明 64-bit；限制：**内核驱动**仍无 IOMMU/IOVA 与 bounce（用户态 PCI 驱动走下面的 RISC-V IOMMU 域），帧分配器无分区接口（32 位 mask 在 RAM 全高于 4 GiB 的机器上会 probe 失败），`dma_alloc_coherent_aligned` 的对齐上限仍是 `PAGE_SIZE` |
| RISC-V IOMMU（用户驱动隔离） | 已扩展 | `riscv_iommu.c`：DDT(1LVL)/CQ/FQ 初始化、TR_REQ 映射/拒绝探测、per-device 动态 domain（最多 8 个，`(devid,pid)` 拥有）、map/unmap + IOTINVAL/IOFENCE、fault queue 按记录 devid 归属并 fail-closed 阻断、`/proc/a20/iommu` 计数。**每个 domain 分配唯一 TA.PSCID**：IOTLB 键是 `(tag,GSCID,PSCID,IOVA)`，共用 PSCID=0 会让两个设备对同一 IOVA 互命中对方的物理页（多设备门禁实测过）。门禁：`smoke-iommu-discovery`、`smoke-iommu-udriver-isolation`、`smoke-iommu-multidevice` **均已实跑 PASS**（双 edu：两个 domain，dev0 fault 后 dev1 仍能 DMA，随后各自 release）；仅覆盖 QEMU `riscv-iommu-pci` |
| class publication | 已扩展 | probe 成功自动建立带引用的 class device；remove 前先下线并排空在途调用 |
| devfs/sysfs | 动态类视图 | char/block/audio 自动生成 `/dev/charN`、`diskN`、`audioN`；所有 class 动态暴露于 `/sys/class`；旧 display/input 聚合节点暂时保留 |
| drvmod loader | 基础可用 | 四架构（riscv64/x86_64/aarch64/loongarch64）ELF `ET_REL`、`.a20drv` 描述符校验（唯一元数据，覆盖 RTC..USB 全部类型）、框架符号白名单、全量 ELF 边界校验（section/symbol/strtab/relocation）、按 `e_machine` 选择重定位解码器（RISC-V CALL、x86-64 large 模型 ABS64/`.lbss`、AArch64 CALL26+veneer、LoongArch CALL36/pcalau12i）；模块与内建共用同一 `driver_t`（经 `drv_driver_register` 注册，核心 `device_t` 匹配/probe），不再有第二套 `drv_driver_t`/`drv_device_t`；统一驱动管理器（`driver_manager.c`）扫描 DriverStore、按描述符路由内核模块与用户服务，已 pinned 模块不会被重复执行 DriverEntry；drv_env 双驻留共享代码的模块后端（DRV_ENV_DRVMOD）；goldfish RTC、CMOS RTC（x86_64 Early DriverStore）、PC speaker、virtio-input 内核探针、virtio-input 完整驱动、PS/2 键鼠控制器、NVMe、TPM 2.0、Intel HDA、virtio-blk、virtio-scsi、AHCI、DW SDIO、E1000、RTL8139、VMSVGA、virtio-net、virtio-gpu、virtio-snd、virtio-console、virtio-rng、xHCI、USB HID、USB storage 均已迁移为模块（generic 无内建设备驱动）；PCI 类驱动访问器、DMA 增强（aligned/sync）与地址 mask（`dma_set_mask`/`dma_addr_ok`/`dma_range_ok`）、调度/等待原语、`clock_ticks_per_sec`、内核熵池 `random_reseed`（供硬件 RNG 模块把读到的字节折进 `kernel/core/random.c`，见下方 virtio-rng 行）与 ACPI TPM2 发现均已导出；签名、运行时 unload 尚未完成 |

## drvmod 迁移矩阵

| 驱动 | 迁移状态 | 说明 |
|---|---|---|
| goldfish RTC | 已迁移 | 内建 `goldfish_rtc_kdrv.c` 已删除；`rtc.a20drv` 在 riscv64/aarch64/loongarch64 QEMU virt 上由 `smoke-drvmod-*` 验证绑定 |
| PC speaker | 已迁移 | 内建 `pc_speaker.c` 已删除；`pc-spkr.a20drv`（x86_64）经统一驱动核心桥接注册 AUDIO 类并绑定 platform 设备 |
| CMOS RTC (MC146818) | 新增 `.a20drv` | `cmos-rtc.a20drv`（x86_64 Early）：x86_64 墙钟来源，只读、无 IRQ、无时区换算、century 寄存器不可信时按 20xx 处理；门禁 `smoke-rtc-cmos` **已实跑 PASS**（`-rtc base=utc`，宿主侧复核来客墙钟；日志 `.kernel-build/smoke/rtc-cmos-x86_64.log`，来客采用 epoch 1791231484，与编译期种子 1791230264 不同，说明读到的是硬件），`smoke-rtc-cmos-fallback`（CMOS 落在可接受窗口外时回退到编译期种子）**尚未执行过**。两块 x86_64 板级（qemu-virt-x86_64、x86_64-pc）都声明了该 platform 设备 |
| UART | 内核服务 | 启动早期串口服务，模块加载前必须可用，不是可迁移设备包。8250 已写 `IER=0x01` 并注册了 `UART0_IRQ`，但**接收实测不是中断驱动的**：没有任务读控制台时输入不被处理（前台不读终端的作业按 ^C 无效），见 [gaps/drivers.md](../gaps/drivers.md) DRIVER-08 |
| virtio-blk | 已迁移 | 根文件系统挂载依赖它；`virtio-blk.a20drv` 已嵌入 Early DriverStore（root ramfs overlay），在挂载真实根盘前加载，解除 bootstrap 循环 |
| PCI bus、virtio-mmio bus、USB core、framebuffer/gpu_core、audio_core、input_mux | 内核服务 | transport、class 聚合和设备节点服务，保持静态链接，不计入迁移账本 |
| DMA/PCI BAR/class ops 框架 API | 已导出 | `drv_dma_alloc_coherent`/`dma_alloc_coherent_aligned`/`dma_sync_*`、`pci_get_bar_resource`/`pci_enable_and_assign_bars`/`pci_intx_irq`/`pci_class_code`/`pci_bus`、block/net/input/audio/display class 操作（统一核心桥接）|
| virtio-input 内核探针 | 已迁移 | 内建 `virtio_input_kprobe.c` 已删除；`vinput-probe.a20drv` 用 drv_env 模块后端复用双驻留共享协议，`smoke-dual-input` 验证与用户态 uinputd 读到同一设备身份 |
| virtio-input 完整事件投递 | 已迁移 | `vinput.a20drv` 模块（virtq + IRQ + input class），`/dev/event0` mux 拆分至 `kernel/drivers/input/input_mux.c`，`input_mux_wake` 导出；QEMU 实测事件流 |
| AHCI、DW SDIO、virtio-scsi | 已迁移 | `ahci.a20drv`（x86_64 Early）、`dw-sdio.a20drv`（riscv64 Early）、`virtio-scsi.a20drv`（四架构 Early）经 Early DriverStore 在根盘挂载前加载，与 virtio-blk 一起解决根设备 bootstrap |
| E1000、virtio-net、virtio-gpu、vmsvga、virtio-snd、xHCI、USB HID、USB storage | `.a20drv` | 设备实现仅在 `tools/driver-modules.mk` 为目标架构列出时由 generic DriverStore 提供；其 transport、framebuffer 和 class 服务仍由内核提供 |
| virtio-console | `.a20drv` / 批量 TTY | raw `/dev/vport0` 保留 id 0、同步写契约；`cli-x86_64` 默认选择带背压的批量终端，协商 MULTIPORT 控制队列并打开固定物理端口 1，逻辑节点仍为 `/dev/vport0`。默认 16 个 DMA TX 槽，仅设备完成后复用；支持 [compact 内存预设与参数覆盖](../../drivers/classes/virtio-console.md)；支持 drain、可中断空间等待，绑定租约保护模块操作。单实例、单数据端口、无 MSI-X。x86_64 QEMU raw 双向回环、完整字节输出及背压测试通过，见 [virtio-console 规范](../../drivers/classes/virtio-console.md)。 |
| virtio-rng | 新增 `.a20drv` | `virtio-rng.a20drv`（riscv64/x86_64）：`VIRTIO_ID_RNG` 熵源 → CHAR 类 `/dev/hwrng`，读到的字节同时经 `random_reseed()` 折进 `kernel/core/random.c`（因此需要新增导出该符号）。双 transport：PCI（x86_64，`1af4:1044` modern 与 `1af4:1005` transitional）与 virtio-mmio（riscv64，device-id 4）。**单静态实例、队列 0 的 8×256 B 设备缓冲只在 read 需要熵时才挂上（QEMU 每次按 avail ring 字节数向宿主索取，挂满就等于持续抽宿主熵）、读路径 500 ms 有界等待、熵池只在 read 路径喂（ISR 不喂，`random_reseed` 经 `arch_entropy_sample()` 触及 `proc_current()`）**；门禁 `smoke-virtio-rng`（x86_64 QEMU + `virtio-rng-pci`，`hwrng_test` 读 ≥256 B 并断言非全 0x00/全 0xff/单字节重复）**已实跑 PASS**（日志 `.kernel-build/smoke/virtio-rng-x86_64.log`，`HWRNG_TEST: PASS bytes=256`） |
| RTL8139 | 新增 `.a20drv` | `rtl8139.a20drv`（x86_64）：PCI 10ec:8139 + class `0x020000`，TSAD/TSD 四描述符 TX、64 KiB RX ring、IMR/ISR 中断收发。此前该型号无任何驱动认领，`-nic user,model=rtl8139` 起不来任何 NET 设备。门禁 `smoke-net-rtl8139`（hostfwd 端到端回环 + 中断计数）**已实跑 PASS**；寄存器图取自 QEMU 设备模型，未在真实硅片上验证 |
| TPM 2.0 | 已迁移 | `tpm.a20drv`（x86_64）：`firmware_acpi_tpm2` 导出 + TIS FIFO，无设备优雅失败 |
| GMAC、SDIO | 板级 platform | StarFive/LS2K GMAC 与 DW SDIO 通过 `platform_bus` + `hardware_id` 注册绑定。板级 IRQ 资源经 `platform_device_irq()` 读取：发布了就用中断，没发布就轮询。DW SDIO 有 riscv64 generic Early 模块；两个 GMAC 当前只在 embedded 的显式静态账本中，没有 generic 包。板级 bring-up 细节（时钟、地址、IRQ、缺项）见 [物理开发板移植](../../platforms/physical-boards.md) |

迁移细节与顺序见 [kernel-modules.md](../../drivers/guide/kernel-modules.md)。

## 设备驱动

| 驱动 | 类 | 状态与限制 |
|---|---|---|
| virtio-blk | BLOCK | 类接口；**完成路径默认中断驱动**，三级阶梯：每队列一个 MSI-X 向量 → 共享 INTx → 轮询（无 cmdline 开关，阶梯由 `request_irq()` 是否成功决定）。每级只在前一级失败时进入，MSI-X 部分注册成功时把已注册的那些 `free_irq()` 掉再退回 INTx。**`/proc/a20/perf` 尚未把中断与轮询两条路径分开计数**（现有 `virtio_blk_*` 计数器是 polls/completions/dma，不区分驱动方），因此从外部无法判断中断路径是否真的在工作 —— 这是与 AHCI、E1000 不同的一处缺口。remove 停止设备并释放已注册 IRQ；有限静态实例 |
| virtio-net | NET | 多实例、IRQ/轮询和类接口；remove 释放 IRQ 并复位 transport |
| virtio-input | INPUT | 类接口、PCI/MMIO、多实例槽和 remove；由 `/dev/event0` 聚合 |
| virtio-gpu | DISPLAY | class registry、framebuffer 页释放和 transport reset；单实例、同步 controlq；VIRGL 3D feature 协商、capset 查询与 `CTX_CREATE`/`RESOURCE_CREATE_3D`/`SUBMIT_3D` 透传（经 `DRM_IOCTL_VIRTGPU_*` UAPI，见 [3D 图形加速栈](../../graphics/3d-graphics.md)）；2D-only 设备自动回退 |
| VirtIO-SCSI | BLOCK | VirtualBox ARM 已验证，remove 复位/释放槽；只支持 target/LUN 0、READ/WRITE(10)、SYNCHRONIZE CACHE(10)、512B sector、每控制器单条 in-flight 命令。command queue 完成走 used-ring 中断：probe 按 MSI-X → 共享 INTx → 轮询顺序选路（`a20.virtio-scsi.poll=1` 强制轮询，注册失败也回退轮询），IRQ top-half 只 ack 并唤醒 park 中的提交者。混合完成窗口仍在（先自旋 `VIRTIO_SCSI_HYBRID_PRE_POLL_US`，再 park 50 ms 一段），因此短命令多数在自旋窗口内完成、被中断唤醒的比例未量化。x86_64 QEMU（virtio-scsi-pci + scsi-hd）由 `make smoke-virtio-scsi-irq` 验证读写/flush 回环与 `irq_count > 0`；其余架构的中断投递**未验证**，VirtualBox ARM 那条路径仍以轮询为主 |
| AHCI | BLOCK | VirtualBox x86_64；单 controller/单 port/单 slot、LBA48；probe 回滚和 remove 释放 DMA/IRQ。完成路径默认中断驱动，**优先 MSI（cap 0x05）**：`ahci_claim_irq()` 先申请一个 vector 并 `pci_msi_enable()`，失败则回退调用方给的 INTx，再失败才轮询（AHCI 没有 MSI-X 能力；非 x86 的 arch message hook 返回 `-EOPNOTSUPP`，自动走 INTx）。`PxIE` 与 `GHC.IE` 只在 `request_irq()` 成功后才打开，`a20.ahci.poll=1` 强制纯轮询；`/proc/a20/perf` 的 `ahci_irq_completions`/`ahci_poll_completions` 把两条路径分开计数，否则它们从外部同样表现为"盘能用"。门禁：`make smoke-ahci-ich9`（x86_64 QEMU q35 + `-device ich9-ahci` + `ide-hd`，ext4 挂 /extra，断言 `block_flushes` 增长且中断完成计数 > 0）**已实跑 PASS**（日志 `.kernel-build/smoke/ahci-ich9-x86_64.log`，`FSYNC_TEST: PASS` 且中断完成计数 > 0），因此 QEMU 上中断路径已验证（ready 行现在为 `completion=irq(msi)`，门禁同时断言 `[MSI] ... enabled vector`）。无 MSI-X 路径；真实 SATA PHY 与未中断硬件未验证 |
| NVMe | BLOCK | 架构无关 PCI class 驱动；x86_64 与 LoongArch64 构建，LoongArch QEMU 已验证 BAR、CAP、admin/I/O queue、Identify，以及跨 8 KiB bounce chunk 的写入/flush/读回比较；要求 NVM command set 和兼容 4 KiB memory page，首个活动 namespace、轮询、每 controller 只发布一个 namespace |
| E1000 | NET | VirtualBox/QEMU 82540EM 与 82574L(e1000e)，**完成路径默认中断驱动**（82574L：MSI-X → 单消息 MSI → INTx；82540EM 无 MSI/MSI-X 能力，直接 INTx；`request_irq()` 成功后才打开 `IMS`，无向量时保持纯轮询），已加 stop/remove；静态单实例；probe 按 RDBAH/TDBAH 的 64 位拆分寄存器声明 64-bit DMA mask，ring 块越窗时 probe 失败（`-EOPNOTSUPP`）；`a20.e1000.poll=1` 强制纯轮询；RX 由 ISR 一次性排空（复用 `a20_lwip_process_netif_irq_locked()`，**无 NAPI budget**），TX 靠 `tx_done` 镜像 TDH 在发送路径与中断路径释放描述符（旧代码清 DD 后从不重置，ring 首绕之后即永久失败）；`/proc/a20/perf` 的 `e1000_irq_calls`/`_rx`/`_tx`/`_empty`/`e1000_tx_reclaimed`/`e1000_rx_drained` 分开计数（`_rx`/`_tx` 是 `_calls` 的子集，不相加）。**e1000e MSI-X 数据面**：82574L 在 MSI-X 模式下用 `RXQ0`/`TXQ0`/`OTHER` 报告收发与链路事件，不是传统的 `RXT0`/`TXDW`；probe 现在写 `IVAR`（RXQ0→vector0，TXQ0→vector1，OTHER→vector0）并把 `IMS` 切到队列位，否则表虽武装但每个队列中断都会被设备当作 invalid vector 丢弃，而协议栈的 poll 钩子会掩盖它。按向量计数 `irq_vec[0/1]` 在进程上下文打印一次 `MSI-X vectors carried traffic: vN=… vM=…`；ITR 的 82574L 判断原先错误地拿 `pci_device_id()`（vendor<<16|device）与 device 号比较，现已修正（实测 `itr=3072us`）。门禁：`make smoke-net-e1000-irq`（82540EM + hostfwd）与 `make smoke-net-e1000e-msix`（q35 + `-device e1000e`，断言 IVAR 行、两个向量都 >0、`e1000_irq_rx/_tx/_tx_reclaimed/_rx_drained` 均增长、自然关机）**均已实跑 PASS**（日志 `.kernel-build/smoke/net-e1000e-msix-x86_64.log`：`v89=1 v90=3`）；`a20.e1000.poll=1` 的强制轮询分支本身未被门禁覆盖；单静态实例仍是已知限制 |
| RTL8139 | NET | Realtek 10ec:8139，PCI class `0x020000` 匹配；x86_64 QEMU（`-device rtl8139`），此前该型号无任何驱动认领、`DEV_CLASS_NET` 里根本没有网卡。**默认中断驱动**（PCI INTx；`request_irq()` 成功且 handler 就位后才写 `IMR`，失败或 `a20.rtl8139.poll=1` 则纯轮询）；静态单实例；RX 64 KiB ring（`RCR[12:11]=0b11`）+ 4×2048 TX 描述符，同一块 kmalloc、按 2048 对齐；`send()` 先线性拷贝进 bounce buffer 再写 TSAD/TSD/`TxPoll`（写 0x60：datasheet 的 bit5 与 QEMU 的 bit6 都置，bit7 保持 0 以保留正常优先级）；`recv()` 遇到坏帧/超长按 **丢弃而非截断** 处理并照样推进 `CAPR`；`caps()` 返回 0，**不声明任何 checksum offload / SG / MRG**（本树有意不开，见 `driver_class.h:145-146`）；`link_up` 留空即始终 up，不读 MediaStatus（8139/8139B/8139C 极性不一致，报错 down 会静默把 netif 下线）；`/proc/a20/perf` 的 `rtl8139_irq_calls`/`_rx`/`_tx`/`rtl8139_tx_reclaimed`/`rtl8139_rx_drained` 分开计数（`_rx`/`_tx` 是 `_calls` 的子集，不相加）。门禁：`make smoke-net-rtl8139`（x86_64 QEMU q35 + `-device rtl8139` + hostfwd 端到端回环，断言 `mode=irq` 且中断与两个收/发退休计数 > 0）**已实跑 PASS**（日志 `.kernel-build/smoke/net-rtl8139-x86_64.log`，`mode=irq` 且中断与两个收/发退休计数 > 0）。寄存器图与 ring 几何取自外部 Debian QEMU 10.0.13+ds 源树（`qemu-10.0.13+ds/hw/net/rtl8139.c`；见[外部 QEMU 源码定位](../../dev/external-dependencies.md)），**未在真实硅片上跑过**；同一寄存器在 RTL8139C datasheet 上记为「8K + 16K」，几何是否一致未验证。已知限制：单静态实例、无 WOL/省电/速率报告、无多队列与接收合并、无吞吐结论；`a20.rtl8139.poll=1` 的强制轮询分支本身未被该门禁覆盖 |
| VMSVGA/SVGAv3 | DISPLAY | VirtualBox x86_64/ARM，BAR offset/pitch 边界和 class registry，已加 remove；静态单实例 |
| xHCI HID | INPUT | PCI class `0x0c0330` 匹配，probe 声明 64-bit DMA mask 并校验 controller/endpoint 块（DCBAA、scratchpad、context、ring 全在其中）落在窗口内，否则 probe 失败；每 controller 动态分配实例，USB core 最多登记四个 HCD；QEMU/VirtualBox keyboard/mouse/tablet 走 class 路径。**完成路径已由全局 poll 改为 per-controller IRQ**：probe 用 `pci_intx_irq()` + `request_irq(IRQF_SHARED)` 申领 INTx，`xhci_op_start()` 在 `USBCMD.RUN` 置起且 `USBSTS` 非 halted 之后才写 `INTR0.IE\|IP`；handler 取事件环、写 `ERDP`（置 EHB）、按批跑 completion 回调，最后 `xhci_ack_event_irq()` 清 `INTR0.IP`。轮询回退保留：无 INTx 或 `request_irq()` 失败自动回退，`a20.xhci.poll=1` 强制回退，两侧计数由 process-context poll 以 `[XHCI] completions: irq=N poll=M` 打印（handler 内不打日志）。**`xhci->lock` 现已真正获取**：同步传输由 `xfer_busy` 所有权标志串行化（有界自旋，超时返回 `-EBUSY`），completion 回调在锁外成批执行，IRQ handler 在 `xfer_busy` 置位期间只 ack 不消费 ring（否则会抢走等待者要匹配的 TRB，并让电平触发 INTx 在有界忙等期间反复重入）；class 侧单向顺序 `h->lock -> xhci->lock`（HID）、`st->lock -> xhci->lock`（storage），HCD 不在持锁时回调 class。**剩余边界（因此本行仍是"有条件"而非"符合"）**：门禁 `smoke-usb-x86_64` 只跑 `-smp 1`，SMP/多核压力未验证；同 controller 上的并发同步传输靠 `xfer_busy` 拒绝而非排队；class remove 与已排队 URB 的 completion 之间仍有窗口（清 `urb.complete` 只缩小、不消除该窗口），HID 与 hub 的 status-change URB 同理。**拔出路径**：`usb_disconnect_port()` 先在 controller 侧 `abort_slot()`——取 `xfer_busy`（有界重试）→ 发 `Disable Slot` 停止该 slot 的全部端点 → 清 DCBAA 条目 → 锁外释放端点 ring；随后释放 bus address。`smoke-usb-hotplug-x86_64` 用 QMP `device_del`/`device_add` 在 qemu-xhci 根口上实跑：拔出后地址 1 与 slot 1 都归还，重插拿到同一个地址并再次完成 HID 枚举，无 `enumeration failed`/page fault，自然关机 |
| USB Storage (BOT) | BLOCK | 共享实现位于 `kernel/drivers/usb/class/usb_storage.c`；generic 由 `usb-storage.a20drv` 包装，embedded 静态链接。xHCI bulk + BOT（CBW/CSW）+ SCSI READ(10)/WRITE(10)/READ_CAPACITY(10) 已在 QEMU x86_64 验证，动态发布 `/dev/diskN`。只支持单 LUN、512/2048/4096 扇区、每命令 4 KiB 数据块；`msc_command()` 的每实例锁已由 `spin_lock_irqsave` 改为 sleepable `mutex_t`，因此跨同步 bulk wait 持有它是合规的（块层从进程上下文调用）；probe 失败路径不再对静态 `g_storage` 槽位执行 `kfree()` |
| TPM 2.0 (TIS) | x86 安全 | `tpm.a20drv` 模块：ACPI TPM2 表发现 + TIS FIFO 状态机 + Startup/GetRandom；无 TPM 时 probe 优雅返回 |
| PS/2 | x86 板级服务 | drvmod 模块（`ps2.a20drv`，x86_64），初始化 + 双向量 ISR；键盘字符经 `uart_receive_char` 进控制台 |
| virtio-console (virtio-serial) | CHAR / TTY backend | PCI `1af4:1043` / `1af4:1003`，raw 模式仅协商 VERSION_1，使用 q0/q1 与端口 0；批量 TTY 模式另协商 MULTIPORT，经 q2/q3 完成 DEVICE_READY / PORT_READY / PORT_OPEN，使用固定端口 1 的 q4/q5。RX 默认 8 × 4 KiB，TX 默认 16 × 4 KiB；[compact 及覆盖参数](../../drivers/classes/virtio-console.md) 可降低内存占用；`tx_lock` 串行化提交与退休，超时或信号不归还设备持有的 DMA 槽。raw 写同步完成，TTY 写返回已复制字节，队列满时有界等待，支持 `VPORT_IOCTL_DRAIN` / `tcdrain`。批量模式通过独立 Unix socket 与有界宿主中继输出，避免 QEMU virtconsole 的短写丢弃。单静态实例、仅一个数据端口，不支持通用多端口枚举/热插拔，无 MSI-X。x86_64 QEMU 已验证；其他架构仅语法检查，不宣称运行时验证。 |
| virtio-rng (VIRTIO_ID_RNG) | CHAR | drvmod 模块（`virtio-rng.a20drv`，riscv64/x86_64 构建），实现位于 `kernel/drivers/char/virtio_rng.c`。**双 transport**：`dev->bus == &pci_bus` 走 `pci_virtio_transport_init(dev, 4, &vt)`（PCI `1af4:1044` modern / `1af4:1005` transitional），其余走原生 virtio-mmio v2 窗口（`RES_MMIO` + `RES_IRQ`，device-id 4），probe 里 magic/version/device-id 三项在两条路径上都读，只是 PCI 传输层自己应答这三个偏移。只协商 `VIRTIO_F_VERSION_1`，队列 0 是唯一的 request queue，8 个槽 × 256 B 与 ring 同在一块 page 对齐 coherent 分配里（描述符地址全为 `dma + offsetof`）。**按需投递**：probe 不挂缓冲；`read()` 发现字节环空才把空闲槽挂进 avail ring 并 notify，used ring 由 ISR（或 read/poll 回退路径）排空进 4 KiB 字节环。之所以不学 Linux 那样常驻挂满：QEMU 的 `virtio_rng_process()` 按 `virtqueue_get_avail_bytes()` 向宿主索取恰好 avail ring 宣告的字节数并填满后停手，常驻挂满等于每次 probe 都在无人读取时持续抽宿主熵。字节环空时 `read()` 发 notify 后**有界**自旋（500 ms，`proc_yield()` 逐轮排空），超时返回 `-ETIMEDOUT` 并计 `read_timeouts`，不 park，因此设备不应答也卡不死读者；等待期间不放实例锁。节点名 `/dev/hwrng` 由 `device_set_devfs_name()` 在 probe 里指定（模块侧 `drv_device_set_devfs_name`，已在符号白名单）。**熵池接线**：内核实有熵池（`kernel/core/random.c` 的 xoshiro `random_reseed()`，消费方是 `sys_getrandom()`、ASLR 与栈保护），本驱动把每次读到的字节按 64 位一组折进去（单次读上限 64 组），为此在 drvmod 白名单里新导出 `random_reseed`；**只走 read 路径，不在 ISR 里喂**（`arch_entropy_sample()` 会取 `proc_current()`）。限制：**单静态实例**（第二个 virtio-rng 函数 probe 返回 `-EBUSY`）、无 MSI-X（只用 PCI INTx / virtio-mmio 共享线）、**熵池只在有读者时才被喂**（不打开 `/dev/hwrng` 的系统完全不会去动这块设备）、字节环满时静默丢弃新到的熵、read 返回的字节数不保证是 256 的整数倍（随设备一次填多少而变）。门禁 `make smoke-virtio-rng`（x86_64 QEMU q35 + `virtio-rng-pci`，`hwrng_test` 读 ≥256 B，断言非全 `0x00`、非全 `0xff`、非单字节重复）**已实跑 PASS**（日志 `.kernel-build/smoke/virtio-rng-x86_64.log`，`HWRNG_TEST: PASS bytes=256`），QEMU 上的实际读出已验证；riscv64 virtio-mmio 路径只做过单文件 `gcc -fsyntax-only`（DRVMOD_CFLAGS）自检，运行时**未验证** |
| PC Speaker | AUDIO | drvmod 模块（`pc-spkr.a20drv`，x86_64），动态 `/dev/audioN`；支持 19 Hz–20 kHz 有界 tone/stop ABI，不冒充 PCM |
| CMOS RTC (MC146818) | 无类（x86 墙钟）| drvmod 模块（`cmos-rtc.a20drv`，x86_64 Early），绑定板级 `cmos-rtc` platform 设备（IOPORT 0x70/0x71）；BCD/二进制两种计数格式、等待 UIP 清除后再读、成功时经 `timekeeping_wallclock_set_hw()` 替换编译期墙钟种子。限制：**只读**（不写任何 CMOS 寄存器，因此没有 RTC_SET_TIME 回写路径）、**无 IRQ**（只在绑定时采样一次，不跟踪秒中断）、**不做时区换算**（CMOS 里是什么就是什么，`-rtc base=utc` 的门禁才使宿主与来客时钟可比）、**century 字节不可信**（读 CMOS 0x32（PC century 字节，非寄存器 C/D）；0x00/0xff 或落在 19xx/20xx 之外时按 20xx 处理并打日志，固件把 84 写成 1984 而 0x32 留 0x00 的机器会被算成 2084）。统一模型里目前没有 RTC class ops（`driver_class.h` 只有 char/block/net/input/display/audio），因此它以 `DEV_CLASS_NONE` 绑定，使用者通过 `timekeeping_get_realtime()`（`/dev/rtc`、`/proc/stat` 的 btime）取时间 |
| Intel HDA | AUDIO | 架构无关 PCI class 驱动；probe 按 GCAP[0] 声明 32/64-bit DMA mask，BDL/PCM 在窗口内分配，越窗仍报 `-EOPNOTSUPP`；x86_64 与 LoongArch64 QEMU 通过 BDL DMA smoke，x86_64 用户态 tone 到 QEMU WAV 验证，RISC-V64 曾在 from-source Wayland/FFmpeg/PulseAudio 路径完成播放（该路径已退役）；`make run-world-gui PKG_WORLD=xfce` 连接宿主音频；支持 48 kHz 双声道 S16_LE、环形 DMA、stop/drain、完整 remove 和用户态 WAV/raw/tone 播放器 |
| STM32 SDIO | BLOCK | 统一类 + MCU bridge；板级 bus 仍用名称匹配 |
| STM32 简单外设 | 允许例外 | 板级轮询轻量 API，不强制统一对象；扩展到多实例/用户 ABI 时必须迁移 |
| StarFive/LS2K GMAC | 有条件 | per-instance 私有锁与轮询，要求 embedded 部署；支持范围及实机状态以各平台文档为准 |
| DW-MSHC SD | BLOCK | VF2、Quartz64 Zero 的固件节点绑定；每设备私有状态、64 位 MMIO、单线 PIO、SDSC/SDHC/SDXC、CSD 容量与写保护、固件/构建只读、有界等待和错误回收。IRQ 可选，短轮询后在调度器可用时等待事件；错误返回后卡离线，不宣称自动恢复。板级时钟与管脚在 arch，宿主生产代码测试及交叉构建通过；实机未验证 |
| sunxi MMC0 SD | BLOCK | PINE A64、V3s 的真实固件节点；400 kHz 单线 PIO、SDSC/SDHC/SDXC、非对齐缓冲区、读写保护、flush、超时与受管回收。embedded 共用实现，AArch64 提供 early 模块；CCU/reset/pinmux 在 arch。生产模型测试及交叉构建通过，实机未验证；无 MMC/eMMC、SDIO function、DMA、高速和热插拔 |
| SDHCI SD | BLOCK | Nano SG2002、Ox64 BL808 的真实固件节点；单线 PIO、SDSC/SDHC/SDXC、非对齐缓冲区、写卡完成等待、写保护/只读与电压检查，出错后停止 I/O。板级时钟/pinmux/PHY/脉冲在 arch；生产模型测试及交叉构建通过，实机未验证；无 eMMC、SDIO function、DMA、UHS 和热插拔 |
| 2K0300 LIOINTC | IRQ domain | 两组 32 路传统中断、HWI0/HWI2 级联、触发/极性、mask/ack、边沿重启和固件节点映射，配合核内定时器。生产寄存器模型测试及交叉构建通过，实机投递未验证；SDIO 强制 DMA，相关数据面尚缺 |

## VirtualBox 平台

VirtualBox ARM64 源码位于 `kernel/arch/aarch64/boards/virtualbox-aarch64/`，通过 UEFI ACPI RSDP/MCFG 枚举 PCI。VirtIO-SCSI、E1000、SVGAv3、xHCI HID 已进入统一类模型。平台 GIC disable 已实现；generic timer trap 和 PCI interrupt routing 仍使数据面主要轮询。运行配置见 [VirtualBox ARM64 运行手册](../../platforms/virtualbox-aarch64.md) 与 [VirtualBox x86_64 运行手册](../../platforms/virtualbox-x86_64.md)，驱动架构见 [VirtualBox 驱动栈](../../platforms/virtualbox.md)。

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

## 相关子系统已知边界（mm/exec）

以下问题属于页缓存/写回或 exec 释放路径的既有行为，独立于驱动核心：

- x86_64 用户态执行边界：在特定高负载或异常释放路径下，`free_vma_pages → frame_put` 涉及的页表释放由 mm 负责，相关修复集中于 mm/vma 路径。
- riscv64 `mm_stress` 边界：在极限写回/读回压力测试下，evict 阶段受页缓存回写超时约束，属于内存管理子系统的压力调度范畴。

## 当前限制

- 每个 controller 只有一个全局 playback stream；尚未提供多客户端混音或 per-open session。
- 不支持 capture、音量控制、mixer、动态采样率或除 S16_LE stereo 之外的格式。
- HDA ring 和 virtio-sound completion 目前均由 write/drain 路径轮询。HDA 尚未启用 period IRQ；若生产者停止时间超过整个 ring，硬件可能在下一次 refresh 前重放旧数据。
- QEMU RISC-V64 使用 coherent DMA 假设；移植到非一致性缓存硬件前必须实现真实的 `dma_sync_for_device()`。
