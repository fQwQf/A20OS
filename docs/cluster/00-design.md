# A20OS 集群子系统：设计参考与使用入口（v0）

> **状态横幅（2026-10-07 按源码核对）**：本文描述的六个集群 syscall、内核数据面（帧编解码、路由表、导出表、传输注册与分发、loopback 虚拟节点、远程端点代理）、UART 传输（头节点侧）、MCU 叶子协议面、内核自检（`clxselftest=1`）、线协议参考实现与金样对拍设施**已实现、可使用**。UDP 传输（跨机 `channel_call`）、可靠性（分片重组/seq/ACK/重传/去重）、用户态 clusterd/jobd、生产硬化（故障注入矩阵/模糊/soak/性能实测）**未实现**——涉及它们的段落均带「未实现/计划」标注，实施计划见 [impl-prompts.md](impl-prompts.md) 的 WA2/WA3/WD/WE 轨道。汇总见 §13「实现状态与路线图」。

本文是集群子系统的顶层入口：先讲现在能用哪些、怎么用（§1），再讲整体架构与设计原理（§2–§12），最后给实现状态、路线图与落地偏差记录（§13–§16）。字段级规范在 01/02/03/04 各文档，本文不重复字段表，只给契约要点与代码指针。

## 1. 现在能用哪些、怎么用

### 1.1 六个集群 syscall（全部已实现）

号段 `0x0520`–`0x0525`（`kernel/include/abi/native/syscall_nr.h:103`-`:108`），ABI 入口在 `kernel/abi/native/sys_native_cluster.c`。参数风格与本机 ABI 一致：单个 `*_args_t` 结构体传入（`kernel/include/abi/native/types.h:1152`-`:1218`），返回 `int64_t`，负值为 `-errno`。

| syscall | 语义 | 实现 |
|---|---|---|
| `cluster_set_self(node_id, caps)` | 注入本节点 128 位 ID 与能力位（`A20_CLUSTER_CAP_RELAY/RELIABLE/LEAF`，`types.h:1110`-`:1115`）。仅成功一次，第二次 `-A20_ERR_EXISTS`；需管理权限 | `sys_native_cluster.c:89` → `kernel/cluster/route.c:54` |
| `cluster_export(ch_handle, service_name, flags) → slot` | 把一个本机 channel 端点导出为集群可寻址，返回单调不复用的 32 位 slot；名长上限 `A20_CLUSTER_SERVICE_NAME_MAX = 64`（`types.h:1148`）。需先 `set_self`，需管理权限 | `sys_native_cluster.c:115` → `kernel/cluster/export.c:36` |
| `cluster_connect(node_id, slot, timeout_ms) → ch_handle` | 返回类型为 `A20_OBJ_CHANNEL_ENDPOINT` 的普通句柄，之后用既有 `channel_call`/`channel_send` 通信。`node_id` 全零（`A20_NODE_ID_LOCAL`）时走本机快路径，按 slot 或服务名解析（`kernel/cluster/export.c:254`）；非本机时按 slot 建远程代理（`kernel/cluster/remote_ep.c:461`），要求路由已存在，否则 `-A20_ERR_NODE_UNREACHABLE`。远程按名解析**未实现**（返回 `-A20_ERR_CLUSTER_UNSUPPORTED`，`sys_native_cluster.c:227`-`:230`，属 clusterd 职责） | `sys_native_cluster.c:166` |
| `cluster_route(node_id, transport_id, next_hop, op, metric)` | 增/删/替换路由表项（`A20_ROUTE_ADD/DEL/REPLACE`，`types.h:1126`-`:1129`；`transport_id`：0=loopback、1=udp、2=uart，`types.h:1144`-`:1146`；`next_hop` 为 ≤16 字节的链路地址，解释归传输）。需管理权限 | `sys_native_cluster.c:256` → `kernel/cluster/route.c:130` |
| `cluster_event_subscribe(mask) → eventq_handle` | 返回普通 EventQ；链路 up/down、路由失效、导出被撤四类事件（`A20_CLX_EVENT_*`，`types.h:1131`-`:1136`）经 `a20_event_notify` 投递，event code = `32 + kind`（`kernel/cluster/clx_internal.h:64`-`:72`）。无权限要求 | `sys_native_cluster.c:298` |
| `cluster_link_status(node_id) → status` | 查询链路状态与计数（`state/rtt_us/tx_frames/rx_frames/tx_drops/rx_drops/retransmits/last_hello_age_ms`，`types.h:1203`-`:1218`）；`node_id` 全零时汇总全部链路。无权限要求 | `sys_native_cluster.c:334` → `kernel/cluster/transport.c:417` |

使用顺序（一个最小远程调用，loopback 传输）：

1. 管理方（euid 0，见 §6 权限注记）调 `cluster_set_self` 注入本节点 ID——未注入前一切远程操作返回 `-A20_ERR_NODE_UNREACHABLE`（`sys_native_cluster.c:133`-`:134`、`:225`-`:226`）。
2. 服务端 `channel_create` 后调 `cluster_export` 拿到 slot。
3. 管理方调 `cluster_route` 为目的节点注入「transport + next_hop」（loopback 的 next_hop 是 4 字节虚拟节点号，`kernel/cluster/loopback.h:12`-`:16`）。
4. 客户端 `cluster_connect(node_id, slot)` 得到普通 channel 句柄，之后 `channel_call`/`channel_send` 与本机用法完全一致。
5. 运维面：`cluster_event_subscribe` 收链路事件，`cluster_link_status` 读计数。

