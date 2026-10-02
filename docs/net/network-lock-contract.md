# 网络锁契约

本契约定义 A20OS 内核网络路径的锁规则，适用于 `kernel/net/` 中的 socket 层、`kernel/net/lwip_stack.c` 中的 lwIP 集成，以及任何会触碰网络状态的 deferred bottom-half 或 workqueue。

最后核实：与 `feat/net-lanes` 分支的代码一致（收包载荷拆分 + poll 分段）。

## 范围与目标

A20OS 以 `NO_SYS=1` 模式运行 lwIP。一个全局 spinlock `g_lwip_lock` 串行化所有 lwIP 核心状态。socket 层额外使用 `g_net_lock` 保护每个 socket 的消息队列、waiter 和 registry。

本契约目标：

- 防止 socket 系统调用、lwIP callback 和驱动路径之间发生死锁。
- 禁止在 `g_lwip_lock` 下执行阻塞操作，保持中断和调度延迟较低。
- 让 socket send/recv/connect/listen/accept 测试可以安全并发运行。
- 记录 deferred bottom-half 如何与两个锁交互。
- 记录收包载荷在两级暂存中的内存所有权。

## 锁

### `g_lwip_lock`

- 在 `kernel/net/lwip_stack.c` 中定义为 `spinlock_t`。
- 保护全部 lwIP 核心状态：PCB 列表、pbuf、timeout 列表、netif 状态、ARP/DNS/DHCP 状态和 lwIP 统计。
- 通过 `a20_lwip_lock()` 获取，通过 `a20_lwip_unlock()` 释放。
- `a20_lwip_lock()` 禁用本地中断并获取 spinlock；`a20_lwip_unlock()` 恢复之前的中断状态。
- 每个 raw lwIP API 调用都必须在持有该锁时运行。

### `g_net_lock`

- 声明于 `kernel/net/socket_internal.h`。
- 保护 `g_sockets[]`、每个 socket 的字段、消息队列、accept 队列、临时端口分配和 socket waiter。
- 必须用 `spin_lock_irqsave(&g_net_lock)` 获取。

### 全局顺序与当前更严格规则

`kernel/include/core/lock.h` 给出的全局允许顺序上界是：

```text
g_lwip_lock -> g_net_lock
```

当前网络实现采用更严格的契约：`g_lwip_lock` 与 `g_net_lock` **不得同时持有**。箭头只表示若将来确有经审查的嵌套，反向顺序永远禁止；它不是对 callback 获取 `g_net_lock` 的许可。

lwIP callback 在隐式持有 `g_lwip_lock` 的上下文中运行，只能向 per-socket 原子 `bh_ring` 写事件并设置 pending flag。`a20_lwip_poll()` 先释放 `g_lwip_lock`，再调用只持有 `g_net_lock` 的 `net_inet_bottom_half_process_all()`。驱动数据面是另一条允许顺序：`g_lwip_lock -> virtio-net/E1000 nonblocking device lock`，驱动锁下不得回调 lwIP。

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

这样安排的原因是 memp **没有任何内部加锁**，而当前每一次 memp 调用都在 `g_lwip_lock` 下发生。消费者运行在只有 `g_net_lock` 的上下文里，在那里调 `pbuf_free()` 会让 memp 的空闲链表被两个 CPU 同时修改。槽位绕回时释放把 pbuf 的存活期限制在 ring 深度以内，并且落在唯一安全的地方。

**消费者故意不释放 spill pbuf。** 如果将来看到这里少了一次 `pbuf_free()`，那是特性不是泄漏。

### 溢出缓冲的所有权

`net_msg_t.overflow` 由该消息独占，`net_msg_free()` 负责 `kfree()`。它在 bottom-half 里分配，也就是在 `g_net_lock` 下——契约禁的是 `g_lwip_lock` 下分配，`g_net_lock` 下分配一直是被允许的（`net_msg_alloc()` 原本就在那里调用）。

## 锁安全的 Socket 入口点

以下小节按操作类型给出锁纪律。实现必须匹配这些规则。

### Socket 创建与销毁

`net_inet_socket_init()` 和 `net_inet_socket_destroy()` 在创建、配置或移除 lwIP PCB 时只持有 `g_lwip_lock`，不同时访问 socket registry。registry 与 socket 字段由调用方在独立的 `g_net_lock` 临界区处理。

