# 网络配置设计

`kernel/net/lwip_stack.c` 不再直接硬编码地址，但部分板级 `arch_bootargs_get()` 仍返回编译进内核的 `a20.*` 参数；这与由启动器传入 bootargs 是两个不同来源。

## 设计决策

网络栈只解析 `bootargs_get()` 返回的 `a20.*` 键，并可由 DHCP 更新生效状态。物理板在没有网络参数时默认启用 DHCP，保证接入交换机/路由器后可以直接工作；提供静态地址或显式 `a20.dhcp=0` 时切换为静态配置。bootargs 的来源依架构/板级而异：RISC-V64/RISC-V32/PPC64LE 从 FDT `/chosen/bootargs` 读取；QEMU AArch64 和 VirtualBox AArch64 因尚未接入外部命令行，分别在板级源码中返回编译期 NAT 默认参数。

做出该决策的原因：

- `10.0.2.15`、`10.0.2.2` 和 `10.0.2.3` 等 QEMU user-network 默认值只是开发便利项，不是架构常量。
- 真实开发板和非 QEMU 后端需要不同地址。
- 运行时配置允许同一内核镜像在多个网络中启动，无需重新构建。
- 长期目标是让同一镜像完全由启动器提供配置；当前两个 AArch64 板级默认是明确记录的过渡例外。

## 当前 bootargs 来源

| 目标 | 来源 | 无外部参数时的行为 |
|---|---|---|
| RISC-V64、RISC-V32、PPC64LE | FDT `/chosen/bootargs` | 没有属性则默认 DHCP；部分 QEMU run/smoke 目标用 `-append` 注入 NAT 参数 |
| QEMU AArch64 | `kernel/platform/qemu-virt-aarch64/board.c` 的编译期字符串 | `10.0.2.15/24`、gateway `10.0.2.2`、DNS `10.0.2.3`、hostname `a20os-qemu` |
| VirtualBox AArch64 | `kernel/platform/virtualbox-aarch64/board.c` 的编译期字符串 | 同一 NAT 地址，hostname `a20os-vbox` |
| LoongArch64 | 固件 DTB 的 `/chosen/bootargs`（`kernel/arch/loongarch64/platform/fdt.c`）；DTB 里没有时，`UART_CMDLINE=y` 会在串口上收一次 | 未配置：**QEMU loongarch virt 不向内核送任何命令行**，实测 `-append`、`-machine append=`、`-machine dtb=<自带 bootargs>`、fw_cfg 四条路都不通。收的方式见 [../exec-xlator/01-usage.md](../exec-xlator/01-usage.md) |
| 其他未实现 `arch_bootargs_get()` 的目标 | weak hook 返回 `NULL` | 未配置 |

QEMU AArch64 当前不导入 FDT `/chosen/bootargs`，所以给 QEMU 增加 `-append` 不能覆盖板级编译字符串。要改变该目标的运行时参数，必须先实现真实的 bootargs handoff；不能把文档中的理想覆盖语义当成当前能力。

## 内核命令行键

内核识别以下命令行键。所有键都是可选的。

| 键 | 值 | 示例 |
|-----|-------|---------|
| `a20.ip` | 点分十进制 IPv4 地址 | `a20.ip=10.0.2.15` |
| `a20.netmask` | 点分十进制 IPv4 netmask | `a20.netmask=255.255.255.0` |
| `a20.gateway` | IPv4 默认网关 | `a20.gateway=10.0.2.2` |
| `a20.dns` | IPv4 DNS 服务器，可出现多次 | `a20.dns=10.0.2.3` |
| `a20.dhcp` | `1` 启用 DHCP，`0` 禁用 | `a20.dhcp=1` |
| `a20.hostname` | hostname 字符串，最长 63 字节 | `a20.hostname=a20os` |
| `a20.tcpmode` | `fast` 或 `lwip`，见下 | `a20.tcpmode=lwip` |

### 键规则

