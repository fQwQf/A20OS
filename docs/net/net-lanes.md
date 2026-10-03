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
上下文是 idle（pid=0）。

**不要被 trap 的措辞误导**：`Kernel failed to access user address`
（`kernel/core/trap.c:594`）是**无条件**打印的，任何未处理的页错都会走到这一行；
真正区分用户/内核地址的是紧接着的 `stval < USER_VA_LIMIT` 判断。该次运行走的是
**panic 分支而不是 `proc_exit_group(-SIGSEGV)`**，所以 `stval` 并非用户地址。
本文件早先把它读成"用户地址"是错的——那句来自 trap 文本，不是证据。

寄存器（更正：早先写的是 `a2`，实际是 `a1`）：

```
a0=0x4affe17e a1=0x3071            a2=0x14              a3=0x3071c710010000af
a4=0xffffffffffffffd0               a5=0x3071c7100100007f
stval=0x3071c710010000af  (== a3)
```

两点值得注意：

1. `0x3071` 同时出现在 `a1` 和坏地址里。
2. **a3 与 a5 的高位完全相同**（`0x3071c710010000`），只有低字节不同（`af` / `7f`）。

**更正：上一条把这个当成"最强线索"是过度解读，撤回。**
`a1 = 0x3071 = 12401`，而 12401 正是那次崩溃运行里 `tcp_accept_test 12401` 用的
端口。也就是说 a1 是**正确解析出来的目的端口**，寄存器里装的是合法的 TCP 报文字段，
不是野指针。a3/a5 与 a1 高位相同这件事因此没有推论价值——它们同属一组正常解析出来的
报文字段，共享高位不说明任何内存被破坏。

同理，"a3/a5 只差低字节 ⇒ 打包数据被当成指针读"这条推理也一并撤回：前提（这些值是
无意义的数据）不成立。唯一仍然确定的是 **a3 == stval**，即 a3 当时被当作地址解引用，
而该地址无效；但 a3 为什么会持有这个值，完全未知。

教训记在这里：**从寄存器 dump 推断内存错误之前，先把每个值和它本该代表的东西
对一遍**。一个恰好等于测试端口的值本该立刻否掉"数据被当指针读"这个假设，我却在
没有核对的情况下把它写成了结论。

因此现在的状态回到"只知道崩在 tcp_input，不知道为什么"。而且那次运行的日志已被
清理（我为了腾 `/tmp` 空间删掉了），所以**重新取证需要重跑一次**：4 lane + 4 CPU +
`a20.tcpmode=lwip` + `CONFIG_NET_LOCK_ASSERT=1`，并且保留 `/proc/net/status`、
`/proc/a20/perf` 输出和完整 trap dump。下一步的定位工作应当从那次 dump 开始，
而不是从我上面那套已经被否掉的推断开始。

这把搜索方向彻底改掉了，也解释了放大器为什么没用：**我把它加错了地方**。
accept staging（`net_inet_accept_stage_drain`）不是窗口。真正的窗口是 pbuf 的所有权，
它跨两个锁域：

- 排空侧：`netif_poll()` 由 timer tick 在 CPU 0 上、持 `g_lwip_lock` 释放
  `netif->loop_first` 的 pbuf；
- 消费侧：socket bottom half 在 `g_net_lock` 下搬运 payload，**不持** `g_lwip_lock`。

两者之间没有任何东西保证 pbuf 在被消费完之前不被排空侧回收。也就是说，多 lane +多 CPU
只是把"另一个 CPU 同时在动"的概率提高了，破坏的其实是一个与 lane 无关的既有 pbuf 所有权
缺口。

### netif_poll() 排空侧读到这里（未完成）

已确认的：

- `netif_loop_output()` 分配**全新的** `r = pbuf_alloc(PBUF_LINK, p->tot_len, PBUF_RAM)`
  并 `pbuf_copy(r, p)`，所以进 `loop_first` 的是副本，不是发送路径那条链。
- `netif_poll()` 在 `SYS_ARCH_PROTECT` 下从 `loop_first` 摘链并**就地**处理
  （`in_end->next = NULL` 后交给 `netif_process_pbuf` → `ip_input`），排空与协议栈
  之间没有窗口。
- `netif_poll()` 走链找末尾用的是 `while (in_end->len != in_end->tot_len) in_end = in_end->next;`，
  **没有显式 NULL 保护**，只靠 `LWIP_ASSERT("bogus pbuf: len != tot_len but next == NULL!")`。
