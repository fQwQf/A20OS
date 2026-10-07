# 集群线协议参考（wire v0）

> **实现状态（2026-10-07 核对）**：本文描述的线格式（§1 帧布局、§2 消息类型、§3 flags、§6 HELLO 载荷与校验、§8 关闭与错误映射）**已在代码中实现**，权威编解码器是 `kernel/cluster/frame.c`（逐字段编解码 + 层 1 判决），并与 `tools/cluster-ref/` 的金样向量逐字节对拍通过。**未实现**：分片重组（§5）与可靠传输 seq/ACK（§7）是 **WA3 计划**（见 [impl-prompts.md](impl-prompts.md) 的 WA3 节），线上今天不会出现 FRAGMENTED/RELIABLE 帧；UDP 传输是 **WA2 计划**，跨机链路尚未存在，v0 实际跑这条协议的链路是 loopback（四种 type）与 UART（头节点 + MCU 叶子）。§10 计数器部分落地（明细见该节）。
>
> 本文是所有档位、所有传输共用的线格式唯一依据。改动线格式必须先改本文与 `tools/cluster-ref/` 的金样向量，再改实现。

## 这份协议今天在哪跑

| 链路 | 线上实际出现的 type | 代码 |
|---|---|---|
| loopback（虚拟节点，全档位） | CALL / CALL_REPLY / CLOSE / ERROR | `kernel/cluster/loopback.c`、`kernel/cluster/transport.c:248-313`（分发） |
| UART（头节点 ↔ MCU 叶子） | 上四种 + HELLO / HELLO_ACK / PING / PONG | `kernel/cluster/uart.c`（头节点）、`kernel/mcu/leaf.c`（叶子） |
| UDP | **未实现（WA2 计划）** | `kernel/cluster/udp.c` 尚不存在 |

HELLO/PING/PONG 只在 UART 链路上运行（loopback 无链路状态机，`transport.c:309-311` 收到这些 type 计 `rx_drops`）；SEND 在 v0 没有发送方（远程 `channel_send` 也编码为 CALL，见实现期记录 #1）；ACK/NACK 属于未实现的可靠层（§7）。

想亲手验证解码行为，不需要起内核：`tools/cluster-ref/` 里有全部帧的十六进制金样和一个主机侧对拍 runner，用法见 §9。

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

**在代码里**：布局常量与字段偏移在 `kernel/cluster/uart.h:67-89`（`A20_CLX_HDR_LEN`/`A20_CLX_MAGIC`/`A20_CLX_OFF_*`），小端访问器在 `uart.h:207-242`，CRC16-CCITT（CCITT-FALSE 参数化）在 `uart.h:255-271`，FNV-1a 节点哈希在 `uart.h:278-288`。这些是 header-only 原语，头节点与 MCU 叶子共用同一份，两端发出的帧逐字节一致。编码入口是 `a20_frame_encode()`（`kernel/cluster/frame.c:70`），解码入口是 `a20_frame_decode()`（`frame.c:122`）：解码器按固定判定顺序返回判决枚举（`frame.h:54-68`），每个判决字符串与参考实现 `clframe.py` 逐字一致，金样对拍靠这个对齐（§9）。两条硬规则的落点：`payload_len` 与实收长度核对在 `frame.c:176-178`；magic/ver 检查在 `frame.c:155-160`，判决失败由分发层计 `rx_malformed`（`transport.c:258-261`）。

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
| 9 | ACK | 4B cumulative_seq | 可靠档累计确认（**未实现，WA3 计划**） |
| 10 | NACK | 4B missing_seq | 可靠档请求重传（可选优化，v0 可实现为不发；**未实现，WA3 计划**） |
| 11 | ERROR | 4B errno + 4B orig_txid | 把远端错误映射回调用方（NOT_FOUND/UNSUPPORTED 等） |

类型常量：`kernel/cluster/uart.h:92-102`；各类型的定长载荷约束由 `a20_frame_fixed_payload_len()` 钉住（`frame.c:25-48`，HELLO/HELLO_ACK=32、PING/PONG=8、CLOSE=0、ACK/NACK=4、ERROR=8，不符判 `fixed_payload_len_mismatch`，`frame.c:211-213`）。

