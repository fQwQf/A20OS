# A20OS Native ABI：Handle 子系统设计

> 本文档定义 Handle 的 13 种对象类型、生命周期状态机、handle table 数据结构和操作语义。权限模型见 [security.md](06-security.md)，类型定义见 [types.md](01-types.md)。

---

## 1. 对象类型

A20OS Native ABI 定义 13 种对象类型。每种类型有特定的合法操作和权限集。

```c
typedef enum a20_object_type {
    A20_OBJ_INVALID           = 0,
    A20_OBJ_TASK              = 1,
    A20_OBJ_THREAD            = 2,
    A20_OBJ_FILE              = 3,
    A20_OBJ_DIRECTORY         = 4,
    A20_OBJ_SOCKET            = 5,
    A20_OBJ_PIPE_ENDPOINT     = 6,
    A20_OBJ_CHANNEL_ENDPOINT  = 7,
    A20_OBJ_EVENT_QUEUE       = 8,
    A20_OBJ_TIMER             = 9,
    A20_OBJ_MEMORY            = 10,   /* 共享内存对象 (shm) */
    A20_OBJ_DEVICE            = 11,
    A20_OBJ_NAMESPACE         = 12,
    A20_OBJ_DEBUG             = 13,
} a20_object_type_t;
```

### 1.1 类型到内核结构体的映射

| Native 类型 | 内核结构体 | 引用计数机制 |
|------------|-----------|-------------|
| A20_OBJ_FILE | `vfile_t *` | `vfile_t.ref_count` |
| A20_OBJ_DIRECTORY | `vfile_t *` (O_DIRECTORY) | `vfile_t.ref_count` |
| A20_OBJ_SOCKET | 新增 `struct a20_socket` | `refcount_t` |
| A20_OBJ_PIPE_ENDPOINT | `vfile_t *` (pipe 端) | `vfile_t.ref_count` |
| A20_OBJ_CHANNEL_ENDPOINT | 新增 `struct a20_channel_ep` | `refcount_t` |
| A20_OBJ_EVENT_QUEUE | 新增 `struct a20_eventq` | `refcount_t` |
| A20_OBJ_TIMER | 新增 `struct a20_timer` | `refcount_t` |
| A20_OBJ_TASK | `task_t *` | 由 proc 管理生命周期 |
| A20_OBJ_THREAD | `task_t *` (线程) | 同上 |
| A20_OBJ_MEMORY | 新增 `struct a20_shm` | `refcount_t` |
| A20_OBJ_DEVICE | `vfile_t *` (设备文件) | `vfile_t.ref_count` |
| A20_OBJ_NAMESPACE | 新增 `struct a20_namespace` | `refcount_t` |
| A20_OBJ_DEBUG | 新增 `struct a20_debug` | `refcount_t` |

### 1.2 设计原则

- **复用优先**：file、directory、device、pipe 都复用 `vfile_t`，避免为每种文件相关对象创建独立结构体。
- **独立对象独立分配**：channel、event queue、timer 等 VFS 无法表达的对象使用独立结构体。
- **handle entry 的 object 字段是 `void *`**：统一持有不同类型对象的指针，通过 `type` 字段区分。

---

## 2. Handle Table 数据结构

### 2.1 核心结构

```c
#define A20_HT_INITIAL_CAP    256
#define A20_HT_MAX_CAP        65536
#define A20_HT_GROWTH_FACTOR  2

typedef struct a20_handle_entry {
    void             *object;    /* 指向内核对象 */
    uint16_t          type;      /* a20_object_type_t */
    uint16_t          _pad;
    a20_rights_t      rights;    /* 该 handle 的声明权限位域 */
    // 时态能力字段（temporal capability）
    uint64_t          expiry_tick;    /* 绝对过期时刻（kernel ticks），0 = 无时间过期 */
    uint32_t          remaining_ops;  /* 剩余操作次数，0 = 无限次（不是"已耗尽"） */
    uint32_t          temporal_flags; /* 时态标志位 */
} a20_handle_entry_t;

// 时态标志位定义
#define A20_TEMPORAL_EXPIRY_ABSOLUTE  (1u << 0)  /* 使用绝对过期时刻 */
#define A20_TEMPORAL_OP_COUNT         (1u << 1)  /* 使用操作次数限制 */
#define A20_TEMPORAL_AUTO_CLOSE       (1u << 2)  /* 过期后自动关闭（而非仅清零权限） */

typedef struct a20_handle_table {
    a20_handle_entry_t *entries;     /* 动态数组，索引 = handle 编号 */
    uint32_t            capacity;    /* 当前数组容量 */
    uint32_t            count;       /* 已使用条目数 */
    uint32_t            free_hint;   /* 下一次搜索的起始位置 */
    spinlock_t          lock;        /* 保护全部字段（L1 级） */
    uint64_t           *free_bitmap; /* 每个 bit: 1=已占用, 0=空闲 */
    uint32_t            bitmap_size; /* bitmap 的 uint64 元素数 */
} a20_handle_table_t;
```

