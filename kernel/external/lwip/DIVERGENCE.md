# lwIP 分叉差异清单 (Divergence Manifest)

最后核实：2026-10-03（`644b3c76`）。

本文件记录 `kernel/external/lwip` 与上游 lwIP 的**可证明**差异。它存在的唯一目的
是让"外部依赖"这个标签不再误导读者，并让重新同步上游成为一件机械可执行的事。

## 0. 上游基线状态：未记录

**本仓库此前没有记录任何上游 SHA。** 这不是遗漏的文档，而是分叉治理缺失的第一个
症状：`kernel/external/` 的语义是"vendored third-party，不随上游风格重排"
（见仓库 README），但该目录里已经存在 A20OS 自有的协议栈概念，因此这个语义
现在是**不成立的**。

在本文件写入时，能从树内证明的上游身份只有：

| 事实 | 值 | 来源 |
|---|---|---|
| lwIP 版本宏 | `2.2.2` + `LWIP_RC_DEVELOPMENT` ⇒ 展开为 `2.2.2d` | `kernel/external/lwip/src/include/lwip/init.h:53-61` |
| 树内 git 元数据 | 无（无 `.git`、无 hash 文件、无 `CHANGELOG`、无 tag） | 全树搜索 |
| 布局 | `src/core/ipv4/`、`src/core/ipv6/`、`src/core/dns.c` | 2.2.0 之后的 master 重排布局 |

`2.2.2d` 是**开发快照**，不对应任何发布 tag，因此**不能用版本号反推唯一上游
commit**。现已显式抓取并记录基线：

| 项 | 值 |
|---|---|
| 上游 SHA | `d08f4773edd0182b7910fc8f046eed82ffcd67c9` |
| `git describe` | `d08f477`（无 tag 命中） |
| 记录位置 | `kernel/external/lwip/.upstream-base` |

### 0.1 两种"差异"必须分开看

把 vendored 树直接与上游 HEAD 对比，会把**两类改动混为一谈**：A20OS 自己写的，
和"上游前进了我们没拉"的。两者必须分别测量：

| 口径 | 测量方法 | 结果 |
|---|---|---|
| **A20OS 自己的改动** | `git diff f773b0aa HEAD -- kernel/external/lwip`（`f773b0aa` = 重新 vendoring 的提交） | **14 文件(源码),+1458 / −304** |
| **相对上游的落后程度** | vendoring 时的树 vs `d08f477` | **18 个文件不同** |
| **与上游 HEAD 的合并差异** | vendored 树 vs `d08f477` | 25 文件,+761 / −427 |

第三行是前两行的叠加，**不能**被当作"A20OS 改了多少"。本文件后续一律使用第一行。

## 1. 树的构成（树内可证明）

| 项 | 值 |
|---|---|
| 跟踪文件数 | 211 |
| `.c` 行数 | 34,964 |
| `.h` 行数 | 30,306 |
| 实际参与编译的 `.c` | **39**（由 `sources.mk` 唯一列举） |
| 编译产物（riscv64 hosted tier 2） | `.text` 389,018 + `.data` 168,592 + `.bss` 530,487 + `.rodata` 1,536 = **1,089,633 字节**，占 `kernel.elf` 的 7.1% |

### 1.1 从未参与编译的头文件（10,503 行）

`sources.mk` 只列 39 个 `.c`，因此下列头文件**一次都不被编译**，只增加树体积与
许可证审计面：

| 目录 | 行数 | 原因 |
|---|---|---|
| `src/include/lwip/apps/` | 4,865 | 35 个头（mqtt/snmp/httpd/mdns/lwiperf/smtp/sntp/tftp/netbios…），而树内**没有** `src/apps/` |
| `src/include/netif/ppp/` | 5,470 | 24+5 个头，`PPP_SUPPORT 0` |
| `src/include/compat/` | 168 | POSIX 兼容 shim，未使用 |

## 2. A20OS 自有改动

**测量口径：`git diff --numstat f773b0aa -- kernel/external/lwip/src kernel/external/lwip/sources.mk` = 15 文件，+1840 / −315**
（`f773b0aa` 是把 lwIP 重新 vendoring 进内核的提交，作为"未改动基线"）。

> 口径说明：数字只统计 `src/` 与 `sources.mk`，不含本文件自身；重跑上面那条
> `git diff` 即可复核。下文 §2.1 的清单以当前口径为准。

### 2.1 受影响的文件（15 个）

```
+483/-0   src/core/tcp_cubic.c                      【新增文件，上游无对应物】
+396/-157 src/core/tcp.c                            PCB 链表按 lane 分桶 + CUBIC RTO 分支 + wnd_limit
                                            （+9）  补齐缺失的 LWIP_ASSERT_CORE_LOCKED()
+209/-0   src/include/lwip/priv/tcp_cubic_priv.h    【新增文件，上游无对应物】
+178/-82  src/core/udp.c                            PCB 链表按 lane 分桶
+175/-4   src/core/memp.c                           全部 14 个池按当前 lane 索引的描述符表 + per-(lane,pool) 计数
                                                          （见 §2.9；阶段 C2 推广自阶段 C 的两个 pbuf 池）
+146/-0   src/include/lwip/priv/pcb_lane.h          【新增文件，上游无对应物】
+100/-37  src/core/tcp_in.c                         lane 感知的输入查找 + CUBIC ACK 分派
                                      （+2）        tcp_trigger_input_pcb_close() 补断言
 +61/-25  src/include/lwip/priv/tcp_priv.h         TCP_REG/TCP_RMV 改为 lane 索引
 +29/-1   src/include/lwip/tcp.h                    struct tcp_pcb 增加 lane / cong_alg / wnd_limit 字段
 +24/-6   src/core/pbuf.c                           LS2K1000 板级诊断 printf
 +18/-0   src/core/tcp_out.c                        CUBIC 快重传分支
 +12/-0   src/include/lwip/priv/memp_priv.h         per-lane 分配计数器读取口（见 §2.9）
  +6/-1   src/include/lwip/udp.h                    struct udp_pcb 增加 lane 字段
  +2/-2   src/core/timeouts.c                       定时器按 lane 分片
  +1/-0   sources.mk                                登记 tcp_cubic.c
```

