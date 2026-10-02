# A20OS 服务器就绪度评估

最后核实：2026-09（`server-hardening` 分支）。下文按服务器部署视角列出 A20OS 的
当前能力边界、已知的结构性限制，以及按严重度排序的阻塞项；每条都给出文件位置，
便于自行复核。

运行类结论以当期提交为准；历史记录见 [archive/](archive/)。

## 一句话结论

A20OS 已经是一个认真的内核，但**当前形态是「QEMU 上的桌面/研究内核」**，
不是「服务器内核」。差距不在功能数量，而在三个结构性问题：网络数据面
被单一全局锁串行化、容器隔离的前置件（PID/userns/pivot_root）缺失、
真机 PCIe 可用性受硬编码 QEMU 假设限制。

网络这一项最近有实质进展：收包路径的**内存模型**已经被认定为比全局锁更根本的
瓶颈并修掉了（见第二节第 0 条），此前把归因指向读路径是错的。当前剩下的最大
单项是 `g_net_lock` 分片，其次才是 `g_lwip_lock` 本身。需要强调的是：所有性能
数字都来自 QEMU TCG，`lwip` 自旋计数在该环境下噪声极大（同一负载四次运行跨越
0 到 920024），因此本文件不再以自旋数量作为任何结论的依据。

在下面的阻塞项收敛之前，把数据库或不受信任的工作负载放上去是不安全的。

## 一、存储持久性

### 已达成

`fsync()` 现在真正到达稳定介质。`block_dev_t` 有可选 `flush` 原语，
`bcache_sync_common()` 在写完数据后、缓存锁之外调用它。实现覆盖
virtio-blk（`VIRTIO_BLK_T_FLUSH`）、loop（转发 backing file 的 fsync）、
AHCI（`FLUSH CACHE EXT`）。

验证：`make smoke-fsync-durability`，断言 `block_flushes` 计数器确实增长。

### 仍缺

- ext4 不是日志文件系统。`kernel/fs/diskfs/ext4_journal.c` 明确只做
  挂载时回放，然后把日志标记为空并清除 `RECOVER`。RW 挂载后没有 journal
  提交、没有 ordered 模式语义，写回途中崩溃可留下 journal 本可避免的
  元数据/数据不一致。对需要崩溃一致性的数据库，这是**硬阻塞**。
- AHCI 路径仅编译验证。`ahci.c` 位于 `CONFIG_AHCI` 之后，树内没有任何
  实例挂载 AHCI 控制器。补一个挂 `ich9-ahci` 的门禁是缺失的一环。
- 无断电/崩溃注入测试基础设施，因此上述 journal 改造无法被验证。
- 无 RAID、无数据校验和、无快照/CoW、无 fs-verity。
  `crc32c` 只用于校验 JBD2 回放日志，不覆盖常规文件数据。

## 二、网络

### 已达成

可用的 IPv4 无状态包过滤（`kernel/net/netfilter.c`），控制面在
`/proc/a20/netfilter`。启动时无规则、默认 accept，未配置机器行为不变。
验证：`make smoke-netfilter`。

### 结构性限制（按严重度）

0. **收包路径的内存模型本身就使服务器不可行，且它才是锁争用的主因。**
   这一条排在最前，因为它比全局锁更根本，而此前的排序把它完全漏掉了。
   当时 `net_bh_event_t` 与 `net_msg_t` 各自内嵌 `data[NET_MAX_PAYLOAD]`
   （65535 字节），于是：

   - `sizeof(net_socket_t)` 约 **1.05 MiB**，其中 16 项 ring 占 1.0 MiB。
     socket obj_cache 留活 128 个对象，最坏**滞留约 134 MiB**。
   - `sizeof(net_msg_t)` 约 65.8 KiB，**超过 `SLAB_MAX_OBJ`**，于是
     `mm/slab.c` 对每条消息走 buddy 并按页取整：一条 100 字节的 datagram
     实际消耗 17 页 = 68 KiB。`NET_MAX_QUEUE` 是 128，单 socket 排队载荷
     上限约 8.5 MiB。
   - 每个 1460 字节的 TCP 段付出约 **136 KiB memset + 三次拷贝**，其中绝大部分
     在 `g_lwip_lock` 内完成：`bh_ring_prepare()` 先 memset 整个 65535 字节事件
     （`socket_inet.c`），再由 callback 拷入段内容；bottom-half 侧还有一次
     冗余的整对象 memset（`obj_cache_alloc_zero` 已经清零）。

   **这解释了下面第 1 条里那个 `max=472365` 的来源，也解释了为什么削掉读路径
   轮询之后自旋量根本没降**（1.15M -> 1.28M）：持锁时长从来不在读路径上，而在
   这些 memset 里。此前把归因指向读路径是错的。

   已修（`feat/net-lanes`）：两级暂存改为小内联缓冲 + 溢出慢路径。实测尺寸：

   | 结构 | 修前 | 修后 | 降幅 |
   |------|------|------|------|
   | `net_bh_event_t` | 65752 B | 1808 B | 36x |
   | `net_msg_t` | ~65850 B | 1368 B | 48x |
   | `net_socket_t` | ~1053000 B | 30032 B | 35x |

   `net_msg_t` 落到 `mm/slab.c` 最大 slab class（2048）以内，不再走 buddy 取整；
   obj_cache 留活上限从约 134 MiB 降到约 3.8 MiB；锁内 memset 从 65535 B 降到
   约 200 B。datagram 超过内联尺寸时改为在 ring 里持有一个 pbuf 引用而不是拷贝，
   引用由 ring 槽位绕回时在 `g_lwip_lock` 下释放——因为 memp 没有任何内部加锁，
   消费者所在的上下文不能安全 `pbuf_free()`。

   这些尺寸现在由 `_Static_assert` 和 `NET_PROFILE_SOCKET_MAX_BYTES` 钉住，
   而不是靠文档约定。

   **仍未解决**：接收缓冲本身。`PBUF_POOL_SIZE` 256 x 1536 = 384 KiB 仍是天花板，
   见第 3 条。