- `LWIP_ASSERT` 在本配置下**是活的**（`LWIP_NOASSERT` 未定义，`kernel/net/lwip_port/lwipopts.h`）。

**这最后两条合起来是一条真实结论**：既然 assert 没触发（实际发生的是页错不是
"LWIP_ASSERT" 输出），说明被摘出的那条链 `len == tot_len` 的末端**确实找到了**，
即链在结构上是良性的。所以"链被写坏导致无限遍历/走飞"这条路可以排除。

### 崩溃指令（已定位到指令级）

重跑取到完整 dump（4 lane / 4 CPU / `tcpmode=lwip` / `CONFIG_NET_LOCK_ASSERT=1`），
并反汇编了 faulting PC `tcp_input+0x2eb2 = 0xffffffc08055963c`：

```
ffffffc080559638:  ld    a5,-88(s0)
ffffffc08055963c:  lbu   a5,48(a5)      <-- 崩溃处
ffffffc080559640:  beqz  a5,...
```

要点：

1. 崩溃是一条**字节读** `lbu`，偏移 **48 (0x30)**，基址取自**栈上 -88(s0)**，
   不是寄存器传参。所以被破坏的是 `tcp_input` 的一个**栈上局部指针**。
2. 数值自洽：`a5 = 0x3071f29c0100007f`，`+0x30 = 0x3071f29c010000af`，
   正是 `stval`。上一轮那次是 `0x3071c7100100007f` → `...af`，同样自洽。
3. **两次独立运行，低 16 位恒为 `0x007f`**，只有高 48 位变化
   （`0x3071c710` / `0x3071f29c`）。这不是随机野指针——随机值不会跨运行保持低位。
   低位稳定、高位每次不同，更像"某个按字节拼出来的值"被当成了指针。
4. 该指令紧邻 `__ubsan_handle_pointer_overflow`（源码里就是 `ptr + 48` 的溢出检查），
   说明这里本来就有一个"指针 + 48"的运算，UBSan 在它之前。

### 崩溃语句已定位：LISTEN pcb 链损坏（哨兵桶）

内核没带行表，`addr2line` 给不出语句，但 **UBSan 的消息描述符里带着文件名和行号**。
faulting PC 前面那条 `__ubsan_handle_pointer_overflow` 用的描述符在
`0xffffffc0807b7258`，解出来是：

- filename 指针 `0xffffffc0805f3678` → **`lwip/src/core/tcp_in.c`**
- line `0x154` = **340**，column `0x12` = **18**

`.rodata` 里紧挨着的下一个字符串正是 `tcp_input: invalid pbuf`。而 340 行落在：

```c
for (lpcb = tcp_listen_pcbs[search_lane].listen_pcbs; lpcb != NULL; lpcb = lpcb->next) {
    if ((lpcb->netif_idx != NETIF_NO_INDEX) &&          /* <-- 340 */
```

`struct tcp_pcb_listen` 的 `netif_idx` 正好落在偏移 48，与 `lbu a5,48(a5)` 和
`a5 = 0x...7f → +0x30 = 0x...af` 完全对上。

**所以 `a5` 里的坏指针就是 `lpcb` 本身**——不是 payload、不是 next 字段被踩，
而是**遍历 LISTEN pcb 链表时 `lpcb` 已经是垃圾**。也就是说
`tcp_listen_pcbs[search_lane].listen_pcbs` 这条链被破坏了。

这条链恰好是 `pcb_lane.h` 自己警告过的地方：

> "A wildcard pcb lives in the sentinel bucket *only*: it must never be linked
> into two lists, because the second insertion would overwrite its ->next and
> silently corrupt both lists."

telnetd 绑 `INADDR_ANY`，它的 LISTEN pcb 就在**哨兵桶**里；`search_lane` 的第二轮
（`NET_PCB_LANE_ANY`）遍历的就是这一桶。一个 LISTEN pcb 若被插入两次，`->next` 指向
另一条链，遍历其中一条就会走到不属于自己的节点，最终 `lpcb` 变成垃圾——与观测到的
**低 16 位跨运行恒定、高 48 位每次不同**（说明它不是随机内存，而是"从链表结构里
取出来的偏移量"）也吻合。

### 根因已确认：`tcp_pcb_remove()` 的 lane 被索引了两次

