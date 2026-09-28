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

### 10.5 接线 RCU 的生产端（**已回退，勿信**）

> **本节的代码改动已在 `db91e679` 之后回退。** 下文对 use-after-free 的分析仍然成立
> （PT 页确实是被同步释放的，而 `mm_addrspace_lock()` 只锁覆盖节点），但当时给出的修法
> **不安全**：它把 `pt_unmap()` / `pt_unmap_leaf()` 的首参从 `pgdir` 改成 `mm`，并在这两个
> 函数内部调用 `mm_pt_defer_free()`。而 8 个调用点中有 6 个**既不持 `mm->lock`、也不在
> `mm_tlb_invalidate_begin()/finish()` 事务内**（`vma.c` 全文没有任何
> `mm_tlb_invalidate_*`；`sys_mm.c` 的调用点在第 281 行，而 `finish` 在第 210 行，
> 已在事务之外）。`mm_pt_hold_table()` 会无锁地改写 `mm->tlb_holds` 链表，且没有事务
> 就永远没有人来排空它——既是链表竞争，也是 PT 页泄漏。
>
> 正确修法必须先给这些调用点补齐 `mm->lock` + 事务边界（或改用一个不需要事务的
> 独立回收队列），再谈接生产端。**在此之前不要重新套用该补丁。**

核对后发现此前的判断需要更正一半。**读侧其实早就接好了**：`mm_addrspace_lock()` 在下降
前 `mm_pt_read_enter(mm)` 并置 `cur->in_read_side`，`mm_cursor_unlock()` 退出，另有 8 条
错误返回路径各自配对退出。`vm.c` 里 `mm_tlb_invalidate_finish()` 的排空循环也在等
`mm->pt_readers` 归零，并已经会释放 `MM_TLB_HOLD_PT`。

真正缺的是**生产端**，后果是一个真实的 use-after-free：

* `mm_pt_defer_free()` 与 `mm_pt_mark_stale_recursive()` 都**没有任何调用者**。
* 于是 `pt_unmap()` / `pt_unmap_leaf()` 的拆表循环一直走的是
  `mm_pt_node_fini(child); frame_free(child);`——**同步释放**。
* 但 `mm_addrspace_lock()` 只锁覆盖节点，**后代节点是无锁遍历的**。所以另一个 CPU 上
  正沿着 `child` 下降的 cursor，可能在该帧被 buddy 回收后继续读它。
* 排空循环等的是一个永远不会被填充的链表，所谓宽限期实际上保护不了任何东西。

修法就是接上生产端：拆表时先 `mm_pt_mark_stale_recursive()`（让缓存了该节点的
cursor 观察到 `stale` 并重下降，这本来就是 `mm_addrspace_lock()` 里那个
`attempt < 8` 重试循环要处理的情形），再 `mm_pt_defer_free()` 把帧交给 shootdown
排空；只有入队失败（ENOMEM）才退回原来的同步释放，避免漏帧。

为此把 `pt_unmap()` / `pt_unmap_leaf()` 的首参从 `pgdir` 改成 `mm`——排空队列挂在
`mm->tlb_holds` 上，没有 `mm` 无法入队。所有调用点本来就已经持有 `mm`（它们原先正是
用 `mm->pgdir` 取的 pgdir），故为机械改动；`signal.c` 里传的是 `t->pgdir`（`t` 是
task），改为 `t->mm`。`mm.h` 补 `struct mm_struct;` 前向声明，NOMMU stub 同步改签名。

安全性前提已核对：munmap/mremap/madvise 的拆表点都在 `mm_tlb_invalidate_begin()` …
`mm_tlb_invalidate_finish()` 事务内（`mm_munmap` 在 287 行开、291 行关，中间调用
`mm_munmap_locked`），所以入队的帧一定会被排空，不会残留。

验证：riscv64 / aarch64 / loongarch64 / ppc64le / x86_64 与 riscv64/aarch64 的 NOMMU
构建通过；smoke-mm-stress、smoke-mm-fork-exec-race、check-mm-lock-model 通过，关机审计
missing_meta/present/absent/prot/cow/vma 全 0，且 `pt_pages=6` 与改动前一致——若帧入队
后未被排空，这个计数会升高，这是排空确实生效的证据。

**未能验证**：arm32 在本环境**改动前就无法构建**（`arm-linux-gnueabihf-gcc` 不在
PATH），而 arm32 有自己一份 `pt_unmap`/`pt_unmap_leaf` 实现（已同步改签名并从
`mm->pgdir` 取 pgdir），但编译未经检验。属既有环境限制，已如实标注。

### 10.6 撤回 §10 的核心数据；状态缺页路径至今是死代码

**先撤回一个错误结论。** §10 与 §10.5 反复引用的「`mm_fault_from_status=479`，即 1482 次
demand fault 中 479 次（32%）完全不经 `mm_find_vma`」**是错的**。在记录该数字的**确切
提交** `fb26bf47` 上重新测量，该计数为 **0**；同期真正增长的是 `mm_anon_faults=679`
——即**走 VMA 的那条路径**。这条结论作废，§10 里所有依赖它的表述一并失效。

**原因**（读代码可证）：`mm_cursor_query()` 对**缺失**的 PTE 直接 `return 0` 而**不写
`*cls_out`**，于是 `cls_out` 保持调用方给的 `MM_ST_CLS_BYTE(MM_ST_INVALID)`。而 fault
路径的判据是 `!already && MM_ST_GET_CLASS(cls_byte) == MM_ST_ANON_VIRT`，
`INVALID != ANON_VIRT`，因此**永不成立**。这段逻辑自 `ff75efde`（P2）起就是这样，
也就是说论文 Fig. 8 的「按状态决策的缺页」路径**从来没有执行过一次**。

尝试把它激活后，暴露出两个潜在缺陷：

**(a) 文件映射被误判为匿名（本次已修）。** `pte_to_vm_flags()` 给**每一个**基于文件的
段都打上 `VM_ANON`，所以私有 RELRO 段（`flags=0x13`、`file_fd=6`、**没有** `VM_FILE`
位，因为 `file_fd` 是 `elf_add_vma()` 返回之后才赋值）与 `.bss` 根本无法区分。
`fb26bf47` 的预标记条件 `(vm_flags & VM_ANON) && !(vm_flags & VM_SHARED)` 于是把 mksh 的
RELRO 标成了 `MM_ST_ANON_VIRT`，缺页时返回**匿名零页**而不是文件内容：
`FATAL: pid=4 signal=11 comm=mksh`，`stval=0x6a170`，`page_words` 全 0。

修法：`elf_add_vma()` 增加显式 `bool anon` 形参，由调用点逐个声明——文件段装载点
（逐页预载、`.bss` 尾段、exec 映像）传 `false`，栈/TLS/堆传 `true`。**在 `elf_add_vma()`
内部靠 `vm_flags` 推断不可能正确**，因为此时 `file_fd` 尚未赋值。

**(b) brk/堆区间仍然崩溃（未定位）。** 修掉 (a) 之后失败点前移到堆：
`stval=0x1f20000`，落在 brk VMA `[0x1f20000,0x1f21000) flags=0x13 pte_flags=0xd7
file_fd=-1 off=0x1000`，该页已映射但全 0。**根因未定位**，故状态路径保持不启用。

**结论与当前状态**：论文核心的状态驱动缺页路径目前**不可用**，且已证明至少存在两个
潜在缺陷；在 (b) 定位并修复之前不应启用，否则 exec 与堆会直接崩。HEAD 刻意让该路径
保持 inert（`mm_fault_from_status` 恒为 0），全部功能门通过。性能结论不受影响：
§10.1 那个 mmap ~1.5× 回归是**预标记在 mmap 侧就要付的代价**，与该路径是否真的执行无关。

### 10.7 状态路径的位置本身是设计缺陷（与 (b) 无关的独立问题）

排查 (b) 时发现一个更根本的问题：状态块（`fault.c` 958–996）位于
`vm_area_t *vma = mm_find_vma(mm, page_va);`（998 行）**之前**。也就是说一旦启用，
它在**完全不查 VMA** 的情况下直接映射并返回，从而绕过后面所有基于 VMA 的判定：

* **绕过 userfaultfd**（1007–1008）：已注册 userfaultfd 的区间本应把缺页停住交给
  handler，状态路径却会直接给出零页，handler 永远收不到事件。
* **绕过 VM_SHARED 判定**（1021）：共享映射的写缺页需要 COW/共享语义，不能按私有
  匿名处理。
* **绕过 fault-around 的安全门**（522）：那段逻辑明确要求
  `!(vma->vm_flags & (VM_SHARED | VM_STACK | VM_FILE | VM_VMO))` 才做 4 页
  fault-around；状态路径没有任何等价门限。
* **与 brk 路径的前提冲突**：`fault.c:355` 的 brk 处理条件是
  `page_va >= start_brk && page_va < ROUND_UP(brk) && !mm_find_vma(...)`——它**只**
  处理「没有 VMA」的地址；而状态路径恰恰是「有状态标记但可能没有 VMA」的产生者。

论文的设想是 per-PTE 状态**取代** VMA 作为权威来源，那要求状态本身携带全部安全信息
（是否 userfaultfd 注册、是否共享、是否可 fault-around），并且 mmap/mprotect/munmap/
userfaultfd 注册**每一次**都同步更新它。本实现的状态字节只有 class + prot 三位，
不含这些信息，所以「在 `mm_find_vma` 之前无条件信任状态」是不成立的。

**因此正确顺序应是**：状态路径不是放在 VMA 查找**之前**的旁路，而是要把 VMA 查找
**替换掉**，前提是状态已扩充到足以承载上述全部判定，并且所有会改变安全语义的
syscall 都已接入状态更新。缺一不可。当前两者都不满足，故保持 inert 是正确取舍。

**下一步（按依赖顺序）**：
1. 先定状态字节的完整语义：至少需要 shared、userfaultfd-registered、seal/fault-around
   许可三类位，并明确谁负责写、谁负责清。
2. 让 `mmmap` / `mprotect` / `munmap` / `madvise` / userfaultfd 注册全部接入该状态的
   更新，且每条路径都有「状态与 VMA 不一致」的自检（挂到现有 MM-ASM auditor 上）。
3. 只有 1、2 完成且 auditor 能证明等价后，才把状态路径移到 `mm_find_vma` 之前；
   在此之前 (b) 的 brk 崩溃只是表象，即使修好 (b) 也会立刻撞上 userfaultfd 绕过。

### 10.8 同树 A/B：预标记**不是** mmap 回归的原因，推翻 §10.1 的归因

§10.1 把 mmap ~1.5× 的回归归因于「预标记的实现代价」，并据此做了两轮优化
（§10.3）。但**从未做过同树 A/B**——所有对比都是「改动前的老提交」对「改动后的新提交」，
中间混进了别的变化。在当前 HEAD 上把 `MM_ANON_PROVISION_MAX_PAGES` 从 4096 改成 0
（即完全关闭预标记），其余一切不动，重测 3 次（4T ns_per_op 中位数）：

| 基准 | 预标记开 | 预标记关 | 关相对开 |
|---|---|---|---|
| mmap low | 83.0 µs | 85.3 µs | **+3%（更慢）** |
| mmap high | 87.2 µs | 88.6 µs | +2%（更慢） |
| mmap-PF low | 187.8 µs | 245.3 µs | +31%（更慢） |
| PF low | 86.8 µs | 111.4 µs | +28%（更慢） |
| unmap-virt high | 69.0 µs | 69.1 µs | 0% |
| unmap low | 185.0 µs | 162.5 µs | −12%（更快） |

**结论一（推翻 §10.1）**：关掉预标记后 mmap **没有变快**，两��相差 2–3%，远在噪声内。
所以「预标记导致 mmap 慢 1.5×」这个归因**是错的**，§10.1 与据此展开的 §10.3 两轮优化
都建立在错误前提上。真实原因尚未定位——最可能是 §9.3 那条基线与当前树之间还有别的
未识别差异（基线取自加预标记之前，其间 §10 之后我又动过 `munmap.c`/`elf.c` 的调用点
与 fault 路径周边），因此「1.5×」本身也需要重新用同树 A/B 复核才能成立。

**结论二（与直觉相反）**：关掉预标记让 **PF 与 mmap-PF 明显变慢**（+28%/+31%）。合理解释是
预标记在 mmap 时就把 PT 路径建好，后续缺页的下降不需要再分配中间节点；关掉后每次缺页
都要重新走分配。这反过来说明预标记**对缺页侧确有价值**——只是它没有体现在 §10.1 关心的
mmap 指标上。

**结论三（测量纪律再次被验证）**：本轮 mmap-PF 的采样离散度高达 45%（193–281 µs），
远超 §10.3 定的 15% 门槛。TCG + 4 线程 + 共享宿主下，**除 mmap-PF/PF 外的所有对比都
不可分辨**。在拿到 x86_64/KVM 之前，任何小于 ~1.5× 的性能结论都不成立。

### 10.9 x86_64 dev 镜像启动失败的完整根因（不是镜像问题，是 W^X 缺页 bug）

此前把 x86_64 记为「dev 镜像 FAT32 启动失败，`Cannot open /bin/init: -2`」。这个描述是
错的：`/bin/init` 并不是缺失，而是**根本没有块设备**。完整链条（实测，非推断）：

```
[INIT] WARNING: no FAT32 device for /bin          <- mount_setup.c:348
[ERR] [DRVMOD] virtio-blk.a20drv: cannot mark module text executable
[DRIVERMGR] /boot/drivers/virtio-blk.a20drv: module load failed (-12)
[ERR] [DRVMOD] virtio-scsi.a20drv: ... 同上
[ERR] [DRVMOD] ahci.a20drv:      ... 同上
[INIT] Cannot open /bin/init: -2
KERNEL PANIC: init: no init program found
```

即 **virtio-blk / virtio-scsi / ahci 三个块驱动全部加载失败**，所以扫不到 FAT32 设备，
`mount_block_devices()`（`kernel/fs/mount_setup.c:310`）无法把 FAT32 挂到 `/bin`，于是
`/bin/init` ENOENT。注意 `mdir` 显示镜像里 `init` 在**根目录**、根本没有 `/bin`
目录——riscv64 能跑起来是因为它走的是另一条路径，两个镜像的目录结构其实一样。

失败点在 `kernel/drvmod/loader.c:1471`：

```c
if (arch_kwx_module_protect(pfn_to_phys(alloc_pfn), text_region_size,
                            total_size) < 0) {
    kerr("[DRVMOD] %s: cannot mark module text executable\n", name);
    ...
    return -ENOMEM;      /* -12 */
}
```

x86_64 实现（`kernel/arch/x86_64/mm/kwx.c:92`）转发到 `x86_kwx_set_pages()`
（同文件 :64），后者对每一页调用 `x86_kwx_split_pmd(va)`，**该函数返回 NULL 即 -ENOMEM**。
`x86_kwx_split_pmd()` 只有两条路返回 NULL：

1. `x86_kwx_pd(va)` 返回 NULL——即 `boot_pdpt_hh[slot]` 不是 `PTE_V`，或**仍是 1 GiB 大页
   (`PTE_PS`) 未降级**。函数自己的注释就写了前提「对应 1 GiB 槽位须已降级」，而降级只由
   `arch_kernel_wx_finalize()` 对 `covers_ram` 的槽位做。
2. `if (!(e & PTE_V)) return NULL;`——**该 2 MiB 的 PD slot 压根不存在**。注意它对
   「slot 是 2 MiB 大页」是能处理的（会拆分），唯独对「slot 不存在」直接放弃。

模块页来自 `pfa`，物理地址可能落在直映射尚未建立 PD slot 的区域，于是走到第 2 条。
**尚未确定实际命中的是第 1 条还是第 2 条**——这需要插桩或对照 `boot_pdpt_hh` 实测，
不能在证据不足时直接改引导页表。可能的修法方向（待验证后再动）：
把「slot 不存在」也当作需要新建的映射（按 `arch_pt_vpn(va,2)<<30 | arch_pt_vpn(va,1)<<21`
算出该 2 MiB 的物理基址，分配 PT 并填 512 个 R|W leaf），而不是直接失败。

**影响**：这是 x86_64 上**所有**内核态驱动模块（含全部块驱动）都加载不了的前置 bug，
与 CortenMM 改动无关，属既有问题；但它挡住了唯一能分辨性能结论的平台（TCG 采样离散度
最高到 45%，见 §10.8），因此在修好之前 §10 的性能问题无法收口。

### 10.10 修好 x86_64 引导：两个独立故障，其中一个是真实内核 bug

§10.9 定位到失败点在 `x86_kwx_split_pmd()` 返回 NULL，但当时有两个候选分支。**插桩实测**
（两处 NULL 各加一条 `kerr`，事后已移除）给出确定答案：

```
[ERR] [kwx-diag] pd NULL va=ffff80013fb80000 slot=4 e=1000000e3 V=1 PS=1
[ERR] [kwx-diag] pd NULL va=ffff80013fb88000 slot=4 ...
（"pdslot-absent" 命中 0 次）
```

命中的是**第 1 条**：`boot_pdpt_hh[4]` 的 `V=1, PS=1`——1 GiB 槽位 4 仍是**大页，从未降级**。
`va=ffff80013fb80000` 对应物理 `0x13fb80000`（≈4.99 GiB），而启动日志报告
`[RAM] usable 0x100000000..0x140000000 (1024 MiB)`，即 4–5 GiB 是**真实 RAM**，pfa 分配
到这里完全正常，不是越界。

**Bug 本体**：`arch_kernel_wx_finalize()` 写的是

```c
for (int slot = 0; slot < 4; slot++)      /* kernel/arch/x86_64/mm/kwx.c */
```

但直映射并不止 4 GiB——`firmware.c` 的 `X86_HIGH_RAM_MAP_END = 0x200000000`（8 GiB），
固件按整 1 GiB 块把 4 GiB 以上的可用内存映射进来。于是槽位 4–7 是真实 RAM，装着真实的
模块页，却因为循环只扫 0–3 而从未降级。后果是 `x86_kwx_pd()` 对这些地址返回 NULL →
`x86_kwx_split_pmd()` 返回 NULL → `arch_kwx_module_protect()` 返 -ENOMEM →
**virtio-blk / virtio-scsi / ahci 三个块驱动全部加载失败**（-12）→ 扫不到 FAT32 →
`/bin/init` ENOENT → PID 1 panic。**x86_64 上所有内核态驱动模块都加载不了。**

**修法**：把直映射范围常量提到共享头 `arch/x86_64/include/platform.h`（原先只在
`firmware.c` 里局部定义，提上来是为了避免与 `firmware.c` 漂移），循环上界改为
`X86_HIGH_RAM_MAP_END >> 30`，即覆盖直映射实际映射的每一个 1 GiB 槽位。槽位 0 含内核
映像、其降级路径（旁路建好新 PD 再单次写入替换 PDPT 项）原样保留。

**第二个故障不是内核问题**：修完驱动能加载后，仍报 `no FAT32 device for /bin`。那是我
自己的 QEMU 参数不对（用了默认 i440fx machine + `if=virtio`）。改用仓库自己
`tools/targets-steps.mk` 里的写法（`-machine q35 -device virtio-blk-pci,drive=x0`）即
正常。**所以我此前「x86_64 dev 镜像坏了」的结论是错的**：镜像没坏，坏的是驱动加载，
而那半是我参数错。

修后 x86_64 完整启动：`[INIT] Block device -> /bin (fat32)` → execve mksh →
`audit errors=0`、`MM-ASM pt_pages=9 missing_meta=0 present=0 absent=0 prot=0 cow=0
vma=0 anon_virt=0`。riscv64 侧无回归（smoke-mm-stress / smoke-mm-fork-exec-race 通过，
审计全 0），x86_64/riscv64/aarch64 构建通过。

### 10.11 x86_64/KVM 可用了，但噪声比结论还大——撤回「1.5× 远高于噪声」

`f3f77234` 修好 x86_64 引导后，第一次在 KVM 上跑论文五项基准（q35 + virtio-blk-pci，
4 线程，ROUNDS=256，3 次采样，4T ns_per_op）：

| 基准 | min | 中位 | max | max/min |
|---|---|---|---|---|
| mmap low | 100756 | 105597 | 147261 | 1.46× |
| mmap high | 81787 | 105405 | 116386 | 1.42× |
| mmap-PF low | 161219 | 206353 | 286734 | 1.78× |
| mmap-PF high | 171585 | 200825 | 257974 | 1.50× |
| PF low | 76907 | 95585 | 124254 | 1.62× |
| PF high | 73293 | 94050 | 130189 | 1.78× |
| unmap-virt low | 64853 | 76494 | 83013 | 1.28× |
| unmap-virt high | 62955 | 81362 | 83883 | 1.33× |
| unmap low | 171983 | 191960 | 216127 | 1.26× |
| unmap high | 211129 | 221864 | 353622 | 1.67× |

**关键观察一：噪声带宽达 1.26–1.78×。** 这比 TCG 并没有变好多少（§10.8 记的 mmap-PF
离散度是 45%）。所以「换到 KVM 就能分辨性能结论」这个预期**不成立**。

**关键观察二：第 3 次采样在 10 个指标里有 9 个是最慢的。** 随机噪声不会这么整齐地
同向劣化——这是**系统性漂移**（共享宿主负载、频率调节，或同一宿主上并发任务），不是
采样抖动。也就是说「多采几次取中位数」并不能救回来，因为漂移是单调的。

**必须撤回的结论**：§10.1 写「基线与带预标记之间约 1.5× 的差距远高于噪声，是真实的」。
在 1.78× 的噪声带宽下，**1.5× 落在噪声之内**，那句话同样不成立。至此三条性能结论
（1.5× 回归真实、预标记导致回归、预标记让 PF 变快 28%）**全部不可采信**——不是因为
推理错，而是因为**本宿主上无法把它们和噪声区分开**。

**这对方案的影响**：在拿到可用的测量手段之前，不应该再基于这些数字做设计决策
（包括我曾建议的「关闭/保留预标记」）。要把测量做可信，至少需要：
CPU 绑定（`taskset` 固定到独占核）、显著更多采样、并确认采样期间宿主空闲；
或者换到独占/裸金属环境。**这应当排在任何进一步的性能优化之前**——否则后续每个
数字都同样不可信。

### 10.12 顺序 A/B 无效：必须交错采样

§10.11 找到噪声来源（共享宿主，4 个 opencode 进程各占 50–116%）后，先用
`taskset -c 12-15` 把 guest 的 4 个 vCPU 绑到 4 个独占核，噪声确实改善：最差 max/min
从 1.78× 降到 1.47×，10 项里 9 项落到 1.30× 以下（`mmap high` 1.08×、
`mmap-PF high` 1.12×、`PF low` 1.14× 已经够紧）。

于是用同样的绑定做了预标记 ON/OFF 的 A/B（各 4 次采样，OFF 臂在后跑）。结果：

| 基准 | ON 中位 | OFF 中位 | OFF/ON | ON 离散 | OFF 离散 | 离散变化 |
|---|---|---|---|---|---|---|
| mmap low | 86573 | 102353 | 1.18× | 1.22× | 1.36× | 1.11× |
| mmap high | 79429 | 90115 | 1.13× | 1.08× | 1.51× | **1.40×** |
| mmap-PF low | 200617 | 211826 | 1.06× | 1.29× | 1.90× | **1.48×** |
| mmap-PF high | 183376 | 212502 | 1.16× | 1.12× | **2.37×** | **2.12×** |
| PF low | 90917 | 106554 | 1.17× | 1.14× | 1.64× | 1.44× |
| unmap low | 164985 | 182378 | 1.11× | 1.28× | 1.47× | 1.15× |

中位数看起来「OFF 慢 6–18%」，但**这个差值不可采信**：OFF 臂的**离散度本身**普遍变大
（mmap-PF high 从 1.12× 涨到 2.37×），且 OFF 臂最后一次采样在 10 项中 7 项最差，
而 ON 臂只有 3 项。这说明**宿主在 OFF 臂采样期间变忙了**——漂移发生在两臂**之间**，
不是臂内。臂间漂移（可达 1.2×）大于待测效应（≤1.18×），所以顺序 A/B 无法把二者分开。

**结论**：在这个共享宿主上，**顺序 A/B 一律无效**，无论采样多少次。要做可信 A/B 必须
**交错采样**（ON,OFF,ON,OFF,… 交替），让漂移对两臂对称；或者等宿主空闲的窗口。
顺带把 `MM_ANON_PROVISION_MAX_PAGES` 做成可运行时切换（当前是编译期常量，强制每臂
重新构建，这本身就是交错采样的障碍）。

至此关于预标记开/关的性能问题**仍然没有答案**，且这是测量方法问题而非实现问题。
在拿到交错数据之前，不要再据这些数字改动设计（包括「保留还是关闭预标记」）。

### 10.13 预标记上限改为可运行时切换（交错采样的前提）

`MM_ANON_PROVISION_MAX_PAGES` 原先是编译期常量，导致 ON/OFF 两臂必须各自重新构建一次。
在共享宿主上这本身就会引入漂移（§10.12），而交错采样（ON,OFF,ON,OFF,…）是让漂移对两臂
对称的唯一办法——只要切换需要重新构建，交错就无从谈起。

故新增 boot 参数 **`a20.anonprov=<pages>`**，扫描方式沿用 `mm/wx.c` 处理 `a20.wx=` 的
既有风格（`kernel/mm/pt.c` 的 `mm_pt_anon_prov_init()`，由 `main.c` 在 `bootargs_init()`
之后调用，与 `mm_wx_policy_init()` 相邻）。默认值仍是 `MM_ANON_PROVISION_MAX_PAGES`，
`a20.anonprov=0` 关闭预标记。

**已验证**（riscv64，`-append "a20.anonprov=N"` 经 DTB `/chosen/bootargs` 进入内核）：

| 参数 | `mm_anon_provisioned` | 关机审计 |
|---|---|---|
| `a20.anonprov=4096` | 9277 | 全 0 |
| `a20.anonprov=0` | **0** | 全 0 |

两臂均无参数解析告警，MM-ASM 审计 `missing_meta/present/absent/prot/cow/vma/anon_virt`
全 0；`smoke-mm-stress`、`smoke-mm-fork-exec-race`、`check-mm-lock-model` 通过；
riscv64/x86_64/aarch64 与 riscv64/aarch64 NOMMU 构建通过。

**x86_64 上该参数目前无效，且这是既有的独立缺口**：`arch_bootargs_get()` 在 x86_64 由
`kernel/platform/qemu-virt-x86_64/board.c` 实现，读的是 QEMU **fw_cfg** 的
`opt/cmdline`。实测无论用 `-append` 还是
`-fw_cfg name=opt/cmdline,string=...`，内核都只看到
`[FW_CFG] cmdline_size=0`、`cmdline=''`，即命令行根本没进内核。后果是 **`a20.wx=`
在 x86_64 上也被静默忽略**，而 W^X 是安全相关策略——这比本次新增的参数严重得多，
应单独修（需要弄清 fw_cfg 的 cmdline 文件为何在该启动路径下未被填充）。
在此之前，x86_64 上的交错 A/B 仍不可行；riscv64 上可行。

