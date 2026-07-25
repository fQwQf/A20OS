# 测试与文档门禁

`DOCS_AS_FACT_CONTRACT`：`docs/`、`kernel/abi/*/*.md` 以及顶层状态文件中的架构文档描述当前实现。未来设计应放在规划材料中，而不是事实文档中。

`TEST_FIRST_ARCHITECTURE_MATRIX`：每个架构债务领域在 TODO 条目可以勾选完成前，都必须有一个可重复执行的门禁。

| 领域 | 门禁 |
| --- | --- |
| 并发基础 | `make check-concurrency-foundation` |
| MM/VMA/页表 | `make check-mm-lock-model` |
| I/O 进展 | `make check-io-progress-model` |
| VFS 抽象 | `make check-vfs-abstraction` |
| ABI 边界 | `make check-abi-boundary` |
| 驱动核心 | `make check-driver-core-model` |
| 外部依赖 | `make check-external-dependency-boundary` |
| 架构边界 | `make check-arch-boundary` |
| SMP 平台边界 | `make check-smp-platform-boundary` |

`BUILD_MATRIX_GATE_CONTRACT`：bringup 构建覆盖 `riscv64`、`loongarch64`、`aarch64` 和 `x86_64`；用户态构建通过 `make check-user-build` 覆盖同一组架构。

`ARCH_MMU_RUNTIME_MATRIX_CONTRACT`：当前 NOMMU 支持集合明确限定为 `arm32`、`aarch64`、`riscv64`、`riscv32`；其他架构在构建入口即被拒绝，不再形成可链接但不可运行的伪配置。`make smoke-arch-mmu-matrix` 在 QEMU 中覆盖这四个架构的 MMU 与 NOMMU 八种有效组合。每个组合必须进入交互式 shell，分别执行 shell builtin 与外部程序，并通过用户态 `poweroff` 正常关机。架构差异通过 `kernel/arch/<arch>/` 提供的 hook/capability 表达；`make check-arch-boundary` 禁止通用内核代码直接按具体架构条件编译。

`SMP_PLATFORM_BOUNDARY_CONTRACT`：`kernel/core/smp.c` 统一管理逻辑 CPU 拓扑、online 状态、启动等待和 IPI 分派；`kernel/platform/<board>/` 提供 CPU 发现、启动、IPI 和本地控制器 hooks；`kernel/arch/<arch>/platform/smp.c` 只保留 secondary 入口与架构机制，不得按具体 board 编译平台策略。

`ABI_SMOKE_GATE_CONTRACT`：Linux ABI smoke 通过 `smoke-abi-linux` 运行 `syscall_smoke` 和用户态命令；Native ABI 覆盖包括 `native-minimal`、`native-test`、`user/tests/test_liba20c.c`，以及用于 handle dup/transfer 的 `make smoke-native-handle` 运行时覆盖。

`DOC_DRIFT_KEYWORD_GATE`：`stub`、`partial`、`TODO`、`Future`、`not yet`、`for simplicity` 等漂移关键词只有在绑定到明确的覆盖表、TODO 条目或门禁契约时才允许出现。`kernel/external/` 和 `user/external/` 下导入的第三方代码树不参与该门禁。

## 运行手册

### 并发基础
- **How to run**: `make check-concurrency-foundation`
- **What it checks**: 检查 `SCHEDULER_CONCURRENCY_PREREQS`、`PER_CPU_CURRENT_VALIDATION`、`TASK_STATE_MUTATION_CONTRACT`、`BLOCK_WAKE_PROTOCOL` 等静态契约；并用 `NR_CPUS=2 ALLOW_UNVERIFIED_SMP=1 BRINGUP=1` 构建一次以验证 SMP 基础编译路径。
- **When it fails**: 检查 `kernel/proc/sched.c`、`kernel/proc/current.c`、`kernel/include/proc/proc.h`、`kernel/include/core/sync.h` 中对应契约字符串是否缺失；若 SMP 编译失败，通常是新增代码持有大锁或违反锁顺序。

### MM/VMA/页表
- **How to run**: `make check-mm-lock-model`
- **What it checks**: 检查 `MM_LOCK_MODEL`、`MM_VMA_PTE_AUDIT`、`COW/DEMAND_FAULT_TLB_CONTRACT`、`MM_FORK_COW_REGRESSION_GUARD`、`FILE_MMAP_PAGE_CACHE_CONTRACT`、`OOM_RECLAIM_LIFETIME_CONTRACT` 等静态契约；确认 `smoke-mm-stress` 与 `MM_STRESS: PASS` 存在。
- **When it fails**: 补充或恢复 `kernel/include/mm/vm.h`、`kernel/mm/vm.c`、`kernel/mm/fault.c`、`kernel/include/mm/oom.h` 中对应契约字符串；确保 MM 压力测试入口未删除。