上面这条"哨兵桶重复链接"的假设是**错的**，`pcb_lane.h` 那段警告是红鲱鱼；
最后那段类型混淆推断也已撤回。真正的原因是纯地址算术错误：

`TCP_RMV(pcbs, npcb)` 会**自己**按 lane 索引：`(pcbs)[(npcb)->lane]`。
所以 `TCP_REG`/`TCP_RMV` 的所有调用方都必须传**桶数组基址**
（`tcp_bound_pcbs`、`tcp_active_pcbs`、`tcp_tw_pcbs`、`TCP_LISTEN_PCBS`）。

但 `tcp_pcb_remove()` 有 5 个调用方**预先按 lane 取了地址**再传进来：

```c
tcp_pcb_remove(&tcp_active_pcbs[pcb->lane], pcb);
tcp_pcb_remove(&TCP_LISTEN_PCBS[pcb->lane], pcb);
tcp_pcb_remove(&tcp_tw_pcbs[pcb->lane], pcb);
```

而 `tcp_pcb_remove()` 把参数原样交给 `TCP_RMV()`，于是 lane 被算了两次。
lane `L` 的 pcb 实际被摘/插到桶 `2L`：

| lane | 实际落到 | |
|---|---|---|
| 0 | 桶 0 | **纯属巧合**正确 |
| 1 | 桶 2 | 错桶 |
| 2 | 桶 4 | 错桶 |
| 3 | 桶 6 | **越界**（数组只有 `NET_PCB_LANE_BUCKETS` = 5）|
| 4 | 桶 8 | **越界**；lane 4 正是通配桶 |

于是 pcb 被从**别的**桶上摘链——在通配桶上甚至直接写坏桶外内存——
splice 会在本该离开的链上留下野 `->next`，同时把活指针塞进一条它从未加入过的链。
这正是 `tcp_in.c:340` 遍历 LISTEN 链时 `lpcb` 变成垃圾的原因，
`netif_idx` 在偏移 48，与 `lbu a5,48(a5)` 吻合。

**为什么锁探针抓不到**：这跟锁无关，是纯粹的指针运算错误，探针当然显示 0 违规。

**为什么之前所有门禁都是绿的**：lane 0 自相抵消，所以 `NET_LANES=1` 永远看不到；
而 smoke 里那唯一一个 LISTEN pcb 从不走被算错桶的摘除路径。只有
"NET_LANES>1 + 真实 LISTEN pcb 被增删"才会触发。

**修复**：调用方改回传基址数组，与其余 `TCP_REG`/`TCP_RMV` 调用方一致（`16304db8`）。

**验证**：`CONFIG_NET_LANES=4` + `NR_CPUS=4` + `a20.tcpmode=lwip` + 真实 TCP
listener，修复前 4 次运行 `faults=7`；修复后 `faults=0`，在开/关
`CONFIG_NET_PCB_SANE` 两种构建下各复测，均无 panic / page fault / 断言。
单 lane 回归 `smoke-net-accept`、`smoke-net-lanes-n1`、`smoke-network-suite` 全绿。

**但"不再崩溃"不等于"通过"。** `tcp_accept_test` 仍然时通时不通，这一项尚未修好：
端口扫描（同一配置，连续 8 个端口）结果为

```
12401 FAIL   12402 PASS   12403 FAIL   12404 PASS
12405 PASS   12406 FAIL   12407 PASS   12408 FAIL
```

4/8 通过，失败分布**无序**（按 `(local_ip, local_port)` 分桶的话，若成因是"某条
lane 全坏"或"某个桶溢出"，规律应当是周期性的，而实际不是）。失败形态固定为
`client=-1 server_status=256`，即 `server()` 返回负值、客户端在 4s 预算内 connect
失败，**不伴随任何内存破坏**（`faults=0`）。

**已排除的假设（别重走）**：`tcp_input()` 里 `listen_lane = pcb_lane`，而 `pcb_lane`
是按 `NET_PCB_LANE_OF(dst_addr, tcphdr->src)` 算的——用的是**源端口**；但 LISTEN pcb
在 `tcp_bind()` 里是按自己的 `local_port`（即入站段的**目的端口**）入桶的。看起来
应该改成 `tcphdr->dest`。**实测是错的**：改完之后 8 个端口只过 1 个（改前 4/8），
所以已回滚。

