# A20OS Native ABI：IPC 与事件子系统设计

> 本文档定义 Native ABI 的进程间通信机制，包括 Channel（消息通道）和 Event Queue（事件队列）的设计、数据结构、协议和实现架构。

---

## 1. 设计概览

Native ABI 提供两个互补的 IPC 原语：

- **Channel**：同步/异步消息传递，支持 handle 传递。用于 RPC、请求-响应、数据流。
- **Event Queue**：统一事件等待机制。替代 epoll/signalfd/timerfd 的组合。用于 I/O 多路复用、定时器等待、进程退出通知。

两者都基于 handle：创建后返回 handle，操作通过 handle 进行，权限通过 rights 控制。

---

## 2. Channel（消息通道）

### 2.1 模型

Channel 是**双向端对端**的消息管道。创建时产生两个 endpoint handle，分别给通信双方。

```text
进程 A                          进程 B
  │                               │
  channel_create()                │
  → ep0_handle, ep1_handle        │
  │                               │
  channel_send(ep0, data, handles) │
  ─────────────────────────────────→ channel_recv(ep1, ...)
  │                               │
  ←───────────────────────────────── channel_send(ep1, response)
  channel_recv(ep0, ...)          │
```

### 2.2 数据结构

```c
#define A20_CH_MAX_DATA    65536   /* 单条消息最大 64KB */
#define A20_CH_MAX_HANDLES 8       /* 单条消息最多传递 8 个 handle */

typedef struct a20_ch_message {
    uint32_t data_len;             /* 数据长度 */
    uint32_t handle_count;         /* 附带 handle 数量 */
    uint8_t  data[];               /* 变长数据 */
    /* 紧跟 handle 信息（内核内部） */
} a20_ch_message_t;

typedef struct a20_channel_ep {
    refcount_t              refcount;
    spinlock_t              lock;
    wait_queue_t            waiters;        /* 等待消息的线程 */
    struct a20_channel_ep  *peer;           /* 对端 endpoint */
    int                     peer_closed;    /* 对端是否已关闭 */

    /* 消息队列（单向：从本端发往对端） */
    a20_ch_message_t      **msg_queue;      /* 消息指针数组 */
    uint32_t                msg_cap;        /* 队列容量 */
    uint32_t                msg_head;       /* 消费位置 */
    uint32_t                msg_tail;       /* 生产位置 */
    uint32_t                msg_count;      /* 当前消息数 */
    uint32_t                total_data;     /* 当前缓冲数据总量 */
} a20_channel_ep_t;
```

### 2.3 Channel 类型签名（Typed Channel）

Channel 可以选择性地声明类型签名，内核在 send/recv 时强制执行类型约束。

```c
// 通道类型签名
typedef struct a20_channel_type {
    uint32_t version;            // 结构体版本
    uint32_t send_handle_types;  // bitmask: 可发送的 handle 类型
    uint32_t recv_handle_types;  // bitmask: 可接收的 handle 类型
    uint32_t max_data_size;      // 最大字节负载（0 = 使用 A20_CH_MAX_DATA）
    uint32_t max_handles;        // 单条消息最大 handle 数（0 = 使用 A20_CH_MAX_HANDLES）
    uint32_t flags;
    // A20_CHAN_TYPE_ORDERED (1<<0): 强制消息按类型序列（协议合规模式）
    // A20_CHAN_TYPE_STRICT  (1<<1): 拒绝未声明类型
} a20_channel_type_t;

// 类型 bit 位（与 a20_object_type_t 对齐）
#define A20_CHAN_TYPE_FILE     (1u << A20_OBJ_FILE)
#define A20_CHAN_TYPE_SOCKET   (1u << A20_OBJ_SOCKET)
#define A20_CHAN_TYPE_CHANNEL  (1u << A20_OBJ_CHANNEL_ENDPOINT)
#define A20_CHAN_TYPE_PIPE     (1u << A20_OBJ_PIPE_ENDPOINT)
#define A20_CHAN_TYPE_EVENTQ   (1u << A20_OBJ_EVENT_QUEUE)
#define A20_CHAN_TYPE_TIMER    (1u << A20_OBJ_TIMER)
#define A20_CHAN_TYPE_SHM      (1u << A20_OBJ_MEMORY)
#define A20_CHAN_TYPE_TASK     (1u << A20_OBJ_TASK)
#define A20_CHAN_TYPE_NS       (1u << A20_OBJ_NAMESPACE)
#define A20_CHAN_TYPE_ANY      0xFFFFFFFF  // 不限制类型
```

