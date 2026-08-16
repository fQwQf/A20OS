# A20OS 设计速查

A20OS 是为 2026 年全国大学生计算机系统能力大赛（操作系统内核实现赛道）开发的混合内核操作系统。当前 hosted 构建矩阵包含 `riscv64`、`loongarch64`、`aarch64`、`x86_64`、`arm32`、`riscv32` 和 `ppc64le`，另有独立的 `armv7m` MCU profile；内核同时提供 Linux 兼容 musl 程序运行环境和 Native ABI。

本文档面向新贡献者，用于快速理解项目整体结构。完整的 Native ABI 规范见 [docs/native-abi/00-overview.md](native-abi/00-overview.md)。

---

## A20OS 是什么

A20OS 是一个内核、两套用户接口：

* **Linux ABI**（`kernel/abi/linux/`）：362 个系统调用（`syscall_table.def` 登记），可直接运行 git、vim、fastfetch、mksh 等静态链接 musl 程序，无需重新编译。
* **Native ABI**（`kernel/abi/native/`）：136 个系统调用（`syscall_table.def` 登记），基于 handle、capability 和显式内存对象，是面向 A20OS 新程序的现代接口。

内核代码位于 `kernel/`，约 18 万行（含头文件），分布在 800 余个源文件中；第三方代码（musl、lwIP、git、vim 等）隔离在 `user/external/` 和 `kernel/external/`。

---

## 混合内核的设计思路

A20OS 在运行空间上更像宏内核：

* 调度、内存管理、网络栈、文件系统和关键设备路径运行在同一个特权地址空间内；驱动可静态链接、以 `.a20drv` 内核模块部署，也可通过 Native 用户态驱动服务接入。
* 子系统之间通过普通函数调用协作，中断路径和热路径保持低开销。

同时，它在逻辑抽象上吸收微内核思想：

* Native ABI 资源通过带 **rights 位** 的 **handle** 引用。
* Native ABI 进程间通信使用 **Channel**，并用 **EventQ** 等待已接入的对象事件。
* Native ABI 内存接口使用 **VMO**（物理后备对象）和 **VMAR**（虚拟映射），让共享与权限检查变得显式。

为什么这样组合？内核内部路径因为函数调用而保持快速，而 Native 用户可见对象仍受 capability 检查约束。这样兼顾了宏内核的性能和微内核的对象纪律。

混合内核的设计形态与演进目标（用户态服务化、驱动外迁、`channel_call` RPC、svcmgr 恢复）见 [hybrid-kernel/00-design.md](hybrid-kernel/00-design.md)，机制语义和当前边界见 [hybrid-kernel/01-mechanisms.md](hybrid-kernel/01-mechanisms.md) 与 [hybrid-kernel/STATUS.md](hybrid-kernel/STATUS.md)。设计文档中的目标能力只有在源码和运行门禁均已接通后才属于当前事实。

---

## 两套 ABI 的实际用法

| ABI | 系统调用数 | 路径 | 使用场景 |
|-----|-----------|------|---------|
| Linux ABI | 362 | `kernel/abi/linux/` | 运行现有 musl 程序，无需改动。 |
| Native ABI | 136 | `kernel/abi/native/` | 编写面向 A20OS 的新程序，使用 handle/capability 接口。 |

两层 ABI 的用户线格式分开维护，`kernel/abi/linux/` 和 `kernel/abi/native/` 尽量把调用翻译成共同的内核内部 API。当前仍有明确例外：`kernel/drivers/core/driver_manager.c` 直接包含 Native ABI 的类型和 rights 头来安装服务启动句柄，详见 [混合内核状态](hybrid-kernel/STATUS.md)。

### 分层原则（内部实现 ↔ ABI 薄包装）

内核内部实现的目标是**独立且自包含**：自持类型、常量和 API，供 ABI 层调用；ABI 层负责把用户态 syscall 线格式翻译成内部调用。除上文记录的 driver manager 债务外，依赖方向应保持单向：

```
用户态 ── syscall 线格式 ──> ABI 层（薄包装）── 内部 API ──> 内部实现
```

具体落地：

