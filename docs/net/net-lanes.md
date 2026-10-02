# 网络 lane

最后核实：阶段 A 已落地（`kernel/include/net/net_lane.h`、`kernel/net/net_lane.c`、
`net_socket_t.lane`、`net_socket_lane_of_addr()`、`/proc/net/status` 的 lanes 行）。
阶段 B 及以后**尚未实现**，本文件记录它们的顺序与前置条件。

## 为什么是 lane

两种部署缺的是不同的东西：

| | 服务器 | 嵌入式 |
|---|---|---|
| 稀缺 | 锁吞吐、CPU 核数 | RAM、Flash |
| 典型失效 | 锁护航、cache line 弹跳 | OOM、ISR 超时、丢 deadline |

所以规模必须做成**与资源占用正交的一根轴**：

- 资源轴 = `kernel/net/net_profile.h` 的三档（EMBEDDED / DEFAULT / SERVER），纯编译期常量
- 规模轴 = 本文件的 lane 数，同一个 `CONFIG_NET_LANES` 常量

关键性质：**`CONFIG_NET_LANES == 1` 的行为必须与 lane 存在之前完全相同。** 嵌入式
构建把每个 lane 索引折叠为常量 0，编译结果就是改动前的代码。这条性质不是口号：
`net_lane_of()` 是取模 `CONFIG_NET_LANES` 的表达式，在 1 时被编译器折叠。
已实测两档 `net_stress_test` 输出**逐字节一致**（`NET_STRESS_TEST: PASS (4 parallel transfers, 4 rounds x 1048576 B)`）。

**这条性质有一个必须守住的实现约束**：通配 listener 的哨兵桶只能在
`#if CONFIG_NET_LANES > 1` 下存在。否则 N=1 会多出一次对空桶的探测，
行为仍正确但"编译结果就是改动前的代码"这句话就不成立了 —— 而嵌入式
依赖的正是后者，不只是前者。

## 分层与前置

```
A  lane 骨架与归属哈希            无（本文件已完成的部分）
B  lwIP PCB 链表按 lane 分桶       需要 A
C  per-lane pbuf 池与定时轮        需要 B
D  收包投递给目标 lane             需要 C
E  per-socket 锁替 g_net_lock      需要 D，且先要有对象引用计数
F  多队列 + 每队列中断             需要 D
G  RSS / 流引导                    需要 F
```

顺序不可跳。**B 只做分桶而没有 C，会把一个全局定时轮留在 per-lane PCB 后面**，
也就是把正要消除的单核串行又请回来。

## lane 归属

一条 TCP 连接的 PCB 从建立到关闭属于同一条 lane，**永不迁移**。归属由
`(local_ip, local_port)` 哈希决定（`net_lane_hash()`）。

两侧必须算出同一个值，这是整个方案的地基：

- 建连时：本地 `(ip, port)`
- 收到包时：`(对端源端口, 对端目的地址)` —— 对已建立连接，对端源端口就是我们的
  本地端口，对端目的地址就是我们的本地地址

所以哈希里 **port 在前、address 在后**。顺序写反、或只哈希地址，都会让入向包
落到不拥有该 PCB 的桶里。

绝不能用 socket 指针或分配计数器做种子：这个值必须**仅凭线上可见字段**在从未见过
该连接的 CPU 上算出同样的结果。

IPv6 取地址低 32 位。熵足够分散连接，而关键在于它只由线上字节算出，两端一致。

非 IP 家族（AF_UNIX / AF_PACKET / AF_NETLINK / AF_ALG）没有端口可哈希，也没有
入向包需要查找的 PCB，因此保持创建时的临时归属不变。

### listener 是不对称的 —— 上面那条不变量对它不成立

上面说的"两端同值"只对**已建立连接**成立。listener 不是这样：

| | 键 |
|---|---|
| listener 归桶 | `(local_ip, local_port)`，而 wildcard bind 的 `local_ip` 是全零 |
| 入向包查桶 | `(dst_ip, dst_port)`，`dst_ip` 是具体的 |

这两个键不在同一个语义空间里 —— `bind(0.0.0.0)` 的含义是"接受**所有**本地地址"，
不是"挑一个"。**任何哈希函数都无法让它们相等**，所以通配 listener 若按
`hash(local_ip, local_port)` 归桶，入向 SYN 查 `hash(dst_ip, dst_port)` 必然落空，
连接被拒。而绑 `0.0.0.0` 是最常见的部署方式。

已建立连接不受影响，这是不对称性的唯一原因：

- 被动开放：`tcp_listen_input()`（`tcp_in.c:678`）把**具体**的
  `ip_current_dest_addr()` 拷进新 PCB
- 主动开放：`tcp_connect()`（`tcp.c:1102`）在发 SYN 前先经 `ip_route()` 定出具体
  `local_ip`

所以缺陷**只在 LISTEN 路径**（`tcp_in.c:320-353` 遍历 `tcp_listen_pcbs`），
`tcp_active_pcbs` 与 `tcp_tw_pcbs` 的 4 元组精确匹配都是安全的。

