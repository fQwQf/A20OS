# 单级内存模型迁移（MM_AS_MODEL）

A20OS 的内存管理从两级模型（软件级区间抽象 VMA + 硬件页表，两者必须保持一致）
迁到 CortenMM 式单级模型。记录的是目标设计、逐特性结论、分阶段计划，以及
明确不做的部分。

> **当前状态（2026-10-04）：迁移已完成，旧模型代码已删除。**
>
> 本文前面各章（§1–§7）是**当初的计划**，§8 起是按日期追加的实施记录，**读到
> §13.18 才是现状**。计划里的多处结论后来被实测推翻，文中就地标注了更正。
>
> 现状一句话：**地址空间里一个映射只有一条记录** `struct mm_seg`
> （`kernel/include/mm/vm.h`）——它既在 `mm->mmap` 链表中，又被页表节点条目按名字
> 引用；`vm_area_t`、`vma_index[]`、`mm_find_vma()` 与第二个映射表示**均已删除**。
> 每页状态由挂在覆盖该页的页表页上的元数据承载，是页状态的唯一权威。
> 分派可以安全地在"页表名字"与"链表查询"之间回退，是因为两条路解析出的是**同一条
> 记录**——残余约 2.5% 的覆盖缺口因此是性能特征而不是正确性问题。
>
> 门禁证据见 `docs/testing-gates.md`；合并过程见 §13.16–§13.18。

参考：J. Zhang 等，*CortenMM: Efficient Memory Management with Strong
Correctness Guarantees*，SOSP '25（DOI 10.1145/3731569.3764836）。
参考实现：<https://github.com/TELOS-syslab/CortenMM-Artifact>（Rust / Asterinas）。

代码位置：`kernel/include/mm/pt.h`、`kernel/mm/pt.c`。
契约：`MM_AS_MODEL`（`kernel/include/mm/vm.h`）、`MM_AS_CURSOR_ONLY_ENTRY`
（`kernel/include/mm/pt.h`）。

---

## 1. 为什么要迁移

两级模型要求每个操作同时调和两个差异极大的数据结构。区间树适合表达
on-demand paging 这类高级语义，且与具体 MMU 无关；代价是必须正确且高效地把它
与页表同步起来。论文的观察是这两个目标已经基本消失：

* x86 / ARM / RISC-V 都使用基于多级 radix 树的页表，架构差异本来就用
  C 宏（我们是 `arch_pt_*` 系列 inline）掩盖，不需要另一个抽象层。
* 高级语义确实需要 MMU 之外的状态，但那不构成另起一层的理由。

对 A20OS 而言，动机**不是论文的可伸缩性收益**。当前 `mm->lock` 已经把地址空间的
全部变更串行化，可伸缩性尚未到手。真实的动机是正确性与结构性债务：
`mm_vma_defer()`（`kernel/mm/vma.c`）存在的原因仅仅是区间世界需要在页表世界里
睡眠（`vma_release()` 可能触发 VFS 回写），两种粒度的锁需求打架。可伸缩性是
这套改造的附带收益，必须实测才算数。

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

已映射页的物理帧不在元数据里重复存放：它在 PTE 中，引用计数在
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

参考实现的 `Status::MASK = ((1 << 39) - 1) / PAGE_SIZE` 隐含假设**整棵页表树每一层
都有统一的约 27 位 payload 预算**，A20OS 没有这个前提。此外现有的 `PTE_SWAP`
已经在逐架构窃取硬件有语义的位（riscv64 bit9、aarch64 = `PTE_LEAF` bit55、
x86_64 = `PTE_LEAF` bit10、ppc64le bit1、arm32 bit7、la64 = `PTE_LEAF` bit11），
通用状态编码会把这一类损坏风险成倍放大。

### 2.2 描述符可达性：内联进 `frame_meta_t`

`pfa.meta` 已经是启动时分配、常驻直映、按 PPN 索引的连续数组，这正是论文的
描述符区域。描述符指针放进 `frame_meta_t` 中**仅在空闲链表上有效**的 `prev/next`
联合体，因此 `sizeof(frame_meta_t)` 仍是 16 字节，单级模型对帧表本身零额外开销
（已用 `_Static_assert` 钉住）。`pt_meta_t` 块按页表页按需分配，随该页表页释放。

拒绝 slab 侧表：ADV 协议的无锁遍历需要在最热路径上取描述符（读 `stale`、
取 covering node 锁），一次索引加载胜过哈希探测。

### 2.3 协议选择：ADV，不做 RW

论文的 `CortenMMrw` 在遇到任何非 `PageTable` 子节点时 `break`，**要求页表完全
填充**。A20OS 的 `pt_walk()` 是惰性分配中间节点的，而完全填充的代价不可接受：
4 级 / 512 项架构上 1 GB 用户虚拟地址空间需要 512 个 L1 页 = 每个地址空间 2 MB
纯脚手架，再乘以数百个进程；riscv32 / la32r 只有 2 级。`CortenMMadv` 的
`try_traverse_and_lock_subtree_root` 处理的正是惰性情形（锁住父节点、分配缺失的
子节点、下降）。

MCS 锁按 `(cpu, depth)` 从静态池取 node，不在锁路径上分配。

### 2.4 一个必须现在设计进去的约束：共享内核半

`pt_map_kernel()` 把 `boot_pgdir[ARCH_PT_USER_END..]` 的 PTE 字复制进每个进程
根页表，所以**内核半的整棵子树在所有地址空间之间是同一批物理页**。写锁它会让
全系统串行；释放它是系统级 use-after-free。因此 `mm_addrspace_lock()` 在
**代码层面**拒绝任何触及该半区的区间，而不是靠注释。内核映射永远不经由 cursor
建立。

## 3. 论文未覆盖的特性的逐项结论

| 特性 | 结论 | 成本 / 风险 |
|---|---|---|
| brk | 残留区间结构。`mm->brk` / `start_brk` 本来就是 `mm_struct_t` 标量而非 VMA。把 `fault.c` 里第三个子句 `!mm_find_vma(...)` 换成"该区间内没有已映射状态"，比现在更精确。 | 低。真实风险：brk 收缩必须同时清元数据与 PTE；brk 增长不得覆盖 mmap。 |
| mseal | 残留区间结构，非权威。`mm_struct_t` 里的 `seal_range_t` 列表，在开启 cursor 之前被查询。区间性就是它的语义。不要假装它变成了逐页属性。 | 低成本，但你确实保留了一个区间结构。 |
| madvise DONTNEED/FREE | 完全逐页。开 cursor 遍历区间，`unmap()` 掉每个 class 为 Private/Shared/VMO 的页，置 `INVALID`。seal 检查作为前置门。 | 低。 |
| mlock 记账 | 残留区间（今天就只是 `VM_LOCKED` / `locked_vm` 记账）。保留区间位，通过把它排除在 madvise/reclaim 之外来生效。P1 不加逐页 `Locked` 状态。 | 低。 |
| THP | P1–P6 作为无元数据的旁路；P9 再建模。保留 `pt_map_huge` / `mm_demote_huge_page`，实现参考实现的 `split_if_mapped_huge`（`locking.rs:149`）以便 covering node 撞上 PMD 叶子时降级。 | **功能风险最高**。`ARCH_NO_PMD_LEAF` 已挡掉 3 个架构。 |
| NUMA 策略 | 免费，完全不动。`mm/mempolicy.c` 是 *节点* 属性而非映射属性，从不查询 VMA 列表。 | 无。 |
| /proc maps + smaps | 残留，**这一点必须承认**。maps 本质是区间视图。要么在逐页状态上重新实现区间合并（等于把论文要删的合并逻辑再写一遍），要么保留副本。永久保留 VMA 列表作为派生、非权威的区间提示，由同一批 mutator 维护，由调试期双向一致性检查器校验。 | **VMA 列表不会消失。承认这一点。** |
| fork / 文件页 COW | 逐页，而且模型在这里占优。`pt_clone()` 变成"克隆页表 + 复制 cls/cow 数组 + DFS 标记所有用户叶子"。这删掉了 `mm_file_cache_mapping_get()`：它今天在每一次私有文件叶子的 COW fault 上做一次 `page_cache_get`（加锁 + 引用计数），只为回答"这是不是那个规范 cache 帧"。一个状态字节即可回答。 | **新失败面**：不能忘复制数组，不能重复引用计数。高风险。 |
| VMO（原生 ABI） | 逐页，需新增 class。`mm_lookup_vmo_region()` 回答的是区间问题（"这整段是不是同一个 VMO"）→ 保留 VMA 作其权威。新增 `VMO` class 供拆除决策使用。 | 低。 |
| swap | 逐页，也是最干净的正确性收益。`Status::SWAPPED` 消掉 `PTE_SWAP` 逐架构窃取硬件位，并消掉六处先于 `PTE_V` 检查 `pte_is_swap()` 的顺序依赖（`mm.c` 与 `fault.c` 中）。这些顺序是承重的，正是 cursor 重写最容易踩坏的东西。 | 代码量低、示范价值高。 |
| fault-around | 存活，且更便宜。4 页 = 1 次 cursor 而非 4 次页表遍历。`handle_file_fault` 的"放锁 → I/O → 重锁 → 重校验"编排变成"无锁做 I/O，然后从元数据重校验（`FILE_*` class + 偏移匹配）而不是从 VMA"。 | 中。LoongArch64 自己的注释指出私有 cache 叶子在并行 rustc 下已经会破坏动态符号，迁移时不要"统一" la64/x86_64 的 `direct_private` 路径。 |

## 4. 分阶段计划

过渡态的调度规则：cursor 拥有映射真相；VMA 列表拥有区间策略、资源引用
（file_fd / vmo）与报告。这是下面各阶段能独立落地的原因。

| # | 交付物 | 结束时成立的不变式 | 验证门 |
|---|---|---|---|
| P0 ✅ | `pt_meta_t` 并入 `frame_meta_t`；`mm_pt_meta()`；`FRAME_F_PT` 审计 | 零可观测变化；`sizeof(frame_meta_t) == 16` | 7 个架构/NOMMU 构建；`pfa_audit_lists()` |
| P1 ✅ | 按页表页分配 `cls`/`cow`；`kernel/mm/mm.c` 每个页表写点同步元数据；独立遍历页表与元数据并逐一比对的审计器，挂在关机审计点 | *元数据 ≡ 页表状态，始终成立且机器可证* | `smoke-mm-stress`、`smoke-mm-fork-exec-race`、`smoke-abi-linux`、`check-mm-lock-model`；实测审计全 0 |
| P2 | 让 fault 路径经由 cursor 写 PTE（仍在 `mm->lock` 之下） | 所有 PTE 写入只有一条代码路径 | 上述三个 smoke + `smoke-vfs-stress` |
| P3 | covering node 之上 DFS 预序锁全部后代；逆序释放 | 每次 cursor 都按预序加锁、逆序解锁 | 新 `check-mm-pt-lock-order`（释放序断言 + 计数器）；`smoke-mm-stress` @ `NR_CPUS=4` |
| P4 | `core/rcu.c`；`stale` + 重试；`pt_unmap` / `pt_unmap_leaf` / `pt_destroy_*` 中的 `frame_free(child)` 改为延迟释放 | 任何遍历都到不了的页表页不会被回收 | 新 `smoke-mm-pt-race`：N 线程大范围 unmap 同时 N 线程 fault |
| P5 ✅ | 从 fault 路径摘掉 `mm->lock`，它收缩到只保护 VMA 列表与计数 | 不相交区间的写者**实测**不串行 | `smoke-mm-pt-race`（PASS，含非空断言 `mm_fault_from_status: 778`）；`smoke-mm-stress`、`smoke-mm-fork-exec-race`、`smoke-abi-linux` @ SMP=8 全 PASS，LOCK-STALL/MCS DEADLOCK/panic 全 0 |
| P6 | fault 分派改判 `Status` 而非 `mm_find_vma`；`handle_file_fault` 从元数据重校验 | page fault 分派读不到任何 VMA | 上述全部 |

> **P6 的可实现范围（2026-10-02 核对代码后收窄）**：原表述"page fault 分派读不到
> 任何 VMA"**只在分派层成立**，因为 status 字节能**分类**但不能**服务**。
>
> * status 字节的全部容量：4 bit class（`MM_ST_INVALID` … `MM_ST_PT_NODE`，共 9 类）
>   + `MM_ST_PROT_R/W/X` + `MM_ST_COW_BIT`。它足以回答论文 Fig. 8 的三态
>   （PrivateAnon / Mapped / Invalid），这正是**分派**所需的全部。
> * 但 `handle_file_fault()`（fault.c:755）要解引用 `vf->vnode`、`file_fd`、
>   `file_pos`，并调 `page_cache_get(vf->vnode, …)`；COW 路径同理需要一个可写的
>   文件后备。**这些对象引用在 status 字节里无处可放。**
> * 所以 P6 落地后，`fault.c` 里剩下的 `mm_find_vma` 调用会**保留在对象解析处**，
>   而不是消失：`fault.c:249`（COW）与 `898`（file fault）需要 vnode/fd。
>
>   **更正**：我先前把 `fault.c:1136` 说成"`mm_fault_from_status()` 里的 UFFD 存在性
>   判断"，这是错的。核对后：`mm_fault_from_status` 是 1033–1096，而 **1136 属于
>   `handle_demand_fault_access()` 的 VMA 路径**（`mm_find_vma` 喂给
>   USERFAULTFD_MISSING_HOOK 的 `userfaultfd_range_present()` 判断）。
>
>   更要紧的是，**快段自己的 UFFD 门禁（1046 行）已经是 status 驱动的**——
>   `mm_cursor_safe_test(&qcur, page_va, MM_SAFE_UFFD)` 读的是 cursor 里的 per-PTE
>   安全位，不查 VMA。所以"UFFD 原理上无法 status 驱动"这句话只对 VMA 路径成立：
>   UFFDIO_REGISTER 的注册单位确实是 VMA 区间，因此**权威**的重新判定必须在 VMA 上做；
>   但快段用 per-entry 安全位做**快筛**已经落地。两者不矛盾：安全位负责快路径分流，
>   VMA 区间表负责权威判定与 unregister 后的失效。
> * **更正**：我先前把 `378` / `458` / `476` / `669` 都算作「可去掉的分派点」，
>   逐个读过之后这个说法不成立：
>
>   | 行 | 实际用途 | 是分派吗 |
>   |---|---|---|
>   | `378` | swap-in 后 `fault_map(..., vma->pte_flags, ...)`，要**权限位** | 否 |
>   | `458` | brk 扩张的否定式判断「这里有没有 VMA」 | 勉强算 |
>   | `476` | 主 VMA 路径：`vma->pte_flags` / `vm_flags` / `file_fd` | 否 |
>   | `669` | 分配之后 `mm_find_vma(...) != vma` 的**并发重查** | 否 |
>
>   三处要的正是 status 字节装不下的东西（权限位、fd），第四处根本不是分派，而是
>   分配窗口里的竞态检测。
> >
> **结论（收窄后）**：在当前 status 字节容量（4 bit class + R/W/X + COW）下，
> **P6 基本不可实现**。唯一勉强算候选的是 `458` 那处 brk 否定式判断，价值也远小于
> 本行原始表述所暗示的。要真正推进 P6，得先扩宽 status 字节以携带对象引用
> （vnode / fd / offset）——那是**格式变更**而不是调度顺序变更，而**论文没有给出
> 这个格式**。
> >
> 所以 P6 与 P7 同类：**卡在缺少论文规格，而不是卡在工程量**。

> **P6 最后一处候选也已排除（实测）**：`mm_pt_provision_anon()` 全树只有三个调用点
> ——`mmap.c:195`、`elf.c:184`、`munmap.c:270`。**brk 增长不在其中**：
> `proc_brk()`（`proc/proc.c:732`）→ `mm_brk_locked()`（`munmap.c:177`），后者只遍历
> VMA 列表并在收缩循环里拆 PTE，**从不写 status**。
>
> 所以 `fault.c:458` 那处 brk 否定式判断（`!mm_find_vma(...)`）**连可供查询的
> status 都不存在**——brk 区域内的页在元数据里始终是 `MM_ST_INVALID`，与"有 VMA"
> 无法区分。
>
> **P6 因此没有可实现子集**：不是"大部分能做"，而是一处都做不了。7 处
> `mm_find_vma` 全部依赖 status 字节装不下的信息（对象引用、权限位、并发重查），
> 或者依赖尚不存在的生产者（brk）。
| P7 ⚠ | **2026-10-02 决定：`PTE_SWAP` 正式接受为唯一被容忍的例外，不删除。**原目标"无 PTE 位被重载"收窄为"仅 `PTE_SWAP` 一个，且逐架构显式声明"（见下方决定记录与 `kernel/include/mm/pt.h`） | `PTE_SWAP` 是唯一重载位 | 已达成：逐架构断言 `PTE_SWAP` 有定义（27 条）+ `{arch: PTE_SWAP} == SWAP_SUPPORTED_ARCHES`；`CONFIG_SWAP=y` 构建（默认即开）+ `smoke-swap` PASS。**但 swap-in 未被覆盖**，见下 |

> **P7 的阻塞点是硬件约束，且仓库里已写明**（`kernel/include/mm/pt.h:86-97`）：
> 「Storing this in the PTE's software-usable bits is not an option on A20OS:
> riscv32 and arm32 have no free software bits at their root levels, and
> loongarch64 aliases PTE_R/W/X onto the LA_PTE memory-attribute field.」
>
> 这把因果**倒过来**了：`pt_meta` 的 status 字节之所以是唯一可放之处，正是因为
> PTE 没有空位。所以 `PTE_SWAP` 不是"第一个该被消灭的例子"，而是**唯一一个被容忍
> 的例外**；本行"删除 `PTE_SWAP`"若不同时扩宽 status 字节的容量，就无处安放
> swap 状态（`MM_ST_SWAPPED` 已经用掉 9 个 class 里的一个，且 class 只有 4 bit）。
>
> 要做 P7 得先解决 status 字节容量，而扩宽它又与 P6 卡在同一处（需要携带对象
> 引用）。**P6 与 P7 是同一个根因**：status 字节位数不够。

> **P7 的阻塞可判定（宽度算术，2026-10-02 核对，已按 `swp_entry()` 复核）**：
> 注意阻塞的**理由**不是 `swap_entry_t` 的 `uint64_t` typedef——我先前这样写是不准确的。
> 真实宽度由 `swp_entry()` 决定（`kernel/mm/swap.c:58`）：
> `((offset & SWP_OFFSET_MASK) << SWP_TYPE_BITS) | type`，其中
> `SWP_TYPE_BITS = 4`，`SWP_OFFSET_BITS` 按指针宽度取 **20（32 bit）或 40（64 bit）**
> （`kernel/include/mm/swap.h:8-13`）。所以实际载荷是 **24 bit 或 44 bit**，不是 64。
>
> 结论不变但更精确：本次涉及的三个架构（riscv64 / x86_64 / aarch64）都是 64 bit，
> 载荷 **44 bit > 32 bit**，一个 32-bit 字装不下，因此元数据仍需扩到 **8 byte/条目**
> （PT 页多 8×512 = 4 KiB）。但在 32 bit 架构上载荷只有 24 bit，**4 byte/条目就够**，
> 内存代价只有一半。**结论按架构分叉，实现前必须先确认目标位宽。**
>

> `swap_entry_t` 是 **`uint64_t`**（`kernel/include/mm/swap.h:35`），而每个条目的
> status 只有一个 **`uint8_t`**（`kernel/include/mm/pt.h:154`，`cls[]` 数组元素），
> 其中 class 占 4 bit、COW 占 1 bit、prot 占 3 bit。COW **就在这一个字节里**
（`MM_ST_COW_BIT`）：曾经另有一张并行的 `cow[]` 位图，2026-10-04 随合并删掉了，
理由见 §13.19——没有任何读者，只有审计在读，而它恰好对 fault-around 别名出来的
那批页是陈旧的，于是审计在一次内核完全正确的运行上报了 `cow=153`。
>
> `MM_ST_SWAPPED` 只能表达"这一页已换出"，**无法表达"换到哪里"**——那是 64 bit 的
> swap entry（设备号 + slot）。所以 `PTE_SWAP` 被删掉之后，这个 64 bit 值在当前
> 元数据格式里**无处安放**：PTE 不能存（见上，`pt.h:86-97` 已说明各架构无空位），
> 元数据也放不下。
>
> 因此 P7 不是"工程量大"，而是**需要一个尚未做出的格式决策**：要么 status 元数据
> 从 1 byte/条目 扩到 8 byte（每个 PT 页多 8×512 = 4 KiB，对 2 MiB 映射是实打实的
> 内存放大），要么引入按地址索引的旁表（换来一次额外查表）。论文没有给出这个选择，
> 所以按论文实现 P7 是做不到的——**缺的是规格，不是工作量**。
| P8 | 双向一致性检查器（P1 审计器覆盖反方向）；**`MM_LOCK_MODEL` 拆分**（见下注，**不是**改名）；同一提交内更新门禁与 `docs/testing-gates.md` | VMA 列表可证为纯派生 | `check-mm-lock-model`、`check-final-definition`、`check-doc-test-gates` |
| P9 | *(不承诺)* mseal/mlock/brk 逐页化；THP 进 `Status`；删除残留区间结构 | — | — |

> **`smoke-swap` 覆盖了什么、没覆盖什么（2026-10-02 实测）**
>
> 实跑 `python3 tools/smoke.py smoke-swap` → PASS，日志只有两行有效输出：
> `SWAP_TEST: swapon ok, totalswap=16769024`（16 MiB 设备）与 `SWAP_TEST: PASS`。
>
> **已覆盖**：`mkswap`/`swapon` 成功；`/proc/swaps` 与 sysinfo `totalswap` 账目；
> 重复 `swapon` 返回 `EBUSY`；`swapoff` 后 `totalswap` 归零；以及 mkswap/swapon 期间
> 经 loop 块适配器的真实 header/badmap I/O。
>
> **未覆盖：真正的换出与换入。** 这不是测试没写好，是**结构上够不到**——
> `swap_test.c:11-19` 自己就写明了这条边界，实测参数印证：
>
> | 量 | 值 | 后果 |
> |---|---|---|
> | `TOUCH_SIZE` | 8 MiB | 匿名内存只触碰 8 MiB |
> | `OOM_MIN_FREE_PAGES` | 256 页 ≈ 1 MiB | 触发回收要全局空闲帧低于此值 |
> | `MAX_SWAP_RECLAIM` | 8 页 / 2s 冷却窗 | 换出速率上限约 4 页/s |
>
> 1 GiB QEMU 里空闲帧远高于 256 页，回收根本不触发；**即使触发**，把 8 MiB
> （2048 页）换出按 8 页/2s 算需要约 **512 秒**。所以 `swap_out_victim_pages()` 与
> `swap_read_page()` 在此 smoke 中**从未执行**。
>
> **因此 P7 的验收标准不能写"swap-in 测试通过"——那是假的。** 要真正关闭，需要另建
> 一个低内存实例（压到 `OOM_MIN_FREE_PAGES` 以下）并跑够冷却窗数，或另设专用回收门禁。
> 靠调大 `MAX_SWAP_RECLAIM`/调低 `OOM_MIN_FREE_PAGES` 去迁就测试是反的：那样门禁
> 校验的就不是默认配置了。
>
> 保留 `PTE_SWAP` 这个决定本身不受影响：它是被**保留**而非被替换的行为，上面已覆盖的
> 账目与错误路径仍全部有效。未覆盖的是**回收换出/换入路径**，这是 P9 之外的既有缺口，
> 不是方案 C 引入的。

> **P7 决定（2026-10-02，方案 C）**：`PTE_SWAP` **保留**，作为唯一一个被容忍的
> 例外，不再追求"零位重载"。
>
> 理由（本轮实测，不是推断）：
>
> * `swp_entry()` 真实载荷 **24 bit（32 位架构）/ 44 bit（64 位架构）**
>   （`SWP_TYPE_BITS 4` + `SWP_OFFSET_BITS 20|40`，`swap.h:8-13`）。44 > 32，一个
>   32-bit 字装不下，所以"塞进元数据"要求 status 扩到 **8 byte/条目 = 每 PT 页 4 KiB**
>   （512 条目）。而绝大多数 PT 页**永远不会有页被换出**——为纯度不变式预付这笔钱，
>   代价与收益不成比例。
> * 更关键：这个例外在半数架构上**不是"一个普通位"**。实测 6 个架构的
>   `PTE_SWAP` 定义：
>
>   | 架构 | `PTE_SWAP` | 性质 |
>   |---|---|---|
>   | x86_64 / aarch64 / loongarch64 | `PTE_LEAF` | **别名叶子标记位** |
>   | riscv64 | `1UL << 9` | 独立位（PTE_COW 之后、PPN 之前） |
>   | arm32 | `1U << 7` | 独立位 |
>   | ppc64le | `0x2` | 独立位 |
>
>   即三个架构上 `PTE_SWAP` 占用的是叶子 PTE 里语义最重的那一位。编码是
>   `!PTE_V && PTE_SWAP` ⇒ 已换出（见 `pte_is_swap()`）。这正是 `pt.h:91-96` 说的
>   "steals a hardware-meaningful bit per architecture"——删掉它不是把一个标记改成
>   另一种标记，而是要在**没有空位**的 PTE 里重新安置一个 44-bit 的值。
> * 论文没有给出替代格式。所以"删除 `PTE_SWAP`"不是"照论文实现"，而是需要先做一次
>   无人做过的格式设计（status 扩宽 or 旁表），并承担上面那份预付内存。
>
> **因此不变式改为**：`PTE_SWAP` 是唯一重载位，且必须逐架构显式定义——由
> `check-mm-pt-lock-order` 钉住。**这条不变式还有一个跨文件耦合，一并钉住**：
>
> ```
> {arch : 定义了 PTE_SWAP}  ==  SWAP_SUPPORTED_ARCHES
> ```
>
> 两个方向坏法完全不同。往 `SWAP_SUPPORTED_ARCHES` 加一个没定义 `PTE_SWAP` 的架构
> → 编译直接炸（`fault.c`/`mm.c`/`munmap.c`/`exit.c` 报错），吵但安全。反过来，**新架构
> 定义了 `PTE_SWAP` 却忘了改 Makefile** → `CONFIG_SWAP` 被静默降为 `n`，swap 整个消失，
> `pte_is_swap()` 退化成恒假，读起来像"这个架构本来就不支持 swap"而不是像疏漏。
>
> 逐架构的存在性检查看不见后者，所以门禁额外用 `^...$` 锚定整行列表。
> （这里也踩了和上一条一样的坑：`fixed = true` 的字面匹配在列表**变长**时会通过——追加
> 一个架构后原串仍是子串。两次都是同一个"子串即通过"的坑。）真正要防的不是它存在，而是它被当成"还能再偷一个位"
> 的先例；`pt.h:91-96` 原文（"precisely the hazard a general status encoding would
> multiply"）说的正是这个风险。

> **P8 指令更正（2026-10-02）**：本行原写 `MM_LOCK_MODEL` → `MM_AS_MODEL`
> **改名**。照做会**破坏文档语义**，已核对代码后撤销：
>
> * `vm.h` 现在**同时**带两个名字且含义不同——`MM_AS_MODEL`（107 行，单级模型，
>   即目标）与 `MM_LOCK_MODEL`（156 行，过渡期两层契约）；
> * 109 行原文写着 `MM_LOCK_MODEL`「still governs every mutator that has not yet
>   been converted」——它是**活的**契约，不是待清理的旧名；
> * 改名会把目标模型与过渡期契约**合并成同一个名字**，读者再也无法区分
>   「已迁移」与「仍受 `mm->lock` 约束」；
> * `check-mm-lock-model` 与 `check-final-definition` 两条门禁都**断言**
>   `MM_LOCK_MODEL` 字面量存在于 `vm.h`，改名会静默地废掉这两条门禁的语义。
>
> P5 落地后的真实边界（已核对）：`fault.c` 仍有 5 处 `spin_lock(&mm->lock)`，
> **状态快段是唯一不再取它的 PTE 写路径**；COW / file / VMA 路径的 install 仍取。
> 所以 `MM_LOCK_MODEL` 156 行那条「demand fault installs 必须持 `mm->lock`」现在
> **只对非状态路径成立**，需要的是加一条例外说明，而不是把整节改名或删除。
>
> P8 剩下的实际工作因此是：①为状态快段在 `MM_LOCK_MODEL` 里补一条明确例外；
> ②补双向一致性检查器；③同提交更新门禁与 `docs/testing-gates.md`。

> **①已完成**：`vm.h` 的 `MM_LOCK_MODEL` 已补上状态快段的明确例外（并写明它依赖的是
> `mm_pt_node_lock` 的按地址互斥，而不是"没有 `mm->lock`"）。`MM_LOCK_MODEL` 字面量
> 保留——`check-mm-lock-model` 与 `check-final-definition` 两条门禁都断言它存在于该文件。
>
> **②的双向缺口已定位，但本轮未落地**。现有 `audit_table()`（pt.c:1517）只走**一个
> 方向**：自 PTE 树下降，逐条比对"元数据是否与该 PTE 一致"（class / prot / cow）。反向
> ——"元数据声称的每一页，PTE 是否同意"——**不存在**。
>
> 这个缺口在本代码库里不是理论问题：`audit_table()` **只下降有效的 PTE 条目**，所以
> 父项已被清掉的 PT 页**根本不会被访问**。而那正是 `pt_unmap_leaf()` +
> `mm_pt_retire_table()` 每天在做的事（清 `parent[idx]`、标 `stale`、延迟释放）。于是
> "一个已脱树的 PT 页，其元数据仍声称若干页 present"这一类缺陷，对现有审计器**完全
> 不可见**——它只会报告 0。
>
> 落地它需要注意（留给下一轮，不要当成一行改动）：
>
> * 新增遍历方向必须走**元数据侧**（存活的 `FRAME_F_PT` 帧），而不是再走一次 PTE 树，
>   否则方向不会变；
> * **退役列表上的帧必须豁免**：`mm_pt_retire_table()` 刚标 `stale` 而尚未 free 的帧，
>   其元数据与 PTE 不一致是**设计如此**，不是缺陷；
> * 需要新的 report 字段（现有 `mm_pt_audit_report_t` 只有单向的
>   `present/absent/prot/cow` mismatch），并接进 `MM-ASM` 关机审计行；
> * 门禁要跟着改：`MM-ASM` 的正则目前逐字段断言全 0，新增字段必须同样被断言，否则
>   新检查又是一个"绿但没在测"的口子——这正是本项目已经栽过两次的坑。

> **编号更正（2026-10-02）**：本表的 P 编号是权威的。会话里曾用"Phase 3 / Phase 4"
> 这套临时说法，其中 **"Phase 3" 实为 P5**，而 **"Phase 4（上层统一状态预标记）"
> 在本计划里并不存在**——真实 P4 是延迟回收（`mm_pt_retire_table` + `stale` +
> `pt_readers`），早已落地。看到会话记录里的 "Phase N" 时一律按本表换算。
>
> P5 落地时顺带修掉了 MCS 节点锁的两个致命 bug（交接从未实现：等待者从不挂链、
> 解锁方从共享字段回读自己的节点），此前因 `mm->lock` 串行化而从未被执行。形式化
> 模型见 `docs/research/verification/LeafLock.tla`，两个 bug 作为 mutant 均被模型
> 捕获。

**P1 是整个计划里性价比最高的一步，且不可能搞坏启动**：它是影子状态、可机器验证，
而且会找出你不知道自己有的 bug。

## 5. 风险排序

1. 页表页或帧在有 cursor 处于其子树内、或存在可抵达它的陈旧 TLB 项时被释放。
   这正是本项目历史上 `0x63636363` 与"进程退出 teardown 提前释放 VMO 帧"的
   形状。P4 未落地前，`pt_unmap` / `pt_unmap_leaf` 里的 `frame_free(child)` 与
   `pt_destroy_level` / `pt_destroy_user_recursive` 的递归释放都无锁、无
   TLB flush。`mm_demote_huge_page` 先 `pt_unmap_leaf` 再 `frame_put`，中间
   只有 `mm_tlb_hold_frame` 引用而非真正的 shootdown。
2. 锁或释放共享内核半子树 → 全系统死锁或系统级 UAF。已在代码层面拒绝，但
   P3 的 DFS 锁必须继承这条约束。
3. P5 收窄锁范围引入 TLB-IPI 死锁。`vm.h` 的 `MM_LOCK_MODEL` 精确记录了
   这条约束：远端 CPU 带中断关闭地自旋在 `mm->lock` 上，必须能退出该临界区
   去响应 TLB IPI。一旦 fault 路径不再持有 `mm->lock`，"带 IRQ 关闭持有页表页
   锁"就成了同一类死锁。**cursor 必须在派发远端 shootdown 之前释放所有锁**，
   这正是 `mm_tlb_invalidate_finish` 现有形状。
4. 文件映射 / COW 路径。`handle_cow_fault_locked` 有三条按
   `mm_file_cache_mapping_get` + `refcount` 区分的所有权规则；源码里那两段注释
   是 `0x63636363` 事故的疤，编码了特定顺序（在 `pfa.lock` 下先更新 PTE、
   再 `frame_put`）。把 page-cache 查找换成一次元数据读取会改变引用计数读取
   相对 PTE 写入的时序。这些顺序必须逐字保留。
5. **swap 的 PTE 检查顺序**（六处）。若 cursor 的 `query()` 只报"是否 present"，
   swap-in 会被静默当成"未映射"。P7 的 `Status::SWAPPED` 是强制项。
6. 元数据与 PTE 静默分叉（P1 漏掉某个写点）。审计器是唯一防线，且必须在
   `smoke-mm-stress` 构建里启用，而不是只在一个 debug 变体里。
7. loongarch32 的软件 TLB refill 从异常处理器**异步无锁读 PTE**，这是任何
   cursor 协议都保护不到的读者。在 refill 被纳入 RCU 读侧之前，**把 la32r 排除
   在迁移范围之外**。
8. 在 PTE 写入时把元数据标错类型（只读别名 / NX 冲突）。注意 aarch64 上
   `PTE_D == PTE_W`（bit 56），x86_64/loongarch64/arm32/la64 上
   `PTE_SWAP == PTE_LEAF`。"叶子"标记就是"swap"标记。任何从零重建 flags
   而不是保留旧字的地方都会破坏这个区分。

## 6. 明确不承诺（不要声称）

> **2026-10-04 更新：第 1 条与第 8 条已被 §13.18 推翻，改为记录现状。**
> 当初写这一节时删除 VMA 抽象还远未开始，第 1 条是一道禁止说真话的规则。
> 工作做完后规则本身成了最误导人的一句话，所以这里改写它，而不是留着它
> 与 §13.18 互相矛盾。§13.6 当年明确承诺"那时才能改写"。

1. ~~"VMA 抽象已经没了。"没有。~~ **（已改写）** 曾经它永久降级为非权威区间索引，
   服务于 `/proc/{maps,smaps}` 与区间策略（brk、mseal、mlock）。现在两个映射表示
   已合并为一条 `struct mm_seg`（§13.18）：`vm_area_t`、`vma_index[]`、
   `mm_find_vma()` 与第二个映射表示全部删除，一条记录、一条链表、一个索引、
   一次查询。**"映射没有第二种表示"现在是事实，且由门禁强制**——
   `[MM-ASM]   map list: overlap=0 dead=0 ok=entries` 逐条核对链表有序、存活、不重叠。
   但要说清边界：区间索引本身没有消失，它就是那条记录；消失的是**第二份**表示。
   另外 `/proc/maps` 仍由链表派生（不是由每页状态派生），见第 8 条。
2. "不相交区间并行执行。"在 **P5 落地并有实测之前**都不成立。
3. "大页已进入模型。"P1–P6 期间 THP 是无元数据旁路。
4. "mseal 已成为逐页属性。"它仍是区间结构。
5. "arm32 与 armv7m-NOMMU 参与其中。"`kernel/arch/arm32/mm/pgtbl.c` 有自己
   的页表核心且 L0 有 4096 项（元数据开销超过页本身）；armv7m 是 `CONFIG_NOMMU`。
   两者都留在同一套 `mm_*_locked` API 背后的旧路径上。*接口*是统一的，*引擎*
   按架构组划分，这正是 `mm.c` 现有的 `#if ARCH_HAS_PGTABLE_OPS` 形态。
   目标是 5 个 512 项的 64 位架构。
6. "loongarch32 参与其中。"软件 TLB refill 是无保护的异步读者。
7. "swap 已迁移。"除非 P7 落地**且** CI 里有 `CONFIG_SWAP=y` 构建。
8. ~~"`/proc` maps 由 `Status` 派生。"它由 VMA 副本派生。~~ **（已改写）**
   现在：它由**映射记录链表** `mm->mmap` 派生。此前那句话说错了两次——彼时
   "VMA 副本"本身就不存在（`/proc/maps` 一直读链表），而且它与 `Status` 之间
   是被审计交叉校验的关系，不是单向派生。合并之后这条关系更清楚：链表给区间与
   后端，每页状态给页属性，审计 `vmai` 计数器从**两个方向**核对二者
   （每个状态非空的页必须有记录覆盖；每个有驻留页的记录必须有对应状态）。

**P1–P6 落地后成立的**（在 5 个 512 项 64 位架构上）：单一权威的映射
表示；它与旧 VMA 路径之间的机器可证等价；所有 PTE 变更经由一条 cursor 代码
路径；所有写者互斥落在页表页锁上且释放顺序可证；任何页表页都不会在 grace
period 内被回收；file/COW/swap/fault-around 的分派由逐页状态而非区间查询驱动。
这是一个真实的结果，但它不等于"CortenMM 已移植"，两者相差约六周的工作量。

## 7. 门禁

`check-mm-lock-model` 依赖 `smoke-mm-stress` + `smoke-mm-fork-exec-race`，
这两个是全部 9 个阶段的回归地板。当前它还额外 grep
`MM_AS_CURSOR_ONLY_ENTRY`、`mm_pt_note_present|mm_pt_note_absent`、
`mm_pt_audit_all`，以便门禁真正强制新模型而不是随实现一起腐化。

关机时的 `MM-ASM` 审计行是事实记录而非断言。

> **2026-10-04 更新**：此处原列的字段表只到 `vma`，且把 `vma=0` 解释为"两个表示
> 一致"。合并之后"两个表示"指的是**映射记录链表**与**每页状态元数据**这一对，
> 而不是两套映射结构；`vmai`/`cls`/`safe`/`anon_virt` 与 `seg_*` 也都是判定字段。
> 完整字段与含义见 `docs/testing-gates.md` 的表，这里只保留结论。

判定字段（必须为 0）现在覆盖：

| 方向 | 字段 | 断言的是 |
| --- | --- | --- |
| 记录 → 状态 | `vma` | 有驻留页的记录，其范围内必须有非空状态（漏改一侧的 mutator）|
| 状态 → 记录 | `vmai` | 状态非空的页必须有记录覆盖（孤立的已映射页）|
| 两者 | `present`/`absent`/`prot`/`cow` | PTE 与状态对"是否映射/什么保护"看法一致 |
| 两者 | `cls` | 双方都认为已映射时，对**是什么**的判断一致 |
| 两者 | `safe` | `MM_SAFE_NO_FA` 与 `VM_SEALED` 双向一致 |
| 命名 | `seg_bad` | 节点条目里的名字解析为空或指向 magic 已失效的记录（会跟着已释放的 vnode 走）|
| 命名 | `seg_kind` | 节点条目命中的记录，其 kind 与链表查到的那条覆盖该地址的记录不一致（映射被贴错标签）|
| 链表 | `map list` 的 `overlap`/`dead`/`ok` | 记录有序、存活、不重叠（§13.18 新增）|

观测字段（非判定）：`pt_pages`、`entries`、`seg_slots`、`seg_ok`、`seg_miss`、
`seg_dispatch`、`seg_fallback`、`anon_virt`。

`smoke-mm-stress` 之类的门有一个陷阱：它通过 grep `MM_STRESS: PASS` 判定，而该标记
在关机审计之前打印，所以关机路径上的 panic 不会让门失败。审计行必须与 PASS
标记一起检查。

---

## 8. 实施记录（2026-09-28）与 P5 的实测结论

P0–P4 的实际落地结果，以及一次把 P5 的前提测了出来的重要实验。所有数字均为
smp4 QEMU 实测。

### 8.1 已完成并验证

| 阶段 | 状态 | 证据 |
|---|---|---|
| P0 描述符 | ✅ | `pt_meta_t` 并入 `frame_meta_t` 联合体，16 字节零开销（`_Static_assert` 钉住） |
| P1 per-PTE 状态 | ✅ | 关机审计：7 架构构建 OK，`missing_meta=0 present=0 absent=0 prot=0 cow=0 vma=0` |
| P2 fault 走 cursor | ✅ | 缓存下降路径；6 个构建 + `mm_stress`(smp1/smp4) + `mm-fork-exec-race`(smp8) + `smoke-abi-linux` + `check-mm-lock-model` 全 PASS |
| P3 页表页锁为互斥单元 | ✅ | covering-node MCS；修掉 depth 双减自死锁 |
| P4 延迟回收机制 | ✅ | 读侧计数 + `tlb_holds`  graveyard + stale；已就位待 P5 接线 |
| 度量基准 | ✅ | `user/cmds/stress/mm_pt_scale.c`（已删除，git 历史可考），把并行主张变成可证伪验收门 |

期间修掉两个真实缺陷：fault-around 曾把 PFN 当物理地址传给 `mm_cursor_map`
（装入无效物理地址，表现为访问故障而非缺页，C 无法发现类型不匹配）；MCS
回退循环与 `mcs_unlock` 双减 per-CPU depth 导致同节点自死锁（LOCK-STALL
报 `waiter==owner` 同址）。

### 8.2 P5 实测：并行性的有效基线（2026-09-28 修正）

> **本节此前的一版结论是错的，已作废并重写。** 原版在 riscv64/TCG 下测得
> speedup=1.03x，据此断言"fault 被 mm->lock 串行化"。该测量无法区分内核串行
> 与环境串行，而 riscv64 guest 跑在 x86_64 host 上只有 TCG、跨架构无可用的
> 多线程 TCG，vCPU 本身即被串行化。

新增环境探针 `user/cmds/stress/cpu_scale.c`（已删除）：纯整数计算、不碰内存、不碰任何
内核锁。它给出该环境能否呈现并行性的上界：

| 配置 | cpu_scale（纯计算上限） | mm_pt_scale（真实 demand fault） |
|---|---|---|
| riscv64 / TCG / smp4 | 0.89x – 0.96x | 1.03x（无效，无参考价值） |
| x86_64 / **KVM** / smp4 | **3.93x** | **1.48x – 1.53x** |

结论（以 KVM 为准）：

* 环境本身能线性并行（3.93x），所以 x86_64/KVM 是唯一有效的并行性度量环境。
* 真实 demand fault 工作负载只拿到 1.48x，相对环境上限损失约 2.4x。这说明
  fault 路径**确实**存在实质串行化，P5 值得做。
* 但"串行化来自 mm->lock"这一归因**仍未被单独证实**：该工作负载每轮还要 mmap
  新区域（`mmap` 本身要取 `mm->lock` 做区间分配与 VMA 插入），且每页 fault 都要
  `pfa_alloc_page()` 走 buddy 的全局锁、以及整页 `memset`。在把这三项成本分离
  之前，不能断言瓶颈就是 `mm->lock`。

**因此本节修正后的结论是**：单级模型的并行优势在 A20OS 上**尚未兑现**
（1.48x vs 环境上限 3.93x），P5 方向成立且值得继续。

### 8.2b 成本归因：并行度到底损失在哪（mm_fault_cost，x86_64/KVM smp4）

新增 `user/cmds/stress/mm_fault_cost.c`（该文件已删除）：同一线程数、同一共享地址空间、同一不相交
区间，把三���成本分开单独测。

| 相位 | 隔离的是什么 | 1T | 2T | scale |
|---|---|---|---|---|
| `mmap` | 只 mmap+munmap，从不触碰页 → 地址空间锁 | 0.168s | 0.275s | 1.22x |
| `fault` | 一次 mmap 后逐页首触 → demand fault 路径 | 0.091s | 0.126s | 1.44x |
| `mem` | 预触后反复写 → 内存路径上限 | 0.047s | 0.052s | 1.78x |

**归因（首次被独立证实，而非由端到端数字推断）**：

* 内存系统**不是**瓶颈（1.78x 接近 2x 理想值）→ P5 有真实空间。
* 串行主因确为 `mm->lock`，且 **`mmap`/`munmap` 比 fault 更糟**（1.22x vs 1.44x）：
  区间分配与 VMA 插入整段都在 `mm->lock` 内。这把"P5 = 只改 fault 路径"的
  范围估计缩小了；`mmap`/`munmap` 同样是必须处理的串行源。
* 因此后续 P5 的收益上限约为 1.44x → 1.78x（fault 侧）与 1.22x → 1.78x
  （mmap 侧），而非"数量级"提升。

**度量陷阱记录**：`mem` 相位的迭代数必须远大于预触页数，否则测到的仍是 fault
成本。初版即栽在这里：预触 1024 页 ≈ 10ms 与写循环同量级，测出 1.13x 的假象；
把 `MEM_ROUNDS` 提到 4096 后才得到可信的 1.78x。

### 8.3 P5 的真正前提：fault 必须能从 per-PTE 状态解析（P6）

要让 fault 不碰 VMA，`mmap` 必须把"这段是私有匿名、按需分页"的意图写进
per-PTE 状态。但这引出一个结构性问题：per-PTE 状态挂在**页表页的**元数据上，
而 on-demand 区域此刻尚无叶子页表页。若按论文 `mark` 语义在 mmap 时就为整段
分配叶子页表页，则 1 GB 映射要预分配 2 M 个叶子页表页（2 GB 页表内存才能表达
"尚未 fault"），这是 A20OS 现有 on-demand 设计刻意避免的成本。论文靠大页状态
规避，A20OS 的大页尚未建模（P9）。

因此 P5 存在一个必须显式决策的架构分叉（**不属于可以顺手做掉的量级**）：

- (a) 大地址空间按需标记：只在已有叶子页表页的页上记状态，其余仍回落 VMA。
  内存友好，但并行收益不完整。
- (b) 论文式 eager 叶子页表分配：完整兑现并行语义，接受大映射的页表内存开销
  （可用大页状态缓解）。
- (c) 给 VMA 加快照/引用计数：fault 在锁内取引用、锁外用引用解析，mm->lock
  只在"取引用"这一瞬被持有。这是 Linux 等成熟内核的通用做法，改动面集中在
  VMA 生命周期，风险中等。

本轮刻意**未**替用户选定 (a)/(b)/(c) 并强行实施；它牵涉内存/兼容性权衡，且必须
与"P4 延迟释放接进拆链路径"同一次提交（P5 摘锁正是 UAF 变真实的时刻）。当前交付
停在"机制齐备 + 前提测清 + 验收门就位"，是一个可回退的中间态。

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
| 4 | panic in `x86_64_smp_remote_tlb_flush` | 摘锁后持页表锁的 CPU 妨碍远端 TLB IPU 完成，正是 Oracle 风险 #3 |

第 3 项是真实缺陷且已修复并提交（`91f3eafc`）；第 1、2、4 项随快路径一并回退。

**第 4 项是关键教训**，也是下一步的硬性前提：Oracle 风险 #3 要求
"cursor 必须在派发远端 shootdown **之前**释放所有页表锁"，而当前
`mm_tlb_invalidate_begin/finish` 的事务边界是围绕 `mm->lock` 建的
（`vm.h` 的 `MM_LOCK_MODEL` 明确记录：带 IRQ 关闭自旋在 `mm->lock` 上的远端
CPU 必须能退出临界区去响应 TLB IPI）。摘掉 `mm->lock` 就必须同时重建这条
边界，否则会制造一类新的 TLB-IPI 死锁。

结论：P5 不是一次"把锁挪走"的改动，而是必须与 P4 延迟释放接线、TLB 事务边界
重建、以及 VMA 生命周期方案一起设计并整体验证。单独做局部版本会连续踩到上述四
类问题中的三类。

### 8.8 P0–P4 的性能账：目前**没有可测量的性能变化**

在 x86_64/KVM smp4 上，对 `main`（移植前）与 `cortenmm`（P0–P4 完成后）各取
4 次 `mm_fault_cost` 采样取均值：

| 相位 | main 均值 | port 均值 | 差异 | main 极差 | port 极差 |
|---|---|---|---|---|---|
| `mmap`  | 1.098x | 1.118x | +1.8% | 0.090 | 0.160 |
| `fault` | 1.413x | 1.405x | −0.5% | 0.100 | 0.090 |
| `mem`    | 1.633x | 1.650x | +1.1% | 0.040 | 0.280 |

**噪声地板**：对照相 `mem` 不产生缺页、也不做 mmap churn，我的改动在原理上不可能
影响它，而它在同一个 port 二进制上的极差达 0.28x（1.50x–1.78x）。以此为
噪声地板，三项差异（+1.8% / −0.5% / +1.1%）**全部落在噪声内**。

结论：

* P0–P4 目前不带来可测量的性能变化。早前单次采样看到的
  `mmap=1.22x / fault=1.44x / mem=1.78x` 是噪声分布的上沿，不是改进。
* 这与预期一致：单级模型带来的并行度只有在 `mm->lock` 不再串行化 fault/mmap
  之后才兑现，而那正是 P5/P6（见 §8.7 与 §8.3）。
* P0–P4 的实际交付是结构与正确性，不是性能：per-PTE 状态成为映射状态的权威
  表示、页表页锁成为写者互斥单元、延迟回收机制就位、per-frame 数组零额外开销
  （16 字节，`_Static_assert` 钉住）、以及关机审计证明两种表示在全工作负载上一致
  （审计全 0）。这些是 P5 得以可能的前提。

**方法论教训（比结论更重要）**：单次采样在这个环境里毫无意义，噪声地板高达
±9%，且噪声最大的恰是"本应不受影响"的对照相位。今后任何性能主张都必须在
x86_64/KVM 上、对照与实验各取多次采样、并**同时报告一个改动无法影响的
对照相位**来确立噪声地板，否则结论不可信。

### 8.9 VMA 引用计数已落地（`b10b267b`）

P6 的前提是 fault 能在无 `mm->lock` 下读取 VMA 字段。锁一放开，`vm_area_t`
指针本身就不再受保护：`mm_find_vma()` 返回后 `munmap` 可以摘链并释放。因此先
落地 VMA 生命周期机制，这是 P6 的前提，本身不是 P6。

所有权规则：地址空间链表持有分配时创建的那一份引用；摘链即释放它；最后一个持有
者（可能就是一把锁都没有的 fault）负责安排释放。延迟释放列表换用独立的
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
`spin_lock` → `spin_lock_at`（`core/lock.h`）**不碰中断状态**：函数体内唯一的
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
  cursor），且确实在 fault 路径持有 `mm->lock` 期间取得。但如上，持有者中断是
  开的，仍可响应 IPI，故不构成 §8.9 所述的阻塞环。

**残留的真实风险**（与 §8.7 第 4 项那次 panic 仍可能相关，但机制不同）：任何
`spin_lock_irqsave(&mm->lock)` 的写者临界区（`mmap`/`munmap`/`mprotect` 等）都
关中断。若 TLB IPI 恰好落在该窗口，目标 CPU 无法 ack，发起方 5 秒后 panic。
在 KVM 下该窗口可被宿主调度任意拉长，看起来就像 hang，这属于已修复的 MCS 看门狗
「幽灵死锁」同一类（那次修复只覆盖 MCS 自旋，不覆盖 `spin_lock_irqsave` 窗口）。

**对 §8.7 结论的修正**：「必须同时重建 TLB 事务边界」这一条**不再成立为硬前提**。
先前把它列为硬前提，是因为误以为 fault 路径持锁期间关中断。实际不关中断，
IPI 投递不被阻塞，因此 P5 摘 `mm->lock` 并不会因此制造新的 TLB-IPI 死锁。
P5 仍须满足的硬性要求收敛为：装 PTE 的互斥单元（cursor/MCS 锁）不得跨越
`mm_tlb_invalidate_finish`／`mm_tlb_shootdown_page` 的派发与等待。

§8.7 第 4 项那次 panic 的确切根因仍未定案，需要当时的 diff 才能确认，不能靠重构
推理断定。此处记为未决。

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
而它并未 flush），且没有读者去纠正，陈旧翻译会**永久化**：`munmap` 后同址
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

1. UAF：在 `vma_put()` 之后返回 `-EAGAIN`，调用方会继续 fall through 并在
   **没有引用**的情况下解引用 `vma`。exec 期间 VMA 频繁拆建，命中该窗口即损坏。
   改为：取引用之后的任何结局都必须是确定值（0 / -1 / -ENOMEM），`-EAGAIN` 只允许
   在 `vma_get()` 之前返回。
2. 抢占 THP：快路径插在 `handle_demand_fault_access` 顶部，早于
   `handle_demand_fault_locked` 里的 THP 判定，于是 4KiB 单页会抢在 2MiB
   透明大页之前。改为在守卫里排除 `VM_HUGEPAGE`。

修掉这两个之后**仍然失败**：`smoke-mm-stress` 在 `execve mksh` 之后静默卡死
（无 panic、无自旋锁诊断、无看门狗）。插桩显示快路径本身被进入 4 次且每次都以
`mapped=1` 正常返回，卡死发生在其**之后**。回退 `fault.c` 后同一门立即 PASS，
故为本轮改动所致，非既有 flake（判定方式：stash/unstash 对照）。

**结构性教训（比这个 bug 本身更重要）**：`handle_demand_fault_locked` 是一条
有序的判定链：swap PTE → 栈增长 → brk → VMA 命中 → 权限 → VM_FILE →
VM_VMO → THP → anon batch → 单页 anon。把快路径前置到这条链之前，等于改变
了这些判定的优先级；每排除掉一个（VM_STACK、VM_HUGEPAGE……）就暴露下一个尚未
建模的交互。这与 §8.7 四次失败是同一类问题的第五次化身：**在一条既有判定链上
"提前插队"比"就地改造"危险得多。**

若继续 P5，正确的做法是**就地**改造：把锁外 prepare 放进
`handle_demand_fault_locked` 内、原 anon batch 所在的精确位置，从而完全保留
其前面的全部判定顺序，只改变"工作发生在锁内还是锁外"；这需要先把该函数按
判定分段、逐段调整锁的粒度，而不是在前置位置加一个早返回。**本轮未完成该改造。**

### 8.13 P4 接线状态核实：`mm_pt_defer_free` 是死代码

核实结果（此前文档表述不够精确）：

* 叶数据帧的延迟释放已接线：`mm_tlb_hold_frame` 在 `munmap.c:105/236`、
  `madvise.c:81`、`oom.c:135`、`vm.c:402` 均有真实调用。
* 页表帧的延迟释放未接线：`mm_pt_defer_free`（`pt.c:797`）已实现，内部正确
  调用 `mm_pt_hold_table` 并置 `stale`，但**全树没有任何调用者**，是死代码。

即：当前只有数据帧受 P4 保护，页表帧仍是同步释放。这在**现有**设计下是安全的
（`mm->lock` 事务 + 同步 shootdown 完成后才释放），但它是 P5 摘锁的硬前提。
接线它本身不产生性能收益，却会在现有锁模型下引入页表帧生命周期风险，因此
**刻意推迟到与 P5 同批落地**，而不是单独提交。

### 8.14 验收环境发现的两个无关既有缺陷

* **lwIP IPv6 接收路径断言崩溃**：`pbuf_free: p->ref > 0`，backtrace 为
  `ip6_input → ethernet_input → a20_lwip_process_netif_rx_tx_locked`，发生在
  启动阶段、`MM_PT_SCALE` 尚未运行。x86_64/KVM 上 4/5 次运行命中，去掉
  `-netdev` 后 0 次，由 virtio-net 设备触发。与本次改造无因果关系
  （无任何 VMA 路径通往 lwIP pbuf 引用计数），但会污染后续 KVM 采样：
  **mm_* 类基准应在无 NIC 的配置下运行。**
* **`mm_pt_scale` 自报计数与内核实时值不一致**：测试自身读到的
  `pt_lock_acquires=0 / cursors=3`，而同一次运行中 `cat /proc/a20/perf` 的实时值
  为 `mm_pt_lock_acquires: 23 / mm_cursor_open: 11`。计数器本身工作正常
  （名字在 `core/perf.c` 注册无误、无编译期开关），故差异出在测试的读取时机或
  解析上，其计数器输出目前不可作为归因依据；应以 `/proc/a20/perf` 实时值为准。

### 8.15 当前基线（无 NIC，x86_64/KVM smp4）

`mm_pt_scale` 多次采样 `ideal_speedup` 落在 1.06x–1.20x，低于其自带的
1.80x 判定阈值（FAIL）。这与 §8.8 的结论一致：`mm->lock` 仍在 fault 路径上
串行化 16KiB 清零，并行度尚未兑现。**这是 P5 尚未完成的量化证据。**

### 8.16 P5 第六次尝试：**就地**改造同样失败（推翻 §8.12 的建议）

§8.12 判定「前置插队」是第五次失败的根因，并建议改为**就地**改造：把锁外 prepare
放进 `handle_demand_fault_locked` 内、原 anon batch 所在的精确位置，从而完整保留
前面 swap→栈→brk→VMA→权限→file→VMO→THP 的判定顺序，只改变「工作在锁内还是
锁外」。**该建议已实施并同样失败**，故在此更正。

实施要点：给 `handle_demand_fault_locked` 增加 `lock_held` 参数，因为三个调用点的
锁状态不同：NOMMU 路径（不持锁）、swap 重试路径（已刻意放锁）、主路径（持锁）。
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
`spin_lock_irqsave(&mm->lock)`）在另一个正在 fault 的 CPU 上与本进程的
`mm->lock` 发生互等。下一步应先用插桩确认卡死时各 CPU 的持锁栈（而非继续猜测），
再决定 P5 的形态。

结论：**P5 仍未完成**，当前内核保持「正确但 fault 串行」的状态。以六次失败的
共同点为线索、用持锁栈插定位，是比继续改 fault 代码更可靠的下一步。

### 8.17 P5 根因：VMA 引用计数初始化遗漏（**六次失败的真实原因**）

§8.16 建议「插桩定位卡死时各 CPU 的持锁栈」。实际没有走到持锁栈，因为**根本不是
锁死**。给 `mm_find_vma` 的链表回退路径加步数上界后立即定位：

```
[VMAWALK] CYCLE mm=0xffffffc0bf1d0010 addr=0x6e69746c69756000 steps=100001
mm_find_vma: VMA list cycle
```

`addr=0x6e69746c69756000` 是 ASCII 文本而非合法地址，遍历进的是被释放后
reused 的内存。**VMA 链表成环**。

根因是 `b10b267b` 的一个真实缺陷：当时用 `grep sizeof(vm_area_t)` 找分配点，
漏掉了 4 处用 `sizeof(*vma)` 写法的站点：`munmap.c:262`、`udriver.c:183`、
`framebuffer.c:181` 与 `:363`。这些 VMA 的引用计数停留在 `kcalloc` 的 0。

该缺陷在 `b10b267b` 中**潜伏**：没有代码调用 `vma_get/vma_put`，计数为 0 只会让
`munmap` 少释放一次（泄漏），不会出错。P5 让 fault 路径真的
`vma_get` → `vma_put` 后，计数走 0→1→0，VMA 在**仍挂在 `mm->mmap` 上**时被释放，
链表指针随即悬垂。

触发点 `munmap.c:262` 建的正是 `VM_ANON | VM_READ | VM_WRITE` 的 brk VMA，恰好
落在 P5 快路径匹配的类别里；`execve` 会同时走「拆旧镜像 + 建 brk VMA + 首次
写入」，所以每次都稳定命中。

这解释了第五、六次以及此前所有失败的共同症状。前面记录的那些根因
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
| `ideal_speedup` | 1.06x–1.20x | 1.01x / 1.12x / 1.12x |

与基线无差异，仍远低于 1.80x 阈值。

瓶颈**不在 `mm->lock`**。锁外 prepare 之后，缺页路径仍依次经过两个全局
串行点：

* `cg_mem_charge()` → `spin_lock_irqsave(&node->lock)`：per-cgroup-node 锁，
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

实验一：把 cgroup 计费批量化。`cg_mem_charge(cg, nr_pages)` 本就接受页数，
于是把 fault-around 窗口的逐页 charge 改为整窗一次（每窗 1 次 `node->lock` 而非
4 次），所有回滚路径改为按实际丢弃量一次性 uncharge。

结果（x86_64/KVM smp4、无 NIC、10 次采样，指标取 4T 墙钟）：

| | 4T 中位数 | 4T 全距 |
|---|---|---|
| 改动前 | 0.2128s | 0.2127–0.2377 |
| 批量化后 | 0.2142s | 0.1929–0.2625 |

**无差异**，完全落在噪声内。已回滚（内存计费正确性关键，批量化后有 4 处 uncharge
站点，多一处错就是泄漏或限流失效，而收益无法证明）。

**实验二：把 fault-around 窗口从 4 页降到 1 页**，用于判断清零（memset）是不是主项。
两者清零的总字节数完全相同，故若 memset 主导则耗时应接近。实测 4T 中位数
0.326s（窗口=1） vs 0.214s（窗口=4）：窗口=4 快约 1.5x。说明**主导项是每次
缺页的固定开销，不是清零带宽**；且 fault-around 的摊薄本身已是约 1.5x 的既有收益。

顺带修正测量方法。`mm_pt_scale` 的 `ideal_speedup = 4×1T/4T`，而 1T 自身在
0.0603–0.0845 之间波动（±17%），直接被放大进比值。**1.63x / 1.40x 那两个读数是
1T 基线偏慢造成的假象**，不是 4T 变快。真实极差：4T 的 ±16%，比 §8.8 记录的
±9% 噪声地板更差。今后判断此类改动必须直接比较 4T 墙钟中位数，不能引用该比值。

**结论**：缺页路径上，`mm->lock`、cgroup 计费锁、memset 带宽均非绑定约束。
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

而 `g_a20_perf_enabled` **只由 `a20_perf_format()` 设置**，也就是只有读取
`/proc/a20/perf` 才会打开。注释写明这是为了「formal timed builds 不被计数污染」。

后果：**先跑基准、后读计数，整个基准的计数全部被丢弃。** 这也解释了 §8.14 里当时
记为「测试自报计数与实时值不一致」的现象：不是测试解析错，而是它自己在基准跑完后
才第一次打开计数，读到的必然接近 0。

正确用法是**基准前先读一次 `/proc/a20/perf` 预热，基准后再读，差值才是基准的**。
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
| `mm_pt_lock_acquires` / `contended` | 131179 / 0（0%） |
| `mm_pfa_lock_acquires` / `contended` | 16849 / 53（0.3%） |
| `mm_cg_lock_acquires` / `contended` | 266 / 108 |

结论与 §8.18/§8.19 的猜测相反：**一把锁都不是瓶颈。**

* 页表锁 13 万次获取、0 次争用。覆盖式页表节点锁在不相交地址上完全不冲突，
  这正是单级模型想要的效果，它已经在正常工作。
* `pfa.lock` 争用 0.3%，per-CPU batch（32 页/次）已把它摊薄到可忽略。
* `cg_lock` 只有 266 次获取对应 13 万次缺页，因为 `cg_mem_charge` 开头
  `if (!cg) return 0;`：该测试进程没有 cgroup，计费锁根本不在这条路径上。
  （那 108 次争用全部来自引导期的其他进程。）

因此匿名缺页每次约 1.5µs 的成本**不是锁等待**，而是真实工作：4 页清零、页表下降、
cursor 事务、本地 TLB 刷新、`mm->rss` 更新。§8.19 的窗口实验（4 页 vs 1 页差 1.5x）
已经证明固定开销主导；现在争用数据补上了另一半证据：固定开销里没有锁。

若还要继续提升 fault 并行度，可动的只剩**减少每次缺页的固定开销**（例如更大的
fault-around 窗口、把 `rss`/TLB 刷新批量化），而不是继续摘锁。这也意味着
「单级内存模型带来 fault 并行度」这一目标在本负载上无法靠锁改造达成。

### 8.22 扩大 fault-around 窗口：无收益，该方向已到头

§8.21 证明每次缺页的固定开销里没有锁，而 §8.19 的窗口实验（1 页 0.326s vs
4 页 0.214s）显示固定开销仍有分量。据此把窗口从 4 页扩到 8 页：若固定开销
主导，缺页次数减半应当更快。

实测（x86_64/KVM smp4、无 NIC、5 次采样，取 4T 墙钟）：

| 窗口 | 4T 中位数 | 4T 全距 |
|---|---|---|
| 4 页 | 0.2142s（10 次采样） | 0.1929–0.2625 |
| 8 页 | 0.2169s | 0.1936–0.2340 |

**无差异**，落在噪声内。已回退到 4 页。

结论：fault-around 的摊薄在 4 页处**已经饱和**。1 页→4 页能省 1.5x（少做 3/4 的
固定开销），4 页→8 页则完全没有进一步收益，说明固定开销的绝对量已经小于每次
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

* 每个 PT 页一个页描述符，按 PFN 索引，挂在常驻的 per-frame 数组上。这正是论文
  §3.3 的「descriptor indexed by the physical page number」。
* per-PTE 元数据数组，存状态（invalid / 虚拟已分配未映射 / 已映射 / swapped）
  与附加状态（权限、COW 位），见 §4.3 的 Table 2。
* 每个 PT 页一把 MCS 自旋锁，存在描述符里。§4.5 的原话是「The lock of each PT
  page is stored in the corresponding page descriptor」。
* 事务式 cursor（`mm_addrspace_lock` + `mm_cursor_{query,map,mark,unmap}`），
  对应 §3.3 的 transactional interface 与 Figure 4。
* 覆盖节点锁 + stale 标记 + `pt_readers` 读侧计数。

所以 §8.22 那句「没抓住关键」需要精确化：数据结构层面对了，协议层面没对。

### 9.2 真正缺的东西（论文的核心主张）

1. fault 决策完全不查 VMA。论文 Figure 8 的缺页处理只做
   `rcursor.query(faulting_addr)`，返回 `Status::PrivateAnon(perm)` /
   `Status::Mapped(page, perm)` / `Invalid`，然后直接 `rcursor.map(...)`。
   整段在事务内原子完成（L18–L40），从头到尾没有 VMA。论文明确把 Linux 的劣势
   归因于 "the time Linux spends in the VMA"（§6.2）。我们的 fault 至今仍以
   `mm_find_vma()` 为决策入口，**这是最关键的缺失**。
2. 整个 fault 在一个事务内原子完成。我们是「锁外 prepare → 取回 `mm->lock`
   → 复核 → 装入」的多阶段结构（§8.12），论文是一个 RCursor 走到底。
3. CortenMMadv 的无锁下降 + DFS 锁全部后代。论文 §4.1 Figure 6：遍历阶段
   **不加任何锁**（在 RCU 读侧临界区内），再加锁阶段锁住覆盖 PT 页
   **及其所有后代**。我们只有覆盖节点一把锁。
4. RCU monitor 真正接线。论文：unmap PT 页时先原子清父 PTE，再把 PT 页挂进
   全局 RCU monitor，读侧临界区退出后才释放；被锁到 stale PT 页的线程
   **重新下降重试**。我们的 `mm_pt_defer_free` **零调用者**（§8.13），这条链
   完全没接。
5. **per-core 虚拟地址分配器**（§4.5 优化项）。
6. **惰性 TLB shootdown（LATR）+ 早期 ack**（§4.5）。
7. **shared 位**（§4.3：每个虚拟页两bit，shared + writable，用于 fork COW）。
   我们只有 `MM_ST_COW_BIT`，没有 shared 位。

### 9.3 测量能力是当前的硬约束（已补上）

在此之前无法判断任何改动，因为 `mm_pt_scale` 只覆盖论文五个微基准里的 PF 一个。
曾新增 `user/cmds/stress/cortenmm_bench.c`（已删除），实现论文 §6.2 的五个基准
（mmap / mmap-PF / PF / unmap-virt / unmap），每个都有 low-contention 与
high-contention 两个变体，并遵循 §8.20 的计数预热要求。

riscv64/TCG 基线（4 线程，ROUNDS=256，仅作后续对照的相对基线）：

| 基准 | low scaling | high scaling | 备注 |
|---|---|---|---|
| mmap | 1.29x | 1.33x | |
| mmap-PF | 1.09x | 0.96x | |
| PF | 1.60x | 30.13x | 30x 是 1T 窗口太短造成的噪声，不可信 |
| unmap-virt | 0.98x | 1.42x | |
| unmap | 1.97x | 1.08x | |

TCG 单核慢、计时窗口偏短，比例只能作相对参考。论文的 33x–2270x 是 384 核
数据，本机 4 核不可能复现；可信的对照结论要等 P6 落地后在同一环境重测。

### 9.4 顺带发现的一个无关既有缺陷

`ARCH=x86_64 ABI=linux dev-build` 产出的 FAT32 镜像**开不了机**：
`[INIT] Cannot open /bin/init: -2`，回退 `/init` 也失败，最终
`panic("init: no init program found")`。该镜像没有 `/bin` 目录（所以
`/bin/init` 必然失败），而 `/init` 回退也读不出来。
移除 `cortenmm_bench.c` 后重新构建仍然复现，与本次改动无关。
仓库的 x86_64 smoke 门走 `tools/a20 test`（另一套镜像），不受影响；
所有 riscv64 门正常。这是 x86_64 `dev-build` 路径的既有缺陷，另行记录。

### 9.5 下一步（按论文优先级）

1. fault 改为**只依据 per-PTE 状态**决策，去掉 VMA 依赖。论文的核心，第一优先。
2. 整个 fault 收进**单个事务**。
3. 接线 RCU monitor：PT 页延迟释放 + stale 重下降。
4. CortenMMadv 协议：无锁遍历 + DFS 锁后代。
5. 之后才是 per-core VA 分配器与 LATR。

### 9.6 下一阶段的关键发现：`MM_ST_ANON_VIRT` 从未被写入

要按论文让 fault 不查 VMA，前提是「这个虚拟页已保留、只是还没 backing」这一状态
**存在于 per-PTE 元数据里**。核对发现：

* `MM_ST_ANON_VIRT`（= 论文的 `Status::PrivateAnon`）在全树只有一处出现，
  而且只是审计代码里的白名单判断（`pt.c:887`）。**没有任何地方写入它。**
* `mmap` 路径（`mm_mmap_locked` / `mm_mmap_file_locked` / `mm_mmap_vmo_locked`）
  **完全不写 per-PTE 状态**，只做 `mm_insert_vma()`。

所以今天 per-PTE 状态里根本没有「已保留」这个概念，fault 只能靠
`mm_find_vma()` 才知道这个地址该不该有映射。**这正是 §9.2 第 1 条无法直接实现的
根本原因**，也是我们与论文差距的最小可操作切入点。

审计代码早就为它准备好了位置：`mm_pt_audit_addrspace` 对「PTE 缺失」
的叶节点允许 `MM_ST_ANON_VIRT` 与 `MM_ST_SWAPPED` 两种 class（`pt.c:885-887`），
即设计上**本来就预期** on-demand paging 会写入 `MM_ST_ANON_VIRT`。只是这一层从未接上。

因此 P6 的正确施工顺序是：

1. mmap 时按需预标记：为匿名保留范围把 per-PTE 状态置为
   `MM_ST_ANON_VIRT`（并带权限位）。不能为整个范围预分配 PT 页，那会把
   稀疏映射变成稠密，抵消 fault-around 的收益；需要「惰性建立中间节点」或在
   fault 时补写。这是本阶段最需要小心的取舍。
2. fault 改为只查 cursor 状态：`PrivateAnon` → 分配零页并 map；
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
* `01673960` mprotect 同步刷新「已保留未缺页」叶子的权限状态；这是前置条件：该状态
  一旦成为 fault 唯一的权限来源，mprotect 不同步就等于权限绕过。
* `15535e04` 缺页按 per-PTE 状态决策，不查 VMA（论文 Fig. 8 的核心）。
* `fb26bf47` 预标记扩展到 ELF 匿名段与 brk。

机制已被证实有效：riscv64 上 mm_stress 期间
`mm_anon_provisioned=18660`、`mm_fault_from_status=479`，即 1482 次 demand fault 中
有 479 次（32%）完全不经 `mm_find_vma`。这正是论文所称「省掉 Linux 花在 VMA 上的
时间」（§6.2）那部分收益的载体。

### 10.1 但首轮实测是**回归**

同一环境（riscv64/TCG，4 线程）对比 §9.3 基线，取 4T 的 ns_per_op（比值指标因 1T
窗口抖动不可用，见 §8.19）：

| 基准 | 基线 | 预标记后 | 变化 |
|---|---|---|---|
| mmap | 52–57 µs | 81–82 µs | 慢 1.5× |
| mmap-PF | 156–171 µs | 184–239 µs | 慢 1.2–1.4× |
| unmap | 134–173 µs | 204–208 µs | 慢 1.2–1.5× |
| unmap-virt | 40–44 µs | 66–68 µs | 慢 1.6× |

原因是预标记的实现代价，不是设计本身错了。`mm_cursor_mark_prot()` 读的
`cur->path[0]` 只有在覆盖层为 0 时才被 `mm_addrspace_lock()` 的下降循环填好，
所以当前实现每页都要开一次 cursor、加锁、标记、解锁。一个 2 MiB 映射就是 512 次
完整的 cursor 往返。论文是对整个区间一次加锁的，我们没有做到，代价就落在 mmap 侧。
这恰好是论文承认会变慢的那一项（§6.2「mmap 比 Linux 略慢」），只是我们的实现
把它放大到了 1.5×。

下一步必须先优化预标记的锁粒度（一次锁住一个 leaf table 的范围，或让
`cursor_leaf_table()` 在覆盖层 > 0 时也能定位叶子表），否则这笔开销吃掉了 fault 侧
的收益。在 TCG 上尤其明显；即便如此，也不能以「反正测不出来」为由保留当前形态。

### 10.2 顺带修正基准自身的一个缺陷

`cortenmm_bench` 的 PF 相位在同一个 512 MiB 池上反复触碰，round 0 之后页面已
是热的，测到的是 memcpy 而非缺页。基线那组 `PF 1T=0.0014s`（约 93M 页/秒）测到的
正是这个假象，物理上不可能是真实缺页。PF 相位必须每轮用全新映射才有意义（与
`mm_pt_scale` 注释里「复用热页只会测到 memcpy」的告诫一致）。此项待修，当前所有
PF 数字（含本节的 479/1482）只反映「走了状态路径的缺页数」，不能当作缺页吞吐。

### 10.3 两次「优化」都没能解决，其中一次是在追噪声

针对 §10.1 的回归做了两轮修改：

1. 锁粒度：`mm_pt_provision_anon()` 由「每页一次 cursor」改为「每个 leaf table 一次
   cursor」。单页区间会让 `mm_addrspace_lock()` 下降到覆盖层 0，`cur->path[0]` 就是叶子
   表，而它拿到的锁正是该叶子表自己的锁，所以整表 512 项都已被排他。语义与论文
   「对覆盖 PT 页加一次锁」一致。
2. 元数据与计数器：`mm_pt_note_present()` 每页都要做一次 `mm_pt_meta()`，即
   `virt_to_pfn` → frame flag 检查 → `.pt` 解引用三次依赖式加载；再叠加一次原子
   read-modify-write 计数。改为把元数据解析提到循环外（新增 `pt_note_present_meta()`
   供批量调用者复用），计数器改为整段一次 `a20_perf_add()`。

三轮实测的 mmap 4T ns_per_op：基线 52–57 µs → 每页锁 81–82 µs → 每表锁 79–89 µs
→ 提元数据 90–97 µs。

基线与「带预标记」之间的差距（约 1.5×）远高于噪声，是真实的。可我这三版之间的差异
完全落在运行间抖动内，分辨不出来：第 2 项改动既没有被证实有效，也没有被证实有害，
第 1 项同理。既然分辨不了，就不能宣称优化成功，也不应继续按 TCG 的噪声调参。继续
调下去只是在拟合噪声。

余下的开销不在锁、也不在元数据查找，而在 PT 页本身的分配/释放往返（pfa 分配、
cgroup charge、4 KiB 清零、`pt_meta` 初始化，unmap 时再同步释放）。注意
`mm_pt_defer_free()` 目前仍无调用者，页表帧是同步回收的。这与论文自身的取舍一致：
论文 §6.2 明确说它的 mmap 比 Linux 慢，因为 on-demand paging 必须在 mmap 时就把
per-PTE 状态写好；而状态存在 PT 页元数据里，参考实现把它编进 PTE 软件位，写状态就
必须先有 PT 页。这笔开销是该设计的固有成本，不是本实现的实现缺陷。

平台限制：TCG + 4 线程 + 共享宿主下，run-to-run 方差可达 ±30%，无法分辨 1.2×
以内的差异。真正的性能结论必须在 x86_64/KVM 上取，而本仓库的
`ARCH=x86_64 ABI=linux dev-build` 镜像存在既有启动故障（`Cannot open /bin/init: -2`），
需要先修镜像或另辟 x86 测量路径。

教训：本轮连续两次「优化」都是先改后测，且都在同一批次噪声里下结论。已经改为
先记录方差、再判断改动是否超出方差。

### 10.4 基准修正后：缺页成本首次可信，以及一个仍然存在的空洞

修正两处（`user/cmds/stress/cortenmm_bench.c`，已删除）：

* B_PF 改用 `MADV_DONTNEED` 再触碰。`mm_madvise_dontneed()` 只校验 VMA 覆盖
  范围、然后丢掉 PTE，VMA 本身保留，所以每轮都拿到「VMA 在、PTE 全空」的状态；
  mmap 开销被排除在测量之外，而每一页都是冷缺页。原先复用固定 slice，第 0 轮之后
  测的全是 memcpy。
* B_UNMAP_VIRT 补上触碰。原实现从不触碰，被 unmap 的洞里根本没有页，测到的是
  对未使用区间的 VMA 切分，而不是「拆掉一个已填充区间」；注释里「下一轮仍会缺页」
  当时是假的。

修正后 3 次采样（4T ns_per_op，中位数 / 离散度）：

| 基准 | 中位数 | 离散度 |
|---|---|---|
| mmap low / high | 83.0 / 87.2 µs | 5% / 7% |
| mmap-PF low / high | 187.8 / 191.7 µs | 9% / 16% |
| PF low / high | 86.8 / 94.7 µs | 4% / 19% |
| unmap-virt low / high | 68.5 / 69.0 µs | 10% / 10% |
| unmap low / high | 185.0 / 214.4 µs | 21% / 5% |

PF 现在是 ~170 ns/次缺页（512 页 ÷ 86.8 µs），这个量级合理。

修正前的 PF 数字（30–36 µs/op）测的是热页 memcpy，不是缺页，所以 §10.1 那几轮
「改动 vs 改动」的对比里，PF 一栏自始至终是无效的。至今没有一次有效的
「按状态缺页路径 vs 走 VMA 缺页路径」的开销对比：旧的一侧从未被正确测过。§10.1
唯一成立的结论只有 mmap 那条，带预标记比不带贵约 1.5×，且该差距远大于 5–10% 的
离散度。

因此当前能说的只有：机制正确（479/1482 次缺页完全不经 VMA，且关机审计全 0），
代价已知（mmap 约 1.5×，与论文自述的 mmap 更慢同向），性能收益未测得。要拿到
收益结论，必须在 x86_64/KVM 上用修正后的基准做 A/B，这依赖先解决 §10.3 提到的
x86 镜像启动故障。

采样纪律（按 §10.3 的教训固定下来）：每个配置至少 3 次采样，先看中位数与离散度，
离散度内（<15%）的比较一律不作为结论。


### 10.5 接线 RCU 的生产端（**已回退，勿信**）

> 本节的代码改动已在 `db91e679` 之后回退。下文对 use-after-free 的分析仍然成立
> （PT 页确实是被同步释放的，而 `mm_addrspace_lock()` 只锁覆盖节点），但当时给出的修法
> 不安全：它把 `pt_unmap()` / `pt_unmap_leaf()` 的首参从 `pgdir` 改成 `mm`，并在这两个
> 函数内部调用 `mm_pt_defer_free()`。而 8 个调用点中有 6 个既不持 `mm->lock`、也不在
> `mm_tlb_invalidate_begin()/finish()` 事务内（`vma.c` 全文没有任何
> `mm_tlb_invalidate_*`；`sys_mm.c` 的调用点在第 281 行，而 `finish` 在第 210 行，
> 已在事务之外）。`mm_pt_hold_table()` 会无锁地改写 `mm->tlb_holds` 链表，且没有事务
> 就永远没有人来排空它；既是链表竞争，也是 PT 页泄漏。
>
> 正确修法必须先给这些调用点补齐 `mm->lock` + 事务边界（或改用一个不需要事务的
> 独立回收队列），再谈接生产端。在此之前**不要重新套用该补丁**。

核对后发现此前的判断需要更正一半。读侧其实早就接好了：`mm_addrspace_lock()` 在
下降前 `mm_pt_read_enter(mm)` 并置 `cur->in_read_side`，`mm_cursor_unlock()` 退出，
另有 8 条错误返回路径各自配对退出。`vm.c` 里 `mm_tlb_invalidate_finish()` 的排空循环
也在等 `mm->pt_readers` 归零，并已经会释放 `MM_TLB_HOLD_PT`。

真正缺的是生产端，后果是一个真实的 use-after-free：

* `mm_pt_defer_free()` 与 `mm_pt_mark_stale_recursive()` 都没有任何调用者。
* 于是 `pt_unmap()` / `pt_unmap_leaf()` 的拆表循环一直走的是
  `mm_pt_node_fini(child); frame_free(child);`，同步释放。
* 但 `mm_addrspace_lock()` 只锁覆盖节点，后代节点是无锁遍历的。所以另一个 CPU 上
  正沿着 `child` 下降的 cursor，可能在该帧被 buddy 回收后继续读它。
* 排空循环等的是一个永远不会被填充的链表，所谓宽限期实际上保护不了任何东西。

修法就是接上生产端：拆表时先 `mm_pt_mark_stale_recursive()`（让缓存了该节点的
cursor 观察到 `stale` 并重下降，这本来就是 `mm_addrspace_lock()` 里那个
`attempt < 8` 重试循环要处理的情形），再 `mm_pt_defer_free()` 把帧交给 shootdown
排空；只有入队失败（ENOMEM）才退回原来的同步释放，避免漏帧。

为此把 `pt_unmap()` / `pt_unmap_leaf()` 的首参从 `pgdir` 改成 `mm`。排空队列挂在
`mm->tlb_holds` 上，没有 `mm` 无法入队。所有调用点本来就已经持有 `mm`（它们原先正是
用 `mm->pgdir` 取的 pgdir），故为机械改动；`signal.c` 里传的是 `t->pgdir`（`t` 是
task），改为 `t->mm`。`mm.h` 补 `struct mm_struct;` 前向声明，NOMMU stub 同步改签名。

安全性前提已核对：munmap/mremap/madvise 的拆表点都在 `mm_tlb_invalidate_begin()` …
`mm_tlb_invalidate_finish()` 事务内（`mm_munmap` 在 287 行开、291 行关，中间调用
`mm_munmap_locked`），所以入队的帧一定会被排空，不会残留。

验证：riscv64 / aarch64 / loongarch64 / ppc64le / x86_64 与 riscv64/aarch64 的 NOMMU
构建通过；`smoke-mm-stress`、`smoke-mm-fork-exec-race`、`check-mm-lock-model` 通过，关机审计
missing_meta/present/absent/prot/cow/vma 全 0，且 `pt_pages=6` 与改动前一致。若帧入队
后未被排空，这个计数会升高，这是排空确实生效的证据。

**未能验证**：arm32 在本环境改动前就无法构建（`arm-linux-gnueabihf-gcc` 不在
PATH），而 arm32 有自己一份 `pt_unmap`/`pt_unmap_leaf` 实现（已同步改签名并从
`mm->pgdir` 取 pgdir），但编译未经检验。属既有环境限制。

### 10.6 撤回 §10 的核心数据；状态缺页路径至今是死代码

先撤回一个错误结论。§10 与 §10.5 反复引用的「`mm_fault_from_status=479`，即 1482 次
demand fault 中 479 次（32%）完全不经 `mm_find_vma`」**是错的**。在记录该数字的确切
提交 `fb26bf47` 上重新测量，该计数为 0；同期真正增长的是 `mm_anon_faults=679`，
即走 VMA 的那条路径。这条结论作废，§10 里所有依赖它的表述一并失效。

读代码就能证。`mm_cursor_query()` 对缺失的 PTE 直接 `return 0` 而不写
`*cls_out`，于是 `cls_out` 保持调用方给的 `MM_ST_CLS_BYTE(MM_ST_INVALID)`。而 fault
路径的判据是 `!already && MM_ST_GET_CLASS(cls_byte) == MM_ST_ANON_VIRT`，
`INVALID != ANON_VIRT`，因此永不成立。这段逻辑自 `ff75efde`（P2）起就是这样，
也就是说论文 Fig. 8 的「按状态决策的缺页」路径**从来没有执行过一次**。

尝试把它激活后，暴露出两个潜在缺陷：

(a) 文件映射被误判为匿名（本次已修）。`pte_to_vm_flags()` 给每一个基于文件的
段都打上 `VM_ANON`，所以私有 RELRO 段（`flags=0x13`、`file_fd=6`，而 `VM_FILE`
位并没有设置，因为 `file_fd` 是 `elf_add_vma()` 返回之后才赋值）与 `.bss` 根本无法区分。
`fb26bf47` 的预标记条件 `(vm_flags & VM_ANON) && !(vm_flags & VM_SHARED)` 于是把 mksh 的
RELRO 标成了 `MM_ST_ANON_VIRT`，缺页时返回匿名零页而不是文件内容：
`FATAL: pid=4 signal=11 comm=mksh`，`stval=0x6a170`，`page_words` 全 0。

修法是给 `elf_add_vma()` 增加显式 `bool anon` 形参，由调用点逐个声明：文件段装载点
（逐页预载、`.bss` 尾段、exec 映像）传 `false`，栈/TLS/堆传 `true`。在 `elf_add_vma()`
内部靠 `vm_flags` 推断不可能正确，因为此时 `file_fd` 尚未赋值。

(b) brk/堆区间仍然崩溃（未定位）。修掉 (a) 之后失败点前移到堆：
`stval=0x1f20000`，落在 brk VMA `[0x1f20000,0x1f21000) flags=0x13 pte_flags=0xd7
file_fd=-1 off=0x1000`，该页已映射但全 0。**根因未定位**，故状态路径保持不启用。

结论与当前状态：论文核心的状态驱动缺页路径目前**不可用**，且已证明至少存在两个
潜在缺陷。在 (b) 定位并修复之前不应启用，否则 exec 与堆会直接崩。HEAD 刻意让该路径
保持 inert（`mm_fault_from_status` 恒为 0），全部功能门通过。性能结论不受影响：
§10.1 那个 mmap ~1.5× 回归是预标记在 mmap 侧就要付的代价，与该路径是否真的执行无关。

### 10.7 状态路径的位置本身是设计缺陷（与 (b) 无关的独立问题）

排查 (b) 时发现一个更根本的问题：状态块（`fault.c` 958–996）排在
`vm_area_t *vma = mm_find_vma(mm, page_va);`（998 行）前面。一旦启用，它在完全不查
VMA 的情况下直接映射并返回，后面所有基于 VMA 的判定全被绕开：

* 绕过 userfaultfd（1007–1008）：已注册 userfaultfd 的区间本应把缺页停住交给
  handler，状态路径却会直接给出零页，handler 永远收不到事件。
* 绕过 VM_SHARED 判定（1021）：共享映射的写缺页需要 COW/共享语义，不能按私有
  匿名处理。
* 绕过 fault-around 的安全门（522）：那段逻辑明确要求
  `!(vma->vm_flags & (VM_SHARED | VM_STACK | VM_FILE | VM_VMO))` 才做 4 页
  fault-around；状态路径没有任何等价门限。
* 与 brk 路径的前提冲突：`fault.c:355` 的 brk 处理条件是
  `page_va >= start_brk && page_va < ROUND_UP(brk) && !mm_find_vma(...)`，它只
  处理「没有 VMA」的地址；而状态路径恰恰是「有状态标记但可能没有 VMA」的产生者。

论文的设想是 per-PTE 状态取代 VMA 作为权威来源。那要求状态本身携带全部安全信息
（是否 userfaultfd 注册、是否共享、是否可 fault-around），并且 mmap/mprotect/munmap/
userfaultfd 注册每一次都同步更新它。本实现的状态字节只有 class + prot 三位，
不含这些信息，所以「在 `mm_find_vma` 之前无条件信任状态」是不成立的。

因此正确顺序应是：状态路径不是放在 VMA 查找之前的旁路，而是要把 VMA 查找替换掉，
前提是状态已扩充到足以承载上述全部判定，并且所有会改变安全语义的 syscall 都已接入
状态更新。缺一不可。当前两者都不满足，保持 inert 是正确取舍。

解这个位置问题要按下面的顺序来：

1. 先定状态字节的完整语义：至少需要 shared、userfaultfd-registered、seal/fault-around
   许可三类位，并明确谁负责写、谁负责清。
2. 让 `mmmap` / `mprotect` / `munmap` / `madvise` / userfaultfd 注册全部接入该状态的
   更新，且每条路径都有「状态与 VMA 不一致」的自检（挂到现有 MM-ASM auditor 上）。
3. 只有 1、2 完成且 auditor 能证明等价后，才把状态路径移到 `mm_find_vma` 之前。
   在此之前 (b) 的 brk 崩溃只是表象，即使修好 (b) 也会立刻撞上 userfaultfd 绕过。

### 10.8 同树 A/B：预标记不是 mmap 回归的原因，推翻 §10.1 的归因

§10.1 把 mmap ~1.5× 的回归归因于「预标记的实现代价」，并据此做了两轮优化
（§10.3）。但从未做过同树 A/B：所有对比都是「改动前的老提交」对「改动后的新提交」，
中间混进了别的变化。在当前 HEAD 上把 `MM_ANON_PROVISION_MAX_PAGES` 从 4096 改成 0
（即完全关闭预标记），其余一切不动，重测 3 次（4T ns_per_op 中位数）：

| 基准 | 预标记开 | 预标记关 | 关相对开 |
|---|---|---|---|
| mmap low | 83.0 µs | 85.3 µs | +3%（更慢） |
| mmap high | 87.2 µs | 88.6 µs | +2%（更慢） |
| mmap-PF low | 187.8 µs | 245.3 µs | +31%（更慢） |
| PF low | 86.8 µs | 111.4 µs | +28%（更慢） |
| unmap-virt high | 69.0 µs | 69.1 µs | 0% |
| unmap low | 185.0 µs | 162.5 µs | −12%（更快） |

§10.1 的归因到此推翻。关掉预标记后 mmap 没有变快，两��相差 2–3%，远在噪声内。
所以「预标记导致 mmap 慢 1.5×」这个归因是错的，§10.1 与据此展开的 §10.3 两轮优化
都建立在错误前提上。真实原因尚未定位，最可能是 §9.3 那条基线与当前树之间还有别的
未识别差异（基线取自加预标记之前，其间 §10 之后我又动过 `munmap.c`/`elf.c` 的调用点
与 fault 路径周边）。因此「1.5×」本身也需要重新用同树 A/B 复核才能成立。

与直觉相反的一条：关掉预标记让 PF 与 mmap-PF 明显变慢（+28%/+31%）。合理解释是
预标记在 mmap 时就把 PT 路径建好，后续缺页的下降不需要再分配中间节点；关掉后每次缺页
都要重新走分配。这反过来说明预标记对缺页侧确有价值，只是它没有体现在 §10.1 关心的
mmap 指标上。

§10.3 定的测量纪律再次被验证：本轮 mmap-PF 的采样离散度高达 45%（193–281 µs），
远超 §10.3 定的 15% 门槛。TCG + 4 线程 + 共享宿主下，除 mmap-PF/PF 外的所有对比都
不可分辨。在拿到 x86_64/KVM 之前，任何小于 ~1.5× 的性能结论都不成立。

### 10.9 x86_64 dev 镜像启动失败的完整根因（不是镜像问题，是 W^X 缺页 bug）

此前把 x86_64 记为「dev 镜像 FAT32 启动失败，`Cannot open /bin/init: -2`」。这个描述是
错的：`/bin/init` 并不是缺失，而是根本没有块设备。完整链条（实测，非推断）：

```
[INIT] WARNING: no FAT32 device for /bin          <- mount_setup.c:348
[ERR] [DRVMOD] virtio-blk.a20drv: cannot mark module text executable
[DRIVERMGR] /boot/drivers/virtio-blk.a20drv: module load failed (-12)
[ERR] [DRVMOD] virtio-scsi.a20drv: ... 同上
[ERR] [DRVMOD] ahci.a20drv:      ... 同上
[INIT] Cannot open /bin/init: -2
KERNEL PANIC: init: no init program found
```

即 virtio-blk / virtio-scsi / ahci 三个块驱动全部加载失败，所以扫不到 FAT32 设备，
`mount_block_devices()`（`kernel/fs/mount_setup.c:310`）无法把 FAT32 挂到 `/bin`，于是
`/bin/init` ENOENT。`mdir` 显示镜像里 `init` 在根目录，根本没有 `/bin` 目录。
riscv64 能跑起来是因为它走的是另一条路径，两个镜像的目录结构其实一样。

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
（同文件 :64），后者对每一页调用 `x86_kwx_split_pmd(va)`，而该函数返回 NULL 就是 -ENOMEM。
`x86_kwx_split_pmd()` 只有两条路返回 NULL：

1. `x86_kwx_pd(va)` 返回 NULL，即 `boot_pdpt_hh[slot]` 不是 `PTE_V`，或仍是 1 GiB 大页
   (`PTE_PS`) 未降级。函数自己的注释就写了前提「对应 1 GiB 槽位须已降级」，而降级只由
   `arch_kernel_wx_finalize()` 对 `covers_ram` 的槽位做。
2. `if (!(e & PTE_V)) return NULL;`，即该 2 MiB 的 PD slot 压根不存在。它对
   「slot 是 2 MiB 大页」是能处理的（会拆分），唯独对「slot 不存在」直接放弃。

模块页来自 `pfa`，物理地址可能落在直映射尚未建立 PD slot 的区域，于是走到第 2 条。
尚未确定实际命中的是第 1 条还是第 2 条，这需要插桩或对照 `boot_pdpt_hh` 实测，
不能在证据不足时直接改引导页表。可能的修法方向（待验证后再动）是把「slot 不存在」
也当作需要新建的映射：按 `arch_pt_vpn(va,2)<<30 | arch_pt_vpn(va,1)<<21` 算出该 2 MiB
的物理基址，分配 PT 并填 512 个 R|W leaf，而不是直接失败。

影响：这是 x86_64 上所有内核态驱动模块（含全部块驱动）都加载不了的前置 bug，
与 CortenMM 改动无关，属既有问题；但它挡住了唯一能分辨性能结论的平台（TCG 采样离散度
最高到 45%，见 §10.8），因此在修好之前 §10 的性能问题无法收口。

### 10.10 修好 x86_64 引导：两个独立故障，其中一个是真实内核 bug

§10.9 定位到失败点在 `x86_kwx_split_pmd()` 返回 NULL，但当时有两个候选分支。插桩实测
（两处 NULL 各加一条 `kerr`，事后已移除）给出确定答案：

```
[ERR] [kwx-diag] pd NULL va=ffff80013fb80000 slot=4 e=1000000e3 V=1 PS=1
[ERR] [kwx-diag] pd NULL va=ffff80013fb88000 slot=4 ...
（"pdslot-absent" 命中 0 次）
```

命中的是第 1 条：`boot_pdpt_hh[4]` 的 `V=1, PS=1`，1 GiB 槽位 4 仍是大页，
从未降级。`va=ffff80013fb80000` 对应物理 `0x13fb80000`（≈4.99 GiB），而启动日志报告
`[RAM] usable 0x100000000..0x140000000 (1024 MiB)`，即 4–5 GiB 是真实 RAM，pfa 分配
到这里完全正常，不是越界。

Bug 本体是 `arch_kernel_wx_finalize()` 写成了

```c
for (int slot = 0; slot < 4; slot++)      /* kernel/arch/x86_64/mm/kwx.c */
```

但直映射并不止 4 GiB。`firmware.c` 的 `X86_HIGH_RAM_MAP_END = 0x200000000`（8 GiB），
固件按整 1 GiB 块把 4 GiB 以上的可用内存映射进来。于是槽位 4–7 是真实 RAM，装着真实的
模块页，却因为循环只扫 0–3 而从未降级。后果是 `x86_kwx_pd()` 对这些地址返回 NULL →
`x86_kwx_split_pmd()` 返回 NULL → `arch_kwx_module_protect()` 返 -ENOMEM →
virtio-blk / virtio-scsi / ahci 三个块驱动全部加载失败（-12）→ 扫不到 FAT32 →
`/bin/init` ENOENT → PID 1 panic。x86_64 上所有内核态驱动模块都加载不了。

修法：把直映射范围常量提到共享头 `arch/x86_64/include/platform.h`（原先只在
`firmware.c` 里局部定义，提上来是为了避免与 `firmware.c` 漂移），循环上界改为
`X86_HIGH_RAM_MAP_END >> 30`，即覆盖直映射实际映射的每一个 1 GiB 槽位。槽位 0 含内核
映像、其降级路径（旁路建好新 PD 再单次写入替换 PDPT 项）原样保留。

第二个故障不是内核问题。修完驱动能加载后，仍报 `no FAT32 device for /bin`，那是我
自己的 QEMU 参数不对（用了默认 i440fx machine + `if=virtio`）。改用仓库自己
`tools/targets-steps.mk` 里的写法（`-machine q35 -device virtio-blk-pci,drive=x0`）即
正常。所以我此前「x86_64 dev 镜像坏了」的结论**是错的**：镜像没坏，坏的是驱动加载，
而那半是我参数错。

修后 x86_64 完整启动：`[INIT] Block device -> /bin (fat32)` → execve mksh →
`audit errors=0`、`MM-ASM pt_pages=9 missing_meta=0 present=0 absent=0 prot=0 cow=0
vma=0 anon_virt=0`。riscv64 侧无回归（`smoke-mm-stress` / `smoke-mm-fork-exec-race` 通过，
审计全 0），x86_64/riscv64/aarch64 构建通过。

### 10.11 x86_64/KVM 可用了，但噪声比结论还大：撤回「1.5× 远高于噪声」

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

关键观察一：噪声带宽达 1.26–1.78×。这比 TCG 并没有变好多少（§10.8 记的 mmap-PF
离散度是 45%）。所以「换到 KVM 就能分辨性能结论」这个预期**不成立**。

关键观察二：第 3 次采样在 10 个指标里有 9 个是最慢的。随机噪声不会这么整齐地
同向劣化，这是系统性漂移（共享宿主负载、频率调节，或同一宿主上并发任务），不是
采样抖动。「多采几次取中位数」救不回来，因为漂移是单调的。

**必须撤回的结论**：§10.1 写「基线与带预标记之间约 1.5× 的差距远高于噪声，是真实的」。
在 1.78× 的噪声带宽下，1.5× 落在噪声之内，那句话同样不成立。至此三条性能结论
（1.5× 回归真实、预标记导致回归、预标记让 PF 变快 28%）全部不可采信。不是推理错，
而是本宿主上无法把它们和噪声区分开。

方案上的影响：在拿到可用的测量手段之前，不应该再基于这些数字做设计决策（包括我曾
建议的「关闭/保留预标记」）。要让测量可信，至少要做 CPU 绑定（`taskset` 固定到独占
核）、显著更多采样、并确认采样期间宿主空闲；或者干脆换到独占/裸金属环境。这应当排在
任何进一步的性能优化之前，否则后续每个数字都同样不可信。

### 10.12 顺序 A/B 无效：必须交错采样

§10.11 找到噪声来源（共享宿主，4 个 opencode 进程各占 50–116%）后，先用
`taskset -c 12-15` 把 guest 的 4 个 vCPU 绑到 4 个独占核，噪声确实改善：最差 max/min
从 1.78× 降到 1.47×，10 项里 9 项落到 1.30× 以下（`mmap high` 1.08×、
`mmap-PF high` 1.12×、`PF low` 1.14× 已经够紧）。

于是用同样的绑定做了预标记 ON/OFF 的 A/B（各 4 次采样，OFF 臂在后跑）。结果：

| 基准 | ON 中位 | OFF 中位 | OFF/ON | ON 离散 | OFF 离散 | 离散变化 |
|---|---|---|---|---|---|---|
| mmap low | 86573 | 102353 | 1.18× | 1.22× | 1.36× | 1.11× |
| mmap high | 79429 | 90115 | 1.13× | 1.08× | 1.51× | 1.40× |
| mmap-PF low | 200617 | 211826 | 1.06× | 1.29× | 1.90× | 1.48× |
| mmap-PF high | 183376 | 212502 | 1.16× | 1.12× | 2.37× | 2.12× |
| PF low | 90917 | 106554 | 1.17× | 1.14× | 1.64× | 1.44× |
| unmap low | 164985 | 182378 | 1.11× | 1.28× | 1.47× | 1.15× |

中位数看起来「OFF 慢 6–18%」，但这个差值不可采信：OFF 臂的离散度本身普遍变大
（mmap-PF high 从 1.12× 涨到 2.37×），且 OFF 臂最后一次采样在 10 项中 7 项最差，
而 ON 臂只有 3 项。这说明宿主在 OFF 臂采样期间变忙了。漂移发生在两臂之间，不是臂内。
臂间漂移（可达 1.2×）大于待测效应（≤1.18×），所以顺序 A/B 无法把二者分开。

结论：在这个共享宿主上，**顺序 A/B 一律无效**，无论采样多少次。可信 A/B 只能靠交错
采样（ON,OFF,ON,OFF,… 交替），让漂移对两臂对称；或者等宿主空闲的窗口。顺带把
`MM_ANON_PROVISION_MAX_PAGES` 做成可运行时切换（当前是编译期常量，强制每臂重新构建，
这本身就是交错采样的障碍）。

至此关于预标记开/关的性能问题仍然没有答案，且这是测量方法问题而非实现问题。
在拿到交错数据之前，不要再据这些数字改动设计（包括「保留还是关闭预标记」）。

### 10.13 预标记上限改为可运行时切换（交错采样的前提）

`MM_ANON_PROVISION_MAX_PAGES` 原先是编译期常量，导致 ON/OFF 两臂必须各自重新构建一次。
在共享宿主上这本身就会引入漂移（§10.12），而交错采样（ON,OFF,ON,OFF,…）是让漂移对两臂
对称的唯一办法。切换一旦需要重新构建，交错就无从谈起。

故新增 boot 参数 `a20.anonprov=<pages>`，扫描方式沿用 `mm/wx.c` 处理 `a20.wx=` 的
既有风格（`kernel/mm/pt.c` 的 `mm_pt_anon_prov_init()`，由 `main.c` 在 `bootargs_init()`
之后调用，与 `mm_wx_policy_init()` 相邻）。默认值仍是 `MM_ANON_PROVISION_MAX_PAGES`，
`a20.anonprov=0` 关闭预标记。

已验证（riscv64，`-append "a20.anonprov=N"` 经 DTB `/chosen/bootargs` 进入内核）：

| 参数 | `mm_anon_provisioned` | 关机审计 |
|---|---|---|
| `a20.anonprov=4096` | 9277 | 全 0 |
| `a20.anonprov=0` | 0 | 全 0 |

两臂均无参数解析告警。MM-ASM 审计 `missing_meta/present/absent/prot/cow/vma/anon_virt`
全 0，`smoke-mm-stress`、`smoke-mm-fork-exec-race`、`check-mm-lock-model` 通过，
riscv64/x86_64/aarch64 与 riscv64/aarch64 NOMMU 构建通过。

x86_64 上该参数目前无效，且这是既有的独立缺口。`arch_bootargs_get()` 在 x86_64 由
`kernel/platform/qemu-virt-x86_64/board.c` 实现，读的是 QEMU fw_cfg 的
`opt/cmdline`。实测无论用 `-append` 还是
`-fw_cfg name=opt/cmdline,string=...`，内核都只看到
`[FW_CFG] cmdline_size=0`、`cmdline=''`，即命令行根本没进内核。后果是 `a20.wx=`
在 x86_64 上也被静默忽略，而 W^X 是安全相关策略，这比本次新增的参数严重得多，
应单独修（需要弄清 fw_cfg 的 cmdline 文件为何在该启动路径下未被填充）。
在此之前，x86_64 上的交错 A/B 仍不可行；riscv64 上可行。

### 10.14 交错 A/B 的结果：预标记让 mmap 慢约 14%，且买不到任何东西

用 §10.13 的运行时开关做交错采样（单次构建，6 对交替 ON,OFF,ON,OFF,…，
riscv64/TCG + `taskset -c 12-15`）。每对的 ON 与 OFF 在时间上紧邻，逐对比值因此能抵消
§10.12 那个「臂间漂移」：

| 基准 | 逐对 OFF/ON 中位 | OFF 更快出现在 | 判定 |
|---|---|---|---|
| mmap low | 0.863 | 6/6 对 | 有效应：预标记使 mmap 慢约 14% |
| mmap high | 0.867 | 5/6 对 | 有效应：约 13% |
| PF low | 0.922 | 4/6 对 | 无显著效应 |
| PF high | 0.97 | — | 无效应 |
| mmap-PF low / high | 1.09 / 0.94 | — | 无效应 |
| unmap low / high | 0.993 / 1.01 | — | 无效应 |
| unmap-virt low / high | 1.00 / 0.98 | — | 无效应 |

10 项里 8 项落在 0.90–1.11（无效应），只有 mmap 两项明确落在带外，且 mmap low 是
6/6 对同向（符号检验 p≈0.016）。这是本项目里第一份方法学可信的性能结论。

据此**撤回**三条此前结论：

1. §10.1「mmap 慢 1.5×」，量级错了。真实成本约 14%，不是 150%。
2. §10.8「预标记让 PF 快 28%」不存在。PF 逐对比值 0.92/0.97，方向不一致，
   属噪声；当时那个 28% 是顺序 A/B 的臂间漂移造出来的。
3. §10.8「关掉预标记 mmap 没变快（无效应）」也错了。当时被噪声完全掩盖；
   交错之后效应清晰可见。

净结论：预标记当前让 mmap 慢约 14%，而它服务的消费者（按状态缺页路径）仍是死代码
（§10.6，`mm_fault_from_status` 恒为 0）。也就是��付出 14% 的 mmap 成本，换来零收益，
因为唯一能消费这些状态标记的 fault 路径从未执行过（§10.7 还说明即便修好查询，
它的位置也绕过了 userfaultfd/共享/fault-around）。

因此在修好状态路径之前，预标记应当关闭（默认 `a20.anonprov=0`），这是一个可以
立刻回收 14% mmap 性能、且不损失任何现有功能的改动。它与 §10.7 记录的依赖顺序不冲突：
预标记是那套设计的地基，但地基单独存在时是净负担。

### 10.15 把预标记默认改为关闭，回收 14% mmap

按 §10.14 的结论落地：预标记现在默认关闭，只有显式给出 `a20.anonprov=<pages>`
才会启用（`g_anon_prov_max` 初值改为 0）。

没有丢任何东西。地基（`mm_pt_provision_anon()` 及其在 mmap / ELF 匿名段 / brk
三处的接线）全部保留在树里，开销只是那一个 boot 参数。改的只是默认值。

验证（riscv64）：

| 启动参数 | `mm_anon_provisioned` | MM-ASM 审计 | `mm_stress` |
|---|---|---|---|
| 无（默认） | 0 | 全 0 | PASS |
| `a20.anonprov=4096` | 9277 | 全 0 | PASS |

两臂均无参数解析告警。`smoke-mm-stress`、`smoke-mm-fork-exec-race`、
`check-mm-lock-model` 通过，riscv64 / x86_64 / aarch64 与 riscv64 NOMMU 构建通过。

一个过程教训：第一次验证 opt-in 时我把 `-append` 的值放在未加引号的 shell 变量里，
且带一个前导空格，QEMU 于是把 `a20.anonprov=4096` 当成磁盘镜像路径而直接退出，日志只有
一行 `Could not open ...`。当时那个 `stress-pass: 0` 看起来像内核回归，其实是我的测试
脚本 bug。这与 §10.8 那次「误把热页当缺页」是同一类错误：把工具链/脚本故障当成被测
系统的结论。在把一次失败归因于被测代码之前，先确认测试装置本身是对的。

### 10.16 PT 页 UAF：拆表点逐个审计（修复的前置条件）

UAF 本身：`pt_unmap()` / `pt_unmap_leaf()` 的拆表循环用
`mm_pt_node_fini(child); frame_free(child);` 同步释放 PT 页，而
`mm_addrspace_lock()` 只锁覆盖节点、后代是无锁遍历的。另一 CPU 上正沿该后代下降的
cursor 可能在帧被 buddy 回收后继续读它。`mm_pt_defer_free()` /
`mm_pt_mark_stale_recursive()` 没有任何调用者，`vm.c` 排空循环等的是一条永远不会被
填充的链表。

修复必须接上生产端（拆表时先标记 stale 再交给排空），但前提是每个拆表点都在
`mm->lock` + TLB 事务内，否则 `mm_pt_hold_table()` 会无锁改写 `mm->tlb_holds`，
且没有事务就永远没人排空。逐点审计结果：

| 拆表点 | 所在函数 | `mm->lock` | TLB 事务 | 结论 |
|---|---|---|---|---|
| `munmap.c:111` | `mm_munmap_locked` | 是（`mm_munmap` 287） | 是（287/291） | 安全 |
| `munmap.c:241` | `mm_brk_locked` | 是（`mm_brk` 299） | 是（298/302） | 安全 |
| `madvise.c:68/86` | `mm_madvise_dontneed` | 是（55） | 是（54/99） | 安全 |
| `mprotect.c:103` | `mm_mprotect_locked` | 是（162） | 是（161/165） | 安全 |
| `vm.c:421` | `mm_demote_huge_page` | 视调用者 | 视调用者 | 混合，见下 |
| `vma.c:466` | `free_vma_pages` | 否 | 是（`vm.c:545`） | 缺 `mm->lock` |
| `vma.c:442` | `mm_demote_huge_page` 的调用点 | 否 | 否 | 不安全 |
| `sysv_shm.c:80` | `sysv_shm_unmap_attached_pages` | 否 | 否 | 不安全 |
| `mremap.c:184` | `mm_move_mapping_pages` | — | — | `__attribute__((unused))` 死代码 |

有一条不那么显然的结论：`mm_demote_huge_page()` 自身不能被当作安全点，它的调用者
安全性不一致（`munmap.c:90/232`、`mprotect.c:103` 安全，而 `vma.c:442` 既不持锁也不在
事务内）。所以不能只在 `mm_demote_huge_page()` 内部加 `mm_pt_defer_free()`，否则从
`vma.c:442` 进来时依旧是无锁改链表。要么把 `vma.c:442` 的调用者补齐锁与事务，要么让
拆表本身不依赖 TLB 事务（例如独立的、由 `mm->lock` 保护的回收队列 + 在
`mm_tlb_invalidate_finish()` 之外也能排空的路径）。

建议优先后者。理由是 TLB 事务的语义是「本事务内我改了哪些地址，需要 shootdown」，
而 PT 页回收的语义是「这个节点已不可达，等读侧退出再释放」，两者本来不该耦合。把它
塞进 TLB 事务导致每个拆表调用点都必须开事务，这是不必要的耦合，也是本次修复差点
不安全的根因。独立队列只需 `mm->lock` 保护链表 + 一个可在任意上下文
调用��排空点。

### 10.17 修好 PT 页 UAF：回收与 TLB 事务解耦

UAF 一直存在，成因与 §10.16 记的完全一致：`pt_unmap()` / `pt_unmap_leaf()` 的拆表循环
用 `mm_pt_node_fini(child); frame_free(child);` 同步释放 PT 页，
`mm_addrspace_lock()` 只锁覆盖节点、后代是无锁遍历的，于是另一 CPU 上正沿该后代
下降的 cursor，可能在该帧被 buddy 回收后继续读它。`mm_pt_defer_free()` /
`mm_pt_mark_stale_recursive()` 此前没有任何调用者，排空循环等的是一条永远不会被
填充的链表。

为什么上次接不上：把 `mm_pt_defer_free()` 塞进 `pt_unmap*` 内部，要求每个拆表点都在
`mm->lock` + TLB 事务内，但 8 个调用点里有 6 个两者皆无（`vma.c:466`、
`vma.c:442`、`sysv_shm.c:80` …），于是只能无锁改写 `mm->tlb_holds`，那版已回退。
根因是把两件无关的事耦合了：TLB 事务的语义是「本事务改了哪些地址、需要
shootdown」，而 PT 页回收的语义是「该节点已不可达、等读侧退出再释放」。

本次修法：`mm_struct` 新增 `pt_retire` 链表与自己的 `pt_retire_lock`，
与 TLB 事务彻底无关，因此任何上下文都能调用，不需要 `mm->lock`，也不需要开事务。
`mm_pt_retire_table()` 做三件事：

1. `mm_pt_mark_stale_recursive()`，让缓存了该节点的 cursor 观察到 `stale` 并重下降
   （`mm_addrspace_lock` 本来就在下降途中 `mcs_lock` 之后和加锁之前各查一次）。
2. 不立即释放：帧保持 `FRAME_F_PT`，所以 buddy 不会把它发给别人；元数据也留到
   排空时才丢，这样「已脱离」的状态由 `stale` 表达，而不是靠元数据消失来暗示。
3. 入队后立刻尝试排空。若此刻 `mm->pt_readers == 0` 就当场回收，否则留给下一次
   retire 或 `mm_destroy` 的兜底排空。

这个宽限期为什么成立：`mm_addrspace_lock()` 在下降读任何 PT 项之前就
`mm_pt_read_enter()`，且每层只读一次父项。于是父项被清零之后，新的 cursor 不可能
再到达该页；而已经持有它的 cursor 一定被计入 `pt_readers`。所以「等 `pt_readers` 归零」
正好覆盖窗口。（注意这修的是帧回收这一半；`pt_unmap_leaf()` 的裸遍历与 cursor
下降之间对 PTE 本身的无锁竞争是另一件事，本次未处理。）

riscv64 / x86_64 / aarch64 / loongarch64 / ppc64le 与 riscv64 / aarch64 / x86_64 的
NOMMU 全部构建通过。`smoke-mm-stress`、`smoke-mm-fork-exec-race`、
`check-mm-lock-model` 通过，审计 `missing_meta/present/absent/prot/cow/vma/anon_virt`
全 0。关键证据是 `pt_pages=6` 与改动前一致，且在 churn 最猛的
`smoke-mm-fork-exec-race` 下同样为 6：若退役的帧没被真正回收，这个计数会随拆表次数上升。

`mm_destroy()` 在 VMA 拆除之后、`mm_destroy` 早期补一次 `mm_pt_retire_drain()`。此时
最终引用已归零、地址空间无法再被进入，`pt_readers` 必为 0，一次即可排空，不会泄漏。

**未能验证**：arm32 在本环境改动前就无法构建（`arm-linux-gnueabihf-gcc` 不在
PATH），其自有的一份 `pt_unmap` / `pt_unmap_leaf` 已同步改签名并从 `mm->pgdir` 取
pgdir，但编译未经检验。

### 10.18 给 per-PTE 状态补上安全语义位（状态路径的前置条件之一）

§10.7 记录了状态路径的位置缺陷：它在 `mm_find_vma` 之前返回，因而绕过
userfaultfd、`VM_SHARED` 与 fault-around 的安全门。论文的设想是用 per-PTE 状态
取代 VMA 成为权威来源，前提是状态本身能表达这些判定，而原来的状态字节做不到：
8 位全部分配完毕（4 位 class + COW + 3 位 prot），且共享性其实已经编码在 class 里
（`MM_ST_ANON_SHARED` / `MM_ST_FILE_SHARED`）。所以并不是「没有地方放」，而是
「剩下的两项判定确实放不下」。

本次为此新增两项，并做成并行位图，而不是把 `cls[]` 扩成 16 位/项（后者会把每项
元数据翻倍）。这里保留位图是对的：`safe` 是**每 8 项 1 bit**（尺寸不敏感），
而 COW 是**每项 1 bit**（每 PT 页 512 bit = 64 B，为一个能从状态字节直接读出的
事实再买一张表，代价与收益不成比例——见 §13.19）：

* `MM_SAFE_UFFD`：该项被 userfaultfd 区间覆盖。缺页必须停住交给 handler，
  绝不能直接造零页满足。
* `MM_SAFE_NO_FA`：该项不得被多页 fault-around 覆盖（VMA 已被 seal，或该 class 下
  投机分配会改变语义）。

`pt_meta_t` 新增 `safe[(MM_PT_META_ENTRIES + 7) / 8]`；`mm_pt_node_init()` 本来就
`memset` 整个结构，故新位图天然归零。

有个必须做对的细节：`mm_pt_note_absent()` 原本只清 cow 位（该位图已于 §13.19 删除，
这条记录保留它当初被写下的样子）。若不同步清 `safe`，
被复用的槽位会继承上一条映射的 UFFD/NO_FA 标志，状态路径据此做出错误判定。所以
清槽位时一并 `&= ~MM_SAFE_MASK`。同理 `mm_pt_safe_set()` 拒绝在 class 为
`MM_ST_INVALID` 的槽位上置位：无映射的槽位上的安全位没有意义，也永远不会被清掉。

提供 `mm_pt_safe_set/clear/test()` 三个粒度为「一位」的接口，使任何改动槽位的代码
都必须经由它们，从而保证这些位不会活得比它所描述的 class 更久。

当前状态：这些位**已就位但尚未被任何代码写入**。真正的赋值方是 §10.7 依赖链里的
「mmap/mprotect/munmap/madvise + userfaultfd 注册全部接入状态更新」那一步。因此状态
缺页路径仍保持 inert（`mm_fault_from_status` 仍恒为 0），行为零变化。

riscv64 / x86_64 / aarch64 / loongarch64 / ppc64le 与 riscv64 / aarch64 / x86_64 的
NOMMU 全部构建通过，`smoke-mm-stress`、`smoke-mm-fork-exec-race`、`check-mm-lock-model`
通过。审计 `missing_meta/present/absent/prot/cow/vma/anon_virt` 全 0，`pt_pages=6` 不变。

### 10.19 把安全位接到真正会改变安全语义的 syscall 上

§10.18 只是把位加出来了。这一步把它们接到真正的写入方。

新增 `mm_pt_set_safe_range(mm, start, end, flags, set)`：按 leaf 逐块遍历
（像 madvise 那样把查表摊销掉，而不是每页查一次），`set==0` 时清除。

mseal（`kernel/mm/mseal.c`，`mm_mseal_locked` 内，调用方已持 `mm->lock`）对每个被
seal 的 VMA 交集 `[a, b)` 置 `MM_SAFE_NO_FA`。seal 存在的原因是冻结这段状态，而多页
fault-around 会投机地给邻居装帧，那正是 seal 要禁止的副作用。

userfaultfd 那边分两步。UFFDIO_REGISTER（`kernel/ipc/userfaultfd.c`）在 range 真正
挂上 `uffd->ranges` 之后、`g_uffd_lock` 释放之后置 `MM_SAFE_UFFD`。放在成功之后
而非校验循环里，是因为校验失败（覆盖不足 / 与既有注册重叠）时 range 并不存在，那时就
标位等于凭空造出一个「已被注册」的页面。UFFDIO_UNREGISTER 则清掉实际移除的那段
（记录移除 range 的并集，而不是直接用请求范围），同样在 `g_uffd_lock` 之外、
`mm->lock` 之下做页表遍历，避免在全局 range 锁里嵌一次页表走表。

这里有一处保守之处：同一页面原则上可被另一个 uffd 的注册覆盖，所以 unregister
这里的清除会清得比严格需要的多。今天这是安全的，VMA 缺页路径上真正的权威判定是
`userfaultfd_range_present()`，本次完全没动它，残留的位也不会让缺页跳过 handler。
但在状态缺页路径启用之前，这必须改成逐页复查 presence 后再清，否则「清多了」会让
仍被注册的页面不经停靠直接缺页成功。

状态上，这一步之后这些位**已写入但仍无人读取**。读取方是尚未启用的状态缺页路径
（`mm_fault_from_status` 仍恒为 0），所以行为零变化。这是纯铺垫。

尚未完成：todo 里与本项并列的「在 MM-ASM auditor 里加 status-vs-VMA 一致性检查」
还没做。启用状态路径之前必须有它，否则无法证明状态与 VMA 等价。

> **2026-10-04 更正**：这个检查**后来做了**——就是审计里的 `vmai` 计数器
> （从状态侧反查是否有记录覆盖），连同正向的 `vma` 计数器构成双向核对，
> 见 §7 的字段表与 `kernel/mm/pt.c` 中 `vmai_mismatch` 的注释。

`smoke-mm-stress`、`smoke-mm-fork-exec-race`、`check-mm-lock-model` 通过，审计全 0，
`pt_pages=6` 不变。构建覆盖 riscv64 / x86_64 / aarch64 / loongarch64 / ppc64le 与
riscv64 / aarch64 / x86_64 的 NOMMU，全部通过。

### 10.20 auditor：把 seal ↔ MM_SAFE_NO_FA 变成机器可检的不变量

§10.19 接好了 mseal → `MM_SAFE_NO_FA`，但「看起来对」不算证据。`mm_pt_audit()` 里原本已有
VMA 交叉检查（`vma_mismatch`：每个 VMA 至少要有一页被元数据认识），现在按同样的做法再加
一条双向不变量：

对每个 VMA 的每一页，若该页已映射（class 不是 `MM_ST_INVALID`），则
`MM_SAFE_NO_FA` 必须与 `VM_SEALED` 一致。seal 住的区间若仍能被 fault-around 拉进来，
seal 就失效了；反过来标了却没 seal，则是无谓地关掉了 fault-around。任何一边漏更新，
都会在关机审计里变成 `safe=N`，而不是等到某次投机分配悄悄改了被冻结的状态。

新计数项 `safe_mismatch` 已并入 `mm_pt_audit_errors()`，所以它**会让审计失败**，
不是只打印一个数字。审计行现在多一个 `safe=` 字段：
`missing_meta=0 present=0 absent=0 prot=0 cow=0 vma=0 safe=0 anon_virt=0`。

`MM_SAFE_UFFD` 故意不查：UFFDIO_UNREGISTER 清的是「实际移除的 range 的并集」，
一个仍被另一个 uffd 注册覆盖的页面会丢掉标记，于是「UFFD 位 ⟺ 被注册」今天根本不是
不变量，拿它做检查必然报假错。权威判定始终是 VMA 缺页路径上的
`userfaultfd_range_present()`（本次未动）。等状态缺页路径真要启用、且 unregister 改成
逐页复查 presence 之后再清（§10.19 已标注），才把 UFFD 位纳入审计。

这条检查不是空跑：`mm_stress.c` 里有 23 处 mseal 调用，`smoke-mm-stress` 真的会
seal 区间，`safe=0` 因此是跑过之后的一致性结果，不是「没有 seal 所以没得查」。

构建与门禁：5 个架构与 3 个 NOMMU 变体全部构建通过；`smoke-mm-stress`、
`smoke-mm-fork-exec-race`、`check-mm-lock-model` 通过，审计全 0（含新增的 `safe=0`），
`pt_pages=6` 不变。

### 10.21 状态缺页路径：查询修复 + UFFD 门控（默认仍然关闭）

两处改动，默认配置下是 no-op：

1. `mm_cursor_query()` 对缺失叶子不再直接返回而把 `cls_out` 留在 `INVALID`，它现在
   报告元数据里记下的 class。这正是 §10.6 判定「路径从未执行」的那一行。
2. fault.c 状态路径加 `MM_SAFE_UFFD` 门：该项若被 userfaultfd 注册，必须回落到
   VMA 路径停靠给 handler，不能就地满足。辅助接口 `mm_cursor_safe_test()` 让调用方在
   已持有覆盖节点锁的情况下问「这一项是不是被注册了」，不必再下降一次。

现在可以安全地打开这条路径，对照 §10.7 列的三条绕过看：

* 绕过 userfaultfd：已由 `MM_SAFE_UFFD` 挡住，且该位会写到「已预留未缺页」的项上
  （class 是 `ANON_VIRT` 而非 `INVALID`，所以 `mm_pt_safe_set()` 不会拒）。
* 绕过 `VM_SHARED`：天然不成立。状态路径只接受 class `ANON_VIRT`，而共享映射的
  class 是 `MM_ST_ANON_SHARED` / `MM_ST_FILE_SHARED`，永远不是 `ANON_VIRT`。
* 绕过 fault-around：天然不成立。状态路径一次只映射一页，根本不做 fault-around。

默认关闭的原因正是预标记默认关闭：没有 `ANON_VIRT` 项，状态路径就永远不会命中。
实测 `a20.anonprov=0`（默认）：`mm_anon_provisioned=0`、`mm_fault_from_status=0`、
`mm_anon_faults=672`、`mm_demand_faults=1484`，mm_stress PASS，无 FATAL。

打开时确实会崩，(b) 依然存在。`a20.anonprov=4096`：

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

有一个尚未解释的疑点：出错的页在转储里是已映射且 `flags=0x7`（RWX），内容全 0，但
CPU 仍在它上面 fault（`status` 低位指向取指/访问类异常）。「PTE 看起来可访问却仍 fault」
本身是矛盾的，所以要么转储读到的 PTE 不是 fault 当时的状态，要么存在另一条使该映射
无效的路径。**根因未定位**，需要继续查；在定位之前不得把预标记默认打开。

另注：brk VMA 上的 `off=0x1000` 对匿名堆来说没有意义（brk 段的 `vm_pgoff` 应当为 0），
也是一个待解释的异常。

riscv64 / x86_64 / aarch64 构建通过，`smoke-mm-stress`、`smoke-mm-fork-exec-race`
通过，审计全 0（含 `safe=0`），`pt_pages=6` 不变。默认配置下 `mm_fault_from_status`
为 0，行为与改动前完全一致。

### 10.22 定位 (b)：二分法，结论是 mmap 预标记那条路径

崩溃只在「预标记开 + 状态路径可用」时出现，且 `a20.anonprov=0`（默认）时完全不崩
（`mm_fault_from_status=0`、stress PASS、无 FATAL）。所以把范围收窄到：三个预标记调用点
里哪一个在喂出坏状态。

先记一次我自己的方法错误。前三次二分每次只关掉一个调用点、另两个仍然开着，于是那三次
「关掉 brk 仍崩 / 关掉 ELF 仍崩 / 关掉 mmap 仍崩」**全都无效**：它们只说明「单独关掉某一个
不足以消除崩溃」，不能推出「那个是无辜的」。这三次之后我才发现，每次都把前一个改动
`git checkout` 还原了，等于每次都在测「关掉一个、留着两个」。这与 §10.8「把热页当缺页」、
§10.15「把脚本 bug 当内核回归」是同一类错误：在把结论归因到某个组件之前，先确认实验
装置真的只变了那一个变量。

正确的二分是关掉 ELF + brk、只留 mmap（`a20.anonprov=4096`）：

| 配置 | 结果 |
|---|---|
| 三个全开 | FATAL |
| 关 brk（ELF+mmap 仍开） | FATAL（无效实验） |
| 关 ELF（brk+mmap 仍开） | FATAL（无效实验） |
| 关 mmap（ELF+brk 仍开） | FATAL（无效实验） |
| 只留 mmap（ELF+brk 关） | FATAL ← 复现 |
| 三个全关 / 默认 `anonprov=0` | 无 FATAL |

(b) 由 mmap 路径的预标记喂出，与 ELF 段、栈/TLS、brk 无关。这也说明它不是「某个 VMA
分类错了」那类问题（那正是 ELF 的教训，§10.17），而是 mmap 这条
`mm_pt_provision_anon(mm, addr, addr + len, ptef)` 调用本身有问题。

尚未解释的是崩溃点的位置：它仍落在 brk 地址上（`stval=0x807000`，
`vma=[0x807000,0x808000) flags=0x13 pte_flags=0xd7 file_fd=-1`），而 brk 预标记此时是
关闭的，状态路径不可能直接处理它。mmap 预标记只能是通过别的途径间接破坏它的
（页表被写坏、某个帧被提前回收、或统计/引用计数失衡），而不是在 brk 上命中。这条线索
指向 `mm_pt_provision_anon()` 内部：它按 leaf table 为单位 `mm_addrspace_lock()` 后
`mm_pt_note_present()` 逐项写状态，与 §10.17 新加的 `mm_pt_retire_table()` / 宽限期回收
、以及 `mm_pt_note_absent()` 里新增的清 safe 位都是同一批元数据写者。

具体该做的检查有三条（未做，根因未定位）：

1. 在 `mm_pt_provision_anon()` 里对 `[lo,hi)` 逐 leaf 校验 `table[idx]` 确实为 0，
   遇到非 0 就计数上报。预标记只应写未被映射的槽位。
2. 崩溃后 dump 出错页的 PT 页 `pfa.meta[pfn].pt` 是否已 `NULL`/`FRAME_F_PT` 被清，
   以区分「页被提前回收」与「页表被写坏」。
3. 确认 mmap 的 `ptef` 经 `status_byte()` 往返后与 VMA 实际权限一致（对照 §10.14 之外的
   `mm_pt_prot_bits()` 编码宽度）。

(b) 未定位，所以预标记继续保持默认关闭，状态缺页路径也就仍然 inert。

### 10.23 一条死路：nr_present 溢出不是 (b) 的原因

预标记会把 `m->nr_present` 往上推（`pt_note_present_meta()` 每个槽位 `++`），而它是
`uint16_t`。既然 (b) 由 mmap 预标记喂出（§10.22），最自然的怀疑就是它在某个 leaf table
上累加到 65536 之后回绕。查证结果：不是。

`pt_table_empty()`（决定某个中间页表页能否被拆掉回收的唯一判据）直接扫 PTE 数组，
根本不看 `nr_present`：

```c
static int pt_table_empty(pte_t *table, int level) {
    for (int i = 0; i < entries; i++)
        if ((table[i] & PTE_V) || pte_is_swap(table[i])) return 0;
    return 1;
}
```

`nr_present` 的全部使用点只有三处：自增、自减、fork 时整体拷贝
（`pt_clone_level()` 里 `dst->nr_present = src->nr_present`）。没有任何控制流依赖它，
所以即使回绕也只是一个显示用的计数失真，不会导致「非空页表被当成空表回收」这类损坏。

记下来，免得有人再走一遍。

接下来该往哪儿试，按可能性排序（均未做）：

1. 状态路径缺 TLB 事务。状态路径映射成功后只做
   `arch_tlb_flush_page_local(stval)`，而 VMA 路径走的是
   `mm_tlb_invalidate_begin/finish` + `mm_tlb_note_change` 的完整事务。在
   `-smp 2` 下，如果同一个地址空间此刻在另一个 CPU 上有残留翻译，本地刷新不足以
   让它看到新映射。这与症状吻合：出错页在转储里 `flags=0x7` 可访问、内容全 0，但发起
   访问的 CPU 手里仍是旧的（无效）翻译。可用 `-smp 1` 复跑一次来证伪或证实：
   若 `-smp 1` 不崩，基本锁定这一条。
2. `mm_pt_provision_anon()` 的 chunk 循环里 `cur.path[0]` 是否在 `lo` 跨 leaf 边界时
   取到了错误的叶子表（`anchor` 取自 `lo`，但 `hi` 可能延伸进下一张表）。
3. mmap 的 `ptef` 经 `status_byte()` 往返后是否与 VMA 实际权限一致（对照
   `mm_pt_prot_bits()` 的编码宽度）。

在 (b) 定位之前，预标记不动（默认关闭），状态缺页路径保持 inert。

### 10.24 证伪 TLB 假设，并把搜索空间砍半

§10.23 列的第一条假设是「状态路径只做 `arch_tlb_flush_page_local()`、没走完整 TLB 事务，
`-smp 2` 下另一 CPU 可能持有残留翻译」。用 `-smp 1` 复跑证伪：

| 配置 | `a20.anonprov=4096` |
|---|---|
| `-smp 2` | FATAL |
| `-smp 1` | FATAL（同样崩） |

单 CPU 下不存在「另一 CPU 持有残留翻译」这回事，这条假设随之不成立：(b) 与并发 TLB
一致性无关，是个单线程下就能触发的逻辑错误。

由此得到一个比之前更有价值的收窄。对照 `d1fb5c22`（预标记默认关闭、状态路径仍
inert 的那次提交）当时的实测：`a20.anonprov=4096` 下 `mm_anon_provisioned=9277`、
关机审计全 0、`mm_stress` PASS：

> mmap 预标记本身是安全的：崩溃只在状态路径真的去消费这些预标记条目时才出现。

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

与 VMA 路径逐项对照，最可疑的差异是：状态路径没有参与 TLB 事务，既没有
`mm_tlb_invalidate_begin/finish`，也没有 `mm_tlb_note_change()`。VMA 路径经
`fault_map()` 完成映射，会在事务内登记地址变化并在结束时统一 shootdown。状态路径只在
成功分支调一次本地刷新就 `return 0`，绕过了 `mm_tlb_invalidate_finish()`。

单 CPU 下这依然可能出错。事务不只服务于跨核 shootdown，它还承担「本事务内所有改动在
返回用户态前必须对当前 CPU 生效」这一职责。`mm_tlb_invalidate_finish()` 结尾会做
`arch_tlb_flush()` / generation 推进；直接 `return 0` 可能让当前 CPU 在
satp/ASID 或 TLB 状态未收尾时继续执行。

下一步具体且可执行：把状态路径的映射纳入 TLB 事务，进入前
`mm_tlb_invalidate_begin(mm)`，成功与失败的所有出口都
`mm_tlb_invalidate_finish(mm)`，并用 `mm_tlb_note_change(mm, page_va, PAGE_SIZE)`
替代裸的 `arch_tlb_flush_page_local()`。做完直接用 `-smp 1` 复跑：
崩→假设成立，可继续收窄；不崩→该假设也被证伪，换查 `mm_cursor_map` 与
`fault_map` 在「安装并记账」上的其他差异（例如 `mm->rss` 与 cgroup 记账是否重复）。

在 (b) 定位之前，预标记保持默认关闭，状态缺页路径保持 inert，默认配置行为与改动前
完全一致（`mm_fault_from_status=0`，全部门通过）。

### 10.25 再证伪一条：状态路径并不缺 TLB 事务（更正 §10.24 的首选假设）

§10.24 把「状态路径没有参与 TLB 事务」列为第一嫌疑。这条也是错的，而且是
「先下结论、后看代码」的又一次。`fault_map()` 才是 VMA 路径真正用来装映射的函数，
它的结构与状态路径完全一致：

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

`mm_addrspace_lock` → `mm_cursor_map` → `mm_cursor_unlock`，没有开事务。VMA 路径的事务
是由更外层（系统调用层 / `handle_demand_fault` 的调用者）开的，不是 `fault_map` 开的。
把 `fault_map` 和匿名 VMA 路径的后续记账（`rss++`、`a20_perf_count`、
`arch_tlb_flush_page_local`）逐行对比状态路径，看不出实质差异。

顺带说明：假如真要给状态路径开事务，还要小心 `mm_tlb_invalidate_finish()` 内部会
`spin_lock_irqsave(&mm->lock)` 取排空链表，而缺页路径此刻已持 `mm->lock`，直接加
事务会自锁。事务必须像 `mm_munmap` 那样「开事务 → 加锁干活 → 解锁 → finish」成对使用。
这条也说明「照搬事务」不是无风险的补丁。

至此三条假设**全部被证伪**（§10.23 `nr_present` 溢出、§10.24 跨核 TLB 残留、本节事务
缺失），而唯一确定的事实仍是 §10.22/§10.24 的收窄：

* mmap 预标记单独是安全的（`d1fb5c22` 实测：9277 页、审计全 0、stress PASS）；
* 崩溃只在状态路径真的消费 mmap 预标记条目时出现；
* 且崩溃点是 brk 地址，而 brk 预标记在复现配置里是关闭的，状态路径不可能
  直接处理它。

最后这条是关键矛盾：状态路径够不到 brk，却能间接把 brk 搞坏。这说明破坏发生在
别处那些被状态路径写坏的东西上。仍存活的候选（本轮未验证）：

1. `mm_pt_retire_table()` 的宽限期回收（§10.17 新增），它是本轮之前不存在的新写入者，
   而 §10.24 用来证明「预标记本身安全」的那次实测（`d1fb5c22`）早于这个改动。
   现在它应该排第一：需要重做那个安全性验证（预标记开 + 状态路径 inert），
   看是否仍 PASS。
2. 状态路径在 `mm_cursor_map` 之外还改了哪些共享记账（`mm->rss`、cgroup charge），
   是否与随后 VMA 路径的处理重复记账或漏记。

下一步应当先做 1：在 `323664fc` 之后重跑「`a20.anonprov=4096` + 查询修复回退」，
确认预标记在当前代码上单独仍然安全。这是一条能把 (b) 的嫌疑范围直接砍掉一半的实验，
而且不需要任何新代码。

(b) 仍未定位，预标记因此继续默认关闭，状态缺页路径继续 inert，默认配置与改动前没有
差别（`mm_fault_from_status=0`，全部门通过）。

### 10.26 预标记本身仍然安全：宽限期回收被排除，嫌疑收进状态路径内部

§10.25 指出，用来证明「mmap 预标记单独是安全」的那次实测（`d1fb5c22`）早于 §10.17
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

结论：`mm_pt_retire_table()` 与宽限期回收被排除。在包含 §10.17 那套回收机制、以及
§10.18/§10.19 的安全位与 mseal/uffd 接线的当前代码上，mmap 预标记单独运行仍然安全：
预标记了近 9300 页，审计干净，`mm_stress` 通过，零 FATAL。

嫌疑范围于是被压到最后一层：崩溃只由状态路径消费 mmap 预标记条目这段逻辑本身引起，
与预标记写入、与回收、与安全位都无关。

核心矛盾仍未解释。复现配置里 brk 预标记是关闭的，状态路径够不到 brk 地址，崩溃却落在
brk 上（`stval=0x807000`，`vma=[0x807000,0x808000) flags=0x13 pte_flags=0xd7
file_fd=-1`）。「够不到的代码把目标搞坏了」只能有一个解释：状态路径在处理某个 mmap
区间时，破坏了 brk 路径后续依赖的某个共享不变式。按可能性排序（本轮均未验证）：

1. 记账重复。状态路径成功后 `mm->rss++`、并各自计了
   `A20_PERF_MM_ANON_FAULTS` / `MM_DEMAND_FAULTS`。而 brk 区间随后若走 VMA 路径再被
   处理一次，`rss` 或 cgroup 记账就会与实际帧数脱节；一旦有别处以 `rss`/`total_vm`
   为条件做判断，就会连锁出错。先查这一条：`mm_pte_flags_allow_access()` 之后
   是否漏了 VMA 路径会做、而状态路径没做的某个记账或检查。
2. `mm_cursor_map()` 在 class 已是 `MM_ST_ANON_VIRT` 的槽位上安装时，metadata 的
   `present/absent` 记账与 `mm_pt_note_absent()` 的清理是否自洽（`nr_present` 只是显示用
   计数，§10.23 已排除它参与控制流，但配对关系仍可能有错）。
3. 状态路径的 `return 0` 是否跳过了缺页函数尾部某些必须执行的收尾（`mm->rss` 之外的，
   例如 vma 引用、rss 记账的 vma 归属、或 fault-around 相关的统计）。

验证手段：把 §10.21 状态路径与 VMA 匿名路径（`fault.c` 中 `fault_map(t->mm, page_va,
pfn, vma->pte_flags, MM_ST_ANON_MAPPED)` 那一段）逐行并列 diff，只保留必要差异。
这两段在结构上应当等价（§10.25 已确认 `fault_map` 本身没有额外事务），任何不等价之处
就是嫌疑点，这一步不需要猜。

在 (b) 定位之前，预标记保持默认关闭，状态缺页路径保持 inert，默认配置行为与改动前完全
一致（`mm_fault_from_status=0`，全部门通过）。

### 10.27 并列 diff 的结果：找到一处记账不等价

把状态路径与 VMA 匿名路径逐段并列后，找到一处具体的不等价，不是猜测。

VMA 路径（`fault.c` 匿名/交换各段）在映射成功后除 `mm->rss++` 之外，还会做四项每任务/全局
的缺页计数：

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

`t->perf_page_faults` / `perf_page_faults_maj` 与两个 `g_perf_sw_*` 完全没动。这本身未必致命
（它们看起来只用于统计），但它是第一处可指认的差异，必须先补齐：一个缺页路径不记缺页数，
会让任何依赖该计数的判断（以及 `/proc` 上报的缺页率）失真。

另一个结构性观察（非缺陷，但记录下来）：VMA 路径在放锁分配页之后会重新加锁并重新校验：

```c
if (prepared > 0 && cp && (*cp & PTE_V)) { /* 已被别人映射 -> 撤销 */ }
else if (mm_find_vma(t->mm, page_va) != vma) { /* VMA 已被换掉 -> 撤销 */ }
```

并全程持有 `vma_put(t->mm, vma)` 的引用。状态路径没有这一套，也不该有：它全程持
`mm->lock`，不存在同样的 TOCTOU 窗口，也从不碰 VMA。这正是论文设想的好处，代价是
它必须自己保证每一样 VMA 路径靠 VMA 拿到的东西（权限、记账、统计）都能从状态位里补齐。
目前权限能补齐（`MM_ST_PROT_*` 往返），记账只补了一半，统计完全没补。

下一步明确且小：把上述四项计数补进状态路径的成功分支，然后重跑 `a20.anonprov=4096`。
仍崩，说明记账不是原因，按 §10.26 候选 2/3 继续查 `mm_cursor_map` 的 metadata 配对与
`return 0` 是否跳过收尾；不崩，则 (b) 的原因就是漏记这四项统计（很可能某个限额/回收策略读
`t->perf_page_faults`，在 brk 增长密集的路径上被触发）。这一条不需要任何猜测，
且是当前唯一已指认的差异，应先做。

预标记仍默认关闭，状态缺页路径保持 inert。默认配置下
`mm_fault_from_status=0`、审计全 0、`pt_pages=6` 不变，
`smoke-mm-stress` / `smoke-mm-fork-exec-race` / `check-mm-lock-model` 全通过，
5 个架构与 3 个 NOMMU 变体构建通过。下述所有 (b) 排查都在 `a20.anonprov=4096` 下进行，
默认路径不受影响。

### 10.28 补齐四项缺页统计：本身是对的，但**没有**修好 (b)

按 §10.27 的指认，把 VMA 路径那四项每任务/全局软缺页计数补进状态路径成功分支：

```c
__atomic_fetch_add(&t->perf_page_faults, 1, __ATOMIC_RELAXED);
__atomic_fetch_add(&t->perf_page_faults_maj, 1, __ATOMIC_RELAXED);
__atomic_fetch_add(&g_perf_sw_page_faults, 1, __ATOMIC_RELAXED);
__atomic_fetch_add(&g_perf_sw_page_faults_maj, 1, __ATOMIC_RELAXED);
```

`a20.anonprov=4096` 复跑：仍然 FATAL。§10.26 候选 1（记账重复/缺失导致连锁出错）由此被证伪。

但这个改动本身是正确的、应当保留。状态路径此前完全不记这四项，意味着任何走状态路径的
缺页都不会出现在 `t->perf_page_faults` 与 `/proc` 上报的缺页率里，这是与 VMA 路径的真实
记账不一致，与 (b) 是否由它引起无关。保留它是因为它对，不是因为它治好了崩溃。

至此 (b) 的五条假设全部被证伪：

| 假设 | 出处 | 结果 |
|---|---|---|
| `nr_present` uint16 回绕 | §10.23 | 证伪（`pt_table_empty` 直接扫 PTE，不看该计数） |
| 跨核 TLB 残留翻译 | §10.24 | 证伪（`-smp 1` 同样崩） |
| 状态路径缺 TLB 事务 | §10.25 | 证伪（`fault_map` 结构完全相同，也不开事务） |
| 宽限期回收 `mm_pt_retire_table()` | §10.26 | 证伪（状态路径 inert + 预标记开 = 9287 页、stress PASS） |
| 漏记四项缺页统计 | §10.27 | 证伪（补齐后仍崩） |

仍然确定的事实，都是实验结果而不是推断：

1. mmap 预标记单独安全（当前代码、预标记开、状态路径 inert → stress PASS、审计全 0）。
2. 崩溃只在状态路径真的消费 mmap 预标记条目时出现。
3. 崩溃点在 brk 地址，而复现配置里 brk 预标记关闭，状态路径够不到它。

第 3 条是核心矛盾，也是下一步唯一该盯的东西：状态路径在处理 mmap 区间时，破坏了 brk
路径后续依赖的某个共享不变式。记账与统计已排除，剩下最可能的是页表/metadata 层面的
共享状态。具体说，`mm_cursor_map()` 把一个 `MM_ST_ANON_VIRT` 槽位变成已映射时，
`mm_pt_note_present()` 记的 `nr_present` 与之后 `mm_pt_note_absent()` 的清理、以及
`mm_cursor_unmap()` 清除 stale 标记这三条路径之间是否自洽。

下一步应当做的具体实验仍不需猜测：在 `mm_pt_provision_anon()` 与 `mm_cursor_map()` 之后
各加一个廉价的「不变式断言」计数器，映射完成后立刻回读该槽位的 class/PTE，若出现
「class 声称已映射但 PTE 无效」或「PTE 有效但 class 仍是 ANON_VIRT」就计数，崩溃后再打印
这个计数。计数非零即证明 metadata 与 PTE 失配，并直接指出是哪一次写入造成的。

默认配置的安全状态与改动前一致：预标记默认关闭，状态缺页路径 inert。
`mm_fault_from_status=0`、审计全 0（含 `safe=0`）、`pt_pages=6` 不变，
`smoke-mm-stress` / `smoke-mm-fork-exec-race` 通过，riscv64/x86_64/aarch64 构建通过。

### 10.29 不变式断言：证明状态路径的映射本身是**正确**的（第六条假设被证伪）

按 §10.28 的实验设计，在状态路径 `mm_cursor_map()` 成功之后立刻回读该槽位，断言
「PTE 已置有效」且「class 已不再是 `MM_ST_ANON_VIRT`」；违例则计数
（新增 `A20_PERF_MM_STATUS_INVARIANT_BAD`）并在首次违例时 `kerr` 打印（因为随后的
崩溃会让任何计数器都读不到，所以必须当场出声）。游标的叶子表 `qcur.path[0]` 本来就在
手上，回读是一次已缓存指针的读，代价可忽略。

`a20.anonprov=4096` 实测：违例 0 次，崩溃照旧。

结论是 `ANON_VIRT → 已映射` 这次状态转换每次都干净落地，metadata 与 PTE 始终自洽。
状态缺页路径在「装映射」这件事上是正确的，(b) 不是它把页表写坏了。

至此六条假设全部证伪：

| 假设 | 结果 |
|---|---|
| `nr_present` uint16 回绕 | 证伪（`pt_table_empty` 不看该计数） |
| 跨核 TLB 残留翻译 | 证伪（`-smp 1` 同样崩） |
| 状态路径缺 TLB 事务 | 证伪（`fault_map` 结构相同，也不开事务） |
| 宽限期回收 `mm_pt_retire_table()` | 证伪（状态路径 inert + 预标记开 = stress PASS） |
| 漏记四项缺页统计 | 证伪（补齐后仍崩） |
| metadata 与 PTE 失配 | 证伪（回读断言 0 违例） |

新出现的最强线索来自崩溃转储里的寄存器，之前一直没被利用：

```
regs: a0=0x806000  a1=0x1000
mm:  brk=0x808000  start_brk=0x806000
vma= [0x807000,0x808000) flags=0x13 pte_flags=0xd7 file_fd=-1 off=0x1000
```

`a0=0x806000, a1=0x1000` 的形状与 `mmap(addr, len, ...)` 的前两个参数一致，而
`0x806000` 正好等于 `start_brk`。若被测程序确实在这里做了一次定址 mmap
（`MAP_FIXED`），它就与 brk 区间头部重叠；此时 mmap 路径给 `[0x806000,0x807000)`
打上了 `MM_ST_ANON_VIRT` 标记，而紧邻的 `[0x807000,0x808000)` 是 brk 的 VMA。

由此得到一个能解释全部矛盾的机制：状态路径运行在 `mm_find_vma()` 之前，只认 status。
若某段 status 标记比它描述的 VMA 活得久（VMA 已被拆除/替换，而标记没被清），那么落在
该地址上的后续缺页，包括本该由 brk 逻辑处理的那些，会被状态路径用上一任映射的权限
就地满足，绕过 brk 分支的记账与语义。页表于是被合法地、自洽地写好了（所以断言抓不到），
破坏却发生在语义层而非页表层。

下一步具体且可执行：

1. 找出所有「移除/替换 VMA 但不清 per-PTE status」的路径。已知 `mm_cursor_unmap()`
   只在逐叶子 unmap 时清 `ANON_VIRT`；需要核查的是整段 VMA 被拆除而叶子未被
   逐页 unmap 的情形：定址 `mmap(MAP_FIXED)` 覆盖旧 VMA、`mremap`、`munmap` 掉一段
   从未 fault 过的区间。此时没有叶子可逐，但 status 标记是从 `mm_pt_provision_anon()`
   写下的，不是 fault 写的，这正是漏洞所在。
2. 加一个针对性断言：状态路径命中时，核对该地址当前确实存在一个 VMA；不存在就计数
   并出声。这是对「status 必须与 VMA 生命周期同步」这一不变式的直接检验。

第 2 条尤其关键，它把 §10.7 记录的那个结构性问题（状态路径在 `mm_find_vma` 之前就返回）
从「设计缺陷」落成了一条可机检的不变量。

预标记与状态缺页路径的状态依旧：默认关闭、inert。默认配置
`mm_fault_from_status=0`、审计全 0（含 `safe=0`）、`pt_pages=6` 不变，
`smoke-mm-stress` / `smoke-mm-fork-exec-race` 通过，riscv64/x86_64/aarch64 构建通过。
所有 (b) 排查均在 `a20.anonprov=4096` 下进行。

### 10.30 第七条假设也被证伪；(b) 仍未定位，收尾于此

按 §10.29 的实验设计，在状态路径命中时加了一条断言：该地址当前必须存在 VMA
（`mm_find_vma()` 为空即计数并 `kerr`，因为随后的崩溃会让计数器读不到）。

`a20.anonprov=4096` 实测：孤儿命中 0 次，不变式违例 0 次，崩溃照旧。

per-PTE status 标记并没有比它描述的 VMA 活得久。§10.29 那个「标记比 VMA 长寿」的机制
不成立：定址 mmap / mremap / munmap 未 fault 区间这些路径实际上都清干净了
（`mm_cursor_unmap()` 逐叶子清 `ANON_VIRT` 的行为足够）。

至此七条假设全部被实测证伪：

| # | 假设 | 证伪方式 |
|---|---|---|
| 1 | `nr_present` uint16 回绕 | `pt_table_empty` 直接扫 PTE，不看该计数 |
| 2 | 跨核 TLB 残留翻译 | `-smp 1` 同样崩 |
| 3 | 状态路径缺 TLB 事务 | `fault_map` 结构完全相同，也不开事务 |
| 4 | 宽限期回收 `mm_pt_retire_table()` | 状态路径 inert + 预标记开 = 9287 页、stress PASS |
| 5 | 漏记四项缺页统计 | 补齐后仍崩 |
| 6 | metadata 与 PTE 失配 | 回读断言 0 违例 |
| 7 | status 标记比 VMA 活得久 | 孤儿断言 0 命中 |

两条诊断断言已回退，工作树回到 `c53e0d70` 的干净状态。回退理由不只是「没查到」：
`mm_find_vma()` 那条若留在快路径上，等于把论文最核心的「不查 VMA」又加了回去，它在
概念上就不属于这条路径。这类检查应当放进关机 auditor（像 §10.20 的 `safe_mismatch`
那样），而不是热路径。

到这一步，以下几条都来自实验，而不是推断：

1. mmap 预标记单独安全：当前代码、预标记开、状态路径 inert → 9287 页、审计全 0、
   `mm_stress` PASS、零 FATAL。
2. 崩溃只在状态路径真的消费 mmap 预标记条目时出现。
3. 状态路径的映射正确：回读断言证明 `ANON_VIRT → 已映射` 每次都干净落地。
4. 状态路径从不在没有 VMA 的地址上触发。
5. 崩溃点是 brk 地址，而复现配置里 brk 预标记关闭。
6. 与并发无关（`-smp 1` 同样崩），与回收机制无关（#4），与页表/metadata 一致性无关（#3/#6/#7）。

剩下最值得测的一处结构差异（尚未验证）：状态路径在 `handle_demand_fault_locked` 内
全程持有 `mm->lock`，包括 `pfa_alloc_page()`、`cg_mem_charge()` 和 4 KiB `memset()`；
而 VMA 路径是放锁分配、再重新加锁并重新校验的（fault.c 里那段 `lock_held` +
`mm_find_vma() != vma` 复查）。也就是说状态路径比 VMA 路径多持锁做了一整套内存分配。
这与页表正确性无关，但它是两条路径间最大的行为差异，也是下一轮该量的东西：在
`a20.anonprov=4096` 下若把 `mm->lock` 的持有时间或 `pfa.lock` 的获取顺序打点，
或许能看出它是否触碰了某条未预期的锁序。

结论：(b) 未定位。上面 7 条都是排除法得到的结论，没有一条是根因。继续盲试的边际
收益已经很低，而预标记默认关闭这一状态是安全的：默认配置下 `mm_fault_from_status=0`、
审计全 0（含 `safe=0`）、`pt_pages=6` 不变、`smoke-mm-stress` /
`smoke-mm-fork-exec-race` / `check-mm-lock-model` 全通过、5 个架构与 3 个 NOMMU 变体
构建通过。**状态缺页路径在 (b) 定位之前保持 inert。**

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

`firmware_bootargs()` 读 `0x0000` 拿到 `0x51454d55`（"QEMU"），这说明 fw_cfg 机制本身是通的；
随后读 `0x0014` 拿到 0，于是 `cmdline_size=0`、`cmdline=''`。

原因是 QEMU 的 fw_cfg 文件 selector 是动态分配的，由目录枚举顺序决定。`0x0000` 的签名是
唯一固定的那个；`opt/cmdline` 落在哪个 selector，取决于 QEMU 注册 `etc/`、`opt/` 等目录与
各文件的顺序，没有任何保证是 0x14/0x15。把动态 selector 写死，读到的就是一个不存在的
条目（或别的文件），长度自然是 0。

正确修法是按 fw_cfg 目录格式从 selector `0x0001` 开始枚举，逐级找到 `opt` 目录再找到其下的
`cmdline`，用条目里的 select value 定位后读取。本次**未做**，原因见下：

* selector `0x0001` 处先读一个 0 字节，表示顶层目录结束；
* 每个条目为 `[len+1 字节][名字含结尾 NUL][1 字节 select value][4 字节 value][4 字节 size]`
  （多字节字段均为大端）；
* 目录条目的 `value` 是其内容的基 selector，文件条目的实际 selector 由基址加 select
  value 得到。

不做是因为这是一段引导期代码，跑在 fw_cfg 之上，而我刚把 x86_64 引导修好（§10.10）。
在没有充分上下文验证的前提下写一个不完整的目录枚举器，风险是把刚修好的 x86_64 引导
再次弄坏，而它的收益只是让一个默认安全的策略可配置。留待专门一轮来做，并且必须配
「有/无 `-append` 两种情况下都能启动」的门。

影响范围是明确的：

* x86_64：`a20.wx=` 恒为默认 `deny`（安全，仅不可放宽）；`a20.anonprov=` 无效，故
  x86_64 上无法做交错 A/B（§10.13 的实验只能在 riscv64 上做）。
* 其他架构不受影响：riscv64 经 DTB `/chosen/bootargs` 取命令行，实测可用；aarch64 /
  ppc64le / loongarch32 / riscv32 经 FDT 同理。
* 平台层 `qemu-virt-x86_64/board.c` 在 fw_cfg 为空时会回落到静态 `a20.ip=...` 默认值，
  所以网络配置仍能工作，这也是该缺陷此前没被察觉的原因。

### 10.32 x86_64 命令行：**实测证明 fw_cfg 根本不带文件目录**，目录遍历也救不了

§10.31 定位到 selector 被硬编码，并提出「按目录格式遍历」的修法。这次真的把遍历实现了
（`0x0001` 起按 `len/name/select/value/size` 逐条找 `opt` 再找 `cmdline`），并且按要求先立门：
有 `-append` 与无 `-append` 两种情况都必须能启动。

门通过（`booted=1 panic=0`，两种情况都是），说明遍历是只读的、不会破坏刚修好的引导。
但它找不到 `opt`，于是直接把 `0x0001` 的原始字节打出来看：

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

这是实测而不是推断：在这条 `-kernel <ELF>` 启动路径上，QEMU 根本没有向 fw_cfg 注册任何
文件项。`0x0000` 的签名能读到，说明端口与协议都对，但 `0x0001` 的文件目录是空的。所以：

* §10.31 那个硬编码的 `0x0014/0x0015` 从来就不可能对，它读到的是一个空条目；
* 我实现的目录遍历逻辑正确但无处可施，目录里没有条目可遍历。

因此本次把目录遍历回退了，工作树回到 `d567a4c5`/`3aaf683f` 一线的干净状态。理由不是
「没验证通过」，遍历本身经过「有/无 `-append` 都能启动」这道门，验证是过的；理由是它在这条
启动路径上证明无功能，而它位于引导期代码，留着只会让人误以为命令行可用。一条读不到东西的
代码路径，比没有更糟。

x86_64 命令行的正解不在 fw_cfg，而在启动协议本身：`-kernel <ELF>` 这种直接加载 ELF 的
方式，QEMU 不会像加载 bzImage 或 multiboot 那样把 `-append` 送到内核。要让 `a20.*` 内核
参数在 x86_64 生效，需要改的是启动协议/构建产物（例如改走 multiboot 入口以拿到 multiboot
info 里的命令行，或把参数编进 ELF 的 note 段）。这是一个独立的、更大的改动，与本次
「补全 CortenMM」的主线无关，**不在本次范围内**。

影响与 §10.31 一致，未变：

* x86_64：`a20.wx=` 恒为默认 `deny`（安全，仅不可放宽）；`a20.anonprov=` 无效，故交错
  A/B 的实验（§10.13）只能在 riscv64 上做。
* 其他架构不受影响：riscv64 经 DTB `/chosen/bootargs` 取命令行，实测可用。
* 网络仍能工作：平台层 `qemu-virt-x86_64/board.c` 在拿到空命令行时会回落到静态
  `a20.ip=...` 默认值，这也是这个缺陷一直没被察觉的原因。

### 10.33 **(b) 根因找到并修复**：状态路径装的是内核态 PTE，缺 `PTE_U`

根因是权限位在往返中丢了 user 属性。

`mm_pt_prot_bits()` 只把 PTE 的 R/W/X 三个位搬进状态字节，而 `status_byte()` 把它们
放在 `MM_ST_PROT_R/W/X`（3 位）里，状态字节里没有任何一位表示「用户态」。
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

装上去的是一页仅内核态可访问的映射。缺页处理「成功」返回，用户态下一次访问立刻在刚装好的
那一页上取页故障。

证据链取自 `a20.anonprov=4096`、状态路径可用的那一轮：

1. 临时在状态路径成功分支加 trace。注意：第一次加 trace 时脚本 assert 失败、根本没编进去，
   我却据此得出「状态路径 0 次成功」的结论，又一次把自己的工具故障当成被测系统行为。
   加 trace 后必须先 `grep` 确认它真的在文件里再构建。重做后：

   ```
   [MM-STATUS] ok va=46d000 prot=6      <- 状态路径唯一一次成功
   FAULT-VA] stval=0x46d000             <- 崩溃地址与上面完全相同
   ```

   装映射的地址就是随后取故障的地址，这一条把范围压到「刚装好的那一页权限不对」。

2. `prot=6`：riscv64 上 `PTE_R=1<<1=0x02`、`PTE_W=1<<2=0x04`、`PTE_X=1<<3=0x08`，
   所以 6 = R|W，没有 `PTE_U`（1<<4=0x10）。我一度把 6 误读成「缺 R 位」，那又一次是
   把自己的算术当成结论。

3. 早先的崩溃转储独立佐证：出错页 `flags=0x7` = `PTE_V|PTE_R|PTE_W`，而同一时刻 VMA
   期望的 `pte_flags=0xd7` 里含 `0x10`(U) 与 `0x40/0x80`。差的正是 `PTE_U`。

修法是不让 `PTE_U` 占用状态字节的一位。状态只可能为用户区间记录，因为
`mm_pt_provision_anon()` 会拒绝用户地址空间以外的范围。所以 `PTE_U` 在这里是不变量，
由状态路径显式补上：

```c
allow |= PTE_U;
```

修后实测（riscv64，`a20.anonprov=4096`）：

```
mm_demand_faults:     3411
mm_anon_provisioned:  9287
mm_anon_faults:       2851
mm_fault_from_status: 2842      <- 83% 的 demand fault 不经 VMA
stress-pass: 1   fatals: 0
MM-ASM: missing_meta=0 present=0 absent=0 prot=0 cow=0 vma=0 safe=0 anon_virt=0
```

这是本项目第一次，论文 Fig. 8 的「按状态决策、不查 VMA」的缺页路径真正跑起来：2842 次
缺页直接由 per-PTE 状态满足，不经过 `mm_find_vma()`。此前所有关于它的性能结论都建立在这条
路径从未执行的前提上（§10.6 撤回的 479、§10.8/§10.14 的 14% 与 28%），现在这个前提变了，
那些数字需要重新测量。

同时确认：§10.26/§10.29 的两条诊断断言（回读不变式、孤儿 status）已从热路径彻底移除
（此前 §10.30 声称已回退，实际只回退了 `pt.c`/`perf.h`，`fault.c` 里的块一直还在，其中孤儿
检查还会反过来调 `mm_find_vma()`，与这条路径的设计前提直接冲突；这本身就是一次
「文档与代码不一致」的实例）。

默认行为不变：预标记仍默认关闭，故默认配置下 `mm_fault_from_status` 恒为 0，与改动前完全
一致；本次修复只在显式 `a20.anonprov=<pages>` 时生效。

验证：riscv64 / x86_64 / aarch64 / loongarch64 / ppc64le 与 riscv64 / aarch64 / x86_64 的
NOMMU 全部构建通过；`smoke-mm-stress`、`smoke-mm-fork-exec-race`、`check-mm-lock-model`
通过，审计全 0。

### 10.34 修好 (b) 之后重测：**预标记变成性能中性**，§10.14 的 14% 作废

§10.14 那组「预标记让 mmap 慢约 14%、6/6 对同向」的测量，是在状态路径根本跑不起来
（会崩）的构建上做的。既然 §10.33 修好了 PTE_U，那组数字的前提已经不成立，必须重测。
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

10 项全部没有可分辨的效应。每一项的逐对比值都在 1.0 两侧摆动，没有任何一项出现 6/6 同向
（按 §10.14 定的判据）。所以：

* §10.14 的「mmap 慢 14%」**作废**。那组数据测自状态路径崩溃的构建：预标记写状态的代价
  没变，但当时消费者根本不工作，两组数字不可比。这也是我第三次栽在「拿一个坏掉的构建当
  基线」上。
* 修好之后，预标记在本平台上性能中性，既没有可测的代价，也没有可测的收益。从机制上讲
  这说得通：预标记的 mmap 侧代价（写 per-PTE 状态）现在被缺页侧省下的 `mm_find_vma()`
  抵消掉了，而后者在 TCG 上并不贵。

默认值仍然保持默认关闭，理由是「中性」不等于「更好」。

1. 没有任何证据支持打开它，也同样没有证据说明它有害。按 §10.11/§10.14 的纪律，在能分辨
   1.2× 以内差异的平台上（x86_64/KVM）拿到数据之前，不应把默认配置改成一次可测量的行为变更。
2. 打开预标记会启用一大片平时不走的代码（状态快路径 + UFFD 门 + 宽限期回收）。这些虽然都
   已在 `a20.anonprov=4096` 下验证通过（mm_stress PASS、审计全 0、2842 次状态缺页、零 FATAL），
   但让它们成为默认路径应当是独立的一次决定，而不是顺手改默认值。
3. 想在本平台验证时，用 `a20.anonprov=4096` 显式打开即可；两条路径都已被验证正确。

至此论文的机制第一次真正运行起来：2842/3411 = 83% 的 demand fault 不经 VMA，而在
riscv64/TCG 上它既不更快也不更慢。论文声称的优势是「省掉 Linux 花在 VMA 上的
时间」；在 TCG 上 VMA 查找本就很便宜，因此测不出来。要看到收益需要 x86_64/KVM，
而 x86_64 的命令行仍不可用（§10.31/§10.32：`-kernel <ELF>` 这条启动路径上 QEMU 根本
不向 fw_cfg 注册文件项），这是现在挡住性能收口的最后一道障碍。

### 10.35 x86_64 拿到开关了，于是暴露出**第二个**、且与架构相关的缺口

`PTE_U` 修好后（§10.33），x86_64 仍然拿不到命令行（§10.31/§10.32），所以给 Makefile 加了
`EXTRA_CFLAGS` 逃生口，并让预标记上限支持构建期默认值
（`-DCONFIG_ANON_PROV_DEFAULT=<n>`；`a20.anonprov=<n>` 仍可在有命令行的架构上运行时覆盖）。
这样在 x86_64 上可以构建两张镜像、交替运行，得到与 riscv64 同样的「时间相邻」A/B。

两个开关都验证过（各 5 架构 + NOMMU 构建通过）：

| 镜像 | `mm_anon_provisioned` | `mm_fault_from_status` | `mm_stress` |
|---|---|---|---|
| `DEFAULT=4096` | 53 | **12** | **FATAL(signal=11)** |
| `DEFAULT=0` | 0 | 0 | PASS |

结论一：`PTE_U` 必要但不充分。x86_64 上状态路径已经真的跑起来（12 次），说明 U 位的修复是
通用有效的；但随后仍然崩。

结论二：权限往返是架构相关的，而我按 riscv64 调通后没有再检查别的架构。riscv64 的页表位是
`PTE_R=1<<1 / PTE_W=1<<2 / PTE_X=1<<3 / PTE_U=1<<4`，`allow` 里补 U 就够了。
但 x86_64 的定义完全不同：

```c
#define PTE_V    (1UL << 0)   /* Present */
#define PTE_R    (1UL << 0)   /* Present implies readable —— 与 PTE_V 同一位！ */
#define PTE_W    (1UL << 1)
#define PTE_U    (1UL << 2)
#define PTE_X    (1UL << 11)  /* 软件位；arch_pte_leaf 会清 NX */
#define PTE_USER (PTE_V | PTE_W | PTE_U | PTE_NX | PTE_LEAF)
```

状态路径只用 `MM_ST_PROT_R/W/X` 三个位重建 `allow`，因此丢掉了 `PTE_NX` 与 `PTE_LEAF`。
在 x86_64 上这意味着经状态路径装上的页是可执行的（缺 NX）且缺叶子标记。这与「装完立刻在
该页取故障」的症状一致，也是 `PTE_U` 修好后 x86_64 仍崩的最可能原因。

根本教训，也是本项目第三次同类错误：`PTE_U` 那个 bug 的本质是我把某个架构的 PTE 语义
当成了通用语义。修的时候只验证了 riscv64（唯一能跑命令行的架构），没有在另一个架构上交叉
验证，于是「修好了」只在一个架构上成立。**页表抽象层的任何改动都必须至少在两个语义不同的
架构上验证**，这正是本仓库有 riscv64 / x86_64 / aarch64 的意义。

顺带发现一个独立问题：`mprotect.c:141` 的
`mm_pt_refresh_absent_prot(pte - idx, idx, ptef)` 被 UBSAN 报 pointer-overflow
（`pte - idx` 从表内某个条目反推表基址，在对象边界外做指针运算）。一次 x86_64 运行里出现
35 次。这与 (b) 无关（是 UBSAN 诊断，不是 FATAL 的原因），但它说明「用条目指针反推表基址」
这种写法在指针消毒器眼里始终是越界运算，属于应当改写的脆弱写法（应显式传表基址）。

状态：x86_64 上的 (b)-后续问题未修。`PTE_NX` / `PTE_LEAF` 的缺失是下一步明确的目标，但需要
设计一个跨架构的「权限往返」表示（当前 3 位的 `MM_ST_PROT_*` 在 x86_64 上不足以表达
NX/LEAF），属于对状态字节格式的再一次扩展，与 §10.18/§10.19 记下的「所有 8 位都已用尽」
是同一个约束。顺序是：先在 riscv64 上把跨架构权限往返设计对，再谈 x86_64 复测。

### 10.36 权限往返改为走架构接口（正确的重构，但**没有**修好 x86_64）

§10.35 猜测 x86_64 仍崩是因为状态路径手搓 PTE 位、丢了 `PTE_LEAF`/`PTE_NX`。这个猜测是错的。
仓库里早就有正确的接口：

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

状态路径原先是手搓 `PTE_R|PTE_W|PTE_X|PTE_U`，现在改成把状态里的三个 prot 位还原成 prot
掩码后交给 `mm_prot_to_pte_flags()`。这样任何架构的必需位（`PTE_U`、`PTE_LEAF`、
`PTE_A`、`PTE_MAT1`、NX 语义）与 W⇒R 依赖都由架构自己负责，调用点不再需要知道任何架构
细节。也就是把 §10.33 那个「补 `PTE_U`」的补丁，从架构相关的补丁升格为架构无关的正确写法
（`allow |= PTE_U;` 随之删除）。

验证结果：

* riscv64（`a20.anonprov=4096`）：`mm_fault_from_status=2836`、stress PASS、零 FATAL、
  MM-ASM 全 0，与重构前（2842）一致，无回归。
* x86_64（`CONFIG_ANON_PROV_DEFAULT=4096`）：仍然 FATAL，且 `pc=0x1e2e2`、`signal=11`、
  `pid=6`、UBSAN 35 次，与重构前逐字节相同。

这次重构是对的（架构抽象、去掉架构相关的补丁、riscv64 无回归），但它不是 x86_64 崩溃的
原因。§10.35 关于 `PTE_LEAF`/`PTE_NX` 缺失的推断**被证伪**：崩溃地址、信号、UBSAN 计数在
重构前后完全一致，说明那条路径根本没被触及。

x86_64 的崩溃**仍然未定位**。已知事实：状态路径在 x86_64 上确实跑了 12 次
（`mm_fault_from_status=12`），随后 `mm_stress` 在 `pc=0x1e2e2` 崩溃；关机审计全 0
（`pt_pages=9`、各 mismatch 全 0、`safe=0`），说明元数据与 PTE 没有失配；
同一次运行里有 35 次 `mprotect.c:141` 的 UBSAN pointer-overflow。

`pc=0x1e2e2` 在 `mm_stress` 里，落在用户态。下一步该做的是把 `pc` 解析回源码行，而不是继续
猜权限位。这是连续三次猜权限/记账/回收都落空之后，应当改换的取证方式。

### 10.37 x86_64 崩溃取证：`pc` 解析 + PTE/VMA 权限**不一致**

按 §10.36 写的取证方式（不再猜），把 `pc=0x1e2e2` 解析回源码：

```
pc=0x1e2e2  →  __copy_tls + 0x72   (glibc)
  1e2db: mov 0x5ea6(%rip),%rax   # 24188
  1e2e2: mov %rax,0x0(%r13)      ← 取故障指令
  1e2e9: mov %r13,0x8(%r12)
```

`%r13` 是线程指针 TBLOCK，所以这是一次经线程块的写。崩溃转储随后给出决定性的信息：

```
[FAULT-VA] stval=0x108369f20
  pte value = 0x800000007ee92425     ← bit63 = NX
  leaf: base=0x108369000 pa=0x7ee92f20 pfn=519570 flags=0x425
  page_words: 全 0
  mm: brk=0x1d65000 start_brk=0x1d65000 stack=[0x3fcd1000,0x3fcf1000)
  vma=[0x108349000,0x10836a000) flags=0x13 pte_flags=0x467 file_fd=-1 off=0x2000
```

核心矛盾一处就能定位：

| | 读 | 写 |
|---|---|---|
| PTE（`flags=0x425`） | 有 | **无** |
| VMA（`pte_flags=0x467`） | 有 | **有** |

VMA 说这段可写，PTE 却是只读。用户态的写（`__copy_tls` 往线程块写）落到只读页上，于是取
页故障：这是内核正确拒绝了一次越权写，错的是映射本身与 VMA 不一致。

这指向 mprotect。同一次运行里有 35 次 UBSAN pointer-overflow，位置全部在
`kernel/mm/mprotect.c:141`：

```c
mm_pt_refresh_absent_prot(pte - idx, idx, ptef);   /* 由条目指针反推表基址 */
```

该行属于 mprotect 的「已预留但从未缺页」分支。而本次故障的现象是「VMA 已改成可写、PTE 仍是
只读」，正好是 mprotect 只更新了 VMA、没更新 PTE。

最可能的机制：`mprotect` 里用 `pte - idx` 从叶条目反推表基址，在对象边界外做指针运算
（UBSAN 每次都报），若这个基址算错，`mm_pt_refresh_absent_prot()` 就写到错误的槽位，真正
该改的那一页的 PTE 权限从未被更新。VMA 侧照常更新，于是两者就此分叉。

这条推理把「猜权限位」换成了「一处可指认的不一致」，但根因仍未最终确认。还差一步：确认
mprotect 在已映射页上是否走了另一个分支，以及那 35 次 UBSAN 是否恰好落在本次故障页上。
验证手段（下一步，未做）：在 mprotect 里对「已映射」与「已预留」两条分支各加一个计数/告警，
并打印 `pte - idx` 算出的表基址，与 `pt_lookup_leaf()` 返回的表指针比对。

有一条反直觉之处值得单独点出：这与 §10.33 修的 `PTE_U` 不是同一处。`PTE_U` 那次是
「装出来的页缺 user 位」，症状是刚装完立刻 fault；这次是「PTE 与 VMA 权限分叉」，
且在 riscv64 上不复现（riscv64 走完 2836 次状态缺页、stress PASS、审计全 0）。

状态：x86_64 崩溃仍未修复，但已从「不可观测的 FATAL」收窄到「mprotect 使 PTE 与 VMA 权限
分叉 + `mprotect.c:141` 的指针反推越界」这一处具体矛盾。riscv64 侧功能与性能结论不受影响。

### 10.38 修掉 mprotect 的 NULL 指针运算（真实缺陷，但**不是** x86_64 崩溃的原因）

§10.37 观察到 mprotect 的「已预留未缺页」分支里有一行：

```c
} else {
    int idx = arch_pt_vpn(va, 0);
    mm_pt_refresh_absent_prot(pte - idx, idx, ptef);   /* pte 可能为 NULL */
    va += PAGE_SIZE;
}
```

这个分支的条件是 `!(pte && (*pte & PTE_V))`，所以 `pte` 可能是 NULL（地址根本没有叶子表），
而代码无条件做 `pte - idx`，对 NULL 做指针运算。后果链：

1. `pte - idx` 从 NULL 出发算出野指针（UBSAN 每次都报 pointer-overflow）；
2. `mm_pt_refresh_absent_prot(野指针, …)` 里 `mm_pt_meta()` 对野地址算出的 pfn
   `pfn_valid()` 不成立，于是静默什么都不做；
3. per-PTE 状态因此保留旧权限，而循环末尾的 `v->pte_flags` 照常更新为新权限；
4. 之后状态缺页路径按旧状态装帧 → PTE 与 VMA 权限分叉。

修法很直接：没有叶子表就没有元数据，也就没有状态可刷新，直接跳过。

```c
if (pte) {
    int idx = arch_pt_vpn(va, 0);
    mm_pt_refresh_absent_prot(pte - idx, idx, ptef);
}
```

验证：

| | UBSAN 次数 | `mm_stress` |
|---|---|---|
| 修前 | **35** | FATAL(signal=11, pc=0x1e2e2) |
| 修后 | **2** | FATAL(signal=11, pc=0x1e2e2) |

UBSAN 从 35 降到 2，证明这处 NULL 指针运算确实存在、确实被修掉了。崩溃依旧，且 `pc` 一字
未变。§10.37 那条因果链「状态未刷新 → 装出旧权限 → PTE/VMA 分叉 → 缺页」由此**被证伪**：
它是症状链上的相关项，不是原因（或者至少不是唯一原因）。PTE/VMA 权限分叉这个观察本身仍然
成立，仍然是一个真实的不一致，但导致它产生的机制另有其人。

这一版修的是真缺陷、没修好崩溃。保留它的理由很直接：那处 NULL 指针运算是确凿的 UB，
无论它是不是本次崩溃的成因都该修；不保留它的理由不存在。

x86_64 崩溃仍未定位。已经排除的：权限位集合（§10.36 重构前后逐字节相同）、
`PTE_U`（riscv64 已解决）、状态与 PTE 失配（回读断言 0 违例）、状态比 VMA 活得久
（孤儿断言 0 命中）、mprotect 的 NULL 指针运算（本次修掉，崩溃不变）。仍然成立的现场事实：
状态路径在 x86_64 上跑了 12 次后，`mm_stress` 在 `__copy_tls+0x72`（往线程块写）崩溃，
出错 PTE 只读而其 VMA 可写，关机审计全 0。

下一步的取证应当换成系统化定位，继续猜的边际收益已经很低：在 `__copy_tls` 取故障的那个
地址上，把「该页由谁装上、装的时候用的权限是什么、对应 VMA 的权限是什么」三者一起 dump
出来（页表项、VMA、以及 `pt_meta` 里的 class/prot 字节），一次就能判定是「状态路径装错」
还是「mprotect 改错」还是「别处改错」。这需要在该页上加一次性诊断，而不是继续单点猜测。

riscv64 侧不受影响：5 架构 + 2 NOMMU 变体构建通过，三个门全通过，审计全 0。

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

PTE 与状态彼此一致（都只读），只有 VMA 是可写的。所以问题不是「谁装错了这一页」，而是有人
把 VMA 改成可写、却没有把 PTE 一起改。`acc=1` 是写访问，落在 `__copy_tls` 往线程块写的那条
路上，于是被正确拒绝 → SIGSEGV。

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

循环体只修改 `pt_lookup_leaf()` 返回的那一个 leaf 条目，随后 `va = base + size` 一次跳过该
leaf 的全部条目。只要这个大 leaf 完全落在 VMA 内部（不触发降级），中间的条目就一个都没被改，
而循环外的 `v->pte_flags` 却把整个 VMA 的权限改了。结果就是观测到的形态：VMA 可写、被跳过
的那些页仍是只读。

riscv64 上不复现，是因为那里 `mm_stress` 的 mprotect 区间没有正好套住一个未降级的大 leaf
（或叶子几何不同），所以每一页都走到了「更新 `*pte`」这一步。这与 §10.33 那个
「把 riscv64 的 PTE 语义当成通用语义」是同一类错误的另一个面：只在能跑通的那个平台上验证。

修法（未做，需一并处理大 leaf）：循环必须遍历该 leaf 内的每一个条目，而不是只改第一个。
最小且安全的做法是把 `va = base + size` 改成 `va += PAGE_SIZE`（逐页处理），或者在
`level > 0` 且 leaf 完整落在 VMA 内时先降级再逐页。当前「不跨界就不降级」的优化正是漏洞
来源。

这是本项目里第四次「只在一个架构/一条路径上验证」的教训。前三次分别是热页当缺页（§10.8）、
脚本 bug 当内核回归（§10.15）、把 riscv64 的 PTE 语义当通用（§10.33/§10.36）。共同点是用
单平台、单路径的成功冒充了正确性。

riscv64 侧不受影响：5 架构 + 2 NOMMU 变体构建通过，三个门全通过，审计全 0，
状态路径 2836 次缺页正常。x86_64 崩溃仍未修，但已定位到一处确定的代码缺陷，而不是一个
模糊的症状。

### 10.40 修掉 mprotect 跳过大 leaf 其余条目（真实缺陷，仍**不是**崩溃原因）

§10.39 定位到的机制是：页循环只改 `pt_lookup_leaf()` 返回的那一个 leaf 条目，随后
`va = base + size` 一次跳过该 leaf 的全部条目；而循环外的 `v->pte_flags` 却把整个 VMA 的
权限改了。原来的降级条件只在大 leaf 跨越 VMA 边界时才降级，所以一个完整落在 VMA 内部的
大 leaf 不会被拆开，于是它除第一个条目外的所有页都保留旧权限。

修法：只要 `level > 0` 就先降级，使 `size` 变成单页，`va = base + size` 的推进才成立。

验证：

| | `mm_stress` | 计数 | UBSAN |
|---|---|---|---|
| 修前 | FATAL | 53 / 12 | 2 |
| 修后 | **FATAL** | **53 / 12** | 2 |

崩溃依旧，且所有计数一字未变。§10.39 那条因果链「跳过条目 → VMA 与 PTE 分叉 → 缺页」由此
**被证伪**：PTE/VMA 权限分叉这个观测是真的，但不是由这个跳过来历。

这一版同样是修了真缺陷、没修好崩溃，与 §10.38 那次性质一致：跳条目是确凿的
逻辑错误（无论成因与否都该修），但它不是本次崩溃的根因。

x86_64 崩溃的排查到此为止，因为已经连续四次猜错：

| 假设 | 证伪方式 |
|---|---|
| 权限位集合不对（缺 `PTE_LEAF`/`PTE_NX`） | 重构前后崩溃逐字节相同（§10.36） |
| `PTE_U` 缺失 | riscv64 修好后完全正常（§10.33） |
| 状态未刷新导致装出旧权限（NULL 指针运算） | 修掉后 UBSAN 35→2，崩溃不变（§10.38） |
| mprotect 跳过大 leaf 其余条目 | 修掉后计数一字不变（§10.40） |

仍然确定的事实，全部来自实验而不是推断：

* 状态路径在 x86_64 上确实跑了 12 次才崩（`mm_fault_from_status=12`），riscv64 上跑
  2836 次完全正常；
* 出错现场是一次写访问落在只读 PTE 上，而该 PTE 的 per-PTE 状态也只读
  （`cls=2` ANON_MAPPED、`prot=1`），只有 VMA 是可写（`vpte=0x467`）；
* 关机审计全 0（含新增的 `safe=0`），元数据与 PTE 之间没有失配；
* 与并发无关（`-smp 1` 同样崩）、与预标记本身无关（状态路径 inert 时预标记开 = PASS）。

也就是说，PTE、per-PTE 状态、VMA 三者里有两者一致、一者（VMA）单独不同。前两次修复分别
动了「状态刷新」与「条目遍历」，都没有改变这个形态，说明做出这个差异的不是这两处。下一步
若要继续，应当在写入 VMA `pte_flags` 的每一处加断言（凡是让 VMA 变可写的地方，同时确认
对应叶子已被更新），从「谁改了 VMA」这一侧反查，而不是再从 PTE 侧猜。

riscv64 侧的功能与性能结论完全不受影响：5 架构 + 2 NOMMU 变体构建通过、
三个门全通过、审计全 0、状态路径 2836 次缺页正常、预标记开/关均为性能中性（§10.34）。
所有 x86_64 排查均在 `EXTRA_CFLAGS=-DCONFIG_ANON_PROV_DEFAULT=4096` 下进行。

### 10.41 排查 x86_64 崩溃：已排除的「谁把 VMA 改成可写」候选

现场事实（§10.39，一次三路 dump，全程只触发 1 次）：

```
PTE      flags=0x425          有读，无写
per-PTE  cls=2 prot=1         有读，无写     ← 与 PTE 一致
VMA      vpte=0x467           有读，有写     ← 只有它不同
```

做这个差异的只可能是「把 VMA 改成可写、却没同步 PTE 与状态」的那一方。把 `vma->pte_flags`
的全部写入点列出来逐一排除：

| 写入点 | 是否可能 | 理由 |
|---|---|---|
| `mprotect.c:68` | **排除** | 位于 `#ifdef CONFIG_NOMMU` 内；NOMMU 无页表，只改 VMA 是正确的。有页表时该分支根本不编译。 |
| `mprotect.c:169` | **排除** | 前面有逐页循环会改 PTE；§10.38/§10.40 两次修完，崩溃与计数一字未变。 |
| `mmap.c:163`、`elf.c:170`、`munmap.c:270`、`sysv_shm.c:298`、`framebuffer.c:198/379`、`io_uring.c:146` | **排除** | 全是 VMA 创建点：新建 VMA 时对应叶子尚不存在，之后的第一页要么走 VMA 缺页路径按 VMA 装帧，要么走状态路径按状态装帧。没有任何一条能在「PTE 与状态都已是只读」之后再把 VMA 变可写。 |

由此可以确定地收窄：既不是 mprotect（两个分支都排除），也不是任何 VMA 创建点。剩下的
可能只有两类，且都不是「谁改了 VMA」：

1. VMA 被拆分/合并后 `pte_flags` 取值错误。`mm_split_vma_at()` 与 `vma_try_merge()`
   （mprotect 末尾会调用）会新建或合并 VMA。若合并时从相邻 VMA 抄了 `pte_flags`，而相邻
   那段其实是只读的，就可能造出「本段 RW、叶子 R」的组合。
2. 页被换成了另一个 VMA 覆盖的范围，即该地址后来落进了另一个（可写的）VMA，而页表项
   仍是先前那段留下的只读映射。dump 里 `vstart/vend` 与 `va` 的相对位置值得再核对一次：
   `va` 距 `vstart` 是 `0x20f20`，距 `vend` 是 `0x20e0`，两端都不近，说明 `va` 落在这个
   VMA 的内部而非边界，这一点与「跨界拆分出错」相容。

若继续，该查的是拆分/合并路径，而不是再改 PTE 权限：在 `mm_split_vma_at()` 与
`vma_try_merge()` 里打印新 VMA 的 `[start,end)` 与继承来的 `pte_flags`，并检查它们是否与
该范围内现存叶子的实际权限一致。这是一条此前从未看过的路径。前四次排查全部集中在
「PTE 怎么被写」与「状态怎么被写」，而现场证据表明两者都是自洽的。

x86_64 崩溃仍未修，但排查已从「猜权限位」推进到「VMA 生命周期」，且排除掉了全部显式写入点；
riscv64 侧完全不受影响（5 架构 + 2 NOMMU 变体构建通过、三个门全通过、
审计全 0、状态路径 2836 次缺页正常、预标记开/关性能中性）。

### 10.42 VMA 拆分/合并路径也排除：x86_64 排查到此为止

接着 §10.41 排除全部显式写入点之后，把 VMA 生命周期路径也看了一遍：

`mm_split_vma_at()`（`kernel/mm/vma.c`）用 `*tail = *v` 整体复制父 VMA，再改 `start` 与
`file_offset`。`pte_flags` 是被原样继承的，所以拆分本身不可能造出「一段 RW、一段 R」，
两半拿到的是同一份权限。**排除。**

`vma_try_merge()` → `vma_can_merge()` 有一道显式守卫：

```c
if (a->vm_flags != b->vm_flags || a->pte_flags != b->pte_flags)
    return 0;
```

即 `pte_flags` 不同的两个相邻 VMA 拒绝合并，所以合并也不会把只读段的权限"抹"成可写。
**排除。**

至此，与 VMA 权限有关的全部路径都已排查：

| 路径 | 结论 |
|---|---|
| `mprotect.c:68` | 排除（`#ifdef CONFIG_NOMMU` 内，有页表时不编译） |
| `mprotect.c:169` 的逐页循环 | 排除（两次修复后崩溃与计数一字未变） |
| `mprotect` 跳过大 leaf 其余条目 | 排除（修复后计数一字未变） |
| `mmap/elf/munmap/sysv_shm/framebuffer/io_uring` 的 VMA 创建点 | 排除（创建时叶子尚不存在） |
| `mm_split_vma_at()` | 排除（`*tail = *v` 原样继承 `pte_flags`） |
| `vma_try_merge()` | 排除（`pte_flags` 不同即拒绝合并） |
| per-PTE 状态的写入（provision / refresh / status 路径安装） | 排除（回读断言 0 违例；状态与 PTE 现场自洽） |

x86_64 上的「只读 PTE + 可写 VMA」这个组合，其来源不在上述任何一处。四次猜测、四次证伪，
现场证据（PTE 与 per-PTE 状态自洽、只有 VMA 单独不同）始终指向 VMA 侧，但把 VMA 侧所有写入
路径都排除之后，仍然解释不了。这说明现场 dump 里的那个 VMA 可能根本不是当前 fault 所属的
VMA，即 dump 打印 VMA 的时机或取值有问题，而不是权限真的分叉了。若真如此，之前基于
「PTE/VMA 分叉」的三次推理（§10.37/§10.39/§10.40 的因果链）从一开始就是建立在一个错误的
观测之上。

这是我第一次开始怀疑观测本身，而不是继续怀疑被观测的对象。下一步不该再改代码，而应先验证
这个观测是否可信：在 dump 里把「fault 时的 VMA」与「`mm_find_vma(stval)` 现场返回的 VMA」
同时打印（应当是同一个指针），并把该范围内每一个已映射叶子的实际权限全部列出。若二者一致
且权限真的分叉，则说明存在本文档未记录的第三条写入路径；若不一致，则观测本身是错的，
上面三次因果推理需全部作废。

riscv64 侧完全不受影响：5 架构 + 2 NOMMU 变体构建通过、`smoke-mm-stress` /
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

结论一：`covers=1`，§10.42 的自疑**被推翻**。dump 里的 VMA 确实覆盖 `stval`，观测可信，
「PTE 只读 + VMA 可写」是真的，不是打印时机或取值造成的假象。

结论二也是本次真正的新事实：这是一个 33 页的可读可写匿名 VMA，而它整个邻域里只有一个页是
已映射的，恰好就是出错那一页，且它是只读；其余页 `pte=0`（不存在）。

由此可以一次性结掉整条 mprotect / VMA 生命周期线索，理由是结构性的、不再是猜测性的：

* 出错页在该 VMA 内的偏移是 `0xa473c000 - 0xa471c000 = 0x20000`，即第 9 页（共 33 页），
  既不在边界、也不在任何 split/merge 接缝附近，所以 §10.41/§10.42 排查的
  `mm_split_vma_at()` / `vma_try_merge()` 在此处根本没有生效的机会。
* 邻域 32 页 `pte=0`，意味着 mprotect 的逐页循环在这个范围里无页可改；即使执行过，
  也不可能只把这一页留下旧权限。

「只读 PTE + 可写 VMA」因此不是事后被谁改坏的，而是这一页在被 provision 时就带着只读权限
出生了。六次被证伪的假设（权限位集合 / `PTE_U` / 状态未刷新 / 跳过大 leaf / VMA 创建点 /
VMA 拆分合并）全部属于「事后改坏」这一类，因此全部与本现象无关。

唯一尚未被检查过的路径是 `mm_pt_provision_anon()`（`kernel/mm/pt.c:951`），它在 mmap 时把
一整段匿名范围预标记为 `status_byte(MM_ST_ANON_VIRT, flags)`。缺陷只可能在这两个地方之一：

1. 调用点传入的 `flags` 与覆盖它的 VMA 的 `pte_flags` 不一致，例如按 `PROT_READ` 传参、
   或取了某个默认/模板值，而不是取该 VMA 真实的 `pte_flags`。
2. `status_byte()` 的 class/prot 打包把可写位丢了。

`mm_anon_provisioned=53`（累计）而本 VMA 有 33 页却只落了 1 页，也与 `mm_pt_provision_anon()`
里 `if (span / PAGE_SIZE > g_anon_prov_max) return 0;` 的「太大就不预标记、改走 VMA 缺页路径」
行为一致，说明未预标记的页走 VMA 路径是好的（那条路径从未出问题），只有被预标记的页会带错
权限出生。

下一步明确且从未做过：把 `mm_pt_provision_anon()` 的全部调用点列出来，逐个核对传入的
`flags` 是否等于该范围内 VMA 的 `pte_flags`，并重读 `status_byte()` 的打包逻辑。这是一个
范围极小、可穷举的检查，与前六次「结构上不可能」的排除不同。

本节诊断为临时插桩，已在记录后从 `kernel/mm/fault.c` 移除（`git checkout`），不留调试
打印在缺页热路径上。

> 插桩过程中还暴露了一个小坑：本内核的 `kerr` 不支持 `%+d`，导致 varargs 整体错位
> （首轮输出里 `va=fffffffe` 其实是 `i=-2`）。改成 `%d` 后数据才正确。
> **教训：新增诊断格式串只用 `%d/%x/%lx/%p`，不要用 `+`/`-`/`0` 标志。**

插桩之外的部分没有任何变化：5 架构 + 2 NOMMU 变体构建通过、`smoke-mm-stress` /
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

且 `mm_pt_prot_bits()` 逐位正确打包 `PTE_R/W/X`，`status_byte()` 只做 `cls | COW | prot` 的
组合，不会丢可写位。所以预标记阶段在结构上不可能造成「VMA 可写、状态只读」，三处都把同一个
值同时写进了 VMA 与状态。§10.43 猜测的两个候选（调用点 `flags` 不一致 / `status_byte`
丢位）**双双排除**。

但这一轮读代码带出一个此前没被注意到的、决定性的性质：

```c
int mm_pt_refresh_absent_prot(pte_t *table, int idx, pte_t ptef) {
    ...
    uint8_t cls = MM_ST_GET_CLASS(*slot);
    if (cls != MM_ST_ANON_VIRT)
        return 0;                       /* <-- 关键 */
    *slot = ... mm_pt_prot_bits(ptef);
}
```

而 §10.39 现场读回的状态是 `cls = 2`，即 `MM_ST_ANON_MAPPED`，不是 `MM_ST_ANON_VIRT`。

也就是说，**一个预标记页一旦被 fault 过、状态类从 `ANON_VIRT` 翻成 `ANON_MAPPED`，
它的状态权限位就再也没有任何代码路径能改写了**。唯一会改写状态权限的函数对已 fault 的页直接
early-return。此后该页的权限完全由 PTE 决定，状态权限只是一份不再更新的快照。

这解释了为什么前六次修复全部无效：它们都在试图修「事后改坏」的路径，而现象根本不是事后
改坏的。结合 §10.43 的邻域证据（33 页 VMA 里只有出错那一页已映射、其余 32 页 `pte=0`），
唯一自洽的时序是：

1. 某段匿名范围被预标记，`ANON_VIRT` + 当时的 VMA 权限，此刻状态与 VMA 必然一致；
2. 某次 `mprotect` 把这段 VMA 改成只读。此时页尚未 fault，`mm_pt_refresh_absent_prot`
   生效（`cls` 还是 `ANON_VIRT`），状态跟着改成只读，仍然一致；
3. 该页被 fault，状态类翻成 `ANON_MAPPED`，按当时（只读）的状态装出只读 PTE，仍然一致；
4. 此后该页状态权限被冻结（上面那段 early-return）；
5. 某次 `mprotect` 把 VMA 改回可写。页是 present，走 mprotect 的「已映射」分支，
   应当更新 PTE，但现场 PTE 仍是只读。

全部矛盾因此收敛到第 5 步：一次把已映射页所在 VMA 改成可写、却没有把该页 PTE 改成可写的
`mprotect`。而 §10.38 与 §10.40 两次修 mprotect 后「计数一字未变」，说明这两次修改没有改变
实际执行的代码路径（`level > 0` 从未命中、NULL 指针也从未命中），因而既没修好、也没掩盖。
**真正的缺陷还在 mprotect 里，只是不在我改的那两处。**

下一步因此收窄到一个点、且是可穷举的：审计 mprotect 的「已映射」分支在什么条件下会只改
`v->pte_flags` 而跳过 `*pte` 的写入。候选包括（但需逐个读码确认，不能再猜）：循环条件与
`va` 推进是否在某处提前 `break`/`continue`；`pt_lookup_leaf()` 返回 `level > 0` 时的降级失败
分支；以及 `mm_pte_flags_apply_prot()` 之后是否还有把 `ptef` 重置成只读的第二处赋值。

另外，`mm_pt_refresh_absent_prot()` 那个 `cls != MM_ST_ANON_VIRT` 的 early-return 本身值得
单独审视：它意味着「已 fault 的预标记页」在 mprotect 下完全依赖 PTE 正确，失去了状态这份
冗余。是否应当在 mprotect 的已映射分支里同时刷新状态权限（而非依赖 early-return），属于设计
取舍，需要在定位到第 5 步的真正缺陷之后再决定。

riscv64 侧完全不受影响：5 架构 + 2 NOMMU 变体构建通过、`smoke-mm-stress` /
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

即「mprotect 每次把某个 VMA 变可写时，把该 VMA 的每一个页都扫一遍，若存在已映射且只读的页
就报出来」。

结果：`[MM-MP]` 命中 0 次，崩溃照旧发生（`fatal=1`）。

结合 §10.41（全部显式 `vma->pte_flags` 写入点排除）与 §10.42（拆分/合并排除），VMA 权限
一侧至此被穷尽排除。

#### (b) §10.39 的「状态与 PTE 自洽」是同义反复，不是证据：本节最重要的修正

§10.39 一直把现场读数当作「三向分叉」：`PTE 只读` / `per-PTE 状态只读` / `VMA 可写`，并据此
推出「PTE 与状态一致、只有 VMA 不同」。

但 `mm_pt_query()` 在返回前会用 PTE 覆写状态里的权限位（`kernel/mm/pt.c:1095-1099`）：

```c
uint8_t byte = mm_pt_peek(table, 0, idx);
if (MM_ST_GET_CLASS(byte) == MM_ST_INVALID)
    byte = MM_ST_CLS_BYTE(MM_ST_ANON_MAPPED);
byte = (uint8_t)((byte & (uint8_t)~MM_ST_PROT_MASK) | mm_pt_prot_bits(pte));
```

注意最后一行：`prot` 是从 `pte` 算出来覆盖进去的。查询输出的「状态权限」在定义上就等于 PTE
的权限，**永远不可能与 PTE 不一致**。

于是 §10.39 的「PTE 与 per-PTE 状态自洽」根本不是一条独立证据，而是恒真；「三向分叉」这个
框架从一开始就是伪的。真正存在的分叉只有一个，而且更朴素：

> **该页的 PTE 是只读，而覆盖它的 VMA 是可写。**

这也让 §10.41～§10.44 一连串推理的前提失效：既然没有「状态与 PTE 各自独立地一致」这回事，
那么「状态被冻结」「provision 三处自洽」这些结论虽然各自成立，却都与本崩溃无关。它们描述的
是一条根本没被触发的路径：结论本身不错，但答的不是这个问题。

#### 由此得到的、唯一还站得住的机制

mprotect 不会造成这种分叉（守卫 0 命中），VMA 侧其它写入点也全被排除。那么只读的 PTE 必定
早于那个可写的 VMA 存在，即这一页的 PTE 是更早一次映射留下的残留，而后来有一个 VMA 被建到
了这个地址之上，继承了可写权限。

§10.43 的邻域观测与这个机制高度吻合：33 页的 VMA 里只有出错那一页已映射，其余 32 页
`pte=0`。若 VMA 与 PTE 是同一次建立的，不该只孤零零剩一页；而「一段先前映射的残留 PTE 恰好
落在新 VMA 内部」正好给出这种稀疏分布。

所以排查方向要整个换掉：不再是「谁把 VMA 弄宽了」，而是「哪条路径在建立新 VMA 时，底下还
压着一张旧的、只读的 PTE」。具体要查两件事：

1. VMA 拆除是否把该范围内所有 PTE 都反映射了。`mm_pt_note_absent()` 只在
   `kernel/mm/pt.c:819/830/836` 三处被调用，需核对 munmap/brk-收缩/`vma_release`
   是否都走到了，以及是否有「只清 VMA 不清 PTE」或「只清一部分」的分支
   （尤其 `mm_pt_note_absent()` 里 `if (cls != MM_ST_INVALID && m->nr_present)` 这个
   有条件 early-return）。
2. VMA 新建是否会覆盖到仍存在的 PTE。`mm_pt_provision_anon()` 在 `if (r > 0)`（被更大的
   leaf 覆盖）时是 `continue` 跳过而不做任何处理，需确认 brk 扩张（`munmap.c:262-280`）
   与 ELF 段加载在目标地址上已有 PTE 时的行为。

本节诊断为临时插桩，已在记录后移除（`kernel/mm/mprotect.c` 执行 `git checkout`），不留调试
打印在 mprotect 热路径上。

mprotect 这一轮的插桩同样没有改变 riscv64 侧：5 架构 + 2 NOMMU 变体构建通过、`smoke-mm-stress` /
`smoke-mm-fork-exec-race` / `check-mm-lock-model` 三个门全通过、关机审计全 0（含 `safe=0`）、
状态路径 2836 次缺页正常、预标记开/关性能中性（§10.34）。

### 10.46 拆除路径核对通过，x86_64 排查暂停于「新建 VMA 压着旧 PTE」

按 §10.45 定下的方向，核对「旧 PTE 是否会残留」这条线的拆除侧两端。

`mm_pt_note_absent()`（`pt.c:305-324`）正确。它置 `*slot = 0`（回到 `MM_ST_INVALID`）、按条件
递减 `m->nr_present`、清 COW 位，并且显式清安全位：

```c
/* Safety bits describe the class that was just cleared, so they must go
 * with it -- otherwise a reused slot would inherit a stale UFFD or
 * NO_FA flag and the fault path would make the wrong decision. */
uint8_t *sb = safe_bit(m, idx);
if (sb) *sb &= (uint8_t)~MM_SAFE_MASK;
```

`if (cls != MM_ST_INVALID && m->nr_present)` 这个有条件递减不是漏清的 bug：它只在槽位本来就
持有 class 且计数非零时递减，避免下溢。**排除。**

`mm_cursor_unmap()`（`pt.c:805-840`）也正确。三条分支都把 PTE 与状态一并清掉，且专门覆盖了
「预标记但从未 fault」的页：

```c
if (!(*pte & PTE_V) || !arch_pte_is_leaf(pte)) {
    if (MM_ST_GET_CLASS(mm_pt_peek(table, 0, idx)) == MM_ST_ANON_VIRT)
        mm_pt_note_absent(table, 0, idx);
    return 0;
}
```

**排除。**

「拆除时漏清 PTE / 漏清状态」这一族假设因此在拆除侧已被排除。§10.45 提出的第二条
（**新建** VMA 时底下压着旧 PTE）里，`mm_pt_provision_anon()` 的三个调用点已确认都把同一个
`flags` 同时写进 VMA 与状态（§10.44），而 `mm_pt_provision_anon()` 自身在被更大 leaf 覆盖时
是 `if (r > 0) { mm_cursor_unlock(&cur); continue; }`，只解锁跳过，不做处理。这正是尚未读过的
第三条路径。

排查到此暂停。理由是已经连续七次猜错，且前六次的推理框架（§10.39）事后被证明是错的
（见 §10.45(b)）：在没有新证据来源的情况下继续试第八个假设，重复前七次的错误。应当先把
「新建 VMA 压着旧 PTE」这条尚未读过的代码路径（`mm_pt_provision_anon()` 的 `r > 0` 分支，
以及 mmap/brk/ELF 三个 VMA 创建点在目标地址已有 PTE 时的处理）真正读完，再决定是修代码
还是继续查。

这七次被证伪的假设，及其被证伪的方式，都有记录价值：

| # | 假设 | 证伪方式 |
|---|---|---|
| 1 | 权限位集合不对（缺 `PTE_LEAF`/`PTE_NX`） | 重构前后崩溃逐字节相同（§10.36） |
| 2 | `PTE_U` 缺失 | riscv64 修好后完全正常（§10.33） |
| 3 | 状态未刷新导致装出旧权限（NULL 指针运算） | 修掉后 UBSAN 35→2，崩溃不变（§10.38） |
| 4 | mprotect 跳过大 leaf 其余条目 | 修掉后计数一字未变（§10.40） |
| 5 | VMA 创建点 / 拆分 / 合并造成分叉 | 枚举全部写入点后逐一排除（§10.41/§10.42） |
| 6 | mprotect 把已映射只读页留在可写 VMA 下 | **运行时守卫 0 命中**（§10.45a） |
| 7 | 观察到的「状态与 PTE 自洽」是独立证据 | **同义反复**：`mm_pt_query()` 用 PTE 覆写状态权限（§10.45b） |

其中 #7 推翻了前六次推理的共同前提，是最重要的一条：真正存在的分叉只有一个，即该页 PTE
只读、覆盖它的 VMA 可写，所有关于「per-PTE 状态」的分析都是无关分支。

这一轮同样只动诊断代码：5 架构 + 2 NOMMU 变体构建通过、`smoke-mm-stress` /
`smoke-mm-fork-exec-race` / `check-mm-lock-model` 三个门全通过、关机审计全 0（含 `safe=0`）、
状态路径 2836 次缺页正常、预标记开/关性能中性（§10.34），riscv64 侧结论不变。

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

这个 `continue` 在真正的并发竞争下是无害的（同一段 VMA 刚被 fault 装帧，VMA 与 PTE
本来就该一致）；但在「目标地址上已经存在一张更早的、属于别的映射的 PTE」时是有害的：

* 旧的那张只读 PTE 被原样留下（`continue` 之前没有任何改写）；
* 旧的状态字节也被原样留下（`pt_note_present_meta()` 根本没被调用）；
* 而**新建的 VMA** 已经带着自己的 `pte_flags`（可写）插进了 `mm`：三个调用点
  （`mmap.c:197` / `elf.c:183` / `munmap.c:275`）都是在 `mm_insert_vma()` **之后**才调
  预标记的，所以 VMA 是这段地址上更新、也更权威的那一份声明。

结果就是一张只读的残留 PTE 压在可写的 VMA 之下，正是 §10.45 认定的唯一真实
分叉，也正是 §10.43 邻域观测的形状：33 页 VMA 里只有出错那一页已映射，其余 32 页
`pte=0`（新 VMA 是全新的，本来该一片空白；那孤零零的一页就是残留）。

同一函数里 `if (r > 0)`（被更大的 leaf 覆盖）分支也是 `mm_cursor_unlock(&cur); continue;`，
同样只解锁跳过、不做任何处理，属同一族的漏处理。

七次猜测全部落空、而这一条一次命中，根因就在于前七次都在猜「谁把权限改坏了」，但真相是
「没有人改权限，是一张旧 PTE 从头到尾没被让位」。两者在现象上完全一样（PTE 只读 / VMA
可写），但在代码上分别属于「写」和「漏写」两族。前七次全在查写，自然全查不到。这也解释了
§10.45(a) 的守卫为什么 0 命中：mprotect 确实没留下「已映射只读页」，因为那张只读页根本不是
mprotect 留下的。

#### 修法（尚未实施，需要先定一个语义问题）

直觉修法是「遇到已存在的 leaf 且权限与新 VMA 不一致时，把 PTE 的权限位改写成新 VMA 的
`flags` 并做 TLB 失效」。但这里有一个不能靠直觉决定的安全问题：若那张残留 PTE 是文件映射
（file-backed），保留它的 frame 会把文件内容泄漏进本应匿名的区域，而这正是 `elf.c` 里
`anon` 参数的注释所警告过的同一类问题：

> *Provisioning such a range made the fault path hand out anonymous zero pages instead of
> file content and killed exec with SIGSEGV, so the call site decides.*

所以有三个语义选项，风险差别很大，不应由排查过程单方面决定：

| 选项 | 做法 | 风险 |
|---|---|---|
| **A. 改权限** | 保留 frame，只把 PTE 权限位与状态改写为新 VMA 的 `flags` | 最小改动；但若残留 PTE 是文件映射，会把文件内容暴露给匿名写者——**安全问题，不可默认采用** |
| **B. 先回收再建 VMA** | 在新建 VMA 之前，先把该地址范围上仍存在的 PTE 全部反映射（`mm_cursor_unmap` 已可用） | 语义最干净（残留映射理应消失）；改动面最大，涉及 mmap/brk/ELF 三个创建点 |
| **C. 检测并拒绝** | 发现权限不一致就返回失败，让 mmap/brk/ELF 报错而不是静默继承 | 最安全、绝不引入信息泄漏；但会让某些原本「能跑」的映射开始失败 |

推荐 B：它修的是「VMA 生命周期与页表不同步」这个真正的病根，而不是掩盖症状；且
`mm_cursor_unmap()` 已经能正确清除 PTE 与状态（含预标记未 fault 的分支），可复用。但 B
会改变 mmap/brk/ELF 的行为，需确认「在这些创建点之前，该地址范围上存在 PTE」本身是否就属
非法状态。若属非法，正确做法可能反而是 C。

排查到此结束：根因已定位到具体的三行代码与它所处的语义问题。实施哪种修法需要先回答
「目标地址上已有 PTE 时，正确的行为是什么」，这个问题不该在连续七次猜错之后、由一次仓促的
收尾来决定。

`g_anon_prov_max=0` 是默认值，该路径在默认构建下不激活，riscv64 侧也不受本轮影响：5 架构 +
2 NOMMU 变体构建通过、`smoke-mm-stress` / `smoke-mm-fork-exec-race` / `check-mm-lock-model`
三个门全通过、关机审计全 0（含 `safe=0`）、状态路径 2836 次缺页正常、预标记开/关性能中性
（§10.34）。

### 10.48 §10.47 的根因**被自己的诊断证伪**（第 8 次），排查收束

按 §10.47 的判断加装诊断：在 `mm_pt_provision_anon()` 的逐页循环里，凡因 `table[idx] & PTE_V`
而跳过、且该 PTE 的权限与本次要记录的 `flags` 不一致时，打印 `[MM-PROV]`。

结果：命中 0 次，崩溃照旧（`fatal=1`）。§10.47 **被证伪**。

这条零命中本身是有信息量的，它排除了一整族可能：预标记阶段从来没见过「一张已存在、且权限
与新 VMA 冲突的 PTE」。结合 §10.43 的邻域观测（33 页 VMA 里只有出错那一页已映射，其余 32 页
`pte=0`），可以推出：

> **出错的那一页是在预标记之后才变成"已映射"的**，也就是被某次缺页装上去的。

而 §10.45(a) 已经证明 mprotect 不会把「已映射只读页」留在「可写 VMA」之下。矛盾进一步收紧到
缺页安装这一步。

#### 缺页安装这一步读码的结论

状态路径（`kernel/mm/fault.c:960-993`）完全不查 VMA，权限只从记录的状态字节还原：

```c
if (cls_byte & MM_ST_PROT_R) prot |= 1;
if (cls_byte & MM_ST_PROT_W) prot |= 2;
if (cls_byte & MM_ST_PROT_X) prot |= 4;
pte_t allow = mm_prot_to_pte_flags(prot);
...
mm_cursor_map(&qcur, page_va, pfn_to_phys(np), allow, MM_ST_ANON_MAPPED);
```

这正是论文的设计（SS6.2「省掉 Linux 花在 VMA 上的时间」），也意味着只要该页的状态字节是
「只读」，缺页就一定会装出只读的 PTE，不管 VMA 说什么。而 §10.45(b) 已证明「状态与 PTE
一致」是同义反复，所以现在唯一待解的问题只有一个：

> **谁把该页的状态权限改成了「只读」，却没有同步把 VMA 改回只读？**

写状态权限的函数全仓库只有一个：`mm_pt_refresh_absent_prot()`（`pt.c:857`），它只被 mprotect
的「该页尚未映射」分支调用，且带一道早退：

```c
if (cls != MM_ST_ANON_VIRT) return 0;
```

而 mprotect 的同一个分支会无条件执行 `v->pte_flags = mm_pte_flags_apply_prot(...)`。这两行在
同一次调用里、传的是同一个 `ptef`，因此单次 mprotect 内部不可能分叉。分叉只可能来自两次
不同的 mprotect：前一次把状态刷成只读，后一次把 VMA 放宽回可写却因为上面那道早退（或
`pte == NULL`，`pt.c` 注释所述「没有叶子表就没有状态可刷」）而没有刷状态。

这是本次排查得到的唯一一条尚未被实验排除、且在代码里确有对应早退的机制。但要把它钉死为
根因，还需要一次专门的实验（记录该页的状态字节在 VMA 放宽前后的值），而这已经超出本轮
排查的合理成本。

#### 收束

**连续 8 次猜测全部被各自的诊断证伪。** 其中 §10.45(b) 推翻了前 6 次的共同前提、§10.47
推翻了第 7 次，代价是这些结论虽然各自正确，却都在描述没有被触发的路径。

停止继续猜。理由已经充分：每一次「读码 → 提出假设 → 加诊断 → 证伪」都真实推进了认知
（尤其 §10.45b、§10.47、§10.48 三条排除了大片区域），但也证明这个崩溃无法靠读码猜出来，
它的机制发生在一次具体的运行期事件序列里，必须被记录而不是被推理。下一步唯一合理的做法是：
在 `mm_pt_refresh_absent_prot()` 与 mprotect 的 VMA 改写处，把「该页的状态字节 / VMA 权限 /
事件」按时间序打进环形缓冲，复现后离线读序列，一次性看清 VMA 与状态是在哪两个事件之间分叉
的。这是观测问题，不是推理问题。

当前可交付状态（riscv64 侧，完整且可信）：5 架构 + 2 NOMMU 变体构建通过、
`smoke-mm-stress` / `smoke-mm-fork-exec-race` / `check-mm-lock-model` 三个门全通过、关机审计
全 0（含 `safe=0`）、状态路径 2836 次缺页正常、预标记开/关**性能中性**（§10.34）。所有旧
性能结论已撤回，文档中无残留错误数字。

默认构建不受影响：`g_anon_prov_max = 0`，本崩溃只在
`EXTRA_CFLAGS=-DCONFIG_ANON_PROV_DEFAULT=<n>` 的实验构建下出现。

### 10.49 命中：`vma_file=/bin/mm_stress` 说明这**根本不是**匿名页，COW 才是主线

§10.48 定下的实验（把 `mm_pt_refresh_absent_prot()` 的「没刷成」与「刷成了」区分开，返回 1
表示 declined；mprotect 在「放宽到 RW 却 declined」时报 `[MM-DIV]`）命中了：

```
[MM-DIV] widened v[158d51000,158d72000) to RW but status NOT refreshed
         at va=158d51000 (pte=0)          ... 共 33 次，每页一次
SIGSEGV: pid=6 code=14 sepc=0x1e2e2 stval=0x158d71f20 abi=0
[ERR]   vma_file=/bin/mm_stress
FATAL: pid=6 signal=11 pc=0x1e2e2 comm=mm_stress path=/bin/mm_stress
```

相关性是确凿的，不是巧合：

* `stval=0x158d71f20` 落在 `v[0x158d51000,0x158d72000)` 内，偏移 `0x20f20`，
  即第 32 页（共 33 页），正是该 VMA 的最后一页，与最后一条 `[MM-DIV]`
  （`va=158d71000`）对应；
* `[MM-DIV]` 恰好 33 次，每个地址一次，`pte=0` 即当时连叶子表都还不存在；
* `code=14` → x86_64 错误码 `0xE` = present + write + user，即「该页已映射、可写访问」；
* `vma_file=/bin/mm_stress`：这段 VMA 是可执行文件自己的文件映射，不是堆、不是 brk。

#### 这推翻了前八次排查赖以成立的前提

对 file-backed private 的 VMA，mprotect 故意把 PTE 留成只读并置 `PTE_COW`
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

也就是说，「可写 VMA 之下压着一张只读 PTE」在这种 VMA 上是设计上的正确状态，等第一次写时
由 `handle_cow_fault()` 收尾。

于是 §10.45(a) 那个守卫（「mprotect 放宽到 RW 后，VMA 里若存在已映射只读页就报警」）量的是错的
东西：对文件私有 COW 段，这种情况本来就该发生，守卫理应静默。把它的静默读成「mprotect
无关」，是把符合设计的行为误判成了缺陷。§10.47、§10.48 建立在该结论上的推理也随之失效。

另外，§10.39 读到的 `cls=2 (MM_ST_ANON_MAPPED)` 同样是假象：`mm_pt_query()` 在
`MM_ST_GET_CLASS(byte) == MM_ST_INVALID` 时会凭空合成 `MM_ST_ANON_MAPPED`（`pt.c:1097`），
所以一个没有状态记录的文件页读回来就是 `ANON_MAPPED`。「这是匿名页」这个判断，从一开始就是
查询函数伪造出来的。

#### 真正的问题因此变成一句话

> **写故障落在了文件私有 COW 段的最后一页上，而 `handle_cow_fault()` 没有接手，
> 直接变成了致命 SIGSEGV。**

这与前面八次假设完全不同类：不是「权限被谁改坏」，而是 COW 收尾路径没被走到。

而 `[MM-DIV]` 给出的 `pte=0` 正是关键线索：mprotect 走这一段时，这些页连叶子表都还没有，
所以 mprotect 的「已映射」分支（含上面那段置 `PTE_COW` 的代码）一次都没执行。
这些页后来是由缺页路径装上去的。因此真正的缺陷极可能是：`PTE_COW` 只在 mprotect 的已映射
分支里被设置，而由缺页路径首次装帧的文件私有页没有带上 COW 标记，于是它的第一次写永远等不到
`handle_cow_fault()`，直接致命。

下一步明确、单点、可验证：读 `handle_cow_fault()` 的触发条件，确认它靠什么识别「这是一次需要
COW 的写故障」（是查 `PTE_COW` 位，还是查 VMA 的 `VM_FILE && !VM_SHARED`），再检查缺页安装
文件私有页的那条路径有没有置 `PTE_COW`。若缺，就补上。这既是最小修复，也正好解释了为什么
前八次全错：它们都在查「只读 PTE 从哪来」，而答案是「它本来就该是只读的」，错的是它永远等
不到 COW 收尾。

本节诊断为临时插桩，结论确定后应移除（`kernel/mm/pt.c` 与 `kernel/mm/mprotect.c`）。

本节同样没有改变 riscv64 侧：5 架构 + 2 NOMMU 变体构建通过、三个门全通过、关机审计全 0
（含 `safe=0`）、状态路径 2836 次缺页正常、预标记开/关性能中性（§10.34）。

### 10.50 根因：私有文件缺页装出的页**只读且不带 `PTE_COW`**，第一次写无人接管

三处代码合起来即闭环。

(1) 出错页的 PTE 是 `0x425`，而 `PTE_COW = 1UL << 9 = 0x200`（`x86_64/include/page_table.h:26`），
`0x425 & 0x200 == 0`，`PTE_COW` 没有置。该页 present、user、leaf、只读。

(2) `handle_cow_fault_locked()` 只认两种情况（`fault.c:150` 起）：

```c
if (*pte & PTE_COW) { ... 复制 ... }
if (*pte & PTE_W)    { ... 只补 PTE_D ... }
return -1;                       /* 既非 COW 又不可写 → 放弃 */
```

我们的页既没有 `PTE_COW`、也没有 `PTE_W`，于是落到末尾 `return -1`，写故障无人接管，直接
致命。这与观测完全吻合：`code=14` → 错误码 `0xE` = present + write + user（页确实在、
确实是写）。

(3) 私有文件缺页安装这条路径（`fault.c:428-448`）根本没有 COW 的概念：它老老实实 `memcpy`
出一份私有副本，然后

```c
int r = fault_map(t->mm, page_va, copy, vma->pte_flags, MM_ST_FILE_PRIVATE);
```

就这样把页装上了，没有像 `mprotect.c:120-133` 那样「清 W、置 `PTE_COW`、让第一次写去
`handle_cow_fault()` 收尾」。而 mprotect 那段置 COW 的代码位于它的已映射分支里，本次运行中
这些页 `pte=0`（连叶子表都没有），该分支一次都没执行。

于是链条完整闭合：

> 私有文件段的页由缺页路径首次装帧，而该路径**只给只读、不置 `PTE_COW`**；
> 进程第一次写它，`handle_cow_fault()` 两个分支都不匹配，`return -1`，**致命 SIGSEGV**。

#### 为什么前八次全错

八次假设都在问「这张只读 PTE 是**谁**弄出来的、为什么没跟着 VMA 变可写」。但对文件私有
COW 段，「只读 PTE」本来就是设计要求的中间状态，它必须等 `handle_cow_fault()` 被触发的那一刻
才变可写。真正缺的不是「谁把它改可写」，而是「第一次写时谁来收尾」。问错了问题，八次自然全错。

三处早前的误判也一并得到解释：

* §10.45(a) 守卫静默，因为「可写 VMA 下的只读页」在 COW 段是正常的，守卫量错了对象；
* §10.39 的 `cls=2 (ANON_MAPPED)`，`mm_pt_query()` 在 `cls==MM_ST_INVALID` 时凭空合成
  `ANON_MAPPED`（`pt.c:1097`），文件页因此被误报成匿名页；
* §10.47「预标记跳过已存在 PTE」是真实现象，但那 33 次 `pte=0` 说明这些页当时根本不存在，
  与残留 PTE 无关。

#### 修复方向（明确，但**尚未实施**）

对称于 `mprotect.c:120-133`，在 `fault_map()` 装私有文件页时：清掉 `PTE_W|PTE_D`、置上
`PTE_COW`，让第一次写按既有机制走 `handle_cow_fault()`。这样 COW 的置位点就有两处（mprotect
与缺页），而不是只有 mprotect 一处。后者正是本次缺陷的结构性成因：**COW 标记的建立只挂在
「已映射」路径上，首次装帧的路径被漏掉了。**

实施前必须先确认一件事（本轮上下文已尽，未做）：`fault_map()` 内部把 `vma->pte_flags` 变成
实际 PTE 时，`PTE_W` 是在哪一步被丢掉的。因为 VMA 的 `pte_flags=0x467` 是含 W 的，而装出来的
PTE `0x425` 不含 W。这决定了修复应当落在 `fault_map()` 内部（按 class 处理），还是落在调用点
（传入已清 W 的 flags）。这个定位需要读 `fault_map()`，不应凭猜测下手。

本节诊断插桩（`kernel/mm/pt.c` 的 declined 区分、`kernel/mm/mprotect.c` 的 `[MM-DIV]`）在结论
确定后应移除。

插桩之外 riscv64 侧一切照旧：5 架构 + 2 NOMMU 变体构建通过、三个门全通过、关机审计全 0
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

结论：整条安装链没有任何一处会为私有文件页置 `PTE_COW`。唯一置位点是 `mprotect.c:120-133`
的已映射分支。因此只要一个私有文件段先被 mprotect 放宽权限、之后才首次缺页（本次正是如此：
`[MM-DIV]` 的 33 次全部 `pte=0`），它的页就永远拿不到 `PTE_COW`；而
`handle_cow_fault_locked()` 又只认 `PTE_COW` 或 `PTE_W`，两者皆无即 `return -1`。缺陷的
结构性成因至此完全确认：

> **COW 标记的建立只挂在「页已映射」路径上；「页首次装帧」路径上从未建立。**

#### 仍未解决的一个细节（需下一次实验，不要凭猜测下手）

观测到的 PTE 是 `0x425`（无 `PTE_W`），而 §10.43 读到的那次 VMA `pte_flags=0x467`（含
`PTE_W`）。但这是两次不同的运行，`[MM-DIV]` 那次运行从未打印 `vma->pte_flags`。所以「`PTE_W`
是在哪一步丢的」**尚未确定**，两种可能都还活着：

1. `arch_pte_leaf()` 在 x86_64 上按某种方式滤掉了 `PTE_W`；
2. 调用点传入的 `vma->pte_flags` 在缺页当时本来就不含 `PTE_W`（例如该段先被
   `mprotect(PROT_READ)` 收窄过、随后又被放宽，而 VMA 上的 `pte_flags` 与实际不符）。

决定性实验一次即可：在 `fault.c:445` 的 `fault_map(... MM_ST_FILE_PRIVATE)` 处打印
`vma->pte_flags` 与传入的 `flags`，并同时打印 `PTE_COW`/`PTE_W` 位。缺 `PTE_COW` 这一点已经
确证，`PTE_W` 的去向尚待确证。二者的修法落点不同：若 `vma->pte_flags` 本就含 W 而 PTE 里没有，
修 `arch_pte_leaf()` 或安装点的掩码；若 `vma->pte_flags` 本就不含 W，修 VMA 权限与 `pte_flags`
的同步（另一处 bug）。

因此本节不实施修复。缺 `PTE_COW` 的结论已足够坚实，但把它写进去之前必须先确定 `PTE_W` 的
去向，否则很可能修好 COW 却留下「页仍然只读」的第二个症状，浪费一次验证。

当前状态：诊断插桩仍在 `kernel/mm/pt.c`（declined 区分）与 `kernel/mm/mprotect.c`
（`[MM-DIV]`），**故意保留**，下一次实验要一并复用来打印 `vma->pte_flags`。HEAD `3258ebe3`
已记录根因机制；工作树仅含这两处已知插桩。riscv64 侧不受影响：5 架构 + 2 NOMMU 变体构建通过、
`smoke-mm-stress` / `smoke-mm-fork-exec-race` / `check-mm-lock-model` 三个门全通过、关机审计
全 0（含 `safe=0`）、状态路径 2836 次缺页正常、预标记开/关性能中性（§10.34）。默认
`g_anon_prov_max=0`。

### 10.52 §10.50 的**归因**被证伪（代码缺陷本身仍成立）

§10.51 要求的一次性实验已执行：在 `fault.c` 私有文件缺页的
`fault_map(..., MM_ST_FILE_PRIVATE)` 调用点打印 `vma->pte_flags`、传入的 `flags`、
`PTE_COW`/`PTE_W` 位与当前 PTE（`[MM-FP]`）。

结果：命中 0 次，崩溃照旧（`stval=0xa2194f20`，`code=14`）。也就是说
`fault_map(..., MM_ST_FILE_PRIVATE)` 在整个 `mm_stress` 运行中一次都没有被调用。

§10.50 把这次崩溃归因到「私有文件缺页装出的页不带 `PTE_COW`」因此是错的：出错那一页根本不是
这条路径装上去的。这条路径上「首次装帧不置 `PTE_COW`」的代码缺陷本身依然成立（§10.51 的
读码结论与运行时无关），但它不是本次崩溃的成因。

这次实验同时否掉了「先收紧再放宽导致 `vma->pte_flags` 与实际不符」这条支线：`[MM-FP]` 一次
没打，说明根本没有私有文件页走到缺页，也就无从比较两者。`PTE_W` 的去向问题**仍然悬空**，
而且现在多了一个新事实：出错页是被别的路径装上去的。

#### 目前唯一与运行时无关、且已确证的结论

`handle_cow_fault_locked()` 只认 `PTE_COW` 或 `PTE_W`，两者皆无即 `return -1`；而出错页
`0x425` 两者皆无，VMA 却可写，故第一次写必然致命。这个「COW 页既无 COW 位又不可写 ⇒ 无人
接管」的结构是确证的，但装出这种页的代码路径尚未找到：它不是私有文件缺页（已证伪），也不是
预标记跳过（已证伪）、也不是 mprotect（守卫已证伪）、也不是状态路径（`[MM-PROV]` 已证伪无
权限冲突）。

**这是第 9 次归因失败。** 停止继续猜路径。剩下的可行办法只有一条，且必须是观测：在所有
`mm_cursor_map()` / `mm_cursor_replace()` 的调用点（或在 `mm_cursor_replace()` 内部按 `cls`
打点）记录「该页是由哪个 cls、由哪个调用点装上去的」，复现后直接读出那条路径，而不是继续在
代码里猜。这一步应当在下一次有充足上下文时进行。

插桩现状（故意保留，供下一次实验复用）：`kernel/mm/pt.c`（declined 区分）、
`kernel/mm/mprotect.c`（`[MM-DIV]`）、`kernel/mm/fault.c`（`[MM-FP]`）。三者均为 `kerr`，量级
为每次运行几十行，可接受。riscv64 侧不受影响：5 架构 + 2 NOMMU 变体构建通过、三个门全通过、
关机审计全 0（含 `safe=0`）、状态路径 2836 次缺页正常、预标记开/关性能中性（§10.34）。
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

`stval=0xabd17f20` 的页基址正是 `0xabd17000`，最后一条安装记录就是出错的那一页。

#### 这一条轨迹同时钉死了三件事

1. 它不是 `PTE_COW` 问题。该页 `cow=0` 且 `W=0`，但根因是**权限位本身就少了 `PTE_W`**：
   同一 VMA 的 28 个兄弟页全部以 `flags=0x467`（含 `PTE_W`）装上，只有它以 `0x425` 装上。
   `0x467 & ~0x425 = 0x42`，即只差 `PTE_W`（0x2）与 0x40。§10.50/§10.51 关于
   `PTE_COW` 的整条线索是无关分支，属 `handle_cow_fault()` 那个 `return -1` 造成的误导。

2. 装它的是状态路径（per-PTE status），不是 VMA 路径。状态路径是全仓库唯一
   不从 VMA 取权限、而是把记录的状态字节回环成 PTE 权限的地方
   （`fault.c:968-980`：`prot` → `mm_prot_to_pte_flags(prot)`）：

   ```c
   if (cls_byte & MM_ST_PROT_R) prot |= 1;
   if (cls_byte & MM_ST_PROT_W) prot |= 2;
   if (cls_byte & MM_ST_PROT_X) prot |= 4;
   pte_t allow = mm_prot_to_pte_flags(prot);
   ```

   `mm_prot_to_pte_flags(1)`（只读）正是 `0x425`。而其余 28 页都由 VMA 路径装、
   带着 `0x467`。`mm_fault_from_status=12` 与「唯一一次 `W=0` 的安装来自状态字节」相符。
   （严格确认只需在状态路径上加一个独立标记，成本一次运行。）

3. 真正的分叉是：这一页的 per-PTE 状态记录了「只读」，而它所在的 VMA 是可写。
   状态路径忠实地按状态装了只读页，它没有错；错的是状态本身。

#### 于是问题收敛成一个，且已有唯一候选

「谁把这一页的状态改成了只读，却没有同步 VMA？」

写状态权限的函数**全仓库只有一个**：`mm_pt_refresh_absent_prot()`（`pt.c:857`），
只被 mprotect 的「该页尚未映射」分支调用。而 mprotect 在同一个分支之后会
无条件执行 `v->pte_flags = mm_pte_flags_apply_prot(v->pte_flags, ptef);`。
两者传同一个 `ptef`，所以单次 mprotect 不会分叉；分叉只能来自两次 mprotect：
前一次把状态刷成只读，后一次把 VMA 放宽回可写却没能刷新状态，而它有两条现成的
「刷不成」路径，且 `[MM-DIV]` 已经证明其中一条在本次运行中真实发生过 33 次
（`pte=0`，即 `pt_lookup_leaf` 返回 NULL、调用点直接跳过刷新）：

* `pte == NULL` → 调用点 `if (pte)` 为假，根本没调用刷新（`mprotect.c:162-165`）；
* `cls != MM_ST_ANON_VIRT` → 刷新函数自己 `return 1` 早退（`pt.c:867`）。

#### 修复方向（**证据已足，但本轮不做**）
根本问题是状态与 VMA 之间没有一致性约束：状态路径信任状态字节到「不查 VMA」的程度，
而状态字节又只有 mprotect 会在缺页时刷新、且刷新失败时不报错。两条可选修法：

* **A（治标，最小）**：mprotect 在「放宽为可写却没能刷新状态」时，不要只 `kerr`，
  而是把该页状态改写为 VMA 的权限（或直接把状态置为 `INVALID`，让下一次缺页回落到
  VMA 路径）。语义上等价于「VMA 是权威，状态只是缓存」，与 Linux 一致。
* **B（治本）**：让状态路径在 `ANON_VIRT` 命中时与 VMA 交叉校验，不一致就放弃状态路径、
  回落 VMA 路径。代价是每次状态缺页多一次 VMA 查找，与论文「省掉 VMA 查找」的主张相悖，
  **与本项目目标冲突，不建议**。

推荐 A：它承认了「VMA 权威、状态缓存」这个本该早就成立的约定，
并把当前静默的分叉变成不可能的分叉。
未做完的收尾（下一次务必先做，成本极低）：

1. 在状态路径（`fault.c:993` 附近）加独立标记，确证 `0x425` 那次确实来自它；
2. **移除全部 TEMP 插桩**：`pt.c`（`[MM-INS]`、declined 区分）、
   `mprotect.c`（`[MM-DIV]`）、`fault.c`（`[MM-FP]`）。三处均为热路径 `kerr`，
   修复验证通过后必须清理；
3. 实施 A 之后重跑：期望 x86_64 `mm_stress PASS`、`mm_anon_provisioned=53`、
   `mm_fault_from_status>0`、MM-ASM 全 0；再回归 riscv64 三门与 5 架构构建。

本节同样没有改变 riscv64 侧：5 架构 + 2 NOMMU 变体构建通过、三个门全通过、关机审计全 0
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

1. **`[MM-RF]`**：mprotect 要把这一页的状态刷成 `ptef=0x467`（含 `PTE_W`，可写），
   但 `declined=1` 且 `pte=0`：`pt_lookup_leaf()` 返回 NULL，调用点的
   `if (pte)` 为假，刷新被整段跳过。而循环之后的
   `v->pte_flags = mm_pte_flags_apply_prot(v->pte_flags, ptef)` 照样执行，
   VMA 被放宽成可写。**分叉就在这一行产生。**
2. **`[MM-ST]`**：随后缺页走状态路径，读到 `status_prot=1`（只有 R），
   回环出 `allow=0x425`（无 `PTE_W`）。
3. **`[MM-INS]`**：照此装帧，得到一张只读页，压在可写 VMA 之下。
4. 第一次写 → `handle_cow_fault()` 既无 `PTE_COW` 又无 `PTE_W` → `return -1` → **致命**。

至此 §10.53 的推断**被运行时轨迹逐行证实**：状态路径没有错，是状态陈旧而 VMA 已放宽。

#### 修复（现在可以动手了，方向唯一）

「状态」与「VMA」之间缺少一致性约束。状态路径按设计不查 VMA（这是论文的卖点），
因此它把状态字节当作绝对权威；可是状态字节只在 mprotect 的缺页分支里被刷新，
且刷新失败时不报错、不回退。`[MM-DIV]` 的 33 次 `pte=0` 证明这条「静默失败」在一次运行里
发生了 33 次。

最小且语义正确的修法：在 mprotect 的缺页分支里，当 `declined` 为真时，
不要保留可能陈旧的状态，而是把该页的状态置为 `INVALID`，让下一次缺页回落到 VMA 路径
（VMA 是权威，与 Linux 语义一致）。当 `pte == NULL` 时本就没有元数据可清，
而新建叶子表的元数据本就是 `INVALID`，所以这条路径自动安全；真正需要显式处理的是
`pte != NULL` 但 `mm_pt_refresh_absent_prot()` 因 `cls != MM_ST_ANON_VIRT` 早退的情形。

同时应当把 `declined` 从「静默」变成「有后果」：目前它只 `kerr`，没有任何状态变化，
这正是缺陷得以长期潜伏的原因。

#### 仍未解释的一处

本次运行里 `pte=0` 意味着 mprotect 当时连叶子表都还没有；但状态路径随后却读到了
`status_prot=1`。**即「mprotect 时无叶子表」与「稍后有 R-only 状态」这两件事如何同时成立，
本轮没有查清。** 可能的解释（均未验证）：叶子表是在同一 4K 对齐区域的兄弟页缺页时被顺带
创建的，其元数据里该页仍带着更早一次 `mprotect(PROT_READ)` 刷下的 R；
或 `mm_addrspace_lock()` 在创建叶子表时按某种方式继承了旧元数据。
**这不影响上面的修复方向**（无论叶子表何时出现，「刷新失败就丢弃陈旧状态」都成立），
但它是一个独立于本崩溃的疑点，值得单独查清。

插桩现状（故意保留，供修复验证复用）：`pt.c`（`[MM-INS]`）、`mprotect.c`（`[MM-DIV]`、
`[MM-RF]`）、`fault.c`（`[MM-ST]`、`[MM-FP]`）。修复通过后必须全部移除。

riscv64 侧不受影响：5 架构 + 2 NOMMU 变体构建通过、三个门全通过、关机审计全 0
（含 `safe=0`）、状态路径 2836 次缺页正常、预标记开/关性能中性（§10.34）。

### 10.55 元数据生命周期核对：新建叶子表**不可能**带出陈旧状态，疑点因此收得更紧

§10.54 留下的疑点是：`[MM-RF]` 显示 mprotect 时 `pte=0`（`pt_lookup_leaf()` 返回 NULL），
而稍后的 `[MM-ST]` 却读到了 `status_prot=1`（`ANON_VIRT` + 只读）。
「当时无叶子表」与「稍后有 R-only 状态」如何同时成立？

核对元数据的生命周期（`kernel/mm/pt.c`）：

* `mm_pt_meta()`（`pt.c:180`）按叶子表的 PFN 查 `pfa.meta[pfn].pt`，
  并要求 `pfa.meta[pfn].flags == FRAME_F_PT`；
* `mm_pt_node_init()`（`pt.c:210`）分配元数据后 `memset(m, 0, sizeof(*m))`，
  再在 `pfa.lock` 下发布 `flags = FRAME_F_PT`；
* `mm_pt_node_fini()`（`pt.c:241`）把指针置 NULL、`flags = FRAME_F_ALLOC`，并回收元数据页。

因此新建叶子表的元数据必然是全 0（即全 `MM_ST_INVALID`），
**一个陈旧的 `ANON_VIRT|R` 状态绝不可能来自一张新建的叶子表。** 这条解释被排除。

于是剩下的可能只有两类，且都还没验证：

1. `pt_lookup_leaf()` 返回 NULL 的原因不是「没有叶子表」。这是我此前的误读：
   `[MM-RF]` 的 `pte=0` 只说明返回指针为空，而 `pt_lookup_leaf()` 返回 NULL 也可能是因为
   该地址被一个非 leaf 的上层条目（中间节点或大页）覆盖，或其它查找失败情形。
   若如此，则当时叶子表是存在的，`mm_pt_refresh_absent_prot()` 本该被调用；
   而它被调用却返回 `declined=1`，只可能是走了 `cls != MM_ST_ANON_VIRT` 的早退
   （`pt.c:867`）；但那样状态路径就不会用这份状态了（它要求 `cls == ANON_VIRT`），
   于是又与 `[MM-ST]` 读出 `status_prot=1` 矛盾。**这条也需要查清。**
2. 同一 4K 对齐区域里兄弟页的缺页顺带创建了叶子表，其元数据里该页带着更早一次
   `mprotect(PROT_READ)` 刷下的 R。但这与第 1 条的 `memset` 结论并不冲突，
   因为那张表不是新建的，而是被复用的（其元数据此前已存在且非零）。

注意：这两条都不是「新建表带陈旧状态」，而是「表被复用时元数据未被重置」或
「`pt_lookup_leaf()` 的 NULL 被我误读」。二者都指向同一个需要读的地方：
**`pt_lookup_leaf()` 到底在什么情况下返回 NULL，以及叶子表被复用/释放时元数据是否随之释放。**

这已是一个独立于本次崩溃的、更底层的不变量问题（元数据是否与叶子表生命周期严格绑定），
值得单独查清；但它需要新的一轮完整上下文才能严谨推进。

因此本轮到此为止，不实施修复。理由：§10.54 给出的修复（mprotect 刷新失败就丢弃陈旧状态）
在机制层面已被运行时轨迹逐行证实，但要写对必须先知道
**`pt_lookup_leaf()` 返回 NULL 的确切条件**：`pte=0` 究竟是「无表」还是「被上层条目覆盖」，
决定了该走「新建表路径（自动安全）」还是「复用表路径（需显式重置）」，两种修法完全不同。
在这一点查清之前动手，就是又一次凭猜测下手；本轮已经证伪 9 次，不应再添第 10 次。

交付状态：
* riscv64 侧完整、可用、可信：5 架构 + 2 NOMMU 变体构建通过、`smoke-mm-stress` /
  `smoke-mm-fork-exec-race` / `check-mm-lock-model` 三个门全通过、关机审计全 0（含 `safe=0`）、
  状态路径 2836 次缺页正常、预标记开/关**性能中性**（§10.34）、所有旧性能结论已撤回。
* x86_64 侧机制已闭环、修复待一个前置读码：分叉点精确定位在
  `mprotect.c` 缺页分支的「`declined=1` 却照样 `v->pte_flags = ...`」这一行，
  运行时三行轨迹（`[MM-RF]`→`[MM-ST]`→`[MM-INS]`）逐行证实。
* 工作树含故意保留的诊断插桩：`pt.c`（`[MM-INS]`）、`mprotect.c`（`[MM-DIV]`、`[MM-RF]`）、
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

关键在于：返回 NULL 的条件是「当前这一级的条目没有 `PTE_V`」，而不是「叶子表不存在」。
走到 level 0 时，`table` 就是那张叶子表，只是它的 `table[idx]` 尚未被映射。
所以完全可能出现：**叶子表存在、其元数据里该 idx 带着陈旧的 `ANON_VIRT|R`，
而 `pt_lookup_leaf()` 却返回 NULL。** §10.55 的第 1 条（非 leaf 上层条目导致 NULL）被排除：
被上层条目覆盖时它会正常返回那个 leaf 并给出 `level > 0`，不会返回 NULL。

#### 于是 `mprotect` 里的这句注释与代码都是错的

```c
if (pte) {
    int idx = arch_pt_vpn(va, 0);
    declined = mm_pt_refresh_absent_prot(pte - idx, idx, ptef) != 0;
}
/* 注释原文：「The per-PTE status lives in that table's metadata, so when
 *  there is no table there is no status to refresh.」 */
```

它把「`pt_lookup_leaf()` 返回 NULL」误读成「没有叶子表」，于是整段跳过刷新。
但真实情况是「叶子表在、条目没映射」，而这恰恰是最需要刷新状态的那种情形：
条目没映射说明将来会走缺页路径，而缺页路径读的正是这张表的元数据里那份陈旧的
`ANON_VIRT|R`。跳过刷新 = 把陈旧只读权限留给未来的缺页去执行。

这与 §10.54 的三行运行时轨迹完全吻合，且每一行都得到解释：

```
[MM-RF]  va=8dc0f000 ptef=467 declined=1 pte=0
```

`pte=0` 并不是「没有叶子表」，而是「叶子表在、`table[idx]` 未映射」；
`declined=1` 来自 `if (pte)` 为假而**根本没进刷新**；VMA 却被 `ptef=0x467` 放宽成可写。
于是陈旧的 `ANON_VIRT|R` 留在元数据里 → 缺页时 `[MM-ST] status_prot=1` →
`[MM-INS] flags=0x425` 装出只读页 → 压在可写 VMA 下 → 首次写无人接管 → **致命**。

**结构性根因一句话**：
> **`mprotect` 用「能否取到 PTE 指针」来代替「能否取到叶子表」来判断要不要刷新
> per-PTE 状态**；对「表在、条目未映射」这一最常见的情形，它错误地跳过了刷新，
> 于是陈旧状态被后来的状态缺页照单执行。

§10.38 当初把 `if (pte)` 加上，是为了消除 `pte - idx` 对 NULL 做指针运算的 UBSAN；
方向对，但结论错：真正该做的是「用 `mm_addrspace_lock()` 走到叶子表」，
而不是「PTE 取不到就跳过」。这也是为什么 §10.38 修完 UBSAN 从 35 降到 2、崩溃却丝毫未变。

#### 修复（方向唯一，且已完全确定）

在 `mprotect` 的缺页分支里，不要用 `pt_lookup_leaf()` 的返回值判断，改用能走到叶子表的
路径（例如 `mm_addrspace_lock()` + `cursor_leaf_table()`，或新增一个只下探到叶子表的
helper），然后无条件对 `table[idx]` 调用 `mm_pt_refresh_absent_prot()`。
这样：

* 表在、条目未映射 → 刷新成功，陈旧权限被纠正，**修掉本崩溃**；
* 表不在 → 拿不到元数据，此时新建叶子表的元数据必为全 0（§10.55 的 `memset` 结论），
  状态路径不会命中 `ANON_VIRT`，天然安全；
* `mm_pt_refresh_absent_prot()` 内部那条 `cls != MM_ST_ANON_VIRT` 早退可以保留：
  非 `ANON_VIRT` 的状态本来就不会被状态路径使用，不必在这里强行改写别人的 class。

本轮不做实施：改动需要新增一个「下探到叶子表」的 helper 并处理与 `mm_addrspace_lock()`
的锁序关系（`mm->lock` → 页表锁），属于需要完整上下文才能保证不引入死锁的改动。
但根因、修复方向、以及为什么前九次全错，此刻都已确定到可以直接实施的程度。

前九次为何全错：都在查「状态从哪来」，没人查「状态为什么没被更新」。
状态路径忠实执行状态字节，从不查 VMA，这是论文的设计。真正的契约是
**「mprotect 必须在同一次调用里让状态与 VMA 保持一致」**，而这条契约被一个
「用 PTE 指针的有无代替叶子表的有无」的近似判断破坏了。

插桩现状（故意保留，供修复验证复用）：`pt.c`（`[MM-INS]`）、`mprotect.c`（`[MM-DIV]`、
`[MM-RF]`）、`fault.c`（`[MM-ST]`、`[MM-FP]`）。修复通过后必须全部移除。

### 10.57 **修复完成并验证**：改用 `mm_pt_leaf_table()` 刷新状态

改动在 `kernel/mm/mprotect.c` 的缺页分支：把「取到 PTE 指针」换成「取到叶子表」。

```c
-  if (pte) {
-      int idx = arch_pt_vpn(va, 0);
-      declined = mm_pt_refresh_absent_prot(pte - idx, idx, ptef) != 0;
-  }
+  pte_t *ltab = mm_pt_leaf_table(mm->pgdir, va);
+  if (ltab)
+      (void)mm_pt_refresh_absent_prot(ltab, arch_pt_vpn(va, 0), ptef);
```

关键点在于：仓库里早就有 `mm_pt_leaf_table(pgdir, addr)`（`kernel/include/mm/pt.h:208`，
注释写着「The table that owns the leaf slot for addr … NOT the table `pt_walk()` returns」），
它正是不分配任何东西的「下探到叶子表」，`mm_pt_refresh_absent_prot()` 需要的
`table` 参数可以直接由它给出。所以**真正的缺陷不是「缺一个 helper」，而是
「helper 早就存在，mprotect 没用它」。** 修复因此只有几行，且不引入新锁序、不新增分配：
`mm_pt_leaf_table()` 不进 `mm_pt_read_enter()`，与既有 `pt_lookup_leaf()` 同量级。

顺带清掉了 §10.38 引入的 `if (pte)` 守卫。它当初为消除 `pte - idx` 的 UBSAN 而加，
方向对但结论错，正是它把刷新挡在了门外。

验证全部在无插桩的干净树上完成：

| 项 | 结果 |
|---|---|
| x86_64 `CONFIG_ANON_PROV_DEFAULT=4096` | `mm_stress` **PASS**，0 FATAL，`mm_anon_provisioned=9280`，`mm_fault_from_status=2842` |
| x86_64 `CONFIG_ANON_PROV_DEFAULT=0`（回归） | `mm_stress` **PASS**，0 FATAL |
| MM-ASM 关机审计（两臂） | 全 0，含 `prot=0 safe=0 anon_virt=0` |
| 5 架构（riscv64/x86_64/aarch64/loongarch64/ppc64le） | 0 errors |
| riscv64 + aarch64 NOMMU | 0 errors |
| `smoke-mm-stress` / `smoke-mm-fork-exec-race` / `check-mm-lock-model` | 全 PASS |
| 残留插桩 | 0（`pt.c` / `mprotect.c` / `fault.c` 已清空） |

带插桩那轮还额外确认了修复的判据：`MM-DIV=0`、`declined-refresh=0`、
`read-only installs=0`，三类分叉迹象全部归零。

两处仍未解释的地方：

1. 第一次「干净树」验证其实跑的是过期二进制。移除插桩后 `declined` 变成未使用变量，
   `-Werror` 让构建失败，而 `dev-build` 静默保留了旧的 `kernel.elf`，于是日志里仍有
   `[MM-INS]`。是靠检查日志里残留插桩 + 比对 `.elf` 与源文件 mtime 才发现的。
   教训：`dev-build` 失败时不会自动删除旧产物，必须核对时间戳，不能只看运行结果。
2. 最后那次 riscv64 复跑用的是 `make ARCH=riscv64 run`（默认构建），
   因此 `mm_anon_provisioned=0 / mm_fault_from_status=0`。它验证的是关闭臂，
   **没有覆盖 riscv64 的状态缺页路径**。riscv64 开启臂需带 `a20.anonprov=<n>` 重跑，
   **此项尚未复核**（修复只动 mprotect 的状态刷新，理论上不影响，但不应凭「理论上」记账）。

十次证伪的最终教训：前九次都在问「这份只读权限是**谁写进去的**」，
第十次才问对，即「它为什么没被更新」。状态路径按设计从不查 VMA（论文卖点），
因此真正的契约是「**mprotect 必须在同一次调用里让状态与 VMA 一致**」。
这条契约被一句「取不到 PTE 就没有状态可刷」的近似判断破坏了，而那句注释写得如此笃定，
以至于十次排查里有九次都被它引到了错误的分支上。

### 10.58 补齐 riscv64 开启臂复核：状态缺页路径修复后完好（§10.57 的遗留项已关闭）

§10.57 记录的唯一遗留项是「riscv64 开启臂尚未复核」。现已补齐。

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

两平台的「开启」臂数字高度一致（9280 预标记 / ~2840 状态缺页），且修复前崩溃的平台
x86_64 现在与 riscv64 表现一致，这正是本次修复的目标：消除平台间的行为分歧。
riscv64 开启臂的 2844 与修复前的 2836 几乎相同，说明**修复没有扰动正常的匿名预标记路径**，
只是补上了「状态与 VMA 不一致时的那一次刷新」。

#### 关于 TCG 时长的一点记录

第一次 riscv64 复核（`-m 2G -smp 1`，600s 上限）只跑到 `mm_fault_from_status=797`、
`mm_stress` 未完成；给到 `-m 2G -smp 2`、1500s 后跑完并 PASS。**这是 TCG 慢，不是失败**：
`mm_stress` 本身在 riscv64 上就是最慢的一档。教训是：TCG 上的「没跑完」不能记成「失败」，
也不能记成「通过」，必须区分超时与失败。之前 x86_64 那次「既非 PASS 也非 FATAL」则是另一回事，
那是过期二进制，判据完全不同（见 §10.57 第 1 条）。

至此本次工作收束：x86_64 崩溃已修复并双平台验证，§10.39–§10.58 共十次归因尝试的完整
证据链保留在文档中，包含七次被自身诊断证伪的假设，以及最终靠运行时安装轨迹
（而非读码推理）才定位到真因这一方法论教训。

### 10.59 UFFD「过度清除」的前置条件**已满足**——上锁前必须先修

§10.19 当初把 UFFD 过度清除标为「状态缺页路径启用之前必须先解决」。现在状态缺页路径已经
修好并双平台验证（§10.57/§10.58），那个前置条件已经到达。

#### 缺陷的确切形状

`kernel/ipc/userfaultfd.c:517-532`（unregister）在合并并 unlink 了本次要注销的所有 range 之后，
对合并后的整段 `[rlo, rhi)` 无条件清标记：

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

注释里那句「`userfaultfd_range_present()` 是权威判定，标记陈旧不会让缺页跳过 handler」，
现在对 `ANON_VIRT` 页已经不成立：

* 状态缺页路径 `kernel/mm/fault.c:970` 用的是位：
  `!mm_cursor_safe_test(&qcur, page_va, MM_SAFE_UFFD)`；
* 权威的 `userfaultfd_range_present()` 在 `fault.c:1036`，即 VMA 路径上；
* 而 `ANON_VIRT` 的页在 `fault.c:970` 就被就地满足并返回，根本走不到 1036。

`fault.c:967` 那句注释（「the authoritative userfaultfd_range_present() check still runs on
the VMA path below for **every other case**」）对 `ANON_VIRT` 这一类恰恰是假的：
注释是随状态路径一起写的，写的时候没有意识到这条路径会短路掉权威检查。

后果：若同一页同时被两次注册覆盖，注销其中一次会把 `MM_SAFE_UFFD` 清掉，
此后该页的缺页不会 parked 给仍存活的 handler，而会被状态路径直接满足，
即 §10.19 预言的「仍被注册的页被缺页而不 park」。

#### 缓解（必须说清楚，避免夸大严重性）

默认构建不受影响：实测默认 riscv64 构建 `mm_anon_provisioned: 0`（§10.58），
即预标记默认关闭，状态缺页路径不启用，UFFD 仍由 `fault.c:1036` 的权威检查把关。
该缺口目前仅存在于 `a20.anonprov=<n>` / `CONFIG_ANON_PROV_DEFAULT=<n>` 的实验构建里。
（`MM_ANON_PROVISION_MAX_PAGES = 4096` 与 `CONFIG_ANON_PROV_DEFAULT` 是两个东西：
前者是上限常量，后者才是默认开关，实测默认为 0。）

#### 因此「把预标记默认打开」的前置条件清单

1. 修 UFFD 过度清除：逐页清除并在清之前重新判定 presence。难点是锁序：
   unregister 现在是「放掉 `g_uffd_lock` → 取 `mm->lock`」，若在 `mm->lock` 内再取
   `g_uffd_lock` 做 presence 复查，就形成 `mm->lock → g_uffd_lock` 的新嵌套；
   需先确认全仓库没有「持 `g_uffd_lock` 再取 `mm->lock`」的路径，否则会造出环路。
   **这一步本轮未做，锁序未验证，不应凭猜测下手。**
2. 或者：让状态路径在 `ANON_VIRT` 命中时也调用 `userfaultfd_range_present()`
   （它查的是 range 链表，不是 VMA，比 VMA 遍历便宜得多），代价是每次状态缺页多一次
   加锁查询。这比方案 1 简单，但确实侵蚀论文「缺页不查任何表」的主张。
3. 顺手修正 `fault.c:967` 那句已经不成立的注释。

当前状态是默认构建安全（预标记关闭），UFFD 语义正确。**在 1 或 2 完成之前，
不要把预标记默认打开。**

### 10.60 UFFD 修复的锁序障碍**已排除**（§10.59 的方案 1 现可实施）

§10.59 留下的唯一疑问是锁序：在 `mm->lock` 内再取 `g_uffd_lock` 做逐页 presence 复查，
会不会与某处「持 `g_uffd_lock` 再取 `mm->lock」形成环路。**已查证：不会。**

证据一：`userfaultfd.c` 里没有任何路径在持有 `g_uffd_lock` 时去取 `mm->lock`。
逐个核对 `g_uffd_lock` 的临界区（128-136、146-155、163-171、439-463、496-516、574-587、
681-690），其中没有一处包含 `spin_lock(&mm->lock)`；`mm->lock` 的取用点（209、212、
241、244、421、428、435）全部位于 `g_uffd_lock` 临界区之外。
unregister 更是明确地先放掉 `g_uffd_lock`（第 516 行）再取 `mm->lock`（约 530 行），
两者从不嵌套。

证据二：`mm->lock → g_uffd_lock` 这个顺序早已在本代码库里实际使用。
`kernel/mm/fault.c:1036` 在仍然持有 `mm->lock` 的情况下调用
`userfaultfd_range_present()`，紧接着的下一行才是 `spin_unlock(&mm->lock);`：

```c
if (vma &&
    (vma->vm_flags & (VM_ANON | VM_FILE | VM_VMO | VM_SHARED)) == VM_ANON &&
    userfaultfd_range_present(mm, page_va)) {
    spin_unlock(&mm->lock);
    ...
```

结论：`mm->lock → g_uffd_lock` 是既有且在用的顺序，而反向嵌套全仓库不存在。
因此 §10.59 的方案 1（在 `mm->lock` 内逐页清除 `MM_SAFE_UFFD`、每页清除前重新判定
presence）不引入任何新的锁嵌套，不构成死锁，可以安全实施。

实施要点（供下一轮直接落地）：

1. `mm_pt_set_safe_range()` 增加一个「清 UFFD 标记前先复查 presence」的钩子，或在
   `userfaultfd.c` 里改成逐页处理而非整段一次性清除；
2. presence 复查复用既有的 `userfaultfd_range_present(mm, page_va)`（它查 range 链表，
   已在 `fault.c:1036` 于 `mm->lock` 内被调用，安全）；
3. 顺带修正 `fault.c:967` 那句对 `ANON_VIRT` 已不成立的注释（§10.59 第 3 条）；
4. 验证：需覆盖「同一页被两次注册」的场景。现有 `mm_stress` 未必包含，
   应补一个针对性用例，否则改完也无法证明过度清除已消除。

本轮到此为止：锁序已查清、方案已确定，但**未实施**。实施需要新增逐页复查逻辑并补
一个「双重注册」测试用例，属于需要完整上下文才能一次做对并验证的改动，不宜在收尾阶段动手。

### 10.61 UFFD 过度清除已修复：逐页清除 + 每页复查 presence

按 §10.60 确认的方案 1 实施（锁序已查证为既有的 `mm->lock → g_uffd_lock`，不新增嵌套）。

改动一：新增 `mm_pt_safe_clear_page()`（`kernel/mm/pt.c`，紧邻 `mm_pt_set_safe_range()`）。
整段清位的 `mm_pt_set_safe_range()` 对 `MM_SAFE_UFFD` 不适用：一个页可能同时被另一个
uffd 注册覆盖，整段清会把别人的标记一起清掉。新函数只清一个页：

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

改动二：unregister 改为逐页 + 逐页复查（`kernel/ipc/userfaultfd.c`）：

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

改动三：NOMMU 守卫。`pt.c` 的全部安全位函数位于
`#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)`（`pt.c:33`）之内，
NOMMU 构建下不参与编译，故调用点必须同样加守卫。NOMMU 无页表、也就没有安全位，
不加守卫会得到 `undefined reference to mm_pt_safe_clear_page`。**这是本次改动
唯一一次编译失败，且是靠链接错误而非源码错误暴露的，值得记住。**

改动四：修正 `fault.c` 那句已经不成立的注释（§10.59 第 3 条）。原文声称
「`userfaultfd_range_present()` 是权威判定……对其它所有情况都仍会在下面的 VMA 路径上运行」，
而 `ANON_VIRT` 的页在状态路径里就地满足并返回，根本走不到 `fault.c:1036`。
现已改成直接描述：标记在此处具有权威性、不被重新推导，因此 unregister 有义务
只为「无任何注册仍覆盖」的页清标记。

验证：

| 项 | 结果 |
|---|---|
| 5 架构（riscv64/x86_64/aarch64/loongarch64/ppc64le） | 0 errors |
| riscv64 + aarch64 NOMMU | 0 errors |
| x86_64 开启臂（`CONFIG_ANON_PROV_DEFAULT=4096`） | `mm_stress` **PASS**，0 FATAL，审计全 0 |
| riscv64 开启臂（`a20.anonprov=4096`） | `mm_stress` **PASS**，0 FATAL，审计全 0 |
| `smoke-mm-stress` / `smoke-mm-fork-exec-race` / `check-mm-lock-model` | 全 PASS |

尚未证明的一点：本次修复**没有针对性的回归测试**。要证明「过度清除已消除」，
需要一个「同一页被两次注册、注销其中一次、确认另一注册仍能收到 fault」的用例；
现有 `mm_stress` 是否覆盖这一场景未确认。因此本次验证证明的是
**「改动没有破坏既有行为」**（各门与两个开启臂仍全绿），**不是**「过度清除已被测试证明消除」。
补这个用例是下一轮的明确任务。

### 10.62 UFFD 回归测试：写出来了、能通过，但**查出另一处既有脆弱性**，故暂缓落地

§10.61 明确记下「没有针对性回归测试」是本次修复的短板。测试已写出（`syscall_ext.c` 的
`test_uffd_double_registration()`）：同一页在两个 uffd 上各注册一次 → 注销其中一个 →
另一线程触发读缺页 → **必须仍然被 park**（`read()` 返回 `UFFD_EVENT_PAGEFAULT`）
→ 用存活的 uffd `UFFDIO_COPY` 解决 → 校验内容。若 unregister 仍整段清位，
该页会被状态缺页路径就地满足，`read()` 永远不返回（挂死），测试即失败。

测试本身通过：两次运行都没有打印任何 `uffd2 ...` 失败信息，
`pthread_join` 也正常返回，说明 park → 解决 → 内容校验这条链是通的。

但它暴露了另一处既有问题：启用该测试后，后续一个与 uffd 无关的测试失败：

```
MM_STRESS: evict start
[BCACHE] no evictable page page=5772 valid=2000 dirty=1879 referenced=0 total_refs=0 max_refs=0
MM_STRESS: evict-mmap start
MM_STRESS: FAIL evict-mmap-verify-mapped errno=17      <-- 17 = EEXIST
```

`evict-mmap-verify-mapped` 是对带提示地址的 `mmap` 断言映射成功，拿到 `EEXIST`
即提示地址已被占用。隔离实验确认因果：

| 条件 | 结果 |
|---|---|
| 启用新测试 | `MM_STRESS: FAIL evict-mmap-verify-mapped errno=17` |
| `MM_SKIP_UFFD2=1`（跳过新测试） | `MM_STRESS: PASS` |

因此这不是新测试的缺陷，而是 `evict-mmap` 对地址空间布局/页缓存状态敏感：
新测试多创建一个线程、多做几次 `mmap`/`munmap`，就足以让 `evict-mmap` 的提示地址撞车。
前面那句 `[BCACHE] no evictable page` 也提示回收路径在那一刻本就处于吃紧状态。

处置：不在本轮落地这个测试。理由是不能让默认的 `mm_stress` 变红，
提交一个让既有测试失败的测试，比暂时没有这个测试更糟。`syscall_ext.c` 已回退到
`72117795` 的状态（`git checkout`），工作树干净、构建 0 error。
UFFD 逐页清位这一修复本身已合入并验证（§10.61），缺的只是这个针对性用例。

下一轮的明确任务（有先后顺序）：

1. 先查 `evict-mmap-verify-mapped` 为何用提示地址、以及为何会 `EEXIST`。
   这本身就是一个既有缺陷（与 UFFD 无关），应当独立修掉，而不是靠新测试绕开；
2. 再把 `test_uffd_double_registration()` 落地（可直接复用本次实现：双注册 → 注销其一 →
   断言另一方仍收到 fault → COPY 解决 → 校验内容），并在落地时确认它与 `evict-mmap`
   不再有布局耦合。

方法论记录：这次「写了测试 → 测试通过 → 却发现别处坏了」的循环，
与 §10.39–§10.56 那十次排查是同一个教训的另一个侧面：
**一个新增的测试改变地址空间状态，就足以唤醒此前被掩盖的既有缺陷。**
测试通过 ≠ 周边无问题；测试失败也不必然是测试自己的错。

### 10.63 **更正 §10.62 的误判**：`evict-mmap-verify-mapped` 不是地址冲突，是**内容丢失**

§10.62 把 `MM_STRESS: FAIL evict-mmap-verify-mapped errno=17` 读成「带提示地址的 `mmap`
返回 `EEXIST`」，据此把下一轮任务定为「查提示地址撞车」。**这个读法是错的。**

`user/cmds/stress/mm_stress.c:15` 的 `fail()` 只接收一个字符串，并打印环境里遗留的 `errno`：

```c
static int fail(const char *what)
{
    printf("MM_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}
```

而这一处调用是 `return fail("evict-mmap-verify-mapped");`（`mm_stress.c:1416`），
没有传 errno，所以那个 `errno=17 (EEXIST)` 是早前某次无关系统调用留下的残留值，
与失败原因毫无关系。§10.62 建立在它之上的因果链（「提示地址被占用」）**作废**。

#### 真正的失败性质

`mm_stress.c:1404-1416` 那段循环是逐页内容校验：

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

即：往文件映射里逐页写入一个可预测图案 → `fsync(fd_b)` → 再通过映射读回逐字节比对。
**比对不符 = 映射里的内容与刚写进去的不一致，也就是内容丢失/被写坏。**

这比 §10.62 说的「地址撞车」严重得多：它不是「映射建不起来」，
而是「**已经写好的数据读回来变了**」，且发生在有内存压力、有回收参与的情况下。
紧邻其前的那条消息正指向回收侧：

```
[BCACHE] no evictable page page=5772 valid=2000 dirty=1879 referenced=0 total_refs=0 max_refs=0
```

**`valid=2000`、`dirty=1879`，却「无页可回收」**：在明显应当有可回收页的时刻，
回收路径报出找不到候选。随后映射内容就出现不一致。

#### 目前确定与未确定的

确定：

* 失败是数据不一致，不是系统调用失败；`errno` 字段在此无诊断价值；
* 失败发生在「文件映射 + 内存压力 + 回收」这一组合下；
* 回收侧此前刚报出「无页可回收」。

未确定（本轮未查）：

* 该映射的确切 `mmap` 标志（`MAP_SHARED` 还是 `MAP_PRIVATE`、有无 `MAP_NORESERVE`）。
  §10.62 的 grep 没抓到 `mmap` 调用本身；
* 内容究竟丢在**脏页回写**、**回收/回写（reclaim）**、还是**重新映射（remap）**哪一环；
* 那个 `no evictable page` 判定本身是否就是错的（例如 `referenced=0 total_refs=0`
  是否意味着引用计数统计在丢页路径上没被维护）。

下一轮任务据此更正为（取代 §10.62 的第 1 条）：

1. 读 `evict-mmap` 测试的 `mmap` 调用，确认映射类型；
2. 顺着 `[BCACHE] no evictable page` 的判定条件往回查：为什么 `valid=2000` 却判定无可回收；
3. 再把 §10.62 第 2 条（落地 UFFD 双重注册测试）接在其后。

这一条也说明 `fail()` 的设计有问题：它无条件打印 `errno`，于是大量与 errno 无关的
断言失败都会带上一段误导性的 errno 文本，本次就差点因此把排查方向带偏（先信了 `EEXIST`
整整一节）。要么让它区分「系统调用失败」与「断言失败」，要么在断言类失败时不打印 errno。
这本身是个应当单独修的小缺陷。

### 10.64 `no evictable page` 的诊断数据自相矛盾：2000 个 valid 页，引用计数**全是 0**

§10.63 定下的第 2 步（查 `valid=2000` 却判定无可回收）已经有了决定性的数据。
`kernel/fs/block_cache.c:907-931` 那段诊断在 `pcache_evict_locked()` 返回 NULL 之后，
遍历整个 page pool 统计：

```
[BCACHE] no evictable page page=5772 valid=2000 dirty=1879
                     referenced=0 total_refs=0 max_refs=0
```

* `valid=2000`——池里有 2000 个有效页；
* `dirty=1879`——其中 1879 个是脏的；
* `referenced=0 total_refs=0 max_refs=0`——按 `cache_ref_read(&page->ref)`
  逐页统计，2000 个页里没有任何一个的引用计数大于 0。

矛盾就在这里：若 `pcache_evict_locked()` 的候选判据与这段诊断用的是同一个引用计数，
那么「全部 2000 页都未被引用」应当让它轻松选出 2000 个候选，而不是返回 NULL。
它返回了 NULL，说明二者的判据不一致，即：

> **`pcache_evict_locked()` 认定「不可回收」的理由，并不是这段诊断所统计的那个 `ref` 计数。**

诊断数据本身是可信的（它直接读 `page->ref`，且是在 `bc->lock` 保护下遍历整个池），
所以问题不在引用计数被清零（若真是被清零，反而更好回收）。问题在于淘汰路径认定
这些页不可回收，用的是另一个条件。

最有嫌疑的方向（按可能性排序，均未验证）：

1. 脏页为主 + 回写失败/被跳过。`dirty=1879 / valid=2000`，即 94% 是脏页。
   若 `pcache_evict_locked()` 对脏页要求先成功回写，而此路径下回写失败或被跳过
   （注意调用链上游刚拿过 `bc->writeback_lock` 并做过一次 fill 尝试，
   `block_cache.c:900` 处 `return e`），则所有脏页都会被判为不可回收，
   剩下 121 个干净页又因别的原因落选，最终返回 NULL。
2. 存在 `valid` 但被 pin / 正在被填充（inflight）的页，淘汰扫描显式跳过它们。
3. 扫描范围与诊断范围不同：淘汰可能只扫 `page_no` 附近的一个窗口/时钟环
   （注意形参 `page_no=5772`），而诊断扫的是全池。若淘汰只看某个环/窗口，
   而该窗口恰好全被 pin 住，就会「窗口内无候选」而「全池有 2000 个」。

第 3 条尤其值得先查：形参 `page_no=5772` 被打印出来，说明淘汰是围绕某个页号进行的，
这与「时钟/LRU 环」或「按页号窗口」的实现相符，而诊断统计的是全池。
**若两者范围不同，那么这条诊断信息会系统性地夸大「无可回收」的程度**，
`valid=2000` 这个数字就不足以说明问题严重性，真正该看的是淘汰实际扫描的那个范围里有什么。

#### 这与 §10.63 的内容丢失如何连起来

`evict-mmap` 是 `MAP_SHARED` 文件映射（`mm_stress.c:1320`，`hint=NULL`，顺带再次确认
§10.62「提示地址撞车」的读法是错的）。它在内存压力下逐页写入图案并 `fsync`，
然后通过同一映射读回逐字节比对，结果不一致。

尚未确定内容究竟丢在**回写**、**reclaim**、还是 **remap** 哪一环，
但 §10.64 的数据把嫌疑显著收窄到**回写/淘汰这一侧**：在 94% 的页都是脏页、
且淘汰路径同时报出「无可回收」的情况下，脏数据留在内存里没能落盘，
下一次通过映射读取时读到的就不是刚写进去的内容。

下一轮的具体第一步（明确、可执行）：读 `pcache_evict_locked()`，确认
(a) 它扫描的范围是全池还是某个页号窗口；(b) 对 `dirty` 页是否要求回写成功才可淘汰；
(c) 是否有 pin/inflight 跳过条件。三者任一即可解释「2000 个未引用页却零候选」。

同时值得单独确认：`block_cache.c:900` 那个 `return e` 分支，上游 fill 失败后是否
也直接返回 NULL 而根本没有尝试淘汰。若是，则「无可回收」的诊断信息会误导：
真正的失败原因是**回写/填充出错**，而不是「找不到可淘汰的页」。

### 10.65 根因与修复：`pcache_evict_locked()` 单趟扫描，热的页被跳过后就再也不被看

读了 `pcache_evict_locked()`（`kernel/fs/block_cache.c:801`），§10.64 的矛盾有了解释：

```c
pcache_entry_t *e = bc->page_lru_tail.prev;
while (e != &bc->page_lru_head) {
    if (cache_ref_read(&e->ref) == 0) {
        if (quarantined && e->valid && e->dirty) { e = e->prev; continue; }
        if (e->valid) {
            ...
            if (e->accessed) {
                e->accessed = 0;        /* 跳过，并把 accessed 清掉 */
                ...
                e = e->prev;
                continue;               /* 这一趟不淘汰它 */
            }
            page_lru_remove(e); ... return e;   /* 真正淘汰 */
        }
        ...
    }
    e = e->prev;
}
return NULL;
```

扫描只有一趟。一个 `valid` 且 `ref==0` 的页若 `accessed` 为真，会被跳过并清零
`accessed`，它因此只在「下一趟」才是候选，而这一趟就直接走到头了。

于是在内存压力下恰好必然发生：`evict-mmap` 正在逐页写满一个 `MAP_SHARED` 映射，
每个页都是热的（`accessed=1`）；一轮 LRU 走完把它们的 `accessed` 全清掉，
然后返回 `NULL`，尽管池里有 2000 个未被引用的 valid 页。
这正是 §10.64 那组数据的成因：`valid=2000 dirty=1879 referenced=0` 却「无可回收」，
而诊断代码从来没统计 `accessed`，所以这个自相矛盾的现象在日志里无从解释。

#### 修复

在 `pcache_evict_locked()` 外层加**有界重试**：只要本趟清过 `accessed`（即确实取得了
「让热页降温」这种进展），就再来一趟；没有这种进展才判定真的无候选并返回 `NULL`。
上限 4 趟，用来防止一个页被持续重新访问的池在这里自旋。

```c
for (int pass = 0; pass < 4; pass++) {
    int cleared_accessed = 0;
    ... 原有单趟扫描，accessed 跳过处加 cleared_accessed = 1 ...
    /* 什么都没淘汰，但我们确实让热页降温了：它们现在是候选了。 */
    if (!cleared_accessed)
        break;
}
return NULL;
```

验证在 x86_64/KVM、`CONFIG_ANON_PROV_DEFAULT=4096` 下完成：

| 项 | 修复后 |
|---|---|
| `mm_stress` | **PASS**，0 FAIL，0 FATAL |
| `[BCACHE] no evictable page` 出现次数 | **0**（修复前出现过） |
| MM-ASM 审计 | 全 0 |
| 5 架构构建 | 0 errors |

一处限度：我没有做「修复前 vs 修复后」的 `no evictable page`
计数对照。§10.62 那个失败只在启用 UFFD 双重注册测试时出现，而该测试已被回退；
默认测试序列里该消息是否出现、出现几次，我没有在修复前测量过。
所以准确的表述是：**该函数里确实存在这样一个可证伪的逻辑缺陷**（单趟 + 跳过并清 `accessed`
⇒ 可以在有大量候选时返回 `NULL`），修复消除了它，
并且在本次验证中 `mm_stress` 全绿、该消息计数为 0。
**不能据此宣称「它就是 `evict-mmap` 内容丢失的原因」**，那需要把双重注册测试重新落地、
在同一条件下对照修复前后，才能成立。

#### 下一步（不变，且现在更可行）

`evict-mmap` 这条线尚未被证明与 UFFD 测试的失败同源。§10.63 的第 1 条
（查清 `evict-mmap` 为何失败）**降级为「已定位到一个真实缺陷，但因果未闭合」**。
优先级更高的仍是：**把 `test_uffd_double_registration()` 重新落地**，
然后在同一条件下观察 `evict-mmap` 是否仍然失败，
那才是能同时闭合「UFFD 过度清除已修」与「内容丢失成因」两件事的实验。

### 10.66 因果链闭合：`evict-mmap` 的内容丢失是**页缓存淘汰缺陷**，不是 UFFD 测试的干扰

把 `test_uffd_double_registration()` 重新落地后（同一构建、同一参数），结果：

| 条件 | `evict-mmap` | `no evictable page` | `mm_stress` |
|---|---|---|---|
| 双重注册测试 **启用** + 淘汰修复 **前**（§10.62） | `FAIL evict-mmap-verify-mapped` | 出现 | 红 |
| 双重注册测试 **跳过** + 淘汰修复 **前**（§10.62） | — | — | PASS |
| 双重注册测试 **启用** + 淘汰修复 **后**（本次） | **不失败** | **0** | **PASS** |

**结论：双重注册测试是「触发器」，不是「原因」。**
它多创建一个线程、多做几次 `mmap`/`munmap`，制造了内存压力；
内存压力让 `pcache_evict_locked()` 走进那条「所有 valid 页都热 → 单趟扫描清空 `accessed`
后返回 `NULL`」的路径（§10.65），回收失败 → 脏数据没落盘 → `MAP_SHARED` 映射读回内容不符。
**缺陷一直在那里，只是默认测试序列的压力不足以走到那一步。**

因此 §10.62/§10.63 里「新测试扰动地址空间、撞车导致 `evict-mmap` 失败」的说法是错的，
在此更正：不是**地址布局**被扰动，而是**内存压力**被施加。
（`EEXIST` 那个误导性 `errno` 也一并作废，见 §10.63。）

这一轮的净结果：一个 UFFD 回归测试的落地，顺带挖出并修掉了一个独立的页缓存缺陷。
这正是「测试写出来之后才发现别处坏了」的完整闭环：**两处缺陷现在都修好了，且互相独立。**

#### 全部验证（修复后）

| 项 | x86_64 (KVM) | riscv64 (TCG) |
|---|---|---|
| `mm_stress`（开启臂） | **PASS**，0 FAIL，0 FATAL | **PASS**，0 FAIL，0 FATAL |
| `[BCACHE] no evictable page` | **0** | **0** |
| MM-ASM 关机审计 | 全 0（含 `safe=0`） | 全 0（含 `safe=0`） |
| 5 架构 / riscv64+aarch64 NOMMU 构建 | 0 errors | 0 errors |
| `smoke-mm-stress` / `smoke-mm-fork-exec-race` / `check-mm-lock-model` | 全 PASS | 全 PASS |

UFFD 双重注册测试随 `mm_stress` 常规执行，已在两个平台上真实跑过并通过，
不是被 `MM_SKIP_UFFD2` 跳过的（那个诊断开关已在本次重新落地时去掉了）。

#### 仍然遗留的两点

1. `fail()` 的设计问题（§10.63 末）未修：它无条件打印环境里的 `errno`，
   本次差点让排查整整偏一节（先信了 `EEXIST`）。应区分「系统调用失败」与「断言失败」。
2. `fail()` 之外没有 UFFD 覆盖的说明：本次测试证明的是「注销其一后另一方仍能收到 fault」。
   **反向情形**（注销其一后该页应当不再被 park）没有单独断言，
   那是过度清除的另一半，**尚未被测试覆盖**。

### 10.67 修掉 `fail()` 打印环境 `errno` 的诊断陷阱（171 个调用点受影响）

§10.63 记下「`fail()` 无条件打印 `errno`」这个缺陷，本次修掉。

`user/cmds/stress/mm_stress.c` 的 `fail()` 原本是：

```c
static int fail(const char *what)
{
    printf("MM_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}
```

171 个调用点全部是单参，其中绝大多数（`fail("mseal-mmap")`、`fail("mseal-seal")`、
`fail("mseal-mprotect")`……）是断言或 `ret < 0` 检查，与 `errno` 毫无关系。
于是每一次这类失败都会带上一个环境里遗留的、与原因无关的 `errno`。

它已经造成过实打实的误导（§10.62–§10.63）：一次数据比对失败被打印成
`FAIL evict-mmap-verify-mapped errno=17 (EEXIST)`，据此整整一节都在追一个
根本不存在的「提示地址撞车」，而真实原因是内存压力下的页缓存回收失败。
（对照之下，`syscall_ext.c` 自己的 `fail(what, e)` 是两参、显式传 `errno`、
共 188 个调用点，没有这个问题。**同一个仓库里两套 `fail` 语义不一致**，也是这次混淆的诱因。）

改动是让 `fail()` 不再打印 `errno`：

```c
static int fail(const char *what)
{
    printf("MM_STRESS: FAIL %s\n", what);
    return 1;
}
```

取舍说明：我没有顺手加一个 `fail_ex(what, e)` 并把 171 个调用点逐个分类迁移。
那需要逐个判断「此处失败是否真的源自某次系统调用」，是一次独立的、需要逐点复核的改动，
**在收尾阶段凭猜测分类比不做更糟**（把该报错的地方改成不报错，同样是错的）。
先消除「假信号」这一确定的危害；真正需要 `errno` 的少数站点，应当在**各自被修改时**
显式传递，而不是依赖环境残留值。注释里已写明这个约定。

验证：x86_64/KVM 开启臂 `mm_stress` **PASS**、0 FAIL、0 FATAL、
`no evictable page` = 0、MM-ASM 审计全 0；构建 0 error、无未使用函数告警。

至此本轮收束时，§10.66 列出的两项遗留：

* 本节已修「`fail()` 假 `errno`」；
* **仍遗留**：UFFD 的反向情形未单独断言。本次测试证明的是「注销其一后另一方仍能收到
  fault」，而「注销其一后该页应当不再被 park」是过度清除的另一半，尚未被测试覆盖。

### 10.68 UFFD 反向情形**不需要**测试：它没有可观测的失败模式

§10.67 留下的最后一项是「反向情形未单独断言」。核对代码后结论是
**它构造不出一个会失败的断言**，因此补测试只会得到一个「无论实现对错都通过」的用例。

反向情形是：某页注册过 UFFD、现已注销，其 `MM_SAFE_UFFD` 标记是否已被清掉。
两种实现下的可观测行为是完全相同的：

| 实现 | 注销后该页再缺页 | 结果 |
|---|---|---|
| 标记**已**清掉 | 走状态路径，**不 park**，按状态装帧 | 正常 |
| 标记**残留** | `fault.c:974` 的 `!mm_cursor_safe_test(... MM_SAFE_UFFD)` 为假 → **跳过状态路径**，落回 VMA 路径 | 同样**不 park**，按 VMA 装帧 |

两种情况都不会 park、都能正确完成缺页，因为此时该页确实已无任何注册覆盖，
「不被 park」本就是正确行为。差别只在快慢：标记残留会让这一页永久走不到
per-PTE 状态快路径（每次缺页多一次 VMA 查找），是**性能**问题，不是正确性问题。
所以「该页应当不再被 park」这个断言恒真，写出来不具区分度。

真正的失败方向只有一个，而且已被覆盖：标记**被过度清除**，
即某页仍被另一个注册覆盖、却因别人注销而被清了标记，于是本该 park 的缺页没 park，
handler 永远收不到事件。这正是 `test_uffd_double_registration()` 断言的那一侧
（注销其一 → 断言另一方**仍**收到 `UFFD_EVENT_PAGEFAULT`），且已在两个平台的
开启臂构建上真实跑通。

补充一点：`pt.c:312-323` 的 `mm_pt_note_absent()` 在清 class 的同时清 `MM_SAFE_MASK`，
注释也写明了理由（否则被复用的槽位会继承过期的 UFFD/NO_FA 标志）。
所以标记不会比它描述的映射活得更久，跨映射泄漏的情形也不存在。

结论：UFFD 过度清除的修复与测试覆盖至此完整，
危险方向有断言且已通过，无害方向无断言但也无失败模式。

### 10.69 关于「x86_64 启动协议改造以传递命令行」这一遗留项的决定

上一轮的待办把它列为「IDENTIFIED NEXT PROJECT, not started」。本次重新审视后，
我决定不做这次启动协议改造，理由如下，并且这一决定有赖于本轮已完成的成果：

待办本身描述的是「一条路线」，不是「一个目标」。目标是在唯一能分辨 <1.2x 效应的平台
（x86_64/KVM）上做可信的 A/B 性能测量；「multiboot 入口 / ELF note 传递参数」
只是达成该目标的一种手段。

该目标不需要这次改造即可达成。本轮已经证实：

* `EXTRA_CFLAGS=-DCONFIG_ANON_PROV_DEFAULT=<n>` 能产出两个**都可用**的 x86_64 内核，
  并且两臂都已实测 `mm_stress` **PASS**、MM-ASM 审计全 0（§10.57/§10.66）；
* 因此 A/B 完全可以按「**交错运行两个预构建内核**」的方式做：
  `ON, OFF, ON, OFF, …` 逐轮交替，这与 riscv64 侧用 DTB `a20.anonprov=` 做的
  交错 A/B 是同一种方法，统计口径一致。

而启动协议改造的代价很高：QEMU 的 `-kernel <ELF>` 直启不向 fw_cfg 注册任何文件项，
所以要让 `-append` 落地必须改启动约定（multiboot 入口或携带参数的 ELF note），
这会同时改动**构建、链接、引导路径**三处，并需要自己的一份 gate 来证明
新协议在所有架构上都没被破坏。正是 §10.9 那条「x86_64 阻塞项」当初被判定为
「有意排除在 CortenMM 工作之外」的原因。**在测量需求已能被满足的前提下，
去做一次高风险启动改造是不划算的。**

因此本项的状态应记为「被更高性价比的方案取代」而非「未做」，
真正的遗留是：**x86_64 交错 A/B 测量本身尚未执行**。

#### 该测量尚未执行的确切原因（**更正 §10.69 的一处错误断言**）

本节此前写「基准程序没进镜像」，这是错的。`cortenmm_bench` 确实已被构建：

```
$ ls user/build/x86_64/ | grep -E "mm_stress|cortenmm_bench"
cortenmm_bench
mm_stress
```

错误的原因很单纯：用户命令产物落在 **`user/build/$(ARCH)/`**（`Makefile:160`
`USER_BUILD_DIR = user/build/$(USER_VARIANT)`），而我当时去 `.kernel-build/` 里找，
那个目录只放内核与镜像，自然什么都看不到。「`dev-build` 镜像里找不到该二进制」
这个推论本身也是无效的：`fat32.img` 是运行期磁盘镜像，而用户命令在构建期就装进去了，
二者本来就不该用「文件存不存在」的方式对应起来。

所以 A/B 没有任何实现层面的阻塞，纯属我此前找错了目录。基准接口已确认：

```
usage: cortenmm_bench {mmap|mmap-pf|pf|unmap-virt|unmap|all} [threads]
```

论文 SS6.2 对应的正是前两项：`mmap`（应略慢）与 `mmap-pf`（应更快）。

下一步（明确，已无实现阻塞）：直接执行 x86_64 交错 A/B。

1. 用 `EXTRA_CFLAGS=-DCONFIG_ANON_PROV_DEFAULT=4096` 与 `=0` 各构建一个 x86_64 内核并**分别留存**；
2. 交替运行 `ON, OFF, ON, OFF, …` **至少 6 轮**（两臂必须交错，顺序跑无效，宿主是共享的）；
3. 每轮先 `cat /proc/a20/perf` 预热计数器，再跑 `cortenmm_bench mmap` 与
   `cortenmm_bench mmap-pf`；
4. 报告**中位数与离散度**（不是单次值），并同时给出两平台的
   `mm_anon_provisioned` / `mm_fault_from_status` 作为「该臂确实生效」的证据。

**在拿到这组数之前，不得对 x86_64 的性能下任何结论**，包括不得声称「预标记中性」
（riscv64 上的中性结论不能外推到 x86_64，宿主噪声量级不同）。

### 10.70 x86_64 交错 A/B：**结果是「测不出来」，不是「快/慢」**

第一次在 x86_64/KVM 上真正做了交错 A/B（6 轮 × 2 臂 = 12 次运行，
`ON, OFF, ON, OFF, …` 严格交替，两臂各用 `EXTRA_CFLAGS=-DCONFIG_ANON_PROV_DEFAULT=4096`
与 `=0` 预先构建好的内核，每轮先 `cat /proc/a20/perf` 预热计数器，
再跑 `cortenmm_bench mmap 1` 与 `cortenmm_bench mmap-pf 1`）。

| 基准 | 模式 | ON(4096) 中位数 | OFF(0) 中位数 | 比值 | 组内离散 | **p** |
|---|---|---|---|---|---|---|
| mmap | high | 67046 ns | 65908 ns | 1.017 | ON 45% / OFF 27% | 0.873 |
| mmap | low | 77440 ns | 60206 ns | **1.286** | ON 37% / OFF 20% | **0.078** |
| mmap-PF | high | 155551 ns | 151154 ns | 1.029 | ON 34% / OFF 26% | 0.873 |
| mmap-PF | low | 158772 ns | 149132 ns | 1.065 | ON 26% / OFF 29% | 0.262 |

（Mann-Whitney 双侧正态近似，n=6/6。原始值例如 `mmap low`：
ON `[53675,79398,78493,63457,76387,82593]`、OFF `[66425,55076,59134,54227,61277,62010]`。）

#### 结论必须写成「测不出来」

四个比较没有一个达到 p<0.05，最小的是 `mmap low` 的 p=0.078。
更关键的是组内离散度 20%–45%，也就是说同一臂自己六次运行的波动，
就已经大于绝大多数臂间差异。宿主是共享的（`taskset -c 12-15` 只做了软绑定，
没有独占核、没有裸金属），这个噪声地板远高于本平台本应分辨的 <1.2x 效应。

**因此：关于 x86_64 上的预标记性能，本次不能下任何结论。**
既不能说「中性」，也不能说「mmap 回归 29%」。

#### 两条必须一并记录的观察（都不能当结论）

1. `mmap low` 是唯一像有信号的一项（ON 慢 29%），方向上与论文一致
   （SS6.2：预标记让 mmap 略慢）。但 p=0.078，且两组原始值大幅重叠
   （ON 最低 53675 < OFF 最低 54227，ON 最高 82593 远超 OFF 最高 66425），
   6 个样本不足以支撑它。
2. `mmap-PF` 两项都是 ON 偏慢（1.029 / 1.065），与论文预期的
   「mmap-PF 更快」方向相反；同样不显著，但方向相反这件事本身值得记住：
   它说明**在这份噪声里，连方向都不可信**。

#### 两处方法学缺陷

1. 这 12 份日志本身不能自证「ON 臂确实生效」：我把 `/proc/a20/perf` 重定向到了
   `/dev/null`。生效性是由**产物同一性**确立的：`kern-4096.elf` 与 §10.66 中实测
   `mm_anon_provisioned=9280 / mm_fault_from_status=2842` 的那个构建是同一份，
   且两个 ELF 在**第 542963 字节**不同（正是预标记上限那个立即数所在处）。
   **这个论证成立，但它不等于「日志自证」**，下一轮务必把计数器打印进日志。
2. `mm_stress` 之外的基准未被纳入同一轮的对照，`cortenmm_bench` 的 `low`/`high`
   两种模式各自的内存压力设定也未记录成文。

#### 要真正分辨这个效应，需要什么（不是「再跑几轮」）

* **降噪，而不是加样本**：独占核 / 裸金属 / 关掉同宿主其它负载。
  当前 20%–45% 的组内离散度下，把 n 从 6 加到 60 也只是把噪声看得更清楚，
  不会让信号浮现。
* **改成进程内配对**：在同一进程内交替 ON/OFF（需要运行时可切换，
  这正是 §10.69 认定「不做启动协议改造」时保留的那条路：用
  `a20.anonprov=` 之类的运行时开关，就能在 x86_64 上做到这一点，
  从而把宿主漂移变成两臂共有的噪声）。**这是当前最值得做的一步。**
* 只有在拿到组内离散度降到个位数百分比的数据之后，
  才值得讨论 mmap / mmap-PF 的方向与幅度。

**再次声明**：在拿到这样的数据之前，**不得**在任何总结里写
「x86_64 上预标记性能中性」或「mmap 回归 x%」。此前 §10.33/§10.34 那次
riscv64 上的「中性」结论**只对 riscv64 成立，不能外推**。

### 10.71 新增运行时开关 `/proc/a20/anonprov`，并做**单次启动内配对** A/B

§10.70 判定「要降噪就得让两臂在同一进程内交替」，而此前唯一的开关是**启动时**的
cmdline/编译期常量，这正是 §10.69 那个「启动协议改造」的真正用途。
本轮用远小于启动协议改造的方式补上了它：把上限暴露成一个可写 procfs 节点。

实现在 `kernel/mm/pt.c` 与 `kernel/include/mm/pt.h`：

* `g_anon_prov_max` 连同 `mm_pt_anon_prov_max()` / `mm_pt_set_anon_prov_max()`
  移出 `#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)` 守卫，
  否则 NOMMU 链接时 `undefined reference`（这个坑撞了两次才定位：先是访问器落在守卫内，
  再是变量本身也在守卫内）。它是不依赖页表的普通状态；NOMMU 下写它返回 `-ENOSYS`，
  读它照常返回当前值。
* 读用 `__ATOMIC_ACQUIRE`、写用 `__ATOMIC_RELEASE`。每次 mmap 都会读它，
  这个定序正是「一次启动内反复切换」能成立的前提。

procfs 接口为 `PF_A20_ANONPROV` / `/proc/a20/anonprov`，写入为十进制页数，
范围 `[0, 65536]`，`0` = 关闭。读回与写入已实测：`0 → 写 4096 → 4096 → 写 0 → 0`。

验证：5 架构 + riscv64/aarch64 NOMMU 构建 0 error。

#### 单次启动内配对 A/B（8 轮，严格 ON/OFF 交替，宿主漂移成为两臂共有噪声）

| 基准 | 模式 | ON(4096) | OFF(0) | 比值 | ON 离散 | OFF 离散 | **p** |
|---|---|---|---|---|---|---|---|
| mmap | high | 61174 ns | 56398 ns | 1.085 | 21.3% | 30.5% | 0.248 |
| mmap | low | 62508 ns | 58542 ns | 1.068 | 50.0% | 37.2% | 0.318 |
| mmap-PF | high | 140115 ns | 165997 ns | **0.844** | 43.3% | 34.8% | 0.093 |
| mmap-PF | low | 146172 ns | 165292 ns | **0.884** | 32.0% | 28.8% | 0.074 |

本次日志自证了 ON 臂生效（补上了 §10.70 的方法学缺陷 1）：
`mm_anon_provisioned: 19467`、`mm_fault_from_status: 8859`。

#### 两条结论，方向相反，都必须写下来

(1) 我的降噪预期是错的。§10.70 断言「进程内配对能把宿主漂移变成两臂共有的噪声」，
实测没有发生：配对后的组内离散度是 21%–50%，与跨启动 A/B 的 20%–45%
基本相同，一点也没有降。`mmap low` 的 ON 离散甚至达到 50%。
这说明主导噪声的不是「两臂之间的宿主漂移」，而是每次测量内部的宿主噪声
（同宿主其它负载抢占、cache 抖动、TCG/KVM 翻译缓冲抖动），
配对法原理上就治不了它。**这条预期被自己的实验证伪，应当作废。**

(2) 但方向第一次与论文一致。本次 `mmap-PF` 两项在 ON 下更快
（0.844 / 0.884），正是论文 SS6.2 预期的方向；而 `mmap` 两项在 ON 下略慢
（1.085 / 1.068），也正是论文预期的方向。
**两项同时与理论同号，这是此前两轮都没出现的。**
（对照：跨启动 A/B 里 `mmap-PF` 是 1.029/1.065，方向与理论相反，说明那一轮是噪声。）

然而仍然不能下结论：四项 p 值最小只有 0.074，没有一项达到 0.05；
组内离散 21%–50% 依旧远大于臂间差异 6%–16%。
**「方向一致」可以是真实弱效应 + 大噪声，也可以是巧合。n=8 分不开。**

#### 结论（与 §10.70 同样的纪律）

x86_64 上的预标记性能依然无法判定。本轮把「能不能配对测量」这个工程问题解决了
（用 40 行代码替代了一次高风险启动协议改造），但没有解决「能不能测出来」这个物理问题。
现在可以确定：**这台共享宿主测不出来**，无论用哪种设计。

要再进一步只剩两条路，都不是「多跑几轮」：

1. 换测量环境：独占核（`isolcpus` + `nohz_full` + 绑核）甚至裸金属。
   当前噪声地板下任何统计都无法给出结论。
2. 换被测量：`cortenmm_bench` 的 `mmap`/`mmap-pF` 单次只有十几毫秒
   （`1T=0.02s` 量级），样本本身就太短，一次调度抢占就能吞掉它。
   应当把每次迭代放大到**数百毫秒以上**（增大映射规模或迭代数），
   让单次测量的时长超过噪声的相关时间。这比换宿主便宜得多，且当前就该先做。

在拿到组内离散降到个位数百分比的数据之前，
**不得**声称「x86_64 上预标记性能中性」，也**不得**声称「已复现论文的 mmap-PF 加速」。

### 10.72 第二个降噪假设**同样被证伪**，但短样本的「大幅效应」被证明主要是假象

§10.71 给了两条路，第二条是「换被测量」：当时 `cortenmm_bench` 每次测量只有
`1T≈0.02s`，**一次调度抢占就能吞掉整个样本**。本轮把每次迭代放大到数百毫秒
（`ROUNDS` 由编译期常量改为 `argv[3]` 可调，同时修正 `ops_out` 用的是同一个值，
避免 `ops/s` 被算错），样本时长从 **0.02s → 0.21s（约 10 倍）**，
单次启动内配对 A/B，10 轮。

| 基准 | 模式 | ON(4096) | OFF(0) | 比值 | ON 离散 | OFF 离散 | **p** |
|---|---|---|---|---|---|---|---|
| mmap | high | 52162 ns | 51545 ns | 1.012 | 64.2% | 14.8% | 0.257 |
| mmap | low | 53316 ns | 51550 ns | 1.034 | 59.0% | 11.8% | 0.070 |
| mmap-PF | high | 138360 ns | 139192 ns | 0.994 | 32.7% | 58.0% | 0.326 |
| mmap-PF | low | 138338 ns | 145650 ns | 0.950 | 35.1% | 71.7% | 0.290 |

日志自证 ON 臂生效：`mm_anon_provisioned: 331536`、`mm_fault_from_status: 164676`。

#### 结论一：加长样本**没有**降低噪声地板（我的第二个假设也被自己的实验证伪）

离散度 32%–72%，比短样本那轮（21%–50%）**更差**，`mmap high` 的 ON 离散高达 64%。
至此两条降噪路线都失败了：

| 尝试 | 预期 | 实测 |
|---|---|---|
| 跨启动交错（§10.70） | 基线 | 20%–45% |
| 单次启动内配对（§10.71） | 宿主漂移变成共有噪声 | 21%–50%（**没降**） |
| 样本 ×10 加长（§10.72） | 样本时长超过噪声相关时间 | 32%–72%（**更差**） |

结论应当写死：这台共享宿主测不出 <1.2x 的效应，与测量设计无关。
主导噪声是宿主本身（其它租户抢占、cache/THP 抖动、KVM 翻译缓冲），
它在每一次测量内部起作用，因此任何跨测量的统计设计都治不了它。

#### 结论二：但短样本那一轮的大幅效应，基本是**样本太短造成的假象**

把三轮点估计并排看，比值随样本变长系统性地向 1.0 收敛：

| 基准/模式 | 20ms 样本(§10.70) | 210ms 样本(本次) |
|---|---|---|
| mmap low | **1.286** | **1.034** |
| mmap-PF high | 1.029（跨启动）/ 0.844（配对） | **0.994** |
| mmap-PF low | 1.065（跨启动）/ 0.884（配对） | **0.950** |
| mmap high | 1.017 | **1.012** |

§10.70 那个「mmap 回归 29%」在样本长 10 倍后缩到 **3.4%**。
**这验证了当时「不把它当结论」是对的**，它确实只是噪声。
同时，§10.71 里那个「两项同时与论文同号、可能是真实弱效应」的乐观读法
**也应当收回**：那些同号在大样本下并没有变强，而是**一起塌回 1.0 附近**。
两组符号一致，原来更可能是巧合。

#### 仍然不能下的结论

p 值最小 0.070，没有一项达到 0.05；离散 32%–72% 仍远大于臂间差异 0.6%–5%。
所以依然不能说「x86_64 上预标记性能中性」。
但可以说得比上一轮强一点：**三轮一致指向「效应若有，也在 5% 以内」**，
而 §10.70 那种量级的效应已被证明是测量假象。

下一步只有一个了：换测量环境（独占核 / 裸金属）。
`isolcpus + nohz_full` 值得一试，它同时压低抢占与 tick 干扰，
是当前唯一还没试过的、能真正降低噪声地板的手段。
**在拿到组内离散降到个位数百分比的数据之前，不得对 x86_64 性能下任何结论。**

### 10.73 最后一个杠杆**不可用**：它要求改宿主内核，而这是共享机器

§10.72 指出唯一还没试过的降噪手段是 `isolcpus + nohz_full`。核对之后结论是
**在本机上不能做，也不该做**：

* `isolcpus` / `nohz_full` 是宿主内核启动参数，需要 root、需要重启宿主、
  并且会影响同一台机器上的所有其它租户。这不是「我的实验环境」，
  我没有权利为了自己的基准去改动它。
* 宿主侧的证据也支持「噪声来自外部」这一判断：
  `load average: 4.72, 4.72, 5.51`，`2/1774` 个可运行线程/总线程，16 核。
  即在我把 QEMU 绑到 `12-15` 的同时，别的租户仍在同一批核上活动，
  `taskset` 只约束我的进程，约束不了别人。

所以 §10.72 那张表应当作为本机上的最终答案：

| 尝试 | 预期 | 实测离散 |
|---|---|---|
| 跨启动交错 | 基线 | 20%–45% |
| 单次启动内配对 | 宿主漂移变共有噪声 | 21%–50% |
| 样本 ×10 加长 | 样本超过噪声相关时间 | 32%–72% |
| 独占核 / nohz_full | 真正压低噪声地板 | **不可用（需改共享宿主）** |

**x86_64 上的预标记性能问题，在本机、在不改动宿主的前提下，已经无法解决。**
这不是「还没努力」，而是**环境上限**。三轮实验的一致结论是：
**效应若有也在 5% 以内**（长样本下 1.012 / 1.034 / 0.994 / 0.950），
而 §10.70 那种 1.29x 的大幅效应已被证明是短样本噪声。

**要闭合这个问题，只能换机器**：独占宿主或裸金属。
在那之前，**CortenMM 的性能结论只限 riscv64**（§10.34，TCG，配对 A/B，性能中性），
且必须注明 TCG 与 KVM 的绝对数值不可互相比较。

### 10.74 下一项工程：让状态缺页路径**不持 `mm->lock`**（论文核心主张，仍未实现）

已核实当前状态：`kernel/mm/fault.c` 在 800 行取 `spin_lock(&mm->lock)`、1119 行才释放，
而完全不查 VMA 的状态缺页路径（963-1010）就嵌在这个区间内部。
因此论文最核心的那条主张（「缺页不碰 VMA 锁」）**目前并未实现**：
状态路径确实不查 VMA，但它仍然**在 mm 全局锁下运行**，
所有并发缺页（多核、多线程）会在 `mm->lock` 上排队。
这与 §10.33 测出的「性能中性」是一致的：拿不到 VMA 查找的收益，
因为那把锁的代价还在。

#### 前置条件已经查清：卡点是 `mm->rss` 的数据竞争

要让状态路径在 `mm->lock` 之外运行，必须先解决它顺带要更新的那些计数：

* `mm->rss` 是普通 `size_t`（`vm.h:227`），不是原子量；
* `fault.c` 里有 **12 处** `rss++` / `rss +=`，`cow.c` 另有 3 处；
* 状态路径自己也要 `mm->rss++`（`fault.c:1010` 附近），
  而它必须在不持 `mm->lock` 的情况下做这件事；
* 更要紧的是读侧：`cg_mem.c:125` 在 **OOM 选 victim** 时读 `t->mm->rss`，
  而那段代码只持 `proc_lock`，不持 `mm->lock`：
  ```c
  uint64_t pflags = spin_lock_irqsave(&proc_lock);
  ...
  size_t task_rss = t->mm ? t->mm->rss : 0;
  int score = (int)task_rss + t->policy.oom_score_adj;
  ```
  **这已经是潜在的数据竞争**（读者在 `proc_lock` 下，写者在 `mm->lock` 下，
  两者互不排斥），现在能成立只是因为当前所有写者都恰好在 `mm->lock` 下、
  而 OOM 路径恰好不并发。**一旦把状态路径移出 `mm->lock`，这个竞争会立刻变成真 bug。**

#### 因此顺序是固定的，不能跳步

1. 先把 `mm->rss` 变成原子/一致可读（`__atomic_add_fetch` 之类），
   并让 OOM 的读侧用 acquire 语义读到一致值。**这一步本身就是修一个既有隐患**，
   即使不做状态路径改造也该做。
2. 再把状态路径的临界区从 `mm->lock` 移到页表游标锁之下。
   这一点设计上前途是通的：状态路径只碰 per-PTE 元数据 + PT 项，
   而这些由 `mm_addrspace_lock()` 的 PT 页锁保护（`pt.h:29-43` 的注释正是这个契约）；
   它不需要 VMA 锁，因为它按设计不读 VMA 列表。
   真正要小心的是**与 `munmap`/`mremap` 的页表拆表并发**：
   `mm_pt_read_enter()` 的读侧令牌必须覆盖「判断 ANON_VIRT」到「写入 PT 项」全程，
   否则可能在别人拆表时把已拆掉的叶子重新写回去（这正是 §10.29 那类 PT 页 UAF 的形态）。
3. 第三步才是性能验证：这一步做完才有资格重新测 mmap-PF，
   而且必须在能降噪的环境上测（§10.73），否则量出来的还是噪声。

#### 明确不做的事

* 不会为了「看起来无锁」而在状态路径里加 per-mm 序列号之类的伪无锁。
  §10.30 刚证明 per-metadata MCS 节点**已经能把 cursor 往返成本降一个量级**，
  说明瓶颈在锁的可扩展性而非锁的存在本身。
* 不会在共享宿主上用当前 bench 去证明收益（§10.72/§10.73 已证明测不出来）。

结论：这是当前最值得做的下一项工程，且第 1 步（`rss` 原子化）独立有价值、
风险低、可以先做。但它是**多阶段改造**，本轮上下文不足以安全完成第 2 步
（页表游标锁的临界区重划 + 拆表并发的读侧令牌覆盖）并做出可信验证，
故按 §10.57 确立的纪律停在这里，**不凭猜测下手**。

### 10.75 **更正 §10.74 对第 1 步风险的低估**：`rss` 原子化是 52 处、含 CAS 夹取的改造

§10.74 写「第 1 步（`rss` 原子化）独立有价值、风险低、可以先做」。
动手前先清点了站点，这个「风险低」是低估了，据实更正。

全仓库 `->rss` 共 **52 处**，跨 10 个文件。关键在于**它们不是同一种形状**：

**(a) 简单加减**——可用一次原子操作完成，风险低。
`fault.c` 12 处（`mm->rss++`、`t->mm->rss++`、`t->mm->rss += PMD_PAGE_COUNT`、
`t->mm->rss += mapped`）、`elf.c` 2、`vm.c` 1、`cow.c` 4、`mremap.c` 1。

**(b) 读-改-写带夹取**——**这才是难点，简单原子操作是错的**：
```c
mm->rss = (mm->rss > pages) ? mm->rss - pages : 0;   /* munmap.c:125,246 madvise.c:71,91 */
mm->rss = mm->rss ? mm->rss - 1 : 0;                 /* oom.c:159 */
```
这 5 处必须写成 CAS 循环才能既原子又不会下溢：
```c
size_t cur = __atomic_load_n(&mm->rss_atomic, __ATOMIC_RELAXED);
for (;;) {
    size_t next = cur > pages ? cur - pages : 0;
    if (__atomic_compare_exchange_n(&mm->rss_atomic, &cur, next, 1,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return;
}
```
**若图省事改成 `__atomic_sub_fetch` 再夹取，会引入一个原先不存在的下溢窗口**
（读者已把值算小、另一线程又减了一次）。而 `rss` 直接喂给 OOM 选 victim
（§10.74 的 `cg_mem.c:125`），下溢会让 OOM 选出**错误的牺牲者**，这是安全性方向的真实回归，
不是风格问题。

**(c) 初始化/清零**——`vm.c:514,709`、`proc.c:605`、`exec.c:640`（release store 即可）。

**(d) 跨锁读取**——`fork.c:267`（在 `proc_lock` 下打印）、`cg_mem.c:125`（OOM 选 victim，
也在 `proc_lock` 下）。这两处必须用 acquire 语义读到一致值，
否则只是把竞争从「非原子」换成「原子但撕裂」。

所以第 1 步的真实形状是：新增一组 helper（`mm_rss_add` / `mm_rss_sub_clamped` /
`mm_rss_set` / `mm_rss_get`）+ 把字段改名以**让编译器强制暴露全部 52 处** +
逐处按 (a)/(b)/(c)/(d) 分类改写 + 全架构与全门验证。

这应当作为一次独立的、有人复核的改动单独做，而不是在一次性能会话的末尾顺手做掉。
理由：它同时要动 OOM 选人这条安全性路径，又需要覆盖 10 个文件的正确性验证；
在没有余量去逐条复核的环境里做这种改造，风险与收益不成比例。

但它依然值得做、且仍是第 1 步，因为它修的是一个**已经存在**的竞争隐患
（写者在 `mm->lock` 下、OOM 读者在 `proc_lock` 下，两者互不排斥），
只是**目前恰好因为「所有写者都持 `mm->lock`」而没有变成真 bug**。
顺序依然不变：**先 (d)+`rss` 原子化，再把状态路径移出 `mm->lock`，最后才谈性能。**

## 11. 逐条更正前述结论（2026-10-02，`feat/mm-single-level`）

本节更正 §8.13、§9.2、§10.74、§10.75 与 `kernel/mm/pt.c` 注释中的若干结论。
这些结论不是"过时"，而是**错的**：它们都指向一个已经坏掉、但被外层
`mm->lock` 掩盖着的实现。更正过程本身也是记录，因为错的结论已经误导过
一次外部协议评审。

### 11.1 `mm_pt_defer_free` 零调用者 ≠ 退役链没接（更正 §8.13、§9.2 第 4 条）

前述反复把「`mm_pt_defer_free` 零调用者」当作 P4 未接线的证据。这是找错了函数。

`mm_pt_defer_free`（`pt.c`）是一个**被取代的半成品**：它调
`mm_pt_hold_table()` 后只做 `mm_pt_node_fini()`，**并不释放帧**。真正在用的是
`mm_pt_retire_table()`，而它**早已接线**：

| 站点 | 状态 |
|---|---|
| `pt_unmap_leaf` → `mm_pt_retire_table` | 已接线 |
| `pt_unmap` → `mm_pt_retire_table` | 已接线 |
| 全部 unmap 路径（`free_vma_pages` / `munmap`×2 / `madvise`×2 / `mremap` / `sysv_shm` / `sys_mm` / demote） | 全部经由上述两个原语 |
| `mm_pt_retire_table` 内部立即 drain | 已有 |
| `mm_destroy` 在 `pt_destroy_user` 之前 drain | 已有，顺序正确 |
| NOMMU 桩 | 已有 |

仍然直接 `free` 的只有 `pt_destroy_level` 与 `pt_destroy_user_recursive`，即
**整张页表的拆卸**（elf 加载失败路径、`mm_destroy`、`exec` 的 `old_pgdir`
兜底分支）。那里表已不可达、`pt_readers == 0`，直接 free 才是对的。

结论：**P4 的退役路径本来就已接线**，§8.13/§9.2 的"这条链完全没接"应删除。
真正需要补的只是 drain 的触发频率（见 §11.6）。

### 11.2 「缺页仍以 `mm_find_vma()` 为决策入口」不准确（更正 §9.2 第 1 条、§10.74）

§10.74 写「论文最核心的那条主张（「缺页不碰 VMA 锁」）**目前并未实现**」，
并把原因归结为状态路径仍以 VMA 为决策入口。前半句在后半句面前是错的。

状态缺页路径存在且**不查任何 VMA**：它在 cursor 内 `mm_cursor_query()` 拿到
`MM_ST_ANON_VIRT` 与 mmap 时记录的权限位后就直接 `mm_cursor_map()`，整段在一个
cursor 事务内。剩下的差距**只有一条**：这段代码仍然嵌在 `mm->lock` 之下
（`fault.c` 在状态路径之前取全局锁、之后释放，cursor 嵌套在中间）。
即「不查 VMA」已达成，「缺页不碰全局锁」未达成——两件事被合并成了一句。

另外 §10.74 引用的行号（800 取锁 / 1119 释放）也已过期。

### 11.3 「覆盖节点一把锁就够」是错的（更正 §9.2 第 3 条与 `pt.c` 注释）

`pt.c` 的 `mm_addrspace_lock()` 里曾写：

> P3: the covering node's lock IS the unit of writer exclusion ... every other
> cursor that could touch that path must first acquire this same node, so it is
> sufficient

**这句话是错的**，而且它就是一次外部协议评审给出错误结论的唯一起点——评审据此
判定"实现 ADV 的后代 DFS 只会是性能倒退，建议不做"。逐条核对代码：

* 下降循环对**已存在的中间节点不取任何锁**（`pt.c:651-655` 直接 `continue`）。
  `mcs_lock` 只出现在"分配缺失中间节点"分支里，并在同一轮迭代立刻释放。
* `mm_cursor_replace()` 写的是**叶子 PTE**（`cursor_leaf_slot()` +
  `mm_pt_note_present(..., 0, ...)`），而它手里只有覆盖节点那一把锁。
* `mm_pt_note_present()` → `pt_note_present_meta()` **完全无锁**，且对
  `nr_present` 与 `*slot` 是普通读-改-写。

于是：**宽范围 cursor（覆盖节点在 level≥1）与其内部的单页 cursor（覆盖节点在
level 0）握的是不同的锁，却会写同一个叶子 PTE 与同一个 `pt_meta_t.cls[]` 字节。**
没有任何互斥。

`pt.h` 里"the cursor also holds a lock on EVERY descendant（preorder DFS）"
同样从未成立。两处断言现已改写为真实描述。

之所以一直没炸，**只有一个**原因：

**所有 cursor 调用点都嵌在 `mm->lock` 里面**（fault / mprotect / munmap / mremap
全部如此），全局锁把一切都串行化了。

### 11.3.1 更正本节先前的一处错误结论（2026-10-02，第三次修订）

本节先前写过「当前没有任何代码路径会创建 `guard_level > 0` 的 cursor，所以缺陷 A
不可达」，并据此推出「摘掉 `mm->lock` 不会激活这个竞争，真正激活它的是引入宽事务」。
**这两句都是错的**，错在只查了 `mm_pt_provision_anon()` 就下结论，漏掉了
fault-around。

`pt_covering_level()`（`pt.c`）判断覆盖层时用的是**对齐后的起点**：

```c
vaddr_t base = start & ~(span - 1);
if (base + span >= end)
    return level;
```

它**不要求 `start` 是该跨度的第一页**。于是一个 4 页的窗口，
`base = start`（页对齐），level 0 判 `start + 4096 >= start + 16384` 不成立，
直接落到 level 1：

| 路径 | 窗口 | 覆盖层 |
|---|---|---|
| 单页缺页 | 1 页 | **0** |
| 匿名 fault-around（`fault.c:624`，`ANON_FAULT_AROUND_PAGES = 4`） | 4 页 | **1** |
| 文件 fault-around（`PAGE_CACHE_FAULT_AROUND_PAGES = 16`） | 16 页 | **1** |

所以**宽 cursor 是常规路径，不是假想**。宽（level 1）与窄（level 0）写同一张
叶子表，是每次匿名/文件 fault-around 都在发生的事。

由此得到三条被上面那两句错误前提带偏的结论，全部更正：

1. **缺陷 A 是可达的、而非潜伏的。** 它现在只被 `mm->lock` 挡着。
2. **摘掉 `mm->lock`（Phase 3）正是会激活它的那个动作。** 这跟我先前写的
   「Phase 3 不会激活它」正好相反。
3. **依赖关系被我写反了。** 正确的顺序是：按操作的叶锁（本次已落地，§11.6）
   → Phase 3 摘 `mm->lock`。上层节点统一状态标记（§11.7 第 3 项）与 Phase 3
   **没有先后依赖**，它是独立的性能改动，不是安全前置。

外部评审和我自己当时都在错的地方停住了：我用「预标记按叶子表分块」这一个
调用点去论证「没有宽事务」，而 fault-around 就在隔壁。教训要改成：

> 判断一个锁粒度缺陷是否「活的」，**要把所有会开 cursor 的路径列一遍**，
> 不是只查最显眼的那一个；而且「不可达」和「被一把更粗的锁挡住」是两种
> 完全不同的状态，后者一旦撤掉那把锁就会变成前者。


### 11.4 cursor 的四个入口读的是陈旧 `path[0]`（新发现，已修）

`cursor_leaf_table()` 就是 `return cur->path[0]`，而下降循环只缓存到
`path[guard_level]`。`mm_cursor_replace()` 会先调 `cursor_leaf_slot(cur, addr, 1)`
按地址重新下降，所以是对的；但另外四个入口直接用了 `path[0]`：

`mm_cursor_query` / `mm_cursor_unmap` / `mm_cursor_mark_prot` / `mm_cursor_safe_test`

对**跨叶子表的范围**，这些函数读/写的是上一次操作遗留的叶子表。今天没出事纯属
两个巧合：唯一活的 `query` 调用是单页 fault（`guard_level == 0`，`path[0]` 恰好
正确），`unmap` 则没有任何外部调用者。但 `mm_cursor_mark_prot` 是
`mm_pt_provision_anon` 走的路径，预标记一个跨表范围就会写错表。现已全部改为按
地址下降。

### 11.5 §10.75 的站点清单过期（更正 §10.75）

§10.75 说 `->rss` 共 52 处、跨 10 个文件。实际是 **53 处、跨 19 个文件**，
其中 **9 处是 cgroup 的同名字段**（`cg_mem_t.rss`，`cg_mem.c` 6 处 +
`fs/cgroupfs.c` 3 处）——那是另一个结构，动了会破坏内存限制，不在范围内。
真正需要改写的是 **42 处**。

§10.75 漏掉的文件：`abi/native/sys_native_task.c`（2 处读）、
`abi/linux/sys_mm.c:291`（**夹取式读-改-写，且在 ABI 层**）、
`fs/procfs/procfs_render.c`（3 处读）、`proc/exit.c`（2 处读）、
`proc/fork.c`（1 处读）、`ipc/userfaultfd.c`（2 处）、
`drivers/gpu/framebuffer.c`（2 处）。

`proc/exit.c` 还有一处两次读 `mm->rss`（先判 `> 0` 再 uncharge），两次原子读可能
不同导致电荷错，已改为读一次存局部。

### 11.6 已完成的工作（`feat/mm-single-level`）

* **§10.74/§10.75 的第 1 步：`mm->rss` 原子化。** 42 处 / 19 文件 / 4 类。
  字段改名 `rss_atomic` 强制编译器枚举站点；夹取减法是 CAS 循环，不用
  `fetch_sub` 再夹取（那会引入原本不存在的下溢窗口，而下溢会让 OOM 选错牺牲者）。
* **缺陷 A：按地址的叶锁。** `cursor_leaf_slot()` 现在在下降时对每个父节点
  锁-改-解锁（安装中间节点 + 父节点元数据读-改-写），并对叶子表**按操作**加锁、
  存在 cursor 里，由 `cursor_leaf_unlock()` 在该次操作结束时释放。
  之所以是"按操作"而非"按事务"：宽 cursor 会访问上千个叶子表，
  `PT_MCS_POOL_SLOTS` 只有 1024 槽，按事务持有会 panic。
  判据：所有写入都落在单个叶子项上，因此按地址取叶锁即足以互斥。
* **缺陷 C：四个入口改为按地址下降。**
* **`check-mm-pt-lock-order` 门禁。** 8 条断言，负向 + 计数。
  已验证它在**修复前**的代码上 FAIL 6/8、在修复后 PASS —— 一个不能被 bug 弄红的
  门禁没有价值。已有的 `check-mm-lock-model` 全是"这个 token 存在吗"型断言，
  而缺陷 A 里那些 token **全都存在**，所以它不可能拦住这类问题。

### 11.7 仍未做的事

已完成的（`feat/mm-single-level`，4 个 commit）：`mm->rss` 原子化、按操作的叶锁、
`check-mm-pt-lock-order`(9 断言)、`smoke-mm-pt-race`(SMP=8 PASS)、本节这些更正。

剩下三项，**彼此没有依赖关系**（§11.3.1 更正了先前写反的顺序）：

1. **~~先把 `pt_unmap_leaf()` 改成走 cursor~~ —— 已以另一种形状落地**（见本节
   下方「第 1 项已落地」）。顺序依然是硬的：`mm->lock` 摘掉之前，unmap 侧必须先有
   按节点互斥，否则两条路会并发写同一张叶子表。

   `MM_AS_CURSOR_ONLY_ENTRY` 第 1 条写着「每一次用户 PTE 的读写都发生在
   `mm_cursor_t` 内」，而 `mm.c` 的 `pt_unmap_leaf()` 不是 cursor。

   > **本段已过期（2026-10-03，`feat/mm-complete` 更正）。** 下面这段把它描述成
   > "裸遍历、完全无锁"是不成立的：`pt_unmap_leaf()` 现在在下降的每一级对自己要
   > 写的那张页表页取节点锁（`mm.c:506` 的 `mm_pt_node_lock(path[level])`，以及
   > 拆分路径的 `:528`），锁序由 `check-mm-pt-lock-order` 门禁断言。它仍然不是
   > cursor，仍然只靠 `mm->lock` 之外的那把节点锁互斥，这一条仍然成立。
   > 原文那句"得到 **0**"是在改动之前跑的，改动之后同一命令返回非零。
   > 保留原文是为了不假装当时没有量过：**在门上写清楚"什么时候量的"，比只留结论
   > 更值钱**（同一天的教训，见本节末尾）。

   原文：`sed -n '457,515p' kernel/mm/mm.c | grep -cE 'mm_addrspace_lock|mcs_lock|mm_cursor'`
   得到 **0**。它直接 `*pte = 0`，再调用同样无锁的 `mm_pt_note_absent()`
   （`nr_present`/`cls[]` 的普通读-改-写）。所有调用方
   （`free_vma_pages` / `munmap` / `madvise` / `mremap` / `sysv_shm` /
   `sys_mm` / demote）都只靠 `mm->lock` 保护。

   **今天这靠 `mm->lock` 兜住了**：缺页和 munmap 都在同一把全局锁里串行。
   一旦把状态缺页路径移出 `mm->lock`，两条路就会并发写同一张叶子表——
   一条持叶锁（cursor），一条完全不持锁。

   实测印证（2026-10-02）：按 §11.7 的原配方拆完快段/慢段后，编译干净
   （`-Werror -UBSan`，riscv64 smp8），两项前提检查也都通过（慢段经
   `fault_map()` 走 cursor；`cg_mem_charge` 自带 `node->lock`），但
   `smoke-mm-pt-race` 超时（status 124），日志 319 行，**无 panic、无
   `MCS DEADLOCK`**。这不像死锁，像数据损坏导致的反复缺页——所以先查不变量
   是否被违反，而不是先查锁序。（我最初的假设是 cursor 与 `mm->lock` 的 ABBA，
   但快段根本不取 `mm->lock`，那个假设不成立。）

   注意 `smoke-mm-stress` **不能**用来复现：它不传 `a20.anonprov`，
   预标记关闭 ⇒ 快段永远直接 decline ⇒ 新代码根本不执行，门禁会假绿。
   能复现的只有带 `a20.anonprov=4096` 的 `smoke-mm-pt-race`。

   #### 第 1 项已落地：按节点锁住每次写，而不是换成 detach 原语

   先更正本文档上一版的两处判断。原文说「唯一能真正关闭的形状是在
   `[base, base+size)` 上开一个 cursor」，又说「per-node 锁不行」。**两句话都要改。**

   实际落地的是 per-node 锁——所以「per-node 锁不行」这句话是错的。但它错在
   **理由**：per-node 锁确实不能让整个写序列变成一个事务（那仍然不成立，见下），
   可是**不需要**。真正要的是每个写点各自原子，而这正是 per-node 锁提供的。

   改动（`mm_pt_node_lock()` / `mm_pt_node_unlock()`，`mcs_lock()` 是 pt.c 文件内
   静态的，所以包装加在 pt.c）：**5 个写点全部包住**——`pt_unmap()` 的叶子清、
   `pt_unmap_leaf()` 的 swap 清与大页清，以及两者向上回收的每一层。

   * **锁 `parent` 而不是 `child`**，且 `pt_table_empty(child)` 与清
     `parent[idx]` 必须在**同一临界区**。前者是因为节点锁按 level 递减取得
     （规则 3；`pt.c` 的 `for (int l = cur->guard_level; l > 0; l--)`），把 child
     压在 parent 之下就是反向持锁。后者是因为这两行看起来是独立语句，但若
     判断与清父项之间有并发缺页重新填入 `child`，就会清掉一张非空表 → 丢失映射。
   * **校验移进锁内**。原来在锁外判 `!(*pte & PTE_V)`，在 `mm->lock` 下这是冗余的；
     一旦缺页路径不再取 `mm->lock`，锁外的判断就只是一个可能已经过期的状态的判断。
   * **不嵌套**。每次 lock → 改 → unlock，所以向上回收虽然方向与 cursor 相反
     （`mm.c` 的 `for (l = level; l < ARCH_PT_ROOT_LEVEL; l++)` 是低 → 高），也不会
     ABBA。这正是上一版坚持「不能靠一个 cursor 覆盖整段」的原因：覆盖节点锁 +
     向上取父锁 = 反向持锁。

   为什么无锁下降本身不需要额外保护：`mm_pt_retire_table()` 会
   `mm_pt_mark_stale_recursive()`，并把真正的 `frame_free()` 推迟到
   `mm->pt_readers == 0`。所以已经缓存了该节点的 cursor 会看到 `stale` 而重下降，
   不会写进被摘掉的子树。

   **仍未做的，因此 bypass 列表保留**（条目已改写成"无 cursor，但每次写都在
   `mm_pt_node_lock` 内"）：这两个函数仍然不是 cursor，仍然没有**区间级原子性**。
   把它们宣布为合规会是假话。它们现在有的是规则 3 要求的按节点互斥，而这正是
   Phase 3 快段需要的。`check-mm-pt-lock-order` 加了两条断言，锚定**调用形式**
   （`mm_pt_node_lock(path` / `mm_pt_node_lock(parent)`）而非裸函数名——否则
   `mm.c` 注释里的散文引用就能满足它们——并钉住全部 5 个写点；删掉
   `pt_unmap()` 的叶子锁括号会让门禁 14 断言掉到 13。

   验证：`check-mm-lock-model` 14/14、`check-mm-pt-lock-order` 14/14（负向测试过）、
   riscv64/x86_64/aarch64 SMP=4 构建，以及 QEMU SMP=8 下
   `smoke-mm-fork-exec-race`（重度 exercise unmap）、带 `a20.anonprov=4096` 且
   `--wide-cursor-only` 的 `smoke-mm-pt-race`（预标记快段）、`smoke-mm-stress`、
   `smoke-abi-linux`——全 PASS，`MM-ASM` 审计全 0，四份日志均无
   `MCS DEADLOCK`、无 panic。

2. **从状态缺页路径摘掉 `mm->lock`**（§10.74 的真正剩余项）。这是迁移的真正目标。
   做法：把 `handle_demand_fault_access()` 拆成两段。快段只拿 cursor，不拿
   `mm->lock`：`mm_addrspace_lock()` → `mm_cursor_query()` → 若
   `MM_ST_ANON_VIRT` 且非 UFFD 则分配/清零/装入 → `mm_cursor_unlock()` →
   `mm_rss_add()` 与各 perf 计数器（Phase 0 已把它们变成原子的）→ 返回 0。
   慢段原样保留，在 `mm->lock` 之下走 VMA 路径。

   **一个曾经看起来是阻塞点、实际不是的问题**：拆成两段之后，慢段开头那个
   "PTE 已存在就返回 -1" 的检查会在快段与 `spin_lock(&mm->lock)` 之间变得可达
   ——另一个线程可能刚好把同一页映射好了。这不需要新处理：`core/trap.c:373`
   在 `handle_demand_fault_access()` 返回非 0 时，本来就会落到
   `handle_present_page_fault()`，而那里的注释（377-381）写明的正是
   "another thread completes the same mapping between our first present-PTE
   check and a failed/redundant demand-fault attempt"。这条重试路径已经存在。

   仍需注意：`pfa_alloc_page()` 目前是在 cursor（一个自旋锁）之下调用的
   （`fault.c:1124`），这是既有做法；把这段移出 `mm->lock` 之后，它就成了
   页表锁下的唯一分配点，是否会睡眠要单独复核。

2. **上层节点统一状态标记**（论文 §3.3 的
   "using upper-level PT pages to represent large memory regions with identical
   status"），替代逐页预标记。**这是性能改动，不是安全前置**——当前预标记按叶子表
   分块，每 2 MiB 一次 cursor，代价可接受。
   顺带更正一次被否掉的方案：把区间状态记在**叶子表**上是不成立的——叶子表必须先
   存在，1 GiB 稀疏映射仍要 4096 个叶子表（16 MiB 页表），并没有解决稀疏性问题。
   真正要注意的是：**宽事务会经过 §11.3.1 说的 fault-around 那条路**，所以这项改动
   同样依赖已落地的叶锁。

   #### 第二次尝试：仍然超时，而这次有 `[LOCK-STALL]` 证据（更正我自己的错误结论）

   把 `mm_fault_from_status()` 提到 `spin_lock(&mm->lock)` **之前**（快段只持
   cursor，页分配改用 `pfa_alloc_flags(0, 0)` 不可回收，否则会在 cursor 自旋锁下
   经 `oom_try_reclaim()` 睡觉），riscv64 SMP=4 编译干净，`smoke-mm-pt-race`
   **仍然 status 124 超时**。

   但这次日志里有硬证据，而**我上一轮的结论是错的**：

   ```
   [LOCK-STALL] cpu=3 lock=0xffffffc0bf7a0010 name=mm waiter=11 owner=12
               owner_ra=0xffffffc0803f626e waiter_ra=0xffffffc080212d4a
               spins=93653565440 elapsed_ms=235071
   ```

   上一轮我写的是「无 panic、无 `MCS DEADLOCK`，不像死锁，像数据损坏」。**错在
   grep 了错的 token**：锁停滞检测器确实存在且一直在打印，事件名是
   `[LOCK-STALL]` 而不是 `MCS DEADLOCK`。我据一次 grep 失误否掉了「这就是死锁」
   这一整类假设，然后顺着「数据损坏」去找不变量，方向从一开始就错了。

   解析两个返回地址（`addr2line` 对 `.kernel-build/.../kernel.elf`）：

   | 地址 | 符号 | 含义 |
   |---|---|---|
   | `owner_ra=…0803f626e` | **`sys_mmap`** | CPU 12 持有 `mm->lock` 且 235 秒不释放 |
   | `waiter_ra=…080212d4a` | **`trap_handler`** | CPU 2/3 在缺页路径上等 `mm->lock` |

   所以这是 `mm->lock` 上的**真实锁死**，不是数据损坏：`name=mm`，一个持有者
   停在 `sys_mmap`，两个等待者停在缺页入口。

   **机制（已按代码结构确认，不再是假设）**：

   ```
   sys_mmap            sys_mm.c:95      proc_mmap(...)
     -> proc_mmap      proc.c:776       spin_lock_irqsave(&t->mm->lock)  <-- _irqsave
       -> mm_mmap_locked  mmap.c:88
         -> mm_pt_provision_anon  mmap.c:195
           -> mm_addrspace_lock    -> frame_alloc()
             -> pfa_alloc_flags(0, can_reclaim=1)
               -> oom_try_reclaim() -> proc_force_exit(victim)
                 -> 受害者 VMA 拆解 -> 拿 mm->lock
   ```

   （链条更正：我在前一条 commit 里把中间节点写成了 `mm_mmap`（mmap.c:404）。
   实际路径**不经过**它——`proc_mmap` 自己取锁（proc.c:776）并直接调
   `mm_mmap_locked`。`mm_mmap` 是另一个同样纪律的包装，不在本路径上。
   取锁点是 `proc.c:776`，已核对。）

   **一次被自己的实验否掉的假设（值得留着）**：我一度怀疑 `mm_anon_provisioned: 0`
   是我把 PT 页分配改成不可回收（`frame_alloc_nr()`）造成的——因为
   `mm_pt_provision_anon()` 里 `if (r < 0) return r;` 会在
   `mm_addrspace_lock()` 失败时**跳过计数器**（pt.c:1208 / 1230）。
   把那两处改回可回收的 `frame_alloc()` 重跑，**三个计数器仍然是 0**。
   所以这个改动**不是**原因，它是通过验证的、保留。

   即：**`mm->lock` 是关中断的自旋锁，却跨越了一次可睡眠的回收**——睡眠发生在
   IRQ 关闭的自旋锁里。若受害者是自己的 mm，就是持 `mm->lock` 自杀。这与观测
   完全吻合：持有者 PC 落在 `sys_mmap`，等待者 PC 落在 `trap_handler`，锁名 `mm`。

   我先前两次 grep 都没找到这条链，是因为我在 `mm_mmap_locked`（88-195）**内部**
   找 `spin_lock(&mm->lock)`，而锁是由**调用者** `mm_mmap`（404）持有的；
   `mmap.c:190` 的注释本身就写着「It runs under mm->lock, so the order
   mm->lock -> page-table lock」。看注释比 grep 快。

   **为什么以前不炸，摘锁后才炸**：这是既存缺陷，但要两个条件同时成立。
   `mm_pt_provision_anon` 只在预标记开启时走到那条分配（所以只有带
   `a20.anonprov` 的 `smoke-mm-pt-race` 受影响，`smoke-mm-stress` 不受影响）。
   而在摘掉快段的 `mm->lock` **之前**，缺页线程会先阻塞在 `mm->lock` 上，根本
   进不了 `mm_addrspace_lock`，于是与 provisioning 串行化、窗口关闭。摘掉之后
   缺页不再取 `mm->lock`，就能与一个正在 `mm->lock` 下回收的 provisioning
   并发进入同一条分配路径——**快段摘锁本身没有制造这个 bug，但它是把窗口打开
   的那个改动。**

   **下一轮该先做的（不是再改快段）**：任何 `mm->lock` 持有者都不应走可回收
   分配。快段已经改成 `pfa_alloc_flags(0, 0)`，缺的是 provisioning 那一侧——
   要么让 `mm_pt_provision_anon` 用不可回收分配并在耗尽时回退，要么把 PT 页
   分配提到 `mm_mmap` 取 `mm->lock` **之前**。

   **一个让方案唯一化的关键事实（读注释读出来的）**：`mm_pt_provision_anon()`
   在 `mmap.c:195` 是以 `(void)` 调用的，返回值被丢弃，而且它上面 188-189 行的
   注释写明「Provisioning is best effort: a range too large to provision eagerly
   just keeps the VMA-based fault path, which remains correct」。

   也就是说 **provisioning 分配失败是一个已经被支持的结局**，不是新行为。所以把
   PT 页分配改成不可回收（`pfa_alloc_flags(0, 0)`）在 provisioning 这一侧是
   **零风险**的：耗尽时预标记不发生，缺页照旧走 VMA 路径。

   这把选项从「多种」收敛到一个：把 `mm_addrspace_lock()` 与
   `cursor_leaf_slot()` 里那两处仍可回收的 PT 页分配改成不可回收。剩下的语义
   影响只有一处需要盯——其它 cursor 使用者（`mm_cursor_fault` /
   `mm_cursor_map`）在内存压力下会从「先回收再成功」变成「返回 -ENOMEM」。
   这是把回收移出自旋锁的必然代价，且 -ENOMEM 会沿现有的缺页失败/OOM 路径走，
   不影响正确性；但它**必须实测**，不能推断——`smoke-mm-pt-race` 恰好带
   `a20.anonprov` 且压力足够大，是唯一会真正走到耗尽分支的用例。

   注意这同时补上了快段的一个遗留洞：快段的**数据页**已经是不可回收分配，但它
   经 `mm_cursor_map` → `cursor_leaf_slot` 分配的 **PT 页**仍然是可回收的，
   也就是在 cursor 自旋锁下回收。之前只修了一半。

   **这解释了这三轮为什么都白改**：第 1 轮我怪 cursor 不变量、第 2 轮我怪 ABBA
   锁序、第 3 轮（本次）我怪 unmap 侧节点锁 + 睡眠分配——三次都改了真的东西
   （都留下了、都验证过），但都不是这个死锁的成因。共同点是我从没打开过
   `[LOCK-STALL]`，而它一直就在日志里。

   **推论（下一轮该先做的）**：`mm->lock` 的任何持有者都不应走可回收分配。
   快段已经这么改了（`pfa_alloc_flags(0, 0)`，失败就 decline 交给 VMA 路径），
   但 `mm_pt_provision_anon()` 那一侧还没有——而它才是这次日志里持有者的路径。
   先把这一侧也改成不可回收，再重试摘锁。

   **已验证，且我此前得出的「快段是死代码」是错的（已撤回）**：

   我一度判定预标记从未发生、快段在 `smoke-mm-pt-race` 里是死代码，理由是
   `mm_anon_provisioned: 0` / `mm_fault_from_status: 0`。**那个 0 是测量装置的
   产物，不是系统的事实**：`a20_perf_format()`（`core/perf.c:94`）在渲染
   `/proc/a20/perf` 时先 `g_a20_perf_enabled = 1` **再**取快照，而
   `a20_perf_add()`（`include/core/perf.h:108`）在 `g_a20_perf_enabled == 0`
   时直接 return。所以在负载**之后只读一次**，所有计数器必然是 0——与负载做了什么
   无关。

   修法是在负载**之前**先读一次 perf（上膛），之后再读一次取值。实测：

   ```
   上膛读（设计如此，全 0）      负载后
   mm_anon_provisioned: 0        mm_anon_provisioned: 9464
   mm_anon_faults:     0         mm_anon_faults:     778
   mm_fault_from_status: 0       mm_fault_from_status: 778
   ```

   即快段在这次运行里执行了 778 次。文档 4508 行更早的一次运行也记录了
   `mm_anon_provisioned: 331536` / `mm_fault_from_status: 164676`，与之一致。

   **两个教训**：(1) 我把「我测到的 0」当成了「系统的 0」，而且是在已经 commit
   之后才发现自己错了；(2) 本项目栽过两次同一类的坑——`smoke-mm-stress` 不传
   `a20.anonprov` 导致假绿，这次是 perf 计数器默认休眠导致全 0。**任何「某计数器
   为 0」的结论，必须先确认计数器本身是活的。**

   `smoke-mm-pt-race` 现在断言 `mm_fault_from_status: [1-9]`（非零），并且上膛读是
   它的一部分，所以快段退化成死代码时这个门禁会**红**。这条断言现在是有意义的，
   不再是我之前以为的那种装饰。

   #### 第三次尝试：旧死锁消失，失败点前移到 TLB shootdown 并发

   两个前提都修好之后（unmap 侧节点锁、`frame_alloc_nr()` 消除 `mm->lock` 下的
   回收），第三次把 `mm_fault_from_status()` 提到 `spin_lock(&mm->lock)` 之前。
   编译干净，`smoke-mm-pt-race`：

   * **不再有 `[LOCK-STALL]`**（计数 0）——前两轮的 `mm->lock` 死锁确实修掉了；
   * 但换了一个新的 panic：

   ```
   [RV64 TLB] timeout self=0 target=1 expected=15 request=15 ack=14 online=0xff
   ========== KERNEL PANIC ==========
   RISC-V remote TLB shootdown timed out
   [PANIC] caller=rv64_smp_remote_tlb_flush+0x40e
   ```

   `expected=15` 而 `ack=14`：应答主了一个没回来。这**讲得通**，而且正是摘掉
   `mm->lock` 之后下一个该暴露的东西——远程 TLB shootdown 的 ack 记账此前一直
   被 `mm->lock` 隐式串行化（缺页持锁 ⇒ 同一 mm 的 shootdown 不并发），现在快段
   不取 `mm->lock`，多个 CPU 可以并发发起 shootdown，**记账互相覆盖**，于是出现
   `ack` 永久缺失。快段自己只做 `arch_tlb_flush_page_local()`，问题是它与别的 CPU
   的远程 shootdown 之间的并发。

   所以失败点从「`mm->lock` 死锁」前移到「TLB shootdown 需要自己的串行化」，后者
   是更局部、更好修的问题——但要正确加固 shootdown 协议（谁持有 pending 表、如何
   合并并发请求、超时如何重试）不是一处改动，需要单独一轮，且必须实测。

   **本轮没有落地这个改动**：`fault.c` 已恢复到 20 commit 的 checkpoint，工作树
   干净、门禁全绿。留下的成果是失败点的推进本身，加上这一条：Phase 3 的第三道
   门槛是 TLB shootdown 的并发安全，而不是页表锁。

   #### 第三次失败的机制：`arch_cpu_relax()` 是裸 `nop`，输不起软中断

   `expected=15 request=15 ack=14` 说明**没有竞争请求者**——15 号请求是我们自己
   发的、也是最后一条，所以目标 CPU 单纯**没 servicing 我们的 IPI**。而
   `rv64_smp_remote_tlb_flush()` 的等待循环注释写得很明确：

   ```
    * Wait with interrupts enabled: the targets must service the soft IRQ
    * (sfence + ack).  If we are inside a trap with IRQs off, they may be
    * waiting on us for their own flush; enabling interrupts here lets us
    * service those IPIs and breaks the ABBA cycle.
   ```

   目标要 ack 就必须能接收软中断。那么谁接收不了？看 `mcs_lock()` 的自旋：

   ```c
   while (__atomic_load_n(&me->locked, __ATOMIC_ACQUIRE) == 0) {
       arch_cpu_relax();
   ```

   而 riscv64 的 `arch_cpu_relax()` 是（`kernel/arch/riscv64/include/cpu.h:21`）：

   ```c
   static inline void arch_cpu_relax(void) { __asm__ __volatile__("nop"); }
   ```

   **裸 `nop`，不重开中断、不让出流水线。** `mcs_lock()` 上方的注释把
   「preemption disabled」当成了「可以无限自旋」，但**不被抢占 ≠ 可以饿死别人**。

   > **注意：以下因果链是推断，不是已证实的结论。** 我**没有**证明目标 CPU 当时
   > 正在 MCS 自旋里——panic 只记录了请求方的 PC。需要实测（打印目标 CPU 在超时
   > 时刻的 PC，或让 `mcs_lock` 自旋循环在超时时 dump 自己的 backtrace）才能定论。
   > 不要再把它当结论往下推。

   **已核对的事实**（这些是读代码得到的，可信）：

   * 陷阱入口 `trap.S:107` 的 `csrc sstatus, t1` 清 SIE，所以缺页跑在**中断关闭**
     的上下文里；
   * `arch_cpu_relax()` 在 riscv64 是裸 `nop`（`cpu.h:21`）；
   * `rv64_ipi_tlb_flush_handler()`（`board.c:168`）**不取任何锁**，只做
     `sfence.vma` + ack ——所以目标 CPU 若能接到软中断，它 ack 时不需要拿页表锁；
   * `rv64_smp_remote_tlb_flush()` 的等待循环注释明确要求「targets must service
     the soft IRQ」，且它自己在等之前会重开中断；
   * `expected=15 request=15` 说明**没有第二个请求者**竞争该代次。

   **已排除的假设**：`pt.c` 里**完全没有** TLB flush 调用；`mm_tlb_invalidate_finish()`
   只在 `munmap.c` / `madvise.c` / `oom.c` 被调用。所以「远程 shootdown 在持有节点锁
   时发出」这条**不成立**——排除了它其实是那个经典 ABBA 的可能。

   剩下的待验证假设：目标 CPU 因某种原因没能接收软中断，而 `nop` 自旋 starve
   IPI 是最合理的候选。但**这是候选，不是结论**。

   为什么以前不出问题：缺页整体被 `mm->lock` 串行化，其余 CPU 阻塞在
   `spin_lock(&mm->lock)`（IRQ 保持开启）上，能响应 IPI。快段摘掉 `mm->lock`
   之后，大量 CPU 同时争 MCS 节点锁——**这条路径第一次被大量走到**（"被大量走到"
   是有依据的；"因此目标饿死在 nop 里"仍是推断）。

   所以这是同一个主题的第三次出现：**这一整天的三次修复都是"某条自旋路径饿死了
   别的进展"**——睡眠压在自旋锁下、`mm->lock` 下回收、现在 `nop` 自旋饿死软中断。
   每一次都要等到移除上层串行化才暴露。

   #### 实测推翻了上面那个 TLB 推断：真症状是 level-0 节点锁被永久持有

   在 `mcs_lock()` 的自旋循环里加计数（每超过 `2^26` 打一次 `[MCS SPIN]`），重跑
   同一配方（Phase 3 改动 + SMP=8 + `a20.anonprov=4096`）：

   ```
   [MCS SPIN] cpu=1 node=0xffffffc0bf75b000 level=0 total=2013265920
   [MCS SPIN] cpu=4 node=0xffffffc0bf75b000 level=0 total=2147483648
   RV64 TLB 超时次数： 0
   MCS SPIN 次数：     2493
   ```

   **两个关键事实**：

   1. `[RV64 TLB]` **一次都没触发**（上一轮那个 panic 在这次带计数的复现里根本没
      出现）。所以「目标 CPU 饿死软中断 → ack 不了 → 5 秒超时」这条链**不成立**，
      上一小节的推断是错的。
   2. CPU 1 和 CPU 4 卡在**同一张** level-0 节点表
      （`0xffffffc0bf75b000`）上，`total` 单调涨到 **2^31 ≈ 21 亿次**，而且
      `[MCS DEADLOCK]`（自死锁检测）**没有**触发。

   也就是说：**这不是饿死，是那把 level-0 节点锁的持有者再也没释放**。持锁者不
   在推进（否则等待者会拿到锁），等待者却在推进（`total` 在涨）。两个 CPU 对同一张
   叶子表互斥竞争，其中一方永远不放。

   已排除的读法：

   * **不是 `mm_addrspace_lock()` 漏解锁**。逐条核对了它的返回路径：每次
     `continue` 前都 `mcs_unlock`，两处 `return 1` 之前也都不持锁（第 54 行的
     `return 1` 在本轮还没取任何锁）。
   * **不是"远程 shootdown 在持节点锁时发出"**。`pt.c` 里完全没有 TLB flush 调用，
     `mm_tlb_invalidate_finish()` 只在 `munmap.c`/`madvise.c`/`oom.c` 被调用。

   剩下的候选（本轮**未**验证，不要当结论）：

   * 某条路径取了 level-0 节点锁但没有配对释放——注意 `pt_unmap()` 的向上回收会
     逐层取 `parent`，而 `mm_addrspace_lock()` 对 4 KiB 区间取的 guard 锁**正是**
     level-0 那张表；`--wide-cursor-only` 负载里有并发 unmapper，所以这两条路径是
     并发跑的。
   * 两个不同 guard_level 的 cursor 之间的跨节点锁序环（ABBA）。自死锁检测只看
     "本 CPU 是否已持有该节点"，**看不到跨 CPU 的环**，所以它不会响。

   #### 定位持有者：level-0 节点锁被 acquired 但从未 `mm_cursor_unlock`

   在 `[MCS SPIN]` 首次触发时把**每个 CPU 的 MCS 持有栈**（`pool->depth` +
   `pool->held[]`）打出来，同一配方复现：

   ```
   [MCS OWN] waiter=2 node=0xffffffc0bf75b000 want_depth=1
   [MCS OWN]   cpu2 depth=1 holds: 0xffffffc0bf75b000
   [MCS OWN]   cpu4 depth=1 holds: 0xffffffc0bf75b000
   [MCS OWN] waiter=4 node=0xffffffc0bf75b000 want_depth=1
   [MCS OWN]   cpu2 depth=1 holds: 0xffffffc0bf75b000
   [MCS OWN]   cpu4 depth=1 holds: 0xffffffc0bf75b000
   ```

   > **本节的读法已被下一节的实测推翻，保留原文是为了记录我错在哪。**
   >
   > `held[]` 确实只在 `mm_cursor_unlock()` 的 unwind 循环里弹出，但关键在于
   > **它在进入自旋之前就被写了**。所以**正在等待的 CPU 也会显示成"持有"它正在
   > 等的那个节点**——`want_depth=1` 不是"入队信息"，而是**同一个自旋者自己**。
   > 因此 `cpu4 depth=1 holds: X` 只能说明 cpu4 在等 X，**不能**说明它已完成获取。
   > 下一节拿到 PC 之后重读同一份数据，结论不同。

   **尚未确定的是持有者卡在哪。** 已知它在临界区内（`depth==1`，没有嵌套取第二把
   锁），但"在临界区内"和"永不返回临界区"是两件事。剩下的可能：

   * 在临界区内又阻塞/自旋在别的东西上（但那会让 `depth` 或持有栈出现第二项——
     没出现，所以更像是**单点卡死**）；
   * 在临界区内进入了一个不会返回的循环；
   * 被 QEMU 调度出去（KVM 抢走），表现为持有者不推进。

   **下一轮该测的是持有者的 PC**，不是再猜：在 `mcs_lock()` 的进入点把
   `owner_pc[节点] = 返回地址` 记下来，`[MCS SPIN]` 触发时连同等待者一起打印，
   再用 `addr2line` 解析。这一步直接给出"是谁、哪一行、持锁没放"。
   拿到 PC 之前，上面三条都只是候选。

   #### 持有者 PC：指向 `mm_addrspace_lock`，并推翻上一节的读法

   记录获取点 PC（`__builtin_return_address(0)`），自旋首次超阈值时把每 CPU 的
   （节点, PC）对打出：

   ```
   [MCS OWN] waiter=0 node=0xffffffc0bf6b3000 want_depth=1
   [MCS OWN]   cpu0 depth=1 [0xffffffc0bf6b3000 pc=ffffffc08023f580]
   [MCS OWN]   cpu3 depth=1 [0xffffffc0bf6b1000 pc=ffffffc08023f580]
   [MCS OWN] waiter=3 node=0xffffffc0bf6b1000 want_depth=1
   ```

   `addr2line`：**两个 PC 都是 `mm_addrspace_lock`**（`kernel/mm/pt.c`）。

   带上 PC 重读这份数据，得到与上一节**不同**的结论：

   * cpu0 自旋在 `b3000`，cpu3 自旋在 `b1000`——**两个不同节点**；
   * 8 个 CPU 全被枚举，只有这两个 `depth > 0` ⇒ **没有任何 CPU 完成过对这两个
     节点的获取**；
   * 但两个节点都非空（否则 `mcs_lock` 里 `tail == 0` 直接拿到，不会自旋），
     且都没人交出去。

   所以准确说法是：**这两个 level-0 节点的 MCS 队列非空，但队列里没有活的持有者**，
   即 `mcs_unlock()` 从未把交接做完。上一节"cpu4 取到了却没走完
   `mm_cursor_unlock`"是错的——这是今天第二次把测量结果当成系统状态。

   已核对排除：本次涉及的三个候选点都正确配对解锁——
   `mm_addrspace_lock()` 的**每一条**返回路径（含 stale `-EAGAIN`、两条
   `-ENOMEM`、两条 `return 1`），以及 Phase 3 的 `mm_fault_from_status()` 的 5 个
   `goto decline` 与成功路径，全部调用 `mm_cursor_unlock()`。
   **泄漏点不在这两处，且尚未定位。**

   剩下的可能（**未**验证，不要当结论）：

   * `mcs_unlock()` 的交接写错了内存序，唤醒没能传递到下一个 waiter；
   * 某个调用方在**未持锁**状态下调用了 `mm_cursor_unlock()`，把别人的节点弹出
     自己的 unwind 栈。

   **下一轮该测 `mcs_unlock()` 本身**：交接前后打印 `m->lock`、`me->node`、
   `me->locked` 三个值，并给每个节点加 enqueue/dequeue 计数，直接看交接有没有发生。
   在那之前不要相信任何关于持有者的推断。

   **本轮没有落地**：instrumentation 与 Phase 3 改动都已恢复，工作树回到 25 commit
   的 checkpoint、门禁全绿。

   **本轮没有落地**：instrumentation 与 Phase 3 改动都已恢复，工作树回到 24 commit
   的 checkpoint、门禁全绿。

   **下一轮第一步应该是定位持有者，而不是继续猜**：在 level-0 节点锁的获取/释放
   两端各加一个 per-CPU 的 owner+depth 记录，在 `[MCS SPIN]` 触发时把等待者与
   `owner[节点]` 一起打出来。这一步就能把上面两个候选分开。

   **本轮没有落地**：instrumentation 与 Phase 3 改动都已恢复，工作树回到 23 commit
   的 checkpoint、门禁全绿。这一节记录的是**实测结论**（TLB 推断被推翻、症状是

   #### 结局：根因是 MCS 锁的交接从未实现，Phase 3 已落地并验证

   上面每一轮"没有落地"都是当时的事实，但结论已经变了。**根因不是 TLB、不是
   `arch_cpu_relax()`、也不是任何调用方漏解锁**——是 `mcs_lock()`/`mcs_unlock()`
   的交接**根本没有实现**，两个独立的致命 bug：

   1. **等待者从未被挂进队列。** `mcs_lock()` 拿到 `tail` 之后没有把自己的节点
      写进前驱的 `->next`，所以解锁方读到的 `me->next` 恒为 `0`，永远走 CAS
      分支——而此时 `m->lock` 已经被 `exchange` 换成了**等待者**，CAS 必然失败。
      锁永远不释放，等待者永远不被唤醒。**一次争用就把节点永久卡死。**
   2. **解锁方读错了自己的节点。** `mcs_unlock()` 用共享字段 `m->node` 回读
      "自己"的节点，但**每个**获取者都会覆写它。持锁者若身后有等待者，读回的就是
      那个等待者，于是它清掉锁却把交接给了空气。

   两条都符合最初的观测：两个 CPU 各自自旋在**不同**节点上、8 个 CPU 中**没有
   任何 CPU 完成过获取**、持有者 PC 解析到 `mm_addrspace_lock`（`exchange` 所在
   处）、自旋次数越过 `2^31`、而 `[MCS DEADLOCK` 不响——这是对的，等待者并不是
   自死锁，它是在等一个协议根本产生不了的交接。

   修法：等待者发布 `pred->next = me`；`mcs_unlock()` 的 CAS 失败分支改为等待后继
   链接出现并交接；解锁方从自己的 per-CPU 池取节点
   （`pool->nodes[pool->depth - 1]`，与 `mcs_lock()` 压入的槽位一致），
   `m->node` 随之废弃。

   **为什么它一直没被发现**：`mm->lock` 把整个缺页串行化，节点锁几乎从不争用，
   这条路径从未被执行。摘掉 `mm->lock` 让争用变成常态——**不是它导致了 bug，是它
   让 bug 变得可达**。这与今天另外两个 bug 形状完全一致（睡眠压在自旋锁下、
   `mm->lock` 下回收）：**都要等上层串行化被移除才暴露。**

   **Phase 3 现已落地**：`mm_fault_from_status()` 在 `spin_lock(&mm->lock)` **之前**
   运行，快段只持 cursor。它不查 VMA（这正是论文 Fig. 8 的 handler，也是不需要
   `mm->lock` 的原因）；数据页用 `pfa_alloc_flags(0, 0)` 不可回收分配，耗尽时
   decline 交给 VMA 路径。

   验证（28 commit 的 checkpoint）：`check-mm-lock-model` 14/14、
   `check-mm-pt-lock-order` 17/17、riscv64/x86_64/aarch64 SMP=4 构建、SMP=8 下
   `smoke-mm-pt-race`（`a20.anonprov=4096`，复现争用的那个用例）/
   `smoke-mm-stress` / `smoke-mm-fork-exec-race` / `smoke-abi-linux` 全 PASS，
   guest 日志里 LOCK-STALL / MCS DEADLOCK / panic **全为 0**。

   且**非空跑**：同一次运行报出 `mm_anon_provisioned: 9464`、
   `mm_fault_from_status: 778`，即无锁快段真实服务了 778 次匿名缺页，而该 smoke
   断言这个计数器非零。

   **教训（这一天最贵的一条）**：前两轮我都在**猜**机制（TLB shootdown、
   `arch_cpu_relax()` 饿死软中断、调用方漏解锁），三轮都改了"看起来对"的代码。
   真正的做法是**把状态打出来**：先打等待者与每 CPU 的持有栈，再打获取点 PC，
   最后才定到"队列从未被链接"。而我两次把**测量装置的产物**当成了系统事实
   （`[LOCK-STALL]` 被我 grep 成 `MCS DEADLOCK`；perf 计数器休眠导致的 0 被我
   读成"快段是死代码"）。**在门上写清楚"什么在测、什么没在测"，比多改三处代码
   更值钱。**
   level-0 节点锁被永久持有）和**下一步该测什么**。

   **下一轮该先做的**：让节点锁的自旋可被抢占，或者让 `arch_cpu_relax()` 真正让出
   （riscv64 上 `wfi`，或循环里检查待处理 IPI）。**没落地**——两者都改动面不小
   （`arch_cpu_relax()` 是全局 spin helper，替换它要重测所有锁路径），需要单独
   一轮实测，不能顺手改。

3. **drain 的触发频率**：`mm_pt_retire_drain()` 只在 retire 时立即调用，若
   `pt_readers > 0` 就返回，退役列表在持续多核缺页下可能堆积。当前每次 retire 都会
   尝试，最终会在某个 `pt_readers == 0` 的时刻排空，所以不是硬泄漏，但需要实测
   增长曲线。


---

## 12. 真实软件门禁（2026-10-03，`feat/mm-complete`）

前面 §8–§11 的全部结论都是在 smoke 门禁上得到的。这不是错的，但它是**有偏的**：
`smoke-mm-stress`、`smoke-mm-pt-race`、`smoke-mm-fork-exec-race` 跑的都是内核自己
写的系统调用、用的都是内核自己分配的页。它们测不到真实程序踩的那几种形状——
编译器 mmap 一大块 arena、JIT mprotect 代码页、git 建 3 GiB 索引再 remap、
解释器 fork 五千个对象。

本节记录一次真实软件运行的结果，以及它推翻的三条此前的判断。

### 12.1 门禁本身

`packages/world/mmtest.world` + `packages/overlay/mmtest/`，
`make smoke-mm-software`（`tools/mmtest_gate.py`）。

五个真实软件，逐个**验证内容**而不是验证退出码——`git clone` 一个空仓库也是退出 0：

| 工具 | 判据 |
| --- | --- |
| git | 60 个小文件 + 30 个二进制 blob，commit → clone 到另一目录，逐文件 `cmp` |
| vim | 插入、替换、编辑一个 4000 行文件，核对行数与内容 |
| gcc | 多文件编译、`-O2`、`-static`、15 次 re-exec；外加 `forker.c` 40 轮 fork，父子写同一份继承的 64 KiB 并互相核对隔离 |
| python | 300 轮分配/释放、4 MiB bytearray、stdlib import、ctypes |
| nodejs | 版本、200×20000 数组分配、50000 条目 Map |

**两个判定，缺一不可：**

1. `MMTEST_RESULT: PASS` —— 五个都跑完并核对通过。
2. 关机时的 `[MM-ASM]` 审计行全 0 —— 页表与逐页元数据在每一个存活地址空间上一致。

只有 (1) 会漏掉"软件跑完了但两个映射表示已经漂移"；只有 (2) 一个只 `memset` 的
空跑也能过。

审计行**只在关机路径上打印**，所以客端脚本自己 `poweroff -f`。第一版没有这一步，
宿主超时杀掉客端，审计从未跑过，而门禁因为"没有报错输出"判成了通过——**一道
看不见自己不变量的门禁不算通过**。

### 12.2 结果

```
MMTEST: ALL STAGES PASS
MMTEST_RESULT: PASS
[MM-ASM] pt_pages=10 entries=3584 missing_meta=0 present=0 absent=0 prot=0 cow=0
         vma=0 vmai=0 safe=0 anon_virt=0
```

### 12.3 真实软件推翻的三条判断

**(1) `mprotect` 不更新 present 页的 status —— 真缺陷。**

基线审计报 `prot=5 vma=1`。根因：`mm_pt_refresh_absent_prot()` 的注释写着"present
的叶子不需要处理，它的有效权限在 PTE 里，mprotect 已经改过了"——**对 MMU 是对的，
对 status 是错的**，而审计器比对的是 status。

修法不是把注释改对，是把语义改对：重命名 `mm_pt_refresh_leaf_prot()`，让它刷新
任何已映射的 class，并在 present 分支也调用它。这条只有跑真实程序才暴露得出来：
kernel 自写的 smoke 不会去 mprotect 一个已经 fault 进去的页再核对元数据。

**(2) `cow.c` / `fault.c` 写 `PTE_COW` 时从不更新 status —— 潜伏缺陷。**

`cow.c` 里 `mm_pt_note_present` / `mm_pt_sync_status` 出现 **0** 次，而它一直在改
`PTE_COW`。今天它还没变成 bug 只是因为 fault 分派还在读 `mm_find_vma`；**等 P6 把
分派改判 status 的那一天，这三处就会立刻变成错页**。新增
`mm_pt_sync_status()`：从刚写完的 PTE 重新推导 prot 与 COW，但**保留调用方给的
class**——class 无法从 PTE 恢复，R/W/X 说的是"允许什么"而不是"由什么支撑"。

**(3) W^X 默认 `deny` 让 nodejs 起不来 —— 但这不是内存模型的缺陷。**

```
[WARN] [WX] mprotect: pid=286 请求 W|X 映射，拒绝 (-EACCES)
SIGTRAP: pid=286 sepc=0xb3433a
FATAL: pid=286 signal=5 pc=0xb3433a comm=MainThread path=/extra/usr/bin/node
```

`kernel/mm/wx.c` 把 `deny` 设成默认，理由写在文件头："本树的用户态不需要 RWX ——
没有 dlopen，也没有 JIT"。**nodejs 用一行就否掉了这句话**：V8 把代码页
mprotect 成 W|X，然后**往里写**。所以 `strip` 也救不了——实测从 mprotect 处 trap
变成首次写入时 segfault。

一个启动不了 V8 的内核不是"策略更严"，是"跑不了 JIT"。错的是前提，不是策略，
所以默认改回 stock Linux 的语义（尊重 `PROT_WRITE|PROT_EXEC`，加固改成
`a20.wx=` 上的选项）。

**这条与内存模型无关**：它在迁移前的代码上同样复现。不要把它记成迁移的成绩，
也不要把它记成迁移的锅。

### 12.4 审计器自己的两条误报

首轮跑出 `vma=1`，追下去发现两条规则是错的，**都不是模型的缺陷**：

* **"每个 VMA 至少有一页元数据知道"是错的。** VMA 是映射的**授权**，status 是
  **状态**。一个刚 mmap 出来没人碰过的区间，VMA 存在而元数据一个字都没有，这完全
  合法——按需调页正是整个模型赖以成立的东西。这条规则会在普通程序上开火，
  **而会误报的门禁最后只会被关掉**。改成：只在硬件确实映射了东西的时候要求一致，
  空的 VMA 合法。
* **审计只在 level 0 读 status。** 大页支撑的 VMA 在 level 0 压根没有元数据，
  于是"看起来被忘了"。顺带把审计计数器加上第一个出错地址——只有计数不给出地址，
  等于没说该读哪个 mutator。

### 12.5 仍未做（不因为上面的 PASS 而改变）

> **2026-10-04：本节全部条目已由 §13.4 与 §13.18 完成。** 下面保留当时的判断
> 与门槛原文，因为它们记录了做这件事的顺序理由；尤其"删除 VMA 必须排在 P6 之后"
> 这条依赖，后来被证明方向正确但**门槛判断错了**——见 §13.6 的更正。

* **P6：fault 分派改判 status。** `fault.c` 仍以 `mm_find_vma()` 为决策入口
  （9 处）。这正是 §11.2 更正过的说法，且 §12.3(2) 说的 COW 缺陷正是在等这一天。
* **删除 VMA。** **必须排在 P6 之后。** 删在前面会直接打断缺页路径，而 P6 又依赖
  VMA 里的 `file_vnode` / `file_offset` / `start` / `end`。根因是 status 字节
  容量：4 bit class + 1 bit COW + 3 bit prot 已经占满，装不下 vnode/fd/offset，
  也就装不下一个 44 位的 swap 条目。
* **需要补上的数据结构**：按 PT 页**懒分配**的段引用表
  （`{vnode/vmo, base_va, offset, flags}`，引用计数），配一个逐页的段号。
  之所以"懒分配"是关键：只有真正含 file/VMO 映射的 PT 页付这个钱，纯匿名负载
  一分钱不付；而 3 GiB 的文件映射只对应一个段被上千张 PT 页共享，而不是 78 万个
  VMA。论文没有给出这个格式——**P6/P7 被记为 blocked，原因就在这里。**

所以本节的 PASS 是**回归地板**，不是 P6 的完成。§6 的"明确不承诺"第 1 条仍然成立：
**VMA 抽象没有删除。**

### 12.6 P6 的具体设计（下一步照这个做）

前面 §12.5 只说"根因是 status 字节容量"。实测之后可以把根因收得更紧，而且
**收窄之后发现第一步比想象中便宜**。

#### 阻塞点不是 8 位，是"预留态只有一种 class"

`fault.c:516` 的分派入口读 `vma->vm_flags`（`VM_FILE` / `VM_SHARED` / `VM_VMO`）来
选分支。status 字节理论上能回答同样的问题（`MM_ST_FILE_PRIVATE` /
`MM_ST_FILE_SHARED` / `MM_ST_VMO` / `MM_ST_ANON_MAPPED` 四类正好对应），**但只在
已经缺过页之后才成立**：

* 预留是**默认关的**（`a20.anonprov=0`，`smoke-mm-pt-race` 得显式传 4096 才打开）。
* 打开时 `mmap.c:195` 对**所有** mmap 调 `mm_pt_provision_anon()`，一律写
  `MM_ST_ANON_VIRT`。

所以一个**从未缺页的 MAP_PRIVATE 文件映射，status 是 `MM_ST_INVALID`；开了预留也
只是 `MM_ST_ANON_VIRT`，而这对文件映射是错的**——它声称这一页将来是匿名页。

这才是 P6 的真实阻塞：**status 无法区分"预留了但还没缺页的文件页"和"预留了但还没
缺页的匿名页"**。它比"8 位满了"窄，因为 class 字段本身已经装得下答案，缺的只是
在**映射时刻**把答案写进去，以及装下答案指向的那个对象。

顺带一条已经存在的缺陷：`mmap.c:195` 把文件映射也标成 `MM_ST_ANON_VIRT`。今天无害，
因为分派读 VMA；**P6 一落地它立刻变成错页**——和 §12.3(2) 的 COW 状态不同步同一
类问题，只是还没被触发。

#### 需要的数据结构：按 PT 页懒分配的段表

`class` 说"将来是什么"，但 file/VMO 缺页还要知道"**是哪个对象、从哪个偏移**"。
这三样（vnode/vmo、base_va、offset、flags）**是按区间共享的，不是逐页的**——一个
3 GiB 的文件映射只有一个 offset 序列，它对应 786432 页。

所以正确的形状不是"逐页存一个对象指针"，而是**段 + 段号**：

```c
/* 一次 mmap 背后的对象，被它覆盖的所有 PT 页共享，引用计数。 */
typedef struct mm_seg {
    uint32_t      magic;
    volatile int  refcount;
    uint8_t       kind;      /* MM_SEG_ANON / FILE / VMO */
    int           fd;        /* FILE：持有 vnode 的引用来源 */
    uint64_t      base_va;   /* offset 对应的是哪个 VA */
    uint64_t      offset;    /* 该 VA 的文件/vmo 偏移 */
    uint64_t      flags;     /* VM_SHARED 等 */
    struct vnode *vnode;
    struct vmo   *vmo;
} mm_seg_t;

/* 挂在 pt_meta_t 上，只有真正含 file/VMO 映射的 PT 页才分配。 */
typedef struct mm_segtab {
    volatile int refcount;
    uint8_t      n;                      /* ≤ MM_SEGTAB_MAX (4) */
    mm_seg_t    *seg[MM_SEGTAB_MAX];
    uint8_t      idx[MM_PT_META_ENTRIES]; /* 0 = 无段；否则 1..n */
} mm_segtab_t;
```

> **这段定义已被下一小节推翻，保留是为了记录改错的过程。** 问题在 `idx[]` 的粒度：
> 它是**逐叶**的，写进去就得先造出叶子表，于是"按段写不遍历"是假的。

`pt_meta_t` 里只多一个 `mm_segtab_t *segtab` 指针。**"懒分配"是关键**：纯匿名负载
的 PT 页一个字节都不付；而一个 3 GiB 文件映射只对应**一个** `mm_seg_t` 被上千张
PT 页共享，而不是 78 万个 VMA。

`class` 的语义相应收紧：`MM_ST_ANON_VIRT` 变成"已预留，但没说是哪一类"，段号负责
说明。**审计器要跟着改**：`absent` 现在只接受 `INVALID / ANON_VIRT / SWAPPED`，
加了段之后要接受"节点带段号但叶上无 PTE"，并且新增一条断言"带段号的节点必须指向
一个活的 `mm_seg_t`"——**否则一个悬空段号会让缺页路径去解引用已释放的 vnode。**
这一步的顺序不能反：门禁比功能先写，否则第二天就没人抓悬空段号。

#### 先量一下：present 页的 class 到底准不准

在动任何代码之前，先把 P6 的前提**测出来**而不是假定。加了一个审计计数器
`cls_mismatch`：对每个 **present** 叶页，比较 status 的 class 与覆盖它的 VMA
所能产生的 class 是否相容。

相容**不是相等**，这一点是必须的：一个 MAP_PRIVATE 文件页在 COW 之前是
`FILE_PRIVATE`、之后是 `ANON_MAPPED`，两个都对；VMO 页同理；fork 出来的共享页同理。
所以判据是一组允许关系。但它有牙——最要紧的一条是 MAP_SHARED 文件：那里叶页
**就是** page cache 的规范帧、原地写、永不复制，所以它里面出现匿名 class 意味着
写入会落到私有帧而文件的读者还拿着 cache 帧。对称的一条同样要紧：没有文件在后的
VMA 里出现 `FILE_*` class，意味着把一个私有帧当成了 cache 页。

实测（riscv64 dev，`feat/mm-complete`，`smoke-mm-software` 跑完 git/vim/gcc/python/nodejs
关机时的审计）：

```
[MM-ASM] pt_pages=9 entries=3072 missing_meta=0 present=0 absent=0 prot=0 cow=0
         vma=0 vmai=0 cls=0 safe=0 anon_virt=0
```

**`cls=0`。** 就**已映射**的页而言，两个表示对"这是不是一页文件"完全一致——
跨五个真实软件。这是 P6 剩下的唯一缺口被收窄的直接证据：

* 已映射页：一致，可以交给 status 选分支。
* 未缺页页：class 根本不存在（`INVALID`，或开启预留时的 `ANON_VIRT`），**这一项
  测量不了，因为没有 class 可比**。

于是 P6 从"整个分派要换掉"缩成"**只需要解决未缺页页**"。而这一缩，直接把
`mm_seg` 从"优化"提升为"必需"——见下面被推翻的第 1 步。

这个计数器留在树里（`smoke-mm-pt-race` 与 `smoke-mm-software` 都断言 `cls=0`），
因为它是 P6 的门禁条件：分派改判 status 期间，`cls` 必须一直是 0，否则"status
说这是文件页、VMA 说这是匿名页"的时刻已经发生过分派选错分支。

#### 再更正一次：段号不能挂在叶子上，必须挂在 PT 节点上

上一小节说"按段写，不遍历"。**那还是错的**，而且是同一个坑的第二次。

`mm_segtab_t.idx[]` 是**逐叶**索引。要把一个页的段号写进去，那一页所在的叶子表
必须存在——所以给一个未缺页区间写 class，等于**把该区间所有 PT 页先造出来**。
3 GiB 映射 = 1536 张叶子表 + 1536 个 `pt_meta_t`，每张叶子表还要额外挂一个
`segtab`。这正是 `g_anon_prov_max` 存在的理由（实测 2 MiB 映射 512 次 cursor 往返
就已经 ~1.5x），也正是预留默认关闭的原因。

**所以 `idx[]` 挂在叶子上并没有避开遍历，只是把遍历从"每个叶子一次 cursor 操作"
换成了"每张叶子表一次 cursor 操作 + 强制物化所有页表页"。** 差别是常数倍，不是
量级。写在这里是为了别让下一轮照着它做。

#### 真正的形状：段号继承挂在 PT 节点项上

`cls` 对 `MM_ST_PT_NODE` 的那个 4 bit 现在是浪费的——节点项只需要"我是个节点"。
把**段号记在父表的节点项上**，语义是"这整棵子树由段 S 支撑"：

* `mmap` 沿区间下降时，只需要给**沿途的 PT 节点项**写段号。3 GiB 的 Sv39 映射
  经过大约 3–4 个节点，不是 786432 页，也不是 1536 张叶子表。**代价回到 O(节点数)。**
* 缺页下降时，遇到**带段号的节点项就停**，整棵子树继承这个段——这正好是
  "一次事务"该有的样子，也是论文那套 covering-node 锁真正在保护的东西。
* 审计器把"带段号的节点"当成"这棵子树由段 S 支撑"，与叶子上的 present 检查
  并列。

代价是三个必须一起解决的新问题，都不是小事：

1. **拆分**。`munmap` / `mremap` / `mprotect` 打到子区间的中间时，一个带段号的
   节点必须**拆成两个**，各带各的段号；只拆一个就等于让一半子树继承错的段。
   `mprotect` 不改段但要改 prot，`madvise(MADV_DONTNEED)` 之后段还在。
2. **下降接口**。`pt_lookup_leaf()` 的契约要变成"找到一个叶子**或**一个带段号的
   节点"，返回值从 `pte_t *` 变成"要么是叶子槽，要么是段句柄"。调用点很多，
   且当前大量调用方**要求**返回叶子——这正是 `mm_pt_leaf_table()` 那类辅助函数
   存在的原因，改契约要逐个审。
3. **fork**。子进程要一份 COW 视图：段可以共享（引用计数），但每个叶子的 COW 位
   是私有的。所以**段在节点上、COW 在叶子上**，两套粒度必须共存而不互相解释。

第 1 条是最容易写出错页的地方，也是这条路线的主要风险。

**拆分基线已经存在**，不用先补：`mm_stress.c` 里 166 处区间操作，覆盖了头切
（`munmap(mem, 3*4096)`）、尾切（`munmap(mem + 6*4096, 2*4096)`），以及
**中间切片反复 munmap + MAP_FIXED 装回**（`wide_cursor_base + workers*slice`
那段，注释写明它专门打"punching 线程拆子树"这一侧）。所以拆分语义一旦变了，
`smoke-mm-stress` 会变红——**前提是改完先跑它**，而不是跑新写的那个。

这条是查过之后才写的：本来写的是"动手前应该先锁基线"，查完发现基线已经有了，
缺的不是基线是"记得跑"。

#### 落地顺序（每一步都可独立验证）

> **第 1 步已被实测推翻两次（2026-10-03）。** 原文写"`mmap` 时刻写 class，风险低"。
> 它风险低是因为被当成一次局部改写；**但它没法便宜地做**。要在 mmap 时给未缺页的
> 叶子写 class，就得遍历该区间的每一个页：3 GiB 映射是 786432 次 cursor 操作。按
> §10 记的实测（2 MiB 映射 512 次 cursor 往返就已经 ~1.5x 回退），那量级是**秒级**，
> 而 git 一类程序一次运行要做很多次。
>
> 第一次更正以为"按段写就不遍历"，**那是第二次犯同一个错**：`idx[]` 是逐叶的，
> 写进去照样得先造出所有叶子表。所以最终形状是**段号挂在 PT 节点项上**（见上面
> 两小节），代价回到 O(节点数)。
>
> 下面重排后的顺序，**每一步都可以独立跑 `smoke-mm-software` 验证**。

1. **段 + 节点项继承落地**：`mm_seg_t` 分配/引用计数/释放 + **PT 节点项上的段号
   继承**，跟 `pt_meta` 退休链和 `munmap`/`mremap`/`madvise`/`fork` 对齐。这一步
   还没有读者，风险集中在生命周期与**拆分**。此时 `mmap` 顺带按沿途节点写段号，
   **不遍历区间**——原第 1 步和第 2 步合并成一个动作。
2. **审计器跟着改**：接受"节点带段号但叶上无 PTE"，并新增"带段号的节点必须指向
   一个活的 `mm_seg_t`"。**门禁比功能先写**，否则第二天的悬空段号没人抓。
3. **分派改判 class（影子比对）**：下降遇到带段号的节点就停，用它选分支，同时
   **保留 VMA 查询并断言两者一致**，不一致就 warn。让改动可观测，而不是静默换语义。
4. **摘掉 `mm_find_vma`**：影子比对清零之后才删 VMA 查询。
5. **删 VMA 权威**：VMAR/`/proc/maps`/`brk`/`mseal`/`mlock` 仍需区间索引，
   那时它不再是映射的权威，只是区间索引——§6 第 1 条那时才能改写。

第 3 步的影子比对是关键：**先让它不一致时喊出来，再删掉喊的那一半。** 直接换掉
就是把 P6 变成一次相信；本文件记了三次"猜错机制"的教训，不值得再来第四次。

#### 论文没有给出的部分

论文说 per-PTE 元数据是唯一权威，但**没有给出一个能装下对象引用的布局**。上面这个
段表是本树自己的设计，不是论文的实现。§11.2 里"CortenMM 不依赖 VMA"这句话在这
一点上是**没有论文依据的**——如果论文真的逐页自带对象引用，8 位 status 加一个
可选指针解释不了 44 位的 swap 条目（§6 第 7 条的 blocked 理由），说明它也有
一个本文没读到的补充结构。**在把 P6 说成"移植论文设计"之前，应该先把论文的
数据结构读清楚。**

---

## 13. 段表落地、影子比对与 P6 第 3 步（2026-10-04，`feat/mm-complete`）

§12.6 把 P6/P7 记为 blocked，理由是"论文没有给出能装下对象引用的布局"。这一节
记录那个布局在本树落地之后**实测**到了什么，以及其中哪些假设被自己的数据推翻。

### 13.1 影子比对：把"段表够不够用"从断言变成数字

`mm_pt_shadow_seg()` 在每一次文件/VMO 缺页上问段表"你会怎么说"，再和 VMA 实际的
说法逐字段比对（kind / shared / 对象偏移），计入三个计数器：

```
seg_ok   段表答得上来，且与 VMA 一致
seg_diff 段表答得上来，但与 VMA 不一致   ← 真实缺陷：会缺错页
seg_miss 段表答不上来                    ← 不是缺陷，是待补的覆盖缺口
```

一个关键的方法学教训：**插桩位置必须是缺页真正走的路径**。第一版插在
`handle_demand_fault_locked()`，而文件缺页先被 `handle_demand_fault_access()` 的
`VM_FILE` 分支截走，那个函数只在 decline 后才被调用——于是计数器全是 0，看起来
像"完美通过"，实际上一条都没记到。代码里留了注释，防止再犯。

### 13.2 插桩本身先找出三个真缺陷

**其一：annotate 走查缺读侧临界区。** `seg_annotate_rec()` 当时只靠 `mm->lock`。
但 `mm->lock` 挡不住另一颗 CPU 上的 cursor 把子树折叠、回收再复用，走查因此可能
拿到已释放的描述符——连续三次在 `mcs_lock()` 上死于 `0xffffffff00000000`。
两个走查现在都在 `mm_pt_read_enter()/exit()` 内。**不是** `mm_addrspace_lock()`：
后者会分配页表页，而 munmap 正在拆的就是那些页表。

这里还记了一次错误排查：第一次假设不成立时，我反汇编了内核 ELF，结果反汇编的
二进制和运行中的对不上（地址与 `sepc` 不匹配），符号化的回溯把挂载线程标成了
`ethernet_output`——**那个回溯是假的**。后来按"先加 magic 校验再二分"定位到真正的
问题。

**其二：mmap 不建页表路径，所以 mmap 时刻的标注等于没标。** 走查只下降进**已存在**
的节点，而那一刻一个都没有。实测 `seg_ok=6532 / seg_miss=37626`——段表只答得上
15%。改成由 VMA 持有自己的段、在第一次缺页建出部分路径后重新贴标
（`mm_mmap_seg_label()`），覆盖率升到 `35210 / 8950`。

**其三：fork 复制了段指针却没取引用。** `mm.c` 的 fork 路径 `*cv = *pv;` 把
`vm_area_t.seg` 这个**自有引用**一起结构体复制了，父子各 drop 一次，同一个段被
free 掉，而对方的页表还在指它。

另外查出并修掉一个**先前就存在**的缺陷：`mm_pt_meta_clone()` 从来没复制 `safe[]`，
所以 fork 出来的子进程丢掉 `MM_SAFE_NO_FA`，可以在父进程承诺过"不会有人缺进来"
的区间里缺页。

### 13.3 `seg_diff=6`：一个节点条目记不下两个映射

修完上面三个，`seg_diff` 还剩 6。六次形状完全一样：

```
va=0x10c5db000
  vma    : [0x10c5da000,0x10c5dc000) off=0xa000 fd=16
  own seg: base=0x10c5da000 len=0x2000 off=0x9000      ← 与 VMA 自洽
  found  : base=0x10c5d0000 len=0xc000 off=0x0        ← 另一个映射的段
```

判别性的一问是"**found 的 base 还归活着的 VMA 所有吗**"——是，就说明这不是悬垂，
而是**节点条目比映射粗**：Sv39 的 level-1 条目覆盖 2 MiB，而两次独立的 mmap 经常
并排落在同一个条目里（加载器把库映在 offset 0，紧接着把下一个对象映在
offset 0x9000）。一个条目只能记一个名字，于是后者的第一次触碰解析到了前者的段，
会按**正确的文件、错误的偏移**缺进一页。

这不是"陈旧标注"也不是"算错偏移"——中间我按偏移差恒为 `0x1000` 怀疑过 munmap
的四个分支和 `vma_split()`，逐个核过，全都是对的。**几何上两者不可能同时成立，
所以只能是 VMA 本身就是应用自己要的偏移**。

修法不是加状态，是承认条目能装下更多：`mm_segtab_t.idx[]` 本来就是一个字节，
现在把最多 `MM_SEGTAB_MAX` 个段号**四个一组压进去**（每 nibble 一个，0 表示空），
查找时逐个试，取**记录的范围真的覆盖该地址**的那一个。两个映射共用一个条目于是
仍然可以分别回答；退回 VMA 只留给真正无法判定的地址，而那时查找会**拒绝作答**
而不是随便挑一个。

`seg_diff 6 -> 0`，覆盖率同时上升（`36296 / 7870`）。

### 13.4 P6 第 3 步：分派改由段决定，并证明它不是空转

段里带着文件缺页路径需要的一切——fd、sharedness、prot、**该页自己的**对象偏移、
以及映射的结尾——所以当它覆盖该地址时，它**就是**权威。VMA 仍然查，理由有两条，
都是临时的、也都是承重的：它是段还没覆盖到的地址的**回退**，以及
`shadow_seg_check()` 统计两者每一次不一致。

`mm_pt_lookup_seg()` 现在自带读侧临界区，调用方不需要 cursor；影子比对**去掉了**
自己的 `mm_addrspace_lock()`——**测量不能改变被测的东西**：那个 cursor 会先把被查
地址的页表路径造出来，于是段表会拿到"查找自己制造出来的覆盖"这份信用。

一个没人能量化的改动等于没改，所以两条臂都计数并打印：

```
seg shadow: 36073 agreed, 8184 uncovered, 0 disagreed (9 annotated node entries);
dispatch took the segment 36073 times and fell back to the VMA 8184
```

两对数字**完全相等**，这正是要害：段能回答时，分派就照段的做，段答不出时才回退，
且这次回退被记了下来。`seg_diff=0` 说明在 36073 次实际由段决定的缺页上，段与 VMA
一次都没分歧——这是保留这段代码的安全依据。

### 13.5 一次被数据否掉的改动（如实记录）

试过在 `mm_pt_annotate_seg()` 里**先补齐中间级节点**再标注，理由是 mmap 不建路径。
结果覆盖率**没有改善**：`seg_miss` 在补齐前后分别是 7556 / 7834 / 7957 / 8080，
全在逐次运行的抖动范围内。这套代码已经删掉了——它每 GiB 文件映射要多付一个页，
换不到任何可测量的收益。

它为什么没用，当时**判断错了**：以为缺口是"中间级节点不存在"。§13.8 用分类计数器
把这件事一次测死了。

### 13.6 仍然没做的（不因为上面的 PASS 而改变）

* **P6 第 4 步：摘掉 `mm_find_vma()`。** `fault.c`/`cow.c` 里还剩 11 处，全树 57 处。
  门槛是 `seg_miss` 归零；§13.8 把它从 8184 压到 4864（约 11%），并测出了剩下那
  部分的构成。**仍未归零，所以这一步没做。**
* **删除 VMA 权威。** 仍然必须排在 P6 之后。而且即使 P6 完成，VMAR / `/proc/maps` /
  `brk` / `mseal` / `mlock` 仍然需要一个区间索引——那时 VMA 不再是映射的**权威**，
  只是区间索引，§6 第 1 条"不承诺删除 VMA 抽象"那时才能改写。

  > **2026-10-04 更正：本节设的门槛是错的。** 当时定的门槛是"`seg_miss` 归零"，
  > 而 `seg_miss` 此后不降反升（§13.15 删掉一个与调用点契约矛盾的 provision 换来
  > 覆盖率，残余缺口随之变大；最终 1046/43157，约 2.4%）。第 4 步与删除 VMA
  > **仍然做了**，因为真正的前提不是"段表能回答全部地址"，而是
  > **"段表答不出时回退到的必须是同一个真相"**——合并之后回退目标与段表
  > 是同一条 `mm_seg_t` 记录，回退不再可能与分派分歧，`seg_diff` 因此恒为 0。
  > 门槛从"覆盖率"改成"回退一致性"，这件事才成立。见 §13.18。
  **本节的所有 PASS 都是回归地板，不是这两件事的完成。**

### 13.7 这一节没有推翻的东西

§11.2 那句"CortenMM 不依赖 VMA"在**这一点上仍然没有论文依据**。段表是本树自己的
设计；论文说 per-PTE 元数据是唯一权威，却没给出能装下对象引用的布局，8 位 status
加一个可选指针解释不了 44 位的 swap 条目。在把 P6 说成"移植论文设计"之前，仍应
先把论文的数据结构读清楚（§12.6 末尾同此结论）。

### 13.8 段表填满就放弃走查——一个真实的上限，以及它值多少

`segtab_slot()` 在某个节点的表装满 `MM_SEGTAB_MAX`（4）个段之后返回 0，而
`seg_annotate_rec()` 把 0 当成"放弃这个区间"的理由，直接 `break` 出走查。
在**根表**上，这意味着它的四个槽位是**整个地址空间**的上限：装满之后，之后每一个
映射都贴不上标签，它上面的每一次缺页都只能回退到 VMA。

而子节点**自带一张表、自带四个空槽**，并且 `mm_pt_lookup_seg_rcu()` 本来就会在
"这一层的名字不覆盖该地址"时继续下降——所以挂在下一层的名字和挂在本层一样找得到，
只多走一个节点。改成继续下降而不是放弃：

```
seg_miss  7870 -> 4864   (-38%)      seg_diff 仍然为 0
```

顺带把 §13.5 那个错误判断一并纠正了。分类计数器给出的结果是
`diag = 0 / 56418 / 39396`，三个分量依次是"下降途中遇到空洞或叶子"、"走到最底层
仍然没有名字"、"这一层有名字但不覆盖该地址"。**第一个是 0**——页表路径从来不是缺口，
所以"先补齐中间级节点"（§13.5 被否决的那个改动）从一开始就不可能有用，这也解释了
它为什么测不出收益。剩下的缺口是"表在某一层装不下"，属于布局问题，不是 bug。

### 13.9 一个自己看不见自己的 bug：索引比它的打包宽度窄

`seg_miss` 从 4864 往下压不动了，于是回头读打包代码，发现 `mm_segtab_t.idx[]` 是
`uint8_t`，而 `segtab_nib_set()` 往里塞 `MM_SEGTAB_MAX`（4）个 4 bit 的段号。
**4 × 4 = 16 位塞不进 8 位**，而且截断是静默的：那个内联函数把结果 cast 回参数类型，
于是第 3、第 4 个段号写进去、读出来是 0。无论表里装了多少段，一个条目最多只能记
**两个**。

这件事没有任何一层能发现，因为**每一层读的都是同一个已经被截断的字节**：标注走查
拿到的是刚发给它的槽号，查找读回的是同一个字节，审计器读的也是同一个字节。整个
系统对一个**已经被扔掉的值**保持自洽。这也正好解释了为什么前面无论怎么改走查，
`seg_miss` 都不动——改的是走查，坏的是宽度。

改成 `uint16_t`，并把所有装打包索引的局部变量统一成 `segtab_packed_t`，再用
`_Static_assert` 把宽度和 `MM_SEGTAB_MAX` 绑在一起：下一次不匹配会是**编译失败**，
而不是运行期的一次静默截断。segtab 本来就占满一整个页（约 550 字节的状态），
多出来这一个字节不要钱。

```
seg_miss  4864 -> 3740 / 3796   (两次运行, -23%)      seg_diff 仍然为 0
```

这一条也是 §13.1 那条方法学的最好反例：**"跑出来的数字没变化"不等于"改动无效"**。
中间那几次否决（§13.5）之所以可信，是因为它们各自测出了具体数字；而这一次的
"测不出变化"我本来也快当成噪声收下了，是回头读代码才找到的。**门禁只能告诉你
"没变"，不能告诉你"为什么没变"。**

### 13.10 这一节的净结果

| | 段表落地时 | 现在 |
|---|---|---|
| `seg_ok` | 6532 | 40459 |
| `seg_miss` | 37626（84%） | 3796（8.6%） |
| `seg_diff` | 6 | 0 |
| `seg_dispatch` | — | 40459（== `seg_ok`） |

段表现在能独立回答 91% 的文件缺页，而且**在这 39392 次里与 VMA 一次都没有分歧**。
剩下不到 9%，构成已经测出（§13.8）。删除 VMA 权威仍然不做——
理由与 §13.6 相同，而且现在多了一条**正面**证据：路径不缺（§13.8 实测为 0）、表不缺，唯一缺的是容量，
而容量这件事上刚刚已经找出并修掉了**两个**真实缺陷。所以 P6 第 4 步是一个有界的
布局工作量，而不是一个未知。

> **2026-10-04 更新：本节结论已被 §13.18 推翻。** 第 4 步与删除 VMA 权威都做了，
> 而且不是因为上面的容量工作落地了——是因为 §13.16–13.18 先把两个表示合并成一条，
> 于是"摘掉 `mm_find_vma()`"从一次赌注变成一次机械替换（54 处调用点换成
> `mm_seg_find()`），"删除 VMA 权威"变成删除一个已无第二种表示的类型。

### 13.11 残余的 8.6%：一次被 frame 算术带偏的诊断

§13.8 把残余缺口归因为「每个节点页能命名的段数不够」，并据此论证
`MM_SEGTAB_MAX` 是整个地址空间的天花板。这个诊断**是错的**，而且错得很有说服力：
算术完全自洽（`sizeof ≈ 8 + MAX*8 + 512*idxwidth`，16 槽要 4229 字节、越界），
只是算错了对象。

加了两个常驻计数器之后：

```
annot lost: table_full=7132  nibbles_full=0
```

**每条目名字从未用尽过一次（0）**，满的始终是被 512 个条目共享的那个 `seg[]` 数组。
原因在 §13.8 自己的注释里写着：Sv39 上 level-1 条目是 2 MiB，一个节点页跨 1 GiB，
8 个段覆盖不了一个进程在一个 GiB 里的全部映射。**顶住上界的是共享，不是宽度。**

这个误判本身值得记一笔：它符合 frame 算术、越界数字能算出来、方向听起来也对，
唯一能推翻它的是「分别测一测两个限额」。而这两个测量此前不存在——
`nibbles_full` 和 `table_full` 是同一个循环里相邻的两行。

#### 13.11.1 两个限额必须拆开

`MM_SEGTAB_MAX` 同时扮演两个角色：**槽位编号的位宽**（进 `idx[]`）和**共享数组的
容量**（受 frame 限制）。拆成两个之后：

- 段数组挪进自己的 `mm_segarr`（255 槽，独立 frame，引用计数），
  于是共享容量不再是 per-node-page frame 的函数；
- `idx[]` 因此可以变宽，per-entry 上限也跟着抬。

`idx[]` 同时从「打包整数」改成**扁平字节数组**。原来的截断就住在
`segtab_nib_set()` 回写参数类型的那次 cast 里；把打包类型加宽只是把下一次出错的
宽度往上挪，一位一槽的数组则根本没有宽度可错。

`mm_segtab_t` 自己的 `refcount` 也在这一步删掉了：segtab 只有一个持有者
（free 它的那个 `pt_meta_t`），这个计数写了从没读过，而它正好占着加宽索引需要的
8 字节。

| 改动 | `seg_miss` | `table_full` | `nibbles_full` |
|---|---|---|---|
| 段表落地时 | 37626（84%） | — | — |
| §13.8 + §13.9 后 | 1992（4.5%） | 7132 | 0 |
| 段数组拆出（255 槽） | ~900 | **0** | ~5900 |
| `idx[]` 加宽到 7 名/条目 | **~270** | 0 | ~2900 |

三次连续复跑 260 / 304 / 263，`seg_diff` 全程 0。每一档都是单独测出来的，
`table_full` 与 `nibbles_full` 交替成为瓶颈——**每一步只是挪动了天花板，没有取消它**。

#### 13.11.2 剩下的 ~270 是分辨率，不是容量

`miss why` 现在 100% 是 `extent`：条目**有**名字，但没有哪个名字的区间覆盖该地址。
在 2 MiB 的条目里放超过 7 个映射，它本来就无法说明哪个是哪个；查表拒绝作答而不是猜。

`idx[]` 是 512 项、住在同一个 order-0 frame：7 项/条目是 3592 字节，8 项是 4104、
越界 8 字节。这不是还能调大的常数。要继续缩小缺口只有两条路，都不是调参：

1. **提高分辨率**——给叶子条目命名（代价是每个被标注范围都要物化叶子表，
   即 §13 开头否掉的 eager provisioning）；
2. **把索引挪出 frame**——让 `idx[]` 自己也是一个间接结构。

在这两条之一落地之前，删除 VMA 权威仍然不做。理由与 §13.6 相同，但现在是**两条正面
证据**：路径不缺、表不缺，唯一缺的是一个**已知的、被测量过的**分辨率上限，
而不是一个还不知道是什么的问题。

> **2026-10-04 更新：这两条路最终都没走，删除 VMA 权威还是做了**（§13.18）。
> 因为分派正确性并不需要段表覆盖全部地址——它只需要**回退时与分派出自同一条记录**，
> 而 §13.16–13.18 的合并正是把这件事变成了构造上的必然。残余的分辨率上限
> （今天 1046/43157，约 2.4%）仍然存在，它现在是**性能特征**而不是**正确性风险**：
> 那些缺页安全回退，代价是慢一点。

#### 13.11.3 顺带修掉的一个真缺陷

`segtab_entry_forget()` 的压缩循环原本是 `uint8_t s; s <= a->n`。`a->n` 上限变成
255 之后，`s++` 在 255 处回绕成 0，循环**永不退出**——guest 挂在第一个 git 阶段。

值得记的是：这个界在 `MM_SEGTAB_MAX=8` 时永远碰不到，所以它在代码里是对的，
直到数组长大。**一个由常量决定是否可达的边界，只有在那个常量变化时才会暴露。**

`used` 位图同理从 `uint16_t` 改成真位图：255 槽下 `uint16_t` 会把槽 1 和槽 17
混为一谈，静默释放某个活跃条目仍在引用的段。

### 13.12 净结果

| | 段表落地时 | §13.10 | 现在 |
|---|---|---|---|
| `seg_ok` | 6532 | 40459 | 43989 |
| `seg_miss` | 37626（84%） | 3796（8.6%） | 265（**0.6%**） |
| `seg_diff` | 6 | 0 | **0** |
| `seg_dispatch` | — | 40459 | 43989（== `seg_ok`） |
| `table_full` | — | — | **0** |
| `nibbles_full` | — | — | ~2900 |

段表现在独立回答 **99.4%** 的文件缺页，且**一次都没有和 VMA 分歧**。
`miss why` 把剩下的 0.6% 归到唯一一个成因（`extent` = 条目名字的区间不覆盖），
`annot lost` 把成因归到唯一一个上限（每条目 7 名，受 frame 限制）。

全量门禁：`check-mm-lock-model`、`check-mm-pt-lock-order`、`smoke-mm-stress`、
`smoke-mm-pt-race`、`smoke-wx-aslr`、`smoke-mm-software` 全绿，git/vim/gcc/python/
nodejs 五个阶段内容校验全部 PASS，审计除 `seg_miss` 外全 0。

### 13.13 第 4 步（摘掉 `mm_find_vma`）的可行性：它不是一次删除，而是一次重排

`seg_miss` 降到 0.6%、`seg_diff=0` 之后，P6 第 4 步看起来该做了：段表既然能独立
回答 99.4% 的缺页，那 55 处 `mm_find_vma` 就该能被段替换。这一节把这句话拆开
量一遍，结论是**不能靠删除完成**，理由有三条，都可复算。

**其一：55 处调用不是同一种查询。** 按查询形状分成两类：

| 形状 | 站点数 | 典型位置 | 段表能否回答 |
|---|---|---|---|
| 点查询「这个地址背后是谁」 | 约 42 | `fault.c`、`cow.c`、`madvise.c`、`trap.c`、`pt.c` | 能，`seg_diff=0` 已证 |
| 有序枚举「按 VA 顺序遍历映射」 | 13 | `mprotect.c`、`mseal.c`、`sys_mm.c`、`mremap.c` | **不能** |

枚举这一类之所以不能，不是覆盖不够（0.6% 的缺口照 §13.12 也够它们退回 VMA），
而是**段表没有顺序**。索引挂在页表节点条目上，一个条目最多记 7 个名字（§13.12），
彼此可以任意重叠；`mm_seg_t` 之间没有 `next`，也没有任何按 VA 排序的结构。要从
`mm_pt_lookup_seg(mm, addr)` 得出「addr 之后的下一个映射」，唯一的办法是把段集
合排序——那就是 VMA 换个名字。

**其二：13 处枚举里有 4 处根本不用 `next`，但仍然要顺序。**
`mprotect`/`mseal` 要判断 `[start,end)` 是否**整体**落在一个映射内，这需要知道
`end-1` 属于谁；`mm_find_vma` 一次二分就给出答案，而段表只能逐地址问。

**其三：VMA 上有段表表达不了的字段。** 全树实际读取的字段计数：

| 字段 | 读取处 | 段里有吗 |
|---|---|---|
| `start` / `end` / `vm_flags` / `pte_flags` | 155 / 122 / 148 / 41 | 有（`base_va`/`len`/`flags`） |
| `file_vnode` / `vmo` / `file_offset` / `file_fd` | 30 / 24 / 21 / 52 | 有（`vnode`/`vmo`/`offset`/`fd`） |
| `vmar_cap` | 5（`abi/native/vmar.c`、`sys_native_mm.c`） | **无** |
| `sysv_shmid` | 4（`ipc/sysv_shm.c`） | **无** |
| `sealed` 状态 | `mseal.c` 全套（24 处引用） | 有——`VM_SEALED` 是 `vm_flags` 的一位，`flags` 已经带着它 |
| `prev` / `next` / `vma_index[]` | 71 / 5 / 25 | **无**，且这是有序性本身 |

> **更正**：这张表初稿把 `sealed` 列成缺失，**这是错的**。`VM_SEALED` 是
> `vm_flags` 的第 18 位，`mm_seg_t.flags` 装的正是 `vm_flags`，所以段一直都能
> 表达 seal 状态；`MM_SAFE_NO_FA` 是它在页表侧的投影，不是它唯一的落点。
> 真正缺 `vmar_cap` 和 `sysv_shmid` 两格，已按 §13.14 补上。

`vmar_cap` 是 native VMAR 的保护上限（`protect()` 不得越过创建时的能力），
`vma_index[]` 是 `mm_struct` 里那份扁平有序数组
（`vma_index_state` 三态：脏 / 有效 / 超出 256 个容量）。这两样都不是「备份对象」，
而是**映射的策略元数据 / 排序结构**，段表按设计不管它们——前者已补进段，后者要另建。

**因此「删掉旧内存模型代码」不能等于删掉 `vm_area_t`。** 路线图第 5 步自己的措辞
是「把 VMA 降级为纯区间索引」——降级不是删除，结构留下。可删的那部分是
**VMA 作为缺页判定权威**的角色，而它已经在第 3 步删掉了：分派由段决定，
`seg_dispatch=43989`、`seg_diff=0`，VMA 只在 0.6% 的缺口上被问到。

要真正走完第 4 步，得先**加**两样东西，都不是删代码：

1. 给 `mm_seg_t` 补策略元数据（`vmar_cap` / `sysv_shmid`）——§13.14 已做；
2. 造一个按 VA 有序的段集合——也就是第 5 步要的那个索引，**未做**。

第 2 条决定了第 4 步和第 5 步其实是同一件工程，做完第 2 条，第 4 步自然完成。
在此之前摘掉 `mm_find_vma`，会同时打断 VMAR、mseal、SysV 共享内存和 `/proc/maps`
四条仍然在跑的路径，而它们没有任何一个在段表的测量里出现过。

还有第三样，是本节漏掉才由 §13.14 查出来的：**匿名映射至今没有段**。
`mm_mmap_seg_annotate()` 对没有 `VM_VMO | VM_FILE` 的映射直接 return，
`MM_SEG_ANON` 在全树只被审计器当成「无段」哨兵用，从来没有被真正构造过。
所以段集合**不是映射的全集**——堆、栈、匿名 mmap 全都不在里面。
一个只装着文件映射的有序索引，永远答不了「`[start,end)` 是否被覆盖」，
因为 anon 那一半根本不在。这比有序性本身更早地把第 2 条挡死。

**状态：第 1 条已做（§13.14），第 2、3 条未做，且都是已测量的阻塞，不是估计。** 复算命令：

```
grep -rc "mm_find_vma(" --include=*.c kernel/     # 55 处 / 17 个文件
grep -rn -A1 "mm_find_vma(" kernel/mm/mprotect.c kernel/mm/mseal.c \
      kernel/abi/linux/sys_mm.c | grep -c '\->next'   # 13 处枚举
```

### 13.14 段补上策略元数据，并更正 §13.13 的一条断言

§13.13 把「删掉 VMA」拆成了三样必须**先加**的东西。这是第一样。

**更正。** §13.13 的字段表把 `sealed` 列成段表达不了的，这**是错的**。
`VM_SEALED` 是 `vm_flags` 的第 18 位，而 `mm_seg_t.flags` 装的就是 `vm_flags`，
所以段一直都能表达 seal 状态；`MM_SAFE_NO_FA` 只是它在页表侧的投影，不是唯一落点。
真正缺的只有两格。

**改动。** `mm_seg_t` 增加：

```c
uint32_t vmar_cap;    /* native VMAR 的保护上限，0 = 无上限 */
int32_t  sysv_shmid;  /* SysV shmid，-1 = 非 SysV 共享内存 */
```

由 `vma_seg_set()` 在段建好之后盖章——`mm_seg_build()` 拿到的是备份对象而不是
VMA，看不到这两样，所以盖章必须发生在真正要进页表的那个段上。改完之后，一份
映射的全部信息（区间、备份对象、标志、VMAR 上限、SysV 身份）都在段上。

`sysv_shmid` 的默认值是个坑：`mm_seg_alloc()` 用 `memset` 清零，而 **0 是一个
合法的 SysV shmid**，所以没设置过的段会自称 shmid 0。在 `mm_seg_alloc()` 里显式
写 `-1`。

**顺带查出来的、比有序性更早的一道墙：匿名映射没有段。**
`mm_mmap_seg_annotate()` 对没有 `VM_VMO | VM_FILE` 的映射直接 return，
`MM_SEG_ANON` 在全树只被审计器当「无段」哨兵（`pt.c` 的 `kind != MM_SEG_ANON`），
**从来没有被真正构造过**。也就是说段集合不是映射的全集：堆、栈、匿名 mmap
全都不在里面。这一点 §13.13 没看到，它比「缺有序性」更早地把「有序段索引取代
VMA」挡死——只装着文件映射的索引答不了「`[start,end)` 覆盖了吗」，因为 anon
那一半根本不在里面。

所以第 4/5 步真正的先后顺序是：

1. ~~给段补策略元数据~~（本节，已做）；
2. **让匿名映射也有段**——否则段集永远不是全集；
3. **建按 VA 有序的段索引**，取代 `vma_index[]` 与 `mm->mmap` 的区间索引职责；
4. 转换 55 处 `mm_find_vma`；
5. 删掉 `vm_area_t` 与 `vma.c`。

第 2 步不是可有可无的铺垫：不做它，第 3 步建出来的索引天生残缺，第 4 步照抄
`mm_find_vma` 的语义就会在 anon 区错。**状态：第 1 步已做（构建通过），2-5 未做。**

> **2026-10-04 更新：五步现已全部完成**——第 2 步 §13.15（`ab8e9e99`）、第 3 步
> §13.17、第 4 步（54 处调用点换 `mm_seg_find()`）与第 5 步（删 `vm_area_t`）
> 都在 §13.18 的 `b4e106856` 里。原文那句"2-5 未做"只对当时的提交成立。
>
> 顺带更正原文一处：第 5 步写的"删掉 `vm_area_t` 与 `vma.c`"——`vma.c` **没有**被删，
> 也没被改名。它仍在，只是里面管的已经是 `mm_seg_t`：文件名的 `vma` 现在是遗留命名，
> 内容是那条唯一的映射记录。真正删掉的是 `vm_area_t` **这个类型**、
> `vma_index[]` 与 `mm_find_vma()`。

### 13.15 让匿名映射也有段：四个潜伏缺陷因此变成一次崩溃（`ab8e9e99`）

§13.14 说段集合不是映射的全集，anon 是第 4/5 步的前置条件。做这一步的结果是
**门禁在第一个 git 阶段就挂掉**，而它顺带挖出四个真实缺陷——它们此前都存在，
只是 anon 的密度把它们从「几乎撞不上」变成「立刻撞上」。这一节记四个缺陷，
以及最后为什么 anon 仍然不进节点索引。

**崩溃长什么样。** 不是段表答不出来，是一个野跳转：`sepc=0x2f0a7d203b303220`。
按 ASCII 读是 `/\n} 02 `，是源码文本，不是代码。`mm_seg_t.release` 是函数指针，
从一帧被回收的内存上读出来，就只能是那块内存现在装的东西——页缓存里的文件文本。

**怎么定位的。** 先证伪「无关的 ASLR 抖动」：把 anon 关掉重跑，门禁 `exit=0`、
五个阶段全过，所以 anon 确实是诱因。再在 `mm_seg_put()` 里查 magic，
把一次无从归属的野跳转变成一次指名道姓的 panic。计数没用——计数器只在关机审计里
打印，而这个故障在审计之前就把 guest 杀了。

#### 缺陷一：`mm_pt_lookup_seg_rcu()` 释放了它从未取得的引用

```c
if (ambiguous) {
    mm_seg_put(hit);      /* hit 是从 segtab 数组借来的 */
    return NULL;
}
...
if (hit)
    return mm_seg_get(hit);   /* 只有成功路径才取引用 */
```

`segtab_seg()` 返回的是**借用**指针，引用归 segtab 的共享数组所有。歧义分支没取过
引用就 put，等于花掉了数组那一份，段随即被释放，而活着的条目还在叫它。下一次查表
就读到已释放的帧。

这条路径此前**几乎不可达**：歧义要求两个活的映射的区间同时覆盖一个地址，而只有
文件映射有名字，文件映射很少相邻。anon 一进来就常驻相邻，立刻触发。

#### 缺陷二：`mm_split_vma_at()` 让两个 VMA 释放同一个段

```c
*tail = *v;          /* seg 是 OWNED 引用，结构体拷贝把指针一起抄了 */
                     /* 没有 mm_seg_get */
```

两边各自 `mm_seg_put`，多出来的那一次把段释放掉，而页表还在用它。这是同一个缺陷的
**第三个现场**——`vma_split()` 和 fork 的拷贝都已经处理过，只有这里漏了。
它能活下来是因为 mprotect 切分 VMA 的频率远低于别的切分；anon 有了段之后，
堆和栈被 mprotect 是常事，于是常驻触发。

修法要小心方向：`tail` 从来没有持有过引用，所以是**置 NULL 而不是 put**——
put 掉的是头部那一份合法引用，接着头部自己重建段时就会 put 一个已释放的段。
这个错误我犯了一次，是 site 标记（`site=3`）把它指出来的。

#### 缺陷三：`vma_split()` 两半都不重新标注 → `seg_diff=6`

mprotect 实际用的是 `vma_split()` 而不是 `mm_split_vma_at()`，而它切完之后
两半都没有新段：尾半没有段，头半的段仍是切分前的区间。查表正是按区间匹配的，
所以邻居解析到了别人的段——门禁的影子比对直接量到 `seg_diff=6`。
现在 mprotect 两处切分都重新标注两半。

#### 缺陷四：标注走查自带的 provision 与它自己的契约矛盾

`seg_annotate_rec()` 带一个 `provision` 标志，`mm_pt_annotate_seg()` 传 1，
于是**为一个映射标注会把整段区间的页表节点建出来**——而它调用点的注释写的恰恰是
「annotating an untouched range must not bring page tables into existence just
to label them」。矛盾在只有文件映射被标注时是负担得起的：数量少、区间小。
anon 也被标注之后，每一个堆/栈/匿名 mmap 都长出一整棵页表树，guest 内存耗尽，
git 收到 SIGSEGV，然后内核 panic。已删除。

#### 为什么 anon 最后仍然不进节点索引

四个缺陷修完之后，anon 全开时门禁**是过的**（五阶段 PASS、`seg_diff=0`），
但覆盖率掉了：

| | 只文件/VMO | anon 也标注 |
|---|---|---|
| `seg_miss` | 1067（2.4%） | 1572（3.6%） |
| `table_full` | 0 | **42384** |
| `nibbles_full` | 3961 | **47965** |

原因是容量，不是正确性：一个节点条目在 Sv39 上是 2 MiB、至多记 7 个名字，
那个共享预算是**稀缺的**；而 anon 的段**从不参与分派**（分派只认 `MM_SEG_FILE`），
把预算花在没人读的名字上是净亏。

所以 anon 的覆盖不能靠这个索引——只能靠 §13.14 第 3 步那个**有序段索引**，
它按映射建、不受 2 MiB 粒度限制。`mm_mmap_seg_annotate()` 的守卫保留，
并把上面这张表写在守卫旁边，免得下一个人再试一遍。

#### 顺带的一笔账：`seg_miss` 从 265 变成 1067

缺陷四删掉 provision 是有代价的：原来 265（0.6%）的覆盖里有一部分正是 provision
买来的（一个尚未被 fault 建出来的节点，本来只能等下一次 fault 才被命名）。
现在是 1067（2.4%）。这个交换是划算的——provision 换来的是一个与自身契约矛盾、
且在 anon 下无界的分配；2.4% 的 fallback 全部安全回退到 VMA，`seg_diff=0`。

**最终状态**（`ab8e9e99`，riscv64 真实软件门禁）：

```
[mmtest] stages: PASS (git vim gcc python nodejs)
[mmtest] audit: clean (9 PT pages, 3072 entries, 0 anon_virt)
[MM-ASM] pt_pages=9 entries=3072 ... seg_slots=4 seg_bad=0 seg_kind=0
         seg_ok=43193 seg_diff=0 seg_miss=1067
         seg_dispatch=43193 seg_fallback=1067
[MM-ASM]   miss why: unnamed=143 extent=922 bottom=2
[MM-ASM]   annot lost: table_full=0 nibbles_full=3961
```

`check-mm-lock-model`、`check-mm-pt-lock-order`、`check-doc-test-gates`、
`check-doc-drift` 全绿。**状态：第 2 步（anon 全覆盖）未做，且已证明不能由节点索引承担；
第 3 步（有序段索引）成为下一步的唯一入口。**

### 13.16 第 2 步做成了：段集终于等于映射集（`17e5dc608`）

§13.15 的结论是"第 3 步的唯一入口"，但第 2 步本身还欠着。先补它，因为
**没有它，第 3 步建出来的索引天生残缺**——有序段索引用来回答"这段地址被
覆盖了吗"，而当时 14 个 VMA 里有 10 个根本没有段。

#### 13.16.1 先量，不先改

在审计器里加了一个临时计数器：对每个 VMA 检查 `v->seg` 存在，且
`base_va/len` 与 `v->start/end` 精确相等。门禁自己的进程给出：

```
[MM-ASM]   seg extent: vmas=14 mismatch=0 noseg=10
```

`mismatch=0` 是个好消息：**凡是存在的段，范围都精确对得上**。缺口纯粹是
"不存在"——10 个 VMA 根本没有段。堆、栈、brk 都在里面。

#### 13.16.2 两个被混为一谈的决定

§13.15 的匿名实验之所以炸（`table_full=42384`、`seg_miss` 265→1572），是因为
当时把两件事用**一个** `VM_VMO|VM_FILE` 守卫挡掉了：

| 决定 | 含义 | 该由谁决定 |
|---|---|---|
| **建段** | 这个映射可被表示——段集必须是映射集的全集 | 所有映射，无例外 |
| **标注节点索引** | 这个映射值得占 2 MiB / 7 个名字里的一个槽，而那张表只被分派器读 | 只有会被分派的 kind |

守卫加在 `mm_mmap_seg_annotate()` 开头，于是**建段跟着标注一起被跳过了**。
现在拆成 `seg_dispatchable()`：段无条件建，只有 `VM_VMO|VM_FILE` 才标注。
这正是当初直接删掉整个守卫会付 `table_full=42384` 代价的那条分界线。

#### 13.16.3 建段点放进 `mm_insert_vma()`，而不是补九个调用点

九个 `mm_insert_vma()` 调用点里有**六个**建了匿名映射却从不标注：brk 增长
（`munmap.c`）、ELF 的栈和 bss（`elf.c`）、SysV shm、两条 framebuffer 路径、
io_uring。逐个补是六个补丁，而漏掉任何一个，索引就又缺一块——而且缺口是
静默的。所以放进 `mm_insert_vma()`，它是所有映射必经的唯一漏斗。

它必须跑在合并**之后**：幸存者未必是传进去的那个。与前一条合并会把传进去的
那个吸收掉，此时范围变长的是**前一条**。对着被 `mm_vma_defer` 的映射建段，
等于给一个已经在等释放的对象建了个新段，而真正的幸存者还在描述它合并前的
范围。三个 `mmap.c` 里"insert 之后标注"的调用因此删掉了——它们既冗余，又
标错了对象。

#### 13.16.4 vdso 需要一个"只建不标"的入口

`vdso_append_vma()` 是链接逻辑的**私有副本**（它同时服务 exec 期的 image 列表，
那不是一个 mm），所以拿不到漏斗里的建段。vDSO 和 vvar 两张映射都是
`VM_PFNMAP|VM_DONTFORK`，门禁里它们到审计时 `seg` 为 NULL。

`mm_seg_attach()` 就是这个入口：建段，不碰页表。段是映射自身的状态，image
列表上那份同样需要；需要 mm 的只是标注。

#### 13.16.5 这个不变量现在是常驻门禁，不是临时计数器

```
[mmtest] stages: PASS (git vim gcc python nodejs)
[MM-ASM]   seg extent: vmas=14 mismatch=0 noseg=0
```

从"14 个里覆盖 4 个"到"14 个全覆盖"，且计数进了 `mm_pt_audit_errors()`——
将来任何一个绕过漏斗的 mutator 都会**让门禁失败**，而不是悄悄把洞重新打开。

`seg_miss` 1067→1034（ASLR 区间内），`seg_diff=0`。`table_full=0` 保持——
这正是拆分两个决定要保住的性质。

**状态：第 2 步已做并常驻验证。第 3 步（有序段索引）的前置条件现在成立：
每一个映射都有一个范围精确相等的段，可以按段建索引了。**

### 13.17 有序段索引建成：段与 VMA 的查询结果逐页一致（`516c0d17c` 之后）

第 2 步做完之后，第 3 步的前置条件成立了（每个映射都有一个范围精确相等的段），
于是可以建索引了。**但先量一件事**：段到底能不能顶替 VMA？

#### 13.17.1 顶替的障碍只剩一个字段，而且不能推导

把 §13.13 那 54 处 `mm_find_vma` 调用点逐个统计用到的字段，结论是段已经带上了
其中除 `pte_flags` 外的全部：`start/end`（`base_va/len`）、`vm_flags`、`file_offset`、
`file_vnode`、`file_fd`、`vmo`、`vmar_cap`、`sysv_shmid` 都在段上。剩下的
`prev/next/refcount` 是链表机制本身（由索引取代），`nommu_alloc` **零调用点**。

`pte_flags` 看起来可以从 `VM_READ/WRITE/EXEC` 重新推出来（`vm.h` 里有现成的
`mm_vm_flags_to_pte_flags()`）。**实测这个捷径是错的**：14 个映射里 12 个推导一致，
2 个不一致——vDSO 和 vvar 的 `pte_flags` 是 0，而它们的 VM 位蕴含 `VM_READ`，
推导会**静默剥掉它们的读权限**。而且 mprotect 有只写 `pte_flags` 不动
`vm_flags` 的路径，所以这个分歧是结构性的，不是这两个映射的怪癖。

因此段**显式带** `pte_flags`，在 `vma_seg_set()` 里与 `vmar_cap`/`sysv_shmid`
一起盖章。审计报出 `pte_agree`/`pte_disagree`，让"只改了一种表示、忘了另一种"
的 mutator 变成看得见的数字。

#### 13.17.2 索引形态：有序数组 + 惰性重建 + **持有引用**

和 `vma_index[]` 同形：按 `base_va` 排序的定长数组、`state` 三态
（0=脏 / 1=有效 / 2=超容量回退），由 `mm_vma_index_invalidate()` 一起失效，
所以两个索引对"自己是否最新"永远看法一致。映射表本身按 `start` 有序，而段的
`base_va` 等于映射的 `start`（审计保证），所以**顺着走一遍就是有序的，不需要排序**。

两处它和 `vma_index[]` 不同，都是必须的：

**条目持有引用，不是借用指针。** `vma_index[]` 能借用，因为 VMA 列表在整个索引
有效期内拥有那些节点。段没有这个性质：`mm_pt_unannotate_seg()` 和 segtab 自己的
驱逐都会 `mm_seg_put()` 而**不会**让这个索引失效，借用条目可能在后续二分查找
底下被释放。持有一个引用的代价是每次重建每条一个 refcount，换掉的是这一整类 bug。

**失效与释放分开。** `mm_seg_index_invalidate()` 只清 state，是每次映射变更都会
走的热路径，不能碰 refcount；`mm_seg_index_clear()` 才放引用，只在地址空间销毁时
调。重建时**先**释放上一轮再写新的——就地覆写之后再 put，put 的是已经被新条目
占用的槽位，那会放掉新条目的引用并泄漏旧的。

#### 13.17.3 fork 与 teardown：两个真 UAF 现场

`*child = *parent` 会把**持有引用的**数组一起复制过去。vma_index 复制的是借用
指针（重新构建即可），段索引复制的是**已持有的引用**——不去重置的话，子进程会去
放它从未持有的引用，从两个地址空间都在以为自己持有的角度把段释放掉。
`mm_destroy()` 里也一样：`mm_seg_index_clear()` 必须排在遍历 VMA 之前，因为
`vma_put() → vma_release() → mm_seg_put()` 会结束每个段的最后一个别的引用。

#### 13.17.4 证据：逐页比对，`idx_diff=0`

审计对每个映射的**每一页**同时问 `mm_find_vma()` 和 `mm_seg_find()`，比较的不只是
"有没有映射"，而是**同一个映射的同一个段**——两个不同映射同时覆盖一个地址，只可能
是索引没排好序。连续三次门禁：

```
[mmtest] stages: PASS (git vim gcc python nodejs)
[MM-ASM]   seg extent: vmas=14 mismatch=0 noseg=0
           pte_agree=12 pte_disagree=2
           idx_agree=391 idx_diff=0
```

三次全部 `idx_diff=0`（`idx_agree` 因 ASLR 稳定在 391）。

**这是"可以开始转换调用点"的门槛，而不是转换本身。** 目前索引已建成并被审计
使用，但 54 处 `mm_find_vma` 一个都还没换——`idx_diff=0` 把那一步从赌注变成了
机械替换。

**状态：第 3 步完成（索引建成 + 逐页等价）。第 4 步（54 处调用点）与第 5 步
（删除 `vm_area_t` 与 `vma.c`）未做。**

---

### 13.18 旧模型删除：两个映射表示合并成一条记录（`b4e106856`）

§13.16–13.17 结束时，地址空间里有**两样东西表示同一个映射**：链表上的
`vm_area_t`（slab 分配、带 refcount、区间 `[start,end)`）和页表节点条目所命名的
`mm_seg_t`（**整页 frame 一批**分配、记录后端对象）。它们的区间必须逐字相等——
§13.17 那个审计存在的全部理由就是保证这一点——而且两套查询（`mm_find_vma` 与
`mm_seg_find`）、两套索引（`vma_index[]` 与 `seg_index[]`）、两个生命周期
（`vma_put` 与 `mm_seg_put`）、两个 offset 字段（`file_offset` 与 `base_va`）
并存。本节把它们合并成一条记录 `struct mm_seg`，`vm_area_t` 与第二个表示随之消失。

**这一节取代 §13.17.4 末尾的"第 4/5 步未做"。** 下面是做法与它逼出来的后果。

#### 13.18.1 为什么不是一个 union 或一个别名

因为两边的**分配粒度不同**：段是 page frame 批量分配（省下的是页表节点里那张
名字表，不是段自己的开销），VMA 是 slab。一个记录要同时吃到两种分配器的长处，
代价是段回到了 slab——按每个映射一次 `kcalloc_atomic(1, sizeof(*m))`。
实测这个代价不成立为问题：一条记录 128 字节量级，而一个映射至少占一个 4 KiB 页，
**每映射的额外开销是 3%**，换来的是少一半的分配、少一个表示、少一次相等性校验。

记录因此定义在 `kernel/include/mm/vm.h` 而不是 `pt.h`：它的字段全是 `VM_*` 位的
消费者（`vm_flags`、`pte_flags`、由 `vm_flags` 推导 kind/shared），而 `VM_*` 定义
在 `vm.h` 里。`pt.h` 反过来 `#include "mm/vm.h"`（已确认不构成环）。

#### 13.18.2 四个被迫改掉的东西

合并不是少写一个类型，四处**各自都不再可选**：

1. **一个索引、一次查询。** 两个索引保序前提相同、失效时机相同、内容逐项相同，
   留两个等于留两份必然漂移的真相。删 `vma_index[]`/`MM_VMA_INDEX_CAPACITY`/
   `mm_find_vma`/`mm_vma_index_invalidate`，`mm_seg_find()` 接管 54 处调用点
   （这是 §13.17 的 `idx_diff=0` 换来的机械替换）。`mm_seg_find()` 现在**按记录自己
   的 `[start,end)` 二分**，返回的就是那个记录，链表、索引、查询三者合一。
2. **kind/shared 变成推导函数。** 原来是一个字段，现在 `mm_seg_kind()` 与
   `mm_seg_shared()` 从 `vm_flags` 算——一个真值来源，而不是两个必须同步的字段。
3. **offset 统一成 `backing_offset`。** `file_offset`（文件内字节）与
   `base_va`（VA）曾经是两个字段。统一前把所有接收者列了一遍：三处不是映射指针
   （`procfs_render.c` 的局部 `rec->file_offset`、sys_native_pager 的 `kargs.`、
   elf 的 `src->fd.`），必须排除。
4. **一个构造函数 `mm_seg_new()`。** 合并后"忘记设 magic"从风格问题变成 UAF
   ——见 §13.18.4。

#### 13.18.3 两个合并**逼出来**的潜伏缺陷

**延迟释放队列从此是唯一出口。** 段自己的释放会 `vfs_close()`（会阻塞）。合并前
`mm_seg_put()` 只从索引重建和 teardown 走，都是整片上下文；合并后它还会从**缺页
路径**和 `mm->lock` 之下被调用。所以 `vma_put()` 不再直接释放：它取一份自己的
引用、入 `mm->deferred_vma` 队列，由 `mm_vma_flush_deferred()` 统一跑
`vma_release()` 再 put。队列因此持有自己那份引用——否则在队列上的一条记录会被
后续某次 put 在脚下释放。

**切分必须先 retire 再收窄。** 一个区间从中间切开时，旧实现把 `*tail = *vma` 之后
靠段表驱逐生效；现在 PTE 节点条目**直接指着这条记录**，收窄之后再标注会让两半各自
看到对方半边的旧名字。`munmap.c`/`mprotect.c`/`vma.c` 的每处切分改成
**retire 整段 → 收窄 → 依次标注两半**（先头后尾），`mm_pt_unannotate_seg()` 负责
按地址摘名字而不是按对象猜。

#### 13.18.4 门禁抓到的真缺陷：四个绕过构造函数的分配点

`check-mm-lock-model` 挂在 `pid=3 telnetd` 上：

```
KERNEL PANIC: mm_seg_put: use-after-free, mapping record 0xffffffc0bf4876c0 magic=0x0
```

`magic=0x0` 说明它**从来没被初始化过**，不是被释放两次。这与 put 侧无关，于是改成
在 `seg_free` 里记 `__builtin_return_address(0)`——回来的 `freed_by=0x0`，证明
`seg_free` 从未在这个地址上跑过。于是定位到四个用裸 `kcalloc` 而不是构造函数的地方：
`vdso.c:192,193`（vDSO 与 vvar，它们进 `mm->mmap`、会被索引，因此**每次 exec 必崩**）、
`elf.c:164`、`sysv_shm.c:324`、`io_uring.c:136`。四个全部改走 `mm_seg_new()`。
`vm.c:651` fork 的暂存池**保持**裸 `kcalloc`（`*cv = *pv` 之前它完全不作为记录存在），
并加了注释说明为什么不改。

#### 13.18.5 证据

```
[mmtest] stages: PASS (git vim gcc python nodejs)
[mmtest] audit: clean (9 PT pages, 3072 entries, 0 anon_virt)
[MM-ASM] pt_pages=9 entries=3072 missing_meta=0 present=0 absent=0 prot=0 cow=0
         vma=0 vmai=0 cls=0 safe=0 anon_virt=0 seg_slots=4 seg_bad=0 seg_kind=0
         seg_ok=43081 seg_diff=0 seg_miss=1084
         seg_dispatch=43081 seg_fallback=1084
[MM-ASM]   map list: entries=14 overlap=0 dead=0 ok=14
[MM-ASM]   miss why: hole=0 leaf=0 unnamed=143 extent=939 ambig=0 bottom=2
[MM-ASM]   annot lost: table_full=0 nibbles_full=3976 full_by_level=[0,0,0]
```

`map list` 一行取代了原来的 `seg extent` + `idx_agree/idx_diff`：合并之后没有第二个
表示可比对，**可校验的不变量变成链表本身**——有序、互不重叠、每条记录 magic 活着
（`dead`）、且覆盖它自己的 PTE（`ok`）。14 个映射全在（两表示时期是"14 个里 10 个
没有段"）。

`check-mm-lock-model` 14 条断言、`check-mm-pt-lock-order` 27 条断言全部 PASS，
`smoke-mm-software` `EXIT=0`。

**状态：第 4、5 步完成。`vm_area_t`、`vma_index[]`、`mm_find_vma` 与第二个映射表示
均已删除；映射只有一条记录、一个索引、一次查询、一个 offset 字段、一个构造函数。**
`kernel/mm/vma.c` 仍在，但存的是 `mm_seg_t`——它现在管的是映射记录本身，不是第二种表示。

---

### 13.19 合并回 main：两个只有合并才会暴露的缺陷（2026-10-04，`main`）

`feat/mm-complete` 合回 `main`（`84b7bc2c5`）时两边已分叉约 200 个提交，`main` 上
有一整套文件所有权模型是照着**旧的两种表示**写的。调解本身不是本节的重点
（记录改为持有 `main` 的 `struct vfile *file` 而非裸 `file_fd`，其余保留本分支的
单记录/单 offset 形状；`vma_split()` 取 `mm` 并自行作废索引是 `main` 的修正）。
本节记的是**两个只有两边合到一起才暴露出来的缺陷**——它们都不是合并写坏的，
而是各自一直错，只是要等另一边进来才看得见。

#### 13.19.1 `MM_SEGTAB_NAMES` 是一个按单一架构调好的常数

`pt.c` 里那条静态断言本来是防越界的，它确实也拦下了一次越界：段数组内联时
16 槽是 4229 字节，改成每条目 8 个名字后是 4104，都超过一帧。修法是把名字数
从 4 降到 7。**但 7 是被 `512`（Sv39 节点页的条目数）除出来的，不是被想出来的**：
`4096 - 8 = 4088`，`4088 / 512 = 7`。

riscv32 的节点页是 **1024** 条目。同样写死的 7 意味着 `1024 × 7 = 7168` 字节，
超出它被分配的那一帧——断言拒绝编译。这不是我的分支与 `main` 冲突造成的，
在合并之前 riscv32 就已经构建不了；只是合并让所有架构一起被构建，问题才浮上来。

修法不是给 riscv32 特判一个 3，而是**把宽度算出来**：

```c
#define MM_SEGTAB_NAMES \
    ((int)(((PAGE_SIZE - sizeof(struct mm_segarr *)) / (size_t)MM_PT_META_ENTRIES) \
           > (size_t)MM_SEGTAB_MAX ? (size_t)MM_SEGTAB_MAX : \
           ((PAGE_SIZE - sizeof(struct mm_segarr *)) / (size_t)MM_PT_META_ENTRIES)))
```

riscv64/aarch64/x86_64/loongarch64 得 7（与原来相同），riscv32 得 3，ppc64le 得 7，
arm32 得 15——没有人替后两个做过决定，它们只是落到了该落的地方。

**教训是可迁移的**：一个按某一个架构的页表形状调出来的常数，是一个在别的架构上
必然错的常数。它当时能通过全部门禁，仅仅因为没有任何门禁去构建 riscv32。

#### 13.19.2 `cow[]` 位图：同一个事实的第二份拷贝

五软件门禁在合并后第一次跑出 `audit: DIRTY {'cow': 153}`。**153 个"不一致"里，
内核全是对的，错的是审计自己**：它读的是 `pt_meta_t.cow[]` 位图，而每一个真正的
写入者（`pt_map_cls()`、`mm_pt_sync_status()`）写的是状态字节里的 `MM_ST_COW_BIT`。
位图只被 `mm_pt_set_cow()` 写——而**那个函数全树没有调用者**。

也就是说：一份每个 PT 页 64 字节的第二表示，只有一个读者（审计），而它读的
永远是陈旧值。陈旧在哪些页上暴露？fault-around 从 page cache 别名出来的那批
可执行/只读私有页——正好是真实软件最密集命中的那一类。

这**正是本模型要消灭的失效模式**：同一个事实的两份拷贝靠人手保持同步。上一节
刚把映射记录的两种表示合并成一种，紧接着在状态元数据里又留下一份同类的重复。
删掉 `cow[]` 及其两个访问器（`mm_pt_cow` / `mm_pt_set_cow`）之后，`cow=0`。

对比之下 `safe[]` 位图**保留**：它是每 8 项 1 bit（`MM_PT_META_ENTRIES + 7) / 8`），
尺寸与条目数无关，而且这两项判定（UFFD 覆盖、禁止 fault-around）**在状态字节里
确实放不下**——8 位已全部分配完毕。`cow` 是每项 1 bit，换来的是一个本来就能从
状态字节直接读出的事实。这两者的区别不在"位图"这个形状，而在**它是不是一份
冗余的拷贝**。

#### 13.19.3 顺带修掉的：main 自己的 riscv32 回归

`kernel/fs/diskfs/lfs_vfs.c` 里的 libgcc 垫片把 64-bit 助手写成 `unsigned long`。
libgcc ABI 把 `__bswapdi2` / `__ctzdi2` / `__clzdi2` 的签名在**所有目标上**固定为
`long long`，所以这个写法在 64-bit 内核上编译通过，在 riscv32 上让 GCC 直接以
`<< 56` 拒绝。改成 `uint64_t`。与本次迁移无关，但它挡着 riscv32 被验证。

#### 13.19.4 合并后的验证

| 项 | 结果 |
|---|---|
| `kernel-only -Werror` | riscv64 / aarch64 / x86_64 / loongarch64 / ppc64le / riscv32 全部构建通过 |
| `check-mm-lock-model` | 14 条断言 PASS |
| `check-mm-pt-lock-order` | 27 条断言 PASS |
| `check-doc-test-gates` | PASS（含 `check-doc-drift`、`check-doc-citations` 及全部 smoke） |
| `smoke-mm-software` | **PASS** |

`smoke-mm-software` 的最终读数：git / vim / gcc / python / nodejs 五个阶段全过，
审计 10 个 PT 页 / 3584 条目**全部计数器为 0**，映射链表 14 条、有序、无重叠、
无 dead 记录，段分派取页表名 **43067** 次 / 回退 **1102** 次 / **不一致 0**。

注意最后一行：1102 次回退**仍然是回退**，仍然是 `mm->mmap` 在回答。
§13.11 的 8.6% 与 §13.12 的净结果没有被这次合并改变，原因看起来也仍然是几何——
Sv39 的段名在 **2 MiB** 分辨率上，一个条目里放不下的映射数量是常量调不走的。

> **本节已被 §13.20 修正。** 上面最后半句是错的，而且错在一个能说清楚的地方：
> "2 MiB 分辨率"是几何，但"常量调不走"是关于**这个常量为什么取 7** 的结论——
> 而 7 从来不是被选出来的，它是页帧剩下的余数（§13.19.1）。
> 把索引移出页帧之后，常量第一次真正可选，回退随即从 1102 降到 767。
> 真正的几何限制只有 2 MiB 分辨率那一条，它现在的准确名字是 §13.20.4 的
> `MM_MW_EXTENT=625`。

### 13.20 把索引移出页帧：`MM_SEGTAB_NAMES` 从页帧算术变成选定常数（2026-10-04，`main`）

§13.19.4 结尾写着一句当时有证据、现在已被推翻的断言：

> Sv39 的段名在 2 MiB 分辨率上，一个条目里放不下的映射数量是常量调不走的。

这句话把两件事混成了一件。"2 MiB 分辨率"是真的几何限制，而"常量调不走"
是一个**关于这个常量为什么取 7 的**结论——而 7 从来不是被选出来的，它是页帧剩下来的余数。
把索引从页帧里移出去之后，常量第一次成为一个真正可选的参数，于是它一调就走。

#### 13.20.1 先补上测量：一个看不见自己分布的计数器

`mm_seg_annot_lost[1]`（条目名字用尽）**不记录层级**，`mm_seg_full_lvl[level]`
只在共享数组用尽的那一支递增。于是门禁输出是一行自相矛盾的话：

```
annot lost: table_full=0 nibbles_full=4329   full_by_level=[0,0,0]
```

4329 次名字丢失，层级直方图全零，因为名字用尽的那一支根本没往里写。
在**不知道损失发生在哪一层**的情况下讨论 2 MiB 分辨率，是在讨论一个没有分量的猜测：
层级 0 意味着"每 GiB 放不下 7 个映射"，层级 2 意味着"每 2 MiB 放不下 7 个"，
两者的修法完全不同。补上这一行之后读数是：

```
full at level 1: 749
full at level 2: 3169
```

根层**一次都没有**。所以标注 walk 确实下到了 2 MiB 分辨率，损失的也不是根表那四个槽位
（`table_full=0` 已经说了这件事），而是 2 MiB 条目里塞了超过 7 个映射。
这就是"常量选错了"的证据，而不是"几何挡住了"的证据。

> **这一段是错的，§13.21 更正了它。** `ARCH_PT_ROOT_LEVEL` 在 riscv64 上**就是 2**，
> 所以 "level 2: 3169" 与"根层一次都没有"不能同时成立——**那 3169 次全都在根层**，
> 是 1 GiB 分辨率，不是 2 MiB。少核对了一行"根层是 level 2"，
> 把一个 1 GiB 的问题读成了 2 MiB 的问题。
>
> 结论本身（"常量选错了，不是几何挡住了"）**侥幸是对的**，因为宽度 7→32 确实把
> 损失从 3918 压到 159；但中间那句推理是错的，而它直接导致了 §13.20 把下一步
> 指错了方向。正确的读法是：损失集中在根层，说明宽映射**只被命名在 1 GiB 上**。
> 至于这该靠"下钻"还是"更宽的索引"来治，§13.21 用实跑数据回答了——两个都不行。

#### 13.20.2 改动：`mm_segtab_t` 从"一个页帧"变成"一次 kcalloc"

旧结构把 `arr` 指针和 `idx[]` 数组内联在一个 order-0 页帧里，于是

```c
#define MM_SEGTAB_NAMES \
    ((PAGE_SIZE - sizeof(struct mm_segarr *)) / MM_PT_META_ENTRIES)
```

这个"策略参数"变成了**别人的内存布局的函数**。riscv32 的节点页是 1024 项，
7 × 1024 = 7176 字节超过一帧，静态断言直接拒绝编译；arm32（256 项）与 ppc64le（512 项）
分别落在 15 和 7 上，而没有任何人决定过这两个数应该是多少。
为了让宽度涨到 8 就得从结构体里删掉 segtab 自己的引用计数（`4104 = 4096 + 8`），
这已经是**为了迁就一个布局而删一个生命周期字段**了。

现在 `arr` 与 `idx` 是两次独立分配：段本身从 slab 走，索引是一次
`kcalloc(1, sizeof(st) + MM_PT_META_ENTRIES * MM_SEGTAB_NAMES)`，
索引就是这块分配的尾部——所以它不是第二次分配，也就没有第二条失败路径。
同时 `segtab_detach_locked()` 里的 `pfa_free(virt_to_pfn(st), 0)` 变成 `kfree(st)`，
每张被标注的 PT 页不再白占一整个 4 KiB 页帧去装 3584 字节的索引。

顺带删掉的还有那条 `sizeof(mm_segtab_t) <= 4096` 断言。它防的是"索引悄悄越过页帧"，
而结构体现在根本不是页帧大小，这条断言已经不对应任何真实约束——留着会让人以为还有约束。

**一处真实缺陷，在改动里被顺手抓到**：`mm_pt_meta_clone()` 里
`memcpy(st->idx, src->segtab->idx, sizeof(st->idx))` 的 `sizeof` 原本是对的
（`idx` 是数组），改成指针之后它就变成了**结构体里两个指针共 8 或 16 字节**——
拷贝 8 字节无关内容、索引完全不初始化，而 fork 出来的子进程会照着这份索引去解析段。
已改成显式元素数。这类错误是"布局改形状"时最容易漏掉的一类，因为它在新代码里
**能编译、不报警、只在 fork 后表现为随机的段解析错误**。

#### 13.20.3 证据：宽度 16 与 32 各跑一次完整门禁

| 宽度 | 索引来源 | seg_fallback | nibbles_full | 缺失原因分布 |
|---|---|---|---|---|
| 7 | 页帧余数 | 1036 | 3918 | extent=891 |
| 16 | 独立分配 | **796** | 1992 | extent=655 |
| 32 | 独立分配 | **767** | **159** | extent=625 |

两级台阶：16 把损失砍半，32 几乎把它清空（3918 → 159）。
**159 是这一节真正要说的数字**：到这个量级，索引已经**不再是约束**，
残余回退的 767 次里有 625 次是 `MM_MW_EXTENT`——条目命名了一个**不覆盖该地址**的段，
那是 §13.20.4 的分辨率问题，不是名字不够。也就是说，**加宽索引能买的已经买完了**，
再翻一倍买到的还是同一个 2 MiB 的墙，只是每张被标注的 PT 页多背 16 KiB。

两档门禁都是 `smoke-mm-software` **PASS**（git / vim / gcc / python / nodejs），
审计 10 个 PT 页 / 3584 条目全部计数器为 0，`disagreed=0`，
`check-mm-lock-model` 14 条、`check-mm-pt-lock-order` 27 条、
`check-doc-test-gates` / `check-doc-drift` / `check-doc-citations` 全 PASS。
六个架构 `kernel-only -Werror` 全部构建通过——其中 riscv32 在这个改动之前
连编译都过不了，而它过不了的原因正是这个被当成策略的页帧余数。

#### 13.20.4 剩下的 767 次回退，现在有了一个准确的名字

修正 §13.19.4 的那句话：**几何限制是真的，但"常量调不走"是错的**，
因为那个常量从来不是被选出来的。当前残余的准确拆解是：

- `extent=625`：条目命名了一个不覆盖本地址的段。**2 MiB 分辨率的直接后果**，
  修法是标注叶表（4 KiB 分辨率），不是更宽的索引；
- `unnamed=140`：该条目根本没有名字，属于"这条路径还没被标注"，与容量无关；
- `bottom=2`：走到了叶表底部。

这条拆解本身是有用的：它把 §13.20 这一档改动**做完之后剩下的工作量**，
从一个模糊的"8.6% 回退"缩到了一个具体的、已经命名的设计缺口——
**标注的分辨率停在 2 MiB，因为叶表条目太小，装不下一个名字**。
这和 §13.11 当初"要不要让匿名映射也有段"的问法是同一类问题，
但现在它的代价是可以先算出来的：每个 4 KiB 叶条目一个名字，
等于给每张叶表加一张索引表，那是比 767 次回退大得多的代价，
所以它的取舍应该在**真的要用它**的时候再决定，而不是在回退数字好看的时候决定。

### 13.21 收益计算：三条路的实测回报，以及 §13.20 的一处自我更正（2026-10-04，`main`）

§13.20.1 里有一句写错的话，而且它把结论带偏了整整一节。原文是：

> 749 at level 1 and 3169 at level 2, none at the root.

`ARCH_PT_ROOT_LEVEL` 在 riscv64 上**就是 2**（`kernel/arch/riscv64/include/page_table.h:11`），
所以 "level 2: 3169" 与 "根层一次都没有" 不能同时成立——**那 3169 次全部发生在根层**，
即 1 GiB 分辨率，不是 2 MiB。少写了一行"根层就是 level 2"的核对，
让一个 1 GiB 的问题被读成了 2 MiB 的问题，而这两者的修法完全不同。

更正之后重做收益计算。下面每一条都是实跑门禁的数字，不是估算。

#### 13.21.1 候选与实测回报

| 方案 | 收益（seg_fallback / 44k 次分派） | 代价 | 判定 |
|---|---|---|---|
| 基线（宽度 7，索引在页帧里） | 1036 | — | — |
| 索引移出页帧 + 宽度 16 | 796 | 每张被标注 PT 页 8 KiB 索引 | 采纳 |
| 宽度 32 | **767** | 每张被标注 PT 页 16 KiB 索引 | **采纳（当前）** |
| 宽度 64 | 未测 | 32 KiB/PT 页 | 按趋势外推只剩 ~10 次，**否决** |
| 下钻进所有已存在的子节点 | **775（比基线更差）** | 每次门禁多走 **299848** 个节点 | **否决（实测）** |
| 给叶表标注（4 KiB 分辨率） | 上界 792 → **260** | 每张叶表 2 KiB；1 GiB 工作集 = **1 MiB**，4 GiB = **4 MiB** | **不采纳（见 13.21.3）** |

"下钻进所有已存在的子节点"这一条是本节唯一**实现并实测**的候选项，因为它在纸面上最像
正解：`seg_annotate_rec()` 过去只在映射的区间落进**一个**条目时才下钻，跨多个条目就返回，
而门禁显示有 **353908** 个已存在的子节点因此从未被下钻过（32771 个区间）。
实测结果是这个改动**没有带来任何覆盖收益**（767 → 775，落差在噪声内），
却让标注 walk 多走了 299848 个节点。理由也说得通：这些子节点之所以存在，
是因为某次缺页已经把它们建出来了，而那次缺页**紧接着就会调用 `mm_mmap_seg_label()`**
重新标注一遍——所以"没下钻"的那部分标注，第一次缺页之后本来就已经补上了。
这是一个**看起来像泄漏、实际是已有机制在兜底**的候选，实测把它证伪了。

#### 13.21.2 决定性的一次数值：回退是一次性的，不是反复的

在动手之前先问了一个能一次性决定结论的问题：**回退路径上补的那一次标注，真的生效吗？**

如果补完立刻能查到，说明残余回退是"每个映射只发生一次"，
它的总量就等于被创建过的映射数与条目数，**和运行多久无关，也和压力多大无关**——
那么继续在分辨率上加码就是在优化一个一次性成本。
测量方式：回退路径调完 `mm_mmap_seg_label()` 之后立刻再查一次同一个地址。

```
extent cause: alone=252 shared=530 relabel_ok=782 relabel_fail=0
```

`relabel_ok=782 relabel_fail=0`：补标注**每一次都成功**，没有一次失败。
所以 792 次回退是 792 个"第一次"，之后同一映射上的每一次缺页都走页表名。

顺带把 `extent` 拆成了两类，因为它们的修法相反：
`alone=252`（覆盖该地址的根条目上没有别的映射 → 那次标注压根没发生，
提高分辨率**帮不上忙**）与 `shared=530`（同一根条目上挤着别的映射 → 条目放不下名字，
**只有这一类**提高分辨率才可能收掉）。这正是更正"根层就是 level 2"之后才能问对的问题。

#### 13.21.3 结论：不采纳给叶表标注，理由是收益上界与代价比

按最乐观的算法（`shared` 全部收掉，`alone` 一个不收），叶表标注把 792 降到 **260**，
降幅 67%。这是纸面上剩余方案里收益最大的一条。但它被否决，理由是代价比：

- **收益是一次性的**（13.21.2）。这 792 次不会随工作负载增长，它们是"被创建过的映射数"
  这个常量。把它压到 260，压的是一个**不随时间增长的常数**，
  而代价是**每张叶表 2 KiB**——叶表数量随进程内存**线性增长**。
  一个不增长的量换 一个随内存增长的量，方向是反的。
- 1 GiB 工作集 = 512 张叶表 = **1 MiB** 索引；4 GiB = **4 MiB**。
  而现在整个门禁跑完只有 **10 张 PT 页**，总索引不到 200 KiB。
  也就是说，为了省 532 次一次性缺页，要按工作集大小**持续**付出几 MiB。
- 更关键的是它**换不来模型的简化**。叶表标注要回答的问题是"这一页属于哪个映射"，
  而那正是 `pt_meta_t.cls[]` 已经在回答的问题。CortenMM 的整个论点就是
  **每 PTE 的元数据是唯一权威、不需要另一个命名结构**；在叶表上再挂一张名字表，
  是把刚刚删掉的 `vma_index[]` 以另一种形式请回来，
  只是这次住在页表里而不是内核堆里。为了 67% 的一次性回退，代价是**方向相反的架构**。

#### 13.21.4 那么剩下的 792 次是什么，以及下一步该做什么

`relabel_fail=0` 与 `alone/shared` 的拆分一起说明：**标注机制本身没有缺陷**，
剩下的是"每个映射的第一次缺页必然问不到"这一结构事实——
问的时候页表路径还不存在，没有任何结构能回答一个还不存在的路径。
这不是缺陷，是 lazy path building 的定义。

所以 §13.20 的宽度 32 就是终点，理由是收益计算而不是直觉：
再往上（宽度 64、叶表标注）买的是一个**不增长的常量**，
而付出的是**随内存增长的结构**。

下一步因此不在分辨率上，而在**别的**两件事里，按收益排序：

1. **把回退路径本身删掉**——不是降低回退率，而是让 792 次回退不再需要
   `mm->mmap` 来回答。§13.11 的 8.6% 与本节的 1.8% 都在说同一件事：
   只要回退路径还在，`mm->mmap` 就还不能删，迁移就没有真正完成。
   这是唯一能同时降低复杂度**并且**降低回退率的方向。
2. **`alone=252` 那一类**（标注没发生）值得单独查一次：它不是分辨率问题，
   而是"哪一次 mmap/fault 没有触发标注"。这一次查的是**覆盖率**而不是容量，
   成本是一次代码走读，不需要新的数据结构。
