# 网络锁契约

本契约定义 A20OS 内核网络路径的锁规则，适用于 `kernel/net/` 中的 socket 层、`kernel/net/lwip_stack.c` 中的 lwIP 集成，以及任何会触碰网络状态的 deferred bottom-half 或 workqueue。

> **更正（2026-10-05，两次）：**
>
> 1. 本文此前写"accept 路径上 `g_net_lock` 与 `g_lwip_lock` 同时持有，顺序 net → lwip"。
>    这条**已经不成立**。提交 `2f17a5ba8`（`net: 删除 g_net_lock，socket 表分片为
>    net_bucket 桶锁`）删除了 `g_net_lock`，全树再无该符号；accept 落底现在只取
>    listener 所在的那一把 `net_bucket[b]`。`g_lwip_lock` 与**任何** bucket 锁不得
>    同时持有这条互斥不变量在分片时被明确保留（`kernel/include/core/lock.h:82-84`，
>    `kernel/net/socket_internal.h:434-436`）。**净效果是本文变严格了，不是放松了**：
>    原来只禁 `g_net_lock` 一把，现在禁全部 `NET_SOCK_BUCKET_COUNT` 把。
> 2. 本文把 `g_net_lock` 当作"现存的一把全局锁"来描述适用范围（哪些状态由它保护、
>    谁可以取它、取它的顺序要求）。这些描述**逐条按 `net_bucket` 重写**在下方
>    「锁」一节；正文其余部分凡出现 `g_net_lock` 的，统一读作
>    "**该 socket 所属的 bucket 锁**"，而不是一把全局锁。

最后核实：与 `2f17a5ba8` 之后的 `net_bucket` 分片树一致，并补入 `7d217d3fd`
（`LWIP_ASSERT_CORE_LOCKED()` 接线）与 `edc29d31a`（回环 TCP 传输不结束）的事实。

## 范围与目标

A20OS 以 `NO_SYS=1` 模式运行 lwIP。一个全局 spinlock `g_lwip_lock` 串行化所有 lwIP 核心状态。socket 层用一组分片桶锁 `g_net_buckets[]` 保护 socket 表、每个 socket 的消息队列、accept 队列与 waiter。

本契约目标：

- 防止 socket 系统调用、lwIP callback 和驱动路径之间发生死锁。
- 禁止在 `g_lwip_lock` 下执行阻塞操作，保持中断和调度延迟较低。
- 让 socket send/recv/connect/listen/accept 测试可以安全并发运行。
- 记录 deferred bottom-half 如何与这些锁交互。
- 记录收包载荷在两级暂存中的内存所有权。

## 锁

### `g_lwip_lock`

- 在 `kernel/net/lwip_stack.c` 中定义为 `spinlock_t`。
- 保护全部 lwIP 核心状态：PCB 列表、pbuf、timeout 列表、netif 状态、ARP/DNS/DHCP 状态和 lwIP 统计。
- 通过 `a20_lwip_lock()` 获取，通过 `a20_lwip_unlock()` 释放。
- `a20_lwip_lock()` 禁用本地中断并获取 spinlock；`a20_lwip_unlock()` 恢复之前的中断状态。
- 每个 raw lwIP API 调用都必须在持有该锁时运行。

### `g_net_buckets[]`（原 `g_net_lock`，已删除）

**`g_net_lock` 不再存在。** `2f17a5ba8` 删除了它并把 socket 表分片。当前形态：

- 声明于 `kernel/net/socket_internal.h`，类型 `net_bucket_t[]`，下标是
  `net_socket_bucket(s)`（`socket_internal.h:505-511`）。该下标是
  `s->reg_idx >> NET_SOCK_BUCKET_SHIFT`，即 **slot 号的高位**，不是哈希。
- **分片粒度**（`socket_internal.h:457-467`）按 profile 缩放：
  `NET_MAX_SOCKETS >= 65536`（SERVER）→ 512 slot/桶 → 128 桶；
  `>= 1024`（DEFAULT）→ 32 slot/桶 → 32 桶；
  否则 1 slot/桶。三条 `_Static_assert` 钉住"桶宽整除 slot 上限"与"slot 上限是 2 的幂"。
- 额外有一个**孤儿桶** `NET_SOCK_ORPHAN_BUCKET = NET_SOCK_BUCKETS`
  （`socket_internal.h:470-473`）：`reg_idx < 0` 的 socket——正在创建，或正与并发
  `close()` 竞争——还没有 slot，因而没有桶。它们取这把额外的锁，并在入口重新检查
  `in_registry`。它是最后一个下标，因此在 `net_bucket_lock2()` 里排在所有真桶之后。
- 保护的状态与旧 `g_net_lock` 相同：`g_sockets[]`、每个 socket 的字段、消息队列、
  accept 队列、临时端口分配和 socket waiter，外加**每桶的空闲位图**
  （`net_bucket_t.free_bits`，位清=空闲，与分片前的 `g_sock_free` 极性一致）。
  位图按桶整字对齐，一个字只属于一个桶，所以注册/注销不会碰到别的桶锁保护的东西。
