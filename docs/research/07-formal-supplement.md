# 理论补充：精化、并发与实现对应

> **本文是 06 的补充**，主题是"证明的模型如何对应到运行的 C 实现"（精化）、以及并发/内存层面的补充论证（重组自 07-deep-theory-supplement.md，并吸收 08-architecture-deep-dive.md 中 SOS→实现映射的部分）。这是 08-verification 的素材来源——本文的每条纸笔论证都是待机器检验的对象。

---

## 1. Trace Induction（轨迹归纳）

**定义 1.1（执行 Trace）** 状态序列 $\sigma_0 \to \sigma_1 \to \ldots$，每一步由一条 SOS 规则驱动。
**定理 1.1（安全性的轨迹归纳证明）** 若 $\mathcal{I}$ 在 $\sigma_0$ 成立且每条规则保持 $\mathcal{I}$，则 $\mathcal{I}$ 在任意可达状态成立——把 06 定理 3.1 的操作级证明升级为轨迹级。

## 2. 复合操作的失败与回滚

**定义 2.1（回滚语义）** 复合操作 $op = op_1; op_2; \ldots$ 在某步失败时回滚已生效副作用。
**定理 2.1（预验证-提交原子性）** 若 $op$ 的每个前置条件可在第一步验证且不依赖中间状态，则预验证+提交与整体原子等价。应用于：
- **T-SPAWN**：预验证所有 handle 权限/类型 → 创建 → 提交（失败时 `elf_load_info_discard()` 回收映像，见 08 §5）。
- **CH-SEND/CH-RECV**：reserve-then-dequeue 无部分投递（recv 端 HT 满时消息留队）。

**定理 2.2（TOCTOU 防护）** handle 绑定由内核保证不变（`binding(h,o)` 在 close 前稳定），消除"检查-使用"窗口（01 §2.7 的形式化根源）。

## 3. Handle Table 溢出

**定义 3.1（溢出语义）** HT 容量 $H_{max}$；超过时 `create/dup/transfer` 返回 `NO_SPACE`。
**定理 3.1（溢出下不变量保持）** $H_{max}$ 边界下 I1-I5 仍成立；**活性**：溢出失败路径不产生悬挂状态。实际实现中 handle 表可增长（256 → 65536，quota 4096），增长路径的精化见 §8 的 ht_grow。

## 4. 锁层级与死锁自由

**定义 4.1（锁层级）** 固定部分序：`park_lock → timer_lock`、`park_lock → runq_lock`、`proc_lock → {runq_lock, signal_state, files_struct, mm_struct, a20_handle_table}`、`device locks`、`g_lwip_lock → g_net_lock`。

**定理 4.1（锁序无死锁）** 若所有路径遵循固定锁序且不持自旋锁阻塞，则无死锁。
**实现对应**：park/wake 的三锁划分（per-task `park_lock` + `g_wait_timer_lock` + per-CPU runq 锁）与无环证明见 `docs/roadmap/park-wake-protocol-split.md` §4。

## 5. 活性证明的 Variant Function

**定理 5.1（Event Wait 终止性）** variant = 剩余超时 + pending 事件数，单调递减。
**定理 5.2（引用计数归零）** variant = refcount，单调递减至 0，级联销毁链有限步终止。

## 6. Linearizability 论证

**定义 6.1** HT/Channel 操作存在线性化点（单锁临界区）。
**定理 6.1（HT 操作 Linearizability）** 每操作在临界区入口原子生效。
**定理 6.2（Channel 操作 Linearizability）** send/recv 在队列锁临界区内原子。
**定理 6.3（跨对象 Linearizability）** 多对象操作（transfer 等）在统一的临界区内序列化。

## 7. 完整类型兼容矩阵

操作-类型-权限完整映射（`compat` 的 53×13 展开）见旧 07 §7，重组后并入 06 §1 的兼容矩阵；权限必要性论证（每个权限位为何存在）保留为论文 Appendix 素材。

## 8. 精化框架（SOS → C 实现）

### 8.1 两层模型

抽象层（SOS 状态 $\sigma$）与具体层（C 数据结构 `task_t`、`a20_handle_table`、channel 端点）之间的精化关系。

