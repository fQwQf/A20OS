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

### 8.9 VMA 引用计数已落地（`b10b267b`）

P6 的前提是 fault 能在无 `mm->lock` 下读取 VMA 字段。锁一放开，`vm_area_t`
指针本身就不再受保护——`mm_find_vma()` 返回后 `munmap` 可以摘链并释放。因此先
落地 VMA 生命周期机制（**这是 P6 的前提，本身不是 P6**）。

所有权规则：地址空间链表持有分配时创建的那一份引用；摘链即释放它；最后一个持有
者——可能就是一把锁都没有的 fault——负责安排释放。延迟释放列表换用独立的
`vma_ref_lock` 而非继续借用 `mm->lock`：否则一个刚逃离 `mm->lock` 的 fault 要
放下引用就必须重新获取它刚释放的锁，fast path 白做。该锁只保护链表 push/pop，
绝不跨 `vma_release()`（后者经 `vfs_close` 触发回写，可睡眠）。另加
`deferred_next`，避免与 `mm->mmap` 的 `next` 混用。

fork 有两处状态继承必须重置，否则子进程会用着带父锁状态的锁、或与父 VMA 共享
同一份引用计数：`*child = *parent` 复制了 `vma_ref_lock` 状态；克隆 VMA 的
`*cv = *pv` 继承了父 VMA 的引用计数。

不变式：**任何进入过 `mm->mmap` 的 VMA 都不再被直接 `kfree`**，只经 `vma_put`
摘链。剩余的直接 `kfree` 全部位于"发布前失败回滚"路径（引用计数为 1、从未对其他
线程可见），包括 `elf_discard_vmas` 的半成品镜像回滚。10 个分配点全部初始化引用
计数（4 个结构体复制点必须重置为自己的 1，而非继承源 VMA 的值）。

验证：riscv64/aarch64/x86_64/loongarch64/ppc64le + riscv64/aarch64 NOMMU 全部
`-Werror` 通过；`smoke-mm-stress`、`smoke-mm-fork-exec-race`、`check-mm-lock-model`
通过；x86_64/KVM smp4 跑 `mm_stress` 全绿（含 `vma-deferred-race` 子项），关机
审计 `pt_pages=10 entries=2560 missing_meta=0` 且 present/absent/prot/cow/vma 全 0，
无 panic、无看门狗中止。

### 8.10 更正：§8.9 之后对 P5 死锁机制的错误定位（已推翻）

本节先前一版把 §8.7 第 4 项的死锁定位为「MCS 页表锁在持有 IRQ 关闭的
`mm->lock` 时取得，等待者继承中断关闭状态、无法接收 TLB IPI」。**该定位是错的**，
现予更正，以免后续工作建立在错误根因上。

错误原因：把 `spin_lock(&mm->lock)` 当成了关中断自旋锁。实际实现里
`spin_lock` → `spin_lock_at`（`core/lock.h`）**不碰中断状态**——函数体内唯一的
`arch_irqs_enabled()` 出现在死锁诊断的 backtrace 打印里。只有
`spin_lock_irqsave` 才调用 `arch_local_irq_disable()`。因此 fault 路径
（`spin_lock(&mm->lock)`）持锁期间中断仍然是开的，**能够**接收并响应 TLB IPI，
不构成 IPI 投递阻塞。

已在当前分支核实的两条事实：

* **shootdown 已经在 `mm->lock` 之外发起。** 例如 `mprotect.c:154-158` 与
  `fault.c` 的 COW 路径都是 `mm_tlb_invalidate_begin` → `spin_lock_irqsave` →
  改写 → `spin_unlock_irqrestore` → `mm_tlb_invalidate_finish`，即派发 IPI 时
  `mm->lock` 已释放。所以「持 `mm->lock` 派发 shootdown」这一环在本分支不存在。
* **MCS 页表锁确实存在**（`kernel/mm/pt.c`，约 31KB，含 `mcs_lock`/`mcs_unlock`/
  cursor），且确实在 fault 路径持有 `mm->lock` 期间取得——但如上，持有者中断是
  开的，仍可响应 IPI，故不构成 §8.9 所述的阻塞环。

**残留的真实风险**（与 §8.7 第 4 项那次 panic 仍可能相关，但机制不同）：任何
`spin_lock_irqsave(&mm->lock)` 的写者临界区（`mmap`/`munmap`/`mprotect` 等）都
关中断。若 TLB IPI 恰好落在该窗口，目标 CPU 无法 ack，发起方 5 秒后 panic。
在 KVM 下该窗口可被宿主调度任意拉长，看起来就像 hang——这与已修复的 MCS 看门狗
「幽灵死锁」属同一类（那次修复只覆盖 MCS 自旋，不覆盖 `spin_lock_irqsave` 窗口）。

**对 §8.7 结论的修正**：「必须同时重建 TLB 事务边界」这一条**不再成立为硬前提**。
先前把它列为硬前提，是因为误以为 fault 路径持锁期间关中断。实际不关中断，
IPI 投递不被阻塞，因此 P5 摘 `mm->lock` 并不会**因此**制造新的 TLB-IPI 死锁。
P5 仍须满足的硬性要求收敛为：装 PTE 的互斥单元（cursor/MCS 锁）不得跨越
`mm_tlb_invalidate_finish`／`mm_tlb_shootdown_page` 的派发与等待。

§8.7 第 4 项那次 panic 的确切根因仍未定案——需要当时的 diff 才能确认，不能靠重构
推理断定。如实记录为未决。

### 8.11 异步 generation 方案在 x86_64 上不可用（已核实）