### I/O 进展
- **How to run**: `make check-io-progress-model`
- **What it checks**: 检查 `KERNEL_PROGRESS_SERVICE_CONTRACT`、progress bottom-half 调用点、`LWIP_NO_THREAD_PROGRESS_CONTRACT`、virtio-net 非阻塞路径；禁止在 `kernel/proc/sched.c` 或 `kernel/proc/proc.c` 中直接调用 `virtio_blk_poll_all` 或 `a20_lwip_poll`。
- **When it fails**: 检查 `kernel/core/progress.c`、`kernel/proc/sched.c`、`kernel/include/core/progress.h`、`kernel/net/lwip_stack.c` 中对应契约字符串；禁止在调度器/进程路径中直接轮询 virtio-blk 或 lwIP。

### VFS 抽象
- **How to run**: `make check-vfs-abstraction`
- **What it checks**: 检查 `VFS_OPEN_DISPATCH_CONTRACT`、`VFS_REFCOUNT_HELPER_CONTRACT`、`VFS_DCACHE_MOUNT_VNODE_INVARIANT`、`VFS_CONCURRENCY_SMOKE_MATRIX` 等静态契约；确认 `vfile_ref_init`/`vfile_get`/`vfile_put_ref_only`、各文件系统 `open` 方法表、`smoke-vfs-stress` 与 `VFS_STRESS: PASS` 存在。
- **When it fails**: 确认 `kernel/fs/vfs.c`、`kernel/fs/file.c`、`kernel/include/fs/vfs.h`、`kernel/include/fs/file.h` 中契约字符串与辅助函数存在；确保各文件系统后端仍有 `open` 方法，且未直接操作底层 `ref_count`。

### ABI 边界
- **How to run**: `make check-abi-boundary`
- **What it checks**: 重新生成 Linux syscall 覆盖表；检查 `LINUX_ABI_BOUNDARY_CONTRACT`、`LINUX_ABI_EXPLICIT_STUB_CONTRACT`、`NATIVE_HANDLE_CAPABILITY_CONSISTENCY_MATRIX`、`NATIVE_DEBUG_LIMITED_CONTRACT` 等静态契约；确认 `docs/native-abi/00-overview.md` 仍包含 `Debug 分区受限`。
- **When it fails**: 运行 `python3 scripts/gen_linux_syscall_coverage.py` 看是否生成失败；检查 `kernel/abi/linux/syscall_impl.h`、`kernel/abi/linux/syscall_table.def`、`kernel/abi/native/handle_table.h`、`kernel/abi/native/sys_phase2.c` 中契约字符串；确认 `00-overview.md` 的 `Debug 分区受限` 说明未删除。

### 驱动核心
- **How to run**: `make check-driver-core-model`
- **What it checks**: 检查 `DRIVER_CORE_CONCURRENCY_MODEL`、`DRIVER_CORE_DYNAMIC_LIMITS`、`DRIVER_PROBE_FAILURE_CLEANUP`、`DRIVER_ENUMERATION_FAILURE_MODEL`、`DRIVER_IRQ_DMA_SEMANTICS`、`DRIVER_SMOKE_MATRIX` 等静态契约；确认驱动生命周期测试、virtio-blk/net、UART、PTY、loop、PCI 与 virtio-mmio 枚举函数存在；`kernel/main.c` 不再直接调用 `virtio_blk_init`/`virtio_net_init`。
- **When it fails**: 检查 `kernel/drivers/core/driver_core.c`、`kernel/drivers/core/driver_hwapi.c`、`kernel/drivers/core/driver_lifecycle_test.c` 中对应契约字符串；确认 `docs/drivers/README.md` 引用了 `kernel/drivers/` 与 `kernel/platform/`；修复直接调用驱动的初始化代码。

