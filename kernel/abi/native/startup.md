# A20OS Native ABI：用户态启动协议

> 本文档定义 Native ABI 程序的启动协议、初始 handle 分配和 native libc 分层设计。

---

## 1. 启动协议

Native 程序不直接继承 Linux auxv 语义。内核在初始用户栈或只读启动信息页提供 `a20_start_info_t`。

### 1.1 启动信息结构

```c
typedef struct a20_start_info {
    uint32_t size;
    uint32_t version;

    uint32_t argc;               /* 命令行参数数量 */
    uint32_t envc;               /* 环境变量数量 */
    uint32_t auxc;               /* 辅助向量条目数 */
    uint32_t reserved0;

    uint64_t argv;               /* 用户指针：char*[] */
    uint64_t envp;               /* 用户指针：char*[] */
    uint64_t auxv;               /* 用户指针：a20_auxv[] */

    a20_handle_t root_dir;       /* 根目录 handle */
    a20_handle_t cwd_dir;        /* 工作目录 handle */
    a20_handle_t stdin_handle;   /* 标准输入 handle */
    a20_handle_t stdout_handle;  /* 标准输出 handle */
    a20_handle_t stderr_handle;  /* 标准错误 handle */
    a20_handle_t self_task;      /* 当前进程的 task handle */
    a20_handle_t main_thread;    /* 主线程的 thread handle */
    a20_handle_t default_event_queue; /* 默认事件队列 handle */

    uint64_t page_size;          /* 页大小 */
    uint64_t user_clock_freq;    /* 用户态时钟频率 */
} a20_start_info_t;
```

### 1.2 启动参数设计意图

- **handle 而非 fd**：不依赖固定的 fd 0/1/2 特殊语义。Native libc 可以把 `stdin/stdout/stderr` 映射成自己的 fd 表，但内核不假设这一点。
- **self_task**：进程可以通过此 handle 查询自身状态、注册退出事件等。需要 `A20_RIGHT_WAIT` 权限。
- **default_event_queue**：为不需要自己创建事件队列的简单程序提供默认等待机制。

### 1.3 启动流程

```text
内核：
  1. 分配 task 结构，设置 abi_mode = NATIVE
  2. 创建 handle table，分配初始 handle
  3. 准备用户栈，压入 a20_start_info
  4. 将入口地址设为 ELF entry point
  5. 跳转到用户态

用户态入口（crt0）：
  1. 从约定位置读取 a20_start_info
  2. 调用 abi_info() 确认 ABI 版本
  3. 初始化 libc（liba20rt/liba20c）
  4. 调用 main(argc, argv, envp)
  5. 返回后调用 task_exit(ret)
```

### 1.4 初始 handle 权限

| Handle | 类型 | Rights |
|--------|------|--------|
| root_dir | dir | READ, STAT, DUP, TRANSFER |
| cwd_dir | dir | READ, STAT, DUP |
| stdin_handle | file | READ, DUP |
| stdout_handle | file | WRITE, DUP |
| stderr_handle | file | WRITE, DUP |
| self_task | task | WAIT, SIGNAL, STAT, DUP |
| main_thread | thread | STAT, DUP |
| default_event_queue | eventq | READ, DUP |

---

## 2. Native libc 分层设计

### 2.1 三层结构

```
┌──────────────────────────────────────────┐
│         POSIX shim（可选）                  │  musl 上层代码 + 语义桥接
├──────────────────────────────────────────┤
│           liba20c                        │  最小 C 库：malloc, stdio, string, time
├──────────────────────────────────────────┤
│           liba20rt                       │  syscall wrapper, 启动代码, handle I/O
├──────────────────────────────────────────┤
│          kernel                          │  Native ABI syscall 接口
└──────────────────────────────────────────┘
```

**两种实现路径**（按项目阶段选择）：

| 路径 | 适用场景 | 工作量 | POSIX 兼容性 |
|------|---------|--------|-------------|
| 从零写 liba20c | Phase 0-1，验证最小程序 | ~3000 行 | ISO C 子集 |
| 移植 musl | Phase 2+，支持真实程序 | ~2500 行新代码 + musl | 完整 POSIX |

推荐策略：先用 liba20c 验证内核，再移植 musl 支撑生态。

### 2.2 liba20rt — 最小运行时

只提供 syscall wrapper 和基本 handle I/O：

- `a20_syscall(n, args)` — 通用 syscall 入口
- 各 syscall 的内联 wrapper（`a20_handle_close(h)` 等）
- `a20_start(argc, argv)` — crt0 启动代码
- `a20_handle_write_simple(h, buf, len)` — 不需要 iovec 的简化写

**设计约束**：liba20rt 不依赖任何堆分配。所有操作在栈上完成。

#### 2.2.1 crt0 启动代码（aarch64）

