# 用户态集群服务规范（clusterd / 命名 / jobd / 演示）

内容已按 2026-10 源码核对。服务编写范式参照 `user/svc/echod.c`（最小 channel 服务）、`user/svc/svcmgr.c`（监管）、`user/svc/a20_services.idl`（服务接口声明惯例）。所有服务是 Native ABI 普通进程，崩溃由 svcmgr 重启——**状态必须能重建**（从内核查询 + 种子配置），禁止把不可恢复状态只存内存。

## 1. clusterd（成员与路由服务）

### 职责边界

| 做 | 不做 |
|---|---|
| 启动时 `cluster_set_self`（从配置/UID 派生 node_id） | 帧编解码（内核的事） |
| 种子节点引导、SWIM 成员检测、路由计算与 `cluster_route` 注入 | 业务消息转发（RELAY 在内核） |
| 服务名注册与 `service://` 解析（对接本机服务注册表） | 作业调度（jobd 的事） |
| MCU 叶子短地址表维护 | 叶子算子实现 |
| 链路统计采集（`cluster_link_status` 轮询，供观测） | — |

### 引导流程

1. 读实例配置（TOML 注入的环境/参数）：node_id 种子、profile 档、种子节点列表（可为空=孤立引导）。
2. `cluster_set_self` → `cluster_event_subscribe(全mask)`。
3. 对每个种子：注入静态路由（metric=100，标记"引导"），等 LINK_UP。
4. 进入 SWIM 主循环；稳定后把引导静态路由替换为计算路由（metric 更低）。

### SWIM 参数（LAN profile，服务器档）

| 参数 | 值 | 说明 |
|---|---|---|
| 探测周期 | 1s | 每周期随机选一个存活成员发应用层探测（借 PING/PONG 之外的应用消息，payload 携带 gossip） |
| 间接探测 | k=3，超时 500ms | 直接探测失败→委托 3 个成员代探 |
| suspect → dead | 15s | suspect 期间继续参与 gossip（带标记） |
| gossip 捎带 | 每条探测/应答带 ≤8 条成员变更 | 感染式传播，收到新变更更新本地表 |
| 成员变更去抖 | 同一 node 的 incarnation 严格大者胜 | incarnation = 本地单调计数，节点重启清零并靠全量同步对齐 |

实现参照公开的 SWIM 论文（D.Das 等, 2002）与 memberlist 的算法描述；**不要**引入外部依赖，逻辑 ≤1500 行。

### MCU 叶子管理（头节点侧）

- 短地址分配：2 字节空间，LRU 回收长期 DOWN 的叶子。
- 叶子判定完全由头节点主动 PING；叶子超时回 DISCONNECTED 后重新 HELLO，可能拿到新短地址——**业务不得以短地址做持久标识**，持久标识是完整 node_id。
- 路由注入：叶子 → uart 传输；其他头节点/服务器 → udp。头节点置 CAP_RELAY，叶子间通信经头节点转发。

### 崩溃恢复

svcmgr 重启 clusterd 后：set_self（同 node_id，从配置重派生）→ 从内核读回现存导出/路由（内核状态未丢）→ 向所有 LINK_UP 邻居请求全量成员同步。**全程不重置内核路由表**——数据面在 clusterd 重启期间继续转发。

## 2. 服务命名与解析

```
service://local/<name>   → 本机注册表查名 → 直接 channel_connect（不经过集群路径）
service://<nodehex>/<name> → clusterd 查该节点的 (slot) → cluster_connect(node_id, slot)
service://any/<name>     → clusterd 在"导出过该服务且存活"的节点集中按策略选一个
```

- 节点注册自身导出：服务进程 `cluster_export` 后，clusterd 经 LINK_UP 后的全量同步把 (node, name, slot) 扩散进 gossip。
- `any` 策略 v0 = 轮询 + 剔除 SUSPECT/DOWN；v1 可加 RTT 就近与负载因子。
- 解析 API：clusterd 自身注册为本机服务 `"cluster"`，客户端用普通 channel_call 发解析请求（消息格式进 `a20_services.idl` 惯例，TLV 编码，见 §3）。解析结果含 (node_id, slot, caps)。

