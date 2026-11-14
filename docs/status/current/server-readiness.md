# A20OS 服务器就绪度评估

> 待完成事项与演进需求统一汇总于 [1.0 缺口台账](../gaps/README.md)；本文档提供当前系统的服务器能力与边界评估。

## 一句话结论

A20OS 目前定位于**针对 QEMU 平台的桌面与混合内核架构原型**，尚未达到生产级服务器内核的成熟度要求。其关键差距体现在四个核心结构性维度：

1. **入站 TCP 与协议服务**：入站 TCP 监听机制已接入真实 LISTEN PCB，但底层仍受协议栈全局并发限制（详见第二节）。
2. **网络并发与数据面锁**：虽然 socket 层已拆分为桶锁和 per-socket 锁，但网络核心数据面仍受全局锁制约。
3. **命名空间与容器前置隔离**：PID namespace、user namespace 与 `pivot_root` 机制已具备初步支持，但资源配额与深度沙箱仍在完善（详见第三节）。
4. **实体硬件与 PCIe 适应性**：驱动实现存在较多针对 QEMU 虚拟化环境的假设，尚未建立完备的实体服务器 PCIe 与多队列网卡支持。

在上述结构性瓶颈收敛之前，不建议将生产数据库或不受信任的外部工作负载直接部署于此环境。

### CI 门禁基线与已知异常项

在基线构建与自动化回归中，系统已通过 ABI=both riscv64 构建与 `check-abi-boundary` / `check-native-abi-coverage` / `check-abi-config-guard` / `check-smoke-cases`。目前 CI 中存在三条已知异常门禁：

| 门禁 | 状态 | 归因 |
|---|---|---|
| `smoke-native-contract` | 停在 `vmol-leak-vmo` | **主干既有**。归因为主干既有的 VMO 引用计数缺陷，门禁接入后准确暴露了该问题 |
| `check-doc-drift` | 外部引用解析异常 | **主干既有**。历史文档检查脚本对外部内核头文件引用的解析规则漂移 |
| `smoke-native-handle` | 跑完首个用例后在关机前中止 | **主干既有**。在启用 `CONFIG_KERNEL_PREEMPT` 的构建中，用例执行顺序较预期提前中止 |

对应缺陷的解决路线参见 [roadmap/a20os-improvement-todo.md](../../roadmap/a20os-improvement-todo.md)。需要特别说明的是：`smoke-native-handle` 与 `smoke-native-mm` 未闭环前，部分用户态断言（如 `handle_set_meta` 的时间戳截断、`xattr` 权限分离、`vm_advise` 策略）尚未完成完整的全量端到端动态验收。

入站 TCP 监听机制已完成关键修复：`net_listen()` 支持 `tcpmode` 双档，`lwip` 档将绑定 PCB 转换为协议栈原生 LISTEN PCB，消除了入站 SYN 默认被 RST 的问题。在启用 `a20.tcpmode=lwip` 时，SLIRP hostfwd 能够正常接入 guest telnetd 并提供 shell 服务（详见第二节）。

网络数据面锁优化方面：`g_net_lock` 已被移除，socket 表采用分片桶锁管理，读写路径进一步下沉为 per-socket 锁（`net_socket_t.lock`）。当前协议栈核心数据面的主要并发约束为 `g_lwip_lock` 全局锁本身（阶段 C/D/E 已逐步收窄 socket 侧与分发侧）。所有性能数据采集自 QEMU TCG 环境，分析以确定性临界区行为与代码契约为主。

在下面的阻塞项收敛之前，把数据库或不受信任的工作负载放上去是不安全的。

## 一、存储持久性

### 已达成

ext4 现在是可写日志文件系统，运行时 metadata 更新带 JBD2 ordered 语义：每次写入经 `ext4_journal_meta_write` 登记进事务并 `bcache_hold_page` 持有，使任何通用 sync 都不能把元数据写在其日志副本之前；`sync()`/`fsync()`触发 commit（数据 → descriptor → 日志 superblock `s_start` → commit block →元数据本位 → 标记日志为空）。挂载时是否回放以 journal superblock 的`s_start` 为权威判据，而不仅是 `EXT4_FEATURE_INCOMPAT_RECOVER`——后者与 free 计数共享同一个被持有的 cache page，崩溃可能丢掉这个位却留下非空日志。

验证：`make smoke-ext4-journal`。它在提交序列的四个点上真的把机器停住（注入点经 `/proc/a20/journal` 写入；同一机制也接受 `a20.journal_crash=`命令行参数，便于手工复现），用同一块镜像重启，断言承诺过的写入没丢、没承诺的写入没回来、`replay complete` 恰好出现在承诺点之后，并在宿主机上用`e2fsck -fn` 检查崩溃后的镜像与恢复后的镜像都干净。走 procfs 而不是命令行，是因为命令行注入点只在启动时解析一次，而 procfs 写入可以在崩溃发生前、提交序列进行到一半时才武装，从而命中的正是那个边界；命令行形式则留给手工复现。这是本文件里唯一一处"断电"不是模拟出来而是真发生过的地方。详见 `docs/testing-gates.md`「ext4 JBD2 崩溃一致性」。

`fsync()` 现在真正到达稳定介质。`block_dev_t` 有可选 `flush` 原语，`bcache_sync_common()` 在写完数据后、缓存锁之外调用它。实现覆盖 virtio-blk（`VIRTIO_BLK_T_FLUSH`）、loop（转发 backing file 的 fsync）、 AHCI（`FLUSH CACHE EXT`）。

验证：`make smoke-fsync-durability`，断言 `block_flushes` 计数器确实增长。在 virtio-blk 之上还有第二条路径：`make smoke-ahci-ich9` 在 x86_64 QEMU q35 上挂 `-device ich9-ahci` + `ide-hd`，把 ext4 镜像接到该控制器上，让`fsync_durability_test` 真的打在 SATA 盘上，并额外断言驱动走了中断完成路径。

### 仍缺

- AHCI 中断路径已在 QEMU 上验证：`make smoke-ahci-ich9` 实跑 PASS （日志 `.kernel-build/smoke/ahci-ich9-x86_64.log`），一个 ich9-ahci port 绑在 MSI 路径上（`[MSI] ... enabled vector 89`、`completion=irq(msi)`）， 17 条命令全部经中断完成、0 条轮询，`/proc/a20/perf` 的`ahci_irq_completions > 0`。用的命令行是`-machine q35 -device ich9-ahci,id=ahci -device ide-hd,drive=xa,bus=ahci.0`。**这仍只是 QEMU 证据**，下面的边界没有因此改变。
- AHCI 仍是单 controller / 单 port / 单 command slot；完成路径现为 MSI（能力 0x05）优先，失败回退 INTx，再回退轮询（`completion=irq(msi)`，见§八 P1）。没有 MSI-X 路径；真实 SATA PHY 上的行为没有任何证据。
- 无 RAID、无数据校验和、无快照/CoW、无 fs-verity。文件数据块本身仍无校验和；`crc32c` 覆盖 JBD2 日志与 ext4 元数据（`metadata_csum`），不覆盖常规文件数据内容。
- JBD2 只支持 checksum v3（`COMPAT_CHECKSUM` 的 v1/v2 返回 `-EOPNOTSUPP`），没有 `barrier` 与 `async_commit` 特性位（`s_features` 中对应位不声明，因此 commit block 不写 `JBD2_FLAG_ASYNC_COMMIT`，设备也没有`ordered`/`journal_data` 语义差别）。断电原子性由 ordered 模式本身提供，不依赖设备 FUA 之外的屏障。

## 二、网络

### 入站 TCP 监听机制（`tcpmode=lwip` 支持）

`net_listen()` 原实现只做 `s->local_tcp = 1` + `net_tcp_drop_pcb()`，未调用 `tcp_listen()`。因此 `/proc/net/status` 的 `tcp_listen=0` 是常态，入站 SYN 找不到 listener 被回 RST。仅存在于 socket 层的 listener 只能被同一内核内走同样快捷路径的本地进程连接。

实测改动前表现：SLIRP `hostfwd` 指向 guest `telnetd:2323` 时，宿主机连接返回 "connection reset by peer"。

当前实现：`net_listen()` 增加 `tcpmode` 两档：`fast`（默认）与 `lwip`（`tcp_listen_with_backlog()` + `tcp_accept` 回调，端口在协议栈原生监听）。改动后同一 hostfwd 实测可正常获取 guest telnetd 的 `A20OS remote shell` 提示符。

选择方式有 `a20.tcpmode=lwip` 内核命令行与 `/proc/net/config` 写入口两处。

**连接关闭与数据交付修复（`edc29d31a`）**：`tcpmode=lwip` 下的回环 TCP 传输曾存在两处边界缺陷：

1. `net_inet_socket_destroy()` 对已建立的 pcb 无条件执行 `tcp_abort()`，会触发 RST 导致对端丢弃未收完的数据。现修正为先解绑回调再 `tcp_close()`，仅在 `ERR_MEM` 时退回 `tcp_abort()`。
2. `net_inet_bottom_half_process_socket_locked()` 曾先处理 `bh_error`/`bh_closed` 再排空 `bh_ring`，导致未处理完的有效交付数据被丢弃。现修正为先排空接收环再处理关闭状态。

实证（`ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=4 dev-build`，QEMU，`a20.tcpmode=lwip`）：修复后同端口连跑 8 次全部 PASS，CLI 报 `recv=2 data=ok`，`make smoke-network-suite` 稳定通过。

优雅关闭产生的 TIME_WAIT pcb 支持通过 `SO_REUSE` 复用：`lwipopts.h` 打开 `SO_REUSE=1`，`net_inet_tcp_apply_options()` 将 `reuseaddr` 映射为 `SOF_REUSEADDR`（参见 `DIVERGENCE.md` §2.6）。

**对端重置后的错误码规范化（`ec47d621e`）**：对端 reset 后 `send()` 曾因 `s->tcp == NULL` 误返回 `ENOTSOCK`。现按套接字类型判定并豁免 fast 路径，沿用 Linux ABI 规则：`ever_connected` 返回 `EPIPE`，否则返回 `ENOTCONN`。实证：`tcp_edge_test` 稳定 3/3 PASS。

### `net_stress_test` 历史性能归因重估

本地 TCP connect 走 `net_inet_send_tcp()` 的 `local_tcp` 短路：直接把数据入队到 peer 的 socket 队列，**不分配 pbuf、不进 lwIP 状态机、不触发 recv 回调**（该路径获取 `(s, peer)` 套接字锁，见 [net/network-lock-contract.md](../../net/network-lock-contract.md)）。

实测（4 核，16 MiB）：`net_rx_packets` / `net_tx_packets` / `net_bh_runs` / `net_bh_events` 全部为 0，`net_lock_acquires` 仅 51。

因此，历史测试中将 `lwip` 锁争用归因于 `net_stress_test` 是不准确的，所测瓶颈实际位于 socket 队列同步。要客观评估 TCP 数据面，必须采用走真实 netif 的测试负载或显式指定 `tcpmode=lwip`。

### 回环接口排空与 RX-pending 门控

`netif_poll()` 是 `netif->loop_first` 的唯一排空点。历史实现中若无外部网卡流量输入，仅有回环流量时，park 在 `connect()` 的任务可能由于未轮询而等待超时。现已将 loopif 排空调度纳入 timer tick（保证基线周期性驱动），并将硬件提示感知与 `loop_first` 状态解耦。

### 网络面现在有了可用的测量口径

`/proc/a20/perf` 此前约 75 个计数器**零个网络相关**，网络唯一信号是`/proc/a20/lock_contention` 里一个没有分母的锁计数。已补 11 个（收发包/字节、`g_lwip_lock` 获取、poll 次数与被门控跳过的次数、bottom-half 次数与事件数，以及`net_bh_overflow` / `net_alloc_fail` 两个**正确性**计数器——非零即表示收包路径丢了自己已接受的数据），另加 accept 路径的 staged/queued/drop。

`/proc/a20/{perf,lock_contention}` 增加 `reset` 写入口。此前计数器自启动累计、没有 reset，压力期与引导期无法分离——这正是 `server-readiness.md` 曾经把"单次 acquire 极值"误归给压力期、后来撤回的那个观测窗口缺陷。`lock_counters_reset()` 连`contended_max_spins` 一起清零：reset 之后要回答的是"本窗口内的最大值"，留着引导期的旧极值会让之后每个窗口都看起来和最差引导期一样坏。

**仍缺**：吞吐与延迟仍无法在本环境给出可信数字。loopback 绕过驱动路径，`/proc/net/dev` 在 `lo` 上读 0；`rdcycle` 在 QEMU TCG 下跨 vCPU 不单调，已产出过 `holdmax≈32s` 这种对微秒级临界区不可能的数值。绝对带宽只能上真机（VisionFive 2 / LS2K1000，GMAC 已在树里）。

### 嵌入式档在 20 KiB SRAM 下放不下 —— 已用实测数字确认

数字来自实测结构体尺寸（探针编译单元 + `/proc/a20/netmem` 交叉验证，两者对六个池完全一致），不是估算。**不能**从 QEMU 镜像的 `.bss` 反推：`MEMP_MEM_MALLOC=1` 让池成为对堆的 claim，而 riscv64 QEMU 目标有 1 GiB RAM，对 20 KiB 部件没有说明力。

### 静态数组预算与档位约束

池上限合计 25696 B 对 `MEM_SIZE` 16384 B（超 57%），但**即使把池全部解决也放不下**。真正的约束曾是两块**与档位无关的静态数组**：

| | 字节 | 位置 |
|---|---|---|
| `g_pkt_ring` | 24,640 | `socket_packet.c`，硬编码 `NET_PACKET_RX_RING=16` × 1540 |
| `g_netif_state` | 12,672 | `lwip_stack.c`，`A20_NET_MAX_DEVS=4` × (rx1536 + tx1536) |
| 合计 | **37,312** | |

对照 20 KiB：`kernel/net` 静态合计 **42,662 B = 整个部件的 2.08 倍**；8 个`net_socket_t` 合计 32,768 B = 1.60 倍；lwIP 堆 16,407 B = 80%。上述数值为静态测算基准。在引入扩展 conntrack 条目（由 56 B 调整为 64 B）后，总静态开销增加约 256 B，结论方向保持一致。这两块当时都在 profile 作用域之外，所以**只改 `net_profile.h`/`lwipopts.h` 无法让 tier 1 装进 20 KiB**。

**现已修（`wt/embedded`）**：`net_profile.h` 新增 `PACKET_RING_SLOTS` / `PACKET_FRAME_SIZE` / `NETIF_MAX_DEVS` / `NETIF_FRAME_SIZE` / `NETIF_MTU` / `STATIC_BUDGET`，EMBEDDED 档取 4 × 512 与 1 个 netif，DEFAULT/SERVER 保持 16 × 1536 与 4 个 netif。总账由断言钉住而不是靠注释：`net_profile.h` 断言宏算术的上界（≤ `STATIC_BUDGET`），`socket_packet.c:40` 与 `lwip_stack.c:178` 各断言一次真实`sizeof()`。

实测（`make dev-build NET_PROFILE=1`，`nm --size-sort` / 运行期 `/proc/a20/netmem`）：

| | 前 | 后 |
|---|---|---|
| `g_pkt_ring` | 24,640 | 2,064 |
| `g_netif_state` | 12,672 | 1,120 |
| 合计 | **37,312** | **3,184**（−91.5%） |

DEFAULT 与 SERVER 两档的同名符号字节不变（`0x6040` / `0x3180`），行为未动。为让这个数字在运行期可读，`/proc/a20/netmem` 在池表**之后**追加一行`static .bss (not from the heap): ...`（放在表外，故不干扰任何 `^POOLNAME <数字>`形态的既有断言）。

**仍未解决，因此本节标题的结论只被部分推翻**：20 KiB 依然装不下 tier 1 —— 8 个`net_socket_t` 单是 32,768 B（1.60 倍）就已经越界，lwIP 堆 16,407 B 也是 80%，这两项都在 profile 之内、且本次未降。64 KiB 按当前配置仍**不够**：42,662 + 16,871 = 59,533 B（90.8%），余 6,003 B —— 装不下两个 socket。净效果是 tier 1 的静态部分从"整个部件的 2.08 倍"变成"约 0.16 倍"，即**剩下的账全在 profile 之内、可以按档位调的量上了**；要真做到 30 KiB，还需要 profile 内降 `MEM_SIZE`/池/`MAX_SOCKETS` 与关 IPv6。