`net_inet_socket_destroy()` 还在同一个临界区内排空 bottom-half ring 的 spill 引用（见上）。

### Bind

`net_inet_bind_pcb()` 在不持有任何锁的情况下解析用户地址，然后只在调用 `udp_bind()`、`raw_bind()` 或 `tcp_bind()` 时获取 `g_lwip_lock`。bind 期间 socket registry 不发生变化。

### Connect

Stream connect 分为三个阶段：

1. 本地目标解析。如果目的地址是本地地址，该路径在搜索 listener 表并构造配对 socket 时持有 `g_net_lock`。此时不持有 `g_lwip_lock`。
2. 远端 TCP connect。地址解析后，路径获取 `g_lwip_lock`，带 connected callback 调用 `tcp_connect()`，然后释放 `g_lwip_lock`。
3. 阻塞等待。调用者释放所有锁，并通过 `net_block_on_socket_locked()` 在 `g_net_lock` 上阻塞。connected callback 只向 `bh_ring` 发布事件；随后不持有 `g_lwip_lock` 的 bottom-half 获取 `g_net_lock`、更新状态并在解锁后唤醒 waiter。

UDP 和 RAW connect 遵循与 bind 相同的模式：在锁外解析，然后只在调用 `udp_connect()` 或 `raw_connect()` 时获取 `g_lwip_lock`。

### Listen 与 accept

Listen 将 TCP PCB 设置为监听状态。listen 调用必须在 `tcp_listen()` 状态转换和安装 accept callback 时持有 `g_lwip_lock`。

Accept 只使用 `g_net_lock`。它从 listener accept 队列中弹出预创建的 child socket。如果返回了 child，调用者随后调用 `net_inet_accept_child_ready()`，该函数获取 `g_lwip_lock` 并调用 `tcp_backlog_accepted()`。

### Send

send 路径对本地 socket 和远端 socket 行为不同。

对本地 UDP loopback 或已连接本地 socket，路径在将数据入队到目标 socket 时持有 `g_net_lock`。如果目标队列已满且调用是阻塞的，它会释放 `g_net_lock`、阻塞并重试。

对远端 UDP、RAW 或 TCP send，socket 地址/本地队列状态和 lwIP PCB 操作分成互不重叠的临界区。调用 `pbuf_alloc()`、`udp_sendto()`/`udp_send()`、`tcp_sndbuf()`、`tcp_write()` 或 `tcp_output()` 时持有 `g_lwip_lock`，不得同时持有 `g_net_lock`。需要更新本地 socket 状态或等待队列时先释放 lwIP 锁，再进入 `g_net_lock` 临界区。

UDP、RAW 和 TCP 三条发送路径现在形状一致：**一次迭代一次持锁**，在临界区内调用 `a20_lwip_poll_locked()`。TCP 路径此前用 `a20_lwip_poll()` 开头，额外取放一次全局锁并多跑一整轮 whole-stack pass；按 64 KiB 发送缓冲写 4 MiB 约迭代 64 次，也就是原本 128 轮而 64 轮就够。

bottom-half **不**在发送循环里逐轮运行：它们取 `g_net_lock`，而两个锁从不同时持有。循环结束后在锁外排一次即可覆盖同样的工作。错误返回路径不排是安全的，因为 `sched()` 在挑选下一个任务前会运行两个 bottom-half。

### Recv

Recv 只使用 `g_net_lock`。它从 socket 接收队列中出队消息。如果队列为空且调用是阻塞的，它释放锁，通过 `net_block_on_socket_locked()` 阻塞，然后重试。

当 recv 消费 TCP 数据后，调用者随后调用 `net_tcp_recved()`，该函数获取 `g_lwip_lock` 来更新 TCP window。

## lwIP Callback 规则

lwIP callback 运行时，lwIP 已经持有 `g_lwip_lock`。callback 不得：

- 阻塞或睡眠。
- 调用 `kmalloc()` 或 `kfree()`。
- 调入 VFS、scheduler，或任何可能获取其他 spinlock 的路径，除非该路径明确记录为非阻塞且锁顺序安全。
- 递归获取 `g_lwip_lock`。

