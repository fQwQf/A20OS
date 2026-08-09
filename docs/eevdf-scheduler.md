# EEVDF 调度器设计

本文描述 `kernel/proc/sched.c` 当前实现的 EEVDF（Earliest Eligible VirtualDeadline First，最早资格虚拟截止时间优先）调度核心，及其在 SMP 负载均衡、资格门控、时间片旋钮方面的扩展。与[进程、调度与阻塞协议](process-scheduler.md) 互补：后者讲任务状态、CPU所有权与锁协议，本文讲公平/延迟的**选择策略**。

## 1. 背景与目标

A20OS 同时面向三类场景，对调度器有相互冲突的要求：

| 场景 | 核心诉求 |
| --- | --- |
| 高性能计算（BuildStorm） | 吞吐、8 核满载、无饿死、负载均衡 |
| 桌面 | 低延迟、交互任务快速抢占、UI 流畅 |
| 低资源 MCU | 确定性强、开销小、可编译到极小内核 |

旧实现是 8 级 MLFQ + aging 计时器：公平性靠"等得久就升到级 0"来近似，nice/weight 并不真正参与排序，SMP 只有唤醒时放置、没有空闲窃取，8 核下容易出现负载失衡。

调研了 Linux CFS/EEVDF、MuQSS（桌面 EEVDF 变体）、WALT（移动端负载跟踪）以及 Liu & Layland 的 RT 理论后，选择 **EEVDF 作为统一核心**：它用一套模型同时给出按权公平（HPC 吞吐）与短时间片低延迟（桌面交互），且不需要CFS 那套脆弱的交互启发式。Linux 6.6+ 主线和独立发展的 MuQSS 都收敛到EEVDF，说明该算法在通用负载上经过充分验证。

## 2. 核心模型

### 2.1 加权虚拟运行时间

普通任务 `t` 每运行 `dt` 个 tick，按权重累加虚拟运行时间：

```text
vruntime += dt * NICE0_LOAD / weight
```

`weight` 来自 nice（`sched_prio_to_weight[]`，低 nice 权重高）。权重高的任务 vruntime 增长慢，因此"理应"获得更多 CPU。这修复了旧调度器里nice/weight 纯装饰的问题。

### 2.2 系统虚拟时间与资格

每个 CPU 的 runqueue 维护系统虚拟时间 `vtime`，按可运行任务的总权重推进：

```text
vtime += dt * NICE0_LOAD / total_weight
```

只有当队列里存在可运行的 EEVDF 任务时 `vtime` 才推进。任务 `t` 当且仅当满足以下条件时**有资格**（eligible）被选中：

```text
vruntime <= vtime
```

含义：`vruntime` 落后于系统虚拟时间的任务还没用满自己的公平份额（lag为正，欠账），可以运行；跑过头的任务（vruntime 领先）被排除在候选之外，从而不可能仅靠短时间片反复抢占。

### 2.3 虚拟截止时间与选择规则

每个任务有虚拟截止时间：

```text
vslice = base_slice * NICE0_LOAD / weight
deadline = vruntime + vslice
```

**选择规则：在所有有资格的任务里，选 `deadline` 最早的一个。** 短时间片的任务 `deadline` 更早，因此低延迟任务自然先跑；但资格门控保证它不能超出自己的加权份额。这正是 EEVDF 同时提供公平与延迟的关键。

### 2.4 调度类层次

| 类 | 队列 | 语义 |
| --- | --- | --- |
| RT（`SCHED_FIFO`/`SCHED_RR`） | 级 0 | 固定优先级 1..99，不记账 vruntime，永远先于 EEVDF 类 |
| EEVDF（`SCHED_NORMAL`/`BATCH`/`IDLE`） | 级 1 | 按"最早资格虚拟截止时间"排序的链表 |

级 0 由 bitmap 的最低位置位表示，因此 RT 恒优先于 EEVDF。

## 3. 记账与切换

任务的实际 CPU 时间在三个点计入 `vruntime` 并推进 `vtime`：

- `proc_sched_tick()`：周期 tick 记账；
- `proc_yield()`：主动让出前记账；
- `context_switch()`：切出旧任务时记账。

所有记账共用 `task->eevdf_last_account` 时间戳，避免重复计数。任务跨睡眠**保留** `vruntime`（sleeper bonus）：刚唤醒的任务 vruntime 落后、deadline早，天然获得交互优先级；但 bonus 在入队时被钳制到 `EEVDF_MAX_LAG`（默认 10ms），防止长睡后 CPU-burn 的任务长时间独占。

## 4. 数据结构和锁

每个 CPU 一个 `proc_runq_t`，EEVDF 类是一个**按 deadline 升序**的双向链表：

