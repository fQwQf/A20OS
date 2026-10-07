# cluster ABI 使用与参考手册（v0）

> **实现状态（2026-10-07 按源码核对）**：本文描述的六个 syscall **全部已实现并接线到内核数据面**（不是 stub）：入口在 `kernel/abi/native/sys_native_cluster.c`，登记于 `kernel/abi/native/syscall_table.def:155-161`，号段 `0x0520`–`0x0525`（`kernel/include/abi/native/syscall_nr.h:102-108`）。配套已可用：loopback 传输（含虚拟节点与测试钩子，`kernel/cluster/loopback.c`）、UART 传输（`kernel/cluster/uart.c`）、内核自检（`clxselftest=1` 引导参数，`kernel/cluster/selftest.c`）、MCU 叶子协议面（`kernel/mcu/leaf.c`）。**尚未实现**：UDP 传输（WA2，`kernel/cluster/udp.c` 不存在，跨机 `channel_call` 不可用）、可靠性档（WA3，分片/ACK/重传均无）、用户态 clusterd/jobd（WD）、生产硬化（WE）——见文末『未实现 / 计划』。

本文是 cluster ABI 的**使用手册 + 字段级参考**：每个 syscall 给出用途、参数填法、权限、返回值、典型错误码与注意点；所有结构体/errno/限额以代码为准（引用到行号）。线协议（帧布局、HELLO、ERROR 映射）见 [02-wire-protocol.md](02-wire-protocol.md)，内核实现见 [03-kernel-impl.md](03-kernel-impl.md)，传输见 [04-transports.md](04-transports.md)。

## 通用约定：怎么填参数、怎么读返回值

- 所有参数经单个 `*_args_t` 结构体指针传入（syscall 第 0 参数），返回 `int64_t`：≥0 为成功值（句柄/槽位/0），<0 为 `-errno`。
- **结构体头部是 `size` + `version` 两个 `uint32_t`**（全树 Native ABI 统一约定，`a20_abi_header_t`；校验器 `a20_validate_struct_header()`，`kernel/abi/native/sys_validate.h:30-40`）。调用方填 `size = sizeof(struct)`、`version = 1`。校验规则：`size` 小于内核已知布局 → `A20_ERR_INVALID_ARGUMENT`；`version` 为 0 或大于内核支持版本 → `A20_ERR_INVALID_ARGUMENT`；`size` 更大时按内核已知字段截断——这是将来追加字段的唯一扩展机制，**禁止**改动已冻结结构体中间字段。
- 未命名字段（`reserved`）与 flags 保留位：调用方必须置零，非零 → `A20_ERR_INVALID_ARGUMENT`。
- 句柄均为 `a20_handle_t`（无效值 `A20_HANDLE_NULL`，`kernel/include/ipc/ipc.h:24`）。集群不新增对象类型——`cluster_connect` 返回的就是普通 `A20_OBJ_CHANNEL_ENDPOINT` 句柄。
- 用户指针不可读/不可写 → `A20_ERR_FAULT`（六个 syscall 一致，不再逐个列出）。

## 快速上手

### 跑内核自检（最快的验证方式）

内核命令行（bootargs）加 `clxselftest=1`，引导时内核自动跑 24 项数据面检查（set_self、路由增删查、导出、REPLACE、A/B echo 往返、deadline、LOCAL connect、句柄拒绝、在途上限、CLOSE 两分支、远程 CLOSE、deadline 扫描等，验收清单见 `kernel/cluster/selftest.c:1-44` 文件头注释）。每项打印 `[CLX-TEST] <name>: PASS/FAIL`，末尾汇总 `clx-selftest: <n>/<total>`。接线位置：`kernel/main.c:215-223` 无条件调用 `a20_clx_selftest_boot()`，引导参数门控在 `kernel/cluster/selftest.c:688-696`（无该参数时零开销返回）。

### 最小调用序列

```c
/* 1. 注入本节点身份（euid 0，只允许一次） */
a20_cluster_set_self_args_t self = {
    .size = sizeof(self), .version = 1,
    .caps = A20_CLUSTER_CAP_RELAY | A20_CLUSTER_CAP_RELIABLE,
    .node_id = my_node_id,          /* 非全零、非全 0xff */
};
syscall(A20_SYS_cluster_set_self, &self);

/* 2. 登记一条到"对端"的路由（loopback 虚拟节点为例） */
a20_cluster_route_args_t rt = {
    .size = sizeof(rt), .version = 1,
    .op = A20_ROUTE_REPLACE,
    .node_id = peer_node_id,
    .transport_id = A20_CLX_TRANSPORT_LOOPBACK,  /* 0 */
    .next_hop = { 0x02, 0, 0, 0 },  /* loopback: 4B 虚拟节点号 */
    .next_hop_len = 4,
};
syscall(A20_SYS_cluster_route, &rt);

/* 3. 服务端：导出一个已建好的 channel 服务端点 */
a20_cluster_export_args_t ex = {
    .size = sizeof(ex), .version = 1,
    .channel = server_ep_handle,    /* A20_OBJ_CHANNEL_ENDPOINT，需 R 权 */
    .service_name = "echo", .name_len = 4,
};
int64_t slot = syscall(A20_SYS_cluster_export, &ex);   /* ≥0：槽位号 */

/* 4. 调用端：拿到一条普通 channel 句柄 */
a20_cluster_connect_args_t cc = {
    .size = sizeof(cc), .version = 1,
    .node_id = peer_node_id, .slot = (uint32_t)slot,
};
a20_handle_t h = syscall(A20_SYS_cluster_connect, &cc);

/* 5. 之后就是普通 channel_call / channel_send / channel_recv */
```