**类型强制执行**：
- `send_handle_types`：`channel_send` 时，每个被传输的 handle 的对象类型必须在此 bitmask 中
- `recv_handle_types`：`channel_recv` 时，从消息中取出的 handle 的对象类型必须在此 bitmask 中
- 类型检查失败返回 `A20_ERR_TYPE_MISMATCH`

> **内核执行**：`a20_channel_send()` 调用 `ch_check_send_types()` 检查 `send_handle_types` bitmask；`a20_channel_recv()` 在取出消息后调用 `ch_check_recv_types()` 检查 `recv_handle_types` bitmask。两者均在 `kernel/ipc/a20_channel.c` 中实现。

**设计意图**：类型化通道使得内核能够强制执行 IPC 通信的结构约束，而不是仅依赖用户态协议（如 FIDL）。这提供了：
- 被攻破的进程无法向 channel 发送错误类型的 handle
- 系统管理员可以静态分析哪些进程对之间可以传输哪些类型的资源
- 形式化证明可以建立在通道类型之上（定理 2.1-2.3，见 09-innovation-deep-dive.md §2）

### 2.4 Channel 创建

```c
typedef struct a20_channel_create_args {
    uint32_t size;
    uint32_t version;
    uint32_t msg_capacity;        /* 队列容量提示 */
    uint32_t flags;
    a20_channel_type_t *type;     /* 可选：通道类型签名（NULL = 无类型约束） */
    a20_handle_t out_endpoints[2]; /* 输出：两个 endpoint handle */
} a20_channel_create_args_t;

int64_t channel_create(a20_channel_create_args_t *args);
```

创建过程：
1. 分配两个 `a20_channel_ep_t`，互相指向对方（peer）
2. 如果 `type != NULL`，将类型签名复制到两个 endpoint（类型签名共享同一份拷贝）
3. 在调用者的 handle table 中分配两个 handle
4. 每个 handle 获得 READ | WRITE | DUP | TRANSFER 权限
5. 返回两个 endpoint handle

**向后兼容**：`type == NULL` 时行为与无类型约束的 channel 完全一致。

### 2.4 Send 协议（两阶段锁）

```c
int64_t channel_send(a20_msg_send_args_t *args);
```

**两阶段锁分离设计**：发送方和接收方的 handle table 不同时加锁。

```text
阶段 1：预验证（锁发送方 HT）
  1.1 lookup channel handle → 验证 WRITE 权限
  1.2 对每个要传递的 handle：lookup → 验证 TRANSFER 权限
  1.3 对每个 handle：refcount_inc（原子操作）
  1.4 构造 a20_ch_message（拷贝数据，记录 handle 对象信息）
  1.5 解锁发送方 HT

阶段 2：投递（锁接收方 peer endpoint）
  2.1 spin_lock(&peer->lock)
  2.2 检查 peer_closed、队列满等条件
  2.3 将消息追加到 peer->msg_queue
  2.4 wake_one(&peer->waiters)（唤醒等待的接收线程）
  2.5 解锁 peer
```

**错误处理**：
- 阶段 1 失败：所有 refcount_inc 回滚（refcount_dec），返回错误
- 阶段 2 队列满：返回 `WOULD_BLOCK`（非阻塞）或阻塞等待空间（阻塞模式）
- 对端已关闭：返回 `CANCELED`，释放消息和 handle 引用

### 2.5 Recv 协议

```c
int64_t channel_recv(a20_msg_recv_args_t *args);
```

```text
1. lookup channel handle → 验证 READ 权限
2. spin_lock(&ep->lock)
3. 检查 msg_count > 0：
   - 是：取出队头消息
   - 否：阻塞等待（释放锁 → 等待 → 重新获取锁）
4. 拷贝数据到用户缓冲区
5. 对每个附带的 handle：
   - 在当前进程 HT 中分配新槽位
   - 写入 (object, type, requested_rights)
   - refcount 已经在 send 时 inc 过
6. 解锁
7. 唤醒可能在等待空间的发送方
```

### 2.6 Partial Delivery 状态机

如果接收方的 handle table 满了，消息中的 handle 只能部分投递。这触发 partial delivery：

```text
状态：
  IDLE → 正常
  PARTIAL → 消息已取出，部分 handle 已投递，等待剩余空间
  ROLLED_BACK → 投递失败，消息回滚

转换：
  IDLE → recv 开始
  如果所有 handle 投递成功 → IDLE（消息完成）
  如果部分 handle 投递失败 → PARTIAL
  PARTIAL → 重试投递 / 超时 → ROLLED_BACK（释放所有 handle 引用）
```

**设计决策**：不使用部分投递。如果接收方 HT 空间不足，整个 recv 返回错误（`NO_SPACE`），消息留在队列中。这简化了实现且避免了部分状态。

