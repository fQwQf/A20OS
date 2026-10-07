# 集群子系统实现：并行轨道 prompt 系统（v2）

> **实现状态（2026-10-07）**：本文件整体是**实施计划**（prompt 调度系统），不是完成记录；下文各 prompt 中"前提：XX 已验收"等字样请按计划语境阅读，实际进度以本横幅为准。
> - **已完成**：W0（ABI 冻结）、WA1（内核 loopback 骨架，见 `kernel/cluster/`）、WB1（参考编解码与金样向量，见 `tools/cluster-ref/`）、WB2（双 QEMU 互联，见 `tools/cluster-net-up.sh` 与 [02-udp-demo.md](02-udp-demo.md)）、WC1（UART 传输与 MCU 叶子，见 `kernel/cluster/uart.c`）。
> - **未启动**：WA2（UDP 传输 + 跨机 channel_call）、WA3（可靠性）、WD1/WD2（clusterd/jobd，规范见 [05-userspace.md](05-userspace.md)）、WE1/WE2（生产硬化与终验，标准见 [06-production.md](06-production.md)）。

## 怎么用（给人看的调度说明）

**核心原则：规范文档是真正实现者，prompt 只是点火器。** 每条 prompt 都先让执行者读 `docs/cluster/` 下的规范；执行中发现规范与代码冲突时，停下来报告，由人仲裁后回写规范——不允许 agent 自行改设计。

### 依赖图与并行规则

```
W0（串行门禁，必须第一个且单独完成）
 ├─ WA1 ──→ WA2 ──→ WA3 ─┐        （WA 轨道内部串行）
 │    └────→ WC1 ────────┤        （WC1 与 WA2/WA3 并行）
 ├─ WB1 ────────────────┤         （WB1 是 WA2 的对拍依据，必须先于 WA2 收尾）
 ├─ WB2 ────────────────┤         （WB2 是 WA2 的验收环境，必须先于 WA2 联调段）
 │                       ▼
 │              WD1 ──→ WD2       （WD1 需 WA2 验收后启动）
 │                       ▼
 │              WE1 ──→ WE2       （全部功能完成后，串行）
```

- **可并行窗口**：W0 完成后，WA1 / WB1 / WB2 三条同时跑（不同会话）。WA1 验收后加跑 WC1。
- **文件所有权**（防并行互踩，违反者返工）：WA 拥有 `kernel/cluster/`（除 `uart.c`）与 `kernel/abi/native/sys_native_cluster.c`；WB 拥有 `tools/cluster-ref/`、`instances/*cluster*`、`tools/` 演示脚本；WC 拥有 `kernel/cluster/uart.c` 与 `kernel/mcu/`；WD 拥有 `user/svc/clusterd*.c`、`user/svc/jobd*.c`。跨所有权的改动必须先在对应规范文档登记再动手。
- 每条轨道建议在独立 git worktree/分支上工作，按依赖序合并：W0 → WB1/WB2 → WA1 → WA2/WC1 → WA3 → WD1 → WD2 → WE1 → WE2。
- 每条 prompt 都要求执行者末尾输出**验收报告**（通过的测试编号 + 偏差清单 + 性能数值）。人审报告后再点火下一条。

### 通用约束（已写进每条 prompt，勿删）

1. 实现前必读指定规范文档；规范 > prompt 记忆 > 猜测。
2. 每个阶段必须跑通指定 `make check-*` 门禁；红了不算完成。
3. 不改与任务无关的文件；发现既有 bug 另行报告，不顺手修。
4. 实现期对规范的任何偏差：回写规范文档 + 刷新核对日期 + 列入验收报告。

---

## W0 — ABI 冻结（串行门禁）

