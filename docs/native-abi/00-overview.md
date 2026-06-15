# A20 Native ABI Design

本文档是 A20OS Native ABI 设计的顶层概述。详细规范见各子文档。

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

长期布局：

```text
kernel/abi/linux/    Linux-compatible ABI subset（当前主用户态运行时）
kernel/abi/native/   A20OS native ABI（内核入口已实现，用户态 SDK 为 liba20rt + liba20c）
```

当前激活的用户态运行时是 Linux ABI 之上的 musl 兼容层。Native ABI 在内核侧已完成 90 个 syscall 入口；用户态活跃组件是 `liba20rt`（Native SDK）和 `liba20c`（最小原生 C 库）。`user/musl-port/` 目录在当前仓库中不存在，历史上与 musl 移植相关的参考材料已移至 `user/archive/`，仅作历史参考，不参与当前构建。Debug 分区当前是受限兼容实现，只支持基础寄存器快照和目标内存读写，不等价于完整 ptrace 语义。

## 核心原则

### 1. 一切资源都是 handle

Native ABI 不区分 Linux 风格的 fd、pid、tid、timerid、shmid、epoll fd 等多种编号。所有可操作资源都由进程本地 handle table 引用。

```c
typedef uint32_t a20_handle_t;
```

**为什么采用handle？**

> 我看 Windows NT 搞得不错，内核对象极大丰富，各类资源的调用差异基本消灭，面向对象，安全权能机制也受重视，如果再加上开源，Windows NT 就是我们理想中的操作系统内核。  
> 作者：fQwQf

13 种对象类型（详见 [handle.md](03-handle.md)）：

| 类型 | 说明 |
|------|------|
| task | 进程（地址空间 + handle table 容器） |
| thread | 线程（执行上下文） |
| file | 打开的文件 |
| dir | 打开的目录 |
| socket | 网络套接字 |
| pipe | 管道端点 |
| channel | IPC 通道端点 |
| eventq | 事件队列 |
| timer | 定时器 |
| shm | 共享内存对象 |
| device | 设备 |
| ns | 命名空间 |
| debug | 调试对象 |

handle 是进程本地编号，不是全局对象 ID。不同进程中的同一个数字不代表同一个对象。

### 2. handle 带 capability rights

每个 handle 都携带权限位。操作对象时不仅检查对象本身权限，也检查当前 handle 是否具备对应 capability。

权限只能降级，不能通过 `dup` 或 `transfer` 升级：

```text
new_rights must be subset of old_rights
```

14 个权限位的完整定义和 rights 代数见 [security.md](06-security.md)。

### 3. syscall 使用稳定结构体

复杂 syscall 不直接传一串裸参数，而是传结构体指针。所有结构体以 `size` 和 `version` 开头。详见 [types.md](01-types.md)。

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

版本规则：

- `abi_major` 改变表示不兼容变更。
- `abi_minor` 增加表示向后兼容新增功能。
- `abi_patch` 只表示 bugfix，不改变 ABI 表面。
- feature bits 用于检测可选能力。

