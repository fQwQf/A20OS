# A20OS 改进 TODO

只记录**尚未完成**的工程瓶颈与剩余工作（最后核实：2026-10，含 `wt/practical-readiness`
的一次全表复核）。条目在落地时即从本文删除，不保留已完成的 checkbox；实现细节与验证
入口留在源码注释、提交历史和事实文档
（[../testing-gates.md](../testing-gates.md)、
[../security/hardening.md](../security/hardening.md)、
[../../kernel/abi/linux/syscall_coverage.md](../../kernel/abi/linux/syscall_coverage.md)），
下一个量级的方向评估见 [next-horizon.md](next-horizon.md)。

**关于"移除已完成条目"**：`wt/practical-readiness` 那一轮把本文每一个未勾选条目
逐条对着该轮的代码 diff 核过一遍，**没有任何一条因此完成**，所以这一轮从本文删除的
条目数是 0。该轮实际落地的是两道新的宿主侧静态门禁与若干 Native ABI 正确性修复，
它们对应的"还欠什么"已经作为新条目写进下面的「默认 ABI=both 构建缺少运行时门禁」
——按本文的规矩，这些已落地的部分本身不该出现在本文里，缺的是门禁与运行验证，不是
功能。本版另外改写了一处**不是该轮造成**的过时：「P0：并发与 SMP 就绪」的
`proc_lock` 条目——它的对象已在并入本分支基线的锁拆分轮（`65bd609eb`）里删除，
原文对"剩余工作"的描述被该轮自己的设计文档
（[lock-serialization-split.md](lock-serialization-split.md)）标注为过时，现按
当前树改写，把真正欠的度量收口列为剩余工作。

面向服务器部署的当前能力边界与阻塞项排序见
[../server-readiness.md](../server-readiness.md)。

checkbox 表示实现里程碑，不表示运行结果已在当前提交复验；带日期的验证记录如何引用
见文末"验证环境说明"。

下面两条排在所有 P0 之前，因为它们都不是"功能还差点"，是"默认配置下没人验证过"
与"整机可能永久失联"。其余条目仍按原顺序。

## P0：距可投产最远的一项——内核抢占与 RT 限流

排在最前不是因为它最容易，而是因为它的后果最重：**一次用户态 RT 任务跑飞，在这台
机器上就是永久失联，而且没有任何东西会发现**。评估与证据见
[../server-readiness.md](../server-readiness.md) §四 开头与 §八 首行。

- [ ] 内核抢占点与 IRQ 线程化
  - 现状：无 `CONFIG_PREEMPT`（全树 grep 命中 0），也无 IRQ 线程化。调度器唯一的
    让出点是 `kernel/core/trap.c:589` 的 `proc_sched_safe_point()`，位于用户态陷阱
    出口；`rt_pick_best_locked()`（`kernel/proc/sched.c:350`）只对 `SCHED_RR` 轮转，
    FIFO 头选中即返回。
  - 后果：`SCHED_FIFO` 任务只要不阻塞、不返回用户态就不让出，长内核路径（TLB 收敛、
    页缓存同步、大段 copy）过不了 deadline。
  - 已知障碍：**改调度器的时序协议之前需要先有正式基准**，否则会把一个未定位的
    热点改成一个更难定位的热点。旧稿在这里写"`proc_lock` 的长持有成因至今未定"，
    该前提已随 `65bd609eb`（并入本分支基线的锁拆分轮）失效：那把锁已删除，替代的
    per-task `park_lock` 尚未注册进 `/proc/a20/lock_contention`，锁拆分之后的
    proc 侧竞争现状不可测（收口清单见 `docs/measured/lock-after.md` 末节）。
    QEMU TCG 下拿不到可信持锁时长的限制仍然成立。
  - 完成条件：长内核路径上的抢占可观测（`/proc/a20/perf` 有 per-CPU 抢占计数），
    且有一个门禁让"低优先级任务在 RT 任务持 CPU 时仍能在有界时间内运行"成为一条
    会红的断言。
