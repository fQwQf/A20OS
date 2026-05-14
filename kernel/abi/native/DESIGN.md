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

13 种对象类型（详见 [handle.md](handle.md)）：

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

14 个权限位的完整定义和 rights 代数见 [security.md](security.md)。

### 3. syscall 使用稳定结构体

复杂 syscall 不直接传一串裸参数，而是传结构体指针。所有结构体以 `size` 和 `version` 开头。详见 [types.md](types.md)。

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

结构体定义见 [types.md](types.md)，返回约定见 [errors.md](errors.md)。

### 5. syscall 编号分区

Native ABI syscall 编号按子系统分区，便于扩展和阅读。

```text
0x0000 - 0x00ff  core / abi / system
0x0100 - 0x01ff  handle
0x0200 - 0x02ff  task / thread
0x0300 - 0x02ff  memory
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

稳定 syscall 不允许随意改号。实验 syscall 只能在 `0x1000+` 范围内。完整的 90 个 syscall 编号表见 [handle.md](handle.md) §6。

## 文档索引

| 文档 | 内容 |
|------|------|
| [types.md](types.md) | 基础类型定义、ABI 头约定、所有 syscall 参数结构体 |
| [errors.md](errors.md) | 错误码定义、返回值约定、错误处理策略 |
| [startup.md](startup.md) | 用户态启动协议、start_info 结构、初始 handle、libc 分层设计 |
| [handle.md](handle.md) | Handle 生命周期状态机、handle table 规范、13 种对象类型映射 |
| [memory.md](memory.md) | VMO/VMAR 内存模型、内存操作语义、共享内存与映射 |
| [ipc.md](ipc.md) | Channel IPC 协议、Event Queue 机制、partial delivery 状态机 |
| [security.md](security.md) | Rights 代数、handle transfer 语义、安全标签格、capability 安全模型 |

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

Native ABI 应配套一个很薄的 native runtime，而不是直接改 musl。

建议阶段：

1. `liba20rt`：只提供 syscall wrapper、启动代码、handle I/O。
2. `liba20c`：最小 C 库，支持 malloc、stdio、string、time。
3. POSIX shim：可选，将部分 POSIX API 映射到 native handle 模型。

不要一开始就承诺完整 POSIX。详见 [startup.md](startup.md)。

## 兼容策略

Native ABI 一旦稳定，需要遵守：

1. syscall 编号不重用。
2. 结构体字段只追加。
3. flag 保留位必须检查。
4. 默认行为不能随意改变。
5. 不兼容变更只能增加 `abi_major`。
6. 实验 syscall 必须留在 experimental 编号区。

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

- `task_spawn`、`thread_create`
- `timer_create`
- message channel
- socket
- shared memory（VMO/VMAR 模型）

## 实现建议

推荐代码结构：

```text
kernel/abi/native/
  DESIGN.md            本文档（顶层概述）
  types.md             类型与结构体定义
  errors.md            错误码
  startup.md           启动协议
  handle.md            Handle 子系统
  memory.md            内存子系统
  ipc.md               IPC 与事件子系统
  security.md          安全模型
  syscall_table.c      syscall 分发表
  syscall_table.def    syscall 编号宏定义
  syscall_impl.h       内部实现接口
  sys_core.c           0x00xx: abi_info
  sys_handle.c         0x01xx: handle 操作
  sys_task.c           0x02xx: task/thread 操作
  sys_memory.c         0x03xx: 内存操作
  sys_path.c           0x04xx: 文件系统操作
  sys_event.c          0x05xx: IPC/事件操作
  sys_net.c            0x06xx: 网络操作
  sys_time.c           0x07xx: 时间操作

kernel/include/abi/native/
  types.h              用户可见类型定义
  errno.h              错误码常量
  rights.h             权限位定义
  syscall_nr.h         syscall 编号常量
  syscall_entry.h      syscall 入口约定
  startup.h            启动信息结构
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

Linux 的 9 个信号 syscall 在 Native ABI 中没有对应——这是**有意的范式差异**。信号模型存在根本性的设计缺陷（异步中断、可重入约束、与多线程冲突）。A20 用事件队列模型替代：进程间通知通过 channel 消息，定时器通过 timer → event_queue，异常通过 task_wait。兼容层负责在事件模型之上模拟 POSIX 信号语义。

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

## 总结

A20 native ABI 应该是一套基于 handle/capability 的现代系统接口。它的价值不是替代 Linux 兼容层，而是给 A20OS 一个清晰、自洽、长期可演进的原生用户态契约。

短期最合理的路线是：

1. 保持 `abi/linux` 作为主 ABI。
2. 先文档化 native ABI（本文档体系）。
3. 等核心模块 API 更稳定后，实现最小 native runtime。
4. 用 native init 验证启动、输出、退出。
5. 再逐步加入 spawn、event、IPC、network 和 sandbox。

理论分析和形式化证明见 `docs/research/` 目录下的研究笔记体系（`00-index.md` 为入口）。
