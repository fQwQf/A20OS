# Native ABI 增添与深化设计

> 本文按 A20 原则（[02-system.md](../research/02-system.md)、 [06-formal-foundations.md](../research/06-formal-foundations.md)、 [06-security.md](06-security.md)）对 Native ABI 进行增添与深化： 盘点内核层为 Linux ABI 实现但尚未被 Native 包装的机制，按原则三分 （应包装 / 应拒绝 / 已有等价物），并为"应包装"项设计干净的 Native 接口， 同时收口现有接口的壳/受限语义。Syscall 编号沿用 [03-handle.md §6](03-handle.md) 的分区约定（0x0000-0x0FFF 稳定区）；结构体演进遵循 [01-types.md §2](01-types.md) 的 E-APPEND / E-DEPRECATE / E-RESERVED 规则。 **实现状态（2026-08）**：§2-§7 的应包装项与深化项均已实现并通过 `smoke-native-deepen`（Pager、monitor、task_mem、vm_share_region、 vm_protect capability、FS 事件、socket 事件源）；`check-abi-boundary` 与 `check-doc-drift` 通过。§8 的其余深化项为后续演进。

## 1. 机制三分清单

内核为 Linux ABI 实现了 21 类"独有内核机制"（io_uring、perf、userfaultfd、 epoll、inotify/fanotify、eventfd/timerfd/signalfd、SysV 三件套、POSIX mq、 keyring、pidfd、AIO、file-handle、mount-context、Landlock、cgroupfs/PSI、 drvmod、acct、rseq 等）。按 A20 原则逐类判定：

### 1.1 应包装（A20 对齐，用更干净的 Native 接口表达）

| Linux 机制 | 内核实现 | Native 形式 | 设计依据 |
|-----------|---------|------------|---------|
| userfaultfd | `kernel/ipc/userfaultfd.c` | **Pager**（PAGED VMO + pager channel 页供给） | P1 对象模型 + P3.2 EventQ；对齐 Zircon pager，去掉 Linux fd/ioctl 面 |
| inotify/fanotify | `kernel/fs/inotify.c` | **event_watch_fs 深化**（vnode-keyed FS 事件 → EventQ） | P3.2 统一事件等待；无全局 watch、以 dir handle 为界 |
| epoll + signalfd/timerfd/eventfd 组合 | `sys_epoll.c`、`ipc/eventfd.c` 等 | **EventQ 文件/网络事件源接线** | P6 明确拒绝组合拼装；EventQ 是替代，缺的是文件/套接字就绪事件源 |
| perf 软件事件 | `abi/linux/sys_perf.c` | **monitor 计数对象**（可订阅 EventQ） | P1.1 统一对象模型；计数器是对象而非 fd |
| process_vm_readv/writev | `mm/process_vm.c` | **task_mem_read / task_mem_write**（TASK handle + rights） | P5.1 最小权限；debug 接口只覆盖已停止目标，新增运行时权限化访问 |
| pidfd | `abi/linux/sys_pidfd.c` | 已有 **TASK handle** 等价物（`task_wait/kill/info` + `event_watch(EXITED)`） | — |
| memfd / memfd_secret | `fs/memfd.c` | 已有 **MEMORY handle** 等价物（`vm_create_object` + `vm_map`） | — |

### 1.2 应拒绝（A20 反模式，保持 Linux-only，不包装）

| Linux 机制 | 拒绝理由（A20 原则） | Native 已有替代 |
|-----------|---------------------|----------------|
| SysV IPC（shm/sem/msg） | P2.3 全局 ID；P6 明示拒绝 | channel + vm_share |
| POSIX mq | P2.3 全局命名；P6 | typed channel |
| io_uring | P6 opcode 大杂烩、fixed fd 无 rights | channel_call RPC + EventQ 完成通知 |
| Landlock / seccomp / LSM | P5.3 sandbox 是原生能力（spawn+rights+时态），非叠加层 | spawn 显式注入 + rights |
| cgroupfs | P2.1 全局层次；资源控制走权限化接口 | task_set_limits + VMO cgroup 记账 |
| 新 mount API（fscontext） | 内核配置面，非应用能力 | fs_mount/fs_umount |
| swap / mempolicy / NUMA | 单 NUMA 简化内核 | vm_advise / vm_flush |
| keyring | P2.3 全局 serial；安全模型是 capability+label，密钥即能力 | rights 降级 + channel 传递 |
| file-handle（name_to_handle_at） | P2.4 全局注册表 + 代次；TOCTOU 面 | dir handle 相对路径 + VMO |
| acct | 进程级记账，非对象化 | task_get_usage |
| rseq | A20 无任意迁移中断保证需求 | thread_get_cpu |
| drvmod / init_module | 特权加载面 | ext_prog (KEP) + device syscalls |
| PSI | 全局压力统计 | monitor 的全局计数模式 |

