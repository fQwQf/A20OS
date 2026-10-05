# 锁串行化拆分设计（feat/lock-serialization-split）

状态：设计定稿，实施中。本文档是本轮锁拆分的权威设计；实施细节以各实现笔记
（`docs/measured/impl-notes-*.md`）和最终 diff 为准。测量协议见 §5。

> 修订记录（2026-10-05，design review round 1）：本版逐条落实复核提出的 17 条结论。
> 每条在文末 §8「复核结论落点表」里给出落点，便于反查。被推翻的复核结论写明理由。

## 0. 范围

本轮解决两个已实测确认的锁串行化瓶颈（基线证据见 `docs/measured/lock-baseline.md`）：

1. **`proc_lock` 残留在上下文切换发布路径上**（P0，`server-readiness.md` §八）。
2. **`g_net_lock` 单锁覆盖整个 socket 表**（P0，同上）。

**slab 从本轮范围移出**，理由见 §3：per-CPU 对象数组在合并前已经合入树
（`fc71e9674` "mm, core: per-CPU slab object arrays and a seqlock realtime reader"，
本分支 `git merge-base --is-ancestor fc71e9674 HEAD` 为真），本轮对它无事可做，
保留它只会让 before/after 对比表多一列恒零差异。

**明确不做**（本轮范围外，理由如实记录）：
- `g_lwip_lock` 本体分片：需要先对 lwIP 的 `tcp_active`/`tcp_bound_pcbs`/`udp_pcbs`
  做桶化，触及 vendored 代码（`kernel/external/lwip/`，有 DIVERGENCE.md 治理）。
  本轮在 `impl-notes-net.md` 里记录普查结论，留给下一轮。
- 内核抢占 / RT throttling：与锁拆分正交，另立项。

成功标准（三者同时满足才算完成）：
- §7 门禁清单全绿；
- 新的静态 gate 钉住新不变量（切换路径无 `proc_lock`），旧 gate 演进而非删除
  （逐条对照 §1.5 的「保留 / 演进」表，不允许出现未列出的 gate 变更）；
- `docs/measured/lock-after.md` 与基线同协议对比，结论不超出 TCG 噪声分辨率。

## 1. proc_lock：切换发布路径去全局化

### 1.1 现状事实（已核实）

- 定义：`kernel/proc/proc.c:43` `spinlock_t proc_lock = SPINLOCK_INIT;`。
  全树 265 处**文本引用**（`grep -rn proc_lock kernel/ | wc -l` → 265，其中
  `kernel/proc/sched.c` 59 处）。引用数 ≠ 取锁点数，也不等于改动面：真实的
  获取点是 `spin_lock_irqsave(&proc_lock)`。范围规划按获取点与**状态字段归属**
  来做，不按文本引用计数。逐文件计数见 §4.2。
- **park/wake 协议已经是 per-task 锁**：`kernel/proc/park.c` 全部经 `task->park_lock`
  （`proc_park_prepare` :169、`proc_try_wake` :188、`proc_park_commit` :250 前后），
  即"按等待对象分锁"的 P0 条目里 park/wake 这一半已经落地。TODO 文档里残留的
  "park/wake 协议竞争集中在 proc_lock"描述早于该改造，已过时。
- 真正残留在 `proc_lock` 上的是**切换发布**，而这条路径上牵涉 **5 处**：3 处真正
  取锁的 callsite，加上 2 个每次切换必经、但只消费/释放调用方已持有的锁的函数
  （`context_switch_locked()` / `proc_switch_complete()`）。旧稿只断言
  `sched()` / `context_switch()` 两个函数体：

  | 位置 | 形态 | 保护什么 |
  |---|---|---|
  | `sched.c:1894`（`context_switch()` :1891-1896） | `spin_lock_irqsave(&proc_lock)` → `:1895` 调 `context_switch_locked(next, flags)` | 独立入口的发布 |
  | `sched.c:1925`（`sched()` :1898） | 同上取锁，`:1950` 传给 `context_switch_locked(next, flags)`，在 `:1936`/`:1953` 之间使用 | 抢占判定 + 发布 |
  | `sched.c:1963`（`sched()` 落 idle 前的 late-wake recheck） | 同上取锁，`:1972` 释放 | late-wake recheck |
  | **`sched.c:1792` `context_switch_locked()`** | **不自取**，但依赖调用方已持锁；锁在 `:1835`（`next == proc_current()` 分支）与 `:1866`（正常分支）释放 | `next` 的 `state/on_rq/dispatching/on_cpu/owner_cpu` 发布、cgroup 计费、`mm_context_enter` |
  | **`current.c:148` `proc_switch_complete()`** | **每次切换都取** `proc_lock`（`current.c:145`），`:149` 调 `proc_switch_complete_locked(cpu)` | **outgoing（上一个）任务** 的 `on_cpu/owner_cpu` 清零 + 必要时入 runq |

  `context_switch_locked()` 由 `:1895` 和 `:1950` 调用，`proc_switch_complete()`
  由 `sched.c:1888`（`__switch(next->kstack)` 之后）调用。**只断言
  `sched()` / `context_switch()` 两个函数体，会让 P0 目标在门禁全绿的情况下完全
  没达成**，因此 INV-P4 的范围按上表收紧（见 §1.2）。

- **切换路径会改到两把（某些架构下三把）task 锁**，不只 `next`：
  `sched.c:1856` `proc_set_current(next)` 在 `current.c:132-134` 会回调
  `proc_switch_complete_locked(cpu)`，它对**上一个** outgoing 任务写
  `old->on_cpu = 0; old->owner_cpu = PROC_CPU_NONE;`（`current.c:40-42`），并在
  `old->state == PROC_READY && !old->dispatching && !old->on_rq` 时
  `proc_runq_enqueue_locked(old)`（`current.c:55`）。按 INV-P1 这两处都必须持
  `old->park_lock`。§1.3 的 A 行与 INV-P3 已按此改写。
- 既有锁序（`kernel/include/core/lock.h:24` 起）已声明
  `proc_lock -> park_lock`、`park_lock -> runq_lock`、`park_lock -> signal_state.lock`、
  `cg_node.lock -> proc_lock -> runq_lock -> pfa.lock`，以及
  `proc_lock -> files_struct.lock -> VFS 全局锁`、`proc_lock -> mm_struct.lock`、
  `proc_lock -> a20_handle_table.lock`。

### 1.2 目标不变量

- **INV-P1（park_lock 侧：park/等待状态机）**：`state`/`park_state`/`wait_seq`/
  `wake_reason`/`on_cpu` 由该任务自己的 `task->park_lock` 保护。
  `on_cpu` 在此列的理由：`current.c:40-41` 与 `sched.c:1829/:1860` 是同一批发布者，
  语义是"占着某个 CPU 的执行槽"，与 `dispatching` 不同（见 INV-P4b）。
