# A20OS 改进 TODO

本文档只记录**尚未完成**的工程瓶颈与剩余工作（最后核实：2026-09）。条目在落地
时即从本文删除，不再保留已完成的 checkbox；实现细节与验证入口留在源码注释、
提交历史和事实文档（[../testing-gates.md](../testing-gates.md)、
[../security/hardening.md](../security/hardening.md)、
[../../kernel/abi/linux/syscall_coverage.md](../../kernel/abi/linux/syscall_coverage.md)）中，
下一个量级的方向评估见 [next-horizon.md](next-horizon.md)。

checkbox 表示实现里程碑，不表示运行结果已在当前提交复验；文中带日期的验证记录均为
历史记录，引用规则见文末"验证环境说明"。

## P0：混合内核改造（Native ABI 本体化）

改造定位、边界原则与阶段验收标准见 [../hybrid-kernel/03-refactor-plan.md](../hybrid-kernel/03-refactor-plan.md)。
阶段一（核心原语契约化）、阶段二（Native ABI SMP 正确性收口）、阶段四（服务接口
IDL 化）已落地，已从本文删除。

- [ ] 阶段三：驱动双态部署框架 + IOMMU/DMA 真隔离
  - IOMMU/DMA 真隔离子项已达成（2026-09-24）：RISC-V IOMMU per-device domain 动态
    claim/map/unmap/release，fault queue 消费把 record 归属到 owner 并立即阻断设备
    bus mastering，`/proc/a20/iommu` 暴露 enabled/domains/maps/unmaps/faults/
    blocked_events/last_fault_* 计数器；`make smoke-iommu-udriver-isolation` 与
    `make smoke-iommu-discovery` 在 QEMU 10.0.13 riscv64 PASS。QEMU ≤10.0 的
    TR_RESPONSE PPN 未左移 10 位，探测同时接受规范与 legacy 编码（上游 master 已修，
    升级后可删兼容分支）。
  - 仍缺：
    - 同源双态契约测试——完整 DRVMOD 驱动（goldfish RTC、ubd 等）仍是独立实现，
      不是同一份源码的两种部署；
    - fault 消费的中断驱动化——当前由 `a20_device_get_info` 拉取 FQ，未接 MSI/WSI；
    - 多设备并发 domain——当前单实例 `g_user_domain`；
    - 非 PCI 设备（virtio-mmio）的 domain 绑定。
  - 完成条件：同一完整驱动源码双态部署通过同一契约测试；用户驱动 DMA 动态绑定
    per-device IOMMU domain，未授权访问产生并消费 fault。
- [ ] 阶段五：Linux 人格层在 Native 原语上重建（starnix 式对照）
  - 已完成起步（作为基线保留在此）：`a20_personality.h` 提供 pipe-shaped
    channel/EventQ facade，`smoke-native-personality` 验证写入、MESSAGE_READY、
    读取和关闭。
  - 仍缺：fd 表、byte-stream accumulator、mmap/VMO、socket 等关键子集在直通实现
    与人格层实现下同通过，语义 diff 与性能对照归档。
  - 完成条件：同上。

## P0：并发与 SMP 就绪

- [ ] 按等待对象分锁，完整消除 `proc_lock` 竞争
  - 现状：tokenized Park/Wake、task 引用与异步所有权收口、timeout heap 所有权、
    SMP runqueue 迁移与持久抢占、本地 pick 锁拆分、EEVDF 替换 MLFQ 均已落地，已从
    本文删除。riscv64 `-smp 8 -accel tcg,thread=multi`（`mm_stress` 后）实测
    `proc_lock` 仍是压倒性热点：**33335 次竞争 / 16191822 自旋**；page cache、
    dcache、block cache、vfile_table 的分桶锁已把各自竞争归零。切换路径的两次获取
    已合并，`proc_wait4` 的 child 全局扫描已改为 per-task children/线程组链表。
  - 剩余工作：callsite 归因显示 `proc_lock` 竞争高度集中在**互斥量 park/wake 协议**
    （`proc_park_prepare/commit/finish` 与 `proc_try_wake` 各自单独持 `proc_lock`）
    与**每次上下文切换的发布路径**（`sched()` 内联进 `idle_loop` 的
    `spin_lock(&proc_lock)`）。消除它需要把 tokenized Park/Wake 状态机从单一全局锁
    改为按等待对象（wait queue / mutex / futex）分锁——这是
    [../eevdf-scheduler.md](../eevdf-scheduler.md) 与既往性能审计明确警告的
    "无完整并行编译负载验证前不做的高风险核心协议重写"，需要先有正式基准复测。
  - 测量与 callsite 归因工具：见 [perf-overhaul.md](perf-overhaul.md) §3。