1. **整个 TCP/IP 数据面被一把全局自旋锁串行化**。
   `g_lwip_lock` 保护全部 lwIP 核心状态，每次 raw lwIP 调用都必须持有
   （`docs/net/network-lock-contract.md`）。**多核服务器最核心的收益在这里
   直接归零**。这不是性能调优能解决的，需要重构 lwIP 集成。

   实测（`NR_CPUS=4` 真实 SMP 构建 + `net_stress_test` 4 路并发 × 4 MiB TCP）：
   ```
   lwip:   27 acquires / 1149170 spins
   proc:  3487 acquires / 5687843 spins
   runq:   14 / 103859      (每 CPU 一把)
   ```
   两把锁的 spin 量都极高，`lwip` 平均每次争用要空转约 4.2 万次。
   **而且真正更大的热点是 `proc_lock`**：争用次数约为 lwIP 的 126 倍、
   spin 量约 5 倍。原排序把 `proc_lock` 放在 P2（"进程数上去后的扩展性"）
   低估了；按这份数据，它比 lwIP 全局锁更该先处理。

   **默认配置根本测不出这类问题**。`Makefile` 里
   `NR_CPUS ?= 1`，所以所有默认 dev/smoke 门禁都是单核构建，给 QEMU 传
   `-smp 4` 也没用：内核只起 1 个 CPU，而单核永远不可能争用自旋锁。
   换句话说，之前观察到的 `lwip: 0 0` 是**单核默认配置伪造出来的假阴性**，
   不能解读为"锁没问题"。跨核锁竞争只有显式 `NR_CPUS>1` 才可见。

   spin 归因已修好，热点高度集中。此前 `spin_lock_at()` 里
   `site->spins` 是在外层重试循环里加 1，等于 acquire 次数的副本，真正
   的自旋次数只汇总到锁级 `contended_spins`，所以归因表的 spin 列毫无信息量
   （实测 27 次 acquire 对 114 万次自旋，调用点 spin 合计只有 27）。
   现改为在进循环前先固定 slot、内层自旋排空后按 `spins - spun_before`
   累加，修复后调用点 spin 合计与锁级总数精确相等（689073 = 689073，
   另一次 381751 = 381751）。`smoke-smp-lock-contention` 现在把这条不变量
   钉住（调用点 spin 合计须 ≥ 锁级的 90%），删掉归因即失败。
   锁级数字（`contended_acquires`/`contended_spins`）是直接计数，可信。

   归因标签已查清：`3133c97d` 的撤回是错的，这里纠正回来。
   `net_vfile_read+0xf6` = 0x2068，正是 `socket_file.o` 里
   `call a20_lwip_poll`（0x2064）之后的那条指令，也就是一个返回地址。
   `lwip_stack.o` 里 `a20_lwip_poll` 调的是 `spin_lock_at.constprop.0`，
   存在 `.constprop` 克隆说明 `caller_ra` 被常量折叠了：
   `a20_lwip_lock` 与 `a20_lwip_poll` 同在 `lwip_stack.c`，GCC 在该编译
   单元内可自由内联，`spin_lock_irqsave` 里的 `__builtin_return_address(0)`
   于是被折成常量 0x2068。所以标签是可信的，指向读路径进入 poll 的
   那个调用点。（之前怀疑"extern 跨单元不能内联"是错的：跨单元确实不能，
   但这两个函数本就在同一单元。）

   要点比"热点在读路径"更精确，必须分两半看。
   - 谁在发起争用（acquire 侧）：socket 读路径。`net_vfile_read()`
     在 `for(;;)` 里反复调 `a20_lwip_poll()`（`socket_file.c:28`），
     数据没到就 park、醒来再 poll，是高频 acquire 方。
   - 别人在为什么而自旋（hold 侧）：`a20_lwip_poll_locked()` 在同一把
     锁里做 `sys_check_timeouts()`、逐设备 `poll()`、以及
     `a20_lwip_process_netif_rx_tx_locked()` 的无界 `for(;;)` 收包排空。
     自旋时间消耗在这段排空工作上，属于收包路径。
   所以原表述"争用来自读路径而非收包路径"把 acquire 方和 hold 方混为一谈，
   已撤回。**结构问题是一个阻塞读会高频触发 whole-stack poll，而 poll 在
   全局锁内跨越整段收包排空**：读路径与收包路径被同一把锁串在一起。
   注意"排空无上界"这个说法是错的：`virtio_net_recv()` 在 used ring 空时
   返回 0，而 ring 深度是 `VIRTIO_QUEUE_SIZE` = 32，所以单次 poll 最多处理
   32 个包，**本来就有上界，给它加界是空操作**。分片本身仍未完成。

   下一步已经很明确：读路径应复用既有的 RX-pending 门控。
   这套门控早就存在，而且正是为了解决同一类问题，只是读路径没走它：
   - `core/progress.c:38-44` 明写 *"NO_SYS lwIP has one global core lock.
     Letting every idle CPU poll it turns an otherwise idle SMP guest into a
     permanent lock convoy"*，因此 `kernel_progress_poll()` 只允许 CPU 0 轮询。
   - `kernel_progress_timer_tick()` 同样限定 CPU 0，且只有设备真的上报了
     pending 才重新取 `g_lwip_lock`；注释明确目标是"让 per-context-switch
     调度热路径不碰这把锁"。
   - `net/lwip_stack.c:55-62` 的 RX progress hint 契约：virtio-net IRQ 顶半部
     在排空前置位，`a20_lwip_poll_locked()` 消费它。
   而 `net_vfile_read()`（`socket_file.c:28`）绕过该 hint，在 `for(;;)`
   里无条件 `a20_lwip_poll()`，每轮都取一次全局锁，哪怕设备毫无数据。
   这就是实测里 acquire 侧集中在读路径的直接原因。

   已实现：读路径改为 `a20_lwip_poll_waiter()`，只在设备上报过工作时才取
   `g_lwip_lock`。先回答了三个"改错就是 read 挂死"的疑问，答案都是可以：
   - 交付不依赖读者自己 poll。virtio-net IRQ 顶半部跑
     `a20_lwip_poll_locked()` 把包排进 `bh_ring`；真正把 `bh_ring` 搬进
     socket 队列并唤醒 `read_waitq` 的是 `net_inet_bottom_half_process_all()`，
     而 `sched()` 在挑下一个任务之前就会跑它（`sched.c:1838-1841`）。
     读者一旦 park，`sched()` 必被调用，交付与唤醒照常发生。
   - TCP 定时器不依赖它。`kernel_progress_timer_tick()` 在全部 8 个架构
     的定时器中断里被调用，重传超时由中断推进，与读者是否 poll 无关。
   - bottom-half 故意不门控。它取的是 `g_net_lock` 而非 `g_lwip_lock`，
     且读者要靠它消费自己那份延迟收包数据。
   另外 `net_vfile_read()` 在 `wait_queue_link()` 前会在 `g_net_lock` 下复查
   `s->rx_head`（`socket_file.c:84-87`），因此不存在丢唤醒竞态。

   驱动无关的做法：给 `net_dev_ops_t` 加可选 `rx_irq_driven`，**NULL 表示未知，
   而未知按"没有 IRQ"处理**。所以任何未改造的驱动仍会被无条件排空，不会
   静默丢 RX。

   实测结果要分开看，不能只报好的一面：
   - `lwip` 争用 acquire 次数确实大幅下降：多轮 55 / 27 / 17 → 6 / 1。
   - **但 spin 量没有下降**：改前 1.15M / 1.07M，改后仍约 1.28M。
   也就是说这次只削掉了"为了发现没数据而白取的锁"，大头在 hold 侧：
   持锁者要把这一轮收的包全部走完 pbuf 分配、netfilter 和 `n->input()`
   （含完整 TCP 输入处理）才放锁。
   **但 spin"次数"不是持锁"时长"**：`arch_cpu_relax()` 的迭代次数与墙上时间
   没有固定换算，所以 1.28M 不能直接当成持锁毫秒数读。
   同时**"给排空加界"这条路是走不通的**（见上，排空本来就有 32 的上界）。
   真正剩下的是要么把每包在锁内的工作量降下来，要么分片这把锁，两者都远大于
   本次改动，需要各自的门禁。

   **本轮（`feat/net-lanes`）做的是前者，以及把 poll 分段。** 三处改动：
   收包载荷两级内联化（第 0 条）、`a20_lwip_poll_locked()` 拆成 timers 段与
   带包数预算的收包段、三条 socket 发送路径每迭代只取一次锁（TCP 此前用
   `a20_lwip_poll()` 开头，等于每迭代取放两次并多跑一轮 whole-stack pass）。

   **关于这些改动的效果，必须把话说准，不能重复本文件上面犯过的同类错误。**
   结构调整本身是确定的、可在编译期断言的：每 socket 1.05 MiB → 30 KiB，
   锁内每包 memset 65535 B → 200 B，发送循环每迭代 2 次全局获取 → 1 次，
   定时器中断 2 次全局获取 → 1 次 + 有界排空。

   但**争用计数本身在 QEMU TCG 下噪声极大，不能作为结论**。同一
   `NR_CPUS=4` + `net_stress_test` 负载的 4 次独立运行，stress 窗口的 `lwip`
   自旋数分别落在 0 / 52776 / 189334 / 920024。`arch_cpu_relax()` 的迭代次数与
   墙上时间没有固定换算（本文件上面已经因为 `rdcycle` 在 TCG 下不可信而撤回过一次
   时长结论），所以任何以 spin 数量为阈值的门禁都是 flaky 的。

   因此：`smoke-smp-lock-contention` 现在**记录**该数值而不做幅度断言，门槛只保留
   确定性的部分——per-callsite 自旋合计与锁级总数吻合，以及压力测试本身通过。
   曾经短暂加入过一个 400000 的 spin 上限，已在同一次运行里撤掉（它会在
   920024 那轮误报）。

   上面第一次改动后曾写下"833397 -> 528039 -> 94232，累计 -88%"这样的表述。
   **按本条提供的方差，该表述不成立，予以撤回**：那些是不同运行的单次读数，区间
   互相重叠，不能构成趋势。可以确定的是结构性改动本身，以及 `lwip` 在 acquire
   次数上从 27 降到个位数。

   顺带修掉一个真实的记账漏洞：`lock_counters.c` 过去会丢弃 `ra == 0` 的采样
   （如中断上下文无可恢复返回地址），导致 per-site spin 之和与锁级
   `contended_spins` 对不上；现在按 `?` 计入，门禁不变量恢复成立。
