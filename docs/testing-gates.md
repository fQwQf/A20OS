# 测试与文档门禁

`DOCS_AS_FACT_CONTRACT`：`docs/`、`kernel/abi/*/*.md` 以及顶层状态文件中的架构文档描述当前实现。未来设计应放在规划材料中，而不是事实文档中。

`TEST_FIRST_ARCHITECTURE_MATRIX`：每个架构债务领域在 TODO 条目可以勾选完成前，都必须有一个可重复执行的门禁。

门禁存在不等于门禁已通过：任何 PASS 结论都必须来自当前提交上的实际运行，不能继承历史结果（最后核实：2026-09）。

| 领域 | 门禁 |
| --- | --- |
| 并发基础 | `make check-concurrency-foundation` |
| task 引用与异步所有权 | `make check-task-lifetime-boundary` |
| Park/Wake 与阻塞点 | `make check-blocking-point-boundary` |
| 信号、停止与远程退出 | `make check-signal-exit-boundary` |
| timeout heap 所有权 | `make check-timeout-ownership-boundary` |
| SMP runqueue、迁移与抢占 | `make check-smp-runqueue-boundary` |
| 本地 pick 锁拆分 | `make check-process-lock-split-boundary` |
| MM/VMA/页表 | `make check-mm-lock-model` |
| 内存模型跑真实软件 | `make smoke-mm-software`（在 mmtest world 里跑 git/vim/gcc/python/nodejs，并要求关机审计全 0；见下文「MM/VMA/页表」的说明） |
| I/O 进展 | `make check-io-progress-model` |
| VFS 抽象 | `make check-vfs-abstraction` |
| ABI 边界 | `make check-abi-boundary` |
| Native ABI 三表交叉 | `make check-native-abi-coverage`（登记表 / 编号表 / `docs/native-abi/` 互相对齐；宿主侧） |
| 双 ABI 编译守卫 | `make check-abi-config-guard`（禁裸 `#ifdef CONFIG_ABI_LINUX`/`#ifdef CONFIG_ABI_NATIVE`；宿主侧） |
| 驱动核心 | `make check-driver-core-model` |
| 外部依赖 | `make check-external-dependency-boundary` |
| 剪裁注册表 | `make check-trim-registry`（`components/trim.toml` 自洽、生成的 `components/trim.mk` 不过期、每个发射变量都有 makefile 消费者；并入 `check-manifests`） |
| 架构边界 | `make check-arch-boundary` |
| SMP 平台边界 | `make check-smp-platform-boundary` |
| 上游包运行（Alpine gcc 经 chroot 编译+运行） | `make smoke-devtools`（需网络拉取上游包，守护 trap.S sp 守卫修复） |
| 3D 通路像素回读（三种架构） | `tools/a20 test smoke-gpu3d-riscv64` / `-x86_64` / `-aarch64`（需 host virgl + `tools/with-virgl-display.sh`；见 [graphics/3d-graphics.md](graphics/3d-graphics.md)） |
| stock Mesa/virgl attach 门禁 | `make smoke-mesa-attach`（需 host virgl）。**断言 guest 起来了 virgl virtio-gpu，且 Mesa 真的 bind 到 virgl**（renderer 字符串判据 `profile renderer: *virgl`）。默认硬失败；`MESA_REQUIRE_VIRGL_ATTACH=0` 可降级为只报告。已通过（renderer `virgl (…radeonsi…)`）。详见 [graphics/3d-graphics.md §0.8](graphics/3d-graphics.md) |

`BUILD_MATRIX_GATE_CONTRACT`：完整 hosted 构建集合是 `riscv64`、`loongarch64`、`aarch64`、`x86_64`、`arm32`、`riscv32` 和 `ppc64le`。Linux 上 `make check-build-matrix` 使用这七项，macOS 的默认集合只含 RISC-V64；需要与主机无关的显式七架构集合时使用 `make check-build-matrix-all`，它额外包含 `loongarch32`（NaiLoong LA32R，仅 kernel-only bring-up，无 QEMU 目标）、VisionFive2 与 LS2K1000 板级构建门禁。每架构门禁列表由根 `Makefile` 的 `SUPPORTED_HOSTED_ARCHES` 单一真源派生，新增架构必须只改那一处。ARMv7-M 由独立 STM32 build/check 目标覆盖，不属于 hosted 用户态矩阵。

`CI_BUILD_MATRIX_CONTRACT`：CI 逐架构构建的就是上述全部七项，由根 `Makefile` 的 `CI_KERNEL_ARCHES` 声明（`riscv64 loongarch64 aarch64 x86_64 arm32 riscv32 ppc64le`），`.github/workflows/ci.yml` 的 `kernel-build` 矩阵经 `make print-ci-kernel-arches` 解析得到，因此 CI 不会与 `SUPPORTED_HOSTED_ARCHES` 漂移。`arm32` 曾因 `kernel/mm/fault.c` 直接调用只在 `ARCH_HAS_PGTABLE_OPS` 下声明的 `mm_addrspace_lock()` / `mm_cursor_*` 事务接口而被排除；这两处现已改经 `fault.c` 的架构 hook 调用，arm32 因此**在** CI 列表里。`pt.h` 的 `ARCH_HAS_PGTABLE_OPS` 守卫本身**没有**被放宽——声明一个没有实现支撑的 cursor 是会静默失败的能力。唯一在 hosted 集合之外的是 `loongarch32`：它需要从源码构建的 cloudspurs la32 工具链，发行版没有可安装的包，CI 容器无法提供。CI 的宿主侧源码契约门禁（`check-honesty-policy`、`check-smoke-cases`、`host-tests`、`check-drm-abi` 与 `check-doc-test-gates` 中不依赖 QEMU 的 6 个子门禁）不进入构建容器，见 [packaging/ci.md](packaging/ci.md)。arm32 的实测红灯（推送树 `cow.c` 缺守卫）、CI 自 2026-09-29 起的全红状态，以及本机无 arm32 工具链时用宿主 gcc 语法沙箱做的验证及其边界，同样记录在 [packaging/ci.md §CI 的内核构建矩阵](packaging/ci.md)。

2026-10-08，本地在清空外置 `.kernel-build` 缓存与七个用户态架构输出后，运行 `make -j8 FF_LINUX_UAPI_ROOT=/tmp/a20-arm-toolchain/root/usr check-build-matrix`，七项 hosted 架构内核 bring-up 与用户态构建全部通过。ARM32 使用临时解包的交叉工具链和 Linux UAPI 头文件；详细命令、边界及远端 CI 状态见 [packaging/ci.md](packaging/ci.md)。此结果仅证明当前本地树构建通过，不能作为远端 CI 绿灯证据。

`ARCH_MMU_RUNTIME_MATRIX_CONTRACT`：`NOMMU_SUPPORTED_ARCHES` 的构建集合是 `arm32`、`aarch64`、`riscv64`、`riscv32`、`armv7m`。`make smoke-arch-mmu-matrix` 的 hosted runtime 集合只包含前四个架构的 MMU 与 NOMMU 八种组合；ARMv7-M 由 STM32 MCU 目标单独处理。LoongArch64、x86_64、PPC64LE 的 NOMMU 配置在构建入口被拒绝。每个 hosted runtime 组合应进入交互式 shell，执行 shell builtin 与外部程序，并通过用户态 `poweroff` 正常关机。架构差异通过 `kernel/arch/<arch>/` 提供的 hook/capability 表达；`make check-arch-boundary` 禁止通用内核代码直接按具体架构条件编译。

`SMP_PLATFORM_BOUNDARY_CONTRACT`：`kernel/core/smp.c` 统一管理逻辑 CPU 拓扑、online 状态、启动等待和 IPI 分派；`kernel/platform/<board>/` 提供 CPU 发现、启动、IPI 和本地控制器 hooks；`kernel/arch/<arch>/platform/smp.c` 只保留 secondary 入口与架构机制，不得按具体 board 编译平台策略。

`ABI_SMOKE_GATE_CONTRACT`：Linux ABI smoke 通过 `smoke-abi-linux` 运行 `syscall_smoke` 和用户态命令；Native ABI 的 `native-handle-test` 与 `native-libc` 是构建检查，其中 `native-libc` 编译 `user/tests/test_liba20c.c`。用于 handle dup/transfer 的 `make smoke-native-handle` 才是 QEMU 运行时覆盖。

`DOC_DRIFT_KEYWORD_GATE`：`stub`、`partial`、`TODO`、`Future`、`not yet`、`for simplicity` 等漂移关键词只有在绑定到明确的覆盖表、TODO 条目或门禁契约时才允许出现。`kernel/external/` 和 `user/external/` 下导入的第三方代码树不参与该门禁。

`HOST_RESOURCE_GATE_CONTRACT`：任何会启动 guest 的门禁都必须先做宿主资源预检。QEMU 申请到宿主机给不出的内存时不会返回非零退出码，而是宿主 OOM killer 挑一个进程杀掉，被杀的通常不是正在被调试的那个对象，所以"启动失败"在这里不是一个可观测的错误路径。预检由 `tools/a20_resource.py` 单一实现：经 `tools/a20` 的实例路径，以及经 `tools/smoke.py` 的 `run_gate()`（每个 `smoke-*` 目标都在那里起 `qemu-system-*`，另有若干目标在自己的 recipe 里显式调用 `tools/a20_resource.py`），调用的是同一个 `gate()` 与同一套 `A20_*` 环境策略，两者不得各自实现等待逻辑。门禁参数必须与该目标 `qemu` 命令行里的 `-m`/`-smp` 一致，否则预检在保护另一件事。等待策略有意分两种：`tools/a20 run/debug/test` 的 `A20_WAIT_TIMEOUT` 默认 `0`（一直等，因为交互式跑实例时"等资源释放"正是期望行为）；`smoke-*` 门禁的可覆盖默认是 `900s` 有界等待，因为 CI 在宿主机磁盘真的满时必须失败而不是挂死。可用性取 `/proc/meminfo` 的 `MemAvailable` 而非 `free`（后者不含可回收 page cache，会让门禁永远阻塞），并发 guest 数单独统计（空闲 vCPU 不进 loadavg，只看 load 无法判断是否已有 guest 占着 CPU）。

`make check-doc-test-gates` 是广泛的聚合门禁，不是快速的纯文档检查。其依赖包含内核构建以及 MM、VFS、驱动生命周期等 QEMU runtime smoke；阻塞点、信号/退出、timeout、SMP runqueue 与本地 pick 五个边界门禁分别依赖 `smoke-proc-stress`、`smoke-futex-stress` 和 `smoke-sched-stress`（在 QEMU 中 grep 运行时日志，而非源码标记），可能运行较长时间。

## 运行手册

### 宿主资源预检

宿主资源预检没有独立入口，随门禁自动执行。手动预检一个即将发起的启动，跑
`python3 tools/a20_resource.py -m 1G -c 1 [--hostfwd 127.0.0.1:5555] [--no-wait]`；
看策略与当前余量用 `tools/a20 resources`（不带参数，输出门禁所依据的宿主预算）。

检查项是内存（`MemAvailable`）、空闲 vCPU、磁盘余量、并发 guest 数，以及本次启动会绑定的宿主端口。任一不足则按策略等待或退出。

门禁卡在 `a20: waiting for host resources (mem ... MiB, ... vCPU, ...)` 而不是启动失败，属于预期行为。先释放资源，或调 `A20_RESERVE_MEM_MB` / `A20_MAX_CONCURRENT` / `A20_MIN_DISK_MB`。`--no-wait` 下会以非零退出码立即失败，用于确认某个需求是否真的能满足。CI 上若持续超时，要检查的是宿主而非被测系统。

### 并发基础

`make check-concurrency-foundation` 检查 `SCHEDULER_CONCURRENCY_PREREQS`、`SCHEDULER_CPU_OWNERSHIP`、`PER_CPU_CURRENT_VALIDATION`、`TASK_STATE_MUTATION_CONTRACT`、`A20_PARK_WAKE_PROTOCOL` 和 `WAIT_QUEUE_PARK_PROTOCOL`，并用两核 bringup 配置验证 SMP 基础编译路径。

失败时先看 `kernel/proc/{sched,current}.c`、`kernel/include/proc/{proc,park}.h`、`kernel/include/core/sync.h` 中对应契约。不要通过删除所有权字段或放宽门禁来绕过失败。

### 内核抢占探针

`user/cmds/core/preempt_lat.c`（经 `instances/preempt-lat-*.toml`）回答一个窄问题：
内核态长 syscall 期间，更高优先级的可运行任务能否在 syscall 返回前拿到 CPU。
hog 子进程循环 `read()` 一个 256MB page-cache 热文件（单次 `read()` 在 TCG 下耗时
数百毫秒，页间无重调度点），RT 子进程以 `SCHED_FIFO(10)` 睡眠-唤醒 20 次并报告
每次迟到量。`preempt_lat <size_mb> [hog_cpu] [rt_cpu]` 的后两个参数用于 SMP 实例
（`preempt-lat-smp-*`，`smp=2`）把两个子进程钉在同一 CPU 上——不钉扎时 RT 会被
放到空闲 CPU，数字失去判别力。

判读：有抢占时最大唤醒延迟为 tick 量级（smp=1 下 1-2 tick；smp=2 下定时器堆被两个
CPU 的 tick 交错扫描，x86_64 实测 ≤1ms）；无抢占对照（CONFIG_KERNEL_PREEMPT=0）
时延迟等于 hog 当前那次 read 的剩余时长。x86_64 必须经
`tools/run_instance_tcg.py <instance>` 强制 TCG——宿主 KVM 下 256MB 的 read 只需
几毫秒，"长 syscall"前提不成立；riscv64 直接 `tools/a20 test preempt-lat-riscv64`。
失败时先核对 `PREEMPT_LAT: rt ... policy=1`（FIFO 是否生效）与 `pin_rc`（SMP 钉扎
是否生效），再查 `kernel/proc/sched.c` 的 `kernel_preempt_at_irq_return()` 条件链。

### Task 引用与异步所有权

主入口是 `make check-task-lifetime-boundary`，双架构累计运行使用 `make check-proc-step35-local`。

检查项：PID 查询必须返回带引用的 task；task list、PID table、 runqueue、dispatch/current、wait/wake 和 timeout owner 的引用能够闭环；`/proc/a20/task_lifetime` 的错误计数与压力测试入口仍然存在；禁止重新引入裸 `proc_find()`。

失败时先确认新增的异步 task 指针是否在发布前 `proc_get()`，并在摘除后的唯一所有者路径 `proc_put()`，再比较压力测试前后的 task/ref、 wait/wake、timeout 和 zombie 基线。

### Park/Wake 与阻塞点

`make check-blocking-point-boundary` 守的就是这条发布边界，完整累计矩阵使用 `make check-proc-step4-local`。

检查项：只有 `task.c`/`park.c` 能发布 `PROC_BLOCKED`，只有 scheduler 白名单能直接调用 `proc_make_ready()`；wait queue、futex、 timeout 和 wake queue entry 都保存 task 引用与 `wait_seq`；Futex wait 在入队前完成用户值二次检查。

失败时把阻塞路径改成 prepare → 对象锁内重查/link → unlock → commit → unlink/recheck → finish；waker 必须在对象锁内 collect，在解锁后 flush，不能直接写 task 状态。

### MM/VMA/页表

`make check-mm-lock-model` 覆盖 MM/VMA/页表这一组静态契约：`MM_LOCK_MODEL`、`MM_VMA_PTE_AUDIT`、`COW/DEMAND_FAULT_TLB_CONTRACT`、`MM_FORK_COW_REGRESSION_GUARD`、`FILE_MMAP_PAGE_CACHE_CONTRACT`、`OOM_RECLAIM_LIFETIME_CONTRACT`、`mm_seg_index_overflow` 等；并确认 `smoke-mm-stress` 与 `MM_STRESS: PASS` 存在。它现在还跑 `smoke-mm-seg-index-overflow`——那条链路的容量溢出分支是整棵树里唯一能被触达的地方，只挂在单跑用例上等于没人验证（见下文「`seg index overflow`」一节）。