修法是**给通配 pcb 一个专用桶**，而不是让它同时进两个桶：

```c
#if CONFIG_NET_LANES > 1
#define TCP_PCB_LANE_ANY  CONFIG_NET_LANES      /* 哨兵，不与真实 lane 冲突 */
#endif
```

- 注册：`local_ip` 为全零 → 归 `TCP_PCB_LANE_ANY`；否则归 `hash(local_ip, local_port)`
- 查找：先探 `hash(dst_ip, dst_port)`，再探 `TCP_PCB_LANE_ANY`
- 该桶**只在 `CONFIG_NET_LANES > 1` 时存在**，所以 N=1 一次额外探测都不引入

**不要让通配 pcb 同时注册进两个桶。** PCB 链表是单链表，第二次插入会覆写
`npcb->next`，两个桶同时被破坏 —— 这是静默的链表损坏，不会报错。

常见路径（具体地址的 listener、已建立连接）仍然只探一个桶；只有通配 listener 多探
一次。代价可接受。

### 网卡地址变化：TCP 改写 listen 链表，UDP 改写全部

先前这里写的是"`tcp_netif_ip_addr_changed()` 会改写 bound 与 active PCB 的
`local_ip`"。**那是错的**，已按源码更正：`tcp_netif_ip_addr_changed()`（`tcp.c`）对
地址匹配的 bound/active PCB 调用的是 **`tcp_abort(pcb)`** —— 直接终止，**不改写**
`local_ip`。所以这两条链表上的 PCB 不存在"归桶后 lane 漂移"的问题。

真正改写 `local_ip` 的是 **listen 链表**（以及 `udp_netif_ip_addr_changed()`，它改写
UDP PCB 的 `local_ip`）。这些需要**重归桶**：在全局锁下先摘链（此时 `pcb->lane` 是
唯一记录着它实际在哪的凭据），再按新地址重算lane 并插入；桶未变时保持原有链表位置。
`tcp_listen_pcb_rebucket()` 就是为此存在的。

这个区别很重要：原描述会让人以为 TCP 主路径也受影响，从而过度设计。

### 在本项目里，TCP 的通配路径是结构性不可达的

lwIP core 之外**没有任何地方调用 `tcp_listen()`**（只有 `altcp_tcp.c` 调
`tcp_listen_with_backlog_and_err()`），而 A20OS 的 `net_listen()` 刻意不把 lwIP pcb
传下去。因此 `tcp_listen_pcbs` 在本项目**恒为空**，TCP 侧的哨兵桶是死代码。

已实测确认：把哨兵探测关掉，一个 TCP 测试仍然通过 —— 即该测试结构上无法覆盖这条路径。

所以**通配路径的真实验证在 UDP**：`udp_input()` 会真的被走到。已用 A/B 证明：

- 哨兵探测**开** → `WILDCARD_UDP_TEST: PASS`（发往网卡地址 10.0.2.15 的数据报到达了
  绑在 `0.0.0.0:12459` 的 PCB，载荷相符）
- 哨兵探测**关** → `WILDCARD_UDP_TEST: FAIL`

这是目前唯一能证明哨兵桶有效的证据。

### `smoke-net-lanes-n1` 比看起来弱

本地 `connect()` 在 socket 层内部配对，**根本不进入 lwIP**。因此即使 TCP 分桶完全
失效，`net_stress_test` 的输出仍会逐字节相同。N=1/N=4 等价性仍然有价值（它证明了没有
*额外*的行为差异），但**不能**作为"TCP 分桶正确"的证据。真正的证据是上面那个 UDP A/B。

### 广播 UDP 是唯一无法按桶定向的用例

子网内 PCB 的桶不由目的地址决定，所以 `broadcast != 0` 时 `udp_input` 扫描**全部**桶。
N>1 时这会改变多个同等匹配的 PCB 中谁赢得一次全局广播。TCP 不受影响 ——
`tcp_input` 在任何查找之前就丢弃广播/组播。

### 关于 TIME-WAIT：没有回收这回事

曾以为 lwIP 会把 TIME-WAIT PCB 回收进 `tcp_active_pcbs`（同一 4 元组）。**lwIP 2.2
没有这条路径**：正常超时走 `tcp_slowtmr()` → `tcp_pcb_purge()` → `tcp_free()`；
内存不足时 `tcp_kill_timewait()` 直接 `tcp_abort()`；收到 TIME-WAIT 段的包走
`tcp_timewait_input()` 补 ACK 后**留在原处**。

所以"已归桶的 pcb 永不跨桶"在 stock lwIP 2.2 里**本来就成立** —— PCB 只在
bound → listen → active → tw 之间换*链表*，从不在同一链表内换*桶*。

### 该断言什么

`tcp_pcbs_sane()` 已经在遍历 `tcp_pcb_lists`，教它认 lane 即可：