- [ ] RT 限流与 `RLIMIT_RTPRIO`
  - 现状：`sched_rt_runtime_us` 在 `kernel/`、`user/` 下 grep 命中 0；内核侧没有任何
    一处定义或强制 `RLIMIT_RTPRIO`（该常量只出现在 `user/external/` 下的
    musl/mlibc/mksh 用户态头文件里）。`sys_sched_setscheduler()`
    （`kernel/abi/linux/sys_sched.c:319-321`）对 RT policy 只校验优先级 1..99，
    **不做能力判定**，任何进程都能把自己设成 `SCHED_FIFO` 99。
  - 后果：RT 任务可独占 100% CPU，无预算、无计量。
  - 完成条件：RT 运行时间按周期核算并在超限时 throttled 到 SCHED_NORMAL；
    设置 RT 策略需要 `CAP_SYS_NICE` 或 `RLIMIT_RTPRIO` 放行，两条路径都有门禁。

## P0：默认 ABI=both 构建缺少运行时门禁

`ABI ?= both`（`Makefile:117`），也就是说**上面两条之外，默认发布配置还有一类问题
根本没有任何门禁能发现**。本轮（`wt/practical-readiness`）补的是静态断言；运行时
那一半仍然空着。

- 现状（本次逐条核实）：
  - `tools/smoke_cases.py` 里名字带 `native` 的运行时门禁共 21 条，**全部**写死
    `ARCH=riscv64` + `ABI=both`；Native 运行时只有 riscv64 一个架构。
  - 本轮之前 `.github/workflows/ci.yml` 里 `native` 出现 **0** 次——21 条运行时门禁
    在主干上一次都没跑过。本轮接入 `smoke-native-contract` 一条。
  - 唯一的 ABI 冒烟门禁 `smoke-abi-linux` 构建的是 `ABI=linux`
    （`instances/smoke-abi-linux.toml`），与 `ABI=both` 不重叠。
  - 两个覆盖生成器（`tools/gen_linux_syscall_coverage.py`、
    `tools/gen_envelope_coverage.py`）只读 Linux syscall 表，对
    `A20_NATIVE_SYSCALL` / `A20_SYS_` 零引用。
  - 已补的静态断言：`make check-native-abi-coverage`（登记表 / 编号表 /
    `docs/native-abi/` 三表交叉）与 `make check-abi-config-guard`（禁裸
    `#ifdef CONFIG_ABI_*`，默认构建里 `CONFIG_ABI_LINUX` 无人定义，该写法恒假）。
    **这两条是源码侧的，不能替代任何架构的运行门禁。**
- [ ] 让已接线的 native 运行时门禁转绿，并把它接进发布流水线
  - 当前是红的：`make smoke-native-contract` 停在
    `user/tests/test_native_contract.c:466` 的 `vmol-leak-vmo`（`a20_vm_unmap` 之后
    objstat 的 `vmos` 没回到基线）。在 `git archive HEAD` 的干净副本上复现，失败点
    逐行相同，**是主干既有的 VMO 引用计数缺陷，与本轮改动无关**。
  - 完成条件：修好 `mm/` 的 VMO 释放路径，该门禁在 `ABI=both` 下通过并进入发布
    流水线（与 `release.yml` 现有的 smoke 前置一致）。
- [ ] 修好 `smoke-native-handle` 与 `smoke-native-mm`
  - 两条在主干上本就是红的（HEAD 干净副本上失败点相同）。2026-10-06 在已并入
    `wt/practical-readiness` 且带 `CONFIG_KERNEL_PREEMPT` 的 main 上重测，
    `smoke-native-handle` 的实际失败形态是**跑完第一个用例就在 power-off 前中止**：
    `missing ['part ok', 'tchan ok', 'bch ok', 'evq ok', 'opc ok', 'ac ok',
    'System is going down for power-off NOW']`——即 `part ok` 之后的下一个断言
    就没有再输出，且没有走到正常关机。此前记的"停在第一个 transfer 用例
    （`dup ok` 之后）"是更早提交上的形态，升级到抢占配置后提前得更靠前。
    `smoke-native-mm` 仍停在 `vm_map FILE`。后果是本轮新写的用户态断言
    （`handle_set_meta` 的 truncate/时间戳、`xattr` 的读写权限分离、`vm_advise` 的
    `MADV_NORMAL` 与四个 fork-policy advice）**一次都没有被成功执行验证过**——
    它们现在只是写下的断言，不是已验证的行为。
  - 完成条件：两条门禁在当前提交 PASS，且新增断言确实被执行到（不是在到达它们
    之前就失败）。
