# A20OS 服务器就绪度评估

最后核实：2026-10（`feat/net-strengthening`）。§二 的网络小节与 §八 的阻塞项排序表
已按本轮实际提交逐条重写：入站 TCP / 回环传输 / IPv6 入站 / SACK 与时间戳 /
CUBIC / SO_SNDBUF·RCVBUF / RTNETLINK 通知 / 驱动 SG 与能力位 / conntrack+NAT /
核心锁断言接线 / socket 表与 per-socket 锁，均已落到代码并在对应小节给出
`path:line` 与门禁名；没做的项保持「未做」并写清原因。下文按服务器部署视角列出
A20OS 的当前能力边界、已知的结构性限制，以及按严重度排序的阻塞项；每条都给出
文件位置，便于自行复核。

运行类结论以当期提交为准；历史记录见 [archive/](archive/)。

## 一句话结论

A20OS 已经是一个认真的内核，但**当前形态是「QEMU 上的桌面/研究内核」**，
不是「服务器内核」。差距不在功能数量，而在四个结构性问题：入站 TCP 此前完全
不通、网络数据面被单一全局锁串行化、容器隔离的前置件（PID/userns/pivot_root）
缺失、真机 PCIe 可用性受硬编码 QEMU 假设限制。

第一条是本轮最大发现，且**已修**：`net_listen()` 此前丢弃已绑定的 PCB，
`tcp_listen()` 全树从未被调用，所以 listener 从来不存在于 lwIP 里，入站 SYN
一律被回 RST——协议栈没有任何对外服务能力。现在 `net_listen()` 按 `a20.tcpmode`
分两档，`lwip` 档会把绑定 PCB 转成真正的 LISTEN pcb；端到端实测（SLIRP
hostfwd 指向 guest telnetd）从"连接被对方重置"变为拿到可用 shell。**默认仍是
`fast` 档、行为不变**，服务器需显式选 `a20.tcpmode=lwip`。详见第二节。

网络这一项另有实质进展：收包路径的**内存模型**已经被认定为比全局锁更根本的
瓶颈并修掉了（见第二节），此前把归因指向读路径是错的——真正的解释是
`net_stress_test` 根本不经过 TCP 路径（见第二节"从没有测过 TCP"一条）。

**本轮（`feat/net-strengthening`）之后，socket 侧的最大单项已经不是全局锁了**：
`g_net_lock` 被删（`2f17a5ba8`），socket 表分片为桶锁，桶锁又被 per-socket 锁取代
（`7c7a4d7c8`，阶段 E），于是"1024 个 socket 在 recv/send/accept/close 上互相串行"
这条**已不再是未做项**。剩下最大的一项是 `g_lwip_lock` 本身——它至今仍是一把全局锁，
阶段 C/D/E 三步都只改了 socket 侧与分发侧。需要强调的是：所有性能数字都来自
QEMU TCG，`lwip` 自旋计数在该环境下噪声极大（同一负载四次运行跨越 0 到 920024），
因此本文件不再以自旋数量作为任何结论的依据。

在下面的阻塞项收敛之前，把数据库或不受信任的工作负载放上去是不安全的。

## 一、存储持久性

### 已达成

ext4 现在是可写日志文件系统，运行时 metadata 更新带 JBD2 ordered 语义：
每次写入经 `ext4_journal_meta_write` 登记进事务并 `bcache_hold_page` 持有，
使任何通用 sync 都不能把元数据写在其日志副本之前；`sync()`/`fsync()`
触发 commit（数据 → descriptor → 日志 superblock `s_start` → commit block →
元数据本位 → 标记日志为空）。挂载时是否回放以 journal superblock 的
`s_start` 为权威判据，而不仅是 `EXT4_FEATURE_INCOMPAT_RECOVER`——后者与
free 计数共享同一个被持有的 cache page，崩溃可能丢掉这个位却留下非空日志。

验证：`make smoke-ext4-journal`。它在提交序列的四个点上真的把机器停住
（注入点经 `/proc/a20/journal` 写入；同一机制也接受 `a20.journal_crash=`
命令行参数，便于手工复现），用同一块镜像重启，断言承诺过的写入没丢、没承诺
的写入没回来、`replay complete` 恰好出现在承诺点之后，并在宿主机上用
`e2fsck -fn` 检查崩溃后的镜像与恢复后的镜像都干净。走 procfs 而不是命令行，
是因为命令行注入点只在启动时解析一次，而 procfs 写入可以在崩溃发生前、
提交序列进行到一半时才武装，从而命中的正是那个边界；命令行形式则留给手工
复现。这是本文件里唯一一处
"断电"不是模拟出来而是真发生过的地方。详见 `docs/testing-gates.md`
「ext4 JBD2 崩溃一致性」。

`fsync()` 现在真正到达稳定介质。`block_dev_t` 有可选 `flush` 原语，
`bcache_sync_common()` 在写完数据后、缓存锁之外调用它。实现覆盖
virtio-blk（`VIRTIO_BLK_T_FLUSH`）、loop（转发 backing file 的 fsync）、
AHCI（`FLUSH CACHE EXT`）。

验证：`make smoke-fsync-durability`，断言 `block_flushes` 计数器确实增长。

### 仍缺

- AHCI 路径仅编译验证。`ahci.c` 位于 `CONFIG_AHCI` 之后，树内没有任何
  实例挂载 AHCI 控制器。补一个挂 `ich9-ahci` 的门禁是缺失的一环。
- 无 RAID、无数据校验和、无快照/CoW、无 fs-verity。
  文件数据块本身仍无校验和；`crc32c` 覆盖 JBD2 日志与 ext4 元数据
  （`metadata_csum`），不覆盖常规文件数据内容。
- JBD2 只支持 checksum v3（`COMPAT_CHECKSUM` 的 v1/v2 返回 `-EOPNOTSUPP`），
  没有 `barrier` 与 `async_commit` 特性位（`s_features` 中对应位不声明，
  因此 commit block 不写 `JBD2_FLAG_ASYNC_COMMIT`，设备也没有
  `ordered`/`journal_data` 语义差别）。断电原子性由 ordered 模式本身提供，
  不依赖设备 FUA 之外的屏障。

## 二、网络

### 入站 TCP 曾被 RST：listener 从来不在 lwIP 里（本轮已修）

`net_listen()` 只做 `s->local_tcp = 1` + `net_tcp_drop_pcb()`，而 `tcp_listen()`
**全树从未被调用**（已 grep 确认）。因此 `/proc/net/status` 的 `tcp_listen=0` 是常态，
入站 SYN 找不到 listener 被回 RST。**只存在于 socket 层的 listener 只能被同一内核内
走同样快捷路径的进程连接**，等于协议栈没有任何对外服务能力。

端到端实测（改动前）：SLIRP `hostfwd` 指向 guest `telnetd:2323`，宿主机连接返回
**"connection reset by peer"**。

已修：`net_listen()` 增加 `tcpmode` 两档。`fast`（默认，行为不变）与 `lwip`
（`tcp_listen_with_backlog()` + `tcp_accept` 回调，端口真正在协议栈上 listen）。
默认仍是 `fast`，因为既有 accept 测试是照它写的，且原注释记录了它存在的理由
（LTP 的 localhost accept 测试重依赖 close-after-accept）。改动后同一 hostfwd 实测
**拿到 guest telnetd 的 `A20OS remote shell` 提示符**。

选择方式有 `a20.tcpmode=lwip` 内核命令行与 `/proc/net/config` 写入口两处：服务器的
第一个 listener 通常由用户态开机创建，shell 写入口来不及生效。

**遗留缺口已修（`edc29d31a`）**：`tcpmode=lwip` 下的回环 TCP 传输此前过不了
`tcp_loopback_test`（握手完成、`tcp_recv_cb` 收到 18 B，但传输不结束）。两个成因：

1. `net_inet_socket_destroy()` 对已建立的 pcb **无条件** `tcp_abort()`。那是
   `tcp_abandon(pcb, 1)`，会把 RST 放上线，等于告诉对端"丢掉我发的东西"。服务端做
   的是最普通的 `write(); close()`，最后一段数据被这一行 routine 地毁掉，对端能不能
   看见取决于 RST 和数据谁先到接收队列。改成先解绑回调再 `tcp_close()`，`ERR_MEM`
   时才退回 `tcp_abort()`。
2. `net_inet_bottom_half_process_socket_locked()` 先处理 `bh_error`/`bh_closed` 再
   drain `bh_ring`，于是 ring 的 `if (!s->closed)` 守卫把 lwIP **已经交付**的字节
   一起扔了。TCP 在同一趟 bottom half 里交付数据和紧随其后的 FIN，于是 `read()` 对
   一个还欠着数据的连接回答 EOF。改成先把两个标志 capture 到局部变量、drain 完再按序
   施加。

实证（`ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=4 dev-build`，QEMU，
`a20.tcpmode=lwip`）：修前端口 12345 连跑 3 次 3 PASS、连跑 10 次 **0 PASS**，
每次 CLI 都报 `recv=0`，换 6 个不同端口仍 0/6；修后同端口连跑 8 次 **8 PASS**，
CLI 报 `recv=2 data=ok`。`make smoke-network-suite` PASS。

修 (1) 还暴露出一个此前被它掩盖的缺陷：优雅关闭会留 TIME_WAIT pcb，而 `SO_REUSE`
在本树曾是 `opt.h` 的默认 0、socket 层的 `reuseaddr` 又从未搬上 pcb，于是同端口
重启一律 `EADDRINUSE`。`lwipopts.h` 打开 `SO_REUSE=1`，`net_inet_tcp_apply_options()`
把 `reuseaddr` 搬成 `SOF_REUSEADDR`，登记见 `DIVERGENCE.md` §2.6。

**同一条路径上还修好了一个对外撒谎的错误码（`ec47d621e`）**：对端 reset 之后
`send()` 返回 `ENOTSOCK` 而不是 `EPIPE`。原因是派发按"当前是否持有 pcb"路由，
而 `lwip_tcp_err_cb()` 在 RST 和每一个致命错误上都会把 `s->tcp` 置 NULL，于是
`s->tcp == NULL` 恰恰是一条死掉的 TCP 连接的常态，这些 `write` 绕过
`net_inet_sendto()` 掉进通用的"两个套接字"路径。改成按套接字类型判定并显式豁免
fast 路径，沿用 `net_inet_send_tcp()` 已有的 Linux ABI 规则：`ever_connected` 则
`EPIPE`，否则 `ENOTCONN`。`sys_sendto` 与 `write(2)` 两条派发路径都有同一个缺陷。
这不是上一提交引入的——基线上它失败在 partial-io 上（3 次连跑只有 2 次过），
优雅关闭改变了 RST 与 send 的先后，把一直存在的窗口变成了稳定命中。
实证：`tcp_edge_test` 修前 2/3、修后 3/3；跨模式 lwip→fast→lwip 各跑一次，两次实际
执行均 PASS。

### `net_stress_test` 从来没有测过 TCP —— 历史性能归因需重读

