# Park/Wake 协议按任务分锁：设计推导与实现说明

> 分支：`fqwqf/performance-overhaul`。目标：把 tokenized Park/Wake 状态机从单一全局
> `proc_lock` 拆到 per-task `park_lock` + 独立 timer-heap 锁，在**不破坏无丢失唤醒不变量**
> 的前提下消除 park/wake 路径的全局锁竞争。
> 状态：**已实现并验证**（riscv64 单核 smoke 全组 + `-smp 8 thread=multi` 下
> futex/proc/sched/vfs/mm 压力连续多轮 PASS；loongarch64 编译通过；网络套件与基线逐字节一致）。
> **`kernel/proc/park.c` 已完全不再取 `proc_lock`**：prepare/commit/cancel/interrupt/wake/
> `proc_wake_q_flush`/donate 全部走 `task->park_lock`，`sched()` 返回后的 wake_reason 读取用
> 无锁原子读（带归因实验证明：park 状态迁出 proc_lock 后，该读的无锁化是净收益，不再是旧代码
> 里充当背压的负优化）。SMP8 mm_stress 测量：`proc` 竞争从基线 16-30K 降到 5.6-6.7K
> （futex+proc+mm 合跑约 17-21K），较最初 51K 改善约 4 倍。

## 1. 现状：为什么所有 park/wake 都挤在 `proc_lock` 上

测量（`-smp 8 thread=multi`，mm_stress）显示 `proc_lock` 是唯一剩余热点；callsite 归因表明
竞争集中在互斥量/futex 的 park/wake 周期。当前每次 park 周期串行取 `proc_lock`：

- `proc_park_prepare`：取 `proc_lock`，置 `PREPARING`、`wait_seq++`、按需注册 timer、检查 exit/signal 挂起。
- `proc_park_commit`：取 `proc_lock`，`PREPARING→PARKED`（除非已被唤醒为 `WOKEN`）、置 `BLOCKED`，释放锁后 `sched()`。
- `proc_park_finish`：复位 park 字段（本分支已把常见情形无锁化）。
- 唤醒侧：`proc_try_wake` / `proc_wake_q_flush` 取一次 `proc_lock`，做 `PREPARING→WOKEN` 或
  `PARKED→READY` + 入队 + 抢占判断。
- timer heap 的注册/取消/到期扫描都在 `proc_lock` 下。

`proc_lock` 同时保护：任务表、父子关系、signal 状态、runqueue 发布、timer heap、以及
fork/exit/wait 的任务表扫描。park/wake 只是它众多职责之一，却被最频繁地触发。

## 2. 必须保持的正确性不变量（无丢失唤醒）

1. **PREPARING 是"即将睡眠"的原子标记**：prepare 与 commit 之间，唤醒方要么把任务置 `WOKEN`
   （于是 commit 不得睡眠），要么在 `PARKED` 之后把它入队（于是睡眠会被正确调度回）。
2. **wait_seq 隔离陈旧唤醒**：每次 park 递增 `wait_seq`；唤醒携带发起时读到的 seq，与任务当前
   seq 不符即为陈旧唤醒，必须 no-op。
3. **timer 取消幂等**：任何把任务从 PREPARING/PARKED 转为 WOKEN 的路径都要取消其 timer；
   超时到期路径同样要摘除堆项并复位 `wait_timer_index = -1`。
4. **on_cpu 任务不重复入队**：目标仍在 `on_cpu`/`dispatching` 时，唤醒只发布 READY，不能把它
   同时放进 runqueue（`proc_runq_enqueue_locked` 在 runq 锁内检查 `on_rq/dispatching/on_cpu`）。
5. **远程抢占通知**：唤醒后若目标 CPU 非本 CPU 且应抢占，发 IPI；这必须在释放锁之后做。

## 3. 设计：三把锁的职责划分

