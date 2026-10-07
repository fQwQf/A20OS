# 网络 lane

最后核实：阶段 A–E **均已落地**（按落地顺序）：
阶段 A（`kernel/include/net/net_lane.h`、`kernel/net/net_lane.c`、`net_socket_t.lane`、
`net_socket_lane_of_addr()`、`/proc/net/status` 的 lanes 行）、
阶段 B（lwIP PCB 链表按 lane 分桶，含通配哨兵桶）、
阶段 C（`5ea06a786`：当前 lane 上下文 + memp 的 `MEMP_PBUF`/`MEMP_PBUF_POOL` lane
索引**骨架**）、阶段 D（`f6f327b96`：IRQ 只按 lane 入队、协议处理移到进程上下文）、
阶段 E（`7c7a4d7c8`：per-socket 锁取代桶锁保护全部 socket 状态）。

**仍未落地**的是 F（多队列 + 每队列中断）与 G（RSS / 流引导），以及阶段 C 的
**内存真分片**与阶段 D/E 都未触及的 `g_lwip_lock` 分片。本文件记录顺序、前置条件，
以及每阶段"做到哪、剩什么"的边界。

> **一处必须先更正的旧记录。** 本文「阻塞」一节里"为什么当初没有任何断言拦住它"
> 那一段，是**提交 `7d217d3fd` 之前**的状态。该宏现在已接到 `g_lwip_lock` 的持有者
> CPU 上，见下文「断言已接线」。保留原文是因为它记录了方法与教训，但**不要再把它读成
> 当前状态**。同一节还有一处把 `tcp_abort()` 记成"没有断言"，那也是错的，已在该处更正。

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
A  lane 骨架与归属哈希            无                                  ← 已落地
B  lwIP PCB 链表按 lane 分桶       需要 A                              ← 已落地
C  per-lane pbuf 池与定时轮        需要 B                              ← 骨架已落地，内存未分片
D  收包投递给目标 lane             需要 C                              ← 已落地
E  per-socket 锁替 net 桶锁          需要 D，且先要有对象引用计数            ← 已落地
F  多队列 + 每队列中断             需要 D                              ← 未做
G  RSS / 流引导                    需要 F                              ← 未做
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

- 被动开放：`tcp_listen_input()`（`tcp_in.c:665`）把**具体**的
  `ip_current_dest_addr()` 拷进新 PCB
- 主动开放：`tcp_connect()`（`tcp.c:1102`）在发 SYN 前先经 `ip_route()` 定出具体
  `local_ip`

所以缺陷**只在 LISTEN 路径**（`tcp_in.c:339-420` 遍历 `tcp_listen_pcbs`），
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
**已经宏抽象**（`tcp_priv.h:391` `TCP_REG`、`:406` `TCP_RMV`、`:453`/`:459`
ACTIVE 变体），且表头是**参数**，所以分桶比"重写 TCP 查找"小得多：

| 现在 | 之后 |
|---|---|
| `tcp_bound_pcbs`、`tcp_active_pcbs`（`tcp.c:171,176`） | `[NET_LANES]` |
| `tcp_pcb_lists[]`（`tcp.c:183`，含 listen/bound/active/tw 四条） | 每条各一个数组 |
| `udp_pcbs`（`udp.c:83`） | `[NET_LANES]` |
| — | `struct tcp_pcb` / `udp_pcb` 加 `u8_t lane` |
| `tcp_lookup()` 四条链表全遍历 | 只进 `net_lane_of()` 那一个桶 |

`NET_LANES == 1` 时全部退化为下标 0，行为不变 —— 这是这个补丁能安全落地的依据。

lwIP 2.2 已有 `tcp_active_pcbs_changed`（`tcp.c:187`）这个代际标志，说明"用代际计数
替代遍历"在上游被认可。顺带一提：`kernel/net/netfilter.c` 的文件头曾**声称**有同款
机制而代码里没有，已在另一提交中补上。

### 头文件声明审计：第二例，以及一次把自己当骗子的审计

上面那处之后又做了一次同类审计，范围是 `kernel/include/net/netfilter.h` 的文件头
（conntrack + NAT 那一节）。结论记在这里，因为**方法上的教训比结论更值得留**。

头里几条机制性声明，逐条对代码：

| 声明 | 审计结果 |
|---|---|
| "conntrack ... **Always on**" | **不成立**。`/proc/a20/netfilter` 有 `cton`/`ctoff` 动词，`netfilter_conntrack_set_enabled()` 门控插入。实际语义是"默认开、运行时可关"，且关只停**新**条目、保留既有条目，以免拆掉正在用的 NAT 绑定。已改。 |
| hook 在 `a20_lwip_process_netif_rx_tx_locked` / `a20_lwip_linkoutput` 下调用 | 成立。`lwip_stack.c:542` / `:232`，函数边界已核。 |
| 清扫从 `a20_lwip_poll_timers_locked()` 调用 | 成立。`lwip_stack.c:651`。 |
| "/proc 读者取同一把锁而非 seqlock" | **成立，但审计者一度判错**。 |

最后一条是这次审计真正的收获。`netfilter_format()` 在 `netfilter.c:641` 已经取了
`g_lwip_lock` 再调 `netfilter_nat_format()`，所以 connwalk 并没有漏锁。但当初那次
grep 只覆盖了 `lwip_stack.c` 和 `procfs.c`，没覆盖 `netfilter.c`，读出来像"缺一把锁"，
于是加了一把。加锁的后果是自死锁——`spin_lock_irqsave()` 不可重入。

它**不像自死锁**：单 CPU 构建上表现为 `netfilter_nat_format` 里 owner=-1、spins 20 亿的
`[LOCK-STALL]`，因为 `g_lwip_lock` 的 owner 字段只在 `CONFIG_NET_LOCK_ASSERT=1` 下维护，
否则恒为陈旧值，看起来就是"有别人拿着"。真正定位靠的是 trylock 探针：62 次渲染
**全部**获取失败，而单 CPU 上 trylock 失败只可能是当前 CPU 已经持有。教训是：怀疑锁
问题先量 reentrancy，别先读 owner 字段——在这个构建里 `owner=-1` 不是信息。

对 lane 改造的净影响：conntrack 是**必须保持全局**的状态（见下节表格），所以 lane 拆分
时它不参与分桶，`g_lwip_lock` 拆成 per-lane 之后 conntrack 会成为需要单独处理的那个
全局点。这条已写进
[network-lock-contract.md](./network-lock-contract.md) 的理由里——不另设专用锁，正是为了
不让它在 lane 化之后顶替 `g_lwip_lock` 成为新的串行点。

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
   不能用——它 miss 路径会 `spin_unlock` 后调普通 `kmalloc()`（`objcache.c:41-42`），
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
  只在 `tcp_fasttmr` / `tcp_slowtmr` 里出现（`tcp.c:251,1263,1269,1560,1562`），
  按 lane 切开是自洽的。
- `tcp_ticks` **不是**。它是整个协议栈的共享时基，而且在**收包与发包路径**上被写入：
  `pcb->tmr = tcp_ticks`（`tcp_in.c:809,889`）、`pcb->rttest = tcp_ticks`
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
`net_socket_t::lane` 目前只被写、从不被读——`grep` 全树只有 `kernel/net/socket.c:30` 与
`kernel/net/socket.c:389` 两处赋值，没有任何 dispatch、锁选择或桶索引用它。阶段 A 只留了字段，
阶段 D 才会消费它。

