# A20OS Native ABI：架构设计深度补充

> 本文档是 `03-implementation-plan.md` 的深度补充。基于对内核现有数据结构的分析（`task_t`, `vnode_t`, `vfile_t`, `mm_struct_t`, `vm_area_t`, `spinlock_t`, `wait_queue_t`, `refcount_t`），定义 Native ABI 对象到内核结构的具体映射、各子系统的实现架构、以及关键路径的锁序协议。

---

## 1. Native 对象到内核结构体的映射

### 1.1 映射总表

| Native 类型 | 内核结构体 | 映射方式 | 引用计数机制 |
|------------|-----------|---------|-------------|
| A20_OBJ_FILE | `vfile_t *` | handle 直接持有 vfile 指针 | `vfile_t.ref_count` |
| A20_OBJ_DIRECTORY | `vfile_t *`（O_DIRECTORY） | 同 file，打开时带 O_DIRECTORY | `vfile_t.ref_count` |
| A20_OBJ_SOCKET | 新增 `struct a20_socket` | 独立结构体 | `refcount_t` |
| A20_OBJ_PIPE_ENDPOINT | `vfile_t *`（pipe 端） | 复用 vfile，priv 指向 pipe 缓冲 | `vfile_t.ref_count` |
| A20_OBJ_CHANNEL_ENDPOINT | 新增 `struct a20_channel_ep` | 独立结构体 | `refcount_t` |
| A20_OBJ_EVENT_QUEUE | 新增 `struct a20_eventq` | 独立结构体 | `refcount_t` |
| A20_OBJ_TIMER | 新增 `struct a20_timer` | 独立结构体 | `refcount_t` |
| A20_OBJ_TASK | `task_t *` | handle 持有 task 指针 | `task_t` 本身由 proc 管理生命周期 |
| A20_OBJ_THREAD | `task_t *`（线程也是 task_t） | 同 task | 同上 |
| A20_OBJ_MEMORY (shm) | 新增 `struct a20_shm` | 独立结构体 | `refcount_t` |
| A20_OBJ_DEVICE | `vfile_t *`（设备文件） | 复用 vfile | `vfile_t.ref_count` |
| A20_OBJ_NAMESPACE | 新增 `struct a20_namespace` | 独立结构体 | `refcount_t` |
| A20_OBJ_DEBUG | 新增 `struct a20_debug` | 独立结构体 | `refcount_t` |

### 1.2 设计原则

**原则 1：复用优先**。文件、目录、设备、pipe 端点都复用 `vfile_t`，避免为每种文件相关对象创建独立结构体。这与 Linux 的 "一切皆 file" 理念一致。

**原则 2：独立对象独立分配**。channel、event queue、timer 等 VFS 无法表达的对象使用独立结构体，通过 `refcount_t` 管理生命周期。

**原则 3：handle entry 的 object 字段是 `void *`**。统一持有不同类型对象的指针，通过 `type` 字段区分。

---

## 2. Handle Table 数据结构设计

### 2.1 数据结构

```c
// kernel/include/core/handle.h

#define A20_HT_INITIAL_CAP    256
#define A20_HT_MAX_CAP        65536
#define A20_HT_GROWTH_FACTOR  2

typedef struct a20_handle_entry {
    void             *object;    // 指向内核对象（vfile_t*, a20_channel_ep*, task_t*, ...）
    uint16_t          type;      // a20_object_type_t
    uint16_t          _pad;
    a20_rights_t      rights;    // 该 handle 的权限位域
} a20_handle_entry_t;

typedef struct a20_handle_table {
    a20_handle_entry_t *entries;     // 动态数组，索引 = handle 编号
    uint32_t            capacity;    // 当前数组容量
    uint32_t            count;       // 已使用条目数
    uint32_t            free_hint;   // 下一次搜索的起始位置
    spinlock_t          lock;        // 保护全部字段
    // 扩展：bitmap 加速 free slot 查找
    uint64_t           *free_bitmap; // 每个 bit 表示一个 slot 是否空闲
    uint32_t            bitmap_size; // bitmap 的 uint64 元素数
} a20_handle_table_t;
```

### 2.2 关键设计决策

**Q1：为什么用动态数组而不是红黑树？**

A20OS 的 handle 编号是连续的 `uint32_t`。与 Zircon 的 `fbl::WAVLTree` 不同，动态数组提供 O(1) 的 lookup 和 close，而树的 lookup 是 O(log n)。在 65536 个 handle 的上限下，数组占用约 65536 × 24 bytes ≈ 1.5 MB——对教学内核的 512MB 内存完全可以接受。

**Q2：为什么加 free bitmap？**

Free slot 查找在纯数组上是 O(n) 扫描。Bitmap 将其优化为 O(n/64) 的 word 级扫描，配合 `free_hint` 记录上次释放的位置，实际接近 O(1)。

**Q3：增长策略？**

当 `count == capacity` 时，按 `A20_HT_GROWTH_FACTOR = 2` 倍扩容。扩容需要分配新数组、复制旧条目、释放旧数组。由于持有锁，扩容期间该进程的全部 handle 操作阻塞。在 count 达到 `A20_HT_MAX_CAP` 时拒绝扩容（返回 NO_SPACE）。

### 2.3 Handle 操作实现

```c
// handle 分配：O(n/64) worst case, O(1) amortized
static int ht_alloc_slot(a20_handle_table_t *ht) {
    // 从 free_hint 开始扫描 free_bitmap
    for (uint32_t i = ht->free_hint / 64; i < ht->bitmap_size; i++) {
        uint64_t word = ht->free_bitmap[i];
        if (word != UINT64_MAX) {
            int bit = __builtin_ctzll(~word);  // 找第一个 0 bit
            uint32_t slot = i * 64 + bit;
            if (slot < ht->capacity) {
                ht->free_bitmap[i] |= (1ULL << bit);
                ht->free_hint = slot + 1;
                return slot;
            }
        }
    }
    return -1;  // 需要扩容
}

// handle lookup：O(1)
int64_t a20_handle_lookup(a20_handle_table_t *ht, a20_handle_t h,
                          uint16_t expected_type, a20_rights_t required_rights,
                          a20_handle_entry_t *out) {
    if (h >= ht->capacity) return -A20_ERR_BAD_HANDLE;
    a20_handle_entry_t *e = &ht->entries[h];
    if (e->object == NULL) return -A20_ERR_BAD_HANDLE;
    if (expected_type != A20_OBJ_INVALID && e->type != expected_type)
        return -A20_ERR_BAD_HANDLE;  // 类型不匹配
    if ((e->rights & required_rights) != required_rights)
        return -A20_ERR_ACCESS;       // 权限不足
    *out = *e;
    return A20_OK;
}
```

### 2.4 并发方案

**方案 A（当前选择）：全局 spinlock**

每个 handle table 一个 `spinlock_t lock`。所有操作（lookup, alloc, close, dup, transfer_in）在访问前获取锁。

- **读操作（lookup）也加锁**：简化正确性论证。未来可优化为读写锁。
- **性能影响**：lookup 是热路径（每个 syscall 至少一次）。spinlock 在无竞争时开销约 10-20ns（test_and_set + mb）。
- **教学内核可接受**：seL4 在早期也使用全局 CSpace lock。

**方案 B（未来优化）：per-bucket lock**