| 锁 | 范围 | 保护对象 |
|---|---|---|
| `task->park_lock`（新增，per-task） | park 状态机 | `wait_seq`、`park_state`、`wake_reason`、`wait_deadline`、`wake_time`、`wait_mode`，以及 `PREPARING/PARKED/WOKEN` 迁移和 `→READY` 发布 |
| `g_wait_timer_lock`（新增，全局） | timer heap | heap 数组、`wait_timer_index`、`wait_timer_count`；注册/取消/到期摘除 |
| per-CPU runqueue 锁（已有） | runqueue | `on_rq`、队列成员、`nr_running`（`proc_runq_enqueue_locked` 已只持 runq 锁） |
| `proc_lock`（保留） | 非 park/wake | 任务表、父子、signal、**切换发布**、fork/exit/wait 扫描 |

关键观察（源码事实）：`proc_runq_enqueue_locked` 已经只取 runqueue 锁（`RUNQ_LOCK_IRQ`），
不需要 `proc_lock`；`proc_sched_should_preempt_locked` 只读不锁。因此唤醒路径真正依赖
`proc_lock` 的只有"目标任务的 park 状态迁移 + timer 取消"。

## 4. 锁序与无环证明

固定顺序：**`park_lock → timer_lock`，`park_lock → runq_lock`**；`proc_lock` 只与这两者以
`proc_lock → runq_lock`、`proc_lock → task 字段` 的方式组合（切换发布、task-list）。

- park 侧：`task->park_lock` 内注册/取消 timer → `park_lock → timer_lock`。
- 唤醒侧：`target->park_lock` 内取消 timer → `park_lock → timer_lock`；入队 → `park_lock → runq_lock`。
- **timer 到期扫描**：取 `timer_lock` 摘出到期项后**先释放** `timer_lock`，再取 `target->park_lock`
  重新校验 seq 并唤醒——与 wait-queue "collect 后释放再 wake" 同构，绝不同时持有
  `timer_lock → park_lock`，因此与 `park_lock → timer_lock` 不构成环。
- 调度器：`proc_runq_pick_local` 只持 runq 锁；`sched()` 在释放 runq 锁后才取 `proc_lock`
  （切换发布），无 `runq_lock → park_lock` 或 `runq_lock → proc_lock` 同时持有。
- 结论：按边 `park_lock→timer_lock`、`park_lock→runq_lock`、`proc_lock→runq_lock`、
  `proc_lock→(task 字段)`，无向图中无环；`timer_lock` 与 `runq_lock` 之间无直接依赖。

## 5. 各操作重写

### 5.1 `proc_park_prepare`
```
task->park_lock:
    wait_seq++（0 则再 ++）
    wait_deadline/wait_mode/wake_reason 初始化
    park_state = PREPARING
    if deadline: park_lock -> timer_lock { register }（失败则回滚到 IDLE 并返回错误）
    检查 exit_pending / 致命信号（模式相关）→ 若需立即唤醒，走 5.5 的 PREPARING 分支
```

### 5.2 `proc_park_commit`
```
task->park_lock:
    if park_state == WOKEN: 返回 wake_reason（不睡眠）
    if park_state != PREPARING: 返回 CANCEL
    park_state = PARKED; state = BLOCKED
释放 park_lock
sched()   # 内部：runq 锁选任务、proc_lock 发布切换（已有）
返回 wake_reason（sched 返回后读；见 §5.6 备注）
```

### 5.3 `proc_park_finish`
维持现状（本分支已无锁化常见路径）；仅当 `wait_timer_index >= 0` 才 `park_lock → timer_lock` 取消。

