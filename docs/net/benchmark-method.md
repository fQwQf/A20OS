# 网络后端对照方法

`tools/bench-net-kvm.py` 使用同一个静态 Linux ELF 校验 TCP/UDP 请求与回显，记录吞吐、
延迟分位数、QEMU 参数、镜像/ELF 哈希和完整失败样本。`--a20-setup <脚本>` 在测试前
配置 A20OS，脚本副本与哈希随证据保存。`--pcap` 保存 QEMU Ethernet 抓包。
有任一用例退出失败、缺少结果或未通过 payload 验证时，脚本最终非零退出。

CPU 成本以验证成功的请求加回显 payload 字节数为分母。宿主客户端使用进程及等待
子进程的 user+system CPU；KVM 使用整个 QEMU 进程的 CPU，包括所有 vCPU/I/O 线程
与 VMM，因此不是单独的 guest CPU。Linux `/proc/<pid>/stat` 的时钟粒度也记录在结果中。
外部 echo 服务的父进程 CPU 单独保存，TCP fork worker 未计入，不应解释为完整 peer 成本。
目前指标为 ns/字节，不是硬件 cycles/字节，且没有静默扣除 idle 成本。

性能比较要求后端使用相同 vCPU、affinity、virtio 特性、MTU、链路和外部 peer。
runner 现在显式关闭双方 virtio 的 checksum、TSO/GSO、UFO、ECN 卸载特性，避免 Linux
默认协商了 A20OS 尚未启用的能力。NIC 完整参数保存在 manifest；宿主回环仍只是参考。
SLIRP 与宿主回环的路径不同，只能作功能和回归比较，不能由此推断线速或硬件 NIC
性能。最终默认后端决策需要同一桥接/TAP 拓扑的 guest Linux 基线、连接规模与峰值/
回收后内存；这些尚未由当前 microbenchmark 完整实现。FreeBSD 尚不可运行，不能排名。

抓包校验器独立重算 IPv4/TCP/UDP 校验和，明确统计跳过的分片和未启用校验的 UDP。
它适用于完整的软件校验抓包；启用 TX checksum offload 时，抓取点可能在校验完成
之前，不能直接将这些报文认定为线上损坏。A20OS 本次验证使用关闭 TX/RX checksum
offload 的相同 virtio 配置。

## 本次修复后的初步 Linux 对照

证据目录 `build/evidence/net-refactor-20261028/matched-linux-comparison/`。
同一冻结 ELF、2 vCPU/2 GiB、CPU4,6、相同 virtio/SLIRP，三轮交替顺序，每case至少
2秒；18个case全部通过payload校验。下表为每轮指标的中位数，p99也是各轮p99的
中位数，并非合并延迟样本后的p99。A20OS使用4 lane，无NAT规则，默认后端未改变。

| 系统 | echo payload | 吞吐 MiB/s | p99 μs | CPU ns/双向payload字节 |
|---|---|---:|---:|---:|
| 宿主 Linux 回环参考 | TCP 16 KiB | 412.974 | 54.616 | 0.65 |
| Linux KVM | TCP 16 KiB | 35.968 | 1094.583 | 20.02 |
| A20OS KVM | TCP 16 KiB | 14.484 | 5229.824 | 47.71 |
| 宿主 Linux 回环参考 | UDP 64 B | 2.791 | 30.870 | 68.97 |
| Linux KVM | UDP 64 B | 0.329 | 716.138 | 2323.12 |
| A20OS KVM | UDP 64 B | 0.291 | 303.116 | 2560.00 |

TCP路径仍有明显CPU/吞吐和尾延迟差距。这些结果不足以将成本归因于lwIP本体：收发
复制、socket桥、调度/进展、锁、驱动和VMM都在测量范围内。短UDP的尾延迟比较也不能
推广到其他负载。该矩阵是后续定位参照，尚缺连接规模、内存回收成本和可运行的BSD
后端，不能用它提前决定桌面/服务器默认栈。
