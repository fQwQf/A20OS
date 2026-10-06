# conntrack 与 NAT

在既有的最小 netfilter（`kernel/net/netfilter.c`）之上加了一层流状态和一层地址转换。
这一节写它**是什么**、**边界在哪**，边界那部分写得比功能部分长，因为能撞上的坑都在
那边。头文件 `kernel/include/net/netfilter.h` 顶部有一份等价的 `HONEST BOUNDARIES`，
两处若不一致以头文件为准，并回来改这里。

## 位置：为什么能就地改包

两个 hook 挂在 **lwIP 之下**、以太帧上：

- input：`kernel/net/lwip_stack.c:969`（多 lane 构建，enqueue 之后、算 lane 之前）与
  `:1188`（单 pbuf 路径，`pbuf_alloc()` 之前），`netfilter_input()` 都在从
  `rx_frame` 解析之前；
- output：`kernel/net/lwip_stack.c:391`（TX_SG 快路径）与 `:405`（普通路径），
  `netfilter_output()` 在帧交给网卡之前。

两处 input 的先后顺序是**承重的**，源码里就地写了理由：DNAT 原地改写目的地址，
hook 跑在 lane 判定或 `pbuf_take()` 之后的话，下游读到的仍是翻译前的地址，规则会
报匹配而连接永远建不起来。

所以一次转换就是**原地改写调用方正在持有的帧缓冲**加一次校验和修正，不需要重组包、不
需要分配。这是本实现能做 NAT 的唯一原因，也是它做不了更多事情的原因。

## conntrack

五元组（src、dst、sport、dport、proto）做 FNV-1a 哈希，桶数由
`net_profile.h` 的 `NET_PROFILE_CONNTRACK_BUCKETS` 给。哈希**刻意不对称**：一个元组和
它的反方向落进不同的桶，`netfilter_ct_find()` 用两次探测补偿。让哈希对称可以省掉回程
的一次探测，代价是每个包都要先交换元组——不划算。

| 档位 | entries | buckets | 静态占用 |
|---|---|---|---|
| EMBEDDED | 32 | 8 | 2 KiB |
| DEFAULT | 256 | 32 | 16 KiB |
| SERVER | 1024 | 128 | 64 KiB |

entries 逐档取自 `net_profile.h:177`（EMBEDDED）、`:455`（DEFAULT）、`:404`（SERVER）。
**EMBEDDED 是 32，不是早先文档写的 64**：`net_profile.h:170-176` 记着这次下调的理由——
这张表每次引导都付，不管 netfilter 有没有被加载过，所以它的天花板属于档位的内存
预算，而不是一个没人开启的功能（见「EMBEDDED 达标」那一节）。

每条**上限** 64 字节（`NET_PROFILE_CONNTRACK_ENTRY_BYTES`，`net_profile.h:486-487`），
由 `_Static_assert` 钉住（`netfilter_nat.c:44`），将来加字段撑破档位预算是编译错误，
不是静悄悄变大的 `.bss`。

**实测 sizeof 现在正好是 64 字节，即上界，本档的 filter 项没有余量。** `810e9e431`
给条目加了 `icmp_off` / `icmp_type` / `icmp_id` 三个字段（`netfilter.h:281-284`），
结构体从 56 涨到 64：原布局排到 `rhash_next` 是 52 B，尾部补齐到 56；加 5 B 后是 57 B，
因含 `uint64_t` 而按 8 对齐，补到 64。于是 EMBEDDED 档的 filter 项是
32×64 + 16×80 = **3328 B，正好等于** `NET_PROFILE_FILTER_BUDGET`
（`netfilter_nat.c:561` 的断言取 `<=`，所以仍然通过）。**再加一个字节到条目上，
EMBEDDED 档就编译不过**——这个方向是好的，但要知道余量已经归零。
注意 `netfilter_nat.c:554-556` 那条预算注释仍写着旧的 "32 entries x 56 B … = 3072 B"，
它没有跟着 `810e9e431` 一起更新，**以本行的编译期实测为准**。
另两条断言钉住 `NET_CONNTRACK_MAX < NET_CONNTRACK_NONE` 与
`NET_CONNTRACK_MAX % NET_CONNTRACK_BUCKETS == 0`（`:46-48`）——后者是回程链按下标
取桶的前提，不成立会静默取错桶头。