### 嵌入式档并发容量与压力测试边界

**0/5 通过**（tier 2 是 5/5）。原因确凿：`run_worker` 每个 worker 用 3 个 socket （client connect + server listener + accepted child）× 4 worker = 12 个并发，而 tier 1 的`NET_MAX_SOCKETS = 8`。单次偶发 PASS 只是 worker 未同时到达峰值。按要求报出来而非上调上限 —— 这说明"tier 1 可运行"这个说法需要限定。

**现已改为显式 SKIP**：`net_stress_test` 在起跑前从 `/proc/net/status` 读档位上限（`syscall-sockets: ... max=N`，为此在 `kernel/net/socket/socket.c` 的该行末尾追加了`max=`），算不出 `WORKERS * 3 < 上限` 时输出

```
NET_STRESS_TEST: SKIP (socket table ceiling 8 < 12 concurrent sockets required:
4 workers x 3 (listener + accepted child + client)); tier too small for this
workload, upper limits left as configured
```

并以 0 退出。**上限没有上调，也不会有门禁因为这个 SKIP 变绿**：所有以`NET_STRESS_TEST: PASS` 为判据的门禁（`smoke-smp-lock-contention` / `smoke-net-lanes` / `smoke-net-lanes-n1`）都匹配不到这一行，档位过小时它们仍然失败。上限在运行时读而不是编译进测试，是因为用户态构建不随 `NET_PROFILE` 重建（`Makefile:447` 的 `USER_BUILD_ID` 只含 `ARCH/NOMMU/OPT/PROFILE`）；读不到时照常跑测试，不会因缺证据而静默跳过。实测：tier 1 输出上面的 SKIP（`max=8`），tier 2 仍是`NET_STRESS_TEST: PASS (4 parallel transfers, 4 rounds x 1048576 B)`（`max=1024`）。

### 嵌入式档资源预算断言与能力权衡

上一条之前的两条已经把静态数组（`g_pkt_ring` / `g_netif_state`）纳入档位，但当时留了一句"剩下的账全在 profile 之内、可以按档位调的量上了"，并且没有做。本节把它做掉：档位现在有一个自己声明、自己断言、自己运行期可读的总账，代价是**明确的协议/并发能力削减**，下面逐项列出。

**总账（riscv64 LP64 实测，`make dev-build NET_PROFILE=1`）**：

| 项 | 字节 | 断言位置 |
|---|---|---|
| socket 表 `8 × sizeof(net_socket_t)` | **19,520** | `kernel/net/socket/socket_internal.h:457`，对上 `NET_PROFILE_SOCKET_BUDGET` = 20 KiB（余 960 B） |
| 帧数组（netif 状态 + AF_PACKET 环） | 4,100 | `kernel/net/socket/socket_packet.c` 与 `kernel/net/backends/lwip/lwip_stack.c` 各自的 `sizeof()` |
| filter 表（conntrack + NAT 规则） | 3,328 | `kernel/net/filter/netfilter_nat.c:130` 的 `sizeof()` 断言，**正好等于** `NET_PROFILE_FILTER_BUDGET` = 3,328 B，余量归零（条目在 `810e9e431` 后由 56 B 涨到 64 B；`netfilter_nat.c:118-131` 的注释仍写旧的 3,072 B，已过期） |
| lwIP 堆 `MEM_SIZE` | 16,384 | `memp/mem.c` 里就是这么大一个静态数组 |
| 合计 | **43,796 B（LP64，约 42.8 KiB；ILP32 41,364 B）** | 上限 `NET_PROFILE_TOTAL_BUDGET` = 44 KiB。求和项：socket 表、`(ring 槽 + drain 槽) × slot`、每设备 netif state、`g_netifs + g_loopif`（384 B/个预算）、filter 表、heap、per-lane memp 表 |

运行期可读：`/proc/a20/netmem` 在池表之后多两行（`kernel/net/backends/lwip/lwip_stack.c:1947`）。EMBEDDED 档实测：

```
static .bss (not from the heap): netif_state=1152 netif=368 pkt_ring=2580 total=4100
socket table: per_socket=2440 slots=8 bh_ring=2 inline_payload=256 total=19520 budget=20480
```

只有 EMBEDDED 档定义了 `NET_PROFILE_SOCKET_BUDGET`，所以另外两档这一列打印 `budget=n/a`而不是 `0` —— 打印 0 会被读成"3100 万字节的 socket 表对 0 预算"，那是一个内核并不持有、也没有任何断言支持的越界结论。DEFAULT 档实测同一行是`per_socket=30552 slots=1024 bh_ring=16 inline_payload=1600 total=31285248 budget=n/a`， ring 深度与内联载荷与改动前逐字节一致。

**池上限与堆对账**（子项 2）。此前 tier 1 的池上限合计 22,880 B，而 `MEM_SIZE` 只有 16,384 B —— 声明了 22 KiB 的池容量却只有 16 KiB 的堆。在 `MEMP_MEM_MALLOC=1` 下这不是无害的夸大：`memp` 没有 per-pool 上限，所有池从同一个 `mem_malloc()` 里抢，差额会以`/proc/a20/netmem` 上某一行 `err > 0` 的形式冒出来，看起来像一次莫名其妙的收包丢失。现在 `net_profile.h` 把每个池的元素尺寸上限（向上取整到 8 的倍数，故只会让断言提前发火）写成宏，求和得 `NET_PROFILE_MEMP_CLAIM_BYTES`，并在 `net_profile.h:251` 断言它不超过`MEM_SIZE`：13,332 B 对 16,384 B，余 3,052 B 给 `memp` 不服务的那些分配（`pbuf_custom` 链、netconn、DNS 表、lwIP 自身）。`MEMP_NUM_ND6_QUEUE` / `MEMP_NUM_MLD6_GROUP` 原来根本没走 profile，直接吃 lwIP `opt.h` 的默认值 20 / 4 —— 那是给另一个部件定的数，现在进 profile（`kernel/net/backends/lwip/port/lwipopts.h:117-118`）。

**因此减少的能力，逐条**（这是档位定义，不是回归）：

| 能力 | 之前 | 现在 | 换来了什么 / 代价是什么 |
|---|---|---|---|
| 每 socket 收包 staging 深度 | 4 | **2** | ring 满时 `net_inet_tcp_stage_payload()` 返回 false → lwIP callback 回 `ERR_MEM` → pbuf 进 `refused_data` 重试。**不丢段**，但突发吸收从 4 段降到 2 段，突发下的延迟与重传变差 |
| 每 socket 内联载荷 | 320 B | **256 B** | 256 是地板不是圆整值：`socket_inet.c` 断言 `NET_BH_INLINE_PAYLOAD >= TCP_MSS`，本档 MSS 就是 256。再低则每段走 spill（按 pbuf 引用暂存），正确但每段多一次引用计数、拷贝变成链表遍历 |
| `SO_SNDBUF` / `SO_RCVBUF` 上限 | 8 KiB | **2.5 KiB** | 收紧到与 ring 同量级；旧上限是它所守护的结构体的三倍，等于守不住 |
| pbuf pool 元素数 | 24 | **10** | 单个 536 B 元素，本档同时在网的整尺寸帧从 ~8 降到 ~4 |
| TCP 段缓存 `TCP_SEG_MULT × WND_MULT` | 16 × 4 = 64 | **8 × 4 = 32** | 拥塞时的排队深度减半 |
| IP 重组 / 分片 | 16 / 32 | **8 / 8** | 分片重组深度降到 8 段 |
| IPv6 邻居队列 / 组播组 | 20 / 4 | **6 / 2** | 邻居发现与组播的并发等待项都按比例缩小 |
| conntrack 表项 | 64 | **32** | 同时跟踪的流 64 → 32。表是无条件 `.bss`，每次启动都付，即使 netfilter 从未加载 |
| 链路 MTU | 1536 B 帧缓冲 | 512 B 帧缓冲 → **MTU 上限 498** | 嵌入式档的 MTU 由 profile 的帧缓冲决定，`a20_lwip_if_set_mtu()` 拒收 `mtu + ETH_HLEN > 帧尺寸` |

**DEFAULT 与 SERVER 两档逐字节不变**：ring 深度、内联载荷、socket 预算、池上限、conntrack 表项在这两档都没有被改，新增宏的缺省值就是 `lwipopts.h` 原有的值（`net_profile.h:463-473`）。两档的 `dev-build` 门禁（`NET_PROFILE=2` / `=3`）通过。

**20 KiB 依然装不下，这一点没有变，也不打算变**。八个 socket 就是 19,520 B，本身占满 20 KiB 部件的 95%，一个 socket-capable 的 lwIP 塞不进剩下的 960 B。因此该档位规格明确声明需要 44 KiB 内存预算（对应 `NET_PROFILE_TOTAL_BUDGET`），彻底消除 20 KiB 虚假预算的承诺。README 里的 STM32F103（20 KiB SRAM）仍然不编译网络栈，见下一条。

### MCU 配置网络栈支持边界

两个独立障碍：

1. 本机无 ARM 工具链（无 `arm-none-eabi-gcc`）。
2. 更根本：`PROFILE=mcu`（armv7m 自动启用，`Makefile:247-248`）走 `Makefile:1166` 的**另一份源文件清单**（`components/trim.toml` 的 `[profile.mcu].sources`），其中**既无`kernel/net` 源文件也无 `$(LWIP_SRC)`**。所以 `NET_PROFILE` 对该目标完全没有作用，**tier 1 根本没被编译进去**。

即 README 声称的 STM32F103（20 KiB SRAM）支持，在网络上不只是"内存不够"，而是**从未构建**。

**已处理（本条按"修 README"的方向关闭）**：README「支持的硬件平台」的 MCU 条目现在明写"**不含网络**"，说明 `PROFILE=mcu` 的独立源清单里既没有 `kernel/net/*.c` 也没有 lwIP、`NET_PROFILE` 对该目标无效，并把"给 mcu profile 真的纳入网络栈"记为尚未做的**产品决定**而非调参；`make stm32f103-bringup` 的注释也一并标注"无网络栈"。（行号随本节引用漂移：`Makefile:931` 现在是 CFLAGS 段，MCU 源清单在 `:1166`。）

### 帧缓冲区与 MTU 动态适配

原附带项：`lwip_stack.c` 的 `rx_frame`/`tx_frame` 硬编码 1536，不随 `PBUF_POOL_BUFSIZE`变，所以 Cortex-M3 会把整尺寸帧收进 512 字节的池，每帧链 3 个元素；`PBUF_POOL_SIZE=24`只够 8 个整尺寸帧，而整个部件只有 20 KiB。

**现已修**：帧缓冲区改为 `NET_PROFILE_NETIF_FRAME_SIZE`（EMBEDDED 档 = `PBUF_POOL_BUFSIZE` = 512），并且 `NET_PROFILE_NETIF_MTU`（= `PBUF_POOL_BUFSIZE - ETH_HLEN` = 498）跟着走 ——只改缓冲区不解决链元素的问题，MTU 还是 1500 时收上来的帧仍要链 3 个元素。DEFAULT 与 SERVER 档这两个值就是原来的 1536 与 1500，行为不变。同时补上了`a20_lwip_if_set_mtu()` 这个运行时漏洞（它此前只校验 RFC 791 下限，`SIOCSIFMTU=1500`会让收包截断、发包 `ERR_BUF`），现按档位帧尺寸封顶。

**未验证的边界**：`a20_lwip_if_set_mtu()` 的新封顶路径没有任何门禁——树内除 musl 头文件外没有 `SIOCSIFMTU` 使用者。DEFAULT/SERVER 的上限 1522 高于历史 1500，所以现有行为不变，但这条路径未被任何 smoke 覆盖。EMBEDDED 档 MTU=498 与`net_stress_test` 的 SKIP 行为只在 riscv64 QEMU 上实测过（`TCP_LOOPBACK_TEST` / `ICMP_LOOPBACK_TEST` PASS）；本机没有 ARM 工具链，且 `PROFILE=mcu` 根本不编译网络栈，所以 Cortex-M3 上的行为**无实测证据**。

### `g_net_lock` 分片 —— 已完成（`2f17a5ba8` + 阶段 E `7c7a4d7c8`）

本文件此前把"`g_net_lock` 分片"列为 P0「当前收益最大的未做项」。**它已经做完了**，而且分两步：`2f17a5ba8` 删掉 `g_net_lock`、把 socket 表分片成 `g_net_buckets[]`；阶段 E 再把 per-socket 状态从桶锁搬到 `net_socket_t` 内嵌的 `spinlock_t lock`（`socket_internal.h:448`）。桶锁现在**只**管 registry slot 表与每桶空闲位图。

对服务器的净影响：SERVER 档一个桶是 512 个 slot，所以阶段 E 之前一个 socket 的`recv` 要和同桶另外 511 个 socket 互斥——**桶号来自 slot 分配顺序，不来自 lane**，所以 lane 分得再细也没用。阶段 E 之后只剩真正共享同一对象的那一对。

`2f17a5ba8` 的提交说明自陈三项运行期验证**当时**未做——引用计数不漏不重、`LOCK_COUNTERS_MAX` 注册预算、`-ENOTCONN` 窗口。**这三项现在都跑起来了**：前两项在 4 CPU + 4 lane 下 20 次引导全部干净，读数见§八对应行；第三项做成了**可观测而非可判定**——13 处 `-ENOTCONN` 各自带 reason，其中 4 个属于 §8.5 那个窗口，`/proc/net/status` 分开打 `total=` 与 `window=`（`socket.c:105-120`、`:147-166`）。20 次引导读数是 `total=0 window=0`：**没命中过，不等于该窗口不存在**。仍未做的是**合并后全量回归与 ASAN + SMP 压测**；阶段 C/D/E 自身也只做了各自的编译期与逐字节等价自检。锁规则写在[net/network-lock-contract.md](../../net/network-lock-contract.md)。

### IPv6 入站 TCP：AF_INET6 socket 现在真的能拿 pcb（`c34ddd7f8`）

此前 AF_INET6 流式套接字在**两种 TCP 模式下都无法从外部 accept**，而且对端 FIN 会静默丢掉对端已经发来的数据。两件事都已修。

v6 入站不可达有五处各自独立的丢弃点：`net_inet_socket_init()` 只对 AF_INET 调`tcp_new_ip_type()`，所以 v6 socket 一辈子 `s->tcp == NULL`；`net_inet_bind_pcb()`对 AF_INET6 提前返回 0，留不下 pcb，于是 `listen()` 没有东西可转成 LISTEN pcb；`net_listen_sock()` 把 lwIP LISTEN-pcb 路径按 AF_INET 门控；`net_inet_connect_stream()`要求 `domain == AF_INET`，于是每个 v6 connect 都得到 `-ECONNREFUSED`——一个误导性的 errno，因为没有任何东西拒绝过；accept drain 硬编码 `child->domain = AF_INET`，于是被接受的 v6 连接把 16 字节的 v6 地址按 v4 布局读回来。

lwIP 的 `tcp_pcb` 本身是双栈的：family 来自交给 `tcp_bind` / `tcp_listen_with_backlog` 的那个 `ip_addr_t`，不需要任何 pcb 机制。要做的是 socket 层别再把 family 扔掉。

数据丢失是另一个与 family 无关的既有缺陷，是 v6 的工作把它暴露出来的：`net_inet_bottom_half_process_socket_locked()` 在对端 FIN 到达时置 `s->closed = 1`，而 `s->closed` 是**本地**生命周期标志、`net_socket_is_live()` 是`s && !s->closed`——于是一个健康 socket 报告自己死了并从 `/proc/net/tcp` 消失。更糟的是 FIN 分支跑在同一趟的 staging 循环**之前**，那个循环的 `if (!s->closed)` 守卫于是把 ring 里还没搬的每个载荷事件一起丢掉。lwIP 先交付数据再交付 FIN，所以对端一次`write(); close()` 把两者放进同一趟 bottom half，消息的尾巴被扔了。现在对端 FIN 只置 `s->peer_closed`（`socket.c` 本来就当它 EOF）加一次 `A20_EVENT_CLOSED`。

