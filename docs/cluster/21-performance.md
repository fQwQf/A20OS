# Native 集群性能测量与回归

本文对应 [06-production.md](06-production.md) 的性能验收。功能正确、性能达标、
长期稳定性分别记录；任何一项通过都不能替代另外两项。

## 可复现入口

```sh
make check-cluster-performance
make check-cluster-fuzz-smoke
```

前者构建 `CLUSTER_PROFILE=3` 的 RISC-V64 内核与 Native 程序，依次启动本机、
loopback、双独立内核 UDP 场景。每个场景用独立 QEMU snapshot；检查器只回收
自己启动的进程。日志与 JSON 写入 `.kernel-build/cluster/performance/`。
运行期间不能重打包它所使用的磁盘镜像。

`native-cluster-bench` 可单独跨架构构建。它是主机 Native 基准程序，使用约
1 MiB 静态采样/业务缓冲，不适合直接移植到 20 KiB SRAM 的 MCU。
MCU 的统一机制由共用 wire/CW 格式、叶子执行器与资源档提供，串口性能入口
见 [13-uart-head.md](13-uart-head.md)。

也可在已经构建好服务器档镜像后缩小场景：

```sh
python3 tools/check-cluster-performance.py --skip-build --mode udp --udp-calls 1000
python3 tools/check-cluster-performance.py --skip-build --mode local --calls 100000
```

## 测量口径

- 本机、loopback 各 100000 次 64B 调用；UDP 为 10000 次 64B 和 1000 次
  65536B 调用。每阶段先做 100 次预热。
- 客户端实际调用 Native 融合 syscall `a20_channel_call`。服务是另一个真实
  Native 线程；UDP 服务运行在另一套内核中，没有主机 echo 代答。
  loopback 使用默认 v0 代理，UDP 使用协商的 v1 可靠连接；可靠等级分别标明。
- 每次请求携带序号，回复逐字节核对。阶段末读取服务执行计数，要求恰好等于
  已执行的预热与测量调用总数。统计请求本身不增加该计数。
- RTT 为两次 MONOTONIC 读数之差，包含融合调用及其阻塞等待；逐字节核对位于
  结束读数之后。`elapsed_ns` 还包含循环与内容核对，用于应用有效吞吐计算。
- p50/p95/p99 使用排序后 `floor((n-1)*p/100)` 索引。100 个连续等长块的总和
  与全部样本和必须一致；块均值标准误只描述本次运行，不能当成独立启动实验。
- 双向有效负载 MB/s = `2 * bytes * calls / elapsed_seconds / 1000000`，
  不含帧头、收据、重传，也不是网卡线速或四节点聚合吞吐。
- JSON 保留原始块均值、调用数、实际执行数、内核 ELF 摘要、源码摘要、QEMU
  参数/版本、时间和工作区状态。失败时仍保存已完成阶段，不写成功结果。

## 本机改造前后比较

使用独立的只读基线 checkout 构建旧内核，不使用原工作目录的脏构建产物。
两版内核运行同一个镜像中的同一个 Native 基准程序。本机模式不初始化集群。
参考内核通过 `--kernel /absolute/path/kernel.elf` 指定，仅允许与
`--mode local --skip-build` 同用。

每版至少两次、建议五次独立启动，每次 100000 次调用，并交替先后顺序：

```sh
python3 tools/compare-cluster-performance.py \
  --baseline /absolute/baseline-0/evidence.json /absolute/baseline-1/evidence.json \
  --candidate /absolute/candidate-0/evidence.json /absolute/candidate-1/evidence.json \
  --output .kernel-build/cluster/local-comparison.json
```

比较器以每次独立启动的均值为单位，报告均值变化、双侧精确置换检验 p 值与
固定种子的 bootstrap 95% 区间。它拒绝失败记录、重复文件、不同版本基准、
同组不同内核，以及样本不足。回归超过 20% 返回失败；观察均值差小于 3%
只是一项结果，不能据此声称置信区间已证明等价。TCG 代码布局与主机调度仍会
影响结果；严谨的硬件性能结论需要独立物理机测试。

## 五分钟模糊档

`check-cluster-fuzz-smoke` 在 ASan/UBSan 下持续至少 300 秒运行生产 frame
解码器和 MCU leaf 分发器，每批 100 万用例并更换确定性种子；完整批次结束
才停止，因此可能略超时。JSON 记录每批种子、实际变异数和变异阶段时长。
额外运行分片、端到端会话、配置、作业和可靠收据回归。原有
`check-cluster-fuzz` 仍是百万用例快速档。这个入口是有界语料变异，未声称使用
覆盖率反馈的 libFuzzer，也不提供目标设备性能证据。

手动触发 CI 的扩展运行接入这两个入口；本地运行通过不等于远端 CI 已运行。

## 本轮已取得结果（2026-10-11）

