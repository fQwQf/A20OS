# F-Stack 实例统一与软路由补全

本工作在 `codex/fstack-router` 独立 worktree 进行。目标是 64 位非嵌入式实例统一使用
F-Stack，并完成软路由功能与运行验证；编译通过不代替网络运行验证。

用户确认按用途划分，并进一步明确 **32 位保留 lwIP**。因此桌面、服务器、
发布/基准和普通带 MMU 的 64 位虚拟机使用 F-Stack；32 位、NOMMU 与明确的
嵌入式板卡保留 lwIP，MCU 默认 none。静态驱动打包不等于嵌入式用途。

## 验收范围

| 项目 | 所需证据 | 状态 |
|---|---|---|
| 64 位非嵌入式实例统一后端 | 实例检查、四架构真实 BSD 内核编译及 QEMU 网络收发 | 基础迁移完成 |
| BSD 网卡 IPv4 分片 NAT | 重组后过滤/NAT；乱序、重叠、超时、配额及回包验证 | x86_64 双网口 QEMU 通过 |
| BSD 连接容量 | 可配置预算、满表行为、数千连接负载及回收 | 4096 / 16384 条双网口 QEMU 通过 |
| PPPoE 宽带接入 | discovery/session、认证、MTU、IPv6CP、IPCP DNS 与断线重连 | Linux PAP、CHAP 双栈和 IPv4 回归通过 |
| IPv6 | RA/SLAAC、DHCPv6-PD 与 LAN 前缀/路由发布的对端验证 | WAN RA/SLAAC、Kea 委派与续租、Linux LAN 自动地址与转发、租约撤销和重编号通过 |
| 策略路由 | 多 FIB 与源策略、IPv4/IPv6、正反向及阴性对照 | IPv4 转发双 WAN 通过，IPv6 与本机 socket 待完成 |
| WAN 配置与恢复 | DHCP/静态配置、租约更新、链路恢复、服务监督与持久配置 | 待完成 |
| 稳定性与性能 | 多客户端持续流量、吞吐/延迟/丢包/内存、故障恢复 | 待完成 |
| 真机验证 | 具体硬件 PCI ID、双网口收发、中断/链路恢复和长时负载 | 待硬件验收 |

## 初始审计

- Makefile 支持 x86_64 BSD router，但实例校验器仍拒绝 router，需统一。
- BSD 对象编译工具写死 amd64 机器头、x86 指令选项和宿主 binutils。
  切换 RISC-V/AArch64/LoongArch 实例前必须完成实际移植。
- 现有文档含历史状态：README 与实例说明还声称 BSD 不能构建，部分状态页
  尚未同步已实现的动态 NAPT/ICMP RELATED。后续以源码和本 worktree 新证据更新。
- 原有三节点证据路径未随仓库提交；本工作重新生成验证结果，不沿用口头通过结论。

构建及运行证据保存在本 worktree 的 `build/evidence/fstack-router/`。

## 实例迁移进展

- 已将普通 64 位实例明确设为 BSD，普通 ARM32/RV32 实例明确保留 lwIP；
  `NET_INSTANCE` 同时传递 NOMMU，CLI 已允许 BSD router。
- 四个 64 位架构均已通过真实内核网络测试：TCP/UDP 与 IPv6 回环、512 个
  空闲 socket、四路外部 TCP 共 256 KiB、外部 UDP 16×512 B、正常关机。
  这些是网络基础回归，不代表桌面应用、软路由转发或真机性能已验收。
- 修复静态驱动部署未启动 BSD 执行器、UMA 引导元数据初始化顺序，以及
  LoongArch 新内核线程中断关闭导致远端 TLB shootdown 超时。
- LoongArch 使用显式的任务 TLS 访问器，其余架构使用编译器 emulated TLS；
  两种路径的所有者隔离、配额、回收已通过 ASan/UBSan host 测试。