- `a20.dhcp=1` 优先于静态 `a20.ip`、`a20.netmask` 和 `a20.gateway`。内核运行 DHCP，并使用租约填充运行时配置。
- 如果 `a20.dhcp=0`，内核使用静态值；如果没有提供 `a20.dhcp` 但提供了任一静态地址键，也使用静态值。
- 如果没有任何网络键，内核默认启用 DHCP；需要保持链路未配置时可显式传入 `a20.dhcp=0` 且不提供地址。
- `a20.dns` 可以出现多次。第一次填充 DNS server slot 0，第二次填充 slot 1，依此类推，直到 lwIP DNS server 数量上限。
- `a20.hostname` 当前只赋给 loopback netif；Ethernet netif 初始化尚未设置 `hostname` 字段。
- `a20.tcpmode` 决定本地 TCP listener 是否真的存在于 lwIP。`fast`（默认、缺省值）
  保持既有行为：listener 只存在于 socket 层，只能被同一内核内走同样快捷路径的进程连接。
  `lwip` 把已绑定的 pcb 转成真正的 LISTEN pcb，端口在协议栈上 listen，**入站连接可以完成
  握手**。未识别的值按 `fast` 处理。
- 所有值都在早期启动期间解析一次，并存储到运行时 `a20_net_config` 结构体中。

## 解析位置

内核在处理其他启动参数的同一轮早期命令行遍历中解析网络键。解析后的值存放在一个结构体中：

```c
typedef struct a20_net_config {
    ip4_addr_t ip;
    ip4_addr_t netmask;
    ip4_addr_t gateway;
    ip4_addr_t dns[LWIP_DNS_SERVER_LIST_SIZE];
    int        dns_count;
    int        dhcp_enable;
    char       hostname[64];
} a20_net_config_t;
```

存储位置是 `kernel/net/net_config.c`。该结构体先清零并把 `dhcp_enable` 置为 1，然后从 `bootargs_get()` 的完整字符串解析。`kernel/net/lwip_stack.c` 的 `a20_lwip_register_netifs()` 读取该结构体，不再自行选择 QEMU 地址；板级编译默认仍会以普通 bootargs 的形式进入同一解析器。

## 回退行为

回退行为必须明确且安全：

- 如果最终 bootargs 没有任何网络键，内核默认发起 DHCP；在租约到达前 netif 暂时没有 IPv4 地址。需要完全禁用自动配置时传入 `a20.dhcp=0`。
- 需要路由的网络系统调用在提供有效配置前返回 `-ENETUNREACH`。
- 如果提供了 `a20.dhcp=1`，但 DHCP 交换在用户态启动前尚未完成，接口会保持未配置直到交换完成。用户态必须容忍临时 `-ENETUNREACH` 错误，或等待 config-ready 信号。
- 如果提供 `a20.dhcp=0` 但只提供了部分静态键，缺失值保持为零。例如只有 `a20.ip` 而没有 `a20.netmask` 时，netmask 保持 `0.0.0.0`。

lwIP 层不存在额外的隐藏地址回退；但 QEMU AArch64 与 VirtualBox AArch64 的 `arch_bootargs_get()` 明确编译了 `10.0.2.15`/`10.0.2.2`/`10.0.2.3`。其他平台是否得到这些值取决于启动器是否传入对应 bootargs。

## 运行时配置接口

除命令行外，内核通过 `/proc/net/config` 暴露运行时配置：

```text
ip=10.0.2.15
netmask=255.255.255.0
gateway=10.0.2.2
dns0=10.0.2.3
dhcp=1
hostname=a20os
tcpmode=fast
```

地址类字段只读，并反映当前生效配置。如果启用 DHCP，租约变化时这些值会更新。

**`tcpmode` 可写**，接受 `tcpmode fast` 与 `tcpmode lwip` 两条命令，其余输入返回
`-EINVAL`。它可写是因为它是唯一一个"选在 listener 建立之前"就有意义的开关，而
`/proc/net/config` 是现成的控制面；但**开机第一个 listener 通常由用户态创建，
shell 写入口来不及生效**，所以服务器仍应当用 `a20.tcpmode=lwip` 命令行键。