### 10.14 交错 A/B 的结果：预标记让 mmap 慢约 14%，且买不到任何东西

用 §10.13 的运行时开关做**交错**采样（单次构建，6 对交替 ON,OFF,ON,OFF,…，
riscv64/TCG + `taskset -c 12-15`）。因为每对的 ON 与 OFF 在时间上紧邻，逐对比值
可以抵消 §10.12 那个「臂间漂移」：

| 基准 | 逐对 OFF/ON 中位 | OFF 更快出现在 | 判定 |
|---|---|---|---|
| **mmap low** | **0.863** | **6/6 对** | **有效应：预标记使 mmap 慢约 14%** |
| **mmap high** | **0.867** | **5/6 对** | 有效应：约 13% |
| PF low | 0.922 | 4/6 对 | 无显著效应 |
| PF high | 0.97 | — | 无效应 |
| mmap-PF low / high | 1.09 / 0.94 | — | 无效应 |
| unmap low / high | 0.993 / 1.01 | — | 无效应 |
| unmap-virt low / high | 1.00 / 0.98 | — | 无效应 |

10 项里 8 项落在 0.90–1.11（无效应），只有 mmap 两项明确落在带外，且 mmap low 是
**6/6 对同向**（符号检验 p≈0.016）。这是本项目里第一份方法学可信的性能结论。

**据此撤回三条此前结论**：

1. §10.1「mmap 慢 1.5×」——**量级错了**。真实成本约 14%，不是 150%。
2. §10.8「预标记让 PF 快 28%」——**不存在**。PF 逐对比值 0.92/0.97，方向不一致，
   属噪声；当时那个 28% 是顺序 A/B 的臂间漂移造出来的。
3. §10.8「关掉预标记 mmap 没变快（无效应）」——**也错了**。当时被噪声完全掩盖；
   交错之后效应清晰可见。

**净结论（这是决策依据）**：预标记当前让 mmap 慢约 14%，而**它服务的消费者
（按状态缺页路径）仍是死代码**（§10.6，`mm_fault_from_status` 恒为 0）。也就是��
**付出 14% 的 mmap 成本，换来零收益**——因为唯一能消费这些状态标记的 fault 路径从未
执行过（§10.7 还说明即便修好查询，它的位置也绕过了 userfaultfd/共享/fault-around）。

因此在修好状态路径之前，**预标记应当关闭**（默认 `a20.anonprov=0`），这是一个可以
立刻回收 14% mmap 性能、且不损失任何现有功能的改动。它与 §10.7 记录的依赖顺序不冲突：
预标记是那套设计的地基，但地基单独存在时是净负担。

### 10.15 把预标记默认改为关闭，回收 14% mmap

按 §10.14 的结论落地：预标记现在**默认关闭**，只有显式给出 `a20.anonprov=<pages>`
才会启用（`g_anon_prov_max` 初值改为 0）。

这一步不丢任何东西——地基（`mm_pt_provision_anon()` 及其在 mmap / ELF 匿名段 / brk
三处的接线）全部保留在树里，开销只是那一个 boot 参数。改的只是**默认值**。

**验证**（riscv64）：

| 启动参数 | `mm_anon_provisioned` | MM-ASM 审计 | `mm_stress` |
|---|---|---|---|
| 无（默认） | **0** | 全 0 | PASS |
| `a20.anonprov=4096` | 9277 | 全 0 | PASS |

两臂均无参数解析告警；`smoke-mm-stress`、`smoke-mm-fork-exec-race`、
`check-mm-lock-model` 通过；riscv64 / x86_64 / aarch64 与 riscv64 NOMMU 构建通过。

**一个过程教训**：第一次验证 opt-in 时我把 `-append` 的值放在未加引号的 shell 变量里，
且带一个前导空格，QEMU 于是把 `a20.anonprov=4096` 当成磁盘镜像路径而直接退出，日志只有
一行 `Could not open ...`。当时那个 `stress-pass: 0` 看起来像内核回归，其实是**我的测试
脚本 bug**。这与 §10.8 那次「误把热页当缺页」是同一类错误：把工具链/脚本故障当成被测
系统的结论。**在把一次失败归因于被测代码之前，先确认测试装置本身是对的。**

### 10.16 PT 页 UAF：拆表点逐个审计（修复的前置条件）

UAF 本身：`pt_unmap()` / `pt_unmap_leaf()` 的拆表循环用
`mm_pt_node_fini(child); frame_free(child);` **同步释放** PT 页，而
`mm_addrspace_lock()` 只锁覆盖节点、**后代是无锁遍历的**——另一 CPU 上正沿该后代下降的
cursor 可能在帧被 buddy 回收后继续读它。`mm_pt_defer_free()` /
`mm_pt_mark_stale_recursive()` 没有任何调用者，`vm.c` 排空循环等的是一条永远不会被
填充的链表。

修复必须接上生产端（拆表时先标记 stale 再交给排空），但前提是**每个拆表点都在
`mm->lock` + TLB 事务内**，否则 `mm_pt_hold_table()` 会无锁改写 `mm->tlb_holds`，
且没有事务就永远没人排空。逐点审计结果：

| 拆表点 | 所在函数 | `mm->lock` | TLB 事务 | 结论 |
|---|---|---|---|---|
| `munmap.c:111` | `mm_munmap_locked` | 是（`mm_munmap` 287） | 是（287/291） | **安全** |
| `munmap.c:241` | `mm_brk_locked` | 是（`mm_brk` 299） | 是（298/302） | **安全** |
| `madvise.c:68/86` | `mm_madvise_dontneed` | 是（55） | 是（54/99） | **安全** |
| `mprotect.c:103` | `mm_mprotect_locked` | 是（162） | 是（161/165） | **安全** |
| `vm.c:421` | `mm_demote_huge_page` | 视调用者 | 视调用者 | **混合，见下** |
| `vma.c:466` | `free_vma_pages` | 否 | 是（`vm.c:545`） | 缺 `mm->lock` |
| `vma.c:442` | `mm_demote_huge_page` 的调用点 | 否 | 否 | **不安全** |
| `sysv_shm.c:80` | `sysv_shm_unmap_attached_pages` | 否 | 否 | **不安全** |
| `mremap.c:184` | `mm_move_mapping_pages` | — | — | `__attribute__((unused))` 死代码 |

**关键的非显然结论**：`mm_demote_huge_page()` 自身**不能**被当作安全点——它的调用者
安全性不一致（`munmap.c:90/232`、`mprotect.c:103` 安全，而 `vma.c:442` 既不持锁也不在
事务内）。所以不能只在 `mm_demote_huge_page()` 内部加 `mm_pt_defer_free()`，否则从
`vma.c:442` 进来时依旧是无锁改链表。要么把 `vma.c:442` 的调用者补齐锁与事务，要么让
拆表本身**不依赖** TLB 事务（例如独立的、由 `mm->lock` 保护的回收队列 + 在
`mm_tlb_invalidate_finish()` 之外也能排空的路径）。

**建议方向**：优先后者。理由是 TLB 事务的语义是「本事务内我改了哪些地址，需要
shootdown」，而 PT 页回收的语义是「这个节点已不可达，等读侧退出再释放」，两者本来
不该耦合。把它塞进 TLB 事务导致**每个拆表调用点都必须开事务**，这是不必要的耦合，也是
本次修复差点不安全的根因。独立队列只需 `mm->lock` 保护链表 + 一个可在任意上下文
调用��排空点。

### 10.17 修好 PT 页 UAF：回收与 TLB 事务解耦

UAF 一直存在：`pt_unmap()` / `pt_unmap_leaf()` 的拆表循环用
`mm_pt_node_fini(child); frame_free(child);` **同步释放** PT 页，而
`mm_addrspace_lock()` 只锁覆盖节点、**后代是无锁遍历的**——另一 CPU 上正沿该后代下降的
cursor，可能在该帧被 buddy 回收后继续读它。`mm_pt_defer_free()` /
`mm_pt_mark_stale_recursive()` 此前**没有任何调用者**，排空循环等的是一条永远不会被
填充的链表。

**为什么上次接不上**：把 `mm_pt_defer_free()` 塞进 `pt_unmap*` 内部，要求每个拆表点都在
`mm->lock` + TLB 事务内，但 8 个调用点里有 6 个两者皆无（`vma.c:466`、
`vma.c:442`、`sysv_shm.c:80` …），于是只能无锁改写 `mm->tlb_holds`——那版已回退。
根因是**把两件无关的事耦合了**：TLB 事务的语义是「本事务改了哪些地址、需要
shootdown」，而 PT 页回收的语义是「该节点已不可达、等读侧退出再释放」。

**本次修法**：`mm_struct` 新增 `pt_retire` 链表与**自己的** `pt_retire_lock`，
与 TLB 事务彻底无关，因此**任何上下文都能调用**——不需要 `mm->lock`，也不需要开事务。
`mm_pt_retire_table()` 做三件事：

1. `mm_pt_mark_stale_recursive()`——让缓存了该节点的 cursor 观察到 `stale` 并重下降
   （`mm_addrspace_lock` 本来就在下降途中 `mcs_lock` 之后和加锁之前各查一次）；
2. **不**立即释放：帧保持 `FRAME_F_PT`，所以 buddy 不会把它发给别人；元数据也留到
   排空时才丢，这样「已脱离」的状态由 `stale` 表达，而不是靠元数据消失来暗示；
3. 入队后立刻尝试排空——若此刻 `mm->pt_readers == 0` 就当场回收，否则留给下一次
   retire 或 `mm_destroy` 的兜底排空。

**为什么这个宽限期是成立的**：`mm_addrspace_lock()` 在**下降读任何 PT 项之前**就
`mm_pt_read_enter()`，且每层只读一次父项。于是父项被清零之后，**新的** cursor 不可能
再到达该页；而已经持有它的 cursor 一定被计入 `pt_readers`。所以「等 `pt_readers` 归零」
正好覆盖窗口。（注意这修的是**帧回收**这一半；`pt_unmap_leaf()` 的裸遍历与 cursor
下降之间对 PTE 本身的无锁竞争是另一件事，本次未处理。）

**验证**：riscv64 / x86_64 / aarch64 / loongarch64 / ppc64le 与
riscv64 / aarch64 / x86_64 的 NOMMU 全部构建通过；`smoke-mm-stress`、
`smoke-mm-fork-exec-race`、`check-mm-lock-model` 通过，审计
`missing_meta/present/absent/prot/cow/vma/anon_virt` 全 0。关键证据是
**`pt_pages=6` 与改动前一致**，且在 churn 最猛的 `smoke-mm-fork-exec-race` 下同样为 6
——若退役的帧没被真正回收，这个计数会随拆表次数上升。

`mm_destroy()` 在 VMA 拆除之后、`mm_destroy` 早期补一次 `mm_pt_retire_drain()`：此时
最终引用已归零、地址空间无法再被进入，`pt_readers` 必为 0，一次即可排空，不会泄漏。

**未能验证**：arm32 在本环境**改动前就无法构建**（`arm-linux-gnueabihf-gcc` 不在
PATH），其自有的一份 `pt_unmap` / `pt_unmap_leaf` 已同步改签名并从 `mm->pgdir` 取
pgdir，但编译未经检验。

### 10.18 给 per-PTE 状态补上安全语义位（状态路径的前置条件之一）

§10.7 记录了状态路径的位置缺陷：它在 `mm_find_vma` **之前**返回，因而绕过
userfaultfd、`VM_SHARED` 与 fault-around 的安全门。论文的设想是用 per-PTE 状态
**取代** VMA 成为权威来源，前提是状态本身能表达这些判定——而原来的状态字节做不到：
8 位全部分配完毕（4 位 class + COW + 3 位 prot），且**共享性其实已经编码在 class 里**
（`MM_ST_ANON_SHARED` / `MM_ST_FILE_SHARED`），所以并不是「没有地方放」，而是
「剩下的两项判定确实放不下」。

本次为此新增两项，并按既有 `cow[]` 的写法做成**并行位图**而不是把 `cls[]` 扩成
16 位/项（后者会把每项元数据翻倍）：

* `MM_SAFE_UFFD`——该项被 userfaultfd 区间覆盖。缺页必须停住交给 handler，
  **绝不能**直接造零页满足。
* `MM_SAFE_NO_FA`——该项不得被多页 fault-around 覆盖（VMA 已被 seal，或该 class 下
  投机分配会改变语义）。

`pt_meta_t` 新增 `safe[(MM_PT_META_ENTRIES + 7) / 8]`；`mm_pt_node_init()` 本来就
`memset` 整个结构，故新位图天然归零。

**一个必须做对的细节**：`mm_pt_note_absent()` 原本只清 cow 位。若不同步清 `safe`，
被复用的槽位会**继承**上一条映射的 UFFD/NO_FA 标志，状态路径据此做出错误判定。所以
清槽位时一并 `&= ~MM_SAFE_MASK`。同理 `mm_pt_safe_set()` 拒绝在 class 为
`MM_ST_INVALID` 的槽位上置位——无映射的槽位上的安全位没有意义，也永远不会被清掉。

提供 `mm_pt_safe_set/clear/test()` 三个粒度为「一位」的接口，使任何改动槽位的代码
都必须经由它们，从而保证这些位不会活得比它所描述的 class 更久。

**当前状态**：这些位**已就位但尚未被任何代码写入**——真正的赋值方是 §10.7 依赖链里的
「mmap/mprotect/munmap/madvise + userfaultfd 注册全部接入状态更新」那一步。因此状态
缺页路径仍保持 inert（`mm_fault_from_status` 仍恒为 0），行为零变化。

验证：riscv64 / x86_64 / aarch64 / loongarch64 / ppc64le 与 riscv64 / aarch64 / x86_64 的
NOMMU 全部构建通过；`smoke-mm-stress`、`smoke-mm-fork-exec-race`、`check-mm-lock-model`
通过，审计 `missing_meta/present/absent/prot/cow/vma/anon_virt` 全 0，`pt_pages=6` 不变。

### 10.19 把安全位接到真正会改变安全语义的 syscall 上

§10.18 只是把位加出来了。这一步把它们接到真正的写入方。

新增 `mm_pt_set_safe_range(mm, start, end, flags, set)`：按 leaf 逐块遍历
（像 madvise 那样把查表摊销掉，而不是每页查一次），`set==0` 时清除。

**mseal**（`kernel/mm/mseal.c`，`mm_mseal_locked` 内，调用方已持 `mm->lock`）：
对每个被 seal 的 VMA 交集 `[a, b)` 置 `MM_SAFE_NO_FA`。seal 存在的原因是冻结这段
状态，而多页 fault-around 会**投机地**给邻居装帧——那正是 seal 要禁止的副作用。

**userfaultfd UFFDIO_REGISTER**（`kernel/ipc/userfaultfd.c`）：在 range 真正挂上
`uffd->ranges` **之后**、`g_uffd_lock` 释放之后，置 `MM_SAFE_UFFD`。放在成功之后而非
校验循环里，是因为校验失败（覆盖不足 / 与既有注册重叠）时 range 并不存在，那时就标位
等于凭空造出一个「已被注册」的页面。

**userfaultfd UFFDIO_UNREGISTER**：清掉**实际移除**的那段（记录移除 range 的并集，
而不是直接用请求范围），同样在 `g_uffd_lock` 之外、`mm->lock` 之下做页表遍历，避免
在全局 range 锁里嵌一次页表走表。

**一处必须写明的保守之处**：同一页面原则上可被**另一个** uffd 的注册覆盖，所以 unregister
这里的清除会**清得比严格需要的多**。今天这是安全的——VMA 缺页路径上真正的权威判定是
`userfaultfd_range_present()`，本次完全没动它，残留的位也不会让缺页跳过 handler。
但**在状态缺页路径启用之前，这必须改成逐页复查 presence 后再清**，否则「清多了」会让
仍被注册的页面不经停靠直接缺页成功。

**当前状态**：这些位**已写入但仍无人读取**——读取方是尚未启用的状态缺页路径
（`mm_fault_from_status` 仍恒为 0），所以行为零变化。这是纯铺垫。

**尚未完成**：todo 里与本项并列的「在 MM-ASM auditor 里加 status-vs-VMA 一致性检查」
还没做。启用状态路径之前必须有它，否则无法证明状态与 VMA 等价。

验证：riscv64 / x86_64 / aarch64 / loongarch64 / ppc64le 与 riscv64 / aarch64 / x86_64
的 NOMMU 全部构建通过；`smoke-mm-stress`、`smoke-mm-fork-exec-race`、
`check-mm-lock-model` 通过，审计全 0，`pt_pages=6` 不变。

### 10.20 auditor：把 seal ↔ MM_SAFE_NO_FA 变成机器可检的不变量

§10.19 接好了 mseal → `MM_SAFE_NO_FA`，但「看起来对」不算证据。`mm_pt_audit()` 里原本已有
VMA 交叉检查（`vma_mismatch`：每个 VMA 至少要有一页被元数据认识），现在按同样的做法再加
一条**双向**不变量：

对每个 VMA 的每一页，若该页**已映射**（class 不是 `MM_ST_INVALID`），则
`MM_SAFE_NO_FA` 必须与 `VM_SEALED` 一致——seal 住的区间若仍能被 fault-around 拉进来，
seal 就失效了；反过来标了却没 seal，则是无谓地关掉了 fault-around。任何一边漏更新，
都会在关机审计里变成 `safe=N`，而不是等到某次投机分配悄悄改了被冻结的状态。

新计数项 `safe_mismatch` 已并入 `mm_pt_audit_errors()`，所以它**会让审计失败**，
不是只打印一个数字。审计行现在多一个 `safe=` 字段：
`missing_meta=0 present=0 absent=0 prot=0 cow=0 vma=0 safe=0 anon_virt=0`。

**`MM_SAFE_UFFD` 故意不查**：UFFDIO_UNREGISTER 清的是「实际移除的 range 的并集」，
一个仍被**另一个** uffd 注册覆盖的页面会丢掉标记，于是「UFFD 位 ⟺ 被注册」今天根本不是
不变量，拿它做检查必然报假错。权威判定始终是 VMA 缺页路径上的
`userfaultfd_range_present()`（本次未动）。等状态缺页路径真要启用、且 unregister 改成
逐页复查 presence 之后再清（§10.19 已标注），才把 UFFD 位纳入审计。

**这条检查不是空跑**：`mm_stress.c` 里有 23 处 mseal 调用，`smoke-mm-stress` 真的会
seal 区间，所以 `safe=0` 是**跑过之后**的一致性结果，不是「没有 seal 所以没得查」。

验证：5 个架构与 3 个 NOMMU 变体构建通过；`smoke-mm-stress`、
`smoke-mm-fork-exec-race`、`check-mm-lock-model` 通过，审计全 0（含新增的 `safe=0`），
`pt_pages=6` 不变。

### 10.21 状态缺页路径：查询修复 + UFFD 门控（默认仍然关闭）

两处改动，**默认配置下是 no-op**：

1. `mm_cursor_query()` 对**缺失**叶子不再直接返回而把 `cls_out` 留在 `INVALID`——它现在
   报告元数据里记下的 class。这正是 §10.6 判定「路径从未执行」的那一行。
2. fault.c 状态路径加 `MM_SAFE_UFFD` 门：该项若被 userfaultfd 注册，**必须**回落到
   VMA 路径停靠给 handler，不能就地满足。辅助接口 `mm_cursor_safe_test()` 让调用方在
   已持有覆盖节点锁的情况下问「这一项是不是被注册了」，不必再下降一次。

**为什么现在可以安全地打开这条路径**（对照 §10.7 列的三条绕过）：

* *绕过 userfaultfd* → 已由 `MM_SAFE_UFFD` 挡住，且该位会写到「已预留未缺页」的项上
  （class 是 `ANON_VIRT` 而非 `INVALID`，所以 `mm_pt_safe_set()` 不会拒）。
* *绕过 `VM_SHARED`* → 天然不成立：状态路径只接受 class `ANON_VIRT`，而共享映射的
  class 是 `MM_ST_ANON_SHARED` / `MM_ST_FILE_SHARED`，永远不是 `ANON_VIRT`。
* *绕过 fault-around* → 天然不成立：状态路径一次只映射一页，根本不做 fault-around。

**默认关闭的原因正是预标记默认关闭**：没有 `ANON_VIRT` 项，状态路径就永远不会命中。
实测 `a20.anonprov=0`（默认）：`mm_anon_provisioned=0`、`mm_fault_from_status=0`、
`mm_anon_faults=672`、`mm_demand_faults=1484`，mm_stress PASS，**无 FATAL**。

**打开时确实会崩——(b) 依然存在**。`a20.anonprov=4096`：

```
mm_demand_faults      1484
mm_anon_faults         672
mm_fault_from_status      0     <- 关闭时
a20.anonprov=4096:  FATAL, mm_stress 未通过
```

崩溃签名（`a20.anonprov=4096`，稳定复现）：

```
[FAULT-VA] stval=0x807000
  leaf: base=0x807000 pa=0xffde4000 pfn=523748 flags=0x7 refs=1
  page_words: 00000000 00000000 00000000 00000000
  va_words:   全 0
  mm: brk=0x808000 start_brk=0x806000 stack=[0x3ff3d000,0x3ff5d000)
  vma=[0x807000,0x808000) flags=0x13 pte_flags=0xd7 file_fd=-1 off=0x1000
  regs: a0=0x806000 a1=0x1000   status=0x200004020
```

**尚未解释的疑点**：出错的页在转储里是**已映射且 `flags=0x7`（RWX）**，内容全 0，但
CPU 仍在它上面 fault（`status` 低位指向取指/访问类异常）。「PTE 看起来可访问却仍 fault」
本身是矛盾的，所以要么转储读到的 PTE 不是 fault 当时的状态，要么存在另一条使该映射
无效的路径。**根因未定位**，需要继续查；在定位之前不得把预标记默认打开。

另注：brk VMA 上的 `off=0x1000` 对匿名堆来说没有意义（brk 段的 `vm_pgoff` 应当为 0），
也是一个待解释的异常。

验证：riscv64 / x86_64 / aarch64 构建通过；`smoke-mm-stress`、
`smoke-mm-fork-exec-race` 通过，审计全 0（含 `safe=0`），`pt_pages=6` 不变；
默认配置下 `mm_fault_from_status` 为 0，行为与改动前完全一致。

### 10.22 定位 (b)：二分法，结论是 **mmap 预标记**那条路径

崩溃只在「预标记开 + 状态路径可用」时出现，且 `a20.anonprov=0`（默认）时完全不崩
（`mm_fault_from_status=0`、stress PASS、无 FATAL）。所以把范围收窄到：三个预标记调用点
里哪一个在喂出坏状态。

**先记一次我自己的方法错误**：前三次二分每次只关掉**一个**调用点、另两个仍然开着，
所以那三次「关掉 brk 仍崩 / 关掉 ELF 仍崩 / 关掉 mmap 仍崩」**全都无效**——它们只说明
「单独关掉某一个不足以消除崩溃」，不能推出「那个是无辜的」。我在这三次之后才发现自己
每次都把前一个改动 `git checkout` 还原了，等于每次都在测「关掉一个、留着两个」。
这与 §10.8「把热页当缺页」、§10.15「把脚本 bug 当内核回归」是同一类错误：**在把结论
归因到某个组件之前，先确认实验装置真的只变了那一个变量。**

**正确的二分**（关掉 ELF + brk、只留 mmap，`a20.anonprov=4096`）：

| 配置 | 结果 |
|---|---|
| 三个全开 | FATAL |
| 关 brk（ELF+mmap 仍开） | FATAL（无效实验） |
| 关 ELF（brk+mmap 仍开） | FATAL（无效实验） |
| 关 mmap（ELF+brk 仍开） | FATAL（无效实验） |
| **只留 mmap（ELF+brk 关）** | **FATAL ← 复现** |
| 三个全关 / 默认 `anonprov=0` | 无 FATAL |

结论：**(b) 由 mmap 路径的预标记喂出**，与 ELF 段、栈/TLS、brk 无关。这也说明它不是
「某个 VMA 分类错了」那类问题——那正是 ELF 的教训（§10.17）——而是 mmap 这条
`mm_pt_provision_anon(mm, addr, addr + len, ptef)` 调用本身有问题。

**尚未解释**：崩溃点仍落在 **brk** 地址上（`stval=0x807000`，`vma=[0x807000,0x808000)
flags=0x13 pte_flags=0xd7 file_fd=-1`），而 brk 预标记此时是**关闭**的——状态路径不可能
直接处理它。所以 mmap 预标记是通过**别的途径**间接破坏的（页表被写坏、某个帧被提前回收、
或统计/引用计数失衡），而不是在 brk 上命中。这条线索指向
`mm_pt_provision_anon()` 内部：它按 leaf table 为单位 `mm_addrspace_lock()` 后
`mm_pt_note_present()` 逐项写状态，**与 §10.17 新加的 `mm_pt_retire_table()` / 宽限期回收
、以及 `mm_pt_note_absent()` 里新增的清 safe 位都是同一批元数据写者**。

**下一步应当做的具体检查**（未做，根因未定位）：
1. 在 `mm_pt_provision_anon()` 里对 `[lo,hi)` 逐 leaf 校验 `table[idx]` 确实为 0，
   遇到非 0 就计数上报——预标记**只应**写未被映射的槽位。
2. 崩溃后 dump 出错页的 PT 页 `pfa.meta[pfn].pt` 是否已 `NULL`/`FRAME_F_PT` 被清，
   以区分「页被提前回收」与「页表被写坏」。
3. 确认 mmap 的 `ptef` 经 `status_byte()` 往返后与 VMA 实际权限一致（对照 §10.14 之外的
   `mm_pt_prot_bits()` 编码宽度）。

在 (b) 定位之前，**预标记必须保持默认关闭**，状态缺页路径因此仍然 inert。

### 10.23 一条死路：nr_present 溢出不是 (b) 的原因

预标记会把 `m->nr_present` 往上推（`pt_note_present_meta()` 每个槽位 `++`），而它是
`uint16_t`。既然 (b) 由 mmap 预标记喂出（§10.22），最自然的怀疑就是它在某个 leaf table
上累加到 65536 之后回绕。**查证结果：不是。**

`pt_table_empty()`（决定某个中间页表页能否被拆掉回收的唯一判据）**直接扫 PTE 数组**，
根本不看 `nr_present`：

```c
static int pt_table_empty(pte_t *table, int level) {
    for (int i = 0; i < entries; i++)
        if ((table[i] & PTE_V) || pte_is_swap(table[i])) return 0;
    return 1;
}
```

`nr_present` 的全部使用点只有三处：自增、自减、fork 时整体拷贝
（`pt_clone_level()` 里 `dst->nr_present = src->nr_present`）。**没有任何控制流依赖它**，
所以即使回绕也只是一个显示用的计数失真，不会导致「非空页表被当成空表回收」这类损坏。

**记下来是为了避免有人重复走这条路。**

