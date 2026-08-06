# A20OS 改进 TODO

本文档记录 A20OS 当前的工程瓶颈和剩余改进工作。它基于当前代码和文档，而不是未来设计意图。

## P0：混合内核改造（Native ABI 本体化）

改造定位、边界原则与阶段验收标准见
[../hybrid-kernel/03-refactor-plan.md](../hybrid-kernel/03-refactor-plan.md)。
本节只跟踪各阶段的工程完成度。

- [x] 阶段一：核心原语契约化（句柄 rights 代数、channel 背压、EventQ 语义、VMO 生命周期）。
  - 证据：`user/tests/test_native_contract.c` 全分区通过；副产品：修复
    `CHANNEL_ENDPOINT`/`EVENT_QUEUE` 类型掩码缺 STAT 导致句柄不可 query 的
    ABI 缺陷（`kernel/abi/native/handle_table.c`）。
  - 验证：`make smoke-native-contract`（riscv64 PASS）；loongarch64 构建通过。
  - 备注：验证期间发现 main HEAD（a7eb6d2）存在与本阶段无关的线程/阻塞
    路径挂起回归（见 `docs/hybrid-kernel/STATUS.md` 基线回归观察），既有
    线程类 native smoke 的回归验证在其收敛前受阻。
- [x] 阶段二：Native ABI SMP 正确性收口（native-shmring SMP=2/8 偶发破坏）。
  - 证据：SMP=2 连续 20 轮 + SMP=8 连续 20 轮零失败零挂起（2026-08-06，
    日志 `.kernel-build/smoke/shmring-smp{2,8}/`）；M5 修复 `98a1260`/
    `1af0d02` 复验有效，破坏不可复现。
  - 副产品：`[VMO-PAGE]` 串口诊断降级为 `/proc/a20/objects` 的
    `vmo_dirty_frames` 计数器（合法复用，消除输出交错）；记录 HEAD 的
    mm_stress 45s 门禁预算不足观察。
- [ ] 阶段三：驱动双态部署框架 + IOMMU/DMA 真隔离。
  - 证据：骨架已落地（`kernel/include/drivers/dual/`，设计文档
    `docs/hybrid-kernel/04-dual-placement.md`）；goldfish RTC 同源码
    双态运行（内核壳 boot probe + 用户壳 `smoke-native-rtcd` PASS）；
    virtio-input 第二样板完成，`smoke-dual-input` 验证两种部署读出
    相同设备身份；DMA ops 已进 drv_env（页粒度，信任模型）。
  - 完成条件：同一驱动源码双态部署通过同一契约测试；未授权 DMA 被
    IOMMU 硬件拒绝。待办：IOMMU 硬件强制 + 多页连续 DMA heap、
    设备所有权仲裁（virtqueue 事件面进入共享层的前提）、双态
    一致性测试从 probe 扩展到功能面。
- [ ] 阶段四：服务接口 IDL 化（替换 `user/svc/*_proto.h` 手写协议）。
  - 完成条件：svcmgr/registry 协议由 IDL 生成；手写 proto 头退出活跃树。
- [ ] 阶段五：Linux 人格层在 Native 原语上重建（starnix 式对照）。
  - 完成条件：选定测例集在直通实现与人格层实现下同通过，语义 diff 与性能对照归档。

## P0：并发与 SMP 就绪

- [x] 建立 tokenized Park/Wake，消除“检查条件后、真正睡眠前”的丢失唤醒窗口。
  - 证据：`kernel/include/proc/park.h` 的 `A20_PARK_WAKE_PROTOCOL` 与
    `kernel/include/core/sync.h` 的 `WAIT_QUEUE_PARK_PROTOCOL`；wait queue、
    futex、timeout 和 wake queue 都保存 task 引用及 `wait_seq`。
  - 验证：`make check-blocking-point-boundary`。
- [x] 收口 task 引用与异步所有权。
  - 证据：PID 查询使用 `proc_find_get()`；task list、PID table、runqueue、
    dispatch/current、wait/wake 和 timeout owner 都有显式引用交接；
    `/proc/a20/task_lifetime` 提供基线与错误计数。
  - 验证：`make check-task-lifetime-boundary` 和
    `make check-proc-step35-local`。
- [x] 统一信号、停止态和远程退出协议。
  - 证据：`signal_state.lock` 保护共享 action/pending 与 task mask 交接；
    `PROC_STOPPED` 使用显式 resume；远程退出发布 `exit_pending`，不再把任意
    blocked task 直接改为 READY。
  - 验证：`make check-signal-exit-boundary` 和
    `make check-proc-step5-local`。
