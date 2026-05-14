# A20OS Native ABI：贡献定位与设计空间分析

> 本文档系统化地分析 A20OS Native ABI 在操作系统接口设计研究中的贡献定位。内容包括：设计空间的维度映射、与现有工作的精确对比、新贡献的明确声称、对声称的威胁分析，以及面向 OSDI/SOSP 审稿人的定位声明。

---

## 1. 设计空间映射

### 1.1 操作系统接口设计空间

操作系统用户态接口的设计可分解为以下正交维度：

```
维度 1：资源标识模型
  ├── 异构标识符（POSIX: fd/pid/tid/sid/shmid/timerid/...）
  ├── 统一文件抽象（Plan 9: 一切皆文件）
  ├── 统一 handle 抽象（Zircon/A20: handle + rights）
  └── 纯能力引用（seL4: CNode capability 引用）

维度 2：权限模型
  ├── 无权限（DOS, 早期 Unix）
  ├── 粗粒度 ACL（POSIX: uid/gid/mode bits）
  ├── 细粒度 RBAC（SELinux, AppArmor）
  ├── 对象能力（seL4, EROS, A20: capability rights）
  └── 硬件能力（CHERI: capability hardware tags）

维度 3：进程创建
  ├── fork（Unix: 地址空间复制 + COW）
  ├── fork + exec（Unix 组合）
  ├── posix_spawn（受限参数化 fork+exec）
  ├── 分解式创建（seL4: 5+ 步骤, Zircon: create+start）
  └── 显式 spawn（A20: 单步 + handle 注入）

维度 4：事件/等待模型
  ├── select/poll（O(n) 扫描，fd only）
  ├── epoll/kqueue（O(ready)，fd + 部分扩展）
  ├── io_uring（环形缓冲，任意操作）
  ├── 统一 port wait（Zircon: zx_port_wait）
  └── 统一 event queue（A20: event_wait on any handle）

维度 5：ABI 演进
  ├── 事实固定（Linux: syscall 不可变，新 syscall 编号）
  ├── vDSO 版本化（Zircon: vdso 版本协商）
  ├── 结构体版本化（A20: {size, version} + 追加规则）
  └── 协议版本化（9P: version handshake）

维度 6：POSIX 兼容策略
  ├── 原生 POSIX（Linux, FreeBSD）
  ├── POSIX 子集兼容（Redox, Fuchsia: libfdio）
  ├── POSIX 兼容层（A20: liba20posix 用户态 shim）
  ├── 不兼容（seL4, NOVA）
  └── 绕过（Exokernel, Unikernel）
```

### 1.2 A20OS 在设计空间中的位置

```
              资源标识    权限模型      进程创建      事件模型     ABI演进     POSIX兼容
POSIX         异构        粗粒度ACL     fork          select/poll  事实固定     原生
Plan 9        文件抽象    粗粒度ACL     fork+rfork    无(select)   协议版本化   不兼容
seL4          纯能力      对象能力      分解式(5+步)  无(轮询)     无           不兼容
Zircon        handle      对象能力      两步(create+  port_wait   vDSO版本化   兼容层(libfdio)
                                       start)
Redox         文件抽象    粗粒度+部分   fork(exec)    epoll-like   无           子集兼容
Capsicum      fd          fd能力        fork+exec     epoll        无           扩展POSIX
io_uring      fd          无            fork+exec     io_uring     无           原生

A20OS Native  handle      对象能力      spawn(单步)   event_queue  结构体版本化  兼容层(shim)
              ↓           ↓             ↓             ↓            ↓            ↓
              (Zircon级)  (简化seL4)    (新设计)      (类Zircon)   (新设计)     (新策略)
```

**Insight**：A20OS 不是在任何单一维度上的突破，而是在六个维度的**组合**上找到了一个未被占据的设计点——一个在教学内核上可实现、具有形式化安全保证、提供双 ABI 渐进迁移路径的系统。

---

## 2. 贡献声称与论证

### 贡献 C1'：时态能力演算（Temporal Capability Calculus）

**声称**：A20OS 提出内核级的时态能力模型——handle 的有效权限 $\rho_{eff}(h,t)$ 同时受声明权限、过期时间和操作次数约束——并证明时态权限单调递减（09 定理 3.1）、时态不可刷新性（09 定理 3.2）和过期原子性（09 定理 3.3）。

