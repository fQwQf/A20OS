# A20OS 操作系统设计方案

## 1. 设计思路与核心架构解析

A20OS 采用**混合内核 (Hybrid Kernel)** 架构理念构建，核心目标在于探索现代操作系统架构的演进边界。项目在追求极简代码与极致性能的同时，在兼容性、内存安全及并发模型上进行了深度创新。通过结合宏内核的执行效率与微内核的逻辑抽象，本系统的架构设计主要体现在以下五个核心维度：

### 1.1 双重 ABI 架构 (Dual-ABI Design)
系统在 `kernel/abi/` 下创新性地实现了 `linux` 与 `native` 两套严格隔离的 ABI 接口层：
- **Linux 兼容层 (`kernel/abi/linux`)**：提供高保真度的 POSIX/Linux 系统调用支持。完整实现了复杂边界语义（如 `openat2` 扩展解析标志、`renameat2` 交换/非覆盖语义）、精确的内存序以及高级 Futex 机制。使得系统能够无缝运行 `musl`、`git`、`vim` 等复杂用户态生态。
- **Native 能力层 (`kernel/abi/native`)**：提供了一套面向未来的能力导向 (Capability-based) 原生 ABI。引入了对象句柄 (Handle)、细粒度权限校验 (rights)、通道 (Channel) 以及事件队列 (EventQ)，实现了更安全的细粒度 IPC 隔离与资源控制。

### 1.2 高阶内存与 IPC 模型 (VMO/VMAR & Channels)
在底层的原生架构上，A20OS 借鉴了现代微内核（如 Zircon）的先进理念，构建了功能强大的资源抽象体系，这完全独立于传统的 Linux 抽象：
- **基于对象的内存管理 (VMO/VMAR)**：在 `kernel/mm/` 下，系统实现了 `a20_vmo.c` (Virtual Memory Object) 和 `a20_vmar.c` (Virtual Memory Address Region)。这种将物理内存容器 (VMO) 与虚拟地址空间布局 (VMAR) 解耦的设计，不仅为 Native ABI 提供了灵活的共享内存基础，也为实现细粒度的高效零拷贝打下了底座。
- **先进的原生 IPC 通信**：在 `kernel/ipc/` 下，除了支持传统的 `sysv_sem/shm` 和 `eventfd` 外，原生引入了 `a20_channel.c` 和 `a20_event.c`。通过原生的 Channel 提供可靠的双向消息传递机制，并结合 Event 对象，构筑了高吞吐、强一致性的进程间通信底座。

### 1.3 面向高并发的锁契约与状态机
在 C 语言实现的核心底座中，系统采用了极其严格的锁契约与去全局锁化设计：
- **精细化锁层级**：如 `proc_lock -> runq_lock` 保证了进程状态（fork, exit, signal）与 Per-CPU 运行队列之间的死锁免疫。
- **多核 VMA 保护**：在 `kernel/mm/vm.c` 中，摒弃了传统的大内核锁，将内存映射路径收紧为仅持有特定 `mm->lock`，保障了并行 `mmap`/`munmap` 和缺页异常 (Page Fault) 处理的高吞吐率。

### 1.4 模块化虚拟文件系统 (VFS)
`kernel/fs/vfs.c` 作为超级分发器，抽象了完备的 `vnode` 和 `vfile` 接口：
- **多元文件系统后端**：不仅支持传统的 FAT32 和 EXT4 块设备，还高度集成了 `ramfs`、`devfs`、`procfs`、`sysfs` 和 `cgroupfs` 等伪文件系统。
- **缓存与一致性**：引入了统一的 `page_cache` 与 `block_cache`，并深度整合了大页降级 (demotion)、COW (Copy-On-Write) 机制，甚至为 `MAP_SHARED` 提供了完整的 dirty-page/writeback 生命周期管理，确保了数据一致性。

### 1.5 事件驱动网络栈与混合驱动模型
- **网络集成与异步进展**：在 `kernel/net` 层通过 `NO_SYS=1` 模式深度集成 `lwIP` 栈，摒弃了传统 lwIP 的多线程轮询，转而使用 `a20_lwip_poll` 和内核 `progress` 模块，将网卡 (virtio-net) 的 RX/TX 切换为中断+事件驱动，大幅降低了上下文切换开销。
- **混合驱动抽象 (`kernel/drivers`)**：采用动态设备注册模型，通过规范的 `hwapi.h` 抽象了平台相关性；针对板级设备的寄存器访问，利用 `readl`/`writel` 内联优化实现了零开销 MMIO 操作，兼顾了驱动的跨架构移植能力与执行效率。

