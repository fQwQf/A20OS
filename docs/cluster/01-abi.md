# 集群 ABI 字段级规范（v0）

内容已按 2026-10 源码核对。本文是 `kernel/include/abi/native/` 新增内容的**唯一依据**。风格基准：`types.h:715` 的 `a20_channel_create_args_t`、`errno.h`、`rights.h`、`resource.h:31` 的限额风格。实现时行号以代码现状为准。

## 通用约定

- 所有参数经单个 `*_args_t` 结构体传入，syscall 返回 `int64_t`：≥0 为成功值（句柄/槽位/0），<0 为 `-errno`。
- 每个 args 结构体首字段必须是 `uint32_t struct_size`，调用方填 `sizeof(struct)`；内核校验 `struct_size >= 内核已知最小尺寸`，多出的尾部字节必须为零，否则返回 `A20_ERR_INVALID_ARGS`。这是将来扩展字段的唯一机制——**禁止**改动已冻结结构体中间字段。
- 未命名字段与 flags 保留位：调用方必须置零，内核必须忽略非零保留位之外的合法组合并拒绝非法 flags（返回 `A20_ERR_INVALID_ARGS`）。
- 句柄均为 `a20_handle_t`，类型 `A20_OBJ_*` 沿用 `types.h` 现有枚举；集群新增的句柄本身不新增对象类型——`cluster_connect` 返回的就是普通 `A20_OBJ_CHANNEL_ENDPOINT`。

## 节点标识

```c
typedef struct a20_node_id { uint8_t bytes[16]; } a20_node_id_t;
```

| 保留值 | 含义 |
|---|---|
| 全零 | `A20_NODE_ID_LOCAL`，本机。寻址 `(LOCAL, slot)` 必须走本机快路径，禁止序列化 |
| 全 `0xff` | `A20_NODE_ID_BROADCAST`，仅 SERVER 档、`SEND` 帧可用；MCU 档收到必须丢弃 |

派生规则在用户态 clusterd（见 05-userspace.md），内核只保存 `cluster_set_self` 注入的值。注入前 `self == LOCAL`，一切远程操作返回 `A20_ERR_NODE_UNREACHABLE`。

### 节点哈希

帧头携带 32 位节点哈希而非完整 ID：`fnv1a32(node_id.bytes, 16)`，FNV 偏移基 `2166136261`、质数 `16777619`，逐字节 `h = (h ^ b) * 16777619`。哈希冲突由 HELLO 协商检测（两端完整 ID 不同但哈希相同 → 后连入者被拒绝，`ERROR` 帧 + `A20_ERR_CLUSTER_UNSUPPORTED`），v0 不做在线重编址。

## 能力位（cluster_set_self 的 caps）

| 位 | 名称 | 含义 |
|---|---|---|
| 0 | `A20_CLUSTER_CAP_RELAY` | 可为其他节点转发帧（头节点/服务器置位，MCU 叶子清零） |
| 1 | `A20_CLUSTER_CAP_RELIABLE` | 支持 ACK/重传/分片重组（DEFAULT/SERVER 档） |
| 2 | `A20_CLUSTER_CAP_LEAF` | 叶子节点：只应答，不主动建链（MCU 档置位） |

## syscall 详表

号段：`0x05xx` 段顺延分配（`syscall_nr.h:100` 现有 `channel_call = 0x0508`，新号从 `0x0520` 起留足间距）。登记进 `syscall_table.def`，实现放新文件 `kernel/abi/native/sys_native_cluster.c`。

### 1. `A20_SYS_cluster_set_self`

```c
typedef struct a20_cluster_set_self_args {
    uint32_t       struct_size;
    uint32_t       caps;          /* A20_CLUSTER_CAP_* */
    a20_node_id_t  node_id;
    uint64_t       reserved[2];   /* must be zero */
} a20_cluster_set_self_args_t;
```

- 语义：注入本节点身份。**只允许调用一次**，第二次起返回 `A20_ERR_ALREADY_EXISTS`（若 errno 中无此名，用最接近的现有码并在实现文档登记）。
- 权限：调用任务需 `A20_RIGHT_CLUSTER_ADMIN`（通常仅 clusterd）。
- 错误：`A20_ERR_INVALID_ARGS`（node_id 为保留值、caps 含未知位、非零 reserved）。

### 2. `A20_SYS_cluster_export`