将 handle 空间分为 N 个 bucket（如 N=16），每个 bucket 有独立锁。操作时只锁目标 bucket。降低锁竞争，但增加实现复杂度。

### 2.5 与 task_t 的集成

```c
// 在 task_t 中添加：
typedef struct task_t {
    // ... 现有字段 ...

    // Native ABI 支持
    int abi_mode;                          // TASK_ABI_LINUX or TASK_ABI_NATIVE
    struct a20_handle_table *handle_table; // Native 模式下的 handle table
} task_t;
```

- Linux ABI 的 task：`abi_mode = TASK_ABI_LINUX`，`handle_table = NULL`
- Native ABI 的 task：`abi_mode = TASK_ABI_NATIVE`，`handle_table` 在 proc_alloc 时创建

---

## 3. Event Queue 实现架构

### 3.1 数据结构

```c
// kernel/include/core/event.h

typedef struct a20_watch_entry {
    a20_handle_t    target_handle;  // 被观察对象的 handle 编号（在 owner 的 HT 中）
    void           *target_object;  // 被观察对象的内核指针
    uint16_t        target_type;    // 被观察对象类型
    uint64_t        event_mask;     // 关注的事件类型
    uint64_t        user_data;      // 用户数据，原样返回
    struct a20_watch_entry *next;
} a20_watch_entry_t;

typedef struct a20_pending_event {
    a20_handle_t    source;         // 产生事件的对象的 handle 编号
    uint32_t        type;           // 事件类型
    uint64_t        events;         // 事件位域
    uint64_t        user_data;      // 从 watch entry 复制
    uint64_t        data0, data1, data2; // 事件附带数据
} a20_pending_event_t;

typedef struct a20_eventq {
    refcount_t       refcount;
    spinlock_t       lock;
    wait_queue_t     waiters;        // 阻塞等待的线程

    // Watch list
    a20_watch_entry_t *watches;      // 注册的观察列表
    uint32_t           watch_count;

    // Pending event ring buffer
    a20_pending_event_t *ring;       // 环形缓冲区
    uint32_t            ring_cap;    // 环形缓冲区容量
    uint32_t            ring_head;   // 读取位置
    uint32_t            ring_tail;   // 写入位置
    uint32_t            ring_count;  // 当前事件数
} a20_eventq_t;
```

### 3.2 关键设计决策

**Q1：为什么用环形缓冲区？**

环形缓冲区提供 O(1) 的 enqueue 和 dequeue，且有界内存使用。容量在创建时由 `capacity_hint` 决定，默认 256 个事件。环形缓冲区满了后新事件被丢弃（或唤醒消费者后再排队——选择 **唤醒策略**）。

**Q2：wake-up 机制？**

当事件追加到 pending ring 后，调用 `wait_queue_wake_one(&eq->waiters)`。这是 O(1) 定向唤醒（唤醒等待队列中的第一个线程）。

若多个线程等待同一 event queue（合法但罕见），一次 wake_one 只唤醒一个。其余线程在后续事件到达时被唤醒。

**Q3：与调度器的交互？**

`wait_queue_wake_one` 调用 `proc_make_ready`，将等待线程加入就绪队列。无优先级反转风险——等待线程恢复其原始优先级。

### 3.3 事件分发机制

```c
// 内核中任何产生事件的地方调用：
void a20_event_notify(void *target_object, uint16_t target_type,
                      uint32_t event_type, uint64_t data0, uint64_t data1) {
    // 遍历所有 event queue，找到 watch 了 target_object 的
    // （优化：全局 hash table：object → [watch_entry]）
    for each eventq that watches target_object {
        spin_lock(&eq->lock);
        // 检查 event_mask
        // 追加到 ring buffer
        // wake_one
        spin_unlock(&eq->lock);
    }
}
```

**优化：反向索引**

维护全局 hash table `object_watches: void* → [a20_watch_entry*]`，使得事件产生时 O(1) 找到所有 watch 了该对象的 event queue。

### 3.4 Event Queue 关闭时的 Cleanup 协议

#### 问题

当 event queue handle 被关闭（`handle_close`）或持有 event queue 的进程退出（`task_exit`），需要：(1) 释放 event queue 的全部内部资源；(2) 从全局反向索引中移除所有相关的 watch entry；(3) 释放 pending ring buffer 中的事件。类似地，当被 watch 的对象被销毁时，需要从 event queue 的 watch list 中移除对应的 entry。

#### Cleanup 触发点

| 触发场景 | 调用路径 | 需要清理的资源 |
|---------|---------|--------------|
| handle_close(eq_handle) | `a20_handle_close` → `refcount_dec_and_test` → `a20_eventq_destroy` | watch list + ring buffer + 反向索引 |
| task_exit（持有 eq handle） | `task_exit` → 对每个 handle 调用 close | 同上 |
| 被监控对象销毁 | `refcount(o) → 0` → `object_destroy` | 从 eventq watch list 移除对应 entry + 反向索引 |

#### 实现

```c
void a20_eventq_destroy(a20_eventq_t *eq) {
    // 步骤 1：清理 watch list 并更新反向索引
    a20_watch_entry_t *w = eq->watches;
    while (w) {
        // 从全局反向索引中移除
        object_watches_remove(w->target_object, w);
        a20_watch_entry_t *next = w->next;
        kfree(w);
        w = next;
    }
    eq->watches = NULL;
    eq->watch_count = 0;

    // 步骤 2：唤醒等待中的线程（使其返回错误）
    // 等待线程被唤醒后，发现 eq 已被销毁（通过检查 eq 的状态标记），
    // 返回 err(CANCELLED)
    wait_queue_wake_all(&eq->waiters);

    // 步骤 3：释放 ring buffer
    if (eq->ring) {
        kfree(eq->ring);
        eq->ring = NULL;
    }

    // 步骤 4：释放 eventq 自身
    kfree(eq);
}
```

**被监控对象销毁时的清理**：

```c
// 在 object_destroy 中调用：
void a20_eventq_on_object_destroy(void *object) {
    // 查找全局反向索引中 watch 了 object 的所有 entry
    a20_watch_entry_list_t *list = object_watches_lookup(object);
    if (!list) return;

    for each watch_entry w in list {
        a20_eventq_t *eq = w->owner_queue;  // watch entry 记录所属的 eventq

        spin_lock(&eq->lock);
        // 从 eq 的 watch list 中移除 w
        linked_list_remove(&eq->watches, w);
        eq->watch_count--;
        spin_unlock(&eq->lock);

        // 释放 watch entry
        kfree(w);
    }

    // 清除反向索引中 object 的条目
    object_watches_remove_all(object);
}
```

#### 关键设计决策

**Q：为什么不在 handle_close 的 spinlock 临界区内做 cleanup？**

`a20_eventq_destroy` 需要遍历 watch list 并更新全局反向索引。全局反向索引有独立的锁（`object_watches_lock`）。在持有 HT lock (L1) 的同时获取 `object_watches_lock`（L2 级别）是允许的。但 cleanup 还需要 `kfree`，这可能涉及内存管理锁（L4）。

**策略**：在 `refcount_dec_and_test` 返回 true（最后一个引用）时，延迟 cleanup 到锁释放之后。使用引用计数的特性：一旦 refcount 降至 0，无其他线程能访问该 eventq（因为没有 handle 指向它），因此不需要持锁保护 eventq 的内部状态。