评估「不阻塞等 ack、只递增 generation 让各 CPU 惰性 flush」这一备选方案时，
核实到一条决定性事实：

`ARCH_MM_CONTEXT_ALLOC` **只有 riscv64 覆盖**（`arch/riscv64/include/arch.h:16`
→ `riscv64_asid_alloc()`），其余架构走 `core/arch.h:82` 的默认 `0U`。已确认
`kernel/arch/x86_64/` 下没有任何地方把 `arch_asid` 置为非零，故 x86_64 上恒为 0。

而 `mm_context_enter` 里的惰性 generation 校正循环被 `if (mm->arch_asid && ...)`
整个门控。**x86_64 上该循环从不执行**：`tlb_cpu_generation[cpu]` 只在发起方完成
同步 shootdown 之后被写入（`vm.c` 内 flush 收尾处），从不用于修复。

后果：x86_64 上没有惰性兜底。若把 shootdown 改成不阻塞，发起方写入
`tlb_cpu_generation[cpu] = generation` 就是一句**假话**（记录某 CPU 已丢弃某代，
而它并未 flush），且没有读者去纠正，陈旧翻译**永久化**——`munmap` 后同址
`re-mmap` 会把旧映射的数据交给新映射；更糟的是延迟回收会在 shootdown 尚未真正
完成时释放数据帧，而远端 CPU 仍可能经由该帧翻译。

因此：**x86_64 必须保留同步阻塞 shootdown**，异步 generation 方案仅在有 ASID 的
riscv64 上才谈得上安全。基准与死锁复现都在 x86_64，故该方案出局。

### 8.12 P5 第五次尝试：把 prepare 移出锁，**已回退**（附结构性教训）

在 §8.10 更正根因之后重做 P5，采用「锁外 prepare、锁内 commit 并复核」：
匿名写 fault-around 路径在锁外做 4 次 `pfa_alloc_page` + 4 次 `cg_mem_charge` +
4 次 4KiB `memset`（16KiB 清零，占单次 fault 成本的大头），再取回 `mm->lock`
复核后用 cursor 一次事务装入。这正是 VMA 引用计数（§8.9）所解锁的用法。

实现过程中自查出并修掉两个真实缺陷（均由本轮引入）：

1. **UAF**：在 `vma_put()` 之后返回 `-EAGAIN`，调用方会继续 fall through 并在
   **没有引用**的情况下解引用 `vma`。exec 期间 VMA 频繁拆建，命中该窗口即损坏。
   改为：取引用之后的任何结局都必须是确定值（0 / -1 / -ENOMEM），`-EAGAIN` 只允许
   在 `vma_get()` 之前返回。
2. **抢占 THP**：快路径插在 `handle_demand_fault_access` 顶部，早于
   `handle_demand_fault_locked` 里的 THP 判定，于是 4KiB 单页会抢在 2MiB
   透明大页之前。改为在守卫里排除 `VM_HUGEPAGE`。

修掉这两个之后**仍然失败**：`smoke-mm-stress` 在 `execve mksh` 之后静默卡死
（无 panic、无自旋锁诊断、无看门狗）。插桩显示快路径本身被进入 4 次且每次都以
`mapped=1` 正常返回，卡死发生在其**之后**。回退 `fault.c` 后同一门立即 PASS，
故为本轮改动所致，非既有 flake（判定方式：stash/unstash 对照）。

**结构性教训（比这个 bug 本身更重要）**：`handle_demand_fault_locked` 是一条
**有序**的判定链——swap PTE → 栈增长 → brk → VMA 命中 → 权限 → VM_FILE →
VM_VMO → THP → anon batch → 单页 anon。把快路径**前置**到这条链之前，等于改变
了这些判定的优先级；每排除掉一个（VM_STACK、VM_HUGEPAGE……）就暴露下一个尚未
建模的交互。这与 §8.7 四次失败是同一类问题的第五次化身：**在一条既有判定链上
"提前插队"比"就地改造"危险得多。**

若继续 P5，正确的做法是**就地**改造：把锁外 prepare 放进
`handle_demand_fault_locked` 内、**原 anon batch 所在的精确位置**，从而完全保留
其前面的全部判定顺序，只改变"工作发生在锁内还是锁外"；这需要先把该函数按
判定分段、逐段调整锁的粒度，而不是在前置位置加一个早返回。**本轮未完成该改造。**

### 8.13 P4 接线状态核实：`mm_pt_defer_free` 是死代码

核实结果（此前文档表述不够精确）：

* **叶数据帧的延迟释放已接线**：`mm_tlb_hold_frame` 在 `munmap.c:105/236`、
  `madvise.c:81`、`oom.c:135`、`vm.c:402` 均有真实调用。
* **页表帧的延迟释放未接线**：`mm_pt_defer_free`（`pt.c:797`）已实现，内部正确
  调用 `mm_pt_hold_table` 并置 `stale`，但**全树没有任何调用者**——是死代码。

即：当前只有数据帧受 P4 保护，页表帧仍是同步释放。这在**现有**设计下是安全的
（`mm->lock` 事务 + 同步 shootdown 完成后才释放），但它是 P5 摘锁的硬前提。
接线它本身不产生性能收益，却会在现有锁模型下引入页表帧生命周期风险，因此
**刻意推迟到与 P5 同批落地**，而不是单独提交。

### 8.14 验收环境发现的两个无关既有缺陷

