# 预算能力（Budgeted Capabilities）——C1 机制贡献

> **本文是研究主轴的第一根支柱（机制）**：把"授权"从二元权限推广为**多维预算**。核心洞察：**预算可部署，权限不可部署**——二元权限要求代码重写（rewrite mandate），而量化、可耗竭的预算可以在部署时注入。这与 C2（能力信封 05，系统贡献）一起，把能力系统从"重写命令"变成"部署选项"。
>
> **范围说明**：本文的"时间/次数"维度即旧版时态能力；本文将其推广为**类型 × 权限 × 时间 × 次数 × 传播**的同构预算格。定理沿用旧编号体系（3.1-3.3、7.1）。所有证明为纸笔论证，机器检验计划见 08。

---

## 1. 从二元权限到多维预算

### 1.1 传统 capability 的两个死结

**死结一（重写命令）**：capability 是二元权限（能/不能），获得它要求代码显式请求、显式传递、显式管理 → 系统必须重写。这是 seL4（全重写）、Zircon（生态从零）从未被采纳的根源。

**死结二（授予即永久）**：权限一旦授予永久有效，直到显式撤销。供应链组件共享宿主进程全部权限，且**没有时间/数量边界**（Log4Shell 2021、SolarWinds 2020、XZ 2024 的共同模式）。

### 1.2 关键观察：安全界早已普遍接受"临时凭证"

OAuth token、JWT、X.509 证书都有有效期——**凭证是量化的、会过期的**。但它们是**用户态协议**：内核不参与执行，进程被攻破后可以自己延长（自签 token），也没有与能力模型结合。

### 1.3 我们的推广：authority 是多维预算

> **权限是"能否"，预算是"多少、多久、多少次、能碰到哪类"**。把授权建模为多维预算向量，能力纪律就可以被**注入**（部署时），而不是**写入**（构建时）。

$$\rho_{eff}(h,t) = \bigwedge_{\dim \in \{type, rights, time, ops, propagation\}} \rho_{dim}(h,t)$$

任意一维耗尽 → $\rho_{eff}(h,t) = \emptyset$。

| 维度 | 预算内容 | 对应机制 |
|------|---------|---------|
| **类型** | 可接触的资源类 | typed channel / 信封 allowed_types |
| **权限** | rights 子集上限 | handle_dup 降权 |
| **时间** | `expiry_tick` | 时态能力 |
| **次数** | `remaining_ops` | 操作预算 |
| **传播** | 可向外委托的类型/预算 | 委托链耗散 + 类型化通道 |

---

## 2. 设计

### 2.1 Handle 条目扩展（预算字段）

在 handle 表项（对象、类型、rights）上叠加：

```c
typedef struct a20_handle_entry {
    void             *object;
    a20_object_type_t type;
    a20_rights_t      rights;
    uint64_t          expiry_tick;     // 时间预算；0 = 无限制
    uint32_t          remaining_ops;   // 次数预算；OP_COUNT 未置位则忽略
    uint32_t          temporal_flags;  // EXPIRY_ABSOLUTE / OP_COUNT / AUTO_CLOSE
    uint64_t          data_budget;     // 可选：累计数据量预算（预留）
} a20_handle_entry_t;
```

空间代价：+16 bytes（基本维度）/ +24（含数据预算）。

### 2.2 有效权限（预算合取）

$$\rho_{eff}(h, t) = \begin{cases} \rho(h) & \text{if } (\neg EXP \lor t < expiry) \land (\neg OP \lor remaining > 0) \land \tau(h) \in \text{allowed}(h) \\ \emptyset & \text{otherwise} \end{cases}$$

**O(1) 判定**：各维都是固定字段的位运算/比较，不查历史。这是"预算可支撑 syscall 频率"的系统学关键。

### 2.3 用户态入口

- `handle_control(SET_TEMPORAL/GET_TEMPORAL/SET_LABEL)`：**仅可增强约束**（添加 flag、提前 expiry、减少 ops、提升 label）——不可刷新由接口保证。
- 复制/传递路径（dup/replace/spawn/transfer/vm_share）**自动继承**预算并耗散。
- deadline-driven sweeper 周期扫描，`AUTO_CLOSE` 过期即回收。

### 2.4 委托链耗散（O(1) 策略执行）

预算在传播中只减不增：`expiry' ≤ expiry`、`ops' ≤ remaining`、`allowed_types' ⊆ allowed_types`。在**源头**设置一次 `expiry = T`，委托链任意长度的终端过期时刻都不晚于 $T$。

