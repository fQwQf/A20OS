# impl-notes: slab per-CPU 对象数组（magazine 形态）

本文件是 slab 缓存 per-CPU 数组这条实现路的现状普查与落点记录。**本路的核心结论：设计 §3 要求的
per-CPU 对象数组在当前树里已经实现，且已是 HEAD 的祖先提交，因此本路没有可做的功能实现；
实际改动只有两处零行为变化的补强（测量通道 + IRQ/CPU-id 取用顺序），其余是核实与风险记录。**

复核意见里关于 §3 的那一条（"已经实现，slab agent 无事可做"）经核实**成立并采纳**，依据见 §2。

---

## 1. 输入材料的可得性（诚实性声明）

> 历史扫描输出保留原样；其中 `qemu-10.0.13+ds/...` 是当时记录的源树路径。该解包目录已从仓库清理，当前应将路径理解为外部 Debian QEMU 10.0.13+ds 源包根目录下的相对路径。来源与解包定位见[迁移说明](../history/2026-10-08/migration.md)。


- 设计文档 `lock-serialization-split.md` **在本仓库中不存在**。核实方式：
  - `find . -path ./kernel/external -prune -o -name "*serialization*" -print` → 只命中
    `.git/refs/heads/feat/lock-serialization-split` 与 `qemu-10.0.13+ds/tests/unit/test-visitor-serialization.c`；
  - `git rev-list --all | while read c; do git ls-tree -r --name-only $c | grep -q "lock-serialization-split.md" && ...` → 遍历全部可达提交，`exit=1`，无任何提交含该文件；
  - `grep -rn "lock-serialization-split" --include=*.md --include=*.toml --include=*.py .` → 无输出。

  因此 §3 的原文无法逐字引用。下文所有"设计要求"一律以**本次 ask 正文**给出的四条要求为准
  （本地数组命中 / 批量补给 / 批量回灌 / 深度 8–16 / 早期启动与单核退化 / 优先复用 per-CPU 设施），
  并逐条对照代码核实。
- 复核意见里引用的 `tools/lock_bench.py` 与 `make bench-locks` 在本树中**也不存在**
  （`ls -la tools/lock_bench.py` → No such file or directory）。这条复核意见针对的不是本树，
  本路不据此行动，只在 §6 记录。

---

## 2. 现状普查：per-CPU 数组已存在

**提交**：`fc71e9674` "mm, core: per-CPU slab object arrays and a seqlock realtime reader"
（作者 fQwQf，2026-10-01）。`git merge-base --is-ancestor fc71e9674 HEAD` → **是**，已在 main 线上。
该提交改动 `kernel/mm/slab.c`（+275/-112 之前的对比行）与 `kernel/core/timekeeping.c`。

以下逐条核对 ask 的要求，全部命中：

| ask 要求 | 代码位置 | 结论 |
|---|---|---|
| 分配走本地数组命中（无 cache 锁） | `kernel/mm/slab.c:466-475` | ✅ 关中断 → `arr->count` 非零则直接弹出返回，全程不碰 `caches[idx].lock` |
| 数组空了从 cache 锁批量补给 | `slab.c:477-489` | ✅ `spin_lock_irqsave(&c->lock)` 后 `while (arr->count < SLAB_CPU_REFILL) slab_alloc_obj_locked(idx)` |
| 释放入本地数组 | `slab.c:555-562` | ✅ 关中断后 `arr->objs[arr->count++] = ptr` 直接返回 |
| 满了整批回灌 | `slab.c:565-572` | ✅ 持 cache 锁 `for (i < SLAB_CPU_ARRAY_CAP/2) slab_free_obj_locked(arr->objs[--arr->count], ra)`，回灌一半再放入本次对象 |
| 数组深度 8–16 | `slab.c:85-86` | ✅ `SLAB_CPU_ARRAY_CAP 16`、`SLAB_CPU_REFILL 8` |
| 优先复用内核已有 per-CPU 设施 | `slab.c:81-84` 注释 + `kernel/mm/frame.c:86-93` | ✅ 与页分配器的 CPU page batch 同构（同样的静态二维数组 + `aligned(64)` + `arch_current_cpu_id()` 钳位）。树中**不存在** `__per_cpu`/`percpu` 段设施（`grep -rn "per_cpu\|percpu\|__per_cpu\|this_cpu" kernel/include/core/*.h` 无输出），静态数组就是本树的既有 per-CPU 设施 |
| 早期启动 per-CPU 区未就绪须退化 | `slab.c:93` | ✅ **按构造成立，无需退化分支**：`g_slab_cpu` 是静态 BSS 数组，不存在"未就绪"状态；`slab_cpu_array()` 里 `if (cpu >= CONFIG_NR_CPUS) cpu = 0`（`slab.c:98-99`）兜住 CPUID/MPIDR 读出的越界硬件 id。单核同理：`armv7m/include/cpu.h` 与 `ppc64le/include/cpu.h` 的 `arch_current_cpu_id()` 直接 `return 0`，`x86_64/include/cpu.h` 在 `CONFIG_NR_CPUS == 1` 时也 `return 0`（注释明确写这是为了避开 CPUID 的 KVM VM-exit） |