* **lwIP IPv6 接收路径断言崩溃**：`pbuf_free: p->ref > 0`，backtrace 为
  `ip6_input → ethernet_input → a20_lwip_process_netif_rx_tx_locked`，发生在
  启动阶段、`MM_PT_SCALE` 尚未运行。x86_64/KVM 上 4/5 次运行命中，**去掉
  `-netdev` 后 0 次**——由 virtio-net 设备触发。与本次改造无因果关系
  （无任何 VMA 路径通往 lwIP pbuf 引用计数），但会污染后续 KVM 采样：
  **mm_* 类基准应在无 NIC 的配置下运行。**
* **`mm_pt_scale` 自报计数与内核实时值不一致**：测试自身读到的
  `pt_lock_acquires=0 / cursors=3`，而同一次运行中 `cat /proc/a20/perf` 的实时值
  为 `mm_pt_lock_acquires: 23 / mm_cursor_open: 11`。计数器本身工作正常
  （名字在 `core/perf.c` 注册无误、无编译期开关），故差异出在测试的读取时机或
  解析上，**其计数器输出目前不可作为归因依据**；应以 `/proc/a20/perf` 实时值为准。

### 8.15 当前基线（无 NIC，x86_64/KVM smp4）

`mm_pt_scale` 多次采样 `ideal_speedup` 落在 **1.06x–1.20x**，低于其自带的
1.80x 判定阈值（FAIL）。这与 §8.8 的结论一致：`mm->lock` 仍在 fault 路径上
串行化 16KiB 清零，并行度尚未兑现。**这是 P5 尚未完成的量化证据。**

### 8.16 P5 第六次尝试：**就地**改造同样失败（推翻 §8.12 的建议）

§8.12 判定「前置插队」是第五次失败的根因，并建议改为**就地**改造：把锁外 prepare
放进 `handle_demand_fault_locked` 内、原 anon batch 所在的精确位置，从而完整保留
前面 swap→栈→brk→VMA→权限→file→VMO→THP 的判定顺序，只改变「工作在锁内还是
锁外」。**该建议已实施并同样失败**，故在此更正。

实施要点：给 `handle_demand_fault_locked` 增加 `lock_held` 参数，因为三个调用点的
锁状态不同——NOMMU 路径（不持锁）、swap 重试路径（已刻意放锁）、主路径（持锁）。
锁外 prepare 后重新取锁，按三种结局复核：叶已有效（他人装成，返回 0）、VMA 已被
摘链（返回 -1 重试）、逐页复核仍是缺页（丢弃被他人抢先的页后再装）。

结果：`smoke-mm-stress` 与 `smoke-mm-fork-exec-race` **双双失败**，且失败签名与第五次
**完全一致**：日志 222 行、静默停在 `[init] fork=0, calling execve mksh`，无 panic、
无断言、无自旋锁诊断、无看门狗。已回退 `fault.c`，HEAD 恢复为已验证状态。

**已排除的假设**（避免重复劳动）：

* *判定链被打乱*：就地改造已排除该假设，失败依旧。
* *THP 被抢占*：守卫已排除 `VM_HUGEPAGE`。
* *`-EAGAIN` 后的 UAF*：已改为取引用后只返回确定值。
* *栈增长路径被绕过*：`VM_STACK` 确由 `elf.c:578/599` 设置，故 VMA 命中即可排除。
* *`pfa.lock` → `mm->lock` 锁序反转*：`frame.c` 中 `pfa.lock` 在调用
  `oom_try_reclaim()` **之前**已释放（`spin_unlock` 早于 reclaim），不存在该持锁序。

**尚未排除、最值得下一步追查的线索**：第五、六两次的**唯一共同点**是把
`pfa_alloc_page()`（及其 `cg_mem_charge` / `memset`）移出 `mm->lock`。签名是
**静默卡死而非 panic**，符合自旋锁互等而非断言失败。尚未验证的具体机制：
`pfa_alloc_page` 的慢路径（`oom_try_reclaim` → `oom.c:100/146` 的
`spin_lock_irqsave(&mm->lock)`）在**另一个**正在 fault 的 CPU 上与本进程的
`mm->lock` 发生互等。下一步应先用插桩确认卡死时各 CPU 的持锁栈（而非继续猜测），
再决定 P5 的形态。

结论：**P5 仍未完成**，当前内核保持「正确但 fault 串行」的状态。以六次失败的
共同点为线索、用持锁栈插定位，是比继续改 fault 代码更可靠的下一步。

### 8.17 P5 根因：VMA 引用计数初始化遗漏（**六次失败的真实原因**）

§8.16 建议「插桩定位卡死时各 CPU 的持锁栈」。实际没有走到持锁栈——因为**根本不是
锁死**。给 `mm_find_vma` 的链表回退路径加步数上界后立即定位：

```
[VMAWALK] CYCLE mm=0xffffffc0bf1d0010 addr=0x6e69746c69756000 steps=100001
mm_find_vma: VMA list cycle
```

`addr=0x6e69746c69756000` 是 ASCII 文本而非合法地址——遍历进的是被释放后
reused 的内存。**VMA 链表成环**。

根因是 `b10b267b` 的一个真实缺陷：当时用 `grep sizeof(vm_area_t)` 找分配点，
漏掉了 4 处用 `sizeof(*vma)` 写法的站点——`munmap.c:262`、`udriver.c:183`、
`framebuffer.c:181` 与 `:363`。这些 VMA 的引用计数停留在 `kcalloc` 的 0。

