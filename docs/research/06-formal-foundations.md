# 形式化基础：SOS 模型、不变量与安全证明

> **本文是全部研究贡献（03/04/05）的数学模型基底**（重组自旧理论文档，保留定理编号）。定位：这是"方法/工具箱"——使预算能力、类型化通道、能力信封**可证**的基础，不单独主张为论文贡献（见 12 §2）。 **预算扩展**：本文的 $\rho_{eff}$（时态模型）即预算能力的特例。完整的预算格（类型 × 权限 × 时间 × 次数 × 传播）定义与单调衰减定理见 03；基础模型本文给出。 **证明边界（诚实声明）**：本模型面向 53 个形式化核心 syscall；当前实现为 126+ 入口，07 的 trace 分类明确覆盖 43/53。**"设计被证明" ≠ "126 个实现入口全被证明"**，二者必须分开陈述。所有证明为纸笔论证；机器检验计划见 08。

---

## 1. 系统模型

**定义 1.1（对象标识）** 对象空间 $\mathcal{O}$，类型映射 $\tau: \mathcal{O} \to \mathcal{T}$，$\mathcal{T} = \{task, thread, file, dir, socket, pipe, channel, eventq, timer, shm, device, ns, debug\}$。

**定义 1.2（权限域）** $\mathcal{R}ights = 2^{\{R, W, X, Stat, Seek, Dup, Transfer, Map, Wait, Connect, Accept, Control, Admin, Signal\}}$。类型 $\tau$ 的合法权限集 $Legal(\tau) \subseteq \mathcal{R}ights$。

**定义 1.3（Handle 表）** 进程 $p$ 的 handle 表是有限偏函数 $HT_p: \mathbb{N}_{32} \rightharpoonup (\mathcal{O} \times \mathcal{R}ights)$。良构条件 W1：$\rho \subseteq Legal(\tau(o))$。

**定义 1.4（系统状态）** $\sigma = (P, \{HT_p\}, Obj, Mem)$。

**定义 1.5（引用计数）** $refcount(o, \sigma) = |\{(p,n) \mid HT_p(n)=(o,\rho)\}| + |\{(c,m,i) \mid m \in Queued(c,\sigma) \cup Pending(c,\sigma) \land m.handles_i.object=o\}|$——HT 条目与排队/投递中消息引用共同计入。

**操作-类型-权限兼容矩阵**（`compat: OpName → 2^𝒯`，每操作所需权限位）：handle_read/write/stat/control、task_wait、event_wait、msg_send/recv、vm_map、net_connect/accept 等（完整矩阵见 07 §7）。

---

## 2. 操作语义（SOS）

框架：Plotkin SOS，规则形如 $\langle op, \sigma \rangle \longrightarrow \langle ok, \sigma' \rangle$ 或 $\langle err(e), \sigma \rangle$。

**H-CLOSE**：$HT_p(n)=(o,\rho)$ 时移除条目；$refcount$ 降 0 时同步移除对象。$n \notin dom(HT_p) \to BAD\_HANDLE$。

**H-DUP**（核心单调性保证）：

$$\frac{HT_p(s)=(o,\rho_s) \quad \rho_{req} \subseteq \rho_s \quad n_{fresh} \notin dom(HT_p)}{\langle dup_p(s,\rho_{req}), \sigma \rangle \longrightarrow \langle ok(n_{fresh}), \sigma[HT_p(n_{fresh}) \mapsto (o,\rho_{req})] \rangle}$$

**H-REPLACE**：= H-DUP + H-CLOSE 的原子组合（`Dup` 权）。

**H-CLOSE-MANY / H-QUERY**：批量关闭（单次锁获取）；查询需 `Stat`。

**O-CREATE**：`create_p(type, args)` 创建新对象，创建者获得 `Legal(τ(o))`。应用于 eventq/timer/channel/socket。

**T-SPAWN**：单步进程创建，显式 handle 注入 + 权限降级。性质 **S1（显式资源注入）**：新进程权限完全由 `handles[]` 决定；**S2（最小权限可构造性）**：对任意安全策略 $P$ 存在 handle 配置使其成立（由 H-DUP 可精确降权）。T-THREAD-CREATE：共享 HT 与地址空间，不改变 $\mathcal{I}$。

**O-DESTROY**：$refcount=0$ 时触发类型化 cleanup（channel 通知 peer_closed、eventq 清理 watch、task 级联关闭全部 handle……）。级联销毁链由 07 §5.2 保证有限步终止。