引入这些改动的 A20OS 提交：

| 提交 | 说明 |
|---|---|
| `a06f4575` | 首次引入网络栈（lwIP 初次 vendoring） |
| `74578420` | ABI 文件划分 |
| `f773b0aa` | 重新 vendoring：210 文件一次性引入（用户态 → 内核） |
| `8e466330` | LS2K1000 移植，`pbuf.c` +15 行板级诊断 |
| `7a0966c0` | lane stage B：按 lane 分桶 lwIP PCB 链表 |
| `b1bb28b5` | 按 lane 分片 TCP 快/慢定时器 |
| `16304db8` | 修 lane 桶中 TCP pcb 双重索引移除 |
| `5ea06a78` | lane stage C：pbuf 池按当前 lane 索引的骨架（见 §2.9） |
| 见 §2.9 | lane stage C2：分片推广到全部池，并记下 `MEMP_MEM_MALLOC=1` 下真正无锁共享的是 mem.c 全局堆 |
| 见 §2.5 | 把锁契约变成可执行：`LWIP_ASSERT_CORE_LOCKED()` 从空宏接到 `g_lwip_lock` 的持有者 CPU |

### 2.2 引入的独有概念

`pcb_lane.h` 定义了上游不存在的概念：`NET_PCB_LANE_BUCKETS`、
`NET_PCB_LANE_WILDCARD`、以及把 4 元组哈希到 lane 的 `net_lane_of()`。
`struct tcp_pcb` / `struct udp_pcb` 增加了 `lane` 字段。

**桶下标与归属 lane 是两个不同的量，`pcb_lane.h` 现在把两者分开命名：**

| 宏 | 回答的问题 | 取值 |
|---|---|---|
| `NET_PCB_LANE_OF_PCB(pcb)` | 查找要遍历哪个链表头 | 通配 pcb 返回哨兵桶 `NET_PCB_LANE_ANY` |
| `NET_PCB_LANE_OWNER_OF_PCB(pcb)` | 这个 pcb 的**工作**归哪条 lane | 恒为 `0..CONFIG_NET_LANES-1` 的真实 lane |

通配 pcb（`bind(0.0.0.0)`）在 `NET_PCB_LANE_OWNER_OF_PCB()` 下的归属 lane 是
`net_lane_of(0, local_port)`，与移植层 `net_socket_lane_of_addr()` 对同一个通配
bind 算出的值相同——两处都是"哈希零地址"，所以这是一致而不是两套需要手工对齐
的规则。阶段 D 按归属 lane 分发收包，所以 socket 侧必须取归属 lane。哨兵桶只是
查找用的桶，`CONFIG_NET_LANES` 并不存在这一条 lane，任何拿它去索引 per-lane 数组
的地方都是越界。

**这不是"补丁级"改动，而是数据结构级改动。** 这一点必须让任何读代码的人知道：
本目录下的 TCP/UDP 链表组织方式与上游不同，不可按上游文档推断行为。

§2.9 的 `LWIP_MEMP_LANE()` 是同一族概念的另一处：**"当前 lane"是移植层维护的
上下文**（`net_lane.h` 的 `net_lane_ctx_push/pop/get`），由进入 lwIP 核心的各入口
设置，而不是 lwIP 自己推导出来的。它刻意**不是**第二种 lane 定义——从 CPU 派生
lane 的 `net_lane_of_cpu()` 已经存在，`docs/net/net-lanes.md` 记录了那正是
`16304db8` 与 `f7f3d670` 两次事故的成因：一条连接上的 pcb 与 pbuf 必须落在同一条
lane 上，而这件事只有在"lane 是什么"全树只有一个定义时才可能成立。

### 2.3 我们主动裁掉的上游目录

以下上游目录/文件在本树中**不存在**，属有意裁剪（不是遗漏）：

```
src/api/          netconn + socket API（LWIP_NETCONN=0 / LWIP_SOCKET=0）
src/apps/         MQTT/SNMP/HTTP/mDNS/…（无实现，仅剩头文件，见 §1.1）
src/netif/ppp/    PPP_SUPPORT=0
src/netif/bridgeif*.c, lowpan6*.c, slipif.c, zepif.c
```

裁剪的后果：本树**无法**使用 netconn/socket API、MQTT、SNMP、HTTP、mDNS、
PPP、6LoWPAN、SLIP、Zephyr 网关。若要启用其中任一项，需要先把对应上游文件
重新 vendoring 回来，并在 `sources.mk` 中登记。

### 2.4 明确的非改动

`src/core/netif.c` **未被 A20OS 改动**（见 §2.1 的文件清单不含它；它与上游
HEAD 的 2 行差异属于 §0.1 的"上游漂移"，不是我们的编辑）。

因此 `struct netif` 仍是上游定义：单组 input/output/linkoutput 回调、无队列、
无 RSS、无多队列、无校验和/TSO 卸载。**多队列与卸载能力完全受限于上游 `netif`
抽象**——这是 lwIP 无法通过"小幅修改"解决的能力天花板。

### 2.5 `LWIP_ASSERT_CORE_LOCKED()` 的接线（本树独有）

上游 `src/include/lwip/opt.h:227` 把 `LWIP_ASSERT_CORE_LOCKED()` 定义成**空宏**，
除非移植层自己 `#define` 它。本树此前没有定义，于是 `tcp.c` / `tcp_in.c` / `raw.c` /
`udp.c` / `dns.c` / `ethernet.c` 里那几十处断言**全部是空操作**——
`docs/net/network-lock-contract.md` 里的锁纪律在运行时没有任何强制手段。

