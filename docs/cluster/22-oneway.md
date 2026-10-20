# 统一业务单向 SEND

本节定义已接入主机和静态 MCU leaf 的单向业务扩展。CALL 的服务端产生业务
回复；SEND 的服务端
只接收输入，内核产生传输接纳回执。两者共享身份、会话、路由、分片、资源
预算、重传和去重，不把服务器进程模型强加给 MCU。以下区分已执行的
软件验证和仍需完成的规模、时长及物理板验收。

## 接口与激活

- 主机显式以 `A20_CLUSTER_CAP_ONEWAY`（bit 4）配置身份能力。远端连接使用
  `A20_CONNECT_RELIABLE | A20_CONNECT_ONEWAY`，导出使用 `A20_EXPORT_ONEWAY`
  （bit 3）。持久配置用 `feature oneway` 激活；SDK 的
  `a20_cluster_connect_bound_flags()` 可同时绑定目录发现的 epoch 与发送模式。
  默认配置不增加这个能力；未开启的既有部署保持原 HELLO 能力集。
- ONEWAY 能力需要两端启用。新端能读取旧端 HELLO，但单向连接到未声明能力的
  对端立即返回 `CLUSTER_UNSUPPORTED`。旧版严格校验器会拒绝未知能力位，因此
  启用前须升级相关端点；这里没有声称新能力与旧二进制自动互通。
- 连接返回只写 channel handle，仍使用普通 `channel_send`。读取或融合
  `channel_call` 在权限检查处失败。LOCAL 连接仍是本地 channel，服务端照常
  接收携带连接句柄的握手；连接模式必须与导出标志相同。
- MCU 使用 `a20_mcu_leaf_set_sink(name, len, receive, ctx)` 注册一个静态生命周期
  接收器，slot=3。须在首次握手前配置，不分配堆。回调输入至多 206 B，不能
  保留输入指针、重入 leaf 或无限等待；返回 A20 状态码。只有注册后才声明
  ONEWAY 能力和目录服务。原 dot/work 服务保留。

目录的 `A20_DIRECTORY_ONEWAY` 与 Native 导出位一致。CALL 与 SEND 使用独立
导出：SEND 不写 CALL 配对 FIFO，服务端误发的回复由导出工作线程丢弃，不会
抢占其他 CALL 的回复位置。未匹配的远端连接在首次发送收到绑定会话的错误
回执后关闭；原 slot 连接没有远端服务预查握手。

## 线协议与完成含义

业务 SEND 使用 v1、RELIABLE、CRC16，`txid=0` 保持既有格式约束；非零 `seq`
作为可靠消息编号，不能取 `0xffffffff`。负载以双方 16 B boot epoch 开头，
后接原业务字节。无可靠标志的业务 SEND 不执行。控制 CS 仍为保留 slot、
`seq=txid=0` 的独立格式。

验证 epoch 与能力后，接收实现才把 seq 转为内部事务键。SEND 与 CALL 共用
每来源 64 位滑动窗口，先预留接纳记录再执行。SEND 仅预留 54 B 空回执缓存，
不按 64 KiB 最大 RPC 回复预留。回复使用已有 v1 CALL_REPLY 封装，编号为
SEND 的 seq，业务回复长度为零，仅携带反向 epoch 与状态码。代理内部消费
回执并发送 slot-zero 退休收据，不向只写端点注入消息。ACK 仍只表示可靠层
接纳，不单独完成事务；绑定 epoch 的完成回执才释放重试缓冲。

主机成功回执表示已放入接收 channel；MCU 成功回执表示静态回调已接纳。它
不证明业务执行结束、外部副作用提交或持久化。`channel_send` 成功只表示本地
队列接纳；错误回执、会话失效和预算耗尽通过代理关闭体现。应用若需要业务
完成确认，应定义独立 CALL 查询或应用层确认服务。在观察到自己的业务完成
条件前关闭代理，会放弃尚未完成的传输；不能把关闭当作 flush。

## 有界流水线与大消息

主机单向代理最多 64 个在途；同一 LEAF 对端的全部可靠代理合计最多 1 个。
满时工作线程保留本地队列
中的消息，让 channel 原有阻塞/NONBLOCK 规则提供背压。共享重试预算满时
至多保留每工作线程一个待提交消息，等待计入 30 秒提交/事务预算。