- [ ] 修好 `check-doc-drift` 的 4 条失效引用
  - 这是合并双 ABI 整改时**在主干上就红、且与该轮改动无关**的一条
    （在合并前的提交 `b21565373~1` 上重测，报错逐条相同）：
    `docs/net/checksum-offload.md:99`、`docs/net/checksum-offload.md:160`、
    `kernel/external/lwip/DIVERGENCE.md:441` 都把
    `include/standard-headers/linux/virtio_net.h:132` 指到了
    `kernel/include/drivers/net/virtio_net.h`（现仅 15 行）之外。
    `check-doc-citations` 扫 1240 条行内引用，这 3 条让门禁退出码为 2。
  - 完成条件：三条引用各自重指向到符号当前所在行，或在「引用文件而非定位」时
    去掉行号，让 `make check-doc-drift` 转绿。
- [ ] 把剩余 20 条 native 运行时门禁接进 CI，并扩到第二个架构
  - 需要先让 `tools/smoke_cases.py` 的 `argv` / `build.vars` 可按架构参数化
    （`qemu-system-$(ARCH)` 与 `AX=ARCH=…`），再在 `smoke` job 里开架构矩阵；只把
    某条用例复制成 riscv64/x86_64 两份会让同一份断言在两处漂移。
  - 交叉编译侧同样没有断言：`tools/targets-native*.mk` 里的 `native-<prog>-arch`
    目标 CI 一个都不调，非 riscv64 的 native 用户态能否编译成功没有任何门禁。
  - 完成条件：CI 的 smoke 矩阵里有 native 行，且至少两个架构各跑一遍同样的断言。
- [ ] `ABI=linux` 与 `ABI=both` 的行为交叉断言
  - 现状：没有任何一条门禁断言"同一份语义在两个 ABI 下结果一致"。本轮修的
    `CONFIG_ABI_LINUX` 恒假就是这条缺口的直接产物——它让 Linux 侧代码在默认构建里
    静默消失，而所有门禁都是绿的。
  - 完成条件：至少有一组断言对两个 ABI 各跑一次并比较同一份可观测结果
    （`/proc/a20/perf` 计数器或 procfs 输出）。

## P0：混合内核改造（Native ABI 本体化）

改造定位、边界原则与阶段验收标准见 [../hybrid-kernel/03-refactor-plan.md](../hybrid-kernel/03-refactor-plan.md)。
阶段一（核心原语契约化）、阶段二（Native ABI SMP 正确性收口）、阶段四（服务接口
IDL 化）已落地，已从本文删除。

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
  - 仍缺：fd 表、byte-stream accumulator、mmap/VMO、socket 等关键子集在直通实现
    与人格层实现下同通过，语义 diff 与性能对照归档。
  - 完成条件：同上。

## P0：并发与 SMP 就绪

- [ ] 收口 proc 侧锁拆分的度量（`proc_lock` 本体已删除，旧条目按当前树改写）
  - 已落地（`feat/lock-serialization-split`，并入本分支基线）：park/wake 协议早已
    是 per-task 锁（`kernel/proc/park.c` 全部经 `task->park_lock`，设计文档 §1.1
    核实并注明旧 TODO 描述过时）；`65bd609eb` 把剩余的切换发布路径从全局锁上摘掉
    并删除了 `proc_lock`（全树 0 处获取点、定义已删）——调度状态归 per-task
    `park_lock`，runq 成员关系归 per-CPU `runq_lock`，任务表迭代/OOM 扫描/聚合
    统计等低频残余归新 `tasklist_lock`（`kernel/proc/proc.c:56`，锁序
    `tasklist_lock -> park_lock`）。tokenized Park/Wake 语义逐位保留；三处刻意的
    语义弱化见 `docs/measured/impl-notes-proc.md`。设计、验收标准与门禁演进见
    [lock-serialization-split.md](lock-serialization-split.md)。旧条目引用的
    8 核热点数字（`proc_lock` 33335 次竞争 / 16191822 自旋）是**被删除的锁的
    历史**，不是现状。
  - 剩余工作（`docs/measured/lock-after.md` 末节自列，本文照录）：把 `park_lock`
    注册进 `/proc/a20/lock_contention`、补 roadmap §5.1 的 B1 基线（仅注册不改锁）、
    注册 7 个 slab cache 锁——没有这些，"拆分后 proc 侧竞争降到了哪"在当前树上
    不可回答。TCG 下 5 轮采样的噪声分辨率使多数活跃格子落在不可分辨带，
    提高 RUNS 是出结论的前提而非可选项。
  - 测量与 callsite 归因工具：见 [perf-overhaul.md](perf-overhaul.md) §3。

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