- **INV-P2（runq_lock 侧：队列成员与发布身份）**：`on_rq`/`cpu_id`/`sched_level`/
  `ready_since` 由 `runq_lock` 保护；`dispatching`/`owner_cpu` 由 `runq_lock` **作为
  发布动作**写入，在 `runq_lock` 之外只允许原子 acquire/release 读。
  一个任务任一时刻恰好在"某个 runq 上 / 某个 CPU 上运行 / park 在某个等待对象上"
  三者之一。**跨两把锁的转移必须按 `park_lock -> runq_lock` 这一唯一方向嵌套**。
- **INV-P3（两把 task 锁的排序）**：需要同时持有两个不同任务的 `park_lock` 时，
  按 task 指针地址升序加锁、逆序释放。唯一例外是本 CPU 的 current /
  outgoing（`g_cpu_current[cpu]` / `g_cpu_switching_out[cpu]` 指向者），它们只被本
  CPU 的切换路径触碰，不参与排序。**per-CPU 锁（`g_cpu_switch_out[]`）比
  `park_lock` 更外层**，不参与 task 地址排序：`g_cpu_switch_out[cpu] -> park_lock`。
- **INV-P4a（切换路径无 `proc_lock`，覆盖全部五处）**：
  `sched()`、`context_switch()`、**`context_switch_locked()`**、
  **`proc_switch_complete()`**、`proc_switch_complete_locked()` 的函数体内都不出现
  `proc_lock` 获取。前三个是 3 处取锁 callsite 所在的函数体，
  后两个只消费/释放调用方已持有的锁，但**每次切换都必经**，
  调用方函数体断言覆盖不到，因此显式列名。
  门禁形式见 §1.5：源码里用一对 `SCHED_SWITCH_PATH_BEGIN/END` 标记把
  `context_switch_locked` / `context_switch` / `sched` 圈起来做负向断言，外加对
  `kernel/proc/current.c` 里 `proc_switch_complete*` 的独立负向断言。
- **INV-P4b（禁止 `runq_lock -> park_lock` 反转）**：`proc_runq_pick_local()` 与
  `sched_runq_steal_locked()` 在**只持本 CPU runq_lock** 的条件下写
  `dispatching`/`owner_cpu`（`sched.c:1585-1586`、`sched.c:1658-1659`），
  `park.c:66` 的入队与 `sched.c:1695` 的 unpick 则在 `park_lock` 下取 runq_lock。
  这两条方向相反，所以：**pick 侧绝不取 `park_lock`**；`sched()` 里对
  `next->park_lock` 的获取发生在 `proc_runq_pick_local()` **返回之后**（runq_lock
  已释放），`sched_runq_unpick_locked()` 同样在 `park_lock` 之下取 runq_lock
  （`sched.c:1695` 早于 `:1698` 的 `dispatching = 0`）。两方向一致，无反转。
  代价是 park_lock 的持有者不能直接信任 `dispatching`/`owner_cpu`：这些字段在
  park 侧一律改成原子读（唯一的读者是 `park.c:50-52` 用它算 IPI 目标 CPU）。
  这条是本轮**唯一**新增的锁序限制，`kernel/include/core/lock.h` 的
  "Never acquire proc_lock while holding a runqueue lock" 一节按此改写。
- **INV-P5**：真正全局的残余状态（全局任务表迭代：OOM 扫描、聚合统计、debug dump、
  pid 命名空间遍历等低频路径）改用专职的 `tasklist_lock`（新名字，新语义：
  保护任务列表成员关系 `task_list_head` / `all_next` / `all_prev`，**不保护调度状态**），
  锁序 `tasklist_lock -> park_lock`，替换 `lock.h` 原 `cg_node.lock -> proc_lock -> ...`
  链中 proc_lock 的位置。
  `proc_first_task_locked()` / `proc_next_task_locked()`（`proc.c:149`/`:154`，后者包
  `all_next` 的内核地址校验，`:155-162`）改为要求 `tasklist_lock`，函数名保留 `_locked` 后缀。

### 1.3 转换规则（按类）

对 265 处引用先做**普查分类**（结果写入 `impl-notes-proc.md` 的普查表）。
分类判据是「这个 callsite 保护的是哪一组字段」，按 §1.2 的字段归属表（INV-P1 /
INV-P2 / tasklist）归类，**不按是否出现在热路径归类**。

| 类 | 典型位置 | 转换 |
|---|---|---|
| A 切换发布（`next` 侧） | `sched.c:1936`（`sched_runq_unpick_locked(next)`）、`:1938`（`current->state = PROC_RUNNING`）、`:1925-:1953` 的取锁窗口、`:1963-:1972` 的 late-wake recheck | 在 `proc_runq_pick_local()` 返回**之后**取 `next->park_lock`，把 `state`/`on_cpu` 的发布与 unpick 一起放进这个临界区；`current`（本 CPU）的 late-wake recheck 取自身 `park_lock`。**pick 与加锁之间的窗口不是"天然闭合"**：见下方「窗口 W1」 |
| A' 切换发布（`old`/outgoing 侧） | `current.c:40-55`（`proc_switch_complete_locked`）、`current.c:130-134`（`proc_set_current` 内的回调） | 拆成两个单任务锁窗口：`context_switch_locked()` 里在 `proc_set_current()` 之前先取 `old->park_lock`（`old = g_cpu_switching_out[cpu]`）完成 outgoing 收尾（顺序：next→old 还是 old→next 由 INV-P3 地址排序决定，两个都在时按地址升序）；`proc_switch_complete()`（新栈上、`sched.c:1888`）只取单个 `old->park_lock`。`proc_set_current()` 里的回调调用**移出**到调用点显式执行。per-CPU slot 改由 `g_cpu_switch_out[cpu]`（新 per-CPU 锁）保护，取代今天的 `g_cpu_switching_out[cpu]` 原子 + 全局 `proc_lock` 组合 |
| B wake 发布 | `proc_try_wake*` 家族、`park.c:40-89` | 已在 `park_lock` 下；`park.c:50-52` 读 `on_cpu`/`dispatching` 改原子读（INV-P4b）。`park.c:66` 的 `proc_runq_enqueue_locked` 保持 `park_lock -> runq_lock`，**注释 `Caller must hold proc_lock`（`sched.c:1473`/`:1510`）今天已经是假的**（`park.c:66` 就是在 park_lock 下调它的），本轮把这两个注释改写为 `Caller must hold the task's park_lock` |
| C 定时器/僵尸 | `sched_scan_timers`、`sched_reap_zombies` | **两者都由 `sched()` 直接调用**（`sched.c:1912`、`sched.c:1917`），不是 idle 驱动。`sched_reap_zombies` 在单把 `proc_lock` 下走完整任务表（`sched.c:1734`）并对候选做 `proc_get`，因此改造后它仍是**切换路径上的全局锁点**，不得按低频路径处理：表遍历换 `tasklist_lock`，每个候选的 `state` 读与 reap 决策在该任务的 `park_lock` 下做（`tasklist_lock -> park_lock`）。`sched_scan_timers` 的 deadline 扫描同理：任务表项用 `tasklist_lock`，每任务的 `park_deadline` 在其 `park_lock` 下读写 |
| D 生命周期入口 | fork/exit/wait4/信号投递中"改目标任务调度状态"的 callsite | 改目标 `park_lock`；只改列表成员关系的改 `tasklist_lock` |
| D' 改调度属性并跨 runq | `proc_sched_set()`（`sched.c:748-850`）、`proc_sched_get()`（`sched.c:728`）、`sched_runq_requeue_locked()`（`sched.c:665-711`，双 runq 锁按 CPU 号升序） | `proc_sched_set` 改目标 `park_lock`；`sched_runq_requeue_locked` 在其之下按 `first/second` 顺序取两把 runq_lock（`sched.c:675-680` 已是升序），读写 `on_rq/cpu_id/dispatching/on_cpu`。这一类今天完全没有 A–F 条目，漏掉它就等于 `sched_setaffinity` 路径在 proc_lock 撤掉后无人负责 |
| E1 只读列表的低频迭代 | `loadavg.c:86`、`pidns.c:244/:276/:300`、`proc.c:201`/`:234` | 纯 `tasklist_lock` |
| E2 迭代中读调度状态 | OOM 扫描（`kernel/mm/oom.c:37`）、`/proc` 聚合、`debug.c:737` | `tasklist_lock` 取任务指针，再逐个在该任务 `park_lock` 下读 `state`/计数；**不得只持 `tasklist_lock` 就读 `t->state`**，否则与 park_lock 保护的 `state` 毫无同步关系（今天 oom.c:40 读 `t->state` 是靠 proc_lock 兜底的，改后会静默失效） |
| E3 console dump | `uart_dump_tasks()`（`kernel/drivers/char/uart.c:80`，取锁在 `:83`） | **不映射到 `tasklist_lock`**。`uart.c:111-116` 的注释记录了原因：这个 dump 在关中断态持全局锁逐 task 打 kdebug 到同一 UART，从顶半路径触发就会在自己的 IRQ 下重入 console 并 livelock 在它正在打印的那把锁后面——这正是 `CTRL_C_CONTEXT_SPLIT` 把 task 表 dump 从顶半路径踢到 `uart_getc()` 任务上下文的原因。改法是**快照后打印**：`tasklist_lock` 下把 pid/ppid/pgid/sid/state/name 复制进固定栈上缓冲（条数上限写进实现笔记），释放后再 `kdebug`。`uart.c:82` 的 `LOCK_ORDER` 注释同步改写 |
| F 其他子系统借 proc_lock 做跨任务状态转移 | cgroup 节流（`sched.c:1798-1820`，在 `context_switch_locked` 内）、`debug.c` | cgroup 那段在 `next->park_lock` 已持有时执行，不再单独加锁；`debug.c` 按 E1/E2 归类 |