第 5 步是这套 ABI 的核心：**connect 之后调用方完全用既有 channel syscall 通信，无需感知远程**（`kernel/abi/native/sys_native_cluster.c:200-251`）。

### 没有第二台机器也能对拍：loopback 虚拟节点

loopback 传输永远注册（`kernel/cluster/transport.c:383-390`），语义是零丢包、保序、发送即拷贝投递（`kernel/cluster/loopback.c:1-14`）。用 `cluster_route` 把某个 `node_id` 指到 `transport_id=0`、`next_hop`=4 字节虚拟节点号，就得到本机内的一个"虚拟对端"——上面的最小程序在一台机器上即可跑通。测试构建（`CONFIG_CLUSTER_TEST_HOOKS`）还可用引导参数 `clxhooks=drop=10,delay=5,dup=1,corrupt=1,part=100` 注入丢包/延迟/重复/损坏/分区（`kernel/cluster/loopback.c:7-13`）。

### 跨 QEMU 实例（当前只到 L2 可达）

`instances/qemu-riscv64-cluster-a.toml` / `-b.toml` + `tools/cluster-net-up.sh` 起双实例并互 ping 验证 L2 连通，步骤见 [02-udp-demo.md](02-udp-demo.md)。注意：这只验证网络环境——**UDP 集群传输（WA2）尚未实现**，跨机 `cluster_connect` 今天还走不通。

### 线协议对拍

帧编解码的参考实现与金样向量在 `tools/cluster-ref/`（`clframe.py`、`refdec.c`、`check_c_side.py`、`vectors/MANIFEST.json`），用于核对任何第二实现与内核编解码器（`kernel/cluster/frame.c`）逐字节一致。

## 节点标识

```c
typedef struct a20_node_id { uint8_t bytes[16]; } a20_node_id_t;   /* types.h:1105-1107 */
```

| 保留值 | 含义 |
|---|---|
| 全零 | `A20_NODE_ID_LOCAL`，本机。寻址 `(LOCAL, slot)` 走本机快路径，不序列化 |
| 全 `0xff` | `A20_NODE_ID_BROADCAST`，仅 SERVER 档、`SEND` 帧可用；MCU 叶子收到丢弃计数（`kernel/mcu/leaf.c:26-28`） |

两个保留值都不能作为 `cluster_set_self` 的参数（`kernel/abi/native/sys_native_cluster.c:38-47,102-103`）。内核只保存 `cluster_set_self` 注入的值；注入前一切远程操作返回 `A20_ERR_NODE_UNREACHABLE`（如 `kernel/abi/native/sys_native_cluster.c:225-226`）。节点 ID 的派生与全网分配是用户态 clusterd 的职责（见 [05-userspace.md](05-userspace.md)，**未实现**，WD 轨道）。

### 节点哈希

帧头携带 32 位节点哈希而非完整 ID：`fnv1a32(node_id.bytes, 16)`，FNV 偏移基 `2166136261`、质数 `16777619`，逐字节 `h = (h ^ b) * 16777619`（内核侧 `a20_clx_node_hash()`，`kernel/cluster/route.c:70-73`）。哈希冲突由 HELLO 协商检测（两端完整 ID 不同但哈希相同 → 后连入者被拒，`ERROR` 帧 + `A20_ERR_CLUSTER_UNSUPPORTED`），v0 不做在线重编址。

## 能力位（cluster_set_self 的 caps）

定义于 `kernel/include/abi/native/types.h:1110-1115`：

| 位 | 名称 | 含义 |
|---|---|---|
| 0 | `A20_CLUSTER_CAP_RELAY` | 可为其他节点转发帧（头节点/服务器置位，MCU 叶子清零）。**注意：v0 转发本身未实现**——发给非本机身份的帧一律丢弃计数（`kernel/cluster/transport.c:268-274` 注释） |
| 1 | `A20_CLUSTER_CAP_RELIABLE` | 宣称支持 ACK/重传/分片重组。**v0 无 `reliable.c`，该位只是声明**；`A20_CONNECT_RELIABLE` 仍被无条件拒绝（见 syscall 3） |
| 2 | `A20_CLUSTER_CAP_LEAF` | 叶子节点：只应答，不主动建链（MCU 档置位） |

未知位（`~A20_CLUSTER_CAPS_ALL`）→ `A20_ERR_INVALID_ARGUMENT`（`kernel/abi/native/sys_native_cluster.c:100-101`）。

## syscall 详表

号段 `0x0520`–`0x0525`（`kernel/include/abi/native/syscall_nr.h:102-108`），处理函数集中在 `kernel/abi/native/sys_native_cluster.c`。

### 1. `A20_SYS_cluster_set_self`（0x0520）— 注入本节点身份