调用方须知的三条失败语义（与代码一致）：

- 远程 `channel_call` 的在途 deadline 固定 30 s（`CLX_CALL_DEADLINE_MS`，`kernel/cluster/clx_internal.h:59`-`:62`）；在途数超限立即返回 `-A20_ERR_NO_SPACE`，不阻塞（`clx_internal.h:224`-`:226`）。
- 远程关闭、链路 down、deadline 到期、对端 ERROR 在冻结的 channel API 上统一呈现为「对端关闭」式唤醒（syscall 返回 `-A20_ERR_CANCELED`，与本机 peer close 逐位一致）；精确原因进 klog 与计数（`kernel/cluster/remote_ep.c:39`-`:45`，登记于 01-abi/02 实现期记录）。
- 远程端点上携带 handle 的发送被拒为 `-A20_ERR_CLUSTER_UNSUPPORTED`（类型检查点 `A20_CHAN_TYPE_REMOTE`，`kernel/include/ipc/ipc.h:166`-`:172`；ABI 层映射 `sys_native_cluster.c:78`-`:85`）。

### 1.2 跑内核自检（clxselftest=1）

内核自检在 `kernel/cluster/selftest.c`，由内核命令行 `clxselftest=1` 门控（`selftest.c:692`；引导挂点 `kernel/main.c:215`-`:222`）。riscv64 上命令行经 QEMU `-append` 进 FDT `/chosen/bootargs`（`kernel/arch/riscv64/platform/fdt.c:301`-`:318`）。自检在同一内核内用 loopback 虚拟节点 B 驱动 WA1 验收清单：A/B echo 往返、connect(LOCAL/self)、link_status 汇总、export REPLACE 及其旧 worker 竞态、handle 拒绝、在途超限、CLOSE 两分支、远程 CLOSE 拆解、deadline 扫描等（完整清单见 `selftest.c:1`-`:45` 文件头）。每例打印 `[CLX-TEST] <name>: PASS/FAIL`，汇总行 `clx-selftest: <n>/<total>`。不携带该 bootarg 时自检完全惰性（一次 `strstr` 开销，`kernel/main.c:215`-`:222`）。

### 1.3 离线对拍：参考实现 + 金样向量

`tools/cluster-ref/` 是主机侧线协议参考实现与金样，不依赖内核代码：

- `clframe.py`：唯一权威参考编解码器；`refdec.c`：C 语言第二实现；`vectors/`：108 条金样（valid 30 / malformed 49 / reserved 7 / state 6 / slip 16，机器可读清单 `vectors/MANIFEST.json`，人工索引 `vectors/INDEX.md`）。
- 对拍内核解码器：`python3 tools/cluster-ref/check_c_side.py --decoder <你的解码器>`；`check_c_side.py` 默认对拍 `refdec.c`。内核侧解码器 `kernel/cluster/frame.c`（`a20_frame_encode` `frame.c:70`、`a20_frame_decode` `frame.c:122`）的判决字符串与 `clframe.py` 逐字节一致（`kernel/cluster/frame.h:19`-`:22`、`:49`-`:68`），host 侧经 `tools/tests/test_clx_frame.c` 直接编译同一份源码参与对拍。
- 回归：`python3 tools/cluster-ref/selftest.py` 会把向量整个重生成并与提交树逐字节比对，漂移即失败；改协议先改 `clframe.py` 与生成器再重新生成（`tools/cluster-ref/README.md`「快速开始」）。
- `make host-tests`（`tools/targets-gates.mk:90`）覆盖三个集群 host 测试：`test_clx_frame.c`（帧编解码 + 金样 CLI）、`test_clx_uart.c`（SLIP/UART 编解码）、`test_mcu_leaf.c`（叶子协议面）。

### 1.4 双实例 L2 环境（WA2 的验收环境，跨机 channel_call 尚不可用）

`instances/qemu-riscv64-cluster-a.toml` / `-b.toml` + `tools/cluster-net-up.sh` 把两个 QEMU 实例接到同一条 L2 段（socket UDP 隧道，10.0.3.2/24 ↔ 10.0.3.3/24）并证明双向 ICMP 可达：

```bash
tools/cluster-net-up.sh                 # 构建（如缺产物）+ 起双实例 + A ping B + B ping A
tools/cluster-net-up.sh --skip-build    # 产物已就绪时跳过构建
tools/cluster-net-up.sh --keep          # 验证后保留两个 guest
```

注意：这一环境**不含任何 `kernel/cluster/` 数据面行为**——内核 UDP 传输 `kernel/cluster/udp.c` 尚未实现，文件仍是 WA2 规划项，跨机 `cluster_connect` + `channel_call` 今天跑不通。拓扑、端口、选型理由与手动检查方法见 [02-udp-demo.md](02-udp-demo.md)。

### 1.5 UART 传输与 MCU 叶子