**每条目挂两条链**，不是一条：正向链按原始元组索引，回程链按**转换后**的元组索引。
这不是冗余。NAT 之后线上跑的是转换后的元组，而条目里存的是转换前的；回程包带着转换后
的地址到达，按原始元组查表必然查不到。加第二条链是为了让回程一次哈希命中，而不是线性
扫全表或把两条元组都塞进条目。只挂一条链的失败模式是**静默**的：正向翻译正常，回程查不
到条目，原样发出，对端收到一个不认识的源地址——没有任何计数器会动。

**容量与超时。** 表满时退化成有界 LRU 缓存而不是失败：路由器留住新流、忘掉最老的流，对
这么小的表是合适的取舍。清扫在 `a20_lwip_poll_timers_locked()` 里做（调用点
`kernel/net/lwip_stack.c:1326`，被一秒间隔的 `g_ct_sweep_at` 门控），每次只扫
`max_scan`（固定传 32）条并从上次的断点续扫（`netfilter_conntrack_expire()`，
`netfilter_nat.c:490-540`），所以满表也不会让某个 tick 无界。空闲超时按协议和状态分开：
TCP NEW 30s、TCP ESTABLISHED 120s、UDP 30s、**ICMP echo 30s（与 UDP 共用同一个
`g_to_udp`，`netfilter_nat.c:520-526`）**。分开的理由是半开 TCP 流不能占着槽位等一个
已建立连接的寿命，而 UDP 的"流"常常就是单个数据报，对端不会再回。ICMP 不落到
`g_to_tcp_new` 是有理由的：`netfilter_nat.c:515-519` 的注释写着——echo 没有握手，
按连接超时计它是在陈述一个该协议并不拥有的握手。

**ICMP echo 的元组编码。** 表里那两个 16 位字段对 TCP/UDP 是端口，对 echo 是**类型与
标识符**：类型一律**归一化**成请求值 8 存进 `src_port`，标识符存进 `dst_port`。归一化
是安全的，因为被跟踪的只有 8 与 0 两种类型，而它们互为补集，不会有两次不同的交换归一化
到同一个元组上。**归一化的目的**是让回程匹配不必到处开特例：echo 的两个方向**只交换地址，
两个字段都不换**——标识符在请求与回程里是同一个数（这正是它们成对的依据），类型则是 8↔0
互补。TCP/UDP 那套"两个端口全换"的比较式永远匹配不上它，所以 `netfilter_ct_icmp_is()`
单独处理；归一化之后，这个比较退化成"地址交换、两个字段相等"。
回程链上同理：ICMP 只有地址会因翻译而移动，标识符不会，类型两边都已归一化。

`/proc` 里 echo 条目**不带 `:端口` 字段**渲染，而是
`ct N: A -> B proto=1 icmp=echo id=<id> state=... nat=... packets=N`。照 TCP/UDP 的形状
印出来会读成"这条流去端口 1234"，而那个端口根本不存在。

LRU 的键是 `last_ms`，没有独立的 LRU 链表：满表插入时线性扫一遍取最小值，同 tick 插入的
两条按槽位下标决胜。**这条路径的边界**：淘汰是全局最老，不是"最老的未完成流"，所以一条
正在握手的 TCP 流和一个空闲的 UDP 报在竞争同一个受害者位置；被淘汰的流如果还有包到达，会
按新流重新建条目并**重新翻译一次**——如果期间规则变了，翻译结果可能与之前不同。

超时的边界同样要说清楚：清扫由一秒间隔门控，因此是每秒**至多**一次、每次 32 个槽位。
SERVER 档一张 1024 条的满表因此最多需要 32 轮才被完整看过一遍，也就是 32 秒；条目在被
看到之前不会被回收，`ct_tracked` 会在这段时间里显示为满。断点每轮前移 `scanned` 个槽位
（`netfilter_nat.c:538`），所以 32 秒是**上界**而不是平均——一个刚插进"还没被扫到的那
一段"的条目，要等断点绕回来才可能被回收。EMBEDDED 档 32 条恰好一轮扫完，这个延迟不存在。