```c
typedef struct a20_cluster_set_self_args {   /* types.h:1152-1159 */
    uint32_t       size;
    uint32_t       version;
    uint32_t       caps;          /* A20_CLUSTER_CAP_* */
    uint32_t       _pad;
    a20_node_id_t  node_id;
    uint64_t       reserved[2];   /* must be zero */
} a20_cluster_set_self_args_t;
```

- **功能**：给本节点命名（node_id + 能力位）。这是一切远程操作的前提。
- **权限**：调用任务 euid 必须为 0（clusterd 以 root 运行；本树没有任务级 rights 位图，`A20_RIGHT_CLUSTER_ADMIN` 的承载方式是 euid——见落地状态 #7，`kernel/abi/native/sys_native_cluster.c:64-69,104-105`）。
- **返回**：0 成功。
- **典型错误**：
  - `A20_ERR_INVALID_ARGUMENT`（12）：reserved 非零、caps 含未知位、node_id 为全零/全 0xff 保留值。
  - `A20_ERR_PERM`（1）：非 euid 0。
  - `A20_ERR_EXISTS`（10）：**第二次调用**——身份只允许注入一次（`kernel/cluster/route.c:54-60`）。
- **注意**：首次调用同时惰性拉起整个 cluster 核心（传输注册 + RX/housekeeping 线程，`a20_clx_core_init()`，`kernel/cluster/transport.c:372-413`）；从不触碰 cluster syscall 的内核引导不付任何开销。

### 2. `A20_SYS_cluster_export`（0x0521）— 把一个 channel 端点发布为集群服务

```c
typedef struct a20_cluster_export_args {   /* types.h:1161-1170 */
    uint32_t       size;
    uint32_t       version;
    uint32_t       flags;         /* A20_EXPORT_* */
    uint32_t       _pad;
    a20_handle_t   channel;       /* A20_OBJ_CHANNEL_ENDPOINT，需 R 权 */
    const char    *service_name;  /* UTF-8，不必 NUL 结尾，配合 name_len */
    uint32_t       name_len;      /* 1..64（A20_CLUSTER_SERVICE_NAME_MAX, types.h:1148） */
    uint32_t       reserved;      /* must be zero */
} a20_cluster_export_args_t;
/* 返回 ≥0：32 位槽位号 */
```

flags（`types.h:1118-1120`）：

| flag | 含义 |
|---|---|
| `A20_EXPORT_REPLACE` (bit0) | 同名服务已存在时替换（见下）；不带则同名返回 `A20_ERR_EXISTS` |
| `A20_EXPORT_LOCAL_ONLY` (bit1) | 只登记名→槽位，拒绝远程连接：远程 CALL 到该槽位收到 `ERROR(NOT_FOUND)`（`kernel/cluster/export.c:341-348`）；本机 `connect(LOCAL)` 不受影响 |

- **权限**：euid 0（`kernel/abi/native/sys_native_cluster.c:135-136`）；且调用前必须已 `set_self`，否则 `A20_ERR_NODE_UNREACHABLE`（:133-134）。
- **槽位**：per-node 单调分配、**永不复用**，0 保留（`kernel/cluster/export.c:25,108`）。REPLACE 时旧导出当场退役（推 `EXPORT_DROPPED` 事件、携带旧 slot），新导出占**新** slot——持有旧 slot 的远端 caller 不会误中替换者（`kernel/cluster/export.c:76-127`）。
- **典型错误**：
  - `A20_ERR_INVALID_ARGUMENT`（12）：flags 越界、reserved 非零、`name_len==0` 或 >64、`service_name==NULL`。
  - `A20_ERR_BAD_HANDLE`（5）：channel 为 `A20_HANDLE_NULL`、句柄失效或类型不是 `A20_OBJ_CHANNEL_ENDPOINT`；`A20_ERR_ACCESS`（7）：句柄缺 R 权（句柄查找语义，`kernel/abi/native/handle_table.c:465-490`）。
  - `A20_ERR_NODE_UNREACHABLE`（26）：未 `set_self`。
  - `A20_ERR_PERM`（1）：非 euid 0。
  - `A20_ERR_EXISTS`（10）：同名服务已存在且未带 REPLACE（`kernel/cluster/export.c:71-75`）。
  - `A20_ERR_NO_SPACE`（13）：导出表满（限额见下文；`kernel/cluster/export.c:98-104`）。
- **注意**：导出持有的是服务端点的 **peer** 引用；服务端关闭自己的句柄会被探测到并自动 revoke（`EXPORT_DROPPED` 事件，`kernel/cluster/export.c:6-13,152-178`）。导出的端点若想支持本机 `connect(LOCAL)`，必须能接收携带句柄的消息（见 syscall 3 注意点）。

### 3. `A20_SYS_cluster_connect`（0x0522）— 连接一个服务，拿回普通 channel 句柄

```c
typedef struct a20_cluster_connect_args {   /* types.h:1172-1182 */
    uint32_t       size;
    uint32_t       version;
    uint32_t       flags;         /* A20_CONNECT_* */
    uint32_t       slot;          /* cluster_export 的返回值 */
    a20_node_id_t  node_id;       /* 全零 LOCAL = 本机解析 */
    const char    *service_name;  /* 可选；非 NULL 时优先于 slot */
    uint32_t       name_len;
    uint32_t       timeout_ms;    /* 0 = 默认 5000；v0 未消费，见下 */
    uint32_t       reserved;      /* must be zero */
} a20_cluster_connect_args_t;
/* 返回 ≥0：普通 A20_OBJ_CHANNEL_ENDPOINT 句柄（R|W|STAT|DUP|TRANSFER） */
```