#### 切换路径上的两个 late-wake 窗口（必须显式规定）

- **W1：pick 与 `next->park_lock` 之间**。今天的注释（`sched.c:1957-1958`）
  "Recheck under proc_lock, which serializes against proc_try_wake()"**与代码矛盾**：
  `proc_try_wake()`（`park.c:181`）只取 `task->park_lock`（`:188`），从不取
  `proc_lock`。今天 recheck 与唤醒者分别用两把不同的锁写同一批字段，是**没有被
  串行化**的，改成 `park_lock` 之后这条路径才第一次真正被串行化。所以本轮把它记为
  **修正**而不是"天然闭合"：A 类实现必须让
  `proc_runq_pick_local()` → `spin_lock_irqsave(&next->park_lock)` 之间的窗口由
  `next` 自己的 park 状态机覆盖（`proc_try_wake_locked_common` 的
  `PROC_PARK_WOKEN` / `default: return 0` 分支，`park.c:39`/`:88-90`），
  并在实现笔记里写清「pick 后到加锁前被唤醒会发生什么、为什么无害」。
- **W2：recheck 释放锁到 `context_switch(idle)` 之间**。`sched.c:1968` 把
  `cur->state` 置 `PROC_RUNNING`，`:1972` 释放锁，直到 `:1980`
  `context_switch(idle)` 之间，唤醒者仍可持 `cur->park_lock` 把 `state` 改回
  `PROC_READY` 并入 runq（`park.c:66`）。规定：
  1. fall-to-idle 分支在 `context_switch(idle)` 之前**必须再取一次 `cur->park_lock`**，
     重查 `cur->state == PROC_READY && cur->on_rq`，命中则改为 keep_current 走 `out`；
  2. `context_switch(idle)` 本身按 INV-P4a 的要求**不取 `proc_lock`**；idle 是
     `next == proc_current()` 的分支（`sched.c:1827-1835`），其发布字段与 A 类同构，
     因此取 `idle->park_lock`（idle 永不 park，其 `park_lock` 无竞争者）。

### 1.4 风险与降级路径

- 这是仓库文档明确警告过的"高风险核心协议重写"（`docs/eevdf-scheduler.md`）。
  因此**逐类提交**：每完成一类，跑 §7 对应冒烟再进行下一类。
- **降级许可**：某一类若被证明转换不安全（找不到闭合竞争窗口的方案），允许该类
  保留 `proc_lock`，但必须在 `impl-notes-proc.md` 记录论证过程；INV-P4a 的 gate
  断言范围只覆盖切换路径（A / A'），不因降级而放宽或删除。**降级不得发生在
  A / A' 类**——这两类是 P0 目标本身，降级即目标未达成，直接停在那里如实记录。
- tokenized Park/Wake 的语义（`wait_seq`/`park_state` 状态机、donate 变体）**逐位保留**，
  本轮只改"哪把锁保护哪一步"，不改协议本身。
- `proc_sched_assert_task_locked()` 这类 debug 断言必须随锁语义同步改写，不许留
  恒真断言。

### 1.5 gate 演进（proc agent 专属）

`tools/gates.toml` 的 `check-process-lock-split-boundary`
（当前 7 条断言，本轮开工前实测 `make check-process-lock-split-boundary` →
`7 assertions PASS`）逐条处置如下。**没有列在这里的断言不许删、不许改**：

| gates.toml 行 | 现状 | 处置 |
|---|---|---|
| :717-719 `PROCESS_LOCK_SPLIT_AUDIT` | 文档锚点 | **保留**；`docs/archive/process-lock-split-audit.md` 追加本轮记录 |
| :721-723 `SCHED_LOCAL_PICK_LOCK_SPLIT_BEGIN` | 标记存在（`sched.c:1596`） | **保留**，标记范围 `sched.c:1596-1676` 不变 |
| **:725-728** `task_t \*next = proc_runq_pick_local\(\);[\s\S]{0,2048}uint64_t flags = spin_lock_irqsave\(&proc_lock\)` | **正向**断言：A 类转换后必然失败（`sched.c:1924` 之后不再出现 `spin_lock_irqsave(&proc_lock)`） | **必须演进**，否则 §0 的"禁止删 gate"会把 A 类逼成违规。语义等价替换为「pick 之后 2048 字符内必须出现对 `next` 的 per-task 锁获取，且该获取被 `if (next)` 保护」：`…proc_runq_pick_local\(\);[\s\S]{0,2048}if \(next\)[\s\S]{0,256}spin_lock_irqsave\(&next->park_lock\)`。演进理由写进 `impl-notes-proc.md` |
| :730-734 `negate`：`SCHED_LOCAL_PICK_LOCK_SPLIT_BEGIN…END` 之间不得出现 `spin_lock_irqsave(&proc_lock)` | 已满足 | **保留**；按 INV-P4b 追加一条同形态的负向断言：标记区间内也不得出现 `spin_lock_irqsave(&…->park_lock)`，把"pick 侧不取 park_lock"钉死 |
| :736-742 `negate`：`proc_runq_pick_locked` 不得回潮 | 已满足 | **保留** |
| :744-749 `runqueue_parallel_pick_peak` | 并行 pick 峰值埋点 | **保留**；INV-P1/INV-P2 拆分后该埋点的读侧不改 |
| :751-753 `SCHED_STRESS: lock-split PASS` | 冒烟锚点 | **保留**并要求复验通过 |

