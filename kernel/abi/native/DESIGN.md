# A20 Native ABI Design

本文档定义 A20OS 的 native ABI 设计方向。它不是当前 Linux 兼容 ABI 的替代品，也不应影响 `kernel/abi/linux` 的稳定运行。A20 native ABI 的目标是提供一套现代、清晰、可扩展、易于实现和维护的用户态接口，为未来的 A20OS 原生用户态、实验性运行时、语言运行时和安全模型打基础。

## 设计定位

`abi/linux` 的目标是兼容现有 Linux 用户态生态，因此必须承担大量历史包袱：`fork/clone`、`ioctl`、`fcntl`、`stat` 结构演进、uid/gid 权限、信号、文件描述符、各种特殊 fd、`epoll`、`timerfd`、`eventfd` 等。

`abi/native` 的目标不同：

1. 不追求兼容 Linux syscall。
2. 不复刻 POSIX 历史接口。
3. 不要求现有 musl/glibc 程序直接运行。
4. 以 handle/capability 作为统一资源模型。
5. 从第一天设计版本协商、异步事件、权限降级、结构体扩展和 ABI 稳定策略。
6. 让 syscall 数量少、语义直观、参数结构稳定。
7. 让内核实现能保持清晰，不被兼容层反向污染。

推荐长期布局：

```text
kernel/abi/linux/    Linux-compatible ABI subset
kernel/abi/native/   A20OS native ABI
```

Linux ABI 继续作为主用户态接口。Native ABI 先作为实验性接口，等内核内部 API 更稳定后再逐步实现。

## 核心原则

### 1. 一切资源都是 handle

Native ABI 不区分 Linux 风格的 fd、pid、tid、timerid、shmid、epoll fd 等多种编号。所有可操作资源都由进程本地 handle table 引用。

```c
typedef uint32_t a20_handle_t;
```

典型 handle 类型：

- task
- thread
- process group
- file
- directory
- socket
- pipe endpoint
- event queue
- timer
- shared memory object
- memory mapping object
- device
- namespace
- debug object

handle 是进程本地编号，不是全局对象 ID。不同进程中的同一个数字不代表同一个对象。

### 2. handle 带 capability rights

每个 handle 都携带权限位。操作对象时不仅检查对象本身权限，也检查当前 handle 是否具备对应 capability。

示例 rights：

```c
#define A20_RIGHT_READ       (1ull << 0)
#define A20_RIGHT_WRITE      (1ull << 1)
#define A20_RIGHT_EXEC       (1ull << 2)
#define A20_RIGHT_STAT       (1ull << 3)
#define A20_RIGHT_SEEK       (1ull << 4)
#define A20_RIGHT_DUP        (1ull << 5)
#define A20_RIGHT_TRANSFER   (1ull << 6)
#define A20_RIGHT_MAP        (1ull << 7)
#define A20_RIGHT_SIGNAL     (1ull << 8)
#define A20_RIGHT_WAIT       (1ull << 9)
#define A20_RIGHT_CONNECT    (1ull << 10)
#define A20_RIGHT_ACCEPT     (1ull << 11)
#define A20_RIGHT_CONTROL    (1ull << 12)
#define A20_RIGHT_ADMIN      (1ull << 13)
```

权限只能降级，不能通过 `dup` 升级：

```text
new_rights must be subset of old_rights
```

这使 native ABI 可以表达 sandbox、最小权限、对象转交和服务化架构。

### 3. syscall 使用稳定结构体

为了 ABI 可扩展，复杂 syscall 不直接传一串裸参数，而是传结构体指针。所有结构体以 `size` 和 `version` 开头。

```c
typedef struct a20_abi_header {
    uint32_t size;
    uint32_t version;
} a20_abi_header_t;
```

规则：

1. 用户传入的 `size` 小于内核支持结构体大小时，缺失字段按 0 处理。
2. 用户传入的 `size` 大于内核支持结构体大小时，内核只读取已知字段。
3. 新字段只能追加，不能改变已有字段含义。
4. flag 保留位必须为 0，否则返回 `A20_ERR_INVALID_ARGUMENT`。

### 4. ABI 必须可版本协商

第一个核心 syscall 是 ABI 查询：

```c
int64_t a20_abi_info(a20_abi_info_t *out);
```

```c
typedef struct a20_abi_info {
    uint32_t size;
    uint32_t version;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    uint32_t pointer_bits;
    uint32_t page_size;
    uint32_t handle_bits;
    uint64_t feature_bits[4];
    uint64_t syscall_bitmap_addr;
    uint64_t syscall_bitmap_size;
} a20_abi_info_t;
```

