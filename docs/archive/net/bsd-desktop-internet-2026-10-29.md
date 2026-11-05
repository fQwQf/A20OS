# BSD 后端桌面联网垂直切片（2026-10-29，2026-10-30 更新）

`NET_STACK=bsd` 现在是 x86_64 host/desktop 的可构建、可启动后端。真实 XFCE 桌面会话已经通过公共 Linux socket ABI、virtio-net、F-Stack/FreeBSD 协议核心和 QEMU user/SLIRP 完成证书校验的 HTTP/HTTPS 访问，并由真实 Firefox 151 的 Marionette 会话读取 `example.org` 的 HTTP 与 HTTPS DOM。x86_64 桌面和服务器默认 `NET_STACK` 已切到 BSD；其它架构仍保持各自已验证默认。这仍不等于所有 IPv6/DHCP/TUN/raw/路由能力都已完备。

## 已实现链路

| 层 | 实现 |
|---|---|
| 执行模型 | 单个持久 BSD 内核任务；一次 SYSINIT/UMA；所有 BSD 调用经同步请求队列串行执行；IRQ 只唤醒 |
| TLS | Clang emulated TLS 绑定实际 `proc_current()` 任务，不读写用户 FS/GS |
| 时间 | 100 Hz `ff_hardclock()` 按单调时间推进，不按 poll 次数推进 |
| NIC | 真实上游 `ff_veth.c`，四个非 DPDK 宿主边界；单 virtio、动态 IPv4/MTU/link、offload 全关 |
| RX | 32 个固定 1536 B token；池满停止 drain；mbuf 外挂 token，由 BSD 释放回调归还 |
| TX | mbuf 线性化后调用 class `send`；驱动复制到自有 TX buffer 后立即释放 mbuf；提交不等于线上完成 |
| socket | 真实 `ff_socket/bind/connect/sendto/recvfrom/close`；BSD errno 显式转 Linux errno |
| 公共 ABI | 复用现有 Linux socket 前端、接收队列、poll/wait 和 `/proc`；未引入私有 fd/PCB |
| 数据发布 | 执行器用单个 kqueue 取得就绪 socket，将数据送入公共 `net_msg_t` 队列并唤醒等待者 |
| 控制面 | 常见 `ifreq` 查询/设置与 netlink 索引；地址、MTU、link up/down 经执行器事务执行 |
| 地址事务 | 替换 IPv4 主地址时先删除旧 `in_ifaddr`，再安装新地址；失败恢复旧地址与默认路由 |

IPv4/IPv6 的本地 TCP `bind/listen/accept/connect/close` 已用同一 Linux ELF 验证； BSD accept 子连接经公共 accept 队列交给现有前端。外网目前验证 IPv4，入站 TCP/UDP 由 QEMU `hostfwd` 从宿主驱动。动态 IPv4 地址、MTU、link flags 和默认路由增删已通过`ip` 命令验证；外部 IPv6 地址/RA、DHCP 客户端、TUN、raw、PMTU、邻居注入和 router 仍返回准确错误。没有静默回退。BSD 目前要求 `ARCH=x86_64`、`NET_LANES=1`、`NET_PROFILE=2`，且不是 router 构建；embedded 资源档尚未做预算证据。

## 实际验收

- benchmark world、2 vCPU：`/proc/net/status` 显示 `backend: bsd`；BusyBox DNS、 HTTP、HTTPS 全部成功，页面 577 B 且包含 `Example Domain`。
- 同一 benchmark world、hostfwd 入站：`net-microbench` TCP 单连接、4 并发 TCP、 UDP 单连接、4 并发 UDP 均 verified=1；串口输出 `BSD_INBOUND_PASS`。
- 真实 `build/images/xfce-x86_64.img`、4 vCPU/4 GiB：XFCE/labwc 启动后， certificate-validating curl 完成 HTTP 与 HTTPS，两次均为 577 B，串口输出`BSD_DESKTOP_NET_PASS`。
- 同一桌面镜像、真实图形 Firefox 151.0.3：
  - Marionette 在 GUI 进程中监听 2828；
  - `http://example.org/` 的 `document.title` 与正文匹配 `Example Domain`；
  - `https://example.org/` 同样通过，证书校验保持开启；
  - 串口输出 `BSD_BROWSER_WEBDRIVER_RC 0` 和 `BSD_BROWSER_PASS`。
- 动态控制面（同一桌面镜像）：
  - `ip link set dev a20n0 mtu 1400` → `BSD_RECONFIG_MTU_1400_RC 0`；
  - `ip addr replace 10.30.0.2/24 dev a20n0` → 成功，`ip addr show` 读回新地址；
  - `ip addr replace 10.0.2.15/24 dev a20n0` → 恢复原地址，默认路由重新安装；
  - `ip link set dev a20n0 mtu 1500` → 成功；
  - 随后的 HTTPS 仍返回 577 B `Example Domain`。
- 链路统计示例：`rx=45 tx=42 rx_drop=0 tx_error=0 rx_starved=0`。
- PCAP 包含 ARP、DNS A/AAAA、TCP 443 握手与有效载荷；未启用 checksum/TSO/LRO offload。
- 原版 lwIP 内核构建、公共边界检查、宿主 BSD 初始化/UDP/TCP 回环仍通过。

默认切换证据冻结内核 SHA256（同一工作树默认 `NET_STACK=bsd` 构建）：

`81218b273a9cc43a91a4d31d4340901c87b23813ce78aa1207a8296176361569`

证据在 `build/evidence/net-router-completion-20261030/`：

