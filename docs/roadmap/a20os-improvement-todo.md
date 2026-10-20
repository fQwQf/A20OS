# A20OS 改进 TODO

1.0 的总范围与完成标准以 [发布计划](../releases/v1.0.md) 为准：本页不是全部工作包，
集群、虚拟化、运行时、桌面等已推进子系统还须按各自设计完成。Rust 重写与这些缺口一起
收口；既有行为和性能按 [对照规范](../releases/v1.0-validation.md) 验收。
LoongArch32/PPC64LE 已退出 1.0；其源码、实例、构建和 CI 清理归入 V1-DELIVERY。

只记录**尚未完成**的工程瓶颈与剩余工作（最后核实：2026-10，含 `wt/practical-readiness`
的一次全表复核）。条目在落地时即从本文删除，不保留已完成的 checkbox；实现细节与验证
入口留在源码注释、提交历史和事实文档
（[../testing-gates.md](../testing-gates.md)、
[../security/hardening.md](../security/hardening.md)、
[../../kernel/abi/linux/syscall_coverage.md](../../kernel/abi/linux/syscall_coverage.md)），
下一个量级的方向评估见 [next-horizon.md](next-horizon.md)。

2026-10-13 已转入完整构建、宿主测试和 QEMU 实际验证。已通过的工程项删除，
未覆盖条件继续保留；命令、配置与结果见
[验证记录](../status/history/validation-docs-completion-2026-10-13.md)，编码历史见
[阶段记录](../status/history/development-docs-completion-2026-10-13.md)。

面向服务器部署的当前能力边界与阻塞项排序见
[../status/current/server-readiness.md](../status/current/server-readiness.md)。

checkbox 表示实现里程碑，不表示运行结果已在当前提交复验；带日期的验证记录如何引用
见文末"验证环境说明"。

优先验收调度隔离与默认 ABI=both：前者关系普通任务的进展，后者覆盖默认发布配置。

## P0：距可投产最远的一项——内核抢占与 RT 限流

权限与周期预算已通过 `sched_stress` 的同 CPU 普通进展与 RT 恢复验收；
长 syscall 和真机边界仍需下述工作。
当前边界见 [../status/current/server-readiness.md](../status/current/server-readiness.md) §四。

- [ ] IRQ 线程化与长内核路径的抢占运行验收
  - `CONFIG_KERNEL_PREEMPT` 已在基线实现；IRQ 返回路径通过
    `kernel_preempt_at_irq_return()` 调度。`/proc/a20/perf` 的 per-CPU
    `kernel_preempts` 已接线，尚未在本轮采样。
  - 仍缺 IRQ 线程化，以及长 syscall 下低优先级任务能在有界时间内运行的断言。

## P0：默认 ABI=both 的运行验收

已运行的 handle、MM、deepen、contract 用例覆盖权限上限、区域约束、对象生命周期
和 `remap-owner-rollback PASS`，通过结果与配置见
[本轮验证](../status/history/validation-docs-completion-2026-10-13.md)。相关本地复验项已移除。

- [ ] 在云端复验 Native CI 与发布前置门禁
  - `.github/workflows/native.yml` 已接线；本轮只验证本地配方，未触发云端 CI 或发布。
  - RTC、服务注册表 RTC 重启和 UBD 依赖 RISC-V 私有 MMIO/IRQ，AArch64 明确跳过，
    跳过不计为覆盖。AArch64 PTY 与 timeout 新增入口本地已通过。
- [ ] `ABI=linux` 与 `ABI=both` 的行为交叉断言
  - 仍需对同一组语义在两种构建中运行并比较可观测结果；源码登记表和配置守卫
    检查不能代替此项。

## P0：混合内核改造（Native ABI 本体化）

改造定位、边界原则与阶段验收标准见 [../hybrid-kernel/03-refactor-plan.md](../hybrid-kernel/03-refactor-plan.md)。
阶段一、二已有阶段性实现与运行记录；IDL 常量/消息头生成已接线，但动态版本协商和
完整双端绑定仍需完成。早期里程碑通过不等于整个混合内核已达到 1.0 完成条件。