新增断言：

1. `SCHED_SWITCH_PATH_BEGIN` / `SCHED_SWITCH_PATH_END` 标记必须存在，且标记区间内
   （覆盖 `context_switch_locked` :1792、`context_switch` :1891、`sched` :1898）
   不得出现 `spin_lock_irqsave(&proc_lock)` —— 对应 INV-P4a 的三个函数体。
2. `kernel/proc/current.c` 内 `proc_switch_complete` / `proc_switch_complete_locked`
   函数体范围不得出现 `proc_lock` —— 对应 INV-P4a 的第四、第五个点。
   这两条是新钉的**覆盖补强**：只查 `sched()`/`context_switch()` 会让 P0 目标
   在门禁全绿的情况下完全没达成。
3. `tasklist_lock` 存在且被 `lock_counters_register` 注册（见 §5.1）。

形态参照现有 negate+pcre2 断言（`tools/gates.toml:730-734`）。

## 2. g_net_lock：socket 表分片

### 2.1 现状事实

- `g_net_lock` 定义在 `kernel/net/socket.c:20`，只在 `net_init()`（`:164`）里
  `spin_init(&g_net_lock)`（`:165`），**从未 `lock_counters_register`**（见 §5.1）。
  全树 **235 处文本引用**
  （`grep -rn g_net_lock kernel/ | wc -l` → 235），其中**真正的取锁点只有 54 处**
  （`grep -rn "spin_lock_irqsave(&g_net_lock)" kernel/ | wc -l` → 54）。
  范围规划按 54 个取锁点 + 「哪些状态由哪把锁保护」来做，不按 235 这个被
  注释、声明、锁序文档放大的数字做——那会让"一轮只动一个数据面结构才能让前后
  测量可归因"的归因前提失效。§0 的"一轮只动一个数据面结构"因此明确为：
  **本轮只分片 socket 表 + slot 分配元数据，不触碰 per-socket 队列**。
- `kernel/net/socket_table.c` 头注释："Everything in this file runs under
  g_net_lock and nothing else"，表 + slot 位图。
- 既有不变量（`kernel/net/socket_packet.c:6`、`socket_table.c:11`）：
  **g_lwip_lock 与 g_net_lock 永不同时持有**。分片后表述改为
  "g_lwip_lock 与任何 net bucket lock 不同时持有"，语义不变。
- `NET_MAX_SOCKETS` 是 profile 常量：`net_profile.h:108` 服务器档 65536、
  `:136` 默认档 1024。分桶数必须同时整除这两个值下的 slot 数，见 §2.2。

### 2.2 设计

#### 2.2.1 分桶必须按位图字对齐（本节为复核推翻旧设计的重写）

旧稿写"每桶保护：槽位成员关系、slot 位图中该桶覆盖的位段"，**不可实现**：
空闲位图 `g_sock_free[(NET_MAX_SOCKETS+31)/32]`（`kernel/net/socket_registry.c:15`）
是按 slot 连续排的 `uint32_t` 字数组，注册（`:46-67`）与注销（`:68-94`）都是对
整字的 read-modify-write；哈希分桶后同一个字里的 32 个 slot 分属最多 32 个桶，
两把桶锁会互相踩同一字。

改为**连续分区，且桶边界对齐到位图字**：

- 桶大小固定为 512 个 slot（= 16 个 `uint32_t` 字），因此
  - 服务器档：65536 / 512 = **128 桶**；
  - 默认档：1024 / 512 = **2 桶**。
- 位图随之重排为 `uint32_t g_sock_free[NBUCKETS][16]`，**一个位图字只属于一个桶**，
  注册/注销只 RMW 自己的字，不存在跨桶踩字。
- 桶号 = `slot >> 9`，与 vfile/dcache 表的连续分区同形（不是哈希）；
  `net_register_socket_locked` 改为在**调用方所在 CPU 的起始桶**上环形扫描 128 个桶，
  起始桶 = `cpu_current_id() % NBUCKETS`，保证并发注册落在不同桶上。
- 这条分区方式顺带保证了 `net_socket_table_walk` 的分桶遍历是连续的，见 §2.2.4。

#### 2.2.2 按 `reg_idx` 索引的两张全局表必须搬进 socket 或桶

旧稿漏了这两处，它们同样隐含"全树只有一把 `g_net_lock`"：

- `kernel/net/socket_packet.c:53` `g_pkt_bound_slots[(NET_MAX_SOCKETS+31)/32]`
  （`:61-72` 的 `net_packet_bound_acquire/release` 做整字 RMW）。
- `kernel/net/socket_queue.c:32` `g_rxq_tally[NET_MAX_SOCKETS]`
  （`:38`、`:47-57`、`:63-73`、`:85-91` 的 `net_rxq_reset_slot` /
  `net_rxq_bytes_added_locked` / `net_rxq_bytes_removed_locked` / `net_rxq_bytes_locked`）。

两处的所有访问点**都已经持有 `net_socket_t *s`**（签名如此），
所以不需要额外的桶锁，直接把状态搬进 `net_socket_t`：

- `g_pkt_bound_slots` → `s->pkt_bound_marked`（per-socket 1 bit，复用已有 padding）。
  `g_pkt_bound_count`（`socket_packet.c:53`）保持全局原子——`:38-52` 的注释已说明
  它就是给无锁读侧用的计数，语义不变。
- `g_rxq_tally` → `s->rxq_tally`，读写全部在该 socket 自己的锁下，随 §2.2.3 的
  per-socket 字段结论一起定。`net_rxq_reset_slot(idx)`（`:34`）改为
  `net_rxq_reset_locked(s)`，因为调用点在 `net_register_socket_locked`
  （`socket_registry.c:63`），手里已经是 socket 而不是 idx。

#### 2.2.3 per-socket 字段与全局计数器

- **per-socket 字段**：仅被单 socket 读写的状态（队列入队/出队、状态字）
  改为 per-socket 锁或原子变量；普查后按实际热度和共享面决定，
  结论写 `impl-notes-net.md`。
