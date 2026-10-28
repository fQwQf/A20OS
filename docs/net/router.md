# Router 构建与三节点验证

`NET_ROLE=host` 是默认值，不转发经过本机的 IPv4/IPv6 报文。
`NET_ROLE=router` 显式编入跨接口转发能力，产物目录与 host 隔离：

```sh
make ARCH=x86_64 NR_CPUS=2 NET_LANES=4 NET_ROLE=router kernel-only
```

这项选择和 `NET_STACK`、`NET_PROFILE` 独立。无 TCP/IP 的 MCU 不能选择 router。
**router 表示已编入能力，不表示成熟软路由已经验收。** 分片 NAT、ICMP 差错翻译、
PMTU、故障恢复和规模成本仍需逐项完成，不能把打开 forwarding 宏当作结论。
LAN DHCPv4/DNS A 转发的配置与已验证范围见 [lan-services.md](./lan-services.md)。

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

以上不是完整防火墙部署配置，也没有配置默认互联网路由、IPv6 RA 或 LAN DHCP。
NAT 不是转发访问控制。路由/策略 API 见 [routing.md](./routing.md)，NAT 约束见
[nat.md](./nat.md)。

## 可复现验证

```sh
python3 tools/test-net-router-kvm.py \
  --baseline <网络基线manifest.json> --router-kernel <冻结router kernel.elf> \
  --binary <静态Linux net-microbench ELF> --out <新证据目录>
```

工具创建三个独立 KVM 节点：Linux LAN → A20OS 双网卡 router → Linux WAN。
QEMU socket 后端连接两段隔离以太网，不修改宿主 TAP、路由、iptables 或网桥。
CPU affinity 可用 `--router-cpus/--lan-cpus/--wan-cpus` 指定；默认本机采用4,6/8,10/12,14。
每节点2 vCPU/2 GiB，checksum/segmentation卸载关闭。此拓扑用于正确性，不能直接与
SLIRP/宿主回环吞吐作排名。

LAN 节点用两个地址模拟两个客户端，显式绑定同一 UDP 源端口53000，并发访问同一
WAN端点。它们是同一Linux guest上的两个IP端点，并非两台LAN物理主机。当前工具验收：

- IPv4 ping、IPv6 ping、TCP16KiB与双客户端UDP的payload完整。
- 出口两个NAPT端口不同；转发TTL/hop limit均从64变为63。
- 源策略规则把192.168.10.3导向表100，经10.20.0.2网关到WAN的198.51.100.5/32；
  匹配源TCP回显成功，不匹配源192.168.10.2无法建立连接。
- 四份pcap独立重算IPv4/TCP/UDP校验和；WAN捕获证明不存在匹配UDP/TCP的LAN源地址泄漏。

`tools/net-microbench.c` 的 client 模式支持末尾可选的 `SOURCE_IP SOURCE_PORT`，默认
调用方式保持不变。构建成同一个静态Linux ELF后用于各端点；源码和ELF哈希随证据保存。

## 当前 lwIP 分叉中的必要修正

现有转发代码原本将 `CHECKSUM_GEN_*` 当成清零transit报文校验和的依据，实际
Ethernet output不会再生成这些校验和。本轮保留未变化的传输层校验和，仅对TTL变化
增量修正IPv4头；同时阻止TTL/hop limit为0时减法回绕成255。这是当前分叉的数据面
修复，不是仅靠port层开关能够完成的事。恢复原版lwIP前必须对照审计并复跑这些验收。