- [ ] 为 swap 的真实换出路径补运行门禁
  - 现状：`CONFIG_SWAP` 默认开启（NOMMU 与没有 swap PTE 编码的 riscv32/loongarch32
    由构建入口强制关闭）；`/dev/loopN` 可作 swap 目标，同一 bdev 重复 swapon 返回
    `-EBUSY`，`/proc/swaps` 渲染正确。`make smoke-swap` 覆盖 loop 绑定 → mkswap →
    swapon → `sysinfo` totalswap 与 `/proc/swaps` 断言 → 重复 swapon EBUSY → 触访
    匿名内存 → swapoff 归零。
  - 仍缺：门禁不驱动真实换出。OOM reclaim 每次最多换出 `MAX_SWAP_RECLAIM`=8 页且
    受 2s 冷却限制，1 GiB QEMU 冒烟无法现实触发；`swap_read_page`/缺页读回路径只有
    编译覆盖。
  - 完成条件：低内存实例（或可注入的换出阈值）下门禁真实触发一次换出-读回，并校验
    换出前后的数据一致性。
- [ ] 为 driver/device/bus registry 的容量耗尽补运行测试
  - 现状：`kernel/drivers/core/driver_core.c` 的 registry 已从初始容量起在锁保护下
    `krealloc` 扩容，扩容失败记录 capacity-exhausted 错误并返回失败，不再静默丢失
    注册项。源码已满足动态扩容与结构化失败要求，但全仓库没有专用的 exhaustion
    运行测试。
  - 完成条件：注入分配失败后断言注册项未静默丢失，且错误可从 `/proc` 或日志结构化
    读出。
- [ ] 补 `pty_stress` / `timeout_test` 的多架构入口
  - 现状：两个压力程序已分别由 `smoke-pty-stress` 与 `smoke-timeout-test` 覆盖
    （timeout 所有权门禁依赖后者），但都固定在 RISC-V64。
  - 完成条件：与 `smoke-futex-stress-aarch64`、`smoke-network-suite-aarch64` 同形，
    至少覆盖 aarch64。

## P1：内核核心锁收敛的后续（2026-10 feat/kernel-core-scalability 分支）

已落地：EventQ 反向索引 256 桶分锁、per-vnode 缓冲写锁、mount 表稳定
堆指针（umount 搬移指错 `vnode->mnt` 的正确性 bug）、vfile 表 128 桶分锁
（原 `g_file_lock` 单锁）、路径查找 errno per-task、slab per-CPU 对象数组、
timekeeping 读路径 seqlock、kswapd 式后台回收（`/proc/a20/oom` 的
`kswapd_*`）。设计文档锁序表与 [../server-readiness.md](../server-readiness.md)
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

- [x] MSI-X 的 IRQ 亲和性与 per-CPU 目标字段（x86_64）。消息地址现在按目标 CPU 的
      APIC ID 生成（`LAPIC_PHYS_BASE + (apic_id << 12)`，xAPIC 物理目的模式），
      每条目在 `pci_msix_state_t` 里记 `target_cpu`/`vector`，
      `pci_msix_set_affinity()` / `pci_msix_get_affinity()` / `pci_msix_set_all_affinity()`
      是驱动侧入口，`/proc/a20/irq_affinity` 是运行时入口。消息中断的 LVT 在**目标
      CPU 自己的** LAPIC 页里，所以给别的 CPU 编程必须在那里执行：新增 IPI 向量
      `IRQ_VECTOR_MSIX_VECTOR`，`arch_msix_vector_setup()` 在目标不是当前 CPU 时
      经它下发，改写顺序为 mask → 改消息地址 → 远端 arm LVT → unmask；地址回读
      不符时状态等同迁移前，远端 arm 失败时条目保持 masked 停在新地址（不丢中断，
      但该 vector 不通），批量迁移不是事务。
      默认目标仍是 boot CPU，逐位与改动前一致。门禁 `smoke-msix-x86_64` 现在以
      `-smp 2` 跑，写入 `/proc/a20/irq_affinity` 把全部条目移到 CPU1，断言投递行
      带 `cpu=1` 且读回表里目的已是 1。**该门禁已实跑通过**
      （`smoke-msix-x86_64: PASS`，日志 `.kernel-build/smoke/msix-x86_64.log`）。
      仍然缺的是：per-function/per-queue 的细粒度接口与
      cmdline 亲和性策略（现只有一个全局 CPU id），以及非 x86 平台的实现——见上一条。
