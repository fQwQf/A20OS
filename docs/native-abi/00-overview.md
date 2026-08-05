# A20 Native ABI 设计

本文概述 A20OS Native ABI。A20OS 的混合内核动机、双 ABI 与 Linux 兼容层的关系，以及整体架构决策，请阅读 [OS-Design.md](../OS-Design.md)。详细规范见各子文档。

## 设计定位

`abi/linux` 承担 Linux 用户态兼容职责，`abi/native` 面向原生用户态设计，不复制 POSIX 或 Linux syscall 编号。当前用户态主运行时是 Linux ABI 上的 musl 兼容层；Native ABI 在内核侧已实现 93 个 syscall 入口，用户态活跃组件是 `liba20rt`（Native SDK）和 `liba20c`（最小原生 C 库）。

长期布局：

```text
kernel/abi/linux/    Linux-compatible ABI subset（当前主用户态运行时）kernel/abi/native/   A20OS native ABI（内核入口已实现，用户态 SDK 为 liba20rt + liba20c）
```

Debug 分区当前是受限兼容实现，只支持基础寄存器快照和目标内存读写，不等价于完整 ptrace 语义。

## 核心原则

### 1. 一切资源都是 handle

Native ABI 不区分 Linux 风格的 fd、pid、tid、timerid、shmid 等编号。所有可操作资源由进程本地 handle table 引用。

```c
typedef uint32_t a20_handle_t;
```

**为什么采用 handle？**

> 我看 Windows NT 搞得不错，内核对象极大丰富，各类资源的调用差异基本消灭，面向对象，安全权能机制也受重视，如果再加上开源，Windows NT 就是我们理想中的操作系统内核。  ——fQwQf

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
new_rights 必须是 old_rights 的子集
```

14 个权限位的完整定义和 rights 代数见 [security.md](06-security.md)。

### 3. syscall 使用稳定结构体

复杂 syscall 不直接传一串裸参数，而是传结构体指针。所有结构体以 `size` 和 `version` 开头。详见 [types.md](01-types.md)。

规则：

1. 用户传入的 `size` 必须覆盖所声明 version 的完整必需前缀（version 1 即完整结构体）。
2. 用户传入的 `size` 大于内核支持结构体大小时，内核只读取已知字段。
3. 新字段只能追加并提高 `version`，不能改变已有字段含义。
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

Native ABI syscall 编号按子系统分区：

```text
0x0000 - 0x00ff  core / abi / system0x0100 - 0x01ff  handle0x0200 - 0x02ff  task / thread0x0300 - 0x03ff  memory0x0400 - 0x04ff  path / filesystem0x0500 - 0x05ff  ipc / event0x0600 - 0x06ff  net0x0700 - 0x07ff  time0x0800 - 0x08ff  security / namespace0x0900 - 0x09ff  debug / trace0x0a00 - 0x0aff  system info / random / power0x0b00 - 0x0bff  sync (futex)0x0c00 - 0x0fff  reserved for future core extensions0x1000 - 0x1fff  experimental, not stable
```

稳定 syscall 不允许随意改号。实验 syscall 只能在 `0x1000+` 范围内。完整的 93 个 syscall 编号表见 [handle.md](03-handle.md) §6。

## 文档索引

| 文档 | 内容 |
|------|------|
| [types.md](01-types.md) | 基础类型定义、ABI 头约定、所有 syscall 参数结构体 |
| [errors.md](02-errors.md) | 错误码定义、返回值约定、错误处理策略 |
| [handle.md](03-handle.md) | Handle 生命周期状态机、handle table 规范、13 种对象类型映射 |
| [memory.md](04-memory.md) | VMO/VMAR 内存模型、内存操作语义、共享内存与映射 |
| [ipc.md](05-ipc.md) | Channel IPC 协议、Event Queue 机制、partial delivery 状态机 |
| [security.md](06-security.md) | Rights 代数、handle transfer 语义、安全标签格、capability 安全模型 |
| [startup.md](07-startup.md) | 用户态启动协议、start_info 结构、初始 handle、libc 分层设计 |
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
┌──────────────────────────────────────────┐│         Linux musl 用户态（当前主运行时）      │├──────────────────────────────────────────┤│           liba20c（最小原生 C 库）            │  malloc, stdio, string, time├──────────────────────────────────────────┤│           liba20rt（Native SDK）             │  syscall wrapper, 启动代码, handle I/O├──────────────────────────────────────────┤│          kernel（Native ABI syscall 接口）    │└──────────────────────────────────────────┘
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

当前状态以 `abi/linux` 为主用户态接口。`abi/native` 内核侧入口完整，用户态 SDK 由 `liba20rt` 和 `liba20c` 组成。`liba20c` 已按 ABI 约定填充 `size` 和 `version` 字段，内核在 `kernel/abi/native/sys_validate.h` 中统一校验；任何不匹配都返回 `A20_ERR_INVALID_ARGUMENT`。

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

## 代码结构

```text
kernel/abi/native/DESIGN.md            顶层设计参考syscall_table.c      syscall 分发表syscall_table.def    syscall 编号宏定义（93 条）sys_core.c           Phase 1 syscall 实现（17 个核心 syscall）sys_phase2.c         Phase 2 syscall 实现（73 个扩展 syscall）handle_table.c       Handle table 实现（状态机 + 查找/安装/移除）startup.c            用户态启动协议a20_graceful.c       错误降级处理

