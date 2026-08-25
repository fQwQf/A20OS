# 系统文献综述（Systematic Literature Review）——工作文档

> **本文是 A20OS 研究的前提性文档**。旧版研究笔记在每处新颖性声明后都写"据当前调研……尚需系统文献复核"——这个"复核"就是本文。目标：把三个贡献层面（预算能力 03、类型化通道 04、能力信封 05）逐一放到文献谱系里，给出**已确立** / **候选差异** / **被覆盖** 的明确分级，消灭一切含糊的"可能是首次"。 **诚实声明**：本工作文档基于作者掌握的文献知识整理，**尚未完成逐篇全文核对与穷尽式检索**（本次写作时外网检索不可用）。投稿前必须按 §6 的检索协议补全。条目凡未亲自核对的都标 `[待核]`。

---

## 1. 综述框架

按论文的三个贡献层面组织：

1. **预算能力（03）**：把"授权"从二元权限推广为多维预算（时间/次数/类型/传播），权威资源化。核心新颖性：内核强制 + 多维预算格 + 单调衰减/不可刷新形式化。
2. **类型化通道（04）**：能力传播的类型约束（预算的传播维度）。
3. **能力信封（05）**：给未修改二进制"包上"能力预算——**本层面是论文主角，也是新颖性审查的主战场**（见 §5）。

每个支柱回答四个问题：**谁做过？做到什么程度？与我们差在哪？我们还缺什么证据？**

---

## 2. 支柱一：预算能力（授权的时间/数量维度）

### 2.1 能力系统谱系（无预算基线）

| 系统 | 年份 | 授权模型 | 是否有时间/次数维度 | 验证 |
|------|------|---------|-------------------|------|
| Dennis & Van Horn | 1966 | 原始 capability | 无 | 概念 |
| Hydra | 1981 | 类型化 capability + 保护 | 无 | 实现 |
| KeyKOS | 1992 | 持久 capability | 无 | 实现 |
| EROS | 1999 | 持久 capability + 撤销 | 无（显式撤销） | 实现 |
| seL4 | 2009/2014 | CNode capability 树 | 无 | Isabelle/HOL |
| Capsicum | 2010 | fd-as-capability | 无 | 实现 |
| CHERI | 2014- | 硬件 capability tags | 无（借用/回收见 2.2） | 硬件+逻辑 |
| Zircon | 2016- | handle + rights | 无 | 实现 |

**结论**：主流 capability 系统把授权视为"持久直到显式撤销"。这是 A20OS 论证的基线。

### 2.2 时间相关的邻近工作（必须逐篇厘清）

这是**优先性风险最高的区域**，投稿前必须完成以下条目的全文核对：

| 工作 | 内容 | 与我们差在哪 | 状态 |
|------|------|------------|------|
| Gray & Cheriton, "Leases", SOSP'89 | 分布式缓存一致性中的租约（时间有限的授权） | 分布式系统一致性，非内核 capability；但"租约=限时授权"的概念早于此 | 需写入谱系 |
| SPKI/SDSI (Ellison et al., 1996-) | 授权证书带有效期 | **用户态证书，非内核强制**；但"capability 带有效期"概念已被 SPKI 表达 | 需写入谱系 |
| OAuth2 / JWT / X.509 | 临时凭证 | 用户态协议，内核不参与执行 | 已确立为对照 |
| IBM/Oak 系统 "capability leasing" | 对象系统限时引用 | `[待核]`：需查证是否内核强制 + 是否有形式化 | **优先性核对点** |
| S3K, EuroS&PW 2024 | RTOS 分区的时间保护 | 时间是分区隔离维度，不是授权衰减 | 已区分 |
| Georges et al., "Directed Capabilities", OOPSLA'22 | CHERI 时态内存安全 | 解决 UAF/内存生命周期，不是授权过期 | 已区分 |
| Skorstengaard et al., POPL'21 ×2 | capability 撤销（uninitialized caps） | 撤销机制本身，不引入时间维度 | 已区分 |
| Kamp et al., "Borrowed Capabilities", 2024 | CHERI 借用/租约式 capability 生命周期 | 借用周期用于内存安全；需核对是否含授权过期 | **优先性核对点** |
| CHERIvoke, EuroS&P'23 | 指针撤销 | 撤销机制 | 已区分 |
| Zylos / "Capability Leasing" (2026) | `[待核]` | 原文档 06 引用；需获取原文确认其是否覆盖内核级时间衰减 | **优先性核对点** |

