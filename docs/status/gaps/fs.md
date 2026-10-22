# FS 缺口

对应 V1-FS。现状见 [current/fs-status.md](../current/fs-status.md)。
### FS-11 fs 的 32 位内核构建不通过

- 现状：`make -k check-riscv32-bringup` 在 `kernel/fs/uxfs/uxfs.c:1049`
  （`uxfs namespace mirror: sb view prefix layout` 静态断言）、
  `kernel/fs/procfs/procfs_render.c:427`（两次 `proc_loadavg_snapshot` 指针类型不匹配）、
  `kernel/fs/vfs/dcache_state.rs:41`（`DcacheEntry` 期望 312、实际 304）报错；
  arm32 侧同样报 `fs/uxfs/uxfs.c:1049` 与 `fs/vfs/dcache_state.rs:41`。
- 缺口：fs 的 32 位内核构建不通过。两处静态断言按“uint64 以 4 字节对齐”的 32 位布局写死，
  而 riscv32/arm32 的 uint64 是 8 字节对齐。
- 关闭条件：两个架构的内核构建不再有 `kernel/fs/` 条目。
- 来源：MM-05 的 32 位复现（[mm.md](mm.md)），本轮 `make -k check-riscv32-bringup` /
  容器内 `make -k check-arm32-bringup` 输出。