```c
/* crt0_a20.S — Native ABI 程序入口 */
    .global _start
_start:
    /* x0 指向 a20_start_info_t（内核约定） */
    mov     x19, x0                 /* 保存 start_info 指针 */

    /* 确认 ABI 版本 */
    sub     sp, sp, #256
    mov     x0, sp
    /* a20_abi_info(sp) — syscall 0x0000 */
    mov     x8, #0x0000
    svc     #0

    /* 提取 argc, argv, envp */
    ldr     w0, [x19, #8]           /* argc */
    ldr     x1, [x19, #24]          /* argv */
    ldr     x2, [x19, #32]          /* envp */

    /* 初始化 libc（fd 表、堆等） */
    mov     x3, x19                 /* start_info 指针传给 __libc_init */
    bl      __libc_init

    /* 调用 main */
    bl      main

    /* 退出 */
    mov     w0, w0
    /* task_exit(code) — syscall 0x0200 */
    mov     x8, #0x0200
    svc     #0
```

#### 2.2.2 syscall wrapper 模式

```c
/* a20_syscall.h — syscall 发射层 */

/* 通用 syscall 入口（6 参数） */
static inline int64_t a20_syscall6(uint32_t nr,
    uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3, uint64_t a4, uint64_t a5)
{
    register uint64_t x8 __asm__("x8") = nr;
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    register uint64_t x4 __asm__("x4") = a4;
    register uint64_t x5 __asm__("x5") = a5;
    __asm__ volatile("svc #0"
        : "+r"(x0)
        : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
        : "memory");
    return (int64_t)x0;
}

/* 具名 wrapper 示例 */
#define a20_handle_close(h) \
    a20_syscall6(0x0100, (uint64_t)(h), 0, 0, 0, 0, 0)

#define a20_task_exit(code) \
    a20_syscall6(0x0200, (uint64_t)(int32_t)(code), 0, 0, 0, 0, 0)

/* 复杂 syscall（传结构体指针） */
static inline int64_t a20_handle_read(struct a20_io_args *args) {
    return a20_syscall6(0x0401, (uint64_t)args, 0, 0, 0, 0, 0);
}
```

### 2.3 liba20c — 最小 C 库

在 liba20rt 之上提供 ISO C 标准库子集：

| 类别 | 函数 | 实现方式 |
|------|------|---------|
| 内存 | malloc, free, realloc | 基于 vm_alloc，bump allocator 或 slab 分配器 |
| 字符串 | strlen, strcmp, memcpy, ... | 纯用户态实现，无 syscall 依赖 |
| I/O | fopen, fread, fwrite, printf | 基于 handle + fd→handle 映射表 |
| 时间 | clock_gettime, time | 基于 clock_get syscall |
| 进程 | exit, getpid | 基于 task_exit, handle_query |
| 环境访问 | getenv, setenv | 操作 a20_start_info 传入的 envp |

### 2.4 POSIX shim — 兼容层

将 POSIX API 映射到 native handle 模型：

```text
open(path, flags)     → path_open + fd_table_insert
read(fd, buf, n)      → handle_read(fd_table[fd], ...)
write(fd, buf, n)     → handle_write(fd_table[fd], ...)
close(fd)             → handle_close(fd_table[fd]) + fd_table_remove
pipe(fds)             → channel_create + fd_table_insert ×2
socket(domain, type)  → net_socket + fd_table_insert
fork()                → 不支持（返回 ENOSYS）
execve(path, ...)     → 不支持（返回 ENOSYS）
```

**明确不支持**的 POSIX 操作（Phase 2 可通过 musl 移植逐步支持）：

- `fork()` — spawn 模型是 native ABI 的进程创建方式
- `execve()` — task_spawn 替代
- `signal()` — event queue 替代
- `ioctl()` — handle_control 替代
- `epoll_*()` — event_queue 替代

不要一开始就承诺完整 POSIX。只支持足够的子集让简单程序运行。

---

## 3. fd ↔ handle 映射

POSIX 用 `int fd`（小整数，0=stdin），A20 用 `a20_handle_t`（handle table 索引，也是 `uint32_t`）。libc 负责在两者之间桥接。

### 3.1 fd 表数据结构

```c
/* liba20c 内部 fd 表 */
#define A20_FD_INIT_SIZE   32
#define A20_FD_MAX_SIZE    1024

struct a20_fd_entry {
    a20_handle_t  handle;       /* 对应的 A20 handle，A20_HANDLE_NULL = 空闲 */
    uint32_t      flags;        /* O_RDONLY / O_WRONLY / O_RDWR 等 */
    a20_off_t     pos;          /* 文件当前偏移 */
    uint32_t      fd_flags;     /* FD_CLOEXEC 等 */
};

static struct a20_fd_entry *__fd_table;
static int __fd_table_size;      /* 当前容量 */
static int __fd_table_used;      /* 已用数量 */
static spinlock_t __fd_lock;     /* 用户态自旋锁（多线程保护） */
```

### 3.2 初始化

```c
/* __libc_init 中调用 */
void __fd_table_init(const a20_start_info_t *si) {
    __fd_table = __bare_alloc(A20_FD_INIT_SIZE * sizeof(struct a20_fd_entry));
    __fd_table_size = A20_FD_INIT_SIZE;
    memset(__fd_table, 0, ...);

    /* fd 0/1/2 映射到 start_info 中的 stdio handle */
    __fd_table[0] = (struct a20_fd_entry){
        .handle = si->stdin_handle, .flags = O_RDONLY };
    __fd_table[1] = (struct a20_fd_entry){
        .handle = si->stdout_handle, .flags = O_WRONLY };
    __fd_table[2] = (struct a20_fd_entry){
        .handle = si->stderr_handle, .flags = O_WRONLY };

    __fd_table_used = 3;
}
```