- `bsd-actual-browser/`：按 `a20 run` 的实际 `[net]` 静态参数和默认 Firefox profile 完成 HTTP/HTTPS DOM；
- `bsd-fix-browser/`：不传 `-append` 的板级 fallback 浏览器门禁；
- `bsd-fix-inbound/`：宿主 hostfwd 驱动的 TCP/UDP 单连接与 4 并发入站；
- `bsd-fix-route/`：动态地址/MTU/link、默认路由删除/重建与恢复后的 HTTPS；
- `bsd-fix-internet/`：benchmark world 的 DNS/HTTP/HTTPS；
- `bsd-fix-kernel.sha256`：冻结内核指纹。

## Firefox 阻塞的定位结果

此前“Firefox 停在 `do_wait`”不是 BSD TCP/UDP 数据面失败，也不是 Firefox 真的永久等待。两个测试环境问题叠加造成了误判：

1. A20OS 的 `/proc/<pid>/wchan` 对任意 `PROC_BLOCKED` 任务统一报告`do_wait`，不能用来区分 `wait4()`、socket、futex 或其它阻塞点；
2. 桌面测试复用了 benchmark 的 `Guest` 构造器，它在启动后等待`A20_BENCH_READY`。桌面镜像不打印这个 benchmark 标记，构造器 60 秒后杀掉 QEMU，正好落在 Firefox 启动/建 Marionette 的时间窗内。

修复方式是给 `Guest` 增加可选的 `ready_pattern`，桌面测试传 `None`；浏览器门禁直接启动打包的 ELF，并在实例配置的 4 个 vCPU / 4 GiB 上等待 Marionette 就绪。修复后 Firefox 进程正常进入 GUI，HTTP/HTTPS DOM 均通过。

实际 `tools/a20 run xfce-x86_64` 还暴露了第二个启动问题：QEMU 直接 `-kernel`启动且没有 `-append` 时，会把 kernel 文件路径放进 Multiboot command line； qemu-virt-x86_64 板级代码看到非空字符串后不再使用静态 SLIRP 默认值，BSD 后端因此以“没有静态 IPv4”启动。现在两层都修了：

1. 实例显式声明 `[net] guest_ip/guest_netmask/guest_gateway`，`a20 run` 生成`-append 'a20.dhcp=0 a20.ip=...'`；
2. 板级 fallback 只在命令行含 `key=value` 时才信任它，kernel 路径不再被误认成网络配置。

浏览器门禁现在走实例默认 Firefox profile 和 QEMU 的板级 fallback（不传`-append`），同时仍然完成 HTTP/HTTPS DOM。

## 复现

```sh
make -j8 ARCH=x86_64 NR_CPUS=4 OPT=-O3 CONFIG_UBSAN=0 kernel-only
KDIR=$(make --no-print-directory ARCH=x86_64 NR_CPUS=4 \
  OPT=-O3 CONFIG_UBSAN=0 print-build-dir)
make -j8 ARCH=x86_64 NR_CPUS=4 OPT=-O3 CONFIG_UBSAN=0 "$KDIR/fat32.img"

python3 tools/net/tests/test-bsd-internet-kvm.py --kernel "$KDIR/kernel.elf" \
  --fat "$KDIR/fat32.img" \
  --world build/evidence/apk-bench-2026-10-26-input/world.img \
  --out /tmp/a20-bsd-internet

python3 tools/net/tests/test-bsd-desktop-kvm.py --kernel "$KDIR/kernel.elf" \
  --fat "$KDIR/fat32.img" --world build/images/xfce-x86_64.img \
  --out /tmp/a20-bsd-desktop --cpus 4,5,6,7 --smp 4

python3 tools/net/tests/test-bsd-desktop-kvm.py --kernel "$KDIR/kernel.elf" \
  --fat "$KDIR/fat32.img" --world build/images/xfce-x86_64.img \
  --out /tmp/a20-bsd-browser --cpus 4,5,6,7 --smp 4 --browser
```

输出目录必须不存在。桌面测试只修改镜像副本；不安装到生产 overlay。`--cpus`必须给出与 `--smp` 数量匹配的宿主 CPU，否则 BSD 执行器与 Firefox 会争用同一物理核。

## 已知边界和下一步

当前不支持：外部 IPv6 peer/RA、DHCP 客户端配置、UDP control/IP_PKTINFO、raw、 TUN、router、本机 PMTU/错误队列、完整分片 NAT、热拔插、worker 停机和回调撤销。 IPv6 只有本地 socket 回环；`if_set_addr6`/`if_del_addr6` 仍返回 `EOPNOTSUPP`。`ip route` 的主表路由事务会同时驱动公共 FIB 和 FreeBSD RIB；boot 隐含的默认路由删除也可被正确识别。F-Stack `v1.25` 携带的 FreeBSD 13.0 基线仍需安全维护评估。

默认切换已完成于 x86_64：桌面和服务器/基准主机构建现在默认使用 BSD。这以真实 Firefox、入站 TCP/UDP、动态控制面与公网 HTTP/HTTPS 证据为前提；非 x86_64 架构和 router/embedded 形态仍保持原默认，直到有同一级别证据。

下一阶段优先级：

1. 把 BSD 的 IPv6 ifaddr/RA、路由事务和通用 netlink FIB 同步补齐。
2. 接入 DHCP 客户端、UDP control、TUN/raw 与软路由/服务器能力。
3. 完成关闭/取消、低内存回收、重传、热拔插和长时间稳定性。
4. 运行在线 apk、ABI/IPv6、性能与容量矩阵，并把默认扩展到更多架构/资源档。
