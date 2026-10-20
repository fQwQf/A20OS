# ABI / IPC / SIGNAL 缺口

对应 V1-ABI。现状见 [current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)、
[current/native-abi-runtime-status.md](../current/native-abi-runtime-status.md)。

### ABI-01 ABI=linux 与 ABI=both 的行为交叉断言

- 现状：两种构建都有针对性用例；源码 guard 不比较运行时行为。
- 缺口：没有同一用例在两种档位下逐项比较成功、错误、权限与副作用。
- 关闭条件：同一 fixture 在两种档位运行，归一化结果与独立预期值一致；差异项有明确契约。
- 来源：requirements N03。

### ABI-02 IDL 版本协商的覆盖

- 现状：C/Rust 双绑定、重叠/不相交版本、畸形 ACK 与 legacy 回退在 RV64 与 x86_64 已测。
- 缺口：更广的保留架构/ABI，以及其他消息族的未知值契约未覆盖。
- 关闭条件：同样的精确线格式拒绝与 legacy 回退用例在保留架构/ABI 上通过；其他消息族的未知值有明确行为。
- 来源：requirements N04。

### ABI-03 Linux personality 的语义子集

- 现状：fd table、mmap/VMO、sockets、epoll 的 Native 原语实现存在。
- 缺口：这些子系统在 Linux personality 下的语义等价没有逐操作对照。
- 关闭条件：逐操作语义差异测试 + guest 负载；功能契约通过后才有性能结论。
- 来源：requirements N06。

### ABI-04 syscall 覆盖提升

- 现状：`kernel/abi/linux/syscall_coverage.md` 记录逐项等级；部分区域只覆盖正常路径。
- 缺口：边界情况、保留位、错误码未被执行，不能凭相邻 syscall 推断区域完成。
- 关闭条件：每批选一个部分区域，表内链接的边界/错误测试实际运行后更新覆盖。
- 来源：requirements N08。

### ABI-05 UFFD 与 signal 的剩余路径

- 现状：UFFD 的旧 C 竞态对照失败、Rust 路径通过；signalfd/signal 有针对性用例。
- 缺口：UFFD unregister/fault liveness、fd reuse、权限拒绝与对象生命周期的组合未完整覆盖。
- 关闭条件：unregister 后不再 fault、fd 复用不串对象、拒绝路径无副作用；SMP2 下通过。
- 来源：[current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)。

### ABI-06 keyring 的语义缺口

- 现状：add/replace/request/search/link/unlink 等已实现，属主与 `CAP_SYS_ADMIN` 检查生效。
- 缺口：存储的权限掩码不参与 read/search/link 约束；`KEYCTL_CLEAR`/`KEYCTL_INVALIDATE` 等未实现；无 `nsproxy`；capabilities 仅 15 个子集。
- 关闭条件：权限位真正生效；未实现命令的行为在文档里是明确契约而非遗漏；相关用例通过。
- 来源：[native-abi/keyring.md](../../native-abi/keyring.md)。

### ABI-07 子进程退出通知的完整性

- 现状：Linux `SIGCHLD`/wait 与 Native exit 事件有实现。
- 缺口：线程组、vfork、parent-death 与 `CLONE_PARENT` 的组合没有完整矩阵。
- 关闭条件：每种组合都有 guest 用例，退出与回收恰好一次。
- 来源：[kernel/linux-abi/child-exit-notification.md](../../kernel/linux-abi/child-exit-notification.md)。

### ABI-08 `a20_envelope_acquire_race_test` 未登记

- 现状：`check-abi-boundary` 报 `missing annotations: a20_envelope_acquire_race_test`；`check-envelope-coverage` 的生成器以 `unclassified syscall: a20_envelope_acquire_race_test` 退出。该符号是随 Rust 信封工作一起进来的测试钩子（`stamps.py`、`ENVELOPE_ACQUIRE_RACE_TEST`、`BUILD_VARIANT` 都有它的位置）。
- 缺口：新增的调用/测试钩子没有进 ABI 注解表与信封覆盖分类，两条 CI 宿主侧门禁为红。
- 关闭条件：该符号被登记并分类后两条门禁通过。
- 来源：DELIVERY 的 CI `toolchain-gates` job 本地复现（[current/delivery-baseline.md](../current/delivery-baseline.md) §6）。

### ABI-09 IPC 通道重现被禁止的 task 指针模式

- 现状：`check-task-state-boundary` 报 `task_t *waiter` 出现在 `kernel/ipc/a20_channel.c:491`，而该模式被门禁明确禁止（11 条断言中 1 条失败）。
- 缺口：task 状态的持有方式偏离既定边界，门禁为红。
- 关闭条件：断言在候选上通过，或按当前契约改写断言并有依据。
- 来源：同上。