1. `pcb->lane < CONFIG_NET_LANES`（哨兵桶除外）
2. 非通配 PCB：`pcb->lane == TCP_PCB_LANE_OF_PCB(pcb)`
3. `TCP_PCB_LANE_ANY` 桶里**只有**通配 PCB
4. 通配 PCB**不出现在**任何真实 lane 桶里
5. 同一链表内没有 PCB 出现在两个桶里

第 3、4 条正是把"同时注册进两个桶"那种错误**变成可检出**的地方。

## 阶段 B：lwIP PCB 分桶

**状态：已实现并验证。** `NET_LANES=1` 与 `4` 均在 `-Werror` 下干净编译、链接、启动；
`net_stress_test` 两档输出逐字节相同（md5 `41ce8d13`）；`smoke-net-lanes`、
`smoke-net-lanes-n1`、`smoke-netfilter` 全 PASS；两种启动日志均无 panic、page fault、UBSan。
另外在 5 个文件 × N∈{1,4} × {plain, `TCP_DEBUG_PCB_LISTS=1`, `TCP_DEBUG=1`, `SO_REUSE=1`}
共 32 种配置下全部干净编译。

通配路径用 **UDP A/B** 证明（TCP 侧结构上不可达，见下）：哨兵探测开→
`WILDCARD_UDP_TEST: PASS`（发往网卡地址的数据报到达绑在 `0.0.0.0:12459` 的 PCB，载荷相符），
关→ `FAIL`。这是目前唯一能证明哨兵桶有效的证据，因为关掉它测试就会失败。

实现中被查出并修掉的两个静默缺陷：

- `NET_PCB_LANE_ANY_PROBE` 曾在两个分支都是 0，使 `NET_PCB_LANE_SEARCH_BUCKETS == 1`，
  查找只走哈希桶、**从不探测哨兵桶** —— 通配 PCB 被写入却从不被读取。
- 各链表数组声明为 `[CONFIG_NET_LANES]`，而哨兵下标恰等于 `CONFIG_NET_LANES`，
  用它索引**越界一个元素**。

两者互相掩盖：探测关闭所以哨兵从不被访问，恰好避开了越界。**只修其一会把静默失效变成
内存破坏**，必须同时修。数组尺寸统一用 `NET_PCB_LANE_BUCKETS`（单 lane 时等于
`CONFIG_NET_LANES`，多 lane 时为 `CONFIG_NET_LANES + 1`），所有"遍历全部 PCB"的循环上界
也一并改用它，否则通配 PCB 会在 close/abort/kill_timewait 路径上泄漏。

热路径要无锁，就得让已建立 TCP 的收发不再遍历全局 PCB 链表。lwIP 2.2 的链表操作
**已经宏抽象**（`tcp_priv.h:355` `TCP_REG`、`:370` `TCP_RMV`、`:417`/`:423`
ACTIVE 变体），且表头是**参数**，所以分桶比"重写 TCP 查找"小得多：

| 现在 | 之后 |
|---|---|
| `tcp_bound_pcbs`、`tcp_active_pcbs`（`tcp.c:171,176`） | `[NET_LANES]` |
| `tcp_pcb_lists[]`（`tcp.c:181`，含 listen/bound/active/tw 四条） | 每条各一个数组 |
| `udp_pcbs`（`udp.c:81`） | `[NET_LANES]` |
| — | `struct tcp_pcb` / `udp_pcb` 加 `u8_t lane` |
| `tcp_lookup()` 四条链表全遍历 | 只进 `net_lane_of()` 那一个桶 |

`NET_LANES == 1` 时全部退化为下标 0，行为不变 —— 这是这个补丁能安全落地的依据。

lwIP 2.2 已有 `tcp_active_pcbs_changed`（`tcp.c:185`）这个代际标志，说明"用代际计数
替代遍历"在上游被认可。顺带一提：`kernel/net/netfilter.c` 的文件头曾**声称**有同款
机制而代码里没有，已在另一提交中补上。

## 缓冲改造的前提：callback 里不能分配

`net_bh_ring` 目前把 `net_bh_event_t` **按值内嵌**，所以每 socket 的 staging 成本是
`O(NET_BH_RING_SIZE × INLINE_PAYLOAD)`，DEFAULT/SERVER 档约 28 KiB/socket。把 `events[]`
换成指针、按需从 slab 分配，是降低这个成本的自然做法——**但它与本仓库自己的锁契约
直接冲突**。

`bh_ring_prepare()` 由 lwIP callback 调用，运行在 `g_lwip_lock` 下；而
[network-lock-contract.md](../net/network-lock-contract.md) 明确规定"持有 `g_lwip_lock`
时不得调用 `kmalloc()`、`kfree()`、`net_msg_alloc()` 或任何 slab allocator 函数"。
按需分配事件正是要在 callback 里调 slab。

三条出路，各有代价，**都需要先做决定**：

1. **放开契约**：允许 callback 走一条无锁的 per-CPU magazine 快路径。`obj_cache_t`
   不能用——它 miss 路径会 `spin_unlock` 后调普通 `kmalloc()`（`objcache.c:40-41`），
   不是硬 IRQ 安全。需要新写一个，并为其单独设门禁。
