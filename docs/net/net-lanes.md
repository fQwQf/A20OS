# 网络 Lane 并发架构与数据通路 (Network Lanes)

A20OS 网络栈采用基于 Lane 的分片并发模型，用于在多核 SMP 环境下消除集中锁瓶颈。普通单播 TCP/UDP 热路径数据报由所属 Lane 锁并行处理；广播、组播、ARP、ND6、ICMP 与控制配置维护统一走全 Lane 控制屏障（Control Barrier）同步。连接跟踪（Conntrack）由独立 CT 锁同步。单 Lane 配置（`CONFIG_NET_LANES=1`）保留单锁与中断安全语义；多 Lane 配置（`CONFIG_NET_LANES > 1`）支持服务器工作负载下的高并发吞吐。

---

## 1. 核心架构与并发分层

```
+-------------------------------------------------------------+
|               用户态 Socket / 系统调用接口                   |
|          per-socket 锁 (net_socket_t.lock)                  |
+-------------------------------------------------------------+
                              |
                              v
+-------------------------------------------------------------+
|             网络协议分片数据面 (Lane 0 .. N-1)              |
|        PCB 分桶哈希 (4-tuple) / per-lane 接收队列 / 锁       |
+-------------------------------------------------------------+
         |                                           |
         v                                           v
+-----------------------+                   +-----------------------+
|  全局共享控制面        |                   |  网络设备 Ingress     |
|  ARP / ND6 (seqlock)  |                   |  网卡中断出入队列     |
|  路由表 (seqlock)     |                   |  Bottom-Half 调度     |
|  Conntrack (独立 CT)  |                   |  Control Barrier      |
+-----------------------+                   +-----------------------+
```

系统在部署规模与资源占用上保持正交：
- **资源轴**：由 `kernel/include/net/profile.h` 的三档（EMBEDDED / DEFAULT / SERVER）编译期常量决定。
- **并发轴**：由 `CONFIG_NET_LANES` 常量指定 Lane 数量。
- **单 Lane 保持等价性**：当 `CONFIG_NET_LANES == 1` 时，系统严格保持单 Lane 锁与 IRQ-save 兼容语义。

---

## 2. Lane 归属与哈希映射算法

### 2.1 4 元组哈希确定性

一条 TCP 连接的 PCB 从建立到关闭绑定在固定 Lane 上，归属由 `(local_ip, local_port)` 哈希决定（`net_lane_hash()`）：

- **建连时**：使用本地 `(ip, port)`；
- **收到包时**：使用 `(dst_ip, dst_port)`。

哈希算法中端口在前、地址在后，仅凭线上报文头部字段在各 CPU 上计算出严格一致的结果。IPv6 取目的地址低 32 位参与哈希。非 IP 协议族（AF_UNIX / AF_PACKET / AF_NETLINK）保持创建时的临时归属。

### 2.2 通配监听（Wildcard Listener）哨兵桶

对于 `bind(0.0.0.0)` 的通配监听套接字，其本地 IP 在握手前未知，与入向报文的具体目的 IP 无法直接通过单次哈希匹配。系统采用哨兵桶设计：

```c
#if CONFIG_NET_LANES > 1
#define NET_PCB_LANE_ANY        CONFIG_NET_LANES      /* 哨兵桶下标，不与真实 lane 冲突 */
#define NET_PCB_LANE_BUCKETS    (CONFIG_NET_LANES + 1)
#else
#define NET_PCB_LANE_ANY        0
#define NET_PCB_LANE_BUCKETS    1
#endif
```

- **注册规则**：`local_ip` 为全零时归入 `NET_PCB_LANE_ANY` 哨兵桶；指定具体 IP 时归入哈希桶；
- **查找规则**：先探测 `hash(dst_ip, dst_port)` 对应桶，若未命中再探测 `NET_PCB_LANE_ANY` 哨兵桶；
- **约束**：单 Lane 配置下不编译哨兵桶分支；通配 PCB 严禁同时注册进两个桶，避免双重链表破坏。

### 2.3 IP 分片散列