各 type 在今天的实现范围：

- **CALL / CALL_REPLY / CLOSE / ERROR**：全链路可用。loopback 与 UART 都跑；编码/分发见 §4、§8。
- **HELLO / HELLO_ACK / PING / PONG**：只在 UART 链路实现——头节点侧状态机在 `uart.c:557-600`（HELLO 重试、PING 周期、SUSPECT/DOWN 迁移），叶子侧被动应答在 `leaf.c:214-326`。loopback 不运行链路状态机。
- **SEND**：编解码与线格式完整（金样 `valid-send-*` 覆盖），但 v0 没有发送方——远程 `channel_send` 在 v0 编码为 CALL（实现期记录 #1）。
- **ACK / NACK**：**未实现（WA3 计划）**。线格式与判决规则已冻结并有金样（`valid-ack-*`/`valid-nack-*`），但内核没有产生或消费它们的代码。

## 3. flags

| 位 | 名称 | 含义 |
|---|---|---|
| 0 | RELIABLE | 本帧参与 seq/ACK 重传（协商两端都有 CAP_RELIABLE 才允许置位） |
| 1 | FRAGMENTED | 本帧是某消息的分片（frag 字段有效） |
| 2 | BROADCAST | dst_hash 为广播哈希（仅 SERVER 档 SEND） |
| 3 | COMPRESSED | v0 保留，必须 0 |

常量：`uart.h:105-108`。v0 内核**不会发出** RELIABLE/FRAGMENTED/BROADCAST 中任何一个：`cluster_connect` 带 `A20_CONNECT_RELIABLE` 一律拒绝（`remote_ep.c:467-472`，没有 `reliable.c`）；消息超 MTU 直接拒绝而不分片（`remote_ep.c:636-641`，见 §5）；BROADCAST 没有发送方。编解码器仍然在层 1 钉住 flag 与字段的一致性：FRAGMENTED 与 frag 字段必须互相印证（`frame.c:196-202`，单片 `frag=0|last` 判 `frag_flag_mismatch`），非 RELIABLE 帧 seq 必须为 0（`frame.c:205-206`）。COMPRESSED 按 §1 保留位规则**接受并忽略**（登记簿 A-16），发送侧则拒绝置位（`frame.c:87-88`）。

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

**v0 实际这么跑**（以代码为准）：

1. 用户在 `cluster_connect` 返回的普通 channel 句柄上做 `channel_call`；代理端点（`a20_clx_proxy_create()`，`remote_ep.c:461`）的 TX worker 从队列取出请求，分配 txid（**全局**单调计数器 `g_clx_txid_next`，`remote_ep.c:54,651`——全局单调蕴含 per-endpoint 单调；u32 回绕跳过在飞值的逻辑 v0 未实现，见实现期记录 #7），编码 CALL 帧发出（`remote_ep.c:652-664`），同时在代理的在飞表里登记 `(txid, deadline)`（`remote_ep.c:221-237`）。
2. deadline 是链路常量 30 s（`CLX_CALL_DEADLINE_MS`，`clx_internal.h:62`）——冻结的 channel ABI 没有 per-call deadline 字段，"min(调用方 deadline, 30s)" 退化为 30 s（01-abi 实现期记录 #6）。每 250 ms 的管家线程扫描超期的在飞事务并拆解代理（`remote_ep.c:425-447`，线程在 `transport.c:330-337`）。
3. 在飞上限（01-abi 限额"待确认未应答 CALL"，DEFAULT=64/ABSOLUTE=512/MCU=1）打在每代理的在飞表上；`a20_clx_proxy_inflight_add()` 满员即拒绝、不阻塞（`remote_ep.c:224-226`；内核自检报告项 `inflight over-limit -> NO_SPACE` 钉住这条，`kernel/cluster/selftest.c:520-540`）。上限命中时 worker 丢弃该请求并记 klog（`remote_ep.c:665-670`），等待中的调用线程按 01-abi #6 的"冻结 channel 表面"模型被唤醒——v0 对调用线程呈现为与本机 peer 关闭逐位一致的 `-A20_ERR_CANCELED`，精确原因进 klog。
4. 服务方一侧：入站 CALL 只投给导出表（`transport.c:303-304`；先经 proxy demux 会自拆端点，实现期记录 #6）；槽位不存在或导出是 LOCAL_ONLY → 回 `ERROR(NOT_FOUND)`（`export.c:340-348`）；服务 endpoint 已消失 → `ERROR(REMOTE_CLOSED)`（`export.c:371-385`）。请求以普通 channel 消息注入服务的接收队列（`export.c:379-380`），应答按每导出 8 项的 `(caller, txid)` FIFO 配对发出（`clx_internal.h:57`，`export.c:205` 起）。
5. 去重：服务方维护 64 项 `(src_hash, txid)` 窗口（`CLX_DEDUP_WINDOW`，`clx_internal.h:52`；`export.c:189-198`）。重复 CALL **丢弃**并计 `dedup_drops`（`export.c:350-355`）——注意 v0 内核侧**不重发缓存的 REPLY**，规范中的"重发缓存 REPLY"今天只在 MCU 叶子实现（单在飞事务 + 缓存应答，`leaf.c:397-402`）；内核侧"不重发"在 v0 无影响（没有重传源，见 §7），已登记为实现期记录 #7。
6. 迟到的 CALL_REPLY/ERROR（txid 查无在飞事务）丢弃并计 `rx_drops`（`remote_ep.c:351-356`）。