结构体定义见 [types.md](01-types.md)，返回约定见 [errors.md](02-errors.md)。

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
0x0a00 - 0x0aff  system info / random / power
0x0b00 - 0x0fff  reserved for future core extensions
0x1000 - 0x1fff  experimental, not stable
```

稳定 syscall 不允许随意改号。实验 syscall 只能在 `0x1000+` 范围内。完整的 90 个 syscall 编号表见 [handle.md](03-handle.md) §6。

## 文档索引

| 文档 | 内容 |
|------|------|
| [types.md](01-types.md) | 基础类型定义、ABI 头约定、所有 syscall 参数结构体 |
| [errors.md](02-errors.md) | 错误码定义、返回值约定、错误处理策略 |
| [startup.md](07-startup.md) | 用户态启动协议、start_info 结构、初始 handle、libc 分层设计 |
| [handle.md](03-handle.md) | Handle 生命周期状态机、handle table 规范、13 种对象类型映射 |
| [memory.md](04-memory.md) | VMO/VMAR 内存模型、内存操作语义、共享内存与映射 |
| [ipc.md](05-ipc.md) | Channel IPC 协议、Event Queue 机制、partial delivery 状态机 |
| [security.md](06-security.md) | Rights 代数、handle transfer 语义、安全标签格、capability 安全模型 |
| [08-runtime-status.md](08-runtime-status.md) | 当前用户态运行时状态、已知偏差与路线图 |

## 与 Linux ABI 的关系

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

双 ABI 形式化隔离的完整证明见研究笔记 `docs/research/04-theory-deep-dive.md §9`。

## libc / runtime 设计

Native ABI 配套一个很薄的 native runtime，不直接改 musl。

当前结构：

```text
┌──────────────────────────────────────────┐
│         Linux musl 用户态（当前主运行时）      │
├──────────────────────────────────────────┤
│           liba20c（最小原生 C 库）            │  malloc, stdio, string, time
├──────────────────────────────────────────┤
│           liba20rt（Native SDK）             │  syscall wrapper, 启动代码, handle I/O
├──────────────────────────────────────────┤
│          kernel（Native ABI syscall 接口）    │
└──────────────────────────────────────────┘
```

- `liba20rt`：当前活跃的 Native SDK，提供 syscall wrapper、启动汇编、handle I/O 和 ABI 类型头。
- `liba20c`：最小 C 库，覆盖 malloc、stdio、string、time、errno 等 ISO C 子集。
- `user/archive/`：保存历史上的 musl 移植参考代码和旧版 coreutils，不参与当前构建。

不要一开始就承诺完整 POSIX。详见 [startup.md](07-startup.md)。

## 兼容策略

Native ABI 一旦稳定，需要遵守：

1. syscall 编号不重用。
2. 结构体字段只追加。
3. flag 保留位必须检查。
4. 默认行为不能随意改变。
5. 不兼容变更只能增加 `abi_major`。
6. 实验 syscall 必须留在 experimental 编号区。

## 实现状态

当前状态以 `abi/linux` 为主用户态接口。`abi/native` 内核侧入口完整，用户态 SDK 由 `liba20rt` 和 `liba20c` 组成。

| 组件 | 文件 | 状态 | 说明 |
|------|------|------|------|
| Linux ABI | `kernel/abi/linux/` | 活跃 | 当前主用户态运行时接口 |
| Native 核心 syscall | `kernel/abi/native/sys_core.c` | 已实现 | 17 个 Phase 1 syscall |
| Native 扩展 syscall | `kernel/abi/native/sys_phase2.c` | 已实现 | 73 个扩展 syscall，Debug 分区受限 |
| Handle table | `kernel/abi/native/handle_table.c` | 已实现 | handle 状态机 + 查找/安装/移除 |
| 启动协议 | `kernel/abi/native/startup.c` | 已实现 | 用户态 Native 启动 |
| Channel IPC | `kernel/ipc/a20_channel.c` | 已实现 | Channel 消息传递 |
| Event Queue | `kernel/ipc/a20_event.c` | 已实现 | 事件等待与通知 |
| Native SDK | `user/liba20rt/` | 活跃 | syscall wrapper、启动代码、handle I/O |
| 最小原生 C 库 | `user/liba20c/` | 活跃，存在技术债 | malloc/stdio/unistd 等仍使用裸参数数组调用 syscall，未统一使用版本化结构体 |
| 历史参考 | `user/archive/` | 不参与构建 | 旧版 musl 移植、coreutils、build_sysroot.sh 等 |
| musl 移植目录 | `user/musl-port/` | 不存在 | 该目录未在当前仓库中创建 |

已知偏差：

- `liba20c` 已按 ABI 约定填充 `size` 和 `version` 字段；内核在 `kernel/abi/native/sys_validate.h` 中统一校验这些字段，任何大小或版本不匹配都会返回 `A20_ERR_INVALID_ARGUMENT`。
- 当前原生测试只有少量示例（如 `user/tests/test_native_handle.c` 和 `user/liba20c` 内部测试），历史上宣称的 4 套 118 cases host-mode 测试套件目前不存在。
- `user/archive/build_sysroot.sh` 引用了 `user/musl-port/` 等不存在的路径，已不可直接使用。

## 最小可实现原型

以下是最初建议的最小 syscall 集，内核侧已实现并远超：

```text
0x0000 abi_info            ✅
0x0100 handle_close        ✅
0x0101 handle_dup           ✅
0x0102 handle_query         ✅
0x0200 task_exit            ✅
0x0300 vm_alloc             ✅
0x0301 vm_unmap             ✅
0x0400 path_open            ✅
0x0401 handle_read          ✅
0x0402 handle_write         ✅
0x0403 handle_stat          ✅
0x0500 event_queue_create   ✅
0x0501 event_watch          ✅
0x0502 event_wait           ✅
0x0700 clock_get            ✅
```

第二阶段也已在内核侧实现：

- ✅ `task_spawn`、`thread_create`
- ✅ `timer_create/set/cancel`
- ✅ message channel（`channel_create/send/recv`）
- ✅ socket（`net_socket/bind/connect/accept/listen/sendmsg/recvmsg/socketpair/shutdown/getname`）
- ✅ shared memory（VMO/VMAR 模型：`vm_create_object/vm_map/vm_share`）

## 代码结构

```text
kernel/abi/native/
  DESIGN.md            顶层设计参考
  syscall_table.c      syscall 分发表
  syscall_table.def    syscall 编号宏定义（90 条）
  sys_core.c           Phase 1 syscall 实现（17 个核心 syscall）
  sys_phase2.c         Phase 2 syscall 实现（73 个扩展 syscall，含 timer/thread/debug/namespace/net sendmsg 等）
  handle_table.c       Handle table 实现（状态机 + 查找/安装/移除）
  startup.c            用户态启动协议
  a20_graceful.c       错误降级处理

