# LAN DHCP 与 DNS 转发

网络服务使用 Alpine 的原版 Linux ELF，不在内核重新实现 DHCP/DNS 服务。已在三节点隔离 KVM 拓扑验证 dnsmasq 2.91-r1、BusyBox udhcpc 1.37.0 的 DHCPv4 租约与 DNS A 查询转发。它是功能验收，尚未覆盖 DHCPv6、RA、DNS TCP、大规模租约、重启恢复或服务安全加固，不能据此称为生产软路由。

## 配置示例

先按 [router.md](./router.md) 配好 LAN/WAN 接口与路由。下面使用 LAN `en2`的 `192.168.10.1/24` 和 WAN 上游 DNS `10.20.0.2`。改为部署地址后再使用。

```sh
apk add dnsmasq alpine-baselayout-data
mkdir -p /var/lib/misc
dnsmasq --no-daemon --user=root --no-resolv --no-hosts \
  --bind-interfaces --interface=en2 --server=10.20.0.2 \
  --dhcp-range=192.168.10.50,192.168.10.60,255.255.255.0,1h \
  --dhcp-option=3,192.168.10.1 --dhcp-option=6,192.168.10.1 \
  --dhcp-leasefile=/var/lib/misc/dnsmasq.leases --log-facility=-
```

这是便于调试的前台配置；`--user=root` 是当前验收条件，不是已验证的降权部署方案。`alpine-baselayout-data` 提供账户文件；最小测试镜像缺少它时，即使在 Linux 上也会因找不到 root 账户而退出。实际服务监督、权限收敛和持久化路径要单独验收。完整选项以 [dnsmasq 官方手册](https://thekelleys.org.uk/dnsmasq/docs/dnsmasq-man.html)为准。

## 内核契约

- IPv4 `IP_PKTINFO` 接收端返回实际入口索引、包目的地址和入口本地地址。发送端使用每报文 `ifindex/source_v4` 值，借用仅持续到发送调用返回，不临时覆盖共享 PCB。
- 空 iovec 的 `sendmsg` 仍能发出零长度 UDP 报文；地址与控制数据不会被丢弃。
- `SO_BINDTODEVICE` 实际约束原生 PCB 的收发接口，要求命名空间所属用户域的`CAP_NET_RAW`。未知接口返回 ENODEV；清空绑定后读回长度为 0。当前非初始网络域不支持该选项，明确返回错误。TCP accept 从原生 PCB 继承接口约束。
- 具体 IPv4 地址可分别监听同一 TCP 端口；wildcard 仍与它们冲突。`SIOCGIFNAME` 按索引查名字，输入名字可以未初始化。
- `SIOCSARP` 支持有 `CAP_NET_ADMIN` 的初始网络域，完整动态 Ethernet 邻居`ATF_COM`；明确拒绝永久、代理和其他 flags。校验地址为指定接口子网的单播地址、 MAC 为非零单播。更新经公共后端执行接口，不向前端返回 netif 指针。 lwIP 端口扩展 `a20_etharp_update_dynamic` 复用已有 ARP 有界缓存、排队包释放和老化。这仍是已记录的 lwIP 补丁，不是原版上游接口。
- 当前 `IP_MTU_DISCOVER` 只支持实际存在的 `IP_PMTUDISC_DONT`。其余模式返回 EOPNOTSUPP；完整 DF/PMTU 行为仍待实现。

物理网络域的 UDP 全部经协议栈分流。旧的按端口查找本地 socket 的快捷路径会把外部 DNS 请求错误送回本机，并遗漏入口元数据/接口约束，已停止在物理域使用。隔离网络域当前只有软件 loopback，仍使用自己的前端投递路径。

## 验证

把 `tools/net-socket-abi-probe.c` 用与 microbench 相同的 musl 静态工具链编译，保存源码、命令和 ELF 哈希。再运行：

```sh
python3 tools/test-net-router-kvm.py \
  --baseline <基线manifest.json> --router-kernel <冻结router kernel.elf> \
  --binary <静态Linux net-microbench ELF> \
  --socket-probe <静态Linux net-socket-abi-probe ELF> \
  --services-world <已装dnsmasq及账户文件的Alpine镜像> --out <新证据目录>
```

工具复制镜像并安装测试，不改动输入镜像或宿主网络。Linux WAN 和 A20OS 使用同一探针 ELF，检查具体地址/wildcard 绑定、接口选项、IP_PKTINFO 和零长度 UDP。随后 Linux LAN 验证 DHCP OFFER/REQUEST/ACK 的租约、网关与 DNS，查询上游专属的 `upstream.test`。查询有 10 秒截止时间，所有报文独立重算校验和。转发 TTL 与路由器新发 DNS 的初始 TTL 分别检查，避免把本机发包误判为未递减的转发包。

成功证据：`build/evidence/net-refactor-20261028/router-services-buckets-kvm/manifest.json`。两个内核均输出 `NET_SOCKET_ABI_PASS`，LAN 输出 `DHCP_LEASE_OK`、`LAN_SERVICES_PASS`，四份抓包校验和检查通过。失败样本保留在同级目录，UDP connect 自环可由 ABI 探针复现。

## 原版默认后端

原版 `NET_STACK=lwip` 当前不提供动态 SIOCSARP 写入。LAN DHCP 使用 dnsmasq 的`--dhcp-broadcast`，已完成真实 Linux 客户端租约回归。`NET_STACK=native` 仍支持有界动态邻居更新。不能将 upstream 对 SIOCSARP 返回的不支持改为假成功。
