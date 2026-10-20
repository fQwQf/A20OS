# MM 缺口

对应 V1-MM。现状见 [current/validation-dontunmap.md](../current/validation-dontunmap.md)。

### MM-01 DONTUNMAP 的换出源与共享文件路径

- 现状：私有匿名页的目标保留、源 refault、fork/COW、RSS 清理与确定性回滚已覆盖。
- 缺口：源 PTE 已换出（swap-in）时槽/引用保留与源 refault 未验证；共享文件失败路径、更多回滚/分配器阶段、并发变更未覆盖。
- 关闭条件：确定性 swap-in guest 用例同时核对源/目标字节与换入换出计数；共享文件与并发变更用例通过。
- 来源：requirements N09、[current/validation-dontunmap.md](../current/validation-dontunmap.md)。

### MM-02 活 PTE 的 TLB 拆除

- 现状：unmap 路径有 TLB 完成后再释放的顺序约束；活 PTE 的拆表压力未测。
- 缺口：映射页在并发访问下的 TLB teardown 与释放时序没有对应门禁。
- 关闭条件：并发访问 + unmap/reclaim 的 SMP 用例通过，释放后不再命中；负控能复现旧错误。
- 来源：[current/v1.0-progress.md](../current/v1.0-progress.md) V1-MM 行。

### MM-03 共享写的冷启动与磁盘一致性

- 现状：共享文件写入在 `mprotect(PROT_READ)`/unmap 时不丢 dirty 的路径已修，RV64 有字节精确冷启动读回与 e2fsck 证据。
- 缺口：其他架构与更多映射形态没有同等证据；dirty-state 跨 shutdown 的完整矩阵未覆盖。
- 关闭条件：适用架构上共享写 → 只读保护 → 关机 → 冷启动读回字节一致且 e2fsck 干净。
- 来源：[kernel/mm/fork-mprotect-cow.md](../../kernel/mm/fork-mprotect-cow.md)。

### MM-04 分配回滚与 OOM 路径

- 现状：部分分配失败有逐阶段回滚；OOM 与分配失败的完整回滚矩阵未覆盖。
- 缺口：rollback-leaf/split-fail 之外的失败阶段（页表分裂、大页、swap 槽）没有系统性注入。
- 关闭条件：每个失败阶段都有确定性注入，且资源计数回到操作前的水平。
- 来源：[current/v1.0-progress.md](../current/v1.0-progress.md) V1-MM 行。

### MM-05 32 位架构的内核 MM 编译不过

- 现状：`check-riscv32-bringup` 在 `kernel/mm/cow.c`、`kernel/mm/fault.c`、`kernel/mm/madvise.c` 上编译失败：`pt_lookup_leaf()` 的参数是 `int *` / `vaddr_t *`（64 位 `long`），调用方传的是 `int32_t *` / `uintptr_t *`；`kernel/mm/fault.c:405` 的 `"Rust/C cowf probe ABI"` 静态断言在 riscv32 上失败。arm32 在内核构建里报同样的 `cow.c` 指针宽度问题，另有 `mm_pt_sync_status` 隐式声明，以及 `kernel/include/mm/elf_rust.h:384` 的 `"elf_rust ABI drift"` 静态断言。两处都在固定 CI 容器里用 `arm-linux-gnueabihf-gcc` 复现。
- 缺口：riscv32 与 arm32 的内核 bring-up 构建不通过，32 位档的 MM 没有任何可运行证据；这两个架构的用户态构建是通过的，所以问题集中在 MM 的 32 位类型宽度。
- 关闭条件：`check-riscv32-bringup` 与 `check-arm32-bringup` 编译通过，并在 QEMU 里完成 bring-up 与自然关机。
- 来源：DELIVERY 的保留平台构建矩阵（[current/delivery-baseline.md](../current/delivery-baseline.md) §3）。

### MM-06 宿主侧 MM 用例与当前源码脱节

- 现状：`check-a20-tests` 里 `test_usercopy_strings_host`、`test_usercopy_nommu_host`、`test_tlb_transaction_races`、`test_process_vm_host`、`test_vma_region_host`、`test_rust_merge_policy_against_actual_c_function` 失败。前四个把内核 C 抽出来用宿主 gcc 编译，抽取窗口不含新增的 `extern` 声明与 Rust 桥接原型（或 C/Rust 接口签名已变，如 `a20_usercopy_page_access` 的参数个数）；后两个在源码里找不到自己绑定的标记，直接 `ValueError: substring not found`。这些用例此前被 linked worktree 里的 `.git` 定位错误掩盖，本轮修好那个定位问题后才暴露。
- 缺口：MM 的宿主侧行为断言当前无法区分"实现回退"与"夹具漂移"。
- 关闭条件：上述用例在候选上通过，或按当前契约改写（保留行为断言与负控）后通过。
- 来源：DELIVERY 的 `check-a20-tests` 基线（[current/delivery-baseline.md](../current/delivery-baseline.md) §5）。
