# 集群帧协议完整规范（wire v0）

内容已按 2026-10 源码核对（引用对象：00-design.md 帧协议草案、01-abi.md 节点哈希与 errno）。本文是**所有档位、所有传输**共用的线格式唯一依据。任何实现期改动必须回写本文并更新 `tools/cluster-ref/` 的金样向量。

## 1. 帧布局

定长 32 字节头 + 载荷 + 可选 CRC16 尾部。多字节字段**小端**。禁止依赖结构体直接收发做跨节点互操作（MCU 档同构机器间除外）；编解码必须逐字段显式读写。

```
偏移  字段            宽度   说明
0     magic           u16    固定 0x4C43（"CL"）
2     ver             u8     协议版本，v0 = 0
3     type            u8     消息类型，见 §2
4     flags           u16    §3
6     frag            u16    低 12 位=分片序号(0 起)，bit15=末片标志，bit12-14 保留=0
8     txid            u32    事务号：CALL/CALL_REPLY/ERROR 匹配键；SEND 置 0
12    src_hash        u32    fnv1a32(src node id)，见 01-abi.md
16    dst_hash        u32    fnv1a32(dst node id)
20    dst_slot        u32    目标导出槽位（CALL/SEND/CLOSE 用）；其余置 0
24    seq             u32    可靠档序号（每 (src,dst) 链路单调）；不可靠档置 0
28    payload_len     u16    载荷字节数，≤ 传输 MTU - 头 - CRC
30    ttl             u8     每跳 -1，到 0 丢弃；初始默认 8
31    csum_kind       u8     0=无 CRC；1=CRC16-CCITT（poly 0x1021，init 0xFFFF），附于载荷尾 2 字节
32    payload...
```

硬规则：

- 保留位/字段必须发零；接收方对非零保留位**不拒绝**（向前兼容），但必须忽略。
- `payload_len` 与传输层实收长度不符 → 帧丢弃 + 计数 `rx_malformed`。
- magic/ver 不匹配 → 丢弃 + 计数；ver 高于本地时在 HELLO 阶段处理（§6），数据帧静默丢弃。

## 2. 消息类型

| type | 名称 | 载荷 | 说明 |
|---|---|---|---|
| 1 | HELLO | §6 协商结构 | 建链/重协商；UART 档同时完成短地址分配 |
| 2 | HELLO_ACK | 同 HELLO | 应答 |
| 3 | PING | 8B 发送时戳(µs, 本机单调钟) | 链路心跳 |
| 4 | PONG | 回显 PING 载荷 | RTT 采样 |
| 5 | SEND | 应用载荷 | 单向消息（对应本机 channel_send） |
| 6 | CALL | 应用载荷 | RPC 请求（对应 channel_call 请求半） |
| 7 | CALL_REPLY | 应用载荷 | RPC 应答，txid 匹配请求 |
| 8 | CLOSE | 空 | 端点关闭通告，dst_slot 指明哪个端点 |
| 9 | ACK | 4B cumulative_seq | 可靠档累计确认 |
| 10 | NACK | 4B missing_seq | 可靠档请求重传（可选优化，v0 可实现为不发） |
| 11 | ERROR | 4B errno + 4B orig_txid | 把远端错误映射回调用方（NOT_FOUND/UNSUPPORTED 等） |

## 3. flags

| 位 | 名称 | 含义 |
|---|---|---|
| 0 | RELIABLE | 本帧参与 seq/ACK 重传（协商两端都有 CAP_RELIABLE 才允许置位） |
| 1 | FRAGMENTED | 本帧是某消息的分片（frag 字段有效） |
| 2 | BROADCAST | dst_hash 为广播哈希（仅 SERVER 档 SEND） |
| 3 | COMPRESSED | v0 保留，必须 0 |

## 4. CALL 事务生命周期

```
调用方                                    服务方
  | -- CALL(txid=T, seq=s) --------------> |
  |    [启动 deadline 定时器]              | 路由/槽位查找失败 → ERROR(errno, T)
  | <------------- CALL_REPLY(txid=T) --- | 成功
  | -- ERROR / 超时 → errno 返回调用线程   |
```

- txid：per-endpoint 单调分配，回绕后跳过仍在飞行中的值。上限见 01-abi 限额"待确认未应答 CALL"，超限新调用返回 `A20_ERR_RESOURCE_LIMIT`（不得阻塞等待空位——v0 简化决定）。
- CALL 的可靠性：**RELIABLE 档靠 seq/ACK 保证 CALL 帧本身送达**；CALL_REPLY 同理。应用层语义仍是 at-most-once：服务方按 `(src_hash, txid)` 去重窗口（≥64 项）识别重传的 CALL，直接重发缓存的 REPLY，不重复执行。
- 超时：min(调用方 deadline, 30s)。超时后调用方发送方释放 txid；迟到的 REPLY 按 txid 查无事务 → 丢弃计数。