kernel/ipc/a20_channel.c        Channel IPC 实现a20_event.c          Event Queue 实现

kernel/include/abi/native/types.h              用户可见类型定义errno.h              错误码常量rights.h             权限位定义syscall_nr.h         syscall 编号常量syscall_entry.h      syscall 入口约定startup.h            启动信息结构vmo.h / vmar.h       VMO/VMAR 内存模型ipc_internal.h       Channel/Event 内部接口fastpath.h           Syscall 快速路径（inline handle/rights/iov/bitmap）ring_spsc.h          Lock-free SPSC ring bufferresource.h           资源限制常量和检查函数

user/liba20rt/         当前活跃的 Native SDKa20_types.h          ABI 类型定义（与 kernel/include/abi/native/types.h 对应）a20_syscall.h        syscall wrapper 和编号常量a20_handle.h / a20_fs.h / a20_task.h / ...  高层封装头a20_simple_io.c      简化 I/O 实现crt0_*.S             多架构启动汇编

user/liba20c/          最小原生 C 库malloc.c / stdio.c / unistd.c / string.c / ...include/             ISO C 头文件子集fdtable.c            fd↔handle 映射

user/archive/          历史参考代码，不参与构建a20coreutils/        旧版原生 coreutils 示例src/                 旧版 musl 桥接实现build_sysroot.sh     旧版 sysroot 脚本（引用路径已过期）arch/a20/            旧版 musl arch 适配头tests/               旧版测试程序
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

## Syscall 完整性

Native ABI 用 93 个 syscall 覆盖 Linux ABI 分发表当前定义的 254 个 syscall。关键统一机制包括：

- `handle_set_meta`：一次调用修改 chmod/chown/utimes/truncate 等元数据。
- `handle_transfer`：统一 splice/sendfile/copy_file_range/tee 的零拷贝语义。
- `task_set_sched`/`task_get_sched`：统一 sched/priority/nice/affinity。
- `event_queue`：统一 epoll/select/poll/signalfd/timerfd/inotify。
- `security_get_context`/`security_set_context`：统一 uid/gid/cap。

完整分类对比、未覆盖 Linux 功能（由兼容层模拟）以及形式化讨论，见 [OS-Design.md](../OS-Design.md) §4 与 `docs/research/04-theory-deep-dive.md §9`。

## 当前状态与路线

已完成的阶段：

- Phase 0：`liba20rt` 最小运行时（syscall 发射宏、93 个 syscall 编号、多架构 crt0、hello world 测试）。
- Phase 1：`liba20c` 最小 C 库（malloc、fd↔handle 映射、FILE*、errno、基础 POSIX open/read/write/close 包装）。
- Phase 2：内核侧 93 个 syscall 扩展（task_spawn、thread_create、timer、channel、socket、VMO/VMAR 等）。

剩余工作：

- 修复 `liba20c` 中裸参数数组调用 syscall 的技术债，统一使用版本化 ABI 结构体。
- 扩展原生测试覆盖，从示例程序逐步建立回归套件。
- 按需实现 `A20_SPAWN_FORK_SELF` 与事件驱动信号模拟，以支持需要 fork 的复杂 POSIX 程序。
- 设计 Native ABI 动态链接器。

详细运行时状态与后续路线图见 [08-runtime-status.md](08-runtime-status.md)。
