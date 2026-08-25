# A20OS 并行编译负载热路径优化：分桶锁与 fsync 收敛（`fqwqf/performance-overhaul`）

> 分支：`fqwqf/performance-overhaul`，基自 `main` (`ca6c7008`)。 目标：把 8 核并行编译负载中"page cache / block cache / dcache 单全局锁 + fsync 全盘写回"的 结构性开销拆掉，并给后续优化提供可量化的锁竞争观测点。 验证：riscv64 单核 smoke 全组 + 真实 `-smp 8 -accel tcg,thread=multi` 下的 `mm_stress`/`vfs_stress`/`sched_stress` 全部 PASS。

## 1. 动机

既往性能审计（2026-08）指出的热路径中， 当前 `main` 仍保留三个高频全局 spinlock 和一条全盘 fsync：

- **page cache 单全局锁**（`g_page_cache_lock`）：每次 warm hit 都要取锁 + 做全局 LRU 摘链/插头。
- **block cache 单锁**（每个 mount 的 `bc->lock`）：每次 ext4 元数据/4K 页命中都串行。
- **dcache 单锁**（`g_dcache_lock`）：每次路径分量查找都串行。
- **fsync 全盘写回**：`vfs_fsync` 对每次 fsync 都 `bcache_sync(mnt)`，把整个 mount 的脏页 （包括其他并发进程的数据）全部写回。

这些在 8 个并发 rustc 下是"每个 syscall 都要付"的全局串行点。

## 2. 改动

### 2.1 page cache 分桶锁 + 第二机会 LRU（`kernel/fs/page_cache.c`、`include/fs/page_cache.h`）

- 新增 `PAGE_CACHE_BUCKET_LOCKS=1024` 把 262144 个 hash 桶分成 1024 组，每组一把 spinlock。
- warm hit 只取 bucket 锁 + 原子 refcount + 置 `accessed` 位，**不再**触碰全局锁、不再移动全局 LRU。
- 全局锁只留给 miss/grow/evict/invalidate 路径；evict 改为第二机会（clock）近似 LRU： 扫描时取候选页的 bucket 锁重查 refcount 再摘除，杜绝"并发 hit 已 pin、evict 仍回收"的竞态。
- 锁序不变量：`全局锁 → bucket 锁`，任何路径不得持 bucket 锁再取全局锁。

### 2.2 block cache 分桶锁 + 第二机会 LRU（`kernel/fs/block_cache.c`、`include/fs/block_cache.h`）

- 512B 池与 4K 池共用 `BCACHE_BUCKET_LOCKS=256` 把 hash 分组。
- warm hit（`bcache_get`/`pcache_get`）只取 bucket 锁；evict 在候选的 bucket 锁内重查 ref。
- 沿用 `writeback_lock` + 每 entry `dirty_gen` 的写序保护，未触碰持久化语义。

### 2.3 dcache 分桶锁 + 第二机会 LRU（`kernel/fs/vfs/dcache.c`）

- `VFS_DCACHE_BUCKET_LOCKS=64`；hit 只取 bucket 锁，evict 在 bucket 锁内对 refcount/vnode 做原子交接。
- slot 分配与 LRU 仍由全局锁保护，但全局锁只在 insert/evict 时持有（临界区很短）。

### 2.4 fsync 收敛到单文件（`kernel/fs/block_cache.c`、`kernel/fs/diskfs/ext4_sync.c`、`kernel/fs/vfs/file.c`）

- 新增 `bcache_sync_scoped(bc, page_nos, n)`：只写回列表内（排序去重后）的 4K 脏页 + 512B 脏块； 顺序与 `bcache_sync_checked` 完全一致（writeback_lock + dirty_gen）。
- 新增 `ext4_vn_sync()` 作为 `vnode_ops.sync_vnode`：收集该 inode 的数据 extent 页、 其 inode-table 页、以及它占用的 block group 的位图/组描述符页，只对这些页做 scoped 写回。
- `vfs_fsync` 优先走 `ops->sync_vnode`；无该 op 的 fs 回退到整 mount 同步。
- 正确性：crash 后不丢该文件数据、inode、以及标记其块已分配的位图；其他文件数据留给其自身的 fsync （与 Linux `fsync(file)` 语义一致，不保证父目录项）。

### 2.5 锁竞争观测（`kernel/core/lock_counters.c`、`include/core/lock.h`、procfs）