- **功能**：解析 `(node_id, slot|service_name)` 并返回一条 channel 句柄。**这是整套 ABI 的核心**：之后用普通 `channel_call`/`channel_send`/`channel_recv` 通信，调用方无需感知远程。远程句柄由内核代理端点（`kernel/cluster/remote_ep.c`）把消息编成 CALL 帧发出、把 CALL_REPLY 投回你的接收队列。
- **权限**：**不需要** cluster 管理权限——普通任务可以连接远端服务，但不能改变拓扑（`kernel/abi/native/sys_native_cluster.c:200-204`）。
- **两条解析路径**（`kernel/abi/native/sys_native_cluster.c:207-241`）：
  - `node_id == LOCAL`（全零）：本机解析，`slot` 或 `service_name` 二选一（都给则以名为准）。返回新建 channel pair 的调用端；连接端作为一条**零长度、携带一个 `A20_OBJ_CHANNEL_ENDPOINT` 句柄的消息**注入被导出服务的接收队列（`a20_clx_export_connect_local()`，`kernel/cluster/export.c:254-312`）。服务端按 `handle_count > 0` 识别这是本机连接请求（远程注入永不携带句柄）。
  - 远程节点：按路由表解析（无路由 → `A20_ERR_NODE_UNREACHABLE`），创建代理端点。**v0 只按 slot 解析**；远程按名解析是 clusterd 的职责，内核直接返回 `A20_ERR_CLUSTER_UNSUPPORTED`（`kernel/abi/native/sys_native_cluster.c:227-231`）。
- **典型错误**：
  - `A20_ERR_INVALID_ARGUMENT`（12）：flags 越界、reserved 非零、`name_len` 越界或与 `service_name` 不配、`node_id` 为 BROADCAST、LOCAL 且名/槽都没给。
  - `A20_ERR_NODE_UNREACHABLE`（26）：远程且未 `set_self`；无路由。
  - `A20_ERR_CLUSTER_UNSUPPORTED`（29）：远程按名 connect；或 flags 带 `A20_CONNECT_RELIABLE`——v0 没有可靠档实现，**无条件拒绝**（`kernel/cluster/remote_ep.c:467-472`）。
  - `A20_ERR_NO_SPACE`（13）：远程端点代理表满（限额见下文；`kernel/cluster/remote_ep.c:498-503`）。
  - `A20_ERR_NOT_FOUND`（24）：LOCAL 解析时 slot/服务名不存在或服务端点已消亡（`kernel/cluster/export.c:265-274`）。
  - `A20_ERR_TYPE_MISMATCH`（23）：LOCAL connect 的服务是 typed channel 且不接受端点句柄（`kernel/cluster/export.c:248-252` 注释）。
  - `A20_ERR_WOULD_BLOCK`（18）：LOCAL connect 时服务接收队列满——注入按 NONBLOCK 执行，**建链处永不阻塞**（`kernel/cluster/export.c:299-302`）。
- **注意点**：
  - **远程调用强制超时**：代理上的每个 CALL 挂 30 s 链路 deadline（`CLX_CALL_DEADLINE_MS`，`kernel/cluster/clx_internal.h:59-62`），housekeeping 线程每 250 ms 扫描（`kernel/cluster/transport.c:330-337`），超时拆除代理。**任何情况下远程调用不会无限期阻塞。**
  - **远程失败的呈现形式**：受冻结 channel ABI 所限，远程 CLOSE、deadline 超时、ERROR 帧、链路 DOWN 对阻塞中的调用线程**一律呈现为与本机 peer 关闭逐位一致的唤醒**（`channel_call`/`channel_recv` 返回 `-A20_ERR_CANCELED`）；精确 errno 进 klog 与链路计数器（落地状态 #6，`kernel/cluster/remote_ep.c:39-44,384-393,425-447`）。`A20_ERR_CLUSTER_TIMEOUT`/`A20_ERR_REMOTE_CLOSED` 因此**不会**作为 syscall 返回值出现；`NODE_UNREACHABLE`/`CLUSTER_UNSUPPORTED`/`NO_SPACE` 这些 connect 阶段即时错误会。
  - `timeout_ms` 字段会被校验但 **v0 不消费**：v0 无建链握手，connect 即时返回（落地状态 #13）。UDP 建链（WA2）落地时接入。
  - 载荷上限：单条消息 ≤ `A20_CH_MAX_DATA`（64 KiB，`kernel/include/ipc/ipc.h:147`）；v0 无分片，超过路由 MTU 预算的消息被丢弃并记 klog（`kernel/cluster/remote_ep.c:634-642`）。

### 4. `A20_SYS_cluster_route`（0x0523）— 管理路由表