kernel/ipc/
  a20_channel.c        Channel IPC 实现
  a20_event.c          Event Queue 实现

kernel/include/abi/native/
  types.h              用户可见类型定义
  errno.h              错误码常量
  rights.h             权限位定义
  syscall_nr.h         syscall 编号常量
  syscall_entry.h      syscall 入口约定
  startup.h            启动信息结构
  vmo.h / vmar.h       VMO/VMAR 内存模型
  ipc_internal.h       Channel/Event 内部接口
  fastpath.h           Syscall 快速路径（inline handle/rights/iov/bitmap）
  ring_spsc.h          Lock-free SPSC ring buffer
  resource.h           资源限制常量和检查函数

user/liba20rt/         当前活跃的 Native SDK
  a20_types.h          ABI 类型定义（与 kernel/include/abi/native/types.h 对应）
  a20_syscall.h        syscall wrapper 和编号常量
  a20_handle.h / a20_fs.h / a20_task.h / ...  高层封装头
  a20_simple_io.c      简化 I/O 实现
  crt0_*.S             多架构启动汇编

user/liba20c/          最小原生 C 库
  malloc.c / stdio.c / unistd.c / string.c / ...
  include/             ISO C 头文件子集
  fdtable.c            fd↔handle 映射

user/archive/          历史参考代码，不参与构建
  a20coreutils/        旧版原生 coreutils 示例
  src/                 旧版 musl 桥接实现
  build_sysroot.sh     旧版 sysroot 脚本（引用路径已过期）
  arch/a20/            旧版 musl arch 适配头
  tests/               旧版测试程序
```

## 明确不做的事

Native ABI 不应该：

- 完整复刻 POSIX。
- 复制 Linux syscall 编号。
- 复制 Linux `ioctl` 大杂烩。
- 复制 Linux `clone` 的 flag 组合复杂度。
- 强制使用 uid/gid 作为唯一安全模型。
- 让内核核心模块依赖 native 用户结构体。
- 为了短期兼容测试牺牲接口清晰度。

## Syscall 完整性审计

### 审计方法

以已实现的 223 个 Linux syscall（`kernel/abi/linux/syscall_table.def`）为基准，逐类对照 Native ABI 的功能覆盖。目标：**Native ABI 必须能表达 Linux ABI 已实现的全部功能**，同时保持自身的设计一致性。

审计原则：
1. **不是 1:1 映射**：一个 Native syscall 可覆盖多个 Linux syscall（通过 args struct 统一参数变体）。
2. **不是 POSIX 复刻**：功能等价即可，接口形式不必相同（如用事件队列替代信号）。
3. **兼容层承担适配**：Linux 特有语义（cwd、fork/exec 两步、信号）由兼容层在 Native 之上模拟，Native 不必暴露对应原语。

### 覆盖统计

| 功能类别 | Linux syscall 数 | Native syscall 数 | 覆盖方式 |
|----------|-----------------|------------------|----------|
| Handle/FD 管理 | 11 | 13 | 直接映射 + handle_control 统一 fcntl/flock |
| 文件 I/O | 11 | 3 | handle_read/write 已含 scatter/gather+offset；handle_transfer 统一零拷贝 |
| 文件系统元数据 | 25 | 13 | handle_set_meta 统一 chmod/chown/utimes/truncate；xattr 4 个专用 syscall |
| 目录/命名空间 | 5 | 2 | dir handle 替代 cwd；ns_apply 替代 chroot |
| 进程/Task | 19 | 14 | task_spawn 替代 fork+exec；task_info 替代 getpid 等 |
| 调度 | 12 | 2 | task_set_sched/task_get_sched 统一全部 sched_* + priority |
| 内存 | 12 | 10 | vm_* 覆盖 mmap 系列 + madvise/mremap/mlock/memfd |
| 信号 | 9 | 0 | **范式差异**：事件队列模型替代信号（兼容层模拟） |
| 事件/Poll | 8 | 8 | event_queue 统一 epoll/select/poll；event_watch_fs 统一 inotify |
| 网络 | 15 | 10 | net_* + handle_control 统一 setsockopt/getsockopt |
| 时间 | 15 | 6 | clock_get/set/timer_* 覆盖 POSIX timer 全集 |
| 身份/安全 | 16 | 4 | security_get/set_context 统一 uid/gid/cap 操作 |
| 系统/Misc | 12 | 3 | system_info/random/reboot 覆盖核心系统查询 |
| **总计** | **223** | **90** | **平均 2.5× 压缩比** |

### 设计亮点

**1. 统一元数据操作（handle_set_meta）**

Linux 为 `chmod`/`chown`/`utimes`/`truncate`/`fallocate` 各维护独立 syscall（含路径变体共 12+）。A20 通过 flags 位图指定要修改的字段，一次调用可同时修改多个属性，减少 syscall 次数。例如：

```c
// Linux 需要 3 次 syscall
fchmod(fd, 0644);
fchown(fd, uid, gid);
futimes(fd, times);