### 3.3 fd 操作函数

```c
/* 分配新 fd（线性扫描找空闲槽位） */
int __fd_alloc(a20_handle_t handle, uint32_t flags) {
    spin_lock(&__fd_lock);
    for (int i = 0; i < __fd_table_size; i++) {
        if (__fd_table[i].handle == A20_HANDLE_NULL) {
            __fd_table[i].handle = handle;
            __fd_table[i].flags = flags;
            __fd_table[i].pos = 0;
            spin_unlock(&__fd_lock);
            return i;
        }
    }
    /* 扩容或返回 EMFILE */
    spin_unlock(&__fd_lock);
    errno = EMFILE;
    return -1;
}

/* fd → handle 查询 */
a20_handle_t __fd_to_handle(int fd) {
    if (fd < 0 || fd >= __fd_table_size)
        return A20_HANDLE_NULL;
    return __fd_table[fd].handle;
}

/* POSIX read() 的实现 */
ssize_t read(int fd, void *buf, size_t count) {
    a20_handle_t h = __fd_to_handle(fd);
    if (h == A20_HANDLE_NULL) { errno = EBADF; return -1; }

    struct a20_iovec iov = { .base = (uint64_t)buf, .len = count };
    struct a20_io_args args = {
        .size = sizeof(args), .version = 1,
        .handle = h,
        .iov = (uint64_t)&iov, .iov_count = 1,
        .offset = A20_OFFSET_CURRENT,   /* 使用内部 pos */
    };
    int64_t ret = a20_handle_read(&args);
    if (ret < 0) { errno = __a20_to_errno(ret); return -1; }
    return (ssize_t)args.out_count;
}

/* POSIX open() 的实现 */
int open(const char *path, int flags, ...) {
    struct a20_path_open_args args = {
        .size = sizeof(args), .version = 1,
        .dir = __cwd_handle,   /* libc 维护的 cwd dir handle */
        .flags = __posix_to_a20_flags(flags),
        .rights = __posix_to_a20_rights(flags),
        .path = (uint64_t)path,
        .path_len = 0,         /* nul-terminated */
        .mode = mode,
    };
    int64_t ret = a20_path_open(&args);
    if (ret < 0) { errno = __a20_to_errno(ret); return -1; }
    return __fd_alloc(args.out_handle, flags);
}
```

### 3.4 为什么不直接用 handle 值当 fd？

`a20_handle_t` 和 `int fd` 都是 `uint32_t`，理论上可以省掉映射表。但实际不行：

1. **musl 内部假设 fd 0/1/2 是 stdio**。很多地方硬编码 `fd < 3` 的检查。handle 值不保证从 0 开始。
2. **fd 的连续性假设**。POSIX 保证 `dup()` 返回最小可用 fd，`select()` 遍历 `0..nfds-1`。handle 分配是 bitmap 扫描，不一定连续。
3. **close-on-exec 语义**。fd 有 `FD_CLOEXEC` 标志，handle 没有。映射表是放 fd 级别属性的自然位置。
4. **POSIX 约定 `open` 返回最小可用 fd**。handle 分配不保证这个。

所以：**映射表是必要的，不能省掉**。

---

## 4. musl 移植策略

### 4.1 为什么选 musl

| 因素 | musl | glibc | dietlibc | uclibc |
|------|------|-------|----------|--------|
| 代码量 | ~50K 行 | ~180K 行 | ~8K 行 | ~40K 行 |
| 许可证 | MIT | LGPL | BSD | LGPL |
| 架构分离 | 清晰（`arch/`） | 耦合 | 简单 | 中等 |
| POSIX 完整性 | 高 | 最高 | 低 | 中 |
| 静态链接 | 原生支持 | 臃肿 | 原生 | 原生 |
| 代码质量 | 高（统一风格） | 混杂 | 较低 | 中等 |

musl 的 `arch/<arch>/syscall.h` 是唯一的 syscall 发射点。替换这一层就能让上面 48K 行的 POSIX 实现代码在 A20 上运行。

### 4.2 musl 代码结构分析

```text
musl/
├── arch/
│   ├── aarch64/
│   │   ├── syscall.h         ← 定义 __syscall 宏（SVC #0 指令）
│   │   ├── crt_arch.h        ← crt0 汇编
│   │   ├── pthread_arch.h    ← 线程指针（tp 寄存器）
│   │   └── atomic.h          ← 原子操作（LDXR/STXR）
│   └── ...
├── src/
│   ├── internal/
│   │   ├── syscallops.h      ← 高层 syscall 封装（__sys_open 等）
│   │   └── libc.h            ← 内部数据结构
│   ├── fd/                   ← 文件描述符操作
│   │   ├── open.c            ← open() → __sys_openat(SYS_openat, ...)
│   │   ├── read.c            ← read() → __syscall_cp(SYS_read, ...)
│   │   └── ...
│   ├── thread/               ← pthread 实现
│   ├── signal/               ← 信号实现
│   ├── network/              ← socket 实现
│   ├── stdio/                ← FILE* 实现
│   └── ...                   ← string, stdlib, math, etc.
├── include/                  ← 公共头文件
└── tools/                    ← 构建工具
```