这条推理错在哪：它假设 LISTEN pcb 落在 `hash(dst_ip, dst_port)`。实测说明**不是**这样
——改用 `dest` 后命中率不升反降，说明 listener 实际所在的桶既不是
`hash(dst_ip, src_port)` 也不是 `hash(dst_ip, dst_port)`。而且改前的 4/8 也未必是
"按 src_port 撞对的运气"，因为 1/8 与 4/8 在 8 个样本内都可能是噪声。

### 更正一则不存在的构建缺陷（infra bug 是假的）

过程中曾记下"用户态构建目录 `user/build/riscv64/obj/*` 会在构建中途消失"
（`tlse.d`、`sbase/libutil.a`、`libutf.a`、`fastfetch/gen`），并当成一条待修的
构建缺陷。**这是误判，已作废。**

证伪实验：手动 `rm -rf user/build/riscv64/obj/tlse user/build/riscv64/obj/fastfetch/gen`
之后直接 `dev-build`，结果是 `BUILD=0`，两个目录都被构建重新创建。

也就是说 Makefile 里的 `@mkdir -p $(dir $@)` / `| build_dir` 顺序依赖是正确的，
目录本就会自建。当时之所以"消失"，是因为同时有另一个进程在反复 `git stash` /
`git stash pop`（`stash@{0}`、`stash@{1}` 都是 "WIP on main: f7f3d670"），
构建目录被并发动作删除，与构建规则无关。

**代价**：这个假象让第一次 `16304db8` 的证伪变成假阳性——门禁红了，但红在
缺 `tlse.d` 上，而不是红在缺陷上。教训是"门禁变红"必须同时确认失败原因。

### 阶段 C 剩余项的可行性评估：per-lane pbuf pool **不是局部改动**

先量了规模，免得按"小改动"排期：

- `memp.c` 里 `memp_pools[MEMP_MAX]` 是 25 个 `struct memp_desc *` 的静态表，
  `memp.c` 目前**完全不认识lane**（`grep -c lane memp.c` = 0）。
- `MEMP_MEM_MALLOC=1`（`lwipopts.h:68` -> `NET_PROFILE_MEMP_MEM_MALLOC`），
  每个 pool 底层是 `mem_malloc`，不是静态数组。

关键障碍：**lwIP 的分配器 API 没有 lane 这一维**。`memp_malloc(MEMP_PBUF)` 只收 pool id，
`do_memp_malloc_pool(desc)` 也没有 lane 参数。要做 per-lane pool 只有两条路：

1. 给每个分配点传 lane —— 要改遍全栈的分配调用，侵入极大；
2. 让 memp 自己按"当前 lane"索引 pool 数组 —— 但**当前 lane 从哪来是个坑**。

第2 条正是本次已经踩过两次的坑：lane 有两套来源。socket 层用绑定地址/端口
（`NET_PCB_LANE_OF_PCB`，权威值，`socket_inet.c:212`的`net_socket_lane_of_addr()`），
而 `socket.c:30` 那个 `s->lane = net_lane_of_cpu(cpu_current_id())` 注释明写
"Provisional only"。若 memp 用 `cpu_current_id() % CONFIG_NET_LANES` 选池，
就会**重新引入"CPU 派生 lane"与"地址派生 lane"两套语义**——那正是
`16304db8` / `f7f3d670` 两类 bug 的根源。

所以做per-lane pbuf pool 之前必须先定：pool 选择依据是地址派生 lane（则需要一个
在 lwIP 分配点可得的 lane 上下文，例如由 A20OS 侧在进入 lwIP 前设置一个
"当前 lane"），还是接受 CPU 派生（则与 PCB 分桶不一致，必须写清代价）。
**这个前提没定之前不建议动 memp。**

### 门禁现状：`smoke-net-accept` 本身是 flaky 的（与本次修复无关）

在最终干净树上复跑 `smoke-net-accept` 三次：

| 轮次 | 门禁 | guest 内 PASS 行数 |
|---|---|---|
| 1 | exit 2 | 1/2 |
| 2 | exit 2 | 1/2 |
| 3 | exit 0 | 2/2 |

失败形态固定是：boot-time `tcpmode=lwip` 那次 **PASS**，随后
`echo tcpmode fast > /proc/net/config` 的那次 **FAIL**（`client=-1 server_status=256`）。