### 2.7 Handle Transfer 语义

当 handle 通过 channel 传递时：

$$\rho_{recv} = \rho_{send} \cap \rho_{transfer}$$

- $\rho_{send}$：发送方对原始 handle 的权限
- $\rho_{transfer}$：发送方在 send 调用中指定的权限（未指定则为 $\rho_{send}$）
- $\rho_{recv}$：接收方获得的权限

**共享语义**（不是移动语义）：发送方在 send 后仍持有原 handle。对象的引用计数增加。这避免了"send 后 handle 消失"的惊讶行为。

### 2.8 Channel 关闭

当一端 endpoint 被关闭（handle_close 或进程退出）：

1. `refcount_dec`：如果不是最后引用，只是减少计数
2. 如果是最后引用：
   - 设置 `peer->peer_closed = true`
   - 唤醒对端所有等待线程（它们收到 `CANCELED`）
   - 释放本端未读消息和 handle 引用
   - 释放 endpoint 结构

---

## 3. Event Queue（事件队列）

### 3.1 模型

Event Queue 是 Native ABI 的统一等待机制。所有可观察对象的事件都通过 event queue 汇聚。

```text
┌──────────────────────────────────────────────────┐
│                 Event Queue                       │
│                                                   │
│  watch list:                                      │
│    [file_h, READABLE | WRITABLE]                  │
│    [timer_h, EXPIRED]                             │
│    [task_h, EXITED]                               │
│    [channel_h, MESSAGE_READY]                     │
│                                                   │
│  pending ring:                                    │
│    [event(file, READABLE), event(timer, EXPIRED)] │
│                                                   │
│  waiters: [thread_1 (blocked in event_wait)]      │
└──────────────────────────────────────────────────┘
```

### 3.2 数据结构

```c
typedef struct a20_watch_entry {
    a20_handle_t    target_handle;  /* 被观察对象的 handle 编号 */
    void           *target_object;  /* 被观察对象的内核指针 */
    uint16_t        target_type;    /* 被观察对象类型 */
    uint64_t        event_mask;     /* 关注的事件类型位图 */
    uint64_t        user_data;      /* 用户数据，原样返回 */
    struct a20_watch_entry *next;
} a20_watch_entry_t;

typedef struct a20_pending_event {
    a20_handle_t    source;         /* 产生事件的 handle */
    uint32_t        type;           /* 事件类型 */
    uint64_t        events;         /* 触发的事件位图 */
    uint64_t        user_data;      /* 从 watch entry 复制 */
    uint64_t        data0, data1, data2;
} a20_pending_event_t;

typedef struct a20_eventq {
    refcount_t       refcount;
    spinlock_t       lock;
    wait_queue_t     waiters;

    a20_watch_entry_t *watches;     /* 注册的观察列表 */
    uint32_t           watch_count;

    a20_pending_event_t *ring;      /* 环形缓冲区 */
    uint32_t            ring_cap;
    uint32_t            ring_head;
    uint32_t            ring_tail;
    uint32_t            ring_count;
} a20_eventq_t;
```

### 3.3 可观察事件类型

| 对象类型 | 事件 | 说明 |
|---------|------|------|
| file | READABLE | 数据可读 |
| file | WRITABLE | 缓冲区可写 |
| file | ERROR | I/O 错误 |
| file | CLOSED | 文件被关闭 |
| socket | READABLE, WRITABLE, ERROR, CLOSED | 同 file |
| socket | CONNECTION | 新连接到达 |
| socket | ACCEPT_READY | 可接受连接 |
| timer | EXPIRED | 定时器到期 |
| task | EXITED | 进程退出 |
| thread | EXITED | 线程退出 |
| channel | MESSAGE_READY | 有消息可接收 |
| pipe | READABLE, WRITABLE | 同 file |

### 3.4 操作

#### event_queue_create

```c
int64_t event_queue_create(a20_event_queue_create_args_t *args);
```

创建 eventq 结构，分配环形缓冲区（容量由 `capacity_hint` 决定，默认 256），返回 eventq handle。

#### event_watch

```c
int64_t event_watch(a20_event_watch_args_t *args);
```

1. 验证 queue handle（类型 eventq，READ 权限）
2. 验证 target handle（任意类型）
3. 创建 watch_entry，追加到 queue 的 watch list
4. 注册到全局反向索引（object → watch_entry 列表）

重复 watch 同一 target：更新 event_mask 和 user_data，不创建新 entry。

#### event_wait

```c
int64_t event_wait(a20_event_wait_args_t *args);
```