基线为 `05f62d16` 原有配置，候选使用默认主机档内核；业务基准源文件完全
相同。本机 channel 与融合 IPC syscall 源文件相对基线没有修改。每版五次
独立启动、每次 100000 次调用，共 100 万次测量调用，交替启动顺序。

| 项目 | 实测 |
|---|---|
| 基线五次启动均值 | 75.400 µs |
| 候选五次启动均值 | 71.724 µs |
| 均值变化 | −4.875% |
| 双侧精确置换检验 | p=0.00794，252 种分组 |
| bootstrap 95% 区间 | [−7.114%, −2.914%] |
| 五分钟模糊档 | 300.960 秒、217 批、2.17 亿次变异，ASan/UBSan 通过 |

候选在这组 TCG 运行中更快，没有超过 20% 的退化，但观察差值不在 ±3%
区间内，不能写成严格等价验收通过，更不能把它归因于未修改的本机 IPC
实现得到优化。主机同时存在其他测试负载，内核布局也不同；需要物理机和
受控环境复测。原始结果见 `.kernel-build/cluster/performance-local-release/`，
模糊结果见 `.kernel-build/cluster/fuzz-smoke/`。早期单组测量不替代这组最终结果。

## 长流量发现的 virtio-net 描述符回收错误

最初连续大消息测量在第 717/776 次测量调用附近重试 8 次后失败；接收侧
malformed/策略丢弃为零，但发送失败不断增长。短的 8 次分片测试未暴露它。
实际驱动把 `used.ring[].id` 当成 available ring 下标，再从旧映射取槽位，
导致 TX 回收错误缓冲；RX 首片和续片也做了同样的错误转换。

[VIRTIO 1.2 §2.7.8](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html)
规定该值标识描述符链头。现已直接按完整 32 位 ID 做边界检查与回收，移除
两个多余映射数组，每设备少用 1024 B。核心和 drvmod 共用同一个实现。

`make check-virtio-net-completion` 从生产文件提取实际 TX 完成函数，测试乱序、
错误 ID、available 位置与描述符编号不同，以及 30 万次计数回绕。旧函数
在首次不同编号断言失败，修复后 ASan/UBSan 通过。连续 UDP 调用还必须验证
真实设备路径；函数单测不代替运行结果。

修复后的默认规模入口已完整通过，日志为
`.kernel-build/cluster/performance-release/`。其中 UDP 服务端实际执行 11200
次业务请求，恰好包含两阶段预热；两端 TX/RX drops 与 malformed 均为零，
客户端发生 1 次重传且未重复执行。结束时两端重组 context/bytes 均为零。

驱动修复后，另行顺序运行默认档直连 UDP（故障与重启）、服务器档三节点
路由、服务器/默认档混合三节点路由，以及 UDP→x86 COM2→静态 leaf 网关
回归，全部通过。对应日志前缀为 `performance-fixed-udp-` 与
`performance-fixed-gateway.log`。完整 `make check-doc-test-gates` 在性能
测量结束后独立重跑通过；早期并发运行曾触发接收超时测试的调度时长阈值，
保留失败日志，没有放宽测试阈值。

| 路径 | 测量次数 | RTT 均值 | p50 | p99 | 双向有效负载 MB/s |
|---|---:|---:|---:|---:|---:|
| 本机，64B | 100000 | 70.712 µs | 67.600 µs | 168.600 µs | 1.687 |
| loopback v0，64B | 100000 | 2141.549 µs | 1883.100 µs | 4035.600 µs | 0.059 |
| UDP v1，64B | 10000 | 7289.380 µs | 7628.400 µs | 12118.800 µs | 0.018 |
| UDP v1，65536B | 1000 | 83946.012 µs | 82826.900 µs | 98664.000 µs | 1.545 |

loopback 同次运行均值比为 30.286，超过目标 3；UDP 64B 的 p50 为 7.628 ms，
超过目标 5 ms。RX 与 adapter poll 各自有 1 ms 周期等待，代理/导出工作线程
也增加调度次数。64 KiB 发送每 4 片主动让出 1 ms，单请求往返的固定节流、
47 片双向编解码与多缓冲 virtio RX 都会影响结果。这些是代码可确认的瓶颈，
尚未以消融确定各自占比。下一步需要事件唤醒与受控的有界流水线，再按相同
基准对比，不能只改验收数值。顺序单连接数据不是四节点批量聚合测试，
100 MB/s 聚合目标仍未验收。

## 尚不能替代的验收

普通远端业务仍通过 CALL 事务映射到 channel；控制面的 SEND 不等于业务单向
SEND。当前基准不覆盖四节点 60 秒 SEND 吞吐，也不满足四节点、混合 SEND/CALL、
10% 丢帧、8 小时 soak。资源漂移、首尾 p99 漂移和物理 MCU 性能必须另行实测。
这些差距继续保留在生产验收清单中。
