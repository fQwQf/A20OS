# 进程、调度与阻塞协议

本文描述当前 `kernel/proc/` 实现，而不是早期设计草案。任务状态、CPU
所有权、Park/Wake token、timeout、信号和引用计数必须作为一个整体理解；
只观察 `task_t.state` 无法判断任务是否仍在运行队列、已被某个 CPU 选中，
或正在旧内核栈上完成切换。

源码中的权威契约位于：

- `kernel/include/proc/proc.h`：任务状态、CPU 所有权和引用生命周期；
- `kernel/include/proc/park.h`：Park/Wake token、等待模式和唤醒原因；
- `kernel/include/core/sync.h`：wait queue 的 link/collect/flush 协议；
- `kernel/include/proc/signal.h`：共享信号状态及锁；
- `kernel/proc/sched.c`：per-CPU runqueue、EEVDF 记账/资格/选择、空闲窃取、
  持久抢占请求和 RT 类；
- `kernel/proc/timer_heap.c`：wait-timer 截止时间最小堆与定时扫描；
- `kernel/proc/current.c`：current slot 与切换完成；
- `kernel/proc/lifetime.c`：运行时不变量和统计。

## 1. 三个相互独立的状态维度

### 1.1 进程语义状态

`task_t.state` 表示用户可见的生命周期：

| 状态 | 含义 |
| --- | --- |
| `PROC_UNUSED` | 对象尚未发布或已经完成销毁 |
| `PROC_READY` | 可以运行；可能在 runqueue，也可能正处于 dispatch |
| `PROC_RUNNING` | 已由一个 CPU 发布为 current |
| `PROC_BLOCKED` | 已提交 Park，等待事件、超时、信号或退出 |
| `PROC_STOPPED` | POSIX job-control 停止态，不属于 Park |
| `PROC_ZOMBIE` | 已退出，等待父进程回收 |

状态只能由 `kernel/proc/task.c`、`park.c`、`sched.c`、`signal.c` 和退出/回收
路径中的受控入口改变。对象等待者不能直接写 `PROC_BLOCKED` 或
`PROC_READY`。

### 1.2 调度所有权

`on_rq`、`dispatching`、`on_cpu` 三者互斥，形成以下所有权交接：

```text
on_rq -> dispatching -> on_cpu -> unowned
```

- `on_rq`：runqueue 持有任务引用，`cpu_id` 指明所属队列；
- `dispatching`：本地 picker 已从队列取出任务，但尚未发布为 current，
  `owner_cpu` 指明选择它的 CPU；
- `on_cpu`：任务仍在某个 CPU 上执行，或切换后的旧栈清理尚未完成；
- `unowned`：不在队列、未被选择、也不占有 CPU。

出队不能直接释放 runqueue 引用。本地 picker 把该引用转交给 dispatch；
`context_switch()` 再把 dispatch 所有权转为 CPU 所有权。旧任务的 `on_cpu`
必须跨越底层 `__switch` 保持有效，直到新任务在自己的内核栈上调用
`proc_switch_complete()`。

### 1.3 Park 状态

等待状态独立于上述两个维度：

```text
IDLE -> PREPARING -> PARKED -> WOKEN -> IDLE
                  \----------^
```

事件可以在 `PREPARING` 阶段到达，因此“先唤醒、后提交”是合法竞态。
`wait_seq` 每次 prepare 都递增；任何延迟事件必须同时匹配 task 和
`wait_seq`，否则视为旧 token，不能影响下一次等待。

## 2. 锁域与锁顺序

不存在一把覆盖所有调度操作的“全局调度锁”。当前锁域是：

| 数据 | 锁或所有权 |
| --- | --- |
| task list、parent/wait、reap、Park 状态、非本地-pick 状态转换 | `proc_lock` |
| PID hash | 独立 PID 锁；查询返回带引用的 task |
| 每个 CPU 的队列链、bitmap、`on_rq` | 对应 `runq.lock` |
| signal action、进程/线程 pending、mask、sigwait 交接 | `signal_state.lock` |
| MM/VMA/PTE | `mm->lock` |
| files/fd | `files_struct.lock` |

主要顺序是：

```text
cg_node.lock -> proc_lock -> runq.lock -> pfa.lock
proc_lock -> signal_state.lock
proc_lock -> files_struct.lock -> VFS locks
proc_lock -> mm->lock
proc_lock -> a20_handle_table.lock
```

本地 picker 是唯一重要例外：它只获取当前 CPU 的 `runq.lock`，完成
`on_rq -> dispatching` 和引用转交后先释放队列锁，调用者随后才获取
`proc_lock` 发布切换。任何路径都不得在持有 runqueue 锁时反向获取
`proc_lock`。

