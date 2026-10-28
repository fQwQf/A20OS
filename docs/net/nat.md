# IPv4 NAT 状态与使用边界

本轮提供 TCP/UDP 的冲突检测及动态源端口分配、事务式绑定和独立同步。
这不是软路由整体验收：分片 NAT、ICMP 差错翻译、PMTU 仍未完成。
跨网卡转发见 [router.md](./router.md)，LAN DHCPv4/DNS A 转发见
[lan-services.md](./lan-services.md)；各自有独立的多节点功能证据。

## 配置

接口和地址配置后，可由管理员写入现有控制入口，例如：

```sh
echo 'natadd out proto=tcp action=masquerade' > /proc/a20/netfilter
echo 'natadd out proto=udp action=masquerade' > /proc/a20/netfilter
cat /proc/a20/netfilter
```

MASQUERADE 使用实际出口的地址；出口没有地址时拒绝创建映射，不能借用其他网卡
的地址。SNAT 可通过 `action=snat to=<地址>` 指定源地址，`toport=<端口>` 固定端口。
不指定固定端口时，先保留客户端端口；完整回复元组冲突时，在 1024–65535 中探测。
不同远端可以复用外部端口，同一远端的回复元组必须只有一个所有者。固定端口冲突
直接失败，不擅自改成其他端口。ICMP echo 目前仅做地址变换，冲突时拒绝，尚无标识符分配。

`natdel <索引>` 删除规则，已有绑定继续工作。`reset` 清理规则，`ctflush` 显式丢弃
连接状态，后者会破坏依赖这些绑定的连接。`ctoff` 停止建立新状态，保留已有状态；
匹配 NAT 的新流会被拒绝，不允许悄悄绕过 NAT 策略。满表返回 ENOSPC 并丢弃报文，
不驱逐活跃连接。资源预算由 `net/profile.h` 管理。

## 事务、锁与内存

`filter/netfilter_nat.rs` 统一决定规则匹配、绑定分配、双索引、状态和超时。C 提供
固定容量存储、IRQ-safe CT 锁、时钟和原位重写。每个 Rust 入口用 RAII guard 持锁，
配置、查询、包处理和回收都遵循同一同步方式；不再依赖 lwIP 全局锁或重试后跳过规则。
共享表项以值副本读写，不制造可重叠的 `&'static mut` 引用。

锁顺序为后端执行锁/配置写锁 → CT 锁；CT 内不能反向取得后端锁或 socket 锁。
不在事务中分配内存、休眠或转移包所有权。当前有界表使用一个 CT 锁，便于保证元组
预留与索引更新原子性；未来是否分片必须由争用测量决定，不能以无保护共享表替代。

绑定先提交，随后才能修改报文；失败时保持报文不变，由 filter 丢弃。
转发流可能先在 ingress 建立未转换状态，再在 egress 附加 SNAT；回复索引在同一
事务中重建。反向映射只在正确方向执行，避免跨 ingress/egress 时重复转换。

超时统一为 32 位回绕毫秒，不能把高频原始 tick 截断后当作毫秒。默认 TCP NEW 30 秒、
ESTABLISHED 120 秒、UDP/echo 30 秒；活跃流刷新时间，闲置流由有界 sweeper 回收。
这些是当前状态跟踪超时，不等同于完整 TCP 状态机或抗攻击策略。

## 验证与未覆盖项

```sh
bash tools/tests/netfilter_nat_host/run.sh "$PWD/build/evidence/<新目录>"
bash tools/tests/netfilter_filter_host/run.sh "$PWD/build/evidence/<另一新目录>"
sh tools/test-nat-rewrite-host.sh c
sh tools/test-nat-rewrite-host.sh rust
python3 tools/check-net-pcap.py <完整、已完成软件校验的Ethernet抓包>
```

生产状态机测试覆盖两客户端同端口冲突、回包归属、重传、转发两阶段、固定端口冲突、
满表不变换、不驱逐、关闭 tracking、4 线程竞争，以及 1 kHz/2 GHz 时钟与毫秒回绕。
重写测试独立重算校验和：TCP 的零值仍是合法计算结果，必须更新；IPv4 UDP 的原始
零值表示未启用校验，但变换计算出的零必须编码成 0xffff。

三节点 KVM 已覆盖双网卡拓扑及两个客户端的同源端口冲突。仍未覆盖组合
DNAT+SNAT/hairpin、分片、ICMP 差错/PMTU、完整 TCP
状态检查、独立网络域和与本地 socket 端口分配器的统一预留。当前能力不足以承诺
生产软路由。不要为这些未完成项编写声称可用的部署配置。
