# 信号、停止与退出协议审计

`SIGNAL_EXIT_PROTOCOL_AUDIT`

这是 `PROC.md` 第 5 步的验收记录。信号不是泛型对象事件：信号产生首先在信号锁下记录持久待决状态，然后模式兼容的 Park 唤醒可能消耗当前序列。作业控制停止保持在 Park 之外，远程退出保持为持久请求，直到目标到达安全边界。

## Park 模式与唤醒原因

| 等待模式 | 普通可投递信号 | 致命信号 | 任务退出请求 | 对象事件/关闭 |
|---|---|---:|---:|---:|
| `INTERRUPTIBLE` | 唤醒 | 唤醒 | 唤醒 | 唤醒 |
| `KILLABLE` | 不唤醒 | 唤醒 | 唤醒 | 唤醒 |
| `UNINTERRUPTIBLE` | 不唤醒 | 不唤醒 | 不唤醒 | 唤醒 |

`PROC_WAKE_SIGNAL`、`PROC_WAKE_FATAL_SIGNAL` 与 `PROC_WAKE_TASK_EXIT` 不同于现有的对象关闭 `PROC_WAKE_EXIT`。这防止设备或 pipe 关闭被误认为任务终止。`proc_park_prepare_locked()` 还会在发布令牌后检查已待决的退出、致命或可投递普通信号，从而关闭竞态的「准备前已待决」一侧。

eventfd 与 timerfd 等待可以安全取消，因为其中断的读或写之后，其持久计数器/定时器状态和带令牌的等待条目仍然有效。因此它们使用 `INTERRUPTIBLE`，并对任务中断唤醒原因返回 `-ERESTARTSYS`。Virtio 块请求保持 `UNINTERRUPTIBLE`：在设备完成它之前，in-flight 请求与栈拥有的 completion 不能放弃。

## 信号状态串行化

`SIGNAL_STATE_LOCK_CONTRACT` 保护：

- 共享信号动作、进程待决位、siginfo 存在性与 siginfo；
- 每任务阻塞掩码与线程待决位；
- 临时掩码、`sigsuspend` 与 `sigtimedwait` 交接状态。

唯一允许的嵌套是 `proc_lock -> signal_state.lock`。信号产生首先取得信号锁，释放它，然后才在 `proc_lock` 下尝试 Park 唤醒或 STOPPED 转移。调度器停止/恢复与 Park 准备已经持有 `proc_lock`，可以按文档化顺序查询信号状态。

`SIGNAL_MASK_PARK_PROTOCOL` 覆盖 `sigsuspend`：临时掩码在持有 `proc_lock` 时发布，重查待决状态，并在释放该锁之前准备 Park 令牌。`sigtimedwait` 类似地在准备令牌之前发布其等待掩码。即使信号对普通处理器投递被阻塞，匹配的信号仍会唤醒等待者。

## STOPPED 状态

默认停止动作只在 `proc_sched_stop_current()` 中进入 `PROC_STOPPED`。在发布该状态之前，该路径拒绝竞态的待决 `SIGCONT`、致命信号或退出请求。普通信号保持待决，无法恢复任务。

`proc_sched_resume_stopped()` 是唯一恢复路径：

- `SIGCONT` 恢复并记录一次继续事件；
- 致命信号在没有继续事件的情况下恢复，以便它能在下一个信号边界终止；
- 任务退出请求在没有继续事件的情况下恢复，以便安全边界退出检查可以运行。

停止与继续事件唤醒父进程等待者，遵守 `SA_NOCLDSTOP`，并通过 `WUNTRACED`/`WCONTINUED` 报告一次，除非请求了 `WNOWAIT`。`SIGCONT` 在其无条件恢复副作用之后是默认忽略的投递动作；它不再落到默认终止。

## 远程退出

`REMOTE_EXIT_SAFE_BOUNDARY` 实现以下序列：

1. 存储请求的退出码并发布 `exit_pending`；
2. 对目标当前 Park 序列尝试 `PROC_WAKE_TASK_EXIT`；
3. 让不可中断等待者入睡直到其资源事件；
4. 显式恢复 STOPPED 目标；
5. 让目标上正常的等待条目与定时器清理完成；
6. 在 syscall、trap 或恢复执行边界上从 `proc_check_exit_pending()` 执行 `proc_exit()`。

没有任何后备路径直接将 BLOCKED 任务改为 READY。如果事件或超时已经赢得 Park 令牌，`exit_pending` 仍会在下一个安全边界可观察。

## 回归与门禁覆盖

`proc_stress` 验证：

- 停止的子进程报告一次，在普通信号后保持停止，只在 `SIGCONT` 上恢复，然后投递待决普通信号；
- `WCONTINUED` 报告显式恢复，`SIGKILL` 终止停止的子进程；
- 到 `sigsuspend` 的反复阻塞信号交接不能丢失唤醒；
- 未阻塞信号中断 eventfd 读，而阻塞信号不会。

`make check-signal-exit-boundary` 强制唤醒模式标记、信号锁所有权、移除 `proc_make_ready()` 后备、中断原因处理以及目标测试标记。`make check-proc-step5-local` 增加双架构调试/发布压力矩阵；`make check-proc-step5` 额外运行两个正式 CAgent 入口。
