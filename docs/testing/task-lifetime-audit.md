# 任务生命周期所有权审计

`TASK_LIFETIME_OWNERSHIP_AUDIT`

本审计是 `PROC.md` 第 3.5 步的验收记录。它覆盖 Park/Wake、CPU 所有权和任务生命周期改动引入的所有权模型。诊断面在 `/proc/a20/task_lifetime` 只读可见；它不改变调度决策。

## 所有权转移表

| 所有者 | 引用获取 | 转移或释放 |
|---|---|---|
| 分配/全局任务列表 | `proc_task_alloc_storage()` 在 `proc_alloc_task_slot()` 发布任务之前创建分配引用 | `proc_destroy_task()` 标记 `destroy_started`，移除调度器/全局可达性，然后释放分配引用 |
| 静态空闲任务 | `proc_task_init_idle_state()` 安装一个永久基础引用 | 永不释放；`/proc/a20/task_lifetime` 检查每个当前静态任务除 CPU 所有权之外仍持有其基础引用 |
| PID 表 | `proc_pid_register()` 在发布前调用 `proc_get()` | `proc_pid_unregister()` 移除哈希/位图条目，然后调用 `proc_put()` |
| 运行队列 | `proc_runq_enqueue_locked()` 在设置 `on_rq` 之前调用 `proc_get()` | `proc_runq_pick_local()` 在本地运行队列锁下将同一引用转移给 `dispatching`；`proc_runq_remove_locked()` 释放它 |
| 派发/当前 CPU | 被选中的任务在 `dispatching` 期间保留运行队列引用；`proc_set_current()` 获取当前槽位引用 | `context_switch()` 释放派发引用；`proc_switch_complete()` 在旧栈不再活动之后释放出去的当前/切换引用 |
| 等待条目 | `wait_queue_link()` 和 `futex_waiter_alloc()` 在发布前调用 `proc_get()` | 解除链接/移除释放它，或回收将其转移给唤醒批次 |
| 唤醒批次 | `proc_wake_q_add()` 接收等待条目引用而不额外 get | `proc_wake_q_flush()` 调用 `proc_try_wake_locked()`，然后恰好调用一次 `proc_put()` |
| 超时堆 | `proc_wait_timer_register_locked()` 在堆插入前调用 `proc_get()` | 取消和过期在 `proc_put()` 之前移除堆项 |
| PID 查找调用方 | `proc_find_get()` 在 `pid_lock` 下递增 | 每次成功的外部查找都与 `proc_put()` 配对；返回引用任务的辅助函数会在其调用方文档中说明所有权 |

## 静态审计

- 内核中没有调用点使用裸 `proc_find()`。
- 进程、信号、procfs、VFS、cgroup、OOM、设备、Linux ABI 和 Native ABI 路径中的 `proc_find_get()` 调用点均检查在成功与错误退出上是否配有匹配的 `proc_put()`。`sys_sched` 查找辅助函数将返回的引用转移给其 syscall 调用方，由后者释放。
- 每个通过 PID、运行队列、当前 CPU、等待队列、futex 等待者、唤醒批次或超时堆跨锁边界存储的任务指针都拥有或接收一个引用。
- `on_rq`、`dispatching` 与 `on_cpu` 互斥。运行队列引用在 `on_rq -> dispatching` 时是转移而非重新获取；当前槽位所有权仅在切换完成后释放。
- 最终资源销毁只能从最后一次 `proc_put()` 到达，且要求同时满足动态任务与 `destroy_started`。
- 重复销毁、活动任务 get 失败、引用下溢，或对活动/静态任务的最终 put，都会在不安全路径被阻止之前递增一个单调诊断错误计数。

`make check-task-lifetime-boundary` 保护静态标记与对裸 PID 查找的禁止。`make check-proc-step35-local` 运行双架构调试/发布 smoke 与竞态矩阵；`make check-proc-step35` 额外运行两个正式 CAgent 评测。

## 运行时收口标准

`lifetime_stress` 运行调度器、futex、进程、I/O 事件和 VFS 压力程序，随后反复执行以下批次：

- `fork/exit/wait4`；
- `signal/exit`；
- 超时/exit；
- futex wake/exit。

它会在运行前后采样诊断面，并要求任务对象、总引用数、列出的任务/引用、PID 条目、等待条目、唤醒条目和超时条目回到相同基线。它还要求 `lifetime_errors: 0`。宿主机侧目标还要求每个压力 PASS 标记、零 QEMU 状态码以及正常的关机标记。