```
你在 A20OS 仓库根目录工作（自研混合内核 OS；Native ABI 是其能力/句柄导向的 syscall 接口，定义在 kernel/include/abi/native/，实现在 kernel/abi/native/）。

任务：为集群子系统做 ABI 冻结——只新增类型、errno、rights 位、syscall 号与 stub 实现，零行为变化。

第一步（写任何代码前）按序精读：
1. docs/cluster/01-abi.md（你的唯一实现依据，逐节照做）
2. kernel/include/abi/native/types.h（参数结构体风格，参考 a20_channel_create_args_t 与 a20_channel_call_args_t）
3. kernel/abi/native/errno.h、rights.h、syscall_table.def、kernel/include/abi/native/resource.h
4. docs/cluster/00-design.md 的"集群 ABI"一节（理解设计意图，但字段级细节以 01-abi.md 为准）

然后按 01-abi.md 落地：
1. types.h：a20_node_id_t 与六个 syscall 的 args 结构体（每个含 struct_size 首字段，保留字段置零要求写进注释）。
2. errno.h：四个新集群 errno + 视需要新增 RESOURCE_LIMIT/ALREADY_EXISTS/NOT_FOUND（若已有等价码，复用并在 01-abi.md 登记映射）。
3. rights.h：A20_RIGHT_CLUSTER_ADMIN。
4. syscall_nr.h：0x0520 起六个号；syscall_table.def 登记；新建 kernel/abi/native/sys_native_cluster.c，六个 stub 全部返回 A20_ERR_CLUSTER_UNSUPPORTED（stub 也要先做 struct_size 校验再返回错误，给后面立规矩）。
5. resource.h：按 01-abi 限额表加常量（MCU/DEFAULT/ABSOLUTE 三组）。

验收：
- make check-riscv64-bringup && make check-aarch64-bringup 通过（其他架构有余力全跑，目标清单见 make help）。
- 写一个最小验证：调六个 stub 均返回期望错误码（测试放仓库现有 native ABI 测试的惯例位置，先找先例再决定）。
- git diff 不含对 kernel/ipc/、kernel/net/ 的任何改动。

完成后输出验收报告：改动文件清单、每个新号的数值、门禁结果、对 01-abi.md 的任何偏差（应为零；有则必须已回写文档）。
```

## WA1 — kernel/cluster 骨架 + loopback（WA 轨道第 1 棒）

```
你在 A20OS 仓库根目录工作。前提：W0 已验收——六个 cluster_* stub 在位（当前返回 CLUSTER_UNSUPPORTED）。

第一步按序精读：
1. docs/cluster/03-kernel-impl.md（你的实施手册，含文件划分、集成方案、锁层级、实现顺序——§5 的顺序必须严格遵守）
2. docs/cluster/01-abi.md（syscall 语义）、docs/cluster/02-wire-protocol.md §1-§2（帧布局）、docs/cluster/04-transports.md §1-§2（传输契约与 loopback）
3. kernel/include/ipc/ipc.h（a20_channel_ep_t 与 send/recv/shutdown 函数族）和 kernel/ipc/a20_channel.c 的 ch_try_enqueue、a20_channel_ep_peer_shutdown 附近——理解 03-§1 的"代理端点"方案为什么不需要改快路径

实现（严格按 03-§5 顺序，每步全架构编译后再下一步）：
frame.c（离线金样自测）→ route.c + export.c（含限额）→ transport.c + loopback.c（含 CONFIG_CLUSTER_TEST_HOOKS 测试钩子：drop/delay/dup/corrupt/partition）→ remote_ep.c（代理端点 + TX 线程 + 在途事务 + deadline 定时器）→ sys_native_cluster.c 接线 set_self/export/connect/link_status → 内核自检（虚拟节点 echo 往返、超时、CLOSE、handle 拒绝、限额打满）。

硬性要求：
- 本机路径零改动：node_id==LOCAL 走改造前逐指令相同的路径；跑一个 channel 密集既有负载（如 ufsd 文件操作）确认无回归。
- 03-§6"常见坑"逐条自查，报告中逐条回答"是/否/不适用+理由"。

验收：make check-riscv64-bringup + 另一个架构 + mcu profile 构建（先 make help 确认目标名）；自检全绿；金样自测通过。

验收报告：文件清单、门禁结果、03-§6 逐条自查表、规范偏差（须已回写）。
```