### 1.3 已有等价物（不新增）

ptrace ↔ debug handle（同一 proc_debug 状态机）；bpf ↔ ext_prog (KEP)； futex ↔ futex_wait/wake；channel IPC ↔ channel_*（已桥接）；定时器/时钟 ↔ timer_*； 网络 ↔ net_*；AIO 与 io_uring 同上（异步 I/O 走 channel_call + EventQ，见 §5）。

## 2. 新增：Pager（0x0D00）

**定位**：用户态驱动的按需分页。PAGED VMO 的缺页不静默填零，而是把页请求 作为 channel 消息交给 pager 线程；pager 用 `pager_supply_pages` 回填后唤醒 faulting 线程。对齐 Zircon `zx_pager_*`，语义更简单：VMO 直接关联请求端点， 无 ZX_WAIT/TIME 等额外对象。

```c
/* 创建 pager：返回 pager handle；请求端点作为 handle 传出（也可自行 dup/transfer）。 */
typedef struct a20_pager_create_args {
    uint32_t       size;
    uint32_t       version;
    uint32_t       flags;              /* 0；保留位必须为 0 */
    a20_handle_t   out_pager;          /* A20_OBJ_PAGER */
    a20_handle_t   out_requests;       /* A20_OBJ_CHANNEL_ENDPOINT：页请求消息 */
} a20_pager_create_args_t;

/* 创建与 pager 关联的 PAGED VMO。 */
typedef struct a20_pager_vmo_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   pager;              /* 需 A20_RIGHT_CONTROL */
    a20_handle_t   vmo;                /* 由 vm_create_object(PAGED) 产出，需 CONTROL */
} a20_pager_vmo_args_t;

/* 回填页：把 source VMO 的 [source_offset, +len) 拷入 paged VMO 的
 * [vmo_offset, +len)。len 必须页对齐。缺页线程随后被唤醒。 */
typedef struct a20_pager_supply_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   pager;              /* 需 A20_RIGHT_WRITE */
    a20_handle_t   vmo;                /* 需 A20_RIGHT_WRITE */
    a20_handle_t   source;             /* 源 MEMORY handle，需 A20_RIGHT_READ */
    uint64_t       vmo_offset;
    uint64_t       source_offset;
    uint64_t       len;
    uint64_t       out_supplied;
} a20_pager_supply_args_t;
```

**页请求消息**（pager 端 channel 消息体，`data0/data1` 由请求协议承载）：

| 字段 | 含义 |
|------|------|
| `type` | `A20_PAGE_REQ_READ` / `A20_PAGE_REQ_WRITE`（缺页访问类型） |
| `data0` | 缺失页的 vmo 内字节偏移 |
| `data1` | 预留（0） |

**语义**：

1. `pager_create` → 创建 `struct a20_pager`（refcount 对象）+ 一个 channel 对， 一端挂 pager 对象作为请求接收队列，一端作为 `out_requests` handle 返回。
2. `vm_create_object(size, flags)` 支持 `A20_VMO_PAGED`：`flags & A20_VMO_PAGED` 创建 `VMO_PAGED` VMO（`vmo_create(VMO_PAGED, ...)`）。
3. `pager_vmo_attach(pager, vmo)`：把 PAGED VMO 关联到 pager（VMO 持有 pager ref；pager 持有 VMO 的 watch 引用）。未关联的 PAGED VMO 缺页退回填零。
4. 缺页路径（`kernel/mm/vmo.c` 的 `vmo_get_page*` 增加 PAGED 分支）：
   - 页未物化且 VMO 关联 pager → 向 pager 请求 channel 发送页请求消息 （`a20_channel_send` 内部形式），faulting 线程 park 在 VMO 页级 wait-queue；
   - pager 收到请求 → `pager_supply_pages` 把源页拷入 VMO `pages[idx]`、 `arch_tlb_flush` 并唤醒该页的 waiters；
   - 唤醒后 faulting 线程重查页，已物化则继续，未物化则重新请求。
5. 权限：pager handle `{READ, WRITE, STAT, DUP, TRANSFER, CONTROL}`；PAGED VMO 与普通 MEMORY 一致。页供给不跨越 BLP 标签（源 label ≤ VMO label 才允许）。