`ct_sweeps`（`/proc/a20/netfilter`）统计清扫**运行过**的次数，和 `ct_timeout`（清扫回收了
多少条）分开，因为"没有条目空闲"和"根本没人看过"在别的计数器上长得一样。`ct_lru_victim`
是下一次满表插入会被淘汰的条目的**源端口**（`netfilter_nat.c:407-410`）——计数器说不出
"满表会忘掉哪条流"，而这正是排障时真正要问的问题。**它的边界**：ICMP echo 条目的
`src_port` 存的是归一化后的类型值 8，所以对一条 echo 条目，这个字段打印出来是 `8`
而不是端口；用它判断"下一条被忘掉的是哪条流"时要知道这一点。

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

- **只跟踪 ICMP echo，不跟踪任何 ICMP 差错报文。** TCP、UDP 与 **ICMP echo**（type 8/0）
  会建条目；其余 ICMP 类型（destination unreachable、time exceeded 等）既不匹配已有流
  也不能新建流。**为什么是 echo 而不是别的**：echo 请求与回程由发送方自己选的标识
  （identifier）配对，"这两个包属于同一次交换"这件事已经在线上了，不需要推断；而把差错
  报文配到流上必须去读它引用的那个原始包再比对，那就是一个 ALG，本轮不做。
  因此**依赖 ICMP 差错跟踪的路径 MTU 探测在这里不工作**：一条被翻过 NAT、需要 PMTU 的流
  会黑洞，而不是靠 fragmentation-needed 恢复。
- **ICMP echo 只按地址翻译。** 它没有端口，所以 NAT 规则里写了 `toport=` 也无处可落，该
  字段被忽略；报文体内部（比如引用的内嵌头部）一个字都不改。**echo 标识符从不翻译**，
  这正是回程还能配上的原因。ICMP 校验和覆盖 ICMP 头与载荷、**不覆盖 IP 地址**，所以
  地址改写不必动它——这一点是"只翻地址"能成立的前提，不是省事。
  另外 ICMP 超时走 **UDP 的空闲超时**而不是 TCP 的：echo 是无连接协议，回程时间由对方
  决定，没有握手可据以判断"新建"状态，用 connect 超时去计它是在陈述一个它没有的握手。
- **ICMP 差错报文本身不被翻译。** 按 RFC 5508 NAT 应当改写差错报文内嵌报文的五元组并
  重算 ICMP 校验和，这里不改；于是一端从对端收到的差错报文里看到的是翻译后的地址，
  内层校验和对不上，通常整条报文被丢。这是上面第一条的另一面：既不认领，也不还原。
- **没有 ALG。** 无 FTP/SIP/ISAKMP 载荷检查，地址写在报文体里的协议只翻头部，控制通道
  不会跟着走。
- **不分片 NAT。** 无分片包和首片会翻；非首片没有端口，既无元组可索引也无头部可改，直接
  放行——选择放行而不是"记条目但不翻译"，因为后者会让条目看起来有状态而实际转换是残的。
  被分片的流因此可能被静默弄坏。**分片的 echo 属于这一类**：解析器只在未分片的包上认出
  echo 类型，所以它既不被跟踪也不被翻译。
- **无 helper 模块**，无 fullcone / 端口保持变体，无 IPv6 NAT（hook 只解析 IPv4）。
- **除 TCP/UDP/ICMP echo 外一律不跟踪**，即使 NAT 规则里写了 `proto=`。
- **表是定长的**，三档分别 32 / 256 / 1024 条（`net_profile.h:177` / `:455` / `:404`），
  满即 LRU 淘汰（`ct_evicted` 计数），不是拒绝新流。淘汰的是全局最老条目，边界见上文
  "容量与超时"。**EMBEDDED 档的 32 条是本轮从 64 下调的**，该档整张网络内存的断言上界
  是 44 KiB（`NET_PROFILE_TOTAL_BUDGET`），下调的直接后果是这个目标上并发跟踪流数减半，
  溢出表现为静默重新建条目 + 重新翻译，与上面 LRU 那一段同源。
- **没有动态端口分配。** `masquerade` 不带 `toport=` 时保留客户端原端口，多个同源端口的
  并发连接不会被打散；只有显式写 `toport=` 才会改端口，而那是所有走这条规则的流共用的一个
  固定值，不是分配器。