版本规则：

- `abi_major` 改变表示不兼容变更。
- `abi_minor` 增加表示向后兼容新增功能。
- `abi_patch` 只表示 bugfix，不改变 ABI 表面。
- feature bits 用于检测可选能力。

### 5. syscall 编号分区

Native ABI syscall 编号按子系统分区，便于扩展和阅读。

```text
0x0000 - 0x00ff  core / abi / system
0x0100 - 0x01ff  handle
0x0200 - 0x02ff  task / thread
0x0300 - 0x03ff  memory
0x0400 - 0x04ff  path / filesystem
0x0500 - 0x05ff  ipc / event
0x0600 - 0x06ff  net
0x0700 - 0x07ff  time
0x0800 - 0x08ff  security / namespace
0x0900 - 0x09ff  debug / trace
0x0a00 - 0x0fff  reserved for future core extensions
0x1000 - 0x1fff  experimental, not stable
```

稳定 syscall 不允许随意改号。实验 syscall 只能在 `0x1000+` 范围内。

## 基础类型

Native ABI 固定使用小端、补码整数、显式宽度类型。

```c
typedef uint32_t a20_handle_t;
typedef uint64_t a20_rights_t;
typedef uint64_t a20_flags_t;
typedef int64_t  a20_status_t;
typedef uint64_t a20_time_ns_t;
typedef uint64_t a20_off_t;
typedef uint64_t a20_size_t;
typedef uint64_t a20_vaddr_t;
```

指针大小由 `a20_abi_info.pointer_bits` 指示。64 位架构上 native ABI 首选 64 位用户指针。若未来支持 32 位用户态，应视为单独 ABI profile。

## 错误模型

Native ABI 使用负数表示错误，非负数表示成功返回值。

```c
#define A20_OK                       0
#define A20_ERR_PERM                 1
#define A20_ERR_NO_ENTRY             2
#define A20_ERR_INTERRUPTED          3
#define A20_ERR_IO                   4
#define A20_ERR_BAD_HANDLE           5
#define A20_ERR_NO_MEMORY            6
#define A20_ERR_ACCESS               7
#define A20_ERR_FAULT                8
#define A20_ERR_BUSY                 9
#define A20_ERR_EXISTS               10
#define A20_ERR_NOT_SUPPORTED        11
#define A20_ERR_INVALID_ARGUMENT     12
#define A20_ERR_NO_SPACE             13
#define A20_ERR_NOT_DIR              14
#define A20_ERR_IS_DIR               15
#define A20_ERR_NOT_EMPTY            16
#define A20_ERR_NAME_TOO_LONG        17
#define A20_ERR_WOULD_BLOCK          18
#define A20_ERR_TIMED_OUT            19
#define A20_ERR_CANCELED             20
#define A20_ERR_PROTOCOL             21
#define A20_ERR_RANGE                22
```

返回约定：

```text
>= 0  success
<  0  -A20_ERR_*
```

Native ABI 的错误码不要求等于 Linux errno。兼容层可以在 libc 或 shim 中映射到 POSIX errno。

## 用户态启动协议

Native 程序不直接继承 Linux auxv 语义。内核在初始用户栈或只读启动信息页提供 `a20_start_info_t`。

```c
typedef struct a20_start_info {
    uint32_t size;
    uint32_t version;

    uint32_t argc;
    uint32_t envc;
    uint32_t auxc;
    uint32_t reserved0;

    uint64_t argv;        /* user pointer to char*[] */
    uint64_t envp;        /* user pointer to char*[] */
    uint64_t auxv;        /* user pointer to a20_auxv[] */

    a20_handle_t root_dir;
    a20_handle_t cwd_dir;
    a20_handle_t stdin_handle;
    a20_handle_t stdout_handle;
    a20_handle_t stderr_handle;
    a20_handle_t self_task;
    a20_handle_t main_thread;
    a20_handle_t default_event_queue;

    uint64_t page_size;
    uint64_t user_clock_freq;
} a20_start_info_t;
```

启动参数里直接提供常用 handle，而不是依赖固定 fd 0/1/2 的特殊语义。不过 native libc 可以把 `stdin/stdout/stderr` 映射成自己的 fd 表。

## Handle 子系统

### handle_close

```c
int64_t handle_close(a20_handle_t handle);
```

关闭当前进程 handle table 中的 handle。若这是对象最后一个引用，内核释放对象。