2. 无连接跟踪与 NAT。因此不能做端口转发、地址转换，也无法实现
   有状态的防火墙规则。
3. 窗口缩放已启用，但新的瓶颈是接收缓冲而非协议上限。lwIP 2.2 自带
   RFC 1323 实现，此前 `LWIP_WND_SCALE` 停在默认 0，整段代码被条件编译掉，
   窗口是裸 16 位字段，双向都卡死在 65535 字节。现已置 1（`TCP_RCV_SCALE 3`），
   `TCP_WND`/`TCP_SND_BUF` 提到 64×MSS = 93440 字节，线上字段为 `93440 >> 3`
   = 11680，可用 `_Static_assert` 保证不溢出。
   **但真正的天花板现在是接收缓冲**：`PBUF_POOL_SIZE` 256 × 1536 = 384 KiB，
   `TCP_WND` 再往上就该先把 pbuf 池撑大，否则中途耗尽丢段的代价高于宽窗口
   的收益。要真正吃满跨地域 BDP，还需要更大的接收缓冲与 AIO/零拷贝收包。
   **本环境无法给出吞吐实测。** loopback RTT 近 0，窗口限制根本不
   生效，因此这里只能声称"协议上限已解除"，**不能声称任何带宽数字**。
   池压力现已可观测：`/proc/a20/netmem` 暴露 lwIP 各池的
   used/max/err（`MEMP_STATS` 因 `MEMP_MEM_MALLOC=0` 派生为 1，计数器本来
   就在维护，但 lwIP 自带的唯一读取入口被 `LWIP_STATS_DISPLAY=0` 编译掉了，
   这是 A20OS 侧新写的读取路径）。其中 `err` 正是"池不够大"的信号，
   `smoke-lwip-memp` 断言跑完网络套件后 `err` 仍为 0。
   但 loopback 下 `max` 峰值极低，**这份数据不足以论证当前池容量合理**：
   要定容量需要真实高 RTT/大流量负载。