**新颖性论证**：
- Zircon/seL4 的能力一旦授予就永久有效，没有时间或操作次数受限的概念
- OAuth 2.0 的 token 过期是用户态协议，内核不参与执行
- S3K (KTH 2024) 将"时间"引入 RTOS 的分区保护，但没有形式化能力过期语义
- Zylos/Capability Leasing (2026) 提出能力租约概念，但无内核级形式化证明
- **A20OS 是首个在内核层面形式化时态能力并证明安全性保持的系统**

**安全性论证**：
- 定理 3.1（时态单调递减）：$\rho_{eff}$ 随时间单调不增，严格包含原定理 3.2
- 定理 3.2（不可刷新）：持有者无法通过 dup 延长自身能力生命周期
- 定理 3.3（过期原子性）：过期与正在进行的操作无竞争窗口

### 贡献 C2'：混合信任能力边界（Mixed-Trust Capability Boundary）

**声称**：A20OS 首次形式化了"当系统同时包含能力感知（Native ABI）和能力无感知（Linux ABI）子系统时，能力安全性质是否仍然成立"——答案是肯定的，且边界可精确刻画。

**新颖性论证**：
- seL4、Zircon、Capsicum、CHERI 的形式化都假设**整个系统在同一套能力纪律下运行**
- 没有任何现有工作形式化了"混合信任"场景下的能力边界
- A20OS 的双 ABI 设计天然提供了一个"能力感知 vs 能力无感知"的实验环境
- **这是唯一形式化了混合信任能力边界的系统**

**安全性论证**：
- 定理 1.1（直接不可观测性）：Linux ABI 进程无法直接观察 Native ABI handle
- 定理 1.2（降级精确性）：共享资源降级通道只传递数据内容，不泄露 handle 元数据
- 定理 1.3（能力边界定理）：要么完全不可观测，要么构成精确降级通道
- 定理 1.4（性能隔离）：Native ABI 操作不影响 Linux ABI syscall 延迟

### 贡献 C3：类型化通道协议（Typed Channel Protocol）

**声称**：A20OS 在内核 IPC 层强制执行类型化通道约束——a20_channel_type_t 定义了 handle 类型 bitmask、数据大小和 handle 数量上限——并证明通道类型安全（09 定理 2.1）、类型化能力流不变式（09 定理 2.2）。

**新颖性论证**：
- Zircon channel 是无类型字节流，类型检查在用户态 FIDL 解码器完成
- seL4 endpoint 无类型约束
- Session types (Wadler/Honda) 是语言层面的理论，从未在内核 IPC 中实现
- **A20OS 是唯一在内核层实现 session-type 式类型约束的系统**

**安全性论证**：
- 定理 2.1：所有通过类型化通道的消息都满足类型约束（归纳证明）
- 定理 2.2：能力流图上可以做静态的类型传播分析
- 推论 2.2.1：给定能力流图和对象类型，可静态确定哪些进程对可传输该类型

### 贡献 C4：运行时无关的 ABI 演进框架（Runtime-Free ABI Evolution）

**声称**：`{size, version}` 结构体版本化方案不需要 vDSO，可直接应用于任何使用 C 结构体的 syscall 接口。

**新颖性论证**：
- Linux 的 ABI 演进策略是"不改变已有 syscall"——只能新增 syscall 编号（~400 个）
- Zircon 使用 vDSO 进行版本管理——需要特殊运行时
- **A20OS 的方案将版本协商嵌入数据结构本身，消除运行时依赖**

**演进性论证**：定理 10.1（兼容性保持），定理 10.2（编号稳定性）。

### 贡献 C5：委托模式组合安全（Delegation Pattern Composition Safety）

**声称**：提出 5 种规范委托模式（Grant, Attenuate-Grant, Delegate-Return, Pipeline, Fork-Sandbox）并证明其组合安全性。

**新颖性论证**：
- seL4 的 trace 归纳证明了单操作安全性，但没有对操作序列的模式做分类
- Zircon 无委托模式分析
- **A20OS 首次对内核能力委托模式做分类和组合安全证明**

**安全性论证**：
- 引理 4.1-4.5：每种模式保持 $\mathcal{I}$
- 定理 4.1（权限衰减上界）：$\rho_k \subseteq \rho_0$
- 定理 4.2（组合安全性）：不变式保持 + 权限递减 + 无 ambient authority + 可追溯性
- 定理 4.3（P5 完全可回收性）：Fork-Sandbox 模式终止子进程时无悬空引用

---

## 3. 与最相关工作的详细对比

### 3.1 与 Zircon (Fuchsia) 的对比

Zircon 是 A20OS 最直接的前驱工作。两者的核心模型高度相似（handle + rights + channel），但存在关键差异：

