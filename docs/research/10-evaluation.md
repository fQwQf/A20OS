# 评估：计划与结果记录

> **本文把"评估"当作论文的最硬证据来设计**，取代旧 05-evaluation-framework.md 的"纯框架"定位。核心变化：① 所有实验区分为 **[计划]**（尚未执行）与 **[实测]**（有数据）；② 新增与核心贡献（能力信封、预算能力、供应链场景）直接挂钩的评估；③ 明确每个实验结果在论文中对应哪条主张。
>
> **现状声明**：截至本文写作，Native ABI 侧**没有任何已实测数据**。旧文档中的所有数字都是待验证方案。以下所有 **[计划]** 条目都不构成当前性能结论。工程侧（Linux ABI）的实测数据见 `docs/roadmap/perf-overhaul.md`。

---

## 1. 评估主张-实验映射

| 论文主张 | 实验 | 状态 |
|---------|------|------|
| 预算能力实现成本低 | E1: 预算字段/判定开销 | [计划] |
| **信封可部署且提供真实安全价值** | **E2: 包安装/插件场景 + E7 攻击套件 + E10 对照** | **[计划]** |
| 信封开销可接受 | E11: 信封调解 syscall 延迟/应用级 | [计划] |
| typed channel 内核强制开销低 | E3: typed vs untyped channel | [计划] |
| 能力接口与 Linux 可比 | E4: 微基准 syscall 延迟 | [计划] |
| 双 ABI 共存性能隔离 | E5: Linux ABI 延迟不受 Native 干扰 | [计划] |
| 渐进部署可行 | E6: 真实 Linux 应用 + Native 服务并存 | [计划] |
| 核心安全不变量成立 | E8: 运行时不变式监测 + 攻击套件 | [计划] |
| 工程可实现性 | E9: 复杂度度量 vs 对照 | [计划] |
| 与 Linux 的工程差距（非 Native 主张） | 见 roadmap 审计 | [实测，工程侧] |

---

## 2. 实验方法论（沿用旧 05 的合理部分）

- **统计**：n ≥ 1000，报告均值/中位数/P99/95% CI；A/B 用 Welch t-test，p<0.05 且 d>0.5 才声称显著。
- **环境**：QEMU virt（riscv64 主基线，aarch64/x86_64/loongarch64 交叉验证）+ 真机（VisionFive 2 / LS2K1000）按实际板卡记录。TCG 与真机结论分开标注。
- **冷/热**：报告测量类型；热路径取稳态（warm）。
- **对照**：Linux 6.x 同 QEMU 同配置；Zircon 不做直接基准（无同条件环境），以 Linux 对照为主，Zircon 开销引官方文档。
- **证据边界**：每次运行记录 commit、镜像哈希、QEMU 参数、退出状态（沿用 roadmap 审计的协议）。

---

## 3. E1 [计划] 预算能力开销量化

**目的**：回答"预算能力到底贵不贵"，直接支撑"O(1) 失效判定"的卖点。

- **E1a 每 handle 空间**：`sizeof(handle_entry)` 变化（+16 bytes）；大 handle 表下 cache 影响。对照：无预算字段版本。
- **E1b 判定开销**：`handle_lookup` 中 $\rho_{eff}$（预算合取）判定 vs 纯 $\rho$ 判定（位运算分支数）。
- **E1c sweeper 成本**：deadline-driven sweep 的每周期 CPU 时间 vs handle 表大小（H=100/1000/10000/65536）；不同到期率。
- **E1d op-count 递减**：每操作 `remaining_ops--` 的分支预测代价（高命中率路径）。

**报告**：一张表 + 一张扫掠图。预期结论（待验证）：判定 $O(1)$ 且 <10ns；sweeper 随 H 线性但常数小。

---

## 4. E2 [计划] 包安装 / 插件场景端到端演示 ★论文杀手级评估

**目的**：证明能力信封（05）能真实约束不可信组件，且**零源码改动**。

**场景 A：包安装脚本信封**。用真实 npm/pip/cargo 包（含 postinstall/build 脚本）直接放进信封：
- policy：`allowed_types = {FILE, SOCKET, PIPE}`；`FILE={R,W}`、`SOCKET={R,W}`（禁 LISTEN）；`time=300s, ops=100k, data=100MB`；`propagation = {PIPE, FILE:RO}`。
- 度量：合法安装流程功能完整性（包能装上）；在约束下脚本行为。

**场景 B：插件过期委托**。服务器把第三方插件以 `time=30s, ops=1000, types={SOCKET}` 放进信封，演示请求处理生命周期后权限自动归零。

**攻击注入**（每场景）：被攻破的组件尝试
(a) 打开任意文件（类型拒绝/无 handle）；
(b) listen 端口（rights 上限拒绝）；
(c) 经 IPC 传出 task/shm 类资源（propagation 拒绝）；
(d) 超时/超次数后继续操作（预算归零）；
(e) dup/自签延长预算（不可刷新拒绝）。

**对照**（同一攻击集）：
- 无信封（攻击全部成功——证明价值）；
- 仅 seccomp-bpf 白名单（证明 seccomp 不可表达类型/预算/时间）；
- 仅 Landlock 路径规则（证明无对象/预算）；
- 用户态沙箱（Firejail/bubblewrap，若可搭建）——证明对象级预算差异。

**交付**：可复现 smoke + 攻击脚本；论文 Figure：攻击成功率 vs 防御配置。

---

## 5. E3 [计划] Typed vs Untyped Channel

**目的**：量化内核类型检查成本。

- 消息大小 1B-64KB，0/1/8 handles，typed（send/recv bitmask 检查）vs untyped。
- 对比 Linux pipe / UNIX socket / io_uring（作为生态对照）。