隔离方法值得记一笔，因为它是**量出来的不是猜的**：一个按 family 参数化的探针显示 v4 与 v6 **在同一偏移截断**，在 stash 出来的基线上重跑该探针，v4 也在同一偏移截断——所以丢失是既有的、不是本次引入的，而 v6 的 accept 是彻底超时。在对端 close 之前插 8 秒 sleep 让两者都过，于是定位为"与对端 read 的竞态"而非发送故障，指向 bottom half 的顺序，修复确认了这个判断。

`/proc/net` 下现在有真的 `tcp6` 与 `udp6`（`procfs.c:464-469`），按 v6 布局渲染， v4 文件不再混入 v6 行。**残留**：rtnetlink 组播只覆盖 IPv4 地址组，没有 IPv6 地址写入路径。

### TCP 选项与拥塞控制从 opt.h 的默认 0 变成档位决定（`89bb8c348` / `0d42ca758`）

SACK（RFC 2018）与 TCP 时间戳（RFC 7323）在上游 `opt.h` 里默认是 0，于是对着真实对端只有 Reno 的丢包恢复、没有 PAWS。现在按 `net_profile.h` 的档位决定：DEFAULT 与 SERVER 打开（各 4 个 SACK 块），EMBEDDED 关掉——在 512 B 的池元素上这两个选项的开销直接从载荷预算里扣，而那个档位的目标是有界占用。代价记在同一处：选项与载荷**共用**池的头元素，所以 `NET_PROFILE_PBUF_BUFSIZE` 必须从 1536 涨到 1600，账是`1460 + 54 + 12 + 36 = 1562`，并由 `lwipopts.h:214` 的 `_Static_assert` 钉住。 DEVICE 侧的 netif 暂存仍是 1536——它装整帧（1500 + 14），不是带选项的段。

`TCP_CONGESTION` 此前接受 `"cubic"` 并返回成功，**而本树没有 CUBIC**：一个`cubic-but-not-really` 的名字会让调用方以为自己拿到了算法。现在未知名返回`-ENOPROTOOPT`，`getsockopt` 回真实算法名，`"cubic"` 走 RFC 8312 的核心条款。

**CUBIC 未做的部分**（逐条登记在 `DIVERGENCE.md` §2.7 与`lwip/priv/tcp_cubic_priv.h` 的头注释）：没有 HyStart / TCP-AQ / DCTCP / Prague，没有 ECN，`W_max` 不跨 pcb 持久化。

**§4.2 的 TCP-friendly 区域规范实现**（`8115a0c1a`）：`tcp_cubic_w_est()` 按 RFC Eq. 4 计算 `W_est(t) = W_max*beta_cubic + alpha_aimd*(t/RTT)`，与 `W_cubic(t)` 取**较大者**——RFC 4.2 明确 "cwnd SHOULD be set to W_est(t)"，标准友好线作为下界保障。此前采用的 Reno 速率近似未参考 `t/RTT` 与流本身 `W_max`，无法保证不低于标准 TCP 的收敛速率。

**实现带来的边界**（必须一起读，否则会以为 4.2 在大窗口上也在起作用）：lwIP 2.2.x 的 `struct tcp_pcb` 没有 `rtt` 字段，本栈唯一能测的 RTT 是 `pcb->sa`，以整个`TCP_SLOW_INTERVAL`（500 ms）为单位采样，所以 Eq. 4 的分母在 0.5 s 以下没有分辨率；`sa == 0`（回环、大多数局域网）兜底成一个 tick。于是该区域只在 `W_max` 低于约 3.25 段时可能成为约束项——**在 CUBIC 真正要对付的大窗口传输上它不会成为约束项**。毫秒级 RTT 需要在 `tcp_in.c` 里更新一个新 pcb 字段，未做。门禁`tools/host-tests/test-tcp-cubic-host.sh`（`test_tcp_friendly` 跨 (t, RTT) 钉住 Eq. 4，`test_friendly_region_binding` 在 `sa=1` / `sa=40` 下把区域选择推过真实 ACK 路径）。

### `SO_SNDBUF` / `SO_RCVBUF` 不再是被接受然后忽略（`aacce4dcc`）

`SO_SNDBUF` 现在约束该 socket 在 pcb 发送队列里的字节上限，`net_inet_send_tcp()`拿它量队列深度；`SO_RCVBUF` 经 pcb 的新字段 `wnd_limit` 约束该连接的接收窗口（没有这个字段，上限会活到第一次 `read()` 为止，因为 `tcp_recved()` 每次读都把窗口原样还给 `TCP_WND`）。`getsockopt` 回读夹紧后的生效值。

边界四条，都写进 `net_inet_tcp_buf_apply()` 的注释：发送侧**不约束在途飞行字节**（在途是拥塞控制的事）；**发送侧有界自动调优**（初始 32 KiB，窗口耗尽时倍增到`TCP_SND_BUF`，显式 `setsockopt` 关闭；接收侧默认已是 `TCP_WND`，没有可增长空间，`wnd_limit` 只是显式 `SO_RCVBUF` 的软上限）；两者都是 **TCP 范围**（UDP/RAW 的语义见下一节）；**不跨连接持久化**（每次`connect()` 新建 pcb，ceiling 由 `net_inet_tcp_buf_apply()` 重新写进去）。

**实现修正项**：

- **抬高 `SO_SNDBUF` 现在对既有连接立即生效**（`272c80a2f`）。此前 ceiling 只在**下调**时写进 `pcb->snd_buf`，抬高要等下一次 `connect()`——那是对 lwIP 的准确描述，但对服务器是个坏答案：一个长连 socket 上的一次 `setsockopt()` 在客户端碰巧重连之前静默无效。lwIP 没有 setter（`snd_buf` 只被 `tcp_write()` 扣、被 ACK 路径加回，此外无人写），所以直接双向写这个字段，依赖的不变量以表格登记在 `DIVERGENCE.md` §2.10。
- **发送队列深度与预留死锁修正**。发送路径原先另外维护"队列深度 = `TCP_SND_BUF - pcb->snd_buf`"的估计再拿它和 ceiling 比——而 ceiling 已经被写进`snd_buf` 了，导致预留空间被误判为已排队并引发空队列阻塞。当前实现直接查询 pcb 实时可用空间。

UDP/RAW 侧的 `SO_SNDBUF` / `SO_RCVBUF` 已支持并执行（`92b399e5d`）：发送侧约束单个数据报大小（超出返回 `-EMSGSIZE`），接收侧约束队列字节总数（装不下就丢，`-EAGAIN`）。语义差异（无窗口缩放折算、不约束在途、接收侧是丢包不是反压）逐条写在 [net/network-config-design.md](../../net/network-config-design.md)「UDP/RAW 的 `SO_SNDBUF` / `SO_RCVBUF`」一节。接收侧丢包不合成 ICMP port unreachable。

修 `SO_RCVBUF` 时曾引入过一条回归：`socket()` 时发出杂散 ACK。成因是拿`TCP_WND_MAX(pcb)` 与 `pcb->rcv_scale` 当夹紧上限——两者在握手前都不对（`TCP_WND_MAX()` 此时答 65535 而非 93440，`rcv_scale` 仍是 0），会把每个 socket 从创建那一刻永久钉在 64 KiB。症状是 IPv6 回环传输在 lwIP TCP 模式下不完成而 fast 模式下正常（后者根本没有 pcb）。改为用**编译期常量** `TCP_WND` 与 `TCP_RCV_SCALE`夹紧，另加"只有收窄才需要通告窗口"的判定。回归定位与修复见 `f34b451f4`。

### RTNETLINK 现在真的会通知（`8b15e3323` / `f7524bb32`）

`NETLINK_ROUTE` 套接字 bind 到 `RTNLGRP_LINK` / `RTNLGRP_IPV4_IFADDR` 时，分别会收到 `RTM_NEWLINK`（链路状态变化）与 `RTM_NEWADDR` / `RTM_DELADDR`（IPv4 地址写入或删除）。地址全零时发 `RTM_DELADDR` 而不是 `RTM_NEWADDR`——"你现在有地址 0.0.0.0"不是任何接口处于的状态。一个监听者的队列满不挡其他监听者（计数并 klog 一条）。

**IPv6 地址组支持（`f7524bb32`）**：补齐 `RTNLGRP_IPV6_IFADDR` 组号与能往里投递的写入路径（`a20_lwip_if_set_addr6()`，`kernel/net/backends/lwip/lwip_stack.c:2132`），`nlrt_apply_addr6()` 把 `ifa_family == AF_INET6` 的 `RTM_NEWADDR` 接到其上。`netlink_test` 的 IPv6 用例对该行为进行了回归校验。

**当前边界**：`RTM_NEWADDR` 与 `RTM_DELADDR` 都已实现（删除把槽位置`IP6_ADDR_INVALID`，不存在返回 `-EADDRNOTAVAIL`，事件为 `RTM_DELADDR`）；不做 DAD（直接置 `IP6_ADDR_VALID`，监听者不得把事件读成"DAD 通过"，也没有 DAD 完成事件）；`ifa_prefixlen` 只校验不落地；ND6 自学地址与生命周期转换不发通知（`LWIP_NETIF_EXT_STATUS_CALLBACK=0`，已知边界）。逐条见[net/network-config-design.md](../../net/network-config-design.md) 的 RTNETLINK 一节。

### 驱动：SG 发送与能力位上报已接线，校验和卸载刻意不做（`d12733f48` / `089a2055c`）

`net_dev_ops_t` 增加了可选的 `send_sg()` 与 `caps()`（`driver_class.h:201-207`），`lwip_stack.c` 在 `a20_lwip_linkoutput()` 里二选一。**没实现 `send_sg` 的驱动走原线性拷贝路径，逐字节不变**——但要注意 SG 分支**不是零拷贝**：命中前提是`p->next == NULL`，帧仍然先拷进 `st->tx_frame`，因为 netfilter 的 output hook 是对调用方手里那块帧的**原地改写**，而 pbuf payload 是别的持有者也在看的池内存。SG 给驱动的是描述符而不是 lwIP 的 pbuf，省掉的是驱动内部再拷一次。

**校验和卸载按诚实原则未启用**：`TX_CSUM_OFFLOAD` / `RX_CSUM_OFFLOAD` 两个能力位已定义（`driver_class.h:171-172`）但**永不置位**。此前记录的理由（"vendored lwIP 2.2.2 没有任何承载该握手的 flag"）经复核**不成立**，已更正：位与字段都在（`netif.h:140-153`、`:340-342`、`:408-417`），只是被`LWIP_CHECKSUM_CTRL_PER_NETIF` 关着（`opt.h:2371-2373` 默认 0）。不启用的真实理由是三条接不上的线：TCP 发送路径 `tcp_out.c:1587-1596` 无条件写完整校验和、lwIP 的位是**每 netif** 而不是**每帧**而本 HAL 的 `recv()` 只回长度、QEMU 的 virtio-net 从不设置`VIRTIO_NET_HDR_F_DATA_VALID`。完整调查与"要启用需要改哪些上游文件"见`docs/net/checksum-offload.md`，意向已登记在`kernel/external/lwip/DIVERGENCE.md` §2.11。`MRG_RXBUF` 不同：它是纯设备侧的接收属性，lwIP 从来看不见，所以协商了的驱动**应该**上报它。

virtio-net 收发环 32 → 256 并协商 `MRG_RXBUF`；e1000 环 64 → 256、多缓冲帧线性重组、 82574L ITR 节流。**MRG_RXBUF 的重组分支现在由真实流量跑到了**：接收缓冲改为 512 字节载荷的小缓冲（各占一条独立 avail 条目、非链式描述符），按 QEMU 的收包循环算术，一个满 MTU 帧跨 3 个缓冲、600 字节上下的帧跨 2 个。QEMU 10.0.13 下实测首个跨缓冲帧打出 `[VIRTIO-NET0] first frame reassembled from 2 buffers (rxbuf=524)`，同一轮里 guest `wget` 取回 65536 字节且 `wc -c` 复核为 65536、`ping 10.0.2.2 4` 为 4/4。原先"重组永不触发"的原因是投递的缓冲比帧还大，而"拆成多个描述符就收不到"的原因是把头拆去了另一个描述符——取舍与三组实测数据写在`virtio_net.c` 的 `VIRTIO_NET_RX_DESC_MAX` 注释里。

**这一条的门禁边界**：`make smoke-network-suite` 本身**不**覆盖跨缓冲收帧——它跑的是回环与 telnetd/DNS 的小帧，`ping` 回包和 TCP 握手帧都装得下一个 524 字节缓冲，`num_buffers` 恒为 1，所以该套件通过时日志里不会有重组行。跨缓冲路径要单独用上面那种"guest 向宿主 HTTP 服务器拉大文件 + ping"的实测来证明。`VIRTIO_NET_F_MQ` 只探测不协商（多队列未实现），启动行以 `offered(mq=.. csum=..)`打印，不存在"协商了却没用"的半成品。

**验证边界**：QEMU 的 e1000 是 82540EM，无 ITR 寄存器，驱动按设计跳过（打印`itr=0us`）——这一条仍是"只做到编译 + QEMU 启动验证（ring=256）"。e1000 多缓冲重组**已被真实跨描述符帧跑到**：默认 RCTL 丢弃 1514 字节以上的帧（`e1000x_is_oversized()`），所以驱动现在置 `RCTL.LPE` 并把 `JUMBO` 定在两个缓冲（4074 字节）。帧由`-netdev socket,id=n0,udp=127.0.0.1:<P>,localaddr=127.0.0.1:<P+1>` 从一个普通 UDP socket 注入，一个数据报即一帧（slirp 无 mtu 选项、本机无 raw socket，都产不出 2048 字节以上的帧；UDP 形式比 `fd=N` 形式少一道 fd 传递，QEMU 10.0 才肯收）。**正反对照都实测过**：同一份注入脚本（2842 / 3000 / 1500 / 64 字节各一帧，帧头目的 MAC 填 guest 的 `52:54:00:12:34:56`）在 `RCTL.LPE` 置位时打出

```
[E1000] ready: mac=52:54:00:12:34:56 link=up irq=87 ring=256 itr=0us
[E1000] first frame reassembled from 2 descriptors (buf=2048)
```

把 `E1000_RCTL_LPE` 从 RCTL 写入里去掉、重编、重跑同一脚本：ready 行照旧，**重组行完全不出现**——四种长度的帧全被设备按 oversized 丢掉。所以决定性的是 LPE，不是缓冲大小。**未做到的**：这样的帧在 lwIP 里仍会被丢弃，因为链路 MTU 是 1500 （`net_profile.h`），端到端 jumbo 需要档位表与 pbuf 一起改，属协议栈范围。

### conntrack + NAT 已落地（`f48a8f5f2` / `0d9d0885f` / `4b4472d17` / `810e9e431`）

五元组哈希表（**EMBEDDED 32 条** / DEFAULT 256 / SERVER 1024，按档位）、NEW/ESTABLISHED 状态、按状态分开的空闲超时（TCP NEW 30s、ESTABLISHED 120s、UDP 30s、**ICMP echo 30s**）、满表 LRU、`/proc` 计数。SNAT/MASQUERADE 在 output hook，DNAT 在 input hook，就地改写帧并按 RFC 1624 增量修校验和。运行时动词挂在 `/proc/a20/netfilter`（`ctflush` / `cton` / `ctoff` / `natadd` / `natdel` / `reset` / `flush`）。

新增门禁 `make smoke-netfilter-nat`：QEMU `hostfwd` 18081 → guest 18082，宿主侧探针连上并收到回显。

**四个必须一起说的边界**：