即失败发生在**运行时切换 tcpmode** 这条路径上，而不是 lwIP lane 路径上：

- fast 模式由 socket 层直接配对两个 socket，不建 lwIP pcb，因此
  `f7f3d670`（`tcp_in.c` 的 `pcb_lane`）与 `16304db8`（`tcp_pcb_remove` 调用方）
  都不在这条路径上；
- 这与已记录的既有限制一致：tcpmode 需要 boot 参数，运行时 `/proc/net/config`
  切换后 listener 能建起来但数据通路不工作。

**所以不能把整套门禁报成全绿。** 准确说法是：
`smoke-net-lanes`、`smoke-net-lanes-n1`、新增的 `smoke-net-tcp-lanes` 稳定通过；
`smoke-net-accept` 因运行时切 tcpmode 的既有问题而 flaky（约 2/3 失败），
该问题独立于 lane 工作，且本次未修。

### 残留 flake：已定量，且**不是** lane 0 相关（假设已被证伪）

在稳定树上（并发写者已消失）用 `smoke-net-tcp-lanes` 的同一配置反复跑：

| 采样 | 结果 |
|---|---|
| 混合 8 端口 × 10 轮 = 80 次 | 79 通过 / 1 失败（`12405`）|
| 仅 lane 0 的 3 个端口（12404/12405/12407）× 10 轮 = 30 次 | **30/30 全通过** |
| 早前若干轮 | 另见 `12404` 失败 1 次 |

即总失败率约 **2%**，且**不是按 lane 分布**：

- 观察到 2 次失败（12404、12405）恰好都落在 lane 0，看着像 lane 0 有问题；
- 但专门只跑 lane 0 的三个端口时30/30 全过，反而比混合跑更干净；
- 同为lane 0 的 `12407` 从未失败过。

所以"lane 0 有问题"这个假设**被证伪**，不能按 lane 去查。

同时排除：

- 不是 `16304db8`（双重索引）—— 已被证伪验证：放回去会 `passes=0/8` + `list-checker hits=3` + panic；
- 不是 `f7f3d670`（源端口哈希）—— 放回去会 `passes=2/8`，量级远高于 2%；
- 不是链表损坏 —— `tcp_pcbs_sane` 全程 0 命中；
- 不是内存破坏 —— 无 panic / page fault；
- 不是并发写者干扰 —— 在干净树上同样复现。

形态固定为 `client=-1 server_status=256`，即 `server()` 返回负值、客户端在 4s 预算内
`connect()` 未完成。

**已确认的相关事实**（继续查下去时从这里起步）：

- 回环队列 `netif->loop_first` 的排空点是 `kernel_progress_timer_tick()` ->
  `a20_lwip_poll_timers_locked()`，它遍历 `netif_list` 对 `loop_first != NULL` 的
  netif 调 `netif_poll()`（`lwip_stack.c:574`）。
- **该排空只在 CPU 0 上发生**：`kernel_progress_timer_tick()` 开头就是
  `if (cpu_current_id() != 0) return;`（`progress.c:47`）。理由是 NO_SYS 下
  lwIP 只有一把全局 core lock，让每个空闲 CPU 都去轮询会变成锁护航。
- `LWIP_LOOPBACK_MAX_PBUFS` 在 `opt.h:1800` 默认为 `0`，而 `netif.c` 里限流判断
  写在 `#if LWIP_LOOPBACK_MAX_PBUFS` 内（1154-1166），所以**当前没有队列上限**。
  （`opt.h` 的注释写 "0 = disabled" 有误导：真正被编译掉的是限流，不是整个回环队列；
  `loop_first` 仍在使用。）

所以"每 tick 最多排空 N 个 pbuf 导致 SYN 饿死"这条假设**不成立**，可以排除。
"tick 会不会饿死"这条也可以排除：`kernel_progress_timer_tick()` 每次都会把定时器重装为
`proc_next_timer_interval(now)`，其下限是 `SCHED_TICK_INTERVAL = TICKS_PER_SEC/100`
即 **10ms**（`timer_heap.c:204`，更小的 `SCHED_MIN_TIMER_INTERVAL` 只用于已到期场景）。
10ms 远小于客户端 4s 预算，所以"tick 来不及排空 SYN"不成立。

至此这条 flake 已排除：lane 相关、链表损坏、内存破坏、队列上限、tick 频率，
以及本次修复的两个 bug（各自放回后的失败特征都远大于 2%）。