### 2.3 支柱一结论

- **已确立**：主流能力系统无授权时间维度；临时凭证（OAuth/JWT/X.509/SPKI）存在于用户态。
- **候选差异**：内核强制的、带形式化保证（单调衰减/不可刷新/过期原子）的时态能力。
- **被覆盖风险**：capability leasing（IBM/Oak）、Borrowed Capabilities（2024）若被证明包含了"内核级时间衰减 + 保证"，则我们的贡献收窄为"形式化 + 与类型化通道/委托链的组合 + 供应链应用"。
- **证据缺口**：这些近邻工作的精确语义，**投稿前必须完成原文核对**。

---

## 3. 支柱二：能力传播的类型维度（类型化通道）

### 3.1 谱系

| 工作 | 类型约束位置 | 形式化 | 与我们的差异 |
|------|------------|--------|------------|
| Session types (Honda 1998; Wadler 2014) | 语言层 | 类型论 | 不落地为内核机制 |
| FIDL (Fuchsia) | 用户态 IDL/解码器 | 无 | 类型检查在用户态，内核不阻止错误类型 handle 传输 |
| WASI component model | 用户态 | 有（接口类型） | 用户态 |
| seL4 endpoint | 无 | — | 无类型约束 |
| Zircon channel | 无 | — | 无类型约束 |
| **A20OS typed channel** | **内核 send/recv 路径** | **SOS 纸笔** | 内核强制 + 类型化能力流静态可判 |

### 3.2 需核对的历史工作

- **L4 家族的 typed IPC 提案**：L4 系（Pistachio、Fiasco、L4Re）是否有过"类型化 IPC"设计？`[待核]`——若有，需区分"消息结构类型"与"能力传播的类型约束"。
- **微内核与安全语言结合**：OCaml/ML 系内核（如 CamlOS/ Mirage 前身）的 typed IPC `[待核]`。
- **"type-safe IPC" / "typestate-aware OS"**：`[待核]`——session-type 下沉到 OS 层是否有直接先例（例如面向嵌入式组件的 TOR 系统、seL4 之上的类型化组件框架）。

### 3.3 支柱二结论

- **候选差异**：内核强制 + 能力流静态可判定 + confused-deputy 静态消除（04 定理 8.3）。
- **被覆盖风险**：若 L4/组件框架已有"内核层类型约束 IPC"，则贡献收窄为"与 capability 系统结合 + 形式化 + 静态验证规则"。
- **证据缺口**：L4 typed IPC、嵌入式组件系统（如 TOR/Genode typed IPC）的原始材料核对。

---

## 4. 支柱三：能力信封（给未修改二进制附加能力纪律）★新颖性主战场

**本文是论文主角，也是被审稿人攻击最猛的区域**。三组邻近工作必须逐篇厘清：(a) 在既有系统上"附加"安全纪律的手段；(b) 对不可信插件/包脚本的沙箱；(c) 能力纪律的渐进采用。

### 4.1 组 (a)：给既有软件附加安全纪律

| 工作 | 机制 | 对象模型 | 时间/次数预算 | 形式化 | 源码改动 | 与信封差异 |
|------|------|---------|-------------|--------|---------|-----------|
| Capsicum | fd 加 rights / capability mode | fd 级 | 无 | 无 | 需（libc/应用） | fd 语义包袱、无预算、无时间维度 |
| Landlock | path 规则 | 路径级 | 无 | 无 | 无 | 无对象能力、无预算 |
| seccomp-bpf | syscall 号过滤 | syscall 级 | 无 | 无 | 无 | 按号过滤可被白名单绕过、无对象/预算 |
| AppArmor / SELinux | 策略叠加 | 进程/文件级 | 无 | 无 | 无 | 粗粒度、无时间/次数维度 |
| Firejail / bubblewrap | 用户态沙箱（mount/ns/seccomp 组合） | 进程级 | 无 | 无 | 无 | 无对象能力预算；策略组合脆弱 |
| gVisor | 用户态内核（syscall 拦截） | 进程级 | 无 | 无 | 无 | 用户态开销大；无能力对象模型 |
| starnix (Android) | Linux 人格层于 Zircon | 进程级 | 无 | 无 | 无 | **最接近的架构**；但无预算/时态，且不主张安全边界形式化 |
| **A20OS 信封** | **内核调解器 + 影子 handle** | **对象能力级** | **是（时间/次数/类型/传播）** | **预算格 + 逃逸不可行（TLA+/Lean 计划）** | **无** | 唯一同时具备全部维度 |