本地 TCP connect 走 `net_inet_send_tcp()` 的 `local_tcp` 短路：直接
把数据入队到 peer 的 socket 队列，**不分配 pbuf、不进 lwIP 状态机、不触发 recv 回调**。
（这段短路此前写的是"取 `g_net_lock`"；`g_net_lock` 已随 `2f17a5ba8` 删除，
阶段 E 之后它取的是 `(s, peer)` 那对 socket 锁，见
[net/network-lock-contract.md](net/network-lock-contract.md)。事实本身不变。）

实测（4 核，16 MiB）：`net_rx_packets` / `net_tx_packets` / `net_bh_runs` /
`net_bh_events` **全部恰好为 0**，`net_lock_acquires` 仅 51。

所以凡是把 `lwip` 锁竞争归因于"net_stress_test N 路并发 × TCP"的地方，测的其实是
socket 队列。这也解释了为什么"去掉读路径轮询后 spin 没降"：流量根本不在 lwIP 路径上。
要评价 TCP 数据面，必须用走真实 netif 的负载或 `tcpmode=lwip`。

### RX-pending 门控曾饿死 loopif（本轮已修）

`netif_poll()` 是 `netif->loop_first` 的唯一排空点，而**没有任何路径无条件调用它**：
`kernel_progress_poll()` 与读者路径都是"按需"到达 `a20_lwip_poll_*`，而 park 在
`connect()` 的任务两个选择都不做。回环流量不产生设备 RX，于是门控只按设备提示关闭时，
SYN 永远躺在队列里直到 connect 超时。已把 loopif 排空移到 timer tick（唯一无条件运行的
progress 驱动），并让提示认 `loop_first`。假阳性只多一次锁获取，假阴性是挂死。

### 网络面现在有了可用的测量口径

`/proc/a20/perf` 此前约 75 个计数器**零个网络相关**，网络唯一信号是
`/proc/a20/lock_contention` 里一个没有分母的锁计数。已补 11 个（收发包/字节、
`g_lwip_lock` 获取、poll 次数与被门控跳过的次数、bottom-half 次数与事件数，以及
`net_bh_overflow` / `net_alloc_fail` 两个**正确性**计数器——非零即表示收包路径丢了自己
已接受的数据），另加 accept 路径的 staged/queued/drop。

`/proc/a20/{perf,lock_contention}` 增加 `reset` 写入口。此前计数器自启动累计、没有 reset，
压力期与引导期无法分离——这正是 `server-readiness.md` 曾经把"单次 acquire 极值"误归给
压力期、后来撤回的那个观测窗口缺陷。`lock_counters_reset()` 连
`contended_max_spins` 一起清零：reset 之后要回答的是"本窗口内的最大值"，留着引导期的
旧极值会让之后每个窗口都看起来和最差引导期一样坏。

**仍缺**：吞吐与延迟仍无法在本环境给出可信数字。loopback 绕过驱动路径，
`/proc/net/dev` 在 `lo` 上读 0；`rdcycle` 在 QEMU TCG 下跨 vCPU 不单调，
已产出过 `holdmax≈32s` 这种对微秒级临界区不可能的数值。绝对带宽只能上真机
（VisionFive 2 / LS2K1000，GMAC 已在树里）。

### 嵌入式档在 20 KiB SRAM 下放不下 —— 已用实测数字确认

数字来自实测结构体尺寸（探针编译单元 + `/proc/a20/netmem` 交叉验证，两者对六个池完全
一致），不是估算。**不能**从 QEMU 镜像的 `.bss` 反推：`MEMP_MEM_MALLOC=1` 让池成为对堆的
claim，而 riscv64 QEMU 目标有 1 GiB RAM，对 20 KiB 部件没有说明力。

### 真正的瓶颈不是 lwIP 池 —— 两块静态数组已纳入档位（2026-10）

池上限合计 25696 B 对 `MEM_SIZE` 16384 B（超 57%），但**即使把池全部解决也放不下**。
真正的约束曾是两块**与档位无关的静态数组**：

| | 字节 | 位置 |
|---|---|---|
| `g_pkt_ring` | 24,640 | `socket_packet.c`，硬编码 `NET_PACKET_RX_RING=16` × 1540 |
| `g_netif_state` | 12,672 | `lwip_stack.c`，`A20_NET_MAX_DEVS=4` × (rx1536 + tx1536) |
| 合计 | **37,312** | |

对照 20 KiB：`kernel/net` 静态合计 **42,662 B = 整个部件的 2.08 倍**；8 个
`net_socket_t` 合计 32,768 B = 1.60 倍；lwIP 堆 16,407 B = 80%。
这两块当时都在 profile 作用域之外，所以**只改 `net_profile.h`/`lwipopts.h` 无法让
tier 1 装进 20 KiB**。

**现已修（`wt/embedded`）**：`net_profile.h` 新增 `PACKET_RING_SLOTS` /
`PACKET_FRAME_SIZE` / `NETIF_MAX_DEVS` / `NETIF_FRAME_SIZE` / `NETIF_MTU` /
`STATIC_BUDGET`，EMBEDDED 档取 4 × 512 与 1 个 netif，DEFAULT/SERVER 保持 16 × 1536
与 4 个 netif。总账由断言钉住而不是靠注释：`net_profile.h` 断言宏算术的上界
（≤ `STATIC_BUDGET`），`socket_packet.c:40` 与 `lwip_stack.c:178` 各断言一次真实
`sizeof()`。

实测（`make dev-build NET_PROFILE=1`，`nm --size-sort` / 运行期 `/proc/a20/netmem`）：

| | 前 | 后 |
|---|---|---|
| `g_pkt_ring` | 24,640 | 2,064 |
| `g_netif_state` | 12,672 | 1,120 |
| 合计 | **37,312** | **3,184**（−91.5%） |

DEFAULT 与 SERVER 两档的同名符号字节不变（`0x6040` / `0x3180`），行为未动。为让这个
数字在运行期可读，`/proc/a20/netmem` 在池表**之后**追加一行
`static .bss (not from the heap): ...`（放在表外，故不干扰任何 `^POOLNAME <数字>`
形态的既有断言）。

**仍未解决，因此本节标题的结论只被部分推翻**：20 KiB 依然装不下 tier 1 —— 8 个
`net_socket_t` 单是 32,768 B（1.60 倍）就已经越界，lwIP 堆 16,407 B 也是 80%，这两项
都在 profile 之内、且本次未降。64 KiB 按当前配置仍**不够**：42,662 + 16,871 =
59,533 B（90.8%），余 6,003 B —— 装不下两个 socket。净效果是 tier 1 的静态部分从
"整个部件的 2.08 倍"变成"约 0.16 倍"，即**剩下的账全在 profile 之内、可以按档位调的量
上了**；要真做到 30 KiB，还需要 profile 内降 `MEM_SIZE`/池/`MAX_SOCKETS` 与关 IPv6。

### tier 1 连 `net_stress_test` 都跑不了 —— 现在它自己会说（2026-10）

**0/5 通过**（tier 2 是 5/5）。原因确凿：`run_worker` 每个 worker 用 3 个 socket
（client connect + server listener + accepted child）× 4 worker = 12 个并发，而 tier 1 的
`NET_MAX_SOCKETS = 8`。单次偶发 PASS 只是 worker 未同时到达峰值。
按要求报出来而非上调上限 —— 这说明"tier 1 可运行"这个说法需要限定。

**现已改为显式 SKIP**：`net_stress_test` 在起跑前从 `/proc/net/status` 读档位上限
（`syscall-sockets: ... max=N`，为此在 `kernel/net/socket.c` 的该行末尾追加了
`max=`），算不出 `WORKERS * 3 < 上限` 时输出

```
NET_STRESS_TEST: SKIP (socket table ceiling 8 < 12 concurrent sockets required:
4 workers x 3 (listener + accepted child + client)); tier too small for this
workload, upper limits left as configured
```

并以 0 退出。**上限没有上调，也不会有门禁因为这个 SKIP 变绿**：所有以
`NET_STRESS_TEST: PASS` 为判据的门禁（`smoke-smp-lock-contention` /
`smoke-net-lanes` / `smoke-net-lanes-n1`）都匹配不到这一行，档位过小时它们仍然失败。
上限在运行时读而不是编译进测试，是因为用户态构建不随 `NET_PROFILE` 重建
（`Makefile:447` 的 `USER_BUILD_ID` 只含 `ARCH/NOMMU/OPT/PROFILE`）；读不到时照常跑
测试，不会因缺证据而静默跳过。实测：tier 1 输出上面的 SKIP（`max=8`），tier 2 仍是
`NET_STRESS_TEST: PASS (4 parallel transfers, 4 rounds x 1048576 B)`（`max=1024`）。

### STM32F103 根本不编译网络栈 —— README 已按实情改写（2026-10）

两个独立障碍：

1. 本机无 ARM 工具链（无 `arm-none-eabi-gcc`）。
2. 更根本：`PROFILE=mcu`（armv7m 自动启用，`Makefile:247-248`）走 `Makefile:1166` 的
   **另一份源文件清单**（`components/trim.toml` 的 `[profile.mcu].sources`），其中**既无
   `kernel/net` 源文件也无 `$(LWIP_SRC)`**。所以 `NET_PROFILE` 对该目标完全没有作用，
   **tier 1 根本没被编译进去**。

即 README 声称的 STM32F103（20 KiB SRAM）支持，在网络上不只是"内存不够"，而是**从未构建**。

**已处理（本条按"修 README"的方向关闭）**：README「支持的硬件平台」的 MCU 条目现在
明写"**不含网络**"，说明 `PROFILE=mcu` 的独立源清单里既没有 `kernel/net/*.c` 也没有
lwIP、`NET_PROFILE` 对该目标无效，并把"给 mcu profile 真的纳入网络栈"记为尚未做的
**产品决定**而非调参；`make stm32f103-bringup` 的注释也一并标注"无网络栈"。
（行号随本节引用漂移：`Makefile:931` 现在是 CFLAGS 段，MCU 源清单在 `:1166`。）

### ~~`rx_frame`/`tx_frame` 硬编码 1536~~ —— 已修（2026-10）

原附带项：`lwip_stack.c` 的 `rx_frame`/`tx_frame` 硬编码 1536，不随 `PBUF_POOL_BUFSIZE`
变，所以 Cortex-M3 会把整尺寸帧收进 512 字节的池，每帧链 3 个元素；`PBUF_POOL_SIZE=24`
只够 8 个整尺寸帧，而整个部件只有 20 KiB。

**现已修**：帧缓冲区改为 `NET_PROFILE_NETIF_FRAME_SIZE`（EMBEDDED 档 = `PBUF_POOL_BUFSIZE`
= 512），并且 `NET_PROFILE_NETIF_MTU`（= `PBUF_POOL_BUFSIZE - ETH_HLEN` = 498）跟着走 ——
只改缓冲区不解决链元素的问题，MTU 还是 1500 时收上来的帧仍要链 3 个元素。DEFAULT 与
SERVER 档这两个值就是原来的 1536 与 1500，行为不变。同时补上了
`a20_lwip_if_set_mtu()` 这个运行时漏洞（它此前只校验 RFC 791 下限，`SIOCSIFMTU=1500`
会让收包截断、发包 `ERR_BUF`），现按档位帧尺寸封顶。