### 2.2 Bitmap 约定

- **bit = 1** 表示槽位已占用（used）
- **bit = 0** 表示槽位空闲（free）

分配时 `|= (1ULL << bit)` 标记为已占用；释放时 `&= ~(1ULL << bit)` 标记为空闲。

### 2.3 设计决策

**动态数组 vs 红黑树**：A20OS 的 handle 编号是连续 `uint32_t`。动态数组提供 O(1) 的 lookup 和 close，而树的 lookup 是 O(log n)。在 65536 个 handle 上限下，数组占用约 65536 × 24 bytes ≈ 1.5 MB。

**Free bitmap 的作用**：Free slot 查找在纯数组上是 O(n)。Bitmap 将其优化为 O(n/64) 的 word 级扫描，配合 `free_hint` 记录上次释放的位置，实际接近 O(1)。

### 2.4 关键操作

```c
/* 分配空闲槽位：O(n/64) worst case */
static int ht_alloc_slot(a20_handle_table_t *ht) {
    for (uint32_t i = ht->free_hint / 64; i < ht->bitmap_size; i++) {
        uint64_t word = ht->free_bitmap[i];
        if (word != UINT64_MAX) {
            int bit = __builtin_ctzll(~word);  /* 找第一个 0 bit（空闲） */
            uint32_t slot = i * 64 + bit;
            if (slot < ht->capacity) {
                ht->free_bitmap[i] |= (1ULL << bit);  /* 标记已占用 */
                ht->free_hint = slot + 1;
                return slot;
            }
        }
    }
    return -1;  /* 需要扩容 */
}

/* Handle lookup：O(1)，检查有效权限 ρ_eff */
int64_t a20_handle_lookup(a20_handle_table_t *ht, a20_handle_t h,
                          uint16_t expected_type, a20_rights_t required_rights,
                          a20_handle_entry_t *out) {
    if (h >= ht->capacity) return -A20_ERR_BAD_HANDLE;
    a20_handle_entry_t *e = &ht->entries[h];
    if (e->object == NULL) return -A20_ERR_BAD_HANDLE;
    if (expected_type != A20_OBJ_INVALID && e->type != expected_type)
        return -A20_ERR_BAD_HANDLE;
    /* 计算有效权限：过期或操作次数耗尽则有效权限为空 */
    a20_rights_t effective = a20_effective_rights(e);
    if ((effective & required_rights) != required_rights)
        return -A20_ERR_ACCESS;
    /* 若启用了操作计数，递减剩余次数 */
    if (e->temporal_flags & A20_TEMPORAL_OP_COUNT && e->remaining_ops > 0)
        e->remaining_ops--;
    *out = *e;
    out->rights = effective;  /* 返回有效权限 */
    return A20_OK;
}

/* 计算有效权限 ρ_eff(h, t) */
static inline a20_rights_t a20_effective_rights(const a20_handle_entry_t *e) {
    /* 检查时间过期 */
    if (e->expiry_tick > 0 && a20_current_tick() >= e->expiry_tick)
        return 0;  /* 已过期，有效权限为空 */
    /* 检查操作次数耗尽（remaining_ops == 0 且 flags 包含 OP_COUNT 表示已耗尽） */
    if ((e->temporal_flags & A20_TEMPORAL_OP_COUNT) && e->remaining_ops == 0)
        return 0;  /* 操作次数已耗尽 */
    return e->rights;  /* 未过期且次数未耗尽，返回声明权限 */
}
```

### 2.5 增长策略

当 `count == capacity` 时，按 `A20_HT_GROWTH_FACTOR = 2` 倍扩容。扩容在持锁期间分配新数组、复制旧条目、释放旧数组。到达 `A20_HT_MAX_CAP` 时拒绝扩容。

