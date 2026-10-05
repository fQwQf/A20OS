# g_net_lock 分片实现笔记（net agent）

工作树：`/home/fqwqf/OS/A20OS-locks`（分支 `feat/lock-serialization-split`）。
设计：`docs/roadmap/lock-serialization-split.md` §2。
本轮范围：**只分片 socket 表 + slot 分配元数据**，不触碰 per-socket 队列的协议本身。

---

## 0. 现状事实（本次会话实测）

| 事实 | 证据 |
|---|---|
| `g_net_lock` 全树文本引用 235 处 | `grep -rn "g_net_lock" kernel/ \| wc -l` → 235 |
| 其中**真正的取锁点 54 处** | `grep -rn "spin_lock_irqsave(&g_net_lock)" kernel/ \| wc -l` → 54 |
| 从未注册进 `lock_counters` | 全树 `lock_counters_register` 只出现在 `kernel/proc/proc.c:365`、`kernel/proc/sched.c:589`、`kernel/net/lwip_stack.c:369`、`kernel/fs/block_cache.c:332`、`kernel/fs/page_cache.c:507`、`kernel/fs/vfs/dcache.c:108` |
| 定义与初始化 | `kernel/net/socket.c:20` `spinlock_t g_net_lock = SPINLOCK_INIT;`；仅 `net_init()` `kernel/net/socket.c:164-165` 做 `spin_init` |
| 空闲位图 | `kernel/net/socket_registry.c:15` `static uint32_t g_sock_free[(NET_MAX_SOCKETS + 31) / 32];`，注册 `:46-67`、注销 `:68-94` 都是整字 read-modify-write |
| `NET_MAX_SOCKETS` | 服务器档 65536（`kernel/net/net_profile.h:108`），默认档 1024（`:136`），**embedded 档 8**（`:62`）。第 62 行这一条开工时普查漏了，是实现期被 `_Static_assert` 逼出来的，见 §2 |
| 既有不变量 | `kernel/net/socket_packet.c:6-8`、`kernel/net/socket_table.c:8-11`、`kernel/include/core/lock.h:35`：**g_lwip_lock 与 g_net_lock 永不同时持有** |

---

## 1. 普查表：54 个取锁点分类

分类判据是**这个临界区保护的是哪一组字段**，不按是否在热路径分。
"表" = `g_sockets[]` / `g_sock_free[]` 的槽位成员关系；"per-socket" = 只有一个
`net_socket_t` 的字段；"统计" = 全局计数器。

> **本表的行号是改造前（`git stash` 前的 HEAD）的坐标**，符合任务书"先落盘再动
> 代码"的顺序。改造后行号会漂移，用本表定位请按函数名而不是行号。

| # | 位置 | 分类 | 涉及的 socket | 桶方案 |
|---|---|---|---|---|
| 1 | `socket.c:182` `net_format_status` | 表 + 统计 | 全部 | 逐桶 |
| 2 | `socket.c:267` `net_socket_create`→register | 表 | 新 socket（未注册） | register 自持桶锁 |
| 3 | `socket.c:296` `net_socketpair_create` | per-socket ×2 | `sa`,`sb` | 有序双桶 |
| 4 | `socket.c:376` `net_bind_sock` | 表 + per-socket | `s` + 全表搜索 | 有序双桶（搜索逐桶，命中后再取 `s` 的桶） |
| 5 | `socket.c:411` `net_connect_sock` | per-socket | `s` | 单桶 |
| 6 | `socket.c:460` `net_sendto_raw_ipv6`（绑定 `s`） | per-socket | `s` | 单桶 |
| 7 | `socket.c:476` 同上（扫描投递） | 表 | 全表 dst | 逐桶（`s->local` 先拷出） |
| 8 | `socket.c:550` `net_sendto_sock` | per-socket ×2 | `s`,`dst` | 有序双桶 |
| 9 | `socket.c:656` `net_recvfrom_socket_meta`（ch_ep 分支，两段） | per-socket | `s` | 单桶 |
| 10 | `socket.c:736` 同上（legacy 队列入 park 前复查） | per-socket | `s` | 单桶 |
| 11 | `socket.c:765` 同上（park 后复查） | per-socket | `s` | 单桶 |
| 12 | `socket.c:857` 同上（legacy 分支 dequeue 段） | per-socket | `s` | 单桶 |
| 13 | `socket_control.c:267` `net_accept_sock`（pop 段） | per-socket | `s` | 单桶 |
| 14 | `socket_control.c:304` 同上（park 前复查） | per-socket | `s` | 单桶 |
| 15 | `socket_control.c:353` 同上（park 后 link） | per-socket | `s` | 单桶 |
| 16 | `socket_control.c:391` `net_accept_sock` 安装失败回滚 | per-socket ×2 | `child`,`peer` | 有序双桶 |
| 17 | `socket_control.c:879` `net_shutdown_sock` | per-socket ×2 | `s`,`peer` | 有序双桶 |
| 18 | `socket_control.c:900` 同上（SHUT_RD 清 rx 后重取） | per-socket | `s` | 单桶 |
| 19 | `socket_control.c:945` `net_set_nonblock_vfile` | per-socket | `s` | 单桶 |
| 20 | `socket_control.c:956` `net_set_nonblock` | per-socket | `s` | 单桶 |
| 21 | `socket_control.c:968` `net_poll_file` | per-socket | `s`(+`s->peer` 只读) | 单桶 |
| 22 | `socket_netlink.c:132` `net_netlink_bind` | per-socket | `s` | 单桶 |
| 23 | `socket_netlink.c:178` `net_netlink_diag_request` | 表 + per-socket ×2 | 全表 + `requester` | 逐桶（每桶与 requester 的桶有序取） |
| 24 | `socket_netlink.c:309` `netlink_uevent_emit` | 表 | 全表 dst | 逐桶 |
| 25 | `socket_netlink.c:805` `net_netlink_route_request` | per-socket | `requester` | 单桶（只往 requester 入队） |
| 26 | `socket_alg.c:130` `net_alg_socket_bind` | per-socket | `s` | 单桶 |
| 27 | `socket_alg.c:161` `net_alg_socket_accept`→register | 表 | 新 socket | register 自持桶锁 |
| 28 | `socket_file.c:32` `net_vfile_read`（dequeue 段） | per-socket | `s` | 单桶 |
| 29 | `socket_file.c:84` 同上（park 前复查） | per-socket | `s` | 单桶 |
| 30 | `socket_file.c:130` `net_vfile_write` | per-socket ×2 | `s`,`dst` | 有序双桶 |
| 31 | `socket_file.c:209` `net_socket_close_file` 主体 | per-socket ×2 | `s`,`peer` | 有序双桶 |
| 32 | `socket_file.c:270` 同上（每个 accepted child） | per-socket ×2 | `accepted`,`peer` | 有序双桶，逐 child |
| 33 | `socket_file.c:300` 同上（unregister accepted） | 表 | `accepted` | 单桶 |
| 34 | `socket_queue.c:269` `net_enqueue_msg_blocking` 第 1 段 | per-socket ×2 | `s`,`dst` | 有序双桶 |
| 35 | `socket_queue.c:336` 同上 第 2 段（park 后） | per-socket ×2 | `s`,`dst` | 有序双桶 |
| 36 | `socket_unix.c:114` `net_unix_socket_bind` | 表 + per-socket | 全表搜索 + `s` | 有序双桶 |
| 37 | `socket_unix.c:133` 同上（vfs_open 失败回滚） | per-socket | `s` | 单桶 |
| 38 | `socket_unix.c:158` `net_unix_socket_connect` | 表 + per-socket ×3 | 全表搜索 + `s` + `listener` + `child` | 有序三桶（child 未注册，register 自持桶） |
| 39 | `socket_unix.c:243` `net_unix_socket_sendto_impl` | per-socket ×2 | `s`,`dst` | 有序双桶 |
| 40 | `socket_unix.c:343` `unix_ch_send`（写 `dst->ch_cred_*`） | per-socket | `dst` | 单桶 |
| 41 | `socket_table.c:51` `net_socket_table_walk` | 表 | 全部 | 逐桶（回调仍在桶内，见 §4） |
| 42 | `socket_table.c:89` `net_socket_rx_available` | per-socket | `s` | 单桶 |
| 43 | `socket_packet.c:185` `net_packet_deliver_locked` 调用点 | 表 | 全表 dst | 逐桶 |
| 44 | `socket_packet.c:224` `net_packet_socket_bind` | per-socket | `s` | 单桶 |
| 45 | `lwip_stack.c:732` lane 统计 | 表 + 统计 | 全部 | 逐桶 |
| 46 | `socket_inet.c:1103` `net_inet_bottom_half_process_socket` | per-socket | `s` | 单桶 |
| 47 | `socket_inet.c:1134` `net_inet_bottom_half_process_all` | 表 | 全部 | 逐桶 |
| 48 | `socket_inet.c:1355` `net_inet_connect_stream`（补 loopback 绑定） | per-socket | `s` | 单桶 |
| 49 | `socket_inet.c:1373` 同上（本地 listener 路径） | 表 + per-socket ×3 | 全表搜索 + `s` + `listener` + `child` | 有序双桶 + register 自持桶 |
| 50 | `socket_inet.c:1458` `net_inet_connect_stream` 等待段 | per-socket | `s` | 单桶 |
| 51 | `socket_inet.c:1477` 同上（park 后复查） | per-socket | `s` | 单桶 |
| 52 | `socket_inet.c:1660` `net_inet_send_udp` | per-socket ×2 | `s`,`local_dst` | 有序双桶 |
| 53 | `socket_inet.c:1772` `net_inet_send_tcp`（local_tcp 分支） | per-socket ×2 | `s`,`dst` | 有序双桶 |
| 54 | `socket_inet.c:1829` 同上（写 waitq 时的 link） | per-socket | `s` | 单桶 |