该缺陷在 `b10b267b` 中**潜伏**：没有代码调用 `vma_get/vma_put`，计数为 0 只会让
`munmap` 少释放一次（泄漏），不会出错。P5 让 fault 路径真的
`vma_get` → `vma_put` 后，计数走 0→1→0，VMA 在**仍挂在 `mm->mmap` 上**时被释放，
链表指针随即悬垂。

触发点 `munmap.c:262` 建的正是 `VM_ANON | VM_READ | VM_WRITE` 的 brk VMA，恰好
落在 P5 快路径匹配的类别里；`execve` 会同时走「拆旧镜像 + 建 brk VMA + 首次
写入」，所以每次都稳定命中。

**这解释了第五、六次以及此前所有失败的共同症状。** 前面记录的那些根因
（判定链被打乱、THP 抢占、`-EAGAIN` 后的 UAF、栈增长绕过、锁序反转）**全部不是**
本次卡死的原因；即使它们都成立，也不足以致命。教训：

* **按类型名 grep 分配点不可靠**，必须同时覆盖 `sizeof(*vma)`、`sizeof (struct …)`
  等等价写法。已复核全部 14 处站点均已初始化。
* 一个「只在启用新调用者后才致命」的初始化遗漏，可以伪装成并发/锁问题并骗过
  多次尝试。**加断言/上界把静默死循环变成指名报错**，其价值高于其运行开销。
  `mm_find_vma` 的步数上界已作为失效保护保留。

### 8.18 P5 已实现且正确，但**性能假设被证伪**

修掉初始化遗漏后，`smoke-mm-stress` / `smoke-mm-fork-exec-race` /
`check-mm-lock-model` 全绿，关机审计全 0。**但 P5 没有带来任何可测量收益。**

x86_64/KVM smp4、无 NIC（规避 §8.14 的 lwIP 崩溃）、3 次采样：

| | 改动前基线 | P5 之后 |
|---|---|---|
| `ideal_speedup` | 1.06x–1.20x | **1.01x / 1.12x / 1.12x** |

与基线无差异，仍远低于 1.80x 阈值。

**瓶颈不在 `mm->lock`。** 锁外 prepare 之后，缺页路径仍依次经过两个**全局**
串行点：

* `cg_mem_charge()` → `spin_lock_irqsave(&node->lock)`：**per-cgroup-node** 锁，
  同一 cgroup 内所有缺页共用；
* `pfa_alloc_page()` → `pfa.lock`：`extern pfa_t pfa` 是**单一全局**分配器
  （`spinlock_t lock` 保护全部可变字段），per-CPU batch 也在这把锁下 refill。

摘掉 `mm->lock` 只是把串行点从地址空间锁换成这两把，4 个线程依旧互相排队。
因此「P0–P5 兑现 fault 并行度」这一假设**不成立**；要提升 fault 并行度，必须
先处理分配器与 cgroup 计费（例如 per-CPU 无锁分配器、批量 charge、或把 charge
移出每页路径），而不是继续在地址空间锁上做文章。

### 8.19 追查 §8.18 指出的两个瓶颈：都不是瓶颈（含否证实验）

§8.18 指出锁外仍经过 `cg_mem_charge` 的 per-cgroup-node 锁与全局 `pfa.lock`。按此
做了两个实验，**两个假设都被证伪**。

**实验一：把 cgroup 计费批量化。** `cg_mem_charge(cg, nr_pages)` 本就接受页数，
于是把 fault-around 窗口的逐页 charge 改为整窗一次（每窗 1 次 `node->lock` 而非
4 次），所有回滚路径改为按实际丢弃量一次性 uncharge。

结果（x86_64/KVM smp4、无 NIC、10 次采样，指标取 4T 墙钟）：

| | 4T 中位数 | 4T 全距 |
|---|---|---|
| 改动前 | 0.2128s | 0.2127–0.2377 |
| 批量化后 | **0.2142s** | 0.1929–0.2625 |

**无差异**，完全落在噪声内。已回滚（内存计费正确性关键，批量化后有 4 处 uncharge
站点，多一处错就是泄漏或限流失效，而收益无法证明）。

**实验二：把 fault-around 窗口从 4 页降到 1 页**，用于判断清零（memset）是不是主项。
两者清零的总字节数完全相同，故若 memset 主导则耗时应接近。实测 4T 中位数
**0.326s（窗口=1） vs 0.214s（窗口=4）**：窗口=4 快约 1.5x。说明**主导项是每次
缺页的固定开销，不是清零带宽**；且 fault-around 的摊薄本身已是约 1.5x 的既有收益。

**顺带修正测量方法。** `mm_pt_scale` 的 `ideal_speedup = 4×1T/4T`，而 1T 自身在
0.0603–0.0845 之间波动（±17%），直接被放大进比值。**1.63x / 1.40x 那两个读数是
1T 基线偏慢造成的假象**，不是 4T 变快。真实极差：4T 的 ±16%，比 §8.8 记录的
±9% 噪声地板更差。今后判断此类改动必须直接比较 4T 墙钟中位数，不能引用该比值。

**结论**：缺页路径上，`mm->lock`、cgroup 计费锁、memset 带宽**均非**绑定约束。
剩余的每次缺页固定开销（页表下降、cursor 事务、TLB 本地刷新、`mm->rss` 更新等）
仍未定位到具体热点；要继续提升并行度，需要先给这些点加争用计数，而不是继续
凭猜测改锁。下一步应当是**先测量再优化**：在缺页路径的每个锁上补 perf 计数，
用 KVM 多核采样找出真正的串行点。

### 8.20 perf 计数此前一直无效：**首次读取前计数是关闭的**

