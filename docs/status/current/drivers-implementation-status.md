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
| PCI bus | 已扩展 | ECAM、ID/subsystem match、零值未分配 BAR sizing/assignment、BAR 编号查询、modern VirtIO capabilities；已设置 `matched_id`。BAR 规划在 discovery 事务内核对资源声明/占用，全部通过后提交新地址与资源；规划失败保留原 BAR/command/资源，调用者仍须先 quiesce；已持有 function 的用户查询仅读现有 BAR/IRQ 清单，不重写 BAR/BME 或资源 |
| 显式失败 probe 清理 | 基础可用 | `device_probe_use_remove` 在初始化前无分配选择既有部分实例退出管线；清理失败保留绑定/devres/依赖和原 probe_error，子设备阻塞状态拒绝新使用。device/driver 布局沿用既有模型，当前 ABI 10，旧驱动需逐一迁移；不代表实体 GPU 停止已验证 |
| USB 部分初始化清理 | 基础可用 | 预分配端口 owner，非零 alloc_slot 结果仍可交还已知 token；枚举/部分发布失败先停 slot 再解绑，错误保留地址/协议图/HCD，扫描和注销可重试，保留三次连接预算。device/driver 布局沿用既有模型，当前 ABI 10；xHCI 整机退出另有已确认停机协议；其他 HCD 和未知 slot 的单独恢复仍待实现 |
| 提前退出准备 | 基础可用 | 当前 probe 显式登记可失败 prepare，先于 consumer unbind；LIFO/逐项完成记录，错误保留 PREPARING/绑定/图/资源。xHCI 已接入失败 probe 和已确认停机后的软件 slot 清理，device/driver 布局沿用既有模型，当前 ABI 10。HID/hub 独立退出已接入可失败 endpoint 释放，storage 及额外 DMA 错误路径仍需完善；USB 拓扑已串行协调，xHCI 锁外回调已计数保留 |
| USB endpoint 交还 | 基础可用 | ABI 10 HCD release_endpoint；HID/hub 的独立退出和失败 probe 保留 URB/ring，xHCI Stop/Drop 分段读回确认，回调在途返回 EBUSY，重试与同控制器其他 endpoint 隔离；storage 和同步 bulk 错误保留仍待迁移 |
| 可失败解绑 | 基础可用 | remove 非零保留离线绑定/匹配/devres/依赖和注册引用，device/platform/driver unregister 传播错误并支持显式重试；仅执行一次 quiesce，注销期间拒绝新绑定/消费关系与递归退出。发布失败清理同样保留；PCI 槽、已发布 USB 端口/slot/address 保留已接入。C/Rust 主机失败注入与合成 QEMU 门禁通过；实际硬件停止、失败 probe、关机/kexec shutdown 仍由 provider 负责 |
| 原生异步驱动运行时 | 基础可用 | `quiesce`、页级 DMA domain/mapping、fence、可取消工作队列、IRQ 全分发排空、有界固件来源/image。失败保留与回收契约及 hosted 限制见 [原生驱动运行时](../../drivers/guide/native-runtime.md)；尚未接入实体 GPU |
| 资源占用与清单核对 | 基础可用 | owner-pinned MMIO/port claim、受管映射先 unmap 后释放占用；checked claim 核对未绑定设备声明并阻止后续冲突发布。已导入受管 RAM 与 ACPI 表页；PFA 之外的 RAM/其他固件保留区尚不完整，也不拦截 raw MMIO/PIO；主机竞争/失败注入与 QEMU 元数据自检记录见运行时文档 |
| 物理存储保留 | 基础可用 | 原生独立句柄，复制/归并区间后原子发布，阻止 MMIO claim/map、设备发布和 PCI 地址使用；`RES_MEM` 描述可共存。hosted 启动导入受管 RAM 与 ACPI 表页，失败不发布部分保护；主机竞争/失败注入、QEMU 双核 `memory=1` 已验证。不是 PFA 页分配或 DMA 隔离 |
| DMA 物理约束 | 基础可用 | 独立复制至多 1024 个物理字节规则，权限交集/identity 合并；带 policy 的 managed domain 在整批 pin/provider 前预检，IOVA 违规进入可失败回滚并保留 policy。字节模型、方向、部分页、Unity、分配失败和清理重试通过；不检查硬件 PTE，旧构造器不自动获得约束 |
| AMD-Vi IVMD policy | 受限可用 | 保留路由转换完整固件声明集合（含 sibling alias）及替代 IVHD 的匹配 IVMD，每个原始记录一次；IR/IW/Unity 转为独立约束，ExclusionRange/保留 flags 拒绝。主机交集/独立存储/失败验证，QEMU 空 policy 集成通过；硬件页表权限、Unity 映射和排除旁路尚未实现 |
| PCI requester scope | 只读观察 | `pci_device_scope_get` 返回带引用的 client/上游桥，核对核心注册状态、live 身份和唯一完整桥路径，重叠/缺桥/循环拒绝。AMD Range 辅助查询另验证 PCIe root port 与 capability 链；无效 Range 要求固件路径。QEMU root-port/EDU 桥查询与主机负向用例通过；不是 ACS/alias 隔离组或 DMA provider |
| PCI requester 集合占用 | 基础可用 | 显式复制、排序去重并原子占用最多 128 个完整 requester 键；同 owner 重叠拒绝，owner pin/占用跨注销与 BDF 替换保留。真实线程竞争/注销竞争及失败回滚通过；不推断 alias，也不拦截 raw 配置或旧 provider |
| AMD-Vi 保留路由 | 基础可用 | 显式 acquire/validate/requesters/reserve/memory/release，组合 IVRS、原 requester/alias PCI 路径、可用 Range 与绑定快照；解绑使旧句柄失效，最后释放前保留完整 aperture 和固件。主机解绑/重绑/替换/失败注入及 QEMU EDU 路由查询通过；reserve 已检查并原子占用完整固件声明集合，保留各成员身份/路径，缺失/BME/用户所有权冲突拒绝；成员消失/替换后继续占用，IVMD 已覆盖全部成员；尚缺通过各所属驱动停止全部 peer 与归属变更协调、硬件权限/Unity/Exclusion 和翻译后端 |
| AMD-Vi 发现 | 只读诊断 | IVRS 全表校验、候选路由/IVMD 约束、唯一单元枚举及 PCI capability/已启用 MMIO 的只读快照；x86 segment 0 启动报告明确 `driver_enabled=0`。发现报告本身不保留占用，DMA translation 与 fault recovery 尚未实现 |
| AMD-Vi 被动 PCI 所有权 | QEMU 已验证 | x86_64 + ACPI 内建隔离服务；普通 PCI 绑定保留 IVRS 与完整 16/512-KiB checked aperture claim，同 function 多 capability 重叠拒绝，失败/解绑先 unmap 后释放占用。主机失败注入及 QEMU 双核注销/重绑通过；无硬件写入或翻译启用，受管 RAM/ACPI 表页已参与冲突核对，仍缺其余保留来源和实际 DMA 隔离 |
| MSI-X (消息中断) | x86_64 已实现亲和性；非 x86 未实现 | 公共层 `kernel/drivers/bus/pci_msix.c`：capability 两种编码解析、条目编程与回读校验、PBA、Function Mask，以及每条目的 `target_cpu`/`vector` 记录。驱动侧 API `pci_msix_set_affinity(dev, index, cpu)` / `pci_msix_get_affinity()` / `pci_msix_set_all_affinity(cpu)`（后两个已进 drvmod 符号白名单）。**x86_64**：消息地址按目标 CPU 的 APIC ID 生成（`LAPIC_PHYS_BASE + (apic_id << 12)`，xAPIC 物理目的模式），目标 CPU 不是当前 CPU 时经 IPI（向量 `IRQ_VECTOR_MSIX_VECTOR`）在远端写 LVT，流程为 mask → 改写地址 → 远端 arm → unmask；地址回读不符返回 `-EIO` 且状态等同迁移前，远端 arm 失败则条目保持 masked 并停在新地址（不丢中断，但该 vector 不通），已成功的条目不回滚。默认目标仍是 boot CPU（`PCI_MSIX_CPU_BOOT`），与改动前逐位一致。运行时入口 `/proc/a20/irq_affinity`（读＝全部已编程条目，写＝单个十进制 CPU id 迁移全部条目）。**残留 1：非 x86 平台实现** —— riscv64/aarch64/loongarch64/ppc64le 的 GIC/EIOINTC/MPIC 尚未接 `arch_msix_message_address()`/`arch_msix_vector_setup()`，弱符号默认返回 `-EOPNOTSUPP`，`arch_irq_msix_cpu_count()` 返回 1，只有 boot CPU 合法。**残留 2：IRQ 亲和性 API 采用情况** —— 目前仅 `pci_msix_set_affinity()` 与 `/proc/a20/irq_affinity` 两个入口，无 per-function/per-queue 的细粒度接口、无 cmdline 亲和性策略、无驱动在 probe 期自行选 CPU；e1000e 的 MSI-X 现已由 `smoke-net-e1000e-msix` 证实两个条目分别承载 RX/TX 流量（`v89=1 v90=3`） |
| MSI (cap 0x05) | x86_64 可用；非 x86 显式回退 INTx | `kernel/drivers/bus/pci_msi.c`：解析 Message Control（MMC/64-bit/per-vector mask）、写 32/64 位 message address 与 data、回读校验，消息地址复用 x86_64 的 LAPIC MSI-X hook（`arch_msix_message_address`/`arch_msix_vector_setup`，弱符号在其它架构返回 `-EOPNOTSUPP`）。只编程单条消息（MME=0）。**采用者与降级链**：AHCI（无 MSI-X）MSI → INTx → 轮询，门禁 `smoke-ahci-ich9` 实跑 PASS（`[MSI] pci-8086:2922-4: enabled vector 89`、`completion=irq(msi)`、17 次中断完成、0 轮询）；e1000e MSI-X → MSI → INTx → 轮询（MSI 只在 MSI-X 表无法武装或注册失败时使用）；82540EM 无能力 → INTx；xHCI/NVMe/rtl8139 仍只用 INTx 或轮询。非 x86 没有消息中断实现，所有 PCI 驱动按上述每设备链回退到 INTx/轮询 |
| VirtIO-MMIO bus | 符合 | MMIO/IRQ 资源、type match、`matched_id`；静态最多 8 个设备 |
| platform bus | 已扩展 | `resource_t` 一直有 `RES_IRQ`，riscv64 FDT 枚举路径也已从 `interrupts` 生成 `RES_IRQ`；本次补上驱动侧唯一入口 `platform_device_irq(dev)`，语义为 `>= 0` 可用线号 / `-ENODEV` 板级未发布 IRQ 资源（调用方轮询，**不是错误**）/ `-EINVAL` 非 platform 设备或 `end != start`（只接受单点线号）/ `-ERANGE` 超出固定 256 线表。向后兼容是完整的：不带 `RES_IRQ` 的设备读出 `-ENODEV`，未使用该函数的驱动行为不变 |
| hwapi MMIO | 符合 | 宽度访问和 barrier；relaxed 版本需谨慎 |
| hwapi IRQ | 已扩展 | 固定 256 线；`IRQF_SHARED` 提供单链表 handler chain（首个注册必须是共享标志，后续共享 handler 追加）；非共享重复注册返回 `-EBUSY`；`free_irq` 校验 owner token 并等待在途 handler |
| hwapi DMA | 基础可用 | coherent helper 返回物理 handle，sync 委托 arch cache hook；`device_t.dma_mask`（默认 64 位全通）+ `dma_set_mask()`/`dma_addr_ok()`/`dma_range_ok()`，分配器按 mask 校验：kmalloc 块落在窗口外时改从帧分配器取物理连续页，aligned 版本有界重试，不满足返回 `NULL` 并让 probe 失败，无 bounce 路径也不截断 handle；HDA 按 GCAP[0] 声明窗口，xHCI/E1000 声明 64-bit；限制：这些 coherent helper 不自动接入 IOMMU/IOVA，也无 bounce（原生页映射和用户态 PCI 驱动可显式使用下面的 RISC-V IOMMU 域），帧分配器无分区接口（32 位 mask 在 RAM 全高于 4 GiB 的机器上会 probe 失败），`dma_alloc_coherent_aligned` 的对齐上限仍是 `PAGE_SIZE` |
| RISC-V IOMMU（用户驱动隔离） | 已扩展 | `riscv_iommu.c`：DDT(1LVL)/CQ/FQ 初始化、TR_REQ 映射/拒绝探测、per-device 动态 domain（最多 8 个，正 PID 接口；与原生 domain 共享 PCI requester 占用，失效失败保留）、map/unmap + IOTINVAL/IOFENCE、fault queue 按记录 devid 归属并 fail-closed 阻断、`/proc/a20/iommu` 计数。**每个 domain 分配唯一 TA.PSCID**：IOTLB 键是 `(tag,GSCID,PSCID,IOVA)`，共用 PSCID=0 会让两个设备对同一 IOVA 互命中对方的物理页（多设备门禁实测过）。门禁：`smoke-iommu-discovery`、`smoke-iommu-udriver-isolation`、`smoke-iommu-multidevice` **均已实跑 PASS**（双 edu：两个 domain，dev0 fault 后 dev1 仍能 DMA，随后各自 release）；仅覆盖 QEMU `riscv-iommu-pci` |
| 原生 RISC-V IOMMU provider | QEMU 已验证 | 将当次发布 PCI 身份接入 managed domain；负 token 与正 PID 用户 domain 共用 PCI requester 登记，DC 发布前排斥其他 claim，失效失败保留页面/domain/占用。构造/关闭要求 BME 已清除，shutdown 后占用保留至最后 put；用户新 claim 在取得占用后才清除/读回 BME。EDU 实际跨页 DMA、unmap 后 fault、worker → fence 通知已有验证；仅该 RISC-V 后端，不含 AMD-Vi 或 GPUVM |
| class publication | 已扩展 | probe 成功自动建立带引用的 class device；先 offline，再 quiesce，然后排空在途 class 调用，最后 remove/devres |
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
| E1000、virtio-net、virtio-gpu、vmsvga、virtio-snd、xHCI、USB HID、USB storage | `.a20drv` | 设备实现仅在 `tools/mk/driver-modules.mk` 为目标架构列出时由 generic DriverStore 提供；其 transport、framebuffer 和 class 服务仍由内核提供 |
| virtio-console | `.a20drv` / 批量 TTY | raw `/dev/vport0` 保留 id 0、同步写契约；`cli-x86_64` 默认选择带背压的批量终端，协商 MULTIPORT 控制队列并打开固定物理端口 1，逻辑节点仍为 `/dev/vport0`。默认 16 个 DMA TX 槽，仅设备完成后复用；支持 [compact 内存预设与参数覆盖](../../drivers/classes/virtio-console.md)；支持 drain、可中断空间等待，绑定租约保护模块操作。单实例、单数据端口、无 MSI-X。x86_64 QEMU raw 双向回环、完整字节输出及背压测试通过，见 [virtio-console 规范](../../drivers/classes/virtio-console.md)。 |
| virtio-rng | 新增 `.a20drv` | `virtio-rng.a20drv`（riscv64/x86_64）：`VIRTIO_ID_RNG` 熵源 → CHAR 类 `/dev/hwrng`，读到的字节同时经 `random_reseed()` 折进 `kernel/core/random.c`（因此需要新增导出该符号）。双 transport：PCI（x86_64，`1af4:1044` modern 与 `1af4:1005` transitional）与 virtio-mmio（riscv64，device-id 4）。**单静态实例、队列 0 的 8×256 B 设备缓冲只在 read 需要熵时才挂上（QEMU 每次按 avail ring 字节数向宿主索取，挂满就等于持续抽宿主熵）、读路径 500 ms 有界等待、熵池只在 read 路径喂（ISR 不喂，`random_reseed` 经 `arch_entropy_sample()` 触及 `proc_current()`）**；门禁 `smoke-virtio-rng`（x86_64 QEMU + `virtio-rng-pci`，`hwrng_test` 读 ≥256 B 并断言非全 0x00/全 0xff/单字节重复）**已实跑 PASS**（日志 `.kernel-build/smoke/virtio-rng-x86_64.log`，`HWRNG_TEST: PASS bytes=256`） |
| RTW8852BE | NET + 用户态服务 / 已实机 | 独立上游 rtw89 服务通过受控 PCI/DMA broker 提供 Ethernet。单无线设备/单 MM 所有者、32 MiB DMA 上限；服务异常且 DMA 未归还时隔离内存并拒绝重绑定。WHU-WLAN 连接已获用户确认，网页认证有 GUI/HTTP/HTTPS 回归；不声称其他芯片或企业 EAP 已验收。 |
| RTL8139 | 新增 `.a20drv` | `rtl8139.a20drv`（x86_64）：PCI 10ec:8139 + class `0x020000`，TSAD/TSD 四描述符 TX、64 KiB RX ring、IMR/ISR 中断收发。此前该型号无任何驱动认领，`-nic user,model=rtl8139` 起不来任何 NET 设备。门禁 `smoke-net-rtl8139`（hostfwd 端到端回环 + 中断计数）**已实跑 PASS**；寄存器图取自 QEMU 设备模型，未在真实硅片上验证 |
| RTL8168/8111 (r8169) | NET / 模型自检 | 每设备私有状态、DMA ring、IRQ/轮询，remove 超时保留资源，成功后释放。模块自动匹配10ec:8168，静态 ID 表还有8169/8161/8136/8101；这些其他硅片变体未实机验收，不据此宣称整个系列可用。QEMU无相应硬件模型，寄存器模型不代替有线网络实测。 |
| TPM 2.0 | 已迁移 | `tpm.a20drv`（x86_64）：`firmware_acpi_tpm2` 导出 + TIS FIFO，无设备优雅失败 |
| GMAC、SDIO | 板级 platform | StarFive/LS2K GMAC 与 DW SDIO 通过 `platform_bus` + `hardware_id` 注册绑定。板级 IRQ 资源经 `platform_device_irq()` 读取：发布了就用中断，没发布就轮询。DW SDIO 有 riscv64 generic Early 模块；两个 GMAC 当前只在 embedded 的显式静态账本中，没有 generic 包。板级 bring-up 细节（时钟、地址、IRQ、缺项）见 [物理开发板移植](../../platforms/physical-boards.md) |