### 1.1 按 `reg_idx` 索引、隐含"全树只有一把 g_net_lock"的两张全局表

| 位置 | 现状 | 处理 |
|---|---|---|
| `socket_packet.c:53` `g_pkt_bound_slots[(NET_MAX_SOCKETS+31)/32]`，`:61-72` 整字 RMW | 全树唯一在 lwIP 接收侧无锁读的 census 位图 | 搬进 `net_socket_t.pkt_bound_marked`；`g_pkt_bound_count`（`:53`）保持原子，语义不变 |
| `socket_queue.c:32` `g_rxq_tally[NET_MAX_SOCKETS]`（服务器档 512 KiB），`:34/:38/:47-57/:63-73/:85-91` | 按 slot 索引的接收队列字节/条数合计 | 搬进 `net_socket_t.rxq_tally`；`net_rxq_reset_slot(idx)` → `net_rxq_reset_locked(s)` |

---

## 2. 分桶方案

- **桶号 = `slot >> NET_SOCK_BUCKET_SHIFT`，连续分区**，不是哈希。与
  `vfs_dcache_invalidate_all`（`kernel/fs/vfs/dcache.c:355-375`）同一形态。
- 空闲位图搬进桶内（`net_bucket_t.free_bits[]`），**一个位图字只属于一个桶**，
  注册/注销只 RMW 自己的字，不存在"同一字里的 32 个 slot 分属不同桶"这种跨桶
  踩字（这是复核推翻旧稿的核心理由）。
- **桶大小按 profile 导出，不是写死的 512**。实现时发现写死 512 会直接打破
  `kernel/net/net_profile.h:62` 的 **embedded 档**：它的表只有 **8** 个 slot，
  512 会让 `NET_SOCK_BUCKETS` 变成 0，既触发 `_Static_assert` 又让桶名表越界。
  现按 `NET_MAX_SOCKETS` 分档（`socket_internal.h:456-472`）：

  | profile | `NET_MAX_SOCKETS` | shift | 桶内 slot | 桶数 |
  |---|---|---|---|---|
  | `CONFIG_NET_PROFILE_SERVER` | 65536 | 9 | 512（16 字） | 128 |
  | `CONFIG_NET_PROFILE_DEFAULT` | 1024 | 5 | 32（1 字） | 32 |
  | `CONFIG_NET_PROFILE_EMBEDDED` | 8 | 0 | 1（1 字，尾部 31 bit 钉死占用） | 8 |

  三档的 slot 数都是 2 的幂，所以分档是精确的；另加一条 `_Static_assert` 把
  "必须是 2 的幂"钉住。桶窄于一个字时位图多出来的 bit 在
  `net_socket_registry_init()` 里被置成"占用"，否则 claim 会把桶外的偏移发给
  下一个桶。
- 注册从 `cpu_current_id() % NET_SOCK_BUCKETS` 起**环形**扫桶，使并发注册首先
  落在不同桶上；每个候选桶单独取锁、单独释放，找到空槽即认领并退出。
- `g_sockets[]` 保持按 slot 的扁平数组（`slot -> socket` 的 O(1) 映射），成员关系
  的读写由 `bucket(slot)` 的桶锁保护。

### 2.1 锁序

- 任意时刻**至多持两把桶锁**，且必须**按桶号升序**。因此不存在 ABBA：
  两条路径即使反向申请（`a->b` 与 `b->a`）也会被强制排成同一方向。
- **禁止同时持多把桶锁遍历全表**（`dcache.c:355-370` 记录了这条路会 livelock：
  "every VFS_DCACHE_BUCKET_LOCKS lock was held, with interrupts disabled … an
  interrupt that reached any lookup spun on a lock its own interrupted context
  was holding"）。遍历一律逐桶：取桶 → 扫 512 个 slot → 释放。
- `g_lwip_lock` 与**任何**桶锁不同时持有，与今天 `g_lwip_lock` 与 `g_net_lock`
  的不变量语义完全一致。

### 2.2 未注册 socket 的桶

`reg_idx < 0` 的 socket 没有槽位，也就没有桶。它只在两个时刻可见：创建中
（私有，别的 CPU 拿不到指针）和 close 竞态中（正在被拆）。这两种情况都落到
**`NET_SOCK_ORPHAN_BUCKET`（编号 = `NET_SOCK_BUCKETS`，即数组最后一格，
因此排在任何有序双桶的最后）**，进去之后第一件事就是复查 `s->in_registry`，
不成立就直接返回错误。创建/销毁是唯一会走到这把锁的流量。