- 内部 IPC 子系统（对象模型、Channel、EventQ、句柄表、启动信息）的头文件 位于 `kernel/include/ipc/`（`ipc.h`、`handle_table.h`、`start_info.h`）， 不包含任何 `abi/` 内容；`kernel/include/abi/native/*` 里曾属于内部的部分 现在只是再导出（shim）。
- Linux ABI 的线格式常量（errno、fcntl、mman、poll、signal、stat、ioctl、 input）定义在 `kernel/include/core/*.h`，`kernel/include/abi/linux/*.h` 再导出——内部代码只 include `core/`。
- 例外：syscall 分派（`kernel/syscall/syscall.c`）与 arch 胶水 （`kernel/arch/*/abi/`、`syscall_hook.h`）本身是 ABI 边界的一部分， 有权感知 ABI。

这一原则的收益：内部实现（尤其 IPC/MM/调度）保持 ABI 无关，任何 ABI（包括 Linux ABI）都能直接包装内部机制而受益，无需复制实现。

### 具体示例

**打开文件**

* Linux ABI：`openat(dirfd, "foo.txt", O_RDONLY)` 返回整数 fd。
* Native ABI：`path_open(parent_dir_handle, "foo.txt", A20_OPEN_READ, ...)` 返回带 READ 权限的 `a20_handle_t`。

**创建进程**

* Linux ABI：先 `fork()`，再 `execve("/bin/sh", argv, envp)`。
* Native ABI：一次 `task_spawn(&args)`，参数结构体中指定可执行文件、能力和初始 handle。

**等待 I/O**

* Linux ABI：`epoll_create` + `epoll_ctl` + `epoll_wait`。
* Native ABI：`event_queue_create` + `event_watch` + `event_wait` 可统一等待已接入的 Channel、task、timer 和用户态驱动 IRQ 事件；file/socket/pipe readiness 与 signal 尚未接入。

**内存映射**

* Linux ABI：`mmap(addr, len, prot, flags, fd, off)`。
* Native ABI：先用 `vm_create_object` 创建 VMO，再用 `vm_map` 把它挂到指定 VMAR，并附带 rights 集合。

Native ABI 的完整规范见 [docs/native-abi/00-overview.md](native-abi/00-overview.md)。

---

## 构建目标与已验证平台

A20OS 的七个 hosted 架构都进入内核和用户态构建矩阵；ARMv7-M 使用独立 MCU profile：

* **RISC-V 64**：QEMU `qemu-virt-riscv64` 和 StarFive VisionFive 2 开发板
* **ARM64**：QEMU `qemu-virt-aarch64`
* **x86_64**：QEMU `qemu-virt-x86_64`
* **LoongArch 64**：QEMU `qemu-virt-loongarch64` 和龙芯 LS2K1000 开发板
* **PPC64LE**：QEMU `qemu-virt-ppc64le`（pSeries 固件）
* **ARM32**：QEMU `qemu-virt-arm32`
* **RISC-V 32**：QEMU `qemu-virt-riscv32`
* **ARMv7-M**：STM32F103 MCU profile（Cortex-M3、NOMMU，不属于 hosted 用户态矩阵）

构建支持与运行验证是不同层级。`Makefile` 的已验证 SMP 白名单只包含 `riscv64`、`aarch64`、`loongarch64`、`x86_64` 及其同名 `qemu-virt-*` 板；其他组合的 `NR_CPUS>1` 会被拒绝，除非明确设置 `ALLOW_UNVERIFIED_SMP=1` 做 bring-up。PPC64LE 当前按 QEMU pSeries 单核边界记录。

NOMMU 构建支持集合为 `riscv64`、`riscv32`、`aarch64`、`arm32` 与 `armv7m`（`NOMMU_SUPPORTED_ARCHES`）。其中 hosted 运行矩阵 `smoke-arch-mmu-matrix` 只覆盖前四个 hosted 架构的 MMU/NOMMU 组合；ARMv7-M 由 STM32 目标单独覆盖。LoongArch64、x86_64 和 PPC64LE 的 NOMMU 构建在入口被拒绝。

典型构建命令：