**定义 8.1（精化映射）** $R: \text{State}_{conc} \to \text{State}_{abs}$ 投影具体状态到抽象状态（丢弃实现细节）。

### 8.2 已建立映射的路径

| 抽象对象 | 具体实现 | 精化要点 |
|---------|---------|---------|
| HT 条目 | `a20_handle_entry_t`（含时态字段） | 字段投影 |
| Channel 队列 | 有界消息队列 + reserve/commit 槽 | 两阶段精化 |
| EventQ | ring buffer + watch list + 全局反向索引 | 投递语义（06 定理 2.3） |
| task | `task_t` + 独立 handle 表 | spawn 发布协议（defer_ready） |
| IRQ 事件 | `event_notify` 在中断上下文 | §8.3 |

**定理 8.1（精化正确性）** 若具体实现每步精化抽象层，且抽象层安全性质成立，则具体实现的安全性质成立。

### 8.3 IRQ 安全性

**定理 12.1（IRQ 安全，event_notify）** 中断上下文的 `event_notify` 只做锁保护的事件入队 + 唤醒收集，实际 wake 在释放锁后执行——不违反锁序、不阻塞。

### 8.4 关键路径精化

**定理 13.1（ht_grow 精化保持）** 扩容（256→65536）在不变量保持的前提下重哈希，不中断并发查找。
**定理 14.1（Channel Recv 两阶段精化）** reserve → dequeue/abort → commit 与抽象层"原子取出完整元组"一致，失败时消息留队（无部分投递）。

### 8.5 Error 路径的系统化精化覆盖

**定理 8.2（Error 路径精化覆盖）** 每条 SOS 错误规则（`err(e)` 且 $\sigma$ 不变或回滚）对应到具体实现的错误返回路径。**注意**：此覆盖矩阵只覆盖 43/53 核心操作；新增实现入口的 error 路径逐项待补（08 §4 收口矩阵）。

### 8.5a 信封调解器的精化映射（05 → 实现，待 W2 完成）

C2（能力信封）在"理论-实现对应"层必须有与基础模型同等的精化映射。此处定义骨架，W2 实现后逐项填实：

| 抽象对象 | 具体实现（计划） | 精化要点 |
|---------|----------------|---------|
| 信封 policy | `a20_env_policy_t`（每进程） | 字段投影 |
| 影子 handle 表 | 复用 `a20_handle_entry` 预算字段（03） | 预算合取复用 03 的 $\rho_{eff}$ |
| 全获取咽喉 | `env_mediate()` 覆盖的资源 syscall 集合 | **咽喉完备性枚举**（08 模块 4 V5） |
| 子进程继承 | fork/clone 的预算耗散拷贝 | 继承协议（05 §2.3） |
| exec 复查 | execve 重新校验 policy | 防身份逃逸（05 §2.2） |

**待验证性质**（对应 08 模块 4）：咽喉完备性、逃逸不可行、预算耗散、刷新不可行、单调采用。

### 8.6 精化论证的局限性

- 精化映射是手工建立的，未机器检验（08 是机器检验计划）。
- C 内存模型与 SOS 的对应（§10）依赖 spinlock 原子性假设。

## 9. 并发 Trace

**定义 9.1-9.4（并发系统状态与并发 Trace）** 多进程交错执行；并发 Trace 是全局事件序列。
**定理 9.1（并发安全性）** 并发 Trace 的安全性归约到顺序 Trace。
**定理 9.2（Linearizability 归约）** 有线性化点 ⇒ 并发执行等价某顺序执行。
**定理 9.3（并发精化正确性）** 精化在并发设置下保持。
**定理 9.4（内存序精化）** 并发精化映射在适当内存序下有效。

## 10. C 内存模型与 SOS 的对应

**定理 9.5（Refcount 操作内存序正确性）** refcount 的 inc/dec 在锁保护下原子，其内存序（release/acquire）与引用生命周期对应。
**论证依赖**：spinlock 的原子性（acquire/release 语义）是本文全部并发论证的基础假设，**未机器检验**——是 08 TLA+ 建模（park/wake 模块）要覆盖的第一优先级对象。
**局限**：若引入无锁数据结构（如 event ring 的 RELAXED 优化），需补充单独的内存序论证（当前未做）。
