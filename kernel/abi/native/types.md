# A20OS Native ABI：类型与结构体定义

> 本文档定义 A20OS Native ABI 的所有用户可见类型和结构体。权限语义见 [security.md](security.md)，Handle 生命周期见 [handle.md](handle.md)。

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

1. 用户传入的 `size` 小于内核支持结构体大小时，缺失字段按 0 处理。
2. 用户传入的 `size` 大于内核支持结构体大小时，内核只读取已知字段。
3. 新字段只能追加，不能改变已有字段含义。
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
    a20_rights_t task_rights;    /* 新进程的初始权限 */
    a20_handle_t out_task;       /* 输出：新进程的 task handle */
    uint32_t reserved;
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
    a20_handle_t source;         /* 源对象 handle（file/shm/device） */
    uint32_t prot;               /* 保护标志 */
    uint32_t flags;              /* 映射标志 */
    uint64_t offset;             /* 源对象内偏移 */
    uint64_t length;             /* 映射长度 */
    uint64_t addr_hint;          /* 建议地址 */
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
    uint32_t flags;
    uint32_t reserved;
    a20_handle_t out_memory;     /* 输出：共享内存对象 handle */
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
    uint32_t object_type;        /* A20_OBJ_FILE 或 A20_OBJ_DIRECTORY */
    uint32_t mode;               /* 文件模式 */
    uint64_t length;             /* 文件大小 */
    uint64_t block_size;         /* 块大小 */
    uint64_t blocks;             /* 块数 */
    uint64_t create_time_ns;     /* 创建时间 */
    uint64_t modify_time_ns;     /* 修改时间 */
    uint64_t access_time_ns;     /* 访问时间 */
    uint64_t change_time_ns;     /* 元数据变更时间 */
    uint64_t fs_id;              /* 文件系统标识 */
    uint64_t inode_hint;         /* inode 号（仅调试用） */
} a20_stat_t;
```

---

## 8. Event / IPC 结构体

### a20_event_queue_create_args_t — 创建事件队列

```c
typedef struct a20_event_queue_create_args {
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t capacity_hint;      /* 容量提示 */
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
    uint64_t events;             /* 关注的事件位图 */
    uint64_t user_data;          /* 用户关联数据 */
    uint32_t flags;
    uint32_t reserved;
} a20_event_watch_args_t;
```

### a20_event_t / a20_event_wait_args_t — 等待事件

```c
typedef struct a20_event {
    uint32_t size;
    uint32_t version;
    a20_handle_t source;         /* 产生事件的 handle */
    uint32_t type;               /* 事件类型 */
    uint64_t events;             /* 触发的事件位图 */
    uint64_t user_data;          /* 注册时的 user_data */
    uint64_t data0;              /* 事件相关数据 */
    uint64_t data1;
    uint64_t data2;
} a20_event_t;

typedef struct a20_event_wait_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t queue;          /* 事件队列 handle */
    uint32_t flags;              /* 等待标志 */
    uint64_t events;             /* a20_event_t[] 输出缓冲区 */
    uint32_t max_events;         /* 缓冲区容量 */
    uint32_t out_count;          /* 输出：实际事件数 */
    uint64_t timeout_ns;         /* 超时（纳秒），0 表示不等待 */
} a20_event_wait_args_t;
```

### a20_msg_send_args_t / a20_msg_recv_args_t — 消息通道

```c
typedef struct a20_msg_send_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t channel;        /* 通道端点 handle */
    uint32_t flags;
    uint64_t bytes;              /* 消息字节缓冲区 */
    uint64_t byte_count;         /* 字节数 */
    uint64_t handles;            /* a20_handle_t[]：要传递的 handle */
    uint32_t handle_count;       /* 传递的 handle 数量 */
    uint32_t reserved;
} a20_msg_send_args_t;

