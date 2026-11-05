# IPv4 NAT 状态与使用边界

当前实现提供 TCP/UDP 的冲突检测及动态源端口分配、事务式绑定和独立同步。BSD 网卡数据路径支持重组后的 IPv4 分片 NAT。本机 socket 的完整 PMTU/差错队列仍处待实现状态。ICMP RELATED 翻译与路由器转发路径的 MTU 差错已接入并有端到端回归，见下文。跨网卡转发见 [router.md](./router.md)，LAN DHCPv4/DNS A 转发见 [lan-services.md](./lan-services.md)。

## 配置

接口和地址配置后，可由管理员写入现有控制入口，例如：

```sh
echo 'natadd out proto=tcp action=masquerade' > /proc/a20/netfilter
echo 'natadd out proto=udp action=masquerade' > /proc/a20/netfilter
cat /proc/a20/netfilter
```

MASQUERADE 使用实际出口的地址；出口没有地址时拒绝创建映射，不能借用其他网卡的地址。SNAT 可通过 `action=snat to=<地址>` 指定源地址，`toport=<端口>` 固定端口。不指定固定端口时，先保留客户端端口；完整回复元组冲突时，在 1024–65535 中探测。不同远端可以复用外部端口，同一远端的回复元组必须只有一个所有者。固定端口冲突直接失败，不擅自改成其他端口。ICMP echo 目前仅做地址变换，冲突时拒绝，尚无标识符分配。

`natdel <索引>` 删除规则，已有绑定继续工作。`reset` 清理规则，`ctflush` 显式丢弃连接状态，后者会破坏依赖这些绑定的连接。`ctoff` 停止建立新状态，保留已有状态；匹配 NAT 的新流会被拒绝，不允许悄悄绕过 NAT 策略。满表返回 ENOSPC 并丢弃报文，不驱逐活跃连接。资源预算由 `net/profile.h` 管理。

## 事务、锁与内存

`filter/netfilter_nat.rs` 统一决定规则匹配、绑定分配、双索引、状态和超时。C 提供固定容量存储、IRQ-safe CT 锁、时钟和原位重写。每个 Rust 入口用 RAII guard 持锁，配置、查询、包处理和回收都遵循同一同步方式；不再依赖 lwIP 全局锁或重试后跳过规则。共享表项以值副本读写，不制造可重叠的 `&'static mut` 引用。

锁顺序为后端执行锁/配置写锁 → CT 锁；CT 内不能反向取得后端锁或 socket 锁。不在事务中分配内存、休眠或转移包所有权。当前有界表使用一个 CT 锁，便于保证元组预留与索引更新原子性；未来是否分片必须由争用测量决定，不能以无保护共享表替代。

绑定先提交，随后才能修改报文；失败时保持报文不变，由 filter 丢弃。转发流可能先在 ingress 建立未转换状态，再在 egress 附加 SNAT；回复索引在同一事务中重建。反向映射只在正确方向执行，避免跨 ingress/egress 时重复转换。

超时统一为 32 位回绕毫秒，不能把高频原始 tick 截断后当作毫秒。默认 TCP NEW 30 秒、 ESTABLISHED 120 秒、UDP/echo 30 秒；活跃流刷新时间，闲置流由有界 sweeper 回收。这些是当前状态跟踪超时，不等同于完整 TCP 状态机或抗攻击策略。

## BSD 连接表预算

`NET_ROLE=router NET_STACK=bsd` 默认分配 4096 条。用
`NET_CONNTRACK_ENTRIES` 或实例的 `[net].conntrack_entries` 选择 256 至
16384 的 2 的幂。该容量随内核构建固定。连接表与两组哈希索引的占用
显示在 `/proc/a20/netfilter` 的 `ct_storage_bytes`。

新建连接从空闲链表取槽位，满表时保留已有映射并拒绝新连接。
BSD 执行器每约 50 毫秒检查最多 256 个槽位，过期后回收原正向和回包索引，
将槽位放回空闲链表。以此节奏，最大表每约 3.2 秒巡完一轮。

双网口回归用真实 UDP 流两次填满连接表，中间验证满表拒绝、
已有流回包归属以及没有入站流量时的超时回收。使用同一构建方式：

```sh
make ARCH=x86_64 NET_STACK=bsd NET_ROLE=router NET_CONNTRACK_ENTRIES=16384 \
  NR_CPUS=2 DRIVER_DEPLOYMENT=embedded RAMFS_USER=1 RAMFS_NET_TESTS=1 kernel-only
python3 tools/net/tests/test-bsd-router-capacity.py --capacity 16384 \
  --kernel <生成的kernel.bin> --out <新证据目录>
```

## 验证与未覆盖项

```sh
bash tools/tests/netfilter_nat_host/run.sh "$PWD/build/evidence/<新目录>"
bash tools/tests/netfilter_filter_host/run.sh "$PWD/build/evidence/<另一新目录>"
sh tools/host-tests/test-nat-rewrite-host.sh c
sh tools/host-tests/test-nat-rewrite-host.sh rust
python3 tools/net/check-net-pcap.py <完整、已完成软件校验的Ethernet抓包>
```