| 属性 | Zircon | A20OS | 差异的意义 |
|------|--------|-------|-----------|
| 总 syscall 数 | ~150 | 53 | A20OS 追求最小充分集 |
| 对象类型 | ~25 | 13 | A20OS 合并了可合并的类型 |
| Rights 位数 | ~30 | 14 | A20OS 使用更少的通用权限 |
| 进程创建 | create + start (两步) | spawn (单步) | A20OS 简化常见路径 |
| 事件模型 | zx_port_wait | event_queue + event_wait | 语义等价 |
| Channel | zx_channel_write/read | msg_send/recv | 语义等价 |
| **通道类型** | **无** | **a20_channel_type_t（内核强制）** | **A20OS 首创内核层类型约束** |
| **时态能力** | **无** | **expiry_tick + remaining_ops** | **A20OS 首创内核级过期** |
| ABI 版本管理 | vDSO | 结构体版本化 | A20OS 不需运行时支持 |
| POSIX 兼容 | libfdio (用户态) | liba20posix (用户态) + Linux ABI (内核) | A20OS 有内核级双 ABI |
| **双 ABI 隔离** | **无** | **信息流能力边界定理（9.5）** | **A20OS 有形式化证明** |
| **委托模式分析** | **无** | **5 种模式 + 组合安全证明** | **A20OS 首创模式分类法** |
| 形式化证明 | 无 | 41 个定理/引理 (SOS) | A20OS 提供了形式化 |
| 代码复杂度 | ~200K LOC | 目标 < 5K LOC | A20OS 面向教学 |
| 生产使用 | Google 产品 | 教学内核 | 不同的验证标准 |

**核心差异总结**：A20OS 不再是"简化的 Zircon"。它在四个维度上做出了原创贡献：
1. 类型化通道（内核层 session type，Zircon 无）
2. 时态能力（内核级过期 + 形式化保证，Zircon 无）
3. 混合信任能力边界（信息流 + 能力理论，Zircon 无）
4. 委托模式组合安全（模式分类 + 证明，Zircon 无）

### 3.2 与 seL4 的对比

seL4 是形式化验证的标杆。A20OS 不声称达到 seL4 的验证级别，但对比两者有助于理解设计取舍：

| 属性 | seL4 | A20OS |
|------|------|-------|
| 验证级别 | 完整 Isabelle/HOL 证明 | SOS 规则 + 不变式证明 |
| Capability 模型 | CSpace 图 + CNode | 扁平 handle table |
| IPC | Endpoint + IPC buffer | Channel + message |
| 内存管理 | Untyped + Frame caps | vm_alloc + vm_map |
| 进程创建 | 5+ 步骤 | 1 步 (spawn) |
| 代码量 | ~10K（验证版） | 目标 < 5K |
| 实用性 | 极低（学术/军事） | 教学竞赛 |

**A20OS 的选择**：放弃完整的机器检验证明，换取更简单的模型和更易用的接口。但保留结构化的操作语义，使得核心性质可被数学证明（尽管非机器检验）。

### 3.3 与 Capsicum 的对比

Capsicum 在 FreeBSD/Linux 中为 fd 添加了 capability 语义：

| 属性 | Capsicum | A20OS |
|------|----------|-------|
| 载体 | fd（扩展 POSIX） | handle（新模型） |
| 权限 | fd rights | handle rights |
| 与 POSIX 关系 | 扩展 POSIX | 独立于 POSIX |
| fork 行为 | fd rights 随 fork 继承 | 不使用 fork |
| 形式化 | 无 | SOS + 证明 |

**核心差异**：Capsicum 的设计受限于 fd 的语义包袱（fd 是整数、fork 会复制 fd table、fd 有固定的 0/1/2 含义）。A20OS 从零设计 handle 模型，避免了这些历史约束。

### 3.4 与 jsix 的对比

jsix 是另一个使用 handle-based ABI 的教学/实验性内核：

| 属性 | jsix | A20OS |
|------|------|-------|
| 设计目标 | 教学/实验 | 教学/竞赛 |
| Handle 模型 | handle + rights | handle + rights |
| IPC | channel | channel |
| 进程创建 | spawn | spawn |
| POSIX 兼容 | 不支持 | 双 ABI |
| 形式化 | 无 | SOS + 证明 |

jsix 证明了 handle-based ABI 在教学内核中的可行性。A20OS 在此基础上增加了：(1) 双 ABI 共存，(2) 形式化证明，(3) 结构化版本化框架。

