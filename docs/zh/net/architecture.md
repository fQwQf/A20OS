# 多场景网络架构与迁移约束

A20OS 网络子系统的核心架构目标是提供可裁剪的网络框架：统一系统调用接口由 A20OS 内核持有，TCP/IP 协议栈实现由网络域选择的具体后端承载。64 位非嵌入式实例统一选择 BSD/F-Stack，32 位、NOMMU 与嵌入式板卡保留原版 lwIP，MCU 默认不带网络栈；native 承接特定协议扩展。BSD 已完成 x86_64 单 NIC、IPv4 出站/入站、桌面 Firefox 和动态控制面；多架构移植的逐项运行证据见 [补全记录](router-completion.md)。原版 lwIP 的迁移决定和能力归属见 [backend-policy.md](backend-policy.md)。

## 分层与所有权

| 目录 | 职责 | 约束 |
|---|---|---|
| `kernel/net/socket/` | fd/句柄桥接、生命周期、等待、UNIX/ALG/packet socket | 逐步移除 INET 协议私有状态 |
| `kernel/net/control/` | 配置、netlink、路由控制事务 | 请求必须真实生效，不接受假成功 |
| `kernel/net/filter/` | 过滤、conntrack、NAT | 状态表只有一个所有者；变换前后校验完整 |
| `kernel/net/backends/lwip/` | 原版/衍生实现共用的私有 C ABI 适配器 | 不向公共层暴露 PCB |
| `kernel/net/backends/native/protocol/` | A20OS 维护的 lwIP 衍生协议实现 | 保留版权、来源和差异审计 |
| `kernel/net/backends/bsd/` | BSD 适配、执行器与 `ff_veth` 数据面 | x86_64 单 NIC、host 模式；IPv4 动态地址/MTU/link 已接入，IPv6 仅本地 |
| `kernel/net/crypto/` | 网络使用的密码辅助 | 不拥有 socket 或协议状态 |
| `kernel/include/net/` | 子系统契约、ABI 桥和资源定义 | 不暴露新后端的 PCB/mbuf 类型 |
| `kernel/external/f-stack/` | 固定版本上游 submodule | 上游保持只读；适配源码放后端目录 |
| `kernel/external/lwip-upstream/` | 原版 lwIP submodule | 不修改上游源码；已接入运行 |

源码由 `kernel/net/sources.mk` 显式列举；不递归收集后端目录中的任意 C 文件。公共 sockaddr 构造/端口辅助已移入 `socket/address.c`，地址归属查询通过后端值接口。公共 `socket_internal.h` 已移除 PCB、lane、BH ring 与 accept 暂存。它们由后端 attachment 独立拥有，操作经 `net/backend.h` 的 `socket` 模块分派；物理 NIC IRQ 与 TUN 均经公共入口分派；TUN 以接口索引和同步 reader 传递报文。

网络域为接口、地址、路由、端口空间和协议状态的所有权边界。一个域选择一个后端，存活连接不迁移。初期编译/启动时选择后端，不按每个包或每个 socket 任意混用栈。前端持有引用、权限和等待者，后端持有 TCP 状态、流控、重传及收发缓冲；不得新增一套与后端 socket 缓冲重复的通用 payload 队列。读写就绪、关闭与异步错误通过明确通知发布。

设备层使用容量、包所有权和完成事件契约：RX 在取走设备帧前尊重软件承载容量；预算耗尽但仍有工作时重新调度；TX 完成后才归还 DMA 缓冲。后端可以选择单循环、每核队列或自身线程模型，公共接口不规定现有 lwIP lane 哈希。所有跨边界缓冲必须规定借用/转移、释放上下文和是否可写，复制降级须计数。

## 场景与选项

