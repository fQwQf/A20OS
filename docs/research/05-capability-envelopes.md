# 能力信封（Capability Envelopes）——C2 系统贡献（论文主角）

> **本文是研究主轴的系统贡献，也是论文的主角**：给未修改的 Linux 二进制"包上"能力纪律。能力信封把预算能力（03）从"新程序可用的机制"变成"部署时施加到任何遗留程序的手段"——这是能力系统摆脱"重写命令"（rewrite mandate）的唯一路径，也是 A20OS 双 ABI 内核存在的理由。
>
> **范围说明**：本文吸收旧「双 ABI 混合信任边界」的形式化（原 05-dual-abi-coexistence.md 的全部定理并入本文 §5）——混合信任边界从侧证升级为信封逃逸不可行性的核心防线。实现状态以源码为准；调解器核心与 §2.5 逃逸面语义已于 2026-08 落地（见 §7），W2 剩余增量亦列于 §7。

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

### 2.5 逃逸面语义：异步执行、描述符传递与持久对象（防线 #0）

> **咽喉完备性先于咽喉存在性**。§2.2 的调解器若只钩住经典同步 syscall，则"全获取咽喉"是假的。本节枚举三类已知的权威逃逸面并给出规范语义；每条都对应攻击套件的一个 case（11 §4.6）与文献依据（09 §4.6）。设计原则：**凡是"一个新的可用权威实例进入进程的 fd/映射空间"，或"一次对既有权威的使用"，都必须穿过 env_mediate。**

#### 2.5.1 权威进入点的完备清单

| # | 进入/使用路径 | 调解点 | 语义 |
|---|-------------|--------|------|
| A1 | `open/openat/creat/mknod` | 打开时创建影子 handle | 类型 ∩ rights ∩ 类上限（§2.2） |
| A2 | `socket/connect/accept` | 同上 | 网络 rights + 次数 |
| A3 | `pipe/pipe2/memfd/eventfd/timerfd/signalfd/userfaultfd` | 创建时 | 各自类型位 + 次数预算 |
| A4 | `mmap`（file-backed） | 映射时 | Map right + 类型 + 时间；映射计入影子表 |
| A5 | `shmat/shmget`（SysV/POSIX shm） | 挂接时 | 见 §2.5.4 |
| A6 | `recvmsg` 附带数据（SCM_RIGHTS） | **接收安装时** | 见 §2.5.3 |
| A7 | `pidfd_getfd`（跨进程取 fd） | 取得时 | 视为全新获取：类型检查 + 信封根预算新子预算 + 强制审计日志 |
| A8 | `/proc/self/fd/N` 魔链重开 | 解析后 | rights = 请求 ∩ 源影子 rights ∩ 类上限；新影子继承源剩余预算（单调衰减）——堵住"只读 fd 重开成读写"的经典逃逸 |
| A9 | io_uring fixed-file 注册 / 跨环 fd 安装 | **每个安装事件** | 等同 A6/A7 处理（见 §2.5.2） |
| A10 | io_uring SQE 执行 | **op 执行点**（非提交点） | 见 §2.5.2 |

#### 2.5.2 io_uring：在执行点调解，而非提交点

提交点是性能路径、执行点才是权威使用点——这与上游 RingGuard 的结论一致（eBPF'23：SQE 执行点审计 + 批量化）。语义：

1. **ring 创建**：信封内 `io_uring_setup` 仅当 flags 无 `IORING_SETUP_SQPOLL` 时放行；SQPOLL 使内核线程代表进程提交操作，破坏"操作归属清晰计数"。V1 直接拒绝 SQPOLL（返回 -EPERM），文档如实声明。
2. **每次 SQE 执行**：`env_mediate_use(shadow, opcode_class)` —— 类型类检查 + `remaining_ops--` + 时间检查。失败则该 SQE 以 `-EBUDGET`（复用 -EPERM/-EACCES 映射）完成，CQE 如实上报；**不中止整个 ring**。
3. **批量记账优化**（E11 关键）：per-ring 聚合计数器 + cacheline 对齐，避免每 SQE 一锁；时间检查按 jiffies 粗判 + 到期精确回收。
4. **fixed-file 表**：注册时逐个验证源 fd 是影子 handle 且类型合规；ring ↔ 影子集合建立边。跨环 fd 安装（MSG_RING SEND_FD 类）按 A6 处理。上游 Linux 已出现此类路径绕过 LSM 的实例（09 §4.6）——信封从第一天就把"任何 fd 安装事件"视为一等获取。
5. **过期语义**：根预算过期 ⇒ 该信封所有 ring 进入 rejected 态：未执行 SQE 全部以错误码完成，新提交被拒。保证与同步路径同一"过期原子性"（03 定理 3.3 在异步维度的推广）。

#### 2.5.3 描述符传递：预算随渡（budget travels with authority)

SCM_RIGHTS 使权威跨进程持久存在，直接威胁过期原子性。语义分三段：

- **发送（信封 → 外部）**：仅当 policy.propagation 允许该资源类型的对外委托。发送方保留自己的影子；发送本身计一次传播次数。
- **接收（外部/另一信封 → 信封内）**：为收到的 fd 创建新影子 handle，其预算字段 = **min(接收信封 policy 上限, 发送方剩余预算)**——委托链单调衰减的直接推广（03 继承规则）。无发送方预算信息时（外部非信封进程发来），按接收方 policy 上限新建。
- **诚实边界（V1）**：被发送到**非信封**进程的 fd，其可用性不受发送方信封过期影响。即：信封约束的是**信封内进程的权威生命周期**，不是对象在全系统的可见性。可选强模式 `ENVELOPE_FD_TIE_LIFETIME`：给底层对象打全局到期戳，内核对所有持有者在使用点检查——把"过期原子性"升级为系统级性质，代价是所有使用者各付一次 tick 比较；作为评估中的开关项（E11d），不作为默认主张。