## WB1 — 线协议参考编解码 + 金样向量（可与 WA1 并行）

```
你在 A20OS 仓库根目录工作。任务：在 tools/cluster-ref/ 新建一个**主机侧（host）Python 参考实现**，产出 02-wire-protocol.md §9 要求的全部金样向量。它是内核（C）与 MCU 叶子（C）的对拍依据，不依赖任何内核代码——所以你可以与内核开发并行。

第一步精读 docs/cluster/02-wire-protocol.md 全文 + docs/cluster/01-abi.md 的节点哈希节。

交付物（全部放 tools/cluster-ref/）：
1. clframe.py：帧编解码参考实现（逐字段显式 struct.pack，禁止 ctypes 内存映像），含 FNV-1a 节点哈希、CRC16-CCITT、SLIP 编解码（04-§4）、TLV（05-§3）。
2. vectors/：金样向量目录。每种消息 type ≥2 个合法帧（hex 文件 + 对应 JSON 解码结果）；≥20 个畸形帧（错 magic、payload_len 越界、frag 跳号、CRC 错、保留位非零、ver 过高）+ 每例期望行为（丢弃/计数/ERROR 及具体 errno）。
3. scenario_hello_call_close.txt：一个完整事务（HELLO 协商→CALL→分片→REPLY→CLOSE）的帧序列脚本。
4. check_c_side.py：给 C 侧用的对拍 runner 约定——读取 C 程序对向量的解码输出并比对（约定输出格式，写 README）。

质量要求：你自己先用 Python 实现跑通全部向量自洽（编码→解码往返一致）；向量里的每个字段都能说出在 02 文档哪一节。

验收：python3 tools/cluster-ref/ 下自测全绿；向量覆盖清单与 02-§9 三条要求逐条对应。

验收报告：向量数量统计、覆盖清单、对 02 文档发现的任何歧义（逐条列出——这正是你要帮规范堵的洞）。
```

## WB2 — 双 QEMU 实例网络打通（可与 WA1 并行）

```
你在 A20OS 仓库根目录工作。任务：不依赖任何集群代码，打通两个 A20OS QEMU 实例之间的 UDP 通信，为 WA2 准备验收环境。

第一步：
1. make help 与 tools/run-targets.mk——搞清楚 run-<arch> 如何启动实例、实例 TOML（instances/*.toml）如何被消费、网络后端当前怎么配。
2. docs/platforms/ 下对应平台文档与 kernel/net/ 的既有测试（user/cmds/net/ 有 ping/netctl 等工具可参考用法）。

交付物：
1. instances/qemu-riscv64-cluster-a.toml 与 -b.toml：两个实例各有独立 MAC/IP/主机端口映射；若实例 schema 缺少表达这些的字段，扩展 schema（改动最小化，向后兼容现有全部 toml——跑一遍现有实例的构建证明没破）。
2. tools/cluster-net-up.sh：一键拉起双实例并验证——A ping B 通、B ping A 通（用现有 ping 命令）；记录启动参数。
3. docs/cluster/02-udp-demo.md（新建）：接线拓扑图、命令、预期输出。

验收：脚本一键复现双向 ping；现有 instances/*.toml 全部仍可正常构建启动（抽 3 个不同架构验证）。

验收报告：schema 改动清单、脚本用法、三个现有实例的回归结果、发现的 QEMU 网络坑（写进 demo 文档）。
```

## WA2 — UDP 传输 + 跨机 channel_call（WA 轨道第 2 棒）