**关键发现**：musl 的 `src/fd/open.c` 调用 `__sys_openat`，后者展开为 `__syscall(SYS_openat, ...)`。`SYS_openat` 是 Linux syscall 编号。替换为 `a20_path_open` 就是替换一个宏。

### 4.3 移植的代码改动清单

#### 6.3.1 新增 `arch/a20/`（~600 行）

```text
arch/a20/
├── syscall.h              ← A20 syscall 发射宏（~80 行）
├── crt_arch.h             ← A20 启动汇编（~40 行）
├── pthread_arch.h         ← TLS 指针约定（~20 行，复用 aarch64）
├── atomic.h               ← 原子操作（~100 行，复用 aarch64）
├── reloc.h                ← 重定位（~30 行，复用 aarch64）
└── bits/
    └── syscall.h          ← A20 syscall 编号定义（~300 行）
```

#### 6.3.2 syscall 发射层替换（`arch/a20/syscall.h`）

```c
/* arch/a20/syscall.h — A20 syscall 发射 */
#ifndef _SYSCALL_H
#define _SYSCALL_H

/* A20 syscall 编号 */
#define __NR_a20_abi_info          0x0000
#define __NR_a20_handle_close      0x0100
#define __NR_a20_handle_dup        0x0101
#define __NR_a20_task_exit         0x0200
#define __NR_a20_task_spawn        0x0201
#define __NR_a20_vm_alloc          0x0300
#define __NR_a20_path_open         0x0400
#define __NR_a20_handle_read       0x0401
#define __NR_a20_handle_write      0x0402
/* ... 全部 90 个 */

/* A20 使用相同的 SVC #0 指令，但 x8 放 A20 编号 */
static inline long __syscall0(long nr)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0");
    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory");
    return x0;
}

static inline long __syscall1(long nr, long a0)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8), "0"(x0) : "memory");
    return x0;
}

/* __syscall2 ... __syscall6 类似 */

#endif
```

#### 6.3.3 syscall 映射层（`src/internal/a20_syscallops.c`，~800 行）

这一层把 musl 的 `__sys_*` 函数映射到 A20 syscall：

```c
/* src/internal/a20_syscallops.c — musl → A20 syscall 桥接 */

/* ===== 文件 I/O ===== */

/* musl 调用 __sys_openat(dirfd, path, flags, mode) */
long __sys_openat(int dirfd, const char *path, int flags, int mode)
{
    struct a20_path_open_args args = {
        .size = sizeof(args), .version = 1,
        .dir = __fd_to_handle(dirfd),
        .flags = __oflag_to_a20(flags),
        .rights = __oflag_to_rights(flags),
        .path = (uint64_t)path, .path_len = 0,
        .mode = mode,
    };
    long ret = __syscall1(__NR_a20_path_open, (long)&args);
    if (ret < 0) return ret;
    return __fd_alloc(args.out_handle, flags);
}

/* musl 调用 __syscall_cp(SYS_read, fd, buf, count) */
/* 需要在 __syscall 宏中做拦截 */
long __a20_read(int fd, void *buf, size_t count)
{
    a20_handle_t h = __fd_to_handle(fd);
    struct a20_iovec iov = { (uint64_t)buf, count };
    struct a20_io_args args = {
        .size = sizeof(args), .version = 1,
        .handle = h,
        .iov = (uint64_t)&iov, .iov_count = 1,
        .offset = A20_OFFSET_CURRENT,
    };
    long ret = __syscall1(__NR_a20_handle_read, (long)&args);
    if (ret < 0) return ret;
    return (long)args.out_count;
}

/* write, close, lseek, fstat, ... 类似模式 */

/* ===== 进程 ===== */

long __sys_clone(unsigned long flags, ...) {
    /* A20 不支持 raw clone */
    return -ENOSYS;
}

long __sys_execve(const char *path, char **argv, char **envp) {
    /* A20 不支持 execve，返回 ENOSYS */
    return -ENOSYS;
}

/* ===== 网络 ===== */

long __sys_socket(int domain, int type, int protocol) {
    struct a20_net_socket_args args = {
        .size = sizeof(args), .version = 1,
        .domain = domain, .type = type, .protocol = protocol,
        .rights = A20_RIGHT_READ | A20_RIGHT_WRITE,
    };
    long ret = __syscall1(__NR_a20_net_socket, (long)&args);
    if (ret < 0) return ret;
    return __fd_alloc(args.out_socket, 0);
}

/* ===== 信号 ===== */

long __sys_rt_sigaction(int sig, ...) {
    /* A20 无信号。返回成功但什么都不做（桩函数） */
    return 0;
}

long __sys_rt_sigprocmask(int how, ...) {
    return 0;
}

/* kill 映射到 channel 通知或 event 投递 */
long __sys_kill(int pid, int sig) {
    /* 通过 task_kill 发送信号编号作为 reason */
    a20_handle_t task = __pid_to_handle(pid);
    return __syscall2(__NR_a20_task_kill, task, sig);
}
```