// A20 只需 1 次
struct a20_set_meta_args args = {
    .handle = h,
    .flags = A20_SET_META_MODE | A20_SET_META_OWNER | A20_SET_META_MTIME,
    .mode = 0644,
    .uid = uid, .gid = gid,
    .mtime_ns = ts_ns,
};
handle_set_meta(&args);
```

**2. 统一零拷贝传输（handle_transfer）**

Linux 的 `splice`/`sendfile`/`copy_file_range`/`tee` 是 4 个独立 syscall，语义高度重叠。A20 统一为 `handle_transfer`，通过 flags 区分模式（消耗源 vs 不消耗），通过 offset 字段支持偏移。任何两个 handle 之间都可以零拷贝传输。

**3. 统一调度参数（task_set_sched/task_get_sched）**

Linux 的 10 个 `sched_*`/`*priority` syscall 被压缩为 2 个。flags 位图指定要操作的参数（策略/优先级/亲和性/nice），避免每个参数维度一个 syscall 的膨胀。

**4. 事件驱动替代信号**

Linux 的 9 个信号 syscall 在 Native ABI 中没有对应。这是**有意的范式差异**。信号模型存在根本性的设计缺陷（异步中断、可重入约束、与多线程冲突）。A20 用事件队列模型替代：进程间通知通过 channel 消息，定时器通过 timer → event_queue，异常通过 task_wait。兼容层负责在事件模型之上模拟 POSIX 信号语义。

**5. 安全上下文统一（security_get/set_context）**

Linux 的 16 个 uid/gid/capability syscall 被压缩为 2 个。`a20_security_context_t` 同时承载 POSIX 兼容身份（uid/gid/euid/egid/groups）和 A20 原生权限（effective_rights, namespace_mask），通过 flags 指定要查询/修改的字段。

### 未覆盖的 Linux 功能（兼容层职责）

以下 Linux 功能**有意不在 Native ABI 中暴露**，由兼容层在 Native 之上模拟：

| Linux 功能 | 原因 | 兼容层实现方式 |
|-----------|------|--------------|
| `fork`/`clone`/`vfork` | 进程创建应原子化（spawn） | `task_spawn` 模拟 fork（COW + 复制 handle table） |
| `execve`/`execveat` | A20 无"替换自己"语义 | 创建新 task + 迁移 handle + 终止旧 task |
| `getcwd`/`chdir`/`fchdir` | 无 cwd 概念 | 兼容层维护用户态 cwd 路径 + dir handle |
| `brk`/`sbrk` | vm_alloc 替代 | 兼容层用 `vm_alloc` 模拟 brk 语义 |
| `signal` 全系 | 事件驱动模型替代 | 兼容层在 channel/event 之上模拟信号投递 |
| `set_robust_list` | channel 的 peer_closed 替代 | 兼容层在 task 退出时扫描 handle table |
| `futex` | channel + event_queue 替代 | 兼容层用 `event_wait` + 共享内存实现 |
| `bpf` | 内核扩展，非基本功能 | 不提供 Native 等价（未来可通过 handle_control 扩展） |
| `personality` | ABI 兼容层职责 | 不提供 |

## 实施路线图

### Phase 0：liba20rt 最小运行时（已完成）

目标：让极简 Native 程序能打印并退出。

工作项：

- [x] syscall 发射宏（`a20_syscall6`）
- [x] 全部 90 个 syscall 编号定义
- [x] 多架构 crt0 启动汇编
- [x] 简单测试：write stdout + exit

### Phase 1：liba20c 最小 C 库（活跃，持续维护）

目标：能用标准 C 子集写 Native 程序。

工作项：

- [x] malloc/free/realloc（基于 vm_alloc 的 bump allocator + 空闲链表）
- [x] fd↔handle 映射表
- [x] FILE* 实现（fopen/fread/fwrite/fclose/printf）
- [x] POSIX open/read/write/close（基于 fd 表）
- [x] string/stdlib（直接复用或独立实现）
- [x] errno 映射
- [ ] 统一使用版本化 ABI 结构体（已知技术债，待修复）
- [x] 测试：hello world + 文件读写 + malloc

### Phase 2：完整 musl 移植（历史参考，不在当前构建中）

目标：让 busybox 或 dropbear 等真实程序运行。

历史尝试的代码保存在 `user/archive/`，包括 `arch/a20/` 适配头、`src/internal/a20_syscallops.c`、pthread 桥接、信号桩、`build_sysroot.sh` 等。这些代码不参与当前构建，也不保证路径正确。若未来重新启动完整 POSIX 兼容层，应基于该目录作为参考，而不是直接复用。

### Phase 3：POSIX 完整兼容（按需）

目标：能运行 Python/Ruby 等需要完整 POSIX 的程序。

工作项：

- [ ] fork 模拟（COW + state transfer，需要内核 A20_SPAWN_FORK_SELF）
- [ ] 信号完整模拟（异步投递通过 event + channel）
- [ ] select/poll → event_wait 映射
- [ ] timerfd → A20 timer + event_queue
- [ ] inotify → event_watch_fs
- [ ] dlopen 动态加载（需要 A20 的动态链接器设计）

### 关键依赖关系

```text
Phase 0 ──→ Phase 1 ──→ Phase 2（历史参考）──→ Phase 3（按需）
  │            │            │                      │
  │            │            │                      └── 内核: A20_SPAWN_FORK_SELF
  │            │            │                      └── 内核: 异步信号投递
  │            │            └── 参考实现已归档        └── 待实现
  │            ✅ 活跃维护    └── 内核: 全部 90 个 syscall ✅
  ✅ 已完成    └── 内核: ~15 个基础 syscall ✅
  └── 内核: 启动协议 + abi_info + handle_close + vm_alloc ✅
          + handle_write + path_open + task_exit ✅