## 5. 分片与重组（RELIABLE 档专属）

- 触发：消息 > 传输 MTU。分片大小 = MTU - 32(头) - 2(CRC)。UDP 档 MTU 1472 → 片载荷 1438；64 KiB 消息 ≈ 46 片。
- 同一消息全部分片共享 txid，frag 序号从 0 递增，末片 bit15=1。允许乱序到达。
- 重组缓存：per-(src_hash,txid) 位图 + 拼接缓冲。全片到齐 → 重组为一条消息投递。超时（默认 5s，与 CALL deadline 取小）清缓存，调用方得 `A20_ERR_CLUSTER_TIMEOUT`。
- 缓存总量受限额（01-abi）；超限丢弃最旧半重组消息并计数 `reasm_evicted`。
- MCU 档：禁止分片（FRAGMENTED 帧直接丢弃计数），发送方消息 > MTU 时直接返回 `A20_ERR_INVALID_ARGS`。

## 6. 链路建立与版本协商

HELLO 载荷（定长 32 字节）：

```
0   node_id(16)
16  proto_min(1) proto_max(1) profile_tier(1) caps(1)
20  short_addr(2)   /* UART 档：头节点分配给叶子的 16 位短地址；其余档置 0 */
22  link_addr_len(1) reserved(1)
24  nonce(8)          /* 防回环：收到自己 nonce 的 HELLO 判定为自环，拒绝 */
```

状态机（每条链路独立）：

```
DISCONNECTED --发HELLO--> HELLO_SENT --收HELLO_ACK且校验通过--> UP
HELLO_SENT --超时×3--> DOWN（上报 LINK_DOWN 事件）
UP --PING 丢失≥3--> SUSPECT --再丢失≥3--> DOWN
SUSPECT --收PONG--> UP
DOWN --收新HELLO--> 重新协商
```

校验内容：proto 交集非空（取交集最大值为生效版本）、哈希冲突检查、nonce 非自环、caps 与档位一致（叶子不得置 RELAY）。任何失败 → ERROR 帧 + 状态到 DOWN。

心跳：UP/SUSPECT 期间每 1s 发 PING（MCU 档放宽到 5s，叶子可不主动发，只应答）。PONG 时戳差进 RTT 滑动平均（α=1/8）。

## 7. 可靠传输（RELIABLE 档）

- seq：每 (src,dst,transport) 链路单调，HELLO 重协商后从 0 重启。
- ACK 累计确认：`ACK(cumulative_seq=N)` 表示 ≤N 全收。捎带在任何反向帧的载荷前 4 字节（flags.bit0 置位时接收方先剥离），无反向流量时每 200ms 或每收 8 帧发裸 ACK，取先到。
- 重传：帧发出后未确认，RTO = max(200ms, 2×RTT 均值) 起，指数退避 ×2，上限 5s；最多 7 次后判链路 SUSPECT。
- 接收去重：每链路维护最近 256 个 seq 位图，重复帧丢弃但重发 ACK。
- NACK 为可选加速：发现 seq 空洞可发 NACK；v0 允许只实现 ACK+超时重传。

## 8. 关闭与错误映射

- 本地句柄释放 → 发 `CLOSE(dst_slot)`；对端代理端点调 `a20_channel_ep_peer_shutdown()`，后续本地调用返回 `A20_ERR_REMOTE_CLOSED`（语义对齐本机 `peer_closed`）。
- 链路 DOWN：所有该链路的在飞 CALL 以 `A20_ERR_NODE_UNREACHABLE` 唤醒；已建立的端点句柄不销毁（允许链路恢复后续用？——**v0 决定：不恢复**，链路 DOWN 即对端点发 peer_shutdown，调用方需重新 cluster_connect。理由：重协商后 seq/txid 状态全部重置，恢复旧端点语义太复杂）。
- ERROR 帧 errno 字段只使用 01-abi 定义的集群 errno；收到未知码按 `A20_ERR_CLUSTER_UNSUPPORTED` 处理。

## 9. 金样向量（互操作强制）

`tools/cluster-ref/`（WB1 产物）必须产出并维护：