## IRQ 触发模型（电平 / oneshot）

`driver_irq_dispatch()` 现在按**触发类型**处理设备线，而不是把每条线都当成边沿：

- 路由代码在编程 IOAPIC 重定向项时记录触发类型（PCI INTx = 电平低有效，PIIX3 PIRQ 转换 = 边沿，ACPI 中断资源按自身 trigger 位），`arch_irq_line_is_level()` 把它报给核心；驱动也可以用 `IRQF_LEVEL` 自行声明。
- 电平线在调用 handler **之前**被 mask，handler 返回后按需 unmask。handler 自己清掉设备中断源的（e1000/rtl8139/AHCI/r8169/xHCI）走这一形态。
- **`IRQF_ONESHOT`**：handler 只唤醒线程、无法在自身内清源的设备（NVMe INTx 的 completion 由提交者摘除）必须用它。核心保持 mask，驱动在真正处理完（摘除 CQE）后 `irq_enable()` 重新武装；屏蔽期间到达的完成会让电平线继续有效，重新武装时立刻再投递，因此不丢唤醒。
- IOAPIC 的 mask/unmask 是读改写，新增 `g_pci_intx_mask_lock` 串行化；MSI-X 的 LVT 是每 CPU 状态，不需要该锁。
- IRQ 返回处的抢占窗口新增**每 CPU 单实例保护**：窗口会重新开中断，仍被拉住的电平线可以让中断在上一个窗口内再次进入并嵌套，最终压穿内核栈。现在同一 CPU 同时只允许一个窗口，`__switch()` 发布新栈前清除标志。