- [x] 关闭 timeout heap 的引用、取消、过期和容量边界。
  - 证据：heap entry 保存 `(deadline, task, wait_seq)`；cancel/expiry
    唯一摘除；容量失败向 syscall 传播；压力测试覆盖 capacity±1 和 stale
    timeout isolation。
  - 验证：`make check-timeout-ownership-boundary` 和
    `make check-proc-step6-local`。
- [x] 建立 SMP runqueue 迁移和持久抢占请求。
  - 证据：`on_rq -> dispatching -> on_cpu` 所有权链；跨队列迁移按 CPU
    编号升序锁定；per-CPU `need_resched` 由安全点消费，IPI 只负责通知。
  - 验证：`make check-smp-runqueue-boundary` 和
    `make check-proc-step7-local`。
- [x] 从本地 pick 热路径移除全局 `proc_lock`。
  - 证据：`proc_runq_pick_local()` 只持本 CPU runqueue 锁完成
    `on_rq -> dispatching`，释放队列锁后调用者才获取 `proc_lock`；
    `/proc/a20/task_lifetime` 暴露 pick、争用和并行峰值。
  - 验证：`make check-process-lock-split-boundary` 和
    `make check-proc-step8-local`。
- [x] 修复低地址用户 `execve` 参数在 identity-mapped 架构上的来源误判。
  - 证据：`proc_exec()` 始终按用户指针复制 `argv/envp`；
    `proc_stress` 在 `0x02000000` 构造参数数组。
  - 验证：双架构 `PROC_STRESS: low-user-argv PASS` 和正式 CAgent 10/10。
- [x] 将仍依赖单线程执行的 MM 路径改为在 VMA 和页表修改期间持有 `mm->lock`。
  - 证据：`kernel/include/mm/vm.h` 说明部分路径仍依赖单线程执行或更窄的局部锁。
  - 完成条件：`make check-mm-lock-model` 包含 concurrent mmap、munmap、fault、fork COW 和 exit teardown 的行为测试。
 - [x] 为 close/read/write、dup/close_range、rename/unlink/open、symlink loop 和 mount/unmount 增加运行时 VFS 并发压力测试。
   - 证据：`kernel/fs/vfs.c` 在 VFS 并发 smoke 矩阵中将运行时扩展描述为未来工作。
   - 完成条件：`make check-vfs-abstraction` 能在这些竞争回归时失败，而不仅仅检查文档标记是否存在。
- [x] 用 EEVDF 替换 8 级 MLFQ，作为普通任务的统一调度核心。
  - 证据：`kernel/proc/sched.c` 实现加权 vruntime、系统虚拟时间资格门控、
    虚拟截止时间选择、空闲窃取与时间片旋钮；`/proc/a20/sched_base_slice`
    可运行时切换桌面/HPC；nice -20 相对 nice 19 获得约 10^5 倍 CPU，
    8 个忙任务在 8 核上全核分布。
  - 验证：`make check-doc-test-gates`、双架构 sched/futex/proc 压力、
    riscv64 8 核 `sched_stress` smp-runqueue + lock-split PASS。
  - 设计：`docs/eevdf-scheduler.md`。


## P0：Linux ABI 正确性

- [x] 只有在 syscall 组 smoke 测试和边界测试存在后，才将 Linux syscall 区域从 `partial` 提升到 `full`。
  - 证据：`kernel/abi/linux/syscall_coverage.md` 将 fd I/O、path、process lifecycle、signal、MM、futex、poll、socket 和 timer 标记为 partial。
  - 完成条件：每个升级区域都在覆盖表条目旁列出对应测试。
- [x] 用符合受支持 Linux 语义的行为替换 scheduler policy 和 affinity 近似实现。
  - 证据：`kernel/abi/linux/syscall_coverage.md` 将 scheduler API 标记为兼容性近似。
  - 完成条件：sched policy、priority、affinity 和 cgroup cpuset 行为被 LTP 风格测试覆盖。
- [x] 完成高级 futex 操作和内存顺序边界语义。
  - 证据：`kernel/abi/linux/syscall_coverage.md` 将 futex 标记为 partial；`kernel/abi/linux/sys_futex.c` 仍有不支持路径。
  - 完成条件：basic、requeue、private/shared、timeout 和 robust-list 场景都有覆盖。