- 获取必须走内联包装，不要直接摸 `g_net_buckets[b].lock`：
  `net_bucket_lock(b)` / `net_bucket_unlock(b, flags)` / `net_bucket_lock2(b1, b2)` /
  `net_bucket_unlock2(p)`（`socket_internal.h:513-557`）。它们统一用
  `spin_lock_irqsave` / `spin_unlock_irqrestore`。

**取桶锁的规则**（完整表述见 `socket_internal.h:424-442`，权威副本在
`kernel/include/core/lock.h:67-90`）：

- 单 socket 临界区：只取**拥有该 socket 的那一把**。
- 需要两个 socket：取两把，按**桶号升序**。`net_bucket_lock2()` 是唯一被认可的写法；
  它把较小者放进 `lo` 较大者放进 `hi`，所以任何一对桶都不会被反向持有，顺序无环。
- **同时最多持两把桶锁。** 遍历整张表时**绝不可**把整组桶锁都取上：中断已关的
  情况下，一个走到查表的中断会去自旋等一把它自己的被中断上下文正持有的锁，那个
  活锁的记录见 `fs/vfs/dcache.c` 的同一处错误。`net_socket_table_walk()` 和其他
  全表扫描都是一次一把、取了就放。
- `g_lwip_lock` **不与任何桶锁同时持有**。这是旧 "`g_lwip_lock` 与 `g_net_lock`
  从不同时持有" 那条规则的**原样保留**，只是范围从一把锁扩到全部桶。
- `net_register_socket_locked()` 自己会取桶锁（一次一把），所以**不得**在已持有
  另一个 socket 桶锁的情况下调用它：一个桶由 512 个 slot 共享，把任意一把桶锁嵌到
  已持有的一把之下，正是这个顺序规则要禁的 ABBA。

生命周期与桶锁是两件事。`2f17a5ba8` 把 socket 注销改成新引用计数原语
（注册 +1 / 锁外注销 -1），accept drain 另加 `drain_active` 显式串行化
（`socket_inet.c:917-919`）——原先这个 single-consumer 保证是隐含在全局锁里的，
分片之后必须显式化。

### 全局顺序与当前更严格规则

`kernel/include/core/lock.h:47-48` 给出的全局允许顺序上界是：

```text
g_lwip_lock -> net_bucket[*]
g_lwip_lock -> virtio-net nonblocking send/recv paths only
```

当前网络实现采用更严格的契约：`g_lwip_lock` 与**任何** `net_bucket` 锁
**不得同时持有**。桶与桶之间按升序，无环。箭头只表示若将来确有经审查的嵌套，
反向顺序永远禁止；它不是对 callback 获取桶锁的许可。

lwIP callback 在隐式持有 `g_lwip_lock` 的上下文中运行，只能向 per-socket 原子 `bh_ring` 写事件并设置 pending flag。`a20_lwip_poll()` 先释放 `g_lwip_lock`，再调用只持有桶锁的 `net_inet_bottom_half_process_all()`。驱动数据面是另一条允许顺序：`g_lwip_lock -> virtio-net/E1000 nonblocking device lock`，驱动锁下不得回调 lwIP。

## 核心锁断言：本文从"文档"变成"可执行"

本文此前只是一份文档——上面每条规则都要靠人读代码去核对。上游
`kernel/external/lwip/src/include/lwip/opt.h:227` 把 `LWIP_ASSERT_CORE_LOCKED()`
定义成**空宏**，除非移植层自己 `#define`。本树此前没有定义，于是
`tcp.c` / `tcp_in.c` / `raw.c` / `udp.c` / `dns.c` / `ethernet.c` 里那几十处断言
**全部是空操作**。`net-lanes.md` 记的那个多 lane + 真实 LISTEN pcb 的链表损坏之所以
能一路走到汇编和 UBSan 描述符才被抓住，就是因为它。

### 接线

`7d217d3fd` 把它接上，四个要点：

1. **记录持有者 CPU，而不是布尔量。** `a20_lwip_lock()` 在取到锁时写入
   `g_lwip_lock_owner = cpu_current_id()`（`kernel/net/lwip_stack.c:426-427`），
   `a20_lwip_unlock()` 写回 `A20_LWIP_LOCK_UNOWNED`（`434-435`）。
   **不用布尔量的理由**：布尔量回答的是"这个标志置位了吗"，而别的 CPU 持有锁时它在
   每个 CPU 上都是真——恰好会放行断言要抓的那种情况。CPU id 才能回答"是不是**我**"。
2. **宏映射到一个会 panic 的函数。** `lwipopts.h` 把 `LWIP_ASSERT_CORE_LOCKED()` 映射到
   `a20_lwip_assert_core_locked(site)`，`site` 取宏展开点上的
   `__builtin_return_address(0)`——也就是"未持锁运行的那个 lwIP 函数"。panic 之前先把
   site 记进 `g_lwip_lock_sites[]`（`lwip_stack.c:440-458`），这样即使 panic 的输出被
   截断，也知道是哪些调用点在违约。
