# 原版 lwIP 后端与公共边界

`NET_STACK=lwip` 现在链接固定的 lwIP 2.2.1 submodule 原始源码；没有改写
上游文件、替换内部头文件或带入旧 CUBIC/PCB lane 补丁。每个内核选定一个执行器和
transport 操作表，目前只支持初始网络域的单一后端，不支持运行中迁移连接。

```sh
git submodule update --init --depth 1 kernel/external/lwip-upstream
make ARCH=x86_64 NR_CPUS=2 NET_STACK=lwip NET_LANES=1 \
  NET_ROLE=router OPT=-O3 CONFIG_UBSAN=0 kernel-only
make NET_STACK=lwip check-net-backend-boundary
```

`NR_CPUS` 与协议执行并发度独立。原版强制 `NET_LANES=1`，多个应用 CPU 的 raw API
访问由核心锁串行化；IRQ 仍通过设备层推进。`NET_PROFILE=1` 也通过编译与组合预算
检查。原版现为 hosted 默认；旧分叉仅作为 `native` 衍生协议实现维护。

## 已拆分的状态

`net_socket_t` 只保存后端拥有的不透明 attachment。PCB、lane、BH payload ring、
accept 暂存位于 `backends/lwip/native.h` 的私有结构；仅 INET socket/accepted child
分配，UNIX/ALG 不携带这些数据。attachment 在发布前创建，close 先撤销 PCB/回调，
最后一个前端引用释放时才回收 attachment；失败创建也走同一释放入口。

`net/transport.h` 定义创建、关闭、绑定、连接、收发、选项与接口控制分派。公共发送
事务只观察 transport kind 的值快照，不借用 PCB 地址。公共队列以借用 reader 回调
复制源数据，不知道 pbuf 布局，也不保留源引用；复制失败不会发布未完成的消息。
接收地址/接口元数据跨边界按值复制。IPv6 raw 每报文选项使用同步发送的 PCB 值副本，
不再由 Linux ABI 临时覆盖共享 PCB。

地址配置为 A20OS 值类型；netlink 接口快照、地址/MTU 控制、packet socket、状态显示
与等待推进经公共执行器分派。`socket/`、`control/` 与公共 socket 内部头不包含 lwIP
头或私有字段，由 `check-network-backend-boundary.py` 检查。virtio/e1000/rtl8139 IRQ 与 procfs 已通过公共执行器分派；
TUN 的 lwIP 专用桥仍在，后续 BSD 接入还需完成包/设备侧适配；不能把这一阶段称为任意后端已可运行。

## 原版适配的能力差异

| 能力 | 原版后端 |
|---|---|
| TCP 拥塞算法 | 上游 Reno；CUBIC 请求明确返回不支持 |
| TCP 缓冲 | 使用上游编译期限额；小于真实限额的 per-socket 设置明确拒绝 |
| DHCPv4 服务 | dnsmasq `--dhcp-broadcast` 已验证；动态 `SIOCSARP` 更新不支持 |
| PMTU | 仅已实现的 DONT 模式；完整 PMTU 仍未完成 |
| 网络域 | 原有隔离软件 loopback 保留；尚无多个独立 lwIP 全局实例 |

这些差异会影响应用兼容性，选择原版默认时必须知晓。不能以原版后端返回假成功
维持表面兼容，也不能把关闭 conntrack 当作 NAT 路由器的容量方案。

2.2.1 的 IPv4 转发会清零校验和。当前 NIC 无 checksum offload，端口层在 linkoutput
前补齐软件校验和，再进入 NAT/过滤；独立宿主测试覆盖24种 TCP/UDP/ICMP 与 IP options
组合，并拒绝无法安全补齐的分片。输入 hook 防止 transit TTL/hop-limit 为0时回绕；
本地投递和组播不因该 hook 改变。完整分片 NAT、ICMP 差错翻译与 PMTU 仍待实现。

## 验证与证据

`build/evidence/net-upstream-20261029/` 保存冻结内核、ELF/镜像哈希、构建/运行参数和
失败样本。`upstream-private-services-kvm`、`upstream-release-services-kvm` 完成双网卡
IPv4/IPv6 转发、并发 NAPT、网关/源策略正负例、socket ABI、DHCPv4 与 DNS A 转发。
后者的同一探针还在 Linux/A20OS 校验无 INET attachment 的 UNIX socketpair 与
SO_REUSEADDR。较早的 marker 被 shell xtrace 插入导致失败，已保留，并停止在 readiness
前并发输出 xtrace。UNIX SO_SNDBUF 的探测暴露原有不支持范围，未改成假成功。

15组 Rust 状态机宿主测试通过；真实 C 队列的分配/复制失败回滚、C registry 并发测试
通过。软件 checksum 完成器、旧分叉并发 wiring 与公共边界检查通过。
性能与连接规模数据另见 [benchmark-method.md](./benchmark-method.md)。

## 驱动/诊断边界的后续提交

virtio、e1000、rtl8139 的 IRQ 统一传递借用的设备身份，不再包含 lwIP 头、取得协议
锁或假定驱动 slot 等于协议接口索引。所选后端解析绑定并负责入口序列化；virtio
兼容 poll 入口也委托给公共进展接口。e1000/rtl8139 的 TX 描述符回收仍由驱动拥有，
队列锁在通知后端前释放。procfs 的内存、设备与路由输出亦经后端表分派。

`lwip-driver-bridge-services-kvm` 与 `native-driver-bridge-services-kvm` 在上述最终
驱动接口构建上均再次通过三节点服务回归；KVM 中 virtio IRQ 已注册。e1000/rtl8139
本轮仅构建与边界检查，未进行对应硬件/模拟设备运行验证。TUN 尚保留私有 netif 桥。
