# A20OS Native API 设计

> 本文为 A20OS Native ABI 理论研究的第二部分。基于 [01-posix-limitations.md](01-posix-limitations.md) 中的 POSIX 问题分析和现有替代方案研究，针对 A20OS 项目特点设计具体的 Native API。

## 目录

1. [设计目标与约束](#1-设计目标与约束)
2. [与现有 DESIGN.md 的关系](#2-与-existing-designmd-的关系)
3. [核心设计决策分析](#3-核心设计决策分析)
4. [API 分类设计](#4-api-分类设计)
5. [补充接口设计](#5-补充接口设计)
6. [POSIX 兼容层设计](#6-posix-兼容层设计)
7. [Native libc 设计](#7-native-libc-设计)
8. [安全性考量](#8-安全性考量)
9. [设计决策总结表](#9-设计决策总结表)
10. [内存模型形式化](#10-内存模型形式化)
11. [Handle 生命周期状态机](#11-handle-生命周期状态机)
12. [Rights 代数与完备性分析](#12-rights-代数与完备性分析)

---

## 1. 设计目标与约束

### 1.1 目标

| 优先级 | 目标 | 理由 |
|--------|------|------|
| P0 | 统一资源模型 | 消除 POSIX fd/pid/tid/shmid 的碎片化 |
| P0 | Capability-based 权限 | 支持 sandbox 和最小权限 |
| P1 | 可扩展的 ABI 演进 | 结构体版本化，避免 Linux 的 ioctl 补丁 |
| P1 | 异步事件统一等待 | 替代 epoll/signalfd/timerfd 的组合 |
| P2 | 清晰的进程创建模型 | 替代 fork+exec 的组合 |
| P2 | 显式 IPC 和 handle transfer | 替代 SCM_RIGHTS 和 SysV IPC |
| P3 | POSIX 兼容层 | 允许渐进式迁移 |

### 1.2 约束

- **内核已有 Linux ABI**：`kernel/abi/linux/` 是主 ABI，不能影响其运行
- **内核核心模块是共享的**：mm、proc、vfs、net 是两套 ABI 的共同基础
- **人力有限**：教学/竞赛内核项目，设计必须精简，优先可实现的子集
- **三架构支持**：aarch64、loongarch64、riscv64——ABI 必须架构无关

---

## 2. 与现有 DESIGN.md 的关系

`docs/native-abi/00-overview.md` 已定义了良好的骨架：

- Handle 作为统一资源引用 ✓
- Handle rights (capability) 系统 ✓
- 版本化结构体参数 ✓
- ABI 版本协商 ✓
- Syscall 编号分区 ✓
- 错误模型 ✓
- 启动协议 ✓

本文档在此基础上，**补充以下内容**：

1. 对每个设计决策进行论证（为什么这样设计，而不是其他方式）
2. 补充 DESIGN.md 中未覆盖的接口细节
3. 设计 POSIX 兼容层
4. 设计 native libc 的分层结构
5. 给出完整的 syscall 编号和接口规范

---

## 3. 核心设计决策分析

### 3.1 Handle vs File Descriptor

**选择：Handle（uint32_t，进程本地编号）**

| 方案 | 优势 | 劣势 |
|------|------|------|
| POSIX fd (int) | 兼容现有代码 | 无权限信息，全局状态多 |
| Zircon handle (uint32_t) | 统一、带 rights | 需要全新的 libc |
| seL4 CPtr (cap slot) | 最小权限 | 管理复杂 |
| Redox scheme fd | URL 寻址 | 依赖用户态 scheme |

**理由**：Handle 是 fd 的超集。fd 可以视为 handle 的子集（无 rights 的 handle）。通过在 libc 层维护 `fd -> handle` 映射表，可以同时支持两种模型。

### 3.2 Rights 模型设计

**选择：64-bit 位域 rights，仅支持降级**

参考 Zircon 的 `zx_rights_t` 设计，但简化：

```c
// 基础权限（适用于所有 handle 类型）
#define A20_RIGHT_READ       (1ull << 0)   // 读取数据
#define A20_RIGHT_WRITE      (1ull << 1)   // 写入数据
#define A20_RIGHT_EXEC       (1ull << 2)   // 执行（内存映射）
#define A20_RIGHT_MAP        (1ull << 3)   // 映射到地址空间
#define A20_RIGHT_STAT       (1ull << 4)   // 查询属性
#define A20_RIGHT_CONTROL    (1ull << 5)   // 对象特定控制操作

// 派生权限（控制 handle 自身操作）
#define A20_RIGHT_DUP        (1ull << 8)   // 复制 handle
#define A20_RIGHT_TRANSFER   (1ull << 9)   // 通过 IPC 传递
#define A20_RIGHT_WAIT       (1ull << 10)  // 等待对象事件

// 对象特定权限
#define A20_RIGHT_CONNECT    (1ull << 16)  // 发起连接（socket）
#define A20_RIGHT_ACCEPT     (1ull << 17)  // 接受连接（socket）
#define A20_RIGHT_SIGNAL     (1ull << 18)  // 发送信号（task/event）
#define A20_RIGHT_SEEK       (1ull << 19)  // 修改位置
#define A20_RIGHT_ADMIN      (1ull << 20)  // 管理操作
```

**降级规则**：
- `handle_dup` 产生的新 handle 的 rights 必须是原 handle rights 的子集
- `handle_transfer` 传递后接收方获得的 rights 必须是发送方 rights 的子集
- 不存在任何方式升级 rights

### 3.3 Syscall 参数传递

**选择：结构体指针，以 `{size, version}` 开头**

```c
typedef struct a20_abi_header {
    uint32_t size;     // 结构体实际大小（字节）
    uint32_t version;  // 结构体版本号
} a20_abi_header_t;
```

**演进规则**（与 DESIGN.md 一致）：
- `size < 内核支持` → 缺失字段视为 0
- `size > 内核支持` → 内核只读已知字段
- 新字段只能追加
- 保留 flag 位必须为 0

**为什么不用 Zircon 的直接参数方式？**

Zircon 大部分 syscall 直接传参数，只有少数用结构体。但 Zircon 有 vDSO 做中间层可以重新映射。A20OS 没有这个基础设施，结构体方式更安全、更易扩展。

### 3.4 事件模型

**选择：Event Queue（统一等待机制）**

```
event_queue_create()  → 创建事件队列 handle
event_watch(queue, target, events, user_data) → 注册关注
event_wait(queue, ..., timeout) → 等待事件
```

**替代方案比较**：

| 方案 | 优势 | 劣势 |
|------|------|------|
| epoll | Linux 标准 | 只能等 fd，需要 signalfd/timerfd/eventfd 变通 |
| kqueue | 更统一 | BSD 特有，非标准 |
| io_uring | 最高性能 | 实现复杂，需要共享内存 ring buffer |
| Zircon port | 统一 handle 等待 | 需要整个 handle 系统就绪 |
| **Event Queue** | 统一、简单、可扩展 | 性能不如 io_uring |

**Event Queue 的关键设计**：

1. 所有可观察对象（file、socket、timer、task、channel）都可被 watch
2. 事件类型由对象类型决定：
   - File: READABLE, WRITABLE, ERROR, CLOSED
   - Socket: 上述 + CONNECTION, ACCEPT_READY
   - Timer: EXPIRED
   - Task: EXITED
   - Channel: MESSAGE_READY
3. user_data 字段允许调用者关联上下文，避免额外的查找表
4. `event_wait` 返回事件数组，支持批量处理

### 3.5 进程创建模型

**选择：spawn 模型（显式构造新进程）**

```c
task_spawn({
    .image = executable_handle,
    .root_dir = root_handle,
    .cwd_dir = cwd_handle,
    .argv = ...,
    .envp = ...,
    .handles = {handle_array_with_rights},
    .task_rights = initial_rights_mask,
});
```

**替代方案比较**：

| 方案 | 优势 | 劣势 |
|------|------|------|
| fork + exec | 简单、灵活 | 不安全、不线程安全、不可组合 |
| posix_spawn | 标准化 | 不完整，错误报告差 |
| **spawn** | 显式、安全、支持 sandbox | 不如 fork 灵活（但灵活性可用 IPC 补偿） |
| Zircon process_create + thread_start | 最小权限 | 需要多步操作，复杂 |

**关键创新**：handles 数组允许显式传递资源给新进程，每个 handle 可指定降级后的 rights。这实现了 fork 无法做到的最小权限进程创建。

---

## 4. API 分类设计

### 4.1 Syscall 编号方案（扩展 DESIGN.md）

```
Range         Category            Status
─────────────────────────────────────────────
0x0000-0x00FF  Core / ABI         Stable
0x0100-0x01FF  Handle             Stable
0x0200-0x02FF  Task / Thread      Stable
0x0300-0x03FF  Memory             Stable
0x0400-0x04FF  Path / Filesystem  Stable
0x0500-0x05FF  Event / IPC        Stable
0x0600-0x06FF  Network            Stable
0x0700-0x07FF  Time               Stable
0x0800-0x08FF  Security           Stable
0x0900-0x09FF  Debug / Trace      Stable
0x0A00-0x0FFF  Reserved           —
0x1000-0x1FFF  Experimental       Unstable
```

### 4.2 完整 Syscall 列表

#### Core (0x0000)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0000 | `abi_info` | `int64_t abi_info(a20_abi_info_t *out)` | 查询 ABI 版本和能力 |
| 0x0001 | `feature_test` | `int64_t feature_test(uint64_t feature_id)` | 测试特定 feature 是否可用 |

#### Handle (0x0100)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0100 | `handle_close` | `int64_t handle_close(a20_handle_t h)` | 关闭 handle |
| 0x0101 | `handle_dup` | `int64_t handle_dup(a20_handle_dup_args_t *args)` | 复制 handle（rights 可降级） |
| 0x0102 | `handle_query` | `int64_t handle_query(a20_handle_t h, a20_handle_info_t *out)` | 查询 handle 类型和 rights |
| 0x0103 | `handle_replace` | `int64_t handle_replace(a20_handle_t h, a20_rights_t rights, a20_handle_t *out)` | 替换 handle（原 handle 失效） |
| 0x0104 | `handle_close_many` | `int64_t handle_close_many(const a20_handle_t *handles, uint32_t count)` | 批量关闭 |

#### Task / Thread (0x0200)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0200 | `task_exit` | `void task_exit(int32_t code)` | 退出当前 task |
| 0x0201 | `task_spawn` | `int64_t task_spawn(a20_task_spawn_args_t *args)` | 创建新 task |
| 0x0202 | `task_wait` | `int64_t task_wait(a20_handle_t task, a20_flags_t flags, a20_task_status_t *out)` | 等待 task 退出 |
| 0x0203 | `task_kill` | `int64_t task_kill(a20_handle_t task, int32_t reason)` | 终止 task（需 A20_RIGHT_SIGNAL） |
| 0x0204 | `task_info` | `int64_t task_info(a20_handle_t task, a20_task_info_t *out)` | 查询 task 信息 |
| 0x0205 | `thread_create` | `int64_t thread_create(a20_thread_create_args_t *args)` | 创建线程 |
| 0x0206 | `thread_exit` | `void thread_exit(int32_t code)` | 退出当前线程 |
| 0x0207 | `thread_sleep` | `int64_t thread_sleep(a20_time_ns_t deadline)` | 线程睡眠到指定时间 |

#### Memory (0x0300)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0300 | `vm_alloc` | `int64_t vm_alloc(a20_vm_alloc_args_t *args)` | 分配匿名内存 |
| 0x0301 | `vm_unmap` | `int64_t vm_unmap(uint64_t addr, uint64_t len)` | 解除映射 |
| 0x0302 | `vm_protect` | `int64_t vm_protect(uint64_t addr, uint64_t len, uint32_t prot)` | 修改内存保护 |
| 0x0303 | `vm_map` | `int64_t vm_map(a20_vm_map_args_t *args)` | 映射对象到地址空间 |
| 0x0304 | `vm_share` | `int64_t vm_share(a20_vm_share_args_t *args)` | 导出内存为可共享对象 |
| 0x0305 | `vm_flush` | `int64_t vm_flush(uint64_t addr, uint64_t len, uint32_t flags)` | 刷新内存（sync/clean/invalidate） |

#### Path / Filesystem (0x0400)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0400 | `path_open` | `int64_t path_open(a20_path_open_args_t *args)` | 打开路径 |
| 0x0401 | `handle_read` | `int64_t handle_read(a20_io_args_t *args)` | 读取 |
| 0x0402 | `handle_write` | `int64_t handle_write(a20_io_args_t *args)` | 写入 |
| 0x0403 | `handle_stat` | `int64_t handle_stat(a20_handle_t h, a20_stat_t *out)` | 查询文件属性 |
| 0x0404 | `path_create` | `int64_t path_create(a20_path_create_args_t *args)` | 创建文件系统节点 |
| 0x0405 | `path_unlink` | `int64_t path_unlink(a20_handle_t dir, const char *path, uint32_t flags)` | 删除节点 |
| 0x0406 | `path_rename` | `int64_t path_rename(a20_handle_t old_dir, const char *old_path, a20_handle_t new_dir, const char *new_path)` | 重命名 |
| 0x0407 | `handle_control` | `int64_t handle_control(a20_control_args_t *args)` | 对象特定控制（替代 ioctl） |
| 0x0408 | `path_readdir` | `int64_t path_readdir(a20_handle_t dir, a20_readdir_args_t *args)` | 目录列举 |

#### Event / IPC (0x0500)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0500 | `event_queue_create` | `int64_t event_queue_create(a20_event_queue_create_args_t *args)` | 创建事件队列 |
| 0x0501 | `event_watch` | `int64_t event_watch(a20_event_watch_args_t *args)` | 注册事件关注 |
| 0x0502 | `event_wait` | `int64_t event_wait(a20_event_wait_args_t *args)` | 等待事件 |
| 0x0503 | `event_cancel` | `int64_t event_cancel(a20_handle_t queue, a20_handle_t target)` | 取消关注 |
| 0x0504 | `channel_create` | `int64_t channel_create(a20_channel_create_args_t *args)` | 创建消息通道 |
| 0x0505 | `channel_send` | `int64_t channel_send(a20_msg_send_args_t *args)` | 发送消息（可附带 handle） |
| 0x0506 | `channel_recv` | `int64_t channel_recv(a20_msg_recv_args_t *args)` | 接收消息 |

#### Network (0x0600)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0600 | `net_socket` | `int64_t net_socket(a20_net_socket_args_t *args)` | 创建 socket |
| 0x0601 | `net_bind` | `int64_t net_bind(a20_handle_t sock, const a20_net_addr_t *addr)` | 绑定地址 |
| 0x0602 | `net_connect` | `int64_t net_connect(a20_handle_t sock, const a20_net_addr_t *addr)` | 发起连接 |
| 0x0603 | `net_accept` | `int64_t net_accept(a20_handle_t listener, a20_flags_t flags, a20_handle_t *out)` | 接受连接 |
| 0x0604 | `net_listen` | `int64_t net_listen(a20_handle_t sock, uint32_t backlog)` | 开始监听 |
| 0x0605 | `net_sendmsg` | `int64_t net_sendmsg(a20_net_sendmsg_args_t *args)` | 发送消息（datagram） |
| 0x0606 | `net_recvmsg` | `int64_t net_recvmsg(a20_net_recvmsg_args_t *args)` | 接收消息（datagram） |

#### Time (0x0700)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0700 | `clock_get` | `int64_t clock_get(uint32_t clock_id, a20_time_ns_t *out)` | 获取时钟 |
| 0x0701 | `timer_create` | `int64_t timer_create(a20_timer_create_args_t *args)` | 创建定时器 handle |
| 0x0702 | `timer_set` | `int64_t timer_set(a20_handle_t timer, uint64_t deadline_ns, uint64_t interval_ns)` | 设置定时器 |
| 0x0703 | `timer_cancel` | `int64_t timer_cancel(a20_handle_t timer)` | 取消定时器 |

#### Security (0x0800)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0800 | `ns_create` | `int64_t ns_create(uint32_t ns_type, a20_flags_t flags, a20_handle_t *out)` | 创建 namespace |
| 0x0801 | `ns_apply` | `int64_t ns_apply(a20_handle_t ns, a20_handle_t target)` | 应用 namespace |

#### Debug (0x0900)

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 0x0900 | `debug_attach` | `int64_t debug_attach(a20_handle_t task)` | 附加调试 |
| 0x0901 | `debug_read_regs` | `int64_t debug_read_regs(a20_handle_t thread, void *buf, uint32_t size)` | 读取寄存器 |
| 0x0902 | `debug_write_regs` | `int64_t debug_write_regs(a20_handle_t thread, const void *buf, uint32_t size)` | 写入寄存器 |

### 4.3 总 syscall 数

| 分类 | 数量 |
|------|------|
| Core | 2 |
| Handle | 5 |
| Task/Thread | 8 |
| Memory | 6 |
| Path/Fs | 9 |
| Event/IPC | 7 |
| Network | 7 |
| Time | 4 |
| Security | 2 |
| Debug | 3 |
| **总计** | **53** |

相比之下，Linux 有约 400 个 syscall，Zircon 约 150 个。53 个 syscall 覆盖了核心功能，同时保持了可管理性。

---

## 5. 补充接口设计

### 5.1 Channel IPC（补充 DESIGN.md 的消息通道）

Channel 是 Native ABI 的核心 IPC 机制，设计参考 Zircon Channel 但简化。

```c
// 创建双向通道，返回两个 endpoint handle
typedef struct a20_channel_create_args {
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t msg_size_hint;    // 建议的最大消息大小
    a20_handle_t out_endpoints[2];
} a20_channel_create_args_t;
```

**关键特性**：
- 消息原子性：要么完整发送，要么失败
- Handle 随消息传递：采用**共享语义**——发送方**保留**原 handle，接收方获得新 handle，对象的引用计数增加（与 Zircon Channel 语义一致）
- 消息大小有上限（建议 64KB）
- Channel endpoint 可 dup 和 transfer
- 关闭一端时，对端的 peer-closed 事件触发

> **设计选择说明**：A20OS 采用共享语义而非移动语义（send 后移除发送方 handle），原因如下：(1) 共享语义更简单——不需要从发送方 handle table 中移除条目，避免了两把 HT 锁同时持有的死锁风险；(2) 共享语义更安全——发送方可以在 recv 确认后再决定是否 close 原始 handle；(3) Zircon 的 zx_channel_write 也使用共享语义，这是工业实践验证过的设计。

### 5.2 Handle Transfer 协议

```
Process A                              Process B
   |                                      |
   |-- channel_send(ch, data, h1) ------>|
   |     (h1 保留在 A 的 HT 中)           |
   |     (h1 指向对象的 refcount += 1)    |
   |                                      |
   |<-- channel_recv(ch, data, &h2) -----|
   |     (B 获得 h2，rights <= h1's rights)
   |     (h2 是 B HT 中的新条目)
```

**规则**：
1. 传递的 handle 必须有 `A20_RIGHT_TRANSFER`
2. 接收方获得的 rights 是发送方 rights 的子集（由内核强制）
3. 消息中的所有 handle 要么全部传递，要么全部丢弃（原子性）
4. 发送方保留原 handle，对象的引用计数增加（共享语义）
5. 目标进程关闭后，未接收的消息中的 handle 引用被释放（refcount 减至正确值）

### 5.3 路径解析规则

Native ABI 的路径操作使用基于 handle 的相对路径：

```c
// 以 dir_handle 为根解析 path
int64_t path_open({
    .dir = root_dir_handle,    // 基准目录
    .path = "etc/config.txt",  // 相对路径
    .flags = A20_PATH_OPEN_FOLLOW,  // 跟随符号链接
    ...
});
```

**与 POSIX 的区别**：
- 无全局根 `/`——根目录是进程启动时获得的 root handle
- 路径穿越 `..` 受限于 dir handle 的边界
- 绝对路径需要通过 root_dir handle 解析
- 这天然支持 chroot 风格的隔离，无需额外机制

---

## 6. POSIX 兼容层设计

### 6.1 分层架构

```
┌─────────────────────────────────────────────┐
│             POSIX Application               │
│         (uses open/read/write/pid)          │
├─────────────────────────────────────────────┤
│           POSIX Shim (liba20posix)          │
│  fd ↔ handle mapping, pid ↔ task handle    │
│  errno emulation, signal emulation          │
├─────────────────────────────────────────────┤
│            liba20c (C library)              │
│   malloc, printf, string, stdio            │
├─────────────────────────────────────────────┤
│           liba20rt (runtime)                │
│   syscall wrappers, startup code           │
├─────────────────────────────────────────────┤
│              Kernel (Native ABI)            │
│         handle/capability based             │
└─────────────────────────────────────────────┘
```

### 6.2 关键映射

| POSIX 概念 | Native 概念 | 映射策略 |
|-----------|------------|---------|
| `int fd` | `a20_handle_t` | 进程本地 fd → handle 映射表（数组或哈希表） |
| `pid_t pid` | `a20_handle_t task` | libc 维护 `pid → task_handle` 映射 |
| `errno` | `int64_t return` | 负数返回值转 errno |
| `FILE *` | `a20_handle_t + buffer` | stdio 在 handle 上构建缓冲层 |
| `DIR *` | `a20_handle_t` | readdir 通过 handle_read 实现 |
| `signal` | `event_wait + task_signal` | 信号模拟为事件；libc 在专用线程中等待 |
| `fork()` | `task_spawn()` | **不支持**，链接时错误或返回 ENOSYS |
| `exec()` | `task_spawn()` | 重新映射为带 exec 语义的 spawn |
| `mmap(fd)` | `vm_map(handle)` | fd 先转 handle，再映射 |
| `socket()` | `net_socket()` | socket handle 享有 A20_RIGHT_READ/WRITE |
| `epoll` | `event_queue` | 一个 epoll fd 对应一个 event queue handle |
| `pipe()` | `channel_create()` | 管道两端是 channel endpoints |
| `SCM_RIGHTS` | `channel_send(handles)` | 直接传递 handles |

### 6.3 不支持的 POSIX 特性

以下 POSIX 特性在 Native ABI 中不提供兼容：

| 特性 | 原因 | 替代方案 |
|------|------|---------|
| `fork()` | 不安全、不线程安全 | `task_spawn()` |
| `signal()` / `sigaction()` | 异步不安全 | event_wait + task_kill |
| `ioctl()` | 无类型、混乱 | `handle_control()` |
| SysV IPC (`shmget`/`semget`/`msgget`) | 全局 ID、权限模型差 | channel + vm_share |
| `ptrace()` | 不安全 | debug 接口 |
| Terminal API (termios) | 过于复杂 | 用户态终端模拟器 |

---

## 7. Native libc 设计

### 7.1 三层设计

```
┌───────────────────────────────────────┐
│  liba20posix (可选 POSIX 兼容层)       │
│  - open/close/read/write (fd based)   │
│  - getpid/kill/waitpid                │
│  - signal stubs                       │
│  - dirent, stat                       │
├───────────────────────────────────────┤
│  liba20c (最小 C 库)                   │
│  - malloc/free (基于 vm_alloc)        │
│  - printf/snprintf                    │
│  - string (memcpy, strlen, ...)       │
│  - stdio (FILE* → handle buffering)   │
│  - time (clock_gettime, ...)          │
│  - thread (pthread minimal subset)    │
│  - errno (thread-local)               │
├───────────────────────────────────────┤
│  liba20rt (系统运行时)                  │
│  - _start 入口点                       │
│  - syscall wrappers (inline asm)      │
│  - TLS 设置                            │
│  - handle table 管理                   │
└───────────────────────────────────────┘
```

### 7.2 liba20rt 设计

```c
// liba20rt/start.S — 程序入口点
// 内核将 a20_start_info_t 放在约定位置
// _start 初始化 TLS、handle table、argc/argv，然后调用 main()

// liba20rt/syscall.h — syscall wrapper
static inline int64_t a20_syscall0(uint64_t nr);
static inline int64_t a20_syscall1(uint64_t nr, uint64_t a0);
static inline int64_t a20_syscall2(uint64_t nr, uint64_t a0, uint64_t a1);
static inline int64_t a20_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2);

// liba20rt/handle_table.h — handle 管理
// 维护进程的 handle 表（简单数组或 bitmap）
int64_t a20_handle_slot_alloc(void);
void a20_handle_slot_free(uint32_t slot);
```

### 7.3 liba20c 设计

**内存分配器**：
```c
// 基于 vm_alloc 的 sbrk 替代
// 大块分配用 vm_alloc
// 小块用 slab/arena 分配器
void *malloc(size_t size);
void free(void *ptr);
```

**stdio 实现**：
```c
// FILE 结构体包装 handle
typedef struct A20_FILE {
    a20_handle_t handle;
    int flags;            // O_RDONLY etc
    uint8_t *buffer;      // 用户态缓冲
    size_t buf_size;
    size_t buf_pos;
    // ...
} A20_FILE;

// stdin/stdout/stderr 从 a20_start_info 的 handles 初始化
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
```

### 7.4 liba20posix 设计

**fd ↔ handle 映射**：
```c
// 进程级 fd 表
#define A20_MAX_FDS 1024
static struct {
    a20_handle_t handle;
    int flags;           // O_CLOEXEC etc
    bool used;
} a20_fd_table[A20_MAX_FDS];

// POSIX fd 操作映射
int open(const char *path, int flags, ...) {
    a20_path_open_args_t args = { ... };
    int64_t rc = path_open(&args);
    if (rc < 0) { errno = -rc; return -1; }
    return a20_handle_to_fd(args.out_handle, flags);
}
```

---

## 8. 安全性考量

### 8.1 最小权限原则

Native ABI 通过 rights 系统实现最小权限：

- 新进程通过 `task_spawn` 的 handles 数组显式接收资源，每个 handle 可降级 rights
- 沙箱化只需给进程传递最少必要的 handles
- 不存在 fork 的"继承一切再逐一关闭"的反模式

### 8.2 权限降级不可逆

```
original rights: READ | WRITE | MAP | DUP
    │
    ├── dup(rights=READ | MAP) → handle with READ | MAP only
    │       └── dup(rights=READ) → handle with READ only
    │               └── dup(rights=0) → 拒绝（至少需要一个 right）
    │
    └── dup(rights=READ | WRITE | EXEC) → 拒绝（EXEC 不是子集）
```

### 8.3 Namespace 隔离

- 每个 task 持有 root_dir handle，路径操作以此为界
- 不同 task 可持有不同 root_dir handle，天然实现 chroot 隔离
- Network namespace、pid namespace 可作为显式 handle 传入 task_spawn

---

## 9. 设计决策总结表

| 决策 | 选择 | 替代方案 | 选择理由 |
|------|------|---------|---------|
| 资源模型 | Handle (uint32_t) | fd (int) | 统一所有资源类型，支持 rights |
| 权限模型 | 64-bit rights 位域 | seL4 CSpace / ACL | 简单、高效、足够表达 |
| Syscall 参数 | 版本化结构体 | Zircon 直接参数 | 更好的演进性，无需 vDSO |
| 进程创建 | task_spawn | fork+exec | 安全、显式、支持 sandbox |
| 等待机制 | Event Queue | epoll / io_uring | 统一 handle 等待，实现简单 |
| IPC | Channel + handle transfer | pipe + SCM_RIGHTS | 原子传递、类型安全 |
| 错误返回 | 负数 status_t | errno | 无全局状态、线程安全 |
| 内存管理 | vm_alloc/vm_map | mmap/brk | 统一接口、handle 化 |
| 路径模型 | 基于 handle 的相对路径 | 全局绝对路径 | 天然隔离、无 TOCTOU |
| 兼容策略 | 用户态 shim | 内核兼容 | 不污染内核设计 |

---

## 10. 内存模型形式化

### 10.1 VMO（Virtual Memory Object）抽象

A20OS 的内存管理设计参考 Zircon 的 VMO/VMAR 模型但做了简化。核心思想是将"可映射的内存"显式化为内核对象。

**定义 10.1（VMO）** VMO（Virtual Memory Object）是持有物理页集合的内核对象，定义为一个四元组：

$$VMO = (pages, size, type, \text{phys\_contiguous})$$

其中：
- $pages: \mathbb{N} \to Page$ 是页号到物理页的偏函数（稀疏表示）
- $size \in \mathbb{N}$ 是 VMO 的逻辑大小（字节）
- $type \in \{anonymous, file\_backed, physical, shared\}$ 是 VMO 类型
- $\text{phys\_contiguous} \in \mathbb{B}$ 表示物理连续性

**VMO 操作**：

| 操作 | 对应 syscall | 权限要求 | 语义 |
|------|-------------|---------|------|
| 创建匿名 VMO | `vm_alloc` | — | 分配 size 字节的 VMO，按需填充物理页 |
| 创建文件映射 VMO | `vm_map` | R（读文件）/ W（写回文件） | 创建 VMO 并关联文件 handle |
| 创建共享 VMO | `vm_share` | W + Map | 将地址空间区域导出为 VMO |
| 读取 VMO 内容 | `handle_read` | R | 读取 VMO 内容到用户缓冲区 |
| 写入 VMO 内容 | `handle_write` | W | 将用户缓冲区写入 VMO |
| 映射 VMO 到地址空间 | `vm_map` | Map | 在调用者地址空间创建 VMO 的映射 |

**与 Zircon VMO 的对比**：

| 特性 | Zircon VMO | A20OS VMO |
|------|-----------|-----------|
| 独立对象 | 是（可通过 Channel 传递） | 是（通过 vm_share 导出后成为 handle 持有对象） |
| 子区域 | `zx_vmo_create_child` | 不支持（简化） |
| 物理连续 | `ZX_VMO_CONTIGUOUS` | 通过 vm_share flags 支持 |
| 快照/克隆 | Copy-on-write clone | 不支持（简化） |
| 内存压力通知 | `ZX_VMO_OP_DONT_NEED` | `vm_flush` 的 invalidate flag |

### 10.2 VMAR（Virtual Memory Address Region）抽象

**定义 10.2（VMAR）** VMAR 是进程地址空间的一段连续虚拟地址区间，定义为一个三元组：

$$VMAR = ([base, base + len), prot, mappings)$$

其中：
- $[base, base + len)$ 是虚拟地址区间
- $prot \in 2^{\{R, W, X\}}$ 是默认保护属性
- $mappings: [addr, addr + size) \to (vmo\_handle, offset, prot')$ 是映射子集

**VMAR 层次结构**：每个进程有一个根 VMAR（对应整个用户地址空间），子 VMAR 从中分配。这与 Zircon 的 VMAR 树结构一致，但 A20OS 简化为两层（根 VMAR + 映射区域）。

```
Process Address Space (root VMAR)
├── [0x0000_0000_0000, 0x0000_0100_0000) — unmapped (null page guard)
├── [0x0000_0100_0000, ...) — text segment (R+X)
├── [...] — data segment (R+W)
├── [...] — heap (R+W, growable via vm_alloc)
├── [...] — mmap regions (R+W, from vm_map)
├── [...] — shared memory (from vm_share + vm_map)
└── [0x7fff_xxxx_xxxx, ...) — stack (R+W, fixed size)
```

**关键不变式**：

1. **互不重叠**：$\forall m_1, m_2 \in mappings.\ m_1 \neq m_2 \implies range(m_1) \cap range(m_2) = \emptyset$
2. **权限子集**：映射的保护属性不能超出 VMO handle 的权限：$prot' \subseteq rights\_to\_prot(handle\_rights)$
3. **VMO 引用完整**：每个映射关连的 VMO handle 在映射存在期间保持有效（通过引用计数）

### 10.3 内存操作的统一模型

A20OS 的 6 个内存 syscall 可以通过 VMO/VMAR 模型统一理解：

```
vm_alloc(size) → 创建匿名 VMO + 自动映射到当前 VMAR
vm_map(handle, addr, ...) → 将已有 VMO (handle) 映射到 VMAR
vm_unmap(addr, len) → 解除 VMAR 中的映射
vm_protect(addr, len, prot) → 修改 VMAR 中已有映射的保护属性
vm_share(addr, len, ...) → 将 VMAR 中已有区域导出为 VMO handle
vm_flush(addr, len, flags) → 刷新 VMAR 中映射的缓存状态
```

**与 POSIX mmap 的对比**：

| POSIX mmap | A20OS | 改进 |
|-----------|-------|------|
| `mmap(NULL, len, prot, MAP_ANON|MAP_PRIVATE, -1, 0)` | `vm_alloc({size=len, prot})` | 结构化参数、无 fd 参数歧义 |
| `mmap(addr, len, prot, MAP_SHARED, fd, offset)` | `vm_map({handle=fd_handle, addr, len, prot, offset})` | handle 引用替代 fd、权限检查显式 |
| `munmap(addr, len)` | `vm_unmap(addr, len)` | 语义相同 |
| `mprotect(addr, len, prot)` | `vm_protect(addr, len, prot)` | 语义相同 |
| `shmget + shmat` | `vm_share` + `vm_map` | 无全局 ID、基于 handle 传递 |
| `madvise(addr, len, MADV_DONTNEED)` | `vm_flush(addr, len, INVALIDATE)` | 语义更清晰 |

---

## 11. Handle 生命周期状态机

### 11.1 状态定义

Handle 从创建到销毁经历以下状态：

```
                     ┌─────────────────────────────────┐
                     │                                 │
                     ▼                                 │
  ┌──────┐    ┌──────────┐    ┌───────────┐    ┌──────┴──┐
  │ Free │───→│ Active   │───→│ Transient │───→│ Closing │
  └──────┘    └──────┬───┘    └───────────┘    └─────────┘
     ▲              │               │                │
     │              │               │                ▼
     │              ▼               │         ┌──────────┐
     │        ┌──────────┐          │         │ Released │
     │        │ Duplicated│          │         └──────────┘
     │        └──────────┘          │              │
     │                              │              ▼
     └──────────────────────────────┴──────────────┘
                     (slot recycled)
```

**状态语义**：

| 状态 | 含义 | handle 表条目 | 对象引用 |
|------|------|-------------|---------|
| **Free** | 条目空闲，可分配 | `type = FREE` | 无 |
| **Active** | 正常使用的 handle | `type = 对象类型, rights = ρ` | refcount ≥ 1 |
| **Duplicated** | 刚被 dup 复制，原条目仍在 | 同 Active | refcount 增加后 |
| **Transient** | 正在 channel 传输中 | 原条目保持（共享语义） | refcount += 1（在 send 时） |
| **Closing** | 正在执行 handle_close | 标记为关闭中 | refcount -= 1 进行中 |
| **Released** | 对象 refcount 降为 0，对象被销毁 | 回收 | refcount = 0 |

### 11.2 状态转移规则

$$\frac{p \text{ calls } \text{handle\_dup}(h, \rho')}{\text{Active}(h) \xrightarrow{\text{dup}} \text{Active}(h) \land \text{Active}(h')}$$

$$\frac{p \text{ calls } \text{handle\_close}(h)}{\text{Active}(h) \xrightarrow{\text{close}} \text{Free}(slot)}$$

$$\frac{p \text{ calls } \text{channel\_send}(ch, data, [h_1, \ldots, h_k])}{\forall i.\ \text{Active}(h_i) \xrightarrow{\text{send}} \text{Transient}(h_i) \text{ in sender HT}}$$

$$\frac{q \text{ calls } \text{channel\_recv}(ch, \ldots)}{\text{Transient}(h_i) \text{ (sender)} \land \text{new slot in receiver HT} \xrightarrow{\text{recv}} \text{Active}(h_i') \text{ (receiver)}}$$

**关键不变式**：在共享语义下，send 操作**不改变**发送方 handle 的状态——它仍然是 Active。Transient 状态仅表示"此 handle 关联的引用正在传输通道中"，发送方可以继续正常使用该 handle。

### 11.3 退出时的状态清理

当进程 $p$ 调用 `task_exit` 时，内核遍历 $p$ 的 handle 表，对每个 Active handle 执行 handle_close 语义。此过程保证：

1. **完整性**：所有 Active 条目都被关闭（不存在遗漏）
2. **有序性**：按 handle 编号顺序关闭，避免依赖顺序问题
3. **原子性**：整个过程持有 `ht_lock`，无并发干扰

---

## 12. Rights 代数与完备性分析

### 12.1 Rights 作为格

**定义 12.1（Rights 格）** 权限集合 $\mathcal{R}ights = 2^{\{R,W,X,Stat,Seek,Dup,Transfer,Map,Wait,Connect,Accept,Control,Admin,Signal\}}$ 在子集关系 $\subseteq$ 下构成有限布尔格 $(\mathcal{R}ights, \subseteq, \cap, \cup, \emptyset, \mathcal{R}ights)$。

**格性质**：

| 性质 | 验证 |
|------|------|
| 偏序 | $\subseteq$ 是偏序（自反、反对称、传递） |
| 下界 | $A \cap B$ 是 $A$ 和 $B$ 的最大下界 |
| 上界 | $A \cup B$ 是 $A$ 和 $B$ 的最小上界 |
| 分配律 | $A \cap (B \cup C) = (A \cap B) \cup (A \cap C)$ |
| 补集 | $\overline{A} = \mathcal{R}ights \setminus A$ |

**Rights 降级操作**：`handle_dup` 的 `rights` 参数执行格中的 meet 操作：

$$\rho' = \rho_{orig} \cap \rho_{requested}$$

此操作满足：
- **单调递减**：$\rho' \subseteq \rho_{orig}$（04 定理 3.2）
- **幂等**：$\text{dup}(\text{dup}(h, \rho_1), \rho_2) = \text{dup}(h, \rho_1 \cap \rho_2)$
- **最大下界**：$\rho' = \text{glb}(\rho_{orig}, \rho_{requested})$

### 12.2 类型-Rights 一致性

每种对象类型定义了合法权限集合 $Legal(\tau) \subseteq \mathcal{R}ights$（见 04 §1.2）。Rights 降级操作必须保持类型一致性：

$$\text{dup\_valid}(h, \rho') \iff \rho' \subseteq HT_p(h).rights \land \rho' \subseteq Legal(\tau(HT_p(h).object))$$

**完备性分析**：$Legal(\tau)$ 的定义是否完备？即，是否存在某个合法操作需要但未被包含的权限？

| 对象类型 | 合法操作 | 对应 Rights | 覆盖？ |
|---------|---------|------------|--------|
| file | 读取 | R | ✓ |
| file | 写入 | W | ✓ |
| file | 状态查询 | Stat | ✓ |
| file | 随机访问 | Seek | ✓ |
| file | 复制/传递 | Dup, Transfer | ✓ |
| file | 内存映射 | Map | ✓ |
| file | 控制（fsync, ftruncate） | Control | ✓ |
| task | 等待退出 | Wait | ✓ |
| task | 发信号 | Signal | ✓ |
| task | 管理（设置优先级） | Admin | ✓ |
| channel | 读（recv） | R | ✓ |
| channel | 写（send） | W | ✓ |
| channel | 传递 handle | Transfer（在 handles 上） | ✓ |
| eventq | 读取事件 | R | ✓ |
| eventq | 注册/注销 | Control | ✓ |
| timer | 设置/取消 | Control | ✓ |
| shm | 映射 | Map | ✓ |
| shm | 读写 | R, W | ✓ |

**结论**：14 个权限位覆盖了全部 13 种对象类型的所有合法操作。每种类型只有其合法权限位的子集有意义，其他位在 handle_dup 时被自动忽略（内核不设置非法位）。

### 12.3 Rights 传递的完备性

当 handle 通过 channel 传递时，接收方获得的权限 $\rho_{recv}$ 满足：

$$\rho_{recv} = \rho_{send} \cap \rho_{transfer}$$

其中 $\rho_{send}$ 是发送方对原始 handle 的权限，$\rho_{transfer}$ 是发送方在 send 调用中指定的权限（可选，若未指定则为 $\rho_{send}$）。

**完备性论证**：

1. **降级保证**：$\rho_{recv} \subseteq \rho_{send}$（由格的 meet 性质）
2. **类型一致**：$\rho_{recv} \subseteq Legal(\tau(o))$（由 $\rho_{send} \subseteq Legal(\tau(o))$ 和 $\cap$ 的封闭性）
3. **传递传递性**：若 A 以 $\rho_A$ 传给 B，B 再以 $\rho_B$ 传给 C，则 C 的权限为 $\rho_A \cap \rho_B \subseteq \rho_A$——传递链中的权限只降不升

此三性质共同构成 A20OS 的权限安全三角：**降级保证 × 类型一致 × 传递传递性 = 权限安全**。
