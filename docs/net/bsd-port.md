# BSD 网络后端接入记录（2026-10-29）

已将完整 FreeBSD 源码子模块替换为独立项目
[F-Stack](https://github.com/F-Stack/f-stack)，固定提交
`52514377b89750ffc03a93f401c22d2d4f269d5f`。它将 FreeBSD 网络实现和用户态适配一起
维护，不要求导入整个 FreeBSD OS。另一个真实候选是
[libuinet](https://github.com/pkelsey/libuinet)，其 README 说明从 FreeBSD 9.1 起步。
本轮选择 F-Stack 作为接入起点，不根据其上游性能宣传推断 A20OS 性能。

## 实际探测结果

源码位于 `kernel/external/f-stack`；本地使用 sparse-checkout 读取 `lib/freebsd/mk`
相关目录，超级项目以 gitlink 固定版本，普通 lwip/native 构建不依赖其初始化。

```sh
git submodule update --init --depth 1 kernel/external/f-stack
make -C kernel/external/f-stack/lib -n all
```

本机第二条命令 exit 2：`lib/Makefile:65` 报未安装 DPDK。原始记录保存在
`build/evidence/net-upstream-20261029/f-stack-build-probe.log`。这是配置探测，尚未完成
BSD 编译、链接或数据面测试。`NET_STACK=bsd` 故意拒绝构建，避免静默回退。

## 需要替换的真实边界

| 上游入口 | 现有依赖 | A20OS 的接入要求 |
|---|---|---|
| `lib/ff_host_interface.c` | libc mmap、分配、pthread、时间与随机数、rte_malloc | 内核分配及预算；阻塞上下文和错误不能照搬宿主行为 |
| `lib/ff_thread.c`、`ff_lock.c` | 线程、TLS/pcurthread、同步 | 调度器线程上下文、锁顺序与每 CPU/任务状态；不能用全局假线程代替 |
| `ff_kern_timeout.c`、`ff_subr_epoch.c` | callout、epoch 及延迟释放 | 关闭与退出时停止回调，等待引用归还后回收 |
| `lib/ff_veth.c`、`ff_veth.h` | ff_dpdk_if_context、mbuf/DPDK 包释放和卸载 | 使用现有 virtio/TUN 设备；RX 只转移一次，TX 完成前不得释放 |
| `ff_syscall_wrapper.c`、`ff_api.h` | BSD socket/errno、线程上下文、sockaddr | 接入现有 `net/transport.h`；Linux IPv6 family=10，BSD=28，不能强转 |
| `ff_route.c` 与 BSD 路由表 | BSD 自身 FIB/接口索引 | 公共路由事务的执行副本；变更失败必须回滚，不能静默分叉 |

上游 `FF_KERNEL_COEXIST` 路径不能用作本项目的正确性替代，否则可能走宿主 Linux
socket。DPDK 不进入 A20OS 正常驱动路径；直接启用 DPDK 会破坏“相同驱动与链路”的
比较条件。也不能只实现分配器几个符号后把可链接当成可运行。

先贯通单线程 BSD 初始化、内存/同步/时钟，再适配 mbuf 与现有设备，随后接入 socket
生命周期、等待/错误和控制事务。每一步保留失败样本；最终运行与 lwip/native 相同的
Linux ELF，逐项比较转发/NAT、服务、吞吐、p99、CPU/字节与连接/内存。尚无 BSD 数值。