```
你在 A20OS 仓库根目录工作。前提：WA1（loopback 骨架）验收通过；WB1 金样向量在 tools/cluster-ref/vectors/；WB2 双实例环境可用（tools/cluster-net-up.sh）。

第一步按序精读：docs/cluster/04-transports.md §3（UDP 规范）、02-wire-protocol.md §4/§6（CALL 生命周期、HELLO 状态机）、05-userspace.md §1（clusterd 职责——本阶段只做骨架）。

实现：
1. kernel/cluster/udp.c：按 04-§3 全部要点（内核内部 socket API、MTU 1472、禁 IP 分片依赖、PING/PONG 心跳与 UP/SUSPECT/DOWN 状态机）。
2. 离线上拍：用 WB1 的 check_c_side.py 约定，内核解码器跑全部金样向量（含畸形帧），全过才进联调。
3. clusterd 骨架（user/svc/clusterd.c，参照 echod.c 范式）：set_self → 静态路由注入（从实例配置读对端）→ PING 保活。不做 SWIM。
4. demo-echo：按 05-§5 demo-echo 定义实现并跑通全部三条错误路径（服务未导出 NOT_FOUND、杀服务 REMOTE_CLOSED、杀实例 TIMEOUT/NODE_UNREACHABLE）。

硬性要求：帧在线上必须与 WB1 向量逐字节一致（对拍是强制的，不是建议）；联调不通先怀疑自己对 02 的理解，把争议点带回报告。

验收：demo-echo 四路径全过（结果写入 docs/cluster/02-udp-demo.md）；金样对拍全绿；make check-riscv64-bringup/user 通过；不开集群配置的现有实例行为零变化。

验收报告：对拍结果、demo 四路径输出摘录、门禁结果、规范偏差（须已回写）。
```

## WA3 — 可靠性：分片 + 重传 + 统计（WA 轨道第 3 棒）

```
你在 A20OS 仓库根目录工作。前提：WA2 验收通过。

第一步精读：docs/cluster/02-wire-protocol.md §5/§7/§10（分片、可靠传输、计数器）、01-abi.md 限额表、04-transports.md §3 的 lane 化段落、03-kernel-impl.md §3（内存纪律）。

实现：
1. reasm.c + reliable.c：按 02-§5/§7 逐条（位图重组、5s 超时淘汰、累计 ACK 捎带、RTO 退避、256 seq 去重窗口、NACK 可选）。
2. cluster_link_status 输出全部统计字段；02-§10 计数器全量落地。
3. CLUSTER_PROFILE 三档落地：Makefile 照 NET_PROFILE（约 :135）样式加 CLUSTER_PROFILE ?= 2；components/trim.toml 登记；mcu profile 自动 =1 且排除 reasm.c/reliable.c/udp.c。
4. SERVER 档收发 lane 化（按 src_hash 低位分 lane）。

验收（依据 06-production.md，逐条引用编号）：
- P-单元-3、P-单元-4（重组与可靠传输边界）
- P-集成-3（64 KiB 跨机）
- P-故障-1/2/3/4 在 loopback 钩子上全绿，其中 1 和 3 在 UDP 上复跑
- mcu profile 构建通过且二进制不含 reasm/reliable 符号（nm 或 size 证明）
- make check-riscv64-bringup/user 通过

验收报告：测试编号逐项结果、丢包注入下的重传统计数、性能初值（64KiB 吞吐、小消息 RTT，方法见 06-§2）、规范偏差。
```

## WC1 — UART 传输 + MCU 叶子（WA1 后可启动，与 WA2/WA3 并行）

