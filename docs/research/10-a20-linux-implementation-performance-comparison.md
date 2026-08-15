# A20OS 与 Linux 实现及性能差异：源码审计、实测边界与优化路线

> 审计日期：2026-08-10（Asia/Shanghai） A20OS 审计源码：`e33c3219dcf5e7f9d1476eeedda99bfb0c619eb1` 最近一份可比的 A20OS 正式性能样本：`f973234811c815a4d6202ae694653f11638fe978` Linux parallel-build 基线：Linux 7.1.6，见本仓库保存的 baseline metadata 范围：本项目一手只读审计 A20OS 源码、设计文档和本地 `.kernel-build/smoke/2026` 归档；该目录被 Git 忽略，干净 clone 不含原始证据。Linux 侧仅采用 Linux 官方文档与 kernel.org 官方源码。

## 1. 先给结论

### 1.1 可以确认的结论

1. **A20OS 已经不是“只能跑 demo”的内核。** 它有 per-CPU EEVDF 运行队列、 多架构 SMP、COW 与文件映射、定向 TLB shootdown、VFS、page/block cache、可写 ext4 子集、lwIP 网络、Linux ABI 与 Native capability ABI、VirtIO/PCI/模块化驱动， 并已在 RISC-V64、LoongArch64 的 8 核真实 Cargo/rustc 并发负载上完成正式流程。 这些是源码和测试日志共同支持的事实，而不是设计目标 （[`OS-Design.md`](../OS-Design.md)、本地证据路径 `.kernel-build/smoke/2026/platform-final-20260810T001000Z-f9732348/REPORT.md`）。
2. **在当前本地归档可审计的 Linux 对照负载 parallel-build 上，A20OS 仍有显著性能差距。** `d5ae5a16` 与 Linux 使用相同 QEMU 10.2.2、配置和 workload，但 runner、启动镜像和控制镜像不同；其 `4.00×/4.10×` 是受控历史参考，不是同 runner 对照。 最近正式 A20OS 样本为 RISC-V64 `1483.00 s`、LoongArch64 `1399.13 s`；若与 Linux 中位数 `638.37/565.21 s` 横向参照，是约 `2.32×/2.48×`。但后者的 QEMU 分别为 10.0.2 和 10.2.2，只能表示**指示性差距**，不是严格同平台倍数。 这是“编译 + 大量小文件元数据
   + 并发进程/线程 + page cache + 块 I/O”的综合差距，不能直接外推到网络、交互、 MCU 或 Native IPC。
3. **跨版本指标显示差距随工程优化谱系缩小，但不能直接量化内部 speedup。** `d5ae5a16` 与 `f9732348` 的 workload/名义配置相同，QEMU 却分别为 10.2.2/10.0.2；期间完成了 idle wait、VirtIO polling、page-cache fill、ext4 bitmap、vnode/dcache 优化，跨 QEMU 指示值由 `4.00×/4.10×` 变为 `2.32×/2.48×`，不能把差值直接归因于源码。其中 `31135d66 -> 5190571f` 的单批正式降时为 RISC-V64 `-32.30%`、LoongArch64 `-28.17%`。这证明当前主要优化空间在**系统级热路径结构**，而不是编译器微调。
4. **当前最大结构性劣势是成熟度与可伸缩层次不足。** A20OS 多处使用固定容量、 单全局锁或 O(n) 结构：EEVDF 有序链表、VMA 链表、全局 buddy 锁、按 slab 类别 的共享锁、全局 page-cache 锁、全局 dcache 锁、ext4 filesystem-wide metadata lock、线性目录项、串行 block-cache writeback、轮询 VirtIO block completion、 lwIP 与 socket 全局锁。Linux 对应路径经过树/哈希、per-CPU/per-node、RCU、 blk-mq、NAPI、writeback domain 等多级拆分。
5. **A20OS 的明确优势主要是可理解、可审计、跨架构一致和定制能力，而不是已证实的 绝对吞吐优势。** Native ABI 的类型化 handle、rights attenuation、temporal capability、VMO/channel/EventQ，统一的内核/用户态驱动部署，以及对生命周期、 page cache、block cache 的专用计数器，都比“复刻所有 Linux 接口”更适合研究、 教学、受控 appliance 和小型可信系统。但尚无同条件实测证明这些路径快于 Linux。

### 1.2 本报告如何标注证据

- **源码事实**：可以从所引 A20OS/Linux 源码或官方设计文档直接读出。
- **实测事实**：来自本地 `.kernel-build/smoke/2026` 归档中带 commit、镜像哈希、QEMU 参数、退出状态 和 judge 结果的记录。
- **推断**：根据数据结构、锁范围和调用路径推导出的性能/可靠性影响；不冒充实测。
- **建议**：后续优化方向；是否有效必须由新的对照实验验证。

审计源码基线 `e33c3219` 晚于正式性能提交 `f9732348`，后续提交主要涉及根盘、 测试入口和 RISC-V 内存/额外磁盘启动修复。因没有该审计基线的等价性能样本，本文 把 `1483.00/1399.13 s` 严格绑定到 `f9732348`，不称为“审计基线时延”。

## 2. 实测性能：能说什么，不能说什么

### 2.1 可比条件

Linux、`d5ae5a16` 与 `f9732348` 均使用官方 parallel-build workload、8 GiB、8 vCPU、 `tcg,thread=multi` 和独立 overlay。Linux 与 `d5ae5a16` 同为 QEMU 10.2.2、同名义 配置/workload，但 runner、启动镜像和控制镜像不同，不是同 runner；`f9732348` 则由 平台 runner 使用 QEMU 10.0.2，并记录 3000 秒 watchdog、只读 raw base、主动关机和 judge 结果。因此 `d5/Linux` 是受控历史参考，`f973/Linux` 与 `d5/f973` 都只能作 跨版本指示。可直接归因的 A20 单批证据是同平台 `31135d66 -> 5190571f` 的 `-32.30%/-28.17%`。证据见：

- `.kernel-build/smoke/2026/EVALUATION.md`；
- `.kernel-build/smoke/2026/metadata/riscv64-linux-parallel-build-baseline-d84b24e94f887134c1e7f09f591dd7cc66cb8ce7-20260808T055633Z-434359316-2.txt`；
- `.kernel-build/smoke/2026/metadata/loongarch64-linux-parallel-build-baseline-d84b24e94f887134c1e7f09f591dd7cc66cb8ce7-20260808T063036Z-685704977-2.txt`；
- `.kernel-build/smoke/2026/platform-final-20260810T001000Z-f9732348/REPORT.md`。

### 2.2 总体结果

| 架构 | Linux 7.1.6 中位数 | A20OS `d5ae5a16` | 受控历史参考 | A20OS `f9732348` | 指示性 `f973` / Linux |
|---|---:|---:|---:|---:|---:|
| RISC-V64 | 638.37 s | 2551.12 s | 4.00× | 1483.00 s | 2.32×（+844.63 s） |
| LoongArch64 | 565.21 s | 2315.48 s | 4.10× | 1399.13 s | 2.48×（+833.92 s） |

`d5ae5a16`/Linux 同 QEMU、配置和 workload，但 runner/启动与控制镜像不同；`f973` 又跨 QEMU 版本。`d5ae5a16 -> f9732348` 的 1.72×/1.65× 只能作为演进指标，不能直接称为 A20OS 内部 speedup。 `f9732348` 两架构 parallel-build 技术项均为 `180/180`，functional tests 均为 `10/10`、 `199.10/200`，但这只说明评分区间已满，不说明性能追平 Linux。

### 2.3 已定位并修掉的历史瓶颈

| 历史瓶颈 | 证据 | 修复及反馈 | 当前解读 |
|---|---|---|---|
| 空闲 CPU 忙轮询 | 旧 A20 ext4 微负载 8 核有约 1692 万次 empty pick，约 99.98% | 后续加入安全 idle wait | 已修历史根因；不能继续称当前仍有同等 busy-loop |
| VirtIO completion 重复 polling | phase9 计数器显示 progress/active poll 远高于 completion | per-device progress、wait loop 去重、32..512 次退避；active poll RV -95.2%、LA -99.3% | 仍是 poll-only block 驱动，但重复工作显著减少 |
| block page-cache 冷 miss 全局串行 | 块页冷填充曾由单一 fill lock 串行 | 64 个 block-cache fill-lock shard | parallel-build 正式批次总体降时的重要组成；不能单独分摊收益 |
| ext4 bitmap 逐 bit 扫描 | LA 10977 个逻辑候选 | 满字节跳过后实际 byte load 1597 | 已消除明显常数开销 |
| ext4 vnode cache O(capacity) 且满 | 约 157 万 probe、cache-full 608/612 | 4096 槽、2048 bucket、free list；probe 约 3100，full 0 | 已消除该工作集的线性缓存热点 |
| dcache 容量不足 | 约 290 次 eviction | 2048 entry/512 bucket 后 0/1 次 | 对当前工作集有效，不等于通用可伸缩 dcache |