**理由（相对 userfaultfd）**：去掉 fd/ioctl 面，改用 channel 消息 + handle rights；无 per-register 区间表，PAGED 由 VMO 对象自身承载；语义与 P3.2（EventQ）/ P1.1（对象模型）完全一致。适合：持久化内存、swap 用户态 pager、虚拟化内存后端、快照/迁移。

## 3. 新增：monitor 计数对象（0x0D10）

**定位**：perf 软件事件的 Native 形式。计数器是内核对象，可查询、可订阅 EventQ（定期/阈值触发），替代 Linux perf fd + mmap 环 + ioctl 的组合。

```c
typedef struct a20_monitor_create_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   target;             /* TASK handle，或 NULL=系统级 */
    uint32_t       kind;               /* A20_MONITOR_* */
    uint32_t       flags;
    a20_handle_t   queue;              /* 可选 EventQ，周期触发 notify */
    uint64_t       period_ns;          /* 采样/上报周期；0 = 无周期上报 */
    a20_handle_t   out_monitor;
} a20_monitor_create_args_t;

#define A20_MONITOR_TASK_CPU_TIME   1   /* 用户态累计 CPU 时间 */
#define A20_MONITOR_TASK_SYS_TIME   2   /* 内核态累计 CPU 时间 */
#define A20_MONITOR_TASK_PAGE_FAULTS 3  /* 累计缺页数 */
#define A20_MONITOR_TASK_CTX_SWITCH 4   /* 累计上下文切换数 */
#define A20_MONITOR_TASK_MIGRATIONS 5   /* 跨 CPU 迁移数 */
#define A20_MONITOR_SYS_PAGE_FAULTS 6   /* 系统级软件事件 */
#define A20_MONITOR_SYS_CTX_SWITCH  7

typedef struct a20_monitor_value {
    uint64_t       count;              /* 累计值 */
    uint64_t       time_active_ns;     /* monitor 启用以来的时间 */
    uint64_t       prev;               /* 上次读取值（delta 计算用） */
} a20_monitor_value_t;
```

**语义**：`monitor_create` 创建 `struct a20_monitor`（refcount），对目标 task （TASK handle，需 STAT）或系统级采样。`handle_read(MONITOR)` 返回 `a20_monitor_value_t`（需 READ）；`handle_control(op=READ)` 支持重置计数； 若指定 `queue`，采样 tick（由 scheduler/缺页/切换路径驱动）周期性地向 EventQ 投递 `A20_EVENT_SIGNALED`（data0 = 计数）。计数源直接读取现有内核统计 （`task->utime/stime`、`mm->faults`、切换/迁移计数），不做 PMU。

**理由（相对 perf）**：无 group leader/继承/采样环/mmap 等 Linux 面；计数器是 对象、可经 channel 传递（按 rights 降级）；订阅复用 EventQ。覆盖"我在这段时间 用了多少 CPU/触发了多少缺页"的监控与记账需求。

## 4. 新增：task_mem_read / task_mem_write（0x0211 / 0x0212）

**定位**：process_vm_readv/writev 的 Native 形式——对 TASK handle（需 READ 或 WRITE right）做跨进程内存访问。与 debug 接口不同：不要求目标停止。

```c
typedef struct a20_task_mem_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   task;               /* TASK handle：READ→read, WRITE→write */
    uint64_t       local_iov;          /* a20_iovec_t[] */
    uint32_t       local_iov_count;
    uint64_t       remote_iov;         /* a20_iovec_t[]（目标地址空间） */
    uint32_t       remote_iov_count;
    uint64_t       out_transferred;
} a20_task_mem_args_t;
```

**语义**：两段 iovec 逐对拷贝，目标地址空间在 `target->mm` 下逐页访问 （`mm->lock` + `pt_lookup_leaf`，逐页 copy_from/to_user 到内核 bounce buffer）， 单次调用总大小上限 64 KiB。目标页不存在时跳过该页（不向目标注入页）； 权限失败返回 `ACCESS`；目标退出返回 `BAD_HANDLE`。BLP：调用方 label 与目标 label 的读/写方向约束与 process_vm 等价（读目标 = no read up；写目标 = no write down）。

**理由**：debug_read/write 仅对 stopped 目标生效（`kernel/proc/debug.c`）； mlibc/调试器需要运行时只读目标。与 `debug_*` 的区别是"观察者模型"与 "权限化随机访问"的分工：观察者用 debug、常规访问用 task_mem。

## 5. 深化：EventQ 文件/网络事件源接线

**现状**：`a20_event_notify()` 只有 channel/timer/task-exit/udriver 调用； event_watch 对 FILE/SOCKET/DEVICE 目标永不触发（[08-runtime-status.md §7]）。 `handle_poll` 只是电平快照。