生产状态机测试覆盖两客户端同端口冲突、回包归属、重传、转发两阶段、固定端口冲突、满表不变换、不驱逐、关闭 tracking、4 线程竞争，以及 1 kHz/2 GHz 时钟与毫秒回绕。重写测试独立重算校验和：TCP 的零值仍是合法计算结果，必须更新；IPv4 UDP 的原始零值表示未启用校验，但变换计算出的零必须编码成 0xffff。

三节点 KVM 测试已覆盖双网卡拓扑及双客户端同源端口冲突。未覆盖项包括组合 DNAT+SNAT/hairpin、本机 socket PMTU/差错队列、完整 TCP 状态检查、独立网络域以及与本地 socket 端口分配器的统一预留，部署配置需遵循上述功能边界约束。

## ICMP RELATED 与转发路径 MTU

Rust CT 事务已处理 IPv4 Destination Unreachable、Time Exceeded 和 Parameter Problem，包括 type3/code4 的下一跳 MTU。匹配现存、未超时的 SNAT/MASQUERADE/DNAT 绑定，按正确方向恢复外层和引用报文的地址/端口，更新引用 IP、可见的 UDP/TCP 校验和以及外层 ICMP/IP 校验和。最短 TCP 引用只有8字节，不要求不存在的完整 TCP 头；UDP 的零校验和保持禁用语义。不会为差错新建连接、刷新超时或把 TCP 改成 ESTABLISHED。

修改前检查长度、外层 IP/ICMP 与引用 IP 校验和、引用源与外层目的的一致性，以及错误类型/代码、方向与活跃元组。截断或无效报文返回失败且不修改原字节。实现依据 [RFC 5508](https://www.rfc-editor.org/rfc/rfc5508.html) 与[RFC 1624](https://www.rfc-editor.org/rfc/rfc1624.html)；当前不宣称覆盖所有 NAT 行为要求。

原版/衍生适配通过输入 hook 在协议转发前处理 TTL=1 和 DF 超出口 MTU，生成带正确 next-hop MTU 的差错；引用尽可能保留至576字节 IPv4 响应上限，具备全局有界速率额度，不回应 ICMP 差错、组播/广播及不合法来源。上游源码保持未修改。IPv6 转发差错仍由后端本身生成。路由器正确返回/转发 MTU 差错，不等于 A20OS 本机已支持 Linux 的全部`IP_RECVERR`、每 socket PMTU 策略和缓存。

默认/embedded/2GHz 生产 C+Rust 测试覆盖 SNAT/DNAT、TCP/UDP、最短/较长引用、三类差错、校验和、MTU保留以及每个截断前缀。三节点 KVM 的 Linux LAN 使用同一 ELF 和真实 MSG_ERRQUEUE，检查 socket 元组、原始数据、错误类型/代码和1280字节下一跳 MTU。 BSD 的网卡与 TUN IPv4 输入和输出经过公共过滤/NAT。

## IPv4 分片

BSD 网卡入口校验 IPv4 首部后，在本地投递或路由查找前重组。队列按入口、
FIB、源地址、目的地址、协议和标识隔离。重叠或冲突终点销毁原队列。
队列使用独立 mbuf，释放网卡 RX token 后再持有分片。沿用 F-Stack 的全局
配额和约 30 秒超时，每个报文最多等待 64 个分片。

过滤和 conntrack 处理完整报文。出口在 F-Stack 按 MTU 分片前完成 NAT。
AF_PACKET 仍看到入口的原始 Ethernet 分片。lwIP/native 的帧过滤路径
在有 NAT 规则时继续整组拒绝分片，避免只转换首片。

双网口 QEMU 回归检查乱序 SNAT/DNAT 及回包、过滤、重叠、配额、超时、
跨接口队列隔离与出口校验和。测试拓扑使用本地 UDP 承载 Ethernet：

```sh
make ARCH=x86_64 NET_STACK=bsd NET_ROLE=router NR_CPUS=2 \
  DRIVER_DEPLOYMENT=embedded RAMFS_USER=1 RAMFS_NET_TESTS=1 kernel-only
python3 tools/net/tests/test-bsd-router-fragments.py \
  --kernel <生成的kernel.bin> --out <新证据目录>
```

## 懒启用机制

conntrack/NAT 不再为“没有配置任何规则”的普通流量建立表项。`netfilter_busy()`在包路径上只做一次原子读；只有存在过滤规则、NAT 规则或仍存活的 conntrack 表项时才进入解析、状态机和改写。配置路径（增删规则、flush、expire、inject）负责刷新该标志。

可观察变化：未配置规则时 `/proc/a20/netfilter` 的 conntrack 计数保持 0，而旧实现会为每条 TCP/UDP 流建表。若需要从第一条流开始跟踪，写 `cton`（显式强制跟踪）或先写入任一条规则；规则删除后已有表项仍会保留到超时，期间继续按 NAT 语义处理。`ctoff` 停止建立新表项，但不拆除已有绑定。