```

每个 Phase 可以独立验证，不依赖后续 Phase 的内核功能。

## 总结

A20 native ABI 是一套基于 handle/capability 的现代系统接口。它的价值不是替代 Linux 兼容层，而是给 A20OS 一个清晰、自洽、长期可演进的原生用户态契约。

当前实现状态：

1. ✅ `abi/linux` 作为主 ABI 保持不变，是当前活跃的用户态运行时接口。
2. ✅ Native ABI 内核侧 90 个 syscall 入口已实现并文档化。
3. ✅ 内核侧完整实现（sys_core.c + sys_phase2.c + handle_table.c + startup.c + IPC）。
4. ✅ 用户态活跃 SDK 为 `liba20rt`，最小 C 库为 `liba20c`。
5. ❌ `user/musl-port/` 目录不存在；历史上的 musl 移植材料已归档到 `user/archive/`，不参与构建。
6. ⚠️ `liba20c` 仍使用裸参数数组调用 syscall，需要迁移到版本化 ABI 结构体。
7. ⚠️ 原生测试覆盖有限，当前仅有少量示例测试。
8. ⚠️ Debug handle 是有意受限的实现，不等价于完整 ptrace。

详细运行时状态与后续路线图见 [08-runtime-status.md](08-runtime-status.md)。

理论分析和形式化证明见 `docs/research/` 目录下的研究笔记体系（`00-index.md` 为入口）。
