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
后端，不能用它提前决定桌面/服务器默认栈。初步连接规模观测见下文。

## 连接规模与内存观测

`tools/net-connection-probe.c` 编译成同一 Linux ELF，由
`tools/bench-net-scale-kvm.py` 在 Linux/A20OS KVM 上运行。每个规模重新启动2 vCPU/2GiB
guest，相同 affinity、virtio 特性、SLIRP 与宿主事件循环 echo peer；没有每连接 fork。
所有连接保持打开，每条在最终连接数下再次校验唯一 token。分别保存 baseline、held、
closed、closed_2s 的 guest 内存和网络状态。`--a20-setup` 支持有副本/哈希的配置消融。
未能保持指定规模时记录已建立数与 errno，整体非零退出，不把部分成功算成通过。

第一轮证据 `build/evidence/net-refactor-20261028/connection-scale2-kvm/`：

| 请求规模 | Linux KVM 校验连接数 | A20OS KVM 校验连接数 | Linux MemFree 减少 KiB | A20OS MemFree 减少 KiB |
|---:|---:|---:|---:|---:|
| 64 | 64 | 64 | 296 | 2068 |
| 256 | 256 | 256 | 1192 | 8608 |
| 320 | 320 | 256，达到目标失败 | 1632 | 8640 |

这是单次全 guest 内存观测，含启动后台活动与缓存，不能直接除以连接数当作协议栈
精确分配成本。Linux 个别样本关闭后 MemFree 超过 baseline，说明噪声不可忽略。
runner 后续默认增加3秒启动等待；上述首轮没有该等待，不将新方法倒写成旧结果。
2秒关闭观测包含 TIME_WAIT 和 slab 缓存，不能据此宣布泄漏或完全回收。

A20OS 默认配置在256处达到 conntrack 容量。重复样本
`connection-scale-ct-evidence-kvm` 显示 `ct_capacity=256`、`ct_tracked=256`，第257个连接
超时；无活跃条目驱逐。同一 ELF/内核/链路，仅 `ctoff`、不配置NAT的消融
`connection-scale-no-ct2-kvm` 达到并校验320条，说明这个限制来自公共状态表配置，
不能归因于 lwIP 的 TCP 最大能力。关闭 tracking 不可用作 NAT 软路由的解决方案。

持有256条时 lwIP heap约96KiB，但前端 `net_socket_t` 本身为30624字节，内含16个
1600字节的 BH inline payload 槽。32KiB级前端对象是需要继续改造的内存成本；切换
原版 lwIP 不会自动消除它。未来应在明确有界所有权及释放上下文后拆出按需包存储，
并复测复制成本与低内存背压，而不是为通过规模测试静默放大资源表。

规模失败还暴露出旧统计把 conntrack/NAT 丢包计为 accepted。本轮已改为最终 verdict
确定后只计一次，并由生产 hook 的进出方向失败用例验证。上述冻结内核在修正前，
其 accepted/drop 行不可用于推导最终报文损失；原始失败证据保留。