```c
typedef struct a20_cluster_export_args {
    uint32_t       struct_size;
    uint32_t       flags;         /* 下表 */
    a20_handle_t   channel;       /* A20_OBJ_CHANNEL_ENDPOINT，需 R 权 */
    const char    *service_name;  /* UTF-8，非 NULL 结尾也可，配合 name_len */
    uint32_t       name_len;      /* ≤ 64 */
    uint32_t       reserved;      /* zero */
} a20_cluster_export_args_t;
/* 返回 ≥0：32 位槽位号 */
```

| flag | 含义 |
|---|---|
| `A20_EXPORT_REPLACE` (bit0) | 同名服务已存在时替换（默认返回 `A20_ERR_ALREADY_EXISTS`） |
| `A20_EXPORT_LOCAL_ONLY` (bit1) | 只登记名→槽位，不接受远程连接（调试/灰度用） |

- 槽位：per-node 单调分配，不复用（避免陈旧 slot 指错服务）。0 保留。上限见限额节。
- 服务名重复、channel 类型错误 → `A20_ERR_INVALID_ARGS`；未 set_self → `A20_ERR_NODE_UNREACHABLE`。

### 3. `A20_SYS_cluster_connect`

```c
typedef struct a20_cluster_connect_args {
    uint32_t       struct_size;
    uint32_t       flags;         /* bit0: A20_CONNECT_RELIABLE（对端无 CAP_RELIABLE 时失败） */
    a20_node_id_t  node_id;       /* LOCAL 表示本机服务名解析，配合 slot=0 + service_name */
    uint32_t       slot;          /* cluster_export 返回值；node_id=LOCAL 时可为 0 走按名解析 */
    const char    *service_name;  /* 可选；非 NULL 时优先于 slot */
    uint32_t       name_len;
    uint32_t       timeout_ms;    /* 建链超时；0 = 默认 5000 */
    uint32_t       reserved;      /* zero */
} a20_cluster_connect_args_t;
/* 返回 ≥0：普通 A20_OBJ_CHANNEL_ENDPOINT 句柄（R|W） */
```

- **这是整套 ABI 的核心**：返回的句柄之后用普通 `channel_call`/`channel_send`/`channel_recv` 通信，调用方无需感知远程。
- 远程调用强制超时：`channel_call` 在远程端点上的等待时间 = min(调用方 deadline, 建链时协商的链路 max_deadline=30s)。超时返回 `A20_ERR_CLUSTER_TIMEOUT`。**任何情况下不得无限期阻塞。**
- 错误：`A20_ERR_NODE_UNREACHABLE`（无路由/链路 down）、`A20_ERR_CLUSTER_TIMEOUT`、`A20_ERR_NOT_FOUND`（slot/服务名远端不存在，经 `ERROR` 帧获知）、`A20_ERR_CLUSTER_UNSUPPORTED`（RELIABLE 不被支持、MCU 档遇到 BROADCAST）。

### 4. `A20_SYS_cluster_route`

```c
typedef struct a20_cluster_route_args {
    uint32_t       struct_size;
    uint32_t       op;            /* 0=ADD 1=DEL 2=REPLACE(默认，推荐) */
    a20_node_id_t  node_id;
    uint32_t       transport_id;  /* cluster 子系统分配的传输实例号，0=loopback 1=udp 2=uart */
    uint8_t        next_hop[16];  /* 链路地址，解释随传输：udp=IPv4(4B)+port(2B)；uart=短地址(2B)；loopback=虚拟节点号(4B) */
    uint32_t       metric;        /* 同目标多路由时取最小；0 默认 */
    uint32_t       reserved;      /* zero */
} a20_cluster_route_args_t;
```

- 权限：`A20_RIGHT_CLUSTER_ADMIN`。
- v0 只支持精确 node_id 匹配，无前缀/子网。路由表为内核唯一拓扑来源；clusterd 负责计算与注入。

### 5. `A20_SYS_cluster_event_subscribe`

```c
typedef struct a20_cluster_event_subscribe_args {
    uint32_t  struct_size;
    uint32_t  mask;      /* 下表，位或 */
} a20_cluster_event_subscribe_args_t;
/* 返回 ≥0：EventQ 句柄 */
```

| mask 位 | 事件 payload |
|---|---|
| `LINK_UP` / `LINK_DOWN` | `{node_id, transport_id, reason}` |
| `ROUTE_LOST` | `{node_id}`（路由存在但链路判定不可达） |
| `EXPORT_DROPPED` | `{slot}`（导出端点被释放，远端会看到 CLOSE） |