- 32 位 BSD 试验已按用户最新范围撤回；保留试验日志用于追溯，不列为支持能力。
- ARM32/RV32 的 lwIP 构建与启动/关机 smoke 已通过。修复了 lwIP 的
  `set_flags` 声明与后端接口不一致的问题：RV32 的 `uint32_t` 是 unsigned long，
  用 unsigned 代替会导致函数指针类型检查失败。

## 本轮验证记录

四架构最终运行结果均为 PASS（2 vCPU）。下列目录位于
`build/evidence/fstack-router/`，各 `manifest.json` 包含完整内核 SHA-256、
QEMU 参数、逐项通过结果；串口日志为 `serial.log`。

| 架构 | 证据目录 | 内核 SHA-256 前 16 位 |
|---|---|---|
| x86_64 | `x86_64-final-network-02` | `278b56f5fefe30ae` |
| AArch64 | `aarch64-final-network-02` | `5dd57aa3f26c09d4` |
| RV64 | `riscv64-final-network-02` | `3c8873a49a1b642c` |
| LoongArch64 | `loongarch64-final-network-02` | `a50b0a61892bbd30` |

复现构建：`make -j6 ARCH=<架构> NET_STACK=bsd NR_CPUS=2
DRIVER_DEPLOYMENT=embedded RAMFS_USER=1 RAMFS_NET_TESTS=1 kernel-only`，
LoongArch64 追加 `UART_CMDLINE=y`。测试命令为
`python3 tools/net/tests/test-bsd-backend-qemu.py --arch <架构>
--kernel <实际生成的内核路径> --out <新证据目录>`。

- `tools/a20 check`：全部 103 个实例通过。
- `python3 -m unittest discover -s tools/tests -p test_a20.py`：共 212 项，
  211 项通过、1 项跳过；最新 32/64 位策略又单独通过 `TestRepositoryInstances`。
- `runtime-host-01`：内存、编译器 TLS、显式 TLS 的 ASan/UBSan 测试全部通过，
  `inputs.json` 的源码哈希已复核。
- `tools/a20 test smoke-arm32` 与 `tools/a20 test smoke-riscv32`：lwIP
  构建、启动检查与正常关机通过；这两项不是 32 位完整网络性能验收。

## IPv4 策略路由验证

`policy-v4-icmp/manifest.json` 和同目录的三份链路抓包记录 x86_64 三网口回归。
两个 LAN 源访问同一个目的地址时分别走预期的 WAN，另一条 WAN 无对应报文。
同一套测试覆盖回 LAN、TTL 超时回包、空表回退、最长前缀、metric 接管与替换、
撤销规则后使用主表，以及拒绝不可达的备用网关。命令见 [IPv4 路由说明](./routing.md)。

## IPv4 分片验证

BSD 网卡入口先校验和重组，出口在重新分片前处理 NAT。重叠及冲突终点
丢弃整组，队列按入口和 FIB 隔离，等待分片不占用 NIC RX token。
出口沿用入站 DNAT 的连接绑定，避免为已改写的正向元组重复建表。

- `fragment-wire-04/manifest.json` 记录双网口 x86_64 回归的 10 项通过结果。
  正反向 UDP 报文均超过 MTU，测试验证内容、地址、端口、TTL、IPv4/UDP
  校验和和重分片，并覆盖过滤、重叠、配额、超时、跨接口隔离和故障后转发。
  同目录的 `link0.pcap`、`link1.pcap` 是两侧实际报文。
- `fragment-{x86_64,aarch64,riscv64,loongarch64}-final` 记录四个架构的
  TCP/UDP/IPv6 回环、并发外部收发和关机回归。
- `fragment-nat-host-03` 覆盖 TCP DNAT 正向出口复用，默认、嵌入式和
  高时钟三个生产状态机配置均通过。独立过滤回归为 `fragment-filter-host-02`。
- 实现边界和命令见 [NAT 说明](./nat.md)。lwIP/native 保持分片保护策略。

## 连接跟踪容量验证