- [ ] MSI-X 的非 x86 平台实现。`arch_msix_message_address()` /
      `arch_msix_vector_setup()` / `arch_irq_msix_cpu_count()` 目前只有
      x86_64 有实现，其余架构返回失败（`arch_irq_msix_cpu_count()` 返回 1，只有
      boot CPU 合法），驱动干净地退回 INTx/轮询；riscv64/aarch64
      的 GIC、loongarch64 的 EIOINTC、ppc64le 的 MPIC 都没有接线。
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

- [ ] fd 路径的最终形态仍是 per-process 直接存 `vfile_t*`（消掉 gfd 间接层
      与 `files->lock -> 桶锁` 链）。本次以分桶锁达到同数量级的去串行化，
      全量改造涉及 25+ 文件（epoll/readiness/eventq/file-locks 都拿 gfd 当
      全局标识），需要先给 readiness 的 GLOBAL_FD 语义找到替代。
- [ ] 锁竞争复测缺 vfs 形态负载：`smoke-smp-lock-contention` 目前只跑
      net_stress；分桶写锁、per-vnode 写锁、mount 表在多核文件负载下的
      竞争数字没有门禁形态（需要并发 open/write/unmount 的 smp 变体）。
- [ ] 其余 smoke 门禁仍是 NR_CPUS=1（vfs-stress 已有 smp2/smp8 变体，
      见 [../server-readiness.md](../server-readiness.md) §四）。

## P1：跨架构正确性（2026-10 feat/kernel-core-scalability 分支）

这一批的共同特征是**在 x86_64 上完全看不出来**，因此每条都必须按架构各跑一遍
`make smoke-ext4-journal ARCH=<arch>`，跑一个架构不算验证完。已修复的部分：

- [x] **`O_*` 常量按架构取值**（`af064443`）。`O_DIRECTORY`/`O_NOFOLLOW`/`O_DIRECT`/
      `O_LARGEFILE` 在 Linux 里逐架构定义，asm-generic、arm/arm64、powerpc 三套布局
      互不相同，且没有任何两套顺序一致。原先 `kernel/include/core/fcntl.h` 用了一套
      硬编码的 asm-generic 值，在 ppc64le 上 `O_DIRECTORY` 恰好等于 `O_LARGEFILE`——
      而 musl 的 `open()` 每次都带 `O_LARGEFILE`，于是 `openat()` 静默失败。
      这是 ppc64le 上 `smoke-ext4-journal` 从 FAIL 变 5/5 PASS 的直接原因。
- [x] **ppc64le stack-protector guard 改 global**（`e8bedcc4`）。GCC 在 ppc64le 上默认
      `-mstack-protector-guard=tls`（`ld 9,-28688(r13)`），而内核的 `__stack_chk_guard`
      是普通 `.data` 全局量、r13 又已被 `arch_set_task_pointer()` 占用。
- [x] **ppc64le trap prologue 自开 FP/VEC/VSX**（`5381bbbe`）。`__trap_from_user` 进入时
      SRR1 带的是用户态 MSR，用户没开 FP 就保存 FPR 会直接陷入。
- [x] **board-bound virtio transport 发布到设备模型 + probe 到槽位耗尽**（`a7a9eae4`）。
- [x] **drvmod 导出表补 `snprintf`/`vsnprintf`/`device_register`**（`eee0535f`）。
      缺一个符号的后果不是那个符号不可用，而是整个 `.a20drv` 加载失败、transport
      整个消失。注意它只在加载式 profile 下显形：virtio-blk 在 aarch64 是模块、
      在 ppc64le 内建。