留在这里是因为它会在阶段 D 变成真 bug：一旦按 socket 的 lane 选处理 lane，
通配 bind 的 listener 就会被派到**不拥有它 pcb 的那个 lane**，而哨兵桶里的 pcb 又不
被任何 lane 的计时器遍历。修法是让两者对 any 的处理一致——要么 socket 侧也返回哨兵值，
要么哨兵桶只用于查找、而 owning lane 仍然有明确定义。这属于阶段 D 的前置条件。

再排除一个：哨兵桶本身没有被越界。`tcp_bound_pcbs` / `tcp_listen_pcbs` /
`tcp_active_pcbs` / `tcp_tw_pcbs` / `tcp_timer[]` / `tcp_timer_ctr[]` 全部声明为
`[NET_PCB_LANE_BUCKETS]`（= N+1），而不是 `[CONFIG_NET_LANES]`；`tcp_pcb_lists[]`
按状态索引后再按 lane 索引，元素类型是 `struct tcp_pcb **`，与这些数组匹配。
`tcp_listen_pcbs` 的三处使用（`tcp.c:895,2488,2661`）也都以
`NET_PCB_LANE_BUCKETS` 为界。所以"`CONFIG_NET_LANES` 大小数组被 `NET_PCB_LANE_ANY`
索引"这个猜测是错的。

**为什么当初没有任何断言拦住它：`LWIP_ASSERT_CORE_LOCKED()` 当时是空的。**
（**这是 `7d217d3fd` 之前的状态**，见下文「断言已接线」。）
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
- RST 生成侧：`tcp_abort()` 的调用点（`tcp_in.c:531,1004`）本身带
  `LWIP_ASSERT_CORE_LOCKED()`，虽然断言是空的，但取锁由上层
  `a20_lwip_process_netif_rx_tx_locked()` 保证。
- netif 地址变更侧：`a20_lwip_if_set_addr()` 在 `netif_set_ipaddr()` **之前**就取了锁
  （`kernel/net/lwip_stack.c:1045`），所以会触发 `tcp_listen_pcb_rebucket()` 的跨 lane
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
只能看见**存在断言**的函数：`tcp.c` 里有一批函数没有 `LWIP_ASSERT_CORE_LOCKED()`。
当时我据此把 **`tcp_abort()`（记作 `tcp.c:654`）**也列了进去——**这条记错了**：
`tcp_abort()` 一直带着断言，它在当前树里是 `tcp.c:658`，断言在 `:660`（它是个两行包装，
转发给 `tcp_abandon(pcb, 1)`，后者自己在 `:590` 也带一条）。`7d217d3fd` 的提交说明
沿用了同一个错行号（写的是 `tcp.c:656`）。这条记录错在**行号**，不在结论方向：
探针当时确实漏了一批入口，只是 RST/abort 那条并不在漏的里面。

真正被漏掉、且补断言后仍然值得盯的是 `tcp_slowtmr` / `tcp_fasttmr` /
`tcp_process_refused_data` / `tcp_alloc` / `tcp_new` / `tcp_new_ip_type` /
`tcp_netif_ip_addr_changed` 这七个，它们已由 `7d217d3fd` 补上（行号见
[network-lock-contract.md](network-lock-contract.md)「移植层自己补的断言」）。

这个盲点是可以补的，而且成本很低：给这些入口补上断言（或者统一在一个 wrapper 里
断言），再跑一次同样的探针，盲区就变成覆盖区。`tcp_abort` 的调用方逐个查过
（`socket_inet.c:729`、`tcp_in.c:531,1004`、`altcp_tcp.c:310`）都在 `g_lwip_lock`
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

### 断言已接线：上面那个前置动作已经做完了（`7d217d3fd`）

`LWIP_ASSERT_CORE_LOCKED()` 现在由 `kernel/net/lwip_port/lwipopts.h:298-304` 映射到
`a20_lwip_assert_core_locked(__builtin_return_address(0))`。细节、开关
（`CONFIG_NET_LOCK_ASSERT`，`net_profile.h:44-45` 默认 0）与 `/proc/net/stats` 的
可见性写在 [network-lock-contract.md](network-lock-contract.md)「核心锁断言」一节。

对上面这条线的净影响，有三件事必须说清楚：

1. **上面那个"探针对最可疑路径是盲的"的缺口已经补掉**，七个此前无断言的裸入口
   （`tcp_new` / `tcp_new_ip_type` / `tcp_slowtmr` / `tcp_fasttmr` /
   `tcp_netif_ip_addr_changed` / `tcp_process_refused_data` /
   `tcp_trigger_input_pcb_close`）现在都在探针覆盖之内。上面记的"`tcp_abort` 没有
   断言"是错的，见上一段的更正。
2. **它没有、也不可能解释 `16304db8` 那个崩溃**：那是一条纯地址算术错误（lane 被索引
   两次），与锁无关，探针显示 0 违规是正确的观测，不是探针失灵。
3. **它在两轮放大实验里全程 `violations=0 sites=0`**（78 次连接，见下文「放大实验已
   执行」），所以它是一条**负面证据**：那条 flake 不经由"未持锁调 lwIP 入口"这条路
   发生。它**不构成**"那条 flake 已定位"的证据。

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
（`NET_PCB_LANE_OF_PCB`，权威值，`socket_inet.c:213`的`net_socket_lane_of_addr()`），
而 `kernel/net/socket.c:30` 那个 `s->lane = net_lane_of_cpu(cpu_current_id())` 注释明写
"Provisional only"。若 memp 用 `cpu_current_id() % CONFIG_NET_LANES` 选池，
就会**重新引入"CPU 派生 lane"与"地址派生 lane"两套语义**——那正是
`16304db8` / `f7f3d670` 两类 bug 的根源。

所以做per-lane pbuf pool 之前必须先定：pool 选择依据是地址派生 lane（则需要一个
在 lwIP 分配点可得的 lane 上下文，例如由 A20OS 侧在进入 lwIP 前设置一个
"当前 lane"），还是接受 CPU 派生（则与 PCB 分桶不一致，必须写清代价）。
**这个前提没定之前不建议动 memp。**

### 阶段 C 落地记录：当前 lane 上下文 + memp lane 索引骨架（`5ea06a78`）

上面那个前提已经定了，选**地址派生 lane**。因此：

- 新增「当前 lane」上下文 `net_lane_ctx_push/pop/get`（`kernel/include/net/net_lane.h:232`
  / `:239` / `:244`，**只在 `CONFIG_NET_LANES > 1` 下存在**；实体 `a20_net_lane_cur`
  的 extern 声明在 `net_lane.h:228`，定义在 `kernel/net/net_lane.c:26`）。
  它是一个上下文，不是一个新的派生式。
- **没有、也明确禁止 `cpu_current_id()` 选池。** 这不是为了守规矩好看：一旦 memp 里
  出现第二种"lane 是什么"，一条连接的 pcb 与 pbuf 就可能落在两条 lane 上，而那正是
  `16304db8` / `f7f3d670` 两类 bug 的机制。全树只能有一个答案。