现在 `kernel/net/lwip_port/lwipopts.h` 在 `CONFIG_NET_LOCK_ASSERT=1` 下把它映射到
`a20_lwip_assert_core_locked()`（`kernel/net/lwip_stack.c`）：

- `a20_lwip_lock()` 记录持有者 **CPU id**（不是布尔量——布尔量在"别的 CPU 持有"时
  会误判为通过），`a20_lwip_unlock()` 清回 `A20_LWIP_LOCK_UNOWNED`；
- 宏里传的 `__builtin_return_address(0)` 在展开点求值，指向**未持锁运行的那个 lwIP
  函数**，panic 之前先记录下来；
- `a20_lwip_init()` 结束时才 **arm**。arm 之前断言是 no-op：lwIP 自己的
  `lwip_init()` / `netif_add()` / `netif_init()` / `dhcp_start()` 链在
  `a20_lwip_init()` 内部跑，那里根本没有 A20OS 的锁；早先的探针在引导期测到 23 次
  命中、8 个不同返回地址（`netif_init`、`netif_add`、`lwip_init`、
  `netif_add_ip6_address`、`a20_lwip_init`、`a20_lwip_loopif_init_cb`），
  对它们 panic 会让**每一个**配置启动即死。
- arm 之后一次违规即 **panic**（不是只计数）：计数型信号会被当成噪声忽略，而这条
  契约的价值恰恰在于"违反必须停下来"。

同时补上了此前**没有**断言的入口，把探针的盲区变成覆盖区（上游在此处也没有断言，
所以这些是纯新增行）：`tcp_new()`、`tcp_new_ip_type()`、`tcp_slowtmr()`、
`tcp_fasttmr()`、`tcp_netif_ip_addr_changed()`、`tcp_process_refused_data()`、
`tcp_trigger_input_pcb_close()`。`tcp_abort()` 在本树**已经**带断言
（`tcp.c:656`），`docs/net/net-lanes.md` 说它没有，那条记录已过时。

默认构建 `CONFIG_NET_LOCK_ASSERT=0`，宏仍为空，代价为零。

### 2.6 `SO_REUSE=1`（编译期开关，非源码改动）

`SO_REUSE` 在上游 `opt.h:2137` 默认为 `0`。本树的 `lwipopts.h` 现在显式打开它。
不改任何 lwIP 源码，但**改变了上游代码的编译结果**，因此登记在此。

关闭状态下 `setsockopt(SO_REUSEADDR)` 是**空操作**：socket 层把标志存进
`net_socket_t::reuseaddr`，`net_bind_reuse_allowed()`（`kernel/net/socket.c:170`）
也确实读它，但 lwIP 只在 `tcp_bind()` / `tcp_listen_with_backlog_and_err()` /
`tcp_connect()` 里问 `ip_get_option(pcb, SOF_REUSEADDR)`，而没有任何代码把这个
标志搬上 pcb。后果是 `tcp_bind()` 照旧扫 TIME-WAIT 表
（`max_pcb_list = NUM_TCP_PCB_LISTS`），上一次连接留下的 TIME-WAIT pcb 会把本地
端口占住 `2 * TCP_MSL`（本树 `TCP_MSL=60000`，即 2 分钟），同端口重启的 listener
拿到 `ERR_USE`（EADDRINUSE）。

打开后需要两处配合，缺一不可：

- `net_inet_tcp_apply_options()`（`kernel/net/socket_inet.c`）把 `s->reuseaddr`
  搬成 `SOF_REUSEADDR`。accept 出来的子 pcb 通过 `SOF_INHERITED` 继承
  （`tcp_in.c`: `npcb->so_options = pcb->so_options & SOF_INHERITED`），所以一个
  调用点同时覆盖 socket() 路径与 accept 路径；
- `setsockopt(SO_REUSEADDR)` 在 socket 层把新值推回已存在的 pcb——它通常发生在
  `socket()` 与 `bind()` 之间，而 pcb 在 `socket()` 时就建好了。

语义与 Linux 一致：REUSEADDR 的 bind 跳过 TIME-WAIT；`listen()` 与 `connect()`
补做"同一 local addr/port 只能有一个 listener"和 5-tuple 唯一性检查，所以打开这个
开关不会让两个活着的 listener 静默别名。

### 2.7 CUBIC 拥塞控制（RFC 8312 核心）

上游 lwIP 2.2.2d **只有 Reno 一种拥塞控制算法**，且硬编码在
`tcp_in.c` / `tcp_out.c` / `tcp.c` 三处：`tcp_in.c` 的 ACK 分支里 `cwnd += 1 MSS
per cwnd acked`，`tcp_out.c` 的 `tcp_rexmit_fast()` 里 `ssthresh = MIN(cwnd,
snd_wnd)/2`，`tcp.c` 的 RTO 分支里 `cwnd = mss`。上游没有任何"按连接选择算法"的
机制，`TCP_CONGESTION` 在 lwIP 里根本不存在。

A20OS 新增：

| 文件 | 作用 |
|---|---|
| `src/core/tcp_cubic.c`（新增） | 算法本体：整数定点实现，无一个 float |
| `src/include/lwip/priv/tcp_cubic_priv.h`（新增） | `struct tcp_cubic_state`、定标常量、算法名解析 |
| `src/include/lwip/tcp.h` | `struct tcp_pcb` 增加 `cong_alg` 与 `cubic` 字段 |
| `src/core/tcp_in.c` | ACK 分支按 `cong_alg` 分派；`dupacks > 3` 的 Reno 加窗对 CUBIC 关闭 |
| `src/core/tcp_out.c` | `tcp_rexmit_fast()` 里的快重传降窗分派 |
| `src/core/tcp.c` | RTO 分支调用 `tcp_cubic_on_rto()` |
| `sources.mk` | 登记新文件 |

**这不是"补丁级"改动，而是算法级改动。** §2.2 的警告同样适用于此：
读本树 TCP 代码时不能按"lwIP = Reno"推断行为。开关是 `LWIP_TCP_CUBIC`，
由 `kernel/net/net_profile.h` 按资源档位给出（EMBEDDED 关，DEFAULT/SERVER 开）。