**设计**：在核心就绪状态变化点调用 `a20_event_notify`。对象键与现有 watch 一致——FILE/SOCKET 的 object 是全局 fd（`(void*)(uintptr_t)gfd`）。

| 事件源 | 触发点 | 事件 |
|--------|-------|------|
| socket | 收包入队 / rx 队列有数据（`socket_inet.c` lwip 回调、`socket.c` dequeue 后） | `READABLE` |
| socket | 发送缓冲释放 / connect 完成 | `WRITABLE` |
| socket | connect 完成（`socket_inet.c` BH_CONNECTED） | `CONNECTION` |
| socket | 监听队列有连接可 accept（accept_waitq 唤醒点） | `ACCEPT_READY` |
| socket | 对端关闭（recv 返回 0 / RST） | `ERROR` + `CLOSED` |
| pipe | 写端写入后（`pipe_wake_readers` 处） | 读端 `READABLE` |
| pipe | 读端消费后（`pipe_wake_writers` 处） | 写端 `WRITABLE` |
| file | 常规文件永久就绪（不投递，避免事件风暴） | — |

实现方式：`net_socket_t` 增加 `gfd` 字段（`net_socket_install_file` 写入）， 新增 `net_event_notify(s, event, d0, d1)` 薄包装；pipe_buf 持有两端 gfd， `pipe_wake_readers/writers` 内追加通知。事件为**边沿触发**（状态变化时投递）， 语义与 Linux epoll 边沿近似；`handle_poll` 继续提供电平查询。

**理由**：补齐 P3.2 承诺的"所有可观察对象都可被 watch"；是 event_watch_fs 与 monitor 的地基；不动 Linux ABI。

## 6. 深化：event_watch_fs 为真 FS 事件

**现状**：`sys_a20_event_watch_fs` 只是普通目录 watch，无路径过滤、不产生 FS 事件。

**设计**：FS watch 以 **vnode** 为对象键（`a20_event_notify(vnode, FS_WATCH, event, ...)`），dir handle 关闭时清理。VFS 变更路径投递事件：

| VFS 路径 | 事件 |
|---------|------|
| `vfs_mkdir` / `vfs_create`（O_CREAT） | `CREATE`（data0 = 名称） |
| `vfs_unlink` / `vfs_rmdir` | `DELETE` |
| `vfs_rename` | `RENAME_FROM` / `RENAME_TO` |
| `vfs_write`（大小变化或尾部写入） | `MODIFY` |
| `vfs_link` / `vfs_symlink` | `CREATE` |

新增事件位（`ipc/ipc.h`，追加编号避免破坏现有掩码）：

```c
#define A20_EVENT_FS_CREATE     16u
#define A20_EVENT_FS_DELETE     17u
#define A20_EVENT_FS_MODIFY     18u
#define A20_EVENT_FS_RENAME     19u
```

`event_watch_fs` 保持 `(dir handle, queue, event_mask, user_data)` 签名；可选 `path` 前缀过滤保留在 args（本期仅目录级）。事件 `data0` = 变化的子项名称 （内核 bounce buffer，最大 256B），`data1` = 变更类型标志。

**理由**：以 dir handle 为界（P2.1/P2.2），无 inotify 的全局 watch 号 （P2.3）；事件进 EventQ（P3.2），不新增 fd 化对象。

## 7. 深化：VMAR 与 vm_share

**现状**：`vm_share` 是裸 `(vmo, target, rights)`；`vm_protect` 不校验原 capability；`vm_flush(CLEAN)` 无范围写回；VMAR 非层级（[04-memory.md §3.3]）。

**设计**：

1. **vm_share_region 新增**（`0x030A`，`a20_vm_share_args_t`）：按地址区间反查 调用者地址空间内的 VM_VMO VMA，导出其背书的 VMO 为新的 MEMORY handle （`vm_share` 保留原有"向目标进程注入 VMO handle"的三参形式）：
   - `addr/length` 必须页对齐，且区间完整落在单个 VM_VMO VMA 内；
   - `out_handle` 为新 MEMORY handle，rights = VMA prot ∩ 请求 rights， 恒含 MAP/STAT/DUP/TRANSFER/CONTROL；
   - 非 VM_VMO 区间返回 `NOT_SUPPORTED`。
2. **vm_protect capability**：VMA 记录创建时 prot（`vmar_cap`，mmap 与 vm_map 均记录），`vm_protect` 只允许请求 ≤ 已存 cap；`vm_map` 的 EXEC 按 source handle 的 EXEC right 收紧（MEMORY 类型新增 EXEC right）。
3. **vm_flush(CLEAN)**：文件映射区间走缓存写回（全量 sync 覆盖文件范围）； VM_VMO 区间由 VMO 自持帧、无范围写回。
4. **VMAR 层级（部分）**：`vmar_cap` 落地为可查询能力位，完整 VMAR 树在 `vm_create_vmar` 单独演进。