4. **本轮顺手查出并已修的实现缺陷**（都不是性能问题，但此前无人记录）：

   - `a20_lwip_register_netifs()` 在注册循环里逐个调 `netif_set_default()`，
     所以多网卡时**最后枚举到的设备覆盖前面的**，而不是第一个。已改为仅在
     `netif_default` 为空时设置。
   - `virtio_net_poll_all()` 全树无调用者，是死代码；而
     `docs/drivers/guide/lock-order.md` 仍把它列为活的入口点。函数与文档引用
     一起删除。
   - `lwip_stack.c` 里"loopback 后注册所以硬件在 netif_list 前面"的注释是
     **反的**：lwIP 的 `netif_add` 是前插，loopback 实际在链表头。功能上无影响
     （轮询遍历整表、IRQ 路径按 `st->idx` 匹配），但注释会误导后续修改。
   - `MEMP_MEM_MALLOC` 此前留空，被 `opt.h` 派生为 0，于是所有池都是 `.bss`
     静态数组而 `MEM_SIZE` 完全没用上。同时这也让 `/proc/a20/netmem` 的 `size`
     列变成**整列静默零值**（`memp_init_pool()` 在该模式下是空桩，唯一写
     `avail` 的代码在 `#if !MEMP_MEM_MALLOC` 里）。现在该列报 `desc->size`，
     并注明 per-pool 容量在 heap 模式下不存在，真实上界是 `MEM_SIZE`。
   - `MEMP_NUM_SYS_TIMEOUT`（32）小于 `MEMP_NUM_TCP_PCB`（64），而三个 TCP
     keepalive 宏全开，每个 established PCB 占一个 `sys_timeo`。池耗尽后
     `tcp_pcb_alloc()` 返回 NULL，表现为 `accept()` 失败。现在是
     `_Static_assert`。