### 2.6 时态能力（Temporal Capabilities）

A20OS 的 handle 条目支持**时态约束**——权限可以在时间或操作次数上受限。

#### 2.6.1 有效权限 $\rho_{eff}$

Handle 的有效权限是声明权限的时态投影：

$$\rho_{eff}(h, t) = \begin{cases} \rho(h) & \text{if } expiry(h) = 0 \text{ or } t < expiry(h) \text{, and } remaining(h) \neq 0 \\ \emptyset & \text{otherwise} \end{cases}$$

所有 syscall 的权限检查统一使用 $\rho_{eff}$ 而非声明权限 $\rho$。

#### 2.6.2 handle_dup 的时态约束

`handle_dup` 创建的新 handle 的时态参数**不宽松于**源 handle：

- `expiry' ≤ expiry(source)`：新 handle 的过期时刻不能晚于源
- `remaining_ops' ≤ remaining_ops(source)`：新 handle 的操作次数不能多于源

这保证了**时态不可刷新性**：持有者无法通过 dup 延长自己的能力生命周期。

#### 2.6.3 过期行为

Handle 过期后有两种行为模式：

| `temporal_flags` | 过期后行为 | 资源占用 |
|---|---|---|
| 未设 `AUTO_CLOSE` | **惰性化**：$\rho_{eff} = \emptyset$，但 entry 仍占用 slot 和 refcount | 占用，需显式 `handle_close` 释放 |
| 设 `AUTO_CLOSE` | **自动关闭**：内核 sweeper 在过期时执行 `handle_close` 的效果 | 不占用 |

#### 2.6.4 Sweeper 机制

内核维护一个后台 sweeper，定期扫描 handle table 中设置了 `A20_TEMPORAL_EXPIRY_ABSOLUTE | A20_TEMPORAL_AUTO_CLOSE` 的条目：

1. 获取 `ht->lock`
2. 检查 `current_tick ≥ entry.expiry_tick`
3. 若已过期，执行 `handle_close` 的效果（清空 entry、递减 refcount、释放 bitmap bit）
4. 释放锁

Sweeper 与正常操作的竞争通过同一把 `ht->lock` 串行化，保证过期原子性（定理 3.3，见 09 §3.6）。

---

## 3. Handle 生命周期

### 3.1 状态机

Handle 在其生命周期中经历以下状态：

```text
                    alloc (ht_alloc_slot)
                         │
                         v
    ┌─────────────────────────────────┐
    │           Free                  │  entry.object == NULL, bitmap bit = 0
    └─────────────────────────────────┘
                         │  write entry (object, type, rights, temporal)
                         │  bitmap bit = 1
                         v
    ┌─────────────────────────────────┐
    │          Active                 │  entry.object != NULL, ρ_eff ≠ ∅, 正常使用
    └─────────────────────────────────┘
          │            │            │            │
          │ dup        │ transfer   │ replace    │ temporal expiry
          v            v            v            v
    ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌───────────────────┐
    │Duplicated│ │Transient │ │Replacing │ │Expired (惰性化)   │
    │ (新槽位) │ │ (传递中) │ │ (替换中) │ │ ρ_eff = ∅         │
    └──────────┘ └──────────┘ └──────────┘ │ 但 entry 仍在      │
          │            │            │      └───────────────────┘
          │            v            │            │
          │     ┌──────────┐        │            │ (若 AUTO_CLOSE)
          │     │ Received │        │            │ = handle_close
          │     │ (接收端) │        │            v
          │     └──────────┘        │     ┌───────────────────┐
          │            │            │     │Auto-Closed        │
          v            v            v     │(sweeper 自动回收)  │
    ┌─────────────────────────────────┐  └───────────────────┘
    │          Closing                │  handle_close 调用中
    └─────────────────────────────────┘
                         │
                         │ entry.object = NULL, bitmap bit = 0
                         │ refcount_dec → 如果最后引用，object_destroy
                         v
    ┌─────────────────────────────────┐
    │          Released               │  回到 Free 状态
    └─────────────────────────────────┘
```

### 3.2 状态转换规则

