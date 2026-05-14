# A20OS Native ABI：安全模型设计

> 本文档定义 Native ABI 的 capability 安全模型，包括 14 个权限位的代数结构、handle transfer 语义、安全标签格和权限降级不变式。

---

## 1. Rights 定义

### 1.1 权限位布局

```c
// ===== 基础权限（适用于大部分对象） =====
#define A20_RIGHT_READ       (1ull << 0)   /* 读取数据 */
#define A20_RIGHT_WRITE      (1ull << 1)   /* 写入数据 */
#define A20_RIGHT_EXEC       (1ull << 2)   /* 执行（内存映射） */
#define A20_RIGHT_STAT       (1ull << 3)   /* 查询属性 */
#define A20_RIGHT_SEEK       (1ull << 4)   /* 修改位置 */
#define A20_RIGHT_DUP        (1ull << 5)   /* 复制 handle */
#define A20_RIGHT_TRANSFER   (1ull << 6)   /* 通过 IPC 传递 */
#define A20_RIGHT_MAP        (1ull << 7)   /* 映射到地址空间 */

// ===== 等待与控制权限 =====
#define A20_RIGHT_WAIT       (1ull << 8)   /* 等待对象事件 */
#define A20_RIGHT_CONNECT    (1ull << 9)   /* 发起连接（socket） */
#define A20_RIGHT_ACCEPT     (1ull << 10)  /* 接受连接（socket） */
#define A20_RIGHT_CONTROL    (1ull << 11)  /* 对象特定控制操作 */
#define A20_RIGHT_ADMIN      (1ull << 12)  /* 管理操作 */
#define A20_RIGHT_SIGNAL     (1ull << 13)  /* 发送信号（task/event） */
```

共 14 个权限位，构成权限域：

$$\mathcal{R}ights = 2^{\{R, W, X, Stat, Seek, Dup, Transfer, Map, Wait, Connect, Accept, Control, Admin, Signal\}}$$

### 1.2 权限分类

| 类别 | 权限 | 说明 |
|------|------|------|
| 数据操作 | R, W, X | 读写执行，最基本的对象操作 |
| 元数据 | Stat, Seek | 查询属性和修改位置 |
| Handle 操作 | Dup, Transfer | 复制和传递 handle |
| 内存 | Map | 映射到地址空间 |
| 等待 | Wait | 等待对象事件 |
| 网络 | Connect, Accept | socket 连接操作 |
| 控制 | Control, Admin | 对象控制和管理 |
| 信号 | Signal | 发送信号给 task |

---

## 2. Rights 代数

### 2.1 格结构

权限集合 $(\mathcal{R}ights, \subseteq)$ 构成偏序集。对任意两个权限集合 $\rho_1, \rho_2$：

- **交集** $\rho_1 \cap \rho_2$：两者都允许的操作
- **并集** $\rho_1 \cup \rho_2$：至少一方允许的操作

$(\mathcal{R}ights, \subseteq, \cap, \cup)$ 构成**有限格**（finite lattice）：
- 顶元素 $\top = \{R, W, X, Stat, Seek, Dup, Transfer, Map, Wait, Connect, Accept, Control, Admin, Signal\}$
- 底元素 $\bot = \emptyset$

### 2.2 降级单调性

**定理（权限单调递减）**：对于任何操作序列 $\sigma_0 \to \sigma_1 \to \cdots \to \sigma_n$，任意 handle 的权限满足：

$$\forall i.\ \rho_i(handle) \subseteq \rho_{i-1}(handle)$$

证明要点：
- `handle_dup`：$\rho_{new} \subseteq \rho_{old}$（子集要求）
- `handle_transfer`：$\rho_{recv} = \rho_{send} \cap \rho_{transfer} \subseteq \rho_{send}$
- `handle_replace`：$\rho_{new} \subseteq \rho_{old}$
- 不存在任何操作使权限增加。

### 2.3 Confused Deputy 不可行性

**定理**：在 A20OS 的 handle 系统中，confused deputy 攻击不可行。

证明：假设进程 A 试图通过进程 B 访问 A 不应有权限的资源 R。B 持有 R 的 handle，权限为 $\rho_B$。A 请求 B 操作 R。B 执行操作时，使用的是 B 自己的 handle 权限 $\rho_B$。A 无法：
1. 增加 B handle 的权限（降级单调性）
2. 伪造指向 R 的 handle（handle 是进程本地编号，无法跨进程伪造）
3. 通过 transfer 获得 R 的 handle（$\rho_{recv} \subseteq \rho_B$，但 A 从未持有 R 的 handle）