- **每条目挂正向 + 回程两条链**，否则回程按转换后元组查不到条目。这是本实现最容易做错的一处：只挂一条链的失败模式是**静默**的——正向翻译正常、回程查不到、原样发出，对端收到一个不认识的源地址，没有任何计数器会动。
- **DNAT 与 SNAT 都有端到端门禁**。`smoke-netfilter-nat` 覆盖入站 DNAT/TCP；`smoke-netfilter-snat` 覆盖出站 SNAT/TCP：guest 主动连 `10.0.2.2`，宿主回显， guest 断言 `action=snat`/established，并做删规则 + `ctflush` 的阴性对照。`masquerade` 相对 `snat` 的差别仅是取址时机（`a20_lwip_netif_ipv4(-1)` 选当前有地址的 netif），**未在真机上验证过 DHCP 换址后的行为**；QEMU user-net 在 guest 外还会做一次转换，所以宿主探针看不到 guest 改写的源端口，端口改写由`test-nat-rewrite` 的主机侧一次性重算覆盖。
- **LRU 淘汰与空闲超时两条路径曾只有 `/proc` 计数暴露**（`ct_evicted` / `ct_timeout`），没有门禁真的把表填满或跑满 30s/120s 超时——这一条**已补**（`faa8ca1b3`）：新增`make smoke-ct-capacity`，把表填到 `ct_capacity` 恰好停住、多一条流恰好淘汰一条，并用`ct_lru_victim` 断言被淘汰的是最久未用的那一条（淘汰前后各读一次）；随后`cttimeout 100 100 100` 把超时压到 100ms，断言条目在截止时间内被回收、`ct_timeout`恰好 +1 且 `ct_sweeps` 前进。代价是两个 `/proc` 测试钩子：`ctinject` 经数据面同一个`netfilter_ct_insert` 插入合成流，`cttimeout` 运行期覆盖三个毫秒常量（0 恢复默认）。它们不新增内核状态，只是让断言不必靠 256 条真实流和 30 秒等待达成。
- **SNAT/MASQUERADE 的地址与端口改写有主机侧单元门禁**：`test-nat-rewrite` 直接编译出货源码（`kernel/net/filter/netfilter_rewrite.c`），校验和用独立的一次性重算 oracle 对拍；出站 SNAT 另有 `smoke-netfilter-snat` 的端到端回程场景。

**ICMP 跟踪范围与机制（`810e9e431`）**：当前 conntrack 支持 **ICMP echo（type 8/0）**，**不跟踪 ICMP 差错报文**。做法是把表里那两个 16 位字段对 echo 当作**类型与标识符**用——类型一律归一化成请求值 8 存进`src_port`、标识符存进 `dst_port`——于是回程匹配不必到处开特例：echo 的两个方向**只交换地址，两个字段都不换**（标识符在请求与回程里是同一个数，这正是它们成对的依据），TCP/UDP 那套"两个端口全换"的比较式永远匹配不上它，所以 `netfilter_ct_icmp_is()`单独处理。归一化后这个比较退化成"地址交换、两个字段相等"。

**机制设计考量**，以及由此产生的两条限制，写在 [net/conntrack-nat.md](../../net/conntrack-nat.md) 的「实现与设计边界」：echo 的请求与回程由发送方自选的 identifier 配对，会话特征在线上可直接匹配；把差错报文配到流上必须解析其内嵌的原始报文头，属于 ALG 范围。**因此依赖 ICMP 差错跟踪的路径 MTU 探测在当前版本不生效**——经过 NAT 且需要 PMTU 的流若遇分片需求会触发黑洞。另外 echo 只按地址翻译（没有端口，`toport=` 无处可落）、标识符从不翻译； ICMP 校验和覆盖 ICMP 头与载荷而**不覆盖 IP 地址**，所以地址改写不必动它。分片的 echo 既不被跟踪也不被翻译（解析器只在未分片的包上认出 echo 类型）。

其余限制（无 ALG、不跟踪 ICMP 差错报文、不做分片 NAT）写在 [net/conntrack-nat.md](../../net/conntrack-nat.md) 的「实现与设计边界」一节，不重复。

锁同步审计注意项：此前曾误判 `/proc` 读者未取锁并额外加锁导致自死锁（`spin_lock_irqsave()` 不可重入，已在 `4fde1c089` 回滚）。分析并发问题需首要排查重入与调用链完整覆盖。

### 核心锁断言接上了，net 锁一侧也接上了（`7d217d3fd` / `2dd28758c` / `067d0eb96`）

本文件此前隐含一个前提：`docs/net/network-lock-contract.md` 只是文档，没有运行期强制手段。这**不再成立**，而且两侧都不再是散文。

**lwIP 侧**（`7d217d3fd`）：`LWIP_ASSERT_CORE_LOCKED()` 从上游的空宏接到 `g_lwip_lock`的**持有者 CPU** 上：违规先记站点再 panic，`/proc/net/stats` 打印`armed= / owner= / violations= / sites=`。开关 `CONFIG_NET_LOCK_ASSERT` 默认 0 （排查用，不进常规构建）。同时给 7 个此前完全没有断言的裸入口补上断言，把探针盲区变成覆盖区。

**net 锁侧**（`2dd28758c`）：新增 `kernel/net/socket/net_lock_probe.c` 的 per-CPU 持锁集合探针，逐次取锁检查重复取锁、socket 锁地址乱序、第三把 socket 锁、桶锁取在 socket 锁之下、第二把桶锁。开关复用同一个 `CONFIG_NET_LOCK_ASSERT`，并新增第三档 `=2`（只计数不 abort，供长跑取最终计数）。`/proc/net/stats` 上与 lwip 行并排打印`net_lock:` 一行，`=0` 的构建显式打印 `not checked`。

**配套的一条**（`067d0eb96`）：`lock_counters_register()` 超出 `LOCK_COUNTERS_MAX`此前是**静默丢弃**，于是 `/proc/a20/lock_contention` 会对一把从未被观测的锁报"干净"。现在有被拒计数，且 `net_socket_registry_init()` 自己核对注册循环前后的差值是否等于 `NET_SOCK_BUCKETS`（`kernel/net/socket/socket_registry.c:109-122`），不足就报"contention 审计已经半盲"，`=1` 下直接 panic。实测：DEFAULT 档`registered=48 capacity=192 dropped=0`，server 档（128 个桶锁）`registered=141 capacity=192 dropped=0`，余量 51。

**仍然没有的两样**（写在[net/network-lock-contract.md](../../net/network-lock-contract.md) 的迁移检查清单末尾）："当前 CPU 是否持有期望的 socket 锁"这个**正向**查询原语（探针只回答"这一把取得合不合规"，不回答"我此刻持有什么"），以及 lane claim 一侧的探针。

**证据的强度与覆盖范围**：两组注入测试覆盖了关键分支（桶锁取在 socket 锁之下触发 panic；地址乱序触发计数但不 abort）。在真实负载侧，4 CPU + 4 lane 下 20 次引导、797 次实际握手全程 `violations=0 sites=0`，外加单 CPU 的 120 次握手。ASAN 压力测试与全量端到端回归由独立测试编排执行。

## 三、隔离与多租户

### 已达成

cgroup v1/v2 是真的，且在热路径上强制：`cg_mem_charge()` 在缺页路径有十四个计费点，`cg_cpu_account()` 在每次上下文切换记账。`cpuset` 通过掩码求交生效（`sched_task_cpu_mask()`）。这比很多「有 cgroup 目录」的项目实在。

### 仍缺

1. **8 个 namespace 已有 3 个是真的**：mount、PID（`smoke-pidns`）与 user（`smoke-userns`）。其余 5 个（net、cgroup、time、uts、ipc）仍由 `unshare()` 显式返回 `-EINVAL`，这一点是干净的（`sys_namespace.c:4-7` 明确写了边界）。
2. ~~`pivot_root` 返回 `-EPERM`~~ —— **已补齐**。前置件确实就是先把 root/cwd 从路径字符串换成真实引用：现在 `mount_t` 带 `mnt_parent` / `mnt_mp` / `mnt_child` 真实挂载树，`proc_fs_context_t` 带`root_mnt` / `root_vn` / `cwd_vn` 三个引用，`pivot_root` 按 Linux 顺序校验后把旧 root 的 mount 摘出命名空间并标记 `VFS_MOUNT_DETACHED`——旧根此后不可按路径访问，只有 pivot 前打开的 fd 还能读到。`umount2` 的 `MNT_FORCE` / `MNT_DETACH` 也真正转发，busy 判定基于 mount 上的引用计数。详见 `docs/fs/vfs-edge-semantics.md` §9.4，门禁 `smoke-pivot-root`。**仍未覆盖**的是共享子树传播（`MS_SHARED` / `MS_PRIVATE` / `MS_SLAVE`），所以 mount 传播语义对容器编排仍然不完整——`pivot_root` 本身可用，但"pivot 之后再让子 mount 传播出去"这条链路还没有。
3. **userns 已补齐**（`kernel/proc/userns.c`：`uid_map` / `gid_map` / `setgroups`、全局↔命名空间 id 翻译、`setns` / `listns`、`/proc/<pid>/ns/user`、按命名空间作用域化的能力判定）。剩下的缺口是无 `nsproxy`（`setns()` 一次只能切一种命名空间），以及 capabilities 仍是 15 个子集。
4. 无容器运行时（lxc/runc/nspawn/crun/podman 均无），`packages/world/`里没有 server world。
5. cgroup 缺 `pids` / `io` / `freeze` 控制器；`cpu.shares` 存了但调度器从不读取。
6. **全局 OOM killer 的评分只看 `oom_score_adj`，不看 RSS**，因此不会可靠地选中最大占用者（cgroup 局部路径倒是用了 RSS）。

结论：**当前形态仍无法承载多租户**。PID ns + userns + `pivot_root` 三件套现已齐备，剩下的门槛是另外几项：没有容器运行时、没有 `nsproxy`、 capabilities 只有 15 个子集、mount 共享子树传播未实现，以及上面第 6 条的 OOM 评分。

## 四、进程与调度

### 首先：抢占与 RT 隔离仍需运行验收

内核已支持 `CONFIG_KERNEL_PREEMPT`，硬件 IRQ 返回时可消费调度请求。系统实现 `RLIMIT_RTPRIO` 软硬上限与 `CAP_SYS_NICE` 授权，并对每个 CPU 的 RT 队列施加一秒周期、95% 运行预算。耗尽预算后跳过 RT 选择，普通任务可以竞争剩余时间并在下一周期恢复。计量由 tick 和上下文切换路径完成，`rt_throttles` 与 `kernel_preempts` 接入 perf。

`sched_stress` 已包含同 CPU FIFO 失控时的普通任务进展、RT 预算恢复和权限边界断言。当前仍缺 IRQ 线程化、长 syscall 的有界延迟保证、可调 RT sysctl 与真机硬件 watchdog。限额预算并不解决长时间屏蔽中断或硬件锁死问题。

### 其余条目