- **全局计数器**（perf/统计类）：改原子变量，读侧聚合。
- **读多写少的 lookup**：直接用分桶锁（TCG 环境下 RCU 收益存疑，先不过度工程；
  若普查显示 lookup 是压倒性读路径且分桶锁已把争用归零，则记录结论即可）。

#### 2.2.4 全表遍历的方案（旧稿缺失，本节新增）

`net_socket_table_walk`（`socket_table.c:41-58`）今天在一把 `g_net_lock` 下扫完
`NET_MAX_SOCKETS` 并回调 procfs，**而该文件 :15-23 的不变量恰恰是**
"`closed` 与注销在同一临界区，所以快照看不到半释放的 socket"。
分桶后如果什么都不定，就会诱导实现去同时持 128 把桶锁 —— `kernel/fs/vfs/dcache.c:355-370`
明确记录这条路会 livelock（"every VFS_DCACHE_BUCKET_LOCKS lock was held, with
interrupts disabled … an interrupt that reached any lookup spun on a lock its own
interrupted context was holding"），并且 dcache 的解法是**一次一把**。

因此规定：

1. **禁止**同时持多把桶锁遍历。遍历改为**逐桶**：`for (b = 0..NBUCKETS) { 取 bucket[b];
   扫 512 个 slot; 释放 }`，与 `vfs_dcache_invalidate_all`（`dcache.c:357-375`）
   的形态一致。
2. **每个 slot 在桶锁下取 `proc_get()` 引用**，然后释放桶锁再执行
   `fn(s, arg)`，回调结束后 `proc_put()`。这样 socket 的存储与内核栈在回调期间存活，
   不需要持锁跑 procfs。
3. 不变量改写为**逐桶一致**而非全局一致：
   "遍历看到的每个 socket 在其所在桶的临界区内都还是 `in_registry` 的引用持有者；
   遍历不保证看到跨桶的同一时刻快照。遍历期间被并发 `close()` 的 socket 可能被
   看到（`closed` 已置位，`net_table_claims()` 对它返回假或由 procfs 行渲染自行
   过滤），但绝不会看到一个已被注销、slot 已释放的 socket。"
   `net_unregister_socket_locked`（`socket_registry.c:68-94`）在桶锁下先 `proc_put`
   登记表引用再清 slot，因此 §3 的 `proc_get` 引用保证回调看到的是释放前的对象。
   `socket_table.c:15-23` 的原注释同步改写。
4. procfs 渲染回调（`fn`）仍然是自旋上下文：不阻塞、不分配、不调 lwIP——约束不变。

#### 2.2.5 锁序

- 新增 `net_bucket[i] -> net_bucket[j]`（按桶号排序，仅在需要双桶原子性时）
  并入 `kernel/include/core/lock.h` 的网络段；`g_lwip_lock` 与 bucket 的互斥不变量
  保留原注释位（`lock.h:35` 的 `g_lwip_lock -> g_net_lock` 一行改为
  `g_lwip_lock -> net_bucket[*]`）。
- **文件所有权冲突的处置见 §4.1**：`kernel/include/core/lock.h` 是 proc agent 独占、
  net agent 禁 `kernel/core/`，而这一行必须由 net agent 写。

## 3. slab：本轮不做（复核结论 7 的落实）

旧稿把「slab 每个 size class 一把锁、无 per-CPU 数组」列为三个瓶颈之一，
**这个现状陈述是假的**。已合入的 `fc71e9674`
（`git merge-base --is-ancestor fc71e9674 HEAD` 在本分支为真）实现了：

- per-CPU 数组 `static slab_cpu_array_t g_slab_cpu[SLAB_NR_CACHES][CONFIG_NR_CPUS]`
  （`kernel/mm/slab.c:92`），64 字节对齐（`:86-90`）；
- `SLAB_CPU_ARRAY_CAP 16`（`:84`）、`SLAB_CPU_REFILL 8`（`:85`）；
- 分配的本地数组快路径（`:451-462`，只在本地关中断，不取 cache 锁），
  空了才取 `caches[idx].lock` 并在锁内批量补给到 `SLAB_CPU_REFILL`（`:466-476`）；
- 释放的本地数组快路径（`:542-551`），满了整批回灌 cache 锁（`:555-562`）。

即旧稿 §3 描述的三条设计（本地数组命中无锁、批量补给、批量回灌）**已经是树的现状**。
因此：

- slab agent **无事可做**，`lock-after.md` 里也不会出现 slab 维度差异，
  写一个恒零对比表列属于自欺；
- 原 §3 的两条约束移入 §5.1 的测量前提：**slab cache 锁从未注册进
  `lock_counters_register`**（全树 `grep -rn lock_counters_register kernel/` 只有
  `proc.c:365`、`sched.c:589`、`lwip_stack.c:369`、`block_cache.c:332`、
  `page_cache.c:507`、`dcache.c:108` 六处，`kernel/mm/` 下零处），
  所以 `tools/lock_bench.py:82` 的 `TRACKED_LOCKS = ["proc","lwip","slab","runq"]`
  里的 `"slab"` 永远不会出现在 `/proc/a20/lock_contention` 里。
  若仍要观测 slab，注册 `caches[i].lock` 的调用点在 `kernel/mm/slab.c`，
  属于 §4 表给 baseline agent 开的具名例外，**不是本轮的锁拆分工作**，
  只作为 §5 的可测性前置条件，由 baseline agent 在 §6 第 1 步串行完成。
- `CONFIG_SLAB_DEBUG`（sentinel、审计）路径行为不变；slab 相关既有 gate
  （`check-concurrency-foundation` 等）保持绿——这两条保留为回归要求，不再是设计。

## 4. 文件所有权（并行实施边界）

实现 agent 并行工作在同一 worktree，**文件集不相交**，违反者造成的冲突由
该 agent 负责：

| Agent | 独占 | 禁入 |
|---|---|---|
| proc | `kernel/proc/`、`kernel/core/sync.c`、`kernel/core/lock_counters.c`、`kernel/include/proc/`、`kernel/include/core/lock.h`、`kernel/include/core/sync.h`、`tools/gates.toml` 中 proc 相关 gate、`docs/archive/process-lock-split-audit.md`（追加）、**§4.2 表中 21 个跨边界文件**（含 `kernel/mm/`、`kernel/fs/`、`kernel/ipc/`、`kernel/abi/`、`kernel/include/mm/`、`kernel/include/cg/` 中被点名的那几个） | `kernel/net/`、`kernel/mm/slab.c`、`Makefile`、`tools/lock_bench.py` |
| net | `kernel/net/`、`kernel/include/net/`、`kernel/drivers/net/`（如需） | `kernel/proc/`、`kernel/core/`、`kernel/mm/slab.c`、`kernel/include/core/lock.h`（走 §4.1 单向授权，不直接编辑） |
| baseline（先于上两者串行完成） | `tools/lock_bench.py`、`Makefile`（仅追加 `include tools/targets-bench.mk`）、`tools/targets-bench.mk`、`user/cmds/`（如需负载程序）、`docs/measured/`，**外加 §5.1 点名的三个 `lock_counters_register` 调用点**（`kernel/net/socket.c`、`kernel/mm/slab.c`，分片/拆分开始**之前**打） | `kernel/` 其余全部 |

