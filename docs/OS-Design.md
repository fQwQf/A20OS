# A20OS 操作系统设计方案

**A20 战队 · 武汉大学 · 2026 全国大学生计算机系统能力大赛 OS 内核实现赛道**

---

## 目录

1. [项目概述与核心目标](#1-项目概述与核心目标)
2. [总体架构](#2-总体架构)
3. [硬件抽象层与跨平台设计](#3-硬件抽象层与跨平台设计)
4. [Native ABI 设计](#4-native-abi-设计)
5. [Linux ABI 兼容层设计](#5-linux-abi-兼容层设计)
6. [内存管理](#6-内存管理)
7. [进程调度与 SMP 并发](#7-进程调度与-smp-并发)
8. [文件系统与 VFS](#8-文件系统与-vfs)
9. [网络栈](#9-网络栈)
10. [驱动模型](#10-驱动模型)
11. [测试与质量保证](#11-测试与质量保证)
12. [核心挑战与解决方案](#12-核心挑战与解决方案)
13. [第三方代码与协议声明](#13-第三方代码与协议声明)
14. [未来工作路线图](#14-未来工作路线图)

---

## 1. 项目概述与核心目标

A20OS 是一款面向下一代操作系统架构探索的混合内核（Hybrid Kernel）。项目以"兼容现有生态、探索未来接口"为双轨目标，在单一内核中同时提供高保真 Linux 用户态兼容层和面向能力的 Native ABI。系统当前代码规模约 12.5 万行，涵盖 533 个源文件，核心原创代码位于 `kernel/` 目录，用户态工具与第三方代码严格隔离于 `user/external/` 与 `kernel/external/`。

本项目的核心目标可概括为以下四点：

* **架构先进性**：将宏内核的执行效率与微内核的逻辑抽象结合，引入基于 Capability 的对象句柄、VMO/VMAR 内存模型与 Channel/EventQ IPC。
* **生态兼容性**：通过 `kernel/abi/linux/` 实现 223 个 Linux 系统调用，支撑 musl、git、vim、fastfetch 等复杂用户态程序。
* **跨平台可移植性**：官方维护 QEMU 四架构（RISC-V、ARM64、x86_64、LoongArch）及 VisionFive 2、Loongson LS2K1000 物理板。
* **工程可验证性**：建立文档化锁契约、压力测试门禁与多架构构建矩阵，确保每次迭代可复现、可审计。

---

## 2. 总体架构

### 2.1 混合内核与双重 ABI

A20OS 从运行空间视角属于宏内核：驱动、网络栈、文件系统与内存管理均在同一特权地址空间运行，函数调用路径短、中断响应快。从逻辑抽象视角，它又吸收了微内核思想：所有资源通过对象句柄访问，IPC 通过 Channel 与 EventQ 完成，内存通过 VMO/VMAR 解耦。

双重 ABI 是系统最显著的架构特征：

| ABI 层 | 路径 | 定位 | 关键抽象 |
|--------|------|------|----------|
| Linux ABI | `kernel/abi/linux/` | 兼容现有生态 | fd、pid、signal、fcntl、epoll |
| Native ABI | `kernel/abi/native/` | 面向未来设计 | handle、rights、VMO/VMAR、Channel、EventQ |

两层 ABI 在设计上严格隔离：核心内核模块只暴露与 ABI 无关的内部 API，`abi/linux` 与 `abi/native` 分别将各自的用户调用翻译为同一组核心调用。禁止 `abi/native` 调用 `abi/linux` 的实现，也禁止核心模块依赖任何 ABI 用户结构体。

### 2.2 内核模块组织

`kernel/` 目录按功能垂直划分，关键模块与代码规模如下（数据来自初赛汇报材料）：

| 模块 | 路径 | 约计代码行数 | 核心职责 |
|------|------|-------------|----------|
| 硬件抽象 | `kernel/arch/`, `kernel/platform/` | 30k+ | 启动、中断、页表、板级配置 |
| 内存管理 | `kernel/mm/` | 22k+ | VMO/VMAR、页表、COW、Page Cache、OOM |
| 文件系统 | `kernel/fs/` | 18k+ | VFS、FAT32、ext4、ramfs、伪文件系统 |
| 进程调度 | `kernel/proc/` | 15k+ | 任务、调度器、fork/exec、signal、Futex |
| 系统调用 | `kernel/abi/`, `kernel/syscall/` | 14k+ | 双重 ABI 分发与语义实现 |
| 设备驱动 | `kernel/drivers/` | 12k+ | virtio-blk、virtio-net、UART、PTY、loop |
| 网络 | `kernel/net/` | 8k+ | Socket 层、lwIP 集成、DHCP/配置 |
| 核心基础设施 | `kernel/core/` | 6k+ | 锁、时间、panic、progress 机制 |
| IPC | `kernel/ipc/` | 并入核心 | Channel、Event、SysV、eventfd、timerfd |

模块依赖遵循自底向上原则：`arch` 与 `core` 位于最底层，`abi` 层位于最顶层，中间由 `mm`、`proc`、`fs`、`net`、`drivers`、`ipc` 构成可复用的核心服务。

---

## 3. 硬件抽象层与跨平台设计

### 3.1 支持的硬件目标

A20OS 的硬件抽象层（HAL）通过 `kernel/arch/` 与 `kernel/platform/` 实现。当前官方维护的目标平台包括：

* **QEMU 虚拟机**：`qemu-virt-riscv64`、`qemu-virt-aarch64`、`qemu-virt-x86_64`、`qemu-virt-loongarch64`
* **物理开发板**：StarFive VisionFive 2（RISC-V）、Loongson LS2K1000（LoongArch）

### 3.2 板级配置抽象

每个平台在 `kernel/platform/<board>/` 中提供 `board_config_t`，包含 RAM 范围、中断控制器、定时器、早期初始化、电源管理与设备枚举函数。驱动的 MMIO 访问不直接操作 CSR，而统一通过 `kernel/include/drivers/hwapi.h` 中的内联 `readl`/`writel`，保证跨平台代码可移植。

### 3.3 构建矩阵

项目使用单一顶层 `Makefile` 完成跨架构构建。典型命令包括：

```bash
make ARCH=riscv64 BOARD=qemu-virt-riscv64 run
make ARCH=aarch64 BOARD=qemu-virt-aarch64 run
make ARCH=x86_64 BOARD=qemu-virt-x86_64 run
make ARCH=loongarch64 BOARD=qemu-virt-loongarch64 run
```

`make check-kernel-build` 与 `make check-user-build` 负责验证四架构 bringup 构建是否通过。

---

## 4. Native ABI 设计

Native ABI 是 A20OS 为下一代用户态接口设计的核心资产。它不复制 POSIX，也不复用 Linux 系统调用编号，而是基于 handle/capability 构建一套小型、版本化、可扩展的接口。

### 4.1 Capability Handle 能力系统

Native ABI 将 fd、pid、tid、timerid、shmid 等 Linux 繁杂标识符统一为进程本地句柄 `a20_handle_t`（32 位无符号整数）。每个 handle 携带 14 位 rights 掩码，并可选设置过期时间与剩余操作次数。系统当前定义 13 种对象类型，对应 `kernel/abi/native/handle_table.c` 中的状态机。

| 对象类型 | 内核结构 | 引用计数 |
|----------|----------|----------|
| task / thread | `task_t *` | 由 proc 管理 |
| file / dir / device / pipe | `vfile_t *` | `ref_count` |
| socket | `struct a20_socket` | `refcount_t` |
| channel endpoint | `struct a20_channel_ep` | `refcount_t` |
| event queue | `struct a20_eventq` | `refcount_t` |
| timer | `struct a20_timer` | `refcount_t` |
| shared memory | `struct a20_shm` | `refcount_t` |
| namespace | `struct a20_namespace` | `refcount_t` |
| debug | `struct a20_debug` | `refcount_t` |

14 个 rights 位定义于 `kernel/include/abi/native/rights.h`，包括 READ、WRITE、EXEC、STAT、SEEK、DUP、TRANSFER、MAP、WAIT、CONNECT、ACCEPT、CONTROL、ADMIN、SIGNAL。任何 `dup` 或 `transfer` 操作只能缩小权限，不能扩大。

### 4.2 VMO/VMAR 内存模型

Native ABI 将物理内存与虚拟地址布局解耦为两个核心抽象：

* **VMO（Virtual Memory Object）**：物理页容器，可被 resize、共享、按页 fault。实现于 `kernel/mm/a20_vmo.c`。
* **VMAR（Virtual Memory Address Region）**：进程地址空间中的连续区域，负责映射与权限。实现于 `kernel/mm/a20_vmar.c`。

VMO 与 VMAR 的交互路径为：`vm_alloc` / `vm_map` / `vm_share` 创建或映射 VMO，`vm_protect`、`vm_unmap`、`vm_remap` 调整映射。有效保护位由请求保护、handle rights 与 VMAR 标志三者取交集决定，公式记录于 `docs/native-abi/04-memory.md`。

### 4.3 Channel 与 EventQ

Native IPC 基于两个互补原语：

* **Channel**：双向消息通道，单条消息最多 64 KiB 数据 + 8 个 Handle。`kernel/ipc/a20_channel.c` 实现两阶段写入与类型化通道约束。
* **EventQ**：统一事件等待机制，替代 epoll/signalfd/timerfd。`kernel/ipc/a20_event.c` 维护 watch list、ring buffer 与全局反向索引。

Channel 传递 handle 时遵循 `ρ_recv = ρ_send ∩ ρ_transfer` 的权限交集规则，且采用共享语义而非移动语义，发送方在 send 后仍持有原 handle。

### 4.4 安全模型

安全模型建立在 rights lattice 与 Bell-LaPadula 多级安全标签之上。标签集合为 `{L, M, H}`，并满足：

* 简单安全性（no-read-up）：主体密级不低于对象密级时方可读。
* 星属性（no-write-down）：主体密级不高于对象密级时方可写。

此外，handle 支持时态能力（Temporal Capability），可通过 `expiry_tick` 与 `remaining_ops` 限制权限生命周期，并由 sweeper 在 `AUTO_CLOSE` 模式下自动回收。详细形式化描述见 `docs/native-abi/06-security.md`。

### 4.5 启动协议

Native 程序启动时，内核通过寄存器（如 aarch64 的 x0）传递 `a20_start_info_t` 指针。该结构包含 argc/argv/envp、page size、初始 handle 列表（root_dir、cwd_dir、stdin、stdout、stderr、self_task、main_thread、default_event_queue）。启动代码位于 `user/liba20rt/crt0_*.S`，运行时分层为 Kernel -> liba20rt -> liba20c -> POSIX shim。

### 4.6 系统调用压缩对比

Native ABI 当前在内核侧实现 90 个系统调用入口，定义于 `kernel/abi/native/syscall_table.def`。与已实现的 223 个 Linux 系统调用相比，压缩比约为 2.5 倍。分类对比如下：

| 功能类别 | Linux syscall 数 | Native syscall 数 | 关键统一机制 |
|----------|-----------------:|------------------:|--------------|
| Handle / FD 管理 | 11 | 13 | `handle_control` 统一 fcntl/flock |
| 文件 I/O | 11 | 3 | `handle_read/write` 含 scatter/gather + offset |
| 文件系统元数据 | 25 | 13 | `handle_set_meta` 统一 chmod/chown/utimes/truncate |
| 目录 / 命名空间 | 5 | 2 | dir handle 替代 cwd；`ns_apply` 替代 chroot |
| 进程 / Task | 19 | 14 | `task_spawn` 替代 fork+exec |
| 调度 | 12 | 2 | `task_set_sched` / `task_get_sched` 统一参数 |
| 内存 | 12 | 10 | `vm_*` 覆盖 mmap/mremap/madvise/memfd |
| 信号 | 9 | 0 | 事件队列模型替代信号 |
| 事件 / Poll | 8 | 8 | `event_queue` 统一 epoll/select/poll |
| 网络 | 15 | 10 | `net_*` + `handle_control` 统一 socket 选项 |
| 时间 | 15 | 6 | `clock_get/set` + `timer_*` |
| 身份 / 安全 | 16 | 4 | `security_get/set_context` 统一 uid/gid/cap |
| 系统 / Misc | 12 | 3 | `system_info/random/reboot` |
| **总计** | **223** | **90** | 平均压缩比约 2.5 倍 |

Native ABI 的 syscall 编号按子系统分区，定义于 `kernel/include/abi/native/syscall_nr.h`，便于扩展与阅读：

| 编号区间 | 子系统 |
|----------|--------|
| 0x0000 - 0x00ff | core / abi / system |
| 0x0100 - 0x01ff | handle |
| 0x0200 - 0x02ff | task / thread |
| 0x0300 - 0x03ff | memory |
| 0x0400 - 0x04ff | path / filesystem |
| 0x0500 - 0x05ff | ipc / event |
| 0x0600 - 0x06ff | net |
| 0x0700 - 0x07ff | time |
| 0x0800 - 0x08ff | security / namespace |
| 0x0900 - 0x09ff | debug / trace |
| 0x0a00 - 0x0aff | system info / random / power |
| 0x0b00 - 0x0fff | reserved for future core extensions |
| 0x1000 - 0x1fff | experimental, not stable |

所有复杂 syscall 参数结构均以 `a20_abi_header_t` 开头，包含 `size` 与 `version` 字段，支持运行时版本协商。稳定 syscall 编号不重用，结构体字段只追加，flag 保留位必须为零。

---

## 5. Linux ABI 兼容层设计

Linux ABI 兼容层位于 `kernel/abi/linux/`，目标是在不修改源码的前提下运行静态链接的 musl 程序。当前已实现 223 个 Linux 系统调用入口，定义于 `kernel/abi/linux/syscall_table.def`。

### 5.1 实现策略

兼容层将 Linux 系统调用翻译为核心模块 API，关键文件包括：

* `kernel/abi/linux/sys_path.c`：路径、openat2、renameat2、statx、xattr
* `kernel/abi/linux/sys_proc.c`：进程、clone、exec、wait、exit
* `kernel/abi/linux/sys_mm.c`：mmap、mremap、madvise、brk、mprotect
* `kernel/abi/linux/sys_futex.c`：Futex 等待与唤醒
* `kernel/abi/linux/sys_sched.c`：调度策略、亲和性、优先级
* `kernel/abi/linux/sys_net.c` 与 `sys_socket_msg.c`：socket、TCP/UDP、unix domain

### 5.2 复杂边界语义

系统特别处理了以下高复杂度语义：

* `openat2`：`RESOLVE_NO_SYMLINKS`、`RESOLVE_BENEATH`、`RESOLVE_IN_ROOT`、`RESOLVE_NO_XDEV` 等解析标志。
* `renameat2`：`RENAME_NOREPLACE`、`RENAME_EXCHANGE`、跨 mount 返回 `-EXDEV`。
* `statx`：mask 处理与 sync type。
* `faccessat2` / `fchmodat2`：`AT_*` flag 校验。
* Futex：`FUTEX_WAIT`、`FUTEX_WAKE`、`FUTEX_REQUEUE` 及私有/共享变量语义。

兼容层不追求 100% 覆盖所有 Linux 行为，但对 musl、git、vim、fastfetch、mksh 等实际程序运行所需路径提供高保真实现。

---

## 6. 内存管理

A20OS 内存管理子系统位于 `kernel/mm/`，核心文件包括 `vm.c`、`a20_vmo.c`、`a20_vmar.c`、`fault.c`、`page_cache.c`、`oom.c`。

### 6.1 mm_struct 锁模型

`mm_struct` 中的 `spinlock_t lock` 是多核 VMA 保护的关键。`kernel/mm/vm.c` 摒弃传统大内核锁，将 `mmap`/`munmap` / `mprotect` / page fault 路径收紧为仅持有 `mm->lock`，保证多核并行的内存映射操作安全。

### 6.2 大页降级

当 fork 创建子进程或 OOM 回收遇到已被映射的 2 MiB 大页时，系统调用 `mm_demote_huge_page()` 将其安全拆分为 4 KiB 页，并在拆分过程中通过 `mm->lock` 隔离并发的缺页异常。

### 6.3 写时复制（COW）

fork 时，`mm_fork_clone_present_level()` 为子进程建立只读 COW 映射。写操作触发 page fault 后，内核分配新物理页并复制内容，再更新页表项。COW 路径同时支持 Native ABI 的 `handle_dup` 与 Linux ABI 的 fork/clone。

### 6.4 MAP_SHARED 一致性

多进程通过 `MAP_SHARED` 映射同一文件时，Page Cache 的 dirty-page/writeback 生命周期由 `kernel/fs/page_cache.c` 统一管理。页面逐出路径中增强一致性栅栏，确保多核压力下 VFS 层数据一致。

---

## 7. 进程调度与 SMP 并发

### 7.1 锁契约

A20OS 采用严格的局部锁层级契约替代全局大锁。全局顺序定义于 `kernel/include/core/lock.h`：

```text
cg_node.lock -> proc_lock -> runq_lock -> pfa.lock
proc_lock -> files_struct.lock -> VFS global-file/vnode locks
proc_lock -> mm_struct.lock
proc_lock -> a20_handle_table.lock
driver registry/IRQ locks -> device-private locks
g_lwip_lock -> g_net_lock
g_lwip_lock -> virtio-net nonblocking send/recv paths only
```

核心规则包括：

* 禁止在持有 `runq_lock` 时获取 `proc_lock`。
* 禁止持有 spinlock 时阻塞或睡眠。
* 禁止在持有设备或 lwIP 锁时调用 VFS、内存分配或调度路径，除非被调用方明确记录为非阻塞。

### 7.2 调度器结构

调度器实现于 `kernel/proc/sched.c`，采用 Per-CPU 独立运行队列，消除全局调度锁。每个运行队列维护 8 级优先级与 bitmap，实现 O(1) 快速选任务。调度器支持老化提升（aging promotion）、`SCHED_FIFO/RR`，并与 `kernel/proc/cg_cpu.c` 中的 cgroup CPU quota 联动实现限流调度。

### 7.3 Futex

Linux ABI 的 Futex 实现于 `kernel/abi/linux/sys_futex.c`，支持等待、唤醒、重排队及私有/共享键。Futex 与 Native ABI 的 event_queue 在语义上互补：Linux 兼容层使用 Futex 实现 pthread 同步原语，Native 用户态则可直接使用 `event_wait`。

---

## 8. 文件系统与 VFS

### 8.1 模块化 VFS

`kernel/fs/vfs.c` 作为超级分发器，抽象了 `vnode` 与 `vfile` 接口。关键文件包括：

* `kernel/fs/vfs/path_resolution.c`：路径解析、mount root、`..` 穿越、符号链接深度限制。
* `kernel/fs/vfs/mount_ops.c`：挂载与卸载。
* `kernel/fs/vfs/dcache.c`：目录项缓存。
* `kernel/fs/vfs/stat_perm.c`：权限检查与 sticky-bit。

### 8.2 文件系统后端矩阵

系统当前支持的文件系统后端及其能力如下：

| 后端 | rename | link | symlink | 关键限制 |
|------|--------|------|---------|----------|
| FAT32 | 不支持 | 不支持 | 不支持 | 元数据仅存 RAM；无 rename/link/symlink |
| ext4 | 支持 | 不支持 | 快链 <= 60 B | 无日志；`st_nlink` 硬编码为 1 |
| ramfs | 支持 | 支持 | 支持 | 单目录 entry 上限 256；总 inode 上限 4096 |
| pipe | 不适用 | 不适用 | 不适用 | `PIPE_BUF` 原子写 |
| devfs | 不支持 | 不支持 | 不支持 | 节点编译期固定；只读设备树 |
| procfs | 不支持 | 不支持 | 不支持 | 合成文件；open 时生成快照 |
| sysfs | 不支持 | 不支持 | 不支持 | 当前仅暴露 `/sys/block/loopN` |

### 8.3 边界语义

* `openat2`：完整支持 `RESOLVE_NO_SYMLINKS`、`RESOLVE_BENEATH`、`RESOLVE_IN_ROOT`、`RESOLVE_NO_XDEV`。
* `renameat2`：支持 `RENAME_NOREPLACE` 与 `RENAME_EXCHANGE`，`RENAME_WHITEOUT` 因缺少 overlayfs 返回 `-EINVAL`。
* 符号链接深度：已规划从 8 提高到 Linux 标准的 40（`MAX_SYMLINKS`）。
* mount point `..` crossing：计划实现逃出 mount root 的语义，与 `chroot` 边界协同。

路径解析器 `vnode_lookup_path` 维护 resolution context，在处理 `..` 时检查当前节点是否为 mount root，并通过 `vfs_mount_parent()` 切换到底层文件系统中 mount point 的父目录。`chroot` 边界通过 `vfs_resolve_rooted()` 在 normalization 前将 `task->fs.root_path` 与请求路径拼接，确保 chrooted 进程无法通过 `../../..` 逃出 root。

---

## 9. 网络栈

### 9.1 lwIP NO_SYS=1 集成

A20OS 在 `kernel/net/` 层以 `NO_SYS=1` 模式集成 lwIP。该模式禁用 lwIP 自带线程与信号量，所有 lwIP 核心调用在 `g_lwip_lock` 保护下由内核主动推进。

### 9.2 Progress Bottom-Half

`kernel/core/progress.c` 提供统一的 bottom-half 进展机制。典型调用路径为：

```text
网卡中断 -> a20_lwip_poll() -> progress_run() -> socket bottom-half -> 唤醒等待任务
```

调度器在切任务前运行 bottom-half，使 RX/TX 处理由中断 + 事件驱动，降低上下文切换开销。

### 9.3 virtio-net 驱动

virtio-net 驱动位于 `kernel/drivers/net/virtio_net.c`，每实例持有 `net->lock`。遵循锁顺序 `g_lwip_lock -> net->lock`：非阻塞 send 路径在 `g_lwip_lock` 下调用 `virtio_net_send(nonblock=1)`，而阻塞 send/recv 不得在 `g_lwip_lock` 下调用。

### 9.4 运行时网络配置

网络配置完全来自内核命令行，无编译期默认值。支持的键包括 `a20.ip`、`a20.netmask`、`a20.gateway`、`a20.dns`、`a20.dhcp`、`a20.hostname`。运行时配置通过 `/proc/net/config` 只读暴露，缺失配置时 socket 返回 `-ENETUNREACH`。

DHCP 作为 lwIP timeout 处理运行，在更新 netif 地址与 DNS 状态时持有 `g_lwip_lock`。用户态读取 `/proc/net/config` 时不持有 spinlock，允许看到稍旧的值，但不至于观察到中间状态。

---

## 10. 驱动模型

### 10.1 混合驱动架构

A20OS 驱动模型采用三层设计：

* **零开销 MMIO**：板级地址通过宏常量内联，`readl`/`writel` 编译为单条 load/store。
* **统一 hwapi**：`kernel/include/drivers/hwapi.h` 抽象 `request_irq`、`dma_alloc`、`clock_get_cycles` 等跨架构接口。
* **类 ops vtable**：`block_dev_ops_t`、`net_dev_ops_t`、`char_dev_ops_t` 提供单次间接调用。

### 10.2 注册机制

内置驱动通过 `DRIVER_REGISTER` 宏放入 `.driver_init` 链接器段，板级配置通过 `BOARD_REGISTER` 放入 `.board_init` 段。启动流程为：

```text
arch_early_init() -> board->early_init() -> driver_core_init()
  -> board->enumerate_devices() -> driver_probe_all() -> subsystem_init()
```

### 10.3 锁契约

各驱动的私有锁与局部顺序在 `docs/driver-lock-order.md` 中详细记录。例如：

| 驱动 | 私有锁 | 特殊局部顺序 |
|------|--------|--------------|
| virtio-blk | `inst->lock` | 无 |
| virtio-net | `net->lock` | `g_lwip_lock -> net->lock` |
| UART | `rx_lock` | `rx_lock -> proc_lock`（仅 Ctrl-C 路径） |
| PTY | `g_pty_alloc_lock`、per-pair `lock` | 无 |
| loop | `g_loop[i].lock` | 无 |

---

## 11. 测试与质量保证

A20OS 建立了一套可重复执行的测试门禁体系，覆盖并发、内存、VFS、ABI、驱动与构建矩阵。

### 11.1 文档与架构门禁

| 领域 | 门禁命令 |
|------|----------|
| 并发基础 | `make check-concurrency-foundation` |
| MM / VMA / 页表 | `make check-mm-lock-model` |
| I/O 进展 | `make check-io-progress-model` |
| VFS 抽象 | `make check-vfs-abstraction` |
| ABI 边界 | `make check-abi-boundary` |
| 驱动核心 | `make check-driver-core-model` |
| 外部依赖边界 | `make check-external-dependency-boundary` |

### 11.2 运行时压力测试

| 测试目标 | 命令示例 |
|----------|----------|
| 多架构内核构建 | `make check-kernel-build` |
| 用户态构建 | `make check-user-build` |
| Linux ABI smoke | `make smoke-riscv64`、`make smoke-abi-linux` |
| 调度并发压力 | `make smoke-sched-stress` |
| Futex 并发压力 | `make smoke-futex-stress` |
| VFS 并发压力 | `make smoke-vfs-stress` |
| MM 压力与 COW | `make smoke-mm-stress` |
| Native handle 覆盖 | `make smoke-native-handle` |

### 11.3 测试日志

测试日志存放于 `test-results/` 目录，例如 `smoke-sched-stress.log`、`smoke-futex-stress.log`、`smoke-vfs-stress.log`，以及各架构构建日志 `build-default-aarch64.log`、`build-default-riscv64.log` 等。通过 `make check-build-matrix` 与 `make check-doc-test-gates` 可在本地复现。

文档门禁遵循 `DOCS_AS_FACT_CONTRACT`：所有 `docs/` 与 `kernel/abi/*/*.md` 中的架构描述必须对应当前实现，未来设计只能放在规划材料中。漂移关键词（如 `stub`、`partial`、`TODO`）只有在绑定到明确覆盖表或 TODO 条目时才允许出现。

---

## 12. 核心挑战与解决方案

### 12.1 多核 SMP 下的调度状态机

**挑战**：任务状态在 fork、exec、exit、signal、futex 唤醒等路径中流转，乱序获取局部锁易导致死锁或调度器崩溃。

**解决**：确立严格局部锁层级 `proc_lock -> runq_lock`，并通过 `CONFIG_DEBUG_LOCKS` 记录 owner 与 spinner 信息。`make check-concurrency-foundation` 在极端压力下验证状态机健壮性。

### 12.2 大页降级与 COW 竞争

**挑战**：fork 或 OOM 回收遇到已映射大页时，传统单页回收机制会导致 TLB 冲刷不及时与引用计数竞争。

**解决**：在 `kernel/mm/vm.c` 中引入 `mm_demote_huge_page()` 与 COW 逻辑，通过解耦 `mm->lock` 安全处理并发缺页异常。

### 12.3 MAP_SHARED 多核缓存一致性

**挑战**：多进程并发 read/write 或 truncate 同一 `MAP_SHARED` 文件时，Page Cache 易出现悬空指针或读取脏数据。

**解决**：引入 dirty-page/writeback 生命周期所有权机制，在页面逐出路径中增强一致性栅栏，确保 VFS 层数据强一致。

### 12.4 Native IPC 权限逃逸防范

**挑战**：Handle 跨进程传递时，若因队列满或中断导致不完整交付，可能发生权限逃逸。

**解决**：Channel 采用两阶段锁分离与原子化 handle 转移语义。若接收方 handle table 空间不足，整个 `recv` 返回 `NO_SPACE`，消息留在队列中，杜绝 partial delivery。

---

## 13. 第三方代码与协议声明

本项目中部分模块与工具为了兼容性或工程需求，使用了非本队编写的开源代码，均遵循原开源协议，并在目录结构上与本队原创代码严格隔离。

### 13.1 lwIP 网络协议栈

* **路径**：`kernel/external/lwip/`
* **来源**：Swedish Institute of Computer Science 等
* **协议**：BSD License
* **说明**：内核借用其核心网络功能，以 `NO_SYS=1` 模式集成，套接字封装层由本队实现。

### 13.2 用户态第三方工具

* **路径**：`user/external/`
* **组件**：`musl`、`musl-cross-make`、`binutils`、`git`、`vim`、`fastfetch`、`zlib`、`sbase`、`tlse`、`mksh-cvs2git`
* **协议**：各组件自有协议（MIT、GPL、BSD 等）
* **说明**：这些仅作为用户态测试、演示或标准库环境，不属于内核核心原创代码。

本项目主体代码使用 Apache 2.0 协议开源。

---

## 14. 未来工作路线图

### 14.1 短期目标（P1 剩余工作）

* **I/O 完全事件驱动**：替换块/网络设备中的兼容轮询路径。
* **网络栈深化**：扩展 socket 并发测试，降低 `g_lwip_lock` 竞争。
* **VFS 语义收紧**：完整实现 `renameat2` 全标志、`statx`、xattr namespace 校验、mount `..` crossing、`chroot` 约束。
* **测试强化**：将静态文档门禁转化为更多可执行行为测试，例如 `smoke-vfs-edge`。

### 14.2 中期目标

* **Native ABI 用户态生态**：继续完善 `liba20rt` 与 `liba20c`，推动更多 Native 示例程序，修复 `liba20c` 中裸参数数组调用 syscall 的技术债，统一使用版本化 ABI 结构体。
* **musl-a20 持续维护**：保持 musl 移植与四架构同步，确保 git、vim、gcc、fastfetch 等关键应用在新内核变更后仍能静态构建与运行。
* **更多 LTP 风格压力测试**：覆盖 futex、调度、VFS、内存子系统，将静态文档门禁转化为可执行行为测试。
* **块设备完全事件驱动**：将 virtio-blk 与 loop 中的兼容轮询路径替换为中断 + progress bottom-half，减少空转。

### 14.3 长期探索

* **Rust 重写实验**：在 `riir` 分支继续研究性 Rust 重写，验证类型系统对锁契约与资源生命周期的静态保证。
* **fork 模拟与异步信号**：按需实现 `A20_SPAWN_FORK_SELF` 与事件驱动信号模拟，使需要 fork 的复杂 POSIX 程序可在 Native ABI 上运行。
* **动态链接器**：为 Native ABI 设计独立动态链接与加载机制，支持 `dlopen` 与共享库。
* **形式化验证**：继续推进 SOS 操作语义证明，覆盖 handle 生命周期、VMO/VMAR 映射、Channel transfer 等关键不变式。

A20OS 的核心追求始终如一：在保证 Linux 生态兼容的前提下，构建更安全、更高性能、更可维护的下一代内核底座。