- [x] 决定哪些显式 `-ENOSYS` Linux syscall 占位符仍在范围外，哪些应该实现。
  - 证据：`kernel/abi/linux/syscall_table.def` 包含 fanotify、signalfd、AIO、module、userfaultfd、perf 和 arch-prctl 占位符。
  - 完成条件：每个占位符都有记录在案的 owner 决策：实现、保留 stub，或从声明的兼容范围中移除。

## P0：MM、Page Cache 与文件映射

- [x] 为 `MAP_SHARED` 文件映射增加 dirty-page/writeback owner。
  - 证据：`kernel/include/mm/vm.h` 说明 `MAP_SHARED` writeback/truncate coherence 不完整。
  - 完成条件：shared mmap 写入按受支持语义在 read、fsync、truncate、fork 和 remap 路径中可见。
- [x] 加强跨 file read/write、mmap fault、truncation 和 reclaim 的 page cache eviction 与 coherence 测试。
  - 证据：`kernel/mm/fault.c` 和 `kernel/fs/page_cache.c` 暴露了当前 page-cache 限制和不支持路径。
  - 完成条件：page-cache 测试在内存压力下运行，并能捕获 stale data 或 use-after-free 回归。
- [x] 验证 fork、mprotect、munmap 和 OOM reclaim 下的 huge-page demotion 与 COW 行为。
  - 证据：`kernel/mm/vm.c` 实现 huge-page demotion 和 COW clone 路径；锁模型要求谨慎处理 TLB/refcount 顺序。
  - 完成条件：回归测试覆盖混合 huge/small 映射、write fault 和对 TLB flush 敏感的场景。
- [x] 让 OOM reclaim 策略可观察、可测试。
  - 证据：`kernel/include/mm/vm.h` 说明 reclaim 不得释放仍可从 task MM、page cache、VMO 或 Native handle 访问的 frame。
  - 完成条件：OOM 测试证明 safe kill/reclaim 行为，而不是只记录分配失败日志。

## P1：I/O 进展与网络

- [ ] 在块设备和网络设备能够发出完成信号的位置，用事件驱动 wakeup 替换 scheduler/idle 轮询进展。
  - 证据：`docs/project/external-dependencies.md` 描述了基于轮询的 lwIP 进展；`kernel/drivers/block/virtio_blk.c` 记录了未来 interrupt wake 路径。
  - 设计：`docs/drivers/lock-order.md`（驱动锁契约）、`docs/net/network-lock-contract.md`（deferred bottom-half 规则）；用户决策：deferred bottom-half / workqueue。
  - 完成条件：块设备和网络进展在正常运行中不再依赖通用 hot-path 轮询。
- [ ] 降低 `g_lwip_lock` 竞争，并为所有 socket 路径记录锁安全入口点。
  - 证据：`kernel/net/lwip_stack.c` 用全局锁串行化 lwIP 核心状态；`kernel/include/core/lock.h` 限制 lwIP 锁下的调用。
  - 设计：`docs/net/network-lock-contract.md`。
  - 完成条件：socket send/recv/connect/listen/accept 测试可并发运行，且没有锁顺序告警或饥饿。
- [ ] 用 board/network 配置管线替换仅适用于 QEMU 的网络地址默认值。
  - 证据：`docs/project/external-dependencies.md` 说明 `10.0.2.15`、`10.0.2.2` 和 `10.0.2.3` 只是开发默认值。
  - 设计：`docs/net/network-config-design.md`；用户决策：只使用命令行 / 运行时配置，不使用编译期板级默认值。
  - 完成条件：真实开发板或非 QEMU 后端无需硬编码 QEMU 假设即可配置 IP、gateway 和 DNS。
- [ ] 将网络 smoke 覆盖扩展到 wget 成功以外。
  - 证据：`docs/project/external-dependencies.md` 说明 TLSe/wget 不能证明存在完整现代 HTTPS 栈。
  - 设计：`docs/net/network-config-design.md`；计划新增门禁 `smoke-network-suite`。
  - 完成条件：DNS、UDP、TCP、ICMP、AF_UNIX、AF_ALG、timeout、partial I/O 和 error-path 测试都有独立门禁。

## P1：VFS 与文件系统语义

- [ ] 收紧 path resolution、symlink、permission、mount 和文件系统特定的 Linux 边界语义。
  - 证据：`kernel/abi/linux/syscall_coverage.md` 将 path 和 metadata 标记为 partial，并要求清理。
  - 设计：`docs/fs/vfs-edge-semantics.md`、`docs/fs/fs-consistency-model.md`；用户决策：完整 Linux `openat2` resolve flag 集合。
  - 完成条件：openat、renameat2、link/symlink、chmod/chown、statx、mount、umount 和 chroot 都有聚焦测试（新增到 `user/cmds/vfs_stress.c`），同时覆盖 openat2、xattr 和文件系统特定边界测试。