```c
// 在 handle_close 中：
if (last_ref) {
    // 此时已无其他线程持有 eq 的 handle
    // 可以安全地在不持 eq->lock 的情况下清理
    spin_unlock(&ht->lock);   // 先释放 HT lock
    a20_eventq_destroy(eq);   // 延迟清理，无需 eq lock
    // 注意：task_exit 的批量 close 需要在最后统一做延迟清理
}
```

**安全性论证**：refcount 降至 0 意味着全局无 handle 指向 eq。全局反向索引中的 entry 仍持有 eq 指针，但这些 entry 会在 `a20_eventq_destroy` 中被清理。不存在 use-after-free 的风险——`a20_eventq_destroy` 是唯一释放 eq 内存的函数，且只在 refcount = 0 时调用。$\square$

---

## 4. Channel 实现架构

### 4.1 数据结构

```c
// kernel/include/core/channel.h

#define A20_CH_MAX_DATA    65536   // 64KB
#define A20_CH_MAX_HANDLES 8

typedef struct a20_ch_message {
    uint32_t data_len;
    uint32_t handle_count;
    uint8_t  data[];               // 变长数据
    // 紧跟 handle 信息：
    // struct { void *object; uint16_t type; a20_rights_t rights; } handles[];
} a20_ch_message_t;

typedef struct a20_channel_ep {
    refcount_t              refcount;
    spinlock_t              lock;
    wait_queue_t            waiters;        // 等待消息的线程
    struct a20_channel_ep  *peer;           // 对端 endpoint
    int                     peer_closed;    // 对端是否已关闭

    // Message queue（单向：从本端发往对端）
    a20_ch_message_t      **msg_queue;      // 消息指针数组
    uint32_t                msg_cap;        // 队列容量
    uint32_t                msg_head;       // 消费位置
    uint32_t                msg_tail;       // 生产位置
    uint32_t                msg_count;      // 当前消息数

    uint32_t                total_data;     // 当前缓冲数据总量
} a20_channel_ep_t;
```

### 4.2 Channel 创建

```c
int64_t native_sys_channel_create(a20_channel_create_args_t *args) {
    // 分配两个 endpoint
    a20_channel_ep_t *ep0 = kzalloc(sizeof(*ep0));
    a20_channel_ep_t *ep1 = kzalloc(sizeof(*ep1));

    // 初始化
    refcount_set(&ep0->refcount, 1);
    refcount_set(&ep1->refcount, 1);
    ep0->peer = ep1;
    ep1->peer = ep0;
    // ... 初始化 msg_queue, waiters 等 ...

    // 在调用者的 handle table 中分配两个 handle
    task_t *cur = proc_current();
    a20_handle_table_t *ht = cur->handle_table;

    spin_lock(&ht->lock);
    int slot0 = ht_alloc_slot(ht);
    int slot1 = ht_alloc_slot(ht);
    if (slot0 < 0 || slot1 < 0) {
        // 回滚
        if (slot0 >= 0) ht_free_slot(ht, slot0);
        spin_unlock(&ht->lock);
        kfree(ep0); kfree(ep1);
        return -A20_ERR_NO_SPACE;
    }
    ht->entries[slot0] = (a20_handle_entry_t){
        .object = ep0, .type = A20_OBJ_CHANNEL_ENDPOINT,
        .rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_DUP | A20_RIGHT_TRANSFER
    };
    ht->entries[slot1] = (a20_handle_entry_t){
        .object = ep1, .type = A20_OBJ_CHANNEL_ENDPOINT,
        .rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_DUP | A20_RIGHT_TRANSFER
    };
    spin_unlock(&ht->lock);

    // 写回用户态
    args->out_endpoints[0] = slot0;
    args->out_endpoints[1] = slot1;
    return A20_OK;
}
```

### 4.3 Send 实现

```c
int64_t native_sys_channel_send(a20_msg_send_args_t *args) {
    task_t *cur = proc_current();
    a20_handle_table_t *ht = cur->handle_table;

    // 预验证阶段
    spin_lock(&ht->lock);
    a20_handle_entry_t ch_entry;
    int rc = a20_handle_lookup(ht, args->channel, A20_OBJ_CHANNEL_ENDPOINT,
                                A20_RIGHT_WRITE, &ch_entry);
    if (rc < 0) { spin_unlock(&ht->lock); return rc; }

    a20_channel_ep_t *ep = ch_entry.object;

    // 验证要传递的 handles
    a20_handle_entry_t pass_entries[A20_CH_MAX_HANDLES];
    for (int i = 0; i < args->handle_count; i++) {
        rc = a20_handle_lookup(ht, args->handles[i], A20_OBJ_INVALID,
                                A20_RIGHT_TRANSFER, &pass_entries[i]);
        if (rc < 0) { spin_unlock(&ht->lock); return rc; }
    }

    // 构造消息
    a20_ch_message_t *msg = construct_message(args->bytes, args->byte_count,
                                               pass_entries, args->handle_count);

    // 锁 channel endpoint，检查容量
    spin_lock(&ep->peer->lock);  // 发送到对端的队列
    if (ep->peer->total_data + args->byte_count > A20_CH_MAX_DATA) {
        spin_unlock(&ep->peer->lock);
        spin_unlock(&ht->lock);
        kfree(msg);
        return -A20_ERR_WOULD_BLOCK;
    }

    // 增加 passed handles 的 refcount
    for (int i = 0; i < args->handle_count; i++) {
        // 通用 refcount 增加
        object_refcount_inc(pass_entries[i].object, pass_entries[i].type);
    }

    // 追加消息到对端队列
    enqueue_message(ep->peer, msg);

    // 唤醒对端等待者
    wait_queue_wake_one(&ep->peer->waiters);

    // 释放锁（先释放 peer lock，再释放 HT lock）
    spin_unlock(&ep->peer->lock);
    spin_unlock(&ht->lock);

    return A20_OK;
}
```

### 4.4 锁序分析

channel_send 的锁获取顺序：
1. `ht->lock`（L1，handle table）
2. `ep->peer->lock`（L2，channel endpoint）

这符合 §4 定义的锁序（L1 < L2）。释放时反向（先 L2 再 L1）。

channel_recv 的锁获取顺序：
1. `ep->lock`（L2，channel endpoint）— 从队列取消息
2. 释放 `ep->lock`
3. `ht->lock`（L1，handle table）— 分配新条目

两步分离，不存在同时持有 L2 + L1 的情况。

### 4.5 Channel Recv 的 Partial Delivery 状态机

#### 问题定义

channel_recv 的两阶段分离（§4.4）引入了一个正确性间隙：步骤 1 从 peer 队列取出消息（标记为已消费），步骤 2 在接收方 HT 中分配条目。如果步骤 2 因为 HT 满而失败，消息已被取出但未安装到接收方的 handle table 中。

如果不正确处理，会导致：(1) 消息中的 handle 引用泄漏（refcount 增加但永不减少）；(2) 消息丢失（从队列取出但接收方未获得数据）。

#### 状态机设计

为 recv 操作引入三种消息状态：

