# 子系统实现与验证状态

> 仍未完成的事项见 [1.0 缺口台账](../gaps/README.md)；本页只描述当前状态。

这是七个已集成模块分支的范围性状态，不是"操作系统或 1.0 验收已完成"的声明。源码集成加上一个针对性的通过用例，并不关闭模块完整的功能或平台目标。

| 领域 | 已集成的行为 | 覆盖范围与剩余限制 |
| --- | --- | --- |
| Signal | Rust 信号核心，接入 Linux/Native signal 与 signalfd。 | 已覆盖 guest 冒烟路径。三组成对 C/R 测量是小样本且区间重叠，不构成稳定的性能优势结论。 |
| ELF / exec | Rust ELF 规划与 exec/kexec 事务路径已集成。 | 覆盖 RV64 与 AArch64 的针对性 guest 用例以及 host fixture 重放。真实 mlibc `ld.so`/`libc.so` 动态程序、exec 继承、kexec staging 与 RV64 NOMMU 静态 ELF exec 已 guest 验证；RV64 QEMU virt 另有 Linux/Native 双路径 SMP kexec handoff 验收，其他平台仍无执行后端。其它 NOMMU 用户态运行未独立验收，ARM32 历史 fork/reap 限制未复验；当前构建与运行范围分别见候选矩阵。见 [native-runtime-1.0.md](native-runtime-1.0.md)。 |
| Filesystem | Rust 拥有部分复杂 VFS、UXFS、procfs 与缓存状态迁移；C 快路径在合适处保留。 | 针对性的缓存对照在最终同档样本中未发现稳定的 rename/unlink 回退。ISOFS 压力覆盖不完整；NTFS 的 crash/remount 在两种实现上都失败；ext4 无日志时的 deferred-orphan EIO 仍未解决。见 [一致性限制](../../fs/fs-consistency-model.md)。 |
| Linux 与 Native IPC | Rust 拥有已接受的 MQ、SysV、Landlock/seccomp、Native IPC 以及 wait/fault 事务切片。 | 组合的 RV64 ABI=both SMP2 guest 跑完三个针对性程序并以状态 0 自然退出。UFFD 的 unregister/close/fd 复用/权限拒绝组合在 SMP2 下通过；keyring 权限掩码与 CLEAR/INVALIDATE 在 Rust/C 两个 selector 上通过；子进程退出矩阵（线程组、vfork、CLONE_PARENT、PDEATHSIG、SA_NOCLDWAIT）在 SMP2 下通过；Linux/Native 归一化行为差分与 ABI=linux/ABI=both 交叉断言通过。性能样本仍有噪声。 |
| MM-VM | Rust 事务切片覆盖 VMA/VMO/COW、fork、回收、共享写 shutdown 与 fault 处理。 | 已覆盖十二个针对性冒烟用例。跨架构的运行时结论不成立。 |
| Process | Rust 拥有部分 task construction、有序 exit、wait/timer 与调度决策；task 存储、PID 分配以及部分 final release 仍为 C/可配置。 | 主机与 SMP2 覆盖是针对性集合。`TASK_OBJECTS_IMPL` 与 `TASK_EXIT_IMPL` 默认 C：task-object 切片按构造只对 Native ABI 生效（`A20_TASK_OBJECTS_ENABLED`），把它设成默认会让 `ABI=linux` 构建在解析期报错，所以这个开关按 ABI 选择而不是给默认值；两个 Rust 切片在 `smoke-proc-lifecycle-smp2` 的同一覆盖上通过（RV64 `ABI=linux` 的 exit 切片、`ABI=both` 的 objects+exit 切片）。默认值本身属于 delivery 合并的 GLOBAL 默认。见 [进程生命周期与调度](../../kernel/process-scheduler.md)。 |
| Network | Rust 拥有部分 socket 队列与连接状态事务。 | 一次 RV64 SMP2 运行完成了 4 路并行传输 × 4 轮 × 1 MiB 并以 0 退出。该证据来自混合构建，不关闭完整 `network_suite`、外部对端/NAT 或性能验收。 |

`tools/a20 run qemu-riscv64` 的普通 shell 门禁能进入 shell 并自然关机。此后交付工作包已在**干净树**上重跑这条门禁并重建了内核与用户态镜像，同时建立了 `check-a20-tests` 的基线并逐条分类，另跑了保留平台构建矩阵与 CI 逐 job 复现；结论、分类与仍然为红的门禁见 [交付基线](delivery-baseline.md)。[测试门禁](../../testing-gates.md) 与 [状态目录说明](../README.md) 仍是入口索引。

1.0 的目标是稳定、可用地完成已承诺的功能。在共享可变状态、争用、所有权、取消、回滚或并发不变量能从更清晰的控制中受益时，Rust 是合适的；简单的 C dispatch、glue、byte-copy 与成熟热点原语可以继续保留。 LOC 比例或删除 C 行数都不是验收门槛。整体发布阻塞与范围见 [1.0 计划](../../releases/v1.0.md)。