1. 验证 queue handle（READ 权限）
2. spin_lock(&eq->lock)
3. 检查 ring_count > 0：
   - 是：从 ring buffer 取出最多 max_events 个事件，拷贝到用户缓冲区
   - 否：阻塞等待（timeout_ns = 0 时直接返回 WOULD_BLOCK）
4. 解锁
5. 返回事件数量

#### event_cancel

```c
int64_t event_cancel(a20_handle_t queue, a20_handle_t target);
```

从 queue 的 watch list 中移除 target 对应的 watch_entry，并从全局反向索引中移除。

### 3.5 事件分发机制

内核中任何产生事件的地方调用 `a20_event_notify`：

```c
void a20_event_notify(void *target_object, uint16_t target_type,
                      uint32_t event_type, uint64_t data0, uint64_t data1) {
    // 通过全局反向索引找到所有 watch 了 target_object 的 event queue
    for each eventq that watches target_object {
        spin_lock(&eq->lock);
        // 检查 event_mask 是否匹配
        // 追加到 ring buffer
        // wake_one(&eq->waiters)
        spin_unlock(&eq->lock);
    }
}
```

**优化**：全局 hash table `object_watches: void* → [a20_watch_entry*]`，使得事件产生时 O(1) 找到所有相关的 event queue。

### 3.6 FIFO 顺序保证

同一对象的事件按产生顺序入队。Ring buffer 保证 FIFO：

$$e_1 \text{ produced before } e_2 \implies e_1 \text{ dequeued before } e_2$$

跨对象的顺序不保证。

### 3.7 事件不丢失保证

如果 ring buffer 满了，新事件的处理策略：

1. **唤醒策略（推荐）**：立即唤醒消费者线程。如果消费者来不及处理，事件在对象内部排队（不是在 eventq 中）。下次 event_wait 时重新检查对象状态。
2. **丢弃策略（不推荐）**：丢弃新事件。仅用于 debug 场景。

推荐唤醒策略：不丢失事件，但可能增加延迟。

---

## 4. Event Queue Cleanup 协议

### 4.1 触发场景

| 场景 | 调用路径 | 清理资源 |
|------|---------|---------|
| handle_close(eq) | close → refcount_dec → eventq_destroy | watch list + ring + 反向索引 |
| task_exit（持有 eq） | 遍历 HT → close 所有 handle | 同上 |
| 被监控对象销毁 | object_destroy → eventq_on_object_destroy | 从 watch list 移除 entry |

### 4.2 Event Queue 销毁

```c
void a20_eventq_destroy(a20_eventq_t *eq) {
    // 步骤 1：清理 watch list + 反向索引
    a20_watch_entry_t *w = eq->watches;
    while (w) {
        object_watches_remove(w->target_object, w);
        a20_watch_entry_t *next = w->next;
        kfree(w);
        w = next;
    }

    // 步骤 2：唤醒等待线程（返回 CANCELED）
    wait_queue_wake_all(&eq->waiters);

    // 步骤 3：释放 ring buffer
    kfree(eq->ring);

    // 步骤 4：释放 eventq
    kfree(eq);
}
```

**延迟清理**：`eventq_destroy` 在 HT lock 释放后执行。安全因为 refcount = 0 后无其他线程持有 eq 的 handle。

### 4.3 被监控对象销毁

```c
void a20_eventq_on_object_destroy(void *object) {
    a20_watch_entry_list_t *list = object_watches_lookup(object);
    for each watch_entry w in list {
        spin_lock(&w->owner_queue->lock);
        linked_list_remove(&w->owner_queue->watches, w);
        w->owner_queue->watch_count--;
        spin_unlock(&w->owner_queue->lock);
        kfree(w);
    }
    object_watches_remove_all(object);
}
```

---

## 5. 与 POSIX 的对比

| POSIX 机制 | Native ABI 替代 | 优势 |
|-----------|----------------|------|
| `pipe()` | `channel_create()` | channel 支持 handle 传递，pipe 不支持 |
| `SCM_RIGHTS` (sendmsg) | `channel_send(handles)` | 显式权限降级，不需要辅助数据 |
| `epoll_create/ctl/wait` | `event_queue_create/watch/wait` | 统一 handle 等待，不限于 fd |
| `signalfd` | `event_watch(task, EXITED)` | 信号模型被事件模型替代 |
| `timerfd` | `event_watch(timer, EXPIRED)` | timer 是 handle，直接可 watch |
| `eventfd` | 不需要 | channel 可以发送零字节消息作为信号 |
| SysV msgget/msgsnd/msgrcv | channel | 无全局 key，权限通过 handle 控制 |
| POSIX mq_open/mq_send/mq_recv | channel | 无需文件系统路径，handle 即标识 |