- 头节点侧：`kernel/cluster/uart.c` 实现 UART 传输（transport_id=2）：SLIP 变体分帧（END=0xC0 首尾各一、ESC 转义，`kernel/cluster/uart.h:170`-`:174`）、MTU 256（`uart.h:165`，单帧载荷上限 222，`uart.h:167`）、50 ms 字节间静默重同步（`uart.h:177`）、2 字节短地址（未分配 `0xFFFF`，`uart.h:168`）、5 s PING 心跳（`uart.h:179`）。DEFAULT 档起由核心初始化注册进传输表（`kernel/cluster/transport.c:392`-`:404`）；链路级 API：`a20_clx_uart_link_register`（`uart.c:618`）、`a20_clx_uart_send_frame`（`uart.c:678`）、`a20_clx_uart_poll_all`（`uart.c:753`）。未注册链路时传输保持空闲。
- 叶子侧：`kernel/mcu/leaf.c` 是被答应答面——HELLO 被动应答 HELLO_ACK（头节点分配短地址）、PING→PONG、CALL→CALL_REPLY（单在飞 + 单条目去重缓存）、未知 slot 回 ERROR、收到 CLOSE 作废缓存回复；50 s 无 PING 回 DISCONNECTED 并作废短地址（`leaf.h:11`-`:32`）。静态缓冲 512 B RX / 320 B TX，零分配（`leaf.c:71`-`:72`）。内置演示算子为 u32 向量点积（slot = `A20_MCU_LEAF_OP_DOT_SLOT = 1`，`leaf.h:56`；实现 `leaf.c:338`），UART MTU 下 n ≤ 27。接入方式：板级 bring-up 调 `a20_mcu_leaf_init()`（可先用 `a20_mcu_leaf_set_identity()` 覆盖默认身份），周期上下文调 `a20_mcu_leaf_poll()`（`leaf.h:68`、`:86`）。编译注意：STM32 QEMU 探测镜像默认把叶子编出（SRAM 到顶且无对端），真实板卡 MCU 构建全量携带；`CONFIG_MCU_CLUSTER_LEAF=1` 可强制开启（`leaf.c:46`-`:54`）。`leaf.c` 已在 MCU profile 源码清单（`components/trim.toml:45`）。
- 两端共用同一份 header-only 线编解码（`kernel/cluster/uart.h`），保证位级一致且可主机侧测试（`uart.h:4`-`:14`）。

## 2. 设计定位

A20OS 已有完整的**单机**微服务框架：Native ABI 的 channel/EventQ/handle 对象模型（`kernel/include/abi/native/types.h:715` 的 `a20_channel_create_args_t`、`:761` 的 `a20_channel_call_args_t`、`kernel/include/abi/native/syscall_nr.h:100` 的 `A20_SYS_channel_call 0x0508`），内核对象 `a20_channel_ep_t`（`kernel/include/ipc/ipc.h:217`），以及用户态服务注册表 + svcmgr 监管。集群化之前，所有 IPC 端点都是本机对象：`a20_channel_ep_t.peer`（`kernel/include/ipc/ipc.h:221`）恒指向本机另一端点，没有任何网络透明性。

集群子系统把这套对象模型扩展为**网络透明**的，同时服务两种极端形态：

- **MCU 集群**：上千片 NOMMU MCU（如 `instances/stm32f103.toml`，64 KiB Flash / 20 KiB SRAM），经 UART/SPI/CAN 互联，作为「一个计算节点」的算力叶子。当前已实现：UART 头节点传输 + 单片叶子的协议面（§1.5）。
- **服务器集群**：多个完整 A20OS 实例（SMP、lwIP 网络、`kernel/net/lwip_stack.c`），经以太网互联。当前已实现：双实例 L2 环境（§1.4）；跨机数据面待 WA2。
- **混合集群**：服务器档节点做头节点/协议桥，MCU 档节点做叶子。「上千 MCU 组成一个计算节点」依赖此拓扑，头节点聚合部分（tree-reduce）属 WD2，未实现。

设计遵循混合内核判定规则（`docs/hybrid-kernel/00-design.md`）：内核只保留**端点路由与帧收发**这一数据面；成员判定、作业调度、协议网关全部放用户态服务。

与 `kernel/net/net_profile.h:27`-`:31` 的 `CONFIG_NET_PROFILE_EMBEDDED/DEFAULT/SERVER` 三档同构，集群子系统按「同一源码树、编译期分档」组织：档位宏 `A20_CLX_PROFILE_MCU/DEFAULT/SERVER`（`kernel/cluster/uart.h:57`-`:59`），当前由 `CONFIG_MCU` 推导（`uart.h:49`-`:55`）；Makefile 的 `CLUSTER_PROFILE` 接线属 WA3，未落地（见 §16）。

## 3. 非目标（v0 明确不做）

- 分布式共享内存 / 跨机 handle 传递。shmring 基于本机共享物理页，跨机无意义；handle 跨机涉及分布式能力表，留到 v1+。代码侧已钉死：远程端点上携带 handle 的发送返回 `-A20_ERR_CLUSTER_UNSUPPORTED`（§1.1）。
- 跨机时间片捐赠。`a20_channel_send_dwc`/recv_donate 依赖本机调度实体，跨机调用一律降级为普通调用。
- 透明任务迁移。任务以「算子+参数」（MCU 档）或「ELF+参数」（服务器档）显式分发，不做进程 checkpoint/迁移。
- 强一致性存储。集群 ABI 不提供一致性原语；需要共识的场景由用户态跑 Raft。

## 4. 总体架构

