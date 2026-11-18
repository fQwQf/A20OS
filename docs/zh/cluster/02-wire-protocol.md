# CL 线协议：v0 基础帧与 v1 会话事务

v0 编解码和金样保留兼容。可靠会话扩展使用协商后的 v1，不复用 v0 设计中未实现的 ACK 捎带载荷语义。`frame.c` 的严格 v0 入口用于旧金样对拍，运行时入口允许 v0/v1，传输适配器再检查对端协商版本。

## 1. 帧布局

所有多字节字段使用显式小端，不能直接序列化 C 结构体。

| 偏移 | 字段 | 字节数 | 约束 |
|---:|---|---:|---|
| 0 | magic | 2 | `0x4c43`，线上字节 `43 4c` |
| 2 | ver | 1 | 0 或协商后的 1 |
| 3 | type | 1 | 下表中的 1–11 |
| 4 | flags | 2 | 低 4 位已定义 |
| 6 | frag | 2 | 低 12 位片号，bit15 末片，bit12–14 保留 |
| 8 | txid | 4 | CALL 事务号；0 保留；SEND 必须为 0 |
| 12 | src_hash | 4 | 完整 node_id 的 FNV-1a32 |
| 16 | dst_hash | 4 | 目标 node_id 的 FNV-1a32 |
| 20 | dst_slot | 4 | 服务槽；可靠 v1 的 0 为完成收据 |
| 24 | seq | 4 | 非 RELIABLE 必须为 0 |
| 28 | payload_len | 2 | 不包含帧头与 CRC |
| 30 | ttl | 1 | 默认 8；转发逐跳减 1；耗尽拒绝 |
| 31 | csum_kind | 1 | 0=无尾部 CRC，1=CRC16-CCITT |
| 32 | payload | 可变 | 精确等于 payload_len |
| 后续 | crc | 0 或 2 | 按 csum_kind 决定 |

CRC16-CCITT-FALSE：多项式 `0x1021`，初值 `0xffff`，不反射，不做末异或。覆盖头与载荷；`123456789` 校验值为 `0x29b1`。单帧载荷预算始终按 `MTU - 32 - 2` 计算，即使未携带 CRC 也不增加预算。 UART 与当前 UDP 控制/数据发送均携带 CRC。

接收顺序为最小长度、magic、版本、type、校验种类、MTU、实收长度、CRC、 frag/flag 一致性、seq/flag 一致性、SEND 的 txid、固定载荷长度。层 1 格式错误计 malformed，TTL/未知身份/策略拒绝计 drops。保留位接收兼容策略与 v0 金样保持一致；发送者必须清零。

## 2. 消息类型

| type | 名称 | 载荷 |
|---:|---|---|
| 1 | HELLO | 固定 32 B |
| 2 | HELLO_ACK | 固定 32 B |
| 3 | PING | 固定 8 B |
| 4 | PONG | 原样回显 PING 的 8 B |
| 5 | SEND | v1 会话前缀 + 单向业务；需 ONEWAY 能力 |
| 6 | CALL | v0 业务载荷，或 v1 会话前缀 + 业务载荷 |
| 7 | CALL_REPLY | v0 业务载荷，或 v1 回复前缀 + 业务载荷 |
| 8 | CLOSE | 空 |
| 9 | ACK | 固定 4 B；v1 为被接纳的单帧 seq |
| 10 | NACK | 固定 4 B；当前不发送 |
| 11 | ERROR | 固定 8 B：u32 errno + u32 orig_txid |

v0 caller 释放代理发 CLOSE，`txid=0xffffffff` 标识调用方关闭；接收方不能因此关闭同节点同 slot 的反向代理，也不能删除共享服务的 FIFO 配对。`txid=0` 保留历史服务关闭处理。可靠事务编号跳过 0 和 `0xffffffff`。

## 3. flags 与能力

| 位 | 名称 | 当前行为 |
|---:|---|---|
| 0 | RELIABLE | v1 有界会话事务；双方版本/能力必须支持 |
| 1 | FRAGMENTED | v1 协商的可靠 CALL/REPLY/SEND 分片，详见第 8 节 |
| 2 | BROADCAST | 当前适配器拒绝 |
| 3 | COMPRESSED | 保留；发送拒绝，v0 层 1 接收兼容 |

v0 的 seq/累计 ACK/捎带 ACK 方案保留在历史参考语料中，当前运行时不把它当作可用可靠服务。旧 peer 只公布版本 0 时，可以继续非可靠 CALL，不能建立`A20_CONNECT_RELIABLE` 连接。

## 4. HELLO 与会话

HELLO 本身用 v0 基础帧，32 B 载荷：

| 偏移 | 字段 |
|---:|---|
| 0 | node_id[16] |
| 16 | proto_min u8 |
| 17 | proto_max u8 |
| 18 | resource_profile u8：1/2/3 |
| 19 | caps u8：RELAY=1、RELIABLE=2、LEAF=4、FRAGMENT=8（v1 主机传输特性）、ONEWAY=16（可选单向业务） |
| 20 | short_addr u16 |
| 22 | link_addr_len u8：UART=2、UDP=6 |
| 23 | reserved u8 |
| 24 | session_nonce u64 |

当前发布版本区间 0–1。身份不能全零、全 ff 或等于本机；完整 ID 和源哈希必须一致；版本区间必须有交集；LEAF 与 RELAY 不能同时声明。资源档不限制角色为某种处理器。FNV 哈希冲突由路由表拒绝。