- [ ] 完成服务协议与 C/Rust 绑定
  - 在已有 IDL 基础上明确布局、长度、版本/未知位与协商语义，生成双端绑定并验证旧/新端互操作。
  - 固定消息头与常量生成不能替代该验收；范围见 [IDL 与人格层](../hybrid-kernel/05-idl-and-personality.md)。

- [ ] 阶段三：驱动双态部署框架 + IOMMU/DMA 真隔离
  - IOMMU/DMA 真隔离子项已达成（2026-09-24）：RISC-V IOMMU per-device domain 动态
    claim/map/unmap/release，fault queue 消费把 record 归属到 owner 并立即阻断设备
    bus mastering，`/proc/a20/iommu` 暴露 enabled/domains/maps/unmaps/faults/
    blocked_events/last_fault_* 计数器；`make smoke-iommu-udriver-isolation` 与
    `make smoke-iommu-discovery` 在 QEMU 10.0.13 riscv64 PASS。QEMU ≤10.0 的
    TR_RESPONSE PPN 未左移 10 位，探测同时接受规范与 legacy 编码（上游 master 已修，
    升级后可删兼容分支）。
  - 仍缺：
    - 同源双态契约测试。完整 DRVMOD 驱动（goldfish RTC、ubd 等）仍是独立实现，
      不是同一份源码的两种部署；
    - fault 消费的中断驱动化。当前由 `a20_device_get_info` 拉取 FQ，未接 MSI/WSI；
    - 多设备并发 domain。当前单实例 `g_user_domain`；
    - 非 PCI 设备（virtio-mmio）的 domain 绑定。
  - 完成条件：同一完整驱动源码双态部署通过同一契约测试；用户驱动 DMA 动态绑定
    per-device IOMMU domain，未授权访问产生并消费 fault。
- [ ] 阶段五：Linux 人格层在 Native 原语上重建（starnix 式对照）
  - 已完成起步（作为基线保留在此）：`a20_personality.h` 提供 pipe-shaped
    channel/EventQ facade，`smoke-native-personality` 验证写入、MESSAGE_READY、
    读取和关闭。
  - pipe 的跨消息字节流、部分读与 pending-ready 已有专项实现；仍需 fd 表、mmap/VMO、
    socket、epoll 等关键子集的完整语义 diff 与性能对照，不能由 pipe 样板外推。
  - 完成条件：同上。

## P0：并发与 SMP 就绪

FUTEX_WAKE_OP 的双架构单核/四核运行已有
[分支记录](../status/history/validation-docs-completion-2026-10-13.md) 和
main 集成记录（该轮记录已删除），移除“仅编译、尚未运行”的旧条目。
相关原子性、COW 与批次唤醒断言继续作为 Rust 迁移及 1.0 候选回归，不由此宣称所有 futex 交错已证明。

- [ ] 补锁拆分的 B1 基线与竞争复测
  - `park_lock` 已通过静态汇总 sink 接入 `/proc/a20/lock_contention`，包含调用点
    采样；任务释放不会给静态注册表留下悬空指针。7 个 slab cache 锁已在基线注册。
  - 仍需 roadmap §5.1 的 B1（仅注册、不改锁）历史基线，以及足够 RUNS 的同负载
    前后采样。当前编码阶段没有新增测量结果，不能回答锁拆分后的性能收益。
  - 工具与历史数据见 [perf-overhaul.md](perf-overhaul.md) 与
    [../status/measurements/lock-after.md](../status/measurements/lock-after.md)。

## P0：Linux ABI 正确性