2. **预分配池**：init 时按 lane/CPU 预填 `net_bh_event_t`（范式见 `mm/pt.c` 的
   `pt_mcs_pool_t`，注释明确"锁路径禁用抢占所以绝不能调分配器"）。代价是这批内存变成
   与 socket 数量无关的静态占用——只是把同一个问题从 socket 挪到池。
3. **在 bottom-half 分配**：producer 不分配，改为投递一个"有数据"的标记，由
   bottom-half（`g_net_lock` 下，契约允许分配）建事件。但 `bh_ring_prepare` 此刻就要
   一个 `net_bh_event_t*` 交给 lwIP 填 payload，所以这条路要求把 payload 暂存改到
   别处（pbuf spill 已经是这个形状），等于重做接收路径。

在三者之一落地之前，per-socket staging 成本只能靠 profile 压小，不能靠结构消除。
`sizeof(net_socket_t) <= NET_PROFILE_SOCKET_MAX_BYTES` 的 `_Static_assert` 已经覆盖
ring size 与 inline payload，所以再加聚合断言是同义反复。

### 阶段 C 的实际代价：三个计时器全局量不是同一个性质

把计时器按 lane 切开的直觉做法是"每个 lane 一份 `tcp_timer` / `tcp_timer_ctr` /
`tcp_ticks`"。审计之后这个做法不成立，因为这三个量的耦合程度不同，而且**其中一个根本
不是计时器私有的**：

- `tcp_timer`（快慢交替的奇偶计数）与 `tcp_timer_ctr`（本轮去重）确实只被 timer 读，
  只在 `tcp_fasttmr` / `tcp_slowtmr` 里出现（`tcp.c:241,1250,1256,1547,1549`），
  按 lane 切开是自洽的。
- `tcp_ticks` **不是**。它是整个协议栈的共享时基，而且在**收包与发包路径**上被写入：
  `pcb->tmr = tcp_ticks`（`tcp_in.c:805,885`）、`pcb->rttest = tcp_ticks`
  （`tcp_out.c:1543`）。读它的地方包括 keepalive、zero-window probe、persist、
  ooseq 超时、TIME_WAIT 的 MSL（`2 * TCP_MSL / TCP_SLOW_INTERVAL`）以及 RTT 估算。

所以按 lane 切 `tcp_ticks` 不是"把计时器分片"，而是**换掉时基**。它只有在
"盖时间戳"和"读时间戳"永远取同一个 lane 的计数器时才正确：盖戳发生在收包路径，而
收包路径正是用同一个哈希找到 pcb 的，lane 天然一致——所以这条路走得通。但代价是要动
`tcp.c`、`tcp_in.c`、`tcp_out.c`、`tcp_priv.h` 四个文件，改到 RTO、keepalive、
zero-window probe、ooseq、TIME_WAIT、RTT 每一处经过时间差计算的地方。任何一处
漏改的后果不是编译错误，而是**算错经过时间**：多余的 RTO、过早的 TIME_WAIT 回收、
失联的 keepalive、被污染的 RTT 估计。

结论：`tcp_timer` 与 `tcp_timer_ctr` 可以按 lane 切（它们自洽），`tcp_ticks` 应当
**保持全局**——它本来就是墙钟量，与 lane 无关，切开只会引入算错的窗口。这也是为什么
当前实现选择"共享计数器 + 切分遍历"：遍历是每包的主要开销，而计数器不是。

已落地部分（`2b3a8b22`）：`tcp_fasttmr` / `tcp_slowtmr` 已拆成单 bucket 遍历函数，
外层保留原来的 lane 循环与"回到 lane 0 重来"的语义，因此行为完全未变，`smoke-net-lanes-n1`
（1 lane 与 4 lane 跑 `net_stress_test` 要求逐字节相同的结论）通过。per-lane 入口
刻意**还没有**导出——先把无调用者的 API 摆在那里是投机式泛用，等第一个真实调用者
一起落地。

尚未落地：per-lane 入口与它的调用者、TIME_WAIT 的分片、per-lane pbuf 池。

## 阻塞：多 lane + 真实 LISTEN pcb 在引导期就会 panic

在写阶段 C 的门禁时撞到的，**先于**任何阶段 C 的改动存在，必须先解决，否则 C 与 D
都是在会 panic 的地基上做。

复现（4 lane、4 CPU、`a20.tcpmode=lwip`）：

```
make ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=4 NET_LANES=4 dev-build
# 以 a20.tcpmode=lwip 启动，无需运行任何测试程序
```

结果是在 **idle 任务、CPU 0、引导期间**就 KERNEL PAGE FAULT → PANIC，
`pcbs:` 显示 `tcp_listen=1`（telnetd 的通配 bind 已经建出真实 lwIP LISTEN pcb）。
经 TCP 路径触发时栈为 `ethernet_output → tcp_pcb_remove`，`BADV=0x100000000000038`，
即链表指针已被破坏，不是空指针。

已确认的三件事：