3. **arm 时机。** 断言在 `a20_lwip_init()` 结束时才 arm（`lwip_stack.c:404-407`）。
   **arm 之前断言是 no-op**：lwIP 自己的 `lwip_init()` / `netif_add()` / `netif_init()` /
   `dhcp_start()` 链是在 `a20_lwip_init()` 内部跑的，那里没有 A20OS 的锁。
   探针在引导期实测到 **23 次命中、8 个不同返回地址**（`netif_init`、`netif_add`、
   `lwip_init`、`netif_add_ip6_address`、`a20_lwip_init`、`a20_lwip_loopif_init_cb` 等），
   对它们 panic 会让**每个配置启动即死**。所以 arm 之后才是"一次违规即 panic"，
   而不是只计数——计数型信号会被当成噪声，而这条契约的全部价值在于"违反必须停下来"。
   引导期的已知命中**不计入** `violations`：把 23 次合法 init 命中和真实违规混在同一个
   计数器里，那行输出就变成噪声，正好训练所有人忽略它。
4. **开关。** 以上全部在 `CONFIG_NET_LOCK_ASSERT` 之下（`net_profile.h`），默认 0。
   打开它会让 tcp.c 里那 42 处断言**从空操作变成 panic**，所以只在排查
   `net-lanes.md` 那条 flake 时开，不进任何常规构建。

### 可见性

`/proc/net/stats` 打印 `lwip_lock: armed=%d owner=%u violations=%u sites=%u`
（`lwip_stack.c:822-827`）。`owner=4294967295`（`A20_LWIP_LOCK_UNOWNED`）表示采样时锁
空闲；`sites=0` 表示探针一次都没触发。**开关关闭时它显式打印
`lwip_lock: not checked (CONFIG_NET_LOCK_ASSERT=0)`**（`lwip_stack.c:837-840`），
不打印会让人把"没报错"误读成"没问题"。

排查 `net-lanes.md` 那条多 lane flake 时的用法：开
`CONFIG_NET_LOCK_ASSERT=1` 跑 4 lane + 4 CPU + `a20.tcpmode=lwip`，
每轮结束看 `violations`。**2026-10-05 的两轮放大实验里它全程是 0**
（72 次连接，`sites=0`），见 `net-lanes.md`「放大实验已执行」——这构成一条负面证据：
那条 flake 不经由"未持锁调 lwIP 入口"这条路发生。

### 移植层自己补的断言

上游的断言集中在 pbuf 列表/计时器与 TCP 内部函数上，**不覆盖**应用直接调用的裸入口。
`7d217d3fd` 在 `tcp.c` 补了 7 个此前完全没有断言的入口，把探针盲区变成覆盖区
（上游此处也没有断言，纯新增）：

```text
tcp_new()、tcp_new_ip_type()、tcp_slowtmr()、tcp_fasttmr()、
tcp_netif_ip_addr_changed()、tcp_process_refused_data()、tcp_trigger_input_pcb_close()
```

`tcp_abort()` **本树已经带断言**（`tcp.c:656`）。`net-lanes.md` 曾记它没有，那条记录
已过时——本文件不重复该错误。

> 分歧登记在 `kernel/external/lwip/DIVERGENCE.md`：lwIP 侧未改上游语义，
> 改的是 `lwipopts.h` 的宏映射，以及上面这 7 个入口前插入的断言语句。

## conntrack 与 NAT 用哪把锁

**决定：conntrack 表由 `g_lwip_lock` 保护，不另设专用锁。** NAT 规则表用
`g_netfilter_lock` 下的 seqlock，因为它的写者只有 /proc 配置面。这两者不是同一类
状态，分开对待是有理由的，不是省事。

分界的依据是**写者是谁**：

| 状态 | 写者 | 锁 |
|---|---|---|
| NAT 规则表 `g_nat[]` | 只有 /proc `natadd`/`natdel` | `g_netfilter_lock` + seqlock |
| conntrack 表 `g_ct[]` | 每包一次（插入、计数、状态迁移） | `g_lwip_lock` |

conntrack 的每个包都要写（`e->packets++`、`e->last_ms`），所以 seqlock 不适用：它会把
写区段放到包路径上，让并发 /proc 读者变成写者饥饿的候选者，而包路径不允许阻塞。

**为什么不加专用 conntrack 自旋锁。** 包路径本来就持有 `g_lwip_lock`，再加一把锁等于
每包多一次全局获取；更要紧的是，等 `g_lwip_lock` 按
[net-lanes.md](./net-lanes.md) 拆成 per-lane 之后，这把新锁会**取代** `g_lwip_lock`
成为新的串行点——正是 lane 改造要消除的那个结果。所以本文件所有 conntrack 变更都发生
在已经持有 `g_lwip_lock` 的上下文里：

- 两个 hook：input 在 `a20_lwip_process_netif_rx_tx_locked()`，output 在
  `a20_lwip_linkoutput()`；
- 空闲超时清扫：`a20_lwip_poll_timers_locked()`（`kernel/net/lwip_stack.c:651`），
  选 timers 段正因为它是唯一无条件推进的驱动点，任务阻塞在 `connect()` 里时只有它还在走。