**运行时切换的已知限制**（首次记录于 2026-10，**机制仍未查明**）：命令行选定 `lwip`
时回环 TCP 传输（`tcp_loopback_test`）通过；当时观察到先用默认 `fast` 跑一次、
再用写入口切到 `lwip`，则 listener 建立与 `accept()` 仍正常（`smoke-net-accept` 覆盖），
但**数据传输不完成**，据此写下"部署一律用命令行键"。

> **2026-10-06 更新：这条限制本轮一次都没复现。** 在合并后的
> `feat/net-strengthening` 树上，`make smoke-net-accept` 10/10 PASS，12 轮
> fast↔lwip 运行时切换共 24/24 PASS，lwip→fast 与 fast→lwip 之后
> `tcp_loopback_test` / `tcp_accept_test` 全过，跨模式 `tcp_edge_test` 两次执行均
> PASS（数字转引自本轮正确性加固工作流的实验记录）。**这不构成"已修复"**——触发
> 条件在本轮环境里不存在，原因未查明；记载本身也可能来自更早的脏树。
> 写入口的 `tcpmode` 功能**原样保留、未削减**，上面那句"部署一律用命令行键"的建议
> 也保留，只是它的依据从"实测可复现"降级为"机理未查明"。详见
> [net-lanes.md](net-lanes.md)「更正一则：2026-10-06 一轮里这条 flake 完全没复现」。

**未来计划**：可以增加 `sys_net_get_config`/`sys_net_set_config` 来支持地址的原子更新。
当前没有这些 syscall，地址类字段仍不能作为运行时设置入口。

## 配置面的三个平面

网络的可调项分三层，各层的可调范围不同。混在一起读会得到错误结论——例如
"SO_SNDBUF 可调"不代表"能自动扩缩"，"档位可调"不代表"MCU 档位也在内"。

| 平面 | 载体 | 本轮（`feat/net-strengthening`）新增 |
|---|---|---|
| 内核命令行 | `a20.*` 键 | 无 |
| 编译期档位 | `kernel/net/net_profile.h` + `lwipopts.h` | SACK / 时间戳 / CUBIC 开关、conntrack 表尺寸、静态帧数组尺寸与预算 |
| 运行时控制面 | `/proc/net/config`、`/proc/a20/netfilter` | conntrack/NAT 动词 |
| 套接字选项 | `setsockopt` / `getsockopt` | `TCP_CONGESTION` 真算法名、`SO_SNDBUF` / `SO_RCVBUF` 真生效 |

### 编译期档位：本轮新增的项

`net_profile.h` 是所有上限的唯一来源，`lwipopts.h` 从这里取值。DEFAULT 与 SERVER
是同一个 block 的两份拷贝，所以下面两行对这两档取值相同。

| 宏 | EMBEDDED | DEFAULT / SERVER | 含义与代价 |
|---|---|---|---|
| `NET_PROFILE_TCP_SACK_OUT` | 0 | 1 | 发送侧 SACK（RFC 2018）。上游 `opt.h` 默认 0 |
| `NET_PROFILE_TCP_MAX_SACK_NUM` | 1 | 4 | 每段最多 4 个 SACK 块 |
| `NET_PROFILE_TCP_TIMESTAMPS` | 0 | 1 | TCP 时间戳与 PAWS（RFC 7323）。上游默认 0 |
| `NET_PROFILE_TCP_CUBIC` | 0 | 1 | CUBIC 拥塞控制（RFC 8312 核心条款） |
| `NET_PROFILE_CONNTRACK_ENTRIES` | 64 | 256 / 1024 | conntrack 静态表条目数 |
| `NET_PROFILE_CONNTRACK_BUCKETS` | 8 | 32 / 128 | conntrack 哈希桶数 |
| `NET_PROFILE_PACKET_RING_SLOTS` / `_FRAME_SIZE` | 4 / 512 | 16 / 1536 | `socket_packet.c` 的静态帧环 |
| `NET_PROFILE_NETIF_MAX_DEVS` / `_FRAME_SIZE` / `_MTU` | 1 / 512 / 498 | 4 / 1536 / 1500 | 每 netif 的 `rx_frame` / `tx_frame` 与 MTU |
| `NET_PROFILE_STATIC_BUDGET` | 20 KiB | 64 KiB / 128 KiB | 上面那些静态数组的 `.bss` 上界，由断言钉住 |