```c
typedef struct proc_runq {
    spinlock_t lock;
    task_t *head[SCHED_LEVELS];   /* 级 0 = RT，级 1 = EEVDF */
    task_t *tail[SCHED_LEVELS];
    uint32_t bitmap;
    unsigned nr_running;
    uint64_t eevdf_vtime;         /* 系统虚拟时间 */
    uint64_t eevdf_weight;        /* 可运行 EEVDF 任务权重和 */
    ...
} proc_runq_t;
```

插入 O(n)（按 deadline），取头 O(1)。选中后 `on_rq -> dispatching -> on_cpu`所有权交接与旧实现一致（见 process-scheduler.md §1.2），本地 picker 仍只持本 CPU 的 runqueue 锁，迁移仍按 CPU 编号升序加锁。

## 5. SMP 负载均衡：空闲窃取

唤醒路径只在唤醒时把任务放到最闲 CPU。新增**空闲窃取**：本地 CPU 的runqueue 为空时，用非阻塞 `spin_trylock_irqsave` 尝试其他 CPU 的 runqueue，把任务拉到本地执行，避免"某核满载、其他核空闲"的持续失衡。

窃取规则刻意保守：

- 只窃取 EEVDF（普通）任务；RT 任务由唤醒路径刻意放置，不挪走；
- 只有远端 `nr_running >= 2` 才窃取（远端有富余），不把单一刻意放置的任务 拔走；
- 校验被窃任务的 `cpus_allowed` 包含本地 CPU（尊重 affinity）；
- 窃取从远端移除后把 runqueue 引用直接转交本地 dispatch，无 put/get 间隙。

## 6. 时间片旋钮（阶段 C）

基础时间片可通过 `/proc/a20/sched_base_slice`（单位 ms，范围 1..1000）运行时调整，默认 10ms：

```sh
cat /proc/a20/sched_base_slice      # 10
echo 2 > /proc/a20/sched_base_slice # 低延迟（桌面）
echo 50 > /proc/a20/sched_base_slice # 高吞吐/缓存友好（HPC 批处理）
```

短片 → 低延迟（EEVDF 让其 deadline 更早）；长片 → 减少抢占、缓存友好、吞吐更高。这与 Linux 的 `sched_base_slice_ns` 思路一致。

## 7. 与旧 MLFQ 的差异

| 维度 | 旧 8 级 MLFQ | EEVDF |
| --- | --- | --- |
| 公平性 | aging 计时器近似 | 加权 vruntime + vtime 资格门控 |
| nice/weight | 装饰 | 真实控制 CPU 份额 |
| 延迟 | 升级到级 0 | 短时间片 deadline 更早 |
| 防饿死 | aging 计时器 | 资格门控 + deadline 排序 |
| SMP 负载均衡 | 仅唤醒放置 | 唤醒放置 + 空闲窃取 |
| RT | 级 0 与老化任务混排 | 级 0 专属，语义清晰 |
| 时间片 | 固定 10ms | 可调旋钮 |

## 8. 验证

- **nice 权重真实生效**：同窗内 `nice -20` 获得约 100000 倍的 CPU（vs `nice 19`），符合加权比例共享。
- **负载均衡**：8 个忙任务分布在全部 8 核（busy_cores=8），此前堆在一核。
- **8 核压力**：`sched_stress` 的 `smp-runqueue` 与 `lock-split` 全 PASS， 空闲窃取以 `runqueue_migrations` 计数正常触发。
- **全量门禁**：双架构（riscv64/loongarch64）构建与启动、5 架构构建、 全部 21 个 `check-doc-test-gates` 通过；procfs/sched/futex/proc/mm/vfs 压力测试全绿。
- **时间片旋钮**：2ms/50ms 下系统稳定。

## 9. 已知限制与后续工作

- `vtime` 资格门控在极端的 nice 差距下（如 nice 19 对 nice -20）会让轻权重 任务获得极少量 CPU；这是加权比例共享的正确行为，但可按需引入最小粒度 保证。
- 窃取只发生在本地队列空时；可扩展为周期性 push/pull 平衡。
- RT 级 0 的优先级线性扫描在 RT 任务极多时才需要优化（当前数量小，开销 可忽略）。
- 时间片是全局旋钮；可扩展为 per-task（sched_attr）粒度以精确表达单任务 延迟需求。
- 窃取/迁移后任务在目标 CPU 重新以自身 vruntime 定义 `vtime`，个别边界 情况下可能"逃逸"资格限制，后续可加迁移时 lag 保留。

## 10. 相关源码

- `kernel/proc/sched.c`：EEVDF 记账、资格、选择、窃取、RT 类；
- `kernel/proc/proc_internal.h`：nice→weight 表、EEVDF 常量；
- `kernel/include/proc/proc.h`：`task_t` 的 vruntime/deadline 字段；
- `kernel/fs/procfs/procfs.c`、`procfs_render.c`：时间片旋钮的可读写文件。