### 1.6 探索下一代内核 API (Next-Gen Native ABI)
除了对现有生态的兼容，A20OS 也在**研究下一代内核操作系统 API**：
- **彻底的 Handle 化**：摒弃 Linux 繁杂的 fd、pid、tid 等标识符，一切底层资源全部统一为基于能力 (Capability) 的对象句柄。
- **面向未来的系统调用**：将 syscall 数量极简化，原生支持异步事件驱动、精准的权限降级控制以及稳定的版本协商策略。
- **配套生态**：目前内核侧已完成 90 个原生 syscall 的入口实现，并正在构建针对该生态的专属最小运行时库 `liba20rt` 和 `liba20c`。

## 2. 核心技术挑战与解决方案

在内核的演进过程中，架构的复杂性带来了诸多深度的工程挑战。以下为几个核心痛点及解决思路：

### 2.1 多核 SMP 下的调度状态机与局部锁死锁
- **挑战**：引入多核 (NR_CPUS > 1) 调度时，任务状态的流转（如 fork、exec、exit、signal、futex 唤醒）极易因乱序获取局部锁导致死锁或调度器崩溃。
- **解决**：摒弃全局大锁，确立了极度严格的局部锁层级契约（`proc_lock -> runq_lock`）。通过内置并发验证门禁（如 `make check-concurrency-foundation`）在极端压力测试下保障状态机的健壮性。

### 2.2 大页降级 (Huge-Page Demotion) 与 COW 竞争
- **挑战**：在执行 fork 创建子进程或 OOM 页面回收时，若遇到已被映射的大页，传统单页回收机制会导致 TLB 冲刷不及时与引用计数竞争。
- **解决**：在底层 `vm.c` 完整引入针对大页的降级处理与 COW (Copy-On-Write) 逻辑。通过解耦 `mm->lock`，确保降级拆分映射时能安全处理并发的缺页异常。

### 2.3 共享映射 (MAP_SHARED) 的多核缓存一致性
- **挑战**：多进程通过 `MAP_SHARED` 映射同一文件并发生并发 read/write 或 truncate 时，Page Cache 容易出现悬空指针 (use-after-free) 或读取脏数据。
- **解决**：引入严格的 dirty-page/writeback 生命周期所有权机制，在页面逐出 (eviction) 路径中增强一致性栅栏，确保 VFS 层在多核压力下的数据强一致。

### 2.4 Native IPC 传递下的权限逃逸防范
- **挑战**：引入 Native ABI 后，在进行 Handle 跨进程传递时，若因队列满或中断导致不完整交付 (partial-delivery)，极易发生权限逃逸。
- **解决**：设计原子化的 Handle 转移语义，在 IPC 层加入能力标签校验 (Label consistency) 与时间窗验证，彻底切断非法的能力逃逸链路。

## 3. 著作权非本队源代码和文档出处说明

本项目中部分模块和第三方工具为了兼容性或工程需求，使用了非本队编写的开源代码，均遵循原开源协议：

1. **lwIP 网络协议栈** 
   - 路径：`kernel/external/lwip/`
   - 来源：Swedish Institute of Computer Science 等
   - 协议：BSD License
   - 说明：内核借用其核心网络功能（NO_SYS=1），套接字封装层由本队实现。

2. **用户态第三方工具 (Userland Tools)**
   - 路径：`user/external/`
   - 组件：`musl`, `musl-cross-make`, `binutils`, `git`, `vim`, `fastfetch`, `zlib`, `sbase`, `tlse`, `mksh-cvs2git`
   - 协议：各组件自有协议（MIT, GPL, BSD 等）。
   - 说明：这些仅作为用户态测试、演示或标准库环境，均不属于内核核心原创代码。

## 4. 功能与性能测试记录

在自动测试平台及本地 QEMU 环境中，本队系统均完整通过了功能与性能测试，具备有效成绩。
相关的测试日志存放在 `test-results/` 目录下，包含但不限于以下记录：
- `smoke-sched-stress.log`
- `smoke-futex-stress.log`
- `smoke-vfs-stress.log`
- 架构构建日志：`build-default-aarch64.log`, `build-default-riscv64.log` 等。

通过命令 `make check-build-matrix` 与 `make check-doc-test-gates` 可以本地复现通过记录。