这些结果来自本地工作区归档 `.kernel-build/smoke/2026/platform-final-20260809T204748Z-5190571f/REPORT.md` 和 `.kernel-build/smoke/2026/platform-final-20260810T001000Z-f9732348/REPORT.md`。 同一批次同时改了多个热区，所以不能把整体降时全部归给某一个补丁。

### 2.4 仍不能下的结论

- 没有 A20OS/Linux 的网络吞吐、尾延迟、context switch、fork、futex、page fault、 fsync 延迟、功耗或 Native IPC 同条件基准，不能声称任一方在这些指标上的倍数。
- TCG 结果对真实硬件 cache、NUMA、IOMMU、IRQ 和存储队列不敏感；它适合对照， 不是裸机服务器结论。
- `f9732348` 同代码一次非最终编排样本为 `1425.50/1393.35 s`，正式报告明确指出 judge 采集编排问题；本文只采用完整正式样本。RISC-V 在第二批相对上一批为 `+3.06%`，落在 TCG 波动范围，不能声称该批对 RISC-V 稳定提速。

## 3. 总体架构与适用定位

### 3.1 内核组织

<<<<<<< HEAD
<<<<<<< HEAD
<<<<<<< HEAD
<<<<<<< HEAD
**源码事实（A20OS）。** A20OS 把调度、MM、VFS、cache、网络和关键驱动放在同一 特权地址空间，以直接 C 调用连接；同时提供 Linux ABI（258 个登记入口）和 Native ABI（126 个登记入口）。登记数只表示分发表面积，不表示 Linux 语义完整性。 Native ABI 使用 handle/rights、VMO/VMAR、 channel/EventQ；generic 驱动可作为 `.a20drv` 内核模块或用户服务部署 （[`OS-Design.md`](../OS-Design.md)、 [`syscall_table.def`](../../kernel/abi/linux/syscall_table.def)、 [`deployment-profiles.md`](../drivers/guide/deployment-profiles.md)）。
=======
=======
>>>>>>> 0779cfdf (Merge branch 'fqwqf/linux-abi-depth')
**源码事实（A20OS）。** A20OS 把调度、MM、VFS、cache、网络和关键驱动放在同一
特权地址空间，以直接 C 调用连接；同时提供 Linux ABI（343 个登记入口）和
Native ABI（126 个登记入口）。登记数只表示分发表面积，不表示 Linux 语义完整性。
Native ABI 使用 handle/rights、VMO/VMAR、
channel/EventQ；generic 驱动可作为 `.a20drv` 内核模块或用户服务部署
（[`OS-Design.md`](../OS-Design.md)、
[`syscall_table.def`](../../kernel/abi/linux/syscall_table.def)、
[`deployment-profiles.md`](../drivers/guide/deployment-profiles.md)）。
<<<<<<< HEAD
>>>>>>> 38c53637 (linux abi: complete syscall breadth on all four mainline architectures)
=======
>>>>>>> 0779cfdf (Merge branch 'fqwqf/linux-abi-depth')
=======
**源码事实（A20OS）。** A20OS 把调度、MM、VFS、cache、网络和关键驱动放在同一 特权地址空间，以直接 C 调用连接；同时提供 Linux ABI（343 个登记入口）和 Native ABI（126 个登记入口）。登记数只表示分发表面积，不表示 Linux 语义完整性。 Native ABI 使用 handle/rights、VMO/VMAR、 channel/EventQ；generic 驱动可作为 `.a20drv` 内核模块或用户服务部署 （[`OS-Design.md`](../OS-Design.md)、 [`syscall_table.def`](../../kernel/abi/linux/syscall_table.def)、 [`deployment-profiles.md`](../drivers/guide/deployment-profiles.md)）。
>>>>>>> ee4f852a (docs: add distro-rootfs run docs; unwrap mid-sentence hard line breaks)
=======
**源码事实（A20OS）。** A20OS 把调度、MM、VFS、cache、网络和关键驱动放在同一 特权地址空间，以直接 C 调用连接；同时提供 Linux ABI（343 个登记入口）和 Native ABI（126 个登记入口）。登记数只表示分发表面积，不表示 Linux 语义完整性。 Native ABI 使用 handle/rights、VMO/VMAR、 channel/EventQ；generic 驱动可作为 `.a20drv` 内核模块或用户服务部署 （[`OS-Design.md`](../OS-Design.md)、 [`syscall_table.def`](../../kernel/abi/linux/syscall_table.def)、 [`deployment-profiles.md`](../drivers/guide/deployment-profiles.md)）。
>>>>>>> be312e5b (Merge upstream/main at 50956544)

