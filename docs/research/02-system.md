# 系统设计与实现状态速览

> **本文提供研究所需的最小系统背景**，取代旧 02-native-api-design.md（API 规格）、03-implementation-plan.md（实现方案）、08-architecture-deep-dive.md（架构深挖）在研究目录中的位置。完整规格以 `docs/native-abi/` 为准，架构与混合内核设计以 `docs/OS-Design.md`、`docs/hybrid-kernel/` 为准；本文只保留研究论证需要的要点和设计决策记录。
>
> **数字口径**：实现状态以 `docs/native-abi/08-runtime-status.md` 和源码为准（Native 分发表约 126-134 入口；形式化核心为 53 个 syscall，见 06）。

---

## 1. 系统是什么

A20OS 是一个**双 ABI 混合内核**：

- **Linux ABI**（~343 登记入口）：运行未修改的静态 musl 程序（git、vim、mksh 等），无 syscall 层面的能力模型。
- **Native ABI**（~126-134 登记入口）：handle + rights + channel + EventQ + VMO/VMAR 的能力接口，是本文全部研究机制的载体。

内核内部子系统（调度、MM、VFS、网络）与驱动共用同一特权地址空间（宏内核形态），但 Native 用户可见对象受 capability 检查约束（吸收微内核的对象纪律）。驱动可静态链接、以 `.a20drv` 模块或 Native 用户态服务部署。

**对研究的含义**：双 ABI 不是两个独立内核，而是共享 core 层（task、MM、VFS、IPC）的两套用户接口。这既是 05 能力信封（给未修改 Linux 二进制附加能力预算）的实现基础——内核是资源获取的唯一咽喉——也是"能力机制在真实系统上的可运行载体"。

---

## 2. Native ABI 设计要点（研究相关）

| 主题 | 设计 | 研究对应 |
|------|------|---------|
| 对象模型 | 13 种形式化对象 / 14 种实现对象；统一 handle 表（可增长到 65536） | 06 §1 |
| 权限 | 14 位 rights 位域；`dup`/`replace`/`transfer`/`spawn` 强制子集关系（单调递减） | 06 §3.3 |
| 内存 | VMO（物理后备对象）+ VMAR（映射容器），保护 = 请求 prot ∩ handle rights ∩ VMAR 标志 | 06 §6 |
| IPC | Channel（64KB 数据 + 8 handle，两阶段写入，reserve-then-dequeue 无部分投递）+ typed channel 类型约束 | 04 |
| 事件 | EventQ 统一等待 channel/task/timer/driver IRQ；file/socket/pipe 事件源已接线 | 06 §2.7 |
| 授权维度 | `expiry_tick` + `remaining_ops` 预算能力（类型×权限×时间×次数×传播）；`handle_control(SET_TEMPORAL)` 仅可增强 | **03** |
| 进程创建 | 单步 `task_spawn`（显式 handle 注入 + 权限降级），无 fork | 01 §3、06 §2.4 |
| 安全标签 | 3 级标签格 {L,M,H}，$\mathcal{L}$-noninterference | 06 §8.5 |
| ABI 演进 | `{size, version}` 结构体版本化 + E-APPEND/E-DEPRECATE/E-RESERVED | 下文 §4 |
| **能力信封** | **给未修改 Linux 二进制附加能力预算的调解器（设计，未实现）** | **05（研究主线）** |

---

## 3. 实现状态（研究机制的成熟度）

| 机制 | 状态 | 证据 |
|------|------|------|
| typed channel（类型 bitmask + 上限强制） | 已实现 + smoke | `kernel/ipc/a20_channel.c`、`docs/native-abi/05-ipc.md`、08-runtime-status |
| 时态能力（SET_TEMPORAL/sweeper/AUTO_CLOSE） | 已实现 + smoke | `kernel/abi/native/handle_table.c`、`docs/native-abi/06-security.md` |
| 阻塞 IPC + Park/Wake（无丢失唤醒） | 已实现 + SMP 压力验证 | `docs/roadmap/park-wake-protocol-split.md` |
| 级联释放 / 无部分投递 | 已实现 | 08-runtime-status §6 |
| VMAR / Pager / monitor / task_mem 深化 | 已实现 | `docs/native-abi/09-native-abi-deepening.md` |
| **task_clone（能力安全续体）** | 已实现 + smoke | 08-runtime-status §5d、`smoke-mlibc-fork` |
| **fork()/execve() + mksh on mlibc** | 已实现 + smoke | `smoke-mlibc-mksh`（内建 + 顺序外部命令） |
| 性能评估 | **未实测** | 10-evaluation.md |
| 机器检验 | **未开始** | 08-verification.md |

**关键诚实点**：Native ABI 的实现成熟度（功能）与"研究证据强度"（评估/验证）是两个维度——前者已相当完整，后者是当前全部缺口所在。

---

## 4. ABI 结构体版本化（设计经验，非贡献）

> 旧 06 曾把"运行时无关的 ABI 演进"列为贡献 C4。文献与实践（Zircon vDSO、Linux ioctl 演进、Android binder、Windows ntdll）表明该模式**已是常见工程实践**，优先性难以主张，故降级为设计经验记录于此。

设计：每个 syscall 参数结构体首字段 `{size, version}`；内核入口校验：
- `size < |S_1|`（最小版本结构）→ 拒绝；
- `size > |S_n|`（比内核已知更大）→ 截断，只读已知字段；
- `version == 0` 或 `> 已知` → 拒绝。

演进规则（形式化见 06 §10）：
- **E-APPEND**：只允许追加字段，保持旧偏移/类型 → 旧程序兼容。
- **E-DEPRECATE**：字段保留偏移，标记弃用，非零值报错。
- **E-RESERVED**：未定义 flag 位必须为 0。

已落地：`liba20c`、`liba20rt` 的调用已迁移到版本化结构体，内核侧 `A20_VALIDATE_AND_COPY` 执行校验。这解决"新内核 + 旧程序 / 新程序 + 旧内核"的兼容，无需 vDSO。

---

## 5. 工程审计（不属研究主张，但为评估背书）

Linux ABI 侧的工程性能工作已移至 `docs/roadmap/`：
- `linux-audit-and-performance-comparison.md`：A20 vs Linux 的源码审计与 parallel-build 实测边界（差距约 2.3-2.5×，工程侧）。
- `perf-overhaul.md`：锁分桶 + fsync 收敛 + callsite 归因工具。
- `park-wake-protocol-split.md`：park/wake 分锁与无丢失唤醒协议（亦是 Native 阻塞 IPC 的正确性基础）。

论文引用这些时只能作为"工程可实现性"背书，**不能**作为 Native ABI 性能或能力机制价值的证据。
