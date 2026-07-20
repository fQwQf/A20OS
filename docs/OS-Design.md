# A20OS 设计速查

A20OS 是为 2026 年全国大学生计算机系统能力大赛（操作系统内核实现赛道）开发的混合内核操作系统。它支持 RISC-V、ARM64、x86_64 和 LoongArch 四种架构，并同时提供 Linux 兼容 musl 程序运行环境和一套新的 Native ABI。

本文档面向新贡献者，用于快速理解项目整体结构。完整的 Native ABI 规范见 [docs/native-abi/00-overview.md](native-abi/00-overview.md)。

---

## A20OS 是什么

A20OS 是一个内核、两套用户接口：

* **Linux ABI**（`kernel/abi/linux/`）：223 个系统调用，可直接运行 git、vim、fastfetch、mksh 等静态链接 musl 程序，无需重新编译。
* **Native ABI**（`kernel/abi/native/`）：90 个系统调用，基于 handle、capability 和显式内存对象，是面向 A20OS 新程序的现代接口。

内核总代码约 12.5 万行，分布在 533 个源文件中。核心代码位于 `kernel/`；第三方代码（musl、lwIP、git、vim 等）隔离在 `user/external/` 和 `kernel/external/`。

---

## 混合内核的设计思路

A20OS 在运行空间上更像宏内核：

* 驱动、网络栈、文件系统、内存管理都运行在同一个特权地址空间内。
* 子系统之间通过普通函数调用协作，中断路径和热路径保持低开销。

同时，它在逻辑抽象上吸收微内核思想：

* 所有用户资源通过带 **rights 位** 的 **handle** 引用。
* 进程间通信使用 **Channel** 和 **EventQ**，而不是信号和管道。
* 内存拆分为 **VMO**（物理后备对象）和 **VMAR**（虚拟映射），让共享与权限检查变得显式。

为什么这样组合？内核内部路径因为函数调用而保持快速，而用户可见资源仍然受 capability 检查约束。这样兼顾了宏内核的性能和微内核的对象纪律。

---

## 两套 ABI 的实际用法

| ABI | 系统调用数 | 路径 | 使用场景 |
|-----|-----------|------|---------|
| Linux ABI | 223 | `kernel/abi/linux/` | 运行现有 musl 程序，无需改动。 |
| Native ABI | 90 | `kernel/abi/native/` | 编写面向 A20OS 的新程序，使用 handle/capability 接口。 |

两层 ABI 严格隔离。`kernel/abi/linux/` 和 `kernel/abi/native/` 都把用户调用翻译成同一组内核内部 API；核心模块不依赖任何 ABI 的用户结构体。

### 具体示例

**打开文件**

* Linux ABI：`openat(dirfd, "foo.txt", O_RDONLY)` 返回整数 fd。
* Native ABI：`path_open(parent_dir_handle, "foo.txt", A20_OPEN_READ, ...)` 返回带 READ 权限的 `a20_handle_t`。

**创建进程**

* Linux ABI：先 `fork()`，再 `execve("/bin/sh", argv, envp)`。
* Native ABI：一次 `task_spawn(&args)`，参数结构体中指定可执行文件、能力和初始 handle。

**等待 I/O**

* Linux ABI：`epoll_create` + `epoll_ctl` + `epoll_wait`。
* Native ABI：`event_queue_create` + `event_watch` + `event_wait`，统一在一个事件队列 handle 上等待。

**内存映射**

* Linux ABI：`mmap(addr, len, prot, flags, fd, off)`。
* Native ABI：先用 `vm_create_object` 创建 VMO，再用 `vm_map` 把它挂到指定 VMAR，并附带 rights 集合。

Native ABI 的完整规范见 [docs/native-abi/00-overview.md](native-abi/00-overview.md)。

---

## 支持的平台

A20OS 面向四种架构构建：

* **RISC-V 64**：QEMU `qemu-virt-riscv64` 和 StarFive VisionFive 2 开发板
* **ARM64**：QEMU `qemu-virt-aarch64`
* **x86_64**：QEMU `qemu-virt-x86_64`
* **LoongArch 64**：QEMU `qemu-virt-loongarch64` 和龙芯 LS2K1000 开发板

NOMMU 仅支持部分架构：`arm32`、`aarch64`、`riscv32`、`riscv64`。顶层构建会拒绝尚未实现 NOMMU 的组合，如 LoongArch64 和 x86_64。

典型构建命令：

```bash
make ARCH=riscv64 BOARD=qemu-virt-riscv64 run
make ARCH=aarch64 BOARD=qemu-virt-aarch64 run
make ARCH=x86_64 BOARD=qemu-virt-x86_64 run
make ARCH=loongarch64 BOARD=qemu-virt-loongarch64 run
```

`make check-kernel-build` 验证四种架构是否都能构建通过。

---

## 主要子系统

### 内存管理（`kernel/mm/`）

内存管理围绕两个核心抽象：

