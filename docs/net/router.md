# Router 构建与三节点验证

`NET_ROLE=host` 是默认值，不转发经过本机的 IPv4/IPv6 报文。`NET_ROLE=router` 显式编入跨接口转发能力，产物目录与 host 隔离：

```sh
make ARCH=x86_64 NR_CPUS=2 NET_STACK=bsd NET_LANES=1 NET_ROLE=router kernel-only
```

这项选择和 `NET_STACK`、`NET_PROFILE` 独立。无 TCP/IP 的 MCU 不能选择 router。**router 表示已编入能力，不表示成熟软路由已经验收。** BSD 的 IPv4 分片 NAT 已有双网口报文回归。故障恢复、规模和性能按 [补全记录](./router-completion.md) 验收。 LAN DHCPv4/DNS A 转发的配置与已验证范围见 [lan-services.md](./lan-services.md)。IPv4 PPPoE 的配置和 Linux 对端测试见 [PPPoE](./pppoe.md)。

BSD/F-Stack 后端现在也接受 `NET_ROLE=router`：启动时按 class NIC 顺序 attach 全部网卡（公共名为 `en2`、`en3`…，索引 1、3…；`lo` 固定为 2），并在执行器内置 `V_ipforwarding=1`、`V_ip6_forwarding=1`。F-Stack 处理 IPv4/IPv6 转发及 TTL/hop-limit 递减。IPv4 过滤和 TCP/UDP NAPT 经 `netfilter_bridge.c` 使用公共规则与 conntrack。

BSD 已验证（三节点 QEMU socket 拓扑）：

- IPv4 ping 与 IPv6 ping 的 TTL/hop limit 均为 63；
- 16 KiB TCP 回显完整；
- UDP 单包经 `10.20.0.1:<port>` NAPT 到 WAN 并收到回包；
- `ip addr`/`ip route` 在 `en2`/`en3` 上读写正常。

DHCPv4 服务端（dnsmasq `--dhcp-range`）已在 en2 上完成完整租约交换：`udhcpc` 拿到 `192.168.10.51`、`router=192.168.10.1`、`dns=192.168.10.1`，随后用租约地址通过该路由器解析上游名字成功。这条路径依赖三处修复：`MSG_PEEK` 的 `IP_PKTINFO` 家族字段、`RTM_GETADDR` 的 `IFA_BROADCAST`，以及 BSD 后端对 `SIOCSARP` 的支持——dnsmasq 在客户端还没有地址时用 `SIOCSARP`预置邻居项，否则 OFFER/ACK 会卡在 FreeBSD 的 ARP hold 队列里。

BSD 当前仍缺多 FIB 与源策略、DHCPv6-PD 续租及 LAN 前缀发布。BSD 的
`SO_BINDTODEVICE` 当前仅接受主接口，指定次级网卡返回 `EOPNOTSUPP`。

## WAN IPv6 自动配置

