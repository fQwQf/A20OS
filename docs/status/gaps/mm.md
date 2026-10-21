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

- 现状：**arm32 内核现在能编过**（`a20os-buildenv:multi` 容器里 `make ARCH=arm32 ABI=linux BRINGUP=0 dev-build` 完成，产出 kernel.bin/fat32/ext4/isofs），但在 QEMU 里启动时在 `memset` 写 `0xff800000` 处缺页 panic，还没有能启动的 arm32 镜像。**riscv32** 还差一截：现在停在 `kernel/net/netfilter.c:284` 的 `uint32_t *` / `unsigned *` 类型名不匹配（ilp32d 下 `int32_t` 是 `long`）。
- 已修的具体项：`pt_lookup_leaf()` 的四个调用点经 `vaddr_t`/`size_t` 局部变量中转；`a20_cowf_probe`、`a20_mprotect_rec/leaf`、`a20_munmap_entry`、`a20_mseal_rec`、`a20_mmap_seg_spec`（Rust `SegSpec`）、`DcacheEntry`、`uxfs_sb_t.server_generation` 偏移的 32 位值按实测改正（原来是按 LP64 猜的，多数不可达）；`a20_elf_interpreter_ops_t` 按指针宽度取值；`mprotect.c` 的溢出检查改成先在 64 位里界定；`cow.c`/`fault.c`/`mprotect.c` 里 per-PTE status sidecar 的操作改为 `ARCH_HAS_PGTABLE_OPS` 内，arm32 走 PTE 级回退；`cow_status`/`fault_status` 两个 Rust 对象在 arm32 上不链接（其 C 对应物本来就被编掉）。连带修掉 `proc/{ptrace_state.h,wait.c,exec.c,debug.c}`、`fs/procfs/procfs_render.c`、`fs/vfs/dcache_state.rs` 的同类宽度问题。
- 缺口：riscv32 与 arm32 的内核 bring-up 构建不通过，32 位档的 MM 没有可运行证据。
- 关闭条件：`check-riscv32-bringup` 与 `check-arm32-bringup` 编译通过，并在 QEMU 里完成 bring-up 与自然关机。
- 来源：DELIVERY 的保留平台构建矩阵（[current/delivery-baseline.md](../current/delivery-baseline.md) §3）；本条现状由 CORE 包在 CORE-07 取证时推进并更新。

### MM-06 宿主侧 MM 用例与当前源码脱节

- 现状：`check-a20-tests` 里 `test_usercopy_strings_host`、`test_usercopy_nommu_host`、`test_tlb_transaction_races`、`test_process_vm_host`、`test_vma_region_host`、`test_rust_merge_policy_against_actual_c_function` 失败。前四个把内核 C 抽出来用宿主 gcc 编译，抽取窗口不含新增的 `extern` 声明与 Rust 桥接原型（或 C/Rust 接口签名已变，如 `a20_usercopy_page_access` 的参数个数）；后两个在源码里找不到自己绑定的标记，直接 `ValueError: substring not found`。这些用例此前被 linked worktree 里的 `.git` 定位错误掩盖，本轮修好那个定位问题后才暴露。
- 缺口：MM 的宿主侧行为断言当前无法区分"实现回退"与"夹具漂移"。
- 关闭条件：上述用例在候选上通过，或按当前契约改写（保留行为断言与负控）后通过。
- 来源：DELIVERY 的 `check-a20-tests` 基线（[current/delivery-baseline.md](../current/delivery-baseline.md) §5）。