| 转换 | 触发 | 效果 |
|------|------|------|
| Free → Active | handle 分配（dup/recv/spawn/open） | 写入 object/type/rights/temporal，bitmap bit = 1 |
| Active → Duplicated | handle_dup | 新槽位变为 Active，时态参数不宽松于源 |
| Active → Transient | channel_send 传递 handle | 原 handle 权限不变，接收方获得新 handle |
| Active → Replacing | handle_replace | 原 handle 失效，新 handle 继承权限子集 |
| Active → Expired | current_tick ≥ expiry_tick 或 remaining_ops 递减至 0 | $\rho_{eff} = \emptyset$（惰性化） |
| Expired → Auto-Closed | sweeper 检测到过期且 AUTO_CLOSE flag | 等同 handle_close |
| Expired → Closing | handle_close（显式关闭已过期 handle） | 清空 entry，bitmap bit = 0 |
| Active → Closing | handle_close | 清空 entry，bitmap bit = 0 |
| Closing → Released | refcount_dec 完成 | 如果是最后引用，销毁对象 |

### 3.3 对象销毁的级联效应

当对象的引用计数降至 0 时，触发 `object_destroy`。这可能导致级联销毁：

```text
task_destroy
  ├── 遍历 handle_table，close 所有 handle
  │   ├── channel endpoint close → 通知对端 peer_closed
  │   ├── event queue close → 清理 watch list + 反向索引
  │   └── shm close → 解除映射
  └── 释放 task 结构

最大级联深度：2
  task → handle → channel message 中的 handle 引用
```

---

## 4. Syscall 操作语义

### 4.1 handle_close

```c
int64_t handle_close(a20_handle_t handle);
```

1. 验证 handle 有效（lookup 成功）
2. 清空 entry（object = NULL, type = INVALID）
3. 清除 bitmap bit（标记空闲）
4. 释放 HT lock
5. `refcount_dec`：如果最后引用，调用 `object_destroy`（可能触发级联）

**原子性**：entry 清空在 HT lock 内完成。object_destroy 在锁外执行（refcount = 0 后无其他线程可访问对象）。

### 4.2 handle_dup

```c
int64_t handle_dup(a20_handle_dup_args_t *args);
```

1. 验证源 handle 有效，检查 `DUP` 权限（使用 $\rho_{eff}$）
2. 验证 `rights_mask ⊆ source.rights`（权限子集检查）
3. 分配新槽位（ht_alloc_slot）
4. 写入新 entry（同一 object, 请求的 rights）
5. **时态约束**：`new.expiry_tick ≤ source.expiry_tick`，`new.remaining_ops ≤ source.remaining_ops`
6. `refcount_inc(object)`（原子增加）

### 4.3 handle_replace

```c
int64_t handle_replace(a20_handle_t handle, a20_rights_t rights, a20_handle_t *out);
```

原子替换：原 handle 失效，新 handle 继承权限子集。用于需要不可分割地改变 handle 的场景（如降级后立即使用）。

1. 验证源 handle 有效，检查 `DUP` 权限
2. 分配新槽位
3. 写入新 entry（rights 子集）
4. 清空原 entry（原子替换）
5. `refcount` 不变（一个引用从旧槽位移到新槽位）

### 4.4 handle_query

```c
int64_t handle_query(a20_handle_t handle, a20_handle_info_t *out);
```

只读操作。返回对象类型、状态、权限和调试 hint。需要 `STAT` 权限。

---

## 5. 并发协议

### 5.1 锁层级

Handle table lock 位于锁层级 L1：

```text
L0 (IRQ) < L1 (handle table) < L2 (内核对象) < L3 (调度器) < L4 (mm)
```

规则：
- 获取 L1 后可以获取 L2（但不可以反过来）
- 任何操作不允许在持有 L1 时调用 `kfree`（L4 级操作）
- `object_destroy` 延迟到 HT lock 释放后执行

### 5.2 Handle Transfer 的锁序

`channel_send` 传递 handle 时需要同时操作发送方的 HT 和接收方的 HT：

```text
1. 锁发送方 HT (L1)，验证 channel handle 和要传递的 handle
2. 对每个要传递的 handle：refcount_inc（原子，不需要接收方 HT 锁）
3. 解锁发送方 HT
4. 锁接收方 HT，分配新槽位，写入 entries
5. 解锁接收方 HT
```

两阶段锁分离：发送方和接收方的 HT 不同时加锁。通过 refcount_inc 在解锁前"保留"对象引用，确保接收方分配时对象仍有效。

---

## 6. 完整 Syscall 列表