同一对端所有可靠代理的未完成序号跨度必须小于 64，包括 CALL 与 SEND。
较早未接纳消息丢失时，后续事务不能越过接收窗口把它变成“旧消息”。这个
约束比“每个代理数量不超限”严格；分配编号与登记在途在同一把锁中完成。
声明 ONEWAY 的新发送端遵守该约束，因此接收端可以回收同来源同 epoch、已
完成且落后新编号至少 64 的缓存；运行中记录不回收。未声明能力的端点仍按
原保留期限处理。完成收据丢失导致 MCU 单缓存背压时，主机可重发最近一次
已完成事务的绑定收据提示，不把无 epoch 的 ACK 当作完成证明。

服务器/默认主机档 SEND 复用既有分片几何与预算，重组键增加 seq，防止多个
`txid=0` 的 SEND 混为一个消息。MCU 不开启重组；UART 限制仍为物理 MTU256，
不将主机 64 KiB 负载无条件下放到 MCU。

重组首片期限为 30 秒，覆盖事务预算且不由后续片续期。旧 2 秒期限在持续
10% 丢帧下早于完整重试周期，已由真实内核失败用例发现并修复。分配前过滤
已接纳请求与已结束代理的回复片；重复请求每 250 ms 最多触发一次缓存回放。
完成收据清除对应部分重组，结束代理清理部分回复，槽位/字节额度保持有界。

## 验收状态

已执行的软件验证：

| 入口 | 已观察结果 | 范围 |
|---|---|---|
| `check-cluster-unit`、`check-cluster-fuzz` | SEND/事务模式冲突、54 B 回执、序号回绕、缓存回收、收据提示和分片资源测试通过 | 生产协议代码；ASan/UBSan，不是物理板 |
| `make smoke-native-cluster` | LOCAL 单向接收、模式拒绝、只写句柄、融合 CALL 权限拒绝与 epoch 绑定 helper 通过 | 真实 Native ABI 与内核 |
| 同一入口的 `test_clx_oneway.c` | 256 次静态 sink 接纳、768 个重复帧，0/1/64/206 B、错误回调缓存、旧 epoch、路由 CS 均通过 | 真实 UART 与 leaf 代码，替换时钟及板级 I/O |
| `make check-cluster-send` | 两端各 264 次 SEND、96 次并发 CALL；服务器档持续 10% 丢帧，以及三内核中继的服务器/默认混合档均通过 | 独立 QEMU 内核，64 KiB 服务器业务，混合档按较小限额 |
| `tools/check-cluster-gateway.py --send` | 128 个 8/64/128/206 B SEND、丢请求/回执/ACK/退休收据、重复帧、MCU 单缓存恢复和旧 SEND 重放隔离通过 | RISC-V Native→UDP→x86 COM2→生产 leaf 主机进程；没有物理板 |
| `tools/check-cluster-udp.py --send --faults --restart` | 丢 SEND/完成回执/ACK/收据、重复/乱序/丢片、对端重启和旧 CALL/SEND 重放通过 | 两套独立内核；接收业务重复计数为零 |

每次软件运行的源码摘要、QEMU 命令和日志保存在检查器的 `--log-dir` 下。
本轮 `send-server/`、`send-routed-mixed/`、`send-restart-final/` 保留成功记录；
之前的缓存满、2 秒重组超时和大 SEND 编码失败日志也保留，没有当作通过。

网关的 fixture 以静态回调记录业务编号，另用真实 CW/DOT 接口提供应用查询；
Native 观察 128 次接纳后再等待在途/重试字节归零。检查器只转发、丢弃、
重复或重放真实帧，不生成业务回复。丢退休收据后必须观察同事务收据再次
出现及 MCU NO_SPACE 背压，再验证后续 SEND 和 CALL 恢复。

四节点 60 秒 SEND 已测量，业务核对通过但吞吐未达标，见
[../status/history/four-node-load.md](../status/history/four-node-load.md)。尚未完成物理板与 8 小时混合
soak；静态 UART 配对及短时网关验证不能替代这些证据。