typedef struct a20_msg_recv_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t channel;        /* 通道端点 handle */
    uint32_t flags;
    uint64_t bytes;              /* 接收缓冲区 */
    uint64_t byte_capacity;      /* 缓冲区容量 */
    uint64_t out_byte_count;     /* 输出：实际接收字节数 */
    uint64_t handles;            /* a20_handle_t[]：接收 handle 缓冲区 */
    uint32_t handle_capacity;    /* handle 缓冲区容量 */
    uint32_t out_handle_count;   /* 输出：实际接收 handle 数 */
} a20_msg_recv_args_t;
```

---

## 9. Network 结构体

### a20_net_socket_args_t — 创建套接字

```c
typedef struct a20_net_socket_args {
    uint32_t size;
    uint32_t version;
    uint32_t domain;             /* 地址族（AF_INET 等） */
    uint32_t type;               /* 套接字类型 */
    uint32_t protocol;           /* 协议 */
    uint32_t flags;
    a20_rights_t rights;         /* 请求的权限 */
    a20_handle_t out_socket;     /* 输出：套接字 handle */
} a20_net_socket_args_t;
```

### a20_net_addr_t — 网络地址

```c
typedef struct a20_net_addr {
    uint32_t size;
    uint32_t version;
    uint32_t family;             /* 地址族 */
    uint32_t flags;
    uint8_t  data[128];          /* 地址数据（足够容纳任何地址族） */
} a20_net_addr_t;
```

不直接承诺 Linux `sockaddr` 布局，通过 `family` 和 `size` 做版本化。

---

## 10. Timer 结构体

### a20_timer_create_args_t — 创建定时器

```c
typedef struct a20_timer_create_args {
    uint32_t size;
    uint32_t version;
    uint32_t clock_id;           /* 时钟源（monotonic/realtime/boottime） */
    uint32_t flags;
    uint64_t deadline_ns;        /* 首次到期时间 */
    uint64_t interval_ns;        /* 周期间隔，0 表示单次 */
    a20_handle_t event_queue;    /* 事件队列 handle（到期事件投递目标） */
    uint64_t user_data;          /* 事件关联数据 */
    a20_handle_t out_timer;      /* 输出：定时器 handle */
} a20_timer_create_args_t;
```

Timer 是 handle，可被 event queue watch，不需要复制 POSIX timer id + signal delivery 模型。

---

## 11. Extended I/O 结构体

### a20_transfer_args_t — 零拷贝传输

统一 splice / sendfile / copy_file_range / tee 语义。通过 `flags` 区分传输模式。

```c
/* 传输标志 */
#define A20_TRANSFER_CONSUME    0x0000u   /* 默认：消耗源偏移 */
#define A20_TRANSFER_PEEK       0x0001u   /* 不推进源偏移（tee 语义） */
#define A20_TRANSFER_KERNEL_BUF 0x0002u   /* 允许内核内部缓冲 */
#define A20_TRANSFER_FORCE_SYNC 0x0004u   /* 强制同步完成，不做异步 */

typedef struct a20_transfer_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t src;             /* 源 handle（文件/pipe/socket） */
    a20_handle_t dst;             /* 目标 handle（文件/pipe/socket） */
    uint64_t src_offset;          /* 源偏移（若 src 支持偏移），-1 表示当前位置 */
    uint64_t dst_offset;          /* 目标偏移（若 dst 支持偏移），-1 表示当前位置 */
    uint64_t length;              /* 传输字节数 */
    uint32_t flags;               /* A20_TRANSFER_* */
    uint32_t reserved;
    uint64_t out_transferred;     /* 输出：实际传输字节数 */
} a20_transfer_args_t;
```

设计说明：
- Linux 的 `splice`、`sendfile`、`copy_file_range`、`tee` 是四个独立 syscall，但核心语义相同：在两个 handle 之间零拷贝传输数据。
- A20 统一为一个 `handle_transfer`，通过 flags 区分模式。
- `src_offset`/`dst_offset` 为 `-1`（`UINT64_MAX`）时使用当前位置（类似 `lseek` + `read`）。
- 权限检查：`src` 需要 `READ`，`dst` 需要 `WRITE`。

---

## 12. Metadata 结构体

### a20_set_meta_args_t — 文件元数据修改

统一 chmod / chown / utimes 语义。通过 flags 指定要修改的字段。

```c
/* 元数据修改标志 */
#define A20_SET_META_MODE       0x0001u   /* 修改 mode（chmod） */
#define A20_SET_META_OWNER      0x0002u   /* 修改 uid/gid（chown） */
#define A20_SET_META_ATIME      0x0004u   /* 修改访问时间 */
#define A20_SET_META_MTIME      0x0008u   /* 修改修改时间 */
#define A20_SET_META_ATIME_NOW  0x0010u   /* atime 设为当前时间 */
#define A20_SET_META_MTIME_NOW  0x0020u   /* mtime 设为当前时间 */
#define A20_SET_META_TRUNCATE   0x0040u   /* 截断文件到指定大小（ftruncate） */
#define A20_SET_META_ALLOCATE   0x0080u   /* 预分配空间（fallocate） */

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
    uint64_t new_size;            /* 截断/预分配大小 */
    int64_t  allocate_offset;     /* 预分配起始偏移，-1 表示文件末尾 */
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
    uint64_t name;                /* const char*：属性名 */
    uint64_t name_len;            /* 属性名长度，0 表示 nul-terminated */
    uint64_t value;               /* void*：值缓冲区 */
    uint64_t value_len;           /* 值大小（set 时为输入，get 时为缓冲区容量） */
    uint64_t out_value_len;       /* 输出：实际值大小（get/list 时有效） */
    uint32_t flags;               /* A20_XATTR_* */
    uint32_t reserved;
} a20_xattr_args_t;
```

### a20_xattr_list_args_t — 列出扩展属性

```c
typedef struct a20_xattr_list_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t handle;          /* 目标 handle */
    uint64_t buffer;              /* char*：输出缓冲区，包含 nul-terminated 的名称列表 */
    uint64_t buffer_len;          /* 缓冲区容量 */
    uint64_t out_size;            /* 输出：实际需要的总大小 */
    uint32_t reserved;
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
#define A20_SCHED_SET_POLICY    0x0001u
#define A20_SCHED_SET_PRIORITY  0x0002u
#define A20_SCHED_SET_AFFINITY  0x0004u
#define A20_SCHED_SET_NICE      0x0008u

