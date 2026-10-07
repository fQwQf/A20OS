# 生产就绪标准（WE 轨道依据）

> **实现状态（2026-10-07）**：本文件描述的测试矩阵（P-单元/集成/故障/soak/模糊）、CI 门禁（`check-cluster-*`）、性能实测与安全基线验证**均尚未落地**，属于 WE 轨道（WE1/WE2）计划；文内条款编号供后续验收引用，当前**不可**作为已通过证据。已完成的是 W0/WA1/WB1/WB2/WC1（ABI 冻结、内核 loopback 数据面、金样向量、双 QEMU 互联、UART/MCU 叶子，进度总览见 [impl-prompts.md](impl-prompts.md) 头部横幅）——WE 须待 WA2/WA3/WD 全部功能完成后才启动。

本文定义"可以投入使用"的量化门槛。所有测试必须脚本化、进 CI 或本地一键复现。条款编号在验收报告中引用（如 "P-故障-3 通过"）。

## 1. 测试矩阵

### P-单元

| # | 对象 | 内容 | 依据 |
|---|---|---|---|
| 1 | 帧编解码 | 02-§9 全部金样向量（合法+畸形 ≥30 例） | 02-wire-protocol.md |
| 2 | 路由/导出表 | 增删替换、限额打满、slot 单调性、并发读写 | 01-abi.md |
| 3 | 重组缓存 | 乱序/重复/超时淘汰/总量超限淘汰 | 02-§5 |
| 4 | 可靠传输 | seq 回绕、去重窗口边界、RTO 退避序列 | 02-§7 |
| 5 | TLV 编解码 | 边界长度、截断、未知 tag 跳过 | 05-§3 |

### P-集成

| # | 场景 | 期望 |
|---|---|---|
| 1 | loopback 双虚拟节点 echo 往返 | 成功，时延记录 |
| 2 | 双 QEMU UDP echo（demo-echo 三错误路径） | 05-§5 全部符合 |
| 3 | 64 KiB 消息跨机 CALL | 分片重组成功 |
| 4 | 头节点→MCU 叶子算子调用 | 结果正确 |
| 5 | clusterd 崩溃重启 | 数据面不中断，成员表 30s 内重建 |
| 6 | demo-wordcount 全程 | 输出与参考逐字节一致 |

### P-故障注入（loopback 钩子先跑，UDP 复跑；每条都是独立用例）

| # | 注入 | 期望 |
|---|---|---|
| 1 | 10% 随机丢帧 | 1000 次可靠档 CALL 全成功，无重复执行 |
| 2 | 50–200ms 随机延迟 | CALL 成功；RTT 统计漂移合理 |
| 3 | 帧重复 | 去重生效，服务方不重复执行 |
| 4 | 1 bit 损坏（csum 开） | CRC 丢弃，重传恢复；解码器零崩溃 |
| 5 | 网络分区 30s 后恢复 | 分区侧调用 NODE_UNREACHABLE；恢复后重新建链成功 |
| 6 | 对端实例 kill -9 | 调用方 15s 内得 NODE_UNREACHABLE 或 TIMEOUT，无句柄泄漏 |
| 7 | 慢消费者（服务方 10s 不应答） | 调用方按 deadline 超时；TX 侧不无限积压（背压返回 RESOURCE_LIMIT） |
| 8 | HELLO 洪泛（1000 个伪造 node_id） | 路由/导出表限额顶住，系统可用，计数器可见 |

### P-soak

8 小时：4 节点，每节点 100 msg/s 混合流量（SEND+CALL，1B–64KiB），10% 丢帧常驻。判据：无 panic/死锁；RSS 与 slab 占用前后漂移 <5%；计数器单调无异常跳变；首尾各 1000 次 CALL 时延 p99 漂移 <20%。

### P-模糊