**未验证的边界**：`a20_lwip_if_set_mtu()` 的新封顶路径没有任何门禁——树内除 musl
头文件外没有 `SIOCSIFMTU` 使用者。DEFAULT/SERVER 的上限 1522 高于历史 1500，
所以现有行为不变，但这条路径未被任何 smoke 覆盖。EMBEDDED 档 MTU=498 与
`net_stress_test` 的 SKIP 行为只在 riscv64 QEMU 上实测过（`TCP_LOOPBACK_TEST` /
`ICMP_LOOPBACK_TEST` PASS）；本机没有 ARM 工具链，且 `PROFILE=mcu` 根本不编译网络栈，
所以 Cortex-M3 上的行为**无实测证据**。

### `g_net_lock` 分片 —— 已完成（`2f17a5ba8` + 阶段 E `7c7a4d7c8`）

本文件此前把"`g_net_lock` 分片"列为 P0「当前收益最大的未做项」。**它已经做完了**，而且
分两步：`2f17a5ba8` 删掉 `g_net_lock`、把 socket 表分片成 `g_net_buckets[]`；
阶段 E 再把 per-socket 状态从桶锁搬到 `net_socket_t` 内嵌的 `spinlock_t lock`
（`socket_internal.h:405`）。桶锁现在**只**管 registry slot 表与每桶空闲位图。

对服务器的净影响：SERVER 档一个桶是 512 个 slot，所以阶段 E 之前一个 socket 的
`recv` 要和同桶另外 511 个 socket 互斥——**桶号来自 slot 分配顺序，不来自 lane**，
所以 lane 分得再细也没用。阶段 E 之后只剩真正共享同一对象的那一对。

**但验证缺口仍在**，且不要把它读成"已完成"：`2f17a5ba8` 的提交说明自陈三项运行期
验证未做——引用计数不漏不重、`LOCK_COUNTERS_MAX` 注册预算、`-ENOTCONN` 窗口，
清单在 `docs/measured/impl-notes-net.md`。合并后**没有跑全量回归，ASAN + SMP 压测
未做**；阶段 C/D/E 自身只做了各自的编译期与逐字节等价自检。锁规则写在
[net/network-lock-contract.md](net/network-lock-contract.md)。

### IPv6 入站 TCP：AF_INET6 socket 现在真的能拿 pcb（`c34ddd7f8`）

此前 AF_INET6 流式套接字在**两种 TCP 模式下都无法从外部 accept**，而且对端 FIN 会
静默丢掉对端已经发来的数据。两件事都已修。

v6 入站不可达有五处各自独立的丢弃点：`net_inet_socket_init()` 只对 AF_INET 调
`tcp_new_ip_type()`，所以 v6 socket 一辈子 `s->tcp == NULL`；`net_inet_bind_pcb()`
对 AF_INET6 提前返回 0，留不下 pcb，于是 `listen()` 没有东西可转成 LISTEN pcb；
`net_listen_sock()` 把 lwIP LISTEN-pcb 路径按 AF_INET 门控；`net_inet_connect_stream()`
要求 `domain == AF_INET`，于是每个 v6 connect 都得到 `-ECONNREFUSED`——一个误导性的
errno，因为没有任何东西拒绝过；accept drain 硬编码 `child->domain = AF_INET`，
于是被接受的 v6 连接把 16 字节的 v6 地址按 v4 布局读回来。

lwIP 的 `tcp_pcb` 本身是双栈的：family 来自交给 `tcp_bind` /
`tcp_listen_with_backlog` 的那个 `ip_addr_t`，不需要任何 pcb 机制。要做的是 socket 层
别再把 family 扔掉。

数据丢失是另一个与 family 无关的既有缺陷，是 v6 的工作把它暴露出来的：
`net_inet_bottom_half_process_socket_locked()` 在对端 FIN 到达时置 `s->closed = 1`，
而 `s->closed` 是**本地**生命周期标志、`net_socket_is_live()` 是
`s && !s->closed`——于是一个健康 socket 报告自己死了并从 `/proc/net/tcp` 消失。更糟的是
FIN 分支跑在同一趟的 staging 循环**之前**，那个循环的 `if (!s->closed)` 守卫于是把
ring 里还没搬的每个载荷事件一起丢掉。lwIP 先交付数据再交付 FIN，所以对端一次
`write(); close()` 把两者放进同一趟 bottom half，消息的尾巴被扔了。现在对端 FIN
只置 `s->peer_closed`（`socket.c` 本来就当它 EOF）加一次 `A20_EVENT_CLOSED`。

隔离方法值得记一笔，因为它是**量出来的不是猜的**：一个按 family 参数化的探针显示
v4 与 v6 **在同一偏移截断**，在 stash 出来的基线上重跑该探针，v4 也在同一偏移截断
——所以丢失是既有的、不是本次引入的，而 v6 的 accept 是彻底超时。在对端 close 之前
插 8 秒 sleep 让两者都过，于是定位为"与对端 read 的竞态"而非发送故障，指向 bottom
half 的顺序，修复确认了这个判断。

`/proc/net` 下现在有真的 `tcp6` 与 `udp6`（`procfs.c:464-469`），按 v6 布局渲染，
v4 文件不再混入 v6 行。**残留**：rtnetlink 组播只覆盖 IPv4 地址组，没有 IPv6 地址
写入路径。

### TCP 选项与拥塞控制从 opt.h 的默认 0 变成档位决定（`89bb8c348` / `0d42ca758`）

SACK（RFC 2018）与 TCP 时间戳（RFC 7323）在上游 `opt.h` 里默认是 0，于是对着真实
对端只有 Reno 的丢包恢复、没有 PAWS。现在按 `net_profile.h` 的档位决定：DEFAULT 与
SERVER 打开（各 4 个 SACK 块），EMBEDDED 关掉——在 512 B 的池元素上这两个选项的开销
直接从载荷预算里扣，而那个档位的目标是有界占用。代价记在同一处：选项与载荷**共用**
池的头元素，所以 `NET_PROFILE_PBUF_BUFSIZE` 必须从 1536 涨到 1600，账是
`1460 + 54 + 12 + 36 = 1562`，并由 `lwipopts.h:214` 的 `_Static_assert` 钉住。
DEVICE 侧的 netif 暂存仍是 1536——它装整帧（1500 + 14），不是带选项的段。

`TCP_CONGESTION` 此前接受 `"cubic"` 并返回成功，**而本树没有 CUBIC**：一个
`cubic-but-not-really` 的名字会让调用方以为自己拿到了算法。现在未知名返回
`-ENOPROTOOPT`，`getsockopt` 回真实算法名，`"cubic"` 走 RFC 8312 的核心条款。

**CUBIC 未做的部分**（逐条登记在 `DIVERGENCE.md` §2.5 与
`lwip/priv/tcp_cubic_priv.h` 的头注释）：没有 §4.2 的 TCP-friendly 公式（只以 Reno
速率近似），没有 HyStart / TCP-AQ / DCTCP / Prague，没有 ECN，`W_max` 不跨 pcb 持久化。

### `SO_SNDBUF` / `SO_RCVBUF` 不再是被接受然后忽略（`aacce4dcc`）

`SO_SNDBUF` 现在约束该 socket 在 pcb 发送队列里的字节上限，`net_inet_send_tcp()`
拿它量队列深度；`SO_RCVBUF` 经 pcb 的新字段 `wnd_limit` 约束该连接的接收窗口
（没有这个字段，上限会活到第一次 `read()` 为止，因为 `tcp_recved()` 每次读都把窗口
原样还给 `TCP_WND`）。`getsockopt` 回读夹紧后的生效值。

边界四条，都写进 `net_inet_tcp_buf_apply()` 的注释：发送侧**不约束在途飞行字节**
（在途是拥塞控制的事）；**无自动调优**（没有 `tcp_wmem`/`tcp_rmem`、没有内存压力反馈）；
**抬高 `SO_SNDBUF` 只在下一条连接生效**（lwIP 在已有未确认字节时没有把 `snd_buf`
涨回去的机制，本轮没发明该机制）；两者都是 **TCP 范围**（UDP/RAW 的 `SO_SNDBUF`
仍返回 `-EOPNOTSUPP`，它们的缓冲区在 socket 层而不是 pcb 上）。

修 `SO_RCVBUF` 时自己引入过一条回归：`socket()` 时发出杂散 ACK。成因是拿
`TCP_WND_MAX(pcb)` 与 `pcb->rcv_scale` 当夹紧上限——两者在握手前都不对
（`TCP_WND_MAX()` 此时答 65535 而非 93440，`rcv_scale` 仍是 0），会把每个 socket 从
创建那一刻永久钉在 64 KiB。症状是 IPv6 回环传输在 lwIP TCP 模式下不完成而 fast
模式下正常（后者根本没有 pcb）。改为用**编译期常量** `TCP_WND` 与 `TCP_RCV_SCALE`
夹紧，另加"只有收窄才需要通告窗口"的判定。回归定位与修复见 `f34b451f4`。

### RTNETLINK 现在真的会通知（`8b15e3323`）

`NETLINK_ROUTE` 套接字 bind 到 `RTNLGRP_LINK` / `RTNLGRP_IPV4_IFADDR` 时，分别会
收到 `RTM_NEWLINK`（链路状态变化）与 `RTM_NEWADDR` / `RTM_DELADDR`（IPv4 地址写入
或删除）。地址全零时发 `RTM_DELADDR` 而不是 `RTM_NEWADDR`——"你现在有地址 0.0.0.0"
不是任何接口处于的状态。一个监听者的队列满不挡其他监听者（计数并 klog 一条）。
**残留**：只覆盖 IPv4 地址组。

### 驱动：SG 发送与能力位上报已接线，校验和卸载刻意不做（`d12733f48` / `089a2055c`）

`net_dev_ops_t` 增加了可选的 `send_sg()` 与 `caps()`（`driver_class.h:174-175`），
`lwip_stack.c` 在 `a20_lwip_linkoutput()` 里二选一。**没实现 `send_sg` 的驱动走原
线性拷贝路径，逐字节不变**——但要注意 SG 分支**不是零拷贝**：命中前提是
`p->next == NULL`，帧仍然先拷进 `st->tx_frame`，因为 netfilter 的 output hook 是对调用
方手里那块帧的**原地改写**，而 pbuf payload 是别的持有者也在看的池内存。SG 给驱动
的是描述符而不是 lwIP 的 pbuf，省掉的是驱动内部再拷一次。

