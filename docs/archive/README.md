# docs/archive/：历史里程碑档案

本目录保留 A20OS 已完成里程碑的**审计快照**。这些文档记录各阶段收口时的设计结构、所有权模型与当时的回归覆盖，对理解子系统契约的演化仍有参考价值。

## 使用约定

- **冻结**：归档后不再随 HEAD 更新。文中的 PASS、测量数字、"已验证"和策略细节（如 MLFQ aging）只属于各文档标明的历史时点，不代表当前源码状态。
- **当前契约**：以 `kernel/include/` 头文件与正文档为准——调度与阻塞协议见 [../process-scheduler.md](../process-scheduler.md)，公平/延迟选择策略见 [../eevdf-scheduler.md](../eevdf-scheduler.md)。
- **门禁引用**：各篇开头的契约标记字符串（如 `TASK_LIFETIME_OWNERSHIP_AUDIT`）仍被 `make check-*-boundary` 静态门禁引用，用于防止契约标记被无意删除；引用它们不等于重新运行了对应测试。
- **复验**：任何结论如需作为当前事实引用，必须在当前提交上按 [../testing-gates.md](../testing-gates.md) 的入口重新运行。

## 内容

| 文档 | 里程碑 |
|------|--------|
| [task-lifetime-audit.md](task-lifetime-audit.md) | task 引用生命周期收口（`PROC.md` 第 3.5 步） |
| [blocking-point-audit.md](blocking-point-audit.md) | 阻塞族逐项审计（第 4 步） |
| [signal-exit-audit.md](signal-exit-audit.md) | 信号、停止与退出协议收口（第 5 步） |
| [timeout-ownership-audit.md](timeout-ownership-audit.md) | timeout/Park 所有权收口（第 6 步） |
| [smp-runqueue-audit.md](smp-runqueue-audit.md) | SMP runqueue 迁移与持久抢占（第 7 步） |
| [process-lock-split-audit.md](process-lock-split-audit.md) | 本地 picker 锁拆分与调度器热路径（第 8 步） |
