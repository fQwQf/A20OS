# 网络后端对照方法

`tools/bench/bench-net-kvm.py` 使用同一个静态 Linux ELF 校验 TCP/UDP 请求与回显，记录吞吐、延迟分位数、QEMU 参数、镜像/ELF 哈希和完整失败样本。`--a20-setup <脚本>` 在测试前配置 A20OS，脚本副本与哈希随证据保存。`--pcap` 保存 QEMU Ethernet 抓包。有任一用例退出失败、缺少结果或未通过 payload 验证时，脚本最终非零退出。

CPU 成本以验证成功的请求加回显 payload 字节数为分母。宿主客户端使用进程及等待子进程的 user+system CPU；KVM 使用整个 QEMU 进程的 CPU，包括所有 vCPU/I/O 线程与 VMM，因此不是单独的 guest CPU。Linux `/proc/<pid>/stat` 的时钟粒度也记录在结果中。外部 echo 服务的父进程 CPU 单独保存，TCP fork worker 未计入，不应解释为完整 peer 成本。目前指标为 ns/字节，不是硬件 cycles/字节，且没有静默扣除 idle 成本。

性能比较要求后端使用相同 vCPU、affinity、virtio 特性、MTU、链路和外部 peer。 runner 现在显式关闭双方 virtio 的 checksum、TSO/GSO、UFO、ECN 卸载特性，避免 Linux 默认协商了 A20OS 尚未启用的能力。NIC 完整参数保存在 manifest；宿主回环仍只是参考。 SLIRP 与宿主回环的路径不同，只能作功能和回归比较，不能由此推断线速或硬件 NIC 性能。最终默认后端决策需要同一桥接/TAP 拓扑的 guest Linux 基线、连接规模与峰值/回收后内存；这些尚未由当前 microbenchmark 完整实现。FreeBSD 尚不可运行，不能排名。

抓包校验器独立重算 IPv4/TCP/UDP 校验和，明确统计跳过的分片和未启用校验的 UDP。它适用于完整的软件校验抓包；启用 TX checksum offload 时，抓取点可能在校验完成之前，不能直接将这些报文认定为线上损坏。A20OS 本次验证使用关闭 TX/RX checksum offload 的相同 virtio 配置。

## 本次修复后的初步 Linux 对照

证据目录 `build/evidence/net-refactor-20261028/matched-linux-comparison/`。同一冻结 ELF、2 vCPU/2 GiB、CPU4,6、相同 virtio/SLIRP，三轮交替顺序，每case至少 2秒；18个case全部通过payload校验。下表为每轮指标的中位数，p99也是各轮p99的中位数，并非合并延迟样本后的p99。A20OS使用4 lane，无NAT规则，默认后端未改变。

| 系统 | echo payload | 吞吐 MiB/s | p99 μs | CPU ns/双向payload字节 |
|---|---|---:|---:|---:|
| 宿主 Linux 回环参考 | TCP 16 KiB | 412.974 | 54.616 | 0.65 |
| Linux KVM | TCP 16 KiB | 35.968 | 1094.583 | 20.02 |
| A20OS KVM | TCP 16 KiB | 14.484 | 5229.824 | 47.71 |
| 宿主 Linux 回环参考 | UDP 64 B | 2.791 | 30.870 | 68.97 |
| Linux KVM | UDP 64 B | 0.329 | 716.138 | 2323.12 |
| A20OS KVM | UDP 64 B | 0.291 | 303.116 | 2560.00 |

TCP路径仍有明显CPU/吞吐和尾延迟差距。这些结果不足以将成本归因于lwIP本体：收发复制、socket桥、调度/进展、锁、驱动和VMM都在测量范围内。短UDP的尾延迟比较也不能推广到其他负载。该矩阵是后续定位参照，尚缺连接规模、内存回收成本和可运行的BSD 后端，不能用它提前决定桌面/服务器默认栈。初步连接规模观测见下文。

## 连接规模与内存观测