---

## 3. 类型-权限兼容矩阵

每种对象类型只有部分权限有意义。对不兼容的权限位，内核在 `handle_dup` 时自动忽略。

### 3.1 合法权限集

| 类型 | 合法权限 | 说明 |
|------|---------|------|
| task | Wait, Signal, Stat, Dup, Transfer, Control, Admin | 进程操作 |
| thread | Stat, Dup, Transfer, Control | 线程操作 |
| file | R, W, Stat, Seek, Dup, Transfer, Map, Control | 文件 I/O |
| dir | R, Stat, Dup, Transfer, Control | 目录操作 |
| socket | R, W, Stat, Dup, Transfer, Connect, Accept, Control | 网络操作 |
| pipe | R, W, Stat, Dup, Transfer | 管道 I/O |
| channel | R, W, Dup, Transfer | 消息传递 |
| eventq | R, Dup, Transfer, Control | 事件队列 |
| timer | Stat, Dup, Transfer, Control | 定时器 |
| shm | R, W, Map, Stat, Dup, Transfer, Control | 共享内存 |
| device | R, W, Map, Stat, Seek, Dup, Transfer, Control | 设备操作 |
| ns | Stat, Dup, Transfer, Control, Admin | 命名空间 |
| debug | Stat, Dup, Transfer, Control, Admin | 调试 |

### 3.2 操作-权限映射

| 操作 | 所需权限 | 适用类型 |
|------|---------|---------|
| handle_read | R | file, dir, pipe, socket, shm |
| handle_write | W | file, pipe, socket, shm |
| handle_stat | Stat | all |
| handle_control | Control | all |
| handle_dup | Dup | all |
| channel_send | W | channel |
| channel_recv | R | channel |
| event_wait | R | eventq |
| task_wait | Wait | task |
| vm_map | Map | file, shm, device |
| net_connect | Connect | socket |
| net_accept | Accept | socket |
| task_kill | Signal | task |
| ns_apply | Admin | ns, debug |

### 3.3 完备性论证

每种权限控制至少一个操作，且每个操作至少需要一个权限。14 种权限和 13 种对象类型构成**最小充分集**：

- **最小性**：移除任何一种权限会使某种合法操作无法独立授权
- **充分性**：14 种权限足以表达 POSIX 子集的全部资源操作需求

---

## 4. Handle Transfer 安全语义

### 4.1 Transfer 规则

Handle 通过 channel 传递时，接收方获得的权限：

$$\rho_{recv} = \rho_{send} \cap \rho_{transfer}$$

| 场景 | $\rho_{send}$ | $\rho_{transfer}$ | $\rho_{recv}$ |
|------|-------------|-----------------|-------------|
| 完整传递 | {R, W} | {R, W} | {R, W} |
| 降级传递 | {R, W} | {R} | {R} |
| 过度请求 | {R} | {R, W} | {R}（交集） |
| 无效请求 | {R} | {W} | $\emptyset$（返回错误） |

### 4.2 共享语义 vs 移动语义

A20OS 选择**共享语义**：

- `channel_send(handle)` 后，发送方仍持有原 handle
- 对象的引用计数增加（refcount_inc）
- 接收方获得独立的 handle，指向同一对象

为什么不选移动语义（send 后原 handle 失效）？

1. **组合性**：共享语义允许一个 handle 同时传递给多个接收方（一对多分发）
2. **错误恢复**：如果 transfer 失败（接收方 HT 满），发送方不需要重新获取 handle
3. **审计性**：发送方在 send 后仍可查询 handle 状态

### 4.3 Spawn 中的 Handle 传递

`task_spawn` 的 `handles` 数组允许显式传递资源给新进程：

```c
typedef struct a20_spawn_handle {
    a20_handle_t handle;
    a20_rights_t rights;         /* 传递给新进程的权限 */
    uint32_t target_slot;
    uint32_t flags;
} a20_spawn_handle_t;
```

新进程获得的权限 $\rho_{child} = rights$，要求 $rights \subseteq \rho_{parent}(handle)$。

这是实现 sandbox 的核心机制：父进程只传递子进程需要的最小资源集合。

---

## 5. 安全标签格（信息流控制）

### 5.1 标签定义

A20OS 定义三值安全格：

$$\mathcal{L} = \{L, M, H\}, \quad L \sqsubseteq M \sqsubseteq H$$

- $L$ (Low)：公开数据
- $M$ (Medium)：内部数据
- $H$ (High)：机密数据

