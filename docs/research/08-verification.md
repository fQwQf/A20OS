# 机器检验计划与理论-实现收口

> **本文是 C4（方法议程）的主文档**，也是通往 OSDI 的硬性前置。目标：把 06/07 的纸笔证明升级为可机器检验的性质，并明确"被证明的模型"与"运行的实现"之间的覆盖关系——回答审稿人最尖锐的问题："你证明的东西和你跑的东西是一回事吗？"

---

## 1. 为什么必须机器检验

旧 00-index §8 已自述：所有证明为纸笔证明。在 seL4（Isabelle/HOL 全量验证）、CertiKOS（分层精化）、Hyperkernel（推按钮符号执行）已经立起门槛的今天，**纯纸笔 SOS 证明在 OSDI 没有说服力**。但 A20OS 无法也不应复制 seL4 的工程规模（8700 行 C 验证 = 20 人年）。折衷路径：

1. **TLA+/TLC 模型检验**：对核心并发状态机（handle table、sweeper、typed channel、park/wake）做穷尽小规模验证——成本低、收益快、覆盖真正的竞态问题。
2. **Lean/Isabelle 关键性质形式化**：对最能证明"数学内容"的定理（预算单调性、不可刷新、过期原子性、通道类型安全）做形式化——论文的"数学卖点"。
3. **运行时不变式检查**：把 I1-I5 + 预算不变量编译进内核（`CONFIG_INVARIANT_CHECK`），在每个 syscall 后检查——把"证明"与"运行"在运行时缝合。

**参考谱系**（详见 09 §5）：Hyperkernel 的"裁剪到核心 + 可推按钮"方法论最贴近我们的规模和目标。

---

## 2. TLA+ 建模范围（首选、S2 优先级最高）

### 2.1 模块 1：Handle Table + 预算能力（对应 03）

建模对象：
- handle 表（有限容量）、`dup/close/transfer/spawn` 注入、`handle_control(SET_TEMPORAL)`
- `expiry_tick`、`remaining_ops`、sweeper（周期性扫描 + AUTO_CLOSE）

待验证性质：
```
∃ 安全不变式: I1（权限合法）、I3/I4（refcount 平衡、对象活性）
∃ 单调性:   ρ_eff(h, t2) ⊆ ρ_eff(h, t1) for t2 > t1         （定理 3.1）
∃ 不可刷新:  dup 后 expiry' ≤ expiry ∧ ops' ≤ remaining       （定理 3.2）
∃ 原子性:   不存在操作成功与 sweep 过期同时成立的交叉状态       （定理 3.3）
∃ 无丢失:   AUTO_CLOSE 释放的条目 refcount 同步 -1
```

状态空间控制：H ≤ 8、进程 ≤ 3、sweep 间隔 ≥ 2 tick。用 TLC 穷举 + Apalache 符号执行。

### 2.2 模块 2：Typed Channel（对应 04）

建模：`channel_create(type)`、`send/recv` 的类型 bitmask 检查、handle 类型化能力流。

待验证性质：
```
∃ 类型安全:  ∀m ∈ messages(c). ∀h ∈ m.handles. τ(o_h) ∈ T.send_handle_types   （定理 2.1）
∃ 无部分投递: reserve-then-dequeue 失败路径不留半条消息                        （08 §14 精化）
```

### 2.3 模块 3：Park/Wake 无丢失唤醒（工程正确性基础）

建模：tokenized Park/Wake（wait_seq、PREPARING/PARKED/WOKEN 状态机、timer heap）。 这一条来自 `docs/roadmap/park-wake-protocol-split.md` 的工程不变量，是"Native 阻塞 IPC 正确性"的前提，也是双 ABI 共享的基础。

待验证性质：**无丢失唤醒**（`∃ 每个 park 要么在 commit 读到 WOKEN，要么被调度回`）、**无重复唤醒**、**陈旧唤醒隔离**（seq 校验）。

### 2.4 模块 4：能力信封调解器（对应 05；核心状态机已建成并全绿）

**这是 C2 系统的验证核心**——证明"全获取咽喉"成立、信封不可逃逸：

建模对象：
- 信封 policy（allowed_types / rights 上限 / time_budget / op_budget / propagation）
- `env_mediate()` 拦截的所有 Linux 资源 syscall 集合（open/socket/connect/mmap/pipe/fork/clone/exec）
- 影子 handle 表 + 预算根 + 继承/耗散

待验证性质：
```
∃ 咽喉完备性: 每个资源获取 syscall ∈ Mediated（政策覆盖所有资源创建路径）
∃ 逃逸不可行:  不存在状态 σ'，使信封进程持有 policy 之外的资源类型或超出预算
∃ 预算耗散:    子进程继承后预算 ⊆ 父进程（时间/次数/类型全维）
∃ 刷新不可行:  信封进程无法延长自身预算或扩大 allowed_types
∃ 单调采用:    收紧 policy ⇒ 可达资源集合单调收缩（支持 C3 定理 8.2）
```