1. **不是阶段 C 引入的。** 把 `tcp.c` 整个回退到本轮计时器工作之前（`048f30e6`），
   panic 依旧复现（7 处 fault 行）。所以它与 per-lane 计数器、per-CPU 分派都无关。
2. **只在有真实 lwIP LISTEN pcb 时出现。** `smoke-net-lanes-n1` 同样是 4 lane +
   4 CPU，但走默认 `fast` 档、不存在真实 lwIP LISTEN pcb，一直是绿的。
3. 因此触发条件是**多 lane 分桶与真实监听 pcb 的交互**，而不是多 lane 本身。

**复现不稳定。** 收窄结果：

| lane | CPU | 真实 LISTEN pcb | 结果 |
|------|-----|----------------|------|
| 4 | 4 | 有 | **panic**（门禁里） |
| 4 | 4 | 有 | 正常（单独重跑，同样是 `cat /proc/net/status` + 4 CPU） |
| 4 | 1 | 有 | 正常 |

所以触发条件是**多 lane + 多 CPU + 真实 LISTEN pcb**，而症状是**间歇性**的：
同一组参数重跑不复现。已知故障上下文有两种——`tcp_accept_test` 任务，以及
`idle`（pid=0）——后者说明它可以在没有任何测试程序参与的情况下发生。

这把结论从"某处的确定性越界"推向"并发竞态"：如果哨兵桶真的每次都越界，4 CPU 重跑
不可能是干净的。因此下面这条只是**待查线索**，不再是头号嫌疑；且一个纯粹的越界读也
解释不了"链表头被破坏"——那更像并发改链表。

定位方向随之改变：确定性越界可以用断言和静态检查抓，竞态不行；需要的是并发压力
（例如反复建连/关闭跨 lane 连接）、锁审计，或能放大竞态的检测手段。

已排除的一个候选：socket 与 PCB 的 lane 概念在**通配 bind** 上不一致。telnetd 绑
`INADDR_ANY`，此时

- `net_socket_lane_of_addr()`（`kernel/net/socket_inet.c:213`）返回
  `net_lane_of(0, 2323)`，落在某个**真实 lane** 0..N-1；
- 而 `NET_PCB_LANE_OF_PCB()`（`lwip/priv/pcb_lane.h`）对 `local_ip` 为 any 的 pcb
  返回**哨兵桶** `NET_PCB_LANE_ANY`（= `CONFIG_NET_LANES`，即 N）。

也就是"socket 说 lane 2、PCB 在桶 4"。但它**不是**当前 panic 的原因：
`net_socket_t::lane` 目前只被写、从不被读——`grep` 全树只有 `socket.c:30` 与
`socket.c:386` 两处赋值，没有任何 dispatch、锁选择或桶索引用它。阶段 A 只留了字段，
阶段 D 才会消费它。

留在这里是因为它会在阶段 D 变成真 bug：一旦按 socket 的 lane 选处理 lane，
通配 bind 的 listener 就会被派到**不拥有它 pcb 的那个 lane**，而哨兵桶里的 pcb 又不
被任何 lane 的计时器遍历。修法是让两者对 any 的处理一致——要么 socket 侧也返回哨兵值，
要么哨兵桶只用于查找、而 owning lane 仍然有明确定义。这属于阶段 D 的前置条件。

再排除一个：哨兵桶本身没有被越界。`tcp_bound_pcbs` / `tcp_listen_pcbs` /
`tcp_active_pcbs` / `tcp_tw_pcbs` / `tcp_timer[]` / `tcp_timer_ctr[]` 全部声明为
`[NET_PCB_LANE_BUCKETS]`（= N+1），而不是 `[CONFIG_NET_LANES]`；`tcp_pcb_lists[]`
按状态索引后再按 lane 索引，元素类型是 `struct tcp_pcb **`，与这些数组匹配。
`tcp_listen_pcbs` 的三处使用（`tcp.c:890,2475,2648`）也都以
`NET_PCB_LANE_BUCKETS` 为界。所以"`CONFIG_NET_LANES` 大小数组被 `NET_PCB_LANE_ANY`
索引"这个猜测是错的。

**为什么没有任何断言拦住它：`LWIP_ASSERT_CORE_LOCKED()` 是空的。**
`kernel/external/lwip/src/include/lwip/opt.h:227` 把它定义成空宏，于是 `tcp.c` 与
`tcp_in.c` 里那几十处 `LWIP_ASSERT_CORE_LOCKED()`（`tcp_close` / `tcp_abort` /
`tcp_bind` / `tcp_new` / `tcp_input` …）**全部是空操作**。本仓库的锁契约
（[network-lock-contract.md](network-lock-contract.md)）在运行时**没有任何强制手段**，
只是一份文档。

这不是 bug 本身，但是它为什么能一直不被发现的原因：任何一条在**没有持
`g_lwip_lock`** 的情况下走到 `tcp_pcb_remove()` / `tcp_listen_closed()` 的路径，
都会安静地破坏 lane 链表——`LWIP_ASSERT` 只在断言条件里查链表一致性，而链表一致性
本身正是被破坏的东西，`tcp_pcbs_sane()` 只能在你还能安全遍历时才有用。