**IO-READ/WRITE**：需对应权限位 + 类型兼容。

**CH-SEND / CH-RECV**（bounded FIFO，$C_{max}=64KB$, $H_{max}=8$）：send 追加完整 $(data, handles)$ 元组并增加 queued 引用（发送方保留 handle，共享语义）；recv 取出完整元组并转交引用。**类型化扩展见 04**。

**定理 2.1（Channel 消息序）** FIFO 顺序保持。 **定理 2.2（Channel 原子性）** 单次 send/recv 对数据与 handle 转移原子，无部分投递。

**EQ-WATCH / EQ-WAIT**：watch 注册对象事件；wait 返回 pending 列表或阻塞/超时返回空。

**定理 2.3（Event 投递保证）** ring 未满则事件必追加；已满则先唤醒消费者再丢弃（中断上下文不能阻塞）。精确陈述：$|pending(q)| < ring\_cap \Rightarrow e \in pending(q)$；$= ring\_cap \Rightarrow wake\_consumers(q) \land e \notin pending(q)$。

---

## 3. 安全不变式与证明

**安全不变式 $\mathcal{I}$**：

- **I1（权限合法性）**：$\rho \subseteq Legal(\tau(o))$
- **I2（权限子集传递）**：$\rho \subseteq \rho_{granted}(o, p)$
- **I3（引用计数一致性）**：$refcount(o)$ 与 HT + 消息引用计数严格一致
- **I4（对象活性）**：$o \in dom(Obj) \iff refcount(o) > 0$
- **I5（类型安全）**：操作与对象类型兼容

**定理 3.1（安全性，已论证操作范围）** 对逐项给出规则的合法操作，$\mathcal{I}$ 在操作前后保持（对每规则逐条验证；H-DUP 的新权限子集、CH-SEND/RECV 的引用交接、T-SPAWN 的降权注入、O-CREATE 的初始化）。

**定理 3.2（Per-Handle 权限单调递减）** 同一 handle 编号生命周期内权限单调递减。*关键观察*：A20OS 没有任何操作修改已有条目的权限字段——只有删除（close）与创建（dup/replace/transfer/spawn），故 $n$ 生命周期内 $\rho$ 不变，平凡递减。**预算版本的时空统一扩展见 03 定理 3.1。**

**推论 3.2.1（Per-Process 权限上界）** 进程对对象 $o$ 的可达权限集非单调，但任何新增权限都来自某已有 handle 的权限子集（无超额授权）。

**定理 3.3（Confused Deputy 不可行性）** $A$ 经 $B$ 最多获得 $Caps_A \cup Caps_B$ 的权限。

---

## 4. 活性（精简）

**定理 4.1（Event Wait 终止性）** watch 活跃事件源 + 有限超时 → event_wait 有限时间返回。 **定理 4.2（Channel 无死锁条件）** 容量 > 0 且不同向操作同一端点 → 通信必然完成。 **定理 4.3（无引用泄漏）** 正常关闭 + endpoint 清理 → 无"无所有者孤儿对象"。 **定理 4.4（公平性前提）** 活性性质依赖公平调度假设（per-CPU EEVDF，见 `docs/eevdf-scheduler.md`）。 **L1-L4（LTL 框架）**：统一活性性质的形式化模板（07 §4.4）。

---

## 5. 并发性与原子性（精简）

- 所有操作在 spinlock 临界区内完成（H-CLOSE/H-DUP/transfer 的线性化点）。
- **定理 5.1（Handle Table 串行化）** HT 操作在单锁下原子。
- Channel transfer 的原子性由 §2 单步语义保证；并发 spawn 的发布协议（defer_ready）见 08 精化。
- 锁层级 L0-L4（07 §4）保证无死锁。

---

## 6. 内存模型（VMO/VMAR）

**VM-SHARE**：`vm_share(addr,len,ρ_grant)` 创建 SHM 对象，backing 指向源地址空间物理页。

**定理 6.1（共享内存权限一致性）** 有效保护 = `prot_map ∩ translate(rights(h))`（handle 权限 ∩ 显式 prot）。 **定理 6.2（地址空间独立性）** 无 fork，进程地址空间完全独立；共享只能经显式 vm_share + handle transfer。*对研究的意义*：这消除了"fork 隐式继承权限"的能力漏洞面（01 §3）。

---

## 7. Capability 撤销