- `SCHED_FIFO` 的预算恢复已通过同 CPU 回归，长内核路径 deadline 尚待运行验收（见上）。
- **[2026-10-06 更正] 本条描述的全局锁已不在当前树上。** `proc_lock` 已由并入本分支基线的锁拆分轮删除（`65bd609eb`：全树 0 处获取点、定义已删；调度状态改由 per-task `park_lock` 保护，runq 成员关系仍归 per-CPU `runq_lock`，任务表迭代/ OOM 扫描/聚合统计等真正的全局残余改用新 `tasklist_lock`，`kernel/proc/proc.c:56`；设计、验收标准与门禁演进见[roadmap/lock-serialization-split.md](../../roadmap/lock-serialization-split.md)）。下面保留的实测与分析因此只是**那把已删除的锁的历史**，「成因未定」的问题对它失效。替代锁的竞争面尚未收口：`park_lock` 已在编码阶段通过静态汇总 sink 注册进`/proc/a20/lock_contention`，但尚未采样；锁拆分设计规范 [roadmap/lock-serialization-split.md](../../roadmap/lock-serialization-split.md) 列出了使 proc 侧对比可解释还需做的事——本文件因此不声称「热点已消除」，只声称「该锁已删除」。原文如下（其中 `sched.c` 的行号是产生这些结论的树上的，与当期文件对不上，引用时按函数名定位）：**`proc_lock` 是当时最大的压倒性热点**：4 核实测 2528 次竞争 / 951 万自旋， 8 核 33335 次 / 1619 万自旋，多轮优化后仍 12–20K。根因是整个任务表只有一把全局自旋锁（定义在当时的 `kernel/proc/proc.c:43`），`sched.c` 里有 30 处取锁点。归因标签要当心：实测最大的一行是 `proc_sched_safe_point+0x42`，但`proc_sched_safe_point()`（当时的 `sched.c:1028-1036`，当期树在`sched.c:1046-1053`）只读一个 per-CPU 的`need_resched`，它自己不取 `proc_lock`；那一行其实是内联进去的`proc_yield()`，与 lwIP 那次 `net_vfile_read+0xf6` 是同一个"返回地址跳过一帧"的假象。`proc_yield()`（`sched.c:1988-1999`）同样不直接取锁：它调`proc_make_ready()`（状态转移）与 `sched()`（切换发布），二者才按`kernel/include/proc/proc.h:135-142` 的契约去取全局 `proc_lock`。**所以争用实际落在上下文切换/状态转移路径上**，与 `kernel/include/proc/proc.h:135-142` 描述的"切换发布要取 proc_lock"一致。这里曾断言成因是"全局锁被跨着一段 TLB 收敛等待持有"，**该断言已被实测推翻**。代码事实仍然成立：`context_switch_locked()` 在持有 `proc_lock` 的临界区内调用 `mm_context_enter(next->mm, cpu)`（`sched.c:1855`；`proc_lock` 由调用方取，在 `sched.c:1835` 放），而 `mm_context_enter()`（`mm/vm.c:120-151`）内部是一个`for(;;)`：只要`mm->tlb_cpu_generation[cpu] != mm->tlb_generation` 就反复`arch_tlb_flush_asid_local()`；`tlb_generation` 会被`mm_tlb_shootdown_page()`（`vm.c:357`）持续累加。但它并不是历史争用的主要成因。为此在 `mm_context_enter()` 内加入三个计数器（`mm_context_enters` / `mm_tlb_converge_waits` / `mm_tlb_converge_flushes`，经 `/proc/a20/perf` 导出），在 4 核 `smoke-smp-lock-contention` 下实测： 136 次进入、15 次至少刷新过一次、共 15 次本地 ASID 刷新，即`flushes - waits == 0`，不存在重复收敛。即便按每次刷新 1µs 的悲观估计， 15 次也只有 15µs 量级，与同一次运行中 `proc: 1517 4051609`（405 万次自旋，按每次 10ns 估约 40ms 聚合）相差三个数量级，无法解释该量级。所以"持锁时间被 TLB 收敛拉长"是错的：先前"高频短临界区"的猜测并未被推翻，反而与实测相符。但**"高频短临界区"同样不成立**，为此又加了"单次 acquire 最大自旋数"度量（输出里的 `max=`，锁级与调用点级都有），4 核实测`proc: 2802 5453081 max=519208`：均值只有 1946，**单次 acquire 最高自旋 519,208 次**（按 10ns/次估约 5ms）。均值不高却被少数几次拉爆，说明这不是稳态高频争用，而是少数几次长持有；`lwip: 4 833397 max=472365` 同形。调用点最差的两个是 `proc_park_commit`（20 次 acquire，max 519208）与`sys_wait4`（6 次，max 438789），**次数极少而单次极长**，与 `proc_sched_tick`（1424 次，max 180652）这种高频但单次不长的形态相反。"park 持锁跨越睡眠"这一猜想已被核对推翻：`proc_park_commit`（`park.c:250-293`）取的是 `task->park_lock`，在 `:275` 先释放，再到 `:283`调 `sched()`，全程没有跨切换持有 `proc_lock`。仍缺的是持锁方一侧的度量：自旋数是等待方计的，只能说明"锁被持有了这么久"，区分不了"持有者在干活"与"持有者在临界区里被抢占"。这条已尝试并失败，结论是**当前验证环境下拿不到可信的持锁时长**。做法是给 riscv64/aarch64/x86_64 补 `arch_read_cycle()`（riscv32 早已有同名封装，其余架构返回 0 作"无计数器"哨兵），在取锁时打时间戳、放锁时收口并按 `owner_ra` 归到调用点。（注意第三条障碍：`owner_ra` 现在只在 `CONFIG_DEBUG_LOCKS=1` 下由真正争用的 acquire 写入，见下文；即便前两条障碍不存在，默认构建里也拿不到它。）两个实测障碍，都不是代码能绕开的：
   1. QEMU TCG 的 `rdcycle` 不保证单调，它由宿主时间派生，多 vCPU 线程之间不一致。直接相减会下溢成 `18446744009993433250`（≈1.8e19）；加"单调性检查"之后不再下溢，但仍得到 `holdmax=31958042598`（≈32s）这种对微秒级临界区完全不可能的数值。**在真机上（`-icount` 或真实 PMU）才可能可信**。
   2. 因此任何在 QEMU 上读出的"持锁周期数"都**不能**当作持锁时长写进结论：宁可缺数据，也不能让一个数看起来像测量结果。换言之，**"持锁方在干活还是被抢占"这个问题在本环境的门禁下仍然没有答案**，下一步要么上真机（VisionFive 2 / LS2K1000），要么改用与时间无关的判据（例如统计"持锁期间该 CPU 是否发生过调度切换"）。另需注意：这些调用点标签与先前 `proc_sched_safe_point` 一样受"返回地址跳过被内联帧"限制（`sched()` 是内联的），符号名未必就是真正取锁的那个函数。`sched.c:1790` 的注释自己写着 *"mm_context_enter() only uses atomics"*：它按设计不需要 `proc_lock` 的一致性，却仍然被罩在临界区里。但不能简单把它前移：地址空间切换必须与 `proc_set_current()` 之间的中断窗口保持一致，否则中断处理会在"新 mm + 旧 task"的错配状态下运行。这属于上下文切换路径的定序设计，需要连带审阅 `sched.c:1880` 那条"one lock per switch" 的注释所描述的观察窗口保证。

  这段循环在本平台确实会执行，不是死代码，上面的计数器已经直接证明： 136 次进入、其中 15 次真的刷新过 ASID。平台相关性依然要交代清楚：QEMU riscv64 启动日志为 `[MM] RISC-V ASID mask=0xffff bits=16`（`riscv64/platform/asid.c:36`），所以 `mm->arch_asid` 非零，`vm.c:127` 的条件成立；arch.h 的通用默认是`ARCH_MM_CONTEXT_ALLOC() 0U`，只有 riscv64 覆写为 `riscv64_asid_alloc()`。**在 `arch_asid == 0` 的架构上这三个计数器恒为 0，引用本节数字时不能跨平台套用**。

  但也不能简单前移：`context_switch()`（`sched.c:1894`）那条调用点是本函数自己取锁，可以改成"先关中断 → mm_context_enter → 再取锁发布"；可是`sched()` 那条进入时 `proc_lock` 已经被更早取走了（取锁在 `sched.c:1963`，进入 `context_switch_locked()` 在 `sched.c:1950`，见 :1944 注释，刻意为了"每次切换只取一次锁"）。在那里要先放锁才能做 mm 切换，而放锁之后`next` 可能被别的 CPU 抢走，必须靠 `dispatching` 引用计数兜住并重新校验。这一点已核对过 `sched_runq_unpick_locked()`（`sched.c:1686`）：它的拒绝条件只在 `!t->dispatching` 时成立，也就是说一个仍处于 `dispatching` 的任务是允许被别的 CPU unpick 的（随后清掉 `dispatching` 与 `owner_cpu`）。

  这已经不是机械前移，而是要重新设计 `sched()` 的取锁时序，因此未作机械调整。这里必须把两件事分开：**"在全局自旋锁里跑一个理论上无界的 `for(;;)`"仍然是应当消除的潜在隐患**，有并发 PTE 修改时它没有次数上界；**但它不是实测的争用成因**。等待方一侧的度量（`max=`）做完之后，"成因"这个问题的答案既不是 rdcycle 封装、也不是继续泛读代码，两条结构性事实直接把它定死：(1) 全树 69 处 `proc_lock` 获取**全部**走 `spin_lock_irqsave`，**没有一处**用关中断之外的 `spin_lock`，所以持锁期间不可能被时钟中断抢占；(2) 持 `proc_lock` 的临界区里**没有任何 `sched()`/`proc_yield()`**（唯一命中是注释），所以也不可能主动让出。既然"被抢占"被排除，剩下只能是**持有者真的在临界区里长时间执行**，而 `max=` 指出的正是"次数极少、单次极长"。到此为止是站得住的。

  但"因此成因就是持锁临界区里的全系统遍历"这一步不成立，已被排除。那样推出的 4 处候选（`kernel/mm/mmap.c:39`、`proc.c:200`、`kernel/proc/loadavg.c:53`、`cg_mem.c:121`）经核对在实测负载下基本不会执行：`net_stress_test` 的`read()`/`write()` 是套接字调用，够不到 `mm_sync_shared_dirty_for_vnode()`；`proc_get_vm_stats()` 的唯一调用点是 `procfs_render.c:435` 的 `PF_MEMINFO`，而门禁只 cat `/proc/a20/perf` 与 `lock_contention`，不读 meminfo。

  观测窗口确有缺陷，且已修好：锁计数器自启动起累计、没有 reset 入口，所以此前"压力测试下的争用"解读建立在一个未分离的窗口上。修法不需要改内核，门禁改为在压力前后各读一次 `/proc/a20/lock_contention` 并求差（`proc_lock` 注册在 `proc.c:365`、打印顺序在 `lock_counters_format()` 里按注册序，故每块的 `^proc: ` 行就是天然分隔符）。**修完后的实测直接否掉了"极值发生在引导期"这个猜测**（两次独立复现）：引导期 2544 次争用/227 万自旋（均值 891），压力期仅 103 次争用/126 万自旋（均值 12202）；另一轮 1786/284 万（均值 1590）对 62/80 万（均值 12867）。即压力期每次争用比引导期重 8–14 倍，超长持有确实发生在压力期。

  "全系统遍历"这一类成因到此可以彻底否掉（不只是"候选点冷"）：全树`proc_first_task_locked()` 的调用点只在 `kernel/mm/`（`oom.c`、`cg_mem.c`、`mmap.c`）与 `kernel/proc/`、`kernel/fs/`（`procfs.c`、`cgroupfs.c`）里，`kernel/net/` 零命中，而压力负载是纯网络，这些点根本不在调用链上。把两个窗口的逐调用点自旋差算出来后，压力期的头部等待者是`proc_clone_impl+0x64`（163315 自旋，仅 2 次争用且 `max` 等于总和，即**单次持有约 1.6ms**）、`sys_wait4+0x36`（70750）、`proc_task_first_entry+0x30`（92972）；而引导期头部是`proc_sched_tick+0x30`（1576889），这正是此前累计值被引导期主导的来源。

  但需明确其观测限制：这些标签是**等待方**（发起 acquire 并自旋的一方），不是持有方。`proc_clone_impl` 自旋 16 万次只说明"有别人长时间持锁"，本身不能说明是谁。此前这里写的是"等待方数据在原理上无法指认持有者"，这句话**只对了一半**，且该修正本身也需要随实现更新：`spinlock_t.owner_ra`（`lock.h` 的`spinlock` 结构）在 acquire 时写入持有者 RA，到 `spin_unlock()` 才清，而现成的 `[LOCK-STALL]` 诊断本来就同时打印 `owner_ra` 与 `waiter_ra`。但这条路径**只在 `CONFIG_DEBUG_LOCKS=1` 的调试构建里存在**，且只有真正发生争用的 acquire 才写：默认构建把 `owner`/`owner_ra` 留空，因为这两个字段只服务于 stall 诊断与 `owner == cur` 自死锁判定，而把它们写在全部~920 个 acquire 点上会给无争用快路径增加一次存储。**因此在默认构建上，持有者身份并不是"对等待方始终可见"**——要靠 `owner_ra` 归因持有方，必须开 `CONFIG_DEBUG_LOCKS` 重编译。引用本文时不要再按行号定位`lock.h`：相关字段与诊断的行号已随上述改动漂移。

  在不依赖硬件时钟的方案下，通过采样估算持有方的方式被证明存在偏差。准确结论为：长持有成因仍待真机环境采样定位，当前仅收窄到压力期 fork/clone 与 wait-for-child 路径存在偶发毫秒级持有。
- `pid_max` 默认 32768，**与 Linux 默认值一致，且可通过`/proc/sys/kernel/pid_max` 运行时调整**（`procfs.c` 的 sysctl 写路径已接线）。因此它不是缺陷；确有需要的部署自行调高即可。（本文件早期版本把默认值列为待修项，是错的：默认值本身与 Linux 相同，上调反而会造成与 Linux 的行为差异。）
- `MAX_PROCS`（consts.h，MCU 32 / 托管构建 4096）是**死常量**：除自身`#define` 外全树零引用，任务结构是 `kcalloc` 动态分配的。它会误导读者以为存在 4096 的进程上限，应删除。
- **`RLIMIT_AS` / `RLIMIT_NPROC` 已实现并强制生效**（防止单进程无节制耗尽系统内存）。
- **单核门禁不再一统天下（2026-10 起）**：此前所有 `smoke-*` 门禁硬编码`-smp 1`，单核通过不证明 SMP 正确性。现在 vfs-stress 工作负载有了`smoke-vfs-stress-smp2`（NR_CPUS=2，已接入 CI smoke job）与`smoke-vfs-stress-smp8`（NR_CPUS=8，本地资源门禁 `-c 8`）两个真多核变体（与既有的 `smoke-mm-fork-exec-race` 同形），其余门禁仍是单核——把整套门禁矩阵 NR_CPUS 化仍是待办。
- 全部性能数据来自 QEMU TCG 模拟器，无真机基准。

## 五、可观测性

可观测性接口已全面向真实测量对齐（参见 [hardening.md](../../security/hardening.md) 的数据一致性原则）：`/proc/<pid>/io`、`/proc/loadavg`、`/proc/pressure`、`getrusage`、`TCP_CONGESTION` 现均为真实值。

`TCP_CONGESTION` 这一条在 `0d42ca758` 之后才真正成立：此前它接受 `"cubic"` 并返回成功，而本树**没有** CUBIC，`getsockopt` 也不回真实算法名。现在未知名返回`-ENOPROTOOPT`，回读的是真正生效的算法。同一原则下，`SO_SNDBUF` / `SO_RCVBUF`也从"接受然后忽略"变成真的生效值（`aacce4dcc`）；而**驱动能力位里那两个校验和卸载位刻意永不置位**（`driver_class.h:171-172`），因为 vendored 的 lwIP 2.2.2 没有承载该握手的 flag，置位会让 lwIP 去验设备没算的校验和——`/proc/net/stats` 里那两行因此**恒为 `off`，且这是设计结果不是未完成项**。

仍然缺失或不可用：

- 无硬件 PMU：`perf_event_open` 只有 software event，无 mmap ring buffer（只能 read），无 group leader → `perf top` 类采样不可用。
- 无 ftrace / kprobes / tracepoints。`bpf(2)` 是真的 eBPF 解释器，但只有 1 个扩展点、所有 map 命令返回 `-EOPNOTSUPP`、无 kprobe/perf 挂载类型，**不能用于可观测性**。
- 无 audit 子系统、无 syscall 审计、无文件完整性度量、无安全启动/dm-verity。
- 无日志外发（syslog/journald 集成）。
- seccomp 机制可用但**树内无任何服务安装 filter**。

## 六、可靠性

- panic 是关机不是重启（`panic.c:89` 调 `firmware_shutdown()`），失败则 `arch_halt()` 死循环。
- 无跨 CPU stop IPI（`panic.c:19-21` 自述）：其他核继续跑到自己 panic，输出被丢弃。
- panic 文本不进 klog 环（直接调 `printf`/`uart_puts`），控制台卡死时 dump 全丢。
- RV64 QEMU virt 已有 kexec handoff；其他架构和真机仍无执行后端，且没有 kdump。
- 无 watchdog（仅 STM32 MCU 的 IWDG），无 softlockup/hung-task 检测→ 内核挂死即永久挂死。
- **无 A/B 分区、无 dm-verity、无自动回滚**。热切换失败后的恢复仍依赖旧启动介质。
- 无节点级自愈：没有 fencing、quorum，也没有监管 `svcmgr` 自身的东西，它挂了所有服务变孤儿。

## 七、真机与虚拟化

- **PCI bridge 遍历已实现（覆盖有限）**：`pci_scan_current()` 现改为 worklist 遍历，读 header type 1 的 secondary/subordinate bus 寄存器并递归跟进，带 `visited[]` 防环。修复前 riscv64 的 `(0,1)` 范围只看得到 bus 0。**但自动化证据只有回归守护**：`smoke-pci-bridge` 在 x86_64 上跑，而 x86_64 板级传入 `0,255`、loongarch64 传入 `0,127`，旧线性扫描本就能覆盖这些 bus，所以该门禁在修复前后同样通过。它锁住的是不回归，不是修复本身。真正体现价值的是 riscv64 `(0,1)` 与 virtualbox-aarch64（固件分配范围），本 QEMU 构建无法驱动这两条路径（riscv64 virt 无 PCIe controller）。
- **MSI-X 已实现（仅 x86_64 真实投递）**：`kernel/drivers/bus/pci_msix.c`提供与协议无关的 MSI-X 层，`pci_bus.c` 把 virtio transport 接上， e1000e 与 virtio-blk 已实测通过 `smoke-msix-x86_64`。能力表位置解析同时支持 Vector Control（PCIe 编码，BIR `3:1` + 偏移 `31:12`）与 Message Address Lower（pre-PCIe 编码，BIR `2:0` + 偏移 `31:3`）—— `-kernel` 引导没有固件写前者，必须读后者。**残留**：只有 x86_64 实现了`arch_msix_message_address()`/`arch_msix_vector_setup()`，其余架构干净拒绝并退回 INTx/轮询；x86_64 的 per-CPU 目的与 IRQ 亲和性机制已落地（默认目标仍是 boot CPU 的 `0xD0..0xF0`，见 §七 的 PCI MSI-X 条），但没有 per-function/per-queue 的细粒度接口与 cmdline 亲和性策略； e1000e 只验证到表被正确解析并 arm，网卡无流量故未实测投递（virtio-blk 一路是端到端的）。
- **INTx 路由不再硬编码，但仍未在真机上执行过**（2026-10-07 改）：原先`arch_pci_intx_irq()` 只认 host bridge `0x29c08086`，否则 `return -1`。现在 `kernel/arch/x86_64/platform/acpi_dsdt.c` 实现 DSDT 子集解释器读`_PRT`（**失败即关闭**：不认识的 opcode、走不通的包、表里没有的设备一律返回"无 GSI"让驱动留在轮询，绝不猜），q35 swizzle 只作为唯一已知、无固件依赖的回退；boot log 打印 `(ACPI _PRT)` 或 `(q35 swizzle)` 标明是谁答的。**残留（全部写死在文档里）**：(1) `_PRT` 只答 bus 0，次级总线需 bridge 树下钻，**未实现**；(2) i440fx 的 PIRQ link 寄存器只读不写；(3) QEMU 的 DSDT 极简，`_PRT` 求值器**从未真正执行过**（q35 门禁实测走的是 swizzle 分支），真机固件上一次都没跑过。详见 [platforms/x86_64-pc.md](../../platforms/x86_64-pc.md)。
- **ECAM 基址不再只靠编译期常量**（2026-10-07 改）：x86_64 各板从 ACPI MCFG 读（`firmware_acpi_mcfg_base()`）；无 ECAM 的机器（i440fx）经`arch_pci_config_read32` 的 `0xCF8`/`0xCFC` legacy 回退枚举——`pci_bus.c`曾自己算 ECAM 地址直接读，使这条回退成了死代码，i440fx 上枚举出 128 个`id=0000:0000` 幽灵设备；已修并由 `make smoke-pci-i440fx` 按设备 ID（非槽位）断言。残留：virtualbox-aarch64 仍是旧板级固件范围路径。
- **ACPI 已有 DSDT 子集解释器与 S5 关机**（2026-10-07 改，原条目"无 DSDT/AML 解释器 → 电源管理在架构上就不可能"已失效）：RSDP + MADT（含 IOAPIC 条目，`IOAPIC_BASE` 硬编码已删）+ FADT（PM1a 块地址/宽度/睡眠状态）+ DSDT 子集（`acpi_dsdt.c`）。`pc_poweroff()` 改走 `firmware_acpi_poweroff()`：先写规范的 `SLP_TYP=5|SLP_EN`，短暂停顿后再写 QEMU 可识别的 `0x2000`——QEMU `acpi_pm_cnt_write()` 只对 sleep type 0 关机、5 落空，而 0 在真机上是保留值，双写是模拟器怪癖，代码内已注明。**残留**：S5 与 `_PRT` 都只在 QEMU 上验证，解释器从未在真实固件上执行。
- RTC 只有 x86_64：`cmos-rtc.a20drv`（MC146818，Early DriverStore）读 CMOS 并经 `timekeeping_wallclock_set_hw()` 替换墙钟；因为它是模块，替换只能发生在 `driver_manager_early_init()` 之后，`timekeeping_init()` 仍从编译期常量 `A20_BUILD_UNIX_TIME` 起步并打一行日志。门禁 `smoke-rtc-cmos` 已实跑 PASS（`-rtc base=utc`，宿主侧复核来客墙钟；日志`.kernel-build/smoke/rtc-cmos-x86_64.log`，来客采用 epoch 1791231484，与编译期种子 1791230264 不同，说明读到的是硬件）。**`smoke-rtc-cmos-fallback` 尚未执行** —— CMOS 落在可接受窗口外时回退到编译期种子这条分支目前没有运行证据。该驱动只读、无 IRQ、不做时区换算，century 寄存器不可信时按 20xx 处理。其余架构仍无真 RTC，墙钟一律从编译期常量起步。
- 无 paravirt clock：KVM 检测只用于决定是否信任 TSC，无 kvm-clock 兜底 → KVM 下 guest 存在时钟漂移风险。
- 无 virtio-fs/DAX（**服务器存储共享路径完全缺失**）、virtio-balloon。（virtio-rng 与 virtio-console 本分支已补上，见`docs/status/current/drivers-implementation-status.md`。）
- 好的一面：RISC-V IOMMU 是 755 行真实现（fail-closed）；x86_64 TSC 校准完整（CPUID 0x15/0x16 + PIT + invariant-TSC）；idle 路径是真实架构停机（`sti;hlt` / `wfi`）而非忙等。