- 存储是一个全局量而不是 per-CPU：本树没有 per-CPU 设施。正确性来自
  `g_lwip_lock` 独占 + `a20_lwip_unlock()` 清零——退出锁时上下文归 0，不跨锁泄漏。

各入口在进入 lwIP 核心前声明自己的 lane：

| 入口 | 声明的 lane | 备注 |
|---|---|---|
| RX 排空 `a20_lwip_process_netif_rx_tx_locked()` | `net_lane_of_ip(netif 的 IPv4)` | **作用域式** push/pop：socket 的发送路径会经 `a20_lwip_poll_locked()` 走到这里，pop 才能让排空时释放的 loopback 回声仍算在那个 socket 头上，而不是算到 netif 头上 |
| socket 系统调用（bind / connect / listen / send / accept / recved …） | `socket->lane` | 19 处；accept 阶段的 pcb 交接用 `listener->lane`，因为子连接此刻还没 bind，`child->lane` 还是那个标注为 provisional 的 CPU 派生值 |
| `a20_lwip_unlock()` | 清零 | 无参调用点无需配对 |

`memp` 侧只对 `MEMP_PBUF` / `MEMP_PBUF_POOL` 两个池按当前 lane 索引；其余池的内容
既不按包也不按连接，保持全局。lane 0 指向上游那个描述符对象本身，
`memp_pools[]` 与 `lwip_stats.memp[]` 的指向因此和上游一模一样；lane 1..N-1 是
`memp_init()` 里取的副本。

**落地的是骨架，不是 per-lane pool。** 理由必须写在这里，否则下一个人会按名字
读大它：

- `net_profile.h` 三档全是 `MEMP_MEM_MALLOC=1`，该模式下描述符只贡献一个大小，
  两份同样大小的描述符从同一块 lwIP 堆上取。**内存没有被分片**。真分片要走
  `!MEMP_MEM_MALLOC` 的静态预留布局，每条 lane 一份 base 数组和空闲链表，
  `lwipopts.h` 的 profile 静态断言目前只按一份拷贝预留 `.bss`。
- 释放路径不变：`do_memp_free_pool()` 在该模式下忽略描述符，元素回到同一块堆。
- 因此今天唯一可观测的是 per-lane 的**分配/释放单调计数**，渲染在既有
  `/proc/net` memp 表下方（仅 `CONFIG_NET_LANES > 1`）。
  之所以是计数而不是"已用"水位：`memp_free()` 只拿到 pool id 和指针，说不出这元素
  是哪条 lane 发出去的；按释放方记会漂移，按分配方记需要 `struct pbuf` 里没有的
  所有权标签，而按前一种口径喂出来的水位会在第一次 lane 不匹配时下溢 `u16_t`。

**RX 排空用的 lane 不是包的归属 lane。** netif 没有自己的 lane（`struct netif`
未改，见 DIVERGENCE.md §2.4），只能拿它自己的地址去 `net_lane_of_ip()`，理由与
ARP/ICMP/NDP 一致。包的归属 lane 要等解析出连接才知道，那是阶段 D。所以今天
所有 lane 的连接仍经同一个 netif 进来，在 netif 那条 lane 上被排空。

**阶段 C 还剩下的**（不要当成已完成）：

- **内存分片没做**，如上。要做需要先把三个 profile 切到 `!MEMP_MEM_MALLOC` 的静态
  预留布局，并让 `lwipopts.h` 的静态断言按 `CONFIG_NET_LANES` 倍预留 `.bss`——那是
  一次会改变嵌入式构建内存占用的决定，不是本阶段能顺手带上的。
- **per-lane 定时器遍历的入口还没导出。** `b1bb28b5` 把 TCP 快/慢定时器按 lane 分片
  了，但驱动遍历的那个入口目前是全局的，`a20_lwip_poll_timers_locked()` 里的
  loopback 排空**刻意继承调用者的 lane**（代码里有注释说明为什么），所以定时器侧
  目前没有自己的 lane 声明点。
- **pbuf 的归属 lane 还无从得知**，阶段 D 才能解；在此之前 per-lane 池表分的是
  "谁在分配"，不是"这个包属于谁"。

#### `CONFIG_NET_LANES=1` 等价：实测结论，以及两个比想象中难缠的坑

`memp.o`、`lwip_stack.o`、`socket_inet.o` 在一 lane 下 `.text` / `.rodata` /
`.sdata` 与改动前**逐字节相同**；再加 `-fno-sanitize=undefined` 重编，三个目标文件
整体也逐字节相同。仅存的差异是 UBSan 内嵌的 `SourceLocation` 行号表——文件多了行
它就变，**加一行注释也一样会变**，所以这不是本改动特有的，也不代表行为差异。

两个坑值得留在案上，因为它们的结论都是反直觉的：

1. **`static inline` 空函数体不够。** 一开始 `a20_lwip_lane_enter()` 写成头文件里的
   `static inline`、一 lane 下函数体为空。实测 `socket_inet.o` 的 `.text` 变了：
   一个函数的栈槽分配被打乱（`sd a4,56(sp)` 与 `sd a4,64(sp)` 互换），`.text` 短了
   4 字节。原因是空 inline 仍然把 `s->lane` 当实参传下去，而这个实参的死活判定
   早到足以扰动寄存器分配。改成一 lane 下展开为空的宏之后，`s->lane` 根本不被读。
   **写成 out-of-line 函数更糟**：19 个调用点会各多一条重定位和一次调用。
2. **"让优化器折叠"不是等价证明。** `memp_desc_for()` 在一 lane 下现在是宏
   `memp_pools[type]`，即上游原句。两种写法编出来的机器码相同（实测 `.text` 相同），
   但"编译器会折叠"是对某个编译器版本的断言，"一 lane 下预处理结果就是上游那句"
   是可以 `grep` 出来的事实。铁律该按后者守。

### 阶段 C2 落地记录：memp 按 lane 索引推广到全部池，以及它**不是**内存分片

阶段 C 只给 `MEMP_PBUF` / `MEMP_PBUF_POOL` 两个池做了 lane 索引，其余 22 个保持全局。
阶段 C2 把索引推广到**每一个池**，理由是直接的：`g_lwip_lock` 切开之后，若
`tcp_pcb_alloc()` 之类仍走全局表，"部分分片"既不是原来的全局锁、也不是新的分片锁，
是第三种更坏的状态。**能统一就统一**，所以选了统一。

**必须先说清楚 `MEMP_MEM_MALLOC=1` 下"池"是什么**，因为这一节的名字很容易被读大。
三档 profile 全是 `MEMP_MEM_MALLOC=1`（`net_profile.h:92`/`:357`/`:418`），该模式下：

| 问题 | 答案 | 出处 |
|---|---|---|
| 有 free-list 吗 | **没有**。`memp_init_pool()` 是空桩 | `src/core/memp.c:343-345` |
| 有静态数组吗 | **没有**。描述符只贡献一个 `size` | `src/include/lwip/priv/memp_priv.h:130-146` |
| 那分配走哪 | `mem_malloc(MEMP_SIZE + MEMP_ALIGN_SIZE(desc->size))` | `src/core/memp.c:449` |
| 释放走哪 | `LWIP_UNUSED_ARG(desc)` 后直接 `mem_free()`，元素回到**同一块**堆 | `src/core/memp.c:585-587` |