- [x] **缺页判定改以 VMA 为准**（`ab700592`）。见下条。

- [x] **aarch64：mksh 在信号跳板处预取异常**。
      `handle_present_page_fault()` 原先只看 PTE 不看 VMA，叶 PTE 上带了一个 VMA
      没有声明的 `PTE_X` 就把 exec fault 判为「已处理」；aarch64 的
      `arch_pte_leaf()` 由 `PTE_X` 推出硬件 `UXN`/`PXN`，所以这个多余的 X 真的让
      该页在 EL0 可执行。结果是约 2.3 万次重复 prefetch abort、**零内核输出**的
      静默活锁（`ab700592` 已把它变成一次干净且指名道姓的 SIGSEGV）。

- [x] **aarch64：跳板页可写 → `SCTLR_EL1.WXN` 下不可执行**。
      `ab700592` 之后仍然每次交付 SIGSEGV。**根因不在缺页路径，也不在 TLB
      一致性**（那是此前最像的方向，已证伪）：aarch64 在
      `kernel/arch/aarch64/mm/kwx.c` 里把镜像切成 RO-X/RO-NX/RW-NX 之后
      **打开了 `SCTLR_EL1.WXN`**（bit 19），语义是「EL0 可写的叶描述符在 EL0
      一律 execute-never」。而 sigreturn 跳板按设计就写在**用户栈**上的信号帧里
      （`signal_deliver_user()` 把 `TRAP_CTX_RA(ctx)` 设成 `tramp_addr`），栈页按
      定义可写，于是只能映射成 AP=01，CPU 就拒绝取指。

      证据链（每一步都实测过，不要重走）：
      - QEMU `-d int` 给出原始 ESR：正常按需缺页是 `ESR 0x24/0x92000007` /
        `0x20/0x82000007`，**FSC=0x07**（translation fault，页不在）；跳板取指是
        `ESR 0x20/0x8200000f`，**FSC=0x0F**（permission fault）。同一个 walk 深度，
        一个是「不在」，一个是「在但没权限」——所以不是 TLB 陈旧。
      - 逐位对比同一进程里两个叶描述符，**权限位只差 AP**：跳板
        `0x3a000007f7...` 是 AP=01（EL0 读写），正常取指的 text
        `0x2a000007f7...` 是 AP=11（EL0 只读）；其余可执行性位（`PTE_X`=1、
        `UXN`=0、`PXN`=1、`nG`=1、`AF`=1、`AttrIndx`=1）逐位相同。
      - 把 `mm_pte_flags_make_writable_dirty()` 从跳板映射里去掉**没有用**：
        aarch64 上 `PTE_D` 与 `PTE_W` 是同一个 bit 56，而
        `arch_signal_tramp_pte_flags()` 自己就带 `PTE_D`，AP 仍然是 01。

      因此**没有任何标志组合能让同一页既是活栈又是可执行跳板**——这是设计问题，
      不是标志问题。修法：给跳板一块**专用只读页**（RO+X 正是 WXN 允许的组合），
      `arch_setup_signal_trampoline()` 在建 mm 时映射它、地址存进
      `mm->sig_tramp`，新增的弱钩子 `arch_signal_tramp_addr()` 让投递路径把
      `TRAP_CTX_RA` 指过去而不是指栈内槽位。x86_64 早就是这么干的
      （`X86_64_SIGRET_TRAMP_ADDR`），只是 aarch64 没跟上。
      **注意**：x86_64 的 `USER_VA_LIMIT` 是 2^47、aarch64 只有 2^46
      （`kernel/arch/aarch64/include/platform.h`），照抄那个常量会被 `mm_mmap()`
      以超范围拒绝——实测表现为 VMA 和 PTE 都没生成，RA 落在裸地址上，报
      "user fault on KERNEL address"。所以 aarch64 改用 `mm_find_gap()` 分配，
      不写死地址。
      结果：aarch64 `smoke-ext4-journal` 由 FAIL 变 **5/5 PASS**，
      mksh 在 `cat /proc/version` 这类会 fork 的命令后不再崩。

      已排除的方向（都有反证，不要重走）：
      - 那片「多余的 X」不是 bug，而是 `signal_make_page_exec()`
        （`kernel/proc/signal.c`）为了跑 sigreturn 跳板**故意**加的。
      - `pt_unmap()` / `pt_map()` 没有丢帧引用：`pt_unmap()` 明确不持有叶引用，
        由调用方释放，而 `signal_make_page_exec()` 原样复用 `pa`，没有 use-after-free。
      - 栈增长/brk 的 `handle_demand_fault_locked()` 已拒绝 exec fault；
        `mm_prot_to_pte_flags`、`PROT_*`、`pt_map_cls` 都逐个核过，均忠实于 VMA。
      - `telnetd` 只把 socket `dup2` 进子进程，不读控制台，与本问题无关。
      - PTE 写入与 `tlbi` 之间没有可见性/时序缺口：返回用户态路径本身就会
        `tlbi vmalle1`，且实测硬件重新 walk 后给出的是**权限**而非**翻译**故障。