### 2.3 引用计数（本次新增，设计稿缺）

设计 §2.2.4 让遍历"在桶锁下 `proc_get()` 引用"，但 `net_socket_t` **不是
`task_t`**，`proc_get()` 是 task 引用计数（`kernel/proc/task.c:377`、
`kernel/include/proc/proc.h:539-550` 的 TASK_REFERENCE_LIFETIME 段），用在
socket 上是类型错误。改为在 `net_socket_t` 上自建 `refs`：

- `net_socket_alloc()` 置 1（创建者那一把）。
- 注册进 `g_sockets[]` 期间登记表持有 1，`net_unregister_socket_locked()` 释放。
- **任何把 socket 指针带出桶临界区的路径必须自己 `net_socket_ref()`**，
  用完再 `net_socket_free()`。
- 没有单独的 `net_socket_unref()`：`net_socket_free()` 本身就是引用计数的
  最后一环（`socket.c:38`），它先 `__atomic_fetch_sub(&s->refs, 1)`，非 1 就返回。
  早先写成独立的 `net_socket_unref()` 是错的——`net_socket_free()` 还要释放
  `s->ch_buf` 并 `obj_cache_free()`，把这两件事拆开会在计数归零时漏掉资源回收。

这条不只是为了分片：今天 `net_socket_from_file()`（`socket_file.c:665`）
先 `vfs_put_file_ref()` 再返回裸指针，调用方随后才取 `g_net_lock`，中间另一个
CPU 完成 close 就会 use-after-free —— 这个洞在分片前就存在，分片后从"理论"
变成"常规路径"，所以必须补。

---

## 3. 保留的既有语义（逐条核对）

1. `closed` 与注销仍在**同一个临界区**（`net_socket_close_file` 先
   `net_unregister_socket_locked` 后 `s->closed = 1`，全程同一把桶锁），见 §4。
2. `net_wait_queue_collect_all_locked` 在锁内排空、`wait_queue_wake_all` 在锁外
   唤醒的配对不变；`proc_wake_q_flush` 仍在锁外。
3. 排队 FIFO、`net_msg_t` 生命周期、`rx_count` 与 `rxq_tally` 的"高 32 位是条数、
   低 32 位是字节"打包语义，全部逐位保留。
4. `g_lwip_lock` ↔ 桶锁互斥不变；`net_tcp_recved()` 等仍刻意在桶锁之外调用。
5. `net_poll_file()` 里读 `s->peer->rx_count`：现在 `s->peer` 可能与 `s` 不同桶。
   该读只用于判断"对端队列满 → 不报 POLLOUT"，读到略旧值的后果是 poll 多返回/
   少返回一次 `POLLOUT`，不影响正确性；记为**有意放宽的弱一致读**，不额外取桶锁
   （取了对端的桶会把 poll 变成一个会跟数据面抢桶的读路径）。

---

## 3bis. 实现中发现的第二处 register-under-lock，以及它带来的不变量变化

普查表把 `socket_inet.c:908`（accept drain）与 `socket_inet.c:1393` /
`socket_unix.c:158`（connect 快捷路径）标成"register 自持桶"，但实现时发现这
三处**当时仍持着 listener/s 的桶锁**，因此它们的桶号只能事后补记。两处按下面
的方式拆开，语义逐条对照过：

**(a) `net_inet_accept_stage_drain()`（`socket_inet.c:798`）**
原来整个循环都在 `g_net_lock` 里。现在把 listener 的桶访问拆成两次短窗口：

1. 第一次：读 `accept_count` 闸门 + 把 listener 的字段拷进新 child，随后释放；
2. 中间无锁：`a20_lwip_lock()` 做 pcb handoff，`net_register_socket_locked()`
   注册 child；
3. 第二次：复查 listener 仍然 `in_registry && !closed`，`net_accept_queue_push_locked()`、
   `net_inet_bh_schedule()`、`net_event_notify()`、`wait_queue_collect_one()`，
   push 失败时在同一窗口里 `net_unregister_socket_locked()` 回滚。

"先注册后 push"的顺序是保留的：`net_inet_bh_schedule()` 依赖 `s->reg_idx` 去
`net_bh_slot_mark()`，所以 child 必须先有槽位。失败路径现在释放**两个**引用
（创建者的 + 注册时加的）。

这条拆分顺带把一个**既有违规**修好了：原来 drain 持着 `g_net_lock` 去取
`a20_lwip_lock()`（`lwip_stack.c:66-77` 的注释当时如实记录了"`g_net_lock` 和
`g_lwip_lock` ARE held together"）。拆分之后 accept 路径上两者不再同时持有，
`lock.h:35` 的 `g_lwip_lock -> g_net_lock` 单向顺序成为**处处成立**而不只是
"今天恰好没有反向路径"。`lwip_stack.c` 那段注释已按事实改写，没有粉饰。

**(b) `net_inet_connect_stream()`（`socket_inet.c:1541`）与
`net_unix_socket_connect()`（`socket_unix.c:144`）**
原来都是"一把大锁里：查表 → 建 child → 注册 → push"。现在：表扫描先在无锁状态
下跑完并带回**一个引用**，据此填好 child，无锁注册，然后再取
`net_bucket_lock2(s 的桶, listener 的桶)` 做 s↔child 连线、push 和唤醒。
两次取锁之间 listener/s 可能已被 close，取到有序对之后**必须复查
`in_registry`**；不成立就走和原来 `push` 失败一样的回滚分支。
AF_UNIX connect 里的 child 与今天一样**不注册**（`reg_idx` 恒为 -1，落 orphan
桶），这是原代码就有的行为，本轮原样保留。

**(c) 新发现的单消费者不变量**（实现时才暴露，必须记下来）
`socket_inet.c:328` 的注释宣称 `bh_ring` 是 single-consumer，靠的是"全局只有
一把 `g_net_lock`"。分片后 `net_inet_bottom_half_process_all()` 逐桶取锁，两个
CPU 可以同时进同一个 listener 的 drain，各自读到同一个 `st->tail`，于是
**同一个 stage slot 被 release 两次**。这是既有不变量的真实破口，不是新代码
引入的，但在分片下变成可达。处置：给 `net_accept_stage_t` 加
`int drain_active`，由 listener 的桶锁保护，进入 drain 时抢占、退出时释放；
抢不到的第二次 drain 直接返回（ring 本来也不会在这一趟被排空，
`net_inet_bh_schedule()` 会重新标脏）。**注意**：`bh_ring` 本身的
single-consumer 仍然成立——slot 索引唯一属于一个桶，per-socket 的 bottom half
处理全程持该桶锁，所以两个 CPU 可以并行处理**不同** socket，但不会并行处理
**同一个**。

---

## 4. 与设计稿的偏离及理由

**偏离：`net_socket_table_walk()` 的回调仍在桶锁内执行**（设计 §2.2.4 第 2 条
要求"释放桶锁再执行 `fn(s, arg)`"）。