所以在 MEMP_MEM_MALLOC=1 下，**池既不是计数器也不是堆分区，它什么都不是**——它只是
一个把 pool id 翻译成元素大小的查表，真正干活的分配器是 `mem.c` 的全局堆。因此：

- **per-lane 化没有 ×N 的内存放大。** 本阶段编排给的预期是"per-lane 化会放大内存
  占用（×N）"，**在这棵树上前提不成立**：池元素仍从同一块 `MEM_SIZE` 堆上取，没有
  按 lane 预留的东西可以放大。三张 per-lane 表是 0 / 784 / 1904 / 4144 B
  （N=1/2/4/8），**三档完全相同**，因为它不随任何一档的堆缩放。
- **本阶段编排给的另一条判断也不成立**："`tcp_pcb_alloc()` 仍会踩同一个 free-list"。
  没有 free-list 可踩。两条 lane 并发 `tcp_pcb_alloc()` 走的是两次 `mem_malloc()`，
  不共享任何 memp 内部状态。
- **被按 lane 切开的只有两样东西**：一是描述符表（将来挂 per-lane base 数组的那个索引），
  二是 per-(lane, pool) 的 alloc/freed 单调计数。

per-lane 副本里的 `stats` 指针**刻意保持指向同一个 `stats_mem`**：`lwip_stats.memp[]`
与 `/proc` 渲染器把这些计数器当全局量读（`lwip_stack.c` 报
`lwip_stats.memp[MEMP_TCP_PCB]->used` 为 `tcp_active`），给每条 lane 一份私有 stats 块
会在多 lane 构建上把这个数悄悄重新定义成"lane 0 的 TCP PCB 数"。能拆的拆了，被当总量读
的仍然是总量。

`/proc/a20/netmem` 的渲染随之从两个 pbuf 池扩到全部池，表头由 `pbuf lane` 改成
`memp lane`，并按 lane 汇总所有池（`MEMP_MAX × CONFIG_NET_LANES` 行会淹没读者要的那
一行）。**注意 `smoke-lwip-memp` 门禁按 `NET_LANES` 默认值 1 构建**（`Makefile:127`
`NET_LANES ?= 1`，该目标不覆盖它），因此整段 lane 行在 N=1 下根本不渲染，该门禁的
期望值**无需改动**——形状变了，但只在 N>1 时可见。

#### 阶段 C2 查到的一件比本阶段更要紧的事：切 memp 并不足以让 `g_lwip_lock` 可切

本阶段被赋予的定位是"锁分片的硬前置"。查完之后必须如实记下：**在这个配置下它不是。**
`g_lwip_lock` 切开之后真正会被并发共享、且**没有任何内部保护**的可变结构，是 `mem.c`
里那一块全局堆——`ram_heap[MEM_SIZE_ALIGNED]` 静态数组加唯一的全局 `lfree` 空闲链表。
实测证据（每一条都独立核过）：

| 事实 | 出处 |
|---|---|
| `NO_SYS 1` | `kernel/net/lwip_port/lwipopts.h:6` |
| `mem_mutex` 只在 `#if !NO_SYS` 下声明，本配置下根本不编译 | `src/core/mem.c:375-377` |
| `LWIP_MEM_FREE_PROTECT()` 定义成 `sys_mutex_lock(&mem_mutex)` | `src/core/mem.c:397` |
| `NO_SYS=1` 时 `sys_mutex_lock(mu)` 展开成空 | `src/include/lwip/sys.h:63` |
| `LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT` 全树未覆盖 = 0，故走上面这条空互斥而非 `SYS_ARCH_PROTECT` | `src/include/lwip/opt.h:400` |
| 预处理后 `mem_free()` 里两处 `LWIP_MEM_FREE_PROTECT/UNPROTECT()` 都是裸 `;`，`lfree` 改写与 `plug_holes()` 全程无保护 | `gcc -E -P kernel/external/lwip/src/core/mem.c` |
| `SYS_ARCH_PROTECT` 也救不了：`arch_local_irq_disable()` 只关本 CPU 的 IRQ | `kernel/net/lwip_compat.c:53-56` |

于是分片锁之前必须先给堆定归属，两条路都有实质代价：**要么**每条 lane 一块堆（回到
`!MEMP_MEM_MALLOC` 的静态预留布局，并把 ×N 记进三档 profile 的账——`net_profile.h`
注释早已把它标为"profile 级的决定"），**要么**给这块堆一把专用的全局分配器锁（但
`NO_SYS=1` 意味着没有现成的 OS 互斥可用，得接到 A20OS 自己的原语上）。**本树两条都
还没选，本阶段也不选。** 阶段 C2 的 per-lane 索引是这两条路的前置件，不是替代品。

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

### 更正一则：2026-10-06 一轮里这条 flake 完全没复现（deferred，不是"已修"）

上面那张表与"约 2/3 失败率"是当时三次采样得出的。**本轮（`feat/net-strengthening`
合并后的树）一次都没复现**，所以本节按 **deferred** 记，不声称已修。

本轮实际执行的（由正确性加固那条工作流在
`/home/fqwqf/OS/A20OS-wt-correctness` 里跑，主工作区未参与；以下数字转引自该流的
实验记录，**本文档作者未复跑**）：

| 项 | 结果 |
|---|---|
| `make smoke-net-accept` | 10/10 PASS |
| 12 轮 fast↔lwip 运行时切换（每轮换一个 `tcp_accept_test` 端口） | 24/24 PASS |
| lwip → fast 之后：`tcp_loopback_test` ×2 + `tcp_accept_test` | 全过 |
| fast → lwip 之后：同样两组 | 全过 |
| 跨模式 `tcp_edge_test`，两次执行 | 均 PASS |

即上文那张"boot-time lwip PASS、运行时切换 FAIL"的形态，在约 40 次执行里一次未现。

**诚实边界**：这只能说明**触发条件在本轮环境里不存在**，不能说明它不存在。原因未查明，
两种可能都没被排除——

- 记载本身来自更早的脏树（当时确有另一个进程在并发 `git stash` / `stash pop`，
  见下文「更正一则不存在的构建缺陷」一节），那么这个 flake 从来就不在当前代码里；
- 触发条件依赖某个本轮恰好没有出现的环境维度。

因此写入口 `/proc/net/config` 的 `tcpmode` **功能原样保留、未削减**，
[network-config-design.md](network-config-design.md) 里那条"部署一律用命令行键"的
建议也随之保留，只是它的依据从"实测可复现"降级为"机理未查明"。

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

> **2026-10-06 更新——这个 2% 在当前树上不再复现。** 同一配置下又跑了 797 次实际执行
> （含一轮放大器归零），0 失败，`0.98^797 ≈ 1.1e-7`。本轮**没有为它改过实现**，
> 所以这不是"已修"，而是"那份 2% 很可能来自更早的脏树"。原始数据、被排除的假设、
> 以及一个必须先修的测量方法问题（guest 串口会吃掉命令名里的字符，导致
> `grep -c PASS` 把没执行的样本算成没失败）见下文
> 「放大实验第二轮（2026-10-06）」。

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
  `if (cpu_current_id() != 0) return;`（`progress.c:95`）。理由是 NO_SYS 下
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
  （`tcp_in.c:739`），而 `tcp_in.c:1052/1070/1080` 三处 `state = TIME_WAIT`
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

