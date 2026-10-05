# conntrack 与 NAT

在既有的最小 netfilter（`kernel/net/netfilter.c`）之上加了一层流状态和一层地址转换。
这一节写它**是什么**、**边界在哪**，边界那部分写得比功能部分长，因为能撞上的坑都在
那边。头文件 `kernel/include/net/netfilter.h` 顶部有一份等价的 `HONEST BOUNDARIES`，
两处若不一致以头文件为准，并回来改这里。

## 位置：为什么能就地改包

两个 hook 挂在 **lwIP 之下**、以太帧上：

- input：`kernel/net/lwip_stack.c:542`，`netfilter_input()` 在 `pbuf_take()` 之前；
- output：`kernel/net/lwip_stack.c:232`，`netfilter_output()` 在帧交给网卡之前。

所以一次转换就是**原地改写调用方正在持有的帧缓冲**加一次校验和修正，不需要重组包、不
需要分配。这是本实现能做 NAT 的唯一原因，也是它做不了更多事情的原因。

## conntrack

五元组（src、dst、sport、dport、proto）做 FNV-1a 哈希，桶数由
`net_profile.h` 的 `NET_PROFILE_CONNTRACK_BUCKETS` 给。哈希**刻意不对称**：一个元组和
它的反方向落进不同的桶，`netfilter_ct_find()` 用两次探测补偿。让哈希对称可以省掉回程
的一次探测，代价是每个包都要先交换元组——不划算。

| 档位 | entries | buckets | 静态占用 |
|---|---|---|---|
| EMBEDDED | 64 | 8 | 4 KiB |
| DEFAULT | 256 | 32 | 16 KiB |
| SERVER | 1024 | 128 | 64 KiB |

每条 64 字节，由 `_Static_assert` 钉住（`netfilter_nat.c`），将来加字段撑破档位预算是
编译错误，不是静悄悄变大的 `.bss`。

**每条目挂两条链**，不是一条：正向链按原始元组索引，回程链按**转换后**的元组索引。
这不是冗余。NAT 之后线上跑的是转换后的元组，而条目里存的是转换前的；回程包带着转换后
的地址到达，按原始元组查表必然查不到。加第二条链是为了让回程一次哈希命中，而不是线性
扫全表或把两条元组都塞进条目。只挂一条链的失败模式是**静默**的：正向翻译正常，回程查不
到条目，原样发出，对端收到一个不认识的源地址——没有任何计数器会动。

**容量与超时。** 表满时退化成有界 LRU 缓存而不是失败：路由器留住新流、忘掉最老的流，对
这么小的表是合适的取舍。清扫在 `a20_lwip_poll_timers_locked()` 里每秒一次，每次只扫
`max_scan` 条并从上次的断点续扫（`netfilter_conntrack_expire`），所以满表也不会让某个
tick 无界。空闲超时按协议和状态分开：TCP NEW 30s、TCP ESTABLISHED 120s、UDP 30s。分开
的理由是半开 TCP 流不能占着槽位等一个已建立连接的寿命，而 UDP 的"流"常常就是单个数据
报，对端不会再回。

LRU 的键是 `last_ms`，没有独立的 LRU 链表：满表插入时线性扫一遍取最小值，同 tick 插入的
两条按槽位下标决胜。**这条路径的边界**：淘汰是全局最老，不是"最老的未完成流"，所以一条
正在握手的 TCP 流和一个空闲的 UDP 报在竞争同一个受害者位置；被淘汰的流如果还有包到达，会
按新流重新建条目并**重新翻译一次**——如果期间规则变了，翻译结果可能与之前不同。

超时的边界同样要说清楚：清扫只在 `a20_lwip_poll_timers_locked()` 走到时发生，也就是每秒
至多一次、每次 32 个槽位。一张 1024 条的满表因此最多需要 32 秒才被完整看过一遍；条目在
被看到之前不会被回收，`ct_tracked` 会在这段时间里显示为满。

`ct_sweeps`（`/proc/a20/netfilter`）统计清扫**运行过**的次数，和 `ct_timeout`（清扫回收了
多少条）分开，因为"没有条目空闲"和"根本没人看过"在别的计数器上长得一样。`ct_lru_victim`
是下一次满表插入会被淘汰的条目的源端口——计数器说不出"满表会忘掉哪条流"，而这正是排障时
真正要问的问题。

**状态推断只有元组和 TCP flags 字节。** 见到回程包，或前向包带 ACK 不带 SYN，就是
ESTABLISHED。没有序列号窗口校验，没有 RST/FIN 拆除（关掉的流会挂到空闲超时），也没有独立
于表容量的半开连接上限。

## NAT

SNAT / MASQUERADE 在 output hook，DNAT 在 input hook，规则由 /proc 管理，语法与既有
drop/accept 规则同族：

```
natadd in  proto=tcp dport=18081 action=dnat to=10.0.2.15 toport=18082
natadd out proto=tcp dport=80    action=masquerade
```

命名 `snat` 或 `dnat` 却不给 `to=` 的规则在**解析期**就被拒，而不是变成一条匹配得上、
什么都不做的规则。

**翻译结果记在 conntrack 条目里，后续包从条目翻译，不重新匹配规则。** 一条 NAT 规则可
能已经在流建立之后被删掉、改序或替换；重新求值要么弄坏回程，要么悄悄挪走一个正在用的
绑定。

"后续包"包含与首包**同方向**的那些，这不是假想：重传的 SYN 是以正向 conntrack 命中的形式
到达的，只翻回程方向的话，这个重传会被送到一个没人监听的端口，lwIP 回 RST，对端的连接
死于一个对端自己没做错的包。首次端到端 DNAT 跑挂的正是这个——第一个 SYN 翻了，第二个没
翻然后吃了 RST。