## 七点五、内核核心可伸缩性演进

以下条目已在本分支落地（各提交含完整论证与验证入口；运行门禁于当前提交复验：`smoke-vfs-stress`、`smoke-abi-linux`、`smoke-mm-stress`、`smoke-vfs-stress-smp2`、`smoke-vfs-stress-smp8` 均 PASS）：

- **vfile 全局表锁分片**（`kernel/fs/file.c`）：fd 解析热路径原来在`g_file_lock` 单锁下串行（server-readiness 早期版本未把它计入热点排行，是观测盲区——单核门禁下它永远显示 0）。现按 gfd 哈希分 128 桶锁 +独立分配锁；全部桶锁登记进 `/proc/a20/lock_contention`（LOCK_COUNTERS_MAX 64→192），可测而非假设干净。
- **per-vnode 缓冲写锁**（`vnode_t.write_lock`）：替代 64 桶全局写互斥，两个哈希冲突的无关文件不再互相阻塞。
- **mount 表稳定指针**：umount 搬移内联数组导致 `vnode->mnt` 指向错误 mount 的正确性 bug 已修（堆分配 + 命名空间墓园）；注意 §三 的`pivot_root` 前置件（root/cwd 路径字符串 → mount 引用）仍是独立待办，本修复只消除了指针失真，没有引入 mount 树。
- **EventQ 反向索引 256 桶分锁**、**路径查找 errno per-task 化**（`vfs_lookup_errno()`）、**slab per-CPU 对象数组**、**timekeeping 读路径 seqlock 化**。
- **kswapd 式后台回收**（`oom_kswapd_thread`，`/proc/a20/oom` 暴露`kswapd_*` 计数）：回收不再全部同步发生在分配最坏路径。
- **发布流水线接入 guest 门禁**：release.yml 新增 smoke job，Release 创建以 `smoke-abi-linux`/`smoke-vfs-stress`/`smoke-mm-stress` 通过为前提。
- **PCI MSI-X**：`kernel/drivers/bus/pci_msix.c` + 能力表位置双编码解析 + x86_64 LAPIC 向量/LVT 编程 + virtio transport 接入（`msix_prepare`/ `msix_arm`/`msix_teardown`）+ e1000e 接入；**x86_64 上已有 per-CPU 目的**：消息地址按目标 APIC ID 生成，目标不是当前 CPU 时经 IPI 在远端编程 LVT，驱动入口 `pci_msix_set_affinity()` / `pci_msix_set_all_affinity()`，运行期入口 `/proc/a20/irq_affinity`，默认仍是 boot CPU。门禁 `smoke-msix-x86_64`断言 `[VIRTIO-BLK] MSI-X delivery on vector 208`，该行由中断处理程序在首次消息中断时打印，且已做反向验证（把消息地址改回错误形式，门禁只缺这一条而失败）；同一门禁以 `-smp 2` 追加断言迁到 CPU1 后投递行带`cpu=1`。详见 `docs/drivers/guide/pci-and-virtio.md` 的「MSI-X」一节。**本次亲和性改动已由 `make smoke-msix-x86_64` 实跑验证**（PASS，日志 `.kernel-build/smoke/msix-x86_64.log`）：`-smp 2` 下投递行带`cpu=1` 的断言成立。仍未验证的是非 x86 平台。
- **server world 声明层**：`packages/world/server.world`（dropbear/chrony/ busybox syslogd+crond）+ overlay init + `server-riscv64` 实例；声明过 `check-instances` 门禁，端到端组装与 SSH 登录验证未做（见 world 头注），不声称可用。

仍属本文件记录且**未**在本分支处理的：lwIP 全局锁分片（推进到阶段 E，但`g_lwip_lock` 本身仍是一把全局锁，见 §八）、`proc_lock` 超长持有成因（**后续已被锁拆分轮以删除该锁的方式终结**，`65bd609eb`，见 §四 更正注记与 §八 的对应行——本行照录该分支当时的边界），其余 5 个 namespace（net/cgroup/time/uts/ipc）与 `nsproxy`、 ACPI `_PRT`、MSI-X 的非 x86 平台实现，以及 IRQ 亲和性的**策略层**（per-function / per-queue 的细粒度接口与 cmdline 亲和性策略；机制层已在 x86_64 落地，见上一条）。

其中 **conntrack/NAT 已在 `feat/net-strengthening` 落地**（见 §二 与 §八）， lwIP 全局锁分片推进到阶段 E（`g_lwip_lock` 本身仍是全局锁）。

## 七点六、双 ABI 模式与门禁覆盖审计

本节分析系统历史演进中修复的 ABI 与配置缺陷，以及此前门禁覆盖的盲区。重点针对默认发布配置（`ABI ?= both`，`Makefile:117`）的正确性以及配套门禁进行审计。

### 为什么这些缺陷能一路绿灯通过

三件事叠加，使得"默认配置"这个事实从来没有被任何门禁兑现过：

1. **唯一的 ABI 冒烟门禁构建的是 `ABI=linux`。** `instances/smoke-abi-linux.toml`里明写 `abi = "linux"`，它跑的是 Linux syscall 冒烟。Native ABI 那 21 条（`tools/smoke_cases.py` 中名字带 `native` 的用例，本次逐条导出确认是 21 条）**全部**是 `ARCH=riscv64` + `ABI=both`，与上面那条不重叠。
2. **两个覆盖生成器只读 Linux syscall 表。** `tools/xlator/gen_linux_syscall_coverage.py`的 `TABLE` 指向 `kernel/abi/linux/syscall_table.def`，`tools/gen_envelope_coverage.py`在 `:144` 用 `^LINUX_SYSCALL\(` 解析同一张表。两者对`A20_NATIVE_SYSCALL` / `A20_SYS_` 一次都没有提及（grep 命中 0）。Native 侧此前的`check-abi-boundary` 是 19 条单文件关键词存在性检查，没有一条跨表比对——所以"加一个 native 入口不需要写任何文档行"这件事，没有任何一道门会红。
3. **CI 从来没跑过任何一条 native 运行时门禁。** `git show HEAD:.github/workflows/ci.yml | grep -c native` → **0**。21 条 native 运行时门禁在主干上一次都没有执行过，本地绿与主干红之间不存在任何 native 差异。

这三条合起来解释了一个此前没人注意到的形状：**CI 是绿的，默认配置却是不工作的** ——不是"跑得少所以少见"，是那条唯一会执行的 ABI 门禁跑的是另一个 ABI。

### 1. `#ifdef CONFIG_ABI_*` 在默认构建里恒假，POSIX 定时器整段驱动消失

`Makefile:918` 只定义 `CONFIG_ABI_$(ABI)`，`Makefile:1087-1089` 在 `ABI=both` 时额外补 `CONFIG_ABI_NATIVE`。**`CONFIG_ABI_LINUX` 在默认构建里全树没有任何一处会定义它**，所以裸 `#ifdef CONFIG_ABI_LINUX` 恒假，整块 Linux 代码静默消失而不报错。

问题主要涉及两处调用点：`kernel/proc/timer_heap.c` 的 `posix_timer_tick()`与 `kernel/proc/sched.c:1023` 的 `posix_itimer_cpu_tick(cur)`（卫语句在 `:1021`）。

**后果一（历史调用链中断问题）**：要紧的是分清哪一侧死、哪一侧活。`timer_posix.c` 的文件头自述它是 "ABI-agnostic process subsystem"，整份文件没有 CONFIG 卫语句，默认构建一直在编；arm/删除一侧也是活的——`posix_timer_delete()`（`timer_posix.c:125`）与 `posix_timer_set_time()`（`:167`、`:194`）都会经`posix_timer_update_deadline()`（`:89`）调 `sched_set_posix_deadline()`（`kernel/proc/timer_heap.c:449`；它唯一的调用方是 `timer_posix.c:99`），所以`next_posix_scan`（`timer_heap.c:158`）**写得进去**。死的是过期一侧：`posix_timer_tick()`（`timer_posix.c:114`；重算 deadline 现由 Rust 的 `a20_posix_timer_tick()` 承担）此前唯一的调用点（`timer_heap.c:570-571`）被恒假卫语句挡住，于是过期没人扫描——信号不发、`expire_tick` 不推进、`next_posix_scan` 不重算。一旦 arm 过的 deadline 过去，`proc_next_timer_interval()`（`timer_heap.c:213`）每次重装都命中`if (next <= now) return SCHED_MIN_TIMER_INTERVAL;`（`:226-227`），`SCHED_MIN_TIMER_INTERVAL`（`:205`，`TICKS_PER_SEC/10000`）成为每次重装的下限——**在默认构建里 arm 过一个 POSIX 定时器、它的 deadline 过去之后，定时器中断就停在这个下限上**，直到该定时器被删除或清零（那条 syscall 路径会重跑`posix_timer_update_deadline()` 把它写回 `SCHED_NO_DEADLINE`）；过期路径自己永远不会清它。恢复 `posix_timer_tick()` 等于恢复了清理方。

**后果二（活代码激活后的并发暴露）**：`g_posix_timers`（`kernel/proc/timer_posix.c:56`）与 `g_cpu_itimers`（`:67`）是无锁静态表；`ABI=linux` 下它们已经同时被定时器中断路径与 syscall 上下文改写。在修复卫语句后，`posix_itimer_set()`（Rust 实现`a20_posix_itimer_set()`，`kernel/proc/posix_timer_state.rs:490`）在为新 itimer 克隆任务引用失败、或表里没有空槽时返回 `-EAGAIN`，经`sys_setitimer()` 透出。需注意 Linux 规范中 `setitimer` 不会因表满返回 `EAGAIN`。

**armv7m 连带**：`Makefile:1177` 把 `KERNEL_SRC` 整体换成`components/trim.mk:8` 的 `TRIM_PROFILE_MCU_SOURCES`，那份表含`kernel/proc/timer_heap.c` 与 `kernel/proc/sched.c` 但**不含** `kernel/proc/timer_posix.c`，而它照样发 `-DCONFIG_ABI_BOTH`。守卫改宽之后`posix_timer_tick()` / `posix_itimer_cpu_tick()` 在 MCU 上会缺定义，因此在`kernel/mcu/mcu_stubs.c:55-56` 补了 stub（与同文件 `a20_timer_tick()` / `psi_tick()` 的处理同形——MCU 源集既无 `syscall/` 也无 `abi/linux/`，POSIX 定时器在那里根本无法被 arm）。

其余 15 处裸 `#ifdef CONFIG_ABI_NATIVE` 在 `ABI=both` 下已有定义，属于宏守卫书写规范问题，已统一规范化为完整判断。

**验证**：把新写的 `tools/gates.py` 与 `tools/gates.toml` 拷到 `git archive HEAD`出的干净副本上跑 `python3 tools/gates.py abi-config-guard`，退出码 1，点名 17 处；在当前树上跑 `make check-abi-config-guard` → PASS。`make ARCH=riscv64 BOARD=qemu-virt-riscv64 ABI=both BRINGUP=0 kernel-only`（`-Werror`）零警告通过。

**验证**：把新写的 `tools/gates.py` 与 `tools/gates.toml` 拷到 `git archive HEAD`出的干净副本上跑 `python3 tools/gates.py abi-config-guard`，退出码 1，点名 17 处；在当前树上跑 `make check-abi-config-guard` → PASS。`make ARCH=riscv64 BOARD=qemu-virt-riscv64 ABI=both BRINGUP=0 kernel-only`（`-Werror`）零警告通过。

### 2. Native ABI 的三份真源互不校验

同一份 HEAD 快照上跑 `python3 tools/gates.py native-abi-coverage`，退出码 1，输出（摘）：

```
5/142 entries in kernel/abi/native/syscall_table.def are named nowhere in
docs/native-abi/ (10 files): fs_serve, fs_block_io, monitor_query,
device_free_dma, device_get_info
16/142 numbers in kernel/include/abi/native/syscall_nr.h have no row in
docs/native-abi/03-handle.md ## 6. 完整 Syscall 列表: device_free_dma,
device_get_info, execve, fs_block_io, fs_serve, monitor_create,
monitor_query, pager_create, pager_supply_pages, pager_vmo_attach,
task_adopt, task_clone, task_mem_read, task_mem_write, vm_create_vmar,
vm_share_region
3/142 numbers ... are missing from user/liba20rt/a20_syscall.h:
fs_block_io, fs_serve, handle_poll
```

逐条独立核对过：`syscall_nr.h` 在 HEAD 登记 142 个；`03-handle.md` §6 当时只有**126 行**表格行，却在末尾写着"总计：142"；`user/liba20rt/a20_syscall.h` 只有**139** 个 `#define A20_SYS_`，缺的就是点名的三个。也就是说，**规范文档的"总计"是一个和真实编号表对不上的数字，而没有任何门在意**；而`A20_SYS_handle_poll` 在用户态镜像里不存在，任何 include 该头的程序都**按名字找不到那个 syscall**，这三条同样没人发现。

新增 `make check-native-abi-coverage`（宿主侧，并入 `check-doc-test-gates`、`make check` 与 CI `toolchain-gates` job）做四份真源的交叉：登记表`kernel/abi/native/syscall_table.def` ↔ 编号表 `kernel/include/abi/native/syscall_nr.h` ↔ 用户态镜像 `user/liba20rt/a20_syscall.h` ↔ `docs/native-abi/`。§6 现在 142 行、"总计 142"、三份表各 142 条；当前树上 `make check-native-abi-coverage` → PASS。

### 3. `vm_advise` 丢弃 advice 参数，纯提示会丢掉调用方的页

改动前 `sys_a20_vm_advise()`（`kernel/abi/native/sys_native_mm.c`）读 `A20_ARG(0)`与 `A20_ARG(1)`，**`A20_ARG(2)` 的 advice 从未被读**，无条件调用`mm_madvise_dontneed()`，然后返回 `A20_OK`。也就是说 `MADV_NORMAL`——语义是"预期正常访问"，什么也不该做——会把调用方的页真的丢掉，并报告成功。`MADV_DONTFORK` / `MADV_WIPEONFORK` / `MADV_REMOVE` 等同样被当成 DONTNEED。

