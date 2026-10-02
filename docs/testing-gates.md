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
| I/O 进展 | `make check-io-progress-model` |
| VFS 抽象 | `make check-vfs-abstraction` |
| ABI 边界 | `make check-abi-boundary` |
| 驱动核心 | `make check-driver-core-model` |
| 外部依赖 | `make check-external-dependency-boundary` |
| 架构边界 | `make check-arch-boundary` |
| SMP 平台边界 | `make check-smp-platform-boundary` |
| 上游包运行（Alpine gcc 经 chroot 编译+运行） | `make smoke-devtools`（需网络拉取上游包，守护 trap.S sp 守卫修复） |
| 3D 通路像素回读（三种架构） | `tools/a20 test smoke-gpu3d-riscv64` / `-x86_64` / `-aarch64`（需 host virgl + `tools/with-virgl-display.sh`；见 [graphics/3d-graphics.md](graphics/3d-graphics.md)） |
| stock Mesa/virgl attach 门禁 | `make smoke-mesa-attach`（需 host virgl）。**断言 guest 起来了 virgl virtio-gpu，且 Mesa 真的 bind 到 virgl**（renderer 字符串判据 `profile renderer: *virgl`）。默认硬失败；`MESA_REQUIRE_VIRGL_ATTACH=0` 可降级为只报告。已通过（renderer `virgl (…radeonsi…)`）。详见 [graphics/3d-graphics.md §0.8](graphics/3d-graphics.md) |

`BUILD_MATRIX_GATE_CONTRACT`：完整 hosted 构建集合是 `riscv64`、`loongarch64`、`aarch64`、`x86_64`、`arm32`、`riscv32` 和 `ppc64le`。Linux 上 `make check-build-matrix` 使用这七项，macOS 的默认集合只含 RISC-V64；需要与主机无关的显式七架构集合时使用 `make check-build-matrix-all`，它额外包含 `loongarch32`（NaiLoong LA32R，仅 kernel-only bring-up，无 QEMU 目标）、VisionFive2 与 LS2K1000 板级构建门禁。每架构门禁列表由根 `Makefile` 的 `SUPPORTED_HOSTED_ARCHES` 单一真源派生，新增架构必须只改那一处。ARMv7-M 由独立 STM32 build/check 目标覆盖，不属于 hosted 用户态矩阵。

`CI_BUILD_MATRIX_CONTRACT`：CI 实际逐架构构建的是上述七项中的六项，由根 `Makefile` 的 `CI_KERNEL_ARCHES` 声明（`riscv64 loongarch64 aarch64 x86_64 riscv32 ppc64le`），`.github/workflows/ci.yml` 的 `kernel-build` 矩阵经 `make print-ci-kernel-arches` 解析得到，因此 CI 不会与 `SUPPORTED_HOSTED_ARCHES` 漂移。被排除的两项是 `arm32` 与 `loongarch32`，理由记录在 `CI_KERNEL_ARCHES` 的注释里：`arm32` 的 `kernel/mm/fault.c` 使用了 `kernel/include/mm/pt.h` 只在 `ARCH_HAS_PGTABLE_OPS` 下声明的 `mm_addrspace_lock()` / `mm_cursor_*` 事务接口，而该宏刻意不给 arm32，所以 `check-arm32-bringup` 当前编译不过；`loongarch32` 需要从源码构建的 cloudspurs la32 工具链，发行版没有可安装的包，CI 容器无法提供。CI 的宿主侧源码契约门禁（`check-honesty-policy`、`check-smoke-cases`、`host-tests`、`check-drm-abi` 与 `check-doc-test-gates` 中不依赖 QEMU 的 6 个子门禁）不进入构建容器，见 [packaging/ci.md](packaging/ci.md)。

`ARCH_MMU_RUNTIME_MATRIX_CONTRACT`：`NOMMU_SUPPORTED_ARCHES` 的构建集合是 `arm32`、`aarch64`、`riscv64`、`riscv32`、`armv7m`。`make smoke-arch-mmu-matrix` 的 hosted runtime 集合只包含前四个架构的 MMU 与 NOMMU 八种组合；ARMv7-M 由 STM32 MCU 目标单独处理。LoongArch64、x86_64、PPC64LE 的 NOMMU 配置在构建入口被拒绝。每个 hosted runtime 组合应进入交互式 shell，执行 shell builtin 与外部程序，并通过用户态 `poweroff` 正常关机。架构差异通过 `kernel/arch/<arch>/` 提供的 hook/capability 表达；`make check-arch-boundary` 禁止通用内核代码直接按具体架构条件编译。