事件 payload 定长 32 字节，第 0 字节为事件码。缓冲区满时新事件覆盖最旧并置溢出计数（经 `cluster_link_status` 可查）——成员事件允许丢失，clusterd 必须以周期全量同步兜底。

### 6. `A20_SYS_cluster_link_status`

```c
typedef struct a20_cluster_link_status_args {
    uint32_t      struct_size;
    uint32_t      reserved;
    a20_node_id_t node_id;       /* LOCAL = 汇总所有链路 */
    /* 输出 */
    uint32_t      state;         /* 0=DOWN 1=SUSPECT 2=UP */
    uint32_t      rtt_us;        /* 滑动平均；MCU 档恒 0 */
    uint64_t      tx_frames, rx_frames, tx_drops, rx_drops, retransmits;
    uint64_t      last_hello_age_ms;
} a20_cluster_link_status_args_t;
```

## 新 errno（加入 `errno.h`，数值顺延现有段，禁止复用已分配值）

| 名称 | 触发场景（必须唯一映射，禁止混用） |
|---|---|
| `A20_ERR_NODE_UNREACHABLE` | 无路由、链路 DOWN、TTL 耗尽、未 set_self |
| `A20_ERR_CLUSTER_TIMEOUT` | 远程 CALL 超 deadline；建链超时；重组超时（对调用方） |
| `A20_ERR_REMOTE_CLOSED` | 对端端点关闭（收到 CLOSE 或已知对端句柄释放后） |
| `A20_ERR_CLUSTER_UNSUPPORTED` | 跨机传 handle、跨机捐赠、MCU 档用分片/广播、RELIABLE 协商失败、HELLO 哈希冲突 |

## 限额（加入 `resource.h`，DEFAULT/ABSOLUTE 两组 + MCU 档小值）

| 限额 | MCU | DEFAULT | ABSOLUTE |
|---|---|---|---|
| 路由表项 | 4 | 256 | 4096 |
| 导出槽位 | 4 | 64 | 256 |
| 远程端点代理 | 2 | 128 | 1024 |
| 重组缓存（字节） | 0（禁用） | 256 KiB | 4 MiB |
| 待确认未应答 CALL | 1 | 64 | 512 |

超限返回 `A20_ERR_RESOURCE_LIMIT`（若无现成码则新增并登记）。

## 跨机限制（v0 硬规则）

1. 远程端点上的 `channel_call`/`channel_send` 携带 handle → `A20_ERR_CLUSTER_UNSUPPORTED`，消息不发出。
2. 捐赠族调用（`a20_channel_send_dwc`、recv_donate）目标是远程端点 → 同上。
3. 载荷上限沿用 `A20_CH_MAX_DATA`（64 KiB）；MCU 档进一步受单帧 MTU 限制（256 字节，不可分片）。
4. 目标 `A20_NODE_ID_LOCAL` 的一切调用与本机语义**逐位一致**，不受 1–3 限制。

## ABI 稳定性规则

- 已冻结结构体：只可追加尾部字段（配合 `struct_size`），禁止改中间。
- 帧协议 `ver` 字段：兼容窗口为一个版本——vN 的实现必须能收 vN-1 的 HELLO 并降级协商；不能协商时发 `ERROR` 帧并拒绝建链。
- errno 数值一经分配永不复用；废弃的码保留注释。

## 落地状态与实现期决策（W0 起）

本节记录本文与代码现实不一致之处。冲突仲裁顺序是代码现实 > 子系统规范 > 顶层设计，所以每一处偏差都写在这里而不是留给读者去猜。

### 已有 errno 的复用

本文写的 `A20_ERR_ALREADY_EXISTS` / `A20_ERR_NOT_FOUND` / `A20_ERR_RESOURCE_LIMIT` / `A20_ERR_INVALID_ARGS` 在本树的 errno 空间里名字不同。复用现有码，不新增同义码：

| 本文用词 | 实际实现 | 说明 |
|---|---|---|
| `A20_ERR_ALREADY_EXISTS` | `A20_ERR_EXISTS`（10） | `cluster_set_self` 二次调用、`cluster_export` 同名冲突 |
| `A20_ERR_NOT_FOUND` | `A20_ERR_NOT_FOUND`（24） | 远端 slot / 服务名不存在 |
| `A20_ERR_RESOURCE_LIMIT` | `A20_ERR_NO_SPACE`（13） | 各项限额打满 |
| `A20_ERR_INVALID_ARGS` | `A20_ERR_INVALID_ARGUMENT`（12） | 保留位非零、flags 越界、node_id 为保留值 |

