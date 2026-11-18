# 真实双内核 UDP 集群验收

该验收使用两个独立 RISC-V QEMU 内核，执行实际 Native channel 代理与 UDP 适配器。它不只是互 ping，也不把同内核 loopback 虚拟节点当作两台机器。

## 1. 一键运行

```bash
make check-cluster-udp
python3 tools/cluster/check-cluster-udp.py --faults --restart
```

脚本自动构建 `ARCH=riscv64 BRINGUP=1 RAMFS_USER=1 DRIVER_DEPLOYMENT=embedded`，从 Makefile 取得 QEMU 参数，安装 virtio-net 并配置 guest 地址 `10.0.3.2/24`和 `10.0.3.3/24`。嵌入驱动配置必不可少：只有泛用内核而没有 NIC 模块时不能把未绑定网卡造成的失败归咎于集群握手。

宿主 L2 UDP 端口自动分配，guest CL 端口为 44020。默认日志目录为`.kernel-build/cluster/udp`，可用 `--log-dir` 改写。`--skip-build` 使用当前产物。脚本只清理自己创建的 QEMU 进程和 socket，不查杀其他实例。

## 2. 验收内容

- 双向 8 次非可靠 echo。
- 双向各 96 次可靠 echo，服务端统计每个请求的实际执行次数，检查重复执行为 0。
- 共用 MCU/主机 dot 算子，包含负数输入，结果为 -24。
- 未知服务（普通/可靠）、零长度调用及超 MTU 调用的可终止失败行为。
- 故障模式丢首个请求、回复、接纳 ACK、完成收据，并延迟/重复真实线上帧。
- 全丢包调用恰好发送初始请求加 8 次重试，随后取消，不能永久等待。
- 重启 B 后，A 的旧代理关闭，重新连接成功；注入旧请求和旧 HELLO_ACK 后，新服务执行计数保持正确。

故障注入器只转发或修改投递时机，不生成业务回复。所有业务回复来自运行生产代码的 guest 服务。成功行必须同时出现在两端日志，缺失或 FAIL 都令脚本非零退出。故障动作另外写入 `udp-faults.json`。

## 3. 证据解释

这是 TCG/QEMU 的独立内核功能与故障恢复证据，不是物理 MCU、真实交换机、无线串口或大规模服务器性能证据。旧 `tools/cluster/cluster-net-up.sh` 仍可用于 L2 连通排查，但它的互 ping 结果不能替代本验收。

用户态服务架构与 Native 运行时集成参见 [05-userspace.md](05-userspace.md)。