理由：设计自己保留了"`closed` 与注销在同一临界区"的语义（§2.2.4 第 3 条），
而该文件头 `socket_table.c:15-23` 把这句话明确写成 load-bearing 不变量。逐桶执行
回调同样满足"至多一把桶锁"这条真正的硬约束（禁止同时持 128 把），而且比设计
要求的形式**更强**：回调看到的 socket 在它自己那一桶的临界区内仍是
`in_registry` 的引用持有者。设计要求的形式会引入一个新的弱化：procfs 行可能在
回调期间读到已被 close 释放后重写的字段。风险收益不划算，故保留锁内回调，
并把这一点写进本笔记而不是默默照抄。

不变量改写（对应设计 §2.2.4 第 3 条，逐桶一致而非全局一致）：

> 遍历看到的每个 socket 在其所在桶的临界区内都还是 `in_registry` 的引用持有者；
> 遍历不保证看到跨桶的同一时刻快照。遍历期间被并发 `close()` 的 socket 可能被
> 看到（`closed` 已置位，`net_table_claims()` 对它返回假或由 procfs 行渲染自行
> 过滤），但绝不会看到一个已被注销、slot 已释放的 socket。

---

## 5. 待并入 `kernel/include/core/lock.h`（net agent 无权编辑该文件）

把下面这段交给 `kernel/include/core/lock.h` 的 owner（proc agent）并入网络段
（当前 `lock.h:35` 的 `g_lwip_lock -> g_net_lock` 一行所在处）。

### 5.1 全局顺序链（替换 `lock.h:35` 的 `g_lwip_lock -> g_net_lock` 一行）

```c
 *   g_lwip_lock -> net_bucket[*]
```

### 5.2 规则段新增/改写（改写自实际实现，不是草案）

```c
 * - Socket-table shard locks: the socket registry is sharded into
 *   net_bucket[0 .. NET_SOCK_BUCKETS-1], one lock per contiguous run of
 *   NET_SOCK_SLOTS_PER_BUCKET registry slots (512 on the server profile, 32
 *   on the default one, 1 on the embedded one; see socket_internal.h).  The
 *   bucket of a socket is reg_idx >> NET_SOCK_BUCKET_SHIFT.  A per-socket
 *   critical section takes the one bucket that owns the socket; a section
 *   that touches two sockets takes their two buckets in ascending bucket
 *   order, so no pair is ever held in two directions and the order is acyclic.
 *   At most two bucket locks are held at any time.
 * - Never hold more than one bucket lock while walking the table: taking all
 *   NET_SOCK_BUCKETS with interrupts disabled livelocks against an interrupt
 *   that reaches a lookup on a lock its own interrupted context was holding
 *   (see the same failure recorded for VFS dcache in kernel/fs/vfs/dcache.c).
 *   net_socket_table_walk() and every other table scan walk bucket by bucket,
 *   taking and releasing one bucket at a time.
 * - g_lwip_lock is never held together with any net bucket lock.  This is the
 *   unchanged meaning of the old "g_lwip_lock and g_net_lock are never held
 *   together" rule (docs/net/network-lock-contract.md), and it is now true
 *   everywhere rather than "true except on the accept path" -- see section 3.5.
 * - A socket with reg_idx < 0 (being created, or racing a concurrent close)
 *   has no bucket.  Such accesses take net_bucket[NET_SOCK_ORPHAN_BUCKET],
 *   re-check s->in_registry, and bail out if it is clear.
 * - net_register_socket_locked() takes shard locks itself, one at a time, so
 *   it must NOT be called with another socket's bucket lock held.  Callers
 *   resolve their counterpart (listener, peer) through a search that returns a
 *   reference, register with no lock held, and only then take the ordered
 *   pair.  This is the one rule that is easy to break by accident: every new
 *   accept/connect path has to be written in that shape.
 * - Carrying a net_socket_t pointer out of a bucket critical section requires
 *   net_socket_ref() / net_socket_free().  The bucket lock can no longer be
 *   what keeps the object alive, because a lookup that finds a socket in one
 *   bucket and then needs a second bucket cannot keep the first one held.
```

### 5.3 文件内注释：本轮已自行同步的（不再需要别人代劳）

| 位置 | 改法 |
|---|---|
| `socket_internal.h:143/:150/:706` | `g_net_lock` → "socket-table bucket lock" |
| `socket.c:45/:213` | 已按"全局锁已消失"的现状改写 |
| `socket_table.c` 头注释 | 逐桶 + §4 的不变量改写 |
| `socket_side.h:10` | 说明两张表为何必须跟着 `net_socket_t` 走 |
| `socket_unix.c`、`socket_inet.c` 全部残留注释 | 已改 |
| `socket_netlink.c` 3 处（`:527/:772/:821`） | 已改为 bucket lock 措辞 |
| `lwip_stack.c:58-78` | 注释里那段"`g_net_lock` 和 `g_lwip_lock` ARE held together"按**新的事实**重写（accept 路径已不再嵌套），并保留"顺序仍是单向"的理由 |
| `lwip_stack.c` 其余 4 处 | 桶锁措辞 |
| `net_lane.h:36-37` | 标记 E 已提前完成（分片 + `refs` 引用计数） |
| `virtio_net.c:540` | 桶锁措辞 |

### 5.4 **仍需别人代劳**的一处：`kernel/include/core/lock.h`

`lock.h` 归 proc agent，本轮**一个字都没改**（`lock.h:35/:43/:45/:48` 四处
`g_net_lock` 字样原样保留）。上面 §5.1/§5.2 就是要并进去的文本。

`kernel/fs/procfs/procfs_render.c` 的 5 处（`:394/:470/:492/:501/:526`）也**没改**：
它在 `kernel/fs/` 下，属于 proc agent 的文件。本轮该文件只是**重新编译通过**
（`-fsyntax-only`），没有改一个字。这些注释现在描述的是一把已经不存在的锁，
语义上不致命（都只是解释性文字），但会误导读锁契约的人，建议一并替换。

## 6. 复核意见的逐条处置

| 复核条目 | 处置 |
|---|---|
| "§2.2 每桶保护 slot 位图位段不可实现" | **采纳**。连续分区，位图搬进 `net_bucket_t.free_bits[]`，一个字只属于一个桶。见 §2。 |
| （本轮自查新增）"固定 512 slot/桶在 embedded 档不可行" | 实现期发现并修正：`net_profile.h:62` 的 embedded 档只有 8 个 slot。桶大小改为按 `NET_MAX_SOCKETS` 分档导出（512/32/1 slot），并加两条 `_Static_assert`。见 §2 的表。 |
| "§2.1 把引用数当取锁点数（235 vs 54）" | **采纳**。普查按 54 个取锁点做，见 §1；235 这个数字只出现在 §0 的现状事实里，并标注它不是范围依据。 |
| "§5 g_net_lock 从未注册进 lock_counters" | **采纳（部分）**。桶锁全部注册进 `lock_counters`，名字 `net_bucket_<n>`（init 时 `snprintf` 生成，因为桶数随 profile 变，写死的名字表在 embedded 档会越界——这是 `-Werror` 抓到的一个真实缺陷）。**未采纳的部分**：`LOCK_COUNTERS_MAX` 只有 192（`kernel/core/lock_counters.c:13`），128 个桶锁会把表吃掉大半，而 `proc.c:365`/`sched.c:589` 之后 proc agent 还要给 `tasklist_lock` 留位置。已把这条预算冲突记在 §8 待验证，扩容 `LOCK_COUNTERS_MAX` 属 `kernel/core/`，超出 net agent 文件所有权，留给集成阶段。 |
| "§5 噪声纪律/负载形态/`make bench-locks` 已过期" | **不属于本轮任务**（任务书只要求 §2 分片）。原样记录：这些是 §5 的问题，且 `tools/lock_bench.py` 的 `TRACKED_LOCKS = ["proc","lwip","slab","runq"]` 里 `"slab"` 至今没有对应的 `lock_counters_register`，属于既有缺口。 |
| "§4 文件所有权表把 lock.h 给了 net agent" | **采纳**。本 agent 不编辑 `kernel/include/core/lock.h`，需要并入的文字整段写在 §5，交给该文件 owner。 |
| "lwip 侧（g_lwip_lock 本体）分桶" | **出范围**，见 §7。 |
| "slab per-CPU 数组已实现" | 与 net agent 无关，确认：本轮未触碰 `kernel/mm/`。 |