```bash
make ARCH=riscv64 BOARD=qemu-virt-riscv64 run
make ARCH=aarch64 BOARD=qemu-virt-aarch64 run
make ARCH=x86_64 BOARD=qemu-virt-x86_64 run
make ARCH=loongarch64 BOARD=qemu-virt-loongarch64 run
make ARCH=arm32 BOARD=qemu-virt-arm32 run
make ARCH=riscv32 BOARD=qemu-virt-riscv32 run
make ARCH=ppc64le BOARD=qemu-virt-ppc64le run
```

在 Linux 主机上，`make check-kernel-build` 和 `make check-user-build` 的默认集合是上述七个 hosted 架构；macOS 默认集合较小。`make check-kernel-build-all`、`make check-user-build-all` 或 `make check-build-matrix-all` 使用显式七架构集合。PPC64LE 的独立入口是 `make check-ppc64le-bringup` 和 `make check-ppc64le-user`。

---

## 主要子系统

### 内存管理（`kernel/mm/`）

Native ABI 的内存对象接口围绕两个核心抽象：

* **VMO**（`kernel/mm/vmo.c`）：物理页容器，可 resize、共享、按页 fault。
* **VMAR**（`kernel/abi/native/vmar.c`）：进程地址空间中的连续区域，维护自身的映射与保护规则（`mm_mmap_vmo`/`mm_munmap`/`mm_mprotect` 的薄包装）。

`mm_struct` 用一把 per-process 自旋锁保护 VMA。Fork 使用写时复制（COW）：`mm_fork_clone_present_level()` 建立只读 COW 映射，写操作触发缺页后内核分配新页并复制内容。`mm_demote_huge_page()` 在 fork 或 OOM 需要回收已映射的 2 MiB 大页时，将其拆分为 4 KiB 页，同时持有 `mm->lock` 避免并发缺页竞态。

`MAP_SHARED` 一致性由 `kernel/fs/page_cache.c` 的页缓存统一处理，包含 dirty-page/writeback 生命周期，并在页面逐出路径中插入内存屏障。

### 进程调度与 SMP（`kernel/proc/`）

调度器使用 per-CPU 运行队列。级 0 承载实时任务（`SCHED_FIFO`/`SCHED_RR`，优先级 1..99）；普通任务使用 **EEVDF（最早资格虚拟截止时间优先）**：每个任务按权重累加虚拟运行时间（`vruntime += dt * NICE0 / weight`），runqueue 的系统虚拟时间 `vtime` 以排队中的 EEVDF 权重和推进，不包含当前运行任务。picker 在按 deadline 排序的队列中扫描并选择第一个 `vruntime <= vtime` 的任务；若没有 eligible 任务，则选择队首的最早 deadline 保证进展。该扫描最坏为 O(n)。nice/weight 控制 CPU 份额；affinity 同时受 online CPU 与 cgroup cpuset 限制，CPU quota 由 `kernel/proc/cg_cpu.c` 执行。

“任务状态”和“CPU 所有权”是两个不同维度。`PROC_READY` 任务可能仍在runqueue，也可能已经被本地 CPU 选中：

```text
on_rq -> dispatching -> on_cpu -> unowned
```

本地 picker 只持有本 CPU 的 runqueue 锁，原子完成`on_rq -> dispatching`；释放队列锁后，调度器才获取 `proc_lock` 发布context switch。本地队列为空时，picker 会非阻塞地尝试从其他 CPU 窃取EEVDF 任务（远端有富余、尊重 affinity），使空闲核吸收突发负载，避免8 核失衡。旧任务的 `on_cpu` 跨底层切换保持有效，直到新任务在自己的内核栈上完成 switch cleanup。迁移同时获取源、目标 runqueue 锁，固定按 CPU编号升序。

远程入队通过 per-CPU 持久 `need_resched` 请求抢占。IPI 只通知目标 CPU，不会在任意中断上下文直接切换；请求在 trap/syscall/timer 返回或显式调度安全点消费。

所有对象等待使用 tokenized Park/Wake。waiter 先生成 `(task, wait_seq)`token，在对象锁内重查条件并 link，释放对象锁后才 commit；waker 在对象锁内只把 task 引用和 token 转移到 wake queue，释放对象锁后再进入 scheduler。因此提前到达的事件、旧 timeout 和重复 wake 都不能唤醒后续等待。