**校验和卸载按诚实原则未启用**：`TX_CSUM_OFFLOAD` / `RX_CSUM_OFFLOAD` 两个能力位
已定义（`driver_class.h:145-146`）但**永不置位**。此前记录的理由（"vendored lwIP 2.2.2
没有任何承载该握手的 flag"）经复核**不成立**，已更正：位与字段都在
（`netif.h:140-153`、`:340-342`、`:408-417`），只是被
`LWIP_CHECKSUM_CTRL_PER_NETIF` 关着（`opt.h:2371-2373` 默认 0）。不启用的真实理由是
三条接不上的线：TCP 发送路径 `tcp_out.c:1587-1596` 无条件写完整校验和、lwIP 的位是
**每 netif** 而不是**每帧**而本 HAL 的 `recv()` 只回长度、QEMU 的 virtio-net 从不设置
`VIRTIO_NET_HDR_F_DATA_VALID`。完整调查与"要启用需要改哪些上游文件"见
`docs/net/checksum-offload.md`，意向已登记在
`kernel/external/lwip/DIVERGENCE.md` §2.10。`MRG_RXBUF` 不同：它是纯设备侧的接收
属性，lwIP 从来看不见，所以协商了的驱动**应该**上报它。

virtio-net 收发环 32 → 256 并协商 `MRG_RXBUF`；e1000 环 64 → 256、多缓冲帧线性重组、
82574L ITR 节流。**MRG_RXBUF 的重组分支现在由真实流量跑到了**：接收缓冲改为 512 字节
载荷的小缓冲（各占一条独立 avail 条目、非链式描述符），按 QEMU 的收包循环算术，
一个满 MTU 帧跨 3 个缓冲、600 字节上下的帧跨 2 个。QEMU 10.0.13 下实测首个跨缓冲帧
打出 `[VIRTIO-NET0] first frame reassembled from 2 buffers (rxbuf=524)`，同一轮里
guest `wget` 取回 65536 字节且 `wc -c` 复核为 65536、`ping 10.0.2.2 4` 为 4/4。
原先"重组永不触发"的原因是投递的缓冲比帧还大，
而"拆成多个描述符就收不到"的原因是把头拆去了另一个描述符——取舍与三组实测数据写在
`virtio_net.c` 的 `VIRTIO_NET_RX_DESC_MAX` 注释里。

**这一条的门禁边界**：`make smoke-network-suite` 本身**不**覆盖跨缓冲收帧——它跑的是
回环与 telnetd/DNS 的小帧，`ping` 回包和 TCP 握手帧都装得下一个 524 字节缓冲，
`num_buffers` 恒为 1，所以该套件通过时日志里不会有重组行。跨缓冲路径要单独用上面那
种"guest 向宿主 HTTP 服务器拉大文件 + ping"的实测来证明。
`VIRTIO_NET_F_MQ` 只探测不协商（多队列未实现），启动行以 `offered(mq=.. csum=..)`
打印，不存在"协商了却没用"的半成品。

**验证边界**：QEMU 的 e1000 是 82540EM，无 ITR 寄存器，驱动按设计跳过（打印
`itr=0us`）——这一条仍是"只做到编译 + QEMU 启动验证（ring=256）"。e1000 多缓冲重组
**已被真实跨描述符帧跑到**：默认 RCTL 丢弃 1514 字节以上的帧（`e1000x_is_oversized()`），
所以驱动现在置 `RCTL.LPE` 并把 `JUMBO` 定在两个缓冲（4074 字节）；帧由
`-netdev socket,id=n0,fd=N` 从一个无需特权的数据报 socketpair 注入（slirp 无 mtu 选项、
本机无 raw socket，都产不出 2048 字节以上的帧）。实测
`[E1000] first frame reassembled from 2 descriptors (buf=2048)`。
**未做到的**：这样的帧在 lwIP 里仍会被丢弃，因为链路 MTU 是 1500
（`net_profile.h`），端到端 jumbo 需要档位表与 pbuf 一起改，属协议栈范围。

### conntrack + NAT 已落地（`f48a8f5f2` / `0d9d0885f` / `4b4472d17`）

五元组哈希表（EMBEDDED 64 条 / DEFAULT 256 / SERVER 1024，按档位）、NEW/ESTABLISHED
状态、按状态分开的空闲超时（TCP NEW 30s、ESTABLISHED 120s、UDP 30s）、满表 LRU、
`/proc` 计数。SNAT/MASQUERADE 在 output hook，DNAT 在 input hook，就地改写帧并按
RFC 1624 增量修校验和。运行时动词挂在 `/proc/a20/netfilter`（`ctflush` / `cton` /
`ctoff` / `natadd` / `natdel` / `reset` / `flush`）。

新增门禁 `make smoke-netfilter-nat`：QEMU `hostfwd` 18081 → guest 18082，宿主侧探针
连上并收到回显。

**三个必须一起说的边界**：

- **每条目挂正向 + 回程两条链**，否则回程按转换后元组查不到条目。这是本实现最容易
  做错的一处：只挂一条链的失败模式是**静默**的——正向翻译正常、回程查不到、原样发出，
  对端收到一个不认识的源地址，没有任何计数器会动。
- **端到端门禁只覆盖 DNAT**。SNAT/MASQUERADE 只有解析器与 `/proc` 规则、conntrack
  记账和单元级证据，没有端到端门禁——QEMU user-net 拓扑里 SNAT 没有对等场景。
  `masquerade` 相对 `snat` 的差别仅是取址时机（`a20_lwip_netif_ipv4(-1)` 选当前有
  地址的 netif），**未在真机上验证过 DHCP 换址后的行为**。
- **LRU 淘汰与空闲超时两条路径只有 `/proc` 计数暴露**（`ct_evicted` / `ct_timeout`），
  没有门禁真的把表填满或跑满 30s/120s 超时；`netfilter_test` 测试 8 覆盖的是新建条目
  与计数增长。

其余限制（无 ALG、无 ICMP 跟踪、不做分片 NAT）写在
[net/conntrack-nat.md](net/conntrack-nat.md) 的「诚实边界」一节，不重复。

顺带一条方法教训记在这里，因为它会被下一个人重犯：审计 `netfilter.h` 时得出过
"`/proc` 读者未取锁"这个**误判**——`netfilter.c:641` 早已取锁；据此加的那把锁造成了
自死锁（`spin_lock_irqsave()` 不可重入），已在 `4fde1c089` 回滚。误判的原因很朴素：
那次 grep 只覆盖了 `lwip_stack.c` 和 `procfs.c`，没覆盖 `netfilter.c`。怀疑锁问题先量
reentrancy，别先读 owner 字段。

### 核心锁断言接上了，net 锁一侧仍然没有（`7d217d3fd`）

本文件此前隐含一个前提：`docs/net/network-lock-contract.md` 只是文档，没有运行期
强制手段。这**不再成立**。`LWIP_ASSERT_CORE_LOCKED()` 从上游的空宏接到 `g_lwip_lock`
的**持有者 CPU** 上：违规先记站点再 panic，`/proc/net/stats` 打印
`armed= / owner= / violations= / sites=`。开关 `CONFIG_NET_LOCK_ASSERT` 默认 0
（排查用，不进常规构建）。同时给 7 个此前完全没有断言的裸入口补上断言，把探针盲区
变成覆盖区。

**但覆盖只有 `g_lwip_lock` 这一把。** net 锁一侧**没有**对应探针：没有"当前 CPU
是否持有期望的 socket 锁"的判据，`net_sock_lock2()` 的地址升序与"同时至多两把"只由
代码评审把关，lane claim 一侧同样没有。补它需要 per-CPU 持锁集合跟踪，本轮未做。
这是锁契约与实现之间最大的一处落差，记在
[net/network-lock-contract.md](net/network-lock-contract.md) 的迁移检查清单末尾。

## 三、隔离与多租户

### 已达成

cgroup v1/v2 是真的，且在热路径上强制：`cg_mem_charge()` 在缺页路径有
十四个计费点，`cg_cpu_account()` 在每次上下文切换记账。`cpuset` 通过掩码
求交生效（`sched_task_cpu_mask()`）。这比很多「有 cgroup 目录」的项目实在。

### 仍缺

1. **8 个 namespace 已有 3 个是真的**：mount、PID（`smoke-pidns`）与
   user（`smoke-userns`）。其余 5 个（net、cgroup、time、uts、ipc）
   仍由 `unshare()` 显式返回 `-EINVAL`，这一点是干净的
   （`sys_namespace.c:4-7` 明确写了边界）。
2. ~~`pivot_root` 返回 `-EPERM`~~ —— **已补齐**。前置件确实就是先把
   root/cwd 从路径字符串换成真实引用：现在 `mount_t` 带 `mnt_parent` /
   `mnt_mp` / `mnt_child` 真实挂载树，`proc_fs_context_t` 带
   `root_mnt` / `root_vn` / `cwd_vn` 三个引用，`pivot_root` 按 Linux 顺序
   校验后把旧 root 的 mount 摘出命名空间并标记 `VFS_MOUNT_DETACHED`——
   旧根此后不可按路径访问，只有 pivot 前打开的 fd 还能读到。
   `umount2` 的 `MNT_FORCE` / `MNT_DETACH` 也真正转发，busy 判定基于
   mount 上的引用计数。详见 `docs/fs/vfs-edge-semantics.md` §9.4，
   门禁 `smoke-pivot-root`。**仍未覆盖**的是共享子树传播
   （`MS_SHARED` / `MS_PRIVATE` / `MS_SLAVE`），所以 mount 传播语义对
   容器编排仍然不完整——`pivot_root` 本身可用，但"pivot 之后再让子 mount
   传播出去"这条链路还没有。
3. **userns 已补齐**（`kernel/proc/userns.c`：`uid_map` / `gid_map` /
   `setgroups`、全局↔命名空间 id 翻译、`setns` / `listns`、
   `/proc/<pid>/ns/user`、按命名空间作用域化的能力判定）。
   剩下的缺口是无 `nsproxy`（`setns()` 一次只能切一种命名空间），
   以及 capabilities 仍是 15 个子集。
4. 无容器运行时（lxc/runc/nspawn/crun/podman 均无），`packages/world/`
   里没有 server world。
5. cgroup 缺 `pids` / `io` / `freeze` 控制器；`cpu.shares` 存了但调度器
   从不读取。
6. **全局 OOM killer 的评分只看 `oom_score_adj`，不看 RSS**，因此不会可靠地
   选中最大占用者（cgroup 局部路径倒是用了 RSS）。

结论：**当前形态仍无法承载多租户**。PID ns + userns + `pivot_root` 三件套
现已齐备，剩下的门槛是另外几项：没有容器运行时、没有 `nsproxy`、
capabilities 只有 15 个子集、mount 共享子树传播未实现，以及上面第 6 条的
OOM 评分。

## 四、进程与调度

- **无内核抢占**（无 `CONFIG_PREEMPT`，只有 `need_resched` 标志在安全点消费），
  也无 IRQ 线程化。`SCHED_FIFO` 存在，但过不了长内核路径的 deadline。
  `SCHED_RR` 本轮已修成真正轮转，但只能在调度器下次运行时让出 peers，
  无法按时间片强制抢占，这依赖上面那条抢占缺口。
- 无 RT 限流（`sched_rt_runtime_us`）、无 `RLIMIT_RTPRIO`：`SCHED_FIFO`
  任务可以独占 100% CPU，无预算、无计量。