**/proc 侧自己取锁**，因为 procfs 访问是系统调用，不在 `g_lwip_lock` 下：

- 写动词 `ctflush` 在 `kernel/fs/procfs/procfs.c:1040` 显式取放；
- 读渲染由 `netfilter_format()` 在 `kernel/net/netfilter.c:641` 取一次锁后调用
  `netfilter_nat_format()`。**渲染路径不得再取第二次**：`spin_lock_irqsave()` 不可重入，
  二次获取是自死锁。它表现得像锁竞争而不是自死锁——单 CPU 构建上是
  `netfilter_nat_format` 里的 `[LOCK-STALL]`，`owner=-1`，因为 `g_lwip_lock` 的 owner
  字段只在 `CONFIG_NET_LOCK_ASSERT=1` 下维护，否则一直是陈旧值。判据是单 CPU 上
  trylock 必然失败：失败只可能意味着当前 CPU 已经持有它。

同族先例：`a20_lwip_format_memp()`（`procfs_render.c`）渲染 lwIP 自有状态时取同一把锁。

conntrack 表不分配内存（静态数组，档位见 `net_profile.h` 的
`NET_PROFILE_CONNTRACK_ENTRIES`），符合本文件"锁下不得分配"那条规则，不需要例外。

## Poll 的分段

lwIP 进展推进被拆成可独立进入的临界区，因为不同调用方需要的部分不同：

| 入口 | 内容 | 调用方 |
|------|------|--------|
| `a20_lwip_poll_timers_locked()` | 仅 `sys_check_timeouts()` 与配置同步 | 定时器中断 |
| `a20_lwip_poll_rx_locked(budget)` | 设备完成轮询 + 收包排空 | 收包路径 |
| `a20_lwip_poll_locked()` | 上面两者，排空不限量 | `a20_lwip_poll()`、socket 发送路径 |

`budget` 为 0 表示不限量。**为 0 时语义与旧的整体 poll 完全一致**，既有调用方不受影响。`budget` 限制的是**单次持锁处理的包数**，不是 ring 能存多少包——后者本来就受 ring 深度限制，给它加界是空操作。

提前停止排空的调用方会拿到返回值 0，此时**必须不清 RX pending 标志**：能排掉剩余包的断已经被消费掉了，标志若被清掉，剩余包会一直等到下一次中断，而那次中断可能不会来。

`kernel_progress_timer_tick()` 只取 timers 段，随后用 `CONFIG_NET_RX_IRQ_BUDGET` 的包数上界取一次收包段。它在 CPU 0 的每次定时器中断上运行；该排空只是"设备中断万一丢失时不让 RX 卡死"的兜底（设备 IRQ 才是主路径），不足以正当化在中断上下文里跑一整轮协议栈处理。

### loopif 必须由 timers 段排空

`netif_poll()` 是 `netif->loop_first` 的**唯一**排空点，而 `netif_loop_output()` 只入队就返回。
所以 timers 段除了推进超时，还必须遍历 netif 链表、对 `loop_first != NULL` 的 netif 调
`netif_poll()`。

**这不是优化，是正确性。** 回环流量不产生设备 RX，因此 RX-pending 提示永远不会为它置位；
而 `kernel_progress_poll()` 与读者路径都是"按需"到达 `a20_lwip_poll_*` 的，park 在
`connect()` 里的任务两个选择都不做。曾经因为排空只挂在按需路径上，握手 SYN 永远躺在
`loop_first` 里直到 connect 超时。定时器中断是唯一无条件运行的进展驱动，所以它必须承担
排空。

同一条推理也约束 RX-pending 提示：它必须把 `loop_first` 一起算进"有活要干"，否则读者会
跳过这次排空。提示里的 `loop_first` 读不需要 `g_lwip_lock`——它是对一个由
`SYS_ARCH_PROTECT` 保护的指针做空判；假阳性只多一次锁获取，假阴性是挂死。

## 收包载荷的内存所有权

收包路径曾经让每个 1460 字节的段付出约 136 KiB 的 memset 加三次拷贝，其中绝大部分在 `g_lwip_lock` 内完成。这是 `docs/server-readiness.md` 记录的单次 acquire 自旋尖峰（`max=472365`）的来源，也是"去掉读路径轮询后自旋量没降"的真正原因——持锁时长从来不在读路径上。

两级暂存现在都改成小内联缓冲 + 溢出慢路径：

| 结构 | 载荷 | 溢出 |
|------|------|------|
| `net_bh_event_t` | 内联 `NET_BH_INLINE_PAYLOAD` | `spill`：ring 持有的 pbuf 引用 |
| `net_msg_t` | 内联 `NET_MSG_INLINE_PAYLOAD` | `overflow`：独占的 kmalloc 缓冲 |

内联尺寸由 `NET_PROFILE_INLINE_PAYLOAD` 给出，并有 `_Static_assert` 钉住两条不变式：`NET_BH_INLINE_PAYLOAD >= TCP_MSS`（否则每个 TCP 段都被推上溢出路径），以及 `NET_MAX_PAYLOAD` 仍大于内联尺寸（否则内联就不是优化了）。