### handle_dup

```c
typedef struct a20_handle_dup_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t source;
    uint32_t flags;
    a20_rights_t rights_mask;
    a20_handle_t out_handle;
    uint32_t reserved;
} a20_handle_dup_args_t;

int64_t handle_dup(a20_handle_dup_args_t *args);
```

`rights_mask` 必须是原 handle rights 的子集。

### handle_query

```c
typedef struct a20_handle_info {
    uint32_t size;
    uint32_t version;
    uint32_t object_type;
    uint32_t state;
    a20_rights_t rights;
    uint64_t object_id_hint;
    uint64_t flags;
} a20_handle_info_t;

int64_t handle_query(a20_handle_t handle, a20_handle_info_t *out);
```

`object_id_hint` 仅用于调试，不保证全局稳定。

### handle_transfer

Handle transfer 是进程间 capability 传递的基础。可以通过 event/message IPC 传递 handle，接收方得到新 handle，rights 可进一步降级。

不建议设计全局 `send_handle(pid, handle)`。更推荐通过明确的 channel/socket/message queue 传递。

## Task / Thread 子系统

Native ABI 不复制 Linux `fork`/`clone` 语义。进程创建以 image handle 和 spawn args 为核心。

### task_spawn

```c
typedef struct a20_spawn_handle {
    a20_handle_t handle;
    a20_rights_t rights;
    uint32_t target_slot;  /* optional, 0 means auto */
    uint32_t flags;
} a20_spawn_handle_t;

typedef struct a20_task_spawn_args {
    uint32_t size;
    uint32_t version;

    a20_handle_t image;       /* executable file or image object */
    a20_handle_t root_dir;
    a20_handle_t cwd_dir;
    a20_handle_t event_queue;

    uint64_t argv;            /* char*[] */
    uint64_t envp;            /* char*[] */
    uint32_t argc;
    uint32_t envc;

    uint64_t handles;         /* a20_spawn_handle_t[] */
    uint32_t handle_count;
    uint32_t flags;

    a20_rights_t task_rights;
    a20_handle_t out_task;
    uint32_t reserved;
} a20_task_spawn_args_t;

int64_t task_spawn(a20_task_spawn_args_t *args);
```

设计意图：

- 不用 `fork` 复制整个地址空间。
- 明确传入 root、cwd、stdio 和其他 handles。
- 支持权限降级和 sandbox。
- 支持服务化进程创建。

### thread_create

```c
typedef struct a20_thread_create_args {
    uint32_t size;
    uint32_t version;
    uint64_t entry;
    uint64_t arg;
    uint64_t stack_base;
    uint64_t stack_size;
    uint64_t tls_base;
    uint32_t flags;
    uint32_t reserved;
    a20_handle_t out_thread;
} a20_thread_create_args_t;

int64_t thread_create(a20_thread_create_args_t *args);
```

线程共享当前 task 的地址空间和 handle table，除非未来明确支持独立 namespace。

### task_wait

```c
typedef struct a20_task_status {
    uint32_t size;
    uint32_t version;
    uint32_t reason;
    int32_t  exit_code;
    uint64_t user_time_ns;
    uint64_t kernel_time_ns;
} a20_task_status_t;

int64_t task_wait(a20_handle_t task, a20_flags_t flags, a20_task_status_t *out);
```

`task_wait` 只等待持有 `A20_RIGHT_WAIT` 的 task handle。没有 Linux 风格的 `waitpid(-1)` 隐式全局扫描。需要等待多个子进程时使用 event queue。

### task_exit / thread_exit

```c
void task_exit(int32_t code);
void thread_exit(int32_t code);
```

`task_exit` 结束整个 task。`thread_exit` 只结束当前线程；若最后一个线程退出，task 退出。

## Memory 子系统

Native ABI 将匿名内存、文件映射、共享内存统一到 mapping 模型。

### vm_alloc

```c
typedef struct a20_vm_alloc_args {
    uint32_t size;
    uint32_t version;
    uint64_t addr_hint;
    uint64_t length;
    uint32_t prot;
    uint32_t flags;
    uint64_t out_addr;
} a20_vm_alloc_args_t;

int64_t vm_alloc(a20_vm_alloc_args_t *args);
```

### vm_map

```c
typedef struct a20_vm_map_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t source;
    uint32_t prot;
    uint32_t flags;
    uint64_t offset;
    uint64_t length;
    uint64_t addr_hint;
    uint64_t out_addr;
} a20_vm_map_args_t;

int64_t vm_map(a20_vm_map_args_t *args);
```