实现的 RFC 条款与**未**实现的条款，都逐条写在
`src/include/lwip/priv/tcp_cubic_priv.h` 的文件注释里。特别注意三条**不是**
上游行为的语义：

1. **快收敛（§4.6）是"减小" `W_max`**，用 `W_max * (1 + beta_cubic) / 2`，
   不是 `W_max * (1 + beta_c)`。名字有误导性：它让流在更长的时间里长得**更慢**，
   目的是给新加入的流让出带宽。Linux 的 fast-convergence 注释里那个
   `beta_c = 0.85` 出自更早的版本，RFC 8312 的伪代码用的是 `beta_cubic = 0.7`
   并带 `/2.0`。
2. **§4.5 的 `ssthresh = cwnd * beta_cubic`（即 ×0.7），不是 `cwnd * (1 - beta)`**
   （§4.1 描述的是 cwnd 被降到 `W_max * beta_cubic`，两处同为 ×0.7）。
3. **§4.7（RTO）与 §4.5（快重传）是两条不同的规则**：RTO 之后的第一次拥塞
   避免用 `K := 0`、`W_max := 当时的 cwnd`。因此 `tcp.c` 的 RTO 分支调
   `tcp_cubic_on_rto()` 而不是 `tcp_cubic_on_loss()`——走同一条路径会让连接停在
   一条锚定在已被超时甩掉的 `W_max` 上的曲线上。

**时间基准是秒。** `W_cubic` 的 `C = 0.4` 单位是 segment/s³，而本栈唯一的时钟是
`tcp_ticks`（每 `TCP_SLOW_INTERVAL` = 500 ms 一跳）。所以 `K` 与 epoch 经过时间都
先换算成 1/256 秒再进立方项。这一步漏掉不是精度损失而是**速率差 8 倍**。

**§4.2 的 TCP-friendly 区域已按 Eq. 4 实现**（`tcp_cubic_w_est()`，目标是
`max(W_cubic, W_est)`），但它的 RTT 分辨率在本栈上有硬边界，不知道就会误读行为：
lwIP 2.2.x 的 `struct tcp_pcb` **没有 `rtt` 字段**，唯一的 RTT 估计量是 `pcb->sa`
（Van Jacobson 平滑 RTT，以整个 `TCP_SLOW_INTERVAL` = 500 ms 为单位取整）。所以
Eq. 4 里 `t/RTT` 的分母在 0.5 s 之下没有任何表达方式：`sa == 0`（回环与绝大多数
局域网流量都会读到 0，因为亚 tick 的 RTT 在 tick 量化里无处安放）被**下限钳到
1 tick**，得到 alpha/0.5 s = 1.07 段/秒——比真实 RTT 允许的更保守，但不是 RFC 说的
那个 RTT。要拿到毫秒级 RTT 得给 `struct tcp_pcb` 加字段并改 `tcp_in.c`，本轮**不做**。

由这条边界还能推出一条对使用者有意义的结论：Eq. 4 压过 Eq. 1 需要
`alpha*K/RTT > 0.3*W_max`，代入最短可表达的 RTT = 0.5 s，条件退化为
`W_max < 约 3.25 段`。也就是说 **TCP-friendly 区域只在刚复位、窗口极小的连接上
可能生效**，在 CUBIC 真正要对付的大窗口传输上永远不会成为约束项。主机侧测试
`test_friendly_region_binding()` 正是在 `W_max = 4` 段、`sa = 1` 的配置下把这一区域
钉住的——否则这个分支在任何可跑的测试里都观测不到，只能靠读代码相信它。

**没有实现、因此不得假定的**（同样逐条列在头注释里）：HyStart / TCP-AQ / DCTCP /
Prague / ECN 与 RTT 方差耦合均无；`W_max` 不跨 pcb 生命周期持久化。

算法正确性由主机侧单元测试 `tools/test-tcp-cubic-host.sh`
（`tools/test-tcp-cubic-host.c`）覆盖，参考值全部按 RFC 原文的双精度公式独立算出，
不从实现反推。往返传输的 smoke 门禁**不能**覆盖这些：一条从不丢包的连接根本不
会离开慢启动，因此对本文件里那几类算错（立方根截断、定标错位、时间轴单位错、
快收敛方向反了）全部不敏感。

### 2.8 每 pcb 接收窗口上限（`wnd_limit`）

lwIP 的 `tcp_recved()` 每次应用层读完就把 `rcv_wnd` 直接补回 `TCP_WND_MAX(pcb)`
（`src/core/tcp.c` 的注释写的是 "restore the window"）。因此**在 lwIP 内部没有任何
一处可以挂一个比 `TCP_WND` 更小的常驻接收上限**——任何写进 `rcv_wnd` 的较小值
都会在下一次 `recv()` 时被抹掉。要让 `SO_RCVBUF` 成为一个真的约束而不是设置即丢，
必须给 pcb 一个上限字段：

| 改动 | 位置 |
|---|---|
| `tcpwnd_size_t wnd_limit;`（0 = 无上限） | `src/include/lwip/tcp.h` 的 receiver 段 |
| `tcp_recved()` 先取 `min(TCP_WND_MAX(pcb), wnd_limit)`，两者皆 0 时保持上游值 | `src/core/tcp.c` |

`struct tcp_pcb` 是 lwIP 的公开结构体，`wnd_limit` 属于**必须与上游同步的字段**，
和 §2.2 的 `lane` / §2.7 的 `cong_alg` 同类：合并上游时这三个字段要一起搬。

**语义边界（不要按 Linux 推断）**：

- 上限只约束 `rcv_wnd`（本地还愿意收多少），不约束 `rx_buf` 的 pbuf 数量，
  也不影响 `tcp_recved()` 之外的行为。