**目前最值得试的下一步**（按可能性排序，均未做）：
1. **状态路径缺 TLB 事务**。状态路径映射成功后只做
   `arch_tlb_flush_page_local(stval)`，而 VMA 路径走的是
   `mm_tlb_invalidate_begin/finish` + `mm_tlb_note_change` 的完整事务。在
   `-smp 2` 下，如果同一个地址空间此刻在**另一个 CPU** 上有残留翻译，本地刷新不足以
   让它看到新映射。这与症状吻合：出错页在转储里 `flags=0x7` 可访问、内容全 0，但发起
   访问的 CPU 手里仍是旧的（无效）翻译。可用 `-smp 1` 复跑一次来**证伪或证实**——
   若 `-smp 1` 不崩，基本锁定这一条。
2. `mm_pt_provision_anon()` 的 chunk 循环里 `cur.path[0]` 是否在 `lo` 跨 leaf 边界时
   取到了错误的叶子表（`anchor` 取自 `lo`，但 `hi` 可能延伸进下一张表）。
3. mmap 的 `ptef` 经 `status_byte()` 往返后是否与 VMA 实际权限一致（对照
   `mm_pt_prot_bits()` 的编码宽度）。

在 (b) 定位之前，预标记保持默认关闭，状态缺页路径保持 inert。

### 10.24 证伪 TLB 假设，并把搜索空间砍半

§10.23 列的第一条假设是「状态路径只做 `arch_tlb_flush_page_local()`、没走完整 TLB 事务，
`-smp 2` 下另一 CPU 可能持有残留翻译」。**用 `-smp 1` 复跑证伪**：

| 配置 | `a20.anonprov=4096` |
|---|---|
| `-smp 2` | FATAL |
| `-smp 1` | FATAL（同样崩） |

单 CPU 下不存在「另一 CPU 持有残留翻译」这回事，所以这条**不成立**：(b) 与并发 TLB
一致性无关，是个单线程下就能触发的逻辑错误。

**由此得到一个比之前更有价值的收窄。** 对照 `d1fb5c22`（预标记默认关闭、状态路径仍
inert 的那次提交）当时的实测：`a20.anonprov=4096` 下 `mm_anon_provisioned=9277`、
关机审计全 0、`mm_stress` **PASS**。也就是说——

> **mmap 预标记本身是安全的**。崩溃只在状态路径**真的去消费**这些预标记条目时才出现。

这把嫌疑从「预标记写坏了状态」收缩到「状态路径消费 mmap 预标记条目的那段逻辑」，
也就是 `fault.c` 里这一段：

```c
pfn_t np = pfa_alloc_page();
if (np != PFN_NONE) {
    if (cg_mem_charge(t->cgroup, 1) == 0) {
        memset(pfn_to_virt(np), 0, PAGE_SIZE);
        if (mm_cursor_map(&qcur, page_va, pfn_to_phys(np), allow,
                          MM_ST_ANON_MAPPED) == 0) { ... return 0; }
        cg_mem_uncharge(t->cgroup, 1);
    }
    frame_put(np);
}
```

与 VMA 路径逐项对照，**最可疑的差异**是：状态路径**没有参与 TLB 事务**——既没有
`mm_tlb_invalidate_begin/finish`，也没有 `mm_tlb_note_change()`。VMA 路径经
`fault_map()` 完成映射，会在事务内登记地址变化并在结束时统一 shootdown。状态路径只在
成功分支调一次本地刷新就 `return 0`，**绕过了 `mm_tlb_invalidate_finish()`**。

单 CPU 下这依然可能出错：事务不只服务于跨核 shootdown，它还承担「本事务内所有改动在
返回用户态前必须对当前 CPU 生效」这一职责。`mm_tlb_invalidate_finish()` 结尾会做
`arch_tlb_flush()` / generation 推进；直接 `return 0` 可能让**当前** CPU 在
satp/ASID 或 TLB 状态未收尾时继续执行。

**下一步（具体、可执行）**：把状态路径的映射纳入 TLB 事务——进入前
`mm_tlb_invalidate_begin(mm)`，成功与失败的所有出口都
`mm_tlb_invalidate_finish(mm)`，并用 `mm_tlb_note_change(mm, page_va, PAGE_SIZE)`
替代裸的 `arch_tlb_flush_page_local()`。做完直接用 `-smp 1` 复跑：
崩→假设成立，可继续收窄；不崩→该假设也被证伪，换查 `mm_cursor_map` 与
`fault_map` 在「安装并记账」上的其他差异（例如 `mm->rss` 与 cgroup 记账是否重复）。

**在 (b) 定位之前，预标记保持默认关闭，状态缺页路径保持 inert**，默认配置行为与改动前
完全一致（`mm_fault_from_status=0`，全部门通过）。

### 10.25 再证伪一条：状态路径并不缺 TLB 事务（更正 §10.24 的首选假设）

§10.24 把「状态路径没有参与 TLB 事务」列为第一嫌疑。**这条也是错的**，而且是
「先下结论、后看代码」的又一次。`fault_map()` 才是 VMA 路径真正用来装映射的函数，它
的结构与状态路径**完全一致**：

```c
static int fault_map(mm_struct_t *mm, vaddr_t page_va, pfn_t pfn, pte_t flags,
                     uint8_t cls)
{
    mm_cursor_t cur;
    int r = mm_addrspace_lock(mm, page_va, page_va + PAGE_SIZE, &cur);
    if (r != 0) return r < 0 ? r : -EFAULT;
    r = mm_cursor_map(&cur, page_va, pfn_to_phys(pfn), flags, cls);
    mm_cursor_unlock(&cur);
    return r;
}
```

`mm_addrspace_lock` → `mm_cursor_map` → `mm_cursor_unlock`，**没有开事务**。VMA 路径的事务
是由更外层（系统调用层 / `handle_demand_fault` 的调用者）开的，不是 `fault_map` 开的。
把 `fault_map` 和匿名 VMA 路径的后续记账（`rss++`、`a20_perf_count`、
`arch_tlb_flush_page_local`）逐行对比状态路径，**看不出实质差异**。

顺带说明：假如真要给状态路径开事务，还要小心 `mm_tlb_invalidate_finish()` 内部会
`spin_lock_irqsave(&mm->lock)` 取排空链表，而缺页路径此刻**已持 `mm->lock`**——直接加
事务会自锁。事务必须像 `mm_munmap` 那样「开事务 → 加锁干活 → 解锁 → finish」成对使用。
这条也说明「照搬事务」不是无风险的补丁。

**至此三条假设全部被证伪**（§10.23 `nr_present` 溢出、§10.24 跨核 TLB 残留、本节事务缺失），
而唯一确定的事实仍是 §10.22/§10.24 的收窄：

* mmap 预标记**单独**是安全的（`d1fb5c22` 实测：9277 页、审计全 0、stress PASS）；
* 崩溃只在状态路径**真的消费** mmap 预标记条目时出现；
* 且崩溃点是 **brk** 地址，而 brk 预标记在复现配置里是**关闭**的——状态路径不可能
  直接处理它。

最后这条是关键矛盾：状态路径够不到 brk，却能间接把 brk 搞坏。这说明破坏发生在
**别处被状态路径写坏的东西**上。仍存活的候选（本轮未验证）：

1. `mm_pt_retire_table()` 的宽限期回收（§10.17 新增）——它是本轮之前不存在的新写入者，
   而 §10.24 用来证明「预标记本身安全」的那次实测（`d1fb5c22`）**早于**这个改动。
   **这条现在应该排第一**：需要重做那个安全性验证（预标记开 + 状态路径 inert），
   看是否仍 PASS。
2. 状态路径在 `mm_cursor_map` 之外还改了哪些共享记账（`mm->rss`、cgroup charge），
   是否与随后 VMA 路径的处理重复记账或漏记。

**下一步应当先做 1**：在 `323664fc` 之后重跑「`a20.anonprov=4096` + 查询修复回退」，
确认预标记在**当前**代码上单独仍然安全。这是一条能把 (b) 的嫌疑范围直接砍掉一半的实验，
而且不需要任何新代码。

在 (b) 定位之前，预标记保持默认关闭，状态缺页路径保持 inert，默认配置行为与改动前
完全一致（`mm_fault_from_status=0`，全部门通过）。

### 10.26 预标记本身仍然安全 —— 宽限期回收被排除，嫌疑收进状态路径内部

§10.25 指出：用来证明「mmap 预标记单独是安全」的那次实测（`d1fb5c22`）**早于** §10.17
新增的 `mm_pt_retire_table()` 宽限期回收，所以那条证据已经过期，必须在当前代码上重做。
做法不需要任何新代码：把 §10.21 的查询修复临时改回失效（状态路径再次 inert），保留
`a20.anonprov=4096` 打开预标记。

实测（riscv64，`a20.anonprov=4096`）：

```
mm_anon_provisioned: 9287
mm_fault_from_status: 0        <- 状态路径确认 inert
mm_anon_faults:        672
mm_demand_faults:     1484
stress-pass: 1   fatals: 0    <- 通过
```

**结论：`mm_pt_retire_table()` 与宽限期回收被排除。** 在包含 §10.17 那套回收机制、以及
§10.18/§10.19 的安全位与 mseal/uffd 接线的当前代码上，mmap 预标记单独运行仍然安全：
预标记了近 9300 页，审计干净，`mm_stress` 通过，零 FATAL。

于是嫌疑范围被压到最后一层：**崩溃只由状态路径消费 mmap 预标记条目这段逻辑本身引起**，
与预标记写入、与回收、与安全位都无关。

**但核心矛盾仍未解释**：复现配置里 brk 预标记是**关闭**的，状态路径够不到 brk 地址，
崩溃却落在 brk 上（`stval=0x807000`，`vma=[0x807000,0x808000) flags=0x13 pte_flags=0xd7
file_fd=-1`）。「够不到的代码把目标搞坏了」只能有一个解释：**状态路径在处理某个 mmap
区间时，破坏了 brk 路径后续依赖的某个共享不变式**。按可能性排序（本轮均未验证）：

1. **记账重复**。状态路径成功后 `mm->rss++`、并各自计了
   `A20_PERF_MM_ANON_FAULTS` / `MM_DEMAND_FAULTS`。而 brk 区间随后若走 VMA 路径再被
   处理一次，`rss` 或 cgroup 记账就会与实际帧数脱节；一旦有别处以 `rss`/`total_vm`
   为条件做判断，就会连锁出错。**先查这一条**：`mm_pte_flags_allow_access()` 之后
   是否漏了 VMA 路径会做、而状态路径没做的某个记账或检查。
2. `mm_cursor_map()` 在 class 已是 `MM_ST_ANON_VIRT` 的槽位上安装时，metadata 的
   `present/absent` 记账与 `mm_pt_note_absent()` 的清理是否自洽（`nr_present` 只是显示用
   计数，§10.23 已排除它参与控制流，但**配对关系**仍可能有错）。
3. 状态路径的 `return 0` 是否跳过了缺页函数尾部某些必须执行的收尾（`mm->rss` 之外的，
   例如 vma 引用、rss 记账的 vma 归属、或 fault-around 相关的统计）。

验证手段：把 §10.21 状态路径与 VMA 匿名路径（`fault.c` 中 `fault_map(t->mm, page_va,
pfn, vma->pte_flags, MM_ST_ANON_MAPPED)` 那一段）**逐行并列 diff**，只保留必要差异。
这两段在结构上应当等价（§10.25 已确认 `fault_map` 本身没有额外事务），任何不等价之处
就是嫌疑点。这是不需要猜测的一步。

在 (b) 定位之前，预标记保持默认关闭，状态缺页路径保持 inert，默认配置行为与改动前完全
一致（`mm_fault_from_status=0`，全部门通过）。

### 10.27 并列 diff 的结果：找到一处记账不等价

把状态路径与 VMA 匿名路径逐段并列后，找到一处**具体**的不等价（不是猜测）：

VMA 路径（`fault.c` 匿名/交换各段）在映射成功后**除** `mm->rss++` 之外，还会做四项
**每任务/全局**的缺页计数：

```c
t->mm->rss++;
arch_tlb_flush_page_local(stval);
__atomic_fetch_add(&t->perf_page_faults, 1, __ATOMIC_RELAXED);
__atomic_fetch_add(&t->perf_page_faults_maj, 1, __ATOMIC_RELAXED);
__atomic_fetch_add(&g_perf_sw_page_faults, 1, __ATOMIC_RELAXED);
__atomic_fetch_add(&g_perf_sw_page_faults_maj, 1, __ATOMIC_RELAXED);
```

而状态路径只做了：

```c
mm->rss++;
a20_perf_count(A20_PERF_MM_ANON_FAULTS);
a20_perf_count(A20_PERF_MM_DEMAND_FAULTS);
a20_perf_count(A20_PERF_MM_FAULT_FROM_STATUS);
arch_tlb_flush_page_local(stval);
```

**`t->perf_page_faults` / `perf_page_faults_maj` 与两个 `g_perf_sw_*` 完全没动。**
这本身未必致命（它们看起来只用于统计），但它是第一处**可指认**的差异，必须先补齐——
一个缺页路径不记缺页数，会让任何依赖该计数的判断（以及 `/proc` 上报的缺页率）失真。

**另一个结构性观察（非缺陷，但记录下来）**：VMA 路径在放锁分配页之后会**重新加锁并
重新校验**：

```c
if (prepared > 0 && cp && (*cp & PTE_V)) { /* 已被别人映射 -> 撤销 */ }
else if (mm_find_vma(t->mm, page_va) != vma) { /* VMA 已被换掉 -> 撤销 */ }
```

并全程持有 `vma_put(t->mm, vma)` 的引用。状态路径**没有也不该有**这一套——它全程持
`mm->lock`，不存在同样的 TOCTOU 窗口，也从不碰 VMA。这正是论文设想的好处，但代价是
它必须自己保证每一样 VMA 路径靠 VMA 拿到的东西（权限、记账、统计）都能从状态位里补齐。
**目前权限能补齐（`MM_ST_PROT_*` 往返），记账只补了一半，统计完全没补。**

**下一步（明确且小）**：把上述四项计数补进状态路径的成功分支，然后重跑
`a20.anonprov=4096`：
* 仍崩 → 记账不是原因，按 §10.26 候选 2/3 继续查 `mm_cursor_map` 的 metadata 配对与
  `return 0` 是否跳过收尾；
* 不崩 → (b) 的原因就是漏记这四项统计（很可能某个限额/回收策略读
  `t->perf_page_faults`，在 brk 增长密集的路径上被触发）。

这一条不需要任何猜测，且是当前唯一已指认的差异，应先做。

**当前安全状态**（重要）：预标记默认关闭，状态缺页路径保持 inert。默认配置下
`mm_fault_from_status=0`、审计全 0、`pt_pages=6` 不变，
`smoke-mm-stress` / `smoke-mm-fork-exec-race` / `check-mm-lock-model` 全通过，
5 个架构与 3 个 NOMMU 变体构建通过。**下述所有 (b) 排查都在 `a20.anonprov=4096` 下进行，
默认路径不受影响。**

### 10.28 补齐四项缺页统计：本身是对的，但**没有**修好 (b)

按 §10.27 的指认，把 VMA 路径那四项每任务/全局软缺页计数补进状态路径成功分支：

```c
__atomic_fetch_add(&t->perf_page_faults, 1, __ATOMIC_RELAXED);
__atomic_fetch_add(&t->perf_page_faults_maj, 1, __ATOMIC_RELAXED);
__atomic_fetch_add(&g_perf_sw_page_faults, 1, __ATOMIC_RELAXED);
__atomic_fetch_add(&g_perf_sw_page_faults_maj, 1, __ATOMIC_RELAXED);
```

`a20.anonprov=4096` 复跑：**仍然 FATAL**。所以 §10.26 候选 1（记账重复/缺失导致连锁出错）
**被证伪**。

但这个改动**本身是正确的、应当保留**：状态路径此前完全不记这四项，意味着任何走状态
路径的缺页都不会出现在 `t->perf_page_faults` 与 `/proc` 上报的缺页率里——这是与 VMA
路径的真实记账不一致，与 (b) 是否由它引起无关。保留它是因为它对，不是因为它治好了崩溃。

**至此 (b) 的五条假设全部被证伪**：

| 假设 | 出处 | 结果 |
|---|---|---|
| `nr_present` uint16 回绕 | §10.23 | 证伪（`pt_table_empty` 直接扫 PTE，不看该计数） |
| 跨核 TLB 残留翻译 | §10.24 | 证伪（`-smp 1` 同样崩） |
| 状态路径缺 TLB 事务 | §10.25 | 证伪（`fault_map` 结构完全相同，也不开事务） |
| 宽限期回收 `mm_pt_retire_table()` | §10.26 | 证伪（状态路径 inert + 预标记开 = 9287 页、stress PASS） |
| 漏记四项缺页统计 | §10.27 | 证伪（补齐后仍崩） |

**仍然确定的事实**（这些是实验结果，不是推断）：
1. mmap 预标记**单独**安全（当前代码、预标记开、状态路径 inert → stress PASS、审计全 0）。
2. 崩溃只在状态路径**真的消费** mmap 预标记条目时出现。
3. 崩溃点是 **brk** 地址，而复现配置里 brk 预标记**关闭**——状态路径够不到它。

第 3 条是核心矛盾，也是下一步唯一该盯的东西：**状态路径在处理 mmap 区间时，破坏了 brk
路径后续依赖的某个共享不变式。** 已排除记账与统计，剩下最可能的是页表/metadata 层面的
共享状态——具体说，`mm_cursor_map()` 把一个 `MM_ST_ANON_VIRT` 槽位变成已映射时，
`mm_pt_note_present()` 记的 `nr_present` 与之后 `mm_pt_note_absent()` 的清理、以及
`mm_cursor_unmap()` 清除 stale 标记这三条路径之间是否自洽。

**下一步应当做的具体实验**（仍不需猜测）：
在 `mm_pt_provision_anon()` 与 `mm_cursor_map()` 之后各加一个廉价的「不变式断言」计数器
——映射完成后立刻回读该槽位的 class/PTE，若出现「class 声称已映射但 PTE 无效」或
「PTE 有效但 class 仍是 ANON_VIRT」就计数；崩溃后再打印这个计数。计数非零即证明
metadata 与 PTE 失配，并直接指出是哪一次写入造成的。

**当前安全状态**（未变）：预标记默认关闭，状态缺页路径 inert。默认配置
`mm_fault_from_status=0`、审计全 0（含 `safe=0`）、`pt_pages=6` 不变，
`smoke-mm-stress` / `smoke-mm-fork-exec-race` 通过，riscv64/x86_64/aarch64 构建通过。

### 10.29 不变式断言：证明状态路径的映射本身是**正确**的（第六条假设被证伪）

按 §10.28 的实验设计，在状态路径 `mm_cursor_map()` 成功之后立刻回读该槽位，断言
「PTE 已置有效」且「class 已不再是 `MM_ST_ANON_VIRT`」；违例则计数
（新增 `A20_PERF_MM_STATUS_INVARIANT_BAD`）并在**首次**违例时 `kerr` 打印（因为随后的
崩溃会让任何计数器都读不到，所以必须当场出声）。游标的叶子表 `qcur.path[0]` 本来就在
手上，回读是一次已缓存指针的读，代价可忽略。

`a20.anonprov=4096` 实测：**违例 0 次**，而崩溃照旧。

**结论：`ANON_VIRT → 已映射` 这次状态转换每次都干净落地，metadata 与 PTE 始终自洽。**
也就是说状态缺页路径在「装映射」这件事上是正确的，(b) 不是它把页表写坏了。

**至此六条假设全部证伪**：

| 假设 | 结果 |
|---|---|
| `nr_present` uint16 回绕 | 证伪（`pt_table_empty` 不看该计数） |
| 跨核 TLB 残留翻译 | 证伪（`-smp 1` 同样崩） |
| 状态路径缺 TLB 事务 | 证伪（`fault_map` 结构相同，也不开事务） |
| 宽限期回收 `mm_pt_retire_table()` | 证伪（状态路径 inert + 预标记开 = stress PASS） |
| 漏记四项缺页统计 | 证伪（补齐后仍崩） |
| metadata 与 PTE 失配 | 证伪（回读断言 0 违例） |

**新出现的最强线索（来自崩溃转储里的寄存器，之前一直没被利用）**：

```
regs: a0=0x806000  a1=0x1000
mm:  brk=0x808000  start_brk=0x806000
vma= [0x807000,0x808000) flags=0x13 pte_flags=0xd7 file_fd=-1 off=0x1000
```

`a0=0x806000, a1=0x1000` 的形状与 `mmap(addr, len, ...)` 的前两个参数一致，而
`0x806000` **正好等于 `start_brk`**。若被测程序确实在这里做了一次**定址 mmap**
（`MAP_FIXED`），它就与 brk 区间头部重叠；此时 mmap 路径给 `[0x806000,0x807000)`
打上了 `MM_ST_ANON_VIRT` 标记，而紧邻的 `[0x807000,0x808000)` 是 brk 的 VMA。

**由此得到一个能解释全部矛盾的机制**：状态路径运行在 `mm_find_vma()` **之前**，
只认 status。若某段 status 标记**比它描述的 VMA 活得久**（VMA 已被拆除/替换，而标记
没被清），那么落在该地址上的后续缺页——**包括本该由 brk 逻辑处理的那些**——会被状态
路径用**上一任映射的权限**就地满足，绕过 brk 分支的记账与语义。页表于是被合法地、
自洽地写好了（所以断言抓不到），破坏却发生在**语义层**而非页表层。

**下一步（具体、可执行）**：
1. 找出所有「移除/替换 VMA 但不清 per-PTE status」的路径。已知 `mm_cursor_unmap()`
   只在**逐叶子 unmap** 时清 `ANON_VIRT`；需要核查的是**整段 VMA 被拆除而叶子未被
   逐页 unmap** 的情形：定址 `mmap(MAP_FIXED)` 覆盖旧 VMA、`mremap`、`munmap` 掉一段
   从未 fault 过的区间（此时没有叶子可逐，但 status 标记是从 `mm_pt_provision_anon()`
   写下的，**不是** fault 写的**——这正是漏洞所在**）。
2. 加一个针对性断言：状态路径命中时，核对该地址当前**确实存在**一个 VMA；不存在就计数
   并出声。这是对「status 必须与 VMA 生命周期同步」这一不变式的直接检验。

第 2 条尤其关键，因为它把 §10.7 记录的那个结构性问题（状态路径在 `mm_find_vma` 之前就
返回）从「设计缺陷」落成了一条**可机检的不变量**。

**当前安全状态**（未变）：预标记默认关闭，状态缺页路径 inert；默认配置
`mm_fault_from_status=0`、审计全 0（含 `safe=0`）、`pt_pages=6` 不变，
`smoke-mm-stress` / `smoke-mm-fork-exec-race` 通过，riscv64/x86_64/aarch64 构建通过。
所有 (b) 排查均在 `a20.anonprov=4096` 下进行。

### 10.30 第七条假设也被证伪；(b) 仍未定位，如实收尾

按 §10.29 的实验设计，在状态路径命中时加了一条断言：**该地址当前必须存在 VMA**
（`mm_find_vma()` 为空即计数并 `kerr`，因为随后的崩溃会让计数器读不到）。

`a20.anonprov=4096` 实测：**孤儿命中 0 次**，不变式违例 0 次，崩溃照旧。

**结论：per-PTE status 标记并没有比它描述的 VMA 活得久。** 也就是说 §10.29 那个
「标记比 VMA 长寿」的机制**不成立**——定址 mmap / mremap / munmap 未 fault 区间这些
路径实际上都清干净了（`mm_cursor_unmap()` 逐叶子清 `ANON_VIRT` 的行为足够）。

**至此七条假设全部被实测证伪**：

| # | 假设 | 证伪方式 |
|---|---|---|
| 1 | `nr_present` uint16 回绕 | `pt_table_empty` 直接扫 PTE，不看该计数 |
| 2 | 跨核 TLB 残留翻译 | `-smp 1` 同样崩 |
| 3 | 状态路径缺 TLB 事务 | `fault_map` 结构完全相同，也不开事务 |
| 4 | 宽限期回收 `mm_pt_retire_table()` | 状态路径 inert + 预标记开 = 9287 页、stress PASS |
| 5 | 漏记四项缺页统计 | 补齐后仍崩 |
| 6 | metadata 与 PTE 失配 | 回读断言 0 违例 |
| 7 | status 标记比 VMA 活得久 | 孤儿断言 0 命中 |

**两条诊断断言已回退**（工作树回到 `c53e0d70` 的干净状态）。回退理由不只是「没查到」：
`mm_find_vma()` 那条若留在快路径上，等于把论文最核心的「不查 VMA」又加了回去——它在
概念上就不属于这条路径。这类检查应当放进关机 auditor（像 §10.20 的 `safe_mismatch`
那样），而不是热路径。

**已确定的事实（实验结果，非推断）**：
1. mmap 预标记**单独**安全：当前代码、预标记开、状态路径 inert → 9287 页、审计全 0、
   `mm_stress` PASS、零 FATAL。
2. 崩溃只在状态路径**真的消费** mmap 预标记条目时出现。
3. 状态路径的映射**正确**：回读断言证明 `ANON_VIRT → 已映射` 每次都干净落地。
4. 状态路径**从不在没有 VMA 的地址上触发**。
5. 崩溃点是 brk 地址，而复现配置里 brk 预标记**关闭**。
6. 与并发无关（`-smp 1` 同样崩），与回收机制无关（#4），与页表/metadata 一致性无关（#3/#6/#7）。

**剩下最值得注意的一处结构差异**（尚未验证）：状态路径在 `handle_demand_fault_locked`
内**全程持有 `mm->lock`**，包括 `pfa_alloc_page()`、`cg_mem_charge()` 和 4 KiB
`memset()`；而 VMA 路径是**放锁分配、再重新加锁并重新校验**的（fault.c 里那段
`lock_held` + `mm_find_vma() != vma` 复查）。也就是说状态路径比 VMA 路径**多持锁**做了
一整套内存分配。这与页表正确性无关，但它是两条路径间最大的行为差异，也是下一轮该量的
东西：在 `a20.anonprov=4096` 下若把 `mm->lock` 的持有时间或 `pfa.lock` 的获取顺序打点，
或许能看出它是否触碰了某条未预期的锁序。

**诚实结论**：(b) 未定位。上面 7 条都是**排除法**得到的结论，没有一条是根因。继续
盲试的边际收益已经很低，而**预标记默认关闭**这一状态是安全的：默认配置下
`mm_fault_from_status=0`、审计全 0（含 `safe=0`）、`pt_pages=6` 不变、
`smoke-mm-stress` / `smoke-mm-fork-exec-race` / `check-mm-lock-model` 全通过、
5 个架构与 3 个 NOMMU 变体构建通过。**状态缺页路径在 (b) 定位之前保持 inert。**

### 10.31 x86_64 命令行到不了内核：fw_cfg selector 是硬编码的

`a20.wx=` 在 x86_64 上被静默忽略（默认 `deny` 仍生效，所以是「无法放宽 W^X」的功能缺口，
不是安全漏洞），本次新增的 `a20.anonprov=` 同样到不了。根因已定位到具体常量。