> **O(1) 安全策略执行**：策略在源头设置一次，之后所有判定 O(1)，无需逐级策略检查。与 05 的信封结合，这变成"部署时设置一次，运行时零策略查询"。

---

## 3. 形式化

基础 SOS 模型（状态、handle 表、不变量 $\mathcal{I}$）见 06。

### 3.1 时间参数化 SOS

状态扩展为 $\sigma(t)$；通用操作前提从声明权限改为有效权限：

$$\frac{HT_p(h) = (o, \rho) \quad R \in \rho_{eff}(h, t)}{\langle op_p(h, \ldots), \sigma(t) \rangle \longrightarrow \ldots}$$

**handle_dup 预算扩展**（权限、时间、次数、类型四重子集约束）：

$$\frac{HT_p(h_s) = (o, \rho) \quad \rho_{eff}(h_s, t) \neq \emptyset \quad \rho_{req} \subseteq \rho \quad \text{DUP} \in \rho_{eff}(h_s, t) \quad expiry' \leq expiry(h_s) \quad ops' \leq remaining(h_s) \quad allowed' \subseteq allowed(h_s)}{\langle dup_p(h_s, \rho_{req}), \sigma(t) \rangle \longrightarrow \langle ok(h_d), \sigma(t)[HT_p(h_d) \mapsto (o, \rho_{req}, \ldots)] \rangle}$$

**次数递减规则**：每次成功操作后 `remaining_ops--`。

**过期回收 TEMP-EXPIRE**（`AUTO_CLOSE` 时）：$refcount \mathrel{-}= 1$，条目置空。

### 3.2 三个核心定理

**定理 3.1（预算单调衰减）** 有效权限随时间的**每一维**单调不增：

$$\forall t_1 < t_2.\ \rho_{eff}(h, t_2) \subseteq \rho_{eff}(h, t_1)$$

*证明要点*：时间维（跨过 expiry 归零）、次数维（递减到 0）、权限维（dup/replace 子集）、类型维（allowed 收缩）各自单调，合取仍单调；无操作可延长 expiry / 增次数 / 扩类型。

**定理 3.2（不可刷新）** 持有者无法创建预算严格更宽的新 handle（dup 的预算子集约束；无 self-refresh 途径）。

**定理 3.3（过期原子性）** 过期相对进行中的操作原子（lookup 与 sweeper 竞争同一把 HT 锁，无中间窗口）。

### 3.3 组合定理（预算 × 类型 = 攻击面三围收窄）

**定理 7.1（攻击窗口缩减，重述）** 不可信组件 $U$ 持有类型受限、时间受限、次数受限的预算集 $H_U$：

$$AS(U) \subseteq \bigcup_{h_i \in H_U} Types(\tau_i) \times [0, e_i] \times [0, n_i]$$

攻击面在**类型 × 时间 × 次数**三围同时收紧。Log4Shell 场景见 11 §4.5。

---

## 4. 与基础不变量 $\mathcal{I}$ 的关系

预算字段不改变 I1（权限合法）、I3（引用计数）、I4（对象活性）、I5（类型安全）——它们只收紧 lookup 阶段的有效权限。$\rho_{eff} \subseteq \rho$ 恒成立，故既有保持论证在预算模型下继续成立。**这是"预算可无侵入叠加在既有 capability 模型上"的形式化依据，也是 05 信封调解器能复用 Native 机制的原因。**

---

## 5. 实现状态

- [x] 时间/次数维度已实现：`handle_control`（SET/GET_TEMPORAL、SET_LABEL，仅可增强）、sweeper、AUTO_CLOSE、继承与耗散。`kernel/abi/native/handle_table.c`
- [x] `user/tests/test_native_handle.c` 覆盖 op-count 衰减、expiry AUTO_CLOSE。
- [ ] 类型维度进入预算格（typed channel 是类型维度的传播约束，04）。
- [ ] 数据预算（`data_budget`）未实现（预留字段）。
- [ ] 与 05 信封的接线是**新的核心工作量**。

---

## 6. 风险

1. **新颖性**：leasing（Gray & Cheriton'89）、SPKI 有效期、OAuth scope+expiry 必须逐篇核对（09 §2.2）；我们可辩护的增量是：**内核强制 + 多维预算格 + 单调衰减/不可刷新形式化 + 双 ABI 部署载体**。
2. **效用**：预算模型必须用真实场景（包安装/插件）证明价值，而不是纸上推演（10 E2）。
3. **开销**：+16 bytes + 判定分支 + sweeper 必须量化（10 E1）。若不可接受，"O(1) 判定"卖点不成立。
4. **tick 单调性**：多核时钟一致性需在 08 机器检验中覆盖。