---

## 7. followup：lwIP 侧（下一轮）

只普查，不实施。`g_lwip_lock` 要分桶必须先桶化 lwIP 自己的三个全局链表
（`tcp_active` / `tcp_bound_pcbs` / `udp_pcbs`），而它们在
`kernel/external/lwip/**`（vendored，有 DIVERGENCE.md 治理）。

本轮顺带确认的一条事实：`net_inet_connect_stream` 的本地 listener 路径
（`socket_inet.c:1373-1420`）在**持有 net 侧锁**的状态下调 `net_tcp_drop_pcb(s)`
——不对，重读后确认那次调用在 `:1421` 已经**释放**了 net 锁之后；
`lwip_stack.c:70-74` 的注释描述的正是这个"先 lwip 后 net / 先 net 后 lwip"
单向顺序。分片后该顺序不变，因为桶锁与 `g_lwip_lock` 依然互斥且不嵌套。

---

## 8. 待验证（本轮无法在 agent 内验证）

> **2026-10-06 更新**：本节 §8.2 / §8.3 / §8.5 / §8.6 / §8.7 已由
> `067d0eb96` 与 `2dd28758c` 落成运行期的东西，逐条状态见 **§11** 的对照表。
> 下面保留原文，因为它记的是"当时为什么没法验证"，而那个"为什么"本身仍然是
> 读这份笔记的人需要知道的。§11.2 记的 AF_UNIX 缺陷正是从 §8.3/§8.6 这两条
> 长出来的——账本接上之后第一次运行就抓到了。

1. **构建与冒烟**：任务书禁止本 agent 跑 `make` / QEMU，因此 **没有链接过、
   没有跑过一次内核**。集成阶段必须先 `make` 一次再进 `smoke-smp-lock-contention`。

   本轮**做过**的是逐翻译单元的 `-fsyntax-only`，三个 profile 各跑一遍，
   `-Wall -Wextra -Werror` 全绿（未写任何构建产物，输出全部丢弃）：

   ```
   B=.kernel-build/riscv64-qemu-virt-riscv64-both-dev-embedded-external-root-smp8
   FLAGS="-Wall -Wextra -O1 -ffreestanding -nostdlib -fno-builtin -fno-common \
     -std=gnu99 -Ikernel/arch/riscv64/include -Ikernel/include -Ikernel \
     -Ikernel/net/lwip_port -Ikernel/external/lwip/src/include \
     -Ikernel/external/littlefs -Ikernel/external/littlefs/compat \
     -Ikernel/platform/qemu-virt-riscv64 -I$B/generated \
     -march=rv64imafdc_zicsr_zifencei -mabi=lp64 -mcmodel=medany \
     -DRISCV64 -DCONFIG_RISCV64 -DCONFIG_ABI_BOTH -DCONFIG_NR_CPUS=8 \
     -DCONFIG_BOARD_QEMU_VIRT_RISCV64 -DCONFIG_NET_LANES=4 -DCONFIG_NET_PROFILE=$P"
   for f in kernel/net/*.c; do riscv64-linux-gnu-gcc -fsyntax-only $FLAGS $f; done
   ```

   覆盖文件：`kernel/net/*.c` 全部 12 个（`socket_registry.c` `socket.c`
   `socket_control.c` `socket_file.c` `socket_queue.c` `socket_packet.c`
   `socket_table.c` `socket_unix.c` `socket_inet.c` `socket_netlink.c`
   `socket_alg.c` `lwip_stack.c`），`P` 取 1/2/3 三轮。

   另外把所有 `#include "net/socket_internal.h"` / `"net/socket_side.h"` 的
   19 个文件（`abi/linux/sys_fs.c` `abi/linux/sys_socket_msg.c`
   `abi/native/sys_native_net.c` `core/progress.c` `drivers/core/driver_class.c`
   `fs/procfs/procfs_render.c` `fs/sysfs.c` 加上面 12 个）在默认档跑了一遍，
   全绿——确认头文件改动没有打断任何调用方。
   注意这份清单里有 `drivers/core/driver_class.c`、`fs/sysfs.c`、
   `abi/native/sys_native_net.c` 等**别人也在改**的文件；它们通过说明我的头
   改动是向后兼容的，但如果集成阶段报它们的错，要先看是不是别人改坏的。
   **这条证据只覆盖语法、类型、`-Werror` 级别的告警和 `_Static_assert`**，
   不覆盖链接、未使用的静态函数、以及任何运行期行为。
   它已经抓到两个真实缺陷：桶名表按 128 个名字写死、在 embedded 档越界；
   以及桶窄于一个字时 `NET_SOCK_BUCKET_WORDS` 为 0 触发静态断言。
2. `LOCK_COUNTERS_MAX` 预算：128 个桶锁 + `proc` + `runq`×CONFIG_NR_CPUS +
   `lwip` + `block_cache` + `page_cache` + `dcache` 已接近 192，`tasklist_lock`
   是否还有位置需要在集成阶段实测 `/proc/a20/lock_contention` 的注册条数。
3. **引用计数的漏点**：`net_socket_ref/unref` 是新引入的原语，任何"把指针带出
   桶临界区"的路径漏掉一次 `unref` 就是泄漏，漏掉一次 `ref` 就是 use-after-free。
   本轮已逐个走查 54 个点，但**没有运行期证据**（需要 ASAN/压力测试）。
4. ~~`net_socket_rxq_tally` 搬进 `net_socket_t` 后
   `NET_PROFILE_SOCKET_MAX_BYTES` 的 `_Static_assert` 是否仍通过~~
   —— **已确认通过**：三个 profile 的 `-fsyntax-only` 都会实例化这条断言
   （`socket_internal.h:382`），全部编译通过。
5. 有序双桶锁下 `s` 的桶在"读 `reg_idx` → 取桶"之间可能已被注销而取错桶。
   本轮的处置是取到桶后立刻复查 `s->in_registry`，不成立即返回错误。
   这个窗口的可达性依赖 close 与 send 的实际交错，需要压力测试确认不会
   表现为静默的 `-ENOTCONN` 泛滥。