四个新集群 errno 落在 26–29，接在既有 1–25 之后：`NODE_UNREACHABLE` / `CLUSTER_TIMEOUT` / `REMOTE_CLOSED` / `CLUSTER_UNSUPPORTED`。

### 结构体首字段是 `size` + `version`，不是 `struct_size`

本文通用约定一节写"首字段必须是 `uint32_t struct_size`"。本树所有 Native ABI args 结构体实际共用 `a20_abi_header_t`（`size` + `version`），并由 `a20_validate_struct_header()` 统一校验：`size` 必须覆盖内核已知布局，`version` 为 0 或大于内核支持版本都拒绝。集群六个结构体沿用既有约定而不是另立一套——一套 ABI 里两种头部约定会让调用方无从判断该填哪个。校验语义与本文要求一致（覆盖完整布局、更大结构体按已知字段截断、版本只增不减），所以本文的意图由既有机制满足。

### route 的 next_hop 带显式长度

本文的 `cluster_route_args_t` 只给了 `uint8_t next_hop[16]`，而 `next_hop` 的解释随传输变化：loopback 4 字节虚拟节点号、udp 6 字节（IPv4+端口）、uart 2 字节短地址。固定 16 字节无法区分"没填"和"填了 6 字节"。实现加了 `next_hop_len`，并校验 `next_hop_len <= 16`。

### link_status 的输出字段不参与入参校验

本文未说明 `cluster_link_status` 的输出字段回写规则。这些字段是纯输出，内核在路由表存在之前没有可写内容，因此 W0 只校验头部与保留位，不因调用方未清零输出字段而报错——`a20_copy_struct_to_user` 已按用户 `size` 截断回写。

### 权限位与标签

`A20_RIGHT_CLUSTER_ADMIN` 是第 15 个 rights 位（`1 << 14`），已加入 `A20_RIGHTS_ALL`。`cluster_set_self` / `cluster_export` / `cluster_route` 要求该权限；`cluster_connect` / `cluster_event_subscribe` / `cluster_link_status` 不要求，所以普通任务可以连接远端但不能改变集群拓扑。W0 的 stub 只做参数校验，权限检查随真实处理器在 WA1 落地。

### WA1 实现期决策（kernel/cluster 落地，2026-10 核对）

6. **远程 CALL 的错误呈现在 v0 经"冻结 channel 表面"退化**。`a20_channel_call_args_t`
   （`kernel/include/abi/native/types.h:761`）没有 deadline 字段，"min(调用方 deadline,
   链路 max_deadline=30s)"退化为链路常量 30 s；且 parked 的 `channel_call`/`channel_recv`
   只能观察"消息到达"或"peer_closed"两种结局。因此远程 CLOSE、deadline 超时、ERROR 帧、
   链路 DOWN 在 v0 对调用线程**一律呈现为与本机 peer 关闭逐位一致的唤醒**
   （syscall 返回 `-A20_ERR_CANCELED`）；精确 errno 进 klog 与 02-§10 计数器。
   `A20_ERR_CLUSTER_TIMEOUT`/`A20_ERR_REMOTE_CLOSED` 在 v0 不作为 syscall 返回值出现
   （`NODE_UNREACHABLE`/`CLUSTER_UNSUPPORTED` 会：connect/route 阶段即时错误）。
   这是"不改变冻结 ABI"与"唯一映射 errno"之间的 v0 取舍，WA2 若要给等待中的调用方
   精确错误，需要先扩展 channel 的等待结果表面（独立提案）。
7. **`A20_RIGHT_CLUSTER_ADMIN` 的承载者**：本树没有任务级 A20 rights 位图（rights 只
   存在于句柄上），W0 登记的"权限检查随真实处理器在 WA1 落地"落地为：**set_self/
   export/route 要求调用任务 euid==0**（clusterd 以 root 运行）；connect/
   event_subscribe/link_status 不做权限检查，与本文口径一致。
8. **远程按名 connect**：`cluster_connect(node!=LOCAL, service_name!=NULL)` 返回
   `A20_ERR_CLUSTER_UNSUPPORTED`——远程名字解析是 clusterd（05-userspace.md）的职责，
   v0 内核只解析 slot。
9. **`A20_CONNECT_RELIABLE` 无条件拒绝**：v0 没有 `reliable.c`，seq/ACK 无法兑现，
   不检查对端 caps 直接返回 `A20_ERR_CLUSTER_UNSUPPORTED`。
