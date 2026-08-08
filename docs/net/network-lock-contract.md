# 网络锁契约

本文档定义 A20OS 内核网络路径的锁规则。它适用于 `kernel/net/` 中的 socket 层、`kernel/net/lwip_stack.c` 中的 lwIP 集成，以及任何会触碰网络状态的 deferred bottom-half 或 workqueue。

## 范围与目标

A20OS 以 `NO_SYS=1` 模式运行 lwIP。一个全局 spinlock `g_lwip_lock` 串行化所有 lwIP 核心状态。socket 层额外使用 `g_net_lock` 保护每个 socket 的消息队列、waiter 和 registry。

本契约目标：

- 防止 socket 系统调用、lwIP callback 和驱动路径之间发生死锁。
- 禁止在 `g_lwip_lock` 下执行阻塞操作，保持中断和调度延迟较低。
- 让 socket send/recv/connect/listen/accept 测试可以安全并发运行。
- 记录 deferred bottom-half 如何与两个锁交互。

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

### 全局顺序

网络路径的锁顺序是：

```text
g_lwip_lock -> g_net_lock
```

`g_lwip_lock` 永远是外层锁。lwIP callback 在隐式持有 `g_lwip_lock` 的上下文中运行，然后可以获取 `g_net_lock`。任何路径都不能先持有 `g_net_lock` 再获取 `g_lwip_lock`。

该顺序与 `kernel/include/core/lock.h` 一致；该文件记录了 `g_lwip_lock -> virtio-net nonblocking send/recv paths only`。

## 锁安全的 Socket 入口点

以下小节描述每类 socket 操作要求的锁纪律。实现必须匹配这些规则。

### Socket 创建与销毁

`net_inet_socket_init()` 和 `net_inet_socket_destroy()` 必须在整个执行期间持有 `g_lwip_lock`。它们创建或移除 lwIP PCB、设置 callback 并配置 TCP 选项。除非正在注册 socket，否则不需要访问 `g_net_lock`；socket 注册发生在 PCB 设置完成之后。

### Bind

`net_inet_bind_pcb()` 在不持有任何锁的情况下解析用户地址，然后只在调用 `udp_bind()`、`raw_bind()` 或 `tcp_bind()` 时获取 `g_lwip_lock`。bind 期间 socket registry 不发生变化。

### Connect

Stream connect 分为三个阶段：

1. 本地目标解析。如果目的地址是本地地址，该路径在搜索 listener 表并构造配对 socket 时持有 `g_net_lock`。此时不持有 `g_lwip_lock`。
2. 远端 TCP connect。地址解析后，路径获取 `g_lwip_lock`，带 connected callback 调用 `tcp_connect()`，然后释放 `g_lwip_lock`。
3. 阻塞等待。调用者释放所有锁，并通过 `net_block_on_socket_locked()` 在 `g_net_lock` 上阻塞。connected callback 通过 `g_net_lock` 唤醒 waiter。

UDP 和 RAW connect 遵循与 bind 相同的模式：在锁外解析，然后只在调用 `udp_connect()` 或 `raw_connect()` 时获取 `g_lwip_lock`。

### Listen 与 accept

Listen 将 TCP PCB 设置为监听状态。listen 调用必须在 `tcp_listen()` 状态转换和安装 accept callback 时持有 `g_lwip_lock`。

Accept 只使用 `g_net_lock`。它从 listener accept 队列中弹出预创建的 child socket。如果返回了 child，调用者随后调用 `net_inet_accept_child_ready()`，该函数获取 `g_lwip_lock` 并调用 `tcp_backlog_accepted()`。

### Send

send 路径对本地 socket 和远端 socket 行为不同。

对本地 UDP loopback 或已连接本地 socket，路径在将数据入队到目标 socket 时持有 `g_net_lock`。如果目标队列已满且调用是阻塞的，它会释放 `g_net_lock`、阻塞并重试。

对远端 UDP、RAW 或 TCP send，路径必须：

1. 如果需要临时 bind，在持有 `g_net_lock` 时确保 socket 已绑定。
2. 获取 `g_lwip_lock`。
3. 分配 pbuf、复制数据、调用 `udp_sendto()`/`udp_send()`/`tcp_write()` 加 `tcp_output()`，然后释放 `g_lwip_lock`。

`net_inet_send_tcp()` 在每轮之间不持有任何锁地轮询 lwIP 进展，然后只在调用 `tcp_sndbuf()`、`tcp_write()` 和 `tcp_output()` 时获取 `g_lwip_lock`。

### Recv

Recv 只使用 `g_net_lock`。它从 socket 接收队列中出队消息。如果队列为空且调用是阻塞的，它释放锁，通过 `net_block_on_socket_locked()` 阻塞，然后重试。

当 recv 消耗 TCP 数据后，调用者随后调用 `net_tcp_recved()`，该函数获取 `g_lwip_lock` 来更新 TCP window。

## lwIP Callback 规则

lwIP callback 运行时，lwIP 已经持有 `g_lwip_lock`。callback 不得：