- 极小 MCU：允许完全不编入 TCP/IP，保留 UART 等现有集群传输。
- 需要 TCP/IP 的资源受限设备：lwIP、静态预算和串行执行模型。
- 桌面/通用服务器：优先验证 FreeBSD 后端；成熟之前使用经过验收的 lwIP 配置。
- 软路由：独立 router 功能档，双网卡转发、动态 NAPT、真实网关/策略路由、ICMP/PMTU、分片与用户态 LAN 服务都必须验收，不能仅开启两个 forwarding 宏。

资源档位与后端选择独立。`NET_PROFILE` 定义预算；后端功能定义能力。不能以增加预算或关闭 SMP 冒充不回退。后端未实现的选项返回准确错误；默认值变化属于可观察行为变化。

当前 64 位非嵌入式实例默认 `NET_STACK=bsd NET_LANES=1`；32 位、NOMMU 与嵌入式板卡默认 `NET_STACK=lwip`，并可显式选择 `NET_STACK=native`。MCU 为 `NET_STACK=none`。原版适配及能力差异见[lwip-upstream.md](lwip-upstream.md)，BSD 当前边界见[bsd-port.md](bsd-port.md)。原版 lwIP 强制 `NET_LANES=1`；native 的 `NET_LANES` 选择执行并发度，`NET_PROFILE=1/2/3` 选择资源预算；它们均不是 QEMU 的网络拓扑选项。

**实例配置是选择入口。** 每个 `instances/<name>.toml` 的 `[net]` 段可以写`stack`、`lanes`、`resource_profile`、`role`；`tools/a20` 与`make NET_INSTANCE=<name>` 都会把它转换成 `NET_STACK/NET_LANES/NET_PROFILE/ NET_ROLE`。构建只链接选中的后端，启动时按名字选择，未编译进来即报错，不做回退。字段、校验规则与迁移说明见 [instance-config.md](instance-config.md)。

后端接口（模块划分、能力位、所有权、迁移对照）见[backend-interface.md](backend-interface.md)。公共代码只通过`net/backend.h` 的模块与 `net/stack.h` 的 facade 接触后端。公共路由事务见 [routing.md](routing.md)，NAT 所有权与限制见 [nat.md](nat.md)，对照测量口径见 [benchmark-method.md](benchmark-method.md)。`NET_ROLE=host/router` 独立选择是否编入IPv4/IPv6 transit转发，默认host；使用与真实三节点验证见 [router.md](router.md)，不能据此宣称完整软路由已经成熟。 LAN 服务及每报文元数据契约见 [lan-services.md](lan-services.md)。

## 上游依赖策略

BSD 候选采用独立 F-Stack 项目的 submodule，不导入完整 freebsd-src，见 [bsd-port.md](bsd-port.md)。普通 lwIP 构建不依赖初始化 FreeBSD；原型、编译成功、数据面可用和验收通过分别记录。适配层不得在构建过程中改写 submodule。更新要提交上游 SHA、变更审计和回归结果。

native 衍生实现携带 PCB lane、并发、分配器、CUBIC 和窗口等修改。直接恢复会改变真实行为。先保留冻结参照并清点每项改动，再将可通过 hook/port 实现的部分迁出。原版候选必须验证 TCP/UDP、IPv6、TUN、关闭/取消、低内存和外部 peer；不得只凭编译通过替换。

## 验收与尚待工作

同一 Linux ELF、相同 NIC/链路与 CPU 资源，对比正确性、吞吐、p99、进程/guest CPU 时间与 CPU/字节、连接规模、分配失败、峰值/回收后内存。保留编译选项、上游 SHA、 ELF/镜像哈希、完整失败样本和原始日志。SLIRP 与宿主回环有不同路径，不能作为线速比较。

接下来按独立提交推进：容量与进展修复；后端执行/控制契约；路由与 NAPT 的 Rust 状态核心；软路由多节点门禁与 LAN 服务；FreeBSD 运行时适配；原版 lwIP 候选；最终后端对照。未完成的项不宣称可用，不提前选择服务器默认。设计会随真实适配结果收窄，不为将来可能的后端引入没有调用者的接口。