```c
typedef struct a20_cluster_route_args {   /* types.h:1184-1194 */
    uint32_t       size;
    uint32_t       version;
    uint32_t       op;            /* A20_ROUTE_* */
    uint32_t       transport_id;  /* 0=loopback 1=udp 2=uart */
    a20_node_id_t  node_id;
    uint8_t        next_hop[16];  /* 链路地址，解释随传输 */
    uint32_t       next_hop_len;  /* 实际长度：loopback=4，uart=2，udp=6（WA2） */
    uint32_t       metric;        /* 保留记录，v0 不参与选路 */
    uint32_t       reserved;      /* must be zero */
} a20_cluster_route_args_t;
```

op（`types.h:1127-1129`）：`A20_ROUTE_ADD`=0（已存在 → `A20_ERR_EXISTS`）、`A20_ROUTE_DEL`=1（不存在 → `A20_ERR_NOT_FOUND`）、`A20_ROUTE_REPLACE`=2（upsert，**推荐**）。

- **权限**：euid 0（`kernel/abi/native/sys_native_cluster.c:279-280`）。
- **语义**：v0 只支持精确 node_id 匹配，每个目标只保留一条路由（REPLACE 就地覆盖；`kernel/cluster/route.c:91-128`），无前缀/子网。`metric` 被保存但暂不参与选择（`kernel/cluster/route.c:5-7`）。路由表是内核唯一拓扑来源；计算与注入是 clusterd（未实现，WD）的职责。
- **`next_hop` 解释随传输**（`transport_id` 有效值 `types.h:1144-1146`）：loopback = 4 字节虚拟节点号（同时把该节点注册进 loopback 虚拟节点表，`kernel/cluster/route.c:140-141,169-170`）；uart = 2 字节短地址；udp = IPv4(4B)+port(2B)（传输本体 WA2 未实现）。`next_hop_len` 必须 ≤16；非 DEL 操作 `next_hop_len==0` → `A20_ERR_INVALID_ARGUMENT`（`kernel/abi/native/sys_native_cluster.c:269-278`）。
- **典型错误**：`A20_ERR_INVALID_ARGUMENT`（op 越界、保留位非零、`next_hop_len>16`、非 DEL 且 node_id 为保留值、transport_id 未知）；`A20_ERR_PERM`；`A20_ERR_EXISTS`（ADD 撞名）；`A20_ERR_NOT_FOUND`（DEL 未中）；`A20_ERR_NO_SPACE`（表满，`kernel/cluster/route.c:107-108`）。
- **注意**：DEL 成功会推 `ROUTE_LOST` 事件（`kernel/cluster/route.c:159-160`）。查询本机 node_id 不需要路由表项——self 总是解析到 loopback（`kernel/cluster/route.c:174-203`）。

### 5. `A20_SYS_cluster_event_subscribe`（0x0524）— 订阅集群事件

```c
typedef struct a20_cluster_event_subscribe_args {   /* types.h:1196-1201 */
    uint32_t  size;
    uint32_t  version;
    uint32_t  mask;      /* A20_CLX_EVENT_* 位或 */
    uint32_t  reserved;  /* must be zero */
} a20_cluster_event_subscribe_args_t;
/* 返回 ≥0：普通 A20_OBJ_EVENT_QUEUE 句柄（R|W） */
```

mask（`types.h:1132-1136`）：`A20_CLX_EVENT_LINK_UP` / `LINK_DOWN` / `ROUTE_LOST` / `EXPORT_DROPPED`。

- **权限**：无要求。返回普通 EventQ，用既有 `eventq_wait` 读取（`kernel/abi/native/sys_native_cluster.c:314-329`）。
- **事件呈现**（不是 32 字节 payload，而是复用冻结的 `a20_pending_event_t`，落地状态 #11）：`type = 32 + kind`（kind 0..3 对应上面四个事件，`kernel/cluster/clx_internal.h:64-72`），`data0` = 节点哈希（`EXPORT_DROPPED` 时为被丢弃的 slot），`data1` = `transport_id | reason<<8`（`kernel/cluster/transport.c:558-563`）。
- **典型错误**：`A20_ERR_INVALID_ARGUMENT`（mask 越界、reserved 非零）；`A20_ERR_NO_MEMORY`（EventQ 创建失败或订阅表满——最多 8 个订阅者，`kernel/cluster/transport.c:516,538-555`）。
- **注意**：成员事件**允许丢失**——队列满时按 eventq 的 wake-then-keep 语义丢新事件，溢出计数经 klog 暴露（`kernel/cluster/transport.c:570-579`）。依赖事件流维持成员关系的消费者（未来的 clusterd）必须以周期全量同步兜底。`LINK_UP/DOWN` 事件当前由 UART 链路事件产生（`kernel/cluster/transport.c:495-507`）；loopback 无链路状态机。

### 6. `A20_SYS_cluster_link_status`（0x0525）— 查询链路状态与计数器

```c
typedef struct a20_cluster_link_status_args {   /* types.h:1203-1218 */
    uint32_t      size;
    uint32_t      version;
    uint32_t      reserved;
    uint32_t      _pad;
    a20_node_id_t node_id;       /* 全零 LOCAL = 汇总所有链路 */
    /* out */
    uint32_t      state;         /* A20_CLX_LINK_DOWN/SUSPECT/UP（types.h:1139-1141） */
    uint32_t      rtt_us;        /* v0 恒 0（loopback 无采样，MCU 档恒 0） */
    uint64_t      tx_frames;
    uint64_t      rx_frames;
    uint64_t      tx_drops;
    uint64_t      rx_drops;
    uint64_t      retransmits;   /* v0 恒 0（无重传，WA3） */
    uint64_t      last_hello_age_ms;  /* loopback 恒 0（无心跳） */
} a20_cluster_link_status_args_t;
```