#### 2.5.4 共享内存与持久对象

- **挂接**：shmat 创建影子 handle；映射足迹一次性计入 data_budget。
- **写计量**：V1 采用惰性记账——msync/shmdt 时按脏页数补扣 data_budget（页粒度，如实声明粒度上限）；不做逐字节拦截（开销不可接受且可被 mprotect 绕过语义混淆）。
- **过期**：信封创建的全部 shm 映射原子解除（从信封进程地址空间 unmap）；**已写出的字节不追溯**。
- **威胁模型对齐（必须写进论文）**：脚本在被授权窗口内主动交出的数据（写入 shm/文件/网络），过期后依然在外部存在——这不是逃逸，是预算语义的定义边界：**信封上界的是"未经授权获取 × 操作量 × 时间窗"，不是信息流控制**。共谋进程事前已有合法权利读取该对象时，泄露责任在策略授予，不在机制。11 §1 的表述与此一致。

#### 2.5.5 小结

三类逃逸面的共同根源是"权威的生命周期与获取路径解耦"。信封的回答统一为一句话：**影子 handle 是权威的唯一记账单位；任何使进程可获得可用权威的事件都是获取事件，任何消费权威的操作都是使用事件；两者分别过 env_mediate 的两个入口。** 咽喉完备性由 §8-verification 的覆盖矩阵从 `syscall_table.def` 自动生成核对，不由人工维护。

---

## 6. 单调采用（Monotone Adoption，C3）

**定义（能力覆盖率）** $\alpha$ = 进入信封 / 使用 Native ABI 的组件比例。

**定理 8.2（增量部署的单调保证，重述）** 覆盖率 $\alpha$ 单调增加（更多组件上信封、信封收紧）⇒ 已建模安全保证集合**单调增加、绝不回退**。

*证明要点*：给组件加信封只收紧其资源获取（新增约束），不改变其他组件的已有保证；信封收紧 = 预算维子集，单调。这给部署者"渐进采用无风险"的承诺。

**与 CompCert 的精神类比**：CompCert 证明编译优化保持语义；我们证明安全增强保持已有安全性质。

---

## 7. 实现状态与工作量

- [x] **信封调解器核心已实现（2026-08，分支 `research/osdi-envelopes`）**：
  - `kernel/ipc/envelope.c`（双 ABI 常驻构建）：policy 快照 + 影子 handle 表（按全局 fd 键控）+ 获取/使用双向调解 + 惰性过期清扫 + KILL_ON_EXPIRE + 主动撤销 + 观测计数器；
  - 控制面 syscall `sys_a20_envelope_{create,enter,revoke,stats}`（902-905）：supervisor create → fork → child enter → execve 部署模式，enter 单调（二次进入 -EINVAL）；
  - fork 共享根预算继承（refcounted）、execve 不可摆脱、task 退出自动释放；
  - 钩子全覆盖（§2.5 A1-A10）：openat（A1 权利推导 + A8 重开权利交集、跨 pid fail-closed）/socket（A2）/pipe·memfd·eventfd·timerfd·signalfd 创建即调解并入类登记表（A3）/shmat MEMORY 类检查（A5）/SCM_RIGHTS 发送 propagation 检查 + 接收安装前裁决（A6，被拒 fd 按 Linux 语义关闭并置 MSG_CTRUNC）/pidfd_getfd 全新获取裁决（A7）/io_uring READ·WRITE·FSYNC 执行点带方向位调解 + REGISTER_FILES 对信封任务 fail-closed（A9/A10）；
  - 方向位仅对已跟踪影子生效；祖父级（enter 前继承的）fd 豁免方向检查但照常计费。
- [x] **攻击套件已全绿**：`make smoke-envelope` 十三场景 QEMU 实测（清单见 docs/testing-gates.md），头号 case 即 §2.5 三类逃逸面对抗用例。
- [x] 依赖已就绪：预算能力机制（03）、typed channel（04）、双 ABI 分派、tokenized park/wake。
- [ ] **W2 剩余增量（不阻塞 pilot 实验）**：
  - shm 足迹 + 脏页 data_budget 记账（当前挂接仅做 MEMORY 类检查，无字节计费）；
  - ENVELOPE_FD_TIE_LIFETIME 强模式（对象级系统到期戳，§2.5.3 可选项）；
  - SCM_RIGHTS 接收预算细化 min(发送方剩余)（当前为接收方 policy 上限）；
  - readv/writev 仅记操作数不逐 iovec 计字节数；
  - io_uring fixed-file 注册对信封 fail-closed，正式按安装事件支持待实现；SQPOLL 当前内核不解析 setup flags（天然不存在），未来引入须同步拒绝。
- [ ] E2 pilot 三对照实验 + E7 攻击扩展 + E11 开销量化（10-evaluation，论文数据主来源）。

---

## 8. 威胁与风险

1. **实现负担**：调解器是真实的新的内核代码。必须保住"全获取咽喉"完整——任何资源 syscall 漏钩都是逃逸。
2. **开销**：调解若使 syscall 延迟劣化 >~20%，"部署选项"的吸引力下降。必须诚实测量。
3. **诚实评估**：不能只报"攻击失败率"——必须对照（无信封/仅 seccomp/仅 Landlock）在同一攻击集上的表现。
4. **语义边界**：不得声称"信封 = 完整安全沙箱"——它附加能力预算，不解决被攻破进程的内部行为（如 ROP）。Threat model（11）必须写清。