5. **门禁 `smoke-lwip-memp` 与 `smoke-network-suite` 在当前 HEAD 上不可通过，
   与网络改动无关。** `alg_test` 在没有内核 crypto provider 时主动退出 78
   (ABSENT)，而 `network_suite` 刻意把 absent 判为 FAIL（其注释写明目的是让
   "被注释掉的内核实现"无法混进绿色门禁）。由于 `socket_alg.c` 明确不提供
   provider，`NETWORK_SUITE: PASS` 因此不可达。这是有意的 fail-closed 设计，
   不是回归——**但需要有人决定**：AF_ALG 的缺失是文档化的设计意图而非被注释掉的
   实现，按 `network_suite` 自己的分类定义更接近 SKIP 而非 ABSENT。改之前，
   任何依赖这两个门禁的网络验证都必须绕过它（例如直接断言 `/proc/a20/netmem`
   的输出）。

6. 无 SACK、无 ECN、无 SYN cookie、无 `MSG_ZEROCOPY`。
7. 多队列/RSS/RPS/XPS/XDP 全部缺失；virtio-net 只有一对硬编码队列
   （RX=0/TX=1），无 MSI-X，无任何卸载（CSUM/TSO/GSO/GRO）。
   深度 32 来自 `virtio_blk.h` 的 `VIRTIO_QUEUE_SIZE`；注意 virtio-net 用的
   是那里的 `ring[32]` 结构而**不是** `drivers/dual/virtq.h`（那个只有 8 深
   且 `virtq_init` 拒绝 `num > 8`），谁"顺手统一"过去会把队列深度静默降到 8。
8. `SO_BINDTODEVICE`、`IP_TRANSPARENT`、`TCP_CORK`、`TCP_USER_TIMEOUT`
   等返回 `-EOPNOTSUPP`（此前它们返回成功并被静默丢弃）。
9. accept 队列固定 128 且溢出静默丢弃；无 socket 内存压力控制，也无
   `/proc/sys/net/*`。

### 剩余工作与各自的阻塞原因

完整方案在 [net/net-lanes.md](net/net-lanes.md)，那里记录了阶段 A–G 的顺序、
各自的前置条件，以及为什么顺序不可跳。本节只记录**为什么还没做完**。

**阶段 A 已落地**：lane 归属哈希、`net_socket_t.lane`、bind 时重算、
`/proc/net/status` 的 `lanes:` 行、`NET_LANES` 构建旋钮。
`NET_LANES=1` 与 `NET_LANES=4` 的 `net_stress_test` 输出逐字节一致
（`4 parallel transfers, 4 rounds x 1048576 B`），嵌入式路径可证明未变。
lanes 行目前**尚不能证明连接已被分散开**——它只统计仍在 registry 里的 socket，
而 `net_stress_test` 会关掉自己的 socket，所以读到 `sockets=1` 是预期的。

#### `g_net_lock` 分片：前置是引用计数，不是锁

`g_net_lock` 覆盖 1024 个 socket、52 处获取，是当前收益最大的未做项。但它
不只是数据锁，**还是对象生命周期锁**：

- `net_socket_free()` 在 `spin_unlock_irqrestore()` **之后**调用
  （`socket_file.c`）。所以读者只要持锁，关闭方就无法注销，对象也就不会被
  释放。
- 因此把它换成 per-socket 锁**必须先有 `refcount_t`**，否则
  `net_socket_from_file()` 读到 `vf->priv` 之后、拿到锁之前，关闭方可能已经
  清空 `vf->priv` 并释放——中间没有任何东西阻挡。

**顺带查出一个既有的 use-after-free 窗口**：上面那个 `vf->priv` 与
`net_socket_free()` 之间的空档在今天就存在，与分片无关。读线程拿到 vfile
引用、读出 `s`、放下 vfile 引用，然后才用 `s`；关闭线程可以在此之间完成
`vf->priv = NULL` 与释放。目前靠 `g_net_lock` 偶然挡住（读者进临界区时
关闭方在等锁），但 `net_socket_from_file()` 本身并不持锁。

设计已想清并被部分实现过，结论记录在此：
`net_socket_t` 加 `ref_count`；`net_socket_alloc()` 起始为 1；
`net_socket_enter(gfd, &out)` 取引用并复核 `in_registry`，`net_socket_exit()`
释放；锁序为 registry 锁先于 socket 锁。

**本轮主动回退了**。用脚本批量改写这 20 个调用点（`net_socket_from_file` 的
使用者，含 `sys_socket_msg.c` 三个跨越整段控制流与 `recvmmsg` 循环的复杂
站点）时三次以不同方式损坏了 C 代码，其中一次把 `socket_control.c` 从 954 行
截到 21 行。半完成的生命周期重构比不做更危险：漏 `exit` 是泄漏，多一次
`exit` 是 use-after-free，而这两者都无法靠编译器发现。已用
`net_socket_exit()` 里的下溢 panic 作为运行时护栏，但护栏不等于正确。
接手者请用**逐函数手工改写**而不是脚本，并在同一提交里跑
`smoke-smp-lock-contention` 与 fd 相关的 stress 门禁。

#### 把协议栈输入移出中断上下文：直接改会死锁

`virtio_net_irq_handler()` 目前在中断上下文持 `g_lwip_lock` 跑完整协议栈。
直觉做法是让 IRQ 只入队并置标志、由 poll 路径处理——**这会死锁**：

- 阻塞读的任务 park 后，`sched()` 运行 bottom-half 去唤醒它；
- bottom-half 需要 ring 已被排空；
- 而能排空的 poll 路径若只在读者唤醒后跑，读者就永远在等一个不会被唤醒的
  事件。