```
                    channel_send
                        │
                        ▼
                  ┌─────────────┐
                  │   QUEUED    │  消息在 peer 队列中
                  └──────┬──────┘
                         │ channel_recv 步骤 1：取出消息
                         ▼
                  ┌─────────────┐
                  │ DELIVERING  │  消息已取出，正在安装到接收方 HT
                  └──────┬──────┘
                    ┌────┴────┐
                    │         │
              安装成功    安装失败（HT 满）
                    │         │
                    ▼         ▼
              ┌────────┐ ┌──────────────┐
              │DELIVERED│ │ DELIVERING   │ ← 等待重试
              │(完成)   │ │ (retryable) │
              └────────┘ └──────┬───────┘
                                 │
                          下次 recv 调用
                          直接进入步骤 2
                          （跳过步骤 1）
```

#### 实现

```c
// channel endpoint 新增字段：
typedef struct a20_channel_ep {
    // ... 现有字段 ...

    // Partial delivery 状态
    a20_ch_message_t *pending_msg;   // 当前正在投递的消息（NULL = 无）
    int               delivery_step; // 0 = 无投递, 1 = 步骤2待执行
} a20_channel_ep_t;
```

**recv 完整逻辑**：

```c
int64_t native_sys_channel_recv(a20_msg_recv_args_t *args) {
    task_t *cur = proc_current();
    a20_handle_table_t *ht = cur->handle_table;
    a20_channel_ep_t *ep = lookup_channel(args->channel, R);

    // === 步骤 1：从 peer 队列取消息（仅当无 pending_msg）===
    spin_lock(&ep->lock);
    if (ep->pending_msg == NULL) {
        if (ep->msg_count == 0) {
            if (args->timeout == 0) {
                spin_unlock(&ep->lock);
                return -WOULD_BLOCK;
            }
            // 阻塞等待...
            return block_and_wait(ep, args->timeout);
        }
        ep->pending_msg = dequeue_message(ep);
        ep->delivery_step = 1;
    }
    // pending_msg 非空：有正在投递的消息
    a20_ch_message_t *msg = ep->pending_msg;
    spin_unlock(&ep->lock);

    // === 步骤 2：在接收方 HT 安装 handles ===
    spin_lock(&ht->lock);

    // 检查 HT 容量
    if (ht->count + msg->handle_count > ht->capacity) {
        if (ht->capacity >= A20_HT_MAX_CAP) {
            spin_unlock(&ht->lock);
            // 不释放消息！保留 pending_msg 供重试
            return -NO_SPACE;  // 用户应 close 一些 handles 后重试
        }
        // 尝试扩容
        ht_grow(ht);
        if (ht->count + msg->handle_count > ht->capacity) {
            spin_unlock(&ht->lock);
            return -NO_SPACE;
        }
    }

    // 安装 handles
    for (int i = 0; i < msg->handle_count; i++) {
        int slot = ht_alloc_slot(ht);
        ht->entries[slot] = (a20_handle_entry_t){
            .object = msg->handles[i].object,
            .type   = msg->handles[i].type,
            .rights = msg->handles[i].rights
        };
        // 注意：refcount 已在 send 时增加，这里不再增加
    }

    // 复制数据到用户缓冲区
    copy_to_user(args->out_data, msg->data, msg->data_len);
    args->out_data_len = msg->data_len;
    args->out_handle_count = msg->handle_count;

    spin_unlock(&ht->lock);

    // === 清除 pending 状态 ===
    spin_lock(&ep->lock);
    kfree(ep->pending_msg);
    ep->pending_msg = NULL;
    ep->delivery_step = 0;
    spin_unlock(&ep->lock);

    return A20_OK;
}
```

#### 正确性保证

**定理 4.1（Partial Delivery 不泄漏引用）**：在 partial delivery 状态下，消息中的 handle 引用（refcount 已在 send 时增加）最终会被正确处理：
- 若 recv 成功安装 handles：引用被转移到接收方 HT，refcount 正确。
- 若 recv 返回 NO_SPACE：用户调用 `handle_close` 释放一些条目后重试 recv，最终成功安装。
- 若接收方进程退出（task_exit）：清理路径检查 `ep->pending_msg`，对其中每个 handle 调用 `object_refcount_dec`，释放引用。

**证明**：消息在 `ep->pending_msg` 中持有 handle 引用。这些引用的 refcount 已在 send 时增加。pending_msg 的生命周期绑定到 endpoint：要么 recv 成功后释放（refcount 由接收方 HT 继承），要么 endpoint 关闭时清理（§3 cleanup 协议）。不存在"取出但遗忘"的路径。$\square$

#### 退出时的清理

```c
// 在 task_exit 中，对每个 channel endpoint handle：
void a20_channel_ep_cleanup(a20_channel_ep_t *ep) {
    spin_lock(&ep->lock);
    if (ep->pending_msg) {
        // 释放 pending 消息中的 handle 引用
        for (int i = 0; i < ep->pending_msg->handle_count; i++) {
            object_refcount_dec(ep->pending_msg->handles[i].object);
        }
        kfree(ep->pending_msg);
        ep->pending_msg = NULL;
    }
    spin_unlock(&ep->lock);
}
```

---

## 5. Spawn 的 ELF 加载路径

### 5.1 从 File Handle 到 ELF 加载

现有内核的 ELF 加载路径（`mm/elf.c`）接受文件路径字符串。Native ABI 的 spawn 接受 file handle。

**需要新增的 API**：

```c
// 在 mm/elf.h 中新增：
int elf_load_from_vfile(vfile_t *vf, uint64_t *entry_out,
                         mm_struct_t *mm, uint64_t *stack_top);
```

此函数：
1. 通过 `vfile_t` 的 ops 读取 ELF header 和 program headers
2. 为每个 PT_LOAD segment 调用 `mm_mmap_file`（需要新增 `file_fd` → `vfile_t*` 的映射）
3. 设置入口点

**或者更简单的方案**：通过 `vfile_t->path` 获取文件路径，复用现有的 `proc_exec`。

### 5.2 a20_start_info_t 的构造

```c
// 在 task_spawn 实现中：
void native_setup_start_info(task_t *new_task, a20_start_info_t *info,
                              a20_handle_t root, a20_handle_t cwd,
                              a20_handle_t stdio[3], a20_handle_t self) {
    info->size = sizeof(a20_start_info_t);
    info->version = 1;
    info->argc = ...;
    info->envc = ...;
    info->root_dir = root;
    info->cwd_dir = cwd;
    info->stdin_handle = stdio[0];
    info->stdout_handle = stdio[1];
    info->stderr_handle = stdio[2];
    info->self_task = self;
    info->main_thread = self;  // 主线程 = task 自身
    info->default_event_queue = ...;  // 需要在 spawn 时创建
    info->page_size = PAGE_SIZE;
}
```

### 5.3 Spawn 的完整步骤

```
1. 预验证（持有 ht->lock）
   ├── 验证 image handle（FILE + READ）
   ├── 验证 root_dir handle（DIR + READ）
   ├── 验证 cwd_dir handle（DIR + READ）
   ├── 验证 event_queue handle（EVENTQ + READ）或创建默认的
   ├── 验证 stdio handles（FILE + READ/WRITE）
   └── 验证 handles[] 中的每个 handle（TRANSFER 权限）

2. 创建新 task
   ├── proc_alloc_user_image() — 分配 task_t, mm_struct, pgdir
   ├── elf_load_from_vfile() — 加载 ELF segments
   ├── 创建新 handle table
   ├── 复制 handles[] 到新 HT（增加 refcount）
   ├── 添加初始 handles（self, root, cwd, stdio, eq）
   ├── 构造 a20_start_info_t，写入新进程的用户栈顶
   ├── 设置 abi_mode = TASK_ABI_NATIVE
   └── proc_make_ready() — 加入就绪队列

3. 在父进程 HT 中分配 task handle
   └── rights = {WAIT, SIGNAL, CONTROL}
```

