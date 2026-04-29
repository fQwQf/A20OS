# Linux ABI 兼容性说明

`kernel/abi/linux` 为 A20OS 用户态实现了一组 Linux-compatible syscall 子集。
它是兼容层，不是完整的 Linux kernel personality。

## 兼容等级

- `full`：目标是在已支持的 flag 和对象类型范围内匹配 Linux 语义。
- `partial`：足够支撑当前用户态和测试，但已知边界语义缺失或被简化。
- `stub`：主要用于让软件探测能力或继续运行；行为固定、简化，或者在没有完整内核语义的情况下返回成功。
- `missing`：syscall 未实现，或者关键操作返回 `-ENOSYS`。

## 当前高风险 Partial 区域

- `bpf(2)`：已有 map CRUD 和最小 socket attach 行为，但没有 verifier、eBPF interpreter、helper model、JIT、BTF、pinning，也没有完整对象生命周期。
- 调度策略调用：priority 和 affinity API 是建立在当前单 CPU scheduler 之上的兼容近似。
- Namespace 和 capability 调用：只实现了较小的兼容子集，没有完整的 Linux namespace/security model。
- Futex：已有基础 wait/wake 路径，但高级操作和所有 Linux memory-ordering 边界语义尚不完整。
- VFS：已有很多路径和文件操作，但 mount namespace、symlink、权限、文件系统特定行为和并发语义仍不完整。
- Sockets：AF_INET/AF_UNIX/AF_ALG 兼容面足够覆盖测试，但没有实现完整 Linux network stack 行为。
- POSIX timers 和 timerfd：足够支撑常见等待场景，但 signal delivery 和 overrun 语义被简化。

## 维护规则

1. 新增 Linux syscall 实现必须登记到 `syscall_coverage.md`。
2. 兼容性捷径应在实现里用简短注释标明，并同步反映到本文档。
3. ABI 文件只应负责转换用户参数并调用内部子系统 API；不应拥有子系统全局状态。
4. 如果某个 syscall 为了兼容而故意对未支持特性返回成功，应登记为 `stub`，不能登记为 `full`。