（slab agent 一行已删除：见 §3。`tools/targets-bench.mk` 与 `Makefile` 的 include 行
已经由 baseline agent 建好，本轮起点上它们是已存在的未跟踪文件
——`ls -la tools/lock_bench.py tools/targets-bench.mk` 两者都在，
`grep -n "include tools/targets" Makefile` 见 :1335。）

### 4.1 `kernel/include/core/lock.h` 的跨界：单向授权，不是并行

§2.2.5 要求 net agent 往 `kernel/include/core/lock.h` 的网络段写
`net_bucket[*]` 锁序，而该文件是 proc agent 独占、net agent 明确禁入 `kernel/core/`。
处置：

- **net agent 不得直接编辑 `kernel/include/core/lock.h`**；它在
  `impl-notes-net.md` 里提交一个「锁序条目请求」（形如
  `g_lwip_lock -> net_bucket[*]`、`net_bucket[i] -> net_bucket[j]`（i<j）），
  并附上它对 `kernel/net/` 内的具体嵌套点清单。
- **proc agent 是 `lock.h` 的唯一写者**，在集成轮（§6 第 3 步）按该请求补写网络段。
  锁序文档晚于锁存在一个窗口，因此 net agent 在其分支上必须留
  `NET_BUCKET_ORDER_PENDING` 断言标记，集成轮由 proc agent 一并清除——
  与 §1.5 的 gate 演进同一条纪律：锁序不留"已写但没断言"的状态。
- 两 agent 都在**同一 worktree**工作这一点使该流程成本很低：集成轮本来就是串行的。

### 4.2 无人拥有的文件（旧稿漏掉的两类必改文件之第二类）

`proc_lock` 的 265 处引用里，**62 处落在 proc agent 独占范围之外**
（`grep -rn proc_lock kernel/` 逐文件计数，剔除 `kernel/proc/`、
`kernel/include/proc/`、`kernel/include/core/`、`kernel/core/`）。
按 §1.1 的判据，这 62 处里只有 **21 处是真取锁点**（分布在 12 个文件），
其余 41 处是注释/锁序声明里的引用。两者都必改，但改法不同：

| 文件 | 引用 | **取锁点** | 改法 |
|---|---|---|---|
| `kernel/fs/fdtable.c` | 12 | **6** | D 类 + `files_struct.lock` 链锚点 |
| `kernel/abi/native/sys_native_debug.c` | 11 | **3** | A/D 类（改目标任务 `park_lock`）或 E1 |
| `kernel/ipc/envelope.c` | 6 | **2** | D 类 + `a20_handle_table.lock` 链锚点 |
| `kernel/drivers/char/uart.c` | 4 | **1** | E3 类（快照后打印，见 §1.3） |
| `kernel/abi/linux/sys_signal.c` | 4 | **2** | B/D 类（信号投递） |
| `kernel/abi/linux/sys_proc.c` | 3 | **1** | B/D 类 |
| `kernel/mm/mmap.c` | 2 | **1** | D 类 + `mm_struct.lock` 链锚点 |
| `kernel/mm/oom.c` | 2 | **1** | E2 类（`tasklist_lock` + 逐任务 `park_lock`） |
| `kernel/mm/pt.c` | 2 | **1** | D 类 + `mm_struct.lock` 链锚点 |
| `kernel/mm/cg_mem.c` | 2 | **1** | F 类（cgroup） |
| `kernel/ipc/signalfd.c` | 2 | **1** | B/D 类 + `a20_handle_table.lock` 链锚点 |
| `kernel/fs/procfs/procfs.c` | 2 | **1** | E1/E2 类 |
| `kernel/include/mm/vm.h` | 3 | 0 | 仅 `LOCK_ORDER` 声明，随 `lock.h` 锁序改写同步 |
| `kernel/include/mm/pt.h` | 1 | 0 | 同上 |
| `kernel/include/cg/cgroup.h` | 1 | 0 | 同上（`lock.h:11` 的 `cg_node.lock -> proc_lock -> ...` 镜像） |
| `kernel/net/lwip_stack.c` | 1 | 0 | 注释（`:367` 提到 proc_lock 作为分片判据），不动代码 |
| `kernel/drivers/core/udriver.c` | 1 | 0 | 注释（oom 路径说明），不动代码 |
| `kernel/drivers/core/driver_class.c` | 1 | 0 | 注释，同上 |
| `kernel/drivers/char/pty.c` | 1 | 0 | 注释，同上 |
| `kernel/abi/native/registry.c` | 1 | 0 | 注释（`:77` 说明为何不跨 `proc_lock` 持 registry 锁），不动代码 |

（`kernel/include/proc/signal.h` 的 2 处、`kernel/core/sync.c` 的 1 处属于 proc
agent 独占范围，未计入 62。）

这不是"顺手改一下"的问题，而是**锚点问题**：`kernel/include/core/lock.h:31-33`
把三条锁序链锚在 `proc_lock` 上——

```
proc_lock -> files_struct.lock -> VFS global-file/vnode locks
proc_lock -> mm_struct.lock
proc_lock -> a20_handle_table.lock
```

`proc_lock` 一撤，这三条链的锚点就悬空，而链条的**另一端**恰好在上面这张表里
（`fs/fdtable.c` 是 files_struct.lock 那一环，`mm/mmap.c`+`include/mm/vm.h` 是
mm_struct.lock 那一环，`ipc/envelope.c` 是 a20_handle_table.lock 那一环）。
因此：

- 这些文件**必须**在本轮改，或者**必须**由 proc agent 明确裁定"锚点迁到哪把锁"
  并写进 `lock.h`。不允许留悬空。
- 归属规则：**谁拥有这条链的另一端，谁拥有该链**。据此
  - `mm_struct.lock` 链（`mm/mmap.c`、`mm/pt.c`、`include/mm/vm.h`、`include/mm/pt.h`）
    与 `mm/oom.c` 归 **proc agent** 在集成轮处理（`oom.c` 属 E2 类，见 §1.3）；
  - `files_struct.lock` 链（`fs/fdtable.c`、`fs/procfs/procfs.c`）归 proc agent；
  - `a20_handle_table.lock` 链（`ipc/envelope.c`、`ipc/signalfd.c`）归 proc agent；
  - `drivers/char/uart.c`（E3 类，见 §1.3）、`mm/cg_mem.c`（F 类）、
    `abi/**` 三个文件（`sys_native_debug.c`、`sys_signal.c`、`sys_proc.c`）归 proc agent；
  - 表中标"取锁点 0"的 9 个文件是纯注释/声明，只需在锁序改写时同步，
    **不构成所有权争议**，但它们的注释必须改对，否则会变成过期文档
    （`lock.h:11` 的 `cg_node.lock -> proc_lock -> runq_lock -> pfa.lock` 镜像
    在 `include/cg/cgroup.h:11`）。