---

## 6. vm_share 与 mm 子系统交互

### 6.1 共享内存对象

```c
typedef struct a20_shm {
    refcount_t       refcount;
    spinlock_t       lock;

    // 物理页引用
    paddr_t         *pages;          // 物理页帧数组
    uint32_t         page_count;     // 页数
    uint32_t         page_cap;       // 数组容量

    // 来源信息（用于 CoW 决策）
    mm_struct_t     *source_mm;      // 原始地址空间（弱引用，不增加 refcount）
    uint64_t         source_addr;    // 原始虚拟地址
    uint64_t         source_len;     // 原始长度
    int              source_prot;    // 原始保护
} a20_shm_t;
```

### 6.2 vm_share 实现路径

```
1. 验证 [addr, addr+len) 在当前 AS 中
2. 分配 a20_shm_t
3. 对 addr 到 addr+len 的每个页：
   ├── 查找页表获取物理地址 paddr
   ├── 增加 physical frame 的引用计数（防止被回收）
   └── 记录 paddr 到 shm->pages[]
4. 创建 handle 指向 shm 对象
5. 返回 handle 给用户
```

### 6.3 vm_map(shm_handle) 实现路径

```
1. 验证 handle 类型 = MEMORY, 权限包含 MAP
2. 计算有效保护：effective_prot = prot & translate(rights)
3. 在目标地址空间中找到空闲区域
4. 创建 vm_area_t，标记为 VM_SHARED | VM_FILE
5. 对每个页：
   └── pt_map(pgdir, vaddr, shm->pages[i], effective_prot → pte_flags)
6. 不使用 CoW——共享内存的写操作直接反映到物理页
```

### 6.4 权限一致性

`vm_map` 时 effective protection 取 handle rights 和显式 prot 参数的交集。这保证：
- 即使显式请求了 PROT_WRITE，如果 handle 没有 WRITE 权限，映射也是只读的
- 页表项（PTE）的 R/W/X 位直接反映 effective protection

---

## 7. Syscall 分派实现

### 7.1 双 ABI 分派

```c
// kernel/syscall/syscall.c 修改后

int64_t syscall_dispatch(trap_context_t *ctx) {
    uint64_t num = TRAP_CTX_SYSCALL_NUM(ctx);
    task_t *cur = proc_current();

    // 快速路径：检查 task 的 ABI 模式
    if (cur && cur->abi_mode == TASK_ABI_NATIVE) {
        return native_syscall_dispatch(num, ctx);
    }

    // 默认：Linux ABI
    linux_syscall_args_t args = { .nr = num, .ctx = ctx, ... };
    const linux_syscall_entry_t *entry = linux_syscall_lookup(num);
    if (entry) return entry->handler(&args);
    // ...
}
```

**为什么用 `abi_mode` 而不是编号范围？**
- 编号范围方案：Linux 用 0-450，Native 用 0x0100-0x1FFF。但 `abi_mode` 更安全——防止 Native 程序意外调用 Linux syscall（反之亦然）。
- `abi_mode` 在 task 创建时确定，之后不可变。

### 7.2 Native 分派器

```c
// kernel/abi/native/syscall_table.c

typedef int64_t (*native_syscall_handler_t)(trap_context_t *ctx);

// 编号 → handler 的查找表
// 使用稀疏数组（4096 个条目，大部分为 NULL）
static native_syscall_handler_t native_syscall_table[0x1000];

int64_t native_syscall_dispatch(uint64_t num, trap_context_t *ctx) {
    if (num >= 0x1000) {
        // Experimental syscall
        if (num < 0x2000)
            return native_syscall_table_exp[num - 0x1000](ctx);
        return -A20_ERR_NOT_SUPPORTED;
    }
    native_syscall_handler_t handler = native_syscall_table[num];
    if (handler) return handler(ctx);
    return -A20_ERR_NOT_SUPPORTED;
}
```

---

## 8. 关键路径性能分析

### 8.1 Hot Path：handle_read/handle_write

每次 I/O 操作需要：
1. `syscall_dispatch`：O(1)（abi_mode 检查 + 数组查找）
2. `a20_handle_lookup`：O(1)（数组索引 + 权限检查）
3. 底层 `vfs_read/vfs_write`：与 Linux ABI 相同

额外开销：1 次 spinlock 获取/释放（lookup）+ 1 次类型/权限检查。

**预估额外延迟**：~30-50ns（spinlock + branch prediction miss）。

### 8.2 Hot Path：event_wait

1. `a20_handle_lookup(queue, EVENTQ, READ)`：O(1)
2. `spin_lock(&eq->lock)`：O(1)
3. 检查 ring buffer 非空：O(1)
4. 复制 events 到用户缓冲区：O(k)，k = min(pending_count, max_events)
5. `spin_unlock(&eq->lock)`

**对比 Linux epoll_wait**：
- epoll_wait 需要遍历 ready list（O(ready)）+ 对每个 fd 查找 fdtable
- A20 的 event_wait 直接从 ring buffer 复制，无需 fdtable 查找（events 包含 handle 编号）

---

## 9. 与 Linux ABI 的共享层

### 9.1 共享的核心模块 API

| Native syscall | 调用的 core API | 与 Linux ABI 共用 |
|---------------|----------------|------------------|
| handle_read | `vfs_read_file(vfile, ...)` | 是，Linux 也用此 API |
| handle_write | `vfs_write_file(vfile, ...)` | 是 |
| vm_alloc | `mm_mmap(mm, ...)` | 是，Linux 的 mmap 也用 |
| vm_map | `mm_mmap_file(mm, ...)` | 是 |
| vm_unmap | `mm_munmap(mm, ...)` | 是 |
| task_exit | `proc_exit(code)` | 是 |
| clock_get | `timer_get_ticks()` | 是 |

### 9.2 不共享的部分

| Native syscall | 需要的新 API | 说明 |
|---------------|-------------|------|
| handle_close | `a20_handle_table_close(ht, slot)` | 全新 API |
| handle_dup | `a20_handle_table_dup(ht, ...)` | 全新 API |
| task_spawn | `elf_load_from_vfile(vf, ...)` | 新增 API（基于现有 elf.c） |
| event_wait | `a20_eventq_wait(eq, ...)` | 全新 API |
| channel_send | `a20_channel_send(ep, ...)` | 全新 API |
| vm_share | `a20_shm_create(mm, addr, len)` | 全新 API |

**结论**：约 60% 的 syscall 复用现有 core API，40% 需要新增。这与 03 号文档的"不修改核心模块"原则一致——新增 API 不改变现有 API 的行为。

---

## 10. 实现优先级与依赖图

