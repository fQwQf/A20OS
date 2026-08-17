# 能力信封（Capability Envelopes）——C2 系统贡献（论文主角）

> **本文是研究主轴的系统贡献，也是论文的主角**：给未修改的 Linux 二进制"包上"能力纪律。能力信封把预算能力（03）从"新程序可用的机制"变成"部署时施加到任何遗留程序的手段"——这是能力系统摆脱"重写命令"（rewrite mandate）的唯一路径，也是 A20OS 双 ABI 内核存在的理由。
>
> **范围说明**：本文吸收旧「双 ABI 混合信任边界」的形式化（原 05-dual-abi-coexistence.md 的全部定理并入本文 §5）——混合信任边界从侧证升级为信封逃逸不可行性的核心防线。实现状态以源码为准；本设计部分尚未实现（W2 计划）。

---

## 1. 为什么信封是主会级贡献

能力系统从未被采纳，根因是**重写命令**：要享受能力纪律，必须重写软件。Capsicum 试图缓解（fd 级、需源码改动、受 fd 语义包袱限制）；Landlock/seccomp 是路径/syscall 级、无对象模型、无时间/次数/类型维度。

**能力信封的核心论断**：

> **一个双 ABI 内核是资源获取的唯一咽喉。既然所有资源必经 syscall 获取，内核就能在不修改二进制的前提下，为每个进程附加一份"预算 + 类型 + 权限"的能力策略——把能力纪律从"写进代码"变成"包在部署时"。**

这回答了 OSDI 审稿人的杀手问题："为什么不做在 Linux 上？"——因为需要在**内核**同时具备：① 完整的能力对象模型（handle/rights/channel）；② 预算机制（03）；③ 能力无感知的二进制兼容路径（Linux ABI）。三件套只有 A20OS 这种双 ABI 能力内核同时具备。

---

## 2. 设计

### 2.1 部署原语

```c
// 把一个未修改的 Linux 二进制放进信封
int64_t envelope_spawn(const char *path,
                       const a20_env_policy_t *policy,
                       a20_handle_t *out_env_handle);
```

```c
typedef struct a20_env_policy {
    a20_object_type_t allowed_types;      // 可获取的资源类型 bitmask
    a20_rights_t      rights_by_class[N]; // 每资源类的 rights 上限
    uint64_t          time_budget_ns;     // 信封生命周期
    uint32_t          op_budget;          // 总资源操作预算
    uint64_t          data_budget;        // 可选：累计数据传输量
    a20_channel_type_t propagation;       // 可向外委托的类型/子预算
    uint32_t          flags;              // KILL_ON_EXPIRE / MONITOR / LOG
} a20_env_policy_t;
```

### 2.2 调解器（mediator）：全获取咽喉

信封进程仍走 Linux ABI（二进制不变）。内核在**每个资源获取/使用 syscall** 插入 `env_mediate()`：

```
open/openat  → 查类型 ∈ allowed_types；O_RDONLY→R 等映射 ⊆ rights_by_class
socket/connect/accept → 类型 + 网络 rights + 次数/时间
mmap        → 类型（file/anon）+ Map right + 时间
pipe/dup    → 类型 + 次数
fork/clone  → 子进程继承信封（预算按委托链耗散）
execve      → 复查 policy（禁止换身份逃逸）
操作使用    → read/write/send 计 op_budget/data_budget；查 time_budget
```

- 每次获取创建**影子 handle**（映射 fd↔handle），供审计与传播控制。
- 预算耗尽/过期：默认 `ACCESS` 拒绝下次操作；`KILL_ON_EXPIRE` 强制回收进程全部资源。
- **开销目标**：调解 = 一次查表 + 若干位运算 + 一次计数，须在 10 E1 量化。

### 2.3 逃逸不可行性（三条防线）

| # | 逃逸路径 | 防线 | 保证 |
|---|---------|------|------|
| 1 | 直接 syscall 获取未授权资源 | **全获取咽喉**：所有资源创建必经 `env_mediate` | 无旁路 |
| 2 | 经 Native ABI 走私 | **混合信任边界**（§5）：信封进程是 Linux ABI，接触不到 Native handle 表 | 无走私通道 |
| 3 | fork/exec 换身份 | **子进程继承信封**，exec 复查 policy | 无身份逃逸 |

**形式化论据**：信封进程的全部资源获取是 syscall 序列的集合；kernel 是这些 syscall 的唯一实现者；`env_mediate` 附加在每条资源相关 syscall 上 ⇒ 进程不可获得超出 policy 的资源。

---

## 3. 预算在信封内的流转

信封 = 进程级预算根 + 影子 handle 表。每个影子 handle 是 03 的预算能力实例：

```
信封根预算 (types, rights, time, ops)
  ├─ 影子 handle f_1 (FILE, {R}, 30s, 1000ops)   ← open 时创建
  ├─ 影子 handle s_1 (SOCKET, {R,W}, 30s, 500ops) ← socket 时创建
  └─ 子进程信封 (继承，预算按委托链耗散)
```

- **单调衰减**：每个影子 handle 与信封根都在耗散；根到期则全部影子失效。
- **不可刷新**：进程无法自行延长（接口只可增强约束；03 定理 3.2）。
- **可回收**：`KILL_ON_EXPIRE` 或 `task_kill` 回收全部影子资源（无悬空）。

---

## 4. 场景：包安装 / 插件的 OS 级信封