```
┌──────────────────────────────────────────────────────────┐
│ 用户态（按档裁剪，可崩溃可重启）                            │  未实现（WD 轨道）
│  clusterd（成员/路由服务） jobd（作业调度） 协议网关（MQTT等）│
├──────────────────────────────────────────────────────────┤
│ 集群 ABI（Native ABI 扩展，6 个 syscall 0x0520-0x0525）     │  已实现
│  远程端点 / 服务导出 / 成员事件订阅 / 路由注入               │  sys_native_cluster.c
├──────────────────────────────────────────────────────────┤
│ kernel/cluster/（内核数据面）                               │  已实现：remote-ep 代理、路由表、
│  remote-ep 代理 · 路由表 · 帧协议 · [分片重组 · 重传(档)]    │  帧协议；未实现：分片重组/重传（WA3）
├──────────────────────────────────────────────────────────┤
│ 传输层（可插拔 a20_clx_transport_t，transport.h:27）        │  已实现：loopback（全档）、UART；
│  loopback │ [UDP(lwIP) 未实现] │ UART 帧 │ (SPI/CAN 后续)   │  未实现：UDP（WA2）
└──────────────────────────────────────────────────────────┘
```

关键结构：**本机 channel 是远程 channel 的特例**，且这一结构已在代码里成立。`a20_channel_ep_t` 的 `peer` 指针仍指向本机端点；集群化后 peer 可以指向一个 remote-ep 代理半——`cluster_connect` 创建一对普通 channel，调用方拿可见半，集群核心持代理半并由每代理一个的 TX worker 像普通本机消费者一样排空其队列、把消息序列化为帧交给传输层（`kernel/cluster/remote_ep.c:1`-`:14`）。本机快路径（含 64 KiB `A20_CH_MAX_DATA` 上限，`kernel/include/ipc/ipc.h:147`）逐指令未改；对 `a20_channel.c` 的全部改动是两个冻结式 API（`a20_channel_ep_peer_ref`，`kernel/include/ipc/ipc.h:372`；类型标志 `A20_CHAN_TYPE_REMOTE`，`ipc.h:172`），登记于 03-kernel-impl 实现期记录 #1。

## 5. 节点身份与命名

128 位节点标识，全集群唯一（`kernel/include/abi/native/types.h:1105`-`:1107`）：

- 全零 = `A20_NODE_ID_LOCAL`（本机）：寻址 `(LOCAL, slot)` 走与本机 channel 等价的快路径，绝不序列化上链——这保证现有程序对集群无感知。全 0xff = `A20_NODE_ID_BROADCAST`（保留，SERVER 档 SEND 帧；`types.h:1101`-`:1104`）。
- 派生规则（按优先级）：烧录配置 > 网卡 MAC 哈希 > 芯片 UID 哈希 > 启动时随机生成并持久化。**未实现**：派生逻辑属于用户态 clusterd（WD1）；当前由管理方直接以 `cluster_set_self` 注入。内核只保存注入的 `self`（`kernel/cluster/route.c:34`-`:36`）。
- MCU 叶子使用 16 位短地址（借鉴 MQTT-SN 的 short topic id 思路）：短地址由头节点在 HELLO 中分配，叶子侧不存 128 位映射表（已实现，§1.5；`uart.h:168`、`leaf.h:11`-`:32`）。
- 线上以 32 位 FNV-1a 节点哈希寻址（`a20_clx_node_hash`，`kernel/cluster/route.c:70`；金样把哈希钉在 `tools/cluster-ref/vectors/MANIFEST.json`）。

### 服务命名（部分未实现）

设计形态为两级名：

```
service://<node>/<name>        # node 为 128 位 ID 的十六进制，或 "local"
service://any/<name>           # 由 clusterd 按策略选一个节点（负载/就近）
```

**当前状态**：`service://` 解析与 `any` 策略属 clusterd，未实现（WD1）。内核 ABI 只认 `(node_id, slot)`；本机侧 `cluster_connect(LOCAL, name)` 按名解析已实现（`sys_native_cluster.c:213`-`:215`），远程按名连接返回 `-A20_ERR_CLUSTER_UNSUPPORTED`（`sys_native_cluster.c:227`-`:230`）。

## 6. 集群 ABI 契约要点

字段级规范见 [01-abi.md](01-abi.md)，此处只钉顶层契约：

1. **`cluster_connect` 返回普通 channel 句柄**是整套 ABI 的核心决定：现有所有使用 channel 的代码（服务客户端、ufsd、pager）无需修改即可指向远端——网络透明性在对象模型层完成，不在应用层完成。已实现（§1.1）。
2. **失败语义是一等的**。cluster 段 errno（`kernel/include/ipc/ipc.h:105`-`:111`；`kernel/include/abi/native/errno.h` 是 re-export 头）：
   - `A20_ERR_NODE_UNREACHABLE (26)`：无路由、链路 DOWN、TTL 耗尽、未 set_self。
   - `A20_ERR_CLUSTER_TIMEOUT (27)`：远程 CALL 超 deadline、建链超时（建链超时属 WA2 语义）。
   - `A20_ERR_REMOTE_CLOSED (28)`：远端端点已关闭（对应本机 `peer_closed`）。
   - `A20_ERR_CLUSTER_UNSUPPORTED (29)`：跨机 handle/捐赠、MCU 档分片/广播、HELLO 哈希冲突。
   远程调用绝不无限期阻塞伪装成本机调用：在途 deadline 固定 30 s（`clx_internal.h:62`）；受冻结 channel API 所限，超时/关闭对调用方的呈现见 §1.1 第三条。