## P0：Linux ABI 正确性

- [ ] 把满足条件的 Linux syscall 区域从 `partial` 提升到 `full`
  - 当前证据：`kernel/abi/linux/syscall_coverage.md` 中 poll/epoll/select、
    eventfd/timerfd、futex、membarrier、process accounting、rseq、landlock、
    ioprio/pkeys、file handles、SysV/POSIX message queues、keyring、pidfd、
    LSM introspection 已提升为 `full`（门禁见
    [../testing-gates.md](../testing-gates.md) 的 poll/timer 边界语义一节）。
  - 仍为 `partial` 的区域：path and metadata、fd I/O and splice、process lifecycle、
    signals、memory management、scheduler、sockets、namespaces、capabilities、bpf、
    file advice/copy helpers、SysV/POSIX shm and memfd、fanotify、Linux AIO、
    driver modules、cross-process memory、mempolicy/NUMA、new mount API、io_uring。
    namespaces 区域已具备真实 mount namespace 对象模型（`CLONE_NEWNS`、`setns`、
    `/proc/<pid>/ns/mnt`），其余 ns 类型诚实返回 `-EINVAL`，但仍无 userns 与完整
    capabilities，故保持 `partial`。
  - 完成条件：每个升级区域都在覆盖表条目旁列出对应测试。

## P0：MM、Page Cache 与文件映射

- [ ] 为 swap 的真实换出路径补运行门禁
  - 现状：`CONFIG_SWAP` 默认开启（NOMMU 与没有 swap PTE 编码的 riscv32/loongarch32
    由构建入口强制关闭）；`/dev/loopN` 可作 swap 目标，同一 bdev 重复 swapon 返回
    `-EBUSY`，`/proc/swaps` 渲染正确。`make smoke-swap` 覆盖 loop 绑定 → mkswap →
    swapon → `sysinfo` totalswap 与 `/proc/swaps` 断言 → 重复 swapon EBUSY → 触访
    匿名内存 → swapoff 归零。
  - 仍缺：门禁不驱动真实换出——OOM reclaim 每次最多换出 `MAX_SWAP_RECLAIM`=8 页且
    受 2s 冷却限制，1 GiB QEMU 冒烟无法现实触发；`swap_read_page`/缺页读回路径只有
    编译覆盖。
  - 完成条件：低内存实例（或可注入的换出阈值）下门禁真实触发一次换出-读回，并校验
    换出前后的数据一致性。
- [ ] 修复 `make smoke-native-shmring` 挂起（**先前遗留，非 2026-09 改进周期引入**）
  - 复现（2026-09-28，HEAD 含本周期全部改动）：`make smoke-native-shmring` 连续 3 次
    全部挂起——串口日志停在 `[PROC] user task pid=6`（`/bin/native-shmring-rv` 已 exec
    但无输出），QEMU 被 60s 超时 SIGTERM 杀掉，`NATIVE_SHMRING: PASS` 从未出现。
  - 归因：shmring 门禁在 2026-08 的 pfa 空闲链损坏条目里已记录"本工作树与基线
    720e16ab0 均复现"，即在本轮改动落地前就已挂起。逐提交扫描（`origin/main..HEAD`
    的 17 条提交）显示挂起与具体提交无对应关系。
  - 仍缺：根因未定位。VMO 帧所有权（`free_vma_pages` 跳过 `VM_VMO`）已在本周期修复，
    但该门禁仍挂起，说明存在与 VMO 所有权之外的第二因素。
  - 完成条件：`smoke-native-shmring` 稳定 PASS 且能正常 poweroff；在此之前该门禁不得
    被计入"已验证"，`docs/testing-gates.md` 中相关结论按历史记录处理。