### Core (0x0000)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0000 | `abi_info` | `int64_t abi_info(a20_abi_info_t *out)` | 查询 ABI 版本和能力 |
| 0x0001 | `feature_test` | `int64_t feature_test(uint64_t feature_id)` | 测试 feature 可用性 |

### Handle (0x0100)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0100 | `handle_close` | `int64_t handle_close(a20_handle_t h)` | 关闭 handle |
| 0x0101 | `handle_dup` | `int64_t handle_dup(a20_handle_dup_args_t *args)` | 复制 handle（可降级 rights） |
| 0x0102 | `handle_query` | `int64_t handle_query(a20_handle_t h, a20_handle_info_t *out)` | 查询 handle 信息 |
| 0x0103 | `handle_replace` | `int64_t handle_replace(a20_handle_t h, a20_rights_t rights, a20_handle_t *out)` | 原子替换 handle |
| 0x0104 | `handle_close_many` | `int64_t handle_close_many(const a20_handle_t *hs, uint32_t count)` | 批量关闭 |
| 0x0105 | `handle_seek` | `int64_t handle_seek(a20_handle_t h, a20_off_t *offset, uint32_t whence)` | 设置/查询流偏移 |
| 0x0106 | `handle_transfer` | `int64_t handle_transfer(a20_transfer_args_t *args)` | 零拷贝传输（splice/sendfile/copy_file_range 统一） |
| 0x0107 | `handle_set_meta` | `int64_t handle_set_meta(a20_set_meta_args_t *args)` | 修改文件元数据（chmod/chown/utimes 统一） |
| 0x0108 | `handle_xattr_set` | `int64_t handle_xattr_set(a20_xattr_args_t *args)` | 设置扩展属性 |
| 0x0109 | `handle_xattr_get` | `int64_t handle_xattr_get(a20_xattr_args_t *args)` | 获取扩展属性 |
| 0x010A | `handle_xattr_list` | `int64_t handle_xattr_list(a20_xattr_list_args_t *args)` | 列出扩展属性名 |
| 0x010B | `handle_xattr_remove` | `int64_t handle_xattr_remove(a20_xattr_args_t *args)` | 删除扩展属性 |

### Task / Thread (0x0200)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0200 | `task_exit` | `void task_exit(int32_t code)` | 退出当前 task |
| 0x0201 | `task_spawn` | `int64_t task_spawn(a20_task_spawn_args_t *args)` | 创建新 task |
| 0x0202 | `task_wait` | `int64_t task_wait(a20_handle_t task, a20_flags_t flags, a20_task_status_t *out)` | 等待 task 退出 |
| 0x0203 | `task_kill` | `int64_t task_kill(a20_handle_t task, int32_t reason)` | 终止 task |
| 0x0204 | `task_info` | `int64_t task_info(a20_handle_t task, a20_task_info_t *out)` | 查询 task 信息 |
| 0x0205 | `thread_create` | `int64_t thread_create(a20_thread_create_args_t *args)` | 创建线程 |
| 0x0206 | `thread_exit` | `void thread_exit(int32_t code)` | 退出当前线程 |
| 0x0207 | `thread_sleep` | `int64_t thread_sleep(a20_time_ns_t deadline)` | 线程睡眠 |
| 0x0208 | `thread_yield` | `int64_t thread_yield(void)` | 主动让出 CPU |
| 0x0209 | `task_set_sched` | `int64_t task_set_sched(a20_sched_args_t *args)` | 设置调度参数（优先级/策略/亲和性） |
| 0x020A | `task_get_sched` | `int64_t task_get_sched(a20_sched_args_t *args)` | 查询调度参数 |
| 0x020B | `task_get_limits` | `int64_t task_get_limits(a20_rlimit_args_t *args)` | 查询资源限制 |
| 0x020C | `task_set_limits` | `int64_t task_set_limits(a20_rlimit_args_t *args)` | 设置资源限制 |
| 0x020D | `task_get_usage` | `int64_t task_get_usage(a20_handle_t task, a20_rusage_t *out)` | 查询资源使用量 |