`SMP_PLATFORM_BOUNDARY_CONTRACT`：`kernel/core/smp.c` 统一管理逻辑 CPU 拓扑、online 状态、启动等待和 IPI 分派；`kernel/platform/<board>/` 提供 CPU 发现、启动、IPI 和本地控制器 hooks；`kernel/arch/<arch>/platform/smp.c` 只保留 secondary 入口与架构机制，不得按具体 board 编译平台策略。

`ABI_SMOKE_GATE_CONTRACT`：Linux ABI smoke 通过 `smoke-abi-linux` 运行 `syscall_smoke` 和用户态命令；Native ABI 的 `native-minimal`、`native-test` 和 `native-libc` 是构建检查，其中 `native-libc` 编译 `user/tests/test_liba20c.c`。用于 handle dup/transfer 的 `make smoke-native-handle` 才是 QEMU 运行时覆盖。

`DOC_DRIFT_KEYWORD_GATE`：`stub`、`partial`、`TODO`、`Future`、`not yet`、`for simplicity` 等漂移关键词只有在绑定到明确的覆盖表、TODO 条目或门禁契约时才允许出现。`kernel/external/` 和 `user/external/` 下导入的第三方代码树不参与该门禁。

`HOST_RESOURCE_GATE_CONTRACT`：任何会启动 guest 的门禁都必须先做宿主资源预检。QEMU 申请到宿主机给不出的内存时不会返回非零退出码，而是宿主 OOM killer 挑一个进程杀掉，被杀的通常不是正在被调试的那个对象，所以"启动失败"在这里不是一个可观测的错误路径。预检由 `tools/a20_resource.py` 单一实现：经 `tools/a20` 的实例路径和经 `tools/targets-smoke.mk` 的 `smoke-gate` 宏（35 个直接起 `qemu-system-*` 的目标）调用的是同一个 `gate()` 与同一套 `A20_*` 环境策略，两者不得各自实现等待逻辑。门禁参数必须与该目标 `qemu` 命令行里的 `-m`/`-smp` 一致，否则预检在保护另一件事。等待策略有意分两种：`tools/a20 run/debug/test` 的 `A20_WAIT_TIMEOUT` 默认 `0`（一直等，因为交互式跑实例时"等资源释放"正是期望行为）；`smoke-*` 门禁的可覆盖默认是 `900s` 有界等待，因为 CI 在宿主机磁盘真的满时必须失败而不是挂死。可用性取 `/proc/meminfo` 的 `MemAvailable` 而非 `free`（后者不含可回收 page cache，会让门禁永远阻塞），并发 guest 数单独统计（空闲 vCPU 不进 loadavg，只看 load 无法判断是否已有 guest 占着 CPU）。

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

### Task 引用与异步所有权

主入口是 `make check-task-lifetime-boundary`，双架构累计运行使用 `make check-proc-step35-local`。

检查项：PID 查询必须返回带引用的 task；task list、PID table、 runqueue、dispatch/current、wait/wake 和 timeout owner 的引用能够闭环；`/proc/a20/task_lifetime` 的错误计数与压力测试入口仍然存在；禁止重新引入裸 `proc_find()`。

失败时先确认新增的异步 task 指针是否在发布前 `proc_get()`，并在摘除后的唯一所有者路径 `proc_put()`，再比较压力测试前后的 task/ref、 wait/wake、timeout 和 zombie 基线。

### Park/Wake 与阻塞点

`make check-blocking-point-boundary` 守的就是这条发布边界，完整累计矩阵使用 `make check-proc-step4-local`。

检查项：只有 `task.c`/`park.c` 能发布 `PROC_BLOCKED`，只有 scheduler 白名单能直接调用 `proc_make_ready()`；wait queue、futex、 timeout 和 wake queue entry 都保存 task 引用与 `wait_seq`；Futex wait 在入队前完成用户值二次检查。

失败时把阻塞路径改成 prepare → 对象锁内重查/link → unlock → commit → unlink/recheck → finish；waker 必须在对象锁内 collect，在解锁后 flush，不能直接写 task 状态。

### MM/VMA/页表

`make check-mm-lock-model` 覆盖 MM/VMA/页表这一组静态契约：`MM_LOCK_MODEL`、`MM_VMA_PTE_AUDIT`、`COW/DEMAND_FAULT_TLB_CONTRACT`、`MM_FORK_COW_REGRESSION_GUARD`、`FILE_MMAP_PAGE_CACHE_CONTRACT`、`OOM_RECLAIM_LIFETIME_CONTRACT` 等；并确认 `smoke-mm-stress` 与 `MM_STRESS: PASS` 存在。

失败时补充或恢复 `kernel/include/mm/vm.h`、`kernel/mm/vm.c`、`kernel/mm/fault.c`、`kernel/include/mm/oom.h` 中对应契约字符串，并确保 MM 压力测试入口未删除。