#### 6.3.4 启动代码替换（`arch/a20/crt_arch.h`）

```c
/* arch/a20/crt_arch.h — 覆盖 musl 默认启动代码 */

/* musl 的 crt0 会调用 __libc_start_main。
 * 我们替换入口点，从 a20_start_info 初始化 */

void __a20_start(const struct a20_start_info *si)
{
    /* 1. 保存 start_info 指针 */
    __a20_start_info = si;

    /* 2. 确认 ABI 版本 */
    struct a20_abi_info abi;
    a20_abi_info(&abi);
    if (abi.abi_major != A20_ABI_MAJOR) {
        a20_task_exit(127);
    }

    /* 3. 初始化 fd 表 */
    __fd_table_init(si);

    /* 4. 初始化堆 */
    __malloc_init();

    /* 5. 设置 TLS */
    __init_tls();

    /* 6. 调用 musl 的 __libc_start_main */
    __libc_start_main(main, si->argc,
                      (char **)si->argv, _init, _fini, 0);
}
```

### 4.4 语义桥接——最难的部分

移植 musl 不只是换 syscall 编号。以下 POSIX 概念和 A20 概念**语义不同**，需要专门的桥接层。

#### 6.4.1 fork/exec → task_spawn

这是最大的语义鸿沟。musl 内部不直接调 `fork`，但用户程序大量使用。

**阶段 1：不支持 fork，只支持 posix_spawn**

```c
/* src/process/a20_fork.c */
pid_t fork(void) {
    errno = ENOSYS;
    return -1;
}

pid_t vfork(void) {
    errno = ENOSYS;
    return -1;
}

/* src/process/a20_posix_spawn.c */
int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr,
                char *const argv[], char *const envp[])
{
    /* 1. path_open 可执行文件 */
    struct a20_path_open_args oa = { ... };
    long ret = a20_path_open(&oa);
    int img_fd = oa.out_handle;

    /* 2. 设置 spawn 参数 */
    struct a20_task_spawn_args sa = {
        .image = img_fd,
        .root_dir = __root_dir_handle,
        .cwd_dir = __cwd_dir_handle,
        .argv = argv, .argc = ...,
        .envp = envp, .envc = ...,
    };

    /* 3. 传递 fd 表中的 handle */
    struct a20_spawn_handle handles[32];
    int nh = 0;
    for (int i = 0; i < __fd_table_size; i++) {
        if (__fd_table[i].handle != A20_HANDLE_NULL &&
            !(__fd_table[i].fd_flags & FD_CLOEXEC)) {
            handles[nh++] = (struct a20_spawn_handle){
                .handle = __fd_table[i].handle,
                .rights = 0,   /* 继承全部 rights */
                .target_slot = i,  /* 子进程中的目标 fd */
            };
        }
    }
    sa.handles = handles;
    sa.handle_count = nh;

    /* 4. task_spawn */
    ret = a20_task_spawn(&sa);
    if (ret < 0) return __a20_to_errno(ret);

    if (pid) *pid = __handle_to_pid(sa.out_task);
    return 0;
}
```

**阶段 2（可选）：fork 模拟**

```c
/* src/process/a20_fork_emul.c — 高级 fork 模拟 */
pid_t fork(void)
{
    /*
     * A20 的 task_spawn 是原子创建+加载。
     * 模拟 fork 需要：
     *
     * 1. task_spawn 当前程序的 ELF（A20_SPAWN_FORK_SELF flag）
     * 2. 内核创建子进程时：
     *    a. COW 复制父进程地址空间
     *    b. 复制 fd 表（handle dup + 相同 slot 号）
     *    c. 复制信号处置、cwd 等
     * 3. 子进程入口点不是 ELF entry，而是 fork 返回点
     * 4. 通过共享内存页传递 fork 上下文（返回值=0, 父 pid 等）
     *
     * 这需要 task_spawn 支持 A20_SPAWN_FORK_SELF flag，
     * 和内核配合做 COW + 状态快照。
     * 实现复杂度高，仅在有明确需求时才做。
     */
    return -ENOSYS;
}
```

#### 6.4.2 信号 → 事件队列

musl 内部使用信号做以下事情：
- `SIGCANCEL`：pthread 取消机制
- `SIGSETXID`：setuid/setgid 广播给线程组
- `SIGTIMER`：POSIX timer 到期通知
- `SIGCHLD`：子进程退出通知
- 用户注册的信号处理器（`sigaction`）

**桥接策略：信号桩 + 事件模拟**