所以需要显式 softirq 加一个**保证被执行的 poll 点**（`sched()` 内的
`kernel_progress_run_bottom_halves()` 是候选），外加一个有界、无锁的 pbuf
队列与溢出丢弃策略。这是数据路径上的并发代码，约 150 行新逻辑，不适合在
没有回归门禁的条件下赶工。

#### lwIP PCB 哈希分桶：`g_lwip_lock` 分片的入场费（阶段 B，**未动**）

热路径要无锁，就得让 established TCP 的收发不去遍历全局 PCB 链表。lwIP 2.2 的
`tcp_bound_pcbs` / `tcp_active_pcbs` / `tcp_listen_pcbs.pcbs` / `tcp_tw_pcbs`
四条加上 `udp_pcbs` 都是全局链表，`tcp_lookup()` 每次线性遍历。

比原估计小的关键发现：链表操作**已经宏抽象**，`TCP_REG(pcbs, npcb)` 与
`TCP_RMV`（`tcp_priv.h:355`、`:370`，ACTIVE 变体在 `:417`、`:423`）把表头作为
**参数**传入，所以分桶是"给表头加下标"而非"重写查找"。

仍未做的原因不是工作量而是风险：它要改 `tcp.c` 里 PCB 注册/摘除/查找的全链路，
而一次失手不会报错，表现为偶发 TCP 挂起。本轮尝试阶段 A 时用脚本批量改写 C 已经
连续三次以不同方式损坏源文件（其中一次把 `socket_control.c` 从 954 行截到 21 行），
在 `tcp.c` 上重复这个错误不值得。接手者请手工逐点改，并先只做分桶、不动状态机。

#### `proc_lock` 分片

当前最大的争用源（实测约为 lwIP 锁的 20 倍），根因是整个任务表一把全局
自旋锁（`proc.c:42`），`sched.c` 里有 30 处取锁点。但它与上面几项共享同一个
方法论约束：这是调度器关键路径，改错表现为偶发挂死而不是报错。**未动**。

#### 多队列 / MSI-X

除 QEMU `mq=` 接线外，驱动本身只协商 `F_MAC`/`F_STATUS`/`F_VERSION_1`，
没有 `F_MQ`。需要多队列对、每队列中断向量、PLIC per-hart 路由或 MSI-X。
注意上面第 7 条记录的 `virtq.h` 队列深度陷阱。
## 三、隔离与多租户

### 已达成

cgroup v1/v2 是真的，且在热路径上强制：`cg_mem_charge()` 在缺页路径有
十四个计费点，`cg_cpu_account()` 在每次上下文切换记账。`cpuset` 通过掩码
求交生效（`sched_task_cpu_mask()`）。这比很多「有 cgroup 目录」的项目实在。

### 仍缺

1. **8 个 namespace 只有 mount 是真的**，其余 7 个由 `unshare()` 显式返回
   `-EINVAL`。这一点是干净的（`sys_namespace.c:4-7` 明确写了边界）。
2. `pivot_root` 返回 `-EPERM`，且这不是顺手能补上的空洞。它的语义
   建立在真实 mount 树之上：把 `new_root` 变成树根、把旧根挂到 `put_old`
   之下，调用方才能用 `umount2(put_old, MNT_DETACH)` 真正摘掉旧根。但当前
   `proc_fs_context_t` 只有 `root_path` / `cwd` 两个路径字符串
   （`kernel/include/proc/proc.h:22-26`），`vfs_move_mount()` 也只是
   `strncpy` 改写挂载点的路径前缀（`kernel/fs/vfs/mount.c:84-97`），
   根本没有 `mnt_parent` 链。字符串模型里不存在"把旧根挂到新根之下"这个
   操作，强写就只能做成一个改 `root_path` 字符串的假动作：调用返回 0，
   旧根却并没有被隔离，`MNT_DETACH` 无从谈起。因此这里刻意保持
   fail-closed，而不是提供一个只会骗过容器运行时的 `-EPERM` 替身。
   真正的前置件是先把 root/cwd 从路径字符串换成真实的 mount 引用。
3. 无 userns、无 `nsproxy`、无完整 capabilities。
4. 无容器运行时（lxc/runc/nspawn/crun/podman 均无），`packages/world/`
   里没有 server world。
5. cgroup 缺 `pids` / `io` / `freeze` 控制器；`cpu.shares` 存了但调度器
   从不读取。
6. **全局 OOM killer 的评分只看 `oom_score_adj`，不看 RSS**，因此不会可靠地
   选中最大占用者（cgroup 局部路径倒是用了 RSS）。

结论：**当前形态无法承载多租户**。PID ns + userns + `pivot_root` 是绕不过
去的三件套。

## 四、进程与调度

- **无内核抢占**（无 `CONFIG_PREEMPT`，只有 `need_resched` 标志在安全点消费），
  也无 IRQ 线程化。`SCHED_FIFO` 存在，但过不了长内核路径的 deadline。
  `SCHED_RR` 本轮已修成真正轮转，但只能在调度器下次运行时让出 peers，
  无法按时间片强制抢占，这依赖上面那条抢占缺口。
