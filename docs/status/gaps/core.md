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
- 来源：[current/known-issues.md](../current/known-issues.md) 当前工程队列。
