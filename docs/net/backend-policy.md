# 原版 lwIP、FreeBSD 与 A20OS 原生能力

A20OS 网络栈支持 `lwip`、`native`、`bsd` 三个协议后端。x86_64 桌面和服务器/基准主机默认使用 `bsd`；其它架构默认使用 `lwip`。默认 `lwip` 使用未修改的上游发布源码；历史衍生协议实现迁入 `backends/native/protocol`，由 A20OS 承担维护。性能差异不作为保留旧后端的理由。

## 桌面和服务器的目标后端

普通桌面与服务器场景采用 BSD 网络栈作为默认配置。性能对照用于基准定位与回归验收。x86_64 桌面/服务器默认已切换到 BSD。BSD 已完成 x86_64 单 NIC、IPv4 出站/入站、公共 socket、动态地址/MTU/路由事务和 XFCE 桌面 Firefox HTTP/HTTPS DOM 垂直切片，完整验收体系与剩余验收项如下，且不允许 `NET_STACK=bsd` 静默回退到 lwIP。当前进度见 [bsd-port.md](./bsd-port.md)：

1. BSD 自身初始化、线程/TLS、分配器、锁/等待、时钟/callout/epoch 在 A20OS 执行。
2. 接入同一套 virtio/包 I/O 与公共 Linux socket 前端，执行真实 BSD TCP/UDP。
3. DNS、公网 HTTP/HTTPS、在线 apk 流程，以及现有 IPv4/IPv6/ABI 回归通过；抓包与 `/proc/net/status` 明确显示实际 BSD 后端，不借用宿主 socket。
4. 按桌面/服务器预算验证并发连接、关闭回收、吞吐、p99 和 CPU/字节，记录限制。

嵌入式保留原版 lwIP，native 继续作为独立衍生后端推进；软路由后端的选用仍须满足完整转发/NAT/服务验收。`NET_PROFILE` 是预算档位，不是场景后端选择器。

## 为什么收敛魔改

lwIP raw API 建立在串行核心访问约束上。NO_SYS 模式不能让多个上下文同时访问核心，OS 模式采用 tcpip_thread/core locking，而非任意拆分协议内部结构到多个核。参考 [上游并发说明](https://www.nongnu.org/lwip/2_1_x/multithreading.html) 和[NO_SYS 约束](https://www.nongnu.org/lwip/2_0_x/group__lwip__opts__nosys.html)。

现有分叉修改 PCB 布局/链表、lane 桶、定时器、内存池和拥塞控制；socket 桥又读取这些字段。UDP connect 漏掉 wildcard 桶、重复登记形成自环，已被真实 Linux ELF 复现。这是偏离上游执行模型的维护成本，不能仅靠增加锁或资源容量解决。小而明确的上游缺陷修复仍然合理，应有回归、来源和回传记录；深度分叉却长期当作普通 lwIP 依赖，会同时失去上游可升级性和自研栈的清晰所有权。

## 能力归属

| 能力 | 长期所有者 | 约束 |
|---|---|---|
| socket ABI、fd、权限、等待与错误发布 | A20OS socket 前端 | 不暴露 PCB/mbuf；每个 socket 使用其网络域后端 |
| 接口身份、地址、FIB/策略、过滤/NAT 策略、追踪 | A20OS 公共控制与策略层 | 单一事实来源；后端执行副本需同步及失效事务 |
| DMA、NIC 队列与卸载、收发完成 | A20OS 设备与包 I/O 层 | 明确所有权、背压、批量预算和释放上下文 |
| TCP 状态机、重传、拥塞控制、协议缓冲与分配器 | 各协议栈后端 | lwIP 使用上游模型，FreeBSD 使用自身模型，不拼接跨栈 TCP 核心 |
| ARP/ND、分片重组、ICMP/PMTU 等协议执行 | 域内指定的协议实现 | 一个状态只有一个所有者，不独立维护两套协议状态 |
| DHCP/DNS/RA 等管理服务 | 用户态 Linux 程序 | 使用真实 socket 能力，不足之处明确报错 |

NAT/转发需要重组与 ICMP 差错翻译时，必须把执行顺序纳入后端契约，不能在 Ethernet 层只改第一片的端口，也不能放两套互不关联的邻居/重组表。公共层持有策略，不等于公共层接管全部协议状态。

## 原版 lwIP 接入规则

`kernel/external/lwip-upstream` 是独立 submodule，固定发布标签`STABLE-2_2_1_RELEASE` 的提交 `77dcd25a72509eb83f72b033d219b1d40cd8eb95`。这是候选基线，不声称覆盖所有最新开发提交。上游目录不写入 A20OS 更改；port、hook、`lwipopts.h` 和适配代码放在第一方后端目录。可以使用上游支持的[hook](https://www.nongnu.org/lwip/2_1_x/group__lwip__opts__hooks.html)，不能靠同名头文件偷偷替换内部结构以维持旧 ABI。

当前 x86_64 默认 `NET_STACK=bsd`；其它架构默认 `NET_STACK=lwip` 链接原版；`NET_STACK=native` 选择衍生协议实现，见[适配记录](./lwip-upstream.md)。获取源码不等于接入成功，不能默认回退后冒充原版。候选采用每域串行协议执行，SMP 仍用于应用与 I/O，不宣称单域能并行执行多个 TCP 核心。多域独立实例需先隔离上游全局状态，不能复制一把锁就宣称支持。

旧 CUBIC、wnd_limit、PCB lane、per-lane 池不搬入原版核心。原版不具备的选项明确返回不支持；需要这些能力的场景选择其他后端，或者选择 native 后端。切换后端不能悄悄改变已支持的系统 ABI，必须有能力说明和对应验收。

## 原生自研栈的范围

`native` 从旧分叉提取，并非从零编写。原有 lwIP 版权、COPYING、基线与差异审计随源码保留；lane、CUBIC、窗口/分配器扩展由该后端维护，绝不带入原版。现阶段二者复用`backends/lwip/` 内的私有 C ABI 适配器；公共 socket 层不依赖这套 ABI。协议内部仍有 lwIP 类型与文件名，这是来源事实，不用机械改名掩盖。

新增能力先确定归属：策略与 Linux ABI 放公共层，TCP/重组/邻居状态由各后端单独拥有。 Rust 优先用于有界状态、解析和队列所有权；不能把保留 C 实现的目录迁移当作 Rust 重写。

## 迁移顺序

1. 冻结旧分叉及功能、性能、失败样本；公共层不再新增私有字段依赖。
2. 隔离 INET 私有状态，建立有实际调用者的 socket、包 I/O、控制和能力契约。
3. 接入原版 lwIP，以同一 Linux ELF、相同 NIC/链路复跑 ABI、NAT、转发和服务。
4. 接入 FreeBSD，比较正确性、吞吐、p99、CPU/字节、连接规模与内存成本。
5. 桌面、服务器已确定以 BSD 为最终后端；完成上述真实联网验收后切换默认。嵌入式与软路由分别按资源和能力验收。

公共 socket 私有状态隔离和原版 lwIP 运行适配已完成第一阶段；物理 NIC/TUN 的公共分派也已接入；完整兼容矩阵与 BSD 运行后端仍未完成。 ICMP RELATED 与转发 MTU 差错已回归；完整分片 NAT、本机 PMTU/差错队列与生产软路由整体验收仍未完成。