---

## 5.5 E11 [计划] 信封调解器开销 ★论文成败关键

**目的**：量化"全获取咽喉"的成本——若调解使资源 syscall 明显变慢，"部署选项"吸引力下降。

- **E11a 微基准**：信封开启 vs 关闭时，open/socket/connect/mmap/pipe/fork/exec 各 syscall 延迟差（调解 = 类型查表 + rights 映射 + 影子 handle 分配 + 预算计数）。
- **E11b 应用级**：一个真实工作负载（如包安装、HTTP 请求处理、文件复制）在信封内外的端到端时间差。
- **E11c 预算耗散**：大预算（100k ops）下计数开销 vs 小预算；sweeper 在大量信封下的 CPU 时间。

**报告**：绝对延迟（ns）+ 相对开销（%）。**如实报告**——若调解开销 >20%，论文必须讨论这是能力纪律的固有成本，还是实现可优化。

---

## 6. E4 [计划] 微基准：Native syscall 延迟

复用旧 05 §2-§7 的矩阵（handle_dup/close/query、path_open、vm_alloc、event_wait、msg_send/recv、task_spawn vs fork+exec、clock_get），逐一对照 Linux 等价操作。**注意**：不要在评估里声称"spawn 一定快于 fork+exec"——结果说话。

---

## 7. E5 [计划] 双 ABI 性能隔离

**目的**：验证 05 定理（跨 ABI 性能隔离）。

- Linux ABI 进程跑 syscall 延迟基准时，另一个核跑 Native ABI 高负载（channel ping-pong + handle churn），观测 Linux 延迟的 P99 漂移。
- 反向同理。
- 对照：Linux-only 双进程同样竞争下的漂移（隔离"ABI 层干扰"与"资源竞争"）。

---

## 8. E6 [计划] 渐进部署演示 + Native ABI 价值场景

**目的**：支撑"不抛弃 Linux 生态"的实用主张，**并证明 Native ABI 的存在理由**（信封做不到、Native 才能做的场景）。

- **E6a 共存**：同一系统同时运行：若干真实静态 musl Linux 程序（如 git/vim 级，现已有）+ 一个 Native ABI 能力服务（如 key-value 服务，通过 typed channel 对外）。
  - 展示：Linux 程序无感；Native 服务持有受限能力；两者经 VFS 共享文件时的边界行为。
- **E6b Native 专属价值场景**（回应"Native ABI 有什么用"）：跨信任域的多方委托——
  - 服务 S（Native，可信）持有细粒度 handle，通过 typed channel 向各不可信租户分配**类型受限 + 时态受限**的子预算；
  - 演示信封**无法表达**的：多方共享同一资源的独立预算、跨域 typed 委托、审计链（谁在何时把什么权限给谁）。
  - 对照组：仅信封（无 Native）配置下这些语义不可表达 → 证明 Native ABI 提供信封之上不可达的能力。

---

## 9. E7 [计划] 攻击与鲁棒性测试

复用旧 A1-A8（权限提升/混淆代理）+ 预算专用攻击（T 系列）+ **信封逃逸攻击（EN 系列，11 §4.6）**：
- T1: dup 刷新 expiry（应拒绝）
- T2: 过期后 re-install 读（应拒绝）
- T3: 并发 sweeper vs 操作竞态（应无既过期又成功的窗口——E8 运行时检查）
- T4: typed channel 绕过（改类型位、发未声明类型）（应 TYPE_MISMATCH）
- T5: 跨 ABI 注入（Linux 进程写共享文件尝试影响 Native 通道类型——应无效）
- **EN1-EN6**：信封逃逸（见 11 §4.6）——含未调解 syscall 枚举、procfs/fd 走私、fork/exec 身份逃逸。

---

## 10. E8 [计划] 运行时不变式监测

- 实现 06 §10.3 风格的 `CONFIG_INVARIANT_CHECK`：每次 syscall 后验证 I1-I5 + 预算不变量（$\rho_{eff}$ 单调、refcount 平衡）。
- 跑 E2/E7 全套，报告不变式违反数（应 0）。
- 监测器自身开销（<5% 目标）。

---

## 11. E9 [计划] 复杂度度量

Native ABI 实现 LOC/圈复杂度 vs seL4/Zircon（引用既有数据），支撑"能力核心可审计"（方法层价值）。注意：这是**支持性**证据，不是论文贡献。

---

## 12. 结果记录（[实测] 填充区）

> 所有实测结果填入此节并标注：commit、日期、环境、QEMU 参数、冷/热、n。**任何进入论文的数字都必须来自这里。**

| 实验 | 结果摘要 | 环境/commit | 日期 |
|------|---------|------------|------|
| （空） | | | |

---

## 13. 验收标准（每项主张必须过这一关）

| 主张 | 通过标准 |
|------|---------|
| 预算能力可实现且便宜 | E1 判定 <10ns（或给出诚实上限）、sweeper 线性可接受 |
| **信封可部署且提供真实安全价值** | **E2 攻击全部失败且对照配置失败；合法安装功能完整；零源码改动** |
| **信封开销可接受** | **E11 如实报告；若 >20% 须讨论与优化** |
| typed channel 内核强制 | E3 开销可量化；E7 绕过全部拒绝 |
| 双 ABI 可用 | E4 与 Linux 同数量级；E6 演示通过 |
| 核心性质成立 | E8 不变式 0 违反；E7 攻击 0 成功 |

**最重要的验收**：任何实验若结果不利（如信封开销过大、逃逸成功、包脚本功能被破坏），**如实报告并调整论文主张**，而不是改标准。这是主会级评估的底线。
