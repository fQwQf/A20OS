# 单级内存模型迁移（MM_AS_MODEL）

本文记录把 A20OS 的内存管理从**两级模型**（软件级区间抽象 VMA + 硬件页表，
两者必须保持一致）迁移到 **CortenMM 式单级模型**的目标设计、逐特性结论、
分阶段计划，以及**明确不做**的部分。

参考：J. Zhang 等，*CortenMM: Efficient Memory Management with Strong
Correctness Guarantees*，SOSP '25（DOI 10.1145/3731569.3764836）。
参考实现：<https://github.com/TELOS-syslab/CortenMM-Artifact>（Rust / Asterinas）。

代码位置：`kernel/include/mm/pt.h`、`kernel/mm/pt.c`。
契约：`MM_AS_MODEL`（`kernel/include/mm/vm.h`）、`MM_AS_CURSOR_ONLY_ENTRY`
（`kernel/include/mm/pt.h`）。

---

## 1. 为什么要迁移

两级模型要求每个操作同时调和两个差异极大的数据结构：区间树适合表达
on-demand paging 这类高级语义、且与具体 MMU 无关，代价是必须正确且高效地
同步它与页表。论文的观察是这两个目标已经基本消失：

* x86 / ARM / RISC-V 都使用基于多级 radix 树的页表，架构差异本来就用
  C 宏（我们是 `arch_pt_*` 系列 inline）掩盖，不需要另一个抽象层。
* 高级语义确实需要 MMU 之外的状态，但那不构成另起一层的理由。

对 A20OS 而言，动机**不是论文的可伸缩性收益**。当前 `mm->lock` 已经把
地址空间的全部变更串行化，可伸缩性尚未到手。真实的动机是正确性与
结构性债务：`mm_vma_defer()`（`kernel/mm/vma.c`）存在的原因仅仅是区间世界
需要在页表世界里睡眠（`vma_release()` 可能触发 VFS 回写），两种粒度的
锁需求打架。可伸缩性是这套改造的附带收益，且必须**实测**才算数。

## 2. 目标模型

```
                 ┌─────────────────────────────────────────┐
   映射状态 ────▶ │ per-PTE 元数据数组（权威）              │
   (唯一真相)     │ 挂在覆盖该页的页表页描述符 pt_meta_t 上   │
                 └─────────────────────────────────────────┘
                              ▲
   事务式接口 ────▶ mm_addrspace_lock(range) → mm_cursor_t
                     query / map / mark / unmap（原子应用，析构时逆序解锁）
                              ▲
   写者互斥 ────▶ 页表页锁（MCS 队列锁），不相交区间不串行
```

一个状态字节：

| 位 | 含义 |
|---|---|
| `[7:4]` | class：`INVALID` / `ANON_VIRT` / `ANON_MAPPED` / `ANON_SHARED` / `FILE_PRIVATE` / `FILE_SHARED` / `VMO` / `SWAPPED` / `PT_NODE` |
| `[3]` | COW `shared` 位（多映射引用同一帧） |
| `[2:0]` | 有效权限 R/W/X |

已映射页的物理帧**不在**元数据里重复存放：它在 PTE 中，引用计数在
`pfa.meta[].refcount`。元数据只承载 MMU 无法表达、而 fault 必须知道的信息。

### 2.1 为什么状态不能放进 PTE 的软件位

参考实现把 `Status` 编码进 PTE 的 `paddr` 字段并配 `has_map = false`。这条路
在 A20OS 上被架构表直接否掉：

| 架构 | 根层可用软件位 | 结论 |
|---|---|---|
| riscv32 (Sv32) | 0（PPN 占 bit10–31） | 根层放不下任何 payload |
| arm32 | 0（8 位索引 + 30 位 PPN） | 同上；且 L0 有 4096 项，16 B/项的元数据超过页本身 |
| loongarch64 / la32r | 0（`PTE_R/W/X` 映射到 `LA_PTE_MAT1` 内存属性位） | 会破坏 MA |
| aarch64 | 55–58 已被 LEAF/W/X/COW 占用，且表描述符与叶子描述符位布局不同 | 非均匀 |
| riscv64 / x86_64 / ppc64le | 充裕 | 可行但不统一 |