- **`proc_lock` 是当前最大的压倒性热点**：4 核实测 2528 次竞争 / 951 万自旋，
  8 核 33335 次 / 1619 万自旋，多轮优化后仍 12–20K。根因是整个任务表只有
  一把全局自旋锁（`kernel/proc/proc.c:43`），`sched.c` 里有 30 处取锁点。
  归因标签要当心：实测最大的一行是 `proc_sched_safe_point+0x42`，但
  `proc_sched_safe_point()`（`sched.c:1028-1036`）只读一个 per-CPU 的
  `need_resched`，它自己不取 `proc_lock`；那一行其实是内联进去的
  `proc_yield()`，与 lwIP 那次 `net_vfile_read+0xf6` 是同一个"返回地址跳过
  一帧"的假象。`proc_yield()`（`sched.c:1988-1999`）同样不直接取锁：它调
  `proc_make_ready()`（状态转移）与 `sched()`（切换发布），二者才按
  `kernel/include/proc/proc.h:135-142` 的契约去取全局 `proc_lock`。**所以争用实际落在上下文切换/
  状态转移路径上**，与 `kernel/include/proc/proc.h:135-142` 描述的"切换发布要取 proc_lock"一致。
  这里曾断言成因是"全局锁被跨着一段 TLB 收敛等待持有"，**该断言已被实测推翻**。
  代码事实仍然成立：`context_switch_locked()` 在持有 `proc_lock` 的临界区内
  调用 `mm_context_enter(next->mm, cpu)`（`sched.c:1855`；`proc_lock` 由调用方取，
  在 `sched.c:1835` 放），而 `mm_context_enter()`（`mm/vm.c:120-151`）内部是一个
  `for(;;)`：只要
  `mm->tlb_cpu_generation[cpu] != mm->tlb_generation` 就反复
  `arch_tlb_flush_asid_local()`；`tlb_generation` 会被
  `mm_tlb_shootdown_page()`（`vm.c:357`）持续累加。
  但它并不是本轮争用的成因。为此在 `mm_context_enter()` 内加入三个计数器
  （`mm_context_enters` / `mm_tlb_converge_waits` / `mm_tlb_converge_flushes`，
  经 `/proc/a20/perf` 导出），在 4 核 `smoke-smp-lock-contention` 下实测：
  136 次进入、15 次至少刷新过一次、共 15 次本地 ASID 刷新，即
  `flushes - waits == 0`，不存在重复收敛。即便按每次刷新 1µs 的悲观估计，
  15 次也只有 15µs 量级，与同一次运行中 `proc: 1517 4051609`（405 万次自旋，
  按每次 10ns 估约 40ms 聚合）相差三个数量级，无法解释该量级。
  所以"持锁时间被 TLB 收敛拉长"是错的：先前"高频短临界区"的猜测并未被推翻，
  反而与实测相符。但**"高频短临界区"同样不成立**，为此又加了"单次 acquire
  最大自旋数"度量（输出里的 `max=`，锁级与调用点级都有），4 核实测
  `proc: 2802 5453081 max=519208`：均值只有 1946，**单次 acquire 最高自旋
  519,208 次**（按 10ns/次估约 5ms）。均值不高却被少数几次拉爆，说明这不是
  稳态高频争用，而是少数几次长持有；`lwip: 4 833397 max=472365` 同形。
  调用点最差的两个是 `proc_park_commit`（20 次 acquire，max 519208）与
  `sys_wait4`（6 次，max 438789），**次数极少而单次极长**，与 `proc_sched_tick`
  （1424 次，max 180652）这种高频但单次不长的形态相反。
  "park 持锁跨越睡眠"这一猜想已被核对推翻：`proc_park_commit`
  （`park.c:250-293`）取的是 `task->park_lock`，在 `:275` 先释放，再到 `:283`
  调 `sched()`，全程没有跨切换持有 `proc_lock`。
  仍缺的是持锁方一侧的度量：自旋数是等待方计的，只能说明"锁被持有了这么久"，
  区分不了"持有者在干活"与"持有者在临界区里被抢占"。
  这条已尝试并失败，结论是**当前验证环境下拿不到可信的持锁时长**。做法是给
  riscv64/aarch64/x86_64 补 `arch_read_cycle()`（riscv32 早已有同名封装，其余架构
  返回 0 作"无计数器"哨兵），在取锁时打时间戳、放锁时收口并按 `owner_ra` 归到
  调用点。（注意第三条障碍：`owner_ra` 现在只在 `CONFIG_DEBUG_LOCKS=1` 下由
  真正争用的 acquire 写入，见下文；即便前两条障碍不存在，默认构建里也拿不到它。）
  两个实测障碍，都不是代码能绕开的：
   1. QEMU TCG 的 `rdcycle` 不保证单调，它由宿主时间派生，多 vCPU 线程之间
      不一致。直接相减会下溢成 `18446744009993433250`（≈1.8e19）；加"单调性检查"
      之后不再下溢，但仍得到 `holdmax=31958042598`（≈32s）这种对微秒级临界区
      完全不可能的数值。**在真机上（`-icount` 或真实 PMU）才可能可信**。
   2. 因此任何在 QEMU 上读出的"持锁周期数"都**不能**当作持锁时长写进结论：
      宁可缺数据，也不能让一个数看起来像测量结果。
  换言之，**"持锁方在干活还是被抢占"这个问题在本环境的门禁下仍然没有答案**，
  下一步要么上真机（VisionFive 2 / LS2K1000），要么改用与时间无关的判据
  （例如统计"持锁期间该 CPU 是否发生过调度切换"）。
  另需注意：这些调用点标签与先前 `proc_sched_safe_point` 一样受"返回地址跳过
  被内联帧"限制（`sched()` 是内联的），符号名未必就是真正取锁的那个函数。
  `sched.c:1790` 的注释自己写着 *"mm_context_enter() only uses
  atomics"*：它按设计不需要 `proc_lock` 的一致性，却仍然被罩在临界区里。
  但不能简单把它前移：地址空间切换必须与 `proc_set_current()` 之间的
  中断窗口保持一致，否则中断处理会在"新 mm + 旧 task"的错配状态下运行。
  这属于上下文切换路径的定序设计，需要连带审阅 `sched.c:1880` 那条
  "one lock per switch" 的注释所描述的观察窗口保证。

  这段循环在本平台确实会执行，不是死代码，上面的计数器已经直接证明：
  136 次进入、其中 15 次真的刷新过 ASID。平台相关性依然要交代清楚：QEMU riscv64
  启动日志为 `[MM] RISC-V ASID mask=0xffff bits=16`（`riscv64/platform/asid.c:36`），
  所以 `mm->arch_asid` 非零，`vm.c:127` 的条件成立；arch.h 的通用默认是
  `ARCH_MM_CONTEXT_ALLOC() 0U`，只有 riscv64 覆写为 `riscv64_asid_alloc()`。
  **在 `arch_asid == 0` 的架构上这三个计数器恒为 0，引用本节数字时不能跨平台套用**。

  但也不能简单前移：`context_switch()`（`sched.c:1894`）那条调用点是本函数
  自己取锁，可以改成"先关中断 → mm_context_enter → 再取锁发布"；可是
  `sched()` 那条进入时 `proc_lock` 已经被更早取走了（取锁在 `sched.c:1963`，
  进入 `context_switch_locked()` 在 `sched.c:1950`，见 :1944 注释，
  刻意为了"每次切换只取一次锁"）。在那里要先放锁才能做 mm 切换，而放锁之后
  `next` 可能被别的 CPU 抢走，必须靠 `dispatching` 引用计数兜住并重新校验。
  这一点已核对过 `sched_runq_unpick_locked()`（`sched.c:1686`）：它的拒绝条件
  只在 `!t->dispatching` 时成立，也就是说一个仍处于 `dispatching` 的任务
  是允许被别的 CPU unpick 的（随后清掉 `dispatching` 与 `owner_cpu`）。

  这已经不是机械前移，而是要重新设计 `sched()` 的取锁时序，因此本轮未改。
  这里必须把两件事分开：**"在全局自旋锁里跑一个理论上无界的 `for(;;)`"仍然是
  应当消除的潜在隐患**，有并发 PTE 修改时它没有次数上界；**但它不是实测的
  争用成因**。等待方一侧的度量（`max=`）做完之后，"成因"这个问题的答案
  既不是 rdcycle 封装、也不是继续泛读代码，两条结构性事实直接把它定死：
  (1) 全树 69 处 `proc_lock` 获取**全部**走 `spin_lock_irqsave`，**没有一处**
  用关中断之外的 `spin_lock`，所以持锁期间不可能被时钟中断抢占；
  (2) 持 `proc_lock` 的临界区里**没有任何 `sched()`/`proc_yield()`**
      （唯一命中是注释），所以也不可能主动让出。
  既然"被抢占"被排除，剩下只能是**持有者真的在临界区里长时间执行**，
  而 `max=` 指出的正是"次数极少、单次极长"。到此为止是站得住的。

  但"因此成因就是持锁临界区里的全系统遍历"这一步不成立，本轮已撤回。
  那样推出的 4 处候选（`kernel/mm/mmap.c:39`、`proc.c:200`、`kernel/proc/loadavg.c:53`、
  `cg_mem.c:121`）经核对在实测负载下基本不会执行：`net_stress_test` 的
  `read()`/`write()` 是套接字调用，够不到 `mm_sync_shared_dirty_for_vnode()`；
  `proc_get_vm_stats()` 的唯一调用点是 `procfs_render.c:435` 的 `PF_MEMINFO`，
  而门禁只 cat `/proc/a20/perf` 与 `lock_contention`，不读 meminfo。

  观测窗口确有缺陷，且已修好：锁计数器自启动起累计、没有 reset 入口，
  所以此前"压力测试下的争用"解读建立在一个未分离的窗口上。修法不需要改内核，
  门禁改为在压力前后各读一次 `/proc/a20/lock_contention` 并求差
  （`proc_lock` 注册在 `proc.c:365`、打印顺序在 `lock_counters_format()` 里
  按注册序，故每块的 `^proc: ` 行就是天然分隔符）。
  **修完后的实测直接否掉了"极值发生在引导期"这个猜测**（两次独立复现）：
  引导期 2544 次争用/227 万自旋（均值 891），压力期仅 103 次争用/126 万自旋
  （均值 12202）；另一轮 1786/284 万（均值 1590）对 62/80 万（均值 12867）。
  即压力期每次争用比引导期重 8–14 倍，超长持有确实发生在压力期。

  "全系统遍历"这一类成因到此可以彻底否掉（不只是"候选点冷"）：全树
  `proc_first_task_locked()` 的调用点只在 `kernel/mm/`（`oom.c`、`cg_mem.c`、
  `mmap.c`）与 `kernel/proc/`、`kernel/fs/`（`procfs.c`、`cgroupfs.c`）里，
  `kernel/net/` 零命中，而压力负载是纯网络，这些点根本不在调用链上。
  把两个窗口的逐调用点自旋差算出来后，压力期的头部等待者是
  `proc_clone_impl+0x64`（163315 自旋，仅 2 次争用且 `max` 等于总和，即
  **单次持有约 1.6ms**）、`sys_wait4+0x36`（70750）、
  `proc_task_first_entry+0x30`（92972）；而引导期头部是
  `proc_sched_tick+0x30`（1576889），这正是此前累计值被引导期主导的来源。

  但要点明一个限制（本轮自查后修正了写法）：这些标签是**等待方**（发起
  acquire 并自旋的一方），不是持有方。`proc_clone_impl` 自旋 16 万次只说明
  "有别人长时间持锁"，本身不能说明是谁。
  此前这里写的是"等待方数据在原理上无法指认持有者"，这句话**只对了一半**，
  且该修正本身也需要随实现更新：`spinlock_t.owner_ra`（`lock.h` 的
  `spinlock` 结构）在 acquire 时写入持有者 RA，到 `spin_unlock()` 才清，
  而现成的 `[LOCK-STALL]` 诊断本来就同时打印 `owner_ra` 与 `waiter_ra`。
  但这条路径**只在 `CONFIG_DEBUG_LOCKS=1` 的调试构建里存在**，且只有真正
  发生争用的 acquire 才写：默认构建把 `owner`/`owner_ra` 留空，因为这两个
  字段只服务于 stall 诊断与 `owner == cur` 自死锁判定，而把它们写在全部
  ~920 个 acquire 点上会给无争用快路径增加一次存储。**因此在默认构建上，
  持有者身份并不是"对等待方始终可见"**——要靠 `owner_ra` 归因持有方，
  必须开 `CONFIG_DEBUG_LOCKS` 重编译。引用本文时不要再按行号定位
  `lock.h`：相关字段与诊断的行号已随上述改动漂移。

  但"不依赖时钟的持有方归因"这条路本轮已试过并否决，两次尝试都失败：
  (1) 只在 2^16 处采样一次、把剩余自旋全部记到当时持有者头上。锁在多个短暂
  持有者之间反复易手时，会把整段等待算在一个恰好当时在持锁的旁观者头上；
  (2) 改成按窗口（每 2^16 自旋记一次当前持有者）后，按窗口计费**本身不含
  时长信息**：无论持有者是持有了 1µs 还是 1ms，每个窗口都记 65536 自旋，于是
  该指标只能回答"谁在争用时持锁、按频次加权"，**回答不了"谁持锁很久"**，
  而后者才是本问题要问的。实测 4 核实测：260 万自旋里只有 13 万（~5%）能归因，
  且只有 2 个调用点各命中恰好一个窗口（`hmax` 恒等于 65536 = 窗口大小，
  作为"最长持有"完全退化）；被点名的 `proc_sched_tick+0x30`、`idle_loop+0x36`
  都是高频但短暂的持有者。第一版点名的 `proc_task_first_entry+0x30` 更是
  假象：它是**空闲任务的 trampoline**（`proc.c:423`、`:454` 里被写进初始
  `ra`），根本不是取锁函数。
  故该归因已整体撤回（与 rdcycle 同样处理）。**结论：指认"长持有者"本质上需要
  时长量测，即需要真实时钟（真机 PMU / `-icount`），在 QEMU 下没有可靠替代。**
  本轮准确结论是：**成因仍未定位**，仅收窄到"压力期 fork/clone 与 wait-for-child
  附近存在毫秒级持有"；要指名持有者，需上真机。