---

## 4. 设计空间中的未被满足的需求

### 4.1 "教学内核可实现的 capability 系统"需求

**问题描述**：现有的 capability-based 系统（seL4, Zircon）要么过于复杂无法在教学内核中实现，要么缺乏形式化安全保证。教学/竞赛内核需要一个**简单到可实现、但安全到可证明**的 capability 模型。

**A20OS 的回答**：
- 13 种类型、14 种权限、53 个 syscall——可在一个学期内实现
- SOS 操作语义 + 不变式证明——可以在论文中完整呈现
- 不追求 seL4 级别的机器检验证明——但在逻辑上等价于更简单的不变式证明

### 4.2 "渐进式 ABI 演进"需求

**问题描述**：现有的"纯"capability 系统（seL4, Zircon）要求完全放弃 POSIX，这意味着无法利用现有的 POSIX 应用生态。而嵌入 capability 的系统（Capsicum）仍然受限于 POSIX 的语义缺陷。

**A20OS 的回答**：双 ABI 共存——Linux ABI 继续支持 POSIX 应用，Native ABI 提供新的 capability 接口。两者在内核中完全隔离，但共享核心实现。

### 4.3 "零运行时 ABI 版本协商"需求

**问题描述**：Linux 的 ABI 演进只能通过新增 syscall 编号，导致接口膨胀。Zircon 的 vDSO 方案需要特殊运行时支持。

**A20OS 的回答**：`{size, version}` 结构体方案将版本协商嵌入到每个数据结构中，不需要任何特殊运行时支持。这种方案在概念上等同于 Protocol Buffers 的向前/向后兼容策略，但应用于 syscall 接口。

---

## 5. 威胁分析（Threats to Validity）

### 5.1 内部有效性威胁

**T1（实现 vs 理论的差距）**：
- 形式化证明基于 SOS 模型，假设内核实现严格遵循语义规则
- 实际内核实现可能存在 bug 导致偏离语义规则
- **缓解措施**：运行时不变式监测（05-evaluation-framework.md §10.3）、模型检验

**T2（简化假设）**：
- 证明不考虑硬件故障、内核 panic、资源耗尽等极端情况
- 证明假设调度器是公平的（定理 4.4 的前提）
- **缓解措施**：明确列出每个定理的前提条件；在结论中指出简化假设

**T3（capability 模型的完整性）**：
- 14 种权限位是否足以覆盖所有合法的权限区分？
- 是否存在需要更细粒度权限的操作？
- **缓解措施**：通过对照 POSIX 的全部操作，论证每种权限位的必要性

### 5.2 外部有效性威胁

**T4（教学内核 vs 生产系统）**：
- 在教学内核上的设计决策可能无法直接推广到生产系统
- 性能数据来自 A20 SoC（ARM Cortex-A7 双核），可能不代表高性能硬件
- **缓解措施**：明确声明目标域为教学/竞赛内核；在对比分析中使用归一化比率而非绝对值

**T5（双 ABI 的实际价值）**：
- 双 ABI 的隔离证明在理论上成立，但在实践中是否真的有用？
- 是否有用户真的会在同一系统上同时使用两套 ABI？
- **缓解措施**：通过实际的 POSIX shim 实现和性能测试论证可行性；使用场景分析（POSIX 应用渐进迁移）

**T6（没有端到端的形式化验证）**：
- 证明停留在操作语义级别，没有机器检验
- 不存在从实现代码到 SOS 规则的精化证明
- **缓解措施**：明确声明这是未来工作；提供模型检验计划（05-evaluation-framework.md §10.1）

### 5.3 构造有效性威胁

**T7（基准测试设计偏差）**：
- 微基准测试可能不代表真实工作负载
- POSIX shim 的性能高度依赖于具体实现的效率
- **缓解措施**：同时提供微基准和宏基准测试；使用标准应用作为宏基准负载

**T8（对比系统的选择偏差）**：
- 选择 Linux 作为主要对比对象可能低估了其他系统（如 FreeBSD, OpenBSD）的优势
- **缓解措施**：在理论分析中覆盖多个系统；在性能对比中主要关注 Linux（因为 A20OS 的 Linux ABI 是最直接的对照）

---

## 6. 定位声明（Positioning Statement）

### 6.1 一句话定位

> A20OS Native ABI 提出了四个原创贡献：内核级时态能力演算、混合信任能力边界形式化、类型化通道协议和委托模式组合安全，在教学内核中实现了 41 个定理/引理覆盖的可证明安全 capability 系统。