`kernel/net/socket_inet.c` 使用 Deferred Bottom-Half 设计：lwIP callback 只把事件写入 per-socket 有界 `bh_ring` 并调用 `net_inet_bh_schedule()`，真正的 `net_msg_t` 分配与 payload 搬运在 bottom-half（`bh_ring` 消费路径）中完成，不持有 `g_lwip_lock`。

### 允许的 callback 工作

callback 只能执行轻量、有界工作：

- 决定事件落在内联缓冲还是 spill 引用上。
- 更新少量 socket 状态标志。
- 记录需要由 bottom-half 处理的事件；不得在 callback 中直接进入 scheduler。
- 释放 lwIP 传入的 pbuf 引用（spill 已另行 `pbuf_ref()`）。

所有重工作，包括内存分配、队列插入、大块数据复制和 waiter wake，都必须推迟到底半部。bottom-half 在对象锁内 collect 带 `wait_seq` 的 wait entry，释放对象锁后 flush wake queue。

## Deferred Bottom-Half 实现

当前 bottom-half 不是独立 workqueue 线程。`a20_lwip_poll()` 在完成 lwIP progress 并释放 `g_lwip_lock` 后同步调用它；per-socket 定点入口也遵循同样的单锁规则。

### Bottom-half 职责

网络 bottom-half 执行 callback 不能完成的工作：

- 分配 `net_msg_t` 项并搬运 payload（内联源或 spill pbuf 源）。
- 将接收消息入队到 socket 接收队列。
- 更新 `closed`、`connected`、`tcp_connecting` 等 socket 标志。
- 通过 `g_net_lock` 唤醒被阻塞的 waiter。

### Top-half / bottom-half 拆分

lwIP callback 是 producer：

1. 检查 pbuf 并确定目标 socket。
2. 载荷装得下就拷进内联缓冲，否则 `pbuf_ref()` 并把引用记进槽位的 `owned[]`。
3. 原子提交 ring head 并设置 pending flag。
4. 调度 bottom-half。
5. 释放 lwIP 自己的 pbuf 引用。

bottom-half 是 consumer，对每条事件：内联源走 `net_enqueue_msg_locked_meta()`，spill 源走 `net_enqueue_msg_locked_pbuf()`。两者都只取 `g_net_lock`。

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

以下属于后续工作，本契约尚未覆盖，届时需要重写对应小节：

- `g_net_lock` 仍是全局的，1024 个 socket 在 recv/send/accept/close 上互相串行。改为 per-socket 锁加引用计数保护的 registry 是当前收益最大的单项改动。
- `g_lwip_lock` 尚未分片。热路径（已建立 TCP 的收发）仍然全局串行。
- netif 各有一块 `rx_frame[1536]` / `tx_frame[1536]` 暂存，单 netif 同时只能处理一个包。
- `st->ops->poll()` 与完整协议输入仍在中断上下文中执行，`g_lwip_lock` 仍从 IRQ handler 获取。

## 迁移检查清单

更新网络实现以符合本契约时，逐项确认（当前实现已满足）：

- [x] lwIP callback 不再调用 `kmalloc()` 或 `kfree()`。
- [x] lwIP callback 不再获取 `g_net_lock`。
- [x] lwIP callback 只执行有界工作并调度 bottom-half（`net_inet_bh_schedule`）。
- [x] bottom-half 只持有 `g_net_lock` 运行，且不持有 `g_lwip_lock`。
- [x] socket send/recv/connect/listen/accept 路径遵循本文档的锁顺序。
- [x] `a20_lwip_poll_locked()` 在持有 `g_lwip_lock` 时调用仍然安全。
- [x] `g_lwip_lock` 下的驱动路径保持非阻塞。
- [x] 三条 socket 发送路径每次迭代只取一次 `g_lwip_lock`。
- [x] 定时器中断只推进 timers 段，收包排空受 `CONFIG_NET_RX_IRQ_BUDGET` 限制。
- [x] 提前停止排空时保留 RX pending 标志。
- [x] spill 引用的释放只发生在 `g_lwip_lock` 下（ring 槽位绕回或 socket 销毁）。
- [x] 并发 socket stress 测试通过，且没有锁顺序告警。