### I/O 进展

`make check-io-progress-model` 检查 `KERNEL_PROGRESS_SERVICE_CONTRACT`、progress bottom-half 调用点、`LWIP_NO_THREAD_PROGRESS_CONTRACT`、virtio-net 非阻塞路径；禁止在 `kernel/proc/sched.c` 或 `kernel/proc/proc.c` 中直接调用 `virtio_blk_poll_all` 或 `a20_lwip_poll`。

失败时查 `kernel/core/progress.c`、`kernel/proc/sched.c`、`kernel/include/core/progress.h`、`kernel/net/lwip_stack.c` 中对应契约字符串，确认调度器/进程路径没有直接轮询 virtio-blk 或 lwIP。

### VFS 抽象

`make check-vfs-abstraction` 检查 `VFS_OPEN_DISPATCH_CONTRACT`、`VFS_REFCOUNT_HELPER_CONTRACT`、`VFS_DCACHE_MOUNT_VNODE_INVARIANT`、`VFS_CONCURRENCY_SMOKE_MATRIX` 等静态契约；确认 `vfile_ref_init`/`vfile_get`/`vfile_put_ref_only`、各文件系统 `open` 方法表、`smoke-vfs-stress` 与 `VFS_STRESS: PASS` 存在。

失败时确认 `kernel/fs/vfs.c`、`kernel/fs/file.c`、`kernel/include/fs/vfs.h`、`kernel/include/fs/file.h` 中契约字符串与辅助函数还在，各文件系统后端仍有 `open` 方法，且未直接操作底层 `ref_count`。

### ABI 边界

`make check-abi-boundary` 重新生成 Linux syscall 覆盖表，检查 `LINUX_ABI_BOUNDARY_CONTRACT`、`LINUX_ABI_PLACEHOLDER_RESOLUTION_CONTRACT`、`NATIVE_HANDLE_CAPABILITY_CONSISTENCY_MATRIX`、`NATIVE_DEBUG_LIMITED_CONTRACT` 等静态契约；确认 `docs/native-abi/00-overview.md` 仍包含 `Debug 分区受限`。

失败时先运行 `conda run -n a20os python tools/gen_linux_syscall_coverage.py` 看是否生成失败，再检查 `kernel/abi/linux/syscall_impl.h`、`kernel/abi/linux/syscall_table.def`、`kernel/include/ipc/handle_table.h`、`kernel/abi/native/sys_phase2.c` 中契约字符串，并确认 `00-overview.md` 的 `Debug 分区受限` 说明未删除。

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

`make native-minimal`、`make native-test` 和 `make native-libc` 只构建对应原生程序，其中 `native-minimal` 与 `native-test` 检查编译和链接，目标名不表示执行；`native-libc` 构建 liba20c 测试程序，`user/tests/test_liba20c.c` 由 `native-libc` 编译。QEMU 运行时覆盖只有 `make smoke-native-handle` 一条：`smoke-native-handle` 启动 `/bin/native-handle-rv` 并验证正常关机。

失败时检查 `user/liba20rt/` 与 `user/liba20c/` 的编译错误，确认 `native-handle-rv` 已生成并放入 fat32 镜像，并查看 `.kernel-build/smoke/native-handle-riscv64.log`。

### 用户态文件系统宿主（uxfs + ufsd）

`make smoke-native-fs-all` 跑四后端端到端，`make smoke-native-ufs` 只跑 FAT 后端回归。`smoke-native-fs-all` 在 QEMU 中挂四块 scratch 盘（bus.2/4/6/7），由 `/bin/ufs_all_test` 逐后端拉起 `/bin/ufsd-rv` 并执行 POSIX 序列：FAT 预置读回+写读+删除；ext4 预置读回+8 KiB 图案写读+rename+删除；iso9660 小写名嵌套读取；ntfs 只读语义（create 必须失败）。内核侧 uxfs 代理把 vnode ops 经 Channel 转发给服务，块 IO 走受控 fs_block_io。见 [hybrid-kernel/06-user-fs.md](hybrid-kernel/06-user-fs.md)。

失败时查看 `.kernel-build/smoke/native-fs-all-riscv64.log` 中各 `UXFS_*` 标记与 `[fs]` 前缀的 FS 内部日志，确认镜像目标（`ufs-scratch.img`/`ufs-ext4.img`/`ufs-iso.img`/`ufs-ntfs.img`)已生成且盘位未占用 bus.3/bus.5（用户驱动预留）。

### SIOCGIFCONF 与 per-interface getter（Linux ABI sockets 区域）

