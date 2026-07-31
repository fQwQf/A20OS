# A20OS Native ABI：类型与结构体定义

> 本文档定义 A20OS Native ABI 的所有用户可见类型和结构体。权限语义见 [security.md](06-security.md)，Handle 生命周期见 [handle.md](03-handle.md)。

---

## 1. 基础类型

Native ABI 固定使用小端（little-endian）、补码整数（two's complement）、显式宽度类型（explicit-width types）。

```c
typedef uint32_t a20_handle_t;     /* 进程本地 handle 编号 */
typedef uint64_t a20_rights_t;     /* 14 位权限位域 */
typedef uint64_t a20_flags_t;      /* 操作标志位 */
typedef int64_t  a20_status_t;     /* 返回状态（>= 0 成功，< 0 错误） */
typedef uint64_t a20_time_ns_t;    /* 纳秒时间戳 */
typedef uint64_t a20_off_t;        /* 文件偏移量 */
typedef uint64_t a20_size_t;       /* 大小 */
typedef uint64_t a20_vaddr_t;      /* 虚拟地址 */
```

指针大小由 `a20_abi_info.pointer_bits` 指示。64 位架构上 native ABI 首选 64 位用户指针。若未来支持 32 位用户态，应视为单独 ABI profile。

---

## 2. ABI 头约定

所有 syscall 参数结构体以 `a20_abi_header_t` 开头：

```c
typedef struct a20_abi_header {
    uint32_t size;      /* 结构体实际大小（字节） */
    uint32_t version;   /* 结构体版本号 */
} a20_abi_header_t;
```

### 演进规则

1. 用户传入的 `size` 必须覆盖其所声明 `version` 的完整必需前缀；当前所有 version-1 结构体均要求 `size >= sizeof(struct)`，拒绝通过零填充补齐必需字段。
2. 用户传入的 `size` 大于内核支持结构体大小时，内核只读取已知字段。
3. 新字段只能追加并提高 `version`；旧 version 的最小 `size` 必须在该 version 引入时固定。
4. flag 保留位必须为 0，否则返回 `A20_ERR_INVALID_ARGUMENT`。

这些规则使 ABI 可以在不破坏旧程序的情况下扩展。内核通过 `size` 和 `version` 确定调用方使用的结构体版本。

---

## 3. Core / ABI 结构体

### a20_abi_info_t — ABI 查询结果

```c
typedef struct a20_abi_info {
    uint32_t size;
    uint32_t version;
    uint32_t abi_major;          /* 主版本号，不兼容变更时递增 */
    uint32_t abi_minor;          /* 次版本号，兼容新增功能时递增 */
    uint32_t abi_patch;          /* 补丁版本号，仅 bugfix */
    uint32_t pointer_bits;       /* 指针宽度（32 或 64） */
    uint32_t page_size;          /* 页大小（字节） */
    uint32_t handle_bits;        /* handle 编号宽度（32） */
    uint64_t feature_bits[4];    /* 可选能力位图 */
    uint64_t syscall_bitmap_addr;/* 用户地址：支持的 syscall 位图 */
    uint64_t syscall_bitmap_size;/* 位图大小（字节） */
} a20_abi_info_t;
```

---

## 4. Handle 操作结构体

### a20_handle_dup_args_t — handle 复制

```c
typedef struct a20_handle_dup_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t source;         /* 源 handle */
    uint32_t flags;              /* 保留，必须为 0 */
    a20_rights_t rights_mask;    /* 请求的权限子集 */
    a20_handle_t out_handle;     /* 输出：新 handle */
    uint32_t reserved;
} a20_handle_dup_args_t;
```

`rights_mask` 必须是源 handle rights 的子集，否则返回 `A20_ERR_ACCESS`。

### a20_handle_info_t — handle 查询结果

```c
typedef struct a20_handle_info {
    uint32_t size;
    uint32_t version;
    uint32_t object_type;        /* A20_OBJ_* 类型常量 */
    uint32_t state;              /* 对象状态 */
    a20_rights_t rights;         /* 当前 handle 的权限 */
    uint64_t object_id_hint;     /* 调试用，不保证全局稳定 */
    uint64_t flags;              /* 对象属性标志 */
} a20_handle_info_t;
```

### a20_control_args_t — 通用对象控制

```c
typedef struct a20_control_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t handle;         /* 目标 handle */
    uint32_t namespace_id;       /* 命令所属的对象协议 */
    uint32_t command;            /* 具体命令 */
    uint64_t in_ptr;             /* 输入缓冲区（用户指针） */
    uint64_t in_size;            /* 输入大小 */
    uint64_t out_ptr;            /* 输出缓冲区（用户指针） */
    uint64_t out_size;           /* 输出缓冲区容量 */
    uint64_t out_actual;         /* 输出：实际写入字节数 */
} a20_control_args_t;
```

规则：`namespace_id` 表明命令属于哪个对象协议；命令结构也必须版本化；通用操作应优先设计成明确 syscall，不滥用 control。

### handle_control 命令与 a20_handle_temporal_args_t — 时态能力控制

当前 `handle_control` 的 syscall 形式为 `handle_control(handle, op, arg0, arg1)`。op 0/1 为 file/device 的 ioctl/fcntl 透传；op 2–4 操作 handle 条目本身：