- 上限不能突破窗口缩放：线上字段是 `rcv_wnd >> rcv_scale`，16 位，所以有效天花板
  是 `min(TCP_WND, 0xFFFF << TCP_RCV_SCALE)`，超出的部分在
  `kernel/net/socket_inet.c` 的 `net_inet_tcp_buf_apply()` 里被夹掉。两个界都用
  **配置常量**而不是 pcb 的当前状态：`TCP_WND_MAX(pcb)` 在握手完成、对端通告窗口
  缩放之前等于 `TCPWND16(TCP_WND)`，而 `pcb->rcv_scale` 在第一条窗口更新选项发出去
  之前还是 0。拿 pcb 状态去夹，会让每个刚 `socket()` 出来的 socket 被永久钉死在
  64 KiB。
- 下调立即生效（同时写 `rcv_wnd` 并重跑公告逻辑）；上调也要写，因为 lwIP 没有
  "还回去" 的机制。

### 2.9 `LWIP_MEMP_LANE()`：memp 按当前 lane 索引（本树独有）

**这条与 §2.5 是同一种接线**：上游不提供这个维度，移植层提供，lwIP 侧只加一个
受 `#ifdef` 保护的分派。它不是"给 memp 打补丁"，因为改的是**分配器回答什么问题**。

上游 `src/core/memp.c` 里没有 lane：`memp_malloc()` 收一个 pool id，
`do_memp_malloc_pool()` 收一个描述符，`pbuf_alloc()` 原样转发 `MEMP_PBUF_POOL`。
本树在 `CONFIG_NET_LANES > 1` 下让移植层通过 `LWIP_MEMP_LANE()`（声明在
`kernel/net/lwip_port/lwipopts.h`，实现在 `kernel/net/lwip_stack.c` 的
`a20_lwip_memp_lane()`）回答"当前这段工作属于哪条 lane"，memp 用它索引一张
per-lane 描述符表。**每一个池都按 lane 索引**，不是只有 pbuf 两个——阶段 C 只切了
`MEMP_PBUF` / `MEMP_PBUF_POOL`，阶段 C2 把它推广到全部 14 个池。理由是直接的：
`g_lwip_lock` 切开之后，若 `tcp_pcb_alloc()` 之类仍走全局表，两条 lane 就是并发踩
同一张表，"部分分片"既不是原来的全局锁、也不是新的分片锁，是第三种更坏的状态。

| 改动 | 位置 |
|---|---|
| `memp_desc_for(type)`：`memp_lane_pool[当前 lane][type]`，未初始化时回落 `memp_pools[type]` | `src/core/memp.c` |
| `memp_init()` 在自身池循环之后，为 lane 1..N-1 复制**每一个**池的描述符 | `src/core/memp.c` |
| `struct memp_lane_count { u32_t alloc; u32_t freed; }` 与 `memp_lane_count_get(lane, pool)` | `src/include/lwip/priv/memp_priv.h` |
| `memp_malloc()` / `memp_free()` 各自按 `(当前 lane, pool)` 记一次单调计数 | `src/core/memp.c` |
| 三张表的真实字节数与 `NET_PROFILE_MEMP_LANE_TABLE_BYTES` 对账的 `_Static_assert` | `src/core/memp.c` |

**lane 0 指向上游那个描述符对象本身**，不是副本，所以 `memp_pools[]` 与
`lwip_stats.memp[]` 对 lane 0 的指向和上游一模一样。lane 1..N-1 的副本在
`memp_init()` 里取——那是描述符唯一完整的那一刻（`LWIP_MEMPOOL_DECLARE` 用池自己的
`LWIP_MEMPOOL()` 行算出元素大小，在这里重写一遍就是第二份要同步的副本）。

**per-lane 副本里的 `stats` 指针刻意保持指向同一个 `stats_mem`**。`lwip_stats.memp[]`
与 `/proc` 渲染器把这些计数器当全局量读——`kernel/net/lwip_stack.c` 报
`lwip_stats.memp[MEMP_TCP_PCB]->used` 为 `tcp_active`——给每条 lane 一份私有 stats
块，会在多 lane 构建上把这个数悄悄重新定义成"lane 0 的 TCP PCB 数"。能拆的拆了；
被当总量读的仍然是总量。

**必须说清楚它没有做到什么**，因为名字很容易被读大：

- **它没有分片内存。** `net_profile.h` 三档全是 `MEMP_MEM_MALLOC=1`，该模式下
  `memp_init_pool()` 是空桩，`do_memp_malloc_pool()` 是
  `mem_malloc(MEMP_SIZE + MEMP_ALIGN_SIZE(desc->size))`——描述符只贡献一个大小，
  于是两个同样大小的描述符从同一块堆上取。真正的分片要走静态预留布局
  （`!MEMP_MEM_MALLOC`），每条 lane 一份 base 数组和一条空闲链表；本树没有建，
  `lwipopts.h` 里的 profile 静态断言也只按**一份**拷贝预留 `.bss`。
- **释放路径没有因此改变。** 同一模式下 `do_memp_free_pool()` 忽略描述符，元素
  无论经哪条 lane 的表索引进来，都回到同一块 lwIP 堆。
- 因此今天被按 lane 切开的只有**账目**，以及将来挂 per-lane base 数组的那个索引。

**内存账（阶段 C2 实测，riscv64 LP64）**。三张表的 `.bss` 占用是
`(N-1)*MEMP_MAX*sizeof(struct memp_desc) + N*MEMP_MAX*sizeof(struct memp_lane_count) +
N*MEMP_MAX*sizeof(struct memp_desc *)`；`MEMP_MAX=14`、`sizeof(struct memp_desc)=24`、
`sizeof(struct memp_lane_count)=8`。得 0 / 784 / 1904 / 4144 B（N=1/2/4/8），
**三档 profile 完全相同**，因为它不随任何一档的堆缩放。也就是说 per-lane 化在池内存上
**没有 ×N**——那正是 `MEMP_MEM_MALLOC=1` 的直接后果：池仍从同一块堆上取。将来切到
`!MEMP_MEM_MALLOC` 时要付的 ×N 是每条 lane 一份 base 数组，那时才是 profile 级的决定。
`kernel/net/net_profile.h` 的 `NET_PROFILE_MEMP_LANE_TABLE_BYTES` 与
`memp.c` 的 `_Static_assert` 是这条账的两端，改一边不改另一边会直接编译失败。