## 5. 分片与重组（RELIABLE 档专属）

> **未实现（WA3 计划，见 [impl-prompts.md](impl-prompts.md) WA3 节）**：`reasm.c` 尚不存在，内核没有任何重组缓存；下文是冻结的设计契约，WA3 按此实现。今天与分片相关的已实现行为只有两条：编解码器在层 1 校验 frag 字段与 FRAGMENTED flag 的一致性（`frame.c:196-202`），发送侧对超 MTU 消息直接拒绝（内核代理：丢弃 + klog，`remote_ep.c:636-641`；UART 传输：`-EMSGSIZE`，`uart.h:644-651` 的接口约定）。

- 触发：消息 > 传输 MTU。分片大小 = MTU - 32(头) - 2(CRC)。UDP 档 MTU 1472 → 片载荷 1438；64 KiB 消息 ≈ 46 片。
- 同一消息全部分片共享 txid，frag 序号从 0 递增，末片 bit15=1。允许乱序到达。
- 重组缓存：per-(src_hash,txid) 位图 + 拼接缓冲。全片到齐 → 重组为一条消息投递。超时（默认 5s，与 CALL deadline 取小）清缓存，调用方得 `A20_ERR_CLUSTER_TIMEOUT`。
- 缓存总量受限额（01-abi）；超限丢弃最旧半重组消息并计数 `reasm_evicted`。
- MCU 档：禁止分片（FRAGMENTED 帧直接丢弃计数），发送方消息 > MTU 时直接返回 `A20_ERR_INVALID_ARGS`。

设计意图简述：分片绑定 RELIABLE 档是因为乱序到达要靠 seq/ACK 体系兜底；MCU 档禁分片换来零重组内存。金样侧已有分片向量（`scenario_hello_call_close.txt` 的 [07]-[11] 步、畸形帧 `mal-frag-*`），WA3 落地时直接以它们验收。

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

**在代码里**：HELLO 载荷构造器是 `a20_clx_build_hello_payload()`（`uart.h:338-357`，头叶共用）。这条状态机的 v0 实现范围：

- **UART 头节点**（`kernel/cluster/uart.c`）：完整运行 DISCONNECTED→HELLO_SENT→UP→SUSPECT→DOWN，含 HELLO 重试（2 s × 3 次，`uart.h:190-191`）、5 s PING 周期（`uart.h:179`）、RTT α=1/8 滑动平均（`uart.c:468-469`）、HELLO 校验失败计 `hello_rejects`（`uart.c:339-381`）。
- **MCU 叶子**（`kernel/mcu/leaf.c`）：被动一侧——不主动发 HELLO，收到合法 HELLO 后应答 HELLO_ACK 并采纳短地址进入 UP（`leaf.c:214-306`），只应答 PING（`leaf.c:326`），50 s 无 PING 回 DISCONNECTED、短地址作废（`leaf.c:29-30,117-118`）。叶子的状态机只有 DISCONNECTED/UP 两态。
- **loopback**：不运行 HELLO/心跳（链路常 UP，`transport.c:483-488`），收到 HELLO 族帧计 `rx_drops`（`transport.c:309-311`）。
- **UDP**：**未实现（WA2 计划）**。`cluster_connect` 的 `timeout_ms` 参数在 v0 不被消费（01-abi 实现期记录 #13），WA2 的 UDP 建链落地时接入。