3. **能力约束**：`cluster_set_self`/`cluster_export`/`cluster_route` 要求管理权限；**实现期决定**是以 effective uid 0 承载（clusterd 以 root 运行），而非新增 `A20_RIGHT_CLUSTER_ADMIN` right 位——本树 rights 附着于 handle、无任务级位图（`sys_native_cluster.c:10`-`:14`、`:64`-`:69`，登记于 01-abi 落地状态）。`cluster_connect`/`cluster_event_subscribe`/`cluster_link_status` 无特权。
4. **限额按档缩放**（`kernel/include/abi/native/resource.h:58`-`:78`）：路由 4/256/4096，导出 slot 4/64/256，远程代理端点 2/128/1024，在途未应答 CALL 1/64/512（MCU/DEFAULT/SERVER）；重组字节上限已冻结（0/256 KiB/4 MiB）但重组本身未实现（WA3）。

## 7. 传输层

传输契约 `a20_clx_transport_t` 已实现（`kernel/cluster/transport.h:27`-`:37`）：`mtu`（含帧头与 CRC）、`flags`（`A20_CLX_TFL_RELIABLE_CAPABLE/BROADCAST_CAPABLE/POLLING`，`transport.h:19`-`:22`）、`send` 到下一跳链路地址、POLLING 档的 `poll`。注册表先注册先得、重复 id 拒绝（`a20_clx_transport_register`，`kernel/cluster/transport.c:126`），上限 8 个传输（`transport.h:24`）。传输只负责链路级投递；多跳路由由 `kernel/cluster` 路由表 + clusterd（未实现）完成。上行唯一入口是 `a20_clx_rx_frame`（`transport.c:197`，只入环形缓冲并唤醒 RX 线程，可在未来中断上下文调用）与 `a20_clx_link_event`（`transport.c:495`）。

三种传输的现状：

| 传输 | id | MTU | 状态 |
|---|---|---|---|
| loopback | 0 | 65536（`transport.c:388`） | **已实现，全档注册**（`transport.c:383`-`:390`）。4 字节虚拟节点号寻址；发送即拷贝进本核 RX 上行，零丢包保序；虚拟节点注册表 `transport.h:72`-`:74`；`CONFIG_CLUSTER_TEST_HOOKS` 下带 partition/drop/dup/corrupt/delay 测试钩子（`kernel/cluster/loopback.c:268`-`:301`） |
| UDP | 1 | 1472（规范值） | **未实现（WA2）**：`kernel/cluster/udp.c` 不存在；跨机 `channel_call` 不可用。验收环境已就绪（§1.4） |
| UART | 2 | 256（`uart.h:165`） | **已实现**：头节点侧 `kernel/cluster/uart.c`（§1.5），DEFAULT 档起注册（`transport.c:392`-`:404`）；MCU 档由 `kernel/mcu/leaf.c` 叶子面承担 |

分档模型（与 `CONFIG_NET_PROFILE_*` 同构；`Makefile:135` 的 `NET_PROFILE` 是接入样式参照）：

- `CLUSTER_PROFILE_MCU(1)`：仅 UART 帧传输，无重传无分片（上层消息必须 ≤ MTU），路由表静态烧录 1~4 项（限额 `resource.h:58`-`:78` 的 MCU 列）。
- `CLUSTER_PROFILE_DEFAULT(2)`：loopback + UDP，ACK+有限重传，分片重组（64 KiB 消息 ↔ MTU 帧）。**其中 UDP、重传、重组未实现（WA2/WA3）**；当前 DEFAULT 档实际注册 loopback + UART。
- `CLUSTER_PROFILE_SERVER(3)`：DEFAULT + 多 lane 收发（复用 `net_lane` 分桶思路，见 `docs/net/net-lanes.md`）、RTT/丢包统计、链路聚合。**未实现**；计数字段已在 ABI 就位（`types.h:1203`-`:1218`），loopback/UART 链路已在计数。

## 8. 帧协议（线格式 v0）

定长 32 字节头 + 变长载荷 + CRC16（CCITT-FALSE；MCU 档可选关）。字段全部小端定宽，逐字段显式编解码、**禁止结构体内存映像收发**（`uart.h:61`-`:64`）。字段偏移即代码常量（`uart.h:75`-`:89`）：

```
 0  magic(2)="CL"(0x4C43)  ver(1)=0  type(1)
 4  flags(2)               frag(2)   /* 片号(低12位)|bit15=末片|bit12-14 保留 */
 8  txid(4)                          /* CALL/CALL_REPLY 事务配对 */
12  src_hash(4)           dst_hash(4) /* FNV-1a 节点哈希；完整 16B ID 在 HELLO 建立 */
20  dst_slot(4)                     /* cluster_export 返回的槽位 */
24  seq(4)                          /* 可靠档重传序号（WA3 启用） */
28  payload_len(2)       ttl(1)=8   csum_kind(1)  /* 0=无 1=CCITT */
32  payload...
```

消息类型 11 种（`uart.h:92`-`:102`）：`HELLO`/`HELLO_ACK`（建链，交换完整 node ID、档位与短地址分配，载荷定长 32 B，`uart.h:111`）、`PING/PONG`（链路心跳）、`SEND`（单向消息）、`CALL/CALL_REPLY`（RPC 对）、`CLOSE`（端点关闭通告）、`ACK/NACK`（可靠档，未实现）、`ERROR`（带 cluster errno 的失败应答）。原设计保留的 `ROUTE_ADV`（带内路由通告）未进入 v0 线格式——类型表以 `uart.h` 为准。