**阶段 C2 查到的、比上面更要紧的一件事：切 memp 并不足以让 `g_lwip_lock` 可切。**
`MEMP_MEM_MALLOC=1` 下真正被并发共享的可变结构不是 memp 的池，是 `src/core/mem.c`
里那一块全局堆：`ram_heap[MEM_SIZE]` 静态数组加唯一的全局 `lfree` 空闲链表，而**它在
本配置下没有任何内部锁**——实测证据：

| 事实 | 出处 |
|---|---|
| `mem_mutex` 只在 `#if !NO_SYS` 下声明，而本移植层 `NO_SYS 1` | `src/core/mem.c:376`、`kernel/net/lwip_port/lwipopts.h:6` |
| `LWIP_MEM_FREE_PROTECT()` 定义成 `sys_mutex_lock(&mem_mutex)`，`NO_SYS=1` 时 `sys_mutex_lock(mu)` 展开成空 | `src/core/mem.c:396`、`src/include/lwip/sys.h:64` |
| `LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT` 未定义（=0），所以走上面这条空互斥而不是 `SYS_ARCH_PROTECT` | `src/core/mem.c:393` |
| 预处理后的 `mem_free()` 里，`LWIP_MEM_FREE_PROTECT()` / `UNPROTECT()` 两处都是裸 `;`，切链表与 `plug_holes()` 全程无保护 | `gcc -E kernel/external/lwip/src/core/mem.c` |
| `SYS_ARCH_PROTECT` 也不能救：`arch_local_irq_disable()` 是 `csrc sstatus`，只关本 CPU 的 IRQ | `kernel/arch/riscv64/include/cpu.h:29` |
| 堆是**一块**全局数组，不随 lane 分 | `src/core/mem.c:371`（`ram_heap`） |

所以在把 `g_lwip_lock` 按 lane 切开之前，堆这条路径必须先有归属：要么每条 lane 一块
堆（回到 `!MEMP_MEM_MALLOC` 的静态预留布局，并把 ×N 记进三档 profile 的账），要么给
这块堆一把专用的全局分配器锁。哪一个都超出本阶段，本树两条都还没选。`memp` 的 per-lane
索引是这两条路的前置件，不是替代品。

**为什么是计数器而不是"已用"水位。** `memp_free()` 拿到的是 pool id 和指针，
没有任何东西说明这个元素是哪条 lane 发出去的；把它记到释放方会漂移，记到分配方
需要 lwIP 的 `struct pbuf` 里没有位置放的所有权标签。按前一种口径喂出来的水位会在
第一次 lane 不匹配时下溢 `u16_t`，所以干脆不提供。现在这两个单调计数是精确的、
不会下溢的：每条 lane 的池发出去多少、回来多少。

**`CONFIG_NET_LANES=1` 折叠成什么**：`memp_desc_for(type)` 是宏
`memp_pools[type]`，即上游原句；`LWIP_MEMP_LANE()` 不被定义，三张表一张都不编译。
实测 `memp.o` 在一 lane 下 `.text`/`.rodata`/`.sdata` 与改动前逐字节相同，加
`-fno-sanitize=undefined` 重编后整个目标文件也逐字节相同（仅存的差异是 UBSan 内嵌的
源码行号表，因为文件多了行——加一行注释也会让它动，这不是本改动特有的）。

### 2.10 从 socket 层直接写 `pcb->snd_buf`（SO_SNDBUF 抬高对既有连接生效）

上游 lwIP **没有任何接口**可以在 pcb 存活期间调整它的发送缓冲大小：
`pcb->snd_buf` 只被两处改写——`src/core/tcp_out.c:786`（`tcp_write()` 按写入量扣减）
和 `src/core/tcp_in.c:1386`（收到 ACK 时按 `recv_acked` 加回）。没有任何 setter，
`tcp_new()` 之后它只能随流量自身漂移。

因此 A20OS 的 socket 层在 `kernel/net/socket_inet.c` 的
`net_inet_tcp_buf_apply()` 里**直接写 `pcb->snd_buf` 字段**，把 `SO_SNDBUF` 的
ceiling 双向（下调与抬高）写进去。这是为了让抬高在**已建立的连接**上立即生效：
不写的话，一次 `setsockopt()` 抬高只对下一个 `connect()` 有效，因为 lwIP 在字节已经
在途时没有任何机制把 `snd_buf` 涨回去。

**依赖的不变量（必须与上游一起维护）**：

| 事实 | 出处 |
|---|---|
| `snd_buf` 是**可用空间**，不是容量 | `src/include/lwip/tcp.h:343` |
| 只有 `tcp_write()` 扣减它 | `src/core/tcp_out.c:786` |
| 只有已确认的字节把它加回来，且加回量不超过当初扣减量 | `src/core/tcp_in.c:1386` |
| 唯一的容量检查是 `len > pcb->snd_buf` | `src/core/tcp_out.c:327` |
| 初值 `TCP_SND_BUF` | `src/core/tcp.c:2048` |

由前两条可得 `snd_buf <= TCP_SND_BUF` 恒成立，因此直接赋值不会让 pcb 报告比它真实
拥有的更多容量；由第四条可得赋值也永远不会让 `tcp_write()` 意外失败（socket 层喂给
`tcp_write()` 的长度取自同一个字段）。

**必须一起记住的边界**：这个写入假定 `snd_buf` 永远只由上面两处 lwIP 代码改写。
如果上游将来给 `snd_buf` 加上别的含义（例如与某个配额共享），本树的写入就会绕过那个
配额——重新同步上游时这一节要重读。