BSD 路由默认 4096 条，可在构建时通过 `NET_CONNTRACK_ENTRIES` 或实例
`[net].conntrack_entries` 选择容量。空闲链表使分配无需扫描全表，
BSD 执行器在无流量时也定期回收。诊断输出报告容量和表的静态字节数。

| 容量 | 条目与索引 | 初次填满 | 超时回收 | 二次填满后累计建连 | 证据目录 |
|---|---:|---:|---:|---:|---|
| 4096 | 231424 B | 4096 | 4096 | 8192 | `conntrack-wire-default-final` |
| 16384 | 925696 B | 16384 | 16384 | 32768 | `conntrack-wire-max-final` |

双网口 x86_64 QEMU 用真实 UDP 报文验证唯一 WAN 映射、满表拒绝新流、
存量映射回包和无流量时的回收。两组均为零驱逐。为缩短测试，填满后将
UDP 空闲超时设为 2 秒。该测试用于验证容量与状态，吞吐量另行验收。

四架构 BSD 网络回归位于 `conntrack-sockets-<arch>-final`。主机 NAT/过滤
回归位于 `conntrack-nat-final` 和 `conntrack-filter-final`。实例 CLI 共
214 项测试，213 项通过、1 项跳过。ARM32/RV32 lwIP 启动关机 smoke 通过。

## PPPoE 数据面准备

BSD TUN 支持 IPv4 接口路由。配置 TUN 的 /32 地址后，可通过
`RTM_NEWROUTE` 指定出口设备并添加无网关默认路由。Linux 主路由表
254 同步到 F-Stack RIB，关闭 TUN 时清除与该接口关联的路由。
TUN IPv4 复用 BSD 重组、过滤和 NAT；普通网卡的 AF_PACKET 捕获由
BSD 执行器交付给套接字，并使用正确的公开接口索引。