```c
/* src/signal/a20_signal.c */

/* 桩函数——让 musl 编译通过，但不真正实现信号投递 */
int __sigaction(int sig, const struct sigaction *sa,
                struct sigaction *old, size_t masksize)
{
    /* 保存用户注册的 handler（用于 kill/sigqueue 模拟） */
    __signal_handlers[sig] = sa ? *sa : __signal_handlers[sig];
    if (old) *old = __signal_handlers[sig];
    return 0;
}

int raise(int sig) {
    /* 直接调用注册的 handler（同步方式） */
    if (__signal_handlers[sig].sa_handler &&
        __signal_handlers[sig].sa_handler != SIG_IGN &&
        __signal_handlers[sig].sa_handler != SIG_DFL) {
        __signal_handlers[sig].sa_handler(sig);
    }
    return 0;
}

/* kill 通过 channel 或 event 投递信号通知 */
int kill(pid_t pid, int sig) {
    /* 方案 A：通过 task_kill 发送 sig 作为 reason */
    a20_handle_t task = __pid_to_handle(pid);
    return a20_task_kill(task, sig);
}

/* pthread_cancel 不再基于信号 */
void __pthread_cancel_handler(struct __pthread *t) {
    /* 直接设置取消标志位，线程在取消点检查 */
    a_thread->cancel = 1;
    /* 如果线程在 event_wait 中，通过 event_cancel 唤醒 */
    a20_event_cancel(t->event_queue, t->thread_handle);
}
```

**关键决策**：A20 不实现"信号中断任意执行点"的 POSIX 语义。信号只在显式检查点（`event_wait` 返回时、`pthread_testcancel` 时）被处理。这是有意为之——异步信号中断是 POSIX 最严重的设计缺陷之一，A20 不应复制它。

#### 6.4.3 pthread → A20 thread

```c
/* src/thread/a20_pthread.c */

int pthread_create(pthread_t *res, const pthread_attr_t *attr,
                   void *(*entry)(void *), void *arg)
{
    struct __pthread *t = malloc(sizeof(*t));

    /* 1. 分配栈 */
    size_t stack_size = attr ? attr->_a_stacksize : DEFAULT_STACK_SIZE;
    struct a20_vm_alloc_args ma = {
        .size = sizeof(ma), .version = 1,
        .length = stack_size,
        .prot = 3, /* RW */
        .flags = 0,
    };
    a20_vm_alloc(&ma);
    t->stack = ma.out_addr;

    /* 2. 分配 TLS */
    size_t tls_size = __tls_size();
    struct a20_vm_alloc_args ta = {
        .size = sizeof(ta), .version = 1,
        .length = tls_size + sizeof(struct pthread),
        .prot = 3,
    };
    a20_vm_alloc(&ta);
    t->tls = ta.out_addr;
    __init_tls_for(t);

    /* 3. 创建 A20 线程 */
    struct a20_thread_create_args ca = {
        .size = sizeof(ca), .version = 1,
        .entry = (uint64_t)__pthread_entry,
        .arg = (uint64_t)t,
        .stack_base = t->stack,
        .stack_size = stack_size,
        .tls_base = t->tls,
        .flags = 0,
    };
    long ret = a20_thread_create(&ca);
    if (ret < 0) { free(t); return ret; }

    t->handle = ca.out_thread;
    t->entry = entry;
    t->arg = arg;
    *res = t;
    return 0;
}

/* 线程入口——包装用户函数 */
static void __pthread_entry(void *arg) {
    struct __pthread *t = arg;
    void *result = t->entry(t->arg);
    __pthread_set_result(t, result);
    /* 通知 joiner */
    if (t->join_event) {
        a20_event_notify(t->join_event);
    }
    a20_thread_exit(0);
}

int pthread_join(pthread_t t, void **res) {
    /* 等待目标线程退出 */
    struct a20_task_status status;
    a20_task_wait(t->handle, 0, &status);
    if (res) *res = t->result;
    return 0;
}
```

#### 6.4.4 pthread_mutex（futex 替代）

Linux 的 `pthread_mutex` 底层用 `futex` 系统调用做等待/唤醒。A20 没有 futex，需要替代方案。

**方案：基于 event_queue 的等待机制**