**EMBEDDED 档把 SACK 与时间戳关掉是刻意的**：两者都会加大每个数据段的 TCP 头，
也会加大每个已建立 PCB 的结构，而在 512 B 的池元素上这笔开销直接从载荷预算里扣。
DEFAULT / SERVER 开启后，`NET_PROFILE_PBUF_BUFSIZE` 必须从 1536 涨到 1600——选项与
载荷**共用池的头元素**，账是
`1460 + 54 + 12 (TS) + 36 (四个 SACK 块) = 1562`。`lwipopts.h:209-213` 的
`A20_TCP_OPT_HDR_MAX` 按 `(14 + 20 + 20 + 可选 12 + 可选 36)` 逐项展开，并由
`lwipopts.h:214` 的 `_Static_assert(TCP_MSS + A20_TCP_OPT_HDR_MAX <= PBUF_POOL_BUFSIZE)`
钉住"这个和不超过 `PBUF_POOL_BUFSIZE`"——**断言检查的是加了选项之后的和，不是原来
那个无选项的 54**。DEVICE 侧的 `NETIF_FRAME_SIZE` 仍是 1536：它装的是整帧
（MTU 1500 + 14 = 1514），而要装进池元素的是带选项的**段**，两者的几何不是一回事。

CUBIC 的范围与未做的部分逐条登记在 `kernel/external/lwip/DIVERGENCE.md` §2.5
（源码侧是 `lwip/priv/tcp_cubic_priv.h` 的头注释）。**它没有** RFC 8312 §4.2 的
TCP-friendly 公式（只以 Reno 速率近似），没有 HyStart / TCP-AQ / DCTCP / Prague，
没有 ECN，`W_max` 不跨 pcb 持久化。

### 运行时控制面：`/proc/a20/netfilter` 的 conntrack 与 NAT 动词

与 `/proc/net/config` 分开，因为写者不同：配置面只有 `/proc`，而 conntrack 表每包
都要写、由 `g_lwip_lock` 保护。锁理由写在
[network-lock-contract.md](network-lock-contract.md)「conntrack 与 NAT 用哪把锁」。

| 写入 | 作用 |
|---|---|
| `ctflush` | 丢掉全部活跃流状态，保留规则 |
| `cton` / `ctoff` | 开关 conntrack 插入。`ctoff` 只停**新**条目，保留既有条目 |
| `natadd <rule>` | 加一条 NAT 规则（`snat` / `dnat` / `masquerade`） |
| `natdel <idx>` | 按索引删一条 NAT 规则 |
| `reset` / `flush` | `reset` 清配置与 NAT 规则，`flush` 只清 drop/accept 规则 |

NAT 规则的语法、连接跟踪表的结构、以及**诚实的边界**（无 ALG、无 ICMP 跟踪、不做
分片 NAT 等）写在 [conntrack-nat.md](conntrack-nat.md)，不重复于此。

### 套接字选项：这三个现在不再是无操作

| 选项 | 之前 | 现在 |
|---|---|---|
| `TCP_CONGESTION` | 接受 `"reno"`，把 `"cubic"` 也当成功——而本树**没有** CUBIC | 未知名返回 `-ENOPROTOOPT`；`getsockopt` 回真实算法名；`"cubic"` 走 RFC 8312 核心条款 |
| `SO_SNDBUF` | 被接受然后忽略 | 直接写 pcb 的 `snd_buf`，约束该 socket 能排进 lwIP 的字节数；`getsockopt` 回读夹紧后的生效值 |
| `SO_RCVBUF` | 被接受然后忽略 | 经 pcb 的 `wnd_limit` 字段约束该连接的接收窗口；`getsockopt` 回读生效值 |