### spill 引用的释放时机

`spill` 只可能来自 datagram socket：TCP 段最大就是 `TCP_MSS`，装得进内联缓冲。

**ring 里的 pbuf 只在生产侧释放。** `bh_ring_prepare()` 在把一个槽位重新发出去之前，先释放该槽位记录的引用；socket 销毁时（`net_inet_socket_destroy()`，持 `g_lwip_lock`）排空整个 `owned[]` 数组。

这样安排的原因是 memp **没有任何内部加锁**，而当前每一次 memp 调用都在 `g_lwip_lock` 下发生。消费者运行在只有桶锁的上下文里，在那里调 `pbuf_free()` 会让 memp 的空闲链表被两个 CPU 同时修改。槽位绕回时释放把 pbuf 的存活期限制在 ring 深度以内，并且落在唯一安全的地方。

**消费者故意不释放 spill pbuf。** 如果将来看到这里少了一次 `pbuf_free()`，那是特性不是泄漏。

### 溢出缓冲的所有权

`net_msg_t.overflow` 由该消息独占，`net_msg_free()` 负责 `kfree()`。它在 bottom-half 里分配，也就是在桶锁下——契约禁的是 `g_lwip_lock` 下分配，桶锁下分配一直是被允许的（`net_msg_alloc()` 原本就在那里调用）。

## 锁安全的 Socket 入口点

以下小节按操作类型给出锁纪律。实现必须匹配这些规则。

### Socket 创建与销毁

`net_inet_socket_init()` 和 `net_inet_socket_destroy()` 在创建、配置或移除 lwIP PCB 时只持有 `g_lwip_lock`，不同时访问 socket registry。registry 与 socket 字段由调用方在独立的桶锁临界区处理。

`net_inet_socket_destroy()` 还在同一个临界区内排空 bottom-half ring 的 spill 引用（见上）。

### Bind

`net_inet_bind_pcb()` 在不持有任何锁的情况下解析用户地址，然后只在调用 `udp_bind()`、`raw_bind()` 或 `tcp_bind()` 时获取 `g_lwip_lock`。bind 期间 socket registry 不发生变化。

### Connect

Stream connect 分为三个阶段：

1. 本地目标解析。如果目的地址是本地地址，该路径在搜索 listener 表并构造配对 socket 时取相关桶（跨两个 socket 时按升序取两把）。此时不持有 `g_lwip_lock`。
2. 远端 TCP connect。地址解析后，路径获取 `g_lwip_lock`，带 connected callback 调用 `tcp_connect()`，然后释放 `g_lwip_lock`。
3. 阻塞等待。调用者释放所有锁，并通过 `net_block_on_socket_locked()` 在该 socket 的桶锁上阻塞。connected callback 只向 `bh_ring` 发布事件；随后不持有 `g_lwip_lock` 的 bottom-half 获取桶锁、更新状态并在解锁后唤醒 waiter。

UDP 和 RAW connect 遵循与 bind 相同的模式：在锁外解析，然后只在调用 `udp_connect()` 或 `raw_connect()` 时获取 `g_lwip_lock`。

### Listen 与 accept

Listen 将 TCP PCB 设置为监听状态。listen 调用必须在 `tcp_listen()` 状态转换和安装 accept callback 时持有 `g_lwip_lock`。

Accept 只取 listener 所属的那一把桶锁（`net_bucket_lock(net_socket_bucket(listener))`）。
它从 listener accept 队列中弹出预创建的 child socket。如果返回了 child，调用者随后
调用 `net_inet_accept_child_ready()`，该函数获取 `g_lwip_lock` 并调用
`tcp_backlog_accepted()`。

`2f17a5ba8` 之前这段用的是全局 `g_net_lock`，所以"accept 落底"曾经是
`net_inet_accept_stage_drain()` 在 `g_lwip_lock` **之外**、全局 net 锁之内运行。
分片后这里只取 listener 那一把桶，**且必须先放掉 `g_lwip_lock` 再取它**——
`g_lwip_lock` 与任何桶锁不得同持。原先隐含在全局锁里的 single-consumer 保证
（"只有一个消费者会排空这个 listener 的 stage"）现在由 `drain_active` 显式串行化
（`socket_inet.c:917-919`），不要因为"只有一把桶锁"就以为不需要它。

`tcpmode=lwip` 下 inbound 走的是两段式交接：`lwip_tcp_accept_cb()` 在 `g_lwip_lock` 内
把已完成握手的 pcb 停进 listener 的 accept stage 并返回 `ERR_OK`，child socket 的分配、
注册与 accept 队列入队都在 bottom half 里做。**返回 `ERR_OK` 只代表"pcb 归应用所有"，
不代表"pcb 现在可以放着不管"**，规则见下文「停在 accept stage 里的 pcb」。

### Send

send 路径对本地 socket 和远端 socket 行为不同。