补一组样本：`NR_CPUS=1` + `NET_LANES=4` 连跑 5 轮 40 次全过（0 失败）。看起来像
"多 CPU 才会出现"，但**这组数据不构成证据**：若真实失败率就是 1.25%，连过 40 次的概率
约 0.6也就是说六成的可能性本来就会看到 0 失败。要区分"单 CPU 免疫"和"单 CPU 也一样，
只是没抽到"，单 CPU 侧至少要 150+ 次无失败才有说服力。别把这条当结论。

**尚未定位的是**：在 tick 与队列都正常的前提下，`connect()` 偶发不完成。
形态是 `server()` 返回负值 + 客户端 4s 内未连通，且无任何内存异常。
剩下的可疑面在 lwIP 之外——例如 `connect()` 的重试路径与 accept staging 之间的
时序，或 4 CPU 下任务被调度到非 0 号 CPU 时、由谁来触发排空。**未定位，不写成结论。**

### 阶段 C 范围更正：TIME-WAIT 分桶**已经**做完了

原计划里"TIME-WAIT per-lane 分片"这一项是**过时前提**，不用再做。实测：

- `tcp_tw_pcbs` 已经是 `struct tcp_pcb *tcp_tw_pcbs[NET_PCB_LANE_BUCKETS]`
  （`tcp_priv.h:339`、`tcp.c:178`），即每 lane 一个链表头；
- `tcp_slowtmr()` 已经是按 lane 遍历 `tcp_tw_pcbs[lane]`（`tcp.c:1505`）；
- TIME-WAIT pcb 在建链前就打好 lane 戳：`npcb->lane = NET_PCB_LANE_OF_PCB(npcb)`
  （`tcp_in.c:739`），而 `tcp_input.c:1052/1070/1080` 三处 `state = TIME_WAIT`
  都在这之后，因此进桶依据与桶数组下标一致。

**所以阶段 C 剩下的只有 per-lane pbuf pool**：目前 `MEMP_NUM_PBUF` 由
`NET_PROFILE_PBUF_POOL_SIZE / 2` 决定（`lwipopts.h:92`），是**单一全局池**，
`memp.c` / `pbuf.c` 里没有任何按 lane 切分的池。

顺带记录一个容易重复踩的坑：本文件此前把 `pcb_lane` 的桶来源写成"入站段的目的端口
在 `listen_lane` 上、源端口在 `pcb_lane` 上"，于是得出"两个 lane 要分别改"的结论。
实际只有**一个**表达式，`tcp_in.c:261` 同时决定 active / TIME-WAIT / LISTEN 三处查找；
只改 `listen_lane` 会把失败推迟到子 pcb 查找，表现为 4/8 -> 1/8 变差。

### 两个修复都已被门禁证伪验证（`88ba28a9`）

`smoke-net-tcp-lanes` 在 4 lane + `CONFIG_NET_PCB_SANE=1` 下跑 8 个端口（每 lane 一个
真实 LISTEN pcb 的建立/匹配/拆除）。分别把两个 bug 重新放回去，确认门禁**因正确的原因**
变红，而不是因为构建缓存坏掉：

| 放回的 bug | 门禁结果 | 判定 |
|---|---|---|
| 修复后的树 | exit 0，`passes=8 of 8`，checker hits=0 | 绿 |
| `16304db8` 双重索引 | exit 2，`passes=0 of 8`，**list-checker hits=3**，guest 侧 `KERNEL PANIC` |红，且是**链表损坏**特征 |
| `f7f3d670` 源端口哈希 | exit 2，`passes=2 of 8`，**list-checker hits=0** | 红，且是**查找未命中**特征 |

两种 bug 的签名不同（`0/8 + 断言` vs `2/8 + 无断言`），说明门禁不是靠"碰巧失败"，
而是分别命中了两类不同的缺陷：前者破坏链表、后者只是查不到桶。这正是设计意图。

> 第一次尝试证伪 `16304db8` 时门禁也变红了，但原因是用户态构建缓存缺
> `build/riscv64/obj/tlse/tlse.d`，属于假阳性。补齐缓存目录后重做才得到上表。
> **教训**：证伪必须确认失败原因，否则"门禁变红"毫无意义。

### 根因机制已定位（修复尚未找到）