- `spinlock_t` 新增 `contended_acquires`/`contended_spins`，只在真正竞争时累加，非竞争 fast path 不变。
- `/proc/a20/lock_contention` 输出已注册锁的 `<name>: <contended_acquires> <contended_spins>`。
- 已注册：`proc`、`runq`、`page_cache`、`dcache`、`block_cache`、`vfile_table`。
- **callsite 归因**：对注册为 hot 的锁（当前为 `proc_lock`），竞争路径会把调用方的返回地址散列进 固定的 32 槽采样表（无共享记账锁），格式化时经 kallsyms 输出 `[锁名] 符号+偏移: 竞争 自旋`。 这用于回答"proc_lock 的竞争到底来自哪些调用点"，从而避免对调度器做无依据的重写。

### 2.6 修复：`spin_lock_at` 竞争下的丢失重获取（`include/core/lock.h`）

实现 2.5 时曾把 `while (exchange(lock,1))` 错写成 `if (exchange(...))`：等待者自旋结束后**没有重新 exchange** 就直接进入临界区，导致两个 CPU 同时认为持有锁。真实 SMP（`-smp 8 thread=multi`）下 表现为 vma/mmap 并发测试卡死。已恢复 `while` 语义并保留计数器；这是本分支最重要的正确性教训， 任何"给 hot lock 加计数"的改动都必须保留原获取循环。

## 3. 测量结果（riscv64，`-smp 8 -accel tcg,thread=multi`，`mm_stress` 后）

```
vfile_table: 0 0
page_cache:  0 0
dcache:      0 0
block_cache: 0 0
runq:        2..68 contended（若干万 spins）
proc:        33335 contended，16191822 spins   ← 剩余主瓶颈
```

- page cache / dcache / block cache 的全局锁竞争已归零（分桶生效），vfile_table 归零。
- `proc_lock` 现在是压倒性热点。**callsite 归因**（32 槽采样）显示其竞争来源：

```
proc: 33335 16191822
  [proc] mutex_lock+0x18e: 4458     <- 互斥量睡眠/唤醒
  [proc] mutex_lock+0x3f4: 5963     <- 互斥量 park/wake 协议
  [proc] idle_loop+0x34: 3001       <- idle->sched() 切换发布
  [proc] mutex_lock+0x866: 1484
  [proc] mutex_lock+0x878: 1429
  ...（其余调用点 1..9 次）
```

- 竞争高度集中于**互斥量争用时的 park/wake 协议**（`proc_park_prepare/commit/finish` 与 `proc_try_wake` 各自单独持 `proc_lock`）和**每次上下文切换的发布路径**（`sched()` 内联进 `idle_loop` 的 `spin_lock(&proc_lock)`）。
- 结论：要消除 `proc_lock` 竞争，需要把 tokenized Park/Wake 状态机从单一全局锁改为按等待对象 （wait queue / mutex / futex）分锁，并合并切换路径里 `sched()`/`context_switch` 的两次获取。 这属于 `docs/eevdf-scheduler.md` 与既往性能审计明确警告的"无完整并行编译负载验证前 不做的高风险核心协议重写"；本分支提供 callsite 归因工具，让该重写在正式基准复测前可先量化 收益，而不是靠猜测。

### 3.1 后续回合：切换路径两次 proc_lock 获取合并（已在 `fqwqf/performance-overhaul` 实现）

`sched()` 先为抢占检查取一次 `proc_lock`，随后 `context_switch` 再取一次用于发布 `on_cpu` 状态。 拆成 `context_switch_locked()`（假定调用方已持锁、用同一 irqsave flags 释放）后，`sched()` 在 抢占检查后不再释放锁，直接进入发布段，每次上下文切换只取一次 `proc_lock`。锁序不变（ `proc_lock -> mm_struct.lock`；`mm_context_enter` 实际只用原子操作），并且发布期间锁全程持有， "neither selected nor owned" 的观测窗口被完全消除（比原来更严）。

SMP8（`sched_stress + mm_stress`）测量：`proc` 竞争从 `33335/16191822` 降到 `16750/9024345`（约减半），mm/sched/vfs stress 全部 PASS。

### 3.2 后续回合：顺序读 readahead 窗口 64 KiB → 128 KiB

readahead 突发与 demand-fault 的 fault-around 共用同一个 16 页（64 KiB）常量。编译器输入以顺序读 为主，把 readahead 独立为 `PAGE_CACHE_READAHEAD_PAGES=32`（128 KiB）后，ext4 可以把一段连续 extent 合并成一次块设备请求而不是两次 64 KiB 往返；匿名/文件 fault-around 保持 16 页，以限制 每次缺页的内存承诺。低风险常量改动，`vfs_stress`/`mm_stress` 在单核与 SMP8 均 PASS。

### 3.4 后续回合：`proc_park_finish` 唤醒后无锁化