`source` 可以是 file、shared memory object、device memory object 等。

### vm_unmap / vm_protect

```c
int64_t vm_unmap(uint64_t addr, uint64_t length);
int64_t vm_protect(uint64_t addr, uint64_t length, uint32_t prot);
```

### vm_share

```c
typedef struct a20_vm_share_args {
    uint32_t size;
    uint32_t version;
    uint64_t addr;
    uint64_t length;
    a20_rights_t rights;
    uint32_t flags;
    uint32_t reserved;
    a20_handle_t out_memory;
} a20_vm_share_args_t;

int64_t vm_share(a20_vm_share_args_t *args);
```

将当前地址空间的一段内存导出为 memory object handle，可传递给其他 task。

## Filesystem / Path 子系统

Native ABI 只保留最小 path API。打开后通过 handle 操作。

### path_open

```c
typedef struct a20_path_open_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t dir;
    uint32_t flags;
    a20_rights_t rights;
    uint64_t path;      /* const char* */
    uint64_t path_len;  /* optional, 0 means nul-terminated */
    uint32_t mode;
    uint32_t reserved;
    a20_handle_t out_handle;
} a20_path_open_args_t;

int64_t path_open(a20_path_open_args_t *args);
```

`dir` 可以是 root、cwd、目录 handle。绝对路径是否允许由 flags 和 namespace policy 决定。

### path_create_node

用于创建目录、普通文件、socket node、device node 等。不要复刻 Linux `mknodat` 的混合历史语义。

```c
typedef struct a20_path_create_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t dir;
    uint32_t node_type;
    uint64_t path;
    uint64_t path_len;
    uint32_t mode;
    uint32_t flags;
    a20_rights_t rights;
    a20_handle_t out_handle;
} a20_path_create_args_t;

int64_t path_create(a20_path_create_args_t *args);
```

### handle_read / handle_write

```c
typedef struct a20_iovec {
    uint64_t base;
    uint64_t len;
} a20_iovec_t;

typedef struct a20_io_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t handle;
    uint32_t flags;
    uint64_t iov;
    uint32_t iov_count;
    uint32_t reserved;
    uint64_t offset;
    uint64_t out_count;
} a20_io_args_t;

int64_t handle_read(a20_io_args_t *args);
int64_t handle_write(a20_io_args_t *args);
```

`offset` 可支持特殊值 `A20_OFFSET_CURRENT`。这样不需要单独的 `pread/pwrite/readv/writev` 家族。

### handle_stat

```c
typedef struct a20_stat {
    uint32_t size;
    uint32_t version;
    uint32_t object_type;
    uint32_t mode;
    uint64_t length;
    uint64_t block_size;
    uint64_t blocks;
    uint64_t create_time_ns;
    uint64_t modify_time_ns;
    uint64_t access_time_ns;
    uint64_t change_time_ns;
    uint64_t fs_id;
    uint64_t inode_hint;
} a20_stat_t;

int64_t handle_stat(a20_handle_t handle, a20_stat_t *out);
```

`inode_hint` 只作为兼容和调试信息，不作为 capability。

### handle_control

Native ABI 应避免 Linux 式无类型 `ioctl`，但仍需要对象特定控制通道。

```c
typedef struct a20_control_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t handle;
    uint32_t namespace_id;
    uint32_t command;
    uint64_t in_ptr;
    uint64_t in_size;
    uint64_t out_ptr;
    uint64_t out_size;
    uint64_t out_actual;
} a20_control_args_t;

int64_t handle_control(a20_control_args_t *args);
```

规则：

1. `namespace_id` 表明命令属于哪个对象协议。
2. 命令结构必须版本化。
3. 通用操作应优先设计成明确 syscall，不滥用 control。

## Event / IPC 子系统

Native ABI 不后补 `select/poll/epoll`。事件队列是基础对象。

### event_queue_create

```c
typedef struct a20_event_queue_create_args {
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t capacity_hint;
    a20_handle_t out_queue;
} a20_event_queue_create_args_t;

int64_t event_queue_create(a20_event_queue_create_args_t *args);
```

### event_watch

```c
typedef struct a20_event_watch_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t queue;
    a20_handle_t target;
    uint64_t events;
    uint64_t user_data;
    uint32_t flags;
    uint32_t reserved;
} a20_event_watch_args_t;

int64_t event_watch(a20_event_watch_args_t *args);
```

### event_wait