- `pid_max` 默认 32768，**与 Linux 默认值一致，且可通过
  `/proc/sys/kernel/pid_max` 运行时调整**（`procfs.c` 的 sysctl 写路径
  已接线）。因此它不是缺陷；确有需要的部署自行调高即可。
  （本文件早期版本把默认值列为待修项，是错的：默认值本身与 Linux 相同，
  上调反而会造成与 Linux 的行为差异。）
- `MAX_PROCS`（consts.h，MCU 32 / 托管构建 4096）是**死常量**：除自身
  `#define` 外全树零引用，任务结构是 `kcalloc` 动态分配的。它会误导读者
  以为存在 4096 的进程上限，应删除。
- **`RLIMIT_AS` / `RLIMIT_NPROC` 本轮已实现并强制**（此前完全缺失，
  单进程可耗尽宿主机内存）。
- **单核门禁不再一统天下（2026-10 起）**：此前所有 `smoke-*` 门禁硬编码
  `-smp 1`，单核通过不证明 SMP 正确性。现在 vfs-stress 工作负载有了
  `smoke-vfs-stress-smp2`（NR_CPUS=2，已接入 CI smoke job）与
  `smoke-vfs-stress-smp8`（NR_CPUS=8，本地资源门禁 `-c 8`）两个真多核
  变体（与既有的 `smoke-mm-fork-exec-race` 同形），其余门禁仍是单核——
  把整套门禁矩阵 NR_CPUS 化仍是待办。
- 全部性能数据来自 QEMU TCG 模拟器，无真机基准。

## 五、可观测性

本轮把「静默说谎」的观测面改成了真实测量（详见
[../security/hardening.md](../security/hardening.md) 的诚实性原则）：
`/proc/<pid>/io`、`/proc/loadavg`、`/proc/pressure`、`getrusage`、
`TCP_CONGESTION` 现均为真实值。

`TCP_CONGESTION` 这一条在 `0d42ca758` 之后才真正成立：此前它接受 `"cubic"` 并返回
成功，而本树**没有** CUBIC，`getsockopt` 也不回真实算法名。现在未知名返回
`-ENOPROTOOPT`，回读的是真正生效的算法。同一原则下，`SO_SNDBUF` / `SO_RCVBUF`
也从"接受然后忽略"变成真的生效值（`aacce4dcc`）；而**驱动能力位里那两个校验和
卸载位刻意永不置位**（`driver_class.h:145-146`），因为 vendored 的 lwIP 2.2.2 没有承载
该握手的 flag，置位会让 lwIP 去验设备没算的校验和——`/proc/net/stats` 里那两行因此
**恒为 `off`，且这是设计结果不是未完成项**。

仍然缺失或不可用：

- 无硬件 PMU：`perf_event_open` 只有 software event，无 mmap ring
  buffer（只能 read），无 group leader → `perf top` 类采样不可用。
- 无 ftrace / kprobes / tracepoints。`bpf(2)` 是真的 eBPF 解释器，
  但只有 1 个扩展点、所有 map 命令返回 `-EOPNOTSUPP`、无 kprobe/perf 挂载
  类型，**不能用于可观测性**。
- 无 audit 子系统、无 syscall 审计、无文件完整性度量、无安全启动/dm-verity。
- 无日志外发（syslog/journald 集成）。
- seccomp 机制可用但**树内无任何服务安装 filter**。

## 六、可靠性

- panic 是关机不是重启（`panic.c:89` 调 `firmware_shutdown()`），
  失败则 `arch_halt()` 死循环。
- 无跨 CPU stop IPI（`panic.c:19-21` 自述）：其他核继续跑到自己 panic，
  输出被丢弃。
- panic 文本不进 klog 环（直接调 `printf`/`uart_puts`），控制台卡死时
  dump 全丢。
- kexec 只有 staging，**无执行后端**（`sys_proc.c:760-764` 返回 `-ENOSYS`）
  → 无 kdump。
- 无 watchdog（仅 STM32 MCU 的 IWDG），无 softlockup/hung-task 检测
  → 内核挂死即永久挂死。
- **无 A/B 分区、无 dm-verity、无回滚**。升级 = 重刷。
- 无节点级自愈：没有 fencing、quorum，也没有监管 `svcmgr` 自身的东西，
  它挂了所有服务变孤儿。

## 七、真机与虚拟化

- **PCI bridge 遍历已实现（覆盖有限）**：`pci_scan_current()` 现改为 worklist
  遍历，读 header type 1 的 secondary/subordinate bus 寄存器并递归跟进，
  带 `visited[]` 防环。修复前 riscv64 的 `(0,1)` 范围只看得到 bus 0。
  **但自动化证据只有回归守护**：`smoke-pci-bridge` 在 x86_64 上跑，而
  x86_64 板级传入 `0,255`、loongarch64 传入 `0,127`，旧线性扫描本就能覆盖
  这些 bus，所以该门禁在修复前后同样通过。它锁住的是不回归，不是修复本身。
  真正体现价值的是 riscv64 `(0,1)` 与 virtualbox-aarch64（固件分配范围），
  本 QEMU 构建无法驱动这两条路径（riscv64 virt 无 PCIe controller）。
- **MSI-X 已实现（仅 x86_64 真实投递）**：`kernel/drivers/bus/pci_msix.c`
  提供与协议无关的 MSI-X 层，`pci_bus.c` 把 virtio transport 接上，
  e1000e 与 virtio-blk 已实测通过 `smoke-msix-x86_64`。能力表位置解析
  同时支持 Vector Control（PCIe 编码，BIR `3:1` + 偏移 `31:12`）与
  Message Address Lower（pre-PCIe 编码，BIR `2:0` + 偏移 `31:3`）——
  `-kernel` 引导没有固件写前者，必须读后者。**残留**：只有 x86_64 实现了
  `arch_msix_message_address()`/`arch_msix_vector_setup()`，其余架构干净
  拒绝并退回 INTx/轮询；无 IRQ 亲和性与 per-CPU 目标字段，向量窗口钉死
  在 boot processor 的 `0xD0..0xF0`；e1000e 只验证到表被正确解析并 arm，
  网卡无流量故未实测投递（virtio-blk 一路是端到端的）。
- INTx 路由硬编码 QEMU q35：`x86_64/trap/irqchip.c:297-311` 只认
  host bridge `0x29c08086`，否则 `return -1`。代码注释自述需要
  ACPI `_PRT` 与 PIRQ link 编程。
- ECAM 基址是编译期常量（仅 virtualbox-aarch64 从 MCFG 读）。
- ACPI 基本没有：只有 RSDP + MADT + HPET + TPM2。**无 DSDT/AML 解释器**
  → 电源管理在架构上就不可能。
- 无真 RTC：wall clock 从编译期常量 `A20_BUILD_UNIX_TIME` 起步。
- 无 paravirt clock：KVM 检测只用于决定是否信任 TSC，无 kvm-clock
  兜底 → KVM 下 guest 存在时钟漂移风险。
- 无 virtio-fs/DAX（**服务器存储共享路径完全缺失**）、virtio-rng、
  virtio-console、virtio-balloon。
- 好的一面：RISC-V IOMMU 是 755 行真实现（fail-closed）；x86_64 TSC 校准
  完整（CPUID 0x15/0x16 + PIT + invariant-TSC）；idle 路径是真实架构停机
  （`sti;hlt` / `wfi`）而非忙等。

## 七点五、2026-10 内核核心收敛（feat/kernel-core-scalability 分支）

以下条目已在本分支落地（各提交含完整论证与验证入口；运行门禁于当前提交
复验：`smoke-vfs-stress`、`smoke-abi-linux`、`smoke-mm-stress`、
`smoke-vfs-stress-smp2`、`smoke-vfs-stress-smp8` 均 PASS）：