**同时删掉的一段并行记账**：改之前 socket 层另外维护了一个"队列深度 =
`TCP_SND_BUF - pcb->snd_buf`"的估计，再拿它和 ceiling 比。那是把 ceiling 已经写进
`snd_buf` 这件事算了两遍：`TCP_SND_BUF` 93440、ceiling 16384 时，它把 77056 字节的
**预留但未用**空间当成已排队，报出 room = 0，socket 在发送队列为空的情况下永久阻塞。
现在 `net_inet_send_tcp()` 直接读 `snd_buf`，对从未设置过该选项的 socket 是同一个数
（`TCP_SND_BUF - (TCP_SND_BUF - avail) == avail`），因此只有原来错的地方变了。

### 2.11 意向登记：per-netif 校验和卸载（**未实施**，不要当成已生效能力）

**状态：只调查，未启用。** 本节记录"如果将来要做 TX/RX 校验和卸载，本树需要哪些
偏离上游的改动"，以及为什么现在不做。完整调查见 `docs/net/checksum-offload.md`。

**先更正一个此前的错误说法。** 上一轮记录的是"vendored lwIP 2.2.2 没有任何承载
握手协商的 flag"。这句话不成立，登记在案的载体是：

| 已有（上游自带，本树未启用） | 位置 |
|---|---|
| `LWIP_CHECKSUM_CTRL_PER_NETIF`，默认 **0** | `src/include/lwip/opt.h:2371-2373` |
| `NETIF_CHECKSUM_GEN_IP/UDP/TCP/ICMP/ICMP6`、`NETIF_CHECKSUM_CHECK_*` 十个位 | `src/include/lwip/netif.h:140-153` |
| `struct netif::chksum_flags` 字段（开关关闭时不编译） | `src/include/lwip/netif.h:340-342` |
| `NETIF_SET_CHECKSUM_CTRL()` / `IF__NETIF_CHECKSUM_ENABLED()` | `src/include/lwip/netif.h:408-417` |

开关为 0 时 `IF__NETIF_CHECKSUM_ENABLED(...)` 展开为空语句（`netif.h:416`），
收发两侧的校验和因此**无条件**执行。这是"能力位永远不置位"的真实原因，
不是缺少表达手段。

**若要实施，需要动上游的文件与位置：**

| 改动 | 位置 | 为什么绕不开 |
|---|---|---|
| `LWIP_CHECKSUM_CTRL_PER_NETIF=1` | `src/include/lwip/opt.h:2371-2373`（或在 `kernel/net/lwip_port/lwipopts.h` 定义，那属配置不属分歧） | 整个机制的入口 |
| TCP 发送路径接入 per-netif 控制 | `src/core/tcp_out.c:1587-1596` | **该处没有任何 `IF__NETIF_CHECKSUM_ENABLED` 包裹**，`seg->tcphdr->chksum` 无条件被写成完整校验和；只打开开关会得到"UDP 卸载、TCP 不卸载"的半吊子状态 |
| 逐帧的校验和已验证通道（RX） | `src/include/lwip/pbuf.h` 的 `PBUF_FLAG_*` | lwIP 现有的 `chksum_flags` 是**每 netif 一个位**，而"这一帧验没验"必须逐帧；`PBUF_FLAG_NOCHECKSUM` 这类位在 2.2.2 已不存在（`CHECKSUM_BY_HARDWARE` 整棵树无引用） |

**风险（按严重度）：** 协商而不逐帧传递结果 ⇒ lwIP 去验设备没算的校验和 ⇒ 每一个包
都被丢、静默损坏；NAT 的 L4 校验和增量修正（`kernel/net/netfilter_nat.c:754-892`）
与"校验和字段是部分和"互斥；`NETIF_CHECKSUM_CHECK_IP` 关掉后 IPv4 头校验和不再
过滤。

**设备侧现状（决定了 RX 一半本机不可验证）：** `VIRTIO_NET_HDR_F_DATA_VALID`
在外部 Debian QEMU 10.0.13+ds 源树 `qemu-10.0.13+ds/include/standard-headers/linux/virtio_net.h:132` 有定义（解包定位见 `docs/history/2026-10-08/migration.md`），
但 `hw/net/virtio-net.c` 全文不使用它（只有 e1000e / igb / vmxnet3 用），即 QEMU
10.0 的 virtio-net 不逐帧上报校验和有效性。TX 一半设备侧是通的：
`hw/net/net_tx_pkt.c:833-838` 会为带 `VIRTIO_NET_HDR_F_NEEDS_CSUM` 的帧补算校验和。

**重新同步上游时**：本节是意向登记，不是已生效差异，合并上游时**没有**必须搬运的
代码；但若上游将来把 `LWIP_CHECKSUM_CTRL_PER_NETIF` 的默认值或 TCP 发送路径改掉，
上面三条"必须动上游"的清单要重新评估。

### 2.10 多 lane 核心并发与 shared-state guards（2026-10-08）

本树的并发模型现不再是“所有核心入口统一由 `g_lwip_lock` 串行”。多 lane 配置下，
`kernel/net/lwip_stack.c` 保守分类普通单播 TCP/UDP 帧并在 owner-lane lock 下调用
lwIP；多 lane `ip_data` 临时状态按 `LWIP_CORE_LANE()` 选择。ARP、ND6、conntrack/NAT、
listener/UDP/RAW 等跨 lane 状态由
`kernel/net/lwip_concurrency.c` 的独立 shared-domain guards 保护。
`LWIP_MEMP_LANE()` 只选择 memp descriptor 与 per-lane 计数上下文；当前
`MEMP_MEM_MALLOC=1`，所有 pool element 从同一全局 lwIP heap 分配，不存在 per-lane pbuf
arena 或物理内存预留。