```c
/* src/thread/a20_mutex.c */

/* A20 的轻量级等待机制：
 * 利用 vm_alloc 的 MAP_SHARED 页做原子变量，
 * 用 event_queue 做阻塞等待。
 */

/* mutex 内部结构 */
struct a20_mutex_internal {
    _Atomic uint32_t state;      /* 0=unlocked, 1=locked, 2=contended */
    uint32_t type;               /* PTHREAD_MUTEX_NORMAL/RECURSIVE/... */
    uint32_t owner;              /* 持有者 thread id */
    uint32_t count;              /* 递归计数 */
    a20_handle_t wait_queue;     /* 等待队列（lazy 创建） */
};

int pthread_mutex_lock(pthread_mutex_t *pm) {
    struct a20_mutex_internal *m = (void *)&pm->__i;

    /* 快速路径：无竞争 */
    uint32_t expected = 0;
    if (atomic_compare_exchange_strong(&m->state, &expected, 1)) {
        m->owner = __pthread_self()->tid;
        return 0;
    }

    /* 慢路径：有竞争 */
    if (m->wait_queue == A20_HANDLE_NULL) {
        /* lazy 创建等待队列 */
        struct a20_event_queue_create_args eq = { ... };
        a20_event_queue_create(&eq);
        atomic_store(&m->wait_queue, eq.out_queue);
    }

    while (1) {
        /* 标记为 contended */
        atomic_store(&m->state, 2);

        /* 等待唤醒事件 */
        struct a20_event_wait_args ew = {
            .queue = m->wait_queue,
            .max_events = 1,
            .timeout_ns = A20_WAIT_FOREVER,
        };
        a20_event_wait(&ew);

        /* 尝试获取 */
        expected = 0;
        if (atomic_compare_exchange_strong(&m->state, &expected, 2)) {
            m->owner = __pthread_self()->tid;
            return 0;
        }
    }
}

int pthread_mutex_unlock(pthread_mutex_t *pm) {
    struct a20_mutex_internal *m = (void *)&pm->__i;
    m->owner = 0;

    uint32_t state = atomic_load(&m->state);
    if (state == 1) {
        /* 无竞争，直接释放 */
        uint32_t expected = 1;
        if (atomic_compare_exchange_strong(&m->state, &expected, 0))
            return 0;
    }

    /* 有等待者：释放并唤醒一个 */
    atomic_store(&m->state, 0);
    /* 向等待队列投递唤醒事件 */
    struct a20_event ev = { .type = A20_EVENT_WAKEUP };
    a20_event_post(m->wait_queue, &ev);
    return 0;
}
```

**性能说明**：快速路径（无竞争）是纯用户态原子操作，和 Linux futex 一样快。慢路径走 event_queue syscall，比 futex 多一次间接，但语义更清晰。event_queue 的通知机制避免了 futex 的内核-用户态 hash table 开销。

### 4.5 工作量估算

| 模块 | 新增/修改代码 | 难度 | 依赖 |
|------|-------------|------|------|
| `arch/a20/syscall.h` | ~80 行 | 低 | — |
| `arch/a20/crt_arch.h` | ~40 行 | 中 | 启动协议确定 |
| `arch/a20/bits/syscall.h` | ~300 行 | 低 | syscall 编号稳定 |
| syscall 映射层 | ~800 行 | 中 | fd 表 + 错误码映射 |
| fd↔handle 映射 | ~200 行 | 低 | — |
| pthread 桥接 | ~600 行 | 高 | thread_create + event |
| mutex/futex 替代 | ~400 行 | 高 | event_queue |
| 信号桩 | ~300 行 | 中 | — |
| fork 桩 + posix_spawn | ~200 行 | 中 | task_spawn |
| 错误码映射 | ~100 行 | 低 | — |
| **总计** | **~3020 行** | — | — |

**不动的代码**：musl 的 `string/`、`stdlib/`、`stdio/`（底层替换后）、`math/`、`regex/`、`locale/` 等共 ~45K 行——完全复用。

---

## 5. 编译工具链

### 5.1 目标 triple

```
aarch64-unknown-a20elf
```

因为 A20 和 Linux 在 aarch64 上共享相同的指令集和 ABI（AAPCS64），不需要新的 LLVM/GCC backend。只需要：

1. **新的 target triple**（区分 A20 ELF 和 Linux ELF）
2. **新的 sysroot**（A20 头文件 + musl-a20 + crt0.o + linker script）
3. **gcc/clang spec 文件**（覆盖默认链接行为）

### 5.2 交叉编译流程

```bash
# 方式 1：使用 clang（推荐，因为 clang 天然支持交叉编译）
clang --target=aarch64-unknown-a20elf \
      --sysroot=/opt/a20-sysroot \
      -isystem /opt/a20-sysroot/include \
      -L /opt/a20-sysroot/lib \
      -static \
      -o hello hello.c

# 方式 2：使用 gcc（需要构建 a20 target 的 cross-gcc）
aarch64-a20-gcc -static -o hello hello.c

# 方式 3：直接用 musl 的构建系统
cd musl && ./configure --target=aarch64-a20 --prefix=/opt/a20-sysroot
make && make install
```

### 5.3 sysroot 结构

```text
/opt/a20-sysroot/
├── include/
│   ├── a20/                    ← A20 Native ABI 头文件
│   │   ├── types.h             ← a20_handle_t, a20_rights_t, ...
│   │   ├── syscall.h           ← syscall 编号 + wrapper
│   │   ├── rights.h            ← 权限位定义
│   │   ├── errno.h             ← A20 错误码
│   │   └── startup.h           ← a20_start_info_t
│   ├── sys/                    ← POSIX 头文件（来自 musl）
│   ├── stdio.h                 ← ISO C 头文件（来自 musl）
│   └── ...
├── lib/
│   ├── crt0.o                  ← A20 启动代码
│   ├── crtbegin.o / crtend.o   ← GCC 构造/析构
│   ├── libc.a                  ← musl-a20 静态库
│   └── liba20rt.a              ← A20 syscall wrappers
├── a20.ld                      ← A20 ELF linker script
└── etc/
    └── a20-abi-version         ← ABI 版本检查
```

### 5.4 linker script 关键段