- 阻塞或睡眠。
- 调用 `kmalloc()` 或 `kfree()`。
- 调入 VFS、scheduler，或任何可能获取其他 spinlock 的路径，除非该路径明确记录为非阻塞且锁顺序安全。
- 递归获取 `g_lwip_lock`。

`kernel/net/socket_inet.c` 使用 Deferred Bottom-Half 设计：lwIP callback 只把事件写入 per-socket 有界 `bh_ring` 并调用 `net_inet_bh_schedule()`，真正的 `net_msg_t` 分配与 payload 复制在 bottom-half（`bh_ring` 消费路径）中完成，不持有 `g_lwip_lock`。

### 允许的 callback 工作

callback 只能执行轻量、有界工作：

- 从 pbuf 复制少量数据到预分配的 per-PCB staging buffer。
- 更新少量 socket 状态标志。
- 记录需要由 bottom-half 处理的事件；不得在 callback 中直接进入 scheduler。
- 释放传入 pbuf。

所有重工作，包括内存分配、队列插入、大块数据复制和 waiter wake，都必须推迟到底半部。bottom-half 在对象锁内 collect 带 `wait_seq` 的 wait entry，释放对象锁后 flush wake queue。

## Deferred Bottom-Half 设计

P1 I/O wakeup 决策是为网络完成处理使用 deferred bottom-half / workqueue。本节定义 bottom-half 必须如何与 `g_lwip_lock` 和 `g_net_lock` 交互。

### Bottom-half 职责

网络 bottom-half 执行目前直接在 lwIP callback 中完成的工作：

- 分配 `net_msg_t` 项并复制 payload 数据。
- 将接收消息入队到 socket 接收队列。
- 更新 `closed`、`connected`、`tcp_connecting` 等 socket 标志。
- 通过 `g_net_lock` 唤醒被阻塞的 waiter。

### Top-half / bottom-half 拆分

lwIP callback 变成最小 top-half：

1. 检查 pbuf 并确定目标 socket。
2. 如果有可用的预分配 per-PCB staging buffer，将 pbuf 复制进去。
3. 在无锁 per-PCB ring 或 atomic flag 中记录事件类型和少量元数据。
4. 调度 bottom-half。
5. 释放 pbuf 并返回。

bottom-half 稍后在 workqueue 上下文中运行：

1. 获取 `g_net_lock`。
2. 处理该 socket 的所有 pending event。
3. 分配 `net_msg_t` 项并复制 payload 数据。
4. 唤醒 waiter。
5. 释放 `g_net_lock`。

### 锁交互

bottom-half 绝不能持有 `g_lwip_lock`。它只在持有 `g_net_lock` 时运行。这样保持了全局顺序：top-half 在 `g_lwip_lock` 下运行且不获取 `g_net_lock`，bottom-half 在 `g_net_lock` 下运行且不获取 `g_lwip_lock`。

如果 bottom-half 需要调用 lwIP，例如更新 TCP window 或关闭 PCB，它必须释放 `g_net_lock`，获取 `g_lwip_lock`，执行 lwIP 调用，释放 `g_lwip_lock`，再重新获取 `g_net_lock`。它不能同时持有两个锁。

### top-half 与 bottom-half 之间的顺序

per-PCB sequence counter 或 ring buffer 保证 bottom-half 按 top-half 入队顺序看到事件。top-half 可能在中断上下文中运行，因此 ring 的生产者侧必须是中断安全的。消费者侧只在 workqueue 上下文中运行。

## lwIP 锁下的 kmalloc 规则

`kernel/include/core/lock.h` 禁止在持有 device 或 lwIP 锁时执行内存分配，除非 callee 被记录为非阻塞。当前网络代码在 lwIP callback 内部分配内存，违反该规则。

修正后的规则：

- 持有 `g_lwip_lock` 时不得调用 `kmalloc()`、`kfree()`、`net_msg_alloc()` 或任何 slab allocator 函数。
- 在 socket 创建时预分配 per-PCB staging buffer，使 top-half 无需分配即可复制 pbuf 数据。
- 将所有 `net_msg_t` 分配和 payload 复制移动到底半部；bottom-half 运行时不持有 `g_lwip_lock`。
- 如果某条代码路径在概念上处于 lwIP 临界区内但必须分配，先释放 `g_lwip_lock`，分配后重新获取。只有当本地 PCB 状态不需要在释放期间保持稳定时，这样做才安全。

## 迁移检查清单

更新网络实现以符合本契约时，逐项确认（该设计已实现）：

- [x] lwIP callback 不再调用 `kmalloc()` 或 `kfree()`。
- [x] lwIP callback 不再获取 `g_net_lock`。
- [x] lwIP callback 只执行有界工作并调度 bottom-half（`net_inet_bh_schedule`）。
- [x] bottom-half 只持有 `g_net_lock` 运行，且不持有 `g_lwip_lock`。
- [x] socket send/recv/connect/listen/accept 路径遵循本文档的锁顺序。
- [x] `a20_lwip_poll_locked()` 在持有 `g_lwip_lock` 时调用仍然安全。
- [x] `g_lwip_lock` 下的驱动路径保持非阻塞。
- [x] 并发 socket stress 测试通过，且没有锁顺序告警。
