# A20OS 改进 TODO

1.0 的总范围与完成标准以 [发布计划](../releases/v1.0.md) 为准：本页不是全部工作包，集群、虚拟化、运行时、桌面等已推进子系统还须按各自设计完成。Rust 重写与这些缺口一起收口；既有行为和性能按 [对照规范](../releases/v1.0-validation.md) 验收。 LoongArch32/PPC64LE 已退出 1.0；其源码、实例、构建和 CI 清理归入 V1-DELIVERY。

本文记录尚未完成的工程瓶颈与剩余规划。实现细节与验证入口见事实文档（[../testing-gates.md](../testing-gates.md)、[../security/hardening.md](../security/hardening.md)、[../../kernel/abi/linux/syscall_coverage.md](../../kernel/abi/linux/syscall_coverage.md)），远期方向评估见 [next-horizon.md](next-horizon.md)。

面向服务器部署的当前能力边界与阻塞项排序见[../status/current/server-readiness.md](../status/current/server-readiness.md)。

checkbox 表示实现里程碑，不表示运行结果已在当前提交复验；带日期的验证记录如何引用见文末"验证环境说明"。

优先验收调度隔离与默认 ABI=both：前者关系普通任务的进展，后者覆盖默认发布配置。

## P0：距可投产最远的一项——内核抢占与 RT 限流

权限与周期预算已通过 `sched_stress` 的同 CPU 普通进展与 RT 恢复验收；长 syscall 和真机边界仍需下述工作。当前边界见 [../status/current/server-readiness.md](../status/current/server-readiness.md) §四。

- [ ] IRQ 线程化与长内核路径的抢占运行验收
  - `CONFIG_KERNEL_PREEMPT` 已在基线实现；IRQ 返回路径通过`kernel_preempt_at_irq_return()` 调度。`/proc/a20/perf` 的 per-CPU `kernel_preempts` 已接线，尚未完成采样。
  - 仍缺 IRQ 线程化，以及长 syscall 下低优先级任务能在有界时间内运行的断言。

## P0：默认 ABI=both 的运行验收

已运行的 handle、MM、deepen、contract 用例覆盖权限上限、区域约束、对象生命周期和 `remap-owner-rollback PASS`，通过结果与配置见测试日志。

- [ ] 在云端复验 Native CI 与发布前置门禁
  - `.github/workflows/native.yml` 已接线；当前仅验证本地配方，未触发云端 CI 或发布。
  - RTC、服务注册表 RTC 重启和 UBD 依赖 RISC-V 私有 MMIO/IRQ，AArch64 明确跳过，跳过不计为覆盖。AArch64 PTY 与 timeout 新增入口本地已通过。
- [ ] `ABI=linux` 与 `ABI=both` 的行为交叉断言
  - 仍需对同一组语义在两种构建中运行并比较可观测结果；源码登记表和配置守卫检查不能代替此项。

## P0：混合内核改造（Native ABI 本体化）

改造定位、边界原则与阶段验收标准见 [../hybrid-kernel/03-refactor-plan.md](../hybrid-kernel/03-refactor-plan.md)。阶段一、二已有阶段性实现与运行记录；IDL 常量/消息头生成已接线，但动态版本协商和完整双端绑定仍需完成。早期里程碑通过不等于整个混合内核已达到 1.0 完成条件。

- [ ] 完成服务协议与 C/Rust 绑定
  - 在已有 IDL 基础上明确布局、长度、版本/未知位与协商语义，生成双端绑定并验证旧/新端互操作。
  - 固定消息头与常量生成不能替代该验收；范围见 [IDL 与人格层](../hybrid-kernel/05-idl-and-personality.md)。

- [ ] 阶段三：驱动双态部署框架 + IOMMU/DMA 真隔离
  - IOMMU/DMA 真隔离子项已达成：RISC-V IOMMU per-device domain 动态 claim/map/unmap/release，fault queue 消费把 record 归属到 owner 并立即阻断设备 bus mastering，`/proc/a20/iommu` 暴露 enabled/domains/maps/unmaps/faults/ blocked_events/last_fault_* 计数器；`make smoke-iommu-udriver-isolation` 与`make smoke-iommu-discovery` 在 QEMU 10.0.13 riscv64 PASS。QEMU ≤10.0 的 TR_RESPONSE PPN 未左移 10 位，探测同时接受规范与 legacy 编码（上游 master 已修，升级后可删兼容分支）。
  - 仍缺：
    - 同源双态契约测试。完整 DRVMOD 驱动（goldfish RTC、ubd 等）仍是独立实现，不是同一份源码的两种部署；
    - fault 消费的中断驱动化。当前由 `a20_device_get_info` 拉取 FQ，未接 MSI/WSI；
    - 多设备并发 domain。当前单实例 `g_user_domain`；
    - 非 PCI 设备（virtio-mmio）的 domain 绑定。
  - 完成条件：同一完整驱动源码双态部署通过同一契约测试；用户驱动 DMA 动态绑定 per-device IOMMU domain，未授权访问产生并消费 fault。