### 5.4 唤醒（`proc_try_wake_locked_common` 的 park 状态段）
```
target->park_lock:
    if wait_seq != seq 或 state 为 UNUSED/ZOMBIE：no-op
    mode/ reason 过滤（SIGNAL vs UNINTERRUPTIBLE 等）
    case PREPARING:
        park_lock -> timer_lock { cancel }
        park_state = WOKEN; wake_reason = reason; wait_deadline = 0
    case PARKED:
        park_lock -> timer_lock { cancel }
        park_state = WOKEN; wake_reason = reason; wait_deadline = 0
        state = READY; sched_level--
        park_lock -> runq_lock { proc_runq_enqueue_locked }   # 本身已 runq 锁
        if on_rq: park_lock 内做 preempt 判断（只读），记录 remote_cpus/priority
释放 park_lock
（调用方在锁外发 IPI——现状已如此）
```

### 5.5 `proc_wake_q_flush`
从"取一次 proc_lock 唤醒整批"改为"对每个 (task, seq) 独立取 `task->park_lock`"，锁外统一发 IPI。
批量语义不变（wait-queue 已在各自 wq 锁下完成 collect）。

### 5.6 `sched_scan_timers`
```
timer_lock:
    摘出所有 deadline <= now 的堆项（复位 wait_timer_index），收集 (task, seq)
释放 timer_lock
for each (task, seq):
    task->park_lock: 校验 wait_seq==seq 且 park_state 在 PREPARING/PARKED → 走 5.4 唤醒
    （已被其他唤醒先处理的任务在此因 park_state 非 PREPARING/PARKED 而 no-op）
```

### 5.7 取消（`proc_park_cancel`）、`proc_interrupt_wait`
同样改为 `task->park_lock`（内部 `park_lock → timer_lock`）。

### 5.8 仍留在 `proc_lock` 的
`proc_make_ready`（fork/新任务/信号路径，非 park 语义，且 fork 调用方本就持 `proc_lock`）、
切换发布、任务表/fork/exit/wait、signal state。这些都是低频或本来就各自持锁的路径。

## 6. 需要关闭的竞态（逐条对应）

1. **唤醒恰在 commit 读状态前发生**：commit 在 `park_lock` 内读到 WOKEN → 不睡眠。✓（同一把锁）
2. **唤醒恰在 commit 释放后、sched() 前发生**：唤醒把任务置 READY 并入队（目标仍在 on_cpu，
   `proc_runq_enqueue_locked` 的 on_cpu 检查使其不真正入队，只发布 READY）；commit 随后 `sched()`，
   `sched()` 的 current==next 分支或切换完成发布一次。✓（不变量 4 沿用现有语义）
3. **signal 唤醒与超时唤醒并发**：二者都先做 `park_lock` 状态迁移，先到者置 WOKEN，后者读到的
   park_state 非 PREPARING/PARKED → no-op；timer 由先到者取消，后到者的取消幂等。✓
4. **超时摘除后、唤醒前的窗口**：`sched_scan_timers` 释放 `timer_lock` 后才取 `park_lock`；
   若期间 signal 唤醒已置 WOKEN，则超时唤醒 no-op（不变量 2/3）。✓
5. **陈旧唤醒（旧 seq）**：seq 校验在 `park_lock` 内完成。✓
6. **`proc_make_ready` 与 park 状态机**：`proc_make_ready` 不走 park 状态机（针对 IDLE 状态任务，
   如 fork 新任务），仍用 `proc_lock`；与 park 状态机的并发通过"任务要么在 park 状态机里、
   要么不在"的生命周期划分隔离（prepare 前必须 IDLE，finish 复位为 IDLE）。
7. **task 生命周期（释放）**：wake_q 在 collect 时对 task 取引用（`proc_get`），唤醒持引用期间
   task 不会释放；`park_lock` 不参与释放。✓

## 7. 验证结果与风险

- **实现要点回顾**：`task->park_lock` 负责 PREPARING/PARKED/WOKEN 迁移；timer heap 独立
  `g_wait_timer_lock`（`park_lock → timer_lock`）；扫描"摘出→释放→按 park_lock 唤醒"；唤醒入队走
  `park_lock → runq_lock`（`proc_runq_enqueue_locked` 本就只持 runq 锁）；`proc_make_ready`、
  exit 唤醒 wait4 按 `proc_lock → park_lock` 顺序取锁；`proc_wake_q_flush` 改为逐目标 park_lock。
  `proc_park_commit` 在 `sched()` 返回后的 `wake_reason` 读取**刻意保留 proc_lock**——实测移除它会
  因失去背压而竞争反升。