6. **`net_socket_free()` 的引用计数语义是本轮新引入的、编译期无法验证的部分。**
   一句话规则：注册进表 +1，`net_unregister_socket_locked()` 之后、锁外 -1。
   `net_socket_close_file()` 对 `s` 和每个 accepted child 各释放两次
   （注册的一次 + 创建者的一次，`socket_file.c:277`/`:352`/`:356`），
   `net_inet_accept_stage_drain()` 与 `net_inet_connect_stream()` 在失败路径上
   各释放两次。**没有任何运行期证据**表明每一处都对齐了，需要压力测试 + ASAN。
7. **桶名与 `lock_counters` 注册**：`net_socket_registry_init()` 给每个真实桶锁
   调 `lock_counters_register()`。三档 profile 分别是 128 / 32 / 8 个桶锁。
   若 `LOCK_COUNTERS_MAX`（`kernel/core/lock_counters.c`）小于当时的总数，
   多出来的注册会被静默丢弃——这需要集成阶段看 `/proc/a20/lock_contention`
   的实际条数确认。
8. `net_unix_socket_connect()` 里 `child` 至今**不注册**（沿用原代码），因此
   accepted 的 AF_UNIX 连接在 `/proc/net/unix` 里查不到，且走 orphan 桶。
   这是既有行为，不是本轮引入，但如果集成阶段看到 AF_UNIX accept 在 SMP 压力
   下有异常，这里是第一嫌疑点。

---

## 9. 集成修复记录（第 1 轮，由集成修复员执行）

### 9.1 §5 的交接已完成：桶锁锁序并入 `kernel/include/core/lock.h`

§5 说本 agent 无权编辑 `kernel/include/core/lock.h`，把要并入的文本整段交给该文件
owner。集成阶段核对发现**尚未并入**（`grep -n "g_net_lock\|net_bucket"
kernel/include/core/lock.h` 只命中 4 处旧 `g_net_lock` 措辞，没有任何 `net_bucket`），
因此由集成修复员代为并入：

| 位置 | 改动 |
|---|---|
| `kernel/include/core/lock.h:47` | 全局顺序链的 `g_lwip_lock -> g_net_lock` 一行替换为 §5.1 给的 `g_lwip_lock -> net_bucket[*]`。 |
| `kernel/include/core/lock.h:58`/`:60`/`:63` | 三条 lwIP 入口规则里的 `g_net_lock` 措辞改为 "a socket-table bucket lock"（§5.3 只覆盖了 `kernel/net/**` 与 `virtio_net.c`，漏了这三条）。 |
| `kernel/include/core/lock.h:65` 起 | 新增一整段 "Socket-table shard locks"，逐条照录 §5.2 的六条规则（分桶与升序双桶、禁止持多把桶锁遍历、`g_lwip_lock` 与任何桶锁互斥、orphan 桶、`net_register_socket_locked()` 不得在他桶下调用、`net_socket_ref()` 出临界区纪律）。 |

并入时**多加了一条 §5.2 里没有的规则**，因为 §5 只声明了 `g_lwip_lock ↔ 桶锁`，
没有声明桶锁与本轮另外两条路（`tasklist_lock` / `park_lock` / `runq_lock`）的关系：

> - Bucket locks are independent of the task locks above: no net bucket lock
>   nests under tasklist_lock / park_lock / runq_lock, and none of those three
>   is ever taken while a bucket lock is held.

这一条是**实测**过的，不是照抄：桶锁 helper（`net_bucket_lock()` /
`net_bucket_lock2()`）是 `kernel/net/socket_internal.h:510`/`:527` 的
`static inline`，`grep -rln "net_bucket_lock" kernel/ | grep -v ^kernel/net/` 无命中，
即 `kernel/net/**` 之外没有任何文件直接取桶锁；`grep -rn "park_lock\|tasklist_lock\|
runq_lock\|proc_lock" kernel/net/` 只命中一条注释（`kernel/net/lwip_stack.c:370`）；
包含 `socket_internal.h` / `socket_side.h` 的六个 `kernel/net/` 外文件
（`abi/linux/sys_socket_msg.c`、`abi/linux/sys_fs.c`、`abi/native/sys_native_net.c`、
`drivers/core/driver_class.c`、`fs/sysfs.c`、`fs/procfs/procfs_render.c`、
`core/progress.c`）里也没有任何 task 锁符号。

### 9.2 未并入的一项（明确留给集成/后续）

§5.4 提到的 `kernel/fs/procfs/procfs_render.c` 五处（`:394` 等）关于
`g_net_lock` 的注释**本轮仍然没有改**：它们是解释性文字，不影响编译，也不影响锁的
实际取得；本轮任务范围是"修构建失败 + 并入 lock.h"，改 `kernel/fs/**` 的注释属于
proc agent 的文件范围，需要时请在合并前统一处理。

---

## 10. 集成修复记录（第 2 轮，由集成修复员执行）：两个运行期缺陷

第 1 轮只做了"修构建 + 并入 lock.h"，本轮是两个**运行期**门禁
（`smoke-network-suite`、`smoke-smp-lock-contention`）暴露的实现疏漏。两者都不是设计
问题，都在实现层，改动不涉及 §2 的分桶方案、§2.3 的引用计数规则与 §3bis 的不变量。

### 10.1 空闲位图的极性反了：所有 `socket()` 返回 ENFILE

**症状**：`smoke-network-suite` 里 12 个测试全红，每一条都是
`socket: Too many open files in system`（errno 23 = 本内核的 `ENFILE`，正是
`net_register_socket_locked()` 找不到空槽时返回的那个码，见 `kernel/include/core/errno.h`）。
同一根因也打挂了 `smoke-smp-lock-contention` 里的 `net_stress_test`
（`user/cmds/net/net_stress_test.c:69` 的第一个 `socket()` 就失败）。

**根因**：位图的极性在树里存在两套互相矛盾的约定。

| 位置 | 约定 |
|---|---|
| `kernel/net/socket_registry.c` 的 `net_socket_claim()`：先取 `~word` 找 0 位，再 `\|= (1U << bit)` | **位 0 = 空闲，位 1 = 占用** |
| `net_unregister_socket_locked()`：`free_bits[i/32] &= ~(1U << (i % 32))` | 同上（清位 = 释放） |
| `net_socket_registry_init()` 改造后写成 `memset(..., 0xff, ...)` | **位 1 = 空闲**（与上两条相反） |
| `kernel/net/socket_internal.h` 里 `net_bucket_t.free_bits` 的注释 | 同上（"Bit set == slot free"） |

claim / unregister 是从 HEAD 的 `g_sock_free`（注释原文 "bit n == 0 -> slot n is free"）
逐字搬过来的，只有 init 和那条注释是分片时新写的、且写反了。init 把 1024 个位全置成
"占用"，于是 `net_bucket_claim()` 第一眼就看到"没有空位"，`net_register_socket_locked()`
环形扫完 32 个桶后返回 `-ENFILE`。三档 profile 全中（server / default / embedded 的
`memset` 是同一行）。