### 放大实验已执行（2026-10-05）：**没炸，也没定位**

上面那份配方被执行了两轮。结论先写清楚：**这条 flake 在本轮实验里没有复现，
因此没有被修，也没有被定位。** 下面是原始数据，不做美化。

放大器与探针的位置（先确认它们真的在，才有意义）：

- `CONFIG_NET_RACE_DELAY_US` 门控在 `kernel/net/net_profile.h:201`（默认 0），
  注入点是 `kernel/net/socket_inet.c:926-932` 的 `for (volatile uint32_t d = 0; ...)`，
  位置确实在 accept stage 取出 slot 之后、第一次 `a20_lwip_lock()` 之前——
  正是配方第 1 步要求的那段窗口。
- 探针 `-DCONFIG_NET_LOCK_ASSERT=1` 走第 1 项接好的 `a20_lwip_assert_core_locked()`：
  armed 之后调用者 CPU 不是持有者就 abort，并累加 `violations` / `sites`。

配置：`make ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=4 NET_LANES=4
OPT="-DCONFIG_NET_PCB_SANE=1 -DCONFIG_NET_LOCK_ASSERT=1 -DCONFIG_NET_RACE_DELAY_US=<D>" dev-build`，
guest 参数 `a20.tcpmode=lwip`，每轮换一批端口跑多连接。

| 轮次 | `CONFIG_NET_RACE_DELAY_US` | 样本 | 结果 |
|---|---|---|---|
| 1 | 200 | 5 轮 × 8 端口 = 40 | **40/40 PASS，0 FAIL** |
| 2 | 2000 | 4 轮 × 8 端口 = 32 | **32/32 PASS，0 FAIL** |
| 3（不放大） | 0 | 6 × `make smoke-net-tcp-lanes` | **6/6 PASS** |

三组共 78 次连接，失败 0 次。每轮末尾 `/proc/net/stats` 均为
`lwip_lock: armed=1 owner=4294967295 violations=0 sites=0`
（`owner=4294967295` 即 `CPU_NONE`，说明采样时锁是空闲的——`sites=0` 表示探针
一次都没触发，**没有出现过 arm 后非持有者调用**），且
`net_accept_drop=0`、`net_bh_overflow=0`、`net_alloc_fail=0`。
`CONFIG_NET_PCB_SANE=1` 全程 0 命中，即链表桶不变量也没被破坏。

**这组数据能说明什么、不能说明什么**：

- **不能**说"2% 的 flake 已经消失"。72 次放大样本下按 2% 计期望失败 1.6 次，
  观测 0 次的概率约 `0.98^72 ≈ 23%`——不构成任何"已修复"的证据。样本量根本不够。
- **能**说的是：在"accept stage 出队后、取 lwip 锁前"这个被指认为最可疑的窗口里，
  把窗口放大到 200 µs（相对 `a20_lwip_lock()` 的一次获取是数千倍）**没有**让间歇故障
  变必然。配合 `sites=0`，**配方第 2 步的前提在这段窗口上不成立**。
- 附带排除：放大到 2000 µs 仍 0 失败，说明该窗口内就算确实有竞态，其参与方也不在
  这条路径上，或者它的触发条件与本放大器无关。

**顺带记一个本轮才出现的观测**：`tcp_active` 在长跑中从 48 爬到 64。这是第 2 项
修复把 server 侧 `close()` 从 `tcp_abort()`（RST，立即回收）改成 `tcp_close()`
（FIN）之后必然出现的 TIME_WAIT 累积，不是新 bug，但会影响任何按 `tcp_active`
判断"还有余量"的解读。已确认它有界：dev profile 的 `MEMP_NUM_TCP_PCB = 8192`，
且 `tcp_alloc()`（`tcp.c:1953` 起）在池压力下会回收最老的 TIME_WAIT pcb。

### 放大实验第二轮（2026-10-06）：补样本 + 放大器归零，**仍未复现**

第一轮（2026-10-05）留下的最大缺口不是"嫌疑区选错了"，而是**样本量**：
72 次阴性在 2% 下有 23% 的概率纯属偶然。第二轮按同一配方再跑两轮，
两轮的差别在**参数组合**上，不在位置上：

| 轮次 | `CONFIG_NET_RACE_DELAY_US` | 引导 | 下发样本 | **实际执行** | 失败 |
|---|---|---|---|---|---|
| A | **500**（第一轮用过 200 / 2000） | 4 CPU / 4 lane | 400 | **398** | **0** |
| B | **0**（放大器关闭） | 4 CPU / 4 lane | 400 | **399** | **0** |

两轮都是 `make ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=4 NET_LANES=4
OPT="-DCONFIG_NET_PCB_SANE=1 -DCONFIG_NET_LOCK_ASSERT=1 -DCONFIG_NET_RACE_DELAY_US=<D>"
dev-build`，guest 参数带 `a20.tcpmode=lwip`，每引导 5 遍 8 端口集、每端口间隔 1 s，
10 次独立引导，每轮末尾落盘 `/proc/net/status`、`/proc/net/config`、
`/proc/a20/lock_contention`。

**合计 797 次实际执行、0 次失败。** 按 2% 计，797 次 0 失败的概率约
`0.98^797 ≈ 1.1e-7`。

**能说与不能说的**：

- **仍然不能说"已修"**。本轮**没有为这条 flake 改过任何一行实现**，唯一的代码改动
  是把 `server()` 的 `close()` 从 `tcp_abort()` 改成 `tcp_close()` 之外的历史修复，
  与 `connect()` 不完成无关。"没复现"是观测，不是结论。
- **能说的是**：在**当前树**上、4 lane + 4 CPU + `tcpmode=lwip` 这个配置下，
  2% 的失败率**不再出现**；第一轮那份"2%"很可能来自更早的脏树。按下一步第 1 条
  自己写的判据（"若 200 次 0 失败，去查历史样本的构建号"），该判据现在**已满足**。
- 轮次 A 的 500 µs **不比第一轮的 2000 µs 更极端**，所以它新增的不是"放大强度"
  而是"同一个放大强度下的样本量"。不要把 A 读成"把窗口拉得更开也没炸"。

**20 次引导里探针一次没响**（每轮末尾逐次核对）：

```
lwip_lock: armed=1 owner=4294967295 violations=0 sites=0
net_lock:  armed=1 violations=0 sites=0 held_cpu0=0 lockcounters_short=0
lock_counters: registered=48 capacity=192 dropped=0
net_notconn: total=0 window=0 reasons=13
lanes: count=4 sockets=1 occupancy: 0 0 1 0
```

20 次引导均无 panic / page fault / `tcp_pcbs_sane` 命中。`net_lock:` 这一行是
`2dd28758c` 新加的 net 锁契约探针（`CONFIG_NET_LOCK_ASSERT=1`，违规即 abort）；
它在 4 CPU 下 800 次真实握手全程 `violations=0`，即**阶段 E 的锁序在本负载下没被违反**。