```ld
/* a20.ld — A20 Native ELF linker script */
ENTRY(_start)

SECTIONS {
    . = 0x400000 + SIZEOF_HEADERS;
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : {
        *(.note.*)
        *(.comment)
    }
}

PHDRS {
    PT_LOAD  PT_A20_START_INFO 0x70000001;
}
```

---

## 6. 实施路线图

### Phase 0：liba20rt 最小运行时（1-2 周）

**目标**：能让一个极简程序在 A20 Native ABI 上打印并退出。

```c
/* test_hello.c — Phase 0 测试程序 */
#include <a20/syscall.h>

void _start(const a20_start_info_t *si) {
    const char msg[] = "Hello from A20 Native ABI!\n";
    struct a20_iovec iov = { (uint64_t)msg, sizeof(msg) - 1 };
    struct a20_io_args args = {
        .size = sizeof(args), .version = 1,
        .handle = si->stdout_handle,
        .iov = (uint64_t)&iov, .iov_count = 1,
    };
    a20_handle_write(&args);
    a20_task_exit(0);
}
```

工作项：
- [x] syscall 发射宏（`a20_syscall6`）
- [x] 全部 90 个 syscall 编号定义
- [x] crt0 启动汇编（aarch64）
- [x] 简单测试：write stdout + exit

### Phase 1：liba20c 最小 C 库（2-4 周）

**目标**：能用标准 C 写程序。

```c
/* test_stdio.c — Phase 1 测试程序 */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    printf("Hello, argc=%d\n", argc);
    FILE *f = fopen("/test.txt", "w");
    fprintf(f, "A20 Native works!\n");
    fclose(f);
    return 0;
}
```

工作项：
- [ ] malloc/free/realloc（基于 vm_alloc 的 bump/slab 分配器）
- [ ] fd↔handle 映射表
- [ ] FILE* 实现（fopen/fread/fwrite/fclose/printf）
- [ ] POSIX open/read/write/close（基于 fd 表）
- [ ] string/stdlib（直接复用 musl 代码，0 改动）
- [ ] errno 映射
- [ ] 测试：hello world + 文件读写 + malloc

### Phase 2：musl 移植（4-8 周）

**目标**：能让 busybox 或 dropbear 等真实程序运行。

工作项：
- [ ] `arch/a20/` 全套（syscall.h, crt_arch.h, bits/syscall.h）
- [ ] syscall 映射层（`a20_syscallops.c`，~800 行）
- [ ] pthread → A20 thread 适配（~600 行）
- [ ] mutex → event_queue 适配（~400 行）
- [ ] 信号桩函数（~300 行）
- [ ] fork ENOSYS + posix_spawn via task_spawn（~200 行）
- [ ] 网络桥接（socket → net_socket, setsockopt → handle_control）
- [ ] sysroot 构建 + clang 集成
- [ ] 测试：busybox 基本命令、nc/socat 网络工具

### Phase 3：POSIX 完整兼容（按需）

**目标**：能运行 Python/Ruby 等需要完整 POSIX 的程序。

工作项：
- [ ] fork 模拟（COW + state transfer，需要内核 A20_SPAWN_FORK_SELF）
- [ ] 信号完整模拟（异步投递通过 event + channel）
- [ ] select/poll → event_wait 映射
- [ ] timerfd → A20 timer + event_queue
- [ ] inotify → event_watch_fs
- [ ] dlopen 动态加载（需要 A20 的动态链接器设计）

### 关键依赖关系

```text
Phase 0 ──→ Phase 1 ──→ Phase 2 ──→ Phase 3
  │            │            │            │
  │            │            │            └── 内核: A20_SPAWN_FORK_SELF
  │            │            │            └── 内核: 异步信号投递
  │            │            └── 内核: 全部 90 个 syscall 实现
  │            │            └── 内核: pthread TLS 支持
  │            └── 内核: ~15 个基础 syscall 实现
  └── 内核: 启动协议 + abi_info + handle_close + vm_alloc
          + handle_write + path_open + task_exit
```

每个 Phase 可以独立验证，不依赖后续 Phase 的内核功能。

---

## 7. Syscall 入口约定

### 7.1 架构相关调用约定

| 架构 | Syscall 号 | 参数 | 返回值 |
|------|-----------|------|--------|
| aarch64 | x8 | x0-x5 | x0 |
| riscv64 | a7 | a0-a5 | a0 |
| loongarch64 | a7 | a0-a5 | a0 |

### 7.2 通用约定

- 所有复杂 syscall 传结构体指针（通过 `a20_abi_header_t` 版本化）
- 简单 syscall（如 `handle_close(handle)`）可以直接传参数
- 返回 `a20_status_t`（>= 0 成功，< 0 错误）
- 内核不保留用户态寄存器（除返回值外）

---

## 8. Native 程序 ELF 要求

Native 程序的 ELF 文件应满足：

1. 包含 `PT_A20_START_INFO` 类型的 program header（值待定义），或通过约定位置传递 start_info。
2. 入口点（`e_entry`）不假设栈内容，只假设 a20_start_info 指针在约定寄存器中。
3. 不需要 interpreter（PT_INTERP）。liba20rt 静态链接。
4. 不需要 vDSO。所有 syscall 通过直接 trap 进入内核。