`tools/bench/net-microbench.c`（及 `tools/probes/net-connection-probe.c`）编译成同一 Linux ELF，由`tools/bench/bench-net-scale-kvm.py` 在 Linux/A20OS KVM 上运行。每个规模重新启动2 vCPU/2GiB guest，相同 affinity、virtio 特性、SLIRP 与宿主事件循环 echo peer；没有每连接 fork。所有连接保持打开，每条在最终连接数下再次校验唯一 token。分别保存 baseline、held、 closed、closed_2s 的 guest内存和网络状态。`--a20-setup` 支持有副本/哈希的配置消融。未能保持指定规模时记录已建立数与 errno，整体非零退出，不把部分成功算成通过。

第一轮证据 `build/evidence/net-refactor-20261028/connection-scale2-kvm/`：

| 请求规模 | Linux KVM 校验连接数 | A20OS KVM 校验连接数 | Linux MemFree 减少 KiB | A20OS MemFree 减少 KiB |
|---:|---:|---:|---:|---:|
| 64 | 64 | 64 | 296 | 2068 |
| 256 | 256 | 256 | 1192 | 8608 |
| 320 | 320 | 256，达到目标失败 | 1632 | 8640 |

这是单次全 guest 内存观测，含启动后台活动与缓存，不能直接除以连接数当作协议栈精确分配成本。Linux 个别样本关闭后 MemFree 超过 baseline，说明噪声不可忽略。 runner 后续默认增加3秒启动等待；上述首轮没有该等待，不将新方法倒写成旧结果。 2秒关闭观测包含 TIME_WAIT 和 slab 缓存，不能据此宣布泄漏或完全回收。

A20OS 默认配置在256处达到 conntrack 容量。重复样本`connection-scale-ct-evidence-kvm` 显示 `ct_capacity=256`、`ct_tracked=256`，第257个连接超时；无活跃条目驱逐。同一 ELF/内核/链路，仅 `ctoff`、不配置NAT的消融`connection-scale-no-ct2-kvm` 达到并校验320条，说明这个限制来自公共状态表配置，不能归因于 lwIP 的 TCP 最大能力。关闭 tracking 不可用作 NAT 软路由的解决方案。

持有256条时 lwIP heap约96KiB，但前端 `net_socket_t` 本身为30624字节，内含16个 1600字节的 BH inline payload 槽。32KiB级前端对象是需要继续改造的内存成本；切换原版 lwIP 不会自动消除它。未来应在明确有界所有权及释放上下文后拆出按需包存储，并复测复制成本与低内存背压，而不是为通过规模测试静默放大资源表。

规模失败还暴露出旧统计把 conntrack/NAT 丢包计为 accepted。统计逻辑已改为最终 verdict 确定后只计一次，并由生产 hook 的进出方向失败用例验证。上述冻结内核在修正前，其 accepted/drop 行不可用于推导最终报文损失；原始失败证据保留。

## 原版接入性能对照

证据根为 `build/evidence/net-upstream-20261029/`。`matched-upstream3` 与`matched-legacy` 使用同一 SHA256 `9a2323f3f9e483a03b94618ad30949c128e7532f145e98acbc5b59af3480aedd`的 Linux ELF、virtio NIC（卸载关闭）、SLIRP、2 vCPU/2 GiB 与 affinity `0,2`。每项3次、每次2秒，所有样本 verified=1。下表分别取各指标的3次中位数；CPU 为整个 QEMU（guest+VMM，全线程）ns/双向已验证 payload 字节，不是纯内核 CPU 周期。宿主回环只有参考意义，不能与 SLIRP 当作相同链路排名。