**关键**：模块 4 的"咽喉完备性"枚举必须与 05 §2.2 的 syscall 覆盖清单逐项对应，这是审稿人检查逃逸的第一攻击点。

**进展（2026-08）：状态机核心已建模并全绿**

模型：`docs/research/verification/Envelope.tla`（+ `Envelope.cfg`），抽象自 `kernel/ipc/envelope.c`。

动作全集对应实现语义：Enter（单调进入，无 Leave，execve 不可摆脱）/ Acquire（A1-A3 创建获取，权利 ⊆ 类上限且逐次计费）/ TransferIn（A6 接收 + A7 pidfd_getfd：授权 clamp 到类上限，空授权即拒绝）/ Reopen（A8 重开权利 = 请求 ∩ 源影子，不可提权）/ SendOut（A6 发送侧 propagation_types 门控）/ Use · UseX（已跟踪影子强制方向位；祖父级豁免方向检查但照常计费）/ LazyExpire · Revoke（惰性过期与主动撤销）/ Kill（KILL_ON_EXPIRE 一次性标志）。

已检验不变式：
- TypeAllowed：影子类型 ∈ allowed_types
- RightsSubCap：影子权利 ⊆ 类上限
- OpsNonNeg / DataNonNeg：预算不越界
- ClockBounded

已检验时序性质：NoResurrect（`[][expired => expired']_vars`，过期单向）、KillOnce（killSent 单向）。

TLC 结果（OpsMax=3, DataMax=4, ExpireAt=3, MaxTick=6，3 任务 × 2 gfd × 2 类型）：**27829 states generated / 4620 distinct / depth 14，全部通过，<1 s**。复现命令：`java -cp tla2tools.jar tlc2.TLC Envelope.tla`。

运行时侧：E8 审计已落地——syscall 906 触发全量信封不变式走查（TypeAllowed / RightsSubCap / 预算界 / 挂载一致性），smoke-envelope 套件末尾自动执行并要求零违例。这是模型不变式与运行时系统之间的持续核对通道。

尚未覆盖（诚实边界）：
- 咽喉完备性枚举（上表第 1 条）属实现侧覆盖矩阵（§4），不是本状态机模型可表达的命题——由 `syscall_table.def` 自动生成核对（W2）；
- 委托链预算耗散、单调采用（C3）作为策略精化性质仍未建模；中介层公平性 liveness 已建模并机器检验（2026-08：Envelope.tla 增 Fairness=WF(Tick/LazyExpire/Kill)，ExpiryLiveness `<>expired`、KillLiveness `expired~>killSent`、DenyAfterExpiry 不变式全绿）；
- 本模型是设计层状态机，不是 C 实现的精化证明——07 §8 的精化议程保持不变。

---

## 3. Lean/Isabelle 形式化范围

选择**不需要大量工程建模、纯数学内容**的定理，避免陷入 C 代码建模的无底洞：

| 定理 | 来源 | 形式化难度 | 价值 |
|------|------|-----------|------|
| 时态单调递减（定理 3.1） | 03 | 低-中 | 核心卖点 |
| 时态不可刷新（定理 3.2） | 03 | 中 | 核心卖点 |
| 通道类型安全（定理 2.1） | 04 | 低 | 配套卖点 |
| 权限单调递减（06 定理 3.2） | 06 | 低 | 基础 |
| 委托链耗散（03 §2.4） | 03 | 中 | 组合卖点 |
| 咽喉完备性 / 逃逸不可行（05） | 05 | 中 | **信封卖点** |

**策略**：用归纳类型编码 handle/rights/时态字段，形式化 SOS 转移关系为"前提 → 结果"的归纳谓词，逐定理归纳证明。**2-4 个核心定理形式化即可**，不必全量。

**状态（2026-08 更新）**：Lean v4.33.1 工具链已从 GitHub release 直接获取并验证可用（绕过了 elan 网络超时问题）。形式化产物 `docs/research/verification/lean/BudgetLattice.lean` 已建成并多轮扩展——预算格核心引理（单调衰减 `deduct_le`、传递衰减 `decay_transitive`、严格正衰减 `deduct_strict`、expiry latch 单向性 `latch_true`）、transfer clamp 双向界定（`and_field_le_cap` / `and_field_preserves` / `and_field_idem`）、方向位强制（`xferred_wr_needs_both` / `xferred_rd_needs_both`：经 clamped 转移安装的 shadow 上读/写方向通过 ⇒ 源请求与策略 cap 双真）、委托无升级（`del_wr_needs_both` / `del_rd_needs_both`：委托权行使 ⇒ 委托方授权与接收方 cap 双真，对应 03 §3.2 rights monotone decrease / no-escalation）、跨信封预算细化（`scm_recv_le_receiver` / `scm_recv_le_sender`：SCM_RIGHTS 接收计费的安全规格界 = min(发送方金额, 接收方剩余)；v1 内核构造性满足——接收仅从自身剩余扣 1 个 op、不耦合发送方侧金额，05 §2.5.4）**全部零 sorry 通过**。剩余：Mathlib 依赖的高级引理待后续扩展；时序/liveness 主张按分工归 TLA+——弱公平假设下 ExpiryLiveness/KillLiveness/DenyAfterExpiry 已机器检验（§2.1），Lean 专注代数性质。咽喉完备性走 §2.4 覆盖矩阵路线不变。