- **正确性**：riscv64 单核 smoke 全组 + `-smp 8 thread=multi` 下 `futex_stress`/`proc_stress`/
  `sched_stress`/`mm_stress`/`vfs_stress` 连续多轮 PASS（无丢唤醒导致的挂起）；网络套件与基线
  逐字节一致（DNS/TIMEOUT 两项失败为既有环境/语义问题）；`check-concurrency-foundation` PASS。
- **已知未复现的罕见 flake（非本分支引入的判断）**：约 9 轮组合压力中出现 1 次
  `vfs_rename_flags` 内 `vnode_put` 的 128B slab 双释放（`cache_idx=2`）。该路径在 vfs/ext4
  vnode 生命周期层，本分支未触碰 vnode 引用计数；单独 `vfs_stress` 与其余组合轮均通过，385a
  基线同样未复现。判断为 vfs_rename 的既有罕见 vnode 引用竞态，可能被调度/锁时序扰动暴露；
  因样本太小无法严格归因，记录在案并建议后续用 fault-injection/长时 vfs 压力单独追查。

- **排查续报（合并后）**：修复 drvmod 的 `kallsyms_print` 导出（见下）使 dev 构建恢复可 boot 后，
  用 8 vCPU 组合压力再次复现该 flake（3 轮中 1 轮：`vfs_rename_flags` 内 `vnode_put` 双释放，
  ra 指向 `vfs_dcache_invalidate` 附近）。静态分析未找到平衡破坏点：`old_victim`/`new_victim`
  （VFS lookup ref + dcache ref）与 `ext4_inode_remove` 的 `deferred_put`（ext4 vnode cache ref）
  各成对 put；dcache per-bucket 重构未改变"锁内摘链、锁外 vnode_put"的既有模式。最可能仍是
  vfs/ext4 vnode 缓存与并发 rename 之间的既有罕见竞态，建议后续用 vnode refcount 断言/
  fault-injection 专案定位。

- **drvmod 模块 ABI 修复（dev 构建可 boot）**：main 的 `b29d6691` 在 `core/lock.h` 内联
  `spin_lock_at` 的 stall 报告里新增 `kallsyms_print()` 调用，任何包含 lock.h 的 `.a20drv`
  模块都产生该外部引用；drvmod 白名单只导出 `kallsyms_lookup`，导致 `virtio-blk.a20drv`/
  `dw-sdio.a20drv` 加载 -22、generic 部署 dev 构建找不到 FAT32 根而 boot panic。已在
  `drvmod/framework.c` 白名单补导出 `kallsyms_print`，dev 构建恢复 boot，全部压力门禁通过。
- **性能**：`/proc/a20/lock_contention` 的 `proc` 竞争从 16-30K 降到 5.6-6.7K（mm 单跑）。
- **架构**：通用代码，loongarch64 编译通过。
- **风险**：这是全项目最核心的协议；任何一步不满足 §2 不变量都会造成隐藏丢唤醒。已用 A/B 确认
  网络/时间测试无回归，且 `proc_park_finish` 与 `proc_park_commit` 的读路径保留了此前测量得到的
  正确取舍。
- **剩余 `proc_lock` 来源（测量归因）**：切换发布（`sched()` 内联的 `spin_lock(&proc_lock)`，
  每次上下文切换 1 次，结构性且移动有损 "neither selected nor owned" 不变量）与 fork/exit 的任务表
  扫描（O(tasks)，但按进程生命周期触发、频率低）。per-parent 子进程链表与切换发布无锁化均评估为
  高复杂度/高风险且当前测量价值低，留待正式 BuildStorm 证据再定。