## 3. Per-CPU runqueue 与迁移

> **与算法文档的关系**：本节只讲 runqueue 的**结构、所有权与锁**。普通任务
> "选谁"（加权公平、资格门控、虚拟截止时间、时间片旋钮）与 SMP 空闲窃取的
> **策略**细节见 [EEVDF 调度器设计](eevdf-scheduler.md)。

每个 CPU 有独立 runqueue。队列包含 8 个调度级别：级 0 用于实时任务
（`SCHED_FIFO`/`SCHED_RR`，优先级 1..99），级 1 用于普通任务的 **EEVDF
（最早资格虚拟截止时间优先）** 列表，其余级别保留未用。EEVDF 的选择策略
（加权 vruntime、系统虚拟时间资格门控、虚拟截止时间、空闲窃取、时间片
旋钮）见 [EEVDF 调度器设计](eevdf-scheduler.md)。有效 affinity 是 task
mask、online CPU mask 和 cgroup cpuset 的交集。

本地选择分两段：

1. `proc_runq_pick_local()` 只持有本 CPU 的 runqueue 锁，选择任务并原子地
   转为 `dispatching`；本地队列为空时，它还会在持有本地锁的情况下尝试从
   其他 CPU 的非阻塞窃取（只窃 EEVDF 任务、远端 `nr_running >= 2`、且本地
   CPU 在被窃任务的允许掩码内）；
2. `sched()` 释放 runqueue 锁后获取 `proc_lock`，复核 current/next，
   必要时 unpick，随后发布 context switch。

迁移一个已排队任务时，同时获取源、目标 runqueue 锁，锁顺序固定为 CPU
编号升序。任务在受锁保护的短暂区间内先从源队列摘除、更新 `cpu_id`，
再进入目标队列；runqueue 持有的引用贯穿整个迁移，不发生释放后重取。

## 4. 持久抢占请求与 IPI

每个 CPU 有 cacheline 分离的 `need_resched`。远程 wake 或更高优先级任务
入队时：

1. 先完整发布 task/runqueue 状态；
2. 以 release 语义设置目标 CPU 的 `need_resched`；
3. 仅在请求从未决变为未决时发送 reschedule IPI。

IPI 是通知，不是调度决定。handler 只确认通知和建立 acquire 顺序，不在
任意中断上下文直接切换。请求由以下安全点消费：

- syscall/trap 返回；
- timer 返回；
- idle loop 或显式 `sched()`。

这样即使 IPI 合并、延迟或先于安全点到达，抢占请求也不会丢失。

## 5. Tokenized Park/Wake

所有可能丢失唤醒的阻塞路径都遵循同一顺序：

1. `proc_park_prepare(mode, deadline)` 创建 token；有 deadline 时同时在
   timeout heap 注册 `(task, wait_seq, deadline)`；
2. 获取对象锁并重新检查持久条件；
3. 条件仍不满足时，用 `wait_queue_link()` 保存带引用的 task 与
   `wait_seq`；
4. 释放对象锁；
5. `proc_park_commit()` 把 `PREPARING` 提交为 `PARKED`，或消费已经到达的
   `WOKEN`；
6. 醒来后先从所有对象队列 unlink，再在对象锁下重查条件；
7. `proc_park_finish()` 取消剩余 timeout 并让 token 回到 `IDLE`。

若第二次检查发现条件已满足，调用者必须 cancel + finish，不能提交睡眠。

唤醒端在对象锁下只做 collect：

1. 从 wait queue 摘除 entry；
2. 把 entry 的 task 引用和 `wait_seq` 转移到有界 `proc_wake_q`；
3. 释放对象锁；
4. 调用 `proc_wake_q_flush()`，由 `proc_try_wake()` 在 `proc_lock` 下验证
   token 并入队。

这条边界禁止设备锁、VFS 锁或 wait-queue 锁嵌套进入 scheduler。超过单批
容量时，wake-all 分批 drain，不能静默留下 `READY` 但未入队的任务。

## 6. 等待模式和唤醒原因

等待模式决定哪些异步事件可以打断：

| 模式 | 普通信号 | 致命信号/任务退出 | 对象事件/超时 |
| --- | --- | --- | --- |
| `INTERRUPTIBLE` | 接受 | 接受 | 接受 |
| `KILLABLE` | 忽略 | 接受 | 接受 |
| `UNINTERRUPTIBLE` | 忽略 | 忽略 | 接受 |

唤醒原因显式区分 event、timeout、timeout capacity、普通信号、致命信号、
task exit、对象关闭和 cancel。syscall 根据原因映射 `EINTR`、`EAGAIN`、
timeout 或正常完成，不能把所有 wake 都当作事件成功。