- **vfile 全局表锁分片**（`kernel/fs/file.c`）：fd 解析热路径原来在
  `g_file_lock` 单锁下串行（server-readiness 早期版本未把它计入热点排行，
  是观测盲区——单核门禁下它永远显示 0）。现按 gfd 哈希分 128 桶锁 +
  独立分配锁；全部桶锁登记进 `/proc/a20/lock_contention`
  （LOCK_COUNTERS_MAX 64→192），可测而非假设干净。
- **per-vnode 缓冲写锁**（`vnode_t.write_lock`）：替代 64 桶全局写互斥，
  两个哈希冲突的无关文件不再互相阻塞。
- **mount 表稳定指针**：umount 搬移内联数组导致 `vnode->mnt` 指向错误
  mount 的正确性 bug 已修（堆分配 + 命名空间墓园）；注意 §三 的
  `pivot_root` 前置件（root/cwd 路径字符串 → mount 引用）仍是独立待办，
  本修复只消除了指针失真，没有引入 mount 树。
- **EventQ 反向索引 256 桶分锁**、**路径查找 errno per-task 化**
  （`vfs_lookup_errno()`）、**slab per-CPU 对象数组**、**timekeeping 读
  路径 seqlock 化**。
- **kswapd 式后台回收**（`oom_kswapd_thread`，`/proc/a20/oom` 暴露
  `kswapd_*` 计数）：回收不再全部同步发生在分配最坏路径。
- **发布流水线接入 guest 门禁**：release.yml 新增 smoke job，Release 创建
  以 `smoke-abi-linux`/`smoke-vfs-stress`/`smoke-mm-stress` 通过为前提。
- **PCI MSI-X**：`kernel/drivers/bus/pci_msix.c` + 能力表位置双编码解析 +
  x86_64 LAPIC 向量/LVT 编程 + virtio transport 接入（`msix_prepare`/
  `msix_arm`/`msix_teardown`）+ e1000e 接入；门禁 `smoke-msix-x86_64`
  断言 `[VIRTIO-BLK] MSI-X delivery on vector 208`，该行由中断处理程序
  在首次消息中断时打印，且已做反向验证（把消息地址改回错误形式，门禁
  只缺这一条而失败）。详见 `docs/drivers/guide/pci-and-virtio.md` 的
  「MSI-X」一节。
- **server world 声明层**：`packages/world/server.world`（dropbear/chrony/
  busybox syslogd+crond）+ overlay init + `server-riscv64` 实例；
  声明过 `check-instances` 门禁，端到端组装与 SSH 登录验证未做（见 world
  头注），不声称可用。

仍属本文件记录且**未**在本分支处理的：`proc_lock` 超长持有成因、
其余 5 个 namespace（net/cgroup/time/uts/ipc）与 `nsproxy`、
ACPI `_PRT`、MSI-X 的 IRQ 亲和性与非 x86 平台实现。

其中 **conntrack/NAT 已在 `feat/net-strengthening` 落地**（见 §二 与 §八），
lwIP 全局锁分片推进到阶段 E（`g_lwip_lock` 本身仍是全局锁）。

## 八、阻塞项排序

| 级别 | 阻塞项 | 理由 |
|---|---|---|
| ~~P0~~ | ~~收包内存模型~~ | **已修**（`feat/net-lanes`）：两级暂存内联化，`net_socket_t` 1.05 MiB → 30 KiB，`net_msg_t` 68 KiB → 1368 B，锁内每包 memset 65535 B → 200 B，并由 `_Static_assert` 钉住 |
| ~~P0~~ | ~~`g_net_lock` 分片~~ | **已完成**（`2f17a5ba8` + 阶段 E `7c7a4d7c8`）：`g_net_lock` 已删除，socket 表分片为桶锁，桶锁又被 `net_socket_t.lock` 取代，现在只管 slot 表。SERVER 档一个桶是 512 个 slot，所以阶段 E 之前一个 socket 的 `recv` 要和同桶另外 511 个互斥。**残留**：`2f17a5ba8` 自陈的三项运行期验证（引用计数不漏不重、`LOCK_COUNTERS_MAX` 注册预算、`-ENOTCONN` 窗口）仍开放，清单在 `docs/measured/impl-notes-net.md`；合并后全量回归与 ASAN + SMP 压测**未做**；net 锁一侧**没有**运行期探针（见下一行） |
| P0 | net 锁一侧的运行期探针 | `LWIP_ASSERT_CORE_LOCKED()` 只覆盖 `g_lwip_lock` 一把。没有"当前 CPU 是否持有期望的 socket 锁"的判据，`net_sock_lock2()` 的地址升序与"至多两把"、以及"桶锁不得在 socket 锁之下取得"只由代码评审把关，lane claim 一侧同样没有。补它需要 per-CPU 持锁集合跟踪（`CONFIG_NET_SOCK_ASSERT`），本轮未做 |
| P0 | lwIP 全局锁分片 | **本轮推进了三步，但仍是最大未做项。** 阶段 C（`5ea06a786`）落「当前 lane」上下文 + memp 的 lane 索引**骨架**（内存未分片）；阶段 D（`f6f327b96`）把协议输入搬出中断上下文、按 lane 分发；阶段 E 收窄 socket 侧锁。**`g_lwip_lock` 本身至今仍是一把全局锁**，所以"各 CPU 各自处理 socket"只成立在"谁处理"这一层，不成立在"谁能同时处理"。持锁方一侧的时长在 TCG 下拿不到，本文件已因此撤回过一次结论；spin 归因已修正（`spin_lock_at` 的 site 计数曾与 acquire 数重复）；4 核实测 4 次争用/83 万自旋，`max=472365`，即同样是少数几次长持有而非稳态高频。已确定的前提是：热路径要靠 socket 单一所有权避免全局 PCB 链表遍历（PCB 链表按 lane 分桶已在阶段 B 落地），仍需给 `tcp_active`/`tcp_bound_pcbs`/`udp_pcbs` 定 per-lane 所有权与冷路径的死锁边界 |
| P0 | 多 lane 的 2% connect flake 仍未定位 | 两轮放大实验（`CONFIG_NET_RACE_DELAY_US` 200 / 2000 µs）共 78 次连接 0 失败，全程 `violations=0 sites=0`、`PCB_SANE` 零命中。**未复现、未定位，未声称已修**。统计边界：72 次放大样本在 2% 下期望 1.6 次失败，观测 0 次的概率约 23%，**不构成"已消失"的证据**。配方见 [net/net-lanes.md](net/net-lanes.md) |
| ~~P0~~ | ~~PID ns + userns + `pivot_root`~~ | **已完成**：`pivot_root`（`smoke-pivot-root`）、PID ns（`smoke-pidns`）、userns（`smoke-userns`）均已落地。残留：无 `nsproxy`、capabilities 仅 15 个子集、mount 共享子树传播未实现 |
| ~~P0~~ | ~~ext4 可写 journal + 崩溃注入测试~~ | **已完成**：运行时 metadata 写入走 JBD2 ordered commit，commit 指针按事务大小推进，数据 checksum 记在 descriptor tag 内（不再写进块尾污染 bitmap），挂载时以日志 `s_start` 为权威判据；`make smoke-ext4-journal` 做四点崩溃—重启往返并用 `e2fsck -fn` 双向把关，x86_64/riscv64/aarch64/ppc64le/loongarch64 五架构 5/5 PASS |
| ~~P1~~ | ~~conntrack + NAT~~ | **已实现**（`f48a8f5f2` / `0d9d0885f` / `4b4472d17` / `1475cd9dd` / `dd4e5678a`）：五元组哈希表（按档位 64/256/1024）、NEW/ESTABLISHED、按状态分开的空闲超时、满表 LRU、SNAT/MASQUERADE（output）与 DNAT（input），RFC 1624 增量校验和，门禁 `make smoke-netfilter-nat`（hostfwd 18081 → guest 18082，宿主探针收到回显）。**残留**：无 ALG / 无 ICMP 跟踪 / 不做分片 NAT；**端到端门禁只覆盖 DNAT**，SNAT/MASQUERADE 只有解析器、`/proc` 规则与单元级证据，`masquerade` 的 DHCP 换址行为未在真机验证；LRU 与超时两条路径只有 `/proc` 计数（`ct_evicted` / `ct_timeout`），没有门禁真的跑满。详见 [net/conntrack-nat.md](net/conntrack-nat.md) |
| P1 | IPv6 地址路径 | AF_INET6 的 **入站 TCP 已通**（`c34ddd7f8`），`/proc/net/tcp6` / `udp6` 按 v6 布局渲染。但 **rtnetlink 组播只覆盖 IPv4 地址组**，本树没有 IPv6 地址写入路径，所以没有 IPv6 地址变更事件 |
| P1 | 接收缓冲自动调优 | `SO_RCVBUF` / `SO_SNDBUF` 已不再是 no-op（`aacce4dcc`），但**没有自动调优**：没有 `tcp_wmem`/`tcp_rmem`、没有内存压力反馈、不从实测吞吐调整，依赖 Linux 那种增长的调用方拿不到。且抬高 `SO_SNDBUF` 只在下一条连接生效。窗口缩放已解除协议上限，池与档位仍是硬边界 |
| P1 | 嵌入式档仍装不进 20 KiB | 两块静态数组已纳入档位并由断言钉住（`g_pkt_ring` 24640→2064，`g_netif_state` 12672→1120，合计 37312→3184 B，−91.5%），但 8 个 `net_socket_t` 单是 32768 B（1.60 倍部件）就越界，lwIP 堆 16407 B 是 80%。剩下的账全在 profile 之内：降 `MEM_SIZE`/池/`MAX_SOCKETS` 与关 IPv6 **未做**——那会改变 EMBEDDED 档的协议能力，属产品决定 |
| P1 | 扩大接收缓冲（pbuf 池 / 零拷贝收包） | 窗口缩放已解除协议上限，现在卡在 384 KiB pbuf 池 |
| ~~P1~~ | ~~MSI-X~~ | **已完成（x86_64）**：能力解析 + LAPIC 编程 + virtio/e1000e 接入 + `smoke-msix-x86_64` 端到端投递断言。残留亲和性与非 x86 实现 |
| P1 | ACPI `_PRT`（bridge 遍历已完成） | 真机服务器的准入条件 |
| P1 | kdump 执行后端 + panic 改为重启 | 故障后能否自动恢复 |
| P1 | 内核抢占 + RT 限流 | 实时性与尾延迟保证 |
| P2 | 硬件 watchdog + A/B 分区 + dm-verity | 无人值守与安全更新 |
| P2 | 硬件 PMU + ftrace/tracepoints | 生产环境可诊断性 |
| P0 | `proc_lock` 超长持有的成因未定 | **只证伪了一半**。已证伪"被抢占"（成立）：全树 69 处 `proc_lock` 获取全部走 `spin_lock_irqsave`，无一处关中断之外；持锁临界区内无任何 `sched()`/`proc_yield()`。所以持有者确实在长时间执行。但**"成因类别已确定"这个说法不成立，本条已撤回**：先前据"持锁临界区里做全系统遍历"推出的 4 处候选，经核对在实测负载下基本不会执行：`net_stress_test` 的 `read()`/`write()` 是套接字调用，够不到 `mm_sync_shared_dirty_for_vnode()`；`proc_get_vm_stats()` 的唯一调用点是 `procfs_render.c:435` 的 `PF_MEMINFO`，而门禁只 cat `/proc/a20/perf` 与 `lock_contention`。更关键的是计数器自启动起累计、没有 reset 入口，所以那个 905K–136 万自旋的单次极值可能发生在引导期而非压力期。结论：成因仍未定位。**观测窗口缺陷已修**：`/proc/a20/{perf,lock_contention}` 现有 `reset` 写入口（`feat/net-lanes`），门禁可前后各读一次求差；`lock_counters_reset()` 连 `contended_max_spins` 一起清零，因为 reset 之后要回答的是"本窗口内的最大值" |
| P2 | virtio-fs/DAX | 共享存储 |
| P2 | 真 RTC + paravirt clock | 真机时间正确性 |
| P3 | NUMA、热管理、C-states | 规模与能效 |