`tun-route-wire-07` 用双网口 x86_64 QEMU、无 IPv4 地址的 WAN 网卡和
固定 PPPoE 会话验证了 1492 字节 MTU、LAN 出口 NAT、回包以及关闭后
重新安装默认路由。会话封装和 MTU 依据 [RFC 2516](https://www.rfc-editor.org/rfc/rfc2516)。
证据目录包含两侧抓包和内核哈希。此回归使用固定会话参数，PADI/PADO、
LCP、PAP/CHAP 和 IPCP 的对端协商见下一节。

构建方法与分片回归相同，运行
`python3 tools/net/tests/test-bsd-router-tun.py --kernel <kernel.bin>
--out <新证据目录>`。四架构网络回归位于 `tun-socket-<arch>-dataplane`，
分片转发复测位于 `tun-fragment-final`。

## PPPoE 拨号与认证

`pppoe` 已接入 BSD TUN。真实 Linux `pppoe-server` 与 `pppd` 对端的
PAP 和 CHAP-MD5 两套回归记录为 `pppoe-pap-racefix`、`pppoe-chap-racefix`。
两套测试均完成 IPCP、超过 MTU 的 LAN UDP 往返、对端断线换地址、
复用原五元组以及错误密码拒绝。CHAP 测试还暂停了对端进程，验证 LCP
Echo 超时会触发重新拨号。

断线后从 `198.18.0.10` 改拨到 `198.18.0.11`。TUN 关闭清除相应出口的
MASQUERADE 绑定，保留其他接口的状态。生产状态机的选择性清理验证
位于 `pppoe-nat-host-final`。使用和复现方法见 [PPPoE 配置](./pppoe.md)。

固定会话复测曾在第二次打开 TUN 时触发内核异常，回溯位于
`pppoe-tun-regression`。`complete()` 现于发布完成并释放锁之前收集唤醒，
避免请求者返回后仍访问栈上的完成对象。修复后 20 轮连续打开、关闭和
重开均通过，记录为 `pppoe-tun-fixed-01` 至 `pppoe-tun-fixed-20`。
四架构 BSD 网络回归为 `pppoe-sockets-<arch>-racefix`。ARM32/RV32
的 lwIP 启动关机 smoke 通过。双栈和 DNS 的后续验证见下文。

## WAN IPv6 RA 与 SLAAC

`ra-before` 在原内核收到有效 WAN RA 后未观察到 SLAAC DAD。`ra-wire-final`
验证了通过 `a20.ipv6_ra=en3` 启用后，LAN RA 和错误 Hop Limit 的
RA 被拒绝。有效 RA 生成了可达的 WAN 地址和可用的默认路由。PCAP
记录了双向 LAN 转发、零寿命撤销、无续期自然过期和收到新通告后恢复。
同一 x86_64 内核通过了 TUN 与 Linux CHAP PPPoE 回归。四个 64 位
架构通过 BSD socket 回归，ARM32 和 RV32 通过 lwIP 启动关机 smoke。

`rs-before` 在没有主动通告的对端上复现了 WAN 无法主动发现路由器。
`rs-wire-final` 验证了 WAN 上的 Router Solicitation 组播、校验和及
重试，错误 Hop Limit、错误链路层选项和零寿命通告均不会终止探测。
对端在收到第二次请求后才发送有效通告。随后观察到 SLAAC、双向转发
和停止探测。管理命令关闭并重开 WAN 时，重新发送了三次请求。


## DHCPv6-PD 首次租约

`pd-stock-02` 复现了原版 BusyBox 因 `RTM_GETADDR` 缺少 WAN IPv6
地址而无法开始。BSD 地址现在从实际接口地址表读取，返回给 Linux
兼容层，并保留链路本地作用域、前缀长度和 DAD 状态。`pd-route-final`
验证了 BusyBox 读到链路本地和 SLAAC 地址，与 Kea 完成四报文交换，
并用收到的 /56 在 LAN 配置一个 /64 后与 Linux 双向通信。测试中的
LAN 地址和上游返程路由由测试程序配置。

`pd-renew-baseline` 进一步复现了续租失败。BusyBox 在切换到 IPv6 UDP
续租套接字时请求绑定 `en3`，当时 BSD 的 `SO_BINDTODEVICE` 对次级
网卡返回 `EOPNOTSUPP`。四个 64 位架构通过 BSD socket 回归，x86_64
通过 RA 回归，ARM32 和 RV32 通过 lwIP 启动关机测试。

### 续租与 IPv6 UDP 绑定

`pd-renew-bind-wire` 验证了 BusyBox 同时申请 IA_NA 和 IA_PD 时，可
通过绑定 `en3` 的套接字接收第一个单播续租 Reply。双网口对照还验证了
绑定状态拒收 LAN 报文、拒绝从 LAN 作用域发送，以及解除绑定后使用
Linux 公共接口索引进行 LAN 收发。四个 64 位架构通过 BSD socket 回归。

`pd-renew-01` 记录了 BusyBox 1.37.0 使用 `-r no` 时在其 UDP 发送
函数中解引用空地址指针的用户态崩溃。首次仅前缀委派继续通过。


### 仅前缀续租与 IPv6 原始套接字

原版 odhcp6c 使用 `-N none -P 56 -F` 与 Kea 交换报文。
`pd-odhcp6c-stock-verified` 在 WAN 抓包中确认四报文交换和续租都只含
IA_PD，Kea 对 `2001:db8:400::/56` 返回 24 秒首选寿命和 36 秒
有效寿命。`bound` 脚本事件收到扣除启动等待后的剩余寿命，
`updated` 事件收到续租后的 24 秒和 36 秒。测试仍手动将该前缀的
第一个 /64 配到 LAN 并验证双向转发。
[OpenWrt odhcp6c](https://github.com/openwrt/odhcp6c)

`odhcp-netlink-raw-wire-verified` 从第二张网卡收到用户态发送的
ICMPv6 报文并校验了校验和、跳数与接口作用域，同时验证了 ICMPv6
过滤和接收。该测试还使用 `NETLINK_ROUTE` 数据报套接字连接内核、
订阅链路组，观察网口关闭事件，并在退订后确认未收到重开事件。
该轮使用内核处理 WAN RA，尚未验证 odhcp6c 通过 SIGIO 响应链路
变化的恢复路径。


### LAN 地址与直连前缀的租约寿命

`lan-addr6-lifetime-shared` 通过 rtnetlink 在同一 LAN /64 上配置两个
有寿命的地址。重复的排他添加返回 `EEXIST`。测试随后续租其中一个
地址，越过原有效期后仍收到该地址的回包和该前缀的转发报文。新租约
到期后，LAN 上不再出现目标主机的邻居请求、转发报文或路由器地址的
回包。主机端测试覆盖 `IFA_CACHEINFO` 的寿命校验和响应布局。该测试
直接设置地址寿命，仍需将 DHCPv6-PD 租约事件接到 LAN 配置和 RA。


### LAN 自动配置、撤销与重编号

`pd-lan-verified` 使用 A20OS 路由器、运行 Kea 与 radvd 的 Linux WAN、
运行 SLAAC 的独立 Linux LAN 三个节点。原版 odhcp6c 续租只带 IA_PD，
Linux LAN 从子网编号 `0x2a` 对应的 /64 自动生成地址，并使用通告
的默认路由与 WAN 通信。抓包校验了通告的跳数、链路本地源地址、校验
和、前缀寿命与网口隔离。测试撤销并恢复 WAN 默认路由时，LAN 的
默认路由随之变化，委派前缀继续有效。

停止 Kea 后，通告撤销过期前缀，Linux 地址和 A20OS 的 LAN 地址到期。
Kea 改用另一个 /56 恢复后，原有发布进程重新获得委派，Linux 使用新
地址恢复通信。正常退出时，发布进程撤销新通告并移除它管理的地址。
这轮同时验证了 UDP 546 复拨需要的 Linux `SO_REUSEADDR` 和
`SOCK_CLOEXEC` 行为。多 LAN 部署及进程或整机重启后的旧前缀处理
仍需运行验证。


### 双栈 PPPoE 与 IPCP DNS

`pppoe-dual-chap-final` 和 `pppoe-dual-pap-silent` 使用 Linux
pppoe-server 和 pppd。在首次拨号和重拨中，抓包确认双方交换
IPv6CP 配置确认，对端建议的地址标识得到采用，IPv6 数据使用 PPP
协议号 `0x0057`。Linux 可以探测 A20OS 的 PPP 链路本地地址，
LAN 可通过 IPv6 默认路由和 PPP 到达 WAN；同一会话也通过 IPv4 NAT
转发。暂停 PAP 对端会触发 LCP Echo 超时，恢复后重拨成功。

IPCP 确认了 `192.0.2.53` 和 `192.0.2.54`。解析器文件在重拨后
重新生成并在正常停止后清理。`pppoe-ipcp-dns-reject` 验证对端拒绝
DNS 选项时 IPv4 仍可拨号和转发。此轮 IPv6 转发使用测试配置的
LAN 前缀。PPP 上的自动委派与重拨恢复见下节。


### PPP 上的前缀委派与链路恢复

`pppoe-pd-verified` 使用 Linux pppoe-server、pppd、Kea 和独立
Linux LAN 主机。`pd-lan` 在 PPP 接口出现前启动，随后在 PPP
上从 Kea 取得并续租 `2001:db8:400::/56`，为 LAN 发布
`2001:db8:400:2a::/64`。Linux 客户端通过 SLAAC 获取地址并与
WAN 双向通信。

关闭 Linux PPP 对端时，旧 TUN 的 `RTM_DELLINK` 通知带原接口索引，
LAN 收到旧前缀和默认路由的撤销通告。重拨复用了接口索引，原发布
服务启动新的 DHCPv6 客户端，取得 `2001:db8:500::/56` 并恢复
转发。抓包校验了两次拨号的 DUID、IAID 和委派寿命。模拟 ISP
的返程路由由测试对端配置。Kea 对端测试保留 pppd 协商出的本地
链路地址，将其以单地址形式配置，以免 Kea 绑定到 Linux PPP
地址对中的远端地址。