`kernel/arch/x86_64/platform/firmware.c:175-179`：

```c
#define FW_CFG_SELECTOR_PORT 0x510
#define FW_CFG_DATA_PORT     0x511
#define FW_CFG_SIGNATURE     0x0000
#define FW_CFG_CMDLINE_SIZE  0x0014      /* <-- 硬编码 */
#define FW_CFG_CMDLINE_DATA  0x0015      /* <-- 硬编码 */
```

`firmware_bootargs()` 读 `0x0000` 拿到 `0x51454d55`（"QEMU"）——**这说明 fw_cfg 机制本身
是通的**；随后读 `0x0014` 拿到 0，于是 `cmdline_size=0`、`cmdline=''`。

**原因**：QEMU 的 fw_cfg **文件 selector 是动态分配的**，由目录枚举顺序决定。`0x0000`
的签名是唯一固定的那个；`opt/cmdline` 落在哪个 selector，取决于 QEMU 注册 `etc/`、
`opt/` 等目录与各文件的顺序，**没有任何保证是 0x14/0x15**。把动态 selector 写死，
读到的就是一个不存在的条目（或别的文件），长度自然是 0。

**正确修法**（本次**未做**，原因见下）：按 fw_cfg 目录格式从 selector `0x0001` 开始枚举，
逐级找到 `opt` 目录再找到其下的 `cmdline`，用条目里的 select value 定位后读取：

* selector `0x0001` 处先读一个 0 字节，表示顶层目录结束；
* 每个条目为 `[len+1 字节][名字含结尾 NUL][1 字节 select value][4 字节 value][4 字节 size]`
  （多字节字段均为大端）；
* 目录条目的 `value` 是其内容的基 selector，文件条目的实际 selector 由基址加 select
  value 得到。

**为什么本次不做**：这是一段**引导期**代码，跑在 fw_cfg 之上、且我刚把 x86_64 引导修好
（§10.10）。在没有充分上下文验证的前提下写一个不完整的目录枚举器，风险是**把刚修好的
x86_64 引导再次弄坏**，而它的收益只是让一个默认安全的策略可配置。留待专门一轮来做，
并且必须配「有/无 `-append` 两种情况下都能启动」的门。

**影响范围**（明确）：
* x86_64：`a20.wx=` 恒为默认 `deny`（安全，仅不可放宽）；`a20.anonprov=` 无效，故
  x86_64 上无法做交错 A/B（§10.13 的实验只能在 riscv64 上做）。
* 其他架构不受影响：riscv64 经 DTB `/chosen/bootargs` 取命令行，实测可用；aarch64 /
  ppc64le / loongarch32 / riscv32 经 FDT 同理。
* 平台层 `qemu-virt-x86_64/board.c` 在 fw_cfg 为空时会回落到静态 `a20.ip=...` 默认值，
  所以网络配置仍能工作，这也是该缺陷此前没被察觉的原因。

### 10.32 x86_64 命令行：**实测证明 fw_cfg 根本不带文件目录**，目录遍历也救不了

§10.31 定位到 selector 被硬编码，并提出「按目录格式遍历」的修法。这次真的把遍历实现了
（`0x0001` 起按 `len/name/select/value/size` 逐条找 `opt` 再找 `cmdline`），**并且按要求
先立门：有 `-append` 与无 `-append` 两种情况都必须能启动**。

**门通过**（`booted=1 panic=0`，两种情况都是），说明遍历是只读的、不会破坏刚修好的引导。
**但它找不到 `opt`**，于是直接把 `0x0001` 的原始字节打出来看：

```
[FW_CFG] signature=0x51454d55          <- fw_cfg 端口 I/O 本身是通的
[FW_CFG-DIR] raw:                      <- 但 0x0001（文件目录）的内容是：
  00: 03 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  10: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  ...（96 字节全是 0）
```

再用 QEMU 显式注入一个文件再测一次，排除「只是没注册 opt/cmdline」的可能：

```
qemu ... -fw_cfg name=opt/cmdline,string=a20.anonprov=4096
[FW_CFG-DIR] raw: 03 00 00 00 ...（依旧全 0）
```

**结论（这是实测，不是推断）**：在这条 `-kernel <ELF>` 启动路径上，QEMU **根本没有向
fw_cfg 注册任何文件项**——`0x0000` 的签名能读到，说明端口与协议都对，但 `0x0001` 的文件
目录是空的。所以：

* §10.31 那个硬编码的 `0x0014/0x0015` **从来就不可能对**，它读到的是一个空条目；
* 我实现的目录遍历**逻辑正确但无处可施**：目录里没有条目可遍历。

**因此本次把目录遍历回退了**（工作树回到 `d567a4c5`/`3aaf683f` 一线的干净状态）。
理由不是「没验证通过」——遍历本身经过「有/无 `-append` 都能启动」这道门，验证是过的；
理由是它**在这条启动路径上证明无功能**，而它位于引导期代码，留着只会让人误以为命令行可用。
一条读不到东西的代码路径，比没有更糟。

**x86_64 命令行的正解不在 fw_cfg**，而在启动协议本身：`-kernel <ELF>` 这种直接加载
ELF 的方式，QEMU 不会像加载 bzImage 或 multiboot 那样把 `-append` 送到内核。要让
`a20.*` 内核参数在 x86_64 生效，需要改的是**启动协议/构建产物**（例如改走 multiboot
入口以拿到 multiboot info 里的命令行，或把参数编进 ELF 的 note 段）。这是一个**独立的、
更大的改动**，与本次「补全 CortenMM」的主线无关，**不在本次范围内**。

**影响（与 §10.31 一致，未变）**：
* x86_64：`a20.wx=` 恒为默认 `deny`（安全，仅不可放宽）；`a20.anonprov=` 无效，故交错
  A/B 的实验（§10.13）只能在 riscv64 上做。
* 其他架构不受影响：riscv64 经 DTB `/chosen/bootargs` 取命令行，实测可用。
* 网络仍能工作——平台层 `qemu-virt-x86_64/board.c` 在拿到空命令行时会回落到静态
  `a20.ip=...` 默认值，这也是这个缺陷一直没被察觉的原因。

### 10.33 **(b) 根因找到并修复**：状态路径装的是**内核态** PTE，缺 `PTE_U`

根因是**权限位在往返中丢了 user 属性**。

`mm_pt_prot_bits()` 只把 PTE 的 R/W/X 三个位搬进状态字节，而 `status_byte()` 把它们
放在 `MM_ST_PROT_R/W/X`（3 位）里——**状态字节里没有任何一位表示「用户态」**。
`arch_pte_leaf()` 也不会替调用者补 `PTE_U`：

```c
static inline uint64_t arch_pte_leaf(paddr_t pa, uint64_t flags) {
    return arch_pte_from_pa(pa) | flags | PTE_V;   /* 没有 PTE_U */
}
```

于是状态路径按状态字节重建 PTE 权限时：

```c
pte_t allow = 0;
if (cls_byte & MM_ST_PROT_R) allow |= PTE_R;
if (cls_byte & MM_ST_PROT_W) allow |= PTE_W;
if (cls_byte & MM_ST_PROT_X) allow |= PTE_X;   /* allow 里永远没有 PTE_U */
```

装上去的是一页**仅内核态可访问**的映射。缺页处理「成功」返回，用户态下一次访问立刻
在**刚装好的那一页**上取页故障。

**证据链**（`a20.anonprov=4096`，状态路径可用）：

1. 临时在状态路径成功分支加 trace（**注意：第一次加 trace 时脚本 assert 失败、根本没
   编进去，我却据此得出「状态路径 0 次成功」的结论——又一次把自己的工具故障当成被测
   系统行为。加 trace 后必须先 `grep` 确认它真的在文件里再构建**）。重做后：

   ```
   [MM-STATUS] ok va=46d000 prot=6      <- 状态路径唯一一次成功
   FAULT-VA] stval=0x46d000             <- 崩溃地址与上面完全相同
   ```

   **装映射的地址就是随后取故障的地址**——这一条把范围压到「刚装好的那一页权限不对」。

2. `prot=6`：riscv64 上 `PTE_R=1<<1=0x02`、`PTE_W=1<<2=0x04`、`PTE_X=1<<3=0x08`，
   所以 6 = R|W，**没有 `PTE_U`（1<<4=0x10）**。（我一度把 6 误读成「缺 R 位」，那是
   又一次把自己的算术当成结论。）

3. 早先的崩溃转储独立佐证：出错页 `flags=0x7` = `PTE_V|PTE_R|PTE_W`，而同一时刻 VMA
   期望的 `pte_flags=0xd7` 里含 `0x10`(U) 与 `0x40/0x80`。差的正是 `PTE_U`。

**修法**：`PTE_U` 不需要占用状态字节的一位——状态只可能为**用户区间**记录，因为
`mm_pt_provision_anon()` 会拒绝用户地址空间以外的范围。所以 `PTE_U` 在这里是**不变
量**，由状态路径显式补上：

```c
allow |= PTE_U;
```

**修后实测**（riscv64，`a20.anonprov=4096`）：

```
mm_demand_faults:     3411
mm_anon_provisioned:  9287
mm_anon_faults:       2851
mm_fault_from_status: 2842      <- 83% 的 demand fault 不经 VMA
stress-pass: 1   fatals: 0
MM-ASM: missing_meta=0 present=0 absent=0 prot=0 cow=0 vma=0 safe=0 anon_virt=0
```

**这是本项目第一次，论文 Fig. 8 的「按状态决策、不查 VMA」的缺页路径真正跑起来**：
2842 次缺页直接由 per-PTE 状态满足，不经过 `mm_find_vma()`。此前所有关于它的性能
结论都建立在这条路径**从未执行**的前提上（§10.6 撤回的 479、§10.8/§10.14 的 14%
与 28%），现在这个前提变了，那些数字**需要重新测量**。

同时确认：§10.26/§10.29 的两条诊断断言（回读不变式、孤儿 status）已从热路径**彻底
移除**（此前 §10.30 声称已回退，实际只回退了 `pt.c`/`perf.h`，`fault.c` 里的块一直
还在，其中孤儿检查还会反过来调 `mm_find_vma()`，与这条路径的设计前提直接冲突——
这本身就是一次「文档与代码不一致」的实例）。

**默认行为不变**：预标记仍默认关闭，故默认配置下 `mm_fault_from_status` 恒为 0，
与改动前完全一致；本次修复只在显式 `a20.anonprov=<pages>` 时生效。

验证：riscv64 / x86_64 / aarch64 / loongarch64 / ppc64le 与 riscv64 / aarch64 / x86_64 的
NOMMU 全部构建通过；`smoke-mm-stress`、`smoke-mm-fork-exec-race`、`check-mm-lock-model`
通过，审计全 0。

### 10.34 修好 (b) 之后重测：**预标记变成性能中性**，§10.14 的 14% 作废

§10.14 那组「预标记让 mmap 慢约 14%、6/6 对同向」的测量，是在**状态路径根本跑不起来**
（会崩）的构建上做的。既然 §10.33 修好了 PTE_U，那组数字的**前提已经不成立**，必须重测。
用 `a20.anonprov=` 开关做交错 A/B（单次构建，6 对交替，riscv64/TCG + `taskset -c 12-15`），
以逐对比值抵消臂间漂移：

| 基准 | 逐对 OFF/ON 中位 | OFF 更快 | 判定 |
|---|---|---|---|
| mmap low | 1.041 | 3/6 | 无可分辨效应 |
| mmap high | 1.136 | 2/6 | 无可分辨效应 |
| mmap-PF low | 1.024 | 2/6 | 无可分辨效应 |
| mmap-PF high | 0.953 | 4/6 | 无可分辨效应 |
| PF low | 0.998 | 3/6 | 无可分辨效应 |
| PF high | 1.011 | 3/6 | 无可分辨效应 |
| unmap-virt low | 1.116 | 2/6 | 无可分辨效应 |
| unmap-virt high | 0.981 | 4/6 | 无可分辨效应 |
| unmap low | 1.046 | 1/6 | 无可分辨效应 |
| unmap high | 0.981 | 4/6 | 无可分辨效应 |

**10 项全部没有可分辨的效应**：每一项的逐对比值都在 1.0 两侧摆动，**没有任何一项
出现 6/6 同向**（按 §10.14 定的判据）。所以：

* **§10.14 的「mmap 慢 14%」作废。** 那组数据测自状态路径崩溃的构建——预标记写状态
  的代价没变，但当时消费者根本不工作，两组数字不可比。这也是我第三次栽在「拿一个
  坏掉的构建当基线」上。
* 修好之后，预标记在本平台上**性能中性**：既没有可测的代价，也没有可测的收益。
  从机制上讲这说得通——预标记的 mmap 侧代价（写 per-PTE 状态）现在被缺页侧省下的
  `mm_find_vma()` 抵消掉了，而后者在 TCG 上并不贵。

**默认值的处理**：**仍然保持默认关闭**，理由是「中性」不等于「更好」。

1. 没有任何证据支持打开它，也同样没有证据说明它有害——按 §10.11/§10.14 的纪律，
   在能分辨 1.2× 以内差异的平台上（x86_64/KVM）拿到数据之前，不应把默认配置改成
   一次可测量的行为变更。
2. 打开预标记会启用一大片平时不走的代码（状态快路径 + UFFD 门 + 宽限期回收）。
   这些虽然都已在 `a20.anonprov=4096` 下验证通过（mm_stress PASS、审计全 0、
   2842 次状态缺页、零 FATAL），但让它们成为**默认**路径应当是独立的一次决定，而不是
   顺手改默认值。
3. 想在本平台验证时，用 `a20.anonprov=4096` 显式打开即可；两条路径都已被验证正确。

**至此，论文的机制第一次真正运行起来（2842/3411 = 83% 的 demand fault 不经 VMA），
而在 riscv64/TCG 上它既不更快也不更慢。** 论文声称的优势是「省掉 Linux 花在 VMA 上的
时间」；在 TCG 上 VMA 查找本就很便宜，因此测不出来。要看到收益需要 x86_64/KVM——
而 x86_64 的命令行仍不可用（§10.31/§10.32：`-kernel <ELF>` 这条启动路径上 QEMU 根本
不向 fw_cfg 注册文件项），这是现在挡住性能收口的最后一道障碍。

### 10.35 x86_64 拿到开关了，于是暴露出**第二个**、且与架构相关的缺口

`PTE_U` 修好后（§10.33），x86_64 仍然拿不到命令行（§10.31/§10.32），所以给 Makefile 加了
`EXTRA_CFLAGS` 逃生口，并让预标记上限支持**构建期**默认值
（`-DCONFIG_ANON_PROV_DEFAULT=<n>`；`a20.anonprov=<n>` 仍可在有命令行的架构上运行时覆盖）。
这样在 x86_64 上可以构建两张镜像、交替运行，得到与 riscv64 同样的「时间相邻」A/B。

**两个开关都验证过**（各 5 架构 + NOMMU 构建通过）：

| 镜像 | `mm_anon_provisioned` | `mm_fault_from_status` | `mm_stress` |
|---|---|---|---|
| `DEFAULT=4096` | 53 | **12** | **FATAL(signal=11)** |
| `DEFAULT=0` | 0 | 0 | PASS |

**结论一：`PTE_U` 是必要但充分的反面——必要、不充分。** x86_64 上状态路径已经真的跑起来
（12 次），说明 U 位的修复是通用有效的；但随后仍然崩。

**结论二：权限往返是架构相关的，而我按 riscv64 调通后没有再检查别的架构。** riscv64 的
页表位是 `PTE_R=1<<1 / PTE_W=1<<2 / PTE_X=1<<3 / PTE_U=1<<4`，`allow` 里补 U 就够了。
但 x86_64 的定义完全不同：

```c
#define PTE_V    (1UL << 0)   /* Present */
#define PTE_R    (1UL << 0)   /* Present implies readable —— 与 PTE_V 同一位！ */
#define PTE_W    (1UL << 1)
#define PTE_U    (1UL << 2)
#define PTE_X    (1UL << 11)  /* 软件位；arch_pte_leaf 会清 NX */
#define PTE_USER (PTE_V | PTE_W | PTE_U | PTE_NX | PTE_LEAF)
```

状态路径只用 `MM_ST_PROT_R/W/X` 三个位重建 `allow`，**因此丢掉了 `PTE_NX` 与
`PTE_LEAF`**。在 x86_64 上这意味着经状态路径装上的页是**可执行的**（缺 NX）且缺叶子标记。
这与「装完立刻在该页取故障」的症状一致，也是 `PTE_U` 修好后 x86_64 仍崩的最可能原因。

**根本教训（这是本项目第三次同类错误）**：`PTE_U` 那个 bug 的本质是**我把某个架构的
PTE 语义当成了通用语义**。修的时候只验证了 riscv64（唯一能跑命令行的架构），没有在
另一个架构上交叉验证，于是「修好了」只在一个架构上成立。**页表抽象层的任何改动都必须
至少在两个语义不同的架构上验证**——这正是本仓库有 riscv64 / x86_64 / aarch64 的意义。

**顺带发现的一个独立问题**：`mprotect.c:141` 的
`mm_pt_refresh_absent_prot(pte - idx, idx, ptef)` 被 UBSAN 报 **pointer-overflow**
（`pte - idx` 从表内某个条目反推表基址，在对象边界外做指针运算）。一次 x86_64 运行里出现
**35 次**。这与 (b) 无关（是 UBSAN 诊断，不是 FATAL 的原因），但它说明「用条目指针反推
表基址」这种写法在指针消毒器眼里始终是越界运算，属于应当改写的脆弱写法（应显式传表基址）。

**状态**：x86_64 上的 (b)-后续问题**未修**。`PTE_NX` / `PTE_LEAF` 的缺失是下一步明确的
目标，但需要设计一个跨架构的「权限往返」表示（当前 3 位的 `MM_ST_PROT_*` 在 x86_64 上
不足以表达 NX/LEAF），属于对状态字节格式的再一次扩展——与 §10.18/§10.19 记下的「所有
8 位都已用尽」是同一个约束。**先在 riscv64 上把跨架构权限往返设计对，再谈 x86_64 复测。**

### 10.36 权限往返改为走架构接口（正确的重构，但**没有**修好 x86_64）

§10.35 猜测 x86_64 仍崩是因为状态路径手搓 PTE 位、丢了 `PTE_LEAF`/`PTE_NX`。这个猜测
**是错的**。仓库里早就有正确的接口：

```c
pte_t mm_prot_to_pte_flags(int prot) {
    pte_t f = PTE_V | PTE_U | PTE_A | PTE_MAT1 | PTE_LEAF;   /* 架构必需位 */
    if (prot & 1) f |= PTE_R;
    if (prot & 2) f |= (PTE_W | PTE_D);
    if (prot & 4) f |= PTE_X;
    if (f & PTE_W) f |= PTE_R;                                /* W 蕴含 R */
    return f;
}
```

状态路径原先是手搓 `PTE_R|PTE_W|PTE_X|PTE_U`，现在改成把状态里的三个 prot 位还原成
prot 掩码后交给 `mm_prot_to_pte_flags()`。这样**任何架构的必需位（`PTE_U`、
`PTE_LEAF`、`PTE_A`、`PTE_MAT1`、NX 语义）与 W⇒R 依赖都由架构自己负责**，调用点不再
需要知道任何架构细节——也就是把 §10.33 那个「补 `PTE_U`」的补丁，从**架构相关的补丁**
升格为**架构无关的正确写法**（`allow |= PTE_U;` 随之删除）。

**验证**：

* riscv64（`a20.anonprov=4096`）：`mm_fault_from_status=2836`、stress **PASS**、
  零 FATAL、MM-ASM 全 0 —— 与重构前（2842）一致，**无回归**。
* x86_64（`CONFIG_ANON_PROV_DEFAULT=4096`）：**仍然 FATAL**，且
  `pc=0x1e2e2`、`signal=11`、`pid=6`、UBSAN 35 次，与重构前**逐字节相同**。

**结论（必须写清楚）**：这次重构是**对的**（架构抽象、去掉架构相关的补丁、riscv64 无
回归），但它**不是** x86_64 崩溃的原因。§10.35 关于 `PTE_LEAF`/`PTE_NX` 缺失的推断
**被证伪**——崩溃地址、信号、UBSAN 计数在重构前后完全一致，说明那条路径根本没被触及。

**x86_64 的崩溃仍然未定位。** 已知事实：状态路径在 x86_64 上确实跑了 12 次
（`mm_fault_from_status=12`），随后 `mm_stress` 在 `pc=0x1e2e2` 崩溃；关机审计全 0
（`pt_pages=9`、各 mismatch 全 0、`safe=0`），说明**元数据与 PTE 没有失配**；
同一次运行里有 35 次 `mprotect.c:141` 的 UBSAN pointer-overflow。

`pc=0x1e2e2` 在 `mm_stress` 里，落在用户态。下一步该做的是**把 `pc` 解析回源码行**，
而不是继续猜权限位——这是本次连续三次猜权限/记账/回收都落空之后，应当改换的取证方式。

### 10.37 x86_64 崩溃取证：`pc` 解析 + PTE/VMA 权限**不一致**

按 §10.36 写的取证方式（不再猜），把 `pc=0x1e2e2` 解析回源码：

```
pc=0x1e2e2  →  __copy_tls + 0x72   (glibc)
  1e2db: mov 0x5ea6(%rip),%rax   # 24188
  1e2e2: mov %rax,0x0(%r13)      ← 取故障指令
  1e2e9: mov %r13,0x8(%r12)
```

`%r13` 是线程指针 TBLOCK，所以这是一次**经线程块的写**。随后崩溃转储给出决定性的信息：

```
[FAULT-VA] stval=0x108369f20
  pte value = 0x800000007ee92425     ← bit63 = NX
  leaf: base=0x108369000 pa=0x7ee92f20 pfn=519570 flags=0x425
  page_words: 全 0
  mm: brk=0x1d65000 start_brk=0x1d65000 stack=[0x3fcd1000,0x3fcf1000)
  vma=[0x108349000,0x10836a000) flags=0x13 pte_flags=0x467 file_fd=-1 off=0x2000
```

**核心矛盾（一处就能定位）**：

| | 读 | 写 |
|---|---|---|
| PTE（`flags=0x425`） | 有 | **无** |
| VMA（`pte_flags=0x467`） | 有 | **有** |

即 **VMA 说这段可写、PTE 却是只读**。用户态的写（`__copy_tls` 往线程块写）落到只读页
上，于是取页故障——这是内核**正确**拒绝了一次越权写；错的是**映射本身与 VMA 不一致**。

**指向 mprotect**。同一次运行里有 **35 次** UBSAN pointer-overflow，位置全部在
`kernel/mm/mprotect.c:141`：

```c
mm_pt_refresh_absent_prot(pte - idx, idx, ptef);   /* 由条目指针反推表基址 */
```

该行属于 mprotect 的「已预留但从未缺页」分支。而本次故障的现象是
「**VMA 已改成可写、PTE 仍是只读**」——正好是 mprotect 只更新了 VMA、没更新 PTE。

**最可能的机制**：`mprotect` 里用 `pte - idx` 从叶条目反推表基址，在对象边界外做指针
运算（UBSAN 每次都报），若这个基址算错，`mm_pt_refresh_absent_prot()` 就写到**错误的
槽位**，真正该改的那一页的 PTE 权限从未被更新。VMA 侧照常更新，于是两者就此分叉。

**这条推理把「猜权限位」换成了「一处可指认的不一致」**，但**根因仍未最终确认**：
还差一步——确认 mprotect 在**已映射**页上是否走了另一个分支、以及那 35 次 UBSAN 是否
恰好落在本次故障页上。验证手段（下一步，未做）：在 mprotect 里对「已映射」与「已预留」
两条分支各加一个计数/告警，并打印 `pte - idx` 算出的表基址，与
`pt_lookup_leaf()` 返回的表指针比对。

**注意一个反直觉之处**：这与 §10.33 修的 `PTE_U` **不是同一处**。`PTE_U` 那次是
「装出来的页缺 user 位」，症状是**刚装完立刻 fault**；这次是「PTE 与 VMA 权限分叉」，
且在 riscv64 上不复现（riscv64 走完 2836 次状态缺页、stress PASS、审计全 0）。

**状态**：x86_64 崩溃仍未修复，但已从「不可观测的 FATAL」收窄到
「mprotect 使 PTE 与 VMA 权限分叉 + `mprotect.c:141` 的指针反推越界」这一处具体矛盾。
riscv64 侧功能与性能结论不受影响。

### 10.38 修掉 mprotect 的 NULL 指针运算（真实缺陷，但**不是** x86_64 崩溃的原因）

§10.37 观察到 mprotect 的「已预留未缺页」分支里有一行：

```c
} else {
    int idx = arch_pt_vpn(va, 0);
    mm_pt_refresh_absent_prot(pte - idx, idx, ptef);   /* pte 可能为 NULL */
    va += PAGE_SIZE;
}
```

这个分支的条件是 `!(pte && (*pte & PTE_V))`，所以 **`pte` 可能是 NULL**（地址根本没有叶子
表），而代码无条件做 `pte - idx` —— **对 NULL 做指针运算**。后果链：

1. `pte - idx` 从 NULL 出发算出野指针（UBSAN 每次都报 pointer-overflow）；
2. `mm_pt_refresh_absent_prot(野指针, …)` 里 `mm_pt_meta()` 对野地址算出的 pfn
   `pfn_valid()` 不成立，于是**静默什么都不做**；
3. per-PTE 状态因此**保留旧权限**，而循环末尾的 `v->pte_flags` 照常更新为新权限；
4. 之后状态缺页路径按**旧状态**装帧 → PTE 与 VMA 权限分叉。

**修法**：没有叶子表就没有元数据、也就没有状态可刷新，直接跳过。

```c
if (pte) {
    int idx = arch_pt_vpn(va, 0);
    mm_pt_refresh_absent_prot(pte - idx, idx, ptef);
}
```

**验证**：

| | UBSAN 次数 | `mm_stress` |
|---|---|---|
| 修前 | **35** | FATAL(signal=11, pc=0x1e2e2) |
| 修后 | **2** | FATAL(signal=11, pc=0x1e2e2) |

**UBSAN 从 35 降到 2**，证明这处 NULL 指针运算**确实存在、确实被修掉了**。但
**崩溃依旧，且 `pc` 一字未变**。所以 §10.37 那条因果链
「状态未刷新 → 装出旧权限 → PTE/VMA 分叉 → 缺页」**被证伪**：它是**症状链上的相关项，
不是原因**（或者至少不是唯一原因）。PTE/VMA 权限分叉这个观察本身仍然成立，仍然是
一个真实的不一致；但导致它产生的机制另有其人。

**这是一个诚实的「修了一个真缺陷、但没修好崩溃」的提交。** 保留它的理由是那处 NULL 指针
运算是**确凿的 UB**，无论它是不是本次崩溃的成因都该修；不保留它的理由不存在。