参考实现的 `Status::MASK = ((1 << 39) - 1) / PAGE_SIZE` 隐含假设**整棵页表
树每一层都有统一的约 27 位 payload 预算**，A20OS 没有这个前提。此外现有的
`PTE_SWAP` 已经在逐架构窃取硬件有语义的位（riscv64 bit9、aarch64 = `PTE_LEAF`
bit55、x86_64 = `PTE_LEAF` bit10、ppc64le bit1、arm32 bit7、la64 = `PTE_LEAF`
bit11）——通用状态编码会把这一类损坏风险成倍放大。

### 2.2 描述符可达性：内联进 `frame_meta_t`

`pfa.meta` 已经是启动时分配、常驻直映、按 PPN 索引的连续数组，这正是论文的
描述符区域。描述符指针放进 `frame_meta_t` 中**仅在空闲链表上有效**的
`prev/next` 联合体，因此 `sizeof(frame_meta_t)` 仍是 16 字节——单级模型对帧表
本身零额外开销（已用 `_Static_assert` 钉住）。`pt_meta_t` 块按页表页按需分配，
随该页表页释放。

拒绝 slab 侧表：ADV 协议的无锁遍历需要在最热路径上取描述符（读 `stale`、
取 covering node 锁），一次索引加载胜过哈希探测。

### 2.3 协议选择：ADV，不做 RW

论文的 `CortenMMrw` 在遇到任何非 `PageTable` 子节点时 `break`，**要求页表
完全填充**。A20OS 的 `pt_walk()` 是惰性分配中间节点的，而完全填充的代价不可
接受：4 级 / 512 项架构上 1 GB 用户虚拟地址空间需要 512 个 L1 页 = 每个地址
空间 2 MB 纯脚手架，再乘以数百个进程；riscv32 / la32r 只有 2 级。
`CortenMMadv` 的 `try_traverse_and_lock_subtree_root` 处理的正是惰性情形
（锁住父节点、分配缺失的子节点、下降）。

MCS 锁按 `(cpu, depth)` 从静态池取 node，不在锁路径上分配。

### 2.4 一个必须现在设计进去的约束：共享内核半

`pt_map_kernel()` 把 `boot_pgdir[ARCH_PT_USER_END..]` 的 PTE 字复制进每个进程
根页表，所以**内核半的整棵子树在所有地址空间之间是同一批物理页**。写锁它会
让全系统串行；释放它是系统级 use-after-free。因此
`mm_addrspace_lock()` 在**代码层面**拒绝任何触及该半区的区间，而不是靠注释。
内核映射永远不经由 cursor 建立。

## 3. 论文未覆盖的特性的逐项结论

