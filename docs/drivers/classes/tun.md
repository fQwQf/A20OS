# TUN 设备形态（A20OS 混合内核）

Linux ABI 的 `/dev/net/tun` 是一个**内核服务型字符设备**：字符设备前端
负责 TUN ABI，真正的网络对象是一个由 lwIP 消费的虚拟 netif。本文先确定
它在 A20OS 驱动模型中的位置，再给出实现与验收边界。

## 1. 形态判断：内核服务，而不是 `.a20drv` 模块

A20OS 有两类驱动形态：

| 形态 | 适用条件 | 例子 |
|---|---|---|
| `.a20drv` 运行时模块（DriverStore） | 可在总线上发现、有 probe/match、部署位置可迁移 | virtio-net、ahci、virtio-gpu |
| 内核服务（`GENERIC_KERNEL_SERVICE_SRCS`） | 无硬件可发现、是内核 ABI/子系统的组成部分 | `pty.c`、`block/loop.c`、`block/udisk.c` |

TUN 全部满足第二列的判据，不满足第一列：

* 没有总线、没有设备树条目、没有 probe/match —— 实例由
  `open("/dev/net/tun")` + `TUNSETIFF` 创建，属于**用户请求的对象**，不是
  启动时枚举出来的硬件。
* 数据面在核内：一端是 lwIP 的 netif 输入/输出，另一端是字符设备队列。
  把它做成用户态模块只会把同一份 lwIP 内核符号导出面扩大，换不到任何
  硬件解耦收益（和 virtio-net 这种"硬件在门外"的驱动正好相反）。
* 它是网络 ABI 的组成部分。做成 DriverStore 模块会让 distro 镜像多一个
  "没装驱动就没有 `/dev/net/tun`" 的失败模式，而 Linux 程序（tailscale、
  OpenVPN、wireguard-go）默认它存在。

因此落点是 `kernel/drivers/net/tun.c`，加入 `GENERIC_KERNEL_SERVICE_SRCS`，
devfs 增加**固定节点** `/dev/net/tun`（与 `/dev/ptmx` 同一类：固定服务节点，
不是动态 class 节点）。netif 一侧与 `g_loopif` 同构：一个虚拟 netif，只是
对端从"回环到自身"换成"用户态 fd"。

## 2. 分层

```text
userspace: open/read/write/ioctl/poll
  -> devfs 固定节点 /dev/net/tun, vfile_ops
  -> tun instance (name, flags, mtu, queues, netif)
  -> lwIP netif  (output -> 实例队列)  /  ip4_input (write 注入)
  -> route table (RTM_NEWROUTE) -> LWIP_HOOK_IP4_ROUTE_SRC
```

* **前端**：`vfile_ops` 实现 TUN ABI。open 只分配实例；`TUNSETIFF` 才创建
  netif（Linux 语义：dummy 打开不产生接口）。
* **核心**：每打开实例一个 `tun_t`，拥有名字、flags、MTU、两个有界队列
  （内核→用户、用户→内核不排队，写入直接注入）。
* **后端**：`netif_add()` 挂进 lwIP；`netif->output` 把 pbuf 拷进实例队列；
  用户 write 的报文在 lwIP 锁内 `ip4_input()`。

## 3. 锁与背压

* `netif->output` 在 lwIP 锁内被调用，只做**有界入队 + 唤醒**，不阻塞、
  不分配大块内存；队列满则丢包并计数（对应 Linux 的 `tx_dropped`）。
* 用户 read 取出整包后拷出；队列空时阻塞在实例 wait queue 上（`poll_sources`
  同时给 poll/select/epoll 供源），非阻塞返回 `-EAGAIN`。
* 实例队列有独立自旋锁；lwIP 锁与实例锁的获取顺序固定为 lwIP 后实例，
  与现有 net lane 纪律一致。

## 4. 路由模型（TUN 模式 tailscale 的关键）

lwIP 原生只有"同网段/默认网关"选择，没有 Linux 的路由表；而 TUN 用户
（tailscale 的 router）通过 netlink 下发 `100.64.0.0/10 dev tunN`、exit node
的 `0.0.0.0/1`+`128.0.0.0/1` 等前缀路由。设计：

* 内核新增一张**小路由表**（`dst/prefixlen -> ifindex, metric`），由
  `RTM_NEWROUTE`/`RTM_DELROUTE` 维护，`RTM_NEWRULE` 在单表语义下接受为
  no-op（A20OS 没有多路由表/策略路由，明确不支持，而不是假装支持）。
* 通过 lwIP 的 `LWIP_HOOK_IP4_ROUTE_SRC(src, dest)` 查询：命中返回 netif，
  未命中返回 NULL，回落到 lwIP 原有逻辑。这样 tunnel 前缀走 tun，其余
  仍走物理网卡，不需要改 lwIP 内部。
* 地址仍走 `RTM_NEWADDR`（`apply_addr`），MTU/UP 走 `RTM_NEWLINK`——这两条
  路径内核已经有，只需保证按 ifindex 能命中 tun netif。

## 5. ABI 覆盖范围

实现（tailscale / wireguard-go / OpenVPN 的公共子集）：

| 项 | 说明 |
|---|---|
| `TUNSETIFF` / `TUNGETIFF` | `IFF_TUN`、`IFF_NO_PI`；名字缺省 `tunN` |
| `TUNGETFEATURES` | 如实报告 `IFF_TUN|IFF_NO_PI` |
| `SIOCSIFMTU` / `SIOCGIFMTU` | Linux TUN 用户在 TUNSETIFF 后用这个设 MTU |
| MTU / UP / ADDR / ROUTE | 经 netlink（已有写路径 + 新路由表） |
| `read`/`write`/`poll` | 单包语义，非阻塞 + 阻塞、丢包计数 |

明确不做（返回 `-EOPNOTSUPP`，不静默降级）：`IFF_TAP`（L2，需要以太网
netif 语义）、`IFF_MULTI_QUEUE`/`TUNSETQUEUE`、`TUNSETPERSIST`、
`TUNSETOWNER/GROUP`、vnet hdr（`TUNSETVNETHDRSZ` 仅接受 0）、IPv6 TUN。

## 6. netns

lwIP 只有一张全局 netif 表；非初始 netns 目前只有合成 loopback。
TUN 先在初始 netns 落地，其他 netns 的 `TUNSETIFF` 返回 `-EOPNOTSUPP`。
per-netns netif 需要单独设计（netif 所有权 + 路由表按 ns 分区），不在本次
范围内，文档里明确记录，避免"看起来支持"。

## 7. 验收

1. 结构：`/dev/net/tun` 打开 + `TUNSETIFF` 后，`net.Interfaces()`/`ip link`
   能看到 `tunN`（非 loopback、有 MTU、能 UP）。
2. 数据面：guest 内一个最小程序写入一个 IPv4 报文，从物理网卡发出；
   反向从物理口收到的报文进入 tun 的 read 队列。
3. 真实负载：`tailscaled --tun=tailscale0`（**不用**
   `--tun=userspace-networking`），`tailscale up` 后：
   * `tailscale ping <peer>` 直连 pong；
   * 透明 `curl http://<peer-ip>:9999/`（不经过 SOCKS）；
   * 宿主 `curl http://<guest-ip>:8000/` 命中 guest 服务（入站不再依赖
     127.0.0.1 转发）。