帧解码器与 HELLO 协商逻辑：对 `frame.c` 解码入口做语料驱动模糊（种子=02-§9 金样，变异=bit 翻转/截断/长度字段篡改），≥10⁶ 用例或 2 小时，零崩溃零断言。MCU 叶子解码路径同测（host 侧 harness 编译同一 `frame.c`）。

## 2. 性能目标（测量脚本进 `tools/`，可复现）

| 指标 | 目标 | 测量方法 |
|---|---|---|
| 本机 channel_call 回归 | 与改造前均值差 <3%（统计显著性 p<0.05） | 现有 channel 微基准（找仓库既有基准或写最小 ping-pong），各 10⁵ 次 |
| loopback 远程 CALL RTT | < 3× 本机 channel_call | 同上经虚拟节点 |
| 双 QEMU UDP CALL RTT（64B） | p50 < 5ms | demo-echo 改测量模式 |
| SERVER 档小消息吞吐 | > 10k msg/s/节点 | 4 节点互打 64B SEND，60s 计数 |
| 64 KiB 大消息吞吐 | > 100 MB/s 聚合（UDP，无丢包） | 批量 CALL 64KiB |
| MCU 叶子算子调用 RTT | < 100ms @115200（载荷 ≤128B） | 头节点侧计时 |

未达标不阻塞合入，但必须记录数值与瓶颈分析进本文附录；回归 >20% 阻塞合入。

## 3. 安全基线

- **默认零导出**：服务不 `cluster_export` 则任何远程节点不可达——测试：未导出服务被远程 connect 得 NOT_FOUND。
- **权限**：无 `A20_RIGHT_CLUSTER_ADMIN` 的任务调 export/route/set_self 全部被拒（逐 syscall 一条测试）。
- **威胁模型（v0 明确声明）**：链路可信的 LAN/机柜内场景。窃听/篡改/伪造**不在** v0 防御范围，必须在运维文档显著位置写明。
- **v1 路线（只写设计，不实现）**：per-link PSK → ChaCha20-Poly1305 帧加密（CSUM 字段升级为 AEAD tag），node_id 与公钥指纹绑定。引入前评估 MCU 侧 ROM/RAM 成本。
- 模糊测试零崩溃（P-模糊）同时作为安全门禁。

## 4. 观测与可运维

- 计数器：02-§10 全量落地，`cluster_link_status` 可读；klog 链路事件单行格式固定（`CLUSTER link <event> node=<hash> transport=<id> reason=<n>`），禁止改格式（运维脚本依赖）。
- 排障决策树（写进运维文档）：调用超时 → 查 link_status state → 查路由表 → 查对端导出 → 查计数器定位丢包环节。
- 升级：协议 ver 兼容窗口一个版本（01-abi §稳定性）；滚动升级顺序 = 叶子先行，头节点其次，服务器任意。

## 5. CI 与回归门禁

- 新 make 目标（风格对齐现有 `check-*`）：`check-cluster-unit`（金样+单元）、`check-cluster-loopback`（P-集成-1）、`check-cluster-udp`（P-集成-2/3，脚本化双 QEMU）、`check-cluster-fuzz-smoke`（模糊 5 分钟档）。
- 进 `.github/workflows/ci.yml` 的从 `print-ci-kernel-arches` 派生的矩阵惯例，不允许手工硬编码架构清单（Makefile 顶部注释的既有教训）。
- 全量既有门禁零回归是合入前提：`make help` 列出的默认 check 目标。

## 6. 文档交付（功能完成≠交付完成）

| 文档 | 内容 | 产出阶段 |
|---|---|---|
| `docs/cluster/10-operator-guide.md` | 实例配置、种子引导、拓扑规划、排障决策树、威胁模型声明 | WE2 |
| `docs/cluster/11-api-tutorial.md` | 用集群 ABI 写一个可远程调用的服务（从 export 到 any 解析） | WE2 |
| 本文附录 | 性能实测数值表 | WE1 |
| 01/02/04/05 更新 | 全部实现期偏差回写，头部核对日期刷新 | 各阶段即时 |