### 5.2 标签传播规则

每个对象携带安全标签 $\ell \in \mathcal{L}$。Handle 操作遵循 Bell-LaPadula 规则：

**简单安全性（No Read Up）**：
$$read(p, o) \implies \ell(p) \geq \ell(o)$$

**星属性（No Write Down）**：
$$write(p, o) \implies \ell(p) \leq \ell(o)$$

**Transfer 规则**：
$$transfer(p_1 \to p_2, o) \implies \ell(p_1) \leq \ell(p_2) \text{ 或 } \ell(o) = L$$

### 5.3 $\mathcal{L}$-Noninterference

**定理**：如果系统初始状态满足标签约束，且所有操作遵循传播规则，则高标签进程的行为不影响低标签进程的观察。

形式化：对任意两个初始状态 $\sigma_0, \sigma_0'$，如果它们在低标签部分相同（$L$-equivalent），则经过任意操作序列后仍 $L$-equivalent。

$$\sigma_0 \stackrel{L}{=} \sigma_0' \implies \forall \pi.\ exec(\sigma_0, \pi) \stackrel{L}{=} exec(\sigma_0', \pi)$$

---

## 6. 时态权限模型（Temporal Permission Model）

### 6.1 动机

传统能力系统一旦授予权限，权限就永久有效直到显式撤销。但在以下场景中，这种"永不过期"的能力是不够的：

- **供应链安全**：第三方库只需在请求处理期间的网络访问权限
- **最小权限委托**：子任务完成后应自动失去所有权限
- **沙箱逃逸防护**：即使能力被意外泄露，过期后自动失效
- **资源预算**：限制某个组件的总 I/O 操作次数

现有内核能力系统（Zircon、seL4）都没有内核级的时间受限委托。OAuth/X.509 有过期机制但那是用户态构造。

### 6.2 有效权限定义

Handle 的有效权限 $\rho_{eff}$ 是声明权限 $\rho$ 的时态投影：

$$\rho_{eff}(h, t) = \begin{cases} \rho(h) & \text{if } expiry(h) = 0 \text{ or } t < expiry(h) \text{, and } remaining(h) \neq 0 \\ \emptyset & \text{otherwise} \end{cases}$$

**解释**：

| 条件 | $\rho_{eff}$ | 含义 |
|------|-------------|------|
| `expiry_tick == 0` 且 `remaining_ops == 0` | $\rho$ | 无时态约束，传统语义 |
| `expiry_tick > 0` 且 `t < expiry_tick` | $\rho$ | 未到过期时间，权限正常 |
| `expiry_tick > 0` 且 `t ≥ expiry_tick` | $\emptyset$ | 时间过期，权限清零 |
| `remaining_ops > 0`（OP_COUNT 模式） | $\rho$（每次操作后递减） | 操作次数未耗尽 |
| `remaining_ops == 0`（OP_COUNT 模式） | $\emptyset$ | 操作次数耗尽 |

**注意**：`remaining_ops == 0` 且 `OP_COUNT` **未设置**时表示"无限次"（不是"已耗尽"）。只有 `OP_COUNT` flag 被设置后，`remaining_ops == 0` 才表示"已耗尽"。

### 6.3 时态单调性

**定理（时态权限单调递减）** 对任意 handle $h$，其有效权限随时间单调不增：

$$\forall t_1, t_2.\ t_2 > t_1 \implies \rho_{eff}(h, t_2) \subseteq \rho_{eff}(h, t_1)$$

证明要点：
1. **时间衰减**：当 $t$ 跨过 $expiry(h)$ 时，$\rho_{eff}$ 从 $\rho$ 变为 $\emptyset$。$\emptyset \subseteq \rho$。
2. **操作计数衰减**：每次操作后 $remaining\_ops$ 递减。当从 1 变为 0 时，$\rho_{eff}$ 从 $\rho$ 变为 $\emptyset$。
3. **显式降级**（handle_dup、handle_replace）：$\rho_{new} \subseteq \rho_{old}$。
4. **没有任何操作能增加** $\rho_{eff}$：expiry 不能延长，remaining_ops 只减不增。

此定理**严格包含**原定理 2.2（权限单调递减）：原定理是 $t_2 = t_1$ 时的特殊情况（只考虑显式操作，不考虑时间衰减）。

### 6.4 时态不可刷新性

**定理（不可刷新）** 持有 handle $h$（$\rho_{eff}(h, t) \neq \emptyset$）的进程 $p$ 无法创建一个有效权限严格包含 $\rho_{eff}(h, t)$ 或过期时间晚于 $h$ 的新 handle $h'$。

