# 超时所有权审计

`TIMEOUT_OWNERSHIP_AUDIT` 记录调度器第六步关于 Park 截止时间的契约。实现仍是带令牌的截止时间最小堆；本步骤不引入第二个定时器轮。

## 所有权

每个堆条目恰好持有一个任务引用，并记录不可变三元组 `(task, wait_seq, deadline)`。注册时获取该引用。取消或过期二者恰好其一会在 `proc_lock` 下移除该条目并释放引用。堆移除会在条目被移入空位之前清除任务的堆索引。

任务的 Park 截止时间使用 `wait_deadline` 和 `wake_time`。POSIX `alarm`/`ITIMER_REAL` 使用独立的 `alarm_expire` 和 `itimer_real_interval` 字段。闹钟投递不会创建或移除 Park 堆条目。

## 线性化与陈旧事件

- 一个任务最多持有一个堆索引。为同一任务注册第二个条目会被拒绝；注册从不取消现有令牌。
- 事件、信号、退出、取消和超时移除都在 `proc_lock` 下串行化。
- 过期先移除堆条目，再调用 `proc_try_wake_locked(task, wait_seq, PROC_WAKE_TIMEOUT)`。因此迟到的过期无法改写之后的 `wait_seq`。
- 定时器回调既不触碰对象锁，也不触碰栈驻留的等待条目。对象等待队列由被唤醒的等待者解除链接。
- 过期的 Park 截止时间直接通过调度器状态机唤醒。它们不经过有界唤醒数组，因此不存在「无运行队列的 READY」溢出场景。

## 容量失败

生产默认值为 8192 个条目（MCU 上为 64 个）。测试构建可以用 `WAIT_TIMER_HEAP_MAX` 选择更小的真实堆；这会改变编译出的数组，而不是在其上叠加一个合成上限。

当堆已满时，注册返回 `PROC_PARK_PREPARE_TIMEOUT_CAPACITY`。`proc_park_wait()` 将其暴露为 `PROC_WAKE_TIMEOUT_CAPACITY`，Linux 定时 syscall 将其映射为 `EAGAIN`。无时限等待不受影响。内部睡眠在让出后重试，而不是提前返回。

`/proc/a20/task_lifetime` 报告编译容量、当前条目数、注册失败数、重复拒绝数、过期事件数和堆不变量违规数。堆违规计入 `lifetime_errors`。

## 运行时覆盖

第六步矩阵编译一个真实的 64 条目堆，并在 RISC-V64 与 LoongArch64 上以调试单核和发布八核配置运行。`lifetime_stress` 将堆填至 63、64 以及尝试的第 65 个条目。它检查精确占用率，要求第 65 个 `nanosleep` 返回 `EAGAIN`，杀死并回收每一个填充任务，并要求堆与任务引用计数回到基线。

`futex_stress` 还提前唤醒一个定时等待，并立即在同一任务上开始一个更晚的等待。第二次等待必须持续到自己的截止时间，从而证明被取消的旧截止时间无法唤醒新令牌。