**理由**：收口 08-runtime-status §7 列出的内存壳接口；prot_eff 契约 （04-memory §4.2）真正生效。

## 8. 其余深化项（设计收口，本期或后续）

| 项 | 现状 | 深化 |
|----|------|------|
| `thread_create` 返回类型 | 返回 TASK handle | 引入 `A20_OBJ_THREAD`（02 §4.2 已有编号）。**落地计划（2026-08 设计冻结）**：① proc_create_thread 发布 THREAD 类型 handle；② 全部 TASK 类型查找点改为接受 THREAD∪TASK：task_wait/kill/info、handle_poll 退出检查、debug 分区目标解析（kernel/proc/debug.c）；③ liba20rt/a20_types.h 与 mlibc sysdeps 同步；④ 回归矩阵：test_native_handle/contract/debug + smoke-native-ipc/mlibc-fork。风险点：NATIVE_HANDLE_CAPABILITY_CONSISTENCY_MATRIX 的 STAT 掩码一致性分区。 |
| `task_wait` flags | ~~忽略 flags~~ **已落地（05854979a）**：A20_TASK_WAIT_NONBLOCK 映射 WNOHANG，未知位拒绝；按 task 集合等待待需求明确后设计 |
| `handle_stat` 非文件类型 | ~~全零结构~~ **已落地**：MEMORY→total_blocks=size/4096、CHANNEL_ENDPOINT→total_files=msg_count，FILE/DIR 维持 block_size |
| `namespace` 强制 | `ns_apply` 只写字段 | pid/fs namespace 的路径解析与进程树可见性强制（逐步） |
| `event_watch_fs` 路径过滤 | 无 | 前缀匹配（本期后） |
| `clock_set` | ~~恒 PERM~~ **已落地**：安全标签 0（system）可设 CLOCK_REALTIME；monotonic 拒绝 INVALID_ARGUMENT |
| EventQ 电平模式 | ~~事件为边沿触发~~ **已落地**：`event_watch` args 按 E-APPEND 追加 `flags`，bit0=`A20_WATCH_LEVEL`（未知位拒绝）；等待侧 park 前（且不进入 readiness 子系统）对非 vfile 类型的 level watch 直接查询对象就绪位（通道端点：msg_count/peer_closed，与 handle_poll 分派一致），就绪即返回、不要求注册后发生状态迁移。回归：test_native_ipc 新增 level/edge 对照分区——同一已就绪端点上 edge watch 零超时返回 WOULDBLOCK、level watch 立即返回 READABLE+user_data。 |
| `vm_remap` 语义委托 | ~~"新建匿名区+字节拷贝"~~ **已落地**：改调 `mm_mremap(MREMAP_MAYMOVE)`，VMA 语义保留（文件映射/共享帧不丢失）；`a20_mm_errno_map` 完成 A20_ERR↔Linux errno 映射；prot≠0 时对结果区间 mm_mprotect；256MB 上限与范围校验留在 ABI |

## 9. 演进与兼容

- 新增 syscall 全部落在稳定分区未用编号（P4.4）：Pager `0x0D00-0x0D0F`、 monitor `0x0D10-0x0D1F`、task_mem `0x0211-0x0212`、vm_share_region `0x030A`。
- 新增结构体均以 `{size, version}` 开头（P4.1），version 1；追加字段用 E-APPEND（如 `a20_pending_event_t` 尾部追加 `fs_name[32]`，liba20rt 同步）。
- 新对象类型（`A20_OBJ_PAGER`=15、`A20_OBJ_MONITOR`=16）在 `ipc.h` 追加， 不在中间插入（不改变既有编号）。
- Linux ABI 侧不受影响：所有新增通知都在核心层，Linux 的 epoll/poll 继续 走既有 wait-queue 路径。

## 10. 参考

- [08-runtime-status.md §7](08-runtime-status.md)（现差距清单）
- [03-handle.md](03-handle.md)（对象模型）、[04-memory.md](04-memory.md)（VMO/VMAR）
- [05-ipc.md](05-ipc.md)（EventQ/channel）
- `kernel/ipc/userfaultfd.c`、`kernel/net/socket_inet.c`、 `kernel/fs/pipe.c`、`kernel/abi/linux/sys_perf.c`、`kernel/mm/process_vm.c`