### Memory (0x0300)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0300 | `vm_alloc` | `int64_t vm_alloc(a20_vm_alloc_args_t *args)` | 分配匿名内存 |
| 0x0301 | `vm_unmap` | `int64_t vm_unmap(uint64_t addr, uint64_t len)` | 解除映射 |
| 0x0302 | `vm_protect` | `int64_t vm_protect(uint64_t addr, uint64_t len, uint32_t prot)` | 修改保护 |
| 0x0303 | `vm_map` | `int64_t vm_map(a20_vm_map_args_t *args)` | 映射对象到地址空间 |
| 0x0304 | `vm_share` | `int64_t vm_share(a20_vm_share_args_t *args)` | 导出内存为共享对象 |
| 0x0305 | `vm_flush` | `int64_t vm_flush(uint64_t addr, uint64_t len, uint32_t flags)` | 刷新内存 |
| 0x0306 | `vm_advise` | `int64_t vm_advise(uint64_t addr, uint64_t len, uint32_t advice)` | 内存使用建议（madvise） |
| 0x0307 | `vm_remap` | `int64_t vm_remap(a20_vm_remap_args_t *args)` | 重映射虚拟内存（mremap） |
| 0x0308 | `vm_lock` | `int64_t vm_lock(uint64_t addr, uint64_t len, uint32_t flags)` | 锁定/解锁物理页面 |
| 0x0309 | `vm_create_object` | `int64_t vm_create_object(a20_vm_object_args_t *args)` | 创建匿名内存对象（memfd） |

### Path / Filesystem (0x0400)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0400 | `path_open` | `int64_t path_open(a20_path_open_args_t *args)` | 打开路径 |
| 0x0401 | `handle_read` | `int64_t handle_read(a20_io_args_t *args)` | 读取 |
| 0x0402 | `handle_write` | `int64_t handle_write(a20_io_args_t *args)` | 写入 |
| 0x0403 | `handle_stat` | `int64_t handle_stat(a20_handle_t h, a20_stat_t *out)` | 查询属性 |
| 0x0404 | `path_create` | `int64_t path_create(a20_path_create_args_t *args)` | 创建节点 |
| 0x0405 | `path_unlink` | `int64_t path_unlink(a20_handle_t dir, const char *path, uint32_t flags)` | 删除节点 |
| 0x0406 | `path_rename` | `int64_t path_rename(...)` | 重命名 |
| 0x0407 | `handle_control` | `int64_t handle_control(a20_control_args_t *args)` | 对象控制 |
| 0x0408 | `path_readdir` | `int64_t path_readdir(a20_handle_t dir, a20_readdir_args_t *args)` | 目录列举 |
| 0x0409 | `path_link` | `int64_t path_link(a20_path_link_args_t *args)` | 创建硬链接 |
| 0x040A | `path_symlink` | `int64_t path_symlink(a20_path_symlink_args_t *args)` | 创建符号链接 |
| 0x040B | `path_readlink` | `int64_t path_readlink(a20_path_readlink_args_t *args)` | 读取符号链接目标 |
| 0x040C | `path_resolve` | `int64_t path_resolve(a20_path_resolve_args_t *args)` | 解析路径（access/test/readlink 统一） |
| 0x040D | `fs_stat` | `int64_t fs_stat(a20_handle_t dir, const char *path, a20_fs_stat_t *out)` | 文件系统统计（statfs） |
| 0x040E | `fs_mount` | `int64_t fs_mount(a20_fs_mount_args_t *args)` | 挂载文件系统 |
| 0x040F | `fs_umount` | `int64_t fs_umount(a20_handle_t mount_point, uint32_t flags)` | 卸载文件系统 |
| 0x0410 | `fs_sync` | `int64_t fs_sync(a20_handle_t handle, uint32_t flags)` | 同步文件系统到磁盘 |

### Event / IPC (0x0500)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0500 | `event_queue_create` | `int64_t event_queue_create(a20_event_queue_create_args_t *args)` | 创建事件队列 |
| 0x0501 | `event_watch` | `int64_t event_watch(a20_event_watch_args_t *args)` | 注册事件关注 |
| 0x0502 | `event_wait` | `int64_t event_wait(a20_event_wait_args_t *args)` | 等待事件 |
| 0x0503 | `event_cancel` | `int64_t event_cancel(a20_handle_t queue, a20_handle_t target)` | 取消关注 |
| 0x0504 | `channel_create` | `int64_t channel_create(a20_channel_create_args_t *args)` | 创建消息通道 |
| 0x0505 | `channel_send` | `int64_t channel_send(a20_msg_send_args_t *args)` | 发送消息 |
| 0x0506 | `channel_recv` | `int64_t channel_recv(a20_msg_recv_args_t *args)` | 接收消息 |
| 0x0507 | `event_watch_fs` | `int64_t event_watch_fs(a20_event_watch_fs_args_t *args)` | 注册文件系统变更通知（inotify 等价） |

