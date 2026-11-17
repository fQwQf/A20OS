# Router 构建与三节点验证

`NET_ROLE=host` 是默认值，不转发经过本机的 IPv4/IPv6 报文。`NET_ROLE=router` 显式编入跨接口转发能力，产物目录与 host 隔离：

```sh
make ARCH=x86_64 NR_CPUS=2 NET_STACK=bsd NET_LANES=1 NET_ROLE=router kernel-only
```

这项选择和 `NET_STACK`、`NET_PROFILE` 独立。无 TCP/IP 的 MCU 不能选择 router。**router 表示已编入能力，不表示成熟软路由已经验收。** BSD 的 IPv4 分片 NAT 已有双网口报文回归。故障恢复、规模和性能按 [补全记录](router-completion.md) 验收。 LAN DHCPv4/DNS A 转发的配置与已验证范围见 [lan-services.md](lan-services.md)。IPv4 PPPoE 的配置和 Linux 对端测试见 [PPPoE](pppoe.md)。

BSD/F-Stack 后端现在也接受 `NET_ROLE=router`：启动时按 class NIC 顺序 attach 全部网卡（公共名为 `en2`、`en3`…，索引 1、3…；`lo` 固定为 2），并在执行器内置 `V_ipforwarding=1`、`V_ip6_forwarding=1`。F-Stack 处理 IPv4/IPv6 转发及 TTL/hop-limit 递减。IPv4 过滤和 TCP/UDP NAPT 经 `netfilter_bridge.c` 使用公共规则与 conntrack。

BSD 已验证（三节点 QEMU socket 拓扑）：

- IPv4 ping 与 IPv6 ping 的 TTL/hop limit 均为 63；
- 16 KiB TCP 回显完整；
- UDP 单包经 `10.20.0.1:<port>` NAPT 到 WAN 并收到回包；
- `ip addr`/`ip route` 在 `en2`/`en3` 上读写正常。

DHCPv4 服务端（dnsmasq `--dhcp-range`）已在 en2 上完成完整租约交换：`udhcpc` 拿到 `192.168.10.51`、`router=192.168.10.1`、`dns=192.168.10.1`，随后用租约地址通过该路由器解析上游名字成功。这条路径依赖三处修复：`MSG_PEEK` 的 `IP_PKTINFO` 家族字段、`RTM_GETADDR` 的 `IFA_BROADCAST`，以及 BSD 后端对 `SIOCSARP` 的支持——dnsmasq 在客户端还没有地址时用 `SIOCSARP`预置邻居项，否则 OFFER/ACK 会卡在 FreeBSD 的 ARP hold 队列里。

BSD 路由器的 IPv4 和 IPv6 转发及本机 TCP/UDP 均可使用多 FIB 与源策略。 IPv4 和 IPv6 UDP、IPv6 原始套接字可使用 `SO_BINDTODEVICE` 限制收发网口。 IPv4 UDP 需要一条经绑定网口的路由，指向其他出口的发送请求返回不可达。

## x86_64 PC 的 UEFI 镜像

`router-x86_64-pc` 使用 PC 板级配置、F-Stack 和内存根文件系统。镜像包含 `netctl`、`wanctl`、`pppoe` 和 `pd-lan`。用以下命令打包可挂载持久配置卷的版本。

```sh
PATH="$PATH:/usr/sbin" tools/a20 package router-x86_64-pc -- RAMFS_DISTRO=1
```

产物是 `build/x86_64-pc/a20os-router-uefi.img`。在使用原生 64 位 UEFI、COM1 和受支持有线网卡的 PC 上，用串口 38400 8N1 检查启动。启动盘提供 EFI 加载器和内核。将静态 BusyBox、odhcp6c 和 WAN 配置放入内核可识别的可写 ext4 分区。GPT 分区类型使用 Linux filesystem，分区名使用 `A20OS distro`。内核将该分区挂载到`/extra`。当前 AHCI 驱动在每个控制器上选择首个就绪磁盘。

先按串口日志的 MAC 地址确认 LAN 和 WAN 接口，再配置地址和 WAN 服务。以下示例使用 `en2` 作为 LAN，配置文件使用 `en3` 作为 WAN。

```sh
/bin/netctl set-ipv4 en2 192.168.10.1 255.255.255.0
/bin/wanctl /extra/wan.conf
```

双物理网口验收需要在测试机上记录两张卡的 PCI ID 和 MAC、驱动绑定与中断变化，再用隔离的 LAN 和 WAN 对端验证双向转发、持续负载及拔插 WAN 网线后的恢复。路由器侧可用 `/proc/interrupts`、`/proc/net/dev`、`/proc/a20/netmem` 和 `/proc/a20/netfilter` 采样。[PC 串口和启动条件](../platforms/x86_64-pc.md)

