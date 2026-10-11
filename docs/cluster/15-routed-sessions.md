# 多跳会话与 UDP/UART 网关

## 1. 已实现的统一路径

客户端不需要知道最终节点运行 MCU 固件还是完整内核。先经固定目录发现服务，
再建立绑定最终节点 boot epoch 的可靠 channel；算子与作业格式保持一致。

```mermaid
flowchart LR
    A[Native 客户端内核] -->|UDP / CL v1| G[RELAY 网关内核]
    G -->|UART / SLIP / CL v1| L[静态内存 MCU leaf]
    A -. CS 双向挑战：最终身份、epoch、路径 MTU .-> L
```

HELLO 只确认邻接关系。客户端与网关的 HELLO 不能证明 leaf 的启动 epoch。
`session.c` 在直连查询失败后，检查下一跳已经通过 HELLO 且声明 RELAY 和
RELIABLE，才启用 CS/1 端到端发现。接收适配器仍先执行下一跳地址和来源检查。

网关须预先拥有原调用者和目的节点的路由。中间节点递减 TTL，转发原始
来源、目的、txid、slot、序号及会话前缀；它不能替 leaf 回答 CS 或改写业务身份。
当前是受信集群内的重放防护，没有密码学身份认证。

## 2. CS/1 格式

CS 使用 wire v1 的 SEND，固定槽位 `0xfffffffd`，flags/frag/txid/seq 均为 0，
CRC16 必须存在。载荷严格为 72 B，完整帧 106 B，MCU 单帧即可容纳。
所有多字节整数均显式小端编码。

| 偏移 | 长度 | 字段 |
|---:|---:|---|
| 0 | 4 | `CS`、版本 1、kind：PROBE=1 / REPLY=2 |
| 4 | 4 | 路径物理 MTU |
| 8 | 8 | 本次 challenge cookie |
| 16 | 16 | 来源完整 node_id |
| 32 | 8 | 来源 boot epoch |
| 40 | 4 | 来源能力 |
| 44 | 4 | 来源资源档 1/2/3 |
| 48 | 16 | 目的完整 node_id |
| 64 | 8 | 目的 epoch：PROBE 必须 0，REPLY 回显探测者 epoch |

完整 ID 必须与帧哈希一致，不接受保留 ID、冲突来源、无效资源档、未知能力位、
无 RELIABLE 或不合法的片能力组合。目的节点还要核对完整目的 ID。

收到 PROBE 可回复本机信息，但不能仅据此改变已确认的远端 epoch。未知节点、
不同 epoch 或租约过期必须发起本方挑战。只有匹配当前 cookie、完整双方 ID、
本机 epoch 及当前路由/邻接会话的 REPLY 才能确认。cookie 在单次启动内单调
分配，耗尽后拒绝新挑战；回显目的 epoch 防止前一次本机启动的回复被采用。

挑战每 250 ms 重发，8 次后退休 cookie；控制面可重新发起新挑战。
会话租约 10 s，5 s 开始主动刷新。重复或旧回复不能刷新租约或使 epoch 回退。
客户端目录解析把等待会话确认计入总 budget，不要求应用自行无限重试。
路由变化、删除或下一跳断开使绑定失效；确认新 epoch 会取消旧代理。

## 3. 路径预算和 MCU 去重

探测起点填写出口 MTU。每个 RELAY 把该值缩小到出口预算并重算 CRC；应答
同时受已探测值和反向出口限制。当前多跳业务以这个双向最小 MTU 为单帧上限，
**没有为间接目的协商分片**。UDP→UART 路径为 256 B：可靠业务请求最多
206 B、回复业务最多 202 B。超出预算在本地结束调用，不把大帧送到串口后丢弃。
直连 UDP 的 4 KiB / 64 KiB 逻辑负载能力仍见 [14-fragmentation.md](14-fragmentation.md)。

完整内核的端到端表槽位数等于资源档路由上限（4/256/4096），不分配每次探测
的动态对象，锁内不进行网络 I/O。表满时拒绝新增发现。

leaf 使用 4 个静态来源槽位，每个槽位有完整 ID、会话状态和独立序号窗口。
来源表满后拒绝新来源；运行期间不逐出去重窗口。所有来源仍共用一个接纳前
预留的回复缓存，不能用更多来源绕过单在途限制。回复收据还核对来源哈希和
来源 epoch，同 txid 的其他来源不能释放缓存。

网关重启只撤销端到端会话的确认状态，保留 leaf 上原调用者的窗口和回复。
重新确认同一调用者 epoch 后，旧事务仍只重放；确认调用者新 epoch 才重置其
窗口。leaf 自身重启则采用新的持久 epoch，旧目的 epoch 的 CALL 直接拒绝。

串口头节点仍每 5 s PING，连续 3 次未应答进入 SUSPECT，再 3 次进入 DOWN。
首次漏掉 PONG 即尝试新 HELLO 挑战，使重启的被动 leaf 能在正式 DOWN 前恢复；
没有收到有效挑战回复时，不采用新会话或清除执行记录。

## 4. 配置顺序

以客户端 `...01`、leaf `...02`、网关 `...03` 为例：

1. 网关设置 RELAY，安装 leaf 的 UART 短地址路由，以及客户端的 UDP 路由。
2. 客户端先安装网关自身 UDP 路由，再把 leaf 路由安装到同一个 UDP next_hop。
3. 客户端解析 leaf 的 `work` 服务，用得到的 slot 和最终 epoch 连接。
4. clusterd 可从内核路由快照重新构造目录，`service://any/work` 及混合树复用同一路径。

UDP 下一跳端口使用网络字节序：44020 编码为 `ab f4`。
UART 短地址使用小端编码。自动传播路由和配置监管仍属于后续控制面工作，
CS 本身不信任陌生节点安装路由。

## 5. 已执行的验证

```sh
make check-cluster-unit
make check-cluster-fuzz
make check-cluster-gateway
```

单元门禁直接执行生产 `session.c`，覆盖相关 cookie/本机 epoch、旧回复、
网关启动会话变化、路由断开、租约、重试耗尽、固定表满和 10 万次精确长度
变异。主叶配对运行生产 `uart.c` / `leaf.c`，验证不同调用者的窗口、收据隔离、
来源额度、网关重启保留去重及调用者 epoch 旧回复拒绝。ASan/UBSan 验证解析边界。

网关门禁启动两个独立 TCG 内核：RISC-V Native 客户端和 x86 RELAY，经真实
Ethernet/UDP、emulated COM2 与独立进程内的生产 leaf 通信。它丢弃两个方向
的首个 CS PROBE/REPLY、首个 UART 工作请求/回复，验证目录、96 次可靠 DOT、
路径 MTU、本机超限结束、资源/任务回收。随后重启 leaf（200→300），注入旧
CS 回复与旧 CALL，验证旧 epoch 连接拒绝、any 发现和 7 任务混合归约。
检查器比对真实执行次数与实际转发的唯一事务数，不生成 guest 回复。

证据位于 `.kernel-build/cluster/gateway/`：内核及两次 leaf 日志、线上帧轨迹、
注入计数、源码 SHA256、环境和时间。仿真吞吐不作为物理 MCU 性能。
完整物理 STM32 网关联调和多跳主机间分片仍未完成。