```c
#define A20_HANDLE_CTRL_IOCTL          0u
#define A20_HANDLE_CTRL_FCNTL          1u
#define A20_HANDLE_CTRL_SET_TEMPORAL   2u  /* arg0 = a20_handle_temporal_args_t* */
#define A20_HANDLE_CTRL_GET_TEMPORAL   3u  /* arg0 = a20_handle_temporal_args_t* */
#define A20_HANDLE_CTRL_SET_LABEL      4u  /* arg0 = 新标签（0=L,1=M,2=H，仅可上调） */

/* 时态能力控制（03-handle.md §2.6，06-security.md §6）。
 * SET 仅可增强（non-refreshability）：flag 只能添加不能清除，
 * expiry 只能提前，remaining_ops 只能减少。 */
typedef struct a20_handle_temporal_args {
    uint32_t size;
    uint32_t version;
    uint64_t expiry_ns;      /* 绝对 CLOCK_MONOTONIC 纳秒；0 = 无过期 */
    uint32_t remaining_ops;  /* OP_COUNT flag 置位时的操作预算；0 = 已耗尽 */
    uint32_t temporal_flags; /* A20_TEMPORAL_* */
} a20_handle_temporal_args_t;
```

---

## 5. Task / Thread 结构体

### a20_spawn_handle_t — spawn 传递的 handle

```c
typedef struct a20_spawn_handle {
    a20_handle_t handle;         /* 要传递的 handle */
    a20_rights_t rights;         /* 传递后的权限（必须是原 handle 权限子集） */
    uint32_t target_slot;        /* 可选目标槽位，0 表示自动分配 */
    uint32_t flags;              /* 保留 */
} a20_spawn_handle_t;
```

### a20_task_spawn_args_t — 进程创建

```c
typedef struct a20_task_spawn_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t image;          /* 可执行文件或 image 对象 */
    a20_handle_t root_dir;       /* 根目录 handle */
    a20_handle_t cwd_dir;        /* 工作目录 handle */
    a20_handle_t event_queue;    /* 默认事件队列 handle */
    uint64_t argv;               /* char*[] */
    uint64_t envp;               /* char*[] */
    uint32_t argc;
    uint32_t envc;
    uint64_t handles;            /* a20_spawn_handle_t[] */
    uint32_t handle_count;
    uint32_t flags;
    a20_handle_t out_task;       /* 输出：新进程的 task handle */
} a20_task_spawn_args_t;
```

### a20_thread_create_args_t — 线程创建

```c
typedef struct a20_thread_create_args {
    uint32_t size;
    uint32_t version;
    uint64_t entry;              /* 入口函数地址 */
    uint64_t arg;                /* 入口参数 */
    uint64_t stack_base;         /* 栈基址 */
    uint64_t stack_size;         /* 栈大小 */
    uint64_t tls_base;           /* TLS 基址 */
    uint32_t flags;
    uint32_t reserved;
    a20_handle_t out_thread;     /* 输出：新线程 handle */
} a20_thread_create_args_t;
```

### a20_task_status_t — 进程退出状态

```c
typedef struct a20_task_status {
    uint32_t size;
    uint32_t version;
    uint32_t reason;             /* 退出原因 */
    int32_t  exit_code;          /* 退出码 */
    uint64_t user_time_ns;       /* 用户态 CPU 时间 */
    uint64_t kernel_time_ns;     /* 内核态 CPU 时间 */
} a20_task_status_t;
```

---

## 6. Memory 结构体

### a20_vm_alloc_args_t — 匿名内存分配

```c
typedef struct a20_vm_alloc_args {
    uint32_t size;
    uint32_t version;
    uint64_t addr_hint;          /* 建议地址，0 表示内核选择 */
    uint64_t length;             /* 分配大小（字节） */
    uint32_t prot;               /* 保护标志（R/W/X） */
    uint32_t flags;              /* 分配标志 */
    uint64_t out_addr;           /* 输出：分配到的地址 */
} a20_vm_alloc_args_t;
```

### a20_vm_map_args_t — 文件/共享内存映射

```c
typedef struct a20_vm_map_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t source;         /* 源对象 handle（MEMORY/FILE/DEVICE），A20_HANDLE_NULL = 匿名 */
    uint32_t _pad;
    uint64_t addr_hint;          /* 建议地址 */
    uint64_t length;             /* 映射长度 */
    uint64_t offset;             /* 源对象内偏移 */
    uint32_t prot;               /* 保护标志 */
    uint32_t flags;              /* 映射标志 */
    uint64_t out_addr;           /* 输出：映射到的地址 */
} a20_vm_map_args_t;
```

### a20_vm_share_args_t — 内存共享导出

```c
typedef struct a20_vm_share_args {
    uint32_t size;
    uint32_t version;
    uint64_t addr;               /* 要共享的地址 */
    uint64_t length;             /* 共享长度 */
    a20_rights_t rights;         /* 导出权限 */
    a20_handle_t out_handle;     /* 输出：共享内存对象 handle */
} a20_vm_share_args_t;
```

---

## 7. Filesystem / Path 结构体

### a20_path_open_args_t — 路径打开

```c
typedef struct a20_path_open_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t dir;            /* 基目录 handle */
    uint32_t flags;              /* 打开标志 */
    a20_rights_t rights;         /* 请求的权限 */
    uint64_t path;               /* const char*：路径 */
    uint64_t path_len;           /* 路径长度，0 表示 nul-terminated */
    uint32_t mode;               /* 创建模式 */
    uint32_t reserved;
    a20_handle_t out_handle;     /* 输出：打开的文件 handle */
} a20_path_open_args_t;
```

### a20_path_create_args_t — 创建文件系统节点