| 特性 | 结论 | 成本 / 风险 |
|---|---|---|
| **brk** | **残留区间结构**。`mm->brk` / `start_brk` 本来就是 `mm_struct_t` 标量而非 VMA。把 `fault.c` 里第三个子句 `!mm_find_vma(...)` 换成"该区间内没有已映射状态"——比现在**更精确**。 | 低。真实风险：brk 收缩必须同时清元数据与 PTE；brk 增长不得覆盖 mmap。 |
| **mseal** | **残留区间结构，非权威**。`mm_struct_t` 里的 `seal_range_t` 列表，在开启 cursor **之前**被查询。区间性就是它的语义。**不要假装它变成了逐页属性。** | 低成本，但你确实保留了一个区间结构。 |
| **madvise DONTNEED/FREE** | **完全逐页**。开 cursor 遍历区间，`unmap()` 掉每个 class 为 Private/Shared/VMO 的页，置 `INVALID`。seal 检查作为前置门。 | 低。 |
| **mlock 记账** | **残留区间**（今天就只是 `VM_LOCKED` / `locked_vm` 记账）。保留区间位，通过把它排除在 madvise/reclaim 之外来生效。P1 不加逐页 `Locked` 状态。 | 低。 |
| **THP** | **P1–P6 作为无元数据的旁路**；P9 再建模。保留 `pt_map_huge` / `mm_demote_huge_page`，实现参考实现的 `split_if_mapped_huge`（`locking.rs:149`）以便 covering node 撞上 PMD 叶子时降级。 | **功能风险最高**。`ARCH_NO_PMD_LEAF` 已挡掉 3 个架构。 |
| **NUMA 策略** | **免费，完全不动**。`mm/mempolicy.c` 是 *节点* 属性而非映射属性，从不查询 VMA 列表。 | 无。 |
| **/proc maps + smaps** | **残留——这是诚实性关键**。maps 本质是区间视图。要么在逐页状态上重新实现区间合并（等于把论文要删的合并逻辑再写一遍），要么保留副本。**永久保留 VMA 列表作为派生、非权威的区间提示，由同一批 mutator 维护，由调试期双向一致性检查器校验。** | **VMA 列表不会消失。承认这一点。** |
| **fork / 文件页 COW** | **逐页，而且模型在这里占优**。`pt_clone()` 变成"克隆页表 + 复制 cls/cow 数组 + DFS 标记所有用户叶子"。这**删掉**了 `mm_file_cache_mapping_get()`——它今天在**每一次私有文件叶子的 COW fault** 上做一次 `page_cache_get`（加锁 + 引用计数），只为回答"这是不是那个规范 cache 帧"。一个状态字节即可回答。 | **新失败面**：不能忘复制数组，不能重复引用计数。高风险。 |
| **VMO（原生 ABI）** | **逐页，需新增 class**。`mm_lookup_vmo_region()` 回答的是区间问题（"这整段是不是同一个 VMO"）→ 保留 VMA 作其权威。新增 `VMO` class 供拆除决策使用。 | 低。 |
| **swap** | **逐页，也是最干净的正确性收益**。`Status::SWAPPED` 消掉 `PTE_SWAP` 逐架构窃取硬件位，并消掉**六处先于 `PTE_V` 检查 `pte_is_swap()` 的顺序依赖**（`mm.c` 与 `fault.c` 中）。这些顺序是承重的，正是 cursor 重写最容易踩坏的东西。 | 代码量低、示范价值高。 |
| **fault-around** | **存活，且更便宜**。4 页 = 1 次 cursor 而非 4 次页表遍历。`handle_file_fault` 的"放锁 → I/O → 重锁 → 重校验"编排变成"无锁做 I/O，然后**从元数据**重校验（`FILE_*` class + 偏移匹配）而不是从 VMA"。 | 中。LoongArch64 自己的注释指出私有 cache 叶子在并行 rustc 下已经会破坏动态符号——迁移时**不要**"统一" la64/x86_64 的 `direct_private` 路径。 |

## 4. 分阶段计划

**过渡态的调度规则**：cursor 拥有映射真相；VMA 列表拥有区间策略、资源引用
（file_fd / vmo）与报告。这是下面各阶段能独立落地的原因。

| # | 交付物 | 结束时成立的不变式 | 验证门 |
|---|---|---|---|
| **P0** ✅ | `pt_meta_t` 并入 `frame_meta_t`；`mm_pt_meta()`；`FRAME_F_PT` 审计 | 零可观测变化；`sizeof(frame_meta_t) == 16` | 7 个架构/NOMMU 构建；`pfa_audit_lists()` |
| **P1** ✅ | 按页表页分配 `cls`/`cow`；`kernel/mm/mm.c` 每个页表写点同步元数据；**独立遍历页表与元数据并逐一比对的审计器**，挂在关机审计点 | *元数据 ≡ 页表状态，始终成立且机器可证* | `smoke-mm-stress`、`smoke-mm-fork-exec-race`、`smoke-abi-linux`、`check-mm-lock-model`；实测审计全 0 |
| **P2** | 让 fault 路径经由 cursor 写 PTE（仍在 `mm->lock` 之下） | 所有 PTE 写入只有一条代码路径 | 上述三个 smoke + `smoke-vfs-stress` |
| **P3** | covering node 之上 DFS 预序锁全部后代；逆序释放 | 每次 cursor 都按预序加锁、逆序解锁 | 新 `check-mm-pt-lock-order`（释放序断言 + 计数器）；`smoke-mm-stress` @ `NR_CPUS=4` |
| **P4** | `core/rcu.c`；`stale` + 重试；`pt_unmap` / `pt_unmap_leaf` / `pt_destroy_*` 中的 `frame_free(child)` 改为延迟释放 | 任何遍历都到不了的页表页不会被回收 | 新 `smoke-mm-pt-race`：N 线程大范围 unmap 同时 N 线程 fault |
| **P5** | **从 fault 路径摘掉 `mm->lock`**，它收缩到只保护 VMA 列表与计数 | 不相交区间的写者**实测**不串行 | `smoke-mm-pt-race`；新增"两线程不相交区间测临界区重叠"测试；`smoke-sched-stress`、`smoke-mm-stress` @ `NR_CPUS=4+` |
| **P6** | fault 分派改判 `Status` 而非 `mm_find_vma`；`handle_file_fault` 从元数据重校验 | page fault 分派读不到任何 VMA | 上述全部 |
| **P7** | `Status::SWAPPED`；删除 `PTE_SWAP` 与六处前置顺序检查 | 没有任何 PTE 位被重载 | `CONFIG_SWAP=y` 构建 + swap-in 测试 |
| **P8** | 双向一致性检查器（P1 审计器覆盖反方向）；`MM_LOCK_MODEL` → `MM_AS_MODEL`；**同一提交内**更新门禁与 `docs/testing-gates.md` | VMA 列表可证为纯派生 | `check-mm-lock-model`、`check-final-definition`、`check-doc-test-gates` |
| **P9** | *(不承诺)* mseal/mlock/brk 逐页化；THP 进 `Status`；删除残留区间结构 | — | — |