### Network (0x0600)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0600 | `net_socket` | `int64_t net_socket(a20_net_socket_args_t *args)` | 创建 socket |
| 0x0601 | `net_bind` | `int64_t net_bind(a20_handle_t sock, const a20_net_addr_t *addr)` | 绑定 |
| 0x0602 | `net_connect` | `int64_t net_connect(a20_handle_t sock, const a20_net_addr_t *addr)` | 连接 |
| 0x0603 | `net_accept` | `int64_t net_accept(a20_handle_t listener, a20_flags_t flags, a20_handle_t *out)` | 接受连接 |
| 0x0604 | `net_listen` | `int64_t net_listen(a20_handle_t sock, uint32_t backlog)` | 监听 |
| 0x0605 | `net_sendmsg` | `int64_t net_sendmsg(a20_net_sendmsg_args_t *args)` | 发送消息 |
| 0x0606 | `net_recvmsg` | `int64_t net_recvmsg(a20_net_recvmsg_args_t *args)` | 接收消息 |
| 0x0607 | `net_socketpair` | `int64_t net_socketpair(a20_net_socketpair_args_t *args)` | 创建已连接的套接字对 |
| 0x0608 | `net_getname` | `int64_t net_getname(a20_handle_t sock, uint32_t flags, a20_net_addr_t *out)` | 获取套接字本地/对端地址 |
| 0x0609 | `net_shutdown` | `int64_t net_shutdown(a20_handle_t sock, uint32_t how)` | 关闭套接字读写方向 |

### Time (0x0700)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0700 | `clock_get` | `int64_t clock_get(uint32_t clock_id, a20_time_ns_t *out)` | 获取时钟 |
| 0x0701 | `timer_create` | `int64_t timer_create(a20_timer_create_args_t *args)` | 创建定时器 |
| 0x0702 | `timer_set` | `int64_t timer_set(a20_handle_t timer, uint64_t deadline_ns, uint64_t interval_ns)` | 设置定时器 |
| 0x0703 | `timer_cancel` | `int64_t timer_cancel(a20_handle_t timer)` | 取消定时器 |
| 0x0704 | `clock_set` | `int64_t clock_set(uint32_t clock_id, a20_time_ns_t value)` | 设置时钟（需 CAP_CLOCK_SET） |
| 0x0705 | `clock_resolution` | `int64_t clock_resolution(uint32_t clock_id, a20_time_ns_t *out)` | 查询时钟分辨率 |

### Security (0x0800)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0800 | `ns_create` | `int64_t ns_create(uint32_t ns_type, a20_flags_t flags, a20_handle_t *out)` | 创建 namespace |
| 0x0801 | `ns_apply` | `int64_t ns_apply(a20_handle_t ns, a20_handle_t target)` | 应用 namespace |
| 0x0802 | `security_get_context` | `int64_t security_get_context(a20_security_context_t *out)` | 查询安全上下文（身份/能力） |
| 0x0803 | `security_set_context` | `int64_t security_set_context(const a20_security_context_t *in)` | 修改安全上下文 |

### Debug (0x0900)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0900 | `debug_attach` | `int64_t debug_attach(a20_handle_t task)` | 附加调试 |
| 0x0901 | `debug_read_regs` | `int64_t debug_read_regs(a20_handle_t thread, a20_regs_t *out)` | 读寄存器 |
| 0x0902 | `debug_write_regs` | `int64_t debug_write_regs(a20_handle_t thread, const a20_regs_t *in)` | 写寄存器 |
| 0x0903 | `debug_map_memory` | `int64_t debug_map_memory(a20_handle_t task, a20_handle_t *out)` | 映射目标内存 |

### System (0x0A00)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0A00 | `system_info` | `int64_t system_info(a20_system_info_t *out)` | 系统信息（uname + sysinfo 统一） |
| 0x0A01 | `system_random` | `int64_t system_random(void *buf, uint64_t len, uint32_t flags)` | 获取密码学安全随机字节 |
| 0x0A02 | `system_reboot` | `int64_t system_reboot(uint32_t cmd, uint64_t arg)` | 重启/关机/电源控制 |

**总计：90 个 syscall。**
