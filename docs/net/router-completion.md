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
| IPv4 PPPoE 宽带接入 | discovery/session、认证、MTU、断线重连及真实对端 | Linux PAP、CHAP 双网口回归通过 |
| IPv6 | RA/SLAAC、DHCPv6-PD 与 LAN 前缀/路由发布的对端验证 | WAN RA/SLAAC 双网口通过，DHCPv6-PD 与 LAN 发布待完成 |
| 策略路由 | 多 FIB 与源策略、IPv4/IPv6、正反向及阴性对照 | 待完成 |
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
的 lwIP 启动关机 smoke 通过。IPv6CP 和 PPP DNS 选项尚未接入。

## WAN IPv6 RA 与 SLAAC

`ra-before` 在原内核收到有效 WAN RA 后未观察到 SLAAC DAD。`ra-wire-final`
验证了通过 `a20.ipv6_ra=en3` 启用后，LAN RA 和错误 Hop Limit 的
RA 被拒绝。有效 RA 生成了可达的 WAN 地址和可用的默认路由。PCAP
记录了双向 LAN 转发、零寿命撤销、无续期自然过期和收到新通告后恢复。
同一 x86_64 内核通过了 TUN 与 Linux CHAP PPPoE 回归。四个 64 位
架构通过 BSD socket 回归，ARM32 和 RV32 通过 lwIP 启动关机 smoke。