`CONFIG_SLAB_DEBUG` 的行为不变性也已核实：
- `slab_page_valid()` 里的位图审计 `if (CONFIG_SLAB_DEBUG && slab_popcount(sp) != sp->in_use) return 0;`（`slab.c:140`）仍在，
  且 `kfree` 的快路径在推进数组之前**仍然先过它**（`slab.c:524` `int is_slab = slab_page_valid(sp);`），所以审计没有被数组绕过。
- `slab_validate_sp()` 的 free_list 遍历 + `in_use + free_count == total` 校验仍在 `slab_alloc_obj_locked`（`slab.c:313-315`）
  与 `slab_free_obj_locked`（`slab.c:394-396` / `405-407`）两侧。
- **勘误**：ask 正文说的 "sentinel" 在树中不存在。`grep -rn "sentinel\|SENTINEL" kernel/mm/ kernel/include/mm/` 无输出；
  `CONFIG_SLAB_DEBUG` 实际提供的是 free_list 校验器 + `alloc_bits` 位图 + popcount 审计，没有写哨兵值的代码。
  本路按"行为不变"理解并据此保护现有三条审计路径。

**内存占用**：`sizeof(slab_cpu_array_t)` = 16×8 + 2 + 2 = 132，因 `aligned(64)`（`slab.c:91`）补齐到 192 字节；
总计 `7 × CONFIG_NR_CPUS × 192`（NR_CPUS=1 时 1344 B，NR_CPUS=4 时 5376 B）。

---

## 3. 设计落点（实际改动）

既然功能已存在，本路的落点收敛为两条"不改语义、只补可观测性与纪律"的改动。

### 3.1 让 `/proc/a20/lock_contention` 出现 slab 维度（复核意见"§5 缺可测性"的落地）

复核意见指出 slab 的 cache 锁完全没有注册进 `lock_counters`，所以 per-CPU 数组这条改动的
"after" 侧没有测量通道。核实为真：

- `spinlock_t` 的 `contended_acquires / contended_spins / contended_max_spins`
  由 `spin_lock_at()` **无条件**维护（`kernel/include/core/lock.h:70-74` 注释 + `lock.h:114` 起的实现，
  "The counters are only touched when the lock is contended, so the uncontended fast path stays a single exchange"），
  即**数据早就在攒**，只是没人注册所以 `/proc/a20/lock_contention` 打不出来。
- 全树 `lock_counters_register` 调用点：`kernel/proc/proc.c:365`("proc")、`kernel/proc/sched.c:589`("runq", 循环
  `CONFIG_NR_CPUS` 次)、`kernel/net/lwip_stack.c:369`("lwip")、`kernel/fs/block_cache.c:332`、`kernel/fs/page_cache.c:507`、
  `kernel/fs/vfs/dcache.c:108`("dcache")。`kernel/mm/` 下**零注册**。
- 注册表容量够：`LOCK_COUNTERS_MAX 192`（`kernel/core/lock_counters.c:23`），当前占用 = 5 + `CONFIG_NR_CPUS`，
  加 7 条仍有大量余量。
