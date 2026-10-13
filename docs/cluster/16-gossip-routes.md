# 有界成员传播和自动路由

实现为 `user/svc/clusterd.c`、共享策略 `kernel/include/uapi/a20/cluster_gossip.h`
和内核条件路由操作。MCU 仍可只运行静态叶子服务；网关负责传播它的节点提示，
客户端使用相同 CS、CD、CW 协议调用 MCU 或主机。

## 1. 配置与发现顺序

只配置已知邻居为 metric=0 的显式种子。例如网关同时配置客户端 UDP 种子和
MCU `uart:1` 种子，客户端只配置网关 UDP 种子。网关须有 RELAY 能力；具体
身份及 COM2 启动见 [15-routed-sessions.md](15-routed-sessions.md)。双方运行
clusterd 后，客户端不需要预先配置 MCU 的间接路由。

```mermaid
flowchart LR
    A[已确认种子网关] -->|CM GOSSIP 提示| B[安装有期限的候选路由]
    B -->|双向 CS 确认目的 epoch| C[直接查询目的 CD/CW]
    C -->|完整目录和能力| D[ALIVE 服务可供 any 调度]
    B -->|提示停止 5 秒| E[条件删除动态路由]
```

仅向已确认 ALIVE、声明 RELAY、具有控制槽位且静态距离为 0 的邻居发起传播
查询。通过该邻居的下一跳地址到达被传播的目的，业务不会由控制面重新编码。
学到的节点不自动成为新的传播邻居；它可以被其他显式种子邻居继续传播。
提示不直接确认 ALIVE，不采用提示中的新 epoch，不发布提示中的服务。
最终目的的双向 CS、CD 目录和 CW 能力分别通过后才进入可调度目录。

## 2. CM/1 分页契约

请求仍为 96 B，回复仍为 80 B。公共格式定义见 `cluster_control.h`。
GOSSIP kind=4，请求 offset4 为游标，offset8/12 为零，offset16 为 16，
offset24 为非零 cookie，offset32 起为请求者 node_id。回复字段：

| 偏移 | 内容 |
|---|---|
| 0..3 | CM、版本 1、kind 4 |
| 4 | status；0 或 NOT_FOUND |
| 8 | 目标完整 node_id（16 B） |
| 24 | 提示 boot_epoch（u64） |
| 32 | 到目标的距离（u32） |
| 36/40/44 | 成员状态、能力、资源档（各 u32） |
| 48 | 下一游标（u32） |
| 64 | 当前请求 cookie 回显（u64） |
| 72 | incarnation（u64） |

其余字节为零。成功页要求游标前进且不超过 8192；一次探测最多 8 页，保留
游标供下一轮继续。整个传播阶段期限 700 ms。旧节点不支持该 kind、格式错误、
超时或容量不足只终止该传播轮次，不把已经通过直接探测的邻居误判为失联。
分页期间成员可变化，因此结果是提示流；不承诺原子全局快照。

## 3. 路由选择、租约与所有权

默认最多 32 个受管路由，与成员表预算一致；没有堆分配和隐式逐出。
只采纳合法 ALIVE 目标，排除自己和 provider，要求 provider 已确认 RELAY 与
RELIABLE。候选距离为提示距离+1，上限 7；更短距离优先，相同距离按 provider
完整 ID 的字典序决胜。同一 provider 可以更新为更长距离，TTL 和跳数上限约束
故障环路；split horizon 禁止把学到的路由立即回传给原 provider。

采用 `A20_ROUTE_DYNAMIC | distance` 保存动态标记。提案仅在内核 LEARN 操作
成功后提交本地所有权；表满/内核容量不足不能产生虚假已安装状态。
显式路由始终优先。每次接受提示续租 5 s；过期只通过 DEL_MATCH 删除自己仍然
拥有的精确路由，再撤销该目标的可选服务。用户 REPLACE 同时发生时由内核
原子检查保护；守护进程不能覆盖或删除用户新配置。

clusterd 重启从内核快照恢复动态标记，给继承路由一次 5 s 刷新窗口。未经新
提示刷新不会永久保留；新提示可以选择原 provider 或更优 provider。停止控制面
不立即清除数据面路由和已建 channel。成员容量不足计数并跳过新记录，不终止
已有成员维护。SUSPECT/DEAD 提示只作用于同 epoch、合法 incarnation 的现有
状态；自身被怀疑时 refute，提高 incarnation。旧提示不能认定一次重启。

## 4. 并发与 MCU 单缓存

后台目录探测与业务调用可同时争用 MCU 的一个回复缓存。接纳前 NO_SPACE
表示请求尚未执行；调用端保留同 txid、重试次数和原期限，等待缓存可用。
已经收到接纳 ACK 的错误按正常终态处理，不能另开 txid 重复执行。
有效迟到回复即使本地代理已超时，也在确认当前完整端点会话后发送完成收据；
不再投递应用结果，避免无主回复把 MCU 缓存占用至完整保留期。旧 epoch 迟到
回复没有收据，不能借此释放新会话的缓存。

## 5. 验证与部署边界

```sh
python3 tools/check-cluster-unit.py
python3 -m unittest tools.tests.test_stamps_clean
make smoke-native-cluster
python3 tools/check-cluster-gateway.py --gossip
```

最后一项运行两套独立内核（RISC-V 客户端、x86 UDP/COM2 网关）和生产静态
leaf 主机进程，注入双向 CS 丢包、业务请求/回复丢包、leaf 重启、旧会话重放
和串口断链。验证自动路由、daemon 重启继承路由、96 次调用、any/混合树、旧
epoch 拒绝、租约清理和实际执行去重。日志、帧 trace、源码 SHA256 与环境
记录在 `.kernel-build/cluster/gateway-gossip/`。Native smoke 检查条件路由 ABI；
共享策略测试检查距离偏好、表满、split horizon、租约及失败安装不提交。

这是有界距离向量提示机制，没有全局一致性或无环证明，不声称复杂拓扑下最短路
即时收敛。CRC 和会话挑战不构成密码学认证，部署信任边界仍为受控网络/串口。
间接业务目前使用最小路径 MTU 的单帧，跨多跳分片仍待实现。物理 MCU 验证
与大规模收敛/吞吐测量仍分别列为未完成。