**P1 是整个计划里性价比最高的一步，且不可能搞坏启动**：它是影子状态、可机器
验证，而且会找出你不知道自己有的 bug。

## 5. 风险排序

1. **页表页或帧在有 cursor 处于其子树内、或存在可抵达它的陈旧 TLB 项时被释放。**
   这正是本项目历史上 `0x63636363` 与"进程退出 teardown 提前释放 VMO 帧"的
   形状。P4 未落地前，`pt_unmap` / `pt_unmap_leaf` 里的 `frame_free(child)` 与
   `pt_destroy_level` / `pt_destroy_user_recursive` 的递归释放**都无锁、无
   TLB flush**。`mm_demote_huge_page` 先 `pt_unmap_leaf` 再 `frame_put`，中间
   只有 `mm_tlb_hold_frame` 引用而非真正的 shootdown。
2. **锁或释放共享内核半子树** → 全系统死锁或系统级 UAF。已在代码层面拒绝，但
   P3 的 DFS 锁必须继承这条约束。
3. **P5 收窄锁范围引入 TLB-IPI 死锁。** `vm.h` 的 `MM_LOCK_MODEL` 精确记录了
   这条约束：远端 CPU 带中断关闭地自旋在 `mm->lock` 上，必须能退出该临界区
   去响应 TLB IPI。一旦 fault 路径不再持有 `mm->lock`，"带 IRQ 关闭持有页表页
   锁"就成了同一类死锁。**cursor 必须在派发远端 shootdown 之前释放所有锁**，
   这正是 `mm_tlb_invalidate_finish` 现有形状。
4. **文件映射 / COW 路径。** `handle_cow_fault_locked` 有三条按
   `mm_file_cache_mapping_get` + `refcount` 区分的所有权规则；源码里那两段注释
   是 `0x63636363` 事故的疤，编码了**特定顺序**（在 `pfa.lock` 下先更新 PTE、
   再 `frame_put`）。把 page-cache 查找换成一次元数据读取会改变引用计数读取
   相对 PTE 写入的**时序**。这些顺序必须逐字保留。
5. **swap 的 PTE 检查顺序**（六处）。若 cursor 的 `query()` 只报"是否 present"，
   swap-in 会被静默当成"未映射"。P7 的 `Status::SWAPPED` 是强制项。
6. **元数据与 PTE 静默分叉**（P1 漏掉某个写点）。审计器是唯一防线，且必须在
   `smoke-mm-stress` 构建里启用，而不是只在一个 debug 变体里。
7. **loongarch32 的软件 TLB refill** 从异常处理器**异步无锁读 PTE**——这是任何
   cursor 协议都保护不到的读者。在 refill 被纳入 RCU 读侧之前，**把 la32r 排除
   在迁移范围之外**。
8. **在 PTE 写入时把元数据标错类型**（只读别名 / NX 冲突）。注意 aarch64 上
   `PTE_D == PTE_W`（bit 56），x86_64/loongarch64/arm32/la64 上
   **`PTE_SWAP == PTE_LEAF`**——"叶子"标记就是"swap"标记。任何从零重建 flags
   而不是保留旧字的地方都会破坏这个区分。

## 6. 明确不承诺（不要声称）