- `lock_counters_register()` 不分配内存（只有 `lock_counters_enable_callsite()` 才 kcalloc），
  所以在 `slab_init()` 里调用是安全的。

**改动**：`slab_init()` 的 `spin_init(&caches[i].lock)` 之后加一行
`lock_counters_register(&caches[i].lock, "slab_cache")`，并 `#include "core/lock_counters.h"`。
热路径零成本（计数器本就在 `spin_lock_at` 里维护，注册只是让格式函数能遍历到这把锁）。

### 3.2 把 `arch_local_irq_disable()` 提到 CPU-id 取用之前（对齐被模仿的参考实现）

**改动前**的写法（提交 `fc71e9674` 的原始行号）是 `slab.c:454-456`（kmalloc）与
`slab.c:543-545`（kfree）：先算 `slab_cpu_array(idx)` 再关中断；
而被它明确模仿的页分配器是反过来的（`kernel/mm/frame.c:570-573`：先读 flags → 关中断 → **再**读 `arch_current_cpu_id()`）。

- **当前是否是真 bug：否。** 本内核是协作式抢占：`need_resched` 只在 `proc_sched_safe_point()`
  （`kernel/proc/sched.c:1029-1036`）这个显式安全点被 `proc_yield()` 消费，`kernel/` 内没有任何
  隐式抢占计数（`grep -rn "CONFIG_PREEMPT\|preempt_count" kernel/` 无输出）。所以改动前
  `slab.c:463-467` 之间那个"IRQs 重新打开但还没有任何调度点"的窗口里，线程不可能被迁移。
- **为什么仍然要改**：这是一条纯顺序调整、零可观测差异的改动，但它把正确性从
  "依赖内核保持非抢占" 降级为 "不依赖"，同时让 slab 的纪律与 frame.c 逐字一致——否则下一个把
  slab 数组照抄到别处、或给内核加上抢占的人，会照抄到一个**错误的顺序**。这正是 §2 里那条
  "mirroring the frame allocator's CPU page batch" 注释所承诺的一致性。

**改动**：`slab.c:464-466` 与 `slab.c:553-555` 两处把 `arch_local_irq_disable()` 上移到
`slab_cpu_array(idx)` 之前；改后两处都是"读 flags → 关中断 → 取 CPU 槽"，与 frame.c 一致。

---

## 4. 复核意见逐条处理

针对本路（§3 slab / §5 测量 / §0 现状事实）的意见：

1. **"§3 的 slab per-CPU 数组在当前树里已经实现，§0 的现状事实'无 per-CPU 数组'为假"** → **采纳，已核实**。
   证据见 §2（`git merge-base --is-ancestor fc71e9674 HEAD` 为真 + 逐条代码位置）。
   据此本路不做功能实现，ask 里"现状：无 per-CPU 数组"这句前提记为不成立。
2. **"§5 缺可测性：slab 的 cache 锁完全没注册进 lock_counters"** → **采纳并落地**，见 §3.1。
   （该意见同时提到 `tools/lock_bench.py` 里 `TRACKED_LOCKS` 写死 `["proc","lwip","slab","runq"]`：
   本树无 `tools/lock_bench.py`，无法核对；但"slab 未注册"这半句经独立核实为真，见 §3.1。）
3. 其余关于 proc 锁序 A–F 分类、`g_net_lock` 分桶、`lock.h` 归属、net 全表遍历、late-wake 窗口、
   `context_switch_locked()` 等 blocking 意见，**属于 proc / net 两条实现路，本路不处理，也不评论**。

本路自查中额外发现的、与本路相关的两点（不改，记录）：

4. `tools/smoke_cases.py:521` 里 `smoke-mtcorrupt` 的 `forbid` 列表含 `'SLAB DEBUG'`，
   但**全树没有任何代码打印 "SLAB DEBUG"**（`grep -rn "SLAB DEBUG" kernel/` 无输出）。
   slab 实际的诊断前缀是 `"[SLAB BUG]"` / `"[SLAB DBG]"`（`slab.c` 中共 18 处）。
   也就是说这条 forbid 是**永不触发的空断言**，该 smoke case 目前只靠 `'SIGSEGV'`/`'FATAL'` 兜底。
   `tools/smoke_cases.py` 不在本路允许改动的文件范围内，故仅记录、不动手。
