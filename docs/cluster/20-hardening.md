# 集群故障硬化与运行诊断

本文记录 2026-10-11 的软件验证。检查器启动独立 RISC-V QEMU 内核，通过
宿主 UDP 桥处理真实以太网帧；它不生成服务回复，不替换内核路由、代理、
重传或服务实现。日志、环境、源码 SHA256 和注入事件保存在
`.kernel-build/cluster/`。这些结果不能作为物理网络性能或 STM32 实机证据。

## 1. 一键复现

```sh
make check-cluster-unit check-cluster-fuzz
make check-cluster-hardening
make smoke-native-cluster smoke-native-cluster-supervisor
make check-cluster-mesh
make demo-wordcount
```

`check-cluster-hardening` 顺序运行以下三个独立场景，不在运行中的 QEMU
之间重写它们使用的内核或磁盘镜像：

| 目录 | 注入与验收 |
|---|---|
| `random-loss1000/` | 每端 1000 次可靠调用，10% 随机丢弃可靠帧，损坏一帧，随后重启 B 并注入旧会话；服务端计数必须恰好为 1000，重复执行为零 |
| `delay-crc-flood/` | 可靠帧随机延迟 50–200 ms；每端一个 CL 载荷 bit 损坏；每端 1000 个伪造身份 HELLO；96 次调用、8 次分片往返及重启恢复必须成功 |
| `partition-slow/` | 服务端 10 秒不应答、调用方 2 秒预算；随后双向 CL 网络分区 30 秒，验证有界失败与恢复后新调用 |

所有场景同时保留定点丢请求、回复、ACK、完成收据、重复请求、分片乱序与
重复、未知槽、空调用和超 MTU 拒绝。黑洞事务必须在线上恰好出现初发加
8 次重试，并最终结束；不能只依据一个超时日志声称重试有界。

随机数默认种子为 `20261011`，可通过 `--seed` 改变。概率针对进入随机
注入阶段的可靠帧，包含重传、ACK 和收据，不包含 ARP、HELLO、心跳，也不
包含已被定点故障拦截或单独送达的 CRC 损坏帧。因此概率 0.1 不表示某次
运行恰好丢失 10.000%。
`udp-faults.json` 中的 `random-eligible`、`random-drop`、`random-delay`
记录实际数量；两端并发时包序可能不同，同一个种子也不承诺完全相同的轨迹。
50–200 ms 是单帧延迟范围，不是 RPC 的端到端 RTT 范围。

## 2. 损坏帧与伪造身份

CRC 注入翻转 CL 载荷中的一个 bit，保留错误的 CL CRC，重新计算合法 UDP
校验和。接收端必须观察到 `RX_MALFORMED > 0`，重传后仍得到正确回复，
从而证明测试经过 CL 解码器，而不是仅在 lwIP 的 UDP 校验阶段被拒绝。

HELLO 注入为每端生成 1000 个不同的完整 node_id，重算源哈希、CL CRC 与
UDP 校验和，经已有邻接地址发送。内核必须拒绝地址绑定以外的身份，策略
drops 至少增加 1000，路由保持 1 条、工作服务导出保持 3 个、可靠事务来源
保持 1 个。统计桶最多为 UDP 邻接桶加 adapter 0 的失败发送桶；伪造身份
不能创建 1000 个成员或来源记录。随后调用继续成功。

该测试覆盖受控链路上伪造 node_id 的输入，不是密码学认证测试，也不代表
任意源地址洪泛、网卡线速拒绝服务或不可信公网部署已经验收。

## 3. 慢服务、取消与网络分区

慢服务在接纳请求后睡眠 10 秒。调用方以非阻塞接收执行 2 秒 attempt 预算，
预算耗尽时关闭代理，与 Native 作业驱动采用同样的资源终止方式。关闭代理
发送 retirement；服务端仍保留正在执行的缓存预留，不能提前复用并再次执行。
12 秒后检查服务执行次数恰好为 1、新请求成功，调用方在途数和重试字节数归零。