## P2：测试门禁与工具

- [ ] 为 driver/device/bus registry 的容量耗尽补运行测试
  - 现状：`kernel/drivers/core/driver_core.c` 的 registry 已从初始容量起在锁保护下
    `krealloc` 扩容，扩容失败记录 capacity-exhausted 错误并返回失败，不再静默丢失
    注册项——源码已满足动态扩容与结构化失败要求，但全仓库没有专用的 exhaustion
    运行测试。
  - 完成条件：注入分配失败后断言注册项未静默丢失，且错误可从 `/proc` 或日志结构化
    读出。
- [ ] 补 `pty_stress` / `timeout_test` 的多架构入口
  - 现状：两个压力程序已分别由 `smoke-pty-stress` 与 `smoke-timeout-test` 覆盖
    （timeout 所有权门禁依赖后者），但都固定在 RISC-V64。
  - 完成条件：与 `smoke-futex-stress-aarch64`、`smoke-network-suite-aarch64` 同形，
    至少覆盖 aarch64。

## P2：仓库卫生与依赖边界

- [ ] 跨架构 `-Werror` 复核
  - 当前证据（2026-09-27，`check-kernel-build-all` 中可在本机执行的部分）：riscv64、
    loongarch64、aarch64、x86_64、ppc64le 以及 VisionFive2 / LS2K1000 板级
    `kernel-only` 全部零警告通过。
  - 仍缺：
    - **riscv32 未通过**：11 处 `-Wint-to-pointer-cast`（32 位下把整数直接转指针），
      分布在 `kernel/ipc/kexec.c`、`kernel/syscall/trace.c`、
      `kernel/abi/linux/sys_proc.c`、`kernel/abi/native/sys_core.c`、
      `kernel/abi/native/sys_native_fs.c`（6 处）、`kernel/abi/native/sys_native_mm.c`。
      另有一处已修：`kernel/proc/proc.c` 的 `proc_mmap` 错误区间判定原先用
      `(int64_t)` 转换 `vaddr_t`，32 位下零扩展使该判定成为死代码，已改为 `(long)`。
    - arm32、loongarch32、armv7m 在当前主机缺交叉工具链，未复核。
  - 完成条件：riscv32 上述 11 处改经 `uintptr_t` 中转，并在有工具链的主机复核
    arm32 / loongarch32 / armv7m。

## 验证环境说明

- 文档不再固化某一台 host 的工具缺失状态。工具链和 QEMU 可用性由对应 build/smoke
  目标在运行时报告。
- 本文按 2026-09 源码核对。文中的验证记录均为历史记录：PASS 只表示它在标注的日期
  与配置下通过，引用为当前结论前必须在当前提交上重新运行。
- Proc/Sched 的当前累计静态门禁是 `make check-doc-test-gates`；双架构 debug/release、
  1 核/8 核运行矩阵是 `make check-proc-step8-local`。
- 需要完整双架构运行矩阵时运行 `make check-proc-step8`，它聚合 RISC-V64 与
  LoongArch64 的 debug/release、单核/八核压力矩阵。
- 项目 Python 命令统一通过 conda 环境 `a20os`；长跑基准入口负责记录 QEMU 命令、
  镜像哈希、退出状态、timeout 与 guest CPU 状态。
- NOMMU 支持集合由构建入口和 `smoke-arch-mmu-matrix` 验证，不以本文中的历史成功
  列表代替当前运行结果。