上面那行 `lock_counters: registered=48 capacity=192 dropped=0` 是 **DEFAULT 档**的。
**server 档**（`NET_PROFILE=3` → `NET_PROFILE_MAX_SOCKETS=65536` →
`NET_SOCK_BUCKET_SHIFT=9` → 128 个桶锁）另测过一次：
`registered=141 capacity=192 dropped=0`，即 128 个桶锁 + 13 个其他锁，**余量 51**，
`lockcounters_short=0`。所以 `LOCK_COUNTERS_MAX = 192` 不是"勉强够"，是三档都够——
而这个"够"现在每次引导都会由 `net_socket_registry_init()` 自己报出来
（`kernel/net/socket_registry.c:109-122`），不再依赖有没有人记得去看。原始读数见
`impl-notes-net.md` §11.4 补记。

**引用计数账本顺带补上了多核数据**（`impl-notes-net.md` §11.4 自记缺的那一块）：
20 次引导的 `net_sock_ref` 全部 `live=1 faults=0`，`allocs` 在 159–167 之间，
`frees` 恒等于 `allocs-1`。这是"4 CPU + 4 lane 真实握手下引用计数不漏"的证据，
单靠 §11.4 那个单 CPU 读数是拿不到的。

#### 本轮的一个**测量方法**发现：控制台输入会吃掉字符

3 次下发里有 3 条命令名在 guest 侧被**相邻字符换位或丢失**：

```
# tcp_accept_test 12407   ->  # tcp_aceptc_test 12407
E: mksh: tcp_aceptc_test: inaccessible or not found
# tcp_accept_test 12402   ->  # tpc_accept_test 12402
E: mksh: tpc_accept_test: inaccessible or not found
# tcp_accept_test 12401   ->  # ctp_accept_test 12401
E: mksh: ctp_accept_test: inaccessible or not found
```

三处都在命令名前三个字符内，都是**换位/丢失**而不是丢字节串；输入速率是每 1 s 一行，
远低于任何 overrun 阈值；宿主侧那 800 条命令是 shell `echo` 逐条写出的，字节本身正确。
所以这是 **guest 串口接收路径**上的偶发字符错位（UART RX / 行规程），与 TCP 无关——
它发生在命令**还没执行**的时候。

**对这条 flake  hunt 的直接后果，必须写清楚**：光靠
`grep -c 'TCP_ACCEPT_TEST: PASS'` 统计样本会把"没执行的"算成"没失败的"。
`smoke-net-tcp-lanes` 判的是 `passes -eq 8`，若 8 条里有 1 条被吃掉，
它同样只看到 7 条 PASS 并判 FAIL——**方向恰好相反**：门禁会误报一次失败，
而手写的统计会把 3/800 的测量损耗悄悄吃掉。上面表里"下发"与"实际执行"分列两栏，
就是为了不让后者被前者冒充。

### 下一步（配方被否定后该换哪一侧）

按上文"如果加了延时仍然不炸，说明嫌疑区选错了"的约定，**换生产者一侧**。
具体按这个顺序，因为成本递增：

1. ~~**先补样本，而不是先换位置。**~~ **2026-10-06 已执行**：两轮合计 **797 次实际
   执行、0 失败**，其中一轮放大器为 0（正是这条要求的配置）。上面那句"若 200 次
   0 失败"的判据**已满足**——按它自己写的结论，接下来该做的是**查历史样本
   （`12404` / `12405` 两次失败）的构建号，而不是继续在当前树上找竞态**。
   - 若在**当前树**上再出现失败：那才是唯一值得带走的线索，立刻把该轮的完整
     `/proc/net/status`、`/proc/net/config`、各 lane 计数和 console 全量存盘。
2. **修测量方法，否则下一轮还会被同一个坑绊倒。** 本轮发现 guest 串口接收会偶发
   吃掉命令名里的相邻字符（3/800，见上），后果是 `grep -c PASS` 把"没执行"算成
   "没失败"。任何统计这条 flake 的脚本都应当**同时**核对三件事：下发的命令回显数、
   mksh 的 `inaccessible or not found` 数、以及 PASS/FAIL 行数，三者对不上就报
   "样本受损"而不是报 0 失败。这条排在第 2 位不是因为它更重要，而是因为它**便宜**
   且不做的话第 3、4 条的实验数据同样不可信。
   **本轮未实施**：只做了记录，定位与修复会落到
   `kernel/arch/riscv64/platform/timer.c` 与 `kernel/drivers/char/uart.c`，不属于网络
   这条流的范围。本轮的做法是在统计口径上分列"下发"与"实际执行"两栏（宁可报
   样本受损也不报 0 失败），但那只是**回避**这个问题，不是修它。
3. **换放大器位置到生产者 `lwip_tcp_accept_cb()`。** 现有放大器只加在消费者
   （`net_inet_accept_stage_drain`）一侧。配方只覆盖了"跨两个锁域"这一半；
   另一半是生产者把 pcb 塞进 stage ring 的时刻，那里同样在 lwIP 锁下写、
   在桶锁外读。若要做，用一个新开关（**不要**复用 `CONFIG_NET_RACE_DELAY_US`，
   否则两组实验的数据无法分开看）。**注意**：这一条现在是在"当前树已 797 次不复现"
   的前提下做的——它值得做的理由是排除假设，不是预期还能复现。
   **本轮未实施**：它按本文是成本递增的第 3 步，在"当前树 797 次不复现"的前提下
   本轮的价值主要是排除假设，按时间盒留给下一轮。
4. **在 `netif_loop_output()` / `netif_poll()` 一侧再看一次。** 回环队列的无界性
   （上文已记 `LWIP_LOOPBACK_MAX_PBUFS=0`）意味着"哪一 CPU 在哪一刻排空它"完全
   不受控；`net_tcp_lane_input()` 把包投给哪条 lane 的时机也就跟着漂。这不是可静态
   证明的东西，只能靠把"入队时刻"和"排空时刻"打上 CPU id + 时间戳对照来缩小范围。
5. 仍然定位不到时，**把 TCP 层的连接建立时序全量打点**（SYN 入、pcb 分配、
   桶查找、SYN-ACK 出、SYN-ACK 重传、accept 入 stage、accept 出 stage），
   逐条打 lane / CPU / 时间戳，然后**用失败样本比对**。这是最后手段，因为它很慢。

> 不要为了"能变红"而降低门禁或改弱 `smoke-net-tcp-lanes`。三轮共 875 次下发全绿
> **不是**把门禁改绿的结果——门禁配置全程未动，`CONFIG_NET_PCB_SANE=1` 与
> `CONFIG_NET_LOCK_ASSERT=1` 只会让失败更容易被看见，不会更容易被隐藏。

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

### 阶段 D 落地记录：IRQ 只按 lane 入队，处理在进程上下文（`f6f327b96`）

落地的是**分发与出中断上下文**，不是 per-lane 锁。`g_lwip_lock` 仍然是一把全局锁，
所以「各 CPU 各自处理自己的 socket」今天成立在"谁去处理"这一层，不成立在
"谁能同时处理"这一层。这条必须写在最前面，否则下一个人会按名字把阶段 D 读大。