**定义 7.1-7.2**：直接撤销（close）与传递撤销（refcount 归零）。 **定理 7.1（撤销完备性）** $o$ 被完全撤销 $\iff refcount(o) = 0$。 **定理 7.2（Task 终止资源回收完备性）** 进程终止释放其全部 handle 引用（经 channel 传出的由接收方持有，不受影响）。 **设计选择**：不做自动级联撤销（可预测性/最小权限），代之以 `ADMIN` 权限下的显式终止。

**预算撤销对比（03 定理 8.1）**：传统撤销（seL4 CNode 树遍历 $O(d \times f)$）vs 预算过期的 lookup 判定 $O(1)$ + 周期 sweep 回收 $O(H)$。前者把成本放在撤销时刻，后者把成本前移到约束设置。

---

## 8. 信息流控制

**定义 8.1（Noninterference）** 高密级操作不影响低密级可观察行为。 **定理 8.1（Capability 隔离 ⇒ 信息流隔离）** 无共享 handle 的进程间无 handle 系统信息流。

**隐信道**：资源竞争、全局状态（FS 可用空间）、网络状态。**定理 8.2（隐信道上界）** $BW_{covert} \leq BW_{cpu} + BW_{fs} + BW_{net}$——**承认残余隐信道，不冒充 seL4**（05/11 据此声明不防御 $\mathcal{A}_4$）。

**受控去分类（定义 8.3）**：channel 传 handle = 显式授权去分类（等价 BLP"向下写需显式授权"）。

**带标签模型（§8.5）**：标签格 $\{L,M,H\}$；LABEL-READ 需 $label(p) \sqsupseteq label(o)$，LABEL-WRITE 需 $label(o) \sqsupseteq label(p)$（*-property），LABEL-TRANSFER 不改变对象标签，进程标签单调递增（L1）。

**定理 8.3（$\mathcal{L}$-Noninterference）** 上述三条规则下系统满足 $\mathcal{L}$-noninterference。*证明要点*：H→L 的信息：H 不能写 L（*-property），H 传高标签 handle 给 L 后 L 因 $L \not\sqsupseteq H$ 无法读其内容（只能感知存在）。

**残余隐信道**（§8.5.4）：资源耗尽、时间、调度信道——需要硬件支持才能消除，不在承诺内。

---

## 9. 双 ABI 模块隔离（句法层）

**约束 C1（模块依赖图）**：`abi_linux → core`、`abi_native → core`；两 ABI 互不依赖；core 不反向依赖任何 ABI。

**定理 9.1（ABI 隔离）** 一个 ABI 的 bug 不影响另一个的正确性（依赖约束 C1 ⇒ 影响仅经 core 契约）。 **定理 9.2（状态空间不相交）** `State_linux ∩ State_native = ∅`（fd 表/信号表/pid 与 handle 表/eventq 独立；共享 task 为 core 层）。

> **语义级边界（更强）**：即使共享 VFS，Linux 进程也无法推断 Native handle 权限状态——见 **05 §5**（定理 1.1-1.4，含 $BridgeFree$ 条件）。该边界在 05 中升级为信封的逃逸防线 #2，此处不再重复。

---

## 10. ABI 可演进性（结构体版本化）

**定义 10.1（结构体版本兼容）** $S_m$ 与 $S_n$（$m \le n$）兼容当且仅当前 $|S_m|$ 字段偏移与类型一致。 **规则 E-APPEND / E-DEPRECATE / E-RESERVED**。 **定理 10.1（兼容性保持）** 仅用合法规则演进 ⇒ 旧程序在新内核正确运行（旧字段偏移/类型不变、reserved 位为零）。 **定理 10.2（编号稳定性）** 稳定 syscall 编号在 major version 内不变（0x0000-0x0FFF 分区，单射）。 **定位**：设计经验而非论文贡献（02 §4）。

---

## 11. 与实现的关系（覆盖边界）

| 项 | 数值 |
|----|------|
| 形式化核心 syscall | 53 |
| 07 trace 分类明确覆盖 | 43（缺 task_wait/task_kill/task_info、4 path、event_wait、net_*) |
| 当前实现入口 | ~126-134 |
| 未加入 SOS 的实现入口 | 全部新增工程 syscall |

**规则**：本文结论只适用于被建模的核心操作。把本模型结论外推到任意实现入口之前，必须先补 SOS 规则与 error-path 精化（07 §8.5.1）。收口矩阵维护见 08 §4。
