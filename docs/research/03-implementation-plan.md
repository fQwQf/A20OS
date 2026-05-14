# A20OS Native ABI 实现方案

> 本文为 A20OS Native ABI 理论研究的第三部分。基于 [01-posix-limitations.md](01-posix-limitations.md) 的问题分析和 [02-native-api-design.md](02-native-api-design.md) 的 API 设计，给出具体的实现方案，包括组件划分、文件组织、libc 设计和分阶段实施路线。

## 目录

1. [现有项目架构分析](#1-现有项目架构分析)
2. [Native ABI 内核侧组件设计](#2-native-abi-内核侧组件设计)
3. [核心模块改造需求](#3-核心模块改造需求)
4. [Build System 集成](#4-build-system-集成)
5. [用户态组件设计](#5-用户态组件设计)
6. [Native libc 设计详案](#6-native-libc-设计详案)
7. [分阶段实施路线](#7-分阶段实施路线)
8. [风险与缓解策略](#8-风险与缓解策略)

---

## 1. 现有项目架构分析

### 1.1 顶层目录结构

```
oskernel2025-a20/
├── kernel/              # 内核主体
│   ├── abi/             # ABI 层（用户态接口）
│   │   ├── linux/       # Linux 兼容 ABI（当前主 ABI）
│   │   └── native/      # A20 Native ABI（本文档目标）
│   ├── arch/            # 架构相关代码
│   │   ├── aarch64/
│   │   ├── loongarch64/
│   │   └── riscv64/
│   ├── bpf/             # BPF 支持
│   ├── core/            # 核心工具（klog, printf, trap, sync, timekeeping）
│   ├── drv/             # 驱动
│   ├── external/        # 外部依赖
│   ├── fs/              # 文件系统
│   │   ├── vfs.c        # VFS 核心
│   │   ├── file.c       # 文件操作
│   │   ├── fdtable.c    # fd 表管理
│   │   ├── ext4.c       # ext4 文件系统
│   │   ├── fat32.c      # FAT32 文件系统
│   │   ├── pipe.c       # 管道
│   │   ├── ramfs.c      # 内存文件系统
│   │   ├── devfs.c      # 设备文件系统
│   │   ├── procfs.c     # proc 文件系统
│   │   └── ...
│   ├── include/         # 内核头文件
│   │   ├── abi/         # ABI 相关头文件
│   │   │   ├── current.h        # ABI 选择器（CONFIG_ABI_LINUX）
│   │   │   ├── syscall_entry.h  # Syscall 入口选择器
│   │   │   └── linux/           # Linux ABI 头文件
│   │   ├── core/
│   │   ├── fs/
│   │   ├── mm/
│   │   ├── net/
│   │   ├── proc/
│   │   └── sys/
│   ├── ipc/             # IPC 子系统
│   ├── main.c           # 内核入口
│   ├── mm/              # 内存管理
│   │   ├── mm.c         # 内存管理核心
│   │   ├── vm.c         # 虚拟内存
│   │   ├── elf.c        # ELF 加载
│   │   ├── frame.c      # 物理帧管理
│   │   └── ...
│   ├── net/             # 网络栈
│   ├── proc/            # 进程管理
│   │   ├── proc.c       # 进程核心
│   │   ├── fork.c       # fork 实现
│   │   ├── exec.c       # exec 实现
│   │   ├── signal.c     # 信号处理
│   │   ├── sched.c      # 调度器
│   │   └── ...
│   └── syscall/         # 系统调用分派
│       ├── syscall.c    # 主分派器
│       ├── syscall_common.c
│       └── syscall_internal.h
├── user/                # 用户态
│   ├── cmds/            # 命令行工具
│   ├── lib/             # 用户态库
│   ├── shell/           # Shell
│   ├── external/        # 外部用户态软件（musl, gcc, vim 等）
│   └── Makefile
├── docs/                # 文档
├── Makefile             # 顶层构建
└── .kernel-build/       # 构建产物
```

### 1.2 当前 Syscall 路径

```
用户态程序
    │
    │  (ecall / svc / syscall 指令)
    ▼
arch/*/trap_handler
    │
    ▼
syscall_dispatch()                    ← kernel/syscall/syscall.c
    │
    ├─ linux_syscall_lookup(nr)       ← kernel/abi/linux/syscall_table.c
    │
    ▼
linux_handle_##name(&args)            ← 由 syscall_table.def 宏展开
    │
    ▼
sys_read/sys_write/sys_openat/...     ← kernel/abi/linux/sys_*.c
    │
    ▼
内核核心模块（mm, proc, fs, net）     ← kernel/mm, kernel/proc, kernel/fs, kernel/net
```

### 1.3 ABI 选择机制

当前使用编译时宏选择 ABI：

```c
// kernel/include/abi/current.h
#if defined(CONFIG_ABI_LINUX)
# include "abi/linux/errno.h"
# include "abi/linux/fcntl.h"
// ...
#else
# error "No user ABI selected"
#endif
```

Makefile 中 `ABI ?= linux`，且硬编码只支持 linux。

### 1.4 关键数据结构

| 结构 | 文件 | 用途 |
|------|------|------|
| `task_t` | `kernel/proc/proc.c` | 进程/线程控制块 |
| `fdtable_t` | `kernel/fs/fdtable.c` | 文件描述符表 |
| `vm_area_t` | `kernel/mm/vm.c` | 虚拟内存区域 |
| `vfs_node_t` | `kernel/fs/vfs.c` | VFS 节点 |
| `linux_syscall_args_t` | `kernel/include/abi/linux/syscall_entry.h` | Linux syscall 参数 |
| `linux_syscall_entry_t` | 同上 | syscall 表条目 |

---

## 2. Native ABI 内核侧组件设计

### 2.1 目标架构

```
用户态程序（Native）        用户态程序（Linux）
       │                        │
       │  ecall/svc/syscall      │  ecall/svc/syscall
       ▼                        ▼
  syscall_dispatch()       syscall_dispatch()
       │                        │
       ├─ 判断 syscall 编号范围    │
       │  (Native: 0x0000-0x1FFF) │
       │  (Linux: 0-449)         │
       │                        │
       ▼                        ▼
 native_syscall_dispatch()  linux_syscall_dispatch()
       │                        │
       ▼                        ▼
 native_handle_###()        linux_handle_###()
       │                        │
       └────────┬───────────────┘
                ▼
        内核核心模块（共享）
        handle_table / mm / proc / vfs / net
```

### 2.2 核心新增组件

#### 2.2.1 Handle Table 子系统

**新增文件**：`kernel/core/handle.c`，`kernel/include/core/handle.h`

Handle table 是 Native ABI 的基础。每个 task 拥有一个 handle table。

```c
// kernel/include/core/handle.h

#ifndef _CORE_HANDLE_H
#define _CORE_HANDLE_H

#include "core/types.h"

typedef uint32_t a20_handle_t;
typedef uint64_t a20_rights_t;

// Handle 对象类型
typedef enum a20_object_type {
    A20_OBJ_INVALID = 0,
    A20_OBJ_TASK,
    A20_OBJ_THREAD,
    A20_OBJ_FILE,
    A20_OBJ_DIRECTORY,
    A20_OBJ_SOCKET,
    A20_OBJ_PIPE_ENDPOINT,
    A20_OBJ_CHANNEL_ENDPOINT,
    A20_OBJ_EVENT_QUEUE,
    A20_OBJ_TIMER,
    A20_OBJ_MEMORY,
    A20_OBJ_DEVICE,
    A20_OBJ_NAMESPACE,
    A20_OBJ_DEBUG,
} a20_object_type_t;

// Handle 条目（进程 handle table 中的一个槽位）
// 注意：handle entry 不持有自己的引用计数。引用计数是对象的属性
// （refcount(o) = 指向 o 的 handle 数量），不是 handle 的属性。
// 每个条目要么被占用（object != NULL），要么空闲（object == NULL）。
typedef struct a20_handle_entry {
    void             *object;      // 指向内核对象的指针（NULL = 空闲槽位）
    a20_object_type_t type;        // 对象类型
    a20_rights_t      rights;      // 该 handle 的权限
} a20_handle_entry_t;

// Handle Table（每个 task 一个）
typedef struct a20_handle_table {
    a20_handle_entry_t *entries;   // 动态数组
    uint32_t            capacity;  // 表容量
    uint32_t            next_slot; // 下一个可用槽位提示
    spinlock_t          lock;      // 自旋锁
    uint64_t           *free_bitmap; // 加速空闲槽位查找（参见 08-architecture-deep-dive.md §2）
    uint32_t            bitmap_size; // bitmap 的 uint64 元素数
} a20_handle_table_t;

// 创建/销毁 handle table
a20_handle_table_t *a20_handle_table_create(uint32_t capacity);
void a20_handle_table_destroy(a20_handle_table_t *table);

// Handle 操作
int64_t a20_handle_alloc(a20_handle_table_t *table, void *object,
                         a20_object_type_t type, a20_rights_t rights,
                         a20_handle_t *out);
int64_t a20_handle_lookup(a20_handle_table_t *table, a20_handle_t handle,
                          a20_object_type_t expected_type, a20_rights_t required_rights,
                          a20_handle_entry_t *out);
int64_t a20_handle_close(a20_handle_table_t *table, a20_handle_t handle);
int64_t a20_handle_dup(a20_handle_table_t *table, a20_handle_t src,
                       a20_rights_t new_rights, a20_handle_t *out);

// 在 task_t 中嵌入 handle_table 指针
// task->native_handle_table = a20_handle_table_create(256);

#endif /* _CORE_HANDLE_H */
```

**关键设计决策**：
- Handle table 是独立于 fdtable 的数据结构。Linux ABI 继续使用 fdtable，Native ABI 使用 handle_table。
- task_t 通过指针关联 handle_table，当 task 以 Linux 模式运行时 handle_table 为 NULL。
- Handle table 使用动态数组，初始容量 256，可按需扩展。

#### 2.2.2 Native Syscall 分派器

**新增文件**：`kernel/abi/native/syscall_table.c`，`kernel/abi/native/syscall_table.def`

参考 Linux ABI 的模式：

```c
// kernel/abi/native/syscall_table.def
// 格式：NATIVE_SYSCALL(编号, 名称, 恢复上下文, 实现调用)

NATIVE_SYSCALL(0x0000, abi_info,         0, native_sys_abi_info(&args))
NATIVE_SYSCALL(0x0100, handle_close,     0, native_sys_handle_close(&args))
NATIVE_SYSCALL(0x0101, handle_dup,       0, native_sys_handle_dup(&args))
NATIVE_SYSCALL(0x0102, handle_query,     0, native_sys_handle_query(&args))
NATIVE_SYSCALL(0x0200, task_exit,        1, native_sys_task_exit(&args))
NATIVE_SYSCALL(0x0201, task_spawn,       0, native_sys_task_spawn(&args))
NATIVE_SYSCALL(0x0300, vm_alloc,         0, native_sys_vm_alloc(&args))
NATIVE_SYSCALL(0x0301, vm_unmap,         0, native_sys_vm_unmap(&args))
NATIVE_SYSCALL(0x0400, path_open,        0, native_sys_path_open(&args))
NATIVE_SYSCALL(0x0401, handle_read,      0, native_sys_handle_read(&args))
NATIVE_SYSCALL(0x0402, handle_write,     0, native_sys_handle_write(&args))
NATIVE_SYSCALL(0x0500, event_queue_create, 0, native_sys_event_queue_create(&args))
NATIVE_SYSCALL(0x0501, event_watch,      0, native_sys_event_watch(&args))
NATIVE_SYSCALL(0x0502, event_wait,       0, native_sys_event_wait(&args))
NATIVE_SYSCALL(0x0700, clock_get,        0, native_sys_clock_get(&args))
```

#### 2.2.3 Native Syscall 实现文件

```
kernel/abi/native/
├── DESIGN.md              # 已有：设计文档
├── syscall_table.c        # 新增：syscall 表构建和查找
├── syscall_table.def      # 新增：syscall 定义宏
├── syscall_impl.h         # 新增：实现函数声明
├── sys_core.c             # 新增：abi_info, feature_test
├── sys_handle.c           # 新增：handle_close, handle_dup, handle_query
├── sys_task.c             # 新增：task_spawn, task_exit, task_wait, thread_create
├── sys_memory.c           # 新增：vm_alloc, vm_map, vm_unmap, vm_protect, vm_share
├── sys_path.c             # 新增：path_open, handle_read, handle_write, handle_stat
├── sys_event.c            # 新增：event_queue_create, event_watch, event_wait
├── sys_net.c              # 新增：net_socket, net_bind, net_connect, net_accept
├── sys_time.c             # 新增：clock_get, timer_create, timer_set
└── sys_security.c         # 新增：ns_create, ns_apply
```

#### 2.2.4 Native ABI 头文件

```
kernel/include/abi/native/
├── types.h        # a20_handle_t, a20_rights_t, a20_status_t 等
├── errno.h        # A20_ERR_* 错误码
├── rights.h       # A20_RIGHT_* 权限位定义
├── syscall_nr.h   # Syscall 编号宏
├── syscall_entry.h # Native syscall 入口结构
└── startup.h      # a20_start_info_t 定义
```

### 2.3 Syscall 分派策略

修改 `syscall_dispatch()` 以区分两种 ABI：

```c
// kernel/syscall/syscall.c（修改后）

int64_t syscall_dispatch(trap_context_t *ctx)
{
    uint64_t num = TRAP_CTX_SYSCALL_NUM(ctx);

    // Native ABI 的 syscall 编号在 0x0000-0x1FFF 范围
    // Linux ABI 的 syscall 编号在 0-449 范围
    // 两者不冲突

    if (num >= 0x1000 && num <= 0x1FFF) {
        // Experimental native syscalls
        return native_syscall_dispatch(num, ctx);
    }

    if (num <= 0x0FFF && num >= 0x0000 &&
        (num & 0xFF00) != 0) {
        // Stable native syscalls (0x0100 - 0x0FFF)
        return native_syscall_dispatch(num, ctx);
    }

    // Linux ABI (0 - ~450)
    // ... 现有逻辑 ...
}
```

**或者更好的方式**——通过 task 标记区分：

```c
int64_t syscall_dispatch(trap_context_t *ctx)
{
    uint64_t num = TRAP_CTX_SYSCALL_NUM(ctx);
    task_t *cur = proc_current();

    if (cur && cur->abi_mode == TASK_ABI_NATIVE) {
        return native_syscall_dispatch(num, ctx);
    }

    // 默认 Linux ABI
    // ... 现有逻辑 ...
}
```

### 2.4 各组件文件功能分配

| 文件 | 功能 | 依赖的核心模块 |
|------|------|---------------|
| `sys_core.c` | `abi_info`, `feature_test` | 无 |
| `sys_handle.c` | Handle 操作（close, dup, query, replace） | `core/handle` |
| `sys_task.c` | 进程/线程创建、退出、等待 | `proc/proc`, `proc/sched`, `mm/elf`, `core/handle` |
| `sys_memory.c` | 内存分配、映射、共享 | `mm/vm`, `mm/mm`, `core/handle` |
| `sys_path.c` | 路径操作、文件 I/O、stat | `fs/vfs`, `fs/file`, `core/handle` |
| `sys_event.c` | 事件队列、IPC channel | `core/handle`, 新增 `core/event` |
| `sys_net.c` | Socket 操作 | `net/socket`, `core/handle` |
| `sys_time.c` | 时钟、定时器 | `core/timer`, `core/timekeeping`, `core/handle` |
| `sys_security.c` | Namespace 操作 | `proc/proc`, `core/handle` |

---

## 3. 核心模块改造需求

### 3.1 task_t 扩展

```c
// 在 task_t 中添加：
typedef struct task {
    // ... 现有字段 ...

    // Native ABI 支持
    int abi_mode;                          // TASK_ABI_LINUX or TASK_ABI_NATIVE
    struct a20_handle_table *handle_table; // Native handle table（NULL for Linux ABI）
    a20_handle_t self_handle;              // 指向自己的 handle（Native 模式）
    a20_handle_t root_dir_handle;          // 根目录 handle
    a20_handle_t cwd_dir_handle;           // 当前工作目录 handle
} task_t;
```

### 3.2 task_t 初始化

- Linux ABI 的 task：`abi_mode = TASK_ABI_LINUX`，`handle_table = NULL`
- Native ABI 的 task：`abi_mode = TASK_ABI_NATIVE`，创建 handle_table
- `task_spawn` 创建的子进程继承父进程的 `abi_mode` 或由参数指定

### 3.3 不需要修改的核心模块

以下模块完全不需要修改，因为 Native ABI 的 syscall 实现只是调用它们的内部 API：

| 模块 | 原因 |
|------|------|
| `kernel/mm/*` | vm_alloc/vm_map 调用相同的内部 mm API |
| `kernel/fs/vfs.c` | path_open 调用 vfs 内部 API |
| `kernel/fs/ext4.c`, `fat32.c` | 文件系统不关心上层 ABI |
| `kernel/net/*` | socket 操作调用相同的 net API |
| `kernel/core/trap.c` | trap 入口不改变 |
| `kernel/arch/*` | 架构代码不改变 |

---

## 4. Build System 集成

### 4.1 Makefile 修改

```makefile
# 顶层 Makefile 添加：
ABI ?= linux   # 默认仍然为 linux

# 未来可支持：
# ABI ?= native
# ABI ?= both   # 同时编译两种 ABI
```

### 4.2 内核构建

```makefile
# kernel/Makefile 片段

# Linux ABI 文件（始终编译）
ABI_LINUX_SRCS = $(wildcard kernel/abi/linux/sys_*.c)

# Native ABI 文件（CONFIG_ABI_NATIVE 或 CONFIG_ABI_BOTH 时编译）
ABI_NATIVE_SRCS = $(wildcard kernel/abi/native/sys_*.c)

# Handle 子系统（Native ABI 需要）
CORE_HANDLE_SRCS = kernel/core/handle.c

ifeq ($(ABI),linux)
  CFLAGS += -DCONFIG_ABI_LINUX
  ABI_SRCS = $(ABI_LINUX_SRCS)
else ifeq ($(ABI),native)
  CFLAGS += -DCONFIG_ABI_NATIVE
  ABI_SRCS = $(ABI_NATIVE_SRCS) $(CORE_HANDLE_SRCS)
else ifeq ($(ABI),both)
  CFLAGS += -DCONFIG_ABI_LINUX -DCONFIG_ABI_BOTH
  ABI_SRCS = $(ABI_LINUX_SRCS) $(ABI_NATIVE_SRCS) $(CORE_HANDLE_SRCS)
endif
```

### 4.3 abi/current.h 修改

```c
#ifndef _ABI_CURRENT_H
#define _ABI_CURRENT_H

#if defined(CONFIG_ABI_LINUX)
# include "abi/linux/errno.h"
# include "abi/linux/fcntl.h"
// ...
#elif defined(CONFIG_ABI_NATIVE)
# include "abi/native/types.h"
# include "abi/native/errno.h"
# include "abi/native/rights.h"
#elif defined(CONFIG_ABI_BOTH)
# include "abi/linux/errno.h"
// ... linux headers ...
# include "abi/native/types.h"
# include "abi/native/errno.h"
#endif

#endif
```

---

## 5. 用户态组件设计

### 5.1 目录结构

```
user/
├── lib/
│   ├── a20rt/           # Native 系统运行时
│   │   ├── include/
│   │   │   └── a20/
│   │   │       ├── syscall.h     # syscall wrapper（inline asm）
│   │   │       ├── types.h       # a20_handle_t 等
│   │   │       ├── rights.h      # A20_RIGHT_* 宏
│   │   │       ├── errno.h       # A20_ERR_* 宏
│   │   │       ├── startup.h     # a20_start_info_t
│   │   │       └── abi.h         # abi_info() 等查询函数
│   │   ├── src/
│   │   │   ├── start.S           # _start 入口点
│   │   │   ├── syscall.S         # syscall 指令封装（各架构）
│   │   │   ├── crt0.c            # C 运行时初始化
│   │   │   ├── handle.c          # handle table 用户态缓存
│   │   │   └── tls.c             # TLS 设置
│   │   └── Makefile
│   │
│   ├── a20c/            # Native C 库
│   │   ├── include/
│   │   │   ├── a20/
│   │   │   │   └── handle.h      # handle 操作高级 API
│   │   │   ├── stdio.h
│   │   │   ├── stdlib.h
│   │   │   ├── string.h
│   │   │   ├── time.h
│   │   │   ├── malloc.h
│   │   │   └── ...
│   │   ├── src/
│   │   │   ├── stdio/
│   │   │   │   ├── fopen.c       # FILE* → handle 映射
│   │   │   │   ├── fread.c
│   │   │   │   ├── fwrite.c
│   │   │   │   ├── printf.c
│   │   │   │   └── fflush.c
│   │   │   ├── stdlib/
│   │   │   │   ├── malloc.c      # 基于 vm_alloc 的分配器
│   │   │   │   ├── exit.c
│   │   │   │   └── getenv.c
│   │   │   ├── string/
│   │   │   │   ├── memcpy.c
│   │   │   │   ├── strlen.c
│   │   │   │   └── ...
│   │   │   └── time/
│   │   │       └── clock.c
│   │   └── Makefile
│   │
│   └── a20posix/        # POSIX 兼容层（可选）
│       ├── include/
│       │   ├── unistd.h
│       │   ├── fcntl.h
│       │   ├── signal.h
│       │   ├── sys/
│       │   │   ├── types.h
│       │   │   ├── stat.h
│       │   │   ├── mmap.h
│       │   │   └── socket.h
│       │   └── ...
│       ├── src/
│       │   ├── fd_table.c         # fd ↔ handle 映射
│       │   ├── unistd.c           # open/close/read/write 映射
│       │   ├── fcntl.c
│       │   ├── signal.c           # 信号模拟
│       │   ├── stat.c
│       │   ├── mmap.c
│       │   ├── socket.c
│       │   ├── dirent.c
│       │   ├── fork.c             # 返回 ENOSYS 或用 spawn 模拟
│       │   ├── exec.c             # 用 task_spawn 模拟
│       │   └── errno.c            # 错误码转换
│       └── Makefile
│
├── cmds/
│   ├── native-init/      # Native ABI 初始化程序
│   │   ├── main.c        # 最小 init：写 stdout，退出
│   │   └── Makefile
│   └── ...
```

---

## 6. Native libc 设计详案

### 6.1 liba20rt — 系统运行时

**职责**：最小化的启动代码和 syscall 封装。

#### 启动流程

```assembly
# user/lib/a20rt/src/start.S（riscv64 示例）
.section .text
.global _start
_start:
    # a0 = a20_start_info_t 指针（由内核放在约定位置）
    # 设置栈指针
    # 设置 TLS
    # 调用 __a20rt_init(start_info)
    # 调用 main(argc, argv, envp)
    # 调用 task_exit(return_value)
```

#### Syscall Wrapper

```c
// user/lib/a20rt/include/a20/syscall.h

// 架构相关的 syscall 指令封装
// riscv64: ecall
// aarch64: svc #0
// loongarch64: syscall 0

static inline int64_t a20_syscall1(uint64_t nr, uint64_t a0)
{
    register uint64_t a7 __asm__("a7") = nr;
    register uint64_t _a0 __asm__("a0") = a0;
    __asm__ volatile("ecall" : "+r"(_a0) : "r"(a7) : "memory");
    return (int64_t)_a0;
}
// ... a20_syscall2, a20_syscall3, 等
```

### 6.2 liba20c — C 库

**职责**：提供标准 C 库功能，基于 liba20rt 构建。

#### 内存分配

```c
// user/lib/a20c/src/stdlib/malloc.c

// 策略：大块用 vm_alloc，小块用内部 slab 分配器
// 初始分配 4MB 堆区域，按需扩展

static void *heap_base;
static size_t heap_size;

void *malloc(size_t size) {
    if (size >= PAGE_SIZE / 4) {
        // 大块直接 vm_alloc
        a20_vm_alloc_args_t args = { ... };
        int64_t rc = a20_syscall2(SYS_vm_alloc, (uint64_t)&args, sizeof(args));
        return rc >= 0 ? (void *)args.out_addr : NULL;
    }
    // 小块用内部 slab
    return slab_alloc(size);
}
```

#### stdio

```c
// user/lib/a20c/src/stdio/fopen.c

typedef struct A20_FILE {
    a20_handle_t handle;
    int flags;
    uint8_t *buffer;
    size_t buf_size;
    size_t buf_pos;
    size_t buf_len;
    int buf_mode;       // _IONBF, _IOLBF, _IOFBF
    int eof;
    int error;
} A20_FILE;

// 全局 stdin/stdout/stderr 从 a20_start_info 初始化
static A20_FILE _stdin_file, _stdout_file, _stderr_file;
FILE *stdin = &_stdin_file;
FILE *stdout = &_stdout_file;
FILE *stderr = &_stderr_file;

void __a20c_init_stdio(const a20_start_info_t *info) {
    _stdin_file.handle = info->stdin_handle;
    _stdin_file.flags = O_RDONLY;
    // ...
    _stdout_file.handle = info->stdout_handle;
    _stdout_file.flags = O_WRONLY;
    _stdout_file.buffer = malloc(BUFSIZ);
    _stdout_file.buf_size = BUFSIZ;
    _stdout_file.buf_mode = _IOLBF;
    // ...
}
```

### 6.3 liba20posix — POSIX 兼容层

**职责**：将 POSIX API 映射到 Native ABI handle 操作。

#### fd ↔ Handle 映射

```c
// user/lib/a20posix/src/fd_table.c

#define POSIX_MAX_FDS 1024

typedef struct {
    a20_handle_t handle;
    int flags;         // O_CLOEXEC 等
    int posix_flags;   // POSIX open flags
} posix_fd_entry_t;

static posix_fd_entry_t posix_fd_table[POSIX_MAX_FDS];
static spinlock_t fd_table_lock = SPINLOCK_INIT;

// 分配 fd 编号
static int fd_alloc(void) {
    for (int i = 0; i < POSIX_MAX_FDS; i++) {
        if (posix_fd_table[i].handle == A20_HANDLE_INVALID) {
            posix_fd_table[i].handle = 0; // 标记占用
            return i;
        }
    }
    return -1; // EMFILE
}

// fd → handle 转换
a20_handle_t fd_to_handle(int fd) {
    if (fd < 0 || fd >= POSIX_MAX_FDS) return A20_HANDLE_INVALID;
    return posix_fd_table[fd].handle;
}

// handle → fd 注册
int handle_to_fd(a20_handle_t handle, int flags) {
    int fd = fd_alloc();
    if (fd < 0) return -1;
    posix_fd_table[fd].handle = handle;
    posix_fd_table[fd].flags = flags;
    return fd;
}
```

#### POSIX API 映射示例

```c
// user/lib/a20posix/src/unistd.c

int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    a20_rights_t rights = 0;
    if (flags & O_RDONLY || flags & O_RDWR) rights |= A20_RIGHT_READ;
    if (flags & O_WRONLY || flags & O_RDWR) rights |= A20_RIGHT_WRITE;
    if (flags & O_EXEC) rights |= A20_RIGHT_EXEC;

    a20_path_open_args_t args = {
        .size = sizeof(args),
        .version = 1,
        .dir = posix_cwd_handle(),
        .flags = 0,
        .rights = rights,
        .path = (uint64_t)path,
        .path_len = 0,
        .mode = mode,
    };

    int64_t rc = a20_syscall1(SYS_path_open, (uint64_t)&args);
    if (rc < 0) {
        errno = a20_to_posix_errno(rc);
        return -1;
    }

    int fd = handle_to_fd(args.out_handle, flags);
    if (fd < 0) {
        handle_close(args.out_handle);
        errno = EMFILE;
        return -1;
    }
    return fd;
}

ssize_t read(int fd, void *buf, size_t count) {
    a20_handle_t h = fd_to_handle(fd);
    if (h == A20_HANDLE_INVALID) {
        errno = EBADF;
        return -1;
    }

    a20_iovec_t iov = { .base = (uint64_t)buf, .len = count };
    a20_io_args_t args = {
        .size = sizeof(args),
        .version = 1,
        .handle = h,
        .iov = (uint64_t)&iov,
        .iov_count = 1,
        .offset = A20_OFFSET_CURRENT,
    };

    int64_t rc = a20_syscall1(SYS_handle_read, (uint64_t)&args);
    if (rc < 0) {
        errno = a20_to_posix_errno(rc);
        return -1;
    }
    return (ssize_t)args.out_count;
}

pid_t getpid(void) {
    return posix_self_pid;
    // Native ABI 不用 PID，由 libc 维护一个 task handle → pid 映射
}

// fork() 不支持
pid_t fork(void) {
    errno = ENOSYS;
    return -1;
}
```

---

## 7. 分阶段实施路线

### Phase 0：基础设施（1-2 周）

**目标**：最小内核支持 + Hello World

| 任务 | 文件 | 说明 |
|------|------|------|
| 定义 Native ABI 类型头文件 | `kernel/include/abi/native/types.h, errno.h, rights.h, syscall_nr.h, startup.h` | 纯头文件，无实现 |
| 实现 handle table | `kernel/core/handle.c` | 核心数据结构 |
| 扩展 task_t | `kernel/proc/proc.c` | 添加 abi_mode, handle_table 字段 |
| 实现 syscall 表框架 | `kernel/abi/native/syscall_table.c, .def` | 参照 Linux ABI 模式 |
| 修改 syscall_dispatch | `kernel/syscall/syscall.c` | 区分两种 ABI |
| 实现 abi_info | `kernel/abi/native/sys_core.c` | 第一个 native syscall |
| 实现 handle_close/dup/query | `kernel/abi/native/sys_handle.c` | |
| 实现 task_exit | `kernel/abi/native/sys_task.c` | |
| 实现 vm_alloc/vm_unmap | `kernel/abi/native/sys_memory.c` | |
| 实现 handle_read/handle_write | `kernel/abi/native/sys_path.c` | |
| 实现 clock_get | `kernel/abi/native/sys_time.c` | |
| 实现 path_open | `kernel/abi/native/sys_path.c` | |
| 实现 event_queue 基础 | `kernel/abi/native/sys_event.c` | |
| 修改 Makefile | `Makefile` | 支持 `ABI=native` 或 `ABI=both` |
| 修改 abi/current.h | `kernel/include/abi/current.h` | 支持 CONFIG_ABI_NATIVE |

**验证**：在 QEMU 上以 `ABI=both` 启动，Linux 程序正常运行。

### Phase 1：用户态运行时（1-2 周）

**目标**：Native 程序可以启动、打印、退出

| 任务 | 文件 | 说明 |
|------|------|------|
| liba20rt syscall wrapper | `user/lib/a20rt/src/syscall.S` | riscv64/aarch64/loongarch64 |
| liba20rt start.S | `user/lib/a20rt/src/start.S` | _start 入口 |
| liba20rt crt0.c | `user/lib/a20rt/src/crt0.c` | C 运行时初始化 |
| liba20c 最小 stdio | `user/lib/a20c/src/stdio/` | printf → handle_write |
| liba20c 最小 string | `user/lib/a20c/src/string/` | memcpy, strlen 等 |
| liba20c 最小 stdlib | `user/lib/a20c/src/stdlib/` | malloc, exit |
| native-init 程序 | `user/cmds/native-init/main.c` | Hello World |
| 构建脚本 | `user/lib/a20rt/Makefile, user/lib/a20c/Makefile` | |

**验证**：`native-init` 在内核中运行，输出 "Hello from A20 Native ABI!"，正常退出。

### Phase 2：进程创建与 IPC（2-3 周）

**目标**：支持 native 进程 spawn 和消息通道

| 任务 | 文件 | 说明 |
|------|------|------|
| task_spawn | `kernel/abi/native/sys_task.c` | 需要新的 ELF 加载路径 |
| thread_create | `kernel/abi/native/sys_task.c` | |
| task_wait | `kernel/abi/native/sys_task.c` | |
| channel_create | `kernel/abi/native/sys_event.c` | 内核中的 channel 对象 |
| channel_send/recv | `kernel/abi/native/sys_event.c` | 含 handle transfer |
| handle_transfer | `kernel/core/handle.c` | 跨进程 handle 传递 |
| event_queue 完善 | `kernel/core/event.c` | 完整的等待/通知机制 |
| vm_map | `kernel/abi/native/sys_memory.c` | 文件/共享内存映射 |
| vm_share | `kernel/abi/native/sys_memory.c` | 内存共享 |

**验证**：Native init 能 spawn 子进程，子进程通过 channel 与父进程通信。

### Phase 3：网络与安全（2-3 周）

**目标**：支持 socket 操作和基本 sandbox

| 任务 | 文件 | 说明 |
|------|------|------|
| net_socket | `kernel/abi/native/sys_net.c` | |
| net_bind/connect/accept | `kernel/abi/native/sys_net.c` | |
| timer_create/set | `kernel/abi/native/sys_time.c` | timer 作为 handle |
| ns_create | `kernel/abi/native/sys_security.c` | namespace 对象 |
| path_create/unlink/rename | `kernel/abi/native/sys_path.c` | 文件系统写操作 |
| handle_stat | `kernel/abi/native/sys_path.c` | |
| handle_control | `kernel/abi/native/sys_path.c` | 替代 ioctl |

### Phase 4：POSIX 兼容层（3-4 周）

**目标**：部分 POSIX 程序可以通过兼容层运行

| 任务 | 文件 | 说明 |
|------|------|------|
| fd ↔ handle 映射表 | `user/lib/a20posix/src/fd_table.c` | |
| open/close/read/write | `user/lib/a20posix/src/unistd.c` | |
| stat/fstat | `user/lib/a20posix/src/stat.c` | |
| mmap/munmap | `user/lib/a20posix/src/mmap.c` | |
| socket/connect/bind | `user/lib/a20posix/src/socket.c` | |
| errno 转换 | `user/lib/a20posix/src/errno.c` | |
| signal 模拟 | `user/lib/a20posix/src/signal.c` | 有限支持 |
| select/poll 模拟 | `user/lib/a20posix/src/select.c` | 基于 event_queue |

**验证**：简单 POSIX 程序（如 ls, cat）通过 liba20posix 运行。

---

## 8. 风险与缓解策略

### 8.1 风险评估

| 风险 | 概率 | 影响 | 缓解策略 |
|------|------|------|---------|
| Linux ABI 回归 | 中 | 高 | Phase 0 的每个 PR 都先跑 Linux 测试套件；ABI=both 模式始终编译两者 |
| handle table 性能 | 低 | 中 | 使用 per-CPU 缓存、RCU 读侧优化；初始容量 256，按需扩展 |
| syscall 编号冲突 | 低 | 高 | Native 使用 0x0100+ 范围，与 Linux 的 0-450 无冲突 |
| libc 开发量大 | 高 | 中 | 优先实现 liba20rt + 最小 liba20c；string/memory 函数可用 musl 的实现 |
| 架构相关代码 | 中 | 中 | syscall wrapper 需要 3 套汇编；参考 Linux 的 arch 实现 |
| 用户态 ELF 加载 | 中 | 高 | 复用现有 `mm/elf.c`，新增 Native ABI 启动协议 |

### 8.2 与 Linux ABI 的隔离原则

1. **内核核心模块不包含任何 ABI 特定代码**
   - `mm/`, `fs/`, `net/`, `proc/` 只暴露内部 API
   - `abi/linux/` 和 `abi/native/` 分别调用这些 API

2. **数据结构隔离**
   - Linux ABI 使用 `fdtable_t`
   - Native ABI 使用 `a20_handle_table_t`
   - 两者不共享用户态结构体定义

3. **Syscall 分派隔离**
   - 通过 `task->abi_mode` 或编号范围区分
   - 两种 ABI 的 syscall 处理函数完全不交叉

4. **构建隔离**
   - `ABI=linux` 时只编译 Linux ABI 代码
   - `ABI=native` 时只编译 Native ABI 代码
   - `ABI=both` 时两者都编译

---

## 附录 A：与 Zircon 的对比

| 方面 | Zircon | A20 Native ABI |
|------|--------|----------------|
| Handle 类型 | ~25 种内核对象 | ~14 种（精简） |
| Syscall 数量 | ~150 | ~53 |
| IPC 原语 | Channel, Socket, FIFO | Channel, Socket |
| 等待机制 | Port + Signal | Event Queue |
| 内存模型 | VMO + VMAR | vm_alloc + vm_map + vm_share |
| 进程创建 | process_create + thread_start | task_spawn（一步完成） |
| ABI 版本 | vDSO | 结构体 size/version |
| 复杂度 | 高（FIDL, component framework） | 低（直接 syscall） |
| 适用场景 | 生产级 OS | 教学/竞赛内核 |

## 附录 B：新增内核代码量估算

| 组件 | 估计代码行数 |
|------|------------|
| `kernel/core/handle.c` | ~300 行 |
| `kernel/core/event.c` | ~200 行 |
| `kernel/abi/native/sys_*.c`（总计） | ~1500 行 |
| `kernel/abi/native/syscall_table.c` | ~50 行 |
| `kernel/include/abi/native/*.h` | ~400 行 |
| `kernel/syscall/syscall.c` 修改 | ~30 行 |
| `kernel/proc/proc.c` 修改 | ~50 行 |
| **内核侧总计** | **~2530 行** |
| `user/lib/a20rt/` | ~400 行 |
| `user/lib/a20c/` | ~2000 行 |
| `user/lib/a20posix/` | ~1500 行 |
| **用户态总计** | **~3900 行** |
| **项目总增加** | **~6430 行** |