```c
typedef struct a20_path_create_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t dir;            /* 父目录 handle */
    uint32_t node_type;          /* 节点类型（file/dir/socket/device） */
    uint64_t path;               /* const char* */
    uint64_t path_len;
    uint32_t mode;               /* 创建模式 */
    uint32_t flags;
    a20_rights_t rights;         /* 请求的权限 */
    a20_handle_t out_handle;     /* 输出：新节点的 handle */
} a20_path_create_args_t;
```

### a20_iovec_t / a20_io_args_t — I/O 操作

```c
typedef struct a20_iovec {
    uint64_t base;               /* 缓冲区基址 */
    uint64_t len;                /* 缓冲区长度 */
} a20_iovec_t;

typedef struct a20_io_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t handle;         /* 目标 handle */
    uint32_t flags;              /* I/O 标志 */
    uint64_t iov;                /* a20_iovec_t[] */
    uint32_t iov_count;
    uint32_t reserved;
    uint64_t offset;             /* 偏移量，支持 A20_OFFSET_CURRENT */
    uint64_t out_count;          /* 输出：实际传输字节数 */
} a20_io_args_t;
```

### a20_stat_t — 文件属性

```c
typedef struct a20_stat {
    uint32_t size;
    uint32_t version;
    uint64_t dev;                /* 设备号 */
    uint64_t ino;                /* inode 号 */
    uint32_t mode;               /* 文件模式 */
    uint32_t nlink;              /* 硬链接数 */
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;         /* 文件大小 */
    uint64_t blocks;             /* 块数 */
    uint64_t atime_ns;           /* 访问时间 */
    uint64_t mtime_ns;           /* 修改时间 */
    uint64_t ctime_ns;           /* 元数据变更时间 */
} a20_stat_t;
```

---

## 8. Event / IPC 结构体

### a20_event_queue_create_args_t — 创建事件队列

```c
typedef struct a20_event_queue_create_args {
    uint32_t size;
    uint32_t version;
    uint32_t capacity_hint;      /* 容量提示 */
    uint32_t flags;
    a20_handle_t out_queue;      /* 输出：事件队列 handle */
} a20_event_queue_create_args_t;
```

### a20_event_watch_args_t — 注册事件关注

```c
typedef struct a20_event_watch_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t queue;          /* 事件队列 handle */
    a20_handle_t target;         /* 被观察的 handle */
    uint64_t event_mask;         /* 关注的事件位图 */
    uint64_t user_data;          /* 用户关联数据 */
} a20_event_watch_args_t;
```

### a20_pending_event_t / a20_event_wait_args_t — 等待事件

```c
typedef struct a20_pending_event {
    a20_handle_t source;         /* 产生事件的 handle */
    uint32_t type;               /* 事件类型（A20_EVENT_* 索引） */
    uint64_t events;             /* 触发的事件位图 = 1ull << type */
    uint64_t user_data;          /* 注册时的 user_data */
    uint64_t data0, data1, data2;/* 事件相关数据 */
} a20_pending_event_t;

/* 可观察事件类型（05-ipc.md §3.3）；事件掩码为 1ull << 类型 */
#define A20_EVENT_READABLE        0u   /* file/socket/pipe：可读 */
#define A20_EVENT_WRITABLE        1u   /* file/socket/pipe：可写 */
#define A20_EVENT_ERROR           2u   /* file/socket：I/O 错误 */
#define A20_EVENT_CLOSED          3u   /* 对象被关闭 */
#define A20_EVENT_CONNECTION      4u   /* socket：新连接到达 */
#define A20_EVENT_ACCEPT_READY    5u   /* socket：可 accept */
#define A20_EVENT_EXPIRED         6u   /* timer：到期 */
#define A20_EVENT_EXITED          7u   /* task/thread：退出 */
#define A20_EVENT_MESSAGE_READY   8u   /* channel：有消息可接收 */
#define A20_EVENT_PEER_CLOSED     9u   /* channel：对端关闭 */
#define A20_EVENT_MASK(ev)        (1ull << (ev))

typedef struct a20_event_wait_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t queue;          /* 事件队列 handle */
    uint32_t _pad;
    uint64_t events;             /* a20_pending_event_t[] 输出缓冲区 */
    uint32_t max_events;         /* 缓冲区容量（内核上限 64） */
    uint32_t _pad2;
    uint64_t timeout_ns;         /* 相对超时（纳秒）：0 = 不等待，
                                  * A20_TIMEOUT_INFINITE = 无限等待 */
    uint32_t flags;              /* 保留 */
    uint32_t out_count;          /* 输出：实际事件数 */
} a20_event_wait_args_t;
```

`event_wait` 在队列为空时默认**阻塞**（tokenized Park/Wake），`timeout_ns == 0` 退化为轮询（空队列返回 `A20_ERR_WOULD_BLOCK`），超时返回 `A20_ERR_TIMED_OUT`。当前产生事件的对象：channel（MESSAGE_READY/PEER_CLOSED/CLOSED）、timer（EXPIRED）、task 退出（EXITED）；file/socket 的 READABLE/WRITABLE 等事件源尚未接入。

### a20_msg_send_args_t / a20_msg_recv_args_t — 消息通道