### 6.2 面向审稿人的核心论述

**问题**：内核能力系统存在三个未解决的形式化缺口：(1) 能力没有时间受限委托；(2) 混合能力信任下安全性质是否成立未被分析；(3) IPC 通道的类型约束和委托模式的组合安全性缺少形式化。

**Gap**：现有工作存在两极化——seL4 安全但不可实现，Zircon 功能完整但无形式化证明。所有现有能力系统形式化都假设系统整体在同一套能力纪律下运行。内核 IPC 没有类型约束。

**Insight**：
1. 时态能力可在内核层面实现且安全性质可证明（C1'）
2. 混合信任环境下能力边界可精确刻画（C2'）
3. 类型化通道将 session type 下沉到内核（C3）
4. 委托模式可分类且组合安全可证明（C5）

**方法**：
1. 扩展 SOS 为时间参数化操作语义（09 §3.3）
2. 定义能力可观测性和降级精确性（04 §9.4）
3. 在 send/recv 路径中增加类型 bitmask 检查（09 §2.3）
4. 提出 5 种委托模式并证明组合安全性（09 §4.4）

**结果**：53 个 syscall、13 种对象类型的 Native ABI，配合独立的 Linux ABI，41 个定理/引理覆盖安全性。

### 6.3 论文可能的标题候选

1. "Temporal Capabilities and Mixed-Trust Boundaries: A Formally Verified Handle-Based ABI with 41 Theorems"
2. "Beyond Simplified Zircon: Typed Channels, Temporal Capabilities, and Delegation Safety in A20OS"
3. "Proving Capability Safety Under Mixed Trust: The A20OS Native ABI"
4. "From Dependencies to Information Flow: Formal Capability Isolation in a Dual-ABI Kernel"

### 6.4 目标会议/期刊

| 目标 | 适合度 | 理由 |
|------|--------|------|
| OSDI / SOSP | 中等 | 理论贡献有，但缺少大规模实现和评估 |
| EuroSys | 较高 | 教学内核 + 理论分析符合 EuroSys 范围 |
| ASPLOS | 中等 | ABI 设计 + 硬件协同是 ASPLOS 主题 |
| Usenix ATC | 较高 | 实用系统设计 + 评估 |
| ACM TOPS / TOCS | 较高 | 理论 + 系统结合适合期刊 |
| ACM SIGOPS OSR | 高 | 操作系统研究通讯，适合发表初步研究成果 |
| APSYS / APLOS | 高 | 亚太系统会议，教学内核研究 |

### 6.5 投稿策略建议

**首选路径**：
1. 先投 SIGOPS OSR（Research Note），验证核心贡献的反应
2. 扩充评估实验后投 EuroSys 或 ATC
3. 完成机器检验证明后投 OSDI/SOSP（理想但难度极大）

**备选路径**：
1. 投 SysML / MLSys（如果定位为 ML 系统基础设施）
2. 投教育类会议（SIGCSE）如果强调教学价值

---

## 7. 未来工作路线图

### 7.1 短期（1-3 个月）

1. **完成 Native ABI 最小原型**：实现 15 个核心 syscall（参见 DESIGN.md §"最小可实现原型"）
2. **运行不变式测试**：实现 §10.3 的运行时检查器
3. **微基准测试**：完成 §2 的全部微基准测试

### 7.2 中期（3-6 个月）

1. **完整 Native ABI 实现**：53 个 syscall 全部实现
2. **POSIX shim (liba20posix)**：实现基本的 POSIX 兼容层
3. **宏基准测试**：运行完整应用（cp, ls, httpd, make）
4. **TLA+ 模型检验**：验证核心不变式（§10.1）

### 7.3 长期（6-12 个月）

1. **Isabelle/HOL 形式化**：核心 SOS 规则的机器检验
2. **完整论文撰写**：OSDI/SOSP 投稿
3. **多核并发验证**：扩展 SOS 到多核场景
4. **性能优化**：基于评估结果的内核优化

---

## 8. 与 04、05 号文档的关系

本文档（06-contribution-landscape.md）与 04、05 号文档构成完整的研究笔记体系：

