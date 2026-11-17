# 网络当前状态

> 仍未完成的事项见 [1.0 缺口台账](../gaps/README.md)；本页只描述当前状态。锁契约与并发边界见 [net/network-lock-contract.md](../../net/network-lock-contract.md)，配置面见 [net/network-config-design.md](../../net/network-config-design.md)， NAT 形态见 [net/conntrack-nat.md](../../net/conntrack-nat.md)。

## 接口与实例选择

- 协议栈接口统一为 `kernel/include/net/backend.h` 的模块化契约：`cycle/event/socket/opts/endpoint/iface/packet/obs` 八个模块，能力位表达 Linux ABI 可观察行为。旧的 `net_stack_ops_t` / `net_transport_ops_t` 已移除，`net/transport.h` 仅作兼容 include。
- 前端只经 `net/stack.h` facade 调用后端；`kernel/net/core/stack.c` 是唯一解引用 `net_backend_t` 的地方，`kernel/net/core/transport.c` 是唯一映射 socket 模块的地方。
- 后端由实例 TOML 的 `[net].stack` 选择：`tools/a20 build <instance>` 与`make NET_INSTANCE=<instance>` 都转换为 `NET_STACK`，构建只链接选中的后端，启动按名字选择且不回退。字段与校验见[net/instance-config.md](../../net/instance-config.md)。
- `/proc/net/status` 增加 `backend: <name>` 后的能力行（如`lwip: stream,dgram,...`），用于确认实际后端与能力，而不是只信构建参数。
- BSD 的资源上限改用共享的 `NET_PROFILE_*`（NIC 数与 RX 预算），不再维护`BSD_MAX_NICS` 之类的私有档位副本。
- 数据面快路径：lwIP 单 lane 收包直接写入 pbuf（去掉 rx_frame→pbuf_take 拷贝）；无 netfilter/NAT/conntrack 时跳过 Rust 解析和状态机；无策略且校验和已完成时 TX 直接借用 pbuf 发送。多 lane 仍保留 staging 拷贝，属于后续工作。
- conntrack 改为懒启用：没有规则时不为普通流建表，见 [net/nat.md](../../net/nat.md#懒启用机制)。
- F-Stack/BSD 与 lwIP 共用 socket 前端与策略层：raw、pktinfo、UDP 控制、 PMTU、congestion、IPv6 地址、单 NIC `SO_BINDTODEVICE` 以及 netfilter/NAT 桥均已接入；TUN/router/外部 IPv6 仍不宣称。

## 锁与 lane

- 普通单播 IPv4/IPv6 TCP/UDP 热路径只取所属 lane 锁；`g_lwip_lock` 仅作为设备 ingress/staging 门和 control barrier 的外层门，出现在冷路径、中断入口和配置事务。
- TCP 的 `tcp_active_pcbs` / `tcp_bound_pcbs` / `tcp_listen_pcbs` / `tcp_tw_pcbs` 按 lane 分桶；`CONFIG_NET_PCB_SANE=1` 时 `tcp_pcbs_sane()` 检查 bucket、`lane` 与 addr/port 一致性。UDP 没有独立 sane 函数，等价断言在 `udp_input()` 的每个 pcb 上就地执行。
- 运行时 panic 边界：lane→control 升级、lane 递归、非 owner 释放、lane 队列误用、 shared guard 误释放。静态门禁 `tools/net/check_lwip_concurrency_wiring.py`（`make check-honesty-policy` 的 `lwip-concurrency-wiring`）检查 filter 顺序、IRQ staging、owner-lane drain 和 accept 发布顺序（C 包装 → Rust `a20_net_accept_publish`）。
- 有意保留的全局面：ARP/ND6 cache、conntrack 表、UDP autobind、TCP ISS 共用一个`NULL`-key shared shard；loopback 在 control barrier 下交付；e1000/rtl8139 的 IRQ 仍取完整 barrier（驱动 owned，已报告给 drivers），virtio 走 ingress lock。

## 套接字缓冲

- `SO_SNDBUF` / `SO_RCVBUF` 真正约束 pcb 发送队列与接收窗口，`getsockopt` 回读生效值。
- 发送侧有界自动调优：DEFAULT/SERVER 初始 32 KiB，窗口耗尽时倍增到 `TCP_SND_BUF`（`pcb->snd_buf` 可用空间），显式 `setsockopt` 关闭该方向。
- 接收侧默认就是协议上限 `TCP_WND`，因此没有可增长空间；`wnd_limit` 仍是显式`SO_RCVBUF` 的软上限：`tcp_recved()` 不会把已经通告的窗口缩小（避免丢掉对端已获准发送的数据），握手前则直接把 `rcv_wnd`/`rcv_ann_wnd` 写成请求值。
- 窗口缩放 `LWIP_WND_SCALE=1`、`TCP_RCV_SCALE=3` 已开启；协议上限不再是 64 KiB。

## IPv6

- `RTM_NEWADDR` / `RTM_DELADDR` + `AF_INET6` 都实现；删除走`netif_ip6_addr_set_state(..., IP6_ADDR_INVALID)`，不存在的地址返回`-EADDRNOTAVAIL`。事件发到 `RTNLGRP_IPV6_IFADDR`（NEWADDR / DELADDR）。
- DAD 契约：管理写入直接置 `IP6_ADDR_VALID`，事件不表示"DAD 通过"，也不补发 DAD 完成事件；链路本地/SLAAC 仍由 `nd6_tmr()` 做 DAD。
- `ifa_prefixlen` 只校验、不落地；DELADDR 事件回报 0（与 IPv4 删除一致）。
- ND6 从 RA 学到的地址以及生命周期转换**不发通知**：`LWIP_NETIF_EXT_STATUS_CALLBACK`在本树是 0，已知边界。管理路径是唯一事件源。

## pbuf 与分档预算

| 档位 | `MEM_SIZE` | pbuf 池 | 池上限断言 |
|---|---|---|---|
| EMBEDDED | 16 KiB | 10 × 512 B | 16,224 B 池声明 ≤ 16,384 B |
| DEFAULT | 1 MiB | 256 × 1600 B | 771,192 B ≤ 1,048,576 B |
| SERVER | 16 MiB | 4096 × 1600 B | 12,510,840 B ≤ 16,777,216 B |

- 三个档位都有 `NET_PROFILE_MEMP_CLAIM_BYTES <= NET_PROFILE_MEM_SIZE`（含每元素 24 B 堆头和空闲链终止符），由 `net_profile.h` 的 `_Static_assert` 检查。
- DEFAULT 的 `MEM_SIZE` 从 512 KiB 调整到 1 MiB：按真实元素尺寸，原值装不下自己声明的池上限（约 771 KiB）。
- EMBEDDED 的 44 KiB 总预算按实测项重新对账：socket 表、ring + drain slot、netif state、两个 `struct netif`、filter 表、heap；LP64 约 43,796 B，ILP32 约 41,364 B。
- `/proc/a20/netmem` 打印池的 `elem/used/max/err`、pbuf 池上限和 heap `size/used/avail/err`，用于区分"池受限"与"堆受限"。
- 零拷贝收包是设计边界：设备 recv op 只提供"拷进调用者缓冲"，真正零拷贝需要改`kernel/drivers` 的接口；lane ring 槽位复用，包装成 custom pbuf 会在下一帧到达时踩到仍被 lwIP 持有的 pbuf。

## lwIP conntrack / NAT

- 五元组 FNV-1a 表（32/256/1024 条）、NEW/ESTABLISHED、按状态超时、满表 LRU、 SNAT/MASQUERADE（output）与 DNAT（input）、RFC 1624 增量校验和、ICMP echo 跟踪。
- 规则方向与 hook 匹配：`dnat` 只能 `in`，`snat`/`masquerade` 只能 `out`，否则 `-EINVAL`。
- 设计边界：无 ALG；不跟踪 ICMP 差错报文（vendored lwIP 的 `icmp_input()` 也丢弃入站差错，PMTU 因此不可用）；分片只翻译首片、非首片放行；无 IPv6 NAT；无动态端口分配。
- 端到端门禁：`smoke-netfilter-nat`（DNAT/TCP，hostfwd）与`smoke-netfilter-snat`（出站 SNAT/TCP，宿主回显 + 返回路径 + 删规则阴性对照）。

## lwIP 真实 netif 与外部对端

- `smoke-net-lanes-hostfwd`：4 lane、4 条并行 64 KiB TCP 流，双向 digest，宿主探针校验返回字节；`smoke-net-tcp-lanes`：8 个真实 LISTEN pcb，`CONFIG_NET_PCB_SANE=1`。
- `smoke-net-udp-hostfwd`：宿主经 QEMU `hostfwd=udp` 发送 16 个带序号的 512 B 数据报， guest 校验后异或回显，宿主逐字节验证；覆盖真实 virtio-net 的双向 UDP。
- `smoke-network-suite`（单 CPU 与 `-smp2`）与 `smoke-netopt-lwip` 覆盖本地 fast path、套接字选项和 bottom-half；它们**不是**外部数据面。
- `smoke-ipv6-rx-soak`：`CONFIG_LWIP_MEMP_OVERFLOW_CHECK=1` 下 4,000 轮 UDP6 （64 B ~ 16 KB，跨越单 pbuf 边界），用于 NET-09 的池引用/越界回归。
- 未覆盖：IPv6 外部 peer、ICMP 差错/PMTU、e1000/rtl8139 的多 lane 并发。

## BSD 后端

x86_64 桌面和服务器/基准主机默认使用 BSD/F-Stack，非 x86_64 架构仍使用 lwIP。已有真实 netif 的 DNS、TCP/UDP 入出站与路由配置记录；较新的 [路由验收](../../net/router-completion.md) 还覆盖 DHCP、外部 IPv6/RA 与前缀委派、TUN/PPPoE、LAN DHCP/DNS、多客户端链路恢复，以及两台 Linux 客户端的 1800 秒 TCP/UDP/ICMP 负载。已有 e1000e MSI-X 配置下的 QEMU UEFI 路由流量记录，不能继续笼统标为“无流量”。

这些结果不等于全部设备、配置、关闭竞态或性能指标通过。物理双网口与完整实机认证仍未完成，按当前范围安排到后续；保留已有网络回归和已知失败记录。原始统计、超时/丢包、镜像与命令见路由验收页，本次未重跑该运行矩阵。

## 常用命令

```sh
make smoke-network-suite
make smoke-network-suite-smp2
make smoke-netopt-lwip
make smoke-net-tcp-lanes
make smoke-net-lanes-hostfwd
make smoke-net-udp-hostfwd
make smoke-netfilter-nat
make smoke-netfilter-snat
make smoke-ipv6-rx-soak
make check-honesty-policy        # lwip-concurrency-wiring 静态门禁
```