带 deadline 的 Park 注册到持有 task 引用的最小堆；cancel 与 expiry 只有一方负责摘除和释放。信号状态由独立 `signal_state.lock` 保护；`INTERRUPTIBLE`、`KILLABLE`、`UNINTERRUPTIBLE` 对普通信号、致命信号和退出使用不同的唤醒规则。`PROC_STOPPED` 是独立 job-control 状态，不借用 Park。

锁遵循严格的部分顺序，记录在 `kernel/include/core/lock.h`：

```text
cg_node.lock -> proc_lock -> runq_lock -> pfa.lock
proc_lock -> park_lock
proc_lock -> signal_state.lock
park_lock -> signal_state.lock
park_lock -> g_wait_timer_lock
park_lock -> runq_lock
proc_lock -> files_struct.lock -> VFS global-file/vnode locks
proc_lock -> mm_struct.lock
proc_lock -> a20_handle_table.lock
driver registry/IRQ locks -> device-private locks
g_lwip_lock -> g_net_lock
```

核心规则：持有自旋锁时禁止阻塞；持有 `runq_lock` 时禁止获取`proc_lock`；对象/设备锁内只 collect waiter，实际 wake 在释放对象锁后flush；持有设备或 lwIP 锁时，除非被调用方明确声明非阻塞，否则禁止调用VFS、内存分配或调度路径。

Linux ABI 的 Futex 实现在 `kernel/abi/linux/sys_futex.c`，支持 wait、wake、requeue 和私有/共享键。Futex waiter 同样保存 task 引用和 `wait_seq`，wait入队前在 `mm->lock -> futex lock` 下做不缺页的用户值二次检查。Native 程序使用 `event_wait` 替代。

完整状态机、所有权表和验证入口见 [进程、调度与阻塞协议](process-scheduler.md)；公平/延迟选择策略、资格门控、空闲窃取和虚拟 slice 旋钮见 [EEVDF 调度器设计](eevdf-scheduler.md)。

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
| FAT32 | 支持 | 不支持 | 不支持 | 元数据仅存 RAM；rename 改写 `..` 项 |
| ext4 | 支持 | 支持 | 快链 <= 60 B | 不生成新的日志事务；可对受支持的现有 JBD2 journal 做 fail-closed recovery；64 位文件大小 + 部分截断回收 |
| NTFS | 支持 | 不支持 | 不支持 | 索引无 B-tree 分裂；不支持 `$ATTRIBUTE_LIST` |
| ISO9660 | 不支持 | 不支持 | 不支持 | 只读 CD-ROM；名字转小写；跨块目录记录 |
| ramfs | 支持 | 支持 | 支持 | 单目录 entry 上限 256；总 inode 上限 4096 |
| pipe | 不适用 | 不适用 | 不适用 | `PIPE_BUF` 原子写 |
| devfs | 不支持 | 不支持 | 不支持 | 合成只读命名空间；保留静态兼容节点，并为 char/block/audio class 动态发布设备节点 |
| procfs | 不支持 | 不支持 | 不支持 | 合成文件；open 时生成快照 |
| sysfs | 不支持 | 不支持 | 不支持 | 合成只读视图；暴露 loop、DRM，以及 `/sys/class/{char,block,net,input,display,audio}` 动态 class 设备 |

Linux ABI 兼容层实现了高复杂度的边界语义，包括 `openat2` 解析标志、`renameat2` 的 `RENAME_NOREPLACE`/`RENAME_EXCHANGE`、`statx` mask，以及 `faccessat2`/`fchmodat2` 的 flag 校验。

四条主线架构（riscv64、aarch64、loongarch64、x86_64）的 Linux syscall 编号覆盖均达 Linux 水平：riscv64/aarch64 覆盖 asm-generic 全表；loongarch64 补齐私有 `file_getattr(468)`/`file_setattr(469)`；x86_64 的映射表（`kernel/arch/x86_64/include/syscall_nr_x86_64.h`）扩展至 463 槽，包含 `io_uring`/`landlock`/`pidfd`/`mseal` 等现代 syscall。编号覆盖不等于语义完整——兼容层按 `partial` 保守记录（见 `kernel/abi/linux/syscall_coverage.md`），仅在支持的 flag/对象范围内主张 Linux 语义。