- **权限**：无要求。输出字段纯回写、不参与入参校验（无需清零，`kernel/abi/native/sys_native_cluster.c:343-345`），回写按用户 `size` 截断（`a20_copy_struct_to_user`，`kernel/abi/native/sys_validate.h:44`）。
- **语义**（`kernel/cluster/transport.c:417-491`）：`node_id` 全零或等于本机 id → 汇总全部链路计数器，任一路由 UP 则 `state=UP`；其他 node_id → 查路由后给出该链路计数。有路由但尚无流量的 loopback 链路报 UP（"常 UP"，`kernel/cluster/transport.c:483-488`）。
- **典型错误**：`A20_ERR_INVALID_ARGUMENT`（reserved 非零）；`A20_ERR_NODE_UNREACHABLE`（未 `set_self`，或 node_id 无路由）。

## errno 参考

集群新增四个 errno，数值 26–29，接在既有 1–25 之后（`kernel/include/ipc/ipc.h:108-111`；一经分配永不复用）：

| 名称 | 值 | 触发场景（唯一映射，禁止混用） |
|---|---|---|
| `A20_ERR_NODE_UNREACHABLE` | 26 | 无路由、链路 DOWN、TTL 耗尽、未 `set_self` |
| `A20_ERR_CLUSTER_TIMEOUT` | 27 | 远程 CALL 超 deadline；建链超时；重组超时。**v0 不作为 syscall 返回值出现**（等待中的调用方看到 `-A20_ERR_CANCELED`，见 syscall 3 注意点）；出现在 ERROR 帧与 klog |
| `A20_ERR_REMOTE_CLOSED` | 28 | 对端端点关闭（收到 CLOSE 或对端句柄已释放）。v0 呈现规则同上 |
| `A20_ERR_CLUSTER_UNSUPPORTED` | 29 | 跨机传 handle、跨机捐赠、MCU 档分片/广播、`RELIABLE` 协商失败、HELLO 哈希冲突、远程按名 connect |

本文早期草稿使用的 `A20_ERR_ALREADY_EXISTS` / `A20_ERR_RESOURCE_LIMIT` / `A20_ERR_INVALID_ARGS` 在代码里的实际名字是 `A20_ERR_EXISTS`(10) / `A20_ERR_NO_SPACE`(13) / `A20_ERR_INVALID_ARGUMENT`(12)（`kernel/include/ipc/ipc.h:88-91`），映射关系见落地状态。

## 限额

定义于 `kernel/include/abi/native/resource.h:58-78`，按编译档（`CONFIG_CLUSTER_PROFILE`）选取（`kernel/cluster/clx_internal.h:34-49`）。超限返回 `A20_ERR_NO_SPACE`（13）：

| 限额 | MCU | DEFAULT | ABSOLUTE |
|---|---|---|---|
| 路由表项 | 4 | 256 | 4096 |
| 导出槽位 | 4 | 64 | 256 |
| 远程端点代理 | 2 | 128 | 1024 |
| 重组缓存（字节） | 0（禁用） | 256 KiB | 4 MiB |
| 待确认未应答 CALL（每代理在飞） | 1 | 64 | 512 |

注意：在飞 CALL 打满时新消息**不排队**——代理直接丢弃该消息并记 klog，调用方经 30 s deadline 收尾（`kernel/cluster/remote_ep.c:663-670`；限额语义"不得阻塞等待空位"）。重组缓存档值为 WA3 预留，v0 无重组代码。

## 跨机限制（v0 硬规则）

1. 远程端点上的 `channel_call`/`channel_send` 携带 handle → 发送失败，消息不发出。代理端点是 typed channel（`send_handle_types = 0` + `A20_CHAN_TYPE_REMOTE`，`kernel/cluster/remote_ep.c:451-459`），ABI 层把类型拒绝映射为 `A20_ERR_CLUSTER_UNSUPPORTED`（`kernel/abi/native/sys_native_cluster.c:71-85`）。
2. 捐赠族调用（`a20_channel_send_dwc`、`recv_donate`）目标是远程端点 → 同上。
3. 载荷上限沿用 `A20_CH_MAX_DATA`（64 KiB，`kernel/include/ipc/ipc.h:147`）；再受路由 MTU 预算约束（UART 档单帧 MTU 256 字节，`kernel/cluster/uart.h:165`；v0 不可分片，超预算丢弃）。
4. 目标 `A20_NODE_ID_LOCAL` 的一切调用走本机快路径，语义与本机 channel **逐位一致**，不受 1–3 限制。

## ABI 稳定性规则

- 已冻结结构体：只可追加尾部字段（配合 `size`+`version` 头），禁止改中间。
- 帧协议 `ver` 字段：兼容窗口为一个版本——vN 的实现必须能收 vN-1 的 HELLO 并降级协商；不能协商时发 `ERROR` 帧并拒绝建链。当前线版本 `A20_CLX_WIRE_VER = 0`（`kernel/cluster/uart.h:70`）。
- errno 数值一经分配永不复用；废弃的码保留注释。