| 系统/冻结版本 | TCP16KiB MiB/s | TCP p99 µs | TCP ns/B | UDP64B MiB/s | UDP p99 µs | UDP ns/B |
|---|---:|---:|---:|---:|---:|---:|
| Linux KVM | 22.367 | 1405.305 | 34.631 | 0.310909 | 716.997 | 2593.433 |
| A20OS 原版适配 | 17.850 | 3431.222 | 33.923 | 0.287948 | 314.059 | 2570.554 |
| A20OS 衍生实现（旧冻结内核） | 12.153 | 6141.358 | 52.853 | 0.274434 | 339.494 | 2683.937 |
| 宿主 Linux 回环参考 | 358.071 | 58.178 | 0.720 | 2.720980 | 32.057 | 69.572 |

这两组后端样本分组先后运行，没有交错随机顺序；SLIRP、宿主调度和短时请求/应答测试都会影响结果，不能用此表证明原版协议核心普遍更快或已解决吞吐瓶颈。冻结内核早于最后的 UNIX 选项空 attachment 防护、零 TTL 输入 guard 和选项重命名；准确源码/ELF 身份以各 manifest 为准。当前重命名后的双网卡正确性另有独立回归，不能冒称表中数据是最终构建性能。无 BSD 样本，不填估计数字。用户已决定去除独立旧分叉后端，该决定不以这张性能表为条件。

`lwip-three-backends-services-kvm` 和 `native-three-backends-services-kvm` 使用最后构建的 `kernel-lwip.elf` / `kernel-native.elf`，都通过真实三节点双网卡、IPv4/IPv6、 NAPT 源端口冲突、策略路由正负例、socket ABI 与 DHCP/DNS。原版 lane=1、native lane=4，两者 NR_CPUS=2；这是后端各自执行模型的正确性回归，不能冒称相同执行并发度的性能实验。

## 当前连接与内存瓶颈

`lwip-three-scale2-kvm` 使用同一静态 Linux connection-probe ELF，每个连接独立 token，连接群建立前后均核验数据，并记录 baseline/held/closed/closed_2s 内存。Linux KVM 的 64/256/320 均通过；A20OS 原版默认档64/256通过，320仅建立256条并返回失败。失败样本保留。默认 conntrack 上限256仍是硬限制；不能关闭跟踪来声称软路由容量已解决。

原版当前 frontend=1192 B、private=29304 B、combined=30496 B（LP64/default）， INET attachment 仍有固定 BH payload ring 的成本。256连接实测持有时 lwIP heap 88736 B，关闭后仍88736 B（TIME_WAIT）；frontend引用从live256回到0，allocs=frees=512， faults=0。整机 MemFree 的持有前后差约9600 KiB，关闭后恢复大部分；它包含分配器、页与系统活动，不能当作精确每连接成本或仅凭2秒窗口判定泄漏。Linux 同次测量还发生后台内存释放，必须结合原始 `/proc` 状态解释，不能简单比较 MemFree 差值。

应继续降低固定 socket attachment 成本、按场景给出跟踪容量与压力行为，再用直连受控链路测多连接吞吐与 CPU 归因。当前结果足以证明存在容量与内存成本问题，尚不足定位单一吞吐热点。

`native-three-scale-kvm` 在 native lane=4 的相同默认预算下也在64/256通过、320只建立256。这进一步表明该限制属于共有跟踪容量，不能归因于原版 lwIP 性能下降。

`lwip-three-server-scale-kvm` 在 `NET_PROFILE=3`、原版 lane=1、NR_CPUS=2 下，保持 conntrack 开启通过320连接（320/320，verified=1）。这证明提高资源档位可越过默认档上限，不等于已解决压力控制、动态容量规划或大规模服务器验收。

## 驱动桥解耦后的最终构建对照

`matched-lwip-driver-bridge` / `matched-native-driver-bridge` 对应已通过最新服务回归的冻结内核。仍使用同一个 Linux ELF、2 vCPU/2 GiB、virtio、卸载关闭、SLIRP、affinity `0,2`，每项3次×2秒。原版 lane=1，native lane=4；分组先后运行，保留原始次序。 Linux host/KVM 与原版共18样本，native 6样本，全部 verified=1、invalid_samples=[]。协议核心和新驱动入口一起参与测量，不能把差异单独归因于驱动接口。