顺带发现同一段里的第二个错：init 用 `free_bits[last] >>= slack` 处理"桶窄于一个字"的
多余位，但那是按"位 1 = 空闲"写的右移；在正确的极性下要把多余的**高位**置成占用，
应该是 `|= ~0u << (32 - slack)`。default / server 档 `slack == 0` 走不到这行，
**只有 embedded 档（每桶 1 个槽）会踩到**，所以本轮之前没有任何构建或冒烟能发现它。

**修复**（`kernel/net/socket_registry.c:81` 与 `:92`）：init 改回 `memset(..., 0, ...)`，
slack 掩码改成置位；`kernel/net/socket_internal.h` 里 `net_bucket_t.free_bits` 的注释
改成"位 0 = 空闲"，并写明这是分片前 `g_sock_free` 的极性。claim / unregister 一行未动。

### 10.2 "还持有槽位吗"被当成"还活着吗"：AF_UNIX 连接上的 send() 全部 ECONNREFUSED

**症状**：10.1 修完后 `smoke-network-suite` 从 0 通过变成 **10 通过 / 1 失败**，
唯一红项是 `UNIX_TEST: FAIL`（`user/cmds/net/unix_test.c`：server bind/listen/accept，
client connect/send，server 收 `hello unix`）。

**取证**：内核无调试信息（构建 flag 里没有 `-g`），所以行断点不可用，改用函数入口断点 +
`bt`。`net_socket_is_valid_locked()` 是实体函数，断点打得住。实测序列（地址为该次运行）：

```
[INSTALL] s=...6c0010      <- server socket()
[BIND]    s=...6c0010      <- bind() 成功，/tmp/unix_test.sock 节点建出来了
[CONNECT] s=...6c8010      <- client connect()
[PUSH]    listener=...6c0010 child=...6d0010   <- accept 队列 push 成功
[INSTALL] s=...6d0010      <- server accept() 返回，装上 child
[UNIXSEND] s=...6c8010     <- client send()
（此后没有 [ENQ]，send 直接返回）
```

另一次运行里 `net_socket_free(child)` 的 backtrace 是
`net_socket_free <- net_unix_socket_sendto_impl <- sys_sendto`，
正是 send 路径上"拿到 dst 之后立刻释放并返回错误"那一支。

**根因**：分片时新加的"引用拿到 dst、取桶锁对之后复查一遍"用的是
`net_socket_is_valid_locked()`，它问的是"**这个 socket 还占着注册表槽位吗**"。
而 §3bis(b) 明确记录的既有行为是：AF_UNIX connect 建的 child **不注册**
（`reg_idx == -1`，走 orphan 桶），listener 已经 accept 它、server 已经在它上面 recv。
对这样一个 socket 问槽位问题，答案永远是"否"，于是
`kernel/net/socket_unix.c:334` 的 `if (!net_socket_is_valid_locked(dst))` 把每一次
AF_UNIX 连接上的 send 都判成对端已死，返回 `-ECONNREFUSED` 并释放 dst。
分片前没有这次复查（全程一把 `g_net_lock`），所以这是分片引入的行为回归。

**修复**：新增 liveness 谓词 `net_socket_is_live()`（`kernel/net/socket_internal.h:670`），
语义是"**我手里有引用，它没被 close**"，并把 `kernel/net/*.c` 里全部 41 处
`net_socket_is_valid_locked()` 调用换成它。理由与等价性：

- 调用点无一例外已经持有该 socket 的引用（fd 自己的 socket、`s->peer` 反向指针、
  或表扫描交回来的带引用结果）。有引用在，对象就不会在脚下被释放。
- `net_socket_close_file()` 在**同一个桶临界区**里清槽位并置 `closed = 1`
  （§3.1 第 1 条保留的不变量），所以对**已注册**的 socket，
  "还占槽位" 与 "没被 close" 始终同真同假——替换不改变任何一个已注册 socket 的判定。
- 唯一被放宽的分支就是"**从未注册过**的 socket"，而那正是 AF_UNIX accepted child，
  它本来就该判活。换成"槽位"谓词才是 bug。
- 换谓词后 6 处 `is_valid(x) && !x->closed` / `|| x->closed` 变成恒等冗余，一并删掉
  （`kernel/net/socket_inet.c` 4 处、`kernel/net/socket_queue.c` 2 处），不改变判定。

`net_socket_is_valid_locked()` 本身**保留**（`kernel/net/socket_registry.c`），它是关于
"表成员关系"的那个问题，不是 liveness；只是现在没有调用点了，注释里写明了这一点。

### 10.3 本轮实跑

```console
$ make ARCH=riscv64 BOARD=qemu-virt-riscv64 kernel-only
EXIT=0（-Werror，无告警）

$ make smoke-network-suite
... UNIX_TEST: PASS ...
NETWORK_SUITE: PASS (12 passed, 0 skipped, 0 absent, 0 declared-absent, 0 failed)
smoke-network-suite: PASS; log saved to .kernel-build/smoke/network-suite-riscv64.log
EXIT=0

$ make check
check: PASS -- all 16 host-side gates green
EXIT=0
```

`smoke-smp-lock-contention` 的另一半修复（`net_stress_test`）由 10.1 解决，该门禁在
proc 笔记 §8 里记录。

**本轮没有覆盖**：SMP 冒烟以外的其余 smoke/stress 用例本轮**未复跑**（`smoke-smp-lock-contention`
自己会跑 `net_stress_test`，本轮跑了）；§8 的引用计数待验证清单（§8.3 / §8.6）仍然只有
静态走查、无压力测试证据——本轮这两个 bug 恰好各命中它列的一个盲区（§8.5 的"取桶后复查"
和 §8.6 的"释放两次"），所以那份清单的优先级应当提高。

---

## 11. 第 3 轮（正确性续修，2026-10-06）：§8 的待验证清单第一次被跑起来

§8 记了七条"本轮无法在 agent 内验证"。这一轮把其中 §8.2 / §8.3 / §8.5 /
§8.6 / §8.7 落成了**运行期**的东西，并且**第一条跑起来就抓到一个真缺陷**。
逐条对照，不含糊：

| §8 条目 | 现在的状态 | 落点 |
|---|---|---|
| §8.2 `LOCK_COUNTERS_MAX` 预算 | **已解决** | `067d0eb96`：`lock_counters_register()` 超出 `LOCK_COUNTERS_MAX` 时改为计数并在 `/proc/a20/lock_contention` 末行打印 `lock_counters: registered=<n> capacity=<n> dropped=<n>`。此前是**静默丢弃**。 |
| §8.3 / §8.6 引用计数漏点 | **已落地，并抓到一个真 bug** | `net_socket_free()` 改 CAS 循环 + `ref_magic` 金丝雀 + `net_sock_ref:` 账本。抓到的缺陷见 §11.2。 |
| §8.5 `-ENOTCONN` 窗口 | **已落地**（可观测，未复现） | 13 处 `-ENOTCONN` 全部改走 `net_notconn(reason)`，其中 4 个 reason 属于该窗口；`/proc/net/status` 分开打印 `total=` 与 `window=`。 |
| §8.4 `NET_PROFILE_SOCKET_MAX_BYTES` 静态断言 | 早已确认（§8 自记） | 本轮新增 `ref_magic` 字段后断言仍通过，三档默认构建均编译成功。 |
| §8.1 构建与冒烟 | 见 §11.4 | |
| §8.8 AF_UNIX child 不注册 | **已确认为真，且正是缺陷成因** | 见 §11.2。 |