语义映射规则：

- 本机 `channel_call` → `CALL` + 等待匹配 `txid` 的 `CALL_REPLY`。v0 的远程消息一律走 `CALL/CALL_REPLY` 对（含单向发送），登记于 02-§2 实现期记录（`clx_internal.h:14`-`:15`）。
- 对端句柄释放 → `CLOSE`，本地代理端点双半 `peer_shutdown`，调用方按本机对端关闭唤醒（`remote_ep.c:39`-`:45`）。
- TTL 每跳减 1，为 0 丢弃（默认 8，`uart.h:71`）——树形/胖树拓扑下防环。
- 接收侧判决分两层：layer-1 编解码判决（13 种 verdict，`kernel/cluster/frame.h:54`-`:68`，与金样对拍锁定）接受/拒绝帧；layer-2 分发策略（TTL 耗尽、非本机目的、重复 seq、HELLO 协商）在分发层处理并计入 `rx_drops`。
- 分片（`frag` 字段）与 `seq/ACK/NACK` 在线格式中已冻结，但内核侧重组与重传**未实现（WA3）**；金样中已有分片与重组类向量（`tools/cluster-ref/vectors/`，含 state 类 6 条）。

完整规范见 [02-wire-protocol.md](02-wire-protocol.md)；编解码实现 `kernel/cluster/frame.c`（encode `:70`，decode `:122`）。

## 9. 成员管理（用户态，分两档）

内核不做成员判定，只暴露链路事件（§1.1 `cluster_event_subscribe`）与路由注入接口（`cluster_route`）。这与「协议解析类迁用户态」的分层规则一致。

- **服务器档 clusterd（未实现，WD1）**：SWIM 风格探测（间接探测 + 疑似/确认两阶段，参考 HashiCorp memberlist 的公开算法），维护全量成员表，向内核注入路由。规模目标 10²~10³ 节点。当前路由由管理方手工/脚本经 `cluster_route` 注入。
- **MCU 档（叶子面已实现）**：叶子是被动的——应答 `PING`、被动应答 `HELLO`。判定与拓扑全部由头节点的 clusterd 完成；一片 20 KiB SRAM 的 MCU 不保存任何成员表（`kernel/mcu/leaf.c`，§1.5）。
- 上千 MCU 的寻址压力集中在头节点：头节点保存叶子短地址表，聚合上行业务（tree-reduce，WD2，**未实现**），避免上千节点直连任一单点。

## 10. 序列化与 handle 策略（v0）

- 跨机消息载荷 = 不透明字节串，上限沿用 `A20_CH_MAX_DATA`（64 KiB，`kernel/include/ipc/ipc.h:147`）。类型化 envelope（`kernel/include/ipc/envelope.h`）跨机线格式由 v1 定义。
- `channel_call`/`channel_send` 携带 handle 时若目标是远程端点，在类型检查点被拒并映射为 `-A20_ERR_CLUSTER_UNSUPPORTED`（`ipc.h:166`-`:172`、`sys_native_cluster.c:78`-`:85`）。唯一例外：`A20_NODE_ID_LOCAL` 目标不受影响。
- 大数据传输（>64 KiB）在服务器档由用户态分片流（类似 shmring 的协议化版本）完成，不进内核 ABI。**未实现**（属 WD 演示层）。

## 11. 与现有构建矩阵的集成（现状）

- MCU profile 构建（`Makefile:1209` 的 `PROFILE=mcu` 块）从 `components/trim.toml` 的 `[profile.mcu].sources` 取源码清单，`kernel/mcu/leaf.c` 已列入（`components/trim.toml:45`）。原设计提的「`cluster` 特性位与 `CLUSTER_PROFILE_MCU_ARCHES`」未采用——特性位未新增，叶子直接进 MCU 源码清单。
- 集群档位当前由 `CONFIG_MCU` 推导（`uart.h:49`-`:55`）；Makefile 的 `CLUSTER_PROFILE ?= 2` 接线**未落地**（`kernel/cluster/uart.h:42`-`:46` 注明属 WA3）。显式 `-DCONFIG_CLUSTER_PROFILE=` 总是优先。
- `instances/` 已有 `qemu-riscv64-cluster-a.toml` / `-b.toml`（§1.4）；原设计列的 `stm32f103-cluster-leaf.toml` 实例**未创建**，MCU 叶子目前经 host 测试（`tools/tests/test_mcu_leaf.c`）与真板构建覆盖。
- 实例 schema 已扩展 `[net].mac`/`guest_ip`（双节点 TOML 在用）；「节点 ID 种子/对端端口」的集群专用字段随 WA2 落地。

## 12. 参考协议对照

| 参考 | 吸收什么 | 不吸收什么 |
|---|---|---|
| Plan 9 / 9P | 网络透明对象模型的存在性证明；「一切皆命名通道」 | 文件语义；9P 的无会话简单性在丢包链路上不成立 |
| DDS-XRCE | MCU 档帧格式、session/keyframe、客户端-代理非对称模型 | DDS 全套 QoS 与发现协议（对 20 KiB SRAM 仍太重） |
| MQTT-SN | 叶子短地址、休眠节点语义 | pub/sub 主题模型（与 channel RPC 模型不符） |
| SWIM / memberlist | 服务器档成员检测算法（公开、已被生产验证） | 其基于 UDP 全互联的假设（MCU 拓扑是树） |
| MPI | 集体通信（bcast/reduce）作为**用户态库**的接口参照 | 不进内核；不做进程组语义 |
| gRPC | 反面教材：HTTP/2+Protobuf 的堆叠正是资源光谱两端不能共用一个协议栈的证据 | 全部 |