typedef struct a20_sched_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t task;            /* 目标 task/thread handle */
    uint32_t flags;               /* 指定要设置/查询的字段 */
    uint32_t policy;              /* 调度策略 */
    int32_t  priority;            /* 静态优先级（1-99 for FIFO/RR） */
    int32_t  nice;                /* nice 值（-20..19 for OTHER） */
    uint64_t affinity;            /* CPU 亲和性位图 */
    uint32_t affinity_size;       /* 亲和性位图大小（字节） */
    uint32_t reserved;
} a20_sched_args_t;
```

设计说明：
- Linux 的 `sched_setparam`/`sched_getparam`/`sched_setscheduler`/`sched_getscheduler`/`sched_setaffinity`/`sched_getaffinity`/`setpriority`/`getpriority`/`sched_setattr`/`sched_getattr` 是 10 个独立 syscall。
- A20 统一为 `task_set_sched`/`task_get_sched` 两个 syscall，通过 flags 组合指定要操作的调度参数。
- `task_get_sched` 使用同一个 args struct，内核填充请求的字段。

---

## 15. Resource Limits 结构体

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
    uint32_t reserved;
    uint64_t cur;                 /* 当前软限制 */
    uint64_t max;                 /* 硬限制上限 */
} a20_rlimit_args_t;
```

### a20_rusage_t — 资源使用量

