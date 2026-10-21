# MM 缺口

对应 V1-MM。现状见 [current/validation-dontunmap.md](../current/validation-dontunmap.md)、
[kernel/mm/fork-mprotect-cow.md](../../kernel/mm/fork-mprotect-cow.md)。

### MM-05 32 位架构的内核 MM bring-up

- 现状：`kernel/mm/**` 的 C 与 Rust 源文件现在在 riscv32 与 arm32 下全部编译通过——
  `make -k check-riscv32-bringup` 与容器内 `make -k check-arm32-bringup` 的失败目标里
  不再有任何 `kernel/mm/` 条目。本轮修的是 32 位类型宽度与后端条件编译：
  `pt_lookup_leaf()` 的 `int *`/`vaddr_t *` 与 Rust ABI 结构的 `int32_t`/`uintptr_t`
  在 32 位下不是同一个 C 类型（宽度相同、类型不同，`-Werror` 直接拒绝）；若干 32 位 ABI
  尺寸断言是按“uint64 以 4 字节对齐”写死的，而 riscv32/arm32 的 uint64 都是 8 字节对齐；
  arm32 不定义 `ARCH_HAS_PGTABLE_OPS`，`mm_pt_node_lock`/`mm_pt_leaf_table`/
  `mm_pt_sync_status`/`mm_pt_refresh_leaf_prot` 的调用缺少条件编译；`oom_policy.rs` 在 32 位
  目标上没有 `AtomicU64`。同一轮里 `kernel/include/mm/elf_rust.h` 的
  `a20_elf_interpreter_ops_t` 断言也补齐了宽度分支。
- 缺口：bring-up 仍然不通过，但剩余失败**全部在 MM 之外**：proc（`kernel/proc/debug.c`、
  `wait.c`、`exec.c`、`resource_ns_state.rs`）、net（`netfilter.c`、`netfilter_nat.c`、
  `socket_control.c`、`netfilter_filter.rs`、`netfilter_nat.rs`、`socket_netlink.rs`）、
  fs（`fs/uxfs/uxfs.c`、`fs/procfs/procfs_render.c`、`fs/vfs/dcache_state.rs`）。
  这些已按领域追加到 [core.md](core.md)（CORE-11）、[net.md](net.md)（NET-12）、
  [fs.md](fs.md)（FS-10）。arm32 侧另外暴露了构建容器本身的缺口，见
  [delivery.md](delivery.md)（DELIVERY-05）。
- 关闭条件：上述跨领域条目关闭后，`check-riscv32-bringup` 与 `check-arm32-bringup` 编译通过，
  并在 QEMU 里完成 bring-up 与自然关机。
- 来源：DELIVERY 的保留平台构建矩阵（[current/delivery-baseline.md](../current/delivery-baseline.md) §3）。
