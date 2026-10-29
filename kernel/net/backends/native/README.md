# Native 协议后端

`NET_STACK=native` 是 A20OS 维护的 lwIP 衍生协议实现，**不是从零自研的 TCP/IP 栈**。
旧 `kernel/external/lwip` 已整体退出外部依赖布局；不保留第四个 legacy 后端。
`protocol/COPYING`、每文件版权、`.upstream-base` 与 `DIVERGENCE.md` 保留来源。

当前提取包括 PCB lane 索引、共享状态并发保护、CUBIC、窗口限额、内存池上下文与
协议修正。这些只在 native 参与编译，不进入原版源码。现阶段与原版复用
`../lwip/` 内私有 C ABI adapter；`CONFIG_LWIP_UPSTREAM` 由构建选择，不能在运行时
把不兼容的 PCB 交给另一个实例。后续改动必须连同锁顺序、回调上下文和回收协议验证。

```sh
make ARCH=x86_64 NR_CPUS=2 NET_STACK=native NET_LANES=4 NET_ROLE=router kernel-only
python3 tools/check_lwip_concurrency_wiring.py
```

源码唯一构建清单位于 `../lwip/sources.mk`。三后端共有的 socket、FIB、策略、NAT
位于上层目录，不能将它们复制进协议树。公共头不暴露私有类型的门禁仍适用。
保留现有 C 实现不等于完成所有临时设计的清理；完整分片 NAT、PMTU、设备桥解耦与
规模扩展仍在推进。使用和证据见 [网络架构](../../../../docs/net/architecture.md)。