```c
typedef struct a20_msg_send_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t channel;        /* 通道端点 handle */
    uint32_t _pad;
    uint64_t data;               /* 消息字节缓冲区 */
    uint32_t data_len;           /* 字节数（上限 64KB） */
    uint32_t flags;              /* A20_MSG_* */
    uint64_t handles;            /* a20_handle_t[]：要传递的 handle */
    uint32_t handle_count;       /* 传递的 handle 数量（上限 8） */
    uint64_t transfer_rights;    /* a20_rights_t[] 每 handle 权限上限，0 = 源权限 */
} a20_msg_send_args_t;

typedef struct a20_msg_recv_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t channel;        /* 通道端点 handle */
    uint32_t _pad;
    uint64_t data_buf;           /* 接收缓冲区 */
    uint32_t data_buf_len;       /* 缓冲区容量 */
    uint32_t _pad2;
    uint64_t handle_buf;         /* a20_handle_t[]：接收 handle 缓冲区 */
    uint32_t handle_buf_count;   /* handle 缓冲区容量 */
    uint32_t flags;              /* A20_MSG_*（原 _pad3 字段，布局不变） */
    uint64_t out_data_len;       /* 输出：实际接收字节数 */
    uint32_t out_handle_count;   /* 输出：实际接收 handle 数 */
    uint64_t out_rights_buf;     /* a20_rights_t[]：每个接收 handle 的权限 */
} a20_msg_recv_args_t;

#define A20_MSG_NONBLOCK   (1u << 0)  /* 不阻塞，无法立即完成时返回 WOULD_BLOCK */
```

`channel_send` 在对端队列满时默认阻塞，`channel_recv` 在队列空时默认阻塞；传 `A20_MSG_NONBLOCK` 退化为非阻塞。接收方 handle table 空间不足时 `channel_recv` 返回 `A20_ERR_NO_SPACE`，消息留在队列中（不做部分投递，05-ipc.md §2.6）。

---

## 9. Network 结构体

### a20_net_socket_args_t — 创建套接字

```c
typedef struct a20_net_socket_args {
    uint32_t size;
    uint32_t version;
    int32_t domain;              /* 地址族（AF_INET 等） */
    int32_t type;                /* 套接字类型 */
    int32_t protocol;            /* 协议 */
    a20_rights_t rights;         /* 请求的权限 */
    a20_handle_t out_socket;     /* 输出：套接字 handle */
} a20_net_socket_args_t;
```

### a20_net_addr_t — 网络地址

```c
typedef struct a20_net_addr {
    uint16_t family;             /* AF_INET / AF_INET6 */
    uint16_t port;
    uint32_t _pad;
    uint8_t  addr[16];           /* IPv4 使用前 4 字节 */
} a20_net_addr_t;
```

不直接承诺 Linux `sockaddr` 布局；调用方通过 syscall 参数中的地址长度区分有效字段。

---

## 10. Timer 结构体

### a20_timer_create_args_t — 创建定时器

```c
typedef struct a20_timer_create_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t event_queue;    /* 事件队列 handle（到期事件投递目标） */
    uint64_t user_data;          /* 事件关联数据 */
    uint32_t flags;
    a20_handle_t out_timer;      /* 输出：定时器 handle */
} a20_timer_create_args_t;
```

Timer 是 handle，可被 event queue watch，不需要复制 POSIX timer id + signal delivery 模型。创建后通过 `timer_set(timer, absolute_deadline_ns, interval_ns)` 设置单次或周期到期时间；到期投递 `A20_EVENT_EXPIRED`，不投递 SIGALRM。

---

## 11. Extended I/O 结构体

### a20_transfer_args_t — 零拷贝传输

统一 splice / sendfile / copy_file_range / tee 语义。通过 `flags` 区分传输模式。

```c
/* 传输标志 */
#define A20_TRANSFER_PEEK       0x0001u   /* 不推进源偏移（tee 语义） */

typedef struct a20_transfer_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t source;          /* 源 handle（仅文件/设备支持 seek） */
    a20_handle_t dest;            /* 目标 handle（仅文件/设备支持 seek） */
    uint32_t flags;               /* A20_TRANSFER_* */
    uint64_t source_offset;       /* 源偏移，A20_OFFSET_CURRENT 表示当前位置 */
    uint64_t dest_offset;         /* 目标偏移，A20_OFFSET_CURRENT 表示当前位置 */
    uint64_t length;              /* 传输字节数 */
    uint64_t out_transferred;     /* 输出：实际传输字节数 */
} a20_transfer_args_t;
```

设计说明：
- `source_offset`/`dest_offset` 为 `A20_OFFSET_CURRENT`（`UINT64_MAX`）时使用当前位置。
- 权限检查：`source` 需要 `READ | TRANSFER`，`dest` 需要 `WRITE | TRANSFER`。
- 当前支持 `flags == 0`（consume，推进源偏移）和 `A20_TRANSFER_PEEK`（tee，不推进源偏移）；其余保留标志位必须为零。

---

## 12. Metadata 结构体

### a20_set_meta_args_t — 文件元数据修改

统一 chmod / chown / utimes 语义。通过 flags 指定要修改的字段。

