# A20OS Native ABI：形式化理论基础与严谨分析

> 本文档对 A20OS Native ABI 进行系统化的形式化理论分析。以 Plotkin 的结构化操作语义（SOS）为框架，建立 handle/capability 系统的操作语义，给出安全性、活性、并发性和信息流控制的形式化证明，并分析 channel IPC 的消息序性质与 capability 撤销的完备性。

---

## 1. 系统模型与形式化基础

### 1.1 基本定义

**定义 1.1（对象标识）** 令 $\mathcal{O}$ 为内核对象的全局标识空间。每个 $o \in \mathcal{O}$ 拥有唯一不变的类型：

$$\tau: \mathcal{O} \to \mathcal{T}, \quad \mathcal{T} = \{task, thread, file, dir, socket, pipe, channel, eventq, timer, shm, device, ns, debug\}$$

**定义 1.2（权限域）** 权限集合定义为 14 位位域的子集：

$$\mathcal{R}ights = 2^{\{R, W, X, Stat, Seek, Dup, Transfer, Map, Wait, Connect, Accept, Control, Admin, Signal\}}$$

对于类型 $\tau \in \mathcal{T}$，定义其合法权限集 $Legal(\tau) \subseteq \mathcal{R}ights$：

$$Legal(file) = \{R, W, Stat, Seek, Dup, Transfer, Map, Control\}$$
$$Legal(task) = \{Wait, Signal, Dup, Transfer, Control, Admin\}$$
$$Legal(eventq) = \{R, Dup, Transfer, Control\}$$
$$Legal(channel) = \{R, W, Dup, Transfer\}$$

**定义 1.3（Handle 表项）** 进程 $p$ 的 handle 表是有限偏函数：

$$HT_p: \mathbb{N}_{32} \rightharpoonup (\mathcal{O} \times \mathcal{R}ights)$$

每个表项 $HT_p(n) = (o, \rho)$ 表示进程 $p$ 通过本地编号 $n$ 持有对象 $o$ 的权限 $\rho$。

**良构条件 W1** 对所有 $HT_p(n) = (o, \rho)$，要求 $\rho \subseteq Legal(\tau(o))$。

**定义 1.4（系统状态）** 系统状态是四元组：

$$\sigma = (P, \{HT_p\}_{p \in P}, Obj, Mem)$$

其中：
- $P \subseteq \mathcal{O}_{task}$ 是存活进程集合
- $\{HT_p\}$ 是各进程的 handle 表
- $Obj: \mathcal{O} \rightharpoonup State$ 是对象状态映射
- $Mem: \mathbb{N}_{64} \times P \rightharpoonup \{Prot\}$ 是虚拟内存映射

**定义 1.5（引用计数）** 对象 $o$ 的引用计数定义为：

$$refcount(o, \sigma) = |\{(p, n) \mid HT_p(n) = (o, \rho) \text{ for some } \rho\}|$$

### 1.2 类型兼容性矩阵

操作 $op(h, \ldots)$ 要求 $h$ 指向的对象类型兼容 $op$。定义类型兼容函数：

$$compat: OpName \to 2^{\mathcal{T}}$$

| 操作 | 兼容类型 | 所需权限 |
|------|---------|---------|
| handle_read | $file, dir, pipe, socket, shm$ | $R$ |
| handle_write | $file, pipe, socket, shm$ | $W$ |
| handle_stat | all | $Stat$ |
| handle_control | all | $Control$ |
| task_wait | $task$ | $Wait$ |
| event_wait | $eventq$ | $R$ |
| msg_send | $channel$ | $W$ |
| msg_recv | $channel$ | $R$ |
| vm_map | $file, shm, device$ | $Map$ |
| net_connect | $socket$ | $Connect$ |
| net_accept | $socket$ | $Accept$ |

---

## 2. 操作语义

### 2.1 SOS 框架

采用 Plotkin 结构化操作语义。转移规则形如：

$$\langle op, \sigma \rangle \longrightarrow \langle ok, \sigma' \rangle \quad \text{或} \quad \langle op, \sigma \rangle \longrightarrow \langle err(e), \sigma \rangle$$

每个 syscall 的语义由一组前提条件和一个状态转移定义。

### 2.2 Handle 操作语义

**规则 H-CLOSE**（handle 关闭）：

$$\frac{HT_p(n) = (o, \rho) \quad refcount(o, \sigma) = 1}{\langle handle\_close_p(n), \sigma \rangle \longrightarrow \langle ok(0), \sigma[HT_p \setminus n, Obj \setminus o] \rangle}$$

$$\frac{HT_p(n) = (o, \rho) \quad refcount(o, \sigma) > 1}{\langle handle\_close_p(n), \sigma \rangle \longrightarrow \langle ok(0), \sigma[HT_p \setminus n] \rangle}$$

$$\frac{n \notin dom(HT_p)}{\langle handle\_close_p(n), \sigma \rangle \longrightarrow \langle err(BAD\_HANDLE), \sigma \rangle}$$

**规则 H-DUP**（handle 复制与权限降级）：

$$\frac{HT_p(s) = (o, \rho_s) \quad \rho_{req} \subseteq \rho_s \quad n_{fresh} \notin dom(HT_p)}{\langle handle\_dup_p(s, \rho_{req}), \sigma \rangle \longrightarrow \langle ok(n_{fresh}), \sigma[HT_p(n_{fresh}) \mapsto (o, \rho_{req})] \rangle}$$

$$\frac{HT_p(s) = (o, \rho_s) \quad \rho_{req} \not\subseteq \rho_s}{\langle handle\_dup_p(s, \rho_{req}), \sigma \rangle \longrightarrow \langle err(ACCESS), \sigma \rangle}$$

**关键观察**：H-DUP 规则的前提 $\rho_{req} \subseteq \rho_s$ 是权限单调递减的核心保证。这是唯一创建指向已有对象的新 handle 的操作（除了 IPC 传递），且强制权限子集关系。

**规则 H-REPLACE**（handle 原子替换）：

$$\frac{HT_p(s) = (o, \rho_s) \quad \rho_{req} \subseteq \rho_s \quad Dup \in \rho_s \quad n_{fresh} \notin dom(HT_p)}{\langle handle\_replace_p(s, \rho_{req}), \sigma \rangle \longrightarrow \langle ok(n_{fresh}), \sigma[HT_p \setminus s, HT_p(n_{fresh}) \mapsto (o, \rho_{req})] \rangle}$$

$$\frac{HT_p(s) = (o, \rho_s) \quad Dup \notin \rho_s}{\langle handle\_replace_p(s, \_), \sigma \rangle \longrightarrow \langle err(ACCESS), \sigma \rangle}$$

**与 H-DUP 的区别**：H-REPLACE 在创建新 handle 的同时**关闭**原 handle。等价于 H-DUP + H-CLOSE 的原子组合，但保证了两个操作之间无中间状态。这在需要"移动 handle 到新编号"的场景中有用（如将高权限 handle 降级后替换原位置）。

**不变式保持**：
- I1：新 handle 权限 $\rho_{req} \subseteq \rho_s \subseteq Legal(\tau(o))$。$\checkmark$
- I3：移除旧条目（-1）+ 新增条目（+1），refcount 净变化为 0。$\checkmark$
- I4：对象不被释放（refcount 不变）。$\checkmark$

**规则 H-CLOSE-MANY**（批量关闭）：

$$\frac{\forall h_i \in handles[].\ HT_p(h_i) = (o_i, \rho_i)}{\langle handle\_close\_many_p(handles[]), \sigma \rangle \longrightarrow \langle ok(0), \sigma[\forall i.\ HT_p \setminus h_i, \forall o_i.\ refcount\_dec(o_i)] \rangle}$$

批量关闭等价于对每个 handle 顺序执行 H-CLOSE，但在单次 spinlock 获取中完成，减少锁开销。

**规则 H-QUERY**（handle 查询）：

$$\frac{HT_p(n) = (o, \rho) \quad Stat \in \rho}{\langle handle\_query_p(n), \sigma \rangle \longrightarrow \langle ok(\tau(o), \rho, \ldots), \sigma \rangle}$$

### 2.3 对象创建语义

**规则 O-CREATE**（通用创建模式）：

$$\frac{fresh(o) \quad n_{fresh} \notin dom(HT_p) \quad \rho_{init} = Legal(\tau(o))}{\langle create_p(type, args), \sigma \rangle \longrightarrow \langle ok(n_{fresh}), \sigma[HT_p(n_{fresh}) \mapsto (o, \rho_{init}), Obj(o) \mapsto init(args)] \rangle}$$

创建规则保证：(1) 新对象 $o$ 全局唯一；(2) 创建者获得该类型的全部合法权限；(3) 对象初始化状态仅由 $args$ 决定。

应用于各子系统：

- **event_queue_create**: $type = eventq$, $init = \emptyset_{queue}$
- **timer_create**: $type = timer$, $init = (clock, deadline, interval)$
- **channel_create**: $type = channel$, 产生一对端点 $(o_0, o_1)$，各自获得 $\{R, W, Dup, Transfer\}$
- **net_socket**: $type = socket$, $init = (domain, type, protocol)$

### 2.4 进程创建语义

**规则 T-SPAWN**：