### 网络栈（`kernel/net/`）

网络栈以 `NO_SYS=1` 模式集成 lwIP：lwIP 自带的线程和信号量被禁用，所有 lwIP 核心调用在 `g_lwip_lock` 保护下由内核主动推进。

IRQ 可用时的典型数据流：

```text
网卡中断 -> a20_lwip_poll() -> progress_run() -> socket bottom-half -> 唤醒等待任务
```

`kernel/core/progress.c` 统一承接设备和 socket 进展：IRQ 路径发出 pending 提示，timer 提供 lwIP timeout 和丢失 IRQ 的安全网，scheduler/idle bridge 仍保留有界 polling fallback。因而当前模型是事件驱动优先、必要时轮询，而不是纯 IRQ 或纯轮询。

virtio-net 驱动位于 `kernel/drivers/net/virtio_net.c`，每个实例持有 `net->lock`。锁顺序为 `g_lwip_lock -> net->lock`：非阻塞 send 在 `g_lwip_lock` 下调用，阻塞 send/recv 不在 `g_lwip_lock` 下调用。

网络配置结构由 bootargs 键 `a20.ip`、`a20.netmask`、`a20.gateway`、`a20.dns`、`a20.dhcp`、`a20.hostname` 填充，并通过 `/proc/net/config` 只读暴露；DHCP 会更新生效值。没有通用 lwIP 编译期地址回退，但 `qemu-virt-aarch64` 与 `virtualbox-aarch64` 的 board 代码目前会合成 QEMU/VBox NAT 静态 bootargs，多个 smoke 目标也显式传入相同地址。没有地址或路由的接口保持未配置，需要路由的调用返回 `-ENETUNREACH`。

### 驱动模型（`kernel/drivers/`）

驱动模型分为三层：

* **零开销 MMIO**：板级地址通过宏常量内联，`kernel/drivers/core/driver_hwapi.h` 中的 `readl`/`writel` 编译为单条 load/store。
* **统一 hwapi**：抽象 `request_irq`、`dma_alloc`、`clock_get_cycles` 等跨架构接口。
* **类 ops vtable**：`block_dev_ops_t`、`net_dev_ops_t`、`char_dev_ops_t` 提供一次间接调用。

静态链接的驱动通过 `DRIVER_REGISTER` 放入 `.driver_init` 链接器段；每次构建选中的 board 则直接在 `kernel/platform/<board>/board.c` 定义唯一 `current_board`。普通 hosted 开发构建默认使用 `DRIVER_DEPLOYMENT=generic`：驱动核心、总线和聚合服务内置，可发现设备驱动由 `kernel/drvmod/examples/` 生成 `.a20drv` 并从 Early/Runtime DriverStore 加载。`DRIVER_DEPLOYMENT=embedded`（包括决赛 `make all`、ARMv7-M 和 PPC64LE 默认）把完整驱动集静态链接进内核。详见 [驱动部署 profile](drivers/guide/deployment-profiles.md)。简化启动顺序为：

```text
trap/MM/time init -> board->early_init() -> driver_core_init()
-> board->enumerate_devices() -> driver_probe_all() -> vfs_init()
-> generic Early DriverStore -> net/mount/proc -> generic Runtime DriverStore
```

各驱动私有锁顺序记录在 [驱动锁顺序](drivers/guide/lock-order.md)。

### IPC（`kernel/ipc/`）

Native IPC 提供两个互补原语：

* **Channel**（`kernel/ipc/a20_channel.c`）：双向消息通道。单条消息最多 64 KiB 数据 + 8 个 handle，采用两阶段写入和类型化通道约束。
* **EventQ**（`kernel/ipc/a20_event.c`）：Native ABI 事件等待机制，维护 watch list、ring buffer 和全局反向索引。当前实际生产事件的是 Channel、task 退出、Native timer 和用户态驱动 IRQ；file、socket 与 signalfd 尚未生产 EventQ 事件。

Channel 传递 handle 时，接收方权限为 `receiver_rights = sender_rights ∩ transfer_rights`。handle 采用共享语义而非移动语义：发送方在 `send` 后仍保留原 handle。

---

## 设计速查