```c
/* 元数据修改标志 */
#define A20_SET_META_MODE      (1u << 0)  /* 修改 mode（chmod） */
#define A20_SET_META_OWNER     (1u << 1)  /* 修改 uid/gid（chown） */
#define A20_SET_META_ATIME     (1u << 2)  /* 修改访问时间 */
#define A20_SET_META_MTIME     (1u << 3)  /* 修改修改时间 */
#define A20_SET_META_CTIME     (1u << 4)  /* 修改状态变更时间 */
#define A20_SET_META_TRUNCATE  (1u << 5)  /* 截断文件到指定大小（ftruncate） */
#define A20_SET_META_ALLOCATE  (1u << 6)  /* 预分配空间（fallocate） */

typedef struct a20_set_meta_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t handle;          /* 目标文件 handle */
    uint32_t flags;               /* A20_SET_META_* 组合 */
    uint32_t mode;                /* 新 mode（flags 含 MODE 时有效） */
    uint32_t uid;                 /* 新 uid（flags 含 OWNER 时有效） */
    uint32_t gid;                 /* 新 gid（flags 含 OWNER 时有效） */
    uint64_t atime_ns;            /* 新 atime（纳秒） */
    uint64_t mtime_ns;            /* 新 mtime（纳秒） */
    uint64_t ctime_ns;            /* 新 ctime（纳秒） */
    uint64_t truncate_size;       /* 截断大小（flags 含 TRUNCATE 时有效） */
    uint64_t allocate_size;       /* 预分配大小（flags 含 ALLOCATE 时有效） */
} a20_set_meta_args_t;
```

设计说明：
- Linux 的 `fchmod`/`fchmodat`/`fchown`/`fchownat`/`utimensat`/`ftruncate`/`fallocate` 是 7+ 个独立 syscall。
- A20 统一为 `handle_set_meta`，一次调用可同时修改多个字段，减少 syscall 次数。
- 只修改 flags 指定的字段，未指定的字段不受影响。
- 对于路径版本（`fchmodat` 等），调用者先 `path_open` 获得 handle 再调用 `handle_set_meta`。

---

## 13. Extended Attribute 结构体

### a20_xattr_args_t — 扩展属性操作

```c
/* xattr 标志 */
#define A20_XATTR_CREATE    0x0001u   /* 仅创建，已存在则报错 */
#define A20_XATTR_REPLACE   0x0002u   /* 仅替换，不存在则报错 */

typedef struct a20_xattr_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t handle;          /* 目标 handle */
    uint32_t _pad;
    uint64_t name;                /* const char*：属性名 */
    uint32_t name_len;            /* 属性名长度，0 表示 nul-terminated */
    uint32_t _pad2;
    uint64_t value;               /* void*：值缓冲区 */
    uint64_t value_len;           /* 值大小（set 时为输入，get 时为缓冲区容量） */
    uint32_t flags;               /* A20_XATTR_* */
} a20_xattr_args_t;
```

### a20_xattr_list_args_t — 列出扩展属性

```c
typedef struct a20_xattr_list_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t handle;          /* 目标 handle */
    uint32_t _pad;
    uint64_t buf;                 /* char*：输出缓冲区，包含 nul-terminated 的名称列表 */
    uint64_t buf_len;             /* 缓冲区容量 */
    uint64_t out_len;             /* 输出：实际需要的总大小 */
} a20_xattr_list_args_t;
```

---

## 14. Scheduling 结构体

### a20_sched_args_t — 调度参数

统一 priority / policy / affinity / scheduler 等参数。

```c
/* 调度策略 */
#define A20_SCHED_OTHER     0
#define A20_SCHED_FIFO      1
#define A20_SCHED_RR        2
#define A20_SCHED_BATCH     3
#define A20_SCHED_IDLE      5
#define A20_SCHED_DEADLINE  6

/* 调度标志 */
#define A20_SCHED_POLICY        (1u << 0)
#define A20_SCHED_PRIORITY      (1u << 1)
#define A20_SCHED_AFFINITY      (1u << 2)
#define A20_SCHED_NICE          (1u << 3)

typedef struct a20_sched_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t task;            /* 目标 task/thread handle */
    uint32_t flags;               /* 指定要设置/查询的字段 */
    int32_t  policy;              /* 调度策略 */
    int32_t  priority;            /* 静态优先级（1-99 for FIFO/RR） */
    int32_t  nice;                /* nice 值（-20..19 for OTHER） */
    uint64_t affinity;            /* CPU 亲和性位图 */
    uint64_t affinity_size;       /* 亲和性位图大小（字节） */
} a20_sched_args_t;
```

设计说明：
- Linux 的 `sched_setparam`/`sched_getparam`/`sched_setscheduler`/`sched_getscheduler`/`sched_setaffinity`/`sched_getaffinity`/`setpriority`/`getpriority`/`sched_setattr`/`sched_getattr` 是 10 个独立 syscall。
- A20 统一为 `task_set_sched`/`task_get_sched` 两个 syscall，通过 flags 组合指定要操作的调度参数。
- `task_get_sched` 使用同一个 args struct，内核填充请求的字段。

---

## 15. Resource Limits 结构体

> **当前调用契约**：`task_get_limits(task, out)` / `task_set_limits(task, in)` 使用 `abi/native/resource.h` 中的聚合 `a20_resource_limits_t`（handles/channels/threads/memory 四个上限）。下述 `a20_rlimit_args_t` 是按 POSIX resource 编号细分的保留布局，当前 syscall 入口尚未使用它。

### a20_rlimit_args_t — 资源限制

```c
/* 资源类型 */
#define A20_RLIMIT_CPU       0    /* CPU 时间（秒） */
#define A20_RLIMIT_DATA      2    /* 数据段大小 */
#define A20_RLIMIT_STACK     3    /* 栈大小 */
#define A20_RLIMIT_CORE      4    /* core 文件大小 */
#define A20_RLIMIT_NOFILE    7    /* 打开文件/handle 数 */
#define A20_RLIMIT_AS        9    /* 地址空间大小 */
#define A20_RLIMIT_NICE     13    /* nice 优先级上限 */
#define A20_RLIMIT_RTPRIO   14    /* 实时优先级上限 */

typedef struct a20_rlimit_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t task;            /* 目标 task handle，A20_HANDLE_NULL 表示当前 task */
    uint32_t resource;            /* A20_RLIMIT_* */
    uint64_t cur;                 /* 当前软限制 */
    uint64_t max;                 /* 硬限制上限 */
} a20_rlimit_args_t;
```