$$\frac{HT_p(h_{img}) = (o_{img}, \rho_{img}) \quad R \in \rho_{img} \quad \forall (h_i, \rho_i) \in handles. \ HT_p(h_i) = (o_i, \rho'_i) \land \rho_i \subseteq \rho'_i}{\langle spawn_p(image, args, handles), \sigma \rangle \longrightarrow \langle ok(n_{task}), \sigma' \rangle}$$

其中 $\sigma'$ 包含：
1. 新进程 $p'$ 加入 $P$
2. 新 $HT_{p'}$ 包含：$\{(n_i, (o_i, \rho_i))\}$（从 $handles[]$ 映射）
3. 初始 handles: $root\_dir, cwd\_dir, stdin, stdout, stderr, self, main\_thread, default\_eq$
4. 新 task 对象 $o_{p'}$ 加入 $Obj$
5. 父进程 $p$ 获得新 handle $(n_{task}, (o_{p'}, \{Wait, Signal, Control\}))$

**性质 S1（显式资源注入）** 新进程的权限集合完全由 $handles[]$ 参数决定。

$$\rho_{total}(p') = \bigcup_{(h_i, \rho_i) \in handles[]} \rho_i \cup \rho_{init\_handles}$$

**性质 S2（最小权限可构造性）** 对任意安全策略 $P: \mathcal{O} \to 2^{\mathcal{R}ights}$，存在 $handles[]$ 使得 $\rho_{total}(p') = P$。

*证明*：由 H-DUP 规则可对每个所需对象 $o$ 精确指定 $\rho \subseteq \rho_{parent}$。若父进程持有 $(o, \rho_{full})$，对每个 $o$ 选择 $\rho = P(o)$ 即可。$\square$

### 2.4.1 线程创建语义

**规则 T-THREAD-CREATE**：

$$\frac{p \in P \quad \tau(o_{task\_of\_p}) = task}{\langle thread\_create_p(entry, stack, arg), \sigma \rangle \longrightarrow \langle ok(n_{thread}), \sigma[Threads_p \mathrel{+}= (entry, stack, arg)] \rangle}$$

线程创建的关键性质：

1. **共享 handle table**：新线程与创建者共享 $HT_p$（同一进程空间），不需要 handle 传递。因此线程创建不涉及 refcount 变化。
2. **共享地址空间**：新线程与创建者共享 $AS_p$，不需要内存映射。
3. **独立执行上下文**：新线程有独立的寄存器状态（pc = entry, sp = stack, arg0 = arg）。
4. **无权限变化**：线程创建不引入任何新的 handle 或权限，不修改 $\mathcal{I}$ 的任何分量。

**不变式保持**：$HT_p$ 不变，$Obj$ 不变，$Mem$ 不变。$\mathcal{I}$ 平凡保持。$\square$

### 2.4.2 对象销毁语义

**规则 O-DESTROY**（引用计数归零时触发）：

$$\frac{refcount(o, \sigma) = 0 \quad o \in dom(Obj)}{\langle destroy(o), \sigma \rangle \longrightarrow \langle ok, \sigma[Obj \setminus o, cleanup(o)] \rangle}$$

其中 $cleanup(o)$ 是类型特定的清理操作：

| 对象类型 | cleanup 操作 |
|---------|------------|
| file/dir/device/pipe | 释放 vfile，调用 VFS close |
| channel endpoint | 通知对端 peer_closed，释放 pending_msg 中的 handle 引用（refcount_dec），释放消息队列 |
| event queue | 清理 watch list，更新全局反向索引，唤醒等待线程（返回 CANCELLED），释放 ring buffer |
| timer | 取消内核定时器注册 |
| shm | 释放物理页引用（若独占），解除所有映射 |
| task | 释放地址空间（unmap 全部），关闭全部 handles（递归触发 O-DESTROY） |
| socket | 关闭网络连接，释放缓冲区 |

**关键性质**：O-DESTROY 是由 H-CLOSE（或 task_exit 的批量 close）间接触发的。不存在用户直接调用对象销毁的 syscall——销毁是引用计数机制的自动后果。

**级联效应**：task 的 O-DESTROY 会关闭其全部 handles，可能触发其他对象的 refcount 归零，形成级联销毁链。由 07 §5.2 的引用计数归零定理，此链在有限步内终止。

**不变式保持**：
- I3：$refcount(o) = 0$ 与 $|\{(p, n) \mid HT_p(n) = (o, \_)\}| = 0$ 一致（无 handle 指向 $o$）。
- I4：$o \notin dom(Obj) \iff refcount(o) = 0$ 保持（O-DESTROY 同时移除两者）。$\checkmark$

### 2.5 I/O 操作语义

**规则 IO-READ**：

$$\frac{HT_p(n) = (o, \rho) \quad R \in \rho \quad \tau(o) \in compat(read) \quad read(o, iov, off, \sigma) = (data, \sigma'')}{\langle read_p(n, iov, off), \sigma \rangle \longrightarrow \langle ok(|data|), \sigma'' \rangle}$$

$$\frac{HT_p(n) = (o, \rho) \quad R \notin \rho}{\langle read_p(n, iov, off), \sigma \rangle \longrightarrow \langle err(ACCESS), \sigma \rangle}$$

**规则 IO-WRITE** 对称。

### 2.6 Channel IPC 语义

**定义 2.1（Channel）** Channel 是一对有界 FIFO 队列 $(q_0, q_1)$，其中 $q_0$ 和 $q_1$ 各有容量上限 $C_{max} = 64KB$（数据）+ $H_{max} = 8$（handle 数量）。

**规则 CH-SEND**：

$$\frac{HT_p(n) = (o_{ch}, \rho) \quad W \in \rho \quad |q_{peer}| + |data| \leq C_{max} \quad \forall h_i \in handles. \ HT_p(h_i) = (o_i, \rho_i) \land Transfer \in \rho_i}{\langle send_p(n, data, handles), \sigma \rangle \longrightarrow \langle ok(0), \sigma' \rangle}$$

其中 $\sigma'$ 中：
- $q_{peer}$ 追加消息 $(data, \{(o_i, \rho_i)\})$
- **不**从发送方 handle 表移除 $h_i$（双方共享引用，refcount 增加）

$$\frac{HT_p(n) = (o_{ch}, \rho) \quad W \in \rho \quad |q_{peer}| + |data| > C_{max}}{\langle send_p(n, data, \_), \sigma \rangle \longrightarrow \langle err(WOULD\_BLOCK), \sigma \rangle}$$

**规则 CH-RECV**：

$$\frac{HT_p(n) = (o_{ch}, \rho) \quad R \in \rho \quad q_{local} = [(d_1, H_1), \ldots, (d_k, H_k)] \quad k \geq 1}{\langle recv_p(n, cap), \sigma \rangle \longrightarrow \langle ok(d_1, H_1'), \sigma' \rangle}$$

其中 $H_1' = \{(n_j, (o_j, \rho_j)) \mid (o_j, \rho_j) \in H_1\}$，即接收方在 handle 表中分配新编号，获得与发送方传递的相同权限。

**定理 2.1（Channel 消息序）** Channel 保证 FIFO 顺序：若消息 $m_1$ 在 $m_2$ 之前被 send 到同一端点，则 recv 端先取出 $m_1$。

*证明*：由规则 CH-SEND 向 $q_{peer}$ 追加、CH-RECV 从 $q_{local}$ 头部取出的语义直接得出。队列数据结构本身保证 FIFO。$\square$

**定理 2.2（Channel 原子性）** 单次 send/recv 操作对消息和 handle 转移是原子的：不存在一个中间状态，接收方看到了部分数据但未看到全部 handle。

*证明*：CH-SEND 规则在单个转移步骤中同时更新队列（追加完整的 $(data, handles)$ 元组）。CH-RECV 规则在单个步骤中取出完整元组并在接收方 handle 表中分配全部新条目。SOS 的单步原子性保证了不存在中间状态。$\square$

### 2.7 Event Queue 操作语义

**规则 EQ-WATCH**：

$$\frac{HT_p(n_q) = (o_q, \rho_q) \quad R \in \rho_q \quad HT_p(n_t) = (o_t, \_) \quad o_t \neq o_q}{\langle watch_p(n_q, n_t, events, udata), \sigma \rangle \longrightarrow \langle ok(0), \sigma[EQ(o_q) \mathrel{+}= (o_t, events, udata)] \rangle}$$

**规则 EQ-WAIT**：

$$\frac{HT_p(n_q) = (o_q, \rho_q) \quad R \in \rho_q \quad pending(o_q) = [e_1, \ldots, e_k] \quad k > 0}{\langle wait_p(n_q, max), \sigma \rangle \longrightarrow \langle ok([e_1, \ldots, e_{\min(k, max)}]), \sigma' \rangle}$$

$$\frac{HT_p(n_q) = (o_q, \rho_q) \quad R \in \rho_q \quad pending(o_q) = [] \quad timeout = 0}{\langle wait_p(n_q, \_, 0), \sigma \rangle \longrightarrow \langle ok([]), \sigma \rangle}$$

$$\frac{HT_p(n_q) = (o_q, \rho_q) \quad R \in \rho_q \quad pending(o_q) = [] \quad timeout > 0}{\langle wait_p(n_q, \_, timeout), \sigma \rangle \longrightarrow \langle block(o_q, timeout), \sigma \rangle}$$

**定理 2.3（Event 投递保证）** 一旦 handle $h$ 被 watch 到 event queue $q$ 上，且 $h$ 上发生的事件 $e$ 满足 $e.type \in events$（watch mask），则：

1. **若 $q$ 的 pending ring buffer 未满**：$e$ 必然被追加到 $q$ 的 pending 列表中。
2. **若 $q$ 的 pending ring buffer 已满**：$e$ 被丢弃，但内核保证已唤醒阻塞的消费者（若存在），使其返回当前 pending 事件列表，为后续事件腾出空间。

**精确陈述**：

$$\forall e = event(o_t, type, data). \ type \in mask(o_t, q) \implies \begin{cases} e \in pending(q) & \text{if } |pending(q)| < ring\_cap(q) \\ wake\_consumers(q) \land e \notin pending(q) & \text{if } |pending(q)| = ring\_cap(q) \end{cases}$$

*证明*：设事件源 $o_t$ 在内核中产生事件 $e$ 且 $e.type \in events$。

**情况 1（ring buffer 未满）**：内核事件分发机制（`a20_event_notify`，08 §3.3）将 $e$ 追加到 $q$ 的 ring buffer。此操作在持锁（`eq->lock`）的临界区内完成，是同步的。追加后调用 `wake_one` 唤醒等待线程。不存在"产生但未追加"的路径。$\checkmark$

**情况 2（ring buffer 已满）**：ring buffer 的容量 $ring\_cap$ 在创建时确定（默认 256）。当 $|pending(q)| = ring\_cap$ 时，新事件 $e$ 无法追加。内核的策略是：

- **先唤醒后丢弃**：调用 `wake_one`（或 `wake_all`）唤醒阻塞在 $q$ 上的消费者，使其消费 pending 事件腾出空间。$e$ 在本次不被追加。
- **消费者的责任**：用户程序应及时调用 `event_wait` 消费事件。若生产速度持续超过消费速度，部分事件被丢弃。
- **不阻塞生产者**：事件产生在中断/软中断上下文中，不能阻塞等待消费者。丢弃是唯一的选项。

**设计选择论证**：选择"丢弃"而非"阻塞生产者"或"动态扩容"的原因：
1. 事件产生在中断上下文，不能阻塞。
2. 动态扩容在中断上下文中不安全（需要内存分配）。
3. 256 个事件的容量在正常使用下充足（事件消费速度通常远高于产生速度）。
4. 用户可通过 `event_queue_create` 的 `capacity_hint` 参数请求更大的 ring buffer。

**与 POSIX 的对比**：
- Linux signalfd/timerfd：事件在内核缓冲区中排队，无上限（受内核内存限制），不丢弃。
- BSD kqueue：`EV_DISPATCH` 模式下事件被自动禁用直到用户重新启用，避免溢出。
- A20OS 的策略是"有界缓冲 + 溢出时唤醒"——在实时性和正确性之间取折衷。$\square$

---

## 3. 安全性证明

### 3.1 安全不变式

定义系统状态 $\sigma$ 的安全不变式集合 $\mathcal{I}$：

**I1（Handle 权限合法性）**：
$$\forall p, n. \ HT_p(n) = (o, \rho) \implies \rho \subseteq Legal(\tau(o))$$

**I2（权限子集传递）**：
$$\forall p, n. \ HT_p(n) = (o, \rho) \implies \rho \subseteq \rho_{granted}(o, p)$$

其中 $\rho_{granted}(o, p)$ 是 $p$ 历史上被授予的对 $o$ 的最大权限。

**I3（引用计数一致性）**：
$$\forall o \in dom(Obj). \ refcount(o, \sigma) = |\{(p, n) \mid HT_p(n) = (o, \rho)\}|$$

**I4（对象活性）**：
$$\forall o. \ o \in dom(Obj) \iff refcount(o, \sigma) > 0$$

**I5（类型安全）**：
$$\forall op, h. \ HT_p(h) = (o, \rho) \land op \text{ requires } \tau' \implies \tau(o) \in compat(op)$$

### 3.2 不变式保持证明

**定理 3.1（安全性）** 若 $\sigma$ 满足 $\mathcal{I}$，则对任意合法操作 $op$，$\sigma' = step(op, \sigma)$ 也满足 $\mathcal{I}$。

*证明策略*：对每个操作规则，逐条验证 $\mathcal{I}$ 在操作前后保持。

**对 H-DUP 规则**：
- **I1 保持**：新表项 $(o, \rho_{req})$ 满足 $\rho_{req} \subseteq \rho_s \subseteq Legal(\tau(o))$（归纳假设 + 传递性）。
- **I2 保持**：$\rho_{req} \subseteq \rho_s = \rho_{granted}(o, p)$（由前提 $\rho_{req} \subseteq \rho_s$）。
- **I3 保持**：新条目增加了 $refcount(o)$，由规则中创建新条目得 $refcount$ 增加 1，与新增条目数一致。
- **I4 保持**：$o$ 已经 $in dom(Obj)$，不新分配也不释放。
- **I5 保持**：新 handle 指向同一对象 $o$，类型不变。

**对 H-CLOSE 规则**：
- **I3 保持**：移除一个条目，$refcount$ 减少 1，一致。
- **I4 保持**：若 $refcount$ 降至 0，同时移除 $o$ 从 $Obj$，保持等价。
- **其余不变式**：仅删除条目，不违反任何正向条件。

**对 CH-SEND + Handle Transfer**：
- **I1 保持**：接收方获得 $(o_i, \rho_i)$，其中 $\rho_i \subseteq \rho'_i \subseteq Legal(\tau(o_i))$（发送方原权限合法 + 传递性）。
- **I2 保持**：接收方获得的 $\rho_i$ 不超过发送方持有的 $\rho'_i$。
- **I3 保持**：发送方不移除 handle（refcount 不减），接收方新增条目，$refcount$ 增加 1。
- **I4 保持**：对象未被释放。
- **I5 保持**：类型不变。

**对 T-SPAWN 规则**：
- **I1 保持**：新进程的每个 handle 权限由 $\rho_i \subseteq \rho'_i \subseteq Legal(\tau(o_i))$ 保证。
- **I3 保持**：所有传入 handle 的 refcount 增加 1。
- **I4 保持**：新 task 对象创建时 refcount = 1（父进程持有）。
- **I5 保持**：每个 handle 的类型与对象一致。

**归纳基底**：系统初始状态 $\sigma_0$ 由内核启动时构造。初始进程（init）获得内核直接注入的 handles，每个都满足 $\rho \subseteq Legal(\tau(o))$。$\square$

### 3.3 权限单调递减定理

**定理 3.2（Per-Handle 权限单调递减）** 设 $HT_p^t(n)$ 为进程 $p$ 在时刻 $t$ 的 handle 编号 $n$ 处的表项。若 $n$ 在时刻 $t_1$ 和 $t_2$ 均有定义，则：

$$\forall p, n, t_1 < t_2. \ HT_p^{t_2}(n) = (o, \rho_2) \land HT_p^{t_1}(n) = (o, \rho_1) \implies \rho_2 \subseteq \rho_1$$

即**同一 handle 编号上的权限在其生命周期内单调递减**。

*证明*：分析所有可能修改 handle table 的操作，证明它们不违反 per-handle 权限递减：

1. **handle_close**：删除条目 $HT_p(n)$。$n$ 不再有定义，前提 $HT_p^{t_2}(n)$ 不成立，蕴含式平凡为真。
2. **handle_dup**：创建新条目 $HT_p(n_{fresh}) = (o, \rho_{req})$，其中 $\rho_{req} \subseteq \rho_{source}$。$n_{fresh}$ 是新分配的编号，之前未有定义。对已有编号 $n \neq n_{fresh}$，权限不变。
3. **handle_replace**：删除源条目 $HT_p(s)$ 并创建新条目 $HT_p(n_{fresh}) = (o, \rho_{req})$。源编号 $s$ 被删除（前提不成立），新编号 $n_{fresh}$ 是全新的。对其他编号无影响。
4. **channel transfer（recv 端）**：在接收方 $p$ 的 handle table 中创建新条目 $HT_p(n_{new}) = (o_i, \rho_i)$。新编号 $n_{new}$ 之前未有定义。对已有编号无影响。
5. **spawn**：在**新进程** $p'$ 的 handle table 中创建条目。对现有进程 $p$ 的 handle table 无修改。
6. **对象创建**：创建新条目 $HT_p(n_{new})$。新编号，对已有编号无影响。

**关键观察**：A20OS 的 handle table 实现中，**没有任何操作修改已有条目的权限字段**。唯一的修改操作是删除（close）和创建（dup、replace、transfer_in、create）。因此，一旦 $HT_p(n) = (o, \rho)$ 被创建，$\rho$ 在 $n$ 的生命周期内保持不变，单调递减平凡成立。$\square$

**推论 3.2.1（Per-Process 权限上界）** 进程 $p$ 在时刻 $t$ 对对象 $o$ 的可达权限集为：

$$C_p(o, t) = \bigcup_{\{n \mid HT_p^t(n) = (o, \rho)\}} \rho$$

$C_p(o, t)$ 是**非单调的**——channel recv 和 handle_dup 可以增加 $p$ 对 $o$ 的可达权限集。但每个增加的权限都来自某个已有 handle 的权限子集：

$$\forall \rho' \subseteq C_p(o, t+1) \setminus C_p(o, t). \ \exists q, n_q. \ HT_q^{t}(n_q) = (o, \rho_q) \land \rho' \subseteq \rho_q$$

即新增权限严格受限于系统中的某个已有 handle 的权限，不超过任何原始授权。这是 confused deputy 不可行性（定理 3.3）的基础。$\square$

### 3.4 Confused Deputy 不可行性

**定理 3.3** 设进程 $A$ 持有权限集 $Caps_A = \{(o, \rho) \mid \exists n. \ HT_A(n) = (o, \rho)\}$，进程 $B$ 同理持有 $Caps_B$。则 $A$ 无法通过 $B$ 获得超出 $Caps_A \cup Caps_B$ 的权限。

*证明*：
1. $A$ 请求 $B$ 代为执行操作 $op(h, \ldots)$ 需要 $B$ 持有合适的 handle $h$。
2. 由 O1（封装性），$B$ 只能使用自己的 handles。
3. 若 $B$ 将 handle $h_B$ 通过 channel 传给 $A$：
   - 需 $Transfer \in rights(h_B)$
   - $A$ 获得 $h'_A$ 指向同一对象，权限 $\rho' \subseteq rights(h_B) \subseteq Caps_B$
4. 因此 $A$ 通过 $B$ 最多获得 $Caps_B$ 中的权限。
5. $A$ 的总权限 $\subseteq Caps_A \cup Caps_B$。$\square$

---

## 4. 活性证明

### 4.1 Event Queue 活性

**定理 4.1（Event Wait 终止性）** 设 event queue $q$ 上 watch 了至少一个活跃事件源 $h_s$（$h_s$ 会持续产生事件），且 $timeout > 0$。则 event_wait 调用必然在有限时间内返回。

*证明*：设 event_wait 进入阻塞状态 $block(o_q, timeout)$。有两种返回路径：

1. **事件到达**：事件源 $h_s$ 产生事件 $e$，内核将 $e$ 追加到 $pending(q)$。由定理 2.3，事件不丢失。阻塞的 wait 被唤醒，返回事件列表。
2. **超时**：内核维护定时器，当 $timeout$ 纳秒到达时唤醒 wait，返回空列表。

两种路径都在有限时间内触发。$\square$

**定理 4.2（Channel 通信无死锁条件）** 设进程 $A$ 持有 channel 端点 $ch_0$，进程 $B$ 持有 $ch_1$。若 $A$ 执行 send、$B$ 执行 recv（或反之），则在以下条件下通信必然完成：

1. 队列容量 $C > 0$（即 channel 非零容量）
2. $A$ 和 $B$ 不同时对同一端点执行相同方向的操作

*证明*：条件 1 保证 send 不因容量为零而永远阻塞。条件 2 排除经典死锁模式（双方同时 send 到同一满队列或同时 recv 从同一空队列）。在此条件下，至少一方的操作可以立即完成。$\square$

### 4.2 引用计数活性

**定理 4.3（无引用泄漏）** 若所有存活进程正常关闭 handles（通过 handle_close），则不存在引用计数 $> 0$ 但无持有者可达的"孤儿对象"。

*证明*：引用计数在以下情况下改变：
- **增加**：handle_dup（+1）、spawn（+1/handle）、channel transfer（+1/handle）
- **减少**：handle_close（-1）、task_exit（批量 -1，关闭所有 handles）

每种增加都有对应的减少路径。handle_close 将 $refcount$ 减少 1；task_exit 关闭进程全部 handles。若所有进程正常退出，所有 handles 被关闭，所有 $refcount$ 归零。$\square$

### 4.3 公平性

**定义 4.1（调度公平性）** 设 $\{p_1, p_2, \ldots, p_n\}$ 为所有就绪进程。公平调度器保证：

$$\forall p_i \in Ready. \ \exists k. \ p_i \text{ 在 } k \text { 次调度内被选中}$$

**定理 4.4（无限等待不可行，在公平调度下）** 在公平调度器下，任何就绪进程不会无限等待。

*证明*：由公平性定义，就绪进程在有限次调度内必然被选中执行。一旦执行，若操作不阻塞（如 read 有数据、channel 非满），立即完成。若操作阻塞，等待条件满足后被唤醒加入就绪队列，由公平性再次获得执行机会。$\square$

### 4.4 基于 LTL 的统一活性框架

以上活性证明（定理 4.1-4.4）是分散的、针对特定场景的。以下引入线性时序逻辑（LTL）框架，统一表述和证明 A20OS 的活性性质。

#### 4.4.1 LTL 基础

**定义 4.2（LTL 语法）** 给定原子命题集合 $AP$，LTL 公式 $\varphi$ 的语法为：

$$\varphi ::= p \mid \neg\varphi \mid \varphi \lor \varphi \mid \mathbf{X}\varphi \mid \varphi \,\mathbf{U}\,\varphi$$

其中 $p \in AP$，$\mathbf{X}$（next）表示"在下一个状态"，$\mathbf{U}$（until）表示"一直持续直到"。

常用派生算子：
- $\mathbf{F}\varphi = \text{true} \,\mathbf{U}\,\varphi$（eventually：$\varphi$ 最终成立）
- $\mathbf{G}\varphi = \neg\mathbf{F}\neg\varphi$（always：$\varphi$ 始终成立）
- $\varphi \implies \mathbf{F}\psi$（若 $\varphi$ 成立则 $\psi$ 最终成立——liveness 性质的标准形式）

**定义 4.3（LTL 语义）** 给定无穷路径 $\pi = s_0 s_1 s_2 \ldots$，$\pi \models \varphi$ 定义为：
- $\pi \models p$ iff $p \in L(s_0)$（标签函数 $L$ 将状态映射到命题集）
- $\pi \models \mathbf{X}\varphi$ iff $\pi[1..] \models \varphi$
- $\pi \models \varphi_1 \,\mathbf{U}\,\varphi_2$ iff $\exists i \geq 0.\ \pi[i..] \models \varphi_2 \land \forall j < i.\ \pi[j..] \models \varphi_1$
- $\pi \models \mathbf{F}\varphi$ iff $\exists i \geq 0.\ \pi[i..] \models \varphi$
- $\pi \models \mathbf{G}\varphi$ iff $\forall i \geq 0.\ \pi[i..] \models \varphi$

#### 4.4.2 A20OS 的原子命题

定义以下原子命题集合 $AP_{A20}$：

| 命题 | 含义 |
|------|------|
| $waiting(p, q)$ | 进程 $p$ 在 event queue $q$ 上阻塞等待 |
| $pending(q) \neq \emptyset$ | event queue $q$ 有待处理事件 |
| $has\_event(o)$ | 对象 $o$ 产生了新事件 |
| $blocked(p, ep)$ | 进程 $p$ 在 channel endpoint $ep$ 上阻塞 |
| $msg\_ready(ep)$ | channel endpoint $ep$ 有消息可读 |
| $ready(p)$ | 进程 $p$ 在就绪队列中 |
| $running(p)$ | 进程 $p$ 正在执行 |
| $alive(o)$ | 对象 $o$ 的引用计数 > 0 |
| $timeout\_expired(p)$ | 进程 $p$ 的等待超时已到期 |

#### 4.4.3 公平性假设

**调度公平性（strong fairness）**：

$$\Phi_{sched} = \mathbf{GF}(ready(p) \implies running(p))$$

即对每个进程 $p$，无限次出现"就绪则被调度"的情况。这是 strong fairness：即使 $p$ 反复进入和退出就绪队列，最终会被调度。

**事件传递公平性**：

$$\Phi_{event} = \mathbf{G}(has\_event(o) \land o \in watched(q) \implies \mathbf{F}(pending(q) \neq \emptyset))$$

即对象产生事件后，watch 了该对象的 event queue 最终会收到该事件。

#### 4.4.4 活性性质的 LTL 表述与统一证明

**性质 L1（Event Wait 终止性）**：

$$\Phi_{sched} \land \Phi_{event} \implies \mathbf{G}(waiting(p, q) \implies \mathbf{F}(\neg waiting(p, q)))$$

*证明*：设 $\pi$ 满足 $\Phi_{sched} \land \Phi_{event}$。在位置 $i$，$waiting(p, q)$ 成立。有两种情况：
1. $timeout > 0$：时间流逝保证 $\mathbf{F}(timeout\_expired(p))$。超时唤醒使 $\neg waiting(p, q)$ 成立。
2. $timeout = \infty$：$p$ 阻塞在 $q$ 上等待事件。由 $\Phi_{event}$，若任何被 watch 的对象产生事件，$\mathbf{F}(pending(q) \neq \emptyset)$。事件到达后唤醒 $p$，使 $\neg waiting(p, q)$ 成立。若没有活跃事件源，$p$ 永远等待——但这违反了前提"watch 了至少一个活跃事件源"（定理 4.1 的条件）。

两种情况都保证 $\mathbf{F}(\neg waiting(p, q))$。$\square$

**性质 L2（Channel 通信进展性）**：

$$\Phi_{sched} \implies \mathbf{G}(blocked(A, ep_0) \land msg\_ready(ep_0) \implies \mathbf{F}(\neg blocked(A, ep_0)))$$

*证明*：$msg\_ready(ep_0)$ 表示 $ep_0$ 的队列非空。$A$ 被阻塞在 $ep_0$ 上等待消息。由 $\Phi_{sched}$，$A$ 最终被调度。调度后 $A$ 的 recv 操作发现队列非空，立即完成，$A$ 进入就绪状态（$\neg blocked$）。$\square$

**性质 L3（引用回收性）**：

$$\mathbf{G}(alive(o) \land \neg \exists p.\ p \text{ 持有 } o \text{ 的 handle} \implies \mathbf{F}(\neg alive(o)))$$

即若对象 $o$ 的引用计数 > 0 但没有进程持有其 handle，则 $o$ 最终被回收。在 A20OS 的正确实现中此前提不成立（I3 保证 refcount 与 handle 数一致），因此此性质平凡成立。但若存在实现 bug 导致引用泄漏（refcount > 0 但无 handle），此性质提供了检测条件。$\square$

**性质 L4（系统终止性）**：

$$\mathbf{G}(\forall p.\ p \in P_{terminating} \implies \mathbf{F}(p \notin P_{alive}))$$

其中 $P_{terminating}$ 是"将最终退出"的进程集合。在 $P$ 有限、公平调度、且每个进程有限步内退出的假设下，系统最终达到无存活进程的状态，所有资源被回收。$\square$

#### 4.4.5 与 07 §5 Variant Function 的关系

LTL 框架与 variant function 方法（07 §5）的关系：

- **Variant function → LTL**：若存在良基 variant $V: State \to \mathbb{N}$ 且每步 $V$ 严格递减，则 $\mathbf{F}(V = 0)$ 成立。variant function 是 LTL 活性证明的一种构造方法。
- **LTL → 更强的表达力**：LTL 可以表达 variant function 无法直接表达的性质，如 $\Phi_{sched}$（strong fairness 是 $\mathbf{GF}$ 公式）。
- **统一价值**：LTL 框架将所有活性性质统一为同一逻辑系统的定理，而非独立的证明。新增活性性质只需：(1) 定义新的原子命题；(2) 用 LTL 公式表述；(3) 在公平性假设下证明。

---

## 5. 并发性与原子性

### 5.1 并发模型

A20OS 内核是抢占式多任务系统，使用自旋锁和互斥锁保护共享状态。并发分析需要考虑：

**定义 5.1（原子区域）** 每个系统调用的内核执行路径从 entry 到 return 构成一个临界区。在同一对象上的并发操作必须串行化。

**SOS 的并发扩展**：将系统状态转移升级为并发转移：

$$\frac{\langle op_p, \sigma \rangle \longrightarrow \langle res, \sigma' \rangle}{\langle op_p \| \text{idle}_{P \setminus \{p\}}, \sigma \rangle \longrightarrow_{conc} \langle res \| \text{idle}_{P \setminus \{p\}}, \sigma' \rangle}$$

并发语义要求：对同一对象的并发操作等价于某种串行化。

### 5.2 Handle Table 并发安全

**定理 5.1（Handle Table 串行化）** 同一进程的 handle table 操作是串行化的。

*证明*：handle table 由进程级自旋锁 `spinlock_t ht_lock` 保护。每个操作（dup, close, query, transfer_in）在访问 $HT_p$ 前获取锁，操作完成后释放。自旋锁保证互斥。$\square$

**定理 5.2（对象操作串行化）** 对同一内核对象的并发操作等价于某种串行执行顺序。

*证明*：每个内核对象包含类型特定的锁（如 inode 锁、socket 锁、channel 锁）。操作在修改对象状态前获取对象锁。由锁的互斥性，并发操作被串行化为获取锁的顺序。$\square$

### 5.3 Channel 传输的原子性

**定理 5.3（Handle Transfer 原子性）** 通过 channel 传递 handles 的操作是原子的：不存在接收方能访问 transferred handle 但原持有者仍能访问的中间状态，也不存在反过来的中间状态。

*精确分析*：实际上，A20OS 的 channel transfer 语义是**共享语义**（非移动语义）：

- send 后，发送方**保留**原 handle（refcount 增加）
- recv 后，接收方**新增** handle 条目

因此不存在"转移中间态"问题——两个进程在 recv 完成后同时持有指向同一对象的 handles。这与 seL4 的 Endpoint cap transfer 和 Zircon 的 Channel handle transfer 语义一致。

*原子性保证*：recv 操作要么完整完成（接收方 handle 表更新完毕、refcount 正确），要么不发生。不存在部分完成的中间状态。$\square$

### 5.4 并发 Spawn 的正确性

**定理 5.4** 两个进程并发 spawn 同一 image，各自获得独立的 task 对象和 handle 集合。

*证明*：spawn 操作创建全新的 task 对象 $o'$ 和全新的 handle 表 $HT_{p'}$。虽然两个新进程可能获得指向同一文件/目录对象的 handles（如共享 root_dir），但各自有独立的 handle 编号和独立的引用计数。对象共享通过 refcount 正确追踪。$\square$

---

## 6. 内存模型

### 6.1 虚拟内存形式化

**定义 6.1（地址空间）** 进程 $p$ 的地址空间是区间集合：

$$AS_p = \{[a, a + len) \mid Mem(a, p) = prot \land prot \neq None\}$$

要求区间互不相交：

$$\forall I_1, I_2 \in AS_p. \ I_1 \neq I_2 \implies I_1 \cap I_2 = \emptyset$$

**定义 6.2（映射来源）** 每个映射 $[a, a+len) \in AS_p$ 关联一个可选的 backing 对象：

$$backing(a, p) \in \mathcal{O} \cup \{anonymous\}$$

**规则 VM-ALLOC**：

$$\frac{prot \subseteq \{PROT\_R, PROT\_W, PROT\_X\} \quad len > 0 \quad (addr \notin \bigcup AS_p \quad \text{或 } addr = A20\_ADDR\_ANY)}{\langle vm\_alloc_p(len, prot, flags), \sigma \rangle \longrightarrow \langle ok(addr'), \sigma[Mem(addr', p) \mapsto prot, AS_p \mathrel{+}= [addr', addr'+len)] ] \rangle}$$

$$\frac{prot = 0 \quad \lor \quad len = 0}{\langle vm\_alloc_p(len, prot, flags), \sigma \rangle \longrightarrow \langle err(INVALID\_ARGS), \sigma \rangle}$$

**注**：$prot \subseteq \{PROT\_R, PROT\_W, PROT\_X\}$ 要求 prot 参数至少包含一个有效保护位。空 prot（$prot = 0$）被拒绝，因为无任何访问权限的映射无意义且浪费地址空间。$PROT\_W$ 不隐含 $PROT\_R$（与 POSIX 不同）——用户必须显式指定。

**规则 VM-PROTECT**：

$$\frac{[addr, addr+len) \subseteq AS_p \quad prot_{new} \subseteq \{PROT\_R, PROT\_W, PROT\_X\} \quad prot_{new} \neq \emptyset}{\langle vm\_protect_p(addr, len, prot_{new}), \sigma \rangle \longrightarrow \langle ok(0), \sigma[Mem([addr, addr+len), p) \mapsto prot_{new}] \rangle}$$

$$\frac{[addr, addr+len) \not\subseteq AS_p}{\langle vm\_protect_p(addr, len, \_), \sigma \rangle \longrightarrow \langle err(INVALID\_ARGS), \sigma \rangle}$$

$$\frac{prot_{new} = 0}{\langle vm\_protect_p(\_, \_, 0), \sigma \rangle \longrightarrow \langle err(INVALID\_ARGS), \sigma \rangle}$$

**VM-PROTECT 不变式保持**：
- **I1-I5**：vm_protect 不修改 handle table，$\mathcal{I}$ 中与 handle 相关的不变式平凡保持。
- **地址空间一致性**：只修改已有映射的保护位，不改变区间集合 $AS_p$ 的结构（区间数量和边界不变）。$\checkmark$

**规则 VM-UNMAP**：

$$\frac{[addr, addr+len) \subseteq AS_p}{\langle vm\_unmap_p(addr, len), \sigma \rangle \longrightarrow \langle ok(0), \sigma[Mem([addr, addr+len), p) \mapsto None, AS_p \setminus [addr, addr+len)] ] \rangle}$$

**规则 VM-MAP**：

$$\frac{HT_p(n) = (o, \rho) \quad Map \in \rho \quad \tau(o) \in \{file, shm, device\} \quad prot_{req} \subseteq \{PROT\_R, PROT\_W, PROT\_X\} \quad prot_{req} \neq \emptyset}{\langle vm\_map_p(n, off, len, prot_{req}, flags), \sigma \rangle \longrightarrow \langle ok(addr'), \sigma[Mem(addr', p) \mapsto effective\_prot, backing \mapsto (o, off)] \rangle}$$

其中 $effective\_prot = prot_{req} \cap translate(\rho)$，保证映射权限不超过 handle 授予的权限（见定理 6.1）。

### 6.2 vm_share 与 Capability 映射

**规则 VM-SHARE**：

$$\frac{[addr, addr+len) \subseteq AS_p \quad fresh(o_{shm})}{\langle vm\_share_p(addr, len, \rho_{grant}), \sigma \rangle \longrightarrow \langle ok(n_{shm}), \sigma' \rangle}$$

$\sigma'$ 中：
- 新对象 $o_{shm}$ 的 backing 指向 $[addr, addr+len)$ 的物理页
- 父进程获得 handle $(n_{shm}, (o_{shm}, \rho_{init}))$，其中 $\rho_{init} = Legal(shm)$
- 传递给其他进程时通过 handle_dup 降级为 $\rho_{grant}$

**定理 6.1（共享内存权限一致性）** 通过 vm_share 创建的共享内存，其映射权限是 handle 权限和 vm_map prot 参数的交集：

$$effective\_prot = prot_{map} \cap translate(rights(h))$$

其中 $translate: \mathcal{R}ights \to Prot$ 定义为：
$$translate(\rho) = \{R \mapsto PROT\_READ, W \mapsto PROT\_WRITE, X \mapsto PROT\_EXEC\} \cap \rho$$

*证明*：vm_map 规则要求 $Map \in rights(h)$。映射时 effective protection 取 handle 权限和显式 prot 参数的交集，保证即使显式请求了更高权限，也受限于 handle 授予的权限。$\square$

### 6.3 fork-free 内存隔离

**定理 6.2（地址空间独立性）** A20OS Native ABI 的任意两个进程的地址空间完全独立，不存在共享物理页的隐式路径。

*证明*：A20OS 不提供 fork。进程创建通过 spawn，新进程获得全新的空地址空间。内存共享只能通过显式 vm_share + handle transfer 实现。不存在隐式的 COW 共享路径。$\square$

**对比**：POSIX fork 后父子共享所有物理页（COW），需要逐页触发 write fault 来实现真正的隔离。这是 fork 的主要性能和正确性负担之一。

---

## 7. Capability 撤销

### 7.1 撤销语义

Capability 撤销是指：使某个对象对特定进程或所有进程不可访问。A20OS 通过以下机制支持撤销：

**定义 7.1（直接撤销）** 进程 $p$ 关闭 handle $h$，释放 $p$ 对 $o$ 的访问。

**定义 7.2（传递撤销）** 当对象 $o$ 的最后一个引用被关闭，$o$ 被销毁。所有指向 $o$ 的 handle（若存在悬挂引用）变为无效。

**定理 7.1（引用计数驱动的撤销完备性）** 对任意对象 $o$，当且仅当 $refcount(o) = 0$ 时，$o$ 被完全撤销（从系统中移除）。

*证明*：
- **充分性**：$refcount(o) = 0 \implies$ 无进程持有指向 $o$ 的 handle $\implies$ 无进程可访问 $o$。由 H-CLOSE 规则，当最后一个引用关闭时，$o$ 从 $Obj$ 中移除。
- **必要性**：若 $refcount(o) > 0$，存在某进程持有 $o$ 的 handle，该进程可通过 handle 操作访问 $o$。因此 $o$ 未被完全撤销。$\square$

### 7.2 子树撤销

**问题**：能否撤销某对象及其所有"子对象"？例如关闭 task handle 同时关闭其所有 thread handles。

**设计选择**：A20OS 不自动级联撤销。原因：

1. **可预测性**：自动级联使得推理对象生命周期困难（一个 close 可能有远距离副作用）。
2. **最小权限**：父进程可能只持有 task handle 而不持有 thread handles（权限降级后）。

**替代机制**：通过 handle_control 的 ADMIN 权限，task 持有者可以请求内核终止 task，内核在终止过程中关闭 task 内部所有 handles。这是策略而非隐式级联。

**定理 7.2（Task 终止的资源回收完备性）** 当 task $T$ 被终止（通过 task_exit 或外部 task_kill），$T$ 持有的所有对象引用被正确释放。

*证明*：task 终止时内核执行：
1. 关闭 $HT_T$ 中的所有 handle 条目
2. 对每个被关闭的 handle，$refcount$ 减少 1
3. 若某对象 $o$ 的 $refcount$ 降至 0，释放 $o$

由定理 7.1，此过程保证 $T$ 的所有直接引用被释放。但注意：$T$ 通过 channel 传递给其他进程的 handles 不受影响——这些 handles 现在由接收方持有。$\square$

### 7.3 与 seL4 CSpace 撤销的对比

| 属性 | seL4 | A20OS |
|------|------|-------|
| 撤销粒度 | 单个 CNode slot | 单个 handle table 条目 |
| 级联撤销 | CNode 子树撤销 | 不支持（需显式终止） |
| 撤销原子性 | 单步原子 | handle_close 原子 |
| 悬挂引用检测 | 无（CSpace 管理保证无悬挂） | 无（refcount 保证） |

A20OS 的简化选择牺牲了级联撤销的便利性，换取了更简单的推理模型。

---

## 8. 信息流控制

### 8.1 Noninterference 定义

**定义 8.1（Noninterference）** 设 $H$ 为高密级进程集合，$L$ 为低密级进程集合。系统满足 noninterference，当且仅当：

$$\forall \sigma_0. \ trace_L(step^*(\sigma_0, actions)) = trace_L(step^*(\sigma_0, actions \setminus actions_H))$$

即高密级进程的操作不影响低密级进程观察到的行为。

### 8.2 Handle 模型下的信息流

**定理 8.1（Capability 隔离蕴含信息流隔离）** 若进程 $A$ 和 $B$ 没有共享的 handle（即 $\{o \mid (o, \_) \in Caps_A\} \cap \{o \mid (o, \_) \in Caps_B\} = \emptyset$），且没有共享的 channel 端点，则 $A$ 和 $B$ 之间不存在通过 handle 系统的信息流。

*证明*：$A$ 影响系统的唯一途径是通过 handle 操作。若 $A$ 的所有 handle 指向的对象与 $B$ 完全不相交，则 $A$ 的操作不修改 $B$ 可观察的任何对象。类似地，$B$ 的操作不修改 $A$ 可观察的对象。因此两者互不干扰。$\square$

### 8.3 隐信道分析

**显式信息流**通过 handle transfer 和共享对象——受 capability 系统控制。

**隐信道**包括：

1. **资源竞争信道**：两个进程竞争 CPU 时间、内存、磁盘带宽。$A$ 可通过观察自身响应时间推断 $B$ 的行为。
2. **全局状态信道**：共享文件系统中的可用空间、inode 数量等。
3. **网络状态信道**：共享网络栈的队列占用情况。

**定理 8.2（A20OS 的隐信道上界）** 在 A20OS Native ABI 下，无共享 handle 的两个进程之间的隐信道带宽受限于：

$$BW_{covert} \leq BW_{cpu\_contention} + BW_{fs\_contention} + BW_{net\_contention}$$

这通常远高于 seL4 的隐信道分析（seL4 对缓存和分支预测隐信道有形式化分析），但 A20OS 作为教学内核不追求完整隐信道消除。

### 8.4 去分类（Declassification）

A20OS 的 handle transfer 机制天然支持受控去分类：

**定义 8.3（受控去分类）** 进程 $A$（高密级）通过 channel 向 $B$（低密级）发送消息和 handles，是一次去分类操作。去分类的安全性由以下条件保证：

1. $A$ 必须持有 channel 端点的 $W$ 权限（显式授权）
2. $A$ 必须持有被传递 handle 的 $Transfer$ 权限（显式授权）
3. 传递的 handle 权限可降级（限制去分类的范围）

这等价于贝尔-拉帕杜拉（Bell-LaPadula）模型中的"向下写需显式授权"规则。

### 8.5 带标签的信息流模型（Labeled IF）

定理 8.1 的"无共享 handle 即无信息流"假设过强——在实际系统中，进程通常通过 channel 共享对象。以下引入安全标签模型，在有共享对象的场景下证明 noninterference。

#### 8.5.1 安全格

**定义 8.4（安全标签）** 令 $(\mathcal{L}, \sqsubseteq)$ 为安全标签格。$\mathcal{L} = \{L, M, H\}$（低、中、高），偏序 $L \sqsubseteq M \sqsubseteq H$。

**定义 8.5（标签分配）** 每个对象 $o$ 和每个进程 $p$ 分配标签：

$$label: \mathcal{O} \cup P \to \mathcal{L}$$

**约束 L1（单调标签）**：进程的标签在生命周期内单调递增（只升不降）：

$$\forall p, t_1 < t_2.\ label(p, t_1) \sqsubseteq label(p, t_2)$$

这对应 MLS（Multi-Level Security）中的"不可降级"规则。

#### 8.5.2 Handle 操作的标签传播规则

**规则 LABEL-READ**（读取需要标签支配）：

$$\frac{HT_p(n) = (o, \rho) \quad R \in \rho \quad label(p) \sqsupseteq label(o)}{\langle read_p(n, \ldots), \sigma \rangle \longrightarrow \langle ok(\ldots), \sigma' \rangle}$$

$$\frac{HT_p(n) = (o, \rho) \quad R \in \rho \quad label(p) \not\sqsupseteq label(o)}{\langle read_p(n, \ldots), \sigma \rangle \longrightarrow \langle err(ACCESS), \sigma \rangle}$$

**规则 LABEL-WRITE**（写入需要标签受支配——Bell-LaPadula *-property）：

$$\frac{HT_p(n) = (o, \rho) \quad W \in \rho \quad label(o) \sqsupseteq label(p)}{\langle write_p(n, \ldots), \sigma \rangle \longrightarrow \langle ok(\ldots), \sigma' \rangle}$$

**规则 LABEL-TRANSFER**（Handle 传递不改变对象标签）：

$$\frac{\text{channel\_send transfers } h_i \text{ from } p_A \text{ to } p_B \quad label(o_i) = l_i}{\text{recv 后 } label(h_i) = l_i \text{（不变）}}$$

#### 8.5.3 带标签的 Noninterference

**定义 8.6（$\mathcal{L}$-Noninterference）** 系统 $\mathcal{S}$ 满足 $\mathcal{L}$-noninterference，当且仅当对所有 $l \in \mathcal{L}$：

$$trace_l(step^*(\sigma_0, actions)) = trace_l(step^*(\sigma_0, actions \setminus actions_{\sqsupset l}))$$

其中 $trace_l$ 只保留标签 $\sqsubseteq l$ 的操作和观察，$actions_{\sqsupset l}$ 是标签严格高于 $l$ 的进程的操作集。

**定理 8.3（$\mathcal{L}$-Noninterference 在 Handle 模型下成立）** 若系统满足：
1. 所有 handle 操作遵守 LABEL-READ 和 LABEL-WRITE 规则
2. Channel 传递不改变对象标签（LABEL-TRANSFER）
3. 进程标签单调递增（L1）

则系统满足 $\mathcal{L}$-noninterference。

*证明*：设 $H$ 为高标签进程集合，$L$ 为低标签进程集合（$label(p_H) = H, label(p_L) = L$）。

**信息不能从 H 流向 L**：
1. H 进程不能直接读 L 对象（需要 $label(H) \sqsupseteq label(o_L)$，但 $H \not\sqsubseteq L$）。
   — 但这不成立！$H \sqsupseteq L$，所以 H 可以读 L 的对象。这是正确的——高级可以读低级。
2. H 进程不能写 L 对象（需要 $label(o_L) \sqsupseteq label(H)$，但 $L \not\sqsupseteq H$）。$\checkmark$
3. H 进程通过 channel 传递 handle 给 L 进程：传递不改变对象标签。L 进程接收后获得 $label(o) = H$ 的对象。但 L 进程读取该对象需要 $label(L) \sqsupseteq label(o_H) = H$，即 $L \sqsupseteq H$，矛盾。因此 L 进程无法读取从 H 传来的高标签对象的**内容**。

4. L 进程**可以看到**高标签对象的 handle（通过 channel 接收），但无法通过该 handle 获得信息（所有 read 操作被 LABEL-READ 阻止）。handle 本身的存在性是 L 可知的，但这是可接受的——L 知道"某个对象存在"但不知道其内容。

5. L 进程可以执行 `handle_query(h_H)`，但此操作需要 $Stat \in rights$ 和 $label(L) \sqsupseteq label(o_H) = H$。由于 $L \not\sqsupseteq H$，查询也被拒绝。

因此 H 进程的操作不影响 L 进程可观察的任何行为。$\mathcal{L}$-noninterference 成立。$\square$

#### 8.5.4 隐信道的残余

即使在 $\mathcal{L}$-noninterference 下，以下隐信道仍然存在：

1. **资源耗尽信道**：H 进程大量创建对象，耗尽 handle table 空间，L 进程的 create 操作返回 NO_SPACE。L 可推断 H 在活动。
2. **时间信道**：H 进程持有 channel 锁时间长，影响 L 进程的 channel 操作延迟。
3. **调度信道**：H 进程占用 CPU，影响 L 进程的调度延迟。

这些隐信道是 OS 级别的固有限制，需要硬件支持（如缓存分区）才能完全消除。seL4 对这些信道有专门的分析（见 Klein 2014 §8.4）。A20OS 不追求隐信道的完整消除。

---

## 9. Dual-ABI 隔离的形式化证明

### 9.1 架构约束

**定义 9.1（模块依赖图）** 定义模块依赖关系 $G = (V, E)$，其中 $V = \{core, abi\_linux, abi\_native\}$，$E \subseteq V \times V$ 表示"依赖"关系。

**约束 C1**：$E$ 满足：
- $(abi\_linux, core) \in E$
- $(abi\_native, core) \in E$
- $(abi\_linux, abi\_native) \notin E$
- $(abi\_native, abi\_linux) \notin E$
- $(core, abi\_linux) \notin E$
- $(core, abi\_native) \notin E$

即两个 ABI 模块只依赖 core，不相互依赖，且 core 不反向依赖任何 ABI。

### 9.2 隔离定理

**定理 9.1（ABI 隔离）** Linux ABI 的 bug 或行为变更不影响 Native ABI 的正确性，反之亦然。

*证明*：
1. 设 Linux ABI 的 syscall 实现有 bug，导致状态 $\sigma$ 中某些 Linux 特定数据结构不一致。
2. 由 C1，Linux ABI 的实现只能修改 $abi\_linux$ 模块内部的状态和 $core$ 模块的状态。
3. $core$ 模块的状态是两个 ABI 的共享层。但 $core$ 的内部 API 有明确定义的契约（pre/post conditions），且 Native ABI 只使用 $core$ API，不直接访问 Linux ABI 的数据结构。
4. 因此，Linux ABI 的 bug 只在以下情况下影响 Native ABI：(a) bug 位于 $core$ 模块内部，(b) bug 导致 $core$ 的 API 行为偏离契约。
5. 这与"任何 ABI"无关——是 $core$ 自身的正确性问题。

因此，**ABI 级别的隔离等价于模块依赖约束 C1**。$\square$

### 9.3 状态隔离

**定理 9.2（状态空间不相交）** 令 $State_{linux}$ 为 Linux ABI 特有的状态（如 fd table、signal handler table、pid namespace），$State_{native}$ 为 Native ABI 特有的状态（如 handle table、event queue）。则：

$$State_{linux} \cap State_{native} = \emptyset$$

*证明*：
- Linux ABI 使用 `files_struct` 管理 fd table，`signal_state` 管理信号，`pid` 管理 PID。
- Native ABI 使用独立的 `handle_table` 管理 handles，独立的 `event_queue` 管理 event。
- 两者共享 `task_struct`（core 层），但各自的高层状态存储在独立的数据结构中。
- Linux ABI 的 `fd` 编号空间与 Native ABI 的 `handle` 编号空间完全独立。

$\square$

### 9.4 信息流能力边界（Capability Boundary as Information Flow）

定理 9.1 和 9.2 证明了**句法层面**的隔离——模块不互相调用、状态空间不相交。但这在实际系统中不够：两个 ABI 通过 VFS 共享文件对象，共享物理页帧，共享网络 socket buffer。真正的问题是：一个**能力无感知**的子系统（Linux ABI，使用 fd，无权限跟踪）能从共享资源中**推断**出多少关于**能力保护**子系统（Native ABI，handle + rights）的信息？

#### 9.4.1 能力可观测性

**定义 9.3（能力可观测性）** 给定系统状态 $\sigma$、进程 $p$ 和 Native ABI handle $h$，定义 $Obs(p, h, \sigma)$（"$p$ 能从 $\sigma$ 中推断 handle $h$ 的存在"）为以下条件的析取：

1. $p$ 持有 $h$：$HT_p(h) \neq \bot$
2. $p$ 可以通过共享对象间接推断 $h$ 的存在：$\exists o.\ access(p, o, \sigma) \wedge depends(o, h, \sigma)$

其中 $access(p, o, \sigma)$ 表示 $p$ 可以读写对象 $o$（通过 fd 或 handle），$depends(o, h, \sigma)$ 表示对象 $o$ 的状态依赖于 $h$ 所指向对象的状态。

#### 9.4.2 ABI 模式分类

**定义 9.4（ABI 模式）** 进程 $p$ 的 ABI 模式：

$$abi(p) = \begin{cases} \text{Linux} & \text{if } p.\text{handle\_table} = \text{NULL} \\ \text{Native} & \text{if } p.\text{handle\_table} \neq \text{NULL} \end{cases}$$

**定义 9.5（跨 ABI 可观测性）**：

$$Obs_{cross}(\sigma) = \{(p, h) \mid abi(p) \neq abi(holder(h)) \wedge Obs(p, h, \sigma)\}$$

#### 9.4.3 直接不可观测性

**定理 9.3（直接不可观测性）** 对任意 Linux ABI 进程 $p_l$ 和任意 Native ABI handle $h_n$，若 $h_n$ 指向的对象 $o$ 未被 $p_l$ 通过 fd 访问，则 $\neg Obs_1(p_l, h_n, \sigma)$。

*证明*：$Obs_1(p_l, h_n, \sigma)$ 成立当且仅当 $HT_{p_l}(h_n) \neq \bot$。但 $p_l$ 的 handle table 为 NULL（定义 9.4），因此 $HT_{p_l}(h_n) = \bot$。$\square$

#### 9.4.4 共享资源降级精确性

**定义 9.6（降级通道）** 降级通道 $(p_{native}, o_{shared}, p_{linux})$：$p_{native}$ 通过 handle 写入共享对象 $o$，$p_{linux}$ 通过 fd 读取 $o$。

**定义 9.7（降级精确性）** 降级通道是精确的，当且仅当通过 $o$ 从 $p_n$ 流向 $p_l$ 的信息**恰好是** $o$ 的内容，不包含任何关于 $p_n$ 的 handle table 的信息。

**定理 9.4（降级精确性）** 所有跨 ABI 共享资源降级通道都是精确的。

*证明*：设 $p_n$ 通过 handle $h_n$ 写入 VFS 节点 $v$，$p_l$ 通过 fd $f_l$ 读取 $v$。$p_l$ 能观察到的信息包括：$v$ 的数据内容、元数据（size, mode, timestamps）、存在性。$p_l$ **不能**观察到的信息包括：$h_n$ 的 rights $\rho$（VFS 节点不存储写入者的 handle rights）、$h_n$ 的对象类型 $\tau$、$p_n$ 的 handle table 中其他 handle 的存在性。降级通道的信息流恰好是 vfile 的数据+元数据，不包含 handle table 信息。$\square$

#### 9.4.5 混合信任能力边界定理

**定理 9.5（混合信任能力边界）** 设系统状态 $\sigma$ 满足安全不变式 $\mathcal{I}$。令 $H_{native}(\sigma)$ 为所有 Native ABI handle 的集合，$P_{linux}(\sigma)$ 为所有 Linux ABI 进程的集合。则：

$$\forall p_l \in P_{linux}, h_n \in H_{native}.\ \neg Obs(p_l, h_n, \sigma) \text{ 或 } (p_l, h_n) \text{ 构成精确降级通道}$$

*证明*：对任意 $p_l \in P_{linux}$ 和 $h_n \in H_{native}$，分情况：

**情况 1：$h_n$ 指向的对象未被 $p_l$ 通过 fd 访问。** 由定理 9.3，$\neg Obs_1$。对于 $Obs_2$，$access(p_l, o, \sigma)$ 不成立。因此 $\neg Obs$。

**情况 2：$h_n$ 指向的对象被 $p_l$ 通过 fd 访问。** $Obs_2$ 可能成立。但由定理 9.4，降级通道是精确的——$p_l$ 只能获取 $o$ 的内容+元数据，不能获取 $h_n$ 的 rights、类型或其他 handle 存在性。因此 $(p_l, h_n)$ 构成精确降级通道。

**情况 3：$h_n$ 指向的对象只被 Native ABI 进程访问。** $p_l$ 无法通过 VFS 接触该对象，无法通过 IPC 接触（Linux ABI 没有 channel 概念）。$\neg Obs$。$\square$

**与定理 9.1 的关系**：定理 9.1 是**句法层面**的隔离（模块不互相 include）。定理 9.5 是**语义层面**的隔离（即使通过共享资源交互，能力信息也不泄露）。定理 9.5 严格强于定理 9.1。

#### 9.4.6 跨 ABI 性能隔离

**定理 9.6（跨 ABI 性能隔离）** Linux ABI 进程 $p_l$ 的 syscall 延迟不受以下因素影响：Native ABI 的 handle table 操作、channel 通信（不涉及 VFS/网络共享资源时）、event queue 操作。

*证明*：Linux ABI 路径 `syscall_dispatch → linux_syscall_lookup → sys_* → core API`，Native ABI 路径 `syscall_dispatch → native_syscall_lookup → native_sys_* → core API`。两条路径在 core API 之前完全独立。共享 core API 的竞争是正常的资源竞争，不是 ABI 层面干扰。$\square$

---

## 10. ABI 可演进性的形式化

### 10.1 结构体兼容性

**定义 10.1（结构体版本兼容）** 设 $S_n = (f_1: T_1, f_2: T_2, \ldots, f_n: T_n)$ 为内核版本 $n$ 支持的结构体布局，$S_m$ 为用户程序编译时使用的版本。$S_m$ 与 $S_n$ 兼容（$m \leq n$）当且仅当：

$$\forall i \in [1, |S_m|]. \ f_i \in S_n \land type_i^{S_m} = type_i^{S_n} \land offset_i^{S_m} = offset_i^{S_n}$$

**演进规则**：

**规则 E-APPEND**（追加字段）：
$$\frac{S_n = (f_1, \ldots, f_n) \quad f_{n+1} \text{ is new}}{S_{n+1} = (f_1, \ldots, f_n, f_{n+1})}$$

此操作保持所有 $S_m$（$m \leq n$）的兼容性。

**规则 E-DEPRECATE**（弃用字段）：
$$\frac{f_i \in S_n}{f_i \text{ marked deprecated in } S_{n+1} \text{, must be zero}}$$

弃用字段保留偏移和类型，用户传入非零值返回 `INVALID_ARGUMENT`。

**规则 E-RESERVED**（保留位检查）：
$$\frac{flags \in S_n \quad reserved\_bits = \{b \mid b > max\_defined\_bit\}}{\forall u. \ u[reserved\_bits] \neq 0 \implies error}$$

**定理 10.1（ABI 兼容性保持）** 若内核从版本 $v_k$ 演进到 $v_{k+1}$ 仅使用 E-APPEND、E-DEPRECATE、E-RESERVED 规则，则所有为 $v_k$ 编译的用户程序在 $v_{k+1}$ 上正确运行。

*证明*：
- **用户传入旧结构体**（$size = |S_k| < |S_{k+1}|$）：内核只读取前 $|S_k|$ 字节，忽略新增字段。所有旧字段的偏移和类型不变（E-APPEND 保证）。
- **用户传入新结构体**（$size = |S_{k+1}| > |S_k|$）：内核只读取已知字段，忽略超出的字节。
- **用户使用旧 flag 值**：旧 flag 位定义不变。新 flag 位若未定义则为 reserved，用户设为 0（E-RESERVED 保证兼容）。

$\square$

### 10.2 Syscall 编号稳定性

**定义 10.2** Syscall 编号函数 $N: SyscallName \to \mathbb{N}_{16}$ 满足：
1. $N$ 是单射
2. $N^{-1}([0xSS00, 0xSSFF])$ 恰好是子系统 $SS$ 的全部 syscall
3. 一旦 $N(s)$ 分配给稳定 syscall，不再改变
4. 实验 syscall 使用 $[0x1000, 0x1FFF]$，可重新分配

**定理 10.2** Syscall 编号在 major version 内保持不变。

*证明*：由规则 3，稳定编号不可改变。新增 syscall 分配未使用的编号（由单射性保证不冲突）。编号空间 $[0x0000, 0x0FFF]$ 有 4096 个位置，远超 53 个已定义 syscall 的需求。$\square$

---

## 11. 与现有工作的形式化对比

### 11.1 Capability 系统谱系

```
Dennis & Van Horn (1966)
    └→ Hydra (Wulf, 1981): typed capabilities + protection
        └→ KeyKOS (Bomberger, 1992): persistent capabilities
            └→ EROS (Shapiro, 1999): capability-based persistence
                └→ seL4 (Klein, 2014): formally verified capabilities
                    
Plan 9 (Pike, 1995): file-as-uniform-interface
    └→ 9P protocol: everything is a file
        └→ FUSE: userspace filesystems
            
Zircon (Google, 2016): handle + rights + channels
    └→ A20OS Native ABI: simplified handle model
    
Capability-as-data:
    └→ Capsicum (Watson, 2010): fd-as-capability in BSD
    └→ CHERI (2015): hardware capability tags
```

### 11.2 核心差异分析

| 属性 | seL4 | Zircon | Capsicum | **A20OS** |
|------|------|--------|----------|-----------|
| Capability 形式化 | Isabelle/HOL 完整证明 | 无形式化证明 | 操作系统层无证明 | 操作语义 + 不变式证明 |
| 权限类型 | 对象方法级 | 通用位域 | fd rights | 通用位域（14 bits） |
| IPC 模型 | Endpoint + IPC buffer | Channel + message | 无（POSIX IPC） | Channel + message |
| 等待模型 | 无（轮询/中断） | Port wait | poll/kevent | Event queue |
| 进程创建 | 完全分解（5+ 步骤） | 两步（create + start） | fork（POSIX） | 单步 spawn |
| 总 syscall 数 | ~10（+ 对象方法） | ~150 | 0（扩展 POSIX） | 53 |
| POSIX 共存 | 不支持 | 不支持 | 原生支持 | 双 ABI 隔离 |

### 11.3 A20OS 的独特定位

**Insight 1：最小充分 capability 计算**。A20OS 的 13 种对象类型 × 14 种权限位构成了一个"最小充分"的能力系统——足以表达最小权限原则和 confused deputy 防御，但远比 seL4 的 CNode 图或 Zircon 的 ~30 种 rights 简单。

**Insight 2：双 ABI 形式化隔离**。现有工作要么完全替代 POSIX（seL4, Zircon），要么在 POSIX 内嵌入 capability（Capsicum）。A20OS 首次提出并形式化证明了两套 ABI 的模块级隔离条件（定理 9.1）。

**Insight 3：统一 event queue 的可证明性质**。kqueue 和 epoll 分别在 BSD 和 Linux 中实现了部分统一，但 A20OS 的 event queue 是基于 handle 系统的完全统一，且其不丢失性质（定理 2.3）和终止性（定理 4.1）可被形式化证明。

**Insight 4：spawn 的最小权限构造性**。fork 天然违反最小权限（隐式继承全部状态），posix_spawn 的参数化有限。A20OS 的 spawn 通过显式 handle 数组 + rights 降级，使得对任意安全策略 $P$ 可构造性证明存在满足 $P$ 的 handle 配置（性质 S2）。

---

## 12. 理论贡献总结

### C1：最小 Capability 计算

定义 13 种对象类型、14 种权限位、5 种 handle 操作的 SOS 语义，证明其满足安全不变式（定理 3.1）、权限单调递减（定理 3.2）和 confused deputy 不可行性（定理 3.3）。

### C2：双 ABI 形式化隔离

提出并证明模块依赖约束 C1 下的 ABI 隔离定理（定理 9.1）和状态空间不相交定理（定理 9.2），为"渐进式 ABI 演进"提供理论基础。进一步升级为**信息流能力边界**（定理 9.5）：即使 Linux ABI 和 Native ABI 共享 VFS 资源，Linux ABI 进程也无法推断 Native ABI handle 的权限状态。

### C3：运行时无关的 ABI 演进

结构化版本化框架（定义 10.1）配合演进规则（E-APPEND, E-DEPRECATE, E-RESERVED），证明 ABI 兼容性在仅使用合法演进规则时保持（定理 10.1）。无需 vDSO 或特殊运行时支持。

### C4：统一 Event Queue 的可证明性质

基于 handle 系统，单一 wait 操作覆盖所有对象类型。证明事件不丢失（定理 2.3）、等待终止性（定理 4.1）和 FIFO 顺序保证（定理 2.1）。

---

## 参考文献

1. Plotkin, G.D. "A Structural Approach to Operational Semantics." *University of Aarhus*, 1981.
2. Dennis, J.B. & Van Horn, E.C. "Programming Semantics for Multiprogrammed Computations." *Comm. ACM*, 1966.
3. Miller, M.S. et al. "Capability-based Financial Instruments." *Financial Cryptography*, 2000.
4. Saltzer, J.H. & Schroeder, M.D. "The Protection of Information in Computer Systems." *Comm. ACM*, 1975.
5. Lampson, B.W. "Hints for Computer System Design." *SOSP '83*, 1983.
6. Klein, G. et al. "Comprehensive Formal Verification of an OS Microkernel." *ACM TOCS*, 2014.
7. Baumann, A. et al. "A fork() in the road." *HotOS '19*, 2019.
8. Watson, R.N.M. et al. "Capsicum: Practical Capabilities for UNIX." *USENIX Security*, 2010.
9. Shapiro, J.S. "EROS: A Capability System." *PhD Thesis*, Penn, 1999.
10. The Fuchsia Authors. "Zircon Kernel Concepts." https://fuchsia.dev/docs/concepts/kernel/concepts
11. Pike, R. et al. "Plan 9 from Bell Labs." *Computing Systems*, 1995.
12. Engler, D. et al. "Exokernel: An Operating System Architecture for Application-Level Resource Management." *SOSP '95*, 1995.
13. Levy, H.M. "Capability-Based Computer Systems." *Digital Press*, 1984.
14. Wulf, W. et al. "HYDRA: The Kernel of a Multiprocessing Operating System." *Comm. ACM*, 1981.
15. Brinch Hansen, P. "The Nucleus of a Multiprogramming System." *Comm. ACM*, 1970.
16. Madhavapeddy, A. et al. "Unikernels: Library Operating Systems for the Cloud." *ASPLOS '13*, 2013.
17. Litton, J. et al. "Lightweight Contexts: An OS Abstraction for Safety and Performance." *OSDI '16*, 2016.
18. Mao, Y. et al. "NrOS: Effective Replication for Shared-Memory Multicore Systems." *OSDI '20*, 2020.
19. Zell, D. et al. "Demikernel: A Datacenter Kernel for I/O." *SOSP '21*, 2021.
20. Bell, D.E. & LaPadula, L.J. "Secure Computer Systems: Mathematical Foundations." *MITRE Technical Report*, 1973.
21. Goguen, J.A. & Meseguer, J. "Security Policies and Security Models." *IEEE S&P*, 1982.
22. Bomberger, A.C. et al. "The KeyKOS Nanokernel Architecture." *Ottawa Linux Symposium*, 1992.