10. **connect(LOCAL) 解析语义**：返回的新句柄指向一条**新建 channel pair 的调用端**；
    连接端作为一条零长度、携带一个 `A20_OBJ_CHANNEL_ENDPOINT` 句柄的消息注入被导出
    服务端点的接收队列（与远程 CALL 同一注入点——经导出 peer，但不占用其回复队列）。
    服务端按 `handle_count > 0` 识别本地连接请求（远程注入永不携带句柄——跨机 handle
    在代理端点的类型检查处被拒，本文跨机限制 1），随后在收到的连接端点上服务该连接。
    WA1 第一版曾直接复用导出 pair 的 peer 给本地调用方，与 export 回复 worker 共享
    同一队列：回复被另一侧消费者取走时本地调用没有 deadline，**可永久挂起**（WA1 终
    审 F6 实锤，原"演示场景不触发"的告诫不成立）。新语义下本地连接与远程回复路径
    完全解耦。**服务端约定**：导出的服务端点必须能接收携带句柄的消息才支持 LOCAL
    connect——typed channel 未把 `A20_OBJ_CHANNEL_ENDPOINT` 放进
    `recv_handle_types`/`send_handle_types` 的导出在注入时被拒（connect 返回
    `-A20_ERR_TYPE_MISMATCH`）；服务接收队列满时 connect 立即失败
    （`-A20_ERR_WOULD_BLOCK`，注入按 NONBLOCK），不在建链处阻塞。
11. **事件订阅的线上呈现**：`cluster_event_subscribe` 返回普通 EventQ；事件经
    `a20_event_notify` 投递到 cluster 令牌对象，event code = `32 + kind`
    （kind 0..3 对应 LINK_UP/LINK_DOWN/ROUTE_LOST/EXPORT_DROPPED），`data0` =
    节点哈希（EXPORT_DROPPED 时为 slot），`data1` = transport_id | reason<<8。
    不是本文设想的 32 字节 payload——EventQ 的 `a20_pending_event_t` 是既有冻结
    结构，v0 复用而不扩展。缓冲区满时沿用 eventq 的 wake-then-keep 丢新事件语义；
    溢出计数经 klog 暴露（link_status 输出结构无此字段，本文 §6 已冻结）。
12. **link_status**：`node_id=LOCAL` 汇总全部链路计数；loopback 链路 `rtt_us=0`、
    `last_hello_age_ms=0`（04-§5：loopback 无心跳）；无流量但有路由的 loopback
    链路报 UP（04-§5"常 UP"）。
13. **connect 的 `timeout_ms` 参数 v0 未消费**：v0 没有建链握手（loopback 无
    HELLO，02-§6 状态机不在 loopback 上运行），connect 即时返回；该字段按头部
    校验规则校验但不被读取。WA2 的 UDP 建链落地时接入。
14. **`A20_EXPORT_REPLACE` 已兑现**（WA1 终审 F5）：同名导出带 REPLACE 旗标注册时，
    旧导出按 revoke 语义当场退役（释放其 peer 引用、推 `EXPORT_DROPPED`（旧 slot）
    事件），新导出占用**新 slot**——slot 单调不复用（本文 §2），持有旧 slot 的远端
    caller 不会误中替换者，其未应答 CALL 由各自 30 s deadline 收尾。不带 REPLACE
    的同名注册仍返回 `A20_ERR_EXISTS`；表满且无同名冲突时仍 `A20_ERR_NO_SPACE`
    （REPLACE 先退役旧项再占位，故"表满 + 同名替换"必然成功）。表项可被新导出
    当场复用，而旧导出的回复 worker 可能仍阻塞在旧 peer 的 recv 上：worker 在
    spawn 时绑定该次注册的 peer 作为身份（自持一个引用），退出路径仅在表项仍
    属于该次注册时才清 `ex->peer` 并释放表项引用，故旧 worker 退出不会误清新
    导出的 peer（03 实现期记录 #13，自检 1d2 钉住）。
15. **`cluster_link_status` 的 LOCAL 识别**（WA1 终审 F4）：`node_id` 全零即
    `A20_NODE_ID_LOCAL`，按 §6 汇总所有链路；此前实现误与 self 比较，LOCAL 恒
    回落到路由查找报 `A20_ERR_NODE_UNREACHABLE`。node_id==self id 保持汇总语义
    不变。