1. **"VMA 抽象已经没了。"** 没有。它永久降级为非权威区间索引，服务于
   `/proc/{maps,smaps}` 与区间策略（brk、mseal、mlock）。声称已删除是最容易被
   抓到的谎言。
2. **"不相交区间并行执行。"** 在 **P5 落地并有实测之前**都不成立。
3. **"大页已进入模型。"** P1–P6 期间 THP 是无元数据旁路。
4. **"mseal 已成为逐页属性。"** 它仍是区间结构。
5. **"arm32 与 armv7m-NOMMU 参与其中。"** `kernel/arch/arm32/mm/pgtbl.c` 有自己
   的页表核心且 L0 有 4096 项（元数据开销超过页本身）；armv7m 是 `CONFIG_NOMMU`。
   两者都留在同一套 `mm_*_locked` API 背后的旧路径上。*接口*是统一的，*引擎*
   按架构组划分——这正是 `mm.c` 现有的 `#if ARCH_HAS_PGTABLE_OPS` 形态。
   目标是 5 个 512 项的 64 位架构。
6. **"loongarch32 参与其中。"** 软件 TLB refill 是无保护的异步读者。
7. **"swap 已迁移。"** 除非 P7 落地**且** CI 里有 `CONFIG_SWAP=y` 构建。
8. **"`/proc` maps 由 `Status` 派生。"** 它由 VMA 副本派生，并对照 `Status` 校验。

**P1–P6 落地后可以诚实声称的**（在 5 个 512 项 64 位架构上）：单一权威的映射
表示；它与旧 VMA 路径之间的机器可证等价；所有 PTE 变更经由一条 cursor 代码
路径；所有写者互斥落在页表页锁上且释放顺序可证；任何页表页都不会在 grace
period 内被回收；file/COW/swap/fault-around 的分派由逐页状态而非区间查询驱动。
这是一个真实的结果，但它不等于"CortenMM 已移植"，两者相差约六周的工作量。

## 7. 门禁

`check-mm-lock-model` 依赖 `smoke-mm-stress` + `smoke-mm-fork-exec-race`，
这两个是全部 9 个阶段的回归地板。当前它还额外 grep
`MM_AS_CURSOR_ONLY_ENTRY`、`mm_pt_note_present|mm_pt_note_absent`、
`mm_pt_audit_all`，以便门禁真正强制新模型而不是随实现一起腐化。

关机时的 `MM-ASM` 审计行是事实记录而非断言：`pt_pages / entries / missing_meta
/ present / absent / prot / cow / vma` 全部为 0 表示两个表示在整个工作负载上一致。

**注意**：`smoke-mm-stress` 之类的门通过 grep `MM_STRESS: PASS` 判定，而该标记
在关机审计之前打印，所以关机路径上的 panic 不会让门失败。审计行必须与 PASS
标记一起检查。

---

## 8. 实施记录（2026-09-28）与 P5 的实测结论

本节记录 P0–P4 的实际落地结果，以及一次把 P5 的前提测了出来的重要实验。
所有数字均为 smp4 QEMU 实测。

### 8.1 已完成并验证

| 阶段 | 状态 | 证据 |
|---|---|---|
| P0 描述符 | ✅ | `pt_meta_t` 并入 `frame_meta_t` 联合体，16 字节零开销（`_Static_assert` 钉住） |
| P1 per-PTE 状态 | ✅ | 关机审计：7 架构构建 OK，`missing_meta=0 present=0 absent=0 prot=0 cow=0 vma=0` |
| P2 fault 走 cursor | ✅ | 缓存下降路径；6 个构建 + `mm_stress`(smp1/smp4) + `mm-fork-exec-race`(smp8) + `smoke-abi-linux` + `check-mm-lock-model` 全 PASS |
| P3 页表页锁为互斥单元 | ✅ | covering-node MCS；修掉 depth 双减自死锁 |
| P4 延迟回收机制 | ✅ | 读侧计数 + `tlb_holds`  graveyard + stale；已就位待 P5 接线 |
| 度量基准 | ✅ | `user/cmds/stress/mm_pt_scale.c`，把并行主张变成可证伪验收门 |

期间修掉两个真实缺陷：fault-around 曾把 PFN 当物理地址传给 `mm_cursor_map`
（装入无效物理地址，表现为访问故障而非缺页，C 无法发现类型不匹配）；MCS
回退循环与 `mcs_unlock` 双减 per-CPU depth 导致同节点自死锁（LOCK-STALL
报 `waiter==owner` 同址）。