### a20_rusage_t — 资源使用量

```c
typedef struct a20_rusage {
    uint64_t user_time_ns;        /* 用户态 CPU 时间 */
    uint64_t sys_time_ns;         /* 内核态 CPU 时间 */
    uint64_t max_rss;             /* 最大驻内存集大小 */
    uint64_t page_faults;         /* page fault 总数 */
    uint64_t io_read;             /* I/O 读取计数 */
    uint64_t io_write;            /* I/O 写入计数 */
} a20_rusage_t;
```

`a20_rusage_t` 是 syscall 输出载荷，不单独携带 `{size, version}`；调用方通过对应查询 syscall 的 ABI 版本确定布局。

---

## 16. Extended Memory 结构体

### a20_vm_remap_args_t — 内存重映射

```c
/* remap 标志 */
#define A20_REMAP_MAY_MOVE     0x0001u   /* 允许内核移动映射到新地址 */
#define A20_REMAP_EXACT        0x0002u   /* 必须映射到指定地址 */

typedef struct a20_vm_remap_args {
    uint32_t size;
    uint32_t version;
    uint64_t old_addr;            /* 现有映射地址 */
    uint64_t old_size;            /* 现有映射大小 */
    uint64_t new_addr_hint;       /* 建议新地址 */
    uint64_t new_size;            /* 新大小 */
    uint32_t flags;               /* A20_REMAP_* */
    uint64_t out_addr;            /* 输出：实际映射地址 */
} a20_vm_remap_args_t;
```

### a20_vm_object_args_t — 创建匿名内存对象

等价于 Linux `memfd_create`，但返回 A20 handle。

```c
/* 内存对象标志 */
#define A20_VM_OBJ_CLOEXEC     0x0001u
#define A20_VM_OBJ_ALLOW_SEAL  0x0002u
#define A20_VM_OBJ_HUGETLB     0x0004u

typedef struct a20_vm_object_args {
    uint32_t size;
    uint32_t version;
    uint64_t size_bytes;          /* 初始大小 */
    uint32_t flags;               /* 当前实现传给 VMO options */
    a20_handle_t out_handle;      /* 输出：内存对象 handle */
} a20_vm_object_args_t;
```

创建的内存对象可通过 `vm_map` 映射到地址空间，通过 `handle_dup` 或 channel 传递分享。命名、seal、hugetlb 与通过 metadata 调整 VMO 大小尚未实现。

---

## 17. Extended Filesystem 结构体

### a20_path_link_args_t — 创建硬链接

```c
typedef struct a20_path_link_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t old_dir;         /* 源文件所在目录 handle */
    a20_handle_t new_dir;         /* 目标目录 handle */
    uint64_t old_path;            /* const char*：源文件路径 */
    uint32_t old_path_len;
    uint64_t new_path;            /* const char*：链接路径 */
    uint32_t new_path_len;
    uint32_t flags;               /* 保留 */
} a20_path_link_args_t;
```

### a20_path_symlink_args_t — 创建符号链接

```c
typedef struct a20_path_symlink_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t dir;             /* 创建链接的父目录 handle */
    uint64_t target;              /* const char*：链接目标 */
    uint32_t target_len;
    uint64_t linkpath;            /* const char*：链接路径 */
    uint32_t linkpath_len;
} a20_path_symlink_args_t;
```

### a20_path_readlink_args_t — 读取符号链接

```c
typedef struct a20_path_readlink_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t dir;             /* 父目录 handle */
    uint64_t path;                /* const char*：链接路径 */
    uint32_t path_len;
    uint64_t buf;                 /* char*：输出缓冲区 */
    uint64_t buf_len;             /* 缓冲区容量 */
    uint64_t out_len;             /* 输出：实际写入字节数 */
} a20_path_readlink_args_t;
```

### a20_path_resolve_args_t — 路径解析

统一 `faccessat` / `readlinkat` 检查类操作。

```c
/* 解析标志 */
#define A20_RESOLVE_ACCESS     0x0000u   /* 检查可访问性（faccessat） */
#define A20_RESOLVE_EXISTS     0x0001u   /* 仅检查存在性 */
#define A20_RESOLVE_NOFOLLOW   0x0002u   /* 不跟随符号链接 */

typedef struct a20_path_resolve_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t dir;             /* 基目录 handle */
    uint64_t path;                /* const char*：目标路径 */
    uint64_t path_len;
    uint32_t flags;               /* A20_RESOLVE_* */
    uint32_t mode;                /* access 检查的模式（R_OK/W_OK/X_OK） */
    a20_handle_t out_handle;      /* 输出：解析后的 handle（可选） */
} a20_path_resolve_args_t;
```

### a20_fs_stat_t — 文件系统统计

```c
typedef struct a20_fs_stat {
    uint32_t size;
    uint32_t version;
    uint64_t total_blocks;        /* 总块数 */
    uint64_t free_blocks;         /* 空闲块数 */
    uint64_t available_blocks;    /* 非特权用户可用块数 */
    uint64_t total_files;         /* 总 inode 数 */
    uint64_t free_files;          /* 空闲 inode 数 */
    uint64_t block_size;          /* 块大小 */
    uint64_t max_name_len;        /* 最大文件名长度 */
    uint64_t fs_id;               /* 文件系统标识 */
    uint32_t fs_type;             /* 文件系统类型 */
    uint32_t flags;               /* 文件系统特性标志 */
} a20_fs_stat_t;
```