**IRQ 侧**（`a20_lwip_process_netif_irq_locked()`，`lwip_stack.c:1242`）：解析刚好够
定 lane 的头部，把帧拷进该 lane 的接收队列，返回。排空仍然是串行的短锁 +
`CONFIG_NET_RX_IRQ_BUDGET`。

包的归属 lane 取**目的地址与目的端口**，不是对端源端口。这是从代码里读出来的，不是
选的：`tcp_in.c` 用 `NET_PCB_LANE_OF(ip_current_dest_addr(), tcphdr->dest)`，
`udp.c` 用 `NET_PCB_LANE_OF(ip_current_dest_addr(), dest)`。目的地址加目的端口就是
这条连接自己的本地四元组，已建立连接、UDP 与被动开出的子连接因此落在同一条 lane 上。

**处理侧**：`a20_lwip_lane_drain_locked()`（`lwip_stack.c:1043`）取 lane 的消费权
（`net_lane_rx_claim()`，`net_lane.h:318`），拿 `g_lwip_lock`，
`net_lane_ctx_push(lane)`，然后逐帧 `pbuf_alloc` + `pbuf_take` + `n->input()`。
驱动遍历的入口是 `a20_lwip_lane_drain_all()`（`lwip_stack.c:1083`）。

#### 三处必须记住的坑

1. **不要简单地让 IRQ 只入队——死锁是真的，但解法不是"更聪明的门控"。**
   解法是**无条件会跑到的 poll 点**。`kernel/core/progress.c:150` 在
   `kernel_progress_run_bottom_halves()` 里调用 `A20_LWIP_LANE_RX_POLL()`，而
   `sched()` 每次调度决策、每次会引发重新调度的时钟 tick、每次 idle pass 都走到它。
   放在"读者醒来之后"就晚了：阻塞读由 socket bottom-half 唤醒，bottom-half 需要暂存的
   帧已经被处理掉，而只在唤醒后才跑的 poll 在等一个不会到来的事件。门控用的量是
   生产者自己抬起来的"已暂存帧数"计数器（`net_lane_rx_queued_total()`，
   `net_lane.h:339`；poll 点在 `lwip_stack.c:1412` 判空），它只可能假阳，假阳的代价是
   一次 relaxed load。位置在 `kernel_progress_net_rx()` **之前**而不是之后：后者是
   CPU 0 排设备 ring 的地方，多 lane 下它排出来的帧只是暂存，放在它之后要等下一趟。
2. **claim 用原子标志，不是 `spinlock_t`。** `core/lock.h` 只有
   `spin_trylock_irqsave`，用 spinlock 意味着拿着 claim 的整个协议处理过程都在关中断
   的状态下跑——而那正是这个阶段要搬出中断的原因。所以 claim 期间中断是开的，这既
   安全（没有任何中断路径会去拿 claim）也是必需的。一条 lane 同时只有一个消费者是
   **正确性要求**：两条 CPU 排同一条 lane 会打乱一条流式 socket 的段序，也会交错重组。
   被抢占的持有者只是延迟问题，不是正确性问题。
3. **IP 分片必须只按地址散列。** 非首片没有传输层头，从里面读"端口"读到的是负载，
   会把一个数据报的分片拆到不同 CPU 上。判据用 `MF || frag_offset != 0`：被拆包的所有
   分片都满足，没拆的一个都不满足。IPv6 那边片头本身就是扩展头，而本移植不遍历扩展头，
   所以 v6 分片自动落到"只按地址"这一支。

另外两处不是可选的：netfilter 必须跑在**算 lane 之前**（DNAT 原地改写目的地址，
lane 得跟着改写后的元组走）；回环**不入队**（它本来就已经被摘链并在原地处理了）。

#### `CONFIG_NET_LANES=1` 等价：实测，以及第三个坑

方法与阶段 C 相同：`git worktree add` 拉出改动前的 `fe03aac27`，两边**同一条命令行**
编 `lwip_stack.c`、`net_lane.c`、`socket_inet.c`、`progress.c`、`udp.c`、`tcp.c`、
`tcp_in.c`、`tcp_out.c`，逐 section 比对。

- `-fno-sanitize=undefined`：`.text` / `.rodata` / `.data` / `.sdata` / `.srodata`
  **全部逐字节相同**。
- 开着 UBSan：`.text` / `.rodata` / `.sdata` / `.srodata` 相同，只有 `.data` 差。
  已定位：那 11 个字节是 UBSan 内嵌的 `SourceLocation` 行号表，差的数值**恰好是
  `pcb_lane.h` 里多出的 19 行注释造成的偏移**；把 `pcb_lane.h` 换回改动前的版本，
  差异归零（换 `net_lane.h` / `net_profile.h` 则不归零，所以责任人是前者）。
  这些 TU 的 `.data` 里**一个符号都没有**，且关掉 UBSan 后整段缩为 0 字节。

**第三个坑：out-of-line 的空函数仍然是调用。** poll 点最初写成普通函数
`a20_lwip_lane_rx_poll()`，一 lane 下函数体是 `(void)budget;`。实测
`progress.c` 的 `.text` 多了 10 字节——多出来的是一次 `call`。
这与阶段 C 的第 1、2 条同源：**"优化器会把空函数折掉"不是等价证明**，能 `grep`
出来的事实才算。改成一 lane 下展开为 `((void)(budget))` 的宏
`A20_LWIP_LANE_RX_POLL()` 之后，调用点从预处理结果里消失。头文件里三处都按这个写法。

#### 阶段 D 还剩下的（不要当成已完成）

- **`g_lwip_lock` 仍然是一把全局锁。** lane 决定的是"谁处理"，不是"谁能并行处理"。
  真正分片要改的是锁的粒度与所有权（谁持有、由谁释放、跨 lane 的 PCB 冷路径怎么不
  死锁），那是一次锁契约的改动，不是本阶段带得上的。
- **回环不入队。** `LWIP_LOOPBACK_MAX_PBUFS=0` 的无界队列问题照旧（见上文
  「`netif_poll()` 排空侧读到这里（未完成）」），所以同机 socket 之间的往返仍然是
  单条 lane 上的串行工作。
- **广播 UDP 仍然无法定向**，见「广播 UDP 是唯一无法按桶定向的用例」。
- **队列深度是 4 的静态槽位环**（`NET_PROFILE_LANE_RXQ_SLOTS`），满了就丢并计数——
  队列没法把压力推回网卡上，就必须丢掉点什么。丢包率是否可接受要等真机。

#### 本阶段新增的观测

`/proc/net/status` 在多 lane 下多一段 per-lane 的 `rx lane / rx / drop`，以及一行
`rx staged (not yet processed)`。per-lane 给的是**单调计数**而不是水位，与阶段 C 的
pbuf 计数同一个理由：读水位要么得拿 claim（`/proc` 与收包路径抢），要么读一个在跨 CPU
下没有意义的 head-tail 差值。`rx staged` 那行才是当下有多少帧在等，那个量是单值的。

## 阶段 E：per-socket 锁