### 8.2 P5 实测：并行性的有效基线（2026-09-28 修正）

> **本节此前的一版结论是错的，已作废并重写。** 原版在 riscv64/TCG 下测得
> speedup=1.03x，据此断言"fault 被 mm->lock 串行化"。该测量无法区分**内核串行**
> 与**环境串行**，而 riscv64 guest 跑在 x86_64 host 上只有 TCG、跨架构无可用的
> 多线程 TCG，vCPU 本身即被串行化。

新增环境探针 `user/cmds/stress/cpu_scale.c`：纯整数计算、不碰内存、不碰任何
内核锁。它给出该环境能否呈现并行性的上界：

| 配置 | cpu_scale（纯计算上限） | mm_pt_scale（真实 demand fault） |
|---|---|---|
| riscv64 / TCG / smp4 | **0.89x – 0.96x** | 1.03x（无效，无参考价值） |
| x86_64 / **KVM** / smp4 | **3.93x** | **1.48x – 1.53x** |

结论（以 KVM 为准）：

* 环境本身能线性并行（3.93x），所以 x86_64/KVM 是唯一有效的并行性度量环境。
* 真实 demand fault 工作负载只拿到 1.48x，相对环境上限损失约 2.4x —— 说明
  fault 路径**确实**存在实质串行化，P5 值得做。
* 但"串行化来自 mm->lock"这一归因**仍未被单独证实**：该工作负载每轮还要 mmap
  新区域（`mmap` 本身要取 `mm->lock` 做区间分配与 VMA 插入），且每页 fault 都要
  `pfa_alloc_page()` 走 buddy 的全局锁、以及整页 `memset`。在把这三项成本分离
  之前，不能断言瓶颈就是 `mm->lock`。

**因此本节修正后的结论是**：单级模型的并行优势在 A20OS 上**尚未兑现**
（1.48x vs 环境上限 3.93x），P5 方向成立且值得继续。

### 8.2b 成本归因：并行度到底损失在哪（mm_fault_cost，x86_64/KVM smp4）

新增 `user/cmds/stress/mm_fault_cost.c`：同一线程数、同一共享地址空间、同一不相交
区间，把三���成本分开单独测。

| 相位 | 隔离的是什么 | 1T | 2T | scale |
|---|---|---|---|---|
| `mmap` | 只 mmap+munmap，从不触碰页 → 地址空间锁 | 0.168s | 0.275s | **1.22x** |
| `fault` | 一次 mmap 后逐页首触 → demand fault 路径 | 0.091s | 0.126s | **1.44x** |
| `mem` | 预触后反复写 → 内存路径上限 | 0.047s | 0.052s | **1.78x** |

**归因（首次被独立证实，而非由端到端数字推断）**：

* 内存系统**不是**瓶颈（1.78x 接近 2x 理想值）→ P5 有真实空间。
* 串行主因确为 `mm->lock`，且 **`mmap`/`munmap` 比 fault 更糟**（1.22x vs 1.44x）：
  区间分配与 VMA 插入整段都在 `mm->lock` 内。这把"P5 = 只改 fault 路径"的
  范围估计缩小了——`mmap`/`munmap` 同样是必须处理的串行源。
* 因此后续 P5 的收益上限约为 1.44x → 1.78x（fault 侧）与 1.22x → 1.78x
  （mmap 侧），而非"数量级"提升。

**度量陷阱记录**：`mem` 相位的迭代数必须远大于预触页数，否则测到的仍是 fault
成本。初版即栽在这里——预触 1024 页 ≈ 10ms 与写循环同量级，测出 1.13x 的假象；
把 `MEM_ROUNDS` 提到 4096 后才得到可信的 1.78x。

### 8.3 P5 的真正前提：fault 必须能从 per-PTE 状态解析（P6）

要让 fault 不碰 VMA，`mmap` 必须把"这段是私有匿名、按需分页"的意图写进
per-PTE 状态。但这引出一个结构性问题：per-PTE 状态挂在**页表页的**元数据
上，而 on-demand 区域此刻尚无叶子页表页。若按论文 `mark` 语义在 mmap 时就
为整段分配叶子页表页，则 1 GB 映射要预分配 2 M 个叶子页表页（2 GB 页表内存
才能表达"尚未 fault"）——这是 A20OS 现有 on-demand 设计刻意避免的成本。
论文靠大页状态规避，A20OS 的大页尚未建模（P9）。