```
Phase 0（基础设施，无依赖）:
  ├── handle_table.c + handle.h          ← 所有其他模块的基础
  ├── task_t 扩展（abi_mode, handle_table）
  ├── syscall_dispatch 双 ABI 支持
  └── sys_core.c (abi_info)

Phase 1（最小可运行，依赖 Phase 0）:
  ├── sys_handle.c (close, dup, query)   ← 依赖 handle_table
  ├── sys_task.c (exit)                  ← 依赖 proc_exit
  ├── sys_memory.c (alloc, unmap)        ← 依赖 mm_mmap
  ├── sys_path.c (open, read, write)     ← 依赖 vfs, handle_table
  ├── sys_time.c (clock_get)             ← 无额外依赖
  └── sys_event.c (queue_create 基础)    ← 依赖 handle_table, wait_queue

Phase 2（IPC + 进程创建，依赖 Phase 1）:
  ├── sys_event.c (watch, wait)          ← 依赖 eventq, scheduler
  ├── sys_event.c (channel_create/send/recv) ← 依赖 channel, handle transfer
  ├── sys_task.c (spawn)                 ← 依赖 elf_load, channel
  └── sys_memory.c (vm_map, vm_share)    ← 依赖 shm, vfile handle

Phase 3（网络 + 安全，依赖 Phase 2）:
   ├── sys_net.c (全部)                   ← 依赖 socket, handle_table
   ├── sys_time.c (timer)                 ← 依赖 timer, eventq
   └── sys_security.c (ns)                ← 依赖 proc, handle_table
```

---

## 11. 对象销毁路径

### 11.1 统一销毁入口

所有 Native ABI 对象的销毁通过统一的 `object_destroy` 入口分发：

```c
// kernel/abi/native/object.c

void native_object_destroy(void *object, uint16_t type) {
    switch (type) {
    case A20_OBJ_FILE:
    case A20_OBJ_DIRECTORY:
    case A20_OBJ_DEVICE:
    case A20_OBJ_PIPE_ENDPOINT:
        // 复用 VFS 释放
        vfs_file_release((vfile_t *)object);
        break;

    case A20_OBJ_CHANNEL_ENDPOINT:
        a20_channel_ep_destroy((a20_channel_ep_t *)object);
        break;

    case A20_OBJ_EVENT_QUEUE:
        a20_eventq_destroy((a20_eventq_t *)object);
        break;

    case A20_OBJ_TIMER:
        a20_timer_destroy((a20_timer_t *)object);
        break;

    case A20_OBJ_MEMORY:
        a20_shm_destroy((a20_shm_t *)object);
        break;

    case A20_OBJ_SOCKET:
        a20_socket_destroy((struct a20_socket *)object);
        break;

    case A20_OBJ_NAMESPACE:
        a20_namespace_destroy((struct a20_namespace *)object);
        break;

    case A20_OBJ_DEBUG:
        kfree(object);
        break;

    case A20_OBJ_TASK:
        // task 的销毁由 proc 子系统管理
        // 不在此处处理
        break;

    default:
        KWARN("unknown object type %d in destroy", type);
        break;
    }
}
```

### 11.2 Channel Endpoint 销毁

```c
void a20_channel_ep_destroy(a20_channel_ep_t *ep) {
    // 步骤 1：通知对端
    if (ep->peer) {
        spin_lock(&ep->peer->lock);
        ep->peer->peer_closed = 1;
        // 唤醒对端等待者（使其返回 PEER_CLOSED）
        wait_queue_wake_all(&ep->peer->waiters);
        ep->peer->peer = NULL;  // 断开双向链接
        spin_unlock(&ep->peer->lock);
        // 注意：不 refcount_dec peer，因为 peer 的生命周期由自己的 handle 管理
    }

    // 步骤 2：释放 pending_msg（如果有）
    if (ep->pending_msg) {
        for (int i = 0; i < ep->pending_msg->handle_count; i++) {
            object_refcount_dec(ep->pending_msg->handles[i].object);
        }
        kfree(ep->pending_msg);
    }

    // 步骤 3：释放队列中的所有消息
    for (uint32_t i = 0; i < ep->msg_count; i++) {
        a20_ch_message_t *msg = ep->msg_queue[(ep->msg_head + i) % ep->msg_cap];
        for (int j = 0; j < msg->handle_count; j++) {
            object_refcount_dec(msg->handles[j].object);
        }
        kfree(msg);
    }
    kfree(ep->msg_queue);

    // 步骤 4：释放 endpoint 自身
    kfree(ep);
}
```

**安全性论证**：

1. 步骤 1 中设置 `peer_closed` 并唤醒对端。对端在被唤醒后检查 `peer_closed`，返回 `PEER_CLOSED` 错误。不存在对已释放 endpoint 的后续访问——对端通过 `peer` 指针访问时持锁，且 `peer = NULL` 断开了链接。

2. 步骤 2-3 释放消息中的 handle 引用。每条消息的 handle 引用在 send 时增加 refcount（§4.3），此处减少 refcount。若 refcount 降至 0，触发 `object_destroy`（可能级联）。

3. 引用计数保证 `a20_channel_ep_destroy` 仅在 refcount = 0 时被调用，因此无并发访问风险。

### 11.3 SHM 销毁

```c
void a20_shm_destroy(a20_shm_t *shm) {
    // 对每个物理页减少引用计数
    for (uint32_t i = 0; i < shm->page_count; i++) {
        paddr_t paddr = shm->pages[i];
        // 减少物理页帧引用计数（可能释放物理页）
        pmm_dec_refcount(paddr);
    }
    kfree(shm->pages);

    // 清除 source_mm 的弱引用（无需操作——弱引用不增加 refcount）
    kfree(shm);
}
```

### 11.4 Timer 销毁

```c
void a20_timer_destroy(a20_timer_t *timer) {
    // 取消内核定时器（如果活跃）
    if (timer->active) {
        timer_cancel_kernel(timer->kernel_timer_id);
    }
    kfree(timer);
}
```

### 11.5 级联销毁分析

当 `handle_close` 触发 `object_destroy` 时，destroy 可能释放其他对象的引用（如 channel destroy 释放消息中的 handle 引用），导致进一步的对象销毁。

**级联深度分析**：

| 起始对象 | 可能的级联链 | 最大深度 |
|---------|------------|---------|
| channel ep | 消息中的 handles → 其他对象 | 2 |
| event queue | watch entries → 无对象引用 | 1 |
| task | 全部 handles → 任意对象 | 2 |
| shm | 物理页引用 | 1 |
| file/dir | 无级联 | 0 |

**最大深度 2** 的论证：task destroy 关闭 handles（深度 1），每个 handle 指向的对象在 destroy 时可能释放消息中的 handle 引用（深度 2），但这些被释放的 handle 引用指向的对象不再触发进一步的级联（因为消息中的 handle 引用只是 refcount_dec，不会触发这些对象内部的 cleanup）。$\square$

**实现注意**：级联销毁在 handle_close 的 spinlock 临界区外执行（延迟清理策略，见 §3.4），因此不存在锁重入或死锁风险。

---

## 12. IRQ 安全性分析

### 12.1 问题

内核中的事件产生（如定时器到期、I/O 完成中断、网络数据到达）通常在中断上下文（IRQ context）中执行。中断上下文不能睡眠、不能获取互斥锁（只能获取 spinlock 且必须禁用中断），且不能调用可能引起调度的函数。A20OS 的 `a20_event_notify` 在中断/软中断上下文中被调用，需要分析其在 IRQ 上下文中的安全性。