- [ ] 把满足条件的 Linux syscall 区域从 `partial` 提升到 `full`
  - 当前证据：`kernel/abi/linux/syscall_coverage.md` 中 poll/epoll/select、
    eventfd/timerfd、futex、membarrier、process accounting、rseq、landlock、
    ioprio/pkeys、file handles、SysV/POSIX message queues、keyring、pidfd、
    LSM introspection 已提升为 `full`（门禁见
    [../testing-gates.md](../testing-gates.md) 的 poll/timer 边界语义一节）。
  - 仍为 `partial` 的区域：path and metadata、fd I/O and splice、process lifecycle、
    signals、memory management、scheduler、sockets、namespaces、capabilities、bpf、
    file advice/copy helpers、SysV/POSIX shm and memfd、fanotify、Linux AIO、
    driver modules、cross-process memory、mempolicy/NUMA、new mount API、io_uring。
    namespaces 区域已具备真实 mount / PID / user 三种命名空间对象模型
    （`CLONE_NEWNS`、`CLONE_NEWPID`、`CLONE_NEWUSER`，各自的 `setns` 与
    `/proc/<pid>/ns/*`），其余 ns 类型如实返回 `-EINVAL`；`nsproxy` 缺失、
    capabilities 仍只有 15 个子集，故保持 `partial`。
  - 完成条件：每个升级区域都在覆盖表条目旁列出对应测试。

## P0：MM、Page Cache 与文件映射

- [ ] 完善 `MREMAP_DONTUNMAP` 的源端再次缺页语义
  - 常驻页移动与已换出 PTE 的 fork/remap 引用保留已实现；真实换出、数据读回与槽回收
    已通过 RISC-V/AArch64 单核及四核门禁，见 [本轮验证](../status/history/validation-docs-completion-2026-10-13.md)。
  - DONTUNMAP 仍需独立实现与运行验收，不能用普通 remap 的通过结果替代。
- driver/device/bus registry 容量耗尽的 RV64 QEMU 验收已完成：注入 backing-array
  `krealloc` 失败后，三种注册均保留旧项、返回 `-ENOMEM`，记录结构化日志，并在重试后
  成功；移除测试项后数量回到基线。证据仅覆盖本轮 RV64 单核 QEMU，不代表其他架构或
  实机验收；registry backing array 按设计保留增长后的容量。详见
  [注册表容量验证记录](../status/history/validation-driver-registry-capacity-2026-10-13.md)。
## P1：内核核心锁收敛的后续（2026-10 feat/kernel-core-scalability 分支）

已落地：EventQ 反向索引 256 桶分锁、per-vnode 缓冲写锁、mount 表稳定
堆指针（umount 搬移指错 `vnode->mnt` 的正确性 bug）、vfile 表 128 桶分锁
（原 `g_file_lock` 单锁）、路径查找 errno per-task、slab per-CPU 对象数组、
timekeeping 读路径 seqlock、kswapd 式后台回收（`/proc/a20/oom` 的
`kswapd_*`）。设计文档锁序表与 [../status/current/server-readiness.md](../status/current/server-readiness.md)
§七点五已同步。实测（4 核 `smoke-smp-lock-contention`，net_stress 负载）：
`vfile_bucket` 全部桶合计 2 次争用 / 0 自旋，`proc: 4498 次 / 973 万自旋`
仍是压倒性热点。

已落地（USB）：class-9 hub 驱动（`kernel/drivers/usb/class/usb_hub.c`）。hub 的
下行端口被发布成第二条 `usb_hcd_t`，因此 hub 后面的设备走的是和根端口完全相同的
枚举代码，hub 再套 hub 只是嵌套。hub 不拥有控制器——下行设备的端点上下文在根控制
器里，所以所有数据通路都转发给父 HCD 并携带父 HCD 发出的 token；只有总线地址归
hub 所有，因此 hub 后设备的地址来自 core 的全局地址池。`usb_hcd_ops_t::init_slot()`
随之变成 `alloc_slot()`，返回一个 `usb_slot_t`（控制器 token / USB 地址 / 物理
端口三者分离），xHCI 的 `Address Device` 也据此能填 slot context 的 Hub 字段。