- 无 RT 限流（`sched_rt_runtime_us`）、无 `RLIMIT_RTPRIO`：`SCHED_FIFO`
  任务可以独占 100% CPU，无预算、无计量。
- **`proc_lock` 是当前最大的压倒性热点**：4 核实测 2528 次竞争 / 951 万自旋，
  8 核 33335 次 / 1619 万自旋，多轮优化后仍 12–20K。根因是整个任务表只有
  一把全局自旋锁（`kernel/proc/proc.c:42`），`sched.c` 里有 30 处取锁点。
  归因标签要当心：实测最大的一行是 `proc_sched_safe_point+0x42`，但
  `proc_sched_safe_point()`（`sched.c:1005-1014`）只读一个 per-CPU 的
  `need_resched`，它自己不取 `proc_lock`；那一行其实是内联进去的
  `proc_yield()`，与 lwIP 那次 `net_vfile_read+0xf6` 是同一个"返回地址跳过
  一帧"的假象。`proc_yield()`（`sched.c:1923-1934`）同样不直接取锁：它调
  `proc_make_ready()`（状态转移）与 `sched()`（切换发布），二者才按
  `proc.h:112` 的契约去取全局 `proc_lock`。**所以争用实际落在上下文切换/
  状态转移路径上**，与 `proc.h:106-113` 描述的"切换发布要取 proc_lock"一致。
  这里曾断言成因是"全局锁被跨着一段 TLB 收敛等待持有"，**该断言已被实测推翻**。
  代码事实仍然成立：`context_switch_locked()` 在持有 `proc_lock` 的临界区内
  调用 `mm_context_enter(next->mm, cpu)`（`sched.c:1790`，锁在 `sched.c:1829` 取），
  而 `mm_context_enter()`（`mm/vm.c:76-102`）内部是一个 `for(;;)`：只要
  `mm->tlb_cpu_generation[cpu] != mm->tlb_generation` 就反复
  `arch_tlb_flush_asid_local()`；`tlb_generation` 会被
  `mm_tlb_shootdown_page()`（`vm.c:277`）持续累加。
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
  调用点。两个实测障碍，都不是代码能绕开的：
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
  `sched.c:1736` 的注释自己写着 *"mm_context_enter() only uses
  atomics"*：它按设计不需要 `proc_lock` 的一致性，却仍然被罩在临界区里。
  但不能简单把它前移：地址空间切换必须与 `proc_set_current()` 之间的
  中断窗口保持一致，否则中断处理会在"新 mm + 旧 task"的错配状态下运行。
  这属于上下文切换路径的定序设计，需要连带审阅 `sched.c:1880` 那条
  "one lock per switch" 的注释所描述的观察窗口保证。

  这段循环在本平台确实会执行，不是死代码，上面的计数器已经直接证明：
  136 次进入、其中 15 次真的刷新过 ASID。平台相关性依然要交代清楚：QEMU riscv64
  启动日志为 `[MM] RISC-V ASID mask=0xffff bits=16`（`riscv64/platform/asid.c:36`），
  所以 `mm->arch_asid` 非零，`vm.c:83` 的条件成立；arch.h 的通用默认是
  `ARCH_MM_CONTEXT_ALLOC() 0U`，只有 riscv64 覆写为 `riscv64_asid_alloc()`。
  **在 `arch_asid == 0` 的架构上这三个计数器恒为 0，引用本节数字时不能跨平台套用**。

  但也不能简单前移：`sched.c:1829` 那条调用点是本函数自己取锁，可以
  改成"先关中断 → mm_context_enter → 再取锁发布"；可是 `sched.c:1885`
  那条进入时 `proc_lock` 已经被 `sched()` 更早取走了（见 :1880 注释，
  刻意为了"每次切换只取一次锁"）。在那里要先放锁才能做 mm 切换，而放锁之后
  `next` 可能被别的 CPU 抢走，必须靠 `dispatching` 引用计数兜住并重新校验。
  这一点已核对过 `sched_runq_unpick_locked()`（`sched.c:1642`）：它的拒绝条件
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
  那样推出的 4 处候选（`mmap.c:37`、`proc.c:200`、`loadavg.c:54`、
  `cg_mem.c:116`）经核对在实测负载下基本不会执行：`net_stress_test` 的
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
  此前这里写的是"等待方数据在原理上无法指认持有者"，**这句话过头了**：
  `spinlock_t.owner_ra`（`lock.h:62`）在 acquire 时写入持有者 RA
  （`lock.h:197`、`irqsave` 变体 `:232`），到 unlock 才清（`:206`），
  也就是整个持有期间持有者身份对等待方始终可见；现成的 `[LOCK-STALL]`
  诊断（`lock.h:158-163`）本来就同时打印 `owner_ra` 与 `waiter_ra`。

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
- 所有 `smoke-*` 门禁硬编码 `-smp 1`（`perf-overhaul.md:127` 明确警告），
  **单核通过不证明 SMP 正确性**。对一个服务器 OS 的 CI 这是结构性缺陷。
- 全部性能数据来自 QEMU TCG 模拟器，无真机基准。

## 五、可观测性