现在 `sys_a20_vm_advise()` 把 advice 交给 `mm_madvise()`（`kernel/mm/madvise.c:219`）分派：DONTNEED/FREE 走丢页，DONTFORK/DOFORK/ WIPEONFORK/KEEPONFORK 走 VMA fork 标志位，`MADV_REMOVE` 返回 `-ENOSYS`（本内核唯一能换出的路径是 OOM 受害者自己的 reclaim，没有按区间换出可调，丢掉等于毁掉调用方被承诺还能读回的内容），一批"实现了但不作用于这条路径"的 advice 做覆盖校验后返回成功，**未知值返回 `-EINVAL`**——这样调用方不会把"没实现"误读成"已生效"。

**同一次改动里修掉的另一个溢出缺陷**：`mm_madvise_end()`（`kernel/mm/madvise.c:53`）现在显式拒绝 `end < addr` 的回绕区间。改动前 `end = (addr + len + PAGE_SIZE - 1) & ~…` 回绕后会落到 `addr` 之下，而下面 `for (va = addr; va < end;)` 的循环体**一次都不执行**，函数返回 0——一个溢出的区间得到的是"成功"，且什么都没碰。

**验证（部分）**：新增的用户态断言在 `user/tests/test_native_mm.c` 第 8、9 节（MADV_NORMAL 不丢页、四个 fork-policy advice 的往返、区间越界与回绕、未知 advice、`MADV_REMOVE`）。**但我实测 `make smoke-native-mm` 是红的，且红在新增断言之前**：输出停在 `NATIVE_MM: FAIL vm_map FILE failed`（第 7 节，file-backed mapping 那一段）。在 HEAD 的干净副本上跑同一条门禁，失败点完全相同。所以**这两批新断言目前没有被任何一次成功执行验证过**——本文件不把它们记为 PASS。

### 4. `handle_set_meta` 的五个 flag 是空实现，却返回 `A20_OK`

改动前 `sys_a20_handle_set_meta()` 对 `ATIME` / `MTIME` / `CTIME` / `TRUNCATE` / `ALLOCATE` 的处理是 `(void)val0; (void)val1;`，然后 `return A20_OK`。也就是说`handle_set_meta(h, A20_SET_META_TRUNCATE, 0, 0)` 会**报告成功并保留原长度**。

现在每个被 honored 的字段都走 VFS（`kernel/abi/native/sys_native_handle.c:288`起的 `vfs_ftruncate` / `vfs_futimens` / `vfs_fchmod` / `vfs_fchown`），而不是直接写 `vf->vnode->mode`——vnode cache 不是后端存储，而且只有 VFS 会检查调用者是否拥有该文件。保留位与未实现位在**任何字段被改动之前**就被拒绝（`kernel/abi/native/sys_native_handle.c:235-237`），`flags == 0` 同样拒绝；`CTIME`（`vfs_set_times()` 每次写都从时钟刷新 ctime，没有入口接受调用者给的 ctime）与 `ALLOCATE`（没有预分配内核可路由）返回 `A20_ERR_NOT_SUPPORTED`；只读挂载上三种写返回 `A20_ERR_ACCESS`。

**验证（部分）**：新增断言在 `user/tests/test_native_handle.c` 的`set_meta_fields()` 与 `xattr_rights_split()`。**实测 `make smoke-native-handle`是红的，红在新增断言之前**：输出停在 `dup ok` 之后（`handle_transfer_byte_move()`，第一个 transfer 用例），`part ok` / `ac ok` 都没有打印。在 HEAD 的干净副本上跑同一条门禁，日志逐行相同。所以**这两组新断言同样没有被成功执行验证过**。

### 5. xattr：只查 STAT 权限，且用户态 SDK 传的是结构体而不是线格式

两处独立的缺陷：

- **权限**：`xattr_common()` 此前对 set/get/list/remove 四种操作**一律**要求`A20_RIGHT_STAT`。set 与 remove 改的是对象元数据却不需要 `WRITE`。现在按操作分（`kernel/abi/native/sys_native_handle.c:375`）：set/remove 要 `WRITE`， get/list 要 `STAT`。`A20_OBJ_DIRECTORY` 的 rights 上限不含 `WRITE`，所以对目录句柄 set/remove 恒失败——这是 rights 代数自身的结果，不是新加的特殊判断。
- **线格式**：内核的 `sys_a20_handle_xattr_set()` 读的是`A20_ARG(0)=handle, ARG(1)=name, ARG(2)=value, ARG(3)=size`（四个平铺的字），而 `user/liba20rt/a20_handle.h` 的 `a20_hdl_xattr_set/get/list` 此前构造了一个`a20_xattr_args_t` 结构体、把 `&args` 塞进 `ARG(0)`——**把一个结构体指针当成句柄传**。`a20_hdl_xattr_remove` 在 SDK 里则**根本不存在**（内核侧 0x010B 有实现）。

SDK 三个 wrapper 改为传平铺参数，并补上 `a20_hdl_xattr_remove()`。

### 6. `ns_apply` 宣称三种不存在的隔离

改动前 `sys_a20_ns_apply()` 对四种 ns_type 都返回成功，并写入`target->ns_ctx.net_ifindex` / `pid_offset` / `dev_access_mask`。本次 grep 核实：这三个字段在整个 `kernel/` 与 `user/` 下**只有写入点，没有任何读取点**（`kernel/include/proc/proc.h:106-108` 的声明和`kernel/abi/native/sys_native_security.c` 的赋值之外零命中）。也就是说这三次写入不产生任何隔离效果，却报告 `A20_OK`。

现在非 `A20_NS_FILESYSTEM` 的类型返回 `A20_ERR_NOT_SUPPORTED`（`kernel/abi/native/sys_native_security.c:139`），即承认"没有实现"而不是宣称"已经隔离"。只有 filesystem 生效，而且只写 `root_path` 字符串——**不更新`root_vn` / `root_mnt`，不重置 cwd**，所以同一 task 上 `fs.root_path` 与`root_vn`/`root_mnt` 会指向不同的 root，直到下一次 chroot/pivot_root 同时覆盖二者。这条边界原样保留（走 `vfs_task_root_set()` 需要把路径解析成活的 `(mnt, vnode)` 对，拒绝应用一个解析不了的 root 是 `ns_apply` 目前没有的行为变更），并写进了`docs/native-abi/06-security.md` §7.2。

**另一处独立的截断**：`struct a20_namespace::root_path` 此前是 `char[256]`，而`task->fs.root_path` 是 `char[MAX_PATH_LEN]`，`MAX_PATH_LEN` 在托管构建下是 512 （`kernel/include/core/consts.h:51`；MCU 下 32/64）。`ns_create` 把调用者的 root 逐字快照进这个字段，于是**一条合法的路径会被静默截断**。现在两者同宽（`kernel/include/ipc/ipc.h:264`），并且 `ns_create` / `ns_apply` 两处的 `strncpy`都补了显式 NUL 终止——原先两处都只 `strncpy(dst, src, MAX_PATH_LEN - 1)` 而不写终止符。

### 7. 接进 CI 的第一条 native 运行时门禁，当前是红的

`.github/workflows/ci.yml` 的 `smoke` job 新增 `make smoke-native-contract`。选它是因为 `tools/smoke_cases.py` 里 21 条 native 用例全写死`ARCH=riscv64` + `ABI=both`，复用同一 dev-build 产物不会新增一次构建，代价是一次 20 s 的 QEMU 启动。

**门禁当前状态**：`make smoke-native-contract` 输出停在 `F:vmol-leak-vmo`（`user/tests/test_native_contract.c:466`），`smoke.py` 随后报 `missing ['vmol ok', 'dma ok']`。在主干干净副本上重跑具有相同表现，归因为主干既有的 VMO 引用计数缺陷。

该接线首次使此缺陷可见，在 `mm/` 的 VMO 释放路径闭环之前，CI 的 `smoke` job 会持续暴露该问题。

### 本节遗留、未核实的部分

- `smoke-native-handle` 与 `smoke-native-mm` 两条门禁为主干既有红灯，对应高级用户态断言尚未完成全量动态端到端闭环验证。
- 剩余 20 条 native 运行时门禁仍未进 CI。
- Native 运行时仍然**只有 riscv64 一个架构**；`tools/targets-native*.mk` 里的`native-<prog>-arch` 交叉编译目标 CI 一个都不调。
- `ABI=both` 的 POSIX 定时器路径在 SMP4 下的实际行为尚未进行大规模并发长跑验证。

## 八、阻塞项排序

按实际进展区分未决项与已完成项：先列仍然阻塞的关键项，已完成项归档于后表。

| 级别 | 阻塞项 | 理由 |
|---|---|---|
| P0 | lwIP 全局锁分片 | **已收窄 socket 侧与分发侧，但仍是网络最大未决瓶颈。** 阶段 C（`5ea06a786`）落「当前 lane」上下文 + memp 的 lane 索引骨架；阶段 D（`f6f327b96`）把协议输入搬出中断上下文、按 lane 分发；阶段 E 收窄 socket 侧锁。`g_lwip_lock` 本身至今仍是一把全局锁。热路径需依赖 socket 单一所有权避免全局 PCB 链表遍历，后续仍需给 `tcp_active`/`tcp_bound_pcbs`/`udp_pcbs` 确定 per-lane 所有权与死锁边界 |
| P0 | 多 lane 的 2% connect flake 仍未定位 | 测试样本扩展至 797 次实际执行且 0 失败，但根本原因尚未彻底定位并关闭。完整测试配方参见 [net/net-lanes.md](../../net/net-lanes.md) |
| P0 | `proc_lock` 超长持有的成因未定 | **只证伪了一半**。已证伪"被抢占"（成立）：全树 69 处 `proc_lock` 获取全部走 `spin_lock_irqsave`，无一处关中断之外；持锁临界区内无任何 `sched()`/`proc_yield()`。所以持有者确实在长时间执行。先前据"持锁临界区里做全系统遍历"推出的 4 处候选已被排除。`/proc/a20/{perf,lock_contention}` 现有 `reset` 写入口供窗口求差分析 |
| P1 | IPv6 地址路径的写入侧 | `RTM_NEWADDR` 与 `RTM_DELADDR` 都已实现（删除把槽位置 `IP6_ADDR_INVALID`，不存在返回 `EADDRNOTAVAIL`，事件为 `RTM_DELADDR`），入站 TCP、`/proc/net/tcp6` 和 IPv6 组通知已有。**边界**：管理写入不做 DAD（事件不代表 DAD 通过，也没有补发完成事件）；`ifa_prefixlen` 只校验不落地；ND6/SLAAC 学到的地址与生命周期转换不发通知（`LWIP_NETIF_EXT_STATUS_CALLBACK=0`） |
| P1 | 接收缓冲自动调优 | `SO_RCVBUF`/`SO_SNDBUF` 真正生效，窗口缩放已解除协议上限；**发送侧有界自动调优**（初始 32 KiB，耗尽时倍增到 `TCP_SND_BUF`，显式设置关闭）。接收侧默认已是 `TCP_WND`，没有可增长空间；`wnd_limit` 是显式 `SO_RCVBUF` 的软上限，`tcp_recved()` 不缩小已通告窗口。没有全局 `tcp_wmem`/`tcp_rmem` 或内存压力反馈，仍是设计边界 |
| P1 | 嵌入式档仍装不进 20 KiB | **账已全部纳入 profile 并按真实元素尺寸对账**：EMBEDDED 的 44 KiB 总预算由 socket 表、ring + drain、netif state、两个 `struct netif`、filter 表和 heap 求和断言；三个档位的池上限都对 `MEM_SIZE` 有求和断言（EMBEDDED 16,224/16,384，DEFAULT 771,192/1,048,576，SERVER 12,510,840/16,777,216）。DEFAULT 的 `MEM_SIZE` 因此从 512 KiB 调到 1 MiB。20 KiB 仍不足以容纳 8 个 socket + lwIP，嵌入式档诚实地声明 44 KiB |
| P1 | 扩大接收缓冲（pbuf 池 / 零拷贝收包） | DEFAULT 池上限 256 × 1600 B（约 406 KiB），SERVER 4096 × 1600 B（约 6.34 MiB）；`/proc/a20/netmem` 同时打印池上限与 heap `used/avail/err`，可区分池受限与堆受限。**零拷贝收包是设计边界**：设备 recv op 只支持拷入调用者缓冲，lane ring 槽位复用也不能安全包装成 custom pbuf；真正零拷贝需要改 `kernel/drivers` 接口 |
| P1 | ACPI `_PRT`（解释器已落，但只答 bus 0、且从未真正执行） | `acpi_dsdt.c` 已在 main（`7f38f2227`），失败即关闭、绝不猜 GSI；**残留三项**：次级总线的 bridge 树下钻未实现（桥后设备留轮询）、i440fx PIRQ link 只读不写、求值器在 QEMU 极简 DSDT 上走不到、真机固件上一次未跑。真机服务器的准入条件 |
| P1 | kdump 执行后端 + panic 改为重启 | 故障后能否自动恢复 |
| P2 | 硬件 watchdog + A/B 分区 + dm-verity | 无人值守与安全更新 |
| P2 | 硬件 PMU + ftrace/tracepoints | 生产环境可诊断性 |
| P2 | virtio-fs/DAX | 共享存储 |
| P2 | 真 RTC + paravirt clock | 真机时间正确性 |
| P3 | NUMA、热管理、C-states | 规模与能效 |

**已完成，不再是阻塞项**（列在这里是为了说明它们曾挡住什么，以及各自还剩什么残留）：