## 门禁

| 门禁 | 断言 |
|---|---|
| `smoke-netfilter` | 既有 drop/accept 语义不回归，且 conntrack 计数随 `udp_send_once` 增长、`/proc` 里能看到该五元组（`user/cmds/net/netfilter_test.c` 测试 8）；**外加 ICMP echo 跟踪**：手工构造的 echo 请求发往网关，`ct_packets` 增长、`ct_tracked` 增加、`/proc` 里出现 `icmp=echo id=...` 且条目是 `state=established`——回程能匹配上这条状态才是本项的关键证据，只有 insert 的话条目会一直是 `new`（测试 9） |
| `smoke-netfilter-nat` | 端到端 DNAT 端口转发：QEMU `hostfwd` 把 `127.0.0.1:18081` 转到 guest 18081，guest 侧规则转到 18082，host 侧 `tools/netnat_host_probe.py` 连上、发出、收到回显 |
| `smoke-ct-capacity` | 表填到 `ct_capacity` 恰好停住；多一条流恰好淘汰一条，且**淘汰的是最久未用的那一条**（淘汰前后的 `ct_lru_victim` 各断言一次）；短超时（`cttimeout 100 100 100`）下条目在截止时间内被回收、`ct_timeout` +1 且 `ct_sweeps` 前进 |
| `test-nat-rewrite`（在 `make check` 内） | 主机侧编译**真实的** `kernel/net/netfilter_rewrite.c`：SNAT/MASQUERADE/DNAT 地址改写后 IP 与 L4 校验和都用独立的一次性重算校验；端口改写与 `toport=0` 语义；masquerade 取的是"正在离开的那块接口"的地址；回程元组的推导，并与对端实际会发出的那个帧逐字段比对（`tools/test-nat-rewrite-host.c`） |

SNAT/MASQUERADE **没有**端到端门禁，这是拓扑决定的而不是遗漏：QEMU user-net 自己在宿主
侧做 NAT，guest 外面没有第二个对端，回程包不存在，所以 `smoke-netfilter-nat` 只能覆盖
DNAT。上表最后一行是这个方向上能拿到的最强证据——它跑的是出货源码，不是副本，但仍然是
主机侧单元测试，不能替代真机上的端到端验证。

两个测试钩子是为了让上面的容量/超时断言不必靠 256 条真实流和 30 秒等待达成：
`ctinject`（经由数据面同一个 `netfilter_ct_insert` 插入合成流）和 `cttimeout`（运行期覆盖
三个毫秒常量，0 恢复默认）。**只有三个**——ICMP 复用 UDP 那个（`netfilter_ct_time_init()`，
`netfilter_nat.c:140-150`），所以覆盖超时测试同时改了 echo 条目的寿命，这一点是巧合
而不是设计，测试脚本改三个值时要知道。**`ctinject` 只接受 TCP / UDP**
（`netfilter_nat.c:411-418` 拒掉其他 proto 与其他状态），理由写在源码注释里：注入器
只应当造出数据面真能造出的表形状。因此 `smoke-ct-capacity` 覆盖的是 TCP/UDP 的淘汰与
超时，**ICMP 条目的淘汰与超时没有门禁**，它们只由代码路径本身保证。
两者都只是 `/proc` 动词，不新增内核状态。

DNAT 门禁有两个 QEMU 10 的坑，都写在 `tools/targets-smoke.mk` 的目标注释里：规则必须
带 `hostfwd=` 前缀（裸短格式被当成已废弃的布尔量），guest 地址前**不能**有冒号（否则
guest 段被解析成 `[addr]:port`，端口取到 10）。另外门禁必须用 `a20.tcpmode=lwip` 启动：
默认的 `fast` 模式不建 lwIP LISTEN pcb，入站 SYN 只会得到 RST
（`kernel/net/socket_control.c:211-231` 的注释与 `:253` 的 `/proc` 打印）。

`user/cmds/net/netnat_test.c` 里的阶段划分是有意的：先在没有 NAT 的情况下验证 conntrack
本身工作正常，再验证规则解析的拒绝路径，再装规则，最后用一次**注定失败**的直连做阴性对
照，确认转发是规则造成的而不是环境本来就能通。