**必须一起说的限制**，否则名字会骗人（理由写在
`kernel/net/socket_inet.c` 的 `net_inet_tcp_buf_apply()` 函数头注释里，lwIP 侧的
分歧登记在 `kernel/external/lwip/DIVERGENCE.md` §2.10）：

- `SO_SNDBUF` 只约束**已排队未确认**的字节，**不约束在途飞行字节**——在途由拥塞控制
  （cwnd / `pcb->cwnd`）负责，在这里也管它会与算法对着干。抬高位子不会放宽 cwnd。
- **抬高对既有连接立即生效。** ceiling 双向写进 `pcb->snd_buf`：lwIP 自己没有在途
  增长 `snd_buf` 的接口，本轮**发明**了这个写入（`socket_inet.c` 中 `net_inet_tcp_buf_apply()`
  里那一句无条件赋值），并把依赖的不变量登记进 DIVERGENCE §2.10。下调不会凭空缩掉
  已经排进 lwIP 的数据：已排队的字节要等 ACK 回来才让出新的可用空间。
- **两者都没有自动调优。** 没有 `tcp_wmem` / `tcp_rmem`，没有内存压力反馈，也不从
  实测吞吐调整。Linux 会据此增长 `sk_sndbuf` / `sk_rcvbuf`，本树不会；依赖那种增长的
  调用方拿不到。
- 这两个选项对 **UDP/RAW 也接受**，语义与 TCP 不同，见下一节。

### UDP/RAW 的 `SO_SNDBUF` / `SO_RCVBUF`：接受，但含义不同

之前 `setsockopt` 对 `SOCK_DGRAM` / `SOCK_RAW` 的这两个名字一律返回 `-EOPNOTSUPP`。
那在"诚实"的意义上是对的（数据报套接字没有 pcb），但它把一个**真实存在**的缓冲区说成
不存在。现在数据报套接字接受这两个选项，`getsockopt` 回读**生效值**（超上限会被夹到
上限，不是回显请求），并且真的被执行：

| 选项 | 数据报上的含义 | 生效点 |
|---|---|---|
| `SO_SNDBUF` | 这个套接字愿意交出去**单个**数据报的大小 | `net_inet_send_udp()` / `net_inet_send_raw()`：超过返回 `-EMSGSIZE` |
| `SO_RCVBUF` | 这个套接字**接收队列**里能排的字节总数 | `net_rxq_fits()`（`socket_queue.c`）：装不下就丢这个数据报（`-EAGAIN`，与队列满同义） |

默认值写在 `socket_internal.h` 的 `NET_DGRAM_SND_BUF_DEFAULT` /
`NET_DGRAM_RCV_BUF_DEFAULT`：**发送侧是 `NET_MAX_PAYLOAD`**（socket 层一次能暂存的
最大数据报），**接收侧是 `NET_MAX_QUEUE * NET_MAX_PAYLOAD`**——也就是既有条数上限换算
成字节。选择后者当默认值是有意的：它让"没设置过"与本轮之前**逐字节等价**，字节检查只对
显式设小了容量的套接字生效，而不是变成一个以后以"莫名其妙丢数据报"形式出现的行为变更。

**与 TCP 的语义差异（不要按 Linux 推断）**：

- **没有窗口缩放折算。** TCP 侧的接收上限要取 `min(TCP_WND, 0xFFFF << TCP_RCV_SCALE)`，
  因为线上窗口字段是 16 位按 `rcv_scale` 右移的；数据报没有窗口、没有缩放字段，也就没有
  这一折算，上限直接是本层的队列容量。
- **不约束在途字节。** UDP 发送不排队（pbuf 交给 `udp_sendto()` 就被释放），所以发送侧
  唯一能约束的是单个数据报；这也意味着**它不是** `sk_wmem_alloc` 的对应物。