### 12.2 IRQ 上下文中的操作分析

| 操作 | IRQ 安全 | 原因 |
|------|----------|------|
| `spin_lock(&eq->lock)` | ❌ 不安全 | IRQ 可能中断当前持有 `eq->lock` 的线程，导致自旋死锁 |
| `spin_lock_irqsave(&eq->lock, flags)` | ✅ 安全 | 禁用本地 CPU 中断，防止 IRQ 重入 |
| `kfree(msg)` | ❌ 不安全 | `kfree` 可能涉及睡眠（如 slab 分配器的锁） |
| `ring_buffer_enqueue(eq, event)` | ✅ 安全 | 纯内存写入 + 索引更新，无内存分配 |
| `wait_queue_wake_one(&eq->waiters)` | ✅ 安全 | 仅将等待线程移入就绪队列，不调度 |

### 12.3 安全的 IRQ 事件分发

```c
// 在中断/软中断上下文中调用
void a20_event_notify_irq(void *target_object, uint16_t target_type,
                           uint32_t event_type, uint64_t data0, uint64_t data1) {
    // 遍历全局反向索引，找到 watch 了 target_object 的 event queue
    // （全局索引查询需要 spin_lock_irqsave）
    unsigned long flags;
    spin_lock_irqsave(&object_watches_lock, flags);

    a20_watch_entry_list_t *list = object_watches_lookup(target_object);
    if (!list) {
        spin_unlock_irqrestore(&object_watches_lock, flags);
        return;
    }

    // 在锁保护下收集目标 event queue 列表
    // （不能在 object_watches_lock 内获取 eq->lock——层次冲突）
    struct eq_target targets[MAX_NOTIFY_TARGETS];
    int count = 0;
    for each watch_entry w in list {
        if (w->event_mask & event_type) {
            targets[count++] = { .eq = w->owner_queue, .udata = w->user_data };
        }
    }
    spin_unlock_irqrestore(&object_watches_lock, flags);

    // 对每个目标 event queue，安全地追加事件
    for (int i = 0; i < count; i++) {
        a20_eventq_t *eq = targets[i].eq;
        spin_lock_irqsave(&eq->lock, flags);

        if (eq->ring_count < eq->ring_cap) {
            // 追加事件到 ring buffer
            eq->ring[eq->ring_tail] = (a20_pending_event_t){
                .source = ...,
                .type = event_type,
                .user_data = targets[i].udata,
                .data0 = data0,
                .data1 = data1
            };
            eq->ring_tail = (eq->ring_tail + 1) % eq->ring_cap;
            eq->ring_count++;
            // 唤醒消费者
            wait_queue_wake_one(&eq->waiters);
        } else {
            // Ring buffer 满：唤醒消费者但不追加事件
            // （定理 2.3 情况 2："先唤醒后丢弃"策略）
            wait_queue_wake_one(&eq->waiters);
        }

        spin_unlock_irqrestore(&eq->lock, flags);
    }
}
```

### 12.4 IRQ 安全性定理

**定理 12.1** `a20_event_notify_irq` 在 IRQ 上下文中安全执行：不会导致自旋死锁、不会睡眠、不会访问已释放的内存。

*证明*：
1. **无自旋死锁**：使用 `spin_lock_irqsave` 禁用本地 CPU 中断。即使在持有 `eq->lock` 时 IRQ 到达，CPU 不会响应中断，不会重入同一锁。
2. **无睡眠**：代码路径中不调用 `kzalloc`、`kfree`、`mutex_lock` 或任何可能阻塞的函数。Ring buffer 写入是纯内存操作。
3. **无 use-after-free**：全局反向索引持有 `eq` 的裸指针。若 `eq` 被销毁（`a20_eventq_destroy`），`a20_eventq_destroy` 先从全局反向索引中移除所有相关 entry（在 `object_watches_lock` 保护下）。因此，`a20_event_notify_irq` 在获取 `object_watches_lock` 后读到的 `eq` 指针保证存活（销毁操作与通知操作互斥）。
4. **锁序**：`object_watches_lock`（全局，L2 级别）先于 `eq->lock`（L2 级别，但不同实例）。先释放前者再获取后者，不违反递增规则（不嵌套持有）。$\square$

### 12.5 与调度器交互

`wait_queue_wake_one` 在 IRQ 上下文中被调用时的行为：
- 将等待线程从 `waiters` 队列移到就绪队列
- **不立即调度**——标记 `need_resched`，在 IRQ 返回时检查
- 在 `spin_unlock_irqrestore` 后，若 `need_resched` 且从中断返回到内核态，触发调度
- 若返回到用户态，在 `restore_user_context` 中检查调度

这保证了事件通知的原子性：事件追加和消费者唤醒在同一 `irqsave` 临界区内完成。

---

## 13. ht_grow 精化论证

### 13.1 问题描述

`ht_grow` 在 handle table 满时扩容：分配新数组、复制旧条目、更新指针。这改变了 `entries` 指针和 `capacity`，但不应改变精化映射 $abs$（即 `RI-1` 到 `RI-4` 在扩容前后应保持）。

### 13.2 ht_grow 实现

```c
static void ht_grow(a20_handle_table_t *ht) {
    // 调用者已持有 ht->lock
    uint32_t new_cap = ht->capacity * A20_HT_GROWTH_FACTOR;
    if (new_cap > A20_HT_MAX_CAP) new_cap = A20_HT_MAX_CAP;

    a20_handle_entry_t *new_entries = kzalloc(sizeof(*new_entries) * new_cap);
    if (!new_entries) return;  // 扩容失败，调用者检测 NO_SPACE

    uint64_t *new_bitmap = kzalloc(sizeof(uint64_t) * (new_cap / 64 + 1));
    if (!new_bitmap) { kfree(new_entries); return; }

    // 复制旧条目
    memcpy(new_entries, ht->entries, sizeof(*new_entries) * ht->capacity);

    // 重建 bitmap：已占用的 slot 设为 1，新增 slot 设为 0
    memcpy(new_bitmap, ht->free_bitmap, sizeof(uint64_t) * ht->bitmap_size);
    // 新增 slot 的 bitmap 位初始化为 0（空闲）
    // kzalloc 已将新部分清零，无需额外操作

    // 替换
    a20_handle_entry_t *old_entries = ht->entries;
    uint64_t *old_bitmap = ht->free_bitmap;

    ht->entries = new_entries;
    ht->free_bitmap = new_bitmap;
    ht->capacity = new_cap;
    ht->bitmap_size = new_cap / 64 + 1;

    // 释放旧数组（在持锁期间——安全因为无其他线程可并发访问）
    kfree(old_entries);
    kfree(old_bitmap);
}
```

### 13.3 精化论证

**定理 13.1** `ht_grow` 保持精化不变式 RI-1 到 RI-4。

*证明*：

设扩容前状态为 $\sigma_c$，扩容后为 $\sigma_c'$。需证明 $abs(\sigma_c') = abs(\sigma_c)$。