## 未实现 / 计划（路线图）

以下功能**当前不可用**，描述仅作设计意图保留；实现顺序与验收标准见 [impl-prompts.md](impl-prompts.md) 对应轨道：

| 功能 | 轨道 | 现状 |
|---|---|---|
| UDP 传输 `kernel/cluster/udp.c`、跨机 `channel_call`、建链握手（消费 `timeout_ms`） | WA2 | 文件不存在；`transport_id=1` 的路由可登记但发送失败。双实例网络环境已就绪（[02-udp-demo.md](02-udp-demo.md)） |
| 可靠性：`reasm.c`/`reliable.c`，分片重组、seq/ACK/重传/去重兑现 `A20_CONNECT_RELIABLE` | WA3 | 未实现；RELIABLE flag 无条件 `A20_ERR_CLUSTER_UNSUPPORTED` |
| 用户态 clusterd / jobd：节点 ID 派生、SWIM 成员、远程 `service://` 按名解析、tree-reduce、demo-wordcount | WD | `user/svc/` 下无这些文件 |
| 生产硬化：`check-cluster-*` CI 门禁、故障注入矩阵、模糊、soak、性能实测 | WE | 未落地 |

## 落地状态与实现期决策（W0 起）

本节记录本文与代码现实不一致之处。冲突仲裁顺序是代码现实 > 子系统规范 > 顶层设计，所以每一处偏差都写在这里而不是留给读者去猜。

### 2026-10-07 核对（本次改写）

- 六个 syscall 已全部接线到 `kernel/cluster/` 数据面（W0 stub 阶段已结束），本文档由此从"实现依据"改写为"使用与参考手册"；正文的结构体布局、错误码名、事件呈现、link_status 语义均已按代码现实重写（引用到行号），上文不再复述旧约定。
- 新登记的代码注释偏差：`kernel/cluster/clx_internal.h:82-83` 注释称 `a20_clx_selftest_boot()` "Called at the end of core_init"，`kernel/cluster/selftest.c:5` 注释称 core_init 拉起自检线程；实际调用点是 `kernel/main.c:222`（无条件调用，引导参数门控在 `kernel/cluster/selftest.c:692`），自检线程自行调用 `a20_clx_core_init()`（`kernel/cluster/selftest.c:209`）。仅注释滞后，行为一致，未改代码。
- 其余偏差沿用下列 W0/WA1 登记，逐条核对仍与代码一致。

### 已有 errno 的复用

本文早期草稿写的 `A20_ERR_ALREADY_EXISTS` / `A20_ERR_NOT_FOUND` / `A20_ERR_RESOURCE_LIMIT` / `A20_ERR_INVALID_ARGS` 在本树的 errno 空间里名字不同。复用现有码，不新增同义码：

| 草稿用词 | 实际实现 | 说明 |
|---|---|---|
| `A20_ERR_ALREADY_EXISTS` | `A20_ERR_EXISTS`（10） | `cluster_set_self` 二次调用、`cluster_export` 同名冲突、route ADD 撞名 |
| `A20_ERR_NOT_FOUND` | `A20_ERR_NOT_FOUND`（24） | 远端 slot / 服务名不存在；route DEL 未中 |
| `A20_ERR_RESOURCE_LIMIT` | `A20_ERR_NO_SPACE`（13） | 各项限额打满 |
| `A20_ERR_INVALID_ARGS` | `A20_ERR_INVALID_ARGUMENT`（12） | 保留位非零、flags 越界、node_id 为保留值 |

四个新集群 errno 落在 26–29，接在既有 1–25 之后：`NODE_UNREACHABLE` / `CLUSTER_TIMEOUT` / `REMOTE_CLOSED` / `CLUSTER_UNSUPPORTED`（`kernel/include/ipc/ipc.h:108-111`）。

### 结构体首字段是 `size` + `version`，不是 `struct_size`

本文早期草稿通用约定一节写"首字段必须是 `uint32_t struct_size`"。本树所有 Native ABI args 结构体实际共用 `a20_abi_header_t`（`size` + `version`），并由 `a20_validate_struct_header()` 统一校验（`kernel/abi/native/sys_validate.h:30-40`）：`size` 必须覆盖内核已知布局，`version` 为 0 或大于内核支持版本都拒绝。集群六个结构体沿用既有约定而不是另立一套——一套 ABI 里两种头部约定会让调用方无从判断该填哪个。校验语义与草稿要求一致（覆盖完整布局、更大结构体按已知字段截断、版本只增不减），所以草稿的意图由既有机制满足。正文已按现实改写。

### route 的 next_hop 带显式长度

草稿的 `cluster_route_args_t` 只给了 `uint8_t next_hop[16]`，而 `next_hop` 的解释随传输变化：loopback 4 字节虚拟节点号、udp 6 字节（IPv4+端口）、uart 2 字节短地址。固定 16 字节无法区分"没填"和"填了 6 字节"。实现加了 `next_hop_len`（`types.h:1191`），并校验 `next_hop_len <= 16`（`kernel/abi/native/sys_native_cluster.c:269-270`）。

