# IPv4 路由控制

路由和策略状态由 `control/route.rs` 持有，不含 lwIP 指针。C 桥提供 IRQ-safe 锁，查询返回值副本；配置锁内不调用后端。IPv4 字段保留网络字节布局，表号、metric 和 priority 使用宿主字节序。前缀比较在大小端机器上含义一致。

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

lwIP 路由 hook 查询这一 FIB；ARP 网关 hook 对同一源/目标查询并取网关值副本。网关仅支持直接连接的下一跳，尚未提供递归下一跳解析。策略源地址需要由应用明确绑定或已存在于转发包中；未绑定本地 socket 的源地址自动选择不是这次改造的验收项。

## 已支持与边界

支持 IPv4 unicast 路由、32 位表号、网关、出口和 metric，以及源/目标前缀的 lookup 规则。修正了原先把 Linux `RTA_GATEWAY=5` 当成 3 的错误。查询输出真实已安装值，并保留尚未迁入 FIB 的接口默认网关作为兼容回退。

尚不支持 IPv6 FIB、非初始网络域独立 FIB、策略 mark/iif/oif、ECMP、黑洞/不可达动作或递归网关。这些请求返回错误，不能依赖写成功但数据面不执行的旧行为。目前 bounded FIB 以线性查表和有界规则遍历为主，不代表大路由规模的最终实现。本功能不等于开启了跨网卡转发，也不等于 NAT、分片或 PMTU 已完成。

## 验证

```sh
sh tools/test-net-route-host.sh
sh tools/test-socket-netlink-host.sh
python3 tools/test-net-control-kvm.py --baseline <网络基线manifest.json> \
  --kernel <冻结kernel.elf> --out <新的证据目录>
```

宿主测试覆盖前缀字节序、最长前缀、metric、替换/删除、设备清理、规则优先级和空表回退、容量耗尽；KVM 使用实际 Alpine `ip` 完成新增、查询及删除事务。后者发现并修复了 netlink `write(2)` 分派和删除时 action 未指定的 Linux ABI 兼容问题。
