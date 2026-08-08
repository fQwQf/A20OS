# 阻塞点协议审计

`BLOCKING_POINT_PROTOCOL_AUDIT`

这是 `PROC.md` 第 4 步的验收记录。一个阻塞操作只有在它的持久条件、条件锁、Park 令牌、异步任务引用、一次性唤醒赢家以及恢复清理都清晰可辨时才算收口。信号/停止/退出策略由 `docs/testing/signal-exit-audit.md` 收口；超时堆容量仍是第 6 步单独限定的工作。

## 已审计的阻塞族

| 族 | 持久条件与锁 | 等待发布与清理 |
|---|---|---|
| 互斥量与 completion | `mutex.lock` 下的 `locked/owner`；`completion.lock` 下的 `done` | 当前任务在对象锁之前准备，在其下重查，链接 `(task ref, seq)`，解锁后提交，然后解除链接并完成。解锁/完成在刷新唤醒令牌之前摘下等待条目。 |
| wait4 | `proc_lock` 下的子进程状态、父进程关系和 `waiting_for_child` | 子条件在 `proc_lock` 下持久存在；`proc_park_prepare_locked()` 在同一临界区发布令牌。子进程退出改变状态并在该锁下赢得令牌。 |
| vfork | `CLONE_VFORK`、`vfork_waiting` 以及 `proc_lock` 下的子退出/exec 完成 | 完成遵循正常的 completion 协议。父进程现在从发布到完成解除链接期间拥有一个显式子引用，因此自动回收无法释放内嵌的 completion。 |
| pipe 与 PTY | pipe/PTY 锁下的环形占用、端点打开计数和挂起状态 | 读/写在对象锁下重查，链接一个带令牌的等待条目，并在每次返回时解除链接。数据/打开状态改变在相同锁下摘下等待者，并在解锁后刷新。 |
| eventfd 与 timerfd | 对象锁下的计数器/空间或定时器到期 | 对象状态是持久的。等待条目拥有 `(task ref, seq)`，定时器截止时间使用同一个 Park 令牌；唤醒、超时和取消在 `proc_try_wake()` 汇合。 |
| Native channel | 端点锁下的队列与对端关闭状态 | 发送/接收是非阻塞的并返回 `-EAGAIN`；就绪状态持久保存在端点中，并通过 Native 事件队列导出。channel 不存储裸任务指针。 |
| SysV 信号量 | `g_sem_lock` 下的信号量值、移除状态和操作可行性 | 在 `g_sem_lock` 外准备单个令牌，加锁重查后链接到集合等待队列，并在重用或返回前总是取消/提交、解除链接并完成。 |
| 文件锁 | 锁表以及 `g_file_lock_table_lock` 下的 `g_file_lock_generation` | 一个代数关闭条件检查/链接窗口。等待条目携带 `(task ref, seq)`，解锁/表变化在唤醒刷新前摘下。 |
| socket 与网络底半部 | `g_net_lock` 下的 accept 队列、接收数据/EOF/错误、发送空间和连接状态 | Accept/读/写/连接在网络锁外准备，在其下重查并链接，提交后清理。底半部回调更新持久状态并在 `g_net_lock` 下收集任务引用，但只在释放它之后才调用 Park 唤醒。 |
| futex | `mm->lock` 下的用户字翻译，`g_futex_lock` 下的等待者桶 | `FUTEX_WAIT` fault in 并检查一次字，准备令牌，然后取得 `mm->lock -> g_futex_lock`，重新加载用户字，并在同一桶临界区链接 `(task ref, seq)`。匹配的唤醒无法在第二次检查与链接之间越过桶锁。超时、信号、事件和清理都使用同一序列。 |
| virtio-blk | `virtio_blk_instance.lock` 下的请求完成与 in-flight 状态 | 每个请求拥有一个等待队列；IRQ/轮询完成持久化 `done`，在设备锁下摘下等待者，并在解锁后刷新。截止时间与事件共享一个令牌。 |
| UART 与输入 | `rx_lock` 或设备实例锁下的 RX/事件环状态 | IRQ 路径在收集带令牌的等待条目之前持久化输入。等待者在加锁的环重扫之前注册，然后在事件/信号/取消时解除链接并完成。 |

所有正常的等待队列条目、futex 等待者、唤醒批次条目和超时堆条目都同时包含一个拥有的任务引用和 Park 序列。唤醒者首先摘下或转移该引用，释放条件锁，尝试一次性序列转移，并恰好释放一次引用。

## 显式兼容白名单

以下路径有意对静态门禁可见：

1. `proc_task_alloc_storage()` 将一个新分配、未发布的任务初始化为 `PROC_BLOCKED`。它没有活动 Park 令牌，只会在一个白名单内的任务发布调用点变为可运行。
2. `proc_park_commit()` 是唯一写入 `PROC_BLOCKED` 的活动任务路径。
3. `proc_make_ready()` 保留用于新任务发布、当前任务让出和 cgroup 解除节流。其 Park 分支委托给 `proc_try_wake_locked(task, wait_seq, EVENT)`，因此无法绕过活动令牌。
4. 信号、停止和退出路径不再出现在此白名单中。它们使用模式检查的 Park 唤醒原因或显式的 STOPPED 恢复辅助函数，如 `docs/testing/signal-exit-audit.md` 所记录。
5. Linux `poll`、`select` 和 `epoll` 当前使用有界周期 `proc_park_wait()` 截止时间，并在每个时间片后重扫持久就绪状态。它们不存储异步任务指针，也不会丢失正确性，但它们还不是事件驱动的多对象订阅。这个有界兼容包装在第 4 步被接受，并作为性能/延迟债务保留，而不是作为已完成的 VFS poll-hook 设计呈现。

不再存在 `proc_block_until()` 调用或实现。除任务初始化和调度器之外，没有任何模块写入 `on_rq`、`cpu_id`、`rq_next` 或 `rq_prev`。

## 回归与门禁覆盖

`futex_stress` 在另一个 futex 必须到达其截止时间时运行无关 futex 的唤醒风暴。这拒绝了被移除的全局唤醒代数捷径，该捷径可能报告虚假的成功等待。`proc_stress` 在父进程阻塞于子进程内嵌的 completion 上时反复自动回收 `vfork` 子进程。`lifetime_stress` 还会运行调度器、futex、进程、I/O 事件、VFS 和 socket 压力，然后验证任务、PID、等待、唤醒、超时和引用计数回到基线。

`make check-blocking-point-boundary` 强制旧 API 禁止、直接状态与运行队列字段边界、有限的 `proc_make_ready()` 白名单、带令牌的异步等待结构以及目标回归标记。`make check-proc-step4-local` 增加双架构调试/发布运行时矩阵；`make check-proc-step4` 额外运行两个正式 CAgent 入口。