### 4.2 组 (b)：不可信插件 / 包安装脚本的沙箱

**真实问题谱系**：
- 包管理器安装/构建脚本：npm postinstall、pip build isolation、cargo build scripts、Gradle/MAVEN plugin——不可信代码以用户全部权限运行。
- 插件系统：浏览器扩展（Chrome 已内置权限模型）、编辑器/IDE 插件、CI runners、数据库扩展。

**现有缓解**（全部**网络/进程级**，无对象能力预算）：
- npm `--ignore-scripts`、pip 网络隔离（build isolation）——全局开关或网络层，无资源预算。
- 沙箱运行包脚本：`[待核]`（社区工具如 `pnpm approve-builds`、`corepack`、`proot` 类）——用户态、无内核保证、无时间/次数。
- 学术工作：包管理安全（如 "Sok: software supply chain"）、`in-toto`/SLSA（完整性 attestation，非运行时隔离）、`[待核]` OS 级包脚本沙箱研究。

**A20OS 信封的回答**：包安装脚本放进信封（05 §4 场景），内核强制 类型×权限×时间×次数，零改动。**这是论文的应用落点，也是"为什么非你不可"的实证。**

### 4.3 组 (c)：能力纪律的渐进采用 / 混合信任

| 工作 | 共存方式 | 形式化边界 |
|------|---------|-----------|
| seL4 + L4Linux/Copter | Linux 兼容层在 seL4 上 | `[待核]`：隔离保证的程度需核对 |
| SecureCells (S&P'24) / BULKHEAD (NDSS'25) | VMA/域级细粒度隔离 | 有（但非能力预算） |
| 混合信任边界（旧 05） | 能力感知/无感知子系统共存 | 信息流能力边界（现并入 05 §5） |

### 4.4 支柱三结论

- **候选差异（信封）**：**内核级对象能力预算 + 零源码改动 + 时间/次数/类型/传播四维 + 形式化逃逸不可行**——现有沙箱（seccomp/Landlock/Firejail/gVisor）无对象能力模型，Capsicum 需改动，无时间维度。
- **被覆盖风险**：① starnix 若被证明具备"给 Zircon 进程附加预算"能力，架构独特性受冲击（但 starnix 面向 Fuchsia 生态、无预算/时态）；② 若存在被忽视的"OS 级包脚本沙箱 + 预算"工作（`[待核]`），应用落点需重定位。
- **证据缺口**：组 (b) 的社区/学术工具盘点；starnix 的隔离保证细节；L4Linux/Copter 的隔离证明程度。

### 4.5 必须正面回答的审稿人问题（预备反驳）

1. "为何不做在 Linux 上？" → Linux 无对象能力模型，需 LSM 级大规模 hook 且无形式化；A20OS 双 ABI 内核天然是全局咽喉。
2. "信封 = 换马甲 seccomp？" → seccomp 按 syscall 号过滤（可被白名单绕过、无对象/预算）；信封按对象能力 + 预算（无法通过获取方式绕过）。
3. "时间预算 = OAuth expiry？" → OAuth 用户态、进程可自续、无内核强制、无对象模型；信封内核强制、不可刷新、与类型/次数/传播同构。

### 4.6 相关形式化基础

- Noninterference: Goguen & Meseguer (1982)、Bell-LaPadula (1973)——05 §5 的 $\mathcal{L}$-noninterference 与降级精确性建模直接沿用。
- **Compositional security**：关于"安全机制组合是否保持已有保证"的形式化（如 "composability of security policies"）`[待核]`——08 与 05 的"增量部署单调保证"（05 §6 定理 8.2）需要与这类工作对齐。
- **Confused deputy**：Norman Hardy (1988) 原始论文必须写入。
- **沙箱逃逸（sandbox escape）文献**：对"全获取咽喉"主张的反向压力测试——检索已知沙箱逃逸技术（ret2dir、via procfs、/proc 走私、fd 继承等）并逐条验证信封防线。

---

## 5. 验证与工程基线（论文方法层的相关谱系）