对于分片报文（`MF == 1 || frag_offset != 0`），后续分片缺少传输层端口信息，散列算法统一退化为仅对源/目的 IP 进行哈希，保证同一数据报的所有分片被归集至同一 CPU/Lane 处理。

---

## 3. 数据通路与生命周期

### 3.1 IRQ 快速入队与上下文解耦

1. **网卡中断处理（IRQ 侧）**：`a20_lwip_process_netif_irq_locked()`（`kernel/net/backends/lwip/lwip_stack.c`）仅解析定 Lane 所需的报头，将帧快速写入对应 Lane 的接收队列并退出中断，由 `CONFIG_NET_RX_IRQ_BUDGET` 限制单次硬中断预算；
2. **进程上下文排空（Drain 侧）**：`a20_lwip_lane_drain_locked()` 通过原子 `net_lane_rx_claim()` 获取当前 Lane 独占消费权，逐帧调用协议栈输入；
3. **防死锁与调度轮询点**：为避免阻塞读任务与下半部死锁，`kernel/core/progress.c` 在 `kernel_progress_run_bottom_halves()` 中设置无条件调度轮询点，在每次时钟 tick、调度决策和 idle pass 时主动执行 Lane 帧排空。

---

## 4. Per-Socket 锁模型与锁序规范

为消除同桶内数百个 Socket 共享桶锁造成的伪争用，系统采用针对单个 Socket 的细粒度锁（`net_socket_t.lock`）：

### 4.1 锁层次结构（外到内）

```
net_bucket[b].lock (桶管理锁，仅保护 slot 表与空闲位图)
    |
    v
net_socket_t.lock (保护套接字队列、状态、引用与等待队列)
```

- **桶锁绝不嵌套在 Socket 锁之内获取**，消除反向死锁隐患；
- **双 Socket 加锁**：跨套接字操作统一使用 `net_sock_lock2(a, b)`，按套接字内存地址升序加锁；
- **安全销毁两阶段**：
  1. 第一阶段在 `net_socket_t.lock` 内标记 `closed`、摘除队列并收集唤醒任务；
  2. 释放 Socket 锁后，在外部获取桶锁执行注销（`net_socket_unregister()`）并释放内存。

---

## 5. 全局共享状态

| 共享状态 | 同步机制 | 设计原因 |
|---|---|---|
| **ARP / ND6 缓存** | `seqlock` | 读高频且无需持互斥锁，写低频 |
| **路由表 / netif 列表** | `seqlock` | 数据包转发只读查表，拓扑变更低频 |
| **Socket 注册表** | 分片哈希 + 引用计数 | 冷路径由桶锁维护 slot 分配 |
| **连接跟踪表 (`g_ct[]`)** | 独立全局 CT 锁 | 五元组 NAT 绑定属于端到端双向流，不能按 Lane 割裂 |

---

## 6. 观测接口

`/proc/net/status` 在多 Lane 开启时暴露每 Lane 活跃度与排队指标：

```text
lanes: count=4 sockets=8 occupancy: 3 2 2 1
rx lane / rx / drop / staged
```

- `occupancy`：报告各 Lane 承载的活跃 Socket 分布；
- `staged`：报告当前已入队但未排空的单调在途帧计数。

---

## 7. 验证门禁与基线规范

| 门禁目标 | 验证重点 |
|---|---|
| `make smoke-net-lanes` | 验证多 Lane 配置下状态接口渲染与参数一致性 |
| `make smoke-net-lanes-n1` | 验证单 Lane 与多 Lane 构建下基准网络负载结果一致 |
| `make check-net-lane-ownership` | 编译期断言哈希映射一致性 |
| `make smoke-network-suite` | 完整端到端 TCP/UDP 压力套件 |

### 噪声带与基准纪律

在 QEMU TCG 仿真环境下，自旋锁累计计数量纲受多核时间片抖动影响较大（历史记录中存在 0–920024 的累计自旋噪声带）。门禁断言严格基于结构性行为（数据一致性、校验和、无故障退出），不依赖容易抖动的自旋计数值。

历史研发阶段的 Panic 现场调查与二分排查记录已归档至目录 `docs/archive/net/`。