### 外部依赖边界
- **How to run**: `make check-external-dependency-boundary`
- **What it checks**: 检查 `include kernel/external/lwip/sources.mk` 是否在 Makefile 中；`docs/project/external-dependencies.md` 是否包含 `EXTERNAL_LWIP_SOURCE_MANIFEST`、`EXTERNAL_LWIP_CONFIG_CONTRACT`、`EXTERNAL_USERLAND_UPGRADE_CHECKLIST`、`EXTERNAL_STATIC_LINK_REBUILD_CONTRACT`、`EXTERNAL_TLSE_WGET_LIMITS` 等；确认 `Makefile` 中没有直接定义 `LWIP_SRC`。
- **When it fails**: 检查 `Makefile` 的 lwIP 包含语句；补充或恢复 `docs/project/external-dependencies.md` 中的对应契约标题；确认 `kernel/external/lwip/sources.mk` 包含 `LWIP_SRC` 与 `core/timeouts.c`。

### 架构边界
- **How to run**: `make check-arch-boundary`
- **What it checks**: 禁止通用内核代码中出现 `CONFIG_AARCH64`/`CONFIG_ARM32`/`__aarch64__`/`__arm__` 等架构条件编译；验证 LoongArch64、x86_64、PPC64LE 的 NOMMU 构建在入口即被拒绝；确认 `smoke-arch-mmu-matrix` 在 Makefile 与 `docs/OS-Design.md` 中存在。
- **When it fails**: 检查 `rg` 扫描结果中是否在 `kernel/arch/**`、`kernel/platform/**`、`kernel/external/**`、`kernel/include/core/arch.h` 之外出现架构宏；确认 `docs/testing/testing-gates.md` 与 `docs/OS-Design.md` 包含 `ARCH_MMU_RUNTIME_MATRIX_CONTRACT` 与 `smoke-arch-mmu-matrix`。

### 构建矩阵
- **How to run**: `make check-build-matrix`
- **What it checks**: 运行默认四架构的内核 bringup 构建与用户态构建；确认 `BUILD_MATRIX_GATE_CONTRACT` 字符串仍存在于本文档。
- **When it fails**: 修复对应架构的 `check-<arch>-bringup` 或 `check-<arch>-user` 错误；确保 `BUILD_MATRIX_GATE_CONTRACT` 仍存在于本文档。

### 架构 / MMU 运行矩阵
- **How to run**: `make smoke-arch-mmu-matrix`
- **What it checks**: 在 QEMU 中运行 `arm32`、`aarch64`、`riscv64`、`riscv32` 的 MMU 与 NOMMU 组合，验证 shell 内置命令、外部程序及 `poweroff` 正常关机。
- **When it fails**: 查看 `.kernel-build/smoke/<arch>[-nommu]-shell.log` 中是否缺少 `A20_MATRIX_<variant>_OK`、`A20_EXTERNAL_OK` 或 `System is going down for power-off NOW`；先修复对应架构的 bringup 或 NOMMU 路径。

### Linux ABI smoke
- **How to run**: `make smoke-abi-linux`
- **What it checks**: 构建 `riscv64 ABI=linux BRINGUP=0` 的镜像，在 QEMU 中运行 `syscall_smoke` 与 `poweroff`，确认串口日志出现 `SYSCALL_SMOKE: PASS`。
- **When it fails**: 检查 `.kernel-build/smoke/abi-linux-riscv64.log` 中是否因 `SYSCALL_SMOKE: PASS` 未出现而超时；修复 `user/cmds/syscall_smoke.c` 或 Linux ABI 实现。

### Native ABI 测试
- **How to run**: `make native-minimal` 或 `make native-test` 生成并运行原生测试；`make smoke-native-handle` 在 QEMU 中运行 handle dup/transfer 覆盖。`user/tests/test_liba20c.c` 是 liba20c 内部测试源文件，随 `native-test` 或用户态构建编译。
- **What it checks**: `native-minimal` 生成最小原生可执行文件；`native-test` 运行原生测试；`smoke-native-handle` 启动 `/bin/native-handle-rv` 并验证正常关机。
- **When it fails**: 检查 `user/liba20rt/` 与 `user/liba20c/` 的编译错误；确认 `native-handle-rv` 已生成并放入 fat32 镜像；查看 `.kernel-build/smoke/native-handle-riscv64.log`。

### 文档漂移关键词
- **How to run**: `make check-doc-drift`
- **What it checks**: 重新生成 Linux syscall 覆盖表；扫描 `docs/` 与 `kernel/` 中漂移关键词，但 `docs/research/**`、`docs/testing/testing-gates.md`、`kernel/external/**` 除外。
- **When it fails**: 若 `for simplicity` 出现在禁用区域，删除或替换为明确 TODO；若 `stub`/`partial`/`Future`/`not yet` 缺失于许可文件，确保它们已绑定到覆盖表或 TODO。