回归证据：`smoke-laptop-shell` 的 1200×8 KiB NVMe 读（oneshot 路径）、`smoke-net-rtl8139` 的 hostfwd 回环（普通电平线，7 次 handler、RX/TX 计数均增长）、`smoke-net-e1000-irq`（第二个 INTx 驱动 + 真实 `link_up`）、`smoke-laptop-gui`（4 vCPU，xHCI/virtio 的 INTx 与 MSI-X 混合）。

另外一个与驱动"可用性"直接相关的补齐：`class_net_link_up()` 的实现一直存在，但**协议栈在 netif 注册后从不复查**，所以拔插网线不会改变接口状态。`a20_lwip_poll_rx_locked()` 现在在 lwIP 锁内以 1 Hz 调用 `a20_lwip_netif_sync_link_state()`（该函数本来就注释为"从同一条 poll 路径可达"），把驱动上报的 carrier 变化转成 netif link 事件。这样 r8169/e1000 的 `link_up` 才真正生效。

### 行为模型测试（从 Linux 源码反推设备契约）

没有真机、QEMU 也没有这些设备模型时，可验证的是**驱动与设备之间的契约**，而这个契约就写在 Linux 驱动如何使用设备里。做法是：先从 Linux 源码反推“设备必须如何响应”（寄存器语义、位位置、ACK/NAK、FIFO、描述符所有权），把这些响应写成寄存器级/字节级模型，再让**真实的驱动代码**跑在模型上。