§8.19 末尾说要「先测量再优化」。真正去测时先发现：**所有基于 perf 计数的归因一直是
无效的。**

`a20_perf_add()`（`core/perf.h`）第一件事就是：

```c
if (__atomic_load_n(&g_a20_perf_enabled, __ATOMIC_RELAXED) == 0)
    return;
```

而 `g_a20_perf_enabled` **只由 `a20_perf_format()` 设置**——也就是只有读取
`/proc/a20/perf` 才会打开。注释写明这是为了「formal timed builds 不被计数污染」。

后果：**先跑基准、后读计数，整个基准的计数全部被丢弃。** 这也解释了 §8.14 里当时
记为「测试自报计数与实时值不一致」的现象——不是测试解析错，而是它自己在基准跑完后
才第一次打开计数，读到的必然接近 0。

正确用法：**基准前先读一次 `/proc/a20/perf` 预热，基准后再读，差值才是基准的。**
（聚合本身没问题：`a20_perf_format` 确实对 `A20_PERF_MAX_CPUS` 求和。）

### 8.21 预热后的真实争用数据：**没有任何一把锁是瓶颈**

为 `cg_mem_charge` 的 `node->lock` 与 `pfa.lock` 各补了 acquires/contended 计数
（`spin_trylock_irqsave` 探测，不改变行为），并在 x86_64/KVM smp4 上以
「预热 → `mm_pt_scale` → 复读」的方式取数：

| 计数器 | 值 |
|---|---|
| `mm_demand_faults` | 131126（与预期 4×64×512=131072 吻合，证明这次真的采到了） |
| `mm_anon_faults` | 131092 |
| `mm_anon_batch_pages` | 524311（≈4 页/fault，fault-around 确实在生效） |
| `mm_pt_lock_acquires` / `contended` | 131179 / **0**（0%） |
| `mm_pfa_lock_acquires` / `contended` | 16849 / 53（0.3%） |
| `mm_cg_lock_acquires` / `contended` | 266 / 108 |

**结论（与 §8.18/§8.19 的猜测相反）：一把锁都不是瓶颈。**

* **页表锁 13 万次获取、0 次争用**——覆盖式页表节点锁在不相交地址上完全不冲突，
  这正是单级模型想要的效果，它已经在正常工作。
* `pfa.lock` 争用 0.3%——per-CPU batch（32 页/次）已把它摊薄到可忽略。
* `cg_lock` 只有 266 次获取对应 13 万次缺页，因为 `cg_mem_charge` 开头
  `if (!cg) return 0;`——该测试进程没有 cgroup，计费锁根本不在这条路径上。
  （那 108 次争用全部来自引导期的其他进程。）

因此匿名缺页每次约 1.5µs 的成本**不是锁等待**，而是真实工作：4 页清零、页表下降、
cursor 事务、本地 TLB 刷新、`mm->rss` 更新。§8.19 的窗口实验（4 页 vs 1 页差 1.5x）
已经证明固定开销主导；现在争用数据补上了另一半证据——固定开销里没有锁。

若还要继续提升 fault 并行度，可动的只剩**减少每次缺页的固定开销**（例如更大的
fault-around 窗口、把 `rss`/TLB 刷新批量化），而不是继续摘锁。这也意味着
「单级内存模型带来 fault 并行度」这一目标在本负载上无法靠锁改造达成。

### 8.22 扩大 fault-around 窗口：无收益，该方向已到头

§8.21 证明每次缺页的固定开销里没有锁，而 §8.19 的窗口实验（1 页 0.326s vs
4 页 0.214s）显示固定开销仍有分量。据此把窗口从 4 页扩到 8 页——若固定开销
主导，缺页次数减半应当更快。

实测（x86_64/KVM smp4、无 NIC、5 次采样，取 4T 墙钟）：

| 窗口 | 4T 中位数 | 4T 全距 |
|---|---|---|
| 4 页 | 0.2142s（10 次采样） | 0.1929–0.2625 |
| 8 页 | **0.2169s** | 0.1936–0.2340 |

**无差异**，落在噪声内。已回退到 4 页。

结论：fault-around 的摊薄在 4 页处**已经饱和**。1 页→4 页能省 1.5x（少做 3/4 的
固定开销），4 页→8 页则完全没有进一步收益——说明固定开销的绝对量已经小于每次
多摊薄一页所增加的工作（多清零一页 + 多一次 charge + 多装一个 PTE）。窗口这一路
已经没有空间。

**至此，缺页路径上可试的方向都已排除**：`mm->lock`（§8.18）、cgroup 计费锁
（§8.21，该路径上根本不存在）、`pfa.lock`（0.3% 争用）、清零带宽（§8.19）、
窗口大小（本节）。剩下的每次缺页成本是**不可并行的真实工作**在共享内存子系统上的
表现：4 线程做 4 倍工作耗时 3.5 倍（1T 0.060s vs 4T 0.214s），有效并行加速约
1.15x。在 4 vCPU KVM 客户机 + 共享宿主内存的条件下，这更像是内存带宽/宿主资源
上限，而非内核可优化的锁竞争。

## 9. 真实 CortenMM 设计：论文核对结果与重做计划（2026-09-28）

§8 的结论是「P0–P5 已把该做的锁都摘了，但没有收益」。用户指出**实现没有抓住
CortenMM 的关键**，这个判断是对的。逐条核对论文原文（§3.3/§4.1/§4.2/§4.3/§4.5）
后，确认我们缺的不是调优，而是**设计的核心机制**。

### 9.1 我们已经对上的部分