## 7. 可靠传输（RELIABLE 档）

> **未实现（WA3 计划，见 [impl-prompts.md](impl-prompts.md) WA3 节）**：`reliable.c` 尚不存在，没有 seq 分配、ACK 捎带、重传与去重的任何内核代码；下文是冻结的设计契约。今天唯一已落地的相关行为是否定性的：编解码器拒绝"非 RELIABLE 帧带非零 seq"（`frame.c:205-206`），`cluster_connect(A20_CONNECT_RELIABLE)` 无条件返回 `A20_ERR_CLUSTER_UNSUPPORTED`（`remote_ep.c:467-472`）。

- seq：每 (src,dst,transport) 链路单调，HELLO 重协商后从 0 重启。
- ACK 累计确认：`ACK(cumulative_seq=N)` 表示 ≤N 全收。捎带在任何反向帧的载荷前 4 字节（flags.bit0 置位时接收方先剥离），无反向流量时每 200ms 或每收 8 帧发裸 ACK，取先到。
- 重传：帧发出后未确认，RTO = max(200ms, 2×RTT 均值) 起，指数退避 ×2，上限 5s；最多 7 次后判链路 SUSPECT。
- 接收去重：每链路维护最近 256 个 seq 位图，重复帧丢弃但重发 ACK。
- NACK 为可选加速：发现 seq 空洞可发 NACK；v0 允许只实现 ACK+超时重传。

捎带拆分规则已冻结在登记簿（A-05/A-06：只在 RELIABLE 且非分片的 SEND/CALL/CALL_REPLY 上剥 4 字节 ACK 前缀），金样 `valid-ack-*`/`valid-nack-*` 与 scenario [12]-[14] 步已把 ACK、重传、去重计数钉成可判定的期望行为。

## 8. 关闭与错误映射

- 本地句柄释放 → 发 `CLOSE(dst_slot)`；对端代理端点调 `a20_channel_ep_peer_shutdown()`，后续本地调用返回 `A20_ERR_REMOTE_CLOSED`（语义对齐本机 `peer_closed`）。
- 链路 DOWN：所有该链路的在飞 CALL 以 `A20_ERR_NODE_UNREACHABLE` 唤醒；已建立的端点句柄不销毁（允许链路恢复后续用？——**v0 决定：不恢复**，链路 DOWN 即对端点发 peer_shutdown，调用方需重新 cluster_connect。理由：重协商后 seq/txid 状态全部重置，恢复旧端点语义太复杂）。
- ERROR 帧 errno 字段只使用 01-abi 定义的集群 errno；收到未知码按 `A20_ERR_CLUSTER_UNSUPPORTED` 处理。

**在代码里**：

- CLOSE 的发送：代理 TX worker 发现调用方半关闭（句柄释放）时发 `CLOSE(dst_slot)`（`remote_ep.c:618-628`，编码在 `clx_send_close()`，`remote_ep.c:208-216`）。空载荷合法化（`payload==NULL` 当且仅当 `payload_len==0`）在 `frame.c:76-80`——这条放宽前 CLOSE 从未真正上线，见实现期记录 #5。
- CLOSE 的接收：**双向投递**——proxy 侧拆解对应端点（`remote_ep.c:396-399`），export 侧丢弃该 caller 的未应答配对（`export.c:323-334`）；分发裁决在 `transport.c:305-307`。
- ERROR 的接收：代理按 `(src_hash, txid)` 找到在飞事务后拆解端点，精确 errno 进 klog（`remote_ep.c:384-394`）——对调用线程的呈现仍是 `-A20_ERR_CANCELED`（01-abi 实现期记录 #6）。
- ERROR 的发送：服务方槽位查找失败发 `ERROR(NOT_FOUND)`（`export.c:346`），服务消失发 `ERROR(REMOTE_CLOSED)`（`export.c:373,385`），编码器 `a20_clx_send_error()` 在 `remote_ep.c:188-202`。线上 errno 集合（24/26/27/28/29）定义在 `uart.h:140-144`；"未知码按 CLUSTER_UNSUPPORTED 处理"由金样 `mal-error-unknown-errno-01` 钉住（登记簿 A-08）。
- 链路 DOWN 唤醒在飞 CALL：UART 链路事件经 `a20_clx_link_event()` 上报（`transport.c:495-507`），节点级拆解入口是 `a20_clx_node_down()`（`remote_ep.c:402-421`）。loopback 无 DOWN 概念。