- 上表 21 个文件全部并入 proc agent 的独占集合，**§6 第 3 步的集成轮由 proc agent
  一次性完成**。这是把 21 个取锁点串行化的代价，写在这里是为了让并行边界真实，
  而不是让 agent 各自踩进别人文件。

## 5. 测量协议

### 5.1 可测性前置（必须先做完，否则 §5 的数字没有意义）

复核指出本节缺可测性，实测确认：

- `g_net_lock` **从未注册进 lock_counters**：`kernel/net/socket.c:165` 只有
  `spin_init(&g_net_lock)`，全树 `grep -rn lock_counters_register kernel/` 只有
  `kernel/proc/proc.c:365`（`&proc_lock, "proc"`）、`kernel/proc/sched.c:589`
  （`&sched_runq[i].lock, "runq"`）、`kernel/net/lwip_stack.c:369`
  （`&g_lwip_lock, "lwip"`）、`kernel/fs/block_cache.c:332`、
  `kernel/fs/page_cache.c:507`、`kernel/fs/vfs/dcache.c:108`，加
  `kernel/core/lock_counters.c:34` 的定义与 `kernel/mcu/mcu_stubs.c:58` 的空桩。
- `tools/lock_bench.py:82` 的 `TRACKED_LOCKS = ["proc", "lwip", "slab", "runq"]`：
  `"slab"` 永远不会出现（见 §3），`"net"` 根本没列入。四项里两项没有测量通道，
  而本轮两个 P0 目标之一正是 net。

因此 §6 第 1 步（baseline，串行）必须先完成：

1. 在 `kernel/net/socket.c` 注册 `lock_counters_register(&g_net_lock, "net")`；
   分片后改为注册每个 `net_bucket[i]`（名字 `net_bucket0..127`，或注册一把
   `net_bucket_registry` 汇总锁并让每把桶锁在 acquire 失败时把计数累加过去——
   二选一，写进 `impl-notes-net.md`）。
2. 在 `kernel/mm/slab.c` 注册 7 个 `caches[i].lock`（名字 `slab0..slab6`），
   使 `"slab"` 这个名字真实存在。
3. 把 `tools/lock_bench.py` 的 `TRACKED_LOCKS` 改成实际注册的名字集合，并新增
   `"net"`。
4. 上面 1、2 两条**是对 `kernel/` 的修改**，与 §5.2「worktree 在任何分片/拆分
   修改前是干净的」冲突。处置：把 `kernel/net/socket.c`、`kernel/mm/slab.c`、
   `tools/lock_bench.py` 从 "baseline 禁入 `kernel/` 全部"里**开一个具名例外**
   （§4 表已按此改写），并把基线采集拆成两段：
   - **B0（真基线）**：未打补丁的树，`lock_contention` 里没有 `net`/`slab` 行——
     这一段的 net 证据只能来自 `impl-notes-net.md` 的静态普查，明确记为"无计数通道"；
   - **B1（可测性补丁后的基线）**：注册调用点合入、**分片/拆分尚未开始**，
     采集含 `net`/`slab` 的完整基线。
   - `lock-after.md` 与 **B1** 对比（协议、镜像、QEMU 命令行一致），B0 仅作交叉参考。
   这样"分片/拆分前采基线"仍然成立，又不会出现 before/after 用两套通道。

### 5.2 采集形态

- **基线先行**：worktree 在任何分片/拆分修改前是干净的，baseline agent
  依次执行 B0、B1（§5.1）。`tools/lock_bench.py`、`tools/targets-bench.mk`、
  `Makefile` 的 `include tools/targets-bench.mk`（:1335）与 `make bench-locks`
  目标（`tools/targets-bench.mk:30`）**在本轮起点上已经存在**（未跟踪文件），
  因此本节不再写"先建"，改为"baseline agent 维护它们"。
- **负载形态**（**注意命令名**）：
  - `mm_stress` — 用户态命令 `mm_stress`
  - `net_stress_test` — **用户态命令是 `net_stress_test`**，不是 `net_stress`。
    `net_stress` 只是 `tools/lock_bench.py:62` 的 scenario key；
    `grep -n "net_stress" tools/smoke_cases.py` 无输出，`tools/smoke_cases.py` 里
    没有这个名字；`smoke-smp-lock-contention`（`tools/targets-smoke.mk:464-470`）
    直接往控制台敲的就是 `net_stress_test`。
  - `sched_stress`、`futex_stress`
  - 镜像与 init 形态复用 `tools/targets-smoke.mk` 已有案例；
    QEMU 参照 `docs/roadmap/perf-overhaul.md` §5 手工形态
    `-smp 8 -accel tcg,thread=multi`（宿主不支持则 4 核并在结果中注明）。
- 每负载 **5 次取中位数**，记录 min/max；采集
  `/proc/a20/lock_contention`（per-lock + callsite 采样）与 `/proc/a20/perf`。
  `lock_contention` 的 callsite 采样有已知限制（`tools/targets-smoke.mk:455-461`：
  call-site 归因哈希进固定 32 槽表，冲突时该 caller 的自旋数丢失而锁总数仍计），
  所以低采集次数下"什么都没归因"是合法结果，不得据此断言。
- **噪声纪律（重写）**：`server-readiness.md:27-28` 与
  `docs/net/net-lanes.md:953` 记录的 0–920024 噪声带，来自**另一套方法学**——
  那是"不清窗口的**累计自旋数**"观测（自 boot 起累加）。而 `lock_bench.py:9-15`
  明确改成每个 workload 先 `echo reset > /proc/a20/lock_contention` 再读，
  量纲是"本 workload 窗口内的自旋数"，**两者不可直接搬运**。
  因此本轮的做法是：
  1. 采集当场的同二进制对照：每个 workload 在同一场次里跑 5 次，
     用这 5 次的 max−min 作为**该指标本场的观测带宽**；
  2. 另在 B1 阶段用**同一二进制、同一镜像、同一场次**把 4 个 workload
     各多跑 3 次，得到"同一份代码在同一场次的复测带宽"，
     写进 `lock-baseline.md` 作为新方法学的带宽来源；
  3. before/after 差异小于上面第 2 条得到的带宽的项一律写"不可分辨"，禁止声称；
  4. 旧文档的 0–920024 只作为"TCG 下自旋计数不可作门禁"的定性依据引用，
     不用作数字阈值。
- **产物**：`docs/measured/lock-baseline.md`（B0 + B1）、`docs/measured/lock-after.md`
  （与 B1 同协议）、同目录 JSON 原始数据；记录 QEMU 命令行、镜像哈希、宿主元数据。

## 6. 实施顺序

1. baseline（串行，先于一切分片/拆分修改）：B0 → 注册调用点（§5.1）→ B1。
2. 两路并行实现（§4 边界内），各写 `impl-notes-proc.md`、`impl-notes-net.md`。
   §4.2 的 21 个跨边界文件在 proc agent 的范围内，proc agent 自行排期；
   §4.1 的 `lock.h` 网络段在集成轮写。
3. 串行构建修复循环（≤3 轮）：一次性构建含两路修改的内核，修复集成期编译错误；
   proc agent 在本轮补 `lock.h` 网络段并清除 `NET_BUCKET_ORDER_PENDING`。