这验证的是调用方预算与执行期间的资源回收；不能声称 retirement 会中止业务，
也不能声称 10 秒服务必然触发内核 30 秒默认 deadline。PROCESS 的期限终止由
agent 杀死其子进程，另由 `smoke-native-cluster` 验证。队列满的背压拒绝另由
loopback 和可靠缓存单元测试验证；本场景不伪造一次 RESOURCE_LIMIT 回复。

分区由宿主桥真实丢弃两个方向的全部 CL 帧，包括控制帧，持续 30 秒。
分区中的调用必须在 15 秒内以代理终止结束；当前 Native channel 接收表现为
`CANCELED`，并不提供每次调用的精细远端原因。恢复后等待链路重新 UP，重新
创建代理并完成独立往返。日志必须同时出现 `partition-canceled` 和
`partition-recovered`，不能把恢复后的一次成功代替分区期间的失败验收。
公共链路状态同步适配器的 SUSPECT，测试在等待期间必须通过 kind 5 观察到
SUSPECT 桶，再验证取消和恢复；只增加一个永远为零的指标不能通过该用例。

## 4. 运行统计

Native `cluster_snapshot` 保持 syscall `0x0526` 和 192 B 信封不变：

| kind | SDK | 内容 |
|---:|---|---|
| 3 | `a20_cluster_resources` | 路由、导出、代理、在途、回复缓存、缓冲字节与预算 |
| 4 | `a20_cluster_reassembly_resources` | 重组上下文、字节、峰值、完成、拒绝与过期 |
| 5 | `a20_cluster_link_resources` | 链路状态桶、收发、策略丢弃、畸形输入与重传 |

kind 5 的统计在链路锁内聚合，`values[0..11]` 为采样 ms、桶数、UP/SUSPECT/
DOWN 桶数、TX/RX 帧数、TX/RX drops、RX malformed、重传数、桶数上限。
其余值清零，输入 index/generation/reserved 必须为零。所有者与计数范围见
[09-work-agent.md](09-work-agent.md)。CRC 拒绝属于 malformed；地址或 TTL
策略拒绝属于 drops。两者分别观察，不把驱动、channel、线程栈的内存占用
混入某个缓冲字节计数，也不把统计桶数解释为集群成员数。

## 5. CI 与尚未完成的生产验证

PR/主干 CI 的宿主门禁执行共享单元测试与 ASan/UBSan fuzz；独立内核 job
执行 loopback、UDP 故障重启、Native agent/jobd/服务监管及三节点 mesh。
手动 `workflow_dispatch` 额外运行本硬化矩阵与八节点的两种词频配置。
七架构内核构建继续使用 `print-ci-kernel-arches` 的既有矩阵，不另抄架构清单。
本地 `make check` 与 CI 宿主门禁同步包含集群单元和 fuzz；CI 配置已写入，
不能据此声称 GitHub 上已经运行成功。

已取得的连续负载为 COM2 的 10000 次可靠调用，并有任务/僵尸/资源回收与
p50/p95 记录。它不等于 [06-production.md](06-production.md) 的四节点
8 小时、每节点 100 msg/s、10% 丢帧与混合 64 KiB 负载 soak。完整生产吞吐、
本机 IPC 对照、物理 STM32 掉电恢复和物理混合集群仍须单独执行。

全库快速门禁最初有两个独立失败：宿主 lwIP 并发测试缺少 CPU 适配头与
CPU 数配置，以及既有 `docs/roadmap/single-level-mm-model.md` 引用越界行号。
已修复构建接线并更新到实际函数，保留四 lane、真实分配器和 8 线程各
20000 次操作；最终 `make check` 的 21 项宿主门禁全部通过。证据位于
`.kernel-build/cluster/final-host-gates.log`。全库格式门禁仍有 114 个既有
非豁免文件不合规，未通过扩大格式豁免或跳过失败用例掩盖。
`make check-doc-test-gates` 的完整聚合门禁最终也已通过，含实际 SMP、进程
生命周期、阻塞/退出、MM、VFS 与驱动运行检查。