## 9. 金样向量：怎么用 tools/cluster-ref 做离线对拍

金样已产出并入库（WB1 交付），**今天的用法**是：改任何编解码代码后离线跑对拍，全绿才谈联调。三样产物对应原规范的三条强制要求：

1. 每种 type 至少 2 个合法帧的十六进制金样 + 解码结果（JSON）→ `tools/cluster-ref/vectors/valid/`（30 个，11 种 type 全覆盖）。
2. 畸形帧 + 期望行为 → `vectors/malformed/`（49）+ `vectors/reserved/`（7，保留位**接受**分支）+ `vectors/state/`（6，需带状态接收方的计数）+ `vectors/slip/`（16，SLIP 变体）。`vectors/MANIFEST.json` 是机器可读清单（共 108 条，其中 1 条抽象向量无 hex 文件），`vectors/INDEX.md` 是人读索引。
3. 完整事务脚本（HELLO→CALL→分片→REPLY→CLOSE）→ `tools/cluster-ref/scenario_hello_call_close.txt`，17 步，含 PING/PONG、裸 ACK、重传、坏帧、重组超时、ERROR 与 UART/SLIP 附录。

**跑对拍**（主机侧，不需要内核，2026-10-07 实跑验证）：

```bash
cd tools/cluster-ref
python3 selftest.py            # 参考实现自测：原语/往返/向量再生/脚本复核 → all green (503 checks)
python3 check_c_side.py        # 编译 refdec.c 并对拍全部金样 → all green (1889 checks over 91 frame + 16 slip vectors)
python3 check_c_side.py --decoder ./your_decoder   # 对拍你自己的内核/叶子解码器
python3 check_c_side.py --mode slip                # 只跑 SLIP 向量
python3 check_c_side.py --filter valid-call        # 按 id 子串过滤
python3 check_c_side.py --print valid-call-01      # 看单个向量的解码输出
```

被测解码器是一个**独立可执行程序**（不链接 Python）：`<decoder> <file.hex> <mtu>` 解码单帧、`<decoder> --slip <file.slip.hex>` 解 SLIP 流、`<decoder> --selftest` 锚点自检。约定（`check_c_side.py:13-34`，细则见 `tools/cluster-ref/README.md`）：

- 退出码：0=接受，1=拒绝（有判决字符串，malformed 向量以 `reason=` 为准），2=用法错误。
- 输出是每行一个 `key=value`：强制键 `wire_len`/`accept`/`reason`；接受时加全部头部字段与 `payload_hex`；HELLO 向量再比 §6 的十个载荷字段。
- 判决字符串必须与 `clframe.py` 的 R_*/D_*/H_* 表逐字一致——内核解码器的 `a20_frame_verdict_str()`（`frame.c:50-68`）返回的就是这些字符串，所以 `check_c_side.py --decoder` 可以直接对拍包了一层 CLI 的内核 `a20_frame_decode()`。
- `mtu` 参数是**接收方**传输的 MTU：UDP 1472、UART 256、loopback 65536（角色表在 `check_c_side.py:51-61`）。