`make smoke-net-iface` 覆盖 `user/cmds/net/net_iface_test.c` 二十项，同时被 `smoke-network-suite` 覆盖，因为 `net_iface_test` 已列入 `network_suite` 的用例表。检查项包括 SIOCGIFCONF 的空 `ifc_buf` 尺寸查询（必须是整条目数，估算值会让调用方反复扩容）、按 `struct ifreq` 步长填充、行数与查询值一致、接口名 NUL 终止、每行是 `AF_INET` sockaddr、不足一个条目的缓冲返回 0 且不写入、单条目缓冲不溢出且只返回整条目、非 IPv4 族请求返回空列表、负 `ifc_len` 返回 EINVAL；再用枚举出的名字回灌 SIOCGIFADDR / SIOCGIFFLAGS，并确认未知接口名被拒。

它必须存在，是因为 getifaddrs()、ifconfig 与 busybox `ip` 全部建立在 SIOCGIFCONF 上，而 per-interface getter 在没有枚举手段之前不可达。SIOCGIFCONF 此前是"派发但未实现"（-ENOTTY），该区域没有任何运行门禁，因此下面两个真实缺陷是写这个门禁时才暴露的。

失败时查看 `.kernel-build/smoke/smoke-net-iface.log` 中首个 `NET_IFACE: FAIL` 行，对照 `kernel/net/socket_file.c` 的 `struct a20_ifreq` 与 `net_ifreq_fill` / `net_ifreq_put_addr`。`info` 行会打印用户态 `sizeof(struct ifreq)`、`offsetof(ifr_ifru)` 与每个接口的地址，ABI 不匹配时这三行即可定位。

### poll / timer 边界语义（Linux ABI poll 与 timer 区域）

`make smoke-poll-edge`、`make smoke-timer-edge` 覆盖两组边界语义。`poll_edge.c` 十二组：poll 超时边界与 POLLNVAL/HUP/ERR、select 语义与结果集剪枝、ppoll 超时与 sigsetsize/负时间 EINVAL、epoll 参数校验、嵌套 epoll 的 ELOOP 环路拒绝、dup 共享 interest list、ET/LT、EPOLLONESHOT、HUP+数据、epoll_pwait2 亚毫秒向上取整、eventfd 计数/信号量/溢出边沿。`timer_edge.c` 十二组：timer_create 的 clockid 与 sigev 校验、settime/gettime 剩余时间、TIMER_ABSTIME、overrun 计数、delete 后不再投递、timerfd 基础语义、TFD_TIMER_CANCEL_ON_SET、itimer REAL/VIRTUAL/PROF 实际投递、clock_nanosleep 相对与 ABSTIME、EINTR + remaining。这两组是把覆盖表 poll/timer 区域提升到 `full` 的依据（`nanosleep` 的 restart 语义仍是记录在案的 partial）。

失败时查看 `.kernel-build/smoke/poll-edge-riscv64.log` / `timer-edge-riscv64.log` 中首个 `POLL_EDGE: FAIL` / `TIMER_EDGE: FAIL` 组名，对照 `kernel/abi/linux/sys_epoll.c`、`kernel/abi/linux/{sys_timer_posix,sys_time,sys_fs}.c`、`kernel/proc/timer_posix.c`、`kernel/ipc/timerfd.c`、`kernel/proc/timer_heap.c`。

### mount namespace（unshare/setns CLONE_NEWNS）

`make smoke-mntns` 用 `mntns_test.c` 覆盖 init 命名空间 ino 非零、`/proc/self/ns/{pid,net}` 渲染、fork 共享挂载命名空间、`unshare(CLONE_NEWNEWPID|NEWNET|NEWUSER)` 如实返回 EINVAL（不假成功）、`unshare(CLONE_NEWNS)` 生成不同 ino 且其挂载对父进程不可见、setns 经 `/proc/<pid>/ns/mnt` fd 加入（目标先退出仍可加入）、非 mnt 目标 EINVAL。

失败时查看 `.kernel-build/smoke/mntns-riscv64.log` 中首个 `MNTNS_TEST: FAIL` 行（含行号与 errno），对照 `kernel/fs/vfs/mntns.c`、`kernel/abi/linux/sys_namespace.c` 与 `kernel/fs/procfs/procfs.c` 的 ns 渲染。

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

### 文档漂移关键词

`make check-doc-drift` 重新生成 Linux syscall 覆盖表，扫描 `docs/` 与 `kernel/` 中漂移关键词，但 `docs/research/**`、`docs/testing-gates.md`、`kernel/external/**` 除外。

若 `for simplicity` 出现在禁用区域，删除或替换为明确 TODO；若 `stub`/`partial`/`Future`/`not yet` 缺失于许可文件，确保它们已绑定到覆盖表或 TODO。