## 3. 应用消息编码：TLV 约定

跨机消息载荷的统一约定（ufsd 等本机服务不受约束）：

```
[tag:u16][len:u16][value:len 字节] ... 对齐到 4 字节
```

- tag 空间：0x0001–0x00FF clusterd 控制面；0x0100–0x01FF jobd；0x8000+ 应用自定义。
- 字符串不带 NUL；整数小端。**禁止**把 C 结构体内存映像直接当载荷（NOMMU 同构机器间也不行——ABI 演进会埋雷）。
- 对齐按**每个元素**结算（实现期核对 2026-10）：value 后补 `(-len) % 4` 字节零填充，
  pad 不计入 `len` 字段；下一元素的 tag 从 4 字节边界起。金样见 `tools/cluster-ref/vectors/valid/`
  各 TLV 载荷。

## 4. jobd（作业调度服务）

### 作业描述（v0）

```
JOB  ::= TLV{
  0x0101 job_id:u64          客户端生成，幂等键
  0x0102 task_kind:u8        0=ELF_EXEC(服务器节点) 1=OPERATOR(MCU叶子内置算子)
  0x0103 payload:bytes       ELF 引用(服务名) 或 算子ID:u32
  0x0104 data_shards:TLV[]   每片 {shard_id:u32, bytes}
  0x0105 reduce_kind:u8      0=无 1=SUM 2=CONCAT 3=自定义(服务名)
  0x0106 max_retries:u8      默认 2
  0x0107 deadline_ms:u32     全作业 deadline
}
```

### 语义与重试纪律

- **幂等是调用方的契约**：jobd 保证 at-least-once 执行，用 (job_id, shard_id) 去重已收结果；重复结果丢弃。
- 分片调度：按存活工作节点数切分/指派；节点 DOWN → 该片重派（不超过 max_retries）。
- tree-reduce：分片数 > 2×节点数时，jobd 指定中间归约节点（优先 RELAY 能力节点），逐级 SUM/CONCAT，**禁止**全叶子直连根。
- ELF 任务交付：节点侧 jobd-agent 收到任务后经本机 exec 起进程，输入经 channel 喂入；agent 也是可崩溃服务。
- MCU 叶子任务：只发 (算子ID, 参数 TLV)，载荷 ≤ UART 单帧预算（04-§4）。

### 失败矩阵（必须逐条有测试）

| 故障 | 期望行为 |
|---|---|
| 工作节点执行中 DOWN | 该片重派他节点，作业完成 |
| 根 jobd 所在节点 DOWN | 客户端收到 NODE_UNREACHABLE；作业状态不可恢复（v0 接受，文档注明） |
| 中间归约节点 DOWN | 其下游片重派并重新归约 |
| 重复交付同一片 | 去重，结果不重复计入 |
| 作业 deadline 到 | 未完成的片取消（尽力），客户端得 CLUSTER_TIMEOUT + 部分结果标记 |

## 5. 演示程序（验收即文档）

### demo-echo（WD1 验收用）

服务器档两节点：B 导出 `"echo"` 服务（`cluster_export`），A 客户端 `service://any/echo` 解析 → connect → channel_call 打印往返。覆盖：正常、B 杀服务（REMOTE_CLOSED）、B 杀实例（TIMEOUT/NODE_UNREACHABLE）。

### demo-wordcount（WD2 终验用）

- 数据：仓库内固定文本集（放 `tools/corpus/` 或演示目录，≤1 MiB，确定性）。
- 8 个 QEMU 服务器节点（或 6 服务器 + 2 模拟 MCU 叶子跑计数算子）。
- jobd 切 16 片，map=词频计数，reduce=SUM，tree-reduce 两级。
- 期望输出：词频表与单机参考实现（脚本）逐字节一致；运行期间杀 1 个非根节点结果仍一致。
- 启动脚本一键复现（进 `tools/`，启动方式基于 `make run-<arch>` 与 WB2 的互联接线）。