5. ask 说 per-CPU 数组的 before/after 对比不会出现 slab 维度差异——**在本次改动落地之前确实如此**；
   §3.1 落地后，`/proc/a20/lock_contention` 会新增 `slab_cache` 一行（7 条同名，靠 `spin_set_debug`
   的 `container` 无法区分，见 §6 待验证 3），集成阶段应据此设计前后对照。

---

## 5. 现有实现的行为差异（记录，不在本次改动范围内回退）

per-CPU 数组引入了三处**可观测但语义合理**的行为变化，本次一律不回退（回退等于退回每对象一次
cache 锁，正是本路要消除的东西）：

1. **统计口径**：`slab_get_stats()`（`slab.c:613-641`）只遍历 cache 的 partial/full/spare 链表，
   而停在 CPU 数组里的对象**仍然记在 `sp->in_use` 上**（`kfree` 只在回灌时才 `sp->in_use--`，
   `slab.c:399-402`）。所以 `allocated_objects` / `allocated_bytes` 会把已释放但仍停在数组里的
   对象算作"已分配"，最多高估 `7 × CONFIG_NR_CPUS × 16`。提交说明里明确承认了这一点。
   **影响**：`slab_reclaim_spare()`（`slab.c:643-660`）只回收 spare 链表，停在数组里的对象所在的
   页因 `in_use > 0` 不会进 spare，**不会被误释放**，内存安全；只是那点内存的回收被推迟到下一次回灌。
2. **双 free / 野指针的发现时机后移**：`kfree` 快路径只做了 `slab_page_valid(sp)`（`slab.c:524`），
   offset 合法性、`obj_idx` 是否已分配（即 double free）等检查都推迟到回灌时的
   `slab_free_obj_locked()`（`slab.c:366-391`）。panic 仍会发生，但**发生时机与 `caller_ra`
   都不再对应**（`slab.c:570` 传的是触发本次回灌的那个 `kfree` 的 `caller_ra`，
   而非当初错误 free 的那个）。定位难度上升，属诊断质量退化。
3. **UAF 更难被偶然抓到**：释放后的对象在回灌前仍留在原 slab 页里、且 `alloc_bits` 仍标记为已分配，
   读它拿到的是旧内容而非已被复用的内容。这削弱了 use-after-free 的自检能力，
   `CONFIG_SLAB_DEBUG` 也无法发现（它只在 slab 自己的不变量被破坏时报）。

---

## 6. 待验证清单（本路未能验证，交集成阶段）

1. **没有构建、没有运行**。按共同约束禁止 `make` 与 QEMU，本路**没有执行完整构建，也没有启动过 QEMU**。
   作为**替代且仅此一项**的检查，本路用仓库里**已存在的** generated 头文件（只读复用，未写入任何构建目录）
   跑了三次编译器语法检查，全部通过：

   ```
   B=.kernel-build/riscv64-qemu-virt-riscv64-both-dev-smp4
   riscv64-linux-gnu-gcc -fsyntax-only -Wall -Wextra -Werror -ffreestanding -nostdlib \
     -fno-builtin -fno-common -std=gnu99 \
     -Ikernel/arch/riscv64/include -Ikernel/include -Ikernel -Ikernel/net/lwip_port \
     -Ikernel/external/lwip/src/include -Ikernel/external/littlefs -Ikernel/external/littlefs/compat \
     -DLFS_MALLOC=lfs_kmalloc -DLFS_FREE=lfs_kfree -DLFS_NO_DEBUG -DLFS_NO_WARN \
     -DLFS_NO_ERROR -DLFS_NO_ASSERT -DLFS_NO_TRACE \
     -I$B/generated -march=rv64imafdc_zicsr_zifencei -mabi=lp64 -mcmodel=medany \
     -DRISCV64 -DCONFIG_RISCV64 -DCONFIG_ABI_LINUX -DCONFIG_NR_CPUS=4 \
     kernel/mm/slab.c
   ```
   - riscv64 / `CONFIG_NR_CPUS=4`：退出码 0，无输出。
   - 同上再加 `-DCONFIG_SLAB_DEBUG=1`：退出码 0，无输出（覆盖审计路径）。
   - x86_64（`gcc`，`-Ikernel/arch/x86_64/include`、复用 `x86_64-qemu-virt-x86_64-both-dev-smp4/generated`、
     `-DCONFIG_NR_CPUS=4`）：退出码 0，无输出。

   **这不等于构建通过**：未链接、未做全树编译、未覆盖 arm32/armv7m/loongarch/ppc64le/riscv32 等其余架构，
   也未做任何运行时验证。正式构建与验证仍归集成阶段。