```c
typedef struct a20_rusage {
    uint32_t size;
    uint32_t version;
    uint64_t user_time_ns;        /* 用户态 CPU 时间 */
    uint64_t kernel_time_ns;      /* 内核态 CPU 时间 */
    uint64_t max_rss;             /* 最大驻内存集大小 */
    uint64_t minor_faults;        /* 次 page fault 数 */
    uint64_t major_faults;        /* 主 page fault 数 */
    uint64_t voluntary_cs;        /* 自愿上下文切换 */
    uint64_t involuntary_cs;      /* 非自愿上下文切换 */
    uint64_t io_read_bytes;       /* I/O 读取字节数 */
    uint64_t io_write_bytes;      /* I/O 写入字节数 */
} a20_rusage_t;
```

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
    uint64_t new_size;            /* 新大小 */
    uint64_t new_addr;            /* 建议新地址（REMAP_EXACT 时为强制地址） */
    uint32_t flags;               /* A20_REMAP_* */
    uint32_t reserved;
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
    uint64_t initial_size;        /* 初始大小 */
    uint32_t flags;               /* A20_VM_OBJ_* */
    uint32_t reserved;
    a20_rights_t rights;          /* 请求的权限 */
    uint64_t name;                /* const char*：可选名称（调试用） */
    uint64_t name_len;
    a20_handle_t out_handle;      /* 输出：内存对象 handle */
} a20_vm_object_args_t;
```

创建的内存对象可通过 `vm_map` 映射到地址空间，通过 `handle_dup` 分享给其他进程，通过 `handle_set_meta`（`TRUNCATE` flag）调整大小。

---

## 17. Extended Filesystem 结构体

### a20_path_link_args_t — 创建硬链接

```c
typedef struct a20_path_link_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t src_dir;         /* 源文件所在目录 handle */
    uint64_t src_path;            /* const char*：源文件路径 */
    uint64_t src_path_len;
    a20_handle_t dst_dir;         /* 目标目录 handle */
    uint64_t dst_path;            /* const char*：链接路径 */
    uint64_t dst_path_len;
    uint32_t flags;               /* 保留 */
    uint32_t reserved;
} a20_path_link_args_t;
```

### a20_path_symlink_args_t — 创建符号链接

```c
typedef struct a20_path_symlink_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t dir;             /* 创建链接的父目录 handle */
    uint64_t link_path;           /* const char*：链接路径 */
    uint64_t link_path_len;
    uint64_t target;              /* const char*：链接目标 */
    uint64_t target_len;
    uint32_t reserved;
} a20_path_symlink_args_t;
```

### a20_path_readlink_args_t — 读取符号链接

```c
typedef struct a20_path_readlink_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t dir;             /* 父目录 handle */
    uint64_t path;                /* const char*：链接路径 */
    uint64_t path_len;
    uint64_t buffer;              /* char*：输出缓冲区 */
    uint64_t buffer_len;          /* 缓冲区容量 */
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
    a20_handle_t dir;             /* 挂载点目录 handle */
    uint64_t source;              /* const char*：设备/源路径 */
    uint64_t source_len;
    uint64_t fs_type;             /* const char*：文件系统类型名 */
    uint64_t fs_type_len;
    uint32_t flags;               /* A20_MOUNT_* */
    uint32_t reserved;
    uint64_t options;             /* const char*：挂载选项 */
    uint64_t options_len;
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

### net_getname flags

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
    uint32_t flags;               /* A20_SEC_SET_*（set 时有效） */
    uint32_t reserved;
    /* POSIX 兼容身份 */
    uint32_t uid;
    uint32_t gid;
    uint32_t euid;
    uint32_t egid;
    uint32_t saved_uid;
    uint32_t saved_gid;
    uint64_t groups;              /* uint32_t*：补充组 ID 数组 */
    uint32_t group_count;
    uint32_t reserved2;
    uint64_t posix_caps;          /* POSIX capability 位图 */
    /* A20 原生 */
    a20_rights_t effective_rights; /* 当前有效 rights 集合 */
    a20_rights_t permitted_rights; /* 允许提升到的 rights 集合 */
    uint64_t namespace_mask;      /* 所属 namespace 掩码 */
} a20_security_context_t;
```

设计说明：
- `security_get_context` 查询当前完整的身份和权限状态。
- `security_set_context` 只修改 `flags` 指定的字段（类似 `setuid`/`setgid`/`setgroups` 的统一接口）。
- 修改 uid/gid 需要对应的 POSIX capability 或 A20 rights。
- A20 原生字段（effective_rights, namespace_mask）是只读的，由内核根据 handle 权限和 namespace 推导。

---

## 20. System 结构体

### a20_system_info_t — 系统信息

统一 uname / sysinfo 语义。

```c
typedef struct a20_system_info {
    uint32_t size;
    uint32_t version;
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
    uint16_t procs;               /* 进程数 */
    uint16_t reserved;
    uint64_t uptime_ns;           /* 系统运行时间（纳秒） */
    uint32_t page_size;           /* 页大小 */
    uint32_t num_cpus;            /* CPU 数量 */
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
    uint64_t path_len;
    uint64_t events;              /* A20_FS_EVENT_* 组合 */
    uint64_t user_data;           /* 事件关联数据 */
    uint32_t flags;               /* 保留 */
    uint32_t reserved;
    a20_handle_t out_watch;       /* 输出：watch handle（用于取消） */
} a20_event_watch_fs_args_t;
```

设计说明：
- Linux 的 `inotify_init`/`inotify_add_watch`/`inotify_rm_watch` 是独立于 epoll 的子系统。
- A20 将文件系统事件**统一纳入现有 event_queue 框架**：`event_watch_fs` 向已有事件队列注册文件系统关注。
- 变更事件通过 `event_wait` 返回标准的 `a20_event_t`，无需新的等待机制。
- 取消关注：`handle_close(out_watch)` 即可。