**真实问题**：npm postinstall、pip build、cargo build、CI runner 全部以"不可信代码 + 用户全部权限"运行。现有缓解都是**网络隔离**（pip build isolation）或**用户态沙箱**（Firejail/bubblewrap），没有对象级能力预算。

**信封策略示例**：

```
npm 安装脚本:
  allowed_types = { FILE, SOCKET, PIPE }
  rights: FILE={R,W}  SOCKET={R,W}（禁 LISTEN）
  time_budget = 300s, op_budget = 100_000, data_budget = 100MB
  propagation = { PIPE, FILE: read-only }
```

- 脚本只能碰文件/出向 socket，不能 listen、不能碰 task/共享内存/设备。
- 300 秒或 10 万次操作或 100MB 后自动失效，即使脚本被攻破。
- **零源码改动**——真实 npm 包直接放进信封。

**评估（10 E2）**：真实包安装脚本在信封内功能完整 + 攻击注入全部失败 + 对照（无信封）成功。

---

## 5. 混合信任边界（并入本文件）：调解器与被调解者的隔离

> 旧「双 ABI 混合信任边界」定理在此成为信封的防线 #2。编号沿用旧体系（1.1-1.4）。

**定义 1.1（能力可观测性）** $Obs(p,h,\sigma) \triangleq HT_p(h) \neq \bot \lor \exists o. access(p,o,\sigma) \land depends(o,h,\sigma)$。

**定理 1.1（直接不可观测性）** Linux ABI 进程（含信封进程）无法直接观察 Native handle（其 handle 表为 NULL）。

**定理 1.2（降级精确性）** 跨 ABI 共享资源降级通道只传递 $o$ 的数据+元数据，不含 handle 权限信息（VFS 不存储写入者的能力元数据）。

**定理 1.3（能力边界，核心）** 设 $\sigma$ 满足 $\mathcal{I}$，Linux 进程执行满足 $BridgeFree$（不经过显式 channel bridge）。则：

$$\forall p_l \in P_{linux}, h_n \in H_{native}.\ BridgeFree \implies (\neg Obs(p_l,h_n,\sigma) \lor (p_l,h_n) \text{ 构成精确降级通道})$$

**对信封的意义**：信封进程无法把影子 handle 走私进 Native ABI 空间，也无法观察其他 Native 进程的能力状态 → 信封的边界在**两个方向**都成立。

**定理 1.4（跨 ABI 性能隔离）** Linux syscall 延迟不受 Native 操作影响（分派路径独立）。

**诚实边界**：Linux ABI 进程彼此之间**不**提供能力隔离（无完整 namespace/seccomp/LSM）。信封是给单个 Linux 进程附加能力纪律的手段；未上信封的 Linux 进程之间的隔离不是本文主张。

### 2.4 与 Native ABI 的职责分工

信封不替代 Native ABI，二者互补（详见 00 §2.1）：

| | Native ABI | 能力信封 |
|---|---|---|
| 面向 | **可信组件**（自己写的核心服务） | **不可信组件**（第三方插件/包脚本） |
| 机制 | 显式最小权限、typed IPC、委托（合作式） | 强制预算、影子 handle、继承耗散（对抗式） |
| 作用 | 架构层的"精确" | 防御层的"扩散" |
| 采用梯子 | 终点 | 入口 |

**评审自问**："没有 Native ABI，信封能成立吗？"——不能：信封的预算机制、类型传播、逃逸防线全部建立在 Native handle 模型上；Native ABI 同时是信封的机制来源与迁移目的地。

---

## 6. 单调采用（Monotone Adoption，C3）

**定义（能力覆盖率）** $\alpha$ = 进入信封 / 使用 Native ABI 的组件比例。

**定理 8.2（增量部署的单调保证，重述）** 覆盖率 $\alpha$ 单调增加（更多组件上信封、信封收紧）⇒ 已建模安全保证集合**单调增加、绝不回退**。

*证明要点*：给组件加信封只收紧其资源获取（新增约束），不改变其他组件的已有保证；信封收紧 = 预算维子集，单调。这给部署者"渐进采用无风险"的承诺。

**与 CompCert 的精神类比**：CompCert 证明编译优化保持语义；我们证明安全增强保持已有安全性质。

---

## 7. 实现状态与工作量

- [ ] **信封调解器未实现**——这是 W2 的核心工作量（8-12 周）：
  - `kernel/abi/linux/` 资源 syscall 的 `env_mediate()` 钩子；
  - 影子 handle 表 + 预算根（复用 03 的 handle 表/时态字段）；
  - `envelope_spawn` + policy 解析 + 继承/耗散；
  - exec/fork 的复查与继承。
- [x] 依赖已就绪：预算能力机制（03）、typed channel（04）、双 ABI 分派、tokenized park/wake。
- [ ] 攻击套件（信封逃逸）未实现（11 §4.6）。

---

## 8. 威胁与风险

1. **实现负担**：调解器是真实的新的内核代码。必须保住"全获取咽喉"完整——任何资源 syscall 漏钩都是逃逸。
2. **开销**：调解若使 syscall 延迟劣化 >~20%，"部署选项"的吸引力下降。必须诚实测量。
3. **诚实评估**：不能只报"攻击失败率"——必须对照（无信封/仅 seccomp/仅 Landlock）在同一攻击集上的表现。
4. **语义边界**：不得声称"信封 = 完整安全沙箱"——它附加能力预算，不解决被攻破进程的内部行为（如 ROP）。Threat model（11）必须写清。