nonce 在一个会话内保持稳定，重试 HELLO 不更换 nonce。启动 epoch 必须来自可靠熵或平台持久计数，不能把固定启动 tick 当作跨断电唯一性的证明。 UDP 主机使用内核 RNG；STM32 已接入两页持久 epoch journal，物理写入验证仍待完成。

UDP 使用 HELLO.txid 作为本方 challenge，HELLO_ACK.txid 回显它。只有当前 challenge 的应答才能更新对端 epoch，旧 ACK 不得使会话回退。UART 使用双向 challenge，收到 HELLO 先应答，再用本方 HELLO 确认当前对端；只有匹配当前 challenge 的 ACK 能更新会话。v0 兼容连接不能替换已经建立的 v1 会话。实现和掉电/重放测试参见 [MCU 共享内存内核规范](mcu-shared-kernel.md) 及 [传输层适配规范](04-transports.md)。

## 5. v1 可靠 CALL

`ver=1, flags.RELIABLE=1, seq=txid`，单帧 CALL 载荷：

```text
u64 caller_session
u64 server_session
byte application_payload[]
```

接收方先校验双方会话，然后预留事务记录和最大单帧回复缓存；成功后才提交序号窗口并注入服务。缓存/队列不足时返回带会话的错误回复，不执行请求。事务键为来源身份、双方会话和 txid。相同 key 的重复 CALL：正在执行则确认接纳，已经完成则重发原回复，不能重新执行。

v1 ACK 的 payload 是本次 CALL 的 seq。它只证明请求已接纳，不能结束业务等待。当前单帧实现不需要累计 ACK，也不捎带 ACK。

## 6. v1 回复与完成收据

CALL_REPLY 载荷：

```text
u64 server_session
u64 caller_session
u32 status                 // 0=业务回复；非零=A20 错误码
byte application_reply[]   // status 非零时为空
```

前缀由代理剥离，正常服务和调用者仍看到原业务消息。业务自己的状态码可继续放在业务载荷里，与这里的数据面错误区分。

调用者接收回复，或关闭仍在途的 endpoint 放弃结果时，发送收据：v1 RELIABLE CALL，`dst_slot=0, seq=txid`，载荷仅 16 B 原会话对。语义是调用者不再需要结果，不是取消远端业务。主机若仍在执行，保留其预留记录直至 worker 完成才允许复用；放弃通知先于原请求到达时，把该序号提交到去重窗口，迟到请求不能再执行。服务方可复用已完成且已获收据的回复存储；独立序号窗口保留去重信息。已经退休或过旧的请求明确拒绝，不能为了腾空间再次执行。

主机有 64 项有界回复表；MCU 单回复缓存。MCU 没收到完成收据且缓存仍在保留期内时，拒绝新请求为 NO_SPACE。应用必须按协商容量分配并发。发送代理对 LEAF 的所有可靠连接合计只允许一个在途；未接纳 NO_SPACE 会重发最近一个绑定会话的退休收据，以恢复收据丢失造成的单槽占用。对声明 ONEWAY 的新实现，发送方还约束同一对端所有代理的未完成编号跨度小于 64。新编号前进至少 64 时，接收方可复用该来源已经完成的旧缓存，即使显式退休收据丢失；运行中的记录和其他来源记录不按此规则回收。

## 7. 重试、期限与保证范围

发送方保留完整请求副本。初始重试间隔 250 ms，指数增加到 2 s，每次调用最多重试 8 次，且固定期限不超过 30 s。任一上限触发即取消代理并唤醒所有等待者。ACK 不停止业务恢复重试，丢回复会触发缓存重放。

服务缓存保留期为 35 s，大于调用期限。采用 64 位序号位图及 32 位 serial 比较，允许窗口内乱序，拒绝过旧未见请求。调用编号回绕跳过保留值与在途号。序号比较要求保留期内不能跨越半个 32 位序号空间。

普通业务单向 SEND 扩展保持 wire `txid=0`，以非零 seq 共享可靠窗口。成功完成回执没有业务回复体，只确认接纳；格式、能力协商和队列背压参见 [运维指南](10-operator-guide.md)。默认未启用的部署不增加 HELLO 能力位。

保证仅覆盖当前会话与有界保留/序号窗口，不是跨断电 exactly-once。作业层仍须对 `(job_id, shard_id)` 去重，并显式声明是否允许重派。

## 8. 分片、转发与错误

v0 格式金样保持不变；v1 主机 UDP 已接入协商分片、有界重组和首片期限。跨不同 MTU 网关的重分片仍未实现，出口容纳不下原片时明确丢弃。 RELAY 转发已实现 TTL 递减和 CRC 更新；CS/1 双向挑战确认间接目的身份、 boot epoch 和路径 MTU。完整 UDP 路径可协商多跳分片；经过 UART 或未协商分片的路径使用单帧预算（参见 [传输层适配规范](04-transports.md)）。

v0 ERROR 保留固定 errno/orig_txid 布局。v1 接纳后的错误通过带会话的 CALL_REPLY.status 返回。旧/错误会话数据丢弃，不能唤醒或完成新会话事务。现有 Native channel 将远程失败统一表现为 CANCELED；精确错误接口待扩展。

## 9. 可复现协议证据

```bash
python3 tools/cluster/check-cluster-unit.py
python3 tools/cluster/check-cluster-udp.py --faults --restart
```

前者运行生产编解码、UART/MCU 配对、序号回绕测试及原 v0 金样；后者在两套独立内核之间丢请求/回复/ACK/完成收据、重复/延迟帧、全丢包直到重试耗尽，重启一端再注入旧 HELLO_ACK 与旧 CALL。测试验证与运行详情参见 [10-operator-guide.md](10-operator-guide.md)。