| 驱动 | 模型 | 覆盖 |
|---|---|---|
| r8169 | `r8169_sim_t`：0x100 字节寄存器阵列 + 状态机（reset 自清 RST 并重载 IDR、Cfg9346 锁窗丢弃 MAC/环基址写入、`TxPoll.NPQ` 触发取环、RX 长度含 FCS、完成描述符清 OWN、PHYstatus 报 link） | 真实 `r8169_reset`→MAC 读→`r8169_hw_start`→`r8169_send`→`r8169_recv`→`r8169_poll` 全链路：64 B 帧回环逐字节比对、环基址写入在锁窗内被丢弃、描述符回收与重挂 |
| i2c-designware | `dw_i2c_sim_t` 寄存器模型 + `dw_i2c_slave_t` 模拟 I2C-HID 从机（寄存器指针、裸读输入报文、命令写） | 真实 `dw_i2c_init_hw`/`dw_i2c_transfer`：30 B 描述符读、300 B 长读（8 深 FIFO 交错）、命令写 `[0x00,0x08]` 框架、错误地址 → `-ENXIO`；并断言 `IC_CON` 含 `RESTART_EN`、时序值 = Linux 公式（100 MHz/400 kHz → ss 427/499、fs 87/159） |
| i2c-hid | 适配器级 mock（HID 描述符/报告描述符/输入报文） | 描述符读取、命令框架、裸读输入、长度头语义、单触点与多点触控事件 |