1. **RI-1（Handle 表对应）**：
   - 对所有 $n < capacity_{old}$：`new_entries[n]` 是从 `old_entries[n]` 复制的，内容完全相同。$HT_a^p(n)$ 的映射不变。
   - 对所有 $n \in [capacity_{old}, capacity_{new})$：`new_entries[n]` 由 `kzalloc` 初始化为 0（`object = NULL`），$HT_a^p(n)$ 为 undefined。这些编号之前也未有定义（超出 `capacity`）。
   - 因此 $HT_a^p$ 不变。$\checkmark$

2. **RI-2（引用计数对应）**：handle 条目内容不变（memcpy 是逐字节复制），`object` 指针不变，refcount 映射不变。$\checkmark$

3. **RI-3（对象状态对应）**：`ht_grow` 不修改任何对象的状态。$\checkmark$

4. **RI-4（Bitmap 一致性）**：
   - 对所有 $n < capacity_{old}$：`new_bitmap` 从 `old_bitmap` 复制，bit 值不变。
   - 对所有 $n \in [capacity_{old}, capacity_{new})$：`new_bitmap` 的对应位为 0（`kzalloc` 清零），`new_entries[n].object = NULL`（`kzalloc` 清零）。$bit = 0 \iff object = NULL$。$\checkmark$

5. **线性化点**：`ht->entries = new_entries` 赋值瞬间。在此之后，所有通过 `ht->entries` 的访问都使用新数组。

6. **kfree 的安全性**：`kfree(old_entries)` 在持锁期间执行。由于持有 `ht->lock`，无其他线程能访问 `ht->entries`（所有操作在获取锁后才读取 entries 指针）。因此 `kfree` 不会导致 use-after-free。$\checkmark$

$\square$

### 13.4 kzalloc 失败的处理

若 `kzalloc` 返回 NULL，`ht_grow` 直接返回不修改任何状态。调用者检测到 `ht_alloc_slot` 仍返回 -1，返回 `NO_SPACE` 错误。这是安全的——不修改状态意味着 RI 平凡保持。

---

## 14. Channel Recv 两阶段锁精化证明

### 14.1 问题回顾

channel_recv 的实现采用两阶段分离（07 §4.3）：
1. 阶段 1：锁 `ep->lock`（L2），从队列取出消息，释放 `ep->lock`
2. 阶段 2：锁 `ht->lock`（L1），在接收方 HT 中分配条目，释放 `ht->lock`

这避免了锁序违规（先 L2 后 L1）。但两阶段之间有间隙——需要证明此间隙不违反精化关系。

### 14.2 两阶段的 SOS 对应

**抽象规则 CH-RECV** 是单步转移：

$$\frac{HT_p(n) = (o_{ch}, \rho) \quad R \in \rho \quad q_{local} \neq []}{\langle recv_p(n, cap), \sigma \rangle \longrightarrow \langle ok(d_1, H_1'), \sigma' \rangle}$$

具体实现分两步完成。精化论证需要证明：两步的组合等价于单步 SOS 转移。

### 14.3 中间状态的精化分析

设具体执行路径为 $\sigma_c^0 \to \sigma_c^1 \to \sigma_c^2$，其中：
- $\sigma_c^0$：recv 开始前的状态
- $\sigma_c^1$：阶段 1 完成（消息已从 peer 队列取出，`ep->pending_msg` 非空）
- $\sigma_c^2$：阶段 2 完成（消息中的 handles 已安装到接收方 HT）

**关键问题**：$\sigma_c^1$ 是否对应某个合法的抽象状态？

**观察**：在 $\sigma_c^1$ 中，消息已从 peer 队列取出但尚未安装到接收方 HT。此时：
- 消息中的 handle 引用的 refcount 已在 send 时增加，当前仍保持
- 消息存在于 `ep->pending_msg` 中——这是一个**实现层**的中间状态，没有直接的 SOS 抽象对应

**精化论证策略**：不要求中间状态 $\sigma_c^1$ 满足精化关系，只要求起点 $\sigma_c^0$ 和终点 $\sigma_c^2$ 满足。这符合 Herlihy & Wing 的 linearizability 定义——线性化点在阶段 2 完成时刻。

**定理 14.1（Channel Recv 两阶段精化）** 若 $RI(\sigma_c^0, \sigma_a^0)$ 成立，且 recv 成功完成（$\sigma_c^0 \to \sigma_c^1 \to \sigma_c^2$），则存在抽象转移 $\sigma_a^0 \to \sigma_a^2$ 使得 $RI(\sigma_c^2, \sigma_a^2)$ 成立且抽象转移与 SOS 规则 CH-RECV 一致。

*证明*：

1. **$\sigma_c^0 \to \sigma_c^1$（阶段 1）**：
   - 从 peer 队列取出消息：`dequeue_message(ep)` 修改 `ep->msg_queue`、`ep->msg_count`、`ep->total_data`。
   - `ep->pending_msg` 记录取出的消息。
   - 对抽象状态 $\sigma_a^0$：此步骤在抽象层无对应——SOS 的 CH-RECV 是单步完成。但在精化论证中，我们不需要中间状态的精化——只需证明最终状态的一致性。

2. **$\sigma_c^1 \to \sigma_c^2$（阶段 2）**：
   - 在接收方 HT 中安装 handles：对每个 `msg->handles[i]`，调用 `ht_alloc_slot` + 写入条目。
   - 数据复制到用户缓冲区。
   - 清除 `ep->pending_msg`。

3. **最终状态对应**：
   - **RI-1**：$\sigma_c^2$ 中接收方 HT 新增了 `msg->handle_count` 个条目，每个条目 $(o_i, \rho_i)$ 与消息中的 handle 信息一致。对应 SOS 中 $H_1' = \{(n_j, (o_j, \rho_j))\}$。$\checkmark$
   - **RI-2**：refcount 在 send 时已增加（+1 per handle），recv 中不再增加。最终 refcount_c = refcount_a。$\checkmark$
   - **RI-3**：channel endpoint 的 `msg_queue` 减少了一条消息，`total_data` 减少了 `msg->data_len`。对应 SOS 中 $q_{local}$ 失去第一个元素。$\checkmark$
   - **RI-4**：接收方 HT 的 bitmap 更新与新条目一致。$\checkmark$

4. **线性化点**：阶段 2 的 `spin_unlock(&ht->lock)` 瞬间。在此时刻，所有状态修改已完成，对外可见。

5. **失败路径（NO_SPACE）**：若阶段 2 因 HT 满而失败：
   - 消息保留在 `ep->pending_msg` 中，不释放。
   - 具体状态回到 $\sigma_c^1$（消息已取出但未安装）。
   - 对应抽象层：recv 操作尚未发生（线性化点未到达）。用户重试 recv 时，阶段 1 被跳过（`pending_msg` 非空），直接进入阶段 2。
   - **RI 保持**：$\sigma_c^1$ 中消息仍在 `pending_msg` 中，refcount 未变化（send 时增加的 refcount 保持不变）。当用户关闭一些 handles 后重试，阶段 2 成功，RI 在 $\sigma_c^2$ 中恢复完整。

$\square$

### 14.4 进程退出时的清理

若接收方进程在 `pending_msg` 非空时退出（`task_exit`），清理路径（08 §4.5）对 `pending_msg` 中的每个 handle 调用 `object_refcount_dec`，释放 send 时增加的引用。对应抽象层：消息从未被接收（recv 的线性化点从未到达），send 增加的 refcount 被正确回退。RI 保持。$\checkmark$