证明要点：`handle_dup` 的时态约束要求 `expiry' ≤ expiry(h)` 和 `ops' ≤ remaining_ops(h)`。因此 $\rho_{eff}(h', t') \subseteq \rho_{eff}(h, t')$。

### 6.5 过期行为

Handle 过期后有两种行为：

| 模式 | `temporal_flags` | 过期后行为 | 适用场景 |
|------|------------------|-----------|---------|
| 惰性化 | 未设 `AUTO_CLOSE` | $\rho_{eff} = \emptyset$，entry 仍占用 slot 和 refcount | 需要显式控制资源回收 |
| 自动关闭 | `AUTO_CLOSE` | 内核 sweeper 自动执行 `handle_close` | 自动资源回收 |

惰性化的好处是过期 handle 仍可通过 `handle_query` 查询（STAT 权限不依赖 $\rho_{eff}$），便于调试和审计。

### 6.6 与其他系统的对比

| 特性 | A20OS | Zircon | seL4 | CHERI | OAuth 2.0 |
|------|-------|--------|------|-------|-----------|
| 时间过期 | ✅ 内核级 | ❌ | ❌ | ❌ | ✅ 用户态 |
| 操作次数限制 | ✅ 内核级 | ❌ | ❌ | ❌ | ❌ |
| 自动回收 | ✅ 可选 | ❌ | ❌ | ❌ | ✅ refresh token |
| 不可刷新性保证 | ✅ 定理证明 | N/A | N/A | N/A | ❌（有 refresh） |
| 形式化时态性质 | ✅ SOS 证明 | ❌ | ❌ | ❌ | ❌ |

---

## 7. 命名空间隔离

### 7.1 Namespace 类型

| 类型 | 说明 | 隔离内容 |
|------|------|---------|
| filesystem | 文件系统命名空间 | 路径解析根 |
| network | 网络命名空间 | socket 地址空间 |
| pid | PID 命名空间 | task 编号空间 |
| device | 设备命名空间 | 设备可见性 |

### 7.2 Namespace 操作

```c
/* 创建命名空间 */
int64_t ns_create(uint32_t ns_type, a20_flags_t flags, a20_handle_t *out);

/* 应用命名空间到目标 task */
int64_t ns_apply(a20_handle_t ns, a20_handle_t target);
```

`ns_apply` 需要 target handle 的 `ADMIN` 权限。Namespace 是 handle，可以传递和降级。

### 7.3 Spawn 中的 Namespace

`task_spawn` 的 flags 可以指定新进程使用的 namespace。如果不指定，继承父进程的 namespace。

---

## 8. 安全审计与调试

### 8.1 调试接口

```c
int64_t debug_attach(a20_handle_t task);    /* 需要 ADMIN 权限 */
int64_t debug_read_regs(a20_handle_t thread, a20_regs_t *out);
int64_t debug_write_regs(a20_handle_t thread, const a20_regs_t *in);
int64_t debug_map_memory(a20_handle_t task, a20_handle_t *out);
```

调试能力通过 handle rights 控制。只有持有 task 的 `ADMIN` 权限的进程才能附加调试器。

### 8.2 Handle 查询审计

`handle_query` 允许进程查询自己持有的 handle 的类型和权限，但不能查询其他进程的 handle。这是最小权限审计：每个进程只能看到自己的权限。

---

## 9. 与其他系统的对比

| 特性 | A20OS | Zircon | seL4 | Capsicum |
|------|-------|--------|------|----------|
| 权限模型 | 14 位 rights | ~30 种 rights | CNode cap types | fd capabilities |
| 对象类型 | 13 | ~25 | ~15 | N/A (fd-based) |
| 权限降级 | dup 子集 + transfer 交集 | zx_handle_duplicate | CNode mint | cap_enter |
| 形式化证明 | SOS 操作语义 | 无 | Isabelle/HOL | 无 |
| Handle 传递 | channel 共享语义 | channel 移动语义 | IPC endpoint | SCM_RIGHTS |
| 信息流控制 | $\mathcal{L}$-noninterference | 无 | 信息流策略 | 无 |
| 双 ABI 隔离 | 信息流能力边界定理 | 无 | 无 | 在 POSIX 内 |
| 时态能力 | 内核级（expiry + ops） | 无 | 无 | 无 |
| 时态形式化 | SOS 时态证明 | 无 | 无 | 无 |