## 13. 实现状态与路线图

### 已完成（按 2026-10-07 源码核对）

| 子系统 | 交付物 | 验证方式 |
|---|---|---|
| 集群 ABI（W0 冻结 + 接线） | 六个 syscall，`kernel/abi/native/sys_native_cluster.c`；类型/errno/限额冻结于 `types.h:1095`-`:1218`、`ipc.h:105`-`:111`、`resource.h:58`-`:78` | 内核自检（§1.2） |
| 内核数据面（WA1） | `kernel/cluster/`：`frame.c`（编解码）、`route.c`（路由表）、`export.c`（导出表）、`transport.c`（注册/分发/计数/RX 线程/250 ms housekeeping，`transport.c:330`-`:334`）、`loopback.c`（虚拟节点 + 测试钩子）、`remote_ep.c`（代理端点 + TX worker + 在途事务 + deadline）、`selftest.c` | `clxselftest=1`；`tools/tests/test_clx_frame.c` |
| UART 传输 + MCU 叶子（WC1） | `kernel/cluster/uart.c`/`uart.h`（头节点）、`kernel/mcu/leaf.c`/`leaf.h`（叶子） | `tools/tests/test_clx_uart.c`、`test_mcu_leaf.c`（`make host-tests`） |
| 线协议参考实现 + 金样（WB1） | `tools/cluster-ref/`：clframe.py、refdec.c、check_c_side.py、selftest.py、108 条向量 | `selftest.py` 逐字节回归；`check_c_side.py` 对拍 |
| 双实例 L2 环境（WB2） | `instances/qemu-riscv64-cluster-{a,b}.toml`、`tools/cluster-net-up.sh`、[02-udp-demo.md](02-udp-demo.md) | 脚本一键复现双向 ping |

### 未实现（实施计划见 [impl-prompts.md](impl-prompts.md) 对应轨道）

| 子系统 | 缺口 | 轨道 |
|---|---|---|
| UDP 传输 | `kernel/cluster/udp.c` 不存在；跨机 `channel_call`、HELLO 建链（非 UART 链路）、`connect` 的 `timeout_ms` 消费均未落地 | WA2 |
| 可靠性 | 分片重组、`seq/ACK/NACK` 重传与去重（`reasm.c`/`reliable.c` 不存在）；`A20_CONNECT_RELIABLE` 旗标已冻结但无可靠档实现 | WA3 |
| 用户态服务 | `user/svc/clusterd.c`、`user/svc/jobd.c` 不存在；SWIM 成员判定、`service://` 解析、节点 ID 派生、tree-reduce、demo-wordcount 均未实现 | WD1/WD2 |
| 生产硬化 | `check-cluster-*` CI 门禁、故障注入矩阵（loopback 测试钩子只是起点）、模糊、8 h soak、性能实测均未落地 | WE1/WE2 |
| 构建接线 | Makefile `CLUSTER_PROFILE` 变量、`stm32f103-cluster-leaf.toml` 实例 | WA3/WC 后续 |

### 轨道依赖图（W0/WA1/WB1/WB2/WC1 已收官，其余为计划）

```
W0 ABI 冻结 ✔（串行门禁，已收官）
 ├─ WA 内核数据面（轨道内串行）：WA1 loopback 骨架 ✔ → WA2 UDP 传输（未启动）→ WA3 可靠性（未启动）
 ├─ WB 验证设施：WB1 参考编解码+金样 ✔ → WB2 双 QEMU 网络打通 ✔
 │     （WB1 金样是 WA2 的对拍依据；WB2 是 WA2 的验收环境——两者均已就位）
 ├─ WC MCU 轨道：WC1 UART 传输 + 叶子协议面 ✔
 ├─ WD 用户态（WA2 后启动）：WD1 clusterd 完整化 → WD2 jobd + tree-reduce 演示
 └─ WE 生产硬化（功能全部完成后）：WE1 故障注入/模糊/soak/性能 → WE2 安全审查 + 运维文档 + 终验
```

文件所有权（避免并行轨道互相踩）：WA 拥有 `kernel/cluster/`（除 uart.c）、`kernel/abi/native/sys_native_cluster.c`；WB 拥有 `tools/cluster-ref/`、`instances/*cluster*`、演示脚本；WC 拥有 `kernel/cluster/uart.c`、`kernel/mcu/` 改动；WD 规划拥有尚未创建的 `user/svc/clusterd.c`、`user/svc/jobd.c`。跨所有权的修改必须先在规范文档中登记。

每个轨道的独立可执行 prompt 见 [impl-prompts.md](impl-prompts.md)——它是**后续轨道的实施计划**（WA2/WA3/WD/WE），不是已完成功能的描述。

## 14. 文档地图

本文件是顶层入口：回答「集群是什么、现在能用哪些、整体设计为什么是这样」。各子系统文档的定位与读者：