| 级别 | 阻塞项 | 理由 |
|---|---|---|
| P0 | 抢占与 RT 隔离验收 | 基线已有 `CONFIG_KERNEL_PREEMPT`；2026-10-13 编码阶段补入 RTPRIO / 能力检查与 per-CPU 一秒周期 95% RT 预算。同 CPU 普通任务进展与 RT 恢复断言已通过 `smoke-sched-stress`。IRQ 线程化、长内核路径有界延迟与真机 watchdog 仍缺 |
| ~~P0~~ | ~~`proc_lock` 超长持有的成因未定~~ | **该行描述的锁已不在当前树上**：`proc_lock` 已由锁拆分轮删除（`65bd609eb`，`kernel/proc/proc.c:45` 记载了它被什么取代，全树 0 处获取点）；调度状态归 per-task `park_lock`，任务表迭代/OOM/聚合统计等低频残余归新 `tasklist_lock`（`kernel/proc/proc.c:56`），设计见 [roadmap/lock-serialization-split.md](../../roadmap/lock-serialization-split.md)，"成因未定"对已删除的锁失效。**未收口、不声称热点已消除**：`park_lock` 已通过静态 sink 注册进 `/proc/a20/lock_contention`，锁拆分之后的 proc 侧竞争现已采样——4 vCPU、5 次，`mm_stress` 的争用主体落在 `park_lock`（中位 2270 次竞争获取）而 `tasklist` 只剩 7 次，`futex_stress` 仍把 `tasklist` 打到 430 次获取 / 550 万次自旋，设计与测试契约见 [roadmap/lock-serialization-split.md](../../roadmap/lock-serialization-split.md)；B1 基线未采集，所以拆分前后的同输入对照仍不可分辨。原文要点（历史）：已证伪"被抢占"——当时全树 69 处获取全部走 `spin_lock_irqsave`、持锁临界区内无 `sched()`/`proc_yield()`，持有者确实在长时间执行；"持锁临界区全系统遍历"成因说已撤回（4 处候选在实测负载下不执行）；观测窗口缺陷已修（`/proc/a20/{perf,lock_contention}` 的 `reset` 写入口） |
| ~~P0~~ | ~~收包内存模型~~ | **已修**（`feat/net-lanes`）：两级暂存内联化，`net_socket_t` 1.05 MiB → 30 KiB，`net_msg_t` 68 KiB → 1368 B，锁内每包 memset 65535 B → 200 B，并由 `_Static_assert` 钉住 |
| ~~P0~~ | ~~`g_net_lock` 分片~~ | **已完成**（`2f17a5ba8` + 阶段 E `7c7a4d7c8`）：`g_net_lock` 已删除，socket 表分片为桶锁，桶锁又被 `net_socket_t.lock` 取代，现在只管 slot 表。SERVER 档一个桶是 512 个 slot，所以阶段 E 之前一个 socket 的 `recv` 要和同桶另外 511 个互斥。**残留**：`2f17a5ba8` 自陈的三项运行期验证（引用计数不漏不重、`LOCK_COUNTERS_MAX` 注册预算、`-ENOTCONN` 窗口）已完成回归验证——引用计数账本在 4 CPU + 4 lane 下 20 次引导全部 `live=1 faults=0`，`LOCK_COUNTERS_MAX=192` 在 DEFAULT 档（48）与 server 档（141）均满足且 `dropped=0`；`-ENOTCONN` 窗口已按代码排查并收敛。原始 2% connect flake 另由 [NET-02](../gaps/net.md) 跟踪。ASAN 压力测试与全量回归作为独立验证阶段安排 |
| ~~P0~~ | ~~PID ns + userns + `pivot_root`~~ | **已完成**：`pivot_root`（`smoke-pivot-root`）、PID ns（`smoke-pidns`）、userns（`smoke-userns`）均已落地。残留：无 `nsproxy`、capabilities 仅 15 个子集、mount 共享子树传播未实现 |
| ~~P0~~ | ~~ext4 可写 journal + 崩溃注入测试~~ | **已完成**：运行时 metadata 写入走 JBD2 ordered commit，commit 指针按事务大小推进，数据 checksum 记在 descriptor tag 内（不再写进块尾污染 bitmap），挂载时以日志 `s_start` 为权威判据；`make smoke-ext4-journal` 做四点崩溃—重启往返并用 `e2fsck -fn` 双向把关，x86_64/riscv64/aarch64/ppc64le/loongarch64 五架构 5/5 PASS |
| ~~P0~~ | ~~net 锁一侧的运行期探针~~ | **已完成**（`2dd28758c` + `067d0eb96`）：per-CPU 持锁集合探针 `kernel/net/socket/net_lock_probe.c` 检查重复取锁、socket 锁地址乱序、第三把 socket 锁、桶锁取在 socket 锁之下、第二把桶锁；开关复用 `CONFIG_NET_LOCK_ASSERT` 并新增第三档 `=2`（只计数不 abort），`/proc/net/stats` 上 `net_lock:` 与 lwip 行并排。配套把 `lock_counters_register()` 超出 `LOCK_COUNTERS_MAX` 的静默丢弃变成可见计数 + `net_socket_registry_init()` 自核对。**残留**：仍然没有"当前 CPU 是否持有 socket X"的**正向**查询原语，lane claim 一侧也没有探针；七条上报分支里只有两条做过故意注入的对照；ASAN 与全量回归未跑 |
| ~~P1~~ | ~~conntrack + NAT~~ | **已实现**：五元组哈希表（按档位 32/256/1024）、NEW/ESTABLISHED、按状态超时、满表 LRU、SNAT/MASQUERADE（output）与 DNAT（input）、RFC 1624 增量校验和、ICMP echo 跟踪；规则方向与 hook 不匹配时返回 `EINVAL`。门禁：`smoke-netfilter`、`smoke-netfilter-nat`（DNAT/TCP）、`smoke-netfilter-snat`（出站 SNAT/TCP + 回程 + 阴性对照）、`smoke-ct-capacity`、`test-nat-rewrite`。**设计边界**：无 ALG；不跟踪 ICMP 差错（vendored lwIP 也丢弃入站差错，PMTU 不可用）；分片只翻首片；无 IPv6 NAT；无动态端口分配。详见 [net/conntrack-nat.md](../../net/conntrack-nat.md) |
| ~~P1~~ | ~~MSI-X~~ | **已完成（x86_64）**：能力解析 + LAPIC 编程 + virtio/e1000e 接入 + `smoke-msix-x86_64` 端到端投递断言。残留亲和性与非 x86 实现 |

### 第二轮（门禁与文档）落地的五条流

第一轮收的是「功能能不能跑」，这一轮收的是「有没有门禁真的跑过」。逐条对应上表里的哪句话：

| 流 | 落地 | 对应阻塞项 |
|---|---|---|
| `make check-format` 真正执行 | 装上 `clang-format` 23.1.2（pipx），新文件 `netfilter_nat.c` / `netfilter_internal.h` / `netnat_test.c` 过格式；**门禁本身仍红**，见下 | 新增，见文末 |
| conntrack LRU / 空闲超时门禁 | `make smoke-ct-capacity`（`faa8ca1b3`） | conntrack + NAT 行的 "LRU 与超时只有计数" 一句已失效，改写 |
| SNAT/MASQUERADE 可测性 | `test-nat-rewrite` 编译出货源码做改写/校验和对照；`smoke-netfilter-snat` 提供出站 SNAT 的端到端回程门禁 |
| `tools/cli/a20_derive.py` 的 hostfwd 拼写 | `tcp::5555-:5555` 补出 guest 地址与 `=on`（`32764f659`）。**原判据不成立**：QEMU 10.0.13 上 `-netdev user` 并不报 "Missing guest address"，裸短格式被拒的报错是 "Invalid parameter"；补齐前后两种拼写都真的绑上 5555。改动仍然保留，因为带地址的拼写是 QEMU 唯一在 `-netdev user` 与 `-nic user` 两种写法下都稳定的形状 | 无（本来不在表里） |
| 文档对齐 | 本节表重排 + `net/conntrack-nat.md` 补 ICMP / LRU / 超时边界与门禁表 + README 网络段 | — |

**`check-format` 为什么还是红的**：`.clang-format` 写的是 `BreakBeforeBraces: Attach`，树里写的是 Linux 风格，995 个在册源文件有 843 个被基线豁免（`tools/clang-format-baseline.txt`写着 "Only ever shrink"），剩下 57 个漂移文件属于别的流。改 `.clang-format` 只会更糟——实测改成 `Linux` 后漂移从 60 涨到 138。这一条要么全树重排（跨流），要么明确决定重新划基线，不是网络这条流能单方面定的，所以留在这里而不是被顺手抹平。

### server world 的实测状态

`packages/world/server.world` 此前标注为"从未执行过组装"。现已推进到**镜像能装出来、并且在 guest 内启动到 shell**：

```
make ARCH=riscv64 BOARD=qemu-virt-riscv64 \
     ALPINE_MIRROR_ROOT=https://dl-cdn.alpinelinux.org/alpine \
     image-world PKG_WORLD=server
```

22 个 Alpine 包（busybox、dropbear、chrony、ca-certificates 及依赖）全部装入 staging 并被 `mkfs.ext4` 打包；把这张镜像作为第二块 virtio-blk 盘挂上启动后，stage-2 init 正常 chroot，dropbear 打印主机密钥。**world 清单 → apk 求解 → overlay 装配 → mkfs → chroot** 整条链路成立。

两处环境相关的坑，都不是仓库逻辑问题：

- 默认 USTC 镜像源在本环境返回 **403**，需换官方源；`ALPINE_MIRROR_ROOT` 在`tools/mk/targets-rootfs.mk` 中是 `?=` 赋值，可从命令行覆盖。
- `mkfs.ext4 -d` 曾报 `do_write_internal: 权限不够 while opening "bbsuid"`。这是`busybox-suid` 的 `bbsuid` 需要 `mknod`、而 fakeroot 只伪造属主不提供 `CAP_MKNOD`所致（单独 `fakeroot chown 101:101` 正常，可确认不是 fakeroot 本身坏了）。已从 world 清单去掉 `busybox-suid` 绕开，见 `6a1ab8f8`；宿主上有 root 时该包可以放回。

#### 让这张镜像真的能被登进去，需要两件事

1. **镜像里必须有认证路径。** 镜像不含密码——密码一旦烤进镜像就等于永久泄露并随镜像复制扩散——所以只能烤公钥。新增 `SSH_PUBKEY`：

   ```
   make image-world PKG_WORLD=server SSH_PUBKEY=~/.ssh/id_ed25519.pub
   ```

   见 `6702613c`。不给 `SSH_PUBKEY` 时镜像不带任何 `authorized_keys`，dropbear 照常启动，但没人能登进去。

2. **内核命令行必须选 `a20.tcpmode=lwip`。** 默认的 `tcpmode=fast` 里 `listen()`直接丢掉已绑定的 pcb、从不把 listener 放进 lwIP，于是该端口在协议栈里根本不存在， slirp 发来的 SYN 被回 RST。这与认证无关，发生在认证之前。

`tcpmode=lwip` 确实修好了这一层：`/proc/net/status` 里 `tcp_listen=1`（真实 LISTEN pcb 存在），且 `smoke-net-accept` 的 lwip 那一趟 **PASS**（`TCP_ACCEPT_TEST: PASS port=12346`，`net_accept_staged=1`、`net_accept_queued=1`）。

#### 入站连接的 double free：已定位并修复

- **入站连接能被 accept，但随即 double free 打死内核。** 最小复现（server world，`a20.tcpmode=lwip`，宿主经 hostfwd 连入）：

  ```
  # guest 内
  nc -l -p 8080 0.0.0.0 < /dev/null
  # 宿主
  bash -c 'exec 3<>/dev/tcp/127.0.0.1/2234'
  ```

  握手是通的——宿主侧 `connect()` 返回成功，lwIP 的 LISTEN 查找命中。随后立刻：

  ```
  ========== KERNEL PANIC ==========
  lwIP assertion failed: mem_free: illegal memory: double free
  [PANIC] task: pid=19 name=nc
  [PANIC] caller=mem_free+0x38c
  ```

  `mem_free` 这条断言只在 `mem.c:639` 命中，即"该块已被标记为未使用"；而本分支`MEMP_MEM_MALLOC=1`，memp 各池的元素都出自 `mem_malloc`，所以这等价于**某个 memp 元素被释放了两次**。

  **根因不是 pcb 的引用计数算错，而是"把 pcb 交给 bottom half"和"pcb 可以安全地留在 lwIP 里"被当成了同一件事。** `lwip_tcp_accept_cb()` 返回 `ERR_OK`（等价于告诉 lwIP 这个 pcb 归应用所有），却**没有给它装任何回调**，于是 `pcb->recv == NULL`。若对端在 bottom half 跑完 `net_inet_tcp_apply_options()` 之前先发来 FIN，lwIP 会走到它自己的`tcp_recv_null()` 兜底路径，其 `p == NULL && err == ERR_OK` 分支直接 `tcp_close(pcb)`并把 pcb 释放掉——而 accept stage 槽位还指着这块已被释放的内存。bottom half 随后拿到这个悬垂指针，把它交给新建的 child socket，teardown 再 `tcp_abort()` 一次，第二次释放就撞上 `mem_free` 的断言。

  修复（`80c20884`）把 accept stage 从"一个裸 pcb 数组"改成自带上下文的每槽位结构（`net_accept_stage_slot_t`），并在 stage 时就装上 `lwip_tcp_stage_{recv,sent,err}_cb`：

  - staging 期间收到的数据以 `pbuf` 引用挂在 `c->pending` 上（`pbuf_free()` 只在`g_lwip_lock` 下安全，所以只能在 err 回调或 bottom half 里释放），被采纳时搬到 child 的 bh_ring；
  - staging 期间到达的 FIN 只置 `c->fin`，绝不动 `dead`，并在采纳时重放到 child 的`bh_closed`，于是"握完手立刻 FIN"的请求不再落到兜底路径上；
  - `lwip_tcp_stage_err_cb()` 置 `dead` 并在 `g_lwip_lock` 下清 `pending`，bottom half 据此跳过这个槽位；
  - 采纳前**先把 pcb 重新 `tcp_arg()` 到 child 并清空槽位，再推进 `tail`**——槽位地址就是 pcb 的 `callback_arg`，只要 `tail` 没过 i，生产者就不可能复用槽位 i，这个顺序消掉了"生产者还在往这个槽位投递事件"的窗口；
  - listener 拆除时（`net_tcp_close_pcb()` / `net_tcp_drop_pcb()` / `net_inet_socket_destroy()`）统一 `net_inet_accept_stage_purge()`，此前已建立的连接会被静默泄漏。

  同一条路径上还修了两个独立缺陷：`lwip_tcp_recv_cb()` 在 bh_ring 满时既 `pbuf_free(p)`又 `return ERR_MEM`，而 `ERR_MEM` 的语义是 lwIP **并没有**接管这个 pbuf（`tcp_in.c`把它存进 `pcb->refused_data`，`tcp_process_refused_data()` 还会再交回来），所以这本身就是第二次释放；现在改成先用 `bh_ring_reserve()` 整体判定、要么全进要么全不进。另外`pbuf_cat()` 会自行为被拼接的链取引用，因此"留下回调交给我的 pbuf"的正确写法是直接 `pbuf_cat(c->pending, p)`，多写一次 `pbuf_ref()` 会多数一。

  验证：`make smoke-net-accept` 的 lwip 那一趟 PASS（`TCP_ACCEPT_TEST: PASS port=12346`，`net_accept_drop=0`、`net_bh_overflow=0`、`net_alloc_fail=0`，无 panic），此前同一份产物必 panic；经 slirp 打到 guest 8080 的入站连接不再触发断言；server world 里 dropbear 在 22 端口可从宿主端到端登录。

#### fast 模式门禁发红：是测试自身的单位错误，不是内核缺陷

- **`smoke-net-accept` 门禁红在 fast 模式**：`TCP_ACCEPT_TEST: FAIL port=12347 (client=-1)`。内核并没有错：`user/cmds/net/tcp_accept_test.c` 的客户端重试循环把 `waited` 按`CONNECT_RETRY_US`（微秒）累加，却拿它去比 `CONNECT_BUDGET_MS`（4000，毫秒），所以`waited` 从 0 加到 20000 就已经越界，**循环体只跑一次**，客户端只发起一次 `connect()`，而它恰好要跟 fork 出来的服务端 `bind()`+`listen()` 抢跑。谁先谁后不确定，于是表现为"fast 模式 1/4 概率挂"。注释里写的意图（"长到不会在 4 核 guest 上误触发，短到卡死时几十秒内失败"）显然是想要 4 秒的重试预算，实现没兑现。

  定位证据是在 `-smp 1`（串口无并发交错，输出不可能丢）下跑多轮：每个失败端口只打印**一条** `connect()` 入口的调试行，而不是注释所暗示的 200 次重试。改成统一微秒预算（`CONNECT_BUDGET_US`）后，同一 boot 内连跑 8 轮 8/8 PASS，且 socket 计数每轮回到基线，无泄漏；关掉全部调试输出后 `make smoke-net-accept` 连跑 6 次全绿。

  两种模式现在给出同一个可观测结果，这条不再是例外项。

## 九、推荐的第一批动作
按「改动小、风险低、避免真实事故」排序：

1. ~~补一个挂 `ich9-ahci` 的门禁，让 AHCI flush 获得运行验证。~~ —— **已完成**：门禁 `make smoke-ahci-ich9`（x86_64 QEMU q35 + `-device ich9-ahci` + `ide-hd`，ext4 镜像挂 /extra，跑`fsync_durability_test`，断言 `block_flushes` 增长且驱动自有的`ahci_irq_completions > 0`）实跑 PASS，同期把 `ahci.c` 的完成路径改为默认中断驱动（`a20.ahci.poll=1` 保留轮询回退）。 QEMU 上的运行验证已拿到；真实 SATA PHY 上仍无证据，那条边界不变。
2. ~~崩溃注入测试基础设施~~ —— **已完成**，形式是 JBD2 提交序列内的定点 panic（`/proc/a20/journal` 下发注入点）+ 同镜像重启 + 宿主 `e2fsck` 比对，见 `make smoke-ext4-journal`。仍未覆盖的是"到点就死"的粗粒度形态（在写盘路径上随机取一个指令位置 kill -9）；当前覆盖的是语义上真正有意义的边界点。
3. 引入真机基准入口；当前所有性能结论都来自 TCG 模拟器。

以下两项曾在本清单里，现已完成，不再是待办：

- `/proc/pressure` 曾是单文件布局，systemd 按 `cpu`/`memory`/`io` 目录读会失败。该布局已改为目录，见 `kernel/fs/procfs/procfs.c` 的`pressure_entries` 与 `procfs_render.c` 的 `PF_PRESSURE_*` 分支。`mem`/`io` 渲染为结构性的零（设备 I/O 全部同步，没有可采样的 stall 状态，见 `kernel/core/psi.c` 的说明），不是伪造的数字。
- `MAX_PROCS` 死常量已删除，全树无残留引用。