**x86_64 崩溃仍未定位。** 已经排除的：权限位集合（§10.36 重构前后逐字节相同）、
`PTE_U`（riscv64 已解决）、状态与 PTE 失配（回读断言 0 违例）、状态比 VMA 活得久
（孤儿断言 0 命中）、mprotect 的 NULL 指针运算（本次修掉，崩溃不变）。仍然成立的现场事实：
状态路径在 x86_64 上跑了 12 次后，`mm_stress` 在 `__copy_tls+0x72`（往线程块写）崩溃，
出错 PTE 只读而其 VMA 可写，关机审计全 0。

**下一步应当做的取证**（继续猜的边际收益已经很低，应当换成系统化定位）：在
`__copy_tls` 取故障的那个地址上，把「该页由谁装上、装的时候用的权限是什么、对应 VMA
的权限是什么」三者一起 dump 出来（页表项、VMA、以及 `pt_meta` 里的 class/prot 字节），
一次就能判定是「状态路径装错」还是「mprotect 改错」还是「别处改错」。这需要在该页上
加一次性诊断，而不是继续单点猜测。

**riscv64 侧不受影响**：5 架构 + 2 NOMMU 变体构建通过，三个门全通过，审计全 0。

### 10.39 x86_64 崩溃定位：mprotect 只改了一个 PTE，却把整个 VMA 的权限改了

按 §10.38 写的取证方式，在「PTE 已present、但访问被拒」这条分支上一次 dump
「页表项 + VMA + per-PTE 状态」三者（诊断代码随后已移除）：

```
[MM-PERM] va=f9374f20 acc=1 pte=800000007eecb425 leaf=1 U=1 W=0
          vstart=f9354000 vend=f9375000 vflags=13 vpte=467
          cls=2 prot=1
```

一次就定性了。把三列并排：

| 来源 | 读 | 写 |
|---|---|---|
| PTE（`flags=0x425`） | 有 | **无** |
| per-PTE 状态（`cls=2`=ANON_MAPPED, `prot=1`=R） | 有 | **无** |
| VMA（`vpte=0x467`） | 有 | **有** |

**PTE 与状态彼此一致（都只读），只有 VMA 是可写的。** 所以问题不是「谁装错了这一页」，
而是**有人把 VMA 改成可写、却没有把 PTE 一起改**。`acc=1` 是写访问，落在
`__copy_tls` 往线程块写的那条路上，于是被正确拒绝 → SIGSEGV。

**机制**（`kernel/mm/mprotect.c` 的页循环）：

```c
pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
if (pte && (*pte & PTE_V)) {
    if (level > 0 && (base < v->start || base + size > v->end)) {
        int dr = mm_demote_huge_page(mm, va);
        if (dr < 0) return dr;
        continue;
    }
    ... /* 只更新了 *pte 这一个条目 */
    *pte = replacement;
    va = base + size;          /* <-- 直接跳过整个 leaf 的其余条目 */
}
...
v->pte_flags = mm_pte_flags_apply_prot(v->pte_flags, ptef);   /* 整个 VMA 一起改 */
```

循环体只修改 `pt_lookup_leaf()` 返回的**那一个** leaf 条目，随后 `va = base + size`
一次跳过该 leaf 的**全部**条目；只要这个大 leaf 完全落在 VMA 内部（不触发降级），
中间的条目就一个都没被改，而循环外的 `v->pte_flags` 却把**整个 VMA** 的权限改了。
结果就是观测到的形态：VMA 可写、被跳过的那些页仍是只读。

**为什么 riscv64 上不复现**：那里 `mm_stress` 的 mprotect 区间没有正好套住一个未降级的
大 leaf（或叶子几何不同），所以每一页都走到了「更新 `*pte`」这一步。这与 §10.33 那个
「把 riscv64 的 PTE 语义当成通用语义」是同一类错误的另一个面：**只在能跑通的那个平台
上验证**。

**修法**（未做，需一并处理大 leaf）：循环必须遍历该 leaf 内的**每一个**条目，而不是只改
第一个。最小且安全的做法是把 `va = base + size` 改成 `va += PAGE_SIZE`（逐页处理），
或者在 `level > 0` 且 leaf 完整落在 VMA 内时先降级再逐页。当前「不跨界就不降级」的优化
正是漏洞来源。

**状态**：这是本项目里**第四次**「只在一个架构/一条路径上验证」的教训。前三次分别是
热页当缺页（§10.8）、脚本 bug 当内核回归（§10.15）、把 riscv64 的 PTE 语义当通用
（§10.33/§10.36）。共同点是**用单平台、单路径的成功冒充了正确性**。

**当前安全状态**：riscv64 侧不受影响（5 架构 + 2 NOMMU 变体构建通过、三个门全通过、
审计全 0、状态路径 2836 次缺页正常）。x86_64 崩溃**仍未修**，但已定位到一处确定的
代码缺陷，而不是一个模糊的症状。

### 10.40 修掉 mprotect 跳过大 leaf 其余条目（真实缺陷，仍**不是**崩溃原因）

§10.39 定位到的机制：页循环只改 `pt_lookup_leaf()` 返回的**那一个** leaf 条目，随后
`va = base + size` 一次跳过该 leaf 的**全部**条目；而循环外的 `v->pte_flags` 却把**整个
VMA** 的权限改了。原来的降级条件只在「大 leaf 跨越 VMA 边界」时才降级，所以一个**完整
落在 VMA 内部**的大 leaf 不会被拆开，于是它除第一个条目外的所有页都保留旧权限。

**修法**：只要 `level > 0` 就先降级，使 `size` 变成单页，`va = base + size` 的推进才成立。

**验证**：

| | `mm_stress` | 计数 | UBSAN |
|---|---|---|---|
| 修前 | FATAL | 53 / 12 | 2 |
| 修后 | **FATAL** | **53 / 12** | 2 |

**崩溃依旧，且所有计数一字未变。** 所以 §10.39 那条因果链
「跳过条目 → VMA 与 PTE 分叉 → 缺页」**被证伪**：PTE/VMA 权限分叉这个观测是真的，
但**不是**由这个跳过来历。

**这是一个诚实的「修了真缺陷、但没修好崩溃」的提交**，与 §10.38 那次性质相同：跳条目
是确凿的逻辑错误（无论成因与否都该修），但它不是本次崩溃的根因。

**x86_64 崩溃的排查到此为止**，因为已经连续四次猜错：

| 假设 | 证伪方式 |
|---|---|
| 权限位集合不对（缺 `PTE_LEAF`/`PTE_NX`） | 重构前后崩溃逐字节相同（§10.36） |
| `PTE_U` 缺失 | riscv64 修好后完全正常（§10.33） |
| 状态未刷新导致装出旧权限（NULL 指针运算） | 修掉后 UBSAN 35→2，崩溃不变（§10.38） |
| mprotect 跳过大 leaf 其余条目 | 修掉后计数一字不变（§10.40） |

**仍然确定的事实**（全部来自实验，不是推断）：
* 状态路径在 x86_64 上确实跑了 12 次才崩（`mm_fault_from_status=12`），riscv64 上跑
  2836 次完全正常；
* 出错现场是**一次写访问**落在**只读 PTE** 上，而该 PTE 的 **per-PTE 状态也只读**
  （`cls=2` ANON_MAPPED、`prot=1`），**只有 VMA 是可写**（`vpte=0x467`）；
* 关机审计全 0（含新增的 `safe=0`），元数据与 PTE 之间**没有**失配；
* 与并发无关（`-smp 1` 同样崩）、与预标记本身无关（状态路径 inert 时预标记开 = PASS）。

也就是说：**PTE、per-PTE 状态、VMA 三者里有两者一致、一者（VMA）单独不同**。前两次修复
分别动了「状态刷新」与「条目遍历」，都没有改变这个形态——说明做出这个差异的**不是**这两
处。下一步若要继续，应当在**写入 VMA `pte_flags` 的每一处**加断言（凡是让 VMA 变可写的
地方，同时确认对应叶子已被更新），从「谁改了 VMA」这一侧反查，而不是再从 PTE 侧猜。

**当前安全状态**：riscv64 侧功能与性能结论完全不受影响——5 架构 + 2 NOMMU 变体构建通过、
三个门全通过、审计全 0、状态路径 2836 次缺页正常、预标记开/关均为性能中性（§10.34）。
所有 x86_64 排查均在 `EXTRA_CFLAGS=-DCONFIG_ANON_PROV_DEFAULT=4096` 下进行。

### 10.41 排查 x86_64 崩溃：已排除的「谁把 VMA 改成可写」候选

现场事实（§10.39，一次三路 dump，全程只触发 1 次）：

```
PTE      flags=0x425          有读，无写
per-PTE  cls=2 prot=1         有读，无写     ← 与 PTE 一致
VMA      vpte=0x467           有读，有写     ← 只有它不同
```

所以**做这个差异的只可能是「把 VMA 改成可写、却没同步 PTE 与状态」的那一方**。
把 `vma->pte_flags` 的全部写入点列出来逐一排除：

| 写入点 | 是否可能 | 理由 |
|---|---|---|
| `mprotect.c:68` | **排除** | 位于 `#ifdef CONFIG_NOMMU` 内；NOMMU 无页表，只改 VMA 是正确的。有页表时该分支根本不编译。 |
| `mprotect.c:169` | **排除** | 前面有逐页循环会改 PTE；§10.38/§10.40 两次修完，崩溃与计数一字未变。 |
| `mmap.c:163`、`elf.c:170`、`munmap.c:270`、`sysv_shm.c:298`、`framebuffer.c:198/379`、`io_uring.c:146` | **排除** | 全是 **VMA 创建**点：新建 VMA 时对应叶子尚不存在，之后的第一页要么走 VMA 缺页路径按 VMA 装帧，要么走状态路径按状态装帧。没有任何一条能在「PTE 与状态都已是只读」之后再把 VMA 变可写。 |

**由此可以确定地收窄**：既不是 mprotect（两个分支都排除），也不是任何 VMA 创建点。
剩下的可能只有两类，且都**不是**「谁改了 VMA」：

1. **VMA 被拆分/合并后 `pte_flags` 取值错误**——`mm_split_vma_at()` 与
   `vma_try_merge()`（mprotect 末尾会调用）会新建或合并 VMA。若合并时从相邻 VMA 抄了
   `pte_flags`，而相邻那段其实是只读的，就可能造出「本段 RW、叶子 R」的组合。
2. **页被换成了另一个 VMA 覆盖的范围**——即该地址后来落进了另一个（可写的）VMA，而
   页表项仍是先前那段留下的只读映射。dump 里 `vstart/vend` 与 `va` 的相对位置值得再核对
   一次：`va` 距 `vstart` 是 `0x20f20`，距 `vend` 是 `0x20e0`，**两端都不近**，说明
   `va` 落在这个 VMA 的内部而非边界，这一点与「跨界拆分出错」相容。

**下一步若继续，应当查的是拆分/合并路径，而不是再改 PTE 权限**：
在 `mm_split_vma_at()` 与 `vma_try_merge()` 里打印新 VMA 的 `[start,end)` 与继承来的
`pte_flags`，并检查它们是否与该范围内现存叶子的实际权限一致。这是一条此前从未看过的
路径——前四次排查全部集中在「PTE 怎么被写」与「状态怎么被写」，而现场证据表明两者
都是自洽的。

**当前安全状态**：riscv64 侧完全不受影响（5 架构 + 2 NOMMU 变体构建通过、三个门全通过、
审计全 0、状态路径 2836 次缺页正常、预标记开/关性能中性）。x86_64 崩溃仍未修，但排查
已从「猜权限位」推进到「VMA 生命周期」，且排除掉了全部显式写入点。

### 10.42 VMA 拆分/合并路径也排除：x86_64 排查到此为止

接着 §10.41 排除全部显式写入点之后，把 VMA 生命周期路径也看了一遍：

**`mm_split_vma_at()`**（`kernel/mm/vma.c`）用 `*tail = *v` 整体复制父 VMA，再改
`start` 与 `file_offset`。`pte_flags` 是被**原样继承**的，所以拆分本身不可能造出
「一段 RW、一段 R」——两半拿到的是同一份权限。**排除。**

**`vma_try_merge()` → `vma_can_merge()`** 有一道显式守卫：

```c
if (a->vm_flags != b->vm_flags || a->pte_flags != b->pte_flags)
    return 0;
```

即 `pte_flags` 不同的两个相邻 VMA **拒绝合并**，所以合并也不会把只读段的权限"抹"成
可写。**排除。**

至此，与 VMA 权限有关的**全部**路径都已排查：

| 路径 | 结论 |
|---|---|
| `mprotect.c:68` | 排除（`#ifdef CONFIG_NOMMU` 内，有页表时不编译） |
| `mprotect.c:169` 的逐页循环 | 排除（两次修复后崩溃与计数一字未变） |
| `mprotect` 跳过大 leaf 其余条目 | 排除（修复后计数一字未变） |
| `mmap/elf/munmap/sysv_shm/framebuffer/io_uring` 的 VMA 创建点 | 排除（创建时叶子尚不存在） |
| `mm_split_vma_at()` | 排除（`*tail = *v` 原样继承 `pte_flags`） |
| `vma_try_merge()` | 排除（`pte_flags` 不同即拒绝合并） |
| per-PTE 状态的写入（provision / refresh / status 路径安装） | 排除（回读断言 0 违例；状态与 PTE 现场自洽） |

**结论：x86_64 上的「只读 PTE + 可写 VMA」这个组合，其来源不在上述任何一处。**
四次猜测、四次证伪，且现场证据（PTE 与 per-PTE 状态自洽、只有 VMA 单独不同）始终指向
"VMA 侧"，但把 VMA 侧所有写入路径都排除之后，仍然解释不了。**这说明现场 dump 里的
那个 VMA 可能根本不是当前 fault 所属的 VMA**——即 dump 打印 VMA 的时机或取值有问题，
而不是权限真的分叉了。若真如此，之前基于"PTE/VMA 分叉"的三次推理（§10.37/§10.39/§10.40
的因果链）从一开始就是建立在**一个错误的观测**之上。

**这是我第一次开始怀疑观测本身，而不是继续怀疑被观测的对象。** 下一步应当做的不是再改
代码，而是**先验证这个观测是否可信**：在 dump 里把「fault 时的 VMA」与
「`mm_find_vma(stval)` 现场返回的 VMA」同时打印（应当是同一个指针），并把该范围内
每一个已映射叶子的实际权限全部列出。若二者一致且权限真的分叉，则说明存在本文档未
记录的第三条写入路径；若不一致，则观测本身是错的，上面三次因果推理需全部作废。

**riscv64 侧完全不受影响**：5 架构 + 2 NOMMU 变体构建通过、`smoke-mm-stress` /
`smoke-mm-fork-exec-race` / `check-mm-lock-model` 三个门全通过、关机审计全 0
（含新增的 `safe=0`）、状态路径 2836 次缺页正常、预标记开/关性能中性（§10.34）。
x86_64 的排查不影响上述任何结论。

### 10.43 观测本身可信（推翻 §10.42 的自疑），且定位到「页面一出生就是只读」

按 §10.42 的结论，先去**验证观测**而不是继续改代码。在拒绝写缺页处临时打印：
（a）`mm_find_vma(stval)` 现场返回的 VMA 指针与 `covers = v->start <= stval < v->end`；
（b）以 `stval` 为中心、±2 页的**实测**叶子权限。

```
[MM-OBS] va=a473cf20 acc=1 vma=0xffff80007eb939c0 covers=1
         v[a471c000,a473d000) vflags=13 vpte=467 present=1
[MM-OBS]   i=-2 page va=a473a000 pte=0                     W=0
[MM-OBS]   i=-1 page va=a473b000 pte=0                     W=0
[MM-OBS]   i=0  page va=a473c000 pte=800000007eecb425      W=0
[MM-OBS]   i=1  page va=a473d000 pte=0                     W=0
[MM-OBS]   i=2  page va=a473e000 pte=0                     W=0
```

**结论一：`covers=1`，§10.42 的自疑被推翻。** dump 里的 VMA 确实覆盖 `stval`，观测可信。
（「PTE 只读 + VMA 可写」是真的，不是打印时机或取值造成的假象。）

**结论二（本次真正的新事实）：这是一个 33 页的可读可写匿名 VMA，而它整个邻域里
*只有一个* 页是已映射的——恰好就是出错那一页，且它是只读；其余页 `pte=0`（不存在）。**

由此可以**一次性结掉整条 mprotect / VMA 生命周期线索**，理由是结构性的、不再是猜测性的：

* 出错页在该 VMA 内的偏移是 `0xa473c000 - 0xa471c000 = 0x20000`，即第 9 页（共 33 页），
  **既不在边界、也不在任何 split/merge 接缝附近**——所以 §10.41/§10.42 排查的
  `mm_split_vma_at()` / `vma_try_merge()` 在此处**根本没有生效的机会**。
* 邻域 32 页 `pte=0`，意味着 mprotect 的逐页循环在这个范围里**无页可改**；即使执行过，
  也不可能只把这一页留下旧权限。

**所以「只读 PTE + 可写 VMA」不是事后被谁改坏的，而是这一页在被 provision 时就带着只读
权限出生了。** 六次被证伪的假设（权限位集合 / `PTE_U` / 状态未刷新 / 跳过大 leaf /
VMA 创建点 / VMA 拆分合并）**全部**属于「事后改坏」这一类，因此全部与本现象无关。

**唯一尚未被检查过的路径是 `mm_pt_provision_anon()`（`kernel/mm/pt.c:951`）**，它在 mmap
时把一整段匿名范围预标记为 `status_byte(MM_ST_ANON_VIRT, flags)`。缺陷只可能在这两个
地方之一：

1. **调用点传入的 `flags` 与覆盖它的 VMA 的 `pte_flags` 不一致**——例如按 `PROT_READ`
   传参、或取了某个默认/模板值，而不是取该 VMA 真实的 `pte_flags`。
2. **`status_byte()` 的 class/prot 打包**把可写位丢了。

`mm_anon_provisioned=53`（累计）而本 VMA 有 33 页、却只落了 1 页，也与
`mm_pt_provision_anon()` 里 `if (span / PAGE_SIZE > g_anon_prov_max) return 0;` 的
"太大就不预标记、改走 VMA 缺页路径" 的行为一致——说明未预标记的页走 VMA 路径是好的
（那条路径从未出问题），**只有被预标记的页会带错权限出生**。

**下一步（明确且从未做过）**：把 `mm_pt_provision_anon()` 的**全部调用点**列出来，逐个核对
传入的 `flags` 是否等于该范围内 VMA 的 `pte_flags`；并重读 `status_byte()` 的打包逻辑。
这是一个范围极小、可穷举的检查，与前六次「结构上不可能」的排除不同。

**本节诊断为临时插桩，已在记录后从 `kernel/mm/fault.c` 移除**（`git checkout`），
不留调试打印在缺页热路径上。

> 插桩过程中还暴露了一个小坑：本内核的 `kerr` 不支持 `%+d`，导致 varargs 整体错位
> （首轮输出里 `va=fffffffe` 其实是 `i=-2`）。改成 `%d` 后数据才正确。
> **教训：新增诊断格式串只用 `%d/%x/%lx/%p`，不要用 `+`/`-`/`0` 标志。**

**riscv64 侧完全不受影响**：5 架构 + 2 NOMMU 变体构建通过、`smoke-mm-stress` /
`smoke-mm-fork-exec-race` / `check-mm-lock-model` 三个门全通过、关机审计全 0（含 `safe=0`）、
状态路径 2836 次缺页正常、预标记开/关性能中性（§10.34）。

### 10.44 预标记本身自洽；真正的发现是「状态权限一旦 fault 就被永久冻结」

按 §10.43 的结论，穷举 `mm_pt_provision_anon()` 的三个调用点，核对传入的 `flags` 与
该范围内 VMA 的 `pte_flags`：

| 调用点 | 传入 `flags` | 同函数里写入 VMA 的值 | 是否一致 |
|---|---|---|---|
| `mmap.c:197` | `ptef` | `vma->pte_flags = ptef;`（`mmap.c:163`） | **一致** |
| `elf.c:183` | `pte_flags` | `vma->pte_flags = pte_flags;`（`elf.c:170`） | **一致** |
| `munmap.c:275` | `mm_user_brk_pte_flags()` | `vma->pte_flags = mm_user_brk_pte_flags();`（`munmap.c:270`） | **一致** |

且 `mm_pt_prot_bits()` 逐位正确打包 `PTE_R/W/X`，`status_byte()` 只做
`cls | COW | prot` 的组合，**不会丢可写位**。所以**预标记阶段在结构上不可能**造成
「VMA 可写、状态只读」——三处都把同一个值同时写进了 VMA 与状态。§10.43 猜测的
两个候选（调用点 `flags` 不一致 / `status_byte` 丢位）**双双排除**。

**但这一轮读代码带出一个此前没被注意到的、决定性的性质**：

```c
int mm_pt_refresh_absent_prot(pte_t *table, int idx, pte_t ptef) {
    ...
    uint8_t cls = MM_ST_GET_CLASS(*slot);
    if (cls != MM_ST_ANON_VIRT)
        return 0;                       /* <-- 关键 */
    *slot = ... mm_pt_prot_bits(ptef);
}
```

而 §10.39 现场读回的状态是 **`cls = 2`，即 `MM_ST_ANON_MAPPED`，不是 `MM_ST_ANON_VIRT`**。

也就是说：**一个预标记页一旦被 fault 过、状态类从 `ANON_VIRT` 翻成 `ANON_MAPPED`，
它的状态权限位就再也没有任何代码路径能改写了。** 唯一会改写状态权限的函数对已 fault 的
页直接 early-return。此后该页的权限完全由 PTE 决定，状态权限只是一份不再更新的快照。

这解释了为什么前六次修复全部无效：它们都在试图修「事后改坏」的路径，而现象根本不是
事后改坏的。结合 §10.43 的邻域证据（33 页 VMA 里只有出错那一页已映射、其余 32 页
`pte=0`），唯一自洽的时序是：

1. 某段匿名范围被预标记，`ANON_VIRT` + 当时的 VMA 权限（**此刻状态与 VMA 必然一致**）；
2. 某次 `mprotect` 把这段 VMA 改成**只读**；此时页**尚未 fault**，`mm_pt_refresh_absent_prot`
   生效（`cls` 还是 `ANON_VIRT`），状态跟着改成只读——**仍然一致**；
3. 该页被 fault，状态类翻成 `ANON_MAPPED`，按当时（只读）的状态装出只读 PTE——**仍然一致**；
4. 此后该页状态权限**被冻结**（上面那段 early-return）；
5. 某次 `mprotect` 把 VMA 改回**可写**。页是 present，走 mprotect 的「已映射」分支，
   应当更新 PTE……**但现场 PTE 仍是只读**。

所以全部矛盾收敛到**第 5 步**：一次把已映射页所在 VMA 改成可写、却**没有**把该页 PTE
改成可写的 `mprotect`。而 §10.38 与 §10.40 两次修 mprotect 后「计数一字未变」，说明这两次
修改**没有改变实际执行的代码路径**（`level > 0` 从未命中、NULL 指针也从未命中），因而
既没修好、也没掩盖——**真正的缺陷还在 mprotect 里，只是不在我改的那两处**。

**下一步因此收窄到一个点、且是可穷举的**：审计 mprotect 的「已映射」分支在什么条件下
会**只改 `v->pte_flags` 而跳过 `*pte` 的写入**。候选包括（但需逐个读码确认，不能再猜）：
循环条件与 `va` 推进是否在某处提前 `break`/`continue`；`pt_lookup_leaf()` 返回
`level > 0` 时的降级失败分支；以及 `mm_pte_flags_apply_prot()` 之后是否还有把 `ptef`
重置成只读的第二处赋值。

**并且**：`mm_pt_refresh_absent_prot()` 那个 `cls != MM_ST_ANON_VIRT` 的 early-return 本身
值得单独审视——它意味着「已 fault 的预标记页」在 mprotect 下**完全依赖 PTE 正确**，
失去了状态这份冗余。是否应当在 mprotect 的已映射分支里**同时**刷新状态权限（而非依赖
early-return），属于设计取舍，需要在定位到第 5 步的真正缺陷之后再决定。

**riscv64 侧完全不受影响**：5 架构 + 2 NOMMU 变体构建通过、`smoke-mm-stress` /
`smoke-mm-fork-exec-race` / `check-mm-lock-model` 三个门全通过、关机审计全 0（含 `safe=0`）、
状态路径 2836 次缺页正常、预标记开/关性能中性（§10.34）。

### 10.45 mprotect 被证伪，并**推翻 §10.39「三向分叉」的框架本身**

不再猜，改做两件事：(a) 在 mprotect 内加守卫，(b) 回头核对 §10.39 的读数是怎么来的。

#### (a) 守卫：mprotect 从未把「已映射只读页」留在「可写 VMA」之下

在 mprotect 的逐页循环之后、`v->pte_flags` 改写之前插入：

```c
if (ptef & PTE_W) {
    for (q = v->start; q < v->end; q += PAGE_SIZE) {
        pte_t *l = pt_lookup_leaf(mm->pgdir, q, NULL, NULL, NULL);
        if (l && (*l & PTE_V) && !(*l & PTE_W)) { kerr("[MM-MP] ..."); break; }
    }
}
```

即「mprotect 每次把某个 VMA 变可写时，把该 VMA 的**每一个**页都扫一遍，若存在
**已映射且只读**的页就报出来」。

**结果：`[MM-MP]` 命中 0 次，而崩溃照旧发生**（`fatal=1`）。

结合 §10.41（全部显式 `vma->pte_flags` 写入点排除）与 §10.42（拆分/合并排除），
**VMA 权限一侧至此被穷尽排除**。

#### (b) §10.39 的「状态与 PTE 自洽」是**同义反复**，不是证据 —— 这是本节最重要的修正

§10.39 一直把现场读数当作「三向分叉」：
`PTE 只读` / `per-PTE 状态只读` / `VMA 可写`，并据此推出「PTE 与状态一致、只有 VMA 不同」。

**但 `mm_pt_query()` 在返回前会用 PTE 覆写状态里的权限位**（`kernel/mm/pt.c:1095-1099`）：

```c
uint8_t byte = mm_pt_peek(table, 0, idx);
if (MM_ST_GET_CLASS(byte) == MM_ST_INVALID)
    byte = MM_ST_CLS_BYTE(MM_ST_ANON_MAPPED);
byte = (uint8_t)((byte & (uint8_t)~MM_ST_PROT_MASK) | mm_pt_prot_bits(pte));
```

注意最后一行：**`prot` 是从 `pte` 算出来覆盖进去的**。所以查询输出的「状态权限」在
定义上就等于 PTE 的权限，**它永远不可能与 PTE 不一致**。

于是 §10.39 的「PTE 与 per-PTE 状态自洽」根本不是一条独立证据，而是**恒真**；
「三向分叉」这个框架从一开始就是伪的。真正存在的分叉只有一个，而且更朴素：

> **该页的 PTE 是只读，而覆盖它的 VMA 是可写。**

这也让 §10.41～§10.44 一连串推理的前提失效：既然没有「状态与 PTE 各自独立地一致」这回事，
那么「状态被冻结」「provision 三处自洽」这些结论虽然各自成立，却都与本崩溃**无关**——
它们描述的是一条根本没被触发的路径。（结论本身不错，但答的不是这个问题。）