因此 P5 存在一个必须显式决策的架构分叉（**不属于可以顺手做掉的量级**）：
- **(a) 大地址空间按需标记**：只在已有叶子页表页的页上记状态，其余仍回落 VMA。
  内存友好，但并行收益不完整。
- **(b) 论文式 eager 叶子页表分配**：完整兑现并行语义，接受大映射的页表内存
  开销（可用大页状态缓解）。
- **(c) 给 VMA 加快照/引用计数**：fault 在锁内取引用、锁外用引用解析，mm->lock
  只在"取引用"这一瞬被持有。这是 Linux 等成熟内核的通用做法，改动面集中在
  VMA 生命周期，风险中等。

本轮刻意**未**替用户选定 (a)/(b)/(c) 并强行实施——它牵涉内存/兼容性权衡，
且必须与"P4 延迟释放接进拆链路径"同一次提交（P5 摘锁正是 UAF 变真实的时刻）。
当前交付停在"机制齐备 + 前提测清 + 验收门就位"，是诚实且可回退的中间态。

### 8.4 P5 接线时的硬性要求（留给下一步）

1. 摘 `mm->lock` 与"把 P4 延迟释放接进 `pt_unmap`/`pt_unmap_leaf`/`pt_destroy_*`"
   必须**同一次提交**：摘锁正是"并发 cursor 缓存指针 + 拆链立即释放"这一 UAF
   变真实的时刻。
2. 远端 TLB shootdown 必须在释放所有页表锁**之后**派发（`vm.h` 的
   `MM_LOCK_MODEL` 明确记录：带 IRQ 关闭自旋在锁上的 CPU 必须能退出临界区去
   响应 TLB IPI）。
3. COW 路径的 `0x63636363` 顺序（fault.c 内注释）必须逐字保留，其
   `rc==1` 分支已在 `pfa.lock` 下运行，合并式 map 会自锁。
4. 每次提交后跑：7 架构构建 + `mm_stress`(smp1/smp4) + `mm-fork-exec-race`(smp8)
   + `smoke-abi-linux` + `check-mm-lock-model` + 关机审计全 0。
5. 提示：QEMU smoke 门会因遗留 qemu 占住 host 5555 端口而假失败，可用
   `make ARCH=riscv64 NET_HOSTFWD=hostfwd=tcp::6099-:6099,... <target>` 换端口。

### 8.5 与本改造无关的既有缺陷（勿误判为回归）

在 x86_64 目标上验证时发现 `syscall_smoke` 失败：
`SYSCALL_SMOKE: FAIL renameat errno=22`（EINVAL）。

**已在未修改的 `main`（31055cd9）上复现同样失败**，故为 A20OS 既有的 x86_64
缺陷，与单级内存模型改造无关。同一目标上 `mm_stress` 通过、关机审计全 0。

另有既有问题（非本轮引入）：

* `riscv32` 在 `main` 上即构建失败（`kernel/ipc/kexec.c` 指针转换），因此
  `smoke-arch-mmu-matrix` 覆盖不到 riscv32。
* `arm32` 缺 `arm-linux-gnueabihf` 工具链，本机无法构建。
* smoke 门以 grep `*_PASS` 标记判定，而该标记在关机审计之前打印，因此关机
  路径上的 panic 不会让门失败。审计行必须与 PASS 标记一并检查。
* QEMU smoke 门会因遗留 qemu 占用 host 5555 而假失败；可用
  `make ARCH=... NET_HOSTFWD=hostfwd=tcp::6099-:6099,...` 换端口。

### 8.6 验收环境的硬性要求

**并行性度量只以 x86_64/KVM 为准。** riscv64/TCG 只能用于功能正确性：跨架构
TCG 无可用多线程 TCG，vCPU 本身即被串行化（`cpu_scale` 实测 0.89x–0.96x），
任何内存管理改动都不可能在该环境下显示加速。x86_64/KVM 下 `cpu_scale` 为
3.93x，是有效的度量环境。

### 8.7 P5 并行快路径：四次尝试、四种失败（已全部回退）

