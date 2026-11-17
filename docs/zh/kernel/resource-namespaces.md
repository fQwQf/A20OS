# 资源命名空间

`/proc/<pid>/ns/{uts,ipc,net,cgroup}` 现在标识真实且被引用的命名空间对象。`clone`/`clone3` 创建成员关系，普通 fork 继承，`unshare` 新建一个域，`setns` 加入由已打开命名空间描述符固定的那个对象。命名空间所有者固定为 user namespace 引用；由更有特权的任务打开描述符不会改变其归属。组合的 NEWUSER 请求先创建该域，再处理资源命名空间。分配失败会回滚命名空间成员关系与凭据。

| 域 | 隔离的资源 |
| --- | --- |
| UTS | 经 uname、setter 与 procfs 暴露的 hostname/domainname |
| IPC | SysV 消息队列、信号量集、共享内存 ID/key，POSIX MQ 名字与计数 |
| Network | socket 端口/监听查找、抽象 Unix 地址、loopback 标志与 MTU、路由/链路与 socket 诊断转储、procfs 网络视图、新挂载的 sysfs 网络视图 |
| Cgroup | 命名空间根、procfs 中相对的 cgroup 路径、新挂载 cgroupfs 视图的根 |

切换 IPC 命名空间后，已存在的 SHM 映射与 MQ 描述符仍可用。最后一个命名空间引用会移除其 SysV 资源并 unlink 其 MQ 名字；映射与已打开的 MQ 文件保留各自的底层存储。socket 在 setns、fork 与 accept 之间保持创建时的命名空间。新建的网络命名空间只含一个 down 状态的 loopback 设备，把它 up 起来即可使用私有的 IPv4/IPv6 TCP 与 UDP；它没有物理网卡、外部路由或 DNS 配置，物理网络事件无法泄漏进来。已存在的挂载保持挂载时的网络视图，新挂载的 sysfs 捕获新命名空间。

这些改动不提供 veth 设备，也不做物理网卡迁移。IPC 与 socket 存储仍使用内核有界的共享分配池。命名空间 procfs 描述符沿用仓库既有的 `type:[inode]` 可读格式；nsfs magic-link 与命名空间所有者 ioctl 的兼容属于另外的工作。接口保留并以隔离为后盾：不支持的操作返回错误，而不是在没有隔离的情况下成功。

门禁为 `make -j8 smoke-resource-ns-x86_64` 与 `make -j8 smoke-resource-ns`，两者运行同一套资源命名空间回归，外加既有的 mount、PID、user namespace 与 procfs 用例。覆盖范围包括跨域拒绝、末次引用后 IPC 的重复拆除、命名空间描述符生命周期、旧 SHM/MQ/socket 句柄、IPv4/IPv6 loopback、继承的成员关系、挂载根与能力归属；目标平台是 x86_64（4 CPU、抢占）与 RISC-V。`make -j8 smoke-vm-fault-x86_64` 另覆盖真实硬件的 GS 读写、fork/exec/COW、稀疏 unmap 与 `no_new_privs`/seccomp ABI 行为。