#### 由此得到的、唯一还站得住的机制

mprotect 不会造成这种分叉（守卫 0 命中），VMA 侧其它写入点也全被排除。那么
**只读的 PTE 必定早于那个可写的 VMA 存在**——即这一页的 PTE 是**更早一次映射留下的残留**，
而后来有一个 VMA 被**建到了这个地址之上**，继承了可写权限。

§10.43 的邻域观测与这个机制高度吻合：33 页的 VMA 里**只有出错那一页**已映射，
其余 32 页 `pte=0`。若 VMA 与 PTE 是同一次建立的，不该只孤零零剩一页；
而「一段先前映射的残留 PTE 恰好落在新 VMA 内部」正好给出这种稀疏分布。

**所以排查方向要整个换掉：不再是「谁把 VMA 弄宽了」，而是「哪条路径在建立新 VMA 时，
底下还压着一张旧的、只读的 PTE」。** 具体要查两件事：

1. **VMA 拆除是否把该范围内所有 PTE 都反映射了**——`mm_pt_note_absent()` 只在
   `kernel/mm/pt.c:819/830/836` 三处被调用，需核对 munmap/brk-收缩/`vma_release`
   是否都走到了、以及是否有「只清 VMA 不清 PTE」或「只清一部分」的分支
   （尤其 `mm_pt_note_absent()` 里 `if (cls != MM_ST_INVALID && m->nr_present)` 这个
   有条件 early-return）。
2. **VMA 新建是否会覆盖到仍存在的 PTE**——`mm_pt_provision_anon()` 在
   `if (r > 0)`（被更大的 leaf 覆盖）时是 `continue` 跳过而不做任何处理；需确认
   brk 扩张（`munmap.c:262-280`）与 ELF 段加载在目标地址上已有 PTE 时的行为。

**本节诊断为临时插桩，已在记录后移除**（`kernel/mm/mprotect.c` 执行 `git checkout`），
不留调试打印在 mprotect 热路径上。

**riscv64 侧完全不受影响**：5 架构 + 2 NOMMU 变体构建通过、`smoke-mm-stress` /
`smoke-mm-fork-exec-race` / `check-mm-lock-model` 三个门全通过、关机审计全 0（含 `safe=0`）、
状态路径 2836 次缺页正常、预标记开/关性能中性（§10.34）。

### 10.46 拆除路径核对通过，x86_64 排查暂停于「新建 VMA 压着旧 PTE」

按 §10.45 定下的方向，核对「旧 PTE 是否会残留」这条线的**拆除侧**两端：

**`mm_pt_note_absent()`（`pt.c:305-324`）——正确。** 置 `*slot = 0`（回到 `MM_ST_INVALID`）、
按条件递减 `m->nr_present`、清 COW 位，并且**显式清安全位**：

```c
/* Safety bits describe the class that was just cleared, so they must go
 * with it -- otherwise a reused slot would inherit a stale UFFD or
 * NO_FA flag and the fault path would make the wrong decision. */
uint8_t *sb = safe_bit(m, idx);
if (sb) *sb &= (uint8_t)~MM_SAFE_MASK;
```

`if (cls != MM_ST_INVALID && m->nr_present)` 这个有条件递减**不是**漏清的 bug：
它只在槽位本来就持有 class 且计数非零时递减，避免下溢。**排除。**

**`mm_cursor_unmap()`（`pt.c:805-840`）——正确。** 三条分支都把 PTE 与状态一并清掉，
且专门覆盖了「预标记但从未 fault」的页：

```c
if (!(*pte & PTE_V) || !arch_pte_is_leaf(pte)) {
    if (MM_ST_GET_CLASS(mm_pt_peek(table, 0, idx)) == MM_ST_ANON_VIRT)
        mm_pt_note_absent(table, 0, idx);
    return 0;
}
```

**排除。**

所以「拆除时漏清 PTE / 漏清状态」这一族假设**在拆除侧已被排除**。§10.45 提出的第二条
（**新建** VMA 时底下压着旧 PTE）里，`mm_pt_provision_anon()` 的三个调用点已确认都把
同一个 `flags` 同时写进 VMA 与状态（§10.44），而 `mm_pt_provision_anon()` 自身在被更大
leaf 覆盖时是 `if (r > 0) { mm_cursor_unlock(&cur); continue; }`——**只解锁跳过，不做处理**，
这正是尚未读过的**第三条路径**。

**排查到此暂停，理由是已经连续七次猜错、且前六次的推理框架（§10.39）事后被证明是错的
（见 §10.45(b)）。** 在没有新证据来源的情况下继续试第八个假设，重复前七次的错误；
应当先把「新建 VMA 压着旧 PTE」这条**尚未读过的代码路径**（`mm_pt_provision_anon()` 的
`r > 0` 分支，以及 mmap/brk/ELF 三个 VMA 创建点在目标地址已有 PTE 时的处理）真正读完，
再决定是修代码还是继续查。

**这七次被证伪的假设，及其被证伪的方式，都有记录价值**：

| # | 假设 | 证伪方式 |
|---|---|---|
| 1 | 权限位集合不对（缺 `PTE_LEAF`/`PTE_NX`） | 重构前后崩溃逐字节相同（§10.36） |
| 2 | `PTE_U` 缺失 | riscv64 修好后完全正常（§10.33） |
| 3 | 状态未刷新导致装出旧权限（NULL 指针运算） | 修掉后 UBSAN 35→2，崩溃不变（§10.38） |
| 4 | mprotect 跳过大 leaf 其余条目 | 修掉后计数一字未变（§10.40） |
| 5 | VMA 创建点 / 拆分 / 合并造成分叉 | 枚举全部写入点后逐一排除（§10.41/§10.42） |
| 6 | mprotect 把已映射只读页留在可写 VMA 下 | **运行时守卫 0 命中**（§10.45a） |
| 7 | 观察到的「状态与 PTE 自洽」是独立证据 | **同义反复**：`mm_pt_query()` 用 PTE 覆写状态权限（§10.45b） |

其中 **#7 推翻了前六次推理的共同前提**，是最重要的一条：真正存在的分叉只有一个——
**该页 PTE 只读、覆盖它的 VMA 可写**——所有关于「per-PTE 状态」的分析都是无关分支。

**riscv64 侧完全不受影响**：5 架构 + 2 NOMMU 变体构建通过、`smoke-mm-stress` /
`smoke-mm-fork-exec-race` / `check-mm-lock-model` 三个门全通过、关机审计全 0（含 `safe=0`）、
状态路径 2836 次缺页正常、预标记开/关性能中性（§10.34）。

### 10.47 根因定位：`mm_pt_provision_anon()` 遇到已存在的 PTE 时直接跳过

读完 §10.46 留下的那条唯一未读路径后，**崩溃的根因找到了**（`kernel/mm/pt.c:1000-1005`）：

```c
for (vaddr_t va = lo; va < hi; va += PAGE_SIZE) {
    int idx = arch_pt_vpn(va, 0);
    if (table[idx] & PTE_V)
        continue;               /* a fault won the race; already mapped */
    pt_note_present_meta(m, idx, byte);
    marked++;
}
```

**这个 `continue` 在真正的并发竞争下是无害的**（同一段 VMA 刚被 fault 装帧，VMA 与 PTE
本来就该一致），**但在「目标地址上已经存在一张更早的、属于别的映射的 PTE」时是有害的**：

* 旧的那张**只读** PTE 被原样留下（`continue` 之前没有任何改写）；
* 旧的状态字节也被原样留下（`pt_note_present_meta()` 根本没被调用）；
* 而**新建的 VMA** 已经带着自己的 `pte_flags`（可写）插进了 `mm`——三个调用点
  （`mmap.c:197` / `elf.c:183` / `munmap.c:275`）都是在 `mm_insert_vma()` **之后**才调
  预标记的，所以 VMA 是这段地址上**更新的、更权威**的那一份声明。

于是结果就是**一张只读的残留 PTE 压在可写的 VMA 之下**——正是 §10.45 认定的唯一真实
分叉，也正是 §10.43 邻域观测的形状：**33 页 VMA 里只有出错那一页已映射，其余 32 页
`pte=0`**（新 VMA 是全新的，本来该一片空白；那孤零零的一页就是残留）。

同一函数里 `if (r > 0)`（被更大的 leaf 覆盖）分支也是 `mm_cursor_unlock(&cur); continue;`
——同样只解锁跳过、不做任何处理，属同一族的漏处理。

**七次猜测全部落空、而这一条一次命中，根因就在于前七次都在猜「谁把权限改坏了」，但
真相是「**没有人改权限，是一张旧 PTE 从头到尾没被让位**」。** 两者在现象上完全一样
（PTE 只读 / VMA 可写），但在代码上分别属于「写」和「漏写」两族——前七次全在查写，
自然全查不到。这也解释了 §10.45(a) 的守卫为什么 0 命中：mprotect 确实没留下
「已映射只读页」，因为那张只读页**根本不是 mprotect 留下的**。

#### 修法（尚未实施，需要先定一个语义问题）

直觉修法是「遇到已存在的 leaf 且权限与新 VMA 不一致时，把 PTE 的权限位改写成新 VMA 的
`flags` 并做 TLB 失效」。**但这里有一个不能靠直觉决定的安全问题**：若那张残留 PTE 是
**文件映射**（file-backed），保留它的 frame 会把文件内容泄漏进本应匿名的区域——而这正是
`elf.c` 里 `anon` 参数的注释所警告过的同一类问题：

> *Provisioning such a range made the fault path hand out anonymous zero pages instead of
> file content and killed exec with SIGSEGV, so the call site decides.*

所以三个语义选项，风险差别很大，**不应由排查过程单方面决定**：

| 选项 | 做法 | 风险 |
|---|---|---|
| **A. 改权限** | 保留 frame，只把 PTE 权限位与状态改写为新 VMA 的 `flags` | 最小改动；但若残留 PTE 是文件映射，会把文件内容暴露给匿名写者——**安全问题，不可默认采用** |
| **B. 先回收再建 VMA** | 在新建 VMA 之前，先把该地址范围上仍存在的 PTE 全部反映射（`mm_cursor_unmap` 已可用） | 语义最干净（残留映射理应消失）；改动面最大，涉及 mmap/brk/ELF 三个创建点 |
| **C. 检测并拒绝** | 发现权限不一致就返回失败，让 mmap/brk/ELF 报错而不是静默继承 | 最安全、绝不引入信息泄漏；但会让某些原本「能跑」的映射开始失败 |

**推荐 B**：它修的是「VMA 生命周期与页表不同步」这个真正的病根，而不是掩盖症状；且
`mm_cursor_unmap()` 已经能正确清除 PTE 与状态（含预标记未 fault 的分支），可复用。
但 B 会改变 mmap/brk/ELF 的行为，需确认「在这些创建点之前，该地址范围上存在 PTE」本身
是否就属非法状态——若属非法，正确做法可能反而是 C。

**排查到此结束**：根因已定位到具体的三行代码与它所处的语义问题。实施哪种修法需要先回答
「目标地址上已有 PTE 时，正确的行为是什么」，这个问题不该在连续七次猜错之后、由一次
仓促的收尾来决定。

**riscv64 侧完全不受影响**：5 架构 + 2 NOMMU 变体构建通过、`smoke-mm-stress` /
`smoke-mm-fork-exec-race` / `check-mm-lock-model` 三个门全通过、关机审计全 0（含 `safe=0`）、
状态路径 2836 次缺页正常、预标记开/关性能中性（§10.34）。默认 `g_anon_prov_max=0`，
该路径在默认构建下不激活。

### 10.48 §10.47 的根因**被自己的诊断证伪**（第 8 次），排查收束

按 §10.47 的判断加装诊断：在 `mm_pt_provision_anon()` 的逐页循环里，凡因
`table[idx] & PTE_V` 而跳过、**且该 PTE 的权限与本次要记录的 `flags` 不一致**时，打印
`[MM-PROV]`。

**结果：命中 0 次，而崩溃照旧（`fatal=1`）。** §10.47 被证伪。

这条零命中**本身是有信息量的**，它排除了一整族可能：预标记阶段从来没见过
「一张已存在、且权限与新 VMA 冲突的 PTE」。结合 §10.43 的邻域观测（33 页 VMA 里
**只有出错那一页已映射**，其余 32 页 `pte=0`），可以推出：

> **出错的那一页，是在预标记**之后**才变成"已映射"的——也就是被某次缺页装上去的。**

而 §10.45(a) 已经证明 mprotect 不会把「已映射只读页」留在「可写 VMA」之下。
于是矛盾进一步收紧到**缺页安装**这一步。

#### 缺页安装这一步读码的结论

状态路径（`kernel/mm/fault.c:960-993`）**完全不查 VMA**，权限只从记录的状态字节还原：

```c
if (cls_byte & MM_ST_PROT_R) prot |= 1;
if (cls_byte & MM_ST_PROT_W) prot |= 2;
if (cls_byte & MM_ST_PROT_X) prot |= 4;
pte_t allow = mm_prot_to_pte_flags(prot);
...
mm_cursor_map(&qcur, page_va, pfn_to_phys(np), allow, MM_ST_ANON_MAPPED);
```

这正是论文的设计（SS6.2「省掉 Linux 花在 VMA 上的时间」），也意味着
**只要该页的状态字节是「只读」，缺页就一定会装出只读的 PTE，不管 VMA 说什么。**
而 §10.45(b) 已证明「状态与 PTE 一致」是同义反复，所以现在唯一待解的问题只有一个：

> **谁把该页的状态权限改成了「只读」，却没有同步把 VMA 改回只读？**

写状态权限的函数**全仓库只有一个**：`mm_pt_refresh_absent_prot()`（`pt.c:857`），
它只被 mprotect 的「该页尚未映射」分支调用，且带一道早退：

```c
if (cls != MM_ST_ANON_VIRT) return 0;
```

**而 mprotect 的同一个分支会无条件执行 `v->pte_flags = mm_pte_flags_apply_prot(...)`。**
这两行在同一次调用里、传的是同一个 `ptef`，因此**单次 mprotect 内部不可能分叉**——
分叉只可能来自**两次不同的 mprotect**：前一次把状态刷成只读，后一次把 VMA 放宽回可写
却因为上面那道早退（或 `pte == NULL`，`pt.c` 注释所述「没有叶子表就没有状态可刷」）
而**没有刷状态**。

这是本次排查得到的**唯一一条尚未被实验排除、且在代码里确有对应早退的机制**。
但要把它钉死为根因，还需要一次专门的实验（记录该页的状态字节在 VMA 放宽前后的值），
而这已经超出本轮排查的合理成本。

#### 收束

**连续 8 次猜测全部被各自的诊断证伪。** 其中 §10.45(b) 推翻了前 6 次的共同前提、
§10.47 推翻了第 7 次，代价是这些结论虽然各自正确，却都在描述**没有被触发**的路径。

**停止继续猜。** 理由已经充分：每一次「读码 → 提出假设 → 加诊断 → 证伪」都真实推进了
认知（尤其 §10.45b、§10.47、§10.48 三条排除了大片区域），但也证明**这个崩溃无法靠
读码猜出来**——它的机制发生在一次具体的运行期事件序列里，必须被**记录**而不是被**推理**。
下一步唯一合理的做法是：在 `mm_pt_refresh_absent_prot()` 与 mprotect 的 VMA 改写处，
把「该页的状态字节 / VMA 权限 / 事件」按时间序**打进环形缓冲**，复现后离线读序列，
一次性看清 VMA 与状态是在哪两个事件之间分叉的。这是**观测**问题，不是**推理**问题。

**当前可交付状态（riscv64 侧，完整且可信）**：
5 架构 + 2 NOMMU 变体构建通过、`smoke-mm-stress` / `smoke-mm-fork-exec-race` /
`check-mm-lock-model` 三个门全通过、关机审计全 0（含 `safe=0`）、状态路径 2836 次缺页
正常、预标记开/关**性能中性**（§10.34）。所有旧性能结论已撤回，文档中无残留错误数字。

**默认构建不受影响**：`g_anon_prov_max = 0`，本崩溃只在
`EXTRA_CFLAGS=-DCONFIG_ANON_PROV_DEFAULT=<n>` 的实验构建下出现。

### 10.49 命中：`vma_file=/bin/mm_stress` 说明这**根本不是**匿名页，COW 才是主线

§10.48 定下的实验（把 `mm_pt_refresh_absent_prot()` 的「没刷成」与「刷成了」区分开，
返回 1 表示 declined；mprotect 在「放宽到 RW 却 declined」时报 `[MM-DIV]`）**命中了**：

```
[MM-DIV] widened v[158d51000,158d72000) to RW but status NOT refreshed
         at va=158d51000 (pte=0)          ... 共 33 次，每页一次
SIGSEGV: pid=6 code=14 sepc=0x1e2e2 stval=0x158d71f20 abi=0
[ERR]   vma_file=/bin/mm_stress
FATAL: pid=6 signal=11 pc=0x1e2e2 comm=mm_stress path=/bin/mm_stress
```

**相关性是确凿的，不是巧合**：

* `stval=0x158d71f20` 落在 `v[0x158d51000,0x158d72000)` 内，偏移 `0x20f20`，
  即第 32 页（共 33 页）——**正是该 VMA 的最后一页**，与最后一条 `[MM-DIV]`
  （`va=158d71000`）对应；
* `[MM-DIV]` **恰好 33 次**，每个地址一次，`pte=0` 即**当时连叶子表都还不存在**；
* `code=14` → x86_64 错误码 `0xE` = **present + write + user**，即「该页已映射、可写访问」；
* **`vma_file=/bin/mm_stress`**——这段 VMA 是**可执行文件自己的文件映射**，不是堆、不是 brk。

#### 这推翻了前八次排查赖以成立的前提

对 **file-backed private** 的 VMA，mprotect **故意**把 PTE 留成只读并置 `PTE_COW`
（`mprotect.c:120-133`）：

```c
if ((ptef & PTE_W) && (v->vm_flags & VM_FILE) && !(v->vm_flags & VM_SHARED)) {
    ...
    /* Keep the canonical cache page read-only.  The first
     * store will copy it in handle_cow_fault(). */
    flags &= ~(uint64_t)(PTE_W | PTE_D);
    flags |= PTE_COW;
}
```

也就是说：**「可写 VMA 之下压着一张只读 PTE」在这种 VMA 上是设计上的正确状态**，
等第一次写时由 `handle_cow_fault()` 收尾。

于是 §10.45(a) 那个守卫——「mprotect 放宽到 RW 后，VMA 里若存在已映射只读页就报警」——
**量的是错的东西**：对文件私有 COW 段，这种情况本来就该发生，守卫理应静默。
把它的静默读成「mprotect 无关」，是把**符合设计的行为误判成了缺陷**。§10.47、
§10.48 建立在该结论上的推理也随之失效。

另外，§10.39 读到的 `cls=2 (MM_ST_ANON_MAPPED)` 同样是假象：`mm_pt_query()` 在
`MM_ST_GET_CLASS(byte) == MM_ST_INVALID` 时会**凭空合成** `MM_ST_ANON_MAPPED`
（`pt.c:1097`），所以一个**没有状态记录的文件页**读回来就是 `ANON_MAPPED`。
「这是匿名页」这个判断，从一开始就是查询函数伪造出来的。

#### 真正的问题因此变成一句话

> **写故障落在了文件私有 COW 段的最后一页上，而 `handle_cow_fault()` 没有接手，
> 直接变成了致命 SIGSEGV。**

这与前面八次假设**完全不同类**：不是「权限被谁改坏」，而是**COW 收尾路径没被走到**。

而 `[MM-DIV]` 给出的 `pte=0` 正是关键线索：**mprotect 走这一段时，这些页连叶子表都还没有**，
所以 mprotect 的「已映射」分支（含上面那段置 `PTE_COW` 的代码）**一次都没执行**。
这些页后来是由**缺页路径**装上去的。**因此真正的缺陷极可能是：
`PTE_COW` 只在 mprotect 的已映射分支里被设置，而由缺页路径首次装帧的文件私有页
没有带上 COW 标记**——于是它的第一次写永远等不到 `handle_cow_fault()`，直接致命。

**下一步（明确、单点、可验证）**：读 `handle_cow_fault()` 的触发条件，确认它靠什么识别
「这是一次需要 COW 的写故障」（是查 `PTE_COW` 位，还是查 VMA 的 `VM_FILE && !VM_SHARED`），
再检查**缺页安装**文件私有页的那条路径有没有置 `PTE_COW`。若缺，就补上——这既是最小修复，
也正好解释了为什么前八次全错：它们都在查「只读 PTE 从哪来」，而答案是「**它本来就该
是只读的**，错的是它永远等不到 COW 收尾」。

**本节诊断为临时插桩，结论确定后应移除**（`kernel/mm/pt.c` 与 `kernel/mm/mprotect.c`）。

**riscv64 侧完全不受影响**：5 架构 + 2 NOMMU 变体构建通过、三个门全通过、关机审计全 0
（含 `safe=0`）、状态路径 2836 次缺页正常、预标记开/关性能中性（§10.34）。

### 10.50 根因：私有文件缺页装出的页**只读且不带 `PTE_COW`**，第一次写无人接管

三处代码合起来即闭环：

**(1) 出错页的 PTE 是 `0x425`，而 `PTE_COW = 1UL << 9 = 0x200`（`x86_64/include/page_table.h:26`），
`0x425 & 0x200 == 0`——`PTE_COW` 没有置。** 该页 present、user、leaf、**只读**。

**(2) `handle_cow_fault_locked()` 只认两种情况**（`fault.c:150` 起）：

```c
if (*pte & PTE_COW) { ... 复制 ... }
if (*pte & PTE_W)    { ... 只补 PTE_D ... }
return -1;                       /* 既非 COW 又不可写 → 放弃 */
```

我们的页**既没有 `PTE_COW`、也没有 `PTE_W`**，于是落到末尾 `return -1`，
**写故障无人接管，直接致命**。这与观测完全吻合：`code=14` → 错误码 `0xE` =
**present + write + user**（页确实在、确实是写）。

**(3) 私有文件缺页安装这条路径（`fault.c:428-448`）根本没有 COW 的概念**：它老老实实
`memcpy` 出一份私有副本，然后

```c
int r = fault_map(t->mm, page_va, copy, vma->pte_flags, MM_ST_FILE_PRIVATE);
```

**就这样把页装上了——没有像 `mprotect.c:120-133` 那样「清 W、置 `PTE_COW`、让第一次写去
`handle_cow_fault()` 收尾」。** 而 `mprotect` 那段置 COW 的代码位于它的**已映射**分支里，
本次运行中这些页 `pte=0`（连叶子表都没有），**该分支一次都没执行**。

于是链条完整闭合：

> 私有文件段的页由**缺页路径**首次装帧 → 该路径**只给只读、不置 `PTE_COW`** →
> 进程第一次写它 → `handle_cow_fault()` 两个分支都不匹配 → `return -1` → **致命 SIGSEGV**。

#### 为什么前八次全错

八次假设都在问「这张只读 PTE 是**谁**弄出来的、为什么没跟着 VMA 变可写」。
但对**文件私有 COW 段**，「只读 PTE」**本来就是设计要求的中间状态**——它必须等
`handle_cow_fault()` 被触发的那一刻才变可写。真正缺的不是「谁把它改可写」，
而是**「第一次写时谁来收尾」**。问错了问题，八次自然全错。

三处早前的误判也一并得到解释：
* §10.45(a) 守卫静默 → 因为「可写 VMA 下的只读页」在 COW 段是**正常**的，守卫量错了对象；
* §10.39 的 `cls=2 (ANON_MAPPED)` → `mm_pt_query()` 在 `cls==MM_ST_INVALID` 时**凭空合成**
  `ANON_MAPPED`（`pt.c:1097`），文件页因此被误报成匿名页；
* §10.47「预标记跳过已存在 PTE」→ 真实现象，但那 33 次 `pte=0` 说明这些页当时**根本不存在**，
  与残留 PTE 无关。

#### 修复方向（明确，但**尚未实施**）

对称于 `mprotect.c:120-133`，在 `fault_map()` 装**私有文件**页时：清掉 `PTE_W|PTE_D`、
置上 `PTE_COW`，让第一次写按既有机制走 `handle_cow_fault()`。这样 COW 的置位点就有
**两处**（mprotect 与缺页），而不是只有 mprotect 一处——后者正是本次缺陷的结构性成因：
**COW 标记的建立只挂在「已映射」路径上，首次装帧的路径被漏掉了。**

**实施前必须先确认的一件事**（本轮上下文已尽，未做）：`fault_map()` 内部把 `vma->pte_flags`
变成实际 PTE 时，**`PTE_W` 是在哪一步被丢掉的**——因为 VMA 的 `pte_flags=0x467` 是**含 W 的**，
而装出来的 PTE `0x425` 不含 W。这决定了修复应当落在 `fault_map()` 内部（按 class 处理），
还是落在调用点（传入已清 W 的 flags）。这个定位需要读 `fault_map()`，不应凭猜测下手。

**本节诊断插桩（`kernel/mm/pt.c` 的 declined 区分、`kernel/mm/mprotect.c` 的 `[MM-DIV]`）
在结论确定后应移除。**

**riscv64 侧完全不受影响**：5 架构 + 2 NOMMU 变体构建通过、三个门全通过、关机审计全 0
（含 `safe=0`）、状态路径 2836 次缺页正常、预标记开/关性能中性（§10.34）。

### 10.51 安装链已读通：`PTE_COW` 在整条链上**从未被置位**

顺着 §10.50 的修复前置条件，把私有文件页的安装链一路读到底：

| 环节 | 位置 | 是否处理 `PTE_COW` |
|---|---|---|
| 私有文件缺页准备副本 | `fault.c:428-447`（`memcpy` + icache） | **否** |
| `fault_map()` | `fault.c:~470` | **否**，纯转发 `flags` |
| `mm_cursor_map()` | `pt.c:795-802` | **否**，纯转发 `flags` |
| `mm_cursor_replace()` | `pt.c:786` | **否**，`*pte = arch_pte_leaf(pa, flags);` 原样写入 |
| `status_byte(cls, flags)` | `pt.c:~840` | 只在 `PTE_COW` 已置时才记 `MM_ST_COW_BIT` |

**结论：整条安装链没有任何一处会为私有文件页置 `PTE_COW`。** 唯一置位点是
`mprotect.c:120-133` 的**已映射**分支。因此只要一个私有文件段**先被 mprotect 放宽权限、
之后才首次缺页**（本次正是如此：`[MM-DIV]` 的 33 次全部 `pte=0`），它的页就永远拿不到
`PTE_COW`；而 `handle_cow_fault_locked()` 又只认 `PTE_COW` 或 `PTE_W`，两者皆无即
`return -1`——**缺陷的结构性成因至此完全确认**：

> **COW 标记的建立只挂在「页已映射」路径上；「页首次装帧」路径上从未建立。**

#### 仍未解决的一个细节（需下一次实验，不要凭猜测下手）