### 11.1 三个探针的形态与它们各自的盲区

**net 锁探针**（`kernel/net/net_lock_probe.c`）：per-CPU 持锁集合，逐次取锁检查
重复取锁 / socket 锁地址乱序 / 第三把 socket 锁 / 桶锁取在 socket 锁之下 / 第二把
桶锁。开关复用 `CONFIG_NET_LOCK_ASSERT`，三档 `0` 无 / `1` panic / `2` 只计数。

盲区（写下来免得被读成更强的结论）：它只知道"哪个 CPU 取了哪把锁"，不知道是哪个
任务，因此抓不到"取用顺序与释放顺序不一致"和"某对锁提前放掉了一把"。抓的是**集合**
违反契约的那一类——本树历来所有 ABBA 都出自这一类。

**引用计数账本**（`kernel/net/socket.c`）：`allocs` / `frees` / `live` / `faults`，
无条件计数，`/proc/net/status` 打印。`live` 是"漏"的读数，`faults` 是"重"的读数。
`live` 与 `syscall-sockets: open=` 的差是刻意的：前者算上正在创建和正在拆除、
没有槽位的那部分 socket，后者只数表里看得见的。

金丝雀的盲区也写明：`obj_cache_alloc_zero()` 会把同一个地址交给下一个 socket，
下一个 socket 会重新写上金丝雀，所以**落在已复用槽位上**的重复释放仍然看不见。
它抓的是"落在还没被复用的槽位上"这一种，也正是实际发生的那一种。

**`-ENOTCONN` 归因**：§8.5 问的是"这个窗口可达吗"，而它当时**没法问**——窗口和
"这个 socket 从来没连过"返回的是同一个 errno。现在按 reason 分开计数，`window=`
一个数字就能回答。

### 11.2 账本抓到的真缺陷：AF_UNIX close 少一次引用、多一次释放

`CONFIG_NET_REF_ASSERT=1` 的第一次运行就在第一个 AF_UNIX close 上 panic：

```
net_socket_free: canary-mismatch: object was not a live socket
s=ffffffc0be440010 refs=0 allocs=127 frees=126 faults=1
[PANIC] task: pid=19 name=unix_test
net_socket_free <- net_socket_close_file <- vfs_finalize_closed_vfile <- sys_close
```

**成因**：`net_socket_unregister()` 对没有槽位的 socket 直接 early-return，
**不释放引用**（槽位引用由调用方在锁外释放）。但两条通用拆除路径
（`net_socket_close_file()` 与 `accept()` 安装失败的回滚）**无条件**释放了
"槽位引用"。

而 AF_UNIX 的 accepted child **从不注册**——`grep -rn net_register_socket_locked
kernel/net/socket_unix.c` 零命中，这正是 §3bis(b) 记的"沿用原代码"的行为。所以它
只有**一个**引用（`net_socket_alloc()` 的创建者引用），不是两个。于是每一次
AF_UNIX 关闭都会：

1. 在 `net_socket_close_file()` 中途就把 socket 释放掉（`refs` 1→0）；
2. 继续对已释放内存解引用——`wait_queue_wake_all(&s->accept_waitq, ...)` 等；
3. 函数末尾再 `net_socket_free(s)` 一次。

**这不是本轮引入的，也不是当场就在破坏内存的。** 旧的
`__atomic_fetch_sub(&s->refs, 1) != 1` 在 `refs == 0` 时读到 -1、不等于 1、直接
返回，对象恰好只被释放了一次——**出于巧合而不是出于设计**；这个 use-after-free
之所以活下来，是因为没有任何一处读已释放内存的方式会触发缺页。变的是它现在
**看得见了**。

**修法**：`net_socket_unregister()` 改为返回"是否真的释放了槽位"，调用方只在
返回 true 时才释放槽位引用。五个调用点全部更新：

| 位置 | 情形 |
|---|---|
| `socket_file.c:309` | fd 自己的 socket `s`。**AF_UNIX accepted child 走的就是这里**，且它自己没有槽位。 |
| `socket_file.c:379` | listener 关闭时仍在 accept 队列里的 child。 |
| `socket_control.c:396` | `accept()` 安装 fd 失败的回滚。 |
| `socket_inet.c:1235` | accept stage drain 回滚（保证已注册，形状统一）。 |
| `socket_inet.c:2076` | 本地 listener 快捷路径 connect 回滚（同上）。 |

把"有没有槽位"作为返回值交出去，是让**释放了被计数的那个东西的函数**自己回答
"这个引用存不存在"，于是这个配对以后不会再漂。

### 11.3 探针不是空转：两组故意注入的对照

只跑一次干净的日志证明不了"探针在检查"。两组**临时**注入，跑完即回退，
**不在提交里**：

1. 让 arm 之后第一次取桶锁看起来像"已经持有一把 socket 锁"，在
   `CONFIG_NET_LOCK_ASSERT=1` 下得到
   `net lock contract: bucket lock under socket lock ... cpu=0 held=1 violations=1`
   紧接 `[PANIC]`——违规确实 abort。
2. 把已持集合污染成一把地址更高的 socket 锁，在 `CONFIG_NET_LOCK_ASSERT=2` 下得到
   `net_lock: armed=1 violations=11449 sites=8`，且 `SOCKET_STRESS: PASS` 走完——
   地址升序检查确实会触发，且第三档只计数不 abort。

**没做的对照**：第三把 socket 锁那一支与地址乱序那一支共用同一个
`net_lock_violation()` 上报路径，但**没有单独注入过**。两次对照两次分支，不要说成
三次对照三次分支。

### 11.4 本轮实跑

```console
$ make dev-build
EXIT=0（-Werror）

$ make dev-build OPT="-DCONFIG_NET_LOCK_ASSERT=1 -DCONFIG_NET_REF_ASSERT=1"
EXIT=0

$ <qemu-system-riscv64, riscv64 both-dev, 单 CPU>
SOCKET_STRESS: PASS
NET_STRESS_TEST: PASS (4 parallel transfers, 4 rounds x 1048576 B)
UNIX_TEST: PASS
lwip_lock: armed=1 owner=4294967295 violations=0 sites=0
net_lock: armed=1 violations=0 sites=0 held_cpu0=0 lockcounters_short=0
syscall-sockets: open=1 bound=1 queued=0 max=1024
net_sock_ref: live=1 allocs=129 frees=128 faults=0
net_notconn: total=0 window=0 reasons=13
```

`live=1` 与 `open=1` 和 `lanes: sockets=1`（那个 DHCP socket）三者一致——这正是
两个读数互相印证的地方。

**仍未跑的**：ASAN 压力测试、`smoke-network-suite` 与 `smoke-smp-lock-contention`
本轮**没有复跑**（全量回归由编排层统一做）。所以"`live` 在真正的多核压测下不增长"
这一句，本轮**没有**证据；§11.4 的数据是单 CPU、三个自带用例跑完的账本平衡，
不是压力测试下的不漏证明。这一点不要被上面的 `live=1` 读成后者。
