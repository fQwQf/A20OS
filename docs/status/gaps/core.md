# CORE 缺口

对应 V1-CORE。现状见 [current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)。

### CORE-01 长内核路径上的有界进展

- 现状：RT/调度预算与抢占已有实现；长 syscall 期间普通任务的有界进展没有断言。
- 缺口：线程化 IRQ 与长内核路径的有界延迟未完成，也没有风暴/故障用例。
- 关闭条件：普通任务进展断言在单核与 SMP 上通过，长 syscall 与 IRQ 风暴下不误触；延迟分布可复现。
- 来源：requirements N01。

### CORE-02 task identity 与生命周期收尾

- 现状：identity/构造/exit 切片为 opt-in，`TASK_OBJECTS_IMPL` 与 `TASK_EXIT_IMPL` 仍默认 C。
- 缺口：取消、超时、资源恢复、分配失败回滚与 exactly-once reap 的完整覆盖不足；C 分叉未收敛。
- 关闭条件：在 SMP2 上覆盖 fork/vfork、线程组退出、wait/cancel、部分发布回滚、引用复用与回收；默认实现的选择有明确结论。
- 来源：[current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)、requirements N01 相关行。

### CORE-03 陷阱风暴诊断

- 现状：用户 trap 与 fault 路径可终止出错任务；"修复后反复重陷"的注入用例缺失。
- 缺口：无法证明风暴会终止出错任务、同时不影响其他任务，也无法证明正常分页/COW 不被误判。
- 关闭条件：确定性修复/重陷注入命中，断言风暴记录、任务退出、无关任务继续、干净关机。
- 来源：requirements N19。

### CORE-04 锁拆分后的竞争未复测

- 现状：`proc_lock` 已删除，调度状态归 per-task `park_lock`，低频残余归 `tasklist_lock`；`g_net_lock` 已分片。
- 缺口：B1 基线（只打注册补丁、未拆分）从未采集，拆分后 `park_lock`/`tasklist` 的竞争没有采样；`-ENOTCONN` 窗口在 20 次引导里一次未命中。
- 关闭条件：采集 B1 基线，按同一协议复测拆分后的竞争并给出可分辨结论；`-ENOTCONN` 窗口要么命中并修复，要么给出排除依据。
- 来源：[current/server-readiness.md](../current/server-readiness.md) §八、[measurements/lock-after.md](../measurements/lock-after.md)。

### CORE-05 VFS 与 SM 争用测量

- 现状：现有负载只压网络锁，bucket/vnode/mount-table 锁未被并发触发。
- 缺口：VFS 锁在并发 open/write/unmount 下的争用未知。
- 关闭条件：SMP VFS 负载覆盖并发操作并有稳定的完成/计数断言，同种子可复现。
- 来源：requirements N17。

### CORE-06 futex PI 的窄窗口

- 现状：prepare 后 fork/COW 窗口已补持锁可写复查与解锁重试，`pi-cow-isolation` 在双架构单核/四核通过。
- 缺口：确定性窄窗口故障注入仍缺失，无法证明该窗口被覆盖。
- 关闭条件：注入在 COW 后、解锁重试路径上命中并验证不丢唤醒；负控失败。
- 来源：原 `known-issues.md` 当前工程队列。

### CORE-07 arm32 的 FP 上下文

- 现状：riscv32 与 ppc64le 已补 trap 帧的 FP 保存（ppc64le 仅编译验证）；arm32 未修。
- 缺口：arm32 用户态启用 VFP（`-mfpu=vfpv3-d16 -mfloat-abi=hard`），trap 帧与 `trap.S` 用逐字段断言强绑定，改动未验证；本机无 arm 工具链。
- 关闭条件：arm32 上 FP 上下文切换用例通过；或明确降级为不支持并写入平台契约。
- 诊断：[notes/core-arm32-fp.md](notes/core-arm32-fp.md)。

### CORE-08 宿主侧 proc 门禁与源码脱节

- 现状：`check-proc-wait-window`（`make host-tests` 的成员）在源码里找不到 `/* WAIT4_C_ENGINE_BEGIN */` 标记，直接 `ValueError: substring not found`；`check-task-lifetime-boundary` 断言 `proc_wait_timer_count_locked` 出现在 `kernel/proc/timer_heap.c`，该符号已不在那里（10 条断言中 1 条失败）。
- 缺口：两条 CI 宿主侧门禁在候选上为红，当前无法区分"实现回退"与"标记/符号改名"；`toolchain-gates` job 因此直接失败。
- 关闭条件：两条门禁在候选上通过，或断言改为不再绑定已改名的标记/符号。
- 来源：DELIVERY 的 CI `toolchain-gates` job 本地复现（[current/delivery-baseline.md](../current/delivery-baseline.md) §6）。

### CORE-09 通用内核代码里的架构条件编译

- 现状：`check-arch-boundary` 报 `kernel/core/smp.c:9` 与 `:189` 的 `#if defined(CONFIG_RISCV64)`、`kernel/include/proc/proc.h:623` 的 `CONFIG_ARM32`，以及 `kernel/mm/fault.c:67`/`:99`、`kernel/abi/native/channel_recv_txn.c:22`、`kernel/abi/native/handle_table.c:36` 的架构宏（6 条断言中 1 条失败）。
- 缺口：架构边界门禁为红；通用内核代码用架构宏分叉，越过了 `kernel/arch/**`、`kernel/platform/**`、`kernel/include/core/arch.h` 的边界。
- 关闭条件：这些分叉改走 `kernel/include/core/arch.h` 的架构 hook 后 `check-arch-boundary` 通过。
- 来源：同上。

### CORE-10 调度器派发所有权的宿主探针中止

- 现状：`check-a20-tests` 的 `test_dispatch_ownership.test_steal_wake_at_source_unlock` 宿主探针以 `SIGABRT` 结束，不是夹具抽取失败。
- 缺口：该行为断言失败，当前无法区分夹具漂移与真实缺陷。
- 关闭条件：探针在候选上通过，或断言按当前契约改写并有行为依据。
- 来源：DELIVERY 的 `check-a20-tests` 基线（[current/delivery-baseline.md](../current/delivery-baseline.md) §5）。