### a20_dirent_t — 目录项

`path_readdir` 返回的目录项结构。内核将底层 `vfs_dirent64` 翻译为此格式。

```c
typedef struct a20_dirent {
    uint32_t type;                /* 文件类型（VFS d_type） */
    uint32_t name_len;            /* 文件名实际长度 */
    char     name[256];           /* 以 NUL 结尾的文件名 */
} a20_dirent_t;
```

### a20_fs_mount_args_t — 挂载文件系统

```c
/* 挂载标志 */
#define A20_MOUNT_RDONLY       0x0001u
#define A20_MOUNT_NOSUID       0x0002u
#define A20_MOUNT_NODEV        0x0004u
#define A20_MOUNT_NOEXEC       0x0008u
#define A20_MOUNT_REMOUNT      0x0010u
#define A20_MOUNT_BIND         0x0020u

typedef struct a20_fs_mount_args {
    uint32_t size;
    uint32_t version;
    uint64_t source;              /* const char*：设备/源路径 */
    uint32_t source_len;
    uint32_t reserved0;
    uint64_t target;              /* const char*：挂载点路径 */
    uint32_t target_len;
    uint32_t reserved1;
    uint64_t fs_type;             /* const char*：文件系统类型名 */
    uint32_t fs_type_len;
    uint32_t flags;               /* A20_MOUNT_* */
} a20_fs_mount_args_t;
```

---

## 18. Extended Network 结构体

### a20_net_socketpair_args_t — 创建套接字对

```c
typedef struct a20_net_socketpair_args {
    uint32_t size;
    uint32_t version;
    uint32_t domain;              /* 地址族 */
    uint32_t type;                /* 套接字类型 */
    uint32_t protocol;            /* 协议 */
    uint32_t flags;
    a20_rights_t rights;          /* 两个端点的请求权限 */
    a20_handle_t out_socket0;     /* 输出：端点 0 */
    a20_handle_t out_socket1;     /* 输出：端点 1 */
} a20_net_socketpair_args_t;
```

### net_getname 标志

```c
#define A20_NET_NAME_LOCAL     0x0000u   /* 获取本地地址（getsockname） */
#define A20_NET_NAME_PEER      0x0001u   /* 获取对端地址（getpeername） */
```

---

## 19. Security Context 结构体

### a20_security_context_t — 安全上下文

A20 的安全上下文同时支持原生 capability 模型和 POSIX 兼容身份。

```c
/* 安全上下文标志 */
#define A20_SEC_SET_UID        0x0001u
#define A20_SEC_SET_GID        0x0002u
#define A20_SEC_SET_GROUPS     0x0004u
#define A20_SEC_SET_CAPS       0x0008u

/* POSIX 兼容能力位 */
#define A20_POSIX_CAP_CHOWN         0
#define A20_POSIX_CAP_DAC_OVERRIDE  1
#define A20_POSIX_CAP_DAC_READ_SEARCH 2
#define A20_POSIX_CAP_FOWNER        3
#define A20_POSIX_CAP_FSETID        4
#define A20_POSIX_CAP_KILL          5
#define A20_POSIX_CAP_SETGID        6
#define A20_POSIX_CAP_SETUID        7
#define A20_POSIX_CAP_SETPCAP       8
#define A20_POSIX_CAP_NET_BIND_SERVICE 9
#define A20_POSIX_CAP_NET_RAW       10
#define A20_POSIX_CAP_SYS_ADMIN     11

typedef struct a20_security_context {
    uint32_t size;
    uint32_t version;
    int32_t uid;
    int32_t euid;
    int32_t gid;
    int32_t egid;
    int32_t ngroups;
    int32_t _pad;
    uint64_t groups;              /* int[]：补充组 ID 数组 */
    uint64_t cap_effective;       /* POSIX capability 位图 */
    uint64_t namespace_mask;      /* 所属 namespace 掩码 */
    a20_rights_t effective_rights;/* 当前有效 rights 集合 */
    uint32_t flags;               /* A20_SEC_SET_*（set 时有效） */
    uint32_t label;               /* 0=L, 1=M, 2=H */
} a20_security_context_t;
```

设计说明：
- `security_get_context` 查询当前完整的身份和权限状态。
- `security_set_context` 只修改 `flags` 指定的字段（类似 `setuid`/`setgid`/`setgroups` 的统一接口）。
- 修改 uid/gid 需要对应的 POSIX capability 或 A20 rights。
- A20 原生字段（effective_rights, namespace_mask）是只读的，由内核根据 handle 权限和 namespace 推导；`label` 只能上调。

---

## 20. System 结构体

### a20_system_info_t — 系统信息

统一 uname / sysinfo 语义。

```c
typedef struct a20_system_info {
    uint32_t size;
    uint32_t struct_version;
    /* uname 等价 */
    char     sysname[64];         /* 操作系统名 */
    char     nodename[64];        /* 网络节点名 */
    char     release[64];         /* 内核版本 */
    char     version[64];         /* 构建版本 */
    char     machine[64];         /* 硬件架构 */
    /* sysinfo 等价 */
    uint64_t total_ram;           /* 总物理内存 */
    uint64_t free_ram;            /* 空闲物理内存 */
    uint64_t total_swap;          /* 总交换空间 */
    uint64_t free_swap;           /* 空闲交换空间 */
    uint16_t num_procs;           /* 进程数 */
    uint16_t _pad;
    uint32_t configured_cpus;     /* 配置 CPU 数 */
    uint32_t online_cpus;         /* 在线 CPU 数 */
    uint32_t current_cpu;         /* 当前 CPU */
    uint32_t page_size;           /* 页大小 */
    uint64_t uptime_ns;           /* 系统运行时间（纳秒） */
} a20_system_info_t;
```

