# IPv4 与 IPv6 路由控制

路由和策略状态由 `control/route.rs` 持有，不含 lwIP 指针。C 桥提供 IRQ-safe 锁，查询返回值副本；配置锁内不调用后端。IPv4 和 IPv6 地址保留网络字节布局，表号、metric 和 priority 使用宿主字节序。前缀比较在大小端机器上含义一致。

## 使用

以下例子要求 `en2` 已配置为 `10.0.2.15/24`，网关为同链路 `10.0.2.2`：

```sh
ip route add 198.51.100.0/24 via 10.0.2.2 dev en2 table 100 metric 7
ip rule add priority 100 from 10.0.2.15/32 table 100
ip route show table 100
ip rule show
ip route get 198.51.100.5 from 10.0.2.15
ip rule del priority 100 from 10.0.2.15/32 table 100
ip route del 198.51.100.0/24 via 10.0.2.2 dev en2 table 100
```

规则按 priority 从小到大查询；匹配但对应表没有路由时继续；最后查询 main(254)。每个表按最长前缀、较低 metric 选路。相同表、目标前缀、metric 的新增返回 EEXIST，显式 replace 更新。设备撤销会清理其路由。默认预算为 64 路由、16 规则；`NET_PROFILE=1` 为 8 路由、4 规则。满表返回 ENOSPC，不覆盖既有项。

lwIP 路由 hook 查询这一 FIB。ARP 网关 hook 对同一源和目的地址查表并取得网关值副本。应用明确绑定的源地址可以参与策略匹配。

F-Stack 路由器为 IPv4 与 IPv6 转发提供主表和最多 8 个附加 FIB。两种协议共享 Linux 表号到原生 FIB 的映射，各自维护路由和规则。入口按源和目的地址选择首个有匹配路由的原生 FIB，IPv4 在完成分片重组、过滤与 DNAT 后选表。附加表通过显式出口连接下一跳，较低 metric 的路由进入原生 FIB，删除后由同前缀的下一条接管。本机地址与 IPv6 链路本地流量由主表处理，转发产生的 ICMP/ICMPv6 差错也经主表回送。F-Stack 本机 socket 的策略选择待接入。

下面的 IPv6 示例使用 en4 链路上的下一跳 `fe80::2`：

```sh
ip -6 route add 2001:db8:99::/64 via fe80::2 dev en4 table 200 metric 20
ip -6 rule add priority 100 from 2001:db8:10::/64 to 2001:db8:99::/64 table 200
ip -6 route show table 200
ip -6 rule show
ip -6 route get 2001:db8:99::9 from 2001:db8:10::2
```

## 已支持与边界

IPv4 rtnetlink 支持 unicast 路由、32 位表号、网关、出口和 metric，以及源和目的前缀的 lookup 规则。查询输出公共路由表中的值，并保留接口默认网关作为兼容回退。F-Stack 原生生成的直连路由由主表维护。IPv6 配置支持显式出口、链路本地或同链路网关、表号和 metric，路由查询使用原生 FIB。IPv6 路由列表列出手动配置的条目，RA 和直连路由可通过原生查表验证。

策略 mark/iif/oif、独立网络域 FIB、ECMP、黑洞/不可达动作和递归网关仍在支持范围之外。公共路由状态使用有界线性表。

## 验证

```sh
sh tools/host-tests/test-net-route-host.sh
sh tools/host-tests/test-socket-netlink-host.sh
python3 tools/net/tests/test-net-control-kvm.py --baseline <网络基线manifest.json> \
  --kernel <冻结kernel.elf> --out <新的证据目录>
```

宿主测试覆盖前缀字节序、最长前缀、metric、替换/删除、设备清理、规则优先级和空表回退、容量耗尽；KVM 使用实际 Alpine `ip` 完成新增、查询及删除事务。后者发现并修复了 netlink `write(2)` 分派和删除时 action 未指定的 Linux ABI 兼容问题。

F-Stack 路由器的三网口 QEMU 回归使用实际 BusyBox `ip` 配置策略，通过抓包验证同一目的地址的双 WAN 分流、反向转发、空表回退、最长前缀、metric 接管与替换、ICMP 差错和错误网关拒绝。

```sh
python3 tools/net/tests/test-bsd-policy-v4.py --kernel <x86_64-router-kernel.bin> \
  --busybox <静态BusyBox> --out <新的证据目录>
```

IPv6 回归给两个 WAN 使用相同的链路本地网关地址，检查邻居发现、源与目的前缀匹配、回程和 ICMPv6 超时。测试也验证删除一个地址族的路由时保留同表的另一地址族。

```sh
python3 tools/net/tests/test-bsd-policy-v6.py --kernel <x86_64-router-kernel.bin> \
  --busybox <静态BusyBox> --out <新的证据目录>
```