- [ ] 阶段五：Linux 人格层在 Native 原语上重建（starnix 式对照）
  - 已完成起步（作为基线保留在此）：`a20_personality.h` 提供 pipe-shaped channel/EventQ facade，`smoke-native-personality` 验证写入、MESSAGE_READY、读取和关闭。
  - pipe 的跨消息字节流、部分读与 pending-ready 已有专项实现；仍需 fd 表、mmap/VMO、 socket、epoll 等关键子集的完整语义 diff 与性能对照，不能由 pipe 样板外推。
  - 完成条件：同上。

## P0：并发与 SMP 就绪

FUTEX_WAKE_OP 的双架构单核/四核运行已有测试记录。相关原子性、COW 与批次唤醒断言继续作为 Rust 迁移及 1.0 候选回归，不由此宣称所有 futex 交错已证明。

- [ ] 补锁拆分的 B1 基线与竞争复测
  - `park_lock` 已通过静态汇总 sink 接入 `/proc/a20/lock_contention`，包含调用点采样；任务释放不会给静态注册表留下悬空指针。7 个 slab cache 锁已在基线注册。
  - 仍需 roadmap §5.1 的 B1（仅注册、不改锁）历史基线，以及足够 RUNS 的同负载前后采样。当前编码阶段没有新增测量结果，不能回答锁拆分后的性能收益。
  - 工具与历史数据见 [perf-overhaul.md](perf-overhaul.md) 与 [lock-serialization-split.md](lock-serialization-split.md)。

## P0：Linux ABI 正确性

- [ ] 把满足条件的 Linux syscall 区域从 `partial` 提升到 `full`
  - 当前证据：`kernel/abi/linux/syscall_coverage.md` 中 poll/epoll/select、 eventfd/timerfd、futex、membarrier、process accounting、rseq、landlock、 ioprio/pkeys、file handles、SysV/POSIX message queues、keyring、pidfd、 LSM introspection 已提升为 `full`（门禁见[../testing-gates.md](../testing-gates.md) 的 poll/timer 边界语义一节）。
  - 仍为 `partial` 的区域：path and metadata、fd I/O and splice、process lifecycle、 signals、memory management、scheduler、sockets、namespaces、capabilities、bpf、 file advice/copy helpers、SysV/POSIX shm and memfd、fanotify、Linux AIO、 driver modules、cross-process memory、mempolicy/NUMA、new mount API、io_uring。 namespaces 区域已具备真实 mount / PID / user 三种命名空间对象模型（`CLONE_NEWNS`、`CLONE_NEWPID`、`CLONE_NEWUSER`，各自的 `setns` 与`/proc/<pid>/ns/*`），其余 ns 类型如实返回 `-EINVAL`；`nsproxy` 缺失、 capabilities 仍只有 15 个子集，故保持 `partial`。
  - 完成条件：每个升级区域都在覆盖表条目旁列出对应测试。

## P0：MM、Page Cache 与文件映射

- [ ] 完善 `MREMAP_DONTUNMAP` 的源端再次缺页语义
  - 常驻页移动与已换出 PTE 的 fork/remap 引用保留已实现；真实换出、数据读回与槽回收已通过 RISC-V/AArch64 单核及四核门禁，见测试记录。
  - DONTUNMAP 仍需独立实现与运行验收，不能用普通 remap 的通过结果替代。
- driver/device/bus registry 容量耗尽的 RV64 QEMU 验收已完成：注入 backing-array `krealloc` 失败后，三种注册均保留旧项、返回 `-ENOMEM`，记录结构化日志，并在重试后成功；移除测试项后数量回到基线。证据覆盖 RV64 单核 QEMU，registry backing array 按设计保留增长后的容量。详见注册表容量验证记录。

## P2：仓库卫生与依赖边界

- [ ] 保留平台的发布配置与工具链复核
  - 历史构建记录已包含 x86_64、LoongArch64、 RISC-V32 等内核构建；这些是指定工作区的构建证据，不是完整运行或所有板级零警告证明。
  - 在候选工具链上复核四条 64 位主线及保留的 ARM32/RISC-V32/ARMv7-M 范围，记录配置、警告策略和产物；缺工具链或实板仍列待验收。
  - LoongArch32/PPC64LE 不再作为待补支持项，转为 V1-DELIVERY 的活跃配置清理。

## 验证环境说明

- 工具链和 QEMU 可用性由对应 build/smoke 目标在运行时报告，本文不固化某一台 host 的工具缺失状态。
- 各项验证以当前源码构建与运行时门禁结果为准。
- Proc/Sched 的当前聚合门禁是 `make check-doc-test-gates`，包含构建与 QEMU 运行；双架构 debug/release、 1 核/8 核运行矩阵是 `make check-proc-step8-local`。
- 需要完整双架构运行矩阵时运行 `make check-proc-step8`，它聚合 RISC-V64 与 LoongArch64 的 debug/release、单核/八核压力矩阵。
- 项目 Python 命令统一使用 Python 3 标准库；长跑基准入口负责记录 QEMU 命令、镜像哈希、退出状态、timeout 与 guest CPU 状态。
- NOMMU 支持集合由构建入口和 `smoke-arch-mmu-matrix` 验证，不以本文中的历史成功列表代替当前运行结果。