## WAN IPv4 DHCP

原版 BusyBox `udhcpc` 可在 F-Stack 路由器的第二张网卡上获取 IPv4 租约。初次发现使用链路层广播，续租使用绑定 WAN 的 IPv4 UDP 套接字。租约的地址、路由和 DNS 通过客户端脚本应用。VirtIO 与 E1000 系列驱动的载波变化可由`RTM_NEWLINK` 或 `SIOCGIFFLAGS` 读取。

```sh
python3 tools/net/tests/test-bsd-dhcp4-wan.py --kernel <路由内核的kernel.bin> \
  --busybox <静态BusyBox> --out <新的证据目录>
```

### 开机配置和链路恢复

F-Stack 路由器构建在 `RAMFS_USER=1` 时包含 `/bin/wanctl`。将配置放在持久存储上，例如挂载到 `/extra` 的磁盘中的 `wan.conf`。给 A20OS 自带 init 的内核启动参数添加 `a20.wan_config=/extra/wan.conf`。也可以从前台运行`/bin/wanctl /extra/wan.conf`。

DHCP 配置使用原版静态 BusyBox：

```ini
interface=en3
mode=dhcp
busybox=/extra/busybox
resolver=/etc/resolv.conf
```

静态配置示例：

```ini
interface=en3
mode=static
address=10.20.0.12/24
gateway=10.20.0.2
dns=192.0.2.53 192.0.2.54
resolver=/etc/resolv.conf
```

`gateway` 和 `dns` 在静态模式下可省略。省略 `resolver` 时使用`/tmp/wan-resolv.conf`。指定 `/etc/resolv.conf` 会为系统 DNS 客户端发布地址，该路径需要预留给服务使用。服务以独占方式添加地址和默认路由，保留已有配置。收到 DHCP 续租时更新 DNS 或网络配置。载波消失、租约到期、客户端退出或服务正常停止时，撤销该服务拥有的地址、默认路由和 DNS 文件。链路恢复后重新应用静态配置或开始 DHCP，客户端意外退出后自动重启。当前每台系统运行一个 IPv4 以太网 WAN 监督进程。IPv6 RA、DHCPv6-PD 和 PPPoE 的配置见各自章节。

```sh
python3 tools/net/tests/test-bsd-wan-config.py --kernel <路由内核的kernel.bin> \
  --busybox <静态BusyBox> --out <新的证据目录>
```

## WAN IPv6 自动配置