2. **`/proc/a20/lock_contention` 是否真的会打出 7 条 slab 行，需实机确认**。
   已核实的是"计数器无条件维护"（`kernel/include/core/lock.h:70-74`）与"注册表容量足够"，
   未核实的是 `lock_counters_format()`（`kernel/core/lock_counters.c:65` 起）在 buf 较小时的截断行为。
3. **7 条同名 `slab_cache` 的可区分性**。`lock_counters_register(&caches[i].lock, "slab_cache")`
   传的是同一个字符串常量，而 `lock_counters_format_one()` 只打印 `name`；
   `spinlock_t` 里有 `container` 字段但 `lock_counters_register` 不设置它。
   因此 7 条会显示成 7 行一模一样的 `slab_cache`，**无法区分是哪个 size class**。
   若集成阶段需要按 size class 归因，需要给 `core/lock_counters.c` 增加按容器或按下标命名
   的能力——那超出本路允许的文件范围。备选：`spin_set_debug(&caches[i].lock, "slab32", &caches[i])`
   能填 `name`/`container`，但 `spin_set_debug` 走的是 `CONFIG_DEBUG_LOCKS` 的 `[LOCK-STALL]`
   报告路径，且名字长度仍受限；本路**没有**采用它，以免改变调试输出。
4. **`CONFIG_NR_CPUS > 1` 下的真实收益未测**。理论收益是"每 refill/drain 一次 cache 锁换 8 个对象"
   （`slab.c:86` / `slab.c:570`），但本树 QEMU 均为 ≤4 vCPU，且锁持有期极短，
   净收益需要实测，不能从代码推断。
5. **跨架构**：`kernel/arch/*/include/cpu.h` 里 `arch_current_cpu_id()` 的实现差异已逐个读过
   （aarch64 读 MPIDR_EL1、riscv32 读 `__boot_hart_id`、loongarch 读 CPUID 0x20、
   x86_64 在 `CONFIG_NR_CPUS==1` 时短路返回 0），`slab_cpu_array()` 的 `cpu >= CONFIG_NR_CPUS → 0`
   钳位覆盖了其余情况；但 `riscv32` 的 `__boot_hart_id` 在 SMP 下的取值未核实。
6. **早退路径的一致性**：`kmalloc` 的大对象分支（`slab.c:435-453`，`size >= SLAB_MAX_OBJ`）
   完全不经过 CPU 数组，直连 buddy 分配器。这是设计使然，但意味着 ≥2048 字节的分配**没有**享受
   本优化；本次未改动。

---

## 7. 改动清单（文件级）

- `kernel/mm/slab.c`
  - 新增 `#include "core/lock_counters.h"`；
  - `slab_init()`：每把 cache 锁注册进 `lock_counters`（名字 `slab_cache`）；
  - `kmalloc_flags()`：`arch_local_irq_disable()` 提到 `slab_cpu_array(idx)` 之前；
  - `kfree()`：同上。
- 未触碰 `kernel/include/mm/` 下任何头文件（本路最终没有需要改的 slab 头）。
- 未触碰 `kernel/core/**`、`kernel/external/**`、`tools/**`。
- 未执行任何 `git add/commit/push`，未执行 `make`，未启动 QEMU；只跑过 §6 待验证 1 里记录的
  三次 `gcc -fsyntax-only`。