* **VMO**（`kernel/mm/a20_vmo.c`）：物理页容器，可 resize、共享、按页 fault。
* **VMAR**（`kernel/mm/a20_vmar.c`）：进程地址空间中的连续区域，维护自身的映射与保护规则。

`mm_struct` 用一把 per-process 自旋锁保护 VMA。Fork 使用写时复制（COW）：`mm_fork_clone_present_level()` 建立只读 COW 映射，写操作触发缺页后内核分配新页并复制内容。`mm_demote_huge_page()` 在 fork 或 OOM 需要回收已映射的 2 MiB 大页时，将其拆分为 4 KiB 页，同时持有 `mm->lock` 避免并发缺页竞态。

`MAP_SHARED` 一致性由 `kernel/fs/page_cache.c` 的页缓存统一处理，包含 dirty-page/writeback 生命周期，并在页面逐出路径中插入内存屏障。

### 进程调度与 SMP（`kernel/proc/`）

调度器使用 per-CPU 运行队列，没有全局调度锁。每个运行队列维护 8 级优先级和一个 bitmap，实现 O(1) 选任务。支持老化提升、SCHED_FIFO/RR，并通过 `kernel/proc/cg_cpu.c` 的 cgroup CPU quota 实现限流调度。

锁遵循严格的部分顺序，记录在 `kernel/include/core/lock.h`：

```text
cg_node.lock -> proc_lock -> runq_lock -> pfa.lock
proc_lock -> files_struct.lock -> VFS global-file/vnode locks
proc_lock -> mm_struct.lock
proc_lock -> a20_handle_table.lock
driver registry/IRQ locks -> device-private locks
g_lwip_lock -> g_net_lock
```

核心规则：持有自旋锁时禁止阻塞；持有 `runq_lock` 时禁止获取 `proc_lock`；持有设备或 lwIP 锁时，除非被调用方明确声明非阻塞，否则禁止调用 VFS、内存分配或调度路径。

Linux ABI 的 Futex 实现在 `kernel/abi/linux/sys_futex.c`，支持 wait、wake、requeue 和私有/共享键。Native 程序使用 `event_wait` 替代。

### 文件系统与 VFS（`kernel/fs/`）

`kernel/fs/vfs.c` 是顶层分发器，定义 `vnode` 和 `vfile` 接口，将调用路由到具体文件系统。

关键文件：

* `kernel/fs/vfs/path_resolution.c`：路径解析、挂载根、`..` 穿越、符号链接深度限制。
* `kernel/fs/vfs/mount_ops.c`：挂载与卸载。
* `kernel/fs/vfs/dcache.c`：目录项缓存。
* `kernel/fs/vfs/stat_perm.c`：权限检查与 sticky-bit。

支持的后端及当前限制：

| 后端 | rename | link | symlink | 关键限制 |
|------|--------|------|---------|---------|
| FAT32 | 不支持 | 不支持 | 不支持 | 元数据仅存 RAM |
| ext4 | 支持 | 不支持 | 快链 <= 60 B | 无日志；`st_nlink` 硬编码为 1 |
| ramfs | 支持 | 支持 | 支持 | 单目录 entry 上限 256；总 inode 上限 4096 |
| pipe | 不适用 | 不适用 | 不适用 | `PIPE_BUF` 原子写 |
| devfs | 不支持 | 不支持 | 不支持 | 节点编译期固定；只读设备树 |
| procfs | 不支持 | 不支持 | 不支持 | 合成文件；open 时生成快照 |
| sysfs | 不支持 | 不支持 | 不支持 | 当前仅暴露 `/sys/block/loopN` |

Linux ABI 兼容层实现了高复杂度的边界语义，包括 `openat2` 解析标志、`renameat2` 的 `RENAME_NOREPLACE`/`RENAME_EXCHANGE`、`statx` mask，以及 `faccessat2`/`fchmodat2` 的 flag 校验。

### 网络栈（`kernel/net/`）

网络栈以 `NO_SYS=1` 模式集成 lwIP：lwIP 自带的线程和信号量被禁用，所有 lwIP 核心调用在 `g_lwip_lock` 保护下由内核主动推进。

典型数据流：

```text
网卡中断 -> a20_lwip_poll() -> progress_run() -> socket bottom-half -> 唤醒等待任务
```

调度器在切换任务前运行 bottom-half，因此 RX/TX 处理由中断和事件驱动，而非轮询。

virtio-net 驱动位于 `kernel/drivers/net/virtio_net.c`，每个实例持有 `net->lock`。锁顺序为 `g_lwip_lock -> net->lock`：非阻塞 send 在 `g_lwip_lock` 下调用，阻塞 send/recv 不在 `g_lwip_lock` 下调用。

网络配置完全来自内核命令行：`a20.ip`、`a20.netmask`、`a20.gateway`、`a20.dns`、`a20.dhcp`、`a20.hostname`。没有编译期默认值。缺失配置时 socket 返回 `-ENETUNREACH`。运行时配置通过 `/proc/net/config` 只读暴露。