因此下一步的方向比"找错索引"更具体了：**审计所有进入 `tcp_pcb_remove()` /
`tcp_listen_closed()` 的路径，确认每一条都持 `g_lwip_lock`**，尤其是 RST 生成
（`ethernet_output` 侧）与 netif 地址变更（会调 `tcp_listen_pcb_rebucket`，跨 lane
搬动 LISTEN pcb）。这两条是当前唯一还没被排除的、且与"需要多 CPU + 需要真实
LISTEN pcb"两个条件都对得上的路径。

**已完成的锁审计（结论是"没找到漏锁"，这对下一步是负面但有用的结果）。**
沿 `tcp_pcb_remove()` / `tcp_listen_closed()` 的调用方逐条查了 `g_lwip_lock`：

- `kernel/net/socket_inet.c` 的 accept 落底路径（`net_inet_accept_stage_drain`）在每一次
  `tcp_abort()`、`net_inet_tcp_apply_options()`、子 socket 拆除周围**都**取了
  `a20_lwip_lock()`；整个树里没有一处裸调 `tcp_abort`。
- RST 生成侧：`tcp_abort()` 的调用点（`tcp_in.c:479,644`）本身带
  `LWIP_ASSERT_CORE_LOCKED()`，虽然断言是空的，但取锁由上层
  `a20_lwip_process_netif_rx_tx_locked()` 保证。
- netif 地址变更侧：`a20_lwip_if_set_addr()` 在 `netif_set_ipaddr()` **之前**就取了锁
  （`kernel/net/lwip_stack.c:969`），所以会触发 `tcp_listen_pcb_rebucket()` 的跨 lane
  搬动是在锁内的。
- `lwip_tcp_accept_cb()` 生产者用 release fence + acquire load 配对，是正确的 SPSC
  无锁 ring；`net_inet_bottom_half_process_all()` 只持 `g_net_lock` 但它不碰 lwIP core。

**运行时证据（已验证）：锁契约在运行时是成立的。**
临时把 `LWIP_ASSERT_CORE_LOCKED()` 接到 `g_lwip_lock` 的真实持有状态上（记录持有者
CPU，而不是布尔量——布尔量在"别的 CPU 持有"时会误判为通过），违规只计数不 panic，
并记录前 8 个不同的违规返回地址。在 `NET_LANES=4` + `a20.tcpmode=lwip` 下：

```
lwip_lock: violations=23 sites=8
lwip_lock_site0: ffffffc08041e81a -> netif_init
lwip_lock_site1: ffffffc08041df64 -> netif_add
lwip_lock_site2: ffffffc08041e842 -> netif_init
lwip_lock_site3: ffffffc080412a62 -> lwip_init
lwip_lock_site4: ffffffc08036a526 -> a20_lwip_init
lwip_lock_site5: ffffffc08036a560 -> a20_lwip_init
lwip_lock_site6: ffffffc0803681d0 -> a20_lwip_loopif_init_cb
lwip_lock_site7: ffffffc08041feb4 -> netif_add_ip6_address
```

**8 个全部是一次性初始化路径**（lwIP 引导 + A20OS 的 netif 注册回调），发生在锁语义
成立之前。也就是说 23 次违规全部是"引导期本来就不持锁"，**没有一次出现在收包、
accept、计时器或 PCB 增删路径上**。

两个推论，以及**这条证据的边界**：

1. panic 在这里是错的选法：这些初始化调用会让**每一个**配置在启动时就死掉。
2. 运行时**所有带断言的入口**都持锁。`tcp_close`、`tcp_bind`、`tcp_new`、
   `tcp_input` 等断言点无一违规。

**但这不足以证伪"某处忘了取 `g_lwip_lock`"**——我先前写得过强，已更正。原因是探针
只能看见**存在断言**的函数：`tcp.c` 里 45 个函数没有 `LWIP_ASSERT_CORE_LOCKED()`，
其中就包括 **`tcp_abort()`（`tcp.c:650`）**——而 RST/abort 正是
`tcp_pcb_remove` 崩溃的路径。也就是说，探针对最可疑的那条路径是**盲的**，它的 0 违规
对它不构成任何证据。

这个盲点是可以补的，而且成本很低：给这些入口补上断言（或者统一在一个 wrapper 里
断言），再跑一次同样的探针，盲区就变成覆盖区。目前 `tcp_abort` 的调用方逐个查过
（`socket_inet.c:575`、`tcp_in.c:527,1000`、`altcp_tcp.c:310`）都在 `g_lwip_lock`
下，所以它**可能**仍是安全的——但那是读代码得出的，不是探针测出来的，两者不能混为一谈。

另：`tcp_kill_state` / `tcp_kill_prio` / `tcp_kill_timewait` 会遍历所有 lane，是与
崩溃形态最像的函数，但 `tcp_kill` 在本树没有任何调用方，这三个是死代码，可以排除。