| 工作 | 验证方法 | 对我们 08-verification 的启示 |
|------|---------|------------------------------|
| seL4 (Isabelle/HOL) | 完整机器检验 | 我们无力复制全套；取其中"核心原语形式化"思路 |
| CertiKOS | 分层精化（compcert 风格） | 我们的 07 精化映射与之精神一致，但层级更浅 |
| Hyperkernel/Biscuit (OSDI'17) | 符号执行 + 精化 + 少量 syscall | **最贴近的可行路径**：TLA+/符号执行验证 handle table 核心 |
| FSCQ | Coq 文件系统 crash 一致性 | 可借鉴 crash-consistency 证明思路（但我们以能力安全为主） |
| TLA+/TLC (Lamport) | 模型检验 | 我们的首选工具：handle table + sweeper + typed channel + **信封调解器（08 模块 4）** |

**结论**：机器检验路径（08）选择 TLA+ 模型检验 + 关键性质 Lean/Isabelle 形式化，参考 Hyperkernel 的"裁剪到核心、可推按钮"方法论；信封的"咽喉完备性 + 逃逸不可行"是验证重点。

---

## 6. 检索协议（投稿前必须完成）

以下步骤是 **paper blocker**，不是可选优化：

1. **数据库**：ACM DL、IEEE Xplore、dblp、Google Scholar、arXiv。
2. **检索式**：
   - 预算能力：`expiring capabilities` / `capability lease` / `temporal capability` / `time-limited authority` / `lease-based access control` / `budgeted authorization` / `resource-bounded capability`
   - 类型通道：`typed IPC kernel` / `session types operating system` / `type-safe interprocess communication` / `typed channel microkernel`
   - **信封/沙箱（主战场）**：`sandbox legacy binaries without source modification` / `kernel-level capability sandbox` / `seccomp alternative object model` / `starnix isolation guarantee` / `capsicum capability mode retrofitting` / `package install script sandbox` / `npm postinstall sandbox` / `supply chain runtime isolation` / `plugin sandboxing kernel` / `sandbox escape techniques`
   - 组合：`supply chain OS isolation` / `time-limited sandbox` / `capability composition security`
3. **近邻优先**：锁定 2.2 表、3.2 表、4.1/4.2/4.3 表中的所有 `[待核]` 条目，逐篇获取原文、记录其确切语义、更新差异化矩阵。**其中 4.1（starnix/gVisor/Capsicum）与 4.2（包脚本沙箱）是主会拒稿风险的最大来源。**
4. **盲审预演**：写完差异化矩阵后，把自己想象为 OSDI 审稿人，为每一项贡献写一句最尖锐的拒绝理由，再写对应的反驳。
5. **沙箱逃逸反向测试**：列出已知沙箱逃逸技术清单，逐条论证信封防线的覆盖/不覆盖（不覆盖的要写进 threat model）。
6. **持续维护**：本文件是活文档；新增引用必须放进对应表格并标注核对日期。

---

## 7. 已有引用书目（待补全）

按 §6 检索后并入此节。当前零散的已有引用分散在旧文档（06/09 的参考文献），重组时统一收编到本文 §7 并去重。

**基线能力系统**：Dennis & Van Horn (1966)；Fabry (1974)；Wulf et al. Hydra (1981)；Bomberger KeyKOS (1992)；Shapiro EROS (1999)；Klein et al. seL4 (SOSP'09, TOCS'14)；Watson et al. Capsicum (USENIX Sec'10)；Woodruff et al. CHERI (ISCA'14)；Miller, "Capability Myths Demolished" (2003)；Saltzer & Schroeder (1975)；Lampson (1983)；Levy, "Capability-Based Computer Systems" (1984)。

**时间/租约**：Gray & Cheriton, "Leases" (SOSP'89)；Ellison et al., SPKI (RFC 2693, 1999)；OAuth 2.0 (RFC 6749)；JWT (RFC 7519)；Georges et al. (OOPSLA'22)；Skorstengaard et al. (POPL'21 ×2)；Kamp et al. "Borrowed Capabilities" (2024)；CHERIvoke (EuroS&P'23)；S3K (EuroS&PW'24)。

**类型化通信**：Honda et al. (TCS'98)；Wadler, "Propositions as Session Types" (2014)；FIDL (Fuchsia docs)；WASI component model；seL4 manual。

**隔离与信息流**：Bell & LaPadula (1973)；Goguen & Meseguer (1982)；Hardy, "The Confused Deputy" (1988)；Capsicum；Landlock/seccomp；gVisor；starnix。

**验证**：Klein et al. (2009/2014)；CertiKOS (Gu et al., SOSP'15)；Hyperkernel (Nelson et al., OSDI'17)；FSCQ (Chen et al., SOSP'15)；Lamport, TLA+ (2002)。