```
你在 A20OS 仓库根目录工作。前提：WA1 验收通过（传输契约与 loopback 在位）；你拥有 kernel/cluster/uart.c 与 kernel/mcu/，不得改 kernel/cluster/ 其他文件——需要核心侧配合的改动写入验收报告请求 WA 轨道处理。

第一步精读：docs/cluster/04-transports.md §4（UART 全节，你的实现依据）、02-wire-protocol.md §6（HELLO 与短地址分配）、01-abi.md（叶子侧只需要其子集）、03-kernel-impl.md §4（分档编译）。

实现：
1. kernel/cluster/uart.c：SLIP 变体编解码、CRC16、部分帧 50ms 超时、短地址路由。WB1 的 SLIP 金样向量对拍先行。
2. MCU 叶子协议面（kernel/mcu/ 内）：HELLO 被动应答、PING→PONG、CALL→CALL_REPLY（单在飞事务）、ERROR、CLOSE；其余帧类型丢弃计数。内置演示算子：u32 向量点积（载荷 = 长度 + 两个 int32 数组）。
3. 头节点侧 glue：静态路由把短地址段指向 uart；提供一个 QEMU 可复现的串口对接方案（socat 或 QEMU chardev 后端管道，脚本化进 tools/）。
4. 头节点演示：riscv64 实例 cluster_connect 到叶子算子 slot，发点积请求，校验结果。

验收：
- 调用结果正确；拔线（关闭串口后端）后调用返回 NODE_UNREACHABLE；恢复后 HELLO 重协商成功（可能拿到新短地址，头节点侧须正确处理）。
- stm32f103 目标构建通过，报告 text/data/bss 前后对比（膨胀须 <8 KiB flash / <1 KiB SRAM，超出要解释）。
- WB1 SLIP 向量对拍全绿；make check-stm32f103（或既有对应目标）通过。

验收报告：尺寸对比、三场景输出、对 WA 轨道的配合请求（如有）、规范偏差。
```

## WD1 — clusterd 完整化（SWIM + 命名解析）

```
你在 A20OS 仓库根目录工作。前提：WA2 验收通过；WA3/WC1 可与本阶段并行收尾。

第一步精读：docs/cluster/05-userspace.md §1-§3 全文（你的实现依据）、01-abi.md 的 cluster_route/event_subscribe/link_status 三节、02-wire-protocol.md（只读 §6 状态机，理解内核给你什么事件）。

实现（user/svc/clusterd.c 及其拆分，范式参照 user/svc/svcmgr.c 与 a20_services.idl）：
1. 引导流程：配置派生 node_id → set_self → 事件订阅 → 种子路由 → 主循环（05-§1 引导节逐步）。
2. SWIM：按 05-§1 参数表实现直接/间接探测、suspect/dead 两阶段、gossip 捎带、incarnation 冲突消解。无外部依赖，≤1500 行。
3. 命名解析：服务名注册（对接本机注册表）+ service:// 三种形式解析，any = 轮询剔除 DOWN/SUSPECT；解析 API 走 TLV（05-§3）。
4. 崩溃恢复：重启后从内核读回导出/路由 + 向邻居全量同步，数据面不中断（05-§1 崩溃恢复节）。
5. 短地址表维护（对接 WC1 的叶子）。

验收：
- 4 节点全互联：杀任一节点，其余 15s 内标记 dead；节点回归后重新被发现。
- clusterd 被杀由 svcmgr 重启：期间跨机 CALL 不中断（用长稳流量验证），成员表 30s 内重建（P-集成-5）。
- service://any/ 轮询与故障剔除正确。
- P-故障-8（HELLO 洪泛）下 clusterd 与内核均不崩，限额计数可见。

验收报告：各场景输出、参数表实现值与 05-§1 的对照、规范偏差。
```

## WD2 — jobd + tree-reduce 演示（终验）

```
你在 A20OS 仓库根目录工作。前提：WD1 验收通过；WA3、WC1 均已验收。

第一步精读：docs/cluster/05-userspace.md §4-§5 全文（作业格式、重试纪律、失败矩阵、demo-wordcount 定义——它们就是验收标准）。

实现：
1. user/svc/jobd.c + 节点侧 jobd-agent：TLV 作业解析、分片调度、(job_id,shard_id) 去重、失败重派（≤max_retries）、tree-reduce（中间归约节点优先 CAP_RELAY）。
2. demo-wordcount：05-§5 定义逐条落地——固定数据集、16 片、两级归约、单机参考脚本、杀 1 个非根节点结果仍逐字节一致。一键启动脚本进 tools/。
3. 失败矩阵：05-§4 表格五行逐行一个测试用例，全部脚本化。

验收：失败矩阵全绿；demo-wordcount 两种节点配比（8 服务器节点；6 服务器+2 MCU 叶子模拟）均输出一致；make 全量既有门禁零回归。

验收报告：失败矩阵逐行结果、两种配比的演示输出、性能数值（作业总时长、聚合流量分布——确认没有全叶子直连根）、规范偏差。
```