- [x] 将大型 VFS 实现重构为更小的 ownership、path、mount、fd 和 syscall-facing 单元。
  - 证据：`kernel/fs/vfs.c` 是承载 path resolution、open/close、mount、init 和兼容行为的大型中心实现。
  - 完成条件：每个单元都有窄 header 契约和子系统特定测试。
- [ ] 尽可能从通用 VFS 路径中移除硬编码运行时文件系统初始化。
  - 证据：`kernel/fs/vfs.c` 在 VFS bringup 期间初始化默认虚拟文件和类似环境的内容。
  - 设计：`docs/fs/fs-consistency-model.md`（ramfs / rootfs 一致性模型）；用户决策：构建期 rootfs overlay / initramfs 风格用户态镜像构造。
  - 完成条件：策略文件迁移到 init/userland image 构造，或迁移到声明式启动文件系统 manifest。
- [ ] 为 FAT32、ext4、ramfs、devfs、procfs、sysfs、pipe 和 anonfd 操作定义清晰的一致性模型。
  - 证据：`kernel/fs/` 包含多个文件系统后端，且 Linux ABI 入口点标记为 partial。
  - 设计：`docs/fs/fs-consistency-model.md`；每后端 unsupported-op errno 矩阵不属于 P1 范围。
  - 完成条件：后端能力差异记录在 `docs/fs/fs-consistency-model.md`；P1 不要求专用 smoke 门禁。

## P1：Native ABI 完成度与可维护性

- [x] 将过大的 Native phase-2 syscall 实现拆分为子系统所有的文件。
  - 证据：`kernel/abi/native/sys_phase2.c` 包含广泛的 memory、IPC、security、debug 和 system 功能。
  - 完成条件：Native syscall 文件映射子系统边界，每个文件只拥有窄 syscall 范围。
- [x] 完成 Native debug 语义，或明确缩小其范围。
  - 证据：`kernel/abi/native/sys_phase2.c` 将 debug 调用记录为有限兼容实现，不具备完整 stop/resume/watchpoint 行为。
  - 完成条件：debug handle 行为要么完整实现并测试，要么在 Native ABI 文档中记录为有意受限。
- [x] 完成 handle transfer、partial-delivery、temporal-rights 和 label consistency 测试。
  - 证据：`kernel/abi/native/handle_table.h` 和 `kernel/abi/native/handle_table.c` 引用了 partial-delivery 与 capability consistency 门禁。
  - 完成条件：Native IPC 测试覆盖成功 transfer、失败 transfer、被撤销权限、过期权限和 label denial。
- [x] 让 Native ABI 文档与活跃用户态运行时实现重新对齐。
  - 证据：`docs/native-abi/00-overview.md` 描述了已完成的 Native ABI 工作；活跃用户态也大量依赖 Linux ABI musl 构建。
  - 设计：`docs/native-abi/00-overview.md`、`docs/native-abi/08-runtime-status.md`、`user/archive/README.md`；用户决策：将 `liba20c` 更新为使用版本化 ABI 结构体。
  - 完成条件：文档区分活跃 Linux-musl 用户态、`liba20rt`、`liba20c`、已归档 A20 syscall bridge 代码和未来 Native POSIX shim 工作；`liba20c` 使用版本化 ABI 结构体，内核校验 `size`/`version`。

## P1：驱动与设备模型

- [x] 用动态大小或具备容量检查且能报告结构化错误的 registry 替换固定大小 driver/device/bus registry。
  - 证据：`kernel/drivers/core/driver_core.c` 为 bringup 使用有界静态 registry。
  - 完成条件：registry exhaustion 被测试，且不会静默丢失 device 或 driver。
- [ ] 在把驱动模型视为通用模型前，增加 hotplug 和 remove-path 生命周期测试。
  - 证据：`kernel/drivers/core/driver_core.c` 有 probe/remove 路径，但模型主要面向内建 bringup。
  - 设计：`docs/drivers/lock-order.md`；用户决策：面向用户的 `/proc/a20/driver_lifecycle` 触发器。
  - 完成条件：bind、probe failure、remove、re-probe 和资源清理都有测试。