观测到的 PTE 是 `0x425`（无 `PTE_W`），而 §10.43 读到的那次 VMA `pte_flags=0x467`（**含
`PTE_W`**）——但**这是两次不同的运行**，`[MM-DIV]` 那次运行从未打印 `vma->pte_flags`。
所以「`PTE_W` 是在哪一步丢的」**尚未确定**，两种可能都还活着：

1. `arch_pte_leaf()` 在 x86_64 上按某种方式滤掉了 `PTE_W`；
2. 调用点传入的 `vma->pte_flags` 在缺页当时**本来就不含 `PTE_W`**（例如该段先被
   `mprotect(PROT_READ)` 收窄过、随后又被放宽，而 VMA 上的 `pte_flags` 与实际不符）。

**决定性实验（一次即可）**：在 `fault.c:445` 的 `fault_map(... MM_ST_FILE_PRIVATE)` 处打印
`vma->pte_flags` 与传入的 `flags`，并同时打印 `PTE_COW`/`PTE_W` 位。缺 `PTE_COW` 这一点
**已经确证**；`PTE_W` 的去向**尚待确证**。二者的修法落点不同：
* 若 `vma->pte_flags` 本就含 W 而 PTE 里没有 → 修 `arch_pte_leaf()` 或安装点的掩码；
* 若 `vma->pte_flags` 本就不含 W → 修 VMA 权限与 `pte_flags` 的同步（另一处 bug）。

**因此本节不实施修复。** 缺 `PTE_COW` 的结论已足够坚实，但把它写进去之前必须先确定
`PTE_W` 的去向——否则很可能修好 COW 却留下「页仍然只读」的第二个症状，浪费一次验证。

**当前状态**：诊断插桩仍在 `kernel/mm/pt.c`（declined 区分）与 `kernel/mm/mprotect.c`
（`[MM-DIV]`），**故意保留**，下一次实验要一并复用来打印 `vma->pte_flags`。
HEAD `3258ebe3` 已记录根因机制；工作树仅含这两处已知插桩。

**riscv64 侧完全不受影响**：5 架构 + 2 NOMMU 变体构建通过、`smoke-mm-stress` /
`smoke-mm-fork-exec-race` / `check-mm-lock-model` 三个门全通过、关机审计全 0（含 `safe=0`）、
状态路径 2836 次缺页正常、预标记开/关性能中性（§10.34）。默认 `g_anon_prov_max=0`。

### 10.52 §10.50 的**归因**被证伪（代码缺陷本身仍成立）

§10.51 要求的一次性实验已执行：在 `fault.c` 私有文件缺页的
`fault_map(..., MM_ST_FILE_PRIVATE)` 调用点打印 `vma->pte_flags`、传入的 `flags`、
`PTE_COW`/`PTE_W` 位与当前 PTE（`[MM-FP]`）。

**结果：命中 0 次，而崩溃照旧**（`stval=0xa2194f20`，`code=14`）。也就是说
**`fault_map(..., MM_ST_FILE_PRIVATE)` 在整个 `mm_stress` 运行中一次都没有被调用**。

因此 §10.50 把这次崩溃归因到「私有文件缺页装出的页不带 `PTE_COW`」是**错的**：
出错那一页**根本不是这条路径装上去的**。这条路径上「首次装帧不置 `PTE_COW`」的
**代码缺陷本身依然成立**（§10.51 的读码结论与运行时无关），但它**不是本次崩溃的成因**。

这次实验同时否掉了「先收紧再放宽导致 `vma->pte_flags` 与实际不符」这条支线：
`[MM-FP]` 一次没打，说明**根本没有私有文件页走到缺页**，也就无从比较两者——
`PTE_W` 的去向问题**仍然悬空**，而且现在多了一个新事实：出错页是**被别的路径**装上去的。

#### 目前唯一与运行时无关、且已确证的结论

`handle_cow_fault_locked()` 只认 `PTE_COW` 或 `PTE_W`，两者皆无即 `return -1`；
而出错页 `0x425` **两者皆无**，VMA 却可写，故第一次写必然致命。
**这个「COW 页既无 COW 位又不可写 ⇒ 无人接管」的结构是确证的**，
但**装出这种页的代码路径尚未找到**——它不是私有文件缺页（已证伪），也不是
预标记跳过（已证伪）、也不是 mprotect（守卫已证伪）、也不是状态路径
（`[MM-PROV]` 已证伪无权限冲突）。

**这是第 9 次归因失败。** 停止继续猜路径。剩下的可行办法只有一条，且必须是**观测**：
在**所有** `mm_cursor_map()` / `mm_cursor_replace()` 的调用点（或在 `mm_cursor_replace()`
内部按 `cls` 打点）记录「该页是由哪个 cls、由哪个调用点装上去的」，
复现后直接读出那条路径，而不是继续在代码里猜。这一步应当在下一次有充足上下文时进行。

**插桩现状**（故意保留，供下一次实验复用）：
`kernel/mm/pt.c`（declined 区分）、`kernel/mm/mprotect.c`（`[MM-DIV]`）、
`kernel/mm/fault.c`（`[MM-FP]`）。三者均为 `kerr`，量级为每次运行几十行，可接受。

**riscv64 侧完全不受影响**：5 架构 + 2 NOMMU 变体构建通过、三个门全通过、关机审计全 0
（含 `safe=0`）、状态路径 2836 次缺页正常、预标记开/关性能中性（§10.34）。
默认 `g_anon_prov_max=0`，本崩溃只在实验构建下出现。

### 10.53 运行时轨迹给出**第一个有证据的根因**（第 9 次猜测失败后的转机）

在 `mm_cursor_replace()` 里对**每一次安装**打点（`[MM-INS] va= cls= flags= W= cow=`），
一次运行的结果（29 次安装）：

```
[MM-INS] va=71000      cls=2 flags=467 W=1 cow=0
[MM-INS] va=4ee000     cls=2 flags=467 W=1 cow=0
... 共 28 次，全部 flags=467 W=1 ...
[MM-INS] va=abd17000   cls=2 flags=425 W=0 cow=0     <<< 唯一一次 W=0
SIGSEGV: pid=6 code=14 stval=0xabd17f20
```

`stval=0xabd17f20` 的页基址正是 `0xabd17000`——**最后一条安装记录就是出错的那一页**。

#### 这一条轨迹同时钉死了三件事

1. **它不是 `PTE_COW` 问题。** 该页 `cow=0` 且 `W=0`，但根因是**权限位本身就少了 `PTE_W`**：
   同一 VMA 的 28 个兄弟页全部以 `flags=0x467`（含 `PTE_W`）装上，只有它以 `0x425` 装上。
   `0x467 & ~0x425 = 0x42`，即**只差 `PTE_W`（0x2）与 0x40**。§10.50/§10.51 关于
   `PTE_COW` 的整条线索是**无关分支**，纯属 `handle_cow_fault()` 那个 `return -1` 把人
   误导过去的假象。

2. **装它的是状态路径（per-PTE status），不是 VMA 路径。** 状态路径是全仓库**唯一**
   不从 VMA 取权限、而是把记录的状态字节**回环**成 PTE 权限的地方
   （`fault.c:968-980`：`prot` → `mm_prot_to_pte_flags(prot)`）：

   ```c
   if (cls_byte & MM_ST_PROT_R) prot |= 1;
   if (cls_byte & MM_ST_PROT_W) prot |= 2;
   if (cls_byte & MM_ST_PROT_X) prot |= 4;
   pte_t allow = mm_prot_to_pte_flags(prot);
   ```

   `mm_prot_to_pte_flags(1)`（只读）**正是 `0x425`**。而其余 28 页都由 VMA 路径装、
   带着 `0x467`。`mm_fault_from_status=12` 与「唯一一次 `W=0` 的安装来自状态字节」相符。
   （严格确认只需在状态路径上加一个独立标记，成本一次运行。）

3. **所以真正的分叉是：这一页的 per-PTE 状态记录了「只读」，而它所在的 VMA 是可写。**
   状态路径**忠实地**按状态装了只读页——它没有错，**是状态本身是错的**。

#### 于是问题收敛成一个，且已有唯一候选

**「谁把这一页的状态改成了只读，却没有同步 VMA？」**

写状态权限的函数**全仓库只有一个**：`mm_pt_refresh_absent_prot()`（`pt.c:857`），
只被 mprotect 的「该页尚未映射」分支调用。而 mprotect 在**同一个分支之后**会
**无条件**执行 `v->pte_flags = mm_pte_flags_apply_prot(v->pte_flags, ptef);`。
两者传同一个 `ptef`，所以**单次 mprotect 不会分叉**；分叉只能来自**两次 mprotect**：
前一次把状态刷成只读，后一次把 VMA 放宽回可写却**没能刷新状态**——而它有两条现成的
「刷不成」路径，且 `[MM-DIV]` 已经证明**其中一条在本次运行中真实发生过 33 次**
（`pte=0`，即 `pt_lookup_leaf` 返回 NULL、调用点直接跳过刷新）：

* `pte == NULL` → 调用点 `if (pte)` 为假，**根本没调用刷新**（`mprotect.c:162-165`）；
* `cls != MM_ST_ANON_VIRT` → 刷新函数自己 `return 1` 早退（`pt.c:867`）。

#### 修复方向（**证据已足，但本轮不做**）

根本问题是**状态与 VMA 之间没有一致性约束**：状态路径信任状态字节到「不查 VMA」的程度，
而状态字节又只有 mprotect 会在缺页时刷新、且刷新失败时**不报错**。两条可选修法：

* **A（治标，最小）**：mprotect 在「放宽为可写却没能刷新状态」时，**不要**只 `kerr`，
  而是把该页状态**改写为 VMA 的权限**（或直接把状态置为 `INVALID`，让下一次缺页回落到
  VMA 路径）。语义上等价于「VMA 是权威，状态只是缓存」——与 Linux 一致。
* **B（治本）**：让状态路径在 `ANON_VIRT` 命中时**与 VMA 交叉校验**，不一致就放弃状态路径、
  回落 VMA 路径。代价是每次状态缺页多一次 VMA 查找，与论文「省掉 VMA 查找」的主张相悖，
  **与本项目目标冲突，不建议**。

**推荐 A**：它承认了「VMA 权威、状态缓存」这个本该早就成立的约定，
并把当前**静默**的分叉变成**不可能**的分叉。

**未做完的收尾**（下一次务必先做，成本极低）：
1. 在状态路径（`fault.c:993` 附近）加独立标记，确证 `0x425` 那次确实来自它；
2. **移除全部 TEMP 插桩**：`pt.c`（`[MM-INS]`、declined 区分）、
   `mprotect.c`（`[MM-DIV]`）、`fault.c`（`[MM-FP]`）——三处均为热路径 `kerr`，
   修复验证通过后必须清理；
3. 实施 A 之后重跑：期望 x86_64 `mm_stress PASS`、`mm_anon_provisioned=53`、
   `mm_fault_from_status>0`、MM-ASM 全 0；再回归 riscv64 三门与 5 架构构建。

**riscv64 侧完全不受影响**：5 架构 + 2 NOMMU 变体构建通过、三个门全通过、关机审计全 0
（含 `safe=0`）、状态路径 2836 次缺页正常、预标记开/关性能中性（§10.34）。

### 10.54 完整时序捕获，机制闭环（`[MM-RF]` → `[MM-ST]` → `[MM-INS]`）

给「状态刷新尝试」和「状态路径安装」各加一个标记后，出错页 `0x8dc0f000` 的**全部**记录
只有三条，顺序如下（本次运行 `ST=24 RF=33 INS=29 DIV=33 FP=0`）：

```
[MM-RF]  va=8dc0f000 ptef=467 declined=1 pte=0
[MM-ST]  va=8dc0f000 status_prot=1 allow=425 W=0 cow=0
[MM-INS] va=8dc0f000 cls=2 flags=425 W=0 cow=0
SIGSEGV: pid=6 code=14 stval=0x8dc0ff20
```

逐行读：

1. **`[MM-RF]`**：mprotect 要把这一页的状态刷成 `ptef=0x467`（**含 `PTE_W`**，可写），
   但 **`declined=1` 且 `pte=0`**——`pt_lookup_leaf()` 返回 **NULL**，调用点的
   `if (pte)` 为假，**刷新被整段跳过**。而循环之后的
   `v->pte_flags = mm_pte_flags_apply_prot(v->pte_flags, ptef)` **照样执行**，
   VMA 被放宽成可写。**分叉就在这一行产生。**
2. **`[MM-ST]`**：随后缺页走状态路径，读到 `status_prot=1`（**只有 R**），
   回环出 `allow=0x425`（无 `PTE_W`）。
3. **`[MM-INS]`**：照此装帧，得到一张只读页，压在可写 VMA 之下。
4. 第一次写 → `handle_cow_fault()` 既无 `PTE_COW` 又无 `PTE_W` → `return -1` → **致命**。

至此 §10.53 的推断**被运行时轨迹逐行证实**：状态路径没有错，**是状态陈旧而 VMA 已放宽**。

#### 修复（现在可以动手了，方向唯一）

**「状态」与「VMA」之间缺少一致性约束**。状态路径按设计**不查 VMA**（这是论文的卖点），
因此它把状态字节当作绝对权威；可是状态字节**只在 mprotect 的缺页分支里被刷新，
且刷新失败时不报错、不回退**。`[MM-DIV]` 的 33 次 `pte=0` 证明这条「静默失败」在一次运行里
发生了 33 次。

**最小且语义正确的修法**：在 mprotect 的缺页分支里，当 `declined` 为真时，
**不要保留可能陈旧的状态**——把该页的状态置为 `INVALID`，让下一次缺页回落到 VMA 路径
（VMA 是权威，与 Linux 语义一致）。当 `pte == NULL` 时本就没有元数据可清，
而新建叶子表的元数据本就是 `INVALID`，所以这条路径**自动安全**；真正需要显式处理的是
`pte != NULL` 但 `mm_pt_refresh_absent_prot()` 因 `cls != MM_ST_ANON_VIRT` 早退的情形。

**同时应当把 `declined` 从「静默」变成「有后果」**：目前它只 `kerr`，没有任何状态变化，
这正是缺陷得以长期潜伏的原因。

#### 仍未解释的一处（诚实记录，不掩盖）

本次运行里 `pte=0` 意味着 mprotect 当时**连叶子表都还没有**；但状态路径随后却读到了
`status_prot=1`。**即「mprotect 时无叶子表」与「稍后有 R-only 状态」这两件事如何同时成立，
本轮没有查清。** 可能的解释（均未验证）：叶子表是在同一 4K 对齐区域的**兄弟页**缺页时被顺带
创建的，其元数据里该页仍带着更早一次 `mprotect(PROT_READ)` 刷下的 R；
或 `mm_addrspace_lock()` 在创建叶子表时按某种方式继承了旧元数据。
**这不影响上面的修复方向**（无论叶子表何时出现，「刷新失败就丢弃陈旧状态」都成立），
但它是一个**独立于本崩溃的疑点**，值得单独查清。

**插桩现状**（故意保留，供修复验证复用）：`pt.c`（`[MM-INS]`）、`mprotect.c`（`[MM-DIV]`、
`[MM-RF]`）、`fault.c`（`[MM-ST]`、`[MM-FP]`）。**修复通过后必须全部移除。**

**riscv64 侧完全不受影响**：5 架构 + 2 NOMMU 变体构建通过、三个门全通过、关机审计全 0
（含 `safe=0`）、状态路径 2836 次缺页正常、预标记开/关性能中性（§10.34）。

### 10.55 元数据生命周期核对：新建叶子表**不可能**带出陈旧状态，疑点因此收得更紧

§10.54 留下的疑点是：`[MM-RF]` 显示 mprotect 时 `pte=0`（`pt_lookup_leaf()` 返回 NULL），
而稍后的 `[MM-ST]` 却读到了 `status_prot=1`（`ANON_VIRT` + **只读**）。
「当时无叶子表」与「稍后有 R-only 状态」如何同时成立？

核对元数据的生命周期（`kernel/mm/pt.c`）：

* `mm_pt_meta()`（`pt.c:180`）按**叶子表的 PFN** 查 `pfa.meta[pfn].pt`，
  并要求 `pfa.meta[pfn].flags == FRAME_F_PT`；
* `mm_pt_node_init()`（`pt.c:210`）分配元数据后 **`memset(m, 0, sizeof(*m))`**，
  再在 `pfa.lock` 下发布 `flags = FRAME_F_PT`；
* `mm_pt_node_fini()`（`pt.c:241`）把指针置 NULL、`flags = FRAME_F_ALLOC`，并回收元数据页。

**因此新建叶子表的元数据必然是全 0（即全 `MM_ST_INVALID`），
一个陈旧的 `ANON_VIRT|R` 状态绝不可能来自一张新建的叶子表。** 这条解释被排除。

于是剩下的可能只有两类，且**都还没验证**：

1. **`pt_lookup_leaf()` 返回 NULL 的原因不是「没有叶子表」。** 这是我此前的误读——
   `[MM-RF]` 的 `pte=0` 只说明**返回指针为空**，而 `pt_lookup_leaf()` 返回 NULL 也可能是因为
   该地址被一个**非 leaf 的上层条目**（中间节点或大页）覆盖，或其它查找失败情形。
   若如此，则当时叶子表**是存在的**，`mm_pt_refresh_absent_prot()` 本该被调用；
   而它被调用却返回 `declined=1`，只可能是走了 `cls != MM_ST_ANON_VIRT` 的早退
   （`pt.c:867`）——**但那样状态路径就不会用这份状态了**（它要求 `cls == ANON_VIRT`），
   于是又与 `[MM-ST]` 读出 `status_prot=1` 矛盾。**这条也需要查清。**
2. **同一 4K 对齐区域里兄弟页的缺页顺带创建了叶子表**，其元数据里该页带着更早一次
   `mprotect(PROT_READ)` 刷下的 R——但这与第 1 条的 `memset` 结论**并不冲突**，
   因为那张表**不是新建的**，而是被**复用**的（其元数据此前已存在且非零）。

**注意：这两条都不是「新建表带陈旧状态」，而是「表被复用时元数据未被重置」或
「`pt_lookup_leaf()` 的 NULL 被我误读」。** 二者都指向同一个需要读的地方：
**`pt_lookup_leaf()` 到底在什么情况下返回 NULL，以及叶子表被复用/释放时元数据是否随之释放。**

**这已是一个独立于本次崩溃的、更底层的不变量问题**（元数据是否与叶子表生命周期严格绑定），
值得单独查清；但它需要新的一轮完整上下文才能严谨推进。

**因此本轮到此为止，不实施修复。** 理由：§10.54 给出的修复（mprotect 刷新失败就丢弃陈旧状态）
在**机制层面已被运行时轨迹逐行证实**，但要写对必须先知道
**`pt_lookup_leaf()` 返回 NULL 的确切条件**——因为 `pte=0` 究竟是「无表」还是「被上层条目覆盖」，
决定了该走「新建表路径（自动安全）」还是「复用表路径（需显式重置）」，两种修法完全不同。
在这一点查清之前动手，就是又一次凭猜测下手——本轮已经证伪 9 次，不应再添第 10 次。

**交付状态（如实）**：
* **riscv64 侧完整、可用、可信**：5 架构 + 2 NOMMU 变体构建通过、`smoke-mm-stress` /
  `smoke-mm-fork-exec-race` / `check-mm-lock-model` 三个门全通过、关机审计全 0（含 `safe=0`）、
  状态路径 2836 次缺页正常、预标记开/关**性能中性**（§10.34）、所有旧性能结论已撤回。
* **x86_64 侧机制已闭环、修复待一个前置读码**：分叉点精确定位在
  `mprotect.c` 缺页分支的「`declined=1` 却照样 `v->pte_flags = ...`」这一行，
  运行时三行轨迹（`[MM-RF]`→`[MM-ST]`→`[MM-INS]`）逐行证实。
* **工作树含故意保留的诊断插桩**：`pt.c`（`[MM-INS]`）、`mprotect.c`（`[MM-DIV]`、`[MM-RF]`）、
  `fault.c`（`[MM-ST]`、`[MM-FP]`）。**这些是热路径 `kerr`，修复验证通过后必须全部移除。**

### 10.56 **根因确认**：`pt_lookup_leaf()` 返回 NULL 不等于「没有叶子表」

读了 `pt_lookup_leaf()`（`kernel/mm/mm.c`）之后，§10.55 的疑点解开，而且**根因就此确证**：

```c
pte_t *pt_lookup_leaf(pt_root_t *pgdir, vaddr_t va, ...) {
    pte_t *table = pgdir;
    for (int level = ARCH_PT_ROOT_LEVEL; level >= 0; level--) {
        pte_t *pte = &table[idx];
        if (!(*pte & PTE_V))
            return NULL;                 /* <-- 只是「该级条目未present」 */
        if (arch_pte_is_leaf(*pte)) { ...; return pte; }
        if (level == 0) return NULL;
        table = arch_pte_to_ptr(*pte);
    }
```

**关键：返回 NULL 的条件是「当前这一级的条目没有 `PTE_V`」，而不是「叶子表不存在」。**
走到 level 0 时，`table` **就是那张叶子表**，只是它的 `table[idx]` 尚未被映射。
所以完全可能出现：**叶子表存在、其元数据里该 idx 带着陈旧的 `ANON_VIRT|R`、
而 `pt_lookup_leaf()` 却返回 NULL。** §10.55 的第 1 条（非 leaf 上层条目导致 NULL）被排除
——被上层条目覆盖时它会正常返回那个 leaf 并给出 `level > 0`，不会返回 NULL。

#### 于是 `mprotect` 里的这句注释与代码都是错的

```c
if (pte) {
    int idx = arch_pt_vpn(va, 0);
    declined = mm_pt_refresh_absent_prot(pte - idx, idx, ptef) != 0;
}
/* 注释原文：「The per-PTE status lives in that table's metadata, so when
 *  there is no table there is no status to refresh.」 */
```

它把「`pt_lookup_leaf()` 返回 NULL」误读成「没有叶子表」，于是**整段跳过刷新**。
但真实情况是「**叶子表在、条目没映射**」——这恰恰是**最需要刷新状态的那种情形**：
条目没映射说明将来会走缺页路径，而缺页路径读的正是**这张表的元数据**里那份陈旧的
`ANON_VIRT|R`。跳过刷新 = 把陈旧只读权限留给未来的缺页去执行。

**这与 §10.54 的三行运行时轨迹完全吻合，且每一行都得到解释：**

```
[MM-RF]  va=8dc0f000 ptef=467 declined=1 pte=0
```

`pte=0` 并不是「没有叶子表」，而是「叶子表在、`table[idx]` 未映射」；
`declined=1` 来自 `if (pte)` 为假而**根本没进刷新**；VMA 却被 `ptef=0x467` 放宽成可写。
于是陈旧的 `ANON_VIRT|R` 留在元数据里 → 缺页时 `[MM-ST] status_prot=1` →
`[MM-INS] flags=0x425` 装出只读页 → 压在可写 VMA 下 → 首次写无人接管 → **致命**。

**结构性根因一句话**：
> **`mprotect` 用「能否取到 PTE 指针」来代替「能否取到叶子表」来判断要不要刷新
> per-PTE 状态；对「表在、条目未映射」这一最常见的情形，它错误地跳过了刷新，
> 于是陈旧状态被后来的状态缺页照单执行。**

§10.38 当初把 `if (pte)` 加上，是为了消除 `pte - idx` 对 NULL 做指针运算的 UBSAN；
**方向对，但结论错**——真正该做的是「用 `mm_addrspace_lock()` 走到叶子表」，
而不是「PTE 取不到就跳过」。这也是为什么 §10.38 修完 UBSAN 从 35 降到 2、崩溃却丝毫未变。

#### 修复（方向唯一，且已完全确定）

在 `mprotect` 的缺页分支里，**不要用 `pt_lookup_leaf()` 的返回值判断**，改用能走到叶子表的
路径（例如 `mm_addrspace_lock()` + `cursor_leaf_table()`，或新增一个只下探到叶子表的
helper），然后**无条件**对 `table[idx]` 调用 `mm_pt_refresh_absent_prot()`。
这样：
* 表在、条目未映射 → 刷新成功，陈旧权限被纠正（**修掉本崩溃**）；
* 表不在 → 拿不到元数据，此时**新建**叶子表的元数据必为全 0（§10.55 的 `memset` 结论），
  状态路径不会命中 `ANON_VIRT`，天然安全；
* `mm_pt_refresh_absent_prot()` 内部那条 `cls != MM_ST_ANON_VIRT` 早退可以保留——
  非 `ANON_VIRT` 的状态本来就不会被状态路径使用，**不必**在这里强行改写别人的 class。

**本轮不做实施**：改动需要新增一个「下探到叶子表」的 helper 并处理与 `mm_addrspace_lock()`
的锁序关系（`mm->lock` → 页表锁），属于需要完整上下文才能保证不引入死锁的改动。
但根因、修复方向、以及为什么前九次全错，此刻都已确定到可以直接实施的程度。

**前九次为何全错（因为都在查「状态从哪来」，没人查「状态为什么没被更新」）**：
状态路径忠实执行状态字节，从不查 VMA——这是论文的设计。真正的契约是
**「mprotect 必须在同一次调用里让状态与 VMA 保持一致」**，而这条契约被一个
「用 PTE 指针的有无代替叶子表的有无」的近似判断破坏了。

**插桩现状**（故意保留，供修复验证复用）：`pt.c`（`[MM-INS]`）、`mprotect.c`（`[MM-DIV]`、
`[MM-RF]`）、`fault.c`（`[MM-ST]`、`[MM-FP]`）。**修复通过后必须全部移除。**

### 10.57 **修复完成并验证**：改用 `mm_pt_leaf_table()` 刷新状态

**改动**（`kernel/mm/mprotect.c`，缺页分支）：把「取到 PTE 指针」换成「取到叶子表」。

```c
-  if (pte) {
-      int idx = arch_pt_vpn(va, 0);
-      declined = mm_pt_refresh_absent_prot(pte - idx, idx, ptef) != 0;
-  }
+  pte_t *ltab = mm_pt_leaf_table(mm->pgdir, va);
+  if (ltab)
+      (void)mm_pt_refresh_absent_prot(ltab, arch_pt_vpn(va, 0), ptef);
```

**关键点：仓库里早就有 `mm_pt_leaf_table(pgdir, addr)`**（`kernel/include/mm/pt.h:208`，
注释写着「The table that owns the leaf slot for addr … NOT the table `pt_walk()` returns」），
它正是**不分配任何东西**的「下探到叶子表」——`mm_pt_refresh_absent_prot()` 需要的
`table` 参数可以直接由它给出。**所以真正的缺陷不是「缺一个 helper」，而是
「helper 早就存在，mprotect 没用它」。** 修复因此只有几行，且不引入新锁序、不新增分配：
`mm_pt_leaf_table()` 不进 `mm_pt_read_enter()`，与既有 `pt_lookup_leaf()` 同量级。

顺带清掉了 §10.38 引入的 `if (pte)` 守卫——它当初为消除 `pte - idx` 的 UBSAN 而加，
**方向对但结论错**，正是它把刷新挡在了门外。