### system_reboot 命令

```c
#define A20_REBOOT_HALT        0   /* 停机 */
#define A20_REBOOT_POWER_OFF   1   /* 关电源 */
#define A20_REBOOT_RESTART     2   /* 重启 */
#define A20_REBOOT_RESTART2    3   /* 重启到指定模式（arg 指定） */
```

---

## 21. Event Watch FS 结构体

### a20_event_watch_fs_args_t — 文件系统变更通知

```c
/* 文件系统事件标志 */
#define A20_FS_EVENT_ACCESS     0x00000001u
#define A20_FS_EVENT_MODIFY     0x00000002u
#define A20_FS_EVENT_ATTRIB     0x00000004u
#define A20_FS_EVENT_CLOSE_WRITE 0x00000008u
#define A20_FS_EVENT_CLOSE_NOWRITE 0x00000010u
#define A20_FS_EVENT_OPEN       0x00000020u
#define A20_FS_EVENT_MOVED_FROM 0x00000040u
#define A20_FS_EVENT_MOVED_TO   0x00000080u
#define A20_FS_EVENT_CREATE     0x00000100u
#define A20_FS_EVENT_DELETE     0x00000200u
#define A20_FS_EVENT_DELETE_SELF 0x00000400u
#define A20_FS_EVENT_MOVE_SELF  0x00000800u

#define A20_FS_EVENT_ISDIR      0x40000000u   /* 事件发生在目录上 */

typedef struct a20_event_watch_fs_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t queue;           /* 事件队列 handle */
    a20_handle_t dir;             /* 监控目录 handle */
    uint64_t path;                /* const char*：监控路径（相对于 dir） */
    uint32_t path_len;
    uint32_t event_mask;          /* A20_FS_EVENT_* 组合 */
    uint64_t user_data;           /* 事件关联数据 */
} a20_event_watch_fs_args_t;
```

设计说明：
- Linux 的 `inotify_init`/`inotify_add_watch`/`inotify_rm_watch` 是独立于 epoll 的子系统。
- A20 将文件系统事件**统一纳入现有 event_queue 框架**：`event_watch_fs` 向已有事件队列注册文件系统关注。
- 目标实现要求变更事件通过 `event_wait` 返回 `a20_pending_event_t`。当前实现仅把 `dir` 注册为普通 watch 目标，尚未实现路径过滤与 VFS 事件源，因此本结构体目前是最小占位契约。
- 取消关注使用 `event_cancel(queue, dir)`；当前没有独立 `out_watch` handle。

---

## 22. Sync 结构体

futex 是**用户地址上的同步原语，不是内核对象**，因此不分配 handle、不携带 rights。这与 Zircon `zx_futex_wait`/`zx_futex_wake` 的定位一致：快速路径是纯用户态原子操作，只有竞争路径才进入内核睡眠。

设计说明：
- 早期草案（startup.md §4.4.4）曾考虑用 event_queue 承担互斥等待，但 event_queue 缺少"投递事件到队列"的用户语义，且每个竞争锁都需要一个内核 handle，成本与语义都不合适。Sync 分区因此回归地址型 futex。
- 快速路径（无竞争）与 Linux futex 一样快：纯用户态 CAS。
- 慢路径通过 `futex_wait`/`futex_futex_wake` 进入内核，复用内核 futex 等待表。
- 跨进程共享内存（`vm_share` 导出的 VMO）上的 futex 字同样有效：等待键同时匹配虚拟地址与物理页。

```c
#define A20_TIMEOUT_INFINITE  ((uint64_t)-1)  /* futex_wait 无限等待 */

typedef struct a20_futex_wait_args {
    uint32_t size;
    uint32_t version;
    uint64_t addr;          /* 用户态 32 位 futex 字地址，必须 4 字节对齐 */
    uint32_t expected;      /* 期望值；*addr != expected 时立即返回 A20_ERR_WOULD_BLOCK */
    uint32_t flags;         /* 保留，必须为 0 */
    uint64_t timeout_ns;    /* 相对超时（纳秒），A20_TIMEOUT_INFINITE 表示无限等待 */
} a20_futex_wait_args_t;

typedef struct a20_futex_wake_args {
    uint32_t size;
    uint32_t version;
    uint64_t addr;          /* 用户态 32 位 futex 字地址 */
    uint32_t count;         /* 最多唤醒的等待者数量，必须 >= 1 */
    uint32_t flags;         /* 保留，必须为 0 */
    uint32_t out_woken;     /* 输出：实际唤醒数量 */
    uint32_t reserved;
} a20_futex_wake_args_t;
```

错误映射：`A20_ERR_WOULD_BLOCK`（值不匹配）、`A20_ERR_TIMED_OUT`（超时）、`A20_ERR_FAULT`（地址无效）、`A20_ERR_INVALID_ARGUMENT`（对齐/flag 错误）、`A20_ERR_INTERRUPTED`（被中断）。

能力发现：`abi_info.feature_bits[0]` 的 bit 2 表示 Sync (0x0B00) 分区可用。