已落地（PCI）：MSI-X 消息中断（`kernel/drivers/bus/pci_msix.c`）。能力表位置
的解析同时支持 Vector Control（PCIe 编码：BIR `3:1` + 偏移 `31:12`，字节偏移要
`<<4`）与 Message Address Lower（pre-PCIe 编码：BIR `2:0` + 偏移 `31:3`，不缩
放），因为 `-kernel` 引导没有任何固件跑过、Vector Control 全零，而 QEMU 的
virtio-pci 与 e1000e 都把表位置写在后者里。BAR 号一律从能力读出，不再由驱动猜
（virtio 在 BAR1、e1000e 在 BAR3，virtio spec 未规定）。`queue_msix_vector` 是表
索引不是中断号。x86_64 侧向量窗口 `0xD0..0xF0`、LVT 按 `LAPIC_LVT_TIMER +
((V-0x10)&0xFF)*16` 定位。消息地址必须是 APIC 页基址，不能把向量 OR 进去——
真机会忽略页内偏移，但实现不保证，前 1 KiB 落在寄存器文件里，结果是表项编程正
确却一个中断都不来。

- [ ] MSI-X per-function/per-queue 的细粒度接口与 cmdline 亲和性策略。
      当前全局 CPU id 接口与 x86_64 目标 CPU 投递已实现；剩余是细粒度选择。
- [ ] MSI-X 的非 x86 平台实现。`arch_msix_message_address()` /
      `arch_msix_vector_setup()` / `arch_irq_msix_cpu_count()` 目前只有
      x86_64 有实现，其余架构返回失败（`arch_irq_msix_cpu_count()` 返回 1，只有
      boot CPU 合法），驱动干净地退回 INTx/轮询；riscv64/aarch64
      的目标中断控制器与 loongarch64 的 EIOINTC 仍需接线；PPC64LE 已退出 1.0。
- [ ] e1000e 的 MSI-X 只验证到表被解析并 arm。门禁里网卡收不到流量，两个向量没有
      真实投递；真实投递的那一路是 virtio-blk。见
      [../testing-gates.md](../testing-gates.md) "MSI-X 消息中断"。
- [ ] hub 后置设备的真机验证。QEMU 的 `usb-hub` 自 QEMU 9 起不再创建下行 bus，
      且不实现端口复位，任何设备都放不到它后面；门禁只能证明 hub 被正确识别、
      描述符被正确解析（hub 描述符必须用类请求 0xA0 取，标准 GET_DESCRIPTOR 会
      被 stall）、下行总线被正确注册。见
      [../testing-gates.md](../testing-gates.md) "USB hub"。
- [ ] USB 拔线路径缺门禁。`usb_disconnect_port()` 已按端口调用 `abort_slot()`，
      hub 会同时归还总线地址；但"拔出设备后端点与 urb 的释放"没有运行门禁。

已落地（文件系统持久化）：block cache 的 pre-sync hook 与它的 owner 绑成一个
API（`bcache_set_sync_hook(bc, hook, owner)`），因为 hook 回到文件系统私有状态
的唯一路径就是 `bc->owner`，此前该字段声明了、被 ext4 与 FAT32 的 hook 读了、
却没有任何地方写入，于是 FAT32 的 FSInfo 回写与 ext4 的 journal 提交都是死代码。
ext4 的 JBD2 现在真的给出 ordered 语义：提交指针按事务大小推进、descriptor 的
checksum 在 tag checksum 回填之后才计算、数据 checksum 只记在 descriptor tag
里（写进块尾会污染 block bitmap 的哈希区）、挂载时以 journal superblock 的
`s_start` 而非仅 `EXT4_FEATURE_INCOMPAT_RECOVER` 判断是否重放。
验证：`make smoke-ext4-journal`，在四个提交序列断点真的停机、用同一镜像重启、
再用宿主 `e2fsck -fn` 双向把关。