* 每个 PT 页一个**页描述符**，按 PFN 索引，挂在常驻的 per-frame 数组上——
  这正是论文 §3.3「descriptor indexed by the physical page number」。
* per-PTE 元数据数组，存状态（invalid / 虚拟已分配未映射 / 已映射 / swapped）
  与附加状态（权限、COW 位）——§4.3 的 Table 2。
* 每个 PT 页一把 MCS 自旋锁，存在描述符里——§4.5「The lock of each PT page is
  stored in the corresponding page descriptor」。
* 事务式 cursor（`mm_addrspace_lock` + `mm_cursor_{query,map,mark,unmap}`）——
  §3.3 的 transactional interface 与 Figure 4。
* 覆盖节点锁 + stale 标记 + `pt_readers` 读侧计数。

所以 §8.22 那句「没抓住关键」需要精确化：**数据结构层面对了，协议层面没对。**

### 9.2 真正缺的东西（论文的核心主张）

1. **fault 决策完全不查 VMA。** 论文 Figure 8 的缺页处理只做
   `rcursor.query(faulting_addr)`，返回 `Status::PrivateAnon(perm)` /
   `Status::Mapped(page, perm)` / `Invalid`，然后直接 `rcursor.map(...)`。
   整段在事务内原子完成（L18–L40），**从头到尾没有 VMA**。
   论文明确把 Linux 的劣势归因于 "the time Linux spends in the VMA"（§6.2）。
   我们的 fault 至今仍以 `mm_find_vma()` 为决策入口——**这是最关键的缺失**。
2. **整个 fault 在一个事务内原子完成。** 我们是「锁外 prepare → 取回 `mm->lock`
   → 复核 → 装入」的多阶段结构（§8.12），论文是一个 RCursor 走到底。
3. **CortenMMadv 的无锁下降 + DFS 锁全部后代。** 论文 §4.1 Figure 6：遍历阶段
   **不加任何锁**（在 RCU 读侧临界区内），再加锁阶段锁住覆盖 PT 页**及其所有后代**。
   我们只有覆盖节点一把锁。
4. **RCU monitor 真正接线。** 论文：unmap PT 页时先原子清父 PTE，再把 PT 页挂进
   全局 RCU monitor，读侧临界区退出后才释放；被锁到 stale PT 页的线程**重新下降重试**。
   我们的 `mm_pt_defer_free` **零调用者**（§8.13），这条链完全没接。
5. **per-core 虚拟地址分配器**（§4.5 优化项）。
6. **惰性 TLB shootdown（LATR）+ 早期 ack**（§4.5）。
7. **shared 位**（§4.3：每个虚拟页两bit，shared + writable，用于 fork COW）。
   我们只有 `MM_ST_COW_BIT`，没有 shared 位。

### 9.3 测量能力是当前的硬约束（已补上）

在此之前无法判断任何改动，因为 `mm_pt_scale` 只覆盖论文五个微基准里的 PF 一个。
已新增 `user/cmds/stress/cortenmm_bench.c`，实现论文 §6.2 的五个基准
（mmap / mmap-PF / PF / unmap-virt / unmap），每个都有 low-contention 与
high-contention 两个变体，并遵循 §8.20 的计数预热要求。

**riscv64/TCG 基线（4 线程，ROUNDS=256，仅作后续对照的相对基线）：**

| 基准 | low scaling | high scaling | 备注 |
|---|---|---|---|
| mmap | 1.29x | 1.33x | |
| mmap-PF | 1.09x | 0.96x | |
| PF | 1.60x | 30.13x | 30x 是 1T 窗口太短造成的噪声，不可信 |
| unmap-virt | 0.98x | 1.42x | |
| unmap | 1.97x | 1.08x | |

TCG 单核慢、计时窗口偏短，比例只能作相对参考；论文的 33x–2270x 是 384 核
数据，本机 4 核不可能复现。**可信的对照结论要等 P6 落地后在同一环境重测。**

### 9.4 顺带发现的一个无关既有缺陷

`ARCH=x86_64 ABI=linux dev-build` 产出的 FAT32 镜像**开不了机**：
`[INIT] Cannot open /bin/init: -2`，回退 `/init` 也失败，最终
`panic("init: no init program found")`。该镜像没有 `/bin` 目录（所以
`/bin/init` 必然失败），而 `/init` 回退也读不出来。
**与本次改动无关**：移除 `cortenmm_bench.c` 后重新构建仍然复现。
仓库的 x86_64 smoke 门走 `tools/a20 test`（另一套镜像），不受影响；
所有 riscv64 门正常。这是 x86_64 `dev-build` 路径的既有缺陷，另行记录。

### 9.5 下一步（按论文优先级）

1. fault 改为**只依据 per-PTE 状态**决策，去掉 VMA 依赖——论文的核心，第一优先。
2. 整个 fault 收进**单个事务**。
3. 接线 RCU monitor：PT 页延迟释放 + stale 重下降。
4. CortenMMadv 协议：无锁遍历 + DFS 锁后代。
5. 之后才是 per-core VA 分配器与 LATR。

### 9.6 下一阶段的关键发现：`MM_ST_ANON_VIRT` 从未被写入

要按论文让 fault 不查 VMA，前提是「这个虚拟页已保留、只是还没 backing」这一状态
**存在于 per-PTE 元数据里**。核对发现：

* `MM_ST_ANON_VIRT`（= 论文的 `Status::PrivateAnon`）在全树**只有一处出现**，
  而且只是审计代码里的白名单判断（`pt.c:887`）——**没有任何地方写入它**。