在 `tcp_listen_with_pcbs()` 里打印 `lpcb->lane` 与 `NET_PCB_LANE_OF_PCB(lpcb)`，
`NET_LANES=4` 实测：

```
[DIAG] listen: port=2323  lane=4 hashed=3 any=4 buckets=2   <- telnetd，通配绑定 → 哨兵桶
[DIAG] listen: port=12401 lane=2 hashed=2 any=4 buckets=2
[DIAG] listen: port=12402 lane=1 hashed=1 any=4 buckets=2
[DIAG] listen: port=12403 lane=2 hashed=2 any=4 buckets=2
[DIAG] listen: port=12404 lane=0 hashed=0 any=4 buckets=2
[DIAG] listen: port=12405 lane=0 hashed=0 any=4 buckets=2
[DIAG] listen: port=12406 lane=3 hashed=3 any=4 buckets=2
[DIAG] listen: port=12407 lane=0 hashed=0 any=4 buckets=2
[DIAG] listen: port=12408 lane=3 hashed=3 any=4 buckets=2
```

两条事实合起来就是根因：

1. `lpcb->lane` 恒等于按 `(local_ip, local_port)` 算出的 `hashed`，listener **确实**
   落在自己端口对应的哈希桶里（telnetd 因为绑 `0.0.0.0` 正确落在哨兵桶 4）。
2. `NET_PCB_LANE_SEARCH_BUCKETS == 2`，所以 `tcp_input()` 的 LISTEN 查找**只搜两个桶**：
   `listen_lane` 和哨兵桶。而 `listen_lane = pcb_lane =
   NET_PCB_LANE_OF(dst_addr, tcphdr->src)` —— 用的是**客户端随机临时源端口**。

也就是说，**入站 SYN 能否命中 listener，取决于客户端随机源端口恰好哈希到同一个桶**，
约 `1/NET_LANES`。这与实测通过率一致（4/8、1/8 等，量级就是随机碰撞），
也解释了为什么 12404/12405/12407 同在 lane 0 却结果不同——它们的差别只可能来自
那次连接的临时源端口。`NET_LANES=1` 时所有桶都是 0，永不碰撞，故 8/8。

**仍未解决**：把 `listen_lane` 改成用 `tcphdr->dest`（目的端口，语义上才是对的）
实测反而更差（4/8 → 1/8），说明 `ip_current_dest_addr()` 在回环路径上算出的哈希
与 listener 的 `local_ip`(=127.0.0.1) **不一致**。这是第二个、尚未定位的问题。

下一步应先确认回环段进入 `tcp_input()` 时 `ip_current_dest_addr()` 到底是什么
（是否 127.0.0.1，还是被 loopif 改写成了别的地址），因为 `net_pcb_lane_ip()`
一旦对两者取值不同，上面那个"改成 dest 就够了"的修复才不会成立。
下一步先**确认 listener 到底在哪个桶**（在 `tcp_listen()` 处把
`lpcb->lane` 与 `NET_PCB_LANE_OF_PCB(lpcb)` 打印出来），再谈改法。现在这条路径上
还有一个未解释的事实：`NET_PCB_LANE_SEARCH_BUCKETS` 搜两个桶（`listen_lane` 与哨兵桶），
若 listener 在哨兵桶就该 8/8，若在哈希桶就该由 `listen_lane` 命中——两者都和实测矛盾。

**已定位到多 lane 路径**（同一条 sweep，仅改 `NET_LANES`）：

| `NET_LANES` | 12401..12408 | 通过 |
|---|---|---|
| 1 | 全部 PASS | 8/8 |
| 4 | F P F P P F P F | 4/8 |

`NR_CPUS` 固定为 4。所以这不是回环 TCP 或 `tcpmode=lwip` 本身的毛病——单 lane 全过；
**只有 lane 数 > 1 时才出现，且不是按桶周期分布**。指向多 lane 特有的状态
（per-lane 定时器/轮转、per-CPU 分发、跨 lane 的 loopback 收发），下一步应沿
"同一连接在多 lane 下走了不同代码路径"去查，而不是继续查 LISTEN 链表。

> 更正：`16304db8` 的提交信息曾写 "PASS 0 -> 3/4"，那是误读——那轮同时跑了
> 12401 和 12402 两个用例，`grep -c` 数的是 PASS **行数**，而 12401 当时就已经
> 在失败了。崩溃数 7 -> 0 是实测的，"测试通过"不是。

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