**哪些代码运行在内核空间？**  网络栈、文件系统、内存管理、调度器和关键设备路径在同一个特权地址空间内运行；驱动还支持 `.a20drv` 内核模块和 Native 用户态服务两种部署形态。

**Native 用户空间能看到什么隔离？**  Native ABI 对象通过带 rights 的 handle 访问，内核在每次操作时校验 handle 及其权限；其显式内存对象通过 VMO/VMAR 共享或映射。Linux ABI 使用 fd、进程和 POSIX 兼容接口，不应描述为全部由 handle 暴露。

**什么时候用 Linux ABI？**  需要直接运行现有 musl 程序（git、vim、fastfetch、mksh）而不重新编译时。

**什么时候用 Native ABI？**  编写面向 A20OS 的新程序，需要更小、基于 capability 的接口时。

**两套 ABI 各有多少系统调用？**  Linux ABI：362 个；Native ABI：136 个（均为 `syscall_table.def` 当前登记数）。

**支持哪些构建目标？**  七个 hosted 架构：RISC-V64、LoongArch64、AArch64、x86_64、ARM32、RISC-V32、PPC64LE；另有 ARMv7-M STM32 MCU profile。物理板源码包括 VisionFive 2 和 LS2K1000。构建支持不自动等于 SMP 或完整运行验证。

**SMP 并发如何保证安全？**  通过文档化的锁顺序、per-CPU 运行队列、显式`on_rq/dispatching/on_cpu` 所有权、带序号 Park/Wake、异步 task 引用和持久抢占请求。`make check-concurrency-foundation` 检查基础契约；`make check-proc-step8-local` 执行双架构 1 核/8 核累计压力矩阵。

**Native ABI 内存共享怎么工作？**  先用 `vm_create_object` 创建 VMO，再用 `vm_map` 把它映射到一个或多个 VMAR。最终生效的保护位是请求保护、handle rights 和 VMAR 标志三者的交集。

**Native IPC 如何处理通知？**  进程间通知通过 Channel 消息，已接入事件通过 EventQ 等待，子进程终止通过 `task_wait`。Linux POSIX 信号由 `kernel/proc/signal.c` 及 Linux signal syscall 路径直接管理，不建立在 EventQ 上。

**网络如何配置？**  通过 `a20.*` bootargs 或 DHCP 填充运行时配置；部分 QEMU/VirtualBox board 当前会提供 NAT 静态 bootargs。`/proc/net/config` 是只读状态面，不是通用写配置接口。

**如何为特定板子构建运行？**  `make ARCH=<arch> BOARD=<board> run`。用 `make check-build-matrix-all` 显式构建七个 hosted 架构的内核和用户态；`make all` 只构建决赛 RISC-V64/LoongArch64 提交产物。

**Native ABI 完整规范在哪里？**  [docs/native-abi/00-overview.md](native-abi/00-overview.md)。

---

## 接下来看什么

* **Native ABI 完整规范**：[docs/native-abi/00-overview.md](native-abi/00-overview.md)
* **进程、调度与阻塞协议**：[docs/process-scheduler.md](process-scheduler.md)
* **驱动锁顺序**：[drivers/guide/lock-order.md](drivers/guide/lock-order.md)
* **构建与运行**：[README.md](../README.md)
* **当前问题与路线图**：[docs/roadmap/a20os-improvement-todo.md](roadmap/a20os-improvement-todo.md)
* **源码布局**：
  * `kernel/abi/`：两套 ABI
  * `kernel/arch/` 和 `kernel/platform/`：HAL 与板级初始化
  * `kernel/mm/`：VMO/VMAR、页缓存、COW、OOM
  * `kernel/proc/`：任务、调度器、信号、Futex、cgroup
  * `kernel/fs/`：VFS、FAT32、ext4、NTFS、ISO9660、ramfs、伪文件系统
  * `kernel/net/`：Socket 层、lwIP 集成、DHCP
  * `kernel/drivers/`：驱动核心、总线、聚合服务和 embedded 设备驱动源码
  * `kernel/drvmod/examples/`：generic profile 的 `.a20drv` 包装与可加载驱动
  * `kernel/ipc/`：Channel、EventQ
  * `kernel/core/`：锁、时间、panic、progress bottom-half