## 7. Timeout heap 所有权

Park deadline 使用全局最小堆。每个活动项持有一个 task 引用，并保存
`wait_seq`；task 的 `wait_timer_index` 提供 O(log n) 取消。

- register 成功后，引用归 heap；
- cancel 或 expiry 只有一方能在 `proc_lock` 下摘除条目并释放引用；
- expiry 先从 heap 摘除，再通过 `proc_try_wake_locked(task, wait_seq,
  PROC_WAKE_TIMEOUT)` 验证 token；
- heap 满、重复注册或引用获取失败必须回滚 prepare 并向调用者传播错误；
- alarm/itimer 使用独立字段，不占用 Park timeout 项。

`CONFIG_WAIT_TIMER_CAPACITY` 是实际容量边界，不允许为了测试静默扩容。

## 8. 信号、STOPPED 与远程退出

共享 `signal_state` 保护 action、process pending、siginfo；同一对象下各 task
的 blocked mask、thread pending、`sigsuspend` 和 `sigtimedwait` 交接也受
这把锁保护。需要同时持锁时顺序为：

```text
proc_lock -> signal_state.lock
```

`PROC_STOPPED` 不使用 Park token，也不通过普通 `proc_make_ready()` 恢复。
只有 `SIGCONT`、致命信号或任务退出通过显式 stopped-resume 路径改变状态；
`WUNTRACED`/`WCONTINUED` 事件用一次性标志报告给父进程。

远程退出只发布持久 `exit_pending`，并按等待模式尝试
`PROC_WAKE_TASK_EXIT`。它不能把任意 `BLOCKED` 任务直接改成 READY；
不可中断等待仍需等对象操作结束，再在安全点观察退出请求。

## 9. Task 引用和异步所有者

PID lookup 必须使用 `proc_find_get()`，并以 `proc_put()` 配对。以下异步结构
在保存 task 指针期间各持有一个引用：

- task list / PID table；
- runqueue、dispatch/current；
- wait queue / wake queue；
- timeout heap；
- vfork completion 与其他明确记录的生命周期所有者。

回收顺序要求先从所有可发现结构中摘除，再释放相应引用。最后一个引用只能在
任务已经 unlisted、unhashed、off-rq、非 dispatch/current、无 wait/wake/
timeout owner 时销毁对象。

## 10. exec 参数的用户指针边界

`proc_exec()` 位于 Linux syscall ABI 边界，`argv`/`envp` 始终保留用户指针
来源。不能用“地址是否落在内核物理映射范围”推断来源；LoongArch 的
identity mapping 会与动态 glibc 程序的低用户虚拟地址重叠。参数数组和字符串
分别通过 `copy_from_user()`、`user_strncpy()` 复制一次，再构造新地址空间。

`proc_stress` 在固定低地址 `0x02000000` 构造 argv，防止重新引入这种误判。

## 11. 可观察性与验证

`/proc/a20/task_lifetime` 输出：

- task/ref、PID、runqueue、dispatch/current、wait/wake、timeout 和 zombie
  数量；
- runqueue migration、local/empty pick、锁获取/争用和并行 pick 峰值；
- reschedule request、priority request、IPI send/ack/consume/pending；
- stale timeout、重复注册、状态与 scheduler violation。

静态与运行时入口：

```bash
make check-doc-test-gates
make check-proc-step8-local
make check-proc-step8
```

`check-proc-step8-local` 运行双架构 debug/release、1 核/8 核累计压力矩阵；
`check-proc-step8` 进一步运行双架构正式 CAgent。单项契约和故障定位见
[testing/testing-gates.md](testing/testing-gates.md)，各阶段证明见
`docs/testing/*-audit.md`。

## 12. 与 EEVDF 调度器设计文档的分工

- 本文（`process-scheduler.md`）描述**协议**：`task_t` 的三个状态维度、
  `on_rq -> dispatching -> on_cpu -> unowned` 所有权链、锁顺序、Park/Wake
  token、timeout heap 所有权、信号与退出、引用生命周期。它回答"任务如何
  安全地进入、离开和交接 CPU"。
- [EEVDF 调度器设计](eevdf-scheduler.md) 描述**策略**：普通任务如何被
  选择（加权 vruntime、系统虚拟时间资格门控、虚拟截止时间）、时间片旋钮、
  SMP 空闲窃取。它回答"CPU 空闲时该跑谁"。

两者在 §3 处衔接：本节说明 runqueue 的数据结构与锁；算法文档说明其中的
EEVDF 列表如何排序和挑选。修改任何一边都应保持另一边的契约不变。