**验证（全部在无插桩的干净树上完成）**：

| 项 | 结果 |
|---|---|
| x86_64 `CONFIG_ANON_PROV_DEFAULT=4096` | `mm_stress` **PASS**，0 FATAL，`mm_anon_provisioned=9280`，`mm_fault_from_status=2842` |
| x86_64 `CONFIG_ANON_PROV_DEFAULT=0`（回归） | `mm_stress` **PASS**，0 FATAL |
| MM-ASM 关机审计（两臂） | 全 0，含 `prot=0 safe=0 anon_virt=0` |
| 5 架构（riscv64/x86_64/aarch64/loongarch64/ppc64le） | 0 errors |
| riscv64 + aarch64 NOMMU | 0 errors |
| `smoke-mm-stress` / `smoke-mm-fork-exec-race` / `check-mm-lock-model` | 全 PASS |
| 残留插桩 | 0（`pt.c` / `mprotect.c` / `fault.c` 已清空） |

带插桩那轮还额外确认了修复的**判据**：`MM-DIV=0`、`declined-refresh=0`、
`read-only installs=0`——三类分叉迹象全部归零。

**两处诚实记录**：
1. 第一次「干净树」验证其实**跑的是过期二进制**——移除插桩后 `declined` 变成未使用变量，
   `-Werror` 让构建失败，而 `dev-build` 静默保留了旧的 `kernel.elf`，于是日志里仍有
   `[MM-INS]`。是靠检查日志里残留插桩 + 比对 `.elf` 与源文件 mtime 才发现的。
   **教训：`dev-build` 失败时不会自动删除旧产物，必须核对时间戳，不能只看运行结果。**
2. 最后那次 riscv64 复跑用的是 `make ARCH=riscv64 run`（**默认构建**），
   因此 `mm_anon_provisioned=0 / mm_fault_from_status=0`——它验证的是**关闭**臂，
   **没有覆盖 riscv64 的状态缺页路径**。riscv64 开启臂需带 `a20.anonprov=<n>` 重跑，
   **此项尚未复核**（修复只动 mprotect 的状态刷新，理论上不影响，但不应凭「理论上」记账）。

**十次证伪的最终教训**：前九次都在问「这份只读权限是**谁写进去的**」，
第十次才问对——「**它为什么没被更新**」。状态路径按设计**从不查 VMA**（论文卖点），
因此真正的契约是「**mprotect 必须在同一次调用里让状态与 VMA 一致**」。
这条契约被一句「取不到 PTE 就没有状态可刷」的近似判断破坏了，而**那句注释写得如此笃定，
以至于十次排查里有九次都被它引到了错误的分支上**。

### 10.58 补齐 riscv64 开启臂复核：状态缺页路径修复后完好（§10.57 的遗留项已关闭）

§10.57 诚实记录的唯一遗留项是「riscv64 开启臂尚未复核」。现已补齐。

`qemu-system-riscv64 -machine virt -append "a20.anonprov=4096"`（走 DTB `/chosen/bootargs`，
`CONFIG_ANON_PROV_DEFAULT` 保持默认不变）：

| 项 | 修复后结果 |
|---|---|
| `mm_stress` | **PASS**，0 FATAL |
| `mm_anon_provisioned` | 9280 |
| `mm_fault_from_status` | **2844**（修复前为 2836，同量级） |
| MM-ASM 关机审计 | 全 0 |
| 关机 | `System is going down for power-off NOW.`，`audit errors=0` |

#### 两平台最终对照（均为修复后、无插桩）

| 平台 | 开启臂 | 状态缺页 | 审计 | 关闭臂 |
|---|---|---|---|---|
| riscv64 (TCG) | `mm_stress` **PASS** | 2844 | 全 0 | PASS（默认构建） |
| x86_64 (KVM/q35) | `mm_stress` **PASS** | 2842 | 全 0 | **PASS** |

两平台的「开启」臂数字高度一致（9280 预标记 / ~2840 状态缺页），且**修复前的崩溃平台
x86_64 现在与 riscv64 表现一致**——这正是本次修复的目标：消除平台间的行为分歧。
riscv64 开启臂的 2844 与修复前的 2836 几乎相同，说明**修复没有扰动正常的匿名预标记路径**，
只是补上了「状态与 VMA 不一致时的那一次刷新」。

#### 关于 TCG 时长的一点记录

第一次 riscv64 复核（`-m 2G -smp 1`，600s 上限）只跑到 `mm_fault_from_status=797`、
`mm_stress` 未完成；给到 `-m 2G -smp 2`、1500s 后跑完并 PASS。**这是 TCG 慢，不是失败**——
`mm_stress` 本身在 riscv64 上就是最慢的一档。**教训：TCG 上的「没跑完」不能记成「失败」，
也不能记成「通过」；必须区分超时与失败。** 之前 x86_64 那次「既非 PASS 也非 FATAL」则是另一回事
——那是过期二进制，判据完全不同（见 §10.57 第 1 条）。

**至此本次工作收束**：x86_64 崩溃已修复并双平台验证，§10.39–§10.58 共十次归因尝试的完整
证据链保留在文档中，包含七次被自身诊断证伪的假设、以及最终靠**运行时安装轨迹**
（而非读码推理）才定位到真因这一方法论教训。

### 10.59 UFFD「过度清除」的前置条件**已满足**——上锁前必须先修

§10.19 当初把 UFFD 过度清除标为「状态缺页路径启用**之前**必须先解决」。现在状态缺页路径已经
修好并双平台验证（§10.57/§10.58），**那个前置条件已经到达**。

#### 缺陷的确切形状

`kernel/ipc/userfaultfd.c:517-532`（unregister）在合并并 unlink 了本次要注销的所有 range 之后，
对**合并后的整段** `[rlo, rhi)` 无条件清标记：

```c
/* A page can in principle still be covered by a *different* uffd
 * registration, so this clears more than strictly necessary.
 * ... Before the status fault path is enabled (docs 10.7/10.19) this
 * has to be refined to clear per page while re-testing presence,
 * otherwise an over-clear would let a still-registered page be faulted
 * without parking. */
spin_lock(&t->mm->lock);
mm_pt_set_safe_range(t->mm, rlo, rhi, MM_SAFE_UFFD, 0);
spin_unlock(&t->mm->lock);
```

**注释里那句「`userfaultfd_range_present()` 是权威判定，标记陈旧不会让缺页跳过 handler」，
现在对 `ANON_VIRT` 页已经不成立了**：

* 状态缺页路径 `kernel/mm/fault.c:970` 用的是**位**：
  `!mm_cursor_safe_test(&qcur, page_va, MM_SAFE_UFFD)`；
* 权威的 `userfaultfd_range_present()` 在 **`fault.c:1036`**，即 **VMA 路径**上；
* 而 `ANON_VIRT` 的页在 `fault.c:970` 就被**就地满足并返回**，**根本走不到 1036**。

`fault.c:967` 那句注释（「the authoritative userfaultfd_range_present() check still runs on
the VMA path below for **every other case**」）对 `ANON_VIRT` 这一类**恰恰是假的**——
注释是随状态路径一起写的，写的时候没有意识到这条路径会**短路**掉权威检查。

**后果**：若同一页同时被两次注册覆盖，注销其中一次会把 `MM_SAFE_UFFD` 清掉，
此后该页的缺页**不会** parked 给仍存活的 handler，而会被状态路径直接满足——
即 §10.19 预言的「仍被注册的页被缺页而不 park」。

#### 缓解（必须说清楚，避免夸大严重性）

**默认构建不受影响**：实测默认 riscv64 构建 `mm_anon_provisioned: 0`（§10.58），
即预标记默认关闭，状态缺页路径**不启用**，UFFD 仍由 `fault.c:1036` 的权威检查把关。
该缺口目前**仅存在于** `a20.anonprov=<n>` / `CONFIG_ANON_PROV_DEFAULT=<n>` 的实验构建里。
（`MM_ANON_PROVISION_MAX_PAGES = 4096` 与 `CONFIG_ANON_PROV_DEFAULT` 是两个东西：
前者是上限常量，后者才是默认开关，实测默认为 0。）

#### 因此「把预标记默认打开」的前置条件清单

1. **修 UFFD 过度清除**：逐页清除并在清之前重新判定 presence。难点是锁序——
   unregister 现在是「放掉 `g_uffd_lock` → 取 `mm->lock`」，若在 `mm->lock` 内再取
   `g_uffd_lock` 做 presence 复查，就形成 `mm->lock → g_uffd_lock` 的新嵌套；
   需先确认全仓库没有「持 `g_uffd_lock` 再取 `mm->lock`」的路径，否则会造出环路。
   **这一步本轮未做，锁序未验证，不应凭猜测下手。**
2. 或者：让状态路径在 `ANON_VIRT` 命中时**也**调用 `userfaultfd_range_present()`
   （它查的是 range 链表，不是 VMA，比 VMA 遍历便宜得多），代价是每次状态缺页多一次
   加锁查询——比方案 1 简单，但确实侵蚀论文「缺页不查任何表」的主张。
3. 顺手修正 `fault.c:967` 那句**已经不成立**的注释。

**当前状态**：默认构建安全（预标记关闭），UFFD 语义正确。**在 1 或 2 完成之前，
不要把预标记默认打开。**

### 10.60 UFFD 修复的锁序障碍**已排除**（§10.59 的方案 1 现可实施）

§10.59 留下的唯一疑问是锁序：在 `mm->lock` 内再取 `g_uffd_lock` 做逐页 presence 复查，
会不会与某处「持 `g_uffd_lock` 再取 `mm->lock」形成环路。**已查证：不会。**

**证据一：`userfaultfd.c` 里没有任何路径在持有 `g_uffd_lock` 时去取 `mm->lock`。**
逐个核对 `g_uffd_lock` 的临界区（128-136、146-155、163-171、439-463、496-516、574-587、
681-690），其中**没有一处**包含 `spin_lock(&mm->lock)`；`mm->lock` 的取用点（209、212、
241、244、421、428、435）全部位于 `g_uffd_lock` 临界区**之外**。
unregister 更是明确地**先放掉** `g_uffd_lock`（第 516 行）**再取** `mm->lock`（约 530 行），
两者从不嵌套。

**证据二：`mm->lock → g_uffd_lock` 这个顺序**早已在本代码库里实际使用**——
`kernel/mm/fault.c:1036` 在**仍然持有 `mm->lock`** 的情况下调用
`userfaultfd_range_present()`，紧接着的下一行才是 `spin_unlock(&mm->lock);`：

```c
if (vma &&
    (vma->vm_flags & (VM_ANON | VM_FILE | VM_VMO | VM_SHARED)) == VM_ANON &&
    userfaultfd_range_present(mm, page_va)) {
    spin_unlock(&mm->lock);
    ...
```

**结论**：`mm->lock → g_uffd_lock` 是**既有且在用**的顺序，而反向嵌套全仓库不存在。
因此 §10.59 的**方案 1**（在 `mm->lock` 内逐页清除 `MM_SAFE_UFFD`、每页清除前重新判定
presence）**不引入任何新的锁嵌套，不构成死锁**，可以安全实施。

**实施要点（供下一轮直接落地）**：
1. `mm_pt_set_safe_range()` 增加一个「清 UFFD 标记前先复查 presence」的钩子，或在
   `userfaultfd.c` 里改成**逐页**处理而非整段一次性清除；
2. presence 复查复用既有的 `userfaultfd_range_present(mm, page_va)`（它查 range 链表，
   已在 `fault.c:1036` 于 `mm->lock` 内被调用，安全）；
3. 顺带修正 `fault.c:967` 那句**对 `ANON_VIRT` 已不成立**的注释（§10.59 第 3 条）；
4. 验证：需覆盖「**同一页被两次注册**」的场景——现有 `mm_stress` 未必包含，
   应补一个针对性用例，否则改完也无法证明过度清除已消除。

**本轮到此为止**：锁序已查清、方案已确定、但**未实施**——实施需要新增逐页复查逻辑并补
一个「双重注册」测试用例，属于需要完整上下文才能一次做对并验证的改动，不宜在收尾阶段动手。

### 10.61 UFFD 过度清除已修复：逐页清除 + 每页复查 presence

按 §10.60 确认的方案 1 实施（锁序已查证为**既有**的 `mm->lock → g_uffd_lock`，不新增嵌套）。

**改动一：新增 `mm_pt_safe_clear_page()`**（`kernel/mm/pt.c`，紧邻 `mm_pt_set_safe_range()`）。
整段清位的 `mm_pt_set_safe_range()` 对 `MM_SAFE_UFFD` 不适用——一个页可能同时被**另一个**
uffd 注册覆盖，整段清会把别人的标记一起清掉。新函数只清**一个**页：

```c
int mm_pt_safe_clear_page(struct mm_struct *mm, vaddr_t va, unsigned flags)
{
    ...
    pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
    if (!pte || !size)
        return 0;               /* 没有叶子就没有标记 */
    pte_t *table = pte - arch_pt_vpn(va, 0);
    mm_pt_safe_clear(table, 0, arch_pt_vpn(va, 0), flags);
    return 0;
}
```

**改动二：unregister 改为逐页 + 逐页复查**（`kernel/ipc/userfaultfd.c`）：

```c
+#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
 spin_lock(&t->mm->lock);
-mm_pt_set_safe_range(t->mm, rlo, rhi, MM_SAFE_UFFD, 0);
+for (vaddr_t p = rlo; p < rhi; p += PAGE_SIZE) {
+    if (!userfaultfd_range_present(t->mm, p))
+        (void)mm_pt_safe_clear_page(t->mm, p, MM_SAFE_UFFD);
+}
 spin_unlock(&t->mm->lock);
+#endif
```

**改动三：NOMMU 守卫**。`pt.c` 的全部安全位函数位于
`#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)`（`pt.c:33`）之内，
NOMMU 构建下不参与编译，故调用点必须同样加守卫。NOMMU 无页表、也就没有安全位，
不加守卫会得到 `undefined reference to mm_pt_safe_clear_page`——**这是本次改动
唯一一次编译失败，且是靠链接错误而非源码错误暴露的，值得记住。**

**改动四：修正 `fault.c` 那句已经不成立的注释**（§10.59 第 3 条）。原文声称
「`userfaultfd_range_present()` 是权威判定……对其它所有情况都仍会在下面的 VMA 路径上运行」——
而 `ANON_VIRT` 的页在状态路径里**就地满足并返回**，根本走不到 `fault.c:1036`。
现已改成如实描述：标记在此处**具有权威性**、不被重新推导，因此 unregister 有义务
只为「无任何注册仍覆盖」的页清标记。

**验证**：

| 项 | 结果 |
|---|---|
| 5 架构（riscv64/x86_64/aarch64/loongarch64/ppc64le） | 0 errors |
| riscv64 + aarch64 NOMMU | 0 errors |
| x86_64 开启臂（`CONFIG_ANON_PROV_DEFAULT=4096`） | `mm_stress` **PASS**，0 FATAL，审计全 0 |
| riscv64 开启臂（`a20.anonprov=4096`） | `mm_stress` **PASS**，0 FATAL，审计全 0 |
| `smoke-mm-stress` / `smoke-mm-fork-exec-race` / `check-mm-lock-model` | 全 PASS |

**尚未证明的一点（如实记录）**：本次修复**没有针对性的回归测试**。要证明「过度清除已消除」，
需要一个「同一页被两次注册、注销其中一次、确认另一注册仍能收到 fault」的用例；
现有 `mm_stress` 是否覆盖这一场景**未确认**。因此本次验证证明的是
**「改动没有破坏既有行为」**（各门与两个开启臂仍全绿），**不是**「过度清除已被测试证明消除」。
补这个用例是下一轮的明确任务。

### 10.62 UFFD 回归测试：写出来了、能通过，但**查出另一处既有脆弱性**，故暂缓落地

§10.61 明确记下「没有针对性回归测试」是本次修复的短板。测试已写出（`syscall_ext.c` 的
`test_uffd_double_registration()`）：同一页在两个 uffd 上各注册一次 → 注销其中一个 →
另一线程触发读缺页 → **必须仍然被 park**（`read()` 返回 `UFFD_EVENT_PAGEFAULT`）
→ 用存活的 uffd `UFFDIO_COPY` 解决 → 校验内容。若 unregister 仍整段清位，
该页会被状态缺页路径就地满足，`read()` **永远不返回**（挂死），测试即失败。

**测试本身通过**：两次运行都**没有**打印任何 `uffd2 ...` 失败信息，
`pthread_join` 也正常返回，说明 park → 解决 → 内容校验这条链是通的。

**但它暴露了另一处既有问题**：启用该测试后，**后续一个与 uffd 无关的测试**失败：

```
MM_STRESS: evict start
[BCACHE] no evictable page page=5772 valid=2000 dirty=1879 referenced=0 total_refs=0 max_refs=0
MM_STRESS: evict-mmap start
MM_STRESS: FAIL evict-mmap-verify-mapped errno=17      <-- 17 = EEXIST
```

`evict-mmap-verify-mapped` 是对**带提示地址的 `mmap`** 断言映射成功，拿到 `EEXIST`
即提示地址已被占用。隔离实验确认因果：

| 条件 | 结果 |
|---|---|
| 启用新测试 | `MM_STRESS: FAIL evict-mmap-verify-mapped errno=17` |
| `MM_SKIP_UFFD2=1`（跳过新测试） | `MM_STRESS: PASS` |

**因此这不是新测试的缺陷，而是 `evict-mmap` 对地址空间布局/页缓存状态敏感**：
新测试多创建一个线程、多做几次 `mmap`/`munmap`，就足以让 `evict-mmap` 的提示地址撞车。
前面那句 `[BCACHE] no evictable page` 也提示回收路径在那一刻本就处于吃紧状态。

**处置：不在本轮落地这个测试。** 理由是**不能让默认的 `mm_stress` 变红**——
提交一个让既有测试失败的测试，比暂时没有这个测试更糟。`syscall_ext.c` 已回退到
`72117795` 的状态（`git checkout`），工作树干净、构建 0 error。
**UFFD 逐页清位这一修复本身已合入并验证**（§10.61），缺的只是这个针对性用例。

**下一轮的明确任务（有先后顺序）**：
1. 先查 `evict-mmap-verify-mapped` 为何用提示地址、以及为何会 `EEXIST`——
   这本身就是一个**既有缺陷**（与 UFFD 无关），应当独立修掉，而不是靠新测试绕开；
2. 再把 `test_uffd_double_registration()` 落地（可直接复用本次实现：双注册 → 注销其一 →
   断言另一方仍收到 fault → COPY 解决 → 校验内容），并在落地时确认它与 `evict-mmap`
   不再有布局耦合。

**方法论记录**：这次「写了测试 → 测试通过 → 却发现别处坏了」的循环，
与 §10.39–§10.56 那十次排查是同一个教训的另一个侧面：
**一个新增的测试改变地址空间状态，就足以唤醒此前被掩盖的既有缺陷。**
测试通过 ≠ 周边无问题；测试失败也不必然是测试自己的错。

### 10.63 **更正 §10.62 的误判**：`evict-mmap-verify-mapped` 不是地址冲突，是**内容丢失**

§10.62 把 `MM_STRESS: FAIL evict-mmap-verify-mapped errno=17` 读成「带提示地址的 `mmap`
返回 `EEXIST`」，据此把下一轮任务定为「查提示地址撞车」。**这个读法是错的。**

`user/cmds/stress/mm_stress.c:15` 的 `fail()` 只接收一个字符串，并**打印环境里遗留的 `errno`**：

```c
static int fail(const char *what)
{
    printf("MM_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}
```

而这一处调用是 `return fail("evict-mmap-verify-mapped");`（`mm_stress.c:1416`），
**没有传 errno**——所以那个 `errno=17 (EEXIST)` 是**早前某次无关系统调用留下的残留值**，
与失败原因**毫无关系**。§10.62 建立在它之上的因果链（「提示地址被占用」）**作废**。

#### 真正的失败性质

`mm_stress.c:1404-1416` 那段循环是**逐页内容校验**：

```c
for (size_t p = 0; p < mmap_pages; p++) {
    const char *page = mem + p * 4096;
    ...
    for (size_t i = 0; i < 4096; i++) {
        if (page[i] != (char)((page_idx * 7 + i) % 251)) {
            ...
            return fail("evict-mmap-verify-mapped");
        }
    }
}
```

即：往文件映射里逐页写入一个可预测图案 → `fsync(fd_b)` → 再**通过映射读回**逐字节比对。
**比对不符 = 映射里的内容与刚写进去的不一致，也就是内容丢失/被写坏。**

这比 §10.62 说的「地址撞车」**严重得多**：它不是「映射建不起来」，
而是「**已经写好的数据读回来变了**」——在有内存压力、有回收参与的情况下发生。
而且紧邻其前的那条消息正指向回收侧：

```
[BCACHE] no evictable page page=5772 valid=2000 dirty=1879 referenced=0 total_refs=0 max_refs=0
```

**`valid=2000`、`dirty=1879`，却「无页可回收」**——即在明显应当有可回收页的时刻，
回收路径报出找不到候选。随后映射内容就出现不一致。

#### 目前确定与未确定的

**确定**：
* 失败是**数据不一致**，不是系统调用失败；`errno` 字段在此**无诊断价值**；
* 失败发生在「文件映射 + 内存压力 + 回收」这一组合下；
* 回收侧此前刚报出「无页可回收」。

**未确定（本轮未查）**：
* 该映射的确切 `mmap` 标志（`MAP_SHARED` 还是 `MAP_PRIVATE`、有无 `MAP_NORESERVE`）——
  §10.62 的 grep 没抓到 `mmap` 调用本身；
* 内容究竟丢在**脏页回写**、**回收/回写（reclaim）**、还是**重新映射（remap）**哪一环；
* 那个 `no evictable page` 判定本身是否就是错的（例如 `referenced=0 total_refs=0`
  是否意味着引用计数统计在丢页路径上没被维护）。

**下一轮任务据此更正为**（**取代** §10.62 的第 1 条）：
1. 读 `evict-mmap` 测试的 `mmap` 调用，确认映射类型；
2. 顺着 `[BCACHE] no evictable page` 的判定条件往回查：为什么 `valid=2000` 却判定无可回收；
3. 再把 §10.62 第 2 条（落地 UFFD 双重注册测试）接在其后。

**这一条也说明 `fail()` 的设计有问题**：它无条件打印 `errno`，于是**大量与 errno 无关的
断言失败都会带上一段误导性的 errno 文本**——本次就差点因此把排查方向带偏（先信了 `EEXIST`
整整一节）。要么让它区分「系统调用失败」与「断言失败」，要么在断言类失败时不打印 errno。
这本身是个应当单独修的小缺陷。

### 10.64 `no evictable page` 的诊断数据自相矛盾：2000 个 valid 页，引用计数**全是 0**

§10.63 定下的第 2 步（查 `valid=2000` 却判定无可回收）**已经有了决定性的数据**。
`kernel/fs/block_cache.c:907-931` 那段诊断在 `pcache_evict_locked()` 返回 NULL 之后，
遍历**整个** page pool 统计：

```
[BCACHE] no evictable page page=5772 valid=2000 dirty=1879
                     referenced=0 total_refs=0 max_refs=0
```

* `valid=2000`——池里有 **2000 个有效页**；
* `dirty=1879`——其中 **1879 个是脏的**；
* **`referenced=0 total_refs=0 max_refs=0`**——按 `cache_ref_read(&page->ref)`
  逐页统计，**2000 个页里没有任何一个的引用计数大于 0**。

**矛盾就在这里**：若 `pcache_evict_locked()` 的候选判据与这段诊断用的是同一个引用计数，
那么「全部 2000 页都未被引用」应当让它**轻松选出 2000 个候选**，而不是返回 NULL。
它返回了 NULL，说明二者的判据**不一致**——即：

> **`pcache_evict_locked()` 认定「不可回收」的理由，并不是这段诊断所统计的那个 `ref` 计数。**

诊断数据本身是可信的（它直接读 `page->ref`，且是在 `bc->lock` 保护下遍历整个池），
所以问题**不在引用计数被清零**（若真是被清零，反而更好回收）。问题在于**淘汰路径认定
这些页不可回收，用的是另一个条件**。

**最有嫌疑的方向（按可能性排序，均未验证）**：

1. **脏页为主 + 回写失败/被跳过**。`dirty=1879 / valid=2000`——**94% 是脏页**。
   若 `pcache_evict_locked()` 对脏页要求先成功回写，而此路径下回写失败或被跳过
   （注意调用链上游刚拿过 `bc->writeback_lock` 并做过一次 fill 尝试，
   `block_cache.c:900` 处 `return e`），则**所有脏页都会被判为不可回收**，
   剩下 121 个干净页又因别的原因落选，最终返回 NULL。
2. **存在 `valid` 但被 pin / 正在被填充（inflight）的页**，淘汰扫描显式跳过它们。
3. **扫描范围与诊断范围不同**：淘汰可能只扫 `page_no` 附近的一个窗口/时钟环
   （注意形参 `page_no=5772`），而诊断扫的是**全池**。若淘汰只看某个环/窗口，
   而该窗口恰好全被 pin 住，就会「窗口内无候选」而「全池有 2000 个」。

**第 3 条尤其值得先查**：形参 `page_no=5772` 被打印出来，说明**淘汰是围绕某个页号进行的**，
这与「时钟/LRU 环」或「按页号窗口」的实现相符；而诊断统计的是**全池**。
**若两者范围不同，那么这条诊断信息会系统性地夸大「无可回收」的程度**，
`valid=2000` 这个数字就不足以说明问题严重性——真正该看的是**淘汰实际扫描的那个范围**里有什么。

#### 这与 §10.63 的内容丢失如何连起来

`evict-mmap` 是 `MAP_SHARED` 文件映射（`mm_stress.c:1320`，`hint=NULL`——顺带再次确认
§10.62「提示地址撞车」的读法是错的）。它在内存压力下逐页写入图案并 `fsync`，
然后**通过同一映射读回逐字节比对**，结果不一致。

**尚未确定**内容究竟丢在**回写**、**reclaim**、还是 **remap** 哪一环——
但 §10.64 的数据把嫌疑显著收窄到**回写/淘汰这一侧**：在 94% 的页都是脏页、
且淘汰路径同时报出「无可回收」的情况下，脏数据**留在内存里没能落盘**，
下一次通过映射读取时读到的就不是刚写进去的内容。

**下一轮的具体第一步（明确、可执行）**：读 `pcache_evict_locked()`，确认
(a) 它扫描的范围是全池还是某个页号窗口；(b) 对 `dirty` 页是否要求回写成功才可淘汰；
(c) 是否有 pin/inflight 跳过条件。三者任一即可解释「2000 个未引用页却零候选」。

**同时值得单独确认**：`block_cache.c:900` 那个 `return e` 分支——上游 fill 失败后是否
也直接返回 NULL 而**根本没有尝试淘汰**。若是，则「无可回收」的诊断信息会误导：
真正的失败原因是**回写/填充出错**，而不是「找不到可淘汰的页」。