启动时传入 `a20.ipv6_ra=en3`，指定接收 IPv6 Router Advertisement 的
网口。该接口在启动或由管理员重新启用时发送 Router Solicitation，最多
发送三次，间隔四秒。收到带有效路由器寿命的通告后停止探测。该接口
使用 BSD 的邻居发现和 SLAAC。启用 `NET_ROLE=router` 时，
它仍能从 RA 学习 WAN 默认路由，供配置过 IPv6 地址的 LAN 使用。
默认路由随通告寿命到期撤销，也接受寿命为零的即时撤销。其他接口
继续拒绝 RA。接收、地址生成和重复地址检测由 BSD 协议栈处理。
[RFC 4861](https://www.rfc-editor.org/rfc/rfc4861.html) ·
[RFC 4862](https://www.rfc-editor.org/rfc/rfc4862.html)

可在测试镜像中用 `netctl set-ipv6 en2 2001:db8:10::1 64` 配置 LAN
地址。以下测试用两个隔离以太网链路验证 WAN 地址、默认路由和 LAN
双向转发。

```sh
python3 tools/net/tests/test-bsd-router-ipv6-ra.py \
  --kernel <路由内核的kernel.bin> --out <新的证据目录>
```

BSD 的 `RTM_GETADDR` 返回链路本地地址和 SLAAC 地址，包括作用域与
DAD 状态。Linux 的 `ip -6 addr` 和 `getifaddrs()` 已用真实地址验证。
IPv6 路由的 Linux 查询接口仍在补全。

### DHCPv6 前缀委派互操作

隔离 KVM 环境中，原版 BusyBox 1.37.0 使用 `udhcpc6 -d -r no` 向
Kea 2.6.3 请求前缀。测试收到 /56 和 36 秒的租期，抓包校验了
Solicit、Advertise、Request 与 Reply。测试在 A20OS 手动配置委派前缀的
第一个 /64，并在 Linux 上配置返程路由，验证了委派地址的双向转发。
续租和向 LAN 自动通告前缀仍在补全。
[RFC 9915](https://www.rfc-editor.org/rfc/rfc9915.html) ·
[Kea 前缀委派](https://kea.readthedocs.io/en/kea-2.6.4/arm/dhcp6-srv.html)

```sh
python3 tools/net/tests/build-dhcp6-peer.py \
  --kea <kea-dhcp6路径> --radvd <radvd路径> --busybox <静态BusyBox路径> \
  --kver <Linux内核版本> --out <新的对端目录>

make ARCH=x86_64 NET_STACK=bsd NET_ROLE=router NR_CPUS=2 \
  DRIVER_DEPLOYMENT=embedded RAMFS_USER=1 RAMFS_NET_TESTS=1 RAMFS_DISTRO=1 kernel-only

python3 tools/net/tests/test-bsd-dhcp6-pd.py \
  --kernel <A20OS的kernel.bin> --linux <匹配的Linux内核> \
  --peer-initrd <对端目录>/peer-initramfs.cpio.gz --busybox <静态BusyBox路径> \
  --out <新的证据目录>
```

## 最小双网卡配置

下面是验证使用的隔离地址，实际部署需按自己的接口/地址更改：

```sh
ip addr replace 192.168.10.1/24 dev en2
ip addr replace 10.20.0.1/24 dev en3
ip route replace 192.168.10.0/24 dev en2
ip route replace 10.20.0.0/24 dev en3
echo 'natadd out proto=tcp action=masquerade' > /proc/a20/netfilter
echo 'natadd out proto=udp action=masquerade' > /proc/a20/netfilter
ip -6 addr add fd42:10::1/64 dev en2
ip -6 addr add fd42:20::1/64 dev en3
```

以上不是完整防火墙部署配置，也没有配置默认互联网路由、IPv6 RA 或 LAN DHCP。 NAT 不是转发访问控制。路由/策略 API 见 [routing.md](./routing.md)，NAT 约束见[nat.md](./nat.md)。

## 可复现验证

```sh
python3 tools/net/tests/test-net-router-kvm.py \
  --baseline <网络基线manifest.json> --router-kernel <冻结router kernel.elf> \
  --binary <静态Linux net-microbench ELF> --out <新证据目录>
```

工具创建三个独立 KVM 节点：Linux LAN → A20OS 双网卡 router → Linux WAN。 QEMU socket 后端连接两段隔离以太网，不修改宿主 TAP、路由、iptables 或网桥。 CPU affinity 可用 `--router-cpus/--lan-cpus/--wan-cpus` 指定；默认本机采用4,6/8,10/12,14。每节点2 vCPU/2 GiB，checksum/segmentation卸载关闭。此拓扑用于正确性，不能直接与 SLIRP/宿主回环吞吐作排名。

LAN 节点用两个地址模拟两个客户端，显式绑定同一 UDP 源端口53000，并发访问同一 WAN端点。它们是同一Linux guest上的两个IP端点，并非两台LAN物理主机。当前工具验收：

- IPv4 ping、IPv6 ping、TCP16KiB与双客户端UDP的payload完整。
- 出口两个NAPT端口不同；转发TTL/hop limit均从64变为63。
- 源策略规则把192.168.10.3导向表100，经10.20.0.2网关到WAN的198.51.100.5/32；匹配源TCP回显成功，不匹配源192.168.10.2无法建立连接。
- 四份pcap独立重算IPv4/TCP/UDP校验和；WAN捕获证明不存在匹配UDP/TCP的LAN源地址泄漏。

`tools/bench/net-microbench.c` 的 client 模式支持末尾可选的 `SOURCE_IP SOURCE_PORT`，默认调用方式保持不变。构建成同一个静态Linux ELF后用于各端点；源码和ELF哈希随证据保存。

## 当前 lwIP 分叉中的必要修正

转发路径中，`CHECKSUM_GEN_*` 曾被误用于清零 transit 报文校验和，导致 Ethernet output 无法补齐校验。当前实现修正为：保留未变化的传输层校验和，仅对 TTL 变化增量更新 IPv4 头校验和；同时阻止 TTL/hop limit 为 0 时减法回绕成 255。这是当前分叉的关键数据面修复，独立于 port 层开关。若迁移或升级上游 lwIP，必须对照审计并复跑对应测试套件。
