# 驱动实现符合性与限制

> 1.0 正确性与恢复缺口见 [台账](../gaps/README.md)。2026-11-16 起，PCI/MSI-X 扩展和 DRM/GPU 功能强化见 [后续路线](../../roadmap/next-horizon.md#后续版本功能强化)；本页保留实现及证据范围，不将后续目标计为完成。

> 驱动实现遵循单一初始化入口规范；状态变更需同步更新相关文档与约束说明；“符合”仅指在声明支持的目标配置下验证通过。

审查覆盖 driver core、公共总线、STM32F103、VirtualBox ARM64/x86_64 及其设备驱动。

## 1.0 当前收尾范围（2026-11-17 更新）

按项目负责人要求，当前只完善[已有支持的常见 PC 与 QEMU 设备](../../drivers/meta/v1-pc-qemu-scope.md)。下文各设备记录保留实际实现与证据边界，其中“其它 provider、实体停止/失效仍开放”等旧范围表述不再要求所有 SoC、开发板或厂商逐项验收。Hi3861 外设和其它板级专用路径不再进入当前收尾队列；已完成改进保留。PC/QEMU 的具体已知缺陷仍按直接相关的检查修复，不把缺少全厂商实板故障记录单独作为发布阻塞。

本页的能力限制和未实测记录不是额外任务清单；常见 PC/QEMU 驱动独立收尾已关闭，剩余综合交付不要求补齐全部型号、开发板或外设功能。

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
| 原生组合协议 | 基础可用 | 每绑定 endpoint 复用 class publication，六类 primary 自动发布，其他服务无需 class；版本/实际表大小/能力匹配，不可变元数据可跨 offline，调用按同一 active count 排空，旧 endpoint 不跟随重绑。完整/MCU 共用核心；VirtIO-net、RTL8139、lwIP 与模块导入已接入，ABI 13 不变；原生请求/队列已接入，硬件特有调度与恢复由设备协议实现 |
| 原生请求/队列 | 已实现并接入 VirtIO GPU | 有界 depth/active/bytes、FIFO dependency、排队取消、乱序硬件完成与任务退休；保留固定 VMO/charge 或 DMA buffer；共享 worker/早期 polling；自动 execution stop 先于 call drain，失败保留并重试。实际 controlq 仍一个 active chain；厂商 BO/GPUVM、主动 abort/抢占、动态队列和公平调度尚需上层实现，见[请求队列](../../drivers/guide/request-queues.md) |
| driver core | 符合 | 动态 registry；bus ID 后可执行无副作用的 driver protocol match；操作 mutex 串行化注册、probe/remove 与遍历；数组 spinlock 不跨回调；managed probe 失败进入可失败退出，未确认停止时保留绑定；增长失败注入在 riscv64（单 hart 与 `-smp 2`）、x86_64、aarch64、loongarch64 上都实跑，driver/device/bus 三条路径均为 `-ENOMEM` → 旧条目保留 → 重试成功 → 计数与 capacity 恢复（`smoke-driver-lifecycle-{x86_64,aarch64,loongarch64,smp2}`） |
| 固件设备模型 | 已实现 | DT、ACPI/uACPI、静态 provider 共用节点/物理资源/IRQ domain/依赖接口；未知设备独立保留，失败事务回滚，probe 可延后。交接、保留区和架构边界见 [启动模型](../../dev/boot-model.md)；不代表任意板卡已完成实机验证。 |
| PCI bus | 已扩展 | ECAM、ID/subsystem match、零值未分配 BAR sizing/assignment、BAR 编号查询、modern VirtIO capabilities；已设置 `matched_id`。BAR 规划在 discovery 事务内核对资源声明/占用，全部通过后提交新地址与资源；规划失败保留原 BAR/command/资源，调用者仍须先 quiesce；已持有 function 的用户查询仅读现有 BAR/IRQ 清单，不重写 BAR/BME 或资源 |
| 显式失败 probe 清理 | 基础可用 | `device_probe_use_remove` 在初始化前无分配选择既有部分实例退出管线；清理失败保留绑定/devres/依赖和原 probe_error，子设备阻塞状态拒绝新使用。device/driver 布局沿用既有模型，当前 ABI 13，旧驱动需逐一迁移；不代表实体 GPU 停止已验证 |
| USB 部分初始化清理 | 基础可用 | 预分配端口 owner，非零 alloc_slot 结果仍可交还已知 token；枚举/部分发布失败先停 slot 再解绑，错误保留地址/协议图/HCD，扫描和注销可重试，保留三次连接预算。device/driver 布局沿用既有模型，当前 ABI 13；xHCI 整机退出另有已确认停机协议；当前 hub 转发层已接入提前关闭与失败保留，未知 slot 的单独恢复仍待实现 |
| 提前退出准备 | 基础可用 | 当前 probe 显式登记可失败 prepare，先于 consumer unbind；LIFO/逐项完成记录，错误保留 PREPARING/绑定/图/资源。xHCI 已接入失败 probe 和已确认停机后的软件 slot 清理，device/driver 布局沿用既有模型，当前 ABI 13。HID/hub/storage 独立退出已接入可失败 endpoint 释放，storage 活跃调用、失败实例保留与 BOT 恢复见下文，额外 DMA 错误路径仍需完善；USB 拓扑已串行协调，xHCI 锁外回调已计数保留 |
| USB endpoint 交还 | 基础可用 | ABI 10 HCD release_endpoint；HID/hub 的独立退出和失败 probe 保留 URB/ring，xHCI Stop/Drop 分段读回确认，回调在途返回 EBUSY，重试与同控制器其他 endpoint 隔离；storage 已完成受管实例、消费引用和端点清理，BOT recovery、短包/STALL 与多 TRB 的软件修复见下文；xHCI bulk 的调用者存储由独立 DMA 暂存对象保护 |
| 可失败解绑 | 基础可用 | remove 非零保留离线绑定/匹配/devres/依赖和注册引用，device/platform/driver unregister 传播错误并支持显式重试；仅执行一次 quiesce，注销期间拒绝新绑定/消费关系与递归退出。发布失败清理同样保留；PCI 槽、已发布 USB 端口/slot/address 保留已接入。C/Rust 主机失败注入与合成 QEMU 门禁通过；关机/kexec 已复用同一可失败解绑管线并检查结果，实际硬件停止和失败 probe 安全仍由 provider 负责 |
| 驱动 CPU 物理映射 | 基础可用 | 原生 MM/Rust 共享 eager 事务，fbdev mmap/旧 ioctl 与用户驱动接入；完整 hint 区间、fixed/noreplace、旧叶/PFN 0、失败页/前缀回滚及 TLB 退休，统一 total_vm/RSS 和保护 ceiling，mprotect 保留 Device 属性；既有通用入口借用 backing；fbdev 已用 MM resource owner 持有旧物理 GPU 绑定，映射尚存时退出返回 EBUSY，最后引用/TLB 退休后可重试；无 DMA 隔离，NOMMU 不支持此新入口 |
| 连续 RAM scanout | 基础可用 | 连续 VMO 工厂将高阶块转换为 canonical 页、清零暴露 padding、固定 extent/slot；ABI 11 引入的 get_scanout 转移 VMO 引用，virtio-gpu/fbdev 经普通 VMA 支持 fork/split。主机错误/引用测试、双核 resident fork/解绑链与 RISC-V 连续 VMO EDU DMA 通过；VMO 引用本身不证明停止；virtio transport 确认另见下项，不表示实体 GPU 加速 |
| VirtIO reset / GPU 失败清理 | 基础可用 | bus reset helper 有时间/poll 双预算，status 读回 0 后才清理；virtio-gpu 采用原生失败 probe/quiesce/remove，reset 失败保留 IRQ/存储/绑定，四个 issuer 的超时或异常 completion 封闭共享 staging。主机故障/重试与双核真实解绑/旧 VMA 保留/重绑通过；实例已由每绑定原生 DMA owner 持有，双设备独立停止/重绑与 peer 刷新已通过 QEMU；DRM 另经每 publication access/停止凭据保留和退休外部 backing；厂商新增队列/GPUVM 功能按后续范围处理 |
| GPU publication 访问 | 基础可用 | 默认策略保留 device 元数据、读取保留 class；scoped access 捕获同绑定 device/ops 与 active call，离线/重绑拒绝旧句柄。fbdev ioctl/mmap 安装/刷新及 sysfs GPU 内容生成已接入；主机竞争与双核 2D QEMU 已验证。选择已改为显式原生角色，排除 headless 并在渲染同级时优先原生输出，RAM framebuffer VMA 已由 owning VMO 保留；DRM 已固定文件 publication 并隔离 GEM/显示状态，失败清理待成功或旧绑定停止才退休；每文件 GEM/FB owner 与 close 回收已接入，完整 master/auth、共享访问权和 GPUVM 仍为后续功能；旧 get_fb 物理 VMA 已接入资源引用和可失败退出，GPU vtable 追加回调后当前 ABI 13 |
| DRM 每文件对象所有权 | 基础可用 | 同 publication 的 GEM 数值 ID 不授予其他文件访问；PRIME/GEM_OPEN 显式安装幂等 owner，正常最后 close 交还该文件 GEM/FB。framebuffer、dma-buf 与 VMA 各持 backing，失败 host 清理继续保留；GETFB 不向未持有者授予新 backing。主机失败/引用测试和 guest 验证见原生运行时；不表示完整 master/auth、只读共享权限或命令流隔离，ABI 13 不变 |
| GPU 原生能力 | 基础可用 | ABI 13 get_capabilities 声明 SCANOUT/RENDER/RAM_SCANOUT/BOOT_SCANOUT，查询失败清零并校验角色关系；默认优先渲染，同级原生管理输出优于固件回退，等分稳定。主机 provider/错误/版本测试、原生双核角色门禁及真实双 virtio 2D 查询通过；不提供通用 job/格式/内存限制或多 GPU 用户节点 |
| 固定 resident RAM VMO | 基础可用 | kind 5 在发布前完整物化/清零 order-0 页，extent/slot 固定，无连续性保证；预算与 cgroup pin 随 VMO 保留，部分失败完整回滚，DRM 已在 store 锁外采用并在 GPU mutex 前捕获 owner。普通 VMA/导出保留 backing；独立 page pin 不转移 charge。主机真实 C/Rust/cgroup 与原生 VMA/EDU DMA 验证见运行时文档；不表示 BO 迁移、VRAM/GPUVM 或任务调度，ABI 13 不变 |
| 固定 RAM VMO 的 owning DMA 视图 | 基础可用 | map_owned_vmo 仅接受工厂 kind 4/5，成功及失败清理句柄保留整个 VMO/原始 charge，包括未映射页，设备只映射所选视图；多视图不重复 charge。buffer/VMO 最终 put 在 domain 锁外，try_unmap 的临时 domain 引用跨 owner 回调；构造回滚依靠 caller 全程持有的 domain 引用。生产 C/Rust/cgroup 主机集成、跨设备 owner 回调与 EDU 实际 DMA 验证见运行时；不等于 GPUVM/权限/停止证明，ABI 13 不变 |
| 原生 VMO DMA 导入 | 基础可用 | Rust 原子保留已物化 canonical RAM，缺页不物化、物理 VMO 拒绝、4096 页批次；显式 map_vmo 交还临时 pin，mapping/清理句柄跨源 resize/销毁保留原帧；不转移预算/权限或建立驻留租约，真实 EDU 在源 VMO 销毁后 DMA 已验证 |
| 独立 DMA buffer | 基础可用 | 记录实际 PFN/分配阶数，支持大小独立于强对齐，捕获 mask，显式引用跨 unbind/owner 注销保留存储；可显式 map_buffer 接入 domain，覆盖请求页及末页 padding，失败清理保留整块对象；RISC-V native 不再二次 pin 尾页；无隐式隔离/停止证明，xHCI bulk 已使用；该 buffer 接口未改布局，当前全局模块 ABI 13 |
| 原生 fence 一次性通知 | 基础可用 | pending 登记独立持 fence，IRQ-safe 短 callback 在锁外执行，竞争准入与原错误状态保留；任务 try_remove 对 RUNNING 返回 EBUSY 保留 owner，成功后锁外释放，通知后也须显式 remove。代码/context 与已调度 work 由调用者保留；主机竞争/失败注入及 x86 原生 worker 验证见运行时，不代表完整 GPU job 或 DRM 同步适配，ABI 13 不变 |
| 原生异步驱动运行时 | 基础可用 | 原生 request/queue、调用排空前可失败 execution stop、页级 DMA domain/mapping、fence、共享工作队列、IRQ 排空与有界固件来源/image。失败保留与回收契约及 hosted 限制见 [原生驱动运行时](../../drivers/guide/native-runtime.md)；尚未接入实体 GPU |
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
| hwapi DMA | 基础可用 | coherent helper 返回物理 handle，sync 委托 arch cache hook；`device_t.dma_mask`（默认 64 位全通）+ `dma_set_mask()`/`dma_addr_ok()`/`dma_range_ok()`，分配器按 mask 校验：普通 helper 的 kmalloc 块越窗后改走帧分配，aligned helper 直接取连续页；受限 mask 由 `pfa_alloc_max` 选择可达页，无可用页时返回 `NULL` 并让 probe 失败，无 bounce 路径也不截断 handle；HDA 按 GCAP[0] 声明窗口，xHCI/E1000 声明 64-bit；限制：这些 coherent helper 不自动接入 IOMMU/IOVA，也无 bounce（原生页映射和用户态 PCI 驱动可显式使用下面的 RISC-V IOMMU 域），32 位 mask 在没有可达低地址 RAM 的机器上仍会 probe 失败，`dma_alloc_coherent_aligned` 的对齐上限仍是 `PAGE_SIZE` |
| RISC-V IOMMU（用户驱动隔离） | 已扩展 | `riscv_iommu.c`：DDT(1LVL)/CQ/FQ 初始化、TR_REQ 映射/拒绝探测、per-device 动态 domain（最多 8 个，正 PID 接口；与原生 domain 共享 PCI requester 占用，失效失败保留）、map/unmap + IOTINVAL/IOFENCE、fault queue 按记录 devid 归属并 fail-closed 阻断、`/proc/a20/iommu` 计数。**每个 domain 分配唯一 TA.PSCID**：IOTLB 键是 `(tag,GSCID,PSCID,IOVA)`，共用 PSCID=0 会让两个设备对同一 IOVA 互命中对方的物理页（多设备门禁实测过）。门禁：`smoke-iommu-discovery`、`smoke-iommu-udriver-isolation`、`smoke-iommu-multidevice` **均已实跑 PASS**（双 edu：两个 domain，dev0 fault 后 dev1 仍能 DMA，随后各自 release）；仅覆盖 QEMU `riscv-iommu-pci` |
| 原生 RISC-V IOMMU provider | QEMU 已验证 | 将当次发布 PCI 身份接入 managed domain；负 token 与正 PID 用户 domain 共用 PCI requester 登记，DC 发布前排斥其他 claim，失效失败保留页面/domain/占用。构造/关闭要求 BME 已清除，shutdown 后占用保留至最后 put；用户新 claim 在取得占用后才清除/读回 BME。EDU 实际跨页 DMA、unmap 后 fault、worker → fence 通知已有验证；仅该 RISC-V 后端，不含 AMD-Vi 或 GPUVM |
| class publication | 已扩展 | probe 成功发布 class/native protocols；有 execution action 时 offline 后先执行可失败 stop，再 quiesce、排空调用和 remove/devres；失败保留旧绑定。未提供任何服务的绑定可无 endpoint |
| devfs/sysfs | 动态类视图 | char/block/audio 自动生成 `/dev/charN`、`diskN`、`audioN`；所有 class 动态暴露于 `/sys/class`；旧 display/input 聚合节点暂时保留 |
| drvmod loader | 基础可用 | 四架构（riscv64/x86_64/aarch64/loongarch64）ELF `ET_REL`、`.a20drv` 描述符校验（唯一元数据，覆盖 RTC..USB 全部类型）、框架符号白名单、全量 ELF 边界校验（section/symbol/strtab/relocation）、按 `e_machine` 选择重定位解码器（RISC-V CALL、x86-64 large 模型 ABS64/`.lbss`、AArch64 CALL26+veneer、LoongArch CALL36/pcalau12i）；模块与内建共用同一 `driver_t`（经 `drv_driver_register` 注册，核心 `device_t` 匹配/probe），不再有第二套 `drv_driver_t`/`drv_device_t`；统一驱动管理器（`driver_manager.c`）扫描 DriverStore、按描述符路由内核模块与用户服务，已 pinned 模块不会被重复执行 DriverEntry；drv_env 双驻留共享代码的模块后端（DRV_ENV_DRVMOD）；goldfish RTC、CMOS RTC（x86_64 Early DriverStore）、PC speaker、virtio-input 内核探针、virtio-input 完整驱动、PS/2 键鼠控制器、NVMe、TPM 2.0、Intel HDA、virtio-blk、virtio-scsi、AHCI、DW SDIO、E1000、RTL8139、VMSVGA、virtio-net、virtio-gpu、virtio-snd、virtio-console、virtio-rng、xHCI、USB HID、USB storage 均已迁移为模块（generic 无内建设备驱动）；PCI 类驱动访问器、DMA 增强（aligned/sync）与地址 mask（`dma_set_mask`/`dma_addr_ok`/`dma_range_ok`）、调度/等待原语、`clock_ticks_per_sec`、内核熵池 `random_reseed`（供硬件 RNG 模块把读到的字节折进 `kernel/core/random.c`，见下方 virtio-rng 行）与 ACPI TPM2 发现均已导出；签名、运行时 unload 尚未完成 |

## drvmod 迁移矩阵

| 驱动 | 迁移状态 | 说明 |
|---|---|---|
| goldfish RTC | 已迁移 | 内建 `goldfish_rtc_kdrv.c` 已删除；`rtc.a20drv` 只匹配固件声明的 Goldfish RTC。AArch64 QEMU virt 使用 PL031，不应声称本模块绑定；该平台的模块冒烟现检查 virtio-blk，接线检查与真实 guest 结果分开记录 |
| PC speaker | 已迁移 | `pc-spkr.a20drv`（x86_64）通过 AUDIO 类绑定 platform；端口 claim、停声读回、受管失败清理与有界音调取消已接入，软件证据见下文 |
| CMOS RTC (MC146818) | 新增 `.a20drv` | `cmos-rtc.a20drv`（x86_64 Early）：x86_64 墙钟来源，只读、无 IRQ、无时区换算、century 寄存器不可信时按 20xx 处理；门禁 `smoke-rtc-cmos` **已实跑 PASS**（`-rtc base=utc`，宿主侧复核来客墙钟；日志 `.kernel-build/smoke/rtc-cmos-x86_64.log`，来客采用 epoch 1791231484，与编译期种子 1791230264 不同，说明读到的是硬件），`smoke-rtc-cmos-fallback`（CMOS 落在可接受窗口外时回退到编译期种子）**尚未执行过**。两块 x86_64 板级（qemu-virt-x86_64、x86_64-pc）都声明了该 platform 设备 |
| UART | 内核服务 | 启动早期串口在模块加载前可用，driver_t 只发布接口，不接管板级永久控制台的存储寿命。原 DRIVER-08 已关闭：RV64 单核/SMP2 无读者 Ctrl-C 实测由真实 RX IRQ 完成，poll 字节保持 0；范围见[本轮进度](v1.0-progress.md#本轮完成与验证2026-11-16)，正式候选配置仍归 N18/N20 |
| virtio-blk | 已迁移 | 根文件系统挂载依赖它；`virtio-blk.a20drv` 已嵌入 Early DriverStore（root ramfs overlay），在挂载真实根盘前加载，解除 bootstrap 循环 |
| PCI bus、virtio-mmio bus、USB core、framebuffer/gpu_core、audio_core、input_mux | 内核服务 | transport、class 聚合和设备节点服务，保持静态链接，不计入迁移账本 |
| DMA/PCI BAR/class ops 框架 API | 已导出 | `drv_dma_alloc_coherent`/`dma_alloc_coherent_aligned`/`dma_sync_*`、`pci_get_bar_resource`/`pci_enable_and_assign_bars`/`pci_intx_irq`/`pci_class_code`/`pci_bus`、block/net/input/audio/display class 操作（统一核心桥接）|
| virtio-input 内核探针 | 已迁移 | 内建 `virtio_input_kprobe.c` 已删除；`vinput-probe.a20drv` 用 drv_env 模块后端复用双驻留共享协议，`smoke-dual-input` 验证与用户态 uinputd 读到同一设备身份 |
| virtio-input 完整事件投递 | 已迁移 | `vinput.a20drv` 模块（virtq + IRQ + input class），`/dev/event0` mux 拆分至 `kernel/drivers/input/input_mux.c`，`input_mux_wake` 导出；QEMU 实测事件流 |
| AHCI、DW SDIO、virtio-scsi | 已迁移 | `ahci.a20drv`（x86_64 Early）、`dw-sdio.a20drv`（riscv64 Early）、`virtio-scsi.a20drv`（四架构 Early）经 Early DriverStore 在根盘挂载前加载，与 virtio-blk 一起解决根设备 bootstrap |
| E1000、virtio-net、virtio-gpu、vmsvga、virtio-snd、xHCI、USB HID、USB storage | `.a20drv` | 设备实现仅在 `tools/mk/driver-modules.mk` 为目标架构列出时由 generic DriverStore 提供；其 transport、framebuffer 和 class 服务仍由内核提供 |
| virtio-console | `.a20drv` / 受管停止与批量 TTY | raw `/dev/vport0` 保留 id 0、同步写契约；`cli-x86_64` 默认选择带背压的批量终端，协商 MULTIPORT 控制队列并打开固定物理端口 1，逻辑节点仍为 `/dev/vport0`。默认 16 个 DMA TX 槽，仅设备完成后复用；支持 [compact 内存预设与参数覆盖](../../drivers/classes/virtio-console.md)；支持 drain、可中断空间等待，绑定租约保护模块操作；受管失败 probe 与活动 RX/TX/ioctl/IRQ 保留已接入，复位失败保留 owner、IRQ 和全部 DMA 供重试。单实例、单数据端口、无 MSI-X。x86_64 QEMU raw 双向回环、完整字节输出及背压测试通过，见 [virtio-console 规范](../../drivers/classes/virtio-console.md)。 |
| virtio-rng | `.a20drv` / 受管停止 | `virtio-rng.a20drv`（riscv64/x86_64）：`VIRTIO_ID_RNG` 熵源 → CHAR 类 `/dev/hwrng`，读到的字节同时经 `random_reseed()` 折进 `kernel/core/random.c`（因此需要新增导出该符号）。双 transport：PCI（x86_64，`1af4:1044` modern 与 `1af4:1005` transitional）与 virtio-mmio（riscv64，device-id 4）。**单静态实例、队列 0 的 8×256 B 设备缓冲只在 read 需要熵时才挂上（QEMU 每次按 avail ring 字节数向宿主索取，挂满就等于持续抽宿主熵）、读路径 500 ms 有界等待、熵池只在 read 路径喂（ISR 不喂，`random_reseed` 经 `arch_entropy_sample()` 触及 `proc_current()`）**；门禁 `smoke-virtio-rng`（x86_64 QEMU + `virtio-rng-pci`，`hwrng_test` 读 ≥256 B 并断言非全 0x00/全 0xff/单字节重复）**已实跑 PASS**（日志 `.kernel-build/smoke/virtio-rng-x86_64.log`，`HWRNG_TEST: PASS bytes=256`）；受管失败 probe、活动读取停止和复位确认后回收已接入，本轮实际读取/重开/自然关机通过，见本轮记录 |
| RTW8852BE | NET + 用户态服务 / 已实机 | 独立上游 rtw89 服务通过受控 PCI/DMA broker 提供 Ethernet。单无线设备/单 MM 所有者、32 MiB DMA 上限；受管停止检查 BME 回读，失败保留绑定；解除映射失败保留 DMA/MM。异常退出遗留 DMA 仍隔离并拒绝重绑定。WHU-WLAN 连接已获用户确认，网页认证有 GUI/HTTP/HTTPS 回归；不声称其他芯片或企业 EAP 已验收。 |
| RTL8139 | 新增 `.a20drv` | `rtl8139.a20drv`（x86_64）：PCI 10ec:8139 + class `0x020000`，TSAD/TSD 四描述符 TX、64 KiB RX ring、IMR/ISR 中断收发。此前该型号无任何驱动认领，`-nic user,model=rtl8139` 起不来任何 NET 设备。门禁 `smoke-net-rtl8139`（hostfwd 端到端回环 + 中断计数）**已实跑 PASS**；寄存器图取自 QEMU 设备模型，未在真实硅片上验证 |
| RTL8168/8111 (r8169) | NET / 模型自检 | 每绑定私有状态、DMA ring、IRQ/轮询；受管失败 probe 和 class drain 前可失败 stop 已接入，忙/超时/失联保留绑定、IRQ 与 DMA，确认复位后回收。模块自动匹配10ec:8168，静态 ID 表还有8169/8161/8136/8101；这些其他硅片变体未实机验收，不据此宣称整个系列可用。QEMU无相应硬件模型，寄存器模型不代替有线网络实测。 |
| TPM 2.0 | 已迁移，软件停止可失败 | `tpm.a20drv`（x86_64）：轮询 TIS FIFO、检查 Startup/GetRandom 帧和超时；停止确认中止、IRQ 全局屏蔽与 locality 归还，失败保留绑定/MMIO；无设备与 CRB 安全拒绝。实体停止未验收。 |
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
| virtio-blk | BLOCK | 类接口；**完成路径默认中断驱动**，三级阶梯：每队列一个 MSI-X 向量 → 共享 INTx → 轮询（无 cmdline 开关，阶梯由 `request_irq()` 是否成功决定）。每级只在前一级失败时进入，MSI-X 部分注册成功时把已注册的那些 `free_irq()` 掉再退回 INTx。现有 `/proc/a20/perf` 的 `virtio_blk_*` 计数器不区分中断和轮询来源，不能单凭这些计数证明 IRQ 完成；该诊断能力不作为新增 1.0 功能项。remove 有界等待 STATUS 归零后才释放已注册 IRQ，超时保留实例和 DMA 队列供重试；失败 probe 已接入受管清理；class drain 前的可失败 stop 已接入；消费引用归还后可复用槽位，仍受原有同时保留实例容量约束 |
| virtio-net | NET | 多实例、IRQ/轮询和类接口；受管失败 probe 与活动收发引用已接入，停止关闭准入、忙时保留，确认 transport reset 后才释放 IRQ/ring；失败保留同一绑定供重试，见下文 |
| virtio-input | INPUT | 类接口、PCI/MMIO 与 `/dev/event0` 聚合；完整 DMA 事件区、逐实例共享 IRQ、受管失败 probe 与复位确认后退休；本轮生产源模型/模块证据见下文 |
| virtio-gpu | DISPLAY | class registry、VMO framebuffer 与 reset 读回确认；失败 probe/退出可失败保留并重试，四个 issuer 发生未知完成后封闭 staging；每绑定原生 DMA allocation 保留独立实例/队列/锁/IRQ，成功 reset/drain 后解锁再交还实例，失败保留；每设备一个同步 controlq；VIRGL 3D feature 协商、capset 查询与 `CTX_CREATE`/`RESOURCE_CREATE_3D`/`SUBMIT_3D` 透传（经 `DRM_IOCTL_VIRTGPU_*` UAPI，见 [3D 图形加速栈](../../graphics/3d-graphics.md)）；2D-only 设备自动回退 |
| VirtIO-SCSI | BLOCK | VirtualBox ARM 已验证，remove 复位/释放槽；只支持 target/LUN 0、READ/WRITE(10)、SYNCHRONIZE CACHE(10)、512B sector、每控制器单条 in-flight 命令。command queue 完成走 used-ring 中断：probe 按 MSI-X → 共享 INTx → 轮询顺序选路（`a20.virtio-scsi.poll=1` 强制轮询，注册失败也回退轮询），IRQ top-half 只 ack 并唤醒 park 中的提交者。混合完成窗口仍在（先自旋 `VIRTIO_SCSI_HYBRID_PRE_POLL_US`，再 park 50 ms 一段），因此短命令多数在自旋窗口内完成、被中断唤醒的比例未量化。x86_64 QEMU（virtio-scsi-pci + scsi-hd）由 `make smoke-virtio-scsi-irq` 验证读写/flush 回环与 `irq_count > 0`；其余架构的中断投递**未验证**，VirtualBox ARM 那条路径仍以轮询为主 |
| AHCI | BLOCK | VirtualBox x86_64；单 controller/单 port/单 slot、LBA48；probe 回滚和 remove 释放 DMA/IRQ。完成路径默认中断驱动，**优先 MSI（cap 0x05）**：`ahci_claim_irq()` 先申请一个 vector 并 `pci_msi_enable()`，失败则回退调用方给的 INTx，再失败才轮询（AHCI 没有 MSI-X 能力；非 x86 的 arch message hook 返回 `-EOPNOTSUPP`，自动走 INTx）。`PxIE` 与 `GHC.IE` 只在 `request_irq()` 成功后才打开，`a20.ahci.poll=1` 强制纯轮询；`/proc/a20/perf` 的 `ahci_irq_completions`/`ahci_poll_completions` 把两条路径分开计数，否则它们从外部同样表现为"盘能用"。门禁：`make smoke-ahci-ich9`（x86_64 QEMU q35 + `-device ich9-ahci` + `ide-hd`，ext4 挂 /extra，断言 `block_flushes` 增长且中断完成计数 > 0）**已实跑 PASS**（日志 `.kernel-build/smoke/ahci-ich9-x86_64.log`，`FSYNC_TEST: PASS` 且中断完成计数 > 0），因此 QEMU 上中断路径已验证（ready 行现在为 `completion=irq(msi)`，门禁同时断言 `[MSI] ... enabled vector`）。无 MSI-X 路径；真实 SATA PHY 与未中断硬件未验证 |
| NVMe | BLOCK | `kernel/drivers/block/nvme.c` 共享实现，drvmod 仅入口/描述符；PCI class、每控制器状态、受限 coherent DMA、INTx oneshot/等待队列和轮询回退。兼容4 KiB page、首个活动namespace。LoongArch原有队列/读写门禁及x86 16 CPU/8 GiB NVMe桌面普通重启、退出后fsck通过；内部SSD上的完整桌面尚未单独实机验收。失败 probe 已接入受管保留；停止关闭 I/O 准入，忙时返回 EBUSY，CC.EN 清除后确认 RDY=0 才退休 IRQ/释放 DMA，超时或寄存器失联保留重试；本轮模型与 x86_64 QEMU 私盘正常读写/flush/停止通过，见下节记录。 |
| E1000 | NET | VirtualBox/QEMU 82540EM 与 82574L(e1000e)，**完成路径默认中断驱动**（82574L：MSI-X → 单消息 MSI → INTx；82540EM 无 MSI/MSI-X 能力，直接 INTx；`request_irq()` 成功后才打开 `IMS`，无向量时保持纯轮询），已加 stop/remove；每设备独立分配状态与 ring；probe 按 RDBAH/TDBAH 的 64 位拆分寄存器声明 64-bit DMA mask，ring 块越窗时 probe 失败（`-EOPNOTSUPP`）；`a20.e1000.poll=1` 强制纯轮询；RX 由 ISR 一次性排空（复用 `a20_lwip_process_netif_irq_locked()`，**无 NAPI budget**），TX 的 `tx_done` 按设备 RS/DD 写回在发送、IRQ 与 poll 路径交还描述符；仅 TDH 前移不再触发回收，避免先于 DMA 写回复用缓冲区；`/proc/a20/perf` 的 `e1000_irq_calls`/`_rx`/`_tx`/`_empty`/`e1000_tx_reclaimed`/`e1000_rx_drained` 分开计数（`_rx`/`_tx` 是 `_calls` 的子集，不相加）。**e1000e MSI-X 数据面**：82574L 在 MSI-X 模式下用 `RXQ0`/`TXQ0`/`OTHER` 报告收发与链路事件，不是传统的 `RXT0`/`TXDW`；probe 现在写 `IVAR`（RXQ0→vector0，TXQ0→vector1，OTHER→vector0）并把 `IMS` 切到队列位，否则表虽武装但每个队列中断都会被设备当作 invalid vector 丢弃，而协议栈的 poll 钩子会掩盖它。按向量计数 `irq_vec[0/1]` 在进程上下文打印一次 `MSI-X vectors carried traffic: vN=… vM=…`；ITR 的 82574L 判断原先错误地拿 `pci_device_id()`（vendor<<16|device）与 device 号比较，现已修正（实测 `itr=3072us`）。门禁：`make smoke-net-e1000-irq`（82540EM + hostfwd）与 `make smoke-net-e1000e-msix`（q35 + `-device e1000e`，断言 IVAR 行、两个向量都 >0、`e1000_irq_rx/_tx/_tx_reclaimed/_rx_drained` 均增长、自然关机）**均已实跑 PASS**（日志 `.kernel-build/smoke/net-e1000e-msix-x86_64.log`：`v89=1 v90=3`）；`a20.e1000.poll=1` 的强制轮询分支本身未被门禁覆盖 |
| RTL8168/8111 (r8169) | NET / 模型自检 | 每绑定私有状态、DMA ring、IRQ/轮询；受管失败 probe 和 class drain 前可失败 stop 已接入，忙/超时/失联保留绑定、IRQ 与 DMA，确认复位后回收。模块自动匹配10ec:8168，静态 ID 表还有8169/8161/8136/8101；这些其他硅片变体未实机验收，不据此宣称整个系列可用。QEMU无相应硬件模型，寄存器模型不代替有线网络实测。 |
| AMD DesignWare I2C | embedded / 已实机 | `AMDI0010/AMDI0011`、DesignWare DT compatible；从固件取得 MMIO、时钟属性，注册带引用的 I²C adapter。FIFO/repeated-start/NAK 模型自检通过；Lenovo 83EG 经 I²C-HID 完成实际传输。跨架构编译不等于控制器实机验收。 |
| I2C-HID 触摸板 | embedded / 已实机 | `_CRS` 地址、`_DSM` 描述符寄存器、原版 libinput 的 MT 输入；CE78 feature reports 放私有 quirk 实现，AMDI0030 attention 读取在 GPIO 适配层。第18轮6566次有效输入、错误/恢复/丢帧为0。整理版使用每设备工作队列与 adapter 引用；2026-11-17 的受管失败 probe/有界 stop 已通过宿主消费者及 x86_64 对象编译，仍需下一轮实机复验。未提供 generic ACPI 驱动包自动选择。 |
| RTL8139 | NET | Realtek 10ec:8139，PCI class `0x020000` 匹配；x86_64 QEMU（`-device rtl8139`），此前该型号无任何驱动认领、`DEV_CLASS_NET` 里根本没有网卡。**默认中断驱动**（PCI INTx；`request_irq()` 成功且 handler 就位后才写 `IMR`，失败或 `a20.rtl8139.poll=1` 则纯轮询）；静态单实例；RX 64 KiB ring（`RCR[12:11]=0b11`）+ 4×2048 TX 描述符，同一块遵守 32-bit DMA mask 的 coherent allocation、按 2048 对齐；`send()` 先线性拷贝进 bounce buffer 再写 TSAD/TSD/`TxPoll`（写 0x60：datasheet 的 bit5 与 QEMU 的 bit6 都置，bit7 保持 0 以保留正常优先级）；`recv()` 遇到坏帧/超长按 **丢弃而非截断** 处理并照样推进 `CAPR`；`caps()` 返回 0，**不声明任何 checksum offload / SG / MRG**（本树有意不开，见 `driver_class.h:171-172`）；`link_up` 留空即始终 up，不读 MediaStatus（8139/8139B/8139C 极性不一致，报错 down 会静默把 netif 下线）；`/proc/a20/perf` 的 `rtl8139_irq_calls`/`_rx`/`_tx`/`rtl8139_tx_reclaimed`/`rtl8139_rx_drained` 分开计数（`_rx`/`_tx` 是 `_calls` 的子集，不相加）。门禁：`make smoke-net-rtl8139`（x86_64 QEMU q35 + `-device rtl8139` + hostfwd 端到端回环，断言 `mode=irq` 且中断与两个收/发退休计数 > 0）**已实跑 PASS**（日志 `.kernel-build/smoke/net-rtl8139-x86_64.log`，`mode=irq` 且中断与两个收/发退休计数 > 0）。寄存器图与 ring 几何取自外部 Debian QEMU 10.0.13+ds 源树（`qemu-10.0.13+ds/hw/net/rtl8139.c`；见[外部 QEMU 源码定位](../../dev/external-dependencies.md)），**未在真实硅片上跑过**；同一寄存器在 RTL8139C datasheet 上记为「8K + 16K」，几何是否一致未验证。已知限制：单静态实例、无 WOL/省电/速率报告、无多队列与接收合并、无吞吐结论；`a20.rtl8139.poll=1` 的强制轮询分支本身未被该门禁覆盖 |
| VMSVGA/SVGAv3 | DISPLAY | 静态单实例与物理 VMA 保留；SVGAv3 命令退休/失败停止经生产源模型验证。SVGA-II 已分离 BAR/端口/FIFO 路径，经 x86_64 KVM SMP4 实际探测、fbdev 映射/UPDATE 与正常停止验证；未扩大为 VirtualBox/物理失效或正式候选认证 |
| xHCI HID | INPUT / 有条件 | PCI class `0x0c0330`、每控制器状态、受限DMA与MSI-X/INTx/轮询回退；同步传输通过`xfer_busy`串行化，完成事件保留唯一消费者，HCD和HID都在私有spinlock外回调/重提交。16 CPU/8 GiB USB桌面、输入、正常关机和退出后fsck通过。USB 拓扑已接入生命周期协调；锁外 completion 按 controller/slot 计数保留，Disable Slot 成功后的重试不重复硬件命令，pending 端口阻止同控制器编号复用。host 线程交错和双 CPU QEMU 控制器退出/重绑通过；HID/hub 独立退出和重绑已验证；storage、其他 DMA 错误阶段与任意 SMP 压力仍未覆盖。具体边界见[锁顺序](../../drivers/guide/lock-order.md)及[复用指南](../../drivers/guide/x86-hardware-reuse.md)。 |
| USB Storage (BOT) | BLOCK | 共享实现位于 `kernel/drivers/usb/class/usb_storage.c`；generic 由 `usb-storage.a20drv` 包装，embedded 静态链接。xHCI bulk + BOT（CBW/CSW）+ SCSI READ(10)/WRITE(10)/READ_CAPACITY(10) 已在 QEMU x86_64 验证，动态发布 `/dev/diskN`。只支持单 LUN、512/2048/4096 扇区、每命令 4 KiB 数据块；xHCI 使用持有到 controller 安全退出的 64 KiB 暂存对象，精确长度成功才拷回，短包返回 EMSGSIZE，超时不保留调用者地址；`msc_command()` 的每实例锁已由 `spin_lock_irqsave` 改为 sleepable `mutex_t`，因此跨同步 bulk wait 持有它是合规的（块层从进程上下文调用）；probe 失败路径不再对静态 `g_storage` 槽位执行 `kfree()` |
| TPM 2.0 (TIS) | x86 安全 | `tpm.a20drv` 模块：ACPI TPM2 发现、轮询 TIS Startup/GetRandom、可失败停止及 MMIO 保留；FIFO/停止模型与模块构建通过，未认证实体 TPM |
| PS/2 | INPUT / 单ISA控制器 | `kernel/drivers/input/ps2.c` 共享实现，drvmod仅包装；通过hwapi操作端口，经固件接口请求架构IRQ路由；键盘、鼠标输出input class，控制台字符经统一输入路径；单实例与声明端口 claim；受管 stop 核对时钟/IRQ 禁用并排空 dispatch，失败保留绑定及 IRQ，成功后 remove 释放。2026-11-17 QEMU 正常关机读回 0x70；该 Lenovo 普通按键来自 USB，PS/2 不替代 USB 键盘验收。 |
| virtio-console (virtio-serial) | CHAR / TTY backend | PCI `1af4:1043` / `1af4:1003`，raw 模式仅协商 VERSION_1，使用 q0/q1 与端口 0；批量 TTY 模式另协商 MULTIPORT，经 q2/q3 完成 DEVICE_READY / PORT_READY / PORT_OPEN，使用固定端口 1 的 q4/q5。RX 默认 8 × 4 KiB，TX 默认 16 × 4 KiB；[compact 及覆盖参数](../../drivers/classes/virtio-console.md) 可降低内存占用；`tx_lock` 串行化提交与退休，超时或信号不归还设备持有的 DMA 槽。raw 写同步完成，TTY 写返回已复制字节，队列满时有界等待，支持 `VPORT_IOCTL_DRAIN` / `tcdrain`。批量模式通过独立 Unix socket 与有界宿主中继输出，避免 QEMU virtconsole 的短写丢弃。单静态实例、仅一个数据端口，不支持通用多端口枚举/热插拔，无 MSI-X。x86_64 QEMU 已验证；其他架构仅语法检查，不宣称运行时验证。 |
| virtio-rng (VIRTIO_ID_RNG) | CHAR | drvmod 模块（`virtio-rng.a20drv`，riscv64/x86_64 构建），实现位于 `kernel/drivers/char/virtio_rng.c`。**双 transport**：`dev->bus == &pci_bus` 走 `pci_virtio_transport_init(dev, 4, &vt)`（PCI `1af4:1044` modern / `1af4:1005` transitional），其余走原生 virtio-mmio v2 窗口（`RES_MMIO` + `RES_IRQ`，device-id 4），probe 里 magic/version/device-id 三项在两条路径上都读，只是 PCI 传输层自己应答这三个偏移。只协商 `VIRTIO_F_VERSION_1`，队列 0 是唯一的 request queue，8 个槽 × 256 B 与 ring 同在一块 page 对齐 coherent 分配里（描述符地址全为 `dma + offsetof`）。**按需投递**：probe 不挂缓冲；`read()` 发现字节环空才把空闲槽挂进 avail ring 并 notify，used ring 由 ISR（或 read/poll 回退路径）排空进 4 KiB 字节环。之所以不学 Linux 那样常驻挂满：QEMU 的 `virtio_rng_process()` 按 `virtqueue_get_avail_bytes()` 向宿主索取恰好 avail ring 宣告的字节数并填满后停手，常驻挂满等于每次 probe 都在无人读取时持续抽宿主熵。字节环空时 `read()` 发 notify 后**有界**自旋（500 ms，`proc_yield()` 逐轮排空），超时返回 `-ETIMEDOUT` 并计 `read_timeouts`，不 park，因此设备不应答也卡不死读者；等待期间不持实例锁。节点名 `/dev/hwrng` 由 `device_set_devfs_name()` 在 probe 里指定（模块侧 `drv_device_set_devfs_name`，已在符号白名单）。**熵池接线**：内核实有熵池（`kernel/core/random.c` 的 xoshiro `random_reseed()`，消费方是 `sys_getrandom()`、ASLR 与栈保护），本驱动把每次读到的字节按 64 位一组折进去（单次读上限 64 组），为此在 drvmod 白名单里新导出 `random_reseed`；**只走 read 路径，不在 ISR 里喂**（`arch_entropy_sample()` 会取 `proc_current()`）。限制：**单静态实例**（停止失败时保留原 owner；释放前第二个 virtio-rng 函数 probe 返回 `-EBUSY`）、无 MSI-X（只用 PCI INTx / virtio-mmio 共享线）、**熵池只在有读者时才被喂**（不打开 `/dev/hwrng` 的系统完全不会去动这块设备）、字节环满时静默丢弃新到的熵、read 返回的字节数不保证是 256 的整数倍（随设备一次填多少而变）。门禁 `make smoke-virtio-rng`（x86_64 QEMU q35 + `virtio-rng-pci`，`hwrng_test` 读 ≥256 B，断言非全 `0x00`、非全 `0xff`、非单字节重复）**已实跑 PASS**（日志 `.kernel-build/smoke/virtio-rng-x86_64.log`，`HWRNG_TEST: PASS bytes=256`），QEMU 上的实际读出已验证；riscv64 virtio-mmio 路径只做过单文件 `gcc -fsyntax-only`（DRVMOD_CFLAGS）自检，运行时**未验证** |
| PC Speaker | AUDIO | drvmod 模块（`pc-spkr.a20drv`，x86_64），动态 `/dev/audioN`；支持 19 Hz–20 kHz 有界 tone/stop ABI，不冒充 PCM |
| CMOS RTC (MC146818) | 无类（x86 墙钟）| drvmod 模块（`cmos-rtc.a20drv`，x86_64 Early），绑定板级 `cmos-rtc` platform 设备（IOPORT 0x70/0x71）；BCD/二进制两种计数格式、两次稳定快照与跨秒/UIP/模式复核、成功时经 `timekeeping_wallclock_set_hw()` 替换编译期墙钟种子。限制：**只读**（不写任何 CMOS 寄存器，因此没有 RTC_SET_TIME 回写路径）、**无 IRQ**（只在绑定时采样一次，不跟踪秒中断）、**不做时区换算**（CMOS 里是什么就是什么，`-rtc base=utc` 的门禁才使宿主与来客时钟可比）、**century 字节不可信**（读 CMOS 0x32（PC century 字节，非寄存器 C/D）；0x00/0xff、畸形 BCD 或未知世纪按 20xx 处理并打日志，已知世纪的完整日期仍须落在 1970..2100，固件把 84 写成 1984 而 0x32 留 0x00 的机器会被算成 2084）。统一模型里目前没有 RTC class ops（`driver_class.h` 只有 char/block/net/input/display/audio），因此它以 `DEV_CLASS_NONE` 绑定，使用者通过 `timekeeping_get_realtime()`（`/dev/rtc`、`/proc/stat` 的 btime）取时间 |
| Intel HDA | AUDIO | 架构无关 PCI class 驱动；probe 按 GCAP[0] 声明 32/64-bit DMA mask，BDL/PCM 在窗口内分配，越窗仍报 `-EOPNOTSUPP`；已接入受管失败 probe 与可失败停止，确认控制器 reset 和流 RUN 清零后才回收，失败保留绑定/DMA 供重试；x86_64 与 LoongArch64 QEMU 通过 BDL DMA smoke，x86_64 用户态 tone 到 QEMU WAV 验证，RISC-V64 曾在 from-source Wayland/FFmpeg/PulseAudio 路径完成播放（该路径已退役）；`make run-world-gui PKG_WORLD=xfce` 连接宿主音频；支持 48 kHz 双声道 S16_LE、环形 DMA、stop/drain 和用户态 WAV/raw/tone 播放器；本轮生命周期范围见下文 |
| VirtIO Sound | AUDIO | modern PCI/MMIO，48 kHz 双声道 S16_LE；受管失败 probe、关闭取消及 status=0 确认后回收已接入，停止失败保留绑定、IRQ 和全部 queue/control/event/PCM DMA 供重试；x86_64 QEMU PCI 实际播放、drain/STOP/reopen 与自然关机通过，MMIO/失败路径由模型覆盖；详见本轮记录 |
| Sunxi A64/V3s Codec | AUDIO | 48 kHz 双声道 S16_LE 播放；受管失败 probe、写入/drain 取消与可失败停止已接入。DMA、发射器和中断屏蔽读回及电源关闭成功后才回收；失败保留绑定、IRQ、全部映射和 PCM/LLI DMA，重试后可释放。生产模型及 AArch64 模块构建通过，实板/波形与 IRQ 时序未验证；无录音、混音或时钟切换 |
| SG2002 内部 DAC | AUDIO | Nano 固定使能 PA、48 kHz 双声道 S16_LE；受管失败 probe、写入/drain 取消和通道 7 的可失败 abort 已接入。发射器/DAC、两级通道 IRQ 屏蔽和 CHEN 读回后回收，失败保留绑定、IRQ、MMIO 与 PCM/LLI DMA 供重试；通道绑定归属独立于当前流活动状态，不更改 peer。生产模型与 RISC-V64 模块构建通过，实板/模拟输出/缓存与 IRQ 时序未认证 |
| RK817 / RK356x I2S / PL330 | AUDIO | 固定 48 kHz 双声道 S16_LE 耳机播放、16→32 位 FIFO 扩展；受管失败 probe、等待取消与可失败停止已接入，独占指定 PL330。manager/channel/fault/IRQ 屏蔽和 codec 原镜像恢复确认后才回收；失败保留绑定、两个 IRQ 登记、映射、PCM/程序 DMA 及 codec 快照。生产字节码/电气模型及 AArch64 模块构建通过，实板/模拟波形/真实 DMA、IRQ 与供电恢复未认证 |
| STM32 SDIO | BLOCK | 统一类 + MCU bridge；板级 bus 仍用名称匹配 |
| STM32 简单外设 | 允许例外 | 板级轮询轻量 API，不强制统一对象；扩展到多实例/用户 ABI 时必须迁移 |
| StarFive/LS2K GMAC | 有条件 | per-instance 私有锁与轮询，要求 embedded 部署；支持范围及实机状态以各平台文档为准 |
| DW-MSHC SD | BLOCK | VF2、Quartz64 Zero 的固件节点绑定；每设备私有状态、64 位 MMIO、单线 PIO、SDSC/SDHC/SDXC、CSD 容量与写保护、固件/构建只读、有界等待和错误回收。IRQ 可选，短轮询后在调度器可用时等待事件；错误返回后卡离线，不宣称自动恢复。板级时钟与管脚在 arch，宿主生产代码测试及交叉构建通过；实机未验证 |
| sunxi MMC0 SD | BLOCK | PINE A64、V3s 的真实固件节点；400 kHz 单线 PIO、SDSC/SDHC/SDXC、非对齐缓冲区、读写保护、flush、超时、受管失败 probe 与可失败停止；复位/时钟更新确认后回收，失败保留绑定/MMIO。embedded 共用实现，AArch64 提供 early 模块；CCU/reset/pinmux 在 arch。生产模型测试及交叉构建通过，实机未验证；无 MMC/eMMC、SDIO function、DMA、高速和热插拔 |
| SDHCI SD | BLOCK | Nano SG2002、Ox64 BL808 的真实固件节点；单线 PIO、SDSC/SDHC/SDXC、非对齐缓冲区、写卡完成等待、写保护/只读与电压检查，出错后停止 I/O。受管失败 probe、在途取消与可失败停止已接入，reset/IRQ mask/时钟关闭确认后回收，失败保留绑定/MMIO；板级时钟/pinmux/PHY/脉冲在 arch；生产模型测试及交叉构建通过，实机未验证；无 eMMC、SDIO function、DMA、UHS 和热插拔 |
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
- HDA 使用播放期间的实例私有 progress work 回收已播放数据并在耗尽后停流，write/drain 仍主动采样。10 ms 是调度目标，不是硬实时保证；未启用 period IRQ，不保证整圈以上调度停滞。virtio-sound completion 仍由 write/drain 路径轮询。
- QEMU RISC-V64 使用 coherent DMA 假设；移植到非一致性缓存硬件前必须实现真实的 `dma_sync_for_device()`。


## 2026-11-16 USB 拔出验收

N13 已按允许的确定性 fake HCD 接缝关闭。`python3 -m unittest tools.tests.test_usb_disconnect_tree -v` 执行真实 driver-core、USB core、hub remove 与地址池代码，硬件停止/URB 后端为 fake HCD；64 轮父 hub 与下游设备各带一个在途 URB。下游 stop 返回 EBUSY 时，两级 owner 与地址均保留，地址池耗尽时不能提前复用；重试成功后两个 URB 撤销，延迟事件不触发旧回调，两个地址按原编号再次分配，注册 HCD、设备、分配计数均归零。ASan/UBSan 通过。

三个相同输入的负控分别移除 slot abort、地址释放、下游 HCD 注销，均被断言或 sanitizer 拒绝。记录为 `build/evidence/v1-closeout-20261116/usb-disconnect-tree.log`。已有真实 xHCI 函数的 endpoint/锁外 callback/bulk/部分枚举失败测试继续覆盖其各自的停止与调用者存储边界。本轮不扩大到实机热拔、其他 HCD 或 SMP 认证，也不关闭 storage/BOT 与厂商设备的剩余资源回收需求。

### 关机与 kexec 的可失败设备停止

2026-11-16 提交 `3e02ee9aa`、`46aa30689`。`driver_core_shutdown_all()` 返回 int，复用已有 prepare、consumer-first、execution stop、quiesce、class drain、remove 管线。它不再执行无法报告错误的旧 void shutdown；没有 remove 协议的绑定在预检阶段返回 EOPNOTSUPP。成功关闭绑定后才释放 devres/依赖，失败保留原绑定和存储，重试继续未完成部分。终止阶段开始后，新 driver/device/bus 注册、probe 与 dependency 返回 ESHUTDOWN；已成功停止的设备不会自动恢复。

Linux/Native reboot/poweroff 检查该结果，停止失败不调用 firmware。kexec 在关闭文件系统并停止设备后，才由架构后端停其他 CPU，避免设备回调等待已被停下的 CPU。没有架构后端时在文件系统/设备变化之前拒绝。后端后续准备失败仍返回错误并取消其资源；已经关闭的文件系统和已停止设备不回滚。

仅运行两项针对性主机检查：现有 `test_driver_remove` 的终止场景在生产 C/Rust core/class 下覆盖缺少协议、在途调用排空、停止错误、DMA/资源保留、禁止新绑定与重试；`test_driver_terminal_entry` 执行提取的生产 kexec/Linux/Native 控制流，检查失败阻止交接及 CPU 停机顺序。结果和源码哈希在 `build/evidence/v1-closeout-20261116/driver-terminal-stop/report.json`。没有完整回归或新增 guest 认证；厂商 stop 的实际硬件完成证明、活跃请求取消和持久显示 backing 仍需按各自条件完成。公开 device/driver 布局及模块 ABI 12 不变，保留旧 void 字段但终止路径不调用它。

### VirtIO 块设备与网卡的复位确认

2026-11-16 提交 `d528c8958`。两条 remove 路径原先写 STATUS=0 后便交还 IRQ/状态，网卡还立即释放 ring；现在使用现有 `virtio_transport_reset(..., 100)`，读回 STATUS=0 才允许后续退休。超时返回 ETIMEDOUT 并保留 `drv_priv`、IRQ、MSI-X 与 DMA 队列，原绑定可显式重试。离线网卡仍确认迟到中断，但不再进入网络栈，避免持续拉起共享中断源。

仅运行 `python3 -m unittest tools.tests.test_virtio_stop_ack` 一项针对性检查：编译生产 reset helper、block/net remove 与 net IRQ 函数，寄存器/时钟模型覆盖超时保留、迟到中断、延迟确认后的同实例重试，以及 INTx/MSI-X 清理顺序。结果和源码哈希在 `build/evidence/v1-closeout-20261116/virtio-stop-ack/report.json`。这不认证实体设备；网卡受管失败 probe 和槽位复用已按下节接入，块设备受管失败 probe 也已按下节接入；块设备整次 I/O 引用、class drain 前 stop 和停止后 DMA 回收见下节；块设备旧元数据已按下节消费引用协议退休，其他驱动的活跃停止与私有入口竞争仍保留。

### VirtIO 网卡失败 probe 的 owner 与槽位

2026-11-16 提交 `49e3b2772`。网卡 probe 在接触 transport 前选择 `device_probe_use_remove`，槽位由该绑定的 device 指针保留，未确认复位的失败实例不能被另一 probe memset。第二条队列设置失败不再立即释放已经可能交给设备的 ring，而由受管 remove 确认复位后清理。未验证的 transport 不发起 reset；已验证 transport 的初始 reset 有界等待，失败不继续协商或分配 ring。清理成功后才交还槽位，正常解绑/重绑不再耗尽静态容量。私有编号入口新增发布状态检查，probe 初始化完成前不可用。

针对性检查编译生产私有布局、claim/probe/init/remove/reset/IRQ，以 transport、队列设置、分配及核心 adapter 替代边界检查部分队列失败、停止超时后的 owner 保留、另一 probe 的隔离、超过槽数的串行复用、早期 transport 失败和初始 reset 超时；与既有 stop ACK 检查共两项通过。发布原子性调整后只重跑受影响的一项，记录在 `build/evidence/v1-closeout-20261116/virtio-net-probe/report.json`。未跑完整回归或 guest。该结果不覆盖任意私有按编号调用的并发寿命、class drain 前的活跃请求取消，或其他驱动的失败 probe。

### VirtIO 块设备失败 probe 的 DMA 保留

2026-11-16 提交 `ff4ab1474`。块设备 probe 在 transport 前选择受管清理并保留槽位 owner，初始化在队列建立后报错不再直接释放 DMA。未验证的 transport 清理不写 reset；已验证 transport 清理必须确认复位，超时继续持有实例、IRQ 与 DMA。未发布的失败实例成功清理后释放 DMA 并交还槽位，不能被另一 probe 提前 memset；ready、批写与旧编号入口拒绝未发布实例。

已发布过的 `block_dev_t` 可能仍被旧缓存借用，因此保留其静态元数据，不让槽位改指向新磁盘。DMA 已按下节的整次调用引用与可失败 stop 在确认后退休；旧元数据消费引用和槽位再利用已按下节接入。

仅运行失败 probe 新检查，以及直接受影响的 stop ACK/批写检查，共三项通过。新检查编译生产布局/claim/probe/init/remove/free-DMA/reset/ready，以设备初始化、分配、transport 和核心 adapter 的替代边界验证队列暴露后的失败、复位超时保留、满槽拒绝、超过容量的失败清理复用、早期 transport 失败和已发布元数据不改指向。记录在 `build/evidence/v1-closeout-20261116/virtio-blk-probe/report.json`。未跑完整回归或 guest，不认证活跃 DMA 请求取消、实体设备或消费引用协议。

### VirtIO 块设备活跃调用与 DMA 退休

2026-11-16 提交 `ef546cce9`。probe 在硬件初始化前登记已有 execution stop，先于 class active-call drain。读写覆盖整次多段传输，flush 与批写也持有同一实例的 active call；stop 关闭新准入，有活跃调用时立即返回 EBUSY，保留 valid/IRQ/poll 完成路径及 DMA，避免锁内等待。停止后禁止新的 request 分配，批写会 kick 并等待已经提交的前缀，再返回关闭错误。调用引用归零后重试 stop 才确认 reset 并退休 IRQ；remove 在 class drain 后释放 DMA，旧 block_dev 元数据保持关闭身份，后续 I/O 在访问 DMA 前被拒绝。

未发布槽位重建保留原子发布守卫，ready/poll/I/O 先检查发布再进入实例锁，防止对失败槽位空洞的初始化中状态作准入。此项代码没有增加设备容量或跨解绑数字编号权限。

仅运行现有 stop ACK、失败 probe 和批写三项相关检查，覆盖忙时保留、整次多段 read/write/flush 引用、批写关闭后的前缀回收、确认后 DMA 释放及旧元数据不改指向，均通过。结果和源码哈希在 `build/evidence/v1-closeout-20261116/virtio-blk-active-stop/report.json`。使用生产所列函数与替代 MMIO/request/core-adapter 边界，未作 guest/实体设备或任意 SMP 认证。旧块设备元数据的消费引用/槽位回收已由下节补齐；其他驱动活跃请求和超时恢复边界仍保留。

### VirtIO 块设备消费引用与槽位复用

2026-11-16 提交 `70a2e27ea`。`virtio_blk_get_dev()` 每次成功返回一个消费引用，调用方持有它直至所有缓存/I/O 使用结束，再调用 `dev->release(dev)`；bcache 仍借用设备，不隐式替调用方归还该引用。解绑后旧引用保留同一个关闭身份，read/flush 等入口返回 ENODEV，不跟随重绑；只有绑定已退休且消费引用归零，槽位才可复用。按编号的整次 I/O、capacity/ready 查询和 poll_all 也短暂固定元数据，查询与槽位重建由专用 spinlock 协调，不与实例锁嵌套。callback 表在发布前一次安装，不在 getter 里并发重写。

原静态容量继续限制同时在线或仍被引用的实例；已结束的绑定不再永久耗尽槽位。引用归还后不能继续使用旧指针。公开 block_dev 布局、release 回调形状与模块 ABI 12 不变，getter 的拥有引用约定写入头文件。

只运行受影响的 stop ACK/调用引用与失败 probe/消费引用两项检查，均通过。覆盖两个消费引用跨解绑保留身份、旧引用 I/O 拒绝、第一个 put 不交还槽位、最后一个 put 才复用、超过容量的成功重绑与 DMA 分配/释放相等；报告在 `build/evidence/v1-closeout-20261116/virtio-blk-consumer/report.json`。使用生产所列函数和替代 MMIO/request/core-adapter 边界，未扩大到实体设备、任意 SMP 压力或所有调用方集成。树内没有该旧 getter 的实际调用方；将来使用者必须遵守引用配对契约。块设备元数据最后消费引用和槽位复用的软件缺口已移除，其他 provider 与整体发布验收保持原范围。

### VirtIO 网卡活跃收发与停止

2026-11-16 提交 `4d6331089`。网卡在 probe 时登记 execution stop；直接 send/recv 在整个调用期间保留实例，准入和槽位复用共用锁。stop 关闭新准入，有活跃调用时返回 EBUSY，保留有效完成路径、IRQ 和 ring；调用退出后重试，确认 reset 后才退休资源。等待中的发送观察 closing 后退出，已提交 TX 即使超时也不清 busy，直到真实 completion 或确认复位后回收。

仅运行既有 stop ACK、网卡失败 probe、共享模型受影响的块设备失败 probe 三项检查，均通过（约 0.6 秒），覆盖忙时保留、关闭准入及退出后清理/复用。没有新增测试矩阵；这些检查未执行完整收发数据路径。其他 provider 和发布验收仍保持原范围。后续日常修改只运行直接相关检查，完整回归留到发布候选阶段。

### USB storage 实例、调用和端点回收

2026-11-16 提交 `46cab0d4a`。storage probe 在端点配置前选择受管失败清理、保留 device owner，并登记 class drain 前的 execution stop；配置部分成功后出错不再 memset 实例。stop 关闭新准入，有整次读写调用时返回 EBUSY，待调用退出后可重试交还接口端点。端点清理失败保留实例、接口与存储，确认成功并排空 class 后才退休绑定。重绑先 SET_INTERFACE 恢复设备端状态。

`usb_storage_get_dev()` 每次成功交付一个消费引用，调用方跨 cache/I/O 保留，结束后调用 block_dev.release；旧引用跨解绑保持关闭身份并拒绝 I/O，最后释放后槽位才可复用。ready 查询不取得引用；树内没有该 getter 的实际调用方。既有容量及 ABI 12 不变。

只运行 `tools.tests.test_usb_storage_lifetime` 一项定向检查，1.063 秒通过。执行生产 storage、driver core 和 USB core，控制器、锁、分配及 class 发布后端为模型；覆盖部分配置/端点清理失败重试、活跃多段读写阻止停止、两份消费引用跨解绑、最后释放及重复成功重绑。移除 storage 静态实例和端点回收的软件缺口；BOT 错误恢复、其他 HCD/厂商协议与发布验收仍保留。

### USB storage 串行 reset recovery

2026-11-16 提交 `b0f5654db`。原传输失败路径先释放 BOT mutex 再发 reset，且忽略 reset 结果；现在从命令到恢复始终持有同一 mutex。先确认交还两个 host 端点，再依次发 Bulk-Only Reset、Clear HALT IN、Clear HALT OUT，全部成功后重新配置端点。任何步骤失败保持 recovery_pending，下一调用在发新 CBW 前重试恢复；运行期使用 lifecycle try-lock，忙时保留 pending 返回，不与生命周期回调互等。probe 已持有生命周期时使用嵌套所有权。失败命令保持原错误，不自动重放写入。

CSW 的签名、tag、residue 上界、保留 status 以及 Phase Error 都先验证，再决定是否恢复；合法 status=1 仍为命令失败，合法非零 residue 不冒充完整读取。IN 数据阶段不再从尚未初始化的目标缓冲区复制输入。控制请求顺序依据 [USB BOT 1.0 §5.3.4](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf)。仅新增常量和私有状态，ABI 12 不变。

扩展既有 `tools.tests.test_usb_storage_lifetime`，只运行这一项，1.097 秒通过。生产 storage/core/USB 配控制器及同步模型，覆盖 malformed/Phase Error CSW、普通命令失败、失败恢复阻止新 CBW、部分 host 重建重试、probe 恢复和运行期生命周期忙的延后处理；既有寿命场景保留。记录在 `build/evidence/v1-closeout-20261116/usb-storage-reset/result.txt`。尚未完成短包/STALL 后数据与 CSW 阶段处理，BOT 的相应缺口继续保留。

### USB storage 短包与 STALL 阶段

2026-11-16 提交 `db916acc2`。URB 新增 actual_length，同步 bulk 保留请求 len，完整返回 0，短包返回 EMSGSIZE 并只交付已传输前缀，STALL 规范为 EPIPE；失败/新提交清零旧计数。URB 公开布局增长，模块 ABI 升至 13，旧模块必须重建，不能按旧大小分配 URB。xHCI 仍使用原有保留暂存区，非法 residue 不交付数据。

storage 遇数据短包时停止数据阶段并读取当前 CSW；遇数据 STALL 时交还对应 host 端点、Clear HALT 并重建，再读取 CSW。首次 CSW STALL 只清除该 IN 端点并重试一次，第二次失败或短 CSW 进入完整 reset recovery。短数据、有效失败状态和非零 residue 不冒充整块读写成功；恢复失败仍阻止新 CBW，不自动重放写入。阶段顺序依据 [USB BOT 1.0 Figure 2 / §5.3](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf)。

只运行 storage 集成、xHCI 正常/短包/超时保留三项及 descriptor ABI guard，共五项，2.177 秒通过。检查包含实际长度/未写尾部、IN/OUT 数据 STALL、CSW 单次重试/重复 STALL/短 CSW、恢复失败重试，以及旧 ABI 12/未来 14 拒绝。生产路径配模型边界，未新增 guest、实板或全候选重建认证。记录在 `build/evidence/v1-closeout-20261116/usb-storage-phases/result.txt`。从缺口表移除已完成的 BOT 短包/STALL 阶段和 actual_length；多 TRB、其他 HCD/厂商协议及发布配置验证继续保留。

### xHCI 多 TRB bulk 的完成与保留

2026-11-16 提交 `532cedf75`。保持 64 KiB 请求上限与同一份 retained DMA buffer，分配对齐改为页；物理范围跨 64 KiB 边界时拆成最多两个 Normal TRB。设置 CH、TD 剩余 packet count 和尾段 IOC，Link 跨环时继承 CH，首段 cycle 延后到全 TD 就绪才发布。首段短包/错误也匹配同一 TD，实际长度按完成 TRB 的本段 residue 加上先前已完成长度计算，拒绝不属于该 TD 的事件。规则依据 [xHCI 1.2b §4.10.1、4.11.2.4、6.4.1、6.4.2.1](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf)。模块 ABI 13 不变。

暂存区不借用 caller 存储，首段 STALL 与整 TD 超时后仍使用既有确认停止/错误保留管线，失败停机不交还任何分段可达的存储。五项不同的直接相关方法分次通过；新增链路场景最初因模型帧恰好 64 KiB 对齐而未触发分段，改为有效非 64 KiB 对齐页后通过。随后补零指针事件拒绝，只重跑链路与共享 EP0 匹配两项，1.246 秒通过，复用其余三项。生产 allocator owner/ring/matcher/bulk/core 配硬件与锁模型，未跑 guest/实板或全套。记录在 `build/evidence/v1-closeout-20261116/xhci-bulk-chain/result.txt`。移除该多 TRB 软件回收缺口，其他 HCD/厂商停止证据和发布配置验证仍保留。

### Hub 在子设备退出前关闭准入

2026-11-16 提交 `ef5d9294b`。当前树的 HCD ops 实现为 xHCI 与 hub 转发层，没有另一个尚待迁移的物理 HCD。hub 原先在 remove 中才设置 stopping，晚于核心 consumer-first 退出；子设备解绑失败时仍可能扫描/重装状态 URB。现在 probe 在端口/端点初始化前登记 remove prepare，先设置 stopping；核心 PREPARING 状态也使 USB 扫描与枚举拒绝新使用。prepare 不释放存储，不把软件关闭当作 DMA 停止。

下游退出失败继续保留 hub HCD、协议图与上游状态 URB；slot abort 失败继续保留其端口；上游 endpoint 交还失败保留 hub 和 callback，直至确认成功后才释放。remove 的关闭动作仍可重复执行，部分 probe 也由同一受管管线清理，公开 ABI 13 不变。

仅新 hub 集成检查与既有 hub endpoint 失败重试检查两项分次通过。新检查执行完整生产 hub probe/prepare/remove、USB core 与 driver core，模型注入上述三个失败点；既有夹具补齐前轮 ring 的 CH 常量后只重跑该项，1.167 秒通过。结果在 `build/evidence/v1-closeout-20261116/usb-hub-prepare/result.txt`。移除“其他 HCD 回收”的未实现描述，当前实现的硬件停止/支持配置验收仍按发布条件，厂商停止证据、旧驱动迁移和显示映射安全继续保留。

### GEM backing 已实现范围核对

2026-11-16 核对当前 `drm.c` 的 detach/drop_storage、retained retry 和 reaper：外部 GEM backing 的软件保留已实现。resource unref 或 remove 失败不释放原 VMO/帧；offline 不等于停止。成功 remove 的旧 publication 停止凭据才允许无驱动回调回收硬件引用；dma-buf pin、普通 VMA VMO 引用各自保留 CPU backing，最后引用后释放。该范围已经在现状页记为可用，不应继续以整条“外部 GEM backing”未实现重复列出。

只运行既有 `test_failed_attach_keeps_published_backing_until_stop` 与 `test_exported_dmabuf_keeps_cpu_storage_after_binding_stop` 两项，3.399 秒通过；执行实际 DRM/core/class，VFS/MM/设备回调为模型，模型在最终释放帧时拒绝仍被未停止设备引用的页面。结果记录在 `build/evidence/v1-closeout-20261116/drm-backing-status/result.txt`。没有扩大实体硬件停止认证，厂商停止证据仍在独立缺口；旧 get_fb 持久物理 VMA 还没有跨解绑的资源持有关系，本轮明确保留。

### 物理 VMA 的可选资源引用

2026-11-16 提交 `84d122b95`。MM 新增可选 `mm_mmap_physical_owned`：成功 VMA 持有独立 resource owner，所有分配/放置/页表失败均不取得引用。物理映射沿用现有 eager/Rust 事务、DONTFORK 与权限/缓存/地址规则，拆分/复制的辅助持有路径增加引用，最后 put 在 TLB 完成和延迟 VMA 释放后执行。使用与文件互斥的 union 指针槽及新 VM_PHYSICAL_OWNER 位，不扩大映射记录布局；文件诊断与 procfs 按 VM_FILE 判断，避免解读资源 owner 为 vfile。

仅两项定向检查通过，0.666 秒：覆盖元数据/PTE/冲突失败、持有、复制记录、局部 unmap 和 TLB 后最终释放，既有 fbdev/udriver 借用路径保持；实际 Rust policy/C bridges/VMA 辅助引用配页表、拆分记录和 TLB 模型。直接用内核头文件语法编译所改 vma/mmap/fault/trap/procfs_render 也通过，未做全内核构建。记录在 `build/evidence/v1-closeout-20261116/physical-mapping-owner/result.txt`。

这是显示修复所需的 MM 持有接口，GPU 绑定 owner、提前退出的忙检查和 fbdev 消费者尚未接入；不删除旧 get_fb 物理 VMA 的缺口，也不以该接口代替完成。

### 旧物理 framebuffer VMA 的 GPU 资源持有

2026-11-16 提交 `e9933597f`。每个借用物理 scanout 的绑定在 probe 前段创建 kernel-owned mapping owner，通过既有组合协议发布，并登记 pre-consumer prepare。get 只在当前 publication 的有效 access 内取得资源引用；prepare 关闭准入，存在映射/捕获引用时返回 EBUSY，保留绑定、devres 和代码，不进入 class drain。最后 VMA 的 TLB/deferred 退休归还引用后，显式重试退出才释放绑定。内核 finalizer 不依赖旧模块回调。

fbdev mmap 和 FBIO_MAP_FB 已改用 mm_mmap_physical_owned，映射错误归还捕获引用。现有 simplefb、lsdc-boot、vmsvga 都在硬件使用前接入；VMSVGA 提前保留部分实例并拒绝覆盖已占用静态槽位，早期无 regs 清理不访问寄存器。VirtIO owning RAM scanout 沿用原 VMO 路径；物理映射仍按原 DONTFORK/NOMMU 边界，不新增映射权限。模块 ABI 13 不变，新增两项 helper 导出。未接入 owner 协议的树外物理 provider 需在 probe 中安装，映射入口否则返回 EOPNOTSUPP。

只运行三项定向检查，2.309 秒通过：生产 GPU/core/class/protocol 的 EBUSY/最后引用/重绑，以及真实 fbdev/MM Rust/C 的 mmap/ioctl 持有、失败回滚和 TLB 后释放。两类检查分别在硬件/页表/TLB 模型边界验证同一引用契约，未声称全 guest/实体 GPU 或任意 SMP 验收。修改过的五个 GPU 源文件直接内核头语法编译通过。记录在 `build/evidence/v1-closeout-20261116/gpu-physical-owner/result.txt`。已删除旧 get_fb 物理 VMA 软件寿命缺口；厂商真实停止证据、旧驱动迁移和整体发布条件继续保留。


## NVMe 失败 probe 与确认停止（2026-11-17）

旧 remove/失败 probe 在清除 CC.EN 后忽略等待结果，仍释放 IRQ、队列和 DMA。
现在在启动硬件前接入 device_probe_use_remove / execution action，并提前保存部分
控制器上下文。停止先关闭新 I/O；io_lock 忙时返回 EBUSY，不进入 class drain。
运行中的多段调用不再提交下一条命令。确认 CSTS.RDY=0 后才同步退休 IRQ，class
调用排空后释放存储；超时或全 1 寄存器保留上下文、IRQ、DMA 和 devres，供显式重试。
CFS 不代替停止确认；disable 时 RDY=0 可确认停止，enable 仍拒绝 CFS。

一个定向生产函数模型检查通过：部分队列、失败 probe、多控制器隔离、活跃 I/O、
停止超时/失联、保留与重试、CFS+RDY=0 和元数据分配失败。MMIO、DMA allocator、
锁及 core adapter 为模型，不能据此认证实体硬件失效。生产 x86_64 dev-build 通过。
QEMU q35/KVM、ABI=both SMP4 的独立 16 MiB NVMe 空盘完成 12 KiB 跨段写读、
原生 block flush、65 次读取和正常 OpenRC 关机，29.515 秒；临时停止日志实际确认
RDY=0。QEMU 自然退出 0，ext4/FAT 检查 0，宿主离线核对 scratch 数据一致。
诊断日志随后删除，字节对照确认没有其它源码变化；最终模型检查和生产构建通过，
未重复整轮 guest。诊断运行中的末尾持久内核日志追加被关闭写门以 -108 拒绝，
没有据此宣称全部日志持久化通过。完整证据为
`build/evidence/v1-closeout-20261116/nvme-stop/report.json`。

NVMe 的这部分软件迁移与正常 QEMU 停止不再作为未完成项；其它旧驱动的失败 probe
迁移及厂商硬件停止失败证据仍按驱动缺口保留，未关闭整个 V1-DRIVER。

## AHCI 失败 probe、停止与旧块引用（2026-11-17）

修复失败 probe/remove 忽略停止结果并释放 DMA，以及下一次 probe 覆盖静态端口状态的
路径。硬件初始化前登记受管失败清理与 execution stop；先关闭新 I/O，有在途调用或
ahci_get_dev 消费引用时返回 EBUSY，保留整个绑定。getter 每次成功取得一个引用，
调用方必须在 cache/I/O 使用结束后调用 block_dev.release；树内没有旧 getter 调用方。
旧引用在停止后拒绝 I/O，最后 release 后显式重试才能解绑和复用静态实例。

停止按 ST=0 → CR=0 → FRE=0 → FR=0 逐步确认；超时或全 1 MMIO 返回错误并保留
上下文、IRQ、DMA 与 devres。确认停止后同步退休 IRQ，class drain 后 remove 才释放
存储。元数据分配、MMIO 映射、固件交接与无可用端口等早期失败使用同一清理管线，
未选中端口时不把 host 寄存器当作端口访问。公开模块 ABI 13 不变。

仅新增一个定向生产函数模型检查（0.370 秒）并运行既有 PCI policy 两项检查
（0.145 秒），以及 x86_64 dev-build；均通过。生产 probe/submit/rw/stop/remove/get/put
配 MMIO、锁、DMA allocator、IRQ 和 core adapter 模型，覆盖失败保留、重试、在途
多段调用、静态实例隔离、旧引用及部分初始化，不据此认证实体厂商硬件失效。

一次 QEMU q35/KVM、ABI=both SMP4、真实 XFCE/OpenRC 私盘运行，16 MiB SATA
空盘完成 66,048 字节跨 64 KiB 读写与原生 block flush，18.426 秒后自然关机退出 0；
ext4/FAT 检查 0，宿主离线核对 scratch 数据相符。未加入停止诊断日志，也未重复
guest/全量/平台矩阵；实际 CR/FR 停止确认的错误注入仅由模型验证。证据为
`build/evidence/v1-closeout-20261116/ahci-stop/report.json`。

AHCI 的上述软件迁移从剩余回收范围移除；其它旧驱动与实体硬件失败停止证据仍保留。

## VirtIO-SCSI 失败 probe、超时 DMA 与停止（2026-11-17）

修复失败 probe 未保留 drv_priv/静态槽位、remove 写 reset 后不确认停止，以及超时
返回后仍将调用者缓冲区暴露给设备的路径。初始化前选择受管失败清理并登记 execution
stop，部分 queue 发布失败保留所属槽位。stop 关闭新准入，在途命令或 getter 引用
未退出时返回 EBUSY；等待 transport status=0 后才同步退休 IRQ/MSI-X，class drain
后释放私有 DMA 缓冲区与实例身份。失败时保留绑定、队列、IRQ、DMA 和 devres 重试。

每个控制器持有 64 KiB coherent bounce；原有单次最大 65,535 扇区的读写按缓冲区
分段，设备不直接使用调用者数据或 probe 栈缓冲区。超时或未知完成编号/索引关闭
新提交，禁止覆盖未确认交还的链；正常返回只复制已成功完整读取的数据。检查 residual、
sense 长度与现代 used 长度；合法 Unit Attention 可重试，QEMU 的 used 长度包含
data-out 的计数得到兼容，legacy 沿用其忽略 used 长度的规范边界。

virtio_scsi_get_dev 每次成功取得消费引用，最后使用后调用 block_dev.release；引用
未归还时保留整个绑定，不能用新绑定覆盖旧身份。ready 查询不取得引用；树内没有
getter 的实际调用方。现有四控制器/target 0 LUN 0 范围与模块 ABI 13 不变。

仅一个新增生产函数模型检查（最终 0.400 秒）与 x86_64 dev-build 通过。模型包含
真实 queue setup/command/probe/stop/remove 与 transport reset；MMIO、锁、分配、
IRQ 与 core adapter 为替代边界。覆盖迟到 DMA 不触及已释放 caller、超时链冻结、
停止失败保留/重试、部分 queue 失败、静态实例隔离、旧引用、非法完成和 Unit Attention。

首轮实际写入被过严的新完成长度上界拒绝，报告为 FAIL，已保留。核对 QEMU 实现并
修正后，仅复跑该私盘路径：QEMU q35/KVM、ABI=both SMP4、XFCE/OpenRC，16 MiB
SCSI 空盘完成 66,048 字节跨 64 KiB 写读、原生 block flush，49.612 秒自然关机退出 0；
ext4/FAT 检查 0，离线逐字节核对相符。未扩大全量/平台矩阵；故障停止和迟到 DMA
仅由生产函数模型注入，不声称实体硬件认证。证据索引为
`build/evidence/v1-closeout-20261116/scsi-stop/report.json`。

VirtIO-SCSI 的上述软件迁移从剩余回收范围移除；其它旧驱动和厂商硬件停止证据保留。

## RTL8139 失败清理、DMA 窗口与接收游标（2026-11-17）

probe 在硬件使用前登记受管失败清理与 execution stop，并立即保留静态实例 owner。
stop 先关闭新收发/轮询准入，ring lock 忙时返回 EBUSY；锁内调用退出后，在锁外
请求软复位，最多 1,000 次、每次间隔 10us 检查 RST/TE/RE 均清零。全 1 寄存器
或超时保留绑定、IRQ、缓冲区和 devres；只有确认停止后同步退休 IRQ，class drain
后才释放存储并允许复用实例。关闭中的 IRQ 仍确认 pending cause，避免共享线反复
触发，但不再进入网络栈。模块 ABI 13 与单实例范围不变。

实际 4 GiB guest 暴露普通 kmalloc 选择 4 GiB 以上物理内存、导致无 NET 接口的旧
缺陷。缓冲区改用已有的 coherent aligned helper，按 32-bit mask 选择可达页，并以
DMA handle 加偏移编程 RBSTART/TSAD；不截断普通内核指针。随后下载超时复现了
另一个旧缺陷：RX 游标向下对齐会读偏下一帧。现按四字节向上对齐，先检查 BUFE，
避免空环中的旧 header 被再次消费；环绕复制与丢弃策略保留。

一个定向生产函数模型最终 0.380 秒通过，执行 probe、buffer allocation、IRQ、收发、
poll/stats、reset/stop/remove；MMIO、帧分配、IRQ、锁和 core adapter 为模型。覆盖
在途调用、复位/启用位不退、寄存器失联、失败实例不可覆盖、重试、部分初始化、
关闭后 IRQ 确认、非四字节帧、数据环绕和空环旧 header。新增 RX 断言在修复前实际
失败，修复后通过。最终 x86_64 dev-build 通过，未扩大全套检查。

前两轮实际 guest 分别因 DMA 窗口和 RX 游标失败，原报告/源码快照保留。最终一轮
QEMU 10.0.13 q35/KVM、ABI=both SMP4、XFCE/OpenRC，以 RTL8139 为唯一网卡，
与仅绑定宿主 loopback 的 HTTP 服务各完成一次 128 KiB 下载/上传，双方哈希一致；
29.291 秒正常关机退出 0，ext4/FAT 检查 0。未重复浏览器/游戏或平台矩阵，未声称
实际故障复位或实体硬件认证。证据为
`build/evidence/v1-closeout-20261116/rtl8139-stop/report.json`。

RTL8139 的本轮软件迁移、DMA 窗口和 RX 正确性缺口已关闭；其它旧驱动及厂商故障
停止证据仍按剩余清单保留。

## E1000 受管停止与发送完成归属（2026-11-17）

probe 在 MMIO/硬件使用前选择受管失败清理、保留每设备 context，并登记 execution
stop。初始化先停止旧数据面，缓存/恢复已验证的 firmware MAC，再配置新队列；失败
不再直接释放 context。停止关闭 send/recv/poll 的新准入，ring lock 忙返回 EBUSY，
旧 IRQ 继续确认 ICR，但关闭后不再回收队列或进入网络栈。确认硬件停止后同步退休
IRQ/MSI/MSI-X，class drain 后才释放 ring/context；失败保留资源和绑定，可显式重试。
原来丢弃错误的 void shutdown/remove 分支已删除，模块 ABI 13 不变。

82574L 设置 GIO Master Disable，最多 1,000 次、间隔 100us 等待 STATUS 的 master
enable 清零，并确认 RX/TX enable 均为零；忽略引擎 disable 的设备保留原绑定，避免
重绑恢复 master 时重新启动旧队列。旧 PCI 型号按厂商序列等待 10ms，再请求 MAC reset，
等待 5ms 后检查 RST 与引擎 enable 位；寄存器失联或超时返回错误。队列长度不是
DMA-pending 状态，QEMU 的旧 e1000 也保留 RDLEN/TDLEN，不以其清零作为停止条件。

发送回收已从读取 TDH 改为实际 RS/DD 写回：只有 DMA 同步后观察到设备设置 DD，
才推进 tx_done；不再由软件伪造已完成状态。修复 TDH 读取可能先于缓冲写回的竞态，
同步更新旧状态表中的归属说明。probe、IRQ/轮询和释放均沿用同一个队列 owner。

新增生产函数模型与既有中断协商检查初次联合通过（两项 0.588 秒，协商含十场景）；
最终 DD acquire 读取的定向模型 0.417 秒通过，x86_64 dev-build 通过。实际 probe、
allocator、send/recv、IRQ、stop/remove 配 MMIO、锁、分配与 core adapter 模型，覆盖
两代停止失败/失联保留、重试、在途调用、部分初始化、不同设备隔离与 DD 迟到。
旧 TDH 回收逻辑在新增断言上实际失败，原报告保留。

同一 q35/KVM、ABI=both SMP4、XFCE/OpenRC 双网卡工作负载：82540EM 与 82574L 使用
en2/10.0.2.15 与 en3/10.0.3.15，各完成一次 512 KiB HTTP 下载/上传，四份宿主 receipt
与 guest 哈希一致。首轮 29.552 秒通过，补齐 PCIe 引擎关闭确认后仅复跑此路径，最终
29.306 秒自然关机退出 0，ext4/FAT 检查 0。本次两块网卡实际
均走 INTx，MSI/MSI-X 停止与协商只由模型覆盖，不声称本轮实际 MSI-X 数据面认证。
没有重复浏览器/游戏、全套或平台矩阵。证据为
`build/evidence/v1-closeout-20261116/e1000-stop/report.json`。

E1000 的受管失败清理、停止保留与 TX DMA 交还的软件缺口关闭；其余 provider、
实体硅片/复位勘误证据与正式候选验收继续保留。

## HDA 失败 probe、PCM 取消与确认停止（2026-11-17）

修复 release 在 quiesce 失败时仅保留 DMA、remove 却清空 drv_priv 并返回成功的路径，
避免 core 提前回收 MMIO、devres 或模块。probe 在映射/硬件操作前登记受管失败清理，
保留 context 与 execution stop；部分初始化失败也经同一停止管线。停止先关闭 PCM
准入并推进一次 generation，活跃 writer 退出后重试；检查控制器 GCTL.CRST=0 与选定
流 RUN=0，超时或全 1 读回保留绑定和 DMA。确认停止并排空 class 调用后才释放。
普通用户 STOP 仍可重新播放，不会永久关闭设备；私有结构新增状态，模块 ABI 13 不变。

新增一个生产函数生命周期模型，ASan/UBSan 下 0.415 秒通过，覆盖失败停止/失联保留、
忙 writer 取消、重试、独立设备和十类部分初始化失败。既有 PCM 就绪检查两项 0.162 秒
通过，x86_64 dev-build 通过。模型中的 MMIO、锁、分配、codec setup/core adapter
不能替代实体硬件故障证据。

实际 q35/KVM、ABI=both SMP4、XFCE/OpenRC 使用同一生产内核，分别写入 96,000 字节
440 Hz 与 880 Hz PCM，逐次 drain、STOP、close/reopen；最终 29.213 秒自然关机，
QEMU/ext4/FAT 检查均为 0。QEMU 的 44.1 kHz 双声道 WAV 检出两个频率，捕获总长
0.937 秒，仅确认实际音频输出，不宣称逐样本完整性。首次运行因 Python ioctl 缓冲区
返回类型的脚本断言失败，修正夹具后通过；失败报告保留，生产二进制未因此修改。
未重跑全套或浏览器/游戏，未扩大 period IRQ/欠载处理功能。

证据为 `build/evidence/v1-closeout-20261116/hda-stop/report.json`，包含源码/header
哈希、模型/构建日志、两轮原始 guest 报告和 WAV 分析。上述 HDA 软件停止/回收缺口
已关闭；其它 provider、实体故障停止证据及正式候选验收继续保留。

## DW SDIO 失败 probe、PIO 取消与确认停止（2026-11-17）

修复旧 resource release 直接停止并释放 context、remove 仅屏蔽 IRQ 就返回成功的
路径。probe 在 MMIO 使用前选择受管失败清理、保存 context 和 execution stop；
OOM、映射/IRQ 资源错误及 card init 失败均走同一管线。停止先原子关闭准入并唤醒
等待者，PIO 锁忙返回 EBUSY，调用观察取消退出后可重试；关闭中的 IRQ 只确认真实
pending 位，不再投递完成。确认 host/FIFO/DMA reset 位和 IRQ enable 清零、INTMASK
为零后才同步退休 IRQ，class drain 后才释放 context。超时、寄存器全 1 或屏蔽不生效
保留绑定、IRQ 与 MMIO/devres。普通等待/FIFO 路径也拒绝已关闭设备或全 1 读回。

PIO 不向硬件发布调用者缓冲区；这里处理 CPU FIFO 访问、IRQ 与控制器状态寿命，
不新增 DMA/MMC/eMMC/UHS 功能，也不把异常取消时的请求宣布为写入成功。公开模块
ABI 13 不变。

扩展现有生产协议模型，27 个场景覆盖 SDHC/SDSC、读写/只读、错误和部分初始化、
停止失败保留/独立 peer/重试、轮询及 park 中取消；同一检查还运行两块板的既有时钟/
引脚胶合模型。UBSan 下两项共 0.572 秒通过。RISC-V64 与 AArch64 的实际 drvmod
编译通过；仅编译模块，没有将它记作整内核或板卡运行。未重跑全套、桌面或平台矩阵。

证据为 `build/evidence/v1-closeout-20261116/dw-sdio-stop/report.json`，包含冻结源码、
测试、两架构模块和哈希。上述软件迁移从旧驱动剩余范围排除；实体卡/板卡 IRQ、
复位失效与实际停止证据仍待验证，不能用这些模型或其它块设备的 QEMU 运行替代。

## R8169 失败 probe、IRQ 保留与关闭准入（2026-11-17）

修复 probe 失败直接释放 context、remove 只保留 DMA 却提前释放 IRQ，以及关闭中仍
能收发/轮询的路径。MMIO 使用前保存每绑定 context、登记受管失败清理与 execution
stop，部分 DMA 分配失败也交由同一管线处理。停止原子关闭准入，ring lock 忙返回
EBUSY；已进入的调用退出后可重试。锁外有限等待 ChipCmd.RST/TE/RE 清零及 IRQ mask
为零，失联或超时保留 IRQ、DMA 与 devres；确认停止才同步退休 IRQ，class drain 后
remove 释放。普通收发、轮询、MAC/carrier 与 IRQ 模式查询都遵守关闭状态。提前到达
的 IRQ 和关闭中的 IRQ 仍确认 cause，不交付网络完成；正常 IRQ 继续交付。

RX 缓冲与 ring 的设备同步提前到启用数据面之前。既有 boot 寄存器回环也先确认停止
再释放；修正注释中“静态单实例”“未接入 carrier”的过时描述。模块 ABI 13 不变，
不新增 PHY、offload、多队列或厂商代际功能。

扩展既有 PCI 网络生命周期检查，执行生产 probe、分配、IRQ、收发、stop/remove 及
原有寄存器回环；四类硬件拒绝/失联、在途 send、十类部分初始化失败、独立 peer 和
轮询回退均通过。MMIO、分配、锁与 IRQ/core 为模型。与同文件未改的无线 owner 检查
合计两项，UBSan 下 0.426 秒；x86_64 标准 drvmod 编译通过。首次新夹具缺少运算括号
导致 -Werror，修正后通过，原日志保留。未重跑整套、桌面或平台矩阵。

证据 `build/evidence/v1-closeout-20261116/r8169-stop/report.json` 含冻结源码/夹具、
模块、ELF 和哈希。本轮软件迁移从旧驱动剩余范围排除；没有实际 R8168/R8169 网络
流量或实体 IRQ/DMA/复位失败验证。普通复位读回也不等同各硅片的 PHY、固件、OOB
与 reset errata 认证，这些实际硬件证据继续保留。

## SDHCI 受管失败清理、PIO 取消与停止读回（2026-11-17）

原停止函数只写中断/时钟关闭、没有错误返回，失败初始化丢弃停止结果，remove 无条件
成功。现于映射/板级准备前保留 context、选择受管失败 probe 并登记 execution stop；
在途 PIO 通过原子 closing 退出，锁忙返回 EBUSY 保留旧绑定。确认全复位自清、中断
enable/signal 为零及内部/SD 时钟 enable 关闭后才允许 class drain 和释放；停止超时
或失联保留 context 与 MMIO/devres 供重试。停止轮询不依赖定时 IRQ；未完成板级准备
时不访问未启动/未供钟的协议寄存器。已有 Nano/Ox64 prepare 不向 host 发起传输。

普通等待和逐字 PIO 接入取消，全 1 读回不会被误当时钟稳定；取消/失联时不再发起
恢复写。关闭后的读写/容量查询不进入硬件。内部 stop 接口改为 int 并同步全部调用者，
未改变用户接口、平台回调或模块 ABI 13；单线 PIO 的能力范围保持。

只扩展已有 SDHCI 定向检查：43 项 ASan/UBSan 共 1.139 秒通过，含原 SDHC/SDSC、
只读、分频、板级准备与错误检查，以及停止失败保留/重试、在途取消、全 1 时钟拒绝。
协议模型运行真实 engine，class 生命周期模型替代 engine 返回；不当作完整设备核心
或板卡集成。RISC-V64 标准 drvmod 编译通过；首次新夹具因缩进警告编译失败，修复后
通过，原报告保留。未追加全局回归、桌面或平台矩阵。

证据 `build/evidence/v1-closeout-20261116/sdhci-stop/report.json` 保存源码/header/测试、
模块、ELF 与哈希。本轮软件迁移不再列为旧驱动缺口；没有实卡停止/失联验证，板卡
运行与认证范围仍遵循既定支持矩阵，不新增平台发布承诺。

## Sunxi MMC 在途取消、失败 probe 与停止结果（2026-11-17）

原 stop 忽略时钟更新超时，release 无条件释放 context；失败初始化的停止结果和
remove 都无法传回 core。现于 MMIO/协议使用前保存每绑定 context，登记受管失败
probe 与 execution stop。停止原子关闭准入，PIO 锁忙返回 EBUSY；旧调用在等待、
延时与 FIFO 循环中观察取消退出后可重试。确认 host/FIFO/DMA reset 自清、PIO 模式、
中断 mask/enable 关闭和卡时钟更新完成/enable 清零后才回收。超时或寄存器全 1
保留绑定、context 与 MMIO/devres，class drain 后 remove 才释放。

原失败 start 中丢弃结果的隐式 stop 已移除，由调用者检查 stop 后释放；内核适配器
统一走受管管线，内部 API 的唯一实际调用者与测试已同步。时钟更新同时检查新请求的 RINT 错误状态，
不把 START 清零而 HLE 置位误当成功；发起前清除旧卡错误，防止旧状态污染停止结果。普通关闭后的 read/write/
flush/capacity 不进入 MMIO；失联或取消不继续时钟清理写。内核内部 stop 改为 int，
不改变用户接口、平台数据或模块 ABI 13，也不扩大单线、最高 400 kHz 的 PIO 范围。

只扩展既有 Sunxi 检查，42 项 ASan/UBSan 共 1.283 秒通过。绑定检查执行真实 class
适配器与协议 engine，内存、MMIO、计数器、锁和 core action 为模型；覆盖六类停止
拒绝/失联、失败 probe 保留、重试、在途取消以及 A64/旧式 DATA0 时钟路径。原 SDHC/
SDSC、非对齐读写、保护、分频/供电和板级胶合检查保留。AArch64 标准 drvmod 编译
通过；没有整内核、ARM32 或实体板卡运行，也未追加全局或应用矩阵。

证据 `build/evidence/v1-closeout-20261116/sunxi-mmc-stop/report.json` 保存冻结源码、
header、测试/夹具、模块与 ELF/哈希。本轮软件迁移从旧驱动缺口排除；实卡和电气/
时序验证仍按既定平台矩阵保留，不用寄存器模型替代。

## VirtIO Sound 失败保留与可重试停止（2026-11-17）

原失败 probe/复位超时把 context 放进无重试入口的全局 quarantine，remove 还清空
设备 drv_priv 并释放 IRQ，core/devres 的寿命与存活 DMA 脱节。现于 transport 初始化前
选择受管失败清理，context 和等待队列在硬件使用前保存，并登记 execution stop。
停止原子关闭准入、推进一次 generation 并唤醒等待；锁忙返回 EBUSY，旧 control/TX
等待退出后可重试。复用公共 transport reset，只有 status=0 读回才同步退休 IRQ，
class drain 后释放全部 DMA/context；超时/失联保持旧绑定和 devres，不再转入游离链表。

health 发现 NEEDS_RESET 只中止当前工作，不在活跃路径内自行复位设备。普通 STOP/
drain 保留既有 stream release 契约，生命周期退出直接执行设备 reset，不等待可能
已经失效的 PCM 控制命令。关闭后的 write/drain/STOP/close 不访问硬件；IRQ 仍确认
和唤醒，失联读回不写 ACK。无任何 DMA/硬件操作的早期不匹配仍可释放元数据。
公开模块 ABI 13、格式和音频功能范围不变。

新增一个生产函数模型，含真实 VirtIO sound 函数体和公共 reset，ASan/UBSan 下
0.545 秒通过。PCI/MMIO probe、17 类部分初始化/分配失败、控制响应和满 TX 队列等待
中的取消、失败停止保留/独立 peer/重试及 IRQ 轮询回退均覆盖；设备 queue 响应、
MMIO、锁/等待与 core adapter 为模型。x86_64 dev-build 通过。

一次实际 q35/KVM、ABI=both SMP4、XFCE/OpenRC guest 使用 virtio-sound-pci，依次写入
96,000 字节的 440/880 Hz PCM，逐次 drain、STOP、close/reopen。29.974 秒自然关机，
QEMU/ext4/FAT 均退出 0。实际 QEMU WAV 为 44.1 kHz 双声道、44,100 帧/1 秒，两个频率
均检出；这不是逐样本保真或实体设备认证。未注入实际 reset 故障，未重跑浏览器/游戏、
全套或架构矩阵。

证据 `build/evidence/v1-closeout-20261116/virtio-snd-stop/report.json` 保存源码、公共
reset、测试/夹具、构建、guest 输入哈希、报告与 WAV 分析。本轮软件迁移与该正常
PCI 播放/关机范围从旧驱动剩余项排除，其它 provider 和正式候选要求继续保留。


## VirtIO RNG 失败绑定保留与读取期间停止（2026-11-17）

`virtio_rng.c` 在接触 transport 前登记受管失败 probe 与 execution stop，单静态实例
以 owner 占用至成功 remove，失败实例不会再被后续 probe 覆盖。停止先关闭准入；
读取、poll 的活动计数覆盖锁外 notify、等待与熵池混入，活动调用退出前返回 EBUSY。
读取等待观察 closing 后退出；确认公共 reset 的 status=0 后才同步退休 IRQ，class
排空后释放 coherent DMA 和 owner。复位超时或寄存器失联保留绑定、IRQ、DMA 与 devres，
可通过原绑定重试；部分初始化失败共用这条清理管线。早期失败未接触硬件时只清元数据。

一个生产函数体/公共 reset 模型通过 ASan/UBSan（0.392 秒）：PCI/MMIO、按需投递、
IRQ 回退、三处活动读取停止竞争、复位超时/失联、单实例隔离与重试、11 类失败 probe。
寄存器、设备响应、分配、锁和 core 排序适配器是替代。x86 dev-build 完成；实际
q35/KVM、ABI=both SMP4 XFCE/OpenRC guest 对 `/dev/hwrng` 逐次 open/read/close，读出
256 与 37 字节，非固定重复值，28.619 秒自然关机，QEMU/ext4/FAT 检查均为 0。
这不认证熵的密码学质量、实际失败复位或物理设备；MMIO 运行和正式平台矩阵仍未覆盖。

证据 `build/evidence/v1-closeout-20261116/virtio-rng-stop/report.json`，含冻结源码、
公共 reset、测试/夹具与实际 guest 输入哈希。仅关闭本轮 RNG 软件迁移范围；单实例、
按需读取和用户/模块 ABI 保持原契约，不扩大后续功能。


## VirtIO Console 活动收发保留与失败清理（2026-11-17）

`virtio_console.c` 已在 transport 初始化前登记受管失败 probe 和 execution stop。
单实例 owner 直到成功 remove 才释放。关闭准入后，TX 等待观察 closing 退出，活动
read/write/poll/ioctl 与 IRQ RX pump 的计数覆盖锁外 notify、UART 镜像递送和 usercopy；
仍有活动调用或锁忙时 stop 返回 EBUSY。公共 reset 确认 status=0 后同步退休 IRQ，class
排空后释放 RX/TX 与可选控制队列 DMA。超时/失联保留原绑定、IRQ、devres 和全部 DMA，
包括 MULTIPORT 握手失败后的设备所有权。原 raw 同步写、批量 TTY 背压、drain 与 ABI 不变。

生产全函数体生命周期模型通过 ASan/UBSan（0.489 秒）：raw/bulk 握手、RX/TX、四处活动
调用停止竞争、超时/失联保留及重试和 14 类部分 probe 失败；设备响应、锁、分配与 core
顺序是替代。原 TX 模型的三种容量配置、四写者、回绕/背压/中断检查通过（0.939 秒）。
x86 dev-build 退出 0。实际 q35/KVM、ABI=both SMP4 guest 的现有 `vport_test` 收发各 64
字节并逐字节核对，统计 dropped/timeouts=0；重新打开后 drain 成功，30.767 秒自然关机，
QEMU/ext4/FAT 为 0。该轮关机期间 `xfsettingsd` SIGSEGV，**guest 整体判定仍为失败**，
该历史失败不改写；[DESKTOP-15 后续修复与关闭](desktop-minecraft-status.md#desktop-15-修复与关闭2026-11-17)另有实际关机证据。首轮夹具路径错误也保留。

证据 `build/evidence/v1-closeout-20261116/virtio-console-stop/report.json`，冻结生产源码、
公共 reset、测试/夹具、guest 输入与宿主回环字节。仅关闭 Console 的软件迁移范围，
不升级为实际复位失败、物理或完整平台矩阵认证。


## Sunxi Audio 停止失败保留与在途取消（2026-11-17）

原 destroy 在 DMA 停止失败后只跳过 coherent 缓冲区释放，仍释放 IRQ、MMIO 和 context，
remove 返回 0，无法由原绑定重试。现于 prepare 前保存 context，登记受管失败 probe
和 execution stop。停止关闭准入，以 writer/IRQ 锁的 trylock 返回 EBUSY；write、drain
等待观察 closing 退出。先确认 DMA IRQ 屏蔽与两处发射/请求使能清零，再在 spinlock 外
等待 DMA_EN=0（失联返回 ENODEV），清 pending 并关闭模拟电路/电源；电源回调失败也
保留原状态。成功后同步退出 IRQ，class 排空后才释放 PCM/LLI DMA、全部映射和 context。
未取得 DMA 通道所有权的早期 busy probe 不写另一 owner 的控制寄存器。

只扩展既有 `test_sunxi_audio.py`：PCM/codec 与架构电气准备两个方法 ASan/UBSan 共
0.875 秒通过。覆盖 A64/V3s 原播放、drain、XRUN/reopen；停止超时、寄存器失联、IRQ
屏蔽/发射器不接受关闭、电源关闭失败、活动 write/drain 的忙 stop 及取消、失败 probe
保留依赖和 DMA 后重试、action 分配失败。MMIO、时间、锁、IRQ、分配与 core 排序为替代。
AArch64 标准 `sunxi-codec.a20drv` 交叉构建退出 0，ELF/动作接口符号检查通过；没有运行
整内核、ARM32、真实 A64/V3s 板或其它 QEMU 控制器，不据此认证模拟波形或实板时序。

证据 `build/evidence/v1-closeout-20261116/sunxi-audio-stop/report.json` 保存冻结生产源码、
平台/period-ring 头、板级准备、测试与模块哈希。仅关闭本轮软件迁移；物理平台与候选
范围继续按既定要求保留，用户音频格式、普通 STOP/reopen 与模块 ABI 不变。


## SG2002 Audio 通道归属与停止失败保留（2026-11-17）

原 remove 在 abort 失败后释放 IRQ、映射和 context，使保留的 PCM/LLI DMA 脱离绑定。
现于 prepare 前保存 context，登记受管失败 probe 和 execution stop。通道 7 的绑定归属
独立于 `dma_owned` 活动状态；即使用户 STOP 已结束当前流，remove 仍核对通道停止。
关闭准入后 writer/IRQ 锁忙返回 EBUSY；write/drain 等待观察 closing 退出，IRQ 只确认
状态、不再退休 period。确认 I2S/DAC 关闭和静音后，在 spinlock 外清并读回通道 IRQ
signal/status enable，发 per-channel abort+WE 并等待 CHEN 清零。超时、失联或读回失败
保留原绑定、全部映射、IRQ、依赖和 DMA，成功重试后同步退出 IRQ，class 排空后回收。
未取得通道的 busy probe 不更改另一 owner，停止不影响其它 DMA 通道。

只扩展已有检查：PCM/DMA 模型 ASan/UBSan、板级准备模型 UBSan，两个方法共 0.489 秒
通过，保留原 LLI、缓存授权、正常播放/drain/XRUN/reopen 检查，补停止失败/重试、关闭
后的 IRQ、两类等待取消、锁忙及部分 probe 失败。MMIO、时间、IRQ、分配、锁和 core
排序为替代。标准 RISC-V64 模块编译退出 0，ET_REL 与动作/trylock 接口检查通过；没有
整内核、真实 Nano、模拟波形或真实 DMA/缓存/IRQ 运行，不升级为实板或平台矩阵认证。

证据 `build/evidence/v1-closeout-20261116/sg2002-audio-stop/report.json`，冻结源码、
测试及导入 stub、硬件/平台/period-ring/clock 头、板级准备与模块 ELF/哈希。仅关闭
本轮软件迁移，PA、格式、普通 STOP/reopen 与模块 ABI 不变。


## RK817 / PL330 停止与 codec 镜像重试（2026-11-17）

原 destroy 在 PL330 停止失败后仍释放 IRQ、映射和 context，codec 恢复错误也只打印
日志。现于 prepare 前选择受管失败 probe、保存 context 并登记 execution stop；指定
PL330 provider 已被 native/user 绑定时在 prepare 前拒绝。停止关闭准入，writer/IRQ 锁
忙返回 EBUSY，write/drain 等待取消；closing 后 IRQ 只确认事件，不再进行 debug 指令
或 period 退休。确认 I2S 请求/发射器与 event IRQ 屏蔽后，在 spinlock 外分别检查并
DMAKILL manager/channel；STOPPED 与故障位都须清除，寄存器失联返回 ENODEV。
硬件停止失败保留两个 IRQ 登记和所有资源；独占拓扑按既有规则禁用不可控 IRQ 线。

codec 原始快照保留至全部恢复写入和读回成功，解除静音最后执行。恢复失败或读回
不匹配返回错误并尽力静音，仍保留绑定、映射、PCM/程序 DMA、IRQ 登记和原快照供重试；
不能把静音写入失败说成已经静音。成功后同步退休两条 IRQ，class 排空后才释放存储。
普通用户 STOP/reopen、音频格式、模块 ABI 和控制器独占范围不变。

只扩展已有两个模型，ASan/UBSan 共 0.975 秒通过：原 PCM 扩展、真实生成的 PL330
指令解释、I2C/时钟和拓扑，以及停止超时/失联、event/发射器拒绝、STOPPED+fault、
codec 恢复失败/错误读回、等待取消和部分 probe 保留/重试。设备响应、MMIO、锁、时间、
IRQ、分配和 core 排序是替代。标准 AArch64 rk817-codec.a20drv 编译退出 0，ET_REL 与
动作/trylock 接口检查通过；没有整内核、真实板、模拟波形或 DMA/IRQ 时序与电源恢复
运行证据，不据此认证物理平台。证据 `build/evidence/v1-closeout-20261116/rk817-audio-stop/report.json`
冻结源码、测试/导入 stub、平台/period-ring/PL330 头、板级准备与模块 ELF/哈希。


## RTW89 broker 停止与映射失败保留（2026-11-17）

在 PCI 写入前登记受管失败 probe/execution stop，检查 BME 与 INTx 屏蔽回读。
停止关闭网络、open、配置与新映射准入；锁忙、PCI 失联、BME 未清除、服务仍打开或
DMA 尚未归还均返回错误，保留原设备绑定。既有所有者仍可执行 DMA_FREE/QUIESCE；
释放 DMA 前若解除映射失败，保留原存储、token、映射和预算。最后一次 close 若解除
映射失败，保留 MM 引用与地址供停止重试；成功后在锁外放引用。正常归还、关闭后可
移除并重新绑定。服务异常退出遗留 DMA 仍隔离至重启；没有把 BME=0 当作 radio idle。

只运行 broker 生命周期与原 usercopy 两个方法（前者 ASan/UBSan，后者 UBSan），
覆盖失败保留/重试、正常重新绑定、关闭准入、部分 probe 和原字节数返回约定负控。
标准 WIFI_RTW89=1、embedded、ABI=both、x86_64 SMP4 配置仅编译 broker 对象退出 0。
PCI、MM/VFS、锁和分配为替代边界；未操作实体网卡，不证明 radio DMA 排空、崩溃后
自动恢复或全内核/实机运行。本轮软件迁移从旧驱动缺口排除；其它 provider、实体停止
与候选验收仍开放。证据 `build/evidence/v1-closeout-20261116/rtw89-broker-stop/report.json`。


## TPM TIS FIFO 与停止保留（2026-11-17）

原路径没有显式 GO，FIFO 使用错误的字访问，零 burst 可无限循环，接收可越过响应
声明长度；Startup 错误也被忽略。现以字节访问 FIFO/ACCESS/STS 控制，检查 DATA_EXPECT
并显式 GO；先读 10 字节头，再按长度读完，截断/额外数据/错误 TPM2B 大小返回失败，
不向 GetRandom 调用者复制未验证输出。保留非零 TPM 返回码，Startup 仅接受成功或
已初始化响应；检查通过才公布 present。轮询使用互斥锁，保持中断可用以推进超时。

MMIO 映射前登记受管失败 probe/execution stop，单设备绑定。停止关闭准入并取消等待；
锁忙返回 EBUSY，随后检查 locality 获取、IRQ 全局屏蔽回读、COMMAND_READY 中止和
locality 归还。任一步失败保留 context/绑定和 devres 映射，成功才允许移除；部分 probe
失败同样走此协议。命令清理失败也关闭准入并保留所有权供 stop 重试。

一个生产模块模型 ASan/UBSan 方法通过（0.231 秒），覆盖正常/短 GetRandom、Startup
错误、帧截断与长度、零 burst/响应超时、取消、部分 probe 和停止失败保留/重试；MMIO、
时间、锁、固件发现和 core 排序为替代。标准 x86_64 SMP4 RAMFS_USER=1 的 tpm.a20drv
编译退出 0，ET_REL 与框架导入核对通过。仅关闭本轮软件修复；未运行完整内核或实体
TPM，不认证 IRQ/时序、熵质量、PCR/密钥存储或 TPM2 Shutdown；CRB 仍拒绝。
协议核对依据为 [Linux TIS core](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/char/tpm/tpm_tis_core.c)
及 [MMIO 字节访问](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/char/tpm/tpm_tis.c)。
证据 `build/evidence/v1-closeout-20261116/tpm-stop/report.json` 冻结源码、模型、接口头、
模块与导入/ELF 哈希。其它 provider、实体停止/失效与正式候选验收继续开放。


### DesignWare I2C 停止与失败 probe 保留（2026-11-17）

原驱动在初始化失败或 remove 中忽略 disable 结果并释放控制器，普通传输也可能在
最后一次 disable 失败后返回成功。现于 MMIO 使用前保存 context，登记受管失败
probe 与 execution stop；只有核对 component type 后才取得硬件清理归属，错误
寄存器块的失败回滚不写硬件。adapter 在已确认 idle-disabled 后才发布。

停止关闭准入，PIO 锁忙返回 EBUSY；确认 component、IRQ mask 读回与 ENABLE_STATUS
后才允许继续 remove。超时、屏蔽拒绝或寄存器失联保留 context/MMIO，显式重试；
adapter 的真实引用未排空则 remove 返回 EBUSY，context 最终只由 devres release
释放。关闭后传输不访问硬件，在途 FIFO/STOP 等待可取消且不再为恢复重新启动
控制器；正常传输最后 disable 的错误也传回调用者并关闭准入，不伪装成成功。

`python3 -m unittest tools.tests.test_i2c_transport_lifetime` 三个方法 0.462 秒通过。
新增方法执行生产 DW 控制器、其原寄存器/从设备自测及真实 adapter registry；硬件
MMIO、锁、时间与 core 生命周期顺序为模型。覆盖部分初始化失败保留/重试、错误
component 零写入、disable/IRQ-mask 拒绝、all-ones 失联、活动 PIO 取消及持有 adapter
引用的 remove。保留原 adapter/HID teardown 回归。标准 x86_64 SMP4 ABI=both C 对象
编译退出 0，1.064 秒；未新增整内核或 guest，也没有物理 I2C/触摸板停止认证。

证据 `build/evidence/v1-closeout-20261116/dw-i2c-stop/report.json` 冻结源码、夹具、
公共接口、编译对象和输出。该软件迁移从旧驱动剩余范围排除；其它 provider、实体
停止/失联证据与正式支持候选验收继续保留。


### I2C-HID 失败初始化与工作线程停止（2026-11-17）

原正常 remove 会等待工作线程，但失败 probe 在 work 创建失败后忽略队列 destroy
结果并直接释放状态，首次 schedule 的错误也被忽略。现于首次 I2C 访问前保存
context、依赖与 adapter 引用，登记受管失败 probe/execution stop。描述符、缓冲区、
queue/work 分配和首次调度失败都走同一清理管线；工作线程未确认退出时保留绑定、
代码、queue/work、描述符/输入缓冲区和 adapter，后续显式重试。

stop 先原子关闭 I/O 与回调重新调度，再以 2 秒截止时间 destroy queue；不先取得
回调可能在传输等待中持有的 io_lock。超时/中断保留状态；成功 join 后才清除运行
标志，class 调用排空及 remove 成功后才由 devres 释放 work、adapter 和缓冲区。
GPIO attention 仍是原有固件 boot-lifetime 只读借用，不改控制器或针脚所有权。

现有 `test_i2c_transport_lifetime` 增强原 HID 方法，执行真实 probe、stop、release、
I/O 和 rearm 函数及生产 adapter registry；3 个方法 0.527 秒通过，UBSan。覆盖
依赖/action/描述符/缓冲区/queue/work/首次 schedule 失败、join 中断/超时的资源
保留、重试、I/O 关闭和不再 rearm。协议/解析器、工作队列、锁和 core 顺序为模型，
不能计作物理触摸板或 SMP 实测。初次夹具提取函数误匹配前置声明及缩进警告导致
编译失败，修正后通过，原失败日志保留。标准 x86_64 SMP4 ABI=both 对象编译
1.462 秒退出 0；未追加整内核/guest/触摸板交互矩阵。

证据 `build/evidence/v1-closeout-20261116/i2c-hid-stop/report.json` 冻结源码、公共头、
夹具、日志与对象。本轮软件迁移从旧驱动剩余范围排除，其它 provider、实体停止/
失联证据与正式支持候选验收继续开放。


### PS/2 可失败停止、端口归属与有界排空（2026-11-17）

原驱动忽略停止命令/IRQ 同步结果，清空输出与 IRQ 处理又可在持续输出或失联时无限
循环。现于访问硬件前保存单实例 owner、受管 probe/execution stop 和声明的 0x60/
0x64 端口 claim；失败 probe 不再提前丢弃清理状态。IRQ 与 flush 每轮最多处理 256
字节，端口 0xff 明确视为失联；命令/数据等待保留有界超时。初始化改用正确的端口
disable 位 0x30 并保留其它控制位，鼠标命令数据也等待输入缓冲可写。

stop 原子关闭 IRQ 读取，屏蔽并检查同步所有已登记 dispatch，再关闭两端口时钟、
有界清空旧数据、清 IRQ enable 并读回控制字；要求 `(config & 0x33) == 0x30`。
任何命令超时、失联、读回不符或同步错误都保留 owner、端口 claim、IRQ 与模块；
成功后 remove 释放 IRQ，最终 devres release 清除静态状态，允许重新绑定。

完整生产驱动在端口/IRQ/core 模型中的定向方法 0.238 秒通过，ASan/UBSan。覆盖
claim/action/初始化/IRQ 失败、停止超时/无回复/写拒绝/失联/同步失败后的保留与重试、
持续输出的有界处理、关闭后的 IRQ 不读端口、键盘按下/释放到 input ring 与控制台
字符的正常路径。最后增加了一条成功 stop 的配置日志供真实 guest 核验；没有新增
协议或放宽模型检查。实际 x86_64 SMP4 ABI=both 模块构建 0.709 秒退出 0。

一次复用私有输入的 KVM q35 SMP4 XFCE/OpenRC guest 核对新模块哈希、成功绑定并
正常关机，停止读回 `config=0x70`，32.045 秒自然退出；串口/持久日志无 FATAL，
监督器无强制超时，QEMU/ext4/FAT 均为 0。它验证的是 QEMU 控制器的正常停止；错误
路径仍来自模型，未新增物理 PS/2/USB 键盘、QMP 键鼠交互或其它架构认证。

证据 `build/evidence/v1-closeout-20261116/ps2-stop/report.json` 保存冻结源码/公共头、
夹具、实际模块、guest 输入转移链与日志。本轮 PS/2 软件迁移和上述 QEMU 正常停止
从剩余旧驱动范围排除，其它 provider、实体停止/失联与正式候选范围继续保留。


### VMSVGA 命令页退休与可失败停止（2026-11-17）

原驱动的下一次 submit 会清空设备可能仍在访问的 command page，remove 又未等待
命令退休即清空静态实例。现命令页保持唯一 pending owner，超时/取消只关闭新提交；
已定义的终态才允许复用，未知/partial 状态继续保留。缓冲页仍属静态模块实例，失败
绑定保留模块代码和页，不以 PCI bus-master 清零代替命令退休。命令等待改为任务
mutex 下的 100 ms / 有界轮数等待，不再长时间在 IRQ-off spinlock 中轮询。

probe 在 PCI 使用前登记 execution stop/release，partial probe 走同一可失败管线。
stop 关闭准入，锁忙返回 EBUSY；先确认旧命令退休，再完成 device-context STOP，
禁用并读回 IRQ mask、显示与 CONFIG_DONE，最后检查 PCI BME 清除/INTx 屏蔽读回。
超时或任何状态错误均保留实例、命令页、MMIO 和绑定。STOP 本身超时后仍拥有该页，
其完成后的重试不重复提交 STOP。仅 stop、class drain 与 remove 均成功后清空实例。

全生产源的主机完成/PCI/MMIO/core 模型回归 1 方法 0.329 秒通过，ASan/UBSan；覆盖
partial probe、超时页逐字节不变、关闭后的提交拒绝、活动调用关闭、延迟 STOP、
未知状态、设备失联、mask/enable/config/PCI 写拒绝及重试。首轮模型的 CONFIG_DONE
故障注入前值已被上一轮清零，补齐前值后正确触发拒绝；错误日志保留。内核不定义
UINT16_MAX 的构建失败已改用既有 0xffffU 风格并保留日志；最终 x86_64 SMP4 ABI=both
模块构建 0.701 秒退出 0，全部导入与框架导出匹配。没有 VirtualBox/实机运行验收。

审查同时按[上游寄存器定义](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/gpu/drm/vmwgfx/device_include/svga_reg.h)
当时修正了 SVGA-II ENABLE/WIDTH/HEIGHT/FB_SIZE 四个错误编号，但旧版 BAR 选择与
刷新仍混用 SVGAv3，未据此关闭整个路径；后续修复与关闭记录见下节。
证据 `build/evidence/v1-closeout-20261116/vmsvga-stop/report.json` 冻结源码、夹具、
公共接口、模块、日志和哈希。其它 provider、实体停止/失联与正式候选仍开放。


### SVGA-II BAR/FIFO 路径修复与关闭（2026-11-17）

旧版 SVGA-II 独立使用 BAR0 的索引/值寄存器、BAR1 的 VRAM 和 BAR2 的 FIFO，
不再把 BAR2 当作 framebuffer 或向旧版设备写 SVGAv3 命令缓冲寄存器。I/O BAR 经
声明端口 claim 后使用新增原生 32 位端口访问器，索引/值端口为 base/base+1；既有
MMIO 索引寄存器路径保留。模块元数据同时匹配 0405/0406，声明 IOPORT 资源。

FIFO 仅在控制器禁用、确认不忙后初始化，校验设备报告容量及 MIN/MAX/NEXT/STOP
边界；在 mutex 内提交完整 UPDATE 包，支持回绕且保留一个空字。每次发布前排空
旧包，提交后的 SYNC、BUSY 和 STOP/NEXT 共同确认消费；超时关闭新提交，FIFO 与
映射保留供 stop 重试。stop 在关闭扫描输出前确认 FIFO 退休。fbdev 仅暴露 BAR1
内的实际 offset/可见字节，原生 layout 返回真实 pitch。

增强现有生产源消费者方法，执行实际端口访问器、驱动、IO/MMIO 索引协议和 FIFO：
含非零 offset、带 padding 的 pitch、UPDATE 坐标、跨末尾回绕、超时数据逐字节
保留、陈旧/损坏指针与失联重试，同时保留原 SVGAv3 生命周期用例。1 方法 0.466 秒
通过，ASan/UBSan。首次内核模块编译发现 UINT32_MAX 在该头文件集合未定义，改为
64 位比较防止乘法溢出；失败日志保留，最终模块构建 0.738 秒、带端口访问器/导出的
x86_64 SMP4 ABI=both 内核构建 26.729 秒均退出 0，模块导入全部匹配。

一次真实 q35/KVM SMP4 guest 只替换为 SVGA-II 和任务私有 OpenRC QA 服务，不启动
桌面/浏览器/游戏。实际新模块建立 1024×768×32、pitch=4096、3 MiB framebuffer，
用户态 mmap 写入四段颜色、调用 FBIO_FLUSH 并读回正确像素；映射在关机前关闭。
26.794 秒自然关机，日志确认 FIFO drained/scanout disabled，QEMU/ext4/FAT 均为 0，
持久日志 FATAL=0。颜色读回和 FIFO 消费不是显示器光学或主机截图认证。

旧版 BAR/刷新条目达到关闭条件，从缺口表删除。源码、公共接口、模块/内核、输入
转移链及结果见 `build/evidence/v1-closeout-20261116/svga2-fifo/report.json`。没有
扩大 VirtualBox、其它架构、实体故障停止或 N18/N20 正式候选认证；通用驱动缺口仍
按其余 provider 和实体范围保留。


### LS2K GMAC 可失败停止与 DMA 槽位保留（2026-11-17）

修复 remove 仅清空静态有效位、未停止 DMA 即允许复用描述符的问题。probe 在映射
和硬件访问前登记 execution stop/release；部分初始化失败同样保留其独占槽位。
stop 先关闭新收发，控制 mutex 或环锁忙返回 EBUSY；在 IRQ 开启的任务上下文确认
DMA IRQ 禁用、ST/SR 与 MAC TE/RE 清零、TX/RX process state 均停止，再等待 SWR
自清并再次检查禁用状态。每个等待同时有 100 ms 时间界限和有限轮数，失联、写拒绝
或超时保留原实例、静态 DMA 存储和 MMIO，显式重试成功且 class drain/remove 完成
后才清空槽位。MDIO 等待移出 IRQ-off 环锁，活动控制调用可观察关闭状态后退出。

发送前检查架构 DMA 准入与完整 32 位地址范围，拒绝跨越 4 GiB 的静态环/缓冲区；
仅在匹配 DWMAC 3.7 后写寄存器。保留 LS2K 原有增强四字描述符格式，改用显式链式
下一地址和 64 字节隔离槽，避免非一致缓存维护覆盖相邻 DMA 描述符。RX 拒绝错误、
非完整帧及超出 4..1536 字节范围的长度，再读取缓冲区；修复 MAC 字节打包的有符号
左移未定义行为。未新增协商模式、IRQ 收发或其它 GMAC 变体支持。寄存器/描述符位
按上游 [DWMAC DMA 定义](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/net/ethernet/stmicro/stmmac/dwmac_dma.h)
与[描述符定义](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/net/ethernet/stmicro/stmmac/descs.h)核对。

整个生产驱动在主机 MMIO/独立 DMA 视图/锁/core 顺序模型中的 1 方法通过，0.399 秒，
ASan/UBSan 错误立即失败。覆盖收发链两次回绕、短包补齐、RX 长度与分片拒绝、DMA
准入/地址跨界、partial probe、控制/环锁忙、寄存器拒绝、失联、process state 卡住、
SWR 卡住、停钟有限轮数退出及保留后重试；停止失败后新调用不访问 MMIO，另一次
槽位预留不能覆盖旧实例。首轮查出 MAC 打包移位 UB；同一轮模型的 MAC 拒绝注入
发生在上一失败尝试已禁用 MAC 后，补齐有效前值后覆盖该故障。原日志均保留。
最终 x86_64 SMP4 ABI=both 单个标准 C 对象编译 0.830 秒通过；没有整内核、guest、
LoongArch 目标构建、实板收发/停止或正式候选认证。

证据 `build/evidence/v1-closeout-20261116/ls2k-gmac-stop/report.json` 保存冻结源码、
公共头、夹具、对象和日志。本轮 LS2K 软件迁移从旧驱动剩余范围排除；StarFive 等
其它 provider、实体停止/失效和正式候选范围继续保留。


### StarFive GMAC 可失败停止与收发所有权修复（2026-11-17）

修复 remove 未停止 DMA 即清空静态槽位，以及失败初始化没有受管清理 owner 的问题。
probe 在映射/硬件访问前登记 execution stop/release；stop 先关闭新调用，控制 mutex
或环锁忙返回 EBUSY，再确认 DMA/MAC IRQ 禁用、TX ST、MAC TE/RE、RX SR 清零，
等待 DMA SWR 自清和通道 process state 停止，复查禁用状态后才允许退休。等待使用
100 ms 时间界限和有限轮数，不在 IRQ-off 区轮询。任何拒绝、超时或失联均保留原
实例、静态 DMA 存储、MMIO 与绑定；class drain/remove 成功后的 final release 才
清空槽位。部分初始化失败走同一重试管线，板级时钟/reset 归属不变。

JH7110 的 64 位 AXI 数据总线用 DSL=6 把四字描述符隔离为 64 字节槽，检查实际
stride 读回。CPU 在检查 OWN 前同步描述符，TX/RX 均在复用时重建可能被硬件写回
覆盖的地址字。RX 验证完整帧、错误/context 标志及 4..1536 字节范围后才读缓冲区；
TX 保留一个空槽，避免 head/tail 重合使满环被当作空环。MDIO Clause-22 写操作不再
误置 C45E，错误读回 0xffff 不再当作链路已连接；MAC 地址打包改为无符号移位。

probe 检查 DWMAC 5.20 身份和架构 DMA 准入，按 feature1 的 32/40/48 位能力校验
整个地址跨度；高于 4 GiB 的合法地址继续使用 HADDR，只有跨越 low-tail 窗口的环
被拒绝。初始化核对 EAME 和 DSL，FIFO 大小按硬件报告设置并启用 RX queue 0，
不再写入超出实际 FIFO 的任意容量；修正 TX flow-control 寄存器偏移。没有新增
IRQ 收发、其它硬件变体、协商模式或板级时钟策略。格式/总线依据见
[上游 EQOS 实现](https://raw.githubusercontent.com/u-boot/u-boot/v2025.01/drivers/net/dwc_eth_qos.c)和
[StarFive 配置](https://raw.githubusercontent.com/u-boot/u-boot/v2025.01/drivers/net/dwc_eth_qos_starfive.c)，
地址能力见 [DWMAC4 DMA](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/net/ethernet/stmicro/stmmac/dwmac4_dma.c)。

整个生产驱动在主机 MMIO/独立 CPU-DMA 视图/锁/core 顺序模型中的 1 方法通过，
0.423 秒，ASan/UBSan 错误立即失败。覆盖合法高地址和 32 位低地址、跨 4 GiB 窗口
拒绝、收发两次回绕、描述符写回地址破坏后恢复、短包补齐、满环不覆盖、RX 异常
长度/分片/context 拒绝、partial probe、MDIO 活动关闭、寄存器拒绝、失联、SWR/
process state 卡住和停钟有限轮数退出，以及保留后的显式重试。x86_64 SMP4
ABI=both 单个标准 C 对象编译 0.866 秒通过，没有整内核、RISC-V 目标、guest、实板
收发/停止、板级缓存一致性或正式候选认证。

证据 `build/evidence/v1-closeout-20261116/starfive-gmac-stop/report.json` 保存冻结
源码、公共头、夹具、对象、结果和哈希。本轮 StarFive 软件迁移从旧驱动剩余范围
排除；其它 provider、实体停止/失效与正式候选范围继续保留。


### VirtIO input 队列范围、共享 IRQ 与可失败停止（2026-11-17）

事件区实际覆盖 32 个 64 字节槽，原 DMA 请求只到 0x600，而事件区结束于 0xc00。
现从事件槽数推导完整分配跨度，静态检查 descriptor/avail/used/events 不重叠，初始化
队列后才设 DRIVER_OK 并通知设备。对已知异常 used-ring，先校验完成数量、完整
32 位 id、8 字节事件长度与同批重复 id，再消费或重新发布缓冲，避免截断 id、读取
不完整事件或无界 IRQ 循环；用户事件复制允许非对齐目标。

每个设备单独用自身实例登记共享 IRQ；删除全局“一个 IRQ 只登记第一个实例”和
remove 时仅修改标志的伪接管。原机制既漏掉同线的第二设备，又可能把回调留在旧
实例。现在只有对应 owner 被注销，其余共享 handler 保持有效，IRQ 排空后才允许
实例退休。关闭后的迟到 IRQ 只确认原因，不读写事件环或重新提交。

probe 在传输初始化前登记 execution stop/release 并预留静态槽；失败 probe 不释放
可能仍被 DMA 使用的队列。stop 先关闭准入，环锁忙返回 EBUSY；离开 IRQ-off 区后
调用生产 `virtio_transport_reset(..., 100)`，读回 status=0 后才注销 IRQ。超时/失联
保留原实例、队列与 transport，显式重试成功且 class drain/remove 完成后才交还
DMA 与槽位。FEATURES_OK、QUEUE_READY 和 DRIVER_OK 写拒绝明确失败；DMA 准入
沿用架构 gate。未新增 LED/status queue、MSI-X 或设备类型支持。

完整生产驱动与生产 reset helper 在 MMIO/PCI transport、IRQ、allocator/cache 与
core 顺序模型中 1 方法通过，ASan/UBSan，0.435 秒。覆盖完整事件区与最后槽、同线
双设备及移除后保活、事件/配置读取、used idx 回绕、非法/重复/截断完成、partial
probe、IRQ 失败、活动调用关闭、超时保留、失联和重试。x86_64 SMP4 ABI=both 模块
构建 0.594 秒、无警告，24 个导入全部匹配框架导出；清理了原有日志宏重定义警告。

本次没有重跑 guest 或实体设备。既有 q35 SMP4 中两个 VirtIO 输入设备的正常绑定
记录保留在 `svga2-fifo/guest/guest.log`；该记录使用旧模块，不冒充新模块运行或
共享 IRQ 实机证据。新代码的停止/同线正确性来自上述生产源模型，正式候选仍归
N18/N20。证据 `build/evidence/v1-closeout-20261116/vinput-stop/report.json` 保存
冻结源码、公共接口、夹具、新模块、导入和日志。本轮 VirtIO input 软件缺口不再
保留；其余常见 PC/QEMU 路径按[当前范围](../../drivers/meta/v1-pc-qemu-scope.md)继续收口。


### PC speaker 端口归属与可失败停声（2026-11-17）

probe 在首次端口访问前登记 execution stop/release 并占用 PIT channel 2 与 speaker
control 两段声明端口，校验完整 16 位范围；任一 claim 失败不触碰硬件。初始化和
每次停声均读回 control 的 gate/speaker 两位，写拒绝或失联明确报错。先前 remove
忽略停声结果后直接释放实例，现 stop 先关闭新调用，锁忙返回 EBUSY，成功清除输出
位后才允许 class drain/remove 和 final release；失败保留实例、端口与绑定供重试。
停止管线使用可返回错误的 execution action，删除不再使用的旧 void shutdown。

最长五秒音调改为每次不超过 10 ms 的任务上下文延时并检查 closing/generation。
显式 stop 或新音调可结束旧等待；旧调用不再停止自己的后继音调。生命周期 stop
在活动调用仍持有 class 引用时先停声，旧调用观察关闭后退出，实例直到调用排空才
释放。tone/write/stop 的停声与启动失败向上返回，关闭后的新调用不访问端口。没有
增加 PCM、定时 IRQ 或新的音频能力。

整个生产模块在端口、延时、锁与 core 顺序模型中 1 方法通过，ASan/UBSan，
0.292 秒。覆盖 PIT 分频/模式、高位保留、正常 write/tone/stop、非法参数、partial
probe/action/claim 失败、停止读回拒绝、失联、锁忙、旧音调被 stop/新音调取代、
活动调用关闭及保留后重试。x86_64 SMP4 ABI=both 模块构建 0.576 秒、无警告，
21 个导入全部匹配框架导出。未追加 guest 或实体扬声器播放；原 q35 中旧模块正常
绑定记录保留，不作为新模块输出认证。正式候选仍按 N18/N20 的实际设备矩阵复核。

`build/evidence/v1-closeout-20261116/pc-speaker-stop/report.json` 保存冻结源码、
板级端口声明、公共接口、夹具、模块、导入和日志。本轮 PC speaker 软件清理缺口
已关闭；其余常见 PC/QEMU 路径继续按现有范围收口。


### CMOS RTC 稳定读取与端口归属（2026-11-17）

原读法只在字段读取前检查一次 UIP，读取被跨秒/跨年更新或抢占打断时可能拼接出
看似合法的错误日期。现在先后核对 seconds、UIP 和 register B 模式，并要求两份
完整原始样本一致后才解码；持续 UIP、秒值或模式变化在最多 20 ms 延时预算后拒绝
采纳，保留原有墙钟种子。未新增时钟写入、时区转换或 IRQ 跟踪。

索引端口不提供可依赖的旧 NMI 屏蔽值；QEMU 的读回为 0xff。删除从该读回伪造
“保存 NMI 状态”的逻辑，索引写入只带低七位，按普通 PC OS 的 NMI 启用策略保持
bit 7 为零。仍不写 CMOS data/SET 或其它时钟寄存器。依据为
[QEMU RTC 模型](https://raw.githubusercontent.com/qemu/qemu/v10.0.0/hw/rtc/mc146818rtc.c)与
[Linux PC RTC 端口访问](https://raw.githubusercontent.com/torvalds/linux/v6.12/arch/x86/kernel/rtc.c)；
跨秒/UIP 检查依据见[上游 RTC 读取协议](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/rtc/rtc-mc146818-lib.c)。

probe 在端口访问前 claim 完整 index/data 资源，拒绝越过 16 位窗口、同端口和缺失
数据端口的声明；支持原有两段端口和单段相邻端口布局。没有 DMA、IRQ、类回调或
后台任务，部分 claim/OOM 使用既有 devres 回滚，不人为增加异步 stop。
无效 12 小时零时值和 24 小时 PM 位明确拒绝；缺失/畸形 century 采用已声明的
20xx fallback 并正确记录。已知 century 的完整年份仍按 1970..2100 窗口拒绝，
不会把真实的 1960 年重新解释为 2060 年。

整个生产模块在端口/分配/墙钟 seed/core 顺序模型中 1 方法通过，ASan/UBSan，
0.286 秒；期望 epoch 由 Python UTC datetime 独立生成。覆盖跨年中断、样本之间
跨秒、读取后 UIP、秒/模式持续变化、BCD/binary/AM/PM、2000/2100 闰日、无效世纪
与日期、冻结时钟、全 ones、port claim 和 OOM 回滚。x86_64 SMP4 ABI=both 模块
构建 0.592 秒、无警告，导入全部匹配。未重跑 guest；原 RTC 正常启动记录保留，
`smoke-rtc-cmos-fallback` 的真实 guest 状态仍按其原记录，不把 host 覆盖写成已跑。

`build/evidence/v1-closeout-20261116/cmos-rtc-read/report.json` 保存冻结源码、
公共接口、夹具、模块、导入和日志。本轮 RTC 软件缺口已关闭，正式候选继续归
N18/N20，其余驱动按常见 PC/QEMU 范围处理。

后续冻结候选 `261deecb113e` 的 x86_64 q35/KVM 单 CPU 实跑中，新 CMOS 模块
读出 epoch `1794886522` 并被内核墙钟采用，随后 QEMU 正常 S5 退出 0。证据见
`build/evidence/v1-closeout-20261116/candidate-runtime/x86_64-bringup/`。这是正常
硬件读取与关机，不补记此前未执行的 fallback guest，也不声称用户态日期命令覆盖。


### Goldfish RTC 只读探针边界（2026-11-17）

Goldfish RTC 内核探针只作同步诊断；现在先检查两个 32 位时间寄存器的完整范围、
地址宽度和对齐，仅映射 8 字节，读取后立即解除映射。过短声明不再产生伪成功，
全宽资源不再让长度加一溢出。日志明确为 `epoch_ns`，没有接管用户 rtcd 的 alarm
或 IRQ，也没有新增异步 stop。原 remove 的重复 unmap 保持幂等。

只运行生产模块的定向 host 检查（ASan/UBSan，0.211 秒）和 RISC-V 模块构建
（0.639 秒、无警告），覆盖非法/最小/正常/全宽声明、映射失败、寄存器读取顺序、
纳秒拼接及退出。未重跑 guest，正式候选设备验证仍归 N18/N20。
证据为 `build/evidence/v1-closeout-20261116/goldfish-rtc-probe/report.json`。

后续冻结候选 `261deecb113e` 的 RISC-V64 单 CPU QEMU 实跑中，新模块绑定
`rtc@101000`，读出 `epoch_ns=1794886303158320000`，与宿主日志时间相差
0.108 秒；系统就绪后自然关机退出 0。
`candidate-runtime/riscv64-rtc/goldfish-bringup-observation.json` 记录这次正常读取
与关机。首次脚本误要求 bringup 提供 shell，其失败结果原样保留；从同一日志提取
的 RTC 观察不算用户态或块数据 I/O 通过，没有因此重跑一次 guest。


### 用户驱动输入探针窗口与退出（2026-11-17）

用户服务拥有的 VirtIO input 诊断探针原先固定映射 4 KiB，超过 AArch64 QEMU
实际 0x200 字节槽位。现在从 transport/config 的真实偏移计算 0x188 字节窗口，
校验资源长度、地址宽度与对齐；RISC-V 的 4 KiB 声明和 AArch64 的 512 字节声明
均适用，匹配失败和成功都同步解除映射，不触碰 status、queue 或 IRQ。

该无后台任务的诊断绑定补上明确的无副作用 remove。原先缺少 remove 会被
`driver_core_shutdown_supported()` 拒绝，导致合法的 QEMU 用户驱动配置无法关机；
现在允许退出，不通过 reset 破坏用户服务的设备状态。

生产探针及两个共享协议头的 host 检查通过（ASan/UBSan，0.230 秒），验证短资源、
映射失败、错误 identity、配置字符串、两种板级窗口和无副作用退出。AArch64 模块
构建通过（0.883 秒、无警告；模块规则实际 CONFIG_NR_CPUS=1）。首次夹具拼错 magic
常量、首次模块编译缺少 stdint include，均已修正；保留初次失败日志。没有追加 guest
或全量验证，候选范围仍归 N18/N20。
证据为 `build/evidence/v1-closeout-20261116/vinput-probe-window/report.json`。


### AArch64 模块冒烟使用实际设备（2026-11-17）

`smoke-drvmod-aarch64` 原先要求 Goldfish RTC 日志，但当前 AArch64 QEMU virt
固件与用户 MMIO 白名单只声明 PL031。门禁改为检查实际配置的启动盘模块：
`virtio-blk.a20drv` 加载、统一 driver core 绑定及非零容量 ready 三项同时存在，
并要求自然关机、进程退出码 0。仅有加载日志、旧 RTC 日志或超时不再算通过。

只运行实际 `report()` 的一个定向方法（0.127 秒），包含缺少各证据和超时/异常
退出的拒绝场景；`make check-smoke-cases` 0.995 秒通过，196 项可达。没有执行新
AArch64 guest，不把接线修复记为 N18/N20 完成。同步更正模块指南和设备状态页的
Goldfish 平台/单位说明。证据为
`build/evidence/v1-closeout-20261116/drvmod-aarch64-gate/report.json`。


### HDA 生产者暂停后的进度回收（2026-11-17）

HDA 原先仅在 write/drain/poll 采样消费位置；生产者返回后不再调用驱动时，循环
DMA 会重新读到已播放样本。现在每实例创建进度 workqueue/work，RUN 前确认已安排
回收，播放期间按 10 ms 目标间隔 trylock 并采样真实 LPIB/BCIS；不按墙钟猜测消费。
已播放区间及跨 ring 尾部的两段清零/sync，未播放区间保留；队列耗尽后确认 RUN=0，
停止重排。空闲无周期任务；没有新增 IRQ、混音、capture 或格式能力。

退出先关闭准入/重排，锁外 destroy/join 队列，再确认控制器 DMA 停止。join 超时或
硬件停止失败保留 work/context、绑定与 DMA，原实例重试成功后才释放。部分 worker
创建失败也使用这条受管管线。运行期 RUN 停止改为有界延时读回，寄存器失联明确
返回，时钟不前进也能结束超时。probe 的既有 PCM smoke 与 worker 共用 PCM mutex。

相关 HDA readiness 两项在首次组合运行中通过；lifecycle 首次因夹具错误地禁止
“另一个 writer 持锁期间 join”而失败，修正为只禁止持有 state_lock 后通过。随后
补上无 DMA 进展不回收和无时钟进展停止超时的直接断言，只重跑 lifecycle（0.541 秒，
ASan/UBSan）通过；保留原失败日志，不把初次聚合改写成成功。x86_64 SMP4 ABI=both
模块构建 0.591 秒、无警告。导入核对与冻结结果见
`build/evidence/v1-closeout-20261116/hda-progress/report.json`。

本轮没有追加 guest、WAV 或实体声卡播放；旧正常播放证据保持历史范围。新的整体
播放链路和候选配置仍归 N18/N20。10 ms 是任务调度目标，不保证任意调度停滞下的
音频连续性；软件回收修复不升级为 period IRQ 或实体时序认证。


### 常见 PC/QEMU 近期集成关闭（2026-11-17）

冻结候选分支的新 HDA、VirtIO input、PC speaker 在 x86_64 q35/KVM SMP4 完成
实际 PCM 写入/暂停生产/drain/STOP/重开、tone/STOP，以及 QMP 键鼠注入到 evdev
读取；正常 OpenRC 关机、QEMU 自然退出 0，25.032 秒。实际 HDA WAV 有 440 与
880 Hz 两段音调，生产者暂停 0.75 秒未持续重播旧数据。模块文件在来客逐一核对散列。

RISC-V64 的既有 `smoke-dual-input` 首次暴露服务重启丢 IRQ 唤醒：注册过早启用
中断，事件 watch 尚未建立。`f2062241b` 改为 masked 注册、watch 后首次 arm，
使用 oneshot 保持用户确认协议，并补 request_irq 失败计数回滚。两项定向宿主检查
通过；最终提交 `a50937976` 的真实入口中，探针识别键盘，用户服务两次读取按键、
确认队列 reset=0 后退出并正常关机，56.162 秒，无 runner traceback。普通退出
停止 DMA 由 `5203527b8` 补齐，不新增双态能力或任意 MMIO 崩溃认证。

共享模块输出未跟踪 NR_CPUS 等编译参数的问题同时由 `f1dd57537` 修复，配置切换
不再沿用旧内联 CPU 路径；相同配置保持增量构建。证据汇总在
`build/evidence/v1-closeout-20261116/candidate-pc-io/report.json`，适用范围与保留
失败见[候选记录](v1.0-candidate.md)。本领域独立缺口已清空；N18 关键并发与 N20 本地交付矩阵均已按候选记录关闭，当前候选云端验收仍归 N02。