1. 每种 type 至少 2 个合法帧的十六进制金样 + 解码结果（JSON）。
2. ≥20 个畸形帧（错 magic、payload_len 越界、保留位非零、frag 序号跳变、CRC 错误）及**期望行为**（丢弃/计数/ERROR）。
3. 一个完整事务脚本（HELLO→CALL→分片→REPLY→CLOSE）的帧序列。

内核解码器（WA）与 MCU 叶子（WC）都必须离线通过全部金样，再进入联调。这消除了"两端各写各的、联调互相甩锅"的最大风险。

**实现期核对（tools/cluster-ref，2026-10）**：金样的逐条判定细则（CRC 覆盖范围、
单片是否合法、ACK 捎带边界、TTL 到达语义、计数器口径等 17 条）登记在
`tools/cluster-ref/README.md` 的"假设登记簿"（A-01…A-17），代码中以 `ASSUMPTION A-nn`
就地标注；改动判定先改登记簿再改实现。本条与 §5 的一处口径落定：§9(2) 字面的
"frag 序号跳变"按 §5 "允许乱序到达"处理——缺片不是线格式错误，落在重组态
（`state-reasm-gap-01`：收下入位图，超时 `reasm_timeouts`）；frag 字段本身的违规
（无 FRAGMENTED flag、last 无 flag、单片 frag=0|last）由 `mal-frag-*` 三例钉住，
判 `frag_flag_mismatch` + `rx_malformed`。

## 10. 计数器（观测口径，进 `cluster_link_status` 与 klog）

每链路必须维护：`tx_frames rx_frames tx_drops rx_drops rx_malformed retransmits reasm_timeouts reasm_evicted dedup_drops hello_rejects`。新增计数器只追加不改名。


## 实现期记录（WA1 loopback 数据面，2026-10 按源码核对）

1. **v0 loopback 线上只出现四种 type**：CALL / CALL_REPLY / CLOSE / ERROR。
   SEND（单向消息）在 v0 没有发送方（远程 `channel_send` 同样编码为 CALL，
   02-§4 实现期登记），HELLO/PING/PONG/ACK/NACK 不由 loopback 发出（04-§5：
   loopback 无心跳、免分片、无需可靠层）；收到这些 type 判 rx_drops。
2. **CALL_REPLY 与 ERROR 的 `src_hash` = 服务方节点哈希**（即入站 CALL 的
   `dst_hash`），不是发送内核的 self 哈希。真实内核上两者相同；loopback 虚拟
   节点场景下回复必须声明虚拟节点身份，否则调用方按 (src_hash, txid) 的
   demux 永远失配（WA1 自检实机暴露并钉死）。§2 表格的"src_hash"语义由此
   细化为"**帧所声称的发送节点**"。
3. **CLOSE 双向投递**：CLOSE 的 `dst_slot` 从发送方视角命名"对端的导出端点"
   （02-§8 已据此规定本地句柄释放→CLOSE(dst_slot)）；接收内核把它同时交给
   proxy 侧（远端服务关了 → 端点拆解）与 export 侧（远端调用方走了 → 丢弃
   未应答的 (caller, txid) 配对）处理。两个方向各自只会在匹配的槽位上生效。
4. **回复配对假设**：服务方按 FIFO 应答（单线程 echo 服务模型），export 以
   每导出 8 项的 (caller, txid) FIFO 配对回复；并发多消费者服务模型需要
   per-request 关联机制，属于 v1 议题（WA1 在 03-§2 登记）。
5. **空载荷帧的编码规则**（WA1 终审 F1）：发送侧 `payload == NULL` 仅在
   `payload_len == 0` 时合法（CLOSE 恒空载；其余类型按 §2 载荷列）。WA1 终审前
   编码器一律拒绝 NULL 载荷，§8「本地句柄释放 → CLOSE(dst_slot)」因此从未真正
   上线；规则放宽后 CLOSE 实际发送，内核自检钉住 RX 两分支（proxy 侧端点拆解、
   export 侧丢弃未应答配对）。线格式本身不变——解码侧早已强制 CLOSE 载荷为 0。
6. **入站 CALL 的分发裁决**（WA1 终审 F3）：接收内核把 CALL 只投给导出表——
   CALL 若先经 proxy 在途事务 demux，其 `(src_hash, txid)` 会命中本机 proxy 的
   在途 CALL（connect 自身 node_id 时必然命中），按 CLOSE 分支自拆端点。
   CALL_REPLY/ERROR 只投 proxy；CLOSE 维持 #3 的双向投递。配套约束：proxy 只
   接受 `dst_hash == 本机 self 哈希` 的帧（回复/错误/CLOSE 的合法目的恒为调用方
   自身），发往其他托管身份（loopback 虚拟节点）的帧不再误中本机 proxy。