- [ ] 将设备特定锁顺序移动到驱动文档中，放在每个私有锁旁边。
  - 证据：`kernel/include/core/lock.h` 要求新锁符合全局顺序，或记录局部顺序。
  - 设计：`docs/drivers/lock-order.md` 和已更新的 `kernel/include/core/lock.h` 注释（Wave 1 已完成）；inline `LOCK_ORDER:` 注释将在实现期间加入。
  - 完成条件：virtio-blk、virtio-net、UART、PTY、loop、SDIO 和平台 NIC 都记录各自私有锁规则。

## P2：测试门禁与工具

- [ ] 将静态 `rg` 风格架构门禁转换为行为测试，只要该行为能在 QEMU 下执行。
  - 证据：`docs/testing/testing-gates.md` 定义了可重复门禁，但当前若干检查验证的是文档标记而非运行时行为。
  - 完成条件：每个架构债务 TODO 都有运行时测试、构建矩阵测试，或有正当理由的静态-only 检查。
- [ ] 在声明更广兼容性前，为每个 Linux ABI 覆盖区域增加 LTP 风格分组 smoke 测试。
  - 证据：`kernel/abi/linux/syscall_coverage.md` 说明每个 syscall 组在升级级别前都需要 smoke 测试。
  - 完成条件：覆盖表生成包含测试目标名称和 last-known status。
- [ ] 将 Native ABI 测试扩展到最小进程启动和 libc smoke 以外。
  - 证据：`docs/testing/testing-gates.md` 将 `native-minimal`、`native-test` 和 `user/tests/test_liba20c.c` 列为 Native 覆盖；`user/tests/test_native_handle.c` 现在覆盖 handle dup/transfer，并接入 `make smoke-native-handle`。
  - 完成条件：Native handle、VMO/VMAR、channel、event queue、timer、task、debug 和 rights 测试在类似 CI 的目标中运行。
- [ ] 增加 memory pressure、fork/exec churn、fd churn、filesystem churn、network churn 和 process reaping 压力测试。
  - 证据：当前 smoke 目标证明基本操作，但不能证明长时间稳定性或竞争行为。
  - 完成条件：stress 目标带有有界 timeout，并在失败时捕获内核日志。

## P2：仓库卫生与依赖边界

- [x] 从活跃源码目录中移除或隔离 patch artifact 文件。
  - 证据：`kernel/proc/fork.c.orig`、`kernel/proc/fork.c.rej` 和 `kernel/abi/linux/sys_futex.c.orig` 等文件出现在活跃树中。
  - 完成条件：活跃源码目录只包含构建输入、文档或有意跟踪的 fixture。
- [ ] 除非测试明确是集成测试，否则不要把 vendored code 纳入第一方质量声明和测试。
  - 证据：`docs/project/external-dependencies.md` 将 lwIP、musl、sbase、mksh、TLSe 和 wget 的角色与 A20 集成工作区分开。
  - 完成条件：TODO 和状态文档一致地把 A20 的工作归功于集成，而不是上游 TCP/IP、libc、shell 或 coreutils 实现。
- [ ] 增加外部依赖升级 checklist 目标，自动运行相关 smoke 组。
  - 证据：`docs/project/external-dependencies.md` 要求修改导入用户态后运行 Linux syscall smoke、shell smoke 和 coreutils smoke。
  - 完成条件：修改 musl、sbase、mksh、lwIP、TLSe 或 wget 时，有记录在案的命令序列和预期 artifact。
- [ ] 记录哪些归档用户态代码只是历史参考，哪些预计会恢复。
  - 证据：`user/archive/` 包含 A20 syscall bridge、pthread/mutex 代码、测试，以及带 TODO 和 ENOSYS stub 的旧 coreutils。
  - 完成条件：归档路径从活跃状态声明中排除，或带 owner 和测试移回活跃树。

## 验证环境说明

- 文档不再固化某一台 host 的工具缺失状态。工具链和 QEMU 可用性由对应 build/smoke 目标在运行时报告。
- Proc/Sched 的当前累计静态门禁是 `make check-doc-test-gates`；双架构 debug/release、1 核/8 核运行矩阵是 `make check-proc-step8-local`。
- 需要同时验证正式比赛 workload 时运行 `make check-proc-step8`，它会追加 RISC-V64 与 LoongArch64 正式 CAgent。
- 项目 Python 命令统一通过 conda 环境 `a20os`；正式入口负责记录 QEMU 命令、镜像哈希、退出状态、timeout、guest CPU 与 judge 状态。
- NOMMU 支持集合由构建入口和 `smoke-arch-mmu-matrix` 验证，不以本文中的 历史成功列表代替当前运行结果。