每次 park 周期里 `proc_park_finish` 只为复位唤醒后任务的状态就取一次 `proc_lock`。所有唤醒路径 （`proc_try_wake` 与定时器到期）都会先把 `wait_timer_index` 置回 -1，任务重新运行后这些字段没有 并发写者，且对 WOKEN/IDLE 任务的过期唤醒是 no-op。因此当没有待处理 timer 索引时跳过 `proc_lock`， 用原子写复位；仅有残留索引才回退到堆锁。

SMP8 mm_stress 测量：`proc` 竞争从 16-30K 区间降到 12-20K（中位数约 28K→16K）。 **测量教训**：曾同时把 `proc_park_commit` 在 `sched()` 返回后的 `wake_reason` 读取也无锁化，结果竞争 不降反升——这次获取实际上充当了 park/wake 循环的自然背压点。已回退该部分，只保留 finish 快路径。

### 3.3 后续回合：page cache 写回按 vnode 分锁

单一 `g_page_cache_writeback_lock` 互斥锁让 8 个并发进程的每次 fsync / 缓存压力写回都互相排队， 即使它们写的是互不相干的文件。`page_cache_writeback_vnode` 改取 64 把按 vnode 哈希的互斥锁 （同一 vnode 仍串行），全局锁保留给整盘写回（`vn == NULL`）。页级 `fill_lock` 与 dirty-gen 发布/清除路径不变，block-cache 写序不变量保留。SMP8 `mm_stress`+`vfs_stress` 连续两轮 PASS。

## 4. 明确不做/暂缓（以及原因）

> 更新（2026-08-25，`perf/core-modernization`）：原暂缓项中的"EEVDF 有序链表 → 树/heap" 与"`proc_wait4` 全局任务表扫描"已在后续回合解决，见第 7 节；其余两项维持暂缓。

- **`proc_lock` 拆分（park/wake 按对象分锁 + 切换路径合并获取）**：测量证明它是唯一剩余热点， 但修复必须重构 tokenized Park/Wake 协议与切换发布路径；在无法跑完整并行编译基准的前提下， 先保留 callsite 归因工具，待拿到真实工作负载证据再动。这是本轮最重要、也最需要谨慎的结论。
- ~~**EEVDF 有序链表 → 树/heap**~~：已完成（见 §7.1）。当时"runq 锁非主瓶颈、不应先做高风险 调度重写"的判断针对的是锁竞争维度；本回合的动机是消除任务数增长下的 O(n) 计算复杂度本身， 两者不矛盾。
- **slab per-CPU magazine**：高复杂度、高回归风险，且本次测量未把 slab 锁列为热点；应先用锁计数器 确认后再做。
- **ext4 HTree 目录查找**：dirent 已去掉逐块 kmalloc；HTree 是正确性敏感的增量，建议单独验证。

## 5. 验证命令

```bash
# 单核 smoke（项目既有门禁）
make ARCH=riscv64 ABI=both PROFILE=benchmark smoke-abi-linux
make ARCH=riscv64 ABI=both PROFILE=benchmark smoke-vfs-stress
make ARCH=riscv64 ABI=both PROFILE=benchmark smoke-mm-stress
make ARCH=riscv64 ABI=both PROFILE=benchmark smoke-sched-stress

# 真实 8 vCPU（注意 smoke 目标硬编码 -smp 1，必须手动带 thread=multi）
qemu-system-riscv64 -machine virt -m 1G -nographic -smp 8 -accel tcg,thread=multi \
  -kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp8/kernel.elf \
  -drive file=...,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
  -drive file=...,id=x1 -device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1 \
  # 然后在 shell 中依次执行 mm_stress / vfs_stress / sched_stress / cat /proc/a20/lock_contention
```

`smoke-*` 门禁把 QEMU 固定为 `-smp 1`，因此**单核 smoke 通过不能证明 SMP 正确性**； 本分支的 SMP 验证必须用上面带 `thread=multi` 的 8 vCPU 命令。

## 7. 后续回合：调度器与进程链表结构现代化（2026-08-25）

分支 `perf/core-modernization`，两个提交：

### 7.1 EEVDF 有序链表 → 增强 treap + RT 位图 O(1) pick（`sched:` 提交）

per-CPU runqueue 的三个 O(n) 路径被替换：

- **EEVDF 层**：deadline 有序双链表 → 以 (deadline, vruntime, 指针) 为键的随机化 BST （treap），节点增广 subtree-min vruntime。eligible 门控的 pick 沿树下降并用增广值剪枝， 等价于旧实现"按 deadline 序找第一个 eligible"，复杂度从 O(n) 降为摊还 O(log n)； insert/remove 同为 O(log n)。缓存的 leftmost/rightmost 分别服务无 eligible 时的 progress fallback 与 idle stealing（仍取最不紧迫任务）。
- **RT 层**：单 FIFO + 全队列最高优先级扫描 → 每优先级 FIFO 桶 + 位图字扫描 O(RT_BITMAP_WORDS)=O(1) pick。MCU bring-up 构建把桶数收缩为粗粒度（RT_PRI_LEVELS=8， 已在源码注释声明）；宿主目标保持 1..99 精确序。
- **顺带修复**：旧 `sched_runq_unpick_locked` 尾插回队既破坏 deadline 序、又漏补 `eevdf_weight` 记账；新实现经统一 insert/remove 路径自然修复。