P5 的目标是让 fault 路径不持 `mm->lock`。本轮实际尝试了"锁内快照 VMA 字段、
锁外用 cursor 装 PTE、装完再回锁复核"的透明快路径，四次尝试各自撞上不同的深层
问题，全部回退。记录在此，因为每一种失败都指向下一步必须解决的具体事项。

| 尝试 | 现象 | 根因 |
|---|---|---|
| 1 | `signal=11` @ `stval=0x6a170` | fault-around 把 **PFN 当物理地址**传给 `mm_cursor_map`，装入无效物理地址，表现为访问故障而非缺页 |
| 2 | 复核失败分支二次 `frame_put` | `mm_cursor_unmap()` 已释放被替换帧，再 put 即**双重释放** |
| 3 | `[MCS DEADLOCK] already_holding=1` | P3 修双减时把 `depth--` 从 `mcs_unlock` 删掉，而下降循环的内联 lock/unlock **依赖它**，导致每分配一次中间节点泄漏一个深度槽 |
| 4 | panic in `x86_64_smp_remote_tlb_flush` | 摘锁后持页表锁的 CPU 妨碍远端 TLB IPU 完成——正是 Oracle 风险 #3 |

第 3 项是真实缺陷且已修复并提交（`91f3eafc`）；第 1、2、4 项随快路径一并回退。

**第 4 项是关键教训**，也是下一步的硬性前提：Oracle 风险 #3 要求
"cursor 必须在派发远端 shootdown **之前**释放所有页表锁"，而当前
`mm_tlb_invalidate_begin/finish` 的事务边界是围绕 `mm->lock` 建的
（`vm.h` 的 `MM_LOCK_MODEL` 明确记录：带 IRQ 关闭自旋在 `mm->lock` 上的远端
CPU 必须能退出临界区去响应 TLB IPI）。摘掉 `mm->lock` 就必须**同时**重建这条
边界，否则会制造一类新的 TLB-IPI 死锁。

结论：P5 不是一次"把锁挪走"的改动，而是必须与 P4 延迟释放接线、TLB 事务边界
重建、以及 VMA 生命周期方案一起设计并整体验证。单独做局部版本会连续踩到上述四
类问题中的三类。

### 8.8 P0–P4 的性能账：目前**没有可测量的性能变化**

在 x86_64/KVM smp4 上，对 `main`（移植前）与 `cortenmm`（P0–P4 完成后）各取
**4 次** `mm_fault_cost` 采样取均值：

| 相位 | main 均值 | port 均值 | 差异 | main 极差 | port 极差 |
|---|---|---|---|---|---|
| `mmap`  | 1.098x | 1.118x | +1.8% | 0.090 | 0.160 |
| `fault` | 1.413x | 1.405x | −0.5% | 0.100 | 0.090 |
| `mem`    | 1.633x | 1.650x | +1.1% | 0.040 | **0.280** |

**噪声地板**：���相 `mem` 不产生缺页、也不做 mmap churn，我的改动**在原理上不可能
影响它**，而它在**同一个 port 二进制**上的极差达 0.28x（1.50x–1.78x）。以此为
噪声地板，三项差异（+1.8% / −0.5% / +1.1%）**全部落在噪声内**。

结论（如实记录）：

* **P0–P4 目前不带来可测量的性能变化。** 早前单次采样看到的
  `mmap=1.22x / fault=1.44x / mem=1.78x` 是噪声分布的上沿，不是改进。
* 这与预期一致：单级模型带来的并行度只有在 `mm->lock` 不再串行化 fault/mmap
  之后才兑现，而那正是 P5/P6（见 §8.7 与 §8.3）。
* P0–P4 的实际交付是**结构与正确性**，不是性能：per-PTE 状态成为映射状态的权威
  表示、页表页锁成为写者互斥单元、延迟回收机制就位、per-frame 数组零额外开销
  （16 字节，`_Static_assert` 钉住）、以及关机审计证明两种表示在全工作负载上一致
  （审计全 0）。这些是 P5 得以可能的前提。

**方法论教训（比结论更重要）**：单次采样在这个环境里毫无意义——噪声地板高达
±9%，且噪声最大的恰是"本应不受影响"的对照相位。今后任何性能主张都必须在
**x86_64/KVM** 上、对照与实验各取**多次采样**、并**同时报告一个改动无法影响的
对照相位**来确立噪声地板，否则结论不可信。