对本地 UDP loopback 或已连接本地 socket，路径要碰到**两个** socket（源与目的），
所以取的是**两把**桶锁、按升序——`net_bucket_lock2()`，这是唯一被认可的写法
（`socket_inet.c:2091` 的 `net_inet_send_tcp()` 就是这个形状）。如果目的队列已满
且调用是阻塞的，它会释放两把桶锁、阻塞并重试。

对远端 UDP、RAW 或 TCP send，socket 地址/本地队列状态和 lwIP PCB 操作分成互不重叠的临界区。调用 `pbuf_alloc()`、`udp_sendto()`/`udp_send()`、`tcp_sndbuf()`、`tcp_write()` 或 `tcp_output()` 时持有 `g_lwip_lock`，不得同时持有任何桶锁。需要更新本地 socket 状态或等待队列时先释放 lwIP 锁，再进入桶锁临界区。

> 分片带来的新约束：跨 socket 的目的地址解析（`net_sendto_sock()`、
> `net_vfile_write()` 里的两 socket 路径）要扫整张表，因此是**一次一把**地走桶
> （`net_socket_table_walk()`），不是一次取全组。目的地址在锁外解析完再进临界区。

UDP、RAW 和 TCP 三条发送路径现在形状一致：**一次迭代一次持锁**，在临界区内调用 `a20_lwip_poll_locked()`。TCP 路径此前用 `a20_lwip_poll()` 开头，额外取放一次全局锁并多跑一整轮 whole-stack pass；按 64 KiB 发送缓冲写 4 MiB 约迭代 64 次，也就是原本 128 轮而 64 轮就够。

bottom-half **不**在发送循环里逐轮运行：它们只取桶锁，而 `g_lwip_lock` 与桶锁从不同时持有。循环结束后在锁外排一次即可覆盖同样的工作。错误返回路径不排是安全的，因为 `sched()` 在挑选下一个任务前会运行两个 bottom-half。

### Recv

Recv 只取该 socket 所属的那一把桶锁。它从 socket 接收队列中出队消息。如果队列为空且调用是阻塞的，它释放锁，通过 `net_block_on_socket_locked()` 阻塞，然后重试。

当 recv 消费 TCP 数据后，调用者随后调用 `net_tcp_recved()`，该函数获取 `g_lwip_lock` 来更新 TCP window。

## lwIP Callback 规则

lwIP callback 运行时，lwIP 已经持有 `g_lwip_lock`。callback 不得：

- 阻塞或睡眠。
- 调用 `kmalloc()` 或 `kfree()`。
- 调入 VFS、scheduler，或任何可能获取其他 spinlock 的路径，除非该路径明确记录为非阻塞且锁顺序安全。
- 递归获取 `g_lwip_lock`。

`kernel/net/socket_inet.c` 使用 Deferred Bottom-Half 设计：lwIP callback 只把事件写入 per-socket 有界 `bh_ring` 并调用 `net_inet_bh_schedule()`，真正的 `net_msg_t` 分配与 payload 搬运在 bottom-half（`bh_ring` 消费路径）中完成，不持有 `g_lwip_lock`。

**为什么禁分配，而不只是"规定如此"**：lwIP 的 `memp` 没有任何内部锁，它只在
`g_lwip_lock` 这一个外部串行点下才安全（`net-lanes.md` 记录了同一个约束）。而
`mm/objcache.c` 不能拿来当逃生口——它的 miss 路径会先 `spin_unlock` 再调普通
`kmalloc()`（`objcache.c:41-42`），所以它对硬 IRQ 上下文不安全；只有命中 free list
的那条路径是无锁的，而 miss 必然发生。

这条约束的**直接后果**是：`net_bh_ring` 只能把 `net_bh_event_t` 按值内嵌，无法改成
"按需从 slab 分配"。因此每 socket 的收包 staging 成本是
`O(NET_BH_RING_SIZE × INLINE_PAYLOAD)`（DEFAULT/SERVER 档约 28 KiB/socket），
只能靠 profile 压小，无法靠结构消除。`sizeof(net_socket_t) <=
NET_PROFILE_SOCKET_MAX_BYTES` 的 `_Static_assert` 已经覆盖 ring size 与 inline
payload 两个因子。改造出路与代价见 [net-lanes.md](net-lanes.md) 的"缓冲改造的前提"。

### 允许的 callback 工作

callback 只能执行轻量、有界工作：

- 决定事件落在内联缓冲还是 spill 引用上。
- 更新少量 socket 状态标志。
- 记录需要由 bottom-half 处理的事件；不得在 callback 中直接进入 scheduler。
- 释放 lwIP 传入的 pbuf 引用（spill 已另行 `pbuf_ref()`）。

所有重工作，包括内存分配、队列插入、大块数据复制和 waiter wake，都必须推迟到底半部。bottom-half 在对象锁内 collect 带 `wait_seq` 的 wait entry，释放对象锁后 flush wake queue。

### 停在 accept stage 里的 pcb

accept stage 是唯一一个「pcb 已经归应用、但应用还没准备好处理它」的窗口，所以它有自己
的一套规则。踩错的表现是 `mem_free` 的 double free 断言，不是崩溃概率问题，而是必然。