启动时传入 `a20.ipv6_ra=en3`，指定接收 IPv6 Router Advertisement 的网口。该接口在启动或由管理员重新启用时发送 Router Solicitation，最多发送三次，间隔四秒。收到带有效路由器寿命的通告后停止探测。该接口使用 BSD 的邻居发现和 SLAAC。启用 `NET_ROLE=router` 时，它仍能从 RA 学习 WAN 默认路由，供配置过 IPv6 地址的 LAN 使用。默认路由随通告寿命到期撤销，也接受寿命为零的即时撤销。路由器模式为其他以太网接口生成链路本地地址，这些接口继续拒绝收到的 RA。接收、地址生成和重复地址检测由 BSD 协议栈处理。[RFC 4861](https://www.rfc-editor.org/rfc/rfc4861.html) · [RFC 4862](https://www.rfc-editor.org/rfc/rfc4862.html)

可在测试镜像中用 `netctl set-ipv6 en2 2001:db8:10::1 64` 配置 LAN 地址。以下测试用两个隔离以太网链路验证 WAN 地址、默认路由和 LAN 双向转发。

```sh
python3 tools/net/tests/test-bsd-router-ipv6-ra.py \
  --kernel <路由内核的kernel.bin> --out <新的证据目录>
```

BSD 的 `RTM_GETADDR` 返回链路本地地址和 SLAAC 地址，包括作用域、 DAD 状态和剩余寿命。Linux 的 `ip -6 addr` 和 `getifaddrs()` 已用真实地址验证。`RTM_NEWADDR` 可通过 `IFA_CACHEINFO` 设置首选寿命和有效寿命。更新地址时，手动配置的直连前缀按同一前缀下最长的剩余有效寿命更新。内核到期后清理地址和直连路由。`RTM_GETROUTE`可查询 IPv6 策略转发表的匹配项、下一跳和出口接口。IPv6 路由列表包含手动配置的条目。相关命令见 [双栈路由说明](routing.md)。

### DHCPv6 前缀委派互操作

隔离 KVM 环境中，原版 BusyBox 1.37.0 使用 `udhcpc6 -d -r no` 向 Kea 2.6.3 请求前缀。测试收到 /56 和 36 秒的租期，抓包校验了 Solicit、Advertise、Request 与 Reply。测试在 A20OS 手动配置委派前缀的第一个 /64，并在 Linux 上配置返程路由，验证了委派地址的双向转发。加上 `--renew --bind-probe` 会同时申请 IA_NA 和 IA_PD，验证首个单播续租 Reply、绑定网口的双向收发和解除绑定。BusyBox 1.37.0 使用`-r no` 时的续租路径解引用空地址指针。[BusyBox 1.37.0 源码](https://busybox.net/downloads/busybox-1.37.0.tar.bz2) · [RFC 9915](https://www.rfc-editor.org/rfc/rfc9915.html) · [Kea 前缀委派](https://kea.readthedocs.io/en/kea-2.6.4/arm/dhcp6-srv.html)

用 `--odhcp6c <静态odhcp6c路径>` 可验证仅前缀续租。原版 odhcp6c 通过 `-N none -P 56 -F` 向 Kea 请求并续租 /56，四报文交换和续租均只含 IA_PD。客户端脚本收到 `bound` 和 `updated` 事件，携带前缀、首选寿命和有效寿命。WAN RA/SLAAC 由内核处理。该互操作测试手动配置 LAN 地址和上游返程路由。[OpenWrt odhcp6c](https://github.com/openwrt/odhcp6c)

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

### 自动发布委派前缀

在启用 `NET_ROLE=router` 的镜像中，以有网络管理权限的用户运行`pd-lan`，传入原版静态 odhcp6c 的路径、WAN 接口、申请的前缀长度以及 LAN 接口与子网编号。

```sh
/bin/pd-lan --state /extra/pd-state /usr/sbin/odhcp6c en3 56 en2:0x2a
```

该命令在前台运行。从委派的 /56 为 `en2` 分配子网编号 `0x2a`对应的 /64，将子网的 `::1` 配给路由器，并向 LAN 发布 SLAAC 前缀。可以提供多个不同的 `LAN:子网编号` 参数。带 PD 排除选项的租约会跳过重叠子网。通告寿命受上游剩余租约约束。WAN 默认路由失效时，通告中的路由器寿命变为零，仍有效的前缀继续发布。租约失效或命令正常退出时，程序撤销通告并清理它管理的地址。WAN 接口可以是普通以太网接口或 IPv6CP 建立的 PPP 接口。服务等待 WAN 链路本地地址，接口关闭、销毁或载波丢失后等待链路恢复，再启动 DHCPv6 会话。[PPP 配置](pppoe.md#在-ppp-上取得并发布-ipv6-前缀) · [RFC 4861](https://www.rfc-editor.org/rfc/rfc4861.html)

`--state` 接受可写持久卷上的绝对路径。部署时每个进程使用独立文件，在进程或整机重启后使用相同路径以及相同 LAN 接口名和 MAC。程序在通告前记录各 LAN 的 /64 和通告寿命。重新启动后，它以零首选寿命和零有效寿命撤销历史前缀。上游再次委派同一前缀后，程序恢复该前缀的正常通告。恢复后的撤销期限以本次启动的单调时钟计算，时长取此前通告的最长有效寿命。每个 LAN 同时跟踪最多 16 个已发布或待撤销的前缀。达到上限时，新前缀等待可用记录。省略 `--state` 时，记录只存在于进程内存中。[RFC 9096 §3.5](https://www.rfc-editor.org/rfc/rfc9096.html#section-3.5)

以下测试复用上述 Kea 对端镜像，分别启动 Linux WAN 和 LAN 虚拟机。 Linux LAN 通过 SLAAC 配置自己的地址及默认路由，测试只在模拟 ISP 配置委派前缀的返程路由。

```sh
python3 tools/net/tests/test-bsd-pd-lan.py \
  --kernel <A20OS的kernel.bin> --linux <匹配的Linux内核> \
  --peer-initrd <对端目录>/peer-initramfs.cpio.gz --busybox <静态BusyBox路径> \
  --odhcp6c <静态odhcp6c路径> --out <新的证据目录>
```

加上 `--restart-state` 可以验证两条 LAN 各自的前缀，以及路由器重启、上游换号和持久卷写入失败后的通告行为。

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

以上不是完整防火墙部署配置，也没有配置默认互联网路由、IPv6 RA 或 LAN DHCP。 NAT 不是转发访问控制。路由/策略 API 见 [routing.md](routing.md)，NAT 约束见[nat.md](nat.md)。

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