* `mmap` 路径（`mm_mmap_locked` / `mm_mmap_file_locked` / `mm_mmap_vmo_locked`）
  **完全不写 per-PTE 状态**，只做 `mm_insert_vma()`。

所以今天 per-PTE 状态里根本没有「已保留」这个概念，fault 只能靠
`mm_find_vma()` 才知道这个地址该不该有映射。**这正是 §9.2 第 1 条无法直接实现的
根本原因**，也是我们与论文差距的最小可操作切入点。

值得注意的是审计代码早就为它准备好了位置：`mm_pt_audit_addrspace` 对「PTE 缺失」
的叶节点允许 `MM_ST_ANON_VIRT` 与 `MM_ST_SWAPPED` 两种 class（`pt.c:885-887`），
即设计上**本来就预期** on-demand paging 会写入 `MM_ST_ANON_VIRT`。只是这一层从未接上。

因此 P6 的正确施工顺序是：

1. **mmap 时按需预标记**：为匿名保留范围把 per-PTE 状态置为
   `MM_ST_ANON_VIRT`（并带权限位）。注意不能为整个范围预分配 PT 页——那会把
   稀疏映射变成稠密，抵消 fault-around 的收益；需要「惰性建立中间节点」或在
   fault 时补写。这是本阶段最需要小心的取舍。
2. **fault 改为只查 cursor 状态**：`PrivateAnon` → 分配零页并 map；
   `Mapped` → COW / 权限；`Invalid` → SIGSEGV。整段在**一个事务**内。
3. 之后才是 RCU monitor 与 CortenMMadv 的无锁下降。

**未解决的设计问题（下一步必须先回答）**：论文没有说明它如何避免为稀疏匿名映射
预建 PT 页。我们的 `mm_pt_note_present` 依赖 PT 页已经存在（见 `mm.c` 的 level
下降），所以「mmap 阶段标记尚未映射的叶子」需要中间节点先存在。候选方案：
(a) fault 时惰性建中间节点并就地标记（回退到接近现状）；
(b) mmap 时为匿名范围预留上层节点但不建叶子表；
(c) 引入「区间级」class 记录（类似 per-VMA 但按 PT 粒度缓存）。
在动手前需要先定这个，否则会做出「为了不查 VMA 而把内存开销放大」的实现。

## 10. 真实设计的落地与首轮实测（2026-09-28）

已按论文把「状态生产端」与「状态消费端」接通，三笔提交：

* `ed97c7e8` mmap 时预标记匿名区间（`MM_ST_ANON_VIRT` + 权限），即论文 §4.3 的
  on-demand paging 状态。
* `01673960` mprotect 同步刷新「已保留未缺页」叶子的权限状态——这是前置条件：该状态
  一旦成为 fault 唯一的权限来源，mprotect 不同步就等于权限绕过。
* `15535e04` 缺页按 per-PTE 状态决策，不查 VMA（论文 Fig. 8 的核心）。
* `fb26bf47` 预标记扩展到 ELF 匿名段与 brk。

**机制已被证实有效**：riscv64 上 mm_stress 期间
`mm_anon_provisioned=18660`、`mm_fault_from_status=479`，即 1482 次 demand fault 中
有 479 次（32%）完全不经 `mm_find_vma`。这正是论文所称「省掉 Linux 花在 VMA 上的
时间」（§6.2）那部分收益的载体。

### 10.1 但首轮实测是**回归**，必须如实记录

同一环境（riscv64/TCG，4 线程）对比 §9.3 基线，取 4T 的 ns_per_op（比值指标因 1T
窗口抖动不可用，见 §8.19）：

| 基准 | 基线 | 预标记后 | 变化 |
|---|---|---|---|
| mmap | 52–57 µs | 81–82 µs | **慢 1.5×** |
| mmap-PF | 156–171 µs | 184–239 µs | 慢 1.2–1.4× |
| unmap | 134–173 µs | 204–208 µs | 慢 1.2–1.5× |
| unmap-virt | 40–44 µs | 66–68 µs | **慢 1.6×** |

原因是**预标记的实现代价**，不是设计本身错了：`mm_cursor_mark_prot()` 读的
`cur->path[0]` 只有在覆盖层为 0 时才被 `mm_addrspace_lock()` 的下降循环填好，
所以当前实现**每页都要开一次 cursor、加锁、标记、解锁**。一个 2 MiB 映射就是 512 次
完整的 cursor 往返。论文是对整个区间一次加锁的，我们没有做到，代价就落在 mmap 侧
——而这恰好是论文承认会变慢的那一项（§6.2「mmap 比 Linux 略慢」），只是我们的实现
把它放大到了 1.5×。

**下一步必须先优化预标记的锁粒度**（一次锁住一个 leaf table 的范围，或让
`cursor_leaf_table()` 在覆盖层 > 0 时也能定位叶子表），否则这笔开销吃掉了 fault 侧
的收益。在 TCG 上尤其明显；即便如此，也不能以「反正测不出来」为由保留当前形态。

### 10.2 顺带修正基准自身的一个缺陷

`cortenmm_bench` 的 PF 相位在**同一个 512 MiB 池**上反复触碰，round 0 之后页面已
是热的，测到的是 memcpy 而非缺页——基线那组 `PF 1T=0.0014s`（约 93M 页/秒）就是
这个假象，物理上不可能是真实缺页。PF 相位必须每轮用**全新映射**才有意义（与
`mm_pt_scale` 注释里「复用热页只会测到 memcpy」的告诫一致）。此项待修，当前所有
PF 数字（含本节的 479/1482）只反映「走了状态路径的缺页数」，不能当作缺页吞吐。