- **接收侧是丢包，不是反压。** TCP 侧靠 `wnd_limit` 停止接收窗口的扩张，让对端慢下来；
  数据报侧只能丢弃——并且**不会**合成 ICMP port unreachable（与本路径既有的"队列满就
  丢"是同一条边界）。
- **同样没有自动调优**，理由与 TCP 侧相同。

### RTNETLINK 多播组

`NETLINK_ROUTE` 套接字 bind 时可以选组（`kernel/net/socket_netlink.c`）：

| 组 | 值 | 收到什么 | 事件源（写入路径） |
|---|---|---|---|
| `RTNLGRP_LINK` | `0x1` | `RTM_NEWLINK`：链路状态变化 | `a20_lwip_sync_link_state()` |
| `RTNLGRP_IPV4_IFADDR` | `0x5` | `RTM_NEWADDR` / `RTM_DELADDR` | `a20_lwip_if_set_addr()` |
| `RTNLGRP_IPV6_IFADDR` | `0xA` | `RTM_NEWADDR` | `a20_lwip_if_set_addr6()` |

**表里第三列是本轮新增 `RTNLGRP_IPV6_IFADDR` 的理由**：先有写入路径，才有组。只定义一个
组号而没有任何内核代码能往里投递，等于给监听者一个可以 bind 却永远收不到东西的承诺——
那比不定义更糟。所以本轮同时补上了 IPv6 地址写入路径
（`a20_lwip_if_set_addr6()`，`kernel/net/lwip_stack.c`），`nlrt_apply_addr6()` 把
`ifa_family == AF_INET6` 的 `RTM_NEWADDR` 接到它上面，`net_netlink_addr6_notify()` 负责
投递到 `RTNLGRP_IPV6_IFADDR`。`netlink_test` 的 IPv6 那一节就是钉这一条的：
它 bind 上这个组、真的收一条事件、并断言 `IFA_ADDRESS` / `IFA_LOCAL` 是 16 字节。

IPv4 地址全零时发 `RTM_DELADDR` 而不是 `RTM_NEWADDR`——"你现在有地址 0.0.0.0"不是任何
接口处于的状态，没有监听者能对它采取行动（实现见 `net_netlink_addr_notify()`）。

**IPv6 写入路径的边界（必须一起读）**：

- **只有加，没有删。** `LWIP_NETIF_API=0` 下 lwIP 不导出
  `netif_remove_ip6_addr()`，删除要走 `nd6.c` 内部地址池的拆除流程，从 socket 层伸手进去
  是一项真实的分歧，而树里目前没有任何东西需要它。所以 `RTM_DELADDR` +
  `AF_INET6` **显式返回 `-EOPNOTSUPP`**，而不是接受之后改发一条 `RTM_NEWADDR` 去谎报
  "已删除"一个其实还在的地址。
- **不做 DAD。** `netif_add_ip6_address()` 会把地址置为 `TENTATIVE` 等 ND6 定时器探测后
  提升；本路径直接置 `IP6_ADDR_VALID`，因为地址来自一条显式的管理请求而不是路由器通告，
  且回环 netif 已经走同一条捷径（`a20_lwip_loopif_init_cb()`）。**监听者不得把这个事件
  读成"重复地址检测通过"。**
- **`ifa_prefixlen` 只校验、不落地。** lwIP 的 IPv6 子网成员关系编在地址自身里
  （`ip6_addr_t`），`struct netif` 没有每地址的前缀长度字段。它被校验（>128 报
  `-EINVAL`），并且是通知里回报的那个数。
- **回环的 `::1` 不产生事件。** 它在 netif 初始化时由 `a20_lwip_loopif_init_cb()` 加入，
  早于任何 netlink 监听者存在，不是"变化"。
- ND6 自己从路由器通告学到的地址（本树开着 `LWIP_IPV6_AUTOCONFIG`）**不会**发通知：
  lwIP 没有给出这个事件的钩子。**已知会漏的事件**。