- **返回 `ERR_OK` 之前必须装齐 recv/sent/err 回调。** `pcb->recv == NULL` 时 lwIP 会走它
  自己的 `tcp_recv_null()` 兜底，其 `p == NULL && err == ERR_OK` 分支直接
  `tcp_close(pcb)`，在应用毫不知情的情况下释放这块内存；stage 槽位还指着它，于是
  bottom half 采纳一个悬垂指针、teardown 再释放一次。**"已接管所有权" 与 "可以安全
  放置" 是两件事。**
- **回调里不得释放 pending 的 pbuf，除非自己拿 `g_lwip_lock`。** stage 期间收到的数据
  以引用形式挂在 `c->pending` 上（`pbuf_free()` 只在 `g_lwip_lock` 下安全），只能在
  err 回调——那里已经持有该锁——或 bottom half 的 `g_lwip_lock` 临界区里释放。
- **记住回调交给你的 pbuf 不要再 `pbuf_ref()`。** 正确写法是首次 `c->pending = p;`、
  其后 `pbuf_cat(c->pending, p);` 然后返回 `ERR_OK`；`pbuf_cat()` 本身会为被拼接的链
  取引用，多 ref 一次会多数一。
- **recv 回调返回 `ERR_MEM` 时不得 `pbuf_free()`。** `ERR_MEM` 的语义是 lwIP **没有**
  接管这个 pbuf：`tcp_in.c` 把它存进 `pcb->refused_data`，`tcp_process_refused_data()`
  之后还会再交回来。释放了就是第二次释放。要么不碰（交给 lwIP 重试），要么先把数据
  完整收下再返回 `ERR_OK`。当前实现的做法是先用 `bh_ring_reserve()` 整体判定容量，
  装得下就一次性全部转成内联事件，装不下原样返回 `ERR_MEM`。
- **槽位地址就是 pcb 的 `callback_arg`，所以槽位必须在 `tail` 越过它之前就与 pcb 脱钩。**
  bottom half 的采纳顺序固定为：先把 pcb `tcp_arg()` 到 child、清空槽位，再
  `__atomic_store_n(&st->tail, tail + 1)`。反过来就存在「生产者仍在往这个槽位投递事件」
  的窗口。
- **listener 消失时必须 purge stage。** `net_tcp_close_pcb()` / `net_tcp_drop_pcb()` /
  `net_inet_socket_destroy()` 三条拆除路径都要走 `net_inet_accept_stage_purge()`，
  否则已经握手成功的连接会被静默泄漏。

## Deferred Bottom-Half 实现

当前 bottom-half 不是独立 workqueue 线程。`a20_lwip_poll()` 在完成 lwIP progress 并释放 `g_lwip_lock` 后同步调用它；per-socket 定点入口也遵循同样的单锁规则。

### Bottom-half 职责

网络 bottom-half 执行 callback 不能完成的工作：

- 分配 `net_msg_t` 项并搬运 payload（内联源或 spill pbuf 源）。
- 将接收消息入队到 socket 接收队列。
- 更新 `closed`、`connected`、`tcp_connecting` 等 socket 标志。
- 通过该 socket 的桶锁唤醒被阻塞的 waiter。

### Top-half / bottom-half 拆分

lwIP callback 是 producer：

1. 检查 pbuf 并确定目标 socket。
2. 载荷装得下就拷进内联缓冲，否则 `pbuf_ref()` 并把引用记进槽位的 `owned[]`。
3. 原子提交 ring head 并设置 pending flag。
4. 调度 bottom-half。
5. 释放 lwIP 自己的 pbuf 引用。

bottom-half 是 consumer，对每条事件：内联源走 `net_enqueue_msg_locked_meta()`，spill 源走 `net_enqueue_msg_locked_pbuf()`。两者都只取该 socket 所属的桶锁。

## lwIP 锁下的分配规则

`kernel/include/core/lock.h` 禁止在持有 device 或 lwIP 锁时执行内存分配，除非 callee 被记录为非阻塞。

当前规则：

- 持有 `g_lwip_lock` 时不得调用 `kmalloc()`、`kfree()`、`net_msg_alloc()` 或任何 slab allocator 函数。
- callback 的 staging 是内联的，不需要分配；溢出走 spill 引用，也不需要分配。
- `net_msg_t` 分配和 socket 队列插入在 bottom-half，运行时不持有 `g_lwip_lock`。
- 如果某条代码路径在概念上处于 lwIP 临界区内但必须分配，先释放 `g_lwip_lock`，分配后重新获取。只有当本地 PCB 状态不需要在释放期间保持稳定时，这样做才安全。

## 资源档位

`kernel/net/net_profile.h` 按编译期档位给出所有上限：内联缓冲尺寸、ring 深度、pbuf 池、PCB 上限、每 socket 字节上界、RX 中断预算、lane 数。`lwipopts.h` 从这里取值，不再自带常量。

现有三档：`EMBEDDED`（单 lane、heap 供电的池、8 socket）、`DEFAULT`（QEMU 开发与冒烟构建）、`SERVER`。

两条跨档不变式由编译期断言保证：