本轮把「静默说谎」的观测面改成了真实测量（详见
[../security/hardening.md](../security/hardening.md) 的诚实性原则）：
`/proc/<pid>/io`、`/proc/loadavg`、`/proc/pressure`、`getrusage`、
`TCP_CONGESTION` 现均为真实值。

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

- panic 是关机不是重启（`panic.c:117` 调 `firmware_shutdown()`），
  失败则 `arch_halt()` 死循环。
- 无跨 CPU stop IPI（`panic.c:20-22` 自述）：其他核继续跑到自己 panic，
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
- 无 MSI/MSI-X → 只有 INTx。
- INTx 路由硬编码 QEMU q35：`x86_64/trap/irqchip.c:251-274` 只认
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

## 八、阻塞项排序

| 级别 | 阻塞项 | 理由 |
|---|---|---|
| P0 | 收包内存模型 | **已修**（`feat/net-lanes`）：两级暂存内联化，`net_socket_t` 1.05 MiB → 30 KiB，`net_msg_t` 68 KiB → 1368 B，锁内每包 memset 65535 B → 200 B，并由 `_Static_assert` 钉住 |
| P0 | `g_net_lock` 分片 | **当前收益最大的未做项**。它同样是一把覆盖 1024 个 socket 的全局锁，52 处获取。改成 per-socket 锁 + 引用计数保护的 registry 是纯局部改动，不触碰 lwIP 核心 |
| P0 | lwIP 全局锁分片 | 持锁方一侧的时长在 TCG 下拿不到，本文件已因此撤回过一次结论；分片方案不应再等这个数。已确定的前提是：热路径要靠 socket 单一所有权避免全局 PCB 链表遍历，这需要先给 lwIP 的 `tcp_active`/`tcp_bound_pcbs`/`udp_pcbs` 做按端口哈希分桶 |
| P0 | PID ns + userns + `pivot_root` | 多租户前置件；`pivot_root` 需先把 root/cwd 从路径字符串改为真实 mount 引用 |
| P0 | ext4 可写 journal + 崩溃注入测试 | 数据库一致性的硬前提 |
| P1 | conntrack + NAT | 容器网络与服务暴露的依赖 |
| P1 | 扩大接收缓冲（pbuf 池 / 零拷贝收包） | 窗口缩放已解除协议上限，现在卡在 384 KiB pbuf 池 |
| P1 | MSI-X + ACPI `_PRT`（bridge 遍历已完成） | 真机服务器的准入条件 |
| P1 | kdump 执行后端 + panic 改为重启 | 故障后能否自动恢复 |
| P1 | 内核抢占 + RT 限流 | 实时性与尾延迟保证 |
| P2 | 硬件 watchdog + A/B 分区 + dm-verity | 无人值守与安全更新 |
| P2 | 硬件 PMU + ftrace/tracepoints | 生产环境可诊断性 |
| P0 | `proc_lock` 超长持有的成因未定 | **只证伪了一半**。已证伪"被抢占"（成立）：全树 69 处 `proc_lock` 获取全部走 `spin_lock_irqsave`，无一处关中断之外；持锁临界区内无任何 `sched()`/`proc_yield()`。所以持有者确实在长时间执行。但**"成因类别已确定"这个说法不成立，本条已撤回**：先前据"持锁临界区里做全系统遍历"推出的 4 处候选，经核对在实测负载下基本不会执行：`net_stress_test` 的 `read()`/`write()` 是套接字调用，够不到 `mm_sync_shared_dirty_for_vnode()`；`proc_get_vm_stats()` 的唯一调用点是 `procfs_render.c:435` 的 `PF_MEMINFO`，而门禁只 cat `/proc/a20/perf` 与 `lock_contention`。更关键的是计数器自启动起累计、没有 reset 入口，所以那个 905K–136 万自旋的单次极值可能发生在引导期而非压力期。结论：成因仍未定位，且现有门禁的观测窗口本身有缺陷，需先给计数器加 reset 以便把引导期与压力期分开 |
| P2 | virtio-fs/DAX | 共享存储 |
| P2 | 真 RTC + paravirt clock | 真机时间正确性 |
| P3 | NUMA、热管理、C-states | 规模与能效 |

## 九、推荐的第一批动作

按「改动小、风险低、避免真实事故」排序：

1. 补一个挂 `ich9-ahci` 的门禁，让 AHCI flush 获得运行验证。
2. 崩溃注入测试基础设施（QEMU 可用 `-device qemu-x-test` 或直接 kill -9 +
   重放镜像比对），这是 ext4 journal 改造的前提。
3. 引入真机基准入口；当前所有性能结论都来自 TCG 模拟器。

以下两项曾在本清单里，现已完成，不再是待办：

- `/proc/pressure` 曾是单文件布局，systemd 按 `cpu`/`memory`/`io` 目录读会
  失败。该布局已改为目录，见 `kernel/fs/procfs/procfs.c` 的
  `pressure_entries` 与 `procfs_render.c` 的 `PF_PRESSURE_*` 分支。
  `mem`/`io` 渲染为结构性的零（设备 I/O 全部同步，没有可采样的 stall 状态，
  见 `kernel/core/psi.c` 的说明），不是伪造的数字。
- `MAX_PROCS` 死常量已删除，全树无残留引用。