- [ ] ext4 JBD2 仍缺的特性位。`barrier`/`async_commit` 未声明（`s_features` 对应位
      不置位，因此不写 `JBD2_FLAG_ASYNC_COMMIT`），设备级 `ordered` 与
      `journal_data` 无区分；`fast_commit` 未实现；descriptor revoke 记录已实现
      但只用于回放期的重复块判定，未用于延迟回滚。
- [ ] JBD2 checksum v1/v2（`COMPAT_CHECKSUM`）只 fail closed 返回 `-EOPNOTSUPP`。
      mke2fs 的默认配置产出 v3，因此常见镜像不受影响，但老镜像会拒绝挂载而不是
      降级只读；需要一个显式的降级策略而不是直接失败。
- [ ] ext4 文件数据块本身无校验和。`metadata_csum` 覆盖 group descriptor、extent
      树与目录项，JBD2 checksum 覆盖事务传输，数据块内容不在任何校验和内；
      静默位翻转只能靠上层应用发现（server/数据库部署下的实际风险）。

- [ ] 锁竞争复测缺 vfs 形态负载：`smoke-smp-lock-contention` 目前只跑
      net_stress；分桶写锁、per-vnode 写锁、mount 表在多核文件负载下的
      竞争数字没有门禁形态（需要并发 open/write/unmount 的 smp 变体）。
- [ ] 其余 smoke 门禁仍是 NR_CPUS=1（vfs-stress 已有 smp2/smp8 变体，
      见 [../status/current/server-readiness.md](../status/current/server-readiness.md) §四）。

## P1：跨架构正确性（2026-10 feat/kernel-core-scalability 分支）

- [ ] trap storm 检测的故障注入验收
  - 通用 user-trap 路径已对同一任务、PC、VA、cause 在一秒内重复 4096 次的缺页
    输出 `[TRAP-STORM]`、寄存器与 PTE，并以 SIGKILL 退出整个故障进程。
    syscall 或不同同步异常重置序列，IRQ 不重置；`trap_storms` 为 per-CPU 计数。
  - 仍需注入“修复成功但硬件反复陷入”的故障，验证诊断出现、进程终止且其他任务
    继续运行；同时验证正常按需分页/COW/权限异常不误报。本轮未运行此项。

## P2：仓库卫生与依赖边界

- [ ] 保留平台的发布配置与工具链复核
  - main 集成记录（该轮记录已删除）已记录 x86_64、LoongArch64、
    RISC-V32 等内核构建；这些是指定工作区的构建证据，不是完整运行或所有板级零警告证明。
  - 在候选工具链上复核四条 64 位主线及保留的 ARM32/RISC-V32/ARMv7-M 范围，记录
    配置、警告策略和产物；缺工具链或实板仍列待验收。
  - LoongArch32/PPC64LE 不再作为待补支持项，转为 V1-DELIVERY 的活跃配置清理。

## 验证环境说明

- 工具链和 QEMU 可用性由对应 build/smoke 目标在运行时报告，本文不固化某一台 host
  的工具缺失状态。
- 本文按 2026-09 源码核对。文中的验证记录均为历史记录，PASS 只表示它在标注的日期
  与配置下通过，引用为当前结论前必须在当前提交上重新运行。
- Proc/Sched 的当前聚合门禁是 `make check-doc-test-gates`，包含构建与 QEMU 运行；双架构 debug/release、
  1 核/8 核运行矩阵是 `make check-proc-step8-local`。
- 需要完整双架构运行矩阵时运行 `make check-proc-step8`，它聚合 RISC-V64 与
  LoongArch64 的 debug/release、单核/八核压力矩阵。
- 项目 Python 命令统一使用 Python 3 标准库；长跑基准入口负责记录 QEMU 命令、
  镜像哈希、退出状态、timeout 与 guest CPU 状态。
- NOMMU 支持集合由构建入口和 `smoke-arch-mmu-matrix` 验证，不以本文中的历史成功
  列表代替当前运行结果。