- `MEMP_NUM_SYS_TIMEOUT >= MEMP_NUM_TCP_PCB`。KEEPALIVE / KEEPIDLE / KEEPINTVL 全开时每个 established PCB 持有一个 `sys_timeo`，池被耗尽后 `tcp_pcb_alloc()` 返回 NULL，表现为 `accept()` 失败而不是分配失败。
- `sizeof(net_socket_t) <= NET_PROFILE_SOCKET_MAX_BYTES`。socket obj_cache 会留活上百个对象，这个尺寸没有任何运行时计数器能反映。

`MEMP_MEM_MALLOC` 必须显式定义。留空会派生成 0，于是所有池变成 `.bss` 里的静态数组，档位里声明的 `MEM_SIZE` 完全不起作用——这正是此前"嵌入式档声称 16 KiB 堆却同时背着几百 KiB 静态池"的成因。

## 已知未完成

以下属于后续工作。每一项的**前置条件与阻塞原因**记录在
`docs/server-readiness.md` 的"剩余工作与各自的阻塞原因"一节，不要只按本节
的字面顺序动手：

- ~~`g_net_lock` 仍是全局的，1024 个 socket 在 recv/send/accept/close 上互相串行~~
  —— **已完成**（`2f17a5ba8`）。当时记的前置条件是"锁改造之前必须先做对象生命周期"，
  引用计数原语已随该提交落地（注册 +1 / 锁外注销 -1），`drain_active` 显式串行化
  取代了原先隐含在全局锁里的 single-consumer 保证。
  剩余的真问题不是串行度而是**验证缺口**：`2f17a5ba8` 的提交说明自己写明，
  引用计数不漏不重、`LOCK_COUNTERS_MAX` 注册预算、`-ENOTCONN` 窗口三项
  **运行期验证未做**，ASAN + SMP 压测也未做。待验证清单在
  `docs/measured/impl-notes-net.md`。
- `g_lwip_lock` 尚未分片。热路径（已建立 TCP 的收发）仍然全局串行，且每包仍
  遍历 lwIP 的全局 PCB 链表。分片的前置条件记录在 `net-lanes.md`：
  **memp 与 lane 上下文的前提没定之前不要动 memp**。
- netif 各有一块 `rx_frame[1536]` / `tx_frame[1536]` 暂存，单 netif 同时只能
  处理一个包。
- `st->ops->poll()` 与完整协议输入仍在中断上下文中执行，`g_lwip_lock` 仍从
  IRQ handler 获取。**不能简单改成"IRQ 只入队"**，会死锁，推理见
  `server-readiness.md`。
- 锁契约的**运行期强制**目前只覆盖 `g_lwip_lock`（`LWIP_ASSERT_CORE_LOCKED()`，
  见「核心锁断言」一节）。桶锁这一侧**没有**对应断言：没有"当前 CPU 是否持有
  期望的桶锁"的探针，`net_bucket_lock2()` 的升序约定同样只有代码评审在把关。
  这是本文与实现之间最大的一处落差。

## 迁移检查清单

更新网络实现以符合本契约时，逐项确认（当前实现已满足）：

- [x] lwIP callback 不再调用 `kmalloc()` 或 `kfree()`。
- [x] lwIP callback 不再获取任何 socket 表桶锁。
- [x] lwIP callback 只执行有界工作并调度 bottom-half（`net_inet_bh_schedule`）。
- [x] bottom-half 只持有该 socket 的桶锁运行，且不持有 `g_lwip_lock`。
- [x] socket send/recv/connect/listen/accept 路径遵循本文档的锁顺序。
- [x] `a20_lwip_poll_locked()` 在持有 `g_lwip_lock` 时调用仍然安全。
- [x] `g_lwip_lock` 下的驱动路径保持非阻塞。
- [x] 三条 socket 发送路径每次迭代只取一次 `g_lwip_lock`。
- [x] 定时器中断只推进 timers 段，收包排空受 `CONFIG_NET_RX_IRQ_BUDGET` 限制。
- [x] 提前停止排空时保留 RX pending 标志。
- [x] spill 引用的释放只发生在 `g_lwip_lock` 下（ring 槽位绕回或 socket 销毁）。
- [x] 并发 socket stress 测试通过，且没有锁顺序告警。
- [x] 跨 socket 的临界区用 `net_bucket_lock2()`，桶号升序；同时最多持两把桶锁。
- [x] 全表扫描（`net_socket_table_walk()` 等）一次一把，取了就放，不取全组。
- [x] `net_register_socket_locked()` 不在持有别的桶锁时被调用。
- [x] `g_lwip_lock` 与任何桶锁不同持。
- [x] `LWIP_ASSERT_CORE_LOCKED()` 已接线到 `g_lwip_lock` 持有者 CPU
      （默认关闭，开关语义与可见性见「核心锁断言」一节）。
- [ ] 桶锁一侧**没有**对应断言：`net_bucket_lock2()` 的升序与"至多两把"目前只有
      评审在把关。补一个 `CONFIG_NET_BUCKET_ASSERT` 探针。