向量是**生成物**：改协议先改 `clframe.py` 与 `gen_vectors.py`/`gen_scenario.py`，再重新生成；`selftest.py` 会把向量整个重生成一遍与提交树逐字节比对，漂移即失败。畸形帧的逐条判定细则（CRC 覆盖范围、单片是否合法、ACK 捎带边界、TTL 到达语义、计数器口径等 17 条）登记在 `tools/cluster-ref/README.md` 的"假设登记簿"（A-01…A-17），代码中以 `ASSUMPTION A-nn` 就地标注；改动判定先改登记簿再改实现。§9(2) 字面的"frag 序号跳变"按 §5"允许乱序到达"处理——缺片不是线格式错误，落在重组态（`state-reasm-gap-01`：收下入位图，超时 `reasm_timeouts`）；frag 字段本身的违规（无 FRAGMENTED flag、last 无 flag、单片 frag=0|last）由 `mal-frag-*` 三例钉住，判 `frag_flag_mismatch` + `rx_malformed`。

## 10. 计数器（观测口径，进 `cluster_link_status` 与 klog）

每链路必须维护：`tx_frames rx_frames tx_drops rx_drops rx_malformed retransmits reasm_timeouts reasm_evicted dedup_drops hello_rejects`。新增计数器只追加不改名。

**今天的暴露面**（与上表名字一一对应，2026-10-07 核对）：

- 十个名字的权威定义是 `a20_clx_link_counters_t`（`uart.h:147-158`），UART 链路状态 `a20_clx_uart_get_status()` 全量携带（`uart.h:693-704`）。
- 内核核心的每链路统计结构 `clx_link_stats_t`（`transport.c:49-58`）维护其中 6 个：`tx_frames/rx_frames/tx_drops/rx_drops/rx_malformed/retransmits`（`retransmits` 字段存在但 **v0 无产生者**——重传是 WA3）。
- `cluster_link_status` 的冻结输出结构只带 5 个计数器 + `state`/`rtt_us`/`last_hello_age_ms`（`kernel/include/abi/native/types.h:1203-1218`，填充在 `transport.c:417-491`；`node_id=LOCAL` 汇总全部链路）。`rx_malformed`、`dedup_drops`、`hello_rejects` 不在 ABI 输出里——UART 链路经 `a20_clx_uart_get_status()` 可见，其余进 klog。
- `dedup_drops` 的产生者是导出侧去重窗口（全局计数，`export.c:26,350-351`）与 MCU 叶子（`leaf.c:400`）；`hello_rejects` 的产生者是 UART 头节点 HELLO 校验（`uart.c:339-381`）与叶子（`leaf.c:272`）。
- `reasm_timeouts`/`reasm_evicted` **未实现（WA3 计划）**：没有重组缓存就没有产生者；金样（`state-reasm-gap-01` 等）已把期望口径钉好。

金样对计数器的覆盖：7 个计数器由单向量钉住（MANIFEST `counts.counters_exercised`），`tx_frames`/`retransmits`/`reasm_timeouts` 需要帧序列，由 scenario 文件的计数小结钉住。

## 实现期记录（WA1 loopback 数据面，2026-10 按源码核对；2026-10-07 复核）

1. **v0 loopback 线上只出现四种 type**：CALL / CALL_REPLY / CLOSE / ERROR。
   SEND（单向消息）在 v0 没有发送方（远程 `channel_send` 同样编码为 CALL，
   02-§4 实现期登记），HELLO/PING/PONG/ACK/NACK 不由 loopback 发出（04-§5：
   loopback 无心跳、免分片、无需可靠层）；收到这些 type 判 rx_drops
   （分发处 `transport.c:308-312`）。
2. **CALL_REPLY 与 ERROR 的 `src_hash` = 服务方节点哈希**（即入站 CALL 的
   `dst_hash`），不是发送内核的 self 哈希。真实内核上两者相同；loopback 虚拟
   节点场景下回复必须声明虚拟节点身份，否则调用方按 (src_hash, txid) 的
   demux 永远失配（WA1 自检实机暴露并钉死）。§2 表格的"src_hash"语义由此
   细化为"**帧所声称的发送节点**"（回复编码处 `remote_ep.c:745-754`）。
3. **CLOSE 双向投递**：CLOSE 的 `dst_slot` 从发送方视角命名"对端的导出端点"
   （02-§8 已据此规定本地句柄释放→CLOSE(dst_slot)）；接收内核把它同时交给
   proxy 侧（远端服务关了 → 端点拆解，`remote_ep.c:396-399`）与 export 侧
   （远端调用方走了 → 丢弃未应答的 (caller, txid) 配对，`export.c:323-334`）
   处理。两个方向各自只会在匹配的槽位上生效。