一个监听者的接收队列满不会挡住其他监听者——Linux 在这里丢消息并报溢出，本树没有
socket 级的溢出上报面，所以计数并 `klog` 一条
（`[RTNETLINK] multicast group 0x%x dropped for %d listener(s)`）。

### 观测面：`/proc/net/stats` 的本轮新增行

| 行 | 何时出现 | 含义 |
|---|---|---|
| `lwip_lock: armed=… owner=… violations=… sites=…` | `CONFIG_NET_LOCK_ASSERT=1` | 核心锁断言探针状态。关闭时**显式**打印 `not checked (CONFIG_NET_LOCK_ASSERT=0)` |
| `lwip_lock_siteN: <addr>` | 同上，且探针触发过 | 违约点（宏展开处的返回地址） |
| `…[sg_tx=N sg_tx_bytes=M]` | 总是 | 走 `send_sg()` 描述符路径的发送帧数与字节 |
| `…[tx_csum_offload:on/off][mrg_rxbuf:on/off]` | 总是 | 驱动上报的能力位 |

`owner=4294967295` 表示采样时锁空闲；`sites=0` 表示探针一次都没触发。
**`violations=0` 在开关关闭时是没有意义的**——那行 `not checked` 就是为了不让人
把它读成"没问题"。细节见
[network-lock-contract.md](network-lock-contract.md)「核心锁断言」。

`/proc/net/` 下另有 `tcp6` 与 `udp6` 两个文件（`procfs.c:464-469`），按 v6 的四字布局
渲染，与 `tcp` / `udp` 分开，v4 文件不再混入 v6 行。这一对文件是 `c34ddd7f8`
（AF_INET6 socket 真正拿到 pcb）带进来的——在那之前 v6 的 bind/listen/accept 走不通，
把它们渲染出来只会显示一张空表，把"v6 入站不可达"这件事藏起来。

**`[tx_csum_offload:off]` 与 `[rx_csum_offload:off]` 是设计结果，不是没做完。**
两个能力位已定义（`kernel/drivers/core/driver_class.h:145-146`）但**永不置位**：
vendored 的 lwIP 2.2.2 没有任何承载校验和卸载握手的 flag（`opt.h:2449-2450` 的
`LWIP_CHECKSUM_ON_COPY` 默认 0，所以 `pbuf_take()` 自己算自己验；
`netif.h:84-107` 的七个 `NETIF_FLAG_*` 里没有一个是给校验和握手用的）。贸然置位会让
lwIP 去验一个设备根本没算的校验和，属于静默损坏。`MRG_RXBUF` 不同：它是纯设备侧的
接收属性，lwIP 从来看不见，所以协商了的驱动**应该**上报它（virtio-net 就是这样）。

## 用户命令消费方式

以下用户命令消费运行时配置。

### `wget`

- 移除硬编码的 `DNS_SERVER_IP` 常量。
- 从 `/proc/net/config` 读取第一个 DNS server。
- 如果没有配置 DNS server，`wget` 在发送任何查询前打印错误并退出。
- 其他行为保持不变，包括 URL 解析、TCP 连接、TLS 握手和 HTTP 获取。

### `ping`

- 移除硬编码的 `DNS_SERVER_IP` 常量。
- 当参数是 hostname 时，从 `/proc/net/config` 读取第一个 DNS server。
- 如果参数已经是 IPv4 字面量，不执行 DNS 查询。
- 如果没有配置 DNS server 且参数是 hostname，`ping` 打印错误并退出。

### `udpsend`

- `udpsend` 通过命令行参数接收目标 IPv4 地址和端口，因此不需要 DNS 查询。
- 它仍依赖接口已配置。如果接口没有地址或路由，内核 `sendto()` 路径返回 `-ENETUNREACH`。
- `udpsend` 未来可以增加可选 `-i` 参数，用于打印 `/proc/net/config` 中的当前网络配置，但默认行为保持不变。

## DHCP 与锁的交互