不能并行的工作仍明确走 control barrier：ARP/ND 输入与维护、ND/RA/ICMP 控制流、所有
IP 分片/重组、广播/多播、无法安全分类的包、全局 timeout 和 netif/configuration
变更。设备 RX staging 使用单独 ingress 锁，先收包/复制入 lane 队列、释放 ingress，
再由 lane consumer 执行协议处理。入队前会在 ingress 锁下短暂取 CT guard 完成 input
filter/NAT，之后释放 CT 再发布队列；不得持 lane/control/shared guard 再取得 ingress 锁。
ARP/ND6 cache guard 在发包/探测前释放，查询向调用者复制 MAC/地址数据，避免解锁后
解引用 cache 内部指针。TCP listener guard 只短暂保护 SYN backlog reservation/update；
成功 accept callback 用 `s->tcp` 作为稳定 key，保护 accept-stage slot 初始化与 head 发布，
释放 guard 后才调度 bottom-half。它不包围整个 `tcp_listen_input()` 或 `TCP_EVENT_ACCEPT`
调用链。Stage 满/无效 callback 返回 `ERR_MEM`，不在 callback 内 abort；lwIP 在 callback
返回后处理错误并 abort child。UDP/RAW per-PCB guard 则按需覆盖接收 callback；callback
契约只允许有界、预分配事件暂存，不能获取核心/ingress/socket 锁、分配或阻塞。Reassembly
只由冷 control barrier 保护；FRAG shared-domain 枚举项当前未使用。

TCP OOM reclamation 与 `tcp_pcbs_sane()` 在普通 owner-lane 路径只访问当前 lane；冷路径
检测到 `LWIP_CORE_ALL_LANES_HELD()` 时才可遍历全部 buckets（包含 wildcard sentinel）。
PCB lane hash 的端口单位是 host order：socket sockaddr 和 raw wire 端口在参与 hash 前
转换为 host order，PCB 内的 port 字段已经是 host order。首轮 N=4 外部 host-forward
测试曾因把 network-order 端口直接 hash 而将流放入错误 lane；该回归已由修复后的
`smoke-net-lanes-hostfwd` 覆盖，具体日志与结果记录在 `docs/net/net-lanes.md`。

锁顺序边界：lane/control -> 独立 shared-domain guard；shared-domain 内不得取得
lane/control/socket/ingress 锁（UDP/RAW callback 仅执行上述受限事件暂存）。分配可在 ARP/ND6 cache guard 内发生，其已检查顺序是
cache guard -> lwIP heap lock -> allocator `SYS_ARCH_PROTECT`；allocator 的 heap/protect
路径不反向进入 cache guard。TX staging guard 保持期间先取得再释放 CT guard，之后才
调用 non-blocking driver send callback；driver send 不得同步重入 lwIP。修改任一 callback
或 allocator 路径时必须重审
这些反向边。单 lane 配置保持既有单锁与 IRQ-save 语义；本节不声称 1-lane 和多 lane
逐字节/机器码等价。

这些描述记录当前源码结构，不是整体运行验证报告。`smoke-net-lanes-hostfwd` 这一项
N=1/N=4 外部收包门禁已通过，日志与探针边界记录在 `docs/net/net-lanes.md`；其他构建、
SMP/runtime 门禁仍需以各自实际执行日志确认，不从这一项推断 PASS。

## 3. 重新同步上游的流程

```sh
# 1. 记录一个上游 SHA 作为基线（一次性，需要网络）
tools/lwip-sync.sh --record /path/to/upstream/lwip

# 2. 随时查看当前差异规模
tools/lwip-sync.sh --stat

# 3. CI：基线缺失或漂移超阈值则失败
tools/lwip-sync.sh --check
```

合并上游时必须人工裁决的项（不可机械套用）：`pcb_lane.h` 及 `lane` 字段、
`TCP_REG`/`TCP_RMV` 的 lane 索引、`pbuf.c` 的 LS2K 诊断。

## 4. 已知上游安全通告

CISA ICS 2026 年通告（ICSA-26-265-01 / -02，影响 lwIP `>= 2.0.1` 至 `2.2.1`）：

| 通告 | CVSS | 组件 | 本树是否受影响 | 依据 |
|---|---|---|---|---|
| CVE-2026-87121 | 9.8 (AV:N) | `src/apps/mqtt.c` MQTT 客户端越界写 | **否** | `src/apps/` 在本树中不存在（§2.3），代码不可达 |
| CVE-2026-91018 | 8.8 | lwIP API double free | **未核验** | 需比对上游修复提交是否在 `d08f477` 内 |

**CVE-2026-91018 的核验方法**（不要靠版本号推断）：

```sh
# 取通告给出的修复提交，确认它是否为 d08f477 的祖先
git -C /path/to/upstream merge-base --is-ancestor <fix-sha> d08f477 && echo "fixed" || echo "vulnerable"
```

在该核验完成前，本文件**不声称**本树不受 CVE-2026-91018 影响。这是
`docs/security/hardening.md` 诚实性原则的直接应用：宁可缺数据，也不能让一个
未核验的结论看起来像结论。

## 5. 板级诊断代码（非上游，注意信息泄露面）

`src/core/pbuf.c` 的 `pbuf_free()` 中有一段 A20OS 自有诊断，编译条件为
`CONFIG_BOARD_LS2K1000 && CONFIG_COOPERATIVE_BOOT`：

```c
if (p->ref == 0) {
    printf("[LS2K-DIAG] pbuf_free p=%p caller=%p entry_sp=0x%lx ...\n", ...);
}
```

它在 pbuf 引用计数即将下溢时打印现场，用于定位 double-free（实测确实抓到过）。
两点需要记录：

1. 它用 `%p` / `%lx` 把内核地址写到串口。串口是特权出口，因此可接受，但**不得
   在量产配置下启用**。
2. 它是 `CONFIG_COOPERATIVE_BOOT` 的产物，在该板型之外是死代码（riscv64 /
   aarch64 / x86_64 等构建均不定义上述两个宏）。

这段代码是"外部目录里放着 A20OS 自有代码"的又一个实例，也是本文件存在的理由。