4. **回复配对假设**：服务方按 FIFO 应答（单线程 echo 服务模型），export 以
   每导出 8 项的 (caller, txid) FIFO 配对回复（`clx_internal.h:57`）；并发多
   消费者服务模型需要 per-request 关联机制，属于 v1 议题（WA1 在 03-§2 登记）。
5. **空载荷帧的编码规则**（WA1 终审 F1）：发送侧 `payload == NULL` 仅在
   `payload_len == 0` 时合法（CLOSE 恒空载；其余类型按 §2 载荷列）。WA1 终审前
   编码器一律拒绝 NULL 载荷，§8「本地句柄释放 → CLOSE(dst_slot)」因此从未真正
   上线；规则放宽后（`frame.c:76-80`）CLOSE 实际发送，内核自检钉住 RX 两分支
   （proxy 侧端点拆解、export 侧丢弃未应答配对）。线格式本身不变——解码侧早
   已强制 CLOSE 载荷为 0。
6. **入站 CALL 的分发裁决**（WA1 终审 F3）：接收内核把 CALL 只投给导出表——
   CALL 若先经 proxy 在途事务 demux，其 `(src_hash, txid)` 会命中本机 proxy 的
   在途 CALL（connect 自身 node_id 时必然命中），按 CLOSE 分支自拆端点。
   CALL_REPLY/ERROR 只投 proxy；CLOSE 维持 #3 的双向投递（`transport.c:301-312`）。
   配套约束：proxy 只接受 `dst_hash == 本机 self 哈希` 的帧（回复/错误/CLOSE 的
   合法目的恒为调用方自身，`remote_ep.c:340-343`），发往其他托管身份
   （loopback 虚拟节点）的帧不再误中本机 proxy。

**2026-10-07 复核追加**（文档改写为参考手册时按代码现状补记；均为口径澄清，不改线格式）：

7. **§4 两处与代码的口径差**：
   a. txid 是**全局**单调分配（`remote_ep.c:54,651`），不是字面的
      "per-endpoint 单调"——全局单调蕴含 per-endpoint 单调，语义兼容；u32
      回绕后"跳过仍在飞行中的值"的检查 v0 未实现（回绕需 2^32 个 CALL，
      v0 接受此简化）。
   b. "识别重传的 CALL，直接重发缓存的 REPLY"在 v0 内核侧只兑现了前半：
      64 项 `(src_hash, txid)` 去重窗口存在（`export.c:189-198`），重复 CALL
      丢弃并计 `dedup_drops`（`export.c:350-355`），但**不重发缓存的应答**
      （v0 无重传源，重复只可能来自对端异常）。MCU 叶子按规范全量兑现
      （单在飞事务 + 缓存应答重发，`leaf.c:397-402`）。WA3 引入重传后内核
      侧需补缓存应答重发，否则 at-most-once 语义退化为 at-most-once-执行
      +可能丢答。
8. **§10"进 cluster_link_status"的实际暴露面**：冻结 ABI 输出只带 5 个计数器
   （`types.h:1212-1216`），十个计数器名的完整集合在 `a20_clx_link_counters_t`
   （`uart.h:147-158`），经 UART 链路状态查询暴露；`retransmits` 有字段无产生者、
   `reasm_timeouts`/`reasm_evicted` 连字段级产生者都没有（均为 WA3）。
   §10 正文已按此改写，计数器**名字与语义不变**。
9. **§9 改写为使用说明**：金样三产物已入库并实跑通过——2026-10-07 在
   `tools/cluster-ref/` 执行 `python3 selftest.py`（all green，503 checks）与
   `python3 check_c_side.py`（all green，1889 checks over 91 frame + 16 slip
   vectors，含 refdec.c 自检 3 个锚点）。MANIFEST 计数：108 条向量 = valid 30 +
   malformed 49 + reserved 7 + state 6 + slip 16（其中 1 条抽象向量无 hex，
   对拍执行 107 条）。