### 10.3 两次「优化」都没能解决，且必须承认其中一次是在追噪声

针对 §10.1 的回归做了两轮修改：

1. **锁粒度**：`mm_pt_provision_anon()` 由「每页一次 cursor」改为「每个 leaf table 一次
   cursor」——单页区间会让 `mm_addrspace_lock()` 下降到覆盖层 0，`cur->path[0]` 就是叶子
   表，而它拿到的锁正是该叶子表自己的锁，所以整表 512 项都已被排他。语义与论文
   「对覆盖 PT 页加一次锁」一致。
2. **元数据与计数器**：`mm_pt_note_present()` 每页都要做一次 `mm_pt_meta()`，即
   `virt_to_pfn` → frame flag 检查 → `.pt` 解引用三次依赖式加载；再叠加一次原子
   read-modify-write 计数。改为把元数据解析提到循环外（新增 `pt_note_present_meta()`
   供批量调用者复用），计数器改为整段一次 `a20_perf_add()`。

三轮实测的 mmap 4T ns_per_op：**基线 52–57 µs → 每页锁 81–82 µs → 每表锁 79–89 µs
→ 提元数据 90–97 µs**。

**结论必须写清楚**：基线与「带预标记」之间的差距（约 1.5×）远高于噪声，是真实的；
但我这三版之间的差异完全落在运行间抖动内，**无法分辨**。也就是说第 2 项改动既没有
被证实有效，也没有被证实有害，第 1 项同理。既然分辨不了，就不能宣称优化成功，也
不应继续按 TCG 的噪声调参——继续调下去只是在拟合噪声。

余下的开销不在锁、也不在元数据查找，而在 PT 页本身的分配/释放往返（pfa 分配、
cgroup charge、4 KiB 清零、`pt_meta` 初始化，unmap 时再同步释放——注意
`mm_pt_defer_free()` 目前仍无调用者，页表帧是同步回收的）。这与论文自身的取舍一致：
论文 §6.2 明确说它的 mmap 比 Linux 慢，因为 on-demand paging 必须在 mmap 时就把
per-PTE 状态写好；而状态存在 PT 页元数据里，参考实现把它编进 PTE 软件位，写状态就
必须先有 PT 页。也就是说这笔开销是该设计的固有成本，不是本实现的实现缺陷。

**平台限制**：TCG + 4 线程 + 共享宿主下，run-to-run 方差可达 ±30%，无法分辨 1.2×
以内的差异。真正的性能结论必须在 x86_64/KVM 上取，而本仓库的
`ARCH=x86_64 ABI=linux dev-build` 镜像存在既有启动故障（`Cannot open /bin/init: -2`），
需要先修镜像或另辟 x86 测量路径。

**教训**：本轮连续两次「优化」都是先改后测，且都在同一批次噪声里下结论。已经改为
先记录方差、再判断改动是否超出方差。

### 10.4 基准修正后：缺页成本首次可信，以及一个必须承认的空洞

修正两处（`user/cmds/stress/cortenmm_bench.c`）：

* **B_PF 改用 `MADV_DONTNEED` 再触碰**。`mm_madvise_dontneed()` 只校验 VMA 覆盖
  范围、然后丢掉 PTE，VMA 本身保留——所以每轮都拿到「VMA 在、PTE 全空」的状态，
  mmap 开销被排除在测量之外，而每一页都是冷缺页。原先复用固定 slice，第 0 轮之后
  测的全是 memcpy。
* **B_UNMAP_VIRT 补上触碰**。原实现从不触碰，被 unmap 的洞里根本没有页，测到的是
  对未使用区间的 VMA 切分，而不是「拆掉一个已填充区间」；注释里「下一轮仍会缺页」
  当时是假的。

修正后 3 次采样（4T ns_per_op，中位数 / 离散度）：

| 基准 | 中位数 | 离散度 |
|---|---|---|
| mmap low / high | 83.0 / 87.2 µs | 5% / 7% |
| mmap-PF low / high | 187.8 / 191.7 µs | 9% / 16% |
| **PF low / high** | **86.8 / 94.7 µs** | 4% / 19% |
| unmap-virt low / high | 68.5 / 69.0 µs | 10% / 10% |
| unmap low / high | 185.0 / 214.4 µs | 21% / 5% |

**PF 现在是 ~170 ns/次缺页**（512 页 ÷ 86.8 µs），这个量级合理。

**必须承认的空洞**：修正前的 PF 数字（30–36 µs/op）测的是热页 memcpy，不是缺页，
所以 §10.1 那几轮「改动 vs 改动」的对比里，**PF 一栏自始至终是无效的**。也就是说
我至今**没有一次有效的「按状态缺页路径 vs 走 VMA 缺页路径」的开销对比**——旧的
一侧从未被正确测过。§10.1 唯一成立的结论只有 mmap 那条：带预标记比不带贵约 1.5×，
且该差距远大于 5–10% 的离散度。

因此当前能说的只有：机制正确（479/1482 次缺页完全不经 VMA，且关机审计全 0），
代价已知（mmap 约 1.5×，与论文自述的 mmap 更慢同向），性能收益**未测得**。要拿到
收益结论，必须在 x86_64/KVM 上用修正后的基准做 A/B，这依赖先解决 §10.3 提到的
x86 镜像启动故障。

采样纪律（按 §10.3 的教训固定下来）：每个配置至少 3 次采样，先看中位数与离散度，
离散度内（<15%）的比较一律不作为结论。