## WE1 — 生产硬化：故障注入 / 模糊 / soak / 性能

```
你在 A20OS 仓库根目录工作。前提：WD2 验收通过——功能已完整。本阶段不加功能，只证明它扛得住。

第一步精读 docs/cluster/06-production.md 全文（你的测试清单，编号即交付物编号）。

任务：
1. 补全 P-单元/P-集成 中此前各阶段未覆盖的余项，全部脚本化。
2. P-故障 1-8 全矩阵在 loopback 钩子上跑绿；1/3/5/6 在 UDP 双实例上复跑。
3. P-模糊：帧解码 + HELLO 协商的语料驱动模糊（host 侧 harness 编译同一份 frame.c；种子用 tools/cluster-ref/vectors/），≥10⁶ 用例或 2 小时零崩溃。MCU 叶子解码路径同 harness 覆盖。
4. P-soak：8 小时（CI 环境不允许则本地 8h + CI 30 分钟档），按判据逐项记录。
5. 性能：06-§2 六个指标全测，数值与瓶颈分析写进 06-production.md 附录；本机 channel 回归必须 <3%（这是硬门禁）。
6. 新 CI 目标：check-cluster-unit / -loopback / -udp / -fuzz-smoke，接入 .github/workflows/ci.yml 的既有矩阵派生方式（禁止硬编码架构清单）。

验收：上述全绿，06-production.md 附录更新完毕。

验收报告：按 06 编号逐项的通过证据（日志摘录/计数器快照）、性能数值表、发现的缺陷清单（含已修/未修分级——未修的阻塞 WE2）。
```

## WE2 — 安全审查 + 运维文档 + 终验

```
你在 A20OS 仓库根目录工作。前提：WE1 验收通过，缺陷清单已清零或有豁免结论。

第一步精读：docs/cluster/06-production.md §3/§4/§6、01-abi.md（权限节）、00-design.md 非目标节（确认没有越界实现）。

任务：
1. 安全基线逐条验证：默认零导出（未导出服务远程 connect 得 NOT_FOUND）、无 CLUSTER_ADMIN 权限的六个 syscall 全被拒（每 syscall 一测试）、跨机 handle/捐赠拒绝。写进 CI。
2. 威胁模型落地：v0"可信 LAN"声明写进运维文档显著位置；v1 链路加密设计（per-link PSK + AEAD，含 MCU 成本评估）写成 docs/cluster/ 内一节，只设计不实现。
3. docs/cluster/10-operator-guide.md：实例配置、种子引导、拓扑规划（树形/全互联选型）、升级顺序、排障决策树（06-§4 定义的链路：超时→link_status→路由→导出→计数器）。
4. docs/cluster/11-api-tutorial.md：手把手写一个可远程调用的服务（export→any 解析→错误处理），代码可直接编译。
5. 全文档一致性巡查：00/01/02/03/04/05/06 互引编号有效、核对日期刷新、与代码现状一致（抽查每个文档 ≥3 个 path:line 引用验证）。
6. 终验：干净 worktree 从 clone 到 demo-wordcount 一键跑通（全程按文档操作，不许用记忆——这是文档完备性的真实测试）。

验收：上述全过。产出最终验收报告：功能/可靠/安全/性能/工程五条生产就绪定义（00-design.md）的逐条结论。这份报告就是"可投入使用"的签发依据。
```

---

## 故障处置

| 情况 | 处置 |
|---|---|
| agent 报告规范与代码冲突 | 人仲裁 → 改规范 → 该阶段重跑受影响部分 |
| 并行轨道需要对方的接口变更 | 在规范文档登记接口契约先行，实现可并行，联调排后 |
| 某阶段验收反复不过（≥3 轮） | 把该阶段按规范文档拆成更小的子任务（规范已按文件/章节切分，天然可拆），逐个子任务验收 |
| 金样对拍不一致 | 以 02-wire-protocol.md 文字为准裁决；文字有歧义就是规范 bug，先修规范 |