```c
typedef struct a20_event {
    uint32_t size;
    uint32_t version;
    a20_handle_t source;
    uint32_t type;
    uint64_t events;
    uint64_t user_data;
    uint64_t data0;
    uint64_t data1;
    uint64_t data2;
} a20_event_t;

typedef struct a20_event_wait_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t queue;
    uint32_t flags;
    uint64_t events;       /* a20_event_t[] */
    uint32_t max_events;
    uint32_t out_count;
    uint64_t timeout_ns;
} a20_event_wait_args_t;

int64_t event_wait(a20_event_wait_args_t *args);
```

可观察事件：

- handle readable
- handle writable
- connection accepted
- connection closed
- timer expired
- task exited
- thread exited
- signal-like notification
- message arrived
- filesystem notification

### message channel

长期建议提供 typed message channel，支持 handle transfer。

```c
typedef struct a20_msg_send_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t channel;
    uint32_t flags;
    uint64_t bytes;
    uint64_t byte_count;
    uint64_t handles;       /* a20_handle_t[] */
    uint32_t handle_count;
    uint32_t reserved;
} a20_msg_send_args_t;

typedef struct a20_msg_recv_args {
    uint32_t size;
    uint32_t version;
    a20_handle_t channel;
    uint32_t flags;
    uint64_t bytes;
    uint64_t byte_capacity;
    uint64_t out_byte_count;
    uint64_t handles;
    uint32_t handle_capacity;
    uint32_t out_handle_count;
} a20_msg_recv_args_t;
```

Handle transfer 必须遵守 rights 降级规则。

## Network 子系统

网络也应 handle 化。Socket 是 handle 的一种。

### net_socket

```c
typedef struct a20_net_socket_args {
    uint32_t size;
    uint32_t version;
    uint32_t domain;
    uint32_t type;
    uint32_t protocol;
    uint32_t flags;
    a20_rights_t rights;
    a20_handle_t out_socket;
} a20_net_socket_args_t;

int64_t net_socket(a20_net_socket_args_t *args);
```

### net_bind / net_connect / net_accept

地址参数使用 typed sockaddr，不直接承诺 Linux sockaddr 布局。

```c
typedef struct a20_net_addr {
    uint32_t size;
    uint32_t version;
    uint32_t family;
    uint32_t flags;
    uint8_t  data[128];
} a20_net_addr_t;
```

```c
int64_t net_bind(a20_handle_t socket, const a20_net_addr_t *addr);
int64_t net_connect(a20_handle_t socket, const a20_net_addr_t *addr);
int64_t net_accept(a20_handle_t listener, a20_flags_t flags, a20_handle_t *out_socket);
```

读写仍使用 `handle_read` / `handle_write`。Datagram 或 message-oriented socket 可以使用 `handle_control` 或专用 `net_sendmsg/net_recvmsg`。

## Time 子系统

时间统一使用纳秒。

```c
typedef uint64_t a20_time_ns_t;
```

### clock_get

```c
int64_t clock_get(uint32_t clock_id, a20_time_ns_t *out);
```

标准 clock：

- monotonic
- realtime
- boottime
- process CPU time
- thread CPU time

### timer_create

Timer 是 handle，可以被 event queue watch。

```c
typedef struct a20_timer_create_args {
    uint32_t size;
    uint32_t version;
    uint32_t clock_id;
    uint32_t flags;
    uint64_t deadline_ns;
    uint64_t interval_ns;
    a20_handle_t event_queue;
    uint64_t user_data;
    a20_handle_t out_timer;
} a20_timer_create_args_t;

int64_t timer_create(a20_timer_create_args_t *args);
```

不要复制 POSIX timer id 和 signal delivery 模型。

## Security / Namespace

Native ABI 应把 namespace 作为显式对象，而不是隐藏在进程全局状态里。

可选 namespace handle：

- filesystem namespace
- network namespace
- pid/task namespace
- device namespace
- security policy namespace

进程 spawn 时显式传入 namespace handles。若不传，继承父进程当前 namespace。

权限模型：

1. 文件系统权限可继续支持 uid/gid/mode，用于兼容和传统场景。
2. Native ABI 的核心授权以 handle rights 为主。
3. 高权限操作需要 `A20_RIGHT_ADMIN` 或具体 capability。
4. handle transfer 是权限委托的主要方式。

## Debug / Trace

Native ABI 应提供明确的调试对象，而不是把调试塞进 procfs。

可能接口：