因此下一步仍然是两件事：把断言补到无断言的入口上重跑探针（把盲区变成覆盖），以及查
"哪些字段在一把锁下写、在另一把锁下读"——accept staging 是唯一同时需要真实 LISTEN
pcb 且跨越两个锁域的路径。

那段探针代码没有保留：它当时让 `smoke-net-accept` 变红，我据此以为是自己写错了而回滚；
实际原因是 `/tmp` 这个 tmpfs 被 QEMU trace 填满、编译器写不出临时汇编文件，与代码无关
（清掉 trace 后同一提交 `smoke-net-accept` 恢复 PASS）。探针本身已验证可用
（`-Werror` 下构建通过并给出上述数据），若要继续这条线，按上面两点重新接一次即可。

所以"某处忘了取 `g_lwip_lock`"这个假设，在上述几条最相关的路径上**都不成立**。
剩下的可能性要么在我没有审到的路径上，要么就不是漏锁，而是**锁内逻辑本身**（例如
`tcp_listen_closed()` 遍历全部 lane 改 `->listener` 时，与另一 CPU 上正在进行的
accept 落底之间的时序），这类问题静态审计看不出来。

这改变了下一步的性质：继续人工审计的收益已经很低，应该先把断言打开、用运行时证据
把范围压下来。

一个便宜且值得先做的前置动作：把 `LWIP_ASSERT_CORE_LOCKED()` 在本配置下接到
`g_lwip_lock` 的实际持有状态上（若 `g_lwip_lock` 有 owner 字段或可测试），让契约
先变成可执行的。之后所有 lane 相关改动才有回归护栏。

尚未定位。曾经的嫌疑是通配 bind 用的哨兵桶：`NET_PCB_LANE_ANY` 定义为
`CONFIG_NET_LANES`（即"最后一个真实 lane 之后"），`NET_PCB_LANE_BUCKETS` 才把它
算进去。若某处用 `CONFIG_NET_LANES` 大小的数组去索引一个 lane 为
`NET_PCB_LANE_ANY` 的 pcb，就是越界一个元素的越界——与观测到的"链表指针被破坏"
一致。但这只是推测，**没有验证**，不能当作结论。

### 真正的落点：loopback 收包路径上的 pbuf 生命周期

放大器实验（`CONFIG_NET_RACE_DELAY_US=200` + 真实 TCP 流量）复现了 fault，并且**第一次
拿到了一条完整可信的回溯**——之前那条 `ethernet_output+0x664` 是按偏移猜的符号，不可信；
这条每帧都能对上：

```
[0] tcp_input   [1] ip4_input   [2] ip_input   [3] netif_poll
[4] a20_lwip_poll_timers_locked          [5] kernel_progress_timer_tick
```

也就是说，崩溃发生在 **timer tick 排空 loopback → ip_input → tcp_input** 这条链上，
上下文是 idle（pid=0）。寄存器里 `stval=0x3071c710010000af`、`a2=0x14`，而
`0x3071` 同时出现在 `a2` 和坏地址里——形状像**从报文数据里算出来的 pbuf 链指针**，
即 pbuf 的 payload/链指针已被破坏，而不是普通空指针解引用。

这把搜索方向彻底改掉了，也解释了放大器为什么没用：**我把它加错了地方**。
accept staging（`net_inet_accept_stage_drain`）不是窗口。真正的窗口是 pbuf 的所有权，
它跨两个锁域：

- 排空侧：`netif_poll()` 由 timer tick 在 CPU 0 上、持 `g_lwip_lock` 释放
  `netif->loop_first` 的 pbuf；
- 消费侧：socket bottom half 在 `g_net_lock` 下搬运 payload，**不持** `g_lwip_lock`。

两者之间没有任何东西保证 pbuf 在被消费完之前不被排空侧回收。也就是说，多 lane +多 CPU
只是把"另一个 CPU 同时在动"的概率提高了，破坏的其实是一个与 lane 无关的既有 pbuf 所有权
缺口。

已排除的两个候选（读代码确认，不是推测）：

- **inline staging 溢出**：`bh_stage_payload()` 的 inline 分支被
  `len <= NET_BH_INLINE_PAYLOAD` 挡住，而 `e->data` 是定长
  `uint8_t data[NET_BH_INLINE_PAYLOAD]`；溢出分支先 `pbuf_ref()` 存进
  `owned[slot]`，调用方随后才 `pbuf_free(p)`。引用与边界都是对的。
- **排空侧自身**：`netif_poll()` 在 `g_lwip_lock` 下**同步**消费
  `loop_first`，把 pbuf 直接交给 `ip_input`，排空与协议栈处理之间不存在窗口。

所以破坏发生在 pbuf **进入 `loop_first` 之前**：发送路径
（`tcp_output` → `ip_output` → `netif_loop_output`）构造出来的链本身就已经是坏的。
下一步应看 `netif_loop_output()` 的 pbuf 构造与 `pbuf_take` 之后的链形状，
以及 PBUF_POOL 元素在被 `tcp_segs_free` 复用后是否仍可能残留在某条链上。