RV64 内核 trap 帧的 `t0` 保存回归单独由 `make smoke-rv64-trap-t0` 覆盖。它在单 hart guest 中用启动参数触发汇编哨兵，并等待本 CPU 的 supervisor timer IRQ；没有真实 IRQ 或 `t0` 在返回后变化都会失败。根因、负控结果和寄存器契约见 [mm-fork-mprotect-cow.md](mm-fork-mprotect-cow.md#rv64-内核-trap-的寄存器保存)。

失败时补充或恢复 `kernel/include/mm/vm.h`、`kernel/mm/vm.c`、`kernel/mm/fault.c`、`kernel/include/mm/oom.h` 中对应契约字符串，并确保 MM 压力测试入口未删除。

关机审计行 `[MM-ASM]` 由 `/proc/a20/perf`（`sys_proc.c` 的 `mm_pt_audit_all()`）在每次关机时打印，它是**每页元数据与硬件页表、映射记录与元数据是否全程一致**的机器证据。各字段都是失配计数，正常必须全 0：

- `missing_meta` / `present` / `absent` / `prot` / `cow` —— 逐条比对"元数据是否与该 PTE 一致"。`cow` 比的是状态字节里的 `MM_ST_COW_BIT` 与硬件 `PTE_COW`。这里曾经比的是一张并行的 `cow[]` 位图，那张表已于 §13.19.2 删除：它只被审计读、只被一个没有调用者的写入者写，于是审计长期读到陈旧值，在一次内核完全正确的运行上报了 `cow=153`。
- `vma` —— 正向：一条映射记录若已经有驻留的 PTE 叶，那些页是否至少有一页被元数据认识。**注意判据不是"每条记录都至少有一页被认识"**：记录是映射的授权、元数据是状态，一个刚 mmap 出来没人碰过的区间两者对不上完全合法（按需调页正是模型赖以成立的东西）。用弱判据会在普通程序上开火，而会误报的门禁最后只会被关掉。
- `vmai` —— **反向（P8）**：凡是元数据声称有东西的页（Mapped / COW / 已预留未缺页），是否都有映射记录覆盖。`MM_ST_INVALID` 豁免，因为空洞不是遗漏。正向检查只从记录出发，所以没有这一项时"有状态但无记录"的页是不可见的。
- `cls` —— 双方都认为某页已映射时，对**那是什么**（anon / file / …）的判断是否一致。
- `safe` —— `MM_SAFE_NO_FA` 与 `VM_SEALED` 的一致性。
- `map list` 的 `overlap` / `dead` / `ok` —— 映射记录链表自身的不变量（§13.18 新增，见下）。

> **2026-10-04 更新**：这一段原先把元数据与硬件页表之外的第二个对象称作"VMA"，
> 并把不变式写作"VMA 列表是纯派生"。合并之后那条不变式**不再有意义**——
> 列表不是派生视图，它**就是**记录本身。此处改按"映射记录 ↔ 每页元数据"
> 这一对来表述，这是目前仍然成立的交叉校验关系。

计数失配时审计器另外打印**第一个出错地址**（`[MM-ASM]   first vma_mismatch  va=…`）。只有计数不给出地址，等于没说该读哪个 mutator。

字段在**测量处**被断言：`smoke-mm-pt-race` 的期望正则要求 `vmai=0`，反之则门禁变红。注意 `smoke-mm-stress` **不**断言 `[MM-ASM]` 这一行，它只凭 `MM_STRESS: PASS` 通过，因此不是本字段的门禁——要验证 `vmai` 请用 `smoke-mm-pt-race` 或 `smoke-mm-software`。

#### `make smoke-mm-software`：真实软件门禁

上面这些 smoke 跑的都是**内核自己写的系统调用、用内核自己分配的页**。它们测不到真实程序踩的形状：编译器 mmap 一大块 arena、JIT mprotect 代码页、git 建大索引再 remap、解释器 fork 五千个对象。

`smoke-mm-software`（`tools/mmtest_gate.py`）起 `packages/world/mmtest.world` 的镜像，在里面跑 **git / vim / gcc / python / nodejs**，逐个**验证内容**而不是验证退出码（`git clone` 一个空仓库也是退出 0）。

需要**两个**判定同时成立：

1. `MMTEST_RESULT: PASS` —— 五个都跑完并核对通过。
2. 关机时的 `[MM-ASM]` 审计行全 0。

只有 1 会漏掉"软件跑完了但映射记录与每页元数据已经漂移"；只有 2，一个只 `memset` 的空跑也能过。

审计行**只在关机路径上打印**，所以客端脚本自己 `poweroff -f`。这一点不是形式主义：早期版本没有它，宿主超时杀掉客端，审计从未执行，而门禁因为"没看到错误输出"判成了通过。**一道看不见自己不变量的门禁不算通过**——`tools/mmtest_gate.py` 因此把"没有 `[MM-ASM]` 行"直接判 FAIL，而不是跳过。

它有自己的 target、没有折进 `check-mm-lock-model`，因为它慢（一次完整镜像构建 + 约 3 分钟启动），不适合进默认 check 集合。这个取舍是有意的：慢的门禁容易被 CI 超时砍掉，而被砍掉之后剩下的门禁**全都测不到这一类缺陷**。

当前状态（riscv64，`feat/mm-mmap-retire`；数值为一次真实运行，绝对值随 ASLR 变化）：

```
MMTEST: ALL STAGES PASS
MMTEST_RESULT: PASS
[MM-ASM] pt_pages=11 entries=4096 missing_meta=0 present=0 absent=0 prot=0 cow=0
         vma=0 vmai=0 cls=0 safe=0 anon_virt=0 seg_slots=4 seg_bad=0 seg_kind=0
         seg_ok=43383 seg_diff=0 seg_miss=788
         seg_dispatch=43383 seg_fallback=788
[MM-ASM]   map list: entries=14 overlap=0 dead=0 ok=14
[MM-ASM]   miss why: hole=0 leaf=0 unnamed=151 extent=635 ambig=0 bottom=2
[MM-ASM]   annot lost: table_full=0 nibbles_full=113
[MM-ASM]     full at level 2: 113
[MM-ASM]   seg index overflow: 0
```

**只有取 0 的字段是门槛**（唯一的例外是 `seg index overflow`，它必须**非 0**，见下文该字段一节）。绝对值随 ASLR 变化——同一棵树相邻两次门禁给出
`entries=3072` 与 `entries=4096`、`seg_ok=43081` 与 `43120`。上面这份是一次真实
运行（riscv64，`feat/mm-complete`，提交 `039414d37`）的输出，不是每次都该逐字复现的
期望值：门禁判的是 `mmtest_gate.py` 那几行 `all zero`，不是这里的数字本身。

> **2026-10-04 更新**（合并回 `main`，`84b7bc2c5`）：合并后的一次运行给出
> `pt_pages=10 entries=3584`、`seg_ok=43067`、`seg_miss=1102`。同一批字段，全部仍为 0。
> `seg_dispatch=43067` 对 `seg_fallback=1102`：**回退仍然是回退**，仍然是映射链表在
> 回答，§13.11 的残余没有因为合并而消失。
>
> **2026-10-04 再更新**（索引移出页帧，roadmap §13.20）：`seg_fallback` 从 1102 降到
> **767**（宽度 32），`nibbles_full` 从 3918 降到 **159**。这一档还改了两个读数本身的
> 形态，因为它们此前**不足以支撑它们要支撑的结论**：
>
> - `annot lost` 那一行里的 `full_by_level=[0,0,0]` **已经删掉**。它只记录共享数组用尽的
>   层级，不记录条目名字用尽的层级，于是在 `nibbles_full=3918` 的同一次运行里打印出
>   全零的层级直方图。现在名字用尽也记层级，并且**按层逐行打印**而不是塞进三个槽位
>   （八个层级里的 3..7 此前被静默丢掉），读数形如
>   `full at level 1: 749` / `full at level 2: 3169`。
>
>   ⚠️ 这一行曾经被读错过，roadmap §13.21 更正并留了痕：`ARCH_PT_ROOT_LEVEL` 在
>   riscv64 上**就是 2**，所以"根层一次都没有"是错的——**3169 次全在根层**，
>   即 1 GiB 分辨率。教训是读这个直方图必须先确认根层是第几层，
>   否则会把 1 GiB 的问题当成 2 MiB 的问题去修。
> - `mm_seg_annot_lost[]` 与 `mm_seg_full_lvl[]` 的含义因此**只增不减**，老日志仍可读。

`seg_*` 是 P6 的影子比对字段（见 roadmap §13），含义与门槛各不相同：

`seg_miss` 是覆盖缺口而不是缺陷：它记的是「段表答不出、回退到映射链表」的缺页。
它走过 37626（84%）→ 3796（8.6%）→ 265（0.6%）→ 1084（2.5%），最后一次**上升**
是 §13.15 删掉标注走查里的 provision 换来的——那个 provision 与它自己的调用点
契约矛盾（它会为一个映射把整段区间的页表节点建出来），在只有文件映射被标注时
负担得起，anon 一进来就无界。覆盖率换掉了它，剩下约 2.5% 全部安全回退，
`seg_diff` 始终为 0。

> **2026-10-04 更新：合并之后这组字段的含义变了，名字没变。** 合并前它们比较的是
> **两种不同的映射表示**（页表命名的段 vs 链表里的 VMA）。现在两条路径解析出的
> **是同一条 `mm_seg_t` 记录**——`mm_pt_lookup_seg()` 与 `mm_seg_find()` 返回同一个
> 类型的对象。所以：
> - `seg_ok` / `seg_diff` 不再是"跨表示一致性"，而是**同一批记录的两种解析方式是否
>   指向同一条**：页表节点条目按名字解析出的记录，与链表按地址查出的记录。它们只在
>   一种情况下会不同——2 MiB 的节点条目把名字给了邻居映射时。这仍然是真缺陷
>   （会缺错文件的一页），所以 `seg_diff` 仍是**必须 0**。
> - `seg_fallback` 也不再是"退回另一个真相"，而是"用链表解析而非页表名字解析"。
>   **这正是分派能安全进行的原因**：两条路给出同一个答案，所以残余的 2.5% 缺口
>   是性能问题而不是正确性问题。

| 字段 | 含义 | 门槛 |
| --- | --- | --- |
| `seg_slots` | 带段号的 PT 节点条目数（观测值） | 非 0，否则测量是空转 |
| `seg_bad` | 段号指向**非活**映射记录的条目数 | **必须 0**（会读到已释放的 vnode） |
| `seg_kind` | 节点条目命中的记录，其 kind 与链表查到的覆盖该地址的记录不符的条目数 | **必须 0** |
| `seg_ok` | 页表名字解析出的记录与链表解析出的是同一条的缺页数 | 观测值 |
| `seg_diff` | 两者**不是同一条**（名字落在了邻居映射上）的缺页数 | **必须 0** |
| `seg_miss` | 段表答不出的缺页数（覆盖缺口，非缺陷） | 观测值 |
| `seg_dispatch` | 实际**由页表名字决定**的文件缺页数 | 观测值 |
| `seg_fallback` | 回退到链表解析的文件缺页数 | 观测值 |

`map list` 一行是**门槛**，不是观测值：合并之后（roadmap §13.18）地址空间里只剩**一种**
映射表示，于是没有第二样东西可以比对，可校验的不变量变成链表本身。

| 字段 | 含义 | 门槛 |
| --- | --- | --- |
| `entries` | 链表上的映射记录条数（观测值） | 非 0 |
| `overlap` | 与前一条区间**重叠**的记录数 | **必须 0**（索引按 `start` 有序，二分查找靠的就是它） |
| `dead` | magic 不对或区间倒置的记录数 | **必须 0**（那就是 use-after-free 的现场） |
| `ok` | 通过以上全部检查的记录数 | **必须等于 `entries`** |

四个数合起来说的是一件事：**每个映射都活着、有序、且占用一个连续区间**。
`overlap=0 dead=0 ok=entries` 意味着任何一次 `mm_seg_find()` 二分查找都只会返回
一个答案。

### `seg index overflow`：唯一一个"读 0 不代表通过"的字段（2026-10-04，`feat/mm-mmap-retire`）

有序索引有容量上限 `MM_SEG_INDEX_CAPACITY = 1024`。超过它的地址空间
`mm_seg_find()` 改为走链表。`[MM-ASM]` 里新增一行：

```
[MM-ASM]   seg index overflow: 2791
```

**这一行的门槛与其他所有字段相反：它必须非 0，而且非 0 是"这条路径被执行过"，
不是"出了问题"。** 原因在门禁的设计里，不在计数本身：
`smoke-mm-seg-index-overflow` 若只断言 `MM_SEG_INDEX_OVERFLOW: PASS`，
那么即使映射全部合并成个位数记录、溢出分支一次都没跑，它照样 PASS——
因为"没跑到"和"跑到了并且正确"在只断言 PASS 的门禁里是同一件事。
其余门禁（包括 `smoke-mm-software`）这一行读 0，那才是正常的：
它们的进程只有十几条映射。

这个字段是补出来的，因为该分支里确实藏着一个真缺陷。
`mm_seg_find()` 的重建条件写的是 `seg_index_state != 1`，
把状态 2（"已超容量"）也当成"脏"，于是超容量的地址空间里
**每一次查找**都会重跑完整 rebuild（取满 1024 个引用、发现仍超容量、
全部放回），然后才去走那条本该被替代的链表遍历。
改成 `== 0` 之后同一份负载的计数从 **10407 降到 2791**
（残留部分是 invalidate 带来的、应该存在的开销）。详见 roadmap §13.22。

这条经验比这个字段本身更值得记：**一个断言非零的门禁，仍然可能因为别的原因非零。**
非 0 只证明"某处发生过"，不证明"这里发生过"。
所以该门禁除这一行外**还断言 `[MM-ASM]` 审计全 0**——
溢出回退必须同时与页表对每一条映射都一致，
否则"它返回了某个东西"就是唯一的结论了。
该用例自己在这个意义上错了两次（映射被 `vma_can_merge()` 合并、
建映射时就把页全 fault 完了），两次都记在 roadmap §13.22.4。

这三个数**由门禁强制**，不是印出来给人看的：`tools/mmtest_gate.py` 用 `MAPLIST_RE`
解析这一行，`overlap`/`dead` 非 0 或 `ok != entries` 直接 FAIL，**整行缺失也 FAIL**
（与 `[MM-ASM]` 主行缺失同一条理由：审计没跑过就不等于通过）。这是补上的——合并
刚做完时内核已经在算这三个数，但门禁脚本只解析主行，于是它们一度只是装饰。
现在验证过四种坏输入（`overlap=2` / `dead=1` / `ok<entries` / 整行缺失）全部 FAIL，
干净日志 PASS。

这条审计的形态是被真实故障换来的。地址空间曾经同时有 `vm_area_t` 与 `mm_seg_t`
两套记录，所以这一行当时是 `seg extent: vmas=… noseg=… mismatch=… pte_disagree=…`
加上 `idx_agree/idx_diff`（逐页比较 `mm_find_vma()` 与 `mm_seg_find()` 给的是不是
同一个段）。合并之后那三个计数器**测的东西不存在了**：没有第二个表示，也就没有
"只改了一种、忘了另一种"可查。因此它们被换成对链表本身的不变量检查——不是把门禁
删掉，而是把门禁指向仍然会崩的那个不变量上。

`seg extent` 那一行曾经把一个静默的洞量了出来：门禁自己的进程里 14 个映射有 **10 个
没有段**（堆、栈、brk、SysV shm、io_uring、两条 framebuffer 路径都不建段）。根因是
`mm_mmap_seg_annotate()` 开头一个 `VM_VMO|VM_FILE` 守卫把**建记录**和**标注**
一起挡掉了，而它们本该是两个决定。合并后守卫自然消失，见 roadmap §13.16、§13.18。

`seg_miss` 的成因由 `miss why` 一行给出，`seg_*` 答不出的地址主要落在 `extent`：
条目**有**名字，但没有哪个名字的区间覆盖该地址。这不是没记上，而是记不下——
见下面的「覆盖缺口：容量已经让位，剩下的才是分辨率」。`annot lost` 一行给出反向的证据：
`table_full=0` 说明每个节点页的共享段数组再没满过，`nibbles_full` 说明一段时间里
真正顶住上界的是**每个条目能记几个名字**。

这两个计数器是常驻的，不是临时诊断。它们推翻过两次错误诊断：残余 `seg_miss`
曾被归因为「条目名字不够」，而 `nibbles_full` 实测是 **0**，`table_full` 却是
**7132**——顶住上界的是被 512 个条目共享的那个段数组；后来同一个 `nibbles_full`
又被当成了"几何挡着、调不动"，而它其实只是量一个**没人选过的**页帧余数
（roadmap §13.20）。两次都是同一类错误：**拿一个布局常量当策略上限**。
现在每次调容量都照着这两个数字，且必须同时看 `full at level` 的**层级分布**——
只知道"名字不够"而不知道"在哪一层不够"，仍然分不清是根表拥挤还是 2 MiB 条目拥挤——
而且必须先确认根层是第几层（riscv64 上 `ARCH_PT_ROOT_LEVEL=2`），
否则会把根层的问题误读成下一层的问题（roadmap §13.21）。

### 覆盖缺口：容量已经让位，剩下的才是分辨率

Sv39 上一个节点条目是 2 MiB，所以「给一个映射命名」实际上是**以 2 MiB 分辨率**
命名。一个 2 MiB 条目里若有超过 `MM_SEGTAB_NAMES` 个不同映射，它就无法说明哪个是哪个；
`mm_pt_lookup_seg()` 此时拒绝作答，而不是猜一个。

> **2026-10-04 更正。** 本节原先的标题是「覆盖缺口是分辨率，不是容量」，并且断言
> 「不是再调大常数能消掉的——`idx[]` 是 512 项、住在同一个 order-0 frame 里，
> 7 项/条目是 3592 字节，8 项就是 4104，已经越界」。这个论证**把页帧算术当成了策略**：
> 那个 7 不是被选出来的，是页帧剩下的余数，所以"8 项越界"只说明**布局**挡着，
> 不说明**容量**到顶了。把索引移出页帧之后（roadmap §13.20），
> `MM_SEGTAB_NAMES` 第一次成为一个真正可选的参数，回退随即 1102 → 796（宽度 16）
> → **767**（宽度 32）。

所以现在要把两件事分开看：**容量**这一关已经过了，`nibbles_full` 只剩 **159**
（宽度 7 时是 3918），索引不再是约束；**分辨率**这一关还在，767 次回退里
**625 次是 `extent`**——条目命名了一个不覆盖该地址的段，这确实是 2 MiB 粒度的直接后果。

要再往下走只有提高分辨率（给叶子命名）这一条路，而它的代价要先算出来：
每个 4 KiB 叶条目一个名字，等于给每张叶表再加一张索引表，比 767 次回退大得多。
所以这个取舍应该在做它的时候决定，而不是在回退数字好看的时候决定。

`seg_diff=0` 与 `seg_ok>0` 必须同时成立：前者说明段没有骗人，后者说明它确实在
被使用。只满足前者（`seg_ok=0`）是空转，门禁会照常变绿——所以 `seg_slots` 与
`seg_dispatch` 也在断言行里。`seg_dispatch` 与 `seg_ok`/`seg_miss` 应当两两相等：
相等才说明"段能回答就照段的做，答不出就走回退"。

详见 [roadmap/single-level-mm-model.md §12](roadmap/single-level-mm-model.md) 与
[§13](roadmap/single-level-mm-model.md)。

### I/O 进展

`make check-io-progress-model` 检查 `KERNEL_PROGRESS_SERVICE_CONTRACT`、progress bottom-half 调用点、`LWIP_NO_THREAD_PROGRESS_CONTRACT`、virtio-net 非阻塞路径；禁止在 `kernel/proc/sched.c` 或 `kernel/proc/proc.c` 中直接调用 `virtio_blk_poll_all` 或 `a20_lwip_poll`。

失败时查 `kernel/core/progress.c`、`kernel/proc/sched.c`、`kernel/include/core/progress.h`、`kernel/net/lwip_stack.c` 中对应契约字符串，确认调度器/进程路径没有直接轮询 virtio-blk 或 lwIP。

#### 这个门禁挡下来的三次真实回归

device 半边的桥接由一个聚合 pending 位把门，活性下限由两件事保证：生产者置位，加上按 tick 兜底重新置位。三次回归都表现为"启动挂在块设备之前"，但断点在不同位置。

**devfs 静态名字索引重复构建。** `devfs_mount()` 不只跑一次：VFS 初始化挂 `/dev`，`mount_external_root_pseudo_filesystems()` 随后在外部根下再挂一次 devtmpfs。`devfs_static_index_build()` 每次都往已经填好的表里重新插入 48 个名字，占用率 48 → 96 → 128（满 128 桶），第三次之后 `while (g_static_index[b])` 的线性探测再也找不到空桶，死循环，挂在 `[INIT] Block device -> /extra (ext4)` 之后。修复：按 ready 标志只构建一次、构建前清表、并把插入与查找两处探测都按桶数封顶（表满时退化为"查不到"而不是转圈）。

**兜底间隔算错了单位。** `KERNEL_PROGRESS_FALLBACK_TICKS` 原本写成 `clock_ticks_per_sec() / 10`，而该函数返回的是**计数器频率**（qemu-virt 上是 1e9），不是 IRQ 频率，于是间隔变成约 10^8 次中断，实际上永远不触发；同时没有任何生产者置位（`kernel_progress_note_pending` 连声明都不在头文件里）。块设备遍历因此一次都不跑，aarch64 停在 `[KSWAPD] background reclaimer started`。修复：头文件里公开 `KERNEL_PROGRESS_PENDING_DEVICE` 与 `kernel_progress_note_pending()`，virtio-blk 每次发布请求时置位（并经 `kernel/drvmod/framework.c` 的导出表暴露给 `.a20drv` 模块，否则模块装载会以 -22 失败），兜底改为固定的 8 次中断。门禁现在断言 `kernel_progress_note_pending` 同时出现在这三处，并禁止再用 `clock_ticks_per_sec` 给这个宏缩放。

**aarch64 在 TTBR0 未变时跳过失效。** `__trap_from_kernel`、`user_trap_return` 与 `__switch` 都改成"读 TTBR0，和帧里的一样就跳过 `tlbi vmalle1`"。但陷入处理器可以在**当前**地址空间里缺页调入并写入新映射，TTBR0 相同不代表映射相同；跳过失效后 faulting VA 依然未映射，aarch64 在 ext4 路径里死在 `lfs_crc`。`switch.S` 那处更糟：跳转目标同时跳过了栈恢复与 DAIF 解屏蔽，SP 会停在 `task_context_t` 上。修复：三处都恢复无条件失效，由 `check-arch-boundary` 禁止这两个文件里出现 TTBR0 相等跳转。

### VFS 抽象

`make check-vfs-abstraction` 检查 `VFS_OPEN_DISPATCH_CONTRACT`、`VFS_REFCOUNT_HELPER_CONTRACT`、`VFS_DCACHE_MOUNT_VNODE_INVARIANT`、`VFS_CONCURRENCY_SMOKE_MATRIX` 等静态契约；确认 `vfile_ref_init`/`vfile_get`/`vfile_put_ref_only`、各文件系统 `open` 方法表、`smoke-vfs-stress` 与 `VFS_STRESS: PASS` 存在。

失败时确认 `kernel/fs/vfs.c`、`kernel/fs/file.c`、`kernel/include/fs/vfs.h`、`kernel/include/fs/file.h` 中契约字符串与辅助函数还在，各文件系统后端仍有 `open` 方法，且未直接操作底层 `ref_count`。

### ABI 边界

`make check-abi-boundary` 重新生成 Linux syscall 覆盖表，检查 `LINUX_ABI_BOUNDARY_CONTRACT`、`LINUX_ABI_PLACEHOLDER_RESOLUTION_CONTRACT`、`NATIVE_HANDLE_CAPABILITY_CONSISTENCY_MATRIX`、`NATIVE_DEBUG_LIMITED_CONTRACT` 等静态契约；确认 `docs/native-abi/00-overview.md` 仍包含 `Debug 分区受限`。

失败时先运行 `conda run -n a20os python tools/gen_linux_syscall_coverage.py` 看是否生成失败，再检查 `kernel/abi/linux/syscall_impl.h`、`kernel/abi/linux/syscall_table.def`、`kernel/include/ipc/handle_table.h`、`kernel/abi/native/sys_phase2.c` 中契约字符串，并确认 `00-overview.md` 的 `Debug 分区受限` 说明未删除。

`check-abi-boundary` 的 19 条断言全部是单文件关键词存在性检查，没有一条跨表比对：Linux 侧靠 `tools/gen_linux_syscall_coverage.py` 与 `tools/gen_envelope_coverage.py` 两个生成器逼着每个新 `LINUX_SYSCALL` 写覆盖行与信封分类，Native 侧两样都没有，加一个 `A20_NATIVE_SYSCALL` 不需要任何文档行。下面两条补的就是这个方向。

### Native ABI 三表交叉（check-native-abi-coverage）

`make check-native-abi-coverage` 把 Native ABI 的三份真源两两对齐，并按入口名报错而不是只报文件：

1. `kernel/abi/native/syscall_table.def` 的 `A20_NATIVE_SYSCALL(...)` 与 `kernel/include/abi/native/syscall_nr.h` 的 `A20_SYS_<name>` 必须是同一组名字——登记了没有编号、或有编号没有登记都失败；
2. 每个入口名必须在 `docs/native-abi/` 里出现过——规范文档目录才是“有人做过判断”的记录处；
3. `docs/native-abi/03-handle.md` 的“## 6. 完整 Syscall 列表”里每一行的编号必须与 `syscall_nr.h` 相等，且行的名字必须有对应编号；
4. 反向也查：`syscall_nr.h` 里每个编号都要在 §6 有行，§6 末尾的“总计：N 个 syscall”要等于 `syscall_nr.h` 的登记数，且 `syscall_nr.h` 内不得有两个入口共用一个编号；
5. `user/liba20rt/a20_syscall.h` 这份用户态镜像要与 `syscall_nr.h` 逐个相等——它没有 `A20_SYS_handle_poll` 之类的新入口，用户态就按名字找不到那个 syscall，而任何 include 它的程序都绕不开它。

第 2 条的判定域刻意是整个 `docs/native-abi/` 而不是 `09-native-abi-deepening.md` §1 的机制三分表（应包装 / 应拒绝 / 已有等价物）：§1 判定的是 21 类 Linux 独有内核机制（io_uring、perf、userfaultfd…），142 个 native 入口里只有 17 个是它的判定对象，按 §1 逐条断言会让另外 125 个入口永远无法通过，只能靠编造行把它变成橡皮图章。§1 的作用是判定“Linux 的机制要不要包装成 Native 形式”，不是逐条登记 142 个入口；两者的问题不同，混成一条断言就两头不靠。第 2 条因此查的是“目录里任何一篇出现过这个名字”，不能读成“§6 有这一行”——单行归属由第 3、4 条负责。

失败时读门禁输出里点名的入口名：第 1 条要么在 `syscall_nr.h` 补 `#define`，要么从登记表撤掉；第 2 条说明该入口的语义、权限与边界从未写下来（不是“有意拒绝”，是没人判过），补进 `03-handle.md` §6 或 `09-native-abi-deepening.md` 的对应章节；第 3、4 条是文档编号与代码编号漂移，一律以 `syscall_nr.h` 为准改文档或改编号表；第 5 条在两份头文件里补同一行。§6 的行必须覆盖 `syscall_nr.h` 的每一个编号，只有这一条能抓住“文档漏写整行”——§6 曾经只有 135 行而末尾照抄“总计 142”，两种写法都在门禁加上反向检查之前一直是绿的。

### 双 ABI 编译守卫（check-abi-config-guard）

`make check-abi-config-guard` 禁止 `kernel/**/*.c` 与 `kernel/**/*.h` 里把 `CONFIG_ABI_LINUX` 或 `CONFIG_ABI_NATIVE` 单独当开关用，要求写成 `#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)` 这样的完整形式。匹配面覆盖 `#ifdef` / `#ifndef` / `#if` / `#elif`，行尾带注释、`#if !defined(X)`、`#if defined(X) && …` 都算违规——`$` 锚只拦得住裸写法，一行行尾注释就能绕过。头文件与 `.c` 同扫：`kernel/include/mm/elf.h` 里 `elf_setup_stack_a20()` 的声明曾长期是裸守卫，只扫 `.c` 就等于没看见——声明在头里、定义在 `.c` 里，而调用点（`kernel/proc/exec.c`、`kernel/drivers/core/driver_manager.c`）早已是完整形式。

`Makefile:918` 只定义 `CONFIG_ABI_$(ABI)`，`ABI=both` 时由 `Makefile:1087` 追加 `CONFIG_ABI_NATIVE`。所以裸 `#ifdef CONFIG_ABI_LINUX` 在 both 构建里恒假（`CONFIG_ABI_LINUX` 在 both 下没有任何一处会定义），Linux 侧代码块静默消失——这一侧是正确性问题。`CONFIG_ABI_NATIVE` 一侧不同：`ABI=both` 时它是被定义了的（见上一句），裸 `#ifdef CONFIG_ABI_NATIVE` 今天已经等价于“native 参与本次构建”，并没有把两件事混为一谈；禁它是约定而不是不变量，理由是形式统一——一旦 ABI 宏被解耦，这一侧会以和 LINUX 侧完全相同的方式坏掉，而且坏得没人读得出来。两条当前都并入 `check-doc-test-gates`、`make check` 的宿主侧层与 CI `toolchain-gates` job，与 `check-abi-boundary` 同一步跑。

失败时门禁会列出全部 `file:line`；把该行改成 `defined(...) || defined(...)` 的完整形式即可（当前两种 ABI 语义下等价），不要改成 `#ifdef CONFIG_ABI_BOTH` 单条件——那会让 `ABI=native` 构建丢掉该代码块。

守卫改宽之后，缺的定义必须在每个源集里都存在。MCU 是一处源集与 ABI 宏解耦的地方：`Makefile:1177` 把 `KERNEL_SRC` 整体换成 `components/trim.mk` 的 `TRIM_PROFILE_MCU_SOURCES`，那份表含 `kernel/proc/timer_heap.c` 与 `kernel/proc/sched.c` 但不含 `kernel/proc/timer_posix.c`，而 `TRIM_PROFILE_MCU_CPPFLAGS` 照样发 `-DCONFIG_MCU`，`ABI=both` 照样发 `-DCONFIG_ABI_BOTH -DCONFIG_ABI_NATIVE`。所以 `posix_timer_tick()` / `posix_itimer_cpu_tick()` 在 armv7m 上唯一的定义在 MCU 源集之外，由 `kernel/mcu/mcu_stubs.c` stub（与同文件里 `a20_timer_tick()` / `a20_monitor_tick()` / `psi_tick()` 的处理同形——MCU 源集既无 `syscall/` 也无 `abi/linux/`，POSIX 定时器在那里根本无法被 arm）。armv7m 不进 CI（`Makefile` 的 `check-stm32f103` 只在 `HOST_OS=Darwin` 分支里进 `DEFAULT_KERNEL_CHECK_TARGETS`），所以“新增 ABI 守卫后 MCU 链接失败”这一类只能靠 `make -s print-trim-mcu-sources` 之类的静态比对挡住，CI 不会告诉你。

#### 已知项：守卫改宽后新暴露的面

这一段记的是把 POSIX 定时器守卫从裸 `#ifdef CONFIG_ABI_LINUX` 改成 `defined(...) || defined(CONFIG_ABI_BOTH)` 之后、默认 ABI=both 下的现状。三条都不是这次改动的缺陷，但都由这次改动第一次在默认发布配置里变成活代码，读 `kernel/proc/timer_posix.c` 前先知道：

- **SMP 下的无锁表。** `g_posix_timers`（`kernel/proc/timer_posix.c:56`）与 `g_cpu_itimers`（`:67`）是无锁静态表。`ABI=linux` 下它们已经同时被定时器中断路径（`proc_sched_scan_signal_timers` → `posix_timer_tick()`）与 syscall 上下文（`posix_timer_set_time()`）并发改写，不是新引入的；但 `instances/qemu-riscv64-smp4.toml` 设 `smp = 4` 且不覆盖 `abi`（继承 `Makefile` 的 `both`），即 4 个 CPU 都会走到 `posix_itimer_cpu_tick()`。默认 `NR_CPUS=1`（`Makefile`），常规门禁不会暴露它，所以别按“both 与 linux 走同一段代码、风险相同”来读。
- **错误码面变宽。** `posix_itimer_set()` 在 `kernel/proc/timer_posix.c:346` 于 `proc_get()` 失败时返回 `-EAGAIN`，经 `kernel/abi/linux/sys_timer_posix.c` 的 `sys_setitimer()` 透出成用户可见的 `setitimer` 返回值；`proc_get()` 走的是引用计数，对一个仍存活的 `cur` 失败属于异常路径。Linux 语义里 `setitimer` 不会因表满或引用失败返回 `EAGAIN`。改动前这段在 both 构建里是死代码。
- **顺带修掉的一个既有缺陷。** `sched_set_posix_deadline()`（`kernel/proc/timer_heap.c:449`）唯一的调用方是 `kernel/proc/timer_posix.c:99`，而后者此前在 both 构建里根本没被编进去——`next_posix_scan` 写进去就没人清。改动前只要用户在 both 构建里 arm 过一个 POSIX 定时器，`proc_next_timer_interval()`（`:213`）就会因 `posix <= now` 一直返回 `SCHED_MIN_TIMER_INTERVAL`（`:205`，`TICKS_PER_SEC/10000`，即 100 µs）并被 `sched_rearm_timer()` 反复重装，定时器中断永久停在 100 µs 下限。恢复 `posix_timer_tick()` 等于恢复了清理方。

### 驱动核心

`make check-driver-core-model` 检查 `DRIVER_CORE_CONCURRENCY_MODEL`、`DRIVER_CORE_DYNAMIC_LIMITS`、`DRIVER_PROBE_FAILURE_CLEANUP`、`DRIVER_ENUMERATION_FAILURE_MODEL`、`DRIVER_IRQ_DMA_SEMANTICS`、`DRIVER_SMOKE_MATRIX` 等静态契约；确认驱动生命周期测试、virtio-blk/net、UART、PTY、loop、PCI 与 virtio-mmio 枚举函数存在，且 `kernel/main.c` 不再直接调用 `virtio_blk_init`/`virtio_net_init`。

失败时查 `kernel/drivers/core/driver_core.c`、`kernel/drivers/core/driver_hwapi.c`、`kernel/drivers/core/driver_lifecycle_test.c` 中对应契约字符串，确认 `docs/drivers/README.md` 引用了 `kernel/drivers/` 与 `kernel/platform/`，并修掉直接调用驱动的初始化代码。

### 外部依赖边界

`make check-external-dependency-boundary` 检查 `include $(KERNEL_DIR)/external/lwip/sources.mk` 是否在 Makefile 中，`docs/external-dependencies.md` 是否包含 `EXTERNAL_LWIP_SOURCE_MANIFEST`、`EXTERNAL_LWIP_CONFIG_CONTRACT`、`EXTERNAL_USERLAND_UPGRADE_CHECKLIST`、`EXTERNAL_STATIC_LINK_REBUILD_CONTRACT`、`EXTERNAL_TLSE_WGET_LIMITS` 等，并确认 `Makefile` 中没有直接定义 `LWIP_SRC`。

失败时查 `Makefile` 的 lwIP 包含语句，补充或恢复 `docs/external-dependencies.md` 中的对应契约标题，并确认 `kernel/external/lwip/sources.mk` 包含 `LWIP_SRC` 与 `core/timeouts.c`。

vendored code（`kernel/external/**`、`user/external/**`）一律不纳入第一方质量声明与测试。所有 `kernel` 目录级 `rg` 扫描必须携带 `--glob '!kernel/external/**'` 排除；`user/` 侧检查只允许对具名第一方文件做 marker 断言，禁止目录级扫描。新增检查目标必须遵守同一规则；唯一的例外是明确标注为集成测试的目标（如 `check-external-dependency-boundary` 本身）。

### 架构边界

`make check-arch-boundary` 禁止通用内核代码中出现 `CONFIG_AARCH64`/`CONFIG_ARM32`/`__aarch64__`/`__arm__` 等架构条件编译；验证 LoongArch64、x86_64、PPC64LE 的 NOMMU 构建在入口即被拒绝；确认 `smoke-arch-mmu-matrix` 在 Makefile 与 `docs/OS-Design.md` 中存在。

失败时看 `rg` 扫描结果中是否在 `kernel/arch/**`、`kernel/platform/**`、`kernel/external/**`、`kernel/include/core/arch.h` 之外出现架构宏，并确认 `docs/testing-gates.md` 与 `docs/OS-Design.md` 包含 `ARCH_MMU_RUNTIME_MATRIX_CONTRACT` 与 `smoke-arch-mmu-matrix`。

### 构建矩阵

`make check-build-matrix` 运行当前主机的默认内核 bring-up 与用户态集合；`check-build-matrix-all` 显式运行七个 hosted 架构。两者都确认 `BUILD_MATRIX_GATE_CONTRACT` 字符串仍存在于本文档。

失败时按 `check-build-matrix` 报告的架构逐个修复 `check-<arch>-bringup` 或 `check-<arch>-user` 错误，并确保 `BUILD_MATRIX_GATE_CONTRACT` 仍存在于本文档。

### 架构 / MMU 运行矩阵

`make smoke-arch-mmu-matrix` 在 QEMU 中运行 `arm32`、`aarch64`、`riscv64`、`riscv32` 的 MMU 与 NOMMU 组合，验证 shell 内置命令、外部程序及 `poweroff` 正常关机；它不覆盖 `armv7m` MCU NOMMU。

失败时查看 `.kernel-build/smoke/<arch>[-nommu]-shell.log` 中是否缺少 `A20_MATRIX_<variant>_OK`、`A20_EXTERNAL_OK` 或 `System is going down for power-off NOW`，先修复对应架构的 bringup 或 NOMMU 路径。

### DRM 对象存储的锁契约

`make check-drm-store-locking` 断言 `kernel/drivers/gpu/drm.c` 与
`kernel/drivers/gpu/virtio_gpu.c` 里那些**错了不会编译报错、只会静默地产生数据竞争**
的东西——`g_drm_store.lock` 的存在与初始化、锁内摘表 / 锁外释放资源的
两段式 teardown（`drm_gem_detach_locked` / `drm_gem_drop_storage`）、
`drm_gem_pin()` / `drm_gem_unpin()` 的配对、`inst->command_lock` 的存在，以及
buffer 尺寸上限。同时断言 `docs/drivers/guide/lock-order.md` 里对应条目也在，
免得代码与契约各自漂移。

为什么需要计数断言（`min_count`）：只断言"锁这个符号出现过"是不够的。
把某个 handler 里的 `drm_lock()` 删掉，符号仍然在别处出现，门禁照样全绿——
它会安静地放过自己存在的目的。本门禁因此对 `drm_lock()` / `drm_unlock()`
的**出现次数**设下限（`gates.py` 的 `min_count`）。实测：删掉 15 对
lock/unlock，计数降到 26，断言转红；删掉 lock-order.md 里的契约条目，两条
文档断言转红；恢复后 13/13。

它做不到的事，`tools/gates.toml` 里也写明了：抓不到**单个 handler 里单独一处**
漏锁。源码正则门禁看不到控制流。计数下限的作用是拦住"整体去掉锁"，单个
handler 的纪律仍归 SMP smoke 测试，规则本身记在 lock-order.md。

本门禁由 `check-doc-test-gates` 聚合，因此进 CI。

### DRM UAPI 一致性

`make check-drm-abi` 把 `kernel/include/drivers/gpu/drm.h` 里的每个 `DRM_IOCTL_*` 编号与宿主 `linux-headers` 的 `include/uapi/drm` 逐条比对，并把 `kernel/drivers/gpu/drm.c` 里的线结构体定义抽出来实测尺寸与字段偏移。编号错一个字符，switch 就永远匹配不上、落进 `default` 分支，用户态看到的是 EINVAL/ENOTTY，看起来像 Mesa 或 libdrm 的 bug，而不是头文件里写错了一个常量。本门禁就是为了让这类错误进不了门。宿主没装 `linux-headers` 时干净跳过，因此不会给非 Linux 构建新增失败面。

失败时输出会直接给出 `ours=0x…` 与 `linux=0x…`（或 `a20os=…` 与 `linux=…`）。按差值改头文件，不要通过改 switch 的 case 值来"对上"，那会让内核与上游 UAPI 同时错。结构体尺寸/偏移不符时，说明本地定义与上游分叉了，以 `include/uapi/drm` 为准。

这一门禁可验证为会失败：把 `MODE_RMFB` 改回旧值、或从 `drm_mode_fb_cmd` 删掉一个字段，两个方向都实测退出非零。

### 宿主资源与镜像卫生（启动前门禁）

随 `tools/a20 run|debug|test` 自动执行，无需单独调用。这一门禁管两件事。

（1）资源：实例放不进宿主时等待而不是直接失败。常见原因是别的构建或同机任务刚好结束，等一下自己就好了，立刻拒绝只是把重试推给人。检查可用内存、CPU 负载与磁盘余量。

（2）镜像卫生：world 镜像无 journal，而桌面实例是带 timeout 启动、到点被杀的，所以每次运行都会把镜像写脏。下次启动拿到的是校验和失效、inode 孤儿化、目录项指向未使用 inode 区的文件系统。这种故障表现为 udev worker 超时被杀、cairo 建不出缩放字体、应用起不来，全部看起来像内核坏了。因此启动前跑 `e2fsck -fn`（只读、不修改），脏镜像直接拒绝启动。

资源不足时按提示等，或调大 `A20_PREFLIGHT_TIMEOUT`（秒，0 表示只报告不等）。镜像脏时按提示 `tools/a20 build <实例>` 重建，等待修不好它。

逃生口：`A20_PREFLIGHT=0` 跳过资源门禁，`A20_PREFLIGHT_SKIP_FSCK=1` 放行脏镜像（脏镜像本身是被测对象时用），`A20_PREFLIGHT_VERBOSE=1` 打印每次采样。`A20_PREFLIGHT=0` 不会跳过镜像检查：资源与镜像是两件不同的事。

### Linux ABI smoke

`make smoke-abi-linux` 构建 `riscv64 ABI=linux BRINGUP=0` 的镜像，在 QEMU 中运行 `syscall_smoke` 与 `poweroff`，确认串口日志出现 `SYSCALL_SMOKE: PASS`。

失败时检查 `.kernel-build/smoke/abi-linux-riscv64.log` 中是否因 `SYSCALL_SMOKE: PASS` 未出现而超时，并修复 `user/cmds/core/syscall_smoke.c` 或 Linux ABI 实现。

`make smoke-riscv64` 是独立的 `BRINGUP=1` 启动检查。它要求串口日志出现 `part ok` 与 `System is going down for power-off NOW`（即内核完成 bring-up 并主动关机）；watchdog timeout 视为失败。它不运行用户态或 syscall smoke，不能替代 `smoke-abi-linux`。

### 外来架构 execve 透明转发（smoke-exec-xlator）

`make smoke-exec-xlator` 验证 `execve(外来架构二进制)` 被内核改写为翻译器 re-exec，且该进程对 A20OS 其余设施与原生进程无异。通道默认关闭，本用例通过 cmdline `a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64` 打开。

翻译器与探针由构建变量 `XLATOR=1` 拉进镜像（`tools/targets-xlator.mk` 把它们挂成 `$(FAT32_IMG)` 的前置依赖），这一步完成两件事：由 `tools/xlator_fetch.py` 从 Alpine v3.23 仓库解析 `qemu-x86_64` 的版本化 `.apk`、取出**宿主架构（riscv64）**静态翻译器落进 `user/build/`，并交叉编译 `xlate_probe-x86_64`（外来架构静态探针）。两条产物都随后由 `--check-guest` 断言 `e_machine`：翻译器必须是宿主原生（否则它自己会被送去翻译，造成递归），探针必须是 x86_64。这条断言是为了防止「探针其实编译成了宿主架构、于是根本没走翻译」这类假通过——它一旦发生，smoke 会安静地变成一个原生回归。

用例从 Linux-ABI 启动器 `xlate_exec` 执行，**不显式调用 qemu**，六种模式都必须 PASS（另有 `stage` / `run` 两种模式供 `smoke-exec-xlator-shim` 使用，见下）：

| 模式 | 输入 | 断言 |
|------|------|------|
| `ok` | 合法 x86_64 ELF | `[XLATOR] x86_64 (e_machine=62 abi=linux) → /bin/qemu-x86_64`、`[XLATOR] pid=N execve /bin/xlate_probe-x86_64 (e_machine=62 abi=linux)`、`[WX] ... 翻译器宿主，放行 W\|X`、`XLATE_PROBE: MARK=SMOKE ARGC=2`、退出码 42 |
| `enoexec` | 30 字节损坏 ELF（magic 合法，`e_type=ET_NONE`，`e_machine=0x9999`） | 仍是 `ENOEXEC`，不被误送翻译器 |
| `unconfigured` | 64 字节**合法** ELF 头，`e_machine=183`（aarch64，注册表里有，但没配路径） | 仍是 `ENOEXEC`——起门禁作用的是「配置了翻译器」，不是「内核编译时知道这个架构」 |
| `native` | `e_machine=62`、唯一那个程序头是 `PT_A20_START_INFO` 的 ELF（A20 **原生 ABI**，外来架构） | 仍是 `ENOEXEC`，且**不出现它的 `[XLATOR] pid=` 行**——查找键是 `(e_machine, ABI)` 二元组，见下 |
| `script` | `#!/bin/echo` 脚本 | 仍走 shebang 路径，退出码 0 |
| `toggle` | 合法 x86_64 ELF + `/proc/a20/xlator` | 运行期开关闭环，见下 |

`unconfigured` 与 `enoexec` 断言的是同一个 errno，但排掉的是两种不同的错：前者证明头部校验之后不会「顺手」去找任何能翻译的东西，后者证明判定依据是配置而不是编译期名单。少了它，一个「内核里写死了 x86_64/aarch64 白名单」的实现也能让本用例全绿。

`native` 排掉的是第三种错，而且它是**唯一一个必须靠 fixture 才能测到的错**：同一个 `e_machine=62`，带上 `PT_A20_START_INFO` 就是 A20 原生 ABI 的外来二进制，翻译器不认它（qemu-user 只实现 Linux ABI，而原生 ABI 的入口从 `a20_start_info_t` 里取 `root_dir` / `cwd_dir` / `stdin_handle` 那九个句柄，不是从 fd 表取）。只按 `e_machine` 查表就会把它送给 `qemu-x86_64`，qemu 抱怨一句 `Unable to find a guest_base` 然后退出 1，调用方看到的是「guest 退出码 1」而不是 `ENOEXEC`——一次静默的误转发。已负向验证过：把 `xlator_lookup()` 里的 `|| g_guests[i].abi != key->abi` 去掉，本用例立刻以这条 `FAIL: expected ENOEXEC, got no error (exit 1)` 变红。

这个 fixture 同时是「未来加一个 native-ABI 翻译器」的验收位：那天给 `.def` 加一行 `(x86_64, 62, XLATOR_ABI_NATIVE, …)`，这个模式就该从「必须 `ENOEXEC`」翻转成「必须被转发」，而它的镜像仍写成 `abi=native`。

`toggle` 在**同一个进程**里跑完整条闭环：读节点确认 `enabled=1` → exec 成功（对照组）→ 写 `0`、重读确认 `enabled=0` → 同一个二进制回到 `ENOEXEC` → 写 `1` → 又成功 → 写 `2` / `on` / `01` / `" 1x"` 必须被 `-EINVAL` 拒绝且开关未被改动。顺序是断言的一部分：中间那条 `ENOEXEC` 只有在前后两次 exec 成功夹着时才有意义，所以它不能拆成独立的 smoke 命令。日志里的 `[XLATOR] 通道禁用（经 /proc/a20/xlator）` 与 `通道启用` 是内核侧开关翻转的旁证。

`forbid` 里有两条值得单独说明：`W\^X 策略: off` 禁止用全局 `a20.wx=off` 换取翻译器运行——本实现只给被内核标记的那一个 task 开 W\|X 放行（见 [exec-xlator/03-internals.md](exec-xlator/03-internals.md)）。**本仓库的用户态 W^X 默认策略是 `off`**（`kernel/mm/wx.c` 的 `g_wx_policy = MM_WX_OFF`，默认 `deny` 的写法被实测否掉了：V8 会先把代码段 mprotect 成 W\|X 再写），所以这条 forbid 配得上的前提是 `-append` 里**显式**写了 `a20.wx=deny`——本用例正是这么启的。少了它，这条 forbid 断言的是一个树本身就不会出现的字符串，用例要么永远红要么永远绿，都不携带信息。`waitpid\(\d+\)` 禁止 harness 打印 `waitpid(N): ...`，即不允许 `wait4` 失败被吞成 `st=0` 的假通过。

失败时看 `.kernel-build/smoke/exec-xlator-riscv64.log`：缺少 `[XLATOR] pid=` 说明判定或接线没走到；缺少 `XLATE_PROBE: MARK=` 而 `[XLATOR]` 齐全，说明翻译器起来了但 guest 在 A20OS syscall 面上崩了；`XLATE_EXEC: <mode> FAIL` 则是内核路径问题，`ARGV[i]=` 逐项可对照定位 argv 改写；`XLATE_EXEC: native FAIL` 且日志里有它的 `[XLATOR] pid=` 行，说明查找键退回成了只比 `e_machine`。

启动日志还必须出现 `[XLATOR] aarch64 (e_machine=183 abi=linux) 未配置翻译器`：注册表有两个 guest 而只配了一个，未配的那个必须被点名，否则管理员只能看到一个无从追查的 `ENOEXEC`。注意这行里的 `abi=`：查找键是 `(e_machine, ABI)`，启动日志逐行打出它，意味着「架构认得、ABI 对不上」这种「配置了路径却仍然 `ENOEXEC`」的状况在日志里是可区分的，而不必靠猜。

**默认关闭的契约**不由本用例覆盖，需要单独确认：不带 `a20.xlator` 启动时日志应为 `[XLATOR] 外来架构翻译通道: 禁用`，且 `execve(/bin/xlate_probe-x86_64)` 返回 `Exec format error`（`[ELF] header check failed: r=-8 class=2 data=1 type=2`），与接入前逐字节一致。

### 通道被裁掉（smoke-exec-xlator-off）

`make smoke-exec-xlator-off` 用 `CONFIG_XLATOR=0` 构建并启动，断言的是「特性可以不存在」，而不是「特性被关掉」。三条断言：

- 外来探针 `xlate_probe-x86_64` **确实在镜像里**，且它的 `execve` 是 `ENOEXEC`——用的是 `xlate_exec foreign` 而不是 `enoexec`，前者先 `access()` 确认文件存在。一个不存在的文件同样会得到 `ENOEXEC`，那样的断言等于什么都没断言；
- 日志里**没有任何** `[XLATOR]` 行——连「已禁用」的通知也没有，因为裁掉之后没有留下任何会打印通知的代码；
- `cat /proc/a20/xlator` 报 `No such file or directory`，即运行期开关节点不存在。

关键在于 cmdline 与 `smoke-exec-xlator` **完全一样**（仍然写着 `a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64`）。传了等于没传，才说明关掉的是编译而不是加了个运行时 no-op。

顺带一个踩过的坑：这两个产物落在 `$(USER_BUILD_DIR)`，而 `make -C user clean` 会在用户态 build id 变化时清空该目录。把 `make xlator-assets` 写成 smoke 的 `pre` 步骤就会在**下一次 make 时被删掉**，表现为「镜像里少一个二进制」，看起来像内核问题。现在它们是镜像的前置依赖，顺序由 make 保证。

`CONFIG_XLATOR=0` 带独立的 `BUILD_VARIANT` 分量（输出到 `.kernel-build/…-noxlator/`）。这不是为了整洁：`BUILD_FLAGS_STAMP` 会在 flag 翻转时强制整目录重建，两个用例共用一个输出目录就意味着它们在 `make -j check` 下不能并发。

构建层面另有两条可核对的证据，不依赖 QEMU：`CONFIG_XLATOR=0` 时 `kernel/proc/xlator.c` 不在 `KERNEL_SRC` 里，构建目录中不存在 `proc/xlator.o`，且 `nm kernel.elf | grep xlator` 无输出。

### 换一个形状的翻译器（smoke-exec-xlator-shim）

`make smoke-exec-xlator-shim` 验证「调用约定是配置而不是代码」这一条：`a20.xlator.<guest>.argv` 与 `.env` 两个启动键真的能决定内核拼出的 argv 与环境。

它**不需要下载、也不需要交叉编译器**——这是它存在的理由。被指向的翻译器是 `user/cmds/core/xlate_shim.c`：一个宿主原生的程序，什么都不翻译，只打印自己收到的 argv 和几个指定环境变量然后退出 0；被翻译的「guest」是 `xlate_exec stage <e_machine> <path>` 现写的一个合法 ELF64 头。

用一个只打印的程序做断言对象是刻意的：真翻译器**会容忍**错误的 argv（qemu 就是），所以拿真翻译器测出来的结论是「qemu 忍住了」，不是「内核拼对了」。

启动配置同时挂两个形状不同的 guest：

```
a20.xlator=1
a20.xlator.x86_64=/bin/xlate_shim  a20.xlator.x86_64.argv=@P,@*  a20.xlator.x86_64.env=XLATOR_TEST_ENV=hello
a20.xlator.aarch64=/bin/xlate_shim
```

| guest | 模板 | 断言 shim 看到的 argv |
|---|---|---|
| `x86_64` | cmdline 覆盖 `@P,@*`（Rosetta 形状：路径就是 argv[0]，无任何选项） | `[0]=/bin/xlate_shim`、`[1]=镜像路径`、`[2..]=guest 参数`，**且路径前面没有多余的 argv[0] 参数**；`XLATOR_TEST_ENV=hello` |
| `aarch64` | 注册表默认 `-0 @A @P @*` | `[1]=-0`、`[2]=诱饵`、`[3]=镜像路径`、`[4]=guest 参数` |

`aarch64` 的 `argv[0]` 传的是一个**诱饵**（`xlate_exec run --argv0=decoy-argv0 …`）。用 `execv` 时 `argv[0]` 与路径必然是同一个字符串，`@A` 写错了也看不出来；诱饵让它成为一个真断言。同时这一条也覆盖了「多于一个 guest 同时配置」——此前没有任何用例跑过第二个 guest。

`.argv` 覆盖的非 qemu 形状正是旧设计表达不了的那一个：旧的每 guest `argv0_flag` 列在填 `-` 时只能不发 flag，可调用者的 `argv[0]` 仍然会作为一个位置参数留在路径前面。断言 `[1]` 是路径而不是别的什么，就是这一条的直接证据。

这个用例也跑 `native` 模式，并配两条 `forbid`：`XLATE_SHIM: \S*xlate_native` 与 `[XLATOR] pid=\d+ execve /tmp/xlate_native\.elf`。只断言「结果仍是 `ENOEXEC`」是不够的：`xlate_shim` **什么都能执行**（它是宿主原生程序），所以一次误转发会安静地成功——`execve` 返回 0，shim 打印自己收到的 argv，调用方看不出出错了。`forbid` 才能把它翻出来。

失败时看 `.kernel-build/smoke/exec-xlator-shim-riscv64.log` 的 `XLATE_SHIM: argv[i]=` 逐行：它就是内核拼出来的 argv，不用推断。

### loongarch64 上的同一条通道（smoke-exec-xlator-la64）

`smoke-exec-xlator` 只在 riscv64 上跑。loongarch64 上这条通道曾经是「编进去了但永远配不上」：`XLATOR_SUPPORTED_ARCHES` 里有它、`CONFIG_XLATOR` 在它的构建里为 `y`，但 `arch_bootargs_get()` 静默落到 `kernel/core/bootargs.c` 的 weak 默认、返回 `NULL`，于是 13 个 `a20.*` 键全部读成「不存在」，`/proc/a20/xlator` 永远 `enabled: 0`。`smoke-exec-xlator-la64` 是这个缺口的回归护栏。

它和 riscv64 那条只有一个实质区别：**命令行不是 QEMU 给的，是操作者在串口上敲的**（`UART_CMDLINE=y`，见 [exec-xlator/01-usage.md](exec-xlator/01-usage.md#没有固件时从串口收命令行)）。因此它必须用 `-serial stdio -monitor none -display none` 而不是 `-nographic`（后者的 mux 吞输入），并且在看到 `[UARTCMD]` 提示之后才送字节——QEMU 一拿到管道字节就交给仿真 UART，远早于 guest 编程 16550。

断言与 riscv64 那条逐条相同（标记串、特征退出码 42、`[WX] … 翻译器宿主，放行 W|X`、W^X 策略仍是 deny——这个 deny 由串口敲进去的命令行里的 `a20.wx=deny` 显式给出，因为树本身默认是 `off`，理由见上一节），外加 `[UARTCMD] using command line from the console` 与 `[FDT] bootargs='a20.wx=deny a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64'`：前者证明串口这条路真的走了，后者证明 `arch_bootargs_get()` 的返回值确实进了 `bootargs_get()`，而不是只打印了一行好看的提示。

同一个 case 还顺带把 loongarch64 的 16550 接收路径变成被测过的：QEMU loongarch virt 没有 UART IRQ，所以这块板子 `uart_rx_is_polled = 1`，`arch_uart_poll_getc()` 直接轮询 LSR。在此之前这条路径在这块板子上从未被读过。FCR bit 0（16 字节 FIFO）也是这次打开的：不打开的话一次按键突发落进一字节保持寄存器会自我覆盖，实测丢首字节。

### 外来架构注册表一致性（check-xlator-guests）

`make check-xlator-guests` 是纯文本门禁（无需交叉工具链与 QEMU，因此进 `CHECK_FAST_GATES` 与 CI 的 `toolchain-gates` job）。它断言七件事：

- `kernel/proc/xlator_guests.def` 里每个 guest 都有 `tools/targets-xlator.mk` 中对应的 `XLATOR_GUEST_CC_<name>`；
- 反向也成立（多一个没人用的 CC 定义就是漂移）；
- 每个 `e_machine` 与 `kernel/include/mm/elf.h` 的 `EM_*` 相等；
- 每个 ABI 列是 `XLATOR_ABI_LINUX` 或 `XLATOR_ABI_NATIVE`，并且 `XLATOR_ABI_X` 在 `kernel/include/mm/elf.h` 里真有对应的 `ELF_ABI_X`；
- 每个 `(名字, ABI)` 二元组只出现一次——**同一个名字出现两次是合法的**（一个架构注册两个 ABI），重复的是**二元组**；
- 每个默认 argv 模板只用了内核认识的那三个记号（`@A` / `@P` / `@*`），并且出现了 `@P`；
- `components/trim.toml` 的 xlator capability（经 `components/trim.mk` 喂给 `Makefile` 的 `XLATOR_SUPPORTED_ARCHES`）里每个架构都实现了 `arch_bootargs_get()`。

第四条挡住的是一类特别安静的错：`.def` 是 X-macro，不 `#include` 任何东西，所以 `XLATOR_ABI_NATIVE` 拼错**不会编译失败**——它只是一个没人认领的 `uint8_t`，而 `xlator_lookup()` 比的是 `key->abi`，于是那一行永远匹配不上，那个 guest 的每一次 `execve` 都返回 `ENOEXEC`，启动日志也不会点名。已负向验证过：把 ABI 列临时写成 `XLATOR_ABI_NATIVEY`，门禁 FAIL 并说明「`xlator_lookup()` 按 `(e_machine, ABI)` 查，这一行永远匹配不上」。

第五条是「名字可以重复、键不可以」这条约定的守卫。重复的二元组意味着其中一行永远匹配不上，而且**没有任何日志会提到这件事**：每一条指名它的命令行都会被接受，然后落进第一个槽。已正反双向验证过：加一行 `(x86_64, 62, XLATOR_ABI_NATIVE, …)` 使 PASS（2 guest → 3 guest，两行同名不同 ABI），再加一行完全相同的 `(x86_64, 62, XLATOR_ABI_NATIVE, …)` 则 FAIL 并说明二元组才是查找键。CC 交叉编译器的正反向检查按**架构名**比对而非二元组，因为同一架构的两个 ABI 行用同一把编译器。

第六条是对内核 `xlator_parse_template()` 的复述，不是第二个实现——内核才是权威并在启动时就地拦截，这条只是让同样的笔误在一秒内在宿主上暴露而不是在目标机启动日志里。已负向验证过：未知记号与缺 `@P` 各自 FAIL 且报错不同，`--argv0=@A @P @*` 这种记号粘字面量的写法 PASS。

第七条查的是另一类「名义支持」：架构进了 xlator capability（`components/trim.toml`）只说明 `CONFIG_XLATOR` 编进去了，而通道**能不能被配置**是另一件事——唯一配置入口是 `bootargs_get()`，没有 `arch_bootargs_get()` 的架构会落到 `kernel/core/bootargs.c` 里返回 NULL 的弱默认，于是那个架构上所有 `a20.*` 键全部读作不存在。`loongarch64` 在这条通道的整个生命周期里都是这个状态：它在 `XLATOR_SUPPORTED_ARCHES` 里、`/proc/a20/xlator` 也注册了，可没有一条 `a20.*` 键能生效。显式写一个返回 NULL 的桩是允许的（`arm32` 与 `loongarch32` 就是有意为之，它们根本没有 FDT 通路）；这条拒绝的是**静默**回落——架构从没做过这个决定。已负向验证过：临时删掉 loongarch64 的实现即 FAIL 并指名道姓。

新增或修改 guest 时的正确顺序：先改 `.def`，再改 `targets-xlator.mk`，然后 `make check-xlator-guests`。

它不是预防性的：写下这道门禁时两份清单已经漂移——`XLATOR_GUEST_CC_riscv64` 存在，而 `--guest riscv64` 会被 argparse 直接拒掉。门禁还会扫 `tools/*.py` 里是否重新长出一份 guest→e_machine 的字典（`HOST_MACHINES` 与 `mkrootfs.py` 的架构表是另外两件事，按名字而非按数字区分）。

### 能力信封（研究门禁）

`make smoke-envelope` 构建 `riscv64 ABI=linux BRINGUP=0` 镜像并在 QEMU 中运行 `envelope_smoke`（docs/research/05 的调解器攻击套件）。十三个子场景：信封内正常文件工作、类型拒绝（EPERM）、权限上限拒绝（EACCES）、操作预算耗竭、数据预算预扣、时间预算过期（惰性清扫）、`/proc/self/fd/<n>` 重开不可提权（A8 方向位拒绝）、主动撤销 + KILL_ON_EXPIRE（SIGKILL 工作进程）、SCM_RIGHTS 接收经调解可用（A6）、SCM_RIGHTS 接收类外丢弃、SCM_RIGHTS 发送传播检查（propagation_types=0 → EPERM）、pidfd_getfd 窃取按类裁决（SOCKET 拒 EPERM / FILE 准且可读，A7）、shmat MEMORY 类检查（A5）。串口日志须出现全部 `ENVELOPE_SMOKE: <场景> PASS` 与总 `ENVELOPE_SMOKE: PASS`。套件末尾另通过 syscall 906 执行 E8 运行时不变式审计（TypeAllowed/RightsSubCap/预算界/挂载一致性全量走查），要求零违例。

失败时查看 `.kernel-build/smoke/envelope-riscv64.log` 中首个 FAIL 场景，对照 `kernel/ipc/envelope.c` 的 `ENVELOPE_MEDIATION_CONTRACT` 与 docs/research/05 §2.5 的逃逸面语义。

### E2 试点矩阵（研究门禁）

`make smoke-envelope-pilot` 在 QEMU 中运行 `envelope_pilot`，五场景 × 四臂（无限制 / Landlock-permissive / Landlock-strict / ENVELOPE）共 20 个单元，验证论文必要性论证：粗粒度机制的两难被量化（permissive 放行全部攻击、strict 连良性安装一起拒绝），信封在"良性可用 × 攻击阻断"两维同时成立。场景定义与预期表见 docs/research/10-evaluation.md §4.1。

失败时查看 `.kernel-build/smoke/envelope-pilot-riscv64.log` 中首个 FAIL 单元（格式 `<场景>/<臂> FAIL rc=N`），对照 10 §4.1 的预期结果表与 `kernel/ipc/envelope.c` 的调解语义。

### 咽喉完备性覆盖（研究门禁）

`make check-envelope-coverage` 从 `syscall_table.def` 机械再生成信封覆盖矩阵（docs/research/verification/envelope_coverage.md）并与已提交版本比对。366 个登记入口必须逐一显式分类：ACQUIRE / TRANSFER / USE / FAILCLOSED / PLANNED-W2 / NA。新登记 syscall 未分类即失败，杜绝静默的咽喉缺口。当前计数：30 已调解 / 36 PLANNED-W2 / 300 NA。

失败时运行 `python3 tools/gen_envelope_coverage.py` 后 diff 该文件，为未分类的新入口补上类目（引用 05 §2.5.1 的进入点编号），重新提交。

### 信封开销微基准（研究门禁）

`make smoke-envelope-bench` 在 QEMU 中运行 `envelope_bench`，每操作族在无限制（off）与信封内（on）各测一轮（CLOCK_MONOTONIC 单点计时，ITERS=20000/族）：open+close（A1 获取调解）、read-64B / write-64B（方向位 R/W + ops/data 计费）、lseek（未调解对照组）。串口日志须出现全部 `ENVELOPE_BENCH:` 行与总 `ENVELOPE_BENCH: PASS`。

失败时查看 `.kernel-build/smoke/envelope-bench-riscv64.log` 中异常大的 Δ 或 off/on 数值倒挂，对照 docs/research/10-evaluation.md §5.5 的实测基线与 `user/cmds/core/envelope_bench.c` 的预算设定。

### 信号、停止与退出

`make check-signal-exit-boundary` 检查 Park mode 与普通/致命/退出唤醒原因的映射、`signal_state.lock` 所有权、`STOPPED` 的显式恢复路径和远程退出安全边界；禁止信号或退出路径通过 `proc_make_ready()` 绕过 token；确认 `proc_stress` 覆盖停止态隔离、`SIGCONT`、停止态 `SIGKILL`、`sigsuspend` 交接和 eventfd 信号中断。完整步骤五本地矩阵运行 `make check-proc-step5-local`。

失败时先运行 `make PYTHON='conda run --no-capture-output -n a20os python' NETDEV_USER='-netdev user,id=net' smoke-proc-stress` 并查看 `.kernel-build/smoke/proc-stress-riscv64.log`，再检查 `kernel/proc/{signal,park,sched,exit}.c` 的锁顺序与唤醒原因。

### Timeout heap 所有权

`make check-timeout-ownership-boundary` 确认 heap entry 保存 deadline、task 引用和 `wait_seq`，cancel/expiry 唯一摘除，容量满与重复注册显式失败，旧 timeout 不能唤醒后续 token；压力测试覆盖 capacity-1、capacity、capacity+1。双架构容量和竞态矩阵使用 `make check-proc-step6-local`。

失败时检查 register 失败是否完整回滚 Park prepare，cancel 和 expiry 是否都在 `g_wait_timer_lock` 下先摘除再释放引用，以及 expiry 是否先释放 heap 锁、再在目标 `park_lock` 下按 `wait_seq` 唤醒。

### SMP runqueue、迁移与抢占

`make check-smp-runqueue-boundary` 确认迁移按 CPU 编号升序获取源/目标队列锁，`cpu_id` 只在 off-rq 区间改变，per-CPU `need_resched` 是持久状态，IPI handler 只确认通知、调度请求由公共安全点消费；压力测试观察迁移、优先级抢占和 IPI send/ack/consume。双架构 1 核/8 核矩阵使用 `make check-proc-step7-local`。

失败时先检查任务是否同时出现在两个 runqueue，或是否同时设置 `on_rq`、`dispatching`、`on_cpu`，再检查远程 enqueue 是否先发布队列状态、后设置请求并发送 IPI。

### 本地 pick 锁拆分

`make check-process-lock-split-boundary` 确认 `proc_runq_pick_local()` 内不获取 `proc_lock`，并在本地 runqueue 锁下原子完成 `on_rq -> dispatching` 与引用转交；调用者释放 runqueue 锁后才获取 `proc_lock`；运行时统计能观察并行 pick 与锁争用。完整累计矩阵使用 `make check-proc-step8-local`。

失败时不要恢复旧的全局 pick 锁。要查 picker 是否在队列锁内调用需要 `proc_lock` 的 helper，或 unpick/switch completion 是否丢失 dispatch 引用。

### Proc/Sched 累计矩阵

`make check-proc-step8-local` 依次包含 task state、引用生命周期、阻塞点、信号/退出、 timeout、SMP runqueue 和本地 pick 门禁，并在 RISC-V64/LoongArch64 的 debug/release、1 核/8 核组合中运行 scheduler、futex、process、I/O、 VFS 和 socket 压力测试；完整聚合矩阵为 `make check-proc-step8`。

失败时从失败日志中的首个 invariant、引用计数或 lock warning 开始定位；后续 timeout 往往只是首个所有权错误的结果。

### Native ABI 测试

`make native-handle-test` 与 `make native-libc` 只构建对应原生程序，检查编译和链接，目标名不表示执行；`native-libc` 构建 liba20c 测试程序，`user/tests/test_liba20c.c` 由 `native-libc` 编译。QEMU 运行时覆盖只有 `make smoke-native-handle` 一条：`smoke-native-handle` 启动 `/bin/native-handle-rv` 并验证正常关机。

失败时检查 `user/liba20rt/` 与 `user/liba20c/` 的编译错误，确认 `native-handle-rv` 已生成并放入 fat32 镜像，并查看 `.kernel-build/smoke/native-handle-riscv64.log`。

#### Native 门禁的架构覆盖现状（2026-10 核实）

这一段记的是**现状**而不是计划；新增 native 门禁前先读它，免得把“多跑了一条 riscv64”当成“架构覆盖已经有了”。

- 运行时（QEMU）：`tools/smoke_cases.py` 共 87 条用例，其中 21 条名字带 `native`，**全部**写死 `ARCH=riscv64` + `ABI=both`（`'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0']}`，`qemu: qemu-system-riscv64`）；这一组配置在用例表里出现 32 次，另外 11 条非 native 用例也用它，所以复用 both-ABI dev-build 的收益比 21 条更大。没有 x86_64 / aarch64 / loongarch64 的 native smoke，用例表里也没有让 smoke 换架构的参数。Native 运行时因此只有一个架构，且 mlibc 的 Native sysdeps（`user/external/mlibc/sysdeps/a20/`）也只支持 riscv64。
- CI（核实于本文件这次更新）：在把 `make smoke-native-contract` 加进 `.github/workflows/ci.yml` 的 `smoke` job 之前，`smoke-native` 在该文件里出现 **0** 次——21 条 native 运行时门禁没有一条在 CI 跑过，本地绿与主干红之间没有任何 native 差异。现在跑的是 `smoke-native-contract`（timeout 20s，`pre` 无附加镜像，复用 `smoke` job 已有的 riscv64 both-ABI dev-build 产物），断言 rights algebra / BPF / EventQ / VMOL / DMA 五项加正常关机；其余 20 条仍未进 CI。
- **这条门禁在 `main` 上是红的，且与双 ABI 守卫改动无关。** 核实于 2026-10-06：`user/tests/test_native_contract.c:466` 的 `vmol-leak-vmo` 断言失败（`a20_vm_unmap` 之后 `objstat` 的 `vmos` 计数没回到基线），`smoke.py` 随后报 `missing ['vmol ok', 'dma ok']` 并以 2 退出。归因做过两次对照：把本次改动的 `kernel/proc/sched.c` 与 `kernel/proc/timer_heap.c` 还原到 `HEAD` 重新构建后失败点完全相同；再在 `git archive HEAD` 出的干净副本上冷构建并跑同一条门禁，失败点仍然完全相同。所以这是主干既有的 VMO 引用计数缺陷，先于本节记录的所有改动。CI 接线本身是对的（它第一次让这条缺陷可见），但要让它转绿得先修 `mm/` 里的 VMO 释放路径，那是本门禁之外的工作。在修好之前，CI 的 `smoke` job 会因为这一行而红。
- 编译（交叉编译，不起 QEMU）：`tools/targets-native*.mk` 里有 24 个 `native-<prog>-arch` 目标，其中 10 个家族有 `-all` 聚合目标（如 `native-contract-all`），每个覆盖 riscv64 / loongarch64 / aarch64 / x86_64 / arm32 / riscv32 / ppc64le 七个架构。CI 的 `build` 矩阵只跑 `dev-build`，不调用这些目标，所以**非 riscv64 的 native 用户态能否编译成功在 CI 里同样没有任何断言**。
- 源码侧：`check-native-abi-coverage` 与 `check-abi-config-guard` 覆盖全部架构共用的那半边（Native ABI 的登记表/编号表/文档对齐，以及 `CONFIG_ABI_*` 守卫写法），见上面两节。这两条不能替代任何架构的运行门禁。

扩到第二个架构需要先让 `tools/smoke_cases.py` 的 `argv`/`build.vars` 可按架构参数化（例如 `qemu-system-$(ARCH)` 与 `AX=ARCH=…`），再在 `smoke` job 里开一个架构矩阵；只把某条用例复制成 riscv64/x86_64 两份会让同一份断言在两处漂移。

### 用户态文件系统宿主（uxfs + ufsd）

`make smoke-native-fs-all` 跑四后端端到端，`make smoke-native-ufs` 只跑 FAT 后端回归。`smoke-native-fs-all` 在 QEMU 中挂四块 scratch 盘（bus.2/4/6/7），由 `/bin/ufs_all_test` 逐后端拉起 `/bin/ufsd-rv` 并执行 POSIX 序列：FAT 预置读回+写读+删除；ext4 预置读回+8 KiB 图案写读+rename+删除；iso9660 小写名嵌套读取；ntfs 只读语义（create 必须失败）。内核侧 uxfs 代理把 vnode ops 经 Channel 转发给服务，块 IO 走受控 fs_block_io。见 [hybrid-kernel/06-user-fs.md](hybrid-kernel/06-user-fs.md)。

失败时查看 `.kernel-build/smoke/native-fs-all-riscv64.log` 中各 `UXFS_*` 标记与 `[fs]` 前缀的 FS 内部日志，确认镜像目标（`ufs-scratch.img`/`ufs-ext4.img`/`ufs-iso.img`/`ufs-ntfs.img`)已生成且盘位未占用 bus.3/bus.5（用户驱动预留）。

### SIOCGIFCONF 与 per-interface getter（Linux ABI sockets 区域）

`make smoke-net-iface` 覆盖 `user/cmds/net/net_iface_test.c` 二十项，同时被 `smoke-network-suite` 覆盖，因为 `net_iface_test` 已列入 `network_suite` 的用例表。检查项包括 SIOCGIFCONF 的空 `ifc_buf` 尺寸查询（必须是整条目数，估算值会让调用方反复扩容）、按 `struct ifreq` 步长填充、行数与查询值一致、接口名 NUL 终止、每行是 `AF_INET` sockaddr、不足一个条目的缓冲返回 0 且不写入、单条目缓冲不溢出且只返回整条目、非 IPv4 族请求返回空列表、负 `ifc_len` 返回 EINVAL；再用枚举出的名字回灌 SIOCGIFADDR / SIOCGIFFLAGS，并确认未知接口名被拒。

它必须存在，是因为 getifaddrs()、ifconfig 与 busybox `ip` 全部建立在 SIOCGIFCONF 上，而 per-interface getter 在没有枚举手段之前不可达。SIOCGIFCONF 此前是"派发但未实现"（-ENOTTY），该区域没有任何运行门禁，因此下面两个真实缺陷是写这个门禁时才暴露的。

失败时查看 `.kernel-build/smoke/smoke-net-iface.log` 中首个 `NET_IFACE: FAIL` 行，对照 `kernel/net/socket_file.c` 的 `struct a20_ifreq` 与 `net_ifreq_fill` / `net_ifreq_put_addr`。`info` 行会打印用户态 `sizeof(struct ifreq)`、`offsetof(ifr_ifru)` 与每个接口的地址，ABI 不匹配时这三行即可定位。

### TCP accept 路径与 listener 存在性（Linux ABI sockets 区域）

`make smoke-net-accept` 覆盖 accept 路径在**两种 TCP 模式**下的一致性，这是
`a20.tcpmode` 分档必须成立的可观测契约。`fast` 档由 socket 层配对两个 socket 来匹配
listener；`lwip` 档把已绑定的 PCB 转成真正的 lwIP LISTEN pcb，由协议栈完成握手。
两档是**两套独立实现**，必须给出相同的可观测结果，所以门禁把 `tcp_accept_test`
在两档各跑一次。

`tcp_accept_test` 只断言"握手完成且 `accept()` 返回可用 fd"，**故意不覆盖数据传输**：
两者可分离，而红门禁必须指向真正坏掉的那一处。所有阻塞步骤都用 `SO_RCVTIMEO` 兜住，
因为 `connect()` 的内核超时是 10 s，会超出任何门禁的合理预算。

承重的断言是 `tcp_listen > 0`。它是本条缺陷的回归护栏：在补上 LISTEN pcb 之前它是稳态
0，入站 SYN 被回 RST。已验证该断言可失败——同一内核以 `a20.tcpmode=fast` 启动时
`tcp_listen=0`、门禁转红，以 `a20.tcpmode=lwip` 启动时为 1。模式经**内核命令行**选择
而非 `/proc/net/config` 写入口，因为服务器第一个 listener 通常由用户态开机创建，shell
写入口来不及生效。

三个按零断言的计数器是不变量而非统计量：`net_accept_drop`、`net_bh_overflow`、
`net_alloc_fail` 任一非零，都表示 accept 或收包路径丢弃了它已经接受的数据，这在任何量级
下都是缺陷。accept 计数只记录不断言，因为一次运行里 accept 多少次取决于客户端重试时序。

它必须存在：修复之前 `tcp_listen()` 全树从未被调用，`/proc/net/status` 的 `tcp_listen=0`
是常态，协议栈没有任何对外服务能力（SLIRP hostfwd 实测返回"连接被对方重置"），而当时
**没有任何门禁跑网络区域**，所以这个状态可以一直不被发现。

失败时查看 `.kernel-build/smoke/net-accept-riscv64.log`，对照
`kernel/net/socket_control.c` 的 `net_listen()`（两档分派）与
`kernel/net/socket_inet.c` 的 `net_inet_tcp_listen()` / `lwip_tcp_accept_cb()` /
`net_inet_accept_stage_drain()`。门禁会在失败时打印 `passes` 与四个计数器值。

### network_suite 的判定语义：declared-absent

`network_suite` 的子测试退出码契约里新增了一个判定：**declared-absent**（`may_skip`
是"本次运行的环境没有覆盖该能力"，属于**运行**的事实；`known_absent` 是"项目已决定
不做该能力"，属于**树**的事实，门禁本该把它钉住）。

2026-10-08 本地 RV64 复验：`make -j8 smoke-lwip-memp`、`make -j8
smoke-net-lanes`、`make -j8 smoke-net-lanes-n1` 与 `make -j8
smoke-network-suite` 顺序通过。4-lane stress 完成 4 轮、每轮 1 MiB 的四路传输，
报告 `lanes: count=4 sockets=1 occupancy: 0 0 1 0`；`/proc/a20/netmem` 的 lane
分配/释放与 RX/drop 行均有实测计数，staged RX 为 0。N1 对照中 1-lane 与
4-lane 的 `NET_STRESS_TEST: PASS` 行逐字相同。完整 suite 报告 11 passed、1
declared-absent（`alg_test`）；对应 `.kernel-build/smoke/*-riscv64.log` 无
panic。该记录只描述 2026-10-08 本地运行，不代表远端 CI 状态。

AF_ALG 是后者的典型：它刻意不提供任何算法（见 `kernel/net/socket_alg.c`），`bind()` 必然
失败，`alg_test` 如实返回 78（ABSENT）。此前套件把 ABSENT 一律当失败，于是
`smoke-network-suite` 在 `main` 上就已经是红的——网络栈无论怎么改都不可能让它变绿，
一个永远红的门禁等于没有门禁。改为：`known_absent` 的 ABSENT 结果是**被断言的状态**，
按名字报告并单独计数，不计入退出码。

fail-closed 在两个方向都保留：未声明 `known_absent` 的测试一旦 ABSENT 仍然让套件失败；
已声明的测试若开始通过（能力落地了），报告 `PRESENT(expected-absent)` 并提示删除
过期声明，而不是静默腐烂。

### poll / timer 边界语义（Linux ABI poll 与 timer 区域）

`make smoke-poll-edge`、`make smoke-timer-edge` 覆盖两组边界语义。`poll_edge.c` 十二组：poll 超时边界与 POLLNVAL/HUP/ERR、select 语义与结果集剪枝、ppoll 超时与 sigsetsize/负时间 EINVAL、epoll 参数校验、嵌套 epoll 的 ELOOP 环路拒绝、dup 共享 interest list、ET/LT、EPOLLONESHOT、HUP+数据、epoll_pwait2 亚毫秒向上取整、eventfd 计数/信号量/溢出边沿。`timer_edge.c` 十二组：timer_create 的 clockid 与 sigev 校验、settime/gettime 剩余时间、TIMER_ABSTIME、overrun 计数、delete 后不再投递、timerfd 基础语义、TFD_TIMER_CANCEL_ON_SET、itimer REAL/VIRTUAL/PROF 实际投递、clock_nanosleep 相对与 ABSTIME、EINTR + remaining。这两组是把覆盖表 poll/timer 区域提升到 `full` 的依据（`nanosleep` 的 restart 语义仍是记录在案的 partial）。

失败时查看 `.kernel-build/smoke/poll-edge-riscv64.log` / `timer-edge-riscv64.log` 中首个 `POLL_EDGE: FAIL` / `TIMER_EDGE: FAIL` 组名，对照 `kernel/abi/linux/sys_epoll.c`、`kernel/abi/linux/{sys_timer_posix,sys_time,sys_fs}.c`、`kernel/proc/timer_posix.c`、`kernel/ipc/timerfd.c`、`kernel/proc/timer_heap.c`。

### mount namespace（unshare/setns CLONE_NEWNS）

`make smoke-mntns` 用 `mntns_test.c` 覆盖 init 命名空间 ino 非零、`/proc/self/ns/{pid,net}` 渲染、fork 共享挂载命名空间、`unshare(CLONE_NEWPID)` 生成不同 pid 命名空间、`unshare(CLONE_NEWNET)` 如实返回 EINVAL（不假成功；`CLONE_NEWUSER` 已实现，见下一节）、`unshare(CLONE_NEWNS)` 生成不同 ino 且其挂载对父进程不可见、setns 经 `/proc/<pid>/ns/mnt` fd 加入（目标先退出仍可加入）、非 mnt 目标 EINVAL。

失败时查看 `.kernel-build/smoke/mntns-riscv64.log` 中首个 `MNTNS_TEST: FAIL` 行（含行号与 errno），对照 `kernel/fs/vfs/mntns.c`、`kernel/abi/linux/sys_namespace.c` 与 `kernel/fs/procfs/procfs.c` 的 ns 渲染。

### user namespace（unshare/setns CLONE_NEWUSER）

`make smoke-userns` 用 `userns_test.c` 覆盖：初始命名空间 ino 非零且
`/proc/<pid>/ns/user` 渲染、`unshare(CLONE_NEWUSER)` 产生不同 ino、
未写映射时进程看到 `USERNS_OVERFLOW_UID`（65534）而非宿主 uid、fork 与线程
继承调用者的命名空间而只有 `clone(CLONE_NEWUSER)` 才新建、父命名空间写
`/proc/<pid>/uid_map` 后翻译立即生效、畸形与越界映射（两字段行、负数、
跑到 id 空间末尾、零长度、重叠）各自以 EINVAL 拒绝且不留残迹、
合法分段可追加、`setgroups` 是单向开关（`deny` 无特权且可重复，
`deny` 之后的 `allow` 即使满权限也 EPERM，畸形关键字 EINVAL）、以及
rootless 全链路：uid 1000 自行建命名空间、写入"映射自己的 id"这一条
唯一无特权路径后成为命名空间内的 0，而任何更宽的映射都是 EPERM、
并且无法 `setns` 回初始命名空间。

失败时查看 `.kernel-build/smoke/userns-riscv64.log` 中首个
`USERNS_TEST: FAIL` 行（含行号与 errno），对照 `kernel/proc/userns.c`、
`kernel/fs/procfs/procfs.c` 的映射写入分支与 `userns_capable()`。

门禁可证伪：把 `userns_capable()` 改成"沿 parent 链向下查找、命中即授予"，
`smoke-userns` 会以 "an unprivileged second extent mapping a foreign id was
accepted" 失败——那正是任何用户借 `unshare -U` 拿到宿主 root 的路径。
把 `map_from_global()` 改回按 `lower` 查找，门禁会以 "uid after installing
its own map is 65534" 失败。

### USB hub（class 9 与下行总线）

`make smoke-usb-hub-x86_64` 在 q35 上挂 `qemu-xhci` + `usb-hub` + 键盘 + 鼠标，
断言 hub 作为 class-9 设备被枚举、`[USB-HUB] hub 0409:55aa: downstream ports=N
status_bytes=... ss=...`（hub 描述符按它真正的请求码 bmRequestType=0xA0 取回，
位图长度由 bNbrPorts 推出）、状态变更中断端点经父控制器配好并 arm、下行总线以
N 个端口注册进 `usb_core`，且根端口上的 HID 设备不受影响。

**为什么 hub 描述符用 0xA0 而不是标准 GET_DESCRIPTOR**：hub 描述符是类请求，
不是标准请求。用 `USB_TYPE_STANDARD` 去问，任何真 hub 都会 stall。门禁对此可证伪：
把 `usb_hub_probe()` 里那次 `usb_control_msg()` 改回
`USB_TYPE_STANDARD | USB_RECIP_DEVICE` 且 `wValue = USB_DT_HUB << 8`，
`smoke-usb-hub-x86_64` 会以 `[USB-HUB] hub descriptor read failed: -110` 失败。
把 `HUB_STATUS_BYTES()` 的 `+1`（hub 自身状态那一个字节）去掉，门禁会以缺失
`status_bytes=3` 失败。

**门禁覆盖不到的部分**：QEMU 9 起 `usb-hub` 不再创建下行 bus
（`-device usb-kbd,bus=hub0.0` 报 "Bus 'hub0.0' not found"），QEMU 也无法实现
hub 的端口复位（`SET_FEATURE(PORT_RESET)` 无响应），所以没有任何设备能被放到
hub 后面，它的端口位图还会在最后两个端口上报幻影连接。因此本门禁证明的是
"hub 被正确识别、描述符被正确解析、下行总线被正确注册"，**不**证明"hub 后面的
设备被枚举"。后者只能靠真机验证。

### xHCI 中断路径（per-controller IRQ）

`make smoke-usb-x86_64` 在 q35 上挂 `qemu-xhci` + `usb-kbd` + `usb-mouse`，
断言键鼠枚举（原有两条），外加三条只有"完成由 controller 自己的 INTx 驱动"
才成立的行：

```
[XHCI] controller ready: MMIO=0x… slots=… ports=… irq=… completion=interrupt
[XHCI] completions: irq=1 poll=1
[USB-HID] key event: code=30 value=1
```

forbid 侧加了 `[XHCI] controller ready: .*completion=polling`：中断没申领上时
驱动会诚实地退回轮询，枚举照样成功，所以必须显式禁止这一行，否则门禁会在
中断路径整个没跑的情况下变绿。

**键值是怎么进去的**：`usb-kbd` 只在报告**变化**时才排一次传输，静止的 guest
什么都不会完成，中断计数也就永远是 0，断言会变成空转。所以门禁给 QEMU 加一条
私有 QMP socket（`-qmp unix:…`），`tools/smoke.py:qmp_key_pump()` 在整个窗口内
反复按下/松开 qcode `a`（Linux keycode 30）。按 cadence 而不是只按一次，是因为
guest 在枚举完成前没有为键盘配 interrupt endpoint，开机早期那一次按键什么都证明不了。

**为什么走 QMP 而不是串口上的 monitor**：`-nographic` 把 monitor mux 到同一条
tty，HMP `sendkey` 经 `qemu_input_find_handler()` 投递，它返回第一个 console-less
且匹配事件掩码的 handler。q35 上 PS/2 键盘先注册（`hw/input/ps2.c`）且从不调用
`qemu_input_handler_activate()`；QEMU 的 `usb-kbd` 会调用（`hw/input/hid.c`），
而 `activate()` 做的是 `QTAILQ_INSERT_HEAD`，于是 USB 键盘排在 PS/2 前面。
所以 `input-send-event` 落在 USB 键盘上，能证明 xHCI 这条路；`sendkey` 只会打到
PS/2，证明不了任何 USB 的事。

**门禁覆盖不到的部分**：机器是 `-smp 1`，SMP/多核压力未验证；`a20.xhci.poll=1`
的回退路径本门禁没跑（需要另一个 case 或手动加 cmdline）；真机 xHCI 的 MSI/MSI-X
也没覆盖（本驱动只用 INTx）。

### MSI-X 消息中断

`make smoke-msix-x86_64` 在 q35 上同时挂一个 `virtio-blk-pci` 和一块
`e1000e`，断言两者的能力都被解析出表位置（virtio 在 BAR1、e1000e 在 BAR3，
两者都报 `Message Address Low` 来源，因为 `-kernel` 引导没有固件写 Vector
Control）、向量被预留并 arm、virtio-blk 改用 MSI-X 而让出 INTx，最后断言
**`[VIRTIO-BLK] MSI-X delivery on vector 208`**。

这一行由中断处理程序在第一次消息中断完成时打印。表项编程正确不等于消息被
投递：能力解析、向量号、mask 状态、设备侧的 notify 路径都对，而消息地址错
了（把向量 OR 进 LAPIC 页基址），设备照样 notify 就是没有中断，只有这行能
区分。

**同一次运行还验亲和性**：这台机器是 `-smp 2`（构建变量多一个 `NR_CPUS=2`，
产物目录 `x86_64-qemu-virt-x86_64-both-dev-smp2`）。shell 起来后脚本按顺序

```
cat /proc/a20/irq_affinity      # 打印 cpus: 2 和每个已编程条目 name/index/vector/cpu
echo 1 > /proc/a20/irq_affinity # 把全部条目迁到 CPU1
ls /bin                          # 真实块 I/O，制造迁移后的完成中断
cat /proc/a20/irq_affinity      # 同一批条目现在以 \t1 结尾
```

再断言三条新增模式：`[MSI-X] affinity: N vector(s) now target cpu 1`、
`[VIRTIO-BLK] MSI-X delivery on vector 208 cpu=1`（处理程序打印取走中断的
CPU，同一向量迁移后会重新打印一次），以及读回行 `pci-1af4:1001-…\t0\t208\t1`。
原来七条断言**一条没删**。forbid 侧加了三条失败模式：远端 LVT 没能 arm
（`controller entry could not be armed`）、某条目拒绝目标 CPU
（`entry N refused cpu 1`）、IPI 握手超时（`[X86_64 MSI-X] … timed out`）。

迁移必须发生在运行时而不是 probe：写节点触发的是 mask → 改消息地址 → 经 IPI
在远端 arm LVT → unmask 这一整条路径，折进 probe 的话本门禁已经证明过的
boot-CPU 投递就没法再单独观察了。第一次 `cat` 也是可证伪的一半——节点不存在时
写就是 shell 错误，第二条读回无论内容如何都不会满足。

**门禁可证伪**：把 `arch_msix_message_address()` 里的 `LAPIC_PHYS_BASE` 改回
`LAPIC_PHYS_BASE | (vector & 0xFF)`，`smoke-msix-x86_64` 会**只**缺
`MSI-X delivery on vector 208` 这一条而失败——其余七条断言照常通过，因为表项、
向量和 mask 都还是对的。把 capability 的解析改回只读 Vector Control，门禁会以
缺 `MSI-X enabled` 失败；把 `queue_msix_vector` 写成向量号（208）而不是表
索引（0），同样只缺这一条。把 `arch_msix_vector_setup()` 的远端分支去掉（直接
`return -EOPNOTSUPP`），迁移会被拒绝，`affinity: … now target cpu 1` 与
`cpu=1` 那条都不出现，且 forbid 里的 `entry N refused cpu 1` 命中——前七条仍
照常通过。

**门禁覆盖不到的部分**：只有 x86_64 有消息中断路径，其他架构的
`arch_msix_message_address()` 返回失败、`arch_irq_msix_cpu_count()` 返回 1，
MSI-X 那段代码在这些板上只被验证到"干净地拒绝"为止，没有真实投递。e1000e 只
验证到表被正确解析并 arm，网卡本身
不会收到流量，所以它的两个向量同样没有真实投递；virtio-blk 的那一路才是端到端
的。亲和性侧只覆盖"全局一个 CPU id"，没有 per-function / per-queue 的细粒度接口，
也没有 cmdline 亲和性策略；CPU1 上的远端 LVT 由 IPI 写，但该 CPU 上是否真的
长期均衡分配中断，未验证。

上面所有断言都已由 `make smoke-msix-x86_64` 实跑验证：门禁 PASS，日志留在
`.kernel-build/smoke/msix-x86_64.log`。它证明的仍只是 x86_64 QEMU 上的向量
编程与投递行，不是真实硬件。

### VirtIO-SCSI 完成中断（x86_64）

`make smoke-virtio-scsi-irq` 在 q35 上挂 `-device virtio-scsi-pci` + 一块
`scsi-hd`（64 MiB 稀疏 raw 盘，挂在 `/dev/sdX`），启动后运行用户态
`virtio_scsi_test`。断言四层：

1. probe 打印的 `completion=` 必须是 `msix` 或 `intx`，**不能**是 `polling`
   （forbid 掉 `[VIRTIO-SCSI] ... completion polling`）；
2. 测试用 `A20_BLK_IOCTL_GET_STATS` 探 `/dev/disk0..7`，只有 virtio-scsi
   实现这个 ioctl，因此设备槽位顺序不影响门禁；它断言
   `irq_mode ∈ {INTX, MSIX}`、`timeouts == 0`；
3. 三轮回环：填图案 → 写盘尾 64 个扇区 → `ioctl(A20_BLK_IOCTL_SYNC)`
   （SYNCHRONIZE CACHE(10)，cdb 里 LBNUM 与 count 全 0 = 整盘）→ 读回
   `memcmp` → 再填再写 → flush → 读回 `memcmp`；每轮前后各取一次计数，
   断言 `commands`/`flushes` 增长、`timeouts == 0`，且
   `irq_count` 严格增长（`(+N)`，N ≥ 1）；
4. `VIRTIO_SCSI_TEST: PASS`。

**为什么用类 ioctl 而不是 `/proc/a20/perf`**：perf 是全局计数器，一台机器上
同时有 virtio-blk 根盘和其他设备时，"IRQ 计数 > 0"无法归属到被测的 virtio-scsi
控制器。`A20_BLK_IOCTL_GET_STATS`（`kernel/include/uapi/a20/block.h`）返回**该设备**
的 `commands/flushes/timeouts/irq_count/irq_completions/spin_completions/
irq_mode/irq_line`，归属是明确的。

**门禁可证伪**：把 `virtio_scsi_command` 里的 park 前重查改成无条件 park（丢掉
"完成先于入队"的重查），`irq_count` 仍会增长而读回 `memcmp` 会开始失败；
把 `virtio_scsi_irq_handler` 换成 virtio-blk 那种"`isr == 0` 就返回"的写法，
走 MSI-X 时 park 的等待者永远收不到唤醒，超时计数与 `PASS` 会一起消失。

**门禁覆盖不到的部分**：只跑 x86_64 QEMU，INTx/MSI-X 的实际投递只在 q35 +
x86 LAPIC 上验证过；riscv64/aarch64/loongarch64/ppc64le/aarch32 的 virtio-scsi
中断路径**未验证**（VirtualBox ARM 那条路径仍以轮询为主）。混合完成窗口意味着
**不能**断言 `irq_completions > 0`：`VIRTIO_SCSI_HYBRID_PRE_POLL_US`（800 µs）
里的短命令本来就在自旋窗口完成，park 之后的 IRP 才计入 `irq_completions`，
所以门禁只断言 handler 被调用过（`irq_count`），不断言完成是被中断唤醒的。
门禁不覆盖：多 target / 多 LUN、READ(16)/WRITE(16)、非 512B 扇区、队列并发
（驱动每控制器仍只有一条 in-flight 命令）、`-kernel` 之外的固件启动路径。
`a20.virtio-scsi.poll=1` 的回退路径**未**被本门禁覆盖（要另跑一遍带该参数的
boot 才验证得到）。

失败时查看 `.kernel-build/smoke/virtio-scsi-irq-x86_64.log` 中首个
`VIRTIO_SCSI_TEST: FAIL` 与其后的 `[VIRTIO-SCSI]` 行，对照
`kernel/drivers/block/virtio_scsi.c` 与 `user/cmds/core/virtio_scsi_test.c`。

### RTL8139 端到端回环与接收中断（x86_64）

`make smoke-net-rtl8139` 在 q35 上挂 `-device rtl8139`（`-nic user` 的
`model=rtl8139`），用 `hostfwd=tcp:127.0.0.1:18093-10.0.2.15:18093` 把宿主
18093 转发到来客 18093，宿主侧跑 `tools/rtl8139_host_probe.py`，来客跑用户态
`tcp_accept_test 18093`。

**为什么要这个门禁**：此前树里 x86_64 只有 e1000，而 e1000 的 id 表里没有任何
Realtek ID，所以 `-nic user,model=rtl8139` 起来的那个 PCI function 没有任何驱动
认领，`DEV_CLASS_NET` 里根本没有网卡。这不是"网络不通"，是"栈从来没有过设备"，
从外面看不出来。驱动补上以后才轮到这个门禁。

断言分五层，每一条的失败原因不同：

1. **端到端往返**：`rtl8139_host_probe.py` 连上转发端口、发一个字节、要求它回到
   宿主（回来时源端口仍是 18093，这是 QEMU 转发规则保证的，所以回环证明的是双向
   都真走通了），并且日志里出现 `TCP_ACCEPT_TEST: PASS`。
2. **`mode=irq`**：`[RTL8139] ready:` 行必须存在且 `mode=irq`。第 1 条排除不了回退
   —— `.poll` 是从同一个 lwIP drain 里跑的，纯轮询的网卡照样搬得动字节。
3. **`rtl8139_irq_calls > 0` 且 `rtl8139_irq_rx > 0`**：handler 真的进去了，且看到
   了只有接收 ring 能产生的 cause。第 1、2 条都推不出这一条：第 2 条只说明"注册过
   handler"，只有它能抓到 INTx 路由/swizzle 写错的情况。计数在测试**之后**从
   `/proc/a20/perf` 读，与 `smoke-net-e1000-irq` 同一个理由：`a20_perf_format()`
   在首次读时才打开累计，所以那次 `cat` 本身就是第一次真实测量。
4. **`rtl8139_tx_reclaimed > 0` 且 `rtl8139_rx_drained > 0`**：收发两半的描述符
   确实退休了。与第 3 条分开，因为 TX ring 只有 4 个描述符深，一个帧根本看不出
   "回收从未发生"；`rx_drained` 用来区分"真的走了 ring"和"栈发现没东西可取"。
5. **日志里没有 panic / assertion failed / page fault**。

**门禁可证伪**：把 `rtl8139_irq_handler` 整个换成空函数，第 1 条照样通过（`.poll`
会驱动一切），第 3 条归零；把 `IMR` 的写去掉或写 0，第 3 条归零而第 1 条照样过；
把 `RTL8139_TXPOLL` 的 `0x60` 改成 datasheet 上的 `0x20`，第 1 条在真硅片上会挂、
在 QEMU 上不会（见下）。

**门禁覆盖不到的部分**：一台 RTL8139、QEMU user-mode 网络后端、TCG、单核、
INTx（这个型号没有 MSI-X）。寄存器图和 RX ring 几何是照着仓库内置的
`qemu-10.0.13+ds/hw/net/rtl8139.c` 写的，**没有在真实硅片上跑过**；同一个
`RCR[12:11]` 字段在 RTL8139C datasheet 上写的是「8K + 16K」，驱动按 QEMU 的语义
编程成 64 KiB，硅片上是否一致**未验证**。`TxPoll` 也有同样的分歧：datasheet 是
bit 5，QEMU 是 bit 6，驱动写 `0x60` 两边都满足，并在代码里写明了这个妥协。不覆盖：
多队列、接收合并、WOL、省电、速率报告、任何吞吐结论、RTL8139C 的增强寄存器。
`a20.rtl8139.poll=1` 的强制轮询分支**不**在本门禁里覆盖 —— 它是这条正向断言的
阴性对照，要另跑一遍带该参数的 boot 才验证得到。

失败时查看 `.kernel-build/smoke/net-rtl8139-x86_64.log` 中首个
`TCP_ACCEPT_TEST: FAIL`、`[RTL8139]` 行，以及测试后的
`cat /proc/a20/perf` 那两段，对照 `kernel/drivers/net/rtl8139.c`、
`tools/rtl8139_host_probe.py` 与 `user/cmds/net/tcp_accept_test.c`。

### virtio-console 双向回环（x86_64）

`make smoke-virtio-console` 在 q35 上挂 `-device virtio-serial-pci` +
`-device virtconsole,chardev=...`，chardev 是 `server=on,wait=off` 的 unix
socket；宿主侧跑 `tools/vport_host_probe.py`，来客跑用户态 `vport_test`。

**为什么是这个形状**：探针不会一上来就连 socket，而是先在串口日志里等
`VPORT_TEST: READY`（来客已经 open 成功 `/dev/vport0` 并断开控制台镜像），
再写 64 字节。这样两个半边都不需要猜来客的启动时间，QEMU 也不必在启动阶段
阻塞等待一个还没人建立的连接。

断言分两层，缺一不可：

1. **宿主 -> 来客**：来客逐字节校验从 `/dev/vport0` 读到的 64 字节（`A`..`Z` 循环）。
   只在宿主侧发完就宣布成功，会把「接收路径丢字节/写坏字节」这条最典型的故障放过。
2. **来客 -> 宿主**：同样的 64 字节写回同一设备，探针比对回显。只看来客 PASS
   则完全没覆盖发送半边——驱动把 used ring 游标或 notify 写错时，接收仍然正常。

**门禁可证伪**：把 `vport_setup_queue()` 的 TX queue 号从 1 改成 0，接收仍然通、
来客仍然打印 PASS，但探针等不到回显，第 2 条失败；把 rx 描述符的 `flags` 从
`WRITE` 改成 0，第 1 条先失败。

**门禁覆盖不到的部分（诚实边界）**：本条门禁已在 x86_64 QEMU 上实跑通过
（`smoke-virtio-console: PASS (64 bytes host->guest->/dev/vport0->host)`，日志
`.kernel-build/smoke/virtio-console-x86_64.log`），因此该环境下的双向回环与 INTx
送达是已验证的。它仍只覆盖 x86_64 QEMU、
单核、单个 port 0、无 MULTIPORT、无 MSI-X、无 termios 层；第二个 virtio-serial
函数（`-device virtio-serial-pci` 挂两个）会因单静态实例返回 `-EBUSY`，这属于
已知限制而非缺陷；riscv64/aarch64/loongarch64 上只做过单文件 `-fsyntax-only`
自检，运行时未验证。

失败时查看 `.kernel-build/smoke/virtio-console-x86_64.log` 中的 `VPORT_TEST:`
与 `[VPORT]` 行，对照 `kernel/drivers/char/virtio_console.c`、
`tools/vport_host_probe.py` 与 `user/cmds/core/vport_test.c`。

### virtio-rng 熵源（x86_64）

`make smoke-virtio-rng` 在 q35 上挂 `-device virtio-rng-pci`，来客跑用户态
`hwrng_test`。断言 `[VRNG] virtio-rng ready` 与 `HWRNG_TEST: PASS`，并禁止
`virtio-rng.*unresolved symbol` —— 后者专门盯 drvmod 白名单漏导出
`random_reseed` 的情况，那种情况下模块会被 loader 直接拒绝而不是加载后行为异常。

`hwrng_test` 读至少 256 B，并断言结果不是全 `0x00`、不是全 `0xff`、也不是单字节
重复。之所以要这三条而不是"读到了就算"：设备不应答时 read 会走 500 ms 有界等待
后返回 `-ETIMEDOUT`，一个只检查返回值的测试会把"读到零"和"读到熵"混为一谈。

**这条门禁证明的边界**：熵来自 QEMU 的宿主熵池，不是真实硬件 RNG；单静态实例；
riscv64 的 virtio-mmio 路径只做过 `-fsyntax-only`，运行时未验证。

已实跑 PASS，日志 `.kernel-build/smoke/virtio-rng-x86_64.log`
（`HWRNG_TEST: PASS bytes=256 first=0x87 second=0xa7 tries=1`）。

### AHCI 完整块设备中断路径（x86_64）

`make smoke-ahci-ich9` 在 q35 上把一个 ext4 镜像挂在 `-device ich9-ahci` +
`-device ide-hd,bus=ahci.0` 上，来客先读一次 `/proc/a20/perf`，跑
`fsync_durability_test`，再读一次。

**为什么要有这条**：q35 芯片组自带一个 AHCI 在 00:1f.2，挂上去的 ich9-ahci 落在
00:02.0，两者走不同的 INTx swizzle。只挂 virtio-blk 的门禁证明不了 AHCI 的
top-half 跑过 —— 而 AHCI 原先是纯轮询的，正因为轮询也能把盘驱动起来，从外面
看不出区别。

断言：`[AHCI] device on port` 恰好出现 1 次（挂两个就说明匹配过宽）、日志里有
`completion=irq`、`FSYNC_TEST: PASS`、`ahci_irq_completions > 0`、
`ahci_commands > 0`，**并且 `ahci_poll_completions == 0`**。最后这条是整条门禁的
关键：它要求每一条完成都经过中断 top-half，只要有一条走了轮询就失败。这正是把
两条路径分开计数的意义 —— 否则"盘能用"这个观察对中断路径是否工作不提供任何信息。

`ahci_irq_wakeups`、`ahci_park_rounds`、`ahci_errors` 只记录不断言，因为它们在
本机 QEMU 上没有稳定的期望值。

已实跑 PASS，日志 `.kernel-build/smoke/ahci-ich9-x86_64.log`

### PCI 枚举走 legacy 端口（i440fx，x86_64）

`make smoke-pci-i440fx` 在 `-machine pc` 上跑，挂 virtio-blk + e1000 + ich9-ahci。

**为什么要有这条**：q35 有 ECAM 窗口，所有其它 x86_64 门禁都经内存访问配置空间，
因此无法区分"配置空间路径能用"和"读了一块根本不存在的窗口"。i440fx 没有 ECAM，
配置空间只在 `0xCF8`/`0xCFC` 后面，而编译进去的 q35 地址上什么都没有。

这个区别值得一条门禁，因为**故障从外面看不见**：枚举不会响亮地失败，它会按槽位
各发布一个幽灵设备（`id=0000:0000`），没有一个匹配任何驱动，然后机器在很后面
才死于 "no init program found"，指向不了任何东西。当时有三处独立缺陷同时掩盖了
它：`pci_bus.c` 自己算 ECAM 地址直接读，绕过了 `pci_host.c` 里早就写好的 legacy
回退；幽灵过滤只认 `0xffff` 不认 `0x0000`；ECAM 探测也只找 `0xffffffff`。断言
真实设备列表才能抓住其中任何一处。

断言：日志有 `bridges walked`、i440FX 宿主桥 `8086:1237`（class `06:00:00`）、
virtio-blk `1af4:1001`、e1000 `8086:100e`、AHCI `8086:2922`，且
**幽灵设备数为 0**。按设备 ID 匹配而非槽位：i440fx 与 q35 把同一批设备放在不同
槽位上，按槽位断言会让门禁只对某一台机器成立。

**不覆盖**：i440fx 的 INTx 路由。其路由由固件写进 PIIX3 的 PIRQ link 寄存器，
这里也没有可读的 DSDT `_PRT`，所以这些设备留在轮询路径上 —— 这是正确结果，
门禁断言的就是它，而不是把"没有中断"当成失败。

已实跑 PASS，日志 `.kernel-build/smoke/pci-i440fx-x86_64.log`
（`FSYNC_TEST: PASS`，12 条命令全部经中断完成）。真实 SATA PHY 上仍无任何证据，
且只走 INTx、没有 MSI-X 路径。

### E1000 端到端回环与接收中断（x86_64）

`make smoke-net-e1000-irq` 用 `hostfwd=tcp:127.0.0.1:18091-10.0.2.15:18091`，
宿主侧跑 `tools/e1000_host_probe.py`，来客跑 `tcp_accept_test 18091 10.0.2.15`
（serve 模式，见 [TCP accept 路径](#tcp-accept-路径与-listener-存在性linux-abi-sockets-区域)）。

断言分四层：`[E1000] ready:` 行存在且 `completion=msix`；宿主往返成功且来客打印
`TCP_ACCEPT_TEST: PASS`；`e1000_irq_calls > 0` 且 `e1000_irq_rx > 0`；
`e1000_tx_reclaimed > 0` 且 `e1000_rx_drained > 0`。计数在测试**之后**读，
因为 `a20_perf_format()` 在首次读时才打开累计，那次 `cat` 本身就是第一次真实测量。

`_rx`/`_tx` 是 `_calls` 的子集，不能相加。`e1000_irq_empty` 只记录不断言。

已实跑 PASS，日志 `.kernel-build/smoke/net-e1000-irq-x86_64.log`。真实 82540EM
硅片上未验证；`a20.e1000.poll=1` 的强制轮询分支本身未被这条门禁覆盖。

### CMOS RTC 墙钟（x86_64）

`make smoke-rtc-cmos` 以 `-rtc base=utc` 启动（让宿主与来客的时钟可比），来客跑
`/bin/date -u +RTC_WALLCLOCK=...`。除日志断言外，还有个宿主侧 `post` 步骤：
`tools/check_rtc_wallclock.py --max-skew 300` 拿日志里的来客时间与宿主时间比对，
容差 300 s。

日志断言同时要求两行：`[TIME] wallclock: no RTC readable yet, seed from build
time`（`timekeeping_init()` 先用编译期种子起步）与 `[TIME] wallclock: hardware
RTC adopted, unix=1...`。要求第一行是因为替换**必须**发生在 seed 之后 ——
驱动是 Early DriverStore 模块，只有 `driver_manager_early_init()` 之后才能绑定，
所以看到两行才说明替换真的发生了，而不是压根没走到 CMOS。

已实跑 PASS，日志 `.kernel-build/smoke/rtc-cmos-x86_64.log`：来客采用
epoch 1791231484，与编译期种子 1791230264 不同。

### CMOS RTC 越界回退（x86_64）

`make smoke-rtc-cmos-fallback` 是同一次启动，但把 RTC 设成
`-rtc base=1960-06-15T12:00:00` —— 落在驱动接受的窗口之外。断言 `[CMOS-RTC] no
usable time` 与 `seed from build time`，并且**禁止** `[CMOS-RTC] wall clock:`
与 `hardware RTC adopted` 两行。

**这条门禁尚未执行过**，目前只有 `smoke-rtc-cmos` 有运行证据。它验的是驱动在读不到
可用时间时仍然完成绑定、不 panic、也不谎称采用了硬件时钟 —— 回退路径不能因为
CMOS 不可信就把机器卡在 probe 里。

### 致命信号 core dump

`make smoke-coredump` 让 `coredump_test.c` 的子进程 SIGSEGV，断言 core 文件的 ELF64/LSB magic、`e_type == ET_CORE`、program header 布局、每个 PT_LOAD 的 `filesz <= memsz`、存在 PT_NOTE 且含 prstatus/prpsinfo/fpregset 三类 note；再验证 `core_pattern` 读写往返、子进程 wait status 的 core 位，以及 `RLIMIT_CORE=0` 时不产生文件。

失败时查看 `.kernel-build/smoke/coredump-riscv64.log` 中首个 `COREDUMP_TEST: FAIL`；宿主侧可用 `readelf -a` 检查 `.kernel-build` 之外的 core 产物。已知边界：仅 64 位、不发 NT_FILE、非驻留页写零、VMA 快照上限 1024、`|pipe` 不支持。

### swap

`make smoke-swap` 让 `swap_test.c` 在 ramfs 上造 backing 文件，经 loop-control `LOOP_CTL_GET_FREE` + `LOOP_SET_FD` 绑定，再 `mkswap` → `swapon` → 断言 `sysinfo` totalswap>0 与 `/proc/swaps` 条目；重复 swapon 返回 EBUSY；触访匿名内存；`swapoff` 后 totalswap 归零。

失败时查看 `.kernel-build/smoke/swap-riscv64.log` 中首个 `SWAP_TEST: FAIL`，对照 `kernel/abi/linux/sys_swap.c`、`kernel/mm/swap.c` 与 `kernel/drivers/block/loop.c`。已知边界：门禁不驱动真实换出（1 GiB 冒烟无法现实触发），`swap_read_page` 缺页读回只有编译覆盖；`CONFIG_SWAP` 在 riscv32/loongarch32（无 swap PTE 编码）与 NOMMU 目标上被构建入口强制关闭。

### 用户态网络控制面（SIOCGIFCONF / /proc/net / NETLINK_ROUTE / MSG_OOB）

`make smoke-netctl` 用 `netctl.c` 做断言，结构体取 musl 自己的 `<net/if.h>`（而非私有副本），因此内核布局必须与用户态实际编译的布局一致。这正是兼容性契约本身：私有的一对自洽结构体什么都验证不了。

覆盖项包括 SIOCGIFCONF 的 size query（`ifc_len` 为 `sizeof(struct ifreq)` 的正整数倍）与 fill（字节数必须与 size query 承诺的一致）；每个枚举出的接口名必须能被 `SIOCGIFADDR` 反查，否则枚举对使用者毫无用处；`/proc/net/dev` 的接口行数不得少于枚举数；`/proc/net/config` 报告了网关时 `/proc/net/route` 必须有对应路由行；`AF_NETLINK`/`NETLINK_ROUTE` 的 `RTM_GETLINK` 与 `RTM_GETADDR` dump 必须返回对象、以 `NLMSG_DONE` 收尾，且每条 link 都带 `IFLA_IFNAME`（缺了它 `ip link` 无法显示接口名），并且有 link 就必须有 addr 对象，否则 `ip addr` 会显示无 IPv4 而 `SIOCGIFCONF` 却报告有地址，两者必须一致；`MSG_OOB` 在 send/recv 两侧都必须是 `EOPNOTSUPP`，本栈无 out-of-band 路径，静默当作普通数据返回比报错更糟；`MSG_NOSIGNAL` 不得被当作未知 flag 拒绝。

netlink 线格式结构体在测试内独立声明（本树 musl 不带 `<linux/netlink.h>`）。独立复述契约才能证明它测的是内核而不是内核的镜像。

失败时查看 `.kernel-build/smoke/netctl-riscv64.log` 中首个 `NETCTL: FAIL`，对照 `kernel/net/socket_file.c`（SIOCGIFCONF 与 `struct a20_ifreq` 线格式）、`kernel/net/socket_netlink.c`（`net_netlink_route_request` 与 `nlrt_snapshot`）、`kernel/net/lwip_stack.c`（`/proc/net` 渲染与 netif 命名）、`kernel/net/socket.c`（`net_msg_flags_check`）。已知边界：本栈无路由表，`/proc/net/route` 与 `RTM_GETROUTE` 只报告各 netif 的默认网关，`RTM_SET*` 与路由增删一律 `-EOPNOTSUPP`；loopback 不经过驱动收发路径，其计数寄存器读零是真实值而非统计缺失；`SIOCSIFMTU`/`SIOCSIFDSTADDR`/`SIOCSIFBRDADDR` 仍是已分发但未实现、返回 `-ENOTTY`，而不是伪造成功；`/proc/net/arp` 仍是空表头，因为 lwIP 没有暴露 ARP 表访问器。

### ext4 JBD2 崩溃一致性

`make smoke-ext4-journal ARCH=<arch>` 在 JBD2 提交序列的四个点上真的把机器停住，用**同一块镜像**重启，检查承诺过的写入没丢、没承诺的写入没回来，并在宿主机上用 `e2fsck -fn` 检查崩溃后的镜像和恢复后的镜像都干净。注入点通过 `echo <point> > /proc/a20/journal` 下发（`/proc/a20/journal` 的读操作会打印当前注入点与可选点列表），这样可以在崩溃发生前、提交序列进行到一半时才武装，命中的正是那个边界；`a20.journal_crash=<point>` 命令行参数是同一机制的启动时形式，仅供手工复现。四个点各自是一个不同的承诺边界，所以期望结果也不同：

| 崩溃点 | 停在什么位置 | 文件是否应存活 | 重放是否应发生 |
| --- | --- | --- | --- |
| `post-recover-flag` | `needs_recovery` 已置位，尚未写入任何日志 | 否 | 否 |
| `post-journal` | descriptor 与各数据块已写入，无 commit block | 否 | 否 |
| `post-commit` | commit block 已落盘 | 是 | 是 |
| `post-checkpoint` | 元数据已写回其本位 | 是 | 是（幂等） |

每个点除文件内容外还断言：注入的 panic 确实触发（否则门禁测的是没跑到的代码）、恢复启动能挂载、日志重放横幅 `replay complete` 当且仅当该点已承诺、恢复后镜像再次 e2fsck 干净。另有一条无崩溃对照（`clean`），确认在没有崩溃的情况下这条路径不产生任何多余重放。

门禁本体在 `tools/ext4_journal_gate.py`，不在 `tools/targets-smoke.mk` 里内联：一次运行是 8 次 TCG 启动加 4 次宿主 fsck，shell 写不出来。之所以要双盘，是因为 FAT32 镜像带 `/bin/init` 而被测文件系统是第二块盘（`mount_setup.c` 把它自动挂到 `/extra`，无需 bootarg）。`ARCH` 同时决定 QEMU machine 与构建目录，因此这是每个架构各自的一道门，而不是只证明 x86_64。

失败时先看 `.kernel-build/smoke/ext4-journal-<arch>.log`，里面每次启动一段、日志打印保留完整；对照 `kernel/fs/diskfs/ext4_journal.c` 的 `ext4_journal_commit`（提交顺序）、`jbd2_write_descriptor`（descriptor checksum 必须在 tag checksum 回填之后算）、`jbd2_data_checksum`（数据 checksum 只覆盖未改动的块镜像）与 `kernel/fs/block_cache.c` 的 hold 语义（`bcache_sync_common` 跳过被持有的页）。宿主侧可以直接 `e2fsck -fn` 那份崩溃镜像复现。门禁断言清单见 `tools/gates.toml` 的 `ext4-journal-crash-consistency`。

### 跨架构陷阱：同一份代码只在某些架构下坏

下面几条都不是逻辑 bug，而是「按 x86 写出来的假设在其他架构上不成立」。它们共同的特征是**在 x86_64 上完全看不出来**，所以每修一条都必须按架构各跑一遍 `smoke-ext4-journal`，不能只跑一个。

**1. `O_*` 常量不是 asm-generic。** Linux 把 `O_DIRECTORY`、`O_NOFOLLOW`、`O_DIRECT`、`O_LARGEFILE` 放在 `arch/<arch>/include/uapi/asm/fcntl.h` 里逐架构定义，三套布局互不相同：

| | `O_DIRECTORY` | `O_NOFOLLOW` | `O_DIRECT` | `O_LARGEFILE` |
| --- | --- | --- | --- | --- |
| asm-generic（x86/riscv/loongarch） | `0x10000` | `0x20000` | `0x4000` | `0x8000` |
| arm / arm64 | `0x4000` | `0x8000` | `0x10000` | `0x20000` |
| powerpc | `0x4000` | `0x8000` | `0x20000` | `0x10000` |

注意 powerpc 那一行的前两列与 arm 相同、第三第四列与 asm-generic 相同——**没有任何两套布局是同一个顺序**。最坑的是 ppc64le：asm-generic 的 `O_DIRECTORY`(`00200000`) 在 PowerPC 上其实是 `O_LARGEFILE`，而 musl 的 `open()` 每次调用都会带上 `O_LARGEFILE`。用 asm-generic 的值当 `O_DIRECTORY`，等于让内核把「打开目录」理解成「设置 largefile」，`openat()` 静默返回错误，表现为挂载点莫名其妙地不存在。`kernel/include/core/fcntl.h` 现在按 `CONFIG_PPC64LE` / `CONFIG_ARM32||CONFIG_ARMV7M||CONFIG_AARCH64` / 其余三分支取值。

**2. ppc64le 的 stack-protector guard 默认走 TLS。** GCC 在该目标上默认 `-mstack-protector-guard=tls`，即 `ld 9,-28688(r13)`；而内核的 `__stack_chk_guard` 是一个普通 `.data` 全局量，r13 又已经被 `arch_set_task_pointer()` 用作内核任务指针。于是取到的 guard 是一个从未初始化的值，且随每次调用变化。ppc64le 必须显式 `-mstack-protector-guard=global`。这类问题不会 panic，只会表现为随机且不可复现的栈校验失败。

**3. ppc64le 的 trap prologue 必须自己开 FP。** `__trap_from_user` 进入时 SRR1 里带着**用户态的 MSR**，用户没开 FP/VEC 就没有 FP 权限，此时保存 FPR 会直接陷入。所以 prologue 里必须自己置 `MSR[FP]`/`MSR[VEC]`/`MSR[VSX]` 再保存向量寄存器。

**4. `.a20drv` 的符号可见性取决于 deployment profile。** 模块只能引用 `kernel/drvmod/framework.c` 里 `drv_export_table[]` 列出的符号，缺一个就是 `unresolved symbol` → **整个模块加载失败** → 对应 transport 整个消失。virtio-blk 在 aarch64 是加载模块、在 ppc64le 是内建驱动，所以导出表少一个 `snprintf` 或 `device_register`，现象是 aarch64 挂不上 `/bin`、`init` panic，而 ppc64le 一切正常。改导出表后必须按两种 profile 各验一次。

**5. 「已处理」的缺页必须真的能推进 PC。** 缺页处理路径如果对一条自己满足不了的异常返回 0，硬件就会在同一条指令上无限重入，而且因为每次都报「已处理」，**一次内核输出都没有**。aarch64 上曾表现为 mksh 在 fork 之后对同一个栈地址反复 prefetch abort（约 2.3 万次）、完全没有 fault 报告，直到超时被杀——看起来像丢唤醒，实际是活锁。根因是 `handle_present_page_fault()` 只看 PTE 不看 VMA：叶 PTE 上带了一个 VMA 从未授予的 `PTE_X` 时，它就把 exec fault 判为可满足。而 aarch64 的 `arch_pte_leaf()` 是由 `PTE_X` 推出硬件 `UXN`/`PXN` 的，所以这个"多余的 X"是真的让该页在 EL0 可执行。修法是**以 VMA 为准**：`handle_present_page_fault()` 在 `mm->lock` 下反查覆盖该地址的 VMA，VMA 没给 `VM_EXEC` 就拒绝 exec fault（写同理），`handle_demand_fault_locked()` 的 stack/brk 分支也拒绝 exec fault。这样无论叶 PTE 错成什么样，最坏结果也只是一次干净且指名道姓的 SIGSEGV，而不是静默活锁。详见 [roadmap/a20os-improvement-todo.md](roadmap/a20os-improvement-todo.md)。

**6. 「可写且可执行」在 aarch64 上是矛盾的要求。** `kernel/arch/aarch64/mm/kwx.c` 在把内核镜像切成 RO-X/RO-NX/RW-NX 之后会打开 `SCTLR_EL1.WXN`（bit 19），语义是**EL0 可写的叶描述符在 EL0 一律 execute-never**。所以任何「先把某页变成 RWX、临执行前再改回来」的做法都会失效——而且失效方式是**静默的**：内核认为可取指，硬件报 permission fault，内核毫无察觉。凡是要「同一页既当数据又当代码」的地方，必须换成两块页。

本仓库的实例是 sigreturn 跳板：它按设计写在**用户栈**上的信号帧里（栈页按定义可写），于是 AP 只能是 `01`，CPU 拒绝取指，表现为 `ESR EC=0x20 / FSC=0x0f`。注意 aarch64 上 `PTE_D` 与 `PTE_W` **是同一个 bit 56**，所以「去掉脏标记」并不能把 AP 变回 `11`——`arch_signal_tramp_pte_flags()` 本身就带 `PTE_D`。修法是给跳板一块**专用只读页**（RO+X 正是 WXN 允许的组合），地址存进 `mm->sig_tramp`，由弱钩子 `arch_signal_tramp_addr()` 交给投递路径设置 `TRAP_CTX_RA`；x86_64 一直用的就是这个模型。

配套的一条教训：**跨架构不要照抄固定虚拟地址。** x86_64 的跳板页固定在 `0x700000000000`，而 `USER_VA_LIMIT` 在 x86_64 是 2^47、在 aarch64 只有 2^46（`kernel/arch/aarch64/include/platform.h`），同一个常量在 aarch64 上会被 `mm_mmap()` 以超范围拒绝——VMA 和 PTE 都不生成，故障现场看起来像「RA 是个裸地址」。aarch64 改用 `mm_find_gap()` 分配。

诊断这类「硬件拒绝、软件说可以」的故障，光看软件 PTE 不够，需要两样东西：QEMU 的 `-d int` 原始 ESR（`FSC` 足以区分 *translation fault*「页不在」与 *permission fault*「页在但没权限」），以及同一进程内一个**确实能执行**的 text 叶作为对照。本轮就是靠把两个叶描述符逐位对比、发现只差 AP 两位才定位到的。

### 文档漂移关键词

`make check-doc-drift` 重新生成 Linux syscall 覆盖表，扫描 `docs/` 与 `kernel/` 中漂移关键词，但 `docs/research/**`、`docs/testing-gates.md`、`kernel/external/**` 除外。

若 `for simplicity` 出现在禁用区域，删除或替换为明确 TODO；若 `stub`/`partial`/`Future`/`not yet` 缺失于许可文件，确保它们已绑定到覆盖表或 TODO。

### RV64 prepared-park yield 回归

`make smoke-rv64-sched-park-yield` 用单核 QEMU 启动带 `a20.sched_park_yield_selftest=1` 的内核。自检创建一个可运行 helper，在当前任务的 wait token 处于 `PREPARING` 时强制真实切换；helper 确认原任务以 `READY` 留在运行队列、token 序号仍有效。原任务恢复后正常提交 park，由 helper 对该 token 发送 `EVENT` 唤醒，并检查 `finish` 将状态清回 `IDLE`。这覆盖 IRQ 抢占在 wait queue 检查之后、`proc_park_commit()` 之前到来的同一状态交错。

测试失败会 panic，未出现 `RV64_SCHED_PARK_YIELD: PASS` 或 QEMU 超时都视为门禁失败。此测试只用于 RV64 单核调度回归；SMP 行为由对应 SMP 门禁覆盖。

### Native cluster ABI smoke

`make smoke-native-cluster` 通过真实 Native syscall ABI 检查集群入口号、节点 ID 哈希、`set_self` 一次成功和重复调用 `A20_ERR_EXISTS`，以及 LOCAL 导出/按 slot 连接、loopback route 与 link-status。参数与结构版本测试继续验证非法输入在真实处理器中返回对应错误；它不再把 WA1 已落地的 syscall 当作统一 stub。