- `debug_attach(task_handle)`
- `debug_read_regs(thread_handle)`
- `debug_write_regs(thread_handle)`
- `debug_map_memory(task_handle)`
- `trace_create()`
- `trace_subscribe(event_class)`

调试能力必须通过 handle rights 控制。

## 最小可实现原型

第一阶段 native ABI 不应追求完整。建议只实现这些 syscall，让一个极简 native init 可以启动、打印、退出。

```text
0x0000 abi_info
0x0100 handle_close
0x0101 handle_dup
0x0102 handle_query
0x0200 task_exit
0x0300 vm_alloc
0x0301 vm_unmap
0x0400 path_open
0x0401 handle_read
0x0402 handle_write
0x0403 handle_stat
0x0500 event_queue_create
0x0501 event_watch
0x0502 event_wait
0x0700 clock_get
```

最小 native init 需要：

1. 从 `a20_start_info` 拿到 stdout handle。
2. 调用 `handle_write(stdout, ...)` 打印。
3. 调用 `task_exit(0)` 退出。

第二阶段再支持：

- `task_spawn`
- `thread_create`
- `timer_create`
- message channel
- socket
- shared memory

## 与 Linux ABI 的关系

Native ABI 和 Linux ABI 可以共存。

推荐策略：

1. 内核内部模块只实现核心语义。
2. `abi/linux` 把 Linux syscall 转成核心模块 API。
3. `abi/native` 把 native syscall 转成同一批核心模块 API。
4. 不允许 `abi/native` 调用 `abi/linux` 的 syscall 实现。
5. 不允许核心模块依赖 `abi/linux` 或 `abi/native` 的用户结构体。

关系图：

```text
Linux userland          Native userland
      |                       |
      v                       v
  abi/linux              abi/native
      |                       |
      +----------+------------+
                 v
        kernel core modules
        mm / proc / vfs / net
```

## libc / runtime 设计

Native ABI 应配套一个很薄的 native runtime，而不是直接改 musl。

建议阶段：

1. `liba20rt`：只提供 syscall wrapper、启动代码、handle I/O。
2. `liba20c`：最小 C 库，支持 malloc、stdio、string、time。
3. POSIX shim：可选，将部分 POSIX API 映射到 native handle 模型。

不要一开始就承诺完整 POSIX。

## 兼容策略

Native ABI 一旦稳定，需要遵守：

1. syscall 编号不重用。
2. 结构体字段只追加。
3. flag 保留位必须检查。
4. 默认行为不能随意改变。
5. 不兼容变更只能增加 `abi_major`。
6. 实验 syscall 必须留在 experimental 编号区。

## 文档要求

实现 native ABI 前，应补齐这些文档：

```text
kernel/abi/native/DESIGN.md          本文档
kernel/abi/native/syscall_table.md   syscall 编号和状态
kernel/abi/native/types.h.md         用户可见类型说明
kernel/abi/native/startup.md         启动栈和 start info
kernel/abi/native/errors.md          错误码说明
kernel/abi/native/security.md        capability rights 和 transfer 规则
```

## 实现建议

推荐代码结构：

```text
kernel/abi/native/
  DESIGN.md
  syscall_table.c
  syscall_table.def
  syscall_impl.h
  sys_core.c
  sys_handle.c
  sys_task.c
  sys_memory.c
  sys_path.c
  sys_event.c
  sys_net.c
  sys_time.c

kernel/include/abi/native/
  types.h
  errno.h
  rights.h
  syscall_nr.h
  syscall_entry.h
  startup.h
```

不要急着接入 `Makefile`。先完成文档和最小 headers，再实现最小 syscall table。

## 明确不做的事

Native ABI 不应该：

- 完整复刻 POSIX。
- 复制 Linux syscall 编号。
- 复制 Linux `ioctl` 大杂烩。
- 复制 Linux `clone` 的 flag 组合复杂度。
- 强制使用 uid/gid 作为唯一安全模型。
- 让内核核心模块依赖 native 用户结构体。
- 为了短期兼容测试牺牲接口清晰度。

## 总结

A20 native ABI 应该是一套基于 handle/capability 的现代系统接口。它的价值不是替代 Linux 兼容层，而是给 A20OS 一个清晰、自洽、长期可演进的原生用户态契约。

短期最合理的路线是：

1. 保持 `abi/linux` 作为主 ABI。
2. 先文档化 native ABI。
3. 等核心模块 API 更稳定后，实现最小 native runtime。
4. 用 native init 验证启动、输出、退出。
5. 再逐步加入 spawn、event、IPC、network 和 sandbox。