- [ ] **aarch64 的 trap storm 类缺陷需要一条通用门禁。** 当前能发现它纯属偶然：
      活锁不产生任何日志，gate 只能靠超时发现，而超时无法区分「机器慢」和
      「内核活锁」。可考虑对 trap 计数设上限（同一 PC 连续 N 次缺页即判定失败并打印
      完整上下文），让这类缺陷在门禁里表现为一次明确的失败而不是挂起。
      本轮 aarch64 的两个缺陷正好说明必要性：先是一遍遍重复且**零输出**的取指异常
      （只能靠超时发现），修完又是一个每次必崩但信息齐全的 SIGSEGV。
      诊断侧已补上的两项能力是：QEMU `-d int` 的原始 ESR（FSC 足以区分「不在」与
      「没权限」）和 `dump_fault_pte()` 里同一进程内可执行 text 叶的对照——
      缺任何一项都定位不到这一层。

## P2：仓库卫生与依赖边界

- [ ] 跨架构 `-Werror` 复核
  - 当前证据（2026-10-04 复核，`check-kernel-build-all` 中可在本机执行的部分）：riscv64、
    loongarch64、aarch64、x86_64、riscv32、ppc64le 以及 VisionFive2 / LS2K1000 板级
    `kernel-only` 全部零警告通过。
  - riscv32 原有的 11 处 `-Wint-to-pointer-cast`（`kernel/ipc/kexec.c`、
    `kernel/syscall/trace.c`、`kernel/abi/linux/sys_proc.c`、
    `kernel/abi/native/sys_core.c`、`kernel/abi/native/sys_native_fs.c`、
    `kernel/abi/native/sys_native_mm.c`）已全部改经 `uintptr_t` 中转，
    `kernel/proc/proc.c` 的 `proc_mmap` 区间判定也已改用 `(long)`。
    **实测 riscv32 `kernel-only` 零警告通过（2026-10-04）**，本项完成。
  - 仍缺：
    - arm32、loongarch32、armv7m 在当前主机缺交叉工具链，未复核。
      arm32 与 loongarch32 由 CI 覆盖（arm32 工具链在镜像里，loongarch32 需从源码
      构建 cloudspurs 工具链），armv7m 由 STM32 目标覆盖。
  - 完成条件：在有工具链的主机复核 arm32 / loongarch32 / armv7m。

## 验证环境说明

- 工具链和 QEMU 可用性由对应 build/smoke 目标在运行时报告，本文不固化某一台 host
  的工具缺失状态。
- 本文按 2026-09 源码核对。文中的验证记录均为历史记录，PASS 只表示它在标注的日期
  与配置下通过，引用为当前结论前必须在当前提交上重新运行。
- Proc/Sched 的当前累计静态门禁是 `make check-doc-test-gates`；双架构 debug/release、
  1 核/8 核运行矩阵是 `make check-proc-step8-local`。
- 需要完整双架构运行矩阵时运行 `make check-proc-step8`，它聚合 RISC-V64 与
  LoongArch64 的 debug/release、单核/八核压力矩阵。
- 项目 Python 命令统一使用 Python 3 标准库；长跑基准入口负责记录 QEMU 命令、
  镜像哈希、退出状态、timeout 与 guest CPU 状态。
- NOMMU 支持集合由构建入口和 `smoke-arch-mmu-matrix` 验证，不以本文中的历史成功
  列表代替当前运行结果。