---

## 4. 理论-实现收口矩阵

审稿人必问："53 个核心 syscall 的证明覆盖 126 个实现入口的哪一部分？"本文档维护这张矩阵，作为论文的 Appendix。

信封侧新增一行（2026-08）：**Linux ABI 咽喉完备性**按 05 §2.5 的契约对全部 365 个登记 syscall 做了机械分类，产物为自动生成的 `docs/research/verification/envelope_coverage.md`（20 已调解 / 46 挂 W2 / 299 无权威参与）；`make check-envelope-coverage` 在矩阵与源码漂移时失败。

| 实现分区 | 实现 syscall 数 | 对应 SOS 规则 | 覆盖状态 | 机器检验 |
|---------|--------------|-------------|---------|---------|
| **Linux ABI 咽喉完备性（信封调解，05 §2.5）** | **365 全量** | 05 §2.5 契约 | **20 已调解 / 46 PLANNED-W2 / 299 NA** | **check-envelope-coverage（自动核对）** |
| handle（dup/close/replace/query/control） | ~15 | 06 §2.2 | 已建模 | TLA+ 模块1 |
| channel（send/recv/typed） | ~8 | 06 §2.6 + 04 | 已建模 | TLA+ 模块2 |
| event queue（create/watch/wait） | ~6 | 06 §2.7 | 部分 | 待补 |
| task（spawn/wait/kill） | ~10 | 06 §2.4 | 部分 | 待补 |
| VMO/VMAR | ~10 | 06 §2.5/§6 | 待补 | — |
| path/fs | ~12 | 未纳入核心模型 | 明确排除 | 明确理由 |
| net | ~10 | 未纳入核心模型 | 明确排除 | 明确理由 |
| time/security/debug/system | 其余 | 查询/调试类 | 明确排除 | 明确理由 |

**规则**：每一行必须写明"已覆盖/部分/排除 + 理由"。被排除的行要在论文中声明"能力安全性质仅依赖被覆盖的核心，被排除行不参与能力决策"。**绝不允许模糊的"整个内核都被验证"表述。**

---

## 5. 与实现的差距声明（旧 00-index §8 的继承）

- 07 的 trace 分类明确覆盖 43/53 个形式化核心 syscall；缺 `task_wait/task_kill/task_info`、4 个 path 操作、`event_wait`、`net_sendmsg/net_recvmsg`。
- 当前实现 126+ syscall，新增入口尚未逐项加入 SOS 规则与 error-path 精化矩阵。
- 06 的并发精化映射与 C 内存模型论证依赖 spinlock 原子性假设，未机器检验。

**行动计划**：① TLA+ 四模块完成（§2；模块 4 核心状态机已于 2026-08 建成并全绿——见 §2.4 进展块，剩余为咽喉覆盖矩阵联动；公平性 liveness 已于 2026-08 增补并全绿：Fairness=WF(Tick/LazyExpire/Kill) 下 ExpiryLiveness/KillLiveness/DenyAfterExpiry 通过）；② Lean 核心定理（§3，13 定理零 sorry 完成）；③ 收口矩阵逐行填实（§4）；④ 运行时不变式检查器（10 §E8）。

---

## 6. 验证环境与可复现性

- TLA+：TLC 模型检验器 + Apalache（符号执行），进 `tools/`，CI-like 门禁。
- Lean：Mathlib 环境，形式化文件进 `research/verification/`（或 `tools/formal/`）。
- 运行时不变式：`CONFIG_INVARIANT_CHECK` 进 smoke gate，随每轮压力测试运行。
- **每个验证结果必须可一键复现**（与正式基准测试同等的证据纪律）。

---

## 7. 里程碑

| 里程碑 | 内容 | 对应论文阶段 |
|--------|------|------------|
| V1 | TLA+ 模块1（handle+sweeper）通过 | S2（12-positioning） |
| V2 | TLA+ 模块2（typed channel）+ 模块3（park/wake） | S2 |
| V3 | Lean 形式化核心定理（3.1/3.2/2.1） | S2 |
| V4 | 运行时不变式检查器 + E8 全套跑通 | S3 |
| V5 | TLA+ 模块4（信封咽喉完备性 + 逃逸不可行） | S3（依赖信封实现） |
| V6 | 收口矩阵填实 + 论文 Appendix | S5/S6 |