| 最终构建 | TCP16KiB MiB/s | TCP p99 µs | TCP ns/B | UDP64B MiB/s | UDP p99 µs | UDP ns/B |
|---|---:|---:|---:|---:|---:|---:|
| Linux KVM | 33.709 | 1052.346 | 22.207 | 0.349942 | 635.377 | 2370.934 |
| A20OS lwip | 21.494 | 3129.021 | 28.832 | 0.299974 | 257.651 | 2431.968 |
| A20OS native | 14.440 | 5955.064 | 48.029 | 0.305182 | 259.233 | 2517.550 |
| 宿主 Linux 回环参考 | 358.091 | 56.991 | 0.704 | 2.741840 | 30.870 | 69.079 |

数值口径同前表（各指标分别取中位数、CPU含整个QEMU、payload双向计数）。当前证据没有证明“原版一定不如魔改版”；也没有证明原版普遍更快。TCP 与 Linux KVM 仍有明显差距，UDP 小包受请求/应答链路影响很大，需要进一步使用受控直连、多流、批量和真正的 CPU 采样定位瓶颈；不以这个短测决定所有场景默认。

独立追踪轮位于 `trace-lwip-driver-bridge` / `trace-native-driver-bridge`，每项1次×2秒，不混入上表。四个样本均 verified=1，且每个 snapshot 的`collection_enabled_before_snapshot=1`，不把未启用的零计数当证据。TCP 样本中原版 BH events=35284、native=21980；二者 `net_bh_overflow=0`、`net_alloc_fail=0`。测试期间未观察到暂存溢出/分配失败，但仍需在长期高压环境下持续观测。锁探针有实际争用记录，但各后端工作量与覆盖点不同，不能仅用累计等待值判定锁是主导瓶颈；计数器不是完整 CPU 调用栈采样。原始 per-site 记录留在各 manifest 的 profile 字段。


## TUN 解耦与 ICMP 改写性能对照

冻结内核对应 `70f7534a`，证据为`build/evidence/net-router-completion-20261029/matched-lwip-completion/` 与`matched-native-completion/`。24个样本均 rc=0、verified=1，invalid_samples 为空。同一 Linux ELF SHA256 为`9a2323f3f9e483a03b94618ad30949c128e7532f145e98acbc5b59af3480aedd`， 2 vCPU/2 GiB，affinity0,2，virtio 卸载关闭、相同 SLIRP 与外部 peer，未启用追踪。每项3轮×2秒，各指标单独取中位数，p99没有合并样本。CPU 仍为整个 QEMU ns/双向已验证 payload 字节；宿主回环只作参考。

| 系统 | TCP16KiB MiB/s | TCP p99 µs | TCP ns/B | UDP64B MiB/s | UDP p99 µs | UDP ns/B |
|---|---:|---:|---:|---:|---:|---:|
| Linux KVM | 34.319 | 1063.535 | 20.702 | 0.331969 | 672.318 | 2340.102 |
| A20OS lwip | 23.578 | 3459.985 | 27.467 | 0.296779 | 308.901 | 2486.737 |
| A20OS native | 12.468 | 6914.284 | 53.499 | 0.301767 | 255.449 | 2613.848 |
| 宿主 Linux 回环参考 | 364.457 | 55.811 | 0.704 | 2.798728 | 30.873 | 67.899 |

原版单执行域、native4 lane，保留其当前执行配置；native 在原版组之后运行，未交错随机化。三节点正确性回归在其他 CPU 上有短时重叠，不能排除宿主共享资源干扰。这张表只说明当前完整系统在该短时负载下仍有 TCP 成本/吞吐及尾延迟差距，不能将差距全部归因于协议栈核心，也不能凭此决定桌面/服务器默认后端。BSD 端口处于非运行状态；当前未重测极限连接规模与内存占用曲线，默认 256 条 CT 容量等边界约束依然生效。

双网卡/TUN/ICMP 验收范围、复现命令及历史失败记录见[router.md](router.md)。
