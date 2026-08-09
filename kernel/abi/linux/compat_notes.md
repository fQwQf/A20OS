# Linux ABI 兼容性说明

`kernel/abi/linux` 为 A20OS 用户态实现了一组 Linux-compatible syscall 子集。
它是兼容层，不是完整的 Linux kernel personality。
`syscall_table.def` 当前登记 258 个入口（含 2 个 A20OS 扩展），但登记只表示
分派表覆盖，不表示 258 项都达到 Linux 语义完整性。其中 16 个入口是表中直接
固定返回 `-ENOSYS` 的显式占位符；其他已登记 syscall 仍可能只支持部分命令、
flag、对象类型或并发边界。

## 兼容等级

- `full`：目标是在已支持的 flag 和对象类型范围内匹配 Linux 语义。
- `partial`：足够支撑当前用户态和测试，但已知边界语义缺失或被简化。
- `stub`：主要用于让软件探测能力或继续运行；行为固定、简化，或者在没有完整内核语义的情况下返回成功。
- `missing`：syscall 未实现，或者关键操作返回 `-ENOSYS`。

## 当前高风险 Partial 区域

- `bpf(2)`：只支持经 A20OS KEP 引擎验证和执行的 `BPF_PROG_LOAD`、`BPF_PROG_ATTACH`、`BPF_PROG_DETACH`；没有 BPF map 命令，attach 的 `target_fd` 是 A20OS extension-point id，也不等同于 Linux verifier/helper/JIT/BTF/pinning 与对象生命周期语义。
- 调度策略调用：底层是 per-CPU EEVDF、SMP 运行队列、affinity 和远程唤醒；Linux policy/priority/affinity API 仍是有边界的兼容映射，不包含完整的 Linux RT、deadline、cgroup 和 topology 语义。
- Namespace 和 capability 调用：只实现了较小的兼容子集，没有完整的 Linux namespace/security model。
- Futex：已有基础 wait/wake 路径，但高级操作和所有 Linux memory-ordering 边界语义尚不完整。
- VFS：已有很多路径和文件操作，但 mount namespace、symlink、权限、文件系统特定行为和并发语义仍不完整。
- Sockets：AF_INET/AF_UNIX/AF_ALG 兼容面足够覆盖测试，但没有实现完整 Linux network stack 行为。
- POSIX timers 和 timerfd：足够支撑常见等待场景，但 signal delivery 和 overrun 语义被简化。
- Keyring：`kernel/ipc/keyring.c` 实现 add/request/keyctl 的核心命令与 session/user keyring，但没有 key type instantiator、`KEYCTL_*` 全部命令或完整 Linux 安全语义。
- fanotify：基于共享 notify 后端（`kernel/fs/inotify.c`）实现 FAN_CLASS_NOTIF + FID；content/pre-content 类和 per-event fd 不在范围内。
- acct(2)：写入 Linux v3 记账记录，但没有 BSD 风格的 `ac_btime` 高精度或完整字段集合。
- Linux AIO：`kernel/fs/aio.c` 提供上下文与完成队列，但 pread/pwrite/fsync/fdatasync 在当前 VFS 上同步执行，不是后台异步 I/O；`io_cancel` 无法中止正在执行的 op。
- Module syscall：`init_module/finit_module/delete_module` 驱动 A20OS 的 drvmod ET_REL 驱动加载器，加载的是 A20 驱动模块而非 Linux 内核模块，且要求 CAP_SYS_MODULE。
- 跨进程内存：`process_vm_readv/writev` 直接走目标页表（`kernel/mm/process_vm.c`），`process_madvise` 复用 madvise 兼容语义，`process_mrelease` 依赖退出时自动回收。
- mempolicy/NUMA：单 NUMA 节点，`set_mempolicy/mbind/migrate_pages/move_pages` 只做策略校验与存储，不移动物理页。
- 文件句柄：`name_to_handle_at/open_by_handle_at` 基于内核句柄注册表（`kernel/fs/file_handle.c`），不是 filesystem export 操作。
- 新 mount API：`fsopen/fsconfig/fsmount/fspick/open_tree/move_mount/mount_setattr` 建立在现有 mount 表之上；`mount_setattr` 不支持逐 mount 属性。
- io_uring：SQ/CQ 环在内核内存（`kernel/fs/io_uring.c`）并映射进调用者；`io_uring_enter` 同步执行 NOP/READ/WRITE/FSYNC/CLOSE，没有真正的后台异步完成。
- Landlock：fd-backed ruleset + path-beneath 规则，在 `vfs_open` 强制；没有完整 LSM 框架。
- rseq：注册/注销每线程 rseq 区域；A20OS 不做跨 CPU 迁移，因此内核从不中止序列。
- `restart_syscall` 返回 `-ERESTARTNOINTR`；`remap_file_pages` 是接受式 no-op；`memfd_secret` 退化为普通 memfd。

## 维护规则

1. 新增 Linux syscall 实现必须登记到 `syscall_coverage.md`。
2. 兼容性捷径应在实现里用简短注释标明，并同步反映到本文档。
3. ABI 文件只应负责转换用户参数并调用内部子系统 API；不应拥有子系统全局状态。
4. 如果某个 syscall 为了兼容而故意对未支持特性返回成功，应登记为 `stub`，不能登记为 `full`。