4. 门禁清单全绿后，同协议复测，产出 `lock-after.md` 对比表（对比对象是 B1）。

## 7. 验证门禁清单（verifier 串行执行）

```sh
make check            # CHECK_FAST_GATES 快速层（host 侧 16 gate）
make check-concurrency-foundation
make check-process-lock-split-boundary   # 演进后，见 §1.5
make smoke-riscv64
make smoke-smp-bringup
make smoke-sched-stress
make smoke-proc-stress
make smoke-futex-stress
make smoke-mm-stress
make smoke-vfs-stress-smp2
make smoke-network-suite
make smoke-smp-lock-contention
```

任何一个 FAIL 都阻塞收尾；修复属于对应所有者文件的，在 impl-notes 里登记后由
verifier 修复循环处理，修不动的如实记录 FAIL 状态并停在那里，不许放宽断言。

开工前实测基线：`make check-process-lock-split-boundary` →
`check-process-lock-split-boundary: 7 assertions PASS` / `PASS`（本轮开工时跑通）。

## 8. 复核结论落点表

| # | 复核结论 | 落点 |
|---|---|---|
| 1 | INV-P4 覆盖不全 | §1.1 表（列出 5 处）、§1.2 INV-P4a（显式列名 `context_switch_locked` / `proc_switch_complete*`）、§1.5 新增断言 1/2 |
| 2 | §1.5 漏列 `gates.toml:725-728` 正向断言 | §1.5 表第 3 行：必须演进，给出等价正则与理由 |
| 3 | A 类未规定第二把 task 锁 | §1.1 第 3 条、§1.2 INV-P3、§1.3 新增 **A' 类** |
| 4 | steal/pick 写远端 `dispatching/owner_cpu` 与 INV-P1 冲突 | §1.2 **INV-P1 拆分**（park 侧 vs runq 侧）+ **INV-P4b**（禁止 `runq_lock -> park_lock`）、§1.3 A/B 行、§1.5 新增负向断言 |
| 5 | "桶覆盖的位段"不可实现 | §2.2.1 重写为按位图字对齐的连续分区（512 slot/桶）、§2.2.2 两张 `reg_idx` 全局表搬进 socket |
| 6 | 文件所有权漏两类 | §4.1 `lock.h` 单向授权；§4.2 跨边界清单（62 处引用 / **21 个取锁点** / 12 个文件需改）+ 锁序锚点问题；§4 表把 21 个文件并入 proc agent |
| 7 | slab per-CPU 已实现 | 旧 §3 删除，改为 §3「本轮不做」，附 `fc71e9674` 与 `slab.c:84/:85/:92/:451-476/:542-562` |
| 8 | §5 缺可测性 | §5.1 注册 `g_net_lock` + 7 把 slab cache 锁、修 `TRACKED_LOCKS`、B0/B1 两段基线；§4 表给 baseline 开具名例外 |
| 9 | 噪声纪律量纲不同 | §5.2 噪声纪律重写：当场带宽 + 同二进制同场次复测对照 |
| 10 | 负载名与"先建 bench"过期 | §5.2：命令名 `net_stress_test`（附 `targets-smoke.mk:470` 与 `smoke_cases.py` 无命中的证据）、bench 产物已存在（附 `ls` 与 `Makefile:1335` 证据） |
| 11 | net 普查数字把引用当取锁 | §2.1：235 文本引用 / 54 取锁点，并说明 §0「一轮只动一个数据面结构」按 54 重述 |
| 12 | 全表遍历无方案 | §2.2.4：逐桶遍历 + `proc_get` 引用 + 不变量改写为逐桶一致，引 `dcache.c:355-370` |
| 13 | "天然闭合"前提未言明 | §1.3「窗口 W1」：记为**修正**，并解释现注释与代码矛盾 |
| 14 | 第二个 late-wake 窗口 | §1.3「窗口 W2」：fall-to-idle 前再取 `cur->park_lock`；`context_switch(idle)` 取 `idle->park_lock` |
| 15 | E 类映射 tasklist_lock 会 livelock / 失去 state 同步 | §1.3 E1/E2/E3 拆分：E3 快照后打印（引 `uart.c:111-116`）、E2 逐任务 park_lock |
| 16 | A–F 漏 `proc_sched_set`；enqueue/remove 注释已是假的 | §1.3 新增 **D' 类**（`proc_sched_set` / `proc_sched_get` / `sched_runq_requeue_locked`）；§1.3 B 行改写 `sched.c:1473`/`:1510` 的 `Caller must hold proc_lock` 注释 |
| 17 | C 类标注"（idle 驱动）"与代码不符 | §1.3 C 行改写：两者由 `sched()` 在 `sched.c:1912`/`:1917` 直接调用，且改造后仍是切换路径上的全局锁点 |

### 复核结论中被部分推翻的部分

- 结论 10 的一半（「先建 `tools/lock_bench.py` + `make bench-locks` 已过期，
  `tools/lock_bench.py` 已在树中，`make bench-locks` 与 `tools/targets-bench.mk`
  都不存在」）：**与本轮起点不符**。实测
  `ls -la tools/lock_bench.py tools/targets-bench.mk` 两者都存在（37688 / 1836 字节，
  未跟踪），`grep -rn "bench-locks" Makefile tools/*.mk` 在
  `tools/targets-bench.mk:30` 命中，`Makefile:1335` 已
  `include tools/targets-bench.mk`。这两个文件正是 §4 表里 baseline agent 的产出，
  复核把它们当成"树里没有"来判断。本节改为"已存在，baseline agent 维护"，
  并保留复核指出的**真实**部分：负载命令名是 `net_stress_test`。
- 结论 6(b) 的清单与实测的差异：复核列了 `kernel/core/sync.c`（1 处）与
  `kernel/include/proc/signal.h`（2 处），这两处属于 proc agent 独占范围，
  实测未计入 62 这个数；其余逐文件计数与实测一致。
  **一处需要加强**：复核给的是"约 60 处引用"，实测 62 处引用里只有
  **21 处是真取锁点**（`spin_lock_irqsave(&proc_lock)` 逐文件计数），
  分布在 12 个文件；另 41 处是注释/锁序声明。§4.2 按两列分别列出，
  因为改法不同（取锁点要换锁，声明只要同步文本）。
- 结论 7（slab 已实现）：复核结论成立，且覆盖面比复核说的更宽。
  `CONFIG_SLAB_DEBUG` 的 sentinel/审计分支仍在（`slab.c:19-20` 默认、
  `:139` popcount 校验、`:305`/`:386`/`:397`），启动早期的 CPU 号越界由
  `slab_cpu_array()` 的 `if (cpu >= CONFIG_NR_CPUS) cpu = 0;`（`slab.c:97-98`）
  兜住——注意这**不是**旧稿设想的"退化到直连 cache 锁"，而是一开始就落在
  CPU 0 的数组上。旧稿 §3 是纯粹的重复设计工作，且两条约束的写法与已合入实现
  不同，照抄反而会把退化路径改坏。