**源码事实（Linux）。** Linux 同样是宏内核，但其内部子系统具有更成熟的模块边界： VFS 对 filesystem、driver model 对 bus/device、networking 对 protocol/qdisc、LSM 对安全策略提供长期稳定接口；配置与模块体系由 Kconfig/Kbuild 管理 （[Linux VFS 官方文档](https://docs.kernel.org/filesystems/vfs.html)、 [Linux driver model](https://docs.kernel.org/driver-api/driver-model/index.html)、 [Linux Kbuild](https://docs.kernel.org/kbuild/index.html)）。

**推断。** A20OS 路径短、抽象层少，利于定位和针对单一 workload 优化；Linux 的 层次和间接调用成本更高，但换来数十年积累的并发拆分、硬件适配和可插拔能力。 “代码少”不自动等于“更快”：parallel-build 已证明 A20OS 的简单全局结构会在并发、 大工作集下反过来成为主要成本。

### 3.2 多架构与配置

**源码事实。** A20OS 源码包含 RISC-V64/32、LoongArch64、AArch64/ARM32、x86_64、 PPC64LE 和 ARMv7-M；公共层禁止直接依赖架构宏，板级通过 board/driver registry 连接。构建默认 `-O3`、freestanding、静态 kernel，按 `ARCH/BOARD/NR_CPUS/ABI` 生成，默认 UBSAN（bring-up/配置可关）；多数架构链接 `-no-pie`，未见统一 KASLR/CFI/stack-protector/FORTIFY 配置 （[`Makefile`](../../Makefile)、[`build.md`](../build.md)）。

Linux 的 Kconfig/Kbuild、LTO/CFI、stack protector、FORTIFY、KASLR、KASAN/KCSAN 等是可组合的正式基础设施 （[Linux self-protection](https://docs.kernel.org/security/self-protection.html)、 [Linux sanitizer 文档](https://docs.kernel.org/dev-tools/index.html)）。

**优势与风险。** A20OS 的公共层与架构边界清晰，已用双架构负载反复暴露并修复 TLB、packed ring 和架构上下文差异；这是实际优势。相对 Linux，它的硬件覆盖深度、 安全加固组合和每架构验证矩阵仍小，新增架构的“能构建”不等于同等运行质量。

## 4. 进程、线程、调度与 SMP

### 4.1 进程/线程模型

**A20OS 源码事实。** `task_t` 同时保存 PID/TGID/父子关系、files、mm、credentials、 signal、调度、cgroup、park 和 Native handle 状态。`clone` 按 `CLONE_VM`、 `CLONE_SIGHAND`、`CLONE_THREAD` 分享相应对象，否则复制 mm/signal；内核栈固定 64 KiB。PID 用 bitmap + 1024 bucket，默认 `pid_max=32768`；每进程 fdtable 最大 1024，已有 open/cloexec bitmap；全局 `vfile` 表固定 8192 且由单锁保护 （[`proc.h`](../../kernel/include/proc/proc.h)、 [`fork.c`](../../kernel/proc/fork.c)、[`pid.c`](../../kernel/proc/pid.c)、 [`fdtable.c`](../../kernel/fs/fdtable.c)、[`consts.h`](../../kernel/include/core/consts.h)）。

Linux 的 `task_struct`/`mm_struct`/`files_struct` 同样按 clone flag 分享，但 fdtable、 PID、task lifetime 结合 RCU、slab 和动态扩容，适合远大于工作集的对象数量 （[Linux `fork.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/kernel/fork.c?id=db2ddb87143519e20a95aa36c60b36107b736a58#n1994)、 [Linux file table 官方说明](https://docs.kernel.org/filesystems/files.html)）。

**推断。** A20OS 固定表使内存上界、失败方式和审计简单，适合 appliance/嵌入式； 大并发 server、海量 fd/process 会更早遇到容量与全局锁，而 Linux 动态/RCU 结构更适合。

### 4.2 调度器

**A20OS 源码事实。** 每 CPU 一个 64-byte aligned runqueue；level 0 运行 FIFO/RR， level 1 运行普通 EEVDF。普通任务按 weight 更新 vruntime，按 eligibility 与最小 deadline 选取；nice、affinity、cgroup cpuset/quota 已连接。远程入队设置持久 `need_resched` 并发 IPI，安全点才切换；空闲 CPU 尝试 `trylock` 远端 runqueue， 从尾部偷普通任务 （[`sched.c`](../../kernel/proc/sched.c)、 [`eevdf-scheduler.md`](../eevdf-scheduler.md)）。

运行队列的 EEVDF 插入、eligible pick 和 RT pick 基于链表扫描，最坏 O(n)；窃取只在 本地空闲时发生，没有 Linux 的 scheduling domains、周期负载平衡、容量/NUMA/能效 模型。`proc_lock` 已从普通 picker 拆出，但仍参与任务状态、父子、wait 和 switch publication。CPU mask 与 mm active CPU mask 是 32 bit。

Linux 当前公平调度也采用 EEVDF 思路，但以增广红黑树维护 eligible task，并结合 per-CPU rq、PELT、sched domains、capacity-aware 与 NUMA balancing （[Linux EEVDF 官方文档](https://docs.kernel.org/scheduler/sched-eevdf.html)、 [Linux `fair.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/kernel/sched/fair.c?id=db2ddb87143519e20a95aa36c60b36107b736a58#n9912)）。

**性能判断。** 小 runnable set 下，A20OS 链表常数小、实现直接，可能很有竞争力， 但**未实测**。大量 runnable task、异构 CPU、NUMA 或频繁迁移时，O(n) picker 与简单 steal 策略是明确的结构性劣势。parallel-build 的当前热点计数没有证明 runqueue 锁是 剩余主瓶颈，因此不应先做高风险调度重写。

### 4.3 SMP、中断与空闲

**A20OS 源码事实。** `core/smp.c` 发现/启动 CPU、维护 32-bit online mask；平台提供 IPI 与 remote TLB flush。RISC-V 使用 PLIC + SBI/软件中断，LoongArch 使用 EIOINTC/ PCH-PIC；IPI handler 只确认并留下 reschedule 请求。timer 动态重设到最近 deadline。 驱动 hwapi 支持 256 IRQ 和 shared handler chain，但设备/平台能力不均 （[`smp.c`](../../kernel/core/smp.c)、 [`riscv64 irqchip.c`](../../kernel/arch/riscv64/trap/irqchip.c)、 [`loongarch64 irqchip.c`](../../kernel/arch/loongarch64/trap/irqchip.c)、 [`driver implementation status`](../drivers/meta/implementation-status.md)）。

**优势。** “IPI 只请求、统一安全点调度”和 prepare-then-WFI 的 idle 协议减少了任意 中断栈直接切换的状态空间；旧 busy-loop 已有定点证据并已修复。**劣势/风险。** 32-bit CPU mask、无 CPU hotplug/topology/NO_HZ 层次、不同架构 IRQ 深度不一致，限制向大 SMP 扩展。下一步先扩展 mask/拓扑数据结构并测量 IPI/TLB/idle，而不是直接引入复杂 topology。

## 5. 内存管理、虚拟内存与 TLB

### 5.1 物理内存与 slab

**A20OS 源码事实。** 物理页由一个全局 spinlock 保护的 buddy allocator 管理；slab 按不超过 2048 byte 的 size class 分组，每个 class 一个共享 spinlock，没有 per-CPU magazine。大对象回到 buddy；源码包含简单回收和 swap 路径，但默认 Makefile 强制 `CONFIG_SWAP=n`，且 OOM/swap 路径源码仍标注需要在块 I/O 前安装 busy swap PTE 并释放 mm lock （[`frame.c`](../../kernel/mm/frame.c)、[`slab.c`](../../kernel/mm/slab.c)、 [`oom.c`](../../kernel/mm/oom.c)、[`Makefile`](../../Makefile)）。

Linux buddy 按 zone/node 组织，SLUB 有 per-CPU fast path，回收由 lruvec、workingset、 memcg、kswapd 等共同完成 （[Linux physical memory 官方文档](https://docs.kernel.org/mm/physical_memory.html)、 [Linux SLUB](https://docs.kernel.org/mm/slub.html)、 [Linux page reclaim](https://docs.kernel.org/mm/page_reclaim.html)）。

**推断。** A20OS 在 8 核编译负载下尚能稳定运行，但全局 buddy 和共享 slab-class lock 会随 CPU/分配频率增加而形成 cacheline ping-pong。优先加 per-CPU 小对象 magazine、 批量 page alloc/free 和真实 lock-wait counter；NUMA 应后置。

### 5.2 VMA、缺页、COW

**A20OS 源码事实。** 每个 mm 的 VMA 是按地址排序的双向链表，由一个 spinlock 保护； gap 查找、覆盖区间和多处 lookup 为线性遍历。支持匿名/文件映射、shared page-cache 映射、private copy、COW、mprotect/mremap、huge-page demotion 和 Native VMO/VMAR。 源码注释承认部分读取路径仍依赖排他 ownership，未来需 RCU-style VMA lifetime （[`vm.h`](../../kernel/include/mm/vm.h)、[`mmap.c`](../../kernel/mm/mmap.c)、 [`fault.c`](../../kernel/mm/fault.c)）。

Linux VMA 当前由 Maple Tree 索引，并通过 VMA lock、RCU/read-side 优化与 page-table lock 分层处理并发 fault/unmap （[Linux process addresses](https://docs.kernel.org/mm/process_addrs.html)、 [Linux Maple Tree](https://docs.kernel.org/core-api/maple_tree.html)、 [Linux `mmap.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/mm/mmap.c?id=db2ddb87143519e20a95aa36c60b36107b736a58#n336)）。

**性能判断。** 少量 VMA 的普通命令行进程里，链表简单可控；JIT、浏览器、语言 runtime、 数据库和大量 mmap workload 中，O(n) gap/search 与单 mm lock 是明显劣势。parallel-build 没有单独证明 VMA 是当前第一热点，优化前应加入 VMA walk length、fault latency、 mm-lock contention 分布。

### 5.3 TLB shootdown 与页生命周期

**A20OS 源码事实。** mm 记录 active CPU mask；PTE clear/replace 后形成 per-mm invalidation transaction，按页或全局本地 flush，并向正在运行该 mm 的远端 CPU 发 shootdown；旧 frame/page-cache backing 延迟到 flush 完成才释放。该设计来自真实 8c rustc ICE/SIGILL/SIGSEGV/ADE 根因修复 （[`vm.c`](../../kernel/mm/vm.c)、本地归档 `.kernel-build/smoke/2026/EVALUATION.md`）。

**优势。** 与无条件广播相比，active-mask 和 range transaction 有明确的低开销潜力， 且生命周期顺序已有压力证据。**限制。** 32 CPU 上限、transaction 由 mm mutex 串行、 缺少 Linux 的 mmu_gather/RCU/architecture batching 成熟组合；优势尚无和 Linux 的 shootdown 微基准。建议保留正确性协议，先测 remote CPU 数、flush range、hold list 长度和 stall，再做 batch/coalescing。

一个额外热路径是 `mm_sync_shared_dirty_for_vnode()`：它持 `proc_lock` 扫描所有 task、 mm、VMA 和 leaf 来同步共享脏映射，并被普通文件 read/fsync 路径触发 （[`mmap.c`](../../kernel/mm/mmap.c)、[`file.c`](../../kernel/fs/vfs/file.c)）。 这是高优先级结构性候选：应改为 vnode/mapping 反向索引或直接由 shared mapping 的 统一 page-cache dirty 状态驱动，避免全系统任务扫描。

## 6. VFS、ext4、缓存、写回与持久化

### 6.1 VFS 与 pathname

**A20OS 源码事实。** VFS 以 `vnode_ops`/`vfile_ops` 抽象 filesystem 和打开文件； `vfile` 自带 offset mutex。dcache 当前是固定 2048 entry、512 hash bucket、单全局 spinlock 和全局 LRU/free list；只对 ramfs/FAT32/ext4 启用。全局 open-file 对象表 固定 8192，仍由 `g_file_lock` 串行分配/释放 （[`vfs.h`](../../kernel/include/fs/vfs.h)、 [`dcache.c`](../../kernel/fs/vfs/dcache.c)、[`file.c`](../../kernel/fs/file.c)）。

Linux VFS 的 dentry/inode/address_space 生命周期采用 hash、LRU、RCU/refcount 和 filesystem-specific operations；pathname 有 RCU-walk fast path，遇到需阻塞或重验证 才回退 ref-walk （[Linux VFS](https://docs.kernel.org/filesystems/vfs.html)、 [Linux pathname lookup](https://docs.kernel.org/filesystems/path-lookup.html)、 [Linux `namei.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/fs/namei.c?id=db2ddb87143519e20a95aa36c60b36107b736a58#n2578)）。

**判断。** A20OS 的哈希化已把当前 parallel-build dcache eviction 降到 0/1，是低复杂度的 有效胜利；但固定容量 + 单锁的扩展性仍弱。短期不应照搬完整 RCU path-walk，而应先加 per-bucket contention、negative hit、path component 数和 eviction 原因，再决定分桶锁、 seqcount/RCU read path 或动态 cache。

### 6.2 Page cache

**A20OS 源码事实。** generic file page cache 是固定 2048 个 4 KiB page（约 8 MiB）、 8192 hash bucket、一个全局 spinlock 和全局 LRU；冷填充由每个 cache page 自身的锁串行。 每 vnode 维护 mapping/dirty 链表，全局也有 dirty list；写回 I/O 期间释放全局 lock， dirty generation 防止旧 writeback 抹掉新 dirty。没有 Linux 式后台 dirty throttling、 folio、readahead state、per-bdi writeback domain 或动态工作集 （[`page_cache.h`](../../kernel/include/fs/page_cache.h)、 [`page_cache.c`](../../kernel/fs/page_cache.c)）。

Linux page cache 由 `address_space` + XArray/folio 统一 buffered I/O 与 mmap，提供 readahead、Dirty/Writeback 状态、writepages、writeback error feedback 和 workingset refault （[Linux page cache](https://docs.kernel.org/mm/page_cache.html)、 [Linux VFS address_space](https://docs.kernel.org/filesystems/vfs.html#the-address-space-object)、 [Linux `filemap.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/mm/filemap.c?id=db2ddb87143519e20a95aa36c60b36107b736a58#n1947)）。

**优势。** A20OS 将 dirty generation、per-vnode dirty list 和 per-page fill lock 用很少代码 做出来，且经过 1 GiB 双架构 fsync/sync/drop-cache/SHA 回读压力；这是正确性优先的 可审计实现。**劣势。** 8 MiB 固定容量与全局 LRU 对编译、数据库和大型顺序 I/O 太小； 缓存 thrash 和全局 cacheline 争用很可能构成 Linux 剩余差距的一部分，但需要分阶段 counter/trace 量化。

### 6.3 Block cache 与块层

**A20OS 源码事实。** 每个 mount/device 有 512-byte metadata cache 与 4 KiB block page cache；当前约 1024 个 sector entry、2000 个 page entry，哈希 1024/512；一个 spinlock、64 个 fill mutex shard、一个 writeback mutex。同步写回扫描固定 pool， metadata page 写回串行，readahead 仅 1 page；`fsync` 最后会 sync 整个 mount 的 block cache （[`block_cache.h`](../../kernel/include/fs/block_cache.h)、 [`block_cache.c`](../../kernel/fs/block_cache.c)、 [`vfs/file.c`](../../kernel/fs/vfs/file.c)）。

Linux block layer 用 blk-mq 的 per-CPU software queue 和 hardware dispatch queue， 支持 request merge、scheduler、多硬件队列、plug/batch；writeback 按 backing device/ cgroup 分域并能聚合 inode/page （[Linux blk-mq](https://docs.kernel.org/block/blk-mq.html)、 [Linux `blk-mq.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/block/blk-mq.c?id=db2ddb87143519e20a95aa36c60b36107b736a58#n3093)、 [Linux writeback](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/fs/fs-writeback.c?id=db2ddb87143519e20a95aa36c60b36107b736a58#n1980)）。

**推断。** A20OS 的粗粒度 `fsync -> whole mount bcache sync`、pool scan 和单 writeback mutex 会放大多文件并发编译的同步成本。优先将 dirty metadata 按 inode/transaction/ device queue 建索引，支持 extent/sector 合并与批量提交；“扩大数组”只能暂时延缓 thrash。

### 6.4 ext4

**A20OS 源码事实。** ext4 支持 extents、64bit、flex_bg、checksum 保护的 JBD2 replay、 revoke 和 fail-closed feature gate。目录 lookup/add/remove 顺序扫描 block/dirent，未使用 HTree；extent 支持主要覆盖 depth 0/1，部分深树 truncate 保守保留；一个 filesystem-wide `metadata_lock` 保护 lookup/create/mkdir 等，一个 `alloc_lock` 保护分配。当前 vnode cache 是 4096 slot、2048 bucket、free list，allocator 有 group hint/rotor 和满 bitmap byte skip （[`ext4.c`](../../kernel/fs/diskfs/ext4.c)、 [`ext4_namei.c`](../../kernel/fs/diskfs/ext4_namei.c)）。

**最重要的语义差异。** A20OS 的 JBD2 是**挂载恢复器，不是运行时可写 journal**。 它能校验并 replay 官方镜像已有事务，然后标记为空；运行时更新没有 Linux ext4 的完整 ordered/writeback/journal data mode、transaction commit、barrier 与 crash consistency。 项目一致性文档也明确 ext4 `fsync` 只达到部分语义 （[`ext4_journal.c`](../../kernel/fs/diskfs/ext4_journal.c)、 [`fs-consistency-model.md`](../fs/fs-consistency-model.md)）。

Linux ext4 有 delayed allocation、multiblock allocator、extent tree、HTree directory、 flex_bg locality 和完整 JBD2 transaction；只有有效 commit/checksum 的事务才恢复，revoke 阻止旧 metadata 被重放 （[Linux ext4 allocator](https://docs.kernel.org/filesystems/ext4/allocators.html)、 [Linux ext4 directory](https://docs.kernel.org/filesystems/ext4/directory.html)、 [Linux JBD2](https://docs.kernel.org/filesystems/ext4/journal.html)、 [Linux `ext4_add_entry()`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/fs/ext4/namei.c?id=db2ddb87143519e20a95aa36c60b36107b736a58#n2460)）。

**性能与风险。** parallel-build 定点结果显示 `f973` 时 metadata/allocator lock contention 接近 0，因此“立刻拆 ext4 大锁”缺乏本 workload 收益证据；目录线扫、无 delalloc、 小 cache 和同步块提交仍是更可信的候选。任何优化必须保留已经用真实故障证明过的： dirent 尾部合法性、JBD2 checksum/revoke、VirtIO ring barrier、block-cache dirty generation。 A20OS 曾因 packed 16-bit used index 撕裂和旧 bitmap 晚写而产生持久化重复分配，说明 “减少 barrier/锁”不是允许的优化方法 （本地归档 `.kernel-build/smoke/2026/EVALUATION.md`）。

## 7. 系统调用、ABI 与用户态兼容

### 7.1 Linux ABI 覆盖

<<<<<<< HEAD
<<<<<<< HEAD
<<<<<<< HEAD
<<<<<<< HEAD
**源码事实。** Linux syscall table 登记 258 项，但登记只证明可分派，不证明 Linux 语义完整。项目自己的 coverage 表保守地把 fd/path/process/signal/MM/socket、scheduler 和 BPF 等标为 `partial`，namespaces/capabilities 的总体兼容面为 `partial`。其中 scheduler wrapper 连接的是 per-CPU EEVDF/SMP 调度器，但 Linux policy/priority/affinity 边界仍不完整； BPF 只支持 KEP-backed `BPF_PROG_LOAD`、`BPF_PROG_ATTACH`、`BPF_PROG_DETACH`，没有 map 命令。表中不再有直接固定 `-ENOSYS` 的 placeholder：legacy AIO、kernel module、 keyring、fanotify、userfaultfd（MISSING 模式匿名区间）与 `perf_event_open` （PERF_TYPE_SOFTWARE 软件事件）均已实现；仅存的 `-ENOSYS` 是架构/版本正确的 Linux 语义（nfsservctl 在 Linux 4.19 移除、map_shadow_stack 是 x86 CET、riscv_* 与 arch_prctl 架构专属） （[`syscall_coverage.md`](../../kernel/abi/linux/syscall_coverage.md)、 [`syscall_table.def`](../../kernel/abi/linux/syscall_table.def)）。
=======
=======
>>>>>>> 0779cfdf (Merge branch 'fqwqf/linux-abi-depth')
**源码事实。** Linux syscall table 登记 343 项，但登记只证明可分派，不证明 Linux
语义完整。项目自己的 coverage 表保守地把 fd/path/process/signal/MM/socket、scheduler
和 BPF 等标为 `partial`，namespaces/capabilities 的总体兼容面为 `partial`。其中 scheduler
wrapper 连接的是 per-CPU EEVDF/SMP 调度器，但 Linux policy/priority/affinity 边界仍不完整；
BPF 只支持 KEP-backed `BPF_PROG_LOAD`、`BPF_PROG_ATTACH`、`BPF_PROG_DETACH`，没有 map
命令。表中不再有直接固定 `-ENOSYS` 的 placeholder：legacy AIO、kernel module、
keyring、fanotify、userfaultfd（MISSING 模式匿名区间）与 `perf_event_open`
（PERF_TYPE_SOFTWARE 软件事件）均已实现；仅存的 `-ENOSYS` 是架构/版本正确的
Linux 语义（nfsservctl 在 Linux 4.19 移除、map_shadow_stack 是 x86 CET、riscv_* 与
arch_prctl 架构专属）
（[`syscall_coverage.md`](../../kernel/abi/linux/syscall_coverage.md)、
[`syscall_table.def`](../../kernel/abi/linux/syscall_table.def)）。
<<<<<<< HEAD
>>>>>>> 38c53637 (linux abi: complete syscall breadth on all four mainline architectures)
=======
>>>>>>> 0779cfdf (Merge branch 'fqwqf/linux-abi-depth')
=======
**源码事实。** Linux syscall table 登记 343 项，但登记只证明可分派，不证明 Linux 语义完整。项目自己的 coverage 表保守地把 fd/path/process/signal/MM/socket、scheduler 和 BPF 等标为 `partial`，namespaces/capabilities 的总体兼容面为 `partial`。其中 scheduler wrapper 连接的是 per-CPU EEVDF/SMP 调度器，但 Linux policy/priority/affinity 边界仍不完整； BPF 只支持 KEP-backed `BPF_PROG_LOAD`、`BPF_PROG_ATTACH`、`BPF_PROG_DETACH`，没有 map 命令。表中不再有直接固定 `-ENOSYS` 的 placeholder：legacy AIO、kernel module、 keyring、fanotify、userfaultfd（MISSING 模式匿名区间）与 `perf_event_open` （PERF_TYPE_SOFTWARE 软件事件）均已实现；仅存的 `-ENOSYS` 是架构/版本正确的 Linux 语义（nfsservctl 在 Linux 4.19 移除、map_shadow_stack 是 x86 CET、riscv_* 与 arch_prctl 架构专属） （[`syscall_coverage.md`](../../kernel/abi/linux/syscall_coverage.md)、 [`syscall_table.def`](../../kernel/abi/linux/syscall_table.def)）。
>>>>>>> ee4f852a (docs: add distro-rootfs run docs; unwrap mid-sentence hard line breaks)
=======
**源码事实。** Linux syscall table 登记 343 项，但登记只证明可分派，不证明 Linux 语义完整。项目自己的 coverage 表保守地把 fd/path/process/signal/MM/socket、scheduler 和 BPF 等标为 `partial`，namespaces/capabilities 的总体兼容面为 `partial`。其中 scheduler wrapper 连接的是 per-CPU EEVDF/SMP 调度器，但 Linux policy/priority/affinity 边界仍不完整； BPF 只支持 KEP-backed `BPF_PROG_LOAD`、`BPF_PROG_ATTACH`、`BPF_PROG_DETACH`，没有 map 命令。表中不再有直接固定 `-ENOSYS` 的 placeholder：legacy AIO、kernel module、 keyring、fanotify、userfaultfd（MISSING 模式匿名区间）与 `perf_event_open` （PERF_TYPE_SOFTWARE 软件事件）均已实现；仅存的 `-ENOSYS` 是架构/版本正确的 Linux 语义（nfsservctl 在 Linux 4.19 移除、map_shadow_stack 是 x86 CET、riscv_* 与 arch_prctl 架构专属） （[`syscall_coverage.md`](../../kernel/abi/linux/syscall_coverage.md)、 [`syscall_table.def`](../../kernel/abi/linux/syscall_table.def)）。
>>>>>>> be312e5b (Merge upstream/main at 50956544)

Linux ABI 本身持续演进，语义不仅是 syscall number，还包括 flags、错误码、race、 restart、namespace、credentials、memory ordering 和 filesystem edge cases。A20OS 薄 ABI wrapper/内部 API 分层清楚，是维护优势；“有 entry”绝不能当成 full compatibility。

### 7.2 Native ABI

**源码事实。** Native ABI 的 handle table 初始 256、可增长到 65536、默认 quota 4096； entry 带 object type、rights、expiry tick、remaining ops、security label 和生命周期状态。 install/dup/transfer 只能保持或削弱权限；channel/EventQ/VMO/VMAR 为一等对象。对象表 按 task 隔离，thread 可共享 process handle table （[`handle_table.h`](../../kernel/include/ipc/handle_table.h)、 [`handle_table.c`](../../kernel/abi/native/handle_table.c)、 [`Native ABI 文档`](../native-abi/00-overview.md)）。

**潜在优势（未实测）。** 对新程序，handle + typed object 能减少 Linux fd/ioctl/路径式 接口的歧义，显式 rights attenuation 有利于最小权限和用户态服务化。代价是 Linux 软件 必须走兼容 ABI，两套 API/对象映射增大测试面；temporal sweeper 的全局 registry 与 per-table 扫描也需要在大对象量下测量。

### 7.3 应避免的“性能优化”

- 不得把 `partial` syscall 固定成功或忽略 flags 来降低耗时；这会让用户态错误地继续执行。
- 不得伪造 CPU 数、时间、`/proc/uptime` 或跳过 helper/compile；文档明确禁止。
- 不得以扩大 static capacity 代替资源 accounting；Native/Linux ABI 都需要一致的 `ENOMEM/ENOSPC/EMFILE` 与回收路径。

## 8. 网络栈

**A20OS 源码事实。** A20OS 使用 lwIP `NO_SYS=1`，启用 IPv4/IPv6、ICMP、TCP、UDP、 DHCP、DNS；lwIP socket/netconn layer 关闭，由 A20 自己的 socket facade 连接 Linux ABI。 lwIP core 由单一 `g_lwip_lock` 串行；socket 表最多 1024 且有另一个 `g_net_lock`。 lwIP 固定 heap 512 KiB、TCP PCB 64、UDP PCB 32、pbuf pool 256、TCP window 32 MSS； 最多 4 个网络设备，IRQ/bottom-half 已存在，但仍有 process-context polling bridge （[`lwipopts.h`](../../kernel/net/lwip_port/lwipopts.h)、 [`lwip_stack.c`](../../kernel/net/lwip_stack.c)、 [`socket_internal.h`](../../kernel/net/socket_internal.h)、 [`progress.c`](../../kernel/core/progress.c)）。

Linux networking 以 `sk_buff`/page frags、per-CPU backlog、NAPI budget、RSS/RPS/RFS/XPS、 GRO/GSO/TSO、多队列 qdisc 与 protocol-specific locking 扩展 （[Linux sk_buff](https://docs.kernel.org/networking/skbuff.html)、 [Linux NAPI](https://docs.kernel.org/networking/napi.html)、 [Linux scaling](https://docs.kernel.org/networking/scaling.html)、 [Linux `net_rx_action()`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/net/core/dev.c?id=db2ddb87143519e20a95aa36c60b36107b736a58#n7916)）。

**优势。** lwIP footprint 小、协议路径短、固定 pool 可预测，适合低并发 appliance、MCU、 控制面和开发板。**劣势。** 两层全局串行、固定小 window/pool、数据复制、无成熟多队列与 offload，预期不适合多核高吞吐、多流或高带宽高时延网络；这是源码推断，没有 A20/Linux 网络实测。parallel-build 网络不是主负载，网络重构优先级应低于 FS/block/MM。

路线应从 virtio-net multi-queue + NAPI-like budget、per-CPU RX backlog、batch RX/TX 和 锁等待计数开始，再评估 SG/zero-copy/GSO；不要一开始复制 Linux 全协议栈。

## 9. 同步、Futex、信号与时间

### 9.1 Park/Wake 与内核同步

**A20OS 源码事实。** Park/Wake 用每 task 单一 wait token 和单调 `wait_seq`，对象 wait queue 在自身锁下摘出 task+seq，再释放对象锁后唤醒；可以拒绝 stale wake。mutex、cond、 pipe、socket、channel、EventQ 共用这一基础。timeout 用 `proc_lock` 保护的固定 8192 项 min-heap，timer 动态 rearm 最近 deadline （[`park.h`](../../kernel/include/proc/park.h)、[`sync.c`](../../kernel/core/sync.c)、 [`timer_heap.c`](../../kernel/proc/timer_heap.c)）。

**优势。** tokenized protocol 和统一 wake reason 将 lost wake、对象锁/调度锁顺序、信号/ timeout 映射做得可审计，且有 lifetime counters。**扩展风险。** 单 `proc_lock` timeout heap、 每 task 同时只能一个 park、固定 8192 capacity 对海量 timer/waiter 不够；Linux hrtimer 是 per-CPU rb-tree 并按 range/coalescing 管理 （[Linux hrtimer](https://docs.kernel.org/timers/hrtimers.html)、 [Linux `hrtimer.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/kernel/time/hrtimer.c?id=db2ddb87143519e20a95aa36c60b36107b736a58#n1493)）。

### 9.2 Futex

**A20OS 源码事实。** futex 有 64 bucket、动态 waiter、WAIT/WAIT_BITSET/WAKE/ WAKE_BITSET/REQUEUE/CMP_REQUEUE/WAKE_OP，wait 侧在 `mm->lock -> bucket lock` 下重检 用户值避免 lost wake；robust list 已有；PI futex 族（LOCK_PI/UNLOCK_PI/TRYLOCK_PI/ WAIT_REQUEUE_PI/CMP_REQUEUE_PI）以有边界的实现提供（无优先级继承提升）。bucket 按 virtual address hash，物理 key 只在 bucket 内比较；因此同一 shared page 映射到不同 VA 时可能落入不同 bucket，源码明确记录该限制 （[`sys_futex.c`](../../kernel/abi/linux/sys_futex.c)）。

Linux futex 同样以 hash bucket 和 wait/wake linearization 为核心，但 key 能稳定标识 private/shared backing，支持 PI、robust ABI 与 futex2 waitv （[Linux `waitwake.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/kernel/futex/waitwake.c?id=db2ddb87143519e20a95aa36c60b36107b736a58#n178)、 [Linux robust futex ABI](https://docs.kernel.org/locking/robust-futex-ABI.html)、 [Linux futex2](https://docs.kernel.org/userspace-api/futex2.html)）。

**优先级。** 先修 shared futex key/bucket 一致性并加 bucket contention/chain length， 再考虑扩 bucket 或 per-mm private table；PI/futex2 是兼容与实时性项目，不应在没有负载 需求时抢占 parallel-build 优先级。

### 9.3 信号

**A20OS 源码事实。** 支持 64 signals、sigaction/altstack/siginfo/rt frame、process 和 thread pending。process state 用一个 64-bit pending mask，并为每个 signal number 保存 至多一份 128-byte siginfo；per-task thread pending 也是 bitmask （[`signal.h`](../../kernel/include/proc/signal.h)、 [`signal.c`](../../kernel/proc/signal.c)）。

**推断出的语义差异。** 同一实时信号多次排队时，bit + 单 slot 结构会合并/覆盖，而 Linux 对 realtime signals 排队多个 `sigqueue`；这既是兼容风险，也使 A20 的内存和扫描成本更小。 Linux 的 `get_signal()` 在 per-task/shared pending queue 上选择和消费 （[Linux `signal.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/kernel/signal.c?id=db2ddb87143519e20a95aa36c60b36107b736a58#n2810)）。 应先补语义与 queue limit，再测 signal-heavy workload；parallel-build 中信号不是已证热点。

### 9.4 时间

**A20OS 源码事实。** monotonic 来自架构 tick，realtime 由 spinlock 保护 offset/base， RISC-V64 有 seqlock vvar/vDSO clock_gettime/gettimeofday/getcpu。Linux ABI 当前把 process/ thread CPU time、RAW、coarse、boottime 等多个 clock ID 归并到同一 monotonic/realtime， 语义是近似的。POSIX timer 是全局固定 32 项，每 tick 全扫；`timer_getoverrun()` 固定 0， `SIGEV_SIGNAL`/`SIGEV_NONE`/`SIGEV_THREAD_ID` 通知已实现，`SIGEV_THREAD` 因需要执行用户 线程函数而明确拒绝 （[`timekeeping.c`](../../kernel/core/timekeeping.c)、 [`vdso.c`](../../kernel/mm/vdso.c)、[`sys_time.c`](../../kernel/abi/linux/sys_time.c)、 [`sys_timer_posix.c`](../../kernel/abi/linux/sys_timer_posix.c)）。

Linux 分离 clocksource、clockevent、timekeeping、sched_clock 与 per-CPU hrtimer，并向多架构 提供 vDSO fast path （[Linux timekeeping](https://docs.kernel.org/timers/timekeeping.html)、 [Linux `timekeeping.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/kernel/time/timekeeping.c?id=db2ddb87143519e20a95aa36c60b36107b736a58#n961)）。 建议先把 clock semantics 与 timer ownership 做正确，再把 POSIX timer 接入统一 deadline heap/per-CPU timer；不要用降低计时精度或伪造 uptime 换性能。

## 10. 驱动、VirtIO 与 I/O completion

**A20OS 源码事实。** driver core 有 device/driver/bus/class、probe/remove、引用与动态 registry；generic profile 把可选驱动打为 `.a20drv`，early DriverStore 解决根盘驱动 bootstrap，embedded profile 静态链接同一源码。PCI 支持 ECAM/BAR assignment/modern VirtIO capabilities；IRQ、DMA、class publication 和用户态驱动协议已有。相比 Linux， 签名、runtime unload、DMA mask、完整 IOMMU fault handling 等仍有限 （[`implementation-status.md`](../drivers/meta/implementation-status.md)、 [`pci-and-virtio.md`](../drivers/guide/pci-and-virtio.md)）。

**VirtIO block 关键差异。** 当前 `virtio_blk.c` 明确强制 `vt->irq=-1`：此前 IRQ completion 在持续并发 I/O 下丢 queue ownership，QEMU 报 `Virtqueue size exceeded`，所以退回经验证的 polling model。每 request 三 descriptors，有固定 request slots、一个 instance lock；wait loop drain used ring 后指数 `cpu_relax()`。这是正确性优先但性能受限的设计 （[`virtio_blk.c`](../../kernel/drivers/block/virtio_blk.c)）。

Linux virtio-blk 接入 blk-mq，支持 split/packed virtqueue、SG、batch requests、event suppression/kick batching 和 IRQ completion （[Linux VirtIO](https://docs.kernel.org/driver-api/virtio/virtio.html)、 [Linux `virtio_ring.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/virtio/virtio_ring.c?id=db2ddb87143519e20a95aa36c60b36107b736a58#n648)、 [Linux `virtio_blk.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/block/virtio_blk.c?id=db2ddb87143519e20a95aa36c60b36107b736a58#n426)）。

**最高收益候选之一。** 在独立 stress/fault injection 下重建 IRQ/bottom-half completion， 再做 per-vq/per-CPU submission、SG/merge、batch publish/kick/completion。必须保留 16-bit used index 的自然对齐、READ_ONCE/DMA sync/barrier 和 queue ownership；packed layout 撕裂曾导致 真实持久化损坏，禁止为少一次 barrier 回退。

## 11. 可观测性、调试、安全与随机数

### 11.1 可观测性

**A20OS 源码事实。** `/proc/a20` 暴露 bcache、page_cache、oom、task_lifetime、perf、 driver_lifecycle、objects、scheduler slice；perf counters 覆盖 cache scan/probe、TLB transaction、VirtIO poll/completion、idle wait，首次读取才开启，正式 timed build 默认 dormant。 还有 klog ring、kallsyms/backtrace、ptrace 和大量静态/运行时 smoke gate （[`procfs.c`](../../kernel/fs/procfs/procfs.c)、[`perf.c`](../../kernel/core/perf.c)、 [`lifetime.h`](../../kernel/include/proc/lifetime.h)、[`debug.md`](../debug.md)）。

这是 A20OS 的真实工程优势：计数器直接围绕已发生的生命周期/缓存故障，开销边界清楚。 但 Linux 有 perf events、ftrace/tracepoints、eBPF、lockdep、KASAN/KCSAN、BPF profiler 和 更完整 proc/sysfs；A20 的 `perf_event_open` 只提供 PERF_TYPE_SOFTWARE 软件事件 （无 PMU 硬件事件、无 mmap 采样环），BPF 只是 KEP 的受限、A20-specific 程序接口 （[Linux ftrace](https://docs.kernel.org/trace/ftrace.html)、 [Linux BPF](https://docs.kernel.org/bpf/)、 [Linux `perf_event_open`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/kernel/events/core.c?id=db2ddb87143519e20a95aa36c60b36107b736a58#n13883)、 [`sys_bpf.c`](../../kernel/abi/linux/sys_bpf.c)）。

### 11.2 安全模型

**A20OS 源码事实。** Linux ABI 有 uid/gid、mode、xattr、有限 capabilities/chroot；coverage 表把 capabilities 与 namespaces 标为 `partial`。Native ABI 有 object type、rights、 temporal limits 和 3-level security label；KEP verifier 只允许 bounded instructions、 forward jump、受限 context memory，规模小且易审计。驱动可移入用户服务、IOMMU 已有发现/ 基础 domain，但不完整 （[`sys_capability.c`](../../kernel/abi/linux/sys_capability.c)、 [`sys_namespace.c`](../../kernel/abi/linux/sys_namespace.c)、 [`kep.c`](../../kernel/ext/kep.c)、[`handle_table.c`](../../kernel/abi/native/handle_table.c)）。

Linux 有 user/mount/pid/net namespaces、完整 capability sets、seccomp、LSM hooks，可叠加 SELinux/AppArmor/Landlock，并有系统化 hardening （[Linux LSM](https://docs.kernel.org/admin-guide/LSM/index.html)、 [Linux credentials](https://docs.kernel.org/security/credentials.html)、 [Linux self-protection](https://docs.kernel.org/security/self-protection.html)）。

**风险。** A20OS 代码量较小、Native capability 更显式，这是审计面优势；但缺少成熟强制 访问控制、namespace/seccomp、模块签名/unload protection 与统一 hardening，不能声称整体 比 Linux 更安全。尤其默认静态/no-PIE kernel、未见 KASLR/CFI/stack protector/FORTIFY； 这些是 P0/P1 安全基线，不应等到性能完成后才考虑。

**随机数风险。** `core/random.c` 使用 xoshiro-style PRNG，以 ticks、地址、ASID、free pages、 task pointer 和可选硬件 sample 混种；无硬件 RNG 的平台弱 hook 返回 0，仍把 RNG 标记 ready， `/dev/random` 与 `/dev/urandom` 共用它且不阻塞。该实现不能视为 Linux CRNG 等价，ASLR key、 网络 secret 或 capability token 使用前必须建立 entropy accounting、CSPRNG 和 reseed 语义 （[`random.c`](../../kernel/core/random.c)、[`devfs.c`](../../kernel/fs/devfs/devfs.c)、 [Linux random driver](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/char/random.c?id=db2ddb87143519e20a95aa36c60b36107b736a58)）。

## 12. 构建、测试与工程效率

**A20OS 源码事实。** 单仓库 Makefile 按架构、board、ABI、driver profile 生成内核、模块、 rootfs 与测试入口；默认 `-O3`，有 UBSAN、静态边界 gate、双架构 smoke/matrix、正式镜像/ overlay/metadata 归档。官方 parallel-build 中工具链/minibuild/full compile 已通过，说明 Linux ABI 与内核可承载真实 Rust/Cargo 开发负载。`make all` 平台构建本身在 `f973` 报告中为 136.20 s，但这不是 guest parallel-build kernel 性能 （[`Makefile`](../../Makefile)、[`testing-gates.md`](../testing/testing-gates.md)、本地归档 `.kernel-build/smoke/2026/platform-final-20260810T001000Z-f9732348/REPORT.md`）。

Linux Kbuild/Kconfig 能增量追踪 command/config/header 依赖、jobserver 并行、built-in/module、 LLVM/Rust/reproducible builds；KUnit/kselftest 与 sanitizers/lockdep/fault injection 覆盖不同层次 （[Linux Kbuild](https://docs.kernel.org/kbuild/)、 [Linux `Kbuild.include`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/scripts/Kbuild.include?id=db2ddb87143519e20a95aa36c60b36107b736a58#n156)、 [Linux testing overview](https://docs.kernel.org/dev-tools/testing-overview.html)）。

**优势。** A20OS 的 benchmark runner 记录 commit/image/QEMU/judge/overlay，且明确区分开发 probe 与正式样本，是很好的可复现实验基础。**劣势。** 测试广度和硬件/故障注入远小于 Linux； 当前曾多次由完整 rustc/build 暴露 TLB、存储和 IRQ 生命周期问题，说明 unit/property/fault injection 仍需补层。性能路线必须把已有端到端矩阵保留为最后门禁，而不是每次只跑长测试。

## 13. 优势、劣势与适用负载总表

| 维度 | A20OS 相对优势 | A20OS 相对劣势 | 更适合 A20OS 的负载 | 更适合 Linux 的负载 |
|---|---|---|---|---|
| 架构/代码 | 代码路径短、跨架构边界清晰、易定制审计 | 平台深度与长期兼容矩阵小 | 教学、研究、固定板卡/appliance | 通用服务器、广泛硬件生态 |
| 调度 | per-CPU EEVDF、简单可解释、安全点 resched | O(n) 链表、简单 steal、无 topology/NUMA | 少量 task、固定同构 CPU | 海量 runnable、异构/NUMA |
| MM/TLB | targeted active-CPU shootdown、deferred free 已有实证 | VMA 链表、全局 buddy/共享 slab lock、简单 reclaim | 小地址空间、受控内存 | JIT/数据库/浏览器/内存压力 |
| VFS/cache | 结构小、专用计数器、dirty generation 清晰 | 固定小 cache、全局锁、粗 fsync/writeback | 小工作集、只读/轻写 appliance | 编译、数据库、文件服务器 |
| ext4 | fail-closed recovery、受支持子集易审计 | 无运行时 journal/HTree/delalloc，深 extent 有限 | 已知镜像、受控关机 | 强 crash consistency、海量目录 |
| block/VirtIO | 实现直接、poll 模式正确性边界清楚 | poll-only、单锁、无 blk-mq/merge/batch | 简单虚拟盘、小并发 | 高 IOPS、多队列、多设备 |
| 网络 | lwIP footprint 小、固定资源可预测 | 全局锁、小 pool/window、无成熟 offload | 控制面、IoT、低并发 | 高吞吐、多流、复杂策略 |
| ABI/安全 | Linux 生态入口 + Native typed capability | Linux 语义多为 partial，安全加固/LSM 不足 | 新 Native 服务、能力研究 | 容器、多租户、现成 Linux 软件 |
| 可观测性 | 专用 lifecycle/cache counters，低开销、问题导向 | 无 perf/ftrace/eBPF/lockdep 等完整生态 | 定点内核研发 | 大规模 production diagnosis |

表中“优势”除明确实测项外都是源码结构判断，不是 A20OS 比 Linux 更快的测量结果。

## 14. 优化路线：按风险和证据排序

### P0：先守住正确性与测量可信度

1. 为审计基线 `e33c3219` 重新采集绑定准确 commit、官方镜像和 QEMU 版本的功能/性能样本；Linux 对照也必须使用同一 QEMU binary、runner、CPU/内存/overlay/串行策略。正式比较各取 1 个有效冷样本，另用开发 probe 做统计。
2. 将以下协议设为不可退化 invariant：TLB flush 后再 release backing；VirtIO used index 对齐 + DMA barrier；block-cache dirty generation；ext4 dirent/JBD2 checksum/revoke； Park/Wake wait_seq。为每项加 fault injection 或模型化最小测试。
3. 修复明显语义/安全债：runtime ext4 crash-consistency 边界必须清楚暴露；shared futex key；realtime signal queue；clock ID；CSPRNG/entropy；Linux capability/namespace 兼容层 不得冒充隔离。启用并验证 stack protector/FORTIFY/NX/W^X 等低风险 hardening。

### P1：建立“时间去哪了”的低开销分解

1. 把 parallel-build 分成 scheduler/wakeup、fault/TLB、path/dcache/vnode、page-cache hit/miss/ fill/evict、dirty/writeback/fsync、block queue/poll/completion 六类时间与计数。
2. 每个锁记录 acquire、contended、wait cycles、hold cycles 的采样分布；优先关注 `proc_lock`、page-cache、dcache、buddy/slab、ext4 metadata/alloc、block writeback、 lwIP/socket。正式 timed build 继续 dormant/static-key 风格。
3. 增加微基准：fork/exec/wait、parallel create/stat/unlink、mmap fault/COW、shared futex、 fsync、sequential/random block I/O、context switch、网络 pps/throughput。所有结论区分 TCG 与真机。

### P2：parallel-build 高收益、较低风险路径

1. **移除全系统 shared-mmap dirty scan。** 建 vnode/mapping 反向索引或统一 dirty ownership； 这是当前源码中最不合理的跨子系统 O(tasks × VMAs × PTEs) 路径。
2. **动态/分片 page cache。** 从固定 8 MiB 全局 LRU 演进为动态容量、per-bucket/segment lock、 active/inactive 或 refault feedback；先保留 4 KiB page，不急于 folio 化。
3. **细化 writeback。** dirty page/metadata 按 vnode/device 建队列，fsync 只收敛相关 inode 与 必需 metadata；批量连续 block，后台 writeback + dirty throttling，错误传回 fsync。
4. **恢复正确的 IRQ VirtIO block。** 独立 ownership stress 证明后启用 IRQ/BH；再做 SG、merge、 batch kick/completion 和 per-vq queue。先测 polling 消耗与 I/O latency，不凭直觉切换。
5. **ext4 workload 优化。** 实现 HTree lookup（至少读取已有 index）、extent 查找索引、 delayed/multiblock allocation 的安全子集；metadata lock 只有测到 contention 才拆。

### P3：CPU 与内存可伸缩性

1. scheduler EEVDF list 改为按 deadline/eligibility 索引的树或 heap；同时加入周期 imbalance 反馈。保留小 runnable set fast path，避免复制 Linux 全部 sched-domain 策略。
2. VMA 从链表迁移到 interval/maple-like tree，page fault 加 read-mostly/per-VMA lock； TLB transaction 做 range coalescing，CPU mask 动态化并超过 32 CPU。
3. slab 增加 per-CPU magazine，buddy 增加 per-CPU page batch；用 batch refill/drain 维持 简单全局 buddy correctness。之后才考虑 NUMA zone/node。
4. timer 从单全局 heap 演进为 per-CPU deadline queue + 迁移协议；对常用 clock 多架构 vDSO。

### P4：非但决定长期定位的能力

1. 网络：NAPI-like budget、multi-queue、per-CPU backlog、batch、SG/GSO，再评估 lwIP 是否仍 合适。没有网络基准前不宣称吞吐收益。
2. 安全：CSPRNG、module signature、seccomp-like syscall filter、可组合 policy hooks、IOMMU fault handling；Native capability 做跨 ABI threat model 的实现级测试。
3. 可观测性：tracepoint ring、sampling profiler、lock dependency/race detector 的开发配置； 与现有 `/proc/a20` counter 共存，不让正式 hot path 常驻重型格式化。
4. 构建/测试：KUnit-like 内核单元、syscall selftest、filesystem crash/fault injection、driver queue model test、真机双架构性能与功耗矩阵。

## 15. 每阶段验收标准

每个性能补丁都至少回答以下问题：

1. **源码事实：** 改掉了哪个 O(n)、全局锁、重复 poll、I/O 粒度或 cache miss？
2. **定点反馈：** 对应 counter/lock wait/latency 是否按预期变化？工作量（completion、bytes、 files、rustc 数）是否完全相同？
3. **正确性：** 双架构、1/8 核、fsync/drop-cache/SHA、并行 rustc、lifetime/TLB/dirty/queue invariant 是否通过？任一失败都不能用重试成功掩盖。
4. **性能样本：** 优化前后是否同主机、同 QEMU、同镜像、同参数、独占冷 overlay？异常样本 是否记录而非挑掉？
5. **收益归因：** 多项一起改时只报告“批次收益”；需要单项归因时用 A/B 或 counter，不做猜测。
6. **适用边界：** TCG、真机、架构、核数、工作集和 ABI 是否明确？不把 parallel-build 外推到网络/ 数据库/实时系统。

## 16. 证据索引与 Linux 来源版本

### 16.1 A20OS 一手来源

- 总体与子系统：[`OS-Design.md`](../OS-Design.md)、 [`process-scheduler.md`](../process-scheduler.md)、 [`eevdf-scheduler.md`](../eevdf-scheduler.md)、 [`fs-consistency-model.md`](../fs/fs-consistency-model.md)、 [`network-lock-contract.md`](../net/network-lock-contract.md)、 [`driver implementation status`](../drivers/meta/implementation-status.md)。
- 实现：`kernel/proc`、`kernel/mm`、`kernel/fs`、`kernel/net`、`kernel/drivers`、 `kernel/abi/linux`、`kernel/abi/native` 与本报告各节直接链接的头文件/源码。
- 测量证据仅在本地工作区归档：`.kernel-build/smoke/2026/EVALUATION.md`、 `.kernel-build/smoke/2026/platform-final-20260809T204748Z-5190571f/REPORT.md`、 `.kernel-build/smoke/2026/platform-final-20260810T001000Z-f9732348/REPORT.md` 及 `.kernel-build/smoke/2026/metadata/` 下两份 Linux baseline metadata；这些路径不是 clean-clone 链接。

### 16.2 Linux 一手来源

仓库内没有 Linux 主线源码或官方 `Documentation/` 树；因此 Linux 实现事实引用 `docs.kernel.org` 与 `git.kernel.org`。为避免 master 漂移，源码链接固定到本次访问的 Torvalds 主线快照 `db2ddb87143519e20a95aa36c60b36107b736a58`；parallel-build 对照内核版本 仍以 metadata 中 Linux 7.1.6 为准。算法/架构说明使用 Linux 官方文档，关键路径源码使用 上述固定快照。Linux 会继续变化，未来复审应同时记录新快照与 A20 commit。

## 17. 最终判断

A20OS 当前最值得保留的是：小而清楚的内部 API、双 ABI 实验空间、per-CPU EEVDF、 targeted TLB transaction、可审计的 Park/Wake、Native capability、统一驱动部署，以及由 真实故障推动的专用计数器与双架构压力证据。最需要改变的是：固定小缓存、全局锁、线性 索引、粗粒度写回、poll-only VirtIO block、Linux ABI 语义近似和安全/观测基础设施不足。

路线不应是“把 A20OS 复制成 Linux”。应学习 Linux 已被证明有效的原则——**按 CPU/对象 分片、读多写少索引、批处理、局部性、自适应反馈和异步完成**——同时保留 A20OS 的短路径 与可审计状态机。先用 P0/P1 建立严格正确、同平台、可归因的证据，再依次攻击 shared-mmap dirty scan、page/writeback、VirtIO queue 和 ext4 pathname/allocator；这些比 无测量地重写调度器或扩大数组更可能缩小 parallel-build 剩余差距，并为通用负载奠定基础。