校验和按 RFC 1624 增量更新 IP 头校验和与 L4 伪首部。**不校验**收到的校验和：带着坏校验
和进来的帧，出去时还是坏的。

## 诚实边界

以下都是**本实现的限制**，不是设计的限制，每一条都是调用方可能踩到的：

- **没有 ALG。** 无 FTP/SIP/ISAKMP 载荷检查，地址写在报文体里的协议只翻头部，控制通道
  不会跟着走。
- **没有 ICMP 跟踪。** 只有 TCP 和 UDP 建条目（`netfilter_conntrack_process` 的第一条
  proto 判断），所以 ICMP 有三重缺失，且都不是"差一点"：
  1. **ICMP echo 不建条目**，因此不复用 NAT 绑定；一条 `masquerade` 规则下的 ping 不会被
     翻译，按原地址出去。
  2. **ICMP 差错报文（type 3 / 11 / 12）不被跟踪**，因此不会终结它内嵌报文所属的那条流。
     这直接关掉了路径 MTU 发现：发往被 NAT 改写过地址的对端，其 ICMP too-big 回不来被认领，
     发端继续用大包，被静默丢弃。
  3. **ICMP 报文本身不被翻译**。按 RFC 5508 NAT 应当改写内嵌报文的五元组（并重算 ICMP
     校验和），这里不改，于是收到 NAT 改写过源地址的对端在差错报文里看到的是翻译后的地址，
     内层校验和对不上，通常整条报文被丢。
- **不分片 NAT。** 无分片包和首片会翻；非首片没有端口，既无元组可索引也无头部可改，直接
  放行——选择放行而不是"记条目但不翻译"，因为后者会让条目看起来有状态而实际转换是残的。
  被分片的流因此可能被静默弄坏。
- **无 helper 模块**，无 fullcone / 端口保持变体，无 IPv6 NAT（hook 只解析 IPv4）。
- **不跟踪非 TCP/UDP 协议**，即使 NAT 规则里写了 `proto=`。
- **表是定长的**。SERVER 档 1024 条，满即 LRU 淘汰（`ct_evicted` 计数），不是拒绝新流。
  淘汰的是全局最老条目，边界见上文"容量与超时"。
- **没有动态端口分配。** `masquerade` 不带 `toport=` 时保留客户端原端口，多个同源端口的
  并发连接不会被打散；只有显式写 `toport=` 才会改端口，而那是所有走这条规则的流共用的一个
  固定值，不是分配器。

## 门禁

| 门禁 | 断言 |
|---|---|
| `smoke-netfilter` | 既有 drop/accept 语义不回归，且 conntrack 计数随 `udp_send_once` 增长、`/proc` 里能看到该五元组（`user/cmds/net/netfilter_test.c` 测试 8） |
| `smoke-netfilter-nat` | 端到端 DNAT 端口转发：QEMU `hostfwd` 把 `127.0.0.1:18081` 转到 guest 18081，guest 侧规则转到 18082，host 侧 `tools/netnat_host_probe.py` 连上、发出、收到回显 |
| `smoke-ct-capacity` | 表填到 `ct_capacity` 恰好停住；多一条流恰好淘汰一条，且**淘汰的是最久未用的那一条**（淘汰前后的 `ct_lru_victim` 各断言一次）；短超时（`cttimeout 100 100 100`）下条目在截止时间内被回收、`ct_timeout` +1 且 `ct_sweeps` 前进 |
| `test-nat-rewrite`（在 `make check` 内） | 主机侧编译**真实的** `kernel/net/netfilter_rewrite.c`：SNAT/MASQUERADE/DNAT 地址改写后 IP 与 L4 校验和都用独立的一次性重算校验；端口改写与 `toport=0` 语义；masquerade 取的是"正在离开的那块接口"的地址；回程元组的推导，并与对端实际会发出的那个帧逐字段比对（`tools/test-nat-rewrite-host.c`） |

SNAT/MASQUERADE **没有**端到端门禁，这是拓扑决定的而不是遗漏：QEMU user-net 自己在宿主
侧做 NAT，guest 外面没有第二个对端，回程包不存在，所以 `smoke-netfilter-nat` 只能覆盖
DNAT。上表最后一行是这个方向上能拿到的最强证据——它跑的是出货源码，不是副本，但仍然是
主机侧单元测试，不能替代真机上的端到端验证。

两个测试钩子是为了让上面的容量/超时断言不必靠 256 条真实流和 30 秒等待达成：
`ctinject`（经由数据面同一个 `netfilter_ct_insert` 插入合成流）和 `cttimeout`（运行期覆盖
三个毫秒常量，0 恢复默认）。两者都只是 `/proc` 动词，不新增内核状态。

DNAT 门禁有两个 QEMU 10 的坑，都写在 `tools/targets-smoke.mk` 的目标注释里：规则必须
带 `hostfwd=` 前缀（裸短格式被当成已废弃的布尔量），guest 地址前**不能**有冒号（否则
guest 段被解析成 `[addr]:port`，端口取到 10）。另外门禁必须用 `a20.tcpmode=lwip` 启动：
默认的 `fast` 模式不建 lwIP LISTEN pcb，入站 SYN 只会得到 RST（`kernel/net/socket_control.c:204`）。

`user/cmds/net/netnat_test.c` 里的阶段划分是有意的：先在没有 NAT 的情况下验证 conntrack
本身工作正常，再验证规则解析的拒绝路径，再装规则，最后用一次**注定失败**的直连做阴性对
照，确认转发是规则造成的而不是环境本来就能通。