### server world 的实测状态（2026-10）

`packages/world/server.world` 此前标注为"从未执行过组装"。现已推进到**镜像能装出来、
并且在 guest 内启动到 shell**：

```
make ARCH=riscv64 BOARD=qemu-virt-riscv64 \
     ALPINE_MIRROR_ROOT=https://dl-cdn.alpinelinux.org/alpine \
     image-world PKG_WORLD=server
```

22 个 Alpine 包（busybox、dropbear、chrony、ca-certificates 及依赖）全部装入 staging
并被 `mkfs.ext4` 打包；把这张镜像作为第二块 virtio-blk 盘挂上启动后，stage-2 init
正常 chroot，dropbear 打印主机密钥。**world 清单 → apk 求解 → overlay 装配 → mkfs
→ chroot** 整条链路成立。

两处环境相关的坑，都不是仓库逻辑问题：

- 默认 USTC 镜像源在本环境返回 **403**，需换官方源；`ALPINE_MIRROR_ROOT` 在
  `tools/targets-rootfs.mk` 中是 `?=` 赋值，可从命令行覆盖。
- `mkfs.ext4 -d` 曾报 `do_write_internal: 权限不够 while opening "bbsuid"`。这是
  `busybox-suid` 的 `bbsuid` 需要 `mknod`、而 fakeroot 只伪造属主不提供 `CAP_MKNOD`
  所致（单独 `fakeroot chown 101:101` 正常，可确认不是 fakeroot 本身坏了）。已从
  world 清单去掉 `busybox-suid` 绕开，见 `6a1ab8f8`；宿主上有 root 时该包可以放回。

#### 让这张镜像真的能被登进去，需要两件事

1. **镜像里必须有认证路径。** 镜像不含密码——密码一旦烤进镜像就等于永久泄露并随镜像
   复制扩散——所以只能烤公钥。新增 `SSH_PUBKEY`：

   ```
   make image-world PKG_WORLD=server SSH_PUBKEY=~/.ssh/id_ed25519.pub
   ```

   见 `6702613c`。不给 `SSH_PUBKEY` 时镜像不带任何 `authorized_keys`，dropbear 照常
   启动，但没人能登进去。

2. **内核命令行必须选 `a20.tcpmode=lwip`。** 默认的 `tcpmode=fast` 里 `listen()`
   直接丢掉已绑定的 pcb、从不把 listener 放进 lwIP，于是该端口在协议栈里根本不存在，
   slirp 发来的 SYN 被回 RST。这与认证无关，发生在认证之前。

`tcpmode=lwip` 确实修好了这一层：`/proc/net/status` 里 `tcp_listen=1`（真实 LISTEN pcb
存在），且 `smoke-net-accept` 的 lwip 那一趟 **PASS**
（`TCP_ACCEPT_TEST: PASS port=12346`，`net_accept_staged=1`、`net_accept_queued=1`）。

#### 入站连接的 double free：已定位并修复

- **入站连接能被 accept，但随即 double free 打死内核。** 最小复现（server world，
  `a20.tcpmode=lwip`，宿主经 hostfwd 连入）：

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

  `mem_free` 这条断言只在 `mem.c:639` 命中，即"该块已被标记为未使用"；而本分支
  `MEMP_MEM_MALLOC=1`，memp 各池的元素都出自 `mem_malloc`，所以这等价于**某个 memp
  元素被释放了两次**。

  **根因不是 pcb 的引用计数算错，而是"把 pcb 交给 bottom half"和"pcb 可以安全地留在
  lwIP 里"被当成了同一件事。** `lwip_tcp_accept_cb()` 返回 `ERR_OK`（等价于告诉 lwIP
  这个 pcb 归应用所有），却**没有给它装任何回调**，于是 `pcb->recv == NULL`。若对端在
  bottom half 跑完 `net_inet_tcp_apply_options()` 之前先发来 FIN，lwIP 会走到它自己的
  `tcp_recv_null()` 兜底路径，其 `p == NULL && err == ERR_OK` 分支直接 `tcp_close(pcb)`
  并把 pcb 释放掉——而 accept stage 槽位还指着这块已被释放的内存。bottom half 随后
  拿到这个悬垂指针，把它交给新建的 child socket，teardown 再 `tcp_abort()` 一次，
  第二次释放就撞上 `mem_free` 的断言。

  修复（`80c20884`）把 accept stage 从"一个裸 pcb 数组"改成自带上下文的每槽位结构
  （`net_accept_stage_slot_t`），并在 stage 时就装上 `lwip_tcp_stage_{recv,sent,err}_cb`：

  - staging 期间收到的数据以 `pbuf` 引用挂在 `c->pending` 上（`pbuf_free()` 只在
    `g_lwip_lock` 下安全，所以只能在 err 回调或 bottom half 里释放），被采纳时搬到
    child 的 bh_ring；
  - staging 期间到达的 FIN 只置 `c->fin`，绝不动 `dead`，并在采纳时重放到 child 的
    `bh_closed`，于是"握完手立刻 FIN"的请求不再落到兜底路径上；
  - `lwip_tcp_stage_err_cb()` 置 `dead` 并在 `g_lwip_lock` 下清 `pending`，bottom half
    据此跳过这个槽位；
  - 采纳前**先把 pcb 重新 `tcp_arg()` 到 child 并清空槽位，再推进 `tail`**——槽位地址
  就是 pcb 的 `callback_arg`，只要 `tail` 没过 i，生产者就不可能复用槽位 i，这个顺序
  消掉了"生产者还在往这个槽位投递事件"的窗口；
  - listener 拆除时（`net_tcp_close_pcb()` / `net_tcp_drop_pcb()` /
    `net_inet_socket_destroy()`）统一 `net_inet_accept_stage_purge()`，此前已建立的连接
    会被静默泄漏。

  同一条路径上还修了两个独立缺陷：`lwip_tcp_recv_cb()` 在 bh_ring 满时既 `pbuf_free(p)`
  又 `return ERR_MEM`，而 `ERR_MEM` 的语义是 lwIP **并没有**接管这个 pbuf（`tcp_in.c`
  把它存进 `pcb->refused_data`，`tcp_process_refused_data()` 还会再交回来），所以这本身
  就是第二次释放；现在改成先用 `bh_ring_reserve()` 整体判定、要么全进要么全不进。另外
  `pbuf_cat()` 会自行为被拼接的链取引用，因此"留下回调交给我的 pbuf"的正确写法是
  直接 `pbuf_cat(c->pending, p)`，多写一次 `pbuf_ref()` 会多数一。

  验证：`make smoke-net-accept` 的 lwip 那一趟 PASS（`TCP_ACCEPT_TEST: PASS port=12346`，
  `net_accept_drop=0`、`net_bh_overflow=0`、`net_alloc_fail=0`，无 panic），此前同一份
  产物必 panic；经 slirp 打到 guest 8080 的入站连接不再触发断言；server world 里
  dropbear 在 22 端口可从宿主端到端登录。

#### fast 模式门禁发红：是测试自身的单位错误，不是内核缺陷

- **`smoke-net-accept` 门禁红在 fast 模式**：`TCP_ACCEPT_TEST: FAIL port=12347 (client=-1)`。
  内核并没有错：`user/cmds/net/tcp_accept_test.c` 的客户端重试循环把 `waited` 按
  `CONNECT_RETRY_US`（微秒）累加，却拿它去比 `CONNECT_BUDGET_MS`（4000，毫秒），所以
  `waited` 从 0 加到 20000 就已经越界，**循环体只跑一次**，客户端只发起一次 `connect()`，
  而它恰好要跟 fork 出来的服务端 `bind()`+`listen()` 抢跑。谁先谁后不确定，于是表现为
  "fast 模式 1/4 概率挂"。注释里写的意图（"长到不会在 4 核 guest 上误触发，短到卡死时
  几十秒内失败"）显然是想要 4 秒的重试预算，实现没兑现。

  定位证据是在 `-smp 1`（串口无并发交错，输出不可能丢）下跑多轮：每个失败端口只打印
  **一条** `connect()` 入口的调试行，而不是注释所暗示的 200 次重试。改成统一微秒预算
  （`CONNECT_BUDGET_US`）后，同一 boot 内连跑 8 轮 8/8 PASS，且 socket 计数每轮回到基线，
  无泄漏；关掉全部调试输出后 `make smoke-net-accept` 连跑 6 次全绿。

  两种模式现在给出同一个可观测结果，这条不再是例外项。

## 九、推荐的第一批动作
按「改动小、风险低、避免真实事故」排序：

1. 补一个挂 `ich9-ahci` 的门禁，让 AHCI flush 获得运行验证。
2. ~~崩溃注入测试基础设施~~ —— **已完成**，形式是 JBD2 提交序列内的定点
   panic（`/proc/a20/journal` 下发注入点）+ 同镜像重启 + 宿主 `e2fsck` 比对，
   见 `make smoke-ext4-journal`。仍未覆盖的是"到点就死"的粗粒度形态
   （在写盘路径上随机取一个指令位置 kill -9）；当前覆盖的是语义上真正有
   意义的边界点。
3. 引入真机基准入口；当前所有性能结论都来自 TCG 模拟器。

以下两项曾在本清单里，现已完成，不再是待办：

- `/proc/pressure` 曾是单文件布局，systemd 按 `cpu`/`memory`/`io` 目录读会
  失败。该布局已改为目录，见 `kernel/fs/procfs/procfs.c` 的
  `pressure_entries` 与 `procfs_render.c` 的 `PF_PRESSURE_*` 分支。
  `mem`/`io` 渲染为结构性的零（设备 I/O 全部同步，没有可采样的 stall 状态，
  见 `kernel/core/psi.c` 的说明），不是伪造的数字。
- `MAX_PROCS` 死常量已删除，全树无残留引用。
