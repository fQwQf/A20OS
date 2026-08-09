# 进程锁拆分与调度器热路径审计

`PROCESS_LOCK_SPLIT_AUDIT`

本文档在 Park/Wake、任务生命周期、超时、SMP 迁移和持久抢占协议通过其双架构压力矩阵之后，收口 PROC.md 第 8 步。该改动被刻意限定在已测量的调度器热路径；它不会仅仅为了增加锁的数量而削弱生命周期串行化。

## 已有的拆分边界

审计发现 PROC.md 命名的几个域已经拆分：

| 域 | 保护锁 | 第 8 步决定 |
|---|---|---|
| PID 位图与哈希 | `pid_lock` | 保持独立 |
| 信号动作与待决状态 | `signal_state.lock` | 保持独立；两者都需要时使用 `proc_lock -> signal_state.lock` |
| 运行队列链接与成员关系 | 每个 CPU 一个 `runq.lock` | 从本地 pick 中移除全局串行化 |
| 任务列表、父/等待与回收 | `proc_lock` | 在测得父/回收竞争前保持粗粒度 |
| 本地 pick 之外的 Park 令牌与调度器状态 | `proc_lock` | 保持粗粒度，因为唤醒、超时、退出与切换完成共享一个状态转移 |

剩余的架构性调度器瓶颈是本地队列选择：每个 CPU 在扫描和老化自己的运行队列之前都会获取 `proc_lock`。即使独立的每 CPU 队列已经由不同锁保护其链接与成员关系，这也会使它们串行化。

## 本地 pick 拆分

`proc_runq_pick_local()` 现在只执行：

```text
lock(this_cpu.runqueue)
    age this CPU's queues
    select a task
    on_rq -> dispatching
    publish owner_cpu
unlock(this_cpu.runqueue)
```

它从不获取 `proc_lock`。调用方只在运行队列锁被释放后才获取 `proc_lock`，用于将选中的任务与仍拥有更高优先级的当前任务比较，并在上下文切换期间发布 `dispatching -> on_cpu`。

该拆分保持以下线性化点：

- 入队、移除、策略重新排队和迁移仍使用 `proc_lock -> 运行队列锁`；
- 本地 pick 在本地运行队列锁下将运行队列拥有的任务引用转移给派发所有权；
- unpick 使用 `proc_lock -> 运行队列锁` 并在没有 put/get 间隙的情况下将同一引用转移回去；
- 切换发布与切换完成仍处于 `proc_lock` 之下；
- Park、唤醒、超时、退出、父/等待与回收语义不变。

本地 picker 在持有运行队列锁时从不获取 `proc_lock`。因此它无法反转全局顺序。持有 `proc_lock` 的生命周期观察者可能等待 picker 释放运行队列锁；picker 完成其有界本地操作而无需等待该观察者。

## 延迟拆分

父/等待与任务 Park 状态仍处于 `proc_lock` 之下。拆分它们需要对退出/唤醒/回收和超时/唤醒/完成竞态提出新的证明，而当前诊断并未将任一域识别为运行队列选择瓶颈。它们只应在工作负载记录到这些路径的持续竞争并且存在专门的所有权测试之后才拆分。

本步骤中没有任何架构上下文、寄存器布局、优先级类、CPU 选择策略或 IPI 行为发生变化。

## 诊断与验收

`/proc/a20/task_lifetime` 现在报告：

- `runqueue_local_picks`；
- `runqueue_empty_picks`；
- 聚合的每 CPU `runqueue_lock_acquires`；
- 观测到的 `runqueue_lock_contentions`；
- `runqueue_parallel_pick_peak`。

竞争计数器是诊断性的：它记录锁在获取开始时就已被持有，可能保守地低估竞态。并行峰值记录重叠的本地 pick 尝试，并不要求在特定模拟器运行上超过一。

`sched_stress` 强制 256 个显式调度点，并验证本地 pick 与运行队列锁计数器前进、竞争从不超过获取数、并行峰值已初始化且调度器违规保持为零。现有的 SMP 迁移/抢占测试以及累积的生命周期、futex、进程、信号、超时、I/O、VFS 和 socket 测试继续不变地运行。