**局限（必须说清楚）**：模型是按“Linux 驱动期望的设备行为”写的，所以它能抓住**驱动与契约不一致**的 bug（本轮就抓到：门铃缺失、`IC_CON` 位错位、`I2C_M_WRITE` 恒假、13 位长度掩码、锁窗内写环基址），但抓不到“驱动与模型共享同一个错误假设”的问题。电气时序、PHY 自协商、真实 report descriptor 形状仍然只能真机验证。

驱动自检的挂载方式是 `main.c` 的 `a20_driver_boot_selftests()`：每个驱动提供一个弱符号钩子（`a20_boot_selftest_i2c_hid()`、`a20_boot_selftest_r8169()`、`a20_boot_selftest_i2c_dw()`），未链接该驱动时弱默认返回 `-ENOSYS` 并被跳过；钩子不得触达真实总线。

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
| NVMe | BLOCK | `kernel/drivers/block/nvme.c` 共享实现，drvmod 仅入口/描述符；PCI class、每控制器状态、受限 coherent DMA、INTx oneshot/等待队列和轮询回退。兼容4 KiB page、首个活动namespace。LoongArch原有队列/读写门禁及x86 16 CPU/8 GiB NVMe桌面普通重启、退出后fsck通过；内部SSD上的完整桌面尚未单独实机验收。 |
| E1000 | NET | VirtualBox/QEMU 82540EM 与 82574L(e1000e)，**完成路径默认中断驱动**（82574L：MSI-X → 单消息 MSI → INTx；82540EM 无 MSI/MSI-X 能力，直接 INTx；`request_irq()` 成功后才打开 `IMS`，无向量时保持纯轮询），已加 stop/remove；每设备独立分配状态与 ring；probe 按 RDBAH/TDBAH 的 64 位拆分寄存器声明 64-bit DMA mask，ring 块越窗时 probe 失败（`-EOPNOTSUPP`）；`a20.e1000.poll=1` 强制纯轮询；RX 由 ISR 一次性排空（复用 `a20_lwip_process_netif_irq_locked()`，**无 NAPI budget**），TX 靠 `tx_done` 镜像 TDH 在发送路径与中断路径释放描述符（旧代码清 DD 后从不重置，ring 首绕之后即永久失败）；`/proc/a20/perf` 的 `e1000_irq_calls`/`_rx`/`_tx`/`_empty`/`e1000_tx_reclaimed`/`e1000_rx_drained` 分开计数（`_rx`/`_tx` 是 `_calls` 的子集，不相加）。**e1000e MSI-X 数据面**：82574L 在 MSI-X 模式下用 `RXQ0`/`TXQ0`/`OTHER` 报告收发与链路事件，不是传统的 `RXT0`/`TXDW`；probe 现在写 `IVAR`（RXQ0→vector0，TXQ0→vector1，OTHER→vector0）并把 `IMS` 切到队列位，否则表虽武装但每个队列中断都会被设备当作 invalid vector 丢弃，而协议栈的 poll 钩子会掩盖它。按向量计数 `irq_vec[0/1]` 在进程上下文打印一次 `MSI-X vectors carried traffic: vN=… vM=…`；ITR 的 82574L 判断原先错误地拿 `pci_device_id()`（vendor<<16|device）与 device 号比较，现已修正（实测 `itr=3072us`）。门禁：`make smoke-net-e1000-irq`（82540EM + hostfwd）与 `make smoke-net-e1000e-msix`（q35 + `-device e1000e`，断言 IVAR 行、两个向量都 >0、`e1000_irq_rx/_tx/_tx_reclaimed/_rx_drained` 均增长、自然关机）**均已实跑 PASS**（日志 `.kernel-build/smoke/net-e1000e-msix-x86_64.log`：`v89=1 v90=3`）；`a20.e1000.poll=1` 的强制轮询分支本身未被门禁覆盖 |
| RTL8168/8111 (r8169) | NET / 模型自检 | 每设备私有状态、DMA ring、IRQ/轮询，remove 超时保留资源，成功后释放。模块自动匹配10ec:8168，静态 ID 表还有8169/8161/8136/8101；这些其他硅片变体未实机验收，不据此宣称整个系列可用。QEMU无相应硬件模型，寄存器模型不代替有线网络实测。 |
| AMD DesignWare I2C | embedded / 已实机 | `AMDI0010/AMDI0011`、DesignWare DT compatible；从固件取得 MMIO、时钟属性，注册带引用的 I²C adapter。FIFO/repeated-start/NAK 模型自检通过；Lenovo 83EG 经 I²C-HID 完成实际传输。跨架构编译不等于控制器实机验收。 |
| I2C-HID 触摸板 | embedded / 已实机 | `_CRS` 地址、`_DSM` 描述符寄存器、原版 libinput 的 MT 输入；CE78 feature reports 放私有 quirk 实现，AMDI0030 attention 读取在 GPIO 适配层。第18轮6566次有效输入、错误/恢复/丢帧为0。整理版改为每设备工作队列、同步移除及 adapter 引用；新生命周期经 host/QEMU 后仍需下一轮实机复验。未提供 generic ACPI 驱动包自动选择。 |
| RTL8139 | NET | Realtek 10ec:8139，PCI class `0x020000` 匹配；x86_64 QEMU（`-device rtl8139`），此前该型号无任何驱动认领、`DEV_CLASS_NET` 里根本没有网卡。**默认中断驱动**（PCI INTx；`request_irq()` 成功且 handler 就位后才写 `IMR`，失败或 `a20.rtl8139.poll=1` 则纯轮询）；静态单实例；RX 64 KiB ring（`RCR[12:11]=0b11`）+ 4×2048 TX 描述符，同一块 kmalloc、按 2048 对齐；`send()` 先线性拷贝进 bounce buffer 再写 TSAD/TSD/`TxPoll`（写 0x60：datasheet 的 bit5 与 QEMU 的 bit6 都置，bit7 保持 0 以保留正常优先级）；`recv()` 遇到坏帧/超长按 **丢弃而非截断** 处理并照样推进 `CAPR`；`caps()` 返回 0，**不声明任何 checksum offload / SG / MRG**（本树有意不开，见 `driver_class.h:145-146`）；`link_up` 留空即始终 up，不读 MediaStatus（8139/8139B/8139C 极性不一致，报错 down 会静默把 netif 下线）；`/proc/a20/perf` 的 `rtl8139_irq_calls`/`_rx`/`_tx`/`rtl8139_tx_reclaimed`/`rtl8139_rx_drained` 分开计数（`_rx`/`_tx` 是 `_calls` 的子集，不相加）。门禁：`make smoke-net-rtl8139`（x86_64 QEMU q35 + `-device rtl8139` + hostfwd 端到端回环，断言 `mode=irq` 且中断与两个收/发退休计数 > 0）**已实跑 PASS**（日志 `.kernel-build/smoke/net-rtl8139-x86_64.log`，`mode=irq` 且中断与两个收/发退休计数 > 0）。寄存器图与 ring 几何取自外部 Debian QEMU 10.0.13+ds 源树（`qemu-10.0.13+ds/hw/net/rtl8139.c`；见[外部 QEMU 源码定位](../../dev/external-dependencies.md)），**未在真实硅片上跑过**；同一寄存器在 RTL8139C datasheet 上记为「8K + 16K」，几何是否一致未验证。已知限制：单静态实例、无 WOL/省电/速率报告、无多队列与接收合并、无吞吐结论；`a20.rtl8139.poll=1` 的强制轮询分支本身未被该门禁覆盖 |
| VMSVGA/SVGAv3 | DISPLAY | VirtualBox x86_64/ARM，BAR offset/pitch 边界和 class registry，已加 remove；静态单实例 |
| xHCI HID | INPUT / 有条件 | PCI class `0x0c0330`、每控制器状态、受限DMA与MSI-X/INTx/轮询回退；同步传输通过`xfer_busy`串行化，完成事件保留唯一消费者，HCD和HID都在私有spinlock外回调/重提交。16 CPU/8 GiB USB桌面、输入、正常关机和退出后fsck通过。USB 拓扑已接入生命周期协调；锁外 completion 按 controller/slot 计数保留，Disable Slot 成功后的重试不重复硬件命令，pending 端口阻止同控制器编号复用。host 线程交错和双 CPU QEMU 控制器退出/重绑通过；HID/hub 独立退出和重绑已验证；storage、其他 DMA 错误阶段与任意 SMP 压力仍未覆盖。具体边界见[锁顺序](../../drivers/guide/lock-order.md)及[复用指南](../../drivers/guide/x86-hardware-reuse.md)。 |
| USB Storage (BOT) | BLOCK | 共享实现位于 `kernel/drivers/usb/class/usb_storage.c`；generic 由 `usb-storage.a20drv` 包装，embedded 静态链接。xHCI bulk + BOT（CBW/CSW）+ SCSI READ(10)/WRITE(10)/READ_CAPACITY(10) 已在 QEMU x86_64 验证，动态发布 `/dev/diskN`。只支持单 LUN、512/2048/4096 扇区、每命令 4 KiB 数据块；`msc_command()` 的每实例锁已由 `spin_lock_irqsave` 改为 sleepable `mutex_t`，因此跨同步 bulk wait 持有它是合规的（块层从进程上下文调用）；probe 失败路径不再对静态 `g_storage` 槽位执行 `kfree()` |
| TPM 2.0 (TIS) | x86 安全 | `tpm.a20drv` 模块：ACPI TPM2 表发现 + TIS FIFO 状态机 + Startup/GetRandom；无 TPM 时 probe 优雅返回 |
| PS/2 | INPUT / 单ISA控制器 | `kernel/drivers/input/ps2.c` 共享实现，drvmod仅包装；通过hwapi操作端口，经固件接口请求架构IRQ路由；键盘、鼠标输出input class，控制台字符经统一输入路径；重复绑定拒绝，remove停止并排空IRQ。该Lenovo普通按键来自USB，PS/2不能替代USB键盘验收。 |
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