下一步应当是审 `netif_poll()`（loop_first 排空）到 bottom-half payload 消费之间的 pbuf
生命周期：谁持有引用、谁负责 `pbuf_free`、以及两条路径之间是否缺少一次引用计数或所有权
移交。放大器应该加在**排空侧的 pbuf 释放点与消费侧的读取之间**，不是加在 accept staging。

### 下一步（保留：不要只读代码，要放大窗口）

触发面已经收窄过了（4 lane + 多 CPU + 真实 LISTEN pcb 才炸，间歇性），静态审计
也已连续七次落空。**继续读代码不会找到它**——要找的是一个跨 CPU 的时序窗口，
读代码看不见时序。要做的是把窗口放大到必然命中，然后当场抓住。

具体配方：

1. 在 `net_inet_accept_stage_drain()` 取出 pcb 之后、第一次取 `a20_lwip_lock()`
   之前，插一个可配置延时（`CONFIG_NET_RACE_DELAY_US`，默认 0）。这段是唯一同时
   需要真实 LISTEN pcb 且跨越两个锁域的路径，也是当前最可疑的位置。
2. 用 `NET_LANES=4 NR_CPUS=4 a20.tcpmode=lwip` 启动，延时从 0 往上加
   （1、10、50、200 µs）。预期现象：panic 从间歇变**必然**，且
   `tcp_pcb_remove` 之前的崩溃点会前移到窗口的另一端。
3. 一旦必然复现，`-DCONFIG_NET_LOCK_ASSERT=1` 的探针配合符号化就能指出
   到底是哪一侧在窗口里改坏了链表头——那时候才是读代码的时候，因为已经有了一个
   可复现的样本。
4. 复现之后**先删掉延时**，再修真正的竞态；延时只是放大器，不是修复。

如果加了延时仍然不炸，说明嫌疑区选错了，此时再换 `lwip_tcp_accept_cb()` 生产者
那一侧——但先做完上面四步，不要跳。

在此之前阶段 C 的 per-lane 计时器分片可以保留（它本身已验证正确且受
`sys_check_timeouts()` 的间隔门控），但**不要**再去动分派。

## 阶段 D：收包引导

单 NIC ring 的**排空**天然串行（一把锁、一个 ring），但**协议栈处理**并不，而后者才是
每包的主要开销。所以服务器的多核收益主要来自阶段 D 的处理侧并行，**不需要多队列**：

- 排空：短锁 + 有限包数预算（`CONFIG_NET_RX_IRQ_BUDGET`，已实现）
- 处理：把包投给 socket 所属的 lane，各 CPU 各自处理自己的 socket

**不要简单地让 IRQ 只入队。** 那会死锁：阻塞读的任务由 bottom-half 唤醒，
bottom-half 需要 ring 已排空，而只在读者唤醒后才跑的 poll 在等一个不会到来的事件。
需要显式 softirq 加一个保证被执行的 poll 点（`sched()` 内的
`kernel_progress_run_bottom_halves()` 是候选）。

阶段 F（多队列）再去掉排空瓶颈，属二阶优化。

## 必须保持全局的部分

| 对象 | 处理 | 理由 |
|---|---|---|
| ARP / etharp 缓存 | seqlock | 每包都要读，必须无锁；几乎不写 |
| 路由 / netif 列表 | seqlock | 同上 |
| socket registry | 分片哈希 + 引用计数 | 冷路径 |
| bind/listen/accept/close 等 PCB 冷路径 | 单把 `g_netctl_lock` | 频率是连接率级，不是包率级 |

现有契约里 `g_lwip_lock` 与 `g_net_lock` **从不同时持有**，所以新层级是一张白纸，
不存在需要拆解的既有嵌套。

## 观测

`/proc/net/status` 的 `lanes:` 行给出 lane 数与每 lane 的 socket 数。它不采信
吞吐数字，只回答"lane 是否真的把连接分散开了"：

```
lanes: count=4 sockets=8 occupancy: 3 2 2 1
```

`CONFIG_NET_BUSY_POLL` 与 lane 数是正交的：单 lane 时轮询就是今天的行为。

## 验证

**不要用吞吐数字做门禁。** QEMU TCG 下同一负载的 `lwip` 自旋数在 0–920024 之间跳
（见 `docs/server-readiness.md`），任何幅度阈值都是 flaky 的。lane 的门禁必须是
结构性的：

| 门禁 | 断言 |
|---|---|
| `smoke-net-lanes` | 多 lane 构建下 lanes 行可渲染，且 lane 数等于构建参数 |
| `smoke-net-lanes-n1` | 1-lane 与多-lane 构建的 `net_stress_test` 校验和逐字节一致 |
| `check-net-lane-ownership`（编译期） | 同一 4 元组两次计算 lane 相同 |
| 现有 `NET_STRESS_TEST` / `smoke-netfilter` | 语义不回归 |

吞吐数字仍然要等真机（VisionFive 2 / LS2K1000 的 GMAC 已在树里）或给测试链路注入
RTT —— 这条没有变。