### link_status 的输出字段不参与入参校验

草稿未说明 `cluster_link_status` 的输出字段回写规则。这些字段是纯输出，内核在路由表存在之前没有可写内容，因此只校验头部与保留位，不因调用方未清零输出字段而报错——`a20_copy_struct_to_user` 已按用户 `size` 截断回写（`kernel/abi/native/sys_native_cluster.c:343-345,357`）。

### 权限位与标签

`A20_RIGHT_CLUSTER_ADMIN` 是第 15 个 rights 位（`1 << 14`），已加入 `A20_RIGHTS_ALL`（`kernel/include/ipc/ipc.h:65,67-72`）。设计上 `cluster_set_self` / `cluster_export` / `cluster_route` 要求该权限；`cluster_connect` / `cluster_event_subscribe` / `cluster_link_status` 不要求，所以普通任务可以连接远端但不能改变集群拓扑。（W0 时登记的原文："W0 的 stub 只做参数校验，权限检查随真实处理器在 WA1 落地"——WA1 的落地形态见下条 #7。）

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
   event_subscribe/link_status 不做权限检查，与本文口径一致
   （`kernel/abi/native/sys_native_cluster.c:64-69`）。
8. **远程按名 connect**：`cluster_connect(node!=LOCAL, service_name!=NULL)` 返回
   `A20_ERR_CLUSTER_UNSUPPORTED`——远程名字解析是 clusterd（05-userspace.md）的职责，
   v0 内核只解析 slot（`kernel/abi/native/sys_native_cluster.c:227-231`）。
9. **`A20_CONNECT_RELIABLE` 无条件拒绝**：v0 没有 `reliable.c`，seq/ACK 无法兑现，
   不检查对端 caps 直接返回 `A20_ERR_CLUSTER_UNSUPPORTED`
   （`kernel/cluster/remote_ep.c:467-472`）。
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
    （`-A20_ERR_WOULD_BLOCK`，注入按 NONBLOCK），不在建链处阻塞
    （`kernel/cluster/export.c:254-312`）。
11. **事件订阅的线上呈现**：`cluster_event_subscribe` 返回普通 EventQ；事件经
    `a20_event_notify` 投递到 cluster 令牌对象，event code = `32 + kind`
    （kind 0..3 对应 LINK_UP/LINK_DOWN/ROUTE_LOST/EXPORT_DROPPED），`data0` =
    节点哈希（EXPORT_DROPPED 时为 slot），`data1` = transport_id | reason<<8
    （`kernel/cluster/clx_internal.h:64-72`，`kernel/cluster/transport.c:558-563`）。
    不是草稿设想的 32 字节 payload——EventQ 的 `a20_pending_event_t` 是既有冻结
    结构，v0 复用而不扩展。缓冲区满时沿用 eventq 的 wake-then-keep 丢新事件语义；
    溢出计数经 klog 暴露（link_status 输出结构无此字段，§6 已冻结）。
12. **link_status**：`node_id=LOCAL` 汇总全部链路计数；loopback 链路 `rtt_us=0`、
    `last_hello_age_ms=0`（04-§5：loopback 无心跳）；无流量但有路由的 loopback
    链路报 UP（04-§5"常 UP"）（`kernel/cluster/transport.c:417-491`）。
13. **connect 的 `timeout_ms` 参数 v0 未消费**：v0 没有建链握手（loopback 无
    HELLO，02-§6 状态机不在 loopback 上运行），connect 即时返回；该字段按头部
    校验规则校验但不被读取。WA2 的 UDP 建链落地时接入。
14. **`A20_EXPORT_REPLACE` 已兑现**（WA1 终审 F5）：同名导出带 REPLACE 旗标注册时，
    旧导出按 revoke 语义当场退役（释放其 peer 引用、推 `EXPORT_DROPPED`（旧 slot）
    事件），新导出占用**新 slot**——slot 单调不复用（§2），持有旧 slot 的远端
    caller 不会误中替换者，其未应答 CALL 由各自 30 s deadline 收尾。不带 REPLACE
    的同名注册仍返回 `A20_ERR_EXISTS`；表满且无同名冲突时仍 `A20_ERR_NO_SPACE`
    （REPLACE 先退役旧项再占位，故"表满 + 同名替换"必然成功）。表项可被新导出
    当场复用，而旧导出的回复 worker 可能仍阻塞在旧 peer 的 recv 上：worker 在
    spawn 时绑定该次注册的 peer 作为身份（自持一个引用），退出路径仅在表项仍
    属于该次注册时才清 `ex->peer` 并释放表项引用，故旧 worker 退出不会误清新
    导出的 peer（03 实现期记录 #13，自检 1d2 钉住）
    （`kernel/cluster/export.c:76-129`，`kernel/cluster/remote_ep.c:133-154,687-779`）。
15. **`cluster_link_status` 的 LOCAL 识别**（WA1 终审 F4）：`node_id` 全零即
    `A20_NODE_ID_LOCAL`，按 §6 汇总所有链路；此前实现误与 self 比较，LOCAL 恒
    回落到路由查找报 `A20_ERR_NODE_UNREACHABLE`。node_id==self id 保持汇总语义
    不变（`kernel/cluster/transport.c:435-465`）。