验证（当前提交实跑）：riscv64 both-dev `-Werror`、`-DDEBUG` 断言构建（激活 CONFIG_DEBUG_SCHED_STATE 不变量检查）、NR_CPUS=8、loongarch64 四组构建零警告； smoke-riscv64 / sched-stress / proc-stress / futex-stress / mm-stress 单核 PASS； 手动 `-smp 8 -accel tcg,thread=multi`：8/8 CPU online，SCHED_STRESS 的 smp-runqueue + lock-split 子项 PASS，MM_STRESS PASS，全程零 panic。

### 7.2 `proc_wait4` 全局扫描 → children/线程组链表（`proc:` 提交）

每次 `wait4()` 及其每次唤醒重试都全扫全局任务表；孤儿 reparent、线程 reaper 查找、 exec 终止兄弟线程、exit_group 同样如此。现在每个任务维护两条 proc_lock 下的成员链：

- **children 链**：与 `->parent` 经 `proc_reparent_task_locked()` 成对维护 （覆盖 fork、孤儿 reparent、auto-reap、ptrace attach/seize/detach 全部改父点）， 链的摘除挂在 reap/detach 公共汇聚点 `proc_unlink_task_locked()` 上，僵尸直到被 收尸前始终可被发现。
- **线程组链**：以 tg_leader 为根，使任意线程可收同组兄弟线程之子（POSIX 语义不变， `__WNOTHREAD` 仍只看自己的 children）。

wait4 只扫等待组的 children 链 = O(组内子进程数)；匹配语义（pid/pgid 过滤、 WNOHANG/WUNTRACED/WCONTINUED/WNOWAIT）逐条保留。验证：四组构建零警告； smoke-proc-stress / pty-stress（wait4/WUNTRACED 路径）/ futex-stress（fork+waitpid 循环） / abi-linux 单核 PASS；SMP8 tcg thread=multi 下 SCHED_STRESS + PROC_STRESS PASS。 一次 futex-stress 运行在高负载宿主上超出预算未完成，干净复跑两轮均 PASS， 判定为宿主负载边缘抖动而非内核挂起（无 panic、无自旋痕迹）。

### 7.3 后续回合：每上下文切换的网络 BH 扫描门控 + MLFQ 残留清理（2026-08-25）

- **`net_inet_bottom_half_process_all` 槽位扫描门控**：该函数经 `sched()` 在每次 上下文切换执行，原先无条件扫全部 `NET_MAX_SOCKETS`（1024）槽位、每槽一次 acquire 原子加载。现在旗标数组所有者（socket_registry）提供 exchange 语义的 `net_bh_slot_mark/clear` 辅助函数维护全局挂起计数，process_all 先做单次 acquire 读早退；mark 与 g_net_lock 内 drain 的竞态由 exchange 语义天然保证 计数不漂移，错过本轮的事件保持 pending 到下次切换（与既有重扫容忍一致）。
- **MLFQ 残留死写入清除**：park 唤醒路径、park 直通派发 donate 路径、定时器到期 唤醒、make_ready 四处仍在写 `task->sched_level` 作"唤醒提权"，但每次 enqueue 都会覆写为 RT/EEVDF 队列选择子——全部可证死代码，删除；idle 的 `SCHED_LEVELS-1` 标记同理。`sched_task_linked_locked()` 从全任务表扫描改为 `state != PROC_UNUSED && !destroy_started` 双字段判定（unlink 与 UNUSED 转换 在 proc_lock 下成对发生，且半初始化槽位本就该拒绝，语义等价或更严）。

验证（当前提交实跑）：riscv64 `-Werror`/DEBUG 断言 + loongarch64 构建零警告； smoke-network-suite（DNS/TCP/UDP/ICMP/UNIX/TIMEOUT）、futex-stress、 timeout-test、sched-stress、abi-linux 单核 PASS；手动 SMP8 tcg thread=multi： 8/8 online，SCHED_STRESS + FUTEX_STRESS PASS 并正常关机。

## 6. 已知（非本分支引入）

基线 `main` 在部分 dev 镜像/配置下，mm_stress 的 `mseal-mprotect` 用例可能返回 errno=0 失败； 与本节改动无关。完整并行编译基准的重跑仍按既定流程执行。