```
04-theory-deep-dive.md    （核心理论）
  ├── SOS 操作语义
  ├── 安全/活性/并发性证明
  ├── 信息流能力边界（定理 9.5）
  └── 内存模型、信息流控制
           │
           ├── 贡献声称 ←── 06-contribution-landscape.md（本文档）
           │   ├── C1' 时态能力演算
           │   ├── C2' 混合信任能力边界
           │   ├── C3  类型化通道协议
           │   ├── C4  ABI 演进
           │   └── C5  委托模式组合安全
           │
           ├── 创新方向 ←── 09-innovation-deep-dive.md
           │   ├── 方向 1: 混合信任能力边界（定理 1.1-1.4）
           │   ├── 方向 2: 类型化通道协议（定理 2.1-2.3）
           │   ├── 方向 3: 时态能力（定理 3.1-3.3）
           │   └── 方向 4: 委托模式组合安全（定理 4.1-4.3）
           │
           └── 评估设计 ←── 05-evaluation-framework.md
               ├── 微基准测试
               ├── 宏基准测试
               ├── 安全性验证
               └── 代码复杂度度量
```

三个文档的定理编号交叉引用：
- 06 引用 04 的定理 3.1, 3.2, 3.3, 9.1, 9.2, 9.5, 2.3, 4.1
- 06 引用 09 的定理 1.1-1.4, 2.1-2.3, 3.1-3.3, 4.1-4.3
- 05 的安全性测试（§8）直接验证 04 的不变式 $\mathcal{I}$
- 06 的威胁分析（§5）指出 04、05 和 09 的局限性

---

## 参考文献

1. Klein, G. et al. "Comprehensive Formal Verification of an OS Microkernel." *ACM TOCS*, 2014.
2. The Fuchsia Authors. "Zircon Kernel Concepts." https://fuchsia.dev/docs/concepts/kernel/concepts
3. Watson, R.N.M. et al. "Capsicum: Practical Capabilities for UNIX." *USENIX Security*, 2010.
4. Baumann, A. et al. "A fork() in the road." *HotOS '19*, 2019.
5. Dennis, J.B. & Van Horn, E.C. "Programming Semantics for Multiprogrammed Computations." *Comm. ACM*, 1966.
6. Pike, R. et al. "Plan 9 from Bell Labs." *Computing Systems*, 1995.
7. Shapiro, J.S. "EROS: A Capability System." *PhD Thesis*, Penn, 1999.
8. Saltzer, J.H. & Schroeder, M.D. "The Protection of Information in Computer Systems." *Comm. ACM*, 1975.
9. Lampson, B.W. "Hints for Computer System Design." *SOSP '83*, 1983.
10. Engler, D. et al. "Exokernel: An Operating System Architecture for Application-Level Resource Management." *SOSP '95*, 1995.
11. Madhavapeddy, A. et al. "Unikernels: Library Operating Systems for the Cloud." *ASPLOS '13*, 2013.
12. Litton, J. et al. "Lightweight Contexts: An OS Abstraction for Safety and Performance." *OSDI '16*, 2016.
13. Mao, Y. et al. "NrOS: Effective Replication for Shared-Memory Multicore Systems." *OSDI '20*, 2020.
14. Zell, D. et al. "Demikernel: A Datacenter Kernel for I/O." *SOSP '21*, 2021.
15. Woodruff, J. et al. "The CHERI Capability Model: Revisiting RISC in an Age of Risk." *ISCA '14*, 2014.
16. Miller, M.S. et al. "Capability-based Financial Instruments." *Financial Cryptography*, 2000.
17. Levy, H.M. "Capability-Based Computer Systems." *Digital Press*, 1984.
18. Wulf, W. et al. "HYDRA: The Kernel of a Multiprocessing Operating System." *Comm. ACM*, 1981.
19. Plotkin, G.D. "A Structural Approach to Operational Semantics." *University of Aarhus*, 1981.
20. Goguen, J.A. & Meseguer, J. "Security Policies and Security Models." *IEEE S&P*, 1982.
21. Skorstengaard, L. et al. "Reasoning about a Stack Machine: Efficient and Verifiable Revocation." *POPL*, 2021.
22. Georges, A.L. et al. "Directed Capabilities for Spatial and Temporal Stack Safety." *OOPSLA*, 2022.
23. Nucleus Project. "Capability Lattice with Monotonicity Proof." *coproduct-opensource/nucleus*, 2026.
24. S3K. "Minimal Partitioning Kernel with Time Protection." *EuroS&PW*, 2024.
25. Kamp, J. et al. "Borrowed Capabilities for CHERI Revocation." *arXiv*, 2024.
26. Honda, K. et al. "A Theoretical Framework for Communication Patterns in Distributed Systems." *Theoretical Computer Science*, 1998. (Session types)
27. Wadler, P. "Propositions as Session Types." *CSL-LICS*, 2014.