| 文档 | 内容 | 主要读者 |
|---|---|---|
| [01-abi.md](01-abi.md) | 集群 ABI 字段级规范：每个结构体、errno、rights、限额、稳定性规则；含落地状态清单 | 使用 ABI 的人 + WA 轨道 |
| [02-wire-protocol.md](02-wire-protocol.md) | 帧协议完整规范：布局、类型、状态机、分片/重传/去重、版本协商 | WA/WB/WC 全部 |
| [03-kernel-impl.md](03-kernel-impl.md) | 内核实现指南：文件划分、与 a20_channel.c 的集成点、并发与内存纪律、常见坑、实现期记录 | WA 轨道 |
| [04-transports.md](04-transports.md) | 传输契约 + loopback/UDP/UART 各自精确规范（含测试钩子） | WA/WC 轨道 |
| [05-userspace.md](05-userspace.md) | clusterd（SWIM 参数与状态机）、服务命名、jobd（作业格式与重试语义）、演示程序的设计规范（对应代码未实现） | WD 轨道 |
| [06-production.md](06-production.md) | 生产就绪标准：测试矩阵、故障注入、模糊测试、性能目标、安全、CI、运维文档（验收条款，均未落地） | WE 轨道 |
| [02-udp-demo.md](02-udp-demo.md) | 双实例 L2 环境的使用说明（已可用） | 所有人 |
| [impl-prompts.md](impl-prompts.md) | 后续轨道（WA2/WA3/WD/WE）的实施计划与依赖图 | 人（调度用） |

冲突仲裁：代码现实 > 子系统规范 > 本文件。任何实现期偏差必须回写对应规范文档并注明核对日期；本文的登记在 §16。

## 15. 生产就绪定义（全部满足才算「完成」；当前进度见 §13）

1. **功能**：00-design 全部契约 + 05-userspace 的 clusterd/jobd 落地，混合集群（服务器节点 + MCU 叶子）演示可一键复现。
2. **可靠**：06-production 的故障注入矩阵全绿（丢包/延迟/重复/损坏/分区/节点杀除），8 小时 soak 无泄漏无死锁。
3. **安全**：默认零导出（服务不 export 就不可达）、权限位生效、帧解码器通过模糊测试无崩溃。
4. **性能**：达到 06-production 的量化目标并有可复现的测量脚本。
5. **工程**：全架构既有 check 门禁不回归；新增 `check-cluster-*` 门禁进 CI；运维文档（配置、故障排查）齐备。

## 16. 落地状态与实现期决策

本文原无此节；2026-10-07 改写时按源码核对补建。各子系统文档（01-abi、02、03、04）的落地状态清单仍然是偏差的**主登记处**，此处只登记顶层叙述与代码现实之间的偏差，不重复子系统条目。

1. **管理权限的承载**（核对 2026-10-07）：本文旧版曾写「`cluster_export`/`cluster_route` 要求 `A20_RIGHT_CLUSTER_ADMIN`（新 right 位，加入已不存在的 `kernel/abi/native/rights.h` 体系）」。代码未新增 right 位：本树 rights 附着于 handle、无任务级位图，管理权限由 effective uid 0 承载（`sys_native_cluster.c:64`-`:69`，文件头 `:10`-`:14` 说明）。01-abi 落地状态已登记；本文 §6 按代码改写。
2. **cluster errno 的物理位置**：旧版曾引用已不存在的 `kernel/abi/native/errno.h`。实际四个 cluster errno 定义在 `kernel/include/ipc/ipc.h:105`-`:111`；`kernel/include/abi/native/errno.h` 只是 re-export 头（其文件头注明「status code space is internal」）。已按代码改写 §6。
3. **行号漂移修正**：`a20_channel_ep_t` 现位于 `kernel/include/ipc/ipc.h:217`（`peer` 字段 `:221`）；`A20_CH_MAX_DATA` 定义于 `ipc.h:147`（旧版经 `kernel/include/fs/ufs_proto.h:10` 间接引用，现改为直指定义处）。
4. **远程消息一律 CALL/CALL_REPLY**：v0 数据面不使用 `SEND` 帧承载远程单向消息（02-§2 实现期决定，`clx_internal.h:14`-`:15`）；`SEND` 类型保留在线格式与金样中。
5. **`connect` 的 `timeout_ms` v0 未消费**：loopback 无 HELLO 建链握手，connect 即时返回；该字段按头部校验规则校验但不被读取，WA2 接入（01-abi 落地状态 #13）。
6. **构建矩阵两处未落地**：`components/trim.toml` 未新增 `cluster` 特性位（`kernel/mcu/leaf.c` 直接列入 `[profile.mcu].sources`，`components/trim.toml:45`）；Makefile 无 `CLUSTER_PROFILE` 变量，档位由 `uart.h:49`-`:55` 推导，接线属 WA3。`stm32f103-cluster-leaf.toml` 实例未创建。本文 §11 按现状改写。
7. **事件投递形态**：`cluster_event_subscribe` 复用冻结的 `a20_pending_event_t`（event code = 32 + kind，`clx_internal.h:64`-`:72`），不是本文早期设想的 32 字节自定义 payload（01-abi 落地状态 #11）。
8. **单一导出命名空间**：v0 每内核一个导出命名空间，loopback 虚拟节点共享之；按节点分表随 WA2 到来（04-§2 实现期记录，`clx_internal.h:12`-`:14`）。
