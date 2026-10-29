# TUN：Linux ABI 与可替换协议后端

TUN 是由 `open("/dev/net/tun")` 和 `TUNSETIFF` 创建的内核服务，没有可探测的硬件。
`drivers/net/tun.c` 拥有名字、文件 ABI、等待与有界报文队列；协议接口、路由、IP 输入
和协议缓冲由当前后端拥有。公共头 `net/tun.h` 不包含 netif/pbuf/mbuf。

## 数据和生命周期

`net_stack_tun_create` 返回公共接口索引；驱动不保存协议对象指针。输出回调同步借用
报文 reader，复制后归还；禁止保留源指针或从回调反向调用协议后端。注入同步借用
输入字节，后端完成复制后调用自己的 IP 输入函数。destroy 与输入/输出执行互斥，
返回后不再回调该实例。物理 NIC 和 TUN 都经过公共过滤/NAT，TUN 不绕过策略。

控制操作以进程上下文 mutex 串行化；回调只取得短期队列锁。控制 mutex 可以调用
后端；后端回调不能取得该 mutex。接口名字查询使用独立表锁，不在持表锁时进入
后端。潜在 fault 的 usercopy 不持有协议执行锁。SETIFF 复制返回失败会撤销接口，
失败名字可再次使用；close 先停止后端回调，再清理全部队列。

持久接口关闭后保留接口、释放当前队列，并可按名字重新绑定；清除 persist 后关闭
即销毁接口。控制槽与协议槽分开计数，全部协议槽持久化后仍有重连控制额度。
`TUNSETPERSIST` 使用 Linux 命令203、标量参数；不再把命令200（TUNSETNOCSUM）冒充
持久化，也不把 TUNGETSNDBUF 冒充不存在的 TUNGETPERSIST。

## 功能与配置

- 支持 IPv4/IPv6 的 L3 TUN、`IFF_NO_PI`，以及带协议信息的 PI 报头。
- MTU/UP/索引/名字查询通过公共接口操作；MTU 直接读取后端，不维护第二份缓存。
  MTU 查询没有修改链路状态的副作用。IPv4 ifreq 设置使用正确的 sockaddr_in 地址偏移。
- `IFF_VNET_HDR` 的报头大小为10字节；支持 IPv4 checksum completion、TSO4、USO4。
  `TUNSETOFFLOAD` 只接受实际实现的能力位，IPv6 GSO 等明确拒绝，IPv6 普通报文可用。
- 修改接口/持久化需要所属网络域的 CAP_NET_ADMIN。当前仅初始网络域；TAP、多队列、
  delegated owner/group 和 IPv6 GSO 尚未实现，不返回假成功。GSO 的完整互操作矩阵不在本轮普通 IP/UDP 回归的通过范围内。

`NET_PROFILE_TUN_MAX_DEVS` 是协议接口上限（默认2）；控制槽为其两倍。每接口队列同时
限制包数与字节：embedded为2包，其他档32包，字节限额为深度×该档帧大小。超限丢包
并计数，不保留无界 jumbo 分配。控制结构有768 B/槽的编译断言，资源宏位于
`net/profile.h`；原44 KiB基础预算此前未包括完整 TUN 队列，现显式加上 TUN 控制及
payload 项。该项是结构/有效载荷预算，分配器取整与瞬时包转换缓冲需另计，不能当作
整机物理内存承诺。`/proc/net/status` 与 `/proc/a20/netmem` 的 tun 行报告当前槽、
排队包/字节、丢包、静态成本与队列额度。

## 回归

`tools/net-tun-abi-probe.c` 编译成同一个静态 Linux ELF，在 Linux KVM 和 A20OS 运行：
IPv4/IPv6 双向 UDP、64次 FIFO 回绕、IPv6 connected/lazy/unbound-connect 的实际线缆
地址/端口、MTU/UP、持久化重连、全部协议槽持久化后重连、24次创建/关闭，
以及只读 ifreq 的 EFAULT 清理。Linux 可能先输出 DAD/RS，探针按实际 UDP 报文匹配，
不把第一帧必为测试数据作为假设；IPv6 bind 等待真实 DAD 就绪，避免偶发使用
link-local 来源探测跨网段服务。宿主不加载模块；测试为固定 Linux guest 显式
安装匹配内核版本的 tun.ko，并在 manifest 记录其哈希。

本轮原始记录位于 `build/evidence/net-router-completion-20261029/`。TUN IPv6 探针还
发现并修复旧 UDP 发送路径对物理域 IPv6 的直接拒绝；延迟绑定现在先在后端绑定，
再发布前端地址/端口。该测试证明具体 ABI 路径，不代表已完成所有 IPv6 socket 选项。

原实现的入队索引固定从 head=0 起算，出队却推进 tail，第二个包即可读空槽并遗留
指针；现统一以 `(tail + count) % depth` 入队。字节计数与包计数同时随消费归还，
64个带独立序号的报文覆盖重复回绕，不能以首包通过代替 FIFO 验证。