阶段 C/D 把 socket 状态按 lane 分了片，但**保护它们的锁没有跟着变**：一个 socket 的
接收队列、accept 队列和连接状态一直由"拥有它那个 registry slot 的桶锁"保护，而桶在
server profile 上是 512 个 slot（`NET_SOCK_BUCKET_SHIFT = 9`，socket_internal.h:516）。
一个 socket 的 recv 因此要和同桶另外 511 个 socket 互斥——lane 分得再细也没用，因为
所有 lane 的 hot socket 大概率落在同一个桶里（桶号来自 slot 分配顺序，不来自 lane）。

阶段 E 做的就是把粒度从桶降到 socket。落地在 `7c7a4d7c8`，锁契约写在
[network-lock-contract.md](./network-lock-contract.md) 的「锁」一节。

### 落地记录

`net_socket_t` 内嵌一把 `spinlock_t lock`（`socket_internal.h:405`）。struct 里除
`g_sockets[]` 的槽位本身之外的一切——队列、`closed`、`connected`、wait 队列、pending
计数——只由这把锁保护，别无其他。桶锁 `net_bucket[b]` 此后**只**管 slot 表和每桶空闲
位图，调用点收敛到三个：`net_register_socket_locked()`、`net_socket_unregister()`、
`net_bucket_slot_ref()`（`socket_internal.h:724`）。

锁序（外到内）：`net_bucket[b]` → `net_socket_t.lock`。桶锁**绝不在** socket 锁之下
取，这就是现在全部的 ABBA 面。两把 socket 锁走 `net_sock_lock2()`（`socket_internal.h:584`），
按 socket 指针升序；`b == NULL` 表示只取一把（peer 为空是常态，不必凑合）。原来的
`net_bucket_lock2()` 连同 `NET_SOCK_ORPHAN_BUCKET`、`NET_SOCK_BUCKET_COUNT` 一起删除
（`grep -rn net_bucket_lock2 kernel/` 当前零命中）——孤儿桶存在的唯一理由是"没有 slot
的 socket 没有桶，而它的状态当时靠桶锁保护"，现在它的状态有自己的锁，不再需要额外分片。

### 三处值得记下来的

**销毁必须拆两段。** `net_socket_unregister()` 要取桶锁，因此不能运行在 socket 锁之下。
`close()` 相应改成：socket 锁内标 `closed`、摘队列、把 wake 收进 `proc_wake_q_t`，然后
解锁，才 unregister + free。锁内绝不等引用归零。这顺手修掉两个既有 bug：
`net_unregister_socket_locked(child)` 曾经在 listener 的桶锁下、以及在 `(s, listener)`
有序对下被调用，两次都写 `g_net_buckets[child_bucket].free_bits`——那把桶锁并不在手上。

**带出指针先 ref。** 桶锁不再能当"对象活着"的凭据（跨桶查找没法一直举着第一把桶锁），
所以从任一临界区带出 `net_socket_t *` 都要 `net_socket_ref()`、用完在锁外 drop。采样
peer 指针的写法因此是"在 `s` 自己的锁下 `net_socket_ref(peer)`"，把采样和取引用放在
同一个临界区，避免中间的 UAF 窗口；`s` 自己的答案（connected / nonblock / timeout /
peer_addr）则在锁内拷到栈上，因为真正入队发生在解锁之后。

**`net_bucket_scan()` 是唯一允许"桶锁里嵌 socket 锁"的地方**，且只用于只读遍历。
`kernel/fs/procfs/procfs_render.c:527` 的 visitor 直接读 `s->reg_idx`，把 socket 锁嵌进
`net_bucket_scan()` 内部就让那类只读回调自动被覆盖，而不需要去动 procfs。

顺带改了 `net_inet_bottom_half_process_all()` 的遍历形状：原来按桶走、每桶整段持桶锁，
正是锁规则禁掉的"取整组"（见 VFS dcache 的活锁记录）。现在改为无锁读
`g_net_bh_pending[]` 位图 + `net_bucket_slot_ref(i)` 逐个取引用，再在那个 socket 自己的
锁下处理事件。

### 这一步没有解决的

`g_lwip_lock` 仍然是一把全局锁，阶段 D 的"各 CPU 各自处理自己的 socket"依然只成立在
"谁处理"这一层。阶段 E 去掉的是 socket 侧的伪共享，没动协议栈侧的全局串行——那要等
把 lwIP 核心状态按 lane 分片，是另一份契约。

**运行期强制这一侧仍然空着。** 阶段 C/D/E 三步都只改了代码，没有给 net 锁加任何探针：
没有"当前 CPU 是否持有期望的 socket 锁"的判据，`net_sock_lock2()` 的地址升序约定
只有代码评审在把关，lane claim 一侧同样没有。`LWIP_ASSERT_CORE_LOCKED()` 只覆盖
`g_lwip_lock`（见上文「断言已接线」）。要补需要 per-CPU 持锁集合跟踪，本轮未做，
形态记在 [network-lock-contract.md](network-lock-contract.md) 的迁移检查清单末尾。

**验证缺口仍在。** 合并后没有跑全量回归，ASAN + SMP 压测未做。`2f17a5ba8` 的提交
说明自陈的三项运行期验证缺口——引用计数不漏不重、`LOCK_COUNTERS_MAX` 注册预算、
`-ENOTCONN` 窗口——**仍然开放**，清单在 `docs/measured/impl-notes-net.md`。
阶段 C/D/E 自身只做了各自的编译期与逐字节等价自检；按本轮 lane 阶段的指令，
`smoke-*` 门禁没有跑。

## 必须保持全局的部分

| 对象 | 处理 | 理由 |
|---|---|---|
| ARP / etharp 缓存 | seqlock | 每包都要读，必须无锁；几乎不写 |
| 路由 / netif 列表 | seqlock | 同上 |
| socket registry | 分片哈希 + 引用计数，桶锁只管 slot 表（阶段 E） | 冷路径 |
| bind/listen/accept/close 等 PCB 冷路径 | 每 socket 一把 `net_socket_t.lock`（阶段 E） | 频率是连接率级，但没必要为低频付整桶的伪共享 |
| conntrack 表 `g_ct[]` | 保持全局，`g_lwip_lock` 保护 | 五元组与 lane 无关，且 NAT 绑定是**流级**的：把表按 lane 切开，一条跨 lane 的流会分裂成两条互不知情的记录，回程方向就找不到入口 |

conntrack 那行是本表里唯一"因为语义而不能分桶"的条目。NAT 规则表反而可以按 lane 切
（规则匹配的是报文头，不依赖流状态），但 conntrack 不行——理由写在
[network-lock-contract.md](./network-lock-contract.md) 的 "conntrack 与 NAT 用哪把锁" 一节。

## 观测

`/proc/net/status` 的 `lanes:` 行给出 lane 数与每 lane 的 socket 数。它不采信
吞吐数字，只回答"lane 是否真的把连接分散开了"：

```
lanes: count=4 sockets=8 occupancy: 3 2 2 1
```

`CONFIG_NET_BUSY_POLL` 与 lane 数是正交的：单 lane 时轮询就是今天的行为。

多 lane 构建下，`/proc/net` 的 memp 表下方还会多出一段 per-lane 的 pbuf 分配/释放
计数（`5ea06a78`）。它回答的是"分配点是不是真的按 lane 分开了"，**不是**每条 lane
占了多少内存——见上面「阶段 C 落地记录」里为什么内存并没有被分片、为什么给的是两个
单调计数而不是一个水位。

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