DHCP 作为 lwIP timeout 处理的一部分运行。它在更新 netif 地址和 DNS server 状态时持有 `g_lwip_lock`。为避免给用户态造成意外，DHCP 产生的地址变化会在持有 `g_lwip_lock` 时同时提交到 lwIP netif 和运行时 `a20_net_config` 结构体。

用户态读取 `/proc/net/config` 时，`a20_net_config_format()` 会短暂获取 `g_lwip_lock`，先把默认 netif 和 DNS 状态同步到 `g_a20_net_config`，释放锁后再格式化文本。读取不会与 DHCP 更新并发撕裂 lwIP 状态，但它不是无锁快照。

## 迁移检查清单

实现该设计时，逐项确认（该设计已实现，`smoke-network-suite` 覆盖）：

- [x] `kernel/net/lwip_stack.c` 不再包含硬编码 QEMU 地址。
- [x] `a20_lwip_register_netifs()` 从 `a20_net_config` 读取配置。
- [x] 命令行解析器识别全部六个 `a20.*` 键。
- [x] `a20.dhcp=1` 触发 DHCP 并覆盖静态值。
- [x] `/proc/net/config` 暴露生效配置。
- [x] `user/cmds/net/wget.c` 从 `/proc/net/config` 读取 DNS。
- [x] `user/cmds/net/ping.c` 从 `/proc/net/config` 读取 DNS。
- [x] `user/cmds/net/udpsend.c` 能容忍未配置状态下的 `-ENETUNREACH`。
- [x] 需要 NAT 配置的部分 QEMU run/smoke 目标传入 `a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os`；两个 AArch64 板级目标使用上述编译期 bootargs。
- [x] 网络 smoke 测试覆盖已配置和未配置两种启动路径。

本轮（`feat/net-strengthening`）新增，逐项对应上文「配置面的三个平面」：

- [x] SACK / 时间戳 / CUBIC 按档位在 `net_profile.h` 里取值，EMBEDDED 关、
      DEFAULT/SERVER 开。
- [x] 带选项的 TCP 头对 MSS 的预算由 `lwipopts.h` 的 `_Static_assert` 钉住，
      断言检查的是加了选项之后的和。
- [x] `SO_SNDBUF` / `SO_RCVBUF` 真的约束发送队列深度与 pcb 接收窗口，
      `getsockopt` 回读生效值；抬高对既有连接立即生效（写 `pcb->snd_buf`，
      DIVERGENCE §2.10）；无自动调优这一点已写进源码注释与本文。
- [x] `TCP_CONGESTION` 对未知名返回 `-ENOPROTOOPT`，`getsockopt` 回真实算法名。
- [x] RTNETLINK 向 `RTNLGRP_LINK` / `RTNLGRP_IPV4_IFADDR` / `RTNLGRP_IPV6_IFADDR`
      投递 `RTM_NEWLINK` / `RTM_NEWADDR`（IPv4 地址全零时改发 `RTM_DELADDR`）；三个组
      都有真实写入路径，IPv6 的边界见上面那一节。
- [x] conntrack 与 NAT 的运行时动词挂在 `/proc/a20/netfilter`，并有
      `make smoke-netfilter-nat` 端到端门禁覆盖 DNAT。
- [x] UDP / RAW 的 `SO_SNDBUF` / `SO_RCVBUF` 被接受并真正执行：单数据报发送上限与
      接收队列字节上限，`getsockopt` 回读生效值，语义差异见上面那一节。
- [ ] **未做**：套接字缓冲无自动调优（无 `tcp_wmem` / `tcp_rmem`、无内存压力反馈、
      不从实测吞吐调整）；IPv6 地址只有加没有删、不做 DAD、ND6 自学地址不发通知；
      conntrack 不跟踪 ICMP。
- [ ] **未做**：`a20.tcpmode` 运行时写入口在 fast↔lwip 切换后数据传输不完成的
      现象，本轮一次都没复现，原因未查明。功能未削减，建议（部署用命令行键）保留。