### 驱动模型（`kernel/drivers/`）

驱动模型分为三层：

* **零开销 MMIO**：板级地址通过宏常量内联，`kernel/include/drivers/hwapi.h` 中的 `readl`/`writel` 编译为单条 load/store。
* **统一 hwapi**：抽象 `request_irq`、`dma_alloc`、`clock_get_cycles` 等跨架构接口。
* **类 ops vtable**：`block_dev_ops_t`、`net_dev_ops_t`、`char_dev_ops_t` 提供一次间接调用。

内置驱动通过 `DRIVER_REGISTER` 宏放入 `.driver_init` 链接器段；板级配置通过 `BOARD_REGISTER` 放入 `.board_init` 段。启动顺序为：

```text
arch_early_init() -> board->early_init() -> driver_core_init()
  -> board->enumerate_devices() -> driver_probe_all() -> subsystem_init()
```

当前驱动包括 virtio-blk、virtio-net、UART、PTY、loop。各驱动私有锁顺序记录在 [docs/drivers/lock-order.md](drivers/lock-order.md)。

### IPC（`kernel/ipc/`）

Native IPC 提供两个互补原语：

* **Channel**（`kernel/ipc/a20_channel.c`）：双向消息通道。单条消息最多 64 KiB 数据 + 8 个 handle，采用两阶段写入和类型化通道约束。
* **EventQ**（`kernel/ipc/a20_event.c`）：统一事件等待机制，替代 epoll/signalfd/timerfd，维护 watch list、ring buffer 和全局反向索引。

Channel 传递 handle 时，接收方权限为 `receiver_rights = sender_rights ∩ transfer_rights`。handle 采用共享语义而非移动语义：发送方在 `send` 后仍保留原 handle。

---

## 设计速查

**哪些代码运行在内核空间？**  
驱动、网络栈、文件系统、内存管理和调度器都在同一个特权地址空间内运行。

**用户空间能看到什么隔离？**  
每个用户资源都是带 rights 的 handle，内核在每次操作时校验 handle 及其权限。内存只能通过 VMO/VMAR 共享或映射。

**什么时候用 Linux ABI？**  
需要直接运行现有 musl 程序（git、vim、fastfetch、mksh）而不重新编译时。

**什么时候用 Native ABI？**  
编写面向 A20OS 的新程序，需要更小、基于 capability 的接口时。

**两套 ABI 各有多少系统调用？**  
Linux ABI：223 个；Native ABI：90 个。

**支持哪些架构？**  
RISC-V 64、ARM64、x86_64、LoongArch 64。物理板：VisionFive 2（RISC-V）和龙芯 LS2K1000（LoongArch）。

**SMP 并发如何保证安全？**  
通过文档化的局部锁顺序、per-CPU 运行队列、per-process `mm->lock` 和局部驱动锁。`make check-concurrency-foundation` 在高压下验证该模型。

**内存共享怎么工作？**  
先用 `vm_create_object` 创建 VMO，再用 `vm_map` 把它映射到一个或多个 VMAR。最终生效的保护位是请求保护、handle rights 和 VMAR 标志三者的交集。

**Native IPC 如何替代信号？**  
进程间通知通过 Channel 消息，等待通过 EventQ，子进程终止通过 `task_wait`。Linux 兼容层在这些原语之上模拟 POSIX 信号语义。

**网络如何配置？**  
完全通过内核命令行：`a20.ip`、`a20.netmask`、`a20.gateway`、`a20.dns`、`a20.dhcp`、`a20.hostname`。没有编译期默认值。

**如何为特定板子构建运行？**  
`make ARCH=<arch> BOARD=<board> run`。用 `make check-kernel-build` 构建全部四种 QEMU 架构。

**Native ABI 完整规范在哪里？**  
[docs/native-abi/00-overview.md](native-abi/00-overview.md)。

---

## 接下来看什么

* **Native ABI 完整规范**：[docs/native-abi/00-overview.md](native-abi/00-overview.md)
* **驱动锁顺序**：[docs/drivers/lock-order.md](drivers/lock-order.md)
* **构建与运行**：[README.md](../README.md)
* **当前问题与路线图**：[docs/roadmap/a20os-improvement-todo.md](roadmap/a20os-improvement-todo.md)
* **源码布局**：
  * `kernel/abi/`：两套 ABI
  * `kernel/arch/` 和 `kernel/platform/`：HAL 与板级初始化
  * `kernel/mm/`：VMO/VMAR、页缓存、COW、OOM
  * `kernel/proc/`：任务、调度器、信号、Futex、cgroup
  * `kernel/fs/`：VFS、FAT32、ext4、ramfs、伪文件系统
  * `kernel/net/`：Socket 层、lwIP 集成、DHCP
  * `kernel/drivers/`：virtio-blk、virtio-net、UART、PTY、loop
  * `kernel/ipc/`：Channel、EventQ
  * `kernel/core/`：锁、时间、panic、progress bottom-half
